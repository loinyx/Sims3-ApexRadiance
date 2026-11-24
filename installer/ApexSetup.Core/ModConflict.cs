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

    // resources that only look or read differently: texts, pictures, icons, sounds and videos
    static readonly HashSet<string> LooksOnly = new(StringComparer.OrdinalIgnoreCase)
        { "STBL", "_IMG", "PNG", "ICON", "0xb1cc1af6", "0x01a527db", "0x01eef63a", "0x8070223d" };
    static readonly Regex Types = new(@"different content \(([^)]*)\)"), Type = new(@"(\w+) \d+"),
        Version = new(@"[\s_\-.]*\(?v?\.?\d+([._]\d+)*\)?$", RegexOptions.IgnoreCase);

    public static ModConflict Of(ConflictEntry c, string[] files)
    {
        // (the resource types, up to a "; n identical")
        var types = Types.Match(c.Text) is { Success: true } m ? Type.Matches(m.Groups[1].Value.Split(';')[0]).Select(x => x.Groups[1].Value).ToList() : [];
        var looks = types.Count > 0 && types.All(LooksOnly.Contains);
        var (level, title, say) = c.Kind switch
        {
            "assembly" => (ConflictLevel.Fix, "Versions of the same mod", "The game loads only one of them, and there is no telling which. Keep the newest."),
            "core mod" => (ConflictLevel.Fix, "Same part of the game replaced", "The game loads only one of them. The changes of the others are lost."),
            "native hook" => (ConflictLevel.Fix, "Same point of the game's program changed", "They can undo each other or crash the game, depending on which loads last."),
            "interaction" when c.Text.Contains("+DEBUG_", StringComparison.Ordinal) =>
                (ConflictLevel.Small, "Same menu command replaced", "It is a command of the cheats menu. One of the mods keeps it, and the rest of each mod works the same."),
            "interaction" => (ConflictLevel.Small, "Same menu command replaced", "One of the mods keeps the command, and the rest of each mod works the same."),
            _ when looks => (ConflictLevel.Small, "Same picture or text changed", c.Winner is null ? "Only one of them shows. Nothing else changes." : "The one from {0} shows. Nothing else changes."),
            _ => (ConflictLevel.Look, "Same game data changed", c.Winner is null ? "Only one of them applies there. The rest of each mod works the same." : "{0} applies there. The rest of each mod works the same."),
        };
        string[] args = say.Contains("{0}") && c.Winner is { } w ? [w] : [];
        var what = c.Kind == "interaction" ? c.Text[(c.Text.IndexOf("replace ", StringComparison.Ordinal) + 8)..].Split(' ')[0] : "";
        var id = string.Join("|", c.Parties.Select(Plain).Order(StringComparer.Ordinal).Prepend(what).Prepend(c.Kind));
        var finding = level == ConflictLevel.Small ? null
            : c.Winner is { } win ? new Finding(Severity.Warning, "{0} change the same game data. {1} likely wins and the others' changes are lost.", [string.Join(", ", c.Parties), win])
            : new Finding(Severity.Warning, title, []);
        return new(id, level, title, say, args, c.Parties, c.Winner, files, $"{c.Kind}: {c.Text}") { Finding = finding };
    }

    // a mod's name without its version ("S3DecoratingTools_V.1.2"), as two versions of it share
    static string Plain(string party) => Version.Replace(party, "") is { Length: > 0 } s ? s.ToLowerInvariant() : party.ToLowerInvariant();
}
