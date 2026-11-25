using System.IO.Compression;
using ApexSetup;

// The Mods screen's helpers: the problem-mod hunt (every answer path, Stop, a restart in the middle), folders moved into
// folders, files added (packages, zips, unsafe names), duplicates found by content, and the remembered view.
static class ModToolsTests
{
    public static void Run(Action<bool, string> check)
    {
        var root = Path.Combine(Path.GetTempPath(), "apex-modtools-" + Guid.NewGuid().ToString("N"));
        var oldLocal = Paths.LocalAppData;
        try
        {
            Paths.LocalAppData = Path.Combine(root, "Local");
            ModNotes.Reload();
            var docs = Path.Combine(root, "Docs");
            var mods = Path.Combine(docs, "Mods");
            var game = MakeGame(root);
            Directory.CreateDirectory(Path.Combine(mods, "Packages"));
            File.WriteAllText(Path.Combine(mods, "Resource.cfg"), "Priority 500\nPackedFile Packages/*.package\nPackedFile Packages/*/*.package\nPackedFile Packages/*/*/*.package\n");
            for (int i = 0; i < 8; i++) Write(Path.Combine(mods, "Packages", $"mod{i}.package"), $"content {i}");
            Write(Path.Combine(mods, "Packages", "already-off.package"), "off");
            var list = () => Mods.List(game, docs, kinds: false);
            Mods.SetEnabled(list().Single(m => m.Name == "already-off.package"), false, docs);
            bool On(string name) => list().Single(m => m.Name == name).Enabled;
            // (the ticked files, or every one that can be added)
            List<AddItem> AddTo(string folder, IEnumerable<string> files, bool all = false)
            {
                using var add = ModAdd.Open(game, docs, folder, files, list());
                foreach (var i in add.Items.Where(i => i.Ok && (i.Add || all))) add.Add(i);
                return add.Items;
            }
            string[] Names() => list().Where(m => m.FileType == "package").Select(m => m.Name).Order().ToArray();
            var before = list().ToDictionary(m => m.Name, m => m.Enabled);

            // the hunt: the culprit is mod5; answers follow whether mod5 is on
            var st = ModHunt.Start(list(), docs);
            var rounds = 0;
            while (st is { Suspects.Count: > 1 } && rounds++ < 10)
                st = ModHunt.Answer(stillThere: On("mod5.package"), list(), docs)!;
            check(ModHunt.Found(st!) == Path.Combine("Mods", "Packages", "mod5.package") && rounds <= 3
                && list().All(m => before[m.Name] == m.Enabled) && !On("already-off.package"),
                "hunt: 8 mods found in 3 rounds, every mod back as it was, the one already off stays off");
            ModHunt.Forget();

            // a hunt stopped half way, and one left by a launcher that closed in the middle
            ModHunt.Start(list(), docs);
            var offInRound = list().Count(m => !m.Enabled);
            ModHunt.Stop(list(), docs);
            check(offInRound > 1 && list().All(m => before[m.Name] == m.Enabled) && ModHunt.Current() is null, "hunt: Stop puts every mod back and ends it");
            ModHunt.Start(list(), docs);
            var reread = ModHunt.Current();
            ModHunt.Stop(list(), docs);
            check(reread is { Round: 1 } && reread.Before.Count == 9 && list().All(m => before[m.Name] == m.Enabled), "hunt: its state is read back from disk (a restart in the middle)");
            static bool Refused(Action a) { try { a(); return false; } catch (SetupException) { return true; } }
            ModHunt.Start(list(), docs);
            check(Refused(() => ModHunt.Start(list(), docs)), "hunt: a second hunt is refused while one is going");
            ModHunt.Stop(list(), docs);

            // a mod held open by another program during a switch: the mods already switched go back and the round is not
            // kept, at the start and at an answer (the next answer is never read against an Off half that did not happen)
            var oldDelay = Fs.RetryDelay;
            Fs.RetryDelay = TimeSpan.FromMilliseconds(1);
            static bool Held(string path, Action a)
            {
                using var h = File.Open(path, FileMode.Open, FileAccess.Read, FileShare.None);
                try { a(); return false; } catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return true; }
            }
            var heldStart = Held(list().Single(m => m.Name == "mod1.package").Path, () => ModHunt.Start(list(), docs));
            var afterStart = ModHunt.Current() is null && list().All(m => before[m.Name] == m.Enabled);
            ModHunt.Start(list(), docs);
            var round1 = list().ToDictionary(m => m.Name, m => m.Enabled);
            var off1 = ModHunt.Current()!.Off;
            var heldAnswer = Held(list().Single(m => m.Name == "mod5.package").Path, () => ModHunt.Answer(stillThere: true, list(), docs));
            var kept = ModHunt.Current();
            check(heldStart && afterStart && heldAnswer && kept is { Round: 1 } && kept.Off.SequenceEqual(off1) && list().All(m => round1[m.Name] == m.Enabled),
                "hunt: a mod held open by another program undoes that round's switches, at the start and at an answer");
            ModHunt.Stop(list(), docs);
            Fs.RetryDelay = oldDelay;
            check(list().All(m => before[m.Name] == m.Enabled) && ModHunt.Current() is null, "hunt: Stop after a held mod puts every mod back");

            // "the answers did not add up": still there with every suspect off this round leaves none
            ModHunt.Start(list(), docs);
            var end = ModHunt.Answer(stillThere: false, list(), docs);
            while (end is { Suspects.Count: > 1 }) end = ModHunt.Answer(stillThere: false, list(), docs);
            check(end is not null && list().All(m => before[m.Name] == m.Enabled), "hunt: always gone ends on one of the first halves, mods back");
            ModHunt.Forget();

            // an on and an off mod never get one place (one Rel, so one set of notes): a move or an add onto a name the other
            // side has gets " (2)", a rename onto it is refused; a pair copied there by hand is left alone by the hunt
            var offRoot = Mods.Disabled(docs);
            Write(Path.Combine(mods, "Packages", "C", "same.package"), "on one");
            Write(Path.Combine(offRoot, "Packages", "D", "same.package"), "off one");
            var onRel = Path.Combine("Mods", "Packages", "C", "same.package");
            ModNotes.SetNotes(onRel, "on note");
            var movedOff = ModLibrary.Move(list().Single(m => m.Name == "same.package" && !m.Enabled), docs, Path.Combine("Packages", "C"));
            var onOne = list().Single(m => m.Rel == onRel);
            var dropSame = Path.Combine(root, "drop-same", "same.package");
            Directory.CreateDirectory(Path.GetDirectoryName(dropSame)!);
            File.WriteAllBytes(dropSame, Dbpf20(9));
            var addedSame = AddTo(Path.Combine("Packages", "C"), [dropSame]);
            check(movedOff is { Name: "same (2).package", Enabled: false } && File.Exists(Path.Combine(offRoot, "Packages", "C", "same (2).package"))
                && ModNotes.Get(onRel)?.Notes == "on note" && Refused(() => ModLibrary.Rename(onOne, "same (2)")) && File.Exists(onOne.Path)
                && addedSame.Single().Added!.EndsWith("same (3).package"),
                "names: a move or an add onto a turned-off mod's name gets \" (2)\" and a rename onto it is refused, the notes stay with their mod");
            Write(Path.Combine(mods, "Packages", "twin.package"), "twin on");
            Write(Path.Combine(offRoot, "Packages", "twin.package"), "twin off");
            var twinRel = Path.Combine("Mods", "Packages", "twin.package");
            var twinHunt = ModHunt.Start(list(), docs);
            ModHunt.Stop(list(), docs);
            check(!twinHunt.Before.ContainsKey(twinRel) && !twinHunt.Suspects.Contains(twinRel) && File.Exists(Path.Combine(mods, "Packages", "twin.package"))
                && File.Exists(Path.Combine(offRoot, "Packages", "twin.package")) && list().Where(m => before.ContainsKey(m.Name)).All(m => before[m.Name] == m.Enabled),
                "hunt: an on and an off mod at one place are left alone, the hunt still runs");
            foreach (var d in new[] { Path.Combine(mods, "Packages", "C"), Path.Combine(offRoot, "Packages", "C"), Path.Combine(offRoot, "Packages", "D") }) Directory.Delete(d, true);
            File.Delete(Path.Combine(mods, "Packages", "twin.package"));
            File.Delete(Path.Combine(offRoot, "Packages", "twin.package"));
            ModNotes.Forget(onRel);

            // folders moved into folders, with notes following and nesting refused
            ModLibrary.CreateFolder(docs, "Packages", "A");
            ModLibrary.CreateFolder(docs, "Packages", "B");
            Write(Path.Combine(mods, "Packages", "A", "inA.package"), "a");
            ModNotes.SetNotes(Path.Combine("Mods", "Packages", "A", "inA.package"), "note");
            var to = ModLibrary.MoveFolder(docs, Path.Combine("Packages", "A"), Path.Combine("Packages", "B"));
            ModNotes.Reload();
            check(to == Path.Combine("Packages", "B", "A") && File.Exists(Path.Combine(mods, "Packages", "B", "A", "inA.package"))
                && ModNotes.Get(Path.Combine("Mods", "Packages", "B", "A", "inA.package"))?.Notes == "note"
                && Refused(() => ModLibrary.MoveFolder(docs, Path.Combine("Packages", "B"), Path.Combine("Packages", "B", "A"))),
                "folders: moved into another with its notes; into itself refused");

            // a folder and its switched-off copy move both or neither: a turned-off folder of the new name, or a file held
            // open in the copy, leaves both where they were
            Write(Path.Combine(mods, "Packages", "E", "e.package"), "e");
            Mods.SetEnabled(list().Single(m => m.Name == "e.package"), false, docs);
            Write(Path.Combine(mods, "Packages", "E", "e2.package"), "e2");
            var offE = Path.Combine(Mods.Disabled(docs), "Packages", "E", "e.package");
            Write(Path.Combine(Mods.Disabled(docs), "Packages", "F", "f.package"), "f");
            var renameRefused = Refused(() => ModLibrary.RenameFolder(docs, Path.Combine("Packages", "E"), "F"));
            bool heldBack;
            using (File.Open(offE, FileMode.Open, FileAccess.Read, FileShare.None))
                try { ModLibrary.MoveFolder(docs, Path.Combine("Packages", "E"), Path.Combine("Packages", "B")); heldBack = false; }
                catch (Exception e) when (e is IOException or UnauthorizedAccessException) { heldBack = true; }
            check(renameRefused && heldBack && File.Exists(Path.Combine(mods, "Packages", "E", "e2.package")) && File.Exists(offE)
                && !Directory.Exists(Path.Combine(mods, "Packages", "F")) && !Directory.Exists(Path.Combine(mods, "Packages", "B", "E")),
                "folders: a rename onto a turned-off folder's name is refused; a move whose turned-off copy can't follow is undone");
            foreach (var d in new[] { Path.Combine(mods, "Packages", "E"), Path.Combine(Mods.Disabled(docs), "Packages", "E"), Path.Combine(Mods.Disabled(docs), "Packages", "F") }) Directory.Delete(d, true);

            // a folder holding a file the launcher installed stays; one Resource.cfg loads stays where it loads; and turning a
            // mod off is refused when Resource.cfg also reads the switched-off folder
            Write(Path.Combine(mods, "Packages", "G", "managed.package"), "g");
            var lockedRefused = Refused(() => ModLibrary.RenameFolder(docs, Path.Combine("Packages", "G"), "H", [Path.Combine(mods, "Packages", "G", "managed.package")]));
            var cfg = Path.Combine(mods, "Resource.cfg");
            var cfgBefore = File.Exists(cfg) ? File.ReadAllText(cfg) : null;
            File.WriteAllText(cfg, "PackedFile Packages/*.package\r\nPackedFile Packages/*/*.package\r\n");
            var deepRefused = Refused(() => ModLibrary.MoveFolder(docs, Path.Combine("Packages", "G"), Path.Combine("Packages", "B")));
            File.WriteAllText(cfg, "PackedFile */*.package\r\nPackedFile */*/*.package\r\nPackedFile */*/*/*.package\r\n");
            var offRefused = Refused(() => Mods.SetEnabled(list().Single(m => m.Name == "managed.package"), false, docs));
            if (cfgBefore is null) File.Delete(cfg); else File.WriteAllText(cfg, cfgBefore);
            check(lockedRefused && deepRefused && offRefused && File.Exists(Path.Combine(mods, "Packages", "G", "managed.package")),
                "folders: a launcher file's folder, a move out of Resource.cfg's reach and a switch-off it would still load are refused");
            Directory.Delete(Path.Combine(mods, "Packages", "G"), true);

            // files added, every one checked first: real packages go in, a zip's by name only; a Sims 4 package, a protected
            // one, a cut-short one, an empty one, text named .package, a Sims3Pack and a file already installed are not added,
            // each with its reason
            var drop = Path.Combine(root, "drop");
            Directory.CreateDirectory(drop);
            File.WriteAllBytes(Path.Combine(drop, "new.package"), Dbpf20(1));
            File.WriteAllBytes(Path.Combine(drop, "house.sims3pack"), [7, 0, 0, 0, .. "TS3Pack"u8.ToArray(), 1, 2, 3]);
            Write(Path.Combine(drop, "readme.txt"), "r");
            Write(Path.Combine(drop, "text.package"), "just text, not a package at all, but long enough to have a header of ninety six bytes ................");
            var ts4 = Dbpf20(2); ts4[8] = 1; File.WriteAllBytes(Path.Combine(drop, "sims4.package"), ts4);
            var prot = Dbpf20(3); prot[3] = (byte)'P'; File.WriteAllBytes(Path.Combine(drop, "protected.package"), prot);
            var cut = Dbpf20(4); File.WriteAllBytes(Path.Combine(drop, "cut.package"), cut[..(cut.Length - 40)]);
            var empty = Dbpf20(5); BitConverter.GetBytes(0u).CopyTo(empty, 0x24); File.WriteAllBytes(Path.Combine(drop, "empty.package"), empty);
            var zip = Path.Combine(drop, "set.zip");
            using (var z = ZipFile.Open(zip, ZipArchiveMode.Create))
            {
                using (var w = z.CreateEntry("set/inner.package").Open()) w.Write(Dbpf20(6));
                using (var w = z.CreateEntry("../../evil.package").Open()) w.Write(Dbpf20(7));
                using (var w = new StreamWriter(z.CreateEntry("broken.package").Open())) w.Write("broken");
                using (var w = new StreamWriter(z.CreateEntry("notes.txt").Open())) w.Write("t");
            }
            var all = new[] { "new.package", "house.sims3pack", "readme.txt", "text.package", "sims4.package", "protected.package", "cut.package", "empty.package" }
                .Select(n => Path.Combine(drop, n)).Append(zip).ToList();
            var res = AddTo("Packages", all);
            string Why(string n) => res.FirstOrDefault(x => x.Name == n) is { } x ? x.Problem ?? (x.Doubt ? x.Warning! : "ok") : "gone";
            check(Why("new.package") == "ok" && Why("house.sims3pack").StartsWith("Install it with") && Why("inner.package") == "ok" && Why("evil.package") == "ok"
                && Why("sims4.package").Contains("Sims 4") && Why("protected.package").Contains("protected") && Why("cut.package").Contains("damaged")
                && Why("empty.package").Contains("empty") && Why("text.package").StartsWith("Not a package") && Why("broken.package").StartsWith("Not a package")
                && Why("readme.txt").StartsWith("Not a mod file") && Why("notes.txt") == "gone",
                "install: each file checked, with the reason a Sims 4, protected, cut-short, empty, fake or Sims3Pack file is not added; a text inside a zip is passed over | " + string.Join(" | ", res.Select(x => x.Name + "=" + (x.Problem ?? "ok"))));
            check(File.Exists(Path.Combine(mods, "Packages", "new.package")) && !File.Exists(Path.Combine(docs, "Downloads", "house.sims3pack"))
                && File.Exists(Path.Combine(mods, "Packages", "inner.package")) && File.Exists(Path.Combine(mods, "Packages", "evil.package"))
                && !File.Exists(Path.Combine(root, "evil.package")) && !File.Exists(Path.Combine(docs, "evil.package"))
                && res.Count(x => x.Added is not null) == 3 && !File.Exists(Path.Combine(mods, "Packages", "sims4.package")),
                "install: only the files that pass go in, a zip's by name only (no path from inside it)");
            var again = AddTo("Packages", [Path.Combine(drop, "new.package")]);
            var forced = AddTo("Packages", [Path.Combine(drop, "new.package")], all: true);
            check(again.Single() is { Added: null, Add: false, SameAs: "new.package" } && forced.Single().Added!.EndsWith("new (2).package"),
                "install: a file already installed is not added twice unless asked, then it gets \" (2)\"");
            // the same bytes twice in one drop (a zip and its own extracted copy): only the first goes in
            var twice = Path.Combine(drop, "twice.zip");
            using (var z = ZipFile.Open(twice, ZipArchiveMode.Create))
            using (var w = z.CreateEntry("inner-copy.package").Open()) w.Write(Dbpf20(8));
            File.WriteAllBytes(Path.Combine(drop, "loose-copy.package"), Dbpf20(8));
            var batch = AddTo("Packages", [Path.Combine(drop, "loose-copy.package"), twice, Path.Combine(drop, "loose-copy.package")]);
            check(batch.Count == 2 && batch.Count(x => x.Added is not null) == 1 && batch.Single(x => x.Added is null) is { SameAs: "loose-copy.package", SameInList: true },
                "install: the same file twice in one drop goes in once");

            // a folder so deep that the file would pass the game's 260-character path: not installed, said why
            var deep = Path.Combine("Packages", new string('d', 120), new string('e', 120));
            Directory.CreateDirectory(Path.Combine(mods, deep));
            var longOne = Path.Combine(drop, "long.package");
            File.WriteAllBytes(longOne, Dbpf20(10));
            var far = AddTo(deep, [longOne], all: true);
            check(far.Single() is { Added: null, Problem: { } tooLong } && tooLong.Contains("too long"), "install: a path too long for the game is refused");

            // duplicates by content, whatever the name or folder
            Write(Path.Combine(mods, "Packages", "B", "copy-of-mod3.package"), "content 3");
            var dups = ModLibrary.Duplicates(list());
            check(dups.Count == 4 && dups.Keys.Any(k => k.EndsWith(@"Packages\mod3.package")) && dups.Keys.Any(k => k.EndsWith("new (2).package")) && dups.Keys.Any(k => k.EndsWith("copy-of-mod3.package"))
                && !dups.Keys.Any(k => k.EndsWith("mod4.package")), "duplicates: the same bytes under two names are found (a copy of mod3, the two new.package), nothing else");

            // a hidden or system package, and a hidden folder, are listed: the game loads them
            var hidDir = Path.Combine(mods, "Packages", "Hid");
            var hidden = Path.Combine(hidDir, "hidden.package");
            Write(hidden, "hidden");
            File.SetAttributes(hidden, FileAttributes.Hidden | FileAttributes.System);
            new DirectoryInfo(hidDir).Attributes |= FileAttributes.Hidden;
            check(list().Any(m => m.Path == hidden) && ModLibrary.Folders(docs).Contains(Path.Combine("Packages", "Hid")), "mods: hidden and system packages and folders are listed");
            File.SetAttributes(hidden, FileAttributes.Normal);
            Directory.Delete(hidDir, true);

            // creators: a prefix two files share, a known maker's prefix alone, never a lone word; the player's name wins
            ModFile F(string name) => new(Path.Combine(root, name), Path.Combine("Mods", name), 1, ModKind.Package, true, false);
            var cmods = new[] { F("ani_BistroStove.package"), F("ani_Merchant.package"), F("NRaas_Career.package"), F("Lonely_Mod.package"), F("BB-SmartSim.package"), F("plain.package") };
            var guess = ModCreators.Guess(cmods);
            ModNotes.SetCreator(cmods[3].Rel, "Someone Real");
            check(guess[cmods[0].Path] == "ani" && guess[cmods[2].Path] == "NRaas" && guess[cmods[4].Path] == "BoringBones"
                && !guess.ContainsKey(cmods[3].Path) && !guess.ContainsKey(cmods[5].Path)
                && ModCreators.Of(cmods[3], guess) == ("Someone Real", false) && ModCreators.Of(cmods[0], guess) == ("ani", true),
                "creators: guessed from shared or known prefixes, set by the player over any guess");
            ModNotes.SetKinds(cmods[1].Rel, ["BuildFence", "BuildFence", "Textures"]);
            var keptKinds = ModNotes.Get(cmods[1].Rel)?.Kinds is ["BuildFence", "Textures"];
            ModNotes.SetKinds(cmods[1].Rel, []);
            check(keptKinds && ModNotes.Get(cmods[1].Rel) is null, "types: the player's choice is kept once each; an empty choice goes back to the package's own");
            var tails = ModCreators.Guess([F("Smart-vehicles-decisions-BoringBones.package"), F("Cooking-Overhaul.package"), F("Job-Overhaul.package"), F("Shop-Overhaul.package"),
                F("one-SomeMaker.package"), F("two-SomeMaker.package"), F("three-SomeMaker.package")]);
            check(tails.Values.Count(v => v == "BoringBones") == 1 && !tails.Values.Contains("Overhaul") && tails.Values.Count(v => v == "SomeMaker") == 3,
                "creators: a maker's name at the end counts (known, or three files), a common word like Overhaul never does");

            // the remembered view
            ModViewPrefs.Save(new ModViewPrefs.Prefs(Path.Combine("Packages", "B"), true, 4, ["tags", "game"]));
            ModViewPrefs.LastPlay = new DateTime(2026, 11, 18, 22, 0, 0, DateTimeKind.Utc);
            ModViewPrefs.MergeLater = [@"Mods\Packages\a.package*1*2", @"Mods\Packages\b.package*3*4", @"Mods\Packages\a.package*1*2"];
            check(ModViewPrefs.MergeLater.SequenceEqual([@"Mods\Packages\a.package*1*2", @"Mods\Packages\b.package*3*4"]), "view: the newer versions put off are kept, each once");
            var p = ModViewPrefs.Load();
            check(p.Place == Path.Combine("Packages", "B") && p.Grid && p.Sort == 4 && p.Collapsed.SequenceEqual(["tags", "game"])
                && ModViewPrefs.LastPlay == new DateTime(2026, 11, 18, 22, 0, 0, DateTimeKind.Utc), "view: place, grid, sort, folded groups and the last game kept");
        }
        catch (Exception e) { check(false, "mod tools threw: " + e); }
        finally
        {
            Paths.LocalAppData = oldLocal;
            ModNotes.Reload();
            try { Directory.Delete(root, true); } catch (Exception) { }
        }
    }

    // a small DBPF 2.0 package with n resources, as the game writes it (header, data, index with no constant fields)
    internal static byte[] Dbpf20(int n)
    {
        using var ms = new MemoryStream();
        var w = new BinaryWriter(ms);
        w.Write("DBPF"u8.ToArray()); w.Write(2u); w.Write(0u); w.Write(new byte[96 - 12]);
        var offsets = new List<uint>();
        for (int i = 0; i < n; i++) { offsets.Add((uint)ms.Position); w.Write(System.Text.Encoding.ASCII.GetBytes($"res{i:000}")); }
        var at = (uint)ms.Position;
        w.Write(0u);
        for (int i = 0; i < n; i++) { w.Write(0x0333406Cu); w.Write(0u); w.Write(0u); w.Write((uint)(i + 1000 * n)); w.Write(offsets[i]); w.Write(6u | 0x80000000u); w.Write(6u); w.Write((ushort)0); w.Write((ushort)1); }
        var end = (uint)ms.Position;
        ms.Position = 0x24; w.Write((uint)n);
        ms.Position = 0x2C; w.Write(end - at);
        ms.Position = 0x40; w.Write(at);
        return ms.ToArray();
    }

    static void Write(string path, string text) { Directory.CreateDirectory(Path.GetDirectoryName(path)!); File.WriteAllText(path, text); }

    static Game MakeGame(string root)
    {
        var bin = Path.Combine(root, "Game", "Game", "Bin");
        Directory.CreateDirectory(bin);
        return new Game(Path.Combine(root, "Game"), bin, "TS3W.exe", Edition.Steam, "1.67.2");
    }
}
