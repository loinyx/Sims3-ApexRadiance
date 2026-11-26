#pragma once
// Game fix: a save opened with "-worldFile:<save>.sims3" (the launcher, the restart host) keeps its name in the game, so
// Save As shows and the first Save does not ask for a new name (docs/features/save-name-fix.md).
//
// Only the main menu tells the game which save it is in (OptionsModel.SaveName); a start with the switch skips the menu.
// The game's own script is completed in memory, in the assembly's bytes, before the main menu state runs
// (save_name_patch.h). No address of the game is used, so every build takes the same path. Nothing runs without the switch.
// Always on, no menu row. Not found or another script there: off, logged.
namespace SaveNameFix {

// From the init thread, as early as possible. Once per process; logs the result.
void Start();

} // namespace SaveNameFix
