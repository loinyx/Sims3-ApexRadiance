using System.Text;
using System.Text.RegularExpressions;

namespace ApexSetup;

// Load order's changes: the order the player drags turned into files and Resource.cfg lines the game reads in that order.
// Inside one PackedFile line the game reads by name (ModConflicts.LoadKey), so a section in an order of the player's own goes to
// Mods\Load order (Apex Launcher)\<section>\ with a number before each name, read by a line of the same Priority put just
// before the section's own (same priority, earlier line: read first). Every change is all or nothing and gives back what
// puts it back: the files where they were and Resource.cfg's bytes as they were. Locked and switched-off mods and the game's
// own files are never touched; every other line of Resource.cfg stays byte for byte.
public static class ModLoadOrder
{
    // (no spaces anywhere the launcher writes into Resource.cfg: the game splits its lines at spaces)
    public const string FolderName = "ApexLoadOrder";

    // one PackedFile line: its folder ('/', "" for Mods itself), how many folders under it it reads, and whether it is the
    // launcher's own
    public sealed record Section(int Index, int Priority, string Text, string Folder, int Depth, bool Launcher);

    // what puts a change back: the moved files (where each is now, where it was, relative to Mods with '/'), Resource.cfg's
    // bytes before (null: not changed) and the folders the change made
    public sealed record Undo(string Cfg, byte[]? Before, List<(ModFile Now, string Was)> Moves, List<string> Made);

    public static string CfgPath(string docs) => Path.Combine(docs, "Mods", "Resource.cfg");

    public static List<Section> Sections(string text) => ModConflicts.ParseResourceCfg(text).Select((d, i) => SectionOf(d, i)).ToList();

    static Section SectionOf(ModConflicts.Directive d, int i)
    {
        var parts = d.Text.Trim().Replace('\\', '/').Split('/');
        var dirs = parts[..^1];
        var fixedParts = dirs.TakeWhile(p => !p.Contains('*')).ToArray();
        var folder = string.Join('/', fixedParts);
        return new(i, d.Priority, d.Text, folder, dirs.Length - fixedParts.Length,
            fixedParts.Length == 2 && fixedParts[0].Equals(FolderName, StringComparison.OrdinalIgnoreCase) && dirs.Length == 2);
    }

    // a mod's path as Resource.cfg sees it: relative to Mods, '/'
    public static string Under(ModFile m) => Path.GetRelativePath("Mods", m.Rel).Replace('\\', '/');

    // ---------- the order ----------

