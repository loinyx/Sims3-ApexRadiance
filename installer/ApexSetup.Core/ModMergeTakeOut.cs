namespace ApexSetup;

public static partial class ModMerge
{
    public sealed record TakeOutResult(List<ModFile> Back, MergeResult? Remerged);

    // Some mods taken out of a merge: the merge is split (every original checked by hash and put back), then the ones that
    // stay are merged again under the same name and folder, through the same plan, write and verify. One left alone stays
    // a plain mod. A merge that cannot be made again leaves every original back in place, the safe state.
    public static TakeOutResult TakeOut(string docs, ModFile merged, IReadOnlyCollection<string> outRels, IProgress<MergeProgress>? progress = null, CancellationToken ct = default)
    {
        var info = Info(docs, merged) ?? throw new SetupException("This file is not a merge made by the launcher.");
        var name = Path.GetFileNameWithoutExtension(merged.Name);
        var folder = Path.GetRelativePath("Mods", Path.GetDirectoryName(merged.Rel) ?? "Mods") is var f && f == "." ? "" : f;
        var back = Split(docs, merged);
        var stay = back.Where(m => !outRels.Contains(m.Rel, StringComparer.OrdinalIgnoreCase)).ToList();
        if (stay.Count < 2) return new(back, null);
        var plan = Plan(docs, stay, name, folder, includeScripts: true);
        if (!plan.CanRun) throw new SetupException("The mods were put back, but the rest could not be merged again: {0}", string.Join(" ", plan.Refusals.Select(r => r.Text)));
        return new(back, Run(plan, progress, ct));
    }
}
