using System.IO.Compression;
using System.Net.Http.Headers;
using System.Security.Cryptography;
using System.Text.Json;

namespace ApexSetup;

// Size: bytes as the API reports them (0 when read from the release page)
public sealed record Asset(string Name, string Url, string? Sha256, long Size = 0);

// A failure Setup explains in the player's words: an English key with {n} arguments, translated by the UI (Message stays readable)
public sealed class SetupException(string key, params string[] args) : InvalidOperationException(string.Format(key, args))
{
    public string Key { get; } = key;
    public string[] Args { get; } = args;
}
public sealed record Release(string Repo, string Tag, IReadOnlyList<Asset> Assets)
{
    public string Version => Tag.TrimStart('v', 'V');
    public Asset Find(Func<string, bool> match) =>
        Assets.FirstOrDefault(a => match(a.Name)) ?? throw new InvalidOperationException($"{Repo} {Tag}: asset not found (the release has: {string.Join(", ", Assets.Select(a => a.Name))})");
}

// Every download comes from the latest release of the project's own GitHub repository.
public static class Releases
{
    public const string Apex = "loinyx/Sims3-ApexRadiance";
    public const string S3ss = "sims3fiend/Sims3SettingsSetter";
    public const string Modernization = "rodneypantonial/sims3-modernization";
    public const string AsiLoader = "ThirteenAG/Ultimate-ASI-Loader";
    public const string Dxvk = "doitsujin/dxvk";
    public const string MonoPatcher = "LazyDuchess/MonoPatcher";

    // settable for the tests' stub handler
    public static HttpClient Http { get; set; } = MakeClient();

    static HttpClient MakeClient()
    {
        var h = new HttpClient { Timeout = TimeSpan.FromMinutes(5) };
        h.DefaultRequestHeaders.UserAgent.Add(new ProductInfoHeaderValue("ApexRadianceSetup", "1.0"));
        return h;
    }

    public static async Task<Release> LatestAsync(string repo, CancellationToken ct = default)
    {
        Exception? apiError = null;
        // short limits: a stalled request must not keep Setup "checking" for the client's 5 minutes
        using (var api = CancellationTokenSource.CreateLinkedTokenSource(ct))
        {
            api.CancelAfter(TimeSpan.FromSeconds(15));
            try { return await FromApiAsync(repo, api.Token); }
            catch (Exception e) when (!ct.IsCancellationRequested) { apiError = e; } // the API allows 60 calls an hour per address; a portal may answer HTML
        }
        using var page = CancellationTokenSource.CreateLinkedTokenSource(ct);
        page.CancelAfter(TimeSpan.FromSeconds(20));
        try { return await FromPageAsync(repo, page.Token); }
        catch (Exception) when (apiError is SetupException limit && !ct.IsCancellationRequested) { throw limit; } // (the page failing too: the limit is the news)
    }

    // GitHub's answer to a refused API call: its rate limit (403 or 429 with no calls left) says when it opens again
    public static SetupException? RateLimit(System.Net.HttpStatusCode status, string? remaining, string? reset, DateTimeOffset now)
    {
        if (status is not (System.Net.HttpStatusCode.Forbidden or System.Net.HttpStatusCode.TooManyRequests) || remaining != "0") return null;
        var minutes = long.TryParse(reset, out var r) ? Math.Max(1, (long)Math.Ceiling((DateTimeOffset.FromUnixTimeSeconds(r) - now).TotalMinutes)) : 60;
        return new SetupException("GitHub's download limit was reached; try again in {0} minutes.", minutes.ToString());
    }

    // a GET that is tried again on a server error (5xx) or a timeout: 3 tries, 1 s then 2 s apart; a refusal (4xx) fails at once
    public static TimeSpan BackoffUnit = TimeSpan.FromSeconds(1);
    static async Task<HttpResponseMessage> GetAsync(string url, CancellationToken ct)
    {
        for (var i = 0; ; i++)
        {
            HttpResponseMessage? r = null;
            try
            {
                r = await Http.GetAsync(url, HttpCompletionOption.ResponseHeadersRead, ct);
                if ((int)r.StatusCode < 500 || i == 2) return r;
            }
            catch (Exception e) when (e is HttpRequestException or TaskCanceledException && !ct.IsCancellationRequested && i < 2) { }
            r?.Dispose();
            await Task.Delay(BackoffUnit * (i + 1), ct);
        }
    }

