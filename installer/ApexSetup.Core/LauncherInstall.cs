using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Runtime.InteropServices.Marshalling;
using Microsoft.Win32;

namespace ApexSetup;

// Apex Launcher installs itself for the signed-in player, without admin: the exe in %LOCALAPPDATA%\Programs\Apex Launcher,
// a Start menu and a desktop shortcut, and an entry in Windows' Installed apps whose Uninstall opens the launcher's own
// uninstall (Uninstall.cs). Everything here is per user and undone by RemoveTraces.
public static partial class LauncherInstall
{
    public const string UninstallArg = "--uninstall", TrayArg = "--tray", PortableVar = "APEX_LAUNCHER_PORTABLE";

    // settable for tests: a key under HKCU and two folders of their own
    public static string RegistryBase = @"Software\Microsoft\Windows\CurrentVersion";
    public static string StartMenuDir = Environment.GetFolderPath(Environment.SpecialFolder.Programs);
    public static string DesktopDir = Environment.GetFolderPath(Environment.SpecialFolder.DesktopDirectory);
    const string Key = "ApexLauncher", Name = "Apex Launcher";
    static string UninstallKey => RegistryBase + @"\Uninstall\" + Key;
    static string RunKey => RegistryBase + @"\Run";
    // "Open with" for mods: the launcher is offered for .package and .sims3pack files, never made their default. Settable for tests.
    public static string ClassesBase = @"Software\Classes";
    const string ProgId = "ApexLauncher.Mod";
    static readonly string[] OpenTypes = [".package", ".sims3pack"];
    // (offered by earlier builds: taken back on the next registration and on uninstall)
    static readonly string[] DroppedTypes = [];
    static string AppKey => ClassesBase + @"\Applications\ApexLauncher.exe";
    public static string StartMenuLink => Path.Combine(StartMenuDir, Name + ".lnk");
    public static string DesktopLink => Path.Combine(DesktopDir, Name + ".lnk");

    public static bool IsInstalledCopy(string exe) => string.Equals(Path.GetFullPath(exe), Path.GetFullPath(Paths.InstalledExe), StringComparison.OrdinalIgnoreCase);

    // ---------- installed or portable ----------

    // Portable is a marker file next to the exe, so a portable folder moved as a whole stays portable. An exe moved without
    // its marker is NotChosen again and the player is asked once more; its data stays in %LOCALAPPDATA% either way.
    public const string PortableMarker = "ApexLauncher.portable", HandoverArg = "--handover", ReplaceArg = "--replace";
    public static string MarkerOf(string exe) => Path.Combine(Path.GetDirectoryName(Path.GetFullPath(exe))!, PortableMarker);

    public static LauncherMode ModeOf(string exe) =>
        IsInstalledCopy(exe) ? LauncherMode.Installed
        : File.Exists(MarkerOf(exe)) || Environment.GetEnvironmentVariable(PortableVar) == "1" ? LauncherMode.Portable
        : LauncherMode.NotChosen;

    public static LauncherMode Mode => Environment.ProcessPath is { } exe ? ModeOf(exe) : LauncherMode.NotChosen;

    // What a start does before its window opens. NotChosen runs here and the window asks; it only hands over when the
    // player installed before (a downloaded or old Setup copy started again).
    public static StartStep FirstStep(string exe, bool elevated) => ModeOf(exe) switch
    {
        LauncherMode.Installed => StartStep.Register,
        _ when elevated => StartStep.RestartAsPlayer, // (the update an old, elevated Setup started)
        LauncherMode.NotChosen when File.Exists(Paths.InstalledExe) => StartStep.HandToInstalled,
        _ => StartStep.RunHere,
    };

    // Copied into the install folder, registered, and that copy started: the caller exits right after. Also the switch from
    // portable: the marker goes, the portable exe stays where it is (started again, it hands over to the installed copy).
    // folder: the install folder the player chose (null: the one it has, or the default). A copy installed elsewhere moves
    // there: the old one goes once the new one is in place.
    public static void Install(string exe, IEnumerable<string>? args = null, Action<string, IEnumerable<string>>? start = null, string? folder = null)
    {
        var (dir, before) = (Paths.InstallDir, Paths.InstalledExe);
        var (was, startup, desktop) = (File.Exists(before), RunsAtStartup(), LauncherSettings.Load().DesktopShortcut);
        if (folder is not null) Paths.SetInstallDir(folder);
        var target = Paths.InstalledExe;
        var made = !File.Exists(target);
        try
        {
            CopyIn(exe, whole: true);
            Register(target, VersionOf(target), desktop);
            if (startup) SetRunAtStartup(target, true); // (the folder it starts from)
            TryDelete(MarkerOf(exe));
            TryDelete(MarkerOf(target));
            Handover(target, args, start);
        }
        catch
        {
            // (all of it points at the copy that works again, or at none)
            try
            {
                Paths.SetInstallDir(dir);
                if (was) { Register(before, VersionOf(before), desktop); if (startup) SetRunAtStartup(before, true); }
                else RemoveTraces();
                if (made && !IsRunningFrom(target, exe)) RemoveInstall(Path.GetDirectoryName(target)!);
            }
            catch (Exception e) { Engine.Log(e); }
            throw;
        }
        if (was && !string.Equals(before, target, StringComparison.OrdinalIgnoreCase)) RemoveInstall(dir);
    }

