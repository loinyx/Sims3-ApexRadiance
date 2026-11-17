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
// removals never reach the real driver: the hook counts the games it was asked to turn frame generation off for
var frameGenOff = new List<string>();
string? frameGenOffFails = null;
var frameGenOffAfterFiles = true; // (the hook runs once the component is gone from setup.json)
Engine.FrameGenOff = game => { frameGenOff.Add(game.Exe); frameGenOffAfterFiles &= !Manifest.Load(game.Bin).Versions.ContainsKey(Components.Bridge); return frameGenOffFails; };
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
    Check(Engine.DefaultLayer(s, false) == Layer.Bridge && Engine.DefaultLayer(s, true) == Layer.Dxvk,
        "first install: the bridge by default, DXVK when the card or driver cannot run it");
    Check(Engine.DefaultLayer(s with { ApexVersion = "4.0.0", Layer = Layer.Dxvk }, false) == Layer.Dxvk
        && Engine.DefaultLayer(s with { ApexVersion = "4.0.0" }, false) == Layer.Native
        && Engine.DefaultLayer(s with { Layer = Layer.Dxvk }, false) == Layer.Dxvk, "an installed renderer is kept");
    await Engine.InstallAsync(g, new[] { Components.Apex, Components.Bridge, Components.Loader }, src, null, docs);
    Check(Read(B("ApexRadiance.asi")) == "apex 3.1.1", "ApexRadiance.asi installed");
    Check(!File.Exists(B("ts3wrap.exe")) && !File.Exists(Engine.ApexToml(docs)), "Apex alone installs no ts3wrap.exe and writes no ApexRadiance.toml");
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
        Check(Releases.Newer("3.2.0", "3.2.0-beta") && !Releases.Newer("3.2.0", "3.2.0") && !Releases.Newer("3.2.0-beta", "3.2.0")
              && Releases.Newer("3.2.0", "3.1.9") && Releases.Newer("3.2.0", "") && !Releases.Newer(null, ""),
              "update check: a final release is newer than its prerelease, an unknown copy is older than any release");
        // an Iris Xe on a 2023 driver: Vulkan 1.3 without VK_KHR_load_store_op_none, which DXVK 3 requires
        var old = Gpu.DxvkExtensions.Where(e => e != "VK_KHR_load_store_op_none").Append("VK_EXT_load_store_op_none").ToList();
        Check(Gpu.Lacking(old).SequenceEqual(new[] { "VK_KHR_load_store_op_none" }) && Gpu.Lacking(Gpu.DxvkExtensions).Length == 0,
              "gpu: the DXVK extensions a driver lacks are found");
        Check(Gpu.Better((new Version(1, 3), Array.Empty<string>()), (new Version(1, 4), new[] { "VK_KHR_maintenance6" }))
              && !Gpu.Better((new Version(1, 2), Array.Empty<string>()), (new Version(1, 3), Array.Empty<string>())),
              "gpu: a card DXVK can use beats a newer one it cannot");
        Check(Gpu.DriverPage("Intel(R) Iris(R) Xe Graphics").Contains("intel.com") && Gpu.DriverPage("AMD Radeon 780M").Contains("amd.com"),
              "gpu: an old driver points at its vendor's driver page");
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

    // 2b. the restart host (experimental): ts3wrap.exe and [ui] restart_host; off again removes both, Apex's removal takes it along
    {
        Check(!Engine.RestartHostWaits(docs), "restart host: no older settings to copy, Setup may create ApexRadiance.apexToml");
        await Engine.InstallAsync(g, new[] { Components.RestartHost }, src, null, docs);
        var apexToml = Engine.ApexToml(docs);
        Check(Read(B("ts3wrap.exe")) == "wrap 3.1.1" && Read(apexToml) == Engine.NewApexToml && Scan.Run(g, docs).RestartHost
              && Scan.Run(g, docs).Installed(Components.RestartHost) == "3.1.1", "restart host: installed, a new ApexRadiance.apexToml keeps the welcome page");
        var pt = RestorePoints.List(bin)[0];
        Check(RestorePoints.Restore(bin, pt).Count == 0 && !File.Exists(apexToml) && !Directory.Exists(Path.GetDirectoryName(apexToml)) && !File.Exists(B("ts3wrap.exe")),
              "restart host: undo before the game ran removes the file and folder Setup created");
        // the game wrote its settings: Setup changes one line, undo puts back only that key
        Directory.CreateDirectory(Path.GetDirectoryName(apexToml)!);
        var game = "[meta]\r\nversion = 1\r\n\r\n[ui]\r\nfont_scale = 1.25\r\nlanguage = 'pt' # mine\r\n\r\n    [ui.profile_shortcuts]\r\n    Night = [ 17, 78 ]\r\n\r\n[patches.NightTerrainRelight]\r\nenabled = true\r\n";
        File.WriteAllText(apexToml, game);
        await Engine.InstallAsync(g, new[] { Components.RestartHost }, src, null, docs);
        Check(Read(apexToml) == game.Replace("language = 'pt' # mine\r\n", "language = 'pt' # mine\r\nrestart_host = true\r\n"),
              "restart host: the key added at the end of [ui], every other line kept");
        File.WriteAllText(apexToml, Read(apexToml).Replace("font_scale = 1.25", "font_scale = 1.5")); // (changed in game since)
        Check(RestorePoints.Restore(bin, RestorePoints.List(bin)[0]).Count == 0 && Read(apexToml) == game.Replace("font_scale = 1.25", "font_scale = 1.5")
              && !File.Exists(B("ts3wrap.exe")), "restart host: undo takes the key out and keeps what the game saved since");
        // removal: off, only the key changed; Remove Apex takes the host along
        var off = game.Replace("font_scale = 1.25", "restart_host = false\r\nfont_scale = 1.25");
        File.WriteAllText(apexToml, off);
        await Engine.InstallAsync(g, new[] { Components.RestartHost }, src, null, docs);
        Check(Read(apexToml) == game.Replace("font_scale = 1.25", "restart_host = true\r\nfont_scale = 1.25"), "restart host: an existing false turned true in place");
        Engine.Remove(g, Components.RestartHost, docs);
        Check(!File.Exists(B("ts3wrap.exe")) && Read(apexToml) == off && !Scan.Run(g, docs).RestartHost
              && Scan.Run(g, docs).Installed(Components.RestartHost) is null, "restart host: removed, the key back to false");
        await Engine.InstallAsync(g, new[] { Components.RestartHost }, src, null, docs);
        Engine.Remove(g, Components.Apex, docs);
        Check(!File.Exists(B("ts3wrap.exe")) && Engine.RestartHostKey(Read(apexToml)) == false && !File.Exists(B("ApexRadiance.asi")), "restart host: removing Apex removes it too");
        await Engine.InstallAsync(g, new[] { Components.Apex }, src, null, docs);
        // an older Setup put ts3wrap.exe in with Apex: an Apex update drops it while the host is off, keeps it while on
        var mOld = Manifest.Load(bin); File.WriteAllText(B("ts3wrap.exe"), "wrap 3.1.0"); mOld.Files.Add(new ManifestFile { Component = Components.Apex, Path = B("ts3wrap.exe") }); mOld.Save();
        await Engine.InstallAsync(g, new[] { Components.Apex }, src, null, docs);
        Check(!File.Exists(B("ts3wrap.exe")) && !Manifest.Load(bin).Files.Any(f => f.Path.EndsWith("ts3wrap.exe")), "restart host: the off ts3wrap.exe of an older Setup removed on update");
        File.Delete(apexToml);
        Directory.Delete(Path.GetDirectoryName(apexToml)!);
        // older settings Apex still has to copy: Setup waits, nothing changes
        Directory.CreateDirectory(Path.Combine(docs, "S3SS"));
        File.WriteAllText(Path.Combine(docs, "S3SS", "S3SS.toml"), "[general]\nx = 1\n");
        Check(!Engine.RestartHostWaits(docs), "restart host: a plain S3SS.toml does not hold it back");
        File.WriteAllText(Path.Combine(docs, "S3SS", "S3SS.toml"), "[general]\nx = 1\n[patches.NightTerrainRelight]\nenabled = true\n");
        try { await Engine.InstallAsync(g, new[] { Components.RestartHost }, src, null, docs); Check(false, "restart host: waits for Apex's copy of older settings"); }
        catch (InvalidOperationException e) { Check(e.Message == Engine.RestartHostWaitsText && !File.Exists(apexToml) && !File.Exists(B("ts3wrap.exe")), "restart host: waits for Apex's copy of older settings"); }
        Directory.Delete(Path.Combine(docs, "S3SS"), true);
    }

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
    Check(Compat.Set("[ patches ]\nshortwaitspin=1\n", "Patches", new[] { "ShortWaitSpin" }, v => v != "0", "0") == "[ patches ]\nshortwaitspin=0\n", "ini section and key match without case");
    var dxw = new List<string>();
    var dxc = "d3d9.customVendorId = 10de\r\nd3d9.customDeviceId = 1080\r\nd3d9.countLosableResources = True\r\n[TS3W.exe]\r\nd3d9.customDeviceDesc = \"Geforce GTX 580\"\r\n";
    Check(Compat.FixDxvk(dxc, dxw) == "d3d9.customDeviceDesc = \"Geforce GTX 580\"\r\n" + dxc.Replace("= True", "= False") && dxw.SequenceEqual(new[] { "d3d9.customDeviceDesc", "d3d9.countLosableResources" }),
          "bridge dxvk.conf: wrong value replaced, key under [TS3W.exe] added to the global part");
    var dxw2 = new List<string>();
    Compat.FixDxvk(Compat.FixDxvk(dxc), dxw2);
    Check(dxw2.Count == 0, "bridge dxvk.conf fix settles");

    // 8. MonoPatcher in, then out; Smooth Patch's ASI next to S3SS is moved away
    await Engine.InstallAsync(g, new[] { Components.MonoPatcher }, src, null, docs);
    Check(Scan.Run(g, docs).MonoPatcher && File.Exists(Path.Combine(Scan.Packages(docs), "MonoPatcher", "ld_MonoPatcher.package")), "MonoPatcher installed");
    var rcfg = Path.Combine(docs, "Mods", "Resource.cfg");
    var rcfgOld = Read(rcfg);
    File.WriteAllText(rcfg, "Priority 500\r\nPackedFile Packages/*.package\r\n");
    var lost = Compat.Check(g, Scan.Run(g, docs), docs).Where(x => x.Text.StartsWith("Mods\\Resource.cfg does not read")).ToList();
    Check(lost.Count == 1 && lost[0].Args[0].EndsWith("ld_MonoPatcher.package"), "Resource.cfg that skips MonoPatcher's folder reported");
    lost[0].Fix!();
    Check(!Compat.Check(g, Scan.Run(g, docs), docs).Any(x => x.Text.StartsWith("Mods\\Resource.cfg does not read")) && File.Exists(rcfg + ".apex-backup"), "Resource.cfg fixed, copy kept");
    File.WriteAllText(rcfg, rcfgOld); File.Delete(rcfg + ".apex-backup");
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
    Engine.RemoveAll(g, docs); // (the bridge has no version, only files)
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
    File.WriteAllText(B("bridge.conf"), "player conf");
    Check(Scan.Run(g, docs).Layer == Layer.Bridge, "scan sees the hand-installed bridge");
    await Engine.InstallAsync(g, new[] { Components.Bridge }, src, null, docs);
    Check(Read(Path.Combine(Manifest.DirOf(bin), "bridge.conf.bak")) == "player conf", "a hand-installed bridge's edited bridge.conf is backed up");
    Engine.Remove(g, Components.Bridge, docs);
    Check(!File.Exists(B("d3d9.dll")) && !File.Exists(B(@".trex\NvRemixBridge.exe")) && !File.Exists(Path.Combine(Manifest.DirOf(bin), "d3d9.dll.bak")),
          "a hand-installed bridge is removed after a repair, with no backup left");
    File.Delete(B("bridge.conf"));

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
    Check(!Verify.Files(g2, s17, null).Any(x => x.Args.Contains(@".trex\d3d9vk_x64.dll")), "d3d9vk_x64.dll is not required of a bridge Setup did not install");
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
    File.WriteAllText(B2(@"scripts\ts3native.ini"), "player switches");
    await Engine.InstallAsync(g2, new[] { Components.Modernization }, src, null, docs2);
    Check(!File.Exists(B2("ts3native.asi")) && File.Exists(B2(@"scripts\ts3native.asi")) && Read(B2(@"scripts\ts3native.ini")) == "player switches",
          "Modernization in scripts is updated there, its ts3native.ini kept");
    Engine.Remove(g2, Components.Modernization, docs2);
    Check(!Scan.Run(g2, docs2).Ts3Native, "Modernization removed from scripts");

    // 18. restore points: a third fake game, every file of Game\Bin and Documents compared byte for byte
    var bin3 = Path.Combine(root, "Third", "Game", "Bin");
    var docs3 = Path.Combine(root, "Docs3");
    string B3(string rel) => Path.Combine(bin3, rel);
    Directory.CreateDirectory(bin3);
    Directory.CreateDirectory(docs3);
    File.WriteAllText(B3("TS3W.exe"), "exe");
    var g3 = new Game(Path.Combine(root, "Third"), bin3, "TS3W.exe", Edition.Steam, "1.67.2");
    var restoreDir = RestorePoints.RootOf(bin3);
    Dictionary<string, string> Tree() => new[] { bin3, docs3 }.SelectMany(d => Directory.EnumerateFiles(d, "*", SearchOption.AllDirectories))
        .Where(f => !f.StartsWith(restoreDir, StringComparison.OrdinalIgnoreCase))
        .ToDictionary(f => f, f => Convert.ToHexString(File.ReadAllBytes(f)));
    bool SameTree(Dictionary<string, string> a, Dictionary<string, string> b) => a.Count == b.Count && a.All(p => b.TryGetValue(p.Key, out var v) && v == p.Value);

    // a foreign d3d9.dll under the bridge, then the point restored: the original back, nothing of Setup left
    File.WriteAllBytes(B3("d3d9.dll"), new byte[] { 0x4D, 0x5A, 0x90, 0x00, 0xFF, 0x13 });
    File.WriteAllText(Path.Combine(docs3, "Options.ini"), "gpu=real");
    var t0 = Tree();
    await Engine.InstallAsync(g3, new[] { Components.Bridge, Components.Loader }, src, null, docs3);
    var pts = RestorePoints.List(bin3);
    Check(pts.Count == 1 && pts[0].Label == "Install {0}" && pts[0].Args[0] == "Ultimate ASI Loader, RTX Remix bridge"
          && pts[0].Entries.Any(e => e.Path.EndsWith("setup.json")) && pts[0].Entries.Any(e => e.Dir && e.Path.EndsWith(".trex")),
          "an install takes one restore point, with setup.json and the folders it created");
    Check(RestorePoints.Restore(bin3, pts[0]).Count == 0 && SameTree(Tree(), t0) && !Directory.Exists(B3(".trex")) && !Directory.Exists(Manifest.DirOf(bin3)),
          "restore: the foreign d3d9.dll byte-identical, no file or folder of Setup left");

    // a compatibility fix on a Documents file, then restored
    Directory.CreateDirectory(Path.Combine(docs3, "Mods", "Packages"));
    File.WriteAllText(Path.Combine(docs3, "Mods", "Packages", "ts3fixes.package"), "pkg");
    var cfg3 = Path.Combine(docs3, "Mods", "Resource.cfg");
    File.WriteAllText(cfg3, "Priority 500\r\nPackedFile *.package\r\n");
    var t1 = Tree();
    Compat.Check(g3, Scan.Run(g3, docs3), docs3).First(x => x.Text.StartsWith("Mods\\Resource.cfg does not read")).Fix!();
    Check(File.Exists(cfg3 + ".apex-backup") && Read(cfg3) != "Priority 500\r\nPackedFile *.package\r\n", "the Resource.cfg fix ran");
    pts = RestorePoints.List(bin3);
    Check(pts.Count == 1 && pts[0].Label == "Fix incompatible settings" && RestorePoints.Restore(bin3, pts[0]).Count == 0 && SameTree(Tree(), t1),
          "a fix on a Documents file restored, its .apex-backup gone");

    // three points, the oldest restored: all three undone, the folders they created removed
    var t2 = Tree();
    await Engine.InstallAsync(g3, new[] { Components.Apex }, src, null, docs3);
    await Engine.InstallAsync(g3, new[] { Components.S3ss }, src, null, docs3);
    await Engine.InstallAsync(g3, new[] { Components.MonoPatcher }, src, null, docs3);
    pts = RestorePoints.List(bin3);
    Check(pts.Count == 3 && pts[0].Args[0] == "MonoPatcher" && pts[2].Args[0] == "Apex Radiance", "three points, newest first");
    Check(RestorePoints.Restore(bin3, pts[2]).Count == 0 && SameTree(Tree(), t2) && !Directory.Exists(Path.Combine(Scan.Packages(docs3), "MonoPatcher"))
          && RestorePoints.List(bin3).Count == 0, "restoring the oldest of three undoes all three and removes the folders they created");

    // setup.json back as it was (not removed: it existed)
    await Engine.InstallAsync(g3, new[] { Components.Apex }, src, null, docs3);
    var json3 = Path.Combine(Manifest.DirOf(bin3), "setup.json");
    var jsonBefore = File.ReadAllText(json3);
    await Engine.InstallAsync(g3, new[] { Components.S3ss }, src, null, docs3);
    Check(RestorePoints.Restore(bin3, RestorePoints.List(bin3)[0]).Count == 0 && Read(json3) == jsonBefore && !File.Exists(B3("Sims3SettingsSetter.asi"))
          && File.Exists(B3("ApexRadiance.asi")) && RestorePoints.List(bin3).Count == 1, "setup.json restored to the point's copy, the older point kept");

    // refused while the game runs
    Games.IsRunning = () => true;
    try { RestorePoints.Restore(bin3, RestorePoints.List(bin3)[0]); Check(false, "restore refused while the game runs"); }
    catch (InvalidOperationException) { Check(File.Exists(B3("ApexRadiance.asi")) && RestorePoints.List(bin3).Count == 1, "restore refused while the game runs"); }
    finally { Games.IsRunning = () => false; }

    // a path that cannot be put back is reported, the others are, and the point stays
    File.WriteAllText(B3("ApexRadiance.asi"), "apex 3.0.0");
    var keepFile = B3("keep.txt");
    using (RestorePoints.Begin(bin3, "Install {0}", "test"))
    {
        Fs.Write(keepFile, "new");
        Fs.Write(B3("ApexRadiance.asi"), "apex 3.1.1");
    }
    var p3 = RestorePoints.List(bin3)[0];
    List<string> failedPaths;
    using (File.Open(keepFile, FileMode.Open, FileAccess.Read, FileShare.None)) failedPaths = RestorePoints.Restore(bin3, p3);
    Check(failedPaths.Count == 1 && failedPaths[0] == Path.GetFullPath(keepFile) && Read(B3("ApexRadiance.asi")) == "apex 3.0.0"
          && RestorePoints.List(bin3).Any(p => p.Dir == p3.Dir), "a locked file is reported, the rest restored, the point kept");
    Check(RestorePoints.Restore(bin3, p3).Count == 0 && !File.Exists(keepFile), "the kept point restores once the file is free");

    // an action that changes nothing keeps no point; the newest 5 are kept
    using (RestorePoints.Begin(bin3, "Install {0}", "nothing")) { }
    Check(RestorePoints.List(bin3).Count == 1, "an empty point is not kept");
    for (int i = 0; i < 6; i++)
        using (RestorePoints.Begin(bin3, "Install {0}", "n" + i)) Fs.Write(B3("counter.txt"), i.ToString());
    pts = RestorePoints.List(bin3);
    Check(pts.Count == RestorePoints.Keep && pts[0].Args[0] == "n5" && pts[4].Args[0] == "n1"
          && Directory.EnumerateDirectories(restoreDir).Count() == RestorePoints.Keep, "the newest 5 points kept, older ones deleted");
    Check(RestorePoints.Restore(bin3, RestorePoints.List(bin3).Last()).Count == 0 && RestorePoints.List(bin3).Count == 0, "every point restored");

    // a locked file in the newer of two points: the restore stops there, the older point untouched, then finishes on retry
    var a3 = B3("a.txt");
    File.WriteAllText(a3, "orig");
    using (RestorePoints.Begin(bin3, "Install {0}", "p1")) { Fs.Write(a3, "p1"); Fs.Write(B3("new1.txt"), "x"); }
    using (RestorePoints.Begin(bin3, "Install {0}", "p2")) { Fs.Write(B3("keep2.txt"), "new"); Fs.Write(a3, "p2"); }
    var older = RestorePoints.List(bin3)[1];
    using (File.Open(B3("keep2.txt"), FileMode.Open, FileAccess.Read, FileShare.None)) failedPaths = RestorePoints.Restore(bin3, older);
    Check(failedPaths.Count == 1 && Read(a3) == "p1" && File.Exists(B3("new1.txt")) && RestorePoints.List(bin3).Count == 2,
          "a failure in a newer point stops the restore before the older one");
    Check(RestorePoints.Restore(bin3, older).Count == 0 && Read(a3) == "orig" && !File.Exists(B3("new1.txt")) && !File.Exists(B3("keep2.txt"))
          && RestorePoints.List(bin3).Count == 0, "the retry undoes both points");

    // point.json is written as the action goes: an action cut off is listed; a folder without it is pruned
    using (RestorePoints.Begin(bin3, "Install {0}", "cut"))
    {
        Fs.Write(a3, "cut");
        Check(RestorePoints.List(bin3) is [{ FileCount: 1 }], "a point being recorded is already listed");
        Directory.CreateDirectory(Path.Combine(restoreDir, "junk"));
        File.WriteAllText(Path.Combine(restoreDir, "junk", "0-x.txt"), "x");
    }
    Check(!Directory.Exists(Path.Combine(restoreDir, "junk")), "a folder without point.json is pruned");

    // a copy that fails records nothing: a later change to the same file in that action still saves the original
    using (RestorePoints.Begin(bin3, "Install {0}", "touch"))
    {
        using (File.Open(a3, FileMode.Open, FileAccess.ReadWrite, FileShare.None))
            try { Fs.ClearReadOnly(a3); Check(false, "a failed copy throws before the change"); } catch (IOException) { }
        Fs.Write(a3, "touched");
    }
    Check(RestorePoints.Restore(bin3, RestorePoints.List(bin3).Last()).Count == 0 && Read(a3) == "orig", "the original saved by the retried touch");

    // a game folder moved: its points name the old folder, so they are not offered there
    var root4 = Path.Combine(root, "Fourth");
    var bin4 = Path.Combine(root4, "Game", "Bin");
    Directory.CreateDirectory(bin4);
    File.WriteAllText(Path.Combine(bin4, "x.txt"), "orig");
    using (RestorePoints.Begin(bin4, "Install {0}", "moved")) Fs.Write(Path.Combine(bin4, "x.txt"), "new");
    var moved = RestorePoints.List(bin4)[0];
    var bin5 = Path.Combine(root, "Fifth", "Game", "Bin");
    Directory.Move(root4, Path.Combine(root, "Fifth"));
    Check(RestorePoints.List(bin5).Count == 0 && RestorePoints.List(bin5 + "\\").Count == 0, "a moved game's points are not listed");
    try { RestorePoints.Restore(bin5, moved); Check(false, "a moved game's point is refused"); }
    catch (InvalidOperationException) { Check(!Directory.Exists(root4) && Read(Path.Combine(bin5, "x.txt")) == "new", "a moved game's point is refused"); }
    // suggestions (what Apex recommends in game): found, never run by Install, applied by their own click into a restore
    // point, and restored
    {
        var bin6 = Path.Combine(root, "Sixth", "Game", "Bin");
        var docs6 = Path.Combine(root, "Docs6");
        Directory.CreateDirectory(bin6);
        Directory.CreateDirectory(docs6);
        File.WriteAllText(Path.Combine(bin6, "TS3W.exe"), "exe");
        var g6 = new Game(Path.Combine(root, "Sixth"), bin6, "TS3W.exe", Edition.Steam, "1.67.2");
        string B6(string rel) => Path.Combine(bin6, rel);
        Dictionary<string, string> Tree6() => new[] { bin6, docs6 }.SelectMany(d => Directory.EnumerateFiles(d, "*", SearchOption.AllDirectories))
            .ToDictionary(f => f, File.ReadAllText);
        bool Same6(Dictionary<string, string> a, Dictionary<string, string> b) => a.Count == b.Count && a.All(p => b.TryGetValue(p.Key, out var v) && v == p.Value);
        List<Finding> Sug() => Compat.Check(g6, Scan.Run(g6, docs6), docs6).Where(x => x.Suggest is not null).ToList();
        Finding? Named(string name) => Sug().FirstOrDefault(x => x.Suggest == name);
        const string Aa = "Turn off the game's Edge Smoothing";

        // the game's Edge Smoothing: on in Options.ini, every Apex effect on by default (no ApexRadiance.toml)
        var opt6 = Path.Combine(docs6, "Options.ini");
        const string optText = "[Options]\r\nedgesmoothing = 2\r\nwaterreflection = 1\r\n";
        File.WriteAllText(opt6, optText);
        Check(Sug().Count == 0, "suggestions: none without ApexRadiance.asi (Apex makes them while it runs)");
        File.WriteAllText(B6("ApexRadiance.asi"), "MZ apex");
        var aa = Named(Aa);
        Check(aa is { CanApply: true, CanFix: false, Severity: Severity.Warning } && aa.Args[0] == "2"
              && aa.Args[1] == "Edge Smoothing, Depth Blur, Ambient Occlusion, Water Reflections", "edge smoothing: suggested with every Apex effect on by default");
        await Engine.InstallAsync(g6, Array.Empty<string>(), src, null, docs6);
        Check(Read(opt6) == optText, "edge smoothing: Install does not apply a suggestion");
        var t6 = Tree6();
        aa!.Fix!();
        var pts6 = RestorePoints.List(bin6);
        Check(Read(opt6) == "[Options]\r\nedgesmoothing = 0\r\nwaterreflection = 1\r\n" && Read(opt6 + ".apex-backup") == optText && Named(Aa) is null,
              "edge smoothing: Apply sets edgesmoothing = 0 only, copy kept");
        Check(pts6.Count == 1 && pts6[0].Label == "Apply suggestion: {0}" && pts6[0].Args[0] == Aa && RestorePoints.Restore(bin6, pts6[0]).Count == 0
              && Same6(Tree6(), t6), "edge smoothing: its own restore point, restored byte for byte");
        // a read-only Options.ini (players set it so the game keeps their settings) stays read-only, applied and restored
        File.SetAttributes(opt6, FileAttributes.ReadOnly);
        Named(Aa)!.Fix!();
        Check(Read(opt6).Contains("edgesmoothing = 0") && File.GetAttributes(opt6).HasFlag(FileAttributes.ReadOnly), "edge smoothing: a read-only Options.ini stays read-only");
        Check(RestorePoints.Restore(bin6, RestorePoints.List(bin6)[0]).Count == 0 && Read(opt6) == optText && File.GetAttributes(opt6).HasFlag(FileAttributes.ReadOnly),
              "edge smoothing: restored, still read-only");
        File.SetAttributes(opt6, FileAttributes.Normal);

        // the effects as ApexRadiance.toml has them
        var apexToml = Path.Combine(docs6, "Apex Radiance", "ApexRadiance.toml");
        Directory.CreateDirectory(Path.GetDirectoryName(apexToml)!);
        File.WriteAllText(apexToml, "[patches.EdgeSmoothing]\nenabled = false\n[patches.DepthBlur]\nenabled = false\n[patches.AmbientOcclusion]\nenabled = true\n[patches.NightTerrainRelight]\nreflexoNoLago = 0.0\n");
        Check(Named(Aa)?.Args[1] == "Ambient Occlusion", "edge smoothing: only the Apex effects that are on are named");
        File.WriteAllText(apexToml, "[patches.EdgeSmoothing]\nenabled = false\n[patches.DepthBlur]\nenabled = false\n[patches.AmbientOcclusion]\nenabled = false\n[patches.NightTerrainRelight]\nenabled = true\nreflexoNoLago = 0.0\n");
        Check(Named(Aa) is null, "edge smoothing: no suggestion with every affected Apex effect off");
        File.WriteAllText(apexToml, "[patches.EdgeSmoothing]\nenabled = yes\n");
        var bad = Compat.Check(g6, Scan.Run(g6, docs6), docs6);
        Check(bad.Any(x => x.Text.Contains("cannot read") && x.Args[0] == "ApexRadiance.toml" && !x.CanFix && !x.CanApply), "edge smoothing: an unreadable ApexRadiance.toml value is reported");
        File.Delete(apexToml);
        File.WriteAllText(opt6, "[Options]\r\nedgesmoothing = high\r\n");
        bad = Compat.Check(g6, Scan.Run(g6, docs6), docs6);
        Check(bad.Any(x => x.Args.SequenceEqual(new[] { "Options.ini", "edgesmoothing", "high" }) && x.Fix is null) && Named(Aa) is null,
              "edge smoothing: an unreadable Options.ini value is reported, not rewritten");
        File.WriteAllText(opt6, "[Options]\r\nedgesmoothing = 0\r\n");
        Check(Named(Aa) is null, "edge smoothing: off in the game, nothing to suggest");
        File.Delete(opt6);
        Check(Named(Aa) is null, "edge smoothing: no Options.ini, nothing to suggest");

        // a renamed ApexRadiance.asi (its debugger line), not the old combined build
        File.WriteAllText(B6("ApexRadiance - Copy.asi"), "MZ [Apex Radiance] Another copy of ApexRadiance.asi is already running in this process");
        File.WriteAllText(B6("Sims3SettingsSetter.asi"), "MZ Sims3 Settings Setter Apex Edition S3SS Log - Started at  ###S3SSWindow");
        var found6 = Compat.Check(g6, Scan.Run(g6, docs6), docs6);
        var copy = found6.FirstOrDefault(x => x.Suggest == "Two copies of the mod");
        Check(copy is { CanApply: true } && copy.Args[0] == "ApexRadiance - Copy.asi"
              && found6.Count(x => x.Args.Contains("Sims3SettingsSetter.asi")) == 1 && found6.First(x => x.Args.Contains("Sims3SettingsSetter.asi")).CanFix,
              "renamed copy: suggested once, the combined build stays its own fix");
        File.Delete(B6("Sims3SettingsSetter.asi"));
        File.Delete(B6("ApexRadiance.asi"));
        Check(!Compat.Check(g6, Scan.Run(g6, docs6), docs6).Any(x => x.Args.Contains("ApexRadiance - Copy.asi")), "renamed copy: the only Apex (no ApexRadiance.asi) is left alone");
        File.WriteAllText(B6("ApexRadiance.asi"), "MZ apex");
        var t6b = Tree6();
        copy!.Fix!();
        Check(!File.Exists(B6("ApexRadiance - Copy.asi")) && Named("Two copies of the mod") is null, "renamed copy: Apply moves it to the backup folder");
        Check(RestorePoints.Restore(bin6, RestorePoints.List(bin6)[0]).Count == 0 && Same6(Tree6(), t6b), "renamed copy: restored");
        File.Delete(B6("ApexRadiance - Copy.asi"));
        // a renamed old standalone (the Apex Edition name without S3SS's strings): a second copy, and S3SS still suggested
        File.WriteAllText(B6("OldApex.asi"), "MZ Sims3 Settings Setter Apex Edition");
        var old6 = Compat.Check(g6, Scan.Run(g6, docs6), docs6);
        Check(old6.Any(x => x.CanFix && x.Args.SequenceEqual(new[] { "OldApex.asi" }) && x.Text.Contains("second copy")) && old6.Any(x => x.Suggest == "Sims3SettingsSetter"),
              "renamed old standalone: moved as a second copy, Sims3SettingsSetter still suggested");
        File.Delete(B6("OldApex.asi"));

        // the components Apex recommends: pointers only, no fix
        Check(Named("RTX Remix bridge") is { Fix: null } && Named("Sims3SettingsSetter") is { Fix: null } && Named("DXVK") is null,
              "the game's own D3D9 and no S3SS: the bridge and S3SS suggested, nothing to apply");
        File.WriteAllText(B6("d3d9.dll"), "some other wrapper");
        Check(Named("RTX Remix bridge") is not null, "an unknown d3d9.dll: the bridge suggested");
        File.WriteAllText(B6("d3d9.dll"), "dxvk d3d9");
        File.WriteAllText(B6("Sims3SettingsSetter.asi"), "s3ss");
        Check(Named("DXVK") is null && Named("Sims3SettingsSetter") is null && Named("RTX Remix bridge") is { Fix: null }, "DXVK and S3SS in: the bridge offered");

        // Apply all: every suggestion with a fix, in one point
        File.WriteAllText(opt6, optText);
        File.WriteAllText(B6("ApexRadiance - Copy.asi"), "Another copy of ApexRadiance.asi is already running");
        var t6c = Tree6();
        using (RestorePoints.Begin(bin6, "Apply all suggestions"))
            foreach (var x in Sug().Where(x => x.CanApply)) x.Fix!();
        pts6 = RestorePoints.List(bin6);
        Check(Sug().All(x => !x.CanApply) && pts6.Count == 1 && pts6[0].Label == "Apply all suggestions", "apply all: every fix in one point");
        Check(RestorePoints.Restore(bin6, pts6[0]).Count == 0 && Same6(Tree6(), t6c), "apply all: restored");
    }

    // frame generation (Smooth Motion, AFMF) on an EA app game (TS3.exe): the frame limit in that game's Documents, undone alone
    {
        var fgToml = Engine.ApexToml(docs);
        Directory.CreateDirectory(Path.GetDirectoryName(fgToml)!);
        File.WriteAllText(fgToml, "[ui]\r\nlanguage = \"pt\"\r\n");
        var before = RestorePoints.List(bin).Count;
        Check(Engine.SetFpsLimitIfNone(g, "NVIDIA Smooth Motion", docs) && Read(fgToml) == "[ui]\r\nlanguage = \"pt\"\r\n\r\n[screen]\r\nfps_limit = 60\r\n"
              && RestorePoints.List(bin)[0].Label == "Turn on {0}", "frame limit: 60 when there is none, CRLF kept, its own restore point");
        File.WriteAllText(fgToml, Read(fgToml).Replace("language = \"pt\"", "language = \"de\"")); // the game saves another setting since
        Check(RestorePoints.Restore(bin, RestorePoints.List(bin)[0]).Count == 0 && Read(fgToml) == "[ui]\r\nlanguage = \"de\"\r\n\r\n[screen]\r\n",
              "frame limit: restoring puts back only fps_limit");
        File.WriteAllText(fgToml, "[screen]\nfps_limit = 0\n");
        Engine.SetFpsLimitIfNone(g, "NVIDIA Smooth Motion", docs);
        Check(RestorePoints.Restore(bin, RestorePoints.List(bin)[0]).Count == 0 && Read(fgToml) == "[screen]\nfps_limit = 0\n", "frame limit: a 0 before is put back as 0");
        File.WriteAllText(fgToml, "[screen]\nfps_limit = 144\n");
        before = RestorePoints.List(bin).Count;
        Check(!Engine.SetFpsLimitIfNone(g, "NVIDIA Smooth Motion", docs) && Read(fgToml) == "[screen]\nfps_limit = 144\n" && RestorePoints.List(bin).Count == before,
              "frame limit: a limit the player chose stays, no restore point");
        File.WriteAllText(fgToml, "[ui]\n");
        Environment.SetEnvironmentVariable("APEX_SETUP_DRY_RUN", "1");
        var threw = false;
        try { Engine.SetFpsLimitIfNone(g, "NVIDIA Smooth Motion", docs); } catch (InvalidOperationException) { threw = true; }
        Environment.SetEnvironmentVariable("APEX_SETUP_DRY_RUN", null);
        Check(threw && Read(fgToml) == "[ui]\n", "frame limit: the dry run writes nothing");
        File.Delete(fgToml);
        Check(!Engine.SetFpsLimitIfNone(g, "NVIDIA Smooth Motion", docs) && !File.Exists(fgToml), "frame limit: nothing written before the game made ApexRadiance.toml");

        // which removals turn it off: the bridge (alone or by a renderer change) and Apex, not DXVK or the others
        Check(Engine.TurnsFrameGenOff(Components.Bridge) && Engine.TurnsFrameGenOff(Components.Apex) && !Engine.TurnsFrameGenOff(Components.Dxvk)
              && !Engine.TurnsFrameGenOff(Components.S3ss), "frame generation: removing the bridge or Apex turns it off");
        frameGenOff.Clear();
        frameGenOffFails = "nvapi64.dll not found";
        Engine.Remove(g, Components.Bridge, docs);
        Check(frameGenOff.SequenceEqual(new[] { "TS3.exe" }) && Engine.TakeFrameGenOffError() == "nvapi64.dll not found" && Engine.TakeFrameGenOffError() is null,
              "frame generation: removing the bridge asks the EA app game's TS3.exe, and a driver failure does not fail the removal");
        Check(frameGenOffAfterFiles, "frame generation: turned off only after the bridge's files are removed");
        // review fixes: the player's later change wins over an undo; no temporary file left; CRLF without a final newline; dotted or
        // inline [screen] and an unparseable limit left alone
        File.WriteAllText(fgToml, "[ui]\r\nx = 1\r\n");
        Engine.SetFpsLimitIfNone(g, "NVIDIA Smooth Motion", docs);
        File.WriteAllText(fgToml, Read(fgToml).Replace("fps_limit = 60", "fps_limit = 75"));
        Check(RestorePoints.Restore(bin, RestorePoints.List(bin)[0]).Count == 0 && Read(fgToml).Contains("fps_limit = 75") && !File.Exists(fgToml + ".apex-tmp"),
              "frame limit: a limit the player changed since stays on restore; written through a temporary file");
        Check(Engine.WithFpsLimit("[screen]\r\nvsync = 1", 60) == "[screen]\r\nvsync = 1\r\nfps_limit = 60\r\n"
              && Engine.WithFpsLimit("[ui]\r\nx = 1", 60) == "[ui]\r\nx = 1\r\n\r\n[screen]\r\nfps_limit = 60\r\n", "frame limit: CRLF without a final newline stays CRLF");
        Check(Engine.WithFpsLimitIfNone("screen.vsync = 1\n").Text == "screen.vsync = 1\n" && Engine.WithFpsLimitIfNone("screen = { vsync = 1 }\n").Text == "screen = { vsync = 1 }\n"
              && Engine.WithFpsLimitIfNone("[screen]\nfps_limit = 99999999999\n").Text == "[screen]\nfps_limit = 99999999999\n",
              "frame limit: dotted or inline screen keys and an unparseable limit are left alone");
        // the frame limit picker: Apex's choices plus 75 and 90, custom clamped as Apex reads it, its own restore point
        Check(Engine.FpsChoices.SequenceEqual(new[] { 0, 30, 60, 75, 90, 120, 144, 165, 240 }) && Engine.FpsRecommended(60) && Engine.FpsRecommended(90)
              && !Engine.FpsRecommended(120) && Engine.ClampFps(-5) == 0 && Engine.ClampFps(0) == 0 && Engine.ClampFps(5) == 20 && Engine.ClampFps(5000) == 1000
              && Engine.ClampFps(77) == 77, "frame limit picker: the choices, the recommended ones, custom clamped to 0 or 20 to 1000");
        File.WriteAllText(fgToml, "[screen]\r\nfps_limit = 144\r\n");
        Check(Engine.SetFpsLimit(g, 5000, docs) && Read(fgToml) == "[screen]\r\nfps_limit = 1000\r\n" && RestorePoints.List(bin)[0].Label == "Apex frame limit"
              && !Engine.SetFpsLimit(g, 1000, docs), "frame limit picker: writes at once, clamped, nothing when unchanged");
        Check(RestorePoints.Restore(bin, RestorePoints.List(bin)[0]).Count == 0 && Read(fgToml) == "[screen]\r\nfps_limit = 144\r\n", "frame limit picker: restoring puts back the player's 144");
        Environment.SetEnvironmentVariable("APEX_SETUP_DRY_RUN", "1");
        var pickThrew = false;
        try { Engine.SetFpsLimit(g, 75, docs); } catch (InvalidOperationException) { pickThrew = true; }
        Environment.SetEnvironmentVariable("APEX_SETUP_DRY_RUN", null);
        Check(pickThrew && Read(fgToml) == "[screen]\r\nfps_limit = 144\r\n", "frame limit picker: the dry run writes nothing");
        frameGenOffFails = null;
        frameGenOff.Clear();
        Engine.Remove(g, Components.S3ss, docs);
        Check(frameGenOff.Count == 0, "frame generation: removing another component leaves it alone");
        Environment.SetEnvironmentVariable("APEX_SETUP_DRY_RUN", "1");
        try { Engine.Remove(g, Components.Bridge, docs); } catch (InvalidOperationException) { }
        Environment.SetEnvironmentVariable("APEX_SETUP_DRY_RUN", null);
        Check(frameGenOff.Count == 0, "frame generation: the dry run turns nothing off");
    }
}
catch (Exception e) { Console.WriteLine("FAIL exception: " + e); failures++; }
finally
{
    foreach (var f in Directory.EnumerateFiles(root, "*", SearchOption.AllDirectories)) File.SetAttributes(f, FileAttributes.Normal); // (a restore point keeps a read-only file's copy read-only)
    Directory.Delete(root, true);
}

