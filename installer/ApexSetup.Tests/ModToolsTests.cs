using System.IO.Compression;
using ApexSetup;

// The Mods screen's helpers: the problem-mod hunt (every answer path, Stop, a restart in the middle), folders moved into
// folders, files added (packages, Sims3Packs, zips, unsafe names), duplicates found by content, and the remembered view.
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

            // "the answers did not add up": still there with every suspect off this round leaves none
            ModHunt.Start(list(), docs);
            var end = ModHunt.Answer(stillThere: false, list(), docs);
            while (end is { Suspects.Count: > 1 }) end = ModHunt.Answer(stillThere: false, list(), docs);
            check(end is not null && list().All(m => before[m.Name] == m.Enabled), "hunt: always gone ends on one of the first halves, mods back");
            ModHunt.Forget();

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

            // files added: packages, a Sims3Pack to Downloads, a zip's packages, unsafe and other names skipped
            var drop = Path.Combine(root, "drop");
            Directory.CreateDirectory(drop);
            Write(Path.Combine(drop, "new.package"), "n");
            Write(Path.Combine(drop, "house.sims3pack"), "s");
            Write(Path.Combine(drop, "readme.txt"), "r");
            var zip = Path.Combine(drop, "set.zip");
            using (var z = ZipFile.Open(zip, ZipArchiveMode.Create))
            {
                using (var w = new StreamWriter(z.CreateEntry("set/inner.package").Open())) w.Write("z");
                using (var w = new StreamWriter(z.CreateEntry("../../evil.package").Open())) w.Write("e");
                using (var w = new StreamWriter(z.CreateEntry("notes.txt").Open())) w.Write("t");
            }
            var res = ModLibrary.AddAny(docs, "Packages", [Path.Combine(drop, "new.package"), Path.Combine(drop, "house.sims3pack"), Path.Combine(drop, "readme.txt"), zip]);
            check(File.Exists(Path.Combine(mods, "Packages", "new.package")) && File.Exists(Path.Combine(docs, "Downloads", "house.sims3pack"))
                && File.Exists(Path.Combine(mods, "Packages", "inner.package")) && File.Exists(Path.Combine(mods, "Packages", "evil.package"))
                && !File.Exists(Path.Combine(root, "evil.package")) && !File.Exists(Path.Combine(docs, "evil.package"))
                && res.Skipped.Contains("readme.txt") && res.Added.Count == 4,
                "add: packages to the folder, Sims3Packs to Downloads, a zip's packages by name only (no path from inside it)");
            var again = ModLibrary.AddAny(docs, "Packages", [Path.Combine(drop, "new.package")]);
            check(again.Added.Single().EndsWith("new (2).package"), "add: a name taken gets \" (2)\"");

            // duplicates by content, whatever the name or folder
            Write(Path.Combine(mods, "Packages", "B", "copy-of-mod3.package"), "content 3");
            var dups = ModLibrary.Duplicates(list());
            check(dups.Count == 4 && dups.Keys.Any(k => k.EndsWith(@"Packages\mod3.package")) && dups.Keys.Any(k => k.EndsWith("new (2).package")) && dups.Keys.Any(k => k.EndsWith("copy-of-mod3.package"))
                && !dups.Keys.Any(k => k.EndsWith("mod4.package")), "duplicates: the same bytes under two names are found (a copy of mod3, the two new.package), nothing else");

            // creators: a prefix two files share, a known maker's prefix alone, never a lone word; the player's name wins
            ModFile F(string name) => new(Path.Combine(root, name), Path.Combine("Mods", name), 1, ModKind.Package, true, false);
            var cmods = new[] { F("ani_BistroStove.package"), F("ani_Merchant.package"), F("NRaas_Career.package"), F("Lonely_Mod.package"), F("BB-SmartSim.package"), F("plain.package") };
            var guess = ModCreators.Guess(cmods);
            ModNotes.SetCreator(cmods[3].Rel, "Someone Real");
            check(guess[cmods[0].Path] == "ani" && guess[cmods[2].Path] == "NRaas" && guess[cmods[4].Path] == "BoringBones"
                && !guess.ContainsKey(cmods[3].Path) && !guess.ContainsKey(cmods[5].Path)
                && ModCreators.Of(cmods[3], guess) == ("Someone Real", false) && ModCreators.Of(cmods[0], guess) == ("ani", true),
                "creators: guessed from shared or known prefixes, set by the player over any guess");
            var tails = ModCreators.Guess([F("Smart-vehicles-decisions-BoringBones.package"), F("Cooking-Overhaul.package"), F("Job-Overhaul.package"), F("Shop-Overhaul.package"),
                F("one-SomeMaker.package"), F("two-SomeMaker.package"), F("three-SomeMaker.package")]);
            check(tails.Values.Count(v => v == "BoringBones") == 1 && !tails.Values.Contains("Overhaul") && tails.Values.Count(v => v == "SomeMaker") == 3,
                "creators: a maker's name at the end counts (known, or three files), a common word like Overhaul never does");

            // the remembered view
            ModViewPrefs.Save(new ModViewPrefs.Prefs(Path.Combine("Packages", "B"), true, 4, ["tags", "game"]));
            ModViewPrefs.LastPlay = new DateTime(2026, 11, 18, 22, 0, 0, DateTimeKind.Utc);
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

    static void Write(string path, string text) { Directory.CreateDirectory(Path.GetDirectoryName(path)!); File.WriteAllText(path, text); }

    static Game MakeGame(string root)
    {
        var bin = Path.Combine(root, "Game", "Game", "Bin");
        Directory.CreateDirectory(bin);
        return new Game(Path.Combine(root, "Game"), bin, "TS3W.exe", Edition.Steam, "1.67.2");
    }
}
