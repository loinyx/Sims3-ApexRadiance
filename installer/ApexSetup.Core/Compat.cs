using System.Text.RegularExpressions;

namespace ApexSetup;

public enum Severity { Info, Warning, Error }

// One thing about the player's install that can break or confuse a game start. Text is an English key with {0} arguments
// (the UI translates it); Fix, when there is one, makes it right and is safe to run more than once. Suggest, the plain name
// of a change Apex Radiance recommends in game: its Fix runs only when the player clicks Apply, never with Install.
public sealed record Finding(Severity Severity, string Text, string[] Args, Action? Fix = null, string? Link = null, string? Suggest = null)
{
    public bool CanFix => Fix is not null && Suggest is null;   // run by Install, Repair and Fix settings
    public bool CanApply => Fix is not null && Suggest is not null;
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
        var combined = false;
        foreach (var asi in new[] { "", "scripts", "plugins" }.Where(d => Directory.Exists(B(d))).SelectMany(d => Directory.EnumerateFiles(B(d), "*.asi")))
        {
            var name = Path.GetFileName(asi);
            if (name.Equals("ApexRadiance.asi", StringComparison.OrdinalIgnoreCase) || name.Equals("S3SSApex.asi", StringComparison.OrdinalIgnoreCase)) continue;
            var rel = Path.GetRelativePath(bin, asi);
            // the old standalone (S3SSApex.asi under another name): the Apex Edition name without S3SS's two strings, as
            // s3ss_detect.cpp tells them apart; S3SS stays recommended
            if (Scan.Contains(asi, "Apex Edition") && !(Scan.Contains(asi, "S3SS Log - Started at ") && Scan.Contains(asi, "###S3SSWindow")))
                f.Add(new(Severity.Error, "{0} in Game\\Bin loads a second copy of Apex Radiance.", new[] { rel }, () => MoveAside(bin, rel)));
            else if (Scan.Contains(asi, "Apex Edition"))
            {
                combined = true;
                f.Add(new(Severity.Error, "{0} is an old build with Apex inside; while it is in Game\\Bin, Apex Radiance stays off. Setup moves it to the backup folder (install the official Sims3SettingsSetter in its place if you used it).",
                    new[] { rel }, () => MoveAside(bin, rel)));
            }
            // a renamed ApexRadiance.asi: the copy loaded second stays idle (apex_main.cpp DllMain; its debugger line is in
            // every release since 1.0.0 and not in the combined build); without ApexRadiance.asi it is the copy that runs
            else if (File.Exists(B("ApexRadiance.asi")) && Scan.Contains(asi, "Another copy of ApexRadiance.asi is already running"))
                f.Add(new(Severity.Warning, "{0} in Game\\Bin is another copy of Apex Radiance; only one copy runs. Apply moves it to the backup folder.",
                    new[] { rel }, () => MoveAside(bin, rel), Suggest: "Two copies of the mod"));
        }

        // a mod ASI both in Game\Bin and in the loader's scripts or plugins folder loads twice: the subfolder copy goes
        foreach (var name in new[] { "Sims3SettingsSetter.asi", "MonoPatcher.asi", "ts3native.asi", "Sims3Performance.asi", "TS3Patch.asi" })
            foreach (var rel in new[] { $@"scripts\{name}", $@"plugins\{name}" })
                if (File.Exists(B(name)) && File.Exists(B(rel)))
                    f.Add(new(Severity.Error, "{0} in Game\\Bin is a second copy of {1}; both load. Setup moves it to the backup folder.", new[] { rel, name }, () => MoveAside(bin, rel)));

        // two loaders load every mod twice: keep the first (wininet.dll, as the mods' guides name it)
        var loaders = Scan.Loaders(bin);
        if (loaders.Count > 1)
            f.Add(new(Severity.Error, "Two ASI loaders ({0}) load every mod twice. Keep one: remove all but {1}.", new[] { string.Join(", ", loaders), loaders[0] },
                () => { foreach (var l in loaders.Skip(1)) MoveAside(bin, l); }));

        if (s.Ts3NativeWrongBuild && !g.ModernizationSupported)
            f.Add(new(Severity.Error, "Sims 3 Modernization has no build for this game version ({0}), so its ts3native does nothing. Turn it off in Components.",
                new[] { g.Version }, Link: ModernizationPlayers));
        else if (s.Ts3NativeWrongBuild)
            f.Add(new(Severity.Error, "Sims 3 Modernization's ts3native is the build for the other edition and does nothing. Turn it on in Components to install the right one.",
                Array.Empty<string>(), Link: ModernizationPlayers));

