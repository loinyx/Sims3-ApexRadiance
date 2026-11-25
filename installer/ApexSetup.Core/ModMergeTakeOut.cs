using System.Diagnostics;

namespace ApexSetup;

// what becomes of a mod taken out of a merge: back at its place and on, back and switched off, or to the Recycle Bin
public enum TakeOutTo { On, Off, Bin }

// A change to a merge, made in one go. The Rels are those of the merge's originals (MergeOriginal.Rel).
public sealed class MergeEdit
{
    public string? Name { get; set; }                   // a new name for the merged package; null: as it is
    public Dictionary<string, TakeOutTo> Out { get; } = new(StringComparer.OrdinalIgnoreCase);
    public List<ModFile> Add { get; } = new();          // mods of the list that go in
    // an original's file replaced by another one: a mod of the list, or a file from outside Mods (ModMerge.Outside), which
    // is copied. The replaced file goes to the Recycle Bin; its place in the order, notes, creator and tags stay.
    public Dictionary<string, ModFile> Replace { get; } = new(StringComparer.OrdinalIgnoreCase);
    // the mods from the first (its copy counts) to the last, as Rels: an original's for itself or for the file that replaces
    // it, an added mod's own. Null: as it is, the added ones last. An added mod that clashes goes in only when listed.
    public IReadOnlyList<string>? Order { get; set; }
}

// A change checked and ready to run. Plan: the merge after it (what goes in, in which order, the clashes, what can't go
// in), or null when fewer than two mods would stay: then every mod goes back and the merge is removed.
public sealed class MergeEditPlan
{
    public required string Docs { get; init; }
    public required ModFile Merged { get; init; }
    public required MergeRecord Record { get; init; }
    public MergePlan? Plan { get; internal set; }
    public List<MergeReason> Refusals { get; } = new(); // besides the plan's own
    public bool CanRun => Refusals.Count == 0 && (Plan is null || Plan.CanRun && Plan.Skipped.Count == 0);
    // why not, in English (the screens translate Refusals, Plan.Refusals and Plan.Skipped themselves)
    public string Why => string.Join(" ", Refusals.Concat(Plan?.Refusals ?? []).Select(r => r.Text)
        .Concat(Plan?.Skipped.Select(s => string.Format("{0} can't go in. {1}", s.Mod.Name, s.Reason.Text)) ?? []));
    internal List<(MergeOriginal O, TakeOutTo To)> Outs { get; } = new();
    internal Dictionary<string, MergeOriginal> Was { get; } = new(StringComparer.OrdinalIgnoreCase); // by the new file's Rel: the original it replaces
    internal bool Ordered { get; init; }                // the player gave the order
}

public static partial class ModMerge
{
    // Back: the mods now in the list again. Left: always empty (a mod that can't go in refuses the change).
    public sealed record TakeOutResult(List<ModFile> Back, MergeResult? Remerged, IReadOnlyList<MergeSkip> Left);

    // a file from outside Mods, to replace an original with (MergeEdit.Replace) or to look for a newer version in (Updates)
    public static ModFile Outside(string path) => new(Path.GetFullPath(path), Path.GetFileName(path), new FileInfo(path).Length, ModKind.Package, true, false);

    // where a merge keeps an original; never outside the merge's folder, whatever the record says
    static string StorePath(MergeRecord rec, MergeOriginal o)
    {
        var p = Path.GetFullPath(Path.Combine(rec.Dir, UnderMods(o.Rel)));
        if (!p.StartsWith(Path.GetFullPath(rec.Dir) + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase)) throw new SetupException("An original of this merge is missing or changed: {0}", o.Rel);
        return p;
    }

    // Some mods taken out of a merge, each back at its place (on, or off when the merge is off).
    public static TakeOutResult TakeOut(string docs, ModFile merged, IReadOnlyCollection<string> outRels, IProgress<MergeProgress>? progress = null, CancellationToken ct = default)
        => Edit(docs, merged, outRels, [], progress, ct);

    public static TakeOutResult Edit(string docs, ModFile merged, IReadOnlyCollection<string> outRels, IReadOnlyList<ModFile> add, IProgress<MergeProgress>? progress = null, CancellationToken ct = default)
    {
        var edit = new MergeEdit();
        foreach (var r in outRels) edit.Out[r] = merged.Enabled ? TakeOutTo.On : TakeOutTo.Off;
        edit.Add.AddRange(add);
        return Edit(docs, merged, edit, progress, ct);
    }

