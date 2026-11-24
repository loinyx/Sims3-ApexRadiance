// Offline checks of one package's content and of the data it shares with other mods (ApexSetup.Core/ModInspect.cs) on
// packages made here, and that what it says matches the conflict scan Health uses.
using System.Text;
using ApexSetup;

static class ModInspectTests
{
    const uint Thum = 0x0580A2B6, CasThum = 0x626F60CD, Img = 0x00B2D882;

    static byte[] Stbl(params (ulong Key, string Text)[] s)
    {
        var ms = new MemoryStream();
        var w = new BinaryWriter(ms);
        w.Write("STBL"u8); w.Write((byte)2); w.Write((ushort)0); w.Write((uint)s.Length); w.Write((ushort)0); w.Write(0u);
        foreach (var (k, t) in s) { w.Write(k); w.Write(t.Length); w.Write(Encoding.Unicode.GetBytes(t)); }
        if (ms.Length < 21) w.Write(new byte[4]);
        return ms.ToArray();
    }

    static byte[] NameMap(params (ulong Id, string Name)[] l)
    {
        var ms = new MemoryStream();
        var w = new BinaryWriter(ms);
        w.Write(1u); w.Write(l.Length);
        foreach (var (id, n) in l) { var b = Encoding.UTF8.GetBytes(n); w.Write(id); w.Write(b.Length); w.Write(b); }
        return ms.ToArray();
    }

    static byte[] Casp(string name, uint clothing)
    {
        var ms = new MemoryStream();
        var w = new BinaryWriter(ms);
        w.Write(0x12u); w.Write(0); w.Write(0);
        var b = Encoding.BigEndianUnicode.GetBytes(name);
        w.Write((byte)b.Length); w.Write(b); w.Write(0f); w.Write((byte)0);
        w.Write(clothing); w.Write(0u); w.Write(0x20u); w.Write(new byte[16]);
        return ms.ToArray();
    }

    static (uint, uint, ulong, byte[]) Xml(ulong i, string s) => (ModConflicts.TypeXml, 0u, i, Encoding.UTF8.GetBytes(s));
    static byte[] Pkg(params (uint, uint, ulong, byte[])[] r) => ModConflicts.WritePackage(r);

    public static void Run(Action<bool, string> check)
    {
        var tmp = Directory.CreateTempSubdirectory("ApexInspect-").FullName;
        try { Content(check, tmp); Shared(check, Path.Combine(tmp, "Docs")); }
        finally { Directory.Delete(tmp, true); }
    }

