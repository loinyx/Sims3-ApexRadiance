namespace ApexSetup;

// Which of some mods that change the same game data comes first: the one in a folder Resource.cfg gives a higher Priority.
// The file moves between the folders Resource.cfg already has (Overrides and Packages in the usual one); Resource.cfg stays as
// it is, and no mod is switched off or removed.
public static class ModPriority
{
    // the moves that put `first` above every other one (none when it is already there), or null when Resource.cfg has no
    // folder for it
    public static List<(ModFile Mod, string Folder)>? Moves(string docs, ModFile first, IReadOnlyList<ModFile> others)
    {
        var cfg = Path.Combine(docs, "Mods", "Resource.cfg");
        string text;
        try { text = File.ReadAllText(cfg); } catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return null; }
        var dirs = ModConflicts.ParseResourceCfg(text);
        int? At(string under) => ModConflicts.LoadKey(dirs, under.Replace('\\', '/'))?.Item1 is { } k ? -k : null; // (its Priority)
        static string Under(ModFile m) => Path.GetRelativePath("Mods", m.Rel);
        if (new[] { first }.Concat(others).Any(m => m.Locked || m.FileType != "package" || !m.Enabled || At(Under(m)) is null)) return null;
        var mine = At(Under(first))!.Value;
        var top = others.Max(m => At(Under(m))!.Value);
        if (mine > top) return [];
        // the folders Resource.cfg names
        var roots = dirs.Select(d => d.Text.Replace('\\', '/').Split('/')[0]).Where(r => r.Length > 0 && !r.Contains('*') && !r.Contains('.')).Distinct(StringComparer.OrdinalIgnoreCase).ToList();
        // where a mod would go in one: under the same subfolders, else at its top
        string? In(ModFile m, string root, Func<int, bool> ok)
        {
            var parts = Under(m).Split(Path.DirectorySeparatorChar);
            var sub = Path.Combine([root, .. parts.Skip(1).SkipLast(1)]);
            return new[] { sub, root }.FirstOrDefault(f => At(Path.Combine(f, m.Name)) is { } p && ok(p));
        }
        // where it could be (its own folder, then each other), with the others that are not below it there going down, never
        // under the game's own files: the fewest moves
        int P(string folder, ModFile m) => At(Path.Combine(folder, m.Name))!.Value;
        List<(ModFile, string)>? best = null;
        foreach (var (folder, p) in roots.Select(r => In(first, r, _ => true)).OfType<string>().Select(f => ((string?)f, P(f, first))).Prepend((null, mine)).OrderBy(x => x.Item2))
        {
            var moves = new List<(ModFile, string)>();
            if (folder is not null) { if (p == mine) continue; moves.Add((first, folder)); }
            foreach (var m in others.Where(m => At(Under(m))!.Value >= p))
                if (roots.Select(r => In(m, r, q => q < p && q >= 0)).OfType<string>().OrderByDescending(f => P(f, m)).FirstOrDefault() is { } down) moves.Add((m, down));
                else { moves = null; break; }
            if (moves is not null && (best is null || moves.Count < best.Count)) best = moves;
        }
        return best;
    }

    // the mods moved, each with the folder it came from; all or none
    public static List<(ModFile Mod, string From)> Apply(string docs, IEnumerable<(ModFile Mod, string Folder)> moves)
    {
        var done = new List<(ModFile Mod, string From)>();
        try
        {
            foreach (var (m, folder) in moves)
                done.Add((ModLibrary.Move(m, docs, folder), Path.GetDirectoryName(Path.GetRelativePath("Mods", m.Rel)) ?? ""));
        }
        catch { Back(docs, done); throw; }
        return done;
    }

    public static void Back(string docs, IEnumerable<(ModFile Mod, string From)> done)
    {
        foreach (var (m, from) in done.Reverse()) ModLibrary.Move(m, docs, from);
    }
}