// A release with only Setup and the manual-install zip (3.1.6 on): Apex, ts3wrap.exe and the bridge come from the zip,
// downloaded over HTTP from a local server as from GitHub
{
    var work = Path.Combine(Path.GetTempPath(), "ApexSetupZip-" + Guid.NewGuid().ToString("N")[..8]);
    var top = Path.Combine(work, "src", "Apex Radiance 9.9.9 - Manual install");
    Directory.CreateDirectory(Path.Combine(top, "RTX Remix bridge", ".trex"));
    File.WriteAllText(Path.Combine(top, "How to install.txt"), "guide");
    File.WriteAllText(Path.Combine(top, "ApexRadiance.asi"), "apex 9.9.9");
    File.WriteAllText(Path.Combine(top, "ts3wrap.exe"), "wrap 9.9.9");
    File.WriteAllText(Path.Combine(top, "RTX Remix bridge", "d3d9.dll"), "bridge d3d9");
    File.WriteAllText(Path.Combine(top, "RTX Remix bridge", ".trex", "NvRemixBridge.exe"), "bridge server");
    var zip = Path.Combine(work, "Apex-Radiance-9.9.9-Manual-install.zip");
    System.IO.Compression.ZipFile.CreateFromDirectory(Path.Combine(work, "src"), zip);
    var port = new System.Random().Next(42000, 48000);
    using var http = new System.Net.HttpListener();
    http.Prefixes.Add($"http://localhost:{port}/");
    http.Start();
    _ = Task.Run(async () =>
    {
        while (http.IsListening)
        {
            System.Net.HttpListenerContext ctx;
            try { ctx = await http.GetContextAsync(); } catch { break; }
            var bytes = File.ReadAllBytes(zip);
            ctx.Response.ContentLength64 = bytes.Length;
            await ctx.Response.OutputStream.WriteAsync(bytes);
            ctx.Response.Close();
        }
    });
    var rel = new Release("loinyx/Sims3-ApexRadiance", "v9.9.9", new[] {
        new Asset("ApexRadianceSetup.exe", $"http://localhost:{port}/setup", null),
        new Asset("Apex-Radiance-9.9.9-Manual-install.zip", $"http://localhost:{port}/zip", Verify.Sha256Of(zip)) });
    var gs = new GitHubSource { Shown = new Dictionary<string, Release?> { [Releases.Apex] = rel } };
    var zbin = Path.Combine(work, "Game", "Bin");
    Directory.CreateDirectory(zbin);
    var zg = new Game(Path.Combine(work), zbin, "TS3W.exe", Edition.Steam, "1.67.2");
    var (apexFiles, apexVer) = await gs.FetchAsync(Components.Apex, zg, work, default);
    string Body(List<(string, string)> l, string name) => l.FirstOrDefault(f => f.Item2.EndsWith(name)) is { Item1: { } src } ? File.ReadAllText(src) : "<missing>";
    var (wrapFiles, wrapVer) = await gs.FetchAsync(Components.RestartHost, zg, work, default);
    Check(apexVer == "9.9.9" && Body(apexFiles, "ApexRadiance.asi") == "apex 9.9.9" && apexFiles.Count == 1 && wrapVer == "9.9.9" && Body(wrapFiles, "ts3wrap.exe") == "wrap 9.9.9",
          "a release with only Setup and the manual zip: Apex Radiance, and ts3wrap.exe for the restart host, from the zip");
    var (bridgeFiles, _) = await gs.FetchAsync(Components.Bridge, zg, work, default);
    Check(Body(bridgeFiles, "d3d9.dll") == "bridge d3d9" && bridgeFiles.Any(f => f.Item2 == Path.Combine(zbin, ".trex", "NvRemixBridge.exe")),
          "the bridge from the zip's RTX Remix bridge folder, .trex kept, into Game\\Bin");
    http.Stop();
    try { Directory.Delete(work, true); } catch (IOException) { }
}

