using System.Text.Json;
using System.Text.RegularExpressions;

namespace ApexSetup;

public static class Components
{
    public const string Apex = "apex", Bridge = "bridge", Dxvk = "dxvk", Loader = "loader", S3ss = "s3ss", Modernization = "modernization", MonoPatcher = "monopatcher", S3pp = "s3pp";
    // removal order: what depends on the loader goes first (installs run in the reverse order)
    public static readonly string[] All = { Bridge, Dxvk, Modernization, MonoPatcher, S3pp, S3ss, Apex, Loader };
}

public sealed class ManifestFile
{
    public string Component { get; set; } = "";
    public string Path { get; set; } = "";
    public string? Backup { get; set; } // the file Setup moved aside, restored on removal
}

// What Setup put where, kept in Game\Bin\Apex Radiance backup\setup.json next to the backups.
public sealed class Manifest
{
    public List<ManifestFile> Files { get; set; } = new();
    public Dictionary<string, string> Versions { get; set; } = new();
    public string? OptionsBackup { get; set; }     // copy of Options.ini taken before the bridge replaced the game's own D3D9
    [System.Text.Json.Serialization.JsonIgnore] public string Bin { get; private set; } = "";

    public static string DirOf(string bin) => System.IO.Path.Combine(bin, "Apex Radiance backup");
    string FilePath => System.IO.Path.Combine(DirOf(Bin), "setup.json");

    public static Manifest Load(string bin)
    {
        var p = System.IO.Path.Combine(DirOf(bin), "setup.json");
        var m = File.Exists(p) ? JsonSerializer.Deserialize<Manifest>(File.ReadAllText(p)) ?? new() : new();
        m.Bin = bin;
        return m;
    }

    public void Save()
    {
        Directory.CreateDirectory(DirOf(Bin));
        File.WriteAllText(FilePath, JsonSerializer.Serialize(this, new JsonSerializerOptions { WriteIndented = true }));
    }
}

// Where the files of a component come from: (source file, destination) pairs and the version.
public interface ISource
{
    Task<(List<(string Src, string Dest)> Files, string Version)> FetchAsync(string component, Game g, string docs, CancellationToken ct);
}

public sealed class GitHubSource : ISource
{
    // Sims 3 Performance Patch is published on Nexus Mods only, which needs the player's own account: the player
    // downloads it there and Setup takes the archive from here
    public string? S3ppArchive { get; init; }

    // the releases Setup showed, by repo: every component of a run comes from them, not from a newer one published since
    public IReadOnlyDictionary<string, Release?>? Shown { get; init; }
    readonly Dictionary<string, Task<Release>> _rel = new();
    Task<Release> Latest(string repo, CancellationToken ct) =>
        _rel.TryGetValue(repo, out var t) ? t : _rel[repo] = Shown?.GetValueOrDefault(repo) is { } r ? Task.FromResult(r) : Releases.LatestAsync(repo, ct);

    public static readonly string TempRoot = Path.Combine(Path.GetTempPath(), "ApexRadianceSetup");
    readonly string _tmp = Path.Combine(TempRoot, Guid.NewGuid().ToString("N"));

