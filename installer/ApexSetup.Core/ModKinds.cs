using System.Buffers.Binary;
using System.Collections.Immutable;
using System.Reflection.PortableExecutable;
using System.Text;

namespace ApexSetup;

// The Mods screen's filter groups
public enum KindGroup { Gameplay, CreateASim, BuyMode, BuildMode, LookAndSound, Other }

// Every kind a package can hold. Buy and Build kinds carry the game's own category names (s3pi ObjectCatalogResource
// FunctionCategory and BuildCategory enums); the order of the members follows the groups (ModKinds.GroupOf).
public enum Kind
{
    Script, CoreMod, Tuning, Skill, Buff,
    Hair, Clothing, Accessories, Makeup, FacialHair, Eyebrows, Eyes, SkinDetails, Tattoos, SkinTones, Sliders, Pets, CasOther,
    BuyAppliances, BuyElectronics, BuyEntertainment, BuyLighting, BuyPlumbing, BuyDecor, BuyKids, BuyStorage, BuyComfort,
    BuySurfaces, BuyVehicles, BuyPets, BuyShowStage, BuyResort, BuyDebug, BuyMisc, BuyModular,
    BuildDoor, BuildWindow, BuildGate, BuildColumn, BuildRabbitHole, BuildFireplace, BuildChimney, BuildArch, BuildFlower,
    BuildShrub, BuildTree, BuildRug, BuildRock, BuildShell, BuildLandmark, BuildElevator, BuildSpiralStaircase,
    BuildBlueprint, BuildResortObjects, BuildModularArch,
    BuildWall, BuildWallPattern, BuildFloorPattern, BuildFence, BuildRailing, BuildStairs, BuildRoofStyle, BuildRoofPattern,
    BuildFoundation, BuildPool, BuildTerrainPaint, BuildTerrainTool, BuildWater,
    Lighting, Effects, Patterns, Animations, Sound, Interface, Textures, Translations,
    Other,
}

// One catalog resource of the package (an object, a wall pattern, a fence...): its STBL name and description keys, its
// catalog thumbnail instance (Common.pngInstance) and the kinds it gives the package.
public sealed record CatalogItem(uint Type, ulong Instance, ulong NameKey, ulong DescKey, ulong PngInstance, IReadOnlyList<Kind> Kinds);

// What one package holds. Counts: resources per kind (an object with two categories counts in both); Primary: the
// single kind Mods.KindOf gives it; Assemblies: the script assemblies' names.
public sealed record ModKindReport(ModKind Primary, IReadOnlyDictionary<Kind, int> Counts, IReadOnlyList<CatalogItem> Items, IReadOnlyList<string> Assemblies)
{
    public IEnumerable<Kind> Kinds => Counts.Keys;
    public IEnumerable<KindGroup> Groups => Counts.Keys.Select(ModKinds.GroupOf).Distinct();
    public bool Has(Kind k) => Counts.ContainsKey(k);
}

// The fields of an OBJD up to its build category (s3pi ObjectCatalogResource.Parse)
public sealed record Objd(uint Version, string? InstanceName, ulong NameKey, ulong DescKey, string Name, string Desc, ulong PngInstance,
    uint ObjectType, uint RoomCategory, uint FunctionCategory, ulong FunctionSubCategory, ulong FunctionSubCategory2, ulong RoomSubCategory, uint BuildCategory);

