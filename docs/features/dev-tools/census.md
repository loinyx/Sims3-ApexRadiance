# Shader census and false colour

Three developer tools answer one question: which of the game's shaders still receive lamp light without a Night
Lighting fix, and would a pattern patch accept them?

- **False colour** paints solid magenta every draw that receives baked lamp light or an outdoor rig but that no Night
  Lighting fix claimed.
- **Census** records the same classification for about three frames per (VS, PS) pair into `ApexRadiance_Censo.txt`,
  and saves the bytecode of every unfixed pair to `Censo\`.
- **Refused shaders** saves, automatically, every shader a fix tried and refused to `ShadersRecusados\`.

The shaders saved by these tools feed offline coverage tests that run the shader patchers outside the game.

## Status

| | |
|---|---|
| Availability | Developer mode only. Released in 2.5.5 as part of the unified build (earlier: developer build only) |
| Default | False colour off; census and refused-shader dump idle |
| Menu | Developer > Lighting > Collect lighting evidence (false colour checkbox, census button) |
| Configuration | False colour is saved as `false_color` in `[developer] controls`; no other keys |
| Source | [`features/lot_light_bridge.cpp`](../../../features/lot_light_bridge.cpp), [`features/lot_light_bridge.h`](../../../features/lot_light_bridge.h) |

## The problem

Night Lighting fixes lamp light surface by surface, by recognising shader patterns. A shader family that no fix
recognises keeps the game's original, blotchy lamp light, and nothing on screen says which family that is. Finding the
gaps by eye, one Light Probe capture at a time, misses families that appear only in some scenes or seasons.

## How Apex Radiance solves it

The lot light bridge's draw hook already sees every draw. While false colour or a census is active, it first decides
whether the draw is lamp-lit (a baked light map is bound, or an outdoor rig is active), then runs all the fixes and
treats the draw as **claimed** when a fix redrew it. Unclaimed lamp-lit draws are painted magenta or counted per shader
pair. The acceptance measure for Night Lighting coverage is no outdoor object in magenta on a summer and a winter test
lot.

## Settings

| Control | Where | Effect |
|---|---|---|
| False colour: magenta = gets lamp light but no fix claimed it | Developer > Lighting > Collect lighting evidence | `LotLightBridge::SetFalseColor`; saved in `[developer] controls false_color` |
| Census: write ApexRadiance_Censo.txt | Same card, with status `(ready)` / `(writing...)` | `LotLightBridge::RequestCensus()` |

Both need the bridge's draw hooks registered: Night Lights installed with at least one of the lot light bridge, the
object fix, roofs, water or the wall fix enabled (`UpdateHooks`). Otherwise nothing is recorded or painted. In normal
mode `OnDraw` passes straight through and `SaveRefused` returns at once.

## Compatibility and interactions

- Rig mode comes from `RigTracker`, Night Lighting's rig tracker.
- False colour returns `Skip`, so hooks that run later (Frame Capture and the Light Probe at `Last`) see the magenta
  redraw, not the game's draw; the Light Probe's `mod:` line would then describe a mod draw (inferred).
- The census also counts the Light Probe's 1x1 occlusion copies if both run in the same frame (inferred; avoid).

## Limitations

- Candidates are found by texture format only: an unlit surface that binds any small A8R8G8B8 single-level texture is
  also a candidate, and a lamp-lit surface that uses per-pixel lamp constants (the HD "Night" shaders) and no map is
  not.
- "Claimed" means the fix returned `Skip`. A fix that changes the draw without redrawing counts as unclaimed: the wall
  strength fix only redraws when its gain is not 1, so at 1 the ExteriorWall pairs show as `SEM`.
- `SEM` does not mean broken: many pairs need nothing (interiors, picking, shadows, HD Night). Check the technique
  first.
- The census does not record the refusal reason or pixel coverage; the reason is only in the log and in
  `ShadersRecusados`.
- Pairs are keyed by D3D pointers during the window; the file shows content hashes. The rig column keeps only the last
  value.
- Census numbers depend on the scene; compare censuses of the same save and camera only.
- False colour keeps the game's blending, depth and textures, so a magenta draw in a multiplicative pass (such as the
  modulate-2x lot light pass) appears tinted rather than pure magenta.

## Technical reference

### Candidate test (`LitCandidate`)

A draw is a lamp-lit candidate when any sampler s0..s15 binds:

- a baked light map: 2D, `D3DFMT_A8R8G8B8`, not DEFAULT pool, 1 mip level, width and height <= 1024 (room, wall,
  floor, lot and per-object 32x32 maps; described as `s<n>:map<W>x<H>`), or
- a terrain chunk map: `D3DFMT_DXT5`, 256x256, at most 5 levels (`s<n>:terrain`),

or it is drawn with an outdoor rig (`RigTracker::CurrentMode() == 2`; described as ` (so rig)`, rig only).

### Classification (`OnDraw`)

`OnDraw` (the bridge's draw hook template) wraps `OnDrawTracked`, which runs all the fixes. When neither false colour
nor a census is active, in normal mode, or inside the bridge's own call, it is a plain pass-through. Otherwise it
evaluates the candidate test and rig mode first, runs `OnDrawTracked`, and counts the draw as claimed if it returned
`HookAction::Skip`.

- **Census**: `g_census[{g_curVs, g_curPs}]` accumulates draws, claimed draws, primitives (`g_curPrims`, set by both
  the DIP and DP hooks), the last rig mode and the first texture description.
- **False colour**: an unclaimed candidate with a pixel shader is redrawn with a magenta shader and the hook returns
  `Skip`. The magenta shaders are built once from tokens, ps_2_0 and ps_3_0, chosen by the game shader's version
  (`PsIs3`): `def c0, 1, 0, 1, 1` + `mov oC0, c0`
  (`0xFFFF0200/0300, 0x05000051, 0xA00F0000, 1.0, 0, 1.0, 1.0, 0x02000001, 0x800F0800, 0xA0E40000, 0x0000FFFF`).

### Census window and file

`RequestCensus` (ignored while one is pending) clears the map and sets `g_censusFrames = 3`; each
`LotLightBridge::OnPresent` decrements it and at 0 `WriteCensus` runs on the render thread. The window is the rest of
the current frame plus about two full frames.

Output, in the Apex Radiance folder: `ApexRadiance_Censo.txt` (truncated) and the folder `Censo\`.

```
Apex Radiance census: draws that get baked light (light map) or an outdoor rig, per shader pair
columns: draws | fixed | triangles | rig | VS hash/size | PS hash/size | textures

