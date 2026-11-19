// Offline checks of the package kind reader (ApexSetup.Core/ModKinds.cs) on packages made here: OBJDs of several
// versions and categories, catalog resources, CAS parts, scripts, strings only, and damaged data.
using System.Reflection;
using System.Reflection.Metadata;
using System.Reflection.Metadata.Ecma335;
using System.Reflection.PortableExecutable;
using System.Text;
using ApexSetup;

static class ModKindsTests
{
    static void Str(BinaryWriter w, string s) { var b = Encoding.BigEndianUnicode.GetBytes(s); w.Write((byte)b.Length); w.Write(b); }

    // s3pi CatalogResource.MaterialList: count, then per material type 1 (no unknown1), the offset to its last uint32
    static void Materials(BinaryWriter w, int n, int extra = 0)
    {
        w.Write(n);
        for (var i = 0; i < n; i++) { w.Write((byte)1); w.Write(10u); w.Write(new byte[10]); w.Write(0u); w.Write(new byte[extra]); }
    }

    // s3pi CatalogResource.Common, all of it
    static void Common(BinaryWriter w, ulong name, ulong png, uint version = 0x0F)
    {
        w.Write(version); w.Write(name); w.Write(name + 1); Str(w, "name"); Str(w, "desc");
        w.Write(1f); w.Write(1f); w.Write(0f); w.Write((byte)0); w.Write(png);
        w.Write((byte)0); w.Write(0f); w.Write(0u); w.Write((byte)0); w.Write((byte)0); w.Write(0u);
        if (version >= 0x0D) w.Write((byte)0);
        if (version >= 0x0E) w.Write((byte)0);
        if (version >= 0x0F) w.Write(0u);
    }

    // an OBJD in s3pi ObjectCatalogResource's field order, up to the build category, then padding for what follows
    public static byte[] ObjdBytes(uint version, uint function, uint build, ulong name = 0x1111, ulong png = 0x77, int materials = 2, uint commonVersion = 0x0F)
    {
        var ms = new MemoryStream();
        var w = new BinaryWriter(ms);
        w.Write(version); w.Write(0u); w.Write(0u);
        Materials(w, materials);
        if (version >= 0x16) Str(w, "objectInstance");
        Common(w, name, png, commonVersion);
        w.Write(0u); w.Write(0u);
        if (version >= 0x1A) w.Write(0u);
        w.Write(new byte[16]);
        w.Write((byte)2); w.Write(new byte[48]); // two wall cutouts
        w.Write((byte)1); w.Write(0u); w.Write(0u);
        w.Write(0x2u); w.Write(function); w.Write(0xFFUL);
        if (version >= 0x1C) w.Write(0UL);
        w.Write(0UL); w.Write(build);
        w.Write(new byte[80]);
        return ms.ToArray();
    }

    static byte[] Cwal(uint pattern)
    {
        var ms = new MemoryStream();
        var w = new BinaryWriter(ms);
        w.Write(0x0Du); w.Write(0u); w.Write(0u);
        Materials(w, 1, 12);
        Common(w, 0x3333, 0x88);
        w.Write(pattern); w.Write(new byte[24]);
        return ms.ToArray();
    }

    static byte[] Cfen()
    {
        var ms = new MemoryStream();
        var w = new BinaryWriter(ms);
        w.Write(0x07u); w.Write(0u); w.Write(0u);
        Materials(w, 1);
        Common(w, 0x4444, 0x99, 0x0C);
        w.Write(new byte[40]);
        return ms.ToArray();
    }

    // a CAS part header (s3pi CASPartResource): version, offset, no presets, name, float, byte, clothing, data type, age/gender
    public static byte[] Casp(uint clothing, uint ageGender)
    {
        var ms = new MemoryStream();
        var w = new BinaryWriter(ms);
        w.Write(0x12u); w.Write(0); w.Write(0); Str(w, "part"); w.Write(0f); w.Write((byte)0);
        w.Write(clothing); w.Write(0u); w.Write(ageGender); w.Write(new byte[32]);
        return ms.ToArray();
    }

