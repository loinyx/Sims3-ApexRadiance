using System.IO.Compression;
using System.Security.Cryptography;

namespace ApexSetup;

public enum AddPlace { Mods, SavedSims, Library, Worlds }

// One file the player gave, or one found in an archive or folder given: what it is, where it goes and what is already there.
public sealed class AddItem
{
    public required string Name { get; init; }
    public string File { get; init; } = "";         // where its bytes are now: the file given, or the copy taken out of an archive
    public string? Archive { get; init; }           // the archive given that held it
    public AddPlace Place { get; set; }
    public ModKind Kind { get; set; }               // a mod's kind
    public bool Household { get; set; }             // a Library item: a family and not a lot
    public long Size { get; set; }
    public string Sha { get; set; } = "";
    public string? Problem { get; set; }            // why it is not added
    public string? Warning { get; set; }
    public string? SameAs { get; set; }             // the same bytes are already there: that file's name, or the merge's (SameInMerge)
    public bool SameInMerge { get; set; }
    public bool SameInList { get; set; }            // ... or another file of this list
    public string? Other { get; set; }              // another version: a file of the same name with other bytes
    public ModFile? OtherMod { get; set; }
    public bool CanReplace { get; set; }
    public string? OtherMerge { get; set; }         // ... or the merge that holds one
    public bool Add { get; set; }                   // the player's choices
    public bool Replace { get; set; }
    public string? Added { get; set; }              // where it went
    public string? Replaced { get; set; }           // the file that went to the Recycle Bin for it

    public bool Ok => Problem is null;
}

// Adding files: opened (archives and folders too), checked, compared with what is installed, then copied one at a time, each
// to the place the game reads it from. Files come from the internet: an archive is opened by what it is (not by its name),
// within a limit of files and bytes, an entry keeps nothing of its path, and nothing in it is ever run. Open only reads
// (archives go to a temporary folder that Dispose deletes); Add copies to a temporary name, checks the copy against the
// bytes that were inspected, and renames it into place; a file there is never written over.
// Where each goes: the game reads mods from Mods, *.sim from Documents\...\SavedSims, the lots and families it exports from
// Documents\...\Library (ExportBinSubdirName in Default.ini) and worlds from GameData\Shared\NonPackaged\Worlds.
public sealed class ModAdd : IDisposable
{
    public static readonly string[] Extensions = [".package", ".sim", ".world", ".zip", ".rar", ".7z"];
    static readonly string[] ArchiveTypes = [".zip", ".rar", ".7z"],
        Programs = [".exe", ".dll", ".asi", ".msi", ".com", ".scr", ".bat", ".cmd", ".ps1", ".vbs", ".js", ".jar", ".lnk"];
    public static readonly string[] Packs = [".sims3pack", ".ts3pack"];
    const string PackText = "Install it with The Sims 3's own launcher: double-click the file.";
    const string ProgramText = "A program or plugin. The launcher doesn't add these.";
    const string NothingText = "It holds no mod, Sim, lot or world.";
    const string NotOpened = "The archive can't be opened. It may be damaged or ask for a password. Extract it, then add the folder.";
    const string TooBig = "The archive is too big to open here. Extract it, then add the folder.";

    public static long MaxBytes = ModConflicts.UnpackCap; // taken out of one archive (settable for tests)

    const string TempPrefix = "apex-add-";
    readonly Game _game;
    readonly string _docs, _folder;
    readonly string _temp = Directory.CreateTempSubdirectory(TempPrefix).FullName;
    IProgress<string>? _progress;
    CancellationToken _ct;
    long _bytesLeft;
    int _filesLeft;

    public List<AddItem> Items { get; } = [];
    public string Folder => _folder; // (relative to Mods)

    ModAdd(Game g, string docs, string folder) => (_game, _docs, _folder) = (g, docs, folder);

    public static ModAdd Open(Game g, string docs, string folder, IEnumerable<string> paths, IReadOnlyList<ModFile> installed, IProgress<string>? progress = null, CancellationToken ct = default)
    {
        ModLibrary.Under(docs, folder);
        Sweep();
        var a = new ModAdd(g, docs, folder) { _progress = progress, _ct = ct };
        try
        {
            foreach (var p in paths.Distinct(StringComparer.OrdinalIgnoreCase))
                if (Directory.Exists(p)) a.TakeFolder(p);
                else a.Take(p, Path.GetFileName(p), null, 0, given: true);
            a.Compare(installed);
            return a;
        }
        catch { a.Dispose(); throw; }
    }

