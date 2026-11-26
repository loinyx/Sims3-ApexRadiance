using System.Buffers.Binary;
using System.Globalization;
using System.Text;
using System.Text.RegularExpressions;
using System.Xml;

namespace ApexSetup;

// One thing a list or a game file names: a resource key (Type 0: only its instance is known), a package's file name, or
// only a name.
public sealed record FindItem(uint Type, uint Group, ulong Instance, string? Name, string? File)
{
    public string? Key => Instance == 0 ? null : Type == 0 ? $"0x{Instance:X16}" : $"0x{Type:X8}-0x{Group:X8}-0x{Instance:X16}";
    public string Text => Key ?? File ?? Name ?? "";
}

// Truncated: the file names more than was read
public sealed record FindSource(string Name, IReadOnlyList<FindItem> Items, bool Truncated);
// a mod that holds an item, by its place (the same on or off); Inside: the mod in that merge
public sealed record FindHit(string Rel, string? Inside);
// Game: the game ships it; Installed: the game's own launcher installed it (Store content, a .sims3pack)
public sealed record FindMatch(FindItem Item, IReadOnlyList<FindHit> Hits, bool Game, bool Installed = false);
public sealed record FindResult(FindSource Source, IReadOnlyList<FindMatch> Matches, bool GameKnown);
public enum FindState { Found, Game, Installed, Off, Missing }

// Find custom content: what a list (XML or any text with resource keys, package names or item names) or a game file (a
// package, a Sim, a lot, a household, a world, a save) names, and which of the player's mods hold it.
//
// The references of game files, measured on every lot, household and world the game ships (1.67), with every reference
// found in the game's own index: 0x05ED1226 (the key table a lot, household or world keeps of all it uses) and 0x34E5247C
// (a lot's objects) hold keys as type, group, instance; so do catalog resources and object keys (0x02DC343F). An outfit
// (0x025ED6F4) and a Create a Sim part hold them as instance, group, type. They are found by their type.
// Everything read here is untrusted: sizes, counts and depths are bounded.
public static class ModFind
{
    public const int MaxText = 16 << 20, MaxItems = 20_000, MaxDepth = 128;
    const int MaxResource = 64 << 20;
    const long MaxDecoded = 1L << 30;

    const uint KeyTable = 0x05ED1226, LotObjects = 0x34E5247C, Outfit = 0x025ED6F4, Objk = 0x02DC343F, Xml = 0x0333406C, Tone = 0x0354796A,
        Slider = 0x0358B08A, Vpxy = 0x736884F1, Footprint = 0xD382BF57;
    static readonly uint[] Catalog = [ModKinds.Objd, ModKinds.Cwst, ModKinds.Cwal, ModKinds.Cfen, ModKinds.Cral, ModKinds.Cstr, ModKinds.Crst, ModKinds.Crmt,
        ModKinds.Cfnd, ModKinds.Cfir, ModKinds.Ccfp, ModKinds.Ctpt, ModKinds.Cttl, ModKinds.Cwat, ModKinds.Mdlr];
    // what a scene uses, as a player knows it; a mod's own resources also need the object keys and meshes they point at
    static readonly HashSet<uint> Items = [.. Catalog, ModKinds.Casp, Xml, Tone, Slider];
    static readonly HashSet<uint> Parts = [.. Items, Objk, Vpxy];

    // ---------- what the file names ----------

    public static FindSource Read(string path, CancellationToken ct = default)
    {
        var found = new Found();
        var name = Path.GetFileName(path.TrimEnd('\\', '/'));
        var parent = Path.GetDirectoryName(Path.GetFullPath(path)) ?? "";
        var inSave = path.EndsWith(".nhd", StringComparison.OrdinalIgnoreCase) && Path.GetFileName(parent).Contains(".sims3", StringComparison.OrdinalIgnoreCase);
        if (Directory.Exists(path) || inSave)
        {
            // (a save: its worlds and households are one file each, read together)
            var dir = inSave ? parent : path;
            var files = Directory.EnumerateFiles(dir, "*.nhd").Where(f => (File.GetAttributes(f) & FileAttributes.ReparsePoint) == 0).Order(StringComparer.OrdinalIgnoreCase).ToList();
            if (files.Count == 0) throw new SetupException("This folder is not a save. Choose one that ends in .sims3.");
            FromPackages(files, found, ct);
            return found.Source(Path.GetFileName(dir.TrimEnd('\\', '/')));
        }
        var head = new byte[4];
        long length;
        using (var f = Dbpf.OpenShared(path)) { length = f.Length; f.ReadAtLeast(head, 4, throwOnEndOfStream: false); }
        if (Encoding.Latin1.GetString(head) is var magic && magic == "DBPP") throw new SetupException("This file is protected, so the launcher can't read it.");
        if (magic == "DBPF") FromPackages([path], found, ct);
        else
        {
            if (length > MaxText) throw new SetupException("This file is too big to be a list.");
            FromText(Decode(path), found, ct);
        }
        return found.Source(name);
    }

