using System.Buffers.Binary;
using System.Collections.Immutable;
using System.Diagnostics;
using System.IO.Compression;
using System.Reflection;
using System.Reflection.Metadata;
using System.Reflection.Metadata.Ecma335;
using System.Reflection.PortableExecutable;
using System.Text;
using System.Text.Json;
using System.Text.RegularExpressions;

namespace ApexSetup;

// Mod-vs-mod conflict scan: which installed mods clash with each other, by fixed rules only. Read-only: links
// found inside mods are printed, never fetched (the only download is the hook table, ModConflictsHooks.cs). Output names types, members, resource keys and counts, never mod code.
//   CONFLICT: one resource key in packages of 2+ mods with a different meaning (ModConflictsSame.cs; and the likely winner
//     by load order; a translation, an add-on or a folder set to load first is told as such); 2+ core
//     mods replacing one game assembly; one script assembly name with different builds; 2+ mods storing the same game
//     interaction Singleton; 2+ ASIs with known hook tables sharing an exe address.
//   WARNING: 2+ mods with write-type calls, event listeners or native imports and strings in one area of the rule table
//     (ModConflicts.json), with the area's plain-language risk.
//   INFO: identical copies, alternatives inside one archive or mod, descriptions and links (Describe).
//   unknown: archives that cannot be read, ASIs without a hook table, assemblies or method bodies that cannot be decoded.
// Ported from sims3-modernization's compat-scan (tools/mod-conflicts.ts), same rules and rule table.
public static partial class ModConflicts
{
    public sealed record Conflict(string Kind, string Text, string[] Parties, string[] Evidence);
    public sealed record WarningParty(string Name, string[] Calls);
    public sealed record AreaWarning(string Area, string Label, string Risk, WarningParty[] Parties);
    public sealed record Note(string Party, string Text);
    public sealed class Report
    {
        public List<Conflict> Conflicts { get; } = new();
        public List<AreaWarning> Warnings { get; } = new();
        public List<Note> Info { get; } = new();
        public List<Note> Unknown { get; } = new();
        public Dictionary<string, int> Summary { get; } = new();
        public bool NeedsSevenZip { get; set; }             // an archive Windows' tar.exe could not read, and 7-Zip is not installed
    }

    // an ASI whose patched TS3W.exe addresses are known (Match is tested on its file name)
    public sealed record KnownAsi(string Name, Regex Match, HashSet<uint> Addrs, string Source);

    public sealed class Options
    {
        public List<string> Roots { get; init; } = new();   // Mods folders (their Resource.cfg sets the load order) or plain folders
        public string? Bin { get; init; }                   // Game\Bin, for ASIs
        public string? AsmDir { get; init; }                // a folder of the game's script dlls; else the S3SA images of Game\Bin\*.package
        public string? Game { get; init; }                  // the install folder (holds Game\Bin)
        public Rules Rules { get; init; } = Rules.Load();
        public bool Describe { get; init; }
        public bool LoadedOnly { get; init; }               // only what the game loads: no archive, nor a package no Resource.cfg line names
        public bool ModFolders { get; init; }               // every first-level folder is one mod (its files are versions of it)
        public List<KnownAsi> KnownAsi { get; init; } = new();
        public HookTable? Hooks { get; init; }              // applied to the edition of the exe in Bin (or Game)
    }

    // ---------- rule table ----------
    public sealed record Area(string Id, string Label, Regex[] Calls, Regex[] Events, Regex[] Native, string Risk);
    public sealed record Rules(Regex GameOwner, Regex WriteVerb, Regex ReadVerb, Regex Ignore, uint TextFrom, uint TextTo, Area[] Areas, JsonElement[] Cases)
    {
        // the embedded ModConflicts.json, or another table
        public static Rules Load(string? file = null)
        {
            string json;
            if (file is not null) json = File.ReadAllText(file);
            else
            {
                using var s = typeof(ModConflicts).Assembly.GetManifestResourceStream("ModConflicts.json")!;
                json = new StreamReader(s).ReadToEnd();
            }
            var j = JsonDocument.Parse(json).RootElement;
            static Regex Rx(JsonElement e) => new(e.GetString()!, RegexOptions.CultureInvariant);
            static Regex[] All(JsonElement e) => e.EnumerateArray().Select(Rx).ToArray();
            var text = j.GetProperty("exeText").EnumerateArray().Select(x => x.GetUInt32()).ToArray();
            return new(Rx(j.GetProperty("gameOwner")), Rx(j.GetProperty("writeVerb")), Rx(j.GetProperty("readVerb")), Rx(j.GetProperty("ignoreCalls")), text[0], text[1],
                j.GetProperty("areas").EnumerateArray().Select(a => new Area(a.GetProperty("id").GetString()!, a.GetProperty("label").GetString()!,
                    All(a.GetProperty("calls")), All(a.GetProperty("events")), All(a.GetProperty("native")), a.GetProperty("risk").GetString()!)).ToArray(),
                j.TryGetProperty("cases", out var c) ? c.EnumerateArray().ToArray() : Array.Empty<JsonElement>());
        }
    }

    // A call "Owner::Name": write-type or not, and its areas (none when the owner is not a game type or the call is ignored).
    public static (bool Write, string[] Areas) ClassifyCall(Rules r, string text, bool field = false)
    {
        var name = text[(text.LastIndexOf("::", StringComparison.Ordinal) + 2)..];
        var write = field || (r.WriteVerb.IsMatch(name) && !r.ReadVerb.IsMatch(name));
        if (!r.GameOwner.IsMatch(text) || r.Ignore.IsMatch(text)) return (write, Array.Empty<string>());
        return (write, r.Areas.Where(a => a.Calls.Any(x => x.IsMatch(text))).Select(a => a.Id).ToArray());
    }
    public static string[] EventAreas(Rules r, string ev) => r.Areas.Where(a => a.Events.Any(x => x.IsMatch(ev))).Select(a => a.Id).ToArray();
    public static string[] NativeAreas(Rules r, string s) => r.Areas.Where(a => a.Native.Any(x => x.IsMatch(s))).Select(a => a.Id).ToArray();

    // ---------- DBPF packages ----------
    public const uint TypeS3sa = 0x073faa07, TypeKey = 0x0166038c, TypeXml = 0x0333406c, TypeStbl = 0x220557da;
    // a package's own manifest (one per package, instance 0): about the package, not the game's data
    public const uint TypeManifest = 0x73e93eeb;
    // a resource this small in every copy is a placeholder (an empty video or image): mods that blank the same thing agree
    public const uint TinyBytes = 256;
    // (media only: a video, an image or a sound this small is blanked out; a small tuning file is still a real change)
    internal static readonly HashSet<uint> Blankable = [0xB1CC1AF6, 0x00B2D882, 0x2F7D0004, 0x01A527DB, 0x01EEF63A, 0x8070223D];
    static readonly Dictionary<uint, string> TypeNames = new()
    {
        [0x0333406c] = "_XML", [0x03b33ddf] = "ITUN", [0x034aeecb] = "CASP", [0x319e4f1d] = "OBJD", [0x073faa07] = "S3SA", [0x0166038c] = "_KEY", [0x00b2d882] = "_IMG",
        [0x220557da] = "STBL", [0x015a1849] = "GEOM", [0x01661233] = "MODL", [0x01d10f34] = "MLOD", [0x033a1435] = "TXTC", [0x736884f1] = "VPXY", [0x02dc343f] = "OBJK",
        [0xd382bf57] = "FTPT", [0xd3044521] = "RSLT", [0x2e75c764] = "ICON", [0x6b20c4f3] = "CLIP", [0x02d5dd13] = "JAZZ", [0x2f7d0004] = "PNG",
    };
    static string TypeName(uint t) => TypeNames.TryGetValue(t, out var n) ? n : $"0x{t:x8}";

    public readonly record struct Entry(uint Type, uint Group, ulong Instance, uint Offset, int FileSize, uint MemSize, ushort Compressed)
    {
        public string Key => $"{Type:x8}:{Group:x8}:{Instance:x16}";
    }

    static uint U32(ReadOnlySpan<byte> b, int p) => BinaryPrimitives.ReadUInt32LittleEndian(b[p..]);

    // head: the 96-byte header; index: the index block it points at
    static List<Entry> ReadIndex(byte[] head, byte[] index)
    {
        if (head.Length < 96 || Encoding.Latin1.GetString(head, 0, 4) != "DBPF") throw new InvalidDataException("not DBPF");
        var count = U32(head, 36);
        var flags = U32(index, 0);
        var p = 4;
        uint Next() { var v = U32(index, p); p += 4; return v; }
        uint cType = (flags & 1) != 0 ? Next() : 0, cGroup = (flags & 2) != 0 ? Next() : 0, cHi = (flags & 4) != 0 ? Next() : 0;
        var out_ = new List<Entry>();
        for (var i = 0; i < count; i++)
        {
            uint type = (flags & 1) != 0 ? cType : Next(), group = (flags & 2) != 0 ? cGroup : Next(), hi = (flags & 4) != 0 ? cHi : Next();
            out_.Add(new(type, group, ((ulong)hi << 32) | U32(index, p), U32(index, p + 4), (int)(U32(index, p + 8) & 0x7fffffff), U32(index, p + 12),
                BinaryPrimitives.ReadUInt16LittleEndian(index.AsSpan(p + 16))));
            p += 20;
        }
        return out_;
    }

