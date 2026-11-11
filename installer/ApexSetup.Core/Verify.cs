using System.Security.Cryptography;

namespace ApexSetup;

// "Check installation": the install's own files, beside the checks of Compat (which the same screen runs). apex: the latest Apex
// Radiance release when Setup could read it (null offline). A finding with no Fix is set right by Repair (install again).
public static class Verify
{
    public static List<Finding> Files(Game g, State s, Release? apex)
    {
        var f = new List<Finding>();
        string B(string rel) => Path.Combine(g.Bin, rel);
        if (s.ApexVersion is null)
            f.Add(new(Severity.Error, "Apex Radiance is not installed in this game.", Array.Empty<string>()));
        if (s.Loader is null)
            f.Add(new(Severity.Error, "No ASI loader in Game\\Bin: Apex Radiance and the other .asi mods do not load.", Array.Empty<string>()));
        if (s.Layer == Layer.Bridge)
            foreach (var rel in new[] { "bridge.conf", @".trex\bridge.conf", @".trex\NvRemixBridge.exe", @".trex\d3d9vk_x64.dll" })
                if (!File.Exists(B(rel)))
                    f.Add(new(Severity.Error, "{0} is missing: the RTX Remix bridge does not start.", new[] { rel }));
        return f;
    }

    // A test build: a -dev / -test version, a version newer than the latest release, or that release's version with other bytes
    // (a development build carries the number of the version it leads to only once it is released). Not an error: the status says
    // so and offers the official file.
    public static bool TestBuild(Game g, State s, Release? apex)
    {
        if (s.ApexVersion is not { } v || v == "?") return false;
        if (v.Contains("-dev", StringComparison.OrdinalIgnoreCase) || v.Contains("-test", StringComparison.OrdinalIgnoreCase)) return true;
        if (apex is null) return false;
        if (Version.TryParse(v.Split('-')[0], out var have) && Version.TryParse(apex.Version.Split('-')[0], out var rel) && have > rel) return true;
        return v == apex.Version && apex.Assets.FirstOrDefault(a => a.Name == "ApexRadiance.asi")?.Sha256 is { } want &&
               !Sha256Of(Path.Combine(g.Bin, "ApexRadiance.asi")).Equals(want, StringComparison.OrdinalIgnoreCase);
    }

    public static string Sha256Of(string path)
    {
        using var stream = File.OpenRead(path);
        return Convert.ToHexString(SHA256.HashData(stream));
    }
}
