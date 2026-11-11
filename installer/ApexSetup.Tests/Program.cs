// Offline check of the install engine on a fake Game\Bin and Documents folder in %TEMP% (no network, no real game).
using ApexSetup;

var root = Path.Combine(Path.GetTempPath(), "ApexSetupTest-" + Guid.NewGuid().ToString("N")[..8]);
var bin = Path.Combine(root, "The Sims 3", "Game", "Bin");
var docs = Path.Combine(root, "Docs");
var payload = Path.Combine(root, "payload");
Directory.CreateDirectory(bin);
Directory.CreateDirectory(docs);
File.WriteAllText(Path.Combine(bin, "TS3.exe"), "exe");
var g = new Game(Path.Combine(root, "The Sims 3"), bin, "TS3.exe", Edition.EA, "1.69.47");
var src = new FakeSource(payload);
Games.IsRunning = () => false; // the fake game never runs
int failures = 0;
void Check(bool ok, string what) { Console.WriteLine((ok ? "ok   " : "FAIL ") + what); if (!ok) failures++; }
string B(string rel) => Path.Combine(bin, rel);
string Read(string p) => File.Exists(p) ? File.ReadAllText(p) : "<missing>";

try
{
    // 1. the game's own D3D9, no loader: Apex + bridge + loader
    File.WriteAllText(Path.Combine(docs, "Options.ini"), "gpu=real");
    var s = Scan.Run(g, docs);
    Check(s.Layer == Layer.Native && s.Loader is null && s.ApexVersion is null, "fresh game scans as native, no loader, no Apex");
    Check(Engine.NeedsLoader(s, true), "a loader is needed");
    await Engine.InstallAsync(g, new[] { Components.Apex, Components.Bridge, Components.Loader }, src, null, docs);
    Check(Read(B("ApexRadiance.asi")) == "apex 3.1.1", "ApexRadiance.asi installed");
    Check(Read(B("wininet.dll")) == "loader", "loader installed as wininet.dll");
    Check(Read(B("bridge.conf")).Contains("client.targetProcess = TS3.exe") && Read(B(@".trex\bridge.conf")).Contains("client.targetProcess = TS3.exe"),
          "both bridge.conf target TS3.exe on the EA app");
    Check(Scan.Run(g, docs).Layer == Layer.Bridge, "scan sees the bridge");
    Check(File.Exists(Path.Combine(Manifest.DirOf(bin), "Options.ini.bak")), "Options.ini copied before the bridge replaced native D3D9");

    // Check installation: the release's own file passes, other bytes of the same version do not; a missing bridge file shows
    {
        var sv = Scan.Run(g, docs) with { ApexVersion = "3.1.1", Loader = "wininet.dll" }; // (the fake loader is no real ASI loader)
        var good = new Release("loinyx/Sims3-ApexRadiance", "v3.1.1", new[] { new Asset("ApexRadiance.asi", "x", Verify.Sha256Of(B("ApexRadiance.asi"))) });
        var other = new Release("loinyx/Sims3-ApexRadiance", "v3.1.1", new[] { new Asset("ApexRadiance.asi", "x", new string('0', 64)) });
        Check(Verify.Files(g, sv, good).Count == 0, "check installation: a sound install has no finding");
        Check(!Verify.TestBuild(g, sv, good) && Verify.TestBuild(g, sv, other), "check installation: other bytes of the release's version are a test build");
        Check(Verify.TestBuild(g, sv with { ApexVersion = "3.1.1-dev.3" }, good) && Verify.TestBuild(g, sv with { ApexVersion = "3.2.0" }, good),
              "check installation: a -dev version and a version newer than the release are test builds");
        Check(!Verify.TestBuild(g, sv with { ApexVersion = "3.1.0" }, other) && Verify.Files(g, sv, other).Count == 0,
              "check installation: an older release is no test build, and a test build is no error");
        File.Move(B(@".trex\NvRemixBridge.exe"), B(@".trex\NvRemixBridge.exe.off"));
        Check(Verify.Files(g, sv, good).Any(x => x.Args.Contains(@".trex\NvRemixBridge.exe")), "check installation: a missing bridge server is found");
        File.Move(B(@".trex\NvRemixBridge.exe.off"), B(@".trex\NvRemixBridge.exe"));
    }

    // 2. the game resets Options.ini under the bridge; removing the bridge brings the copy back
    File.WriteAllText(Path.Combine(docs, "Options.ini"), "gpu=gtx580 defaults");
    Engine.Remove(g, Components.Bridge, docs);
    Check(!File.Exists(B("d3d9.dll")) && !Directory.Exists(B(".trex")) && !File.Exists(B("dxvk.conf")), "bridge files removed");
    Check(Read(Path.Combine(docs, "Options.ini")) == "gpu=real", "Options.ini restored when going back to native D3D9");
    Check(File.Exists(B("ApexRadiance.asi")), "Apex stays when only the bridge is removed");

    // 3. DXVK installed by hand: the bridge backs it up and puts it back
    File.WriteAllText(B("d3d9.dll"), "... DXVK v2.7 ...");
    File.WriteAllText(B("dxvk.conf"), "user dxvk conf");
    Check(Scan.Run(g, docs).Layer == Layer.Dxvk, "scan sees DXVK");
    await Engine.InstallAsync(g, new[] { Components.Bridge }, src, null, docs);
    Check(Read(B("d3d9.dll")).StartsWith("bridge client"), "bridge replaced DXVK");
    await Engine.InstallAsync(g, new[] { Components.Bridge }, src, null, docs); // repair keeps the first backup
    Engine.Remove(g, Components.Bridge, docs);
    Check(Read(B("d3d9.dll")) == "... DXVK v2.7 ..." && Read(B("dxvk.conf")) == "user dxvk conf", "DXVK and its dxvk.conf restored after repair + remove");

    // 4. update over a hand-installed older Apex, a leftover combined build, S3SS and sims3-modernization
    File.WriteAllText(B("ApexRadiance.asi"), "apex 3.0.0");
    var m0 = Manifest.Load(bin); m0.Files.RemoveAll(f => f.Component == Components.Apex); m0.Save(); // as if copied by hand
    File.WriteAllText(B("S3SSApex.asi"), "old");
    Directory.CreateDirectory(Path.Combine(docs, "Mods", "Packages"));
    File.WriteAllText(Path.Combine(docs, "Mods", "Packages", "simler90GameplayCoreMod-UPDATE242-Steam.package"), "pkg");
    var found = Compat.Check(g, Scan.Run(g, docs), docs);
    Check(found.Any(x => x.Args.Contains("S3SSApex.asi") && x.CanFix), "leftover combined build reported with a fix");
    Check(found.Any(x => x.Text.Contains("Resource.cfg") && x.CanFix), "missing Resource.cfg reported with a fix");
    Check(found.Any(x => x.Text.Contains("other edition of the game")), "Steam build of a script mod on the EA app reported");
    await Engine.InstallAsync(g, new[] { Components.Apex, Components.S3ss, Components.Modernization }, src, null, docs);
    Check(Read(B("ApexRadiance.asi")) == "apex 3.1.1" && !File.Exists(Path.Combine(Manifest.DirOf(bin), "ApexRadiance.asi.bak")), "update replaces Apex without a backup");
    Check(!File.Exists(B("S3SSApex.asi")), "leftover combined build moved away");
    var s4 = Scan.Run(g, docs);
    Check(s4.S3ss && s4.Ts3Native && s4.Ts3Fixes && s4.Installed(Components.Modernization) == "0.2.10", "S3SS and sims3-modernization detected with versions");
    Check(File.Exists(Path.Combine(docs, "Mods", "Resource.cfg")), "Resource.cfg created");

    // 5. wrong ts3native build is reported
    File.WriteAllText(B("ts3native.log"), "SKIP all: host is not TS3W.exe at 0x400000");
    Check(Scan.Run(g, docs).Ts3NativeWrongBuild, "wrong ts3native build detected");

    // 6. switch the renderer: bridge -> DXVK (as the UI's Apply does: remove, then install), then DXVK -> the game's own D3D9
    Engine.Remove(g, Components.Bridge, docs); // nothing installed by Setup: a no-op
    File.Delete(B("d3d9.dll"));
    File.WriteAllText(Path.Combine(docs, "Options.ini"), "gpu=real");
    await Engine.InstallAsync(g, new[] { Components.Bridge }, src, null, docs);
    Engine.Remove(g, Components.Bridge, docs);
    await Engine.InstallAsync(g, new[] { Components.Dxvk }, src, null, docs);
    Check(Scan.Run(g, docs).Layer == Layer.Dxvk && Scan.Run(g, docs).Installed(Components.Dxvk) == "3.1.1", "bridge replaced by DXVK");
    File.WriteAllText(Path.Combine(docs, "Options.ini"), "gpu=gtx580 defaults");
    Engine.Remove(g, Components.Dxvk, docs);
    Check(!File.Exists(B("d3d9.dll")) && Read(Path.Combine(docs, "Options.ini")) == "gpu=real", "DXVK removed, Options.ini copy restored");
    File.WriteAllText(B("d3d9.dll"), "... DXVK v2.7 ..."); // back to the user's own DXVK for the last step

    // 7. compatibility fixes keep every other byte
    var ini = "[Patches]\r\nShortWaitSpin = 2 ; note\r\nThreadStackDiet=1\r\n[Tuning]\r\nShortWaitSpinUs = 50\r\n";
    Check(Compat.Set(ini, "Patches", new[] { "ShortWaitSpin", "ThreadStackDiet" }, v => v != "0", "0")
          == "[Patches]\r\nShortWaitSpin = 0 ; note\r\nThreadStackDiet=0\r\n[Tuning]\r\nShortWaitSpinUs = 50\r\n", "Sims3Performance.ini rewrite keeps comments and [Tuning]");
    var toml = "[patches.UncompressedLotTexturesPatch]\nenabled = true\nonlyForLOD0 = true\n[patches.Other]\nenabled = true\n";
    Check(Compat.Set(toml, "patches.UncompressedLotTexturesPatch", new[] { "enabled" }, v => v == "true", "false")
          == "[patches.UncompressedLotTexturesPatch]\nenabled = false\nonlyForLOD0 = true\n[patches.Other]\nenabled = true\n", "S3SS.toml rewrite touches only its section");

    // 8. MonoPatcher in, then out; Smooth Patch's ASI next to S3SS is moved away
    await Engine.InstallAsync(g, new[] { Components.MonoPatcher }, src, null, docs);
    Check(Scan.Run(g, docs).MonoPatcher && File.Exists(Path.Combine(Scan.Packages(docs), "MonoPatcher", "ld_MonoPatcher.package")), "MonoPatcher installed");
    File.WriteAllText(B("TS3Patch.asi"), "smooth");
    await Engine.InstallAsync(g, Array.Empty<string>(), src, null, docs); // nothing to copy: only the fixes run
    Check(!File.Exists(B("TS3Patch.asi")), "Smooth Patch ASI next to S3SS moved away");
    Check(KnownMods.Found(bin, docs).Any(m => m.Name.StartsWith("simler90")), "simler90 detected as a known mod");

    // 9. Sims 3 Performance Patch from the player's download (a zip), next to ts3native: its two thread patches are set to 0
    var dl = Path.Combine(root, "Downloads");
    Directory.CreateDirectory(Path.Combine(dl, "s3pp", "Sims3PerformancePatch"));
    File.WriteAllText(Path.Combine(dl, "s3pp", "Sims3PerformancePatch", "Sims3Performance.asi"), "s3pp");
    File.WriteAllText(Path.Combine(dl, "s3pp", "Sims3PerformancePatch", "Sims3Performance.ini"), "[Patches]\r\nShortWaitSpin=1\r\nThreadStackDiet=1\r\n");
    var zip = Path.Combine(dl, "Sims 3 Performance Patch-235-1-1.zip");
    System.IO.Compression.ZipFile.CreateFromDirectory(Path.Combine(dl, "s3pp"), zip);
    Check(GitHubSource.S3ppFiles(zip) is not null, "S3PP found inside the downloaded zip");
    File.WriteAllText(B("ts3native.asi"), "ts3native");
    var s3ppSrc = new MixedSource(src, zip);
    await Engine.InstallAsync(g, new[] { Components.S3pp }, s3ppSrc, null, docs);
    Check(File.Exists(B("Sims3Performance.asi")) && Read(B("Sims3Performance.ini")).Contains("ShortWaitSpin=0") && Read(B("Sims3Performance.ini")).Contains("ThreadStackDiet=0"),
          "S3PP installed with its clash with Sims 3 Modernization fixed");
    Engine.Remove(g, Components.S3pp, docs);
    Check(!File.Exists(B("Sims3Performance.asi")) && !File.Exists(B("Sims3Performance.ini")), "S3PP removed");

    // 10. uninstall everything
    Engine.RemoveAll(g, docs);
    var left = Directory.EnumerateFiles(bin, "*", SearchOption.AllDirectories).Select(f => Path.GetRelativePath(bin, f)).OrderBy(x => x).ToList();
    Check(!left.Contains("ApexRadiance.asi") && !left.Contains("Sims3SettingsSetter.asi") && !left.Contains("ts3native.asi") && !left.Contains("wininet.dll"),
          "uninstall removes everything Setup added (left: " + string.Join(", ", left) + ")");
    Check(Read(B("d3d9.dll")) == "... DXVK v2.7 ...", "user's DXVK untouched by uninstall");
    Check(!File.Exists(Path.Combine(Scan.Packages(docs), "ts3fixes.package")), "ts3fixes.package removed");
    Check(!File.Exists(B("MonoPatcher.asi")) && !File.Exists(Path.Combine(Scan.Packages(docs), "MonoPatcher", "ld_MonoPatcher.package")), "MonoPatcher removed");

    // 11. a download fails: nothing is placed (the loader is not left beside a d3d9.dll loader)
    try { await Engine.InstallAsync(g, new[] { Components.Loader, Components.Bridge }, new FailingSource(src, Components.Bridge), null, docs); } catch (HttpRequestException) { }
    Check(!File.Exists(B("wininet.dll")) && Read(B("d3d9.dll")) == "... DXVK v2.7 ...", "a failed download changes nothing");

    // 12. a copy fails halfway: what was placed is saved, so Remove puts the player's d3d9.dll back
    File.WriteAllText(B("d3d9.dll"), "reshade");
    File.WriteAllText(Path.Combine(docs, "Options.ini"), "gpu=real");
    Directory.CreateDirectory(B(@".trex\NvRemixBridge.exe")); // (a folder where a file goes: the copy throws)
    try { await Engine.InstallAsync(g, new[] { Components.Bridge }, src, null, docs); } catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
    Check(Manifest.Load(bin).Files.Any(f => f.Path.EndsWith("d3d9.dll") && f.Backup is not null) && !Manifest.Load(bin).Versions.ContainsKey(Components.Bridge),
          "a half-placed component is saved with its backups, not marked installed");
    Engine.Remove(g, Components.Bridge, docs);
    Directory.Delete(B(".trex"), true);
    Check(Read(B("d3d9.dll")) == "reshade", "the player's d3d9.dll restored after a failed copy");

    // 13. over a d3d9 wrapper (not DXVK): removing the bridge restores the wrapper and the Options.ini copy
    await Engine.InstallAsync(g, new[] { Components.Bridge }, src, null, docs);
    File.WriteAllText(Path.Combine(docs, "Options.ini"), "gpu=gtx580 defaults");
    Engine.Remove(g, Components.Bridge, docs);
    Check(Read(B("d3d9.dll")) == "reshade" && Read(Path.Combine(docs, "Options.ini")) == "gpu=real", "wrapper and Options.ini restored after the bridge");

    // 14. a bridge installed by hand: Repair takes its files as its own, so Remove takes the bridge out
    File.Delete(B("d3d9.dll"));
    File.WriteAllText(B("d3d9.dll"), "hand bridge remixapi_InitializeLibrary");
    Directory.CreateDirectory(B(".trex"));
    File.WriteAllText(B(@".trex\NvRemixBridge.exe"), "hand server");
    Check(Scan.Run(g, docs).Layer == Layer.Bridge, "scan sees the hand-installed bridge");
    await Engine.InstallAsync(g, new[] { Components.Bridge }, src, null, docs);
    Engine.Remove(g, Components.Bridge, docs);
    Check(!File.Exists(B("d3d9.dll")) && !File.Exists(B(@".trex\NvRemixBridge.exe")) && !File.Exists(Path.Combine(Manifest.DirOf(bin), "d3d9.dll.bak")),
          "a hand-installed bridge is removed after a repair, with no backup left");

    // 15. the player's ts3native.ini survives a repair; installing the right build clears the wrong one's log
    File.WriteAllText(B("ts3native.log"), "SKIP all: host is not TS3W.exe at 0x400000");
    await Engine.InstallAsync(g, new[] { Components.Modernization }, src, null, docs);
    Check(!Scan.Run(g, docs).Ts3NativeWrongBuild, "installing ts3native clears the old wrong-build log");
    File.WriteAllText(B("ts3native.ini"), "player switches");
    await Engine.InstallAsync(g, new[] { Components.Modernization }, src, null, docs);
    Check(Read(B("ts3native.ini")) == "player switches", "ts3native.ini edits kept on repair");
    Engine.Remove(g, Components.Modernization, docs);
    Check(!File.Exists(B("ts3native.ini")), "ts3native.ini removed with Modernization");

    // 16. a read-only file does not stop a compatibility fix
    File.WriteAllText(B("ts3native.asi"), "ts3native");
    File.WriteAllText(B("Sims3Performance.asi"), "s3pp");
    File.WriteAllText(B("Sims3Performance.ini"), "[Patches]\r\nShortWaitSpin=1\r\n");
    File.SetAttributes(B("Sims3Performance.ini"), FileAttributes.ReadOnly);
    await Engine.InstallAsync(g, Array.Empty<string>(), src, null, docs);
    Check(Read(B("Sims3Performance.ini")).Contains("ShortWaitSpin=0"), "read-only Sims3Performance.ini still fixed");
    foreach (var f in Directory.EnumerateFiles(bin)) File.SetAttributes(f, FileAttributes.Normal); // (so the test folder can be deleted)

    // 17. a second fake game (Origin 1.69.43): the layer comes from d3d9.dll itself, mods in the loader's subfolders count
    var bin2 = Path.Combine(root, "Origin", "Game", "Bin");
    var docs2 = Path.Combine(root, "Docs2");
    string B2(string rel) => Path.Combine(bin2, rel);
    Directory.CreateDirectory(Path.Combine(bin2, "scripts"));
    Directory.CreateDirectory(Path.Combine(docs2, "Mods", "Packages"));
    var g2 = new Game(Path.Combine(root, "Origin"), bin2, "TS3W.exe", Edition.Other, "1.69.43 (Origin)");
    File.WriteAllText(B2("d3d9.dll"), "bridge client dxvk remixapi_InitializeLibrary"); // unzipped without .trex
    var s17 = Scan.Run(g2, docs2);
    Check(s17.Layer == Layer.Bridge && Verify.Files(g2, s17, null).Any(x => x.Args.Contains(@".trex\NvRemixBridge.exe")), "a bridge client without .trex is the bridge, its server missing");
    Directory.CreateDirectory(B2(".trex"));
    File.WriteAllText(B2(@".trex\NvRemixBridge.exe"), "leftover");
    File.WriteAllText(B2("d3d9.dll"), "DXVK d3d9");
    Check(Scan.Run(g2, docs2).Layer == Layer.Dxvk, "DXVK over a leftover .trex is DXVK");
    File.WriteAllText(B2(@"scripts\Sims3SettingsSetter.asi"), "s3ss");
    File.WriteAllText(B2(@"scripts\ts3native.asi"), "ts3native");
    File.WriteAllText(B2(@"scripts\ts3native.log"), "SKIP all: host is not TS3W.exe at 0x400000");
    var s17b = Scan.Run(g2, docs2);
    Check(s17b.S3ss && s17b.Ts3Native && s17b.Ts3NativeWrongBuild, "S3SS and ts3native in scripts found, with the log next to them");
    File.WriteAllText(B2("Sims3SettingsSetter.asi"), "s3ss");
    File.WriteAllText(Path.Combine(docs2, "Mods", "Packages", "XCAS-1.69.package"), "pkg");
    var f17 = Compat.Check(g2, s17b, docs2);
    Check(f17.Any(x => x.Args.Contains(@"scripts\Sims3SettingsSetter.asi") && x.CanFix), "a second S3SS in scripts reported with a fix");
    Check(f17.Any(x => x.Text.StartsWith("Sims 3 Modernization has no build")), "a ts3native on an unsupported version is told to be turned off");
    Check(!f17.Any(x => x.Text.Contains("other edition of the game")), "a 1.69 script mod on Origin 1.69.43 is not the other edition's");
    f17.First(x => x.Args.Contains(@"scripts\Sims3SettingsSetter.asi")).Fix!();
    Check(!File.Exists(B2(@"scripts\Sims3SettingsSetter.asi")) && File.Exists(B2("Sims3SettingsSetter.asi")), "the scripts copy moved aside, the Game\\Bin one kept");
    Engine.Remove(g2, Components.Modernization, docs2);
    Check(!Scan.Run(g2, docs2).Ts3Native, "Modernization removed from scripts");
}
catch (Exception e) { Console.WriteLine("FAIL exception: " + e); failures++; }
finally { Directory.Delete(root, true); }

