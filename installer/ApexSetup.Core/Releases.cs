using System.IO.Compression;
using System.Net.Http.Headers;
using System.Security.Cryptography;
using System.Text.Json;

namespace ApexSetup;

public sealed record Asset(string Name, string Url, string? Sha256);
public sealed record Release(string Repo, string Tag, IReadOnlyList<Asset> Assets)
{
    public string Version => Tag.TrimStart('v', 'V');
    public Asset Find(Func<string, bool> match) =>
        Assets.FirstOrDefault(a => match(a.Name)) ?? throw new InvalidOperationException($"{Repo} {Tag}: asset not found");
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

    static readonly HttpClient Http = MakeClient();

    static HttpClient MakeClient()
    {
        var h = new HttpClient { Timeout = TimeSpan.FromMinutes(5) };
        h.DefaultRequestHeaders.UserAgent.Add(new ProductInfoHeaderValue("ApexRadianceSetup", "1.0"));
        return h;
    }

    public static async Task<Release> LatestAsync(string repo, CancellationToken ct = default)
    {
        try { return await FromApiAsync(repo, ct); }
        catch (HttpRequestException) { return await FromPageAsync(repo, ct); } // the API allows 60 calls an hour per address
    }

    // The release page: /releases/latest redirects to the tag; expanded_assets lists each file with its SHA-256.
    static async Task<Release> FromPageAsync(string repo, CancellationToken ct)
    {
        using var latest = await Http.GetAsync($"https://github.com/{repo}/releases/latest", ct);
        latest.EnsureSuccessStatusCode();
        var tag = Uri.UnescapeDataString(latest.RequestMessage!.RequestUri!.AbsolutePath.Split("/releases/tag/")[1]);
        var html = await Http.GetStringAsync($"https://github.com/{repo}/releases/expanded_assets/{Uri.EscapeDataString(tag)}", ct);
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

    static async Task<Release> FromApiAsync(string repo, CancellationToken ct)
    {
        using var doc = JsonDocument.Parse(await Http.GetStringAsync($"https://api.github.com/repos/{repo}/releases/latest", ct));
        var root = doc.RootElement;
        var assets = root.GetProperty("assets").EnumerateArray().Select(a => new Asset(
            a.GetProperty("name").GetString()!,
            a.GetProperty("browser_download_url").GetString()!,
            a.TryGetProperty("digest", out var d) && d.GetString() is { } s && s.StartsWith("sha256:") ? s[7..] : null)).ToList();
        return new Release(repo, root.GetProperty("tag_name").GetString()!, assets);
    }

    // Downloads to a temp folder; checks GitHub's SHA-256 digest when the release has one.
    public static async Task<string> DownloadAsync(Asset a, string dir, CancellationToken ct = default)
    {
        Directory.CreateDirectory(dir);
        var file = Path.Combine(dir, a.Name);
        await using (var s = await Http.GetStreamAsync(a.Url, ct))
        await using (var f = File.Create(file))
            await s.CopyToAsync(f, ct);
        if (a.Sha256 is { } want && !Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(file))).Equals(want, StringComparison.OrdinalIgnoreCase))
            throw new InvalidDataException($"{a.Name}: download is damaged (checksum mismatch)");
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
        return dir;
    }
}
