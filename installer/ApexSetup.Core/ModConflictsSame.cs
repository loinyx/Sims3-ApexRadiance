using System.Buffers.Binary;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;
using System.Xml;

namespace ApexSetup;

// What a copy of a resource means to the game, so only a real difference is a conflict. XML tuning: its elements, attributes
// and values, whatever the comments, the layout, the attribute order and the encoding. A text table (STBL): its texts by
// id, in any order. A settings file (INI): its sections, names and values, whatever the comments and the layout. Everything
// else, and anything that can't be read that way: its bytes.
public static partial class ModConflicts
{
    // a larger resource is compared by its bytes only (tuning is far smaller; the game's own text tables are about 5 MB)
    public const int MeaningCap = 16 << 20;

    // The texts of a text table by id (as UTF-16 code units, unchanged). Null when it is not one this reads whole: its
    // bytes are compared then.
    public static Dictionary<ulong, string>? Texts(byte[] b)
    {
        if (b.Length < 17 || !b.AsSpan(0, 4).SequenceEqual("STBL"u8)) return null;
        var count = BinaryPrimitives.ReadUInt32LittleEndian(b.AsSpan(7));
        var d = new Dictionary<ulong, string>();
        long p = 17;
        for (uint i = 0; i < count; i++)
        {
            if (p + 12 > b.Length) return null;
            var id = BinaryPrimitives.ReadUInt64LittleEndian(b.AsSpan((int)p));
            var len = BinaryPrimitives.ReadUInt32LittleEndian(b.AsSpan((int)p + 8)) * 2L;
            if (len > b.Length - p - 12) return null;
            var s = new string(MemoryMarshal.Cast<byte, char>(b.AsSpan((int)p + 12, (int)len)));
            if (!d.TryAdd(id, s) && d[id] != s) return null; // (one id with two texts: which one the game shows is not known)
            p += 12 + len;
        }
        return p == b.Length ? d : null;
    }

    // how many texts two text tables don't share (another text, or in one only); null when either can't be read
    public static int? TextsThatDiffer(byte[] a, byte[] b)
    {
        if (Texts(a) is not { } x || Texts(b) is not { } y) return null;
        return x.Count(t => !y.TryGetValue(t.Key, out var s) || s != t.Value) + y.Keys.Count(k => !x.ContainsKey(k));
    }

    static readonly UTF8Encoding Utf8 = new(false, true);
    static readonly UnicodeEncoding Utf16 = new(false, false, true), Utf16Big = new(true, false, true);

    // the text of an XML resource by its byte order mark (UTF-8 without one), or null when the bytes are not valid in it
    static string? XmlText(byte[] b)
    {
        try
        {
            if (b.Length >= 3 && b[0] == 0xEF && b[1] == 0xBB && b[2] == 0xBF) return Utf8.GetString(b, 3, b.Length - 3);
            if (b.Length >= 2 && b[0] == 0xFF && b[1] == 0xFE) return Utf16.GetString(b, 2, b.Length - 2);
            if (b.Length >= 2 && b[0] == 0xFE && b[1] == 0xFF) return Utf16Big.GetString(b, 2, b.Length - 2);
            if (b.Length >= 2 && b[0] == (byte)'<' && b[1] == 0) return Utf16.GetString(b);
            return Utf8.GetString(b);
        }
        catch (ArgumentException) { return null; } // (DecoderFallbackException)
    }

    // An XML document as its elements in order, each with its attributes sorted and its text trimmed; no comments,
    // declaration or layout. Null when it is not well-formed XML (or asks for a DTD): its bytes are compared then.
    static string? XmlMeaning(byte[] b)
    {
        // (most resources are not text: told by the first bytes, before any is decoded)
        var at = b.Length >= 3 && b[0] == 0xEF ? 3 : b.Length >= 2 && b[0] >= 0xFE ? 2 : 0;
        while (at < b.Length && b[at] is 0x20 or 0x09 or 0x0D or 0x0A or 0) at++;
        if (at >= b.Length || b[at] != (byte)'<' || XmlText(b) is not { } text) return null;
        try
        {
            using var r = XmlReader.Create(new StringReader(text), new XmlReaderSettings { DtdProcessing = DtdProcessing.Prohibit, XmlResolver = null });
            // (the control characters between the parts can't be in an XML document, so two documents never read alike by chance)
            var o = new StringBuilder(text.Length);
            var words = new StringBuilder();
            void Flush()
            {
                if (words.Length == 0) return;
                var t = words.ToString().Trim();
                if (t.Length > 0) o.Append('\u0002').Append(t);
                words.Clear();
            }
            while (r.Read())
                switch (r.NodeType)
                {
                    case XmlNodeType.Element:
                        Flush();
                        o.Append('\u0001').Append(r.Name);
                        var empty = r.IsEmptyElement;
                        var attrs = new List<string>();
                        while (r.MoveToNextAttribute()) attrs.Add(r.Name + '\u0003' + r.Value);
                        attrs.Sort(StringComparer.Ordinal);
                        foreach (var a in attrs) o.Append('\u0004').Append(a);
                        if (empty) o.Append('\u0005');
                        break;
                    case XmlNodeType.EndElement: Flush(); o.Append('\u0005'); break;
                    case XmlNodeType.Text or XmlNodeType.CDATA or XmlNodeType.Whitespace or XmlNodeType.SignificantWhitespace: words.Append(r.Value); break;
                }
            return o.ToString();
        }
        catch (XmlException) { return null; }
    }