    public static byte[] RefPack(byte[] src)
    {
        var p = 0;
        var flags = src[p++];
        if (src[p++] != 0xfb) throw new InvalidDataException("not RefPack");
        var sb = (flags & 0x80) != 0 ? 4 : 3;
        if ((flags & 0x01) != 0) p += sb;
        var size = 0;
        for (var i = 0; i < sb; i++) size = size * 256 + src[p++];
        var o = 0;
        var dst = new byte[size];
        for (;;)
        {
            int b0 = src[p++], plain, copy = 0, off = 0;
            if (b0 < 0x80) { int b1 = src[p++]; plain = b0 & 3; copy = ((b0 & 0x1c) >> 2) + 3; off = ((b0 & 0x60) << 3) + b1 + 1; }
            else if (b0 < 0xc0) { int b1 = src[p++], b2 = src[p++]; plain = b1 >> 6; copy = (b0 & 0x3f) + 4; off = ((b1 & 0x3f) << 8) + b2 + 1; }
            else if (b0 < 0xe0) { int b1 = src[p++], b2 = src[p++], b3 = src[p++]; plain = b0 & 3; copy = ((b0 & 0x0c) << 6) + b3 + 5; off = ((b0 & 0x10) << 12) + (b1 << 8) + b2 + 1; }
            else if (b0 < 0xfc) plain = ((b0 & 0x1f) << 2) + 4;
            else { plain = b0 & 3; Array.Copy(src, p, dst, o, plain); break; }
            Array.Copy(src, p, dst, o, plain); o += plain; p += plain;
            for (var i = 0; i < copy; i++, o++) dst[o] = dst[o - off];
        }
        return dst;
    }

    // A package on disk (index only, entries read on demand: some packages are GBs) or in memory (from an archive).
    public sealed class Package
    {
        public List<Entry> Entries { get; }
        readonly Func<Entry, byte[]> _raw;
        Package(List<Entry> entries, Func<Entry, byte[]> raw) { Entries = entries; _raw = raw; }
        public byte[] Read(Entry e) { var b = _raw(e); return e.Compressed == 0xffff ? RefPack(b) : b; }

        public static Package Open(byte[] b)
        {
            if (b.Length < 96) throw new InvalidDataException("not DBPF");
            return new(ReadIndex(b, b[(int)U32(b, 64)..]), e => b[(int)Math.Min(e.Offset, b.Length)..(int)Math.Min((long)e.Offset + e.FileSize, b.Length)]);
        }

        public static Package Open(string file)
        {
            using var fs = File.OpenRead(file);
            var head = ReadAt(fs, 0, 96);
            if (head.Length < 96 || Encoding.Latin1.GetString(head, 0, 4) != "DBPF") throw new InvalidDataException("not DBPF");
            return new(ReadIndex(head, ReadAt(fs, U32(head, 64), (int)U32(head, 44))), e => { using var f = File.OpenRead(file); return ReadAt(f, e.Offset, e.FileSize); });
        }

        static byte[] ReadAt(FileStream fs, long pos, int len)
        {
            var b = new byte[len];
            fs.Position = pos;
            fs.ReadAtLeast(b, len, throwOnEndOfStream: false);
            return b;
        }
    }

    // S3SA body: header + XOR-chained 512-byte blocks (s3pi ScriptResource layout); version 1 with all-zero keys is cleartext.
    public static byte[] DecryptS3sa(byte[] body)
    {
        var p = 0;
        if (body[p++] > 1) p += BinaryPrimitives.ReadInt32LittleEndian(body.AsSpan(p)) * 2 + 4;
        p += 68;
        int count = BinaryPrimitives.ReadUInt16LittleEndian(body.AsSpan(p)); p += 2;
        var table = body.AsSpan(p, count * 8); p += count * 8;
        var data = body.AsSpan(p, Math.Min(count * 512, body.Length - p));
        ulong sum = 0;
        for (var i = 0; i < table.Length; i += 8) sum += BinaryPrimitives.ReadUInt64LittleEndian(table[i..]);
        var seed = (int)((ulong)(table.Length - 1) & sum);
        var dst = new byte[count * 512];
        var r = 0;
        for (int i = 0, blk = 0; i < table.Length; i += 8, blk++)
        {
            if ((table[i] & 1) != 0) continue;
            for (var j = 0; j < 512; j++, r++) { var v = data[r]; dst[blk * 512 + j] = (byte)(v ^ table[seed]); seed = (seed + v) % table.Length; }
        }
        return dst;
    }

    // the PE image inside an S3SA body (after its header), or null
    public static byte[]? PeImage(byte[] body)
    {
        for (var i = body.AsSpan().IndexOf("MZ"u8); i >= 0 && i + 0x40 <= body.Length; i = NextMz(body, i + 1))
        {
            var pe = (long)U32(body, i + 0x3c);
            if (i + pe + 4 <= body.Length && body.AsSpan((int)(i + pe), 4).SequenceEqual("PE\0\0"u8)) return body[i..];
        }
        return null;
    }
    static int NextMz(byte[] b, int from) { var k = b.AsSpan(from).IndexOf("MZ"u8); return k < 0 ? -1 : from + k; }

    // An S3SA version 1 body with zero keys around an assembly (as s3pe writes it): for tests.
    public static byte[] S3saWrap(byte[] asm)
    {
        var count = (asm.Length + 511) / 512;
        var b = new byte[71 + count * 8 + count * 512];
        b[0] = 1;
        BinaryPrimitives.WriteUInt16LittleEndian(b.AsSpan(69), (ushort)count);
        asm.CopyTo(b, 71 + count * 8);
        return b;
    }

    // A package of uncompressed resources: for tests.
    public static byte[] WritePackage(IReadOnlyList<(uint Type, uint Group, ulong Instance, byte[] Data)> res)
    {
        var ms = new MemoryStream();
        ms.Write(new byte[96]);
        var offs = new List<uint>();
        foreach (var r in res) { offs.Add((uint)ms.Position); ms.Write(r.Data); }
        var indexAt = (uint)ms.Position;
        var idx = new byte[4 + res.Count * 32];
        for (var i = 0; i < res.Count; i++)
        {
            var s = idx.AsSpan(4 + i * 32);
            BinaryPrimitives.WriteUInt32LittleEndian(s, res[i].Type);
            BinaryPrimitives.WriteUInt32LittleEndian(s[4..], res[i].Group);
            BinaryPrimitives.WriteUInt32LittleEndian(s[8..], (uint)(res[i].Instance >> 32));
            BinaryPrimitives.WriteUInt32LittleEndian(s[12..], (uint)res[i].Instance);
            BinaryPrimitives.WriteUInt32LittleEndian(s[16..], offs[i]);
            BinaryPrimitives.WriteUInt32LittleEndian(s[20..], (uint)res[i].Data.Length | 0x80000000);
            BinaryPrimitives.WriteUInt32LittleEndian(s[24..], (uint)res[i].Data.Length);
            BinaryPrimitives.WriteUInt16LittleEndian(s[30..], 1);
        }
        ms.Write(idx);
        var o = ms.ToArray();
        "DBPF"u8.CopyTo(o);
        BinaryPrimitives.WriteUInt32LittleEndian(o.AsSpan(4), 2);
        BinaryPrimitives.WriteUInt32LittleEndian(o.AsSpan(36), (uint)res.Count);
        BinaryPrimitives.WriteUInt32LittleEndian(o.AsSpan(44), (uint)idx.Length);
        BinaryPrimitives.WriteUInt32LittleEndian(o.AsSpan(60), 3);
        BinaryPrimitives.WriteUInt32LittleEndian(o.AsSpan(64), indexAt);
        return o;
    }

    // ---------- .NET metadata and IL ----------
    sealed class Md
    {
        public required PEReader Pe;
        public required MetadataReader R;
        public required string Name, Mvid;
        public readonly Dictionary<TypeDefinitionHandle, string> Full = new();
    }

    static Md ReadMd(byte[] img)
    {
        var pe = new PEReader(ImmutableArray.Create(img));
        if (!pe.HasMetadata) throw new InvalidDataException("no .NET metadata");
        var r = pe.GetMetadataReader();
        var mod = r.GetModuleDefinition();
        var m = new Md { Pe = pe, R = r, Name = Regex.Replace(r.GetString(mod.Name), @"\.dll$", "", RegexOptions.IgnoreCase),
            Mvid = Convert.ToHexString(r.GetGuid(mod.Mvid).ToByteArray()).ToLowerInvariant() };
        string FullName(TypeDefinitionHandle h)
        {
            if (m.Full.TryGetValue(h, out var s)) return s;
            var t = r.GetTypeDefinition(h);
            var d = t.GetDeclaringType();
            s = !d.IsNil ? FullName(d) + "+" + r.GetString(t.Name) : Join(r.GetString(t.Namespace), r.GetString(t.Name));
            return m.Full[h] = s;
        }
        foreach (var h in r.TypeDefinitions) FullName(h);
        return m;
    }
    static string Join(string ns, string name) => ns.Length == 0 ? name : ns + "." + name;

