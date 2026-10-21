# Light through stairwells: validation

The feature is described in [features/night-lighting/stairwells.md](../features/night-lighting/stairwells.md). It
must:

- Stop a lamp's ray at a full wall of its own story up to the floor above, and let a ray over a half wall pass.
- Give the light through a floor opening a soft edge: full inside the opening, none under the slab, a partial share on
  both sides of the edge.
- Share ambient through an opening without darkening either room and without exceeding the brighter ambient.
- Light an upper wall facing the opening with the radiance of the surface its line of sight meets below, falling with
  cos x cos / r^2, and nothing on a surface in or below the slab's plane.
- Light the upper floor through the ceiling bounce, less with distance from the opening.
- Fill and adapt only rooms whose lamps all come from another story; keep adaptation between the game's value capped at
  1 and x2.
- Solve a room again when its fill moves by more than 25%, at most twice in 10 s.

## Automated tests

| Harness | Covers | Run | Needs |
|---|---|---|---|
| `tools/stairwell_bounce_test/` | `PenumbraShare`, `BorrowedAdapt`, `FillChanged`, `SurfaceKind`, `UpFlux`, the well lift, the second bounce on the house's geometry, room fill | `run.ps1` | MSVC x86 |
| `tools/opening_bounce_test/` | Shared ambient through openings: no opening, never darker, capped at the brighter, invalid normalisation ignored | `run.ps1` | MSVC x86 |
| `tools/roofed_openings_test/` | The lamp's story on the grid: stairwell veto cases (a ray just under the slab, a half wall) | `run.ps1` | MSVC x86 |
| `tools/development/check.ps1 -Suite Lighting` | The three pure lighting helpers above, as one suite (also run by CI) | `check.ps1 -Suite Lighting` | MSVC x86 |

These harnesses test the pure functions and the grid walk. They do not run the game's solve or show the result on
screen.

## Latest results

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| 2026-10-20 | Working tree on `c22e8d7` | `tools/stairwell_bounce_test/` | 53 checks passed; the upper floor at irradiance 0.5: 0.0094 beside the opening, 0.0019 6 m away | None (pure) |
| 2026-10-20 | Working tree on `c22e8d7` | `tools/opening_bounce_test/` | 11 checks passed; landing ambient (0.0068 0.0068 0.0068) to (0.0121 0.0111 0.0092) | None (pure) |
| 2026-10-20 | Working tree on `c22e8d7` | `tools/roofed_openings_test/` | 196 checks passed | None (pure) |

The developer and public x86 builds of the same tree compile without warnings.

## In-game test plan

1. A lamp under a stairwell at night. **Expected:** the walls above the opening are lit, and the edge of the light on
   the floor below is soft.
2. A lamp in a closed room next to the stairwell, hanging just under the slab. **Expected:** no light from it on the
   walls of the story above.
3. A sconce over the well switched off. **Expected:** the floor below goes dark at once, with no remaining fill.
4. A third story lit only from below. **Expected:** the walls are brighter than the floor, and the room is at most twice
   as bright as the game's normalisation would leave it.
5. A room lit only through a door or window. **Expected:** unchanged from 2.13.

## Confirmed in game

- Nothing yet.

## Open checks

- The appearance in game of every step.
- Whether the shared ambient and the second bounce count the same light twice.
- Lamp visibility in the second bounce (the lamps below are evaluated without Apex's tests).
- The precision of the line of sight through the well.
- The cost on large houses with several stairwells.
- Whether the fill check can resend a room repeatedly.
