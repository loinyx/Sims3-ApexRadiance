using System.Buffers.Binary;
using System.Diagnostics;
using System.Security.Cryptography;
using System.Text;
using System.Text.Json;

namespace ApexSetup;

// One English key with {n} arguments, translated by the UI like SetupException
public sealed record MergeReason(string Key, string[] Args)
{
    public string Text => string.Format(Key, Args);
}
public sealed record MergeSkip(ModFile Mod, MergeReason Reason);
// one resource key ("type:group:instance") several packages hold alike: it goes in once
public sealed record MergeDuplicate(string Key, string Kept, IReadOnlyList<string> Dropped);
// mods that change the same game data in different ways (Resources of them), and the loaded ones outside the merge whose
// order against them it would change. They stay out unless the player gives one of them priority.
public sealed record MergeClash(IReadOnlyList<ModFile> Mods, IReadOnlyList<string> Outside, int Resources);
// of the resources a mod in the merge holds differently from another one in it: how many are its own copy, and how many
// another mod's copy replaces (the rest of the mod goes in whole)
public sealed record MergeShare(int Kept, int Replaced);
// whom a mod clashes with: the other mods of the merge (by Rel, in how many resources), those outside it, and its clashing
// resources in all
public sealed record MergeMeets(IReadOnlyDictionary<string, int> With, IReadOnlyList<string> Outside, int Resources);

public sealed class MergePlan
{
    public required string Docs { get; init; }
    public required string Name { get; init; }
    public required string TargetPath { get; init; }   // Mods\<folder>\<name>.package
    public required string TargetRel { get; init; }    // as ModFile.Rel
    public required string StoreDir { get; init; }     // Merged (Apex Launcher)\<name> next to Mods, where the originals go
    public List<ModFile> Inputs { get; } = new();      // in the merge's order (the first one wins a shared key)
    public List<MergeSkip> Skipped { get; } = new();   // (of a merge in parts: on the first part)
    public List<MergeDuplicate> Duplicates { get; } = new();
    public List<MergeClash> Clashes { get; } = new();  // (of a merge in parts: on the first part)
    // The player's order, as Rels, the first one's copy kept. A mod that clashes goes in only when it is listed here; the
    // mods that clash with nothing follow the listed ones in the game's load order.
    public IReadOnlyList<string> Order { get; init; } = [];
    public IReadOnlyList<string> Priority => Order;    // (the name the plan dialog reads)
    public Dictionary<string, MergeShare> Shares { get; } = new(StringComparer.OrdinalIgnoreCase); // by Rel, the mods that clash and go in
    public Dictionary<string, MergeMeets> Meets { get; } = new(StringComparer.OrdinalIgnoreCase); // by Rel, every mod that clashes, in or out (of a merge in parts: on the first part)
    // the script mods set aside: they can be merged on their own, as a second merge
    public IEnumerable<ModFile> Scripts => Skipped.Where(s => s.Reason.Key == ModMerge.ScriptReason).Select(s => s.Mod);
    public List<MergeReason> Refusals { get; } = new();
    public int Resources { get; internal set; }
    public long OutputBytes { get; internal set; }      // exact: header, stored resource bytes, index
    public bool CanRun => Refusals.Count == 0;
    public string? LogPath { get; internal set; }      // set by Run, also when it fails
    internal List<MergeSource> Sources { get; } = new();
    internal long Data => Sources.Sum(s => s.Keep.Sum(e => (long)e.FileSize)) + Made.Sum(m => (long)m.Data.Length);
    internal List<(ModConflicts.Entry E, byte[] Data)> Made { get; } = new(); // written by the merge itself: joined name maps
    internal EditOf? Of { get; init; }                  // the merge this plan changes, when it is not a new one
}

internal sealed record MergeSource(ModFile Mod, long Size, DateTime Written, List<ModConflicts.Entry> Keep);
// A plan for a merge that exists: its file and record, and the files it takes as they are (its own kept originals and files
// from outside Mods, which are no mods of the list)
internal sealed record EditOf(MergeRecord Rec, ModFile Merged, HashSet<string> Held);

public enum MergeStep { Check, Write, Verify, Move, Finish }
// Done / Total: bytes in Check, Write and Verify, files in Move
public readonly record struct MergeProgress(MergeStep Step, long Done, long Total);
public sealed record MergeResult(ModFile Merged, string StoreDir, int Mods, int Resources, long Bytes, string Sha256, string LogPath, TimeSpan Elapsed);

// merge.json, next to the originals
public sealed class MergeOriginal
{
    public string Rel { get; set; } = "";
    public long Size { get; set; }
    public string Sha256 { get; set; } = "";
    public ModNote? Note { get; set; }
}
public sealed class MergeRecord
{
    public int Version { get; set; } = 1;              // 2: no mod that would lose a part went in
    public string Name { get; set; } = "";
    public string Merged { get; set; } = "";           // the merged file's Rel
    public string Sha256 { get; set; } = "";
    public long Size { get; set; }
    public int Resources { get; set; }
    public DateTime Created { get; set; }
    public List<MergeOriginal> Originals { get; set; } = new(); // in the merge's order (the first one wins a shared key)
    public List<string> Priority { get; set; } = new(); // the player's choice between mods that clash (MergePlan.Order)
    public MergePending? Pending { get; set; }          // a change being made (ModMergeTakeOut): undone when found at the next start
    [System.Text.Json.Serialization.JsonIgnore] public string Dir { get; set; } = "";
}
// Paths relative to Documents\...\The Sims 3. Copy: To is a copy the change made (undone by removing it).
public sealed record MergeMove(string From, string To, bool Copy = false);
public sealed class MergePending
{
    public List<MergeMove> Moves { get; set; } = new();
    public string? Tmp { get; set; }                    // the new merged file while it is not in place
}

