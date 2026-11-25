using System.Diagnostics;
using System.Globalization;
using System.Text.RegularExpressions;
using System.Xml.Linq;

namespace ApexSetup;

// Everything the PC wrote about one game crash or freeze, gathered from each place that keeps it, so Health can say what
// happened and show how it knows:
//   Windows' event log      Application Error 1000 (the exception code), Application Hang 1002 (the game froze and was
//                           closed), the graphics driver's events (nvlddmkm, amdkmdag; Display 4101: driver restarted)
//   the game's crash log    xcpt *.txt (ScriptErrors reads it: the faulting file and who called it)
//   Apex Radiance           ApexRadiance_Crash.txt (newest crash only) and ApexRadiance_LOG(.old).txt: its errors before the
//                           end, and the next start's "[Session]" lines on a session that did not close
//   the renderer's logs     rtx-remix\logs\bridge32/bridge64.log, NvRemixBridge_d3d9.log, TS3W_d3d9.log (DXVK): written
//                           over at each start, so they speak only for the last session
//   the session record      the launcher's: memory peak, time played
// Each clue is a translatable text with its arguments; raw log lines are passed as they are.
public static class CrashEvidence
{
    public sealed record Clue(string Source, string Text, string[] Args);
    // Kind: "crash" (Detail: the exception code), "hang", "gpu" (Detail: provider and event id)
    public sealed record WinEvent(DateTime When, string Kind, string Detail, string Module = "");

    // ---------- Windows' event log ----------

    public static List<WinEvent> Windows(DateTime since)
    {
        var list = new List<WinEvent>();
        var ms = (long)Math.Max(1, (DateTime.Now - since).TotalMilliseconds);
        foreach (var x in Query("Application", $"*[System[(Provider[@Name='Application Error'] or Provider[@Name='Application Hang']) and TimeCreated[timediff(@SystemTime) <= {ms}]]]"))
        {
            var data = Data(x);
            var app = data.GetValueOrDefault("AppName") ?? "";
            if (!Regex.IsMatch(app, @"^TS3W?\.exe$", RegexOptions.IgnoreCase)) continue;
            var provider = (string?)x.Descendants().FirstOrDefault(e => e.Name.LocalName == "Provider")?.Attribute("Name") ?? "";
            list.Add(provider == "Application Hang"
                ? new(TimeOf(x), "hang", "")
                : new(TimeOf(x), "crash", data.GetValueOrDefault("ExceptionCode") ?? "", data.GetValueOrDefault("ModuleName") ?? ""));
        }
        foreach (var x in Query("System", $"*[System[(Provider[@Name='nvlddmkm'] or Provider[@Name='amdkmdag'] or Provider[@Name='amdwddmg'] or (Provider[@Name='Display'] and EventID=4101)) and TimeCreated[timediff(@SystemTime) <= {ms}]]]"))
        {
            var sys = x.Descendants().First(e => e.Name.LocalName == "System");
            var provider = (string?)sys.Elements().FirstOrDefault(e => e.Name.LocalName == "Provider")?.Attribute("Name") ?? "";
            var id = sys.Elements().FirstOrDefault(e => e.Name.LocalName == "EventID")?.Value ?? "";
            list.Add(new(TimeOf(x), "gpu", $"{provider} {id}"));
        }
        return list.OrderByDescending(e => e.When).ToList();
    }

    // wevtutil's XML (no package and nothing the trimmer drops); nothing when Windows refuses or the query fails
    static IEnumerable<XElement> Query(string log, string xpath)
    {
        try
        {
            var psi = new ProcessStartInfo("wevtutil.exe") { RedirectStandardOutput = true, UseShellExecute = false, CreateNoWindow = true, StandardOutputEncoding = System.Text.Encoding.UTF8 };
            foreach (var a in new[] { "qe", log, "/q:" + xpath, "/f:xml", "/rd:true", "/c:300" }) psi.ArgumentList.Add(a);
            using var p = Process.Start(psi);
            if (p is null) return [];
            var text = p.StandardOutput.ReadToEnd();
            p.WaitForExit(5000);
            return text.Length == 0 ? [] : XElement.Parse("<r>" + text + "</r>").Elements().ToList();
        }
        catch (Exception e) when (e is System.ComponentModel.Win32Exception or InvalidOperationException or System.Xml.XmlException or IOException) { Engine.Log(e); return []; }
    }

