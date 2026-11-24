using System.Runtime.InteropServices;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace ApexSetup;

// What the player adds to mods in the launcher: notes, a picture, its page and tags (a name and a colour, made by the player). Kept
// by the mod's place as the game sees it (ModFile.Rel), in %LOCALAPPDATA%\Apex Launcher\mod-notes.json and
// mod-pictures\; moved along when the launcher moves or renames the mod. Nothing is written next to the mod.
public sealed class ModNote
{
    public string? Notes { get; set; }
    public string? Picture { get; set; } // a file name in mod-pictures
    public List<string>? Tags { get; set; }
    public string? Creator { get; set; } // set by the player; else ModCreators guesses it from the file name
    public List<string>? Kinds { get; set; } // what it is, chosen by the player; else what the package index says
    public string? Link { get; set; } // the mod's page
}

public sealed class ModTag
{
    public string Name { get; set; } = "";
    public string Color { get; set; } = "#7F77DD";
}

public sealed class ModNotesFile
{
    public Dictionary<string, ModNote> Mods { get; set; } = [];
    public List<ModTag> Tags { get; set; } = [];
}

public static class ModNotes
{
    // the colours a tag can have
    public static readonly string[] Colors = ["#7F77DD", "#5B8DEF", "#3FB6A8", "#7DBE9A", "#E0C24F", "#E0A84F", "#E8716B", "#D27BB4"];

    static string FilePath => Path.Combine(Paths.Data, "mod-notes.json");
    public static string PictureDir => Path.Combine(Paths.Data, "mod-pictures");
    static ModNotesFile? _file;

    static ModNotesFile Data
    {
        get
        {
            if (_file is not null) return _file;
            ModNotesFile? f = null;
            var readable = true;
            try { if (File.Exists(FilePath)) f = JsonSerializer.Deserialize(File.ReadAllText(FilePath), SetupJson.Default.ModNotesFile); }
            // (a damaged file is kept beside as .corrupt before anything is written over it)
            catch (JsonException e) { Engine.Log(e); try { File.Copy(FilePath, FilePath + ".corrupt", overwrite: true); } catch (Exception c) when (c is IOException or UnauthorizedAccessException) { readable = false; } }
            // (a file locked for a moment: read again next time, and nothing is saved over it meanwhile)
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { Engine.Log(e); readable = false; }
            f ??= new();
            f.Mods = new Dictionary<string, ModNote>(f.Mods ?? [], StringComparer.OrdinalIgnoreCase);
            f.Tags ??= [];
            return readable ? _file = f : f;
        }
    }
    static Dictionary<string, ModNote> All => Data.Mods;

    [System.Runtime.CompilerServices.MethodImpl(System.Runtime.CompilerServices.MethodImplOptions.Synchronized)]
    public static ModNote? Get(string rel) => All.TryGetValue(rel, out var n) ? n : null;
    [System.Runtime.CompilerServices.MethodImpl(System.Runtime.CompilerServices.MethodImplOptions.Synchronized)]
    public static string? PicturePath(string rel) => Get(rel)?.Picture is { } p && File.Exists(Path.Combine(PictureDir, p)) ? Path.Combine(PictureDir, p) : null;

    [System.Runtime.CompilerServices.MethodImpl(System.Runtime.CompilerServices.MethodImplOptions.Synchronized)]
    public static void SetNotes(string rel, string? notes)
    {
        notes = string.IsNullOrWhiteSpace(notes) ? null : notes.Trim();
        if (Get(rel)?.Notes == notes) return;
        Edit(rel, n => n.Notes = notes);
    }

    // a copy of the picture the player chose (the original may move or go); null removes it
    [System.Runtime.CompilerServices.MethodImpl(System.Runtime.CompilerServices.MethodImplOptions.Synchronized)]
    public static void SetPicture(string rel, string? source)
    {
        var old = Get(rel)?.Picture;
        string? name = null;
        if (source is not null)
        {
            if (new FileInfo(source).Length > 20 << 20) throw new SetupException("That picture is too big. Use one under 20 MB.");
            Directory.CreateDirectory(PictureDir);
            name = Guid.NewGuid().ToString("N") + Path.GetExtension(source).ToLowerInvariant();
            File.Copy(source, Path.Combine(PictureDir, name));
        }
        Edit(rel, n => n.Picture = name);
        if (old is not null) TryDelete(Path.Combine(PictureDir, old));
    }

