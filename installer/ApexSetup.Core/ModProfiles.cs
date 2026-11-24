using System.Runtime.CompilerServices;
using System.Text.Json;

namespace ApexSetup;

// A profile: which of the player's mods are on, under a name. It holds every mod the player had when it was saved, on or
// off, by its place as the game sees it (ModFile.Rel); a mod added later is not in it and is left as it is. Mods the
// launcher manages are never in one. Kept in %LOCALAPPDATA%\Apex Launcher\mod-profiles.json.
public sealed class ModProfile
{
    public string Name { get; set; } = "";
    public Dictionary<string, bool> Mods { get; set; } = [];
}

public sealed class ModProfilesFile
{
    public string? Active { get; set; }
    public List<ModProfile> Profiles { get; set; } = [];
}

public static class ModProfiles
{
    // what switching to a profile does: On is how many of the mods here it has on, Gone its mods (on) that are no longer
    // here, Left the mods here it can't tell and leaves as they are
    public sealed record Plan(List<ModFile> TurnOn, List<ModFile> TurnOff, int On, List<string> Gone, List<ModFile> Left)
    {
        public bool Same => TurnOn.Count + TurnOff.Count == 0;
    }

    public const int MaxName = 40;

    static string FilePath => Path.Combine(Paths.Data, "mod-profiles.json");
    static ModProfilesFile? _file;

    static ModProfilesFile Data
    {
        get
        {
            if (_file is not null) return _file;
            ModProfilesFile? f = null;
            var readable = true;
            try { if (File.Exists(FilePath)) f = JsonSerializer.Deserialize(File.ReadAllText(FilePath), SetupJson.Default.ModProfilesFile); }
            // (a damaged file is kept beside as .corrupt before anything is written over it)
            catch (JsonException e) { Engine.Log(e); try { File.Copy(FilePath, FilePath + ".corrupt", overwrite: true); } catch (Exception c) when (c is IOException or UnauthorizedAccessException) { readable = false; } }
            // (a file locked for a moment: read again next time, and nothing is saved over it meanwhile)
            catch (Exception e) when (e is IOException or UnauthorizedAccessException) { Engine.Log(e); readable = false; }
            f ??= new();
            f.Profiles = (f.Profiles ?? []).Where(p => !string.IsNullOrWhiteSpace(p?.Name)).DistinctBy(p => p.Name, StringComparer.OrdinalIgnoreCase).ToList();
            foreach (var p in f.Profiles)
            {
                var mods = new Dictionary<string, bool>(StringComparer.OrdinalIgnoreCase);
                foreach (var (rel, on) in p.Mods ?? []) mods[rel] = on;
                p.Mods = mods;
            }
            return readable ? _file = f : f;
        }
    }

    // a change is saved or it did not happen: a file that can't be read is never written over, and one that can't be written
    // is read again
    static T Edit<T>(Func<ModProfilesFile, T> change)
    {
        var f = Data;
        if (_file is null) throw new IOException(FilePath);
        try
        {
            var r = change(f);
            Save(f);
            return r;
        }
        catch { _file = null; throw; }
    }

    static void Save(ModProfilesFile f)
    {
        Directory.CreateDirectory(Paths.Data);
        Fs.WriteAtomic(FilePath, JsonSerializer.Serialize(f, SetupJson.Default.ModProfilesFile));
    }

    static bool Is(ModProfile p, string? name) => p.Name.Equals(name, StringComparison.OrdinalIgnoreCase);
    static bool Taken(ModProfilesFile f, string name) => f.Profiles.Any(p => Is(p, name));
    static ModProfile Named(ModProfilesFile f, string name) => f.Profiles.FirstOrDefault(p => Is(p, name)) ?? throw new SetupException("That profile no longer exists.");

    static string CleanName(string name)
    {
        name = new string(name.Where(c => !char.IsControl(c)).ToArray()).Trim();
        if (name.Length is 0 or > MaxName) throw new SetupException("A profile name must be 1 to 40 characters.");
        return name;
    }

