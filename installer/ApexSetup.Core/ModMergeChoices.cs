using System.Collections.ObjectModel;

namespace ApexSetup;

// One mod of the merge being made. It goes in when the player puts it in, or when it clashes with nothing and the player
// does not leave it out; one that clashes and is not decided on stays out.
public sealed class MergeChoice(ModFile mod)
{
    public ModFile Mod { get; } = mod;
    public bool? Pick { get; set; }                 // put in, left out, or not decided
    public MergeMeets? Meets { get; internal set; } // whom it clashes with; null: nobody
    public MergeShare? Share { get; internal set; } // how much of what clashes is its own, when it goes in
    public MergeReason? Why { get; internal set; }  // why it stays out although it would go in (too big for its part)
    public bool Scripts { get; internal set; }      // it goes in the separate package of the script mods
    public bool Undecided => Meets is not null && Pick is null;
    public bool In => Pick ?? Meets is null;
}

// the rows; thousands of them join in one change (one change each kept the window laying the list out)
public sealed class MergeRows : ObservableCollection<MergeChoice>
{
    public void Join(IReadOnlyCollection<MergeChoice> top, IReadOnlyCollection<MergeChoice> end)
    {
        if (top.Count + end.Count > 20)
        {
            var list = (List<MergeChoice>)Items;
            list.InsertRange(0, top);
            list.AddRange(end);
            OnCollectionChanged(new(System.Collections.Specialized.NotifyCollectionChangedAction.Reset));
            return;
        }
        var at = 0;
        foreach (var r in top) Insert(at++, r);
        foreach (var r in end) Add(r);
    }
}

// What the merge dialog holds: the mods in the order that counts (the top one's copy wins) and what the player chose for
// each, kept in step with the plans made from them.
public sealed class MergeChoices
{
    public MergeRows Rows { get; } = new();
    public MergeChoices() => ModMerge.Fresh(); // (a new dialog: the files as they are now)
    readonly Dictionary<string, MergeChoice> _known = new(StringComparer.OrdinalIgnoreCase); // (a mod that goes and comes back keeps its choice)

    public List<string> Order => Rows.Where(r => r.In).Select(r => r.Mod.Rel).ToList();
    public List<string> LeaveOut => Rows.Where(r => r.Pick == false).Select(r => r.Mod.Rel).ToList();

    // The plans made with this order and these left out, taken in. False: they say something new (a mod that clashes now and
    // the player has not decided on), so what was planned is not what the rows say and it is planned again.
    public bool Take(IReadOnlyList<MergePlan> plans, MergePlan? scripts, IReadOnlyList<string> order, IReadOnlyList<string> leaveOut)
    {
        var all = scripts is null ? plans : [.. plans, scripts];
        var skips = all.SelectMany(p => p.Skipped).Where(s => scripts is null || s.Reason.Key != ModMerge.ScriptReason).GroupBy(s => s.Mod.Rel, StringComparer.OrdinalIgnoreCase).ToDictionary(g => g.Key, g => g.Last(), StringComparer.OrdinalIgnoreCase);
        var mods = all.SelectMany(p => p.Clashes).SelectMany(c => c.Mods).Concat(all.SelectMany(p => p.Inputs))
            .Concat(skips.Values.Where(s => s.Reason.Key is ModMerge.ClashReason or ModMerge.LeftOutReason).Select(s => s.Mod))
            .Concat(Rows.Select(r => r.Mod).Where(m => skips.TryGetValue(m.Rel, out var s) && s.Reason.Key != ModMerge.ScriptReason))
            .DistinctBy(m => m.Rel, StringComparer.OrdinalIgnoreCase).ToList();
        var now = mods.Select(m => m.Rel).ToHashSet(StringComparer.OrdinalIgnoreCase);
        for (var i = Rows.Count - 1; i >= 0; i--) if (!now.Contains(Rows[i].Mod.Rel)) Rows.RemoveAt(i);
        var had = Rows.Select(r => r.Mod.Rel).ToHashSet(StringComparer.OrdinalIgnoreCase);
        var clash = all.SelectMany(p => p.Meets).DistinctBy(m => m.Key, StringComparer.OrdinalIgnoreCase).ToDictionary(m => m.Key, m => m.Value, StringComparer.OrdinalIgnoreCase);
        // (the ones that clash come in on top, where the player decides on them; the rest in the game's load order)
        var fresh = mods.Where(m => !had.Contains(m.Rel)).Select(m => _known.TryGetValue(m.Rel, out var k) ? k : _known[m.Rel] = new(m)).ToList();
        Rows.Join(fresh.Where(r => clash.ContainsKey(r.Mod.Rel)).ToList(), fresh.Where(r => !clash.ContainsKey(r.Mod.Rel)).ToList());
        var shares = all.SelectMany(p => p.Shares).DistinctBy(x => x.Key, StringComparer.OrdinalIgnoreCase).ToDictionary(x => x.Key, x => x.Value, StringComparer.OrdinalIgnoreCase);
        var apart = (scripts is null ? [] : scripts.Clashes.SelectMany(c => c.Mods).Concat(scripts.Inputs).Concat(scripts.Skipped.Select(s => s.Mod))).Select(m => m.Rel).ToHashSet(StringComparer.OrdinalIgnoreCase);
        foreach (var r in Rows)
        {
            r.Meets = clash.GetValueOrDefault(r.Mod.Rel);
            r.Share = shares.GetValueOrDefault(r.Mod.Rel);
            r.Scripts = apart.Contains(r.Mod.Rel);
            r.Why = skips.TryGetValue(r.Mod.Rel, out var s) && s.Reason.Key is not (ModMerge.ClashReason or ModMerge.LeftOutReason) ? s.Reason : null;
        }
        // (planned with no order yet, the game's load order is the rows')
        return Order.SequenceEqual(order, StringComparer.OrdinalIgnoreCase) && LeaveOut.SequenceEqual(leaveOut, StringComparer.OrdinalIgnoreCase)
            || order.Count == 0 && leaveOut.Count == 0 && all.SelectMany(p => p.Inputs).Select(m => m.Rel).SequenceEqual(Order, StringComparer.OrdinalIgnoreCase);
    }

    // why each mod that has no row stays out
    public List<MergeSkip> Others(IReadOnlyList<MergePlan> plans, MergePlan? scripts)
    {
        var shown = Rows.Select(r => r.Mod.Rel).ToHashSet(StringComparer.OrdinalIgnoreCase);
        return plans.SelectMany(p => p.Skipped).Where(s => scripts is null || s.Reason.Key != ModMerge.ScriptReason).Concat(scripts?.Skipped ?? [])
            .Where(s => !shown.Contains(s.Mod.Rel)).ToList();
    }
}