// Merge packages into one: the plan says what goes in (mods that change the same game data in different ways stay out, see
// ModMergeSafe), Run writes a temporary file copying every resource's stored bytes as they are (compressed stays
// compressed), reads it back and compares each resource, then moves the originals to Merged (Apex Launcher)\<name>
// (next to Mods, so no Resource.cfg loads them) and renames the new file into place. Split puts the
// originals back by hash and sends the merged file to the Recycle Bin.
// The uninstall leaves merges alone: the merged package works without the launcher, merge.json keeps the notes once the
// launcher's data is gone, and splitting there could stop halfway on a replaced original.
public static partial class ModMerge
{
    public const string FolderName = "Merged (Apex Launcher)";
    public static string Folder(string docs) => Path.Combine(docs, FolderName);
    static string OldFolder(string docs) => Path.Combine(docs, "Mods", FolderName); // (where 2.1 and older kept them)
    public const string ShownFolder = "The Sims 3\\" + FolderName;
    // an original kept by a merge, not one of the player's mods now
    public static bool Keeps(string docs, string path) => new[] { Folder(docs), OldFolder(docs) }
        .Any(d => Path.GetFullPath(path).StartsWith(Path.GetFullPath(d) + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase));
    public const long MaxBytes = int.MaxValue; // (the game is 32-bit and the index offsets are 32-bit)
    const int MaxEntries = 200_000;

    // test hook: called at "write" (after the first resource), "verify", "move" (after each original) and "rename"
    public static Action<string> FailPoint = _ => { };
    // test hook: the Recycle Bin (tests must not fill the real one)
    public static Action<string> Recycle = ModLibrary.Recycle;

    static MergeReason R(string key, params string[] args) => new(key, args);
    static string Slash(string p) => p.Replace('\\', '/');
    static string UnderMods(string rel) => Path.GetRelativePath("Mods", rel);

    public const string ClashReason = "Changes the same game data as {0}. A merge keeps only one version.";
    public const string ScriptReason = "A script mod: left out unless you include scripts.";
    public const string LeftOutReason = "You left it out.";
    public const long PartBytes = 1L << 30; // (the size asked of one part of a merge in parts: the game is a 32-bit program)

    // ---------- plan ----------
    // order: the player's order, as Rels, the first one's copy kept where two mods hold a resource differently. A mod that
    // clashes goes in only when it is in the order, so one the player has not decided on stays out. leaveOut: the mods the
    // player leaves out (they stay in the plan, skipped, so the choice can be shown and taken back).
    public static MergePlan Plan(string docs, IReadOnlyList<ModFile> mods, string name, string folderRel, bool includeScripts, IReadOnlyList<string>? order = null, IReadOnlyCollection<string>? leaveOut = null)
        => Plans(docs, mods, name, folderRel, includeScripts, order, leaveOut, long.MaxValue, null)[0];

    // The same merge as several packages of about partBytes at most, "Name 1", "Name 2": each one a merge of its own, to
    // run, split and edit on its own. Mods that clash share a part, so whose copy counts does not depend on the order the
    // game loads the parts in. One plan, under the plain name, when everything fits in one part.
    public static List<MergePlan> PlanParts(string docs, IReadOnlyList<ModFile> mods, string name, string folderRel, bool includeScripts, IReadOnlyList<string>? order = null, IReadOnlyCollection<string>? leaveOut = null, long partBytes = PartBytes)
        => Plans(docs, mods, name, folderRel, includeScripts, order, leaveOut, partBytes, null);