    sealed class Found
    {
        readonly Dictionary<(uint, ulong, string), FindItem> _all = [];
        readonly HashSet<ulong> _typed = [];
        public bool Truncated;
        public int Count => _all.Count;

        // (a group says which copy of a key the game means, not another item)
        public void Add(FindItem i)
        {
            var k = (i.Type, i.Instance, i.Instance != 0 ? "" : (i.File ?? i.Name ?? "").ToLowerInvariant());
            if (_all.TryGetValue(k, out var had)) { if (had.Name is null && i.Name is not null) _all[k] = had with { Name = i.Name }; return; }
            if (_all.Count >= MaxItems) { Truncated = true; return; }
            _all[k] = i;
            if (i.Type != 0) _typed.Add(i.Instance);
        }

        public FindSource Source(string name) => new(name, _all.Values.Where(i => i.Type != 0 || !_typed.Contains(i.Instance)).ToList(), Truncated);
    }

    // ---------- text ----------

    static string Decode(string path)
    {
        using var f = Dbpf.OpenShared(path);
        var d = new byte[(int)Math.Min(f.Length, MaxText)];
        d = d[..f.ReadAtLeast(d, d.Length, throwOnEndOfStream: false)];
        if (d is [0xEF, 0xBB, 0xBF, ..]) return Encoding.UTF8.GetString(d, 3, d.Length - 3);
        if (d is [0xFF, 0xFE, ..]) return Encoding.Unicode.GetString(d, 2, d.Length - 2);
        if (d is [0xFE, 0xFF, ..]) return Encoding.BigEndianUnicode.GetString(d, 2, d.Length - 2);
        if (Array.IndexOf(d, (byte)0, 0, Math.Min(d.Length, 4096)) < 0) return Encoding.UTF8.GetString(d);
        // (UTF-16 without its mark: every second byte of plain text is zero)
        if (d.Length >= 4 && d[0] != 0 && d[1] == 0 && d[2] != 0 && d[3] == 0) return Encoding.Unicode.GetString(d);
        throw new SetupException("The launcher can't read this kind of file. Choose a list, a package, a Sim, a lot or a save.");
    }

    const string Sep = @"(?:[ \t]{0,3}[-:!_,;|/][ \t]{0,3}|[ \t]{1,3})";
    static readonly Regex FullKey = new(
        @"(?<![0-9a-z])(?:0x(?<t>[0-9a-f]{1,8})|(?<t>[0-9a-f]{8}))" + Sep + @"(?:0x(?<g>[0-9a-f]{1,8})|(?<g>[0-9a-f]{8}))" + Sep + @"(?:0x(?<i>[0-9a-f]{1,16})|(?<i>[0-9a-f]{16}))(?![0-9a-z])",
        RegexOptions.IgnoreCase | RegexOptions.CultureInvariant, TimeSpan.FromSeconds(10));
    static readonly Regex BareInstance = new(@"(?<![0-9a-z_])(?:0x(?<i>[0-9a-f]{9,16})|(?<i>[0-9a-f]{16}))(?![0-9a-z_])",
        RegexOptions.IgnoreCase | RegexOptions.CultureInvariant, TimeSpan.FromSeconds(10));
    static readonly HashSet<string> NameWords = new(StringComparer.OrdinalIgnoreCase) { "name", "title", "n", "label", "displayname", "objectname", "itemname", "catalogname" };
    static readonly HashSet<string> TypeWords = new(StringComparer.OrdinalIgnoreCase) { "t", "type", "typeid", "resourcetype" };
    static readonly HashSet<string> GroupWords = new(StringComparer.OrdinalIgnoreCase) { "g", "group", "groupid", "resourcegroup" };
    static readonly HashSet<string> InstanceWords = new(StringComparer.OrdinalIgnoreCase) { "i", "instance", "instanceid", "iid", "resourceinstance" };