// Formats from the community's documentation of The Sims 3 (s3pi wrappers, simswiki "Files By Type Code"). Every read
// is bounded: bad data makes one resource unreadable, never an exception out of Read.
public static class ModKinds
{
    public const uint Objd = 0x319E4F1D, Casp = 0x034AEECB, S3sa = 0x073FAA07, Stbl = 0x220557DA, Nmap = 0x0166038C, Manifest = 0x73E93EEB,
        Cwst = 0x9151E6BC, Cwal = 0x515CA4CD, Cfen = 0x0418FE2A, Cral = 0x04C58103, Cstr = 0x049CA4CD, Crst = 0x91EDBD3E, Crmt = 0xF1EDBD86,
        Cfnd = 0x316C78F2, Cfir = 0x04F3CC01, Ccfp = 0x0A36F07A, Ctpt = 0x04ED4BB2, Cttl = 0x04B30669, Cwat = 0x060B390C, Mdlr = 0xCF9A4ACE,
        Img = 0x00B2D882, Bgeo = 0x067CAA11, Lite = 0x03B4C61D, LiteIni = 0x1F886EAD, UiPng = 0x2F7D0004, UiTga = 0x2F7D0002;
    // object thumbnails small, medium, large, very large (simswiki ICON 0x2E75C764..67)
    public static readonly uint[] IconTypes = [0x2E75C766, 0x2E75C767, 0x2E75C765, 0x2E75C764];

    // the game's own script assemblies, as read from the S3SA resources of Game\Bin's gameplay.package, scripts.package
    // and simcore.package (1.67): a mod assembly of one of these names replaces it
    public static readonly HashSet<string> GameAssemblies = new(StringComparer.OrdinalIgnoreCase)
    {
        "mscorlib", "System", "System.Xml", "ScriptCore", "SimIFace", "Sims3Metadata", "Sims3GameplaySystems", "Sims3GameplayObjects",
        "Sims3StoreObjects", "UI",
    };

    const int MaxParsed = 5000, MaxItems = 500, MaxList = 4096;

    // Resource types that are a kind by themselves (simswiki Files By Type Code). Unverified on real mods (none of the
    // tested packages holds them): PTRN patterns, PETB breeds, TONE skin tones, FBLN sliders, CFND.
    static readonly (uint Type, Kind Kind)[] Direct =
    [
        (0x0333406C, Kind.Tuning), (0x03B33DDF, Kind.Tuning), (0xA8D58BE5, Kind.Skill), (0xDD3223A7, Kind.Buff),
        (0x0354796A, Kind.SkinTones), (0xB52F5055, Kind.Sliders), (0xDEA2951C, Kind.Pets),
        (Mdlr, Kind.BuyModular),
        (Cwst, Kind.BuildWall), (Cfen, Kind.BuildFence), (Cral, Kind.BuildRailing), (Cstr, Kind.BuildStairs), (Crst, Kind.BuildRoofStyle),
        (Crmt, Kind.BuildRoofPattern), (Cfnd, Kind.BuildFoundation), (Cfir, Kind.BuildFireplace), (Ccfp, Kind.BuildPool),
        (Ctpt, Kind.BuildTerrainPaint), (Cttl, Kind.BuildTerrainTool), (Cwat, Kind.BuildWater),
        (0xEA5118B0, Kind.Effects), (0xD4D9FBE5, Kind.Patterns),
        (0x6B20C4F3, Kind.Animations), (0x02D5DF13, Kind.Animations),
        (0x01A527DB, Kind.Sound), (0x01EEF63A, Kind.Sound), (0x8070223D, Kind.Sound),
        (0x025C95B6, Kind.Interface), (0x025C90A6, Kind.Interface),
    ];

    // FunctionCategory and BuildCategory bit -> kind (s3pi ObjectCatalogResource enums; bit 31 is Default, not a category)
    static readonly Dictionary<int, Kind> FunctionBits = new()
    {
        [1] = Kind.BuyAppliances, [2] = Kind.BuyElectronics, [3] = Kind.BuyEntertainment, [5] = Kind.BuyLighting, [6] = Kind.BuyPlumbing,
        [7] = Kind.BuyDecor, [8] = Kind.BuyKids, [9] = Kind.BuyStorage, [11] = Kind.BuyComfort, [12] = Kind.BuySurfaces,
        [13] = Kind.BuyVehicles, [14] = Kind.BuyPets, [15] = Kind.BuyShowStage, [16] = Kind.BuyResort, [30] = Kind.BuyDebug,
    };
    static readonly Dictionary<int, Kind> BuildBits = new()
    {
        [1] = Kind.BuildDoor, [2] = Kind.BuildWindow, [3] = Kind.BuildGate, [4] = Kind.BuildColumn, [5] = Kind.BuildRabbitHole,
        [6] = Kind.BuildFireplace, [7] = Kind.BuildChimney, [8] = Kind.BuildArch, [9] = Kind.BuildFlower, [10] = Kind.BuildShrub,
        [11] = Kind.BuildTree, [12] = Kind.BuildRug, [13] = Kind.BuildRock, [14] = Kind.BuildShell, [15] = Kind.BuildLandmark,
        [16] = Kind.BuildElevator, [17] = Kind.BuildSpiralStaircase, [28] = Kind.BuildBlueprint, [29] = Kind.BuildResortObjects,
        [30] = Kind.BuildModularArch,
    };

