using ApexSetup;

// The Gallery: the pictures listed newest first, each household guessed from the save written right after, a household
// set by hand winning, the albums, and what the player set (favourites, captions, scrapbook looks, notes) kept.
static class GalleryTests
{
    public static void Run(Action<bool, string> check)
    {
        var root = Path.Combine(Path.GetTempPath(), "apex-gallery-" + Guid.NewGuid().ToString("N"));
        var oldLocal = Paths.LocalAppData;
        try
        {
            Paths.LocalAppData = Path.Combine(root, "Local");
            var docs = Path.Combine(root, "Docs");
            var shots = Gallery.Folder(docs);
            Directory.CreateDirectory(shots);
            var now = DateTime.Now;
            string Shot(string name, DateTime when) { var p = Path.Combine(shots, name); File.WriteAllBytes(p, [1, 2, 3]); File.SetLastWriteTime(p, when); return p; }
            var a = Shot("a.png", now.AddDays(-3));
            var b = Shot("b.jpg", now.AddDays(-1));
            var c = Shot("c.png", now.AddHours(-1));
            File.WriteAllText(Path.Combine(shots, "notes.txt"), "not a picture");
            var list = Gallery.List(docs);
            check(list.Count == 3 && list[0].Path == c && list[2].Path == a, "gallery: pictures only, newest first");

            var saves = new List<Save> { new(Path.Combine(docs, "Saves", "Town.sims3"), "Town", now.AddDays(-2)), new(Path.Combine(docs, "Saves", "Arbor.sims3"), "Arbor", now.AddMinutes(-30)) };
            check(Gallery.GuessHousehold(list[2], saves) == "Town" && Gallery.GuessHousehold(list[0], saves) == "Arbor" && Gallery.GuessHousehold(list[1], saves) == "Arbor",
                "gallery: a picture goes to the save written soonest after it");
            check(Gallery.GuessHousehold(new Picture(c, now.AddHours(1)), saves) is null, "gallery: a picture newer than every save has no household yet");

            var d = new GalleryData();
            Gallery.Info(d, b).Household = "Town";
            Gallery.Info(d, a).Favourite = true;
            check(Gallery.HouseholdOf(list[1], d, saves) == "Town", "gallery: a household set by hand wins over the guess");
            check(list.Count(p => Gallery.InAlbum(p, "Town", d, saves)) == 2 && list.Count(p => Gallery.InAlbum(p, Gallery.Favourites, d, saves)) == 1
                && list.Count(p => Gallery.InAlbum(p, Gallery.All, d, saves)) == 3, "gallery: albums by household, favourites and all");

            var trip = new CustomAlbum { Name = "Trip to Egypt", Icon = "" };
            d.Custom.Add(trip);
            Gallery.Toggle(d, trip, b);
            Gallery.Toggle(d, trip, c);
            Gallery.Toggle(d, trip, c);
            check(list.Count(p => Gallery.InAlbum(p, trip.Key, d, saves)) == 1 && Gallery.CustomOf(d, trip.Key) == trip, "gallery: the player's own albums hold the pictures put in them, a second toggle takes one out");
            var look = Gallery.Look(d, "Town");
            look.Background = "cork";
            look.Font = "Segoe Print";
            look.Places[a] = new Placed { X = 120, Y = 80, Angle = -4 };
            look.Notes.Add(new StickyNote { Text = "first week", Color = "pink", X = 300, Y = 40, Angle = 3 });
            Gallery.Info(d, c).Caption = "the kitchen";
            d.HomePicture = c;
            Gallery.Save(d);
            var back = Gallery.Load();
            check(back.Albums["Town"].Background == "cork" && back.Albums["Town"].Font == "Segoe Print" && back.Albums["Town"].Places[a].Angle == -4
                && back.Albums["Town"].Notes.Single().Text == "first week" && back.Pictures[c].Caption == "the kitchen" && back.HomePicture == c && back.Pictures[a].Favourite && back.Custom.Single().Name == "Trip to Egypt" && back.Custom[0].Pictures.Single() == b,
                "gallery: looks, places, notes, captions, favourites and the Home picture are kept");
        }
        finally
        {
            Paths.LocalAppData = oldLocal;
            try { Directory.Delete(root, true); } catch (IOException) { }
        }
    }
}
