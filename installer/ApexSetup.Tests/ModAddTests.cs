using System.IO.Compression;
using System.Text;
using ApexSetup;

// Adding files: each kind to its own place, what is already there (identical, another version, inside a merge), archives as
// untrusted input (nested, a bomb, a false name, programs), the temporary files gone, Undo, and worlds with their plan.
static class ModAddTests
{
    const uint Xml = 0x0333406C, Export = 0x05ED1226, Lot = 0x04F66BCC, Family = 0x062853A8, S3sa = 0x073FAA07, Names = 0x0166038C;

    public static void Run(Action<bool, string> check)
    {
        var root = Path.Combine(Path.GetTempPath(), "apex-addtest-" + Guid.NewGuid().ToString("N"));
        var oldLocal = Paths.LocalAppData;
        var oldRecycle = ModMerge.Recycle;
        var oldMax = ModAdd.MaxBytes;
        var oldExtractors = ModConflicts.Extractors;
        try
        {
            Paths.LocalAppData = Path.Combine(root, "Local");
            ModNotes.Reload();
            var recycled = Path.Combine(root, "Recycle");
            Directory.CreateDirectory(recycled);
            ModMerge.Recycle = p => File.Move(p, Path.Combine(recycled, Path.GetFileName(p) + "." + Guid.NewGuid().ToString("N")));
            var docs = Path.Combine(root, "Docs");
            var mods = Path.Combine(docs, "Mods");
            var bin = Path.Combine(root, "Game", "Game", "Bin");
            Directory.CreateDirectory(bin);
            var game = new Game(Path.Combine(root, "Game"), bin, "TS3W.exe", Edition.Steam, "1.67.2");
            Directory.CreateDirectory(Path.Combine(mods, "Packages"));
            File.WriteAllText(Path.Combine(mods, "Resource.cfg"), "Priority 500\nPackedFile Packages/*.package\nPackedFile Packages/*/*.package\n");
            var drop = Path.Combine(root, "drop");
            List<ModFile> list() => Mods.List(game, docs, kinds: false);
            string Put(string name, byte[] data) { var p = Path.Combine(drop, name); Directory.CreateDirectory(Path.GetDirectoryName(p)!); File.WriteAllBytes(p, data); return p; }
            string Zip(string name, params (string Entry, byte[] Data)[] files)
            {
                Directory.CreateDirectory(drop);
                var p = Path.Combine(drop, name);
                using var z = ZipFile.Open(p, ZipArchiveMode.Create);
                foreach (var (entry, data) in files) using (var s = z.CreateEntry(entry).Open()) s.Write(data);
                return p;
            }
            ModAdd Open(params string[] paths) => ModAdd.Open(game, docs, "Packages", paths, list());
            List<AddItem> AddAll(params string[] paths)
            {
                using var add = Open(paths);
                foreach (var i in add.Items.Where(i => i.Ok && i.Add && i.Place != AddPlace.Worlds)) add.Add(i);
                return add.Items;
            }
            int Temps() => Directory.GetDirectories(Path.GetTempPath(), "apex-add-*").Length + Directory.GetDirectories(Path.GetTempPath(), "modscan-*").Length;
            var temps = Temps();
            static string Text(string path) => Encoding.ASCII.GetString(File.ReadAllBytes(path));

            // each kind to its own place
            var sim = Pkg((0x025ED6F4, 1, "sim"));
            var lot = Pkg((Export, 1, "x"), (Lot, 1, "lot"));
            var items = AddAll(Put("a.package", Pkg((Xml, 1, "a one"))), Put("Anna.sim", sim), Put("House.package", lot),
                Put("Smiths.package", Pkg((Export, 1, "x"), (Family, 1, "fam"))), Put("Town.world", Pkg((Export, 1, "x"), (Export, 2, "x"), (Lot, 1, "l"))));
            AddItem Of(string name) => items.Single(i => i.Name == name);
            check(Of("a.package").Added == Path.Combine(mods, "Packages", "a.package") && Of("Anna.sim").Added == Path.Combine(docs, "SavedSims", "Anna.sim")
                && Of("House.package") is { Household: false } h && h.Added == Path.Combine(docs, "Library", "House.package")
                && Of("Smiths.package") is { Household: true } s3 && s3.Added == Path.Combine(docs, "Library", "Smiths.package")
                && Of("Town.world") is { Place: AddPlace.Worlds, Add: true, Added: null },
                "add: a mod to Mods, a Sim to SavedSims, a lot and a family to Library, a world to the game's Worlds");
            check(!ModAdd.IsLibrary(Dbpf.Index(Put("bundle.package", Pkg((Export, 1, "x"), (Export, 2, "x"), (Lot, 1, "l"), (Lot, 2, "l")))))
                && !ModAdd.IsLibrary(Dbpf.Index(Put("script.package", Pkg((Export, 1, "x"), (Lot, 1, "l"), (S3sa, 1, "s")))))
                && !ModAdd.IsLibrary(Dbpf.Index(Put("plain.package", Pkg((Lot, 1, "l"))))),
                "add: a package that bundles exports, a script or a lot's data alone is a mod, not a Library item");
            check(!Directory.EnumerateFiles(docs, "*.apex-part", SearchOption.AllDirectories).Any(), "add: no temporary copy is left beside the files");

            // an identical file is told and unticked; a Sim of the same name with other bytes is kept beside the old one
            var sameSim = AddAll(Put("Anna.sim", sim)).Single();
            var otherSim = AddAll(Put("Anna.sim", Pkg((0x025ED6F4, 1, "sim two")))).Single();
            check(sameSim is { SameAs: "Anna.sim", Add: false, Added: null } && otherSim is { Other: not null, CanReplace: true, Replace: false }
                && otherSim.Added == Path.Combine(docs, "SavedSims", "Anna (2).sim") && Text(Path.Combine(docs, "SavedSims", "Anna.sim")).Contains("sim"),
                "add: the same Sim is not added twice, another of its name gets \" (2)\"");

            // another version of a mod, in another folder and turned off: replaced where it is, its notes kept, the old one recycled
            Directory.CreateDirectory(Path.Combine(mods, "Packages", "Home"));
            File.Move(Path.Combine(mods, "Packages", "a.package"), Path.Combine(mods, "Packages", "Home", "a.package"));
            var rel = Path.Combine("Mods", "Packages", "Home", "a.package");
            ModNotes.SetNotes(rel, "mine");
            Mods.SetEnabled(list().Single(m => m.Rel == rel), false, docs);
            var offPath = list().Single(m => m.Rel == rel).Path;
            var update = AddAll(Put("a.package", Pkg((Xml, 1, "a two")))).Single();
            check(update is { CanReplace: true, Replace: true, OtherMod.Enabled: false } && update.Added == offPath && update.Replaced == offPath && Text(offPath).Contains("a two")
                && Directory.GetFiles(recycled).Any(f => Text(f).Contains("a one")) && ModNotes.Get(rel)?.Notes == "mine" && list().Count(m => m.Name == "a.package") == 1,
                "add: another version takes the old one's place, on or off as it was, the old one goes to the Recycle Bin");
            // the same name with nothing in common (the list of names is not content): kept beside it unless asked
            var other = AddAll(Put("a.package", Pkg((Xml, 77, "another mod"), (Names, 0, "n")))).Single();
            check(other is { CanReplace: true, Replace: false, Replaced: null } && other.Added == Path.Combine(mods, "Packages", "a.package") && Text(offPath).Contains("a two"),
                "add: a different mod of the same name is added beside the old one");
            // a replace that can't recycle the old one changes nothing
            var keep = ModMerge.Recycle;
            ModMerge.Recycle = _ => throw new SetupException("Windows couldn't move the file to the Recycle Bin.");
            var was = File.ReadAllBytes(other.Added!);
            using (var add = Open(Put("a.package", Pkg((Xml, 77, "third")))))
            {
                var threw = false;
                try { add.Items[0].Replace = true; add.Add(add.Items[0]); } catch (SetupException) { threw = true; }
                check(threw && add.Items[0].Added is null && File.ReadAllBytes(other.Added!).SequenceEqual(was) && Directory.GetFiles(Path.Combine(mods, "Packages")).Length == 1,
                    "add: an old version that can't go to the Recycle Bin stays, and nothing is left");
            }
            ModMerge.Recycle = keep;
            // a file that changed since it was shown is not added
            var moving = Put("moving.package", Pkg((Xml, 5, "before")));
            using (var add = Open(moving))
            {
                File.WriteAllBytes(moving, Pkg((Xml, 5, "after!")));
                var threw = false;
                try { add.Add(add.Items[0]); } catch (SetupException) { threw = true; }
                check(threw && !File.Exists(Path.Combine(mods, "Packages", "moving.package")) && Directory.GetFiles(Path.Combine(mods, "Packages")).Length == 1, "add: a file that changed since it was shown is refused");
            }

            // no framework yet: the standard Resource.cfg goes in with the first mod, so the game loads it (a Sim needs none)
            var bare = Path.Combine(root, "Bare");
            using (var add = ModAdd.Open(game, bare, "Packages", [Put("Solo.sim", sim), Put("solo.package", Pkg((Xml, 90, "solo")))], []))
            {
                add.Add(add.Items[0]);
                var none = !Directory.Exists(Path.Combine(bare, "Mods"));
                add.Add(add.Items[1]);
                check(none && ModLibrary.LoadedBy(bare) is { } by && by(Path.GetRelativePath(Path.Combine(bare, "Mods"), add.Items[1].Added!)), "add: the first mod of a game without Resource.cfg brings it, and the game loads that mod");
            }

            // a copy that is on in a subfolder and one that is off where the new one goes: the one the game loads is replaced
            Directory.CreateDirectory(Path.Combine(mods, "Packages", "Sub"));
            Directory.CreateDirectory(Path.Combine(Mods.Disabled(docs), "Packages"));
            var dupOn = Path.Combine(mods, "Packages", "Sub", "dup.package");
            var dupOff = Path.Combine(Mods.Disabled(docs), "Packages", "dup.package");
            File.WriteAllBytes(dupOn, Pkg((Xml, 91, "one")));
            File.WriteAllBytes(dupOff, Pkg((Xml, 91, "two")));
            // (whatever order the list is in)
            foreach (var order in new[] { list(), list().AsEnumerable().Reverse().ToList() })
                using (var add = ModAdd.Open(game, docs, "Packages", [Put("dup.package", Pkg((Xml, 91, "three"))), Put("copy.package", Pkg((Xml, 91, "two")))], order))
                    check(add.Items[0].Other == dupOn && add.Items[0].OtherMod!.Enabled && add.Items[1] is { SameAs: "dup.package", OtherMod.Enabled: false }, "add: of two copies, the one that is on is replaced; the same file turned off is told so");
            File.Delete(dupOn);
            File.Delete(dupOff);
            Directory.Delete(Path.Combine(mods, "Packages", "Sub"));

            // Undo: the file added goes to the Recycle Bin, one changed since stays
            var two = AddAll(Put("u1.package", Pkg((Xml, 21, "u1"))), Put("u2.package", Pkg((Xml, 22, "u2"))));
            File.AppendAllText(two[1].Added!, "edited");
            ModAdd.Undo(two);
            check(!File.Exists(two[0].Added) && File.Exists(two[1].Added), "add: Undo takes back what was added and leaves a file changed since");
            File.Delete(two[1].Added!);

            // inside a merge: the same bytes are told, another version is told and added
            File.WriteAllBytes(Path.Combine(mods, "Packages", "m1.package"), Pkg((Xml, 31, "m one")));
            File.WriteAllBytes(Path.Combine(mods, "Packages", "m2.package"), Pkg((Xml, 32, "m two")));
            ModMerge.Run(ModMerge.Plan(docs, list().Where(m => m.Name is "m1.package" or "m2.package").ToList(), "Both", "Packages", false), null, default);
            using (var add = Open(Put("m1.package", Pkg((Xml, 31, "m one"))), Put("m2.package", Pkg((Xml, 32, "m two b"))), Put("Both.package", Pkg((Xml, 31, "fake")))))
                check(add.Items[0] is { SameAs: "Both", SameInMerge: true, Add: false } && add.Items[1] is { OtherMerge: "Both", Add: true, Other: null }
                    && add.Items[2] is { Other: not null, CanReplace: false },
                    "add: a file already in a merge is told, another version of it too, and the merge itself is never replaced");

            // archives: a nested one is opened, the next level is not; programs are named and never taken out, a readme passes over
            byte[] Nest(params (string, byte[])[] f) { var p = Zip(Guid.NewGuid().ToString("N") + ".tmp", f); var b = File.ReadAllBytes(p); File.Delete(p); return b; }
            var deep = Nest(("deep.package", Pkg((Xml, 41, "deep"))));
            using (var add = Open(Zip("pack.zip", ("readme.txt", [1]), ("pic.png", [1]), ("__MACOSX/._x.package", [1]), ("setup.exe", [1]), ("plug.asi", [1]), ("house.Sims3Pack", [1]),
                       ("sub/in.package", Pkg((Xml, 42, "in"))), ("sub/inner.zip", Nest(("two.package", Pkg((Xml, 43, "two"))), ("three.zip", deep))), ("..\\..\\out.sim", sim))))
            {
                string P(string n) => add.Items.SingleOrDefault(i => i.Name == n) is { } i ? i.Problem ?? "ok" : "gone";
                check(P("readme.txt") == "gone" && P("pic.png") == "gone" && P("._x.package") == "gone" && P("setup.exe").StartsWith("A program") && P("plug.asi").StartsWith("A program")
                    && P("house.Sims3Pack").StartsWith("Install it with") && P("in.package") == "ok" && P("two.package") == "ok" && P("three.zip").Contains("too deep") && P("out.sim") == "ok"
                    && add.Items.Where(i => i.Ok).All(i => i.Archive == "pack.zip" && Path.GetFileName(Path.GetDirectoryName(i.File)!).StartsWith("apex-add-")),
                    "add: a zip in a zip is opened and no deeper, programs and the game's packs are told, the rest is passed over | " + string.Join(" | ", add.Items.Select(i => i.Name + "=" + (i.Problem ?? "ok"))));
            }
            // what comes out of one archive is limited, nested ones together, and none of a refused archive goes in
            ModAdd.MaxBytes = 100_000;
            var big = new byte[90_000];
            using (var add = Open(Zip("bomb.zip", ("ok.package", Pkg((Xml, 51, "ok"))), ("a.zip", Nest(("big.package", big))), ("b.zip", Nest(("big.package", big))))))
                check(add.Items is [{ Name: "bomb.zip", Problem: { } p1 }] && p1.Contains("too big"), "add: an archive past the limit is refused whole | " + string.Join(" | ", add.Items.Select(i => i.Name + "=" + i.Problem)));
            ModAdd.MaxBytes = oldMax;
            // a tar or text under an archive's name, a cut zip, an empty one
            using (var add = Open(Put("text.zip", "hello there"u8.ToArray()), Put("fake.7z", new byte[1024]), Put("cut.zip", File.ReadAllBytes(Path.Combine(drop, "pack.zip"))[..200]), Zip("none.zip", ("readme.txt", [1])), Path.Combine(drop, "missing.package")))
                check(add.Items.Count == 5 && add.Items.Take(3).All(i => i.Problem!.StartsWith("The archive can't be opened")) && add.Items[3].Problem!.StartsWith("It holds no mod") && add.Items[4].Problem == "The file could not be read.",
                    "add: an archive that is not one, a cut one, an empty one and a missing file are told | " + string.Join(" | ", add.Items.Select(i => i.Name + "=" + i.Problem)));
            ModConflicts.Extractors = () => [];
            using (var add = Open(Put("x.rar", [.. "Rar!"u8, 0x1A, 7, 0, 1, 2, 3])))
                check(add.Items.Single().Problem!.Contains("7-Zip"), "add: a rar with no extractor says what to install");
            ModConflicts.Extractors = oldExtractors;
            // a real .7z, through Windows' tar.exe
            var tar = Path.Combine(Environment.SystemDirectory, "tar.exe");
            var seven = Path.Combine(drop, "real.7z");
            Put(@"src\from7z.package", Pkg((Xml, 61, "seven")));
            Put(@"src\run.exe", [1]);
            if (File.Exists(tar) && Tar(tar, "--format", "7zip", "-cf", seven, "-C", Path.Combine(drop, "src"), "from7z.package", "run.exe"))
            {
                var r = AddAll(seven);
                check(r.Single(i => i.Name == "from7z.package").Added == Path.Combine(mods, "Packages", "from7z.package") && r.Single(i => i.Name == "run.exe").Added is null,
                    "add: a .7z's package is added and its program is not");
            }
            else Console.WriteLine("skip add .7z: this tar.exe cannot write 7z");
            // a folder, and a name Windows keeps for a device
            Put(@"folder\sub\f.package", Pkg((Xml, 71, "f")));
            Put(@"folder\notes.txt", [1]);
            using (var add = Open(Path.Combine(drop, "folder"), Zip("con.zip", ("con.package", Pkg((Xml, 72, "c"))))))
                check(add.Items is [{ Name: "f.package", Ok: true }, { Name: "con.package", Ok: false }], "add: a folder's mods are found at any depth, a device's name is refused");
            // canceled while reading
            var canceled = false;
            try { ModAdd.Open(game, docs, "Packages", [seven, Path.Combine(drop, "pack.zip")], list(), null, new CancellationToken(true)); } catch (OperationCanceledException) { canceled = true; }
            check(canceled && Temps() == temps, $"add: nothing temporary is left, canceled or not ({Temps()} of {temps})");

            // worlds: the plan never takes a name in use, and what the administrator copy is given is checked again there
            var worlds = ModAdd.WorldsDir(game);
            Directory.CreateDirectory(worlds);
            var town = Put("Town.world", Pkg((Export, 1, "x"), (Export, 2, "y")));
            using (var add = Open(town))
            {
                var plan = add.WorldPlan(add.Items);
                ModAdd.AddWorlds(game, plan);
                add.WorldsDone(add.Items, plan);
                check(add.Items[0].Added == Path.Combine(worlds, "Town.world") && !ModAdd.WorldsNeedAdmin(game), "add: a world goes to the game's Worlds folder");
            }
            using (var add = Open(town, Put(@"v2\Town.world", Pkg((Export, 1, "x"), (Export, 2, "other")))))
            {
                check(add.Items[0] is { SameAs: "Town.world", Add: false } && add.Items[1] is { Other: not null, CanReplace: false, Add: false }, "add: a world already there is told, and one of its name never replaces it");
                var plan = add.WorldPlan([add.Items[1]]);
                check(plan["name.0"] == "Town (2).world", "add: a second world of the same name gets \" (2)\"");
                bool Refused(string key, string value)
                {
                    try { ModAdd.AddWorlds(game, new Dictionary<string, string>(plan) { [key] = value }); return false; }
                    catch (Exception e) when (e is SetupException or IOException) { return true; }
                }
                var bad = Put("bad.world", "not a package at all"u8.ToArray());
                check(Refused("name.0", @"..\..\Town (2).world") && Refused("name.0", "Town.exe") && Refused("name.0", "Town.world") && Refused("sha.0", "00") && Refused("file.0", bad)
                    && Directory.GetFiles(worlds).Length == 1 && !File.Exists(Path.Combine(game.Root, "GameData", "Shared", "Town (2).world")),
                    "add: a world plan with a path, another type, a name in use or other bytes is refused, and nothing is left");
            }
            // (a Worlds folder that leads elsewhere; a link needs no rights as a junction)
            Directory.Delete(worlds, true);
            var elsewhere = Path.Combine(root, "elsewhere");
            Directory.CreateDirectory(elsewhere);
            using (var mk = System.Diagnostics.Process.Start(new System.Diagnostics.ProcessStartInfo("cmd.exe", $"/c mklink /J \"{worlds}\" \"{elsewhere}\"") { UseShellExecute = false, CreateNoWindow = true, RedirectStandardOutput = true, RedirectStandardError = true })!) mk.WaitForExit();
            if (Directory.Exists(worlds))
            {
                var refused = false;
                using (var add = Open(town))
                    try { ModAdd.AddWorlds(game, add.WorldPlan(add.Items)); } catch (SetupException) { refused = true; }
                check(refused && Directory.GetFiles(elsewhere).Length == 0, "add: nothing is written through a Worlds folder that is a link");
                Directory.Delete(worlds);
            }
        }
        catch (Exception e) { check(false, "add threw: " + e); }
        finally
        {
            Paths.LocalAppData = oldLocal;
            ModMerge.Recycle = oldRecycle;
            ModAdd.MaxBytes = oldMax;
            ModConflicts.Extractors = oldExtractors;
            ModNotes.Reload();
            try { Directory.Delete(root, true); } catch (Exception) { }
        }
    }

