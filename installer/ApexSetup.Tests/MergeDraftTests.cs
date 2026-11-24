// Offline checks of a merge's change while the player makes it (ApexSetup.Core/MergeDraft.cs), planned on packages made here.

using ApexSetup;

static class MergeDraftTests
{
    public static void Run(Action<bool, string> check)
    {
        static string R(string n) => Path.Combine("Mods", "Packages", n + ".package");
        static ModFile F(string n) => new(@"C:\x\" + n + ".package", R(n), 1, ModKind.Package, true, false);
        var rec = new MergeRecord { Merged = R("m"), Dir = "d", Sha256 = "s", Originals = new[] { "a", "b", "c" }.Select(n => new MergeOriginal { Rel = R(n), Size = 1 }).ToList() };

        var d = new MergeDraft(rec);
        check(!d.Changed && !d.OnlyName && d.Left == 3 && d.Edit() is { Name: null, Order: null } e0 && e0.Out.Count + e0.Add.Count + e0.Replace.Count == 0, "draft: nothing changed at first");
        d.Name = "new";
        check(d.Changed && d.OnlyName && d.Edit().Name == "new", "draft: a name alone is a rename");
        d.Name = null;
        d.Items.Move(2, 0);
        check(d.Changed && d.Ordered && d.Edit().Order!.SequenceEqual([R("c"), R("a"), R("b")]), "draft: another order is given whole");
        d.Items.Move(0, 2);
        check(!d.Changed, "draft: the order put back is no change");
        d.Items[1].Out = TakeOutTo.Off;
        check(d is { Changed: true, Ordered: false, Left: 2 } && d.Edit() is { Order: null } e1 && e1.Out.Single() is { Value: TakeOutTo.Off } o && o.Key == R("b"), "draft: a mod taken out leaves the order as it is");
        d.Items.Move(1, 0);
        check(!d.Ordered, "draft: where a mod that leaves is listed changes nothing");
        d.Items[0].Out = null;
        check(d.Edit().Order!.SequenceEqual([R("b"), R("a"), R("c")]), "draft: kept after all, its new place counts");
        d.Items.Move(0, 1);
        d.Items.Add(new MergeDraft.Item { Rel = R("z"), Size = 1, Added = F("z") });
        check(d is { Changed: true, Ordered: false, Left: 4 } && d.Edit() is { Order: null } e2 && e2.Add.Single().Rel == R("z"), "draft: an added mod at the end is not another order");
        d.Items.Move(3, 1);
        check(d.Edit().Order!.SequenceEqual([R("a"), R("z"), R("b"), R("c")]), "draft: an added mod higher up is another order");
        d.Items.RemoveAt(1);
        d.Items[0].With = ModMerge.Outside(Environment.ProcessPath!);
        check(d.Edit().Replace.Keys.Single() == R("a") && d.Items[0].NewRel == Path.Combine("Mods", "Packages", Path.GetFileName(Environment.ProcessPath!)), "draft: a file from outside takes its mod's place and folder");
        d.Items[0].With = F("y");
        check(d.Items[0].NewRel == R("y"), "draft: a mod of the list keeps its own place");

        var root = Path.Combine(Path.GetTempPath(), "ApexDraftTest-" + Guid.NewGuid().ToString("N")[..8]);
        var oldLocal = Paths.LocalAppData;
        try
        {
            Paths.LocalAppData = Path.Combine(root, "Local");
            ModNotes.Reload();
            var docs = Path.Combine(root, "Docs");
            var mods = Path.Combine(docs, "Mods");
            Directory.CreateDirectory(mods);
            File.WriteAllText(Path.Combine(mods, "Resource.cfg"), "Priority 500\nPackedFile Packages/*.package\n");
            ModFile Make(string n, params (uint, ulong, byte[], bool, int)[] res)
            {
                var p = Path.Combine(mods, "Packages", n + ".package");
                ModMergeTests.Pkg(p, res);
                return new(p, R(n), new FileInfo(p).Length, ModKind.Package, true, false);
            }
            var (a, b, z) = (Make("a", ModMergeTests.Raw(100, 1, "a")), Make("b", ModMergeTests.Raw(101, 2, "b")), Make("z", ModMergeTests.Raw(102, 3, "z"), ModMergeTests.Raw(100, 1, "other")));
            var merged = ModMerge.Run(ModMerge.Plan(docs, [a, b], "m", "Packages", false), null, default).Merged;
            var draft = new MergeDraft(ModMerge.Info(docs, merged)!);
            draft.Items.Add(new MergeDraft.Item { Rel = z.Rel, Size = z.Size, Added = z });
            var plan = draft.Plan(docs, merged)();
            check(draft.Edit().Order is null && plan.CanRun && plan.Plan!.Inputs.Select(m => m.Rel).SequenceEqual([a.Rel, b.Rel, z.Rel]) && plan.Plan.Shares[z.Rel] == new MergeShare(0, 1),
                "draft: an added mod that clashes goes in by its place, and the plan says what of it is replaced");
            draft.Items.Move(2, 0);
            plan = draft.Plan(docs, merged)();
            check(plan.CanRun && plan.Plan!.Shares[z.Rel] == new MergeShare(1, 0) && plan.Plan.Shares[a.Rel] == new MergeShare(0, 1), "draft: moved up, its copy counts");
            check(File.Exists(z.Path) && ModMerge.Info(docs, merged)!.Originals.Count == 2, "draft: a plan changes nothing");
        }
        finally
        {
            Paths.LocalAppData = oldLocal;
            ModNotes.Reload();
            try { Directory.Delete(root, true); } catch (Exception) { }
        }
    }
}
