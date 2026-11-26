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
            // a picture that moved with Apex's folder keeps what was set on it; one still at its old place stays as it is
            var (was, isNow) = (Path.Combine(root, "Windows", "Apex Radiance", "Screenshots"), Path.Combine(docs, "Apex Radiance", "Screenshots"));
            Directory.CreateDirectory(was);
            Directory.CreateDirectory(isNow);
            File.WriteAllBytes(Path.Combine(isNow, "moved.png"), [1]);
            File.WriteAllBytes(Path.Combine(was, "stays.png"), [1]);
            var followed = new GalleryData { HomePicture = Path.Combine(was, "moved.png"), Custom = { new CustomAlbum { Pictures = { Path.Combine(was, "moved.png"), Path.Combine(was, "stays.png") } } } };
            followed.Pictures[Path.Combine(was, "moved.png")] = new() { Favourite = true };
            Gallery.Follow(followed, Path.Combine(root, "Windows"), docs);
            check(followed.HomePicture == Path.Combine(isNow, "moved.png") && followed.Pictures[Path.Combine(isNow, "moved.png")].Favourite
                  && followed.Custom[0].Pictures.SequenceEqual([Path.Combine(isNow, "moved.png"), Path.Combine(was, "stays.png")]) && Gallery.Load().HomePicture == followed.HomePicture,
                "gallery: a picture that moved with Apex's folder keeps its star, album and place");
            File.Delete(Path.Combine(Paths.Data, "gallery.json"));
            Directory.Delete(Path.Combine(docs, "Apex Radiance"), true);
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

            // Apex's save folders: its own and a chosen one are read too, videos with the pictures, the chosen first
            var apexDir = Path.Combine(docs, "Apex Radiance");
            var mine = Path.Combine(root, "My captures");
            Directory.CreateDirectory(Path.Combine(apexDir, "Videos"));
            Directory.CreateDirectory(mine);
            File.WriteAllText(Path.Combine(apexDir, "ApexRadiance.toml"),
                "[capture]\nnotes = true\n\n    [capture.video]\n    custom_folder = ''\n    folder = \"apex\"\n\n[ui]\nscreenshot_custom_folder = \"" + mine.Replace("\\", "\\\\") + "\"\nscreenshot_folder = \"custom\"\n");
            var clip = Path.Combine(apexDir, "Videos", "Video_1.mp4");
            File.WriteAllBytes(clip, [1]);
            File.SetLastWriteTime(clip, now.AddMinutes(-5));
            var mineShot = Path.Combine(mine, "x.png");
            File.WriteAllBytes(mineShot, [1]);
            File.SetLastWriteTime(mineShot, now.AddMinutes(-10));
            var all = Gallery.List(docs);
            check(Gallery.Folder(docs) == Path.GetFullPath(mine) && Gallery.VideoFolder(docs) == Path.GetFullPath(Path.Combine(apexDir, "Videos"))
                && all.Count == 5 && all[0].Path == clip && all[0].Video && all[1].Path == mineShot && !all[1].Video,
                "gallery: Apex's save folders are read (a chosen one first), videos with the pictures");
            File.Delete(Path.Combine(apexDir, "ApexRadiance.toml"));
            File.Delete(clip);
            File.Delete(mineShot);

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
            check(Gallery.HomeOf(trip, d, list, saves) == "Town" && Gallery.HomeOf(new CustomAlbum(), d, list, saves) is null
                && Gallery.HomeOf(new CustomAlbum { Household = "Arbor", Pictures = [b] }, d, list, saves) == "Arbor",
                "gallery: an album sits in the household it was made in, else the one its pictures belong to");
            check(Fonts.Families("Segoe UI Semibold Italic (TrueType)").Single() == "Segoe UI"
                && Fonts.Families("Cambria & Cambria Math (TrueType)").SequenceEqual(["Cambria", "Cambria Math"])
                && Fonts.Families("Black (TrueType)").Single() == "Black" && Fonts.Installed().Count > 0,
                "gallery: the fonts on this PC by family, styles and file kinds left out");
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
