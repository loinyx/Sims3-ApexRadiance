namespace ApexSetup;

// Finding the mod behind a problem by halves (the 50/50 method players do by hand): each round half of the suspects is
// switched off, the player plays, and says whether the problem is still there; the suspects shrink to the half that can
// still hold it, until one is left. Every mod's state from before the hunt is kept in %LOCALAPPDATA%\Apex Launcher\hunt.txt,
// so Stop (or a crash, or a restart of the launcher) always puts every mod back exactly as it was.
public static class ModHunt
{
    public sealed record State(int Round, List<string> Suspects, List<string> Off, Dictionary<string, bool> Before);

    static string FilePath => Path.Combine(Paths.Data, "hunt.txt");

    // the hunt in progress, or null; lines: round, then "s|rel" suspects, "o|rel" switched off this round, "b|1|rel" before
    public static State? Current()
    {
        try
        {
            if (!File.Exists(FilePath)) return null;
            var lines = File.ReadAllLines(FilePath);
            if (lines.Length == 0 || !int.TryParse(lines[0], out var round)) return null;
            var s = new List<string>(); var o = new List<string>(); var b = new Dictionary<string, bool>(StringComparer.OrdinalIgnoreCase);
            foreach (var l in lines.Skip(1))
            {
                if (l.StartsWith("s|")) s.Add(l[2..]);
                else if (l.StartsWith("o|")) o.Add(l[2..]);
                else if (l.StartsWith("b|") && l.Length > 4) b[l[4..]] = l[2] == '1';
            }
            return new State(round, s, o, b);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return null; }
    }

    static void Save(State st)
    {
        Directory.CreateDirectory(Paths.Data);
        var lines = new List<string> { st.Round.ToString() };
        lines.AddRange(st.Suspects.Select(x => "s|" + x));
        lines.AddRange(st.Off.Select(x => "o|" + x));
        lines.AddRange(st.Before.Select(x => $"b|{(x.Value ? 1 : 0)}|{x.Key}"));
        var tmp = FilePath + ".tmp";
        File.WriteAllLines(tmp, lines);
        File.Move(tmp, FilePath, overwrite: true);
    }

    // the player's own packages that are on now are the suspects (the launcher's and the game folder's are not moved)
    public static State Start(IReadOnlyList<ModFile> mods, string docs, IEnumerable<string>? first = null)
    {
        Engine.CheckWritable();
        if (Current() is not null) throw new SetupException("A hunt is already going.");
        var suspects = mods.Where(m => m.FileType == "package" && !m.Locked && m.Enabled).Select(m => m.Rel).ToList();
        if (suspects.Count < 2) throw new SetupException("There are fewer than two mods to look through.");
        // (mods named by a finding go first, so they are tested in the first halves)
        if (first is not null) { var f = first.ToHashSet(StringComparer.OrdinalIgnoreCase); suspects = [.. suspects.Where(f.Contains), .. suspects.Where(x => !f.Contains(x))]; }
        var st = new State(1, suspects, [], mods.Where(m => m.FileType == "package" && !m.Locked).ToDictionary(m => m.Rel, m => m.Enabled, StringComparer.OrdinalIgnoreCase));
        return Apply(st, mods, docs);
    }

    // the player's answer: still there → it is among the mods still on; gone → among the ones switched off
    public static State? Answer(bool stillThere, IReadOnlyList<ModFile> mods, string docs)
    {
        var st = Current() ?? throw new SetupException("No hunt is going.");
        var suspects = stillThere ? st.Suspects.Except(st.Off, StringComparer.OrdinalIgnoreCase).ToList() : st.Off.ToList();
        var next = st with { Round = st.Round + 1, Suspects = suspects, Off = [] };
        return suspects.Count <= 1 ? Finish(next, mods, docs) : Apply(next, mods, docs, st);
    }

    // the one mod left (or none, when the answers did not add up)
    public static string? Found(State st) => st.Suspects.Count == 1 ? st.Suspects[0] : null;

    // rounds left at most, for the progress bar
    public static int RoundsLeft(State st) => (int)Math.Ceiling(Math.Log2(Math.Max(1, st.Suspects.Count)));

    // every mod back as it was before the hunt, the record removed
    public static void Stop(IReadOnlyList<ModFile> mods, string docs)
    {
        var st = Current();
        if (st is null) return;
        Restore(st, mods, docs);
        File.Delete(FilePath);
    }

    static State? Finish(State st, IReadOnlyList<ModFile> mods, string docs)
    {
        Save(st);
        Restore(st, mods, docs);
        return st;
    }

    public static void Forget() { try { File.Delete(FilePath); } catch (IOException) { } }

    // this round: the first half of the suspects off, everything else as it was before the hunt. A mod that can't be switched
    // (held open by another program) puts the ones already switched back and the record back to the last round (none before
    // the first), so the next answer is never read against an Off half that did not happen.
    static State Apply(State st, IReadOnlyList<ModFile> mods, string docs, State? last = null)
    {
        var off = st.Suspects.Take(st.Suspects.Count / 2).ToList();
        var next = st with { Off = off };
        Save(next);
        var offSet = off.ToHashSet(StringComparer.OrdinalIgnoreCase);
        var switched = new List<(ModFile Mod, string Path)>();
        try
        {
            foreach (var m in mods.Where(m => next.Before.ContainsKey(m.Rel)))
            {
                var want = !offSet.Contains(m.Rel) && next.Before[m.Rel];
                if (m.Enabled != want) switched.Add((m, Mods.SetEnabled(m, want, docs)));
            }
        }
        catch
        {
            foreach (var (m, path) in switched)
                try { Mods.SetEnabled(m with { Path = path, Enabled = !m.Enabled }, m.Enabled, docs); }
                catch (Exception e) when (e is IOException or UnauthorizedAccessException) { Engine.Log(e); }
            try { if (last is null) File.Delete(FilePath); else Save(last); }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { Engine.Log(e); }
            throw;
        }
        return next;
    }

    static void Restore(State st, IReadOnlyList<ModFile> mods, string docs)
    {
        foreach (var m in mods.Where(m => st.Before.ContainsKey(m.Rel)))
            if (m.Enabled != st.Before[m.Rel]) Mods.SetEnabled(m, st.Before[m.Rel], docs);
    }
}
