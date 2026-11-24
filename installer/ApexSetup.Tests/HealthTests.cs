// Offline checks of the Health analyzers (GameCaches, ScriptErrors, SaveHealth, SessionHistory, PlaceChecks, ConflictCheck)
// on folders made here. APEX_HEALTH_REAL=1 adds a read-only run on the player's real Documents and the game in
// APEX_HEALTH_GAME: it lists, reads and reports, and writes only to the temporary launcher data.
using System.Text;
using ApexSetup;

static class HealthTests
{
    public static void Run(Action<bool, string> check)
    {
        var root = Directory.CreateTempSubdirectory("ApexHealthTest-").FullName;
        var oldLocal = Paths.LocalAppData;
        Paths.LocalAppData = Path.Combine(root, "Local");
        try
        {
            Caches(check, Path.Combine(root, "caches"));
            Errors(check, Path.Combine(root, "errors"));
            SavesCheck(check, Path.Combine(root, "saves"));
            Sessions(check);
            Places(check);
            Conflicts(check, Path.Combine(root, "conflicts"));
            if (Environment.GetEnvironmentVariable("APEX_HEALTH_REAL") == "1") Real();
        }
        catch (Exception e) { check(false, "health: exception " + e); }
        finally
        {
            Games.IsRunning = () => false;
            Paths.LocalAppData = oldLocal;
            try { Directory.Delete(root, true); } catch (Exception) { }
        }
    }

