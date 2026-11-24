// Offline checks of the Game settings page's files (GameSettings, SetterSettings, DeviceCard) on made-up copies. APEX_HEALTH_GAME
// adds a run over the real game's files, copied first.
using System.Text;
using ApexSetup;

static class GameSettingsTests
{
    const string Rules = "if ($textureMemory == 0)\r\n  seti textureMemory       32\r\nendif\r\noption LightingQuality\r\n    setting $Medium\r\n        prop $ConfigGroup ShadowMapSize 1024\r\n"
        + "    setting $High\r\n        prop $ConfigGroup ShadowMapSize 2048\r\n        prop $ConfigGroup RenderShadowsEnabled true\r\nend\r\noption SimQuality\r\n\tsetting $High\r\n"
        + "\t    prop $ConfigGroup RenderSimTextureSizes \"1024, 1024, 256, 128\"\r\n\tsetting $VeryHigh\r\n\t    prop $ConfigGroup RenderSimTextureSizes \"2048, 2048, 512, 128\"\r\nend\r\n"
        + "option AdvancedRendering\r\n    setting $Off\r\n\t\tprop $ConfigGroup RenderPostProcessEnabled false\r\n    setting $On\r\n\t    prop $ConfigGroup RenderForceMinspecShaders 0\r\nend\r\n# café\r\n";
    const string Ini = "[ResourceSystem]\nMemoryUsageLimit = 20000000\n\n[Config]\nDynamicAvoidance_FieldRadius = 0.15\nDynamicAvoidance_StopDistanceMultiplier = 1.85\n\n[Routing]\nOther = 1\n";
    const string Options = "[options]\r\nenableingamestore = 1\r\n\r\nenableinteractiveloading = 1\r\n\r\nenablememories = 2\r\n\r\nreceiveconnecttns = 1\r\n\r\nvoicelevel = 255\r\n\r\n";

    static void Put(string f, string t) => File.WriteAllText(f, t, Encoding.Latin1);
    static string Get(string f) => File.ReadAllText(f, Encoding.Latin1);

