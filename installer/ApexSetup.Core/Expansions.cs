using Microsoft.Win32;

namespace ApexSetup;

public sealed record Pack(string Code, string Short, string Name, bool Expansion);

// The expansion and stuff packs a game has. Steam keeps each pack in a folder of the game's own (EP1..EP11, SP1..SP9,
// with a dlc-data.ini naming them); the EA app installs packs as folders of their own, listed under the Sims registry
// key with their "Install Dir". Both are looked at; a pack found either way counts.
public static class Expansions
{
    public static readonly Pack[] All =
    [
        new("EP1", "WA", "World Adventures", true), new("EP2", "AM", "Ambitions", true), new("EP3", "LN", "Late Night", true),
        new("EP4", "GN", "Generations", true), new("EP5", "PT", "Pets", true), new("EP6", "SH", "Showtime", true),
        new("EP7", "SN", "Supernatural", true), new("EP8", "SE", "Seasons", true), new("EP9", "UL", "University Life", true),
        new("EP10", "IP", "Island Paradise", true), new("EP11", "IF", "Into the Future", true),
        new("SP1", "HE", "High-End Loft Stuff", false), new("SP2", "FL", "Fast Lane Stuff", false), new("SP3", "OL", "Outdoor Living Stuff", false),
        new("SP4", "TS", "Town Life Stuff", false), new("SP5", "MD", "Master Suite Stuff", false), new("SP6", "KS", "Katy Perry's Sweet Treats", false),
        new("SP7", "DD", "Diesel Stuff", false), new("SP8", "70", "70s, 80s & 90s Stuff", false), new("SP9", "MS", "Movie Stuff", false),
    ];

