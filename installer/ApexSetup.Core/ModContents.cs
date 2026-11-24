using System.Buffers.Binary;
using System.Text;

namespace ApexSetup;

// What a package holds, for the Mods screen's details: the catalog names of its objects and how many Create a Sim parts
// of each kind. Layouts follow the community's documentation of the Sims 3 formats (the Sims 3 Wiki, as the S3PI
// library reads them):
// - STBL (0x220557DA): "STBL", version byte, uint16, uint32 count, uint16, uint32, then per string a uint64 key and a
//   UTF-16 text after its int32 length in characters. The top byte of the instance is the language (0 = English).
// - OBJD (0x319E4F1D): its catalog block holds the name's and the description's STBL keys side by side (two uint64),
//   after a material list of varying length; the name is found as the first such pair the package's STBL knows.
// - CASP (0x034AEECB): uint32 version, int32 offset, a list of presets (int32 count; each an int32-length UTF-16 text and
//   a uint32), the part's own name (7-bit length, UTF-16 big endian), float, byte, then the uint32 clothing type.
// Names: the objects' catalog names; PartNames: the CAS parts' own (internal) names; Parts: how many of each kind
public sealed record ModContents(IReadOnlyList<string> Names, IReadOnlyList<string> PartNames, IReadOnlyList<(string Kind, int Count)> Parts);

public static class ModContent
{
    const uint Stbl = 0x220557DA, Objd = 0x319E4F1D, Casp = 0x034AEECB;
    const int MaxObjects = 200, MaxParts = 400, MaxNames = 60;

    public static ModContents Read(string path)
    {
        var index = Dbpf.Index(path);
        var names = new List<string>();
        var partNames = new List<string>();
        var parts = new Dictionary<string, int>();
        if (index.Count(e => e.Type == Objd) is > 0 and var objects) parts["Object"] = objects;
        // the package's own strings, English first
        var text = new Dictionary<ulong, string>();
        foreach (var e in index.Where(e => e.Type == Stbl).OrderBy(e => e.Instance >> 56 == 0 ? 0 : 1).Take(40))
            if (Dbpf.Read(path, e, 4 << 20) is { } d)
                foreach (var (k, v) in Strings(d)) text.TryAdd(k, v);
        foreach (var e in index.Where(e => e.Type == Objd).Take(MaxObjects))
        {
            if (text.Count > 0 && names.Count < MaxNames && Dbpf.Read(path, e, 1 << 20) is { } d && NameIn(d, text) is { } n && !names.Contains(n)) names.Add(n);
        }
        foreach (var e in index.Where(e => e.Type == Casp).Take(MaxParts))
            if (Dbpf.Read(path, e, 1 << 20) is { } d && Part(d) is { } p)
            {
                parts[KindOf(p.Clothing)] = parts.GetValueOrDefault(KindOf(p.Clothing)) + 1;
                // (a part named only by numbers says nothing to a player)
                if (partNames.Count < MaxNames && p.Name.Any(char.IsLetter) && !partNames.Contains(p.Name)) partNames.Add(p.Name);
            }
        return new ModContents(names, partNames, parts.OrderByDescending(x => x.Value).Select(x => (x.Key, x.Value)).ToList());
    }

    public static IEnumerable<(ulong Key, string Text)> Strings(byte[] d)
    {
        if (d.Length < 21 || BinaryPrimitives.ReadUInt32LittleEndian(d) != 0x4C425453) yield break; // "STBL"
        var count = BinaryPrimitives.ReadUInt32LittleEndian(d.AsSpan(7));
        var at = 4 + 1 + 2 + 4 + 2 + 4;
        for (uint i = 0; i < count && at + 12 <= d.Length; i++)
        {
            var key = BinaryPrimitives.ReadUInt64LittleEndian(d.AsSpan(at));
            var len = BinaryPrimitives.ReadInt32LittleEndian(d.AsSpan(at + 8));
            at += 12;
            if (len < 0 || at + len * 2 > d.Length) yield break;
            yield return (key, Encoding.Unicode.GetString(d, at, len * 2));
            at += len * 2;
        }
    }

    // the first name/description key pair the strings know (the catalog block's two STBL keys, side by side)
    static string? NameIn(byte[] d, Dictionary<ulong, string> text)
    {
        for (int at = 4; at + 16 <= d.Length; at++)
        {
            var name = BinaryPrimitives.ReadUInt64LittleEndian(d.AsSpan(at));
            if (name == 0 || !text.TryGetValue(name, out var s)) continue;
            var desc = BinaryPrimitives.ReadUInt64LittleEndian(d.AsSpan(at + 8));
            if (desc == 0 || text.ContainsKey(desc)) return s.Trim() is { Length: > 0 } t ? t : null;
        }
        return null;
    }

    // AgeGender: the dword after the data type flags (s3pi CASPartResource: age bits 0x7F, species (dword >> 8) & 0xCF)
    public static (string Name, uint Clothing, uint AgeGender)? Part(byte[] d)
    {
        try
        {
            var at = 8;
            var presets = BinaryPrimitives.ReadInt32LittleEndian(d.AsSpan(at));
            at += 4;
            if (presets is < 0 or > 1000) return null;
            for (int i = 0; i < presets; i++)
            {
                var len = BinaryPrimitives.ReadInt32LittleEndian(d.AsSpan(at));
                if (len < 0 || len > d.Length) return null;
                at += 4 + len * 2 + 4;
            }
            // the part's name: a 7-bit encoded byte length, then UTF-16 big endian
            int n = 0, shift = 0;
            byte b;
            do { b = d[at++]; n |= (b & 0x7F) << shift; shift += 7; } while ((b & 0x80) != 0 && shift < 35);
            if (n < 0 || at + n > d.Length) return null;
            var name = Encoding.BigEndianUnicode.GetString(d, at, n);
            at += n + 4 + 1;
            var clothing = BinaryPrimitives.ReadUInt32LittleEndian(d.AsSpan(at));
            return clothing > 0x40 ? null : (name, clothing, BinaryPrimitives.ReadUInt32LittleEndian(d.AsSpan(at + 8)));
        }
        catch (Exception e) when (e is ArgumentOutOfRangeException or IndexOutOfRangeException) { return null; }
    }

    // a CAS part's clothing type, as a kind a player knows (English keys, translated where shown)
    public static string KindOf(uint clothing) => clothing switch
    {
        0x01 => "Hair",
        0x04 => "Outfit",
        0x05 => "Top",
        0x06 => "Bottom",
        0x07 => "Shoes",
        >= 0x08 and <= 0x0F or 0x18 or 0x19 or >= 0x1E and <= 0x20 => "Accessory",
        0x10 => "Facial hair",
        >= 0x11 and <= 0x15 or 0x1A => "Makeup",
        0x16 => "Eyebrows",
        0x17 => "Eye colour",
        0x1B or 0x1C or 0x1D => "Skin detail",
        0x21 or 0x22 => "Tattoo",
        _ => "Other part",
    };
}
