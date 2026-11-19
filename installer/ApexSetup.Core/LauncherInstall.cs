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
    public static string StartMenuLink => Path.Combine(StartMenuDir, Name + ".lnk");
    public static string DesktopLink => Path.Combine(DesktopDir, Name + ".lnk");

    public static bool IsInstalledCopy(string exe) => string.Equals(Path.GetFullPath(exe), Path.GetFullPath(Paths.InstalledExe), StringComparison.OrdinalIgnoreCase);

    // The running exe copied to the install folder when that copy is missing or older (a downloaded update, or the first
    // start); true when the installed copy should be started in place of this one.
    public static bool CopyIn(string exe)
    {
        if (IsInstalledCopy(exe)) return false;
        var target = Paths.InstalledExe;
        if (File.Exists(target) && !Newer(exe, target)) return true;
        Directory.CreateDirectory(Paths.InstallDir);
        if (File.Exists(target)) SelfUpdate.Replace(exe, target); // (it may be running: renamed aside, removed at its next start)
        else File.Copy(exe, target);
        return true;
    }

    static bool Newer(string a, string b) =>
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
        try { k.SetValue("EstimatedSize", (int)(new FileInfo(exe).Length / 1024), RegistryValueKind.DWord); } catch (IOException) { }
    }

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
        foreach (var l in new[] { StartMenuLink, DesktopLink })
            try { if (File.Exists(l)) File.Delete(l); } catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
        Registry.CurrentUser.DeleteSubKeyTree(UninstallKey, false);
        using var k = Registry.CurrentUser.OpenSubKey(RunKey, true);
        k?.DeleteValue(Name, false);
    }

    // The install folder and the launcher's data removed once this process has closed: a running exe cannot delete itself.
    // The log being written goes with them.
    public static void DeleteAfterExit(params string[] dirs)
    {
        var rd = string.Join(" & ", dirs.Where(Directory.Exists).Select(d => $"rd /s /q {SelfUpdate.Quote(d)}"));
        if (rd.Length == 0) return;
        Process.Start(new ProcessStartInfo("cmd.exe", $"/c timeout /t 3 /nobreak >nul & {rd}") { CreateNoWindow = true, UseShellExecute = false });
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
