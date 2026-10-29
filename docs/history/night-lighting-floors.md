# Floors: history

Chronological record of investigations and design decisions. Newest entries at the bottom. The current behaviour is
described in [features/night-lighting/floors.md](../features/night-lighting/floors.md).

### 2026-09-25: winter floors and the pool edge

**Context:** snowy lot floor tiles (m08) took only the lot map; the curved pool edge (m66) was missed.

**Finding:** an exact-byte class for the floor VS (`kFloorVs`) did not match the pool-edge VS.

**Outcome:** the `IsFloorVs` pattern (accepts only `kFloorVs` and the pool-edge VS in the offline test) and `PatchFloor`.
Confirmed patched in game (m62).

### 2026-09-25: summer outdoor floors (v5.4)

**Context:** m61: a summer floor next to a street lamp was nearly black (floor map mean 0.002). Before the `FloorAtlas`
class existed, these floors kept only the faint lot map, because `RecordWorldChunk` fails on them (ground report 4.3).

**Finding:** many ExteriorFloors VS already use TEXCOORD7, so the VS copy must use the first free TEXCOORD from 7.

**Outcome:** `PatchBakedAtlasPs` with `floor_atlas_table.h` (261 entries) and the VS copy.

### 2026-09-25: per-pixel floor lamps (PASSO3 increment 2), not implemented

**Context:** per-pixel floor lamps with HLSL replicas of `1788C5B6` and `9BF0A41E` were planned.

**Finding:** the critique required per-variant constant lists, `c12.x` and `c12.y` handled separately in the Jacobian, and
the per-tile offsets in `fn_006aabe0.c:111-126`. Floors seen only in winter go through VS `9B6DB72A`, where the per-pixel
plan's floor path would have done nothing (critique F7).

**Outcome:** replaced by the pattern approach; the items above apply if the idea is revived. The roadmap kept it as
phase 2, increment 2.

### 2026-09-27: standalone baseline

**Context:** the standalone started from v0.1.0 (`b84d5f1`) with `PatchFloor` and `PatchBakedAtlasPs` (261 entries).

**Finding:** the `scaleConst` / `LampScaleAfter` detection had been added after v0.1.0 in the combined build and fed only
the combined build's HDR lamp gain (removed, see [removed-features.md](../removed-features.md)). The combined build's
Native HDR also had an interior-floor tanh-curve rule (LightingTweaks), removed with it.

**Outcome:** `scaleConst` was later reused for *Ground brightness* (`GroundGain`), which is its role in the current code.

### 2026-10-23: square steps of light on a porch

**Context:** with *Indoor light adds to the ground*, an F8 and two light captures of a porch deck 0.75 m over the ground
showed square steps of light on the deck and a dark square beside a pillar.

**Finding:** the floor copies that add the indoor light (`8530732`) gate it by the height over the lot's ground, but a
floor draw cannot tell story 0's floor map from a higher story's. Above story 0 the map's alpha is the walls' mask (1
outside them): the deck passed the ground gate, read that 1 as indoor light and dropped the atlas texel by texel.

**Outcome:** the floors keep the plain patch (`kFloorIndoorLight` off, `40f351e`); the lot pass and the snowy lot passes,
which draw story 0 only, still add the indoor light. A review fix then keeps alpha 0 under every story-0 floor
tile, so the floors' `1 - alpha` atlas weight no longer takes the garden lamps' light off patios and paths.

### 2026-10-29: weather variants of the winter floors

**Context:** a patio compared in dry weather and in rain at night.

**Finding:** 96 of the 104 pixel shaders drawn with the winter floor vertex shaders in the precomp are weather variants
that `PatchFloor`'s pattern did not take: their map is read from another input or sampler and reaches the lamp scale
through the usual factor or directly. The
game drew them at its own lamp scale, and rain turned the patio 3.8 times brighter. Two of them use every sampler.

**Outcome:** `PatchFloor` takes the weather layouts; the two with no free sampler keep the game's bytes and take only the
floor's lamp-scale gain (`91af563`). `tools/floor_shade_check` now fails if any winter floor pixel shader of the precomp
is left unpatched. Not yet checked in game.