// Game detection: an EA app install moved out of the EA app's folder, with a stray TS3W.exe beside its TS3.exe 1.69.47,
// is the EA app game (a player's report: Setup took it for Steam and called its TS3.exe bridge.conf wrong)
{
    var bin2 = Path.Combine(Path.GetTempPath(), "ApexSetupMoved-" + Guid.NewGuid().ToString("N")[..8], "The Sims 3", "Game", "Bin");
    Directory.CreateDirectory(bin2);
    static byte[] Pe(uint stamp) { var b = new byte[0x100]; BitConverter.GetBytes(0x80).CopyTo(b, 0x3C); "PE  "u8.ToArray().CopyTo(b, 0x80); BitConverter.GetBytes(stamp).CopyTo(b, 0x88); return b; }
    File.WriteAllBytes(Path.Combine(bin2, "TS3.exe"), Pe(0x6707155C));
    File.WriteAllBytes(Path.Combine(bin2, "TS3W.exe"), Pe(0x12345678));
    var moved = Games.FromFolder(Path.Combine(bin2, "..", ".."));
    Check(moved is { Edition: Edition.EA, Exe: "TS3.exe" }, "a moved EA app install with a stray TS3W.exe is the EA app game");
    File.WriteAllBytes(Path.Combine(bin2, "TS3W.exe"), Pe(0x52DEC247));
    File.Delete(Path.Combine(bin2, "TS3.exe"));
    Check(Games.FromFolder(bin2) is { Edition: Edition.Steam, Exe: "TS3W.exe" }, "a Steam install is still the Steam game");
    Directory.Delete(Path.GetFullPath(Path.Combine(bin2, "..", "..", "..")), true);
}

