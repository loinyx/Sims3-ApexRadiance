namespace ApexSetup;

public static partial class ModMerge
{
    public sealed record TakeOutResult(List<ModFile> Back, MergeResult? Remerged, IReadOnlyList<MergeSkip> Left); // Left: mods that stay, but could not go in again

    // Some mods taken out of a merge: the merge is split (every original checked by hash and put back), then the ones that
    // stay are merged again under the same name and folder, through the same plan, write and verify. One left alone stays
    // a plain mod. A merge that cannot be made again leaves every original back in place, the safe state.
    public static TakeOutResult TakeOut(string docs, ModFile merged, IReadOnlyCollection<string> outRels, IProgress<MergeProgress>? progress = null, CancellationToken ct = default)
        => Edit(docs, merged, outRels, [], progress, ct);

    // Edit merge: the same split and merge again, with other mods added to the ones that stay. The plan is checked before
    // the split, so a mod that cannot go in changes nothing.
    public static TakeOutResult Edit(string docs, ModFile merged, IReadOnlyCollection<string> outRels, IReadOnlyList<ModFile> add, IProgress<MergeProgress>? progress = null, CancellationToken ct = default)
    {
        var info = Info(docs, merged) ?? throw new SetupException("This file is not a merge made by the launcher.");
        // (a merge that is off splits into originals that are off, which a merge skips: every one would be left loose)
        if (!merged.Enabled) throw new SetupException("Nothing was changed: {0}", "Turned off.");
        var name = Path.GetFileNameWithoutExtension(merged.Name);
        var folder = Path.GetRelativePath("Mods", Path.GetDirectoryName(merged.Rel) ?? "Mods") is var f && f == "." ? "" : f;
        if (add.Count > 0)
        {
            // (under a scratch name: the merge's own name is taken by the merge itself until the split)
            var check = Plan(docs, [merged, .. add], name + " (edit)", folder, includeScripts: true, [merged.Rel]);
            if (check.Skipped.FirstOrDefault() is { } skip) throw new SetupException("Nothing was changed: {0} can't go in. {1}", skip.Mod.Name, skip.Reason.Text);
            if (check.Refusals.Count > 0) throw new SetupException("Nothing was changed: {0}", string.Join(" ", check.Refusals.Select(r => r.Text)));
        }
        lock (Gate) return EditNow(docs, merged, outRels, add, name, folder, info.Priority, progress, ct);
    }

    static TakeOutResult EditNow(string docs, ModFile merged, IReadOnlyCollection<string> outRels, IReadOnlyList<ModFile> add, string name, string folder, IReadOnlyList<string> priority, IProgress<MergeProgress>? progress, CancellationToken ct)
    {
        var back = Split(docs, merged);
        var stay = back.Where(m => !outRels.Contains(m.Rel, StringComparer.OrdinalIgnoreCase)).Concat(add).ToList();
        if (stay.Count < 2) return new(back, null, []);
        var plan = Plan(docs, stay, name, folder, includeScripts: true, priority);
        if (!plan.CanRun) throw new SetupException("The mods were put back, but the rest could not be merged again: {0}", string.Join(" ", plan.Refusals.Select(r => r.Text)));
        return new(back, Run(plan, progress, ct), plan.Skipped);
    }
}