    // the event's named fields; older Windows 10 builds write Application Error's fields unnamed, in a fixed order
    static readonly string[] AppErrorOrder = ["AppName", "AppVersion", "AppTimeStamp", "ModuleName", "ModuleVersion", "ModuleTimeStamp", "ExceptionCode", "FaultingOffset"];
    static Dictionary<string, string> Data(XElement ev)
    {
        var all = ev.Descendants().Where(e => e.Name.LocalName == "Data").ToList();
        if (all.Any(e => e.Attribute("Name") is not null))
            return all.Where(e => e.Attribute("Name") is not null).GroupBy(e => (string)e.Attribute("Name")!).ToDictionary(g => g.Key, g => g.First().Value);
        var d = new Dictionary<string, string>();
        for (int i = 0; i < Math.Min(all.Count, AppErrorOrder.Length); i++) d[AppErrorOrder[i]] = all[i].Value;
        return d;
    }

    static DateTime TimeOf(XElement ev) =>
        DateTime.TryParse((string?)ev.Descendants().FirstOrDefault(e => e.Name.LocalName == "TimeCreated")?.Attribute("SystemTime"), CultureInfo.InvariantCulture,
            DateTimeStyles.AdjustToUniversal | DateTimeStyles.AssumeUniversal, out var t) ? t.ToLocalTime() : DateTime.MinValue;

    // ---------- what an exception code means ----------

    public static string? CodeMeaning(string code) => code.TrimStart('0', 'x').ToLowerInvariant() switch
    {
        "c0000005" => "It read or wrote memory it had no right to (access violation, {0}).",
        "c0000096" or "c000001d" => "The processor refused an instruction ({0}). This usually comes from damaged code: a hook, an overlay or a driver.",
        "c0000374" => "The game's memory was damaged earlier ({0}). The file named is where it showed, not always who did it.",
        "c0000409" => "A safety check stopped the game ({0}).",
        "c00000fd" => "The game ran out of stack ({0}), usually calls looping without end.",
        "c0000017" or "c000012d" => "Windows could not give the game more memory ({0}).",
        "e06d7363" => "A C++ error was thrown and nothing caught it ({0}).",
        _ => null,
    };

    // ---------- the clues of one incident ----------