    static bool IsRunningFrom(string target, string exe) => string.Equals(Path.GetFullPath(target), Path.GetFullPath(exe), StringComparison.OrdinalIgnoreCase);

    // ---------- the install folder ----------

    // always a folder of the launcher's own, inside the one picked
    public static string FolderFor(string picked) =>
        string.Equals(Path.GetFileName(Path.TrimEndingDirectorySeparator(picked)), Name, StringComparison.OrdinalIgnoreCase) ? Path.TrimEndingDirectorySeparator(picked) : Path.Combine(picked, Name);

    // a folder the launcher can live in and update itself in, with no administrator. Not among its data, nor inside the
    // default install folder: both are removed whole.
    public static bool CanInstallTo(string dir)
    {
        if (!Path.IsPathFullyQualified(dir) || dir.Contains('%') || string.Equals(dir, Paths.Data, StringComparison.OrdinalIgnoreCase) || Engine.Within(dir, Paths.Data, Paths.DefaultInstallDir)) return false;
        // (an administrator step runs this same exe, and it sees no network drive of the player's)
        if (dir.StartsWith(@"\\", StringComparison.Ordinal)) return false;
        try { if (new DriveInfo(Path.GetPathRoot(dir)!).DriveType == DriveType.Network) return false; } catch (ArgumentException) { return false; }
        var made = !Directory.Exists(dir);
        try
        {
            Directory.CreateDirectory(dir);
            var probe = Path.Combine(dir, Path.GetRandomFileName());
            File.WriteAllText(probe, "");
            File.Delete(probe);
            return true;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or NotSupportedException or ArgumentException) { return false; }
        finally { if (made) try { if (Directory.Exists(dir)) Directory.Delete(dir); } catch (Exception e) when (e is IOException or UnauthorizedAccessException) { } }
    }

    // The default folder is the launcher's and goes whole. In a chosen one only its own files go, and the folder if that
    // leaves it empty.
    static bool OwnsFolder(string dir) => string.Equals(dir, Paths.DefaultInstallDir, StringComparison.OrdinalIgnoreCase);
    static string[] OwnFiles(string dir) => [Path.Combine(dir, "ApexLauncher.exe"), Path.Combine(dir, "ApexLauncher.exe.old"), Path.Combine(dir, "ApexLauncher.exe.part"), Path.Combine(dir, PortableMarker)];
    static IEnumerable<string> RemoveInstallCommands(string dir) =>
        OwnsFolder(dir) ? RemoveCommands([dir]) : Directory.Exists(dir) ? [.. RemoveCommands(OwnFiles(dir)), $"rd {SelfUpdate.Quote(dir)}"] : [];