    static void Content(Action<bool, string> check, string tmp)
    {
        var path = Path.Combine(tmp, "content.package");
        File.WriteAllBytes(path, Pkg(
            (ModKinds.Objd, 0, 0xA1, ModKindsTests.ObjdBytes(0x1A, 1u << 11, 0, name: 0x1111, png: 0x77)),
            (ModKinds.Objd, 0, 0xA2, ModKindsTests.ObjdBytes(0x1A, 1u << 11, 0, name: 0x2222, png: 0)),
            (ModKinds.Objd, 0, 0xA3, [1, 2, 3]),
            (ModKinds.Casp, 0, 0xB1, Casp("amTopShirt", 0x05)),
            (ModKinds.Stbl, 0, 0x00UL << 56 | 9, Stbl((0x1111, "Stove"), (0x1112, "d"), (0x2222, "Pan"))),
            (ModKinds.Stbl, 0, 0x11UL << 56 | 9, Stbl((0x1111, "Fogão"), (0x1112, "d"))),
            (ModKinds.Stbl, 0, 0x10UL << 56 | 9, Stbl((0x1111, "Fogão (PT)"))),
            (0x2E75C766, 0, 0x77, [1]), (Thum, 0, 0xA2, [2]), (0x0580A2B4, 0, 0xA2, [3]), (CasThum, 0, 0xB1, [4]), (Thum, 0, 0xEE, [5]),
            (ModKinds.Nmap, 0, 0, NameMap((0xA3, "broken\r\none"), (0xB1, "map name"), (0x55, "Tune"))),
            Xml(0x55, "<x/>")));
        var c = ModInspect.Read(path, "pt");
        string?[] names = c.Items.Select(i => i.Name).ToArray();
        check(names.SequenceEqual(["broken  one", "Fogão", "Pan", "amTopShirt", null]), "content: objects by the player's language, then English, then the name map; parts; loose pictures: " + string.Join(" | ", names));
        check(c.Items[1].Picture is { Type: 0x2E75C766 } && c.Items[2].Picture is { Type: Thum } && c.Items[3].Picture is { Type: CasThum } && c.Items[0].Picture is null && c.Items[4] is { Kind: null, Picture.Instance: 0xEE },
            "content: each item's own picture, the best one once, and one that belongs to no item");
        check(c.Parts.OrderBy(p => p.Kind).SequenceEqual([("Object", 3), ("Top", 1)]) && c.Items[3].Kind == Kind.Clothing && c.Items[2].Kind == Kind.BuyComfort, "content: how many of each kind");
        check(c.Resources.Count == 14 && c.Resources[^1] is { Name: "Tune", Tag: "_XML", Key: "0333406C:00000000:0000000000000055" } && ModInspect.Export([c.Resources[^1], c.Resources[0]])
            == $"0333406C:00000000:0000000000000055\t_XML\t4\tTune{Environment.NewLine}319E4F1D:00000000:00000000000000A1\tOBJD\t{c.Resources[0].Entry.Size}{Environment.NewLine}", "content: every resource with its tag, key and name, and as text");
        check(ModInspect.Read(path, "fr").Items.Any(i => i.Name == "Stove"), "content: English where the player's language is missing");

        var cut = File.ReadAllBytes(path);
        "DBPP"u8.CopyTo(cut);
        File.WriteAllBytes(path, cut);
        check(ModInspect.Read(path, "en") is { Protected: true, Resources.Count: 0 } && ModInspect.Read(Path.Combine(tmp, "none.package"), "en") is { Protected: false, Resources.Count: 0 }, "content: a protected package and a missing one show nothing");
        // damaged data of every kind never throws out
        "DBPF"u8.CopyTo(cut);
        var random = new Random(7);
        string? threw = null;
        for (var i = 0; i < 300; i++)
        {
            var b = (byte[])cut.Clone();
            for (var n = 0; n < 1 + i % 40; n++) b[random.Next(b.Length)] = (byte)random.Next(256);
            File.WriteAllBytes(path, i % 5 == 0 ? b[..random.Next(b.Length)] : b);
            try { ModInspect.Read(path, "pt"); } catch (Exception e) { threw = e.ToString(); }
        }
        check(threw is null, "content: a damaged package never throws out " + threw);
        // (a length that overflows when doubled)
        var huge = Stbl((1, "x"));
        BitConverter.GetBytes(0x60000000).CopyTo(huge, 17 + 8);
        check(!ModContent.Strings(huge).Any(), "content: a string table with an impossible length gives no strings");
        var cancelled = false;
        File.WriteAllBytes(path, cut);
        try { ModInspect.Read(path, "pt", null, new CancellationToken(true)); } catch (OperationCanceledException) { cancelled = true; }
        check(cancelled, "content: stops when another mod is picked");
    }

