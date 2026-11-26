using System.Globalization;
using System.Text;
using System.Text.RegularExpressions;

namespace ApexSetup;

public enum SettingFile { Rules, Ini, Options }

// One option the game keeps in a file. Values are text: a number, or "0"/"1" (1: turned off in the game). Read gives null when
// the file does not have the place it would be written to.
public sealed record GameSetting(string Id, SettingFile File, string Default, Func<string, string?> Read, Func<string, string, string> With, Func<string, bool> Valid);

// The Game settings page and the install's step: graphics in Game\Bin\GraphicsRules.sgr, loading and walking in
// Game\Bin\Sims3.ini, the game's own options in Documents\...\Options.ini. Only a value the player changes is written, every
// other byte stays (read and written as Latin-1), and each file is copied before its first change (RestoreBin, RestoreDocs).
public static class GameSettings
{
    public const string Shadow = "shadow", Sims = "sims", Bloom = "bloom", Reads = "reads",
        Store = "store", Interactive = "loading", Memories = "memories", Online = "online";

    // Sims3.ini [Config]: the values in the game's own file
    public static readonly (string Id, string Key, string Default)[] Walking =
    [
        ("walk.radius", "DynamicAvoidance_FieldRadius", "0.15"),
        ("walk.inactive", "DynamicAvoidance_InactiveFieldLength", "3.0"),
        ("walk.gap", "DynamicAvoidance_MinNonIntersectingDistance", "0.24"),
        ("walk.pad", "DynamicAvoidance_FieldLengthPad", "0.6"),
        ("walk.stop", "DynamicAvoidance_StopDistanceMultiplier", "1.85"),
        ("stroller.path", "Stroller_MinimumPathLength", "5.5"),
        ("stroller.incline", "Stroller_MaxIncline", "0.05"),
    ];

    public static readonly string[] ShadowSizes = ["2048", "4096", "8192"], SimSizes = ["2048", "4096"], ReadSizes = ["20000000", "40000000"];

    public static readonly GameSetting[] All =
    [
        // (the game's highest lighting quality)
        new(Shadow, SettingFile.Rules, "2048", t => Prop(t, "LightingQuality", "$High", "ShadowMapSize"),
            (t, v) => WithProp(t, "LightingQuality", "$High", "ShadowMapSize", v), ShadowSizes.Contains),
        // (the two nearest detail levels of the highest Sim detail; the far ones stay)
        new(Sims, SettingFile.Rules, "2048", t => SimTextures(t)?[0].Trim(),
            (t, v) => SimTextures(t) is { } s ? WithProp(t, "SimQuality", "$VeryHigh", "RenderSimTextureSizes", $"\"{v}, {v},{s[2]},{s[3]}\"") : t, SimSizes.Contains),
        new(Bloom, SettingFile.Rules, "0", t => Block(t, "AdvancedRendering", "$On") is null ? null : Prop(t, "AdvancedRendering", "$On", "RenderPostProcessEnabled") == "false" ? "1" : "0",
            (t, v) => WithoutProp(t, "AdvancedRendering", "$On", "RenderPostProcessEnabled") is var clean && v == "1" ? AddProp(clean, "AdvancedRendering", "$On", "RenderPostProcessEnabled", "false") : clean, IsSwitch),
        new(Reads, SettingFile.Ini, "20000000", t => IniValue(t, "ResourceSystem", "MemoryUsageLimit"), (t, v) => WithIni(t, "ResourceSystem", "MemoryUsageLimit", v), ReadSizes.Contains),
        .. Walking.Select(w => new GameSetting(w.Id, SettingFile.Ini, w.Default, t => IniValue(t, "Config", w.Key), (t, v) => WithIni(t, "Config", w.Key, v), IsNumber)),
        Option(Store, "enableingamestore", "0"),
        Option(Interactive, "enableinteractiveloading", "0"),
        Option(Memories, "enablememories", "3"), // (the game's MemoriesMode: 1 on, 2 on without notifications, 3 off)
        // the Connect notifications, and the sign-in prompt before a save loads: 1 in requireloginbeforeload is its "don't ask again"
        new(Online, SettingFile.Options, "0", t => IniValue(t, "options", "receiveconnecttns") == "0" ? "1" : "0",
            (t, v) => WithIni(WithIni(t, "options", "receiveconnecttns", v == "1" ? "0" : "1"), "options", "requireloginbeforeload", v), IsSwitch),
    ];

