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
        if (Elevation.IsElevated) throw new SetupException("The game is not started from the administrator window. Close it and press Play in Apex Launcher.");
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
// under the same name), its memory sampled, and how it ended worked out from the exit code and the peak.
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
    public Session(Process p) => _p = p;

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
            // a restart (Steam's, the launcher's or the restart host's) brings up a new process within seconds
            _p = null;
            for (int i = 0; i < 10 && _p is null; i++)
            {
                _p = GameNames.SelectMany(Process.GetProcessesByName).FirstOrDefault();
                if (_p is null) await Task.Delay(TimeSpan.FromSeconds(2), ct);
            }
            if (_p is null) break;
        }
        return new SessionInfo(Started, Played, PeakBytes, Classify(exit, PeakBytes), exit);
    }

    public static SessionEnd Classify(int exitCode, long peakBytes) =>
        (uint)exitCode >= 0xC0000000 ? (peakBytes >= LowMemoryBytes ? SessionEnd.LowMemory : SessionEnd.Crash)
        : peakBytes >= LowMemoryBytes ? SessionEnd.LowMemory : SessionEnd.Normal;

    // the restart host (ts3wrap) asks the player itself after a low-memory save: the launcher stays quiet then
    public static bool ShouldOffer(SessionInfo s, bool restartHostOn) => !restartHostOn && s.End != SessionEnd.Normal;
}