    public async Task<(List<(string, string)>, string)> FetchAsync(string c, Game g, string docs, CancellationToken ct)
    {
        var bin = g.Bin;
        var files = new List<(string, string)>();
        switch (c)
        {
            case Components.Apex:
            {
                var r = await Latest(Releases.Apex, ct);
                files.Add((await Releases.DownloadAsync(r.Find(n => n == "ApexRadiance.asi"), _tmp, ct), Path.Combine(bin, "ApexRadiance.asi")));
                return (files, r.Version);
            }
            case Components.Bridge:
            {
                var r = await Latest(Releases.Apex, ct);
                var dir = Releases.Unzip(await Releases.DownloadAsync(r.Find(n => n == "Apex-Radiance-RTX-Remix-bridge.zip"), _tmp, ct));
                AddTree(files, dir, bin);
                return (files, r.Version);
            }
            case Components.Dxvk:
            {
                // the 32-bit d3d9.dll of the official release (Apex recommends DXVK: Settings > Compatibility)
                var r = await Latest(Releases.Dxvk, ct);
                var tgz = await Releases.DownloadAsync(r.Find(n => n.StartsWith("dxvk-") && n.EndsWith(".tar.gz") && !n.Contains("native")), _tmp, ct);
                files.Add((Releases.ExtractFromTarGz(tgz, "x32/d3d9.dll"), Path.Combine(bin, "d3d9.dll")));
                return (files, r.Version);
            }
            case Components.S3pp:
            {
                if (S3ppArchive is null) throw new InvalidOperationException("Download Sims 3 Performance Patch from Nexus Mods first.");
                var (asi, ini) = S3ppFiles(S3ppArchive) ?? throw new InvalidDataException("This file has no Sims3Performance.asi.");
                files.Add((asi, Path.Combine(bin, "Sims3Performance.asi")));
                if (ini is not null) files.Add((ini, Path.Combine(bin, "Sims3Performance.ini")));
                return (files, "Nexus Mods");
            }
            case Components.MonoPatcher:
            {
                // its zip also carries a wininet.dll loader: left out, Setup handles the loader (sims3-modernization's fixes.json)
                var r = await Latest(Releases.MonoPatcher, ct);
                var dir = Releases.Unzip(await Releases.DownloadAsync(r.Find(n => n.EndsWith(".zip")), _tmp, ct));
                foreach (var f in Directory.EnumerateFiles(dir, "*", SearchOption.AllDirectories))
                    if (Path.GetFileName(f).Equals("MonoPatcher.asi", StringComparison.OrdinalIgnoreCase)) files.Add((f, Path.Combine(bin, "MonoPatcher.asi")));
                    else if (f.EndsWith(".package", StringComparison.OrdinalIgnoreCase))
                        files.Add((f, Path.Combine(Scan.Packages(docs), "MonoPatcher", Path.GetFileName(f))));
                if (!files.Any(x => x.Item2.EndsWith("MonoPatcher.asi"))) throw new InvalidDataException("MonoPatcher: MonoPatcher.asi not in the release");
                return (files, r.Version);
            }
            case Components.Loader:
            {
                var r = await Latest(Releases.AsiLoader, ct);
                var dir = Releases.Unzip(await Releases.DownloadAsync(r.Find(n => n == "Ultimate-ASI-Loader-NoPDB.zip"), _tmp, ct));
                files.Add((Path.Combine(dir, "dinput8.dll"), Path.Combine(bin, "wininet.dll"))); // the 32-bit loader, named as the mods' guides say
                return (files, r.Version);
            }
            case Components.S3ss:
            {
                var r = await Latest(Releases.S3ss, ct);
                files.Add((await Releases.DownloadAsync(r.Find(n => n == "Sims3SettingsSetter.asi"), _tmp, ct), Path.Combine(bin, "Sims3SettingsSetter.asi")));
                return (files, r.Version);
            }
            case Components.Modernization:
            {
                var r = await Latest(Releases.Modernization, ct);
                var prefix = g.Edition == Edition.EA ? "ts3native-ea169-" : "ts3native-v";
                var dir = Releases.Unzip(await Releases.DownloadAsync(r.Find(n => n.StartsWith(prefix) && n.EndsWith(".zip")), _tmp, ct));
                AddTree(files, dir, bin);
                files.Add((await Releases.DownloadAsync(r.Find(n => n == "ts3fixes.package"), _tmp, ct), Path.Combine(Scan.Packages(docs), "ts3fixes.package")));
                return (files, r.Version);
            }
        }
        throw new ArgumentException(c);
    }

    // Sims3Performance.asi (and its .ini) from a .zip the player downloaded, or from the .asi itself
    public static (string Asi, string? Ini)? S3ppFiles(string path)
    {
        string dir;
        if (path.EndsWith(".zip", StringComparison.OrdinalIgnoreCase))
        {
            try { dir = Releases.Unzip(CopyToTemp(path)); }
            catch (InvalidDataException) { return null; }
        }
        else dir = Path.GetDirectoryName(path)!;
        var asi = Directory.EnumerateFiles(dir, "Sims3Performance.asi", SearchOption.AllDirectories).FirstOrDefault();
        if (asi is null) return null;
        var ini = Path.Combine(Path.GetDirectoryName(asi)!, "Sims3Performance.ini");
        return (asi, File.Exists(ini) ? ini : null);
    }

