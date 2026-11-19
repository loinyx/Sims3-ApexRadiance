using System.IO.Compression;
using ApexSetup;

// The Mods screen's helpers: the problem-mod hunt (every answer path, Stop, a restart in the middle), folders moved into
// folders, files added (packages, Sims3Packs, zips, unsafe names), duplicates found by content, and the remembered view.
static class ModToolsTests
{
    public static void Run(Action<bool, string> check)
    {
        // the game's installer mode: one entry per line, the switches the game's launcher passes, ids by their first 8 digits
        check(GameContent.ListText([@"C:\D\a.Sims3Pack", " 0xa7e6b0d106f518b841b3aba09cd79153 "]) == "C:\\D\\a.Sims3Pack\n0xa7e6b0d106f518b841b3aba09cd79153\n"
            && GameContent.Args(GameContent.Mode.Remove, @"C:\t\l.txt") == "-ccuninstall:\"C:\\t\\l.txt\" -r:450x350 -w -nosound"
            && GameContent.ShortId("0xa7e6b0d106f518b841b3aba09cd79153") == 0xA7E6B0D1 && GameContent.ShortId("") == 0,
            "game content: the list file, the switches and the short id");
        var rr = GameContent.ReadResult("1 C:\\D\\a.Sims3Pack\r\n0 C:\\D\\b.Sims3Pack\r\n", [@"C:\D\a.Sims3Pack", @"C:\D\b.Sims3Pack"]);
        check(rr is { Done.Count: 1, Failed.Count: 1 } && rr.Done[0] == @"C:\D\a.Sims3Pack" && GameContent.ReadResult("C:\\D\\a.Sims3Pack\n", [@"C:\D\a.Sims3Pack"]) is null,
            "game content: the game's result lines (1 done, 0 refused); the list as written is no result yet");
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

            // files added, every one checked first: real packages and a Sims3Pack go in (the Sims3Pack to Downloads), a zip's
            // by name only; a Sims 4 package, a protected one, a cut-short one, an empty one, text named .package, a Sims3Pack
            // without its header and a file already installed are not added, each with its reason
            var drop = Path.Combine(root, "drop");
            Directory.CreateDirectory(drop);
            File.WriteAllBytes(Path.Combine(drop, "new.package"), Dbpf20(1));
            File.WriteAllBytes(Path.Combine(drop, "house.sims3pack"), [7, 0, 0, 0, .. "TS3Pack"u8.ToArray(), 1, 2, 3]);
            File.WriteAllBytes(Path.Combine(drop, "fake.sims3pack"), "not one"u8.ToArray());
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
            var all = new[] { "new.package", "house.sims3pack", "fake.sims3pack", "readme.txt", "text.package", "sims4.package", "protected.package", "cut.package", "empty.package" }
                .Select(n => Path.Combine(drop, n)).Append(zip).ToList();
            var looked = ModInstall.Inspect(all, list());
            string Why(string n) => looked.First(x => x.Name == n).Problems.FirstOrDefault() ?? "ok";
            check(Why("new.package") == "ok" && Why("house.sims3pack") == "ok" && Why("inner.package") == "ok" && Why("evil.package") == "ok"
                && Why("sims4.package").Contains("Sims 4") && Why("protected.package").Contains("protected") && Why("cut.package").Contains("damaged")
                && Why("empty.package").Contains("empty") && Why("text.package").StartsWith("Not a package") && Why("broken.package").StartsWith("Not a package")
                && Why("fake.sims3pack").Contains("header") && Why("readme.txt").StartsWith("Not a mod file"),
                "install: each file checked, with the reason a Sims 4, protected, cut-short, empty or fake file is not added | " + string.Join(" | ", looked.Select(x => x.Name + "=" + (x.Problems.FirstOrDefault() ?? "ok"))));
            var res = ModLibrary.AddAny(docs, "Packages", all, list());
            check(File.Exists(Path.Combine(mods, "Packages", "new.package")) && File.Exists(Path.Combine(docs, "Downloads", "house.sims3pack"))
                && File.Exists(Path.Combine(mods, "Packages", "inner.package")) && File.Exists(Path.Combine(mods, "Packages", "evil.package"))
                && !File.Exists(Path.Combine(root, "evil.package")) && !File.Exists(Path.Combine(docs, "evil.package"))
                && res.Added.Count == 4 && res.Skipped.Count == 8 && !File.Exists(Path.Combine(mods, "Packages", "sims4.package")),
                "install: only the files that pass go in, a zip's by name only (no path from inside it) | " + res.Added.Count + " | " + string.Join(" | ", res.Skipped));
            var again = ModLibrary.AddAny(docs, "Packages", [Path.Combine(drop, "new.package")], list());
            var forced = ModLibrary.AddAny(docs, "Packages", [Path.Combine(drop, "new.package")], list(), duplicates: true);
            check(again.Added.Count == 0 && again.Skipped.Single().Contains("already installed as new.package") && forced.Added.Single().EndsWith("new (2).package"),
                "install: a file already installed is not added twice unless asked, then it gets \" (2)\"");

            // Sims3Packs: an object set's packages taken out into a folder named after it (a broken one inside refused), a lot
            // left for the game's launcher in Downloads, a manifest pointing past the file unread
            var objPack = Sims3PackFile("object", "Pretty Chairs", ("a.package", Dbpf20(2)), ("b.package", "broken"u8.ToArray()), ("thumb.png", [1, 2, 3]));
            var lotPack = Sims3PackFile("lot", "Cozy Lot", ("lot.package", Dbpf20(3)));
            File.WriteAllBytes(Path.Combine(drop, "chairs.sims3pack"), objPack);
            File.WriteAllBytes(Path.Combine(drop, "lot.sims3pack"), lotPack);
            var badPack = (byte[])objPack.Clone(); Array.Resize(ref badPack, badPack.Length - 10);
            var info = Sims3Pack.Read(Path.Combine(drop, "chairs.sims3pack"));
            File.WriteAllBytes(Path.Combine(drop, "bad.sims3pack"), badPack);
            var s3 = ModLibrary.AddAny(docs, "Packages", [Path.Combine(drop, "chairs.sims3pack"), Path.Combine(drop, "lot.sims3pack")], list());
            check(info is { Type: "object", DisplayName: "Pretty Chairs", Extractable: true } && info.Files.Count == 3 && Sims3Pack.Read(Path.Combine(drop, "bad.sims3pack")) is null
                && File.Exists(Path.Combine(mods, "Packages", "Pretty Chairs", "a.package")) && !File.Exists(Path.Combine(mods, "Packages", "Pretty Chairs", "b.package"))
                && File.Exists(Path.Combine(docs, "Downloads", "lot.sims3pack")) && s3.Skipped.Any(x => x.Contains("b.package")),
                "sims3pack: an object set taken out into its own folder, a broken package in it refused, a lot left for the game's launcher");
            // what a lot needs: its manifest's dependencies, the base game and the packs set aside; and the installed ids the
            // game's cache directory records (bytes 8 to 11 of each item's 16-byte id)
            File.WriteAllBytes(Path.Combine(drop, "needs.sims3pack"), Sims3PackDeps("lot", "0x5ad2691e000000005ad2691e00000000", "0x050cffe800000000050cffe800000000", "0x062d9a0700000000062d9a0700000000"));
            var needs = Sims3Pack.Read(Path.Combine(drop, "needs.sims3pack"));
            var record = new byte[52];
            byte[] id = [1, 2, 3, 4, 5, 6, 7, 8, 0x5A, 0xD2, 0x69, 0x1E, 9, 10, 11, 12];
            id.CopyTo(record, 4); id.CopyTo(record, 36);
            byte[] base3 = [0x05, 0x0C, 0xFF, 0xE8]; base3.CopyTo(record, 20); base3.CopyTo(record, 28);
            check(needs is { Dependencies.Count: 3 } && needs.StoreDependencies.SequenceEqual([0x5AD2691Eu]) && InstalledContent.IdsIn(record).SequenceEqual([0x5AD2691Eu])
                  && !InstalledContent.IdsIn(new byte[52]).Any(),
                "sims3pack: the store items a lot needs, matched to the ids of what the game's launcher installed");

            // a folder so deep that the file would pass the game's 260-character path: not installed, said why
            var deep = Path.Combine("Packages", new string('d', 120), new string('e', 120));
            Directory.CreateDirectory(Path.Combine(mods, deep));
            var far = ModLibrary.AddAny(docs, deep, [Path.Combine(drop, "new.package")], [], duplicates: true);
            check(far.Added.Count == 0 && far.Skipped.Single().Contains("too long"), "install: a path too long for the game is refused");

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
    static byte[] Dbpf20(int n)
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

    // a Sims3Pack as the game writes one: uint32 7, "TS3Pack", uint16 version, uint32 manifest length, the manifest, the files
    static byte[] Sims3PackDeps(string type, params string[] deps)
    {
        var x = System.Text.Encoding.UTF8.GetBytes($"<?xml version=\"1.0\" encoding=\"UTF-8\"?><Sims3Package Type=\"{type}\"><DisplayName>needs</DisplayName><Dependencies>{string.Concat(deps.Select(d => $"<Dependency>{d}</Dependency>"))}</Dependencies></Sims3Package>");
        using var ms = new MemoryStream();
        var w = new BinaryWriter(ms);
        w.Write(7u); w.Write("TS3Pack"u8.ToArray()); w.Write((ushort)0x0501); w.Write((uint)x.Length); w.Write(x);
        return ms.ToArray();
    }

    static byte[] Sims3PackFile(string type, string name, params (string Name, byte[] Data)[] files)
    {
        var xml = new System.Text.StringBuilder($"<?xml version=\"1.0\" encoding=\"UTF-8\"?><Sims3Package Type=\"{type}\" SubType=\"0x00000000\"><DisplayName>{name}</DisplayName>");
        long off = 0;
        foreach (var (n, d) in files) { xml.Append($"<PackagedFile><Name>{n}</Name><Length>{d.Length}</Length><Offset>{off}</Offset><ContentType>{type}</ContentType></PackagedFile>"); off += d.Length; }
        xml.Append("</Sims3Package>");
        var x = System.Text.Encoding.UTF8.GetBytes(xml.ToString());
        using var ms = new MemoryStream();
        var w = new BinaryWriter(ms);
        w.Write(7u); w.Write("TS3Pack"u8.ToArray()); w.Write((ushort)0x0501); w.Write((uint)x.Length); w.Write(x);
        foreach (var (_, d) in files) w.Write(d);
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
