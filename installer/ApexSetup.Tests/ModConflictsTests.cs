// Offline checks of the mod-vs-mod conflict scan (ApexSetup.Core/ModConflicts.cs) on packages and assemblies made here:
// no game files, no third-party mods. The stub assemblies only name game types; they hold no game code.
using System.Diagnostics;
using System.IO.Compression;
using System.Reflection;
using System.Reflection.Metadata;
using System.Reflection.Metadata.Ecma335;
using System.Reflection.PortableExecutable;
using System.Text;
using System.Text.RegularExpressions;
using ApexSetup;

static class ModConflictsTests
{
    const string Routing = "Sims3.Gameplay.Routing", Interactions = "Sims3.Gameplay.Interactions", Events = "Sims3.Gameplay.EventSystem";

    // An assembly with one method, Fixture.Entry.Init, whose body runs ops: ("call", ns, type, member, 0), ("stsfld", ...),
    // ("listen", "", "", "", event id) as EventTracker.AddListener(id, new ProcessEventDelegate(...)). events: an EventTypeId enum.
    internal static byte[] Asm(string name, Guid mvid, (string Op, string Ns, string Type, string Member, int Id)[] ops, (string Name, int Value)[]? events = null, string[]? refs = null)
    {
        var mb = new MetadataBuilder();
        StringHandle S(string s) => mb.GetOrAddString(s);
        BlobHandle Sig(Action<BlobEncoder> f) { var b = new BlobBuilder(); f(new BlobEncoder(b)); return mb.GetOrAddBlob(b); }
        mb.AddModule(0, S(name + ".dll"), mb.GetOrAddGuid(mvid), default, default);
        mb.AddAssembly(S(name), new Version(1, 0, 0, 0), default, default, 0, AssemblyHashAlgorithm.None);
        var corlib = mb.AddAssemblyReference(S("mscorlib"), new Version(4, 0, 0, 0), default, default, 0, default);
        var game = mb.AddAssemblyReference(S("Sims3GameplaySystems"), new Version(0, 0, 0, 0), default, default, 0, default);
        foreach (var r in refs ?? []) mb.AddAssemblyReference(S(r), new Version(1, 0, 0, 0), default, default, 0, default);
        TypeReferenceHandle Ref(AssemblyReferenceHandle a, string ns, string t) => mb.AddTypeReference(a, S(ns), S(t));
        var voidSig = Sig(b => b.MethodSignature().Parameters(0, r => r.Void(), _ => { }));
        var objField = Sig(b => b.FieldSignature().Object());
        var intField = Sig(b => b.FieldSignature().Int32());

        var il = new InstructionEncoder(new BlobBuilder());
        foreach (var o in ops)
            switch (o.Op)
            {
                case "call": il.Call(mb.AddMemberReference(Ref(game, o.Ns, o.Type), S(o.Member), voidSig)); break;
                case "stsfld": il.OpCode(ILOpCode.Ldnull); il.OpCode(ILOpCode.Stsfld); il.Token(mb.AddMemberReference(Ref(game, o.Ns, o.Type), S(o.Member), objField)); break;
                case "listen":
                    il.LoadConstantI4(o.Id);
                    il.OpCode(ILOpCode.Ldnull);
                    il.OpCode(ILOpCode.Newobj); il.Token(mb.AddMemberReference(Ref(game, Events, "ProcessEventDelegate"), S(".ctor"), voidSig));
                    il.Call(mb.AddMemberReference(Ref(game, Events, "EventTracker"), S("AddListener"), voidSig));
                    break;
            }
        il.OpCode(ILOpCode.Ret);
        var ilStream = new BlobBuilder();
        var body = new MethodBodyStreamEncoder(ilStream).AddMethodBody(il);

        var firstMethod = MetadataTokens.MethodDefinitionHandle(1);
        mb.AddTypeDefinition(0, default, S("<Module>"), default, MetadataTokens.FieldDefinitionHandle(1), firstMethod);
        var fields = 0;
        if (events is not null)
        {
            mb.AddFieldDefinition(FieldAttributes.Public | FieldAttributes.SpecialName | FieldAttributes.RTSpecialName, S("value__"), intField);
            foreach (var (n, v) in events)
                mb.AddConstant(mb.AddFieldDefinition(FieldAttributes.Public | FieldAttributes.Static | FieldAttributes.Literal | FieldAttributes.HasDefault, S(n), intField), v);
            fields = events.Length + 1;
            mb.AddTypeDefinition(TypeAttributes.Public | TypeAttributes.Sealed, S(Events), S("EventTypeId"), Ref(corlib, "System", "Enum"), MetadataTokens.FieldDefinitionHandle(1), firstMethod);
        }
        mb.AddMethodDefinition(MethodAttributes.Public | MethodAttributes.Static | MethodAttributes.HideBySig, MethodImplAttributes.IL, S("Init"), voidSig, body, MetadataTokens.ParameterHandle(1));
        mb.AddTypeDefinition(TypeAttributes.Public, S("Fixture"), S("Entry"), Ref(corlib, "System", "Object"), MetadataTokens.FieldDefinitionHandle(fields + 1), firstMethod);

        var o2 = new BlobBuilder();
        new ManagedPEBuilder(PEHeaderBuilder.CreateLibraryHeader(), new MetadataRootBuilder(mb), ilStream).Serialize(o2);
        return o2.ToArray();
    }