    public static void Run(Action<bool, string> check)
    {
        var root = Directory.CreateTempSubdirectory("ApexGameSettingsTest-").FullName;
        var bin = Path.Combine(root, "Game", "Bin");
        var docs = Path.Combine(root, "Docs");
        Directory.CreateDirectory(bin);
        Directory.CreateDirectory(docs);
        var g = new Game(root, bin, "TS3W.exe", Edition.Steam, "1.67.2");
        string rules = Path.Combine(bin, "GraphicsRules.sgr"), ini = Path.Combine(bin, "Sims3.ini"), cards = Path.Combine(bin, "GraphicsCards.sgr"), options = Path.Combine(docs, "Options.ini");
        try
        {
            Put(rules, Rules); Put(ini, Ini); Put(options, Options);
            var now = GameSettings.Now(bin, docs);
            check(now["shadow"] == "2048" && now["sims"] == "2048" && now["bloom"] == "0" && now["reads"] == "20000000" && now["walk.radius"] == "0.15"
                  && !now.ContainsKey("walk.gap") && now["memories"] == "0" && now["online"] == "0", "game settings: the files' values are read, a missing line is left out");
            check(GameSettings.Changes(new Dictionary<string, string> { ["shadow"] = "16384", ["walk.radius"] = "-1", ["walk.stop"] = "1.850", ["nope"] = "1" }, now).Count == 0,
                "game settings: a value the page does not offer, or the one there, changes nothing");

            GameSettings.ApplyBin(g, docs, now);
            check(Get(rules) == Rules && Get(ini) == Ini && !GameSettings.CanRestore(bin, docs), "game settings: nothing wanted, nothing written or copied");

            var want = new Dictionary<string, string> { ["shadow"] = "8192", ["sims"] = "4096", ["bloom"] = "1", ["reads"] = "40000000", ["walk.radius"] = "0.1", ["store"] = "1", ["memories"] = "1", ["online"] = "1" };
            GameSettings.ApplyBin(g, docs, want);
            check(Get(options) == Options, "game settings: the game folder's part leaves Options.ini alone");
            check(GameSettings.ApplyOptions(g, docs, want), "game settings: Options.ini written");
            check(Get(rules) == Rules.Replace("ShadowMapSize 2048", "ShadowMapSize 8192").Replace("2048, 2048, 512", "4096, 4096, 512")
                    .Replace("    setting $On\r\n", "    setting $On\r\n\t\tprop $ConfigGroup RenderPostProcessEnabled false\r\n"), "game settings: only the chosen lines of GraphicsRules.sgr change");
            check(Get(ini) == Ini.Replace("= 20000000", "= 40000000").Replace("0.15", "0.1"), "game settings: only the chosen keys of Sims3.ini change");
            check(Get(options) == Options.Replace("enableingamestore = 1", "enableingamestore = 0").Replace("enablememories = 2", "enablememories = 3")
                    .Replace("receiveconnecttns = 1", "receiveconnecttns = 0").Replace("voicelevel = 255\r\n", "voicelevel = 255\r\nrequireloginbeforeload = 1\r\n"), "game settings: only the chosen keys of Options.ini change");
            now = GameSettings.Now(bin, docs);
            check(want.All(w => now[w.Key] == w.Value), "game settings: what was written reads back");

            GameSettings.ApplyBin(g, docs, new Dictionary<string, string> { ["shadow"] = "4096" });
            Put(options, Get(options).Replace("voicelevel = 255", "voicelevel = 10"));
            check(GameSettings.CanRestore(bin, docs), "game settings: the first copies are kept");
            GameSettings.RestoreBin(g);
            GameSettings.RestoreDocs(docs);
            check(Get(rules) == Rules && Get(ini) == Ini, "game settings: Restore puts the game's files back as they were before the first change");
            check(Get(options).Contains("enablememories = 2") && Get(options).Contains("enableingamestore = 1") && Get(options).Contains("voicelevel = 10"), "game settings: Restore puts back only its own Options.ini keys");
            check(!GameSettings.CanRestore(bin, docs), "game settings: nothing left to restore");

            GameSettings.ApplyBin(g, docs, new Dictionary<string, string> { ["bloom"] = "1" });
            GameSettings.ApplyBin(g, docs, new Dictionary<string, string> { ["bloom"] = "0" });
            check(Get(rules) == Rules, "game settings: bloom off and on again leaves the file as it was");

            File.Delete(options);
            check(GameSettings.Now(bin, docs)["memories"] == "0" && !GameSettings.ApplyOptions(g, docs, want), "game settings: without Options.ini the game's defaults show and nothing is written");
            GameSettings.CreateOptions(docs, new Dictionary<string, string> { ["memories"] = "1", ["store"] = "0", ["shadow"] = "8192" });
            check(Get(options) == "[options]\r\nenablememories = 3\r\n", "game settings: the first Play makes Options.ini with the choices alone");
            GameSettings.CreateOptions(docs, want);
            check(Get(options) == "[options]\r\nenablememories = 3\r\n", "game settings: an Options.ini that is there is never replaced");

            // Sims3SettingsSetter's settings
            const string toml = "[meta]\nversion = 1\n\n[patches.LightingQualityPatch]\nenabled = true\nshadowRadius = 1\n\n[patches.Mimalloc]\nenabled = false\n\n[qol.ui]\ntoggle_key = 45\n";
            var f = SetterSettings.FileOf(docs);
            Directory.CreateDirectory(Path.GetDirectoryName(f)!);
            Put(f, toml);
            SetterSettings.Apply(docs, 144);
            var t = Get(f);
            check(t.Contains("[patches.LightingQualityPatch]\nenabled = false\nshadowRadius = 1") && t.Contains("[patches.Mimalloc]\nenabled = true") && t.Contains("toggle_key = 45")
                  && t.Contains("[patches.SmoothPatchPrecise]") && t.Contains("frameRateLimit = 144.0") && t.Contains("tickRateLimit = 480.0"), "settings setter: the suggestion is applied, the rest stays");
            check(SetterSettings.Differences(t) is { TurnOn.Count: 0, TurnOff.Count: 0 } && SetterSettings.With(t, 60) == t, "settings setter: applied once, Smooth Patch that is on is not changed");
            GameSettings.RestoreDocs(docs);
            check(Get(f).Contains("[patches.LightingQualityPatch]\nenabled = true") && Get(f).Contains("[patches.Mimalloc]\nenabled = false") && !SetterSettings.SmoothOn(Get(f)), "settings setter: Restore puts its switches back");

            // the graphics card
            const string log = "Name (driver):   NVIDIA GeForce RTX \"9090\"\r\nName (database): Unknown [Found: 0, Matched: 0]\r\nChipset:         Vendor: 10de, Device: 2bad, Board: 0\r\nTexture memory:  32MB <<OVERRIDE>>\r\n";
            const string list = "vendor \"ATI\" 0x1002\r\n\tcard 0x2bad \"Other\"\r\nvendor \"NVIDIA\" 0x10b4 0x10de\r\n\tcard 0x1080 \"Geforce GTX 580\"\r\n";
            Put(cards, list); Put(rules, Rules);
            var card = DeviceCard.Parse(log)!;
            check(DeviceCard.State(card, bin) == CardState.NotRecognized, "graphics card: a card the game does not know is found");
            card.Fix(g);
            check(Get(cards) == list.Replace("0x10de\r\n", "0x10de\r\n\tcard 0x2bad \"NVIDIA GeForce RTX 9090\"\r\n") && Get(rules) == Rules.Replace("textureMemory       32", "textureMemory       1024"),
                "graphics card: added under its vendor, the video memory fallback raised");
            check(DeviceCard.State(card, bin) == CardState.Added && DeviceCard.State(card with { Found = true, MemoryGuessed = false }, bin) == CardState.Recognized
                  && DeviceCard.State(null, bin) == CardState.Unknown && DeviceCard.State(card with { Vendor = 0x1234, MemoryGuessed = false }, bin) == CardState.Unknown, "graphics card: added, recognized and unknown are told apart");
            GameSettings.RestoreBin(g);
            check(Get(cards) == list && Get(rules) == Rules, "graphics card: Restore takes the card back out");

            // the real game's files, as copies
            if (Environment.GetEnvironmentVariable("APEX_HEALTH_GAME") is { Length: > 0 } real && Directory.Exists(Path.Combine(real, "Game", "Bin")))
            {
                foreach (var n in new[] { "GraphicsRules.sgr", "Sims3.ini", "GraphicsCards.sgr" }) File.Copy(Path.Combine(real, "Game", "Bin", n), Path.Combine(bin, n), true);
                var (r0, i0) = (Get(rules), Get(ini));
                now = GameSettings.Now(bin, docs);
                check(GameSettings.All.All(s => now.ContainsKey(s.Id)), "game settings (real files): every option has its place");
                GameSettings.ApplyBin(g, docs, new Dictionary<string, string> { ["shadow"] = "8192", ["sims"] = "4096", ["reads"] = "40000000", ["walk.gap"] = "0.1", ["bloom"] = now["bloom"] == "1" ? "0" : "1" });
                check(Get(rules) != r0 && Get(ini) != i0, "game settings (real files): written");
                GameSettings.RestoreBin(g);
                check(Get(rules) == r0 && Get(ini) == i0, "game settings (real files): restored byte for byte");
            }
        }
        catch (Exception e) { check(false, "game settings: exception " + e); }
        finally { try { Directory.Delete(root, true); } catch (Exception) { } }
    }
}
