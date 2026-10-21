# Stairs on the directional maps: validation

The feature is described in [features/night-lighting/stairs.md](../features/night-lighting/stairs.md). It must:

- Change nothing where the wall mask is empty.
- Take no light from a texel behind a full wall, in the directional maps and in the floor-map read, including within
  the wall's 6 cm thickness; keep the lit side unchanged.
- Fill a stair with its tile's room ambient x the floor map's alpha, as the floor does.
- Apply the floor's tone curve to the stair copy.
- Blend the stair's light with the story above by height, rising without a step; change nothing when the blend is off
  or the story above is unknown.
- Produce copies of the game's stair and object shaders that native D3D9 accepts.

## Automated tests

| Harness | Covers | Run | Needs |
|---|---|---|---|
| `tools/basis_shadow_check/` | `PatchBasisSmooth` with the wall mask, floor-map read, room fill, floor tone and story blend on the game's stair and object shaders and on fixture pixels | `run.ps1 -Precomp <Shaders_Win32.precomp> -Captures <folder>` | Native D3D9, the game's shader package |

## Latest results

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| 2026-10-20 | Working tree on `c22e8d7` | `tools/basis_shadow_check/` | 262 checks passed. 56 copies created; with the wall mask, floor read, room fill and floor tone 56; blended with the story above 10; refused the blend for samplers 28, for no normal 18 | Native D3D9 |

What these checks establish, from the development series (each re-run in the result above):

- An empty mask changes 0 LSB; red behind the wall goes from 100 to 0; the lit side does not darken.
- A pixel 4.7 cm inside the lit tile next to the wall takes the smaller texel; outside the band nothing changes.
- The room fill gives (5, 77, 5) where the game's cube gave (64, 115, 64).
- The tone curve takes (5, 77, 5) to (7, 110, 7), x1.44, as the formula gives.
- The story blend raises red by height as 0, 0, 31, 94, 156, 182 with no step; off, nothing changes.
- Moving the options into `BasisSmoothOptions` left the generated shaders identical byte for byte.

## In-game test plan

1. The last step of a stair against the wall of a lit red room. **Expected:** no red sliver on the step or its lip.
2. An object against a wall with a lit room behind it. **Expected:** it does not take that room's colour.
3. A stair with the floor's texture beside the floor, under the same light. **Expected:** the same colour.
4. A stair going down from the second story with a lamp on the second story. **Expected:** the top steps take the
   landing's light, with no dark band under the slab.
5. Developer F8. **Expected:** "walls in the directional maps: matched" greater than 0 and "not matched" 0.

## Confirmed in game

- The wall mask matched its story 8,277 times with no miss in one development session (the wrapper's `+0x30` offset is
  confirmed). The appearance of the fix is not yet confirmed.

## Open checks

- The appearance in game of every part.
- How the stair variants that refuse the story blend look at their top step.