    // from..to: the session's start (or the crash itself) to its end; at: the moment it crashed, froze or was closed
    public static List<Clue> For(DateTime at, DateTime from, ScriptErrors.Error? crash, ScriptErrors.ApexCrash? apex, IReadOnlyList<WinEvent> events,
        SessionInfo? session, string docs, string? bin)
    {
        var clues = new List<Clue>();
        // the game's crash log: where it faulted and who called
        if (crash is not null)
        {
            if (crash.Why == "called" && crash.Mod is not null) clues.Add(new("Game crash log", "The fault was in {0}, called straight from {1}.", [crash.Exception, crash.Mod]));
            else if (crash.Mod is not null) clues.Add(new("Game crash log", "The fault was in {0}, in its own code.", [crash.Files.FirstOrDefault() is { } f ? Path.GetFileName(f) : crash.Mod]));
            else if (crash.Why != "memory") clues.Add(new("Game crash log", "The fault was in the game's own code, with no mod calling it.", []));
            if (crash.Earlier && crash.Files.FirstOrDefault() is { } file && File.Exists(file))
                clues.Add(new("Game folder", "{0} was replaced on {1}, after this crash.", [Path.GetFileName(file), File.GetLastWriteTime(file).ToString("g")]));
        }
        // Apex Radiance's report of the same moment
        if (apex is not null && Math.Abs((apex.When - at).TotalSeconds) < 30)
        {
            var callers = apex.Refs.Take(8).Where(r => !r.Equals(apex.Fault, StringComparison.OrdinalIgnoreCase)).Distinct(StringComparer.OrdinalIgnoreCase).Take(3).ToList();
            clues.Add(new("Apex Radiance report", callers.Count > 0 ? "It faulted in {0}; the code right above it was {1}." : "It faulted in {0}.", [apex.Fault, string.Join(", ", callers)]));
        }
        // Windows: the exception code, a freeze, the graphics driver
        foreach (var e in events.Where(e => Math.Abs((e.When - at).TotalSeconds) <= 90))
            switch (e.Kind)
            {
                case "crash" when CodeMeaning(e.Detail) is { } meaning: clues.Add(new("Windows", meaning, ["0x" + e.Detail.TrimStart('0', 'x')])); break;
                case "crash": clues.Add(new("Windows", "Windows recorded the crash with code {0}.", ["0x" + e.Detail.TrimStart('0', 'x')])); break;
                case "hang": clues.Add(new("Windows", "The game stopped responding and was closed.", [])); break;
                case "gpu": clues.Add(new("Graphics driver", e.Detail.StartsWith("Display", StringComparison.Ordinal) ? "The graphics driver stopped responding and was restarted."
                        : e.Detail.EndsWith(" 153", StringComparison.Ordinal) ? "The graphics driver reset the GPU after it stopped responding ({0})." : "The graphics driver reported a GPU error ({0}).", [e.Detail])); break;
            }
        // the session: memory and time
        if (session is not null)
            clues.Add(new("Session", "Memory peaked at {0} MB of 4096, after {1} min of play.", [(session.PeakBytes >> 20).ToString(), ((int)session.Played.TotalMinutes).ToString()]));
        // the renderer's logs, when they are of this session
        if (bin is not null)
        {
            Renderer(clues, Path.Combine(bin, "rtx-remix", "logs", "bridge32.log"), "RTX Remix bridge log", at, from);
            Renderer(clues, Path.Combine(bin, "rtx-remix", "logs", "bridge64.log"), "RTX Remix bridge log", at, from);
            Renderer(clues, Path.Combine(bin, "NvRemixBridge_d3d9.log"), "RTX Remix log", at, from);
            Renderer(clues, Path.Combine(bin, "TS3W_d3d9.log"), "DXVK log", at, from);
        }
        ApexLog(clues, docs, at, from);
        // (Windows writes some events twice, and a driver fault repeats: each clue once)
        return clues.DistinctBy(c => (c.Source, c.Text, string.Join("\u0001", c.Args))).ToList();
    }

    // lines a renderer log marks as an error or a warning, minus the ones every start writes
    static readonly Regex Benign = new(@"OpenVR|Cache was created with|timeouts for bridge|Discarding old cache|not supported by|VK_[A-Z_]+:\s*$", RegexOptions.IgnoreCase);
    static readonly Regex Bad = new(@"(^|\]\s*)(err|error|warn)\s*:|device lost|DEVICE_LOST|timed out|out of memory|failed|exception", RegexOptions.IgnoreCase);

