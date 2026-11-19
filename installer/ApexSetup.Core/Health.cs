namespace ApexSetup;

public enum HealthState { Ready, NeedsYou, Blocked }
public enum HealthArea { ApexRadiance, Loaders, Graphics, GameSettings, Mods }

// What Health and Home show: every finding (Compat's, Verify's, the mod checks') grouped by area, and one state for the
// crystal: blocked when Apex Radiance cannot run and nothing fixes it, needs you when something waits for the player.
public static class Health
{
    public sealed record Item(Finding Finding, HealthArea Area, IReadOnlyList<string> Files);

    public static List<Item> Gather(Game g, State s, string docs, Release? apex, IReadOnlyList<ModFile>? mods = null)
    {
        var items = Verify.Files(g, s, apex).Concat(Compat.Check(g, s, docs)).Select(f => new Item(f, AreaOf(f), [])).ToList();
        if (mods is not null) items.AddRange(ModChecks.Run(mods, g, docs).Select(m => new Item(m.Finding, HealthArea.Mods, m.Files)));
        return items;
    }

    public static HealthState StateOf(IEnumerable<Item> items)
    {
        var list = items.ToList();
        if (list.Any(i => i.Finding.Severity == Severity.Error && !i.Finding.CanFix)) return HealthState.Blocked;
        return list.Any(i => i.Finding.Severity != Severity.Info) ? HealthState.NeedsYou : HealthState.Ready;
    }

    // shortcut: the area read from the finding's English key; a finding that should land elsewhere needs an Area on Finding
    public static HealthArea AreaOf(Finding f)
    {
        var t = f.Text;
        bool Has(params string[] w) => w.Any(x => t.Contains(x, StringComparison.OrdinalIgnoreCase));
        if (Has("bridge", "DXVK", "dxvk.conf", "d3d9", "Vulkan")) return HealthArea.Graphics;
        if (Has("ASI loader", "loaders", "ts3native", "Modernization")) return HealthArea.Loaders;
        if (Has("Options.ini", "Resource.cfg", "S3SS", "Sims3SettingsSetter", "Sims3Performance", "edgesmoothing")) return HealthArea.GameSettings;
        if (Has("Apex Radiance", "Apex inside")) return HealthArea.ApexRadiance;
        return HealthArea.Mods;
    }
}