        // packages are only read through Mods\Resource.cfg: count the ones the cfg Setup writes would read
        var cfg = Path.Combine(mods, "Resource.cfg");
        if (!File.Exists(cfg) && packages.Count(p => CfgReads(Engine.ResourceCfg, p)) is var reads and > 0)
            f.Add(new(Severity.Error, "Mods has {0} .package files but no Resource.cfg, so the game loads none of them.", new[] { reads.ToString() },
                () => Engine.EnsureResourceCfg(docs)));
        // Setup's own packages (ts3fixes, MonoPatcher's subfolder) under a Resource.cfg that does not reach them
        if (File.Exists(cfg) && File.ReadAllText(cfg) is var cfgText)
            foreach (var p in packages.Where(p => (Path.GetFileName(p).Equals("ts3fixes.package", StringComparison.OrdinalIgnoreCase)
                                                   || Path.GetFileName(p).StartsWith("ld_MonoPatcher", StringComparison.OrdinalIgnoreCase)) && !CfgReads(cfgText, p)))
                f.Add(new(Severity.Warning, "Mods\\Resource.cfg does not read {0}, so the game does not load it. Setup adds the Packages folder lines (a copy is kept as Resource.cfg.apex-backup).",
                    new[] { p }, () => Rewrite(cfg, t => t.TrimEnd() + "\r\n" + string.Concat(Engine.ResourceCfg.Split("\r\n").Skip(1).Where(l => l != "" && !t.Contains(l, StringComparison.OrdinalIgnoreCase)).Select(l => l + "\r\n")))));

        // script mods built for the other edition (sims3-modernization's EA app notes)
        // by version, not edition: Origin 1.69.43 takes the 1.69 builds too
        var v169 = g.Version.StartsWith("1.69");
        string other = v169 ? "steam" : "1.69";
        foreach (var p in packages.Where(p => (p.Contains("simler90", StringComparison.OrdinalIgnoreCase) || p.Contains("XCAS", StringComparison.OrdinalIgnoreCase))
                                              && p.Contains(other, StringComparison.OrdinalIgnoreCase)))
            f.Add(new(Severity.Warning, "{0} is the build for the other edition of the game; it makes the game show \"unofficial game modification\". Use the {1} build.",
                new[] { p, v169 ? "1.69" : "Steam" }, Link: ModernizationPlayers));
        if (g.Edition == Edition.EA && packages.Any(p => Path.GetFileName(p).StartsWith("NRaas_ErrorTrap", StringComparison.OrdinalIgnoreCase)))
            f.Add(new(Severity.Warning, "NRaas ErrorTrap has no build known to work on EA app 1.69.47.", Array.Empty<string>(), Link: ModernizationPlayers));

        // mods that reach the same game code as Sims 3 Modernization (its static scan; not known to break anything)
        if (s.Modernization)
            foreach (var p in packages.Where(p => Path.GetFileName(p) is "Buzz_MoarInteractions.package" or "Overrides.package"))
                f.Add(new(Severity.Info, "{0} overrides game code Sims 3 Modernization also patches (not tested together).", new[] { p }, Link: ModernizationCompat));