Console.WriteLine(failures == 0 ? "ALL PASSED" : failures + " FAILED");
return failures == 0 ? 0 : 1;

// the fake GitHub files, plus the S3PP archive the way GitHubSource takes it
sealed class MixedSource : ISource
{
    readonly ISource _inner; readonly string _zip;
    public MixedSource(ISource inner, string zip) { _inner = inner; _zip = zip; }
    public Task<(List<(string Src, string Dest)> Files, string Version)> FetchAsync(string c, Game g, string docs, CancellationToken ct) =>
        c == Components.S3pp ? new GitHubSource { S3ppArchive = _zip }.FetchAsync(c, g, docs, ct) : _inner.FetchAsync(c, g, docs, ct);
}

// the fake GitHub files, with one component's download failing
sealed class FailingSource : ISource
{
    readonly ISource _inner; readonly string _fail;
    public FailingSource(ISource inner, string fail) { _inner = inner; _fail = fail; }
    public Task<(List<(string Src, string Dest)> Files, string Version)> FetchAsync(string c, Game g, string docs, CancellationToken ct) =>
        c == _fail ? throw new HttpRequestException("offline") : _inner.FetchAsync(c, g, docs, ct);
}

sealed class FakeSource : ISource
{
    readonly string _dir;
    public FakeSource(string dir) { _dir = dir; Directory.CreateDirectory(dir); }