    // ---------- what each file is ----------

    void Refuse(string name, string? archive, string why) => Items.Add(new AddItem { Name = name, Archive = archive, Problem = why });

    void TakeFolder(string dir)
    {
        var name = Path.GetFileName(Path.TrimEndingDirectorySeparator(dir));
        var files = Directory.EnumerateFiles(dir, "*", new EnumerationOptions { RecurseSubdirectories = true, IgnoreInaccessible = true, AttributesToSkip = FileAttributes.ReparsePoint })
            .Take(ModConflicts.UnpackFiles + 1).ToList();
        if (files.Count > ModConflicts.UnpackFiles) { Refuse(name, null, "The folder holds too many files. Add it in parts."); return; }
        var before = Items.Count;
        foreach (var f in files.Order(StringComparer.OrdinalIgnoreCase)) Take(f, Path.GetFileName(f), null, 0, given: false);
        if (Items.Count == before) Refuse(name, null, NothingText);
    }

    // given: the player named this very file (one met in a folder or archive that is no mod is passed over)
    void Take(string path, string name, string? archive, int depth, bool given)
    {
        _ct.ThrowIfCancellationRequested();
        _progress?.Report(name);
        var ext = Path.GetExtension(name).ToLowerInvariant();
        if (ArchiveTypes.Contains(ext)) OpenArchive(path, name, archive, depth);
        else if (Extensions.Contains(ext)) Items.Add(Check(path, name, archive, ext));
        else if (Packs.Contains(ext)) Refuse(name, archive, PackText);
        else if (Programs.Contains(ext)) Refuse(name, archive, ProgramText);
        else if (given) Refuse(name, archive, "Not a mod file. Add packages, Sims, lots, worlds, or archives of them.");
    }

