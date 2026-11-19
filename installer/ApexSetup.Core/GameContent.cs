using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Text;

namespace ApexSetup;

// Installing the game's own content (Sims3Packs of lots, worlds, households and store items) the way the game's
// launcher does it: a list file, one Sims3Pack path per line, and the game started in its installer mode, which writes
// DCCache itself. Checked on a player's game:
//   - the game deletes the list while it works, then writes one result line per entry back into it: "1 <entry>" done,
//     "0 <entry>" refused;
//   - its installer screen then waits on "Done": closing its window normally is what saves the install (a killed game
//     loses it; the pending changes sit in DCCache\dcdbt.ebc until then);
//   - hiding the window or moving it off screen stalls it or is undone by the game, so it stays as the game makes it.
// Removing does not need the game: DcCache.Remove writes the same change itself.
public static class GameContent
{
    public enum Mode { Install, Remove }
    public enum Step { Starting, Working, Finishing }
    public sealed record Result(List<string> Done, List<string> Failed);

    public static TimeSpan Longest = TimeSpan.FromMinutes(30);

    public static string ListText(IEnumerable<string> entries) => string.Concat(entries.Select(e => e.Trim() + "\n"));
    public static string Args(Mode mode, string list) => $"-cc{(mode == Mode.Install ? "install" : "uninstall")}:\"{list}\" -r:450x350 -w -nosound";

    // the 32-bit id the cache and other content name an item by: the first 8 hex digits of its whole id
    public static uint ShortId(string fullId) =>
        fullId.Length >= 10 && uint.TryParse(fullId.AsSpan(2, 8), System.Globalization.NumberStyles.HexNumber, null, out var v) ? v : 0;

    // the game's result lines: "1 <entry>" done, anything else refused; null until it has written them
    public static Result? ReadResult(string text, IReadOnlyList<string> entries)
    {
        var lines = text.Split('\n', StringSplitOptions.RemoveEmptyEntries | StringSplitOptions.TrimEntries);
        if (lines.Length == 0 || !lines.All(l => l.Length > 2 && char.IsDigit(l[0]) && l[1] == ' ')) return null;
        var done = entries.Where(e => lines.Any(l => l[0] == '1' && l[2..].Equals(e.Trim(), StringComparison.OrdinalIgnoreCase))).ToList();
        return new(done, entries.Except(done).ToList());
    }

    public static async Task<Result> InstallAsync(Game g, IReadOnlyList<string> packs, IProgress<Step>? progress = null, CancellationToken ct = default)
    {
        if (packs.Count == 0) return new([], []);
        if (Games.IsRunning()) throw new SetupException("Close The Sims 3 first.");
        var list = Path.Combine(Path.GetTempPath(), $"apex-install-{Guid.NewGuid():N}.txt");
        var text = ListText(packs);
        File.WriteAllText(list, text, new UTF8Encoding(false));
        progress?.Report(Step.Starting);
        var psi = new ProcessStartInfo(g.ExePath, Args(Mode.Install, list)) { WorkingDirectory = g.Bin, UseShellExecute = false };
        if (g.Edition == Edition.Steam) psi.Environment["SteamAppId"] = Launch.SteamAppId.ToString();
        using var p = Process.Start(psi) ?? throw new SetupException("The game did not start.");
        var started = DateTime.Now;
        Result? result = null;
        try
        {
            var working = false;
            while (!p.HasExited && result is null)
            {
                await Task.Delay(500, ct);
                if (DateTime.Now - started > Longest) throw new SetupException("The game's installer did not finish in {0} minutes.", ((int)Longest.TotalMinutes).ToString());
                string? now = null;
                try { now = File.Exists(list) ? File.ReadAllText(list) : null; } catch (IOException) { }
                if (!working && now != text) { working = true; progress?.Report(Step.Working); } // (it took the list)
                if (now is not null && now != text) result = ReadResult(now, packs);
            }
            if (!p.HasExited)
            {
                progress?.Report(Step.Finishing);
                foreach (var h in WindowsOf(p.Id)) PostMessage(h, 0x0010, IntPtr.Zero, IntPtr.Zero); // (WM_CLOSE: saves the install)
                using var wait = CancellationTokenSource.CreateLinkedTokenSource(ct);
                wait.CancelAfter(TimeSpan.FromMinutes(3));
                await p.WaitForExitAsync(wait.Token);
            }
        }
        catch (Exception) when (!p.HasExited)
        {
            // cancelled or stuck: the game saves nothing it was not closed normally for
            try { p.Kill(); p.WaitForExit(15000); } catch (Exception e) when (e is InvalidOperationException or System.ComponentModel.Win32Exception) { }
            throw;
        }
        finally { try { File.Delete(list); } catch (IOException) { } }
        return result ?? throw new SetupException("The game closed before it said what it installed.");
    }

    static List<IntPtr> WindowsOf(int pid)
    {
        var found = new List<IntPtr>();
        EnumWindows((h, _) =>
        {
            GetWindowThreadProcessId(h, out var owner);
            if (owner == pid) found.Add(h);
            return true;
        }, IntPtr.Zero);
        return found;
    }

    delegate bool EnumProc(IntPtr h, IntPtr p);
    [DllImport("user32.dll")] static extern bool EnumWindows(EnumProc f, IntPtr p);
    [DllImport("user32.dll")] static extern uint GetWindowThreadProcessId(IntPtr h, out int pid);
    [DllImport("user32.dll")] static extern bool PostMessage(IntPtr h, uint msg, IntPtr w, IntPtr l);
}