    static (uint, uint, ulong, byte[]) S3sa(string n, byte[] img) => (ModConflicts.TypeS3sa, 0u, (ulong)n.GetHashCode() & 0xfffffff, ModConflicts.S3saWrap(img));
    static (uint, uint, ulong, byte[]) Xml(ulong i, string s) => (ModConflicts.TypeXml, 0u, i, Encoding.UTF8.GetBytes(s));
    static byte[] Pkg(params (uint, uint, ulong, byte[])[] r) => ModConflicts.WritePackage(r);
    // a text table as the game writes it
    static byte[] Stbl(params (ulong Id, string Text)[] texts)
    {
        var ms = new MemoryStream();
        var w = new BinaryWriter(ms);
        w.Write("STBL"u8); w.Write((byte)2); w.Write((ushort)0); w.Write(texts.Length); w.Write(new byte[6]);
        foreach (var (id, t) in texts) { w.Write(id); w.Write(t.Length); w.Write(Encoding.Unicode.GetBytes(t)); }
        return ms.ToArray();
    }
    static (uint, uint, ulong, byte[]) Texts(ulong i, params (ulong, string)[] texts) => (ModConflicts.TypeStbl, 0u, i, Stbl(texts));

    // Only a difference the game sees is a conflict, and every one of those still is.
    static void RunSame(Action<bool, string> check, string tmp, ModConflicts.Rules rules)
    {
        const string doc = "<a k=\"1\" j=\"2\"><v>5</v><w/></a>";
        static byte[] U(string s) => Encoding.UTF8.GetBytes(s);
        bool Xml2(string a, string b) => ModConflicts.SameMeaning(ModConflicts.TypeXml, U(a), U(b));
        check(Xml2(doc, "\uFEFF<?xml version=\"1.0\" encoding=\"utf-8\"?>\r\n<a j='2' k='1'>\r\n\t<!-- five -->\r\n\t<v> 5 </v>\r\n\t<w></w>\r\n</a>\r\n")
              && ModConflicts.SameMeaning(ModConflicts.TypeXml, U(doc), Encoding.Unicode.GetPreamble().Concat(Encoding.Unicode.GetBytes("<?xml version=\"1.0\" encoding=\"utf-16\"?>" + doc)).ToArray())
              && ModConflicts.SameMeaning(0x03B33DDF, U(doc), U("<?xml version=\"1.0\" encoding=\"utf-16\"?>" + doc)),
            "same meaning: XML tuning that differs in its byte order mark, encoding, declaration, comments, layout and attribute order is the same");
        var changed = new[] { doc.Replace(">5<", ">6<"), doc.Replace("k=\"1\"", "k=\"2\""), doc.Replace("<w/>", "<x/>"), doc.Replace("<w/>", ""), doc.Replace("<w/>", "<w/><w/>"), doc.Replace(">5<", ">5 0<"),
                              doc.Replace(" j=\"2\"", ""), doc.Replace("<v>5</v><w/>", "<w/><v>5</v>"), doc.Replace(">5<", "><!--5-->4<"), doc.Replace("<v>5</v>", "<V>5</V>"), doc + "<!DOCTYPE a>", "not xml" };
        check(changed.All(c => !Xml2(doc, c)) && !Xml2("<a>caf\u00E9</a>", "<a>cafe</a>") && !ModConflicts.SameMeaning(ModConflicts.TypeXml, U("<a>caf\u00E9</a>"), Encoding.Latin1.GetBytes("<a>caf\u00E9</a>")),
            $"same meaning: one changed value, attribute, element or order in XML tuning is a difference ({string.Join(", ", changed.Where(c => Xml2(doc, c)))})");
        var (one, shuffled) = (Stbl((1, "Hello"), (2, "World"), (3, "")), Stbl((3, ""), (1, "Hello"), (2, "World")));
        check(ModConflicts.SameMeaning(ModConflicts.TypeStbl, one, shuffled) && ModConflicts.TextsThatDiffer(one, shuffled) == 0
              && !ModConflicts.SameMeaning(ModConflicts.TypeStbl, one, Stbl((1, "Hello"), (2, "Mundo"), (3, ""))) && ModConflicts.TextsThatDiffer(one, Stbl((1, "Hello"), (2, "Mundo"), (3, ""))) == 1
              && !ModConflicts.SameMeaning(ModConflicts.TypeStbl, one, Stbl((1, "Hello"), (2, "World"))) && ModConflicts.TextsThatDiffer(one, Stbl((1, "Ola"), (2, "World"), (4, "x"))) == 3
              && !ModConflicts.SameMeaning(ModConflicts.TypeStbl, one, Stbl((1, "Hello"), (2, "World"), (3, ""), (1, "Other"))) && ModConflicts.Texts(one.Concat(new byte[1]).ToArray()) is null
              && ModConflicts.Texts(one[..^1]) is null && !ModConflicts.SameMeaning(ModConflicts.TypeXml, one, shuffled),
            "same meaning: a text table is its texts by id in any order; one other text, or one more or less, is a difference, and is counted");
        check(!ModConflicts.SameMeaning(0x00B2D882, [1, 2, 3, 4], [1, 2, 3, 5]) && ModConflicts.SameMeaning(0x00B2D882, [1, 2, 3, 4], [1, 2, 3, 4]), "same meaning: anything else of one size with other bytes is a difference");
        (string Piece, string Whole, bool Says)[] names = [("Cooking_Patch", "Cooking", true), ("Creator_Cooking - optional no burn", "Creator_Cooking_v2.1", true), ("CookingAddon", "Cooking", true),
            ("Creator_Overhaul_Tuning", "Creator_Overhaul_Main", true), ("Cooking_v2", "Cooking_v1", false), ("Cooking (2)", "Cooking", false), ("Cooking_old", "Cooking", false), ("Cooking - Copy", "Cooking", false),
            ("TinyUIFix_backup", "TinyUIFix", false), ("Other_Patch", "Cooking", false), ("Cooking", "Cooking_Patch", false)];
        check(names.All(n => ModConflicts.SaysPiece(n.Piece, n.Whole) == n.Says), $"mod conflicts: a name says a patch, an add-on or an optional part of another mod; a version or a copy does not ({string.Join(", ", names.Where(n => ModConflicts.SaysPiece(n.Piece, n.Whole) != n.Says).Select(n => n.Piece))})");

        List<ModConflicts.Conflict> In(string name, params (string File, byte[] Pkg)[] files)
        {
            var mods = Path.Combine(tmp, name, "Mods");
            foreach (var (f, b) in files) { Directory.CreateDirectory(Path.GetDirectoryName(Path.Combine(mods, f))!); File.WriteAllBytes(Path.Combine(mods, f), b); }
            File.WriteAllText(Path.Combine(mods, "Resource.cfg"), "Priority 500\nPackedFile Packages/*.package\nPriority 1000\nPackedFile Overrides/*.package\n");
            return ModConflicts.Scan(new() { Roots = { mods }, Rules = rules }).Conflicts;
        }
        ModConflict Of(ModConflicts.Conflict c) => ModConflict.Of(new ConflictEntry(c.Kind, c.Text, c.Parties, Regex.Match(c.Text, "likely winner: ([^;]+)") is { Success: true } w ? Path.GetFileNameWithoutExtension(w.Groups[1].Value.Trim()) : null), []);

        // written another way in two mods: nothing; with a real difference among them: that one only
        var layout = In("Layout", ("Packages/a.package", Pkg(Xml(1, doc), Xml(2, doc), Texts(3, (1, "a"), (2, "b")), (ModConflicts.TypeKey, 0, 0, U("names a")))),
            ("Packages/b.package", Pkg(Xml(1, "<!-- mine -->\n<a j=\"2\" k=\"1\"><v>5</v><w/></a>"), Xml(2, doc.Replace(">5<", ">7<")), Texts(3, (2, "b"), (1, "a")), (ModConflicts.TypeKey, 0, 0, U("names b")))));
        check(layout is [{ Kind: "resource key" } lay] && lay.Text.StartsWith("1 shared keys with different content (_XML 1; 2 identical)") && lay.Evidence is [var ev] && ev.EndsWith(":0000000000000002"),
            $"mod conflicts: tuning and texts written another way are no conflict, name maps never are, and the one changed value still is ({string.Join(" / ", layout.Select(c => c.Text))})");
        // two mods with the same copy and one with another: one conflict, which says who loses nothing
        var three = In("Three", ("Packages/a.package", Pkg(Xml(1, doc))), ("Packages/b.package", Pkg(Xml(1, " " + doc))), ("Packages/c.package", Pkg(Xml(1, doc.Replace(">5<", ">6<")))));
        var threeLast = In("ThreeLast", ("Packages/a.package", Pkg(Xml(1, doc.Replace(">5<", ">6<")))), ("Packages/b.package", Pkg(Xml(1, doc))), ("Packages/c.package", Pkg(Xml(1, doc))));
        check(three is [{ Parties.Length: 3 } t3] && Of(t3) is { Winner: "a", Alike: ["b"], Level: ConflictLevel.Look } && threeLast is [var tl] && Of(tl) is { Winner: "a", Alike.Length: 0 },
            "mod conflicts: of two mods with the same copy and one with another, the one like the copy in use loses nothing");
        // a script adapted for a folder that loads first is the same build, no conflict; another build is, and so are two in one folder
        var build = Guid.NewGuid();
        byte[] Script(Guid id, int pad) => Pkg((ModConflicts.TypeS3sa, 0, 9, ModConflicts.S3saWrap([.. Asm("Fixture", id, []), .. new byte[pad]])));
        var adapted = In("Adapted", ("Overrides/fix.package", Script(build, 8)), ("Packages/mod.package", Script(build, 0)));
        var other = In("AdaptedOld", ("Overrides/fix.package", Script(Guid.NewGuid(), 8)), ("Packages/mod.package", Script(build, 0)));
        var beside = In("AdaptedBeside", ("Packages/fix.package", Script(build, 8)), ("Packages/mod.package", Script(build, 0)));
        check(!adapted.Any(c => c.Kind is "decided" or "resource key") && other.Any(c => c.Kind == "decided") && beside.Any(c => c.Kind == "resource key"),
            $"mod conflicts: a script's adapted copy of the same build in a folder that loads first is no conflict, another build is ({string.Join(" / ", adapted.Concat(other).Concat(beside).Select(c => c.Kind))})");
        var off = Path.Combine(tmp, "Off", "Mods");
        foreach (var f in new[] { "Packages/a.package", "Disabled (Apex Launcher)/Packages/b.package" })
        { Directory.CreateDirectory(Path.GetDirectoryName(Path.Combine(off, f))!); File.WriteAllBytes(Path.Combine(off, f), Pkg(Xml(1, f))); }
        File.WriteAllText(Path.Combine(off, "Resource.cfg"), "Priority 500\nPackedFile Packages/*.package\n");
        check(ModConflicts.Scan(new() { Roots = { off }, Rules = rules, LoadedOnly = true }).Conflicts.Count == 0 && ModConflicts.Scan(new() { Roots = { off }, Rules = rules }).Conflicts.Count == 1,
            "mod conflicts: a mod the game does not load conflicts with nothing");
        // only texts differ, beside tuning both hold alike: a matter of texts, with how many
        var words = In("Words", ("Packages/a.package", Pkg(Xml(1, doc), Texts(3, (1, "a"), (2, "b"), (3, "c")))), ("Packages/b.package", Pkg(Xml(1, doc), Texts(3, (1, "a"), (2, "B"), (3, "C")))));
        check(words is [{ Kind: "resource key" } wd] && wd.Text.Contains("(STBL 1; 1 identical; 2 texts differ)") && Of(wd) is { Level: ConflictLevel.Small, SayArgs: ["a", "2"], Finding: null },
            $"mod conflicts: texts that differ beside tuning that does not are a small conflict that says how many ({string.Join(" / ", words.Select(c => c.Text))})");
        // a translation that is in use; one that the mod's own texts hide is a conflict of texts as before
        var mod = Pkg(Xml(1, doc), Texts(0x17AA, (1, "Cook"), (2, "Eat")));
        var tr = Pkg(Texts(0x17AA, (1, "Cozinhar"), (2, "Comer")));
        var translated = In("Translated", ("Packages/Cooking.package", mod), ("Packages/!Cooking_PT-BR.package", tr));
        var hidden = In("Hidden", ("Packages/Cooking.package", mod), ("Packages/Cooking_PT-BR.package", tr));
        check(translated is [{ Kind: "translation" } tc] && Of(tc) is { Level: ConflictLevel.Small, SayArgs: ["!Cooking_PT-BR", "Cooking"], Finding: null } && Of(tc).Id.StartsWith("resource key|")
              && hidden is [{ Kind: "resource key" } hc] && Of(hc) is { Level: ConflictLevel.Small, Winner: "Cooking" },
            "mod conflicts: a package of texts only, in use over the mod it translates, is its translation");
        // a patch of another mod in use over it; under it, or as a version of it, a conflict as before
        var whole = Pkg(Xml(1, doc), Xml(2, doc), Xml(3, doc));
        var patch = Pkg(Xml(2, doc.Replace(">5<", ">9<")));
        var patched = In("Patched", ("Packages/Cooking.package", whole), ("Overrides/Cooking_Patch.package", patch));
        var under = In("Under", ("Packages/Cooking.package", whole), ("Packages/Cooking_Patch.package", patch));
        var version = In("Version", ("Packages/Cooking_v2.package", whole), ("Packages/Cooking_v1.package", patch));
        var stranger = In("Stranger", ("Packages/Cooking.package", whole), ("Packages/!Other_Patch.package", patch));
        var more = In("More", ("Packages/Cooking.package", whole), ("Packages/!Cooking_Patch.package", Pkg(Xml(2, doc.Replace(">5<", ">9<")), Xml(8, doc))));
        check(patched is [{ Kind: "add-on" } pc] && Of(pc) is { Level: ConflictLevel.Small, SayArgs: ["Cooking_Patch", "Cooking"], Finding: null }
              && new[] { under, version, stranger, more }.All(l => l is [{ Kind: "resource key" } c] && Of(c).Level == ConflictLevel.Look),
            "mod conflicts: a patch in use over the mod it is for replaces part of it on purpose; one under it, a version, another mod or one with more of its own is a conflict");
        // the folder Resource.cfg puts first decides; the id is the one it had before, so an ignored one stays ignored
        var first = In("First", ("Overrides/zeta.package", Pkg(Xml(1, doc))), ("Packages/alpha.package", Pkg(Xml(1, doc.Replace(">5<", ">6<")))));
        var level = In("Level", ("Packages/zeta.package", Pkg(Xml(1, doc))), ("Packages/alpha.package", Pkg(Xml(1, doc.Replace(">5<", ">6<")))));
        check(first is [{ Kind: "decided" } fc] && Of(fc) is { Level: ConflictLevel.Small, Winner: "zeta", SayArgs: ["zeta"], Finding: null, CanOrder: true }
              && level is [{ Kind: "resource key" } lc] && Of(lc) is { Level: ConflictLevel.Look, Winner: "alpha" } && Of(fc).Id == Of(lc).Id,
            "mod conflicts: a mod in a folder that loads first is the one set to win, a lower conflict with the same id");
        // a mod alone on a key (a default replacement of game data) is no conflict
        check(In("Alone", ("Packages/a.package", Pkg(Xml(1, doc))), ("Packages/b.package", Pkg(Xml(2, doc)))).Count == 0, "mod conflicts: a mod that alone replaces a resource is no conflict");
    }

