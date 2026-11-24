namespace ApexSetup;

public enum HealthState { Ready, NeedsYou, Blocked }
// Saves: each save's size, growth, backups and completeness; Disk: where the game and Documents live, free space and the
// game caches; Stability: crashes, memory peaks and script errors of the last sessions
public enum HealthArea { ApexRadiance, Loaders, Graphics, GameSettings, Mods, Saves, Disk, Stability }

// What Health and Home show: every finding (Compat's, Verify's, the mod checks') grouped by area, and one state for the
// crystal: blocked when Apex Radiance cannot run and nothing fixes it, needs you when something waits for the player.
public static class Health
{
    public sealed record Item(Finding Finding, HealthArea Area, IReadOnlyList<string> Files)
    {
        // a mod conflict (it has its own cards and its own Ignore)
        public bool Conflict { get; init; }
        // the same finding at the next check, whatever its counts and dates: one the player ignored stays ignored. Its files
        // count: the same words about another file are another finding.
        public string Id => string.Join("|", Finding.Args.Select(a => int.TryParse(a, out _) || DateTime.TryParse(a, out _) ? "#" : a)
            .Prepend(Finding.Text.Replace("at least ", "", StringComparison.Ordinal)).Prepend(Area.ToString()).Concat(Files.Select(f => Path.GetFileName(f)).Order(StringComparer.OrdinalIgnoreCase)));
    }

    // conflicts: ModConflicts' scan (seconds on a cold cache: call Gather off the UI thread when passing it).
    // beforePlay: only Verify's and Compat's findings, the ones that decide a start (every StopsApex item is one of them)
    public static List<Item> Gather(Game g, State s, string docs, Release? apex, IReadOnlyList<ModFile>? mods = null, ConflictCheck? conflicts = null, bool beforePlay = false)
    {
        var items = new List<Item>();
        // each analyzer on its own: one that throws is logged and the others still run
        void Add(string name, Func<IEnumerable<Item>> run)
        {
            try { items.AddRange(run().ToList()); }
            catch (Exception e) { Engine.Log(new InvalidOperationException($"Health check {name} failed", e)); }
        }
        Add("files", () => Verify.Files(g, s, apex).Select(f => new Item(f, AreaOf(f), [])));
        Add("compatibility", () => Compat.Check(g, s, docs).Select(f => new Item(f, AreaOf(f), [])));
        if (beforePlay) return items;
        if (mods is not null) Add("mods", () => ModChecks.Run(mods, g, docs).Select(m => new Item(m.Finding, HealthArea.Mods, m.Files)));
        if (mods is not null && conflicts is not null) Add("conflicts", () => conflicts.Check(mods, g, docs).Select(m => new Item(m.Finding, HealthArea.Mods, m.Files) { Conflict = m.Files.Count > 0 }));
        Add("merges", () => ModMerge.Lost(docs).Select(x => new Item(new Finding(Severity.Warning,
            "Part of {1} is missing from the merge {0}. Split it to get every mod back.", [Path.GetFileNameWithoutExtension(x.Merged.Name), string.Join(", ", x.Mods.Take(4)) + (x.Mods.Count > 4 ? "…" : "")],
            () => ModMerge.Split(docs, x.Merged), Suggest: "Split"), HealthArea.Mods, [x.Merged.Path])));
        Add("kept mods", () => ModMerge.Orphans(docs).Select(o => new Item(new Finding(Severity.Warning,
            o.Files.Count == 1 ? "1 mod is out of the game because the merge {1} is gone. Put it back to use it again." : "{0} mods are out of the game because the merge {1} is gone. Put them back to use them again.",
            [o.Files.Count.ToString(), o.Name], () => ModMerge.PutBack(docs, o), Suggest: "Put back"), HealthArea.Mods, [])));
        Add("saves", () => SaveHealth.Findings(SaveHealth.Check(docs), docs).Select(f => new Item(f, HealthArea.Saves, [])));
        Add("places", () => PlaceChecks.Check(g, docs).Select(f => new Item(f, HealthArea.Disk, [])));
        Add("caches", () => CacheFindings(docs).Select(f => new Item(f, HealthArea.Disk, [])));
        Add("sessions", () => SessionHistory.Check().Select(f => new Item(f, HealthArea.Stability, [])));
        // the script errors since the last game start, at most a week back (a game started without the launcher records no
        // start, so the last recorded one can be long ago); Read takes the crash logs of the whole week either way
        var week = DateTime.Now.AddDays(-7);
        // (one read for both: the crash logs of the week come with it, which is what the timeline needs)
        ScriptErrors.Report? report = null;
        Add("script errors", () => ScriptErrors.Findings(report = ScriptErrors.Read(docs, SessionHistory.List().FirstOrDefault()?.Started is { } s && s > week ? s : week, mods))
            .Select(m => new Item(m.Finding, HealthArea.Stability, m.Files)));
        try
        {
            report ??= ScriptErrors.Read(docs, week, mods);
            Timeline = CrashTimeline.Build(SessionHistory.List(), report.Errors, CrashEvidence.Windows(week), ScriptErrors.ApexReport(docs), docs, g.Bin);
        }
        catch (Exception e) { Engine.Log(new InvalidOperationException("Health check timeline failed", e)); }
        return items;
    }