    static bool Tar(string tar, params string[] args)
    {
        var psi = new System.Diagnostics.ProcessStartInfo(tar) { UseShellExecute = false, CreateNoWindow = true, RedirectStandardError = true, RedirectStandardOutput = true };
        foreach (var a in args) psi.ArgumentList.Add(a);
        using var p = System.Diagnostics.Process.Start(psi)!;
        p.WaitForExit(60_000);
        return p.HasExited && p.ExitCode == 0;
    }

    // a small DBPF 2.0 package, as the game writes it (header, data, index with no constant fields)
    static byte[] Pkg(params (uint Type, ulong Instance, string Data)[] resources)
    {
        using var ms = new MemoryStream();
        var w = new BinaryWriter(ms);
        w.Write("DBPF"u8.ToArray()); w.Write(2u); w.Write(0u); w.Write(new byte[96 - 12]);
        var at = new List<uint>();
        foreach (var r in resources) { at.Add((uint)ms.Position); w.Write(Encoding.ASCII.GetBytes(r.Data)); }
        var index = (uint)ms.Position;
        w.Write(0u);
        for (int i = 0; i < resources.Length; i++)
        {
            var (type, instance, data) = resources[i];
            w.Write(type); w.Write(0u); w.Write((uint)(instance >> 32)); w.Write((uint)instance); w.Write(at[i]);
            w.Write((uint)data.Length | 0x80000000u); w.Write((uint)data.Length); w.Write((ushort)0); w.Write((ushort)1);
        }
        var end = (uint)ms.Position;
        ms.Position = 0x24; w.Write((uint)resources.Length);
        ms.Position = 0x2C; w.Write(end - index);
        ms.Position = 0x40; w.Write(index);
        return ms.ToArray();
    }
}
