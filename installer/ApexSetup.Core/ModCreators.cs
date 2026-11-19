namespace ApexSetup;

// Who made a mod: the name the player set, else a guess from how makers name their files ("NRaas_Career",
// "ani_BistroStove", "BB-SmartSim", "icarusallsorts.BeachTowel"): the part before the first _ - . or space, kept only when
// two or more mods share it (one file's prefix is too often just a word of its name) or it is a known maker's.
public static class ModCreators
{
    // makers whose prefix alone is enough, and the short prefixes some of them use
    static readonly Dictionary<string, string> Known = new(StringComparer.OrdinalIgnoreCase)
    {
        ["NRaas"] = "NRaas", ["Twallan"] = "NRaas", ["BB"] = "BoringBones", ["BoringBones"] = "BoringBones", ["icarusallsorts"] = "icarusallsorts",
    };

    public static string? Prefix(string fileName)
    {
        var name = Path.GetFileNameWithoutExtension(fileName).Trim();
        var cut = name.IndexOfAny(['_', '-', '.', ' ']);
        if (cut <= 1) return null;
        var p = name[..cut];
        return p.Length <= 24 && p.Any(char.IsLetter) ? p : null;
    }

    // every mod's guessed maker, from the whole list (a prefix shared by two or more files, or a known one)
    public static Dictionary<string, string> Guess(IEnumerable<ModFile> mods)
    {
        var list = mods.Where(m => m.FileType is "package" or "asi" or "sims3pack").ToList();
        var counts = list.Select(m => Prefix(m.Name)).OfType<string>().GroupBy(p => p, StringComparer.OrdinalIgnoreCase).ToDictionary(g => g.Key, g => g.Count(), StringComparer.OrdinalIgnoreCase);
        var guess = new Dictionary<string, string>(StringComparer.OrdinalIgnoreCase);
        foreach (var m in list)
            if (Prefix(m.Name) is { } p && (Known.TryGetValue(p, out var known) ? known : counts[p] >= 2 ? p : null) is { } who) guess[m.Path] = who;
        return guess;
    }

    // the maker the player set, else the guess; Guessed says which
    public static (string? Name, bool Guessed) Of(ModFile m, IReadOnlyDictionary<string, string> guesses) =>
        ModNotes.Get(m.Rel)?.Creator is { } set ? (set, false) : guesses.TryGetValue(m.Path, out var g) ? (g, true) : (null, false);
}
