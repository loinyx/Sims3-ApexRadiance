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
        // the same version as the release, yet other bytes: a damaged or replaced file (a build of another version is not compared)
        else if (apex is not null && s.ApexVersion == apex.Version && apex.Assets.FirstOrDefault(a => a.Name == "ApexRadiance.asi")?.Sha256 is { } want &&
                 !Sha256Of(B("ApexRadiance.asi")).Equals(want, StringComparison.OrdinalIgnoreCase))
            f.Add(new(Severity.Error, "ApexRadiance.asi is not the {0} release file: install it again with Repair.", new[] { apex.Version }));
        if (s.Loader is null)
            f.Add(new(Severity.Error, "No ASI loader in Game\\Bin: Apex Radiance and the other .asi mods do not load.", Array.Empty<string>()));
        if (s.Layer == Layer.Bridge)
            foreach (var rel in new[] { "bridge.conf", @".trex\bridge.conf", @".trex\NvRemixBridge.exe" })
                if (!File.Exists(B(rel)))
                    f.Add(new(Severity.Error, "{0} is missing: the RTX Remix bridge does not start.", new[] { rel }));
        return f;
    }

    public static string Sha256Of(string path)
    {
        using var stream = File.OpenRead(path);
        return Convert.ToHexString(SHA256.HashData(stream));
    }
}