    // The change planned and run. Refused before anything moves, saying why, when it can't be made whole.
    public static TakeOutResult Edit(string docs, ModFile merged, MergeEdit edit, IProgress<MergeProgress>? progress = null, CancellationToken ct = default)
        => Run(PlanEdit(docs, merged, edit), progress, ct);

    // ---------- the plan of a change ----------
    // Nothing of the player's is moved here. The mods that stay, the files that replace some and the added ones are planned
    // as a merge is (the kept originals read where the merge keeps them), under the merge's name and place, on or off.
    public static MergeEditPlan PlanEdit(string docs, ModFile merged, MergeEdit edit)
    {
        if (merged.Locked) throw new SetupException("The launcher does not move, rename or remove this file.");
        var rec = Info(docs, merged) ?? throw new SetupException("This file is not a merge made by the launcher.");
        if (rec.Pending is not null) lock (Gate) Undo(docs, rec, null); // (an earlier change that did not finish)
        var p = new MergeEditPlan { Docs = docs, Merged = merged, Record = rec, Ordered = edit.Order is not null };
        var modsDir = Path.GetFullPath(Path.Combine(docs, "Mods")) + Path.DirectorySeparatorChar;
        var held = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        var mods = new List<ModFile>();
        var now = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase); // (an original's Rel: the Rel that stands in its place)
        foreach (var o in rec.Originals)
        {
            if (edit.Out.TryGetValue(o.Rel, out var to)) { p.Outs.Add((o, to)); continue; }
            var stays = new ModFile(StorePath(rec, o), o.Rel, o.Size, ModKind.Package, true, false);
            if (edit.Replace.TryGetValue(o.Rel, out var with))
            {
                if (!with.Rel.StartsWith("Mods" + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase))
                {
                    // from outside Mods: a copy goes in, under the file's own name, where the replaced one was
                    var full = Path.GetFullPath(with.Path);
                    var name = Path.GetFileName(full);
                    var fine = name.EndsWith(".package", StringComparison.OrdinalIgnoreCase) && File.Exists(full) && !full.StartsWith(modsDir, StringComparison.OrdinalIgnoreCase) && !Keeps(docs, full);
                    try { ModLibrary.CleanName(name); } catch (SetupException) { fine = false; }
                    if (!fine) { p.Refusals.Add(R("{0} can't replace a mod of this merge.", name)); mods.Add(stays); held.Add(stays.Path); continue; }
                    with = new ModFile(full, Path.Combine(Path.GetDirectoryName(o.Rel)!, name), with.Size, ModKind.Package, true, false);
                    held.Add(full);
                }
                p.Was[with.Rel] = o;
                now[o.Rel] = with.Rel;
                mods.Add(with);
                continue;
            }
            held.Add(stays.Path);
            mods.Add(stays);
        }
        mods.AddRange(edit.Add);
        foreach (var twice in mods.GroupBy(m => m.Rel, StringComparer.OrdinalIgnoreCase).Where(g => g.Count() > 1))
            p.Refusals.Add(R("A mod named {0} is already in this merge.", Path.GetFileName(twice.Key)));
        if (mods.Any(m => m.Rel.Equals(merged.Rel, StringComparison.OrdinalIgnoreCase))) p.Refusals.Add(R("A merge can't go into itself."));

