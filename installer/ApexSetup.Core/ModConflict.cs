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
    // the files (Labels) that hold the same copy as the one in use: they lose nothing
    public string[] Alike { get; init; } = [];
    // each mod's file as the scan names it (as Files has their paths), and the one in use: two mods can share a name
    public string[] Labels { get; init; } = [];
    public string? InUse { get; init; }
    // the mods' names, with the folder where two share one
    public string[] Names { get; init; } = Parties;
    // one conflict and no other: an Id is shared by the same conflict of two versions of a mod
    public string Key => string.Join("\n", Files.Prepend(Id));

    // resources that only look or read differently: texts, pictures, icons, sounds and videos
    static readonly HashSet<string> LooksOnly = new(StringComparer.OrdinalIgnoreCase)
        { "STBL", "_IMG", "PNG", "ICON", "0xb1cc1af6", "0x01a527db", "0x01eef63a", "0x8070223d" };
    static readonly Regex Types = new(@"different content \(([^)]*)\)"), Type = new(@"(\w+) \d+"), Texts = new(@"; (\d+) texts differ\)"), Same = new(@"; same copy as the winner: (.+)$"), Used = new(@"likely winner: ([^;]+)"),
        Version = new(@"[\s_\-.]*\(?(v\.?\s*\d+([._]\d+)*|\d+([._]\d+)+)\)?$", RegexOptions.IgnoreCase); // ("V.1.2", "_v130", "1.2": not a bare number, which is part of a name)

    public static ModConflict Of(ConflictEntry c, string[] files)
    {
        // (the resource types, up to a "; n identical")
        var types = Types.Match(c.Text) is { Success: true } m ? Type.Matches(m.Groups[1].Value.Split(';')[0]).Select(x => x.Groups[1].Value).ToList() : [];
        var looks = types.Count > 0 && types.All(LooksOnly.Contains);
        var texts = Texts.Match(c.Text) is { Success: true } n ? n.Groups[1].Value : null;
        var (level, title, say) = c.Kind switch
        {
            "assembly" => (ConflictLevel.Fix, "Two versions of the same mod", "The game loads only one, and you can't choose which. Keep the newest."),
            "core mod" => (ConflictLevel.Fix, "Mods that replace the same part of the game", "The game loads only one. The others have no effect."),
            "native hook" => (ConflictLevel.Fix, "Mods that change the same game code", "They can cancel each other or crash the game, depending on the load order."),
            "interaction" when c.Text.Contains("+DEBUG_", StringComparison.Ordinal) =>
                (ConflictLevel.Small, "Mods that replace the same menu command", "It's a cheats menu command. Only one mod keeps it. Everything else works as usual."),
            "interaction" => (ConflictLevel.Small, "Mods that replace the same menu command", "Only one mod keeps the command. Everything else works as usual."),
            // (the scan tells these only when the piece is the one in use: Parties are the piece, then the mod it is for)
            "translation" => (ConflictLevel.Small, "A translation of a mod", "{0} translates {1}. Nothing to fix."),
            "add-on" => (ConflictLevel.Small, "A mod that changes part of another", "{0} replaces part of {1} on purpose. Nothing to fix."),
            "decided" when c.Winner is not null => (ConflictLevel.Small, "One mod is set to load first", "{0} is in a folder that loads first, so the game uses it there. Everything else works as usual."),
            _ when looks && texts is not null => (ConflictLevel.Small, "Mods that change the same picture or text",
                c.Winner is null ? "Texts that differ: {0}. Only one mod's texts appear. Everything else works as usual." : "Texts that differ: {1}. The ones from {0} appear. Everything else works as usual."),
            _ when looks => (ConflictLevel.Small, "Mods that change the same picture or text", c.Winner is null ? "Only one appears. Everything else works as usual." : "The one from {0} appears. Everything else works as usual."),
            _ => (ConflictLevel.Look, "Mods that change the same game data", c.Winner is null ? "The game uses only one there. Everything else works as usual." : "The game uses {0} there. Everything else works as usual."),
        };
        var names = c.Parties.Select((p, i) => i < c.Labels.Length && c.Parties.Count(x => x.Equals(p, StringComparison.OrdinalIgnoreCase)) > 1 ? $"{p} ({Path.GetDirectoryName(c.Labels[i])})" : p).ToArray();
        var used = Used.Match(c.Text) is { Success: true } u ? u.Groups[1].Value.Trim() : null;
        var winner = Array.IndexOf(c.Labels, used) is >= 0 and var at ? names[at] : c.Winner;
        string[] args = c.Kind is "translation" or "add-on" ? names[..2]
            : say.StartsWith("Texts", StringComparison.Ordinal) ? (winner is { } first ? [first, texts!] : [texts!])
            : say.Contains("{0}") && winner is { } w ? [w] : [];
        var what = c.Kind == "interaction" ? c.Text[(c.Text.IndexOf("replace ", StringComparison.Ordinal) + 8)..].Split(' ')[0] : "";
        // (one id whatever the scan calls two mods with the same data, so one the player ignored stays ignored)
        var id = string.Join("|", c.Parties.Select(Plain).Order(StringComparer.Ordinal).Prepend(what).Prepend(c.Kind is "translation" or "add-on" or "decided" ? "resource key" : c.Kind));
        var finding = level == ConflictLevel.Small ? null
            : winner is { } win ? new Finding(Severity.Warning, "{0} change the same game data. {1} likely wins and the others' changes are lost.", [string.Join(", ", names), win])
            : new Finding(Severity.Warning, title, []);
        return new(id, level, title, say, args, c.Parties, c.Winner, files, $"{c.Kind}: {c.Text}") { Finding = finding, CanOrder = level != ConflictLevel.Fix && c.Kind != "interaction",
            Labels = c.Labels, InUse = used, Names = names, Alike = Same.Match(c.Text) is { Success: true } a ? a.Groups[1].Value.Split(" | ") : [] };
    }

    // a mod's name without its version ("S3DecoratingTools_V.1.2"), as two versions of it share
    internal static string Plain(string party) => Version.Replace(party, "") is { Length: > 0 } s ? s.ToLowerInvariant() : party.ToLowerInvariant();
}