    static string CopyToTemp(string file)
    {
        var dir = Path.Combine(TempRoot, Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(dir);
        var copy = Path.Combine(dir, Path.GetFileName(file));
        File.Copy(file, copy);
        return copy;
    }

    public static void AddTree(List<(string, string)> files, string dir, string dest)
    {
        foreach (var f in Directory.EnumerateFiles(dir, "*", SearchOption.AllDirectories))
            files.Add((f, Path.Combine(dest, Path.GetRelativePath(dir, f))));
    }
}

public sealed record Progress(double Fraction, string Text, string? Arg = null);

public static class Engine
{
    // Files a component may own although Setup did not install them (installed by hand): removed by Remove, never restored.
    static readonly Dictionary<string, string[]> Known = new()
    {
        [Components.Apex] = new[] { "ApexRadiance.asi" },
        [Components.Bridge] = new[] { "d3d9.dll", "bridge.conf", "dxvk.conf", @".trex\bridge.conf", @".trex\NvRemixBridge.exe", @".trex\d3d9vk_x64.dll", "Apex Radiance RTX Remix bridge.txt" },
        [Components.Dxvk] = new[] { "d3d9.dll" },
        [Components.S3ss] = Loadable("Sims3SettingsSetter.asi"),
        [Components.MonoPatcher] = Loadable("MonoPatcher.asi"),
        [Components.S3pp] = Loadable("Sims3Performance.asi", "Sims3Performance.ini"),
        [Components.Modernization] = Loadable("ts3native.asi", "ts3native.ini", "ts3native.log"),
        [Components.Loader] = Array.Empty<string>(),
    };

    // in Game\Bin or the loader's scripts or plugins folder (Scan.FindAsi)
    static string[] Loadable(params string[] names) => names.SelectMany(n => new[] { n, $@"scripts\{n}", $@"plugins\{n}" }).ToArray();

    // An older copy of these is an update, not a foreign file: replaced without a backup.
    static readonly HashSet<string> Updates = new(StringComparer.OrdinalIgnoreCase)
        { "ApexRadiance.asi", "Sims3SettingsSetter.asi", "ts3native.asi", "ts3fixes.package", "Sims3Performance.asi", "MonoPatcher.asi" };

    // APEX_SETUP_DRY_RUN=1: Setup shows what it finds and plans but writes nothing (a check on a player's real game)
    public static void CheckWritable()
    {
        if (Environment.GetEnvironmentVariable("APEX_SETUP_DRY_RUN") == "1") throw new InvalidOperationException("Dry run: Setup changes nothing.");
        if (Games.IsRunning()) throw new InvalidOperationException("Close The Sims 3 first.");
    }

    public static string OptionsIni(string docs) => Path.Combine(docs, "Options.ini");

    // The loader goes in when there is none, or when the only one is d3d9.dll and the bridge takes that name.
    public static bool NeedsLoader(State s, bool replacesD3d9) => s.Loader is null || (replacesD3d9 && s.LoaderIsD3d9);