    static GameSetting Option(string id, string key, string off) =>
        new(id, SettingFile.Options, "0", t => IniValue(t, "options", key) == off ? "1" : "0",
            (t, v) => (IniValue(t, "options", key) == off) == (v == "1") ? t : WithIni(t, "options", key, v == "1" ? off : "1"), IsSwitch);

    static string[]? SimTextures(string t) => Prop(t, "SimQuality", "$VeryHigh", "RenderSimTextureSizes")?.Trim('"').Split(',') is { Length: 4 } s ? s : null;
    static bool IsSwitch(string v) => v is "0" or "1";
    static bool IsNumber(string v) => Number(v) is >= 0 and <= 100;
    public static double? Number(string? v) => double.TryParse(v, NumberStyles.AllowDecimalPoint, CultureInfo.InvariantCulture, out var d) ? d : null;
    // a value typed with a dot or a comma, as the files write it (a dot); null when it is not a number from 0 to 100
    public static string? Typed(string text) => text.Trim().Replace(',', '.') is var t && IsNumber(t) ? t : null;
    public static bool Same(string? a, string? b) => a == b || Number(a) is { } x && Number(b) is { } y && Math.Abs(x - y) < 1e-6;
    public static GameSetting Of(string id) => All.First(s => s.Id == id);
    public static bool InBin(string id) => Of(id).File != SettingFile.Options;

    // ---------- the files ----------

    public static string RulesFile(string bin) => Path.Combine(bin, "GraphicsRules.sgr");
    static string PathOf(SettingFile f, string bin, string docs) =>
        f switch { SettingFile.Rules => RulesFile(bin), SettingFile.Ini => Path.Combine(bin, "Sims3.ini"), _ => Engine.OptionsIni(docs) };

    // the game's files are plain bytes; one saved again by an editor as UTF-16 or UTF-8 with its mark is read as that, and
    // written back the same way
    internal static string ReadText(string f)
    {
        var b = File.ReadAllBytes(f);
        var e = EncodingOf(b);
        var skip = e.GetPreamble().Length;
        return e.GetString(b, skip, b.Length - skip);
    }
    internal static void WriteText(string f, string text) => Fs.Write(f, text, File.Exists(f) ? EncodingOf(File.ReadAllBytes(f)) : Encoding.Latin1);
    static Encoding EncodingOf(byte[] b) => b is [0xFF, 0xFE, ..] ? Encoding.Unicode : b is [0xFE, 0xFF, ..] ? Encoding.BigEndianUnicode
        : b is [0xEF, 0xBB, 0xBF, ..] ? Encoding.UTF8 : Encoding.Latin1;

    // what the files hold now; a setting without its place in the file is left out. Without Options.ini, its settings are the
    // game's defaults.
    public static Dictionary<string, string> Now(string bin, string docs)
    {
        var now = new Dictionary<string, string>();
        foreach (var file in All.GroupBy(s => s.File))
        {
            string? text = null;
            try { if (PathOf(file.Key, bin, docs) is var p && File.Exists(p)) text = ReadText(p); }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { Engine.Log(e); continue; }
            foreach (var s in file)
                if ((text is not null ? s.Read(text) : file.Key == SettingFile.Options ? s.Default : null) is { } v) now[s.Id] = v;
        }
        return now;
    }

    // the wanted values that are known, allowed and not the ones there now: all a file ever gets
    public static Dictionary<string, string> Changes(IReadOnlyDictionary<string, string> want, IReadOnlyDictionary<string, string> now) =>
        All.Where(s => want.TryGetValue(s.Id, out var v) && s.Valid(v) && now.TryGetValue(s.Id, out var n) && !Same(v, n)).ToDictionary(s => s.Id, s => want[s.Id]);

