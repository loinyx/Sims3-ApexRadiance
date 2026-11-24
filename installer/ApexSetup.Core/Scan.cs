using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Security.Principal;
using System.Text;
using Microsoft.Win32;

namespace ApexSetup;

public enum Layer { Native, Dxvk, Bridge, Other }

// What one game's Game\Bin (and the Mods folder) holds right now, installed by Setup or by hand.
public sealed record State(
    string? ApexVersion,          // ApexRadiance.asi's version, null when absent
    Layer Layer,                  // what d3d9.dll is
    string? Loader,               // file name of the ASI loader found, null when none
    bool LoaderIsD3d9,            // the loader is d3d9.dll: the bridge would replace it
    bool S3ss,
    bool MonoPatcher,
    bool Ts3Native,
    bool Ts3Fixes,
    bool Ts3NativeWrongBuild,     // ts3native.asi is the other edition's build, or the game version has none
    bool RestartHost,             // ts3wrap.exe next to the ASI and [ui] restart_host = true (Engine.RestartHostOn)
    Manifest Manifest)
{
    public bool Modernization => Ts3Native || Ts3Fixes;
    public string? Installed(string component) => Manifest.Versions.GetValueOrDefault(component);
}

public static class Scan
{
    // proxy names Ultimate ASI Loader and dxwrapper load as (the ones that make sense for a 32-bit D3D9 game)
    static readonly string[] LoaderNames = { "wininet.dll", "dinput8.dll", "dsound.dll", "version.dll", "winmm.dll", "winhttp.dll", "xlive.dll", "d3d9.dll", "dxwrapper.dll" };