// A game folder copied elsewhere brings the old setup.json: uninstalling the copy changes only the copy, never the original
{
    var top = Path.Combine(Path.GetTempPath(), "ApexSetupCopy-" + Guid.NewGuid().ToString("N")[..8]);
    string BinOf(string n) => Path.Combine(top, n, "The Sims 3", "Game", "Bin");
    var (orig, copy, docsC) = (BinOf("orig"), BinOf("copy"), Path.Combine(top, "docs"));
    Directory.CreateDirectory(docsC);
    foreach (var b in new[] { orig, copy })
    {
        Directory.CreateDirectory(Manifest.DirOf(b));
        File.WriteAllText(Path.Combine(b, "ApexRadiance.asi"), "apex");
        File.WriteAllText(Path.Combine(b, "d3d9.dll"), "bridge");
        File.WriteAllText(Path.Combine(Manifest.DirOf(b), "Options.ini.bak"), "options");
    }
    var m = new Manifest { OptionsBackup = Path.Combine(Manifest.DirOf(orig), "Options.ini.bak") };
    m.Files.Add(new ManifestFile { Component = Components.Apex, Path = Path.Combine(orig, "ApexRadiance.asi") });
    m.Files.Add(new ManifestFile { Component = Components.Bridge, Path = Path.Combine(orig, "d3d9.dll") });
    m.Versions[Components.Apex] = "3.2.2"; m.Versions[Components.Bridge] = "3.2.2";
    var json = System.Text.Json.JsonSerializer.Serialize(m, SetupJson.Default.Manifest);
    foreach (var b in new[] { orig, copy }) File.WriteAllText(Path.Combine(Manifest.DirOf(b), "setup.json"), json);
    var loaded = Manifest.Load(copy);
    Check(loaded.Files.All(f => f.Path.StartsWith(copy)) && loaded.OptionsBackup == Path.Combine(Manifest.DirOf(copy), "Options.ini.bak"),
        "a copied game's setup.json points at the copy");
    Engine.RemoveAll(new Game(Path.Combine(top, "copy", "The Sims 3"), copy, "TS3W.exe", Edition.Steam, "1.67.2"), docsC);
    Check(!File.Exists(Path.Combine(copy, "ApexRadiance.asi")) && !File.Exists(Path.Combine(copy, "d3d9.dll")), "uninstalling the copy removes its files");
    Check(new[] { Path.Combine(orig, "ApexRadiance.asi"), Path.Combine(orig, "d3d9.dll"), Path.Combine(Manifest.DirOf(orig), "Options.ini.bak") }.All(File.Exists)
        && File.ReadAllText(Path.Combine(Manifest.DirOf(orig), "setup.json")) == json, "uninstalling the copy leaves the original game untouched");
    Directory.Delete(top, true);
}