    static string TypeRefName(MetadataReader r, TypeReferenceHandle h)
    {
        var t = r.GetTypeReference(h);
        return t.ResolutionScope.Kind == HandleKind.TypeReference
            ? TypeRefName(r, (TypeReferenceHandle)t.ResolutionScope) + "+" + r.GetString(t.Name)
            : Join(r.GetString(t.Namespace), r.GetString(t.Name));
    }

    // a TypeDef / TypeRef / TypeSpec: name, and whether it is defined in this assembly
    static (string Name, bool Own) TypeOf(Md m, EntityHandle h)
    {
        switch (h.Kind)
        {
            case HandleKind.TypeDefinition: return (m.Full[(TypeDefinitionHandle)h], true);
            case HandleKind.TypeReference: return (TypeRefName(m.R, (TypeReferenceHandle)h), false);
            case HandleKind.TypeSpecification:
                var b = m.R.GetBlobReader(m.R.GetTypeSpecification((TypeSpecificationHandle)h).Signature);
                var b0 = b.ReadByte();
                if (b0 == 0x15) b0 = b.ReadByte(); // GENERICINST: its CLASS / VALUETYPE type
                if (b0 is 0x12 or 0x11) return TypeOf(m, b.ReadTypeHandle());
                break;
        }
        return ("?", true);
    }

    // a call / field token: owner type, member name, field or not, and whether the member is the assembly's own
    static (string Owner, string Name, bool Field, bool Own)? Member(Md m, int token)
    {
        var h = MetadataTokens.EntityHandle(token);
        var r = m.R;
        switch (h.Kind)
        {
            case HandleKind.MethodDefinition:
                var md = r.GetMethodDefinition((MethodDefinitionHandle)h);
                return (m.Full[md.GetDeclaringType()], r.GetString(md.Name), false, true);
            case HandleKind.FieldDefinition:
                var fd = r.GetFieldDefinition((FieldDefinitionHandle)h);
                return (m.Full[fd.GetDeclaringType()], r.GetString(fd.Name), true, true);
            case HandleKind.MethodSpecification:
                return Member(m, MetadataTokens.GetToken(r.GetMethodSpecification((MethodSpecificationHandle)h).Method));
            case HandleKind.MemberReference:
                var mr = r.GetMemberReference((MemberReferenceHandle)h);
                var field = mr.GetKind() == MemberReferenceKind.Field;
                if (mr.Parent.Kind == HandleKind.MethodDefinition)
                    return (m.Full[r.GetMethodDefinition((MethodDefinitionHandle)mr.Parent).GetDeclaringType()], r.GetString(mr.Name), field, true);
                if (mr.Parent.Kind == HandleKind.ModuleReference) return null;
                var (owner, own) = TypeOf(m, mr.Parent);
                return (owner, r.GetString(mr.Name), field, own);
        }
        return null;
    }

    // enum member names by value (EventTypeId) from the constants of its fields
    static Dictionary<int, string> EnumNames(Md m, string full)
    {
        var o = new Dictionary<int, string>();
        var h = m.Full.FirstOrDefault(x => x.Value == full).Key;
        if (h.IsNil) return o;
        foreach (var fh in m.R.GetTypeDefinition(h).GetFields())
        {
            var f = m.R.GetFieldDefinition(fh);
            var c = f.GetDefaultValue();
            if (c.IsNil) continue;
            var b = m.R.GetBlobReader(m.R.GetConstant(c).Value);
            if (b.Length == 4) o[b.ReadInt32()] = m.R.GetString(f.Name);
        }
        return o;
    }

    // IL operand sizes (ECMA-335 III): -1 token, -2 switch
    static readonly int[] Op1 = new int[256], Op2 = new int[256];
    static ModConflicts()
    {
        foreach (var o in new[] { 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x1f, 0xde }) Op1[o] = 1;
        for (var o = 0x2b; o <= 0x37; o++) Op1[o] = 1;
        for (var o = 0x38; o <= 0x44; o++) Op1[o] = 4;
        Op1[0x20] = 4; Op1[0x21] = 8; Op1[0x22] = 4; Op1[0x23] = 8; Op1[0xdd] = 4; Op1[0x45] = -2;
        foreach (var o in new[] { 0x27, 0x28, 0x29, 0x6f, 0x70, 0x71, 0x72, 0x73, 0x74, 0x75, 0x79, 0x7b, 0x7c, 0x7d, 0x7e, 0x7f, 0x80, 0x81, 0x8c, 0x8d, 0x8f, 0xa3, 0xa4, 0xa5, 0xc2, 0xc6, 0xd0 }) Op1[o] = -1;
        foreach (var o in new[] { 0x06, 0x07, 0x15, 0x16, 0x1c }) Op2[o] = -1;
        foreach (var o in new[] { 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e }) Op2[o] = 2;
        Op2[0x12] = 1; Op2[0x19] = 1;
    }

    readonly record struct Ins(int Op, int V, int Tok);
    static List<Ins> DecodeIl(byte[] b)
    {
        var o = new List<Ins>();
        var p = 0;
        while (p < b.Length)
        {
            int op = b[p++], sz, v = 0, tok = 0;
            if (op == 0xfe) { op = 0xfe00 | b[p++]; sz = Op2[op & 0xff]; } else sz = Op1[op];
            if (sz == -1) { tok = (int)U32(b, p); p += 4; }
            else if (sz == -2) p += 4 + 4 * (int)U32(b, p);
            else if (sz == 1) { v = op == 0x1f ? (sbyte)b[p] : b[p]; p += 1; }
            else if (sz == 4) { v = BinaryPrimitives.ReadInt32LittleEndian(b.AsSpan(p)); p += 4; }
            else p += sz;
            o.Add(new(op, v, tok));
        }
        if (p != b.Length) throw new InvalidDataException("IL overrun");
        return o;
    }
    static int? LdcValue(Ins i) => i.Op is >= 0x15 and <= 0x1e ? i.Op - 0x16 : i.Op is 0x1f or 0x20 ? i.V : null;

    // every external call / field store of an assembly as "Owner::Name"; the event id of EventTracker.AddListener calls
    static (List<(string Text, bool Field, int? Event)> Calls, int Bad) IlRefs(Md m)
    {
        var calls = new List<(string, bool, int?)>();
        var bad = 0;
        foreach (var mh in m.R.MethodDefinitions)
        {
            var md = m.R.GetMethodDefinition(mh);
            if (md.RelativeVirtualAddress == 0 || (md.ImplAttributes & MethodImplAttributes.InternalCall) != 0) continue;
            try
            {
                var ins = DecodeIl(m.Pe.GetMethodBody(md.RelativeVirtualAddress).GetILBytes()!);
                for (var k = 0; k < ins.Count; k++)
                {
                    var x = ins[k];
                    if (x.Op is not (0x28 or 0x6f or 0x7d or 0x80)) continue; // call, callvirt, stfld, stsfld
                    if (Member(m, x.Tok) is not { Own: false } mem) continue;
                    int? ev = null;
                    // shortcut: the event id is the ldc.i4 before the delegate newobj (C# shape AddListener(id, new Delegate(...))); other shapes give no id
                    if (mem.Name == "AddListener" && Regex.IsMatch(mem.Owner, @"(^|\.)EventTracker$"))
                        for (int j = k - 1, seenNew = 0; j >= Math.Max(0, k - 10) && ev is null; j--)
                        {
                            if (ins[j].Op == 0x73) seenNew = 1;
                            else if (seenNew == 1) ev = LdcValue(ins[j]);
                        }
                    calls.Add(($"{mem.Owner}::{mem.Name}", x.Op is 0x7d or 0x80, ev));
                }
            }
            catch (Exception e) when (e is InvalidDataException or BadImageFormatException or ArgumentException or IndexOutOfRangeException or InvalidOperationException or KeyNotFoundException) { bad++; }
        }
        return (calls, bad);
    }

