using System.Text.Json;

namespace ApexSetup;

// The last play sessions (Launch's Session), newest first, in %LOCALAPPDATA%\Apex Launcher\sessions.json: Health reads
// crashes and memory peaks from them, and ScriptErrors reads the errors written since the last start.
public static class SessionHistory
{
    public const int Keep = 20;
    // TS3W.exe is a 32-bit large-address-aware process: 4 GB of address space on 64-bit Windows (a real crash log the game
    // wrote, xcpt *.txt, reports "Total virtual memory: 4095 Mb"). Where it fails below that is Session.LowMemoryBytes;
    // the community's 3.6 to 3.8 GB figure is unverified.
    public const long AddressSpaceBytes = 4096L << 20;

    static string FilePath => Path.Combine(Paths.Data, "sessions.json");

    public static List<SessionInfo> List()
    {
        try { return File.Exists(FilePath) ? JsonSerializer.Deserialize(File.ReadAllText(FilePath), SetupJson.Default.ListSessionInfo) ?? [] : []; }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or JsonException) { return []; }
    }

    // records a finished session; a normal end also drops the cache backup GameCaches.Clean kept
    public static void Add(SessionInfo s)
    {
        var list = List();
        list.Insert(0, s);
        try
        {
            Directory.CreateDirectory(Paths.Data);
            Fs.WriteAtomic(FilePath, JsonSerializer.Serialize(list.OrderByDescending(x => x.Started).Take(Keep).ToList(), SetupJson.Default.ListSessionInfo));
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { Engine.Log(e); }
        if (s.End == SessionEnd.Normal) GameCaches.Forget();
    }

    public static List<Finding> Check()
    {
        var f = new List<Finding>();
        var recent = List().Take(5).ToList();
        if (recent.Count == 0) return f;
        var low = recent.Count(s => s.End == SessionEnd.LowMemory);
        var crash = recent.Count(s => s.End == SessionEnd.Crash);
        var peak = recent.Max(s => s.PeakBytes);
        if (low > 0)
            f.Add(new(Severity.Warning, "{0} of the last {1} game sessions ran out of memory (peak {2} MB of the game's 4096 MB). Fewer mods, less CC or a smaller world lowers it.",
                [low.ToString(), recent.Count.ToString(), (peak >> 20).ToString()]));
        if (crash > 0)
            // (a crash now and then happens to the game without any mod: one is a note, two or more of the last few a look)
            f.Add(new(crash >= 2 ? Severity.Warning : Severity.Info, "{0} of the last {1} game sessions crashed.", [crash.ToString(), recent.Count.ToString()]));
        else if (low == 0 && peak >= Session.LowMemoryBytes * 9 / 10)
            f.Add(new(Severity.Info, "The game came close to its memory limit (peak {0} MB of 4096 MB).", [(peak >> 20).ToString()]));
        return f;
    }
}
