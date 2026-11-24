using System.Buffers.Binary;
using System.Text;
using System.Text.Json;

namespace ApexSetup;

// What keeps a mod from disappearing in a merge: mods whose content would be lost in one are left out of it, and originals
// whose merged file is gone are found and put back.
public static partial class ModMerge
{
    // One merge, split or put back at a time. Recover runs with every health check: it must not touch a merge being made.
    static readonly object Gate = new();

    // ---------- mods that can't share a package ----------
    const uint NameMap = 0x0166038C, Manifest = 0x73E93EEB; // (a package's list of names and its store receipt: not content)
    static bool Content((uint Type, uint Group, ulong Instance) k) => k.Type is not (NameMap or Manifest);

    // The packages of the merge that change the same game data in different ways, and those whose order against a loaded
    // package outside it would change, each with who it meets. One package holds one copy of a resource, and which copy the
    // game uses is not verified in game, so any choice here could take a mod's part out: they stay as they are.
    static Dictionary<int, SortedSet<string>> Clashes(string docs, string modsDir, List<ModConflicts.Directive> dirs, (int, int, string) target,
        IReadOnlyList<(MergeSource Src, (int, int, string) Key)> ok, CancellationToken ct = default)
    {
        var clash = new Dictionary<int, SortedSet<string>>();
        void Meet(int i, string other) => (clash.TryGetValue(i, out var l) ? l : clash[i] = new(StringComparer.OrdinalIgnoreCase)).Add(Path.GetFileNameWithoutExtension(other));
        using var read = new Contents();
        var owners = new Dictionary<(uint, uint, ulong), List<(int I, ModConflicts.Entry E)>>();
        for (var i = 0; i < ok.Count; i++)
            foreach (var e in ok[i].Src.Keep)
            {
                var k = (e.Type, e.Group, e.Instance);
                if (Content(k)) (owners.TryGetValue(k, out var l) ? l : owners[k] = new()).Add((i, e));
            }
        foreach (var l in owners.Values.Where(l => l.Any(x => x.I != l[0].I)))
            for (var a = 0; a < l.Count; a++)
                for (var b = a + 1; b < l.Count; b++)
                {
                    if (l[a].I == l[b].I || clash.TryGetValue(l[a].I, out var ca) && clash.TryGetValue(l[b].I, out var cb)
                        && ca.Contains(Name(l[b].I)) && cb.Contains(Name(l[a].I))) continue;
                    ct.ThrowIfCancellationRequested();
                    if (read.Same(ok[l[a].I].Src.Mod.Path, l[a].E, ok[l[b].I].Src.Mod.Path, l[b].E)) continue;
                    Meet(l[a].I, ok[l[b].I].Src.Mod.Name);
                    Meet(l[b].I, ok[l[a].I].Src.Mod.Name);
                }
        string Name(int i) => Path.GetFileNameWithoutExtension(ok[i].Src.Mod.Name);

        var skip = ok.Select(o => Path.GetFullPath(o.Src.Mod.Path)).ToHashSet(StringComparer.OrdinalIgnoreCase);
        var off = Path.GetFullPath(Mods.Disabled(docs)) + Path.DirectorySeparatorChar;
        List<string> files;
        try { files = Directory.EnumerateFiles(modsDir, "*.package", new EnumerationOptions { RecurseSubdirectories = true, AttributesToSkip = FileAttributes.ReparsePoint, IgnoreInaccessible = true }).ToList(); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { Engine.Log(e); return clash; }
        foreach (var f in files)
        {
            if (skip.Contains(f) || f.StartsWith(off, StringComparison.OrdinalIgnoreCase) || Keeps(docs, f)) continue;
            if (ModConflicts.LoadKey(dirs, Slash(Path.GetRelativePath(modsDir, f))) is not { } lk) continue;
            // (Probe: a damaged package is no known neighbour)
            if (Probe(f).Item1 is not { } entries) continue;
            foreach (var e in entries)
            {
                if (!owners.TryGetValue((e.Type, e.Group, e.Instance), out var l)) continue;
                foreach (var (i, mine) in l)
                    if (ModConflicts.CompareKey(ok[i].Key, lk) < 0 != ModConflicts.CompareKey(target, lk) < 0 && !read.Same(ok[i].Src.Mod.Path, mine, f, e))
                        Meet(i, f);
            }
        }
        return clash;
    }

    // resources compared by content, each file opened once
    sealed class Contents : IDisposable
    {
        readonly Dictionary<string, FileStream> _open = new(StringComparer.OrdinalIgnoreCase);

        byte[] Stored(string path, ModConflicts.Entry e)
        {
            if (!_open.TryGetValue(path, out var f))
            {
                if (_open.Count >= 64) Dispose();
                _open[path] = f = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.Read | FileShare.Delete, 1 << 16);
            }
            var b = new byte[e.FileSize];
            f.Position = e.Offset;
            f.ReadExactly(b);
            return b;
        }

        // the same bytes as stored, or once unpacked (two tools pack one resource differently); one that can't be read differs
        public bool Same(string a, ModConflicts.Entry ea, string b, ModConflicts.Entry eb)
        {
            if (ea.MemSize != eb.MemSize) return false;
            try
            {
                var (x, y) = (Stored(a, ea), Stored(b, eb));
                if (ea.Compressed == eb.Compressed && x.AsSpan().SequenceEqual(y)) return true;
                if (ea.Compressed != 0xFFFF && eb.Compressed != 0xFFFF) return false;
                return (ea.Compressed == 0xFFFF ? ModConflicts.RefPack(x) : x).AsSpan().SequenceEqual(eb.Compressed == 0xFFFF ? ModConflicts.RefPack(y) : y);
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException or InvalidDataException or ArgumentException or IndexOutOfRangeException or OverflowException) { return false; }
        }

        public byte[]? Unpacked(string path, ModConflicts.Entry e)
        {
            try { var b = Stored(path, e); return e.Compressed == 0xFFFF ? ModConflicts.RefPack(b) : b; }
            catch (Exception x) when (x is IOException or UnauthorizedAccessException or InvalidDataException or ArgumentException or IndexOutOfRangeException or OverflowException) { return null; }
        }

        public void Dispose() { foreach (var f in _open.Values) f.Dispose(); _open.Clear(); }
    }