    static void RemoveInstall(string dir)
    {
        if (OwnsFolder(dir)) { RemoveNowOrAfterExit(dir); return; }
        try
        {
            foreach (var f in OwnFiles(dir)) File.Delete(f);
            if (Directory.Exists(dir) && !Directory.EnumerateFileSystemEntries(dir).Any()) Directory.Delete(dir);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { AfterExit(RemoveInstallCommands(dir)); }
    }

    // the install folder and what else goes with it, once this process has closed
    public static void DeleteInstallAfterExit(string install, params string[] more) => AfterExit([.. RemoveInstallCommands(install), .. RemoveCommands(more)]);

    // Runs portable from folder (null: where it is; the installed copy needs a folder). Shortcuts, Run and the Installed apps
    // entry go, the launcher's data stays. Moved: the new copy is started, the caller exits, and the old exe (or the install
    // folder) is removed. Returns the portable exe.
    public static string MakePortable(string exe, string? folder = null, IEnumerable<string>? args = null, Action<string, IEnumerable<string>>? start = null)
    {
        var target = folder is null ? Path.GetFullPath(exe) : Path.Combine(Path.GetFullPath(folder), Path.GetFileName(exe));
        var installed = IsInstalledCopy(exe);
        var dir = Paths.InstallDir;
        // (anywhere under the install folder, which goes once the copy is made, would take the copy with it)
        if (installed && (IsInstalledCopy(target) || Engine.Within(target, Paths.InstallDir, Paths.DefaultInstallDir))) throw new ArgumentException("A portable launcher needs a folder outside the install folder.", nameof(folder));
        var moved = !string.Equals(target, Path.GetFullPath(exe), StringComparison.OrdinalIgnoreCase);
        if (moved)
        {
            Directory.CreateDirectory(Path.GetDirectoryName(target)!);
            File.Copy(exe, target, true);
        }
        File.WriteAllText(MarkerOf(target), "Apex Launcher runs from this folder without installing. Delete this file to be asked again.\r\n");
        // (the shortcuts and Installed apps entry are the installed copy's: they go only when this is that copy, or there is none)
        if (installed || !File.Exists(Paths.InstalledExe)) RemoveTraces();
        if (!moved) return target;
        Handover(target, args, start);
        if (installed) { Paths.SetInstallDir(null); RemoveInstall(dir); }
        else { TryDelete(MarkerOf(exe)); RemoveNowOrAfterExit(exe); }
        return target;
    }

    // The arguments for a copy started while this one still holds the single-instance mutex: it waits for it instead of
    // only bringing this window up.
    public static IEnumerable<string> LastingArgs(IEnumerable<string> args) => args.Where(a => a is not (TrayArg or ReplaceArg or HandoverArg or "--was-setup") && !a.StartsWith("--moved-from=", StringComparison.Ordinal) && !File.Exists(a));

    public static IEnumerable<string> HandoverArgs(IEnumerable<string>? args) => (args ?? []).Where(a => a != HandoverArg).Append(HandoverArg);

    static void Handover(string exe, IEnumerable<string>? args, Action<string, IEnumerable<string>>? start) => (start ?? SelfUpdate.Restart)(exe, HandoverArgs(args));

    static string VersionOf(string exe) => Version.TryParse(FileVersionInfo.GetVersionInfo(exe).FileVersion, out var v) ? v.ToString(3) : "";

    static void TryDelete(string file)
    {
        try { if (File.Exists(file)) File.Delete(file); } catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
    }

    // (a running exe, or a folder holding one, cannot be deleted: it goes once this process has closed)
    static void RemoveNowOrAfterExit(string path)
    {
        try { if (Directory.Exists(path)) Directory.Delete(path, true); else if (File.Exists(path)) File.Delete(path); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { DeleteAfterExit(path); }
    }

    // The running exe copied to the install folder when that copy is missing or older (a downloaded update, or the first
    // start); true when the installed copy should be started in place of this one.
    // whole: one there that is not newer and not as large as this one (a copy cut short) is made again
    public static bool CopyIn(string exe, bool whole = false)
    {
        if (IsInstalledCopy(exe)) return false;
        var target = Paths.InstalledExe;
        if (File.Exists(target) && !Newer(exe, target) && !(whole && !Newer(target, exe) && new FileInfo(target).Length != new FileInfo(exe).Length)) return true;
        Directory.CreateDirectory(Paths.InstallDir);
        // (under another name until it is whole)
        var part = target + ".part";
        try
        {
            File.Copy(exe, part, true);
            if (File.Exists(target)) SelfUpdate.Replace(part, target); // (it may be running: renamed aside, removed at its next start)
            else File.Move(part, target);
        }
        finally { TryDelete(part); }
        return true;
    }

    public static bool Newer(string a, string b) =>
        Version.TryParse(FileVersionInfo.GetVersionInfo(a).FileVersion, out var va) && Version.TryParse(FileVersionInfo.GetVersionInfo(b).FileVersion, out var vb) && va > vb;

    // The shortcuts and the Installed apps entry, written at every start of the installed copy (the version shown follows
    // updates; a shortcut the player deleted from the desktop stays deleted only until then, as the Start menu one does)
    public static void Register(string exe, string version, bool desktop)
    {
        CreateShortcut(StartMenuLink, exe);
        if (desktop) CreateShortcut(DesktopLink, exe);
        using var k = Registry.CurrentUser.CreateSubKey(UninstallKey);
        k.SetValue("DisplayName", Name);
        k.SetValue("DisplayVersion", version);
        k.SetValue("Publisher", "loinyx");
        k.SetValue("DisplayIcon", exe);
        k.SetValue("InstallLocation", Path.GetDirectoryName(exe) ?? "");
        k.SetValue("UninstallString", $"{SelfUpdate.Quote(exe)} {UninstallArg}");
        k.SetValue("NoModify", 1, RegistryValueKind.DWord);
        k.SetValue("NoRepair", 1, RegistryValueKind.DWord);
        // (the date Installed apps shows: the first registration's, kept by every later start)
        if (k.GetValue("InstallDate") is null) k.SetValue("InstallDate", DateTime.Now.ToString("yyyyMMdd", System.Globalization.CultureInfo.InvariantCulture));
        k.SetValue("URLInfoAbout", "https://github.com/loinyx/Sims3-ApexRadiance");
        k.SetValue("HelpLink", "https://github.com/loinyx/Sims3-ApexRadiance/issues");
        try { k.SetValue("EstimatedSize", (int)(new FileInfo(exe).Length / 1024), RegistryValueKind.DWord); } catch (IOException) { }
        RegisterOpenWith(exe);
    }

    static void RegisterOpenWith(string exe)
    {
        var open = $"{SelfUpdate.Quote(exe)} \"%1\"";
        foreach (var key in new[] { ClassesBase + @"\" + ProgId, AppKey })
        {
            using var p = Registry.CurrentUser.CreateSubKey(key);
            p.SetValue("FriendlyAppName", Name);
            using (var icon = p.CreateSubKey("DefaultIcon")) icon.SetValue("", exe + ",0");
            using (var cmd = p.CreateSubKey(@"shell\open\command")) cmd.SetValue("", open);
        }
        using (var types = Registry.CurrentUser.CreateSubKey(AppKey + @"\SupportedTypes"))
            foreach (var t in OpenTypes) types.SetValue(t, "");
        foreach (var t in OpenTypes)
        {
            using var w = Registry.CurrentUser.CreateSubKey(ClassesBase + @"\" + t + @"\OpenWithProgids");
            w.SetValue(ProgId, Array.Empty<byte>(), RegistryValueKind.None);
        }
        foreach (var t in DroppedTypes)
        {
            using (var types = Registry.CurrentUser.OpenSubKey(AppKey + @"\SupportedTypes", true)) types?.DeleteValue(t, false);
            using var w = Registry.CurrentUser.OpenSubKey(ClassesBase + @"\" + t + @"\OpenWithProgids", true);
            w?.DeleteValue(ProgId, false);
        }
        SHChangeNotify(0x08000000, 0, IntPtr.Zero, IntPtr.Zero); // (Explorer reads the new "Open with" entries now)
    }

    // the launcher listed in "Open with" for every mod type it takes
    public static bool OffersOpenWith() => OpenTypes.All(t =>
    {
        using var w = Registry.CurrentUser.OpenSubKey(ClassesBase + @"\" + t + @"\OpenWithProgids");
        return w?.GetValueNames().Contains(ProgId) == true;
    });

    public static bool Registered()
    {
        using var k = Registry.CurrentUser.OpenSubKey(UninstallKey);
        return k is not null;
    }

    // Start with Windows: the installed exe, hidden in the tray until opened
    public static void SetRunAtStartup(string exe, bool on)
    {
        using var k = Registry.CurrentUser.CreateSubKey(RunKey);
        if (on) k.SetValue(Name, $"{SelfUpdate.Quote(exe)} {TrayArg}");
        else k.DeleteValue(Name, false);
    }

    public static bool RunsAtStartup()
    {
        using var k = Registry.CurrentUser.OpenSubKey(RunKey);
        return k?.GetValue(Name) is not null;
    }

    // everything Register and SetRunAtStartup wrote
    public static void RemoveTraces()
    {
        // (a key that goes between the look and the delete throws: the second look finds it gone)
        static void Drop(string key) { try { Registry.CurrentUser.DeleteSubKeyTree(key, false); } catch (IOException) { Registry.CurrentUser.DeleteSubKeyTree(key, false); } }
        foreach (var l in new[] { StartMenuLink, DesktopLink })
            try { if (File.Exists(l)) File.Delete(l); } catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
        Drop(UninstallKey);
        using (var k = Registry.CurrentUser.OpenSubKey(RunKey, true)) k?.DeleteValue(Name, false);
        Drop(ClassesBase + @"\" + ProgId);
        Drop(AppKey);
        foreach (var t in OpenTypes.Concat(DroppedTypes))
        {
            using var w = Registry.CurrentUser.OpenSubKey(ClassesBase + @"\" + t + @"\OpenWithProgids", true);
            w?.DeleteValue(ProgId, false);
        }
        SHChangeNotify(0x08000000, 0, IntPtr.Zero, IntPtr.Zero);
    }

    // Folders (the install folder, the launcher's data) and files (a portable exe) removed once this process has closed: a
    // running exe cannot delete itself. The log being written goes with them.
    public static void DeleteAfterExit(params string[] paths) => AfterExit(RemoveCommands(paths));

    static IEnumerable<string> RemoveCommands(IEnumerable<string> paths) =>
        paths.Select(p => Directory.Exists(p) ? $"rd /s /q {SelfUpdate.Quote(p)}" : File.Exists(p) ? $"del /f /q {SelfUpdate.Quote(p)}" : null).OfType<string>();

    static void AfterExit(IEnumerable<string> commands)
    {
        var rd = string.Join(" & ", commands);
        if (rd.Length == 0) return;
        // (ping waits without a console; timeout refuses to run without one, and another timeout.exe can come first on the
        // PATH, so the delete ran at once while the exe was still open)
        var ping = Path.Combine(Environment.SystemDirectory, "ping.exe"); // (unquoted: cmd /c strips the first and last quote of a line that starts with one)
        Process.Start(new ProcessStartInfo(Path.Combine(Environment.SystemDirectory, "cmd.exe"), $"/c {ping} -n 5 127.0.0.1 >nul & {rd}") { CreateNoWindow = true, UseShellExecute = false, WorkingDirectory = Environment.SystemDirectory }); // (not in the folder it removes)
    }

    // ---------- shortcuts (IShellLinkW through source-generated COM: the exe is trimmed) ----------

    public static void CreateShortcut(string lnk, string target)
    {
        Directory.CreateDirectory(Path.GetDirectoryName(lnk)!);
        var clsid = new Guid("00021401-0000-0000-C000-000000000046"); // CLSID_ShellLink
        var iid = typeof(IShellLinkW).GUID;
        Marshal.ThrowExceptionForHR(CoCreateInstance(ref clsid, IntPtr.Zero, 1 /* CLSCTX_INPROC_SERVER */, ref iid, out var ptr));
        try
        {
            var cw = new StrategyBasedComWrappers();
            var link = (IShellLinkW)cw.GetOrCreateObjectForComInstance(ptr, CreateObjectFlags.None);
            link.SetPath(target);
            link.SetWorkingDirectory(Path.GetDirectoryName(target) ?? "");
            link.SetIconLocation(target, 0);
            link.SetDescription(Name);
            ((IPersistFile)link).Save(lnk, true);
        }
        finally { Marshal.Release(ptr); }
    }

    [LibraryImport("shell32.dll")]
    private static partial void SHChangeNotify(int eventId, uint flags, IntPtr item1, IntPtr item2);

    [LibraryImport("ole32.dll")]
    private static partial int CoCreateInstance(ref Guid clsid, IntPtr outer, int context, ref Guid iid, out IntPtr obj);

    [GeneratedComInterface(StringMarshalling = StringMarshalling.Utf16), Guid("000214F9-0000-0000-C000-000000000046")]
    internal partial interface IShellLinkW
    {
        void GetPath(IntPtr file, int max, IntPtr data, uint flags);
        void GetIDList(out IntPtr pidl);
        void SetIDList(IntPtr pidl);
        void GetDescription(IntPtr name, int max);
        void SetDescription(string name);
        void GetWorkingDirectory(IntPtr dir, int max);
        void SetWorkingDirectory(string dir);
        void GetArguments(IntPtr args, int max);
        void SetArguments(string args);
        void GetHotkey(out short key);
        void SetHotkey(short key);
        void GetShowCmd(out int cmd);
        void SetShowCmd(int cmd);
        void GetIconLocation(IntPtr path, int max, out int index);
        void SetIconLocation(string path, int index);
        void SetRelativePath(string path, uint reserved);
        void Resolve(IntPtr hwnd, uint flags);
        void SetPath(string file);
    }

    [GeneratedComInterface(StringMarshalling = StringMarshalling.Utf16), Guid("0000010b-0000-0000-C000-000000000046")]
    internal partial interface IPersistFile
    {
        void GetClassID(out Guid clsid);
        [PreserveSig] int IsDirty();
        void Load(string file, uint mode);
        void Save(string file, [MarshalAs(UnmanagedType.Bool)] bool remember);
        void SaveCompleted(string file);
        void GetCurFile(out IntPtr file);
    }
}

public enum LauncherMode { Installed, Portable, NotChosen }

public enum StartStep { RunHere, Register, HandToInstalled, RestartAsPlayer }
