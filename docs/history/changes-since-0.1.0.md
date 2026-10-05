# Changes since v0.1.0: history

Audit of every change between the combined build's v0.1.0 and `combined-final` in the lighting and rendering chain,
written when the standalone was started from the v0.1.0 lighting. It drove the order in which post-0.1.0 changes came
back. The current behaviour of each part is described in the feature pages ([features/night-lighting/README.md](../features/night-lighting/README.md),
[features/edge-smoothing.md](../features/edge-smoothing.md), [features/depth-blur.md](../features/depth-blur.md),
[features/picture-filters.md](../features/picture-filters.md)). Published versions since then are in
[releases/README.md](../releases/README.md).

### 2026-09-28: audit of changes since v0.1.0

**Context:** after the split decision, the standalone restarted Night Lighting, Depth Blur and Edge Smoothing from
v0.1.0. Play on `combined-final` had felt worse in visuals, stability and fluidity than v0.1.0, so every later change
had to be judged before coming back.

**Finding:** the audit below, as written at the time (the per-change file and line references are to the combined tree).

Every change between **v0.1.0** (combined tree commit `b84d5f1`, release `nightremake-v0.1.0-alpha`, 27/09 19:13) and
**`combined-final`** (commit `45e36e2`, 28/09 14:23) in the lighting and rendering chain and in everything else that runs
per frame, with a re-add recommendation for the standalone. The intermediate release **v0.2.0** is commit `f18cca8`
(28/09 02:14). There are only these two commits after v0.1.0, so each change is dated by commit plus the chronological
notes `S3SS-dev\NOTAS-ILUMINACAO.md` (Portuguese).

