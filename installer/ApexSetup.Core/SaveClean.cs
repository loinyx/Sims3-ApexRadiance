using System.Buffers.Binary;

namespace ApexSetup;

// What a clean can take out of a save. Each is a set of pictures the game works without:
// Portraits: the Sims' faces, made again as you play. Thumbnails: lot pictures and other households' pictures, made
// again too (the save's own household picture always stays). RepeatedMemories: every copy of a memory's picture but
// one. Memories: that last one of each. Generated: selfies, cards and photo booth pictures. NestedCopies: not pictures,
// the save's own folder again inside it (X.sims3\X.sims3\...), which the game never reads: it reads only the top level.
// Never taken: a picture the player chose to keep, the textures of placed lots, anything that is the game's own data.
[Flags]
public enum CleanParts { None = 0, Portraits = 1, Thumbnails = 2, RepeatedMemories = 4, Memories = 8, Generated = 16, NestedCopies = 32 }

public sealed record CleanGain(int Count, long Bytes);
public enum CleanStep { Copy, Write, Verify, Swap }
// Done / Total: files, the save's (Copy) or the ones rewritten
public readonly record struct CleanProgress(CleanStep Step, int Done, int Total);
// Copy: the zip made before anything changed, which Undo puts back
// Played: the save's time once cleaned (a later one: it was played since)
public sealed record CleanResult(Save Save, long Before, long After, int Removed, string Copy, DateTime Played = default);

public sealed class CleanPlan
{
    public required string Docs { get; init; }
    public required Save Save { get; init; }
    public CleanParts Parts { get; init; }
    public long Before { get; internal set; }       // the save's size now
    public long After { get; internal set; }        // and once cleaned, to the byte
    public long DeadBytes { get; internal set; }    // room in the files nothing uses: always given back
    public int Removed { get; internal set; }       // pictures that go
    // what each part would free by itself, chosen or not (DeadBytes comes on top, once)
    public Dictionary<CleanParts, CleanGain> Gains { get; } = new();
    public MergeReason? Refusal { get; internal set; }
    public bool CanRun => Refusal is null && After < Before;
    internal List<(SavePackage File, bool[] Keep, long Size)> Files { get; } = new();
    internal List<string> Nested { get; } = new(); // the copies that go (NestedCopies chosen)
}

// Makes a save smaller by taking out pictures and unused room. The plan says, to the byte, what each choice frees. Run
// first makes a zip of the whole save and checks it file by file; then each world file and TravelDB.package that
// changes is written anew beside the original: the header as it was, every kept resource's stored bytes untouched
// (compressed stays compressed) in the order they lie in the file, every index field as it was but the offsets. The new
// file is read back and compared with the original resource by resource, and only then do the two change places, the
// originals kept until every file is in. Any failure puts everything back. Undo restores the zip.
// ExportDB packages are left alone: they hold only game data (no picture a clean removes) and their unused room is small.
public static class SaveClean
{
    internal const string NewSuffix = ".apex-new", OldSuffix = ".apex-old";

    // test hook: called at "copy", "write" and "verify" (after each file), "swap" (after each file) and "finish"
    public static Action<string> FailPoint = _ => { };

    static readonly CleanParts[] All = [CleanParts.Portraits, CleanParts.Thumbnails, CleanParts.RepeatedMemories, CleanParts.Memories, CleanParts.Generated];

