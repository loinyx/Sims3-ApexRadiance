using System.Text.Json;
using System.Text.RegularExpressions;

namespace ApexSetup;

// A file that looks like a newer version of a mod a merge keeps. Same: it is that mod as it is in the merge (already
// inside), so there is nothing to update.
public sealed record MergeUpdate(ModFile Merged, MergeRecord Merge, MergeOriginal Original, ModFile Newer, bool Same);

// The mods inside a merge, seen from outside it: each one as a mod the screens can read, its notes, and the files that look
// like newer versions of them.
public static partial class ModMerge
{
    // ---------- a mod inside a merge ----------

    // An original a merge keeps, as a mod: the kept copy's path, the place and name it had. Its contents, what it replaces
    // and its conflicts are read from that path like any mod's. Locked, so the library never moves, renames or removes the
    // kept copy: taking it out, replacing it and its notes go through the merge (Edit, EditNote).
    public static ModFile Inside(MergeRecord rec, MergeOriginal o)
    {
        var path = StorePath(rec, o);
        return new ModFile(path, o.Rel, o.Size, Mods.KindOf(path), true, true);
    }

    // The notes, creator, tags and kinds of a mod inside a merge (MergeOriginal.Note), changed where they are kept: the
    // merge's record. They go back to the mod when it is taken out.
    public static void EditNote(string docs, ModFile merged, string rel, Action<ModNote> change)
    {
        if (!Monitor.TryEnter(Gate)) throw new SetupException("A merge is being made. Try again when it is done.");
        try
        {
            var rec = Info(docs, merged) ?? throw new SetupException("This file is not a merge made by the launcher.");
            var o = rec.Originals.FirstOrDefault(x => x.Rel.Equals(rel, StringComparison.OrdinalIgnoreCase)) ?? throw new SetupException("That mod is no longer in this merge.");
            // (the whole note copied, whatever it holds: what is not changed here is kept)
            static string Json(ModNote n) => JsonSerializer.Serialize(n, SetupJson.Default.ModNote);
            var n = o.Note is null ? new ModNote() : JsonSerializer.Deserialize(Json(o.Note), SetupJson.Default.ModNote)!;
            change(n);
            n.Notes = string.IsNullOrWhiteSpace(n.Notes) ? null : n.Notes.Trim();
            n.Creator = string.IsNullOrWhiteSpace(n.Creator) ? null : n.Creator.Trim();
            if (n.Creator is { Length: > 60 }) throw new SetupException("A creator's name can be up to 60 characters.");
            if (n.Tags is { Count: 0 }) n.Tags = null;
            if (n.Kinds is { Count: 0 }) n.Kinds = null;
            var was = o.Note;
            o.Note = Json(n) == Json(new ModNote()) ? null : n;
            try { Save(rec); }
            catch { o.Note = was; throw; }
        }
        finally { Monitor.Exit(Gate); }
    }

    // ---------- newer versions ----------

    // a version at the end of a file name: "v2", "_v1.4b", "ver 3", " 1.2.1", or the " (2)" Windows and the launcher add to a
    // second copy. A bare number is none ("Hair 1" and "Hair 2" are two mods).
    static readonly Regex VersionEnd = new(@"(?:[\s_.-]*\(\d+\)|[\s_.-]*(?<![a-z])v(?:er(?:sion)?)?[\s_.-]*\d+(?:[._]\d+)*[a-z]?|[\s_-]+\d+(?:\.\d+)+[a-z]?)$", RegexOptions.IgnoreCase | RegexOptions.CultureInvariant);

    // a file name without its version, case, spaces and underscores, and the version's numbers when it has one
    public static string NameKeyOf(string file) => NameKey(file).Key;
    static (string Key, int[]? Version) NameKey(string file)
    {
        var n = Path.GetFileNameWithoutExtension(file);
        int[]? version = null;
        while (VersionEnd.Match(n) is { Success: true, Index: > 0 } m)
        {
            if (!m.Value.Contains('(')) version ??= Regex.Matches(m.Value, @"\d+").Select(x => int.TryParse(x.Value, out var v) ? v : int.MaxValue).ToArray();
            n = n[..m.Index];
        }
        return (new string(n.Where(c => c is not (' ' or '_')).ToArray()).ToLowerInvariant(), version);
    }

