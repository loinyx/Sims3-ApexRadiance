// Apex Radiance Setup texts in Hungarian (Magyar). Fill only the entries between the braces, one per line:
//     ["<English key, exactly as in keys.json>"] = "<translation>",
// C# strings: write \" for a quote, \\ for a backslash, \n for a line break; every other character as itself (UTF-8).
// Keep each {0}, {1} of the key, product and file names as they are. A key without an entry shows in English; never
// leave a translation empty. The same key twice: the last one wins, and ApexSetup.Tests fails.
namespace ApexSetup;

public static partial class Strings
{
    static readonly Dictionary<string, string> Hu = new()
    {
        ["Welcome"] = "Kezdés",
        ["Game"] = "Játék",
        ["Components"] = "Összetevők",
        ["Install"] = "Telepítés",
        ["Done"] = "Kész",
        ["Repair or remove"] = "Javítás vagy eltávolítás",
        ["About and licences"] = "Névjegy és licencek",
        ["Back"] = "Vissza",
        ["Next"] = "Tovább",
        ["Get started"] = "Kezdjük",
        ["Finish"] = "Befejezés",
        ["Close The Sims 3 to continue."] = "A folytatáshoz zárd be a The Sims 3-at.",
        ["Setup needs the internet to download the files."] =
            "A telepítőnek internetre van szüksége a fájlok letöltéséhez.",
        ["Nothing changes until you click Install."] = "Semmi sem változik, amíg nem kattintasz a Telepítés gombra.",
        ["Lighting, picture and performance for The Sims 3."] = "Világítás, kép és teljesítmény a The Sims 3-hoz.",
        ["What Setup does"] = "Mit csinál a telepítő",
        ["Setup can't reach GitHub"] = "A telepítő nem éri el a GitHubot",
        ["Check your internet connection, then try again."] = "Ellenőrizd az internetkapcsolatot, majd próbáld újra.",
        ["Try again"] = "Újrapróbálás",
        ["Choose your game"] = "Válaszd ki a játékot",
        ["Setup found these installs. Pick the one you play."] =
            "A telepítő ezeket a telepítéseket találta. Válaszd azt, amelyikkel játszol.",
        ["Setup found no install. Browse for the game folder."] =
            "A telepítő nem talált telepítést. Tallózd ki a játék mappáját.",
        ["Browse for another folder"] = "Másik mappa tallózása",
        ["No game in this folder"] = "Ebben a mappában nincs játék",
        ["Pick the The Sims 3 folder, or its Game\\Bin folder (the one with TS3W.exe or TS3.exe)."] =
            "Válaszd a The Sims 3 mappáját vagy a Game\\Bin mappáját (amelyikben a TS3W.exe vagy a TS3.exe van).",
        ["Not installed"] = "Nincs telepítve",
        ["Installed"] = "Telepítve",
        ["the game's own Direct3D 9"] = "a játék saját Direct3D 9-e",
        ["another d3d9.dll"] = "másik d3d9.dll",
        ["Required"] = "Kötelező",
        ["Installs {0} into Game\\Bin."] = "A(z) {0} verziót telepíti a Game\\Bin mappába.",
        ["Replaces the installed copy with {0}. Your settings stay."] =
            "A telepített példányt a(z) {0} verzióra cseréli. A beállításaid megmaradnak.",
        ["Updates {0} to {1}. Your settings stay."] =
            "Frissít a(z) {0} verzióról a(z) {1} verzióra. A beállításaid megmaradnak.",
        ["Recommended with Apex"] = "Ajánlott az Apexhez",
        ["No updates available"] = "Nincs elérhető frissítés",
        ["Everything in this game is on its latest version."] = "Ebben a játékban minden a legújabb verzión van.",
        ["Not available for this game version: it supports Steam 1.67.2 and EA app 1.69.47."] =
            "Ehhez a játékverzióhoz nem érhető el: a Steam 1.67.2 és az EA app 1.69.47 verziót támogatja.",
        ["Wrong build"] = "Rossz verzió",
        ["Update {0}"] = "Frissítés: {0}",
        ["Up to date"] = "Naprakész",
        ["Setup could not finish"] = "A telepítő nem tudott végezni",
        ["Close The Sims 3 first."] = "Előbb zárd be a The Sims 3-at.",
        ["You're all set"] = "Minden kész",
        ["Start The Sims 3, load a world and press Ctrl+Shift+F11 to open the Apex Radiance menu."] =
            "Indítsd el a The Sims 3-at, tölts be egy világot, és nyomd meg a Ctrl+Shift+F11 billentyűt az Apex Radiance menü megnyitásához.",
        ["Installed: {0} {1}"] = "Telepítve: {0} {1}",
        ["Choose your game first."] = "Előbb válaszd ki a játékot.",
        ["Removes ApexRadiance.asi. Your settings in Documents stay."] =
            "Eltávolítja az ApexRadiance.asi fájlt. A Documents mappában lévő beállításaid megmaradnak.",
        ["Remove"] = "Eltávolítás",
        ["RTX Remix bridge"] = "RTX Remix bridge",
        ["Removes Sims3SettingsSetter.asi."] = "Eltávolítja a Sims3SettingsSetter.asi fájlt.",
        ["Removes ts3native and ts3fixes.package."] = "Eltávolítja a ts3native és a ts3fixes.package fájlt.",
        ["Repair or update"] = "Javítás vagy frissítés",
        ["Downloads and copies again everything installed here. Your settings stay."] =
            "Újra letölt és bemásol mindent, ami itt telepítve van. A beállításaid megmaradnak.",
        ["Repair"] = "Javítás",
        ["Uninstall everything"] = "Minden eltávolítása",
        ["Removes what Setup installed, and Apex Radiance, and puts back every file Setup replaced."] =
            "Eltávolítja, amit a telepítő telepített, és az Apex Radiance-t, és visszaállít minden fájlt, amelyet a telepítő lecserélt.",
        ["Uninstall"] = "Eltávolítás",
        ["Remove {0}?"] = "Eltávolítod: {0}?",
        ["Close the game first. Setup removes the files and puts back the ones it replaced."] =
            "Előbb zárd be a játékot. A telepítő eltávolítja a fájlokat, és visszaállítja azokat, amelyeket lecserélt.",
        ["{0} removed."] = "{0} eltávolítva.",
        ["Uninstall everything?"] = "Mindent eltávolítasz?",
        ["Setup removes Apex Radiance and everything it installed in this game, and puts back every file it replaced. Your settings in Documents stay."] =
            "A telepítő eltávolítja az Apex Radiance-t és mindent, amit ebbe a játékba telepített, és visszaállít minden fájlt, amelyet lecserélt. A Documents mappában lévő beállításaid megmaradnak.",
        ["Everything Setup installed was removed."] = "A telepítő minden telepített elemet eltávolított.",
        ["Setup fixes this"] = "A telepítő kijavítja",
        ["Read more"] = "Bővebben",
        ["Fixed."] = "Javítva.",
        ["No incompatible settings found."] = "Nincs összeférhetetlen beállítás.",
        ["{0} in Game\\Bin loads a second copy of Apex Radiance."] =
            "A Game\\Bin mappában lévő {0} az Apex Radiance egy második példányát tölti be.",
        ["Two ASI loaders ({0}) load every mod twice. Keep one: remove all but {1}."] =
            "Két ASI loader ({0}) minden modot kétszer tölt be. Csak egy maradjon: a(z) {1} kivételével távolítsd el a többit.",
        ["Sims 3 Modernization's ts3native is the build for the other edition and does nothing. Turn it on in Components to install the right one."] =
            "A Sims 3 Modernization ts3native fájlja a másik kiadás verziója, és nem csinál semmit. Kapcsold be az Összetevők között, hogy a megfelelő települjön.",
        ["Sims 3 Modernization has no build for this game version ({0}), so its ts3native does nothing. Turn it off in Components."] =
            "A Sims 3 Modernization nem rendelkezik verzióval ehhez a játékverzióhoz ({0}), így a ts3native fájlja nem csinál semmit. Kapcsold ki az Összetevők között.",
        ["{0} in Game\\Bin is a second copy of {1}; both load. Setup moves it to the backup folder."] =
            "A Game\\Bin mappában lévő {0} a(z) {1} második példánya; mindkettő betöltődik. A telepítő áthelyezi a biztonsági mentések mappájába.",
        ["Mods has {0} .package files but no Resource.cfg, so the game loads none of them."] =
            "A Mods mappában {0} .package fájl van, de nincs Resource.cfg, így a játék egyiket sem tölti be.",
        ["{0} is the build for the other edition of the game; it makes the game show \"unofficial game modification\". Use the {1} build."] =
            "A(z) {0} a játék másik kiadásához készült; miatta a játék az \"unofficial game modification\" üzenetet mutatja. Használd a(z) {1} verziót.",
        ["NRaas ErrorTrap has no build known to work on EA app 1.69.47."] =
            "Az NRaas ErrorTrapnek nincs olyan verziója, amelyről biztosan tudni, hogy működik az EA app 1.69.47 verzióval.",
        ["{0} overrides game code Sims 3 Modernization also patches (not tested together)."] =
            "A(z) {0} olyan játékkódot ír felül, amelyet a Sims 3 Modernization is módosít (együtt nem tesztelték).",
        ["{0} is an old build with Apex inside; while it is in Game\\Bin, Apex Radiance stays off. Setup moves it to the backup folder (install the official Sims3SettingsSetter in its place if you used it)."] =
            "A(z) {0} egy régi verzió, benne az Apexszel; amíg a Game\\Bin mappában van, az Apex Radiance kikapcsolva marad. A telepítő áthelyezi a biztonsági mentések mappájába (ha használtad, telepítsd helyette a hivatalos Sims3SettingsSetter-t).",
        ["Sims3Performance's ShortWaitSpin and ThreadStackDiet clash with Sims 3 Modernization. Setup sets both to 0 (a copy is kept as Sims3Performance.ini.apex-backup)."] =
            "A Sims3Performance ShortWaitSpin és ThreadStackDiet beállítása ütközik a Sims 3 Modernizationnel. A telepítő mindkettőt 0-ra állítja (egy másolat megmarad Sims3Performance.ini.apex-backup néven).",
        ["Sims3SettingsSetter's uncompressed textures stutter under the RTX Remix bridge. Setup turns them off (a copy is kept as S3SS.toml.apex-backup)."] =
            "A Sims3SettingsSetter tömörítetlen textúrái akadozást okoznak az RTX Remix bridge mellett. A telepítő kikapcsolja őket (egy másolat megmarad S3SS.toml.apex-backup néven).",
        ["{0} sets client.targetProcess = {1}, but this game is {2}: the bridge does not start."] =
            "A(z) {0} a client.targetProcess = {1} értéket állítja be, de ez a játék a(z) {2}: a bridge nem indul el.",
        ["dxvk.conf lacks {0} or has other values: without them the game resets its graphics options or freezes on a resolution change under the bridge."] =
            "A dxvk.conf fájlból hiányzik a(z) {0}, vagy más az értéke: enélkül a játék a bridge mellett visszaállítja a grafikai beállításait, vagy lefagy felbontásváltáskor.",
        ["Mods\\Resource.cfg does not read {0}, so the game does not load it. Setup adds the Packages folder lines (a copy is kept as Resource.cfg.apex-backup)."] =
            "A Mods\\Resource.cfg nem olvassa be ezt: {0}, így a játék nem tölti be. A telepítő hozzáadja a Packages mappa sorait (egy másolat megmarad Resource.cfg.apex-backup néven).",
        ["Fix settings"] = "Beállítások javítása",
        ["Everything is installed and up to date."] = "Minden telepítve van és naprakész.",
        ["Will be removed"] = "Eltávolításra kerül",
        ["Keep"] = "Megtartás",
        ["Nothing to apply"] = "Nincs mit alkalmazni",
        ["Preview"] = "Előnézet",
        ["This is a preview: nothing was installed."] = "Ez csak előnézet: a telepítő semmit sem telepített.",
        ["Setup is applying changes"] = "A telepítő alkalmazza a változásokat",
        ["Wait until it finishes before closing."] = "Bezárás előtt várd meg, amíg befejezi.",
        ["Checking the latest versions…"] = "A legújabb verziók ellenőrzése…",
        ["Checking for updates…"] = "Frissítések keresése…",
        ["Close The Sims 3 to update."] = "A frissítéshez zárd be a The Sims 3-at.",
        ["Update now installs them."] = "A Frissítés most gomb telepíti őket.",
        ["Graphics"] = "Grafika",
        ["Checked at {0}."] = "Ellenőrizve: {0}.",
        ["Test build {0}: kept as it is."] = "Tesztverzió ({0}): változatlan marad.",
        ["Repair or remove sets the game right."] = "A Javítás vagy eltávolítás rendbe hozza a játékot.",
        ["Changes applied"] = "Változások alkalmazva",
        ["Removed: {0}"] = "Eltávolítva: {0}",
        ["The Sims 3 · {0}"] = "The Sims 3 · {0}",
        ["{0} → {1}"] = "{0} → {1}",
        ["{0} · up to date"] = "{0} · naprakész",
        ["Everything is in place."] = "Minden rendben.",
        ["Everything is in place (checked at {0})."] = "Minden rendben (ellenőrizve: {0}).",
        ["Check installation"] = "Telepítés ellenőrzése",
        ["Change components"] = "Összetevők módosítása",
        ["Apex Radiance is not installed in this game."] = "Az Apex Radiance nincs telepítve ebben a játékban.",
        ["{0} · test build"] = "{0} · tesztverzió",
        ["Install the official {0}"] = "A hivatalos {0} verzió telepítése",
        ["No ASI loader in Game\\Bin: Apex Radiance and the other .asi mods do not load."] =
            "Nincs ASI loader a Game\\Bin mappában: az Apex Radiance és a többi .asi mod nem töltődik be.",
        ["{0} is missing: the RTX Remix bridge does not start."] =
            "Hiányzik a(z) {0}: az RTX Remix bridge nem indul el.",
        ["Apply 1 change"] = "1 változás alkalmazása",
        ["Apply {0} changes"] = "{0} változás alkalmazása",
        ["{0} is installed and up to date: nothing to change."] =
            "A(z) {0} telepítve van és naprakész: nincs mit módosítani.",
        ["Keep my d3d9.dll"] = "Saját d3d9.dll megtartása",
        ["Your d3d9.dll stays as it is."] = "A d3d9.dll fájlod változatlan marad.",
        ["Applying changes"] = "Változások alkalmazása",
        ["Remove {0}"] = "Eltávolítás: {0}",
        ["Fix incompatible settings"] = "Összeférhetetlen beállítások javítása",
        // suggestions: what Apex Radiance recommends in game (Compat.Suggestions); the game's own texts as the menu translates them
        ["Suggested for Apex Radiance"] = "Javasolt az Apex Radiance-hez",
        ["Apply"] = "Alkalmaz",
        ["Applied."] = "Alkalmazva.",
        ["Apply all suggestions"] = "Összes javaslat alkalmazása",
        ["Apply suggestion: {0}"] = "Javaslat alkalmazása: {0}",
        ["Two copies of the mod"] = "A mod két példánya",
        ["{0} in Game\\Bin is another copy of Apex Radiance; only one copy runs. Apply moves it to the backup folder."] = "A Game\\Bin mappában lévő {0} az Apex Radiance egy másik példánya; csak egy példány fut. Az Alkalmaz áthelyezi a biztonsági mentések mappájába.",
        ["Turn off the game's Edge Smoothing"] = "A játék élsimításának kikapcsolása",
        ["Options.ini has edgesmoothing = {0}. The Sims 3's Edge Smoothing prevents these enabled Apex effects from working: {1}. Apply sets it to 0 (a copy is kept as Options.ini.apex-backup)."] = "Az Options.ini-ben edgesmoothing = {0}. A The Sims 3 élsimítása megakadályozza ezeknek a bekapcsolt Apex-effektusoknak a működését: {1}. Az Alkalmaz 0-ra állítja (egy másolat megmarad Options.ini.apex-backup néven).",
        ["Edge Smoothing"] = "Élsimítás",
        ["Depth Blur"] = "Mélységi elmosás",
        ["Ambient Occlusion"] = "Környezeti takarás",
        ["Water Reflections"] = "Víztükröződés",
        ["Runs the game on Vulkan: fewer stutters, and Apex Radiance is made and tested with it."] = "Vulkanon futtatja a játékot: kevesebb akadás, és az Apex Radiance ezzel készül és ezzel van tesztelve.",
        ["Apex Radiance works on its own, but it pairs well with Sims3SettingsSetter by sims3fiend: a frame rate limiter, fewer stutters and many extra game settings."] = "Az Apex Radiance önállóan is működik, de jól párosul a sims3fiend Sims3SettingsSetter moddal: képkockasebesség-korlát, kevesebb akadás és sok extra játékbeállítás.",
        ["Moves DXVK into its own 64-bit process: the game keeps about 600 MB more free memory for long sessions and big worlds."] = "A DXVK-t saját 64 bites folyamatba helyezi: a játéknak kb. 600 MB-tal több szabad memóriája marad hosszú játékokhoz és nagy világokhoz.",
        ["{0} has {1} = {2}, which Setup cannot read. Setup leaves the file as it is."] = "A(z) {0} fájlban {1} = {2}, amit a telepítő nem tud beolvasni. A telepítő a fájlt változatlanul hagyja.",
        // restore points (Repair or remove)
        ["Install {0}"] = "Telepítés: {0}",
        ["Restore points"] = "Visszaállítási pontok",
        ["Setup keeps a copy of every file it changes. Restoring a point undoes that change and every later one."] = "A telepítő megőrzi minden általa módosított fájl másolatát. Egy pont visszaállítása visszavonja azt a módosítást és minden későbbit.",
        ["Before: {0}"] = "Előtte: {0}",
        ["{0} · 1 file"] = "{0} · 1 fájl",
        ["{0} · {1} files"] = "{0} · {1} fájl",
        ["Restore"] = "Visszaállítás",
        ["No restore points yet. Setup makes one before every change."] = "Még nincs visszaállítási pont. A telepítő minden módosítás előtt létrehoz egyet.",
        ["Restore this point?"] = "Visszaállítod ezt a pontot?",
        ["Setup puts back the files as they were before this change, and undoes every change made after it. Close the game first."] = "A telepítő visszaállítja a fájlokat a módosítás előtti állapotukra, és visszavon minden későbbi módosítást. Előbb zárd be a játékot.",
        ["Setup could not put back {0}."] = "A telepítő nem tudta visszaállítani: {0}.",
        ["Changes undone."] = "Módosítások visszavonva.",
        ["Undo these changes"] = "Módosítások visszavonása",
        ["This restore point is gone."] = "Ez a visszaállítási pont már nem létezik.",
        ["Updates available"] = "Elérhető frissítések",
        ["Update now"] = "Frissítés most",
        ["{0} {1} → {2}"] = "{0} {1} → {2}",
        ["Removes MonoPatcher.asi and its package."] = "Eltávolítja a MonoPatcher.asi fájlt és a csomagját.",
        ["Smooth Patch's TS3Patch.asi patches the same game code as Sims3SettingsSetter's Smooth Patch. Setup moves it to the backup folder (S3SS does the same job)."] =
            "A Smooth Patch TS3Patch.asi fájlja ugyanazt a játékkódot módosítja, mint a Sims3SettingsSetter Smooth Patch funkciója. A telepítő áthelyezi a biztonsági mentések mappájába (az S3SS ugyanezt a feladatot látja el).",
        ["simler90's Gameplay Core Mod and NRaas StoryProgression replace the same game code. Keep one of them."] =
            "A simler90-féle Gameplay Core Mod és az NRaas StoryProgression ugyanazt a játékkódot cseréli le. Csak az egyiket tartsd meg.",
        ["MonoPatcher needs the Microsoft Visual C++ Redistributable (x86); it is not installed."] =
            "A MonoPatcherhez kell a Microsoft Visual C++ Redistributable (x86), de nincs telepítve.",
        ["Download on Nexus Mods"] = "Letöltés a Nexus Modsról",
        ["Choose the downloaded file"] = "Letöltött fájl kiválasztása",
        ["Ready to install"] = "Telepítésre kész",
        ["Waiting for your download in {0}…"] = "Várakozás a letöltésre itt: {0}…",
        ["Not the Performance Patch"] = "Ez nem a Performance Patch",
        ["{0} has no Sims3Performance.asi. If the download is a .7z or .rar, extract it and choose Sims3Performance.asi."] =
            "A(z) {0} nem tartalmazza a Sims3Performance.asi fájlt. Ha a letöltés .7z vagy .rar, csomagold ki, és a Sims3Performance.asi fájlt válaszd.",
        ["Removes Sims3Performance.asi and its .ini."] =
            "Eltávolítja a Sims3Performance.asi fájlt és a hozzá tartozó .ini fájlt.",
        ["Download Sims 3 Performance Patch from Nexus Mods first."] =
            "Előbb töltsd le a Sims 3 Performance Patch-et a Nexus Modsról.",
        ["Finds your The Sims 3 (Steam or EA app)."] = "Megkeresi a The Sims 3-at (Steam vagy EA app).",
        ["Installs Apex Radiance and the fixes you choose."] =
            "Telepíti az Apex Radiance-t és a kiválasztott javításokat.",
        ["Keeps a copy of every file it changes, so you can undo everything."] =
            "Minden módosított fájlról másolatot készít, így mindent visszavonhatsz.",
        ["Your Apex Radiance settings are always kept."] = "Az Apex Radiance beállításaid mindig megmaradnak.",
        ["No internet connection"] = "Nincs internetkapcsolat",
        ["Vulkan, DXVK (recommended)"] = "Vulkan, DXVK (ajánlott)",
        ["Vulkan with more memory (RTX Remix bridge, experimental)"] = "Vulkan több memóriával (RTX Remix bridge, kísérleti)",
        ["The RTX Remix bridge needs a graphics card with Vulkan 1.3 (found: {0})."] = "Az RTX Remix bridge-hez Vulkan 1.3-at támogató videokártya kell (talált: {0}).",
        ["Windows default (Direct3D 9)"] = "Windows alapértelmezett (Direct3D 9)",
        ["Fewer stutters; Apex Radiance is made and tested with it."] =
            "Kevesebb akadozás; az Apex Radiance ezzel készül, és ezzel tesztelik.",
        ["Keeps about 600 MB more memory free for big worlds. Needs a Vulkan graphics card. If the game misbehaves with it, choose DXVK."] =
            "Kb. 600 MB-tal több memóriát hagy szabadon a nagy világokhoz. Vulkant támogató videokártya kell hozzá. Ha a játék hibásan működik vele, válaszd a DXVK-t.",
        ["Works, but Apex Radiance is tested with DXVK."] = "Működik, de az Apex Radiance-t a DXVK-val tesztelik.",
        ["Your in-game graphics settings may reset once. Setup keeps a copy and puts it back if you switch back."] =
            "A játékon belüli grafikai beállításaid egyszer visszaállhatnak. A telepítő másolatot tart róluk, és visszaállítja őket, ha visszaváltasz.",
        ["Fixes for game and graphics settings."] = "Javítások a játék és a grafika beállításaihoz.",
        ["Fixes stutters, Sims getting stuck and crashes."] =
            "Javítja az akadozást, a beragadó Simeket és az összeomlásokat.",
        ["Needed only by some script mods; turn it on if a mod asks for it."] =
            "Csak néhány szkriptmodnak kell; kapcsold be, ha egy mod kéri.",
        ["Makes the game run faster. Needs a free Nexus Mods account; Setup installs it when your download finishes."] =
            "Gyorsabbá teszi a játékot. Ingyenes Nexus Mods-fiók kell hozzá; a telepítő a letöltés végeztével telepíti.",
        ["Puts back the graphics files you had before."] = "Visszaállítja a korábbi grafikai fájlokat.",
        ["Goes back to the game's default graphics. Your in-game graphics settings may reset once."] =
            "Visszaáll a játék alapértelmezett grafikájára. A játékon belüli grafikai beállításaid egyszer visszaállhatnak.",
        ["Language"] = "Nyelv",
        ["Automatic"] = "Automatikus",
        ["This file has no Sims3Performance.asi."] = "Ebben a fájlban nincs Sims3Performance.asi.",
        ["Dry run: Setup changes nothing."] = "Próbafuttatás: a telepítő semmit sem módosít.",
        ["Apex Radiance by loinyx. The RTX Remix bridge is NVIDIA's, ported to The Sims 3 by dalexlol and alexvixell35-stack. Sims3SettingsSetter is by sims3fiend, Sims 3 Modernization by rodneypantonial and Ultimate ASI Loader by ThirteenAG: Setup downloads them from their own GitHub releases."] =
            "Az Apex Radiance-t loinyx készítette. Az RTX Remix bridge az NVIDIA-é, a The Sims 3-ra dalexlol és alexvixell35-stack ültette át. A Sims3SettingsSetter sims3fiend, a Sims 3 Modernization rodneypantonial, az Ultimate ASI Loader ThirteenAG munkája: a telepítő a saját GitHub-kiadásaikból tölti le őket.",
        ["RTX Remix bridge: third-party notices"] = "RTX Remix bridge: harmadik felek közleményei",
        ["Read"] = "Megnyitás",
        ["Licence text"] = "Licencszöveg",
        ["OK"] = "OK",
        ["Cancel"] = "Mégse",
    };
}