    // memories: keys ("type:group:instance", SaveItem.Key) of memories to take out whatever the parts say; a starred
    // one stays all the same. contents: the save as already read (its memory groups are kept), else it is read here.
    public static CleanPlan Plan(string docs, Save s, CleanParts parts, IReadOnlyCollection<string>? memories = null, SaveContents? contents = null, CancellationToken ct = default)
    {
        var plan = new CleanPlan { Docs = docs, Save = s, Parts = parts };
        contents ??= SaveContents.Read(s.Dir);
        plan.Before = plan.After = contents.Bytes;
        try
        {
            if (contents.Packages.FirstOrDefault(p => p.Unknown) is { } odd)
                throw new SetupException("{0} has a part the launcher does not know, so this save is left as it is.", Path.GetFileName(odd.Path));
            var stars = SaveStars.Of(s.Name);
            var picked = new HashSet<string>(memories ?? [], StringComparer.OrdinalIgnoreCase);

            // which part each picture belongs to (None: it stays whatever is chosen), and the ones picked by key
            var part = new Dictionary<(string, int), CleanParts>();
            var chosen = new HashSet<(string, int)>();
            foreach (var kind in new[] { SaveKind.Portrait, SaveKind.LotThumbnail, SaveKind.HouseholdPicture, SaveKind.Generated })
                foreach (var i in contents.Items(kind))
                    part[(i.File, i.Index)] = kind switch
                    {
                        SaveKind.Portrait => CleanParts.Portraits,
                        SaveKind.Generated => stars.Contains(i.Key) ? CleanParts.None : CleanParts.Generated,
                        // (the save's own household picture stays; when Meta.data does not name it, they all stay)
                        SaveKind.HouseholdPicture when contents.OwnPicture == 0 || i.Instance == contents.OwnPicture => CleanParts.None,
                        _ => CleanParts.Thumbnails,
                    };
            foreach (var group in contents.MemoryGroups(ct))
            {
                // the one copy that stays when repeats go: a starred one, else the first nobody picked
                var last = group.Cast<SaveItem?>().FirstOrDefault(i => stars.Contains(i!.Value.Key)) ?? group.Cast<SaveItem?>().FirstOrDefault(i => !picked.Contains(i!.Value.Key)) ?? group[0];
                foreach (var i in group)
                {
                    var starred = stars.Contains(i.Key);
                    part[(i.File, i.Index)] = starred ? CleanParts.None : i == last ? CleanParts.Memories : CleanParts.RepeatedMemories;
                    if (!starred && picked.Contains(i.Key)) chosen.Add((i.File, i.Index));
                }
            }

            foreach (var p in contents.Packages)
            {
                bool Goes(int i, CleanParts with) => chosen.Contains((p.Path, i)) || (part.TryGetValue((p.Path, i), out var of) && (of & with) != 0);
                var whole = p.SizeWith(_ => true);
                foreach (var one in All)
                {
                    var n = Enumerable.Range(0, p.Count).Count(i => part.TryGetValue((p.Path, i), out var of) && of == one);
                    var g = plan.Gains.GetValueOrDefault(one) ?? new(0, 0);
                    plan.Gains[one] = new(g.Count + n, g.Bytes + whole - p.SizeWith(i => !(part.TryGetValue((p.Path, i), out var of) && of == one)));
                }
                var keep = Enumerable.Range(0, p.Count).Select(i => !Goes(i, parts)).ToArray();
                var size = p.SizeWith(i => keep[i]);
                plan.DeadBytes += Math.Max(0, p.Length - whole);
                if (size >= p.Length) continue; // (nothing to gain in this file: it is not touched)
                // a last guard on what the choices above let through: only pictures the game works without
                for (var i = 0; i < p.Count; i++)
                    if (!keep[i] && (p.Item(i).Kind is SaveKind.Game or SaveKind.LotTexture || (p.Item(i).Kind == SaveKind.HouseholdPicture && p.Item(i).Instance == contents.OwnPicture)))
                        throw new InvalidOperationException("a clean would remove game data");
                plan.Files.Add((p, keep, size));
                plan.Removed += keep.Count(k => !k);
                plan.After -= p.Length - size;
            }
            // (counted in the save's size, so Before is what the zip made first holds; real folders only, never a link)
            var nested = NestedCopies(s.Dir);
            var bytes = nested.Sum(d => new DirectoryInfo(d).EnumerateFiles("*", Deep).Sum(f => f.Length));
            plan.Gains[CleanParts.NestedCopies] = new(nested.Count, bytes);
            plan.Before += bytes;
            plan.After += bytes;
            if ((parts & CleanParts.NestedCopies) != 0) { plan.Nested.AddRange(nested); plan.After -= bytes; }
        }
        catch (SetupException e) { plan.Refusal = new(e.Key, e.Args); return plan; }
        // (the numbers are told also while the game runs or in a test run; only Run is refused)
        try { Engine.CheckWritable(); }
        catch (SetupException e) { plan.Refusal = new(e.Key, e.Args); }
        catch (InvalidOperationException e) { plan.Refusal = new(e.Message, []); }
        return plan;
    }

