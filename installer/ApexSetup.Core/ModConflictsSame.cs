using System.Buffers.Binary;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;
using System.Xml;

namespace ApexSetup;

// What a copy of a resource means to the game, so only a real difference is a conflict. XML tuning: its elements, attributes
// and values, whatever the comments, the layout, the attribute order and the encoding. A text table (STBL): its texts by
// id, in any order. Everything else, and anything that can't be read that way: its bytes.
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