    // A settings file of the game as "section > name" and value, in order. A value ends at its ';' (a comment, or the end of
    // the line as the game's own files write it); 1, 1.0 and 1.0f are one number. Null when a line is none of these.
    static List<(string Name, string Value)>? IniValues(byte[] b)
    {
        if (XmlText(b) is not { } text || text.Contains('\0')) return null;
        var o = new List<(string, string)>();
        var section = "";
        foreach (var raw in text.Split('\n'))
        {
            var l = Uncommented(raw).Trim();
            if (l.Length == 0 || l.StartsWith("//", StringComparison.Ordinal)) continue;
            if (l[0] == '[' && l[^1] == ']') { section = l[1..^1].Trim(); continue; }
            if (l.IndexOf('=') is not (> 0 and var at)) return null;
            var v = l[(at + 1)..].Trim();
            o.Add(((section.Length > 0 ? section + " > " : "") + l[..at].Trim(), Number(v) ?? v));
        }
        return o;
    }

    // a line without its comment (a ; inside quotes is part of the value)
    static string Uncommented(string line)
    {
        var quoted = false;
        for (var i = 0; i < line.Length; i++)
        {
            if (line[i] == '"') quoted = !quoted;
            else if (line[i] == ';' && !quoted) return line[..i];
        }
        return line;
    }

    // (only what a double holds to the last digit: two longer numbers that differ must not read alike)
    static string? Number(string v) =>
        v.Count(char.IsAsciiDigit) <= 15 && double.TryParse(v.EndsWith('f') || v.EndsWith('F') ? v[..^1] : v, System.Globalization.NumberStyles.Float, System.Globalization.CultureInfo.InvariantCulture, out var n) && double.IsFinite(n)
            ? n.ToString("R", System.Globalization.CultureInfo.InvariantCulture) : null;

    // The values of an XML tuning by where they are ("kCarTuning > Speed"), null when it is not one.
    static List<(string Name, string Value)>? XmlValues(byte[] b)
    {
        if (XmlMeaning(b) is null || XmlText(b) is not { } text) return null;
        var o = new List<(string, string)>();
        using var r = XmlReader.Create(new StringReader(text), new XmlReaderSettings { DtdProcessing = DtdProcessing.Prohibit, XmlResolver = null });
        var path = new List<string>();
        while (r.Read())
            if (r.NodeType == XmlNodeType.Element)
            {
                var name = r.GetAttribute("name") ?? r.GetAttribute("n") ?? r.Name;
                var empty = r.IsEmptyElement;
                path.Add(name);
                if (r.GetAttribute("value") is { } v) o.Add((string.Join(" > ", path.TakeLast(2)), Number(v) ?? v));
                if (empty) path.RemoveAt(path.Count - 1);
            }
            else if (r.NodeType == XmlNodeType.EndElement) path.RemoveAt(path.Count - 1);
        return o;
    }

    // what two copies of a tuning or a settings file set to something else, in the player's sight: the name, its value in
    // the first and in the second; empty when they can't be told that way
    public static List<(string Name, string First, string Second)> ValuesThatDiffer(uint type, byte[] a, byte[] b)
    {
        if (a.Length > MeaningCap || b.Length > MeaningCap || type == TypeStbl) return [];
        var (x, y) = type == TypeIni ? (IniValues(a), IniValues(b)) : (XmlValues(a), XmlValues(b));
        if (x is null || y is null) return [];
        // (a name set more than once: which counts is not known, so it is left out)
        static Dictionary<string, string> Once(List<(string Name, string Value)> l) => l.GroupBy(v => v.Name).Where(g => g.Count() == 1).ToDictionary(g => g.Key, g => g.First().Value);
        var (p, q) = (Once(x), Once(y));
        return p.Where(v => q.TryGetValue(v.Key, out var w) && w != v.Value).Select(v => (v.Key, v.Value, q[v.Key])).ToList();
    }