    // Game\Bin's files, in a restore point (as administrator where the folder needs it)
    public static void ApplyBin(Game g, string docs, IReadOnlyDictionary<string, string> want)
    {
        Engine.CheckWritable();
        using var point = RestorePoints.Begin(g.Bin, "Game settings");
        Write(g.Bin, docs, Changes(want, Now(g.Bin, docs)).Where(c => InBin(c.Key)));
    }

    // Options.ini, as the player. False without the file: the choice waits until the game has made it
    public static bool ApplyOptions(Game g, string docs, IReadOnlyDictionary<string, string> want)
    {
        Engine.CheckWritable();
        if (!File.Exists(Engine.OptionsIni(docs))) return false;
        Write(g.Bin, docs, Changes(want, Now(g.Bin, docs)).Where(c => !InBin(c.Key)));
        return true;
    }

    static void Write(string bin, string docs, IEnumerable<KeyValuePair<string, string>> changes)
    {
        foreach (var file in changes.GroupBy(c => Of(c.Key).File))
        {
            var path = PathOf(file.Key, bin, docs);
            var was = ReadText(path);
            var text = file.Aggregate(was, (t, c) => Of(c.Key).With(t, c.Value));
            if (text == was) continue;
            if (file.Key == SettingFile.Options) { Keep(path, DocsCopies(docs)); WriteText(path, text); }
            else { KeepBin(path, bin); WriteBin(path, text, bin); }
        }
    }

    // ---------- the copies and Restore my files ----------

    static string BinCopies(string bin) => Path.Combine(Manifest.DirOf(bin), "game settings");
    // (not in Scan.ApexDir, which changes with the installed ASI)
    static string DocsCopies(string docs) => Path.Combine(docs, "Apex Radiance", "game settings");
    internal static void KeepBin(string path, string bin) => Keep(path, BinCopies(bin));
    // a file of Game\Bin, and what was written: one that is something else at the restore was edited since, as is one that
    // was something else before a later write (no note of it then)
    internal static void WriteBin(string path, string text, string bin)
    {
        var copy = Path.Combine(BinCopies(bin), Path.GetFileName(path));
        var ours = Hash(path) == (File.Exists(copy + Last) ? File.ReadAllText(copy + Last) : Hash(copy));
        File.Delete(copy + Last); // (first: a write cut short is not ours)
        WriteText(path, text);
        if (ours) File.WriteAllText(copy + Last, Hash(path));
    }
    const string Last = ".last";
    static string Hash(string path) => Convert.ToHexString(System.Security.Cryptography.SHA256.HashData(File.ReadAllBytes(path)));
    internal static void KeepDocs(string path, string docs) => Keep(path, DocsCopies(docs));

    // a file that was not there: an empty copy
    internal static void KeepNone(string path, string docs)
    {
        var copy = Path.Combine(DocsCopies(docs), Path.GetFileName(path));
        Directory.CreateDirectory(Path.GetDirectoryName(copy)!);
        if (!File.Exists(copy)) File.WriteAllText(copy, "");
    }

    // the file as it was before the launcher's first change; a later change keeps that first copy
    static void Keep(string path, string dir)
    {
        var copy = Path.Combine(dir, Path.GetFileName(path));
        if (File.Exists(copy)) return;
        Directory.CreateDirectory(dir);
        File.Copy(path, copy + ".tmp", true);
        File.SetAttributes(copy + ".tmp", FileAttributes.Normal);
        File.Move(copy + ".tmp", copy);
    }

    static string[] Copies(string dir) => Directory.Exists(dir) ? Directory.GetFiles(dir).Where(f => !f.EndsWith(".tmp", StringComparison.Ordinal) && !f.EndsWith(Last, StringComparison.Ordinal)).ToArray() : [];
    public static bool BinHasCopies(string bin) => Copies(BinCopies(bin)).Length > 0;
    public static bool CanRestore(string bin, string docs) => BinHasCopies(bin) || Copies(DocsCopies(docs)).Length > 0;

