# Terrain lighting regression checks

`-CompositionCapture` optionally accepts the 2026-10-04 11-52-03 F7 directory.
The captured single and multipass shaders run with controlled equal material,
normal, sun and lamp inputs. The fixture reproduces the native pre-material
UNORM range mismatch and compares the daylight candidate against multipass RGB,
including transition weights, no-lamp pixels and original night pixels. This
is not an exact replay of scene vertices, textures, depth or all material layers.

The harness exercises the production policy, shader patch and sampler guard. It
does not attach to TS3, execute game patch writes, change captures or write files.
The runner compiles into `OutDir`; executables print results to stdout only.

```powershell
./tools/terrain_lighting_test/run.ps1 -Captures 'absolute session folder' -OutDir 'absolute test output folder' -ReferenceSource 'previous lot_light_bridge.cpp'
```

`ReferenceSource` is an unchanged source snapshot from the parent commit. Supplying
it enables pixel comparison of the old and current **actual** replacement HLSL.
Without it the policy, native-alpha GPU and captured-shader tests still run, but
night lot-pass equivalence is not tested. The current captured fixture names are
`PS_2BA33118.bin` (world s7) and `PS_2BA62940.bin` (multi-pass s2) from the 2026-10-04
02-05-41 session. Missing fixtures or unavailable native D3D9 cause a failure.

Coverage includes:

- Native zero lamp factors at day; single-pass and squared multi-pass factors,
  101 night levels, 120 gains, 21 native scales, previous night rounding, invalid
  values and all edit-priority combinations.
- Day/night endpoint debounce, reversal cancellation, world-load merging,
  disabled scheduling and 200,000 deterministic stress frames.
- Build preview zero phase delay, preserved Live delay, captured rapid-switch
  timeline replay and a maximum of four nearest priority chunks with a known eye.
- Queue checks extract the actual production `Attach`, `QueueSweep`, `OnPresent`,
  completion/refusal helpers, types and budget/timeout limits. Fake native memory
  and timing cover 3,000 randomized phase handoffs, an old completed bake awaiting
  consumption, nearest-first order, preserved batch ownership, non-mutating sweep
  refusals, native rebuild/render gates, rolling caps and terrain-owner changes.
- Exact sampler-state restoration after every injected read/set failure; no
  redundant sampler writes when states match. Texture getter/setter failures
  restore partial state changes; GPU checks also restore a real nonempty previous
  texture binding on the spare sampler.
- SM2 and SM3 bytecode creation, malformed/truncated/ambiguous/unsupported
  layouts, GPU pixel readback: smoothed RGB retained, solar alpha identical to the
  original native map at every tested pixel.
- Actual captured shader patching and D3D9 shader creation, exclusive c7 lamp
  constant identification, and world-lamp eligibility/streaming policy tests.
- With the reference: 60 GPU scenarios of the production lot HLSL at night,
  compared within one 8-bit LSB, plus day continuity across forced lot-edge weights
  with native window/lot scaling left at zero.
- Chunk map choice (`run_chunk_map_checks.ps1`): the extracted `RecordWorldChunk` with a fake device and a fake
  rebuilt-map lookup. The rebuilt map stays the chunk's map once seen; a map registered to another key is refused,
  never taken over (no back and forth before the first rebuild), and the chunk's own rebuilt map is drawn at the
  shader's sampler instead; a new rebuilt texture is taken, also after another check in the same frame; the uv mapping
  only in constants the VS reads; objects off a chunk centre refused; references stay balanced.
  `ShaderPatches::ConstantsRead` on synthetic code (def'd, relative, m4x4 rows, vs_1_1).
- Extracted wall and regular lot draw paths against D3D9 fixtures: exact wall gain
  and no-op writes, atlas/chunk fallback, failed original-constant reads, texture
  reference balance and restoration of all touched state. This is not a wall
  daylight shader/pixel validation.
- Extracted edit-state functions: native terrain completion before debounce,
  one independent rig request after debounce or completion, coalesced edits,
  later separate edits and exclusion of ordinary lot/automatic requests. An
  optional `EditReferenceSource` enables reproduction of the old lost-request
  behavior from an unchanged `night_terrain_relight_patch.cpp` snapshot.
- Extracted world reset, memo/scan selection, enumeration/tracking and refresh
  scheduling: no old-world lamp rows, real memo invalidation, forced first read,
  failed enumerations, 512 world changes without false edit events and successful
  new-world selection. Optional `WorldReferenceSource` reproduces the old cache
  behavior from an unchanged `lot_light_bridge.cpp` snapshot.

Per-pixel lamp law of outdoor objects, fences and windows (12/10, realistic reach):

- `ReachLawTable`: `features/lamp_reach.h` against the walls' row of the analysis (garden post, c3.x 0.309: 0.309 /
  0.236 / 0.066 / 0.005 at 3 / 6 / 8 / 12 m), the shader form of the share within 2.5% of `ReachShareImpl` (only up to
  r0), unreadable lamp weights.
