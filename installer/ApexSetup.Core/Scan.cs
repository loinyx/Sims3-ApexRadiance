using System.Diagnostics;
using System.Text;

namespace ApexSetup;

public enum Layer { Native, Dxvk, Bridge, Other }

// What one game's Game\Bin (and the Mods folder) holds right now, installed by Setup or by hand.
public sealed record State(
    string? ApexVersion,          // ApexRadiance.asi's version, null when absent
    Layer Layer,                  // what d3d9.dll is
    string? Loader,               // file name of the ASI loader found, null when none
    bool LoaderIsD3d9,            // the loader is d3d9.dll: the bridge would replace it
    bool S3ss,
    bool Ts3Native,
    bool Ts3Fixes,
    bool Ts3NativeWrongBuild,     // ts3native.log says it skipped itself: the other edition's build
    IReadOnlyList<string> Problems, // leftovers to clean by hand (English keys; the UI translates)
    Manifest Manifest)
{
    public bool Modernization => Ts3Native || Ts3Fixes;
    public string? Installed(string component) => Manifest.Versions.GetValueOrDefault(component);
}

public static class Scan
{
    // proxy names Ultimate ASI Loader and dxwrapper load as (the ones that make sense for a 32-bit D3D9 game)
    static readonly string[] LoaderNames = { "wininet.dll", "dinput8.dll", "dsound.dll", "version.dll", "winmm.dll", "winhttp.dll", "xlive.dll", "d3d9.dll", "dxwrapper.dll" };

    // APEX_SETUP_DOCUMENTS points a test run at a fake Documents folder
    public static string Documents => Environment.GetEnvironmentVariable("APEX_SETUP_DOCUMENTS")
        ?? Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.MyDocuments), "Electronic Arts", "The Sims 3");
    public static string Packages(string docs) => Path.Combine(docs, "Mods", "Packages");

    public static State Run(Game g, string? docs = null)
    {
        docs ??= Documents;
        var bin = g.Bin;
        string F(string n) => Path.Combine(bin, n);

        var apex = File.Exists(F("ApexRadiance.asi")) ? FileVersionInfo.GetVersionInfo(F("ApexRadiance.asi")).ProductVersion ?? "?" : null;
        var layer = !File.Exists(F("d3d9.dll")) ? Layer.Native
            : File.Exists(Path.Combine(bin, ".trex", "NvRemixBridge.exe")) ? Layer.Bridge
            : Contains(F("d3d9.dll"), "dxvk") ? Layer.Dxvk
            : IsLoader(F("d3d9.dll")) ? Layer.Native
            : Layer.Other;
        var loader = LoaderNames.FirstOrDefault(n => File.Exists(F(n)) && IsLoader(F(n)));

        var problems = new List<string>();
        foreach (var old in new[] { "S3SSApex.asi" })
            if (File.Exists(F(old))) problems.Add("An older combined build ({0}) is in Game\\Bin. Setup removes it.|" + old);
        foreach (var sub in new[] { "scripts", "plugins" })
            if (File.Exists(Path.Combine(bin, sub, "ApexRadiance.asi"))) problems.Add("A second ApexRadiance.asi is in Game\\Bin\\{0}. Setup removes it.|" + sub);
        var loaders = LoaderNames.Where(n => n != "dxwrapper.dll" && File.Exists(F(n)) && IsLoader(F(n))).ToList();
        if (loaders.Count > 1) problems.Add("Two ASI loaders ({0}) would load every mod twice. Keep one.|" + string.Join(", ", loaders));

        var log = F("ts3native.log");
        var wrong = File.Exists(F("ts3native.asi")) && File.Exists(log) && File.ReadAllText(log).Contains("SKIP all: host is not");

        return new State(apex, layer, loader, loader == "d3d9.dll", File.Exists(F("Sims3SettingsSetter.asi")),
            File.Exists(F("ts3native.asi")), File.Exists(Path.Combine(Packages(docs), "ts3fixes.package")), wrong,
            problems, Manifest.Load(bin));
    }

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
    static bool Contains(string file, string text)
    {
        var data = File.ReadAllBytes(file);
        var needle = Encoding.ASCII.GetBytes(text.ToLowerInvariant());
        for (int i = 0, n = data.Length - needle.Length; i <= n; i++)
        {
            int j = 0;
            while (j < needle.Length && (data[i + j] | 0x20) == needle[j]) j++;
            if (j == needle.Length) return true;
        }
        return false;
    }
}