    // the name, or the name with " (2)" and so on when a profile already has it
    static string FreeName(ModProfilesFile f, string name)
    {
        string Fit(string end) => (name.Length > MaxName - end.Length ? name[..(MaxName - end.Length)].TrimEnd() : name) + end;
        var free = Fit("");
        for (var i = 2; Taken(f, free); i++) free = Fit($" ({i})");
        return free;
    }

    [MethodImpl(MethodImplOptions.Synchronized)]
    public static List<ModProfile> All() => Data.Profiles.Select(p => new ModProfile { Name = p.Name, Mods = new(p.Mods, StringComparer.OrdinalIgnoreCase) }).ToList();

    // the profile last switched to or saved, whatever was turned on or off since
    [MethodImpl(MethodImplOptions.Synchronized)]
    public static string? Active() => Data is var f && f.Profiles.FirstOrDefault(p => Is(p, f.Active)) is { } a ? a.Name : null;

    [MethodImpl(MethodImplOptions.Synchronized)]
    public static void SetActive(string? name) => Edit(f => f.Active = name);

    [MethodImpl(MethodImplOptions.Synchronized)]
    public static bool Exists(string name) => Taken(Data, name.Trim());

    // what is on now under a new name; free: a taken name gets " (2)" instead of being refused
    [MethodImpl(MethodImplOptions.Synchronized)]
    public static string Create(string name, IReadOnlyList<ModFile> mods, bool free = false, bool activate = true)
    {
        NoHunt();
        return Edit(f =>
        {
            name = free ? FreeName(f, name) : CleanName(name);
            if (Taken(f, name)) throw new SetupException("A profile with that name already exists.");
            f.Profiles.Add(new ModProfile { Name = name, Mods = Snapshot(mods) });
            if (activate) f.Active = name;
            return name;
        });
    }

    [MethodImpl(MethodImplOptions.Synchronized)]
    public static void Rename(string name, string newName)
    {
        newName = CleanName(newName);
        Edit(f =>
        {
            var p = Named(f, name);
            if (!Is(p, newName) && Taken(f, newName)) throw new SetupException("A profile with that name already exists.");
            if (Is(p, f.Active)) f.Active = newName;
            return p.Name = newName;
        });
    }

    [MethodImpl(MethodImplOptions.Synchronized)]
    public static string Duplicate(string name) => Edit(f =>
    {
        var p = Named(f, name);
        var copy = new ModProfile { Name = FreeName(f, p.Name), Mods = new(p.Mods, StringComparer.OrdinalIgnoreCase) };
        f.Profiles.Insert(f.Profiles.IndexOf(p) + 1, copy);
        return copy.Name;
    });

    // the profile holds what is on now and is the active one; what it held before, for Undo
    [MethodImpl(MethodImplOptions.Synchronized)]
    public static Dictionary<string, bool> Update(string name, IReadOnlyList<ModFile> mods)
    {
        NoHunt();
        return Put(name, Snapshot(mods), name);
    }

    [MethodImpl(MethodImplOptions.Synchronized)]
    public static Dictionary<string, bool> Put(string name, Dictionary<string, bool> mods, string? active) => Edit(f =>
    {
        var p = Named(f, name);
        var was = p.Mods;
        p.Mods = mods;
        f.Active = active;
        return was;
    });

    [MethodImpl(MethodImplOptions.Synchronized)]
    public static void Delete(string name) => Edit(f => f.Profiles.Remove(Named(f, name)));

    // the mod moved or was renamed (a folder: every mod under it): every profile follows
    [MethodImpl(MethodImplOptions.Synchronized)]
    public static void Moved(string oldRel, string newRel, bool folder = false) => Follow(mods =>
    {
        var moves = mods.Keys.Where(k => folder ? k.StartsWith(oldRel + Path.DirectorySeparatorChar, StringComparison.OrdinalIgnoreCase) : k.Equals(oldRel, StringComparison.OrdinalIgnoreCase)).ToList();
        foreach (var k in moves)
        {
            mods.Remove(k, out var on);
            mods[newRel + k[oldRel.Length..]] = on;
        }
        return moves.Count > 0;
    });

    // a merge was split: its mods are as the merge was, in the profiles that had the merge
    [MethodImpl(MethodImplOptions.Synchronized)]
    public static void Split(string mergedRel, IEnumerable<string> originals) => Follow(mods =>
    {
        if (!mods.Remove(mergedRel, out var on)) return false;
        foreach (var o in originals) mods[o] = on;
        return true;
    });