ok     10 |    10 |     288 |  2 | VS 9CA7D90C/1296 | PS 4D1636F8/804 | (so rig)
SEM    26 |     0 |    1068 | -1 | VS 3A9A86BF/696 | PS 1E84E3E5/740 | s1:map32x32
...
62 pairs, 38 without a fix (code in Apex Radiance\Censo)
```

| Column | Meaning |
|---|---|
| `ok` / `SEM` | At least one draw of the pair was claimed / none (`SEM` = without a fix) |
| draws | Candidate draws of the pair in the window |
| fixed | Of those, claimed by a fix |
| triangles | Sum of primitive counts |
| rig | Last `RigTracker::CurrentMode()`: -1 none, 0 indoor with ceiling, 1 roofless fenced area, 2 outdoor |
| VS/PS hash/size | FNV-1a 32 of the bytecode (`h = 2166136261; h = (h ^ dword) * 16777619` over DWORDs) and size in bytes; 0 if unreadable |
| textures | Light maps found, or `(so rig)` |

For every `SEM` pair, `Censo\VS_<hash>.bin` and `Censo\PS_<hash>.bin` hold the raw bytecode, named by content hash and
therefore stable across sessions. Log line: `[LotLightBridge] Census written: <pairs> pairs, <n> without a fix`.

### Refused shaders (`SaveRefused`)

`PatchedFor`, the per-shader patch cache, calls `SaveRefused(what, original)` whenever a fix's pattern did not match
(log `[LotLightBridge] <fix>: shader <ptr> does not have the expected pattern, left as the game draws it`) or D3D
refused the patched shader (warning `[LotLightBridge] <fix>: shader <ptr> refused by D3D (<hr>)`).

- Folder: `ShadersRecusados\` in the Apex Radiance folder.
- Files: `<tag>_PS_<fnv>.bin` and, when the current VS is readable, `<tag>_PS_<fnv>_VS_<fnvVS>.bin` (the pair's VS),
  where `<tag>` is the fix name with non-alphanumerics replaced by `_`.
- Named by content, never rewritten if the file exists, at most 300 files per session. The folder is never cleared.

### Files and functions

| File | Symbol | Role |
|---|---|---|
| `features/lot_light_bridge.cpp` | `LitCandidate`, `PsIs3`, `OnDraw<DrawFn>`, `g_falseColor`, `g_magenta`, `g_census`, `CensusRow` | Candidate test, magenta, recording |
| | `WriteCensus`, `RequestCensus`, `CensusStatus`, `SetFalseColor`, `FalseColor`, `OnPresent` (countdown) | Census file |
| | `SaveRefused`, `PatchedFor`, `ShaderCode` | Refused-shader dump |
| `features/lot_light_bridge.h` | `SetFalseColor`, `FalseColor`, `RequestCensus`, `CensusStatus` | API |
| `features/developer_settings.h` | `Capture`, `Apply` (`false_color`) | Persistence |
| `patches/night_terrain_relight_patch.cpp` | `DrawDeveloper` | UI |
| `features/shader_patches.cpp/.h` | `ShaderPatches::*` | The patchers the offline tests exercise |

No game addresses (D3D9 level). The candidate formats encode the light-map families: A8R8G8B8 managed single-level maps
(room, wall, floor, lot, per-object 32x32) and DXT5 256x256 terrain chunk maps.

## Rejected approaches

- Naming dumped shaders by pointer
  ([history](../../history/dev-tools-census.md#2026-09-25-refused-shaders-dump)).
- Judging patched shaders by "it ran in game" under DXVK
  ([history](../../history/dev-tools-census.md#2026-09-25-offline-validity)).
- Writing many output files from a test executable
  ([validation](../../validation/dev-tools-census.md#automated-tests)).

## See also

- [Validation](../../validation/dev-tools-census.md)
- [History](../../history/dev-tools-census.md)
- [Night Lighting](../night-lighting/README.md), [Light Probe](light-probe.md), [Developer mode](../developer-mode.md)
