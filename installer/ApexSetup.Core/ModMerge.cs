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
// one resource key ("type:group:instance") in several packages: the copy the game loads today is kept
public sealed record MergeDuplicate(string Key, string Kept, IReadOnlyList<string> Dropped);

public sealed class MergePlan
{
    public required string Docs { get; init; }
    public required string Name { get; init; }
    public required string TargetPath { get; init; }   // Mods\<folder>\<name>.package
    public required string TargetRel { get; init; }    // as ModFile.Rel
    public required string StoreDir { get; init; }     // Mods\Merged (Apex Launcher)\<name>, where the originals go
    public List<ModFile> Inputs { get; } = new();      // in the game's load order (the first one wins a shared key)
    public List<MergeSkip> Skipped { get; } = new();
    public List<MergeDuplicate> Duplicates { get; } = new();
    public List<MergeReason> Refusals { get; } = new();
    public int Resources { get; internal set; }
    public long OutputBytes { get; internal set; }      // exact: header, stored resource bytes, index
    public bool CanRun => Refusals.Count == 0;
    public string? LogPath { get; internal set; }      // set by Run, also when it fails
    internal List<MergeSource> Sources { get; } = new();
}

internal sealed record MergeSource(ModFile Mod, long Size, DateTime Written, List<ModConflicts.Entry> Keep);

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
    public int Version { get; set; } = 1;
    public string Name { get; set; } = "";
    public string Merged { get; set; } = "";           // the merged file's Rel
    public string Sha256 { get; set; } = "";
    public long Size { get; set; }
    public int Resources { get; set; }
    public DateTime Created { get; set; }
    public List<MergeOriginal> Originals { get; set; } = new();
    [System.Text.Json.Serialization.JsonIgnore] public string Dir { get; set; } = "";
}

// Merge packages into one: the plan says what goes in and which copy of a shared resource key is kept (the one the game
// loads today), Run writes a temporary file copying every resource's stored bytes as they are (compressed stays
// compressed), reads it back and compares each resource, then moves the originals to Mods\Merged (Apex Launcher)\<name>
// (outside Packages and Overrides, so the game does not load them) and renames the new file into place. Split puts the
// originals back by hash and sends the merged file to the Recycle Bin.
// The uninstall leaves merges alone: the merged package works without the launcher, merge.json keeps the notes once the
// launcher's data is gone, and splitting there could stop halfway on a replaced original.
// Shortcut: the merged file's load position replaces each original's, so a key also in a package outside the merge may
// now win or lose against it; not checked, a plan over the whole Mods folder would show it.
public static partial class ModMerge
{
    public const string FolderName = "Merged (Apex Launcher)";
    public static string Folder(string docs) => Path.Combine(docs, "Mods", FolderName);
    public const long MaxBytes = int.MaxValue; // (the game is 32-bit and the index offsets are 32-bit)
    const int MaxEntries = 200_000;

    // test hook: called at "write" (after the first resource), "verify", "move" (after each original) and "rename"
    public static Action<string> FailPoint = _ => { };
    // test hook: the Recycle Bin (tests must not fill the real one)
    public static Action<string> Recycle = ModLibrary.Recycle;

    static MergeReason R(string key, params string[] args) => new(key, args);
    static string Slash(string p) => p.Replace('\\', '/');
    static string UnderMods(string rel) => Path.GetRelativePath("Mods", rel);