// Sims 3 Modernization: a release without this edition's build is not taken for it (v0.2.11 had no ts3native-ea169)
{
    var steamOnly = new Release("rodneypantonial/sims3-modernization", "v0.2.11", new[] { new Asset("ts3fixes.package", "x", null), new Asset("ts3native-v0.2.11.zip", "x", null) });
    var both = new Release("rodneypantonial/sims3-modernization", "v0.2.10", new[] { new Asset("ts3fixes.package", "x", null), new Asset("ts3native-v0.2.10.zip", "x", null), new Asset("ts3native-ea169-v0.2.10.zip", "x", null) });
    Check(!Engine.HasModernizationBuild(steamOnly, Edition.EA) && Engine.HasModernizationBuild(steamOnly, Edition.Steam), "a Steam-only Modernization release is no build for the EA app");
    Check(Engine.HasModernizationBuild(both, Edition.EA) && Engine.HasModernizationBuild(both, Edition.Steam), "a release with both zips serves both editions");
    Check(new[] { steamOnly, both }.FirstOrDefault(x => Engine.HasModernizationBuild(x, Edition.EA))?.Tag == "v0.2.10", "the EA app falls back to the newest release with its build");
}

// Setup's own update: only a newer Apex Radiance Setup is taken; the running exe is renamed, not overwritten
{
    var dir = Path.Combine(Path.GetTempPath(), "ApexSetupSelf-" + Guid.NewGuid().ToString("N")[..8]);
    Directory.CreateDirectory(dir);
    var core = typeof(SelfUpdate).Assembly.Location;
    Check(!SelfUpdate.IsNewer(core, core), "a file that is not Apex Radiance Setup is never taken as an update");
    var self = Path.Combine(dir, "ApexRadianceSetup.exe"); var fresh = Path.Combine(dir, "new.exe");
    File.WriteAllText(self, "old setup"); File.WriteAllText(fresh, "new setup");
    SelfUpdate.Replace(fresh, self);
    Check(File.ReadAllText(self) == "new setup" && File.ReadAllText(self + ".old") == "old setup", "an update puts the new exe in place and keeps the old one as .old");
    SelfUpdate.CleanUp(self);
    Check(!File.Exists(self + ".old"), "the .old exe is removed at the next start");
    var none = new Release("loinyx/Sims3-ApexRadiance", "v3.1.4", new[] { new Asset("ApexRadiance.asi", "x", null) });
    Check(await SelfUpdate.DownloadIfNewerAsync(none, self) is null, "a release without ApexRadianceSetup.exe offers no update");
    var unchecked_ = new Release("loinyx/Sims3-ApexRadiance", "v3.1.4", new[] { new Asset("ApexRadianceSetup.exe", "x", null) });
    Check(await SelfUpdate.DownloadIfNewerAsync(unchecked_, self) is null, "an exe without a SHA-256 digest is never downloaded");
    Directory.Delete(dir, true);
}