    [System.Runtime.CompilerServices.MethodImpl(System.Runtime.CompilerServices.MethodImplOptions.Synchronized)]
    public static void SetCreator(string rel, string? creator)
    {
        creator = string.IsNullOrWhiteSpace(creator) ? null : creator.Trim();
        if (creator is { Length: > 60 }) throw new SetupException("A creator's name can be up to 60 characters.");
        if (Get(rel)?.Creator == creator) return;
        Edit(rel, n => n.Creator = creator);
    }

    // what a mod is, chosen by the player (null or empty: back to what the launcher reads in the package)
    [System.Runtime.CompilerServices.MethodImpl(System.Runtime.CompilerServices.MethodImplOptions.Synchronized)]
    public static void SetKinds(string rel, IEnumerable<string>? kinds)
    {
        var list = kinds?.Where(k => k.Length > 0).Distinct().ToList();
        Edit(rel, n => n.Kinds = list is { Count: > 0 } ? list : null);
    }

    // the mod's page on the web; null removes it
    [System.Runtime.CompilerServices.MethodImpl(System.Runtime.CompilerServices.MethodImplOptions.Synchronized)]
    public static void SetLink(string rel, string? link)
    {
        link = string.IsNullOrWhiteSpace(link) ? null : WebLink(link) ?? throw new SetupException("Use a web address, such as https://example.com.");
        if (Get(rel)?.Link == link) return;
        Edit(rel, n => n.Link = link);
    }

    // (checked again when read: the notes file can be changed by hand, and what is in it is opened)
    public static string? LinkOf(string rel) => WebLink(Get(rel)?.Link);

    // a web address the player typed or pasted, "https://" put before one without it; null when it is anything else
    // (a file, a script, a program's own address, a name and password in front of the site)
    public static string? WebLink(string? text)
    {
        text = text?.Trim() ?? "";
        if (text.Length is 0 or > 2000) return null;
        if (!text.Contains("://")) text = "https://" + text;
        return Uri.TryCreate(text, UriKind.Absolute, out var u) && u.Scheme is "http" or "https" && u.UserInfo.Length == 0 && u.Host.Contains('.') && u.AbsoluteUri.Length <= 2000 ? u.AbsoluteUri : null;
    }

    // ---- tags ----

    public static IReadOnlyList<ModTag> Tags => Data.Tags;
    [System.Runtime.CompilerServices.MethodImpl(System.Runtime.CompilerServices.MethodImplOptions.Synchronized)]
    public static IReadOnlyList<string> TagsOf(string rel) => Get(rel)?.Tags ?? [];
    [System.Runtime.CompilerServices.MethodImpl(System.Runtime.CompilerServices.MethodImplOptions.Synchronized)]
    public static string ColorOf(string tag) => Data.Tags.FirstOrDefault(t => t.Name.Equals(tag, StringComparison.OrdinalIgnoreCase))?.Color ?? Colors[0];

    [System.Runtime.CompilerServices.MethodImpl(System.Runtime.CompilerServices.MethodImplOptions.Synchronized)]
    public static void CreateTag(string name, string color)
    {
        name = TagName(name);
        if (Data.Tags.Any(t => t.Name.Equals(name, StringComparison.OrdinalIgnoreCase))) throw new SetupException("A tag with that name already exists.");
        Data.Tags.Add(new ModTag { Name = name, Color = Colors.Contains(color) ? color : Colors[0] });
        Save();
    }

    [System.Runtime.CompilerServices.MethodImpl(System.Runtime.CompilerServices.MethodImplOptions.Synchronized)]
    public static void EditTag(string name, string newName, string color)
    {
        newName = TagName(newName);
        var tag = Data.Tags.FirstOrDefault(t => t.Name.Equals(name, StringComparison.OrdinalIgnoreCase)) ?? throw new SetupException("That tag no longer exists.");
        if (!newName.Equals(name, StringComparison.OrdinalIgnoreCase) && Data.Tags.Any(t => t.Name.Equals(newName, StringComparison.OrdinalIgnoreCase)))
            throw new SetupException("A tag with that name already exists.");
        tag.Name = newName;
        if (Colors.Contains(color)) tag.Color = color;
        foreach (var n in All.Values.Where(n => n.Tags is not null))
            n.Tags = n.Tags!.Select(t => t.Equals(name, StringComparison.OrdinalIgnoreCase) ? newName : t).ToList();
        Save();
    }

