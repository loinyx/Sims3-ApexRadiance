// Apex Radiance Setup texts in Greek (Ελληνικά). Fill only the entries between the braces, one per line:
//     ["<English key, exactly as in keys.json>"] = "<translation>",
// C# strings: write \" for a quote, \\ for a backslash, \n for a line break; every other character as itself (UTF-8).
// Keep each {0}, {1} of the key, product and file names as they are. A key without an entry shows in English; never
// leave a translation empty. The same key twice: the last one wins, and ApexSetup.Tests fails.
namespace ApexSetup;

public static partial class Strings
{
    static readonly Dictionary<string, string> El = new()
    {
        ["Welcome"] = "Καλώς ήρθες",
        ["Game"] = "Παιχνίδι",
        ["Components"] = "Στοιχεία",
        ["Install"] = "Εγκατάσταση",
        ["Done"] = "Τέλος",
        ["Repair or remove"] = "Επιδιόρθωση ή αφαίρεση",
        ["About and licences"] = "Σχετικά και άδειες",
        ["Back"] = "Πίσω",
        ["Next"] = "Επόμενο",
        ["Get started"] = "Έναρξη",
        ["Finish"] = "Τέλος",
        ["Close The Sims 3 to continue."] = "Κλείσε το The Sims 3 για να συνεχίσεις.",
        ["Setup needs the internet to download the files."] =
            "Η εγκατάσταση χρειάζεται Internet για τη λήψη των αρχείων.",
        ["Nothing changes until you click Install."] = "Τίποτα δεν αλλάζει μέχρι να πατήσεις Εγκατάσταση.",
        ["Lighting, picture and performance for The Sims 3."] = "Φωτισμός, εικόνα και επιδόσεις για το The Sims 3.",
        ["What Setup does"] = "Τι κάνει το πρόγραμμα εγκατάστασης",
        ["Setup can't reach GitHub"] = "Το πρόγραμμα εγκατάστασης δεν μπορεί να συνδεθεί στο GitHub",
        ["Check your internet connection, then try again."] = "Έλεγξε τη σύνδεσή σου στο Internet και δοκίμασε ξανά.",
        ["Try again"] = "Επανάληψη",
        ["Choose your game"] = "Διάλεξε το παιχνίδι σου",
        ["Setup found these installs. Pick the one you play."] =
            "Βρέθηκαν αυτές οι εγκαταστάσεις. Διάλεξε αυτή που παίζεις.",
        ["Setup found no install. Browse for the game folder."] =
            "Δεν βρέθηκε εγκατάσταση. Αναζήτησε τον φάκελο του παιχνιδιού.",
        ["Browse for another folder"] = "Αναζήτηση άλλου φακέλου",
        ["No game in this folder"] = "Δεν υπάρχει παιχνίδι σε αυτόν τον φάκελο",
        ["Pick the The Sims 3 folder, or its Game\\Bin folder (the one with TS3W.exe or TS3.exe)."] =
            "Διάλεξε τον φάκελο του The Sims 3 ή τον φάκελο Game\\Bin του (αυτόν με το TS3W.exe ή το TS3.exe).",
        ["Not installed"] = "Μη εγκατεστημένο",
        ["Installed"] = "Εγκατεστημένο",
        ["the game's own Direct3D 9"] = "το Direct3D 9 του ίδιου του παιχνιδιού",
        ["another d3d9.dll"] = "άλλο d3d9.dll",
        ["Required"] = "Απαιτείται",
        ["Installs {0} into Game\\Bin."] = "Εγκαθιστά την έκδοση {0} στο Game\\Bin.",
        ["Replaces the installed copy with {0}. Your settings stay."] =
            "Αντικαθιστά το εγκατεστημένο αντίγραφο με την έκδοση {0}. Οι ρυθμίσεις σου μένουν.",
        ["Updates {0} to {1}. Your settings stay."] = "Ενημερώνει από {0} σε {1}. Οι ρυθμίσεις σου μένουν.",
        ["Recommended with Apex"] = "Προτείνονται με το Apex",
        ["No updates available"] = "Δεν υπάρχουν ενημερώσεις",
        ["Everything in this game is on its latest version."] = "Όλα σε αυτό το παιχνίδι είναι στην τελευταία έκδοση.",
        ["Not available for this game version: it supports Steam 1.67.2 and EA app 1.69.47."] =
            "Δεν είναι διαθέσιμο για αυτή την έκδοση του παιχνιδιού: υποστηρίζει Steam 1.67.2 και EA app 1.69.47.",
        ["Wrong build"] = "Λάθος έκδοση",
        ["Update {0}"] = "Ενημέρωση {0}",
        ["Up to date"] = "Ενημερωμένο",
        ["Setup could not finish"] = "Η εγκατάσταση δεν ολοκληρώθηκε",
        ["Close The Sims 3 first."] = "Κλείσε πρώτα το The Sims 3.",
        ["You're all set"] = "Όλα έτοιμα",
        ["Start The Sims 3, load a world and press Ctrl+Shift+F11 to open the Apex Radiance menu."] =
            "Άνοιξε το The Sims 3, φόρτωσε έναν κόσμο και πάτησε Ctrl+Shift+F11 για να ανοίξεις το μενού του Apex Radiance.",
        ["Installed: {0} {1}"] = "Εγκαταστάθηκε: {0} {1}",
        ["Choose your game first."] = "Διάλεξε πρώτα το παιχνίδι σου.",
        ["Removes ApexRadiance.asi. Your settings in Documents stay."] =
            "Αφαιρεί το ApexRadiance.asi. Οι ρυθμίσεις σου στο Documents μένουν.",
        ["Remove"] = "Αφαίρεση",
        ["RTX Remix bridge"] = "RTX Remix bridge",
        ["Removes Sims3SettingsSetter.asi."] = "Αφαιρεί το Sims3SettingsSetter.asi.",
        ["Removes ts3native and ts3fixes.package."] = "Αφαιρεί το ts3native και το ts3fixes.package.",
        ["Repair or update"] = "Επιδιόρθωση ή ενημέρωση",
        ["Downloads and copies again everything installed here. Your settings stay."] =
            "Κατεβάζει και αντιγράφει ξανά ό,τι είναι εγκατεστημένο εδώ. Οι ρυθμίσεις σου μένουν.",
        ["Repair"] = "Επιδιόρθωση",
        ["Uninstall everything"] = "Απεγκατάσταση όλων",
        ["Removes what Setup installed, and Apex Radiance, and puts back every file Setup replaced."] =
            "Αφαιρεί ό,τι εγκατέστησε το πρόγραμμα εγκατάστασης και το Apex Radiance, και επαναφέρει κάθε αρχείο που αντικατέστησε.",
        ["Uninstall"] = "Απεγκατάσταση",
        ["Remove {0}?"] = "Να αφαιρεθεί το {0};",
        ["Close the game first. Setup removes the files and puts back the ones it replaced."] =
            "Κλείσε πρώτα το παιχνίδι. Το πρόγραμμα εγκατάστασης αφαιρεί τα αρχεία και επαναφέρει όσα αντικατέστησε.",
        ["{0} removed."] = "Το {0} αφαιρέθηκε.",
        ["Uninstall everything?"] = "Απεγκατάσταση όλων;",
        ["Setup removes Apex Radiance and everything it installed in this game, and puts back every file it replaced. Your settings in Documents stay."] =
            "Το πρόγραμμα εγκατάστασης αφαιρεί το Apex Radiance και ό,τι εγκατέστησε σε αυτό το παιχνίδι, και επαναφέρει κάθε αρχείο που αντικατέστησε. Οι ρυθμίσεις σου στο Documents μένουν.",
        ["Everything Setup installed was removed."] = "Ό,τι εγκατέστησε το πρόγραμμα εγκατάστασης αφαιρέθηκε.",
        ["Setup fixes this"] = "Το πρόγραμμα εγκατάστασης το διορθώνει",
        ["Read more"] = "Περισσότερα",
        ["Fixed."] = "Διορθώθηκε.",
        ["No incompatible settings found."] = "Δεν βρέθηκαν ασύμβατες ρυθμίσεις.",
        ["{0} in Game\\Bin loads a second copy of Apex Radiance."] =
            "Το {0} στο Game\\Bin φορτώνει δεύτερο αντίγραφο του Apex Radiance.",
        ["Two ASI loaders ({0}) load every mod twice. Keep one: remove all but {1}."] =
            "Δύο ASI loaders ({0}) φορτώνουν κάθε mod δύο φορές. Κράτα μόνο έναν: αφαίρεσε όλους τους άλλους εκτός από το {1}.",
        ["Sims 3 Modernization's ts3native is the build for the other edition and does nothing. Turn it on in Components to install the right one."] =
            "Το ts3native του Sims 3 Modernization είναι φτιαγμένο για την άλλη έκδοση του παιχνιδιού και δεν κάνει τίποτα. Ενεργοποίησέ το στα Στοιχεία για να εγκατασταθεί το σωστό.",
        ["Sims 3 Modernization has no build for this game version ({0}), so its ts3native does nothing. Turn it off in Components."] =
            "Το Sims 3 Modernization δεν έχει έκδοση για αυτή την έκδοση του παιχνιδιού ({0}), οπότε το ts3native του δεν κάνει τίποτα. Απενεργοποίησέ το στα Στοιχεία.",
        ["{0} in Game\\Bin is a second copy of {1}; both load. Setup moves it to the backup folder."] =
            "Το {0} στο Game\\Bin είναι δεύτερο αντίγραφο του {1}· φορτώνουν και τα δύο. Το πρόγραμμα εγκατάστασης το μετακινεί στον φάκελο αντιγράφων ασφαλείας.",
        ["Mods has {0} .package files but no Resource.cfg, so the game loads none of them."] =
            "Ο φάκελος Mods έχει {0} αρχεία .package αλλά κανένα Resource.cfg, οπότε το παιχνίδι δεν φορτώνει κανένα από αυτά.",
        ["{0} is the build for the other edition of the game; it makes the game show \"unofficial game modification\". Use the {1} build."] =
            "Το {0} είναι φτιαγμένο για την άλλη έκδοση του παιχνιδιού· κάνει το παιχνίδι να δείχνει \"unofficial game modification\". Χρησιμοποίησε την έκδοση {1}.",
        ["NRaas ErrorTrap has no build known to work on EA app 1.69.47."] =
            "Το NRaas ErrorTrap δεν έχει έκδοση που να λειτουργεί σίγουρα στο EA app 1.69.47.",
        ["{0} overrides game code Sims 3 Modernization also patches (not tested together)."] =
            "Το {0} αντικαθιστά κώδικα του παιχνιδιού που αλλάζει και το Sims 3 Modernization (δεν έχουν δοκιμαστεί μαζί).",
        ["{0} is an old build with Apex inside; while it is in Game\\Bin, Apex Radiance stays off. Setup moves it to the backup folder (install the official Sims3SettingsSetter in its place if you used it)."] =
            "Το {0} είναι παλιά έκδοση με ενσωματωμένο το Apex· όσο βρίσκεται στο Game\\Bin, το Apex Radiance μένει ανενεργό. Το πρόγραμμα εγκατάστασης το μετακινεί στον φάκελο αντιγράφων ασφαλείας (αν το χρησιμοποιούσες, εγκατάστησε στη θέση του το επίσημο Sims3SettingsSetter).",
        ["Sims3Performance's ShortWaitSpin and ThreadStackDiet clash with Sims 3 Modernization. Setup sets both to 0 (a copy is kept as Sims3Performance.ini.apex-backup)."] =
            "Τα ShortWaitSpin και ThreadStackDiet του Sims3Performance συγκρούονται με το Sims 3 Modernization. Το πρόγραμμα εγκατάστασης τα ορίζει και τα δύο σε 0 (κρατά αντίγραφο ως Sims3Performance.ini.apex-backup).",
        ["Sims3SettingsSetter's uncompressed textures stutter under the RTX Remix bridge. Setup turns them off (a copy is kept as S3SS.toml.apex-backup)."] =
            "Οι μη συμπιεσμένες υφές του Sims3SettingsSetter κάνουν το παιχνίδι να κολλάει με το RTX Remix bridge. Το πρόγραμμα εγκατάστασης τις απενεργοποιεί (κρατά αντίγραφο ως S3SS.toml.apex-backup).",
        ["{0} sets client.targetProcess = {1}, but this game is {2}: the bridge does not start."] =
            "Το {0} ορίζει client.targetProcess = {1}, αλλά αυτό το παιχνίδι είναι το {2}: το bridge δεν ξεκινά.",
        ["dxvk.conf lacks {0} or has other values: without them the game resets its graphics options or freezes on a resolution change under the bridge."] =
            "Στο dxvk.conf λείπει το {0} ή έχει άλλες τιμές: χωρίς αυτές, το παιχνίδι επαναφέρει τις επιλογές γραφικών του ή παγώνει όταν αλλάζεις ανάλυση με το bridge.",
        ["Mods\\Resource.cfg does not read {0}, so the game does not load it. Setup adds the Packages folder lines (a copy is kept as Resource.cfg.apex-backup)."] =
            "Το Mods\\Resource.cfg δεν διαβάζει το {0}, οπότε το παιχνίδι δεν το φορτώνει. Το πρόγραμμα εγκατάστασης προσθέτει τις γραμμές του φακέλου Packages (κρατά αντίγραφο ως Resource.cfg.apex-backup).",
        ["Fix settings"] = "Διόρθωση ρυθμίσεων",
        ["Everything is installed and up to date."] = "Όλα είναι εγκατεστημένα και ενημερωμένα.",
        ["Will be removed"] = "Θα αφαιρεθεί",
        ["Keep"] = "Διατήρηση",
        ["Nothing to apply"] = "Τίποτα για εφαρμογή",
        ["Preview"] = "Προεπισκόπηση",
        ["This is a preview: nothing was installed."] = "Αυτή είναι προεπισκόπηση: δεν εγκαταστάθηκε τίποτα.",
        ["Setup is applying changes"] = "Γίνεται εφαρμογή αλλαγών",
        ["Wait until it finishes before closing."] = "Περίμενε να τελειώσει πριν κλείσεις.",
        ["Checking the latest versions…"] = "Έλεγχος των τελευταίων εκδόσεων…",
        ["Checking for updates…"] = "Αναζήτηση ενημερώσεων…",
        ["Close The Sims 3 to update."] = "Κλείσε το The Sims 3 για να γίνει η ενημέρωση.",
        ["Update now installs them."] = "Πάτησε Ενημέρωση τώρα για να εγκατασταθούν.",
        ["Graphics"] = "Γραφικά",
        ["Checked at {0}."] = "Έλεγχος στις {0}.",
        ["Test build {0}: kept as it is."] = "Δοκιμαστική έκδοση {0}: μένει ως έχει.",
        ["Repair or remove sets the game right."] = "Η Επιδιόρθωση ή αφαίρεση επαναφέρει το παιχνίδι.",
        ["Changes applied"] = "Οι αλλαγές εφαρμόστηκαν",
        ["Removed: {0}"] = "Αφαιρέθηκε: {0}",
        ["The Sims 3 · {0}"] = "The Sims 3 · {0}",
        ["{0} → {1}"] = "{0} → {1}",
        ["{0} · up to date"] = "{0} · ενημερωμένο",
        ["Everything is in place."] = "Όλα είναι εντάξει.",
        ["Everything is in place (checked at {0})."] = "Όλα είναι εντάξει (έλεγχος στις {0}).",
        ["Check installation"] = "Έλεγχος εγκατάστασης",
        ["Change components"] = "Αλλαγή στοιχείων",
        ["Apex Radiance is not installed in this game."] =
            "Το Apex Radiance δεν είναι εγκατεστημένο σε αυτό το παιχνίδι.",
        ["{0} · test build"] = "{0} · δοκιμαστική έκδοση",
        ["Install the official {0}"] = "Εγκατάσταση επίσημης έκδοσης {0}",
        ["No ASI loader in Game\\Bin: Apex Radiance and the other .asi mods do not load."] =
            "Δεν υπάρχει ASI loader στο Game\\Bin: το Apex Radiance και τα άλλα mods .asi δεν φορτώνονται.",
        ["{0} is missing: the RTX Remix bridge does not start."] = "Λείπει το {0}: το RTX Remix bridge δεν ξεκινά.",
        ["Apply 1 change"] = "Εφαρμογή 1 αλλαγής",
        ["Apply {0} changes"] = "Εφαρμογή {0} αλλαγών",
        ["{0} is installed and up to date: nothing to change."] =
            "Η έκδοση {0} είναι εγκατεστημένη και ενημερωμένη: δεν χρειάζεται αλλαγή.",
        ["Keep my d3d9.dll"] = "Διατήρηση του δικού μου d3d9.dll",
        ["Your d3d9.dll stays as it is."] = "Το d3d9.dll σου μένει ως έχει.",
        ["Applying changes"] = "Εφαρμογή αλλαγών",
        ["Remove {0}"] = "Αφαίρεση {0}",
        ["Fix incompatible settings"] = "Διόρθωση ασύμβατων ρυθμίσεων",
        // suggestions: what Apex Radiance recommends in game (Compat.Suggestions); the game's own texts as the menu translates them
        ["Suggested for Apex Radiance"] = "Προτείνεται για το Apex Radiance",
        ["Apply"] = "Εφαρμογή",
        ["Applied."] = "Εφαρμόστηκε.",
        ["Apply all suggestions"] = "Εφαρμογή όλων των προτάσεων",
        ["Apply suggestion: {0}"] = "Εφαρμογή πρότασης: {0}",
        ["Two copies of the mod"] = "Δύο αντίγραφα του mod",
        ["{0} in Game\\Bin is another copy of Apex Radiance; only one copy runs. Apply moves it to the backup folder."] = "Το {0} στο Game\\Bin είναι άλλο ένα αντίγραφο του Apex Radiance· τρέχει μόνο ένα αντίγραφο. Η Εφαρμογή το μετακινεί στον φάκελο αντιγράφων ασφαλείας.",
        ["Turn off the game's Edge Smoothing"] = "Απενεργοποίηση της εξομάλυνσης άκρων του παιχνιδιού",
        ["Options.ini has edgesmoothing = {0}. The Sims 3's Edge Smoothing prevents these enabled Apex effects from working: {1}. Apply sets it to 0 (a copy is kept as Options.ini.apex-backup)."] = "Το Options.ini έχει edgesmoothing = {0}. Η εξομάλυνση άκρων του The Sims 3 εμποδίζει αυτά τα ενεργά εφέ του Apex να λειτουργήσουν: {1}. Η Εφαρμογή την ορίζει σε 0 (κρατά αντίγραφο ως Options.ini.apex-backup).",
        ["Edge Smoothing"] = "Εξομάλυνση Άκρων",
        ["Depth Blur"] = "Θόλωση Βάθους",
        ["Ambient Occlusion"] = "Ambient Occlusion",
        ["Water Reflections"] = "Αντανακλάσεις Νερού",
        ["Runs the game on Vulkan: fewer stutters, and Apex Radiance is made and tested with it."] = "Τρέχει το παιχνίδι σε Vulkan: λιγότερα κολλήματα, και το Apex Radiance φτιάχνεται και δοκιμάζεται με αυτό.",
        ["Apex Radiance works on its own, but it pairs well with Sims3SettingsSetter by sims3fiend: a frame rate limiter, fewer stutters and many extra game settings."] = "Το Apex Radiance λειτουργεί μόνο του, αλλά ταιριάζει καλά με το Sims3SettingsSetter του sims3fiend: περιοριστής καρέ, λιγότερα κολλήματα και πολλές επιπλέον ρυθμίσεις παιχνιδιού.",
        ["Moves DXVK into its own 64-bit process: the game keeps about 600 MB more free memory for long sessions and big worlds."] = "Μεταφέρει το DXVK σε δική του διεργασία 64 bit: το παιχνίδι κρατά περίπου 600 MB περισσότερη ελεύθερη μνήμη για μεγάλες συνεδρίες και μεγάλους κόσμους.",
        ["{0} has {1} = {2}, which Setup cannot read. Setup leaves the file as it is."] = "Το {0} έχει {1} = {2}, που το πρόγραμμα εγκατάστασης δεν μπορεί να διαβάσει. Το πρόγραμμα εγκατάστασης αφήνει το αρχείο ως έχει.",
        // restore points (Repair or remove)
        ["Install {0}"] = "Εγκατάσταση {0}",
        ["Restore points"] = "Σημεία επαναφοράς",
        ["Setup keeps a copy of every file it changes. Restoring a point undoes that change and every later one."] = "Το πρόγραμμα εγκατάστασης κρατά αντίγραφο κάθε αρχείου που αλλάζει. Η επαναφορά ενός σημείου αναιρεί εκείνη την αλλαγή και όλες τις επόμενες.",
        ["Before: {0}"] = "Πριν από: {0}",
        ["{0} · 1 file"] = "{0} · 1 αρχείο",
        ["{0} · {1} files"] = "{0} · {1} αρχεία",
        ["Restore"] = "Επαναφορά",
        ["No restore points yet. Setup makes one before every change."] = "Δεν υπάρχουν ακόμη σημεία επαναφοράς. Το πρόγραμμα εγκατάστασης δημιουργεί ένα πριν από κάθε αλλαγή.",
        ["Restore this point?"] = "Επαναφορά αυτού του σημείου;",
        ["Setup puts back the files as they were before this change, and undoes every change made after it. Close the game first."] = "Το πρόγραμμα εγκατάστασης επαναφέρει τα αρχεία όπως ήταν πριν από αυτή την αλλαγή και αναιρεί όλες τις αλλαγές που έγιναν μετά. Κλείσε πρώτα το παιχνίδι.",
        ["Setup could not put back {0}."] = "Το πρόγραμμα εγκατάστασης δεν μπόρεσε να επαναφέρει {0}.",
        ["Changes undone."] = "Οι αλλαγές αναιρέθηκαν.",
        ["Undo these changes"] = "Αναίρεση αυτών των αλλαγών",
        ["This restore point is gone."] = "Αυτό το σημείο επαναφοράς δεν υπάρχει πια.",
        ["Updates available"] = "Υπάρχουν ενημερώσεις",
        ["Update now"] = "Ενημέρωση τώρα",
        ["{0} {1} → {2}"] = "{0} {1} → {2}",
        ["Removes MonoPatcher.asi and its package."] = "Αφαιρεί το MonoPatcher.asi και το πακέτο του.",
        ["Smooth Patch's TS3Patch.asi patches the same game code as Sims3SettingsSetter's Smooth Patch. Setup moves it to the backup folder (S3SS does the same job)."] =
            "Το TS3Patch.asi του Smooth Patch αλλάζει τον ίδιο κώδικα του παιχνιδιού με το Smooth Patch του Sims3SettingsSetter. Το πρόγραμμα εγκατάστασης το μετακινεί στον φάκελο αντιγράφων ασφαλείας (το S3SS κάνει την ίδια δουλειά).",
        ["simler90's Gameplay Core Mod and NRaas StoryProgression replace the same game code. Keep one of them."] =
            "Το Gameplay Core Mod του simler90 και το NRaas StoryProgression αντικαθιστούν τον ίδιο κώδικα του παιχνιδιού. Κράτα μόνο ένα από τα δύο.",
        ["MonoPatcher needs the Microsoft Visual C++ Redistributable (x86); it is not installed."] =
            "Το MonoPatcher χρειάζεται το Microsoft Visual C++ Redistributable (x86), που δεν είναι εγκατεστημένο.",
        ["Download on Nexus Mods"] = "Λήψη από το Nexus Mods",
        ["Choose the downloaded file"] = "Επιλογή αρχείου",
        ["Ready to install"] = "Έτοιμο για εγκατάσταση",
        ["Waiting for your download in {0}…"] = "Αναμονή για τη λήψη σου στο {0}…",
        ["Not the Performance Patch"] = "Δεν είναι το Performance Patch",
        ["{0} has no Sims3Performance.asi. If the download is a .7z or .rar, extract it and choose Sims3Performance.asi."] =
            "Το {0} δεν έχει Sims3Performance.asi. Αν η λήψη είναι .7z ή .rar, κάνε αποσυμπίεση και διάλεξε το Sims3Performance.asi.",
        ["Removes Sims3Performance.asi and its .ini."] = "Αφαιρεί το Sims3Performance.asi και το .ini του.",
        ["Download Sims 3 Performance Patch from Nexus Mods first."] =
            "Κατέβασε πρώτα το Sims 3 Performance Patch από το Nexus Mods.",
        ["Finds your The Sims 3 (Steam or EA app)."] = "Βρίσκει το The Sims 3 σου (Steam ή EA app).",
        ["Installs Apex Radiance and the fixes you choose."] =
            "Εγκαθιστά το Apex Radiance και τις διορθώσεις που διαλέγεις.",
        ["Keeps a copy of every file it changes, so you can undo everything."] =
            "Κρατά αντίγραφο κάθε αρχείου που αλλάζει, ώστε να μπορείς να τα αναιρέσεις όλα.",
        ["Your Apex Radiance settings are always kept."] = "Οι ρυθμίσεις σου στο Apex Radiance διατηρούνται πάντα.",
        ["No internet connection"] = "Δεν υπάρχει σύνδεση στο Internet",
        ["Vulkan, DXVK (recommended)"] = "Vulkan, DXVK (προτείνεται)",
        ["Vulkan with more memory (RTX Remix bridge, experimental)"] = "Vulkan με περισσότερη μνήμη (RTX Remix bridge, πειραματικό)",
        ["Windows default (Direct3D 9)"] = "Προεπιλογή των Windows (Direct3D 9)",
        ["Fewer stutters; Apex Radiance is made and tested with it."] =
            "Λιγότερα κολλήματα· το Apex Radiance φτιάχνεται και δοκιμάζεται με αυτό.",
        ["Keeps about 600 MB more memory free for big worlds. Needs a Vulkan graphics card. If the game misbehaves with it, choose DXVK."] =
            "Κρατά περίπου 600 MB περισσότερη ελεύθερη μνήμη για μεγάλους κόσμους. Χρειάζεται κάρτα γραφικών με Vulkan. Αν το παιχνίδι δεν λειτουργεί σωστά με αυτό, διάλεξε DXVK.",
        ["Works, but Apex Radiance is tested with DXVK."] = "Λειτουργεί, αλλά το Apex Radiance δοκιμάζεται με το DXVK.",
        ["Your in-game graphics settings may reset once. Setup keeps a copy and puts it back if you switch back."] =
            "Οι ρυθμίσεις γραφικών του παιχνιδιού μπορεί να επανέλθουν μία φορά. Το πρόγραμμα εγκατάστασης κρατά αντίγραφο και το επαναφέρει αν γυρίσεις πίσω.",
        ["Fixes for game and graphics settings."] = "Διορθώσεις για τις ρυθμίσεις του παιχνιδιού και των γραφικών.",
        ["Fixes stutters, Sims getting stuck and crashes."] =
            "Διορθώνει κολλήματα, Sims που κολλάνε και κρασαρίσματα.",
        ["Needed only by some script mods; turn it on if a mod asks for it."] =
            "Χρειάζεται μόνο σε ορισμένα mods script· ενεργοποίησέ το αν σου το ζητήσει κάποιο mod.",
        ["Makes the game run faster. Needs a free Nexus Mods account; Setup installs it when your download finishes."] =
            "Κάνει το παιχνίδι πιο γρήγορο. Χρειάζεται δωρεάν λογαριασμό στο Nexus Mods· το πρόγραμμα εγκατάστασης το εγκαθιστά όταν τελειώσει η λήψη σου.",
        ["Puts back the graphics files you had before."] = "Επαναφέρει τα αρχεία γραφικών που είχες πριν.",
        ["Goes back to the game's default graphics. Your in-game graphics settings may reset once."] =
            "Επιστρέφει στα προεπιλεγμένα γραφικά του παιχνιδιού. Οι ρυθμίσεις γραφικών του παιχνιδιού μπορεί να επανέλθουν μία φορά.",
        ["Language"] = "Γλώσσα",
        ["Automatic"] = "Αυτόματα",
        ["This file has no Sims3Performance.asi."] = "Αυτό το αρχείο δεν έχει Sims3Performance.asi.",
        ["Dry run: Setup changes nothing."] = "Δοκιμαστική εκτέλεση: το πρόγραμμα εγκατάστασης δεν αλλάζει τίποτα.",
        ["Apex Radiance by loinyx. The RTX Remix bridge is NVIDIA's, ported to The Sims 3 by dalexlol and alexvixell35-stack. Sims3SettingsSetter is by sims3fiend, Sims 3 Modernization by rodneypantonial and Ultimate ASI Loader by ThirteenAG: Setup downloads them from their own GitHub releases."] =
            "Apex Radiance από τον loinyx. Το RTX Remix bridge είναι της NVIDIA, μεταφερμένο στο The Sims 3 από τους dalexlol και alexvixell35-stack. Το Sims3SettingsSetter είναι του sims3fiend, το Sims 3 Modernization του rodneypantonial και το Ultimate ASI Loader του ThirteenAG: το πρόγραμμα εγκατάστασης τα κατεβάζει από τις δικές τους εκδόσεις στο GitHub.",
        ["RTX Remix bridge: third-party notices"] = "RTX Remix bridge: σημειώσεις τρίτων",
        ["Read"] = "Ανάγνωση",
        ["Licence text"] = "Κείμενο άδειας",
        ["OK"] = "OK",
        ["Cancel"] = "Ακύρωση",
    };
}
