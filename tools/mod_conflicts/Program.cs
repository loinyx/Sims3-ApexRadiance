// Mod-vs-mod conflict scan from the command line (ApexSetup.Core/ModConflicts.cs). Read-only; the only network access is
// the hook table download (off with --offline).
using System.Diagnostics;
using System.Text.Json;
using ApexSetup;

string? Opt(string n) => Array.IndexOf(args, n) is var i and >= 0 && i + 1 < args.Length ? args[i + 1] : null;
var valued = new[] { "--bin", "--asm", "--game", "--rules", "--hook-table" };
var folders = args.Where((a, i) => !a.StartsWith("--") && (i == 0 || !valued.Contains(args[i - 1]))).ToList();
if (args.Contains("--help") || args.Contains("-h"))
{
    Console.WriteLine("ModConflicts [<folder>...] [--mod-folders] [--describe] [--json] [--offline] [--bin <Game\\Bin>] [--asm <game dll folder>] [--game <install folder>] [--rules <json>] [--hook-table <csv>]");
    return 0;
}
// the detected game (its script assemblies and edition); with no folder also its Mods folder and Game\Bin
var named = folders.Count > 0;
var game = Opt("--game") is { } gp ? Games.FromFolder(gp) : Games.Detect().FirstOrDefault();
if (folders.Count == 0 && game is not null) folders.Add(Path.Combine(Scan.DocumentsFor(game), "Mods"));
folders = folders.Where(Directory.Exists).ToList();
if (folders.Count == 0) { Console.Error.WriteLine("No Mods folder found: pass one."); return 2; }
var hooks = Opt("--hook-table") is { } tableFile ? ModConflicts.HookTable.Parse(File.ReadAllText(tableFile), tableFile) : ModConflicts.HookTable.Load(args.Contains("--offline"),
    Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData), "Apex Radiance Setup", "hook-table.csv"));

ModConflicts.Report Run() => ModConflicts.Scan(new ModConflicts.Options
{
    Roots = folders,
    Bin = Opt("--bin") ?? (named ? null : game?.Bin),
    AsmDir = Opt("--asm"),
    Game = game?.Root,
    Rules = ModConflicts.Rules.Load(Opt("--rules")),
    Describe = args.Contains("--describe"),
    ModFolders = args.Contains("--mod-folders"),
    Hooks = hooks,
});
void Print(ModConflicts.Report r) =>
    Console.WriteLine(args.Contains("--json") ? JsonSerializer.Serialize(r, new JsonSerializerOptions { WriteIndented = true }) : ModConflicts.Render(r));

var report = Run();
Print(report);
// archives tar.exe cannot read: offer 7-Zip, installed only on the player's yes; a non-interactive run keeps the command in the report
if (report.NeedsSevenZip && !args.Contains("--json") && !Console.IsInputRedirected && Environment.UserInteractive)
{
    Console.Write($"\nSome archives need 7-Zip. Install it now with \"{ModConflicts.SevenZipInstall}\"? [y/N] ");
    if (Console.ReadLine()?.Trim().ToLowerInvariant() is "y" or "yes")
    {
        var psi = new ProcessStartInfo("winget") { UseShellExecute = false };
        foreach (var a in ModConflicts.SevenZipInstall.Split(' ').Skip(1)) psi.ArgumentList.Add(a);
        try
        {
            using var p = Process.Start(psi)!;
            p.WaitForExit();
            if (p.ExitCode == 0) { Console.WriteLine(); report = Run(); Print(report); }
            else Console.WriteLine($"winget ended with {p.ExitCode}; the report above stands.");
        }
        catch (System.ComponentModel.Win32Exception) { Console.WriteLine("winget is not available; install 7-Zip from https://www.7-zip.org and scan again."); }
    }
}
return report.Conflicts.Count > 0 ? 1 : 0;
