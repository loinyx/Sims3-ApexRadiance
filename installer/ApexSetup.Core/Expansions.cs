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
            }
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
        lock (Icons) Icons[packDir] = png;
        return png;
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

    // A world's own picture, as the game's world chooser shows it (0x0668F635, else 0x8F78F74B in store worlds). World
    // Adventures' destinations have none: no lot's picture stands in for a world, the pack's icon shows instead.
    public static readonly uint[] PictureTypes = [0x0668F635, 0x8F78F74B];
    public static byte[]? Picture(World w) => Dbpf.First(w.Path, PictureTypes);

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