    // wanted: every mod the page lists as read, in the order the player wants, each with the section (PackedFile line) it
    // goes under. Null when that is already the order the game reads (nothing written).
    public static Undo? Apply(string docs, IReadOnlyList<(ModFile Mod, int Section)> wanted)
    {
        Engine.CheckWritable();
        var cfg = Cfg.Read(docs);
        var dirs = ModConflicts.ParseResourceCfg(cfg.Text);
        var secs = dirs.Select((d, i) => SectionOf(d, i)).ToList();
        int? Reads(string rel) => ModConflicts.LoadKey(dirs, rel)?.Item2;
        var free = wanted.Where(w => !w.Mod.Locked && w.Mod.Enabled && w.Mod.FileType == "package").ToList();
        if (free.Any(w => w.Section < 0 || w.Section >= secs.Count)) throw new ArgumentException("No such section.");

        var to = new Dictionary<ModFile, string>();
        var buckets = new List<(string Folder, List<ModFile> Mods)>();
        var inserts = new List<(int Section, string Folder)>();
        List<ModFile> Bucket(string folder)
        {
            var b = buckets.FirstOrDefault(x => x.Folder.Equals(folder, StringComparison.OrdinalIgnoreCase)).Mods;
            if (b is null) buckets.Add((folder, b = []));
            return b;
        }
        foreach (var g in free.GroupBy(w => w.Section).OrderBy(g => g.Key))
        {
            var s = secs[g.Key];
            var list = g.Select(w => w.Mod).ToList();
            if (s.Launcher) { Bucket(s.Folder).AddRange(list); continue; }
            // where each would be with no order of the player's: where it is, or beside a mod of this section
            var plain = list.Select((m, k) => Reads(Under(m)) == s.Index && !InLauncher(m) ? Under(m) : Join(Beside(list, k), Plain(m))).ToList();
            if (plain.Zip(plain.Skip(1)).All(p => string.CompareOrdinal(ModConflicts.NameKey(p.First), ModConflicts.NameKey(p.Second)) < 0))
            {
                for (var k = 0; k < list.Count; k++) to[list[k]] = plain[k];
                continue;
            }
            var folder = FolderName + "/" + LauncherName(s);
            if (!secs.Any(x => x.Launcher && x.Folder.Equals(folder, StringComparison.OrdinalIgnoreCase))) inserts.Add((s.Index, folder));
            Bucket(folder).AddRange(list);

            // a mod that comes from another section goes next to the nearest one already read here
            string Beside(List<ModFile> l, int k)
            {
                foreach (var j in Enumerable.Range(0, k).Reverse().Concat(Enumerable.Range(k + 1, l.Count - k - 1)))
                    if (Reads(Under(l[j])) == s.Index && !InLauncher(l[j])) return Path.GetDirectoryName(Under(l[j]))!.Replace('\\', '/');
                return string.Join('/', s.Text.Replace('\\', '/').Split('/')[..^1].Select(p => p.Contains('*') ? FolderName : p));
            }
        }
        foreach (var (folder, mods) in buckets)
        {
            var width = Math.Max(3, mods.Count.ToString().Length);
            for (var k = 0; k < mods.Count; k++) to[mods[k]] = folder + "/" + (k + 1).ToString("D" + width) + " " + Plain(mods[k]);
        }

        foreach (var (s, folder) in inserts.OrderByDescending(i => i.Section)) cfg.Insert(cfg.DirLine[s], "PackedFile " + folder + "/*.package");
        // before any file moves: the game would read these mods in exactly the order wanted
        var after = ModConflicts.ParseResourceCfg(cfg.Text);
        var finals = free.Select(w => to.TryGetValue(w.Mod, out var t) ? t : Under(w.Mod)).ToList();
        var keys = finals.Select(f => ModConflicts.LoadKey(after, f)).ToList();
        if (keys.Any(k => k is null) || keys.Zip(keys.Skip(1)).Any(p => ModConflicts.CompareKey(p.First!.Value, p.Second!.Value) >= 0)
            || finals.Distinct(StringComparer.OrdinalIgnoreCase).Count() != finals.Count)
            throw new SetupException("Resource.cfg reads these folders in a way the launcher can't put in this order.");

        var moves = to.Where(x => !x.Value.Equals(Under(x.Key), StringComparison.Ordinal)).Select(x => (x.Key, x.Value)).ToList();
        if (moves.Count == 0 && inserts.Count == 0) return null;
        var made = Missing(docs, moves.Select(x => Path.GetDirectoryName(x.Value)!));
        var done = Shuffle(docs, moves);
        if (inserts.Count > 0)
            try { cfg.Write(); }
            catch { Shuffle(docs, done.Select(d => (d.Now, d.Was)).ToList()); Remove(made); throw; }
        return new(cfg.Path, inserts.Count > 0 ? cfg.Before : null, done, made);
    }

    static bool InLauncher(ModFile m) => Under(m).StartsWith(FolderName + "/", StringComparison.OrdinalIgnoreCase);

    // a mod's own name, without the number the launcher put before it
    static string Plain(ModFile m) => InLauncher(m) ? Regex.Replace(m.Name, @"^\d{3,} ", "") : m.Name;

    static string Join(string folder, string name) => folder.Length == 0 ? name : folder + "/" + name;

    // the launcher's folder for a section, by what it reads: Overrides, Overrides_subfolders, Packages_3levels (no spaces)
    static string LauncherName(Section s) => (s.Folder.Length == 0 ? "Mods" : s.Folder.Replace('/', '_').Replace(' ', '_'))
        + s.Depth switch { 0 => "", 1 => "_subfolders", var n => $"_{n}levels" };

    // the files to their new paths (relative to Mods, '/'): first under a name of the launcher's, then under their own, so
    // a folder numbered again never meets its own names; all or none
    static List<(ModFile Now, string Was)> Shuffle(string docs, List<(ModFile Mod, string To)> moves, bool back = false)
    {
        var now = moves.Select(x => x.Mod).ToList();
        var tag = Guid.NewGuid().ToString("N")[..8];
        try
        {
            for (var i = 0; i < moves.Count; i++)
            {
                now[i] = ModLibrary.Move(now[i], docs, Path.GetDirectoryName(moves[i].To.Replace('/', Path.DirectorySeparatorChar)) ?? "");
                now[i] = ModLibrary.Rename(now[i], $"~{tag} {i}.package");
            }
            for (var i = 0; i < moves.Count; i++) now[i] = ModLibrary.Rename(now[i], Path.GetFileName(moves[i].To));
        }
        catch when (!back)
        {
            Shuffle(docs, now.Select((m, i) => (m, Under(moves[i].Mod))).Where(x => !x.Item2.Equals(Under(x.m), StringComparison.Ordinal)).ToList(), back: true);
            throw;
        }
        return now.Select((m, i) => (m, Under(moves[i].Mod))).ToList();
    }