    string Make(string rel, string text) { var p = Path.Combine(_dir, rel); Directory.CreateDirectory(Path.GetDirectoryName(p)!); File.WriteAllText(p, text); return p; }

    public Task<(List<(string Src, string Dest)> Files, string Version)> FetchAsync(string c, Game g, string docs, CancellationToken ct)
    {
        var b = g.Bin;
        const string conf = "client.targetProcess = TS3W.exe\nserver.useVanillaDxvk = True\n";
        List<(string, string)> f = c switch
        {
            Components.Apex => new() { (Make("ApexRadiance.asi", "apex 3.1.1"), Path.Combine(b, "ApexRadiance.asi")) },
            Components.Loader => new() { (Make("dinput8.dll", "loader"), Path.Combine(b, "wininet.dll")) },
            Components.Dxvk => new() { (Make(@"dxvk\d3d9.dll", "DXVK 3.1.1 d3d9"), Path.Combine(b, "d3d9.dll")) },
            Components.S3ss => new() { (Make("Sims3SettingsSetter.asi", "s3ss"), Path.Combine(b, "Sims3SettingsSetter.asi")) },
            Components.MonoPatcher => new()
            {
                (Make(@"mp\MonoPatcher.asi", "mp"), Path.Combine(b, "MonoPatcher.asi")),
                (Make(@"mp\ld_MonoPatcher.package", "mp"), Path.Combine(Scan.Packages(docs), "MonoPatcher", "ld_MonoPatcher.package")),
            },
            Components.Modernization => new()
            {
                (Make(@"m\ts3native.asi", "ts3native"), Path.Combine(b, "ts3native.asi")),
                (Make(@"m\ts3native.ini", "ini"), Path.Combine(b, "ts3native.ini")),
                (Make("ts3fixes.package", "pkg"), Path.Combine(Scan.Packages(docs), "ts3fixes.package")),
            },
            Components.Bridge => Bridge(b, conf),
            _ => throw new ArgumentException(c),
        };
        return Task.FromResult((f, c == Components.Modernization ? "0.2.10" : "3.1.1"));
    }

    List<(string, string)> Bridge(string b, string conf)
    {
        var dir = Path.Combine(_dir, "bridge");
        Make(@"bridge\d3d9.dll", "bridge client dxvk remixapi_InitializeLibrary"); // (the real client holds both)
        Make(@"bridge\bridge.conf", conf);
        Make(@"bridge\dxvk.conf", "d3d9.customDeviceId = 1080");
        Make(@"bridge\.trex\bridge.conf", conf);
        Make(@"bridge\.trex\NvRemixBridge.exe", "server");
        Make(@"bridge\.trex\d3d9vk_x64.dll", "dxvk");
        Make(@"bridge\licenses\DXVK-LICENSE.txt", "zlib");
        var l = new List<(string, string)>();
        GitHubSource.AddTree(l, dir, b);
        return l;
    }
}