    static List<MergePlan> Plans(string docs, IReadOnlyList<ModFile> mods, string name, string folderRel, bool includeScripts, IReadOnlyList<string>? order, IReadOnlyCollection<string>? leaveOut, long partBytes, EditOf? of)
    {
        order ??= [];
        var modsDir = Path.GetFullPath(Path.Combine(docs, "Mods"));
        if (name.EndsWith(".package", StringComparison.OrdinalIgnoreCase)) name = name[..^8];
        var common = new List<MergeReason>();
        var folderFull = Path.GetFullPath(Path.Combine(modsDir, folderRel));
        bool Inside(string full, string dir) => full.StartsWith(dir + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase) || full.Equals(dir, StringComparison.OrdinalIgnoreCase);
        if (!Inside(folderFull, modsDir) || Inside(folderFull, Path.GetFullPath(Mods.Disabled(docs))) || Inside(folderFull, Path.GetFullPath(OldFolder(docs))))
        {
            common.Add(R("That folder is outside Mods."));
            folderFull = modsDir;
        }
        try { Engine.CheckWritable(); }
        catch (SetupException e) { common.Add(new(e.Key, e.Args)); }
        catch (InvalidOperationException e) { common.Add(R(e.Message)); } // (dry run)
        var cfg = Path.Combine(modsDir, "Resource.cfg");
        var dirs = File.Exists(cfg) ? ModConflicts.ParseResourceCfg(File.ReadAllText(cfg)) : null;
        var skipped = new List<MergeSkip>();
        var clashes = new List<MergeClash>();
        var meets = new Dictionary<string, MergeMeets>(StringComparer.OrdinalIgnoreCase);
        var clashKeys = new Dictionary<string, HashSet<(uint, uint, ulong)>>(StringComparer.OrdinalIgnoreCase); // (by Rel)
        var groupOf = new Dictionary<string, int>(StringComparer.OrdinalIgnoreCase);

        // one package of the merge: the name and place checked, then what each mod puts in it
        MergePlan Make(string n, List<(MergeSource Src, (int, int, string) Key)> part)
        {
            var refusals = new List<MergeReason>();
            try { n = ModLibrary.CleanName(n); }
            catch (SetupException e) { refusals.Add(new(e.Key, e.Args)); }
            // (a merge that is off is written where it is, under the folder for switched-off mods)
            var targetPath = Path.Combine(of is null ? folderFull : Path.GetDirectoryName(of.Merged.Path)!, n + ".package");
            var targetRel = Path.Combine("Mods", Path.GetRelativePath(modsDir, Path.Combine(folderFull, n + ".package")));
            var plan = new MergePlan { Docs = docs, Name = n, TargetPath = targetPath, TargetRel = targetRel, StoreDir = of?.Rec.Dir ?? Path.Combine(Folder(docs), n), Order = order, Of = of };
            plan.Refusals.AddRange(refusals);
            plan.Refusals.AddRange(common);
            var (on, off) = (Path.Combine(docs, targetRel), Path.Combine(Mods.Disabled(docs), UnderMods(targetRel)));
            // (a store left holding only merge.json is what an earlier split could not delete: cleaned first, it holds no original)
            if (of is null && !File.Exists(on)) DeleteStore(plan.StoreDir);
            if (of is null ? File.Exists(on) || File.Exists(off) || Directory.Exists(plan.StoreDir) : !targetRel.Equals(of.Merged.Rel, StringComparison.OrdinalIgnoreCase) && (File.Exists(on) || File.Exists(off)))
                plan.Refusals.Add(R("A file or merge named {0} already exists here.", n));
            if (targetPath.Length >= ModLibrary.MaxGamePath) plan.Refusals.Add(R("That name is too long for the game to read. Use a shorter one."));
            if (dirs is null) { plan.Refusals.Add(R("There is no Resource.cfg in Mods, so the game loads no packages from there.")); return plan; }
            if (ModConflicts.LoadKey(dirs, Slash(UnderMods(targetRel))) is null)
                plan.Refusals.Add(R("The game does not load packages from {0} (Resource.cfg).", Path.GetDirectoryName(targetRel) ?? "Mods"));
            if (part.Count < 2) plan.Refusals.Add(R("Pick at least two packages that can be merged."));

            // a resource several packages hold alike goes in once; a package's own repeats of one stay
            var owner = new Dictionary<(uint, uint, ulong), int>();
            var dropped = new Dictionary<(uint, uint, ulong), List<string>>();
            var names = new Dictionary<(uint, uint, ulong), List<(int I, ModConflicts.Entry E)>>();
            for (var i = 0; i < part.Count; i++)
            {
                var (src, _) = part[i];
                var all = src.Keep.ToList();
                src.Keep.Clear();
                foreach (var e in all)
                {
                    var k = (e.Type, e.Group, e.Instance);
                    if (e.Type == NameMap) (names.TryGetValue(k, out var m) ? m : names[k] = new()).Add((i, e));
                    if (owner.TryAdd(k, i) || owner[k] == i) { src.Keep.Add(e); continue; }
                    if (!dropped.TryGetValue(k, out var l)) dropped[k] = l = new();
                    if (!l.Contains(src.Mod.Rel)) l.Add(src.Mod.Rel);
                }
                plan.Inputs.Add(src.Mod);
                plan.Sources.Add(src);
            }
            foreach (var (k, l) in dropped)
                plan.Duplicates.Add(new($"{k.Item1:x8}:{k.Item2:x8}:{k.Item3:x16}", part[owner[k]].Src.Mod.Rel, l));
            for (var i = 0; i < part.Count; i++)
                if (clashKeys.TryGetValue(part[i].Src.Mod.Rel, out var mine))
                    plan.Shares[part[i].Src.Mod.Rel] = new(mine.Count(k => owner.GetValueOrDefault(k, -1) == i && dropped.ContainsKey(k)), mine.Count(k => owner.TryGetValue(k, out var o) && o != i));
            foreach (var (k, l) in names.Where(x => x.Value.Any(y => y.I != x.Value[0].I)))
                if (JoinNames(l.Select(x => (part[x.I].Src.Mod.Path, x.E))) is { } joined)
                {
                    part[owner[k]].Src.Keep.RemoveAll(e => (e.Type, e.Group, e.Instance) == k);
                    plan.Made.Add((new(k.Item1, k.Item2, k.Item3, 0, joined.Length, (uint)joined.Length, 0), joined));
                }
            plan.Resources = plan.Sources.Sum(s => s.Keep.Count) + plan.Made.Count;
            plan.OutputBytes = 96 + plan.Data + 4 + 32L * plan.Resources;
            if (plan.OutputBytes > MaxBytes) plan.Refusals.Add(R("The merged package would be over 2 GB."));
            if (plan.Resources > MaxEntries) plan.Refusals.Add(R("The merged package would hold too many resources."));
            return plan;
        }
        if (dirs is null) return [Make(name, [])];

        var ok = new List<(MergeSource Src, (int, int, string) Key)>();
        foreach (var m in mods)
        {
            MergeReason? why = null;
            List<ModConflicts.Entry>? entries = null;
            var held = of?.Held.Contains(m.Path) == true;
            var key = ModConflicts.LoadKey(dirs, Slash(UnderMods(m.Rel)));
            if (m.FileType != "package" || !m.Rel.StartsWith("Mods" + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase)) why = R("Not a package in Mods.");
            else if (m.Locked) why = R("Part of what the launcher installs.");
            else if (!held && !m.Enabled) why = R("Turned off.");
            else if (!held && Keeps(docs, m.Path)) why = R("Kept by an earlier merge.");
            else if (m.Name.StartsWith('~') || m.Name.Contains(".tmp", StringComparison.OrdinalIgnoreCase)) why = R("A temporary file.");
            else if (key is null) why = R("The game does not load it (Resource.cfg).");
            else
            {
                (entries, why) = Probe(m.Path);
                if (entries is not null && !includeScripts && entries.Any(e => e.Type == ModConflicts.TypeS3sa)) why = R(ScriptReason);
            }
            if (why is not null) { skipped.Add(new(m, why)); continue; }
            var fi = new FileInfo(m.Path);
            ok.Add((new MergeSource(m, fi.Length, fi.LastWriteTimeUtc, entries!), key!.Value));
        }
        ok.Sort((a, b) => ModConflicts.CompareKey(a.Key, b.Key));

        // (mods that clash stay out, unless the player's order says whose copy to keep: the first one's)
        {
            var ranks = new Dictionary<string, int>(StringComparer.OrdinalIgnoreCase);
            for (var i = 0; i < order.Count; i++) ranks.TryAdd(order[i], i);
            int Rank(int i) => ranks.GetValueOrDefault(ok[i].Src.Mod.Rel, -1);
            bool Out(int i) => leaveOut?.Contains(ok[i].Src.Mod.Rel, StringComparer.OrdinalIgnoreCase) == true;
            var left = new HashSet<int>();
            // (against the first part's name: shortcut: a loaded package named between two parts of one merge is judged by the first part's place, upgrade if parts ever clash with one)
            if (ModConflicts.LoadKey(dirs, Slash(UnderMods(Path.Combine("Mods", Path.GetRelativePath(modsDir, Path.Combine(folderFull, name + ".package")))))) is { } tk)
                foreach (var (group, outside, count, keys, pairs, outsideOf) in Clashes(docs, modsDir, dirs, tk, ok, of?.Merged.Path))
                {
                    clashes.Add(new(group.Select(i => ok[i].Src.Mod).ToList(), outside.ToList(), count));
                    foreach (var i in group)
                    {
                        groupOf[ok[i].Src.Mod.Rel] = clashes.Count;
                        clashKeys[ok[i].Src.Mod.Rel] = keys[i];
                        meets[ok[i].Src.Mod.Rel] = new((pairs.GetValueOrDefault(i) ?? []).GroupBy(p => ok[p.Key].Src.Mod.Rel, p => p.Value, StringComparer.OrdinalIgnoreCase).ToDictionary(g => g.Key, g => g.Max(), StringComparer.OrdinalIgnoreCase), outsideOf.GetValueOrDefault(i)?.ToList() ?? [], keys[i].Count);
                        if (Rank(i) >= 0 && !Out(i)) continue;
                        left.Add(i);
                        var with = group.Where(x => x != i).Select(x => Path.GetFileNameWithoutExtension(ok[x].Src.Mod.Name)).Concat(outside).Take(3);
                        skipped.Add(new(ok[i].Src.Mod, Out(i) ? R(LeftOutReason) : R(ClashReason, string.Join(", ", with))));
                    }
                }
            for (var i = 0; i < ok.Count; i++)
                if (Out(i) && left.Add(i)) skipped.Add(new(ok[i].Src.Mod, R(LeftOutReason)));
            ok = ok.Select((o, i) => (o, i)).Where(x => !left.Contains(x.i)).OrderBy(x => Rank(x.i) is var r and >= 0 ? r : int.MaxValue).Select(x => x.o).ToList();
        }

        // in parts: the mods in their order, those of one clash together, a new part where the next ones would not fit
        var parts = new List<List<(MergeSource Src, (int, int, string) Key)>> { ok };
        if (ok.Sum(o => o.Src.Size) > partBytes)
        {
            parts.Clear();
            long size = 0;
            foreach (var unit in ok.GroupBy(o => groupOf.TryGetValue(o.Src.Mod.Rel, out var g) ? g.ToString() : o.Src.Mod.Rel, StringComparer.OrdinalIgnoreCase))
            {
                var bytes = unit.Sum(o => o.Src.Size);
                if (parts.Count == 0 || size + bytes > partBytes) { parts.Add(new()); size = 0; }
                parts[^1].AddRange(unit);
                size += bytes;
            }
            // (a mod alone in a part is no merge: it stays the plain mod it is)
            foreach (var alone in parts.Where(p => p.Count == 1).ToList())
            {
                skipped.Add(new(alone[0].Src.Mod, R("Too big to share a package. It stays as it is.")));
                parts.Remove(alone);
            }
            if (parts.Count == 0) parts.Add(new());
        }
        var plans = parts.Select((p, i) => Make(parts.Count == 1 ? name : $"{name} {i + 1}", p)).ToList();
        plans[0].Skipped.AddRange(skipped);
        plans[0].Clashes.AddRange(clashes);
        foreach (var (rel, m) in meets) plans[0].Meets[rel] = m;
        return plans;
    }

