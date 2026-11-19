using System.ComponentModel;
using System.Diagnostics;
using System.Security.Principal;

namespace ApexSetup;

// The launcher runs as the player. Changes to the game's files open a second copy as administrator (the maintenance
// window, with the same install, repair and restore screens) only when the game folder needs it: a game the player can
// write to (a Steam library on another drive) never asks. The game is never started from the administrator copy.
public static class Elevation
{
    public const string MaintainArg = "--maintain", OpenArg = "--open";

    public static bool IsElevated
    {
        get
        {
            try { using var id = WindowsIdentity.GetCurrent(); return new WindowsPrincipal(id).IsInRole(WindowsBuiltInRole.Administrator); }
            catch (Exception) { return false; }
        }
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
    public static Process? StartMaintenance(string exe, string bin, string screen)
    {
        try
        {
            return Process.Start(new ProcessStartInfo(exe, $"{MaintainArg} {SelfUpdate.Quote(bin)} {OpenArg} {screen}")
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