    [System.Runtime.CompilerServices.MethodImpl(System.Runtime.CompilerServices.MethodImplOptions.Synchronized)]
    public static void DeleteTag(string name)
    {
        Data.Tags.RemoveAll(t => t.Name.Equals(name, StringComparison.OrdinalIgnoreCase));
        foreach (var (rel, n) in All.ToList())
            if (n.Tags?.RemoveAll(t => t.Equals(name, StringComparison.OrdinalIgnoreCase)) > 0) Edit(rel, _ => { }, save: false);
        Save();
    }

    [System.Runtime.CompilerServices.MethodImpl(System.Runtime.CompilerServices.MethodImplOptions.Synchronized)]
    public static void SetTag(string rel, string tag, bool on)
    {
        if (TagsOf(rel).Contains(tag, StringComparer.OrdinalIgnoreCase) == on) return;
        Edit(rel, n => n.Tags = on ? [.. n.Tags ?? [], tag] : n.Tags!.Where(t => !t.Equals(tag, StringComparison.OrdinalIgnoreCase)).ToList());
    }

    static string TagName(string name)
    {
        name = name.Trim();
        if (name.Length is 0 or > 40) throw new SetupException("A tag name must be 1 to 40 characters.");
        return name;
    }

    // the mod moved or was renamed (a folder: every mod under it); its notes and the profiles follow
    [System.Runtime.CompilerServices.MethodImpl(System.Runtime.CompilerServices.MethodImplOptions.Synchronized)]
    public static void Move(string oldRel, string newRel, bool folder = false)
    {
        ModProfiles.Moved(oldRel, newRel, folder);
        var moves = All.Keys.Where(k => folder ? k.StartsWith(oldRel + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase) : k.Equals(oldRel, StringComparison.OrdinalIgnoreCase)).ToList();
        if (moves.Count == 0) return;
        foreach (var k in moves)
        {
            var n = All[k];
            All.Remove(k);
            All[newRel + k[oldRel.Length..]] = n;
        }
        Save();
    }

    // a mod leaves the list (merged into another): its note without deleting its picture, for Put to bring back
    [System.Runtime.CompilerServices.MethodImpl(System.Runtime.CompilerServices.MethodImplOptions.Synchronized)]
    public static ModNote? Take(string rel)
    {
        if (Get(rel) is not { } n) return null;
        All.Remove(rel);
        Save();
        return n;
    }

    [System.Runtime.CompilerServices.MethodImpl(System.Runtime.CompilerServices.MethodImplOptions.Synchronized)]
    public static void Put(string rel, ModNote n)
    {
        All[rel] = n;
        Save();
    }

    [System.Runtime.CompilerServices.MethodImpl(System.Runtime.CompilerServices.MethodImplOptions.Synchronized)]
    public static void Forget(string rel)
    {
        if (Get(rel) is not { } n) return;
        if (n.Picture is not null) TryDelete(Path.Combine(PictureDir, n.Picture));
        All.Remove(rel);
        Save();
    }

    static void Edit(string rel, Action<ModNote> change, bool save = true)
    {
        var n = Get(rel) ?? new ModNote();
        change(n);
        if (n.Tags is { Count: 0 }) n.Tags = null;
        if (n.Kinds is { Count: 0 }) n.Kinds = null;
        if (n.Notes is null && n.Picture is null && n.Tags is null && n.Creator is null && n.Kinds is null && n.Link is null) All.Remove(rel); else All[rel] = n;
        if (save) Save();
    }

    static void Save()
    {
        if (_file is null) return; // (the file could not be read: never write it over with nothing)
        Directory.CreateDirectory(Paths.Data);
        var tmp = FilePath + ".tmp";
        File.WriteAllText(tmp, JsonSerializer.Serialize(Data, SetupJson.Default.ModNotesFile));
        File.Move(tmp, FilePath, overwrite: true);
    }