// Setup's languages: each table loads, its keys are English texts of Setup (the Portuguese table's keys, the complete
// list), with the key's {n} placeholders and no empty text. A key twice in one file fails: the indexer initializer keeps
// the last one without a word, so the file itself is read. Missing keys are only counted (English shows for them).
{
    Check(Strings.Languages.Length == 21 && Strings.Languages[0].Code == "en", "21 languages, English first");
    foreach (var (name, want) in new[] { ("pt-PT", "pt"), ("pt-BR", "pt"), ("zh-CN", "zh_hans"), ("zh-SG", "zh_hans"), ("zh-Hans", "zh_hans"),
                 ("zh-TW", "zh_hant"), ("zh-HK", "zh_hant"), ("zh-MO", "zh_hant"), ("zh-Hant", "zh_hant"), ("nb-NO", "no"), ("nn-NO", "no"),
                 ("de-AT", "de"), ("ja-JP", "ja"), ("th-TH", "th"), ("tr-TR", "en"), ("en-GB", "en") })
        Check(Strings.SystemLanguage(new System.Globalization.CultureInfo(name)) == want, $"Windows in {name} picks {want}");
    var keys = Strings.Table("pt")!.Keys.ToHashSet();
    var dir = Path.Combine(Path.GetDirectoryName(SourceFile())!, "..", "ApexSetup");
    static IEnumerable<string> Holes(string s) => System.Text.RegularExpressions.Regex.Matches(s, @"\{\d+\}").Select(m => m.Value).Distinct().Order();
    foreach (var (code, _) in Strings.Languages.Skip(1))
    {
        Dictionary<string, string>? t;
        try { t = Strings.Table(code); }
        catch (Exception e) { Check(false, $"{code}: the table loads ({e.InnerException?.Message ?? e.Message})"); continue; }
        if (t is null) { Check(false, $"{code}: the table is registered in Languages.cs"); continue; }
        var stray = t.Keys.Where(k => !keys.Contains(k)).ToList();
        Check(stray.Count == 0, $"{code}: every key is a text of Setup" + string.Concat(stray.Select(k => "\n       not a key: " + k)));
        var holes = t.Where(p => !Holes(p.Key).SequenceEqual(Holes(p.Value))).Select(p => p.Key).ToList();
        Check(holes.Count == 0, $"{code}: the key's {{n}} placeholders" + string.Concat(holes.Select(k => "\n       other placeholders: " + k)));
        var empty = t.Where(p => p.Value.Length == 0).Select(p => p.Key).ToList();
        Check(empty.Count == 0, $"{code}: no empty text" + string.Concat(empty.Select(k => "\n       empty: " + k)));
        var dups = System.Text.RegularExpressions.Regex.Matches(File.ReadAllText(Path.Combine(dir, $"Strings.{code}.cs")), @"(?m)^\s*\[""((?:[^""\\]|\\.)*)""\]\s*=")
            .Select(m => System.Text.RegularExpressions.Regex.Unescape(m.Groups[1].Value)).GroupBy(k => k).Where(g => g.Count() > 1).Select(g => g.Key).ToList();
        Check(dups.Count == 0, $"{code}: each key once in Strings.{code}.cs" + string.Concat(dups.Select(k => "\n       twice: " + k)));
        var missing = keys.Count(k => !t.ContainsKey(k));
        Console.WriteLine($"     {code}: {keys.Count - missing}/{keys.Count} translated" + (missing > 0 ? $", {missing} missing (shown in English)" : ""));
    }
}

