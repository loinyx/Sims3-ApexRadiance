// Offline checks of default replacement detection (ApexSetup.Core/DefaultReplacements.cs) on a fake game folder: the
// key index and its cache, a mod's grouping, the game's English names, original thumbnails and damaged packages.
using System.Text;
using ApexSetup;

static class DefaultReplacementsTests
{
    const uint Thum = 0x0580A2B5, CasThumb = 0x626F60CD, Xml = 0x0333406C;
    static readonly byte[] Png = [0x89, 0x50, 0x4E, 0x47, 1, 2, 3];
    static readonly byte[] CasPng = [0x89, 0x50, 0x4E, 0x47, 9, 9];

    static byte[] Stbl(params (ulong Key, string Text)[] s)
    {
        var ms = new MemoryStream();
        var w = new BinaryWriter(ms);
        w.Write("STBL"u8); w.Write((byte)2); w.Write((ushort)0); w.Write((uint)s.Length); w.Write((ushort)0); w.Write(0u);
        foreach (var (k, t) in s) { w.Write(k); w.Write(t.Length); w.Write(Encoding.Unicode.GetBytes(t)); }
        return ms.ToArray();
    }

    static void Write(string path, params (uint Type, uint Group, ulong Instance, byte[] Data)[] r)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        File.WriteAllBytes(path, ModConflicts.WritePackage(r));
    }

    public static void Run(Action<bool, string> check)
    {
        var dir = Path.Combine(Path.GetTempPath(), "apex-defaults-" + Guid.NewGuid().ToString("N"));
        var oldLocal = Paths.LocalAppData;
        var oldRegistry = Expansions.RegistryDir;
        Paths.LocalAppData = Path.Combine(dir, "Local");
        Expansions.RegistryDir = _ => null;
        try { Checks(dir, check); }
        finally
        {
            Paths.LocalAppData = oldLocal;
            Expansions.RegistryDir = oldRegistry;
            try { Directory.Delete(dir, true); } catch (IOException) { }
        }
    }

    static void Checks(string dir, Action<bool, string> check)
    {
        var root = Path.Combine(dir, "The Sims 3");
        var g = new Game(root, Path.Combine(root, "Game", "Bin"), "TS3W.exe", Edition.Steam, "1.67.2");
        var full = Path.Combine(root, "GameData", "Shared", "Packages", "FullBuild0.package");
        (uint, uint, ulong, byte[])[] game =
        [
            (ModKinds.Objd, 0, 0x100, ModKindsTests.ObjdBytes(0x16, 0x800, 0, name: 0xAAA)),
            (ModKinds.Objd, 0, 0x101, ModKindsTests.ObjdBytes(0x16, 0x800, 0, name: 0xBBB)), // (no English name: its internal name)
            (ModKinds.Casp, 0, 0x200, ModKindsTests.Casp(0x01, 0x0120)),
            (ModKinds.Img, 0, 0x300, [1, 2, 3]),
            (ModKinds.Nmap, 0, 0, [0]),
            (ModKinds.Stbl, 0, 0x0000000000000001, Stbl((0xAAA, "Game Sofa"), (0xAAB, "A sofa"))),
            (ModKinds.Stbl, 0, 0x0100000000000001, Stbl((0xAAA, "Spiel Sofa"), (0xBBB, "Andere"))),
        ];
        Write(full, game);
        Write(Path.Combine(root, "Thumbnails", "AllThumbnails.package"), (Thum, 0, 0x100, Png));
        Write(Path.Combine(root, "Thumbnails", "CasThumbnails.package"), (CasThumb, 1, 0x200, [0]), (CasThumb, 0, 0x200, CasPng));
        Write(Path.Combine(g.Bin, "gameplay.package"), (Xml, 0, 0x400, [1]));
        Write(Path.Combine(root, "EP1", "GameData", "Shared", "Packages", "FullBuild_p01.package"), (ModKinds.Objd, 0, 0x500, ModKindsTests.ObjdBytes(0x16, 0x20, 0, name: 0xCCC)));
        Directory.CreateDirectory(Path.Combine(root, "GameData", "Shared", "NonPackaged", "Library"));
        Write(Path.Combine(root, "GameData", "Shared", "NonPackaged", "Library", "house.package"), (ModKinds.Objd, 0, 0x600, [0])); // (a household, not the game's resources)

        // the index
        check(DefaultReplacements.Cached(g) is null, "defaults: no cached game index at first");
        var reports = new List<double>();
        var gi = DefaultReplacements.Index(g, new SyncProgress(reports.Add));
        check(gi.Contains(ModKinds.Objd, 0, 0x100) && gi.Contains(ModKinds.Casp, 0, 0x200) && gi.Contains(Xml, 0, 0x400) && gi.Contains(Thum, 0, 0x100)
            && gi.Contains(ModKinds.Objd, 0, 0x500), "defaults: the index holds the base game's, Game\\Bin's, the thumbnails' and a pack's keys");
        check(!gi.Contains(ModKinds.Objd, 1, 0x100) && !gi.Contains(ModKinds.Objd, 0, 0x600) && !gi.Contains(ModKinds.Nmap, 0, 0),
            "defaults: another group is another key; the Library and name maps are not in it");
        check(reports.Count > 0 && reports[^1] == 1.0, "defaults: progress reaches 1");
        var cached = DefaultReplacements.Cached(g);
        check(cached is not null && cached.Count == gi.Count && cached.Contains(ModKinds.Objd, 0, 0x100), "defaults: the index is cached and read back");

        // a game package changes: the cache no longer matches
        game = [.. game, (ModKinds.Objd, 0, 0x700, ModKindsTests.ObjdBytes(0x16, 0x800, 0))];
        Write(full, game);
        File.SetLastWriteTimeUtc(full, DateTime.UtcNow.AddMinutes(5));
        check(DefaultReplacements.Cached(g) is null, "defaults: a changed game package invalidates the cache");
        gi = DefaultReplacements.Index(g);
        check(gi.Contains(ModKinds.Objd, 0, 0x700) && DefaultReplacements.Cached(g) is not null, "defaults: rebuilt with the new key and cached again");
        using (var cancel = new CancellationTokenSource())
        {
            File.SetLastWriteTimeUtc(full, DateTime.UtcNow.AddMinutes(10));
            cancel.Cancel();
            var stopped = false;
            try { DefaultReplacements.Index(g, null, cancel.Token); } catch (OperationCanceledException) { stopped = true; }
            check(stopped && DefaultReplacements.Cached(g) is null, "defaults: a cancelled build stops and caches nothing");
            gi = DefaultReplacements.Index(g);
        }

        // a mod: a replaced object with its thumbnail, a replaced CAS part, a texture, tuning, a new object, a name map
        var mod = Path.Combine(dir, "Mods", "mod.package");
        Write(mod,
            (ModKinds.Objd, 0, 0x100, ModKindsTests.ObjdBytes(0x16, 0x800, 0, name: 0x1, png: 0x77)),
            (Thum, 0, 0x100, [7]),
            (ModKinds.Objd, 0, 0x101, ModKindsTests.ObjdBytes(0x16, 0x800, 0)),
            (ModKinds.Casp, 0, 0x200, ModKindsTests.Casp(0x01, 0x0120)),
            (ModKinds.Img, 0, 0x300, [9]),
            (Xml, 0, 0x400, [2]),
            (ModKinds.Objd, 0, 0x999, ModKindsTests.ObjdBytes(0x16, 0x800, 0)),
            (ModKinds.Nmap, 0, 0, [0]));
        var r = DefaultReplacements.Analyze(gi, mod);
        check(r.IsDefault && r.Resources == 7 && r.Replaced == 6 && Math.Abs(r.Share - 6.0 / 7) < 1e-9,
            $"defaults: 6 of the mod's 7 resources replace the game's (name map left out) {r.Replaced}/{r.Resources}");
        var sofa = r.Objects.FirstOrDefault(o => o.Instance == 0x100);
        check(r.Objects.Count == 2 && sofa is { Name: "Game Sofa", Kind: Kind.BuyComfort, Resources: 2, PngInstance: 0x77 }
            && r.Objects.Any(o => o is { Instance: 0x101, Name: "objectInstance" }),
            "defaults: the replaced object has the game's English name (not the mod's key), its kind and its thumbnail; one without an English name its internal name");
        check(r.CasParts is [{ Instance: 0x200, Name: "part", Kind: Kind.Hair }], "defaults: the replaced CAS part with its name and kind");
        check(r.Images == 1 && r.Other is [("XML tuning", 1)], "defaults: a texture counts as an image, tuning by its type name");
        check(File.Exists(Path.Combine(Paths.Data, "game-names.bin")), "defaults: the game's names are cached");

        // thumbnails: the game's own, not the mod's
        check(DefaultReplacements.OriginalThumbnail(gi, sofa!) is { } t && t.SequenceEqual(Png), "defaults: an object's original thumbnail comes from AllThumbnails");
        check(DefaultReplacements.OriginalThumbnail(gi, r.CasParts[0]) is { } c && c.SequenceEqual(CasPng), "defaults: a CAS part's from CasThumbnails, group 0 first");
        check(DefaultReplacements.OriginalThumbnail(gi, r.Objects.First(o => o.Instance == 0x101)) is null, "defaults: no thumbnail when the game has none");

        // names read back from the cache by a new index
        var again = DefaultReplacements.Cached(g)!;
        check(DefaultReplacements.Analyze(again, mod).Objects.Any(o => o.Name == "Game Sofa"), "defaults: names come back from their cache");

        // a mod with nothing of the game's
        var own = Path.Combine(dir, "Mods", "own.package");
        Write(own, (ModKinds.Objd, 0, 0x999, ModKindsTests.ObjdBytes(0x16, 0x800, 0)), (ModKinds.Objd, 5, 0x100, [1]));
        check(DefaultReplacements.Analyze(gi, own) is { IsDefault: false, Resources: 2, Share: 0 }, "defaults: a mod of new content only is no default replacement");

        // hostile packages and caches never throw
        var junk = Path.Combine(dir, "Mods", "junk.package");
        File.WriteAllBytes(junk, Enumerable.Range(0, 300).Select(i => (byte)(i * 7)).ToArray());
        var huge = Path.Combine(dir, "Mods", "huge.package");
        var h = ModConflicts.WritePackage([(ModKinds.Objd, 0u, 0x100ul, new byte[] { 1 })]);
        BitConverter.GetBytes(150_000u).CopyTo(h, 36); // (an index count far past the file's end)
        File.WriteAllBytes(huge, h);
        var truncated = Path.Combine(dir, "Mods", "truncated.package");
        var tr = ModConflicts.WritePackage([(ModKinds.Objd, 0u, 0x100ul, ModKindsTests.ObjdBytes(0x16, 0x800, 0)), (ModKinds.Casp, 0u, 0x200ul, new byte[] { 1, 2 })]);
        BitConverter.GetBytes(0x7FFFFFF0u).CopyTo(tr, tr.Length - 64 + 16); // (the first entry's offset past the end)
        File.WriteAllBytes(truncated, tr);
        var ok = true;
        foreach (var p in new[] { junk, huge, truncated, Path.Combine(dir, "missing.package") })
            try { DefaultReplacements.Analyze(gi, p); } catch (Exception e) { ok = false; Console.WriteLine("  " + p + ": " + e.GetType().Name); }
        check(ok && DefaultReplacements.Analyze(gi, truncated) is { CasParts: [{ Name: null }] }, "defaults: damaged packages are read as far as they go, never throw");
        File.WriteAllBytes(Path.Combine(root, "GameData", "Shared", "Packages", "FullBuild1.package"), [0x44, 0x42, 0x50, 0x46, 2, 0, 0, 0]);
        File.WriteAllBytes(Path.Combine(Paths.Data, "game-keys.bin"), [0x41, 0x58, 0x47, 0x4B, 1, 0, 0, 0, 1]);
        var rebuilt = false;
        try { rebuilt = DefaultReplacements.Index(g).Contains(ModKinds.Objd, 0, 0x100); } catch (Exception e) { Console.WriteLine("  " + e); }
        check(rebuilt, "defaults: a damaged game package and a damaged cache rebuild the index without throwing");
        File.WriteAllBytes(Path.Combine(Paths.Data, "game-names.bin"), [1, 2, 3]);
        check(DefaultReplacements.Analyze(DefaultReplacements.Cached(g)!, mod).Objects.Any(o => o.Name == "Game Sofa"), "defaults: a damaged names cache is built again");

        // what the game's launcher installed: items read from DCCache\dcdb*.dbc with their English names and thumbnails, the
        // .tmp copies left out
        var docs = Path.Combine(dir, "Docs");
        Write(Path.Combine(docs, InstalledContent.Folder, "dcdb0.dbc"),
            (ModKinds.Objd, 0, 0x900, ModKindsTests.ObjdBytes(0x16, 0x800, 0, name: 0xDDD)),
            (ModKinds.Stbl, 0, 0x0000000000000002, Stbl((0xDDD, "Store Lamp"))),
            (Thum, 0, 0x900, Png));
        Write(Path.Combine(docs, InstalledContent.Folder, "dcdb1.dbc.tmp"), (ModKinds.Objd, 0, 0x901, ModKindsTests.ObjdBytes(0x16, 0x800, 0, name: 0xDDD)));
        var installed = InstalledContent.Read(docs);
        check(installed.Count(i => i.Type == ModKinds.Objd) == 1 && installed[0].Name == "Store Lamp" && InstalledContent.Thumbnail(installed[0]) is { Length: > 0 }
              && InstalledContent.Read(Path.Combine(dir, "NoDocs")).Count == 0,
            "installed content: the game launcher's items are read from its cache by name with their thumbnail; .tmp copies and a missing cache give none");
    }

    // Progress<T> posts to the thread pool; the checks want the reports in order and done when Index returns
    sealed class SyncProgress(Action<double> report) : IProgress<double> { public void Report(double v) => report(v); }
}