    public static async Task InstallAsync(Game g, IReadOnlyCollection<string> components, ISource src, IProgress<Progress>? p = null,
                                          string? docs = null, CancellationToken ct = default)
    {
        docs ??= Scan.DocumentsFor(g);
        p?.Report(new(0, "Checking that the game is closed"));
        CheckWritable();
        var s = Scan.Run(g, docs);
        var m = s.Manifest;
        var list = Components.All.Reverse().Where(components.Contains).ToList(); // loader first, bridge last
        // everything is downloaded before any file is placed: a network or checksum failure changes nothing
        var fetched = new List<(List<(string Src, string Dest)> Files, string Version)>();
        for (int i = 0; i < list.Count; i++)
        {
            p?.Report(new(0.5 * i / list.Count, "Downloading {0}", list[i]));
            fetched.Add(await src.FetchAsync(list[i], g, docs, ct));
        }
        if (list.Count > 0) CheckWritable(); // the downloads can take minutes: the game may have started since
        for (int i = 0; i < list.Count; i++)
        {
            var c = list[i];
            var (files, version) = fetched[i];
            p?.Report(new(0.5 + 0.48 * i / list.Count, "Copying {0}", c));
            try
            {
                if (c is Components.Bridge or Components.Dxvk && s.Layer is Layer.Native or Layer.Other && m.OptionsBackup is null && File.Exists(OptionsIni(docs)))
                {
                    m.OptionsBackup = Path.Combine(Manifest.DirOf(g.Bin), "Options.ini.bak");
                    Directory.CreateDirectory(Manifest.DirOf(g.Bin));
                    File.Copy(OptionsIni(docs), m.OptionsBackup, true);
                }
                // a bridge installed by hand is this component's own files, not foreign ones to back up and restore on removal
                var own = c == Components.Bridge && s.Layer == Layer.Bridge;
                foreach (var (from, to) in files) Place(m, c, from, to, own);
                if (c == Components.Bridge)
                    foreach (var conf in new[] { Path.Combine(g.Bin, "bridge.conf"), Path.Combine(g.Bin, ".trex", "bridge.conf") })
                        SetTargetProcess(conf, g.Exe);
                // the log of a build that skipped itself would still say "wrong build" until the game runs
                if (c == Components.Modernization) { EnsureResourceCfg(docs); File.Delete(Path.Combine(g.Bin, "ts3native.log")); }
                if (c == Components.Apex) RemoveLeftovers(m, g.Bin);
                m.Versions[c] = version;
            }
            finally { m.Save(); } // also when a copy fails: the files placed and their backups stay removable
        }
        // the install leaves nothing known broken (checks after the copies: the bridge, Sims3Performance and S3SS rules depend on them)
        p?.Report(new(0.98, "Fixing incompatible settings"));
        foreach (var fix in Compat.Check(g, Scan.Run(g, docs), docs).Where(x => x.CanFix))
            try { fix.Fix!(); }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { } // the files are in place; Check installation still shows this one
        p?.Report(new(1, "Done"));
    }

    public static void Remove(Game g, string c, string? docs = null)
    {
        docs ??= Scan.DocumentsFor(g);
        CheckWritable();
        var m = Manifest.Load(g.Bin);
        var layerBefore = Scan.Run(g, docs).Layer;
        // hand-installed files first, so a backup restored below is never taken for one
        foreach (var rel in Known[c])
        {
            var p = Path.Combine(g.Bin, rel);
            // only the d3d9.dll of the layer being removed
            if (rel == "d3d9.dll" && layerBefore != (c == Components.Dxvk ? Layer.Dxvk : Layer.Bridge)) continue;
            if (File.Exists(p) && !m.Files.Any(f => Same(f.Path, p))) File.Delete(p);
        }
        foreach (var f in m.Files.Where(f => f.Component == c).ToList())
        {
            if (File.Exists(f.Path)) File.Delete(f.Path);
            if (f.Backup is not null && File.Exists(f.Backup)) File.Move(f.Backup, f.Path, true);
            m.Files.Remove(f);
        }
        if (c == Components.MonoPatcher && Directory.Exists(Path.Combine(Scan.Packages(docs), "MonoPatcher")))
            foreach (var f in Directory.EnumerateFiles(Path.Combine(Scan.Packages(docs), "MonoPatcher"), "ld_MonoPatcher*.package")) File.Delete(f);
        if (c == Components.Modernization)
        {
            var pkg = Path.Combine(Scan.Packages(docs), "ts3fixes.package");
            if (File.Exists(pkg)) File.Delete(pkg);
        }
        if (c is Components.Bridge or Components.Dxvk)
        {
            foreach (var d in new[] { Path.Combine(g.Bin, ".trex"), Path.Combine(g.Bin, "licenses") })
                if (Directory.Exists(d) && !Directory.EnumerateFileSystemEntries(d).Any()) Directory.Delete(d);
            // the game is back on its own D3D9 (a restored loader or wrapper included): the copy taken then matches the card it sees again
            if (m.OptionsBackup is not null && File.Exists(m.OptionsBackup) && Scan.Run(g, docs).Layer is Layer.Native or Layer.Other)
                File.Copy(m.OptionsBackup, OptionsIni(docs), true);
            if (m.OptionsBackup is not null && File.Exists(m.OptionsBackup)) File.Delete(m.OptionsBackup);
            m.OptionsBackup = null;
        }
        m.Versions.Remove(c);
        m.Save();
        // the restored d3d9.dll is a loader again: the one Setup added for the bridge would load every mod twice
        if (c is Components.Bridge or Components.Dxvk && m.Versions.ContainsKey(Components.Loader) && Scan.Run(g, docs).LoaderIsD3d9)
            Remove(g, Components.Loader, docs);
    }

