using System.Text.Json;
using System.Text.RegularExpressions;

namespace ApexSetup;

public static class Components
{
    public const string Apex = "apex", Bridge = "bridge", Loader = "loader", S3ss = "s3ss", Modernization = "modernization";
    // removal order: what depends on the loader goes first
    public static readonly string[] All = { Bridge, Modernization, S3ss, Apex, Loader };
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
    readonly string _tmp = Path.Combine(Path.GetTempPath(), "ApexRadianceSetup", Guid.NewGuid().ToString("N"));

    public async Task<(List<(string, string)>, string)> FetchAsync(string c, Game g, string docs, CancellationToken ct)
    {
        var bin = g.Bin;
        var files = new List<(string, string)>();
        switch (c)
        {
            case Components.Apex:
            {
                var r = await Releases.LatestAsync(Releases.Apex, ct);
                files.Add((await Releases.DownloadAsync(r.Find(n => n == "ApexRadiance.asi"), _tmp, ct), Path.Combine(bin, "ApexRadiance.asi")));
                return (files, r.Version);
            }
            case Components.Bridge:
            {
                var r = await Releases.LatestAsync(Releases.Apex, ct);
                var dir = Releases.Unzip(await Releases.DownloadAsync(r.Find(n => n == "Apex-Radiance-RTX-Remix-bridge.zip"), _tmp, ct));
                AddTree(files, dir, bin);
                return (files, r.Version);
            }
            case Components.Loader:
            {
                var r = await Releases.LatestAsync(Releases.AsiLoader, ct);
                var dir = Releases.Unzip(await Releases.DownloadAsync(r.Find(n => n == "Ultimate-ASI-Loader-NoPDB.zip"), _tmp, ct));
                files.Add((Path.Combine(dir, "dinput8.dll"), Path.Combine(bin, "wininet.dll"))); // the 32-bit loader, named as the mods' guides say
                return (files, r.Version);
            }
            case Components.S3ss:
            {
                var r = await Releases.LatestAsync(Releases.S3ss, ct);
                files.Add((await Releases.DownloadAsync(r.Find(n => n == "Sims3SettingsSetter.asi"), _tmp, ct), Path.Combine(bin, "Sims3SettingsSetter.asi")));
                return (files, r.Version);
            }
            case Components.Modernization:
            {
                var r = await Releases.LatestAsync(Releases.Modernization, ct);
                var prefix = g.Edition == Edition.EA ? "ts3native-ea169-" : "ts3native-v";
                var dir = Releases.Unzip(await Releases.DownloadAsync(r.Find(n => n.StartsWith(prefix) && n.EndsWith(".zip")), _tmp, ct));
                AddTree(files, dir, bin);
                files.Add((await Releases.DownloadAsync(r.Find(n => n == "ts3fixes.package"), _tmp, ct), Path.Combine(Scan.Packages(docs), "ts3fixes.package")));
                return (files, r.Version);
            }
        }
        throw new ArgumentException(c);
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
        [Components.S3ss] = new[] { "Sims3SettingsSetter.asi" },
        [Components.Modernization] = new[] { "ts3native.asi", "ts3native.ini" },
        [Components.Loader] = Array.Empty<string>(),
    };

    // An older copy of these is an update, not a foreign file: replaced without a backup.
    static readonly HashSet<string> Updates = new(StringComparer.OrdinalIgnoreCase)
        { "ApexRadiance.asi", "Sims3SettingsSetter.asi", "ts3native.asi", "ts3native.ini", "ts3fixes.package" };

    public static string OptionsIni(string docs) => Path.Combine(docs, "Options.ini");

    // The loader goes in when there is none, or when the only one is d3d9.dll and the bridge takes that name.
    public static bool NeedsLoader(State s, bool bridge) => s.Loader is null || (bridge && s.LoaderIsD3d9);

