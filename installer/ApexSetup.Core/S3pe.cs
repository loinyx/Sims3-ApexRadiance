using System.Diagnostics;
using System.Security;
using Microsoft.Win32;

namespace ApexSetup;

// The package editor players use, when it is installed: found by what its installer wrote (App Paths, Installed apps) or
// in Program Files, and opened with the player's package.
public static class S3pe
{
    const string Exe = "s3pe.exe";
    static (string? Path, long At)? _found;

    // (asked for every details pane and row menu: the registry is read at most every half minute)
    public static string? Find()
    {
        if (_found is { } f && Environment.TickCount64 - f.At < 30_000 && (f.Path is null || File.Exists(f.Path))) return f.Path;
        var path = Pick(Candidates());
        _found = (path, Environment.TickCount64);
        return path;
    }

    // the first that is an s3pe.exe on this PC; a folder, a quoted path, an icon's ",0" and an uninstaller beside it count too
    public static string? Pick(IEnumerable<string?> candidates)
    {
        foreach (var c in candidates)
        {
            var p = (c ?? "").Trim().Trim('"');
            if (p.LastIndexOf(".exe", StringComparison.OrdinalIgnoreCase) is > 0 and var i) p = Path.GetDirectoryName(p[..(i + 4)]) ?? "";
            if (!Path.IsPathFullyQualified(p)) continue;
            p = Path.Combine(p, Exe);
            if (File.Exists(p)) return p;
        }
        return null;
    }

    static IEnumerable<string?> Candidates()
    {
        const string root = @"Software\Microsoft\Windows\CurrentVersion\";
        foreach (var hive in new[] { RegistryHive.CurrentUser, RegistryHive.LocalMachine })
            foreach (var view in new[] { RegistryView.Registry64, RegistryView.Registry32 })
            {
                using var b = RegistryKey.OpenBaseKey(hive, view);
                using (var app = Sub(b, root + @"App Paths\" + Exe)) yield return app?.GetValue(null) as string;
                using var all = Sub(b, root + "Uninstall");
                foreach (var name in all?.GetSubKeyNames() ?? [])
                {
                    using var k = Sub(all!, name);
                    if (!(k?.GetValue("DisplayName") as string ?? name).StartsWith("s3pe", StringComparison.OrdinalIgnoreCase)) continue;
                    foreach (var value in new[] { "InstallLocation", "DisplayIcon", "UninstallString" }) yield return k?.GetValue(value) as string;
                }
            }
        foreach (var f in new[] { Environment.SpecialFolder.ProgramFilesX86, Environment.SpecialFolder.ProgramFiles })
            yield return Path.Combine(Environment.GetFolderPath(f), "s3pe");
    }

    static RegistryKey? Sub(RegistryKey k, string name)
    {
        try { return k.OpenSubKey(name); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or SecurityException) { return null; }
    }

    public static void Open(string exe, string package)
    {
        var start = new ProcessStartInfo(exe) { UseShellExecute = true, WorkingDirectory = Path.GetDirectoryName(exe)! };
        start.ArgumentList.Add(package);
        Process.Start(start)?.Dispose();
    }
}
