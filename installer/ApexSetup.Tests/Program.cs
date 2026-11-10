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
    Check(Read(B("d3d9.dll")) == "bridge client", "bridge replaced DXVK");
    await Engine.InstallAsync(g, new[] { Components.Bridge }, src, null, docs); // repair keeps the first backup
    Engine.Remove(g, Components.Bridge, docs);
    Check(Read(B("d3d9.dll")) == "... DXVK v2.7 ..." && Read(B("dxvk.conf")) == "user dxvk conf", "DXVK and its dxvk.conf restored after repair + remove");

    // 4. update over a hand-installed older Apex, a leftover combined build, S3SS and sims3-modernization
    File.WriteAllText(B("ApexRadiance.asi"), "apex 3.0.0");
    var m0 = Manifest.Load(bin); m0.Files.RemoveAll(f => f.Component == Components.Apex); m0.Save(); // as if copied by hand
    File.WriteAllText(B("S3SSApex.asi"), "old");
    Check(Scan.Run(g, docs).Problems.Count == 1, "leftover combined build reported");
    await Engine.InstallAsync(g, new[] { Components.Apex, Components.S3ss, Components.Modernization }, src, null, docs);
    Check(Read(B("ApexRadiance.asi")) == "apex 3.1.1" && !File.Exists(Path.Combine(Manifest.DirOf(bin), "ApexRadiance.asi.bak")), "update replaces Apex without a backup");
    Check(!File.Exists(B("S3SSApex.asi")), "leftover combined build moved away");
    var s4 = Scan.Run(g, docs);
    Check(s4.S3ss && s4.Ts3Native && s4.Ts3Fixes && s4.Installed(Components.Modernization) == "0.2.10", "S3SS and sims3-modernization detected with versions");
    Check(File.Exists(Path.Combine(docs, "Mods", "Resource.cfg")), "Resource.cfg created");

    // 5. wrong ts3native build is reported
    File.WriteAllText(B("ts3native.log"), "SKIP all: host is not TS3W.exe at 0x400000");
    Check(Scan.Run(g, docs).Ts3NativeWrongBuild, "wrong ts3native build detected");

    // 6. uninstall everything
    Engine.RemoveAll(g, docs);
    var left = Directory.EnumerateFiles(bin, "*", SearchOption.AllDirectories).Select(f => Path.GetRelativePath(bin, f)).OrderBy(x => x).ToList();
    Check(!left.Contains("ApexRadiance.asi") && !left.Contains("Sims3SettingsSetter.asi") && !left.Contains("ts3native.asi") && !left.Contains("wininet.dll"),
          "uninstall removes everything Setup added (left: " + string.Join(", ", left) + ")");
    Check(Read(B("d3d9.dll")) == "... DXVK v2.7 ...", "user's DXVK untouched by uninstall");
    Check(!File.Exists(Path.Combine(Scan.Packages(docs), "ts3fixes.package")), "ts3fixes.package removed");
}
catch (Exception e) { Console.WriteLine("FAIL exception: " + e); failures++; }
finally { Directory.Delete(root, true); }

Console.WriteLine(failures == 0 ? "ALL PASSED" : failures + " FAILED");
return failures == 0 ? 0 : 1;

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
            Components.S3ss => new() { (Make("Sims3SettingsSetter.asi", "s3ss"), Path.Combine(b, "Sims3SettingsSetter.asi")) },
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
        Make(@"bridge\d3d9.dll", "bridge client");
        Make(@"bridge\bridge.conf", conf);
        Make(@"bridge\dxvk.conf", "d3d9.customDeviceId = 1080");
        Make(@"bridge\.trex\bridge.conf", conf);
        Make(@"bridge\.trex\NvRemixBridge.exe", "server");
        Make(@"bridge\licenses\DXVK-LICENSE.txt", "zlib");
        var l = new List<(string, string)>();
        GitHubSource.AddTree(l, dir, b);
        return l;
    }
}
