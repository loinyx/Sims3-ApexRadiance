using System.Globalization;
using System.Text.RegularExpressions;

namespace ApexSetup;

public enum CardState { Unknown, Recognized, NotRecognized, Added }

// Whether the game knows the graphics card, from the DeviceConfig.log it writes at each start: the card's line in
// GraphicsCards.sgr ("Found: 1"), and its video memory (32 MB when the driver reports none). The fix adds the card under its
// vendor and raises that fallback; the log shows it the next time the game starts. DXVK and the RTX Remix bridge present a
// card the game knows, so this is about the game's own Direct3D 9.
public sealed record DeviceCard(string Name, int Vendor, int Device, bool Found, int MemoryMb, bool MemoryGuessed)
{
    const int FallbackMb = 1024, LowMb = 512; // (the rules' highest step is at 368 MB)
    static string CardsFile(string bin) => Path.Combine(bin, "GraphicsCards.sgr");

    public static DeviceCard? Read(string docs)
    {
        var log = Path.Combine(docs, "DeviceConfig.log");
        try { return File.Exists(log) ? Parse(GameSettings.ReadText(log)) : null; }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { Engine.Log(e); return null; }
    }

    public static DeviceCard? Parse(string log)
    {
        var chip = Regex.Match(log, @"^Chipset:\s+Vendor:\s*([0-9a-fA-F]{4}),\s*Device:\s*([0-9a-fA-F]{4})", RegexOptions.Multiline);
        var found = Regex.Match(log, @"^Name \(database\):.*\[Found:\s*(\d)", RegexOptions.Multiline);
        if (!chip.Success || !found.Success) return null;
        var mem = Regex.Match(log, @"^Texture memory:\s*(\d{1,9})\s*MB([^\n]*OVERRIDE)?", RegexOptions.Multiline);
        return new(Regex.Match(log, @"^Name \(driver\):[ \t]*([^\r\n]*)", RegexOptions.Multiline).Groups[1].Value.Trim(),
            int.Parse(chip.Groups[1].Value, NumberStyles.HexNumber), int.Parse(chip.Groups[2].Value, NumberStyles.HexNumber),
            found.Groups[1].Value == "1", mem.Success ? int.Parse(mem.Groups[1].Value) : 0, mem.Groups[2].Success);
    }

    // Added: fixed already, the log still from before. A card whose vendor the game's list lacks stays Unknown.
    public static CardState State(DeviceCard? card, string bin)
    {
        if (card is null) return CardState.Unknown;
        if (card.Found && !card.LowMemory) return CardState.Recognized;
        try { return card.Fixes(bin).Any() ? CardState.NotRecognized : card.Found || card.Listed(bin) ? CardState.Added : CardState.Unknown; }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { Engine.Log(e); return CardState.Unknown; }
    }

    bool LowMemory => MemoryGuessed && MemoryMb < LowMb;
    bool Listed(string bin) => File.Exists(CardsFile(bin)) && WithCard(GameSettings.ReadText(CardsFile(bin))) is null;

    IEnumerable<(string Path, string Text)> Fixes(string bin)
    {
        if (!Found && File.Exists(CardsFile(bin)) && GameSettings.ReadText(CardsFile(bin)) is var cards && WithCard(cards) is { } c && c != cards) yield return (CardsFile(bin), c);
        var rules = GameSettings.RulesFile(bin);
        if (LowMemory && File.Exists(rules) && GameSettings.ReadText(rules) is var text && WithMemory(text) is var r && r != text) yield return (rules, r);
    }

    public void Fix(Game g)
    {
        Engine.CheckWritable();
        using var point = RestorePoints.Begin(g.Bin, "Graphics card");
        foreach (var (path, text) in Fixes(g.Bin).ToList())
        {
            GameSettings.KeepBin(path, g.Bin);
            GameSettings.WriteText(path, text);
        }
    }

    // the card's line added under its vendor; null when it is there already, the text as it was without its vendor
    internal string? WithCard(string text)
    {
        var lines = text.Split('\n').ToList();
        bool IsVendor(string l) => Regex.IsMatch(l, @"^\s*vendor\s+""");
        var at = lines.FindIndex(l => IsVendor(l) && Ids(l[l.LastIndexOf('"')..]).Contains(Vendor));
        if (at < 0) return text;
        var end = lines.FindIndex(at + 1, IsVendor);
        for (int i = at + 1; i < (end < 0 ? lines.Count : end); i++)
            if (Regex.Match(lines[i], @"^\s*card\s+([^""]*)""") is { Success: true } m && Ids(m.Groups[1].Value).Contains(Device)) return null;
        // (the driver's name goes into a quoted script text: only plain characters)
        var name = Regex.Replace(Name, @"[^A-Za-z0-9 ()/+.\-]", "").Trim();
        lines.Insert(at + 1, $"\tcard 0x{Device:x4} \"{(name.Length > 0 ? name : "Graphics card")}\"" + (lines[at].EndsWith('\r') ? "\r" : ""));
        return string.Join('\n', lines);
    }
    static IEnumerable<int> Ids(string s) => Regex.Matches(s, @"0x([0-9a-fA-F]{4})\b").Select(m => int.Parse(m.Groups[1].Value, NumberStyles.HexNumber));

    // the video memory the rules assume when the driver reports none
    internal static string WithMemory(string text) =>
        Regex.Replace(text, @"^(\s*if\s*\(\s*\$textureMemory\s*==\s*0\b[^\n]*\n\s*seti\s+textureMemory\s+)(\d+)",
            m => int.TryParse(m.Groups[2].Value, out var mb) && mb < LowMb ? m.Groups[1].Value + FallbackMb : m.Value, RegexOptions.Multiline);
}
