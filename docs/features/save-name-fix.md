# Save Name Fix

A game fix for a save opened straight from the launcher or by the restart host: the game knows which save it is in, so
Save As shows in the options menu and the first Save writes to that save without asking for a name.

## Status

| | |
|---|---|
| Availability | Released in 4.1.1 |
| Default | Always on when the game starts with `-worldFile:"<save>.sims3"`; no menu row |
| Menu | None |
| Configuration | None |
| Source | [`features/save_name_fix.{h,cpp}`](../../features/save_name_fix.cpp), [`features/save_name_patch.h`](../../features/save_name_patch.h) |

## The problem

The game keeps the name of the current save in `OptionsModel.SaveName` (`Sims3GameplaySystems.dll`). Only the main
menu's own code (`UI.dll`) sets it. With EA's `-worldFile` switch `MainMenuState.Startup` loads the save without showing
the menu, and the name stays empty. Then, from the decompiled scripts:

- `Save As` is left out of the options menu until the first save (the pucks of live mode, edit town and the map add it
  only when the name is not empty).
- The first `Save` asks for a name, with the world's name as the suggestion. When a save of that name exists and the
  player accepts replacing it, the game deletes that save (`GameUtils.DeleteSaveFile`) before it writes.

## How Apex Radiance solves it

The scripts run interpreted from the assembly's own bytes. `MainMenuState.Startup` holds a block that never runs (a
local set to false and never changed), next to the branch that loads a `.sims3`:

1. With `-worldFile:` and `.sims3` on the command line, a low-priority thread searches the process's private memory for
   the method's bytes (metadata tokens left free) from the start, every 100 ms, until 2 s after the last copy it
   completed, until a world is active, or for 5 minutes.
2. The tokens of the three calls it needs are read from `LiveModeState` in the same assembly, which makes the same
   calls. No match, two matches or tokens that disagree: nothing is written.
3. The dead block becomes `Responder.Instance.OptionsModel.SaveName = <save>`, and the jump at the end of the `.sims3`
   branch goes through it. The block is written first, then the jump (one 4-byte write).

This is what the menu does when a save is chosen there. The game's files are not changed, and no address of the game is
used.

## Settings

None.

## Compatibility and interactions

- A script mod that replaces `MainMenuState.Startup` keeps its own code: the bytes do not match, and the fix stays off.
- Serves the restart host ([restart-host.md](performance/restart-host.md)) and the launcher's Play on a save.

## Limitations

- The bytes must be completed before the game first runs the method. On the test machine they are found about 11 s
  after the start, some seconds before the save starts to load.
- Not run on the EA app build.

## Technical reference

- Log: `[SaveNameFix] On after <ms> (<copies> in pass <n>): ...`, then `Ended ...`; or a warning naming why it stays off.
- Method offsets and the replaced bytes: `save_name_patch.h`.

## Rejected approaches

- A script mod package that sets the name: one more file in every player's Mods folder, and it would need
  `Resource.cfg`.

## See also

- [Validation](../validation/save-name-fix.md)