    // a package's index, checked against the file before anything trusts it (untrusted input), or why not
    static (List<ModConflicts.Entry>?, MergeReason?) Probe(string path)
    {
        try
        {
            long len;
            var head = new byte[96];
            using (var fs = File.OpenRead(path))
            {
                len = fs.Length;
                if (len < 96) return (null, R("Can't read this package: {0}", "too short"));
                fs.ReadExactly(head);
            }
            var magic = Encoding.Latin1.GetString(head, 0, 4);
            if (magic == "DBPP") return (null, R("A protected package: its contents can't be copied."));
            if (magic != "DBPF" || U32(head, 4) != 2) return (null, R("Can't read this package: {0}", "not DBPF 2"));
            if (len > MaxBytes) return (null, R("Can't read this package: {0}", "over 2 GB"));
            uint count = U32(head, 36), size = U32(head, 44), at = U32(head, 64);
            if (count > MaxEntries) return (null, R("Can't read this package: {0}", "index count too large"));
            if (at < 96 || (long)at + size > len || (count > 0 && size < 4 + 20L * count)) return (null, R("Can't read this package: {0}", "index outside the file"));
            var entries = ModConflicts.Package.Open(path).Entries;
            if (entries.Count != count) return (null, R("Can't read this package: {0}", "index count"));
            if (entries.Any(e => e.FileSize < 0 || (long)e.Offset + e.FileSize > len)) return (null, R("Can't read this package: {0}", "a resource points past the end"));
            return (entries, null);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or InvalidDataException or ArgumentException or IndexOutOfRangeException or OverflowException)
        {
            return (null, R("Can't read this package: {0}", e.Message));
        }
    }
    static uint U32(byte[] b, int p) => BinaryPrimitives.ReadUInt32LittleEndian(b.AsSpan(p));

    // ---------- run ----------
    public static MergeResult Run(MergePlan plan, IProgress<MergeProgress>? progress, CancellationToken ct)
    {
        if (!plan.CanRun) throw new SetupException("Can't merge: {0}", plan.Refusals[0].Text);
        if (plan.Of is not null) throw new InvalidOperationException("the plan of a change to a merge runs as that change");
        lock (Gate) return RunNow(plan, progress, ct);
    }