    // ---------- native (ASI) ----------
    // imported DLL and function names, printable strings, and code-address immediates (push / mov r32 imm32 in code sections)
    static (List<string> Imports, List<string> Strings, HashSet<uint> Addrs) AsiFacts(byte[] img, uint textFrom, uint textTo)
    {
        var h = new PEHeaders(new MemoryStream(img));
        var secs = h.SectionHeaders;
        int Off(long rva)
        {
            foreach (var s in secs)
                if (rva >= s.VirtualAddress && rva < s.VirtualAddress + Math.Max(s.VirtualSize, s.SizeOfRawData)) return (int)(rva - s.VirtualAddress + s.PointerToRawData);
            throw new InvalidDataException($"rva 0x{rva:x} outside sections");
        }
        string Cstr(int at) { var e = Array.IndexOf(img, (byte)0, at); return Encoding.Latin1.GetString(img, at, (e < 0 ? img.Length : e) - at); }
        var imports = new List<string>();
        var addrs = new HashSet<uint>();
        try
        {
            var it = h.PEHeader!.ImportTableDirectory.RelativeVirtualAddress;
            var pe32p = h.PEHeader.Magic == PEMagic.PE32Plus;
            for (var d = it != 0 ? Off(it) : -1; d >= 0 && U32(img, d + 12) != 0; d += 20)
            {
                imports.Add(Cstr(Off(U32(img, d + 12))).ToLowerInvariant());
                for (var t = Off(U32(img, d) != 0 ? U32(img, d) : U32(img, d + 16)); U32(img, t) != 0; t += pe32p ? 8 : 4)
                {
                    var v = U32(img, t);
                    if ((v & 0x80000000) != 0) continue;
                    imports.Add(Cstr(Off(v) + 2));
                }
            }
        }
        catch (Exception e) when (e is InvalidDataException or ArgumentOutOfRangeException or IndexOutOfRangeException) { } // imports unreadable: strings still count
        foreach (var s in secs.Where(s => (s.SectionCharacteristics & SectionCharacteristics.MemExecute) != 0))
            for (int p = s.PointerToRawData, e = Math.Min(img.Length, s.PointerToRawData + Math.Min(s.SizeOfRawData, s.VirtualSize == 0 ? s.SizeOfRawData : s.VirtualSize)) - 4; p < e; p++)
            {
                var o = img[p];
                if (o == 0x68 || o is >= 0xb8 and <= 0xbf) { var v = U32(img, p + 1); if (v >= textFrom && v <= textTo) addrs.Add(v); }
            }
        var strings = Regex.Matches(Encoding.Latin1.GetString(img), @"[\x20-\x7e]{6,200}").Take(20000).Select(x => x.Value).ToList();
        return (imports, strings, addrs);
    }

    // ---------- load order ----------
    // Databases sort by Priority, highest first; among equal priority the first registered wins (verified in the game's
    // registration code). Registration order inside one priority is Resource.cfg directive order, then the file system's
    // listing order inside one PackedFile pattern (NTFS: case-insensitive name order): unverified in game, hence "likely
    // winner". A folder without Resource.cfg gets a synthetic one: Priority 500, *.package ... 6 folders deep.
    public sealed record Directive(int Priority, Regex Pattern, string Text);
    public static List<Directive> ParseResourceCfg(string text)
    {
        var o = new List<Directive>();
        var prio = 0;
        foreach (var raw in text.Split('\n'))
        {
            var l = Regex.Replace(raw, "#.*$", "").Trim();
            var m = Regex.Match(l, @"^(\w+)\s+(.+)$");
            if (!m.Success) continue;
            if (m.Groups[1].Value.Equals("priority", StringComparison.OrdinalIgnoreCase)) prio = int.TryParse(m.Groups[2].Value, out var n) ? n : 0;
            else if (m.Groups[1].Value.Equals("packedfile", StringComparison.OrdinalIgnoreCase))
                o.Add(new(prio, new Regex("^" + string.Join("[^/]*", m.Groups[2].Value.Replace('\\', '/').Split('*').Select(Regex.Escape)) + "$", RegexOptions.IgnoreCase | RegexOptions.CultureInvariant), m.Groups[2].Value));
        }
        return o;
    }
    static readonly List<Directive> Synthetic = Enumerable.Range(0, 6)
        .Select(d => new Directive(500, new Regex("^" + string.Concat(Enumerable.Repeat("[^/]*/", d)) + @"[^/]*\.package$", RegexOptions.IgnoreCase), string.Concat(Enumerable.Repeat("*/", d)) + "*.package")).ToList();

    // the sort key of one package path (relative to the Mods folder, '/'), or null when no directive loads it
    public static (int, int, string)? LoadKey(List<Directive> dirs, string rel)
    {
        var i = dirs.FindIndex(d => d.Pattern.IsMatch(rel));
        return i < 0 ? null : (-dirs[i].Priority, i, rel.ToLowerInvariant());
    }
    public static int CompareKey((int, int, string) a, (int, int, string) b) =>
        a.Item1 != b.Item1 ? a.Item1.CompareTo(b.Item1) : a.Item2 != b.Item2 ? a.Item2.CompareTo(b.Item2) : string.CompareOrdinal(a.Item3, b.Item3);

    // ---------- archives (untrusted input) ----------
    // .rar / .7z through Windows' tar.exe (bsdtar, libarchive), else 7-Zip when it is installed; called with an argument list,
    // no shell. The listing is checked first: no absolute paths, no '..', no links, at most UnpackFiles entries and UnpackCap
    // bytes in total. Each archive goes into a new temp folder that the scan deletes when it ends. Nothing extracted is run.
    // Password-protected archives fail in the extractor and come back as unknown with its message.
    public const long UnpackCap = 2L << 30;
    public const int UnpackFiles = 20000;
    public const string SevenZipInstall = "winget install --id 7zip.7zip -e";

    // an entry name that stays inside the folder: relative, no '..' segment, no drive or UNC prefix, no NUL
    public static bool SafeName(string n) =>
        n.Length > 0 && !Regex.IsMatch(n, @"^([\\/]|[A-Za-z]:)") && !n.Split('/', '\\').Contains("..") && !n.Contains('\0');

    // the extractors at hand, in order: Windows' tar.exe, then 7-Zip
    public static Func<List<(string Kind, string Exe)>> Extractors = () =>
    {
        var o = new List<(string, string)>();
        var tar = Path.Combine(Environment.SystemDirectory, "tar.exe");
        if (File.Exists(tar)) o.Add(("tar", tar));
        var seven = new[] { Environment.SpecialFolder.ProgramFiles, Environment.SpecialFolder.ProgramFilesX86 }
            .Select(f => Path.Combine(Environment.GetFolderPath(f), "7-Zip", "7z.exe"))
            .Concat((Environment.GetEnvironmentVariable("PATH") ?? "").Split(Path.PathSeparator).Where(d => d.Length > 0).Select(d => Path.Combine(d, "7z.exe")))
            .FirstOrDefault(File.Exists);
        if (seven is not null) o.Add(("7z", seven));
        return o;
    };

    // the extracted folder and its files ('/' names, sorted), or why not
    public static (string? Dir, List<string> Files, string? Unknown) Extract(string archive)
    {
        var tools = Extractors();
        var hint = tools.Any(t => t.Kind == "7z") ? "" : $"; 7-Zip may read it ({SevenZipInstall}), then scan again";
        if (tools.Count == 0) return (null, new(), $"no extractor for {Path.GetExtension(archive)}{hint}");
        var errs = new List<string>();
        foreach (var (kind, exe) in tools)
        {
            List<(string Name, long Size, char Type)> entries;
            try { entries = kind == "tar" ? ListTar(exe, archive) : List7z(exe, archive); }
            catch (Exception e) when (e is InvalidDataException or System.ComponentModel.Win32Exception) { errs.Add($"{kind}: {e.Message}"); continue; }
            if (entries.FirstOrDefault(x => !SafeName(x.Name) || x.Type is 'l' or 'h') is { Name: not null } bad)
                return (null, new(), $"archive refused: unsafe entry \"{(bad.Name.Length > 80 ? bad.Name[..80] : bad.Name)}\" (absolute path, '..' or link)");
            if (entries.Count > UnpackFiles) return (null, new(), $"archive refused: {entries.Count} entries (cap {UnpackFiles})");
            if (entries.Sum(x => Math.Max(0, x.Size)) > UnpackCap) return (null, new(), $"archive refused: over {UnpackCap >> 30} GB unpacked");
            var dir = Directory.CreateTempSubdirectory("modscan-").FullName;
            try
            {
                if (kind == "tar") Run(exe, "-xf", archive, "-C", dir);
                else Run(exe, "x", "-y", "-snl-", "-o" + dir, "--", archive);
                var files = new List<string>();
                long total = 0;
                void Walk(string d)
                {
                    foreach (var e in new DirectoryInfo(d).EnumerateFileSystemInfos())
                    {
                        if (e.Attributes.HasFlag(FileAttributes.ReparsePoint)) continue; // links are not followed
                        if (e is DirectoryInfo sub) Walk(sub.FullName);
                        else { files.Add(Slash(Path.GetRelativePath(dir, e.FullName))); total += ((FileInfo)e).Length; }
                    }
                }
                Walk(dir);
                if (total > UnpackCap || files.Count > UnpackFiles) { Remove(dir); return (null, new(), "archive refused: over the size or file cap after extraction"); }
                files.Sort(StringComparer.Ordinal);
                return (dir, files, null);
            }
            catch (Exception e) when (e is InvalidDataException or IOException or System.ComponentModel.Win32Exception)
            {
                Remove(dir);
                errs.Add($"{kind}: {e.Message}");
            }
        }
        return (null, new(), $"not extracted (password, damaged or unsupported variant such as some RAR5): {string.Join("; ", errs)}{hint}");
    }
    static void Remove(string dir) { try { Directory.Delete(dir, true); } catch (IOException) { } catch (UnauthorizedAccessException) { } }

    // bsdtar: names from -t, size and type from -tv (same order)
    static List<(string, long, char)> ListTar(string tar, string archive)
    {
        static string[] Lines(string s) => s.Split('\n').Select(l => l.TrimEnd('\r')).Where(l => l.Length > 0).ToArray();
        var names = Lines(Run(tar, "-tf", archive));
        var longs = Lines(Run(tar, "-tvf", archive));
        if (names.Length != longs.Length) throw new InvalidDataException("listing mismatch");
        return names.Select((n, i) => (n, long.TryParse(longs[i].Trim().Split(' ', StringSplitOptions.RemoveEmptyEntries).ElementAtOrDefault(4), out var z) ? z : 0, longs[i][0])).ToList();
    }

