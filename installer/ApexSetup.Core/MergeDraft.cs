using System.Collections.ObjectModel;

namespace ApexSetup;

// A change to a merge while the player makes it: the merge's mods in the order the screen lists them (the first one's copy
// counts), what becomes of each, and the merge's new name. Nothing is done until its plan is run (ModMerge.Run).
public sealed class MergeDraft
{
    public sealed class Item
    {
        public required string Rel { get; init; }           // the original's, or the added mod's own
        public required long Size { get; init; }
        public ModFile? Added { get; init; }                // a mod of the list that goes in
        public ModFile? With { get; set; }                  // the file that takes its place
        public bool Newer { get; set; }                     // (With is a newer version the launcher found)
        public TakeOutTo? Out { get; set; }
        public string Name => Path.GetFileNameWithoutExtension(Rel);
        public bool Changed => Added is not null || With is not null || Out is not null;
        // the mod's Rel once the change is made (MergePlan.Shares is by it)
        public string NewRel => With is not { } w ? Rel : w.Rel.StartsWith("Mods" + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase) ? w.Rel : Path.Combine(Path.GetDirectoryName(Rel) ?? "", w.Name);
    }

    readonly List<string> _was;

    public MergeDraft(MergeRecord rec)
    {
        (Dir, Sha256, Rel) = (rec.Dir, rec.Sha256, rec.Merged);
        Items = new(rec.Originals.Select(o => new Item { Rel = o.Rel, Size = o.Size }));
        _was = Items.Select(i => i.Rel).ToList();
    }

    public string Dir { get; }                              // which merge (its folder) and which version of it
    public string Sha256 { get; }
    public string Rel { get; set; }                         // the merged file's
    public string? Name { get; set; }                       // a new name; null: as it is
    public ObservableCollection<Item> Items { get; }

    // the mods that stay are in another order, or an added one is not at the end
    public bool Ordered
    {
        get
        {
            var stay = Items.Where(i => i.Out is null).ToList();
            var old = stay.Where(i => i.Added is null).Select(i => i.Rel).ToList();
            return !old.SequenceEqual(_was.Where(old.Contains)) || stay.SkipWhile(i => i.Added is null).Any(i => i.Added is null);
        }
    }
    public bool Changed => Name is not null || Ordered || Items.Any(i => i.Changed);
    public bool OnlyName => Name is not null && !Ordered && !Items.Any(i => i.Changed); // (a rename: no need to write it again)
    public int Left => Items.Count(i => i.Out is null);

    // ordered: the whole order given, whether the player changed it or not
    public MergeEdit Edit(bool ordered = false)
    {
        var e = new MergeEdit { Name = Name };
        foreach (var i in Items)
            if (i.Added is { } a) e.Add.Add(a);
            else if (i.Out is { } to) e.Out[i.Rel] = to;
            else if (i.With is { } w) e.Replace[i.Rel] = w;
        if (ordered || Ordered) e.Order = Items.Where(i => i.Out is null).Select(i => i.Rel).ToList();
        return e;
    }

    // The change as it is now, to be planned off the window's thread while the player goes on. An added mod that clashes
    // goes in by its place in the list; the order is given only when it is needed (it makes an old merge the player's own).
    public Func<MergeEditPlan> Plan(string docs, ModFile merged)
    {
        var (edit, ordered) = (Edit(), Edit(ordered: true));
        return () =>
        {
            var p = ModMerge.PlanEdit(docs, merged, edit);
            return edit.Order is null && p.Plan?.Skipped.Any(s => s.Reason.Key == ModMerge.ClashReason) == true ? ModMerge.PlanEdit(docs, merged, ordered) : p;
        };
    }
}