    static MergeResult RunNow(MergePlan plan, IProgress<MergeProgress>? progress, CancellationToken ct)
    {
        var sw = Stopwatch.StartNew();
        using var log = OpenLog("merge", out var logPath);
        plan.LogPath = logPath;
        log.WriteLine($"merge \"{plan.Name}\" -> {plan.TargetRel}: {plan.Inputs.Count} packages, {plan.Resources} resources, {plan.OutputBytes} bytes");
        foreach (var s in plan.Sources) log.WriteLine($"in   {s.Mod.Rel}: {s.Keep.Count} resources kept, {s.Size} bytes");
        foreach (var s in plan.Skipped) log.WriteLine($"skip {s.Mod.Rel}: {s.Reason.Text}");
        foreach (var d in plan.Duplicates) log.WriteLine($"key  {d.Key}: kept from {d.Kept}, dropped from {string.Join(", ", d.Dropped)}");
        foreach (var m in plan.Made) log.WriteLine($"name {m.E.Type:x8}:{m.E.Group:x8}:{m.E.Instance:x16}: the packages' name lists joined");
        var tmp = Path.Combine(Path.GetDirectoryName(plan.TargetPath)!, $"{plan.Name}.package.tmp-{Guid.NewGuid():N}");
        var moves = new List<(string From, string To)>();
        var json = Path.Combine(plan.StoreDir, "merge.json");
        var placed = false;
        try
        {
            // 1. the inputs are as planned; their hashes go into merge.json
            log.WriteLine("step check");
            Engine.CheckWritable();
            var total = plan.Sources.Sum(s => s.Size);
            long done = 0;
            var hashes = new List<string>();
            foreach (var s in plan.Sources)
            {
                Unchanged(s);
                hashes.Add(Sha(s.Mod.Path, ct));
                progress?.Report(new(MergeStep.Check, done += s.Size, total));
            }

            // 2. the new file, next to the target
            log.WriteLine("step write " + Path.GetFileName(tmp));
            ct.ThrowIfCancellationRequested();
            var (written, resHashes, sha) = Write(plan, tmp, progress, ct);

            // 3. read back: header, index, every resource's stored bytes
            log.WriteLine("step verify");
            ct.ThrowIfCancellationRequested();
            FailPoint("verify");
            Verify(tmp, written, resHashes, sha, progress, ct);
            log.WriteLine($"verified {written.Count} resources, sha256 {sha}");
            ct.ThrowIfCancellationRequested();
            Engine.CheckWritable();
            foreach (var s in plan.Sources) Unchanged(s);

            // 4. the originals into the merge folder (no cancel from here: a rollback instead)
            log.WriteLine("step move");
            var record = new MergeRecord
            {
                Version = 2, Priority = plan.Order.ToList(), Name = plan.Name, Merged = plan.TargetRel, Sha256 = sha, Size = new FileInfo(tmp).Length, Resources = written.Count, Created = DateTime.UtcNow,
                Originals = plan.Sources.Select((s, i) => new MergeOriginal { Rel = s.Mod.Rel, Size = s.Size, Sha256 = hashes[i], Note = ModNotes.Get(s.Mod.Rel) }).ToList(),
            };
            Directory.CreateDirectory(plan.StoreDir);
            record.Dir = plan.StoreDir;
            Save(record);
            foreach (var s in plan.Sources)
            {
                var to = Path.Combine(plan.StoreDir, UnderMods(s.Mod.Rel));
                Directory.CreateDirectory(Path.GetDirectoryName(to)!);
                Fs.Retry(() => File.Move(s.Mod.Path, to));
                moves.Add((s.Mod.Path, to));
                log.WriteLine($"moved {s.Mod.Rel}");
                progress?.Report(new(MergeStep.Move, moves.Count, plan.Sources.Count));
                FailPoint("move");
            }

            // 5. the merged file into place
            FailPoint("rename");
            Fs.Retry(() => File.Move(tmp, plan.TargetPath));
            placed = true;
            log.WriteLine("step finish: in place");
            progress?.Report(new(MergeStep.Finish, 1, 1));

            // the originals' notes now live in merge.json; the merged file carries their tags
            try
            {
                var tags = new List<string>();
                foreach (var s in plan.Sources)
                    if (ModNotes.Take(s.Mod.Rel)?.Tags is { } t) tags.AddRange(t);
                foreach (var t in tags.Distinct(StringComparer.OrdinalIgnoreCase)) ModNotes.SetTag(plan.TargetRel, t, true);
            }
            catch (Exception e) { log.WriteLine("notes not moved: " + e.Message); Engine.Log(e); } // (the merge is done whatever becomes of them)
            Forget(plan.Docs);
            var merged = new ModFile(plan.TargetPath, plan.TargetRel, record.Size, Mods.KindOf(plan.TargetPath), true, false);
            log.WriteLine($"done in {sw.ElapsedMilliseconds} ms");
            return new(merged, plan.StoreDir, plan.Sources.Count, written.Count, record.Size, sha, logPath, sw.Elapsed);
        }
        catch (Exception e) when (!placed)
        {
            log.WriteLine($"failed: {e.GetType().Name}: {e.Message}");
            var stuck = new List<string>();
            for (var i = moves.Count - 1; i >= 0; i--)
                try { var m = moves[i]; Fs.Retry(() => File.Move(m.To, m.From)); log.WriteLine("put back " + moves[i].From); }
                catch (Exception x) when (x is IOException or UnauthorizedAccessException) { stuck.Add(moves[i].To); log.WriteLine($"NOT put back {moves[i].To}: {x.Message}"); }
            try { File.Delete(tmp); } catch (Exception x) when (x is IOException or UnauthorizedAccessException) { log.WriteLine("temp file stays: " + x.Message); }
            if (stuck.Count == 0) DeleteStore(plan.StoreDir);
            Forget(plan.Docs);
            log.WriteLine(stuck.Count == 0 ? "everything as it was" : $"{stuck.Count} files stay in {plan.StoreDir}");
            if (stuck.Count > 0) throw new SetupException("The merge failed and {0} files could not be put back. They are in {1}.", stuck.Count.ToString(), plan.StoreDir);
            throw;
        }
    }

    static void Unchanged(MergeSource s)
    {
        var fi = new FileInfo(s.Mod.Path);
        if (!fi.Exists || fi.Length != s.Size || fi.LastWriteTimeUtc != s.Written)
            throw new SetupException("{0} changed since the merge was planned. Plan it again.", s.Mod.Rel);
    }