    // Game\Bin's files: whole while they are what the launcher wrote; one edited since (by hand, another tool, a restore
    // point) keeps the edit and gets only the launcher's own values back
    public static void RestoreBin(Game g)
    {
        if (!BinHasCopies(g.Bin)) return;
        Engine.CheckWritable();
        using var point = RestorePoints.Begin(g.Bin, "Game settings");
        foreach (var copy in Copies(BinCopies(g.Bin)))
        {
            var f = Path.Combine(g.Bin, Path.GetFileName(copy));
            if (!File.Exists(f) || File.Exists(copy + Last) && File.ReadAllText(copy + Last) == Hash(f)) Fs.Copy(copy, f);
            else if (ReadText(f) is var text && BinBack(Path.GetFileName(f), text, ReadText(copy)) is var back && back != text) WriteText(f, back);
            File.Delete(copy);
            File.Delete(copy + Last);
        }
        Drop(BinCopies(g.Bin));
    }

    // Documents: only the launcher's own keys go back, the game and the player changed others since
    public static void RestoreDocs(string docs)
    {
        foreach (var copy in Copies(DocsCopies(docs)))
        {
            Engine.CheckWritable();
            var was = ReadText(copy);
            var options = Path.GetFileName(copy).Equals("Options.ini", StringComparison.OrdinalIgnoreCase);
            var f = options ? Engine.OptionsIni(docs) : SetterSettings.FileOf(docs);
            if (File.Exists(f) && ReadText(f) is var text && (options ? OptionsBack(text, was) : SetterSettings.Back(text, was)) is var back && back != text) WriteText(f, back);
            File.Delete(copy);
        }
        Drop(DocsCopies(docs));
    }

    // (the exact values: memories has three, so not through on and off; a key the launcher added goes)
    static string OptionsBack(string text, string was) =>
        new[] { "enableingamestore", "enableinteractiveloading", "enablememories", "receiveconnecttns", "requireloginbeforeload" }
            .Aggregate(text, (t, k) => IniValue(was, "options", k) is { } v ? WithIni(t, "options", k, v) : WithoutIni(t, "options", k));

    static string BinBack(string name, string text, string was)
    {
        foreach (var s in All.Where(s => Path.GetFileName(PathOf(s.File, "x", "x")).Equals(name, StringComparison.OrdinalIgnoreCase)))
            if (s.Read(was) is { } v && s.Read(text) is { } n && !Same(v, n)) text = s.With(text, v);
        return text;
    }