    static async Task<string> GetStringAsync(string url, CancellationToken ct)
    {
        using var r = await GetAsync(url, ct);
        if (RateLimit(r.StatusCode, Header(r, "X-RateLimit-Remaining"), Header(r, "X-RateLimit-Reset"), DateTimeOffset.UtcNow) is { } limit) throw limit;
        r.EnsureSuccessStatusCode();
        return await r.Content.ReadAsStringAsync(ct);
    }

    static string? Header(HttpResponseMessage r, string name) => r.Headers.TryGetValues(name, out var v) ? v.FirstOrDefault() : null;

    // The repo's recent published releases, newest first (drafts and prereleases left out); used when the latest one lacks
    // a file another edition needs. experimental: also the prereleases tagged -exp. (Sims 3 Modernization's test channel)
    // (without the API: the latest release alone, from its page)
    public static async Task<IReadOnlyList<Release>> RecentAsync(string repo, CancellationToken ct = default, bool experimental = false)
    {
        string json;
        using (var api = CancellationTokenSource.CreateLinkedTokenSource(ct))
        {
            api.CancelAfter(TimeSpan.FromSeconds(20));
            try { json = await GetStringAsync($"https://api.github.com/repos/{repo}/releases?per_page=20", api.Token); }
            // (GitHub's rate limit is the news itself: an older edition's build may be in a release the latest one does not show)
            catch (Exception e) when (e is not SetupException && !ct.IsCancellationRequested) { return new[] { await LatestAsync(repo, ct) }; }
        }
        using var doc = JsonDocument.Parse(json);
        return doc.RootElement.EnumerateArray()
            .Where(r => !r.GetProperty("draft").GetBoolean()
                && (!r.GetProperty("prerelease").GetBoolean() || experimental && IsExperimental(r.GetProperty("tag_name").GetString()!)))
            .Select(r => new Release(repo, r.GetProperty("tag_name").GetString()!, r.GetProperty("assets").EnumerateArray().Select(a => new Asset(
                a.GetProperty("name").GetString()!,
                a.GetProperty("browser_download_url").GetString()!,
                a.TryGetProperty("digest", out var d) && d.GetString() is { } s && s.StartsWith("sha256:") ? s[7..] : null,
                a.TryGetProperty("size", out var z) ? z.GetInt64() : 0)).ToList()))
            .ToList();
    }

    // a test-channel tag: vX.Y.Z-exp.YYYYMMDD
    public static bool IsExperimental(string tagOrVersion) => tagOrVersion.Contains("-exp.", StringComparison.OrdinalIgnoreCase);

    // a final release is newer than a prerelease of the same number (3.2.0 over 3.2.0-beta); of two -exp. builds of the same
    // number, the later date
    public static bool Newer(string? latest, string? installed)
    {
        static Version? V(string? s) => s is null ? null : Version.TryParse(s.Split('-', '+')[0], out var v) ? v : null;
        static bool Pre(string s) => s.IndexOfAny(new[] { '-', '+' }) >= 0;
        static long Exp(string s) => IsExperimental(s) && long.TryParse(s[(s.IndexOf("-exp.", StringComparison.OrdinalIgnoreCase) + 5)..].Split('.', '+')[0], out var d) ? d : -1;
        if (installed is null || V(latest) is not { } l) return false;
        return V(installed) is not { } i || l > i || l == i && Pre(installed) && !Pre(latest!) // a copy without a version is older than any release
            || l == i && Exp(latest!) > Exp(installed) && Exp(installed) >= 0;
    }

    // The release page: /releases/latest redirects to the tag; expanded_assets lists each file with its SHA-256.
    static async Task<Release> FromPageAsync(string repo, CancellationToken ct)
    {
        using var latest = await GetAsync($"https://github.com/{repo}/releases/latest", ct);
        latest.EnsureSuccessStatusCode();
        // a repo without releases stays on /releases: no tag to read
        if (TagOf(latest.RequestMessage?.RequestUri) is not { } tag) throw new HttpRequestException($"{repo}: no published release");
        var html = await GetStringAsync($"https://github.com/{repo}/releases/expanded_assets/{Uri.EscapeDataString(tag)}", ct);
        var assets = new List<Asset>();
        foreach (var item in html.Split("<li"))
        {
            var href = System.Text.RegularExpressions.Regex.Match(item, $@"href=""(/{System.Text.RegularExpressions.Regex.Escape(repo)}/releases/download/[^""]+)""");
            if (!href.Success) continue;
            var sha = System.Text.RegularExpressions.Regex.Match(item, "sha256:([0-9a-f]{64})");
            var url = "https://github.com" + System.Net.WebUtility.HtmlDecode(href.Groups[1].Value);
            assets.Add(new Asset(Uri.UnescapeDataString(url[(url.LastIndexOf('/') + 1)..]), url, sha.Success ? sha.Groups[1].Value : null));
        }
        if (assets.Count == 0) throw new HttpRequestException($"{repo}: no release files found");
        return new Release(repo, tag, assets);
    }