    static void TryDelete(string p) { try { File.Delete(p); } catch (Exception e) when (e is IOException or UnauthorizedAccessException) { } }

    // for tests: read the files again
    public static void Reload() { _file = null; ModProfiles.Reload(); }
}

// The player's folders under Mods, and moving, renaming and removing mods in them. Only the player's own packages under
// Mods: a locked mod (installed by the launcher, or in Game\Bin) is never moved, renamed or removed
// here. A switched-off mod lives at the same place under Mods\Disabled (Apex Launcher), and goes along.
public static partial class ModLibrary
{
    // every folder under Mods (relative to Mods, "Packages\NRaas"), without the launcher's own switched-off folder
    public static List<string> Folders(string docs)
    {
        var mods = Path.Combine(docs, "Mods");
        if (!Directory.Exists(mods)) return [];
        try
        {
            return Directory.EnumerateDirectories(mods, "*", new EnumerationOptions { RecurseSubdirectories = true, IgnoreInaccessible = true, AttributesToSkip = 0 })
                .Select(d => Path.GetRelativePath(mods, d))
                .Where(r => !new[] { Mods.DisabledName, ModMerge.FolderName }.Any(n => r.Equals(n, StringComparison.OrdinalIgnoreCase) || r.StartsWith(n + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase)))
                .Order(StringComparer.OrdinalIgnoreCase).ToList();
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return []; }
    }

    // a folder or file name the player typed: one name, no path, nothing Windows refuses
    public static string CleanName(string name)
    {
        name = name.Trim().TrimEnd('.');
        if (name.Length == 0 || name.Length > 120 || name.IndexOfAny(Path.GetInvalidFileNameChars()) >= 0 || name is "." or ".."
            || Regex.IsMatch(name, @"^(con|prn|aux|nul|com\d|lpt\d)(\..*)?$", RegexOptions.IgnoreCase))
            throw new SetupException("That name can't be used for a file or folder.");
        if (name.Equals(Mods.DisabledName, StringComparison.OrdinalIgnoreCase) || name.Equals(ModMerge.FolderName, StringComparison.OrdinalIgnoreCase)) throw new SetupException("That name is taken by the launcher's own folder.");
        return name;
    }

    static string Under(string docs, string rel) // rel relative to Mods, never outside it
    {
        var mods = Path.GetFullPath(Path.Combine(docs, "Mods"));
        var full = Path.GetFullPath(Path.Combine(mods, rel));
        if (!full.StartsWith(mods + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase) && !full.Equals(mods, StringComparison.OrdinalIgnoreCase))
            throw new SetupException("That folder is outside Mods.");
        return full;
    }

    public static string CreateFolder(string docs, string parentRel, string name)
    {
        Engine.CheckWritable();
        var rel = Path.Combine(parentRel, CleanName(name));
        var full = Under(docs, rel);
        if (Directory.Exists(full)) throw new SetupException("A folder with that name already exists here.");
        Directory.CreateDirectory(full);
        return rel;
    }

    // the folder renamed, and its switched-off copy with it; the new relative path
    public static string RenameFolder(string docs, string rel, string name, IReadOnlyCollection<string>? locked = null)
    {
        Engine.CheckWritable();
        var to = Path.Combine(Path.GetDirectoryName(rel) ?? "", CleanName(name));
        if (to.Equals(rel, StringComparison.Ordinal)) return rel;
        MoveWithOffCopy(docs, rel, to, caseOnly: to.Equals(rel, StringComparison.OrdinalIgnoreCase), locked: locked);
        return to;
    }