    // ---------- the names ----------

    // The name maps several packages hold under one key, as one with every name in it (the first one's where two differ), as
    // s3pe's import does. Null when any can't be read: the first copy is kept then.
    static byte[]? JoinNames(IEnumerable<(string Path, ModConflicts.Entry E)> maps)
    {
        var names = new Dictionary<ulong, byte[]>();
        using var read = new Contents();
        foreach (var (path, e) in maps)
        {
            if (read.Unpacked(path, e) is not { Length: >= 8 } b || BinaryPrimitives.ReadUInt32LittleEndian(b) != 1) return null;
            var count = BinaryPrimitives.ReadInt32LittleEndian(b.AsSpan(4));
            var p = 8;
            for (var i = 0; i < count; i++)
            {
                if (p + 12 > b.Length) return null;
                var id = BinaryPrimitives.ReadUInt64LittleEndian(b.AsSpan(p));
                var len = BinaryPrimitives.ReadInt32LittleEndian(b.AsSpan(p + 8));
                if (len < 0 || len > b.Length - p - 12) return null;
                names.TryAdd(id, b[(p + 12)..(p + 12 + len)]);
                p += 12 + len;
            }
        }
        using var o = new MemoryStream();
        using (var w = new BinaryWriter(o, Encoding.UTF8, leaveOpen: true))
        {
            w.Write(1u);
            w.Write(names.Count);
            foreach (var (id, n) in names) { w.Write(id); w.Write(n.Length); w.Write(n); }
        }
        return o.ToArray();
    }

    // ---------- merges of an older launcher that lost a part ----------

    static readonly Dictionary<string, List<string>> _lost = new(); // (by the merged file's hash: its originals don't change)

    // Merges made before such mods stayed out: each with the mods of which some part is not in it (one copy of a resource
    // they hold differently was kept). Split brings them back whole.
    public static List<(ModFile Merged, List<string> Mods)> Lost(string docs)
    {
        var list = new List<(ModFile, List<string>)>();
        foreach (var r in Records(docs).Where(r => r.Version < 2).ToList())
            try
            {
                var (on, off) = (Path.Combine(docs, r.Merged), Path.Combine(Mods.Disabled(docs), UnderMods(r.Merged)));
                var path = File.Exists(on) ? on : off;
                if (!File.Exists(path)) continue;
                if (!_lost.TryGetValue(r.Sha256, out var mods))
                {
                    using var read = new Contents();
                    var kept = new Dictionary<(uint, uint, ulong), (string Path, ModConflicts.Entry E)>();
                    mods = new();
                    foreach (var e in ModConflicts.Package.Open(path).Entries) kept.TryAdd((e.Type, e.Group, e.Instance), (path, e));
                    foreach (var o in r.Originals)
                    {
                        var own = Path.Combine(r.Dir, UnderMods(o.Rel));
                        // (a package's own repeats of a resource all went in: only a resource in none of its copies there is lost)
                        if (File.Exists(own) && ModConflicts.Package.Open(own).Entries.GroupBy(e => (e.Type, e.Group, e.Instance))
                                .Any(g => Content(g.Key) && (!kept.TryGetValue(g.Key, out var k) || !g.Any(e => read.Same(own, e, k.Path, k.E)))))
                            mods.Add(Path.GetFileNameWithoutExtension(o.Rel));
                    }
                    _lost[r.Sha256] = mods;
                }
                if (mods.Count > 0) list.Add((new ModFile(path, r.Merged, r.Size, ModKind.Package, path == on, false), mods));
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException or InvalidDataException or ArgumentException or IndexOutOfRangeException or OverflowException) { Engine.Log(e); }
        return list;
    }

    // ---------- originals whose merged file is gone ----------

    // Files: the originals still kept, as paths in Dir
    public sealed record Orphan(string Name, string Dir, IReadOnlyList<string> Files);

    static IEnumerable<string> Stores(string docs) => new[] { Folder(docs), OldFolder(docs) }.Where(Directory.Exists).SelectMany(Directory.EnumerateDirectories);

