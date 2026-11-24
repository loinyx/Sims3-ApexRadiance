// Offline checks of merging packages (ApexSetup.Core/ModMerge.cs) on packages made here, in a temporary Documents folder.
using System.Buffers.Binary;
using System.Security.Cryptography;
using System.Text;
using ApexSetup;

static class ModMergeTests
{
    const uint S3sa = 0x073FAA07;

    // a DBPF 2.0 package: each resource with its type, instance, stored bytes, compression flag and unpacked size
    internal static void Pkg(string path, params (uint Type, ulong Inst, byte[] Data, bool Comp, int Mem)[] res)
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
    internal static (uint, ulong, byte[], bool, int) Raw(uint t, ulong i, string s) => (t, i, Encoding.UTF8.GetBytes(s), false, Encoding.UTF8.GetByteCount(s));

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
    // (an antivirus drops and removes its own NAME.PACKAGE.tmp while scanning: not counted)
    static Dictionary<string, string> Snap(string dir)
    {
        var o = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        foreach (var f in Directory.EnumerateFiles(dir, "*", SearchOption.AllDirectories))
            if (!f.EndsWith(".tmp", StringComparison.OrdinalIgnoreCase)) o[Path.GetRelativePath(dir, f)] = Sha(f);
        return o;
    }
    static void Wipe(string dir) => Fs.DeleteClear(dir);
    // what differs, for a failed check's message
    static string Diff(Dictionary<string, string> a, Dictionary<string, string> b) =>
        string.Join("; ", a.Keys.Union(b.Keys).Where(k => !a.TryGetValue(k, out var x) || !b.TryGetValue(k, out var y) || x != y)
            .Select(k => (a.ContainsKey(k) ? "" : "+") + (b.ContainsKey(k) ? "" : "-") + k));
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

    static void CopyDir(string from, string to)
    {
        foreach (var f in Directory.EnumerateFiles(from, "*", SearchOption.AllDirectories))
        {
            var t = Path.Combine(to, Path.GetRelativePath(from, f));
            Directory.CreateDirectory(Path.GetDirectoryName(t)!);
            File.Copy(f, t);
        }
    }