    // 7-Zip's technical listing: blocks of "Path = ", "Size = ", "Attributes = " after the "----------" line
    static List<(string, long, char)> List7z(string exe, string archive)
    {
        var o = new List<(string, long, char)>();
        var text = Run(exe, "l", "-slt", "--", archive);
        var at = text.IndexOf("\n----------", StringComparison.Ordinal);
        string? name = null; long size = 0; var attr = "";
        void Flush() { if (name is not null) o.Add((name, size, attr.Contains('D') ? 'd' : attr.Contains('l', StringComparison.OrdinalIgnoreCase) ? 'l' : '-')); name = null; size = 0; attr = ""; }
        foreach (var raw in (at < 0 ? "" : text[(at + 1)..]).Split('\n'))
        {
            var l = raw.TrimEnd('\r');
            if (l.StartsWith("Path = ")) { Flush(); name = l[7..]; }
            else if (l.StartsWith("Size = ")) size = long.TryParse(l[7..], out var z) ? z : 0;
            else if (l.StartsWith("Attributes = ")) attr = l[13..];
        }
        Flush();
        return o;
    }

    static string Run(string exe, params string[] args)
    {
        var psi = new ProcessStartInfo(exe) { RedirectStandardOutput = true, RedirectStandardError = true, RedirectStandardInput = true, UseShellExecute = false, CreateNoWindow = true };
        foreach (var a in args) psi.ArgumentList.Add(a);
        using var p = Process.Start(psi)!;
        p.StandardInput.Close(); // (an encrypted archive fails instead of asking for a password)
        var err = p.StandardError.ReadToEndAsync();
        var outp = p.StandardOutput.ReadToEndAsync();
        if (!p.WaitForExit(300_000)) { try { p.Kill(true); } catch (InvalidOperationException) { } throw new InvalidDataException($"{Path.GetFileName(exe)} timed out"); }
        if (p.ExitCode != 0) throw new InvalidDataException((err.Result.Trim() is { Length: > 0 } m ? m : outp.Result.Trim()).Split('\n')[0].Trim());
        return outp.Result;
    }

    // ---------- scan ----------
    sealed class Party(string name, string file, string mod)
    {
        public string Name = name, File = file, Mod = mod;
        public readonly HashSet<string> Kind = new();
        public readonly List<(string Name, string Mvid)> Asms = new();
        public readonly List<string> Replaces = new();
        public readonly Dictionary<string, HashSet<string>> Singletons = new(); // field -> MVIDs of the assemblies that store it
        public readonly Dictionary<string, Dictionary<string, int>> Writes = new();
        public readonly List<string> Info = new(), Unknown = new();
        public HashSet<uint>? Addrs;
        public Dictionary<uint, string>? Purposes;
        public string? KnownTable;
        public string? HookMod; // hook-table mod name
        public Dictionary<uint, IReadOnlyDictionary<string, string>>? Coordinated; // address -> other mod -> "<who> stands down|chains"
        public void AddWrite(string area, string call)
        {
            if (!Writes.TryGetValue(area, out var m)) Writes[area] = m = new();
            m[call] = m.GetValueOrDefault(call) + 1;
        }
    }
    sealed record PkgRec(Party Party, string Label, (int, int, string)? Key, Package Pkg);

    static readonly Regex GameModules = new(@"^(Sims3GameplaySystems|Sims3GameplayObjects|Sims3StoreObjects|Sims3Metadata|ScriptCore|SimIFace|UI|mscorlib|System|System\.Xml)$");
    static readonly Regex UrlRe = new(@"https?://[A-Za-z0-9.-]+\.[a-z]{2,}(/[^\s""'<>)\]]*)?");

    static IEnumerable<string> Walk(string dir)
    {
        if (!Directory.Exists(dir)) yield break;
        foreach (var e in Directory.EnumerateFileSystemEntries(dir))
            if (Directory.Exists(e)) { foreach (var f in Walk(e)) yield return f; }
            else yield return e;
    }
    static string Slash(string p) => p.Replace('\\', '/');

    // the game's script assemblies: a folder of dlls, else the S3SA images in Game\Bin's packages
    static Dictionary<string, byte[]> GameAssemblies(Options o)
    {
        var d = new Dictionary<string, byte[]>();
        if (o.AsmDir is not null && Directory.Exists(o.AsmDir))
            foreach (var f in Directory.EnumerateFiles(o.AsmDir, "*.dll")) d[Path.GetFileNameWithoutExtension(f)] = File.ReadAllBytes(f);
        else if (o.Game is not null)
            foreach (var f in Walk(Path.Combine(o.Game, "Game", "Bin")).Where(n => n.EndsWith(".package", StringComparison.OrdinalIgnoreCase)))
                try
                {
                    var p = Package.Open(f);
                    foreach (var e in p.Entries.Where(e => e.Type == TypeS3sa))
                        if (Image(p.Read(e)) is { } img)
                            try { d[ReadMd(img).Name] = img; } catch (BadImageFormatException) { } catch (InvalidDataException) { }
                }
                catch (Exception e) when (e is InvalidDataException or IOException or ArgumentException) { }
        return d;
    }
    // One build of a script, adapted: in a folder Resource.cfg loads first, a copy of the same build as the others' (Tiny UI Fix
    // makes one of each script whose windows it enlarges). Another build there is another version of the mod, a conflict.
    static bool Adapted(List<(PkgRec Rec, Entry E)> l, List<(PkgRec Rec, Entry E)> sorted)
    {
        if (l[0].E.Type != TypeS3sa || sorted.Count != l.Count || !sorted.Skip(1).All(x => x.Rec.Key!.Value.Item1 > sorted[0].Rec.Key!.Value.Item1)) return false;
        var builds = l.Select(x => { try { return Build(x.Rec.Pkg.Read(x.E)); } catch (Exception e) when (e is InvalidDataException or IOException or IndexOutOfRangeException or ArgumentException) { return null; } }).ToList();
        return builds[0] is not null && builds.All(b => b == builds[0]);
    }

    // which build of an assembly a script resource holds: its name and module id, null when it can't be read
    public static string? Build(byte[] s3sa)
    {
        try
        {
            if (Image(s3sa) is not { } img) return null;
            using var pe = new PEReader(ImmutableArray.Create(img));
            if (!pe.HasMetadata) return null;
            var r = pe.GetMetadataReader();
            var m = r.GetModuleDefinition();
            return r.GetString(m.Name) + " " + r.GetGuid(m.Mvid);
        }
        catch (Exception e) when (e is BadImageFormatException or InvalidDataException or InvalidOperationException or IndexOutOfRangeException or ArgumentException) { return null; }
    }

    static byte[]? Image(byte[] s3sa) => PeImage(s3sa) ?? PeImage(DecryptS3sa(s3sa));

    // The packages that share some keys. Recs: in load order (the one in use first), those Resource.cfg does not load last.
    // All: the types of the shared keys, Types: of those that differ. Real: the keys that differ where some copy is more than a placeholder (TinyBytes or
    // less in every copy: blank intro videos and the like, the same fix whichever wins). Texts: the texts that differ in
    // the text tables among them, null when one can't be read.
    sealed class Shared(List<Party> parties, List<PkgRec> recs, string winner, bool alt, List<string> notLoaded)
    {
        public readonly List<Party> Parties = parties;
        public readonly List<PkgRec> Recs = recs;
        public readonly string Winner = winner;
        public readonly bool Alt = alt;
        public readonly List<string> NotLoaded = notLoaded;
        public readonly Dictionary<string, int> All = new(), Types = new();
        public readonly List<string> Keys = new();
        public readonly HashSet<PkgRec> Unlike = new();
        public int Same, Real;
        public int? Texts = 0;
    }

    // The copies of one key by what they mean: each one's number is the place of the first copy like it (its own when it
    // is unlike the ones before, or can't be read). Read one at a time; a copy with the bytes of the first costs no more.
    static int[] Sides(List<(PkgRec Rec, Entry E)> l)
    {
        var side = new int[l.Count];
        var digest = new UInt128?[l.Count];
        byte[]? first = null;
        var firstAt = 0;
        for (var i = 0; i < l.Count; i++)
        {
            var e = l[i].E;
            side[i] = i;
            // (too large for tuning or texts, and no other copy of its size: different, without reading it)
            if (e.MemSize > MeaningCap && l.Count(x => x.E.MemSize == e.MemSize) == 1) continue;
            byte[] b;
            try { b = l[i].Rec.Pkg.Read(e); }
            catch (Exception x) when (x is InvalidDataException or IOException or IndexOutOfRangeException or ArgumentException) { continue; }
            if (first is null) { (first, firstAt) = (b, i); continue; }
            if (b.AsSpan().SequenceEqual(first)) { side[i] = firstAt; continue; }
            var mine = Canon(e.Type, b);
            if (mine is null && l.Count == 2) continue; // (two copies, and only this one's bytes say what it is: they differ)
            digest[firstAt] ??= Meaning(e.Type, first);
            digest[i] = mine ?? Digest(0, b);
            for (var j = 0; j < i; j++)
                if (side[j] == j && digest[j] == digest[i]) { side[i] = j; break; }
        }
        return side;
    }

