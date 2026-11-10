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

    static readonly HttpClient Http = MakeClient();

    static HttpClient MakeClient()
    {
        var h = new HttpClient { Timeout = TimeSpan.FromMinutes(5) };
        h.DefaultRequestHeaders.UserAgent.Add(new ProductInfoHeaderValue("ApexRadianceSetup", "1.0"));
        return h;
    }

    public static async Task<Release> LatestAsync(string repo, CancellationToken ct = default)
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

    public static string Unzip(string zip)
    {
        var dir = zip + ".d";
        if (Directory.Exists(dir)) Directory.Delete(dir, true);
        ZipFile.ExtractToDirectory(zip, dir);
        return dir;
    }
}