    // the folders (relative to Mods) that are not there yet, deepest last
    static List<string> Missing(string docs, IEnumerable<string> folders)
    {
        var o = new List<string>();
        foreach (var f in folders.Distinct(StringComparer.OrdinalIgnoreCase))
            for (var d = f; !string.IsNullOrEmpty(d); d = Path.GetDirectoryName(d))
            {
                var full = Path.Combine(docs, "Mods", d);
                if (!Directory.Exists(full) && !o.Contains(full, StringComparer.OrdinalIgnoreCase)) o.Add(full);
            }
        return o.OrderBy(p => p.Length).ToList();
    }

    // the folders a change made, when they are empty again
    static void Remove(List<string> made)
    {
        foreach (var d in made.OrderByDescending(p => p.Length))
            try { if (Directory.Exists(d) && !Directory.EnumerateFileSystemEntries(d).Any()) Directory.Delete(d); }
            catch (IOException) { } // (a folder left behind holds nothing)
    }

    // ---------- Resource.cfg's lines ----------

    // a section's priority: the number on its Priority line (one that governs the lines after it as well)
    public static Undo SetPriority(string docs, int section, int priority)
    {
        Engine.CheckWritable();
        var cfg = Cfg.Read(docs);
        var at = cfg.DirLine[section];
        var line = Enumerable.Range(0, at).Reverse().FirstOrDefault(i => cfg.IsPriority(i), -1);
        if (line >= 0) cfg.Lines[line] = new Regex(@"(?i)(priority\s+)[^\s#]+").Replace(cfg.Lines[line], "${1}" + priority, 1);
        else cfg.Insert(0, "Priority " + priority);
        cfg.Write();
        return new(cfg.Path, cfg.Before, [], []);
    }

    // a folder of Mods read at a priority of its own, and its subfolders one level down (as the usual Resource.cfg's
    // Packages): placed before the first Priority line that is lower, so the file stays in its order
    public static Undo NewFolder(string docs, string name, int priority)
    {
        Engine.CheckWritable();
        name = ModLibrary.CleanName(name).Replace(' ', '_'); // (Resource.cfg splits a line at spaces)
        if (name.Equals(FolderName, StringComparison.OrdinalIgnoreCase)) throw new SetupException("That name is taken by the launcher's own folder.");
        var cfg = Cfg.Read(docs);
        var lower = Enumerable.Range(0, cfg.Lines.Count).FirstOrDefault(i => cfg.IsPriority(i) && cfg.PriorityOf(i) < priority, -1);
        cfg.Insert(lower >= 0 ? lower : cfg.Lines.Count, "Priority " + priority, $"PackedFile {name}/*.package", $"PackedFile {name}/*/*.package");
        var made = Missing(docs, [name]);
        Directory.CreateDirectory(Path.Combine(docs, "Mods", name));
        try { cfg.Write(); }
        catch { Remove(made); throw; }
        return new(cfg.Path, cfg.Before, [], made);
    }

    // a package no line reads: a line for its folder, at the priority of the nearest folder above it the game reads (right
    // after that line), else at 500 at the end
    public static Undo MakeRead(string docs, ModFile m)
    {
        Engine.CheckWritable();
        var cfg = Cfg.Read(docs);
        var dirs = ModConflicts.ParseResourceCfg(cfg.Text);
        var rel = Under(m);
        if (ModConflicts.LoadKey(dirs, rel) is not null) throw new SetupException("The game already reads this file.");
        var folder = Path.GetDirectoryName(rel)?.Replace('\\', '/') ?? "";
        if (folder.Contains(' ')) throw new SetupException("Resource.cfg cannot name a folder with spaces in its name: rename the folder, or move the mod to one the game reads.");
        var line = "PackedFile " + Join(folder, "*.package");
        var at = -1;
        for (var up = folder; up.Length > 0 && at < 0;)
        {
            up = Path.GetDirectoryName(up)?.Replace('\\', '/') ?? "";
            at = dirs.FindIndex(d => d.Pattern.IsMatch(Join(up, "~.package")));
        }
        if (at >= 0) cfg.Insert(cfg.DirLine[at] + 1, line);
        else cfg.Insert(cfg.Lines.Count, "Priority 500", line);
        if (ModConflicts.LoadKey(ModConflicts.ParseResourceCfg(cfg.Text), rel) is null) throw new SetupException("Resource.cfg reads these folders in a way the launcher can't change.");
        cfg.Write();
        return new(cfg.Path, cfg.Before, [], []);
    }

