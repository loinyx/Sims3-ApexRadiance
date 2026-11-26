using System.Collections.Immutable;
using System.Reflection.Metadata;
using System.Reflection.PortableExecutable;
using System.Text.RegularExpressions;

namespace ApexSetup;

// The error logs the game and script mods write at the top of Documents, read as text (they are not valid XML: NRaas
// ErrorTrap writes "Career: <Unemployed>" unescaped), each error tied to the mod in its stack:
//   ScriptError_<PC>__<date>__<id>.xml   NRaas ErrorTrap: "-- Script Errors --", per error an exception line and Mono frames
//                                        "#3: 0x000f6 stloc.u1 in <Namespace>.<Namespace>.<Type>:<Method> (...)"
//   <Mod>_Error_<date>_<n>__<id>.xml      other script mods (TS3GarbageService), the same exception and frame lines
//   xcpt <PC> <yy-MM-dd HH.mm.ss>.txt     the game's own crash log: "[Exception info]" with the faulting module
// All three formats seen on a real install; the frame layout matches NRaas' ErrorTrap output.
public static partial class ScriptErrors
{
    // Earlier: a crash in a file replaced since (an update): history, not a pattern. Why: how a crash was tied to its cause
    // (null: it faulted in that module; "called": in the game's code, called straight from that module; "memory": the
    // game's memory was nearly full, whatever faulted)
    public sealed record Error(DateTime When, string? Mod, IReadOnlyList<string> Files, string Exception, bool Crash, string Log, bool Earlier = false, string? Why = null);
    public sealed record ModErrors(string? Mod, IReadOnlyList<string> Files, int Count, int Crashes, DateTime Newest, string Exception, bool Earlier = false, string? Why = null);
    // Unread: script logs past MaxFiles, not read (the counts are then "at least")
    public sealed record Report(DateTime Since, List<Error> Errors, List<ModErrors> ByMod, int Unread = 0)
    {
        public int Total => Errors.Count;
        // the plain-English line for Health; args: count, since, the mod with most errors (empty when none)
        public (string Text, string[] Args) Summary => Total == 0 ? ("No script errors since {0}.", [Since.ToString("g")])
            : ByMod.MaxBy(m => m.Count)!.Mod is { } m ? ("{0} script errors since {1}; most went through {2}.", [Total.ToString(), Since.ToString("g"), m])
            : ("{0} script errors since {1}, in the game's own code.", [Total.ToString(), Since.ToString("g")]);
    }

    public const int MaxFiles = 400;          // newest first: a mod writing one file per error filled a real folder with 764
    public const int MaxBytes = 4 << 20;      // per file: a huge log is read up to here
    // namespaces of the game, the runtime and Mono: a frame there is nobody's mod
    static readonly Regex GameType = new(@"^(Sims3\.(Gameplay|SimIFace|UI|Metadata|Store)|ScriptCore|System|Mono|Microsoft|MonoPatcherLib)\b");
    static readonly Regex ExceptionLine = new(@"^\s*([A-Za-z_][\w.]*(Exception|Error)):\s?(.*)$", RegexOptions.Multiline);
    static readonly Regex Frame = new(@"^#\d+:\s+0x[0-9a-fA-F]+\s+\S+\s+in\s+([^\s:(]+):([^\s(]+)", RegexOptions.Multiline);
    static readonly Regex Runtime = new(@"^(System|Mono)\b");
    const int Near = 3;                       // game frames a mod's call may be above a throw and still count as its cause (unverified cut)

