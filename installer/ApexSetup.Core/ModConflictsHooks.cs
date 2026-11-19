using System.Text.RegularExpressions;

namespace ApexSetup;

// The hook table: which TS3W.exe / TS3.exe addresses each known native mod patches, per game edition (addresses and names
// only), from sims3-modernization's hook conflict map. An embedded copy ships with Setup; every scan also tries the
// published copy (plain HTTPS GET of one public CSV, a generic User-Agent, no other data sent), keeps it when it is valid and
// not older, and caches it. Offline, on a timeout or an invalid file the newest valid copy at hand is used.
public static partial class ModConflicts
{
    public const string HookTableUrl = "https://raw.githubusercontent.com/rodneypantonial/sims3-modernization/master/docs/data/hook-table.csv";

    public sealed record HookMod(string Name, Regex File, string Version, string[] Sha256);
    /// <summary>Coordinated: other mod name -> "&lt;who&gt; stands down" or "&lt;who&gt; chains" (a shared address that is not a conflict).</summary>
    public sealed record Hook(string Mod, string Edition, uint Address, string Kind, string Purpose, IReadOnlyDictionary<string, string> Coordinated);

    public sealed class HookTable
    {
        public string Date { get; private set; } = "";
        public string Source { get; init; } = "";
        public Dictionary<string, uint> Editions { get; } = new();   // tag -> the exe's PE timestamp
        public List<HookMod> Mods { get; } = new();
        public List<Hook> Hooks { get; } = new();

        // a table file; any row out of format rejects the whole file (it may come from the network)
        public static HookTable Parse(string csv, string source)
        {
            if (csv.Length > 4 << 20) throw new InvalidDataException("hook table too large");
            var t = new HookTable { Source = source };
            var date = "";
            var n = 0;
            foreach (var raw in csv.Split('\n'))
            {
                n++;
                var line = raw.TrimEnd('\r');
                if (line.Length == 0 || line[0] == '#') continue;
                var c = CsvRow(line);
                if (c[0] == "hook" && c.Count == 6) c.Add(""); // a table from before the coordinated column
                bool Ok(int count, params Regex[] rx) => c.Count == count && rx.Select((r, i) => r.IsMatch(c[i + 1])).All(x => x);
                switch (c[0])
                {
                    case "date" when Ok(2, DateRx) && DateOnly.TryParseExact(c[1], "yyyy-MM-dd", out _):
                        date = c[1];
                        break;
                    case "edition" when Ok(3, TagRx, StampRx):
                        t.Editions[c[1]] = Convert.ToUInt32(c[2], 16);
                        break;
                    case "mod" when Ok(5, NameRx, FileRx, TextRx, ShaRx):
                        Regex file;
                        try { file = new Regex(c[2], RegexOptions.IgnoreCase | RegexOptions.CultureInvariant, TimeSpan.FromMilliseconds(100)); }
                        catch (ArgumentException) { throw new InvalidDataException($"hook table line {n}: bad file pattern"); }
                        t.Mods.Add(new(c[1], file, c[3], c[4].Split(';', StringSplitOptions.RemoveEmptyEntries)));
                        break;
                    case "hook" when Ok(7, NameRx, TagRx, AddrRx, TextRx, TextRx, TextRx) && t.Mods.Any(m => m.Name == c[1]) && t.Editions.ContainsKey(c[2])
                                     && Convert.ToUInt32(c[3], 16) is >= 0x401000 and <= 0x12FFFFF && Coordinated(c[6], c[1], t) is { } co:
                        t.Hooks.Add(new(c[1], c[2], Convert.ToUInt32(c[3], 16), c[4], c[5], co));
                        break;
                    default:
                        throw new InvalidDataException($"hook table line {n}: not a valid row");
                }
            }
            if (date == "" || t.Editions.Count == 0 || t.Hooks.Count == 0) throw new InvalidDataException("hook table without date, editions or hooks");
            t.Date = date;
            return t;
        }

        public static HookTable Embedded()
        {
            using var s = typeof(ModConflicts).Assembly.GetManifestResourceStream("ModConflicts.hooks.csv")!;
            return Parse(new StreamReader(s).ReadToEnd(), "embedded copy");
        }

