// Apex Radiance Setup texts in German (Deutsch). Fill only the entries between the braces, one per line:
//     ["<English key, exactly as in keys.json>"] = "<translation>",
// C# strings: write \" for a quote, \\ for a backslash, \n for a line break; every other character as itself (UTF-8).
// Keep each {0}, {1} of the key, product and file names as they are. A key without an entry shows in English; never
// leave a translation empty. The same key twice: the last one wins, and ApexSetup.Tests fails.
namespace ApexSetup;

public static partial class Strings
{
    static readonly Dictionary<string, string> De = new()
    {
        ["Welcome"] = "Willkommen",
        ["Game"] = "Spiel",
        ["Components"] = "Komponenten",
        ["Install"] = "Installieren",
        ["Done"] = "Fertig",
        ["Repair or remove"] = "Reparieren oder entfernen",
        ["About and licences"] = "Info und Lizenzen",
        ["Back"] = "Zurück",
        ["Next"] = "Weiter",
        ["Get started"] = "Loslegen",
        ["Finish"] = "Fertigstellen",
        ["Close The Sims 3 to continue."] = "Schließe The Sims 3, um fortzufahren.",
        ["Setup needs the internet to download the files."] = "Setup braucht Internet, um die Dateien herunterzuladen.",
        ["Nothing changes until you click Install."] = "Nichts ändert sich, bis du auf Installieren klickst.",
        ["Lighting, picture and performance for The Sims 3."] = "Beleuchtung, Bild und Leistung für The Sims 3.",
        ["What Setup does"] = "Was Setup macht",
        ["Setup can't reach GitHub"] = "Setup erreicht GitHub nicht",
        ["Check your internet connection, then try again."] = "Prüfe deine Internetverbindung und versuche es erneut.",
        ["Try again"] = "Erneut versuchen",
        ["Choose your game"] = "Wähle dein Spiel",
        ["Setup found these installs. Pick the one you play."] =
            "Setup hat diese Installationen gefunden. Wähle die, die du spielst.",
        ["Setup found no install. Browse for the game folder."] =
            "Setup hat keine Installation gefunden. Suche den Spielordner.",
        ["Browse for another folder"] = "Anderen Ordner suchen",
        ["No game in this folder"] = "Kein Spiel in diesem Ordner",
        ["Pick the The Sims 3 folder, or its Game\\Bin folder (the one with TS3W.exe or TS3.exe)."] =
            "Wähle den Ordner von The Sims 3 oder seinen Ordner Game\\Bin (den mit TS3W.exe oder TS3.exe).",
        ["Not installed"] = "Nicht installiert",
        ["Installed"] = "Installiert",
        ["the game's own Direct3D 9"] = "das eigene Direct3D 9 des Spiels",
        ["another d3d9.dll"] = "eine andere d3d9.dll",
        ["Required"] = "Erforderlich",
        ["Installs {0} into Game\\Bin."] = "Installiert {0} in Game\\Bin.",
        ["Replaces the installed copy with {0}. Your settings stay."] =
            "Ersetzt die installierte Kopie durch {0}. Deine Einstellungen bleiben.",
        ["Updates {0} to {1}. Your settings stay."] = "Aktualisiert {0} auf {1}. Deine Einstellungen bleiben.",
        ["Recommended with Apex"] = "Empfohlen mit Apex",
        ["No updates available"] = "Keine Updates verfügbar",
        ["Everything in this game is on its latest version."] = "In diesem Spiel ist alles auf dem neuesten Stand.",
        ["Not available for this game version: it supports Steam 1.67.2 and EA app 1.69.47."] =
            "Für diese Spielversion nicht verfügbar: unterstützt werden Steam 1.67.2 und EA app 1.69.47.",
        ["Wrong build"] = "Falscher Build",
        ["Update {0}"] = "Update {0}",
        ["Up to date"] = "Aktuell",
        ["Setup could not finish"] = "Setup konnte nicht abgeschlossen werden",
        ["Close The Sims 3 first."] = "Schließe zuerst The Sims 3.",
        ["You're all set"] = "Alles bereit",
        ["Start The Sims 3, load a world and press Ctrl+Shift+F11 to open the Apex Radiance menu."] =
            "Starte The Sims 3, lade eine Welt und drücke Ctrl+Shift+F11, um das Menü von Apex Radiance zu öffnen.",
        ["Installed: {0} {1}"] = "Installiert: {0} {1}",
        ["Choose your game first."] = "Wähle zuerst dein Spiel.",
        ["Removes ApexRadiance.asi. Your settings in Documents stay."] =
            "Entfernt ApexRadiance.asi. Deine Einstellungen in Dokumente bleiben.",
        ["Remove"] = "Entfernen",
        ["RTX Remix bridge"] = "RTX Remix bridge",
        ["Removes Sims3SettingsSetter.asi."] = "Entfernt Sims3SettingsSetter.asi.",
        ["Removes ts3native and ts3fixes.package."] = "Entfernt ts3native und ts3fixes.package.",
        ["Repair or update"] = "Reparieren oder aktualisieren",
        ["Downloads and copies again everything installed here. Your settings stay."] =
            "Lädt alles hier Installierte erneut herunter und kopiert es neu. Deine Einstellungen bleiben.",
        ["Repair"] = "Reparieren",
        ["Uninstall everything"] = "Alles deinstallieren",
        ["Removes what Setup installed, and Apex Radiance, and puts back every file Setup replaced."] =
            "Entfernt, was Setup installiert hat, und Apex Radiance, und stellt jede von Setup ersetzte Datei wieder her.",
        ["Uninstall"] = "Deinstallieren",
        ["Remove {0}?"] = "{0} entfernen?",
        ["Close the game first. Setup removes the files and puts back the ones it replaced."] =
            "Schließe zuerst das Spiel. Setup entfernt die Dateien und stellt die ersetzten wieder her.",
        ["{0} removed."] = "{0} entfernt.",
        ["Uninstall everything?"] = "Alles deinstallieren?",
        ["Setup removes Apex Radiance and everything it installed in this game, and puts back every file it replaced. Your settings in Documents stay."] =
            "Setup entfernt Apex Radiance und alles, was es in diesem Spiel installiert hat, und stellt jede ersetzte Datei wieder her. Deine Einstellungen in Dokumente bleiben.",
        ["Everything Setup installed was removed."] = "Alles, was Setup installiert hat, wurde entfernt.",
        ["Setup fixes this"] = "Setup behebt das",
        ["Read more"] = "Mehr erfahren",
        ["Fixed."] = "Behoben.",
        ["No incompatible settings found."] = "Keine inkompatiblen Einstellungen gefunden.",
        ["{0} in Game\\Bin loads a second copy of Apex Radiance."] =
            "{0} in Game\\Bin lädt eine zweite Kopie von Apex Radiance.",
        ["Two ASI loaders ({0}) load every mod twice. Keep one: remove all but {1}."] =
            "Zwei ASI-Loader ({0}) laden jede Mod doppelt. Behalte einen: Entferne alle außer {1}.",
        ["Sims 3 Modernization's ts3native is the build for the other edition and does nothing. Turn it on in Components to install the right one."] =
            "Der ts3native von Sims 3 Modernization ist der Build für die andere Edition und bewirkt nichts. Schalte ihn unter Komponenten ein, um den richtigen zu installieren.",
        ["Sims 3 Modernization has no build for this game version ({0}), so its ts3native does nothing. Turn it off in Components."] =
            "Sims 3 Modernization hat keinen Build für diese Spielversion ({0}), daher bewirkt sein ts3native nichts. Schalte ihn unter Komponenten aus.",
        ["{0} in Game\\Bin is a second copy of {1}; both load. Setup moves it to the backup folder."] =
            "{0} in Game\\Bin ist eine zweite Kopie von {1}; beide werden geladen. Setup verschiebt sie in den Sicherungsordner.",
        ["Mods has {0} .package files but no Resource.cfg, so the game loads none of them."] =
            "Mods enthält {0} .package-Dateien, aber keine Resource.cfg, daher lädt das Spiel keine davon.",
        ["{0} is the build for the other edition of the game; it makes the game show \"unofficial game modification\". Use the {1} build."] =
            "{0} ist der Build für die andere Edition des Spiels; er lässt das Spiel \"unofficial game modification\" anzeigen. Verwende den Build {1}.",
        ["NRaas ErrorTrap has no build known to work on EA app 1.69.47."] =
            "Für NRaas ErrorTrap ist kein Build bekannt, der mit EA app 1.69.47 funktioniert.",
        ["{0} overrides game code Sims 3 Modernization also patches (not tested together)."] =
            "{0} ersetzt Spielcode, den auch Sims 3 Modernization ändert (nicht zusammen getestet).",
        ["{0} is an old build with Apex inside; while it is in Game\\Bin, Apex Radiance stays off. Setup moves it to the backup folder (install the official Sims3SettingsSetter in its place if you used it)."] =
            "{0} ist ein alter Build mit Apex darin; solange er in Game\\Bin liegt, bleibt Apex Radiance aus. Setup verschiebt ihn in den Sicherungsordner (installiere stattdessen den offiziellen Sims3SettingsSetter, falls du ihn genutzt hast).",
        ["Sims3Performance's ShortWaitSpin and ThreadStackDiet clash with Sims 3 Modernization. Setup sets both to 0 (a copy is kept as Sims3Performance.ini.apex-backup)."] =
            "ShortWaitSpin und ThreadStackDiet von Sims3Performance vertragen sich nicht mit Sims 3 Modernization. Setup setzt beide auf 0 (eine Kopie bleibt als Sims3Performance.ini.apex-backup).",
        ["Sims3SettingsSetter's uncompressed textures stutter under the RTX Remix bridge. Setup turns them off (a copy is kept as S3SS.toml.apex-backup)."] =
            "Die unkomprimierten Texturen von Sims3SettingsSetter ruckeln mit der RTX Remix bridge. Setup schaltet sie aus (eine Kopie bleibt als S3SS.toml.apex-backup).",
        ["{0} sets client.targetProcess = {1}, but this game is {2}: the bridge does not start."] =
            "{0} setzt client.targetProcess = {1}, aber dieses Spiel ist {2}: Die bridge startet nicht.",
        ["dxvk.conf lacks {0} or has other values: without them the game resets its graphics options or freezes on a resolution change under the bridge."] =
            "In dxvk.conf fehlt {0} oder der Wert ist anders: Ohne ihn setzt das Spiel seine Grafikoptionen zurück oder friert mit der bridge beim Ändern der Auflösung ein.",
        ["Mods\\Resource.cfg does not read {0}, so the game does not load it. Setup adds the Packages folder lines (a copy is kept as Resource.cfg.apex-backup)."] =
            "Mods\\Resource.cfg liest {0} nicht, daher lädt das Spiel es nicht. Setup ergänzt die Zeilen für den Ordner Packages (eine Kopie bleibt als Resource.cfg.apex-backup).",
        ["Fix settings"] = "Einstellungen korrigieren",
        ["Everything is installed and up to date."] = "Alles ist installiert und aktuell.",
        ["Will be removed"] = "Wird entfernt",
        ["Keep"] = "Behalten",
        ["Nothing to apply"] = "Nichts anzuwenden",
        ["Preview"] = "Vorschau",
        ["This is a preview: nothing was installed."] = "Dies ist eine Vorschau: Es wurde nichts installiert.",
        ["Setup is applying changes"] = "Setup wendet Änderungen an",
        ["Wait until it finishes before closing."] = "Warte, bis es fertig ist, bevor du schließt.",
        ["Checking the latest versions…"] = "Neueste Versionen werden geprüft…",
        ["Checking for updates…"] = "Suche nach Updates…",
        ["Close The Sims 3 to update."] = "Schließe The Sims 3, um zu aktualisieren.",
        ["Update now installs them."] = "„Jetzt aktualisieren“ installiert sie.",
        ["Graphics"] = "Grafik",
        ["Checked at {0}."] = "Geprüft um {0}.",
        ["Test build {0}: kept as it is."] = "Testbuild {0}: bleibt, wie er ist.",
        ["Repair or remove sets the game right."] = "Unter „Reparieren oder entfernen“ bringst du das Spiel wieder in Ordnung.",
        ["Changes applied"] = "Änderungen angewendet",
        ["Removed: {0}"] = "Entfernt: {0}",
        ["The Sims 3 · {0}"] = "The Sims 3 · {0}",
        ["{0} → {1}"] = "{0} → {1}",
        ["{0} · up to date"] = "{0} · aktuell",
        ["Everything is in place."] = "Alles in Ordnung.",
        ["Everything is in place (checked at {0})."] = "Alles in Ordnung (geprüft um {0}).",
        ["Check installation"] = "Installation prüfen",
        ["Change components"] = "Komponenten ändern",
        ["Apex Radiance is not installed in this game."] = "Apex Radiance ist in diesem Spiel nicht installiert.",
        ["{0} · test build"] = "{0} · Testbuild",
        ["Install the official {0}"] = "Offizielle Version {0} installieren",
        ["No ASI loader in Game\\Bin: Apex Radiance and the other .asi mods do not load."] =
            "Kein ASI-Loader in Game\\Bin: Apex Radiance und die anderen .asi-Mods werden nicht geladen.",
        ["{0} is missing: the RTX Remix bridge does not start."] = "{0} fehlt: Die RTX Remix bridge startet nicht.",
        ["Apply 1 change"] = "1 Änderung anwenden",
        ["Apply {0} changes"] = "{0} Änderungen anwenden",
        ["{0} is installed and up to date: nothing to change."] = "{0} ist installiert und aktuell: nichts zu ändern.",
        ["Keep my d3d9.dll"] = "Meine d3d9.dll behalten",
        ["Your d3d9.dll stays as it is."] = "Deine d3d9.dll bleibt, wie sie ist.",
        ["Applying changes"] = "Änderungen werden angewendet",
        ["Remove {0}"] = "{0} entfernen",
        ["Fix incompatible settings"] = "Inkompatible Einstellungen korrigieren",
        // suggestions: what Apex Radiance recommends in game (Compat.Suggestions); the game's own texts as the menu translates them
        ["Suggested for Apex Radiance"] = "Für Apex Radiance empfohlen",
        ["Apply"] = "Anwenden",
        ["Applied."] = "Angewendet.",
        ["Apply all suggestions"] = "Alle Vorschläge anwenden",
        ["Apply suggestion: {0}"] = "Vorschlag anwenden: {0}",
        ["Two copies of the mod"] = "Zwei Kopien des Mods",
        ["{0} in Game\\Bin is another copy of Apex Radiance; only one copy runs. Apply moves it to the backup folder."] = "{0} in Game\\Bin ist eine weitere Kopie von Apex Radiance; nur eine Kopie läuft. Anwenden verschiebt sie in den Sicherungsordner.",
        ["Turn off the game's Edge Smoothing"] = "Kantenglättung des Spiels ausschalten",
        ["Options.ini has edgesmoothing = {0}. The Sims 3's Edge Smoothing prevents these enabled Apex effects from working: {1}. Apply sets it to 0 (a copy is kept as Options.ini.apex-backup)."] = "Options.ini hat edgesmoothing = {0}. Die Kantenglättung von Die Sims 3 verhindert, dass diese aktiven Apex-Effekte wirken: {1}. Anwenden setzt sie auf 0 (eine Kopie bleibt als Options.ini.apex-backup).",
        ["Edge Smoothing"] = "Kantenglättung",
        ["Depth Blur"] = "Tiefenunschärfe",
        ["Ambient Occlusion"] = "Umgebungsverdeckung",
        ["Water Reflections"] = "Wasserspiegelungen",
        ["Runs the game on Vulkan: fewer stutters, and Apex Radiance is made and tested with it."] = "Lässt das Spiel über Vulkan laufen: weniger Ruckler, und Apex Radiance wird damit entwickelt und getestet.",
        ["Apex Radiance works on its own, but it pairs well with Sims3SettingsSetter by sims3fiend: a frame rate limiter, fewer stutters and many extra game settings."] = "Apex Radiance funktioniert allein, passt aber gut zu Sims3SettingsSetter von sims3fiend: ein Framerate-Limiter, weniger Ruckler und viele zusätzliche Spieleinstellungen.",
        ["Moves DXVK into its own 64-bit process: the game keeps about 600 MB more free memory for long sessions and big worlds."] = "Verlegt DXVK in einen eigenen 64-Bit-Prozess: das Spiel behält etwa 600 MB mehr freien Speicher für lange Sitzungen und große Welten.",
        ["{0} has {1} = {2}, which Setup cannot read. Setup leaves the file as it is."] = "{0} hat {1} = {2}, was Setup nicht lesen kann. Setup lässt die Datei, wie sie ist.",
        // restore points (Repair or remove)
        ["Install {0}"] = "{0} installieren",
        ["Restore points"] = "Wiederherstellungspunkte",
        ["Setup keeps a copy of every file it changes. Restoring a point undoes that change and every later one."] = "Setup behält eine Kopie jeder Datei, die es ändert. Das Wiederherstellen eines Punkts macht diese Änderung und alle späteren rückgängig.",
        ["Before: {0}"] = "Vor: {0}",
        ["{0} · 1 file"] = "{0} · 1 Datei",
        ["{0} · {1} files"] = "{0} · {1} Dateien",
        ["Restore"] = "Wiederherstellen",
        ["No restore points yet. Setup makes one before every change."] = "Noch keine Wiederherstellungspunkte. Setup erstellt vor jeder Änderung einen.",
        ["Restore this point?"] = "Diesen Punkt wiederherstellen?",
        ["Setup puts back the files as they were before this change, and undoes every change made after it. Close the game first."] = "Setup stellt die Dateien so wieder her, wie sie vor dieser Änderung waren, und macht alle späteren Änderungen rückgängig. Schließe zuerst das Spiel.",
        ["Setup could not put back {0}."] = "Setup konnte {0} nicht wiederherstellen.",
        ["Changes undone."] = "Änderungen rückgängig gemacht.",
        ["Undo these changes"] = "Diese Änderungen rückgängig machen",
        ["This restore point is gone."] = "Dieser Wiederherstellungspunkt existiert nicht mehr.",
        ["Updates available"] = "Updates verfügbar",
        ["Update now"] = "Jetzt aktualisieren",
        ["{0} {1} → {2}"] = "{0} {1} → {2}",
        ["Removes MonoPatcher.asi and its package."] = "Entfernt MonoPatcher.asi und sein Paket.",
        ["Smooth Patch's TS3Patch.asi patches the same game code as Sims3SettingsSetter's Smooth Patch. Setup moves it to the backup folder (S3SS does the same job)."] =
            "TS3Patch.asi von Smooth Patch ändert denselben Spielcode wie der Smooth Patch von Sims3SettingsSetter. Setup verschiebt es in den Sicherungsordner (S3SS erledigt dasselbe).",
        ["simler90's Gameplay Core Mod and NRaas StoryProgression replace the same game code. Keep one of them."] =
            "Gameplay Core Mod von simler90 und NRaas StoryProgression ersetzen denselben Spielcode. Behalte nur einen davon.",
        ["MonoPatcher needs the Microsoft Visual C++ Redistributable (x86); it is not installed."] =
            "MonoPatcher braucht Microsoft Visual C++ Redistributable (x86); es ist nicht installiert.",
        ["Download on Nexus Mods"] = "Auf Nexus Mods herunterladen",
        ["Choose the downloaded file"] = "Heruntergeladene Datei wählen",
        ["Ready to install"] = "Bereit zur Installation",
        ["Waiting for your download in {0}…"] = "Warte auf deinen Download in {0}…",
        ["Not the Performance Patch"] = "Nicht der Performance Patch",
        ["{0} has no Sims3Performance.asi. If the download is a .7z or .rar, extract it and choose Sims3Performance.asi."] =
            "{0} enthält keine Sims3Performance.asi. Ist der Download eine .7z oder .rar, entpacke ihn und wähle Sims3Performance.asi.",
        ["Removes Sims3Performance.asi and its .ini."] = "Entfernt Sims3Performance.asi und seine .ini.",
        ["Download Sims 3 Performance Patch from Nexus Mods first."] =
            "Lade zuerst Sims 3 Performance Patch von Nexus Mods herunter.",
        ["Finds your The Sims 3 (Steam or EA app)."] = "Findet dein The Sims 3 (Steam oder EA app).",
        ["Installs Apex Radiance and the fixes you choose."] =
            "Installiert Apex Radiance und die Korrekturen deiner Wahl.",
        ["Keeps a copy of every file it changes, so you can undo everything."] =
            "Bewahrt eine Kopie jeder geänderten Datei auf, damit du alles rückgängig machen kannst.",
        ["Your Apex Radiance settings are always kept."] =
            "Deine Einstellungen von Apex Radiance bleiben immer erhalten.",
        ["No internet connection"] = "Keine Internetverbindung",
        ["Vulkan, DXVK (recommended)"] = "Vulkan, DXVK (empfohlen)",
        ["Vulkan with more memory (RTX Remix bridge, experimental)"] = "Vulkan mit mehr Speicher (RTX Remix bridge, experimentell)",
        ["The RTX Remix bridge needs a graphics card with Vulkan 1.3 (found: {0})."] = "Die RTX Remix bridge braucht eine Grafikkarte mit Vulkan 1.3 (gefunden: {0}).",
        ["1 update for The Sims 3 · {0}"] = "1 Update für The Sims 3 · {0}",
        ["Change"] = "Ändern",
        ["Close the game before you start. Everything comes from each author's official GitHub, and Setup keeps a copy of every file it changes."] = "Schließe das Spiel, bevor du beginnst. Alles kommt vom offiziellen GitHub der jeweiligen Autoren, und Setup behält eine Kopie jeder Datei, die es ändert.",
        ["Everything is updated"] = "Alles ist aktuell",
        ["Game, components and installation"] = "Spiel, Komponenten und Installation",
        ["How the game draws"] = "Wie das Spiel zeichnet",
        ["Not this one?"] = "Nicht dieses?",
        ["Optional mods"] = "Optionale Mods",
        ["Replaces {0}"] = "Ersetzt {0}",
        ["See what's new"] = "Neuerungen ansehen",
        ["Setup makes a restore point before it changes any file, so you can undo this."] = "Setup erstellt vor jeder Dateiänderung einen Wiederherstellungspunkt, du kannst es also rückgängig machen.",
        ["Something looks wrong in the game? This puts back everything as it was before this update."] = "Sieht im Spiel etwas falsch aus? Das stellt alles so wieder her, wie es vor diesem Update war.",
        ["Step {0} of {1}: {2}"] = "Schritt {0} von {1}: {2}",
        ["The Sims 3 · {0} found"] = "The Sims 3 · {0} gefunden",
        ["Undo this update"] = "Dieses Update rückgängig machen",
        ["Update 1 item"] = "1 Element aktualisieren",
        ["Update {0} items"] = "{0} Elemente aktualisieren",
        ["Use {0}"] = "{0} verwenden",
        ["WELCOME"] = "WILLKOMMEN",
        ["Will be installed"] = "Wird installiert",
        ["Your graphics card: {0}"] = "Deine Grafikkarte: {0}",
        ["{0} · will be removed"] = "{0} · wird entfernt",
        ["{0} → updates to {1}"] = "{0} → wird zu {1}",
        ["{0} updates for The Sims 3 · {1}"] = "{0} Updates für The Sims 3 · {1}",
        ["Windows default (Direct3D 9)"] = "Windows-Standard (Direct3D 9)",
        ["Fewer stutters; Apex Radiance is made and tested with it."] =
            "Weniger Ruckler; Apex Radiance wird damit entwickelt und getestet.",
        ["Keeps about 600 MB more memory free for big worlds. Needs a Vulkan graphics card. If the game misbehaves with it, choose DXVK."] =
            "Hält etwa 600 MB mehr Speicher für große Welten frei. Braucht eine Grafikkarte mit Vulkan. Läuft das Spiel damit nicht rund, wähle DXVK.",
        ["Works, but Apex Radiance is tested with DXVK."] = "Funktioniert, aber Apex Radiance wird mit DXVK getestet.",
        ["Your in-game graphics settings may reset once. Setup keeps a copy and puts it back if you switch back."] =
            "Deine Grafikeinstellungen im Spiel werden eventuell einmal zurückgesetzt. Setup bewahrt eine Kopie auf und stellt sie wieder her, wenn du zurückwechselst.",
        ["Fixes for game and graphics settings."] = "Korrekturen für Spiel- und Grafikeinstellungen.",
        ["Fixes stutters, Sims getting stuck and crashes."] = "Behebt Ruckler, hängende Sims und Abstürze.",
        ["Needed only by some script mods; turn it on if a mod asks for it."] =
            "Nur für manche Script-Mods nötig; schalte es ein, wenn eine Mod es verlangt.",
        ["Makes the game run faster. Needs a free Nexus Mods account; Setup installs it when your download finishes."] =
            "Lässt das Spiel schneller laufen. Braucht ein kostenloses Konto bei Nexus Mods; Setup installiert es, sobald dein Download fertig ist.",
        ["Puts back the graphics files you had before."] =
            "Stellt die Grafikdateien wieder her, die du vorher hattest.",
        ["Goes back to the game's default graphics. Your in-game graphics settings may reset once."] =
            "Kehrt zur Standardgrafik des Spiels zurück. Deine Grafikeinstellungen im Spiel werden eventuell einmal zurückgesetzt.",
        ["Language"] = "Sprache",
        ["Automatic"] = "Automatisch",
        ["This file has no Sims3Performance.asi."] = "Diese Datei enthält keine Sims3Performance.asi.",
        ["Dry run: Setup changes nothing."] = "Testlauf: Setup ändert nichts.",
        ["Apex Radiance by loinyx. The RTX Remix bridge is NVIDIA's, ported to The Sims 3 by dalexlol and alexvixell35-stack. Sims3SettingsSetter is by sims3fiend, Sims 3 Modernization by rodneypantonial and Ultimate ASI Loader by ThirteenAG: Setup downloads them from their own GitHub releases."] =
            "Apex Radiance von loinyx. Die RTX Remix bridge stammt von NVIDIA und wurde von dalexlol und alexvixell35-stack auf The Sims 3 portiert. Sims3SettingsSetter ist von sims3fiend, Sims 3 Modernization von rodneypantonial und Ultimate ASI Loader von ThirteenAG: Setup lädt sie aus ihren eigenen GitHub-Releases herunter.",
        ["RTX Remix bridge: third-party notices"] = "RTX Remix bridge: Hinweise zu Drittanbietern",
        ["Read"] = "Lesen",
        ["Licence text"] = "Lizenztext",
        ["OK"] = "OK",
        ["Cancel"] = "Abbrechen",
    };
}