    static void Shared(Action<bool, string> check, string docs)
    {
        var mods = Path.Combine(docs, "Mods");
        ModFile Put(string under, byte[] bytes)
        {
            var p = Path.Combine(mods, under);
            Directory.CreateDirectory(Path.GetDirectoryName(p)!);
            File.WriteAllBytes(p, bytes);
            return new(p, Path.Combine("Mods", under), bytes.Length, ModKind.Package, true, false);
        }
        Directory.CreateDirectory(mods);
        File.WriteAllText(Path.Combine(mods, "Resource.cfg"), "Priority 500\nPackedFile Packages/*.package\nPackedFile Packages/*/*.package\nPriority 1000\nPackedFile Overrides/*.package\n");
        byte[] img = [.. Enumerable.Repeat((byte)1, 400)], other = [.. Enumerable.Repeat((byte)2, 400)];
        (uint, uint, ulong, byte[]) map = (ModKinds.Nmap, 0, 0, NameMap((1, "a"))), map2 = (ModKinds.Nmap, 0, 0, NameMap((1, "b")));
        var a = Put(@"Packages\a.package", Pkg(Xml(1, "a"), Xml(2, "same"), (Img, 0, 3, img), (Img, 0, 4, [1]), map));
        var b = Put(@"Packages\b.package", Pkg(Xml(1, "b"), Xml(2, "same"), (Img, 0, 3, other), (Img, 0, 4, [2]), map2));
        var copy = Put(@"Packages\copy.package", Pkg(Xml(2, "same")));
        var o = Put(@"Overrides\o.package", Pkg(Xml(1, "o")));
        var deep = Put(@"Packages\x\y\deep.package", Pkg(Xml(1, "deep")));
        var cc = Put(@"Packages\ccmerged.package", Pkg(Xml(1, "cc")));
        var alone = Put(@"Packages\alone.package", Pkg(Xml(77, "x"), Xml(77, "y")));
        var off = Put(@"Packages\off.package", Pkg(Xml(1, "off"))) with { Enabled = false };
        List<ModFile> all = [a, b, copy, o, deep, cc, alone, off];
        var shared = new SharedData(docs, all);
        string Show(List<SharedMod> l) => string.Join(" ", l.OrderBy(x => x.Mod.Name, StringComparer.Ordinal).Select(x => $"{Path.GetFileNameWithoutExtension(x.Mod.Name)}{(x.Wins ? ">" : "<")}{string.Join(",", x.Different.OrderBy(d => d.Kind).Select(d => $"{d.Kind}{d.Count}"))}={x.Identical}"));
        check(Show(shared.Of(a)) == "b>Tuning1,Texture1=2 copy>=1 deep>Tuning1=0 o<Tuning1=0", "shared: who counts by load order, what differs by kind, what is the same: " + Show(shared.Of(a)));
        check(Show(shared.Of(b)) == "a<Tuning1,Texture1=2 copy>=1 deep>Tuning1=0 o<Tuning1=0" && Show(shared.Of(o)) == "a>Tuning1=0 b>Tuning1=0 deep>Tuning1=0", "shared: the same seen from the other mods");
        check(shared.Of(off).Count == 0 && shared.Of(alone).Count == 0 && Show(shared.Of(deep)) == "a<Tuning1=0 b<Tuning1=0 o<Tuning1=0", "shared: a mod that is off, one alone, one the game does not load");

        // the packages a merge holds are asked about one by one
        var kept = Path.Combine(docs, "kept.package");
        File.WriteAllBytes(kept, Pkg((Img, 0, 3, img), map));
        check(Show(shared.Of(a, kept)) == "b>Texture1=0", "shared: one package inside a merge answers for its own data only");

        // what the scan says, with the scan's own names
        var scan = ModConflicts.Scan(new() { Roots = [Path.Combine(mods, "Packages")] });
        string[] FilesOf(string[] parties) => parties.SelectMany(p => all.Where(m => Path.GetFileNameWithoutExtension(m.Path) == p)).Select(m => m.Path).ToArray();
        var found = scan.Conflicts.Where(x => !x.Parties.Contains("ccmerged")).Select(x => ModConflict.Of(new(x.Kind, x.Text, x.Parties, null), FilesOf(x.Parties))).ToList();
        var pairs = found.Where(x => x.Raw.StartsWith("resource key:")).SelectMany(x => x.Files.SelectMany(p => x.Files.Where(q => string.CompareOrdinal(p, q) < 0).Select(q => (p, q)))).ToHashSet();
        var inPackages = new[] { a, b, copy, deep, alone };
        var mine = inPackages.SelectMany(m => shared.Of(m).Where(x => x.Conflict && inPackages.Contains(x.Mod)).Select(x => (m.Path, x.Mod.Path))).Select(x => string.CompareOrdinal(x.Item1, x.Item2) < 0 ? x : (x.Item2, x.Item1)).ToHashSet();
        check(pairs.Count > 0 && pairs.IsSubsetOf(mine), "shared: every conflict of Health's scan between two mods is here");
        var rows = shared.Of(a);
        SharedMod Row(ModFile m) => rows.Single(r => r.Mod == m);
        check(ModInspect.InHealth(found, a, Row(b)) is not null && ModInspect.InHealth(found, a, Row(copy)) is null && ModInspect.InHealth(found, a, Row(o)) is null,
            "shared: a conflict is tied to Health's, a copy and one settled by a folder of priority are not");

        // what only Health finds
        var script = new ModConflict("i", ConflictLevel.Small, "t", "s", [], ["a", "b"], null, [a.Path, b.Path], "interaction: x");
        var archive = script with { Parties = ["a", "zipped"], Files = [a.Path], Raw = "resource key: x" };
        var gone = script with { Parties = ["a", "off"], Files = [a.Path, off.Path] };
        check(ModInspect.OnlyInHealth([script, archive, gone, .. found], a, all).SequenceEqual([script, archive]) && ModInspect.OnlyInHealth([script], o, all).Count == 0,
            "shared: scripts and archives come from Health's check, a mod turned off since does not");

        // priority, as the tab gives it: the mod goes first and no longer has Health's conflict
        var moved = ModPriority.Apply(docs, ModPriority.Moves(docs, b, [a])!);
        List<ModFile> now = [a, moved[0].Mod];
        var after = new SharedData(docs, now).Of(moved[0].Mod);
        check(after is [{ Wins: true, Conflict: true }] && ModInspect.InHealth(found, moved[0].Mod, after[0]) is null, "shared: with priority it counts over the other");
        ModPriority.Back(docs, moved);
        check(File.Exists(b.Path), "shared: the undo puts the file back");
        File.Delete(Path.Combine(mods, "Resource.cfg"));
        check(Show(new SharedData(docs, [a, b]).Of(a)) == "b>Tuning1,Texture1=2", "shared: without Resource.cfg, by name");
    }
}
