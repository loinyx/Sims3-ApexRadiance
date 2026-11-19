# Merge round 5 (MERGE-R5)

Branch `gi/merge-r5`, off `feature/realtime-gi` at merge round 4 (92d21db).

Marks: [V] verified by a run or the disassembly named beside it, [I] inferred, [O] open.

## Merged, in order

| Branch | Commit | Conflicts and how they were resolved |
|---|---|---|
| `gi/s2-06-objs` (holds `gi/s2-06`) | 355272e | `lot_light_bridge.cpp`: the list of patched caches is the union (round 4's list plus `g_roomMapGiWallPs`); `realtime_gi.cpp`: the per-frame update keeps the mesh caster block and `MeshFrame()`, then `UpdateRigs()`; `Stop` keeps both sides (S2-06's releases and rig clears, then the lamp index and mesh caster reset) |
| `gi/mc-4b` | 64d3e86 | none |
| `gi/r-g1` | 7254cb3 | none; its bridge patch stays 0023 |
| `gi/wp11` | 34e2ba9 | its bridge patch becomes `0024-bridge-gi-pixel-view.patch` (header `0024:`, `Applies after 0023.`); `README.md`: rows 0023 (R-G1) and 0024 (WP11); runner `selftest.ps1`: both blocks (rig counters, bridge view); the patch number in `gbuffer_patch.cpp` (comment and the "Bridge view off" line), the selftest line, `gbuffer-motion-smoke.json` and [WP11](WP11.md) now say 0024 |
| `docs/room-light-maps-sets` | d47b4ca | none (doc only: room texture sets are per story) |

## Fixes in this round

- Rig write (S2-06 item 8): the per-channel 1.0 clamp is gone. The write lands after the game's cap `0x006B92A0`, so
  `OnRigBind` applies the game's rule itself: the lamp slots from the start slot (0 for a room-mode rig, else 1:
  `0x006BBEBF`, `0x006BBCC7`) and the 4 vertex-light slots are scaled evenly by cap / max(cap, brightest channel of the
  sums along +-X, +-Y, +-Z), cap read each frame from `[0x011D0BA8]` (`GameAddr` `CapGlobal`). The ambient `+0x110` is
  never scaled. The game's lamp colours are kept per rig and restored when no scale is needed; a new gather wins
  (`GiIndirect::RigCapScale`) [V disassembly, gi_indirect_test].
- [XR-10](XR-10.md): bone hash (FNV-1 32 of the lowercased name, `0x00575430`, check values) and palette space (model
  space) from the disassembly, for the next work package.

## Tests (all from the `gi-merge-r5` worktree)

| Test | Result |
|---|---|
| `PATCH_ONLY=1 native/remix-bridge/build.cmd` on a fresh exact checkout (no `src\`) | 0001-0014, 0016-0024 applied in order, no fuzz [V] |
| Full `native/remix-bridge/build.cmd` (both halves) | built, exit 0 [V] |
| `test-gi.cmd`, `DXVK_X64` = a `native/dxvk` build: list_batch_check, on, compute, off, stock, lots, lots-full, dxvk, dxvk-compute | GI server tests passed [V] |
| `test-gi-pixel.cmd` off and on | passed, `views=2 viewFaults=1` [V] |
| `tools/gi_server_test/run.ps1 -Mesh -Game <game folder> -Bridge` (with `test-gi-mesh.cmd`) | all checks passed [V] |
| `tools/gi_indirect_test/run.ps1 -Precomp <Shaders_Win32.precomp>` | 102 checks, 0 failures (with the cap checks) [V] |
| `tools/rt_lamps_test/run.ps1` and `-Gi` | 680,496 and 680,342 checks, 0 failures [V] |
| `tools/ingame_runner/selftest.ps1` | 670 checks, 0 failures [V] |
| `ApexRadiance.sln` Release x86, Developer and Public (`APEX_NO_DEV_TOOLS`), `-m:2` | both succeed, no warnings [V] |
| Counter words: `views` / `viewFaults` (GiPixel words 11-12) read only by `gbuffer_patch.cpp` | [V] grep |

## Open
- In-game smokes (the game belongs to the test chain): `gi-indirect-smoke` with `indirect = "atlas"` (rigs > 0,
  `rigReads` grows, `readFaults=0`, no `Objects and Sims:` line), `mesh-casters-smoke` both variants plus the MC-4b
  lines, `gbuffer-motion-smoke -Variant bridge` (`views=` > 0, `viewFaults=0`), R-G1's `[Gi] Lamp N gathers:` lines.
- [O] The cap's run-time value. [I] A room-mode rig's fill light (slot 1) is added after the game's cap, so the game
  does not scale it; our scale counts slot 1 like the other lamp slots when our slots are written.