    public static async Task InstallAsync(Game g, IReadOnlyCollection<string> components, ISource src, IProgress<Progress>? p = null,
                                          string? docs = null, CancellationToken ct = default)
    {
        docs ??= Scan.Documents;
        p?.Report(new(0, "Checking that the game is closed"));
        if (Games.IsRunning()) throw new InvalidOperationException("Close The Sims 3 first.");
        var s = Scan.Run(g, docs);
        var m = s.Manifest;
        var list = Components.All.Reverse().Where(components.Contains).ToList(); // loader first, bridge last
        int i = 0;
        foreach (var c in list)
        {
            p?.Report(new((double)i / list.Count, "Downloading {0}", c));
            var (files, version) = await src.FetchAsync(c, g, docs, ct);
            p?.Report(new((i + 0.5) / list.Count, "Copying {0}", c));

            if (c == Components.Bridge && s.Layer is Layer.Native or Layer.Other && m.OptionsBackup is null && File.Exists(OptionsIni(docs)))
            {
                m.OptionsBackup = Path.Combine(Manifest.DirOf(g.Bin), "Options.ini.bak");
                Directory.CreateDirectory(Manifest.DirOf(g.Bin));
                File.Copy(OptionsIni(docs), m.OptionsBackup, true);
            }
            foreach (var (from, to) in files) Place(m, c, from, to);
            if (c == Components.Bridge)
                foreach (var conf in new[] { Path.Combine(g.Bin, "bridge.conf"), Path.Combine(g.Bin, ".trex", "bridge.conf") })
                    SetTargetProcess(conf, g.Exe);
            if (c == Components.Modernization) EnsureResourceCfg(docs);
            if (c == Components.Apex) RemoveLeftovers(m, g.Bin);
            m.Versions[c] = version;
            m.Save(); // after each component, so a failure later keeps what is done removable
            i++;
        }
        p?.Report(new(1, "Done"));
    }

    public static void Remove(Game g, string c, string? docs = null)
    {
        docs ??= Scan.Documents;
        if (Games.IsRunning()) throw new InvalidOperationException("Close The Sims 3 first.");
        var m = Manifest.Load(g.Bin);
        var layerBefore = Scan.Run(g, docs).Layer;
        // hand-installed files first, so a backup restored below is never taken for one
        foreach (var rel in Known[c])
        {
            var p = Path.Combine(g.Bin, rel);
            if (rel == "d3d9.dll" && layerBefore != Layer.Bridge) continue; // only the bridge's own d3d9.dll
            if (File.Exists(p) && !m.Files.Any(f => Same(f.Path, p))) File.Delete(p);
        }
        foreach (var f in m.Files.Where(f => f.Component == c).ToList())
        {
            if (File.Exists(f.Path)) File.Delete(f.Path);
            if (f.Backup is not null && File.Exists(f.Backup)) File.Move(f.Backup, f.Path, true);
            m.Files.Remove(f);
        }
        if (c == Components.Modernization)
        {
            var pkg = Path.Combine(Scan.Packages(docs), "ts3fixes.package");
            if (File.Exists(pkg)) File.Delete(pkg);
        }
        if (c == Components.Bridge)
        {
            foreach (var d in new[] { Path.Combine(g.Bin, ".trex"), Path.Combine(g.Bin, "licenses") })
                if (Directory.Exists(d) && !Directory.EnumerateFileSystemEntries(d).Any()) Directory.Delete(d);
            // the game had its own D3D9 before: the copy taken then matches the card it sees again, so nothing resets
            if (m.OptionsBackup is not null && File.Exists(m.OptionsBackup) && !File.Exists(Path.Combine(g.Bin, "d3d9.dll")))
                File.Copy(m.OptionsBackup, OptionsIni(docs), true);
            if (m.OptionsBackup is not null && File.Exists(m.OptionsBackup)) File.Delete(m.OptionsBackup);
            m.OptionsBackup = null;
        }
        m.Versions.Remove(c);
        m.Save();
        // the restored d3d9.dll is a loader again: the one Setup added for the bridge would load every mod twice
        if (c == Components.Bridge && m.Versions.ContainsKey(Components.Loader) && Scan.Run(g, docs).LoaderIsD3d9)
            Remove(g, Components.Loader, docs);
    }

    public static void RemoveAll(Game g, string? docs = null)
    {
        var s = Scan.Run(g, docs);
        foreach (var c in Components.All)
            if (c != Components.Loader || s.Manifest.Versions.ContainsKey(c)) Remove(g, c, docs);
    }

    static void Place(Manifest m, string c, string from, string to)
    {
        var owned = m.Files.FirstOrDefault(f => Same(f.Path, to));
        string? backup = owned?.Backup;
        if (owned is null && File.Exists(to) && !Updates.Contains(Path.GetFileName(to)))
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
    static void EnsureResourceCfg(string docs)
    {
        var cfg = Path.Combine(docs, "Mods", "Resource.cfg");
        if (File.Exists(cfg)) return;
        Directory.CreateDirectory(Path.GetDirectoryName(cfg)!);
        File.WriteAllText(cfg, "Priority 500\r\nPackedFile Packages/*.package\r\nPackedFile Packages/*/*.package\r\nPackedFile Packages/*/*/*.package\r\n");
    }

    static bool Same(string a, string b) => string.Equals(Path.GetFullPath(a), Path.GetFullPath(b), StringComparison.OrdinalIgnoreCase);
}