    public static KindGroup GroupOf(Kind k) => k switch
    {
        <= Kind.Buff => KindGroup.Gameplay,
        <= Kind.CasOther => KindGroup.CreateASim,
        <= Kind.BuyModular => KindGroup.BuyMode,
        <= Kind.BuildWater => KindGroup.BuildMode,
        <= Kind.Translations => KindGroup.LookAndSound,
        _ => KindGroup.Other,
    };

    public static ModKindReport Read(string path)
    {
        var index = Dbpf.Index(path);
        var primary = Mods.KindOf(index.GroupBy(e => e.Type).ToDictionary(x => x.Key, x => x.Count()));
        var counts = new Dictionary<Kind, int>();
        var items = new List<CatalogItem>();
        var assemblies = new List<string>();
        void Add(Kind k, int n = 1) { if (n > 0) counts[k] = counts.GetValueOrDefault(k) + n; }

        var direct = Direct.ToLookup(x => x.Type, x => x.Kind);
        foreach (var e in index)
            foreach (var k in direct[e.Type]) Add(k);
        try
        {
            using var f = File.OpenRead(path);
            var parsed = 0;
            foreach (var e in index)
            {
                if (e.Type is not (Objd or Casp or S3sa or Cwal)) continue;
                var d = ++parsed <= MaxParsed ? Dbpf.Read(f, e, 4 << 20) : null;
                switch (e.Type)
                {
                    case S3sa:
                        Add(Kind.Script);
                        if (d is not null && AssemblyName(d) is { } name)
                        {
                            assemblies.Add(name);
                            if (GameAssemblies.Contains(name)) Add(Kind.CoreMod);
                        }
                        break;
                    case Casp:
                        Add(d is not null && ModContent.Part(d) is { } p ? CasKind(p.Clothing, p.AgeGender) : Kind.CasOther);
                        break;
                    default:
                        var item = d is null ? null : Catalog(e.Type, e.Instance, d);
                        // (an unreadable object is still an object; an unreadable pattern still a wall or floor pattern)
                        if (item is null) { Add(e.Type == Objd ? Kind.BuyMisc : Kind.BuildWallPattern); break; }
                        foreach (var k in item.Kinds) Add(k);
                        if (items.Count < MaxItems) items.Add(item);
                        break;
                }
            }
        }
        catch (Exception x) when (x is IOException or UnauthorizedAccessException) { }

        // catalog types besides OBJD/CWAL: their names and thumbnails, for the details pane
        // (shortcut: only the first MaxItems items are kept, enough for the Mods screen's list)
        if (items.Count < MaxItems)
            try
            {
                using var f = File.OpenRead(path);
                foreach (var e in index.Where(e => e.Type != Cwal && CommonAt.ContainsKey(e.Type)).Take(MaxItems - items.Count))
                    if (Dbpf.Read(f, e, 1 << 20) is { } d && Catalog(e.Type, e.Instance, d) is { } item) items.Add(item);
            }
            catch (Exception x) when (x is IOException or UnauthorizedAccessException) { }

        // blend geometry is a slider's on its own, a hair's or beard's morphs inside a CAS package (Cabelos.package,
        // Barbas.package); a light (LITE, _INI) is a lighting mod's on its own, an object's own lamp beside an OBJD
        if (!index.Any(e => e.Type == Casp)) Add(Kind.Sliders, index.Count(e => e.Type == Bgeo));
        if (!index.Any(e => e.Type == Objd)) Add(Kind.Lighting, index.Count(e => e.Type is Lite or LiteIni));

        // what most of the rest is: textures (default replacements), UI images, or only strings (a translation)
        var total = index.Count(e => e.Type is not (Stbl or Nmap or Manifest));
        if (total == 0 && index.Any(e => e.Type == Stbl)) Add(Kind.Translations, index.Count(e => e.Type == Stbl));
        else if (!counts.Keys.Any(k => GroupOf(k) is KindGroup.CreateASim or KindGroup.BuyMode or KindGroup.BuildMode))
        {
            var img = index.Count(e => e.Type == Img);
            var ui = index.Count(e => e.Type is UiPng or UiTga);
            if (img * 2 >= total) Add(Kind.Textures, img);
            else if (ui * 2 >= total) Add(Kind.Interface, ui);
        }
        if (counts.Count == 0) Add(Kind.Other, Math.Max(1, index.Count));
        return new ModKindReport(primary, counts, items, assemblies);
    }

