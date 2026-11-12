// Apex Radiance Setup texts in Spanish (Español). Fill only the entries between the braces, one per line:
//     ["<English key, exactly as in keys.json>"] = "<translation>",
// C# strings: write \" for a quote, \\ for a backslash, \n for a line break; every other character as itself (UTF-8).
// Keep each {0}, {1} of the key, product and file names as they are. A key without an entry shows in English; never
// leave a translation empty. The same key twice: the last one wins, and ApexSetup.Tests fails.
namespace ApexSetup;

public static partial class Strings
{
    static readonly Dictionary<string, string> Es = new()
    {
        ["Welcome"] = "Inicio",
        ["Game"] = "Juego",
        ["Components"] = "Componentes",
        ["Install"] = "Instalar",
        ["Done"] = "Listo",
        ["Repair or remove"] = "Reparar o quitar",
        ["About and licences"] = "Acerca de y licencias",
        ["Back"] = "Atrás",
        ["Next"] = "Siguiente",
        ["Get started"] = "Empezar",
        ["Finish"] = "Finalizar",
        ["Close The Sims 3 to continue."] = "Cierra The Sims 3 para continuar.",
        ["Setup needs the internet to download the files."] =
            "El instalador necesita internet para descargar los archivos.",
        ["Nothing changes until you click Install."] = "No cambia nada hasta que hagas clic en Instalar.",
        ["Lighting, picture and performance for The Sims 3."] = "Iluminación, imagen y rendimiento para The Sims 3.",
        ["What Setup does"] = "Qué hace el instalador",
        ["Setup can't reach GitHub"] = "El instalador no puede acceder a GitHub",
        ["Check your internet connection, then try again."] = "Comprueba tu conexión a internet y vuelve a intentarlo.",
        ["Try again"] = "Reintentar",
        ["Choose your game"] = "Elige tu juego",
        ["Setup found these installs. Pick the one you play."] =
            "El instalador encontró estas instalaciones. Elige con la que juegas.",
        ["Setup found no install. Browse for the game folder."] =
            "No se encontró ninguna instalación. Busca la carpeta del juego.",
        ["Browse for another folder"] = "Buscar otra carpeta",
        ["No game in this folder"] = "No hay ningún juego en esta carpeta",
        ["Pick the The Sims 3 folder, or its Game\\Bin folder (the one with TS3W.exe or TS3.exe)."] =
            "Elige la carpeta de The Sims 3, o su carpeta Game\\Bin (la que tiene TS3W.exe o TS3.exe).",
        ["Not installed"] = "No instalado",
        ["Installed"] = "Instalado",
        ["the game's own Direct3D 9"] = "el Direct3D 9 del propio juego",
        ["another d3d9.dll"] = "otra d3d9.dll",
        ["Required"] = "Obligatorio",
        ["Installs {0} into Game\\Bin."] = "Instala {0} en Game\\Bin.",
        ["Replaces the installed copy with {0}. Your settings stay."] =
            "Sustituye la copia instalada por {0}. Tus ajustes se mantienen.",
        ["Updates {0} to {1}. Your settings stay."] = "Actualiza de {0} a {1}. Tus ajustes se mantienen.",
        ["Recommended with Apex"] = "Recomendados con Apex",
        ["No updates available"] = "No hay actualizaciones",
        ["Everything in this game is on its latest version."] = "Todo en este juego está en su última versión.",
        ["Not available for this game version: it supports Steam 1.67.2 and EA app 1.69.47."] =
            "No disponible para esta versión del juego: admite Steam 1.67.2 y EA app 1.69.47.",
        ["Wrong build"] = "Versión errónea",
        ["Update {0}"] = "Actualizar {0}",
        ["Up to date"] = "Actualizado",
        ["Setup could not finish"] = "El instalador no pudo terminar",
        ["Close The Sims 3 first."] = "Cierra The Sims 3 primero.",
        ["You're all set"] = "Todo listo",
        ["Start The Sims 3, load a world and press Ctrl+Shift+F11 to open the Apex Radiance menu."] =
            "Abre The Sims 3, carga un mundo y pulsa Ctrl+Shift+F11 para abrir el menú de Apex Radiance.",
        ["Installed: {0} {1}"] = "Instalado: {0} {1}",
        ["Choose your game first."] = "Elige tu juego primero.",
        ["Removes ApexRadiance.asi. Your settings in Documents stay."] =
            "Quita ApexRadiance.asi. Tus ajustes en Documentos se mantienen.",
        ["Remove"] = "Quitar",
        ["RTX Remix bridge"] = "RTX Remix bridge",
        ["Removes Sims3SettingsSetter.asi."] = "Quita Sims3SettingsSetter.asi.",
        ["Removes ts3native and ts3fixes.package."] = "Quita ts3native y ts3fixes.package.",
        ["Repair or update"] = "Reparar o actualizar",
        ["Downloads and copies again everything installed here. Your settings stay."] =
            "Vuelve a descargar y copiar todo lo instalado aquí. Tus ajustes se mantienen.",
        ["Repair"] = "Reparar",
        ["Uninstall everything"] = "Desinstalar todo",
        ["Removes what Setup installed, and Apex Radiance, and puts back every file Setup replaced."] =
            "Quita lo que instaló el instalador y Apex Radiance, y restaura cada archivo que el instalador sustituyó.",
        ["Uninstall"] = "Desinstalar",
        ["Remove {0}?"] = "¿Quitar {0}?",
        ["Close the game first. Setup removes the files and puts back the ones it replaced."] =
            "Cierra el juego antes. El instalador quita los archivos y restaura los que sustituyó.",
        ["{0} removed."] = "{0} quitado.",
        ["Uninstall everything?"] = "¿Desinstalar todo?",
        ["Setup removes Apex Radiance and everything it installed in this game, and puts back every file it replaced. Your settings in Documents stay."] =
            "El instalador quita Apex Radiance y todo lo que instaló en este juego, y restaura cada archivo que sustituyó. Tus ajustes en Documentos se mantienen.",
        ["Everything Setup installed was removed."] = "Se quitó todo lo que instaló el instalador.",
        ["Setup fixes this"] = "El instalador lo corrige",
        ["Read more"] = "Más información",
        ["Fixed."] = "Corregido.",
        ["No incompatible settings found."] = "No se encontraron ajustes incompatibles.",
        ["{0} in Game\\Bin loads a second copy of Apex Radiance."] =
            "{0} en Game\\Bin carga una segunda copia de Apex Radiance.",
        ["Two ASI loaders ({0}) load every mod twice. Keep one: remove all but {1}."] =
            "Dos ASI loaders ({0}) cargan cada mod dos veces. Deja solo uno: quita todos menos {1}.",
        ["Sims 3 Modernization's ts3native is the build for the other edition and does nothing. Turn it on in Components to install the right one."] =
            "El ts3native de Sims 3 Modernization es la versión de la otra edición y no hace nada. Actívalo en Componentes para instalar la correcta.",
        ["Sims 3 Modernization has no build for this game version ({0}), so its ts3native does nothing. Turn it off in Components."] =
            "Sims 3 Modernization no tiene versión para esta versión del juego ({0}), así que su ts3native no hace nada. Desactívalo en Componentes.",
        ["{0} in Game\\Bin is a second copy of {1}; both load. Setup moves it to the backup folder."] =
            "{0} en Game\\Bin es una segunda copia de {1}; se cargan las dos. El instalador la mueve a la carpeta de copias.",
        ["Mods has {0} .package files but no Resource.cfg, so the game loads none of them."] =
            "Mods tiene {0} archivos .package pero ningún Resource.cfg, así que el juego no carga ninguno.",
        ["{0} is the build for the other edition of the game; it makes the game show \"unofficial game modification\". Use the {1} build."] =
            "{0} es la versión para la otra edición del juego; hace que el juego muestre \"unofficial game modification\". Usa la versión {1}.",
        ["NRaas ErrorTrap has no build known to work on EA app 1.69.47."] =
            "NRaas ErrorTrap no tiene ninguna versión que se sepa que funcione en EA app 1.69.47.",
        ["{0} overrides game code Sims 3 Modernization also patches (not tested together)."] =
            "{0} sustituye código del juego que Sims 3 Modernization también modifica (no se han probado juntos).",
        ["{0} is an old build with Apex inside; while it is in Game\\Bin, Apex Radiance stays off. Setup moves it to the backup folder (install the official Sims3SettingsSetter in its place if you used it)."] =
            "{0} es una compilación antigua con Apex dentro; mientras esté en Game\\Bin, Apex Radiance queda desactivado. El instalador la mueve a la carpeta de copias (instala el Sims3SettingsSetter oficial en su lugar si lo usabas).",
        ["Sims3Performance's ShortWaitSpin and ThreadStackDiet clash with Sims 3 Modernization. Setup sets both to 0 (a copy is kept as Sims3Performance.ini.apex-backup)."] =
            "ShortWaitSpin y ThreadStackDiet de Sims3Performance chocan con Sims 3 Modernization. El instalador pone ambos a 0 (se guarda una copia como Sims3Performance.ini.apex-backup).",
        ["Sims3SettingsSetter's uncompressed textures stutter under the RTX Remix bridge. Setup turns them off (a copy is kept as S3SS.toml.apex-backup)."] =
            "Las texturas sin comprimir de Sims3SettingsSetter dan tirones con el RTX Remix bridge. El instalador las desactiva (se guarda una copia como S3SS.toml.apex-backup).",
        ["{0} sets client.targetProcess = {1}, but this game is {2}: the bridge does not start."] =
            "{0} define client.targetProcess = {1}, pero este juego es {2}: el bridge no se inicia.",
        ["dxvk.conf lacks {0} or has other values: without them the game resets its graphics options or freezes on a resolution change under the bridge."] =
            "A dxvk.conf le falta {0} o tiene otros valores: sin ellos, el juego restablece sus opciones gráficas o se congela al cambiar la resolución con el bridge.",
        ["Mods\\Resource.cfg does not read {0}, so the game does not load it. Setup adds the Packages folder lines (a copy is kept as Resource.cfg.apex-backup)."] =
            "Mods\\Resource.cfg no lee {0}, así que el juego no lo carga. El instalador añade las líneas de la carpeta Packages (se guarda una copia como Resource.cfg.apex-backup).",
        ["Fix settings"] = "Corregir ajustes",
        ["Everything is installed and up to date."] = "Todo está instalado y actualizado.",
        ["Will be removed"] = "Se quitará",
        ["Keep"] = "Mantener",
        ["Nothing to apply"] = "Nada que aplicar",
        ["Preview"] = "Vista previa",
        ["This is a preview: nothing was installed."] = "Esto es una vista previa: no se instaló nada.",
        ["Setup is applying changes"] = "El instalador está aplicando cambios",
        ["Wait until it finishes before closing."] = "Espera a que termine antes de cerrar.",
        ["Checking the latest versions…"] = "Buscando las últimas versiones…",
        ["Checking for updates…"] = "Buscando actualizaciones…",
        ["Close The Sims 3 to update."] = "Cierra The Sims 3 para actualizar.",
        ["Update now installs them."] = "«Actualizar ahora» las instala.",
        ["Graphics"] = "Gráficos",
        ["Checked at {0}."] = "Comprobado a las {0}.",
        ["Test build {0}: kept as it is."] = "Versión de prueba {0}: se deja como está.",
        ["Repair or remove sets the game right."] = "«Reparar o quitar» deja el juego como estaba.",
        ["Changes applied"] = "Cambios aplicados",
        ["Removed: {0}"] = "Quitado: {0}",
        ["The Sims 3 · {0}"] = "The Sims 3 · {0}",
        ["{0} → {1}"] = "{0} → {1}",
        ["{0} · up to date"] = "{0} · actualizado",
        ["Everything is in place."] = "Todo está en orden.",
        ["Everything is in place (checked at {0})."] = "Todo está en orden (comprobado a las {0}).",
        ["Check installation"] = "Comprobar instalación",
        ["Change components"] = "Cambiar componentes",
        ["Apex Radiance is not installed in this game."] = "Apex Radiance no está instalado en este juego.",
        ["{0} · test build"] = "{0} · versión de prueba",
        ["Install the official {0}"] = "Instalar la versión oficial {0}",
        ["No ASI loader in Game\\Bin: Apex Radiance and the other .asi mods do not load."] =
            "No hay ningún ASI loader en Game\\Bin: Apex Radiance y los demás mods .asi no se cargan.",
        ["{0} is missing: the RTX Remix bridge does not start."] = "Falta {0}: el RTX Remix bridge no se inicia.",
        ["Apply 1 change"] = "Aplicar 1 cambio",
        ["Apply {0} changes"] = "Aplicar {0} cambios",
        ["{0} is installed and up to date: nothing to change."] =
            "{0} está instalado y actualizado: no hay nada que cambiar.",
        ["Keep my d3d9.dll"] = "Mantener mi d3d9.dll",
        ["Your d3d9.dll stays as it is."] = "Tu d3d9.dll se queda como está.",
        ["Applying changes"] = "Aplicando cambios",
        ["Remove {0}"] = "Quitar {0}",
        ["Fix incompatible settings"] = "Corregir ajustes incompatibles",
        // suggestions: what Apex Radiance recommends in game (Compat.Suggestions); the game's own texts as the menu translates them
        ["Suggested for Apex Radiance"] = "Sugerido para Apex Radiance",
        ["Apply"] = "Aplicar",
        ["Applied."] = "Aplicado.",
        ["Apply all suggestions"] = "Aplicar todas las sugerencias",
        ["Apply suggestion: {0}"] = "Aplicar sugerencia: {0}",
        ["Two copies of the mod"] = "Dos copias del mod",
        ["{0} in Game\\Bin is another copy of Apex Radiance; only one copy runs. Apply moves it to the backup folder."] = "{0} en Game\\Bin es otra copia de Apex Radiance; solo se ejecuta una copia. Aplicar la mueve a la carpeta de copias.",
        ["Turn off the game's Edge Smoothing"] = "Desactiva el suavizado de bordes del juego",
        ["Options.ini has edgesmoothing = {0}. The Sims 3's Edge Smoothing prevents these enabled Apex effects from working: {1}. Apply sets it to 0 (a copy is kept as Options.ini.apex-backup)."] = "Options.ini tiene edgesmoothing = {0}. El suavizado de bordes de Los Sims 3 impide que funcionen estos efectos activos de Apex: {1}. Aplicar lo pone a 0 (se guarda una copia como Options.ini.apex-backup).",
        ["Edge Smoothing"] = "Suavizado de Bordes",
        ["Depth Blur"] = "Desenfoque de Profundidad",
        ["Ambient Occlusion"] = "Oclusión Ambiental",
        ["Water Reflections"] = "Reflejos en el Agua",
        ["Runs the game on Vulkan: fewer stutters, and Apex Radiance is made and tested with it."] = "Ejecuta el juego en Vulkan: menos tirones, y Apex Radiance está hecho y probado con él.",
        ["Apex Radiance works on its own, but it pairs well with Sims3SettingsSetter by sims3fiend: a frame rate limiter, fewer stutters and many extra game settings."] = "Apex Radiance funciona por sí solo, pero combina bien con Sims3SettingsSetter de sims3fiend: limitador de fotogramas por segundo, menos tirones y muchos ajustes extra del juego.",
        ["Moves DXVK into its own 64-bit process: the game keeps about 600 MB more free memory for long sessions and big worlds."] = "Lleva DXVK a su propio proceso de 64 bits: el juego conserva unos 600 MB más de memoria libre para sesiones largas y mundos grandes.",
        ["{0} has {1} = {2}, which Setup cannot read. Setup leaves the file as it is."] = "{0} tiene {1} = {2}, que el instalador no puede leer. El instalador deja el archivo como está.",
        // restore points (Repair or remove)
        ["Install {0}"] = "Instalar {0}",
        ["Restore points"] = "Puntos de restauración",
        ["Setup keeps a copy of every file it changes. Restoring a point undoes that change and every later one."] = "El instalador guarda una copia de cada archivo que cambia. Restaurar un punto deshace ese cambio y todos los posteriores.",
        ["Before: {0}"] = "Antes de: {0}",
        ["{0} · 1 file"] = "{0} · 1 archivo",
        ["{0} · {1} files"] = "{0} · {1} archivos",
        ["Restore"] = "Restaurar",
        ["No restore points yet. Setup makes one before every change."] = "Aún no hay puntos de restauración. El instalador crea uno antes de cada cambio.",
        ["Restore this point?"] = "¿Restaurar este punto?",
        ["Setup puts back the files as they were before this change, and undoes every change made after it. Close the game first."] = "El instalador restaura los archivos como estaban antes de este cambio y deshace todos los cambios posteriores. Cierra el juego antes.",
        ["Setup could not put back {0}."] = "El instalador no pudo restaurar {0}.",
        ["Changes undone."] = "Cambios deshechos.",
        ["Undo these changes"] = "Deshacer estos cambios",
        ["This restore point is gone."] = "Este punto de restauración ya no existe.",
        ["Updates available"] = "Actualizaciones disponibles",
        ["Update now"] = "Actualizar ahora",
        ["{0} {1} → {2}"] = "{0} {1} → {2}",
        ["Removes MonoPatcher.asi and its package."] = "Quita MonoPatcher.asi y su paquete.",
        ["Smooth Patch's TS3Patch.asi patches the same game code as Sims3SettingsSetter's Smooth Patch. Setup moves it to the backup folder (S3SS does the same job)."] =
            "El TS3Patch.asi de Smooth Patch modifica el mismo código del juego que el Smooth Patch de Sims3SettingsSetter. El instalador lo mueve a la carpeta de copias (S3SS hace el mismo trabajo).",
        ["simler90's Gameplay Core Mod and NRaas StoryProgression replace the same game code. Keep one of them."] =
            "Gameplay Core Mod de simler90 y NRaas StoryProgression sustituyen el mismo código del juego. Deja solo uno de los dos.",
        ["MonoPatcher needs the Microsoft Visual C++ Redistributable (x86); it is not installed."] =
            "MonoPatcher necesita Microsoft Visual C++ Redistributable (x86), que no está instalado.",
        ["Download on Nexus Mods"] = "Descargar en Nexus Mods",
        ["Choose the downloaded file"] = "Elegir el archivo descargado",
        ["Ready to install"] = "Listo para instalar",
        ["Waiting for your download in {0}…"] = "Esperando tu descarga en {0}…",
        ["Not the Performance Patch"] = "No es el Performance Patch",
        ["{0} has no Sims3Performance.asi. If the download is a .7z or .rar, extract it and choose Sims3Performance.asi."] =
            "{0} no tiene Sims3Performance.asi. Si la descarga es un .7z o .rar, extráela y elige Sims3Performance.asi.",
        ["Removes Sims3Performance.asi and its .ini."] = "Quita Sims3Performance.asi y su .ini.",
        ["Download Sims 3 Performance Patch from Nexus Mods first."] =
            "Descarga primero Sims 3 Performance Patch desde Nexus Mods.",
        ["Finds your The Sims 3 (Steam or EA app)."] = "Encuentra tu The Sims 3 (Steam o EA app).",
        ["Installs Apex Radiance and the fixes you choose."] = "Instala Apex Radiance y las correcciones que elijas.",
        ["Keeps a copy of every file it changes, so you can undo everything."] =
            "Guarda una copia de cada archivo que cambia, para que puedas deshacerlo todo.",
        ["Your Apex Radiance settings are always kept."] = "Tus ajustes de Apex Radiance siempre se conservan.",
        ["No internet connection"] = "Sin conexión a internet",
        ["Vulkan, DXVK (recommended)"] = "Vulkan, DXVK (recomendado)",
        ["Vulkan with more memory (RTX Remix bridge)"] = "Vulkan con más memoria (RTX Remix bridge)",
        ["The RTX Remix bridge needs a graphics card with Vulkan 1.3 (found: {0})."] = "El RTX Remix bridge necesita una tarjeta gráfica con Vulkan 1.3 (encontrada: {0}).",
        ["1 update for The Sims 3 · {0}"] = "1 actualización para The Sims 3 · {0}",
        ["Change"] = "Cambiar",
        ["Close the game before you start. Everything comes from each author's official GitHub, and Setup keeps a copy of every file it changes."] = "Cierra el juego antes de empezar. Todo viene del GitHub oficial de cada autor, y el instalador guarda una copia de cada archivo que cambia.",
        ["Everything is updated"] = "Todo actualizado",
        ["Game, components and installation"] = "Juego, componentes e instalación",
        ["How the game draws"] = "Cómo dibuja el juego",
        ["Not this one?"] = "¿No es este?",
        ["Optional mods"] = "Mods opcionales",
        ["Replaces {0}"] = "Sustituye {0}",
        ["See what's new"] = "Ver las novedades",
        ["Setup makes a restore point before it changes any file, so you can undo this."] = "El instalador crea un punto de restauración antes de cambiar cualquier archivo, así que puedes deshacerlo.",
        ["Something looks wrong in the game? This puts back everything as it was before this update."] = "¿Algo se ve raro en el juego? Esto devuelve todo a como estaba antes de esta actualización.",
        ["Step {0} of {1}: {2}"] = "Paso {0} de {1}: {2}",
        ["The Sims 3 · {0} found"] = "The Sims 3 · {0} encontrado",
        ["Undo this update"] = "Deshacer esta actualización",
        ["Update 1 item"] = "Actualizar 1 elemento",
        ["Update {0} items"] = "Actualizar {0} elementos",
        ["Use {0}"] = "Usar {0}",
        ["WELCOME"] = "BIENVENIDO",
        ["Will be installed"] = "Se instalará",
        ["Your graphics card: {0}"] = "Tu tarjeta gráfica: {0}",
        ["{0} · will be removed"] = "{0} · se quitará",
        ["{0} → updates to {1}"] = "{0} → se actualiza a {1}",
        ["{0} updates for The Sims 3 · {1}"] = "{0} actualizaciones para The Sims 3 · {1}",
        ["Windows default (Direct3D 9)"] = "Predeterminado de Windows (Direct3D 9)",
        ["Fewer stutters; Apex Radiance is made and tested with it."] =
            "Menos tirones; Apex Radiance se hace y se prueba con él.",
        ["Keeps about 600 MB more memory free for big worlds. Needs a Vulkan graphics card. If the game misbehaves with it, choose DXVK."] =
            "Deja unos 600 MB más de memoria libre para mundos grandes. Necesita una tarjeta gráfica con Vulkan. Si el juego da problemas con él, elige DXVK.",
        ["Works, but Apex Radiance is tested with DXVK."] = "Funciona, pero Apex Radiance se prueba con DXVK.",
        ["Your in-game graphics settings may reset once. Setup keeps a copy and puts it back if you switch back."] =
            "Los ajustes gráficos del juego pueden restablecerse una vez. El instalador guarda una copia y la restaura si vuelves atrás.",
        ["Fixes for game and graphics settings."] = "Correcciones para los ajustes del juego y de los gráficos.",
        ["Fixes stutters, Sims getting stuck and crashes."] = "Corrige tirones, Sims atascados y cierres inesperados.",
        ["Needed only by some script mods; turn it on if a mod asks for it."] =
            "Solo lo necesitan algunos mods de script; actívalo si un mod lo pide.",
        ["Makes the game run faster. Needs a free Nexus Mods account; Setup installs it when your download finishes."] =
            "Hace que el juego vaya más rápido. Necesita una cuenta gratuita de Nexus Mods; el instalador lo instala cuando termine tu descarga.",
        ["Puts back the graphics files you had before."] = "Restaura los archivos gráficos que tenías antes.",
        ["Goes back to the game's default graphics. Your in-game graphics settings may reset once."] =
            "Vuelve a los gráficos predeterminados del juego. Los ajustes gráficos del juego pueden restablecerse una vez.",
        ["Language"] = "Idioma",
        ["Automatic"] = "Automático",
        ["This file has no Sims3Performance.asi."] = "Este archivo no tiene Sims3Performance.asi.",
        ["Dry run: Setup changes nothing."] = "Prueba: el instalador no cambia nada.",
        ["Apex Radiance by loinyx. The RTX Remix bridge is NVIDIA's, ported to The Sims 3 by dalexlol and alexvixell35-stack. Sims3SettingsSetter is by sims3fiend, Sims 3 Modernization by rodneypantonial and Ultimate ASI Loader by ThirteenAG: Setup downloads them from their own GitHub releases."] =
            "Apex Radiance por loinyx. El RTX Remix bridge es de NVIDIA, adaptado a The Sims 3 por dalexlol y alexvixell35-stack. Sims3SettingsSetter es de sims3fiend, Sims 3 Modernization de rodneypantonial y Ultimate ASI Loader de ThirteenAG: el instalador los descarga de sus propias versiones en GitHub.",
        ["RTX Remix bridge: third-party notices"] = "RTX Remix bridge: avisos de terceros",
        ["Read"] = "Leer",
        ["Licence text"] = "Texto de la licencia",
        ["OK"] = "Aceptar",
        ["Cancel"] = "Cancelar",
        ["A new Setup is available"] = "Hay un instalador nuevo",
        ["Apex Radiance Setup {0} was downloaded and checked. Update Setup, then it opens again."] = "Apex Radiance Setup {0} se ha descargado y comprobado. Actualiza el instalador y se abrirá de nuevo.",
        ["Update Setup"] = "Actualizar el instalador",
    };
}
