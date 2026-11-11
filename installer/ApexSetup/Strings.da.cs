// Apex Radiance Setup texts in Danish (Dansk). Fill only the entries between the braces, one per line:
//     ["<English key, exactly as in keys.json>"] = "<translation>",
// C# strings: write \" for a quote, \\ for a backslash, \n for a line break; every other character as itself (UTF-8).
// Keep each {0}, {1} of the key, product and file names as they are. A key without an entry shows in English; never
// leave a translation empty. The same key twice: the last one wins, and ApexSetup.Tests fails.
namespace ApexSetup;

public static partial class Strings
{
    static readonly Dictionary<string, string> Da = new()
    {
        ["Welcome"] = "Velkommen",
        ["Game"] = "Spil",
        ["Components"] = "Komponenter",
        ["Install"] = "Installer",
        ["Done"] = "Færdig",
        ["Repair or remove"] = "Reparer eller fjern",
        ["About and licences"] = "Om og licenser",
        ["Back"] = "Tilbage",
        ["Next"] = "Næste",
        ["Get started"] = "Kom i gang",
        ["Finish"] = "Afslut",
        ["Close The Sims 3 to continue."] = "Luk The Sims 3 for at fortsætte.",
        ["Setup needs the internet to download the files."] = "Installationsprogrammet skal bruge internettet for at hente filerne.",
        ["Nothing changes until you click Install."] = "Intet ændres, før du klikker på Installer.",
        ["Lighting, picture and performance for The Sims 3."] = "Belysning, billede og ydeevne til The Sims 3.",
        ["What Setup does"] = "Det gør installationsprogrammet",
        ["Setup can't reach GitHub"] = "Installationsprogrammet kan ikke nå GitHub",
        ["Check your internet connection, then try again."] = "Tjek din internetforbindelse, og prøv igen.",
        ["Try again"] = "Prøv igen",
        ["Choose your game"] = "Vælg dit spil",
        ["Setup found these installs. Pick the one you play."] = "Installationsprogrammet fandt disse installationer. Vælg den, du spiller.",
        ["Setup found no install. Browse for the game folder."] = "Ingen installation fundet. Find mappen med spillet.",
        ["Browse for another folder"] = "Vælg en anden mappe",
        ["No game in this folder"] = "Intet spil i denne mappe",
        ["Pick the The Sims 3 folder, or its Game\\Bin folder (the one with TS3W.exe or TS3.exe)."] =
            "Vælg mappen The Sims 3 eller dens Game\\Bin-mappe (den med TS3W.exe eller TS3.exe).",
        ["Not installed"] = "Ikke installeret",
        ["Installed"] = "Installeret",
        ["the game's own Direct3D 9"] = "spillets egen Direct3D 9",
        ["another d3d9.dll"] = "en anden d3d9.dll",
        ["Required"] = "Påkrævet",
        ["Installs {0} into Game\\Bin."] = "Installerer {0} i Game\\Bin.",
        ["Replaces the installed copy with {0}. Your settings stay."] = "Erstatter den installerede kopi med {0}. Dine indstillinger bevares.",
        ["Updates {0} to {1}. Your settings stay."] = "Opdaterer {0} til {1}. Dine indstillinger bevares.",
        ["Recommended with Apex"] = "Anbefales med Apex",
        ["No updates available"] = "Ingen tilgængelige opdateringer",
        ["Everything in this game is on its latest version."] = "Alt i dette spil er i den nyeste version.",
        ["Not available for this game version: it supports Steam 1.67.2 and EA app 1.69.47."] =
            "Ikke tilgængelig for denne spilversion: den understøtter Steam 1.67.2 og EA app 1.69.47.",
        ["Wrong build"] = "Forkert version",
        ["Update {0}"] = "Opdater {0}",
        ["Up to date"] = "Opdateret",
        ["Setup could not finish"] = "Installationen kunne ikke fuldføres",
        ["Close The Sims 3 first."] = "Luk The Sims 3 først.",
        ["You're all set"] = "Du er klar",
        ["Start The Sims 3, load a world and press Ctrl+Shift+F11 to open the Apex Radiance menu."] =
            "Start The Sims 3, indlæs en verden, og tryk på Ctrl+Shift+F11 for at åbne menuen i Apex Radiance.",
        ["Installed: {0} {1}"] = "Installeret: {0} {1}",
        ["Choose your game first."] = "Vælg dit spil først.",
        ["Removes ApexRadiance.asi. Your settings in Documents stay."] = "Fjerner ApexRadiance.asi. Dine indstillinger i Dokumenter bevares.",
        ["Remove"] = "Fjern",
        ["RTX Remix bridge"] = "RTX Remix bridge",
        ["Removes Sims3SettingsSetter.asi."] = "Fjerner Sims3SettingsSetter.asi.",
        ["Removes ts3native and ts3fixes.package."] = "Fjerner ts3native og ts3fixes.package.",
        ["Repair or update"] = "Reparer eller opdater",
        ["Downloads and copies again everything installed here. Your settings stay."] =
            "Henter og kopierer alt, der er installeret her, igen. Dine indstillinger bevares.",
        ["Repair"] = "Reparer",
        ["Uninstall everything"] = "Afinstaller alt",
        ["Removes what Setup installed, and Apex Radiance, and puts back every file Setup replaced."] =
            "Fjerner det, installationsprogrammet installerede, og Apex Radiance, og lægger alle filer, det erstattede, tilbage.",
        ["Uninstall"] = "Afinstaller",
        ["Remove {0}?"] = "Fjern {0}?",
        ["Close the game first. Setup removes the files and puts back the ones it replaced."] =
            "Luk spillet først. Installationsprogrammet fjerner filerne og lægger dem, det erstattede, tilbage.",
        ["{0} removed."] = "{0} er fjernet.",
        ["Uninstall everything?"] = "Afinstaller alt?",
        ["Setup removes Apex Radiance and everything it installed in this game, and puts back every file it replaced. Your settings in Documents stay."] =
            "Installationsprogrammet fjerner Apex Radiance og alt, det installerede i dette spil, og lægger alle filer, det erstattede, tilbage. Dine indstillinger i Dokumenter bevares.",
        ["Everything Setup installed was removed."] = "Alt, hvad installationsprogrammet installerede, er fjernet.",
        ["Setup fixes this"] = "Installationsprogrammet retter dette",
        ["Read more"] = "Læs mere",
        ["Fixed."] = "Rettet.",
        ["No incompatible settings found."] = "Ingen inkompatible indstillinger fundet.",
        ["{0} in Game\\Bin loads a second copy of Apex Radiance."] = "{0} i Game\\Bin indlæser en ekstra kopi af Apex Radiance.",
        ["Two ASI loaders ({0}) load every mod twice. Keep one: remove all but {1}."] =
            "To ASI-loadere ({0}) indlæser alle mods to gange. Behold én: fjern alle undtagen {1}.",
        ["Sims 3 Modernization's ts3native is the build for the other edition and does nothing. Turn it on in Components to install the right one."] =
            "ts3native fra Sims 3 Modernization er versionen til den anden udgave og gør ingenting. Slå den til under Komponenter for at installere den rigtige.",
        ["Sims 3 Modernization has no build for this game version ({0}), so its ts3native does nothing. Turn it off in Components."] =
            "Sims 3 Modernization har ingen version til denne spilversion ({0}), så dens ts3native gør ingenting. Slå den fra under Komponenter.",
        ["{0} in Game\\Bin is a second copy of {1}; both load. Setup moves it to the backup folder."] =
            "{0} i Game\\Bin er en ekstra kopi af {1}; begge indlæses. Installationsprogrammet flytter den til backupmappen.",
        ["Mods has {0} .package files but no Resource.cfg, so the game loads none of them."] =
            "Mods har {0} .package-filer, men ingen Resource.cfg, så spillet indlæser ingen af dem.",
        ["{0} is the build for the other edition of the game; it makes the game show \"unofficial game modification\". Use the {1} build."] =
            "{0} er versionen til den anden udgave af spillet; den får spillet til at vise \"unofficial game modification\". Brug {1}-versionen.",
        ["NRaas ErrorTrap has no build known to work on EA app 1.69.47."] =
            "NRaas ErrorTrap har ingen version, der vides at virke med EA app 1.69.47.",
        ["{0} overrides game code Sims 3 Modernization also patches (not tested together)."] =
            "{0} erstatter spilkode, som Sims 3 Modernization også ændrer (ikke testet sammen).",
        ["{0} is an old build with Apex inside; while it is in Game\\Bin, Apex Radiance stays off. Setup moves it to the backup folder (install the official Sims3SettingsSetter in its place if you used it)."] =
            "{0} er en gammel version med Apex indbygget; mens den ligger i Game\\Bin, er Apex Radiance slået fra. Installationsprogrammet flytter den til backupmappen (installer den officielle Sims3SettingsSetter i stedet, hvis du brugte den).",
        ["Sims3Performance's ShortWaitSpin and ThreadStackDiet clash with Sims 3 Modernization. Setup sets both to 0 (a copy is kept as Sims3Performance.ini.apex-backup)."] =
            "ShortWaitSpin og ThreadStackDiet fra Sims3Performance er i konflikt med Sims 3 Modernization. Installationsprogrammet sætter begge til 0 (en kopi gemmes som Sims3Performance.ini.apex-backup).",
        ["Sims3SettingsSetter's uncompressed textures stutter under the RTX Remix bridge. Setup turns them off (a copy is kept as S3SS.toml.apex-backup)."] =
            "Ukomprimerede teksturer fra Sims3SettingsSetter får spillet til at hakke under RTX Remix bridge. Installationsprogrammet slår dem fra (en kopi gemmes som S3SS.toml.apex-backup).",
        ["{0} sets client.targetProcess = {1}, but this game is {2}: the bridge does not start."] =
            "{0} sætter client.targetProcess = {1}, men dette spil er {2}: broen starter ikke.",
        ["dxvk.conf lacks {0} or has other values: without them the game resets its graphics options or freezes on a resolution change under the bridge."] =
            "dxvk.conf mangler {0} eller har andre værdier: uden dem nulstiller spillet sine grafikindstillinger eller fryser ved skift af opløsning under broen.",
        ["Mods\\Resource.cfg does not read {0}, so the game does not load it. Setup adds the Packages folder lines (a copy is kept as Resource.cfg.apex-backup)."] =
            "Mods\\Resource.cfg læser ikke {0}, så spillet indlæser den ikke. Installationsprogrammet tilføjer linjerne for mappen Packages (en kopi gemmes som Resource.cfg.apex-backup).",
        ["Fix settings"] = "Ret indstillinger",
        ["Everything is installed and up to date."] = "Alt er installeret og opdateret.",
        ["Will be removed"] = "Fjernes",
        ["Keep"] = "Behold",
        ["Nothing to apply"] = "Intet at anvende",
        ["Preview"] = "Forhåndsvisning",
        ["This is a preview: nothing was installed."] = "Dette er en forhåndsvisning: intet blev installeret.",
        ["Setup is applying changes"] = "Installationsprogrammet anvender ændringer",
        ["Wait until it finishes before closing."] = "Vent, til det er færdigt, før du lukker.",
        ["Checking the latest versions…"] = "Tjekker de nyeste versioner…",
        ["Checking for updates…"] = "Søger efter opdateringer…",
        ["Close The Sims 3 to update."] = "Luk The Sims 3 for at opdatere.",
        ["Update now installs them."] = "»Opdater nu« installerer dem.",
        ["Graphics"] = "Grafik",
        ["Checked at {0}."] = "Tjekket kl. {0}.",
        ["Test build {0}: kept as it is."] = "Testversion {0}: beholdes, som den er.",
        ["Repair or remove sets the game right."] = "Under »Reparer eller fjern« kan du bringe spillet i orden igen.",
        ["Changes applied"] = "Ændringer anvendt",
        ["Removed: {0}"] = "Fjernet: {0}",
        ["The Sims 3 · {0}"] = "The Sims 3 · {0}",
        ["{0} → {1}"] = "{0} → {1}",
        ["{0} · up to date"] = "{0} · opdateret",
        ["Everything is in place."] = "Alt er på plads.",
        ["Everything is in place (checked at {0})."] = "Alt er på plads (tjekket kl. {0}).",
        ["Check installation"] = "Tjek installationen",
        ["Change components"] = "Rediger komponenter",
        ["Apex Radiance is not installed in this game."] = "Apex Radiance er ikke installeret i dette spil.",
        ["{0} · test build"] = "{0} · testversion",
        ["Install the official {0}"] = "Installer den officielle {0}",
        ["No ASI loader in Game\\Bin: Apex Radiance and the other .asi mods do not load."] =
            "Ingen ASI-loader i Game\\Bin: Apex Radiance og de andre .asi-mods indlæses ikke.",
        ["{0} is missing: the RTX Remix bridge does not start."] = "{0} mangler: RTX Remix bridge starter ikke.",
        ["Apply 1 change"] = "Anvend 1 ændring",
        ["Apply {0} changes"] = "Anvend {0} ændringer",
        ["{0} is installed and up to date: nothing to change."] = "{0} er installeret og opdateret: intet at ændre.",
        ["Keep my d3d9.dll"] = "Behold min d3d9.dll",
        ["Your d3d9.dll stays as it is."] = "Din d3d9.dll forbliver, som den er.",
        ["Applying changes"] = "Anvender ændringer",
        ["Remove {0}"] = "Fjern {0}",
        ["Fix incompatible settings"] = "Ret inkompatible indstillinger",
        // restore points (Repair or remove)
        ["Install {0}"] = "Installer {0}",
        ["Restore points"] = "Gendannelsespunkter",
        ["Setup keeps a copy of every file it changes. Restoring a point undoes that change and every later one."] = "Installationsprogrammet gemmer en kopi af hver fil, det ændrer. Gendannelse af et punkt fortryder den ændring og alle senere.",
        ["Before: {0}"] = "Før: {0}",
        ["{0} · 1 file"] = "{0} · 1 fil",
        ["{0} · {1} files"] = "{0} · {1} filer",
        ["Restore"] = "Gendan",
        ["No restore points yet. Setup makes one before every change."] = "Ingen gendannelsespunkter endnu. Installationsprogrammet laver et før hver ændring.",
        ["Restore this point?"] = "Gendan dette punkt?",
        ["Setup puts back the files as they were before this change, and undoes every change made after it. Close the game first."] = "Installationsprogrammet lægger filerne tilbage, som de var før denne ændring, og fortryder alle ændringer foretaget efter den. Luk spillet først.",
        ["Setup could not put back {0}."] = "Installationsprogrammet kunne ikke lægge {0} tilbage.",
        ["Changes undone."] = "Ændringer fortrudt.",
        ["Undo these changes"] = "Fortryd disse ændringer",
        ["This restore point is gone."] = "Dette gendannelsespunkt findes ikke længere.",
        ["Updates available"] = "Opdateringer tilgængelige",
        ["Update now"] = "Opdater nu",
        ["{0} {1} → {2}"] = "{0} {1} → {2}",
        ["Removes MonoPatcher.asi and its package."] = "Fjerner MonoPatcher.asi og dens pakke.",
        ["Smooth Patch's TS3Patch.asi patches the same game code as Sims3SettingsSetter's Smooth Patch. Setup moves it to the backup folder (S3SS does the same job)."] =
            "TS3Patch.asi fra Smooth Patch ændrer den samme spilkode som Smooth Patch i Sims3SettingsSetter. Installationsprogrammet flytter den til backupmappen (S3SS gør det samme).",
        ["simler90's Gameplay Core Mod and NRaas StoryProgression replace the same game code. Keep one of them."] =
            "Gameplay Core Mod fra simler90 og NRaas StoryProgression erstatter den samme spilkode. Behold kun den ene.",
        ["MonoPatcher needs the Microsoft Visual C++ Redistributable (x86); it is not installed."] =
            "MonoPatcher kræver Microsoft Visual C++ Redistributable (x86); den er ikke installeret.",
        ["Download on Nexus Mods"] = "Hent på Nexus Mods",
        ["Choose the downloaded file"] = "Vælg den hentede fil",
        ["Ready to install"] = "Klar til installation",
        ["Waiting for your download in {0}…"] = "Venter på din download i {0}…",
        ["Not the Performance Patch"] = "Ikke Performance Patch",
        ["{0} has no Sims3Performance.asi. If the download is a .7z or .rar, extract it and choose Sims3Performance.asi."] =
            "{0} indeholder ikke Sims3Performance.asi. Hvis downloaden er en .7z eller .rar, så udpak den og vælg Sims3Performance.asi.",
        ["Removes Sims3Performance.asi and its .ini."] = "Fjerner Sims3Performance.asi og dens .ini.",
        ["Download Sims 3 Performance Patch from Nexus Mods first."] = "Hent Sims 3 Performance Patch fra Nexus Mods først.",
        ["Finds your The Sims 3 (Steam or EA app)."] = "Finder dit The Sims 3 (Steam eller EA app).",
        ["Installs Apex Radiance and the fixes you choose."] = "Installerer Apex Radiance og de rettelser, du vælger.",
        ["Keeps a copy of every file it changes, so you can undo everything."] = "Gemmer en kopi af hver fil, det ændrer, så du kan fortryde alt.",
        ["Your Apex Radiance settings are always kept."] = "Dine Apex Radiance-indstillinger bevares altid.",
        ["No internet connection"] = "Ingen internetforbindelse",
        ["Vulkan, DXVK (recommended)"] = "Vulkan, DXVK (anbefalet)",
        ["Vulkan with more memory (RTX Remix bridge)"] = "Vulkan med mere hukommelse (RTX Remix bridge)",
        ["Windows default (Direct3D 9)"] = "Windows-standard (Direct3D 9)",
        ["Fewer stutters; Apex Radiance is made and tested with it."] = "Færre hak; Apex Radiance er lavet og testet med det.",
        ["Keeps about 600 MB more memory free for big worlds. Needs a Vulkan graphics card. If the game misbehaves with it, choose DXVK."] =
            "Holder ca. 600 MB mere hukommelse fri til store verdener. Kræver et grafikkort med Vulkan. Hvis spillet opfører sig mærkeligt med det, så vælg DXVK.",
        ["Works, but Apex Radiance is tested with DXVK."] = "Virker, men Apex Radiance er testet med DXVK.",
        ["Your in-game graphics settings may reset once. Setup keeps a copy and puts it back if you switch back."] =
            "Dine grafikindstillinger i spillet kan blive nulstillet én gang. Installationsprogrammet gemmer en kopi og lægger den tilbage, hvis du skifter tilbage.",
        ["Fixes for game and graphics settings."] = "Rettelser til spil- og grafikindstillinger.",
        ["Fixes stutters, Sims getting stuck and crashes."] = "Retter hak, Sims der sidder fast, og nedbrud.",
        ["Needed only by some script mods; turn it on if a mod asks for it."] =
            "Kræves kun af nogle script-mods; slå det til, hvis et mod beder om det.",
        ["Makes the game run faster. Needs a free Nexus Mods account; Setup installs it when your download finishes."] =
            "Får spillet til at køre hurtigere. Kræver en gratis Nexus Mods-konto; installationsprogrammet installerer det, når din download er færdig.",
        ["Puts back the graphics files you had before."] = "Lægger de grafikfiler tilbage, du havde før.",
        ["Goes back to the game's default graphics. Your in-game graphics settings may reset once."] =
            "Går tilbage til spillets standardgrafik. Dine grafikindstillinger i spillet kan blive nulstillet én gang.",
        ["Language"] = "Sprog",
        ["Automatic"] = "Automatisk",
        ["This file has no Sims3Performance.asi."] = "Denne fil indeholder ikke Sims3Performance.asi.",
        ["Dry run: Setup changes nothing."] = "Prøvekørsel: installationsprogrammet ændrer intet.",
        ["Apex Radiance by loinyx. The RTX Remix bridge is NVIDIA's, ported to The Sims 3 by dalexlol and alexvixell35-stack. Sims3SettingsSetter is by sims3fiend, Sims 3 Modernization by rodneypantonial and Ultimate ASI Loader by ThirteenAG: Setup downloads them from their own GitHub releases."] =
            "Apex Radiance af loinyx. RTX Remix bridge er fra NVIDIA, tilpasset The Sims 3 af dalexlol og alexvixell35-stack. Sims3SettingsSetter er lavet af sims3fiend, Sims 3 Modernization af rodneypantonial og Ultimate ASI Loader af ThirteenAG: installationsprogrammet henter dem fra deres egne udgivelser på GitHub.",
        ["RTX Remix bridge: third-party notices"] = "RTX Remix bridge: meddelelser fra tredjeparter",
        ["Read"] = "Læs",
        ["Licence text"] = "Licenstekst",
        ["OK"] = "OK",
        ["Cancel"] = "Annuller",
    };
}