    static ulong Hex(string s) => ulong.Parse(s, NumberStyles.HexNumber, CultureInfo.InvariantCulture);
    static bool HexOf(string s, int digits, out ulong v)
    {
        s = s.Trim();
        if (s.StartsWith("0x", StringComparison.OrdinalIgnoreCase)) s = s[2..];
        v = 0;
        return s.Length > 0 && s.Length <= digits && ulong.TryParse(s, NumberStyles.AllowHexSpecifier, CultureInfo.InvariantCulture, out v);
    }

    // the keys and package names in a text; rest: what is left of it without them
    static List<FindItem> Scan(string text, out string rest)
    {
        var list = new List<FindItem>();
        rest = FullKey.Replace(text, m =>
        {
            if (Hex(m.Groups["i"].Value) is var i and not 0) list.Add(new((uint)Hex(m.Groups["t"].Value), (uint)Hex(m.Groups["g"].Value), i, null, null));
            return " ";
        });
        rest = BareInstance.Replace(rest, m =>
        {
            // (sixteen digits without a letter are a number, not a key)
            if (!m.Value.StartsWith("0x", StringComparison.OrdinalIgnoreCase) && m.Value.All(char.IsAsciiDigit)) return m.Value;
            if (Hex(m.Groups["i"].Value) is var i and > uint.MaxValue) list.Add(new(0, 0, i, null, null));
            return " ";
        });
        if (list.Count == 0)
            for (var at = text.IndexOf(Package, StringComparison.OrdinalIgnoreCase); at >= 0; at = text.IndexOf(Package, at + 1, StringComparison.OrdinalIgnoreCase))
            {
                var end = at + Package.Length;
                if (end < text.Length && char.IsLetterOrDigit(text[end])) continue;
                var from = at;
                while (from > 0 && at - from < 120 && "\\/:*?\"<>|\r\n\t,;=".IndexOf(text[from - 1]) < 0) from--;
                if (text[from..end].Trim() is { Length: > 8 } file) list.Add(new(0, 0, 0, null, file));
            }
        return list;
    }
    const string Package = ".package";

    // a name a person would read: some letters, one line, nothing long
    static string? NameOf(string s)
    {
        s = s.Trim(' ', '\t', '\r', '\n', '-', '*', '•', ',', ';', '|', '=', '"', '\'', '(', ')', '[', ']', '>');
        return s.Length is >= 3 and <= 100 && s.Count(char.IsLetter) >= 2 && !s.Contains('\n') ? s : null;
    }

    static void FromText(string text, Found found, CancellationToken ct)
    {
        var xml = text.AsSpan().TrimStart() is ['<', ..];
        var names = new List<string>();
        try
        {
            if (xml && FromXml(text, names, ct) is { } read) foreach (var i in read) found.Add(i);
            else
                foreach (var line in text.Split('\n'))
                {
                    ct.ThrowIfCancellationRequested();
                    var keys = Scan(line, out var rest);
                    var name = NameOf(rest);
                    foreach (var k in keys) found.Add(keys.Count == 1 && k.File is null ? k with { Name = name } : k);
                    // (a comment or a heading is not an item)
                    if (keys.Count == 0 && !xml && name is not null && !line.TrimStart().StartsWith('#') && !line.TrimEnd().EndsWith(':')) names.Add(name);
                }
        }
        catch (RegexMatchTimeoutException) { found.Truncated = true; }
        // (names alone only when the list has nothing better: beside keys they are headings and descriptions)
        if (found.Count == 0) foreach (var n in names) found.Add(new(0, 0, 0, n, null));
    }

    sealed record Frame(string Name, List<(string Name, string Value)> Values, StringBuilder Text) { public bool HasChild; }