    // the header first (every offset is known from the plan), then each resource's stored bytes, then the index
    static (List<ModConflicts.Entry>, List<byte[]>, string) Write(MergePlan plan, string tmp, IProgress<MergeProgress>? progress, CancellationToken ct)
    {
        var written = new List<ModConflicts.Entry>();
        var resHashes = new List<byte[]>();
        var buf = new byte[1 << 16];
        long data = plan.Data, done = 0;
        var indexAt = 96 + data;
        var indexSize = 4 + 32 * plan.Resources;
        using var file = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        using (var o = new FileStream(tmp, FileMode.CreateNew, FileAccess.Write, FileShare.None, 1 << 20))
        {
            void Put(ReadOnlySpan<byte> b) { o.Write(b); file.AppendData(b); }
            var head = new byte[96];
            "DBPF"u8.CopyTo(head);
            BinaryPrimitives.WriteUInt32LittleEndian(head.AsSpan(4), 2);
            BinaryPrimitives.WriteUInt32LittleEndian(head.AsSpan(36), (uint)plan.Resources);
            BinaryPrimitives.WriteUInt32LittleEndian(head.AsSpan(44), (uint)indexSize);
            BinaryPrimitives.WriteUInt32LittleEndian(head.AsSpan(60), 3);
            BinaryPrimitives.WriteUInt32LittleEndian(head.AsSpan(64), (uint)indexAt);
            Put(head);
            foreach (var s in plan.Sources)
            {
                using var src = new FileStream(s.Mod.Path, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 16);
                foreach (var e in s.Keep)
                {
                    ct.ThrowIfCancellationRequested();
                    using var h = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
                    src.Position = e.Offset;
                    for (var left = e.FileSize; left > 0;)
                    {
                        var n = src.Read(buf, 0, Math.Min(buf.Length, left));
                        if (n == 0) throw new InvalidDataException($"{s.Mod.Rel} ended inside a resource");
                        Put(buf.AsSpan(0, n));
                        h.AppendData(buf, 0, n);
                        left -= n;
                    }
                    written.Add(e with { Offset = (uint)(96 + done) });
                    resHashes.Add(h.GetHashAndReset());
                    done += e.FileSize;
                    progress?.Report(new(MergeStep.Write, done, data));
                    if (written.Count == 1) FailPoint("write");
                }
            }
            foreach (var (e, bytes) in plan.Made)
            {
                Put(bytes);
                written.Add(e with { Offset = (uint)(96 + done) });
                resHashes.Add(SHA256.HashData(bytes));
                done += bytes.Length;
            }
            var idx = new byte[indexSize];
            for (var i = 0; i < written.Count; i++)
            {
                var e = written[i];
                var p = idx.AsSpan(4 + i * 32);
                BinaryPrimitives.WriteUInt32LittleEndian(p, e.Type);
                BinaryPrimitives.WriteUInt32LittleEndian(p[4..], e.Group);
                BinaryPrimitives.WriteUInt32LittleEndian(p[8..], (uint)(e.Instance >> 32));
                BinaryPrimitives.WriteUInt32LittleEndian(p[12..], (uint)e.Instance);
                BinaryPrimitives.WriteUInt32LittleEndian(p[16..], e.Offset);
                BinaryPrimitives.WriteUInt32LittleEndian(p[20..], (uint)e.FileSize | 0x80000000);
                BinaryPrimitives.WriteUInt32LittleEndian(p[24..], e.MemSize);
                BinaryPrimitives.WriteUInt16LittleEndian(p[28..], e.Compressed);
                BinaryPrimitives.WriteUInt16LittleEndian(p[30..], 1); // (committed: what the game writes)
            }
            Put(idx);
            o.Flush(true);
        }
        return (written, resHashes, Convert.ToHexString(file.GetHashAndReset()).ToLowerInvariant());
    }

    static void Verify(string tmp, List<ModConflicts.Entry> written, List<byte[]> resHashes, string sha, IProgress<MergeProgress>? progress, CancellationToken ct)
    {
        var (entries, why) = Probe(tmp);
        if (entries is null) throw new InvalidDataException("the merged file does not read back: " + why!.Text);
        if (!entries.SequenceEqual(written)) throw new InvalidDataException("the merged file's index differs from what was written");
        var buf = new byte[1 << 16];
        long total = written.Sum(e => (long)e.FileSize), done = 0;
        using (var f = new FileStream(tmp, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 16))
            for (var i = 0; i < entries.Count; i++)
            {
                ct.ThrowIfCancellationRequested();
                using var h = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
                f.Position = entries[i].Offset;
                for (var left = entries[i].FileSize; left > 0;)
                {
                    var n = f.Read(buf, 0, Math.Min(buf.Length, left));
                    if (n == 0) throw new InvalidDataException("the merged file ends early");
                    h.AppendData(buf, 0, n);
                    left -= n;
                }
                if (!h.GetHashAndReset().AsSpan().SequenceEqual(resHashes[i])) throw new InvalidDataException($"resource {i} differs from its source");
                progress?.Report(new(MergeStep.Verify, done += entries[i].FileSize, total));
            }
        if (Sha(tmp, ct) != sha) throw new InvalidDataException("the merged file's hash differs from what was written");
    }

