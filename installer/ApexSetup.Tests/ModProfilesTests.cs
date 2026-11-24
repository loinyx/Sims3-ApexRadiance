using ApexSetup;

// Profiles of mods (what a switch does, all or nothing, what follows a move, a merge and a split, the saved file), the web
// address of a mod's page, and finding s3pe.
static class ModProfilesTests
{
    public static void Run(Action<bool, string> check)
    {
        var root = Path.Combine(Path.GetTempPath(), "apex-profiles-" + Guid.NewGuid().ToString("N"));
        var (oldLocal, oldRecycle, oldDelay, oldRunning) = (Paths.LocalAppData, ModMerge.Recycle, Fs.RetryDelay, Games.IsRunning);
        try
        {
            Paths.LocalAppData = Path.Combine(root, "Local");
            ModNotes.Reload();
            var docs = Path.Combine(root, "Docs");
            var mods = Path.Combine(docs, "Mods");
            var bin = Path.Combine(root, "Game", "Game", "Bin");
            Directory.CreateDirectory(bin);
            var game = new Game(Path.Combine(root, "Game"), bin, "TS3W.exe", Edition.Steam, "1.67.2");
            Directory.CreateDirectory(Path.Combine(mods, "Packages", "Sub"));
            File.WriteAllText(Path.Combine(mods, "Resource.cfg"), "Priority 500\nPackedFile Packages/*.package\nPackedFile Packages/*/*.package\n");
            string At(string name) => Path.Combine(mods, "Packages", name + ".package");
            string Rel(string name) => Path.Combine("Mods", "Packages", name + ".package");
            var n = 1;
            void Add(string name) => File.WriteAllBytes(At(name), ModToolsTests.Dbpf20(n++));
            foreach (var name in new[] { "a", "b", "c", "d" }) Add(name);
            List<ModFile> List() => Mods.List(game, docs, kinds: false);
            ModFile M(string name) => List().Single(m => m.Rel == Rel(name));
            string On() => string.Join(",", List().Where(m => m.Enabled).Select(m => Path.GetFileNameWithoutExtension(m.Name)).Order());
            ModProfile P(string name) => ModProfiles.All().Single(p => p.Name == name);
            ModProfiles.Plan Plan(string name) => ModProfiles.PlanFor(P(name), List(), docs);
            List<ModFile> Switch(string name) { var p = Plan(name); return ModProfiles.Apply(p.TurnOn, p.TurnOff, docs); }
            static bool Refused(Action a) { try { a(); return false; } catch (SetupException) { return true; } }

            // saved, named, copied; a mod the launcher manages is not in it
            check(ModProfiles.Active() is null && ModProfiles.All().Count == 0, "profiles: none at first");
            ModProfiles.Create("  Everything ", [.. List(), new ModFile(At("own"), Rel("own"), 1, ModKind.Package, true, true)]);
            Mods.SetEnabled(M("c"), false, docs);
            Mods.SetEnabled(M("d"), false, docs);
            ModProfiles.Create("Light", List());
            ModProfiles.Reload();
            check(ModProfiles.Active() == "Light" && P("Everything").Mods.Count == 4 && P("Everything").Mods.Values.All(x => x) && P("Light").Mods[Rel("c")] == false
                && Plan("Light") is { Same: true, On: 2, Gone.Count: 0, Left.Count: 0 } && Plan("Everything") is { On: 4, TurnOn.Count: 2, TurnOff.Count: 0 },
                "profiles: saved and read back, each with what was on; a mod the launcher manages is left out");
            check(Refused(() => ModProfiles.Create("light", List())) && Refused(() => ModProfiles.Create(" ", List())) && Refused(() => ModProfiles.Create(new string('x', 41), List()))
                && ModProfiles.Duplicate("Light") == "Light (2)" && ModProfiles.Create("Light", List(), free: true, activate: false) == "Light (3)" && ModProfiles.Active() == "Light"
                && Refused(() => ModProfiles.Rename("Light (2)", "Light")) && ModProfiles.All().Select(p => p.Name).SequenceEqual(["Everything", "Light", "Light (2)", "Light (3)"]),
                "profiles: a name taken, empty or too long is refused; a copy takes a free name");
            ModProfiles.Delete("Light (3)");
            ModProfiles.Delete("Light (2)");

            // switched, and back with what Apply returned
            var done = Switch("Everything");
            var all = On();
            ModProfiles.Apply(done.Where(m => !m.Enabled).ToList(), done.Where(m => m.Enabled).ToList(), docs);
            check(all == "a,b,c,d" && On() == "a,b" && Plan("Light").Same, "profiles: a switch turns on what it has, Undo puts it back");

            // one mod held open by another program: none is switched
            Fs.RetryDelay = TimeSpan.FromMilliseconds(1);
            ModProfiles.Create("Only c", [M("a") with { Enabled = false }, M("b") with { Enabled = false }, M("c") with { Enabled = true }, M("d") with { Enabled = false }]);
            foreach (var held in new[] { "b", "c" })
            {
                var failed = false;
                using (File.Open(M(held).Path, FileMode.Open, FileAccess.Read, FileShare.None))
                    try { Switch("Only c"); } catch (Exception e) when (e is IOException or UnauthorizedAccessException) { failed = true; }
                check(failed && On() == "a,b", $"profiles: {held} held open by another program leaves every mod as it was");
            }
            Switch("Only c");
            check(On() == "c", "profiles: the same switch once it is free");
            Switch("Everything");

            // not while the game or a hunt runs, nor is what a hunt turned off saved
            Games.IsRunning = () => true;
            var game1 = Refused(() => Switch("Only c"));
            Games.IsRunning = () => false;
            ModHunt.Start(List(), docs);
            var hunt1 = Refused(() => ModProfiles.CheckCanSwitch()) && Refused(() => ModProfiles.Create("Half", List())) && Refused(() => ModProfiles.Update("Light", List()));
            ModHunt.Stop(List(), docs);
            check(game1 && hunt1 && On() == "a,b,c,d" && ModProfiles.All().Count == 3, "profiles: refused while the game or a hunt for a problem mod runs");

            // a mod gone is told, one added since is left as it is; an on and an off copy at one place are left alone
            File.Move(At("d"), Path.Combine(root, "d.package"));
            Add("new");
            Directory.CreateDirectory(Path.GetDirectoryName(Path.Combine(Mods.Disabled(docs), "Packages", "b.package"))!);
            File.WriteAllText(Path.Combine(Mods.Disabled(docs), "Packages", "b.package"), "twin");
            var odd = Plan("Everything");
            var offNew = Plan("Only c");
            check(odd.Gone.SequenceEqual([Rel("d")]) && odd.Left.Count == 3 && odd.Same && offNew.TurnOff.Select(m => m.Name).SequenceEqual(["a.package"]) && offNew.Gone.Count == 0,
                "profiles: a mod gone is told, one added since and two at one place are left as they are");
            File.Delete(Path.Combine(Mods.Disabled(docs), "Packages", "b.package"));
            File.Move(Path.Combine(root, "d.package"), At("d"));
            File.Delete(At("new"));

            // a mod renamed, moved, or in a folder renamed in the launcher stays in every profile
            var moved = ModLibrary.Move(M("d"), docs, Path.Combine("Packages", "Sub"));
            moved = ModLibrary.Rename(moved, "dd");
            ModLibrary.RenameFolder(docs, Path.Combine("Packages", "Sub"), "Sub2");
            ModProfiles.Reload();
            var nowAt = Path.Combine("Mods", "Packages", "Sub2", "dd.package");
            check(Plan("Everything") is { Same: true, Gone.Count: 0, Left.Count: 0 } && P("Everything").Mods[nowAt] && !P("Only c").Mods[nowAt] && P("Everything").Mods.Count == 4,
                "profiles: a mod moved or renamed in the launcher follows in every profile");
            ModLibrary.Move(ModLibrary.Rename(List().Single(m => m.Rel == nowAt), "d"), docs, "Packages");

            // a merge made since follows its mods when they agree; split, its mods are as the merge was
            var recycled = Path.Combine(root, "Recycle");
            Directory.CreateDirectory(recycled);
            ModMerge.Recycle = p => File.Move(p, Path.Combine(recycled, Guid.NewGuid().ToString("N")));
            var ab = ModMerge.Run(ModMerge.Plan(docs, [M("a"), M("b")], "ab", "Packages", false), null, default).Merged;
            var agree = Plan("Only c");
            var told = Plan("Everything");
            check(agree.TurnOff.Select(m => m.Name).Order().SequenceEqual(["ab.package", "d.package"]) && agree.Left.Count == 0 && told is { Same: true, Gone.Count: 0 },
                "profiles: a merge is on or off as its mods are in the profile, and they are not gone");
            ModProfiles.Create("Merged", List().Select(m => m.Rel == ab.Rel ? m with { Enabled = false } : m).ToList());
            ModMerge.Split(docs, M("ab"));
            check(!P("Merged").Mods.ContainsKey(ab.Rel) && P("Merged").Mods[Rel("a")] == false && P("Merged").Mods[Rel("c")] && Plan("Merged").TurnOff.Select(m => m.Name).Order().SequenceEqual(["a.package", "b.package"]),
                "profiles: a split merge leaves its mods as the merge was");
            ModMerge.Run(ModMerge.Plan(docs, [M("a"), M("c")], "ac", "Packages", false), null, default);
            check(Plan("Only c").Left.Select(m => m.Name).SequenceEqual(["ac.package"]), "profiles: a merge of mods that don't agree is left as it is");
            ModMerge.Split(docs, M("ac"));
            ModProfiles.Delete("Merged");

            // updated and put back; the active one follows a rename and goes with a delete
            Mods.SetEnabled(M("a"), false, docs);
            ModProfiles.SetActive("Everything");
            var before = ModProfiles.Update("Light", List());
            var updated = Plan("Light").Same && ModProfiles.Active() == "Light";
            ModProfiles.Put("Light", before, "Everything");
            ModProfiles.Rename("Everything", "All of it");
            check(updated && !Plan("Light").Same && ModProfiles.Active() == "All of it", "profiles: updated with what is on, undone; the active one follows a rename");
            ModProfiles.Delete("All of it");
            check(ModProfiles.Active() is null && Refused(() => ModProfiles.Rename("All of it", "x")), "profiles: the active one goes with a delete");

            // a file that can't be read is never written over; a damaged one is kept beside; odd entries are dropped
            var file = Path.Combine(Paths.Data, "mod-profiles.json");
            var bytes = File.ReadAllBytes(file);
            ModProfiles.Reload();
            var locked = false;
            using (File.Open(file, FileMode.Open, FileAccess.Read, FileShare.None))
                try { ModProfiles.Create("Lost", List()); } catch (IOException) { locked = true; }
            check(locked && File.ReadAllBytes(file).SequenceEqual(bytes) && ModProfiles.All().Count == 2, "profiles: a file held a moment by another program is not written over");
            File.WriteAllText(file, "{ not json");
            ModProfiles.Reload();
            ModProfiles.Create("New", List());
            check(File.ReadAllText(file + ".corrupt") == "{ not json" && ModProfiles.All().Count == 1, "profiles: a damaged file is kept beside");
            File.WriteAllText(file, """{ "Active": "gone", "Profiles": [ null, { "Name": null }, { "Name": "x", "Mods": null }, { "Name": "X" }, { "Name": "y", "Mods": { "Mods\\a": true, "mods\\A": false } } ] }""");
            ModProfiles.Reload();
            check(ModProfiles.All().Select(p => p.Name).SequenceEqual(["x", "y"]) && P("y").Mods.Count == 1 && ModProfiles.Active() is null, "profiles: odd entries in the file are dropped, not an error");

            // a mod's page: a web address only, kept with the mod when it moves and through a merge
            foreach (var (text, want) in new (string, string?)[] { ("https://example.com/mod", "https://example.com/mod"), (" example.com/a b ", "https://example.com/a%20b"), ("http://example.com", "http://example.com/"),
                         ("javascript:alert(1)", null), ("file:///C:/x.exe", null), (@"C:\x.exe", null), (@"\\server\x", null), ("ms-settings:about", null), ("mailto:a@example.com", null), ("https://user:pw@example.com", null),
                         ("ftp://example.com", null), ("https://", null), ("nothing", null), ("", null), ("https://example.com/" + new string('x', 2000), null) })
                check(ModNotes.WebLink(text) == want, $"link: \"{(text.Length > 60 ? text[..60] : text)}\" {(want is null ? "refused" : "kept as " + want)}");
            ModNotes.SetLink(Rel("b"), "example.com/b");
            var bad = Refused(() => ModNotes.SetLink(Rel("b"), "file:///C:/x.exe"));
            var went = ModLibrary.Rename(M("b"), "bb");
            var kept = ModNotes.LinkOf(went.Rel);
            ModLibrary.Rename(went, "b");
            ModMerge.Run(ModMerge.Plan(docs, [M("b"), M("c")], "bc", "Packages", false), null, default);
            var inside = ModNotes.LinkOf(Rel("b"));
            ModMerge.Split(docs, M("bc"));
            check(bad && kept == "https://example.com/b" && inside is null && ModNotes.LinkOf(Rel("b")) == kept, "link: follows a rename, and comes back with its mod after a merge");
            ModNotes.SetLink(Rel("b"), null);
            check(ModNotes.Get(Rel("b")) is null, "link: removed, nothing is kept");

            // s3pe: only an s3pe.exe that is there, from a folder, a quoted path, an icon or its uninstaller
            var dir = Path.Combine(root, "s3pe folder");
            Directory.CreateDirectory(dir);
            var exe = Path.Combine(dir, "s3pe.exe");
            var none = S3pe.Pick([dir, exe]);
            File.WriteAllText(exe, "");
            check(none is null && new[] { dir, dir + @"\", exe, $"\"{exe}\"", exe + ",0", $"\"{Path.Combine(dir, "uninst-s3pe.exe")}\" /S" }.All(x => S3pe.Pick([null, "", "s3pe.exe", "MsiExec.exe /X{0}", x]) == exe)
                && S3pe.Pick(["s3pe.exe", @"C:\no such folder\s3pe.exe", "bad\0path"]) is null, "s3pe: found only where its program is");
        }
        catch (Exception e) { check(false, "profiles: exception " + e); }
        finally
        {
            (ModMerge.Recycle, Fs.RetryDelay, Games.IsRunning, Paths.LocalAppData) = (oldRecycle, oldDelay, oldRunning, oldLocal);
            ModNotes.Reload();
            try { Directory.Delete(root, true); } catch (Exception) { }
        }
    }
}
