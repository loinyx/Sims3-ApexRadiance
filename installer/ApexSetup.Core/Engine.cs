using System.Text.Json;
using System.Text.RegularExpressions;

namespace ApexSetup;

public static class Components
{
    public const string Apex = "apex", Bridge = "bridge", Dxvk = "dxvk", Loader = "loader", S3ss = "s3ss", Modernization = "modernization", MonoPatcher = "monopatcher", S3pp = "s3pp",
        RestartHost = "restarthost";
    // removal order: what depends on the loader goes first (installs run in the reverse order)
    public static readonly string[] All = { Bridge, Dxvk, Modernization, MonoPatcher, S3pp, S3ss, RestartHost, Apex, Loader };

    public static string Name(string c) => c switch
    {
        Apex => "Apex Radiance", Bridge => "RTX Remix bridge", Loader => "Ultimate ASI Loader", Dxvk => "DXVK", S3ss => "Sims3SettingsSetter",
        MonoPatcher => "MonoPatcher", S3pp => "Sims 3 Performance Patch", RestartHost => "Restart host", _ => "Sims 3 Modernization",
    };
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
    // Sims 3 Modernization's channel the player chose: "experimental", or null for stable (left out of the file when null)
    [System.Text.Json.Serialization.JsonIgnore(Condition = System.Text.Json.Serialization.JsonIgnoreCondition.WhenWritingNull)]
    public string? ModernizationChannel { get; set; }
    [System.Text.Json.Serialization.JsonIgnore] public string Bin { get; private set; } = "";

    public static string DirOf(string bin) => System.IO.Path.Combine(bin, "Apex Radiance backup");
    string FilePath => System.IO.Path.Combine(DirOf(Bin), "setup.json");

    public static Manifest Load(string bin)
    {
        var p = System.IO.Path.Combine(DirOf(bin), "setup.json");
        Manifest m;
        try { m = File.Exists(p) ? JsonSerializer.Deserialize(File.ReadAllText(p), SetupJson.Default.Manifest) ?? new() : new(); }
        catch (JsonException e)
        {
            // a damaged setup.json (a crash mid-write before Setup 1.2.4): kept aside as setup.json.corrupt (not in a dry run), and
            // a record rebuilt from the backups so removing still puts the player's files back
            Engine.Log(new InvalidDataException("setup.json could not be read; kept as setup.json.corrupt", e));
            if (Environment.GetEnvironmentVariable("APEX_SETUP_DRY_RUN") != "1")
                try { File.Move(p, p + ".corrupt", true); } catch (Exception x) when (x is IOException or UnauthorizedAccessException) { }
            m = Rebuilt(bin);
            // saved at once (not in a dry run): the next load reads it, and the backups stay linked
            if (Environment.GetEnvironmentVariable("APEX_SETUP_DRY_RUN") != "1")
            {
                m.Bin = bin;
                try { m.Save(); } catch (Exception x) when (x is IOException or UnauthorizedAccessException) { Engine.Log(x); }
            }
        }
        // (a damaged setup.json set aside with nothing saved since, as in a dry run: rebuilt again)
        if (!File.Exists(p) && File.Exists(p + ".corrupt") && m.Files.Count == 0) m = Rebuilt(bin);
        m.Bin = bin;
        m.MoveTo(bin);
        return m;
    }

    // The record a damaged setup.json lost, from the backup folder: each "<path with _ for \>.bak" belongs to the file at that
    // path, for the component that installs that name (d3d9.dll: the bridge when Game\Bin\.trex is there, else DXVK);
    // Options.ini.bak is the copy of Options.ini. A backup no component installs stays in the folder, unmatched.
    public static Manifest Rebuilt(string bin)
    {
        var m = new Manifest();
        var dir = DirOf(bin);
        if (!Directory.Exists(dir)) return m;
        foreach (var bak in Directory.EnumerateFiles(dir, "*.bak"))
        {
            var name = System.IO.Path.GetFileName(bak)[..^4];
            if (name.Equals("Options.ini", StringComparison.OrdinalIgnoreCase)) { m.OptionsBackup = bak; continue; }
            var rel = name.Replace('_', '\\');
            if (!File.Exists(System.IO.Path.Combine(bin, rel))) rel = name;
            var c = rel.Equals("d3d9.dll", StringComparison.OrdinalIgnoreCase)
                ? (Directory.Exists(System.IO.Path.Combine(bin, ".trex")) ? Components.Bridge : Components.Dxvk)
                : Engine.Known.FirstOrDefault(k => k.Value.Contains(rel, StringComparer.OrdinalIgnoreCase)).Key;
            if (c is null) continue;
            m.Files.Add(new ManifestFile { Component = c, Path = System.IO.Path.Combine(bin, rel), Backup = bak });
            m.Versions.TryAdd(c, "?");
        }
        return m;
    }

    // A game folder copied or moved keeps the old one's setup.json, whose paths are absolute: they are moved to this Bin, or
    // removing or repairing here would change the files of the other folder.
    void MoveTo(string bin)
    {
        if (Files.FirstOrDefault(f => System.IO.Path.GetFileName(f.Path).Equals("ApexRadiance.asi", StringComparison.OrdinalIgnoreCase)) is not { } apex) return;
        var old = System.IO.Path.TrimEndingDirectorySeparator(System.IO.Path.GetDirectoryName(apex.Path)!) + System.IO.Path.DirectorySeparatorChar;
        var now = System.IO.Path.TrimEndingDirectorySeparator(System.IO.Path.GetFullPath(bin)) + System.IO.Path.DirectorySeparatorChar;
        if (old.Equals(now, StringComparison.OrdinalIgnoreCase)) return;
        string? Move(string? path) => path is not null && path.StartsWith(old, StringComparison.OrdinalIgnoreCase) ? now + path[old.Length..] : path;
        foreach (var f in Files) { f.Path = Move(f.Path)!; f.Backup = Move(f.Backup); }
        OptionsBackup = Move(OptionsBackup);
    }

    public void Save()
    {
        Fs.CreateDir(DirOf(Bin));
        Fs.Write(FilePath, JsonSerializer.Serialize(this, SetupJson.Default.Manifest));
    }
}

// Where the files of a component come from: (source file, destination) pairs and the version.
public interface ISource
{
    Task<(List<(string Src, string Dest)> Files, string Version)> FetchAsync(string component, Game g, string docs, CancellationToken ct);
    // Sims 3 Modernization from its experimental channel (-exp. prereleases), stable when off
    bool ModernizationExperimental => false;
}

public sealed class GitHubSource : ISource
{
    // Sims 3 Performance Patch is published on Nexus Mods only, which needs the player's own account: the player
    // downloads it there and Setup takes the archive from here
    public string? S3ppArchive { get; init; }

    // the releases Setup showed, by repo: every component of a run comes from them, not from a newer one published since
    public IReadOnlyDictionary<string, Release?>? Shown { get; init; }
    public bool ModernizationExperimental { get; init; }
    public Action<string>? Log { get; init; }
    readonly Dictionary<string, Task<Release>> _rel = new();
    Task<Release> Latest(string repo, CancellationToken ct) =>
        _rel.TryGetValue(repo, out var t) ? t : _rel[repo] = Shown?.GetValueOrDefault(repo) is { } r ? Task.FromResult(r) : Releases.LatestAsync(repo, ct);