    // ---------- putting back ----------

    public static void Back(string docs, Undo u)
    {
        Engine.CheckWritable();
        var done = Shuffle(docs, u.Moves.Select(x => (x.Now, x.Was)).ToList());
        if (u.Before is not null)
            try { File.WriteAllBytes(u.Cfg, u.Before); }
            catch { Shuffle(docs, done.Select(d => (d.Now, d.Was)).ToList()); throw; }
        Remove(u.Made);
    }

    // Resource.cfg as lines of its own bytes: Latin-1 keeps every byte as one character, so the lines not changed are written
    // back as they were (UTF-16 is read as UTF-16); what is added is UTF-8, with the file's own line ends
    sealed class Cfg
    {
        public string Path = "";
        public byte[] Before = [];
        public List<string> Lines = [];
        public List<int> DirLine = [];
        Encoding _view = Encoding.Latin1;
        bool _crlf;

        public static Cfg Read(string docs)
        {
            var c = new Cfg { Path = CfgPath(docs) };
            c.Before = File.Exists(c.Path) ? File.ReadAllBytes(c.Path) : throw new SetupException("The Mods folder has no Resource.cfg.");
            if (c.Before is [0xFF, 0xFE, ..]) c._view = Encoding.Unicode;
            else if (c.Before is [0xFE, 0xFF, ..]) c._view = Encoding.BigEndianUnicode;
            var all = c._view.GetString(c.Before);
            c._crlf = all.Contains("\r\n") || !all.Contains('\n');
            c.Lines = all.Split('\n').ToList();
            c.Index();
            return c;
        }

        void Index() => DirLine = Enumerable.Range(0, Lines.Count).Where(i => Word(i) is "packedfile").ToList();

        // a line as ParseResourceCfg reads it
        string Plain(int i) => Regex.Replace(Decoded(i), "#.*$", "").Trim();
        string? Word(int i) => Regex.Match(Plain(i), @"^(\w+)\s+(.+)$") is { Success: true } m ? m.Groups[1].Value.ToLowerInvariant() : null;
        public bool IsPriority(int i) => Word(i) is "priority";
        public int PriorityOf(int i) => int.TryParse(Regex.Match(Plain(i), @"^\w+\s+(.+)$").Groups[1].Value, out var n) ? n : 0;

        public string Text => string.Join('\n', Enumerable.Range(0, Lines.Count).Select(Decoded));
        // (as File.ReadAllText gives it: UTF-8, no byte order mark)
        string Decoded(int i) => (_view == Encoding.Latin1 ? Encoding.UTF8.GetString(Encoding.Latin1.GetBytes(Lines[i])) : Lines[i]).TrimStart('﻿');

        // lines before line `at` (Lines.Count: at the end)
        public void Insert(int at, params string[] lines)
        {
            var add = lines.Select(l => (_view == Encoding.Latin1 ? Encoding.Latin1.GetString(Encoding.UTF8.GetBytes(l)) : l) + (_crlf ? "\r" : "")).ToList();
            if (at >= Lines.Count)
            {
                // (after the last line: it ends first, and the file ends with a line end as before or as Windows writes)
                if (Lines[^1].Length > 0) { Lines[^1] += _crlf ? "\r" : ""; Lines.Add(""); }
                at = Lines.Count - 1;
            }
            else if (at == 0 && Lines[0] is var first && Bom(first) is { Length: > 0 } bom)
            {
                // (the byte order mark stays the file's first bytes)
                Lines[0] = first[bom.Length..];
                add[0] = bom + add[0];
            }
            Lines.InsertRange(at, add);
            Index();
        }
        string Bom(string line) => _view == Encoding.Latin1 ? (line.StartsWith("ï»¿") ? "ï»¿" : "") : line.StartsWith('﻿') ? "﻿" : "";

        public void Write() => File.WriteAllBytes(Path, _view.GetBytes(string.Join('\n', Lines)));
    }
}