    static readonly EnumerationOptions Deep = new() { RecurseSubdirectories = true, AttributesToSkip = FileAttributes.ReparsePoint };

    // every folder in the save whose name ends in .sims3: a copy of a save inside it, which the game does not read
    static List<string> NestedCopies(string dir) => Directory.Exists(dir)
        ? new DirectoryInfo(dir).EnumerateDirectories("*.sims3").Where(d => d.Name.EndsWith(".sims3", StringComparison.OrdinalIgnoreCase) && !d.Attributes.HasFlag(FileAttributes.ReparsePoint)).Select(d => d.FullName).ToList()
        : [];

    public static CleanResult Run(CleanPlan plan, IProgress<CleanProgress>? progress = null, CancellationToken ct = default)
    {
        if (plan.Refusal is { } why) throw new SetupException(why.Key, why.Args);
        if (!plan.CanRun) throw new SetupException("There is nothing to clean in this save.");
        Engine.CheckWritable();
        SaveCopies.Recover(plan.Docs); // (what an earlier clean that was cut short left in the save)
        lock (SaveCopies.Gate) return RunNow(plan, progress, ct);
    }

    static void Unchanged(CleanPlan plan)
    {
        foreach (var (p, _, _) in plan.Files)
            if (new FileInfo(p.Path) is not { Exists: true } now || now.Length != p.Length || now.LastWriteTimeUtc != p.Written || File.Exists(p.Path + NewSuffix) || File.Exists(p.Path + OldSuffix))
                throw new SetupException("{0} changed since the launcher read it. Try again.", plan.Save.Name);
        if (plan.Nested.Any(d => !Directory.Exists(d) || Directory.Exists(d + OldSuffix))) throw new SetupException("{0} changed since the launcher read it. Try again.", plan.Save.Name);
    }