    public static readonly string TempRoot =Path.Combine(Path.GetTempPath(), "ApexRadianceSetup");

    // The release's *-Manual-install.zip, downloaded and unzipped once per run, or null when the release has none
    readonly Dictionary<string, string?> _manual = new();
    async Task<string?> ManualDirAsync(Release r, CancellationToken ct)
    {
        if (_manual.TryGetValue(r.Tag, out var d)) return d;
        return _manual[r.Tag] = HasManualZip(r) is { } z ? Releases.Unzip(await Releases.DownloadAsync(z, _tmp, ct)) : null;
    }
    public static Asset? HasManualZip(Release r) => r.Assets.FirstOrDefault(a => a.Name.EndsWith("-Manual-install.zip", StringComparison.OrdinalIgnoreCase));
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
                // the ASI as a file of its own (releases up to 3.1.5), else from the manual-install zip (3.1.6 on: the release has
                // only Setup and that zip)
                async Task<string?> FromManual(string name) =>
                    await ManualDirAsync(r, ct) is { } d ? Directory.EnumerateFiles(d, name, SearchOption.AllDirectories).FirstOrDefault() : null;
                var asi = r.Assets.FirstOrDefault(a => a.Name == "ApexRadiance.asi") is { } own ? await Releases.DownloadAsync(own, _tmp, ct)
                    : await FromManual("ApexRadiance.asi") ?? throw new InvalidOperationException($"{r.Repo} {r.Tag}: asset not found");
                files.Add((asi, Path.Combine(bin, "ApexRadiance.asi")));
                return (files, r.Version);
            }
            case Components.RestartHost:
            {
                // ts3wrap.exe from Apex's release: a file of its own in 3.1.3 and 3.1.4, inside the manual-install zip from 3.1.5
                var r = await Latest(Releases.Apex, ct);
                var wrap = r.Assets.FirstOrDefault(a => a.Name == "ts3wrap.exe") is { } own ? await Releases.DownloadAsync(own, _tmp, ct)
                    : await ManualDirAsync(r, ct) is { } d && Directory.EnumerateFiles(d, "ts3wrap.exe", SearchOption.AllDirectories).FirstOrDefault() is { } inZip ? inZip
                    : throw new InvalidOperationException($"{r.Repo} {r.Tag}: asset not found");
                files.Add((wrap, Path.Combine(bin, "ts3wrap.exe")));
                return (files, r.Version);
            }
            case Components.Bridge:
            {
                var r = await Latest(Releases.Apex, ct);
                // its own zip (releases up to 3.1.5), else the "RTX Remix bridge" folder of the manual-install zip (3.1.6 on)
                var dir = r.Assets.FirstOrDefault(a => a.Name == "Apex-Radiance-RTX-Remix-bridge.zip") is { } own
                    ? Releases.Unzip(await Releases.DownloadAsync(own, _tmp, ct))
                    : await ManualDirAsync(r, ct) is { } m && Directory.EnumerateDirectories(m, "RTX Remix bridge", SearchOption.AllDirectories).FirstOrDefault() is { } b ? b
                    : throw new InvalidOperationException($"{r.Repo} {r.Tag}: asset not found");
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
                // the latest release's 32-bit build without PDB, by its name's pattern (Ultimate-ASI-Loader-NoPDB.zip so far)
                var dir = Releases.Unzip(await Releases.DownloadAsync(r.Find(Engine.LoaderZip), _tmp, ct));
                files.Add((Engine.LoaderDll(dir), Path.Combine(bin, "wininet.dll"))); // the 32-bit loader, named as the mods' guides say
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
                // a release can ship without this edition's build (v0.2.11: no ts3native-ea169): the newest recent one with it,
                // its ts3native and ts3fixes.package together. The experimental channel takes the newest -exp. release with the
                // edition's build, else stable (Setup's shown release is already that pick)
                var exp = ModernizationExperimental && !Releases.IsExperimental(r.Tag);
                if (exp || !Engine.HasModernizationBuild(r, g.Edition))
                    r = Engine.PickModernization(r, await Releases.RecentAsync(Releases.Modernization, ct, exp), g.Edition, exp)
                        ?? throw new InvalidOperationException($"Sims 3 Modernization has no build for the {(g.Edition == Edition.EA ? "EA app" : "Steam")} version in its recent releases. Turn it off in Components and install again.");
                Log?.Invoke($"Sims 3 Modernization {r.Tag}: {(Releases.IsExperimental(r.Tag) ? "experimental" : ModernizationExperimental ? "stable (no experimental build for this edition)" : "stable")}");
                var prefix = Engine.ModernizationPrefix(g.Edition);
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
    // Sims 3 Modernization's zip for the game's edition, and a release that has it with its ts3fixes.package
    public static string ModernizationPrefix(Edition e) => e == Edition.EA ? "ts3native-ea169-" : "ts3native-v";
    public static bool HasModernizationBuild(Release r, Edition e) =>
        r.Assets.Any(a => a.Name.StartsWith(ModernizationPrefix(e)) && a.Name.EndsWith(".zip")) && r.Assets.Any(a => a.Name == "ts3fixes.package");

    // The release to install: experimental, the newest -exp. one with this edition's build; else (or none) the latest stable
    // release when it has the build, else the newest recent stable one with it; null when none has. recent null: the listing
    // failed; then experimental is unknown (null: no update and no channel change shown, the install lists again)
    public static Release? PickModernization(Release? latest, IEnumerable<Release>? recent, Edition e, bool experimental)
    {
        if (recent is null)
            return experimental ? null : latest is not null && !Releases.IsExperimental(latest.Tag) && HasModernizationBuild(latest, e) ? latest : null;
        Release? best = null;
        if (experimental)
            foreach (var r in recent)
                if (Releases.IsExperimental(r.Tag) && HasModernizationBuild(r, e) && (best is null || Releases.Newer(r.Version, best.Version))) best = r;
        return best ?? (latest is not null && !Releases.IsExperimental(latest.Tag) && HasModernizationBuild(latest, e) ? latest
            : recent.FirstOrDefault(r => !Releases.IsExperimental(r.Tag) && HasModernizationBuild(r, e)));
    }

    // Files a component may own although Setup did not install them (installed by hand): removed by Remove, never restored.
    internal static readonly Dictionary<string, string[]> Known = new()
    {
        [Components.Apex] = new[] { "ApexRadiance.asi", "ts3wrap.exe" },
        [Components.Bridge] = new[] { "d3d9.dll", "bridge.conf", "dxvk.conf", @".trex\bridge.conf", @".trex\NvRemixBridge.exe", @".trex\d3d9vk_x64.dll", "Apex Radiance RTX Remix bridge.txt" },
        [Components.Dxvk] = new[] { "d3d9.dll" },
        [Components.S3ss] = Loadable("Sims3SettingsSetter.asi"),
        [Components.MonoPatcher] = Loadable("MonoPatcher.asi"),
        [Components.S3pp] = Loadable("Sims3Performance.asi", "Sims3Performance.ini"),
        [Components.Modernization] = Loadable("ts3native.asi", "ts3native.ini", "ts3native.log"),
        [Components.Loader] = Array.Empty<string>(),
        [Components.RestartHost] = new[] { "ts3wrap.exe" },
    };

    // in Game\Bin or the loader's scripts or plugins folder (Scan.FindAsi)
    static string[] Loadable(params string[] names) => names.SelectMany(n => new[] { n, $@"scripts\{n}", $@"plugins\{n}" }).ToArray();

    // An older copy of these is an update, not a foreign file: replaced without a backup.
    static readonly HashSet<string> Updates = new(StringComparer.OrdinalIgnoreCase)
        { "ApexRadiance.asi", "ts3wrap.exe", "Sims3SettingsSetter.asi", "ts3native.asi", "ts3fixes.package", "Sims3Performance.asi", "MonoPatcher.asi" };

    // Checked before any download: what would stop the change (Blocks) and what it will do to the player's own files. Each is
    // an English key with {n} arguments and one action. A change that downloads nothing (a removal, the settings fixes) needs
    // no free space and no note about Documents.
    public sealed record Check(string Key, string[] Args, bool Blocks);
    public static Func<string, long> FreeBytes = dir => new DriveInfo(Path.GetPathRoot(Path.GetFullPath(dir))!).AvailableFreeSpace;
    public const string CloseGameText = "Close The Sims 3, its launcher, the restart host (ts3wrap.exe) and the RTX Remix bridge (NvRemixBridge.exe), then try again.";
    public const string CannotWriteText = "Setup cannot write to {0}. If Windows Security's Controlled folder access is on, allow ApexRadianceSetup.exe there; or move the game out of a protected folder.";
    public static List<Check> Preflight(Game g, State s, string? docs, long downloadBytes, bool replacesD3d9, bool downloads = true)
    {
        var l = new List<Check>();
        if (Games.IsRunning()) l.Add(new(CloseGameText, [], true));
        // Setup always runs elevated: what refuses a write is Controlled folder access (Documents by default) or a protected
        // folder. The probe writes nothing in a dry run.
        if (Environment.GetEnvironmentVariable("APEX_SETUP_DRY_RUN") != "1")
            foreach (var dir in new[] { g.Bin, docs is not null && Directory.Exists(docs) ? docs : null })
            {
                if (dir is null) continue;
                var probe = Path.Combine(dir, ".apex-setup-probe");
                try { File.WriteAllText(probe, ""); File.Delete(probe); }
                catch (Exception e) when (e is IOException or UnauthorizedAccessException) { l.Add(new(CannotWriteText, [dir], true)); }
            }
        if (downloads)
        {
            // the downloads land in %TEMP% and are copied to Game\Bin: twice their size free on each drive
            var need = Math.Max(downloadBytes * 2, 200L << 20);
            foreach (var dir in new[] { g.Bin, Path.GetTempPath() }.DistinctBy(d => Path.GetPathRoot(Path.GetFullPath(d)), StringComparer.OrdinalIgnoreCase))
                try
                {
                    if (FreeBytes(dir) < need)
                        l.Add(new("Setup needs {0} MB free on drive {1}. Free some space, then try again.", [(need >> 20).ToString(), Path.GetPathRoot(Path.GetFullPath(dir))!], true));
                }
                catch (Exception e) when (e is IOException or ArgumentException or UnauthorizedAccessException) { } // (a drive that cannot say: not a no)
            if (docs is null || !Directory.Exists(docs))
                l.Add(new("Setup did not find The Sims 3's folder in Documents. Start the game once, then run Setup again for the settings it writes there.", [], false));
        }
        if (replacesD3d9 && s.Layer == Layer.Other)
            l.Add(new("{0} (d3d9.dll) is set aside in Game\\Bin\\Apex Radiance backup; removing the renderer in Repair or remove puts it back.", [ForeignD3d9(g.Bin)], false));
        if (Scan.Loaders(g.Bin) is { Count: > 1 } loaders)
            l.Add(new("Two ASI loaders ({0}): Setup keeps {1} and sets the others aside in Game\\Bin\\Apex Radiance backup.", [string.Join(", ", loaders), loaders[0]], false));
        return l;
    }

    // What an install downloads, for the free space check: the assets FetchAsync takes for each component (the manual-install
    // zip once for Apex, the bridge and the restart host), else the release's largest asset; the Performance Patch comes from
    // the player's own download.
    public static long DownloadEstimate(IEnumerable<string> comps, Func<string, Release?> release)
    {
        var picked = new Dictionary<string, long>(StringComparer.OrdinalIgnoreCase);
        bool Is(Asset a, string end) => a.Name.EndsWith(end, StringComparison.OrdinalIgnoreCase);
        foreach (var c in comps.Distinct())
        {
            if (c == Components.S3pp || release(c) is not { } r) continue;
            var a = (c is Components.Apex or Components.Bridge or Components.RestartHost && GitHubSource.HasManualZip(r) is { } z ? new[] { z }
                : c switch
                {
                    Components.Apex => r.Assets.Where(x => Is(x, ".asi")),
                    Components.RestartHost => r.Assets.Where(x => x.Name.Equals("ts3wrap.exe", StringComparison.OrdinalIgnoreCase)),
                    Components.Bridge => r.Assets.Where(x => x.Name.Contains("bridge", StringComparison.OrdinalIgnoreCase) && Is(x, ".zip")),
                    Components.Dxvk => r.Assets.Where(x => x.Name.StartsWith("dxvk-") && Is(x, ".tar.gz") && !x.Name.Contains("native")).Take(1),
                    Components.MonoPatcher => r.Assets.Where(x => Is(x, ".zip")).Take(1),
                    Components.Loader => r.Assets.Where(x => LoaderZip(x.Name)).Take(1),
                    Components.S3ss => r.Assets.Where(x => x.Name == "Sims3SettingsSetter.asi"),
                    Components.Modernization => r.Assets.Where(x => Is(x, ".zip")).OrderByDescending(x => x.Size).Take(1)
                        .Concat(r.Assets.Where(x => x.Name == "ts3fixes.package")),
                    _ => [],
                }).ToList();
            if (a.Count == 0) a = r.Assets.OrderByDescending(x => x.Size).Take(1).ToList();
            foreach (var x in a) picked[r.Repo + "/" + x.Name] = x.Size;
        }
        return picked.Values.Sum();
    }

    // a d3d9.dll that is neither DXVK nor the bridge, by the name its file gives (ReShade, GShade, another wrapper)
    public static string ForeignD3d9(string bin)
    {
        var f = Path.Combine(bin, "d3d9.dll");
        try
        {
            var v = System.Diagnostics.FileVersionInfo.GetVersionInfo(f);
            return new[] { v.ProductName, v.FileDescription }.FirstOrDefault(x => !string.IsNullOrWhiteSpace(x))?.Trim() ?? "d3d9.dll";
        }
        catch (FileNotFoundException) { return "d3d9.dll"; }
    }

    // a file a scan removed, blocked (ERROR_VIRUS_INFECTED, ERROR_VIRUS_DELETED) or emptied as Setup downloaded or placed it: the
    // player's words, with the file names and the folder it was in (%TEMP%\ApexRadianceSetup for a download, Game\Bin for a copy)
    public static SetupException Antivirus(string? file, string folder) =>
        new("Your antivirus removed or blocked {0}; add an exception for {1} and run Setup again.",
            Path.GetFileName(file ?? "") is { Length: > 0 } n && !n.Contains(", ") ? n : file ?? "", folder);
    // a download cut off (the connection dropped mid-file): the network, not a file in use
    public static bool IsNetwork(Exception e) =>
        e is HttpRequestException or TaskCanceledException or System.Net.Http.HttpIOException || e is IOException { InnerException: System.Net.Sockets.SocketException };
    public static bool IsVirusBlock(Exception e) => e is IOException && (e.HResult & 0xFFFF) is 0xE1 or 0xE2 && (e.HResult >> 16 & 0x1FFF) == 7;
    public static List<string> Missing(IReadOnlyDictionary<string, long> sizes) =>
        sizes.Where(p => !File.Exists(p.Key) || new FileInfo(p.Key).Length != p.Value).Select(p => Path.GetFileName(p.Key)).ToList();

    public static bool LoaderZip(string name) =>
        name.Contains("NoPDB", StringComparison.OrdinalIgnoreCase) && name.EndsWith(".zip", StringComparison.OrdinalIgnoreCase)
        && !name.Contains("x64", StringComparison.OrdinalIgnoreCase) && !name.Contains("arm", StringComparison.OrdinalIgnoreCase);

    // the loader in its zip: a 32-bit dinput8.dll, else the one 32-bit dll; an error names what the zip held
    public static string LoaderDll(string dir)
    {
        var x86 = Directory.EnumerateFiles(dir, "*.dll", SearchOption.AllDirectories).Where(Is32Bit).ToList();
        if (x86.FirstOrDefault(f => Path.GetFileName(f).Equals("dinput8.dll", StringComparison.OrdinalIgnoreCase)) is { } d) return d;
        if (x86.Count == 1) return x86[0];
        var found = Directory.EnumerateFiles(dir, "*", SearchOption.AllDirectories).Select(f => Path.GetRelativePath(dir, f)).ToList();
        throw new InvalidDataException("Ultimate ASI Loader: no single 32-bit loader in the zip (found: " + (found.Count == 0 ? "nothing" : string.Join(", ", found)) + ")");
    }

    // PE machine IMAGE_FILE_MACHINE_I386 (0x14C)
    public static bool Is32Bit(string file)
    {
        try
        {
            using var r = new BinaryReader(File.OpenRead(file));
            if (r.BaseStream.Length < 0x40 || r.ReadUInt16() != 0x5A4D) return false;
            r.BaseStream.Position = 0x3C;
            var pe = r.ReadInt32();
            if (pe < 0 || pe + 6 > r.BaseStream.Length) return false;
            r.BaseStream.Position = pe;
            return r.ReadUInt32() == 0x4550 && r.ReadUInt16() == 0x14C;
        }
        catch (IOException) { return false; }
    }

    // each file Setup has just copied into place (the tests use it to stand for an antivirus taking it)
    public static Action<string> Placed = _ => { };

    // where Setup's log goes (the app sets it; tests and the engine run without one)
    public static Action<Exception> Log = _ => { };

    // APEX_SETUP_DRY_RUN=1: Setup shows what it finds and plans but writes nothing (a check on a player's real game)
    public static void CheckWritable()
    {
        if (Environment.GetEnvironmentVariable("APEX_SETUP_DRY_RUN") == "1") throw new InvalidOperationException("Dry run: Setup changes nothing.");
        if (Games.IsRunning()) throw new SetupException(CloseGameText);
    }

    public static string OptionsIni(string docs) => Path.Combine(docs, "Options.ini");

    // A first install on the game's own D3D9 takes the RTX Remix bridge, DXVK when the card or driver cannot run it;
    // an installed renderer stays (a player on DXVK is not switched).
    public static Layer DefaultLayer(State s, bool bridgeBlocked) =>
        s.ApexVersion is null && s.Layer == Layer.Native ? (bridgeBlocked ? Layer.Dxvk : Layer.Bridge) : s.Layer;

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
        if (list.Contains(Components.RestartHost) && RestartHostWaits(docs)) throw new InvalidOperationException(RestartHostWaitsText);
        // the restart host is started by Apex Radiance: never installed on its own
        if (list.Contains(Components.RestartHost) && !list.Contains(Components.Apex) && !File.Exists(Path.Combine(g.Bin, "ApexRadiance.asi")))
            throw new InvalidOperationException("Install Apex Radiance first: it starts the restart host");
        using var point = list.Count == 0 ? RestorePoints.Begin(g.Bin, "Fix incompatible settings")
            : RestorePoints.Begin(g.Bin, "Install {0}", string.Join(", ", list.Select(Components.Name)));
        // everything is downloaded before any file is placed: a network or checksum failure changes nothing
        var fetched = new List<(List<(string Src, string Dest)> Files, string Version)>();
        for (int i = 0; i < list.Count; i++)
        {
            p?.Report(new(0.5 * i / list.Count, "Downloading {0}", list[i]));
            try { fetched.Add(await src.FetchAsync(list[i], g, docs, ct)); }
            // (downloaded, then gone or blocked: a scan took it; the Performance Patch archive is the player's own file)
            catch (FileNotFoundException e) when (list[i] == Components.S3pp)
            { throw new SetupException("Setup could not find {0}. Download the Sims 3 Performance Patch again, or choose the downloaded file again.", Path.GetFileName(e.FileName ?? "")); }
            catch (FileNotFoundException e) { throw Antivirus(e.FileName, GitHubSource.TempRoot); }
            catch (IOException e) when (IsVirusBlock(e)) { throw Antivirus(Components.Name(list[i]), GitHubSource.TempRoot); }
        }
        if (list.Count > 0) CheckWritable(); // the downloads can take minutes: the game may have started since
        for (int i = 0; i < list.Count; i++)
        {
            var c = list[i];
            var (files, version) = fetched[i];
            // a mod the player keeps in the loader's scripts or plugins folder is updated there, beside its .ini
            var asi = c == Components.Apex ? null : Known[c].FirstOrDefault(n => n.EndsWith(".asi"));
            var home = asi is null ? g.Bin : Path.GetDirectoryName(Scan.FindAsi(g.Bin, asi) ?? Path.Combine(g.Bin, asi))!;
            files = files.Select(f => Same(Path.GetDirectoryName(f.Dest)!, g.Bin) ? (f.Src, Path.Combine(home, Path.GetFileName(f.Dest))) : f).ToList();
            p?.Report(new(0.5 + 0.48 * i / list.Count, "Copying {0}", c));
            try
            {
                if (c is Components.Bridge or Components.Dxvk && s.Layer is Layer.Native or Layer.Other && m.OptionsBackup is null && File.Exists(OptionsIni(docs)))
                {
                    m.OptionsBackup = Path.Combine(Manifest.DirOf(g.Bin), "Options.ini.bak");
                    Fs.CreateDir(Manifest.DirOf(g.Bin));
                    Fs.Copy(OptionsIni(docs), m.OptionsBackup);
                }
                // a bridge installed by hand is this component's own files, not foreign ones to back up and restore on removal
                var own = c == Components.Bridge && s.Layer == Layer.Bridge;
                // Sims 3 Modernization moved between stable and experimental: that channel's ts3native.ini replaces the player's
                var channelLeft = c == Components.Modernization && m.Versions.GetValueOrDefault(c) is { Length: > 0 } was
                    && Releases.IsExperimental(was) != Releases.IsExperimental(version) ? Releases.IsExperimental(was) ? "experimental" : "stable" : null;
                // each file's size before it is placed: afterwards every file must be there whole, else an antivirus took it
                var sizes = new Dictionary<string, long>(StringComparer.OrdinalIgnoreCase);
                foreach (var (from, to) in files)
                {
                    try
                    {
                        // (the player's ts3native.ini is kept as it is, except on a channel change: no size to check)
                        if (channelLeft is not null || !(Path.GetFileName(to).Equals("ts3native.ini", StringComparison.OrdinalIgnoreCase) && File.Exists(to))) sizes[to] = new FileInfo(from).Length;
                        Place(m, c, from, to, own, channelLeft);
                    }
                    catch (FileNotFoundException e) { throw Antivirus(e.FileName ?? from, File.Exists(from) ? g.Bin : GitHubSource.TempRoot); }
                    catch (IOException e) when (IsVirusBlock(e)) { throw Antivirus(Path.GetFileName(to), g.Bin); }
                }
                if (Missing(sizes) is { Count: > 0 } gone) throw Antivirus(string.Join(", ", gone), g.Bin);
                if (c == Components.Modernization) m.ModernizationChannel = src.ModernizationExperimental ? "experimental" : null;
                if (c == Components.Bridge)
                    foreach (var conf in new[] { Path.Combine(g.Bin, "bridge.conf"), Path.Combine(g.Bin, ".trex", "bridge.conf") })
                        SetTargetProcess(conf, g.Exe);
                // the log of a build that skipped itself would still say "wrong build" until the game runs
                if (c == Components.Modernization) { EnsureResourceCfg(docs); Fs.Delete(Path.Combine(home, "ts3native.log")); }
                if (c == Components.Apex) RemoveLeftovers(m, g.Bin);
                // up to Setup 1.1.4 ts3wrap.exe came with Apex and stayed off: removed unless the restart host is on
                if (c == Components.Apex && !list.Contains(Components.RestartHost) && !s.RestartHost)
                    foreach (var f in m.Files.Where(f => f.Component == c && Path.GetFileName(f.Path).Equals("ts3wrap.exe", StringComparison.OrdinalIgnoreCase)).ToList())
                    {
                        Fs.Delete(f.Path);
                        m.Files.Remove(f);
                    }
                if (c == Components.RestartHost) SetRestartHost(docs, true);
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
        using var point = RestorePoints.Begin(g.Bin, "Remove {0}", Components.Name(c));
        // the restart host is Apex's: it goes first, so its setting is turned off too
        if (c == Components.Apex && (Manifest.Load(g.Bin).Versions.ContainsKey(Components.RestartHost) || Scan.Run(g, docs).RestartHost))
            Remove(g, Components.RestartHost, docs);
        var m = Manifest.Load(g.Bin);
        // (a ts3wrap.exe an older Setup installed with Apex is the restart host's)
        if (c == Components.RestartHost)
            foreach (var f in m.Files.Where(f => Path.GetFileName(f.Path).Equals("ts3wrap.exe", StringComparison.OrdinalIgnoreCase))) f.Component = c;
        var layerBefore = Scan.Run(g, docs).Layer;
        // hand-installed files first, so a backup restored below is never taken for one
        foreach (var rel in Known[c])
        {
            var p = Path.Combine(g.Bin, rel);
            // only the d3d9.dll of the layer being removed
            if (rel == "d3d9.dll" && layerBefore != (c == Components.Dxvk ? Layer.Dxvk : Layer.Bridge)) continue;
            if (File.Exists(p) && !m.Files.Any(f => Same(f.Path, p))) Fs.Delete(p);
        }
        foreach (var f in m.Files.Where(f => f.Component == c).ToList())
        {
            Fs.Delete(f.Path);
            if (f.Backup is not null && File.Exists(f.Backup)) Fs.Move(f.Backup, f.Path);
            m.Files.Remove(f);
        }
        if (c == Components.MonoPatcher && Directory.Exists(Path.Combine(Scan.Packages(docs), "MonoPatcher")))
            foreach (var f in Directory.EnumerateFiles(Path.Combine(Scan.Packages(docs), "MonoPatcher"), "ld_MonoPatcher*.package")) Fs.Delete(f);
        if (c == Components.Modernization)
        {
            var pkg = Path.Combine(Scan.Packages(docs), "ts3fixes.package");
            Fs.Delete(pkg);
        }
        if (c is Components.Bridge or Components.Dxvk)
        {
            foreach (var d in new[] { Path.Combine(g.Bin, ".trex"), Path.Combine(g.Bin, "licenses") })
                if (Directory.Exists(d) && !Directory.EnumerateFileSystemEntries(d).Any()) Fs.DeleteDir(d);
            // the game is back on its own D3D9 (a restored loader or wrapper included): the copy taken then matches the card it sees again
            if (m.OptionsBackup is not null && File.Exists(m.OptionsBackup) && Scan.Run(g, docs).Layer is Layer.Native or Layer.Other)
                Fs.Copy(m.OptionsBackup, OptionsIni(docs));
            if (m.OptionsBackup is not null) Fs.Delete(m.OptionsBackup);
            m.OptionsBackup = null;
        }
        if (c == Components.RestartHost) SetRestartHost(docs, false);
        m.Versions.Remove(c);
        m.Save();
        // after the files are gone (a failed removal leaves frame generation as it was); never fails the removal: the UI logs it and shows it on the row
        if (TurnsFrameGenOff(c)) { var (nv, amd) = FrameGenOff(g); FrameGenOffError = (nv ?? FrameGenOffError.Nv, amd ?? FrameGenOffError.Amd); }
        // the restored d3d9.dll is a loader again: the one Setup added for the bridge would load every mod twice
        if (c is Components.Bridge or Components.Dxvk && m.Versions.ContainsKey(Components.Loader) && Scan.Run(g, docs).LoaderIsD3d9)
            Remove(g, Components.Loader, docs);
    }

    public static void RemoveAll(Game g, string? docs = null)
    {
        // Apex, and what Setup installed; mods the player installed by hand stay (Repair or remove has a row for each)
        var s = Scan.Run(g, docs);
        using var point = RestorePoints.Begin(g.Bin, "Uninstall everything");
        foreach (var c in Components.All)
            // (a component whose copy failed halfway has files but no version)
            if (c == Components.Apex || s.Manifest.Versions.ContainsKey(c) || s.Manifest.Files.Any(f => f.Component == c)) Remove(g, c, docs);
    }

    static void Place(Manifest m, string c, string from, string to, bool own = false, string? channelLeft = null)
    {
        var owned = m.Files.FirstOrDefault(f => Same(f.Path, to));
        // the player's ts3native.ini holds their feature switches: kept, only a missing one is written; on a channel change
        // it is copied next to itself as the left channel's copy (one per channel, the last one kept), and the channel's own
        // ini written
        if (Path.GetFileName(to).Equals("ts3native.ini", StringComparison.OrdinalIgnoreCase) && File.Exists(to))
        {
            if (channelLeft is not null)
            {
                Fs.Copy(to, $"{to}.{channelLeft}.bak");
                Fs.Copy(from, to);
            }
            if (owned is null) m.Files.Add(new ManifestFile { Component = c, Path = to });
            else owned.Component = c;
            return;
        }
        string? backup = owned?.Backup;
        if (owned is null && File.Exists(to) && !Updates.Contains(Path.GetFileName(to))
            // (the player's .conf edits are kept as a backup)
            && !(own && !to.EndsWith(".conf", StringComparison.OrdinalIgnoreCase) && Known[c].Contains(Path.GetRelativePath(m.Bin, to), StringComparer.OrdinalIgnoreCase)))
        {
            var rel = to.StartsWith(m.Bin, StringComparison.OrdinalIgnoreCase) ? Path.GetRelativePath(m.Bin, to) : Path.GetFileName(to);
            backup = Path.Combine(Manifest.DirOf(m.Bin), rel.Replace('\\', '_') + ".bak");
            // a backup already there (one a damaged setup.json lost) is never overwritten: the next free name
            for (var k = 2; File.Exists(backup); k++) backup = Path.Combine(Manifest.DirOf(m.Bin), rel.Replace('\\', '_') + "-" + k + ".bak");
            Fs.CreateDir(Manifest.DirOf(m.Bin));
            Fs.Move(to, backup);
        }
        // recorded before the copy: a copy that fails still leaves the backup linked to its file (setup.json is saved in a finally)
        if (owned is null) m.Files.Add(new ManifestFile { Component = c, Path = to, Backup = backup });
        else owned.Component = c;
        Fs.CreateDir(Path.GetDirectoryName(to)!);
        Fs.Copy(from, to);
        Placed(to);
    }

    // An older combined build or a second copy of the ASI would load Apex twice.
    static void RemoveLeftovers(Manifest m, string bin)
    {
        foreach (var p in new[] { Path.Combine(bin, "S3SSApex.asi"), Path.Combine(bin, "scripts", "ApexRadiance.asi"), Path.Combine(bin, "plugins", "ApexRadiance.asi") })
            if (File.Exists(p))
            {
                Fs.CreateDir(Manifest.DirOf(bin));
                Fs.Move(p, Path.Combine(Manifest.DirOf(bin), Path.GetRelativePath(bin, p).Replace('\\', '_') + ".bak"));
            }
    }

    public static void SetTargetProcess(string conf, string exe)
    {
        if (!File.Exists(conf)) return;
        var text = File.ReadAllText(conf);
        Fs.Write(conf, Regex.Replace(text, @"(?m)^(\s*client\.targetProcess\s*=\s*)\S+", "${1}" + exe));
    }

    // sims3-modernization's player guide: Mods\Resource.cfg, created only when missing (kept on removal: other mods use it)
    public static void EnsureResourceCfg(string docs)
    {
        var cfg = Path.Combine(docs, "Mods", "Resource.cfg");
        if (File.Exists(cfg)) return;
        Fs.CreateDir(Path.GetDirectoryName(cfg)!);
        Fs.Write(cfg, ResourceCfg);
    }
    public const string ResourceCfg = "Priority 500\r\nPackedFile Packages/*.package\r\nPackedFile Packages/*/*.package\r\nPackedFile Packages/*/*/*.package\r\n";

    static bool Same(string a, string b) => string.Equals(Path.GetFullPath(a), Path.GetFullPath(b), StringComparison.OrdinalIgnoreCase);

    // ---------- the restart host's switch: [ui] restart_host in Apex's own settings ----------

    public static string ApexToml(string docs) => Path.Combine(docs, "Apex Radiance", "ApexRadiance.toml");

    // What Setup writes when the game has not made the file yet. [ui] alone would read as an older install and skip Apex's
    // welcome page (apex_config.cpp: a [ui] without start_profile_done counts as true).
    public const string NewApexToml = "[ui]\nrestart_host = true\nstart_profile_done = false\n";

    // Apex copies its older settings (S3SS\Apex\Apex.toml, or its tables in S3SS.toml) only while ApexRadiance.toml does not
    // exist (apex_config.cpp DoMigrate): Setup does not create the file before that copy has run.
    public static bool RestartHostWaits(string docs)
    {
        if (File.Exists(ApexToml(docs))) return false;
        if (File.Exists(Path.Combine(docs, "S3SS", "Apex", "Apex.toml"))) return true;
        var s3ss = Path.Combine(docs, "S3SS", "S3SS.toml");
        try
        {
            return File.Exists(s3ss) && Regex.IsMatch(File.ReadAllText(s3ss),
                @"(?m)^\s*\[\s*(qol\s*\.\s*(picture|hdr)|patches\s*\.\s*(NightTerrainRelight|EdgeSmoothing|DepthBlur|FrameCapture))\s*\]");
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return true; } // (unknown: the safe answer)
    }
    public const string RestartHostWaitsText = "Start the game once with Apex Radiance, then turn on the restart host here.";

    // on: ts3wrap.exe next to the ASI and [ui] restart_host = true, what Apex checks before it starts the host
    public static bool RestartHostOn(string bin, string docs)
    {
        try { return File.Exists(Path.Combine(bin, "ts3wrap.exe")) && File.Exists(ApexToml(docs)) && RestartHostKey(File.ReadAllText(ApexToml(docs))) == true; }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return false; }
    }

    // On writes the key (and the file when the game has not made it yet); off changes only a key that is there. The restore
    // point keeps the key's value: undoing it puts that value back and keeps what the game saved in the file since.
    public static void SetRestartHost(string docs, bool on)
    {
        var f = ApexToml(docs);
        if (!File.Exists(f))
        {
            if (!on) return;
            Fs.CreateDir(Path.GetDirectoryName(f)!);
            Fs.WriteKey(f, NewApexToml, null);
            return;
        }
        var text = File.ReadAllText(f);
        var was = RestartHostKey(text);
        if (was == on || !on && was is null) return;
        Fs.WriteKey(f, WithRestartHost(text, on), was);
    }

    static readonly Regex UiHeader = new(@"^\s*\[\s*ui\s*\]\s*(#.*)?\r?$");
    // a table header line ([name], [a.b], [a."b c"], [[array]]); a value line never matches (a key and = come first)
    static readonly Regex AnyHeader = new(@"^\s*\[\[?\s*[A-Za-z0-9_\-]+(\s*\.\s*([A-Za-z0-9_\-]+|""[^""]*""))*\s*\]\]?\s*(#.*)?\r?$");
    static readonly Regex RestartKey = new(@"^(\s*restart_host\s*=\s*)(true|false)\b");

    // the lines of the text, the [ui] header's index (-1 when there is none) and the index of the next table's header
    static (string[] Lines, int At, int End) UiTable(string text)
    {
        var lines = text.Split('\n');
        var at = Array.FindIndex(lines, l => UiHeader.IsMatch(l));
        var end = at < 0 ? lines.Length : Array.FindIndex(lines, at + 1, l => AnyHeader.IsMatch(l)) is var n and >= 0 ? n : lines.Length;
        return (lines, at, end);
    }

    // [screen] fps_limit, for frame generation (Smooth Motion, AFMF): a limit the player chose stays as it is; only no limit (the
    // key absent, Apex's default, or 0) becomes `fps`. Returns the text and the value before (null when absent).
    static readonly Regex ScreenHeader = new(@"^\s*\[\s*screen\s*\]\s*(#.*)?\r?$");
    static readonly Regex FpsKey = new(@"^(\s*fps_limit\s*=\s*)(\d+)");
    public const int FrameGenFps = 60;

    // Apex's own frame limit choices (features/screen_control.cpp: 0 = off, 30, 60, 120, 144, 165, 240), with 75 and 90 for frame
    // generation's 60 to 90; a custom value is clamped as Apex reads it: 0 or below is off, else 20 to 1000
    public static readonly int[] FpsChoices = { 0, 30, 60, 75, 90, 120, 144, 165, 240 };
    // a pending change: the choice when it differs from the state now, else none (switching back drops the change)
    public static bool? PendingSwitch(bool want, bool now) => want == now ? null : want;
    public static int? PendingFps(int want, int now) => ClampFps(want) == ClampFps(now) ? null : ClampFps(want);
    public static bool FpsRecommended(int fps) => fps is >= 60 and <= 90;
    public static int ClampFps(int fps) => fps <= 0 ? 0 : Math.Clamp(fps, 20, 1000);

    // The frame limit picker: [screen] fps_limit = fps at once, in a restore point that puts back only the key; nothing before
    // the game made ApexRadiance.toml, while it runs, or in a dry run. False when nothing changed.
    public static bool SetFpsLimit(Game g, int fps, string? docs = null)
    {
        var f = ApexToml(docs ?? Scan.DocumentsFor(g));
        if (!File.Exists(f)) return false;
        CheckWritable();
        fps = ClampFps(fps);
        var text = File.ReadAllText(f);
        var was = FpsLimitKey(text);
        if (was == fps || DottedScreen(text)) return false;
        using var point = RestorePoints.Begin(g.Bin, "Apex frame limit");
        Fs.WriteFpsKey(f, WithFpsLimit(text, fps), was, fps);
        return true;
    }
    public static (string Text, int? Was) WithFpsLimitIfNone(string text, int fps = FrameGenFps) =>
        DottedScreen(text) || FpsLimitKey(text) is > 0 or -1 ? (text, FpsLimitKey(text)) : (WithFpsLimit(text, fps), FpsLimitKey(text));

    // [screen] written as dotted keys (screen.fps_limit = 30) or an inline table (screen = { ... }): Setup writes nothing then
    static readonly Regex DottedScreenKey = new(@"^\s*screen\s*(\.|=)");
    static bool DottedScreen(string text) => text.Split('\n').Any(l => DottedScreenKey.IsMatch(l));

    static (string[] Lines, int At, int End) ScreenTable(string text)
    {
        var lines = text.Split('\n');
        var at = Array.FindIndex(lines, l => ScreenHeader.IsMatch(l));
        var end = at < 0 ? lines.Length : Array.FindIndex(lines, at + 1, l => AnyHeader.IsMatch(l)) is var n and >= 0 ? n : lines.Length;
        return (lines, at, end);
    }

    // [screen] fps_limit in the text; null when the table or the key is missing
    public static int? FpsLimitKey(string text)
    {
        var (lines, at, end) = ScreenTable(text);
        for (int i = at + 1; at >= 0 && i < end; i++)
            // a value that does not parse (too big) counts as a limit the player set: -1, left alone
            if (FpsKey.Match(lines[i]) is { Success: true } m) return int.TryParse(m.Groups[2].Value, out var v) ? v : -1;
        return null;
    }

    // the text with [screen] fps_limit = fps, every other line as it was (the value in place, else the key added after the
    // table's last line, else [screen] added at the end)
    public static string WithFpsLimit(string text, int fps)
    {
        var cr = text.Contains("\r\n") ? "\r" : "";
        if (text.Length > 0 && !text.EndsWith('\n')) text += cr + "\n"; // (no final newline: one first, so every line ends alike)
        var (lines, at, end) = ScreenTable(text);
        var line = "fps_limit = " + fps;
        if (at < 0)
            return text + (text.Length == 0 || text.EndsWith('\n') ? "" : cr + "\n") + (text.Length == 0 ? "" : cr + "\n") + $"[screen]{cr}\n{line}{cr}\n";
        for (int i = at + 1; i < end; i++)
            if (FpsKey.IsMatch(lines[i]))
            {
                lines[i] = FpsKey.Replace(lines[i], "${1}" + fps, 1);
                return string.Join('\n', lines);
            }
        var last = end - 1;
        while (last > at && lines[last].Trim().Length == 0) last--;
        var list = lines.ToList();
        list.Insert(last + 1, line + cr);
        return string.Join('\n', list);
    }

    // the text without [screen] fps_limit (undo of a key Setup added)
    public static string WithoutFpsLimit(string text)
    {
        var (lines, at, end) = ScreenTable(text);
        var i = at < 0 ? -1 : Array.FindIndex(lines, at + 1, end - at - 1, l => FpsKey.IsMatch(l));
        return i < 0 ? text : string.Join('\n', lines.Where((_, n) => n != i));
    }

    // Frame generation turned on: Apex's frame limit to 60 when there is none, in a restore point that puts back only the key.
    // Nothing is written while the game has not made ApexRadiance.toml (Apex still copies older settings then) or runs.
    public static bool SetFpsLimitIfNone(Game g, string feature, string? docs = null)
    {
        var f = ApexToml(docs ?? Scan.DocumentsFor(g));
        if (!File.Exists(f)) return false;
        CheckWritable();
        var text = File.ReadAllText(f);
        var (next, was) = WithFpsLimitIfNone(text);
        if (next == text) return false;
        using var point = RestorePoints.Begin(g.Bin, "Turn on {0}", feature);
        Fs.WriteFpsKey(f, next, was, FrameGenFps);
        return true;
    }

    // Frame generation set by Setup in the driver follows the bridge and Apex: removing either (alone, by a renderer change or with
    // Uninstall everything) turns it off. A hook, so tests never reach a real driver; it returns why it could not, never throws.
    // Which frame generation row shows: the one that applies. A usable NVIDIA card shows Smooth Motion; AFMF shows only where ADLX
    // reports it supported (an AMD integrated GPU that cannot run it shows nothing); with neither usable, the NVIDIA row explains
    // why when an NVIDIA card is there, else the AMD row does on an AMD-only PC. An NVAPI error shows on the NVIDIA row.
    public static (bool Nvidia, bool Amd) FrameGenRows(SmoothMotion.Status nv, Afmf.Status amd)
    {
        var nvUsable = nv.On || SmoothMotion.Supported(nv.Card, nv.Driver);
        var amdUsable = amd.On || amd.Supported;
        var nvidia = nv.Error is not null || nv.Card is not null && (nvUsable || !amdUsable);
        return (nvidia, amdUsable || amd.Present && !nvidia);
    }

    // A driver error in plain words for the frame generation rows (the raw call and code go to the log). Setup always runs
    // elevated, so a refusal (NVAPI_INVALID_USER_PRIVILEGE -137, access denied) points at the programs that lock the driver's
    // settings; anything else, a change the driver did not keep included, says it did not accept it and how to do it by hand,
    // turning on or off as the change was.
    public static string FriendlyFrameGenError(string error, bool amd, bool on = true) =>
        error == "Dry run: Setup changes nothing." ? error
        : error.EndsWith(" -137") || error.Contains("denied", StringComparison.OrdinalIgnoreCase) || error.Contains("privilege", StringComparison.OrdinalIgnoreCase)
            ? (amd ? "Windows did not let Setup change the driver settings. Close AMD Software, then try again."
                   : "Windows did not let Setup change the driver settings. Close the NVIDIA App and NVIDIA Profile Inspector, then try again.")
        : amd ? (on ? "The AMD driver did not accept the change. You can turn it on by hand in AMD Software: Gaming > Graphics > AMD Fluid Motion Frames."
                    : "The AMD driver did not accept the change. You can turn it off by hand in AMD Software: Gaming > Graphics > AMD Fluid Motion Frames.")
        : on ? "The NVIDIA driver did not accept the change. You can turn it on by hand with NVIDIA Profile Inspector."
             : "The NVIDIA driver did not accept the change. You can turn it off by hand with NVIDIA Profile Inspector.";

    public static bool TurnsFrameGenOff(string c) => c is Components.Bridge or Components.Apex;

    // The pending frame generation and frame limit changes, in the order Apply runs them: the limit first (frame generation
    // turned on then finds it set). A turn-on is dropped when the same Apply removes the bridge or Apex (the removal turns it off).
    // AutoLimit: the automatic 60 fps after a turn-on, not when the player chose a limit in this Apply, and not for AFMF on a PC
    // with an NVIDIA card (the game may run on that card, where the AMD driver's frame generation does nothing).
    public sealed record PlannedChange(string Kind, bool? On, int? Fps, bool AutoLimit);
    public static List<PlannedChange> PlanFrameGen(int? fps, bool? smooth, bool? afmf, IEnumerable<string> removals, bool nvidiaCard)
    {
        var removing = removals.Any(TurnsFrameGenOff);
        var l = new List<PlannedChange>();
        if (fps is { } f) l.Add(new("fps", null, f, false));
        if (smooth is { } s && !(s && removing)) l.Add(new("nv", s, null, s && fps is null));
        if (afmf is { } a && !(a && removing)) l.Add(new("amd", a, null, a && fps is null && !nvidiaCard));
        return l;
    }

    // A driver change and its check: the setting read back (in a new session) must be what was asked; an "on" that did not take
    // is undone, an "off" that did not take keeps Setup's record (the setting is still Setup's to turn off).
    public static string? VerifyChange(bool on, Func<string?> set, Func<(bool On, string? Error)> read, Action undo, Action? keepRecord = null)
    {
        if (set() is { } err) return err;
        var (now, readError) = read();
        if (readError is null && now == on) return null;
        if (on) undo(); else keepRecord?.Invoke();
        return SmoothMotion.DidNotTake;
    }
    // both drivers are asked, each error kept for its own row (a failed NVIDIA step does not skip AMD's)
    public static Func<Game, (string? Nv, string? Amd)> FrameGenOff = DefaultFrameGenOff;
    static (string? Nv, string? Amd) FrameGenOffError;
    public static (string? Nv, string? Amd) PeekFrameGenOffError() => FrameGenOffError;
    public static (string? Nv, string? Amd) TakeFrameGenOffError() { var e = FrameGenOffError; FrameGenOffError = default; return e; }
    public static (string? Nv, string? Amd) DefaultFrameGenOff(Game g)
    {
        string? nv, amd;
        // (a driver that cannot be read is an error on the row, not a silent skip)
        try { nv = !SmoothMotion.SetupTurnedOn ? null : SmoothMotion.Read() is { Error: { } e } ? e : SmoothMotion.Read() is { On: true } ? SmoothMotion.Set(false) : null; }
        catch (Exception e) { nv = e.Message; }
        // AFMF is system-wide: off only when Setup turned it on (Afmf.Marker), never a setting the player had on
        try { amd = Afmf.OffIfSetupTurnedOn(); }
        catch (Exception e) { amd = e.Message; }
        return (nv, amd);
    }
    // [ui] restart_host in the text of ApexRadiance.toml; null when the table or the key is missing
    public static bool? RestartHostKey(string text)
    {
        var (lines, at, end) = UiTable(text);
        for (int i = at + 1; at >= 0 && i < end; i++)
            if (RestartKey.Match(lines[i]) is { Success: true } m) return m.Groups[2].Value == "true";
        return null;
    }

    // The text with [ui] restart_host = on and every other line as it was: the value replaced in place, else the key added
    // after the table's last line, else [ui] added at the end.
    // shortcut: a [ui] written as dotted keys (ui.x = 1) or an inline table is not seen; Apex writes neither (toml++ tables)
    public static string WithRestartHost(string text, bool on)
    {
        var cr = text.Contains("\r\n") ? "\r" : "";
        var line = "restart_host = " + (on ? "true" : "false");
        var (lines, at, end) = UiTable(text);
        if (at < 0)
            return text + (text.Length == 0 || text.EndsWith('\n') ? "" : cr + "\n") + (text.Length == 0 ? "" : cr + "\n") + $"[ui]{cr}\n{line}{cr}\n";
        for (int i = at + 1; i < end; i++)
            if (RestartKey.IsMatch(lines[i]))
            {
                lines[i] = RestartKey.Replace(lines[i], "${1}" + (on ? "true" : "false"), 1);
                return string.Join('\n', lines);
            }
        var last = end - 1;
        while (last > at && lines[last].Trim().Length == 0) last--;
        var list = lines.ToList();
        list.Insert(last + 1, line + cr);
        return string.Join('\n', list);
    }

    // the text without [ui] restart_host (undo of a key Setup added)
    public static string WithoutRestartHost(string text)
    {
        var (lines, at, end) = UiTable(text);
        var i = at < 0 ? -1 : Array.FindIndex(lines, at + 1, end - at - 1, l => RestartKey.IsMatch(l));
        return i < 0 ? text : string.Join('\n', lines.Where((_, n) => n != i));
    }
}