    public static string? TagOf(Uri? landed) =>
        landed?.AbsolutePath.Split("/releases/tag/") is [_, { Length: > 0 } t] ? Uri.UnescapeDataString(t) : null;

    static async Task<Release> FromApiAsync(string repo, CancellationToken ct)
    {
        using var doc = JsonDocument.Parse(await GetStringAsync($"https://api.github.com/repos/{repo}/releases/latest", ct));
        var root = doc.RootElement;
        var assets = root.GetProperty("assets").EnumerateArray().Select(a => new Asset(
            a.GetProperty("name").GetString()!,
            a.GetProperty("browser_download_url").GetString()!,
            a.TryGetProperty("digest", out var d) && d.GetString() is { } s && s.StartsWith("sha256:") ? s[7..] : null,
            a.TryGetProperty("size", out var z) ? z.GetInt64() : 0)).ToList();
        return new Release(repo, root.GetProperty("tag_name").GetString()!, assets);
    }

    // Downloads to a temp folder; checks GitHub's SHA-256 digest when the release has one.
    public static async Task<string> DownloadAsync(Asset a, string dir, CancellationToken ct = default)
    {
        Directory.CreateDirectory(dir);
        var file = Path.Combine(dir, a.Name);
        using var resp = await GetAsync(a.Url, ct);
        resp.EnsureSuccessStatusCode();
        await using (var s = await resp.Content.ReadAsStreamAsync(ct))
        await using (var f = File.Create(file))
        {
            // HttpClient.Timeout stops at the headers: a body that stops coming fails after 60 s without data
            using var idle = CancellationTokenSource.CreateLinkedTokenSource(ct);
            var buf = new byte[81920];
            while (true)
            {
                idle.CancelAfter(TimeSpan.FromSeconds(60));
                int n;
                try { n = await s.ReadAsync(buf, idle.Token); }
                catch (OperationCanceledException) when (!ct.IsCancellationRequested) { throw new HttpRequestException($"{a.Name}: download stalled"); }
                if (n == 0) break;
                await f.WriteAsync(buf.AsMemory(0, n), ct);
            }
        }
        if (a.Sha256 is { } want && !Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(file))).Equals(want, StringComparison.OrdinalIgnoreCase))
            throw new SetupException("The download of {0} was damaged. Try again; if it repeats, an antivirus web shield or a proxy may be changing downloads.", a.Name);
        return file;
    }

    // one file out of a .tar.gz (DXVK's release format), e.g. "x32/d3d9.dll" under its top folder
    public static string ExtractFromTarGz(string tgz, string inner)
    {
        using var gz = new GZipStream(File.OpenRead(tgz), CompressionMode.Decompress);
        using var tar = new System.Formats.Tar.TarReader(gz);
        while (tar.GetNextEntry() is { } e)
            if (e.Name.Replace('\\', '/').EndsWith("/" + inner, StringComparison.OrdinalIgnoreCase) && e.DataStream is { } data)
            {
                var dir = tgz + ".d";
                Directory.CreateDirectory(dir);
                var file = Path.Combine(dir, Path.GetFileName(inner));
                using var f = File.Create(file);
                data.CopyTo(f);
                return file;
            }
        throw new InvalidDataException($"{Path.GetFileName(tgz)}: {inner} not found");
    }

    public static string Unzip(string zip)
    {
        var dir = zip + ".d";
        if (Directory.Exists(dir)) Directory.Delete(dir, true);
        ZipFile.ExtractToDirectory(zip, dir);
        CheckExtracted(zip, dir);
        return dir;
    }

    // every file of the zip there whole after extraction: one an antivirus took or emptied is a missing file (FileNotFoundException,
    // which the install reports as the antivirus), not a missing asset or an install without it
    public static void CheckExtracted(string zip, string dir)
    {
        using var z = ZipFile.OpenRead(zip);
        foreach (var e in z.Entries)
        {
            if (e.Name.Length == 0) continue; // (a folder)
            var f = Path.Combine(dir, e.FullName.Replace('/', Path.DirectorySeparatorChar));
            if (!File.Exists(f) || new FileInfo(f).Length != e.Length) throw new FileNotFoundException(null, f);
        }
    }
}
