namespace ApexSetup;

// The place for checks that find problems between the player's mods (a mod that breaks another, an outdated script mod).
// A check gets the mod list once and returns findings tied to the files involved: the Mods screen marks those rows
// "Needs a look" and Health lists the findings under Mods. Add a class implementing IModCheck to All.
public interface IModCheck
{
    string Name { get; }
    IEnumerable<ModFinding> Check(IReadOnlyList<ModFile> mods, Game game, string docs);
}

// Finding as Compat writes it (an English key with {0} arguments, an optional fix and link); Files: the mods involved
public sealed record ModFinding(Finding Finding, IReadOnlyList<string> Files);

public static class ModChecks
{
    public static readonly List<IModCheck> All = [new MissingAssembly()];

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

// A script mod that references an assembly no installed package provides: an add-on without its base mod. EA's
// TuningControl.TuneAssemblies stops at the first assembly whose types do not load, so the script mods after it in load
// order never start (sims3-modernization docs/339).
public sealed class MissingAssembly : IModCheck
{
    public string Name => "missing assembly";

    // the game's and Mono's own assemblies. shortcut: told by name; a mod assembly named like them is never flagged
    static readonly System.Text.RegularExpressions.Regex Game = new(@"^(mscorlib|System(\..+)?|Mono\..+|ScriptCore|SimIFace|UI|Sims3.*)$",
        System.Text.RegularExpressions.RegexOptions.IgnoreCase);
    // (per package; read again when it changes)
    static readonly Dictionary<string, (long Size, DateTime Time, List<(string Name, string[] Refs)> Asms)> Seen = new(StringComparer.OrdinalIgnoreCase);

    public IEnumerable<ModFinding> Check(IReadOnlyList<ModFile> mods, Game game, string docs)
    {
        var found = new List<(ModFile Mod, string Name, string[] Refs)>();
        foreach (var m in mods.Where(m => m.Enabled && m.Path.EndsWith(".package", StringComparison.OrdinalIgnoreCase)))
        {
            List<(string Name, string[] Refs)> asms;
            try
            {
                var fi = new FileInfo(m.Path);
                lock (Seen)
                    if (Seen.TryGetValue(m.Path, out var seen) && seen.Size == fi.Length && seen.Time == fi.LastWriteTimeUtc) asms = seen.Asms;
                    else Seen[m.Path] = (fi.Length, fi.LastWriteTimeUtc, asms = ScriptErrors.ReferencesIn(m.Path));
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { continue; }
            found.AddRange(asms.Select(a => (m, a.Name, a.Refs)));
        }
        var provided = found.Select(a => a.Name).ToHashSet(StringComparer.OrdinalIgnoreCase);
        // one finding per package: a mod with several assemblies (Seasons Expanded has seven) is named once
        foreach (var mod in found.GroupBy(a => a.Mod))
            if (mod.SelectMany(a => a.Refs).Where(r => !Game.IsMatch(r) && !provided.Contains(r)).Distinct(StringComparer.OrdinalIgnoreCase).ToList() is { Count: > 0 } missing)
                yield return new(new(Severity.Warning, "{0} needs {1}, which no installed mod provides. Until it is there, script mods loaded after it may not start.",
                    [mod.Key.Name, string.Join(", ", missing)]), [mod.Key.Path]);
    }
}
