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
public sealed class Placed { public double X { get; set; } public double Y { get; set; } public double Angle { get; set; } public bool Moved { get; set; } public int Z { get; set; } }   // Moved: put there by the player; Z: the order it was put down in
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
    public string? Color { get; set; } = "#07070A";       // the board's one colour, "#RRGGBB", fading softly darker
    public string Frame { get; set; } = "rounded";       // polaroid, print, washi, clip, rounded, kraft, deckle, doodle, stamp, film
    public string Font { get; set; } = "Ink Free";
    public double Height { get; set; }                     // the board's height its places were laid on
    public int Layout { get; set; }                        // 4: laid along the timeline, newest first, a little apart
    public string Light { get; set; } = "flat";          // soft, spot (a lit middle, darker edges) or flat
    public string Tape { get; set; } = "colour";         // colour, match (the board's own hue) or none
    public double Size { get; set; } = 1;                 // the pictures' size, 0.75 to 1.3
    public double Tilt { get; set; } = 1;                 // 0 lies them straight, 1.6 throws them about
    public double Spacing { get; set; } = 1.25;            // how far apart the pictures lie along the timeline, 0.6 to 1.7
    public Dictionary<string, Placed> Places { get; set; } = new(StringComparer.OrdinalIgnoreCase);
    public List<StickyNote> Notes { get; set; } = [];
    public string Entrance { get; set; } = "soft";       // how the pictures come onto the board: soft, float, zoom, slide, playful or none
    public int Seed { get; set; }                          // varies the timeline's wave; Rearrange picks another
    public string Pattern { get; set; } = "none";        // none, lines, grid or dots: the paper under the pictures
    public List<Sticker> Stickers { get; set; } = [];
    public DateTime Seen { get; set; }                     // the newest picture when the board was last laid
    public double SeenX { get; set; }                      // and where it lay
    public int Lead { get; set; }                          // pictures (and their places on the wave) added in front since
    public int SlotLead { get; set; }                      // the board was arranged, so the older ones keep their look
    public List<TimeGap> Gaps { get; set; } = [];          // room opened in front of an arrangement for newer pictures
    public int TopZ { get; set; }                          // the last picture put down
}

// room on a timeline just newer than a moment: pictures taken at or before At lie Width further along
public sealed class TimeGap
{
    public DateTime At { get; set; }
    public double Width { get; set; }
}

// a sticker on a board: an emoji, where it lies, its turn and its size
public sealed class Sticker
{
    public string Id { get; set; } = Guid.NewGuid().ToString("N");
    public string Text { get; set; } = "";
    public double X { get; set; }
    public double Y { get; set; }
    public double Angle { get; set; }
    public double Size { get; set; } = 1;
}

// an album the player made (a trip, a family's story): its name, its icon and the pictures put in it
public sealed class CustomAlbum
{
    public string Id { get; set; } = Guid.NewGuid().ToString("N")[..8];
    public string Name { get; set; } = "";
    public string Icon { get; set; } = "";        // a Segoe Fluent Icons glyph
    public List<string> Pictures { get; set; } = [];
    public string? Household { get; set; }     // the household it belongs to; null: the one most of its pictures belong to
    public string Key => Gallery.Mine + Id;
}

public sealed class GalleryData
{
    public List<CustomAlbum> Custom { get; set; } = [];
    public Dictionary<string, PictureInfo> Pictures { get; set; } = new(StringComparer.OrdinalIgnoreCase);
    public Dictionary<string, AlbumLook> Albums { get; set; } = new(StringComparer.OrdinalIgnoreCase);
    public string? HomePicture { get; set; }    // the picture Home opens on; null: the newest
    public bool Book { get; set; }               // the Gallery last showed the scrapbook
    public string? LastAlbum { get; set; }       // and this album
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

    // the household an album belongs to: the one it was made in, else the one most of its pictures belong to
    public static string? HomeOf(CustomAlbum c, GalleryData d, IReadOnlyList<Picture> pictures, IReadOnlyList<Save> saves) =>
        c.Household ?? pictures.Where(p => c.Pictures.Contains(p.Path, StringComparer.OrdinalIgnoreCase)).Select(p => HouseholdOf(p, d, saves))
            .OfType<string>().GroupBy(h => h, StringComparer.OrdinalIgnoreCase).OrderByDescending(g => g.Count()).FirstOrDefault()?.Key;

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
