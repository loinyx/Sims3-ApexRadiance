// Offline checks of Find custom content (ApexSetup.Core/ModFind.cs) and of clearing chosen caches (GameCaches.cs) on files
// made here. APEX_FIND_GAME=<game folder> adds a read-only run on the lots, households and worlds that game ships.
using System.Text;
using ApexSetup;

static class ModFindTests
{
    const uint KeyTable = 0x05ED1226, Outfit = 0x025ED6F4, Xml = 0x0333406C;
    const ulong Sofa = 0x8A41C0E255F01B22, Hair = 0x9C22000000000001, Rug = 0x9C22000000000002, Lamp = 0x9C22000000000003, Gone = 0x9C22000000000004, Ea = 0x98A11F, Stock = 0x100;

    public static void Run(Action<bool, string> check)
    {
        var root = Directory.CreateTempSubdirectory("ApexFindTest-").FullName;
        var oldLocal = Paths.LocalAppData;
        var oldRegistry = Expansions.RegistryDir;
        Paths.LocalAppData = Path.Combine(root, "Local");
        Expansions.RegistryDir = _ => null;
        try
        {
            Lists(check, Path.Combine(root, "lists"));
            GameFiles(check, Path.Combine(root, "files"));
            Search(check, Path.Combine(root, "search"));
            Caches(check, Path.Combine(root, "caches"));
            if (Environment.GetEnvironmentVariable("APEX_FIND_GAME") is { Length: > 0 } game) Real(game);
        }
        catch (Exception e) { check(false, "find: exception " + e); }
        finally
        {
            Games.IsRunning = () => false;
            Paths.LocalAppData = oldLocal;
            Expansions.RegistryDir = oldRegistry;
            try { Directory.Delete(root, true); } catch (Exception) { }
        }
    }

