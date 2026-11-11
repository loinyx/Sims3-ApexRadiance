// Apex Radiance Setup texts in French (Français). Fill only the entries between the braces, one per line:
//     ["<English key, exactly as in keys.json>"] = "<translation>",
// C# strings: write \" for a quote, \\ for a backslash, \n for a line break; every other character as itself (UTF-8).
// Keep each {0}, {1} of the key, product and file names as they are. A key without an entry shows in English; never
// leave a translation empty. The same key twice: the last one wins, and ApexSetup.Tests fails.
namespace ApexSetup;

public static partial class Strings
{
    static readonly Dictionary<string, string> Fr = new()
    {
        ["Welcome"] = "Accueil",
        ["Game"] = "Jeu",
        ["Components"] = "Composants",
        ["Install"] = "Installer",
        ["Done"] = "Terminé",
        ["Repair or remove"] = "Réparer ou retirer",
        ["About and licences"] = "À propos et licences",
        ["Back"] = "Retour",
        ["Next"] = "Suivant",
        ["Get started"] = "Commencer",
        ["Finish"] = "Terminer",
        ["Close The Sims 3 to continue."] = "Fermez The Sims 3 pour continuer.",
        ["Setup needs the internet to download the files."] =
            "L'installateur a besoin d'internet pour télécharger les fichiers.",
        ["Nothing changes until you click Install."] = "Rien ne change avant que vous cliquiez sur Installer.",
        ["Lighting, picture and performance for The Sims 3."] = "Éclairage, image et performances pour The Sims 3.",
        ["What Setup does"] = "Ce que fait l'installateur",
        ["Setup can't reach GitHub"] = "L'installateur n'arrive pas à joindre GitHub",
        ["Check your internet connection, then try again."] = "Vérifiez votre connexion internet, puis réessayez.",
        ["Try again"] = "Réessayer",
        ["Choose your game"] = "Choisissez votre jeu",
        ["Setup found these installs. Pick the one you play."] =
            "L'installateur a trouvé ces installations. Choisissez celle que vous utilisez.",
        ["Setup found no install. Browse for the game folder."] =
            "Aucune installation trouvée. Cherchez le dossier du jeu.",
        ["Browse for another folder"] = "Choisir un autre dossier",
        ["No game in this folder"] = "Aucun jeu dans ce dossier",
        ["Pick the The Sims 3 folder, or its Game\\Bin folder (the one with TS3W.exe or TS3.exe)."] =
            "Choisissez le dossier de The Sims 3, ou son dossier Game\\Bin (celui qui contient TS3W.exe ou TS3.exe).",
        ["Not installed"] = "Non installé",
        ["Installed"] = "Installé",
        ["the game's own Direct3D 9"] = "le Direct3D 9 du jeu",
        ["another d3d9.dll"] = "un autre d3d9.dll",
        ["Required"] = "Requis",
        ["Installs {0} into Game\\Bin."] = "Installe {0} dans Game\\Bin.",
        ["Replaces the installed copy with {0}. Your settings stay."] =
            "Remplace la copie installée par {0}. Vos paramètres sont conservés.",
        ["Updates {0} to {1}. Your settings stay."] = "Met à jour {0} vers {1}. Vos paramètres sont conservés.",
        ["Recommended with Apex"] = "Recommandé avec Apex",
        ["No updates available"] = "Aucune mise à jour disponible",
        ["Everything in this game is on its latest version."] = "Tout est à jour dans ce jeu.",
        ["Not available for this game version: it supports Steam 1.67.2 and EA app 1.69.47."] =
            "Non disponible pour cette version du jeu : seules Steam 1.67.2 et EA app 1.69.47 sont prises en charge.",
        ["Wrong build"] = "Mauvaise version",
        ["Update {0}"] = "Mettre à jour {0}",
        ["Up to date"] = "À jour",
        ["Setup could not finish"] = "L'installateur n'a pas pu terminer",
        ["Close The Sims 3 first."] = "Fermez d'abord The Sims 3.",
        ["You're all set"] = "Tout est prêt",
        ["Start The Sims 3, load a world and press Ctrl+Shift+F11 to open the Apex Radiance menu."] =
            "Lancez The Sims 3, chargez un monde et appuyez sur Ctrl+Shift+F11 pour ouvrir le menu Apex Radiance.",
        ["Installed: {0} {1}"] = "Installé : {0} {1}",
        ["Choose your game first."] = "Choisissez d'abord votre jeu.",
        ["Removes ApexRadiance.asi. Your settings in Documents stay."] =
            "Retire ApexRadiance.asi. Vos paramètres dans Documents sont conservés.",
        ["Remove"] = "Retirer",
        ["RTX Remix bridge"] = "RTX Remix bridge",
        ["Removes Sims3SettingsSetter.asi."] = "Retire Sims3SettingsSetter.asi.",
        ["Removes ts3native and ts3fixes.package."] = "Retire ts3native et ts3fixes.package.",
        ["Repair or update"] = "Réparer ou mettre à jour",
        ["Downloads and copies again everything installed here. Your settings stay."] =
            "Télécharge et recopie tout ce qui est installé ici. Vos paramètres sont conservés.",
        ["Repair"] = "Réparer",
        ["Uninstall everything"] = "Tout désinstaller",
        ["Removes what Setup installed, and Apex Radiance, and puts back every file Setup replaced."] =
            "Retire ce que l'installateur a installé, ainsi qu'Apex Radiance, et remet chaque fichier qu'il a remplacé.",
        ["Uninstall"] = "Désinstaller",
        ["Remove {0}?"] = "Retirer {0} ?",
        ["Close the game first. Setup removes the files and puts back the ones it replaced."] =
            "Fermez d'abord le jeu. L'installateur retire les fichiers et remet ceux qu'il a remplacés.",
        ["{0} removed."] = "{0} retiré.",
        ["Uninstall everything?"] = "Tout désinstaller ?",
        ["Setup removes Apex Radiance and everything it installed in this game, and puts back every file it replaced. Your settings in Documents stay."] =
            "L'installateur retire Apex Radiance et tout ce qu'il a installé dans ce jeu, et remet chaque fichier qu'il a remplacé. Vos paramètres dans Documents sont conservés.",
        ["Everything Setup installed was removed."] = "Tout ce que l'installateur avait installé a été retiré.",
        ["Setup fixes this"] = "L'installateur corrige ceci",
        ["Read more"] = "En savoir plus",
        ["Fixed."] = "Corrigé.",
        ["No incompatible settings found."] = "Aucun paramètre incompatible trouvé.",
        ["{0} in Game\\Bin loads a second copy of Apex Radiance."] =
            "{0} dans Game\\Bin charge une seconde copie d'Apex Radiance.",
        ["Two ASI loaders ({0}) load every mod twice. Keep one: remove all but {1}."] =
            "Deux chargeurs ASI ({0}) chargent chaque mod deux fois. Gardez-en un : retirez tous sauf {1}.",
        ["Sims 3 Modernization's ts3native is the build for the other edition and does nothing. Turn it on in Components to install the right one."] =
            "Le ts3native de Sims 3 Modernization est la version de l'autre édition et ne fait rien. Activez-le dans Composants pour installer la bonne.",
        ["Sims 3 Modernization has no build for this game version ({0}), so its ts3native does nothing. Turn it off in Components."] =
            "Sims 3 Modernization n'a pas de version pour cette version du jeu ({0}), son ts3native ne fait donc rien. Désactivez-le dans Composants.",
        ["{0} in Game\\Bin is a second copy of {1}; both load. Setup moves it to the backup folder."] =
            "{0} dans Game\\Bin est une seconde copie de {1} ; les deux se chargent. L'installateur le déplace dans le dossier de sauvegarde.",
        ["Mods has {0} .package files but no Resource.cfg, so the game loads none of them."] =
            "Mods contient {0} fichiers .package mais pas de Resource.cfg : le jeu n'en charge aucun.",
        ["{0} is the build for the other edition of the game; it makes the game show \"unofficial game modification\". Use the {1} build."] =
            "{0} est la version pour l'autre édition du jeu ; elle fait afficher \"unofficial game modification\" au jeu. Utilisez la version {1}.",
        ["NRaas ErrorTrap has no build known to work on EA app 1.69.47."] =
            "NRaas ErrorTrap n'a aucune version connue pour fonctionner sur EA app 1.69.47.",
        ["{0} overrides game code Sims 3 Modernization also patches (not tested together)."] =
            "{0} remplace du code du jeu que Sims 3 Modernization modifie aussi (non testés ensemble).",
        ["{0} is an old build with Apex inside; while it is in Game\\Bin, Apex Radiance stays off. Setup moves it to the backup folder (install the official Sims3SettingsSetter in its place if you used it)."] =
            "{0} est une ancienne version qui contient Apex ; tant qu'elle est dans Game\\Bin, Apex Radiance reste désactivé. L'installateur la déplace dans le dossier de sauvegarde (installez à sa place le Sims3SettingsSetter officiel si vous l'utilisiez).",
        ["Sims3Performance's ShortWaitSpin and ThreadStackDiet clash with Sims 3 Modernization. Setup sets both to 0 (a copy is kept as Sims3Performance.ini.apex-backup)."] =
            "ShortWaitSpin et ThreadStackDiet de Sims3Performance entrent en conflit avec Sims 3 Modernization. L'installateur les met tous deux à 0 (une copie est gardée dans Sims3Performance.ini.apex-backup).",
        ["Sims3SettingsSetter's uncompressed textures stutter under the RTX Remix bridge. Setup turns them off (a copy is kept as S3SS.toml.apex-backup)."] =
            "Les textures non compressées de Sims3SettingsSetter saccadent avec le RTX Remix bridge. L'installateur les désactive (une copie est gardée dans S3SS.toml.apex-backup).",
        ["{0} sets client.targetProcess = {1}, but this game is {2}: the bridge does not start."] =
            "{0} définit client.targetProcess = {1}, mais ce jeu est {2} : le bridge ne démarre pas.",
        ["dxvk.conf lacks {0} or has other values: without them the game resets its graphics options or freezes on a resolution change under the bridge."] =
            "Il manque {0} dans dxvk.conf ou les valeurs diffèrent : sans elles, le jeu réinitialise ses options graphiques ou se fige lors d'un changement de résolution avec le bridge.",
        ["Mods\\Resource.cfg does not read {0}, so the game does not load it. Setup adds the Packages folder lines (a copy is kept as Resource.cfg.apex-backup)."] =
            "Mods\\Resource.cfg ne lit pas {0}, le jeu ne le charge donc pas. L'installateur ajoute les lignes du dossier Packages (une copie est gardée dans Resource.cfg.apex-backup).",
        ["Fix settings"] = "Corriger",
        ["Everything is installed and up to date."] = "Tout est installé et à jour.",
        ["Will be removed"] = "Sera retiré",
        ["Keep"] = "Garder",
        ["Nothing to apply"] = "Rien à appliquer",
        ["Preview"] = "Aperçu",
        ["This is a preview: nothing was installed."] = "Ceci est un aperçu : rien n'a été installé.",
        ["Setup is applying changes"] = "L'installateur applique les changements",
        ["Wait until it finishes before closing."] = "Attendez la fin avant de fermer.",
        ["Checking the latest versions…"] = "Recherche des dernières versions…",
        ["Checking for updates…"] = "Recherche de mises à jour…",
        ["Close The Sims 3 to update."] = "Fermez The Sims 3 pour mettre à jour.",
        ["Update now installs them."] = "« Mettre à jour » les installe.",
        ["Graphics"] = "Graphismes",
        ["Checked at {0}."] = "Vérifié à {0}.",
        ["Test build {0}: kept as it is."] = "Version de test {0} : laissée telle quelle.",
        ["Repair or remove sets the game right."] = "« Réparer ou retirer » remet le jeu en état.",
        ["Changes applied"] = "Changements appliqués",
        ["Removed: {0}"] = "Retiré : {0}",
        ["The Sims 3 · {0}"] = "The Sims 3 · {0}",
        ["{0} → {1}"] = "{0} → {1}",
        ["{0} · up to date"] = "{0} · à jour",
        ["Everything is in place."] = "Tout est en place.",
        ["Everything is in place (checked at {0})."] = "Tout est en place (vérifié à {0}).",
        ["Check installation"] = "Vérifier l'installation",
        ["Change components"] = "Modifier les composants",
        ["Apex Radiance is not installed in this game."] = "Apex Radiance n'est pas installé dans ce jeu.",
        ["{0} · test build"] = "{0} · version de test",
        ["Install the official {0}"] = "Installer la version officielle {0}",
        ["No ASI loader in Game\\Bin: Apex Radiance and the other .asi mods do not load."] =
            "Aucun chargeur ASI dans Game\\Bin : Apex Radiance et les autres mods .asi ne se chargent pas.",
        ["{0} is missing: the RTX Remix bridge does not start."] =
            "{0} est absent : le RTX Remix bridge ne démarre pas.",
        ["Apply 1 change"] = "Appliquer 1 changement",
        ["Apply {0} changes"] = "Appliquer {0} changements",
        ["{0} is installed and up to date: nothing to change."] = "{0} est installé et à jour : rien à changer.",
        ["Keep my d3d9.dll"] = "Garder mon d3d9.dll",
        ["Your d3d9.dll stays as it is."] = "Votre d3d9.dll reste tel quel.",
        ["Applying changes"] = "Application des changements",
        ["Remove {0}"] = "Retirer {0}",
        ["Fix incompatible settings"] = "Corriger les paramètres incompatibles",
        // suggestions: what Apex Radiance recommends in game (Compat.Suggestions); the game's own texts as the menu translates them
        ["Suggested for Apex Radiance"] = "Suggéré pour Apex Radiance",
        ["Apply"] = "Appliquer",
        ["Applied."] = "Appliqué.",
        ["Apply all suggestions"] = "Appliquer toutes les suggestions",
        ["Apply suggestion: {0}"] = "Appliquer la suggestion : {0}",
        ["Two copies of the mod"] = "Deux copies du mod",
        ["{0} in Game\\Bin is another copy of Apex Radiance; only one copy runs. Apply moves it to the backup folder."] = "{0} dans Game\\Bin est une autre copie d'Apex Radiance ; une seule copie fonctionne. Appliquer la déplace dans le dossier de sauvegarde.",
        ["Turn off the game's Edge Smoothing"] = "Désactivez le lissage des bords du jeu",
        ["Options.ini has edgesmoothing = {0}. The Sims 3's Edge Smoothing prevents these enabled Apex effects from working: {1}. Apply sets it to 0 (a copy is kept as Options.ini.apex-backup)."] = "Options.ini contient edgesmoothing = {0}. Le lissage des bords des Sims 3 empêche ces effets Apex activés de fonctionner : {1}. Appliquer le met à 0 (une copie est gardée dans Options.ini.apex-backup).",
        ["Edge Smoothing"] = "Lissage des Bords",
        ["Depth Blur"] = "Flou de Profondeur",
        ["Ambient Occlusion"] = "Occlusion Ambiante",
        ["Water Reflections"] = "Reflets dans l'Eau",
        ["Runs the game on Vulkan: fewer stutters, and Apex Radiance is made and tested with it."] = "Fait tourner le jeu sur Vulkan : moins d'à-coups, et Apex Radiance est conçu et testé avec.",
        ["Apex Radiance works on its own, but it pairs well with Sims3SettingsSetter by sims3fiend: a frame rate limiter, fewer stutters and many extra game settings."] = "Apex Radiance fonctionne seul, mais s'associe bien à Sims3SettingsSetter de sims3fiend : limiteur d'images par seconde, moins de saccades et de nombreux réglages de jeu en plus.",
        ["Moves DXVK into its own 64-bit process: the game keeps about 600 MB more free memory for long sessions and big worlds."] = "Place DXVK dans son propre processus 64 bits : le jeu garde environ 600 Mo de mémoire libre en plus pour les longues sessions et les grands mondes.",
        ["{0} has {1} = {2}, which Setup cannot read. Setup leaves the file as it is."] = "{0} contient {1} = {2}, que l'installateur ne sait pas lire. L'installateur laisse le fichier tel quel.",
        // restore points (Repair or remove)
        ["Install {0}"] = "Installer {0}",
        ["Restore points"] = "Points de restauration",
        ["Setup keeps a copy of every file it changes. Restoring a point undoes that change and every later one."] = "L'installateur garde une copie de chaque fichier qu'il modifie. Restaurer un point annule cette modification et toutes les suivantes.",
        ["Before: {0}"] = "Avant : {0}",
        ["{0} · 1 file"] = "{0} · 1 fichier",
        ["{0} · {1} files"] = "{0} · {1} fichiers",
        ["Restore"] = "Restaurer",
        ["No restore points yet. Setup makes one before every change."] = "Aucun point de restauration pour l'instant. L'installateur en crée un avant chaque modification.",
        ["Restore this point?"] = "Restaurer ce point ?",
        ["Setup puts back the files as they were before this change, and undoes every change made after it. Close the game first."] = "L'installateur remet les fichiers tels qu'ils étaient avant cette modification et annule toutes les modifications faites après. Fermez d'abord le jeu.",
        ["Setup could not put back {0}."] = "L'installateur n'a pas pu remettre {0}.",
        ["Changes undone."] = "Modifications annulées.",
        ["Undo these changes"] = "Annuler ces modifications",
        ["This restore point is gone."] = "Ce point de restauration n'existe plus.",
        ["Updates available"] = "Mises à jour disponibles",
        ["Update now"] = "Mettre à jour",
        ["{0} {1} → {2}"] = "{0} {1} → {2}",
        ["Removes MonoPatcher.asi and its package."] = "Retire MonoPatcher.asi et son package.",
        ["Smooth Patch's TS3Patch.asi patches the same game code as Sims3SettingsSetter's Smooth Patch. Setup moves it to the backup folder (S3SS does the same job)."] =
            "Le TS3Patch.asi de Smooth Patch modifie le même code du jeu que le Smooth Patch de Sims3SettingsSetter. L'installateur le déplace dans le dossier de sauvegarde (S3SS fait le même travail).",
        ["simler90's Gameplay Core Mod and NRaas StoryProgression replace the same game code. Keep one of them."] =
            "Le Gameplay Core Mod de simler90 et NRaas StoryProgression remplacent le même code du jeu. N'en gardez qu'un.",
        ["MonoPatcher needs the Microsoft Visual C++ Redistributable (x86); it is not installed."] =
            "MonoPatcher a besoin du Microsoft Visual C++ Redistributable (x86), qui n'est pas installé.",
        ["Download on Nexus Mods"] = "Télécharger sur Nexus Mods",
        ["Choose the downloaded file"] = "Choisir le fichier téléchargé",
        ["Ready to install"] = "Prêt à installer",
        ["Waiting for your download in {0}…"] = "En attente de votre téléchargement dans {0}…",
        ["Not the Performance Patch"] = "Ce n'est pas le Performance Patch",
        ["{0} has no Sims3Performance.asi. If the download is a .7z or .rar, extract it and choose Sims3Performance.asi."] =
            "{0} ne contient pas Sims3Performance.asi. Si le téléchargement est un .7z ou un .rar, extrayez-le et choisissez Sims3Performance.asi.",
        ["Removes Sims3Performance.asi and its .ini."] = "Retire Sims3Performance.asi et son .ini.",
        ["Download Sims 3 Performance Patch from Nexus Mods first."] =
            "Téléchargez d'abord Sims 3 Performance Patch sur Nexus Mods.",
        ["Finds your The Sims 3 (Steam or EA app)."] = "Trouve votre The Sims 3 (Steam ou EA app).",
        ["Installs Apex Radiance and the fixes you choose."] =
            "Installe Apex Radiance et les corrections de votre choix.",
        ["Keeps a copy of every file it changes, so you can undo everything."] =
            "Garde une copie de chaque fichier modifié, pour que vous puissiez tout annuler.",
        ["Your Apex Radiance settings are always kept."] = "Vos paramètres Apex Radiance sont toujours conservés.",
        ["No internet connection"] = "Pas de connexion internet",
        ["Vulkan, DXVK (recommended)"] = "Vulkan, DXVK (recommandé)",
        ["Vulkan with more memory (RTX Remix bridge, experimental)"] = "Vulkan avec plus de mémoire (RTX Remix bridge, expérimental)",
        ["The RTX Remix bridge needs a graphics card with Vulkan 1.3 (found: {0})."] = "Le RTX Remix bridge nécessite une carte graphique compatible Vulkan 1.3 (trouvée : {0}).",
        ["Windows default (Direct3D 9)"] = "Par défaut de Windows (Direct3D 9)",
        ["Fewer stutters; Apex Radiance is made and tested with it."] =
            "Moins de saccades ; Apex Radiance est conçu et testé avec.",
        ["Keeps about 600 MB more memory free for big worlds. Needs a Vulkan graphics card. If the game misbehaves with it, choose DXVK."] =
            "Garde environ 600 Mo de mémoire libre en plus pour les grands mondes. Nécessite une carte graphique compatible Vulkan. Si le jeu fonctionne mal avec, choisissez DXVK.",
        ["Works, but Apex Radiance is tested with DXVK."] = "Fonctionne, mais Apex Radiance est testé avec DXVK.",
        ["Your in-game graphics settings may reset once. Setup keeps a copy and puts it back if you switch back."] =
            "Vos paramètres graphiques en jeu peuvent être réinitialisés une fois. L'installateur en garde une copie et la remet si vous revenez en arrière.",
        ["Fixes for game and graphics settings."] = "Corrections des paramètres du jeu et des graphismes.",
        ["Fixes stutters, Sims getting stuck and crashes."] =
            "Corrige les saccades, les Sims bloqués et les plantages.",
        ["Needed only by some script mods; turn it on if a mod asks for it."] =
            "Nécessaire seulement pour certains mods de script ; activez-le si un mod le demande.",
        ["Makes the game run faster. Needs a free Nexus Mods account; Setup installs it when your download finishes."] =
            "Rend le jeu plus rapide. Nécessite un compte Nexus Mods gratuit ; l'installateur l'installe une fois votre téléchargement terminé.",
        ["Puts back the graphics files you had before."] = "Remet les fichiers graphiques que vous aviez avant.",
        ["Goes back to the game's default graphics. Your in-game graphics settings may reset once."] =
            "Revient aux graphismes par défaut du jeu. Vos paramètres graphiques en jeu peuvent être réinitialisés une fois.",
        ["Language"] = "Langue",
        ["Automatic"] = "Automatique",
        ["This file has no Sims3Performance.asi."] = "Ce fichier ne contient pas Sims3Performance.asi.",
        ["Dry run: Setup changes nothing."] = "Test : l'installateur ne change rien.",
        ["Apex Radiance by loinyx. The RTX Remix bridge is NVIDIA's, ported to The Sims 3 by dalexlol and alexvixell35-stack. Sims3SettingsSetter is by sims3fiend, Sims 3 Modernization by rodneypantonial and Ultimate ASI Loader by ThirteenAG: Setup downloads them from their own GitHub releases."] =
            "Apex Radiance par loinyx. Le RTX Remix bridge est de NVIDIA, porté sur The Sims 3 par dalexlol et alexvixell35-stack. Sims3SettingsSetter est de sims3fiend, Sims 3 Modernization de rodneypantonial et Ultimate ASI Loader de ThirteenAG : l'installateur les télécharge depuis leurs propres versions sur GitHub.",
        ["RTX Remix bridge: third-party notices"] = "RTX Remix bridge : mentions tierces",
        ["Read"] = "Lire",
        ["Licence text"] = "Texte de la licence",
        ["OK"] = "OK",
        ["Cancel"] = "Annuler",
    };
}