    // (the files are already in their new places: a failure here is logged, and the next change saves what is in memory)
    static void Follow(Func<Dictionary<string, bool>, bool> change)
    {
        var f = Data;
        if (f.Profiles.Count(p => change(p.Mods)) == 0 || _file is null) return;
        try { Save(f); }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { Engine.Log(e); }
    }

    // the player's own packages; an on and an off mod at the same place share one Rel and can't be told apart
    static (List<ModFile> Mods, List<ModFile> Twins) Mine(IReadOnlyList<ModFile> mods)
    {
        var groups = mods.Where(m => m.FileType == "package" && !m.Locked).GroupBy(m => m.Rel, StringComparer.OrdinalIgnoreCase).ToList();
        return (groups.Where(g => g.Count() == 1).Select(g => g.First()).ToList(), groups.Where(g => g.Count() > 1).SelectMany(g => g).ToList());
    }

    static Dictionary<string, bool> Snapshot(IReadOnlyList<ModFile> mods) => Mine(mods).Mods.ToDictionary(m => m.Rel, m => m.Enabled, StringComparer.OrdinalIgnoreCase);

    public static Plan PlanFor(ModProfile p, IReadOnlyList<ModFile> mods, string docs)
    {
        var (mine, left) = Mine(mods);
        var here = mine.Concat(left).Select(m => m.Rel).ToHashSet(StringComparer.OrdinalIgnoreCase);
        var plan = new Plan([], [], 0, [], left);
        var on = 0;
        // a merge made since is on or off as its mods are in the profile, when they all agree (one of them can be a merge too)
        bool? Want(ModFile m, int depth = 0)
        {
            here.Add(m.Rel);
            bool? known = p.Mods.TryGetValue(m.Rel, out var w) ? w : null;
            if (depth > 8 || ModMerge.Info(docs, m) is not { } merge) return known;
            var inside = merge.Originals.Select(o => Want(m with { Rel = o.Rel }, depth + 1)).Distinct().ToList();
            return known ?? (inside.Count == 1 ? inside[0] : null);
        }
        foreach (var m in mine)
        {
            if (Want(m) is not { } to) { plan.Left.Add(m); continue; }
            if (to) on++;
            if (to != m.Enabled) (to ? plan.TurnOn : plan.TurnOff).Add(m);
        }
        plan.Gone.AddRange(p.Mods.Where(x => x.Value && !here.Contains(x.Key)).Select(x => x.Key).Order(StringComparer.OrdinalIgnoreCase));
        return plan with { On = on };
    }

    static void NoHunt() { if (ModHunt.Current() is not null) throw new SetupException("Finish finding the problem mod first."); }

    // what stops a switch before anything is asked: the game running, a hunt
    public static void CheckCanSwitch()
    {
        Engine.CheckWritable();
        NoHunt();
    }

    // Every mod switched, or none: one that can't be (held open by another program) puts the ones already switched back.
    // The mods as they are now, to switch back with.
    public static List<ModFile> Apply(IReadOnlyList<ModFile> turnOn, IReadOnlyList<ModFile> turnOff, string docs)
    {
        CheckCanSwitch();
        var done = new List<ModFile>();
        ModFile Set(ModFile m, bool on)
        {
            var path = Mods.SetEnabled(m, on, docs);
            return m with { Path = path, Rel = Path.Combine(m.Folder, Path.GetFileName(path)), Enabled = on };
        }
        try
        {
            foreach (var m in turnOff) done.Add(Set(m, false));
            foreach (var m in turnOn) done.Add(Set(m, true));
        }
        catch
        {
            var stuck = new List<string>();
            for (var i = done.Count - 1; i >= 0; i--)
                try { Set(done[i], !done[i].Enabled); }
                catch (Exception e) { Engine.Log(e); stuck.Add(done[i].Name); }
            if (stuck.Count > 0) throw new SetupException("Not every mod could be put back. Check these: {0}", string.Join(", ", stuck));
            throw;
        }
        return done;
    }

    // for tests: read the file again
    public static void Reload() => _file = null;
}