    // An element is a record: the keys in its attributes, its text and its plain children, with the name among them. Null:
    // not XML a list would be (broken, deeper than MaxDepth, with a DTD), so it is read as plain text. No DTD, no entities
    // of its own, nothing outside the file.
    static List<FindItem>? FromXml(string text, List<string> names, CancellationToken ct)
    {
        var list = new List<FindItem>();
        var stack = new Stack<Frame>();
        bool Emit(List<(string Name, string Value)> values, bool named)
        {
            var keys = values.SelectMany(v => Scan(v.Value, out _)).ToList();
            if (keys.Count == 0) return false;
            var name = keys.Count > 1 ? null : values.Where(v => NameWords.Contains(v.Name)).Select(v => NameOf(v.Value)).FirstOrDefault(n => n is not null);
            if (named && name is null) return false;
            list.AddRange(keys.Select(k => k.Key is null ? k : k with { Name = name }));
            return true;
        }
        void End()
        {
            var f = stack.Pop();
            if (f.Text.ToString().Trim() is { Length: > 0 } s)
            {
                f.Values.Add((f.Name, s));
                if (NameWords.Contains(f.Name) && NameOf(s) is { } n) names.Add(n);
            }
            // (type="..." group="..." instance="...": one key)
            ulong Of(HashSet<string> words, int digits) => f.Values.Where(v => words.Contains(v.Name)).Select(v => HexOf(v.Value, digits, out var x) ? x : 0).FirstOrDefault(x => x != 0);
            if (Of(TypeWords, 8) is var t and not 0 && Of(InstanceWords, 16) is var i and not 0)
            {
                var g = Of(GroupWords, 8);
                f.Values.RemoveAll(v => TypeWords.Contains(v.Name) || GroupWords.Contains(v.Name) || InstanceWords.Contains(v.Name));
                f.Values.Add(("key", $"0x{t:X8}-0x{g:X8}-0x{i:X16}"));
            }
            // (a plain child without a name of its own is a field of its parent: <item><name>...</name><key>...</key></item>)
            if (!f.HasChild && stack.Count > 1) { if (!Emit(f.Values, named: true)) stack.Peek().Values.AddRange(f.Values); }
            else Emit(f.Values, named: false);
        }
        try
        {
            using var r = XmlReader.Create(new StringReader(text), new XmlReaderSettings
            {
                DtdProcessing = DtdProcessing.Prohibit, XmlResolver = null, MaxCharactersInDocument = MaxText, MaxCharactersFromEntities = 1024,
                IgnoreComments = true, IgnoreProcessingInstructions = true,
            });
            while (r.Read())
            {
                if (r.NodeType == XmlNodeType.Element)
                {
                    ct.ThrowIfCancellationRequested();
                    if (stack.Count >= MaxDepth) return null;
                    if (stack.Count > 0) stack.Peek().HasChild = true;
                    var f = new Frame(r.LocalName, [], new());
                    var empty = r.IsEmptyElement;
                    while (r.MoveToNextAttribute())
                    {
                        f.Values.Add((r.LocalName, r.Value));
                        if (NameWords.Contains(r.LocalName) && NameOf(r.Value) is { } n) names.Add(n);
                    }
                    stack.Push(f);
                    if (empty) End();
                }
                else if (r.NodeType is XmlNodeType.Text or XmlNodeType.CDATA && stack.Count > 0) stack.Peek().Text.Append(r.Value).Append(' ');
                else if (r.NodeType == XmlNodeType.EndElement) End();
            }
            return list;
        }
        catch (XmlException) { names.Clear(); return null; }
    }

    // ---------- game files ----------

