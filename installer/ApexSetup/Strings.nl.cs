// Apex Radiance Setup texts in Dutch (Nederlands). Fill only the entries between the braces, one per line:
//     ["<English key, exactly as in keys.json>"] = "<translation>",
// C# strings: write \" for a quote, \\ for a backslash, \n for a line break; every other character as itself (UTF-8).
// Keep each {0}, {1} of the key, product and file names as they are. A key without an entry shows in English; never
// leave a translation empty. The same key twice: the last one wins, and ApexSetup.Tests fails.
namespace ApexSetup;

public static partial class Strings
{
    static readonly Dictionary<string, string> Nl = new()
    {
        ["Welcome"] = "Welkom",
        ["Game"] = "Spel",
        ["Components"] = "Onderdelen",
        ["Install"] = "Installeren",
        ["Done"] = "Klaar",
        ["Repair or remove"] = "Herstellen of verwijderen",
        ["About and licences"] = "Over en licenties",
        ["Back"] = "Terug",
        ["Next"] = "Volgende",
        ["Get started"] = "Beginnen",
        ["Finish"] = "Voltooien",
        ["Close The Sims 3 to continue."] = "Sluit The Sims 3 om door te gaan.",
        ["Setup needs the internet to download the files."] = "Setup heeft internet nodig om de bestanden te downloaden.",
        ["Nothing changes until you click Install."] = "Er verandert niets tot je op Installeren klikt.",
        ["Lighting, picture and performance for The Sims 3."] = "Verlichting, beeld en prestaties voor The Sims 3.",
        ["What Setup does"] = "Wat Setup doet",
        ["Setup can't reach GitHub"] = "Setup kan GitHub niet bereiken",
        ["Check your internet connection, then try again."] = "Controleer je internetverbinding en probeer het opnieuw.",
        ["Try again"] = "Opnieuw proberen",
        ["Choose your game"] = "Kies je spel",
        ["Setup found these installs. Pick the one you play."] = "Setup heeft deze installaties gevonden. Kies degene die je speelt.",
        ["Setup found no install. Browse for the game folder."] = "Setup heeft geen installatie gevonden. Zoek de map van het spel.",
        ["Browse for another folder"] = "Andere map zoeken",
        ["No game in this folder"] = "Geen spel in deze map",
        ["Pick the The Sims 3 folder, or its Game\\Bin folder (the one with TS3W.exe or TS3.exe)."] =
            "Kies de map van The Sims 3, of de map Game\\Bin ervan (die met TS3W.exe of TS3.exe).",
        ["Not installed"] = "Niet geïnstalleerd",
        ["Installed"] = "Geïnstalleerd",
        ["the game's own Direct3D 9"] = "de eigen Direct3D 9 van het spel",
        ["another d3d9.dll"] = "een andere d3d9.dll",
        ["Required"] = "Vereist",
        ["Installs {0} into Game\\Bin."] = "Installeert {0} in Game\\Bin.",
        ["Replaces the installed copy with {0}. Your settings stay."] = "Vervangt de geïnstalleerde kopie door {0}. Je instellingen blijven.",
        ["Updates {0} to {1}. Your settings stay."] = "Werkt {0} bij naar {1}. Je instellingen blijven.",
        ["Recommended with Apex"] = "Aanbevolen met Apex",
        ["No updates available"] = "Geen updates beschikbaar",
        ["Everything in this game is on its latest version."] = "Alles in dit spel heeft de nieuwste versie.",
        ["Not available for this game version: it supports Steam 1.67.2 and EA app 1.69.47."] =
            "Niet beschikbaar voor deze spelversie: ondersteunt Steam 1.67.2 en EA app 1.69.47.",
        ["Wrong build"] = "Verkeerde versie",
        ["Update {0}"] = "{0} bijwerken",
        ["Up to date"] = "Actueel",
        ["Setup could not finish"] = "Setup kon niet voltooien",
        ["Close The Sims 3 first."] = "Sluit eerst The Sims 3.",
        ["You're all set"] = "Je bent er klaar voor",
        ["Start The Sims 3, load a world and press Ctrl+Shift+F11 to open the Apex Radiance menu."] =
            "Start The Sims 3, laad een wereld en druk op Ctrl+Shift+F11 om het menu van Apex Radiance te openen.",
        ["Installed: {0} {1}"] = "Geïnstalleerd: {0} {1}",
        ["Choose your game first."] = "Kies eerst je spel.",
        ["Removes ApexRadiance.asi. Your settings in Documents stay."] = "Verwijdert ApexRadiance.asi. Je instellingen in Documenten blijven.",
        ["Remove"] = "Verwijderen",
        ["RTX Remix bridge"] = "RTX Remix bridge",
        ["Removes Sims3SettingsSetter.asi."] = "Verwijdert Sims3SettingsSetter.asi.",
        ["Removes ts3native and ts3fixes.package."] = "Verwijdert ts3native en ts3fixes.package.",
        ["Repair or update"] = "Herstellen of bijwerken",
        ["Downloads and copies again everything installed here. Your settings stay."] =
            "Downloadt en kopieert alles wat hier is geïnstalleerd opnieuw. Je instellingen blijven.",
        ["Repair"] = "Herstellen",
        ["Uninstall everything"] = "Alles verwijderen",
        ["Removes what Setup installed, and Apex Radiance, and puts back every file Setup replaced."] =
            "Verwijdert wat Setup heeft geïnstalleerd en Apex Radiance, en zet elk bestand terug dat Setup heeft vervangen.",
        ["Uninstall"] = "Verwijderen",
        ["Remove {0}?"] = "{0} verwijderen?",
        ["Close the game first. Setup removes the files and puts back the ones it replaced."] =
            "Sluit eerst het spel. Setup verwijdert de bestanden en zet de vervangen bestanden terug.",
        ["{0} removed."] = "{0} verwijderd.",
        ["Uninstall everything?"] = "Alles verwijderen?",
        ["Setup removes Apex Radiance and everything it installed in this game, and puts back every file it replaced. Your settings in Documents stay."] =
            "Setup verwijdert Apex Radiance en alles wat het in dit spel heeft geïnstalleerd, en zet elk vervangen bestand terug. Je instellingen in Documenten blijven.",
        ["Everything Setup installed was removed."] = "Alles wat Setup had geïnstalleerd, is verwijderd.",
        ["Setup fixes this"] = "Setup lost dit op",
        ["Read more"] = "Meer lezen",
        ["Fixed."] = "Opgelost.",
        ["No incompatible settings found."] = "Geen onverenigbare instellingen gevonden.",
        ["{0} in Game\\Bin loads a second copy of Apex Radiance."] = "{0} in Game\\Bin laadt een tweede kopie van Apex Radiance.",
        ["Two ASI loaders ({0}) load every mod twice. Keep one: remove all but {1}."] =
            "Twee ASI-loaders ({0}) laden elke mod twee keer. Houd er één: verwijder alles behalve {1}.",
        ["Sims 3 Modernization's ts3native is the build for the other edition and does nothing. Turn it on in Components to install the right one."] =
            "De ts3native van Sims 3 Modernization is de versie voor de andere editie en doet niets. Zet hem aan bij Onderdelen om de juiste te installeren.",
        ["Sims 3 Modernization has no build for this game version ({0}), so its ts3native does nothing. Turn it off in Components."] =
            "Sims 3 Modernization heeft geen versie voor deze spelversie ({0}), dus de ts3native doet niets. Zet hem uit bij Onderdelen.",
        ["{0} in Game\\Bin is a second copy of {1}; both load. Setup moves it to the backup folder."] =
            "{0} in Game\\Bin is een tweede kopie van {1}; beide laden. Setup verplaatst hem naar de back-upmap.",
        ["Mods has {0} .package files but no Resource.cfg, so the game loads none of them."] =
            "Mods bevat {0} .package-bestanden maar geen Resource.cfg, dus het spel laadt er geen van.",
        ["{0} is the build for the other edition of the game; it makes the game show \"unofficial game modification\". Use the {1} build."] =
            "{0} is de versie voor de andere editie van het spel; daardoor toont het spel \"unofficial game modification\". Gebruik de {1}-versie.",
        ["NRaas ErrorTrap has no build known to work on EA app 1.69.47."] = "Van NRaas ErrorTrap is geen versie bekend die werkt op EA app 1.69.47.",
        ["{0} overrides game code Sims 3 Modernization also patches (not tested together)."] =
            "{0} vervangt spelcode die Sims 3 Modernization ook aanpast (niet samen getest).",
        ["{0} is an old build with Apex inside; while it is in Game\\Bin, Apex Radiance stays off. Setup moves it to the backup folder (install the official Sims3SettingsSetter in its place if you used it)."] =
            "{0} is een oude versie met Apex erin; zolang die in Game\\Bin staat, blijft Apex Radiance uit. Setup verplaatst hem naar de back-upmap (installeer de officiële Sims3SettingsSetter in de plaats ervan als je die gebruikte).",
        ["Sims3Performance's ShortWaitSpin and ThreadStackDiet clash with Sims 3 Modernization. Setup sets both to 0 (a copy is kept as Sims3Performance.ini.apex-backup)."] =
            "ShortWaitSpin en ThreadStackDiet van Sims3Performance botsen met Sims 3 Modernization. Setup zet beide op 0 (er blijft een kopie als Sims3Performance.ini.apex-backup).",
        ["Sims3SettingsSetter's uncompressed textures stutter under the RTX Remix bridge. Setup turns them off (a copy is kept as S3SS.toml.apex-backup)."] =
            "De ongecomprimeerde texturen van Sims3SettingsSetter laten het beeld haperen met de RTX Remix bridge. Setup zet ze uit (er blijft een kopie als S3SS.toml.apex-backup).",
        ["{0} sets client.targetProcess = {1}, but this game is {2}: the bridge does not start."] =
            "{0} stelt client.targetProcess = {1} in, maar dit spel is {2}: de bridge start niet.",
        ["dxvk.conf lacks {0} or has other values: without them the game resets its graphics options or freezes on a resolution change under the bridge."] =
            "In dxvk.conf ontbreekt {0} of staan andere waarden: zonder die waarden zet het spel de grafische opties terug of loopt het vast bij een andere resolutie met de bridge.",
        ["Mods\\Resource.cfg does not read {0}, so the game does not load it. Setup adds the Packages folder lines (a copy is kept as Resource.cfg.apex-backup)."] =
            "Mods\\Resource.cfg leest {0} niet, dus het spel laadt het niet. Setup voegt de regels voor de map Packages toe (er blijft een kopie als Resource.cfg.apex-backup).",
        ["Fix settings"] = "Instellingen corrigeren",
        ["Everything is installed and up to date."] = "Alles is geïnstalleerd en actueel.",
        ["Will be removed"] = "Wordt verwijderd",
        ["Keep"] = "Behouden",
        ["Nothing to apply"] = "Niets toe te passen",
        ["Preview"] = "Voorbeeld",
        ["This is a preview: nothing was installed."] = "Dit is een voorbeeld: er is niets geïnstalleerd.",
        ["Setup is applying changes"] = "Setup voert wijzigingen door",
        ["Wait until it finishes before closing."] = "Wacht tot het klaar is voordat je afsluit.",
        ["Checking the latest versions…"] = "Nieuwste versies controleren…",
        ["Checking for updates…"] = "Controleren op updates…",
        ["Close The Sims 3 to update."] = "Sluit The Sims 3 om bij te werken.",
        ["Update now installs them."] = "Nu bijwerken installeert ze.",
        ["Graphics"] = "Graphics",
        ["Checked at {0}."] = "Gecontroleerd om {0}.",
        ["Test build {0}: kept as it is."] = "Testversie {0}: blijft zoals hij is.",
        ["Repair or remove sets the game right."] = "Herstellen of verwijderen zet het spel weer goed.",
        ["Changes applied"] = "Wijzigingen toegepast",
        ["Removed: {0}"] = "Verwijderd: {0}",
        ["The Sims 3 · {0}"] = "The Sims 3 · {0}",
        ["{0} → {1}"] = "{0} → {1}",
        ["{0} · up to date"] = "{0} · actueel",
        ["Everything is in place."] = "Alles is in orde.",
        ["Everything is in place (checked at {0})."] = "Alles is in orde (gecontroleerd om {0}).",
        ["Check installation"] = "Installatie controleren",
        ["Change components"] = "Onderdelen wijzigen",
        ["Apex Radiance is not installed in this game."] = "Apex Radiance is niet geïnstalleerd in dit spel.",
        ["{0} · test build"] = "{0} · testversie",
        ["Install the official {0}"] = "Officiële {0} installeren",
        ["No ASI loader in Game\\Bin: Apex Radiance and the other .asi mods do not load."] =
            "Geen ASI-loader in Game\\Bin: Apex Radiance en de andere .asi-mods laden niet.",
        ["{0} is missing: the RTX Remix bridge does not start."] = "{0} ontbreekt: de RTX Remix bridge start niet.",
        ["Apply 1 change"] = "1 wijziging toepassen",
        ["Apply {0} changes"] = "{0} wijzigingen toepassen",
        ["{0} is installed and up to date: nothing to change."] = "{0} is geïnstalleerd en actueel: niets te wijzigen.",
        ["Keep my d3d9.dll"] = "Mijn d3d9.dll houden",
        ["Your d3d9.dll stays as it is."] = "Je d3d9.dll blijft zoals hij is.",
        ["Applying changes"] = "Wijzigingen worden toegepast",
        ["Remove {0}"] = "{0} verwijderen",
        ["Fix incompatible settings"] = "Onverenigbare instellingen corrigeren",
        ["Updates available"] = "Updates beschikbaar",
        ["Update now"] = "Nu bijwerken",
        ["{0} {1} → {2}"] = "{0} {1} → {2}",
        ["Removes MonoPatcher.asi and its package."] = "Verwijdert MonoPatcher.asi en het bijbehorende pakket.",
        ["Smooth Patch's TS3Patch.asi patches the same game code as Sims3SettingsSetter's Smooth Patch. Setup moves it to the backup folder (S3SS does the same job)."] =
            "TS3Patch.asi van Smooth Patch past dezelfde spelcode aan als de Smooth Patch van Sims3SettingsSetter. Setup verplaatst hem naar de back-upmap (S3SS doet hetzelfde werk).",
        ["simler90's Gameplay Core Mod and NRaas StoryProgression replace the same game code. Keep one of them."] =
            "De Gameplay Core Mod van simler90 en NRaas StoryProgression vervangen dezelfde spelcode. Houd er één van.",
        ["MonoPatcher needs the Microsoft Visual C++ Redistributable (x86); it is not installed."] =
            "MonoPatcher heeft de Microsoft Visual C++ Redistributable (x86) nodig; die is niet geïnstalleerd.",
        ["Download on Nexus Mods"] = "Downloaden op Nexus Mods",
        ["Choose the downloaded file"] = "Gedownload bestand kiezen",
        ["Ready to install"] = "Klaar om te installeren",
        ["Waiting for your download in {0}…"] = "Wachten op je download in {0}…",
        ["Not the Performance Patch"] = "Niet de Performance Patch",
        ["{0} has no Sims3Performance.asi. If the download is a .7z or .rar, extract it and choose Sims3Performance.asi."] =
            "{0} bevat geen Sims3Performance.asi. Is de download een .7z of .rar, pak hem dan uit en kies Sims3Performance.asi.",
        ["Removes Sims3Performance.asi and its .ini."] = "Verwijdert Sims3Performance.asi en de bijbehorende .ini.",
        ["Download Sims 3 Performance Patch from Nexus Mods first."] = "Download eerst Sims 3 Performance Patch van Nexus Mods.",
        ["Finds your The Sims 3 (Steam or EA app)."] = "Vindt je The Sims 3 (Steam of EA app).",
        ["Installs Apex Radiance and the fixes you choose."] = "Installeert Apex Radiance en de oplossingen die je kiest.",
        ["Keeps a copy of every file it changes, so you can undo everything."] =
            "Bewaart een kopie van elk bestand dat het wijzigt, zodat je alles ongedaan kunt maken.",
        ["Your Apex Radiance settings are always kept."] = "Je Apex Radiance-instellingen blijven altijd bewaard.",
        ["No internet connection"] = "Geen internetverbinding",
        ["Vulkan, DXVK (recommended)"] = "Vulkan, DXVK (aanbevolen)",
        ["Vulkan with more memory (RTX Remix bridge)"] = "Vulkan met meer geheugen (RTX Remix bridge)",
        ["Windows default (Direct3D 9)"] = "Windows-standaard (Direct3D 9)",
        ["Fewer stutters; Apex Radiance is made and tested with it."] = "Minder haperingen; Apex Radiance is ermee gemaakt en getest.",
        ["Keeps about 600 MB more memory free for big worlds. Needs a Vulkan graphics card. If the game misbehaves with it, choose DXVK."] =
            "Houdt ongeveer 600 MB meer geheugen vrij voor grote werelden. Vereist een videokaart met Vulkan. Doet het spel er raar mee, kies dan DXVK.",
        ["Works, but Apex Radiance is tested with DXVK."] = "Werkt, maar Apex Radiance is getest met DXVK.",
        ["Your in-game graphics settings may reset once. Setup keeps a copy and puts it back if you switch back."] =
            "Je grafische instellingen in het spel kunnen één keer worden teruggezet. Setup bewaart een kopie en zet die terug als je terugschakelt.",
        ["Fixes for game and graphics settings."] = "Oplossingen voor spel- en grafische instellingen.",
        ["Fixes stutters, Sims getting stuck and crashes."] = "Lost haperingen, vastzittende Sims en crashes op.",
        ["Needed only by some script mods; turn it on if a mod asks for it."] = "Alleen nodig voor sommige scriptmods; zet het aan als een mod erom vraagt.",
        ["Makes the game run faster. Needs a free Nexus Mods account; Setup installs it when your download finishes."] =
            "Laat het spel sneller lopen. Vereist een gratis Nexus Mods-account; Setup installeert het zodra je download klaar is.",
        ["Puts back the graphics files you had before."] = "Zet de grafische bestanden terug die je eerder had.",
        ["Goes back to the game's default graphics. Your in-game graphics settings may reset once."] =
            "Gaat terug naar de standaardgraphics van het spel. Je grafische instellingen in het spel kunnen één keer worden teruggezet.",
        ["Language"] = "Taal",
        ["Automatic"] = "Automatisch",
        ["This file has no Sims3Performance.asi."] = "Dit bestand bevat geen Sims3Performance.asi.",
        ["Dry run: Setup changes nothing."] = "Proefrun: Setup wijzigt niets.",
        ["Apex Radiance by loinyx. The RTX Remix bridge is NVIDIA's, ported to The Sims 3 by dalexlol and alexvixell35-stack. Sims3SettingsSetter is by sims3fiend, Sims 3 Modernization by rodneypantonial and Ultimate ASI Loader by ThirteenAG: Setup downloads them from their own GitHub releases."] =
            "Apex Radiance door loinyx. De RTX Remix bridge is van NVIDIA, geporteerd naar The Sims 3 door dalexlol en alexvixell35-stack. Sims3SettingsSetter is van sims3fiend, Sims 3 Modernization van rodneypantonial en Ultimate ASI Loader van ThirteenAG: Setup downloadt ze van hun eigen GitHub-releases.",
        ["RTX Remix bridge: third-party notices"] = "RTX Remix bridge: kennisgevingen van derden",
        ["Read"] = "Lezen",
        ["Licence text"] = "Licentietekst",
        ["OK"] = "OK",
        ["Cancel"] = "Annuleren",
    };
}