    // One window layout, resized: the same objects and properties in the same order, and only numbers (places and sizes) that
    // differ. Tiny UI Fix makes such a copy of every window, another mod's among them.
    public static bool Resized(byte[] a, byte[] b)
    {
        static List<string>? Parts(byte[] b)
        {
            if (b.Length > MeaningCap || XmlText(b) is not { } text) return null;
            try
            {
                using var r = XmlReader.Create(new StringReader(text), new XmlReaderSettings { DtdProcessing = DtdProcessing.Prohibit, XmlResolver = null });
                var o = new List<string>();
                while (r.Read())
                    switch (r.NodeType)
                    {
                        case XmlNodeType.Element:
                            o.Add("<" + r.Name);
                            var empty = r.IsEmptyElement;
                            while (r.MoveToNextAttribute())
                            {
                                var v = r.Name == "value" ? string.Join(',', r.Value.Split(',').Select(n => n.Contains("0x", StringComparison.OrdinalIgnoreCase) ? n.Trim() : Number(n.Trim()) is null ? n.Trim() : "#")) : r.Value;
                                o.Add(r.Name + "=" + v);
                            }
                            if (empty) o.Add(">");
                            break;
                        case XmlNodeType.EndElement: o.Add(">"); break;
                        case XmlNodeType.Text or XmlNodeType.CDATA: if (r.Value.Trim() is { Length: > 0 } t) o.Add(Number(t) is null ? t : "#"); break;
                    }
                return o;
            }
            catch (XmlException) { return null; }
        }
        return Parts(a) is { Count: > 2 } x && Parts(b) is { } y && x.SequenceEqual(y);
    }

    static UInt128 Digest(byte tag, ReadOnlySpan<byte> data)
    {
        using var h = IncrementalHash.CreateHash(HashAlgorithmName.SHA256);
        h.AppendData([tag]);
        h.AppendData(data);
        Span<byte> o = stackalloc byte[32];
        h.GetHashAndReset(o);
        return BinaryPrimitives.ReadUInt128LittleEndian(o);
    }

    // A number equal for two copies of a resource between which the game sees no difference.
    public static UInt128 Meaning(uint type, byte[] b) => Canon(type, b) ?? Digest(0, b);

    // (null: the resource is not one read here, so only its bytes say)
    static UInt128? Canon(uint type, byte[] b)
    {
        if (b.Length <= MeaningCap)
        {
            if (type == TypeStbl && Texts(b) is { } texts)
            {
                // (the header's version and unknown fields stay part of it)
                using var w = new MemoryStream();
                w.Write(b.AsSpan(4, 3));
                w.Write(b.AsSpan(11, 6));
                Span<byte> head = stackalloc byte[12];
                foreach (var (id, s) in texts.OrderBy(t => t.Key))
                {
                    BinaryPrimitives.WriteUInt64LittleEndian(head, id);
                    BinaryPrimitives.WriteInt32LittleEndian(head[8..], s.Length);
                    w.Write(head);
                    w.Write(MemoryMarshal.AsBytes(s.AsSpan()));
                }
                return Digest(1, w.GetBuffer().AsSpan(0, (int)w.Length));
            }
            if (type == TypeIni && IniValues(b) is { } ini) return Digest(3, MemoryMarshal.AsBytes(string.Join('\u0001', ini.Select(v => v.Name + '\u0002' + v.Value)).AsSpan()));
            if (type != TypeStbl && XmlMeaning(b) is { } xml) return Digest(2, MemoryMarshal.AsBytes(xml.AsSpan()));
        }
        return null;
    }

    // two copies of a resource with no difference the game sees
    public static bool SameMeaning(uint type, byte[] a, byte[] b) => a.AsSpan().SequenceEqual(b) || Canon(type, a) is { } x && x == Canon(type, b);

    // ---------- a piece of another mod ----------
    static readonly Regex PieceWord = new(@"add[-_ ]?on|patch|fix|optional|module|compat|override|tweak|tuning|flavou?r|plugin|submod", RegexOptions.IgnoreCase | RegexOptions.CultureInvariant);

    // The name of `piece` says it is an add-on, a patch or an optional part of `whole`: it has the other's name (or a word
    // of it) and, beyond that, a word such as "patch". Two versions or copies of one mod ("Mod_v2", "Mod (2)") don't.
    public static bool SaysPiece(string piece, string whole)
    {
        static string N(string s) => Regex.Replace(s, "[^a-z0-9]", "");
        var (p, w) = (N(ModConflict.Plain(piece)), N(ModConflict.Plain(whole)));
        if (p == w || w.Length < 4) return false;
        var rest = p.Replace(w, "");
        if (rest.Length == p.Length)
        {
            var words = Regex.Split(ModConflict.Plain(whole), "[^a-z0-9]+").Where(t => t.Length >= 4 && !PieceWord.IsMatch(t) && p.Contains(t)).ToList();
            if (words.Count == 0) return false;
            foreach (var t in words) rest = rest.Replace(t, "");
        }
        return PieceWord.IsMatch(rest);
    }
}
