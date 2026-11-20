using System.Text.Json;
using System.Text.RegularExpressions;

namespace ApexSetup;

public sealed record ConflictEntry(string Kind, string Text, string[] Parties, string? Winner);
public sealed record OverlapEntry(string Label, string Risk, string[] Parties);
public sealed record DuplicateEntry(string[] Parties, string Text);
public sealed record ConflictResult(string Fingerprint, List<ConflictEntry> Conflicts, List<OverlapEntry> Overlaps, List<DuplicateEntry> Duplicates, bool NeedsSevenZip);

// ModConflicts' scan as a mod check. CONFLICT: a Warning tied to every party's files, with the likely winner; WARNING (two
// mods in one behaviour area, not proven): an Info for Health only; INFO duplicate copies: Duplicates. The result is kept in
// %LOCALAPPDATA%\Apex Launcher\conflicts.json with the size and time of every mod file: while none changed it is reused
// as is. A change rescans everything, since a conflict is between files. The scan runs at most Timeout; one that takes
// longer finishes in the background and is kept for the next check. Not in ModChecks.All: the Mods and Game screens run
// those on the UI thread, and a first scan of a large Mods folder takes seconds, so Health.Gather runs it when asked.
public sealed class ConflictCheck : IModCheck
{
    public string Name => "conflicts";
    public bool Offline { get; init; }                                   // no hook-table download
    public TimeSpan Timeout { get; init; } = TimeSpan.FromSeconds(60);
    public Func<ModConflicts.Options, ModConflicts.Report> Scan { get; init; } = ModConflicts.Scan; // (tests)
    public int Scans { get; private set; }                               // scans run (tests: none when the cache is reused)
    public ConflictResult? Last { get; private set; }

    static string CacheFile => Path.Combine(Paths.Data, "conflicts.json");
    static readonly object Gate = new();
    static readonly Regex Winner = new(@"likely winner: ([^;]+)");

    // the identical copies the last check found (two packages or assemblies with the same bytes: one can go)
    public List<DuplicateEntry> Duplicates => Last?.Duplicates ?? [];

    // (Rules goes up when the scan's rules change, so a result kept from older rules is scanned again)
    const int Rules = 3;
    public static string Fingerprint(IEnumerable<ModFile> mods) => $"rules {Rules}\n" +
        string.Join("\n", mods.Where(Scanned).OrderBy(m => m.Path, StringComparer.OrdinalIgnoreCase)
            .Select(m => $"{m.Path}|{m.Size}|{(File.Exists(m.Path) ? File.GetLastWriteTimeUtc(m.Path).Ticks : 0)}"));

    // what the scan reads: the switched-on packages under Mods and the ASIs
    static bool Scanned(ModFile m) => m.Enabled && (m.FileType == "asi" || m.FileType == "package" && m.Rel.StartsWith("Mods", StringComparison.OrdinalIgnoreCase));

    public ConflictResult? Result(IReadOnlyList<ModFile> mods, Game game, string docs)
    {
        var fp = Fingerprint(mods);
        if (Load() is { } cached && cached.Fingerprint == fp) return Last = cached;
        // one scan at a time per set of mods: a second check while it runs (Play checks twice) waits on that one
        Task<ConflictResult> scan;
        lock (Gate)
        {
            if (Running is { } run && run.Fp == fp && !run.Scan.IsCompleted) scan = run.Scan;
            else { scan = Start(fp, game, docs); Running = (fp, scan); }
        }
        if (!scan.Wait(Timeout)) { Engine.Log(new TimeoutException($"Mod conflict scan over {Timeout.TotalSeconds:0} s; kept for the next check when it ends")); return null; }
        return Last = scan.Result;
    }

    static (string Fp, Task<ConflictResult> Scan)? Running;

    Task<ConflictResult> Start(string fp, Game game, string docs)
    {
        Scans++;
        var o = new ModConflicts.Options
        {
            Roots = Directory.Exists(global::ApexSetup.Scan.Packages(docs)) ? [global::ApexSetup.Scan.Packages(docs)] : [],
            Bin = game.Bin, Game = game.Root,
            Hooks = ModConflicts.HookTable.Load(Offline, Path.Combine(Paths.Data, "hook-table.csv")),
        };
        var scan = Task.Run(() =>
        {
            var r = Scan(o);
            var result = new ConflictResult(fp,
                r.Conflicts.Select(c => new ConflictEntry(c.Kind, c.Text, c.Parties, Winner.Match(c.Text) is { Success: true } w ? Path.GetFileNameWithoutExtension(w.Groups[1].Value.Trim()) : null)).ToList(),
                r.Warnings.Select(w => new OverlapEntry(w.Label, w.Risk, w.Parties.Select(p => p.Name).ToArray())).ToList(),
                r.Info.Where(i => i.Text.StartsWith("duplicate copy", StringComparison.Ordinal)).Select(i => new DuplicateEntry(i.Party.Split(" + "), i.Text)).ToList(),
                r.NeedsSevenZip);
            Save(result);
            return result;
        });
        return scan;
    }

    public IEnumerable<ModFinding> Check(IReadOnlyList<ModFile> mods, Game game, string docs)
    {
        if (Result(mods, game, docs) is not { } r) return [];
        string[] FilesOf(IEnumerable<string> parties) => parties.SelectMany(p => mods.Where(m => Path.GetFileNameWithoutExtension(m.Path).Equals(p, StringComparison.OrdinalIgnoreCase)).Select(m => m.Path)).Distinct().ToArray();
        var f = new List<ModFinding>();
        // (ccmerged.package, the game's own merged store lists, is the player's choice to keep in Mods: never a conflict)
        static bool Ccmerged(string party) => party.Equals("ccmerged", StringComparison.OrdinalIgnoreCase);
        foreach (var c in r.Conflicts.Where(c => !c.Parties.Any(Ccmerged)))
            f.Add(new(c.Winner is { } w
                ? new Finding(Severity.Warning, "{0} change the same game data. {1} likely wins and the others' changes are lost.", [string.Join(", ", c.Parties), w])
                : new Finding(Severity.Warning, "{0} conflict: {1}", [string.Join(", ", c.Parties), c.Text]), FilesOf(c.Parties)));
        foreach (var o in r.Overlaps)
            f.Add(new(new Finding(Severity.Info, "{0} may overlap in {1}: {2}", [string.Join(", ", o.Parties), o.Label, o.Risk]), []));
        if (r.NeedsSevenZip)
            f.Add(new(new Finding(Severity.Info, "Some mod archives could not be read; with 7-Zip installed the conflict check reads them too.", []), []));
        return f;
    }

    static ConflictResult? Load()
    {
        try { return File.Exists(CacheFile) ? JsonSerializer.Deserialize(File.ReadAllText(CacheFile), SetupJson.Default.ConflictResult) : null; }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or JsonException) { return null; }
    }

    static void Save(ConflictResult r)
    {
        try { Directory.CreateDirectory(Paths.Data); var text = JsonSerializer.Serialize(r, SetupJson.Default.ConflictResult); lock (Gate) Fs.Retry(() => Fs.WriteAtomic(CacheFile, text)); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { Engine.Log(e); }
    }
}
