using System.Globalization;

namespace ApexSetup;

// Sims3SettingsSetter's settings that go well with Apex Radiance (Documents\...\S3SS\S3SS.toml): which of its patches are on.
// Everything else in the file stays. Its lighting and texture patches are off: Apex Radiance does the lighting, and
// uncompressed textures stutter under the bridge.
public static class SetterSettings
{
    static readonly string[] On =
        ["CPUOptimization", "CreateFileRandomAccess", "ExpandedCrashLogs", "Mimalloc", "OversizedThreadStackFix", "RefPackDecompressor", "StartupWarningDialogFix", "TimerOptimization"];
    static readonly string[] Off =
        ["AdaptiveWait", "AnimationBlendPatch", "BradyBunchBegone", "GCFinalizeThrottle", "GCStopWorld", "GCTryToCollect", "LightingQualityPatch", "LotStreamingOptimizations", "MirrorSettings",
         "ResolutionSpoofer", "SplitLevelLightingFix", "StoreFeaturedItemsDisablePatch", "UncompressedLotTexturesPatch", "UncompressedSimTexturesPatch", "WorldCacheSizePatch"];
    const string Smooth = "SmoothPatchPrecise", OldSmooth = "SmoothPatchClassic";
    static readonly string[] SmoothKeys = ["enabled", "frameRateLimit", "frameRateLimitInactive", "tickRateLimit"];

    public static string FileOf(string docs) => Path.Combine(docs, "S3SS", "S3SS.toml");
    static string? Value(string text, string patch, string key) => GameSettings.IniValue(text, "patches." + patch, key);

    // the patches that would change ("enabled" missing is off, its default)
    public static (List<string> TurnOn, List<string> TurnOff) Differences(string text) =>
        (On.Where(p => Value(text, p, "enabled") != "true").ToList(), Off.Where(p => Value(text, p, "enabled") == "true").ToList());

    // Smooth Patch depends on the PC and its screen: suggested only where neither version is on, and never changed when one is
    public static bool SmoothOn(string text) => Value(text, Smooth, "enabled") == "true" || Value(text, OldSmooth, "enabled") == "true";

    // smoothHz: also Smooth Patch at its own 480 ticks, frames at the screen's rate
    public static string With(string text, int? smoothHz)
    {
        if (text.Trim().Length == 0) text = "[meta]\nversion = 1\n";
        var (on, off) = Differences(text);
        foreach (var p in on) text = Set(text, p, "enabled", "true");
        foreach (var p in off) text = Set(text, p, "enabled", "false");
        if (smoothHz is not { } hz || SmoothOn(text)) return text;
        var rate = Math.Clamp(hz, 30, 500).ToString(CultureInfo.InvariantCulture) + ".0";
        foreach (var (k, v) in SmoothKeys.Zip(["true", rate, rate, "480.0"])) text = Set(text, Smooth, k, v);
        return text;
    }

    // Restore my files: the patches' switches and Smooth Patch's values as the copy has them
    public static string Back(string text, string was)
    {
        foreach (var p in On.Concat(Off).Append(Smooth))
            foreach (var k in p == Smooth ? SmoothKeys : ["enabled"])
                if (Value(text, p, k) is not null && (Value(was, p, k) ?? (k == "enabled" ? "false" : null)) is { } v) text = Set(text, p, k, v);
        return text;
    }

    public static string Read(string docs)
    {
        try { return File.Exists(FileOf(docs)) ? GameSettings.ReadText(FileOf(docs)) : ""; }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException) { Engine.Log(e); return ""; }
    }

    // the file with the suggestion; one that is not there yet is made (the mod fills in the rest)
    public static void Apply(string docs, int? smoothHz)
    {
        Engine.CheckWritable();
        var f = FileOf(docs);
        var there = File.Exists(f);
        var text = there ? GameSettings.ReadText(f) : "";
        var next = With(text, smoothHz);
        if (next == text) return;
        if (there) GameSettings.KeepDocs(f, docs);
        else
        {
            Directory.CreateDirectory(Path.GetDirectoryName(f)!);
            GameSettings.KeepNone(f, docs); // (Restore my files switches off what it turned on)
        }
        GameSettings.WriteText(f, next);
    }

    static string Set(string text, string patch, string key, string value)
    {
        if (!text.Split('\n').Any(l => l.Trim() == $"[patches.{patch}]"))
            text = text.TrimEnd() + (text.Contains("\r\n") ? $"\r\n\r\n[patches.{patch}]\r\n" : $"\n\n[patches.{patch}]\n");
        return GameSettings.WithIni(text, "patches." + patch, key, value);
    }
}