    // The game's Documents folder. APEX_SETUP_DOCUMENTS points a test run at a fake one.
    public static string DocumentsFor(Game g)
    {
        if (Environment.GetEnvironmentVariable("APEX_SETUP_DOCUMENTS") is { } test) return test;
        var ea = Path.Combine(PlayerShellFolder("Personal") ?? Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments), "Electronic Arts");
        return Path.Combine(ea, PickFolder(Folders(g.ExePath), GameLocale(g.ExePath), System.Globalization.CultureInfo.CurrentCulture.Name, f => File.Exists(Path.Combine(ea, f, "Options.ini"))));
    }

    // Earlier versions went by Windows' language alone. Apex's own folder left in that one comes along, where the right one
    // has none yet (framework/apex_paths.cpp does the same).
    public static void BringApex(Game g)
    {
        if (Environment.GetEnvironmentVariable("APEX_SETUP_DOCUMENTS") is not null) return;
        var docs = DocumentsFor(g);
        var old = Path.Combine(Path.GetDirectoryName(docs)!, WindowsFolder(Folders(g.ExePath), System.Globalization.CultureInfo.CurrentCulture.Name), "Apex Radiance");
        var now = Path.Combine(docs, "Apex Radiance");
        try
        {
            if (Directory.Exists(now) || !Directory.Exists(old)) return;
            Engine.CheckWritable();
            Directory.CreateDirectory(docs);
            Directory.Move(old, now);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or InvalidOperationException) { Engine.Log(e); }
    }

    // Sims3SettingsSetter goes by Windows' language: its file may be beside the game's folder. The one saved last is in use.
    public static string SetterFile(string docs)
    {
        var own = Path.Combine(docs, "S3SS", "S3SS.toml");
        try
        {
            return Directory.EnumerateDirectories(Path.GetDirectoryName(Path.GetFullPath(docs)) ?? docs).Select(d => Path.Combine(d, "S3SS", "S3SS.toml"))
                .Where(File.Exists).OrderByDescending(File.GetLastWriteTimeUtc).FirstOrDefault() ?? own;
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return own; }
    }

    // Setup runs elevated (app.manifest): a standard player who types an administrator's password runs it as that account, whose
    // folders the game never reads. The session's signed-in user is the player: their folder (a Shell Folders value) from their
    // own registry hive; null when Setup runs as the player.
    public static string? PlayerShellFolder(string value)
    {
        try
        {
            var player = (SecurityIdentifier)new NTAccount(Wts(7 /* WTSDomainName */), Wts(5 /* WTSUserName */)).Translate(typeof(SecurityIdentifier));
            if (player == WindowsIdentity.GetCurrent().User) return null;
            return Registry.Users.OpenSubKey($@"{player}\Software\Microsoft\Windows\CurrentVersion\Explorer\Shell Folders")?.GetValue(value) as string;
        }
        catch (Exception) { return null; } // (no signed-in user or hive: the elevated account's folder, as before)
    }

    static string Wts(int info)
    {
        if (!WTSQuerySessionInformationW(0, -1 /* this process's session */, info, out var buf, out _)) throw new InvalidOperationException();
        try { return Marshal.PtrToStringUni(buf) ?? ""; }
        finally { WTSFreeMemory(buf); }
    }

    [DllImport("wtsapi32.dll", CharSet = CharSet.Unicode)] static extern bool WTSQuerySessionInformationW(nint server, int session, int info, out nint buf, out int bytes);
    [DllImport("wtsapi32.dll")] static extern void WTSFreeMemory(nint mem);

    // The folder the game uses. Its rule (framework/apex_paths.cpp has it too): the language in its registry, English when it
    // has no such language; Windows' language only with none in the registry. One the game has played in (it holds
    // Options.ini) goes first, for editions that keep their language elsewhere.
    public static string PickFolder(IReadOnlyDictionary<string, string> folders, string? game, string windows, Func<string, bool> played)
    {
        var order = new List<string>();
        if (game is not null) order.Add(folders.GetValueOrDefault(game.ToLowerInvariant().Replace('_', '-'), "The Sims 3"));
        order.Add(WindowsFolder(folders, windows));
        order.Add("The Sims 3");
        return order.FirstOrDefault(played) ?? order[0];
    }

    static string WindowsFolder(IReadOnlyDictionary<string, string> folders, string windows)
    {
        windows = windows.ToLowerInvariant();
        return folders.GetValueOrDefault(windows) ?? folders.FirstOrDefault(f => f.Key.Split('-')[0] == windows.Split('-')[0]).Value ?? "The Sims 3";
    }

    // the first "Locale" under the game's registry key, whose name is in the exe (Steam's has its own)
    static string? GameLocale(string exe)
    {
        try
        {
            var root = _registry.GetOrAdd(exe, e => File.ReadAllBytes(e).AsSpan().IndexOf("Software\\Sims(Steam)\0"u8) >= 0 ? @"Software\Sims(Steam)" : @"Software\Sims");
            using var machine = RegistryKey.OpenBaseKey(RegistryHive.LocalMachine, RegistryView.Registry32);
            using var key = machine.OpenSubKey(root);
            foreach (var name in key?.GetSubKeyNames() ?? [])
            {
                using var k = key!.OpenSubKey(name);
                if (k?.GetValue("Locale") is string { Length: > 0 } l) return l;
            }
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or System.Security.SecurityException) { Engine.Log(e); }
        return null;
    }
    static readonly System.Collections.Concurrent.ConcurrentDictionary<string, string> _registry = new(StringComparer.OrdinalIgnoreCase);

    // Language code to folder name: the exe has one string-table block per language, the code at the block's first id and the
    // folder name two ids later (blocks of 100 ids from 1000).
    static Dictionary<string, string> Folders(string exe)
    {
        var all = new Dictionary<string, string>();
        var mod = LoadLibraryExW(exe, 0, 0x2 /* LOAD_LIBRARY_AS_DATAFILE */);
        if (mod == 0) return all;
        try
        {
            var buf = new char[260];
            for (uint block = 1000; block < 4000; block += 100)
            {
                int n = LoadStringW(mod, block, buf, 32);
                if (n <= 0) continue;
                var code = new string(buf, 0, n).ToLowerInvariant();
                n = LoadStringW(mod, block + 2, buf, buf.Length);
                if (n > 0) all.TryAdd(code, new string(buf, 0, n));
            }
            return all;
        }
        finally { FreeLibrary(mod); }
    }

    [System.Runtime.InteropServices.DllImport("kernel32.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
    static extern nint LoadLibraryExW(string path, nint file, uint flags);
    [System.Runtime.InteropServices.DllImport("kernel32.dll")] static extern bool FreeLibrary(nint mod);
    [System.Runtime.InteropServices.DllImport("user32.dll", CharSet = System.Runtime.InteropServices.CharSet.Unicode)]
    static extern int LoadStringW(nint mod, uint id, char[] buf, int max);

    // a mod ASI where the loader reads it: Game\Bin, else its scripts or plugins folder (Ultimate ASI Loader); null when absent
    public static string? FindAsi(string bin, string name) =>
        new[] { "", "scripts", "plugins" }.Select(d => Path.Combine(bin, d, name)).FirstOrDefault(File.Exists);

    public static string Packages(string docs) => Path.Combine(docs, "Mods", "Packages");

    // a log the running game keeps open for writing: read beside it
    internal static string ReadShared(string file)
    {
        using var r = new StreamReader(new FileStream(file, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete));
        return r.ReadToEnd();
    }

    public static State Run(Game g, string? docs = null)
    {
        docs ??= DocumentsFor(g);
        var bin = g.Bin;
        string F(string n) => Path.Combine(bin, n);

        var apex = File.Exists(F("ApexRadiance.asi")) ? FileVersionInfo.GetVersionInfo(F("ApexRadiance.asi")).ProductVersion ?? "?" : null;
        var layer = !File.Exists(F("d3d9.dll")) ? Layer.Native
            // the bridge client exports this (framework/platform.cpp RemixBridge); it also holds "dxvk"
            : Contains(F("d3d9.dll"), "remixapi_InitializeLibrary") ? Layer.Bridge
            : Contains(F("d3d9.dll"), "dxvk") ? Layer.Dxvk
            : IsLoader(F("d3d9.dll")) ? Layer.Native
            : Layer.Other;
        var loader = LoaderNames.FirstOrDefault(n => File.Exists(F(n)) && IsLoader(F(n)));

        var ts3native = FindAsi(bin, "ts3native.asi");
        // the ASI names the host it was built for (sims3-modernization keeps these literals stable). Its log is no
        // signal: since ts3native 0.36.240 a non-matching host logs to ts3native.launcher.log, which the Steam
        // launcher also writes on a correct install
        var built = ts3native is null ? (Edition?)null
            : Contains(ts3native, "SKIP all: host is not EA app") ? Edition.EA
            : Contains(ts3native, "SKIP all: host is not TS3W.exe") ? Edition.Steam
            : null;
        var wrong = ts3native is not null && (!g.ModernizationSupported || built is not null && built != g.Edition);

        return new State(apex, layer, loader, loader == "d3d9.dll", FindAsi(bin, "Sims3SettingsSetter.asi") is not null, FindAsi(bin, "MonoPatcher.asi") is not null,
            ts3native is not null, File.Exists(Path.Combine(Packages(docs), "ts3fixes.package")), wrong,
            Engine.RestartHostOn(bin, docs), Manifest.Load(bin));
    }

    public static List<string> Loaders(string bin) =>
        LoaderNames.Where(n => n != "dxwrapper.dll" && File.Exists(Path.Combine(bin, n)) && IsLoader(Path.Combine(bin, n))).ToList();

    static bool IsLoader(string file)
    {
        var v = FileVersionInfo.GetVersionInfo(file);
        // Ultimate ASI Loader 9.7.4: ProductName "Ultimate-ASI-Loader-Win32", FileDescription "Ultimate ASI Loader"
        var id = ((v.ProductName ?? "") + " " + (v.FileDescription ?? "")).Replace('-', ' ');
        return id.Contains("ASI Loader", StringComparison.OrdinalIgnoreCase)
               || id.Contains("dxwrapper", StringComparison.OrdinalIgnoreCase)
               || Path.GetFileName(file).Equals("dxwrapper.dll", StringComparison.OrdinalIgnoreCase);
    }

    // DXVK has no version resource; its name is in the binary (case-insensitive ASCII search)
    public static bool Contains(string file, string text) => ContainsEach(file, text)[0];

    // several strings in one read of the file (an ASI is megabytes): one answer per text, in order
    public static bool[] ContainsEach(string file, params string[] texts)
    {
        var data = File.ReadAllBytes(file);
        return texts.Select(text =>
        {
            var needle = Encoding.ASCII.GetBytes(text.ToLowerInvariant());
            for (int i = 0, n = data.Length - needle.Length; i <= n; i++)
            {
                int j = 0;
                while (j < needle.Length && (data[i + j] | 0x20) == (needle[j] | 0x20)) j++; // (both sides: '_' | 0x20 is not '_')
                if (j == needle.Length) return true;
            }
            return false;
        }).ToArray();
    }
}