    // a folder and its switched-off copy to another place under Mods, both or neither (else the mods in the copy left behind
    // come back under the old name when turned on); its notes and merges follow
    static void MoveWithOffCopy(string docs, string rel, string to, bool caseOnly = false, IReadOnlyCollection<string>? locked = null)
    {
        var from = Under(docs, rel);
        var target = Under(docs, to);
        var offFrom = Path.Combine(Mods.Disabled(docs), rel);
        var offTo = Path.Combine(Mods.Disabled(docs), to);
        var off = Directory.Exists(offFrom);
        // (files the launcher installed stay where its install record says: moved, an uninstall misses them and a repair
        // puts a second copy that loads twice)
        bool Inside(string f, string dir) => f.StartsWith(dir.TrimEnd(Path.DirectorySeparatorChar) + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase);
        if (locked is not null && locked.Any(f => Inside(f, from) || off && Inside(f, offFrom)))
            throw new SetupException("This folder holds files the launcher installed. Turn them on or off in Components instead.");
        // (a folder Resource.cfg loads, moved where it doesn't: the game would stop loading every mod in it)
        if (!caseOnly && LoadedBy(docs) is { } loaded && Directory.Exists(from))
        {
            var mods = Path.Combine(docs, "Mods");
            foreach (var f in Directory.EnumerateFiles(from, "*.package", SearchOption.AllDirectories))
            {
                var now = Path.GetRelativePath(mods, f);
                var then = Path.GetRelativePath(mods, Path.Combine(target, Path.GetRelativePath(from, f)));
                if (loaded(now) && !loaded(then)) throw new SetupException("The game wouldn't load the mods in this folder there: Mods\\Resource.cfg doesn't read that place.");
            }
        }
        if (!caseOnly && (Directory.Exists(target) || off && Directory.Exists(offTo))) throw new SetupException("A folder with that name already exists here.");
        Directory.CreateDirectory(Path.GetDirectoryName(target)!);
        Directory.Move(from, target);
        if (off)
            try
            {
                Directory.CreateDirectory(Path.GetDirectoryName(offTo)!);
                Directory.Move(offFrom, offTo);
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { Directory.Move(target, from); throw; }
        ModNotes.Move(Path.Combine("Mods", rel), Path.Combine("Mods", to), folder: true);
        ModMerge.Moved(docs, Path.Combine("Mods", rel), Path.Combine("Mods", to), folder: true);
    }

    // only an empty folder (no file in it or in its switched-off copy): a mod is never deleted with its folder
    public static void DeleteFolder(string docs, string rel)
    {
        Engine.CheckWritable();
        var full = Under(docs, rel);
        var off = Path.Combine(Mods.Disabled(docs), rel);
        if (HasFiles(full) || HasFiles(off)) throw new SetupException("This folder still has files. Move or remove them first.");
        if (Directory.Exists(full)) Directory.Delete(full, true);
        if (Directory.Exists(off)) Directory.Delete(off, true);
        static bool HasFiles(string d) => Directory.Exists(d) && Directory.EnumerateFiles(d, "*", SearchOption.AllDirectories).Any();
    }

    static void Mine(ModFile m)
    {
        if (m.Locked || m.FileType != "package") throw new SetupException("The launcher does not move, rename or remove this file.");
        Engine.CheckWritable();
    }

    // to another folder under Mods (relative to Mods; "" is Mods itself), on or off as it was
    public static ModFile Move(ModFile m, string docs, string folderRel)
    {
        Mine(m);
        var rel = Path.Combine("Mods", folderRel, m.Name);
        if (rel.Equals(m.Rel, StringComparison.OrdinalIgnoreCase)) return m;
        Under(docs, folderRel);
        string onDir = Path.Combine(docs, "Mods", folderRel), offDir = Path.Combine(Mods.Disabled(docs), folderRel);
        var target = m.Enabled ? Mods.Free(Path.Combine(onDir, m.Name), offDir) : Mods.Free(Path.Combine(offDir, m.Name), onDir);
        Directory.CreateDirectory(Path.GetDirectoryName(target)!);
        File.Move(m.Path, target);
        var moved = m with { Path = target, Rel = Path.Combine("Mods", folderRel, Path.GetFileName(target)) };
        ModNotes.Move(m.Rel, moved.Rel);
        ModMerge.Moved(docs, m.Rel, moved.Rel);
        return moved;
    }

    // a new file name, the extension kept
    public static ModFile Rename(ModFile m, string name)
    {
        Mine(m);
        var ext = Path.GetExtension(m.Path);
        name = CleanName(name);
        if (!name.EndsWith(ext, StringComparison.OrdinalIgnoreCase)) name += ext;
        if (name.Equals(m.Name, StringComparison.Ordinal)) return m;
        var target = Path.Combine(Path.GetDirectoryName(m.Path)!, name);
        var rel = Path.Combine(Path.GetDirectoryName(m.Rel) ?? "", name);
        // (the name is taken on the other side, on or off, too: the two would share one Rel)
        var docs = ModMerge.DocsOf(m);
        var other = docs is null ? null : m.Enabled ? Path.Combine(Mods.Disabled(docs), Path.GetRelativePath("Mods", rel)) : Path.Combine(docs, rel);
        if ((File.Exists(target) || other is not null && File.Exists(other)) && !name.Equals(m.Name, StringComparison.OrdinalIgnoreCase))
            throw new SetupException("A file with that name already exists here.");
        File.Move(m.Path, target);
        var moved = m with { Path = target, Rel = rel };
        ModNotes.Move(m.Rel, moved.Rel);
        if (docs is not null) ModMerge.Moved(docs, m.Rel, moved.Rel);
        return moved;
    }

    // to the Recycle Bin, so a mistake can be undone from there
    public static void Remove(ModFile m)
    {
        Mine(m);
        Recycle(m.Path);
        ModNotes.Forget(m.Rel);
    }

    // copies of the files the player picked, into a folder under Mods; the new paths
    public static List<string> Add(string docs, string folderRel, IEnumerable<string> files)
    {
        Engine.CheckWritable();
        var dir = Under(docs, folderRel);
        Directory.CreateDirectory(dir);
        var added = new List<string>();
        foreach (var f in files.Where(f => f.EndsWith(".package", StringComparison.OrdinalIgnoreCase)))
        {
            var target = Mods.Free(Path.Combine(dir, Path.GetFileName(f)));
            File.Copy(f, target);
            added.Add(target);
        }
        return added;
    }

    public static void Recycle(string path)
    {
        if (!File.Exists(path)) return;
        // (Windows asks before it deletes for good a file the Recycle Bin can't take: a network folder, a file too big)
        var op = new SHFILEOPSTRUCT { wFunc = FO_DELETE, pFrom = Path.GetFullPath(path) + "\0\0", fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT | FOF_WANTNUKEWARNING };
        var r = SHFileOperation(ref op);
        if (r != 0 || op.fAnyOperationsAborted || File.Exists(path)) throw new SetupException("Windows couldn't move the file to the Recycle Bin.");
    }

    const uint FO_DELETE = 3;
    const ushort FOF_SILENT = 0x4, FOF_NOCONFIRMATION = 0x10, FOF_ALLOWUNDO = 0x40, FOF_NOERRORUI = 0x400, FOF_WANTNUKEWARNING = 0x4000;

    [StructLayout(LayoutKind.Sequential, CharSet = CharSet.Unicode)]
    struct SHFILEOPSTRUCT
    {
        public IntPtr hwnd;
        public uint wFunc;
        public string pFrom;
        public string? pTo;
        public ushort fFlags;
        [MarshalAs(UnmanagedType.Bool)] public bool fAnyOperationsAborted;
        public IntPtr hNameMappings;
        public string? lpszProgressTitle;
    }

    [DllImport("shell32.dll", CharSet = CharSet.Unicode)]
    static extern int SHFileOperation(ref SHFILEOPSTRUCT op);

    // Which places under Mods the game loads packages from, read from Mods\Resource.cfg's PackedFile lines
    // ("Packages/*/*.package"). Null when there is no Resource.cfg (nothing to judge by).
    public static Func<string, bool>? LoadedBy(string docs)
    {
        var cfg = Path.Combine(docs, "Mods", "Resource.cfg");
        if (!File.Exists(cfg)) return null;
        var patterns = new List<Regex>();
        try
        {
            foreach (var line in File.ReadAllLines(cfg))
            {
                var t = line.Trim();
                if (!t.StartsWith("PackedFile", StringComparison.OrdinalIgnoreCase)) continue;
                var glob = t["PackedFile".Length..].Trim().Replace('\\', '/');
                if (glob.Length == 0) continue;
                patterns.Add(new Regex("^" + Regex.Escape(glob).Replace(@"\*", "[^/]*").Replace(@"\?", "[^/]") + "$", RegexOptions.IgnoreCase));
            }
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return null; }
        // rel: relative to Mods ("Packages\NRaas\x.package")
        return rel => patterns.Any(p => p.IsMatch(rel.Replace('\\', '/')));
    }
}
