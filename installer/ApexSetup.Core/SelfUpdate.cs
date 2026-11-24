using System.Diagnostics;
using System.Security.Cryptography;

namespace ApexSetup;

// The launcher updates itself from the latest Apex Radiance release: its ApexLauncher.exe (or, as Setups before the launcher
// look for it, the same exe as ApexRadianceSetup.exe), checked against GitHub's SHA-256 digest and found to be Apex Launcher
// or Apex Radiance Setup of a higher version, replaces the running exe (renamed to .old, deleted at
// the next start) and starts in its place. Setups before 1.1.0 (Apex Radiance 3.1.3 and earlier) have no such check.
public static class SelfUpdate
{
    // the launcher's 2.0.x builds still say Product "Apex Radiance Setup": Setups before it accept only that name
    public static readonly string[] AssetNames = ["ApexLauncher.exe", "ApexRadianceSetup.exe"], Products = ["Apex Launcher", "Apex Radiance Setup"];
    public static string AssetName => AssetNames[0];

    // The newer Setup, downloaded and checked, or null when this one is the latest (or the release has none).
    public static async Task<string?> DownloadIfNewerAsync(Release apex, string self, CancellationToken ct = default)
    {
        if (AssetNames.Select(n => apex.Assets.FirstOrDefault(a => a.Name == n)).FirstOrDefault(a => a is not null) is not { } asset) return null;
        if (asset.Sha256 is null) return null; // never run an exe that cannot be checked
        if (Hash(self).Equals(asset.Sha256, StringComparison.OrdinalIgnoreCase)) return null; // this very exe
        // (a folder of its own: an install empties %TEMP%\ApexRadianceSetup when it finishes)
        var file = await Releases.DownloadAsync(asset, Path.Combine(Path.GetTempPath(), "ApexLauncher-update", Guid.NewGuid().ToString("N")), ct);
        return IsNewer(file, self) ? file : null;
    }

    // Apex Launcher (or Setup) with a higher version than self: a different digest alone could be an older or a test build.
    public static bool IsNewer(string file, string self)
    {
        var n = FileVersionInfo.GetVersionInfo(file);
        return Products.Contains(n.ProductName) && Version.TryParse(n.FileVersion, out var nv)
               && Version.TryParse(FileVersionInfo.GetVersionInfo(self).FileVersion, out var sv) && nv > sv;
    }

    // A running exe cannot be overwritten but can be renamed: the new one takes its name; a failed copy puts it back.
    public static void Replace(string newExe, string self)
    {
        var old = self + ".old";
        if (File.Exists(old)) File.Delete(old);
        File.Move(self, old);
        try { File.Copy(newExe, self); }
        catch { File.Move(old, self); throw; }
    }

    public static void Restart(string self, IEnumerable<string> args) =>
        Process.Start(new ProcessStartInfo(self, string.Join(" ", args.Select(Quote))) { UseShellExecute = true });

    // The new exe in place and started; only then may this one close. A start that fails puts the old exe back. The .old is
    // deleted by the new Setup at its start (CleanUp), never by this one.
    public static void ReplaceAndStart(string newExe, string self, IEnumerable<string> args, Action<string, IEnumerable<string>>? start = null)
    {
        Replace(newExe, self);
        try { (start ?? Restart)(self, args); }
        catch
        {
            try { File.Delete(self); File.Move(self + ".old", self); } catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
            throw;
        }
    }

    // one command-line argument as Windows parses it back: quoted, inner quotes escaped, and the backslashes before a quote
    // doubled (a folder argument ending in \ would otherwise swallow its closing quote)
    public static string Quote(string a)
    {
        var sb = new System.Text.StringBuilder("\"");
        var slashes = 0;
        foreach (var ch in a)
        {
            if (ch == '\\') { slashes++; continue; }
            sb.Append('\\', ch == '"' ? slashes * 2 + 1 : slashes).Append(ch);
            slashes = 0;
        }
        return sb.Append('\\', slashes * 2).Append('"').ToString();
    }

    // the folder the update was downloaded to, no longer needed once Setup starts (about 40 MB each time otherwise)
    public static void CleanTemp()
    {
        foreach (var name in new[] { "ApexLauncher-update", "ApexRadianceSetup-update" })
            try { var d = Path.Combine(Path.GetTempPath(), name); if (Directory.Exists(d)) Directory.Delete(d, true); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
    }

    // the exe an update replaced, still running then: removed at the next start
    public static void CleanUp(string self)
    {
        try { if (File.Exists(self + ".old")) File.Delete(self + ".old"); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
    }

    static string Hash(string file)
    {
        using var f = File.OpenRead(file);
        return Convert.ToHexString(SHA256.HashData(f));
    }
}