    // What two packages that differ on some keys are to each other, with the conflict's parties (the one in use first).
    //   translation: the one in use holds only texts, of a mod that holds more.
    //   add-on: all the one in use holds is in the other, which holds more, and its name says it is a piece of it.
    //   decided: the one in use is in a folder Resource.cfg gives a higher Priority than the others'.
    //   resource key: anything else, and whenever the piece is not the one in use (it has no effect then).
    static (string Kind, List<Party> Parties) Relation(Shared g)
    {
        var loaded = g.Recs.Where(r => r.Key is not null).ToList();
        if (loaded.Count == 2 && g.Recs.Count == 2)
        {
            var (piece, whole) = (loaded[0], loaded[1]);
            static bool Content(Entry e) => e.Type is not (TypeKey or TypeManifest);
            static bool OnlyTexts(PkgRec r) => r.Pkg.Entries.Where(Content).All(e => e.Type == TypeStbl);
            static string Name(PkgRec r) => Path.GetFileNameWithoutExtension(r.Label[(r.Label.LastIndexOfAny(['/', '!']) + 1)..]);
            List<Party> ps = [piece.Party, whole.Party];
            if (g.Types.Keys.All(t => t == "STBL") && OnlyTexts(piece) && !OnlyTexts(whole)) return ("translation", ps);
            var keys = whole.Pkg.Entries.Where(Content).Select(e => (e.Type, e.Group, e.Instance)).ToHashSet();
            var mine = piece.Pkg.Entries.Where(Content).Select(e => (e.Type, e.Group, e.Instance)).ToHashSet();
            if (mine.Count < keys.Count && mine.IsSubsetOf(keys) && SaysPiece(Name(piece), Name(whole))) return ("add-on", ps);
        }
        if (loaded.Count > 1 && loaded.Skip(1).All(r => r.Key!.Value.Item1 > loaded[0].Key!.Value.Item1)) return ("decided", g.Parties);
        return ("resource key", g.Parties);
    }

    public static Report Scan(Options o)
    {
        var temps = new List<string>(); // extracted archives: kept until the key comparison has read them
        try { return Scan(o, temps); }
        finally { foreach (var d in temps) Remove(d); }
    }

