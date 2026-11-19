using System.Text;
using System.Xml.Linq;

namespace ApexSetup;

// A Sims3Pack: a uint32 length (7), "TS3Pack", a uint16 version, a uint32 manifest length, the XML manifest, then the
// files the manifest lists (PackagedFile: Name, Length, Offset from the end of the manifest, ContentType). Checked on a
// player's 8 Sims3Packs. Objects, Create a Sim parts and patterns are .package files the game loads from Mods like any
// other: they are taken out there. Lots, households and worlds go to Downloads for the game's own launcher (unverified
// that a taken-out lot works from the Library, so it is not done).
public static class Sims3Pack
{
    public sealed record Packed(string Name, long Offset, long Length, string ContentType);
    public sealed record Info(string Type, string DisplayName, string Description, long DataStart, List<Packed> Files)
    {
        // (what the launcher can take out into Mods; the rest is left to the game's launcher)
        public bool Extractable => Type is not ("lot" or "household" or "world" or "") && Files.Any(f => f.Name.EndsWith(".package", StringComparison.OrdinalIgnoreCase));
    }

    public static Info? Read(string path)
    {
        try
        {
            using var f = File.OpenRead(path);
            var r = new BinaryReader(f);
            if (f.Length < 17 || r.ReadUInt32() != 7 || Encoding.ASCII.GetString(r.ReadBytes(7)) != "TS3Pack") return null;
            r.ReadUInt16();
            var xmlLength = r.ReadUInt32();
            if (xmlLength == 0 || xmlLength > 16 << 20 || 17 + (long)xmlLength > f.Length) return null;
            var xml = Encoding.UTF8.GetString(r.ReadBytes((int)xmlLength));
            var dataStart = f.Position;
            var doc = XDocument.Parse(xml.TrimEnd('\0'));
            var root = doc.Root;
            if (root is null || root.Name.LocalName != "Sims3Package") return null;
            var files = new List<Packed>();
            foreach (var p in root.Elements("PackagedFile"))
            {
                var name = Path.GetFileName((string?)p.Element("Name") ?? "");
                if (!long.TryParse((string?)p.Element("Offset"), out var off) || !long.TryParse((string?)p.Element("Length"), out var len)) return null;
                if (name.Length == 0 || off < 0 || len <= 0 || dataStart + off + len > f.Length) return null; // (a manifest that points outside the file)
                files.Add(new(name, off, len, (string?)p.Element("ContentType") ?? ""));
            }
            return new((string?)root.Attribute("Type") ?? "", (string?)root.Element("DisplayName") ?? Path.GetFileNameWithoutExtension(path),
                (string?)root.Element("Description") ?? "", dataStart, files);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or System.Xml.XmlException or EndOfStreamException or ArgumentException) { return null; }
    }

    // the packages inside, written into dir (made for them); each one checked like any package before it is kept
    public static (List<string> Written, List<string> Refused) Extract(string path, Info info, string dir)
    {
        var written = new List<string>();
        var refused = new List<string>();
        Directory.CreateDirectory(dir);
        using var f = File.OpenRead(path);
        foreach (var p in info.Files.Where(x => x.Name.EndsWith(".package", StringComparison.OrdinalIgnoreCase)))
        {
            var target = Mods.Free(Path.Combine(dir, p.Name));
            if (target.Length >= ModLibrary.MaxGamePath) { refused.Add($"{p.Name}: the folder and name are too long for the game to read"); continue; }
            using (var o = File.Create(target))
            {
                f.Position = info.DataStart + p.Offset;
                var left = p.Length;
                var buf = new byte[1 << 16];
                while (left > 0)
                {
                    var n = f.Read(buf, 0, (int)Math.Min(buf.Length, left));
                    if (n <= 0) break;
                    o.Write(buf, 0, n);
                    left -= n;
                }
            }
            if (ModInstall.PackageProblems(target).FirstOrDefault() is { } why) { File.Delete(target); refused.Add($"{p.Name}: {why}"); }
            else written.Add(target);
        }
        if (written.Count == 0 && Directory.Exists(dir) && !Directory.EnumerateFileSystemEntries(dir).Any()) Directory.Delete(dir);
        return (written, refused);
    }
}