    static AddItem Check(string path, string name, string? archive, string ext)
    {
        var i = new AddItem { Name = name, File = path, Archive = archive, Place = ext switch { ".sim" => AddPlace.SavedSims, ".world" => AddPlace.Worlds, _ => AddPlace.Mods } };
        try
        {
            i.Size = new FileInfo(path).Length;
            if (!ModLibrary.SafeFileName(name)) { i.Problem = "The name can't be used for a file. Rename it first."; return i; }
            if (i.Size == 0) { i.Problem = "The file is empty."; return i; }
            var warnings = new List<string>();
            i.Problem = ModInstall.PackageProblems(path, warnings).FirstOrDefault();
            if (!i.Ok) return i;
            if (ext == ".package")
            {
                i.Warning = warnings.FirstOrDefault();
                var index = Dbpf.Index(path);
                i.Kind = Mods.KindOf(index.GroupBy(e => e.Type).ToDictionary(x => x.Key, x => x.Count()));
                if (IsLibrary(index))
                {
                    i.Place = AddPlace.Library;
                    i.Household = index.All(e => e.Type != LotType);
                }
            }
            i.Sha = Hash(path);
            i.Add = true;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { i.Problem = "The file could not be read."; }
        return i;
    }

    // What the game exports to the Library holds one entry of ExportType and a lot's or a family's own (every one of the 129
    // in the game's Library folders does; elsewhere only worlds and the game's full builds hold them, many of each)
    const uint ExportType = 0x05ED1226, LotType = 0x04F66BCC, FamilyType = 0x062853A8;
    public static bool IsLibrary(IReadOnlyCollection<Dbpf.Entry> index) =>
        index.Count(e => e.Type == ExportType) == 1 && index.Any(e => e.Type is LotType or FamilyType) && index.All(e => e.Type != ModKinds.S3sa);

    // ---------- archives ----------

    sealed class TooBigException : Exception;

    void OpenArchive(string path, string name, string? archive, int depth)
    {
        // (an archive in an archive is opened, one in that is not)
        if (depth > 1) { Refuse(name, archive, "This archive is too deep inside others. Extract it, then add it."); return; }
        if (depth == 0) (_bytesLeft, _filesLeft) = (MaxBytes, ModConflicts.UnpackFiles);
        var before = Items.Count;
        string? why = null;
        try
        {
            var head = new byte[8];
            using (var f = Dbpf.OpenShared(path)) f.ReadAtLeast(head, 8, throwOnEndOfStream: false);
            if (head.AsSpan().StartsWith("PK"u8)) Unzip(path, archive ?? name, depth);
            // (only a rar or 7z by its first bytes: tar.exe would also open a tar or a disc image under that name)
            else if (head.AsSpan().StartsWith("Rar!"u8) || head.AsSpan().StartsWith((ReadOnlySpan<byte>)[0x37, 0x7A, 0xBC, 0xAF])) why = Unpack(path, archive ?? name, depth);
            else why = NotOpened;
        }
        catch (TooBigException) when (depth == 0) { why = TooBig; }
        catch (Exception e) when (e is InvalidDataException or IOException or UnauthorizedAccessException or NotSupportedException)
        {
            Engine.Log(e);
            why = e.HResult == unchecked((int)0x80070070) /* ERROR_DISK_FULL */ ? "There isn't enough free space to open the archive." : NotOpened;
        }
        // (an archive is added whole as its maker packed it, or not at all)
        if (why is not null) { Items.RemoveRange(before, Items.Count - before); Refuse(name, archive, why); }
        else if (Items.Count == before && depth == 0) Refuse(name, archive, NothingText);
    }

    void Unzip(string zip, string archive, int depth)
    {
        using var z = ZipFile.OpenRead(zip);
        if ((_filesLeft -= z.Entries.Count) < 0) throw new TooBigException();
        foreach (var e in z.Entries)
        {
            _ct.ThrowIfCancellationRequested();
            if (EntryName(e.FullName) is not { } name) continue;
            var ext = Path.GetExtension(name).ToLowerInvariant();
            if (!Extensions.Contains(ext)) { Take("", name, archive, depth + 1, given: false); continue; } // (named, never taken out)
            if (e.IsEncrypted) throw new InvalidDataException("encrypted entry");
            var to = Path.Combine(_temp, Guid.NewGuid().ToString("N") + ext);
            using (var from = e.Open())
            using (var f = new FileStream(to, FileMode.CreateNew, FileAccess.Write))
            {
                // (what comes out counts, not the size the entry states)
                var buffer = new byte[1 << 16];
                for (int n; (n = from.Read(buffer)) > 0;)
                {
                    _ct.ThrowIfCancellationRequested();
                    if ((_bytesLeft -= n) < 0) throw new TooBigException();
                    f.Write(buffer, 0, n);
                }
            }
            try { File.SetLastWriteTime(to, e.LastWriteTime.DateTime); } catch (ArgumentOutOfRangeException) { }
            Take(to, name, archive, depth + 1, given: false);
        }
    }

    // an entry's file name alone; null for a folder and for what macOS packs beside each file
    static string? EntryName(string full)
    {
        full = full.Replace('\\', '/');
        var name = full[(full.LastIndexOf('/') + 1)..];
        return name.Length == 0 || name.StartsWith("._") || full.StartsWith("__MACOSX/") ? null : name;
    }

    // .rar and .7z through the extractors the conflict check uses (Windows' tar.exe, 7-Zip), with its limits and checks
    string? Unpack(string path, string archive, int depth)
    {
        if (_bytesLeft <= 0 || _filesLeft <= 0) throw new TooBigException();
        var (dir, files, unknown) = ModConflicts.Extract(path);
        if (dir is null)
        {
            Engine.Log(new InvalidDataException(unknown));
            if (unknown!.StartsWith("archive refused: unsafe")) return "The archive was refused: it would write outside its own folder.";
            if (unknown.StartsWith("archive refused")) return TooBig;
            return ModConflicts.Extractors().Any(x => x.Kind == "7z") ? NotOpened
                : "Windows can't open this archive. Install 7-Zip and add it again, or extract it and add the folder.";
        }
        var here = Path.Combine(_temp, Guid.NewGuid().ToString("N")); // (under the folder Dispose deletes)
        try { Fs.Retry(() => Directory.Move(dir, here)); }
        catch { DeleteFolder(dir); throw; }
        _filesLeft -= files.Count;
        _bytesLeft -= files.Sum(f => new FileInfo(Path.Combine(here, f)).Length);
        if (_bytesLeft < 0 || _filesLeft < 0) throw new TooBigException();
        foreach (var f in files)
            if (EntryName(f) is { } name) Take(Path.Combine(here, f), name, archive, depth + 1, given: false);
        return null;
    }

    // ---------- what is already there ----------

    static bool Same(string a, string b) => a.Equals(b, StringComparison.OrdinalIgnoreCase);

    void Compare(IReadOnlyList<ModFile> installed)
    {
        // (the ones that are on first: of two copies, the one the game loads is compared and replaced)
        var mods = installed.Where(m => m.FileType == "package").OrderByDescending(m => m.Enabled).ToList();
        var merges = mods.Select(m => ModMerge.Info(_docs, m)).OfType<MergeRecord>().ToList();
        var inMerges = merges.SelectMany(r => r.Originals.Select(o => (Merge: r.Name, Original: o))).ToList();
        var hashes = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        string HashOf(string p)
        {
            if (hashes.TryGetValue(p, out var h)) return h;
            try { return hashes[p] = Hash(p); }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return hashes[p] = "?"; }
        }
        static List<(string Path, long Size)> FilesIn(string dir, string pattern)
        {
            try { return Directory.Exists(dir) ? new DirectoryInfo(dir).EnumerateFiles(pattern).Select(f => (f.FullName, f.Length)).ToList() : []; }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return []; }
        }
        var there = new Dictionary<AddPlace, List<(string Path, long Size)>>
        {
            [AddPlace.Mods] = mods.Select(m => (m.Path, m.Size)).ToList(),
            [AddPlace.SavedSims] = FilesIn(Dir(AddPlace.SavedSims), "*.sim"),
            [AddPlace.Library] = FilesIn(Dir(AddPlace.Library), "*.package"),
            [AddPlace.Worlds] = [.. FilesIn(Dir(AddPlace.Worlds), "*.world"), .. FilesIn(Path.Combine(_docs, "InstalledWorlds"), "*.world")],
        };
        var seen = new Dictionary<(AddPlace, string), string>();
        var claimed = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        foreach (var i in Items.Where(i => i.Ok))
        {
            _ct.ThrowIfCancellationRequested();
            if (there[i.Place].FirstOrDefault(f => f.Size == i.Size && Same(HashOf(f.Path), i.Sha)).Path is { } same) (i.SameAs, i.OtherMod) = (Path.GetFileName(same), mods.FirstOrDefault(m => Same(m.Path, same)));
            else if (i.Place == AddPlace.Mods && inMerges.FirstOrDefault(x => x.Original.Size == i.Size && Same(x.Original.Sha256, i.Sha)).Merge is { } merge) (i.SameAs, i.SameInMerge) = (merge, true);
            else if (!seen.TryAdd((i.Place, i.Sha), i.Name)) (i.SameAs, i.SameInList) = (seen[(i.Place, i.Sha)], true);
            if (i.SameAs is not null) i.Add = false;
            else if (there[i.Place].Where(f => Same(Path.GetFileName(f.Path), i.Name) && !claimed.Contains(f.Path))
                .OrderBy(f => Same(Path.GetDirectoryName(f.Path)!, Dir(i.Place)) ? 0 : 1).FirstOrDefault().Path is { } other)
            {
                i.Other = other;
                i.OtherMod = mods.FirstOrDefault(m => Same(m.Path, other));
                // (saves go on using the world file they were made in; the launcher's own files and its merges have their own
                // ways to change)
                i.CanReplace = i.Place != AddPlace.Worlds && i.OtherMod?.Locked != true && !merges.Any(r => i.OtherMod is { } o && Same(r.Merged, o.Rel));
                // (a mod of the same name is taken for the same mod when the two share game resources)
                i.Replace = i.CanReplace && i.Place == AddPlace.Mods && SharesResources(i.File, other);
                if (i.CanReplace) claimed.Add(other);
                if (i.Place == AddPlace.Worlds || i.OtherMod?.Locked == true) i.Add = false;
            }
            else if (i.Place == AddPlace.Mods) i.OtherMerge = inMerges.FirstOrDefault(x => Same(Path.GetFileName(x.Original.Rel), i.Name)).Merge;
            if (i.Place == AddPlace.Mods && !i.CanReplace && TooLong(Path.Combine(Dir(AddPlace.Mods), i.Name))) (i.Problem, i.Add) = (LongText, false);
        }
    }

    // (a package's list of names and its store receipt are not content)
    static bool SharesResources(string a, string b)
    {
        static IEnumerable<(uint, uint, ulong)> Keys(string p) => Dbpf.Index(p).Where(e => e.Type is not (ModKinds.Nmap or ModKinds.Manifest)).Select(e => (e.Type, e.Group, e.Instance));
        return Keys(a).ToHashSet().Overlaps(Keys(b));
    }

    const string LongText = "The folder and name are too long for the game to read.";
    // (the game, a 32-bit program, does not read files past the old 260-character path limit)
    static bool TooLong(string path) => path.Length >= ModLibrary.MaxGamePath;

    // ---------- adding ----------

    public string Dir(AddPlace place) => place switch
    {
        AddPlace.SavedSims => Path.Combine(_docs, "SavedSims"),
        AddPlace.Library => Path.Combine(_docs, "Library"),
        AddPlace.Worlds => WorldsDir(_game),
        _ => ModLibrary.Under(_docs, _folder),
    };

    // one file, not a world, into its place (Added); an error leaves everything as it was
    public void Add(AddItem i)
    {
        Engine.CheckWritable();
        if (!i.Ok || i.Place == AddPlace.Worlds) throw new InvalidOperationException(i.Name);
        var replace = i.Replace && i.CanReplace && File.Exists(i.Other);
        var dir = replace ? Path.GetDirectoryName(i.Other!)! : Dir(i.Place);
        Directory.CreateDirectory(dir);
        if (i.Place == AddPlace.Mods) Engine.EnsureResourceCfg(_docs); // (without it the game loads no mod at all)
        var part = Copy(i.File, dir, i.Sha);
        try
        {
            if (replace)
            {
                // (back where the old one is, on or off, so its notes and tags stay)
                ModMerge.Recycle(i.Other!);
                i.Replaced = i.Other;
                try { Fs.Retry(() => File.Move(part, i.Other!)); }
                catch (Exception e) when (e is IOException or UnauthorizedAccessException)
                {
                    Engine.Log(e);
                    throw new SetupException("{0} went to the Recycle Bin, but the new file could not take its place. Add it again.", Path.GetFileName(i.Other!));
                }
                i.Added = i.Other;
                return;
            }
            var to = Mods.Free(Path.Combine(dir, i.Name), i.Place == AddPlace.Mods ? Path.Combine(Mods.Disabled(_docs), _folder) : null);
            if (i.Place == AddPlace.Mods && TooLong(to)) throw new SetupException(LongText);
            Fs.Retry(() => File.Move(part, to));
            i.Added = to;
        }
        finally { TryDelete(part); }
    }

    // a copy in dir under a name the game does not read, made of the very bytes that were inspected
    static string Copy(string from, string dir, string sha)
    {
        var part = Path.Combine(dir, Guid.NewGuid().ToString("N") + ".apex-part");
        try
        {
            using (var s = new FileStream(from, FileMode.Open, FileAccess.Read, FileShare.Read, 1 << 16))
            using (var d = new FileStream(part, FileMode.CreateNew, FileAccess.Write))
            using (var h = IncrementalHash.CreateHash(HashAlgorithmName.SHA256))
            {
                var buffer = new byte[1 << 20];
                for (int n; (n = s.Read(buffer)) > 0;) { h.AppendData(buffer, 0, n); d.Write(buffer, 0, n); }
                if (!Same(Convert.ToHexString(h.GetHashAndReset()), sha)) throw new SetupException("The file changed while it was being added. Add it again.");
            }
            File.SetLastWriteTimeUtc(part, File.GetLastWriteTimeUtc(from));
            return part;
        }
        catch { TryDelete(part); throw; }
    }

    // the files added, to the Recycle Bin (one moved, changed or removed since is left alone)
    public static void Undo(IEnumerable<AddItem> added)
    {
        Engine.CheckWritable();
        foreach (var i in added)
            if (i.Added is { } p && File.Exists(p) && Same(Hash(p), i.Sha)) ModMerge.Recycle(p);
    }

    // ---------- worlds: in the game's own folder, where Windows asks for an administrator ----------

    public static string WorldsDir(Game g) => Path.Combine(g.Root, "GameData", "Shared", "NonPackaged", "Worlds");

    public static bool WorldsNeedAdmin(Game g) => !Elevation.IsElevated && !(Directory.Exists(WorldsDir(g)) && Elevation.CanWrite(WorldsDir(g)));

    // what the administrator copy is given: each world's file, the name it takes and the hash of what the player saw
    public Dictionary<string, string> WorldPlan(IReadOnlyList<AddItem> worlds)
    {
        var plan = new Dictionary<string, string>();
        var taken = new HashSet<string>(StringComparer.OrdinalIgnoreCase);
        for (int n = 0; n < worlds.Count; n++)
        {
            var name = worlds[n].Name;
            for (int k = 2; File.Exists(Path.Combine(WorldsDir(_game), name)) || !taken.Add(name); k++) name = $"{Path.GetFileNameWithoutExtension(worlds[n].Name)} ({k}).world";
            plan[$"name.{n}"] = name;
            plan[$"file.{n}"] = worlds[n].File;
            plan[$"sha.{n}"] = worlds[n].Sha;
        }
        return plan;
    }

    // Also run by the administrator copy, from a plan another program could have changed: only whole Sims 3 packages, under a
    // plain *.world name, into this game's Worlds folder (none of it a link to another place), and never over a file.
    public static void AddWorlds(Game g, IReadOnlyDictionary<string, string> plan)
    {
        Engine.CheckWritable();
        var dir = WorldsDir(g);
        foreach (var d in new[] { "GameData", @"GameData\Shared", @"GameData\Shared\NonPackaged", @"GameData\Shared\NonPackaged\Worlds" }.Select(d => Path.Combine(g.Root, d)))
            if (Directory.Exists(d) && File.GetAttributes(d).HasFlag(FileAttributes.ReparsePoint)) throw new SetupException("The game's Worlds folder leads to another place, so nothing was added to it.");
        Exception? first = null;
        for (int n = 0; plan.TryGetValue($"name.{n}", out var name); n++)
            try
            {
                if (!ModLibrary.SafeFileName(name) || !name.EndsWith(".world", StringComparison.OrdinalIgnoreCase)) throw new SetupException("That name can't be used for a file or folder.");
                if (!plan.TryGetValue($"file.{n}", out var file) || !plan.TryGetValue($"sha.{n}", out var sha)) throw new SetupException("The file could not be read.");
                if (File.Exists(Path.Combine(dir, name))) throw new SetupException("A file with that name already exists here.");
                Directory.CreateDirectory(dir);
                var part = Copy(file, dir, sha);
                try
                {
                    if (ModInstall.PackageProblems(part).FirstOrDefault() is { } bad) throw new SetupException(bad);
                    Fs.Retry(() => File.Move(part, Path.Combine(dir, name)));
                }
                finally { TryDelete(part); }
            }
            catch (Exception e) { Engine.Log(e); first ??= e; }
        if (first is not null) throw first;
    }

    // which of the plan's worlds are there now, read back
    public void WorldsDone(IReadOnlyList<AddItem> worlds, IReadOnlyDictionary<string, string> plan)
    {
        for (int n = 0; n < worlds.Count; n++)
            try { if (Path.Combine(WorldsDir(_game), plan[$"name.{n}"]) is var p && File.Exists(p) && Same(Hash(p), worlds[n].Sha)) worlds[n].Added = p; }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { Engine.Log(e); }
    }

    // ---------- files ----------

    static string Hash(string path) { using var s = Dbpf.OpenShared(path); return Convert.ToHexString(SHA256.HashData(s)); }

    static void TryDelete(string p) { try { File.Delete(p); } catch (Exception e) when (e is IOException or UnauthorizedAccessException) { } }

    // (a read-only file from an archive would keep the folder)
    static void DeleteFolder(string dir)
    {
        try
        {
            foreach (var f in Directory.EnumerateFiles(dir, "*", new EnumerationOptions { RecurseSubdirectories = true, AttributesToSkip = FileAttributes.ReparsePoint }))
                File.SetAttributes(f, FileAttributes.Normal);
            Directory.Delete(dir, true);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { Engine.Log(e); }
    }

    public void Dispose() { if (Directory.Exists(_temp)) DeleteFolder(_temp); }

    // what a launcher closed in the middle left
    static void Sweep()
    {
        try
        {
            foreach (var d in Directory.EnumerateDirectories(Path.GetTempPath(), TempPrefix + "*"))
                if (Directory.GetCreationTimeUtc(d) < DateTime.UtcNow.AddDays(-1)) DeleteFolder(d);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
    }
}