    // the last Gather's Stability timeline: sessions, crashes and freezes with their causes and clues
    public static List<CrashTimeline.Entry> Timeline { get; private set; } = [];

    // over CacheSuggestBytes, or after a crash: clearing is offered (Apply), never run by Install or Repair; a kept backup can go back
    public const long CacheSuggestBytes = 2L << 30;
    static IEnumerable<Finding> CacheFindings(string docs)
    {
        var bytes = GameCaches.List(docs).Sum(c => c.Size);
        var crashed = SessionHistory.List().FirstOrDefault() is { End: not SessionEnd.Normal };
        if (bytes > 0 && (bytes >= CacheSuggestBytes || crashed))
            yield return new(Severity.Info, crashed ? "The game's caches take {0} MB, and the game didn't close normally last time. Clearing them often helps. The game rebuilds them, and a copy is kept until it starts normally."
                    : "The game's caches take {0} MB. Clearing them frees that space. The game rebuilds them, and a copy is kept until it starts normally.",
                [(bytes >> 20).ToString()], () => GameCaches.Clean(docs), Suggest: "Clear the game caches");
        if (GameCaches.Backup is not null)
            yield return new(Severity.Info, "A copy of the cleared game caches is kept until the game starts and closes normally.", [], () => GameCaches.Restore(), Suggest: "Put the caches back");
    }

    public static HealthState StateOf(IEnumerable<Item> items)
    {
        var list = items.ToList();
        if (list.Any(StopsApex)) return HealthState.Blocked;
        return list.Any(i => i.Finding.Severity != Severity.Info) ? HealthState.NeedsYou : HealthState.Ready;
    }

    // An error nothing fixes that keeps Apex Radiance from loading: Apex itself or the ASI loader. The other unfixable
    // errors (a bridge file missing, Modernization's ts3native of the wrong build) leave Apex running: they need the player.
    // shortcut: Modernization's findings told from the loader's by their text, as AreaOf does; an Area on Finding ends both
    public static bool StopsApex(Item i) => i.Finding.Severity == Severity.Error && !i.Finding.CanFix
        && (i.Area == HealthArea.ApexRadiance || i.Area == HealthArea.Loaders && !i.Finding.Text.Contains("Modernization", StringComparison.OrdinalIgnoreCase));

    // shortcut: the area read from the finding's English key; a finding that should land elsewhere needs an Area on Finding
    public static HealthArea AreaOf(Finding f)
    {
        var t = f.Text;
        bool Has(params string[] w) => w.Any(x => t.Contains(x, StringComparison.OrdinalIgnoreCase));
        if (Has("bridge", "DXVK", "dxvk.conf", "d3d9", "Vulkan")) return HealthArea.Graphics;
        if (Has("ASI loader", "loaders", "ts3native", "Modernization")) return HealthArea.Loaders;
        if (Has("Options.ini", "Resource.cfg", "S3SS", "Sims3SettingsSetter", "Sims3Performance", "edgesmoothing")) return HealthArea.GameSettings;
        if (Has("Apex Radiance", "Apex inside")) return HealthArea.ApexRadiance;
        return HealthArea.Mods;
    }
}
