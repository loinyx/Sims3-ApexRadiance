using System.Buffers.Binary;
using System.Numerics;
using System.Text;
using System.Xml;

namespace ApexSetup;

// The game's Sims3Pack: a "TS3Pack" header, an XML list of the files it carries (Name, ContentType, Offset, Length) and those
// files one after another. The packages of custom content (objects, clothes, hair, patterns) read the same from Mods, so
// they are taken out as mods; lots, worlds and Sims only go into the game through its own installer. A store item's package
// is "protected": its header is scrambled but its index and resources are plain, at the end of the file as in any package,
// so the header is written again from the index found there. Only reads the given file; what it takes out goes to `dir`.
public static class Sims3Pack
{
    public sealed record Packed(string Name, string Type, long Offset, long Length);

    // what only the game's installer puts in the game
    static readonly string[] GameOnly = ["lot", "world", "household", "sim", "screenshot", "video"];
    public static bool IsMod(Packed p) => p.Name.EndsWith(".package", StringComparison.OrdinalIgnoreCase) && !GameOnly.Contains(p.Type);

    const int MaxXml = 16 << 20;

    // its title and files; null when it is no Sims3Pack
    public static (string Title, List<Packed> Files, long Data)? Read(string path)
    {
        try
        {
            using var f = Dbpf.OpenShared(path);
            var r = new BinaryReader(f);
            var n = r.ReadInt32();
            if (n != 7 || Encoding.ASCII.GetString(r.ReadBytes(7)) != "TS3Pack") return null;
            r.ReadUInt16();
            var size = r.ReadInt32();
            if (size <= 0 || size > MaxXml || 17L + size > f.Length) return null;
            var data = 17L + size;
            var xml = new XmlDocument { XmlResolver = null };
            using (var x = XmlReader.Create(new MemoryStream(r.ReadBytes(size)), new XmlReaderSettings { DtdProcessing = DtdProcessing.Prohibit, XmlResolver = null }))
                xml.Load(x);
            var root = xml.DocumentElement;
            if (root is null || !root.Name.Equals("Sims3Package", StringComparison.OrdinalIgnoreCase)) return null;
            string Text(XmlNode n, string name) => n.ChildNodes.Cast<XmlNode>().FirstOrDefault(c => c.Name.Equals(name, StringComparison.OrdinalIgnoreCase))?.InnerText.Trim() ?? "";
            var files = new List<Packed>();
            foreach (XmlNode p in root.ChildNodes)
            {
                if (!p.Name.Equals("PackagedFile", StringComparison.OrdinalIgnoreCase)) continue;
                // (each compared with what is left, so a huge offset can't wrap the sum past the file's end)
                if (!long.TryParse(Text(p, "Offset"), out var at) || !long.TryParse(Text(p, "Length"), out var len) || at < 0 || len <= 0 ||
                    at > f.Length - data || len > f.Length - data - at || len > Array.MaxLength) return null;
                files.Add(new(Path.GetFileName(Text(p, "Name")), Text(p, "ContentType").ToLowerInvariant(), at, len));
            }
            return (Text(root, "DisplayName"), files, data);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or EndOfStreamException or XmlException or ArgumentException) { return null; }
    }

    // one file taken out to `to`; a protected package comes out readable. False when its index can't be found
    public static bool Take(string path, long data, Packed p, string to)
    {
        var bytes = new byte[p.Length];
        using (var f = Dbpf.OpenShared(path)) { f.Position = data + p.Offset; f.ReadExactly(bytes); }
        if (bytes.AsSpan().StartsWith("DBPP"u8) && !Unprotect(bytes)) return false;
        File.WriteAllBytes(to, bytes);
        return true;
    }

    // The name its maker gave it (the package's manifest: <PackageTitle>), made safe for a file; null when it has none
    public static string? Title(string package)
    {
        if (Dbpf.First(package, [ModKinds.Manifest]) is not { } bytes) return null;
        try
        {
            var xml = new XmlDocument { XmlResolver = null };
            using (var x = XmlReader.Create(new MemoryStream(bytes), new XmlReaderSettings { DtdProcessing = DtdProcessing.Prohibit, XmlResolver = null }))
                xml.Load(x);
            var t = xml.DocumentElement?.ChildNodes.Cast<XmlNode>().FirstOrDefault(c => c.Name.Equals("PackageTitle", StringComparison.OrdinalIgnoreCase))?.InnerText;
            return FileName(t);
        }
        catch (Exception e) when (e is XmlException or ArgumentException) { return null; }
    }

    public static string? FileName(string? t)
    {
        if (string.IsNullOrWhiteSpace(t)) return null;
        t = string.Concat(t.Where(c => !Path.GetInvalidFileNameChars().Contains(c))).Trim().TrimEnd('.');
        if (t.Length > 80) t = t[..80].Trim();
        // (CON, NUL, COM1... are Windows' own names, with or without an extension)
        if (System.Text.RegularExpressions.Regex.IsMatch(t, @"^(CON|PRN|AUX|NUL|COM\d|LPT\d)$", System.Text.RegularExpressions.RegexOptions.IgnoreCase)) t += "_";
        return t.Length == 0 ? null : t;
    }

    // A protected package's header written again. Its index is the file's tail: a flags word, the values its entries share,
    // then one entry per resource, each ending in "compressed, committed" (00 00 01 00 or FF FF 01 00). Each entry is
    // 32 bytes less 4 for each shared value; the entries are counted back from the end while they end that way.
    internal static bool Unprotect(byte[] b)
    {
        const int Head = 96;
        static bool End(byte[] b, long at) => at >= Head && BinaryPrimitives.ReadUInt32LittleEndian(b.AsSpan((int)at)) is 0x00010000 or 0x0001FFFF;
        for (var shared = 0; shared <= 3; shared++)
        {
            var size = 32 - 4 * shared;
            long count = 0;
            while (End(b, b.Length - 4 - count * size)) count++;
            for (; count > 0; count--)
            {
                var at = b.Length - count * size - 4 * shared - 4;
                if (at < Head) continue;
                var flags = BinaryPrimitives.ReadUInt32LittleEndian(b.AsSpan((int)at));
                if (flags > 7 || BitOperations.PopCount(flags) != shared) continue;
                var head = new byte[Head];
                "DBPF"u8.CopyTo(head);
                BinaryPrimitives.WriteUInt32LittleEndian(head.AsSpan(4), 2);
                BinaryPrimitives.WriteUInt32LittleEndian(head.AsSpan(36), (uint)count);
                BinaryPrimitives.WriteUInt32LittleEndian(head.AsSpan(44), (uint)(b.Length - at));
                BinaryPrimitives.WriteUInt32LittleEndian(head.AsSpan(60), 3);
                BinaryPrimitives.WriteUInt32LittleEndian(head.AsSpan(64), (uint)at);
                // (tried in place: the scrambled header is put back when this layout does not fit, no copy of the file)
                var was = b[..Head];
                head.CopyTo(b, 0);
                // (every resource inside the file, after the header and before the index)
                var index = Dbpf.Index(new MemoryStream(b, writable: false));
                if (index.Count == count && index.All(e => e.Offset >= Head && e.Offset + (long)e.Size <= at)) return true;
                was.CopyTo(b, 0);
            }
        }
        return false;
    }
}