    static CleanResult RunNow(CleanPlan plan, IProgress<CleanProgress>? progress, CancellationToken ct)
    {
        Unchanged(plan);
        // 1. the whole save in a zip, opened again and checked file by file
        progress?.Report(new(CleanStep.Copy, 0, 1));
        var copy = Saves.Backup(plan.Save, plan.Docs, SaveCopies.BeforeClean);
        var swapped = new List<SavePackage>();
        var aside = new List<string>(); // (the nested copies moved out of the way, deleted only once all is in)
        try
        {
            FailPoint("copy");
            // 2. each new file beside its original, read back and compared before anything moves
            var done = 0;
            foreach (var (p, keep, size) in plan.Files)
            {
                ct.ThrowIfCancellationRequested();
                progress?.Report(new(CleanStep.Write, done, plan.Files.Count));
                Write(p, keep, p.Path + NewSuffix, ct);
                FailPoint("write");
                progress?.Report(new(CleanStep.Verify, done++, plan.Files.Count));
                Verify(p, keep, size, p.Path + NewSuffix, ct);
                // (the save is as old as before: "last played" and the Gallery's household guess read these times)
                File.SetLastWriteTimeUtc(p.Path + NewSuffix, p.Written);
                FailPoint("verify");
            }
            ct.ThrowIfCancellationRequested();
            Engine.CheckWritable();
            // 3. the swap, no cancel from here: a failure puts every original back
            foreach (var (p, _, _) in plan.Files)
            {
                if (new FileInfo(p.Path).Length != p.Length || File.GetLastWriteTimeUtc(p.Path) != p.Written) throw new SetupException("{0} changed since the launcher read it. Try again.", plan.Save.Name);
                Fs.Retry(() => File.Move(p.Path, p.Path + OldSuffix));
                swapped.Add(p);
                Fs.Retry(() => File.Move(p.Path + NewSuffix, p.Path));
                progress?.Report(new(CleanStep.Swap, swapped.Count, plan.Files.Count));
                FailPoint("swap");
            }
            foreach (var d in plan.Nested)
            {
                Fs.Retry(() => Directory.Move(d, d + OldSuffix));
                aside.Add(d);
                FailPoint("swap");
            }
            FailPoint("finish");
            foreach (var (p, _, size) in plan.Files)
                if (new FileInfo(p.Path).Length != size) throw new InvalidDataException(Path.GetFileName(p.Path) + " is not the size that was written");
        }
        catch
        {
            var back = true;
            for (var i = swapped.Count - 1; i >= 0; i--)
                try
                {
                    var path = swapped[i].Path;
                    // (the file in place is the one written here, or nothing yet: the original takes it back)
                    Fs.Retry(() => File.Move(path + OldSuffix, path, overwrite: true));
                }
                catch (Exception e) when (e is IOException or UnauthorizedAccessException) { back = false; Engine.Log(e); }
            foreach (var d in Enumerable.Reverse(aside))
                try { Fs.Retry(() => Directory.Move(d + OldSuffix, d)); }
                catch (Exception e) when (e is IOException or UnauthorizedAccessException) { back = false; Engine.Log(e); }
            foreach (var (p, _, _) in plan.Files)
                try { File.Delete(p.Path + NewSuffix); } catch (Exception e) when (e is IOException or UnauthorizedAccessException) { Engine.Log(e); }
            // (nothing changed: the zip is not a point anyone asked for. When an original is stuck aside, it stays: it is the save.)
            if (back) try { File.Delete(copy); } catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
            else throw new SetupException("The clean failed and a file could not be put back. The original save is in {0}.", copy);
            throw;
        }
        // 4. the originals go: the zip holds them (one that stays is cleared at the next start)
        foreach (var p in swapped)
            try { Fs.Retry(() => File.Delete(p.Path + OldSuffix)); } catch (Exception e) when (e is IOException or UnauthorizedAccessException) { Engine.Log(e); }
        foreach (var d in aside)
            try { Fs.DeleteClear(d + OldSuffix); } catch (Exception e) when (e is IOException or UnauthorizedAccessException) { Engine.Log(e); }
        SaveCopies.Thin(plan.Docs, plan.Save);
        return new(plan.Save, plan.Before, plan.After, plan.Removed, copy, SaveInfo.Read(plan.Save.Dir)?.LastPlayed ?? default);
    }

    // the save as it was before a clean: the zip the clean made goes back in (the save of now is zipped first, like any restore)
    public static Save Undo(string docs, CleanResult r, CancellationToken ct = default) =>
        r.Played != default && SaveInfo.Read(r.Save.Dir)?.LastPlayed > r.Played ? throw new SetupException("This save was played since it was cleaned, so it stays.") :
        SaveCopies.Restore(docs, r.Save, SaveCopies.Timeline(docs, r.Save).FirstOrDefault(p => p.Kind == CopyKind.Launcher && string.Equals(p.Path, r.Copy, StringComparison.OrdinalIgnoreCase))
            ?? throw new SetupException("The copy made before the clean is gone, so it can't be undone."), ct);

    // A package written anew with only some of its resources: the header bytes as they were (but the count and the
    // index's place and size), each kept resource's stored bytes in the order they lie in the file (a block two entries
    // share is written once), then the index with every field as it was but the offsets. A package with no unused room
    // comes out byte for byte the same. keep null: everything.
    public static void Rewrite(string from, string to, Func<SaveItem, bool>? keep = null)
    {
        var p = SavePackage.Open(from);
        if (p.Unknown) throw new InvalidDataException("the package has a part that is not understood");
        Write(p, Enumerable.Range(0, p.Count).Select(i => keep?.Invoke(p.Item(i)) ?? true).ToArray(), to, default);
    }

