// Apex Radiance Setup texts in Norwegian (Norsk). Fill only the entries between the braces, one per line:
//     ["<English key, exactly as in keys.json>"] = "<translation>",
// C# strings: write \" for a quote, \\ for a backslash, \n for a line break; every other character as itself (UTF-8).
// Keep each {0}, {1} of the key, product and file names as they are. A key without an entry shows in English; never
// leave a translation empty. The same key twice: the last one wins, and ApexSetup.Tests fails.
namespace ApexSetup;

public static partial class Strings
{
    static readonly Dictionary<string, string> No = new()
    {
        ["Welcome"] = "Velkommen",
        ["Game"] = "Spill",
        ["Components"] = "Komponenter",
        ["Install"] = "Installer",
        ["Done"] = "Ferdig",
        ["Repair or remove"] = "Reparer eller fjern",
        ["About and licences"] = "Om og lisenser",
        ["Back"] = "Tilbake",
        ["Next"] = "Neste",
        ["Get started"] = "Kom i gang",
        ["Finish"] = "Fullfør",
        ["Close The Sims 3 to continue."] = "Lukk The Sims 3 for å fortsette.",
        ["Setup needs the internet to download the files."] =
            "Installasjonsprogrammet trenger internett for å laste ned filene.",
        ["Nothing changes until you click Install."] = "Ingenting endres før du klikker Installer.",
        ["Lighting, picture and performance for The Sims 3."] = "Belysning, bilde og ytelse for The Sims 3.",
        ["What Setup does"] = "Hva installasjonsprogrammet gjør",
        ["Setup can't reach GitHub"] = "Installasjonsprogrammet når ikke GitHub",
        ["Check your internet connection, then try again."] = "Sjekk internettforbindelsen, og prøv igjen.",
        ["Try again"] = "Prøv igjen",
        ["Choose your game"] = "Velg spillet ditt",
        ["Setup found these installs. Pick the one you play."] =
            "Installasjonsprogrammet fant disse installasjonene. Velg den du spiller.",
        ["Setup found no install. Browse for the game folder."] = "Fant ingen installasjon. Bla etter spillmappen.",
        ["Browse for another folder"] = "Bla etter en annen mappe",
        ["No game in this folder"] = "Ikke noe spill i denne mappen",
        ["Pick the The Sims 3 folder, or its Game\\Bin folder (the one with TS3W.exe or TS3.exe)."] =
            "Velg The Sims 3-mappen, eller Game\\Bin-mappen i den (den med TS3W.exe eller TS3.exe).",
        ["Not installed"] = "Ikke installert",
        ["Installed"] = "Installert",
        ["the game's own Direct3D 9"] = "spillets egen Direct3D 9",
        ["another d3d9.dll"] = "en annen d3d9.dll",
        ["Required"] = "Påkrevd",
        ["Installs {0} into Game\\Bin."] = "Installerer {0} i Game\\Bin.",
        ["Replaces the installed copy with {0}. Your settings stay."] =
            "Erstatter den installerte kopien med {0}. Innstillingene dine blir.",
        ["Updates {0} to {1}. Your settings stay."] = "Oppdaterer {0} til {1}. Innstillingene dine blir.",
        ["Recommended with Apex"] = "Anbefalt med Apex",
        ["No updates available"] = "Ingen oppdateringer tilgjengelig",
        ["Everything in this game is on its latest version."] = "Alt i dette spillet har nyeste versjon.",
        ["Not available for this game version: it supports Steam 1.67.2 and EA app 1.69.47."] =
            "Ikke tilgjengelig for denne spillversjonen: den støtter Steam 1.67.2 og EA app 1.69.47.",
        ["Wrong build"] = "Feil versjon",
        ["Update {0}"] = "Oppdater {0}",
        ["Up to date"] = "Oppdatert",
        ["Setup could not finish"] = "Installasjonsprogrammet ble ikke ferdig",
        ["Close The Sims 3 first."] = "Lukk The Sims 3 først.",
        ["You're all set"] = "Alt er klart",
        ["Start The Sims 3, load a world and press Ctrl+Shift+F11 to open the Apex Radiance menu."] =
            "Start The Sims 3, last inn en verden og trykk Ctrl+Shift+F11 for å åpne Apex Radiance-menyen.",
        ["Installed: {0} {1}"] = "Installert: {0} {1}",
        ["Choose your game first."] = "Velg spillet ditt først.",
        ["Removes ApexRadiance.asi. Your settings in Documents stay."] =
            "Fjerner ApexRadiance.asi. Innstillingene dine i Dokumenter blir.",
        ["Remove"] = "Fjern",
        ["RTX Remix bridge"] = "RTX Remix bridge",
        ["Removes Sims3SettingsSetter.asi."] = "Fjerner Sims3SettingsSetter.asi.",
        ["Removes ts3native and ts3fixes.package."] = "Fjerner ts3native og ts3fixes.package.",
        ["Repair or update"] = "Reparer eller oppdater",
        ["Downloads and copies again everything installed here. Your settings stay."] =
            "Laster ned og kopierer alt som er installert her, på nytt. Innstillingene dine blir.",
        ["Repair"] = "Reparer",
        ["Uninstall everything"] = "Avinstaller alt",
        ["Removes what Setup installed, and Apex Radiance, and puts back every file Setup replaced."] =
            "Fjerner det installasjonsprogrammet installerte, og Apex Radiance, og legger tilbake hver fil det erstattet.",
        ["Uninstall"] = "Avinstaller",
        ["Remove {0}?"] = "Fjerne {0}?",
        ["Close the game first. Setup removes the files and puts back the ones it replaced."] =
            "Lukk spillet først. Installasjonsprogrammet fjerner filene og legger tilbake dem det erstattet.",
        ["{0} removed."] = "{0} er fjernet.",
        ["Uninstall everything?"] = "Avinstallere alt?",
        ["Setup removes Apex Radiance and everything it installed in this game, and puts back every file it replaced. Your settings in Documents stay."] =
            "Installasjonsprogrammet fjerner Apex Radiance og alt det installerte i dette spillet, og legger tilbake hver fil det erstattet. Innstillingene dine i Dokumenter blir.",
        ["Everything Setup installed was removed."] = "Alt installasjonsprogrammet installerte, er fjernet.",
        ["Setup fixes this"] = "Installasjonsprogrammet retter dette",
        ["Read more"] = "Les mer",
        ["Fixed."] = "Rettet.",
        ["No incompatible settings found."] = "Fant ingen inkompatible innstillinger.",
        ["{0} in Game\\Bin loads a second copy of Apex Radiance."] =
            "{0} i Game\\Bin laster en ekstra kopi av Apex Radiance.",
        ["Two ASI loaders ({0}) load every mod twice. Keep one: remove all but {1}."] =
            "To ASI-lastere ({0}) laster hver mod to ganger. Behold én: fjern alle unntatt {1}.",
        ["Sims 3 Modernization's ts3native is the build for the other edition and does nothing. Turn it on in Components to install the right one."] =
            "ts3native fra Sims 3 Modernization er versjonen for den andre utgaven og gjør ingenting. Slå den på under Komponenter for å installere den riktige.",
        ["Sims 3 Modernization has no build for this game version ({0}), so its ts3native does nothing. Turn it off in Components."] =
            "Sims 3 Modernization har ingen versjon for denne spillversjonen ({0}), så ts3native gjør ingenting. Slå den av under Komponenter.",
        ["{0} in Game\\Bin is a second copy of {1}; both load. Setup moves it to the backup folder."] =
            "{0} i Game\\Bin er en ekstra kopi av {1}; begge lastes. Installasjonsprogrammet flytter den til sikkerhetskopimappen.",
        ["Mods has {0} .package files but no Resource.cfg, so the game loads none of them."] =
            "Mods har {0} .package-filer, men ingen Resource.cfg, så spillet laster ingen av dem.",
        ["{0} is the build for the other edition of the game; it makes the game show \"unofficial game modification\". Use the {1} build."] =
            "{0} er versjonen for den andre utgaven av spillet; den får spillet til å vise \"unofficial game modification\". Bruk {1}-versjonen.",
        ["NRaas ErrorTrap has no build known to work on EA app 1.69.47."] =
            "NRaas ErrorTrap har ingen versjon som er kjent for å virke med EA app 1.69.47.",
        ["{0} overrides game code Sims 3 Modernization also patches (not tested together)."] =
            "{0} overstyrer spillkode som Sims 3 Modernization også endrer (ikke testet sammen).",
        ["{0} is an old build with Apex inside; while it is in Game\\Bin, Apex Radiance stays off. Setup moves it to the backup folder (install the official Sims3SettingsSetter in its place if you used it)."] =
            "{0} er en gammel versjon med Apex inni; så lenge den ligger i Game\\Bin, er Apex Radiance av. Installasjonsprogrammet flytter den til sikkerhetskopimappen (installer den offisielle Sims3SettingsSetter i stedet hvis du brukte den).",
        ["Sims3Performance's ShortWaitSpin and ThreadStackDiet clash with Sims 3 Modernization. Setup sets both to 0 (a copy is kept as Sims3Performance.ini.apex-backup)."] =
            "ShortWaitSpin og ThreadStackDiet i Sims3Performance kolliderer med Sims 3 Modernization. Installasjonsprogrammet setter begge til 0 (en kopi lagres som Sims3Performance.ini.apex-backup).",
        ["Sims3SettingsSetter's uncompressed textures stutter under the RTX Remix bridge. Setup turns them off (a copy is kept as S3SS.toml.apex-backup)."] =
            "De ukomprimerte teksturene i Sims3SettingsSetter får spillet til å hakke med RTX Remix bridge. Installasjonsprogrammet slår dem av (en kopi lagres som S3SS.toml.apex-backup).",
        ["{0} sets client.targetProcess = {1}, but this game is {2}: the bridge does not start."] =
            "{0} setter client.targetProcess = {1}, men dette spillet er {2}: broen starter ikke.",
        ["dxvk.conf lacks {0} or has other values: without them the game resets its graphics options or freezes on a resolution change under the bridge."] =
            "dxvk.conf mangler {0} eller har andre verdier: uten dem tilbakestiller spillet grafikkvalgene eller fryser ved bytte av oppløsning med broen.",
        ["Mods\\Resource.cfg does not read {0}, so the game does not load it. Setup adds the Packages folder lines (a copy is kept as Resource.cfg.apex-backup)."] =
            "Mods\\Resource.cfg leser ikke {0}, så spillet laster den ikke. Installasjonsprogrammet legger til linjene for Packages-mappen (en kopi lagres som Resource.cfg.apex-backup).",
        ["Fix settings"] = "Rett innstillinger",
        ["Everything is installed and up to date."] = "Alt er installert og oppdatert.",
        ["Will be removed"] = "Blir fjernet",
        ["Keep"] = "Behold",
        ["Nothing to apply"] = "Ingenting å utføre",
        ["Preview"] = "Forhåndsvisning",
        ["This is a preview: nothing was installed."] = "Dette er en forhåndsvisning: ingenting ble installert.",
        ["Setup is applying changes"] = "Installasjonsprogrammet utfører endringer",
        ["Wait until it finishes before closing."] = "Vent til det er ferdig før du lukker.",
        ["Checking the latest versions…"] = "Ser etter nyeste versjoner…",
        ["Checking for updates…"] = "Ser etter oppdateringer…",
        ["Close The Sims 3 to update."] = "Lukk The Sims 3 for å oppdatere.",
        ["Update now installs them."] = "«Oppdater nå» installerer dem.",
        ["Graphics"] = "Grafikk",
        ["Checked at {0}."] = "Sjekket {0}.",
        ["Test build {0}: kept as it is."] = "Testversjon {0}: beholdes som den er.",
        ["Repair or remove sets the game right."] = "Under «Reparer eller fjern» kan du sette spillet i stand igjen.",
        ["Changes applied"] = "Endringene er utført",
        ["Removed: {0}"] = "Fjernet: {0}",
        ["The Sims 3 · {0}"] = "The Sims 3 · {0}",
        ["{0} → {1}"] = "{0} → {1}",
        ["{0} · up to date"] = "{0} · oppdatert",
        ["Everything is in place."] = "Alt er på plass.",
        ["Everything is in place (checked at {0})."] = "Alt er på plass (sjekket {0}).",
        ["Check installation"] = "Sjekk installasjonen",
        ["Change components"] = "Endre komponenter",
        ["Apex Radiance is not installed in this game."] = "Apex Radiance er ikke installert i dette spillet.",
        ["{0} · test build"] = "{0} · testversjon",
        ["Install the official {0}"] = "Installer den offisielle {0}",
        ["No ASI loader in Game\\Bin: Apex Radiance and the other .asi mods do not load."] =
            "Ingen ASI-laster i Game\\Bin: Apex Radiance og de andre .asi-modsene lastes ikke.",
        ["{0} is missing: the RTX Remix bridge does not start."] = "{0} mangler: RTX Remix bridge starter ikke.",
        ["Apply 1 change"] = "Utfør 1 endring",
        ["Apply {0} changes"] = "Utfør {0} endringer",
        ["{0} is installed and up to date: nothing to change."] = "{0} er installert og oppdatert: ingenting å endre.",
        ["Keep my d3d9.dll"] = "Behold min d3d9.dll",
        ["Your d3d9.dll stays as it is."] = "d3d9.dll-filen din blir som den er.",
        ["Applying changes"] = "Utfører endringer",
        ["Remove {0}"] = "Fjern {0}",
        ["Fix incompatible settings"] = "Rett inkompatible innstillinger",
        // suggestions: what Apex Radiance recommends in game (Compat.Suggestions); the game's own texts as the menu translates them
        ["Suggested for Apex Radiance"] = "Anbefalt for Apex Radiance",
        ["Apply"] = "Bruk",
        ["Applied."] = "Brukt.",
        ["Apply all suggestions"] = "Bruk alle forslag",
        ["Apply suggestion: {0}"] = "Bruk forslag: {0}",
        ["Two copies of the mod"] = "To kopier av moden",
        ["{0} in Game\\Bin is another copy of Apex Radiance; only one copy runs. Apply moves it to the backup folder."] = "{0} i Game\\Bin er en annen kopi av Apex Radiance; bare én kopi kjører. Bruk flytter den til sikkerhetskopimappen.",
        ["Turn off the game's Edge Smoothing"] = "Slå av spillets kantutjevning",
        ["Options.ini has edgesmoothing = {0}. The Sims 3's Edge Smoothing prevents these enabled Apex effects from working: {1}. Apply sets it to 0 (a copy is kept as Options.ini.apex-backup)."] = "Options.ini har edgesmoothing = {0}. Kantutjevningen i The Sims 3 hindrer disse aktive Apex-effektene i å virke: {1}. Bruk setter den til 0 (en kopi lagres som Options.ini.apex-backup).",
        ["Edge Smoothing"] = "Kantutjevning",
        ["Depth Blur"] = "Dybdeuskarphet",
        ["Ambient Occlusion"] = "Ambient Occlusion",
        ["Water Reflections"] = "Vannrefleksjoner",
        ["Runs the game on Vulkan: fewer stutters, and Apex Radiance is made and tested with it."] = "Kjører spillet på Vulkan: mindre hakking, og Apex Radiance er laget og testet med det.",
        ["Apex Radiance works on its own, but it pairs well with Sims3SettingsSetter by sims3fiend: a frame rate limiter, fewer stutters and many extra game settings."] = "Apex Radiance fungerer alene, men passer godt sammen med Sims3SettingsSetter av sims3fiend: en bildefrekvensbegrenser, mindre hakking og mange ekstra spillinnstillinger.",
        ["Moves DXVK into its own 64-bit process: the game keeps about 600 MB more free memory for long sessions and big worlds."] = "Flytter DXVK til sin egen 64-biters prosess: spillet beholder rundt 600 MB mer ledig minne til lange økter og store verdener.",
        ["Sims3SettingsSetter's borderless window"] = "Rammeløst vindu fra Sims3SettingsSetter",
        ["Sims3SettingsSetter's borderless window ({0}) does not apply under the RTX Remix bridge. Use the Window mode on Apex's Display page instead."] = "Det rammeløse vinduet fra Sims3SettingsSetter ({0}) virker ikke med RTX Remix bridge. Bruk heller Vindusmodus på Apex-siden Skjerm.",
        ["{0} has {1} = {2}, which Setup cannot read. Setup leaves the file as it is."] = "{0} har {1} = {2}, som installasjonsprogrammet ikke kan lese. Installasjonsprogrammet lar filen være som den er.",
        // restore points (Repair or remove)
        ["Install {0}"] = "Installer {0}",
        ["Restore points"] = "Gjenopprettingspunkter",
        ["Setup keeps a copy of every file it changes. Restoring a point undoes that change and every later one."] = "Installasjonsprogrammet tar vare på en kopi av hver fil det endrer. Å gjenopprette et punkt angrer den endringen og alle senere.",
        ["Before: {0}"] = "Før: {0}",
        ["{0} · 1 file"] = "{0} · 1 fil",
        ["{0} · {1} files"] = "{0} · {1} filer",
        ["Restore"] = "Gjenopprett",
        ["No restore points yet. Setup makes one before every change."] = "Ingen gjenopprettingspunkter ennå. Installasjonsprogrammet lager ett før hver endring.",
        ["Restore this point?"] = "Gjenopprette dette punktet?",
        ["Setup puts back the files as they were before this change, and undoes every change made after it. Close the game first."] = "Installasjonsprogrammet legger tilbake filene slik de var før denne endringen, og angrer alle endringer gjort etter den. Lukk spillet først.",
        ["Setup could not put back {0}."] = "Installasjonsprogrammet kunne ikke legge tilbake {0}.",
        ["Changes undone."] = "Endringene er angret.",
        ["Undo these changes"] = "Angre disse endringene",
        ["This restore point is gone."] = "Dette gjenopprettingspunktet finnes ikke lenger.",
        ["Updates available"] = "Oppdateringer tilgjengelig",
        ["Update now"] = "Oppdater nå",
        ["{0} {1} → {2}"] = "{0} {1} → {2}",
        ["Removes MonoPatcher.asi and its package."] = "Fjerner MonoPatcher.asi og pakken som hører til.",
        ["Smooth Patch's TS3Patch.asi patches the same game code as Sims3SettingsSetter's Smooth Patch. Setup moves it to the backup folder (S3SS does the same job)."] =
            "TS3Patch.asi fra Smooth Patch endrer den samme spillkoden som Smooth Patch i Sims3SettingsSetter. Installasjonsprogrammet flytter den til sikkerhetskopimappen (S3SS gjør den samme jobben).",
        ["simler90's Gameplay Core Mod and NRaas StoryProgression replace the same game code. Keep one of them."] =
            "Gameplay Core Mod fra simler90 og NRaas StoryProgression erstatter den samme spillkoden. Behold bare én av dem.",
        ["MonoPatcher needs the Microsoft Visual C++ Redistributable (x86); it is not installed."] =
            "MonoPatcher trenger Microsoft Visual C++ Redistributable (x86); den er ikke installert.",
        ["Download on Nexus Mods"] = "Last ned fra Nexus Mods",
        ["Choose the downloaded file"] = "Velg den nedlastede filen",
        ["Ready to install"] = "Klar til å installere",
        ["Waiting for your download in {0}…"] = "Venter på nedlastingen i {0}…",
        ["Not the Performance Patch"] = "Ikke Performance Patch",
        ["{0} has no Sims3Performance.asi. If the download is a .7z or .rar, extract it and choose Sims3Performance.asi."] =
            "{0} har ingen Sims3Performance.asi. Hvis nedlastingen er en .7z eller .rar, pakker du den ut og velger Sims3Performance.asi.",
        ["Removes Sims3Performance.asi and its .ini."] = "Fjerner Sims3Performance.asi og .ini-filen.",
        ["Download Sims 3 Performance Patch from Nexus Mods first."] =
            "Last ned Sims 3 Performance Patch fra Nexus Mods først.",
        ["Finds your The Sims 3 (Steam or EA app)."] = "Finner The Sims 3 (Steam eller EA app).",
        ["Installs Apex Radiance and the fixes you choose."] = "Installerer Apex Radiance og rettingene du velger.",
        ["Keeps a copy of every file it changes, so you can undo everything."] =
            "Tar vare på en kopi av hver fil det endrer, så du kan angre alt.",
        ["Your Apex Radiance settings are always kept."] = "Apex Radiance-innstillingene dine beholdes alltid.",
        ["No internet connection"] = "Ingen internettforbindelse",
        ["Vulkan, DXVK (recommended)"] = "Vulkan, DXVK (anbefalt)",
        ["Vulkan with more memory (RTX Remix bridge)"] = "Vulkan med mer minne (RTX Remix bridge)",
        ["Windows default (Direct3D 9)"] = "Windows-standard (Direct3D 9)",
        ["Fewer stutters; Apex Radiance is made and tested with it."] =
            "Mindre hakking; Apex Radiance er laget og testet med det.",
        ["Keeps about 600 MB more memory free for big worlds. Needs a Vulkan graphics card. If the game misbehaves with it, choose DXVK."] =
            "Holder rundt 600 MB mer minne ledig for store verdener. Krever et skjermkort med Vulkan. Hvis spillet oppfører seg rart med det, velger du DXVK.",
        ["Works, but Apex Radiance is tested with DXVK."] = "Virker, men Apex Radiance er testet med DXVK.",
        ["Your in-game graphics settings may reset once. Setup keeps a copy and puts it back if you switch back."] =
            "Grafikkinnstillingene i spillet kan bli tilbakestilt én gang. Installasjonsprogrammet tar vare på en kopi og legger den tilbake hvis du bytter tilbake.",
        ["Fixes for game and graphics settings."] = "Rettinger for spill- og grafikkinnstillinger.",
        ["Fixes stutters, Sims getting stuck and crashes."] = "Retter hakking, Simmer som setter seg fast, og krasj.",
        ["Needed only by some script mods; turn it on if a mod asks for it."] =
            "Trengs bare av noen skriptmods; slå den på hvis en mod ber om det.",
        ["Makes the game run faster. Needs a free Nexus Mods account; Setup installs it when your download finishes."] =
            "Får spillet til å gå raskere. Krever en gratis Nexus Mods-konto; installasjonsprogrammet installerer den når nedlastingen er ferdig.",
        ["Puts back the graphics files you had before."] = "Legger tilbake grafikkfilene du hadde før.",
        ["Goes back to the game's default graphics. Your in-game graphics settings may reset once."] =
            "Går tilbake til spillets standardgrafikk. Grafikkinnstillingene i spillet kan bli tilbakestilt én gang.",
        ["Language"] = "Språk",
        ["Automatic"] = "Automatisk",
        ["This file has no Sims3Performance.asi."] = "Denne filen har ingen Sims3Performance.asi.",
        ["Dry run: Setup changes nothing."] = "Prøvekjøring: installasjonsprogrammet endrer ingenting.",
        ["Apex Radiance by loinyx. The RTX Remix bridge is NVIDIA's, ported to The Sims 3 by dalexlol and alexvixell35-stack. Sims3SettingsSetter is by sims3fiend, Sims 3 Modernization by rodneypantonial and Ultimate ASI Loader by ThirteenAG: Setup downloads them from their own GitHub releases."] =
            "Apex Radiance av loinyx. RTX Remix bridge er fra NVIDIA, overført til The Sims 3 av dalexlol og alexvixell35-stack. Sims3SettingsSetter er laget av sims3fiend, Sims 3 Modernization av rodneypantonial og Ultimate ASI Loader av ThirteenAG: installasjonsprogrammet laster dem ned fra deres egne GitHub-utgivelser.",
        ["RTX Remix bridge: third-party notices"] = "RTX Remix bridge: tredjepartsmerknader",
        ["Read"] = "Les",
        ["Licence text"] = "Lisenstekst",
        ["OK"] = "OK",
        ["Cancel"] = "Avbryt",
    };
}