    static string Write(string path, string text, Encoding? e = null)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        File.WriteAllText(path, text, e ?? new UTF8Encoding(false));
        return path;
    }

    static string Pkg(string path, params (uint Type, uint Group, ulong Instance, byte[] Data)[] r)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        File.WriteAllBytes(path, ModConflicts.WritePackage(r));
        return path;
    }

    static string? Refused(string path)
    {
        try { ModFind.Read(path); return null; }
        catch (SetupException e) { return e.Key; }
    }

    static void Lists(Action<bool, string> check, string dir)
    {
        var n = 0;
        IReadOnlyList<FindItem> Of(string text, string ext = ".txt", Encoding? e = null) => ModFind.Read(Write(Path.Combine(dir, $"list{n++}{ext}"), text, e)).Items;
        string Keys(IReadOnlyList<FindItem> l) => string.Join(" ", l.Select(i => i.Key ?? "?"));
        const string key = "0x319E4F1D-0x00000000-0x8A41C0E255F01B22";

        foreach (var t in new[] { key, "319E4F1D:00000000:8A41C0E255F01B22", "319e4f1d-00000000-8a41c0e255f01b22", "key:319E4F1D:00000000:8A41C0E255F01B22", "0x319E4F1D\t0x0\t0x8A41C0E255F01B22",
                     "S3_319E4F1D_00000000_8A41C0E255F01B22_Sofa%%+OBJD.objd", "0x319E4F1D, 0x00000000, 0x8A41C0E255F01B22", "319E4F1D!00000000!8A41C0E255F01B22" })
            check(Keys(Of(t)) == key, $"find: the key is read in {t} ({Keys(Of(t))})");
        check(Keys(Of(key, ".txt", Encoding.Unicode)) == key && Keys(Of(key, ".txt", new UnicodeEncoding(false, false))) == key && Keys(Of(key, ".txt", Encoding.BigEndianUnicode)) == key,
            "find: a list in UTF-16, with its mark or without, is read");
        check(Of("md5 d41d8cd98f00b204e9800998ecf8427e\r\nid 1234567890123456 at 0x12").All(i => i.Instance == 0), "find: a checksum, a long number and a short number are no keys");
        check(Keys(Of("0x8A41C0E255F01B22\r\n" + key)) == key && Keys(Of("8A41C0E255F01B22")) == "0x8A41C0E255F01B22", "find: an instance alone is an item, unless its full key is there too");
        var named = Of($"My custom content:\r\nGreen Tractor = {key}\r\n0x034AEECB-0x00000000-0x9C22000000000001\r\n");
        check(named.Count == 2 && named[0].Name == "Green Tractor" && named[1].Name is null, "find: the rest of a key's line is its name, other lines are not items beside keys");
        var both = Of(string.Join(" ", key, "0x034AEECB-0x00000000-0x9C22000000000001", "and two names"));
        check(both.Count == 2 && both.All(i => i.Name is null), "find: two keys on one line take no name from it");
        check(Of(key + "\r\n" + key.Replace("0x00000000", "0x48000001")).Count == 1, "find: the same item in two groups counts once");
        var names = Of("# my list\r\nWanted:\r\n- Comfy Sofa\r\n* Wicker Chair\r\n12\r\n");
        check(names.Select(i => i.Name).SequenceEqual(["Comfy Sofa", "Wicker Chair"]) && names.All(i => i.Key is null), $"find: a list of names only is read line by line ({string.Join("|", names.Select(i => i.Name))})");
        check(Of(@"Sofa: C:\Mods\Packages\Comfy Sofa.package (2 MB)" + "\r\nother.packages").Single().File == "Comfy Sofa.package", "find: a package's file name is an item");

        var x = Of($"<?xml version=\"1.0\"?><list><item name=\"Old Tractor\" key=\"{key}\"/><item><name>Lace Window</name><key>319E4F1D:00000000:9C22000000000002</key></item>"
            + "<r type=\"0x034AEECB\" group=\"0x0\" instance=\"0x9C22000000000001\"/><r type=\"034AEECB\" group=\"0\" instance=\"9C22000000000003\"/></list>", ".xml");
        check(x.Count == 4 && x.Single(i => i.Instance == Sofa).Name == "Old Tractor" && x.Single(i => i.Instance == Rug).Name == "Lace Window"
              && x.Count(i => i.Type == ModKinds.Casp) == 2, $"find: an XML list's keys and names, as attributes or children ({Keys(x)})");
        check(Of("<objects><object name=\"Green Tractor\"/><object><Title>Wicker Chair</Title></object></objects>", ".xml").Select(i => i.Name).SequenceEqual(["Green Tractor", "Wicker Chair"]),
            "find: an XML list of names only");
        var secret = Write(Path.Combine(dir, "secret.txt"), key);
        check(Of($"<!DOCTYPE x [<!ENTITY a SYSTEM \"{new Uri(secret)}\">]><x><i name=\"&a;\">&a;</i></x>", ".xml").Count == 0, "find: an XML with a DTD loads nothing from outside");
        var bomb = "<!DOCTYPE x [<!ENTITY a \"aaaaaaaaaa\">" + string.Concat(Enumerable.Range(0, 9).Select(i => $"<!ENTITY a{i} \"{string.Concat(Enumerable.Repeat(i == 0 ? "&a;" : $"&a{i - 1};", 10))}\">")) + "]><x>&a8;</x>";
        check(Of(bomb, ".xml").Count == 0, "find: an XML entity bomb is not expanded");
        check(Keys(Of(string.Concat(Enumerable.Repeat("<a>", 50_000)) + key, ".xml")) == key && Keys(Of($"<broken <<< {key}", ".xml")) == key, "find: a broken or very deep XML is still read as text");
        check(Of("<a>nothing here</a>", ".xml").Count == 0 && Of("").Count == 0, "find: a list with nothing in it gives nothing");
        var many = new StringBuilder();
        for (var i = 0; i < ModFind.MaxItems + 50; i++) many.Append($"0x319E4F1D-0x00000000-0x{0x9000000000000000UL + (ulong)i:X16}\n");
        var big = ModFind.Read(Write(Path.Combine(dir, "many.txt"), many.ToString()));
        check(big.Items.Count == ModFind.MaxItems && big.Truncated, "find: a list past the limit is cut and says so");
        var huge = Path.Combine(dir, "huge.txt");
        using (var f = File.Create(huge)) f.SetLength(ModFind.MaxText + 1L);
        check(Refused(huge) == "This file is too big to be a list.", "find: a text past the size limit is refused");
        File.WriteAllBytes(Path.Combine(dir, "a.zip"), [0x50, 0x4B, 3, 4, 0, 0, 0, 1, 2]);
        check(Refused(Path.Combine(dir, "a.zip"))!.StartsWith("The launcher can't read this kind"), "find: a file of another kind is refused");
        var cts = new CancellationTokenSource();
        cts.Cancel();
        var stopped = false;
        try { ModFind.Read(Path.Combine(dir, "many.txt"), cts.Token); } catch (OperationCanceledException) { stopped = true; }
        check(stopped, "find: reading stops when cancelled");
    }

    static byte[] Bytes(params object[] v)
    {
        var ms = new MemoryStream();
        var w = new BinaryWriter(ms);
        foreach (var o in v)
            switch (o) { case uint u: w.Write(u); break; case ulong l: w.Write(l); break; case byte b: w.Write(b); break; case string s: w.Write(Encoding.UTF8.GetBytes(s)); break; }
        return ms.ToArray();
    }

    // a lot: the key table (type, group, instance at odd offsets, a two-byte index after each) and an outfit (instance, group, type)
    static (uint, uint, ulong, byte[])[] LotParts() =>
    [
        (KeyTable, 0, 1, Bytes((byte)3, (byte)0, (byte)0, 5u, ModKinds.Objd, 0u, Sofa, (byte)1, (byte)0, ModKinds.Objd, 0x28000000u, Ea, (byte)2, (byte)0, ModKinds.Img, 0u, 0x9C22000000000009UL, (byte)3, (byte)0,
            ModKinds.Objd, 0u, 0UL, (byte)4, (byte)0, ModKinds.Cwal, 0u, Rug, (byte)5, (byte)0, ModKinds.Objd)),
        (Outfit, 0, 2, Bytes((byte)7, Hair, 0x48000003u, ModKinds.Casp, Gone, 0u, ModKinds.Casp)),
        (Xml, 0, 3, Bytes("<preset><value key=\"pattern\" value=\"key:0333406c:00000000:9c22000000000003\"/><v value=\"key:00b2d882:00000000:9c22000000000007\"/></preset>")),
        (ModKinds.Cwal, 0, Rug, [1]),
    ];

    static void GameFiles(Action<bool, string> check, string dir)
    {
        var lot = ModFind.Read(Pkg(Path.Combine(dir, "Farm.package"), LotParts()));
        check(lot.Name == "Farm.package" && lot.Items.Select(i => i.Instance).Order().SequenceEqual(new[] { Ea, Sofa, Hair, Gone, Lamp }.Order()) && lot.Items.Single(i => i.Instance == Hair) is { Type: ModKinds.Casp, Group: 0x48000003 },
            $"find: a lot's objects, its Sims' parts and its patterns are read, not its textures or what it carries itself ({string.Join(" ", lot.Items.Select(i => i.Key))})");
        var part = ModFind.Read(Pkg(Path.Combine(dir, "recolour.package"), (ModKinds.Casp, 0, 5, Bytes((byte)0, Hair, 1u, 0x736884F1u, 0x9C22000000000008UL, 0u, ModKinds.Img))));
        check(part.Items.Single() is { Type: 0x736884F1, Instance: Hair }, "find: a Create a Sim part names the mesh it needs, not its textures");

        var save = Path.Combine(dir, "Family.sims3");
        Pkg(Path.Combine(save, "Riverview_0x1.nhd"), LotParts());
        Pkg(Path.Combine(save, "Other.nhd"), (KeyTable, 0, 1, Bytes((byte)3, ModKinds.Objd, 0u, Lamp)));
        var whole = ModFind.Read(Path.Combine(save, "Riverview_0x1.nhd"));
        check(whole.Name == "Family.sims3" && whole.Items.Any(i => i.Type == ModKinds.Objd && i.Instance == Lamp) && whole.Items.SequenceEqual(ModFind.Read(save).Items), "find: one file of a save, or its folder, reads the whole save");
        var lone = Pkg(Path.Combine(dir, "Lone.nhd"), (KeyTable, 0, 1, Bytes((byte)3, ModKinds.Objd, 0u, Lamp)));
        Pkg(Path.Combine(dir, "Beside.nhd"), LotParts());
        check(ModFind.Read(lone) is { Name: "Lone.nhd", Items.Count: 1 }, "find: a save's file outside a save is read alone");
        File.Delete(lone);
        File.Delete(Path.Combine(dir, "Beside.nhd"));
        check(Refused(dir) == "This folder is not a save. Choose one that ends in .sims3.", "find: another folder is refused");

        File.WriteAllBytes(Path.Combine(dir, "protected.package"), [.. "DBPP"u8, .. new byte[200]]);
        check(Refused(Path.Combine(dir, "protected.package")) == "This file is protected, so the launcher can't read it.", "find: a protected package is refused, not read");
        var bytes = ModConflicts.WritePackage(LotParts());
        File.WriteAllBytes(Path.Combine(dir, "cut.package"), bytes[..^40]);
        File.WriteAllBytes(Path.Combine(dir, "empty.package"), "DBPF"u8.ToArray());
        check(Refused(Path.Combine(dir, "cut.package")) == "This file is empty or damaged." && Refused(Path.Combine(dir, "empty.package")) == "This file is empty or damaged.", "find: a damaged package is refused");
        // (resources that point past the end of the file)
        BitConverter.GetBytes(0x7FFFFFF0u).CopyTo(bytes, bytes.Length - 32 * 4 + 16);
        File.WriteAllBytes(Path.Combine(dir, "lies.package"), bytes);
        check(ModFind.Read(Path.Combine(dir, "lies.package")).Items.All(i => i.Type == ModKinds.Casp || i.Instance == Lamp), "find: a resource past the end of the file is skipped");
    }

    static void Search(Action<bool, string> check, string root)
    {
        var game = Path.Combine(root, "The Sims 3");
        var g = new Game(game, Path.Combine(game, "Game", "Bin"), "TS3W.exe", Edition.Steam, "1.67.2");
        Directory.CreateDirectory(g.Bin);
        Pkg(Path.Combine(game, "GameData", "Shared", "Packages", "FullBuild0.package"), (ModKinds.Objd, 0x28000000, Stock, [1]), (ModKinds.Casp, 0x48000000, 0x77777777777777UL, [1]));
        var gi = DefaultReplacements.Index(g);
        var docs = Path.Combine(root, "Docs");
        Write(Path.Combine(docs, "Mods", "Resource.cfg"), "Priority 500\r\nPackedFile Packages/*.package\r\nPackedFile Packages/*/*.package\r\n");
        var pk = Path.Combine(docs, "Mods", "Packages");
        Pkg(Path.Combine(pk, "Sofas.package"), (ModKinds.Objd, 0, Sofa, [1]));
        Pkg(Path.Combine(Mods.Disabled(docs), "Packages", "Rugs.package"), (ModKinds.Cwal, 0, Rug, [1]));
        Pkg(Path.Combine(pk, "Hair One.package"), (ModKinds.Casp, 0, Hair, [1, 2]));
        Pkg(Path.Combine(pk, "Hair Two.package"), (ModKinds.Casp, 0, Lamp, [3, 4]));
        List<ModFile> Mine() => Mods.List(g, docs, kinds: false).Where(m => m.FileType == "package").ToList();
        var merge = ModMerge.Run(ModMerge.Plan(docs, Mine().Where(m => m.Name.StartsWith("Hair")).ToList(), "Hair", "Packages", false), null, default).Merged;

        var list = ModFind.Read(Write(Path.Combine(root, "list.txt"), string.Join("\n",
            $"Sofa 319E4F1D:00000000:{Sofa:X16}", $"Rug 515CA4CD:00000000:{Rug:X16}", $"0x{Hair:X16}", $"0x034AEECB-0x00000000-0x{Gone:X16}", $"Chair 0x319E4F1D-0x28000000-0x{Stock:X}",
            "0x034AEECB-0x48000007-0x77777777777777", $"0x319E4F1D-0x00000000-0x{Ea:X}", $"0x034AEECB-0x00000000-0x{Sofa + 1:X16}")));
        var mods = Mine();
        bool? On(string rel) => mods.Where(m => m.Rel.Equals(rel, StringComparison.OrdinalIgnoreCase)).Select(m => (bool?)m.Enabled).FirstOrDefault();
        var steps = new List<double>();
        var r = ModFind.Search(list, mods, docs, gi, new Now(steps.Add));
        FindMatch At(FindResult x, ulong instance) => x.Matches.Single(m => m.Item.Instance == instance);
        FindState State(ulong instance) => ModFind.StateOf(At(r, instance), On);
        check(State(Sofa) == FindState.Found && At(r, Sofa).Hits.Single() == new FindHit(Path.Combine("Mods", "Packages", "Sofas.package"), null), "find: an item in a mod that is on");
        check(State(Rug) == FindState.Off && At(r, Rug).Hits.Single().Rel == Path.Combine("Mods", "Packages", "Rugs.package"), "find: an item in a mod that is off");
        check(State(Hair) == FindState.Found && At(r, Hair).Hits.Single() == new FindHit(merge.Rel, "Hair One"), $"find: an item inside a merge names the mod in it ({At(r, Hair).Hits.Single()})");
        check(State(Gone) == FindState.Missing && State(Sofa + 1) == FindState.Missing && !ModFind.MayBeGame(r, At(r, Gone).Item), "find: an item no mod holds is missing; the same instance of another type is not it");
        check(State(Stock) == FindState.Game && State(0x77777777777777) == FindState.Game, "find: the game's own items, also with a numbered group, are told apart");
        check(State(Ea) == FindState.Missing && ModFind.MayBeGame(r, At(r, Ea).Item), "find: a missing item with the game's own small number may come from a pack");
        check(steps.Count > 1 && steps[^1] == 1, "find: the search reports its progress");

        var without = ModFind.Search(list, mods, docs, null);
        check(!without.GameKnown && ModFind.MayBeGame(without, At(without, Gone).Item) && ModFind.StateOf(At(without, Stock), On) == FindState.Missing, "find: without the game's index, what is missing may come with the game");

        var reads = ModFind.PackReads;
        ModFind.Search(list, mods, docs, gi);
        check(ModFind.PackReads == reads, "find: a second search reads no package again");
        Pkg(Path.Combine(pk, "Sofas.package"), (ModKinds.Objd, 0, Sofa, [1]), (ModKinds.Casp, 0, Gone, [1]));
        File.SetLastWriteTimeUtc(Path.Combine(pk, "Sofas.package"), DateTime.UtcNow.AddMinutes(1));
        r = ModFind.Search(list, Mine(), docs, gi);
        check(ModFind.PackReads == reads + 1 && State(Gone) == FindState.Found, "find: a package that changed is read again, alone");

        // installed with the game's launcher; a resource of which the game ships only the picture or the footprint
        const ulong store = 0x9C22000000000077, half = 0x9C22000000000088, foot = 0x9C22000000000099;
        var some = ModFind.Read(Write(Path.Combine(root, "store.txt"), string.Join("\n", $"0x034AEECB-0x00000000-0x{store:X16}", $"515CA4CD:00000000:{half:X16}", $"0x0333406C-0x00000000-0x{foot:X16}", $"0x034AEECB-0x00000000-0x{Rug:X16}")));
        Pkg(Path.Combine(docs, "DCCache", "dcdb0.dbc"), (ModKinds.Casp, 0, store, [1]));
        Pkg(Path.Combine(docs, "DCCache", "dcdb1.dbc.tmp"), (ModKinds.Casp, 0, Rug, [1]));
        Pkg(Path.Combine(docs, "DCBackup", "0x1.package"), (ModKinds.Casp, 0, store + 1, [1]));
        Pkg(Path.Combine(game, "GameData", "Shared", "Packages", "FullBuild1.package"), (ModKinds.Img, 0, half, [1]), (0xD382BF57, 0, foot, [1]));
        var got = ModFind.Search(some, Mine(), docs, DefaultReplacements.Index(g));
        check(got.Matches.Select(m => ModFind.StateOf(m, On)).SequenceEqual([FindState.Installed, FindState.Game, FindState.Game, FindState.Missing]),
            $"find: what the game's launcher installed and what the game ships only in part are not missing ({string.Join(" ", got.Matches.Select(m => ModFind.StateOf(m, On)))})");

        // turned on: the state follows the mods, the search does not run again
        Mods.SetEnabled(mods.Single(m => m.Name == "Rugs.package"), true, docs);
        mods = Mine();
        check(State(Rug) == FindState.Found, "find: an item of a mod just turned on is found");
        mods = mods.Where(m => m.Name != "Rugs.package").ToList();
        check(State(Rug) == FindState.Missing, "find: an item of a mod that is gone is missing");

        // a catalog name, from a package's own strings; damaged strings are no names and stop nothing
        var strings = new MemoryStream();
        var sw = new BinaryWriter(strings);
        sw.Write("STBL"u8); sw.Write((byte)2); sw.Write((ushort)0); sw.Write(2u); sw.Write((ushort)0); sw.Write(0u);
        sw.Write(0x5501UL); sw.Write(11); sw.Write(Encoding.Unicode.GetBytes("Green Couch"));
        sw.Write(0x5502UL);
        var cut = strings.ToArray();
        sw.Write(0);
        Pkg(Path.Combine(pk, "Couches.package"), (ModKinds.Stbl, 0, 1, strings.ToArray()), (ModKinds.Objd, 0, 0x9C22000000000055, ModKindsTests.ObjdBytes(0x16, 0x800, 0, name: 0x5501)));
        Pkg(Path.Combine(pk, "Broken.package"), (ModKinds.Stbl, 0, 1, [.. cut, .. BitConverter.GetBytes(0x7FFFFFFF)]), (ModKinds.Objd, 0, 2, [1]));
        mods = Mine();
        var couch = ModFind.Search(ModFind.Read(Write(Path.Combine(root, "couch.txt"), "green couch")), mods, docs, gi);
        check(couch.Matches.Single().Hits.Single().Rel.EndsWith("Couches.package"), "find: a catalog name matches the package that has it, beside a damaged package");

        // names: the package's name with or without its extension, the hash of a resource's name
        var named = Path.Combine(pk, "Comfy Set.package");
        Pkg(named, (Xml, 0, 0x0A0B2A683D41D37FUL /* FNV-64 of apex_tuning */, [1]));
        mods = Mine();
        var byName = ModFind.Search(ModFind.Read(Write(Path.Combine(root, "names.txt"), "comfy set\nApex_Tuning\nSomething else")), mods, docs, gi);
        check(byName.Matches.Select(m => ModFind.StateOf(m, On)).SequenceEqual([FindState.Found, FindState.Found, FindState.Missing]), "find: names match a package's name or a resource named by its hash");
        var byFile = ModFind.Search(ModFind.Read(Write(Path.Combine(root, "files.txt"), "Hair Two.package\nsofas.PACKAGE\nno.package")), mods, docs, gi);
        check(byFile.Matches.Select(m => ModFind.StateOf(m, On)).SequenceEqual([FindState.Found, FindState.Found, FindState.Missing]) && byFile.Matches[0].Hits.Single().Inside == "Hair Two",
            "find: a package's file name matches a mod, also one inside a merge");

        var cts = new CancellationTokenSource();
        cts.Cancel();
        var stopped = false;
        try { ModFind.Search(list, mods, docs, gi, null, cts.Token); } catch (OperationCanceledException) { stopped = true; }
        check(stopped, "find: the search stops when cancelled");
    }

    sealed class Now(Action<double> report) : IProgress<double> { public void Report(double v) => report(v); }

    static void Caches(Action<bool, string> check, string docs)
    {
        void Make()
        {
            foreach (var n in new[] { @"DCCache\dcc.ent", @"DCCache\dcdb0.dbc.tmp", @"DCBackup\0xabc.package", "CASPartCache.package", "compositorCache.package", "scriptCache.package", @"WorldCaches\Riverview_objects.package", @"Thumbnails\CASThumbnails.package", "socialCache.package" })
                Write(Path.Combine(docs, n), "cache " + n);
            foreach (var n in new[] { @"DCCache\dcdb0.dbc", @"DCBackup\ccmerged.package", "DeviceConfig.log" }) Write(Path.Combine(docs, n), "keep " + n);
        }
        Make();
        const string error = "-- Script Errors --\n\n System.NullReferenceException: x\n#0: 0x0001d callvirt   in Fixture.Fixture.Entry:Init () ()\n";
        string Log(string name, DateTime when) { var p = Write(Path.Combine(docs, name), error); File.SetLastWriteTime(p, when); return p; }
        var old = Log("ScriptError_PC__old.xml", DateTime.Now.AddDays(-9));
        var before = Log("SomeMod_Error_1.xml", DateTime.Now.AddDays(-2));
        var last = Log("ScriptError_PC__last.xml", DateTime.Now.AddHours(-1));
        var crash = Log("xcpt PC 26-01-01 10.00.00.txt", DateTime.Now.AddDays(-9));
        var all = Enum.GetValues<CacheGroup>();
        check(GameCaches.List(docs).All(c => c.Group != CacheGroup.ErrorLogs) && GameCaches.List(docs).Count == 8 && GameCaches.List(docs, all).Select(c => c.Group).Distinct().Count() == all.Length,
            "caches: one click lists every cache and no error log; each group lists its own");
        check(!GameCaches.List(docs, all).Any(c => Path.GetFileName(c.Path) is "dcdb0.dbc" or "ccmerged.package" or "DeviceConfig.log")
              && GameCaches.List(docs).All(c => c.Group != CacheGroup.ContentBackups),
            "caches: the store content, the merged content and the log Health reads are never cleared; content backups only when chosen");
        var launcherLog = Write(Path.Combine(docs, "Sims3LauncherLogFile.log"), "log");
        check(GameCaches.List(docs, [CacheGroup.ErrorLogs]).Any(c => c.Path == launcherLog), "caches: the game's own logs go with the old error logs");
        File.Delete(launcherLog);
        check(GameCaches.List(docs, [CacheGroup.ErrorLogs]).Select(c => c.Path).SequenceEqual([old]), "caches: without a recorded game, only error logs over a week old are old");
        SessionHistory.Add(new(DateTime.Now.AddDays(-1), TimeSpan.FromMinutes(5), 1L << 30, SessionEnd.Crash, -1));
        check(GameCaches.List(docs, [CacheGroup.ErrorLogs]).Select(c => c.Path).Order().SequenceEqual(new[] { old, before }.Order()), "caches: the error logs before the last game are old; a crash log never is");

        string Memory() => string.Join("|", ScriptErrors.Read(docs, ScriptErrors.Since()).Errors.Select(e => e.Log + e.When + e.Mod));
        var memory = Memory();
        Games.IsRunning = () => true;
        var refused = false;
        try { GameCaches.Clean(docs, [CacheGroup.Scripts]); } catch (SetupException) { refused = true; }
        Games.IsRunning = () => false;
        check(refused && File.Exists(Path.Combine(docs, "scriptCache.package")), "caches: clearing chosen caches is refused while the game runs");

        var moved = GameCaches.Clean(docs, [CacheGroup.Scripts, CacheGroup.ErrorLogs]);
        check(moved > 0 && !File.Exists(Path.Combine(docs, "scriptCache.package")) && !File.Exists(old) && !File.Exists(before) && File.Exists(last) && File.Exists(crash) && File.Exists(Path.Combine(docs, "CASPartCache.package")),
            "caches: only the chosen ones are cleared");
        check(Memory() == memory && memory.Length > 0, "caches: Health remembers the same script errors after their old logs are cleared");
        check(GameCaches.Restore() && File.Exists(old) && File.Exists(before) && File.Exists(Path.Combine(docs, "scriptCache.package")) && GameCaches.Backup is null, "caches: what was cleared goes back");

        // a second clear keeps the first one's copy of what it does not touch
        GameCaches.Clean(docs, [CacheGroup.Scripts, CacheGroup.ErrorLogs]);
        Write(Path.Combine(docs, "scriptCache.package"), "newer");
        Thread.Sleep(5);
        GameCaches.Clean(docs, [CacheGroup.Scripts, CacheGroup.Worlds]);
        check(GameCaches.Restore() && File.Exists(old) && File.ReadAllText(Path.Combine(docs, "scriptCache.package")) == "newer" && File.Exists(Path.Combine(docs, @"WorldCaches\Riverview_objects.package")) && GameCaches.Backup is null,
            "caches: a second clear loses nothing of the first, and the newest copy goes back");

        // file by file: only the files chosen go, and a file that is not a cache (or never cleared) is never moved
        var world = GameCaches.List(docs, all).Single(c => c.Group == CacheGroup.Worlds);
        var thumbs = GameCaches.List(docs, all).Single(c => c.Group == CacheGroup.Thumbnails);
        var store = new CacheFile(Path.Combine(docs, @"DCCache\dcdb0.dbc"), @"DCCache\dcdb0.dbc", 1, CacheGroup.Leftovers);
        var merged = new CacheFile(Path.Combine(docs, @"DCBackup\ccmerged.package"), @"DCBackup\ccmerged.package", 1, CacheGroup.ContentBackups);
        Games.IsRunning = () => true;
        refused = false;
        try { GameCaches.Clean(docs, [world]); } catch (SetupException) { refused = true; }
        Games.IsRunning = () => false;
        check(refused && File.Exists(world.Path), "caches: clearing chosen files is refused while the game runs");
        var one = GameCaches.Clean(docs, [world, store, merged]);
        check(one == world.Size && !File.Exists(world.Path) && File.Exists(thumbs.Path) && File.Exists(store.Path) && File.Exists(merged.Path) && GameCaches.Backup is not null,
            "caches: file by file, only the chosen caches go; the store content and the merged content stay even when asked");
        check(GameCaches.Restore() && File.Exists(world.Path) && GameCaches.Backup is null && GameCaches.Clean(docs, Array.Empty<CacheFile>()) == 0 && GameCaches.Backup is null,
            "caches: a file cleared alone goes back; nothing chosen moves nothing");
        var wait = Fs.RetryDelay;
        Fs.RetryDelay = TimeSpan.Zero;
        var held = false;
        using (new FileStream(thumbs.Path, FileMode.Open, FileAccess.Read, FileShare.None))
            try { GameCaches.Clean(docs, [world, thumbs]); } catch (IOException) { held = true; }
        Fs.RetryDelay = wait;
        check(held && File.Exists(world.Path) && File.Exists(thumbs.Path) && GameCaches.Backup is null, "caches: file by file, one in use leaves every chosen file where it was");

        // a move that fails undoes the ones before it
        var delay = Fs.RetryDelay;
        Fs.RetryDelay = TimeSpan.Zero;
        var failed = false;
        using (new FileStream(Path.Combine(docs, "compositorCache.package"), FileMode.Open, FileAccess.Read, FileShare.None))
            try { GameCaches.Clean(docs); } catch (IOException) { failed = true; }
        Fs.RetryDelay = delay;
        check(failed && GameCaches.List(docs).Count == 8 && GameCaches.Backup is null, "caches: a cache in use leaves every cache where it was");
        GameCaches.Clean(docs, all);
        SessionHistory.Add(new(DateTime.Now, TimeSpan.FromMinutes(5), 1L << 30, SessionEnd.Normal, 0));
        check(GameCaches.Backup is null, "caches: the copy is dropped after a game that closed normally");
        File.Delete(Path.Combine(Paths.Data, "sessions.json"));
    }

    // every reference of the game's own files is in the game's own index, or the reader is wrong about a format
    static void Real(string folder)
    {
        var g = Games.FromFolder(folder);
        if (g is null) { Console.WriteLine("REAL find: no game in " + folder); return; }
        var gi = DefaultReplacements.Index(g);
        foreach (var f in Directory.EnumerateFiles(g.Root, "*", SearchOption.AllDirectories).Where(f => f.Contains("NonPackaged") && (f.EndsWith(".package") || f.EndsWith(".world"))))
        {
            var clock = System.Diagnostics.Stopwatch.StartNew();
            try
            {
                var r = ModFind.Search(ModFind.Read(f), [], "", gi);
                var missing = r.Matches.Where(m => ModFind.StateOf(m, _ => null) == FindState.Missing).ToList();
                Console.WriteLine($"REAL find {Path.GetFileName(f)}: {r.Matches.Count} items, {missing.Count} missing ({missing.Count(m => ModFind.MayBeGame(r, m.Item))} of them small), {clock.Elapsed.TotalSeconds:0.0} s{(r.Source.Truncated ? ", cut" : "")} {string.Join(" ", missing.Take(3).Select(m => m.Item.Key))}");
            }
            catch (SetupException e) { Console.WriteLine($"REAL find {Path.GetFileName(f)}: {e.Key}"); }
        }
    }
}
