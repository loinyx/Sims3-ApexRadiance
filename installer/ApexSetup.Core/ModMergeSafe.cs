using System.Buffers.Binary;
using System.Text;
using System.Text.Json;

namespace ApexSetup;

// What keeps a mod from disappearing in a merge: mods whose content would be lost in one stay out of it unless the player
// chooses which comes first, and originals whose merged file is gone are found and put back.
public static partial class ModMerge
{
    // One merge, split or put back at a time. Recover runs with every health check: it must not touch a merge being made.
    static readonly object Gate = new();

    // ---------- mods that can't share a package ----------
    const uint NameMap = 0x0166038C, Manifest = 0x73E93EEB; // (a package's list of names and its store receipt: not content)
    static bool Content((uint Type, uint Group, ulong Instance) k) => k.Type is not (NameMap or Manifest);

    // The packages of the merge that change the same game data in different ways, in groups, and those whose order against a
    // loaded package outside it would change. One package holds one copy of a resource, and which copy the game uses is not
    // verified in game, so any choice made here could take a mod's part out: it is the player's.
    // Keys: what clashes, by package. Pairs: with which others of them each one clashes, in how many resources. OutsideOf: and
    // with which outside ones. exclude: the merged file of the merge being changed, which holds copies of its own mods.
    static List<(List<int> Group, SortedSet<string> Outside, int Resources, Dictionary<int, HashSet<(uint, uint, ulong)>> Keys, Dictionary<int, Dictionary<int, int>> Pairs, Dictionary<int, SortedSet<string>> OutsideOf)> Clashes(string docs, string modsDir, List<ModConflicts.Directive> dirs, (int, int, string) target,
        IReadOnlyList<(MergeSource Src, (int, int, string) Key)> ok, string? exclude)
    {
        using var read = new Contents();
        var of = Enumerable.Range(0, ok.Count).ToArray(); // (each package's group, by its first one)
        int Top(int i) => of[i] == i ? i : of[i] = Top(of[i]);
        var keys = new Dictionary<int, HashSet<(uint, uint, ulong)>>(); // (what clashes, by package)
        var outside = new Dictionary<int, SortedSet<string>>();
        var pairs = new Dictionary<int, Dictionary<int, int>>();
        void Add(int i, (uint, uint, ulong) k) => (keys.TryGetValue(i, out var l) ? l : keys[i] = new()).Add(k);
        var owners = new Dictionary<(uint, uint, ulong), List<(int I, ModConflicts.Entry E)>>();
        for (var i = 0; i < ok.Count; i++)
            foreach (var e in ok[i].Src.Keep)
            {
                var k = (e.Type, e.Group, e.Instance);
                if (Content(k)) (owners.TryGetValue(k, out var l) ? l : owners[k] = new()).Add((i, e));
            }
        foreach (var (k, l) in owners.Where(o => o.Value.Any(x => x.I != o.Value[0].I)))
        {
            // (the copies by content: one kind is no clash, more put every package that holds one in a group)
            var kinds = new List<(int I, ModConflicts.Entry E)>();
            var kindOf = new List<int>();
            foreach (var x in l)
            {
                var at = kinds.FindIndex(y => read.Same(ok[y.I].Src.Mod.Path, y.E, ok[x.I].Src.Mod.Path, x.E));
                kindOf.Add(at >= 0 ? at : kinds.Count);
                if (at < 0) kinds.Add(x);
            }
            if (kinds.Count < 2) continue;
            foreach (var x in l) { Add(x.I, k); of[Top(x.I)] = Top(l[0].I); }
            var copies = l.Select((x, n) => (x.I, Kind: kindOf[n])).Distinct().ToList();
            foreach (var (a, b) in copies.SelectMany(x => copies.Where(y => y.I != x.I && y.Kind != x.Kind).Select(y => (x.I, y.I))).Distinct())
            {
                var with = pairs.TryGetValue(a, out var p) ? p : pairs[a] = new();
                with[b] = with.GetValueOrDefault(b) + 1;
            }
        }

        var skip = ok.Select(o => Path.GetFullPath(o.Src.Mod.Path)).ToHashSet(StringComparer.OrdinalIgnoreCase);
        if (exclude is not null) skip.Add(Path.GetFullPath(exclude));
        var off = Path.GetFullPath(Mods.Disabled(docs)) + Path.DirectorySeparatorChar;
        List<string> files = [];
        try { files = Directory.EnumerateFiles(modsDir, "*.package", new EnumerationOptions { RecurseSubdirectories = true, AttributesToSkip = FileAttributes.ReparsePoint, IgnoreInaccessible = true }).ToList(); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { Engine.Log(e); }
        foreach (var f in files)
        {
            if (skip.Contains(f) || f.StartsWith(off, StringComparison.OrdinalIgnoreCase) || Keeps(docs, f)) continue;
            if (ModConflicts.LoadKey(dirs, Slash(Path.GetRelativePath(modsDir, f))) is not { } lk) continue;
            // (Probe: a damaged package is no known neighbour)
            if (Probe(f).Item1 is not { } entries) continue;
            foreach (var e in entries)
            {
                var k = (e.Type, e.Group, e.Instance);
                if (!owners.TryGetValue(k, out var l)) continue;
                foreach (var (i, mine) in l)
                    if (ModConflicts.CompareKey(ok[i].Key, lk) < 0 != ModConflicts.CompareKey(target, lk) < 0 && !read.Same(ok[i].Src.Mod.Path, mine, f, e))
                    {
                        Add(i, k);
                        (outside.TryGetValue(i, out var o) ? o : outside[i] = new(StringComparer.OrdinalIgnoreCase)).Add(Path.GetFileNameWithoutExtension(f));
                    }
            }
        }
        return keys.Keys.GroupBy(Top).OrderBy(g => g.Key).Select(g => (g.Order().ToList(),
            new SortedSet<string>(g.SelectMany(i => outside.GetValueOrDefault(i) ?? []), StringComparer.OrdinalIgnoreCase), g.SelectMany(i => keys[i]).Distinct().Count(), keys, pairs, outside)).ToList();
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

        // the same bytes as stored, or once unpacked (two tools pack one resource differently), or the same to the game (tuning
        // or texts written another way, ModConflicts.SameMeaning); one that can't be read differs
        public bool Same(string a, ModConflicts.Entry ea, string b, ModConflicts.Entry eb)
        {
            // (of another size and too large for tuning or texts: different, without reading it)
            if (ea.MemSize != eb.MemSize && Math.Max(ea.MemSize, eb.MemSize) > ModConflicts.MeaningCap) return false;
            try
            {
                var (x, y) = (Stored(a, ea), Stored(b, eb));
                if (ea.Compressed == eb.Compressed && x.AsSpan().SequenceEqual(y)) return true;
                return ModConflicts.SameMeaning(ea.Type, ea.Compressed == 0xFFFF ? ModConflicts.RefPack(x) : x, eb.Compressed == 0xFFFF ? ModConflicts.RefPack(y) : y);
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

    // The name maps several packages hold under one key, as one with every name in it (the first one's where two differ).
    // Null when any can't be read: the first copy is kept then.
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
                List<string>? mods;
                lock (_lost) _lost.TryGetValue(r.Sha256, out mods);
                if (mods is null)
                {
                    mods = LostIn(r, path);
                    lock (_lost) _lost[r.Sha256] = mods;
                }
                if (mods.Count > 0) list.Add((new ModFile(path, r.Merged, r.Size, ModKind.Package, path == on, false), mods));
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException or InvalidDataException or ArgumentException or IndexOutOfRangeException or OverflowException) { Engine.Log(e); }
        return list;
    }

    // (only a resource in two originals or more can have lost a copy: the others went in as they are)
    static List<string> LostIn(MergeRecord r, string merged)
    {
        using var read = new Contents();
        var kept = new Dictionary<(uint, uint, ulong), ModConflicts.Entry>();
        foreach (var e in ModConflicts.Package.Open(merged).Entries) kept.TryAdd((e.Type, e.Group, e.Instance), e);
        var own = r.Originals.Select(o => Path.Combine(r.Dir, UnderMods(o.Rel))).Where(File.Exists)
            .Select(p => (Path: p, Keys: ModConflicts.Package.Open(p).Entries.Where(e => Content((e.Type, e.Group, e.Instance))).ToLookup(e => (e.Type, e.Group, e.Instance)))).ToList();
        var shared = own.SelectMany(o => o.Keys.Select(k => k.Key)).GroupBy(k => k).Where(g => g.Count() > 1).Select(g => g.Key).ToHashSet();
        return own.Where(o => o.Keys.Any(g => shared.Contains(g.Key) && (!kept.TryGetValue(g.Key, out var k) || !g.Any(e => read.Same(o.Path, e, merged, k)))))
            .Select(o => Path.GetFileNameWithoutExtension(o.Path)).ToList();
    }

    // ---------- originals whose merged file is gone ----------

    // Files: the originals still kept, as paths in Dir
    public sealed record Orphan(string Name, string Dir, IReadOnlyList<string> Files);

    static IEnumerable<string> Stores(string docs) => new[] { Folder(docs), OldFolder(docs) }.Where(Directory.Exists).SelectMany(Directory.EnumerateDirectories);

    static bool Present(string docs, MergeRecord r) => File.Exists(Path.Combine(docs, r.Merged)) || File.Exists(Path.Combine(Mods.Disabled(docs), UnderMods(r.Merged)))
        || Stores(docs).Any(d => File.Exists(Path.Combine(d, UnderMods(r.Merged)))); // (a merge inside another merge)

    // what a store keeps: not its record, nor what Windows leaves in a folder
    static List<string> Kept(string dir) => Directory.EnumerateFiles(dir, "*", SearchOption.AllDirectories)
        .Where(f => !f.Equals(Path.Combine(dir, "merge.json"), StringComparison.OrdinalIgnoreCase) && Path.GetFileName(f).ToLowerInvariant() is not ("desktop.ini" or "thumbs.db")).ToList();

    // a merged file of that name anywhere in Mods (a record that can't be read says only its folder's name)
    static bool Named(string docs, string name) => Directory.EnumerateFiles(Path.Combine(docs, "Mods"), name + ".package", new EnumerationOptions { RecurseSubdirectories = true, IgnoreInaccessible = true, AttributesToSkip = FileAttributes.ReparsePoint })
        .Any(f => !Keeps(docs, f));

    // The merges whose file is no longer there (removed, or its record lost), with what they still keep: those mods are in
    // neither the game nor the Mods list. None while a merge is being made, nor with no Mods folder to go back to (a player
    // trying the game without mods has it renamed).
    public static List<Orphan> Orphans(string docs)
    {
        var list = new List<Orphan>();
        if (!Monitor.TryEnter(Gate)) return list;
        try
        {
            if (!File.Exists(Path.Combine(docs, "Mods", "Resource.cfg"))) return list;
            Forget(docs);
            var known = Records(docs).ToDictionary(r => Path.GetFullPath(r.Dir), StringComparer.OrdinalIgnoreCase);
            foreach (var d in Stores(docs))
            {
                if (known.TryGetValue(Path.GetFullPath(d), out var r) ? Present(docs, r) : Named(docs, Path.GetFileName(d))) continue;
                if (Kept(d) is { Count: > 0 } files) list.Add(new(Path.GetFileName(d), d, files));
            }
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { Engine.Log(e); }
        finally { Monitor.Exit(Gate); }
        return list;
    }

    // The originals of a merge that is gone, back at their places (beside a different file there, under a free name) with
    // their notes. How many went back. The merge is looked at again: its file may be back since it was found gone.
    public static int PutBack(string docs, Orphan found)
    {
        Engine.CheckWritable();
        lock (Gate)
        {
            var o = Orphans(docs).FirstOrDefault(x => x.Dir.Equals(found.Dir, StringComparison.OrdinalIgnoreCase)) ?? throw new SetupException("That merge is back in Mods. Split it there.");
            using var log = OpenLog("putback", out _);
            var rec = Records(docs).FirstOrDefault(r => Path.GetFullPath(r.Dir).Equals(Path.GetFullPath(o.Dir), StringComparison.OrdinalIgnoreCase));
            var n = 0;
            foreach (var from in o.Files.Where(File.Exists))
            {
                var rel = Path.GetRelativePath(o.Dir, from);
                var to = Path.Combine(docs, "Mods", rel);
                Directory.CreateDirectory(Path.GetDirectoryName(to)!);
                if (File.Exists(to) && Sha(to) == Sha(from)) Fs.Retry(() => File.Delete(from)); // (an identical copy is already in place)
                else { to = Mods.Free(to, Path.Combine(Mods.Disabled(docs), Path.GetDirectoryName(rel) ?? "")); Fs.Retry(() => File.Move(from, to)); }
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
        if (merged.Locked) throw new SetupException("The launcher does not move, rename or remove this file.");
        Engine.CheckWritable();
        lock (Gate)
        {
            var rec = Info(docs, merged) ?? throw new SetupException("This file is not a merge made by the launcher.");
            Recycle(merged.Path);
            foreach (var f in Kept(rec.Dir)) Recycle(f);
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
                    Fs.WriteAtomic(Path.Combine(r.Dir, "merge.json"), JsonSerializer.Serialize(r, SetupJson.Default.MergeRecord));
                    taken.Add(rel);
                    break;
                }
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { Engine.Log(e); }
    }
}