        // Smooth Patch's legacy ASI patches the same SleepEx wrapper as S3SS's Smooth Patch (sims3-modernization fixes.json)
        if (Scan.FindAsi(bin, "TS3Patch.asi") is { } ts3patch && s.S3ss)
        {
            var rel = Path.GetRelativePath(bin, ts3patch);
            var txt = Path.ChangeExtension(rel, ".txt");
            f.Add(new(Severity.Error, "Smooth Patch's TS3Patch.asi patches the same game code as Sims3SettingsSetter's Smooth Patch. Setup moves it to the backup folder (S3SS does the same job).",
                Array.Empty<string>(), () => { MoveAside(bin, rel); if (File.Exists(B(txt))) MoveAside(bin, txt); }));
        }
        // two core mods that replace the same game module
        if (packages.Any(p => Path.GetFileName(p).StartsWith("simler90GameplayCoreMod", StringComparison.OrdinalIgnoreCase))
            && packages.Any(p => Path.GetFileName(p).Contains("StoryProgression", StringComparison.OrdinalIgnoreCase)))
            f.Add(new(Severity.Warning, "simler90's Gameplay Core Mod and NRaas StoryProgression replace the same game code. Keep one of them.", Array.Empty<string>(), Link: ModernizationCompat));
        // MonoPatcher needs a loader for its ASI, and the Microsoft Visual C++ x86 runtime
        if (s.MonoPatcher && !File.Exists(Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.SystemX86), "vcruntime140.dll")))
            f.Add(new(Severity.Warning, "MonoPatcher needs the Microsoft Visual C++ Redistributable (x86); it is not installed.", Array.Empty<string>(),
                Link: "https://aka.ms/vs/17/release/vc_redist.x86.exe"));

        // Sims3Performance next to sims3-modernization: the two thread patches clash (framework/s3pp_settings.cpp)
        var s3pp = Scan.FindAsi(bin, "Sims3Performance.asi");
        var s3ppIni = Path.ChangeExtension(s3pp ?? "", ".ini");
        if (s3pp is not null && s.Ts3Native && File.Exists(s3ppIni)
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
            var wrong = new List<string>();
            FixDxvk(text, wrong);
            if (wrong.Count > 0)
                f.Add(new(Severity.Warning, "dxvk.conf lacks {0} or has other values: without them the game resets its graphics options or freezes on a resolution change under the bridge.",
                    new[] { string.Join(", ", wrong) },
                    () => { if (File.Exists(dxvk)) Rewrite(dxvk, t => FixDxvk(t)); else Fs.Write(dxvk, FixDxvk("")); }));
        }
        Suggestions(f, s, docs, combined);
        // each fix its own restore point, unless the action running it has one open
        return f.Select(x => x.Fix is not { } fix ? x : x with { Fix = () =>
        {
            using var _ = x.Suggest is { } name ? RestorePoints.Begin(bin, "Apply suggestion: {0}", name) : RestorePoints.Begin(bin, "Fix incompatible settings");
            fix();
        } }).ToList();
    }

    // What Apex Radiance recommends in game (apex_gui.cpp: Overview, Compatibility, Attention), in its words where they fit.
    // Only files that exist are read; a value Setup cannot read is reported and its file left as it is. Apex makes them only
    // while it runs: none without ApexRadiance.asi
    static void Suggestions(List<Finding> f, State s, string docs, bool combined)
    {
        if (s.ApexVersion is null) return;
        // the game's MSAA blocks Apex's Edge Smoothing, Depth Blur, Ambient Occlusion and Water Reflections
        // (apex_gui.cpp GameAaCompatibilityNotice). Apex reads the back buffer; Setup reads Options.ini's edgesmoothing,
        // whose non-zero values are taken as MSAA on (0 seen with no MSAA; the others unverified in game)
        var options = Engine.OptionsIni(docs);
        if (File.Exists(options) && Values(File.ReadAllText(options), "options", "edgesmoothing").LastOrDefault() is { } aa)
        {
            if (!int.TryParse(aa, out var level)) f.Add(Unreadable("Options.ini", "edgesmoothing", aa));
            else if (level != 0 && ApexEffectsBlockedByMsaa(f, Path.Combine(docs, "Apex Radiance", "ApexRadiance.toml")) is { Count: > 0 } effects)
                f.Add(new(Severity.Warning, "Options.ini has edgesmoothing = {0}. The Sims 3's Edge Smoothing prevents these enabled Apex effects from working: {1}. Apply sets it to 0 (a copy is kept as Options.ini.apex-backup).",
                    new[] { aa, string.Join(", ", effects) }, () => Rewrite(options, t => Set(t, "options", new[] { "edgesmoothing" }, v => v != "0", "0")),
                    Suggest: "Turn off the game's Edge Smoothing"));
        }
        // the renderer and S3SS Apex recommends (apex_gui.cpp kRecommend*): components, so a pointer to Components only
        if (s.Layer is Layer.Native or Layer.Other)
            f.Add(new(Severity.Info, "Runs the game on Vulkan: fewer stutters, and Apex Radiance is made and tested with it.", Array.Empty<string>(), Suggest: "DXVK"));
        if (!s.S3ss && !combined)
            f.Add(new(Severity.Info, "Apex Radiance works on its own, but it pairs well with Sims3SettingsSetter by sims3fiend: a frame rate limiter, fewer stutters and many extra game settings.",
                Array.Empty<string>(), Suggest: "Sims3SettingsSetter"));
        if (s.Layer == Layer.Dxvk)
            f.Add(new(Severity.Info, "Moves DXVK into its own 64-bit process: the game keeps about 600 MB more free memory for long sessions and big worlds.",
                Array.Empty<string>(), Suggest: "RTX Remix bridge"));
    }

    // the Apex effects on in ApexRadiance.toml that the game's MSAA blocks; each is on by default, as with no file
    static List<string> ApexEffectsBlockedByMsaa(List<Finding> f, string toml)
    {
        var text = File.Exists(toml) ? File.ReadAllText(toml) : "";
        var on = new List<string>();
        bool Enabled(string patch)
        {
            var v = Values(text, "patches." + patch, "enabled").LastOrDefault() ?? "true";
            if (v is "true" or "false") return v == "true";
            f.Add(Unreadable("ApexRadiance.toml", $"[patches.{patch}] enabled", v));
            return false;
        }
        foreach (var (patch, name) in new[] { ("EdgeSmoothing", "Edge Smoothing"), ("DepthBlur", "Depth Blur"), ("AmbientOcclusion", "Ambient Occlusion") })
            if (Enabled(patch)) on.Add(name);
        // Water Reflections: Night Lighting's reflexoNoLago above 0 (default 1)
        if (Enabled("NightTerrainRelight"))
        {
            var v = Values(text, "patches.NightTerrainRelight", "reflexoNoLago").LastOrDefault() ?? "1";
            if (!double.TryParse(v, System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out var shore))
                f.Add(Unreadable("ApexRadiance.toml", "reflexoNoLago", v));
            else if (shore > 0) on.Add("Water Reflections");
        }
        return on;
    }

    static Finding Unreadable(string file, string key, string value) =>
        new(Severity.Warning, "{0} has {1} = {2}, which Setup cannot read. Setup leaves the file as it is.", new[] { file, key, value });

    // native/remix-bridge/conf/dxvk.conf
    static readonly Dictionary<string, string> BridgeDxvk = new()
    {
        ["d3d9.customVendorId"] = "10de", ["d3d9.customDeviceId"] = "1080", ["d3d9.customDeviceDesc"] = "\"Geforce GTX 580\"",
        ["d3d9.countLosableResources"] = "False",
    };

    // dxvk.conf with the bridge keys set in its global part (before the first [exe] section, which the bridge server
    // does not read): wrong values replaced in place, missing keys added at the top; wrong gets the keys that were off
    public static string FixDxvk(string text, List<string>? wrong = null)
    {
        int g = Regex.Match(text, @"(?m)^[ \t]*\[") is { Success: true } m ? m.Index : text.Length;
        var head = text[..g];
        var add = "";
        foreach (var (key, value) in BridgeDxvk)
        {
            var re = new Regex($@"(?mi)^([ \t]*{Regex.Escape(key)}[ \t]*=[ \t]*)([^\r\n]*?)([ \t]*)(?=\r?$)");
            var ms = re.Matches(head);
            if (ms.Count == 0) add += $"{key} = {value}\r\n";
            else if (ms.All(x => x.Groups[2].Value.Equals(value, StringComparison.OrdinalIgnoreCase))) continue;
            else head = re.Replace(head, x => x.Groups[1].Value + value + x.Groups[3].Value);
            wrong?.Add(key);
        }
        return add + head + text[g..];
    }

    // Mods\Resource.cfg reads that package (path relative to Mods) through one of its PackedFile patterns
    static bool CfgReads(string cfg, string rel) =>
        Regex.Matches(cfg, @"(?mi)^\s*PackedFile\s+(\S+)").Any(m => Regex.IsMatch(rel.Replace('\\', '/'),
            "^" + Regex.Escape(m.Groups[1].Value.Replace('\\', '/')).Replace(@"\*", "[^/]*").Replace(@"\?", "[^/]") + "$", RegexOptions.IgnoreCase));

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
            // without case, as Apex's ForPatchKey (SameText)
            if (trimmed.StartsWith('[')) inSection = trimmed.IndexOf(']') is var c and > 0 && trimmed[1..c].Trim().Equals(section, StringComparison.OrdinalIgnoreCase);
            else if (inSection && !trimmed.StartsWith('#') && !trimmed.StartsWith(';') && line.IndexOf('=') is var eq and > 0
                     && keys.Contains(line[..eq].Trim(), StringComparer.OrdinalIgnoreCase))
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
        var attributes = File.GetAttributes(file); // (kept: a player's read-only Options.ini stays read-only)
        foreach (var x in new[] { file, file + ".apex-backup" }) // a read-only file (or its copy from an earlier run) would block the copy and the move
            Fs.ClearReadOnly(x);
        Fs.Copy(file, file + ".apex-backup");
        Fs.Write(file + ".apex-new", changed);
        Fs.Move(file + ".apex-new", file);
        Fs.SetAttributes(file, attributes);
    }

    public static void MoveAside(string bin, string rel)
    {
        var dir = Manifest.DirOf(bin);
        Fs.CreateDir(dir);
        Fs.Move(Path.Combine(bin, rel), Path.Combine(dir, rel.Replace('\\', '_') + ".bak"));
    }
}
