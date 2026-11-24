using System.ComponentModel;
using System.Diagnostics;
using System.Security.Principal;

namespace ApexSetup;

// The launcher runs as the player. Changes to the game's files open a second copy as administrator (the maintenance
// window, with the same install, repair and restore screens) only when the game folder needs it: a game the player can
// write to (a Steam library on another drive) never asks. The game is never started from the administrator copy.
public static class Elevation
{
    public const string MaintainArg = "--maintain", OpenArg = "--open", ChoicesArg = "--choices";

    public static bool IsElevated
    {
        get
        {
            try { using var id = WindowsIdentity.GetCurrent(); return new WindowsPrincipal(id).IsInRole(WindowsBuiltInRole.Administrator); }
            catch (Exception) { return false; }
        }
    }

    // Raised by a UAC prompt (a full token split from the player's limited one), not merely an administrator: with UAC off, or
    // on the built-in Administrator account, every process holds admin rights and is the player's own, so the restart as the
    // player and the refusal to start the game must not apply (they looped and refused Play forever there)
    public static bool IsUacElevated
    {
        get
        {
            try
            {
                using var id = WindowsIdentity.GetCurrent();
                return GetTokenInformation(id.Token, 18 /* TokenElevationType */, out var type, sizeof(int), out _) && type == 2 /* TokenElevationTypeFull */;
            }
            catch (Exception) { return false; }
        }
    }

    [System.Runtime.InteropServices.DllImport("advapi32.dll", SetLastError = true)]
    static extern bool GetTokenInformation(IntPtr token, int infoClass, out int info, int length, out int returned);

    // (a restart as the player that lands elevated again, within half a minute, runs here instead of restarting once more)
    public static bool RestartedRecently()
    {
        var stamp = Path.Combine(Paths.Ops, "restart-as-player.stamp");
        try
        {
            if (File.Exists(stamp) && DateTime.Now - File.GetLastWriteTime(stamp) < TimeSpan.FromSeconds(30)) return true;
            Directory.CreateDirectory(Paths.Ops);
            File.WriteAllText(stamp, "");
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
        return false;
    }

    // a file created and deleted in Game\Bin and its backup folder: Program Files says no to a player without admin rights.
    // The manifest turns UAC's file virtualisation off, so the answer is the real one.
    public static bool CanWrite(string bin)
    {
        foreach (var dir in new[] { bin, Manifest.DirOf(bin) })
        {
            if (!Directory.Exists(dir)) continue;
            try
            {
                using var _ = new FileStream(Path.Combine(dir, ".apex-write-test-" + Guid.NewGuid().ToString("N")), FileMode.CreateNew,
                    FileAccess.Write, FileShare.None, 1, FileOptions.DeleteOnClose);
            }
            catch (Exception e) when (e is UnauthorizedAccessException or IOException) { return false; }
        }
        return true;
    }

    // The same exe as administrator, opened on the game and screen given; null when the player said no to Windows.
    // choices: a file with what the player chose in the launcher, applied there at once (Choices)
    public static Process? StartMaintenance(string exe, string bin, string screen, string? choices = null)
    {
        try
        {
            var more = choices is null ? "" : $" {ChoicesArg} {SelfUpdate.Quote(choices)}";
            return Process.Start(new ProcessStartInfo(exe, $"{MaintainArg} {SelfUpdate.Quote(bin)} {OpenArg} {screen}{more}")
                { UseShellExecute = true, Verb = "runas" });
        }
        catch (Win32Exception e) when (e.NativeErrorCode == 1223) { return null; } // (the UAC prompt cancelled)
    }

    // (--maintain <bin> --open <screen>): the game and the screen the administrator copy opens on
    public static (string? Bin, string? Screen) Parse(IReadOnlyList<string> args)
    {
        string? bin = null, screen = null;
        for (int i = 0; i + 1 < args.Count; i++)
        {
            if (args[i] == MaintainArg) bin = args[i + 1];
            else if (args[i] == OpenArg) screen = args[i + 1];
        }
        return (bin, screen);
    }

    // The choices a launcher passed (key=value lines, read once: the file is deleted); empty without one. Only ever
    // the same choices the screen itself offers, so a changed file can ask for nothing the player could not click.
    public static Dictionary<string, string> Choices(IReadOnlyList<string> args)
    {
        var d = new Dictionary<string, string>();
        for (int i = 0; i + 1 < args.Count; i++)
        {
            if (args[i] != ChoicesArg || !File.Exists(args[i + 1])) continue;
            try
            {
                foreach (var l in File.ReadAllLines(args[i + 1]))
                    if (l.IndexOf('=') is > 0 and var k) d[l[..k]] = l[(k + 1)..];
                File.Delete(args[i + 1]);
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
        }
        return d;
    }

    public static string SaveChoices(IEnumerable<KeyValuePair<string, string>> choices)
    {
        Directory.CreateDirectory(Paths.Ops);
        var f = Path.Combine(Paths.Ops, $"choices-{Guid.NewGuid():N}.txt");
        File.WriteAllLines(f, choices.Select(c => $"{c.Key}={c.Value}"));
        return f;
    }

    // An administrator start of a launcher that is not installed yet (the update an old, elevated Setup started): opened
    // again through Explorer, which starts it as the signed-in player, so the install, shortcuts and settings are theirs.
    public static bool RestartAsPlayer(string exe)
    {
        try
        {
            // (Explorer passes no arguments: they are only a game folder, remembered anyway)
            Process.Start(new ProcessStartInfo("explorer.exe", SelfUpdate.Quote(exe)) { UseShellExecute = false });
            return true;
        }
        catch (Exception e) when (e is Win32Exception or IOException) { return false; }
    }
}
