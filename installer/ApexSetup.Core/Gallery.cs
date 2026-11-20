using System.Text.Json;

namespace ApexSetup;

public sealed record Picture(string Path, DateTime Taken);

// what the player set on one picture
public sealed class PictureInfo
{
    public bool Favourite { get; set; }
    public string? Caption { get; set; }
    public string? Household { get; set; }       // set by hand; null: guessed from the saves
}

// a picture's place on an album's scrapbook board, and a sticky note there
public sealed class Placed { public double X { get; set; } public double Y { get; set; } public double Angle { get; set; } }
public sealed class StickyNote
{
    public string Id { get; set; } = Guid.NewGuid().ToString("N");
    public string Text { get; set; } = "";
    public string Color { get; set; } = "yellow";
    public double X { get; set; }
    public double Y { get; set; }
    public double Angle { get; set; }
}

// an album's scrapbook look: its board, its handwriting, where each picture lies and its notes
public sealed class AlbumLook
{
    public string Background { get; set; } = "desk";     // (the first boards had names; a colour now wins when set)
    public string? Color { get; set; }                    // the board's one colour, "#RRGGBB", fading softly darker
    public string Frame { get; set; } = "polaroid";      // polaroid, rounded, stamp, film, circle
    public string Font { get; set; } = "Ink Free";
    public int Layout { get; set; }                        // 2: laid along the timeline
    public string Light { get; set; } = "soft";          // soft, spot (a lit middle, darker edges) or flat
    public string Tape { get; set; } = "colour";         // colour, match (the board's own hue) or none
    public double Size { get; set; } = 1;                 // the pictures' size, 0.75 to 1.3
    public double Tilt { get; set; } = 1;                 // 0 lies them straight, 1.6 throws them about
    public Dictionary<string, Placed> Places { get; set; } = new(StringComparer.OrdinalIgnoreCase);
    public List<StickyNote> Notes { get; set; } = [];
}

// an album the player made (a trip, a family's story): its name, its icon and the pictures put in it
public sealed class CustomAlbum
{
    public string Id { get; set; } = Guid.NewGuid().ToString("N")[..8];
    public string Name { get; set; } = "";
    public string Icon { get; set; } = "";        // a Segoe Fluent Icons glyph
    public List<string> Pictures { get; set; } = [];
    public string Key => Gallery.Mine + Id;
}

public sealed class GalleryData
{
    public List<CustomAlbum> Custom { get; set; } = [];
    public Dictionary<string, PictureInfo> Pictures { get; set; } = new(StringComparer.OrdinalIgnoreCase);
    public Dictionary<string, AlbumLook> Albums { get; set; } = new(StringComparer.OrdinalIgnoreCase);
    public string? HomePicture { get; set; }    // the picture Home opens on; null: the newest
}

// The game's pictures (Documents\...\Screenshots), the household each belongs to and what the player set on them, kept in
// %LOCALAPPDATA%\Apex Launcher\gallery.json. The pictures themselves are only read, and moved to the Recycle Bin when asked.
public static class Gallery
{
    public const string All = "all", Favourites = "fav", Mine = "my:";
    static readonly string[] Kinds = [".png", ".jpg", ".jpeg", ".bmp"];
    static string FilePath => Path.Combine(Paths.Data, "gallery.json");

    public static string Folder(string docs) => Path.Combine(docs, "Screenshots");

    public static List<Picture> List(string docs)
    {
        var dir = Folder(docs);
        try
        {
            return !Directory.Exists(dir) ? [] : new DirectoryInfo(dir).EnumerateFiles().Where(f => Kinds.Contains(f.Extension.ToLowerInvariant()))
                .Select(f => new Picture(f.FullName, f.LastWriteTime)).OrderByDescending(p => p.Taken).ToList();
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return []; }
    }

    // The game does not say which household a picture is of. The guess: the save written soonest after the picture was
    // taken (a picture is taken while playing, and the game saves that household later). A save only records its newest
    // time, so an older picture of a household played again since can land on another one; the player can set it by hand.
    public static string? GuessHousehold(Picture p, IReadOnlyList<Save> saves) =>
        saves.Where(s => s.LastPlayed >= p.Taken).OrderBy(s => s.LastPlayed).FirstOrDefault()?.Name;

    public static string? HouseholdOf(Picture p, GalleryData d, IReadOnlyList<Save> saves) =>
        d.Pictures.TryGetValue(p.Path, out var i) && i.Household is { Length: > 0 } h ? h : GuessHousehold(p, saves);

    public static bool InAlbum(Picture p, string album, GalleryData d, IReadOnlyList<Save> saves) => album switch
    {
        All => true,
        Favourites => d.Pictures.TryGetValue(p.Path, out var i) && i.Favourite,
        _ when album.StartsWith(Mine, StringComparison.Ordinal) => CustomOf(d, album)?.Pictures.Contains(p.Path, StringComparer.OrdinalIgnoreCase) == true,
        _ => string.Equals(HouseholdOf(p, d, saves), album, StringComparison.OrdinalIgnoreCase),
    };

    // what was set on a picture, read only (nothing stored for a picture nothing was set on)
    public static PictureInfo Peek(GalleryData d, string path) => d.Pictures.TryGetValue(path, out var i) ? i : new PictureInfo();
    public static CustomAlbum? CustomOf(GalleryData d, string key) => d.Custom.FirstOrDefault(c => c.Key == key);

    // a picture in or out of one of the player's albums
    public static void Toggle(GalleryData d, CustomAlbum album, string path)
    {
        if (album.Pictures.RemoveAll(x => string.Equals(x, path, StringComparison.OrdinalIgnoreCase)) == 0) album.Pictures.Add(path);
        Save(d);
    }

    public static PictureInfo Info(GalleryData d, string path) => d.Pictures.TryGetValue(path, out var i) ? i : d.Pictures[path] = new PictureInfo();
    public static AlbumLook Look(GalleryData d, string album) => d.Albums.TryGetValue(album, out var l) ? l : d.Albums[album] = new AlbumLook();

    public static GalleryData Load()
    {
        try { return File.Exists(FilePath) ? JsonSerializer.Deserialize(File.ReadAllText(FilePath), SetupJson.Default.GalleryData) ?? new() : new(); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or JsonException) { Engine.Log(e); return new(); }
    }

    public static void Save(GalleryData d)
    {
        // (a picture nothing is set on keeps no record)
        foreach (var k in d.Pictures.Where(x => !x.Value.Favourite && string.IsNullOrEmpty(x.Value.Caption) && string.IsNullOrEmpty(x.Value.Household)).Select(x => x.Key).ToList()) d.Pictures.Remove(k);
        try { Directory.CreateDirectory(Paths.Data); Fs.WriteAtomic(FilePath, JsonSerializer.Serialize(d, SetupJson.Default.GalleryData)); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { Engine.Log(e); }
    }

    // a picture to the Recycle Bin, and everything kept about it forgotten
    public static void Remove(GalleryData d, string path)
    {
        ModLibrary.Recycle(path);
        d.Pictures.Remove(path);
        foreach (var a in d.Albums.Values) a.Places.Remove(path);
        foreach (var c in d.Custom) c.Pictures.RemoveAll(x => string.Equals(x, path, StringComparison.OrdinalIgnoreCase));
        if (string.Equals(d.HomePicture, path, StringComparison.OrdinalIgnoreCase)) d.HomePicture = null;
        Save(d);
    }
}