    // the hook table: format checks, download fallbacks, edition filter, out-of-date builds
    static void RunHooks(Action<bool, string> check)
    {
        var embedded = ModConflicts.HookTable.Embedded();
        check(embedded.Editions.Count == 3 && embedded.Hooks.Any(h => h.Mod == "ts3native" && h.Edition == "steam-1.67") && embedded.Hooks.Any(h => h.Mod == "S3SS"),
            "hook table: the embedded copy parses, with three editions");
        const string head = "date,2030-01-01\nedition,steam-1.67,0x52DEC247\nedition,retail-1.67,0x52D872DA\nmod,A,^a\\.asi$,1.0,\n";
        foreach (var bad in new[] { head, head + "hook,A,steam-1.67,0x10,imm,low address\n", head + "hook,Z,steam-1.67,0x500000,imm,unknown mod\n",
                                     head + "hook,A,ea-9,0x500000,imm,unknown edition\n", head + "hook,A,steam-1.67,500000,imm,no 0x\n", head + "hook,A,steam-1.67,0x500000,imm,x,Z=A stands down\n",
                                     head + "hook,A,steam-1.67,0x500000,imm,x,A=A chains\n", head + "mod,B,^b\\.asi$,1.0,\nhook,A,steam-1.67,0x500000,imm,x,B=C stands down\n",
                                     head + "mod,B,^b\\.asi$,1.0,\nhook,A,steam-1.67,0x500000,imm,x,B=B ignores\n", "<html>not found</html>\n" })
        {
            var rejected = false;
            try { ModConflicts.HookTable.Parse(bad, "test"); } catch (InvalidDataException) { rejected = true; }
            check(rejected, $"hook table: a bad file is rejected ({bad.Split('\n')[^2]})");
        }
        var good = head + "hook,A,steam-1.67,0x500000,imm,a\n";
        var cache = Path.Combine(Directory.CreateTempSubdirectory("ApexHookCache-").FullName, "hook-table.csv");
        try
        {
            var calls = 0;
            check(ModConflicts.HookTable.Load(false, cache, () => throw new HttpRequestException("offline")).Source == "embedded copy", "hook table: a failed download falls back to the embedded copy");
            check(ModConflicts.HookTable.Load(false, cache, () => "<html>").Source == "embedded copy" && !File.Exists(cache), "hook table: an invalid download is not used or cached");
            check(ModConflicts.HookTable.Load(true, cache, () => { calls++; return good; }).Source == "embedded copy" && calls == 0, "hook table: --offline never downloads");
            var got = ModConflicts.HookTable.Load(false, cache, () => good);
            check(got.Date == "2030-01-01" && got.Source == ModConflicts.HookTableUrl && File.Exists(cache), "hook table: a newer valid download is used and cached");
            check(ModConflicts.HookTable.Load(true, cache).Date == "2030-01-01", "hook table: offline, the newer cached copy is used");
        }
        finally { Directory.Delete(Path.GetDirectoryName(cache)!, true); }
    }

