using System.Text.RegularExpressions;

namespace ApexSetup;

// Fix: the game can load only one of them or they break each other. Look: one mod loses a part of itself. Small: only a menu
// command, a picture or a text is at stake.
public enum ConflictLevel { Fix, Look, Small }

// A conflict of the scan in the player's words: what it is (Title), what it does (Say, with its arguments), and the mods in it.
// Id stays the same across a mod's versions, so one the player ignored stays ignored after an update.
public sealed record ModConflict(string Id, ConflictLevel Level, string Title, string Say, string[] SayArgs, string[] Parties, string? Winner, string[] Files, string Raw)
{
    // what counts in Health (a small one does not): the finding the Mods screen also shows on each mod
    public Finding? Finding { get; init; }
    // which mod comes first is a matter of the folder it is in (not for a menu command or a script: those don't go by it)
    public bool CanOrder { get; init; }

    // resources that only look or read differently: texts, pictures, icons, sounds and videos
    static readonly HashSet<string> LooksOnly = new(StringComparer.OrdinalIgnoreCase)
        { "STBL", "_IMG", "PNG", "ICON", "0xb1cc1af6", "0x01a527db", "0x01eef63a", "0x8070223d" };
    static readonly Regex Types = new(@"different content \(([^)]*)\)"), Type = new(@"(\w+) \d+"),
        Version = new(@"[\s_\-.]*\(?(v\.?\s*\d+([._]\d+)*|\d+([._]\d+)+)\)?$", RegexOptions.IgnoreCase); // ("V.1.2", "_v130", "1.2": not a bare number, which is part of a name)

    public static ModConflict Of(ConflictEntry c, string[] files)
    {
        // (the resource types, up to a "; n identical")
        var types = Types.Match(c.Text) is { Success: true } m ? Type.Matches(m.Groups[1].Value.Split(';')[0]).Select(x => x.Groups[1].Value).ToList() : [];
        var looks = types.Count > 0 && types.All(LooksOnly.Contains);
        var (level, title, say) = c.Kind switch
        {
            "assembly" => (ConflictLevel.Fix, "Two versions of the same mod", "The game loads only one, and you can't choose which. Keep the newest."),
            "core mod" => (ConflictLevel.Fix, "Mods that replace the same part of the game", "The game loads only one. The others have no effect."),
            "native hook" => (ConflictLevel.Fix, "Mods that change the same game code", "They can cancel each other or crash the game, depending on the load order."),
            "interaction" when c.Text.Contains("+DEBUG_", StringComparison.Ordinal) =>
                (ConflictLevel.Small, "Mods that replace the same menu command", "It's a cheats menu command. Only one mod keeps it. Everything else works as usual."),
            "interaction" => (ConflictLevel.Small, "Mods that replace the same menu command", "Only one mod keeps the command. Everything else works as usual."),
            _ when looks => (ConflictLevel.Small, "Mods that change the same picture or text", c.Winner is null ? "Only one appears. Everything else works as usual." : "The one from {0} appears. Everything else works as usual."),
            _ => (ConflictLevel.Look, "Mods that change the same game data", c.Winner is null ? "The game uses only one there. Everything else works as usual." : "The game uses {0} there. Everything else works as usual."),
        };
        string[] args = say.Contains("{0}") && c.Winner is { } w ? [w] : [];
        var what = c.Kind == "interaction" ? c.Text[(c.Text.IndexOf("replace ", StringComparison.Ordinal) + 8)..].Split(' ')[0] : "";
        var id = string.Join("|", c.Parties.Select(Plain).Order(StringComparer.Ordinal).Prepend(what).Prepend(c.Kind));
        var finding = level == ConflictLevel.Small ? null
            : c.Winner is { } win ? new Finding(Severity.Warning, "{0} change the same game data. {1} likely wins and the others' changes are lost.", [string.Join(", ", c.Parties), win])
            : new Finding(Severity.Warning, title, []);
        return new(id, level, title, say, args, c.Parties, c.Winner, files, $"{c.Kind}: {c.Text}") { Finding = finding, CanOrder = level != ConflictLevel.Fix && c.Kind != "interaction" };
    }

    // a mod's name without its version ("S3DecoratingTools_V.1.2"), as two versions of it share
    static string Plain(string party) => Version.Replace(party, "") is { Length: > 0 } s ? s.ToLowerInvariant() : party.ToLowerInvariant();
}
