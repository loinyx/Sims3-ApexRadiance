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
    bool Ts3NativeWrongBuild,     // ts3native.log says it skipped itself: the other edition's build
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
        var dir = Path.Combine(ea, FolderName(g.ExePath) ?? "The Sims 3");
        // a game switched to another language keeps using the English folder (framework/apex_paths.cpp)
        return !Directory.Exists(dir) && Directory.Exists(Path.Combine(ea, "The Sims 3")) ? Path.Combine(ea, "The Sims 3") : dir;
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

    // As Apex does (framework/apex_paths.cpp GameFolderName): the exe has one string-table block per language, the
    // language code at the block's first id and the folder name two ids later (blocks of 100 ids from 1000).
    static string? FolderName(string exe)
    {
        var mod = LoadLibraryExW(exe, 0, 0x2 /* LOAD_LIBRARY_AS_DATAFILE */);
        if (mod == 0) return null;
        try
        {
            // the user locale (GetUserDefaultLocaleName, as Apex), not the display language
            var locale = System.Globalization.CultureInfo.CurrentCulture.Name.ToLowerInvariant();
            var language = locale.Split('-')[0];
            string? same = null;
            var buf = new char[260];
            for (uint block = 1000; block < 4000; block += 100)
            {
                int n = LoadStringW(mod, block, buf, 32);
                if (n <= 0) continue;
                var code = new string(buf, 0, n).ToLowerInvariant();
                n = LoadStringW(mod, block + 2, buf, buf.Length);
                if (n <= 0) continue;
                var folder = new string(buf, 0, n);
                if (code == locale) return folder;
                if (same is null && code.Split('-')[0] == language) same = folder;
            }
            return same;
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
    static string ReadShared(string file)
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
        var log = Path.ChangeExtension(ts3native ?? "", ".log"); // (written next to the ASI)
        var wrong = ts3native is not null && File.Exists(log) && ReadShared(log).Contains("SKIP all: host is not");

        return new State(apex, layer, loader, loader == "d3d9.dll", FindAsi(bin, "Sims3SettingsSetter.asi") is not null, FindAsi(bin, "MonoPatcher.asi") is not null,
            ts3native is not null, File.Exists(Path.Combine(Packages(docs), "ts3fixes.package")), wrong,
            Manifest.Load(bin));
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
    public static bool Contains(string file, string text)
    {
        var data = File.ReadAllBytes(file);
        var needle = Encoding.ASCII.GetBytes(text.ToLowerInvariant());
        for (int i = 0, n = data.Length - needle.Length; i <= n; i++)
        {
            int j = 0;
            while (j < needle.Length && (data[i + j] | 0x20) == (needle[j] | 0x20)) j++; // (both sides: '_' | 0x20 is not '_')
            if (j == needle.Length) return true;
        }
        return false;
    }
}