    public static void RemoveAll(Game g, string? docs = null)
    {
        // Apex, and what Setup installed; mods the player installed by hand stay (Repair or remove has a row for each)
        var s = Scan.Run(g, docs);
        foreach (var c in Components.All)
            if (c == Components.Apex || s.Manifest.Versions.ContainsKey(c)) Remove(g, c, docs);
    }

    static void Place(Manifest m, string c, string from, string to, bool own = false)
    {
        var owned = m.Files.FirstOrDefault(f => Same(f.Path, to));
        // the player's ts3native.ini holds their feature switches: kept, only a missing one is written
        if (Path.GetFileName(to).Equals("ts3native.ini", StringComparison.OrdinalIgnoreCase) && File.Exists(to))
        {
            if (owned is null) m.Files.Add(new ManifestFile { Component = c, Path = to });
            else owned.Component = c;
            return;
        }
        string? backup = owned?.Backup;
        if (owned is null && File.Exists(to) && !Updates.Contains(Path.GetFileName(to))
            && !(own && Known[c].Contains(Path.GetRelativePath(m.Bin, to), StringComparer.OrdinalIgnoreCase)))
        {
            var rel = to.StartsWith(m.Bin, StringComparison.OrdinalIgnoreCase) ? Path.GetRelativePath(m.Bin, to) : Path.GetFileName(to);
            backup = Path.Combine(Manifest.DirOf(m.Bin), rel.Replace('\\', '_') + ".bak");
            Directory.CreateDirectory(Manifest.DirOf(m.Bin));
            File.Move(to, backup, true);
        }
        Directory.CreateDirectory(Path.GetDirectoryName(to)!);
        File.Copy(from, to, true);
        if (owned is null) m.Files.Add(new ManifestFile { Component = c, Path = to, Backup = backup });
        else owned.Component = c;
    }

    // An older combined build or a second copy of the ASI would load Apex twice.
    static void RemoveLeftovers(Manifest m, string bin)
    {
        foreach (var p in new[] { Path.Combine(bin, "S3SSApex.asi"), Path.Combine(bin, "scripts", "ApexRadiance.asi"), Path.Combine(bin, "plugins", "ApexRadiance.asi") })
            if (File.Exists(p))
            {
                Directory.CreateDirectory(Manifest.DirOf(bin));
                File.Move(p, Path.Combine(Manifest.DirOf(bin), Path.GetRelativePath(bin, p).Replace('\\', '_') + ".bak"), true);
            }
    }

    public static void SetTargetProcess(string conf, string exe)
    {
        if (!File.Exists(conf)) return;
        var text = File.ReadAllText(conf);
        File.WriteAllText(conf, Regex.Replace(text, @"(?m)^(\s*client\.targetProcess\s*=\s*)\S+", "${1}" + exe));
    }

    // sims3-modernization's player guide: Mods\Resource.cfg, created only when missing (kept on removal: other mods use it)
    public static void EnsureResourceCfg(string docs)
    {
        var cfg = Path.Combine(docs, "Mods", "Resource.cfg");
        if (File.Exists(cfg)) return;
        Directory.CreateDirectory(Path.GetDirectoryName(cfg)!);
        File.WriteAllText(cfg, ResourceCfg);
    }
    public const string ResourceCfg = "Priority 500\r\nPackedFile Packages/*.package\r\nPackedFile Packages/*/*.package\r\nPackedFile Packages/*/*/*.package\r\n";

    static bool Same(string a, string b) => string.Equals(Path.GetFullPath(a), Path.GetFullPath(b), StringComparison.OrdinalIgnoreCase);
}