// a running game keeps ts3native.log open for writing: the scan still reads it
{
    var lb = Path.Combine(Path.GetTempPath(), "ApexSetupLog-" + Guid.NewGuid().ToString("N")[..8], "The Sims 3", "Game", "Bin");
    Directory.CreateDirectory(lb);
    File.WriteAllText(Path.Combine(lb, "TS3W.exe"), "exe");
    File.WriteAllText(Path.Combine(lb, "ts3native.asi"), "asi");
    using (var open = new FileStream(Path.Combine(lb, "ts3native.log"), FileMode.Create, FileAccess.Write, FileShare.Read))
    {
        open.Write(System.Text.Encoding.ASCII.GetBytes("SKIP all: host is not TS3W.exe"));
        open.Flush();
        var gl = new Game(Path.GetDirectoryName(Path.GetDirectoryName(lb))!, lb, "TS3W.exe", Edition.Steam, "1.67.2");
        bool wrongBuild = false;
        try { wrongBuild = Scan.Run(gl, Path.Combine(lb, "docs")).Ts3NativeWrongBuild; } catch (IOException) { }
        Check(wrongBuild, "ts3native.log open in the running game is still read");
    }
    Directory.Delete(Path.GetDirectoryName(Path.GetDirectoryName(Path.GetDirectoryName(lb)))!, true);
}

// The bridge needs Vulkan 1.3: the driver's packed version reads right, and this PC's cards can be asked
Check(Gpu.Decode((1u << 22) | (3u << 12) | 280) == new Version(1, 3) && Gpu.Decode((1u << 22) | (2u << 12) | 170) < Gpu.Needed,
      "Vulkan 1.3 passes and 1.2 does not");
try { var (vk, card, missing) = Gpu.Vulkan(); Console.WriteLine($"info this PC: {card ?? "no Vulkan card"} Vulkan {vk?.ToString() ?? "-"}, DXVK extensions missing: {(missing.Length == 0 ? "none" : string.Join(", ", missing))}"); }
catch (Exception e) { Console.WriteLine("info this PC: Vulkan driver not asked: " + e.Message); }

