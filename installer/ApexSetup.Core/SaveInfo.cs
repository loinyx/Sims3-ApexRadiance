using System.Buffers.Binary;
using System.Text;
using System.Text.RegularExpressions;

namespace ApexSetup;

// What a save folder says about itself. World and Household are the game's own words (Meta.data); Worlds are the save's
// world files, the one being played first (a save with a university or a holiday world has two). Picture is the
// instance of the household picture in the world file; 0 when Meta.data does not say. The folder's name is the save's
// name: nothing inside repeats it.
public sealed record SaveInfo(string World, IReadOnlyList<string> Worlds, string Household, string Description, ulong Picture, DateTime LastPlayed, long Bytes)
{
    const uint MetaType = 0x628A788F;
    static readonly Dictionary<string, (string Stamp, SaveInfo Info, bool PhotoRead, byte[]? Photo)> Cache = new(StringComparer.OrdinalIgnoreCase);

    public static SaveInfo Of(Save s) => Read(s.Dir);

    // the save's own files (the game's leftover .backup files do not count); a folder that can't be listed has none
    internal static List<FileInfo> FilesOf(string dir)
    {
        try { return new DirectoryInfo(dir).EnumerateFiles().Where(f => !f.Name.EndsWith(".backup", StringComparison.OrdinalIgnoreCase) && !Saves.Passing(f.Name)).ToList(); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return []; }
    }

    // Any save folder, a .sims3.backup one too. Listing the folder is all it costs once read: what was read is kept until
    // a file changes. A broken or half-written save gives what can be told, never an exception.
    public static SaveInfo Read(string dir)
    {
        var files = FilesOf(dir);
        var stamp = string.Join('|', files.OrderBy(f => f.Name, StringComparer.OrdinalIgnoreCase).Select(f => $"{f.Name}:{f.Length}:{f.LastWriteTimeUtc.Ticks}"));
        lock (Cache)
            if (Cache.TryGetValue(dir, out var c) && c.Stamp == stamp) return c.Info;

        string world = "", household = "", description = "";
        ulong picture = 0;
        try
        {
            if (files.FirstOrDefault(f => f.Name.Equals("Meta.data", StringComparison.OrdinalIgnoreCase)) is { } meta
                && Dbpf.Index(meta.FullName).FirstOrDefault(e => e.Type == MetaType) is { Size: > 0 } entry
                && Dbpf.Read(meta.FullName, entry, 1 << 20) is { } b)
            {
                var at = 4;
                string Next()
                {
                    var n = BinaryPrimitives.ReadInt32LittleEndian(b.AsSpan(at));
                    if (n < 0 || n > (b.Length - at - 4) / 2) throw new InvalidDataException("Meta.data text");
                    var s = Encoding.Unicode.GetString(b, at + 4, n * 2);
                    at += 4 + n * 2;
                    return s;
                }
                (world, household, description) = (Next(), Next(), Next());
                picture = BinaryPrimitives.ReadUInt64LittleEndian(b.AsSpan(at));
            }
        }
        catch (Exception e) when (e is InvalidDataException or ArgumentException or IOException) { }

        // world files are "<world>_0x<id>.nhd"; 1a5tf1l3.dat names the one being played
        var current = "";
        try
        {
            if (files.FirstOrDefault(f => f.Name.Equals("1a5tf1l3.dat", StringComparison.OrdinalIgnoreCase)) is { Length: > 0 and < 1024 } dat)
                current = Encoding.UTF8.GetString(File.ReadAllBytes(dat.FullName)).Split('\0', '\r', '\n')[0];
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
        var worlds = files.Where(f => f.Extension.Equals(".nhd", StringComparison.OrdinalIgnoreCase))
            .OrderBy(f => Path.GetFileNameWithoutExtension(f.Name).Equals(current, StringComparison.OrdinalIgnoreCase) ? 0 : 1).ThenBy(f => f.Name, StringComparer.OrdinalIgnoreCase)
            .Select(f => WorldOf(f.Name)).ToList();
        if (world.Length == 0) world = worlds.FirstOrDefault() ?? "";

        var info = new SaveInfo(world, worlds, household, description, picture,
            files.Count > 0 ? files.Max(f => f.LastWriteTime) : DateTime.MinValue, files.Sum(f => f.Length));
        lock (Cache) Cache[dir] = (stamp, info, false, null);
        return info;
    }

    static string WorldOf(string nhd) => Regex.Replace(Path.GetFileNameWithoutExtension(nhd), "_0x[0-9a-fA-F]{8}$", "");

    // The save's own household picture (PNG bytes): the one Meta.data names, looked for in the world being played first.
    // A world keeps a picture of every household in it, so the first one found is often another family's; that one is
    // used only when Meta.data names none.
    public static byte[]? Photo(string dir)
    {
        var info = Read(dir);
        lock (Cache)
            if (Cache.TryGetValue(dir, out var c) && ReferenceEquals(c.Info, info) && c.PhotoRead) return c.Photo;
        byte[]? photo = null;
        try
        {
            var nhds = FilesOf(dir).Where(f => f.Extension.Equals(".nhd", StringComparison.OrdinalIgnoreCase))
                .OrderBy(f => info.Worlds.ToList().IndexOf(WorldOf(f.Name))).Select(f => (f.FullName, Index: Dbpf.Index(f.FullName))).ToList();
            // (0x6B6D837E is the large picture; a save that only has the other sizes shows one of those)
            foreach (var type in new uint[] { Saves.PhotoType, 0x6B6D837F, 0x6B6D837D })
                foreach (var (path, index) in nhds)
                    if (photo is null && index.FirstOrDefault(e => e.Type == type && (info.Picture == 0 || e.Instance == info.Picture)) is { Size: > 0 } e)
                        photo = Dbpf.Read(path, e);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
        lock (Cache)
            if (Cache.TryGetValue(dir, out var c) && ReferenceEquals(c.Info, info)) Cache[dir] = (c.Stamp, info, true, photo);
        return photo;
    }
}