    static int Compare(int[] a, int[] b)
    {
        for (var i = 0; i < Math.Max(a.Length, b.Length); i++)
            if ((i < a.Length ? a[i] : 0).CompareTo(i < b.Length ? b[i] : 0) is var c and not 0) return c;
        return 0;
    }

    // An update found earlier, as the mods are now (its files may have been moved, renamed or turned off since, or the update
    // made); null when it no longer holds.
    public static MergeUpdate? Now(string docs, MergeUpdate u, IReadOnlyCollection<ModFile> mods) =>
        Records(docs).FirstOrDefault(r => r.Dir.Equals(u.Merge.Dir, StringComparison.OrdinalIgnoreCase)) is { } rec
            && rec.Originals.FirstOrDefault(o => o.Rel.Equals(u.Original.Rel, StringComparison.OrdinalIgnoreCase) && o.Sha256 == u.Original.Sha256) is { } original
            && mods.OrderByDescending(m => m.Enabled).FirstOrDefault(m => m.Rel.Equals(rec.Merged, StringComparison.OrdinalIgnoreCase)) is { } merged
            && mods.FirstOrDefault(m => m.Enabled && m.Rel.Equals(u.Newer.Rel, StringComparison.OrdinalIgnoreCase)) is { } newer
            ? new(merged, rec, original, newer, u.Same) : null;

