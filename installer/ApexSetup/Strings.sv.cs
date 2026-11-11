// Apex Radiance Setup texts in Swedish (Svenska). Fill only the entries between the braces, one per line:
//     ["<English key, exactly as in keys.json>"] = "<translation>",
// C# strings: write \" for a quote, \\ for a backslash, \n for a line break; every other character as itself (UTF-8).
// Keep each {0}, {1} of the key, product and file names as they are. A key without an entry shows in English; never
// leave a translation empty. The same key twice: the last one wins, and ApexSetup.Tests fails.
namespace ApexSetup;

public static partial class Strings
{
    static readonly Dictionary<string, string> Sv = new()
    {
        ["Welcome"] = "Välkommen",
        ["Game"] = "Spel",
        ["Components"] = "Komponenter",
        ["Install"] = "Installera",
        ["Done"] = "Klar",
        ["Repair or remove"] = "Reparera eller ta bort",
        ["About and licences"] = "Om och licenser",
        ["Back"] = "Tillbaka",
        ["Next"] = "Nästa",
        ["Get started"] = "Kom igång",
        ["Finish"] = "Slutför",
        ["Close The Sims 3 to continue."] = "Stäng The Sims 3 för att fortsätta.",
        ["Setup needs the internet to download the files."] =
            "Installationsprogrammet behöver internet för att ladda ned filerna.",
        ["Nothing changes until you click Install."] = "Inget ändras förrän du klickar på Installera.",
        ["Lighting, picture and performance for The Sims 3."] = "Ljus, bild och prestanda för The Sims 3.",
        ["What Setup does"] = "Det här gör installationsprogrammet",
        ["Setup can't reach GitHub"] = "Installationsprogrammet når inte GitHub",
        ["Check your internet connection, then try again."] = "Kontrollera internetanslutningen och försök igen.",
        ["Try again"] = "Försök igen",
        ["Choose your game"] = "Välj ditt spel",
        ["Setup found these installs. Pick the one you play."] =
            "Installationsprogrammet hittade de här installationerna. Välj den du spelar.",
        ["Setup found no install. Browse for the game folder."] =
            "Ingen installation hittades. Bläddra fram till spelmappen.",
        ["Browse for another folder"] = "Bläddra efter en annan mapp",
        ["No game in this folder"] = "Inget spel i den här mappen",
        ["Pick the The Sims 3 folder, or its Game\\Bin folder (the one with TS3W.exe or TS3.exe)."] =
            "Välj mappen The Sims 3 eller dess Game\\Bin-mapp (den med TS3W.exe eller TS3.exe).",
        ["Not installed"] = "Inte installerad",
        ["Installed"] = "Installerad",
        ["the game's own Direct3D 9"] = "spelets eget Direct3D 9",
        ["another d3d9.dll"] = "en annan d3d9.dll",
        ["Required"] = "Krävs",
        ["Installs {0} into Game\\Bin."] = "Installerar {0} i Game\\Bin.",
        ["Replaces the installed copy with {0}. Your settings stay."] =
            "Ersätter den installerade kopian med {0}. Dina inställningar finns kvar.",
        ["Updates {0} to {1}. Your settings stay."] = "Uppdaterar {0} till {1}. Dina inställningar finns kvar.",
        ["Recommended with Apex"] = "Rekommenderas med Apex",
        ["No updates available"] = "Inga uppdateringar tillgängliga",
        ["Everything in this game is on its latest version."] = "Allt i det här spelet har den senaste versionen.",
        ["Not available for this game version: it supports Steam 1.67.2 and EA app 1.69.47."] =
            "Inte tillgänglig för den här spelversionen: den stöder Steam 1.67.2 och EA app 1.69.47.",
        ["Wrong build"] = "Fel version",
        ["Update {0}"] = "Uppdatera {0}",
        ["Up to date"] = "Aktuell",
        ["Setup could not finish"] = "Installationen kunde inte slutföras",
        ["Close The Sims 3 first."] = "Stäng The Sims 3 först.",
        ["You're all set"] = "Allt är klart",
        ["Start The Sims 3, load a world and press Ctrl+Shift+F11 to open the Apex Radiance menu."] =
            "Starta The Sims 3, ladda en värld och tryck Ctrl+Shift+F11 för att öppna Apex Radiance-menyn.",
        ["Installed: {0} {1}"] = "Installerat: {0} {1}",
        ["Choose your game first."] = "Välj ditt spel först.",
        ["Removes ApexRadiance.asi. Your settings in Documents stay."] =
            "Tar bort ApexRadiance.asi. Dina inställningar i Dokument finns kvar.",
        ["Remove"] = "Ta bort",
        ["RTX Remix bridge"] = "RTX Remix bridge",
        ["Removes Sims3SettingsSetter.asi."] = "Tar bort Sims3SettingsSetter.asi.",
        ["Removes ts3native and ts3fixes.package."] = "Tar bort ts3native och ts3fixes.package.",
        ["Repair or update"] = "Reparera eller uppdatera",
        ["Downloads and copies again everything installed here. Your settings stay."] =
            "Laddar ned och kopierar om allt som är installerat här. Dina inställningar finns kvar.",
        ["Repair"] = "Reparera",
        ["Uninstall everything"] = "Avinstallera allt",
        ["Removes what Setup installed, and Apex Radiance, and puts back every file Setup replaced."] =
            "Tar bort det som installationsprogrammet installerade och Apex Radiance, och lägger tillbaka varje fil som installationsprogrammet ersatte.",
        ["Uninstall"] = "Avinstallera",
        ["Remove {0}?"] = "Ta bort {0}?",
        ["Close the game first. Setup removes the files and puts back the ones it replaced."] =
            "Stäng spelet först. Installationsprogrammet tar bort filerna och lägger tillbaka dem som ersattes.",
        ["{0} removed."] = "{0} har tagits bort.",
        ["Uninstall everything?"] = "Avinstallera allt?",
        ["Setup removes Apex Radiance and everything it installed in this game, and puts back every file it replaced. Your settings in Documents stay."] =
            "Installationsprogrammet tar bort Apex Radiance och allt det installerade i det här spelet, och lägger tillbaka varje fil det ersatte. Dina inställningar i Dokument finns kvar.",
        ["Everything Setup installed was removed."] = "Allt som installationsprogrammet installerade har tagits bort.",
        ["Setup fixes this"] = "Installationsprogrammet åtgärdar detta",
        ["Read more"] = "Läs mer",
        ["Fixed."] = "Åtgärdat.",
        ["No incompatible settings found."] = "Inga inkompatibla inställningar hittades.",
        ["{0} in Game\\Bin loads a second copy of Apex Radiance."] =
            "{0} i Game\\Bin laddar en andra kopia av Apex Radiance.",
        ["Two ASI loaders ({0}) load every mod twice. Keep one: remove all but {1}."] =
            "Två ASI-laddare ({0}) laddar varje mod två gånger. Behåll en: ta bort alla utom {1}.",
        ["Sims 3 Modernization's ts3native is the build for the other edition and does nothing. Turn it on in Components to install the right one."] =
            "Sims 3 Modernizations ts3native är versionen för den andra utgåvan och gör ingenting. Slå på den under Komponenter för att installera rätt version.",
        ["Sims 3 Modernization has no build for this game version ({0}), so its ts3native does nothing. Turn it off in Components."] =
            "Sims 3 Modernization har ingen version för den här spelversionen ({0}), så dess ts3native gör ingenting. Stäng av den under Komponenter.",
        ["{0} in Game\\Bin is a second copy of {1}; both load. Setup moves it to the backup folder."] =
            "{0} i Game\\Bin är en andra kopia av {1}; båda laddas. Installationsprogrammet flyttar den till säkerhetskopiemappen.",
        ["Mods has {0} .package files but no Resource.cfg, so the game loads none of them."] =
            "Mods har {0} .package-filer men ingen Resource.cfg, så spelet laddar ingen av dem.",
        ["{0} is the build for the other edition of the game; it makes the game show \"unofficial game modification\". Use the {1} build."] =
            "{0} är versionen för den andra utgåvan av spelet; den gör att spelet visar \"unofficial game modification\". Använd {1}-versionen.",
        ["NRaas ErrorTrap has no build known to work on EA app 1.69.47."] =
            "NRaas ErrorTrap har ingen version som veterligen fungerar med EA app 1.69.47.",
        ["{0} overrides game code Sims 3 Modernization also patches (not tested together)."] =
            "{0} ersätter spelkod som Sims 3 Modernization också ändrar (inte testade tillsammans).",
        ["{0} is an old build with Apex inside; while it is in Game\\Bin, Apex Radiance stays off. Setup moves it to the backup folder (install the official Sims3SettingsSetter in its place if you used it)."] =
            "{0} är en gammal version med Apex inbyggt; så länge den ligger i Game\\Bin är Apex Radiance avstängt. Installationsprogrammet flyttar den till säkerhetskopiemappen (installera det officiella Sims3SettingsSetter i stället om du använde det).",
        ["Sims3Performance's ShortWaitSpin and ThreadStackDiet clash with Sims 3 Modernization. Setup sets both to 0 (a copy is kept as Sims3Performance.ini.apex-backup)."] =
            "ShortWaitSpin och ThreadStackDiet i Sims3Performance krockar med Sims 3 Modernization. Installationsprogrammet sätter båda till 0 (en kopia sparas som Sims3Performance.ini.apex-backup).",
        ["Sims3SettingsSetter's uncompressed textures stutter under the RTX Remix bridge. Setup turns them off (a copy is kept as S3SS.toml.apex-backup)."] =
            "Sims3SettingsSetters okomprimerade texturer får spelet att hacka med RTX Remix bridge. Installationsprogrammet stänger av dem (en kopia sparas som S3SS.toml.apex-backup).",
        ["{0} sets client.targetProcess = {1}, but this game is {2}: the bridge does not start."] =
            "{0} anger client.targetProcess = {1}, men det här spelet är {2}: bryggan startar inte.",
        ["dxvk.conf lacks {0} or has other values: without them the game resets its graphics options or freezes on a resolution change under the bridge."] =
            "dxvk.conf saknar {0} eller har andra värden: utan dem återställer spelet sina grafikalternativ eller fryser vid byte av upplösning med bryggan.",
        ["Mods\\Resource.cfg does not read {0}, so the game does not load it. Setup adds the Packages folder lines (a copy is kept as Resource.cfg.apex-backup)."] =
            "Mods\\Resource.cfg läser inte {0}, så spelet laddar den inte. Installationsprogrammet lägger till raderna för mappen Packages (en kopia sparas som Resource.cfg.apex-backup).",
        ["Fix settings"] = "Åtgärda inställningar",
        ["Everything is installed and up to date."] = "Allt är installerat och aktuellt.",
        ["Will be removed"] = "Tas bort",
        ["Keep"] = "Behåll",
        ["Nothing to apply"] = "Inget att verkställa",
        ["Preview"] = "Förhandsvisning",
        ["This is a preview: nothing was installed."] = "Det här är en förhandsvisning: inget installerades.",
        ["Setup is applying changes"] = "Installationsprogrammet verkställer ändringar",
        ["Wait until it finishes before closing."] = "Vänta tills det är klart innan du stänger.",
        ["Checking the latest versions…"] = "Söker efter de senaste versionerna…",
        ["Checking for updates…"] = "Söker efter uppdateringar…",
        ["Close The Sims 3 to update."] = "Stäng The Sims 3 för att uppdatera.",
        ["Update now installs them."] = "”Uppdatera nu” installerar dem.",
        ["Graphics"] = "Grafik",
        ["Checked at {0}."] = "Kontrollerat {0}.",
        ["Test build {0}: kept as it is."] = "Testversion {0}: behålls som den är.",
        ["Repair or remove sets the game right."] = "Under ”Reparera eller ta bort” kan du få ordning på spelet igen.",
        ["Changes applied"] = "Ändringarna har verkställts",
        ["Removed: {0}"] = "Borttaget: {0}",
        ["The Sims 3 · {0}"] = "The Sims 3 · {0}",
        ["{0} → {1}"] = "{0} → {1}",
        ["{0} · up to date"] = "{0} · aktuell",
        ["Everything is in place."] = "Allt är på plats.",
        ["Everything is in place (checked at {0})."] = "Allt är på plats (kontrollerat {0}).",
        ["Check installation"] = "Kontrollera installationen",
        ["Change components"] = "Ändra komponenter",
        ["Apex Radiance is not installed in this game."] = "Apex Radiance är inte installerat i det här spelet.",
        ["{0} · test build"] = "{0} · testversion",
        ["Install the official {0}"] = "Installera officiella {0}",
        ["No ASI loader in Game\\Bin: Apex Radiance and the other .asi mods do not load."] =
            "Ingen ASI-laddare i Game\\Bin: Apex Radiance och de andra .asi-moddarna laddas inte.",
        ["{0} is missing: the RTX Remix bridge does not start."] = "{0} saknas: RTX Remix bridge startar inte.",
        ["Apply 1 change"] = "Verkställ 1 ändring",
        ["Apply {0} changes"] = "Verkställ {0} ändringar",
        ["{0} is installed and up to date: nothing to change."] = "{0} är installerad och aktuell: inget att ändra.",
        ["Keep my d3d9.dll"] = "Behåll min d3d9.dll",
        ["Your d3d9.dll stays as it is."] = "Din d3d9.dll lämnas som den är.",
        ["Applying changes"] = "Verkställer ändringar",
        ["Remove {0}"] = "Ta bort {0}",
        ["Fix incompatible settings"] = "Åtgärda inkompatibla inställningar",
        // restore points (Repair or remove)
        ["Install {0}"] = "Installera {0}",
        ["Restore points"] = "Återställningspunkter",
        ["Setup keeps a copy of every file it changes. Restoring a point undoes that change and every later one."] = "Installationsprogrammet sparar en kopia av varje fil det ändrar. Att återställa en punkt ångrar den ändringen och alla senare.",
        ["Before: {0}"] = "Före: {0}",
        ["{0} · 1 file"] = "{0} · 1 fil",
        ["{0} · {1} files"] = "{0} · {1} filer",
        ["Restore"] = "Återställ",
        ["No restore points yet. Setup makes one before every change."] = "Inga återställningspunkter än. Installationsprogrammet skapar en före varje ändring.",
        ["Restore this point?"] = "Återställa den här punkten?",
        ["Setup puts back the files as they were before this change, and undoes every change made after it. Close the game first."] = "Installationsprogrammet lägger tillbaka filerna som de var före den här ändringen och ångrar alla ändringar som gjorts efter den. Stäng spelet först.",
        ["Setup could not put back {0}."] = "Installationsprogrammet kunde inte lägga tillbaka {0}.",
        ["Changes undone."] = "Ändringarna ångrades.",
        ["Undo these changes"] = "Ångra de här ändringarna",
        ["This restore point is gone."] = "Den här återställningspunkten finns inte längre.",
        ["Updates available"] = "Uppdateringar tillgängliga",
        ["Update now"] = "Uppdatera nu",
        ["{0} {1} → {2}"] = "{0} {1} → {2}",
        ["Removes MonoPatcher.asi and its package."] = "Tar bort MonoPatcher.asi och dess paket.",
        ["Smooth Patch's TS3Patch.asi patches the same game code as Sims3SettingsSetter's Smooth Patch. Setup moves it to the backup folder (S3SS does the same job)."] =
            "Smooth Patchs TS3Patch.asi ändrar samma spelkod som Smooth Patch i Sims3SettingsSetter. Installationsprogrammet flyttar den till säkerhetskopiemappen (S3SS gör samma jobb).",
        ["simler90's Gameplay Core Mod and NRaas StoryProgression replace the same game code. Keep one of them."] =
            "simler90s Gameplay Core Mod och NRaas StoryProgression ersätter samma spelkod. Behåll en av dem.",
        ["MonoPatcher needs the Microsoft Visual C++ Redistributable (x86); it is not installed."] =
            "MonoPatcher behöver Microsoft Visual C++ Redistributable (x86); det är inte installerat.",
        ["Download on Nexus Mods"] = "Ladda ned på Nexus Mods",
        ["Choose the downloaded file"] = "Välj den nedladdade filen",
        ["Ready to install"] = "Redo att installera",
        ["Waiting for your download in {0}…"] = "Väntar på din nedladdning i {0}…",
        ["Not the Performance Patch"] = "Inte Performance Patch",
        ["{0} has no Sims3Performance.asi. If the download is a .7z or .rar, extract it and choose Sims3Performance.asi."] =
            "{0} saknar Sims3Performance.asi. Om nedladdningen är en .7z eller .rar, packa upp den och välj Sims3Performance.asi.",
        ["Removes Sims3Performance.asi and its .ini."] = "Tar bort Sims3Performance.asi och dess .ini.",
        ["Download Sims 3 Performance Patch from Nexus Mods first."] =
            "Ladda först ned Sims 3 Performance Patch från Nexus Mods.",
        ["Finds your The Sims 3 (Steam or EA app)."] = "Hittar ditt The Sims 3 (Steam eller EA app).",
        ["Installs Apex Radiance and the fixes you choose."] =
            "Installerar Apex Radiance och de korrigeringar du väljer.",
        ["Keeps a copy of every file it changes, so you can undo everything."] =
            "Sparar en kopia av varje fil det ändrar, så att du kan ångra allt.",
        ["Your Apex Radiance settings are always kept."] = "Dina Apex Radiance-inställningar behålls alltid.",
        ["No internet connection"] = "Ingen internetanslutning",
        ["Vulkan, DXVK (recommended)"] = "Vulkan, DXVK (rekommenderas)",
        ["Vulkan with more memory (RTX Remix bridge)"] = "Vulkan med mer minne (RTX Remix bridge)",
        ["Windows default (Direct3D 9)"] = "Windows-standard (Direct3D 9)",
        ["Fewer stutters; Apex Radiance is made and tested with it."] =
            "Färre hack; Apex Radiance är gjort och testat med det.",
        ["Keeps about 600 MB more memory free for big worlds. Needs a Vulkan graphics card. If the game misbehaves with it, choose DXVK."] =
            "Håller ungefär 600 MB mer minne ledigt för stora världar. Kräver ett grafikkort med Vulkan. Om spelet krånglar med det, välj DXVK.",
        ["Works, but Apex Radiance is tested with DXVK."] = "Fungerar, men Apex Radiance testas med DXVK.",
        ["Your in-game graphics settings may reset once. Setup keeps a copy and puts it back if you switch back."] =
            "Dina grafikinställningar i spelet kan återställas en gång. Installationsprogrammet sparar en kopia och lägger tillbaka den om du byter tillbaka.",
        ["Fixes for game and graphics settings."] = "Korrigeringar för spel- och grafikinställningar.",
        ["Fixes stutters, Sims getting stuck and crashes."] = "Åtgärdar hack, simmar som fastnar och krascher.",
        ["Needed only by some script mods; turn it on if a mod asks for it."] =
            "Behövs bara av vissa skriptmoddar; slå på den om en mod ber om det.",
        ["Makes the game run faster. Needs a free Nexus Mods account; Setup installs it when your download finishes."] =
            "Får spelet att gå snabbare. Kräver ett gratis Nexus Mods-konto; installationsprogrammet installerar det när nedladdningen är klar.",
        ["Puts back the graphics files you had before."] = "Lägger tillbaka grafikfilerna du hade innan.",
        ["Goes back to the game's default graphics. Your in-game graphics settings may reset once."] =
            "Går tillbaka till spelets standardgrafik. Dina grafikinställningar i spelet kan återställas en gång.",
        ["Language"] = "Språk",
        ["Automatic"] = "Automatiskt",
        ["This file has no Sims3Performance.asi."] = "Den här filen saknar Sims3Performance.asi.",
        ["Dry run: Setup changes nothing."] = "Testkörning: installationsprogrammet ändrar ingenting.",
        ["Apex Radiance by loinyx. The RTX Remix bridge is NVIDIA's, ported to The Sims 3 by dalexlol and alexvixell35-stack. Sims3SettingsSetter is by sims3fiend, Sims 3 Modernization by rodneypantonial and Ultimate ASI Loader by ThirteenAG: Setup downloads them from their own GitHub releases."] =
            "Apex Radiance av loinyx. RTX Remix bridge kommer från NVIDIA och har porterats till The Sims 3 av dalexlol och alexvixell35-stack. Sims3SettingsSetter är gjort av sims3fiend, Sims 3 Modernization av rodneypantonial och Ultimate ASI Loader av ThirteenAG: installationsprogrammet laddar ned dem från deras egna releaser på GitHub.",
        ["RTX Remix bridge: third-party notices"] = "RTX Remix bridge: meddelanden från tredje part",
        ["Read"] = "Läs",
        ["Licence text"] = "Licenstext",
        ["OK"] = "OK",
        ["Cancel"] = "Avbryt",
    };
}