    // a CAS part's kind from its clothing type and species (s3pi CASPartFlags ClothingType, SpeciesType)
    public static Kind CasKind(uint clothing, uint ageGender)
    {
        var species = ageGender >> 8 & 0xCF;
        if (species > 1 || clothing is >= 0x2F and <= 0x34) return Kind.Pets;
        return clothing switch
        {
            0x01 or 0x02 => Kind.Hair,
            0x04 or 0x05 or 0x06 or 0x07 => Kind.Clothing,
            >= 0x08 and <= 0x0F or 0x18 or 0x19 or >= 0x1E and <= 0x20 or 0x23 or 0x24 or 0x25 => Kind.Accessories,
            0x10 => Kind.FacialHair,
            >= 0x11 and <= 0x15 or 0x1A => Kind.Makeup,
            0x16 => Kind.Eyebrows,
            0x17 => Kind.Eyes,
            0x03 or 0x1B or 0x1C or 0x1D or 0x26 or >= 0x27 and <= 0x2E => Kind.SkinDetails,
            0x21 or 0x22 => Kind.Tattoos,
            _ => Kind.CasOther,
        };
    }

    // the S3SA's assembly name (header layout and XOR scheme: s3pi ScriptResource, as ModConflicts reads it)
    public static string? AssemblyName(byte[] s3sa)
    {
        try
        {
            if ((ModConflicts.PeImage(s3sa) ?? ModConflicts.PeImage(ModConflicts.DecryptS3sa(s3sa))) is not { } img) return null;
            using var pe = new PEReader(ImmutableArray.Create(img));
            if (!pe.HasMetadata) return null;
            var md = System.Reflection.Metadata.PEReaderExtensions.GetMetadataReader(pe);
            return md.IsAssembly ? md.GetString(md.GetAssemblyDefinition().Name) : null;
        }
        catch (Exception e) when (e is ArgumentException or InvalidDataException or BadImageFormatException or IndexOutOfRangeException or InvalidOperationException) { return null; }
    }

    // the catalog thumbnail of an item in the package: a THUM (0x0580A2B4..B6) of the item's own instance, as EA's
    // Thumbnails\AllThumbnails.package keys 7068 of the game's 7724 OBJDs, else an ICON of its pngInstance (244 of them);
    // an object that overrides one of EA's has its thumbnail there, not in the mod
    public static byte[]? Thumbnail(string path, CatalogItem item)
    {
        var index = Dbpf.Index(path);
        foreach (var (types, inst) in new[] { (ThumbTypes, item.Instance), (IconTypes, item.PngInstance) })
            foreach (var t in types)
                if (inst != 0 && index.FirstOrDefault(e => e.Type == t && e.Instance == inst) is { Size: > 0 } e) return Dbpf.Read(path, e);
        return null;
    }
    public static readonly uint[] ThumbTypes = [0x0580A2B5, 0x0580A2B6, 0x0580A2B4];