    static byte[] Asm(string name)
    {
        var mb = new MetadataBuilder();
        mb.AddModule(0, mb.GetOrAddString(name + ".dll"), mb.GetOrAddGuid(Guid.NewGuid()), default, default);
        mb.AddAssembly(mb.GetOrAddString(name), new Version(1, 0, 0, 0), default, default, 0, AssemblyHashAlgorithm.None);
        mb.AddTypeDefinition(0, default, mb.GetOrAddString("<Module>"), default, MetadataTokens.FieldDefinitionHandle(1), MetadataTokens.MethodDefinitionHandle(1));
        var o = new BlobBuilder();
        new ManagedPEBuilder(PEHeaderBuilder.CreateLibraryHeader(), new MetadataRootBuilder(mb), new BlobBuilder()).Serialize(o);
        return o.ToArray();
    }

    public static void Run(Action<bool, string> check)
    {
        var dir = Path.Combine(Path.GetTempPath(), "apex-modkinds-" + Guid.NewGuid().ToString("N"));
        Directory.CreateDirectory(dir);
        try { Checks(dir, check); }
        finally { try { Directory.Delete(dir, true); } catch (IOException) { } }
    }

    static void Checks(string dir, Action<bool, string> check)
    {
        var n = 0;
        string Pkg(params (uint Type, byte[] Data)[] r)
        {
            var p = Path.Combine(dir, $"{n++}.package");
            File.WriteAllBytes(p, ModConflicts.WritePackage(r.Select((x, i) => (x.Type, 0u, (ulong)(i + 1), x.Data)).ToList()));
            return p;
        }
        static string Show(ModKindReport r) => string.Join(" ", r.Counts.Select(x => $"{x.Key}={x.Value}"));

        // OBJD versions and categories
        var sofaPath = Pkg((ModKinds.Objd, ObjdBytes(0x15, 0x800, 0)));
        var sofa = ModKinds.Read(sofaPath);
        check(sofa.Counts.Count == 1 && sofa.Has(Kind.BuyComfort) && sofa.Groups.SequenceEqual([KindGroup.BuyMode])
            && sofa.Items is [{ NameKey: 0x1111, DescKey: 0x1112, PngInstance: 0x77, Instance: 1 }] && sofa.Primary == ModKind.Objects,
            "kinds: an OBJD of version 0x15 with the Comfort function category is Buy mode Comfort, with its name keys and thumbnail " + Show(sofa));
        var door = ModKinds.Read(Pkg((ModKinds.Objd, ObjdBytes(0x16, 0x80000000, 0x2))));
        check(door.Counts.Keys.SequenceEqual([Kind.BuildDoor]), "kinds: an OBJD of version 0x16 with the Door build category (function Default) is a door only " + Show(door));
        var lamps = ModKinds.Read(Pkg((ModKinds.Objd, ObjdBytes(0x1A, 0x20 | 0x80, 0)), (ModKinds.Objd, ObjdBytes(0x1C, 0x20, 0, commonVersion: 0x0C)),
            (ModKinds.Objd, ObjdBytes(0x22, 0x20, 0x800, materials: 0))));
        check(lamps.Counts.GetValueOrDefault(Kind.BuyLighting) == 3 && lamps.Counts.GetValueOrDefault(Kind.BuyDecor) == 1 && lamps.Counts.GetValueOrDefault(Kind.BuildTree) == 1
            && lamps.Items.Count == 3, "kinds: OBJDs of versions 0x1A, 0x1C and 0x22 count in every category they hold " + Show(lamps));
        var plain = ModKinds.Read(Pkg((ModKinds.Objd, ObjdBytes(0x16, 0, 0))));
        check(plain.Counts.Keys.SequenceEqual([Kind.BuyMisc]), "kinds: an OBJD with no category is Buy mode misc");
        var o = ModKinds.ParseObjd(ObjdBytes(0x1C, 0x40, 0x10));
        check(o is { Version: 0x1C, InstanceName: "objectInstance", Name: "name", FunctionCategory: 0x40, FunctionSubCategory: 0xFF, RoomCategory: 2, BuildCategory: 0x10 },
            "kinds: the OBJD fields read back as written");

        // catalog resources
        var patterns = ModKinds.Read(Pkg((ModKinds.Cwal, Cwal(1)), (ModKinds.Cwal, Cwal(2)), (ModKinds.Cwal, Cwal(4)), (ModKinds.Cfen, Cfen()), (ModKinds.Mdlr, [1])));
        check(patterns.Counts.GetValueOrDefault(Kind.BuildFloorPattern) == 2 && patterns.Counts.GetValueOrDefault(Kind.BuildWallPattern) == 1
            && patterns.Has(Kind.BuildFence) && patterns.Has(Kind.BuyModular) && patterns.Items.Any(i => i.Type == ModKinds.Cfen && i.NameKey == 0x4444 && i.PngInstance == 0x99),
            "kinds: wall and floor patterns apart, a fence with its name key, a modular object " + Show(patterns));

        // Create a Sim
        var casPath = Pkg((ModKinds.Casp, Casp(0x01, 0x0120)), (ModKinds.Casp, Casp(0x05, 0x0130)), (ModKinds.Casp, Casp(0x11, 0x0120)),
            (ModKinds.Casp, Casp(0x01, 0x0320)), (ModKinds.Casp, Casp(0x30, 0x0420)), (0x0354796A, [1]), (0xB52F5055, [1]));
        var cas = ModKinds.Read(casPath);
        check(cas.Has(Kind.Hair) && cas.Has(Kind.Clothing) && cas.Has(Kind.Makeup) && cas.Counts.GetValueOrDefault(Kind.Pets) == 2
            && cas.Has(Kind.SkinTones) && cas.Has(Kind.Sliders) && cas.Groups.SequenceEqual([KindGroup.CreateASim]) && cas.Primary == ModKind.Cas,
            "kinds: hair, clothing, makeup, a cat's hair and a dog's tail as pets, skin tones and sliders " + Show(cas));

        // scripts: a mod's own assembly, and one named as a game assembly
        var scriptPath = Pkg((ModKinds.S3sa, ModConflicts.S3saWrap(Asm("MyMod"))), (0x0333406C, [1]));
        var script = ModKinds.Read(scriptPath);
        var core = ModKinds.Read(Pkg((ModKinds.S3sa, ModConflicts.S3saWrap(Asm("Sims3GameplaySystems")))));
        check(script.Counts.Keys.Order().SequenceEqual([Kind.Script, Kind.Tuning]) && script.Assemblies.SequenceEqual(["MyMod"]) && script.Primary == ModKind.Script
            && core.Has(Kind.CoreMod) && core.Has(Kind.Script), "kinds: a script mod with tuning; a core mod by its assembly's name " + Show(script) + " / " + Show(core));

        // look and sound, translations, nothing known
        var stbl = ModKinds.Read(Pkg((ModKinds.Stbl, [1]), (ModKinds.Stbl, [1]), (ModKinds.Nmap, [1])));
        check(stbl.Counts.Keys.SequenceEqual([Kind.Translations]) && stbl.Counts[Kind.Translations] == 2, "kinds: only strings is a translation " + Show(stbl));
        var texPath = Pkg((ModKinds.Img, [1]), (ModKinds.Img, [1]), (0x033A1435, [1]));
        var tex = ModKinds.Read(texPath);
        check(tex.Counts.Keys.SequenceEqual([Kind.Textures]) && tex.Primary == ModKind.Textures, "kinds: mostly images is textures " + Show(tex));
        var look = ModKinds.Read(Pkg((0x03B4C61D, [1]), (0xEA5118B0, [1]), (0x6B20C4F3, [1]), (0x01A527DB, [1]), (0x025C95B6, [1])));
        check(new[] { Kind.Lighting, Kind.Effects, Kind.Animations, Kind.Sound, Kind.Interface }.All(look.Has) && look.Groups.SequenceEqual([KindGroup.LookAndSound]),
            "kinds: lighting, effects, animations, sound and interface " + Show(look));
        var other = ModKinds.Read(Pkg((0x12345678, [1])));
        check(other.Counts.Keys.SequenceEqual([Kind.Other]) && ModKinds.GroupOf(Kind.Other) == KindGroup.Other, "kinds: an unknown resource is Other");
        foreach (var p in new[] { sofaPath, casPath, scriptPath, texPath })
            check(ModKinds.Read(p).Primary == Mods.KindOf(p), "kinds: the primary kind is Mods.KindOf's for " + Path.GetFileName(p));

        // damaged data: every truncation, huge counts, unknown versions, random bytes; never an exception, never a hang
        var good = ObjdBytes(0x1C, 0x20, 0);
        var threw = "";
        var cut = 0;
        for (var len = 0; len < good.Length; len++)
            try { if (ModKinds.ParseObjd(good.AsSpan(0, len)) is null) cut++; } catch (Exception e) { threw = e.GetType().Name; }
        var huge = (byte[])good.Clone();
        BitConverter.GetBytes(int.MaxValue).CopyTo(huge, 12);
        var far = (byte[])good.Clone();
        BitConverter.GetBytes(uint.MaxValue).CopyTo(far, 17); // the first material's offset
        var newer = ObjdBytes(0x99, 0x20, 0);
        check(threw == "" && cut >= good.Length - 80 && ModKinds.ParseObjd(huge) is null && ModKinds.ParseObjd(far) is null && ModKinds.ParseObjd(newer) is null,
            $"kinds: a truncated OBJD, a huge material count, a far material offset and an unknown version are unreadable, not thrown ({threw})");
        var rng = new Random(7);
        var types = new[] { ModKinds.Objd, ModKinds.Cwal, ModKinds.Cfen, ModKinds.Cwst, ModKinds.Ctpt, ModKinds.Cstr, ModKinds.Crst };
        threw = "";
        for (var i = 0; i < 3000; i++)
        {
            var b = new byte[rng.Next(0, 400)];
            rng.NextBytes(b);
            if (i % 2 == 0 && b.Length > 4) BitConverter.GetBytes((uint)rng.Next(0, 0x23)).CopyTo(b, 0);
            try { ModKinds.Catalog(types[i % types.Length], 1, b); ModContent.Part(b); } catch (Exception e) { threw = e.GetType().Name; }
        }
        check(threw == "", "kinds: random bytes as catalog resources and CAS parts never throw " + threw);
        var bad = Pkg((ModKinds.Objd, newer), (ModKinds.Objd, good[..40]), (ModKinds.Cwal, [1, 2, 3]), (ModKinds.Casp, [9]), (ModKinds.S3sa, [1, 2]));
        var task = Task.Run(() => ModKinds.Read(bad));
        check(task.Wait(TimeSpan.FromSeconds(20)) && task.Result.Counts.GetValueOrDefault(Kind.BuyMisc) == 2 && task.Result.Has(Kind.Script)
            && task.Result.Has(Kind.CasOther) && task.Result.Items.Count == 0,
            "kinds: a package of damaged resources is read: unreadable objects as Buy misc, no items");
        var junk = Path.Combine(dir, "junk.package");
        File.WriteAllBytes(junk, Encoding.ASCII.GetBytes("DBPF").Concat(new byte[200]).ToArray());
        var missing = Path.Combine(dir, "missing.package");
        check(ModKinds.Read(junk).Counts.Keys.SequenceEqual([Kind.Other]) && ModKinds.Read(missing).Counts.Keys.SequenceEqual([Kind.Other]),
            "kinds: a damaged or missing file is Other");
    }
}
