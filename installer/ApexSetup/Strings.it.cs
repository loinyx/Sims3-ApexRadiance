// Apex Radiance Setup texts in Italian (Italiano). Fill only the entries between the braces, one per line:
//     ["<English key, exactly as in keys.json>"] = "<translation>",
// C# strings: write \" for a quote, \\ for a backslash, \n for a line break; every other character as itself (UTF-8).
// Keep each {0}, {1} of the key, product and file names as they are. A key without an entry shows in English; never
// leave a translation empty. The same key twice: the last one wins, and ApexSetup.Tests fails.
namespace ApexSetup;

public static partial class Strings
{
    static readonly Dictionary<string, string> It = new()
    {
        ["Welcome"] = "Benvenuto",
        ["Game"] = "Gioco",
        ["Components"] = "Componenti",
        ["Install"] = "Installa",
        ["Done"] = "Completato",
        ["Repair or remove"] = "Ripara o rimuovi",
        ["About and licences"] = "Info e licenze",
        ["Back"] = "Indietro",
        ["Next"] = "Avanti",
        ["Get started"] = "Inizia",
        ["Finish"] = "Fine",
        ["Close The Sims 3 to continue."] = "Chiudi The Sims 3 per continuare.",
        ["Setup needs the internet to download the files."] =
            "Il programma di installazione ha bisogno di Internet per scaricare i file.",
        ["Nothing changes until you click Install."] = "Non cambia nulla finché non fai clic su Installa.",
        ["Lighting, picture and performance for The Sims 3."] = "Luce, immagine e prestazioni per The Sims 3.",
        ["What Setup does"] = "Cosa fa il programma di installazione",
        ["Setup can't reach GitHub"] = "Il programma di installazione non riesce a raggiungere GitHub",
        ["Check your internet connection, then try again."] = "Controlla la connessione a Internet e riprova.",
        ["Try again"] = "Riprova",
        ["Choose your game"] = "Scegli il tuo gioco",
        ["Setup found these installs. Pick the one you play."] =
            "Sono state trovate queste installazioni. Scegli quella con cui giochi.",
        ["Setup found no install. Browse for the game folder."] =
            "Nessuna installazione trovata. Cerca la cartella del gioco.",
        ["Browse for another folder"] = "Cerca un'altra cartella",
        ["No game in this folder"] = "Nessun gioco in questa cartella",
        ["Pick the The Sims 3 folder, or its Game\\Bin folder (the one with TS3W.exe or TS3.exe)."] =
            "Scegli la cartella di The Sims 3 o la sua cartella Game\\Bin (quella con TS3W.exe o TS3.exe).",
        ["Not installed"] = "Non installato",
        ["Installed"] = "Installato",
        ["the game's own Direct3D 9"] = "il Direct3D 9 del gioco",
        ["another d3d9.dll"] = "un'altra d3d9.dll",
        ["Required"] = "Obbligatorio",
        ["Installs {0} into Game\\Bin."] = "Installa {0} in Game\\Bin.",
        ["Replaces the installed copy with {0}. Your settings stay."] =
            "Sostituisce la copia installata con {0}. Le tue impostazioni restano.",
        ["Updates {0} to {1}. Your settings stay."] = "Aggiorna da {0} a {1}. Le tue impostazioni restano.",
        ["Recommended with Apex"] = "Consigliati con Apex",
        ["No updates available"] = "Nessun aggiornamento disponibile",
        ["Everything in this game is on its latest version."] = "Tutto in questo gioco è all'ultima versione.",
        ["Not available for this game version: it supports Steam 1.67.2 and EA app 1.69.47."] =
            "Non disponibile per questa versione del gioco: supporta Steam 1.67.2 e EA app 1.69.47.",
        ["Wrong build"] = "Versione errata",
        ["Update {0}"] = "Aggiorna {0}",
        ["Up to date"] = "Aggiornato",
        ["Setup could not finish"] = "Il programma di installazione non è riuscito a finire",
        ["Close The Sims 3 first."] = "Prima chiudi The Sims 3.",
        ["You're all set"] = "Tutto pronto",
        ["Start The Sims 3, load a world and press Ctrl+Shift+F11 to open the Apex Radiance menu."] =
            "Avvia The Sims 3, carica un mondo e premi Ctrl+Shift+F11 per aprire il menu di Apex Radiance.",
        ["Installed: {0} {1}"] = "Installato: {0} {1}",
        ["Choose your game first."] = "Prima scegli il tuo gioco.",
        ["Removes ApexRadiance.asi. Your settings in Documents stay."] =
            "Rimuove ApexRadiance.asi. Le tue impostazioni in Documenti restano.",
        ["Remove"] = "Rimuovi",
        ["RTX Remix bridge"] = "RTX Remix bridge",
        ["Removes Sims3SettingsSetter.asi."] = "Rimuove Sims3SettingsSetter.asi.",
        ["Removes ts3native and ts3fixes.package."] = "Rimuove ts3native e ts3fixes.package.",
        ["Repair or update"] = "Ripara o aggiorna",
        ["Downloads and copies again everything installed here. Your settings stay."] =
            "Scarica e copia di nuovo tutto ciò che è installato qui. Le tue impostazioni restano.",
        ["Repair"] = "Ripara",
        ["Uninstall everything"] = "Disinstalla tutto",
        ["Removes what Setup installed, and Apex Radiance, and puts back every file Setup replaced."] =
            "Rimuove Apex Radiance e ciò che ha installato il programma di installazione, e rimette ogni file che ha sostituito.",
        ["Uninstall"] = "Disinstalla",
        ["Remove {0}?"] = "Rimuovere {0}?",
        ["Close the game first. Setup removes the files and puts back the ones it replaced."] =
            "Prima chiudi il gioco. Il programma di installazione rimuove i file e rimette quelli che ha sostituito.",
        ["{0} removed."] = "{0} rimosso.",
        ["Uninstall everything?"] = "Disinstallare tutto?",
        ["Setup removes Apex Radiance and everything it installed in this game, and puts back every file it replaced. Your settings in Documents stay."] =
            "Il programma di installazione rimuove Apex Radiance e tutto ciò che ha installato in questo gioco, e rimette ogni file che ha sostituito. Le tue impostazioni in Documenti restano.",
        ["Everything Setup installed was removed."] =
            "Tutto ciò che il programma di installazione aveva installato è stato rimosso.",
        ["Setup fixes this"] = "Il programma di installazione lo corregge",
        ["Read more"] = "Ulteriori info",
        ["Fixed."] = "Corretto.",
        ["No incompatible settings found."] = "Nessuna impostazione incompatibile trovata.",
        ["{0} in Game\\Bin loads a second copy of Apex Radiance."] =
            "{0} in Game\\Bin carica una seconda copia di Apex Radiance.",
        ["Two ASI loaders ({0}) load every mod twice. Keep one: remove all but {1}."] =
            "Due ASI loader ({0}) caricano ogni mod due volte. Tienine uno: rimuovi tutti tranne {1}.",
        ["Sims 3 Modernization's ts3native is the build for the other edition and does nothing. Turn it on in Components to install the right one."] =
            "Il ts3native di Sims 3 Modernization è la versione per l'altra edizione e non fa nulla. Attivalo in Componenti per installare quella giusta.",
        ["Sims 3 Modernization has no build for this game version ({0}), so its ts3native does nothing. Turn it off in Components."] =
            "Sims 3 Modernization non ha una versione per questa versione del gioco ({0}), quindi il suo ts3native non fa nulla. Disattivalo in Componenti.",
        ["{0} in Game\\Bin is a second copy of {1}; both load. Setup moves it to the backup folder."] =
            "{0} in Game\\Bin è una seconda copia di {1}; vengono caricate entrambe. Il programma di installazione la sposta nella cartella di backup.",
        ["Mods has {0} .package files but no Resource.cfg, so the game loads none of them."] =
            "Mods contiene {0} file .package ma nessun Resource.cfg, quindi il gioco non ne carica nessuno.",
        ["{0} is the build for the other edition of the game; it makes the game show \"unofficial game modification\". Use the {1} build."] =
            "{0} è la versione per l'altra edizione del gioco; fa mostrare al gioco \"unofficial game modification\". Usa la versione {1}.",
        ["NRaas ErrorTrap has no build known to work on EA app 1.69.47."] =
            "NRaas ErrorTrap non ha una versione nota che funzioni su EA app 1.69.47.",
        ["{0} overrides game code Sims 3 Modernization also patches (not tested together)."] =
            "{0} sostituisce codice del gioco che anche Sims 3 Modernization modifica (non testati insieme).",
        ["{0} is an old build with Apex inside; while it is in Game\\Bin, Apex Radiance stays off. Setup moves it to the backup folder (install the official Sims3SettingsSetter in its place if you used it)."] =
            "{0} è una vecchia versione con Apex incluso; finché è in Game\\Bin, Apex Radiance resta disattivato. Il programma di installazione la sposta nella cartella di backup (se lo usavi, installa al suo posto il Sims3SettingsSetter ufficiale).",
        ["Sims3Performance's ShortWaitSpin and ThreadStackDiet clash with Sims 3 Modernization. Setup sets both to 0 (a copy is kept as Sims3Performance.ini.apex-backup)."] =
            "ShortWaitSpin e ThreadStackDiet di Sims3Performance sono in conflitto con Sims 3 Modernization. Il programma di installazione li imposta entrambi a 0 (una copia resta in Sims3Performance.ini.apex-backup).",
        ["Sims3SettingsSetter's uncompressed textures stutter under the RTX Remix bridge. Setup turns them off (a copy is kept as S3SS.toml.apex-backup)."] =
            "Le texture non compresse di Sims3SettingsSetter fanno scattare il gioco con l'RTX Remix bridge. Il programma di installazione le disattiva (una copia resta in S3SS.toml.apex-backup).",
        ["{0} sets client.targetProcess = {1}, but this game is {2}: the bridge does not start."] =
            "{0} imposta client.targetProcess = {1}, ma questo gioco è {2}: il bridge non si avvia.",
        ["dxvk.conf lacks {0} or has other values: without them the game resets its graphics options or freezes on a resolution change under the bridge."] =
            "In dxvk.conf manca {0} o ha altri valori: senza, il gioco reimposta le opzioni grafiche o si blocca quando cambi risoluzione con il bridge.",
        ["Mods\\Resource.cfg does not read {0}, so the game does not load it. Setup adds the Packages folder lines (a copy is kept as Resource.cfg.apex-backup)."] =
            "Mods\\Resource.cfg non legge {0}, quindi il gioco non lo carica. Il programma di installazione aggiunge le righe della cartella Packages (una copia resta in Resource.cfg.apex-backup).",
        ["Fix settings"] = "Correggi impostazioni",
        ["Everything is installed and up to date."] = "Tutto è installato e aggiornato.",
        ["Will be removed"] = "Verrà rimosso",
        ["Keep"] = "Mantieni",
        ["Nothing to apply"] = "Niente da applicare",
        ["Preview"] = "Anteprima",
        ["This is a preview: nothing was installed."] = "Questa è un'anteprima: non è stato installato nulla.",
        ["Setup is applying changes"] = "Il programma di installazione sta applicando le modifiche",
        ["Wait until it finishes before closing."] = "Attendi che finisca prima di chiudere.",
        ["Checking the latest versions…"] = "Controllo delle ultime versioni…",
        ["Checking for updates…"] = "Ricerca aggiornamenti…",
        ["Close The Sims 3 to update."] = "Chiudi The Sims 3 per aggiornare.",
        ["Update now installs them."] = "«Aggiorna ora» li installa.",
        ["Graphics"] = "Grafica",
        ["Checked at {0}."] = "Controllato alle {0}.",
        ["Test build {0}: kept as it is."] = "Versione di prova {0}: resta così com'è.",
        ["Repair or remove sets the game right."] = "«Ripara o rimuovi» rimette a posto il gioco.",
        ["Changes applied"] = "Modifiche applicate",
        ["Removed: {0}"] = "Rimosso: {0}",
        ["The Sims 3 · {0}"] = "The Sims 3 · {0}",
        ["{0} → {1}"] = "{0} → {1}",
        ["{0} · up to date"] = "{0} · aggiornato",
        ["Everything is in place."] = "È tutto a posto.",
        ["Everything is in place (checked at {0})."] = "È tutto a posto (controllato alle {0}).",
        ["Check installation"] = "Verifica installazione",
        ["Change components"] = "Cambia componenti",
        ["Apex Radiance is not installed in this game."] = "Apex Radiance non è installato in questo gioco.",
        ["{0} · test build"] = "{0} · versione di prova",
        ["Install the official {0}"] = "Installa la versione ufficiale {0}",
        ["No ASI loader in Game\\Bin: Apex Radiance and the other .asi mods do not load."] =
            "Nessun ASI loader in Game\\Bin: Apex Radiance e le altre mod .asi non si caricano.",
        ["{0} is missing: the RTX Remix bridge does not start."] = "Manca {0}: l'RTX Remix bridge non si avvia.",
        ["Apply 1 change"] = "Applica 1 modifica",
        ["Apply {0} changes"] = "Applica {0} modifiche",
        ["{0} is installed and up to date: nothing to change."] = "{0} è installato e aggiornato: niente da cambiare.",
        ["Keep my d3d9.dll"] = "Mantieni la mia d3d9.dll",
        ["Your d3d9.dll stays as it is."] = "La tua d3d9.dll resta così com'è.",
        ["Applying changes"] = "Applicazione delle modifiche",
        ["Remove {0}"] = "Rimuovi {0}",
        ["Fix incompatible settings"] = "Correggi impostazioni incompatibili",
        // suggestions: what Apex Radiance recommends in game (Compat.Suggestions); the game's own texts as the menu translates them
        ["Suggested for Apex Radiance"] = "Consigliato per Apex Radiance",
        ["Apply"] = "Applica",
        ["Applied."] = "Applicato.",
        ["Apply all suggestions"] = "Applica tutti i suggerimenti",
        ["Apply suggestion: {0}"] = "Applica suggerimento: {0}",
        ["Two copies of the mod"] = "Due copie della mod",
        ["{0} in Game\\Bin is another copy of Apex Radiance; only one copy runs. Apply moves it to the backup folder."] = "{0} in Game\\Bin è un'altra copia di Apex Radiance; ne funziona solo una. Applica la sposta nella cartella di backup.",
        ["Turn off the game's Edge Smoothing"] = "Disattiva lo smussamento bordi del gioco",
        ["Options.ini has edgesmoothing = {0}. The Sims 3's Edge Smoothing prevents these enabled Apex effects from working: {1}. Apply sets it to 0 (a copy is kept as Options.ini.apex-backup)."] = "Options.ini ha edgesmoothing = {0}. Lo smussamento bordi di The Sims 3 impedisce a questi effetti attivi di Apex di funzionare: {1}. Applica lo imposta a 0 (una copia resta in Options.ini.apex-backup).",
        ["Edge Smoothing"] = "Smussamento bordi",
        ["Depth Blur"] = "Sfocatura di profondità",
        ["Ambient Occlusion"] = "Occlusione ambientale",
        ["Water Reflections"] = "Riflessi sull'acqua",
        ["Runs the game on Vulkan: fewer stutters, and Apex Radiance is made and tested with it."] = "Fa girare il gioco su Vulkan: meno scatti, e Apex Radiance è creata e testata con esso.",
        ["Apex Radiance works on its own, but it pairs well with Sims3SettingsSetter by sims3fiend: a frame rate limiter, fewer stutters and many extra game settings."] = "Apex Radiance funziona da sola, ma si abbina bene a Sims3SettingsSetter di sims3fiend: limitatore di frame rate, meno scatti e molte impostazioni di gioco in più.",
        ["Moves DXVK into its own 64-bit process: the game keeps about 600 MB more free memory for long sessions and big worlds."] = "Sposta DXVK in un proprio processo a 64 bit: il gioco conserva circa 600 MB di memoria libera in più per sessioni lunghe e mondi grandi.",
        ["{0} has {1} = {2}, which Setup cannot read. Setup leaves the file as it is."] = "{0} ha {1} = {2}, che il programma di installazione non sa leggere. Il programma di installazione lascia il file com'è.",
        // restore points (Repair or remove)
        ["Install {0}"] = "Installa {0}",
        ["Restore points"] = "Punti di ripristino",
        ["Setup keeps a copy of every file it changes. Restoring a point undoes that change and every later one."] = "Il programma di installazione conserva una copia di ogni file che modifica. Ripristinare un punto annulla quella modifica e tutte le successive.",
        ["Before: {0}"] = "Prima di: {0}",
        ["{0} · 1 file"] = "{0} · 1 file",
        ["{0} · {1} files"] = "{0} · {1} file",
        ["Restore"] = "Ripristina",
        ["No restore points yet. Setup makes one before every change."] = "Ancora nessun punto di ripristino. Il programma di installazione ne crea uno prima di ogni modifica.",
        ["Restore this point?"] = "Ripristinare questo punto?",
        ["Setup puts back the files as they were before this change, and undoes every change made after it. Close the game first."] = "Il programma di installazione rimette i file com'erano prima di questa modifica e annulla tutte le modifiche fatte dopo. Prima chiudi il gioco.",
        ["Setup could not put back {0}."] = "Il programma di installazione non è riuscito a rimettere {0}.",
        ["Changes undone."] = "Modifiche annullate.",
        ["Undo these changes"] = "Annulla queste modifiche",
        ["This restore point is gone."] = "Questo punto di ripristino non esiste più.",
        ["Updates available"] = "Aggiornamenti disponibili",
        ["Update now"] = "Aggiorna ora",
        ["{0} {1} → {2}"] = "{0} {1} → {2}",
        ["Removes MonoPatcher.asi and its package."] = "Rimuove MonoPatcher.asi e il suo pacchetto.",
        ["Smooth Patch's TS3Patch.asi patches the same game code as Sims3SettingsSetter's Smooth Patch. Setup moves it to the backup folder (S3SS does the same job)."] =
            "Il TS3Patch.asi di Smooth Patch modifica lo stesso codice del gioco dello Smooth Patch di Sims3SettingsSetter. Il programma di installazione lo sposta nella cartella di backup (S3SS fa lo stesso lavoro).",
        ["simler90's Gameplay Core Mod and NRaas StoryProgression replace the same game code. Keep one of them."] =
            "Gameplay Core Mod di simler90 e NRaas StoryProgression sostituiscono lo stesso codice del gioco. Tienine solo uno.",
        ["MonoPatcher needs the Microsoft Visual C++ Redistributable (x86); it is not installed."] =
            "MonoPatcher richiede Microsoft Visual C++ Redistributable (x86), che non è installato.",
        ["Download on Nexus Mods"] = "Scarica da Nexus Mods",
        ["Choose the downloaded file"] = "Scegli il file scaricato",
        ["Ready to install"] = "Pronto per l'installazione",
        ["Waiting for your download in {0}…"] = "In attesa del download in {0}…",
        ["Not the Performance Patch"] = "Non è il Performance Patch",
        ["{0} has no Sims3Performance.asi. If the download is a .7z or .rar, extract it and choose Sims3Performance.asi."] =
            "{0} non contiene Sims3Performance.asi. Se il download è un .7z o un .rar, estrailo e scegli Sims3Performance.asi.",
        ["Removes Sims3Performance.asi and its .ini."] = "Rimuove Sims3Performance.asi e il suo .ini.",
        ["Download Sims 3 Performance Patch from Nexus Mods first."] =
            "Prima scarica Sims 3 Performance Patch da Nexus Mods.",
        ["Finds your The Sims 3 (Steam or EA app)."] = "Trova il tuo The Sims 3 (Steam o EA app).",
        ["Installs Apex Radiance and the fixes you choose."] = "Installa Apex Radiance e le correzioni che scegli.",
        ["Keeps a copy of every file it changes, so you can undo everything."] =
            "Tiene una copia di ogni file che modifica, così puoi annullare tutto.",
        ["Your Apex Radiance settings are always kept."] = "Le impostazioni di Apex Radiance vengono sempre mantenute.",
        ["No internet connection"] = "Nessuna connessione a Internet",
        ["Vulkan, DXVK (recommended)"] = "Vulkan, DXVK (consigliato)",
        ["Vulkan with more memory (RTX Remix bridge, experimental)"] = "Vulkan con più memoria (RTX Remix bridge, sperimentale)",
        ["The RTX Remix bridge needs a graphics card with Vulkan 1.3 (found: {0})."] = "L'RTX Remix bridge richiede una scheda video con Vulkan 1.3 (trovata: {0}).",
        ["Windows default (Direct3D 9)"] = "Predefinito di Windows (Direct3D 9)",
        ["Fewer stutters; Apex Radiance is made and tested with it."] =
            "Meno scatti; Apex Radiance è creato e testato con questo.",
        ["Keeps about 600 MB more memory free for big worlds. Needs a Vulkan graphics card. If the game misbehaves with it, choose DXVK."] =
            "Lascia liberi circa 600 MB di memoria in più per i mondi grandi. Richiede una scheda video con Vulkan. Se il gioco dà problemi, scegli DXVK.",
        ["Works, but Apex Radiance is tested with DXVK."] = "Funziona, ma Apex Radiance è testato con DXVK.",
        ["Your in-game graphics settings may reset once. Setup keeps a copy and puts it back if you switch back."] =
            "Le impostazioni grafiche del gioco potrebbero tornare ai predefiniti una volta. Il programma di installazione ne tiene una copia e la rimette se torni indietro.",
        ["Fixes for game and graphics settings."] = "Correzioni per le impostazioni del gioco e della grafica.",
        ["Fixes stutters, Sims getting stuck and crashes."] = "Corregge scatti, Sim bloccati e crash.",
        ["Needed only by some script mods; turn it on if a mod asks for it."] =
            "Serve solo ad alcune mod di script; attivalo se una mod lo richiede.",
        ["Makes the game run faster. Needs a free Nexus Mods account; Setup installs it when your download finishes."] =
            "Rende il gioco più veloce. Richiede un account gratuito su Nexus Mods; il programma di installazione lo installa al termine del download.",
        ["Puts back the graphics files you had before."] = "Rimette i file grafici che avevi prima.",
        ["Goes back to the game's default graphics. Your in-game graphics settings may reset once."] =
            "Torna alla grafica predefinita del gioco. Le impostazioni grafiche del gioco potrebbero tornare ai predefiniti una volta.",
        ["Language"] = "Lingua",
        ["Automatic"] = "Automatica",
        ["This file has no Sims3Performance.asi."] = "Questo file non contiene Sims3Performance.asi.",
        ["Dry run: Setup changes nothing."] = "Prova: il programma di installazione non cambia nulla.",
        ["Apex Radiance by loinyx. The RTX Remix bridge is NVIDIA's, ported to The Sims 3 by dalexlol and alexvixell35-stack. Sims3SettingsSetter is by sims3fiend, Sims 3 Modernization by rodneypantonial and Ultimate ASI Loader by ThirteenAG: Setup downloads them from their own GitHub releases."] =
            "Apex Radiance di loinyx. L'RTX Remix bridge è di NVIDIA, portato su The Sims 3 da dalexlol e alexvixell35-stack. Sims3SettingsSetter è di sims3fiend, Sims 3 Modernization di rodneypantonial e Ultimate ASI Loader di ThirteenAG: il programma di installazione li scarica dalle loro release su GitHub.",
        ["RTX Remix bridge: third-party notices"] = "RTX Remix bridge: avvisi di terze parti",
        ["Read"] = "Leggi",
        ["Licence text"] = "Testo della licenza",
        ["OK"] = "OK",
        ["Cancel"] = "Annulla",
    };
}
