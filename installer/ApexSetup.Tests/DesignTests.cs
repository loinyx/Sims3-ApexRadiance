using System.Text.RegularExpressions;

// The design system's rules, read from the launcher's own source (docs/launcher-design.md). A rule at 0 allows none. The others
// count what was there before the rule: the number may only go down, so a new screen takes its measures and parts from
// ApexSetup/Design. Lower it here when a screen is cleaned.
static class DesignTests
{
    static readonly (string Name, string Pattern, int Most)[] Rules =
    [
        ("an icon that is not Lucide", @"\b(FontIcon|SymbolIcon|BitmapIcon)\b|&#x[EF][0-9A-F]{3}", 0),
        ("Windows' Transparent, a clear white that flashes as it fades (use ApexClearBrush)", @"new (Microsoft\.UI\.Xaml\.Media\.)?SolidColorBrush\(Microsoft\.UI\.Colors\.Transparent\)", 0),
        ("a text size off the scale (Design.TextSize)", @"FontSize ?= ?""?(?!(11|12|13|14|16|20|24|28|32|40)\b)\d+", 6),
        ("a corner off the scale (Design.Radius)", @"CornerRadius(\(|="")(?!(0|1|1\.5|2|4|6|8|10|12|16)\b)\d+", 20),
        ("a colour of its own (Design/Themes.cs)", @"""#[0-9A-Fa-f]{6,8}""|Color\.FromArgb\((0x)?[0-9A-F]|\bPick\(0x|\bRgb\(0x", 155),
    ];

    public static void Run(Action<bool, string> check)
    {
        var dir = AppContext.BaseDirectory;
        while (dir is not null && !File.Exists(Path.Combine(dir, "ApexSetup", "Design", "Tokens.cs"))) dir = Path.GetDirectoryName(dir);
        if (dir is null) return; // (run away from the source)
        var files = Directory.EnumerateFiles(Path.Combine(dir, "ApexSetup"), "MainWindow*")
            .Where(f => f.EndsWith(".cs", StringComparison.Ordinal) || f.EndsWith(".xaml", StringComparison.Ordinal)).Select(f => (Name: Path.GetFileName(f), Lines: File.ReadAllLines(f))).ToList();
        check(files.Count > 10, "design: the screens' source is read");
        foreach (var (name, pattern, most) in Rules)
        {
            var found = files.SelectMany(f => f.Lines.Select((l, i) => (f.Name, Line: i + 1, Count: l.TrimStart().StartsWith("//", StringComparison.Ordinal) ? 0 : Regex.Matches(l, pattern).Count))).Where(x => x.Count > 0).ToList();
            var n = found.Sum(x => x.Count);
            check(n <= most, $"design: {name}: {n} of at most {most}" + (n > most ? ", in " + string.Join(", ", found.TakeLast(12).Select(x => $"{x.Name}:{x.Line}")) : ""));
        }
    }
}
