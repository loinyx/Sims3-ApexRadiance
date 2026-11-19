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
    public sealed record Error(DateTime When, string? Mod, IReadOnlyList<string> Files, string Exception, bool Crash, string Log);
    public sealed record ModErrors(string? Mod, IReadOnlyList<string> Files, int Count, int Crashes, DateTime Newest, string Exception);
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
    static readonly Regex Frame = new(@"^#\d+:\s+0x[0-9a-fA-F]+\s+\S+\s+in\s+([^\s:(]+):", RegexOptions.Multiline);

    // the logs written since `since` (default: the last week), mapped to the player's mods (null: the Mods folder's packages)
    public static Report Read(string docs, DateTime? since = null, IReadOnlyList<ModFile>? mods = null)
    {
        var from = since ?? DateTime.Now.AddDays(-7);
        var errors = new List<Error>();
        Func<string, (string Mod, string File)?> map = Mapper(docs, mods);
        // (every crash log: a mod that writes one log per script error must not push the few crashes out of the cap)
        IEnumerable<FileInfo> logs;
        var unread = 0;
        try
        {
            var all = new DirectoryInfo(docs).EnumerateFiles().Where(f => IsLog(f.Name) && f.LastWriteTime >= from).OrderByDescending(f => f.LastWriteTime).ToList();
            var crashes = all.Where(f => IsCrash(f.Name)).ToList();
            var scripts = all.Where(f => !IsCrash(f.Name)).ToList();
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
            errors.AddRange(IsCrash(f.Name) ? Native(text, f, mods) : Script(text, f, map));
        }
        // script errors of a mod no longer installed (no file or folder of that name among the mods) are nothing to act on:
        // its old logs drop out. Crashes always stay (one in d3d9.dll is the renderer's, not a mod's).
        if (mods is not null)
        {
            bool Installed(string mod) => mods.Any(m => Path.GetFileNameWithoutExtension(m.Name).StartsWith(mod, StringComparison.OrdinalIgnoreCase)
                || m.Rel.Split(Path.DirectorySeparatorChar).Any(part => part.Equals(mod, StringComparison.OrdinalIgnoreCase)));
            errors = errors.Where(e => e.Crash || e.Mod is not { } name || Installed(name)).ToList();
        }
        var byMod = errors.GroupBy(e => e.Mod ?? "").Select(g => new ModErrors(g.Key == "" ? null : g.Key, g.SelectMany(e => e.Files).Distinct().ToList(), g.Count(),
                g.Count(e => e.Crash), g.Max(e => e.When), g.GroupBy(e => e.Exception).OrderByDescending(x => x.Count()).First().Key))
            .OrderByDescending(m => m.Newest).ThenByDescending(m => m.Count).ToList();
        return new(from, errors.OrderByDescending(e => e.When).ToList(), byMod, unread);
    }

    static bool IsCrash(string name) => name.StartsWith("xcpt ", StringComparison.OrdinalIgnoreCase) && name.EndsWith(".txt", StringComparison.OrdinalIgnoreCase);

    static bool IsLog(string name) =>
        name.StartsWith("ScriptError_", StringComparison.OrdinalIgnoreCase) && name.EndsWith(".xml", StringComparison.OrdinalIgnoreCase)
        || name.Contains("_Error_", StringComparison.OrdinalIgnoreCase) && name.EndsWith(".xml", StringComparison.OrdinalIgnoreCase)
        || name.StartsWith("xcpt ", StringComparison.OrdinalIgnoreCase) && name.EndsWith(".txt", StringComparison.OrdinalIgnoreCase);

    static IEnumerable<Error> Script(string text, FileInfo f, Func<string, (string Mod, string File)?> map)
    {
        // a mod's own log (<Mod>_Error_...) names the mod that wrote it; ErrorTrap's names nobody
        var writer = f.Name.StartsWith("ScriptError_", StringComparison.OrdinalIgnoreCase) ? null : f.Name[..f.Name.IndexOf("_Error_", StringComparison.OrdinalIgnoreCase)];
        var starts = ExceptionLine.Matches(text).ToList();
        for (var i = 0; i < starts.Count; i++)
        {
            var end = i + 1 < starts.Count ? starts[i + 1].Index : text.Length;
            string? mod = null, file = null, guess = null;
            // the frame closest to the throw that is a player's mod; else the first one outside the game's namespaces
            foreach (Match m in Frame.Matches(text[starts[i].Index..end]))
            {
                var type = m.Groups[1].Value;
                if (map(type) is { } hit) { (mod, file) = hit; break; }
                if (guess is null && !GameType.IsMatch(Dedupe(type))) guess = Dedupe(type).Split('.')[0];
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

    // the game's crash log: the module that faulted ("address: 0x... "<path>":..."), else the first module in the call stack
    // that is neither the game's exe nor Windows'
    static IEnumerable<Error> Native(string text, FileInfo f, IReadOnlyList<ModFile>? mods)
    {
        if (!text.Contains("[Exception info]")) yield break;
        var type = Regex.Match(text, @"^type:\s*([A-Z_]+)", RegexOptions.Multiline).Groups[1].Value;
        var fault = Regex.Match(text, @"^address:\s*0x[0-9a-fA-F]+\s+""([^""]+)""", RegexOptions.Multiline).Groups[1].Value;
        var stack = text.IndexOf("[Call stack]", StringComparison.Ordinal) is var at and >= 0 ? text[at..] : "";
        var module = new[] { fault }.Concat(Regex.Matches(stack, @"^([A-Za-z]:\\[^\r\n]+\.(?:asi|dll|exe))\s*$", RegexOptions.Multiline | RegexOptions.IgnoreCase).Select(m => m.Groups[1].Value))
            .FirstOrDefault(p => p.Length > 0 && !Regex.IsMatch(Path.GetFileName(p), @"^TS3W?\.exe$", RegexOptions.IgnoreCase)
                                 && !p.StartsWith(Environment.GetFolderPath(Environment.SpecialFolder.Windows), StringComparison.OrdinalIgnoreCase));
        var file = module is null ? null : mods?.FirstOrDefault(m => m.Path.Equals(module, StringComparison.OrdinalIgnoreCase))?.Path ?? module;
        yield return new(f.LastWriteTime, module is null ? null : Path.GetFileName(module), file is null ? [] : [file], type.Length > 0 ? type : "crash", true, f.FullName);
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
            // the whole name, then each shorter suffix (Mono's doubled namespace)
            for (var i = -1; i < t.Length; i = t.IndexOf('.', i + 1) is var n and >= 0 ? n : t.Length)
                if (types.TryGetValue(t[(i + 1)..], out var file)) return (Path.GetFileNameWithoutExtension(file), file);
            return null;
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

    // the type names of every S3SA assembly in a package (ModConflicts' reader and decryption); the game's own assemblies,
    // as a core mod carries them, are left out
    static string[] TypesIn(string package)
    {
        try
        {
            var pkg = ModConflicts.Package.Open(package);
            var list = new List<string>();
            foreach (var e in pkg.Entries.Where(e => e.Type == ModConflicts.TypeS3sa))
                try
                {
                    var b = pkg.Read(e);
                    if ((ModConflicts.PeImage(b) ?? ModConflicts.PeImage(ModConflicts.DecryptS3sa(b))) is not { } img) continue;
                    using var pe = new PEReader(ImmutableArray.Create(img));
                    if (!pe.HasMetadata) continue;
                    var r = pe.GetMetadataReader();
                    if (Regex.IsMatch(r.GetString(r.GetAssemblyDefinition().Name), @"^(Sims3GameplaySystems|Sims3GameplayObjects|Sims3StoreObjects|Sims3Metadata|ScriptCore|SimIFace|UI|mscorlib|System(\.Xml)?)$")) continue;
                    string Full(TypeDefinitionHandle h)
                    {
                        var d = r.GetTypeDefinition(h);
                        var outer = d.GetDeclaringType();
                        var ns = r.GetString(d.Namespace);
                        return !outer.IsNil ? Full(outer) + "+" + r.GetString(d.Name) : ns.Length > 0 ? ns + "." + r.GetString(d.Name) : r.GetString(d.Name);
                    }
                    list.AddRange(r.TypeDefinitions.Select(Full).Where(n => n != "<Module>"));
                }
                catch (Exception x) when (x is BadImageFormatException or InvalidDataException or IndexOutOfRangeException or ArgumentException or InvalidOperationException) { }
            return list.ToArray();
        }
        catch (Exception e) when (e is InvalidDataException or IOException or ArgumentException or IndexOutOfRangeException) { return []; }
    }

    // each with the files of the mod it names (to show it in Mods)
    public static List<ModFinding> Findings(Report r)
    {
        var f = new List<ModFinding>();
        foreach (var m in r.ByMod.Take(8))
        {
            var sev = m.Crashes > 0 || m.Count >= 5 ? Severity.Warning : Severity.Info; // (unverified cut: one stray error is common with ErrorTrap)
            if (m.Crashes > 0)
                f.Add(new(new(sev, m.Crashes == 1
                        ? m.Mod is null ? "The game crashed once in its own code since {1}." : "The game crashed once in {1} since {2}."
                        : m.Mod is null ? "The game crashed {0} times in its own code since {1}." : "The game crashed {0} times in {1} since {2}.",
                    m.Mod is null ? [m.Crashes.ToString(), r.Since.ToString("g")] : [m.Crashes.ToString(), m.Mod, r.Since.ToString("g")]), m.Files));
            if (m.Count - m.Crashes is var n and > 0)
                f.Add(new(new(sev, r.Unread > 0
                        ? m.Mod is null ? "At least {0} script errors in the game's own code since {1} ({2})." : "At least {0} script errors went through {1} since {2} ({3}). If it misbehaves, update or remove it."
                        : m.Mod is null ? "{0} script errors in the game's own code since {1} ({2})." : "{0} script errors went through {1} since {2} ({3}). If it misbehaves, update or remove it.",
                    m.Mod is null ? [n.ToString(), r.Since.ToString("g"), m.Exception] : [n.ToString(), m.Mod, r.Since.ToString("g"), m.Exception]), m.Files));
        }
        return f;
    }
}