    // settable for tests: the registry's install folders by pack name ("The Sims 3 World Adventures" → its folder)
    public static Func<string, string?> RegistryDir = name =>
    {
        foreach (var root in new[] { @"SOFTWARE\WOW6432Node\Sims\", @"SOFTWARE\Sims\" })
        {
            try
            {
                using var k = Registry.LocalMachine.OpenSubKey(root + name);
                if (k?.GetValue("Install Dir") is string d && Directory.Exists(d)) return d;
            }
            catch (Exception) { }
        }
        return null;
    };

    // A pack's own icon, as Windows' game list shows it: the first PNG in its game definition file
    // (Game\Bin\Sims3EP01GDF.dll; the base game's is Sims3GDF.dll). Read from the player's install, never shipped.
    static readonly Dictionary<string, byte[]?> Icons = new(StringComparer.OrdinalIgnoreCase);
    public static byte[]? Icon(string packDir)
    {
        lock (Icons)
            if (Icons.TryGetValue(packDir, out var known)) return known;
        byte[]? png = null;
        try
        {
            var bin = Path.Combine(packDir, "Game", "Bin");
            if (Directory.Exists(bin) && Directory.EnumerateFiles(bin, "Sims3*GDF.dll").FirstOrDefault() is { } gdf && new FileInfo(gdf).Length < 16 << 20)
            {
                var d = File.ReadAllBytes(gdf);
                var at = d.AsSpan().IndexOf(new byte[] { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A });
                var end = at < 0 ? -1 : d.AsSpan(at).IndexOf("IEND"u8);
                if (end > 0) png = d[at..(at + end + 8)];
                else png = IconFromDib(d); // (High-End Loft Stuff keeps its icon as a plain 256 px icon bitmap, no PNG)
            }
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
        lock (Icons) Icons[packDir] = png;
        return png;
    }

    // The first 256 px, 32-bit icon bitmap in the file (an icon resource: a BITMAPINFOHEADER whose height counts the
    // colour rows and the mask rows, the colour rows bottom-up in BGRA), as a PNG
    public static byte[]? IconFromDib(byte[] d)
    {
        for (var i = d.AsSpan().IndexOf(new byte[] { 40, 0, 0, 0 }); i >= 0 && i + 40 < d.Length;)
        {
            var w = BitConverter.ToInt32(d, i + 4);
            var h = BitConverter.ToInt32(d, i + 8);
            if (w == 256 && h == 512 && BitConverter.ToUInt16(d, i + 12) == 1 && BitConverter.ToUInt16(d, i + 14) == 32 && BitConverter.ToUInt32(d, i + 16) == 0
                && i + 40 + w * w * 4 <= d.Length)
            {
                var rgba = new byte[w * w * 4];
                for (var y = 0; y < w; y++)
                    for (var x = 0; x < w; x++)
                    {
                        var s = i + 40 + ((w - 1 - y) * w + x) * 4;
                        var o = (y * w + x) * 4;
                        rgba[o] = d[s + 2]; rgba[o + 1] = d[s + 1]; rgba[o + 2] = d[s]; rgba[o + 3] = d[s + 3];
                    }
                return Png(w, w, rgba);
            }
            var next = d.AsSpan(i + 1).IndexOf(new byte[] { 40, 0, 0, 0 });
            i = next < 0 ? -1 : i + 1 + next;
        }
        return null;
    }

    // a plain RGBA PNG (one IDAT, no filter)
    public static byte[] Png(int w, int h, byte[] rgba)
    {
        using var raw = new MemoryStream();
        using (var z = new System.IO.Compression.ZLibStream(raw, System.IO.Compression.CompressionLevel.Optimal, true))
            for (var y = 0; y < h; y++) { z.WriteByte(0); z.Write(rgba, y * w * 4, w * 4); }
        using var png = new MemoryStream();
        png.Write([0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A]);
        void Chunk(string type, byte[] data)
        {
            var t = System.Text.Encoding.ASCII.GetBytes(type);
            png.Write([(byte)(data.Length >> 24), (byte)(data.Length >> 16), (byte)(data.Length >> 8), (byte)data.Length]);
            png.Write(t);
            png.Write(data);
            var crc = Crc(t.Concat(data));
            png.Write([(byte)(crc >> 24), (byte)(crc >> 16), (byte)(crc >> 8), (byte)crc]);
        }
        Chunk("IHDR", [(byte)(w >> 24), (byte)(w >> 16), (byte)(w >> 8), (byte)w, (byte)(h >> 24), (byte)(h >> 16), (byte)(h >> 8), (byte)h, 8, 6, 0, 0, 0]);
        Chunk("IDAT", raw.ToArray());
        Chunk("IEND", []);
        return png.ToArray();
    }

    static uint Crc(IEnumerable<byte> bytes)
    {
        var c = 0xFFFFFFFFu;
        foreach (var b in bytes)
        {
            c ^= b;
            for (var k = 0; k < 8; k++) c = (c & 1) != 0 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
        }
        return c ^ 0xFFFFFFFFu;
    }

    // a pack folder with the pack in it: its game definition file or anything in GameData (a folder left empty is not a pack)
    static bool HasContent(string dir)
    {
        try
        {
            var bin = Path.Combine(dir, "Game", "Bin");
            if (Directory.Exists(bin) && Directory.EnumerateFiles(bin, "Sims3*GDF.dll").Any()) return true;
            var data = Path.Combine(dir, "GameData");
            return Directory.Exists(data) && Directory.EnumerateFiles(data, "*", SearchOption.AllDirectories).Any();
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return false; }
    }

    // each pack found with its folder
    public static List<(Pack Pack, string Dir)> Installed(Game g)
    {
        var found = new List<(Pack, string)>();
        foreach (var p in All)
        {
            var steam = Path.Combine(g.Root, p.Code);
            if (HasContent(steam)) { found.Add((p, steam)); continue; }
            if (RegistryDir("The Sims 3 " + p.Name) is { } ea) found.Add((p, ea));
        }
        return found;
    }
}

public enum WorldKind { BaseGame, Expansion, Store, Custom }
public sealed record World(string Name, WorldKind Kind, string? Pack, string Path);

// The worlds a new game can start in: the base game's and each pack's (GameData\Shared\NonPackaged\Worlds\*.world), and
// those installed from the store or by the player (Documents\...\InstalledWorlds\*.world).
public static class Worlds
{
    // EA's store worlds, as their .world files are named (others in InstalledWorlds count as custom)
    static readonly string[] Store = ["Riverview", "Barnacle Bay", "Hidden Springs", "Lunar Lakes", "Lucky Palms", "Sunlit Tides",
        "Monte Vista", "Aurora Skies", "Dragon Valley", "Midnight Hollow", "Roaring Heights"];

    // A world's own picture, as the game's world chooser shows it (0x0668F635, else 0x8F78F74B in store worlds). The
    // destinations (World Adventures' three, Sims University, Oasis Landing) have none in their .world: their pack's
    // loading-screen image stands in, a PNG named world_loading_<place> (instance = FNV-64 of the name) in the pack's
    // packages (checked: cairo, beijing, paris, university, future).
    public static readonly uint[] PictureTypes = [0x0668F635, 0x8F78F74B];
    const uint Png = 0x2F7D0004;
    static readonly Dictionary<string, string> LoadingName = new(StringComparer.OrdinalIgnoreCase)
    {
        ["China"] = "beijing", ["Egypt"] = "cairo", ["France"] = "paris", ["Sims University"] = "university", ["Oasis Landing"] = "future",
    };
    public static byte[]? Picture(World w)
    {
        if (Dbpf.First(w.Path, PictureTypes) is { } own) return own;
        if (w.Kind != WorldKind.Expansion) return null;
        var stem = System.IO.Path.GetFileNameWithoutExtension(w.Path);
        var names = new[] { LoadingName.GetValueOrDefault(stem), stem.Replace(" ", "").ToLowerInvariant() }.OfType<string>().Select(n => Fnv64("world_loading_" + n)).ToHashSet();
        // (Worlds sits in <pack>\GameData\Shared\NonPackaged\Worlds; the images are in <pack>\GameData\Shared\Packages)
        var packages = System.IO.Path.GetFullPath(System.IO.Path.Combine(System.IO.Path.GetDirectoryName(w.Path)!, "..", "..", "Packages"));
        if (!Directory.Exists(packages)) return null;
        foreach (var f in Directory.EnumerateFiles(packages, "*.package"))
            if (Dbpf.Index(f).FirstOrDefault(e => e.Type == Png && names.Contains(e.Instance)) is { Type: Png } hit) return Dbpf.Read(f, hit);
        return null;
    }

    static ulong Fnv64(string s)
    {
        var h = 0xCBF29CE484222325UL;
        foreach (var c in s.ToLowerInvariant()) { h *= 0x100000001B3UL; h ^= (byte)c; }
        return h;
    }

    // a pack's own test or helper worlds shipped beside the playable ones
    static bool Hidden(string name) => name.Contains('_') || name.EndsWith("Test", StringComparison.OrdinalIgnoreCase);

    public static List<World> List(Game g, string docs)
    {
        var list = new List<World>();
        void Add(string dir, WorldKind kind, string? pack)
        {
            var w = Path.Combine(dir, "GameData", "Shared", "NonPackaged", "Worlds");
            if (!Directory.Exists(w)) return;
            foreach (var f in Directory.EnumerateFiles(w, "*.world"))
                if (Path.GetFileNameWithoutExtension(f) is var n && !Hidden(n)) list.Add(new World(Pretty(n), kind, pack, f));
        }
        Add(g.Root, WorldKind.BaseGame, null);
        foreach (var (p, dir) in Expansions.Installed(g)) Add(dir, WorldKind.Expansion, p.Short);
        var installed = Path.Combine(docs, "InstalledWorlds");
        if (Directory.Exists(installed))
            foreach (var f in Directory.EnumerateFiles(installed, "*.world"))
            {
                var n = Pretty(Path.GetFileNameWithoutExtension(f));
                list.Add(new World(n, Store.Any(s => s.Equals(n, StringComparison.OrdinalIgnoreCase)) ? WorldKind.Store : WorldKind.Custom, null, f));
            }
        return list;
    }

    // "IslaParadiso" → "Isla Paradiso", "AppaloosaPlains" → "Appaloosa Plains"; World Adventures' destinations by name
    static string Pretty(string file) => file switch
    {
        "China" => "Shang Simla", "Egypt" => "Al Simhara", "France" => "Champs Les Sims",
        _ => System.Text.RegularExpressions.Regex.Replace(file, "(?<=[a-z])(?=[A-Z])", " "),
    };
}