        if (mods.Count < 2)
        {
            // (a merge needs two mods: the ones that would stay go back as the merge is, on or off)
            foreach (var o in rec.Originals.Where(o => !edit.Out.ContainsKey(o.Rel))) p.Outs.Add((o, merged.Enabled ? TakeOutTo.On : TakeOutTo.Off));
            p.Was.Clear();
        }
        else
        {
            var order = (edit.Order ?? []).Concat(rec.Originals.Select(o => o.Rel)).Select(r => now.GetValueOrDefault(r, r)).Distinct(StringComparer.OrdinalIgnoreCase).ToList();
            var folder = Path.GetRelativePath("Mods", Path.GetDirectoryName(merged.Rel) ?? "Mods") is var f && f == "." ? "" : f;
            p.Plan = Plans(docs, mods, edit.Name ?? Path.GetFileNameWithoutExtension(merged.Name), folder, includeScripts: true, order, null, long.MaxValue, new EditOf(rec, merged, held))[0];
        }
        foreach (var (o, to) in p.Outs.Where(x => x.To != TakeOutTo.Bin))
            if (File.Exists(Path.Combine(docs, o.Rel)) || File.Exists(Path.Combine(Mods.Disabled(docs), UnderMods(o.Rel))))
                p.Refusals.Add(R("{0} is already in Mods. Move or rename that file first.", Path.GetFileName(o.Rel)));
        return p;
    }

    // ---------- the change ----------
    public static TakeOutResult Run(MergeEditPlan plan, IProgress<MergeProgress>? progress = null, CancellationToken ct = default)
    {
        // (the first reason, as its own text: the screens translate it)
        if (plan.Refusals.Concat(plan.Plan?.Refusals ?? []).FirstOrDefault() is { } no) throw new SetupException(no.Key, no.Args);
        if (plan.Plan?.Skipped.FirstOrDefault() is { } skip) throw new SetupException("{0} can't go in. {1}", skip.Mod.Name, skip.Reason.Text);
        Engine.CheckWritable();
        lock (Gate) return EditNow(plan, Paced.Of(progress), ct);
    }

    // Every original is checked against the record, the new merged file is written beside the old one and read back, and
    // only then do files move: the mods taken out to their places, the new ones in, the old merged file aside and the new
    // one in its place. The moves are written in merge.json first and each can be undone, so a failure (or a launcher that
    // ends) at any of them leaves the merge exactly as it was. The new record is the last write: from there the change is
    // made, and what is left (the Recycle Bin, the notes) can no longer undo it.
    static TakeOutResult EditNow(MergeEditPlan p, IProgress<MergeProgress>? progress, CancellationToken ct)
    {
        var sw = Stopwatch.StartNew();
        var (docs, merged, plan) = (p.Docs, p.Merged, p.Plan);
        using var log = OpenLog("edit", out var logPath);
        if (plan is not null) plan.LogPath = logPath;
        Forget(docs);
        var rec = Info(docs, merged);
        if (rec is null || rec.Sha256 != p.Record.Sha256 || rec.Pending is not null || !File.Exists(merged.Path)
            || !rec.Originals.Select(o => o.Rel).SequenceEqual(p.Record.Originals.Select(o => o.Rel), StringComparer.OrdinalIgnoreCase))
            throw new SetupException("This merge changed since the change was planned. Plan it again.");
        log.WriteLine($"edit \"{rec.Name}\" {merged.Rel}: {rec.Originals.Count} originals, {p.Outs.Count} out, {plan?.Sources.Count ?? 0} in the new merge");
        // (the files that come in: not the merge's own originals, which stay where they are kept)
        // (by its kept file: a mod of the list can come in under the name of one that goes out)
        bool Stays(MergeSource s) => !p.Was.ContainsKey(s.Mod.Rel) && rec.Originals.Any(o => o.Rel.Equals(s.Mod.Rel, StringComparison.OrdinalIgnoreCase) && StorePath(rec, o).Equals(s.Mod.Path, StringComparison.OrdinalIgnoreCase));
        bool Copied(MergeSource s) => plan!.Of!.Held.Contains(s.Mod.Path); // (from outside Mods)
        string Store(MergeSource s) => Path.Combine(rec.Dir, UnderMods(s.Mod.Rel));
        var fresh = plan?.Sources.Where(s => !Stays(s)).ToList() ?? [];
        var moves = new List<(string From, string To, bool Copy)>();
        var hashes = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        string? tmp = null;
        var aside = merged.Path + ".tmp-" + Guid.NewGuid().ToString("N");
        var outDir = Path.Combine(rec.Dir, OutPrefix + Guid.NewGuid().ToString("N")[..8]);
        bool journal = false, done = false;
        MergeRecord? record = null;
        try
        {
            // 1. every original as the record says, the places of the ones that go back free, the new files as planned
            log.WriteLine("step check");
            long total = rec.Originals.Sum(o => o.Size) + fresh.Sum(s => s.Size), read = 0;
            foreach (var o in rec.Originals)
            {
                var f = StorePath(rec, o);
                if (!File.Exists(f) || new FileInfo(f).Length != o.Size || Sha(f, ct) != o.Sha256) throw new SetupException("An original of this merge is missing or changed: {0}", o.Rel);
                progress?.Report(new(MergeStep.Check, read += o.Size, total));
            }
            foreach (var (o, to) in p.Outs.Where(x => x.To != TakeOutTo.Bin))
                if (File.Exists(Path.Combine(docs, o.Rel)) || File.Exists(Path.Combine(Mods.Disabled(docs), UnderMods(o.Rel))))
                    throw new SetupException("{0} is already in Mods. Move or rename that file first.", Path.GetFileName(o.Rel));
            foreach (var s in plan?.Sources ?? []) Unchanged(s);
            foreach (var s in fresh)
            {
                hashes[s.Mod.Rel] = Sha(s.Mod.Path, ct);
                progress?.Report(new(MergeStep.Check, read += s.Size, total));
            }

            // 2. and 3. the new merged file beside the old one, read back
            List<ModConflicts.Entry> written = [];
            var sha = "";
            if (plan is not null)
            {
                tmp = Path.Combine(Path.GetDirectoryName(plan.TargetPath)!, $"{plan.Name}.package.tmp-{Guid.NewGuid():N}");
                log.WriteLine("step write " + Path.GetFileName(tmp));
                ct.ThrowIfCancellationRequested();
                log.Flush();
                (written, sha, _) = Write(plan, tmp, progress, ct);
                log.WriteLine("step verify");
                log.Flush();
                ct.ThrowIfCancellationRequested();
                FailPoint("verify");
                Verify(tmp, written, sha, progress, ct);
                log.WriteLine($"verified {written.Count} resources, sha256 {sha}");
                ct.ThrowIfCancellationRequested();
                Engine.CheckWritable();
                foreach (var s in plan.Sources) Unchanged(s);
            }

            // 4. the moves (no cancel from here: a rollback instead), written down before the first one
            log.WriteLine("step move");
            log.Flush();
            foreach (var (o, to) in p.Outs)
                moves.Add((StorePath(rec, o), to switch
                {
                    TakeOutTo.On => Path.Combine(docs, o.Rel),
                    TakeOutTo.Off => Path.Combine(Mods.Disabled(docs), UnderMods(o.Rel)),
                    _ => Path.Combine(outDir, UnderMods(o.Rel)), // (set aside in the merge's folder until the change is made: the Recycle Bin can't be undone)
                }, false));
            foreach (var o in p.Was.Values) moves.Add((StorePath(rec, o), Path.Combine(outDir, UnderMods(o.Rel)), false));
            foreach (var s in fresh) moves.Add((s.Mod.Path, Store(s), Copied(s)));
            moves.Add((merged.Path, aside, false));
            if (plan is not null) moves.Add((tmp!, plan.TargetPath, false));
            string Rel(string full) => Path.GetRelativePath(docs, full);
            rec.Pending = new MergePending { Moves = moves.Select(m => new MergeMove(Rel(m.From), Rel(m.To), m.Copy)).ToList(), Tmp = tmp is null ? null : Rel(tmp) };
            Save(rec);
            journal = true;
            FailPoint("journal");
            var n = 0;
            foreach (var m in moves)
            {
                Directory.CreateDirectory(Path.GetDirectoryName(m.To)!);
                if (m.Copy) Fs.Retry(() => File.Copy(m.From, m.To));
                else Fs.Retry(() => File.Move(m.From, m.To));
                log.WriteLine($"{(m.Copy ? "copied" : "moved")} {Rel(m.From)} -> {Rel(m.To)}");
                progress?.Report(new(MergeStep.Move, ++n, moves.Count));
                FailPoint("edit");
            }
            // (a file from outside Mods is not held while it is copied: the copy is the file that was merged, or nothing is)
            foreach (var s in fresh.Where(Copied))
                if (Sha(Store(s)) != hashes[s.Mod.Rel]) throw new SetupException("{0} changed since the merge was planned. Plan it again.", s.Mod.Name);

            // 5. the record
            FailPoint("commit");
            if (plan is null) Fs.Retry(() => File.Delete(Path.Combine(rec.Dir, "merge.json")));
            else
            {
                record = new MergeRecord
                {
                    // (a merge of an older launcher stays one, still checked for lost parts, until the player orders its mods)
                    Version = p.Ordered ? 2 : rec.Version, Priority = rec.Priority, Name = plan.Name, Merged = plan.TargetRel, Sha256 = sha, Size = new FileInfo(plan.TargetPath).Length, Resources = written.Count, Created = DateTime.UtcNow, Dir = rec.Dir,
                    Originals = plan.Sources.Select(s => Stays(s) ? rec.Originals.First(o => o.Rel.Equals(s.Mod.Rel, StringComparison.OrdinalIgnoreCase)) : new MergeOriginal
                    {
                        Rel = s.Mod.Rel, Size = s.Size, Sha256 = hashes[s.Mod.Rel],
                        // (a new file's own notes, else those of the one it replaces)
                        Note = (Copied(s) ? null : ModNotes.Get(s.Mod.Rel)) ?? p.Was.GetValueOrDefault(s.Mod.Rel)?.Note,
                    }).ToList(),
                };
                Save(record);
                Inner(docs, record.Originals.Where(o => !rec.Merged.Equals(o.Rel, StringComparison.OrdinalIgnoreCase)));
            }
            done = true;
            log.WriteLine("step finish: the record written");
            progress?.Report(new(MergeStep.Finish, 1, 1));
        }
        catch (Exception e) when (!done)
        {
            log.WriteLine($"failed: {e.GetType().Name}: {e.Message}");
            try
            {
                if (journal) Undo(docs, rec, log);
                else if (tmp is not null) try { File.Delete(tmp); } catch (Exception x) when (x is IOException or UnauthorizedAccessException) { log.WriteLine("temp file stays: " + x.Message); }
            }
            finally { Forget(docs); }
            log.WriteLine("everything as it was");
            throw;
        }

        // the change is made. What follows can fail without undoing it: a file the Recycle Bin can't take stays in the list.
        Forget(docs);
        try { Recycle(aside); log.WriteLine("old merged file to the Recycle Bin"); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or SetupException) { log.WriteLine("old merged file stays as " + Path.GetFileName(aside) + ": " + e.Message); }
        if (Directory.Exists(outDir))
            foreach (var f in Directory.GetFiles(outDir, "*", SearchOption.AllDirectories))
                try { Recycle(f); log.WriteLine("to the Recycle Bin: " + Path.GetRelativePath(outDir, f)); }
                catch (Exception e) when (e is IOException or UnauthorizedAccessException or SetupException) { log.WriteLine($"not to the Recycle Bin: {f}: {e.Message}"); }
        var back = p.Outs.Where(x => x.To != TakeOutTo.Bin).Select(x => (x.O, On: x.To == TakeOutTo.On, Path: x.To == TakeOutTo.On ? Path.Combine(docs, x.O.Rel) : Path.Combine(Mods.Disabled(docs), UnderMods(x.O.Rel))))
            .Select(x => new ModFile(x.Path, x.O.Rel, x.O.Size, Mods.KindOf(x.Path), x.On, false)).ToList();
        try { back.AddRange(SetAside(docs, rec.Dir)); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { log.WriteLine("set aside: " + e.Message); Engine.Log(e); }
        try
        {
            foreach (var (o, to) in p.Outs.Where(x => x.To != TakeOutTo.Bin))
                if (o.Note is not null && ModNotes.Get(o.Rel) is null) ModNotes.Put(o.Rel, o.Note);
            if (plan is null) ModNotes.Forget(merged.Rel);
            else
            {
                // the merged file's notes, picture and tags follow a new name; the added mods' notes now live in merge.json
                if (!plan.TargetRel.Equals(merged.Rel, StringComparison.Ordinal)) ModNotes.Move(merged.Rel, plan.TargetRel);
                foreach (var s in fresh.Where(s => !Copied(s)))
                    foreach (var t in ModNotes.Take(s.Mod.Rel)?.Tags ?? []) ModNotes.SetTag(plan.TargetRel, t, true);
            }
        }
        catch (Exception e) { log.WriteLine("notes not moved: " + e.Message); Engine.Log(e); } // (the change is made whatever becomes of them)
        if (plan is null) DeleteStore(rec.Dir);
        Forget(docs);
        log.WriteLine($"done in {sw.ElapsedMilliseconds} ms");
        if (plan is null) return new(back, null, []);
        var file = new ModFile(plan.TargetPath, plan.TargetRel, record!.Size, Mods.KindOf(plan.TargetPath), merged.Enabled, false);
        return new(back, new(file, rec.Dir, plan.Sources.Count, record.Resources, record.Size, record.Sha256, logPath, sw.Elapsed), []);
    }

    // The moves of a change that did not finish, undone from the last to the first: each file back where it was, a copy the
    // change made removed, the new merged file deleted. Only inside Mods and the merges' folder, whatever the record says.
    // It can run again on the same record (a start that ended halfway): what is already back is left alone.
    static void Undo(string docs, MergeRecord rec, TextWriter? log)
    {
        string Full(string rel) => Path.GetFullPath(Path.Combine(docs, rel));
        bool Ours(string p) => new[] { Path.Combine(docs, "Mods"), Folder(docs) }.Any(d => p.StartsWith(Path.GetFullPath(d) + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase));
        var pending = rec.Pending!;
        var stuck = 0;
        for (var i = pending.Moves.Count - 1; i >= 0; i--)
        {
            var (from, to) = (Full(pending.Moves[i].From), Full(pending.Moves[i].To));
            try
            {
                if (!Ours(to) || !File.Exists(to)) continue;
                // (a file an earlier move was to take from this place and did not, or that is already back: not this move's)
                if (pending.Moves.Take(i).Any(m => Full(m.From).Equals(to, StringComparison.OrdinalIgnoreCase) && !File.Exists(Full(m.To)))) continue;
                if (pending.Moves[i].Copy) { if (Keeps(docs, to)) Fs.Retry(() => File.Delete(to)); }
                else if (Ours(from) && !File.Exists(from))
                {
                    Directory.CreateDirectory(Path.GetDirectoryName(from)!);
                    Fs.Retry(() => File.Move(to, from));
                }
                else continue;
                log?.WriteLine("put back " + pending.Moves[i].From);
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { stuck++; log?.WriteLine($"NOT put back {to}: {e.Message}"); Engine.Log(e); }
        }
        // (the record keeps the moves while one is stuck: the next start tries again)
        if (stuck > 0) throw new SetupException("The change did not finish and {0} files are out of place. Open Mods again to put them back.", stuck.ToString());
        if (pending.Tmp is { } t && Full(t) is var tmp && Ours(tmp) && Path.GetFileName(tmp).Contains(".package.tmp-"))
            try { File.Delete(tmp); } catch (Exception e) when (e is IOException or UnauthorizedAccessException) { log?.WriteLine("temp file stays: " + e.Message); }
        rec.Pending = null;
        Save(rec);
        foreach (var d in Directory.GetDirectories(rec.Dir, OutPrefix + "*"))
            try { if (!Directory.EnumerateFiles(d, "*", SearchOption.AllDirectories).Any()) Directory.Delete(d, true); } catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
    }

    const string OutPrefix = ".out-";

    // What a change set aside for the Recycle Bin and Windows could not take there: back in the list, switched off, under a
    // free name, so nothing stays kept where no screen shows it. The mods put there.
    static List<ModFile> SetAside(string docs, string store)
    {
        var list = new List<ModFile>();
        if (!Directory.Exists(store)) return list;
        foreach (var d in Directory.GetDirectories(store, OutPrefix + "*"))
        {
            foreach (var f in Kept(d))
            {
                var under = Path.GetRelativePath(d, f);
                // (not under the name of a mod the merge keeps, the file that replaced it: that one could not go back at a split)
                var to = Mods.Free(Path.Combine(Mods.Disabled(docs), under), Path.Combine(docs, "Mods", Path.GetDirectoryName(under) ?? ""), Path.Combine(store, Path.GetDirectoryName(under) ?? ""));
                Directory.CreateDirectory(Path.GetDirectoryName(to)!);
                Fs.Retry(() => File.Move(f, to));
                list.Add(new ModFile(to, Path.Combine("Mods", Path.GetRelativePath(Mods.Disabled(docs), to)), new FileInfo(to).Length, ModKind.Package, false, false));
            }
            // (empty now; one Windows will not let go of goes at the next start, and the mods put back are still told)
            try { Fs.DeleteClear(d); } catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
        }
        return list;
    }
}
