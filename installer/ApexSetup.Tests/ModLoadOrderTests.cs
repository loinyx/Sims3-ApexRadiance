using System.Text;
using ApexSetup;

// Load order's changes on a fake Mods folder: the natural order writes nothing, an order of the player's is read by the game
// as wanted (files numbered under the launcher's folder, one Resource.cfg line), a second order renumbers without nesting,
// moves across sections, priorities, a new priority folder, a file made read, and every Undo byte for byte; a file held
// open midway puts back what was done.
static class ModLoadOrderTests
{
    public static void Run(Action<bool, string> check)
    {
        var root = Path.Combine(Path.GetTempPath(), "apex-loadorder-" + Guid.NewGuid().ToString("N"));
        var oldLocal = Paths.LocalAppData;
        try
        {
            Paths.LocalAppData = Path.Combine(root, "Local");
            ModNotes.Reload();
            var docs = Path.Combine(root, "Docs");
            var mods = Path.Combine(docs, "Mods");
            var bin = Path.Combine(root, "Game", "Game", "Bin");
            Directory.CreateDirectory(bin);
            var game = new Game(Path.Combine(root, "Game"), bin, "TS3W.exe", Edition.Steam, "1.67.2");
            var cfgPath = ModLoadOrder.CfgPath(docs);
            // (a byte order mark, a comment with UTF-8 in it and Windows line ends: all kept)
            var original = Encoding.UTF8.GetPreamble().Concat(Encoding.UTF8.GetBytes(
                "# mine: ção\r\nPriority 1000\r\nPackedFile Overrides/*.package\r\nPackedFile Overrides/*/*.package\r\n\r\nPriority 500\r\nPackedFile Packages/*.package\r\nPackedFile Packages/*/*.package\r\n")).ToArray();
            Directory.CreateDirectory(mods);
            File.WriteAllBytes(cfgPath, original);
            foreach (var f in new[] { "Overrides/a", "Overrides/b", "Overrides/c", "Packages/d", "Packages/e", "Stray/x", "Packages/A/B/y" })
                Write(Path.Combine(mods, f.Replace('/', '\\') + ".package"), f);
            ModNotes.CreateTag("kept", "#888888");
            ModNotes.SetTag(Path.Combine("Mods", "Overrides", "a.package"), "kept", true);

            List<ModFile> List() => Mods.List(game, docs, kinds: false);
            // the mods the game reads, in its order, each with its section
            List<(ModFile Mod, int Section)> Now()
            {
                var dirs = ModConflicts.ParseResourceCfg(File.ReadAllText(cfgPath));
                return List().Select(m => (m, ModConflicts.LoadKey(dirs, ModLoadOrder.Under(m)))).Where(x => x.Item2 is not null)
                    .OrderBy(x => x.Item2!.Value, Comparer<(int, int, string)>.Create(ModConflicts.CompareKey)).Select(x => (x.m, x.Item2!.Value.Item2)).ToList();
            }
            string Names() => string.Join(",", Now().Select(x => File.ReadAllText(x.Mod.Path).Split('/')[^1]));
            // the order wanted by the files' contents (their first names)
            List<(ModFile, int)> Want(params string[] order)
            {
                var now = Now();
                return order.Select(o => { var at = o.Split('@'); var x = now.Single(n => File.ReadAllText(n.Mod.Path).EndsWith("/" + at[0])); return (x.Mod, at.Length > 1 ? int.Parse(at[1]) : x.Section); }).ToList();
            }
            var files0 = Snapshot(mods);

            check(ModLoadOrder.Apply(docs, Now()) is null && File.ReadAllBytes(cfgPath).SequenceEqual(original) && Snapshot(mods).SequenceEqual(files0),
                "load order: the order the game reads already writes nothing");

            var u1 = ModLoadOrder.Apply(docs, Want("c", "a", "b", "d", "e"))!;
            var text1 = File.ReadAllText(cfgPath);
            var dir = Path.Combine(mods, ModLoadOrder.FolderName, "Overrides");
            check(Names() == "c,a,b,d,e" && File.Exists(Path.Combine(dir, "001 c.package")) && File.Exists(Path.Combine(dir, "003 b.package"))
                && text1.Contains("Priority 1000\r\nPackedFile ApexLoadOrder/Overrides/*.package\r\nPackedFile Overrides/*.package\r\n"),
                "load order: an order inside Overrides is numbered files read by one line before Overrides', in the order wanted");
            var bytes1 = File.ReadAllBytes(cfgPath);
            var added = Encoding.UTF8.GetBytes("PackedFile ApexLoadOrder/Overrides/*.package\r\n");
            var at1 = IndexOf(bytes1, added);
            check(at1 > 0 && bytes1[..at1].Concat(bytes1[(at1 + added.Length)..]).SequenceEqual(original),
                "load order: every other byte of Resource.cfg stays as it was");
            check(ModNotes.Get(Path.Combine("Mods", ModLoadOrder.FolderName, "Overrides", "002 a.package"))?.Tags?.Contains("kept") == true,
                "load order: a moved mod keeps its notes and tags");

            var u2 = ModLoadOrder.Apply(docs, Want("a", "c", "b", "d", "e"))!;
            check(Names() == "a,c,b,d,e" && File.Exists(Path.Combine(dir, "001 a.package")) && File.Exists(Path.Combine(dir, "002 c.package"))
                && !Directory.EnumerateFiles(dir).Any(f => Path.GetFileName(f).StartsWith("001 0")) && File.ReadAllText(cfgPath) == text1,
                "load order: a second order numbers the launcher's folder again, with no number on a number and no new line");

            ModLoadOrder.Back(docs, u2);
            check(Names() == "c,a,b,d,e" && File.ReadAllText(cfgPath) == text1, "load order: Undo of a second order puts the first back");
            ModLoadOrder.Back(docs, u1);
            check(File.ReadAllBytes(cfgPath).SequenceEqual(original) && Snapshot(mods).SequenceEqual(files0) && !Directory.Exists(Path.Combine(mods, ModLoadOrder.FolderName)),
                "load order: Undo puts the files and Resource.cfg back byte for byte");

            // d dropped at the end of Overrides: alphabetical there, so it only changes folder
            var u3 = ModLoadOrder.Apply(docs, Want("a", "b", "c", "d@0", "e"))!;
            check(Names() == "a,b,c,d,e" && File.Exists(Path.Combine(mods, "Overrides", "d.package")) && u3.Before is null,
                "load order: a mod dropped in another section moves to its folder");
            // and out of the launcher's folder: its own name again
            var u4 = ModLoadOrder.Apply(docs, Want("d", "a", "c", "b", "e"))!;
            var u5 = ModLoadOrder.Apply(docs, Want("a", "c", "b", "d@3", "e"))!;
            check(Names() == "a,c,b,d,e" && File.Exists(Path.Combine(mods, "Packages", "e.package")) && List().Any(m => m.Rel.EndsWith(Path.Combine("Packages", "d.package"))),
                "load order: a mod taken out of the launcher's folder gets its own name back");
            ModLoadOrder.Back(docs, u5);
            ModLoadOrder.Back(docs, u4);
            ModLoadOrder.Back(docs, u3);
            check(File.ReadAllBytes(cfgPath).SequenceEqual(original) && Snapshot(mods).SequenceEqual(files0), "load order: moves across sections undo in turn");

            var u6 = ModLoadOrder.SetPriority(docs, 2, 700);
            check(File.ReadAllBytes(cfgPath).SequenceEqual(Swap(original, "Priority 500", "Priority 700")) && ModConflicts.ParseResourceCfg(File.ReadAllText(cfgPath))[3].Priority == 700,
                "load order: a new priority changes only its number");
            ModLoadOrder.Back(docs, u6);

            var u7 = ModLoadOrder.NewFolder(docs, "Mine", 800);
            var s7 = ModLoadOrder.Sections(File.ReadAllText(cfgPath));
            check(Directory.Exists(Path.Combine(mods, "Mine")) && s7.FindIndex(s => s.Text == "Mine/*.package") == 2 && s7[2].Priority == 800 && s7[3].Text == "Mine/*/*.package" && s7[4].Priority == 500,
                "load order: a new priority folder is made and read at its priority, before the lower ones");
            ModLoadOrder.Back(docs, u7);
            check(File.ReadAllBytes(cfgPath).SequenceEqual(original) && !Directory.Exists(Path.Combine(mods, "Mine")), "load order: Undo of a new folder takes it away");

            var x = List().Single(m => m.Name == "x.package");
            var y = List().Single(m => m.Name == "y.package");
            var u8 = ModLoadOrder.MakeRead(docs, y);
            var d8 = ModConflicts.ParseResourceCfg(File.ReadAllText(cfgPath));
            var u9 = ModLoadOrder.MakeRead(docs, x);
            var d9 = ModConflicts.ParseResourceCfg(File.ReadAllText(cfgPath));
            check(ModConflicts.LoadKey(d8, "Packages/A/B/y.package") is { } ky && ky.Item1 == -500 && d8[ky.Item2 - 1].Text == "Packages/*/*.package"
                && ModConflicts.LoadKey(d9, "Stray/x.package") is { Item1: -500 } && File.ReadAllText(cfgPath).EndsWith("Priority 500\r\nPackedFile Stray/*.package\r\n"),
                "load order: a file no line reads is read at its nearest read folder's priority, else at 500");
            ModLoadOrder.Back(docs, u9);
            ModLoadOrder.Back(docs, u8);
            check(File.ReadAllBytes(cfgPath).SequenceEqual(original), "load order: Undo of make it read");

            // b held open by another program: what moved goes back, Resource.cfg is not written
            var held = false;
            using (File.Open(Path.Combine(mods, "Overrides", "b.package"), FileMode.Open, FileAccess.Read, FileShare.None))
                try { ModLoadOrder.Apply(docs, Want("c", "a", "b", "d", "e")); }
                catch (Exception e) when (e is IOException or UnauthorizedAccessException) { held = true; }
            check(held && File.ReadAllBytes(cfgPath).SequenceEqual(original) && Snapshot(mods).SequenceEqual(files0),
                "load order: a failure midway puts back what was done");
        }
        finally
        {
            Paths.LocalAppData = oldLocal;
            ModNotes.Reload();
            try { Directory.Delete(root, true); } catch (Exception) { }
        }
    }

    static List<string> Snapshot(string dir) => Directory.EnumerateFiles(dir, "*.package", SearchOption.AllDirectories)
        .Select(f => Path.GetRelativePath(dir, f) + "=" + File.ReadAllText(f)).Order(StringComparer.Ordinal).ToList();

    static int IndexOf(byte[] a, byte[] b)
    {
        for (var i = 0; i + b.Length <= a.Length; i++) if (a.AsSpan(i, b.Length).SequenceEqual(b)) return i;
        return -1;
    }

    static byte[] Swap(byte[] a, string from, string to) => Encoding.Latin1.GetBytes(Encoding.Latin1.GetString(a).Replace(from, to));

    static void Write(string path, string text) { Directory.CreateDirectory(Path.GetDirectoryName(path)!); File.WriteAllText(path, text); }
}
