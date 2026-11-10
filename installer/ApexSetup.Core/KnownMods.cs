namespace ApexSetup;

// Mods of sims3-modernization's community fixes list (tools/setup/fixes.json in its repository) that Setup recognises
// but does not install: their authors publish them outside GitHub, or they are generators. Shown with a link to the
// official page (where known), so the player gets them from their authors.
public sealed record KnownMod(string Name, bool InBin, string[] Patterns, string? Url);

public static class KnownMods
{
    public static readonly KnownMod[] All =
    {
        new("Smooth Patch (ASI)", true, new[] { "TS3Patch.asi" }, "https://modthesims.info/d/658759/smooth-patch-2-1.html"),
        new("Smooth Patch", false, new[] { "ld_SmoothPatch*.package" }, "https://modthesims.info/d/658759/smooth-patch-2-1.html"),
        new("NRaas ErrorTrap", false, new[] { "NRaas_ErrorTrap*.package" }, "https://www.nraas.net/community/ErrorTrap"),
        new("NRaas Overwatch", false, new[] { "NRaas_Overwatch*.package" }, "https://www.nraas.net/community/Overwatch"),
        new("NRaas MasterController", false, new[] { "NRaasMasterController*.package", "NRaas_MasterController*.package" }, "https://www.nraas.net/community/MasterController"),
        new("NRaas StoryProgression", false, new[] { "NRaasStoryProgression*.package", "NRaas_StoryProgression*.package" }, "https://www.nraas.net/community/StoryProgression"),
        new("simler90 Gameplay Core Mod", false, new[] { "simler90GameplayCoreMod*.package" }, "https://modthesims.info/d/659969/"),
        new("Catalogue Lag and Memory Leak Fix", false, new[] { "S3DBG_CatalogueLagMemoryLeakFix*.package" }, null),
        new("Tiny UI Fix", false, new[] { "tiny-ui-fix*.package", "TinyUIFix*.package" }, "https://github.com/just-harry/tiny-ui-fix-for-ts3"),
        new("TS3 Banding Fix (LessBanding)", false, new[] { "ld_LessBanding*.package" }, "https://github.com/LazyDuchess/TS3-Banding-Fix"),
    };

    public static List<KnownMod> Found(string bin, string docs)
    {
        var mods = Path.Combine(docs, "Mods");
        return All.Where(m => m.Patterns.Any(p => m.InBin
            ? Directory.Exists(bin) && Directory.EnumerateFiles(bin, p).Any()
            : Directory.Exists(mods) && Directory.EnumerateFiles(mods, p, SearchOption.AllDirectories).Any())).ToList();
    }
}