    static Report Scan(Options o, List<string> temps)
    {
        var R = o.Rules;
        var parties = new List<Party>();
        var recs = new List<PkgRec>();
        var game = GameAssemblies(o);
        var gameMvid = new Dictionary<string, string>();
        var eventNames = new Dictionary<int, string>();
        foreach (var (n, b) in game)
            try
            {
                var m = ReadMd(b);
                gameMvid[n] = m.Mvid;
                if (n == "Sims3GameplaySystems") eventNames = EnumNames(m, "Sims3.Gameplay.EventSystem.EventTypeId");
            }
            catch (Exception e) when (e is BadImageFormatException or InvalidDataException) { }
        var report = new Report();
        var edition = o.Hooks?.EditionOf(o.Bin ?? (o.Game is null ? null : Path.Combine(o.Game, "Game", "Bin")));
        if (o.Hooks is { } table)
            report.Info.Add(new("hook table", $"dated {table.Date}, {table.Source}; " + (edition is null ? "game build not identified, so it is not applied" : $"game build {edition}")));
        static bool OneMod(IEnumerable<Party> ps) => ps.Select(p => p.Mod).Distinct().Count() == 1;

        void DoAsm(Party p, byte[] img, string where)
        {
            Md m;
            try { m = ReadMd(img); }
            catch (Exception e) when (e is BadImageFormatException or InvalidDataException or InvalidOperationException) { p.Unknown.Add($"{where}: assembly not parsed ({e.Message})"); return; }
            p.Asms.Add((m.Name, m.Mvid));
            if (GameModules.IsMatch(m.Name)) // core mod: the game's own calls, no area scan
            {
                if (gameMvid.GetValueOrDefault(m.Name) != m.Mvid) { p.Replaces.Add(m.Name); p.Kind.Add("core"); }
                return;
            }
            var (calls, bad) = IlRefs(m);
            if (bad > 0) p.Unknown.Add($"{m.Name}: {bad} method bodies not decoded");
            foreach (var c in calls)
            {
                if (c.Event is not null || Regex.IsMatch(c.Text, @"(^|\.)EventTracker::AddListener$"))
                {
                    if (c.Event is { } id)
                    {
                        var ev = eventNames.GetValueOrDefault(id) ?? $"EventTypeId#{id}";
                        foreach (var a in EventAreas(R, ev)) p.AddWrite(a, $"listens {ev}");
                    }
                    continue;
                }
                var text = c.Field ? $"{c.Text} (store)" : c.Text;
                var k = ClassifyCall(R, text, c.Field);
                if (c.Field && c.Text.EndsWith("::Singleton", StringComparison.Ordinal) && R.GameOwner.IsMatch(c.Text))
                    (p.Singletons.TryGetValue(c.Text, out var mv) ? mv : p.Singletons[c.Text] = new()).Add(m.Mvid);
                if (k.Write) foreach (var a in k.Areas) p.AddWrite(a, text);
            }
        }

        // path: the file on disk (for its version resource), null inside an archive
        void DoAsi(Party p, byte[] img, string file, string? path)
        {
            p.Kind.Add("asi");
            (List<string> Imports, List<string> Strings, HashSet<uint> Addrs) f;
            try { f = AsiFacts(img, R.TextFrom, R.TextTo); }
            catch (Exception e) when (e is BadImageFormatException or InvalidDataException or ArgumentException) { p.Unknown.Add($"ASI not a readable PE ({e.Message})"); return; }
            foreach (var s in f.Imports.Concat(f.Strings))
                foreach (var a in NativeAreas(R, s)) p.AddWrite(a, s.Length > 60 ? s[..57] + "..." : s);
            var name = Path.GetFileName(file);
            if (o.KnownAsi.FirstOrDefault(k => k.Match.IsMatch(name)) is { } known) { p.Addrs = known.Addrs; p.KnownTable = known.Source; return; }
            // the hook table applies to a build it lists (by hash or version) on the identified game edition; another build of
            // a listed mod is INFO, its addresses may have moved
            if (o.Hooks is { } ht && edition is not null && ht.Mods.FirstOrDefault(m => m.File.IsMatch(name)) is { } hm)
            {
                var sha = Convert.ToHexString(System.Security.Cryptography.SHA256.HashData(img)).ToLowerInvariant();
                var version = path is null ? null : FileVersionInfo.GetVersionInfo(path).FileVersion;
                if (hm.Sha256.Contains(sha) || version == hm.Version)
                {
                    var rows = ht.Hooks.Where(h => h.Mod == hm.Name && h.Edition == edition).ToList();
                    p.Addrs = rows.Select(h => h.Address).ToHashSet();
                    p.Purposes = rows.GroupBy(h => h.Address).ToDictionary(g => g.Key, g => g.First().Purpose);
                    p.KnownTable = $"hook table {ht.Date}";
                    p.HookMod = hm.Name;
                    p.Coordinated = rows.Where(h => h.Coordinated.Count > 0).GroupBy(h => h.Address).ToDictionary(g => g.Key, g => g.First().Coordinated);
                    return;
                }
                p.Info.Add($"hook table may be out of date for {name} (table dated {ht.Date}): this build is not listed, its hooks are not compared");
            }
            p.Addrs = f.Addrs;
            p.Unknown.Add($"ASI hook sites unknown: no hook table; {f.Addrs.Count} code-address immediates (candidates, not proven hooks)");
        }

        void DoPackage(Party p, Func<Package> open, string label, string rel, List<Directive> dirs)
        {
            Package pkg;
            try { pkg = open(); }
            catch (Exception e) when (e is InvalidDataException or IOException or ArgumentException) { p.Unknown.Add($"{label}: not a readable package ({e.Message})"); return; }
            p.Kind.Add(pkg.Entries.Any(e => e.Type == TypeS3sa) ? "script" : "package");
            recs.Add(new(p, label, LoadKey(dirs, rel), pkg));
            foreach (var e in pkg.Entries.Where(e => e.Type == TypeS3sa))
                try
                {
                    if (Image(pkg.Read(e)) is { } img) DoAsm(p, img, label);
                    else p.Unknown.Add($"{label}: S3SA {e.Instance:x} without a PE image");
                }
                catch (Exception x) when (x is InvalidDataException or IOException or ArgumentException or IndexOutOfRangeException) { p.Unknown.Add($"{label}: S3SA not read ({x.Message})"); }
            if (o.Describe)
                foreach (var e in pkg.Entries.Where(e => (e.Type == TypeXml || e.Type == TypeStbl) && e.MemSize < 262144))
                    try
                    {
                        var b = pkg.Read(e);
                        var text = Encoding.Latin1.GetString(b) + "\n" + Encoding.Unicode.GetString(b, 0, b.Length & ~1);
                        foreach (var u in UrlRe.Matches(text).Select(x => x.Value).Distinct()) p.Info.Add($"link (not fetched): {u}");
                    }
                    catch (Exception x) when (x is InvalidDataException or IOException or IndexOutOfRangeException) { }
        }

        foreach (var root in o.Roots)
        {
            var cfgAt = new[] { Path.Combine(root, "Resource.cfg"), Path.Combine(Path.GetDirectoryName(Path.GetFullPath(root)) ?? root, "Resource.cfg") }.FirstOrDefault(File.Exists);
            var @base = cfgAt is not null ? Path.GetDirectoryName(cfgAt)! : root;
            var dirs = cfgAt is not null ? ParseResourceCfg(File.ReadAllText(cfgAt)) : Synthetic;
            var rootName = Path.GetFileName(Path.TrimEndingDirectorySeparator(Path.GetFullPath(root)));
            foreach (var f in Walk(root))
            {
                var ext = Path.GetExtension(f).ToLowerInvariant();
                if (ext is not (".package" or ".zip" or ".rar" or ".7z" or ".asi" or ".dbc")) continue;
                var rel = Slash(Path.GetRelativePath(@base, f));
                if (o.LoadedOnly && (ext is ".zip" or ".rar" or ".7z" || ext == ".package" && LoadKey(dirs, rel) is null)) continue;
                var relRoot = Slash(Path.GetRelativePath(root, f));
                var file = $"{rootName}/{relRoot}";
                // ModFolders: every first-level folder is one mod (a collection of versions); else every file
                var p = new Party(Path.GetFileNameWithoutExtension(f), file, o.ModFolders && relRoot.Contains('/') ? $"{rootName}/{relRoot.Split('/')[0]}" : file);
                parties.Add(p);
                if (ext == ".dbc") { p.Unknown.Add("DCCache .dbc (the game's installed store content): not scanned"); continue; }
                if (ext == ".asi") { DoAsi(p, File.ReadAllBytes(f), f, f); continue; }
                if (ext == ".package") { DoPackage(p, () => Package.Open(f), p.File, rel, dirs); continue; }
                // as if unpacked into a folder named after the archive (unverified: players unpack that way or flatten)
                OpenArchive(p, f, null, ext, p.File, rel[..^ext.Length], dirs, 0);
            }
        }

        // one archive member: package, ASI, nested archive (one level below the top archive) or readme text
        void Member(Party p, string name, string? path, byte[]? data, string lab, string rel, List<Directive> dirs, int depth)
        {
            var e = Path.GetExtension(name).ToLowerInvariant();
            byte[] Buf() => data ?? File.ReadAllBytes(path!);
            if (e == ".package") DoPackage(p, () => path is not null ? Package.Open(path) : Package.Open(data!), $"{lab}!{name}", $"{rel}/{name}", dirs);
            else if (e == ".asi") DoAsi(p, Buf(), name, path);
            else if (e is ".zip" or ".rar" or ".7z")
            {
                if (depth >= 1) p.Unknown.Add($"{lab}!{name}: archive nested more than one level, unpack it by hand");
                else OpenArchive(p, path, data, e, $"{lab}!{name}", $"{rel}/{name[..^e.Length]}", dirs, depth + 1);
            }
            else if (o.Describe && (Regex.IsMatch(name, @"\.(txt|md)$", RegexOptions.IgnoreCase) || name.Contains("readme", StringComparison.OrdinalIgnoreCase)))
            {
                var b = Buf();
                if (b.Length >= 262144) return;
                var t = Encoding.UTF8.GetString(b);
                var first = t.Split('\n').Select(l => l.Trim()).FirstOrDefault(l => l.Length > 3);
                if (first is not null) p.Info.Add($"description ({name}): {(first.Length > 160 ? first[..160] : first)}");
                foreach (var u in UrlRe.Matches(t).Select(x => x.Value).Distinct()) p.Info.Add($"link (not fetched): {u}");
            }
        }

        void OpenArchive(Party p, string? path, byte[]? data, string ext, string lab, string rel, List<Directive> dirs, int depth)
        {
            if (ext == ".zip")
            {
                List<(string, byte[])> items;
                try
                {
                    using var z = data is not null ? new ZipArchive(new MemoryStream(data)) : ZipFile.OpenRead(path!);
                    items = z.Entries.Where(x => !x.FullName.EndsWith('/')).Select(x => { using var s = x.Open(); var ms = new MemoryStream(); s.CopyTo(ms); return (x.FullName, ms.ToArray()); }).ToList();
                }
                catch (Exception x) when (x is InvalidDataException or IOException or NotSupportedException) { p.Unknown.Add($"{lab}: zip not readable ({x.Message})"); return; }
                foreach (var (n, b) in items) Member(p, n, null, b, lab, rel, dirs, depth);
                return;
            }
            if (data is not null)
            {
                var d = Directory.CreateTempSubdirectory("modscan-").FullName;
                temps.Add(d);
                File.WriteAllBytes(path = Path.Combine(d, "nested" + ext), data);
            }
            var (dir, files, unknown) = Extract(path!);
            if (unknown is not null) { p.Unknown.Add($"{lab}: {unknown}"); return; }
            temps.Add(dir!);
            foreach (var f in files) Member(p, f, Path.Combine(dir!, f), null, lab, rel, dirs, depth);
        }

        if (o.Bin is not null)
            // (not the launcher's backup folder, whose restore points keep old plugins the game never loads)
            foreach (var f in Walk(o.Bin).Where(n => n.EndsWith(".asi", StringComparison.OrdinalIgnoreCase)
                         && !n.StartsWith(Manifest.DirOf(o.Bin) + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase)))
            {
                var p = new Party(Path.GetFileNameWithoutExtension(f), $"Bin/{Slash(Path.GetRelativePath(o.Bin, f))}", "");
                p.Mod = p.File;
                parties.Add(p);
                DoAsi(p, File.ReadAllBytes(f), f, f);
            }

        // (1) resource keys: the likely winner by load order; the same meaning = duplicate; one archive or mod = alternatives
        var byKey = new Dictionary<string, List<(PkgRec Rec, Entry E)>>();
        foreach (var rec in recs)
            foreach (var e in rec.Pkg.Entries)
            {
                if (e.Type is TypeKey or TypeManifest) continue;
                if (!byKey.TryGetValue(e.Key, out var l)) byKey[e.Key] = l = new();
                l.Add((rec, e));
            }
        var pairs = new Dictionary<string, Shared>();
        foreach (var (k, l) in byKey)
        {
            if (l.Count < 2) continue;
            var sorted = l.Where(x => x.Rec.Key is not null).OrderBy(x => x.Rec.Key!.Value, Comparer<(int, int, string)>.Create(CompareKey)).ToList();
            var side = Sides(l);
            if (Adapted(l, sorted)) Array.Clear(side);
            var ps = l.Select(x => x.Rec.Party).Distinct().ToList();
            var id = string.Join(" | ", l.Select(x => x.Rec.Label).OrderBy(x => x, StringComparer.Ordinal));
            if (!pairs.TryGetValue(id, out var g))
                pairs[id] = g = new(ps, sorted.Select(x => x.Rec).Concat(l.Select(x => x.Rec)).Distinct().ToList(), sorted.FirstOrDefault().Rec?.Label ?? "-", OneMod(ps), l.Where(x => x.Rec.Key is null).Select(x => x.Rec.Label).ToList());
            var tn = TypeName(l[0].E.Type);
            g.All[tn] = g.All.GetValueOrDefault(tn) + 1;
            if (side.All(x => x == side[0])) { g.Same++; continue; }
            g.Types[tn] = g.Types.GetValueOrDefault(tn) + 1;
            if (!Blankable.Contains(l[0].E.Type) || l.Any(x => x.E.MemSize > TinyBytes)) g.Real++;
            if (g.Keys.Count < 5) g.Keys.Add($"{tn} {k}");
            // (who holds another copy than the one in use; with none loaded, than the first)
            var used = sorted.Count > 0 ? l.IndexOf(sorted[0]) : 0;
            for (var i = 0; i < l.Count; i++)
                if (side[i] != side[used]) g.Unlike.Add(l[i].Rec);
            if (l[0].E.Type == TypeStbl && g.Texts is not null)
                try
                {
                    var first = l[used].Rec.Pkg.Read(l[used].E);
                    foreach (var x in l.Where((_, i) => side[i] == i && side[i] != side[used])) g.Texts += TextsThatDiffer(first, x.Rec.Pkg.Read(x.E));
                }
                catch (Exception e) when (e is InvalidDataException or IOException or IndexOutOfRangeException or ArgumentException) { g.Texts = null; }
        }
        foreach (var (id, g) in pairs)
        {
            var differ = g.Types.Values.Sum();
            var total = differ + g.Same;
            var types = string.Join(", ", g.Types.Select(t => $"{t.Key} {t.Value}"));
            var nl = g.NotLoaded.Count > 0 ? $"; not loaded by Resource.cfg: {string.Join(", ", g.NotLoaded)}" : "";
            if (g.Alt) report.Info.Add(new(g.Parties[0].Name, $"{total} keys in several packages of one {(g.Parties.Count == 1 ? "archive" : "mod")} (versions or alternatives: install one): {id}{nl}"));
            else if (differ == 0) report.Info.Add(new(string.Join(" + ", g.Parties.Select(p => p.Name)), $"duplicate copy: {total} identical resources ({string.Join(", ", g.All.Select(t => $"{t.Key} {t.Value}"))}) in {id}"));
            else if (g.Real == 0) report.Info.Add(new(string.Join(" + ", g.Parties.Select(p => p.Name)), $"the same fix twice: {differ} keys replaced by small placeholders in each ({types}) in {id}"));
            else
            {
                var (kind, who) = Relation(g);
                var texts = g.Texts is > 0 and var t && g.Types.Count == 1 ? $"; {t} texts differ" : "";
                // (the others that hold the copy in use: they lose nothing there)
                var alike = g.Recs.Skip(1).Where(r => r.Key is not null && !g.Unlike.Contains(r)).Select(r => r.Label).ToList();
                report.Conflicts.Add(new(kind, $"{differ} shared keys with different content ({types}{(g.Same > 0 ? $"; {g.Same} identical" : "")}{texts}): {id}; likely winner: {g.Winner}{nl}"
                    + (alike.Count > 0 && g.Winner != "-" ? $"; same copy as the winner: {string.Join(" | ", alike)}" : ""), who.Select(p => p.Name).ToArray(), g.Keys.ToArray()));
            }
        }

        // (2) core mods, (3) assembly copies, (4) singletons, (5) ASI hook tables; parties all of one mod (ModFolders) = INFO
        void Add(Conflict c, List<Party> ps)
        {
            if (OneMod(ps)) report.Info.Add(new(ps[0].Mod, $"versions of one mod: {c.Text}"));
            else report.Conflicts.Add(c);
        }
        foreach (var grp in parties.SelectMany(p => p.Replaces.Select(r => (r, p))).GroupBy(x => x.r))
        {
            var ps = grp.Select(x => x.p).Distinct().ToList();
            if (ps.Count > 1) Add(new("core mod", $"{ps.Count} mods replace the game assembly {grp.Key}: only one copy loads, the others' changes are lost", ps.Select(p => p.Name).ToArray(), Array.Empty<string>()), ps);
        }
        foreach (var grp in parties.SelectMany(p => p.Asms.Where(a => !GameModules.IsMatch(a.Name)).Select(a => (a, p))).GroupBy(x => x.a.Name))
        {
            var ps = grp.Select(x => x.p).Distinct().ToList();
            if (ps.Count < 2) continue;
            var mv = grp.Select(x => x.a.Mvid).Distinct().ToList();
            if (mv.Count > 1) Add(new("assembly", $"script assembly {grp.Key} in {ps.Count} mods with {mv.Count} different builds: old or duplicate copies, keep one",
                ps.Select(p => p.Name).ToArray(), mv.Select(x => $"MVID {x}").ToArray()), ps);
            else report.Info.Add(new(string.Join(" + ", ps.Select(p => p.Name)), $"duplicate copy: script assembly {grp.Key} (same build) in {ps.Count} mods"));
        }
        // the same assembly build stored twice (a duplicate copy, reported above) is no interaction conflict
        foreach (var grp in parties.SelectMany(p => p.Singletons.Select(s => (s.Key, s.Value, p))).GroupBy(x => x.Key))
        {
            var ps = grp.Select(x => x.p).ToList();
            if (ps.Count > 1 && grp.SelectMany(x => x.Value).Distinct().Count() > 1) Add(new("interaction", $"{ps.Count} mods replace {grp.Key[..^"::Singleton".Length]} (store its Singleton): the last one to run wins",
                ps.Select(p => p.Name).ToArray(), new[] { grp.Key }), ps);
        }
        var tabled = parties.Where(p => p.Addrs is not null && p.KnownTable is not null).ToList();
        for (var i = 0; i < tabled.Count; i++)
            for (var j = i + 1; j < tabled.Count; j++)
            {
                var (a, b) = (tabled[i], tabled[j]);
                var all = a.Addrs!.Where(b.Addrs!.Contains).OrderBy(x => x).ToList();
                // coordinated (hook table): one side stands down or chains there; INFO per "<who> stands down|chains"
                string? Co(uint x) => (b.HookMod is not null ? a.Coordinated?.GetValueOrDefault(x)?.GetValueOrDefault(b.HookMod) : null)
                                     ?? (a.HookMod is not null ? b.Coordinated?.GetValueOrDefault(x)?.GetValueOrDefault(a.HookMod) : null);
                foreach (var g in all.Where(x => Co(x) is not null).GroupBy(x => Co(x)!))
                    report.Info.Add(new($"{a.Name} + {b.Name}", $"both hook {string.Join(", ", g.Select(x => $"0x{x:X}"))}; {g.Key} (coordinated)"));
                var shared = all.Where(x => Co(x) is null).ToList();
                if (shared.Count > 0) report.Conflicts.Add(new("native hook",
                    $"{a.Name} and {b.Name} patch {shared.Count} of the same TS3W.exe addresses ({a.KnownTable} / {b.KnownTable}): install order decides which patch survives",
                    new[] { a.Name, b.Name }, shared.Take(8).Select(x => $"0x{x:X}" + (a.Purposes?.GetValueOrDefault(x) is { } pa && b.Purposes?.GetValueOrDefault(x) is { } pb ? $" ({pa} / {pb})" : "")).ToArray()));
            }
        foreach (var p in parties.Where(p => p.Addrs is not null && p.KnownTable is null))
            foreach (var t in tabled)
                if (p.Addrs!.Count(t.Addrs!.Contains) is var n and > 0)
                    report.Unknown.Add(new(p.Name, $"{n} code-address immediates equal hook sites of {t.Name} (possible shared hook, not proven)"));

        // WARNING per area: 2+ mods with write evidence
        var mods = parties.GroupBy(p => p.Mod).Select(grp =>
        {
            var w = new Dictionary<string, Dictionary<string, int>>();
            foreach (var p in grp)
                foreach (var (a, m) in p.Writes)
                {
                    if (!w.TryGetValue(a, out var t)) w[a] = t = new();
                    foreach (var (c, n) in m) t[c] = t.GetValueOrDefault(c) + n;
                }
            return (Name: grp.Count() > 1 || o.ModFolders ? grp.Key : grp.First().Name, Writes: w);
        }).ToList();
        foreach (var a in R.Areas)
        {
            var ps = mods.Where(p => p.Writes.TryGetValue(a.Id, out var m) && m.Count > 0).ToList();
            if (ps.Count < 2) continue;
            report.Warnings.Add(new(a.Id, a.Label, a.Risk, ps.Select(p =>
            {
                var m = p.Writes[a.Id].OrderByDescending(x => x.Value).ToList();
                return new WarningParty(p.Name, m.Take(6).Select(x => $"{x.Key} x{x.Value}").Concat(m.Count > 6 ? new[] { $"+{m.Count - 6} more" } : Array.Empty<string>()).ToArray());
            }).ToArray()));
        }
        foreach (var p in parties)
        {
            foreach (var t in p.Info) report.Info.Add(new(p.Name, t));
            foreach (var t in p.Unknown) report.Unknown.Add(new(p.Name, t));
        }
        report.NeedsSevenZip = report.Unknown.Any(u => u.Text.Contains(SevenZipInstall));
        var s = report.Summary;
        s["files"] = parties.Count; s["packages"] = recs.Count; s["assemblies"] = parties.Sum(p => p.Asms.Count); s["asi"] = parties.Count(p => p.Kind.Contains("asi"));
        s["gameAssemblies"] = game.Count; s["eventNames"] = eventNames.Count;
        s["conflicts"] = report.Conflicts.Count; s["warnings"] = report.Warnings.Count; s["info"] = report.Info.Count; s["unknown"] = report.Unknown.Count;
        return report;
    }