- `ObjectLawPixels` (native D3D9): a synthetic fence-shaped pixel shader patched both ways, one lamp at 2 m over a
  64 x 64 sweep of positions (d 2 to 26 m, N.l from about 0.06 to 1), compared at every pixel with the CPU law: the first law
  and the reach law (through the 8-bit range and over the texel clamp) within 1-2 LSB. D3D9 pixel centres lie on whole
  screen coordinates: pixel x reads uv x / 64.
- `ObjectLawPatches` (with `-ObjectLawRoots 'folder;folder'`, for example the Light Probe `Captures` and
  `ShadersRecusados` folders): every distinct `ps_3_0` there through `PatchInstancedLamps`, `PatchObjectLampPs` and
  `PatchWindowOutdoor`, both variants: a refusal leaves the shader unchanged, the reach variant only where the first law is
  accepted, its rows right after the lamp rows and exactly 5 instructions per lamp plus the clamp more, `D3DDisassemble`
  within 512 slots, `CreatePixelShader` on native D3D9. One `FIRST` line per shader gives the first-law outputs' FNV, to
  compare with the same walk of the previous `shader_patches.cpp`.
  Fences are patched without the ground lamp shade (13/10) here, so the hashes stay those of the law alone; the shade has
  its own checks (`tools/floor_shade_check`).

The reported sampler-guard cost is an isolated driver CPU benchmark, not game FPS
or end-to-end lighting latency. A supported smooth terrain draw adds one native
texture read and one alpha move. No smoothing means no native-alpha alias.

Gameplay validation remains necessary: repeated Build mode day/night switching,
move/place/remove/recolour/intensity/on/off edits inside and outside lot borders,
moving camera, lot streaming, unsupported variants, seasons, native D3D9 and DXVK.
Preserved chunk render gates, caps and timeout fallbacks mean instant visual
updates cannot be promised for every scene or driver.

The queue suite can also run separately:

```powershell
./tools/terrain_lighting_test/run_queue_checks.ps1 -OutDir 'absolute test output folder'
```

It executes extracted queue logic against memory/timing fixtures, not the TS3
engine or driver. The printed source hash identifies the queue source tested.
# Ground scale regression

`run_ground_scale_checks.ps1 -Captures <2026-10-04 15-33-55 Session>` reads the
captured s7/s11 shaders, checks linear versus squared constant classification and
refusal of shared, mixed or saturated multipliers. Native D3D9 readback compares
both multiplier equations at five day/night weights and eight brightness gains.
The executable only reads captures and prints results; it does not install files.
These checks do not substitute for visual gameplay validation.

The resource suite also checks the realistic reach variant of the object and fence draws (the walls' scale times the slider trims, the reach R and lamp weight in the lamp rows, the r0^2 rows after the cones, every row restored, the first law's values with it off) and that a fence lamp is tested from the group's piece nearest it. Since the 12/10 review it also covers the lamp weight per list entry (`LampReach::Entries`: 2 for a story-0 lot lamp, else 1), the native wall scale's 2 s freshness (`NativeWallScale`, extracted with `DrawWallGain`; a stale daytime 0 gives the night value 1), a refused reach copy (the first law draws, the refusal is counted), fences without placed lamps keeping the first law, and draws without placed lamps not counted as reach draws. Since 13/10 it also checks the fences' ground lamp shade: a copy with it binds its 3 rows for the draw (a fixture `FloorShadeBind`) with the lot under the group's centre (`FindLotAt`), restores them, counts the draw with or without a lot, adds the F7 line, and a copy without it binds nothing. `DrawWindowOutdoor` is not extracted (its rows are checked by review only).

The resource suite extracts the current object lamp picker and cone functions as well as the draw paths. Its fixture covers full-daylight native fallback, the expanded cone upload/restoration block, upward-only atlas mode, legacy centre-cone callers, and wall bloom alpha/depth/stencil restoration. Group discovery, wall blocking, atlas production and wet/mask resource construction are controlled fixture dependencies; these checks do not validate those producers or gameplay.