    // ---------- plan ----------
    public static MergePlan Plan(string docs, IReadOnlyList<ModFile> mods, string name, string folderRel, bool includeScripts)
    {
        var modsDir = Path.GetFullPath(Path.Combine(docs, "Mods"));
        if (name.EndsWith(".package", StringComparison.OrdinalIgnoreCase)) name = name[..^8];
        var refusals = new List<MergeReason>();
        try { name = ModLibrary.CleanName(name); }
        catch (SetupException e) { refusals.Add(new(e.Key, e.Args)); }
        if (name.Equals(FolderName, StringComparison.OrdinalIgnoreCase)) refusals.Add(R("That name is the launcher's own folder."));
        var folderFull = Path.GetFullPath(Path.Combine(modsDir, folderRel));
        bool Inside(string full, string dir) => full.StartsWith(dir + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase) || full.Equals(dir, StringComparison.OrdinalIgnoreCase);
        if (!Inside(folderFull, modsDir) || Inside(folderFull, Path.GetFullPath(Mods.Disabled(docs))) || Inside(folderFull, Path.GetFullPath(Folder(docs))))
        {
            refusals.Add(R("That folder is outside Mods."));
            folderFull = modsDir;
        }
        var targetRel = Path.Combine("Mods", Path.GetRelativePath(modsDir, Path.Combine(folderFull, name + ".package")));
        var plan = new MergePlan
        {
            Docs = docs, Name = name, TargetPath = Path.Combine(folderFull, name + ".package"), TargetRel = targetRel,
            StoreDir = Path.Combine(Folder(docs), name),
        };
        plan.Refusals.AddRange(refusals);
        try { Engine.CheckWritable(); }
        catch (SetupException e) { plan.Refusals.Add(new(e.Key, e.Args)); }
        catch (InvalidOperationException e) { plan.Refusals.Add(R(e.Message)); } // (dry run)
        // (a store left holding only merge.json is what an earlier split could not delete: cleaned first, it holds no original)
        if (!File.Exists(plan.TargetPath)) DeleteStore(plan.StoreDir);
        if (File.Exists(plan.TargetPath) || File.Exists(Path.Combine(Mods.Disabled(docs), UnderMods(targetRel))) || Directory.Exists(plan.StoreDir))
            plan.Refusals.Add(R("A file or merge named {0} is already there.", name));

        var cfg = Path.Combine(modsDir, "Resource.cfg");
        if (!File.Exists(cfg)) { plan.Refusals.Add(R("There is no Resource.cfg in Mods, so the game loads no packages from there.")); return plan; }
        var dirs = ModConflicts.ParseResourceCfg(File.ReadAllText(cfg));
        if (ModConflicts.LoadKey(dirs, Slash(UnderMods(targetRel))) is null)
            plan.Refusals.Add(R("The game does not load packages from {0} (Resource.cfg).", Path.GetDirectoryName(targetRel) ?? "Mods"));

        var ok = new List<(MergeSource Src, (int, int, string) Key)>();
        var store = Path.GetFullPath(Folder(docs)) + Path.DirectorySeparatorChar;
        foreach (var m in mods)
        {
            MergeReason? why = null;
            List<ModConflicts.Entry>? entries = null;
            var relMods = Slash(UnderMods(m.Rel));
            var key = ModConflicts.LoadKey(dirs, relMods);
            if (m.FileType != "package" || !m.Rel.StartsWith("Mods" + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase)) why = R("Not a package in Mods.");
            else if (m.Locked) why = R("Part of what the launcher installs.");
            else if (!m.Enabled) why = R("Switched off.");
            else if (Path.GetFullPath(m.Path).StartsWith(store, StringComparison.OrdinalIgnoreCase)) why = R("Kept by an earlier merge.");
            else if (m.Name.StartsWith('~') || m.Name.Contains(".tmp", StringComparison.OrdinalIgnoreCase)) why = R("A temporary file.");
            else if (key is null) why = R("The game does not load it (Resource.cfg).");
            else if (ModConflicts.LoadKey(dirs, Slash(Path.Combine(FolderName, name, relMods))) is not null) why = R("Resource.cfg would load it from the merge folder.");
            else
            {
                (entries, why) = Probe(m.Path);
                if (entries is not null && !includeScripts && entries.Any(e => e.Type == ModConflicts.TypeS3sa)) why = R("A script mod: left out unless scripts are included.");
            }
            if (why is not null) { plan.Skipped.Add(new(m, why)); continue; }
            var fi = new FileInfo(m.Path);
            ok.Add((new MergeSource(m, fi.Length, fi.LastWriteTimeUtc, entries!), key!.Value));
        }
        ok.Sort((a, b) => ModConflicts.CompareKey(a.Key, b.Key));
        if (ok.Count < 2) plan.Refusals.Add(R("Pick at least two packages that can be merged."));

        // the first package in load order owns a key; its own repeats of it stay, the later packages' copies go
        var owner = new Dictionary<(uint, uint, ulong), int>();
        var dropped = new Dictionary<(uint, uint, ulong), List<string>>();
        for (var i = 0; i < ok.Count; i++)
        {
            var (src, _) = ok[i];
            var all = src.Keep.ToList();
            src.Keep.Clear();
            foreach (var e in all)
            {
                var k = (e.Type, e.Group, e.Instance);
                if (owner.TryAdd(k, i) || owner[k] == i) { src.Keep.Add(e); continue; }
                if (!dropped.TryGetValue(k, out var l)) dropped[k] = l = new();
                if (!l.Contains(src.Mod.Rel)) l.Add(src.Mod.Rel);
            }
            plan.Inputs.Add(src.Mod);
            plan.Sources.Add(src);
        }
        foreach (var (k, l) in dropped)
            plan.Duplicates.Add(new($"{k.Item1:x8}:{k.Item2:x8}:{k.Item3:x16}", ok[owner[k]].Src.Mod.Rel, l));
        plan.Resources = plan.Sources.Sum(s => s.Keep.Count);
        plan.OutputBytes = 96 + plan.Sources.Sum(s => s.Keep.Sum(e => (long)e.FileSize)) + 4 + 32L * plan.Resources;
        if (plan.OutputBytes > MaxBytes) plan.Refusals.Add(R("The merged package would be over 2 GB."));
        if (plan.Resources > MaxEntries) plan.Refusals.Add(R("The merged package would hold too many resources."));
        return plan;
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
                if (len < 96) return (null, R("Not a readable package: {0}", "too short"));
                fs.ReadExactly(head);
            }
            var magic = Encoding.Latin1.GetString(head, 0, 4);
            if (magic == "DBPP") return (null, R("A protected package: its contents can't be copied."));
            if (magic != "DBPF" || U32(head, 4) != 2) return (null, R("Not a readable package: {0}", "not DBPF 2"));
            if (len > MaxBytes) return (null, R("Not a readable package: {0}", "over 2 GB"));
            uint count = U32(head, 36), size = U32(head, 44), at = U32(head, 64);
            if (count > MaxEntries) return (null, R("Not a readable package: {0}", "index count too large"));
            if (at < 96 || (long)at + size > len || (count > 0 && size < 4 + 20L * count)) return (null, R("Not a readable package: {0}", "index outside the file"));
            var entries = ModConflicts.Package.Open(path).Entries;
            if (entries.Count != count) return (null, R("Not a readable package: {0}", "index count"));
            if (entries.Any(e => e.FileSize < 0 || (long)e.Offset + e.FileSize > len)) return (null, R("Not a readable package: {0}", "a resource points past the end"));
            return (entries, null);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or InvalidDataException or ArgumentException or IndexOutOfRangeException or OverflowException)
        {
            return (null, R("Not a readable package: {0}", e.Message));
        }
    }
    static uint U32(byte[] b, int p) => BinaryPrimitives.ReadUInt32LittleEndian(b.AsSpan(p));

    // ---------- run ----------
    public static MergeResult Run(MergePlan plan, IProgress<MergeProgress>? progress, CancellationToken ct)
    {
        if (!plan.CanRun) throw new SetupException("This merge can't run: {0}", plan.Refusals[0].Text);
        var sw = Stopwatch.StartNew();
        using var log = OpenLog("merge", out var logPath);
        plan.LogPath = logPath;
        log.WriteLine($"merge \"{plan.Name}\" -> {plan.TargetRel}: {plan.Inputs.Count} packages, {plan.Resources} resources, {plan.OutputBytes} bytes");
        foreach (var s in plan.Sources) log.WriteLine($"in   {s.Mod.Rel}: {s.Keep.Count} resources kept, {s.Size} bytes");
        foreach (var s in plan.Skipped) log.WriteLine($"skip {s.Mod.Rel}: {s.Reason.Text}");
        foreach (var d in plan.Duplicates) log.WriteLine($"key  {d.Key}: kept from {d.Kept}, dropped from {string.Join(", ", d.Dropped)}");
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
                Name = plan.Name, Merged = plan.TargetRel, Sha256 = sha, Size = new FileInfo(tmp).Length, Resources = written.Count, Created = DateTime.UtcNow,
                Originals = plan.Sources.Select((s, i) => new MergeOriginal { Rel = s.Mod.Rel, Size = s.Size, Sha256 = hashes[i], Note = ModNotes.Get(s.Mod.Rel) }).ToList(),
            };
            Directory.CreateDirectory(plan.StoreDir);
            File.WriteAllText(json, JsonSerializer.Serialize(record, SetupJson.Default.MergeRecord));
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
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { log.WriteLine("notes not moved: " + e.Message); Engine.Log(e); }
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
        long data = plan.Sources.Sum(s => s.Keep.Sum(e => (long)e.FileSize)), done = 0;
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
        foreach (var r in Records(docs).ToList())
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
        Forget(docs);
    }

    // the merge folder once its originals are gone: merge.json and empty folders only
    static void DeleteStore(string dir)
    {
        try
        {
            if (!Directory.Exists(dir)) return;
            if (Directory.EnumerateFiles(dir, "*", SearchOption.AllDirectories).Any(f => !Path.GetFileName(f).Equals("merge.json", StringComparison.OrdinalIgnoreCase)
                || !Path.GetDirectoryName(f)!.Equals(dir, StringComparison.OrdinalIgnoreCase))) return;
            Fs.Retry(() => Directory.Delete(dir, true));
            var parent = Path.GetDirectoryName(dir)!;
            if (!Directory.EnumerateFileSystemEntries(parent).Any()) Fs.Retry(() => Directory.Delete(parent));
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

    static List<MergeRecord> Records(string docs)
    {
        docs = Path.GetFullPath(docs);
        lock (_records)
        {
            if (_records.TryGetValue(docs, out var l)) return l;
            l = new();
            if (Directory.Exists(Folder(docs)))
                foreach (var d in Directory.EnumerateDirectories(Folder(docs)))
                    try
                    {
                        var j = Path.Combine(d, "merge.json");
                        if (!File.Exists(j)) continue;
                        var r = JsonSerializer.Deserialize(File.ReadAllText(j), SetupJson.Default.MergeRecord);
                        if (r is null) continue;
                        r.Dir = d;
                        l.Add(r);
                    }
                    catch (Exception e) when (e is IOException or UnauthorizedAccessException or JsonException) { Engine.Log(e); }
            return _records[docs] = l;
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
            File.WriteAllText(Path.Combine(r.Dir, "merge.json"), JsonSerializer.Serialize(r, SetupJson.Default.MergeRecord));
        }
    }

    // ---------- split ----------
    // The originals back where they were (switched off when the merged file is), checked by hash; the merged file to the
    // Recycle Bin; the notes back. The restored mods.
    public static List<ModFile> Split(string docs, ModFile merged)
    {
        var rec = Info(docs, merged) ?? throw new SetupException("This file is not a merge made by the launcher.");
        if (merged.Locked) throw new SetupException("The launcher does not move, rename or remove this file.");
        Engine.CheckWritable();
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
        if (taken.Count > 0) throw new SetupException("A different file now sits where an original goes: {0}", string.Join(", ", taken));

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
