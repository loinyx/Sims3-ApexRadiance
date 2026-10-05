# Light between stories: validation

The feature is described in [features/night-lighting/level-light-share.md](../features/night-lighting/level-light-share.md).
It must:

- Give every story's room 0 the outdoor lamps of every other story, with the weight each lamp has on its own story.
- Never let a borrowed lamp pass a wall of its own story or of a story in between, and never touch the room's own lamps.
- Light an indoor room with a lamp of another story only through an opening in every floor the real ray crosses; fail
  closed when a floor, manager, room or crossing order cannot be read.
- Keep native light range and attenuation; add no artificial brightness.
- Give the rooms of one atrium the same ambient colour and normalisation, and meet the walls above and below a floor line
  on the same light.
- Send only the affected rooms to gather again, with no re-gather loop, and leave rooms whose lamps did not change at
  rest on a floor switch.

## Automated tests

| Harness | Covers | Run | Needs |
|---|---|---|---|
| [`tools/terrain_lighting_test/run_indoor_stories_checks.ps1`](../../tools/terrain_lighting_test/run_indoor_stories_checks.ps1) (`indoor_stories_fixture.cpp`) | Extracts the production `IndoorBoundaryPass`, `IndoorPassImpl` and `BasisLightHook` and runs them on mock game structures: all story pairs, open shafts, every solid intermediate floor, wall blocking, wall-mode restoration, tile heights, unavailable data, ghost rows and split-level crossings, the raised-room exterior wall veto, partial basis transmission. Optional `-ReferenceSource` compiles an older `IndoorPassImpl` to compare adjacent-story results | `run_indoor_stories_checks.ps1 [-OutDir ...] [-ReferenceSource ...]` | MSVC x86 |
| [`tools/room_structure_test/run.ps1`](../../tools/room_structure_test/run.ps1) (`check.cpp`) | Extracts `NoteRoomStructure`: a new room schedules a refresh, LOD or repeated gathers do not, roof and wall changes do, repeated changes coalesce | `run.ps1` | MSVC x86 |
| [`tools/room_ambient_test`](../../tools/room_ambient_test/README.md) (`room_ambient_test.cpp`) | Room ambient and scheduling policy (`room_ambient_policy.h`): group deadlines, first dispatch, overdue updates, tick wrap, window recheck timing (0 / 2 s / 6 s, bounded, wrap), lamp refresh delay (120 / 700 ms), gather ordering, floor priority, structure and floor-edit timing | `run.ps1` | MSVC x86 |
| [`tools/basis_test/basis_test.cpp`](../../tools/basis_test/basis_test.cpp) | `PatchBasisSmooth` / `PatchIndoorBasis` on a captured pixel shader, disassembled with d3dcompiler_47 | `basis_test <ps.bin> [lmSampler]` | d3dcompiler_47, a captured shader |

These harnesses do not validate the game's room topology, lamp falloff, visual convergence or performance. They read
only the repository and print to the console.

## Latest results

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| 2026-10-05 | PR #2 (`2584e01`) | Indoor stories fixture, raised-room wall veto | 18,992 extracted checks passed; the raised-wall fixture failed twice before the guard and passes after | None (mocks) |
| 2026-10-04 | PR #2, candidate `2.5.6-indoor-wall-guard-test` | Extracted indoor and basis tests | 30,890 passed | None |
| 2026-10-04 | same | Resource restoration and lot UV fallback tests | 13,682 passed | Native D3D9 |
| 2026-10-04 | same | Lot UV inversion and production ps_2_0 compilation and device acceptance | 128,512 passed | Native D3D9, RTX 4070 Ti SUPER |
| 2026-10-04 | PR #2, `2.5.6-indoor-stories-test2` | Indoor stories fixture | 6 regression assertions fail on test1 and pass on test2; adjacent-story results match the previous implementation | None |
| 2026-10-01 | `2.5.2-test005` | Group, startup, priority and cache checks with the extracted group publisher and cache forgetting function | 36 passed, plus 89 policy checks; extracted pending loop, stale-lamp visitor and Rooms at Night checks passed | None |
| 2026-10-01 | `2.5.2-test004` | Policy checks; extracted pending loop (4,500 ready entries behind 500 busy, cursor removal, rehash, empty queue); extracted stale-lamp visitor | 89 policy checks and both extracted harnesses passed | None |
| 2026-10-01 | `2.5.2-test001` | Policy checks (short and drag debounce, gather ordering, tick wrap) | 65 passed | None |
| 2026-10-01 | `2.5.2-window-sync-test` | Window recheck scheduling (delays, bounded completion, tick wrap); `LightEntryUpdate` signatures on the installed TS3W.exe | 58 checks passed; one match per signature, both resolving to `0x006C7BA0` (229 address ids, 0 mismatches) | None |
| 2026-10-01 | `2.5.2-room-sync-live-test` | Live group background (delta preservation, round trips, weighted merge, normalisation, zero brightness) | 51 passed | None |
| 2026-09-30 | combined build | Directional-map shader cap | 4 captured indoor-object shaders patched and valid | d3dcompiler |

