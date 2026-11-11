using System.Text.RegularExpressions;

namespace ApexSetup;

public enum Severity { Info, Warning, Error }

// One thing about the player's install that can break or confuse a game start. Text is an English key with {0} arguments
// (the UI translates it); Fix, when there is one, makes it right and is safe to run more than once.
public sealed record Finding(Severity Severity, string Text, string[] Args, Action? Fix = null, string? Link = null)
{
    public bool CanFix => Fix is not null;
}

// The checks Apex Radiance makes in game (Settings > Compatibility), made before the game starts, plus the install
// mistakes the mods' own guides describe.
public static class Compat
{
    const string ModernizationPlayers = "https://github.com/rodneypantonial/sims3-modernization/blob/main/docs/PLAYERS.md";
    const string ModernizationCompat = "https://github.com/rodneypantonial/sims3-modernization/blob/main/docs/COMPATIBILITY.md";

    public static List<Finding> Check(Game g, State s, string? docs = null)
    {
        docs ??= Scan.DocumentsFor(g);
        var f = new List<Finding>();
        var bin = g.Bin;
        string B(string n) => Path.Combine(bin, n);
        var mods = Path.Combine(docs, "Mods");
        var packages = Directory.Exists(mods)
            ? Directory.EnumerateFiles(mods, "*.package", SearchOption.AllDirectories).Select(p => Path.GetRelativePath(mods, p)).ToList()
            : new List<string>();

        // leftovers that load Apex twice (README: remove S3SSApex.asi); moved to the backup folder
        foreach (var rel in new[] { "S3SSApex.asi", @"scripts\ApexRadiance.asi", @"plugins\ApexRadiance.asi" })
            if (File.Exists(B(rel)))
                f.Add(new(Severity.Error, "{0} in Game\\Bin loads a second copy of Apex Radiance.", new[] { rel }, () => MoveAside(bin, rel)));

        // an old build with Apex inside S3SS (framework/s3ss_detect.cpp): while it loads, Apex stays off
        foreach (var asi in Directory.EnumerateFiles(bin, "*.asi"))
        {
            var name = Path.GetFileName(asi);
            if (name.Equals("ApexRadiance.asi", StringComparison.OrdinalIgnoreCase) || name.Equals("S3SSApex.asi", StringComparison.OrdinalIgnoreCase)) continue;
            if (Scan.Contains(asi, "Apex Edition"))
                f.Add(new(Severity.Error, "{0} is an old build with Apex inside; while it is in Game\\Bin, Apex Radiance stays off. Setup moves it to the backup folder (install the official Sims3SettingsSetter in its place if you used it).",
                    new[] { name }, () => MoveAside(bin, name)));
        }

        // two loaders load every mod twice: keep the first (wininet.dll, as the mods' guides name it)
        var loaders = Scan.Loaders(bin);
        if (loaders.Count > 1)
            f.Add(new(Severity.Error, "Two ASI loaders ({0}) load every mod twice. Keep one: remove all but {1}.", new[] { string.Join(", ", loaders), loaders[0] },
                () => { foreach (var l in loaders.Skip(1)) MoveAside(bin, l); }));

        if (s.Ts3NativeWrongBuild)
            f.Add(new(Severity.Error, "Sims 3 Modernization's ts3native is the build for the other edition and does nothing. Turn it on in Components to install the right one.",
                Array.Empty<string>(), Link: ModernizationPlayers));

        // packages are only read through Mods\Resource.cfg
        if (packages.Count > 0 && !File.Exists(Path.Combine(mods, "Resource.cfg")))
            f.Add(new(Severity.Error, "Mods has {0} .package files but no Resource.cfg, so the game loads none of them.", new[] { packages.Count.ToString() },
                () => Engine.EnsureResourceCfg(docs)));

        // script mods built for the other edition (sims3-modernization's EA app notes)
        string other = g.Edition == Edition.EA ? "steam" : "1.69";
        foreach (var p in packages.Where(p => (p.Contains("simler90", StringComparison.OrdinalIgnoreCase) || p.Contains("XCAS", StringComparison.OrdinalIgnoreCase))
                                              && p.Contains(other, StringComparison.OrdinalIgnoreCase)))
            f.Add(new(Severity.Warning, "{0} is the build for the other edition of the game; it makes the game show \"unofficial game modification\". Use the {1} build.",
                new[] { p, g.Edition == Edition.EA ? "EA app" : "Steam" }, Link: ModernizationPlayers));
        if (g.Edition == Edition.EA && packages.Any(p => Path.GetFileName(p).StartsWith("NRaas_ErrorTrap", StringComparison.OrdinalIgnoreCase)))
            f.Add(new(Severity.Warning, "NRaas ErrorTrap has no build known to work on EA app 1.69.47.", Array.Empty<string>(), Link: ModernizationPlayers));

        // mods that reach the same game code as Sims 3 Modernization (its static scan; not known to break anything)
        if (s.Modernization)
            foreach (var p in packages.Where(p => Path.GetFileName(p) is "Buzz_MoarInteractions.package" or "Overrides.package"))
                f.Add(new(Severity.Info, "{0} overrides game code Sims 3 Modernization also patches (not tested together).", new[] { p }, Link: ModernizationCompat));

        // Smooth Patch's legacy ASI patches the same SleepEx wrapper as S3SS's Smooth Patch (sims3-modernization fixes.json)
        if (File.Exists(B("TS3Patch.asi")) && s.S3ss)
            f.Add(new(Severity.Error, "Smooth Patch's TS3Patch.asi patches the same game code as Sims3SettingsSetter's Smooth Patch. Setup moves it to the backup folder (S3SS does the same job).",
                Array.Empty<string>(), () => { MoveAside(bin, "TS3Patch.asi"); if (File.Exists(B("TS3Patch.txt"))) MoveAside(bin, "TS3Patch.txt"); }));
        // two core mods that replace the same game module
        if (packages.Any(p => Path.GetFileName(p).StartsWith("simler90GameplayCoreMod", StringComparison.OrdinalIgnoreCase))
            && packages.Any(p => Path.GetFileName(p).Contains("StoryProgression", StringComparison.OrdinalIgnoreCase)))
            f.Add(new(Severity.Warning, "simler90's Gameplay Core Mod and NRaas StoryProgression replace the same game code. Keep one of them.", Array.Empty<string>(), Link: ModernizationCompat));
        // MonoPatcher needs a loader for its ASI, and the Microsoft Visual C++ x86 runtime
        if (s.MonoPatcher && !File.Exists(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.SystemX86), "vcruntime140.dll")))
            f.Add(new(Severity.Warning, "MonoPatcher needs the Microsoft Visual C++ Redistributable (x86); it is not installed.", Array.Empty<string>(),
                Link: "https://aka.ms/vs/17/release/vc_redist.x86.exe"));

        // Sims3Performance next to sims3-modernization: the two thread patches clash (framework/s3pp_settings.cpp)
        var s3ppIni = B("Sims3Performance.ini");
        if (File.Exists(B("Sims3Performance.asi")) && File.Exists(B("ts3native.asi")) && File.Exists(s3ppIni)
            && Values(File.ReadAllText(s3ppIni), "Patches", "ShortWaitSpin", "ThreadStackDiet").Any(v => int.TryParse(v, out var n) && n > 0))
            f.Add(new(Severity.Warning, "Sims3Performance's ShortWaitSpin and ThreadStackDiet clash with Sims 3 Modernization. Setup sets both to 0 (a copy is kept as Sims3Performance.ini.apex-backup).",
                Array.Empty<string>(), () => Rewrite(s3ppIni, t => Set(t, "Patches", new[] { "ShortWaitSpin", "ThreadStackDiet" }, v => v != "0", "0"))));

        var bridge = s.Layer == Layer.Bridge;
        // S3SS uncompressed textures stutter under the bridge (framework/s3ss_textures.cpp)
        var toml = Path.Combine(docs, "S3SS", "S3SS.toml");
        string[] texSections = { "patches.UncompressedLotTexturesPatch", "patches.UncompressedSimTexturesPatch" };
        if (bridge && s.S3ss && File.Exists(toml) && texSections.Any(sec => Values(File.ReadAllText(toml), sec, "enabled").Contains("true")))
            f.Add(new(Severity.Warning, "Sims3SettingsSetter's uncompressed textures stutter under the RTX Remix bridge. Setup turns them off (a copy is kept as S3SS.toml.apex-backup).",
                Array.Empty<string>(), () => Rewrite(toml, t => texSections.Aggregate(t, (x, sec) => Set(x, sec, new[] { "enabled" }, v => v == "true", "false")))));

        if (bridge)
        {
            // the bridge's client must attach to this game's exe
            foreach (var conf in new[] { B("bridge.conf"), B(@".trex\bridge.conf") })
                if (File.Exists(conf) && TargetProcess(conf) is { } target && !target.Equals(g.Exe, StringComparison.OrdinalIgnoreCase))
                    f.Add(new(Severity.Error, "{0} sets client.targetProcess = {1}, but this game is {2}: the bridge does not start.",
                        new[] { Path.GetRelativePath(bin, conf), target, g.Exe }, () => Engine.SetTargetProcess(conf, g.Exe)));
            // dxvk.conf next to the bridge: DXVK's TS3 profile (else Options.ini resets) and the Reset fix
            var dxvk = B("dxvk.conf");
            var text = File.Exists(dxvk) ? File.ReadAllText(dxvk) : "";
            var missing = BridgeDxvk.Where(kv => !Regex.IsMatch(text, $@"(?mi)^\s*{Regex.Escape(kv.Key)}\s*=")).ToList();
            if (missing.Count > 0)
                f.Add(new(Severity.Warning, "dxvk.conf lacks {0}: without them the game resets its graphics options or freezes on a resolution change under the bridge.",
                    new[] { string.Join(", ", missing.Select(m => m.Key)) },
                    () => File.AppendAllText(dxvk, "\r\n" + string.Concat(missing.Select(m => $"{m.Key} = {m.Value}\r\n")))));
        }
        return f;
    }

    // native/remix-bridge/conf/dxvk.conf
    static readonly Dictionary<string, string> BridgeDxvk = new()
    {
        ["d3d9.customVendorId"] = "10de", ["d3d9.customDeviceId"] = "1080", ["d3d9.customDeviceDesc"] = "\"Geforce GTX 580\"",
        ["d3d9.countLosableResources"] = "False",
    };

    static string? TargetProcess(string conf) =>
        Regex.Match(File.ReadAllText(conf), @"(?m)^\s*client\.targetProcess\s*=\s*(\S+)") is { Success: true } m ? m.Groups[1].Value : null;

    // values of keys inside [section] of an ini/toml text
    public static List<string> Values(string text, string section, params string[] keys)
    {
        var list = new List<string>();
        Walk(text, section, keys, (_, v) => list.Add(v));
        return list;
    }

    // the text with those keys of [section] set to value where match says so; every other byte stays (as Apex's TurnOff)
    public static string Set(string text, string section, string[] keys, Func<string, bool> match, string value)
    {
        var spans = new List<(int At, int Len)>();
        Walk(text, section, keys, (at, v) => { if (match(v)) spans.Add((at, v.Length)); });
        foreach (var (at, len) in spans.OrderByDescending(x => x.At)) text = text.Remove(at, len).Insert(at, value);
        return text;
    }

    static void Walk(string text, string section, string[] keys, Action<int, string> found)
    {
        bool inSection = false;
        int at = 0;
        while (at <= text.Length)
        {
            int end = text.IndexOf('\n', at);
            if (end < 0) end = text.Length;
            var line = text[at..end].TrimEnd('\r');
            var trimmed = line.Trim();
            if (trimmed.StartsWith('[')) inSection = trimmed.IndexOf(']') is var c and > 0 && trimmed[1..c].Trim() == section;
            else if (inSection && !trimmed.StartsWith('#') && !trimmed.StartsWith(';') && line.IndexOf('=') is var eq and > 0
                     && keys.Contains(line[..eq].Trim()))
            {
                int v = eq + 1;
                while (v < line.Length && line[v] is ' ' or '\t') v++;
                int ve = v;
                while (ve < line.Length && line[ve] is not (' ' or '\t' or ';' or '#')) ve++;
                found(at + v, line[v..ve]);
            }
            at = end + 1;
        }
    }

    // a copy first, then the new text through a temporary file (as Apex does), so a failed write never leaves half a file
    static void Rewrite(string file, Func<string, string> change)
    {
        var text = File.ReadAllText(file);
        var changed = change(text);
        if (changed == text) return;
        foreach (var x in new[] { file, file + ".apex-backup" }) // a read-only file (or its copy from an earlier run) would block the copy and the move
            if (File.Exists(x)) File.SetAttributes(x, File.GetAttributes(x) & ~FileAttributes.ReadOnly);
        File.Copy(file, file + ".apex-backup", true);
        File.WriteAllText(file + ".apex-new", changed);
        File.Move(file + ".apex-new", file, true);
    }

    public static void MoveAside(string bin, string rel)
    {
        var dir = Manifest.DirOf(bin);
        Directory.CreateDirectory(dir);
        File.Move(Path.Combine(bin, rel), Path.Combine(dir, rel.Replace('\\', '_') + ".bak"), true);
    }
}
