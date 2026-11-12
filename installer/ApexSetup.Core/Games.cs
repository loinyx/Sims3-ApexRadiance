using System.Diagnostics;
using System.Text.RegularExpressions;
using Microsoft.Win32;

namespace ApexSetup;

public enum Edition { Steam, EA, Other }

// One install of The Sims 3. Bin is the folder with the game's exe (Game\Bin).
public sealed record Game(string Root, string Bin, string Exe, Edition Edition, string Version)
{
    public string ExePath => Path.Combine(Bin, Exe);
    public string Label => Edition switch { Edition.Steam => "Steam", Edition.EA => "EA app", _ => Exe };
    // sims3-modernization ships builds for these two only (docs/PLAYERS.md in its repo)
    public bool ModernizationSupported => Version is "1.67.2" or "1.69.47";
}

public static class Games
{
    // PE timestamps of the main module, as in docs/engine/game-versions.md
    static readonly Dictionary<uint, (Edition, string)> Builds = new()
    {
        [0x52DEC247] = (Edition.Steam, "1.67.2"),
        [0x52D872DA] = (Edition.Other, "1.67.2 (disc)"),
        [0x6707155C] = (Edition.EA, "1.69.47"),
        [0x568D4BAC] = (Edition.Other, "1.69.43 (Origin)"),
    };

    // The folders the player picked with Browse, and the game used last, so the next run (an Apex update) opens on it:
    // %LOCALAPPDATA%\Apex Radiance Setup\games.txt, one Game\Bin per line, the last used first. A folder that no longer holds
    // the game is left out when read.
    static string RememberedFile => Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Apex Radiance Setup", "games.txt");

    public static List<Game> Remembered()
    {
        var list = new List<Game>();
        try
        {
            if (!File.Exists(RememberedFile)) return list;
            foreach (var line in File.ReadAllLines(RememberedFile))
                if (line.Trim() is { Length: > 0 } bin && FromFolder(bin) is { } g && !list.Any(f => f.Bin.Equals(g.Bin, StringComparison.OrdinalIgnoreCase)))
                    list.Add(g);
        }
        catch (Exception) { } // (unreadable: nothing remembered)
        return list;
    }

    // puts the game first in the remembered list (picked with Browse, or chosen on the Game step)
    public static void Remember(Game game)
    {
        try
        {
            var bins = new List<string> { game.Bin };
            if (File.Exists(RememberedFile))
                bins.AddRange(File.ReadAllLines(RememberedFile).Select(l => l.Trim()).Where(l => l.Length > 0 && !l.Equals(game.Bin, StringComparison.OrdinalIgnoreCase)));
            Directory.CreateDirectory(Path.GetDirectoryName(RememberedFile)!);
            File.WriteAllLines(RememberedFile, bins.Take(16));
        }
        catch (Exception) { } // (not writable: the next run detects as before)
    }

    public static List<Game> Detect()
    {
        var roots = new List<string>();
        foreach (var lib in SteamLibraries())
            roots.Add(Path.Combine(lib, "steamapps", "common", "The Sims 3"));
        foreach (var key in new[] { @"SOFTWARE\WOW6432Node\Sims\The Sims 3", @"SOFTWARE\Sims\The Sims 3" })
            if (Registry.LocalMachine.OpenSubKey(key)?.GetValue("Install Dir") is string dir) roots.Add(dir);
        var pf = Environment.GetFolderPath(Environment.SpecialFolder.ProgramFiles);
        var pf86 = Environment.GetFolderPath(Environment.SpecialFolder.ProgramFilesX86);
        roots.Add(Path.Combine(pf, "EA Games", "The Sims 3"));
        roots.Add(Path.Combine(pf86, "Electronic Arts", "The Sims 3"));
        roots.Add(Path.Combine(pf86, "Origin Games", "The Sims 3"));
        roots.Add(Path.Combine(pf86, "Steam", "steamapps", "common", "The Sims 3"));

        var found = new List<Game>();
        foreach (var r in roots)
            if (FromFolder(r) is { } g && !found.Any(f => f.Bin.Equals(g.Bin, StringComparison.OrdinalIgnoreCase)))
                found.Add(g);
        return found;
    }

    // Accepts the game folder, Game or Game\Bin, or the exe itself (Browse).
    public static Game? FromFolder(string path)
    {
        if (File.Exists(path)) path = Path.GetDirectoryName(path)!;
        foreach (var bin in new[] { path, Path.Combine(path, "Bin"), Path.Combine(path, "Game", "Bin") })
        {
            var exes = new[] { "TS3W.exe", "TS3.exe" }.Where(e => File.Exists(Path.Combine(bin, e))).ToList();
            if (exes.Count == 0) continue;
            // a folder with both exes (an EA app install moved or copied, a leftover TS3W.exe) is the build its known exe
            // is: the EA app's TS3.exe 1.69.47 next to a stray TS3W.exe made Setup take the game for Steam
            var exe = exes.FirstOrDefault(e => Builds.ContainsKey(PeTimestamp(Path.Combine(bin, e)))) ?? exes[0];
            var p = Path.Combine(bin, exe);
            var full = Path.GetFullPath(bin);
            var root = Path.GetFullPath(Path.Combine(full, "..", ".."));
            var (ed, ver) = Builds.TryGetValue(PeTimestamp(p), out var b) ? b
                : (exe == "TS3.exe" ? Edition.EA : Edition.Steam, FileVersionInfo.GetVersionInfo(p).FileVersion ?? "?");
            return new Game(root, full, exe, ed, ver);
        }
        return null;
    }

    public static uint PeTimestamp(string file)
    {
        try { return ReadPeTimestamp(file); } catch (Exception e) when (e is IOException or ArgumentOutOfRangeException or EndOfStreamException) { return 0; }
    }

    static uint ReadPeTimestamp(string file)
    {
        using var f = File.OpenRead(file);
        using var r = new BinaryReader(f);
        f.Position = 0x3C;
        f.Position = r.ReadInt32() + 8;
        return r.ReadUInt32();
    }

    // replaced by the offline tests, which may run while the player has the game open
    public static Func<bool> IsRunning = DefaultIsRunning;
    static bool DefaultIsRunning() =>
        Process.GetProcessesByName("TS3W").Length + Process.GetProcessesByName("TS3").Length > 0;

    static IEnumerable<string> SteamLibraries()
    {
        var steam = Registry.CurrentUser.OpenSubKey(@"Software\Valve\Steam")?.GetValue("SteamPath") as string
                    ?? Registry.LocalMachine.OpenSubKey(@"SOFTWARE\WOW6432Node\Valve\Steam")?.GetValue("InstallPath") as string;
        if (steam is null) yield break;
        yield return steam;
        var vdf = Path.Combine(steam, "steamapps", "libraryfolders.vdf");
        if (!File.Exists(vdf)) yield break;
        foreach (Match m in Regex.Matches(File.ReadAllText(vdf), "\"path\"\\s+\"([^\"]+)\""))
            yield return m.Groups[1].Value.Replace(@"\\", @"\");
    }
}