    static void FromPackages(IReadOnlyList<string> paths, Found found, CancellationToken ct)
    {
        var indexes = paths.Select(p => (Path: p, Entries: Dbpf.Index(p))).ToList();
        if (indexes.All(x => x.Entries.Count == 0)) throw new SetupException("This file is empty or damaged.");
        var own = indexes.SelectMany(x => x.Entries).Select(e => (e.Type, e.Instance)).ToHashSet();
        long decoded = 0;
        void Add(uint type, uint group, ulong instance)
        {
            if (instance != 0 && !own.Contains((type, instance))) found.Add(new(type, group, instance, null, null));
        }
        foreach (var (path, entries) in indexes)
        {
            using var f = Dbpf.OpenShared(path);
            foreach (var e in entries)
            {
                var scene = e.Type is KeyTable or LotObjects or Outfit;
                if (!scene && e.Type is not (ModKinds.Casp or Objk or Xml) && Array.IndexOf(Catalog, e.Type) < 0) continue;
                ct.ThrowIfCancellationRequested();
                // (a small file can claim the same bytes as many resources)
                if ((decoded += Math.Max(e.Size, e.MemSize)) > MaxDecoded) { found.Truncated = true; return; }
                if (Dbpf.Read(f, e, MaxResource) is not { } d) continue;
                if (e.Type == Xml)
                {
                    // (a game text names its resources in the same notations as a list)
                    if (d.Length > 4 << 20) continue;
                    try
                    {
                        foreach (var k in Scan(Encoding.UTF8.GetString(d), out _).Where(k => Items.Contains(k.Type))) Add(k.Type, k.Group, k.Instance);
                    }
                    catch (RegexMatchTimeoutException) { found.Truncated = true; }
                    continue;
                }
                var instanceFirst = e.Type is Outfit or ModKinds.Casp;
                var types = scene ? Items : Parts;
                for (var p = instanceFirst ? 12 : 0; p + (instanceFirst ? 4 : 16) <= d.Length; p++)
                {
                    var type = BinaryPrimitives.ReadUInt32LittleEndian(d.AsSpan(p));
                    if (type <= 0xFFFF || !types.Contains(type)) continue;
                    if (instanceFirst) Add(type, BinaryPrimitives.ReadUInt32LittleEndian(d.AsSpan(p - 4)), BinaryPrimitives.ReadUInt64LittleEndian(d.AsSpan(p - 12)));
                    else Add(type, BinaryPrimitives.ReadUInt32LittleEndian(d.AsSpan(p + 4)), BinaryPrimitives.ReadUInt64LittleEndian(d.AsSpan(p + 8)));
                }
            }
        }
    }

    // ---------- the player's mods ----------

    // what the game's own launcher installed: the game reads it from here, not from Mods (a .dbc is a package)
    static IEnumerable<string> InstalledContent(string docs)
    {
        foreach (var (folder, pattern) in new[] { ("DCCache", "*.dbc"), ("DCBackup", "*.package") })
        {
            string[] files = [];
            try { if (Path.Combine(docs, folder) is var dir && Directory.Exists(dir)) files = Directory.GetFiles(dir, pattern); }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { Engine.Log(e); }
            foreach (var f in files) yield return f;
        }
    }

    // a package's keys and, once a list of names asks for them, its items' names; read again when the file changes
    sealed class Pack
    {
        public required long Size;
        public required DateTime Time;
        public required (uint Type, ulong Instance)[] Keys;
        public List<string>? Names;
    }
    static readonly Dictionary<string, Pack> Packs = new(StringComparer.OrdinalIgnoreCase);
    // how many packages were read rather than remembered, for the tests
    public static int PackReads { get; private set; }

    static Pack? PackOf(string path)
    {
        try
        {
            var fi = new FileInfo(path);
            if (!fi.Exists) return null;
            lock (Packs)
                if (Packs.TryGetValue(path, out var had) && had.Size == fi.Length && had.Time == fi.LastWriteTimeUtc) return had;
            var p = new Pack { Size = fi.Length, Time = fi.LastWriteTimeUtc, Keys = Dbpf.Index(path).Where(e => e.Type is not (ModKinds.Nmap or ModKinds.Manifest)).Select(e => (e.Type, e.Instance)).ToArray() };
            lock (Packs)
            {
                PackReads++;
                if (p.Keys.Length > 0) Packs[path] = p; // (an empty one may be a file held a moment: read again next time)
            }
            return p;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return null; }
    }

    // (a damaged package names nothing, and the search goes on)
    static List<string> NamesIn(string path)
    {
        try { return ModInspect.Read(path, "en").Items.Select(i => i.Name).OfType<string>().ToList(); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or ArgumentException or IndexOutOfRangeException or InvalidDataException) { Engine.Log(e); return []; }
    }