    public static string Render(Report r)
    {
        var o = new List<string> { "mod-vs-mod scan: " + string.Join(", ", r.Summary.Select(x => $"{x.Key} {x.Value}")), "", $"CONFLICT ({r.Conflicts.Count})" };
        foreach (var c in r.Conflicts) o.Add($"- [{c.Kind}] {string.Join(" vs ", c.Parties)}: {c.Text}" + (c.Evidence.Length > 0 ? $"\n    evidence: {string.Join("; ", c.Evidence)}" : ""));
        o.Add(""); o.Add($"WARNING ({r.Warnings.Count}) possible behaviour overlap, not proven");
        foreach (var w in r.Warnings)
        {
            o.Add($"- {w.Label}: {w.Parties.Length} mods. {w.Risk}");
            foreach (var p in w.Parties) o.Add($"    {p.Name}: {string.Join("; ", p.Calls)}");
        }
        o.Add(""); o.Add($"INFO ({r.Info.Count})");
        foreach (var i in r.Info) o.Add($"- {i.Party}: {i.Text}");
        o.Add(""); o.Add($"unknown ({r.Unknown.Count})");
        foreach (var u in r.Unknown) o.Add($"- {u.Party}: {u.Text}");
        o.Add(""); o.Add("Load order: \"likely winner\" follows Resource.cfg priority and order, then file name order inside one folder pattern; the name order is not verified in game.");
        return string.Join("\n", o);
    }
}
