// Offline checks of merging packages (ApexSetup.Core/ModMerge.cs) on packages made here, in a temporary Documents folder.
using System.Buffers.Binary;
using System.Security.Cryptography;
using System.Text;
using ApexSetup;

static class ModMergeTests
{
    const uint S3sa = 0x073FAA07;

    // a DBPF 2.0 package: each resource with its type, instance, stored bytes, compression flag and unpacked size
    static void Pkg(string path, params (uint Type, ulong Inst, byte[] Data, bool Comp, int Mem)[] res)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(path)!);
        using var f = File.Create(path);
        var w = new BinaryWriter(f);
        w.Write(0x46504244u); w.Write(2u); w.Write(new byte[96 - 8]);
        var offs = new List<uint>();
        foreach (var r in res) { offs.Add((uint)f.Position); w.Write(r.Data); }
        var index = (uint)f.Position;
        w.Write(0u);
        for (var i = 0; i < res.Length; i++)
        {
            w.Write(res[i].Type); w.Write(0x10u); w.Write((uint)(res[i].Inst >> 32)); w.Write((uint)res[i].Inst);
            w.Write(offs[i]); w.Write((uint)res[i].Data.Length | 0x80000000u); w.Write((uint)res[i].Mem);
            w.Write(res[i].Comp ? (ushort)0xFFFF : (ushort)0); w.Write((ushort)1);
        }
        var size = (uint)(f.Position - index);
        f.Position = 0x24; w.Write((uint)res.Length);
        f.Position = 0x2C; w.Write(size);
        f.Position = 0x40; w.Write(index);
    }
    static (uint, ulong, byte[], bool, int) Raw(uint t, ulong i, string s) => (t, i, Encoding.UTF8.GetBytes(s), false, Encoding.UTF8.GetByteCount(s));

    // RefPack of a 4-byte pattern repeated: 4 literals, then copies at distance 4, as the game's compressor writes them
    static byte[] Pack(byte[] data)
    {
        var o = new List<byte> { 0x10, 0xFB, (byte)(data.Length >> 16), (byte)(data.Length >> 8), (byte)data.Length, 0xE0 };
        o.AddRange(data[..4]);
        for (var left = data.Length - 4; left > 0;)
        {
            var c = Math.Min(64, left);
            o.AddRange(new byte[] { (byte)(0x80 | (c - 4)), 0, 3 });
            left -= c;
        }
        o.Add(0xFC);
        return o.ToArray();
    }

    static string Sha(string p) => Convert.ToHexString(SHA256.HashData(File.ReadAllBytes(p)));
    // every file under a folder and its hash: the "no trace" check
    // (an antivirus may drop and remove its own NAME.PACKAGE.tmp while scanning: a file gone before it is read is not counted)
    static Dictionary<string, string> Snap(string dir)
    {
        var o = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        foreach (var f in Directory.EnumerateFiles(dir, "*", SearchOption.AllDirectories))
            try { o[Path.GetRelativePath(dir, f)] = Sha(f); } catch (FileNotFoundException) { }
        return o;
    }
    // what differs, for a failed check's message
    static string Diff(Dictionary<string, string> a, Dictionary<string, string> b, string docs) =>
        string.Join("; ", a.Keys.Union(b.Keys).Where(k => !a.TryGetValue(k, out var x) || !b.TryGetValue(k, out var y) || x != y)
            .Select(k => (a.ContainsKey(k) ? "" : "+") + (b.ContainsKey(k) ? "" : "-") + k))
        + (Directory.Exists(ModMerge.Folder(docs)) ? " [merge folder left: " + string.Join(", ", Directory.EnumerateFileSystemEntries(ModMerge.Folder(docs), "*", SearchOption.AllDirectories).Select(f => Path.GetRelativePath(docs, f))) + "]" : "");
    static bool Same(Dictionary<string, string> a, Dictionary<string, string> b) => a.Count == b.Count && a.All(x => b.TryGetValue(x.Key, out var v) && v == x.Value);

    static byte[] Stored(string path, ModConflicts.Entry e)
    {
        using var f = File.OpenRead(path);
        f.Position = e.Offset;
        var b = new byte[e.FileSize];
        f.ReadExactly(b);
        return b;
    }

    sealed class Sync(Action<MergeProgress> a) : IProgress<MergeProgress> { public void Report(MergeProgress v) => a(v); }

    // a copy of a real Documents folder (APEX_MERGE_REAL): everything mergeable in one merge, every resource of every mod that
    // went in is in the merged file with the same content, and the split brings each file back identical
    static void Real(Action<bool, string> check, string docs)
    {
        var before = Snap(docs);
        var mods = Mods.List(new Game(docs, docs, "TS3.exe", Edition.EA, "1.69.47"), docs);
        var plan = ModMerge.Plan(docs, mods, "Everything", "Packages", true);
        foreach (var s in plan.Skipped) Console.WriteLine($"  left out {s.Mod.Rel}: {s.Reason.Text}");
        Console.WriteLine($"REAL merge: {plan.Inputs.Count} of {mods.Count} mods, {plan.Resources} resources, {plan.Duplicates.Count} held alike, {string.Join(" ", plan.Refusals.Select(r => r.Text))}");
        static string Hash(string path, ModConflicts.Entry e, byte[] b) => e.Compressed == 0xFFFF ? Convert.ToHexString(SHA256.HashData(ModConflicts.RefPack(b))) : Convert.ToHexString(SHA256.HashData(b));
        var want = plan.Inputs.SelectMany(m => ModConflicts.Package.Open(m.Path).Entries.Where(e => e.Type is not (0x0166038C or 0x73E93EEB)).Select(e => ((e.Type, e.Group, e.Instance), Hash(m.Path, e, Stored(m.Path, e))))).ToHashSet();
        var res = ModMerge.Run(plan, null, default);
        var have = ModConflicts.Package.Open(res.Merged.Path).Entries.Select(e => ((e.Type, e.Group, e.Instance), Hash(res.Merged.Path, e, Stored(res.Merged.Path, e)))).ToHashSet();
        check(want.IsSubsetOf(have), $"REAL merge: all {want.Count} resources of the mods that went in are in it, with the same content ({want.Except(have).Count()} missing)");
        ModMerge.Split(docs, res.Merged);
        check(Same(before, Snap(docs)), "REAL merge: the split brings every file back identical");
    }

    public static void Run(Action<bool, string> check)
    {
        var root = Path.Combine(Path.GetTempPath(), "ApexMergeTest-" + Guid.NewGuid().ToString("N")[..8]);
        var oldLocal = Paths.LocalAppData;
        var oldRecycle = ModMerge.Recycle;
        var recycled = Path.Combine(root, "Recycle");
        try
        {
            Paths.LocalAppData = Path.Combine(root, "Local");
            ModNotes.Reload();
            Directory.CreateDirectory(recycled);
            ModMerge.Recycle = p => File.Move(p, Path.Combine(recycled, Guid.NewGuid().ToString("N")));
            var docs = Path.Combine(root, "Docs");
            var mods = Path.Combine(docs, "Mods");
            Directory.CreateDirectory(mods);
            File.WriteAllText(Path.Combine(mods, "Resource.cfg"), "Priority 500\nPackedFile Packages/*.package\nPackedFile Packages/*/*.package\nPriority 1000\nPackedFile Overrides/*.package\n");
            ModFile M(string rel, bool on = true) => new(Path.Combine(mods, rel), Path.Combine("Mods", rel), 0, ModKind.Package, on, false);

            if (Environment.GetEnvironmentVariable("APEX_MERGE_REAL") is { Length: > 0 } real) Real(check, real);

            var plain = Encoding.UTF8.GetBytes(string.Concat(Enumerable.Repeat("abcd", 50)));
            var packed = Pack(plain);
            check(RefPack.Decode(packed).AsSpan().SequenceEqual(plain) && ModConflicts.RefPack(packed).AsSpan().SequenceEqual(plain) && packed.Length < plain.Length,
                "merge: the test's RefPack stream decodes with both readers");
            var cut = false;
            try { RefPack.Decode([0x90, 0xFB, 0, 0, 1]); } catch (InvalidDataException) { cut = true; }
            check(cut, "refpack: a 5-byte stream with the large flag is refused as data, not an index error");
            string A = @"Packages\a.package", B = @"Overrides\b.package", S = @"Packages\s.package";
            Pkg(Path.Combine(mods, A), Raw(1, 1, "A1"), (2, 2, packed, true, plain.Length), Raw(3, 3, "share"));
            Pkg(Path.Combine(mods, B), Raw(3, 3, "share"), Raw(4, 4, "B4"));
            Pkg(Path.Combine(mods, S), (S3sa, 9, ModConflicts.S3saWrap(new byte[600]), false, 671));
            Pkg(Path.Combine(mods, @"Packages\~tmp.package"), Raw(5, 5, "t"));
            Pkg(Path.Combine(mods, @"Packages\Sub\off.package"), Raw(6, 6, "o"));
            // hostile inputs: cut short, index past the end, a resource past the end, a huge count, a protected package
            Pkg(Path.Combine(mods, @"Packages\trunc.package"), Raw(7, 7, new string('x', 400)));
            var trunc = File.ReadAllBytes(Path.Combine(mods, @"Packages\trunc.package"));
            File.WriteAllBytes(Path.Combine(mods, @"Packages\trunc.package"), trunc[..300]);
            void Patch(string rel, int at, uint v) { var p = Path.Combine(mods, rel); Pkg(p, Raw(8, 8, "hostile")); var b = File.ReadAllBytes(p); BinaryPrimitives.WriteUInt32LittleEndian(b.AsSpan(at), v); File.WriteAllBytes(p, b); }
            Patch(@"Packages\past.package", 0x40, 100_000);
            Patch(@"Packages\huge.package", 0x24, 0x7FFFFFFF);
            Patch(@"Packages\count.package", 0x24, 50); // (count beyond the index's size)
            var resPast = Path.Combine(mods, @"Packages\respast.package");
            Pkg(resPast, Raw(8, 8, "hostile"));
            { var b = File.ReadAllBytes(resPast); BinaryPrimitives.WriteUInt32LittleEndian(b.AsSpan((int)BinaryPrimitives.ReadUInt32LittleEndian(b.AsSpan(0x40)) + 4 + 16), 0x7FFF0000); File.WriteAllBytes(resPast, b); }
            File.WriteAllBytes(Path.Combine(mods, @"Packages\dbpp.package"), "DBPP"u8.ToArray().Concat(new byte[200]).ToArray());
            var hostile = new[] { "trunc", "past", "huge", "count", "respast", "dbpp" }.Select(n => M($@"Packages\{n}.package")).ToList();
            var asi = new ModFile(Path.Combine(root, "x.asi"), "x.asi", 1, ModKind.Asi, true, true);
            var all = new List<ModFile> { M(A), M(B), M(S), M(@"Packages\~tmp.package"), M(@"Packages\Sub\off.package", false), asi, M(A) with { Locked = true } };
            all.AddRange(hostile);
            var before = Snap(docs);

            // plan: load order, the copy the game loads kept, every skip with its reason, nothing written
            var plan = ModMerge.Plan(docs, all, "Merged One", "Packages", includeScripts: false);
            string Why(string rel) => plan.Skipped.FirstOrDefault(s => s.Mod.Rel == rel)?.Reason.Key ?? "";
            check(plan.CanRun && plan.Inputs.Select(m => m.Rel).SequenceEqual([Path.Combine("Mods", B), Path.Combine("Mods", A)]) && plan.Resources == 4
                && plan.Duplicates.Count == 1 && plan.Duplicates[0].Kept == Path.Combine("Mods", B) && plan.Duplicates[0].Dropped.SequenceEqual([Path.Combine("Mods", A)])
                && plan.OutputBytes == 96 + 5 + 2 + 2 + packed.Length + 4 + 4 * 32,
                "merge: plan in the game's load order, a resource both hold alike goes in once, exact size");
            check(Why(Path.Combine("Mods", S)).Contains("script") && Why(@"Mods\Packages\~tmp.package").Contains("temporary") && Why(@"Mods\Packages\Sub\off.package") == "Turned off."
                && Why("x.asi").Contains("Not a package") && plan.Skipped.Any(s => s.Mod.Locked && s.Reason.Key.Contains("launcher installs")),
                "merge: script mods (by default), temporary files, switched-off mods, non-packages and locked mods are skipped with a reason");
            check(hostile.All(h => Why(h.Rel).StartsWith("Can't read this package") || Why(h.Rel).Contains("protected")) && Same(before, Snap(docs)) && !Directory.Exists(ModMerge.Folder(docs)),
                $"merge: hostile packages (truncated, index or resource past the end, huge counts, protected) refused at plan time, nothing written {string.Join("; ", hostile.Select(h => Why(h.Rel)))}");
            check(ModMerge.Plan(docs, all, "x", "Packages", true).Inputs.Any(m => m.Rel.EndsWith("s.package")), "merge: script mods go in when asked");
            // keys 1 (a) and 4 (Overrides, priority 1000) also in a loose package: merged into Packages\x, c.package (before x)
            // would win both; z.package (after x) loses them as it does today
            List<MergeSkip> Left(string n, string c4 = "C4", string c1 = "C1")
            {
                var loose = Path.Combine(mods, $@"Packages\{n}.package");
                Pkg(loose, Raw(4, 4, c4), Raw(1, 1, c1));
                try { return ModMerge.Plan(docs, all, "x", "Packages", false).Skipped.Where(s => s.Reason.Key.StartsWith("Changes the same")).ToList(); } finally { File.Delete(loose); }
            }
            var c = Left("c");
            check(c.Count == 2 && c.All(x => x.Reason.Args[0] == "c") && Left("z").Count == 0 && Left("c", "B4", "A1").Count == 0,
                $"merge: a mod whose place against one outside the merge would change stays out; one that holds the same content does not ({c.Count})");
            // two that change one resource differently stay out, whichever the game would use; what is alike, packed or not, does not count
            string P(string n) => $@"Packages\{n}.package";
            Pkg(Path.Combine(mods, P("k1")), Raw(20, 20, "one"), Raw(21, 21, "k1"));
            Pkg(Path.Combine(mods, P("k2")), Raw(20, 20, "two"), Raw(22, 22, "k2"));
            Pkg(Path.Combine(mods, P("k3")), (2, 2, plain, false, plain.Length), Raw(23, 23, "k3"));
            Pkg(Path.Combine(mods, P("k4")), (2, 2, packed, true, plain.Length), Raw(24, 24, "k4"));
            var mixed = ModMerge.Plan(docs, [M(P("k1")), M(P("k2")), M(P("k3")), M(P("k4"))], "Mixed", "Packages", false);
            check(mixed.CanRun && mixed.Inputs.Select(m => m.Name).SequenceEqual(["k3.package", "k4.package"]) && mixed.Skipped.Count == 2 && mixed.Resources == 3
                && mixed.Skipped.Single(s => s.Mod.Name == "k1.package").Reason.Args[0] == "k2" && !ModMerge.Plan(docs, [M(P("k1")), M(P("k2"))], "x", "Packages", false).CanRun,
                "merge: mods that change the same resource differently stay out, each naming the other; the same content packed and unpacked goes in once");
            // tuning written another way (a byte order mark, a comment, the layout, the attribute order) is alike too; another value is not
            Pkg(Path.Combine(mods, P("x1")), Raw(0x0333406C, 30, "<a k=\"1\" j=\"2\"><v>5</v></a>"), Raw(31, 31, "x1"));
            Pkg(Path.Combine(mods, P("x2")), Raw(0x0333406C, 30, "\uFEFF<?xml version=\"1.0\"?>\r\n<a j=\"2\" k=\"1\">\r\n  <!-- five -->\r\n  <v>5</v>\r\n</a>"), Raw(32, 32, "x2"));
            var alike = ModMerge.Plan(docs, [M(P("x1")), M(P("x2"))], "x", "Packages", false);
            Pkg(Path.Combine(mods, P("x3")), Raw(0x0333406C, 30, "<a k=\"1\" j=\"2\"><v>6</v></a>"), Raw(33, 33, "x3"));
            var unlike = ModMerge.Plan(docs, [M(P("x1")), M(P("x3"))], "x", "Packages", false);
            foreach (var n in new[] { "x1", "x2", "x3" }) File.Delete(Path.Combine(mods, P(n)));
            check(alike.CanRun && alike.Skipped.Count == 0 && alike.Resources == 3 && !unlike.CanRun && unlike.Skipped.Count == 2,
                "merge: the same tuning written another way goes in once; one changed value keeps the two mods out");
            // the player's choice between them: both go in, the first one's copy counts, and it is kept through an edit
            foreach (var (first, other, text) in new[] { ("k1", "k2", "one"), ("k2", "k1", "two") })
            {
                var chosen = ModMerge.Plan(docs, [M(P("k1")), M(P("k2")), M(P("k3"))], "Chosen", "Packages", false, [Path.Combine("Mods", P(first)), Path.Combine("Mods", P(other))]);
                var done = ModMerge.Run(chosen, null, default).Merged;
                string Kept(string path) => Encoding.UTF8.GetString(Stored(path, ModConflicts.Package.Open(path).Entries.Single(e => e.Type == 20)));
                var keptAll = ModConflicts.Package.Open(done.Path).Entries.Count == 5 && Kept(done.Path) == text && chosen.Clashes.Single() is { Resources: 1, Mods.Count: 2 };
                var more = ModMerge.Edit(docs, done, [], [M(P("k4"))]).Remerged!.Merged;
                check(keptAll && chosen.Skipped.Count == 0 && Kept(more.Path) == text && ModMerge.Info(more)!.Originals.Count == 4 && ModMerge.Lost(docs).Count == 0,
                    $"merge: the copy of the mod with priority counts ({first}), the rest of each goes in, also after an edit");
                ModMerge.Split(docs, more);
            }
            // priority without a merge: up to the folder that comes first, else the others down; the undo puts each back
            var upM = ModPriority.Moves(docs, M(P("k2")), [M(P("k1"))]);
            check(upM is [var u] && u.Folder == "Overrides" && u.Mod.Name == "k2.package" && ModPriority.Moves(docs, M(B), [M(P("k1"))]) is { Count: 0 }
                && ModPriority.Moves(docs, M(P("k1")), [M(B)]) is [var up1, var dn] && up1.Folder == "Overrides" && dn.Folder == "Packages" && dn.Mod.Name == "b.package"
                && ModPriority.Moves(docs, M(P("k1")) with { Locked = true }, [M(P("k2"))]) is null,
                "priority: the mod goes to the folder Resource.cfg puts first, or the other one down when it is there already");
            var wasAll = Snap(docs);
            var put = ModPriority.Apply(docs, upM!);
            var there = File.Exists(Path.Combine(mods, @"Overrides\k2.package")) && !File.Exists(Path.Combine(mods, P("k2")));
            ModPriority.Back(docs, put);
            check(there && Same(wasAll, Snap(docs)), "priority: the file moves and the undo puts it back identical");
            // the packages' name lists become one with every name
            static byte[] Names(params (ulong Id, string N)[] l)
            {
                using var ms = new MemoryStream();
                using var w = new BinaryWriter(ms);
                w.Write(1u); w.Write(l.Length);
                foreach (var (id, n) in l) { w.Write(id); w.Write(n.Length); w.Write(Encoding.ASCII.GetBytes(n)); }
                return ms.ToArray();
            }
            (uint, ulong, byte[], bool, int) Map(byte[] b) => (0x0166038C, 0, b, false, b.Length);
            Pkg(Path.Combine(mods, P("k5")), Map(Names((1, "a"), (2, "b"))), Raw(25, 25, "k5"));
            Pkg(Path.Combine(mods, P("k6")), Map(Names((2, "other"), (3, "c"))), Raw(26, 26, "k6"));
            Pkg(Path.Combine(mods, P("k7")), Map([1, 0, 0, 0, 9, 0, 0, 0]), Raw(27, 27, "k7"));
            var named = ModMerge.Run(ModMerge.Plan(docs, [M(P("k5")), M(P("k6"))], "Named", "Packages", false), null, default).Merged;
            var map = ModConflicts.Package.Open(named.Path).Entries.Single(e => e.Type == 0x0166038C);
            check(Stored(named.Path, map).AsSpan().SequenceEqual(Names((1, "a"), (2, "b"), (3, "c"))), "merge: the packages' name lists are joined");
            ModMerge.Split(docs, named);
            check(ModMerge.Plan(docs, [M(P("k5")), M(P("k7"))], "Broken", "Packages", false) is { CanRun: true, Resources: 3 }, "merge: a damaged name list keeps the first one");
            // a health check in the middle of a merge leaves it alone
            var told = 0;
            ModMerge.FailPoint = x => { if (x == "move" && told++ == 0) { var t = new Thread(() => ModMerge.Recover(docs)); t.Start(); t.Join(); } };
            var busy = ModMerge.Run(ModMerge.Plan(docs, [M(P("k3")), M(P("k4"))], "Busy", "Packages", false), null, default).Merged;
            ModMerge.FailPoint = _ => { };
            check(told == 2 && File.Exists(busy.Path) && ModMerge.Info(busy)?.Originals.Count == 2 && !File.Exists(Path.Combine(mods, P("k3"))), "merge: a recovery during a merge does not touch it");
            // a merged file renamed outside the launcher is found again
            var moved = busy with { Path = Path.Combine(mods, P("My CC")), Rel = Path.Combine("Mods", P("My CC")) };
            File.Move(busy.Path, moved.Path);
            check(ModMerge.Orphans(docs).Count == 1, "merge: a merge whose file is not found keeps mods that are told");
            ModMerge.Recover(docs);
            check(ModMerge.IsMerged(moved) && ModMerge.Orphans(docs).Count == 0, "merge: a merged file renamed outside the launcher is found by its content");
            // one deleted outside the launcher: its mods are told and go back, beside a different file that took a place
            File.Delete(moved.Path);
            var own = File.ReadAllBytes(Path.Combine(ModMerge.Folder(docs), "Busy", P("k3")));
            File.WriteAllText(Path.Combine(mods, P("k3")), "newer");
            var told2 = ModMerge.Orphans(docs).Single();
            check(told2.Files.Count == 2 && ModMerge.PutBack(docs, told2) == 2 && File.ReadAllText(Path.Combine(mods, P("k3"))) == "newer" && File.Exists(Path.Combine(mods, P("k4")))
                && Directory.GetFiles(Path.Combine(mods, "Packages"), "k3*.package").Any(f => File.ReadAllBytes(f).AsSpan().SequenceEqual(own)) && ModMerge.Orphans(docs).Count == 0 && !Directory.Exists(ModMerge.Folder(docs)),
                "merge: the mods of a merge that is gone go back, beside a newer file of the same name");
            // a merge of an older launcher that kept one copy of what two mods hold differently: told, with the mod that lost it
            var older1 = ModMerge.Run(ModMerge.Plan(docs, [M(P("k5")), M(P("k4"))], "Lossy", "Packages", false), null, default).Merged;
            var store1 = Path.Combine(ModMerge.Folder(docs), "Lossy");
            var json1 = File.ReadAllText(Path.Combine(store1, "merge.json"));
            var k4Was = File.ReadAllBytes(Path.Combine(store1, P("k4")));
            Pkg(Path.Combine(store1, P("k4")), Raw(25, 25, "changed"));
            check(ModMerge.Lost(docs).Count == 0, "merge: one made now is not checked for lost parts");
            File.WriteAllText(Path.Combine(store1, "merge.json"), json1.Replace("\"Version\": 2", "\"Version\": 1").Replace("\"Version\":2", "\"Version\":1"));
            ModMerge.Recover(docs);
            check(ModMerge.Lost(docs) is [var lost1] && lost1.Mods.SequenceEqual(["k4"]) && lost1.Merged.Path == older1.Path, "merge: an older one that lost a part says which mod's");
            File.WriteAllBytes(Path.Combine(store1, P("k4")), k4Was);
            ModMerge.Split(docs, older1);
            // a record that can't be read (a power cut): its mods are told too; Remove with everything leaves nothing kept
            var lostRec = ModMerge.Run(ModMerge.Plan(docs, [M(P("k5")), M(P("k4"))], "Lost rec", "Packages", false), null, default).Merged;
            File.WriteAllText(Path.Combine(ModMerge.Folder(docs), "Lost rec", "merge.json"), "");
            ModMerge.Recover(docs);
            check(ModMerge.Orphans(docs).Count == 0, "merge: mods in a merge whose record can't be read are not told gone while its file is there");
            File.Delete(lostRec.Path);
            check(ModMerge.Orphans(docs).Single().Files.Count == 2 && ModMerge.PutBack(docs, ModMerge.Orphans(docs).Single()) == 2 && File.Exists(Path.Combine(mods, P("k5"))), "merge: mods kept without a readable record go back");
            var whole = ModMerge.Run(ModMerge.Plan(docs, [M(P("k5")), M(P("k4"))], "Whole", "Packages", false), null, default).Merged;
            ModMerge.RemoveAll(docs, whole);
            check(!File.Exists(whole.Path) && !File.Exists(Path.Combine(mods, P("k5"))) && !Directory.Exists(ModMerge.Folder(docs)) && Directory.GetFiles(recycled).Length >= 3, "merge: removed with its mods, all of it goes to the Recycle Bin");
            foreach (var f in Directory.GetFiles(Path.Combine(mods, "Packages"), "k?*.package")) File.Delete(f);
            foreach (var f in Directory.GetFiles(recycled)) File.Delete(f);
            before = Snap(docs);
            check(!ModMerge.Plan(docs, [M(A)], "x", "Packages", false).CanRun && !ModMerge.Plan(docs, all, "a", "Packages", false).CanRun
                && !ModMerge.Plan(docs, all, "x", @"..\..", false).CanRun && !ModMerge.Plan(docs, all, "x", "Elsewhere", false).CanRun,
                "merge: refused with fewer than 2 packages, a taken name, a folder outside Mods or one the game does not load");
            Games.IsRunning = () => true;
            var running = ModMerge.Plan(docs, all, "x", "Packages", false);
            Games.IsRunning = () => false;
            check(running.Refusals.Any(r => r.Key == Engine.CloseGameText), "merge: refused while the game runs");

            // cancel at each step before the move: no trace; a cancel during the move does not stop it
            foreach (var step in new[] { MergeStep.Check, MergeStep.Write, MergeStep.Verify })
            {
                using var cts = new CancellationTokenSource();
                var p = ModMerge.Plan(docs, all, "Merged One", "Packages", false);
                var canceled = false;
                try { ModMerge.Run(p, new Sync(v => { if (v.Step == step) cts.Cancel(); }), cts.Token); }
                catch (OperationCanceledException) { canceled = true; }
                check(canceled && Same(before, Snap(docs)) && !Directory.Exists(ModMerge.Folder(docs)) && File.Exists(p.LogPath), $"merge: a cancel in step {step} leaves no trace");
            }
            // a failure mid-write, before the move, mid-move and at the rename: everything as it was
            foreach (var at in new[] { "write", "verify", "move", "rename" })
            {
                ModMerge.FailPoint = x => { if (x == at) throw new IOException("injected"); };
                var failed = false;
                try { ModMerge.Run(ModMerge.Plan(docs, all, "Merged One", "Packages", false), null, default); }
                catch (IOException) { failed = true; }
                ModMerge.FailPoint = _ => { };
                check(failed && Same(before, Snap(docs)) && !Directory.Exists(ModMerge.Folder(docs)), $"merge: a failure at \"{at}\" restores everything " + Diff(before, Snap(docs), docs));
            }

            // run: stored bytes equal, compressed stays compressed, originals kept with their notes, the merged file tagged
            ModNotes.SetNotes(Path.Combine("Mods", A), "hello");
            ModNotes.CreateTag("Fav", "#E8716B");
            ModNotes.SetTag(Path.Combine("Mods", B), "Fav", true);
            var srcA = ModConflicts.Package.Open(Path.Combine(mods, A));
            var srcB = ModConflicts.Package.Open(Path.Combine(mods, B));
            var want = srcB.Entries.Select(e => Stored(Path.Combine(mods, B), e)).Concat(srcA.Entries.Where(e => e.Type != 3).Select(e => Stored(Path.Combine(mods, A), e))).ToList();
            using (var cts = new CancellationTokenSource())
            {
                var steps = new HashSet<MergeStep>();
                var res = ModMerge.Run(ModMerge.Plan(docs, all, "Merged One", "Packages", false), new Sync(v => { steps.Add(v.Step); if (v.Step == MergeStep.Move) cts.Cancel(); }), cts.Token);
                var outPkg = ModConflicts.Package.Open(res.Merged.Path);
                var got = outPkg.Entries.Select(e => Stored(res.Merged.Path, e)).ToList();
                var comp = outPkg.Entries.Single(e => e.Type == 2);
                check(got.Count == 4 && got.Zip(want).All(x => x.First.AsSpan().SequenceEqual(x.Second)) && comp.Compressed == 0xFFFF && comp.MemSize == plain.Length
                    && outPkg.Read(comp).AsSpan().SequenceEqual(plain) && Encoding.UTF8.GetString(outPkg.Read(outPkg.Entries.Single(e => e.Type == 3))) == "share"
                    && res.Sha256 == Sha(res.Merged.Path).ToLowerInvariant() && steps.SetEquals(Enum.GetValues<MergeStep>()),
                    "merge: every resource's stored bytes read back equal, compressed and uncompressed, the winner's copy of the shared key, all five steps reported (a cancel in the move ignored)");
                var store = Path.Combine(ModMerge.Folder(docs), "Merged One");
                var listed = Mods.List(new Game(root, root, "TS3.exe", Edition.EA, "1.69.47"), docs, kinds: false);
                check(File.Exists(Path.Combine(store, A)) && File.Exists(Path.Combine(store, B)) && File.Exists(Path.Combine(store, "merge.json")) && !File.Exists(Path.Combine(mods, A))
                    && !listed.Any(m => m.Path.StartsWith(ModMerge.Folder(docs))) && listed.Any(m => m.Rel == res.Merged.Rel) && !ModLibrary.Folders(docs).Any(f => f.StartsWith(ModMerge.FolderName)),
                    "merge: originals kept in Merged (Apex Launcher) next to Mods, at their places, out of the mod list and the folder list");
                check(ModMerge.IsMerged(res.Merged) && ModMerge.Info(res.Merged)!.Originals.Count == 2 && ModNotes.Get(Path.Combine("Mods", A)) is null
                    && ModNotes.TagsOf(res.Merged.Rel).SequenceEqual(["Fav"]) && File.ReadAllLines(res.LogPath).Any(l => l.StartsWith("key  00000003")),
                    "merge: made of 2 mods; the originals' notes kept in merge.json, their tags on the merged file; the log names the shared key");

                // split: refused while a different file sits at an original's place, then everything back by hash
                File.WriteAllText(Path.Combine(mods, A), "other");
                var refused = false;
                try { ModMerge.Split(docs, res.Merged); } catch (SetupException e) { refused = e.Message.Contains(A); }
                check(refused && File.Exists(res.Merged.Path) && File.Exists(Path.Combine(store, A)) && File.Exists(Path.Combine(store, B)) && !File.Exists(Path.Combine(mods, B)),
                    "split: refused, naming the file, when a different file sits where an original goes; nothing moved");
                File.Delete(Path.Combine(mods, A));
                ModMerge.Split(docs, res.Merged);
                ModNotes.Reload();
                check(Same(before, Snap(docs)) && !File.Exists(res.Merged.Path) && Directory.GetFiles(recycled).Length == 1 && !Directory.Exists(ModMerge.Folder(docs))
                    && ModNotes.Get(Path.Combine("Mods", A))?.Notes == "hello" && ModNotes.TagsOf(Path.Combine("Mods", B)).SequenceEqual(["Fav"]) && ModNotes.Get(res.Merged.Rel) is null,
                    "split: the originals back identical by hash, the merged file in the Recycle Bin, notes and tags back");
            }

            // a merge of a merged file, then a split of each restores all; a changed original in the store refuses the split
            Pkg(Path.Combine(mods, @"Packages\c.package"), Raw(9, 9, "C9"));
            var before2 = Snap(docs);
            var m1 = ModMerge.Run(ModMerge.Plan(docs, [M(A), M(B)], "First", "Packages", false), null, default).Merged;
            var m2 = ModMerge.Run(ModMerge.Plan(docs, [m1, M(@"Packages\c.package")], "Second", "Packages", false), null, default).Merged;
            var store2 = Path.Combine(ModMerge.Folder(docs), "Second");
            var cPath = Path.Combine(store2, @"Packages\c.package");
            var cBytes = File.ReadAllBytes(cPath);
            File.WriteAllText(cPath, "tampered");
            var refused2 = false;
            try { ModMerge.Split(docs, m2); } catch (SetupException e) { refused2 = e.Message.Contains("missing or changed"); }
            File.WriteAllBytes(cPath, cBytes);
            check(refused2 && ModConflicts.Package.Open(m2.Path).Entries.Count == 5 && ModMerge.Info(m2)!.Originals.Count == 2, "merge: a merged file merges again; a changed original refuses the split");
            ModMerge.Split(docs, m2);
            var renamed = ModLibrary.Rename(m1, "First again");
            check(ModMerge.IsMerged(renamed) && File.Exists(renamed.Path), "split: the inner merge comes back, still a merge after a rename");
            ModMerge.Split(docs, renamed);
            check(Same(before2, Snap(docs)) && !Directory.Exists(ModMerge.Folder(docs)), "split: both splits restore every original identical by hash " + Diff(before2, Snap(docs), docs));

            // one mod taken out: it is back identical, the other two are merged again under the same name; taking one of
            // two out leaves both plain mods
            Pkg(Path.Combine(mods, @"Packages\d.package"), Raw(10, 10, "D10"));
            var before3 = Snap(docs);
            var three = ModMerge.Run(ModMerge.Plan(docs, [M(A), M(B), M(@"Packages\d.package")], "Three", "Packages", false), null, default).Merged;
            var outA = ModMerge.TakeOut(docs, three, [Path.Combine("Mods", A)]);
            var aBack = File.Exists(Path.Combine(mods, A)) && Sha(Path.Combine(mods, A)) == before3[Path.Combine("Mods", A)];
            check(aBack && outA.Remerged is { Mods: 2 } rm && rm.Merged.Name == "Three.package" && ModMerge.Info(rm.Merged)!.Originals.Count == 2 && !File.Exists(Path.Combine(mods, B)),
                "take out: the mod comes back identical, the rest is merged again under the same name");
            var outB = ModMerge.TakeOut(docs, outA.Remerged!.Merged, [Path.Combine("Mods", B)]);
            check(outB.Remerged is null && Same(before3, Snap(docs)) && !Directory.Exists(ModMerge.Folder(docs)), "take out: one of two out leaves both plain, every file identical");

            // a mod added to a merge: it goes in beside the others, and splitting brings every file back identical
            var two = ModMerge.Run(ModMerge.Plan(docs, [M(A), M(B)], "Two", "Packages", false), null, default).Merged;
            var added = ModMerge.Edit(docs, two, [], [M(@"Packages\d.package")]);
            check(added.Remerged is { Mods: 3 } am && am.Merged.Name == "Two.package" && ModMerge.Info(am.Merged)!.Originals.Count == 3 && !File.Exists(Path.Combine(mods, @"Packages\d.package")),
                "edit: an added mod goes into the merge under the same name");
            ModMerge.Split(docs, added.Remerged!.Merged);
            check(Same(before3, Snap(docs)) && !Directory.Exists(ModMerge.Folder(docs)), "edit: splitting after an add restores every file identical " + Diff(before3, Snap(docs), docs));

            // a split that stopped halfway (the launcher closed with one original back): the next split finishes it
            var half = ModMerge.Run(ModMerge.Plan(docs, [M(A), M(B)], "Half", "Packages", false), null, default).Merged;
            File.Move(Path.Combine(ModMerge.Folder(docs), "Half", A), Path.Combine(mods, A));
            ModMerge.Split(docs, half);
            check(Same(before3, Snap(docs)) && !Directory.Exists(ModMerge.Folder(docs)), "split: one that stopped halfway is finished by the next, every file identical " + Diff(before3, Snap(docs), docs));

            // a merge that is off: taking a mod out is refused before anything moves (its originals would come back off,
            // and a merge leaves out mods that are off)
            var offMerge = ModMerge.Run(ModMerge.Plan(docs, [M(A), M(B)], "Off", "Packages", false), null, default).Merged;
            var offMerged = offMerge with { Path = Mods.SetEnabled(offMerge, false, docs), Enabled = false };
            var offRefused = false;
            try { ModMerge.TakeOut(docs, offMerged, [Path.Combine("Mods", A)]); } catch (SetupException) { offRefused = true; }
            check(offRefused && File.Exists(offMerged.Path) && ModMerge.Info(offMerged)?.Originals.Count == 2 && !File.Exists(Path.Combine(mods, A))
                && !File.Exists(Path.Combine(Mods.Disabled(docs), A)), "take out: refused on a merge that is off, nothing moved");
            foreach (var m in ModMerge.Split(docs, offMerged)) Mods.SetEnabled(m, true, docs);
            check(Same(before3, Snap(docs)), "take out: the merge that is off still splits, every file identical");

            // a merge stopped during the move (the launcher ended with A kept aside, B not yet, the merged file still under its
            // temporary name): the next read puts it right; a merged file the player removed keeps its originals kept
            var stopped = ModMerge.Run(ModMerge.Plan(docs, [M(A), M(B)], "Stopped", "Packages", false), null, default).Merged;
            File.Move(stopped.Path, stopped.Path + ".tmp-" + Guid.NewGuid().ToString("N"));
            File.Move(Path.Combine(ModMerge.Folder(docs), "Stopped", B), Path.Combine(mods, B));
            ModMerge.Recover(docs);
            check(Same(before3, Snap(docs)) && !Directory.Exists(ModMerge.Folder(docs)), "merge: one stopped during the move is undone at the next read, every file identical " + Diff(before3, Snap(docs), docs));
            var removed = ModMerge.Run(ModMerge.Plan(docs, [M(A), M(B)], "Removed", "Packages", false), null, default).Merged;
            File.Delete(removed.Path);
            ModMerge.Recover(docs);
            check(File.Exists(Path.Combine(ModMerge.Folder(docs), "Removed", A)) && !File.Exists(Path.Combine(mods, A)), "merge: a merged file the player removed keeps its originals kept");
            // a merge an older launcher kept inside Mods: next to Mods at the next read, and it still splits
            File.Move(Path.Combine(ModMerge.Folder(docs), "Removed", A), Path.Combine(mods, A));
            File.Move(Path.Combine(ModMerge.Folder(docs), "Removed", B), Path.Combine(mods, B));
            Directory.Delete(ModMerge.Folder(docs), true);
            ModMerge.Recover(docs);
            var older = ModMerge.Run(ModMerge.Plan(docs, [M(A), M(B)], "Older", "Packages", false), null, default).Merged;
            var inMods = Path.Combine(docs, "Mods", ModMerge.FolderName);
            Directory.Move(ModMerge.Folder(docs), inMods);
            ModMerge.Recover(docs);
            check(!Directory.Exists(inMods) && File.Exists(Path.Combine(ModMerge.Folder(docs), "Older", A)) && ModMerge.IsMerged(older), "merge: originals an older launcher kept inside Mods move next to it");
            ModMerge.Split(docs, older);
            check(Same(before3, Snap(docs)) && !Directory.Exists(ModMerge.Folder(docs)), "merge: one moved out of Mods still splits, every file identical " + Diff(before3, Snap(docs), docs));
            // a write cut short before its record: an old .package.tmp-* no record names goes, one written now stays
            var orphan = Path.Combine(mods, "Lost.package.tmp-" + Guid.NewGuid().ToString("N"));
            var fresh = Path.Combine(mods, "Busy.package.tmp-" + Guid.NewGuid().ToString("N"));
            File.WriteAllText(orphan, "half a merge"); File.SetLastWriteTime(orphan, DateTime.Now.AddDays(-1));
            File.WriteAllText(fresh, "a merge being written");
            ModMerge.Recover(docs);
            check(!File.Exists(orphan) && File.Exists(fresh), "merge: an orphan temporary file from an earlier run goes, one being written stays");
            File.Delete(fresh);
        }
        catch (Exception e) { check(false, "merge: exception " + e); }
        finally
        {
            ModMerge.FailPoint = _ => { };
            ModMerge.Recycle = oldRecycle;
            Games.IsRunning = () => false;
            Paths.LocalAppData = oldLocal;
            ModNotes.Reload();
            try { Directory.Delete(root, true); } catch (Exception) { }
        }
    }
}
