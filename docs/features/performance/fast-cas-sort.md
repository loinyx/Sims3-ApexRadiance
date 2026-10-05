# Faster Sim Building

Fewer hitches when Sims are edited in Create a Sim or change outfits. When the game builds a Sim it sorts the triangles
of hair and other see-through layers with a slow test of every triangle against every point of the mesh. Faster Sim
Building does the same sort many times faster, with exactly the same result.

## Status

| | |
|---|---|
| Availability | Released in 2.3.0; on by default since 2.5.5 |
| Default | On |
| Menu | System > Performance > Textures and Sims > *Faster Sim building* |
| Configuration | `[patches.FastCasSort]` in `ApexRadiance.toml` |
| Source | [`features/cas_tri_sort.{h,cpp}`](../../../features/cas_tri_sort.cpp), [`features/fast_cas.{h,cpp}`](../../../features/fast_cas.cpp), [`patches/performance_patches.cpp`](../../../patches/performance_patches.cpp) |

## The problem

The CAS model builder's triangle sort (0x005D1960, "CAS/ModelBuilder/TriangleSortDataList") does triangles x vertices
steps of `divps` and `rsqrtps`. A 2,013-vertex, 3,000-triangle part takes about 90 ms on the render thread, inside the
CAS SimService.

## How Apex Radiance solves it

1. Compute every vertex's (x, y, z) / w once, with the same `divps`.
2. Test four vertices per SSE instruction, each lane doing the game's operations in the game's grouping (`rsqrtps` per
   lane gives the same approximation as on the game's broadcast value).
3. Split the triangles over up to 6 worker threads when triangles x vertices ≥ 300,000 (`kParallelMinTests`) and there
   are at least 8 triangles, with the caller's MXCSR copied
   to the workers.
4. Sort with `std::stable_sort` by count, descending (the game's merge sort is stable and descending), and write the
   indices back.
5. Check the first 16 calls of each session against the game's own function.

Allocation failures fall back to the game's function on the untouched indices.

## Settings

| Menu label | TOML key | Type | Default | Range | Effect |
|---|---|---|---|---|---|
| Faster Sim building | `[patches.FastCasSort] enabled` | bool | on | | Replaces the triangle sort. A missing key reads as on |
| (no visible control) | `[developer.controls.fast_cas] verify_every` | int | 16 in developer mode, 0 otherwise | 0 to 1024 when loaded | After the first 16 calls, also run the game's function on 1 call in N and compare |

## Compatibility and interactions

- Not part of any profile part ([README](README.md#compatibility-and-interactions)).
- No Developer card calls `FastCas::RenderDeveloperUI` in the current menu, so its *Check 1 call in N* slider (0 to 64)
  is not shown; the saved value still applies.
- Uses the texture encoder's worker hand-off protocol for its threads.

## Limitations

- None known beyond the general verification rule: a difference turns the feature off for the session.

## Technical reference

### The game side (Steam 1.67.2)

FUN_005d1960 cdecl(u16* indices, u8* vertices, u32 indexCount, u32 vertexCount, u16 stride, u8 positionOffset), plain
`ret`. Its only call is at 0x005D38F3 in FUN_005d3760 "CAS/ModelBuilder/FillDrawable" (per mesh part, when the part asks
for it); 0x005D38F8 is that call's return address. For each triangle: its three positions (x, y, z, x) / w from signed
16-bit values (the packing of FUN_005d1010 case 1), two unit edge vectors (`rsqrtps` + two Newton steps
`(1 - r r L)(0.5 r) + r`, constants 1.0 at 0x0107A538 and 0.5 at 0x00F9A5AC), a cross product C. Then for every vertex v
of the part (read at `v & 0xFFFF`) the unit vector from P0 and the sum `(z + y) + x` of its product with C, counted when
above FLT_EPSILON (0x00FE3474, `comiss` / `jbe`). The triangle list is sorted by EASTL's list merge sort (0x005CC2D0,
merge 0x005C9DE0: a node of the second half goes first only when its count is larger, unsigned: stable, descending)
and the indices are written back.

### The replacement

- Pure code in `features/cas_tri_sort.{h,cpp}` (`CasTriSort::Ref` is the literal translation used offline); the feature
  in `features/fast_cas.{h,cpp}`.
- Hook: `EntryChain` site `CasTriSort`, layer `FastCas`. GameAddr `CasTriSort`, group `FastCasSort`.
- **Checks:** the first 16 calls of each session, and in developer mode 1 in 16 afterwards, run the game's own function
  on a copy of the indices and compare. A difference logs `[FastCas] Result differs from the game's`, the game's result
  is used and the feature turns itself off for the session.

### Address reference

| Id (`GameAddr`) | Steam 1.67.2 | Kind |
|---|---|---|
| CasTriSort | 0x005D1960 | Sig (entry) |

## Rejected approaches

- Attributing the CAS hitches to FUN_005d1010 from sampler data: the sampler's function-start guess booked FUN_005d1960's
  samples to it. See [history](../../history/performance-fast-cas-sort.md).

## See also

- [Performance overview](README.md)
- [Validation](../../validation/performance-fast-cas-sort.md)
- [History](../../history/performance-fast-cas-sort.md)