    public static void Run(Action<bool, string> check)
    {
        RunHooks(check);
        // the rule table's own examples
        var rules = ModConflicts.Rules.Load();
        var bad = new List<string>();
        foreach (var c in rules.Cases)
        {
            var want = c.GetProperty("area").ValueKind == System.Text.Json.JsonValueKind.Null ? null : c.GetProperty("area").GetString();
            if (c.TryGetProperty("call", out var call))
            {
                var k = ModConflicts.ClassifyCall(rules, call.GetString()!, c.TryGetProperty("field", out var f) && f.GetBoolean());
                if (k.Write != c.GetProperty("write").GetBoolean() || k.Areas.FirstOrDefault() != want) bad.Add(call.GetString()!);
            }
            else if (c.TryGetProperty("event", out var ev)) { if (ModConflicts.EventAreas(rules, ev.GetString()!).FirstOrDefault() != want) bad.Add(ev.GetString()!); }
            else if (ModConflicts.NativeAreas(rules, c.GetProperty("native").GetString()!).FirstOrDefault() != want) bad.Add(c.GetProperty("native").GetString()!);
        }
        check(rules.Cases.Length > 20 && bad.Count == 0, $"mod conflicts: the rule table's {rules.Cases.Length} examples classify as listed {string.Join(", ", bad)}");

        // load order: priority, then directive order, then name order inside one pattern (the last part unverified in game)
        var cfg = ModConflicts.ParseResourceCfg("Priority 500\nPackedFile Packages/*.package\nPackedFile Packages/*/*.package # sub\nPriority 1000\nPackedFile Overrides/*.package\n");
        var K = (string rel) => ModConflicts.LoadKey(cfg, rel);
        check(cfg.Count == 3 && cfg[2].Priority == 1000 && K("Packages/a.package")!.Value.Item2 == 0 && K("Packages/x/a.package")!.Value.Item2 == 1 && K("Packages/x/y/a.package") is null,
            "mod conflicts: Resource.cfg priorities and pattern depth");
        check(ModConflicts.CompareKey(K("Overrides/z.package")!.Value, K("Packages/a.package")!.Value) < 0 && ModConflicts.CompareKey(K("Packages/B.package")!.Value, K("Packages/c.package")!.Value) < 0
            && ModConflicts.CompareKey(K("Packages/z.package")!.Value, K("Packages/a/a.package")!.Value) < 0, "mod conflicts: higher priority, then top folder, then case-insensitive name order load first");

        var game = new (string, int)[] { ("kSimDied", 3), ("kSimAgeTransition", 7), ("kBoughtObject", 9) };
        var stub = Asm("Sims3GameplaySystems", new Guid("11111111-1111-1111-1111-111111111111"), Array.Empty<(string, string, string, string, int)>(), game);
        (string, string, string, string, int)[] a = { ("call", Routing, "Route", "PlanTo", 0), ("call", Routing, "RoutingComponent", "SetRouteOption", 0), ("stsfld", Interactions, "ChatRelaxed", "Singleton", 0), ("listen", "", "", "", 3) };
        var modA = Asm("ModRouteA", new Guid("aaaaaaaa-0000-0000-0000-000000000001"), a);
        var modOld = Asm("ModRouteA", new Guid("aaaaaaaa-0000-0000-0000-000000000002"), a);
        var modB = Asm("ModRouteB", new Guid("bbbbbbbb-0000-0000-0000-000000000001"), new[] { ("call", Routing, "RoutingComponent", "SetRouteOption", 0), ("stsfld", Interactions, "ChatRelaxed", "Singleton", 0) });
        var modR = Asm("ModReader", new Guid("cccccccc-0000-0000-0000-000000000001"), new[] { ("call", Routing, "RoutingComponent", "GetRouteOption", 0), ("call", Routing, "Route", "get_IsRouting", 0) });

        var tmp = Directory.CreateTempSubdirectory("ApexModConflictsTest-").FullName; // the test writes only here; the scan reads
        try
        {
            var mods = Path.Combine(tmp, "Mods");
            var packages = Path.Combine(mods, "Packages");
            Directory.CreateDirectory(Path.Combine(packages, "sub"));
            Directory.CreateDirectory(Path.Combine(tmp, "asm"));
            Directory.CreateDirectory(Path.Combine(tmp, "Bin"));
            File.WriteAllText(Path.Combine(tmp, "asm", "Sims3GameplaySystems.dll"), "not the stub"); // a game copy whose MVID differs from the stub's
            File.WriteAllText(Path.Combine(mods, "Resource.cfg"), "Priority 500\nPackedFile Packages/*.package\nPackedFile Packages/*/*.package\n");
            File.WriteAllBytes(Path.Combine(packages, "a_route.package"), Pkg(S3sa("A", modA), Xml(1, "<a/>"), Xml(2, "<same/>")));
            File.WriteAllBytes(Path.Combine(packages, "b_route.package"), Pkg(S3sa("B", modB), Xml(1, "<b/>"), Xml(2, "<same/>")));
            File.WriteAllBytes(Path.Combine(packages, "c_reader.package"), Pkg(S3sa("R", modR)));
            File.WriteAllBytes(Path.Combine(packages, "d_core1.package"), Pkg(S3sa("G", stub)));
            File.WriteAllBytes(Path.Combine(packages, "sub", "e_core2.package"), Pkg(S3sa("G", stub)));
            File.WriteAllBytes(Path.Combine(packages, "f_old.package"), Pkg(S3sa("A2", modOld)));
            File.WriteAllBytes(Path.Combine(tmp, "Bin", "x.asi"), stub); // (a PE without routing strings)
            File.WriteAllBytes(Path.Combine(tmp, "Bin", "y.asi"), stub);
            var known = new List<ModConflicts.KnownAsi>
            {
                new("x", new Regex("^x\\.asi$"), new HashSet<uint> { 0x401000, 0x500000 }, "table x"),
                new("y", new Regex("^y\\.asi$"), new HashSet<uint> { 0x500000 }, "table y"),
            };
            var r = ModConflicts.Scan(new() { Roots = { mods }, Rules = rules, AsmDir = Path.Combine(tmp, "asm"), Bin = Path.Combine(tmp, "Bin"), KnownAsi = known });
            List<ModConflicts.Conflict> Kind(ModConflicts.Report rep, string k) => rep.Conflicts.Where(c => c.Kind == k).ToList();
            var key = Kind(r, "resource key");
            check(key.Count == 1 && key[0].Text.StartsWith("1 shared keys") && key[0].Text.Contains("; 1 identical") && key[0].Text.Contains("likely winner: Mods/Packages/a_route.package"),
                "mod conflicts: one _XML key with different bytes is a conflict, the identical one is counted, a_route is the likely winner");
            check(Kind(r, "core mod") is [{ Parties.Length: 2 }], "mod conflicts: two packages replacing Sims3GameplaySystems are a core mod conflict");
            check(Kind(r, "assembly") is [{ } asm] && string.Join(",", asm.Parties) == "a_route,f_old", "mod conflicts: one assembly name with two builds is a conflict");
            check(Kind(r, "interaction") is [{ } it] && it.Evidence[0] == "Sims3.Gameplay.Interactions.ChatRelaxed::Singleton" && string.Join(",", it.Parties) == "a_route,b_route,f_old",
                "mod conflicts: mods storing one interaction Singleton conflict");
            check(Kind(r, "native hook") is [{ } nh] && string.Join(",", nh.Evidence) == "0x500000", "mod conflicts: two ASIs with known tables sharing an address conflict");
            var rt = r.Warnings.FirstOrDefault(w => w.Area == "routing");
            check(rt is not null && string.Join(",", rt.Parties.Select(p => p.Name)) == "a_route,b_route,f_old" && rt.Parties[0].Calls[0] == "Sims3.Gameplay.Routing.RoutingComponent::SetRouteOption x1",
                "mod conflicts: routing warning names the mods and their write calls");
            check(!r.Warnings.Any(w => w.Parties.Any(p => p.Name == "c_reader")), "mod conflicts: a mod that only reads gives no warning");
            check(ModConflicts.Render(r).Contains("CONFLICT (5)"), "mod conflicts: the report lists five conflicts");

            // with the stub as the game's assembly: the event id gets its name, the stubs are the game's own build
            File.WriteAllBytes(Path.Combine(tmp, "asm", "Sims3GameplaySystems.dll"), stub);
            var r2 = ModConflicts.Scan(new() { Roots = { mods }, Rules = rules, AsmDir = Path.Combine(tmp, "asm") });
            var ag = r2.Warnings.FirstOrDefault(w => w.Area == "aging");
            check(ag is not null && ag.Parties.All(p => p.Calls[0] == "listens kSimDied x1"), "mod conflicts: AddListener(kSimDied) counts in aging, by the game's event name");
            check(Kind(r2, "core mod").Count == 0 && r2.Info.Any(i => i.Text.StartsWith("duplicate copy: 1 identical")), "mod conflicts: the game's own build is no core mod; identical copies are INFO");

            // two mods blanking the same video with their own small placeholders: the same fix twice, no conflict; a real
            // resource in one of them still is one
            var blanks = Path.Combine(tmp, "Blanks");
            Directory.CreateDirectory(blanks);
            File.WriteAllBytes(Path.Combine(blanks, "nointro.package"), Pkg((0xB1CC1AF6, 0, 0x4E5D8B03C2FF9DFC, new byte[32])));
            File.WriteAllBytes(Path.Combine(blanks, "nointromaxis.package"), Pkg((0xB1CC1AF6, 0, 0x4E5D8B03C2FF9DFC, Enumerable.Repeat((byte)1, 72).ToArray()), (0x73E93EEB, 0, 0, "<manifest/>"u8.ToArray())));
            var rb = ModConflicts.Scan(new() { Roots = { blanks }, Rules = rules });
            File.WriteAllBytes(Path.Combine(blanks, "nointromaxis.package"), Pkg((0xB1CC1AF6, 0, 0x4E5D8B03C2FF9DFC, new byte[5000])));
            var rb2 = ModConflicts.Scan(new() { Roots = { blanks }, Rules = rules });
            check(Kind(rb, "resource key").Count == 0 && rb.Info.Any(i => i.Text.StartsWith("the same fix twice")) && Kind(rb2, "resource key").Count == 1,
                "mod conflicts: the same small placeholders twice are no conflict, and package manifests never are; a real resource still is");

            // one build stored twice (a duplicate copy) is no interaction conflict
            var dup = Path.Combine(tmp, "Dup");
            Directory.CreateDirectory(dup);
            File.WriteAllBytes(Path.Combine(dup, "one.package"), Pkg(S3sa("A", modA)));
            File.WriteAllBytes(Path.Combine(dup, "two.package"), Pkg(S3sa("A", modA)));
            check(Kind(ModConflicts.Scan(new() { Roots = { dup }, Rules = rules }), "interaction").Count == 0, "mod conflicts: the same build twice is no interaction conflict");

            // hook table: rows of the game's edition only, listed builds only; another build of a listed mod is INFO
            {
                var gb = Path.Combine(tmp, "HookBin");
                Directory.CreateDirectory(gb);
                string Sha(byte[] b) => Convert.ToHexString(System.Security.Cryptography.SHA256.HashData(b)).ToLowerInvariant();
                var table = ModConflicts.HookTable.Parse(
                    "date,2030-01-01\nedition,steam-1.67,0x52DEC247\nedition,retail-1.67,0x52D872DA\n"
                    + $"mod,A,^a\\.asi$,1.0,{Sha(stub)}\nmod,B,^b\\.asi$,1.0,{Sha(modR)}\nmod,C,^c\\.asi$,1.0,{new string('0', 64)}\n"
                    + "hook,A,steam-1.67,0x500000,detour,A's detour\nhook,B,steam-1.67,0x500000,call,B's call\nhook,C,steam-1.67,0x500000,imm,C's patch\n"
                    + "hook,A,steam-1.67,0x500010,detour,A's,B=B stands down\nhook,B,steam-1.67,0x500010,call,B's\nhook,B,steam-1.67,0x500020,call,B's,A=A chains\nhook,A,steam-1.67,0x500020,detour,A's,\n"
                    + "hook,A,retail-1.67,0x700000,detour,A moved\nhook,B,retail-1.67,0x600000,call,B moved\n", "test");
                File.WriteAllBytes(Path.Combine(gb, "a.asi"), stub);
                File.WriteAllBytes(Path.Combine(gb, "b.asi"), modR);
                File.WriteAllBytes(Path.Combine(gb, "c.asi"), modA);
                void Exe(uint stamp)
                {
                    var exe = (byte[])stub.Clone();
                    BitConverter.GetBytes(stamp).CopyTo(exe, BitConverter.ToInt32(exe, 0x3c) + 8); // COFF TimeDateStamp
                    File.WriteAllBytes(Path.Combine(gb, "TS3W.exe"), exe);
                }
                var empty = Path.Combine(tmp, "NoMods");
                Directory.CreateDirectory(empty);
                ModConflicts.Report HookScan() => ModConflicts.Scan(new() { Roots = { empty }, Rules = rules, Bin = gb, Hooks = table });
                Exe(0x52DEC247);
                var steam = HookScan();
                check(Kind(steam, "native hook") is [{ } h1] && string.Join(",", h1.Parties) == "a,b" && string.Join(",", h1.Evidence) == "0x500000 (A's detour / B's call)" && h1.Text.Contains(" 1 of the same")
                      && steam.Info.Any(i => i.Party == "a + b" && i.Text == "both hook 0x500010; B stands down (coordinated)") && steam.Info.Any(i => i.Text == "both hook 0x500020; A chains (coordinated)")
                      && steam.Info.Any(i => i.Party == "hook table" && i.Text.Contains("game build steam-1.67")),
                    "hook table: on Steam, two listed builds sharing an address conflict, with both purposes; coordinated addresses are INFO");
                check(steam.Info.Any(i => i.Party == "c" && i.Text.StartsWith("hook table may be out of date for c.asi (table dated 2030-01-01)")) && !Kind(steam, "native hook").Any(c => c.Parties.Contains("c")),
                    "hook table: a build the table does not list is INFO, never a conflict");
                Exe(0x52D872DA);
                check(Kind(HookScan(), "native hook").Count == 0, "hook table: on retail, the retail rows apply (no shared address)");
                Exe(0x12345678);
                var unknownExe = HookScan();
                check(Kind(unknownExe, "native hook").Count == 0 && unknownExe.Info.Any(i => i.Text.Contains("game build not identified")), "hook table: an unknown game build applies no rows");
            }

            RunSame(check, tmp, rules);

            // no extractor that reads an archive: unknown with the 7-Zip command, which the command line offers
            {
                var saved = ModConflicts.Extractors;
                ModConflicts.Extractors = () => new();
                try
                {
                    var rar = Path.Combine(tmp, "Rar");
                    Directory.CreateDirectory(rar);
                    File.WriteAllBytes(Path.Combine(rar, "x.rar"), new byte[] { 0x52, 0x61, 0x72, 0x21 });
                    var rr = ModConflicts.Scan(new() { Roots = { rar }, Rules = rules });
                    check(rr.NeedsSevenZip && rr.Unknown.Single().Text.Contains(ModConflicts.SevenZipInstall), "mod conflicts: an archive no extractor reads names the 7-Zip install command");
                }
                finally { ModConflicts.Extractors = saved; }
            }

            // archives: names that leave the folder are refused; a 7z with a nested zip is read (when this tar can write 7z)
            var names = new (string, bool)[] { ("a/b.package", true), ("x..y/z", true), ("../evil", false), ("a/../../evil", false), ("/etc/x", false), ("C:\\x", false), ("\\\\srv\\x", false), ("a\\..\\b", false), ("", false) };
            check(names.All(n => ModConflicts.SafeName(n.Item1) == n.Item2), "mod conflicts: archive entry names are checked");
            var tar = Path.Combine(Environment.SystemDirectory, "tar.exe");
            var arc = Path.Combine(tmp, "Arc");
            var src = Path.Combine(tmp, "src");
            Directory.CreateDirectory(arc);
            Directory.CreateDirectory(src);
            File.WriteAllBytes(Path.Combine(src, "g_in7z.package"), Pkg(Xml(1, "<from7z/>")));
            using (var z = ZipFile.Open(Path.Combine(src, "inner.zip"), ZipArchiveMode.Create))
            using (var s = z.CreateEntry("h_nested.package").Open())
                s.Write(Pkg(Xml(5, "<n/>")));
            if (File.Exists(tar) && RunTar(tar, "--format", "7zip", "-cf", Path.Combine(arc, "m7.7z"), "-C", src, "g_in7z.package", "inner.zip"))
            {
                File.WriteAllBytes(Path.Combine(arc, "a.package"), Pkg(Xml(1, "<a/>"), Xml(5, "<other/>")));
                var before = Directory.GetDirectories(Path.GetTempPath(), "modscan-*").Length;
                var r3 = ModConflicts.Scan(new() { Roots = { arc }, Rules = rules });
                var k3 = string.Join("\n", Kind(r3, "resource key").Select(c => c.Text));
                check(k3.Contains("m7.7z!g_in7z.package") && k3.Contains("m7.7z!inner.zip!h_nested.package") && r3.Unknown.Count == 0, "mod conflicts: a 7z and a zip inside it are read");
                check(Directory.GetDirectories(Path.GetTempPath(), "modscan-*").Length == before, "mod conflicts: extracted folders are removed after the scan");
            }
            else Console.WriteLine("skip mod conflicts: this tar.exe cannot write 7z");
            // a tar with a '../' entry (tar reads it whatever the extension)
            var h = new byte[512];
            void Put(int at, string s) => Encoding.ASCII.GetBytes(s).CopyTo(h, at);
            Put(0, "../evil.package"); Put(100, "0000644\0"); Put(108, "0000000\0"); Put(116, "0000000\0"); Put(124, "00000000004\0"); Put(136, "00000000000\0");
            Put(148, "        "); Put(156, "0"); Put(257, "ustar\0"); Put(263, "00");
            Put(148, Convert.ToString(h.Sum(b => b), 8).PadLeft(6, '0') + "\0 ");
            var evil = Path.Combine(tmp, "evil.7z");
            File.WriteAllBytes(evil, h.Concat(Encoding.ASCII.GetBytes("evil")).Concat(new byte[508 + 1024]).ToArray());
            if (File.Exists(tar))
            {
                var ev = ModConflicts.Extract(evil);
                check(ev.Dir is null && ev.Unknown is { } u && u.Contains("unsafe entry \"../evil.package\""), "mod conflicts: an archive entry with '..' is refused before extraction");
            }
        }
        finally { Directory.Delete(tmp, true); }
    }

    static bool RunTar(string tar, params string[] args)
    {
        var psi = new ProcessStartInfo(tar) { UseShellExecute = false, CreateNoWindow = true, RedirectStandardError = true, RedirectStandardOutput = true };
        foreach (var a in args) psi.ArgumentList.Add(a);
        using var p = Process.Start(psi)!;
        p.WaitForExit(60_000);
        return p.HasExited && p.ExitCode == 0;
    }
}