    static void Write(string path, string text)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        File.WriteAllText(path, text);
    }

    static void Caches(Action<bool, string> check, string docs)
    {
        foreach (var n in new[] { "CASPartCache.package", "scriptCache.package", @"WorldCaches\Riverview_objects.package", @"Thumbnails\CASThumbnails.package", @"FeaturedItems\thumb_1.png" })
            Write(Path.Combine(docs, n), "cache " + n);
        Write(Path.Combine(docs, @"DCCache\dcdb0.dbc"), "installed content");
        Write(Path.Combine(docs, "Options.ini"), "options");
        var list = GameCaches.List(docs);
        check(list.Count == 5 && !list.Any(c => c.Rel.StartsWith("DCCache")), "caches: the five caches are listed, DCCache and Options.ini are not");

        Games.IsRunning = () => true;
        var refused = false;
        try { GameCaches.Clean(docs); } catch (SetupException) { refused = true; }
        Games.IsRunning = () => false;
        check(refused && GameCaches.List(docs).Count == 5 && GameCaches.Backup is null, "caches: Clean refuses while the game runs and moves nothing");

        var moved = GameCaches.Clean(docs);
        check(moved > 0 && GameCaches.List(docs).Count == 0 && GameCaches.Backup is not null && File.Exists(Path.Combine(docs, @"DCCache\dcdb0.dbc")),
            "caches: Clean moves them to the launcher's backup and leaves DCCache");
        Write(Path.Combine(docs, "CASPartCache.package"), "rebuilt");
        check(GameCaches.Restore() && File.ReadAllText(Path.Combine(docs, "CASPartCache.package")) == "cache CASPartCache.package"
              && File.Exists(Path.Combine(docs, @"WorldCaches\Riverview_objects.package")) && GameCaches.Backup is null,
            "caches: Restore puts the originals back over the rebuilt ones");

        var logged = new List<string>();
        var oldLog = Engine.Log;
        Engine.Log = e => logged.Add(e.GetType().Name + ": " + e.Message);
        GameCaches.Clean(docs);
        SessionHistory.Add(new(DateTime.Now, TimeSpan.FromMinutes(5), 1L << 30, SessionEnd.Crash, -1));
        var keptAfterCrash = GameCaches.Backup is not null;
        SessionHistory.Add(new(DateTime.Now, TimeSpan.FromMinutes(5), 1L << 30, SessionEnd.Normal, 0));
        Engine.Log = oldLog;
        check(keptAfterCrash && GameCaches.Backup is null, $"caches: the backup is kept after a crash and dropped after a normal game {(keptAfterCrash ? "" : "(not kept)")} {GameCaches.Backup} {string.Join(" | ", logged)}");
        File.Delete(Path.Combine(Paths.Data, "sessions.json"));
    }

    // NRaas ErrorTrap's layout (unescaped <...> inside Content, as the real files have it)
    static string ErrorTrap(params string[] frames) =>
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>\n<NRaas.ErrorTrap>\n<ModVersion value=\"100\"/>\n<Content>\n\n-- Script Errors --\n\n Career: <Unemployed>\n\n"
        + "System.NullReferenceException: A null value was found where an object instance was required.\n"
        + string.Join("\n", frames.Select((f, i) => $"#{i}: 0x0001d callvirt   in {f} () ()")) + "\n\n</Content>\n</NRaas.ErrorTrap>\n";

    static void Errors(Action<bool, string> check, string docs)
    {
        // a script mod whose assembly defines Fixture.Entry
        var pkg = Path.Combine(docs, @"Mods\Packages\fixturemod.package");
        Directory.CreateDirectory(Path.GetDirectoryName(pkg)!);
        File.WriteAllBytes(pkg, ModConflicts.WritePackage([(ModConflicts.TypeS3sa, 0u, 1ul, ModConflicts.S3saWrap(ModConflictsTests.Asm("FixtureMod", Guid.NewGuid(), [])))]));
        var mods = new List<ModFile> { new(pkg, @"Mods\Packages\fixturemod.package", new FileInfo(pkg).Length, ModKind.Script, true, false),
            new(Path.Combine(docs, @"Mods\Packages\SomeMod\SomeMod_Script.package"), @"Mods\Packages\SomeMod\SomeMod_Script.package", 1, ModKind.Script, true, false) };

        Write(Path.Combine(docs, "ScriptError_PC__a.xml"), ErrorTrap("Sims3.Gameplay.Core.Sims3.Gameplay.Core.Lot:Get", "Fixture.Fixture.Entry:Init", "ScriptCore.ScriptCore.ScriptProxy:Simulate"));
        Write(Path.Combine(docs, "ScriptError_PC__b.xml"), ErrorTrap("Sims3.Gameplay.Actors.Sims3.Gameplay.Actors.Sim:DoRoute") + "\n<broken <<< not xml");
        Write(Path.Combine(docs, "SomeMod_Error_2030-01-01_1__x.xml"), "<SomeMod><Line>ERROR\nSystem.InvalidCastException: bad\n#0: 0x0 call in SomeMod.SomeMod.Boot:Run () ()\n</Line></SomeMod>");
        Write(Path.Combine(docs, "xcpt PC 30-01-01 10.00.00.txt"), "[Exception info]\ntype: ACCESS_VIOLATION reading\naddress: 0x7a1ff912 \"X:\\Game\\Bin\\Crashy.asi\":0x0001\n\n[Call stack]\nX:\\Game\\Bin\\TS3W.exe\n0x1\n");
        Write(Path.Combine(docs, "ScriptError_PC__old.xml"), ErrorTrap("Fixture.Fixture.Entry:Init"));
        File.SetLastWriteTime(Path.Combine(docs, "ScriptError_PC__old.xml"), DateTime.Now.AddDays(-30));
        // a huge log: one error at the top, then 6 MB of noise past MaxBytes
        var huge = new StringBuilder(ErrorTrap("Fixture.Fixture.Entry:Init"));
        huge.Append('x', 6 << 20);
        Write(Path.Combine(docs, "ScriptError_PC__huge.xml"), huge.ToString());
        Write(Path.Combine(docs, "ScriptError_PC__empty.xml"), "");
        // a mod since removed: its old logs no longer count
        Write(Path.Combine(docs, "GoneMod_Error_2030-01-01_1__x.xml"), "<GoneMod><Line>ERROR\nSystem.InvalidCastException: bad\n#0: 0x0 call in GoneMod.GoneMod.Boot:Run () ()\n</Line></GoneMod>");

        // an add-on script mod without its base mod (Mono's and the game's assemblies are always there)
        ModFile Script(string name, string[] refs)
        {
            var p = Path.Combine(docs, @"Mods\Packages", name + ".package");
            File.WriteAllBytes(p, ModConflicts.WritePackage([(ModConflicts.TypeS3sa, 0u, 1ul, ModConflicts.S3saWrap(ModConflictsTests.Asm(name, Guid.NewGuid(), [], refs: refs)))]));
            return new(p, @"Mods\Packages\" + name + ".package", new FileInfo(p).Length, ModKind.Script, true, false);
        }
        var addon = Script("AddOnMod", ["BaseMod", "System.Xml", "ScriptCore"]);
        var lost = new MissingAssembly().Check([mods[0], addon], null!, docs).ToList();
        var baseMod = Script("BaseMod", []);
        check(lost.Count == 1 && lost[0].Finding.Args.SequenceEqual(["AddOnMod.package", "BaseMod"]) && lost[0].Files.Single() == addon.Path
              && !new MissingAssembly().Check([addon, baseMod], null!, docs).Any() && new MissingAssembly().Check([addon, baseMod with { Enabled = false }], null!, docs).Count() == 1,
            "mod checks: an add-on whose base assembly is missing or switched off is named; with its base mod installed it is not");
        // a package with two assemblies that both need the missing one: named once
        var twoPath = Path.Combine(docs, @"Mods\Packages", "TwoAsm.package");
        File.WriteAllBytes(twoPath, ModConflicts.WritePackage([(ModConflicts.TypeS3sa, 0u, 1ul, ModConflicts.S3saWrap(ModConflictsTests.Asm("TwoA", Guid.NewGuid(), [], refs: ["BaseMod"]))),
            (ModConflicts.TypeS3sa, 0u, 2ul, ModConflicts.S3saWrap(ModConflictsTests.Asm("TwoB", Guid.NewGuid(), [], refs: ["BaseMod", "OtherBase"])))]));
        var two = new ModFile(twoPath, @"Mods\Packages\TwoAsm.package", new FileInfo(twoPath).Length, ModKind.Script, true, false);
        var twoLost = new MissingAssembly().Check([two], null!, docs).ToList();
        check(twoLost.Count == 1 && twoLost[0].Finding.Args[1] == "BaseMod, OtherBase", "mod checks: a package with several assemblies is named once, with everything it misses");
        File.Delete(twoPath);
        File.Delete(addon.Path);
        File.Delete(baseMod.Path);

        var r = ScriptErrors.Read(docs, DateTime.Now.AddDays(-1), mods);
        var fixture = r.ByMod.FirstOrDefault(m => m.Mod == "fixturemod");
        check(r.Total == 5 && fixture is { Count: 2 } && fixture.Files.SequenceEqual([pkg]),
            "script errors: the frame in a mod's assembly ties the error to its package (malformed, huge and empty logs read without throwing; the old one left out)");
        check(r.ByMod.Any(m => m.Mod is null && m.Count == 1) && r.ByMod.Any(m => m.Mod == "SomeMod") && r.ByMod.Any(m => m.Mod == "Crashy.asi" && m.Crashes == 1),
            "script errors: game-only stacks, a mod's own log and the game's crash log are told apart");
        check(!r.ByMod.Any(m => m.Mod == "GoneMod") && ScriptErrors.Read(docs, DateTime.Now.AddDays(-1), null).ByMod.Any(m => m.Mod == "GoneMod"),
            "script errors: a removed mod's old logs drop out (a crash in something that is no mod, such as Crashy.asi, stays)");
        check(r.Summary.Args[0] == "5" && r.Summary.Args[2] == "fixturemod" && ScriptErrors.Findings(r).Any(f => f.Finding.Args.Contains("fixturemod") && f.Files.Count > 0) && ScriptErrors.Read(docs, DateTime.Now.AddDays(-60), mods).Total == 6,
            "script errors: summary, findings and the time window");
        string Crashes(int n) => ScriptErrors.Findings(r with { ByMod = [r.ByMod.First(m => m.Mod == "Crashy.asi") with { Count = n, Crashes = n }] }).Single().Finding.Severity.ToString();
        // one the player ignored stays ignored whatever its count and date, and no other finding goes with it
        Health.Item Stab(string text, params string[] args) => new(new Finding(Severity.Warning, text, args), HealthArea.Stability, []);
        check(Stab("{0} in {1} since {2}", "3", "Mod", DateTime.Now.ToString("d")).Id == Stab("{0} in {1} since {2}", "12", "Mod", DateTime.Now.AddDays(-3).ToString("g")).Id
              && Stab("{0} in {1}", "3", "Save1").Id != Stab("{0} in {1}", "3", "Save2").Id && (Stab("x") with { Files = ["a1.package"] }).Id != (Stab("x") with { Files = ["a2.package"] }).Id,
              "health: what an ignored finding is known by does not change with counts and dates, and no other goes with it");
        check(Crashes(1) == "Info" && Crashes(2) == "Info" && Crashes(ScriptErrors.CrashPattern) == "Warning",
            $"script errors: one or two crashes in a place are a note, {ScriptErrors.CrashPattern} a warning ({Crashes(1)}, {Crashes(2)}, {Crashes(3)})");
        // a mod that only started the action (a hook over the game's own method, or one far up the stack) is not the cause; the
        // one that called the game code that threw is
        var far = Path.Combine(docs, "ScriptError_PC__far.xml");
        string? Blamed(params string[] frames)
        {
            File.WriteAllText(far, ErrorTrap(frames));
            return ScriptErrors.Read(docs, DateTime.Now.AddDays(-1), mods).Errors.Single(e => e.Log == far).Mod;
        }
        const string Game = "Sims3.Gameplay.Core.Sims3.Gameplay.Core.";
        check(Blamed(Game + "Route:Do", Game + "Route:Single", Game + "Sim:Go", Game + "GoHome:Walk", "Fixture.Fixture.Entry:Run") is null
              && Blamed(Game + "GoHome:RunOrig", "Fixture.Fixture.Entry:Run") is null && Blamed(Game + "GoHome:Run", "Fixture.Fixture.Entry:Run") is null
              && Blamed("System.Collections.System.Collections.List`1:get_Item", "Fixture.Fixture.Entry:Run") == "fixturemod"
              && Blamed(Game + "Household:Add", "Fixture.Fixture.Entry:Run") == "fixturemod" && Blamed(Game + "Other.Entry:Add") is null,
            "script errors: a mod is named when it threw or made the call that threw, not for being further down the stack");
        File.Delete(far);
        // two older crashes, before the window asked for (the last game start) but within the week
        foreach (var n in new[] { 1, 2 })
        {
            var old = Path.Combine(docs, $"xcpt PC 30-01-0{n} 09.00.00.txt");
            File.Copy(Path.Combine(docs, "xcpt PC 30-01-01 10.00.00.txt"), old);
            File.SetLastWriteTime(old, DateTime.Now.AddDays(-2 - n));
        }
        var week = ScriptErrors.Read(docs, DateTime.Now.AddDays(-1), mods);
        check(week.ByMod.Any(m => m.Mod == "Crashy.asi" && m.Crashes == 3) && week.Total == 7 && ScriptErrors.Findings(week).Any(f => f.Finding.Severity == Severity.Warning && f.Finding.Args.Contains("Crashy.asi")),
            $"script errors: the week's crashes count toward the pattern, whatever the script window ({week.Total})");
        // who a crash is put on: a fault in the game called straight from a mod is that mod's; a fault deep in a hook's own code
        // is the hook's, whatever mod sits further down the stack
        static string Xcpt(string fault, params string[] frames) =>
            $"[Exception info]\r\ntype: ACCESS_VIOLATION reading from address 0x00000000 (NULL)\r\naddress: 0x00401000 \"{fault}\":0x0001:0x00001000\r\n\r\n[Call stack]\r\n"
            + string.Concat(frames.Select(x => $"{x}\r\n0x00401000\r\n\r\n")) + "\r\n[Stack data]\r\n";
        const string Exe = @"C:\G\Game\Bin\TS3W.exe", Hooky = @"C:\G\Game\Bin\Hooky.asi", Apex = @"C:\G\Game\Bin\ApexRadiance.asi";
        var called = Path.Combine(docs, "xcpt PC 30-02-01 10.00.00.txt");
        var deep = Path.Combine(docs, "xcpt PC 30-02-01 11.00.00.txt");
        File.WriteAllText(called, Xcpt(Exe, Exe, Hooky, Exe, Apex));
        File.WriteAllText(deep, Xcpt(Apex, Apex, Apex, Apex, Apex, Hooky, Exe));
        var who = ScriptErrors.Read(docs, DateTime.Now.AddDays(-1), mods);
        check(who.Errors.Any(e => e.Log == called && e.Mod == "Hooky.asi" && e.Why == "called" && e.Exception == "TS3W.exe")
              && who.Errors.Any(e => e.Log == deep && e.Mod == "Apex Radiance" && e.Why is null),
            "crashes: a fault in the game called straight from a mod is that mod's; deep in a hook's own code it is the hook's");
        File.Delete(called);
        File.Delete(deep);
        // the timeline: a crash inside a session is that session's end; a freeze with a GPU driver error at that moment is the
        // driver's; a freeze outside any session is an entry of its own
        var t0 = DateTime.Now.AddHours(-5);
        var played = new SessionInfo(t0, TimeSpan.FromMinutes(20), 1L << 30, SessionEnd.Crash, -1073741819);
        var inside = new ScriptErrors.Error(t0.AddMinutes(20), "Hooky.asi", [], "ACCESS_VIOLATION", true, "x");
        var hang = new CrashEvidence.WinEvent(t0.AddHours(2), "hang", "");
        var gpu = new CrashEvidence.WinEvent(t0.AddHours(2).AddSeconds(-20), "gpu", "nvlddmkm 13");
        var tl = CrashTimeline.Build([played], [inside], [hang, gpu], null, docs, null);
        check(tl.Count == 2 && tl[1].End == CrashTimeline.Ending.Crash && tl[1].Culprit == "Hooky.asi" && tl[1].Played == played.Played
              && tl[0].End == CrashTimeline.Ending.Froze && tl[0].Why == "driver" && tl[0].Clues.Any(c => c.Source == "Graphics driver"),
            "timeline: a crash ends its session, a freeze with a GPU driver error is the driver's, with the clues that say so");
        Health.Item Err(string text, Action? fix = null) { var f = new Finding(Severity.Error, text, [], fix); return new(f, Health.AreaOf(f), []); }
        check(Health.StateOf([Err("Apex Radiance is not installed in this game.")]) == HealthState.Blocked && Health.StateOf([Err("No ASI loader in Game\\Bin: x")]) == HealthState.Blocked
            && Health.StateOf([Err("Sims 3 Modernization's ts3native is the build for the other edition and does nothing."), Err("{0} is missing: the RTX Remix bridge does not start.")]) == HealthState.NeedsYou
            && Health.StateOf([Err("{0} in Game\\Bin loads a second copy of Apex Radiance.", () => { })]) == HealthState.NeedsYou,
            "state: blocked only by an unfixable error in Apex Radiance or the ASI loader");
    }

    static void SavesCheck(Action<bool, string> check, string docs)
    {
        var good = Path.Combine(docs, @"Saves\Good.sims3");
        Write(Path.Combine(good, "Meta.data"), "meta");
        Write(Path.Combine(good, "Riverview_0x00000000.nhd"), new string('w', 4000));
        Write(Path.Combine(good, "TravelDB.package"), "travel");
        var bad = Path.Combine(docs, @"Saves\Broken.sims3");
        Write(Path.Combine(bad, "Meta.data"), "meta");
        Write(Path.Combine(bad, "TravelDB.package"), "travel");
        var empty = Path.Combine(docs, @"Saves\Empty.sims3");
        Write(Path.Combine(empty, "Meta.data"), "meta");
        Write(Path.Combine(empty, "Riverview_0x00000000.nhd"), "");

        var r = SaveHealth.Check(docs);
        var g = r.First(x => x.Save.Name == "Good");
        check(r.Count == 3 && g.Incomplete is null && g.Bytes == 4010 && r.First(x => x.Save.Name == "Broken").Incomplete == "no .nhd world file"
              && r.First(x => x.Save.Name == "Empty").Incomplete!.Contains("empty"), "saves: complete, missing .nhd and zero-size parts are told apart");

        var f = SaveHealth.Findings(r, docs);
        check(f.Count(x => x.Text.Contains("incomplete")) == 2 && f.Any(x => x.Text.Contains("no backup") && x.Args[0] == "Good" && x.CanApply), "saves: findings, with a backup offered");
        f.First(x => x.Text.Contains("no backup") && x.Args[0] == "Good").Fix!();
        Write(Path.Combine(good, "Riverview_0x00000000.nhd"), new string('w', 40 << 20));
        var again = SaveHealth.Check(docs).First(x => x.Save.Name == "Good");
        check(again.LastBackup is not null && again.History.Count == 2 && File.Exists(Path.Combine(Paths.Data, "save-sizes.json")),
            "saves: the size history is kept and the launcher's backup is found");
        check(SaveHealth.Findings([again], docs).Any(x => x.Text.Contains("grew")), "saves: fast growth is reported");
    }

    static void Sessions(Action<bool, string> check)
    {
        var t = DateTime.Now.AddDays(-1);
        for (var i = 0; i < 25; i++) SessionHistory.Add(new(t.AddMinutes(i), TimeSpan.FromMinutes(30), (i == 24 ? 3_500L : 1_000L) << 20, i == 24 ? SessionEnd.LowMemory : SessionEnd.Normal, 0));
        var list = SessionHistory.List();
        check(list.Count == SessionHistory.Keep && list[0].Started == t.AddMinutes(24) && list[0].End == SessionEnd.LowMemory && list[^1].Started == t.AddMinutes(5),
            "sessions: the newest 20 are kept, newest first, and read back");
        check(SessionHistory.Check().Any(f => f.Text.Contains("ran out of memory") && f.Args[2] == "3500"), "sessions: a low-memory end is reported with its peak");
        File.WriteAllText(Path.Combine(Paths.Data, "sessions.json"), "{ not json");
        check(SessionHistory.List().Count == 0 && SessionHistory.Check().Count == 0, "sessions: a damaged file reads as no history");
        File.Delete(Path.Combine(Paths.Data, "sessions.json"));
    }

    static void Places(Action<bool, string> check)
    {
        var old = PlaceChecks.OneDriveRoots;
        PlaceChecks.OneDriveRoots = () => [@"D:\Sync\Cloud"];
        try
        {
            check(PlaceChecks.InOneDrive(@"D:\Sync\Cloud\Documents\Electronic Arts\The Sims 3") && PlaceChecks.InOneDrive(@"E:\OneDrive - Work\Documents")
                  && !PlaceChecks.InOneDrive(@"D:\Sync\CloudOther\Documents") && !PlaceChecks.InOneDrive(@"D:\Documents\Electronic Arts"),
                "places: Documents in OneDrive is detected by its root or its folder name");
            var g = new Game(@"D:\Games\The Sims 3", @"D:\Games\The Sims 3\Game\Bin", "TS3W.exe", Edition.Steam, "1.67.2");
            check(PlaceChecks.Check(g, @"D:\Sync\Cloud\Docs").Any(f => f.Text.Contains("OneDrive")), "places: the OneDrive finding");
            var unc = g with { Root = @"\\server\share\The Sims 3" };
            check(PlaceChecks.Check(unc, @"D:\Docs").Any(f => f.Text.Contains("network")), "places: a game on a network share is reported");
        }
        finally { PlaceChecks.OneDriveRoots = old; }
    }

    static void Conflicts(Action<bool, string> check, string root)
    {
        var docs = Path.Combine(root, "Docs");
        var bin = Path.Combine(root, @"The Sims 3\Game\Bin");
        Directory.CreateDirectory(bin);
        File.WriteAllText(Path.Combine(bin, "TS3W.exe"), "exe");
        var g = new Game(Path.Combine(root, "The Sims 3"), bin, "TS3W.exe", Edition.Steam, "1.67.2");
        var packages = Path.Combine(docs, @"Mods\Packages");
        Directory.CreateDirectory(packages);
        byte[] Res(ulong inst, string data) => ModConflicts.WritePackage([(0x0333406Cu, 0u, inst, Encoding.UTF8.GetBytes(data))]);
        File.WriteAllBytes(Path.Combine(packages, "alpha.package"), Res(1, "alpha tuning"));
        File.WriteAllBytes(Path.Combine(packages, "beta.package"), Res(1, "beta tuning"));
        File.WriteAllBytes(Path.Combine(packages, "copy1.package"), Res(2, "same"));
        File.WriteAllBytes(Path.Combine(packages, "copy2.package"), Res(2, "same"));
        List<ModFile> Mods() => Directory.EnumerateFiles(packages).Select(p => new ModFile(p, Path.Combine("Mods", "Packages", Path.GetFileName(p)), new FileInfo(p).Length, ModKind.Package, true, false)).ToList();

        var c = new ConflictCheck { Offline = true };
        var f = c.Check(Mods(), g, docs).ToList();
        var conflict = f.FirstOrDefault(x => x.Finding.Severity == Severity.Warning);
        check(c.Scans == 1 && conflict is not null && conflict.Files.Count == 2 && conflict.Files.All(p => p.EndsWith("alpha.package") || p.EndsWith("beta.package"))
              && conflict.Finding.Args.Length == 2, "conflicts: two mods with one key and different bytes are a Warning tied to both files, with the likely winner");
        check(c.Duplicates.Count == 1 && c.Duplicates[0].Parties.OrderBy(x => x).SequenceEqual(["copy1", "copy2"]) && f.All(x => x.Finding.Args.All(a => !a.Contains("copy1"))),
            "conflicts: identical copies go to Duplicates, not to findings");

        var only = c.Found.Single();
        check(only is { Level: ConflictLevel.Look, Files.Length: 2, SayArgs.Length: 1 } && !new ConflictCheck { Offline = true, Ignored = [only.Id] }.Check(Mods(), g, docs).Any(x => x.Finding.Severity == Severity.Warning),
            "conflicts: a game data conflict is one to look at, and an ignored one warns no more");
        ModConflict Kind(string kind, string text, params string[] parties) => ModConflict.Of(new ConflictEntry(kind, text, parties, null), []);
        var debug = Kind("interaction", "2 mods replace Sims3.Gameplay.Abstracts.GameObject+DEBUG_Clone (store its Singleton): the last one to run wins", "icarusallsorts.Transmogrifier", "S3DecoratingTools_V.1.2");
        check(debug is { Level: ConflictLevel.Small, Finding: null } && Kind("assembly", "x", "a", "b").Level == ConflictLevel.Fix && Kind("core mod", "x", "a", "b").Level == ConflictLevel.Fix
              && Kind("resource key", "3 shared keys with different content (STBL 2, _IMG 1; 4 identical): x", "a", "b").Level == ConflictLevel.Small
              && Kind("resource key", "3 shared keys with different content (STBL 2, _XML 1): x", "a", "b").Level == ConflictLevel.Look, "conflicts: the level follows what is at stake");
        check(debug.Id == Kind("interaction", "2 mods replace Sims3.Gameplay.Abstracts.GameObject+DEBUG_Clone (store its Singleton): x", "S3DecoratingTools_V.1.3", "icarusallsorts.Transmogrifier").Id
              && debug.Id != Kind("interaction", "2 mods replace Other+Thing (store its Singleton): x", "icarusallsorts.Transmogrifier", "S3DecoratingTools_V.1.2").Id,
            "conflicts: an ignored conflict is the same one after a mod's update, and no other");
        var backup = Path.Combine(Manifest.DirOf(bin), "restore", "p");
        Directory.CreateDirectory(backup);
        File.WriteAllBytes(Path.Combine(backup, "0-Old.asi"), [1, 2]);
        File.WriteAllBytes(Path.Combine(bin, "Real.asi"), [1, 2]);
        check(ModConflicts.Scan(new ModConflicts.Options { Bin = bin }).Summary["asi"] == 1, "conflicts: the plugins the launcher's restore points keep are not mods");
        File.Delete(Path.Combine(bin, "Real.asi"));
        Directory.Delete(Manifest.DirOf(bin), true);

        var again = new ConflictCheck { Offline = true };
        again.Check(Mods(), g, docs).ToList();
        check(again.Scans == 0 && again.Duplicates.Count == 1, "conflicts: an unchanged Mods folder reuses the kept result");
        File.SetLastWriteTimeUtc(Path.Combine(packages, "beta.package"), DateTime.UtcNow.AddMinutes(1));
        again.Check(Mods(), g, docs).ToList();
        check(again.Scans == 1, "conflicts: a changed file scans again");

        var s = Scan.Run(g, docs);
        var broken = new ConflictCheck { Offline = true, Scan = _ => throw new InvalidOperationException("broken scan") };
        File.Delete(Path.Combine(Paths.Data, "conflicts.json"));
        var items = Health.Gather(g, s, docs, null, Mods(), broken);
        check(items.Count > 0 && items.Any(i => i.Area == HealthArea.Saves || i.Area == HealthArea.Disk || i.Finding.Text.Contains("not installed")) && !items.Any(i => i.Finding.Text.Contains("likely wins")),
            "conflicts: a scan that throws leaves the other Health checks in place");
        var counting = new ConflictCheck { Offline = true, Scan = o => throw new InvalidOperationException("Play ran the conflict scan") };
        var play = Health.Gather(g, s, docs, null, Mods(), counting, beforePlay: true);
        var full = items.Where(i => i.Area is not (HealthArea.Mods or HealthArea.Saves or HealthArea.Disk or HealthArea.Stability)).Select(i => i.Finding.Text).ToList();
        check(play.All(i => i.Area is not (HealthArea.Saves or HealthArea.Disk or HealthArea.Stability)) && counting.Scans == 0
              && play.Select(i => i.Finding.Text).Where(t => full.Contains(t)).Count() == full.Count && play.Where(Health.StopsApex).Count() == items.Where(Health.StopsApex).Count(),
            "health: the check before Play has the files and compatibility findings only, no conflict scan, and the same blockers");
        var (log, failed) = (Engine.Log, 0);
        Engine.Log = _ => failed++;
        List<Health.Item> rest;
        try { rest = Health.Gather(null!, s, docs, null); } finally { Engine.Log = log; }
        check(failed > 0 && rest.Count > 0, $"health: a file or compatibility check that throws is logged and the others still run ({failed} failed, {rest.Count} items)");
        var slow = new ConflictCheck { Offline = true, Timeout = TimeSpan.FromMilliseconds(100), Scan = o => { Thread.Sleep(1500); return ModConflicts.Scan(o); } };
        var t0 = DateTime.Now;
        check(!slow.Check(Mods(), g, docs).Any() && DateTime.Now - t0 < TimeSpan.FromSeconds(1.4), "conflicts: a scan over its time limit returns nothing at once");
        Thread.Sleep(2000);
        var warm = new ConflictCheck { Offline = true };
        warm.Check(Mods(), g, docs).ToList();
        check(warm.Scans == 0, "conflicts: the slow scan finished in the background and is reused");
        File.Delete(Path.Combine(Paths.Data, "conflicts.json"));
        var (one, two) = (new ConflictCheck { Offline = true, Scan = o => { Thread.Sleep(500); return ModConflicts.Scan(o); } }, new ConflictCheck { Offline = true });
        Parallel.Invoke(() => one.Check(Mods(), g, docs).ToList(), () => { Thread.Sleep(100); two.Check(Mods(), g, docs).ToList(); });
        check(one.Scans + two.Scans == 1 && two.Last is not null, $"conflicts: a second check while one scans waits on it, no second scan ({one.Scans} + {two.Scans})");
    }

    // read-only on the player's folders: what each analyzer finds
    static void Real()
    {
        var docs = Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments), "Electronic Arts", "The Sims 3");
        var g = Games.FromFolder(Environment.GetEnvironmentVariable("APEX_HEALTH_GAME") ?? "") ?? Games.Detect().FirstOrDefault();
        Console.WriteLine($"REAL docs={docs} game={g?.Root}");
        if (System.Diagnostics.Process.GetProcessesByName("TS3W").Length + System.Diagnostics.Process.GetProcessesByName("TS3").Length > 0) { Console.WriteLine("REAL skipped: the game is running"); return; }
        var caches = GameCaches.List(docs);
        Console.WriteLine($"REAL caches: {caches.Count} files, {caches.Sum(c => c.Size) >> 20} MB");
        foreach (var grp in caches.GroupBy(c => Path.GetDirectoryName(c.Rel) is { Length: > 0 } d ? d : c.Rel)) Console.WriteLine($"  {grp.Key}: {grp.Count()} files {grp.Sum(c => c.Size) >> 20} MB");
        foreach (var s in SaveHealth.Check(docs, record: false))
            Console.WriteLine($"REAL save {s.Save.Name}: {s.Bytes >> 20} MB, incomplete={s.Incomplete ?? "no"}, last backup={s.LastBackup?.ToString("g") ?? "none"}");
        foreach (var f in SaveHealth.Findings(SaveHealth.Check(docs, record: false), docs)) Console.WriteLine("  finding: " + string.Format(f.Text, f.Args));
        var mods = g is null ? null : Mods.List(g, docs, kinds: false);
        var r = ScriptErrors.Read(docs, DateTime.Now.AddDays(-30), mods);
        Console.WriteLine($"REAL script errors (30 days): {r.Total}; " + string.Format(r.Summary.Text, r.Summary.Args));
        foreach (var m in r.ByMod) Console.WriteLine($"  {m.Mod ?? "(game)"}: {m.Count} ({m.Crashes} crashes), newest {m.Newest:g}, {m.Exception}, files {string.Join(", ", m.Files.Select(Path.GetFileName))}");
        Paths.LocalAppData = Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData); // (the player's own session record)
        var week = ScriptErrors.Read(docs, DateTime.Now.AddDays(-7), mods);
        foreach (var e in CrashTimeline.Build(SessionHistory.List(), week.Errors, CrashEvidence.Windows(DateTime.Now.AddDays(-7)), ScriptErrors.ApexReport(docs), docs, g?.Bin))
        {
            Console.WriteLine($"REAL timeline {e.When:g} x{e.Count} {e.End} culprit={e.Culprit ?? "-"} why={e.Why ?? "-"} into={e.Into ?? "-"} earlier={e.Earlier} renderer={e.Renderer} peak={e.Peak >> 20}MB");
            foreach (var cl in e.Clues) Console.WriteLine($"    [{cl.Source}] " + string.Format(cl.Text, cl.Args));
        }
        if (g is null) return;
        foreach (var f in PlaceChecks.Check(g, docs)) Console.WriteLine("REAL place: " + string.Format(f.Text, f.Args));
        Console.WriteLine($"REAL places: OneDrive={PlaceChecks.InOneDrive(docs)}");
        var c = new ConflictCheck { Offline = true, Timeout = TimeSpan.FromMinutes(5) };
        var t0 = DateTime.Now;
        var found = c.Check(mods!, g, docs).ToList();
        Console.WriteLine($"REAL conflicts: {mods!.Count} mod files, {(DateTime.Now - t0).TotalSeconds:0.0} s, {found.Count(x => x.Finding.Severity == Severity.Warning)} conflicts, {found.Count(x => x.Finding.Severity == Severity.Info)} overlaps/info, {c.Duplicates.Count} duplicates, 7-Zip needed={c.Last?.NeedsSevenZip}");
        foreach (var x in found.Take(12)) Console.WriteLine("  " + string.Format(x.Finding.Text, x.Finding.Args) + $" [{x.Files.Count} files]");
        foreach (var d in c.Duplicates.Take(6)) Console.WriteLine("  duplicate: " + d.Text);
        var t1 = DateTime.Now;
        new ConflictCheck { Offline = true }.Check(mods, g, docs).ToList();
        Console.WriteLine($"REAL conflicts rerun (cache): {(DateTime.Now - t1).TotalSeconds:0.00} s");
    }
}