    // ---------- catalog resources ----------

    // a bounds-checked little-endian reader: any read past the end throws InvalidDataException, caught by its caller
    ref struct R
    {
        readonly ReadOnlySpan<byte> _d;
        public int At;
        public R(ReadOnlySpan<byte> d) { _d = d; At = 0; }
        public int Length => _d.Length;
        ReadOnlySpan<byte> Take(int n)
        {
            if (n < 0 || n > _d.Length - At) throw new InvalidDataException("catalog resource truncated");
            var s = _d.Slice(At, n);
            At += n;
            return s;
        }
        public byte U8() => Take(1)[0];
        public uint U32() => BinaryPrimitives.ReadUInt32LittleEndian(Take(4));
        public ulong U64() => BinaryPrimitives.ReadUInt64LittleEndian(Take(8));
        public void Skip(int n) => Take(n);
        // s3pi BigEndianUnicodeString: a 7-bit encoded byte length, then UTF-16 big endian
        public string Str()
        {
            int n = 0, shift = 0;
            byte b;
            do { b = U8(); n |= (b & 0x7F) << shift; shift += 7; } while ((b & 0x80) != 0 && shift < 35);
            return Encoding.BigEndianUnicode.GetString(Take(n));
        }
        public int Count(int min)
        {
            var n = U32();
            if (n > MaxList || (long)n * min > _d.Length - At) throw new InvalidDataException("catalog list count");
            return (int)n;
        }
    }

    // what comes between a catalog resource's version and its Common block, per type (s3pi CatalogResource wrappers):
    // the TGI list offset and size (CatalogResourceTGIBlockList), a material list (from a version on), a uint32
    // (CWST unknown2), nothing (CFIR, CRMT, CRST, CCFP) or the brush version (CTTL, CWAT, CTPT: no TGI list header).
    // CFND is left out: its fields before Common depend on its foundation type.
    enum Pre { None, MaterialsFrom3, MaterialsFrom7, Uint, Brush, CwalMaterials }
    static readonly Dictionary<uint, Pre> CommonAt = new()
    {
        [Cwal] = Pre.CwalMaterials, [Cfen] = Pre.MaterialsFrom7, [Cral] = Pre.MaterialsFrom3, [Cstr] = Pre.MaterialsFrom3,
        [Cwst] = Pre.Uint, [Cfir] = Pre.None, [Crmt] = Pre.None, [Crst] = Pre.None, [Ccfp] = Pre.None,
        [Cttl] = Pre.Brush, [Cwat] = Pre.Brush, [Ctpt] = Pre.Brush,
    };

    // one catalog resource as an item, or null when unreadable. OBJD: by its categories; CWAL: wall or floor pattern.
    public static CatalogItem? Catalog(uint type, ulong instance, ReadOnlySpan<byte> d)
    {
        try
        {
            if (type == Objd) return ParseObjd(d) is { } o ? new CatalogItem(type, instance, o.NameKey, o.DescKey, o.PngInstance, KindsOf(o)) : null;
            if (!CommonAt.TryGetValue(type, out var pre)) return null;
            var r = new R(d);
            var version = r.U32();
            if (pre != Pre.Brush) r.Skip(8);
            switch (pre)
            {
                case Pre.CwalMaterials: Materials(ref r, 12); break;
                case Pre.MaterialsFrom3 when version >= 3: Materials(ref r, 0); break;
                case Pre.MaterialsFrom7 when version >= 7: Materials(ref r, 0); break;
                case Pre.Uint: case Pre.Brush: r.Skip(4); break;
            }
            var (name, desc, _, _, png) = Common(ref r, full: type == Cwal);
            var kind = type switch
            {
                // (s3pi Pattern: 1 floor, 2 wall, 3 ceiling (as a wall pattern: unverified), 4 floor edge)
                Cwal => r.U32() switch { 1 or 4 => Kind.BuildFloorPattern, 2 or 3 => Kind.BuildWallPattern, _ => throw new InvalidDataException("pattern type") },
                _ => Direct.First(x => x.Type == type).Kind,
            };
            return new CatalogItem(type, instance, name, desc, png, [kind]);
        }
        catch (InvalidDataException) { return null; }
    }