// NVIDIA Smooth Motion: which card and driver may turn it on, a player's own profile left alone, the dry run writes nothing
{
    SmoothMotion.Status Nv(string? card, uint driver = 57186, bool on = false, string? owner = null) => new(card, driver, on, owner, null);
    Check(SmoothMotion.Supported("NVIDIA GeForce RTX 4070", 57186) && SmoothMotion.Supported("NVIDIA GeForce RTX 5090 Laptop GPU", 59000)
          && !SmoothMotion.Supported("NVIDIA GeForce RTX 3080", 59000) && !SmoothMotion.Supported("NVIDIA GeForce RTX 4070", 57185)
          && !SmoothMotion.Supported(null, 59000), "smooth motion: RTX 40 and 50 on driver 571.86 or newer only");
    Check(SmoothMotion.Blocked(Nv("NVIDIA GeForce RTX 4070"), true, false) is null, "smooth motion: an RTX 40 card under the bridge can turn it on");
    Check(SmoothMotion.Blocked(Nv("NVIDIA GeForce RTX 3080"), true, false)?.Key.StartsWith("Needs an NVIDIA") == true
          && SmoothMotion.Blocked(Nv("NVIDIA GeForce RTX 4070", 56600), true, false)?.Arg == "566.00", "smooth motion: an older card or driver says why");
    Check(SmoothMotion.Blocked(Nv("NVIDIA GeForce RTX 4070"), false, false)?.Key.Contains("RTX Remix bridge") == true
          && SmoothMotion.Blocked(Nv("NVIDIA GeForce RTX 4070", on: true), false, true) is null, "smooth motion: needs the bridge to turn on, never to turn off");
    Check(SmoothMotion.Blocked(Nv("NVIDIA GeForce RTX 4070", on: true), true, false)?.Key.StartsWith("Turned on outside Setup") == true,
          "smooth motion: on in NVIDIA's profile but not by Setup (Profile Inspector) is left alone");
    Check(SmoothMotion.Blocked(Nv("NVIDIA GeForce RTX 4070", owner: "My TS3"), true, false) is { Arg: "My TS3" }, "smooth motion: an exe in the player's own profile is left alone");
    Check(SmoothMotion.Blocked(new SmoothMotion.Status(null, 0, false, null, "no nvapi64.dll"), true, false) is { Arg: "no nvapi64.dll" }, "smooth motion: an NVAPI error reads as a reason");
    Environment.SetEnvironmentVariable("APEX_SETUP_FAKE_NV", "NVIDIA GeForce RTX 4080|57186|1|");
    Check(SmoothMotion.Read() is { Card: "NVIDIA GeForce RTX 4080", Driver: 57186, On: true, Owner: null }, "smooth motion: the fake stands in for the driver");
    Environment.SetEnvironmentVariable("APEX_SETUP_FAKE_NV", null);
    Environment.SetEnvironmentVariable("APEX_SETUP_DRY_RUN", "1");
    Check(SmoothMotion.Set(true) == "Dry run: Setup changes nothing.", "smooth motion: the dry run writes nothing");
    Environment.SetEnvironmentVariable("APEX_SETUP_DRY_RUN", null);
    Check(Engine.WithFpsLimitIfNone("[screen]\nfps_limit = 0\nvsync = 1\n") == ("[screen]\nfps_limit = 60\nvsync = 1\n", 0)
          && Engine.WithFpsLimitIfNone("[screen]\nfps_limit = 144\n") == ("[screen]\nfps_limit = 144\n", 144)
          && Engine.WithFpsLimitIfNone("[screen]\nfps_limit = 30\n") == ("[screen]\nfps_limit = 30\n", 30)
          && Engine.WithFpsLimitIfNone("[ui]\nx = 1\n") == ("[ui]\nx = 1\n\n[screen]\nfps_limit = 60\n", null)
          && Engine.WithFpsLimitIfNone("[screen]\r\nvsync = 1\r\n\r\n[ui]\r\n").Text == "[screen]\r\nvsync = 1\r\nfps_limit = 60\r\n\r\n[ui]\r\n",
          "frame generation: only no frame limit becomes 60 fps, a limit the player chose stays, other keys and CRLF as they were");
    Check(Engine.WithoutFpsLimit("[screen]\r\nvsync = 1\r\nfps_limit = 60\r\n") == "[screen]\r\nvsync = 1\r\n"
          && Engine.WithFpsLimit("[screen]\nfps_limit = 60\n", 0) == "[screen]\nfps_limit = 0\n", "frame generation: the frame limit's undo");
    {
        var smMarker = SmoothMotion.Marker;
        SmoothMotion.Marker = Path.Combine(Path.GetTempPath(), "ApexSetupTest-sm-" + Guid.NewGuid().ToString("N")[..8] + ".txt");
        Check(SmoothMotion.Set(false) is null && !SmoothMotion.SetupTurnedOn, "smooth motion: off without Setup's record touches nothing");
        File.WriteAllText(SmoothMotion.Marker, "on");
        SmoothMotion.Tidy(new("NVIDIA GeForce RTX 4070", 62124, true, null, null));
        var smKept = SmoothMotion.SetupTurnedOn;
        SmoothMotion.Tidy(new("NVIDIA GeForce RTX 4070", 62124, false, null, null));
        Check(smKept && !SmoothMotion.SetupTurnedOn, "smooth motion: Setup's record kept while on, dropped once it reads off");
        SmoothMotion.Marker = smMarker;
    }
    // AMD Fluid Motion Frames: system-wide, so maintenance turns off only what Setup turned on
    Check(Afmf.Blocked(new(true, true, false, null)) is null && Afmf.Blocked(new(true, false, false, null)) is { Key: var k } && k.StartsWith("Not available")
          && Afmf.Blocked(new(true, false, true, null)) is null && Afmf.Blocked(new(true, false, false, "ADLXInitialize 3")) is { Arg: "ADLXInitialize 3" },
          "afmf: on only where ADLX says it is supported; off always; an ADLX error reads as a reason");
    Environment.SetEnvironmentVariable("APEX_SETUP_FAKE_AMD", "1|1");
    Check(Afmf.Read() == new Afmf.Status(true, true, true, null) && Afmf.Set(false) == "Dry run: Setup changes nothing.", "afmf: the fake stands in for ADLX and writes nothing");
    Environment.SetEnvironmentVariable("APEX_SETUP_FAKE_AMD", "none");
    Check(Afmf.Read() is { Present: false }, "afmf: no AMD card, no row");
    var marker = Afmf.Marker;
    Afmf.Marker = Path.Combine(Path.GetTempPath(), "ApexSetupTest-afmf-" + Guid.NewGuid().ToString("N")[..8] + ".txt");
    Check(Afmf.OffIfSetupTurnedOn() is null, "afmf: a setting the player turned on is never turned off by a removal");
    File.WriteAllText(Afmf.Marker, "on");
    Check(Afmf.OffIfSetupTurnedOn() == "Dry run: Setup changes nothing." && File.Exists(Afmf.Marker), "afmf: one Setup turned on is turned off (the fake writes nothing)");
    File.Delete(Afmf.Marker);
    Afmf.Marker = marker;
    Environment.SetEnvironmentVariable("APEX_SETUP_FAKE_AMD", null);
    Environment.SetEnvironmentVariable("APEX_SETUP_DRY_RUN", "1");
    Check(Afmf.Set(true) == "Dry run: Setup changes nothing.", "afmf: the dry run writes nothing");
    Environment.SetEnvironmentVariable("APEX_SETUP_DRY_RUN", null);
    Check(SmoothMotion.Failed(new DllNotFoundException()) is { Card: null, Error: null } && SmoothMotion.Failed(new EntryPointNotFoundException()).Error is null
          && SmoothMotion.Failed(new InvalidOperationException("DRS_LoadSettings -1")).Error == "DRS_LoadSettings -1" && SmoothMotion.NoCard(-6) && !SmoothMotion.NoCard(-1),
          "smooth motion: no NVIDIA driver or device is no card, no warning; other errors show");
    Check(Afmf.Started(0) && Afmf.Started(2) && !Afmf.Started(8) && !Afmf.Started(3), "afmf: ADLX that does not start is no AMD card");
    Afmf.Marker = Path.Combine(Path.GetTempPath(), "ApexSetupTest-afmf-" + Guid.NewGuid().ToString("N")[..8] + ".txt");
    File.WriteAllText(Afmf.Marker, "on");
    Afmf.Tidy(new(true, true, true, null));
    var keptOn = File.Exists(Afmf.Marker);
    Afmf.Tidy(new(true, true, false, "IsEnabled 3"));
    var keptOnError = File.Exists(Afmf.Marker);
    Afmf.Tidy(new(true, true, false, null));
    Check(keptOn && keptOnError && !File.Exists(Afmf.Marker), "afmf: a record left while AFMF is off is dropped");
    Afmf.Marker = marker;
    var amd = Afmf.Read(); // read only
    Console.WriteLine($"info this PC: AMD {(amd.Present ? "driver found" : "none")}{(amd.Error is null ? "" : ", error " + amd.Error)}");
    var nv = SmoothMotion.Read(); // read only
    Console.WriteLine($"info this PC: NVIDIA {nv.Card ?? "none"}, driver {SmoothMotion.DriverText(nv.Driver)}, Smooth Motion {(nv.On ? "on" : "off")}{(nv.Owner is null ? "" : ", in profile " + nv.Owner)}{(nv.Error is null ? "" : ", error " + nv.Error)}");
}

// the generated JSON writes and reads exactly what the reflection serializer did: players' setup.json and restore points
{
    var legacy = new System.Text.Json.JsonSerializerOptions { WriteIndented = true };
    var m = new Manifest { OptionsBackup = "Options.ini.bak" };
    m.Files.Add(new ManifestFile { Component = Components.Bridge, Path = @"C:\Game\Bin\d3d9.dll", Backup = "d3d9.dll.bak" });
    m.Files.Add(new ManifestFile { Component = Components.Apex, Path = @"C:\Game\Bin\ApexRadiance.asi" });
    m.Versions[Components.Apex] = "3.2.3"; m.Versions[Components.Bridge] = "1.0 ü \"quoted\" <tag>";
    var oldManifest = System.Text.Json.JsonSerializer.Serialize(m, legacy);
    Check(oldManifest == System.Text.Json.JsonSerializer.Serialize(m, SetupJson.Default.Manifest), "setup.json is written as before");
    var mBack = System.Text.Json.JsonSerializer.Deserialize(oldManifest, SetupJson.Default.Manifest)!;
    Check(mBack.Files.Count == 2 && mBack.Files[0].Backup == "d3d9.dll.bak" && mBack.Files[1].Backup is null
        && mBack.Versions[Components.Bridge] == m.Versions[Components.Bridge] && mBack.OptionsBackup == "Options.ini.bak", "setup.json written by older Setups reads back");

    var point = new RestorePoint { Created = new DateTime(2026, 11, 14, 12, 0, 0, DateTimeKind.Utc), CreatedLocal = "14/11/2026 09:00",
        Label = "Installed {0}", Args = new[] { "Apex Radiance" }, SetupVersion = "1.1.3", Bin = @"C:\Game\Bin" };
    point.Entries.Add(new RestoreEntry { Path = @"C:\Game\Bin.dll", Existed = true, Copy = "a.dll" });
    point.Entries.Add(new RestoreEntry { Path = @"C:\Game\Bin\Mods", Dir = true });
    var oldPoint = System.Text.Json.JsonSerializer.Serialize(point, legacy);
    Check(oldPoint == System.Text.Json.JsonSerializer.Serialize(point, SetupJson.Default.RestorePoint), "restore points are written as before");
    var pBack = System.Text.Json.JsonSerializer.Deserialize(oldPoint, SetupJson.Default.RestorePoint)!;
    Check(pBack.Created == point.Created && pBack.Args[0] == "Apex Radiance" && pBack.Entries.Count == 2 && pBack.Entries[0].Copy == "a.dll"
        && pBack.Entries[1].Dir && pBack.FileCount == 1, "restore points written by older Setups read back");
}

Console.WriteLine(failures == 0 ? "ALL PASSED" : failures + " FAILED");
return failures == 0 ? 0 : 1;

static string SourceFile([System.Runtime.CompilerServices.CallerFilePath] string path = "") => path;

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
            Components.RestartHost => new() { (Make("ts3wrap.exe", "wrap 3.1.1"), Path.Combine(b, "ts3wrap.exe")) },
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