        // The newest valid table among the embedded copy, the cached download and (unless offline) the published copy;
        // a valid download is cached. fetch stands in for the download in tests.
        public static HookTable Load(bool offline, string? cacheFile, Func<string>? fetch = null)
        {
            static HookTable? Try(Func<string?> get, string source)
            {
                try { return get() is { } text ? Parse(text, source) : null; }
                catch (Exception e) when (e is InvalidDataException or IOException or UnauthorizedAccessException or HttpRequestException
                                          or TaskCanceledException or InvalidOperationException or RegexMatchTimeoutException) { return null; }
            }
            var best = Embedded();
            if (cacheFile is not null && Try(() => File.Exists(cacheFile) ? File.ReadAllText(cacheFile) : null, "cached copy of " + HookTableUrl) is { } cached
                && string.CompareOrdinal(cached.Date, best.Date) > 0) best = cached;
            if (offline) return best;
            string? text = null;
            if (Try(() => text = (fetch ?? Download)(), HookTableUrl) is { } got)
            {
                if (string.CompareOrdinal(got.Date, best.Date) >= 0) best = got;
                if (cacheFile is not null)
                    try
                    {
                        Directory.CreateDirectory(Path.GetDirectoryName(cacheFile)!);
                        File.WriteAllText(cacheFile + ".tmp", text);
                        File.Move(cacheFile + ".tmp", cacheFile, true);
                    }
                    catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
            }
            return best;
        }

        static string Download()
        {
            using var http = new HttpClient { Timeout = TimeSpan.FromSeconds(5) };
            http.DefaultRequestHeaders.UserAgent.ParseAdd("ApexModConflicts/" + (typeof(ModConflicts).Assembly.GetName().Version?.ToString(3) ?? "1.0"));
            return http.GetStringAsync(HookTableUrl).GetAwaiter().GetResult();
        }

        // the edition of the game exe in bin (TS3W.exe or TS3.exe), by its PE timestamp; null when none matches
        public string? EditionOf(string? bin)
        {
            if (bin is null) return null;
            foreach (var exe in new[] { "TS3W.exe", "TS3.exe" })
                if (File.Exists(Path.Combine(bin, exe)) && Games.PeTimestamp(Path.Combine(bin, exe)) is var stamp and not 0
                    && Editions.FirstOrDefault(e => e.Value == stamp).Key is { } tag) return tag;
            return null;
        }
    }

    // "<other mod>=<who> stands down|chains;...": the other mod is listed, <who> is one of the two; null = invalid
    static Dictionary<string, string>? Coordinated(string v, string mod, HookTable t)
    {
        var d = new Dictionary<string, string>();
        foreach (var e in v.Split(';', StringSplitOptions.RemoveEmptyEntries))
        {
            var kv = e.Split('=');
            if (kv.Length != 2 || kv[0] == mod || !t.Mods.Any(m => m.Name == kv[0]) || d.ContainsKey(kv[0])
                || !new[] { kv[0], mod }.Any(n => kv[1] == $"{n} stands down" || kv[1] == $"{n} chains")) return null;
            d[kv[0]] = kv[1];
        }
        return d;
    }

    static readonly Regex DateRx = new(@"^\d{4}-\d{2}-\d{2}$"), TagRx = new(@"^[a-z0-9.-]{1,20}$"), StampRx = new(@"^0x[0-9A-Fa-f]{8}$"),
        NameRx = new(@"^[\w .+-]{1,40}$"), FileRx = new(@"^[\x20-\x7e]{1,100}$"), TextRx = new(@"^[\x20-\x7e]{0,200}$"),
        ShaRx = new(@"^([0-9a-f]{64}(;[0-9a-f]{64})*)?$"), AddrRx = new(@"^0x[0-9A-Fa-f]{1,8}$");

    // one CSV row: plain or double-quoted fields ("" is a quote)
    static List<string> CsvRow(string line)
    {
        var o = new List<string>();
        var f = new System.Text.StringBuilder();
        var q = false;
        for (var i = 0; i < line.Length; i++)
        {
            var ch = line[i];
            if (q) { if (ch != '"') f.Append(ch); else if (i + 1 < line.Length && line[i + 1] == '"') { f.Append('"'); i++; } else q = false; }
            else if (ch == '"') q = true;
            else if (ch == ',') { o.Add(f.ToString()); f.Clear(); }
            else f.Append(ch);
        }
        o.Add(f.ToString());
        return o;
    }
}
