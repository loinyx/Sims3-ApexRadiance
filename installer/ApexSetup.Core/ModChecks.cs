namespace ApexSetup;

// The place for checks that find problems between the player's mods (a mod that breaks another, an outdated script mod).
// A check gets the mod list once and returns findings tied to the files involved: the Mods screen marks those rows
// "Needs a look" and Health lists the findings under Mods. None ship yet; add a class implementing IModCheck to All.
public interface IModCheck
{
    string Name { get; }
    IEnumerable<ModFinding> Check(IReadOnlyList<ModFile> mods, Game game, string docs);
}

// Finding as Compat writes it (an English key with {0} arguments, an optional fix and link); Files: the mods involved
public sealed record ModFinding(Finding Finding, IReadOnlyList<string> Files);

public static class ModChecks
{
    public static readonly List<IModCheck> All = [];

    // every check, each on its own: one that throws is logged and the others still run
    public static List<ModFinding> Run(IReadOnlyList<ModFile> mods, Game game, string docs)
    {
        var found = new List<ModFinding>();
        foreach (var c in All)
        {
            try { found.AddRange(c.Check(mods, game, docs)); }
            catch (Exception e) { Engine.Log(new InvalidOperationException($"Mod check {c.Name} failed", e)); }
        }
        return found;
    }
}