    static void Drop(string dir)
    {
        try { if (Directory.Exists(dir) && !Directory.EnumerateFileSystemEntries(dir).Any()) Directory.Delete(dir); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { Engine.Log(e); }
    }

    // ---------- GraphicsRules.sgr: option X ... setting $Y ... prop $ConfigGroup Name value ... end ----------
    // (as the game reads it: any case, and a comment may close a line)

    const RegexOptions Loose = RegexOptions.IgnoreCase | RegexOptions.CultureInvariant;
    const string Tail = @"\s*(?:(?:#|//).*)?$";

    static (List<string> Lines, int From, int To)? Block(string text, string option, string setting)
    {
        var lines = text.Split('\n').ToList();
        var at = lines.FindIndex(l => Regex.IsMatch(l, @"^\s*option\s+" + Regex.Escape(option) + Tail, Loose));
        if (at < 0) return null;
        var end = lines.FindIndex(at, l => Regex.IsMatch(l, @"^\s*end" + Tail, Loose));
        if (end < 0) return null;
        var from = lines.FindIndex(at, end - at, l => Regex.IsMatch(l, @"^\s*setting\s+" + Regex.Escape(setting) + Tail, Loose));
        if (from < 0) return null;
        var to = lines.FindIndex(from + 1, end - from - 1, l => Regex.IsMatch(l, @"^\s*setting\s", Loose));
        return (lines, from, to < 0 ? end : to);
    }

    static Regex PropLine(string name) => new(@"^(\s*prop\s+\$ConfigGroup\s+" + Regex.Escape(name) + @"\s+)(.*?)(\s*(?:(?:#|//).*)?)$", Loose);
    static int PropAt((List<string> Lines, int From, int To) b, string name) => b.Lines.FindIndex(b.From + 1, b.To - b.From - 1, l => PropLine(name).IsMatch(l));

    static string? Prop(string text, string option, string setting, string name) =>
        Block(text, option, setting) is { } b && PropAt(b, name) is >= 0 and var i ? PropLine(name).Match(b.Lines[i]).Groups[2].Value : null;

    static string WithProp(string text, string option, string setting, string name, string value)
    {
        if (Block(text, option, setting) is not { } b || PropAt(b, name) is not (>= 0 and var i)) return text;
        b.Lines[i] = PropLine(name).Replace(b.Lines[i], m => m.Groups[1].Value + value + m.Groups[3].Value);
        return string.Join('\n', b.Lines);
    }

    static string WithoutProp(string text, string option, string setting, string name)
    {
        if (Block(text, option, setting) is not { } b || PropAt(b, name) is not (>= 0 and var i)) return text;
        b.Lines.RemoveAt(i);
        return string.Join('\n', b.Lines);
    }

    static string AddProp(string text, string option, string setting, string name, string value)
    {
        if (Block(text, option, setting) is not { } b) return text;
        b.Lines.Insert(b.From + 1, $"\t\tprop $ConfigGroup {name} {value}" + (b.Lines[b.From].EndsWith('\r') ? "\r" : ""));
        return string.Join('\n', b.Lines);
    }

    // ---------- [section] key = value ----------

    static (List<string> Lines, int At, int End) Section(string text, string section)
    {
        var lines = text.Split('\n').ToList();
        var at = lines.FindIndex(l => l.Trim().Equals($"[{section}]", StringComparison.OrdinalIgnoreCase));
        var end = at < 0 ? -1 : lines.FindIndex(at + 1, l => l.TrimStart().StartsWith('['));
        return (lines, at, end < 0 ? lines.Count : end);
    }
    static Regex KeyLine(string key) => new(@"^([ \t]*" + Regex.Escape(key) + @"[ \t]*=[ \t]*)([^;#\r]*?)(\s*([;#].*)?)$", RegexOptions.IgnoreCase);

    internal static string? IniValue(string text, string section, string key)
    {
        var (lines, at, end) = Section(text, section);
        return at >= 0 && lines.FindIndex(at + 1, end - at - 1, l => KeyLine(key).IsMatch(l)) is >= 0 and var i ? KeyLine(key).Match(lines[i]).Groups[2].Value : null;
    }

    static string WithoutIni(string text, string section, string key)
    {
        var (lines, at, end) = Section(text, section);
        if (at >= 0) for (var i = end - 1; i > at; i--) if (KeyLine(key).IsMatch(lines[i])) lines.RemoveAt(i);
        return string.Join('\n', lines);
    }

    // the value in place, or the key added after the section's last line; a file without the section stays as it is
    internal static string WithIni(string text, string section, string key, string value)
    {
        var (lines, at, end) = Section(text, section);
        if (at < 0) return text;
        var i = lines.FindIndex(at + 1, end - at - 1, l => KeyLine(key).IsMatch(l));
        if (i >= 0) lines[i] = KeyLine(key).Replace(lines[i], m => m.Groups[1].Value + value + m.Groups[3].Value);
        else
        {
            var last = end - 1;
            while (last > at && lines[last].Trim().Length == 0) last--;
            lines.Insert(last + 1, $"{key} = {value}" + (lines[at].EndsWith('\r') ? "\r" : ""));
        }
        return string.Join('\n', lines);
    }
}