    // which of the player's mods hold each item. gi: the game's own index, null when it can't be read (what is missing may
    // then come with the game). progress: 0..1.
    public static FindResult Search(FindSource source, IReadOnlyList<ModFile> mods, string docs, GameIndex? gi, IProgress<double>? progress = null, CancellationToken ct = default)
    {
        var items = source.Items;
        var hits = items.Select(_ => new List<FindHit>()).ToArray();
        var byInstance = new Dictionary<ulong, List<int>>();
        var byFile = new Dictionary<string, List<int>>(StringComparer.OrdinalIgnoreCase);
        var byName = new Dictionary<string, List<int>>(StringComparer.OrdinalIgnoreCase);
        static void Put<TK>(Dictionary<TK, List<int>> d, TK key, int n) where TK : notnull => (d.TryGetValue(key, out var l) ? l : d[key] = []).Add(n);
        for (var n = 0; n < items.Count; n++)
        {
            if (items[n].Instance != 0) Put(byInstance, items[n].Instance, n);
            else if (items[n].File is { } file) Put(byFile, file, n);
            else if (items[n].Name is { } name)
            {
                Put(byName, name, n);
                // (a package's name, as a list without extensions has it; a resource named by the hash of its name, as the
                // game's tuning and CC tools key them, with the high bit tools set)
                Put(byFile, name + Package, n);
                Put(byInstance, Worlds.Fnv64(name), n);
                Put(byInstance, Worlds.Fnv64(name) | 1UL << 63, n);
            }
        }

        void Keys(Pack p, HashSet<int> found)
        {
            foreach (var (type, instance) in p.Keys)
                if (byInstance.TryGetValue(instance, out var l))
                    foreach (var n in l) if (items[n].Type == 0 || items[n].Type == type) found.Add(n);
        }

        HashSet<int> Holds(string path)
        {
            var found = new HashSet<int>();
            if (byFile.TryGetValue(Path.GetFileName(path), out var named)) found.UnionWith(named);
            if (PackOf(path) is not { } p) return found;
            Keys(p, found);
            if (byName.Count > 0)
            {
                foreach (var name in p.Names ??= NamesIn(path)) if (byName.TryGetValue(name, out var l)) found.UnionWith(l);
            }
            return found;
        }

        var packages = mods.Where(m => m.FileType == "package").ToList();
        for (var k = 0; k < packages.Count; k++)
        {
            ct.ThrowIfCancellationRequested();
            progress?.Report((double)k / packages.Count);
            var m = packages[k];
            var left = Holds(m.Path);
            if (ModMerge.Info(docs, m) is { } merge)
                foreach (var o in merge.Originals)
                {
                    ct.ThrowIfCancellationRequested();
                    var original = Path.GetFullPath(Path.Combine(merge.Dir, Path.GetRelativePath("Mods", o.Rel)));
                    // (the record is a file anyone can edit: only what is inside the merge's own folder)
                    if (!original.StartsWith(Path.GetFullPath(merge.Dir) + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase)) continue;
                    foreach (var n in Holds(original))
                        // (a key the merged file does not hold was left out of it; a name or a file name is the original's own)
                        if (left.Remove(n) || items[n].Instance == 0 && hits[n].All(h => h.Rel != m.Rel)) hits[n].Add(new(m.Rel, Path.GetFileNameWithoutExtension(o.Rel)));
                }
            foreach (var n in left) hits[n].Add(new(m.Rel, null));
        }
        var installed = new HashSet<int>();
        foreach (var f in InstalledContent(docs))
        {
            ct.ThrowIfCancellationRequested();
            if (PackOf(f) is { } p) Keys(p, installed);
        }
        progress?.Report(1);
        if (gi is { Count: 0 }) gi = null;
        // (some keys carry a number in the group's low bits: the game's resource is in the group without it)
        // (and the game's own saves name catalog resources of which it ships only the footprint or the picture)
        bool Ships(FindItem i) => gi is not null && i.Type != 0 && (gi.Contains(i.Type, i.Group, i.Instance) || gi.Contains(i.Type, i.Group & 0xFF000000, i.Instance) || gi.Contains(i.Type, 0, i.Instance)
            || gi.Contains(Footprint, 0, i.Instance) || gi.Contains(ModKinds.Img, 0, i.Instance));
        return new(source, items.Select((i, n) => new FindMatch(i, hits[n], Ships(i), installed.Contains(n))).ToList(), gi is not null);
    }

    // where an item stands now. on: whether the mod of that place is on, null when it is gone.
    public static FindState StateOf(FindMatch m, Func<string, bool?> on) =>
        m.Hits.Any(h => on(h.Rel) == true) ? FindState.Found : m.Game ? FindState.Game : m.Installed ? FindState.Installed : m.Hits.Any(h => on(h.Rel) == false) ? FindState.Off : FindState.Missing;

    // an instance this small is the game's own numbering (custom content keys are 64-bit hashes): something of a pack the
    // player does not have, not custom content that is gone
    public static bool MayBeGame(FindResult r, FindItem i) => i.Instance is > 0 and <= uint.MaxValue || !r.GameKnown && i.Instance != 0;
}