    // For every merge whose file is in Mods, the candidates (mods of the list that are on, or files from outside Mods as
    // Outside gives them) that look like a newer version of one of its mods:
    //  - the same file name once versions, case, spaces and underscores are left out, with some resource in common; or
    //  - with the game's index: nearly all of its own resources (those the game does not ship: two default replacements
    //    of one game item are two mods) are that mod's, and most of that mod's are its.
    // Newer by the version in the names when both have one, else by the file's date. The very same file, or one whose
    // resources are all in the mod unchanged, comes back as Same. One candidate per mod, the newest. Unsure: nothing.
    public static List<MergeUpdate> Updates(string docs, IEnumerable<ModFile> candidates, GameIndex? game = null)
    {
        var found = new List<(MergeUpdate U, int[]? Version, DateTime Written)>();
        var kept = new List<(ModFile Merged, MergeRecord Rec, MergeOriginal O, string Path, string Name)>();
        foreach (var r in Records(docs).ToList())
        {
            var (on, off) = (Path.Combine(docs, r.Merged), Path.Combine(Mods.Disabled(docs), UnderMods(r.Merged)));
            if ((File.Exists(on) ? on : File.Exists(off) ? off : null) is not { } path) continue;
            var merged = new ModFile(path, r.Merged, r.Size, ModKind.Package, path == on, false);
            foreach (var o in r.Originals)
                try { if (StorePath(r, o) is var p && File.Exists(p)) kept.Add((merged, r, o, p, NameKey(o.Rel).Key)); }
                catch (SetupException) { }
        }
        if (kept.Count == 0) return [];
        var mergedFiles = kept.Select(k => k.Merged.Path).ToHashSet(StringComparer.OrdinalIgnoreCase);

        using var read = new Contents();
        var indexes = new Dictionary<string, Dictionary<(uint, uint, ulong), ModConflicts.Entry>?>(StringComparer.OrdinalIgnoreCase);
        // a package's resources by key (name maps and manifests are no content); null when it can't be read
        Dictionary<(uint, uint, ulong), ModConflicts.Entry>? Index(string path)
        {
            if (indexes.TryGetValue(path, out var d)) return d;
            if (Probe(path).Item1 is { } entries)
            {
                d = new();
                foreach (var e in entries.Where(e => Content((e.Type, e.Group, e.Instance)))) d.TryAdd((e.Type, e.Group, e.Instance), e);
            }
            return indexes[path] = d;
        }
        bool Own((uint Type, uint Group, ulong Instance) k) => game is null || !game.Contains(k.Type, k.Group, k.Instance);
        // by content: which kept mod holds each resource the game does not ship (read once, when the first candidate needs it)
        Dictionary<(uint, uint, ulong), List<int>>? holders = null;
        Dictionary<(uint, uint, ulong), List<int>> Holders()
        {
            if (holders is not null) return holders;
            holders = new();
            for (var i = 0; i < kept.Count; i++)
                foreach (var k in Index(kept[i].Path)?.Keys.Where(Own) ?? [])
                    (holders.TryGetValue(k, out var l) ? l : holders[k] = new()).Add(i);
            return holders;
        }

        foreach (var c in candidates)
            try
            {
                if (c.FileType != "package" || c.Locked || !c.Enabled || !File.Exists(c.Path) || mergedFiles.Contains(c.Path) || Keeps(docs, c.Path)) continue;
                var info = new FileInfo(c.Path);
                // the very same file
                string? sha = null;
                if (kept.FindIndex(k => k.O.Size == info.Length && k.O.Sha256 == (sha ??= Sha(c.Path))) is >= 0 and var same)
                {
                    found.Add((new(kept[same].Merged, kept[same].Rec, kept[same].O, c, true), null, default));
                    continue;
                }
                if (Index(c.Path) is not { Count: > 0 } mine) continue;
                var (key, version) = NameKey(c.Name);
                var at = -1;
                var named = key.Length < 3 ? [] : Enumerable.Range(0, kept.Count).Where(i => kept[i].Name == key && Index(kept[i].Path) is { } theirs && mine.Keys.Any(theirs.ContainsKey)).ToList();
                if (named.Count == 1) at = named[0];
                else if (named.Count == 0 && game is not null && mine.Keys.Where(Own).ToList() is { Count: >= 3 } own)
                {
                    var hits = own.SelectMany(k => Holders().GetValueOrDefault(k) ?? []).GroupBy(i => i).Select(g => (I: g.Key, N: g.Count())).OrderByDescending(x => x.N).ToList();
                    if (hits.Count > 0 && hits[0].N * 5 >= own.Count * 4 && (hits.Count == 1 || hits[1].N * 2 < own.Count)
                        && hits[0].N * 2 >= Index(kept[hits[0].I].Path)!.Keys.Count(Own)) at = hits[0].I;
                }
                if (at < 0) continue;
                var k = kept[at];
                var its = Index(k.Path)!;
                if (mine.All(x => its.TryGetValue(x.Key, out var e) && read.Same(c.Path, x.Value, k.Path, e)))
                {
                    found.Add((new(k.Merged, k.Rec, k.O, c, true), null, default));
                    continue;
                }
                var theirVersion = NameKey(k.O.Rel).Version;
                var newer = version is not null && theirVersion is not null && Compare(version, theirVersion) != 0 ? Compare(version, theirVersion) > 0 : info.LastWriteTimeUtc > File.GetLastWriteTimeUtc(k.Path);
                if (newer) found.Add((new(k.Merged, k.Rec, k.O, c, false), version, info.LastWriteTimeUtc));
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { Engine.Log(e); }

        return found.Where(f => f.U.Same).Select(f => f.U)
            .Concat(found.Where(f => !f.U.Same).GroupBy(f => f.U.Original).Select(g => g.Aggregate((a, b) =>
                (a.Version is not null && b.Version is not null && Compare(a.Version, b.Version) is var c and not 0 ? c > 0 : a.Written >= b.Written) ? a : b).U)).ToList();
    }

    // The newer files into their merge, as one change: each takes the place of its older version, which goes to the Recycle
    // Bin; the order, notes and tags stay. The updates of one merge.
    public static TakeOutResult Update(string docs, IReadOnlyCollection<MergeUpdate> updates, IProgress<MergeProgress>? progress = null, CancellationToken ct = default)
    {
        var edit = new MergeEdit();
        foreach (var u in updates.Where(u => !u.Same && u.Merged.Rel.Equals(updates.First().Merged.Rel, StringComparison.OrdinalIgnoreCase))) edit.Replace[u.Original.Rel] = u.Newer;
        if (edit.Replace.Count == 0) throw new SetupException("There is nothing to update.");
        return Edit(docs, updates.First().Merged, edit, progress, ct);
    }
}