    // the logs written since `since` (default: the last week), mapped to the player's mods (null: the Mods folder's packages).
    // The crash logs go back a week at least: a game session ends at its crash, so "since the last start" never holds two,
    // and three in one place are the pattern Findings looks for.
    public static Report Read(string docs, DateTime? since = null, IReadOnlyList<ModFile>? mods = null)
    {
        var week = DateTime.Now.AddDays(-7);
        var from = since ?? week;
        var crashFrom = from < week ? from : week;
        var errors = new List<Error>();
        Func<string, (string Mod, string File)?> map = Mapper(docs, mods);
        var sessions = SessionHistory.List();
        var apexReport = ApexReport(docs);
        // (every crash log: a mod that writes one log per script error must not push the few crashes out of the cap)
        IEnumerable<FileInfo> logs;
        var unread = 0;
        try
        {
            var all = new DirectoryInfo(docs).EnumerateFiles().Where(f => IsLog(f.Name) && f.LastWriteTime >= (IsCrash(f.Name) ? crashFrom : from)).OrderByDescending(f => f.LastWriteTime).ToList();
            var crashes = all.Where(f => IsCrash(f.Name)).ToList();
            // (a removed mod's own logs are not read, so they never fill the cap)
            var scripts = all.Where(f => !IsCrash(f.Name) && (mods is null || Writer(f.Name) is not { } w || Installed(mods, w))).ToList();
            unread = Math.Max(0, scripts.Count - MaxFiles);
            logs = crashes.Concat(scripts.Take(MaxFiles)).ToList();
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { logs = []; }
        foreach (var f in logs)
        {
            string text;
            try
            {
                using var s = new FileStream(f.FullName, FileMode.Open, FileAccess.Read, FileShare.ReadWrite | FileShare.Delete);
                var buf = new byte[Math.Min(s.Length, MaxBytes)];
                s.ReadAtLeast(buf, buf.Length, throwOnEndOfStream: false);
                text = System.Text.Encoding.UTF8.GetString(buf);
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { continue; }
            errors.AddRange(IsCrash(f.Name) ? Native(text, f, mods, sessions, apexReport) : Script(text, f, map));
        }
        // script errors of a mod no longer installed (no file or folder of that name among the mods) are nothing to act on:
        // its old logs drop out. Crashes always stay (one in d3d9.dll is the renderer's, not a mod's).
        if (mods is not null) errors = errors.Where(e => e.Crash || e.Mod is not { } name || Installed(mods, name)).ToList();
        var byMod = errors.GroupBy(e => (Mod: e.Mod ?? "", e.Earlier, e.Why)).Select(g => new ModErrors(g.Key.Mod == "" ? null : g.Key.Mod, g.SelectMany(e => e.Files).Distinct().ToList(), g.Count(),
                g.Count(e => e.Crash), g.Max(e => e.When), g.GroupBy(e => e.Exception).OrderByDescending(x => x.Count()).First().Key, g.Key.Earlier, g.Key.Why))
            .OrderByDescending(m => m.Newest).ThenByDescending(m => m.Count).ToList();
        return new(from, errors.OrderByDescending(e => e.When).ToList(), byMod, unread);
    }

    // from when Health counts script errors: the last game start, at most a week back (a game started without the launcher
    // records no start, so the last recorded one can be long ago). It only moves forward.
    public static DateTime Since() => DateTime.Now.AddDays(-7) is var week && SessionHistory.List().FirstOrDefault()?.Started is { } s && s > week ? s : week;

    // the script error logs Health no longer reads (a crash log is never one: the Stability timeline goes by them)
    public static List<string> OldLogs(string docs)
    {
        try
        {
            var since = Since();
            return new DirectoryInfo(docs).EnumerateFiles().Where(f => IsLog(f.Name) && !IsCrash(f.Name) && f.LastWriteTime < since).Select(f => f.FullName).ToList();
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { return []; }
    }

    static bool Installed(IReadOnlyList<ModFile> mods, string mod) => mods.Any(m => Path.GetFileNameWithoutExtension(m.Name).StartsWith(mod, StringComparison.OrdinalIgnoreCase)
        || m.Rel.Split(Path.DirectorySeparatorChar).Any(part => part.Equals(mod, StringComparison.OrdinalIgnoreCase)));

    // a mod's own log (<Mod>_Error_...) names the mod that wrote it; ErrorTrap's names nobody
    static string? Writer(string log) => log.StartsWith("ScriptError_", StringComparison.OrdinalIgnoreCase) || IsCrash(log) ? null
        : log[..log.IndexOf("_Error_", StringComparison.OrdinalIgnoreCase)];

    static bool IsCrash(string name) => name.StartsWith("xcpt ", StringComparison.OrdinalIgnoreCase) && name.EndsWith(".txt", StringComparison.OrdinalIgnoreCase);

    static bool IsLog(string name) =>
        name.StartsWith("ScriptError_", StringComparison.OrdinalIgnoreCase) && name.EndsWith(".xml", StringComparison.OrdinalIgnoreCase)
        || name.Contains("_Error_", StringComparison.OrdinalIgnoreCase) && name.EndsWith(".xml", StringComparison.OrdinalIgnoreCase)
        || name.StartsWith("xcpt ", StringComparison.OrdinalIgnoreCase) && name.EndsWith(".txt", StringComparison.OrdinalIgnoreCase);

    static IEnumerable<Error> Script(string text, FileInfo f, Func<string, (string Mod, string File)?> map)
    {
        var writer = Writer(f.Name);
        var starts = ExceptionLine.Matches(text).ToList();
        for (var i = 0; i < starts.Count; i++)
        {
            var end = i + 1 < starts.Count ? starts[i + 1].Index : text.Length;
            string? mod = null, file = null, guess = null, below = null;
            var game = 0;
            // The mod the throw is in, or the one that called the game code that threw, at most Near frames up. A mod further
            // down the stack only started the Sim's action, and a hook that runs the game's own method (Run over RunOrig, or an
            // override over its base) passes the call on: neither is the cause.
            foreach (Match m in Frame.Matches(text[starts[i].Index..end]))
            {
                var (type, method) = (m.Groups[1].Value, m.Groups[2].Value);
                if (Runtime.IsMatch(type)) continue;
                if (below is null || below != method && below != method + "Orig")
                {
                    if (map(type) is { } hit) { (mod, file) = hit; break; }
                    if (!GameType.IsMatch(Dedupe(type))) { guess = Dedupe(type).Split('.')[0]; break; }
                }
                if (++game > Near) break;
                below = method;
            }
            yield return new(f.LastWriteTime, mod ?? writer ?? guess, file is null ? [] : [file], starts[i].Groups[1].Value, false, f.FullName);
        }
    }

    // Mono prints a type as <namespace>.<namespace>.<name>: the namespace once
    static string Dedupe(string t)
    {
        for (var i = t.IndexOf('.'); i > 0; i = t.IndexOf('.', i + 1))
            if (t.AsSpan(i + 1).StartsWith(t.AsSpan(0, i + 1))) return t[(i + 1)..];
        return t;
    }

    // Apex Radiance's own crash report (framework/crash_report.cpp; only the newest is kept): when it was written, the file
    // that faulted and the files the code addresses on the stack point into, nearest first
    public sealed record ApexCrash(DateTime When, string Fault, List<string> Refs);

    public static ApexCrash? ApexReport(string docs)
    {
        try
        {
            var path = Path.Combine(Scan.ApexDir(docs), "ApexRadiance_Crash.txt");
            if (!File.Exists(path)) return null;
            var text = File.ReadAllText(path);
            var when = Regex.Match(text, @"crash report, (\d{4}-\d\d-\d\d \d\d:\d\d:\d\d)");
            var fault = Regex.Match(text, @"^Exception \w+ at [0-9A-Fa-f]+ ([^\s+]+)\+0x", RegexOptions.Multiline);
            if (!when.Success || !fault.Success) return null;
            var refs = Regex.Matches(text, @"^\s+\+[0-9A-Fa-f]+: [0-9A-Fa-f]+ ([^\s+]+\.(?:asi|dll|exe))\+0x", RegexOptions.Multiline | RegexOptions.IgnoreCase).Select(m => m.Groups[1].Value).ToList();
            return new(DateTime.Parse(when.Groups[1].Value, System.Globalization.CultureInfo.InvariantCulture), fault.Groups[1].Value, refs);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or FormatException) { return null; }
    }

    // the game's exe and Windows' files: a fault there is in the game's own code unless a mod called it with bad data
    static bool GameOrWindows(string file) =>
        Regex.IsMatch(Path.GetFileName(file), @"^TS3W?\.exe$", RegexOptions.IgnoreCase)
        || Path.IsPathRooted(file) && file.StartsWith(Environment.GetFolderPath(Environment.SpecialFolder.Windows), StringComparison.OrdinalIgnoreCase)
        || !Path.IsPathRooted(file) && File.Exists(Path.Combine(Environment.SystemDirectory, file)) && !file.Equals("d3d9.dll", StringComparison.OrdinalIgnoreCase);

    static bool HookLayer(string file) => Regex.IsMatch(Path.GetFileName(file), @"^(ApexRadiance|ts3native)\.asi$", RegexOptions.IgnoreCase);

    // The game's crash log, read with everything that says who did it:
    // - the file that faulted ("address: 0x... "<path>":...") is the first suspect; a file further down the call stack is not
    //   (Apex Radiance and Sims 3 Modernization hook the game's frame and sit in nearly every stack without having done anything);
    // - a fault in the game's code or Windows' is the game's own, unless the frames right above it are a mod's: that mod called
    //   it. A fault in Apex Radiance's or Sims 3 Modernization's hooks with another mod's frames right above is that mod's call;
    // - Apex Radiance's own report of the same crash fills a call stack the game's log lost;
    // - the game's memory nearly full in that session (the launcher's session record) is the cause, whatever faulted;
    // - a file replaced since the crash is an update since: the crash is history.
    static IEnumerable<Error> Native(string text, FileInfo f, IReadOnlyList<ModFile>? mods, List<SessionInfo> sessions, ApexCrash? apex)
    {
        if (!text.Contains("[Exception info]")) yield break;
        var type = Regex.Match(text, @"^type:\s*([A-Z_]+)", RegexOptions.Multiline).Groups[1].Value;
        if (type.Length == 0) type = "crash";
        var fault = Regex.Match(text, @"^address:\s*0x[0-9a-fA-F]+\s+""([^""]+)""", RegexOptions.Multiline).Groups[1].Value;
        var stack = text.IndexOf("[Call stack]", StringComparison.Ordinal) is var at and >= 0 ? text[at..] : "";
        if (stack.IndexOf("[Stack data]", StringComparison.Ordinal) is var end and > 0) stack = stack[..end];
        var frames = Regex.Matches(stack, @"^([A-Za-z]:\\[^\r\n]+\.(?:asi|dll|exe))\s*$", RegexOptions.Multiline | RegexOptions.IgnoreCase).Select(m => m.Groups[1].Value).ToList();
        var bin = fault.Length > 0 ? Path.GetDirectoryName(fault) : null;
        // (the game's log lost the stack: Apex Radiance's report of the same moment has it, as file names in Game\Bin)
        if (frames.Count <= 1 && apex is not null && Math.Abs((apex.When - f.LastWriteTime).TotalSeconds) < 15 && bin is not null)
            frames = [fault, .. apex.Refs.Select(r => File.Exists(Path.Combine(bin, r)) ? Path.Combine(bin, r) : r)];
        if (fault.Length == 0 && frames.Count > 0) fault = frames[0];

        // the frames right above the fault (its caller and the one above): a mod among them is the one that called
        var near = frames.Skip(1).Take(3).ToList(); // (raw frames: a run of the faulting file's own frames is its own code)
        string? caller = near.FirstOrDefault(x => !GameOrWindows(x) && !x.Equals(fault, StringComparison.OrdinalIgnoreCase)
                                                  && (!HookLayer(x) || !HookLayer(fault)));
        string? module = null, why = null, into = type;
        if (fault.Length > 0 && !GameOrWindows(fault))
        {
            module = fault;
            // (a hook passes calls on: a crash inside one, called straight from another mod, is that mod's call)
            if (HookLayer(fault) && caller is not null && !HookLayer(caller)) { why = "called"; into = NameOf(fault); module = caller; }
        }
        else if (caller is not null) { module = caller; why = "called"; into = Path.GetFileName(fault); } // (TS3W.exe, or the Windows file)

        // the game's memory nearly full in the session that crashed: that is the cause, not the file the allocation failed in
        var session = sessions.FirstOrDefault(s => s.Started <= f.LastWriteTime && f.LastWriteTime <= s.Started + s.Played + TimeSpan.FromMinutes(5));
        if (session is not null && session.PeakBytes >= Session.LowMemoryBytes * 9 / 10)
        {
            yield return new(f.LastWriteTime, null, [], $"{session.PeakBytes >> 20}", true, f.FullName, false, "memory");
            yield break;
        }
        var file = module is null ? null : mods?.FirstOrDefault(m => m.Path.Equals(module, StringComparison.OrdinalIgnoreCase))?.Path ?? module;
        // (the file was replaced after the crash: an update since, so the crash is history)
        var earlier = module is not null && File.Exists(module) && File.GetLastWriteTime(module) > f.LastWriteTime.AddMinutes(1);
        yield return new(f.LastWriteTime, module is null ? null : NameOf(module), file is null ? [] : [file], into, true, f.FullName, earlier, why);
    }

    // what a faulting file is, by its version info: a d3d9.dll is the renderer put in Game\Bin, not "a mod"
    public static string NameOf(string path)
    {
        var name = Path.GetFileName(path);
        if (name.Equals("ApexRadiance.asi", StringComparison.OrdinalIgnoreCase)) return "Apex Radiance";
        try
        {
            var v = System.Diagnostics.FileVersionInfo.GetVersionInfo(path);
            var what = $"{v.ProductName} {v.FileDescription} {v.CompanyName}";
            var known = what.Contains("remix", StringComparison.OrdinalIgnoreCase) ? "RTX Remix bridge"
                : what.Contains("dxvk", StringComparison.OrdinalIgnoreCase) ? "DXVK"
                : what.Contains("reshade", StringComparison.OrdinalIgnoreCase) ? "ReShade"
                : null;
            return known is null ? name : $"{known} ({name})";
        }
        catch (Exception e) when (e is FileNotFoundException or IOException or UnauthorizedAccessException) { return name; }
    }

    // ---------- type name -> the script mod that defines it ----------
    sealed record Assemblies(long Size, DateTime Time, string[] Types);
    static readonly Dictionary<string, Assemblies> Seen = new(StringComparer.OrdinalIgnoreCase); // (per package; read again when it changes)

    static Func<string, (string Mod, string File)?> Mapper(string docs, IReadOnlyList<ModFile>? mods)
    {
        Dictionary<string, string>? types = null;
        return t =>
        {
            types ??= TypeMap(mods?.Where(m => m.Enabled && m.Path.EndsWith(".package", StringComparison.OrdinalIgnoreCase)).Select(m => m.Path)
                              ?? (Directory.Exists(Path.Combine(docs, "Mods")) ? Directory.EnumerateFiles(Path.Combine(docs, "Mods"), "*.package", SearchOption.AllDirectories) : []));
            // (the whole name, then without Mono's doubled namespace; never a shorter suffix, which is another type's name)
            return types.TryGetValue(t, out var file) || types.TryGetValue(Dedupe(t), out file) ? (Path.GetFileNameWithoutExtension(file), file) : null;
        };
    }

    static Dictionary<string, string> TypeMap(IEnumerable<string> packages)
    {
        var map = new Dictionary<string, string>(StringComparer.Ordinal);
        foreach (var p in packages)
        {
            Assemblies a;
            try
            {
                var fi = new FileInfo(p);
                lock (Seen)
                    if (!Seen.TryGetValue(p, out a!) || a.Size != fi.Length || a.Time != fi.LastWriteTimeUtc)
                        Seen[p] = a = new(fi.Length, fi.LastWriteTimeUtc, TypesIn(p));
            }
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { continue; }
            foreach (var t in a.Types) map.TryAdd(t, p);
        }
        return map;
    }

    // the type names of every S3SA assembly in a package; the game's own assemblies, as a core mod carries them, are left out
    static string[] TypesIn(string package) => Read(package, r =>
    {
        if (Regex.IsMatch(r.GetString(r.GetAssemblyDefinition().Name), @"^(Sims3GameplaySystems|Sims3GameplayObjects|Sims3StoreObjects|Sims3Metadata|ScriptCore|SimIFace|UI|mscorlib|System(\.Xml)?)$")) return [];
        string Full(TypeDefinitionHandle h)
        {
            var d = r.GetTypeDefinition(h);
            var outer = d.GetDeclaringType();
            var ns = r.GetString(d.Namespace);
            return !outer.IsNil ? Full(outer) + "+" + r.GetString(d.Name) : ns.Length > 0 ? ns + "." + r.GetString(d.Name) : r.GetString(d.Name);
        }
        return r.TypeDefinitions.Select(Full).Where(n => n != "<Module>").ToArray();
    }).SelectMany(t => t).ToArray();

    // each S3SA assembly in a package: its name and the names of the assemblies it references
    internal static List<(string Name, string[] Refs)> ReferencesIn(string package) => Read(package, r =>
        (r.GetString(r.GetAssemblyDefinition().Name), r.AssemblyReferences.Select(h => r.GetString(r.GetAssemblyReference(h).Name)).ToArray()));

    // one result per S3SA assembly in a package (ModConflicts' reader and decryption); a damaged one is left out
    static List<T> Read<T>(string package, Func<MetadataReader, T> read)
    {
        var list = new List<T>();
        try
        {
            var pkg = ModConflicts.Package.Open(package);
            foreach (var e in pkg.Entries.Where(e => e.Type == ModConflicts.TypeS3sa))
                try
                {
                    var b = pkg.Read(e);
                    if ((ModConflicts.PeImage(b) ?? ModConflicts.PeImage(ModConflicts.DecryptS3sa(b))) is not { } img) continue;
                    using var pe = new PEReader(ImmutableArray.Create(img));
                    if (pe.HasMetadata) list.Add(read(pe.GetMetadataReader()));
                }
                catch (Exception x) when (x is BadImageFormatException or InvalidDataException or IndexOutOfRangeException or ArgumentException or InvalidOperationException or OverflowException or InvalidCastException) { }
        }
        catch (Exception e) when (e is InvalidDataException or IOException or ArgumentException or IndexOutOfRangeException) { }
        return list;
    }

    // each with the files of the mod it names (to show it in Mods)
    public const int CrashPattern = 3;
    public static List<ModFinding> Findings(Report r)
    {
        var f = new List<ModFinding>();
        foreach (var m in r.ByMod.Take(8))
        {
            // (unverified cut: one stray error is common with ErrorTrap. Errors no mod is behind ask nothing of the player)
            var sev = m.Mod is not null && m.Count - m.Crashes >= 5 ? Severity.Warning : Severity.Info;
            // the game crashes now and then without any mod: one or two in a place is a note, three a pattern worth a look
            if (m.Crashes > 0 && m.Why == "memory")
            {
                // (the memory was nearly full: no file is blamed for where the allocation happened to fail)
                var full = r.Errors.Where(e => e.Crash && e.Why == "memory").Max(e => e.When).ToString("g");
                f.Add(new(new(m.Crashes >= 2 ? Severity.Warning : Severity.Info, m.Crashes == 1
                        ? "The game crashed once with its memory nearly full (peak {1} MB of 4096), on {2}. Fewer mods, less custom content or a smaller world help."
                        : "The game crashed {0} times with its memory nearly full (peak {1} MB of 4096), last on {2}. Fewer mods, less custom content or a smaller world help.",
                    [m.Crashes.ToString(), m.Exception, full]), m.Files));
            }
            else if (m.Crashes > 0 && m.Earlier)
            {
                // (crashes in a file updated since: a note, never a pattern; nothing has crashed there since the update)
                var before = r.Errors.Where(e => e.Crash && e.Earlier && e.Mod == m.Mod && e.Why == m.Why).Max(e => e.When).ToString("g");
                f.Add(new(new(Severity.Info, m.Crashes == 1 ? "{1} crashed the game once on {2}, before it was updated. It has not crashed since."
                        : "{1} crashed the game {0} times before it was updated, last on {2}. It has not crashed since.",
                    [m.Crashes.ToString(), m.Mod!, before]), m.Files));
            }
            else if (m.Crashes > 0 && m.Why == "called" && m.Mod is not null)
            {
                // (the fault was in the game's code or a hook, called straight from this mod)
                var last = r.Errors.Where(e => e.Crash && !e.Earlier && e.Mod == m.Mod && e.Why == m.Why).Max(e => e.When).ToString("g");
                f.Add(new(new(m.Crashes >= CrashPattern ? Severity.Warning : Severity.Info, m.Crashes == 1
                        ? "The game crashed once in a call {1} made into {3}, on {2}."
                        : "{1} crashed the game {0} times in the past week, in calls to {3}. The last one was on {2}.",
                    [m.Crashes.ToString(), m.Mod, last, m.Exception]), m.Files));
            }
            else if (m.Crashes > 0)
            {
                var last = r.Errors.Where(e => e.Crash && !e.Earlier && e.Mod == m.Mod && e.Why == m.Why).Max(e => e.When).ToString("g");
                f.Add(new(new(m.Crashes >= CrashPattern ? Severity.Warning : Severity.Info, m.Crashes == 1
                        ? m.Mod is null ? "The game crashed once in its own code, on {1}." : "The game crashed once in {1}, on {2}."
                        : m.Mod is null ? "The game crashed {0} times in its own code in the past week. The last one was on {1}." : "{1} crashed the game {0} times in the past week. The last one was on {2}.",
                    m.Mod is null ? [m.Crashes.ToString(), last] : [m.Crashes.ToString(), m.Mod, last]), m.Files));
            }
            if (m.Count - m.Crashes is var n and > 0)
                f.Add(new(new(sev, r.Unread > 0
                        ? m.Mod is null ? "Script errors in the game's own code: at least {0} since {1}. No mod appears as the cause." : "Script errors in {1}: at least {0} since {2}. If it causes problems, update it or turn it off."
                        : m.Mod is null ? "Script errors in the game's own code: {0} since {1}. No mod appears as the cause." : "Script errors in {1}: {0} since {2}. If it causes problems, update it or turn it off.",
                    m.Mod is null ? [n.ToString(), r.Since.ToString("d")] : [n.ToString(), m.Mod, r.Since.ToString("d")]), m.Files));
        }
        return f;
    }
}
