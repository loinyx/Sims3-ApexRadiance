# Smooth light maps: validation

Smooth light maps must read every room-lit shader's map as a B-spline and nothing else, keep each wall's taps in its own
atlas block, fall back to the game's shader where the device cannot run the copy, dither texels without changing their mean
or shimmering, and fade a lamp at the light cut without lighting anything the game would not. The feature is described in
[features/night-lighting/room-map-smoothing.md](../features/night-lighting/room-map-smoothing.md).

## Automated tests

| Harness | Covers | Run | Needs |
|---|---|---|---|
| `tools/room_map_smooth_check/` | The pattern over every precomp pixel shader (only room-lit techniques match; every InteriorWall, every InteriorFloor with a room map); each copy disassembles, has four taps for the read and is created by native D3D9 when the device's `PS20Caps` allow it; the paired vertex shaders' uv scale; GPU pixels of a synthetic room-lit shader against a CPU B-spline with and without the wall band clamp; captured shaders; the dither offset (range, mean, same texel same offset) | `run.ps1 -Precomp <Shaders_Win32.precomp> [-Captures <folder>]` | the game's precomp (read-only), a D3D9 device |
| `tools/i18n_check/` | The new row's translations | see the file header | none |

## Latest results

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| 2026-10-27 | `perf/address-space` working tree (floor read off the walls) | `tools/room_map_smooth_check -Precomp`; `tools/basis_shadow_check -Precomp` | 1666 and 262 checks, 0 failures: the 186 walls copies created natively; synthetic two rooms with a wall and a doorway: +0 green through the wall (was +63), the doorway within 1 LSB of the read without walls, GPU = CPU reference within 1 LSB. Not checked in game | Native D3D9 (offline device) |
| 2026-10-27 | `47ec493` (release 2.16.0 candidate; read of the own side, `804ddfb` weight guard) | `tools/room_map_smooth_check -Precomp`; `tools/roofed_openings_test` | 1096 checks, 0 failures (the window wall falls 0.0 levels after the wall, was 30.6 with the 4 x 4 bound; GPU = CPU reference within 1 LSB; the 27 ps_3_0 ExteriorWall shaders take the outdoor scale copy); roofed openings OK. Not checked in game after `7c3d641` | Native D3D9 (offline device) |
| 2026-10-26 | `eedcb23` (shadow edges smooth beside walls) | `tools/room_map_smooth_check -Precomp`; `tools/roofed_openings_test` (`WallFootChecks`) | 1094 and 1726 checks, 0 failures: the 186 copies and the 120 arch copies created natively; a hall within 2 texels of a lit courtyard's wall +124 levels of green with the plain B-spline and with the 2 x 2 bound, +2 now; a jamb's penumbra cut by up to 6.4 levels at texel lines with the 2 x 2 bound, 0 now; the floor map beside the glass door falls off along the wall at every texel (the green gone within 2 texels of the jamb's line). Not checked in game | native D3D9 / CPU harness |
| 2026-10-26 | `ae5a3aa` (the pixel's side of a wall) | `tools/room_map_smooth_check -Precomp` (the game's Shaders_Win32.precomp) | 1094 checks, 0 failures: the 186 copies and the 120 arch copies created natively; a hall's last half texel beside a lit courtyard +124 levels of green with the 2 x 2 bound, +2 with the pixel's side; the courtyard's own pixels, light sent out and bright room texels unchanged; the GPU within 1 LSB of the CPU reference. Not checked in game | native D3D9 |
| 2026-10-20 | Working tree on `c22e8d7` | `tools/room_map_smooth_check -Precomp` | 1084 checks passed; includes the 120 floor and wall copies with the arch variant (6 segments) created natively and a pixel within 1 LSB of the CPU formula | native D3D9 |
| 2026-10-14 | `release/2.13.0` (`2aefeb7`) | `tools/room_map_smooth_check -Precomp` (the game's Shaders_Win32.precomp) | 957 checks, 0 failures; pixels within 1 LSB of the CPU B-spline | native D3D9 |
| 2026-10-14 | feature/v2.12.3-rt | `tools/room_map_smooth_check` | 957 checks, 0 failures | native D3D9 |
| 2026-10-13 | smooth light maps commits on release/2.12.4-arriscada | `tools/room_map_smooth_check` | 972 checks, 0 failures: 186 of 8016 precomp pixel shaders match (InteriorFloor 48/58, InteriorWall 21/21, InteriorWallWith\* 33/33, FloorWith\* 104/104, Hideable 40/42, FloorTileCeiling 12/40; ExteriorWall, Ceiling, TerrainLight_Indoor\* 0), all 186 created natively; 24 paired vertex shaders (9 walls, 15 floors); pixels within 1 LSB of the CPU B-spline in both modes; 3 of 64 captured pixel shaders match | native D3D9 (PS 3.0, PS20Caps 32 temps, 512 slots) |
| 2026-10-27 | branch `perf/lot-streaming` | Offline re-render of a captured floor map (2.16.2 light capture, atlas 256 x 128) with the copy's read: the plain B-spline, the 2.16.0 masked read, and the masked read with roofed texels at alpha 5/255 or more | Masked read: 7.6% of the pixels off the plain read by more than 2/255 (up to 0.55), in texel-wide squares beside the brightest texels; with the floor: identical to the plain read inside the room | Offline CPU model |
| 2026-10-27 | 2.16.2-dev.3 (`c774c91`) | In game, the capture's room relit | The dark squares under the table, the plant and the floor lamp are gone (maintainer) | In game |
| 2026-10-27 | release 2.16.3 | Independent review of the fix | The thunk order and stack, ebx as the room at the pack call, every alpha reader and roofed-texel writer checked; the ebx check now gates only the alpha floor (it had turned the dither off on a mismatch) and the added ambient is stated as a few 8-bit levels at most on near-white texels | None |

## In-game test plan

2026-10-19: `tools/basis_shadow_check/run.ps1` passes 142 native D3D9 checks. All 56 matching precomp basis copies
create as pixel shaders. The unguarded patch exactly reproduces one distinct captured shader. Black/red boundary
fixtures show 320 and 448 spill channels before the guard and zero after it; a flat fixture differs by zero LSB.
Fixtures include an alpha-zero edge. These are filter tests, not proof that the stair scene is corrected.
The existing room-map regression passes 962 checks, including one captured match created natively.

1. A room at night with a ceiling lamp, *Smooth light maps* off then on. **Expected:** on, the floor and walls show no
   texel steps or diamond pattern; shadow edges of walls and furniture keep their place.
2. A lamp's circle on a large floor. **Expected:** no ring where the light ends; a fine grain at most, not visible at a
   normal distance.
3. Walls of two rooms stored above each other in the atlas (a light wall next to a dark one). **Expected:** no light of
   one at the top or bottom of the other.
4. Developer > Night Lighting status, *Smooth light maps* line. **Expected:** wall and floor draws counting; "caps",
   "unknown vertex shader" and "wall blocks off the grid" at 0; dither texels counting; soft cut points counting.
5. A lamp switched on and off twice. **Expected:** the room looks the same after each relight (no shimmer).

## Confirmed in game

- Nothing yet.

## Open checks

- 2026-10-19: `tools/room_surface_check/run.ps1` passes 212 checks. Seven native packed floor vertex shaders match,
  all confined to the floor techniques; changed packing constants are rejected. The captured arch floor shader
  matches the existing room-map patch and all three captured copies create natively. Forty-five precomp vertex
  shaders expose wall matrices. The captured wall resolves clip c0..c3 and world c4..c6, rather than the probe's older
  fixed c4/c8 assumption. The routing change and arch brightness continuity remain unverified in game.
  Relocated matrix rows are recovered, and a missing position row is rejected.
  The room-map regression passes 967 checks with both captured room-map shader families created natively; the basis
  shadow regression remains at 142 checks with zero failures.

- *Unverified:* every wall block starts on the 3n + 1 grid (the developer status counts the pieces off it).
- *Unverified:* the light between wall blocks packed side by side and at the edge of a room's floor area (1/6 bleed).
- *Unverified:* the game binds the room maps with the bilinear filter (a point filter keeps the game's read; counted).
- *Unverified:* a device with ps_2_x but without arbitrary swizzle or 32 temps (the copy is not used; counted as caps).