    static void Write(SavePackage p, bool[] keep, string to, CancellationToken ct)
    {
        var kept = Enumerable.Range(0, p.Count).Where(i => keep[i]).ToList();
        var moved = new Dictionary<(uint, uint), uint>();
        var buf = new byte[1 << 16];
        using var src = new FileStream(p.Path, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 16);
        using var o = new FileStream(to, FileMode.CreateNew, FileAccess.Write, FileShare.None, 1 << 20); // (never over a file that is there)
        try
        {
            o.Write(new byte[96]);
            foreach (var i in kept.OrderBy(i => p.Offset(i)).ThenBy(i => i))
            {
                ct.ThrowIfCancellationRequested();
                if (!moved.TryAdd((p.Offset(i), p.Size(i)), checked((uint)o.Position))) continue;
                src.Position = p.Offset(i);
                for (var left = (int)p.Size(i); left > 0;)
                {
                    var n = src.Read(buf, 0, Math.Min(buf.Length, left));
                    if (n == 0) throw new InvalidDataException(Path.GetFileName(p.Path) + " ended inside a resource");
                    o.Write(buf, 0, n);
                    left -= n;
                }
            }
            var index = new byte[p.IndexBytes(kept.Count)];
            var at = 0;
            void Put(uint v) { BinaryPrimitives.WriteUInt32LittleEndian(index.AsSpan(at), v); at += 4; }
            Put(p.Flags);
            for (var b = 0; b < 3; b++) if ((p.Flags >> b & 1) != 0) Put(p.Shared[b]);
            foreach (var i in kept)
                for (var w = 0; w < 8; w++)
                    if (w >= 3 || (p.Flags >> w & 1) == 0) Put(w == 4 ? moved[(p.Offset(i), p.Size(i))] : p.Raw[i * 8 + w]);
            var head = (byte[])p.Head.Clone();
            BinaryPrimitives.WriteUInt32LittleEndian(head.AsSpan(36), (uint)kept.Count);
            BinaryPrimitives.WriteUInt32LittleEndian(head.AsSpan(44), (uint)index.Length);
            BinaryPrimitives.WriteUInt32LittleEndian(head.AsSpan(64), checked((uint)o.Position));
            o.Write(index);
            o.Position = 0;
            o.Write(head);
            o.Flush(true);
        }
        catch { o.Dispose(); try { File.Delete(to); } catch (Exception e) when (e is IOException or UnauthorizedAccessException) { } throw; }
    }

    // the new file read back from disk: it parses, it is the planned size, its header and index say what the original's
    // did for every kept resource, and each resource's stored bytes are the original's
    static void Verify(SavePackage p, bool[] keep, long size, string made, CancellationToken ct)
    {
        static InvalidDataException Bad(string what) => new("the cleaned file does not read back right: " + what);
        var n = SavePackage.Open(made);
        var kept = Enumerable.Range(0, p.Count).Where(i => keep[i]).ToList();
        if (n.Unknown || n.Length != size || n.Count != kept.Count || n.Flags != p.Flags || !n.Shared.AsSpan().SequenceEqual(p.Shared)) throw Bad("its size or index");
        for (var b = 0; b < 96; b++)
            if (n.Head[b] != p.Head[b] && b is not (>= 36 and < 40 or >= 44 and < 48 or >= 64 and < 68)) throw Bad("its header");
        using var a = new FileStream(p.Path, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 16);
        using var c = new FileStream(made, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 16);
        byte[] x = new byte[1 << 16], y = new byte[1 << 16];
        for (var j = 0; j < kept.Count; j++)
        {
            ct.ThrowIfCancellationRequested();
            var i = kept[j];
            for (var w = 0; w < 8; w++)
                if (w != 4 && n.Raw[j * 8 + w] != p.Raw[i * 8 + w]) throw Bad("resource " + p.Item(i).Key);
            a.Position = p.Offset(i);
            c.Position = n.Offset(j);
            for (var left = (int)p.Size(i); left > 0;)
            {
                var len = Math.Min(x.Length, left);
                a.ReadExactly(x, 0, len);
                c.ReadExactly(y, 0, len);
                if (!x.AsSpan(0, len).SequenceEqual(y.AsSpan(0, len))) throw Bad("resource " + p.Item(i).Key);
                left -= len;
            }
        }
    }
}
