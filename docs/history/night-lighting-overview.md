# Night Lighting: history

Chronological record of investigations and design decisions that concern Night Lighting as a whole. Newest entries at
the bottom. The current behaviour is described in [features/night-lighting/README.md](../features/night-lighting/README.md).
Sub-part histories: [lot light pass](night-lighting-lot-light-pass.md),
[world atlas](night-lighting-world-atlas-and-smoothed-maps.md), [terrain relight](night-lighting-terrain-relight.md),
[world lamp response](night-lighting-world-lamp-response.md).

### 2026-09-25: robustness review of the draw hooks

**Context:** review of the combined build's lighting code.

**Finding:** item 6: reinstalling on the message-loop thread crashed (textures released while the render thread drew
them). Item 3: `Uninstall` called `DirtyAllRigs` off the render thread. Item 5: `PatchLeafShadow` emitted an `lrp` with
two constant sources, which DXVK accepts and native D3D9 refuses. Shader caches keyed by pointer without AddRef gave
stale classes when addresses were reused. Any menu change used to switch off `automaticoAoAnoitecer` while
`luzDoLoteNaGrama` was off (fixed 25/09 10:40). The hooks gained exception catching (`HookFailed`, log "Excecao dentro
do gancho de desenho ..."), `g_stateUnknown` and a mutex in `UpdateHooks`.

**Outcome:** `DeferredReinstall` on the render thread, rig dirtying deferred to `OnPresent`, all patches follow native
D3D9 rules, `g_pinned` AddRefs every classified shader. Kept.

### 2026-09-27: standalone baseline at the split

**Context:** the standalone's Night Lighting started from v0.1.0 (commit b84d5f1), not from the combined build's
`combined-final`.

**Finding:** everything after v0.1.0 was absent at first and was re-added one change at a time after a test, fences
first: the story gate 0xC294D9 and the relight reconciliation / local relight (v0.1.0 had fixed triggers), the
bake-matched per-pixel law `W = 0.4 x range` with `SelectPixelLamps`, the rule `max(rig + vertex lights, per-pixel,
ground)`, the ground facing factor `sat(N.y + 1)` with strength `max(1, forcaNosObjetos)`, per-pixel lamps on fences,
shader pre-creation at `CreatePixelShader` / `CreateVertexShader` (`PrecreatePs/Vs`, foliage VS pool, `OwnCreate*`,
`CodeBytes`), the batched shader log (`FlushShaderLog`) and English status and log strings. v0.1.0 had no HDR code.
Unchanged since v0.1.0 at the split: lot pass, smoothed maps and atlas, walls, floors, roads, snow, level light share,
lamp colour, CPU rig boost, foliage, roofs, water. The feature was then marked experimental and off by default.

**Outcome:** re-add order recorded in [changes-since-0.1.0.md](changes-since-0.1.0.md). The combined build's HDR lamp
gain (`HdrOutput::LampGain()` multiplied into several constants, `DrawLampGainOnly`) is not part of the standalone
([removed-features.md](../removed-features.md)).

### 2026-09-28: start-up shader compilation

**Context:** the combined build compiled each HLSL replacement when the game created the matching shader
(`PrecreatePs`), usually during loading, so DXVK did not compile it in the frame an object first appeared.

**Finding:** the first lot pass, lamp, roof or water still ran `D3DCompile` on the render thread in the standalone.

**Outcome:** the five replacements of `lot_light_bridge.cpp` and the eight smoothing shaders are compiled at start-up
on a background thread (`framework/shader_cache.h`). Kept.

### 2026-09-29: draw-handler CPU cost (research\perf2\apexcost\report.md P3 to P9)

**Context:** same pixels, less CPU per replaced draw.

**Finding:** each handler's own state calls re-entered Apex's own hook chains, where only the bridge's own tracking
(which skips them) and the profiler's counts looked at them. Six maps were looked up per draw for vertex shader
facts. The lamp refresh re-read the enumeration several times and rebuilt a std::map.

**Outcome:** `CallOriginal*` wrappers, `SamplerBind`, one `VsInfo` per shader, memoized `SelectLamps`, a single
`ReadEnumeratedLamps`, sorted reused vectors, `RecordWorldChunk` returning its entry, kept chunk order in the smoothing.
Written, not measured in game.

### 2026-09-29: night darkness and lamp range studied

**Context:** a "night darkness" control and a "light range" control were considered next to the 1.5.0 brightness
sliders.

**Finding:** night darkness has no single engine value. The ambient comes from the exterior diffuse probe cube and
`terrainLightProbeMap`, both built from the AmbientDome curves (sky +0x760 / +0x770, written every frame by
0x00C14860); whether the asynchronous probe capture follows a scaled dome is unverified (lightMgr+0xB0 state machine). A
post-process night filter would also dim lamp light. Lamp range +0x130 is a brightness weight on 1/d^2, not a radius,
read by the bake, the room solve, the rigs and the mod's own kernels; the only consistent change is scaling +0x130 on
the light objects, recomputing the rect (vfunc+0x50), a full rebuild and every room re-solved.

**Outcome:** neither control implemented. Measure with F7 captures first if revisited.

### 2026-09-30: furniture dark after floor switches

**Context:** study of F6 015204, F7 087 / 091 / 092 and F8 01:51.

**Finding:** the indoor object path (`DrawIndoorObject` + `PatchIndoorBasis`) read the 64x64 directional basis maps
with the room light map's uv and texel = uv x basis size. The room light map covers the lot's power-of-two size at
4 texels/m (lot CF2DEA20: 128 x 256 = 32 x 64 m, VS uv rows c15 / c16 with |xz| = 1/32, 1/64); the basis maps always
cover 64 x 64 m. So x was read at twice the object's position, outside the house plan (alpha 0): basis light 0, and
since the path replaced the rig diffuse and zeroed the vertex lights, only the ambient cube was left (091: 0.051 grey;
087: pixel 0.012 with the red lamp at 1.79 in rig slot 0). Offline, 4 of 12 captured vs_3_0 give TEXCOORD0.xy from two
dp4 (c15 / c16, one c195 / c196); 091's VS gives (32, 64, 1/64, 1/64). It followed floor switches because
`RoomMapPadding` released a light map's set after 600 frames (about 3 s) without a draw, so a floor out of view lost it
and its furniture alternated between the game's shader (lit) and the Apex path (dark). The verifiers added: with the
lamps off the basis maps are about 0 (they hold lamp light only), and the path replaced the rig diffuse, which for
furniture carries the moon and fill slots with the Rooms at Night fill, so it stayed darker than the game's shader.
`max(rig, basis)` was tried and rejected: it brought the per-object lamp back (see [unlit-rooms](../features/night-lighting/unlit-rooms.md)).

**Outcome:** `IndoorBasisScale`, sets kept while the game holds the maps (checked every 300 frames), the 30-frame look
per (+X basis map, shader), and the diffuse = unlit-room rig lights + basis x strength. `PatchIndoorBasis` valid on
every captured PS it matches. Not tested in game.

### 2026-10-04: lighting styles scoped to the Lighting page (PR #2)

**Context:** the styles also changed the water glow (`brilhoNaAgua`), which lives on the Water & Snow page.

**Finding:** a style applied from the Lighting page silently changed a setting on another page.

**Outcome:** styles hold nine values (water removed: Subtle 0.27, Soft 0.3, Natural 0.4 dropped); the tiles became
choice rows with a *Custom* row; *Undo choice* shows only when available. Kept.

### 2026-10-04: menu hierarchy and Rooms at Night ranges (PR #2)

**Context:** menu reorganisation.

**Finding:** Ground split into a behaviour card and a *Ground intensity* card, dusk timing under *Updates*; Objects
split into *Objects*, *Doors, counters and fences* and *Indoor objects*; Stories moved the seam and all-floor switches
under *Floor detail*. Rooms at Night brightness range became 10 to 80% (was 0 to 100%) and the blue tint default 0%
(was 20%). The exterior wall switch now means "wall lamp light by day and night" rather than "gain 1 when off".

**Outcome:** current settings table in the feature page. Other bindings, defaults and dependencies unchanged.