    static void Renderer(List<Clue> clues, string path, string source, DateTime at, DateTime from)
    {
        try
        {
            if (!File.Exists(path)) return;
            var written = File.GetLastWriteTime(path);
            // (a log written over by a later start is another session's)
            if (written < from.AddMinutes(-1) || written > at.AddMinutes(15)) return;
            var bad = ReadShared(path).Where(l => Bad.IsMatch(l) && !Benign.IsMatch(l)).TakeLast(4).ToList();
            if (bad.Count == 0) clues.Add(new(source, "No error or warning in that session.", []));
            foreach (var l in bad) clues.Add(new(source, "{0}", [l.Trim()]));
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
    }

    // Apex Radiance's log of that session (its errors before the end) and the next start's report on it ("[Session]")
    static void ApexLog(List<Clue> clues, string docs, DateTime at, DateTime from)
    {
        var dir = Scan.ApexDir(docs);
        foreach (var name in new[] { "ApexRadiance_LOG.old.txt", "ApexRadiance_LOG.txt" })
        {
            var path = Path.Combine(dir, name);
            try
            {
                if (!File.Exists(path)) continue;
                var lines = ReadShared(path);
                var head = lines.FirstOrDefault() is { } h ? Regex.Match(h, @"started (\d{4}-\d\d-\d\d \d\d:\d\d:\d\d)") : Match.Empty;
                if (!head.Success || !DateTime.TryParse(head.Groups[1].Value, CultureInfo.InvariantCulture, DateTimeStyles.None, out var started)) continue;
                var written = File.GetLastWriteTime(path);
                if (Math.Abs((started - from).TotalMinutes) < 3 && written >= at.AddMinutes(-5))
                {
                    // the session's own log: its errors before the end
                    foreach (var l in lines.Where(l => l.Contains("[ERROR]")).TakeLast(3)) clues.Add(new("Apex Radiance log", "{0}", [l.Trim()]));
                }
                else if (started > from.AddMinutes(3) && started > at.AddMinutes(-2) && started < at.AddDays(2))
                {
                    // the next start looked back at a session that did not close: its "[Session]" lines about the one that
                    // started at `from` (the line names the start), up to the next session it reports on
                    var mine = false;
                    string? lastStep = null;
                    foreach (var l in lines.Take(400))
                    {
                        // ("    frame N, T s: <step>": the GPU's last steps before it stopped, oldest first; the last one is where)
                        if (mine && Regex.Match(l, @"^\d\d:\d\d:\d\d\.\d+ \[\w+\]\s{2,}(frame \d+, [\d.]+ s: .+)$") is { Success: true } step) { lastStep = step.Groups[1].Value; continue; }
                        if (!l.Contains("[Session]")) continue;
                        if (lastStep is not null) { clues.Add(new("Apex Radiance log", "The last step the GPU finished: {0}.", [lastStep])); lastStep = null; }
                        var of = Regex.Match(l, @"started (\d{4}-\d\d-\d\d \d\d:\d\d:\d\d)");
                        if (of.Success) mine = DateTime.TryParse(of.Groups[1].Value, CultureInfo.InvariantCulture, DateTimeStyles.None, out var s) && Math.Abs((s - from).TotalMinutes) < 3;
                        if (mine) clues.Add(new("Apex Radiance log", "{0}", [Regex.Replace(l.Trim(), @"^\d\d:\d\d:\d\d\.\d+ \[\w+\] \[Session\] ", "").TrimEnd(':', ' ').Replace(". Its last steps, oldest first", ".")]));
                    }
                    if (lastStep is not null) clues.Add(new("Apex Radiance log", "The last step the GPU finished: {0}.", [lastStep]));
                }
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { }
        }
    }

    // (one read per file and version: the timeline asks for the same Apex Radiance logs once per session)
    static readonly Dictionary<string, (DateTime Written, List<string> Lines)> Read = new(StringComparer.OrdinalIgnoreCase);
    static List<string> ReadShared(string path)
    {
        var written = File.GetLastWriteTimeUtc(path);
        lock (Read) if (Read.TryGetValue(path, out var c) && c.Written == written) return c.Lines;
        var lines = ReadLines(path);
        lock (Read) Read[path] = (written, lines);
        return lines;
    }

    static List<string> ReadLines(string path)
    {
        using var s = new FileStream(path, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
        using var r = new StreamReader(s);
        var list = new List<string>();
        while (r.ReadLine() is { } l) list.Add(l.Length > 300 ? l[..300] + "…" : l);
        return list;
    }
}