## In-game test plan

1. Two-story house at night with a wall sconce on the upper outside wall near a corner and garden lamps. **Expected:** the
   wall below the sconce is lit continuously across the floor line; the lower side wall around the corner is not
   brighter than the upper one.
2. Developer mode, Developer page status *Stories*. **Expected:** `Active | outdoor lights carried to other stories: N |
   stories updated: N | walls of the light's story: 9/9 classes, N tests, N blocked` (plus *on another thread* or
   *failures* when non-zero). Log: `[LevelLightShare] Installed (walls of the light's story: 9 of 9 classes; ...)`.
3. F8 after 10 s with a still camera. **Expected:** the per-story room 0 lists hold the same lamps on every story,
   level-0 lamps twice; samples near each light show the game's wall test and Apex's factor.
4. F7 on both walls of the floor line. **Expected:** the light map in `s2` of both stories shows the lamp.
5. A red lamp next to a stairwell on the upper story. **Expected:** the wall of the story below is lit red where the lamp
   sees it through the opening, and nothing is lit through solid floor. F8 *STORIES INDOORS* lists a floor object per
   story, openings on the upper story and a gather line for the lower room. Turning *Indoor light between floors* off and
   on brings the light back within about a second.
6. Three-story open shaft with a lamp on the top story. **Expected:** the bottom story is lit through both openings;
   closing either floor blocks it.
7. A raised room (tower) on an intermediate story with a lamp on the story above, and furniture below behind its
   exterior wall. **Expected:** no lamp colour on the furniture behind the wall (F7 basis texel of the furniture shows no
   lamp colour).
8. An atrium wall next to a sconce, both stories in view, *Seamless walls between floors* on. **Expected:** no step at
   the floor line, except where a real floor strip separates the walls. The status reports moved samples and zero pieces
   left as the game has them.
9. Start a recording, switch the atrium lamps, save. **Expected:** `Wall seams.csv` holds rows of both rooms at the same
   positions and normals.
10. Enter a lot with openings at night, leave, enter again; restart the game and load the save. **Expected:** rooms are
    right without moving a lamp or pressing *Refresh the lighting*.
11. Switch floors several times with no lamp change. **Expected:** rooms keep their light; the recorder line *Rooms keep
    their light* shows marks kept.
12. Switch a lamp, drag a lamp, remove a floor tile, add a wall that closes a room, toggle a roof. **Expected:** the
    affected rooms on every story update within a second or two and no room keeps re-solving.
13. Repeat scenarios 1 and 5 with Sims3SettingsSetter Lighting Quality at 16 and 32 samples. **Expected:** same light;
    note the cost.

## Confirmed in game

- Outdoor sharing (parts 1 and 2) and the wall test of the lamp's story (part 3), combined build: confirmed by the
  maintainer as almost seamless after the first round and much better after the wall test.
- Floor pairing through the story's own floor object (build `e3fab9d4`): the walls above a double-height room lit at once.
- Seamless walls, first version: a very slight crease remained next to a sconce just under the line; the ghost-row blur
  addresses it.
- 2.5.3: Rooms at Night and connected-room behaviour reported working in maintainer tests (scenario-specific feedback).
- PR #2, indoor seam candidate: maintainer feedback validated the visual result before the wall guard candidate.
- Session `2026-10-04 16-58-09`, recording `18-11-40` (`2.5.6-indoor-stories-test2`): story 0 room 23, story 1 room 19
  and story 2 room 20 converge to identical ambient RGB after lamp edits; queued at 18:11:30.743, matching ambient at
  18:11:31.399. This measures queue-to-ambient convergence, not visible latency.
- Session `2026-10-01 15-26-42`: rooms 23/19/3/20 of lot `27D24720` change their merged ambient during a slider drag.

## Open checks

- Both leak paths fixed in the wall guard candidate (furniture behind walls through the directional maps, and the lot
  UV contraction on the ground) and the raised-room veto: gameplay appearance, DXVK and frame-time cost.
- The three-story open-shaft scene with multi-story openings.
- Visible seam between atrium walls where the ambient already agrees (the indoor wall inconsistency reported on test2);
  compare `Wall seams.csv` rows at identical positions and normals, remembering raw samples precede the blur and upload.
- Floor-edit refresh after 250 ms and structure-change refresh: response in game.
- Window activation recheck: whether a stale roof/topology classification needs more than the 0 / 2 / 6 s re-evaluation.
- Whether the floor objects of a lot rebuilt without floor calls are always seen (`LevelCtorHook` covers construction;
  unverified that the game rebuilds them that way).
- Cost of the per-point hooks in game, alone and with Sims3SettingsSetter Lighting Quality at high sample counts.
- An outdoor room 0 of story 3 sent to solve about every 50 ms for 3 s and never finishing (one F8, 2026-09-29): the
  journal fields `Q`/`H`, lot, camera story and flag `+0x19` should identify the source.
- Basements: untouched by design, no report yet.
