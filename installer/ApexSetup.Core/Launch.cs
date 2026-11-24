using System.ComponentModel;
using System.Diagnostics;

namespace ApexSetup;

// Play: the game's own exe started from Game\Bin as the player (the launcher never starts it as administrator), with
// EA's -worldFile switch when a save is chosen, as the restart host does.
public static class Launch
{
    public const int SteamAppId = 47890;

    public static string Args(string? save)
    {
        if (save is null) return "";
        if (save.Contains('"')) throw new ArgumentException("A save name with a quote cannot be passed to the game.");
        var name = save.EndsWith(".sims3", StringComparison.OrdinalIgnoreCase) ? save : save + ".sims3";
        return $"-worldFile:\"{name}\"";
    }

    public static Process Start(Game g, string? save = null)
    {
        if (Environment.GetEnvironmentVariable("APEX_SETUP_DRY_RUN") == "1") throw new InvalidOperationException("Dry run: the game is not started.");
        if (Elevation.IsElevated) throw new SetupException("This window can't start the game. Close it and press Play in Apex Launcher.");
        if (Games.IsRunning()) throw new SetupException("The Sims 3 is already running.");
        var psi = new ProcessStartInfo(g.ExePath, Args(save)) { WorkingDirectory = g.Bin, UseShellExecute = false };
        // (a Steam copy started by its exe asks Steam to start it otherwise; the app id lets it run in place)
        if (g.Edition == Edition.Steam) psi.Environment["SteamAppId"] = SteamAppId.ToString();
        try { return Process.Start(psi) ?? throw new SetupException("Windows did not start the game."); }
        catch (Win32Exception e) when (e.NativeErrorCode == 740) // (the exe asks for administrator: started through the shell)
        {
            return Process.Start(new ProcessStartInfo(g.ExePath, Args(save)) { WorkingDirectory = g.Bin, UseShellExecute = true })
                   ?? throw new SetupException("Windows did not start the game.");
        }
    }
}

public enum SessionEnd { Normal, LowMemory, Crash }
public sealed record SessionInfo(DateTime Started, TimeSpan Played, long PeakBytes, SessionEnd End, int ExitCode);

// One play session: the game process followed until it closes (also when Steam or the restart host starts it again
// under the same name), its memory sampled, and how it ended worked out from the exit code, the crash files written
// during it and the peak.
public sealed class Session
{
    // The Sims 3 is 32-bit with 4 GB of address space: a peak this close to it ends as the low-memory crash players know
    public const long LowMemoryBytes = 3_400L << 20;
    public static readonly string[] GameNames = ["TS3W", "TS3"];

    public DateTime Started { get; } = DateTime.Now;
    public long MemoryBytes { get; private set; }
    public long PeakBytes { get; private set; }
    public TimeSpan Played => DateTime.Now - Started;

    Process? _p;
    readonly string? _docs;
    // docs: the game's Documents folder, where EA's xcpt logs and Apex Radiance's crash report land
    public Session(Process p, string? docs = null) => (_p, _docs) = (p, docs);

    // samples every interval until the game is gone; progress gets each sample
    public async Task<SessionInfo> WatchAsync(IProgress<Session>? progress = null, TimeSpan? interval = null, CancellationToken ct = default)
    {
        var exit = 0;
        while (true)
        {
            while (_p is { } p && !p.HasExited)
            {
                try { p.Refresh(); MemoryBytes = p.PrivateMemorySize64; PeakBytes = Math.Max(PeakBytes, MemoryBytes); } catch (InvalidOperationException) { break; }
                progress?.Report(this);
                await Task.Delay(interval ?? TimeSpan.FromSeconds(5), ct);
            }
            try { exit = _p?.ExitCode ?? 0; } catch (InvalidOperationException) { }
            // a restart (Steam's, the launcher's or the restart host's) brings up a new process within seconds; the restart
            // host first asks the player and waits for the old bridge, so it is followed for as long as it runs
            _p = null;
            for (int i = 0; _p is null && (i < 10 || HostRunning()); i++)
            {
                _p = GameNames.SelectMany(Process.GetProcessesByName).FirstOrDefault();
                if (_p is null) await Task.Delay(TimeSpan.FromSeconds(2), ct);
            }
            if (_p is null) break;
        }
        return new SessionInfo(Started, Played, PeakBytes, Classify(exit, PeakBytes, _docs is not null && CrashFileSince(_docs, Started)), exit);
    }

    static bool HostRunning()
    {
        var host = Process.GetProcessesByName("ts3wrap");
        foreach (var p in host) p.Dispose();
        return host.Length > 0;
    }

    // a crash the exit code does not show (EA's handler, or a restart that hid it): EA's "xcpt *.txt" or Apex Radiance's
    // ApexRadiance_Crash.txt/.dmp (framework/crash_report.cpp) written since the session started
    public static bool CrashFileSince(string docs, DateTime started)
    {
        bool Newer(string f) => File.GetLastWriteTime(f) >= started;
        try
        {
            var apex = Path.Combine(docs, "Apex Radiance");
            return Directory.Exists(docs) && Directory.EnumerateFiles(docs, "xcpt *.txt").Any(Newer)
                   || new[] { "ApexRadiance_Crash.txt", "ApexRadiance_Crash.dmp" }.Select(n => Path.Combine(apex, n)).Any(f => File.Exists(f) && Newer(f));
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { Engine.Log(e); return false; }
    }

    public static SessionEnd Classify(int exitCode, long peakBytes, bool crashFile = false) =>
        (uint)exitCode >= 0xC0000000 || crashFile ? (peakBytes >= LowMemoryBytes ? SessionEnd.LowMemory : SessionEnd.Crash)
        : peakBytes >= LowMemoryBytes ? SessionEnd.LowMemory : SessionEnd.Normal;

    // the restart host (ts3wrap) asks the player itself after a low-memory save, but a crash ends it without a question
    // (docs/features/performance/restart-host.md): the launcher offers after a crash either way
    public static bool ShouldOffer(SessionInfo s, bool restartHostOn) =>
        s.End == SessionEnd.Crash || !restartHostOn && s.End == SessionEnd.LowMemory;
}