    public static Objd? ParseObjd(ReadOnlySpan<byte> d)
    {
        try
        {
            var r = new R(d);
            var version = r.U32();
            // (s3pi knows versions up to 0x22; a later one could move the fields: unreadable, not guessed)
            if (version is 0 or > 0x22) return null;
            r.Skip(8); // (TGI list offset and size)
            Materials(ref r, 0);
            var instanceName = version >= 0x16 ? r.Str() : null;
            var (nameKey, descKey, name, desc, png) = Common(ref r, full: true);
            r.Skip(4); // objkIndex
            var objectType = r.U32();
            if (version >= 0x1A) r.Skip(4); // objectTypeFlags2
            r.Skip(4 * 4); // wallPlacement, movement, wallCutoutTilesPerLevel, levels
            r.Skip(r.U8() * 24); // WallCutoutList: byte count, 4 floats + 2 uint32 each
            r.Skip(1 + 4 + 4); // isScriptEnabled, diagonalIndex, ambienceTypeHash
            var room = r.U32();
            var function = r.U32();
            var functionSub = r.U64();
            var functionSub2 = version >= 0x1C ? r.U64() : 0;
            var roomSub = r.U64();
            var build = r.U32();
            return new Objd(version, instanceName, nameKey, descKey, name, desc, png, objectType, room, function, functionSub, functionSub2, roomSub, build);
        }
        catch (InvalidDataException) { return null; }
    }

    // an object is in Build mode by its build category bits, in Buy mode by its function category bits; both, if both
    public static IReadOnlyList<Kind> KindsOf(Objd o)
    {
        var kinds = new List<Kind>();
        for (var bit = 0; bit < 31; bit++)
        {
            if ((o.BuildCategory >> bit & 1) != 0 && BuildBits.TryGetValue(bit, out var b)) kinds.Add(b);
            if ((o.FunctionCategory >> bit & 1) != 0 && FunctionBits.TryGetValue(bit, out var f)) kinds.Add(f);
        }
        if (kinds.Count == 0) kinds.Add(Kind.BuyMisc);
        return kinds;
    }

    // s3pi CatalogResource.MaterialList: int32 count; each material a type byte, a uint32 unless type 1, then a uint32
    // offset (from after it) to its last uint32; extra: bytes after each material (CWAL: three uint32)
    static void Materials(ref R r, int extra)
    {
        var n = r.Count(9 + extra);
        for (var i = 0; i < n; i++)
        {
            if (r.U8() != 1) r.Skip(4);
            long offset = r.U32();
            var end = r.At + offset; // (from after the offset field, as s3pi reads it)
            if (end > r.Length - 4) throw new InvalidDataException("material offset");
            r.At = (int)end; // (forward only: a uint32 offset never goes back)
            r.Skip(4 + extra);
        }
    }

    // s3pi CatalogResource.Common: version, name and description STBL keys, name and description strings, price,
    // niceness, crap score, status byte, png instance; the rest is skipped only when a field follows (full)
    static (ulong Name, ulong Desc, string NameText, string DescText, ulong Png) Common(ref R r, bool full)
    {
        var version = r.U32();
        var name = r.U64();
        var desc = r.U64();
        var nameText = r.Str();
        var descText = r.Str();
        r.Skip(4 + 4 + 4 + 1);
        var png = r.U64();
        if (full)
        {
            r.Skip(1 + 4 + 4 + 1 + 1 + 4); // unknown7, environmentScore, fireType, isStealable, isReposessable, uiSortPriority
            if (version >= 0x0D) r.Skip(1);
            if (version >= 0x0E) r.Skip(1);
            if (version >= 0x0F) r.Skip(4);
        }
        return (name, desc, nameText, descText, png);
    }
}
