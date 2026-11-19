// Mod-vs-mod conflict scan from the command line (ApexSetup.Core/ModConflicts.cs). Read-only, offline.
using System.Text.Json;
using System.Text.RegularExpressions;
using ApexSetup;

string? Opt(string n) => Array.IndexOf(args, n) is var i and >= 0 && i + 1 < args.Length ? args[i + 1] : null;
var valued = new[] { "--bin", "--asm", "--game", "--rules", "--hooks" };
var folders = args.Where((a, i) => !a.StartsWith("--") && (i == 0 || !valued.Contains(args[i - 1]))).ToList();
if (args.Contains("--help") || args.Contains("-h"))
{
    Console.WriteLine("ModConflicts [<folder>...] [--mod-folders] [--describe] [--json] [--bin <Game\\Bin>] [--asm <game dll folder>] [--game <install folder>] [--rules <json>] [--hooks <csv>]");
    return 0;
}
// the detected game (its script assemblies); with no folder also its Mods folder and Game\Bin
var named = folders.Count > 0;
var game = Opt("--game") is { } gp ? Games.FromFolder(gp) : Games.Detect().FirstOrDefault();
if (folders.Count == 0 && game is not null) folders.Add(Path.Combine(Scan.DocumentsFor(game), "Mods"));
folders = folders.Where(Directory.Exists).ToList();
if (folders.Count == 0) { Console.Error.WriteLine("No Mods folder found: pass one."); return 2; }
// --hooks: known hook tables, lines "<asi file name regex>,<hex address>"
var hooks = Opt("--hooks") is { } h
    ? File.ReadLines(h).Select(l => l.Split(',')).Where(c => c.Length >= 2 && !c[0].StartsWith('#'))
        .GroupBy(c => c[0].Trim()).Select(g => new ModConflicts.KnownAsi(g.Key, new Regex(g.Key, RegexOptions.IgnoreCase),
            g.Select(c => Convert.ToUInt32(c[1].Trim(), 16)).ToHashSet(), Path.GetFileName(h))).ToList()
    : new();
var report = ModConflicts.Scan(new ModConflicts.Options
{
    Roots = folders,
    Bin = Opt("--bin") ?? (named ? null : game?.Bin),
    AsmDir = Opt("--asm"),
    Game = game?.Root,
    Rules = ModConflicts.Rules.Load(Opt("--rules")),
    Describe = args.Contains("--describe"),
    ModFolders = args.Contains("--mod-folders"),
    KnownAsi = hooks,
});
Console.WriteLine(args.Contains("--json") ? JsonSerializer.Serialize(report, new JsonSerializerOptions { WriteIndented = true }) : ModConflicts.Render(report));
return report.Conflicts.Count > 0 ? 1 : 0;
