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

    // The EA app copy: its TS3.exe started by anyone but the EA app asks the EA app to start the game, and the EA app starts
    // its own target, Sims3Launcher.exe -test (installerdata.xml), so the Sims 3 launcher opens. Asked through the EA app's
    // own link instead, with the launcher's -editInGameMode:true (it then starts TS3.exe at once, passing every argument on,
    // with no window of its own). The game reads that switch too (any value starting with t or 1 is on) and would load the
    // save in Edit Town with time stopped, so -apexDirect asks Apex Radiance (4.1.6 and later, apex_main.cpp) to rename it
    // before the game reads it. The EA app rewrites "-worldFile:" as "-worldfile:"" with a quote of its own, so the save
    // goes unquoted and closes with a quote, and comes first so the EA app's -test does not join its name.
    public static string EaParams(string? save)
    {
        var name = save is null ? null : Args(save)["-worldFile:\"".Length..^1];
        return name is null ? "-editInGameMode:true -apexDirect" : $"-worldFile:{name}\" -editInGameMode:true -apexDirect";
    }

    // the Apex Radiance that renames the switch for -apexDirect: an older one would leave the game in Edit Town
    public static bool CanStartDirect(string? apexVersion) => Version.TryParse(apexVersion, out var v) && v >= new Version(4, 1, 6);

    // the game's id for the EA app: the first content id in its installer data (sims3_dd for The Sims 3)
    public static string? EaContentId(Game g)
    {
        try
        {
            var xml = System.Xml.Linq.XDocument.Load(Path.Combine(g.Root, "__Installer", "installerdata.xml"));
            return xml.Descendants("contentID").Select(e => e.Value.Trim()).FirstOrDefault(v => v.Length > 0);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or System.Xml.XmlException) { return null; }
    }

    // apexVersion: the installed ApexRadiance.asi's; the EA app copy goes through the EA app only with one that can (CanStartDirect),
    // else as before (the EA app then opens the Sims 3 launcher)
    public static Process Start(Game g, string? save = null, string? apexVersion = null)
    {
        if (Environment.GetEnvironmentVariable("APEX_SETUP_DRY_RUN") == "1") throw new InvalidOperationException("Dry run: the game is not started.");
        if (Elevation.IsUacElevated) throw new SetupException("This window can't start the game. Close it and press Play in Apex Launcher.");
        if (Games.IsRunning()) throw new SetupException("The Sims 3 is already running.");
        if (g.Edition == Edition.EA && CanStartDirect(apexVersion) && EaContentId(g) is { } id) return StartThroughEaApp(id, save);
        var psi = new ProcessStartInfo(g.ExePath, Args(save)) { WorkingDirectory = g.Bin, UseShellExecute = false };
        // (a Steam copy started by its exe asks Steam to start it otherwise; the app id lets it run in place)
        if (g.Edition == Edition.Steam) psi.Environment["SteamAppId"] = SteamAppId.ToString();
        try { return Process.Start(psi) ?? throw new SetupException("Windows did not start the game. Press Play to try again."); }
        catch (Win32Exception e) when (e.NativeErrorCode == 740) // (the exe asks for administrator: started through the shell)
        {
            return Process.Start(new ProcessStartInfo(g.ExePath, Args(save)) { WorkingDirectory = g.Bin, UseShellExecute = true })
                   ?? throw new SetupException("Windows did not start the game. Press Play to try again.");
        }
    }

    // the EA app link, then the game's process once the EA app has started it (it may have to start itself and sign in first)
    static Process StartThroughEaApp(string contentId, string? save)
    {
        var link = "origin2://game/launch?offerIds=" + Uri.EscapeDataString(contentId) + "&cmdParams=" + Uri.EscapeDataString(EaParams(save));
        try { Process.Start(new ProcessStartInfo(link) { UseShellExecute = true }); }
        catch (Win32Exception) { throw new SetupException("The EA app is needed to start this copy of the game. Install or open the EA app, then press Play again."); }
        var until = DateTime.UtcNow.AddMinutes(2);
        while (DateTime.UtcNow < until)
        {
            Thread.Sleep(500);
            if (Process.GetProcessesByName("TS3").FirstOrDefault() is { } game) return game;
        }
        throw new SetupException("The EA app did not start the game. Check that you are signed in to the EA app, then press Play again.");
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
                _p = GameNames.SelectMany(Process.GetProcessesByName).FirstOrDefault(x => !IsTestRun(x));
                if (_p is null) await Task.Delay(TimeSpan.FromSeconds(2), ct);
            }
            if (_p is null) break;
        }
        return new SessionInfo(Started, Played, PeakBytes, Classify(exit, PeakBytes, _docs is not null && CrashFileSince(_docs, Started)), exit);
    }

    // Apex Radiance's own test runner (tools/ingame_runner, -ApexTestAgent:1) is no restart of the player's game: a session
    // that followed it took the runner's minutes, and its crash files, for the player's
    public static bool IsTestRun(string? commandLine) => commandLine?.Contains("-ApexTestAgent", StringComparison.OrdinalIgnoreCase) == true;
    static bool IsTestRun(Process p) => IsTestRun(CommandLineOf(p));

    // a process's command line, read with NtQueryInformationProcess (ProcessCommandLineInformation, Windows 8.1 and later);
    // null when it cannot be read
    static string? CommandLineOf(Process p)
    {
        try
        {
            var size = 0;
            NtQueryInformationProcess(p.Handle, 60, IntPtr.Zero, 0, ref size);
            if (size <= 0 || size > 1 << 20) return null;
            var buffer = System.Runtime.InteropServices.Marshal.AllocHGlobal(size);
            try
            {
                if (NtQueryInformationProcess(p.Handle, 60, buffer, size, ref size) != 0) return null;
                // a UNICODE_STRING: its length in bytes (ushort), then (aligned) the pointer to its text
                var length = (ushort)System.Runtime.InteropServices.Marshal.ReadInt16(buffer);
                var text = System.Runtime.InteropServices.Marshal.ReadIntPtr(buffer, IntPtr.Size);
                return System.Runtime.InteropServices.Marshal.PtrToStringUni(text, length / 2);
            }
            finally { System.Runtime.InteropServices.Marshal.FreeHGlobal(buffer); }
        }
        catch (Exception e) when (e is Win32Exception or InvalidOperationException or NotSupportedException) { return null; }
    }

    [System.Runtime.InteropServices.DllImport("ntdll.dll")]
    static extern int NtQueryInformationProcess(IntPtr process, int infoClass, IntPtr info, int length, ref int returned);

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
            var apex = Scan.ApexDir(docs);
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