    static string Sha(string path, CancellationToken ct = default)
    {
        ct.ThrowIfCancellationRequested();
        using var f = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 20);
        return Convert.ToHexString(SHA256.HashData(f)).ToLowerInvariant();
    }

    // A merge stopped during the move (the launcher closed or was ended): the merged file is not in place and its temporary
    // file is still next to the target. The originals go back and the record and the temporary file go, as a failed merge
    // leaves it. A merged file the player removed leaves no temporary file: its originals stay kept, as Remove says.
    public static void Recover(string docs)
    {
        try { Engine.CheckWritable(); } catch (InvalidOperationException) { return; } // (a dry run, the game running: next time)
        if (!Monitor.TryEnter(Gate)) return; // (a merge is being made: its files are its own)
        try { RecoverNow(docs); }
        finally { Monitor.Exit(Gate); }
    }

    static void RecoverNow(string docs)
    {
        MoveOut(docs);
        // (a change to a merge that did not finish is undone first: the merge as it was)
        foreach (var r in Records(docs).Where(r => r.Pending is not null).ToList())
            try { Undo(docs, r, null); }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException or SetupException) { Engine.Log(e); }
        try
        {
            var busy = Records(docs).Where(r => r.Pending is not null).Select(r => Path.GetFullPath(r.Dir)).ToHashSet(StringComparer.OrdinalIgnoreCase);
            foreach (var d in Stores(docs).Where(d => !busy.Contains(Path.GetFullPath(d))).ToList()) SetAside(docs, d);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { Engine.Log(e); }
        foreach (var r in Records(docs).Where(r => r.Pending is null).ToList())
            try
            {
                var dir = Path.GetDirectoryName(Path.Combine(docs, r.Merged))!;
                if (File.Exists(Path.Combine(docs, r.Merged)) || File.Exists(Path.Combine(Mods.Disabled(docs), UnderMods(r.Merged))) || !Directory.Exists(dir)) continue;
                var tmps = Directory.GetFiles(dir, r.Name + ".package.tmp-*");
                if (tmps.Length == 0) continue;
                var taken = false;
                foreach (var o in r.Originals)
                {
                    var from = Path.Combine(r.Dir, UnderMods(o.Rel));
                    var to = Path.Combine(docs, o.Rel);
                    if (!File.Exists(from)) continue;
                    if (File.Exists(to)) { taken = true; continue; }
                    Directory.CreateDirectory(Path.GetDirectoryName(to)!);
                    Fs.Retry(() => File.Move(from, to));
                }
                if (taken) continue;
                Fs.Retry(() => File.Delete(Path.Combine(r.Dir, "merge.json")));
                DeleteStore(r.Dir);
                foreach (var t in tmps) Fs.Retry(() => File.Delete(t));
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { Engine.Log(e); }
        // a write cut short before its record was made leaves a .package.tmp-* with nothing naming it: the game never loads
        // it, so it only takes space (one of a record still waiting stays; so does one younger than this launcher)
        try
        {
            var mods = Path.Combine(docs, "Mods");
            var started = System.Diagnostics.Process.GetCurrentProcess().StartTime;
            // (a merge still waiting for its file keeps its temporary files, and so does a change that could not be undone yet)
            var waiting = Records(docs).Where(r => !Present(docs, r)).SelectMany(r => new[] { Path.Combine(Path.GetDirectoryName(Path.Combine(docs, r.Merged))!, r.Name + ".package.tmp-"), Path.Combine(docs, r.Merged) + ".tmp-" })
                .Concat(Records(docs).Where(r => r.Pending is not null).SelectMany(r => r.Pending!.Moves.SelectMany(m => new[] { m.From, m.To }).Append(r.Pending.Tmp ?? "").Where(x => x.Length > 0).Select(x => Path.GetFullPath(Path.Combine(docs, x))))).ToList();
            if (Directory.Exists(mods))
                foreach (var t in Directory.EnumerateFiles(mods, "*.package.tmp-*", new EnumerationOptions { RecurseSubdirectories = true, AttributesToSkip = FileAttributes.ReparsePoint }).ToList())
                    if (File.GetLastWriteTime(t) < started && !waiting.Any(w => t.StartsWith(w, StringComparison.OrdinalIgnoreCase)))
                        Fs.Retry(() => File.Delete(t));
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { Engine.Log(e); }
        try { Relink(docs); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { Engine.Log(e); }
        Forget(docs);
    }

    // the merges an older launcher kept inside Mods go next to it, where no Resource.cfg loads them; one that can't (a file in
    // use, a Mods folder on another drive) stays and is still read from there
    static void MoveOut(string docs)
    {
        var old = OldFolder(docs);
        if (!Directory.Exists(old)) return;
        try
        {
            Directory.CreateDirectory(Folder(docs));
            foreach (var d in Directory.GetDirectories(old))
                try
                {
                    var to = Path.Combine(Folder(docs), Path.GetFileName(d));
                    if (!Directory.Exists(to)) Fs.Retry(() => Directory.Move(d, to));
                }
                catch (Exception e) when (e is IOException or UnauthorizedAccessException) { Engine.Log(e); }
            foreach (var d in new[] { old, Folder(docs) })
                if (!Directory.EnumerateFileSystemEntries(d).Any()) Fs.Retry(() => Directory.Delete(d));
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { Engine.Log(e); }
        Forget(docs);
    }

    // the merge folder once its originals are gone: merge.json and empty folders only
    static void DeleteStore(string dir)
    {
        try
        {
            if (!Directory.Exists(dir)) return;
            bool Others() => Kept(dir).Count > 0;
            // (a file just moved or deleted that an antivirus still holds stays listed a moment: up to 2 s for it to go)
            for (var i = 0; i < 40 && Others(); i++) Thread.Sleep(50);
            if (Others()) return;
            // (a recursive delete says nothing when a subfolder is still held: asked again, 3 times)
            for (var i = 0; i < 3 && Directory.Exists(dir); i++) { Fs.Retry(() => Directory.Delete(dir, true)); Fs.Gone(dir); }
            var parent = Path.GetDirectoryName(dir)!;
            if (!Directory.EnumerateFileSystemEntries(parent).Any()) { Fs.Retry(() => Directory.Delete(parent)); Fs.Gone(parent); }
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { Engine.Log(e); }
    }

    static StreamWriter OpenLog(string kind, out string path)
    {
        var dir = Path.Combine(Paths.Data, "logs");
        Directory.CreateDirectory(dir);
        path = Path.Combine(dir, $"{kind}-{DateTime.Now:yyyyMMdd-HHmmss-fff}.log");
        return new StreamWriter(path, append: true) { AutoFlush = true };
    }

    // ---------- what a merged file is made of ----------
    static readonly Dictionary<string, List<MergeRecord>> _records = new(StringComparer.OrdinalIgnoreCase);
    static void Forget(string docs) { lock (_records) _records.Remove(Path.GetFullPath(docs)); }
    static void Save(MergeRecord r) => Fs.WriteAtomic(Path.Combine(r.Dir, "merge.json"), JsonSerializer.Serialize(r, SetupJson.Default.MergeRecord));

    static List<MergeRecord> Records(string docs)
    {
        docs = Path.GetFullPath(docs);
        lock (_records)
        {
            if (_records.TryGetValue(docs, out var l)) return l;
            l = new();
            var read = true;
            foreach (var folder in new[] { Folder(docs), OldFolder(docs) }.Where(Directory.Exists))
                foreach (var d in Directory.EnumerateDirectories(folder))
                    try
                    {
                        var j = Path.Combine(d, "merge.json");
                        if (!File.Exists(j)) continue;
                        var r = JsonSerializer.Deserialize(File.ReadAllText(j), SetupJson.Default.MergeRecord);
                        if (r is null) continue;
                        r.Dir = d;
                        l.Add(r);
                    }
                    catch (Exception e) when (e is IOException or UnauthorizedAccessException or JsonException) { Engine.Log(e); read &= e is JsonException; }
            if (read) _records[docs] = l; // (one held a moment by another program is read again the next time)
            return l;
        }
    }

    // Documents\...\The Sims 3 from a mod's path and Rel (on or off)
    public static string? DocsOf(ModFile m)
    {
        var p = Path.GetFullPath(m.Path);
        if (p.EndsWith(Path.DirectorySeparatorChar + m.Rel, StringComparison.OrdinalIgnoreCase)) return p[..^(m.Rel.Length + 1)];
        var off = Path.DirectorySeparatorChar + Path.Combine("Mods", Mods.DisabledName, UnderMods(m.Rel));
        return p.EndsWith(off, StringComparison.OrdinalIgnoreCase) ? p[..^off.Length] : null;
    }

    public static MergeRecord? Info(string docs, ModFile m) => Records(docs).FirstOrDefault(r => r.Merged.Equals(m.Rel, StringComparison.OrdinalIgnoreCase));
    public static MergeRecord? Info(ModFile m) => DocsOf(m) is { } d ? Info(d, m) : null;
    public static bool IsMerged(ModFile m) => Info(m) is not null;

    // the merged file moved or was renamed (a folder: every merged file under it); merge.json follows
    public static void Moved(string docs, string oldRel, string newRel, bool folder = false)
    {
        foreach (var r in Records(docs))
        {
            if (folder ? !r.Merged.StartsWith(oldRel + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase) : !r.Merged.Equals(oldRel, StringComparison.OrdinalIgnoreCase)) continue;
            r.Merged = newRel + r.Merged[oldRel.Length..];
            Fs.WriteAtomic(Path.Combine(r.Dir, "merge.json"), JsonSerializer.Serialize(r, SetupJson.Default.MergeRecord));
        }
    }

    // ---------- split ----------
    // The originals back where they were (switched off when the merged file is), checked by hash; the merged file to the
    // Recycle Bin; the notes back. The restored mods.
    public static List<ModFile> Split(string docs, ModFile merged)
    {
        if (merged.Locked) throw new SetupException("The launcher does not move, rename or remove this file.");
        Engine.CheckWritable();
        lock (Gate) return SplitNow(docs, merged, Info(docs, merged) ?? throw new SetupException("This file is not a merge made by the launcher."));
    }

    static List<ModFile> SplitNow(string docs, ModFile merged, MergeRecord rec)
    {
        using var log = OpenLog("split", out _);
        log.WriteLine($"split \"{rec.Name}\" {merged.Rel}: {rec.Originals.Count} originals");
        if (Sha(merged.Path) != rec.Sha256) log.WriteLine("the merged file changed since the merge (it goes to the Recycle Bin)");
        var home = merged.Enabled ? Path.Combine(docs, "Mods") : Mods.Disabled(docs);
        var todo = new List<(MergeOriginal O, string From, string To, bool Same)>();
        var taken = new List<string>();
        foreach (var o in rec.Originals)
        {
            var from = Path.Combine(rec.Dir, UnderMods(o.Rel));
            var to = Path.Combine(home, UnderMods(o.Rel));
            var same = File.Exists(to) && Sha(to) == o.Sha256;
            // (gone from the store but back in its place, by hash: a split that stopped halfway put it back)
            if (!(same && !File.Exists(from)) && (!File.Exists(from) || new FileInfo(from).Length != o.Size || Sha(from) != o.Sha256))
                throw new SetupException("An original of this merge is missing or changed: {0}", o.Rel);
            if (File.Exists(to) && !same) taken.Add(o.Rel);
            todo.Add((o, from, to, same));
        }
        if (taken.Count > 0) throw new SetupException("A different file is now where an original should be: {0}", string.Join(", ", taken));

        var moved = new List<(string From, string To)>();
        try
        {
            foreach (var t in todo.Where(t => !t.Same))
            {
                Directory.CreateDirectory(Path.GetDirectoryName(t.To)!);
                Fs.Retry(() => File.Move(t.From, t.To));
                moved.Add((t.From, t.To));
                log.WriteLine("restored " + t.O.Rel);
                FailPoint("split");
            }
            Recycle(merged.Path);
            log.WriteLine("merged file to the Recycle Bin");
        }
        catch (Exception e)
        {
            log.WriteLine($"failed: {e.Message}");
            for (var i = moved.Count - 1; i >= 0; i--)
                try { var m = moved[i]; Fs.Retry(() => File.Move(m.To, m.From)); }
                catch (Exception x) when (x is IOException or UnauthorizedAccessException) { log.WriteLine($"NOT put back {moved[i].To}: {x.Message}"); }
            throw;
        }
        foreach (var t in todo.Where(t => t.Same))
        {
            if (File.Exists(t.From)) Fs.Retry(() => File.Delete(t.From)); // (an identical copy is already in place)
            log.WriteLine("already in place " + t.O.Rel);
        }
        Fs.Retry(() => File.Delete(Path.Combine(rec.Dir, "merge.json")));
        DeleteStore(rec.Dir);
        Forget(docs);
        ModNotes.Forget(merged.Rel);
        foreach (var o in rec.Originals.Where(o => o.Note is not null && ModNotes.Get(o.Rel) is null)) ModNotes.Put(o.Rel, o.Note!);
        return todo.Select(t => new ModFile(t.To, t.O.Rel, t.O.Size, Mods.KindOf(t.To), merged.Enabled, false)).ToList();
    }
}