Tree for all paths below: `%USERPROFILE%\Desktop\S3SS-dev\Sims3SettingsSetter\`. Line numbers are in `combined-final`
unless marked `b84d5f1`. Diff commands: `git diff b84d5f1 f18cca8 -- <file>` (v0.2.0) and
`git diff f18cca8 combined-final -- <file>` (combined-final).

Evidence used besides the code:
- `Documents\Electronic Arts\The Sims 3\S3SS\S3SS_LOG.txt` of the last combined-final session (started 28/09 14:20:59);
- the Frame Profiler output `S3SS_Hitches.txt` of that same session (profiler session "2026-09-28 14:20:59");
- the maintainer's configs: current `S3SS.toml`, and the backups `Backups Sims 3\11-antes da reorganizacao Apex\S3SS.toml.antes-remover-splitlevel`
  (28/09 02:07, the config used while playing v0.1.0), `12-...\S3SS.toml`, `14-...\S3SS.toml`;
- `re\TS3W.exe` (bytes at `0x006BC020` and `0x00C294CE`, and a scan for calls to `0x006BC020`).

Status column values: **approved** (tested in game and kept), **reported fix** (a bug reported from play), **experiment**
(the mod's own idea), **infrastructure** (enables other changes), **removed** (dropped from the standalone by the maintainer decisions of 28/09, see
[architecture.md](architecture.md#120-decisions-of-2026-09-28-they-override-the-plan) 12.0).

#### 1. Summary, in suggested re-add order

Lowest risk and highest player value first. "Cost" means stability and fluidity risk.

| # | Change | Commit | Status | Look | Cost | Recommendation |
|---|---|---|---|---|---|---|
| 0 | **Split-Level Lighting Fix stays on in official S3SS** (it was on in v0.1.0 play; removed from the combined build in v0.2.0) | v0.2.0 (removal) | baseline parity | lot lamps of every story reach the world grass; type-11 lot lamps reach every story's outdoor room | none (13-byte patch, upstream) | **Do first, no Apex code.** Test the v0.1.0 baseline with it on, as it was played (section 3) |
| 1 | **Fence per-pixel lamps** (bake-matched falloff W = 0.4 x range) | combined-final | **approved** (the only change judged clearly better in play) | upright fence faces turned to a lamp get its direct light, like real light, instead of only the ground's | low: ~87 ALU/pixel on fence pixels, 1 lamp scan per fence draw, 1 more cached PS variant | **Re-add first**, fence-only (section 4 lists exactly what it needs) |
| 2 | Lot-edge story gate (`BakeLevelStub`, `0xC294D9`) | combined-final | reported fix | removes the straight cut at lot borders of houses on a foundation | very low (a call per lot light per chunk bake) | **Only if #0 is not used.** Redundant with the Split-Level fix (bypassed when it is on) |
| 3 | Batched shader-first-draw log lines (`NoteShader`) | combined-final | experiment | none | lowers cost (no disk write per new shader) | Optional, low value: the standalone's logger is already buffered |
| 4 | Relight reconciliation + localized relight (`Reconcile`, `Settle`, chunk `+0x55`) | combined-final | reported fix (automatic relight: lamps switched on later kept a cut) | ground under a changed lamp updates by itself | **high**: 12-60 ms hitches about once a second (evidence 2.1) | **Re-add only after a redesign and a test** (throttle, lit lamps only, no churn from streaming) |
| 5 | Gate darkness fix (rig kept, ground light >= 1, facing `sat(N.y + 1)`) | combined-final | reported fix | gates and doors never darker than the game; objects overall brighter and flatter | low CPU, a few ALU | **Keep out for now** (maintainer feedback: objects looked no better, or worse). Retest alone, without #6, only if dark gates come back |
| 6 | Object per-pixel kernel and lamp choice (W/d^2, no cut-off) | combined-final | experiment | much more lamp light on objects 8-20 m from lamps (5x at 10 m), faint wash from lamps up to ~140 m | low | **Keep out** (maintainer feedback: objects looked worse) |
| 7 | Shader pre-creation at `CreatePixelShader` / `CreateVertexShader` | combined-final | experiment | none | medium risk, no measured gain (log: 0 copies made) | **Keep out** |
| 8 | HDR lamp-gain plumbing (`ConstGain`, `DrawLampGainOnly`, `LampScaleAfter`, water `params.z`, foliage gain copy) | v0.2.0 + combined-final | removed | none in SDR (gain = 1) | small, inert | **Keep out** (HDR removed) |
| 9 | PostScene camera votes (`CameraNear`, `CameraViewProj`, `CameraDepthA`) | v0.2.0 | removed (AO) | none | 7 constant reads per draw for the first 24 scene draws, every frame | **Keep out** (only AO used them; profiler has another source) |
| 10 | Picture filters (SDR) | combined-final | kept by decision | strong: the maintainer's config had exposure -0.93 EV, sharpen 1.5, clarity -0.46, saturation 0.87, shadows -0.35 | 1 scene copy + downsample chain + full-screen pass per frame, a Priority::First hook on every draw | Kept. **A/B test it off** against v0.1.0 (section 2.2) |
| 11 | SMAA 1x in Edge Smoothing (default method) | v0.2.0 | kept by decision | sharper than FXAA; the maintainer ran **Ultra** | 3 passes + 4 timestamp queries per frame; 3 large D3DCompile at first use | Kept. **A/B test FXAA** (what v0.1.0 used with the same config) |
| 12 | Depth Blur off in map view (`MapView::IsOpen`, fade 0.3 s) | v0.2.0 | kept by decision | map view stays sharp | negligible | Kept |
| 13 | Buffered logger (flush thread, 1 s) | combined-final | kept (standalone has its own `apex_log`) | none | lowers cost | Kept (own implementation) |
| 14 | Frame Profiler | combined-final | kept, dev only, off by default | none | when on: 27 game detours + 2 hooks on every draw | Keep dev-only; **it was on in the combined-final session that was judged** |
| 15 | Smooth Streaming, Script GC Scheduler, Service Frame Budget, Smooth Patch Precise additions (MMCSS, 0.5 ms timer, WaitOnAddress) | combined-final | removed | Smooth Streaming delays lot room lighting and spreads terrain rebuilds | per-frame engine changes | Out (decision); confounders of the comparison |
| 16 | UI, English strings, Advanced sections, pond options moved to Reflections | v0.2.0 | cosmetic | none | none | The standalone has its own UI |

Everything else in the diff (HDR output, Native HDR, Ambient Occlusion, lot map probe, `CallOriginal*` registry helpers,
frame capture strings, light probe / diag strings) is either removed with those features or developer-only.


#### 2. What most plausibly explains "worse visuals, stability, fluidity than v0.1.0"

In order of weight of evidence.

##### 2.1 Fluidity: the relight reconciliation (change #4) makes a terrain rebuild about once a second
- `S3SS_LOG.txt` (combined-final session, 14:20:59): **87** "Local relight: N lamps changed, K terrain chunks relit" lines
  in one session, 1-6 chunks each, at night **and by day** (`night level 0.00`), besides 3 full rebuilds from the button.
- `S3SS_Hitches.txt`, same session: hitch frames containing "Terrain update" come about once a second while lamps churn,
  against a median frame of ~5.3 ms. Examples (frame ms / terrain update self ms): t=86.8 s 18.0/12.6; t=87.8 s 49.7/22.7;
  t=88.0 s 54.2/35.5; t=88.3 s 60.1/33.2; t=88.7 s 52.7/33.3; t=89.7 s 52.5/35.0; t=96.3 s 41.5/33.5; t=97.9 s 19.9/12.7;
  t=163.7-171.7 s a second burst of 15-166 ms frames. The three button rebuilds cost 254-263 ms each.
- Across the 9 profiler sessions of 28/09, up to 791 hitches per session carry more than 10 ms of terrain update.
- Mechanism: each local relight sets `chunk+0x55` on the chunks under the changed lamps; the game then re-renders each
  chunk's 4 composited textures (one per frame, see `smooth_streaming_patch.cpp` header, note 4), and `LightmapSmooth`
  re-reads, re-smooths, re-uploads the 1024x1024 map and rebuilds the atlas for each changed chunk.
- Why it churns: `DiffBaked` treats every lamp that appears in or disappears from the light enumeration as a
  "structural" change (relit at once, no 5 s throttle). Lamps of lots streaming in and out, and lamps changing lit state,
  keep producing such changes. v0.1.0 only rebuilt on a lot lamp edit (0.7 s after it), a stuck countdown (at most every
  15 s, night only, only after a lot lamp armed it) and 5 s after loading.
- Caveat: Smooth Streaming (terrain spread 64 chunks per frame) and the Frame Profiler were also on in that session.

##### 2.2 Visuals: the post-processing and the config, not only the lighting code
The maintainer's config changed between v0.1.0 play (backup 11, 28/09 02:07) and now:

| Setting | v0.1.0 play | now | Effect |
|---|---|---|---|
| Split-Level Lighting Fix | on | gone (removed in v0.2.0) | see 2.3 |
| Picture filters | did not exist | on: exposure -0.93 EV, sharpen 1.5 (max), clarity -0.46, saturation 0.87, shadows -0.35, deband 1.08 | darker, flatter, crunchier image; lamp pools look dimmer |
| Edge Smoothing | FXAA quality 2 (v0.1.0 has no `metodo`) | SMAA Ultra (`metodo = 1`, `qualidadeSmaa = 3`) | different edge look, more GPU |
| Depth Blur | on | off | |
| LotStreamingOptimizations (upstream) | off | on (object throttle 2 per lot, max 12 active lots) | lots and objects pop in later |
| `forcaNosObjetos` | 0.57 | 0.38 | in v0.1.0 this also scales the ground light on objects: 0.38 is the "dark gate" |
| `forcaLuzPorPixelNosObjetos` | 1.0 | 1.76 | tuned for the new kernel (#6); on the v0.1.0 kernel it overbrightens objects near lamps |
| `forcaNasCercas` | 1.0 | 1.2 | |

The standalone copies `S3SS.toml` values on first run (architecture.md 12). **For a fair v0.1.0 baseline, reset the
three Night Lighting strengths to the v0.1.0-play values (0.57 / 1.0 / 1.0) or the defaults, and compare with Picture off
and FXAA before judging any re-added change.**

##### 2.3 Visuals: Split-Level Lighting Fix removed in v0.2.0 (change #0)
- v0.1.0 still built `patches\split_level_lighting_fix_patch.cpp` (an upstream S3SS patch). It rewrites
  `BaseLight::GetLotID` to return 0. It was **enabled** while playing v0.1.0 (backup 11 line 153-154).
- `re\TS3W.exe`: `0x006BC020` = `8B 81 C0 00 00 00 8B 91 C4 00 00 00 C3` (GetLotID). Only two direct callers:
  `0x006B635D` (the outdoor-room world-light gather, [engine/room-light-maps.md](../engine/room-light-maps.md)) and
  `0x00C294D0` (the terrain bake, right before the story gate `cmp [edi+0D0h],0` at `0x00C294D9`).
- So in v0.1.0, with the fix on, lot lamps of every story passed the bake's lot/story test (no lot-edge cut on houses on
  a foundation) and type-11 lot lamps lit every story's outdoor room. v0.2.0 dropped the fix; the lot-edge cut
  reported on 28/09 afternoon appeared, and combined-final re-fixed half of it with `BakeLevelStub` (#2).
- Official S3SS still ships this patch. The standalone runs beside official S3SS, so turning it on there restores
  v0.1.0 exactly. Caveat from its own description: "May cause some light near walls/floors to bleed through".

##### 2.4 Visuals: objects brighter and flatter (changes #5 and #6 together)
- v0.1.0 object lamps: kernel `sat(1 - d^2/R^2)^2`, R = clamp(1.2 sqrt(range), 2, 25) (11.8 m for a street lamp,
  range 97), lamps = the nearest 8 by `d - R <= 40` (`SelectLamps`), and the game's rig **zeroed** (PS c5..c7 and the VS
  vertex-light colours).
- combined-final: kernel `min(1, W cos / d^2)`, W = 0.4 x range (38.8 for a street lamp), lamps = best 8 by W/d^2 with a
  0.002 threshold (reaches ~140 m for a street lamp), the rig **kept** and max'd with the per-pixel lamps, ground term
  `sat(N.y + 1) x max(1, strength)` instead of `(0.5 + 0.5 N.y) x strength`.
- For a street lamp at 10 m the new kernel gives 0.39 cos vs 0.08 (about 5x); at 18 m 0.12 vs 0. Upright faces get the full
  ground light instead of half. Result: more light everywhere on objects, less falloff contrast. Consistent with the
  maintainer feedback that objects looked no better, or worse.

##### 2.5 Stability: new code on threads and in create hooks (change #7)
- The shader pre-creation hook runs inside the registry's `CreatePixelShader` / `CreateVertexShader` dispatch, which holds
  the registry's recursive mutex (`ExecuteCreatePixelShaderHooks`, `d3d9_hook_registry.cpp` 633) while it may run
  `D3DCompile` for up to five HLSL replacements. Every draw hook takes the same mutex. If the game ever creates a shader
  off the render thread, the render thread waits for the compile, and `g_status`, `g_replacementPs`, `g_snowPs` etc. are
  written from that thread without a lock (data race on a `std::string`).
- It brought no measured benefit: every "Shaders at their first draw" line of the session reports "foliage vertex shader
  copies made at shader load: 0 new, 0 total, 0 used", and `[LotLightBridge] Active` is still logged after the world load
  (line 130), i.e. at the first draw. The game creates its shaders before the hook registry is initialised
  (`[D3D9Hooks] Hook registry initialized`, line 111, after device creation).
- No crash was recorded in the notes for any post-0.1.0 change; this is a risk, not an observed failure.

##### 2.6 Confounders of the judged combined-final session
Frame Profiler on (27 detours, `[FrameProfiler] Timing 27 of 27`), Smooth Streaming on (lighting cap 10 ms, slices,
terrain spread), Script GC Scheduler on, Smooth Patch Precise with MMCSS "Games" on the render and simulation threads and
a 0.5 ms timer, Ambient Occlusion toggled on during the session. All are out of the standalone or dev-only; none of
them is a lighting change, but each changes frame pacing or when lot lighting arrives.


#### 3. Baseline parity: test v0.1.0 as it was played (change #0)

Before re-adding anything: standalone at the v0.1.0 lighting, official S3SS beside it with **Split-Level Lighting Fix
on**, Night Lighting strengths at 0.57 / 1.0 / 1.0 (`forcaNosObjetos`, `forcaLuzPorPixelNosObjetos`, `forcaNasCercas`),
Picture off, Edge Smoothing FXAA. That is the v0.1.0 look. Then turn Picture and SMAA on one at a time.

Interaction with later changes: with Split-Level on, `GetLotID` returns 0, so the bake jumps over the story gate and
`BakeLevelStub` (#2) is never reached; the two are compatible. If #4 is ever re-added, its `Bakeable()` assumes upper
story lot lamps are baked only with `BakeLevelStub`; with Split-Level on they always are (and so are lot lights #4 does
not track), so its bookkeeping would under-count, which is harmless for relighting.


#### 4. Fence per-pixel lamps: exactly what it needs (change #1, approved)

##### 4.1 What it does
`DrawInstanced` (`lot_light_bridge.cpp` 1135; v0.1.0 `b84d5f1` 926) draws fences, railings, fence posts and stairs
(SceneModelArray, one light rig for a whole instanced group) with a patched pixel shader. v0.1.0:
`rT = max(ground atlas x cB.x, vC)` (vC = the rig's "vertex lights", usually 0). combined-final adds, when the fence VS
exports world y and the normal and the group's rig is outdoors: `rT = max(atlas x cB.x, vC, Q x cS.y)`, Q = the sum over
8 lamps of `colour x min(1, W x sat(N.l) / d^2)` at the pixel's world position, W = 0.4 x range. Lamps are chosen once
per group around the rig centre, so they do not change with the camera. Indoor railings and stairs (rig mode 0) keep the
ground light only. Evidence for the law: NOTAS 28/09 (`probe7_cerca`), and the comment on `AppendPixelLamps`
(`shader_patches.cpp` 661): the atlas around a street-lamp pair follows `2 w cos / d^2`, w = 0.2 x range x intensity.

##### 4.2 Pieces it needs (all from `combined-final`)
1. **`ReadLamp`** (`lot_light_bridge.cpp` 503): `out[7] = 0.4f * range` (was 0). The colour block's `.w`.
2. **`SelectPixelLamps(x, z, strength, lamps)`** (`lot_light_bridge.cpp` 621): new function. Reads only `g_allLamps`
   (`v[7]` = W), ranks by W/(d^2 + 1), threshold 0.002, fills `lamps[0] = (0, strength, 0, 1e-4)` and 8 pairs
   (pos + W, colour + 0), unused slots at 1e6 with colour 0. Writes `g_lastLampCandidates` (diagnostic only). Does
   **not** touch `g_lampData`.
3. **`ShaderPatches::AppendPixelLamps`** (`shader_patches.cpp` 661): new shared helper, 9 instructions per lamp (8
   lamps). Signature `(code, pw, Nrm, A, B, Q, cS, cL)`.
4. **`ShaderPatches::IsInstancedStructureVs(t, bool* worldY)`** (685): the optional out flag; true when the VS writes
   `mov oT2.xyz, rN` and `mov oT2.w, rW.y` exactly once each, `rW` being the register of `TEXCOORD1.zw` (all 9 captured
   variants VS_216C8910 ... VS_28228DD8 do). In `lot_light_bridge.cpp`: the map `g_instancedWorldY` filled by
   `ClassifyVs` for class 7 (`ClassifyVsCode` 403 in combined-final; in a v0.1.0 base just add it to `ClassifyVs`), and
   cleared in `Shutdown`.
5. **`ShaderPatches::PatchInstancedLamps(t, out, pixelLamps)`** (733) and `InstancedPatch::{pixelLamps, lampParamConst,
   lampConst}`: the pixelLamps variant needs TEXCOORD2 declared in the PS, 18 more constants (cS + 16 lamp registers)
   and 5 more temps, else it refuses (the draw then falls back to the plain variant).
6. **`RigTracker::CurrentCentre(float[3])`** (`rig_tracker.cpp`, reads rig+0x140 under `__try`, render thread only) plus
   `RigTracker::CurrentMode()` (already in v0.1.0). RigTracker must be installed: in v0.1.0 Night Lighting installs it
   only when "Doors, windows and counters get the ground light" (`objetosDeForaComLuzDoChao`) is on.
7. **`DrawInstanced` changes**: the second cache `g_fenceLampPs` (released in `Shutdown`), the choice between the two
   variants, get/set/restore of the 17 lamp constants around the draw, and the optional light-probe text
   (`g_objDrawInfo`, dev only).
8. **Lamp list refresh**: `LotLightBridge::OnPresent` refreshes `g_allLamps` every 20 frames only when roofs, water or
   object per-pixel lamps are on. A fence-only option must be added to that condition.

Not needed: `ConstGain` / `HdrOutput::LampGain()` (the approved look had gain 1), `ObjectGroundStrength`, the rig-keeping
and `sat(N.y + 1)` of the object patch, `PrecreatePs/Vs`, `NoteShader`, the `VsInfo` refactor.

##### 4.3 Can fences get the W falloff without changing objects and gates? Yes
- The falloff lives in three places only: the W in the lamp constants (`ReadLamp` `out[7]`, packed by
  `SelectPixelLamps`), the ranking in `SelectPixelLamps`, and the fence PS code from `AppendPixelLamps`.
- The v0.1.0 object path is independent of all three: `DrawObjectLamp` (`b84d5f1` 1046) selects with `SelectLamps` into
  `g_lampData`, packs `1/R^2` from the radius `v[3]`, and `PatchObjectLampPs` (`b84d5f1` 913) has its own inline
  `sat(1 - d^2/R^2)^2` loop. Keep that loop; do **not** switch `PatchObjectLampPs` to `AppendPixelLamps`.
- `out[7]` is also copied into `g_lampData[16 + k].w` by `SelectLamps` for roofs, snowy roofs and water; `roof_ps.hlsl`,
  `roof_snow_lamps_ps.hlsl` and `water_lamps_ps.hlsl` read only `lampCol[k].rgb`, and the v0.1.0 object packing copies
  colour `.rgb` only. So the new `.w` is inert everywhere except the fence path.
- Gates, doors and counters are drawn by `DrawObjectLamp` (object rig shaders), not `DrawInstanced`, so they stay exactly
  as in v0.1.0.
- The one coupling to undo: combined-final gates the fence lamps with the **object** option and strength
  (`g_objPixelLamps` = `luzPorPixelNosObjetos`, `g_objPixelLampStrength` = `forcaLuzPorPixelNosObjetos`). Give fences
  their own switch and strength (new keys) so object sliders no longer move the fences. **The approved look was
  made with `forcaLuzPorPixelNosObjetos = 1.76`** (current `S3SS.toml`), so a fence lamp strength default of about 1.76
  reproduces it; the ground term used `forcaNasCercas = 1.2`.

##### 4.4 Cost and risk
- GPU: per fence pixel 5 setup instructions + 8 x 10 lamp instructions + 2 (~87 ALU). Fences are a small share of the
  screen. Shader limit checked by the patch (maxConst + 20 < 224, maxTemp + 6 < 32).
- CPU: per fence draw one `CurrentMode`, one `CurrentCentre` (`__try` read), one pass over `g_allLamps`, 17 constant
  reads and 34 writes. Fence draws are instanced groups, few per frame.
- One more cached pixel shader per fence PS variant (DXVK compiles it at first draw, as for every patched shader).
- `g_instancedWorldY[g_curVs]` uses `operator[]` (inserts on a miss); harmless, but a `find` is cleaner.


#### 5. Each change in detail

##### #0 Split-Level Lighting Fix removed from the build (v0.2.0)
- Files: `Sims3SettingsSetter.vcxproj` (the `split_level_lighting_fix_patch.cpp` entry dropped); commit message of
  `f18cca8`: "Split-Level Lighting Fix removed from the build (Night Lighting lights every floor)".
- What it did: `BaseLight::GetLotID` (`0x006BC020`) returns 0 (13-byte body replaced).
- Look: see 2.3. Removing it re-created the lot-edge cut on houses on a foundation and removed type-11 lot lamps from the
  outdoor rooms of other stories.
- Cost: none.
- Kind: a removal decided by the mod (duplicate of Night Lighting's level sharing, per the commit message), not a reported
  request.
- Recommendation: do not re-add to Apex; enable it in official S3SS for the baseline (section 3).

##### #1 Fence per-pixel lamps (combined-final, 28/09 afternoon)
See section 4. Kind: experiment, then **approved after in-game testing**. Re-add first.

##### #2 Lot-edge story gate `BakeLevelStub` (combined-final, 28/09 afternoon)
- Files: `patches/night_terrain_relight_patch.cpp`: `kBakeLevelSite = 0x00C294D9` (86), `BakeLevelTest` (360),
  `BakeLevelStub` (382), install in `Install` (checks 11 bytes before and 6 after the 7-byte `cmp`), counters in the
  developer status.
- What: the bake `FUN_00C292B0` drops lot lights whose story `light+0xD0 != 0`. The stub keeps story 0 as the game does,
  keeps upper-story outdoor lot lamps (type 3..6, alive, enabled, room 0, lit) and refuses basements.
- Look: no straight cut at the border of lots with a house on a foundation (evidence `probe2_clara/escura`, lot
  `6C11001B51A4D8B0`, NOTAS "Corte na borda do lote", 28/09 tarde).
- Cost: one `__stdcall` with `__try` per lot light that reaches the gate, only during a bake. Negligible.
- Kind: **reported fix**, but the bug was introduced by #0's removal.
- Recommendation: redundant when Split-Level is on in official S3SS. Re-add only if Split-Level is not to be used
  Split-Level (it is narrower: no effect on the outdoor-room gather, lit outdoor lamps only). Test on the lot above.

##### #3 Batched shader log lines (combined-final)
- Files: `lot_light_bridge.cpp` `NoteShader` (249), `FlushShaderLog` (255), called from `OnPresent`; `PatchedFor`,
  `ObjectVsFor`, `FoliageVsFor`, `DrawFloorAtlas` log through it.
- What: "shader patched / refused" lines are counted per kind and written as one line at most once a second.
- Look: none. Cost: lowers render-thread disk writes when new shaders come into view (v0.1.0 wrote and flushed one line
  per shader synchronously). Warnings are still immediate.
- Kind: experiment (fluidity).
- Recommendation: optional; the standalone's `apex_log` already buffers (writer thread every 0.5 s), which removes most
  of the cost. Re-add only if the log gets noisy.

##### #4 Relight reconciliation and localized relight (combined-final, 28/09 afternoon)
- Files: `patches/night_terrain_relight_patch.cpp`: `Bakeable` (464), `CollectCurrent`, `MarkAllCovered`, `DiffBaked`
  (530), `Reconcile` (567), `Settle` (620), `OnPresent` (645); new option `relightLocal` ("Relight only around changed
  lamps"); removed the v0.1.0 triggers (load kick at 5 s, lamp-edit kick 0.7 s after `LotLampEdits` changed, the
  night-only stuck kick). `lot_light_bridge.cpp`/`.h`: `OutdoorLotLamp`, `RefreshOutdoorLotLamps` (577),
  `ForEachOutdoorLotLamp` (replace `TrackLotLampEdits` / `LotLampEdits`); `UpdateLampList` returns bool.
  `patches/smooth_streaming_patch.cpp`: `SmoothStreamingRelightTerrainRects` (946) / `RelightRectsImpl`, which reads
  each chunk's bake rect (`FUN_00C2A470` grid) and sets `chunk+0x55` (or queues it in Smooth Streaming's terrain queue).
- What: about once a second the lamps the bake takes are compared with what it was last given; differences are relit
  locally (rects of old and new place), or with a full rebuild at most every 15 s when the change covers more than 40 %
  of the terrain or the terrain cannot be read. Value-only changes (colour, brightness) at most every 5 s. After a world
  load one full rebuild once lit lot lamps have been stable for 3 s (5-60 s after load) instead of a fixed 5 s.
- Look: lamps switched on after they registered, lots streaming in, lamps edited, moved or removed update the ground by
  themselves (no need for "Rebuild terrain light now").
- Cost: **the main fluidity regression** (2.1). Also depends on Smooth Streaming for the chunk flagging (the standalone
  decision moves that into Night Lighting).
- Kind: **reported fix** (automatic relight).
- Recommendation: re-add only after a redesign and a test with the Frame Profiler: (a) only lit lamps and only while the
  night level is above ~0.5 (by day nothing on the ground changes; dusk and load already rebuild everything); (b) ignore a
  lamp that disappears from the enumeration unless its lot is still loaded (streaming out is not "switched off"); (c) a
  minimum interval of several seconds between local relights, batching the rects; (d) at most N chunks per relight
  released over several frames; (e) log each relight with the chunk count so churn is visible. Until then keep v0.1.0's
  triggers (they are the baseline).

##### #5 Gate darkness fix (combined-final, 28/09 afternoon)
- Files: `lot_light_bridge.cpp` `DrawObjectLamp` (1291): the rig is no longer zeroed; `ObjectGroundStrength()` (996) =
  max(1, `forcaNosObjetos`) for the ground term. `shader_patches.cpp` `PatchObjectLampPs` (1110): ground facing factor
  `sat(N.y + 1)` (def `cH = (1, 1, 1, 0)`, was `(0.5, 0.5, 1, 0)` for `0.5 + 0.5 N.y`); shapes A/B compute the rig's
  diffuse estimate `B` (3 `dp3`, `mul`, 2 `mad`) and add only `max(Q x s - rig - vC, 0)`; shape C takes
  `max(vC, Q x s, ground)`. Option hint texts updated ("never darker than the game's own object light").
- Look: a gate 9-26 m from 8 lamps no longer gets ~13 % of the nearest one (`probe4_portao`, 28/09); objects are never
  darker than the game, and upright faces get the full ground light, like the fence next to them. Side effect: brighter,
  flatter objects everywhere (2.4).
- Cost: ~6-10 ALU more per object pixel; no CPU change.
- Kind: **reported fix** (dark gate). Part of the darkness came from the configured `forcaNosObjetos = 0.38`, which in v0.1.0
  also scales the ground light on objects (0.19 x the ground light at the gate).
- Recommendation: keep out (maintainer feedback: objects looked no better, or worse). If dark gates return on the v0.1.0 baseline
  with `forcaNosObjetos` at 0.57 or 1.0, try only `ObjectGroundStrength` (ground term >= 1) first, then the facing
  factor, each tested alone; never together with #6.

##### #6 Object per-pixel kernel and lamp choice (combined-final, 28/09 afternoon)
- Files: `PatchObjectLampPs` switched from its inline kernel to `AppendPixelLamps`; `DrawObjectLamp` switched from
  `SelectLamps(m, 40)` + `1/R^2` packing to `SelectPixelLamps` (W = 0.4 x range); `ReadLamp` `out[7]`;
  `DescribeObjectDraw` text.
- Look: see 2.4 (5x more light at 10 m from a street lamp, light up to ~140 m). The previous kernel was 0 beyond
  R = 1.2 sqrt(range) while the atlas still shows the lamp there; that was the reason for the change.
- Cost: negligible (same instruction count per lamp; the selection loop is the same size).
- Kind: experiment.
- Recommendation: keep out. Note that v0.1.0's `DrawObjectLamp` overwrites the shared `g_lampData` with `SelectLamps`
  (also used by roofs and water); harmless because each roof and water draw selects again.

##### #7 Shader pre-creation (combined-final)
- Files: `lot_light_bridge.cpp`: `OwnCreatePs` / `OwnCreateVs` with `t_ownCreate`, `GameCodeBytes`, `ClassifyPsCode` /
  `ClassifyVsCode` + `VsInfo` (refactor of `Classify` / `ClassifyVs`), `Ensure*` split out of the draw functions,
  foliage VS pool (`CreateFoliageVs`, `PoolFoliageVs`, `ClearVsPool`, `kMaxPendingVs = 64`), `PrecreatePs` (2090),
  `PrecreateVs` (2118), two new registry hooks in `UpdateHooks` (2146) at `Priority::Last`. `shader_patches.cpp`:
  `CodeBytes`.
- What: when the game creates a shader, classify it and make the one-per-session replacements (lot pass, snowy lot pass,
  moon-shadow objects, roofs, snowy roofs, lake) and the patched foliage VS immediately, so DXVK translates them at load.
- Look: none.
- Cost/risk: see 2.5. Every game shader creation now pays a classification (hash tests, 58-entry wall table scan,
  sampler scan) and every VS a bytecode copy plus a `PatchFoliageVs` parse; up to five `D3DCompile` calls can run inside
  the create hook while the registry mutex is held; cross-thread writes to globals. Measured benefit: none (0 copies).
- Kind: experiment (fluidity).
- Recommendation: keep out. If first-draw compile hitches matter, compile the five HLSL replacements once on the render
  thread right after the device is created (no create hook, no mutex held by a foreign thread).

##### #8 HDR lamp-gain plumbing (v0.2.0 and combined-final)
- v0.2.0: `x HdrOutput::LampGain()` on roof, fence, snow-on-object, object ground, object per-pixel and wall strengths
  (`lot_light_bridge.cpp`).
- combined-final: `ConstGain` (161), `DrawLampGainOnly` (1734), `TerrainLampConst` / `ShaderPatches::LightMapScaleConst`
  / `LampScaleAfter` (`shader_patches.cpp` 100) and `scaleConst` in `RoadPatch` / `FloorPatch`; `PatchFoliageVs(t, gain)`
  with `LampWeightsOnlyScaleColours` (607); foliage `vsGain` copies; `RecordWorldChunk(record)`; `g_lampGainOn` keeps the
  hooks registered; water `params.z` (`water_lamps_ps.hlsl`, `water_lamps_hlsl.h`) = gain after the 0.8 clamp.
- Look: none in SDR (`s_lampGain` is 1 when HDR is off; every path returns early at gain 1).
- Cost: small bookkeeping, one-time extra parses in `PatchRoad` / `PatchFloor` / `PatchSnowFloor` / `PatchBakedAtlasPs`.
- Kind: HDR feature (removed).
- Recommendation: keep out. If the water shader is taken from combined-final, set `params.z = 1` or take the v0.1.0 HLSL.

##### #9 PostScene camera votes (v0.2.0)
- Files: `post_scene.cpp` `NearFromBlock`, `VoteNear`, `CameraNear`, `CameraViewProj`, `CameraDepthA`; `post_scene.h`
  (`kSsao` renamed `kAmbientOcclusion`).
- What: for the first 24 depth-tested back-buffer draws of each frame, read 7 vertex-constant blocks and vote the
  camera's near plane and view-projection.
- Look: none (used only by Ambient Occlusion; the Frame Profiler reads `CameraViewProj` as a fallback camera source).
- Cost: ~170 `GetVertexShaderConstantF` calls and some vector work per frame while any post-scene effect (Edge Smoothing,
  Depth Blur) is on.
- Recommendation: keep out ([features/depth-blur.md](../features/depth-blur.md) "Standalone baseline" already says so).

##### #10 Picture filters (combined-final, 28/09 night)
- Files: `hdr_output.cpp/.h` (`PictureParams`, `[qol.picture]`), the pass in `HdrOutput::OnEndScene`, scene copy in
  `BeforeOverlay`, `RegisterHooks` (Present, SetRenderTarget, both draw calls at `Priority::First`, registered once and
  never removed).
- Look: whatever is configured; the current values darken (exposure -0.93 EV, shadows -0.35) and sharpen (1.5, the
  maximum) the whole image, so lamp pools read dimmer and gradients harsher than in v0.1.0.
- Cost: scene copy (`StretchRect`), 1/2-1/4-1/8 downsamples when clarity is non-zero, one full-screen pass, deband;
  a cheap hook on every draw.
- Kind: experiment, kept by maintainer decision.
- Recommendation: kept; judge every lighting re-add with Picture off first.

##### #11 SMAA 1x (v0.2.0)
- Files: `patches/edge_smoothing_patch.cpp` (`SmaaShaders`, `CreateSmaaLookups`, `RunSmaa`, timestamp queries),
  `third_party/smaa/*`. New keys `metodo` (default 1 = SMAA) and `qualidadeSmaa` (default 2).
- Look: sharper edges and textures than FXAA. The maintainer ran Ultra (threshold 0.05, 32/16 search steps).
- Cost: 3 passes plus 2 extra render targets; 4 queries issued per frame (read back non-blocking); the three passes are
  compiled from the full SMAA.hlsl with `OPTIMIZATION_LEVEL3` at first use (a one-off hitch when turned on).
- Kind: experiment, kept by maintainer decision. v0.1.0 with the same config runs FXAA quality 2.
- Recommendation: kept; compare against FXAA once.

##### #12 Depth Blur off in map view (v0.2.0)
- Files: `map_view.cpp/.h` (script binding `Camera_IsMapViewModeEnabled`, found through its name at run time),
  `patches/depth_blur_patch.cpp` `StepMapFade`, `offInMapView`.
- Look: map view stays sharp, 0.3 s fade. Cost: one small function call per frame. Kept by decision.

##### #13 Buffered logger (combined-final)
- File: `logger.cpp` (flush thread, 1 s, warnings written at once, 64 KB cap, exit flush).
- Look: none. Cost: lowers render-thread disk I/O. The standalone has its own `framework/apex_log.*`. Kept (own code).

##### #14 Frame Profiler (combined-final)
- Files: `frame_profiler.cpp/.h`, hooks in `d3d9_hook_registry.cpp` (`RegistryHookTimingActive` check per draw hook),
  `dllmain.cpp` (`FrameProfiler::Shutdown`).
- Off by default: nothing hooked. On: 27 game-function detours, two registry hooks per draw, a file writer thread,
  optional sampling thread. It was on in the combined-final session (log `Timing 27 of 27`), which adds to what was
  felt in play. Kept, dev build only.

##### #15 Performance patches (combined-final)
Smooth Streaming (`smooth_streaming_patch.cpp`: lot budget per frame, shorter slices, room-lighting cap for loading
lots and for the current lot while the camera moves, terrain rebuild spread over frames), Script GC Scheduler, Service
Frame Budget, `gc_try_to_collect_patch.cpp` (6 lines), and the Smooth Patch Precise additions (0.5 ms timer request,
MMCSS "Games" / Above normal on render and simulation threads, `WaitOnAddress`). Removed from the standalone by decision.
Visual side effect of Smooth Streaming: room lighting of loading lots and of the current lot is capped, so lots can show
unfinished lighting longer, and terrain rebuilds are spread. The standalone moves only the chunk-flagging part of
`SmoothStreamingRelightTerrainRects` into Night Lighting, and only if #4 comes back.

##### #16 UI and text (v0.2.0)
`night_terrain_relight_patch.cpp`: one menu for both builds (main options, "Advanced", "Developer"), English strings,
the pond options moved to a Reflections section with their own reset (`ResetDefaults` no longer resets them), "Update
automatically at dusk" gets its own checkbox under Advanced (the "Lot lights light the ground outside the lot" checkbox
still turns both on). Strings only in `level_light_share.cpp`, `lightmap_smooth.cpp`, `object_light_bridge.cpp`,
`light_diag.cpp`, `light_probe.cpp`, `frame_capture_patch.cpp`, `depth_share.h` (comment). No render effect.

##### Also in the diff, no action
- `rig_tracker.cpp/.h` `CurrentCentre`: part of #1.
- `d3d9_hook.cpp`: HDR calls in `CreateDevice`, `Reset`, `EndScene` (removed with HDR). `HdrOutput::OnEndScene` ran
  every frame and early-returned with HDR and Picture off.
- `d3d9_hook_registry.cpp/.h`: `CallOriginalDrawIndexedPrimitive`, `CallOriginalDrawPrimitive`,
  `CallOriginalSetVertexShaderConstantF` (for the lot map probe, dev), profiler timing in the draw dispatch.
- `hdr_native.cpp/.h` (Native HDR; hooks only with HDR on and a lamp gain other than 1), `patches/ambient_occlusion_patch.cpp`
  (replaced `ssao_patch.cpp`, which v0.1.0 had; off in the maintainer's config), `patches/lot_map_probe_patch.cpp` (dev,
  idle unless capturing): removed or dev-only.
- Unchanged since v0.1.0: `d3d9_extra_hooks.*`, `floor_atlas_table.h`, `wall_lamp_table.h`, `roof_ps*.hlsl/.h`,
  `roof_snow_lamps*`, `shader_ids.h`, `level_light_share.cpp` logic, `lightmap_smooth.cpp` logic,
  `object_light_bridge.cpp` logic, `light_diag` / `light_probe` logic.


#### 6. State tracking

| # | Re-added in the standalone | Tested in game | Result |
|---|---|---|---|
| 0 | n/a (official S3SS option) | | |
| 1 | | | |
| 2 | | | |
| 3 | | | |
| 4 | | | |
| 5 | | | |
| 6 | | | |

Update this table when a change comes back; later entries in the notes win over this file.

**Outcome:** used as the re-add plan for the standalone: Split-Level parity first, fence per-pixel lamps first among
the code changes, the relight reconciliation only after a redesign. The state-tracking table above was not maintained;
later entries in the feature history pages and in `NOTAS-ILUMINACAO.md` supersede this audit. PostScene's camera votes
returned with Ambient Occlusion in 2.1.0.
