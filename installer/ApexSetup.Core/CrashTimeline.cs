namespace ApexSetup;

// Health's Stability timeline: the last game sessions, the week's crashes and the freezes Windows recorded, newest first, each
// with how it ended, what caused it and the clues that say so (CrashEvidence). A crash or freeze outside a recorded session
// (the game started without the launcher) is an entry of its own; several in a row with one cause are one entry.
public static class CrashTimeline
{
    public enum Ending { Normal, Crash, Froze, NoReport }
    // Culprit: the file's name as ScriptErrors gives it (null: the game's own code, memory or the graphics driver); Why:
    // ScriptErrors' Why, or "driver" when the graphics driver failed at that moment; Into: what a "called" crash called;
    // Replaced: when the culprit's file was replaced, for a crash fixed by an update
    public sealed record Entry(DateTime When, DateTime First, int Count, TimeSpan? Played, long Peak, Ending End, string? Culprit, string? Why, string? Into,
        bool Earlier, DateTime? Replaced, bool Renderer, IReadOnlyList<string> Files, IReadOnlyList<CrashEvidence.Clue> Clues);

    public const int Max = 8;

    public static List<Entry> Build(IReadOnlyList<SessionInfo> sessions, IReadOnlyList<ScriptErrors.Error> errors, IReadOnlyList<CrashEvidence.WinEvent> events,
        ScriptErrors.ApexCrash? apex, string docs, string? bin)
    {
        var crashes = errors.Where(e => e.Crash).OrderByDescending(e => e.When).ToList();
        var hangs = events.Where(e => e.Kind == "hang").ToList();
        var used = new HashSet<object>();
        var list = new List<Entry>();
        foreach (var s in sessions.Take(Max))
        {
            var end = s.Started + s.Played + TimeSpan.FromMinutes(5);
            var c = crashes.FirstOrDefault(e => e.When >= s.Started && e.When <= end);
            var h = hangs.FirstOrDefault(e => e.When >= s.Started && e.When <= end);
            if (c is not null) { used.Add(c); list.Add(Of(c, 1, c.When, s, events, apex, docs, bin)); }
            else if (h is not null)
            {
                used.Add(h);
                list.Add(Plain(h.When, s.Started, s, Ending.Froze, events, apex, docs, bin));
            }
            else
            {
                var at = s.Started + s.Played;
                var entry = Plain(at, s.Started, s, s.End == SessionEnd.Normal ? Ending.Normal : Ending.NoReport, events, apex, docs, bin);
                // (Apex Radiance's next start saw it end without closing, though its exit looked normal: a hang ended from the Task
                // Manager exits with 1, as a normal close can)
                if (entry.End == Ending.Normal && CrashEvidence.For(at, s.Started, null, null, [], null, docs, null) is var said
                    && said.Any(c => c.Args.Any(a => a.Contains("did not close", StringComparison.Ordinal))))
                    entry = entry with { End = Ending.NoReport, Clues = CrashEvidence.For(at, s.Started, null, apex, events, s, docs, bin) };
                list.Add(entry);
            }
        }
        // crashes and freezes of sessions the launcher did not follow, a run with one cause as one entry
        foreach (var run in Runs(crashes.Where(e => !used.Contains(e)).ToList()))
            list.Add(Of(run[0], run.Count, run[^1].When, null, events, apex, docs, bin));
        foreach (var h in hangs.Where(h => !used.Contains(h) && !list.Any(e => Math.Abs((e.When - h.When).TotalMinutes) < 2)))
            list.Add(Plain(h.When, h.When.AddMinutes(-30), null, Ending.Froze, events, apex, docs, bin));
        return list.OrderByDescending(e => e.When).Take(Max).ToList();
    }

    static IEnumerable<List<ScriptErrors.Error>> Runs(List<ScriptErrors.Error> newestFirst)
    {
        List<ScriptErrors.Error>? run = null;
        foreach (var e in newestFirst)
        {
            if (run is not null && run[0].Mod == e.Mod && run[0].Why == e.Why && run[0].Earlier == e.Earlier) { run.Add(e); continue; }
            if (run is not null) yield return run;
            run = [e];
        }
        if (run is not null) yield return run;
    }

    static bool DriverFailed(IReadOnlyList<CrashEvidence.WinEvent> events, DateTime at) => events.Any(e => e.Kind == "gpu" && Math.Abs((e.When - at).TotalSeconds) <= 90);

    static Entry Plain(DateTime at, DateTime from, SessionInfo? s, Ending end, IReadOnlyList<CrashEvidence.WinEvent> events, ScriptErrors.ApexCrash? apex, string docs, string? bin)
    {
        var clues = end == Ending.Normal ? [] : CrashEvidence.For(at, from, null, apex, events, s, docs, bin);
        var driver = end != Ending.Normal && DriverFailed(events, at);
        return new(at, at, 1, s?.Played, s?.PeakBytes ?? 0, end, null, driver ? "driver" : null, null, false, null, false, [], clues);
    }

    static Entry Of(ScriptErrors.Error c, int count, DateTime first, SessionInfo? s, IReadOnlyList<CrashEvidence.WinEvent> events, ScriptErrors.ApexCrash? apex, string docs, string? bin)
    {
        var file = c.Files.FirstOrDefault();
        DateTime? replaced = c.Earlier && file is not null && File.Exists(file) ? File.GetLastWriteTime(file) : null;
        var renderer = c.Mod is { } m && (m.Contains("(d3d9.dll)") || m.StartsWith("RTX Remix", StringComparison.Ordinal) || m.StartsWith("DXVK", StringComparison.Ordinal)
                                          || m.StartsWith("ReShade", StringComparison.Ordinal) || m.Equals("d3d9.dll", StringComparison.OrdinalIgnoreCase));
        var peak = s?.PeakBytes ?? 0;
        if (c.Why == "memory" && long.TryParse(c.Exception, out var mb)) peak = Math.Max(peak, mb << 20);
        var clues = CrashEvidence.For(c.When, s?.Started ?? c.When.AddMinutes(-30), c, apex, events, s, docs, bin);
        // (the graphics driver failing at that moment comes before a renderer or the game, which only ran into it; a mod's own fault stays the mod's)
        var why = c.Why;
        string? culprit = c.Mod;
        if (DriverFailed(events, c.When) && (renderer || c.Mod is null) && c.Why != "memory") { why = "driver"; culprit = null; renderer = false; }
        return new(c.When, first, count, s?.Played, peak, Ending.Crash, culprit, why, c.Why == "called" ? c.Exception : null, c.Earlier, replaced, renderer, c.Files, clues);
    }
}