    static bool Present(string docs, MergeRecord r) => File.Exists(Path.Combine(docs, r.Merged)) || File.Exists(Path.Combine(Mods.Disabled(docs), UnderMods(r.Merged)))
        || Stores(docs).Any(d => File.Exists(Path.Combine(d, UnderMods(r.Merged)))); // (a merge inside another merge)

    // The merges whose file is no longer there (removed, or its record lost), with what they still keep: those mods are in
    // neither the game nor the Mods list.
    public static List<Orphan> Orphans(string docs)
    {
        var list = new List<Orphan>();
        try
        {
            var known = Records(docs).ToDictionary(r => Path.GetFullPath(r.Dir), StringComparer.OrdinalIgnoreCase);
            foreach (var d in Stores(docs))
            {
                if (known.TryGetValue(Path.GetFullPath(d), out var r) && Present(docs, r)) continue;
                var files = Directory.EnumerateFiles(d, "*", SearchOption.AllDirectories).Where(f => !f.Equals(Path.Combine(d, "merge.json"), StringComparison.OrdinalIgnoreCase)).ToList();
                if (files.Count > 0) list.Add(new(Path.GetFileName(d), d, files));
            }
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { Engine.Log(e); }
        return list;
    }

    // The originals of a merge that is gone, back at their places (beside a different file there, under a free name) with
    // their notes. How many went back.
    public static int PutBack(string docs, Orphan o)
    {
        Engine.CheckWritable();
        lock (Gate)
        {
            using var log = OpenLog("putback", out _);
            var rec = Records(docs).FirstOrDefault(r => Path.GetFullPath(r.Dir).Equals(Path.GetFullPath(o.Dir), StringComparison.OrdinalIgnoreCase));
            var n = 0;
            foreach (var from in o.Files.Where(File.Exists))
            {
                var rel = Path.GetRelativePath(o.Dir, from);
                var to = Path.Combine(docs, "Mods", rel);
                Directory.CreateDirectory(Path.GetDirectoryName(to)!);
                if (File.Exists(to) && Sha(to) == Sha(from)) Fs.Retry(() => File.Delete(from)); // (an identical copy is already in place)
                else { to = Mods.Free(to); Fs.Retry(() => File.Move(from, to)); }
                log.WriteLine($"put back {rel} as {Path.GetRelativePath(docs, to)}");
                n++;
                if (rec?.Originals.FirstOrDefault(x => UnderMods(x.Rel).Equals(rel, StringComparison.OrdinalIgnoreCase))?.Note is { } note && ModNotes.Get(Path.Combine("Mods", rel)) is null) ModNotes.Put(Path.Combine("Mods", rel), note);
            }
            DeleteStore(o.Dir);
            Forget(docs);
            return n;
        }
    }

    // a merged package and its originals to the Recycle Bin, so none is left kept where nothing shows it
    public static void RemoveAll(string docs, ModFile merged)
    {
        var rec = Info(docs, merged) ?? throw new SetupException("This file is not a merge made by the launcher.");
        if (merged.Locked) throw new SetupException("The launcher does not move, rename or remove this file.");
        Engine.CheckWritable();
        lock (Gate)
        {
            Recycle(merged.Path);
            foreach (var f in Directory.EnumerateFiles(rec.Dir, "*", SearchOption.AllDirectories).Where(f => !f.Equals(Path.Combine(rec.Dir, "merge.json"), StringComparison.OrdinalIgnoreCase)).ToList()) Recycle(f);
            DeleteStore(rec.Dir);
            Forget(docs);
            ModNotes.Forget(merged.Rel);
        }
    }

    // A merged file renamed or moved outside the launcher: found again by its size and hash, and its record follows.
    static void Relink(string docs)
    {
        var lost = Records(docs).Where(r => !Present(docs, r)).ToList();
        if (lost.Count == 0) return;
        var taken = Records(docs).Where(r => !lost.Contains(r)).Select(r => r.Merged).ToHashSet(StringComparer.OrdinalIgnoreCase);
        var (mods, off) = (Path.Combine(docs, "Mods"), Mods.Disabled(docs));
        var files = Directory.EnumerateFiles(mods, "*.package", new EnumerationOptions { RecurseSubdirectories = true, IgnoreInaccessible = true, AttributesToSkip = FileAttributes.ReparsePoint })
            .Where(f => !Keeps(docs, f)).Select(f => new FileInfo(f)).ToList();
        foreach (var r in lost)
            try
            {
                foreach (var f in files.Where(f => f.Length == r.Size))
                {
                    var isOff = f.FullName.StartsWith(off + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase);
                    var rel = Path.Combine("Mods", Path.GetRelativePath(isOff ? off : mods, f.FullName));
                    if (taken.Contains(rel) || Sha(f.FullName) != r.Sha256) continue;
                    r.Merged = rel;
                    File.WriteAllText(Path.Combine(r.Dir, "merge.json"), JsonSerializer.Serialize(r, SetupJson.Default.MergeRecord));
                    taken.Add(rel);
                    break;
                }
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { Engine.Log(e); }
    }
}