    // One order for a merge, a change to a merge made in one go (and undone whole when it fails), merges in parts, script
    // mods set aside, newer versions of the mods inside, and a mod inside a merge read as a mod. In Packages\E, on its own files.
    static void Edits(Action<bool, string> check, string root, string docs, string recycled)
    {
        var mods = Path.Combine(docs, "Mods");
        const string Folder = @"Packages\E";
        string P(string n) => $@"{Folder}\{n}.package";
        string Rel(string n) => Path.Combine("Mods", P(n));
        ModFile M(string n, bool on = true) => new(Path.Combine(on ? mods : Mods.Disabled(docs), P(n)), Rel(n), 0, ModKind.Package, on, false);
        void Make(string n, params (uint, ulong, byte[], bool, int)[] res) => Pkg(Path.Combine(mods, P(n)), res);
        string Text(string path, uint type) => Encoding.UTF8.GetString(Stored(path, ModConflicts.Package.Open(path).Entries.Single(e => e.Type == type)));
        List<string> Names(MergeRecord r) => r.Originals.Select(o => Path.GetFileNameWithoutExtension(o.Rel)).ToList();
        void Empty() { foreach (var f in Directory.GetFiles(recycled)) File.Delete(f); }
        HashSet<string> Binned() => Directory.GetFiles(recycled).Select(Sha).ToHashSet();
        bool Throws<T>(Action a) where T : Exception { try { a(); return false; } catch (T) { return true; } }
        Empty();

        Make("e1", Raw(100, 100, "e1"), Raw(110, 110, "one"));
        Make("e2", Raw(101, 101, "e2"), Raw(110, 110, "two"));
        Make("e3", Raw(102, 102, "e3"));
        Make("e4", Raw(103, 103, "e4"));
        Make("e5", Raw(104, 104, "e5"));
        Make("e6", Raw(106, 106, "e6"));
        var downloads = Path.Combine(root, "Downloads");
        var v2 = Path.Combine(downloads, "e3 v2.package");
        Pkg(v2, Raw(102, 102, "e3 new"), Raw(105, 105, "more"));
        ModNotes.SetNotes(Rel("e1"), "n1");
        var start = Snap(docs);

        // ---- one order ----
        var four = new[] { "e1", "e2", "e3", "e4" }.Select(n => M(n)).ToList();
        var undecided = ModMerge.Plan(docs, four, "Ord", Folder, false);
        check(undecided.Inputs.Select(m => m.Name).SequenceEqual(["e3.package", "e4.package"]) && undecided.Skipped.Count == 2 && undecided.Skipped.All(s => s.Reason.Key == ModMerge.ClashReason) && undecided.Shares.Count == 0,
            "order: mods that clash and the player has not decided on stay out");
        var leftOut = ModMerge.Plan(docs, four, "Ord", Folder, false, [Rel("e1"), Rel("e2")], [Rel("e2"), Rel("e4")]);
        check(leftOut.Inputs.Select(m => m.Name).SequenceEqual(["e1.package", "e3.package"]) && leftOut.Clashes.Count == 1 && leftOut.Skipped.Count == 2 && leftOut.Skipped.All(s => s.Reason.Key == ModMerge.LeftOutReason),
            "order: a mod the player leaves out stays out and says so, the clash it is part of still shown");
        var ordered = ModMerge.Plan(docs, four, "Ord", Folder, false, [Rel("e2"), Rel("e1")]);
        check(ordered.Inputs.Select(m => m.Name).SequenceEqual(["e2.package", "e1.package", "e3.package", "e4.package"]) && ordered.Skipped.Count == 0
            && ordered.Shares[Rel("e2")] == new MergeShare(1, 0) && ordered.Shares[Rel("e1")] == new MergeShare(0, 1) && ordered.Shares.Count == 2,
            "order: the mods that clash follow the player's order, the rest the game's; each says how many of its resources count and how many are replaced");
        var m = ModMerge.Run(ordered, null, default).Merged;
        check(Text(m.Path, 110) == "two" && Names(ModMerge.Info(m)!).SequenceEqual(["e2", "e1", "e3", "e4"]), "order: the first one's copy counts and the record keeps the order");
        m = ModMerge.Edit(docs, m, [], [M("e5")]).Remerged!.Merged;
        check(Text(m.Path, 110) == "two" && Names(ModMerge.Info(m)!).SequenceEqual(["e2", "e1", "e3", "e4", "e5"]), "order: kept through an edit, an added mod last");
        m = ModMerge.Edit(docs, m, new MergeEdit { Order = [Rel("e1"), Rel("e2")] }).Remerged!.Merged;
        check(Text(m.Path, 110) == "one" && Names(ModMerge.Info(m)!).SequenceEqual(["e1", "e2", "e3", "e4", "e5"]) && Binned().Count == 2, "edit: a new order alone changes whose copy counts, the old merged file to the Recycle Bin each time");
        Empty();

        // ---- a mod inside a merge ----
        var rec = ModMerge.Info(m)!;
        var inside = ModMerge.Inside(rec, rec.Originals[2]);
        check(inside.Rel == Rel("e3") && inside.Locked && File.Exists(inside.Path) && ModMerge.Keeps(docs, inside.Path) && Dbpf.Index(inside.Path).Count == 1 && Throws<SetupException>(() => ModLibrary.Remove(inside)),
            "inside: a mod of a merge reads as a mod at its kept copy, and the library refuses to touch it");
        ModMerge.EditNote(docs, m, Rel("e3"), n => { n.Notes = " about e3 "; n.Creator = "Ana"; n.Tags = ["Fav"]; });
        check(Throws<SetupException>(() => ModMerge.EditNote(docs, m, Rel("e3"), n => n.Creator = new string('x', 61))) && Throws<SetupException>(() => ModMerge.EditNote(docs, m, Rel("nope"), _ => { }))
            && ModMerge.Info(m)!.Originals[2].Note is { Notes: "about e3", Creator: "Ana" } && File.ReadAllText(Path.Combine(rec.Dir, "merge.json")).Contains("about e3") && ModMerge.Info(m)!.Originals[0].Note?.Notes == "n1",
            "inside: notes, creator and tags of a mod inside a merge are kept in the merge's record; a bad value changes nothing");

        // ---- a change in one go: out (off, Recycle Bin), in, a file replaced from outside Mods, a new order, a new name ----
        ModNotes.SetNotes(m.Rel, "merged note");
        ModNotes.SetTag(m.Rel, "Fav", true);
        var pic = Path.Combine(root, "pic.png");
        File.WriteAllBytes(pic, [1, 2, 3]);
        ModNotes.SetPicture(m.Rel, pic);
        var store = rec.Dir;
        string Kept(string n) => Path.Combine(store, P(n));
        var (h1, h2, h3, hOld, hV2) = (Sha(Kept("e1")), Sha(Kept("e2")), Sha(Kept("e3")), Sha(m.Path), Sha(v2));
        MergeEdit Staged()
        {
            var e = new MergeEdit { Name = "Edited", Order = [Rel("e4"), Rel("e3"), Rel("e6")] };
            e.Out[Rel("e1")] = TakeOutTo.Off;
            e.Out[Rel("e2")] = TakeOutTo.Bin;
            e.Replace[Rel("e3")] = ModMerge.Outside(v2);
            e.Add.Add(M("e6"));
            return e;
        }
        var before = Snap(docs);
        var planned = ModMerge.PlanEdit(docs, m, Staged());
        check(planned.CanRun && planned.Plan!.Inputs.Select(x => x.Name).SequenceEqual(["e4.package", "e3 v2.package", "e6.package", "e5.package"]) && planned.Plan.TargetRel == Rel("Edited") && Same(before, Snap(docs)),
            "edit: the plan of a change says what the merge becomes and moves nothing " + planned.Why);
        var crash = Path.Combine(root, "Crash");
        foreach (var (at, nth) in new[] { ("write", 1), ("verify", 1), ("commit", 1) }.Concat(Enumerable.Range(1, 7).Select(i => ("edit", i))))
        {
            var seen = 0;
            ModMerge.FailPoint = x =>
            {
                if (x != at || ++seen != nth) return;
                if (at == "edit" && nth == 5) CopyDir(docs, crash); // (as a launcher that ends here leaves it)
                throw new IOException("injected");
            };
            var failed = Throws<IOException>(() => ModMerge.Edit(docs, m, Staged()));
            ModMerge.FailPoint = _ => { };
            check(failed && Same(before, Snap(docs)) && Directory.GetFiles(recycled).Length == 0 && Sha(v2) == hV2 && ModMerge.Info(m) is { Pending: null, Originals.Count: 5 },
                $"edit: a failure at \"{at}\" {nth} leaves the merge exactly as it was " + Diff(before, Snap(docs)));
        }
        using (var cts = new CancellationTokenSource())
        {
            var canceled = Throws<OperationCanceledException>(() => ModMerge.Edit(docs, m, Staged(), new Sync(v => { if (v.Step == MergeStep.Write) cts.Cancel(); }), cts.Token));
            check(canceled && Same(before, Snap(docs)), "edit: a cancel while the new file is written leaves no trace");
        }
        // the launcher ended in the middle of the moves: the next start puts the merge back as it was
        var crashed = Snap(crash);
        ModMerge.Recover(crash);
        check(!Same(before, crashed) && Same(before, Snap(crash)) && ModMerge.Info(crash, m) is { Pending: null }, "edit: a change cut short by the launcher ending is undone at the next start " + Diff(before, Snap(crash)));
        // a record that names files outside the game's folder (a shared folder, a damaged file): they are left alone
        var victim = Path.Combine(root, "victim.txt");
        File.WriteAllText(victim, "mine");
        var json = Path.Combine(ModMerge.Folder(crash), "Ord", "merge.json");
        File.WriteAllText(json, File.ReadAllText(json).Replace("\"Pending\": null", "\"Pending\": { \"Moves\": [ { \"From\": \"x\", \"To\": \"..\\\\victim.txt\", \"Copy\": true }, { \"From\": \"Mods\\\\gone.package\", \"To\": \"..\\\\victim.txt\" } ], \"Tmp\": \"..\\\\victim.txt\" }"));
        var tampered = Path.Combine(root, "Tampered");
        CopyDir(crash, tampered);
        ModMerge.Recover(tampered);
        check(File.ReadAllText(victim) == "mine" && !File.Exists(Path.Combine(tampered, "Mods", "gone.package")) && ModMerge.Info(tampered, m) is { Pending: null }, "edit: an unfinished change's record never moves or removes a file outside Mods and the merges' folder");

        var res = ModMerge.Edit(docs, m, Staged());
        var em = res.Remerged!.Merged;
        var info = ModMerge.Info(em)!;
        ModNotes.Reload();
        check(em.Name == "Edited.package" && em.Enabled && em.Path == Path.Combine(mods, P("Edited")) && !File.Exists(m.Path) && Names(info).SequenceEqual(["e4", "e3 v2", "e6", "e5"])
            && ModConflicts.Package.Open(em.Path).Entries.Count == 5 && Text(em.Path, 102) == "e3 new" && info.Sha256 == Sha(em.Path).ToLowerInvariant() && info.Pending is null,
            "edit: one change takes mods out, adds one, replaces a file, reorders and renames, written and verified once");
        check(res.Back.Single() is { Enabled: false } b1 && b1.Path == Path.Combine(Mods.Disabled(docs), P("e1")) && Sha(b1.Path) == h1 && Binned().SetEquals([h2, h3, hOld]) && Sha(v2) == hV2
            && !File.Exists(Path.Combine(mods, P("e6"))) && Directory.GetFiles(store, "*", SearchOption.AllDirectories).Length == 5 && Sha(Kept("e3 v2")) == hV2 && !Directory.GetFiles(Path.Combine(mods, Folder), "*.tmp-*").Any(),
            "edit: the mod taken out is back (off) identical, the one removed, the replaced file and the old merged file are in the Recycle Bin, the outside file is copied, not moved");
        check(ModNotes.Get(em.Rel)?.Notes == "merged note" && ModNotes.TagsOf(em.Rel).Contains("Fav") && ModNotes.PicturePath(em.Rel) is not null && ModNotes.Get(m.Rel) is null
            && info.Originals[1].Note is { Notes: "about e3", Creator: "Ana" } && ModNotes.Get(Rel("e1"))?.Notes == "n1",
            "edit: the merged file's notes, picture and tags follow its new name, a replaced mod's notes stay with the new file, a mod taken out gets its own back");
        Empty();

        // ---- refused before anything moves ----
        var held = Snap(docs);
        Make("Taken", Raw(120, 120, "t"));
        Make("e4", Raw(121, 121, "another e4"));
        var takenName = ModMerge.PlanEdit(docs, em, new MergeEdit { Name = "Taken" });
        var takenPlace = new MergeEdit();
        takenPlace.Out[Rel("e4")] = TakeOutTo.On;
        var addSelf = new MergeEdit();
        addSelf.Add.Add(em);
        var badFile = new MergeEdit();
        badFile.Replace[Rel("e5")] = ModMerge.Outside(pic);
        var changed = Path.Combine(downloads, "e5 v2.package");
        Pkg(changed, Raw(104, 104, "e5 new"));
        var late = new MergeEdit();
        late.Replace[Rel("e5")] = ModMerge.Outside(changed);
        var latePlan = ModMerge.PlanEdit(docs, em, late);
        Pkg(changed, Raw(104, 104, "e5 newer still"));
        var refusedAll = !takenName.CanRun && takenName.Plan!.Refusals.Any(r => r.Key.Contains("already exists")) && !ModMerge.PlanEdit(docs, em, takenPlace).CanRun && Throws<SetupException>(() => ModMerge.Edit(docs, em, takenPlace))
            && !ModMerge.PlanEdit(docs, em, addSelf).CanRun && !ModMerge.PlanEdit(docs, em, badFile).CanRun && latePlan.CanRun && Throws<SetupException>(() => ModMerge.Run(latePlan));
        File.Delete(Path.Combine(mods, P("Taken")));
        File.Delete(Path.Combine(mods, P("e4")));
        check(refusedAll && Same(held, Snap(docs)) && Directory.GetFiles(recycled).Length == 0,
            "edit: a name that is taken, a place that is taken, the merge into itself, a file that is no package and a file changed since the plan are refused, nothing moved " + Diff(held, Snap(docs)));

        // ---- a merge that is off ----
        var off = em with { Path = Mods.SetEnabled(em, false, docs), Enabled = false };
        var offOut = ModMerge.TakeOut(docs, off, [Rel("e4")]);
        var om = offOut.Remerged!.Merged;
        check(offOut.Back.Single() is { Enabled: false } b4 && b4.Path == Path.Combine(Mods.Disabled(docs), P("e4")) && !om.Enabled && om.Path == off.Path && Names(ModMerge.Info(om)!).SequenceEqual(["e3 v2", "e6", "e5"])
            && ModNotes.Get(om.Rel)?.Notes == "merged note" && !File.Exists(Path.Combine(mods, P("e4"))),
            "edit: a merge that is off is changed where it is and stays off, the mod taken out comes back off");
        // fewer than two left: every mod goes back (on, off or to the Recycle Bin as asked; the rest as the merge is) and the merge is gone
        Empty();
        var last = new MergeEdit();
        last.Out[Rel("e3 v2")] = TakeOutTo.On;
        last.Out[Rel("e6")] = TakeOutTo.Bin;
        var gone = ModMerge.Edit(docs, om, last);
        check(gone.Remerged is null && gone.Back.Count == 2 && File.Exists(Path.Combine(mods, P("e3 v2"))) && File.Exists(Path.Combine(Mods.Disabled(docs), P("e5"))) && !File.Exists(om.Path) && !Directory.Exists(store)
            && Directory.GetFiles(recycled).Length == 2 && ModNotes.Get(om.Rel) is null && ModMerge.Orphans(docs).Count == 0,
            "edit: with fewer than two mods left the merge is removed, every mod where it was asked to go");
        Empty();
        foreach (var n in new[] { "e1", "e4", "e5" }) Mods.SetEnabled(M(n, false), true, docs);
        Make("e2", Raw(101, 101, "e2"), Raw(110, 110, "two"));
        Make("e6", Raw(106, 106, "e6"));

        // ---- the Recycle Bin can't take a file: it comes back in the list, switched off ----
        var noBin = ModMerge.Run(ModMerge.Plan(docs, [M("e4"), M("e5"), M("e6")], "NoBin", Folder, false), null, default).Merged;
        var bin = ModMerge.Recycle;
        ModMerge.Recycle = p => throw new SetupException("Windows couldn't move the file to the Recycle Bin.");
        var binOut = new MergeEdit();
        binOut.Out[Rel("e4")] = TakeOutTo.Bin;
        var kept4 = ModMerge.Edit(docs, noBin, binOut);
        ModMerge.Recycle = bin;
        check(kept4.Back.Single() is { Enabled: false } b5 && b5.Path == Path.Combine(Mods.Disabled(docs), P("e4")) && File.Exists(b5.Path) && kept4.Remerged is { Mods: 2 } && !Directory.GetDirectories(ModMerge.Info(kept4.Remerged.Merged)!.Dir, ".out-*").Any(),
            "edit: a mod the Recycle Bin can't take comes back switched off instead of staying out of sight");
        // (the old merged file the Recycle Bin did not take stays beside the new one, not loaded, until a later start of the launcher)
        var stale = Directory.GetFiles(Path.Combine(mods, Folder), "NoBin.package.tmp-*");
        foreach (var f in stale) File.SetLastWriteTime(f, DateTime.Now.AddDays(-1));
        ModMerge.Recover(docs);
        check(stale.Length == 1 && !File.Exists(stale[0]) && File.Exists(kept4.Remerged!.Merged.Path), "edit: an old merged file the Recycle Bin could not take is cleared at a later start");
        ModMerge.Split(docs, kept4.Remerged!.Merged);
        Mods.SetEnabled(M("e4", false), true, docs);
        Empty();
        File.Delete(Path.Combine(mods, P("e3 v2")));
        Make("e3", Raw(102, 102, "e3"));
        check(Same(start, Snap(docs)) && !Directory.Exists(ModMerge.Folder(docs)), "edit: after all of it every mod is back identical " + Diff(start, Snap(docs)));

        // ---- in parts ----
        var big = new string('x', 200);
        Make("p1", Raw(201, 201, big));
        Make("p2", Raw(202, 202, big), Raw(210, 210, "p2 wins"));
        Make("p3", Raw(203, 203, big));
        Make("p4", Raw(204, 204, big), Raw(210, 210, "p4 wins"));
        Make("p5", Raw(205, 205, big), Raw(201, 201, big));
        var five = new[] { "p1", "p2", "p3", "p4", "p5" }.Select(n => M(n)).ToList();
        var whole = ModMerge.PlanParts(docs, five, "Big", Folder, false, [Rel("p4"), Rel("p2")]);
        var parts = ModMerge.PlanParts(docs, five, "Big", Folder, false, [Rel("p4"), Rel("p2")], null, 700);
        check(whole is [{ Name: "Big", Inputs.Count: 5, CanRun: true }] && parts.Count == 2 && parts.All(p => p.CanRun) && parts[0].Name == "Big 1" && parts[1].Name == "Big 2"
            && parts[0].Inputs.Select(x => x.Name).SequenceEqual(["p4.package", "p2.package"]) && parts[1].Inputs.Select(x => x.Name).SequenceEqual(["p1.package", "p3.package"])
            && parts[0].Skipped.Single() is { Mod.Name: "p5.package" } alone && alone.Reason.Key.StartsWith("Too big") && parts[0].Clashes.Count == 1 && parts[1].Skipped.Count == 0,
            "parts: a merge too big for one package is planned as several, mods that clash together in their order, a mod left alone stays as it is");
        var beforeParts = Snap(docs);
        var made = parts.Select(p => ModMerge.Run(p, null, default).Merged).ToList();
        check(Text(made[0].Path, 210) == "p4 wins" && made.All(x => ModMerge.Info(x)!.Originals.Count == 2) && File.Exists(Path.Combine(mods, P("p5"))) && ModMerge.Orphans(docs).Count == 0 && ModMerge.Lost(docs).Count == 0,
            "parts: each part is a merge of its own, with its record");
        var partOut = ModMerge.TakeOut(docs, made[1], [Rel("p1")]);
        foreach (var x in made.Take(1)) ModMerge.Split(docs, x);
        check(partOut.Remerged is null && Same(beforeParts, Snap(docs)) && !Directory.Exists(ModMerge.Folder(docs)), "parts: each part splits and is edited on its own, every file back identical " + Diff(beforeParts, Snap(docs)));
        Empty();

        // ---- script mods ----
        var (asm1, asm2) = (ModConflicts.S3saWrap(new byte[600]), ModConflicts.S3saWrap(new byte[700]));
        Make("s1", (S3sa, 301, asm1, false, asm1.Length), Raw(220, 220, "s1 tuning"));
        Make("s2", (S3sa, 302, asm2, false, asm2.Length));
        var mix = new[] { "p1", "p3", "s1", "s2" }.Select(n => M(n)).ToList();
        var noScripts = ModMerge.Plan(docs, mix, "Mix", Folder, false);
        var scripts = ModMerge.Plan(docs, noScripts.Scripts.ToList(), "Mix scripts", Folder, true);
        check(noScripts.CanRun && noScripts.Inputs.Count == 2 && noScripts.Scripts.Select(x => x.Name).SequenceEqual(["s1.package", "s2.package"]) && scripts.CanRun && scripts.Inputs.Count == 2,
            "scripts: a plan says which script mods it set aside, and they plan as a merge of their own");
        var beforeScripts = Snap(docs);
        var withScripts = ModMerge.Run(ModMerge.Plan(docs, mix, "Mix", Folder, true), null, default).Merged;
        check(ModConflicts.Package.Open(withScripts.Path).Entries.Count(e => e.Type == S3sa) == 2 && ModConflicts.Package.Open(withScripts.Path).Entries.Count == 5 && withScripts.Kind == ModKind.Script,
            "scripts: script mods and other mods merge into one package when asked");
        ModMerge.Split(docs, withScripts);
        var (plain, own) = (ModMerge.Run(noScripts, null, default).Merged, ModMerge.Run(scripts, null, default).Merged);
        check(own.Name == "Mix scripts.package" && ModMerge.Info(own)!.Originals.Count == 2 && ModMerge.Info(plain)!.Originals.Count == 2, "scripts: the script mods merge as a second package beside the first");
        ModMerge.Split(docs, plain);
        ModMerge.Split(docs, own);
        check(Same(beforeScripts, Snap(docs)), "scripts: both split, every file back identical");

        // ---- the choices of the merge dialog: what the rows say is what is planned ----
        Make("s3", (S3sa, 303, asm1, false, asm1.Length), Raw(220, 220, "s3 tuning"));
        var chosen = new[] { "p1", "p2", "p3", "p4", "s1", "s2", "s3" }.Select(n => M(n)).ToList();
        var ch = new MergeChoices();
        var apart = true;
        List<MergePlan> now = [];
        MergePlan? own2 = null;
        bool Plan(long part = long.MaxValue)
        {
            var (o, l) = (ch.Order, ch.LeaveOut);
            now = ModMerge.PlanParts(docs, chosen, "Dlg", Folder, false, o, l, part);
            own2 = apart ? ModMerge.Plan(docs, now[0].Scripts.ToList(), "Dlg scripts", Folder, true, o, l) : null;
            return ch.Take(now, own2, o, l);
        }
        MergeChoice Row(string n) => ch.Rows.Single(r => r.Mod.Rel == Rel(n));
        List<string> In() => now.Concat(own2 is null ? [] : [own2]).SelectMany(p => p.Inputs).Select(m => Path.GetFileNameWithoutExtension(m.Name)).ToList();
        check(Plan() && ch.Rows.Count == 7 && ch.Rows.Take(4).All(r => r.Undecided && !r.In) && Row("p2").Meets is { Resources: 1, Outside.Count: 0 } pm && pm.With.Single() is { Value: 1 } pw && pw.Key == Rel("p4")
            && Row("p1").Meets is null && Row("p1").In && Row("s1").Scripts && !Row("p2").Scripts && Row("s1").Meets!.With.ContainsKey(Rel("s3")) && In().SequenceEqual(["p1", "p3", "s2"]),
            "dialog: the mods that conflict come first, say with whom and in how many resources, and stay out until decided on");
        // a conflict not known when the plan was asked for: not silently in
        Row("p1").Pick = true;
        check(!ch.Take(now, own2, [Rel("p3"), Rel("p1"), Rel("s2")], []), "dialog: a plan made for another order is not taken as the one on screen");
        Row("p1").Pick = null;
        Row("p2").Pick = Row("p4").Pick = Row("s1").Pick = Row("s3").Pick = true;
        ch.Rows.Move(ch.Rows.IndexOf(Row("p4")), 0);
        check(Plan() && In().SequenceEqual(["p4", "p2", "p1", "p3", "s1", "s3", "s2"]) && Row("p4").Share == new MergeShare(1, 0) && Row("p2").Share == new MergeShare(0, 1),
            "dialog: the mods put in follow the rows, each saying whose copy counts");
        check(Plan(400) && now.Count == 1 && In().Take(2).SequenceEqual(["p4", "p2"]) && ch.Rows.Count == 7 && Row("p1").Why!.Key.StartsWith("Too big") && Row("p4").Why is null, "dialog: a mod too big for its part keeps its row and says why");
        Row("p2").Pick = false;
        apart = false;
        Plan();
        check(Plan() && ch.Rows.Count == 4 && In().SequenceEqual(["p4", "p1", "p3"]) && ch.Others(now, own2).Count == 3 && ch.Rows.Any(r => r.Mod.Rel == Rel("p2") && !r.In),
            "dialog: a mod left out keeps its row, script mods left out have none and say why");
        apart = true;
        Plan();
        check(Plan() && ch.Rows.Count == 7 && Row("s1").Pick == true && In().Count == 6 && ch.Others(now, own2).Count == 0, "dialog: script mods that come back keep their choice");
        File.Delete(Path.Combine(mods, P("s3")));
        var put = Path.Combine(mods, P("p1"));
        var (stamp, key1) = (File.GetLastWriteTimeUtc(put), ModViewPrefs.LaterKey(M("p1")));
        File.SetLastWriteTimeUtc(put, stamp.AddHours(1));
        var key2 = ModViewPrefs.LaterKey(M("p1"));
        File.SetLastWriteTimeUtc(put, stamp);
        check(key1 != key2 && key1 == ModViewPrefs.LaterKey(M("p1")), "dialog: an update put off is offered again once its file changes");
        Empty();

        // ---- newer versions of the mods inside a merge ----
        var gameRoot = Path.Combine(root, "GameX");
        var game = new Game(gameRoot, Path.Combine(gameRoot, "Game", "Bin"), "TS3W.exe", Edition.Steam, "1.67.2");
        Directory.CreateDirectory(game.Bin);
        Pkg(Path.Combine(gameRoot, "GameData", "Shared", "Packages", "FullBuild0.package"), Raw(90, 90, "game"), Raw(91, 91, "game"), Raw(92, 92, "game"));
        var gi = DefaultReplacements.Index(game);
        Make("Hair v2", Raw(50, 50, "h-old"), Raw(51, 51, "h2"));
        Make("Chair 1", Raw(60, 60, "c1"));
        Make("Sofa", Raw(80, 80, "s"), Raw(81, 81, "s"), Raw(82, 82, "s"), Raw(83, 83, "s"), Raw(90, 90, "s"));
        Make("SkinA", Raw(90, 90, "a"), Raw(91, 91, "a"), Raw(92, 92, "a"));
        var upd = ModMerge.Run(ModMerge.Plan(docs, [M("Hair v2"), M("Chair 1"), M("Sofa"), M("SkinA")], "Upd", Folder, false, [Rel("Sofa"), Rel("SkinA")]), null, default).Merged;
        var updStore = ModMerge.Info(upd)!.Dir;
        Make("hair_V3", Raw(50, 50, "h-new"), Raw(51, 51, "h2"), Raw(52, 52, "extra"));
        Make("Hair v4", Raw(50, 50, "h-4"), Raw(51, 51, "h2"));
        Make("Hair v1", Raw(50, 50, "h-older"), Raw(51, 51, "h2"));
        File.Copy(Path.Combine(updStore, P("Hair v2")), Path.Combine(mods, P("Copy of hair")));
        Make("Chair 2", Raw(60, 60, "c2"));
        Pkg(Path.Combine(mods, @"Packages\hair V2.package"), Raw(70, 70, "another mod"));
        Make("Couch remake", Raw(80, 80, "s2"), Raw(81, 81, "s"), Raw(82, 82, "s"), Raw(83, 83, "s"));
        Make("SkinB", Raw(90, 90, "b"), Raw(91, 91, "b"), Raw(92, 92, "b"));
        File.SetLastWriteTimeUtc(Path.Combine(mods, P("Couch remake")), DateTime.UtcNow.AddHours(1));
        File.SetLastWriteTimeUtc(Path.Combine(mods, P("SkinB")), DateTime.UtcNow.AddHours(1));
        var loose = new[] { "hair_V3", "Hair v4", "Hair v1", "Copy of hair", "Chair 2", "Couch remake", "SkinB" }.Select(n => M(n))
            .Append(new ModFile(Path.Combine(mods, @"Packages\hair V2.package"), @"Mods\Packages\hair V2.package", 0, ModKind.Package, true, false)).Append(upd).ToList();
        var byName = ModMerge.Updates(docs, loose);
        var withGame = ModMerge.Updates(docs, loose, gi);
        check(byName.Count == 2 && byName.Single(u => u.Same) is { Newer.Name: "Copy of hair.package" } && byName.Single(u => !u.Same) is { Newer.Name: "Hair v4.package" } hair && hair.Original.Rel == Rel("Hair v2") && hair.Merged.Path == upd.Path,
            "updates: a file named like a mod of a merge, with a higher version and resources in common, is its newer version (the newest of several); the same file again is said to be inside; an older version, a numbered sibling and a namesake with nothing in common are nothing "
            + string.Join(", ", byName.Select(u => u.Newer.Name)));
        check(withGame.Count == 3 && withGame.Single(u => u.Newer.Name == "Couch remake.package") is { Same: false } couch && couch.Original.Rel == Rel("Sofa") && !withGame.Any(u => u.Newer.Name == "SkinB.package"),
            "updates: with the game's index, a file that holds nearly all of a mod's own resources, changed and newer, is its newer version under any name; another replacement of the same game items is not");
        check(ModMerge.NameKeyOf("My_Mod v1.2b.package") == ModMerge.NameKeyOf("my mod (2).package") && ModMerge.NameKeyOf("Mod-1.69.package") == "mod" && ModMerge.NameKeyOf("Hair 1.package") != ModMerge.NameKeyOf("Hair 2.package") && ModMerge.NameKeyOf("Luv2.package") == "luv2",
            "updates: a name is compared without its version, case, spaces and underscores; a bare number is part of the name");
        var hV4 = Sha(Path.Combine(mods, P("Hair v4")));
        var hHair = Sha(Path.Combine(updStore, P("Hair v2")));
        var updated = ModMerge.Update(docs, withGame.Where(u => !u.Same).ToList()).Remerged!.Merged;
        check(Names(ModMerge.Info(updated)!).SequenceEqual(["Couch remake", "SkinA", "Chair 1", "Hair v4"]) && Text(updated.Path, 50) == "h-4" && !File.Exists(Path.Combine(mods, P("Hair v4"))) && Sha(Path.Combine(updStore, P("Hair v4"))) == hV4
            && Binned().Contains(hHair) && Directory.GetFiles(recycled).Length == 3 && File.Exists(Path.Combine(mods, P("hair_V3"))),
            "updates: the newer files take their older versions' places in the merge in one change, the old ones to the Recycle Bin " + string.Join(", ", Names(ModMerge.Info(updated)!)));
        var v5 = Path.Combine(downloads, "Hair v5.package");
        Pkg(v5, Raw(50, 50, "h-5"), Raw(51, 51, "h2"));
        check(ModMerge.Updates(docs, [ModMerge.Outside(v5)]) is [{ Same: false, Newer.Path: var np }] && np == v5 && ModMerge.Updates(docs, [ModMerge.Outside(Path.Combine(updStore, P("Hair v4")))]).Count == 0,
            "updates: a file from outside Mods is checked the same way; a merge's own kept copy is no candidate");
        check(Throws<SetupException>(() => ModMerge.Update(docs, byName.Where(u => u.Same).ToList())), "updates: nothing to update is said, not done");
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
            // no merge folder left; one that is gets said and, when it holds no file, goes, so only this check fails
            bool NoStore()
            {
                var folder = ModMerge.Folder(docs);
                if (!Directory.Exists(folder)) return true;
                Console.WriteLine("  merge folder left: " + string.Join(", ", Directory.EnumerateFileSystemEntries(folder, "*", SearchOption.AllDirectories).Select(f => Path.GetRelativePath(docs, f))));
                if (!Directory.EnumerateFiles(folder, "*", SearchOption.AllDirectories).Any()) Wipe(folder);
                return false;
            }
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
            check(hostile.All(h => Why(h.Rel).StartsWith("Can't read this package") || Why(h.Rel).Contains("protected")) && Same(before, Snap(docs)) && NoStore(),
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
                && Directory.GetFiles(Path.Combine(mods, "Packages"), "k3*.package").Any(f => File.ReadAllBytes(f).AsSpan().SequenceEqual(own)) && ModMerge.Orphans(docs).Count == 0 && NoStore(),
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
            check(!File.Exists(whole.Path) && !File.Exists(Path.Combine(mods, P("k5"))) && NoStore() && Directory.GetFiles(recycled).Length >= 3, "merge: removed with its mods, all of it goes to the Recycle Bin");
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
                check(canceled && Same(before, Snap(docs)) && NoStore() && File.Exists(p.LogPath), $"merge: a cancel in step {step} leaves no trace");
            }
            // a failure mid-write, before the move, mid-move and at the rename: everything as it was
            foreach (var at in new[] { "write", "verify", "move", "rename" })
            {
                ModMerge.FailPoint = x => { if (x == at) throw new IOException("injected"); };
                var failed = false;
                try { ModMerge.Run(ModMerge.Plan(docs, all, "Merged One", "Packages", false), null, default); }
                catch (IOException) { failed = true; }
                ModMerge.FailPoint = _ => { };
                check(failed && Same(before, Snap(docs)) && NoStore(), $"merge: a failure at \"{at}\" restores everything " + Diff(before, Snap(docs)));
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
                check(Same(before, Snap(docs)) && !File.Exists(res.Merged.Path) && Directory.GetFiles(recycled).Length == 1 && NoStore()
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
            check(Same(before2, Snap(docs)) && NoStore(), "split: both splits restore every original identical by hash " + Diff(before2, Snap(docs)));

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
            check(outB.Remerged is null && Same(before3, Snap(docs)) && NoStore(), "take out: one of two out leaves both plain, every file identical");

            // a mod added to a merge: it goes in beside the others, and splitting brings every file back identical
            var two = ModMerge.Run(ModMerge.Plan(docs, [M(A), M(B)], "Two", "Packages", false), null, default).Merged;
            var added = ModMerge.Edit(docs, two, [], [M(@"Packages\d.package")]);
            check(added.Remerged is { Mods: 3 } am && am.Merged.Name == "Two.package" && ModMerge.Info(am.Merged)!.Originals.Count == 3 && !File.Exists(Path.Combine(mods, @"Packages\d.package")),
                "edit: an added mod goes into the merge under the same name");
            ModMerge.Split(docs, added.Remerged!.Merged);
            check(Same(before3, Snap(docs)) && NoStore(), "edit: splitting after an add restores every file identical " + Diff(before3, Snap(docs)));

            // a split that stopped halfway (the launcher closed with one original back): the next split finishes it
            var half = ModMerge.Run(ModMerge.Plan(docs, [M(A), M(B)], "Half", "Packages", false), null, default).Merged;
            File.Move(Path.Combine(ModMerge.Folder(docs), "Half", A), Path.Combine(mods, A));
            ModMerge.Split(docs, half);
            check(Same(before3, Snap(docs)) && NoStore(), "split: one that stopped halfway is finished by the next, every file identical " + Diff(before3, Snap(docs)));

            // an antivirus that puts a file of its own in the store and removes it, again and again, is no kept mod and keeps no folder
            var scanned = ModMerge.Run(ModMerge.Plan(docs, [M(A), M(B)], "Scanned", "Packages", false), null, default).Merged;
            var scan = Path.Combine(ModMerge.Folder(docs), "Scanned", "Packages", "A.PACKAGE.tmp");
            FileStream Scan() => new(scan, FileMode.Create, FileAccess.Write, FileShare.None, 1, FileOptions.DeleteOnClose);
            File.Move(scanned.Path, scanned.Path + ".aside");
            using (Scan()) check(ModMerge.Orphans(docs) is [{ Files.Count: 2 }], "merge: what an antivirus adds for a moment is not a kept mod");
            File.Move(scanned.Path + ".aside", scanned.Path);
            var scanning = new Thread(() => { try { for (var i = 0; i < 40; i++) { using (Scan()) Thread.Sleep(3); Thread.Sleep(3); } } catch (Exception e) when (e is IOException or UnauthorizedAccessException) { } });
            ModMerge.FailPoint = x => { if (x == "split" && scanning.ThreadState == ThreadState.Unstarted) scanning.Start(); };
            ModMerge.Split(docs, scanned);
            ModMerge.FailPoint = _ => { };
            scanning.Join();
            check(Same(before3, Snap(docs)) && NoStore(), "split: the folder goes whatever an antivirus does in it meanwhile " + Diff(before3, Snap(docs)));

            // a merge that is off: a mod taken out comes back off, the merge stays off where it is
            var offMerge = ModMerge.Run(ModMerge.Plan(docs, [M(A), M(B), M(@"Packages\d.package")], "Off", "Packages", false), null, default).Merged;
            var offMerged = offMerge with { Path = Mods.SetEnabled(offMerge, false, docs), Enabled = false };
            var offOut = ModMerge.TakeOut(docs, offMerged, [Path.Combine("Mods", A)]);
            check(offOut.Back.Single() is { Enabled: false } ob && ob.Path == Path.Combine(Mods.Disabled(docs), A) && offOut.Remerged is { Mods: 2 } orm && !orm.Merged.Enabled && orm.Merged.Path == offMerged.Path
                && !File.Exists(Path.Combine(mods, A)), "take out: from a merge that is off, the mod comes back off and the merge stays off");
            foreach (var m in ModMerge.Split(docs, offOut.Remerged!.Merged).Append(offOut.Back[0])) Mods.SetEnabled(m, true, docs);
            check(Same(before3, Snap(docs)), "take out: the merge that is off still splits, every file identical " + Diff(before3, Snap(docs)));

            // a merge stopped during the move (the launcher ended with A kept aside, B not yet, the merged file still under its
            // temporary name): the next read puts it right; a merged file the player removed keeps its originals kept
            var stopped = ModMerge.Run(ModMerge.Plan(docs, [M(A), M(B)], "Stopped", "Packages", false), null, default).Merged;
            File.Move(stopped.Path, stopped.Path + ".tmp-" + Guid.NewGuid().ToString("N"));
            File.Move(Path.Combine(ModMerge.Folder(docs), "Stopped", B), Path.Combine(mods, B));
            ModMerge.Recover(docs);
            check(Same(before3, Snap(docs)) && NoStore(), "merge: one stopped during the move is undone at the next read, every file identical " + Diff(before3, Snap(docs)));
            var removed = ModMerge.Run(ModMerge.Plan(docs, [M(A), M(B)], "Removed", "Packages", false), null, default).Merged;
            File.Delete(removed.Path);
            ModMerge.Recover(docs);
            check(File.Exists(Path.Combine(ModMerge.Folder(docs), "Removed", A)) && !File.Exists(Path.Combine(mods, A)), "merge: a merged file the player removed keeps its originals kept");
            // a merge an older launcher kept inside Mods: next to Mods at the next read, and it still splits
            File.Move(Path.Combine(ModMerge.Folder(docs), "Removed", A), Path.Combine(mods, A));
            File.Move(Path.Combine(ModMerge.Folder(docs), "Removed", B), Path.Combine(mods, B));
            Wipe(ModMerge.Folder(docs));
            ModMerge.Recover(docs);
            var older = ModMerge.Run(ModMerge.Plan(docs, [M(A), M(B)], "Older", "Packages", false), null, default).Merged;
            var inMods = Path.Combine(docs, "Mods", ModMerge.FolderName);
            Directory.Move(ModMerge.Folder(docs), inMods);
            ModMerge.Recover(docs);
            check(!Directory.Exists(inMods) && File.Exists(Path.Combine(ModMerge.Folder(docs), "Older", A)) && ModMerge.IsMerged(older), "merge: originals an older launcher kept inside Mods move next to it");
            ModMerge.Split(docs, older);
            check(Same(before3, Snap(docs)) && NoStore(), "merge: one moved out of Mods still splits, every file identical " + Diff(before3, Snap(docs)));
            // a write cut short before its record: an old .package.tmp-* no record names goes, one written now stays
            var orphan = Path.Combine(mods, "Lost.package.tmp-" + Guid.NewGuid().ToString("N"));
            var fresh = Path.Combine(mods, "Busy.package.tmp-" + Guid.NewGuid().ToString("N"));
            File.WriteAllText(orphan, "half a merge"); File.SetLastWriteTime(orphan, DateTime.Now.AddDays(-1));
            File.WriteAllText(fresh, "a merge being written");
            ModMerge.Recover(docs);
            check(!File.Exists(orphan) && File.Exists(fresh), "merge: an orphan temporary file from an earlier run goes, one being written stays");
            File.Delete(fresh);
            foreach (var f in Directory.GetFiles(recycled)) File.Delete(f);

            Edits(check, root, docs, recycled);
        }
        catch (Exception e) { check(false, "merge: exception " + e); }
        finally
        {
            ModMerge.FailPoint = _ => { };
            ModMerge.Recycle = oldRecycle;
            Games.IsRunning = () => false;
            Paths.LocalAppData = oldLocal;
            ModNotes.Reload();
            try { Wipe(root); } catch (Exception) { }
        }
    }
}
