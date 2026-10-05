# Removed features

Archive of features that were removed from Apex Radiance or replaced by a different implementation. Each entry has the
same structure: **What it was**, **Why removed**, and **Revival notes** (where the code lives, settings, the findings and
pitfalls worth keeping, and what a revival would need). Nothing here describes current behaviour; current features are
listed in [README.md](README.md).

The removed code is in the combined build (S3SS and Apex in one ASI, git tag `combined-final`, commit 45e36e2, tree
`%USERPROFILE%\Desktop\S3SS-dev\Sims3SettingsSetter\`), which is the ground truth; file paths below are relative to
that tree. Scope rule: these features are not restored without an explicit request.

| Feature | Removed | Replaced by |
|---|---|---|
| [HDR output](#hdr-output) | 2026-09-28, standalone scope decision | Picture filters, SDR only ([features/picture-filters.md](features/picture-filters.md)) |
| [Native HDR](#native-hdr) | 2026-09-28, with HDR output | Nothing |
| [Ambient Occlusion (HBAO, combined build)](#ambient-occlusion-hbao-combined-build) | 2026-09-28 | Standalone GTAO since 2.1.0 ([features/ambient-occlusion.md](features/ambient-occlusion.md)) |
| [Smooth Streaming](#smooth-streaming) | 2026-09-28, no perceptible gain | Localized terrain relight moved into Night Lighting |
| [Script GC Scheduler](#script-gc-scheduler) | 2026-09-28, no perceptible gain | Nothing |
| [Service Frame Budget](#service-frame-budget) | 2026-09-28, no perceptible gain | Nothing |

## HDR output

### What it was
Real HDR output of the game: the back buffer switched to 16-bit float (`D3DFMT_A16B16G16R16F`), the swapchain colour
space to scRGB (Vulkan `VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT`, 1.0 = 80 nits), and one full-screen pass at the end of
each frame converting the finished SDR image to HDR: gamma 2.2 decode, the scene at a "paper white" level, the top of
the range expanded towards the monitor's peak, the UI (game UI and S3SS overlay) kept at its own brightness. Extras: sky
boost from scene depth, glow above white, OLED average-brightness limiter, wide-gamut push protecting greys and skin,
peak calibration pattern, developer diagnostics (`HDR_Diag_N`). UI: Display tab > "HDR" collapsing header
(`HdrOutput::RenderUI`). Config `[qol.hdr]`. Everything was read from the system (Windows HDR state, SDR white level,
monitor luminance) and could be overridden.

### Why removed
Scope decision of 28/09/2026: the standalone keeps only the Picture filters (SDR, see
[features/picture-filters.md](features/picture-filters.md)). HDR output and Native HDR are dropped with everything that
exists only for them.

Status at removal: the first version was run in game (diagnostics HDR_Diag_1/2, 28/09) and the scene/UI mask bug found
there was fixed; the later additions (sky, glow, limiter, gamut, calibration, Native HDR) have no in-game test result
recorded in the notes (unverified).

### Revival notes

#### Where the code lives (tag `combined-final`)

| File | What |
|---|---|
| `hdr_output.h/.cpp` | `HdrParams`, `HdrDisplayInfo`, `HdrOutput` (device hooks, pass, display query, UI, TOML); Picture is in the same files |
| `hdr_native.h/.cpp` | Native HDR (next section) |
| `third_party/dxvk/d3d9_vk_ext.h` | declarations of `ID3D9VkExtInterface` and `ID3D9VkExtSwapchain` (from DXVK `src/d3d9/d3d9_interfaces.h`, zlib) |
| `d3d9_hook.cpp` | `HookedCreateDevice` (approx. 405-437: `BeforeCreateDevice`, retry via `CreateDeviceFailed`, `AfterCreateDevice`), `HookedEndScene` (`BeforeOverlay` approx. 167, `OnEndScene` approx. 237), `HookedReset` (`BeforeReset` approx. 253, `AfterReset` approx. 258) |
| `config/config_store.cpp` | `HdrOutput::SaveToToml` / `LoadFromToml` (approx. 52 / 90) |
| `gui.cpp` | Display tab "HDR" header (approx. 600-610) |
| `lot_light_bridge.cpp`, `shader_patches.cpp` (`PatchFoliageVs(t, lampGain)`), `water_lamps_ps.hlsl` (`params.z`) | lamp gain consumers (Native HDR section) |
| `d3d9_extra_hooks` | `RawGet/RawSetDepthStencilSurface` used to unbind the INTZ depth while the sky boost samples it |

#### Settings (`[qol.hdr]`, for revival)

| UI label | key | default | UI range / mapping |
|---|---|---|---|
| Enable HDR | `enabled` | false | takes effect at next game start ("(restart the game)" shown while it differs from the device state) |
| Overall brightness | `brightness` | 1.0 | 0.25..4 (log); multiplies paper white and UI |
| Lamp strength | `lamp_power` | 2.0 | UI 1..6 (log), clamp 0.25..8; `LampGain = lamp_power^(1/2.2)` |
| Paper white | `paper_mode` | 0 | 0 = BT.2408 203 nits, 1 = Windows SDR content brightness, 2 = manual |
| Paper white (nits) | `paper_nits` | 200 | 80..500 (mode 2) |
| Sky brightness | `sky_boost` | 0.5 | 0..1; linear gain 1 + 2 x value on sky |
| Glow | `glow` | 0.35 | 0..1.5 (clamp 0..2) |
| UI at the paper white | `ui_auto` / `ui_nits` | true / 200 | 80..500 |
| Automatic peak | `peak_auto` / `peak_nits` | true / 1000 | 100..4000 (log) |
| Highlight expansion | `expansion` | 0.6 | 0..1 |
| Expansion start | `knee` | 0.5 | 0.1..0.95 |
| Highlight colour | `highlight_sat` | 1.0 | 0..1.5 |
| Limit full-screen brightness | `limiter` / `limit_auto` / `limit_nits` | true / true / 300 | 100..1000 |
| Wide colour (gamut) | `gamut` | 0.0 | 0..1 |
| Game gamma | `gamma` | 0 | 0 = 2.2, 1 = sRGB piecewise |

Before the Picture split, `[qol.hdr]` also held the grade keys; the Picture loader still migrates them (see
[features/picture-filters.md, Technical reference](features/picture-filters.md#technical-reference) ("Configuration migration")).

#### Key findings worth keeping
1. **The game clamps at 1.0.** HDR_Diag_1/2 (28/09): nothing above white in the game's output, gamma ramp identity, no
   sRGB-on-write draws into the back buffer, the input was *not* quantised to 8 bits (0.7-0.8% of mid-tones exactly on
   8-bit levels). So a 16-bit back buffer alone gives no highlights: HDR content must come from the mod (the lamp gain of
   Native HDR) or from expanding the top of the SDR range.
2. **Official DXVK route (no modified DXVK).** The game's `d3d9.dll` is the official DXVK 3.1.1 release (hash identical).
   DXVK >= 2.3 exposes:
   - `ID3D9VkExtInterface` (IID `65b55086-e3e3-4c3e-b3a0-86815cce2c4c`, on the `IDirect3D9`): `UnlockAdditionalFormats()`
     must be called **before** `CreateDevice`; then `BackBufferFormat = A16B16G16R16F` is accepted (and on `Reset`).
   - `ID3D9VkExtSwapchain` (IID `13776e93-4aa9-430a-a4ec-fe9e281181d5`, on `IDirect3DSwapChain9`):
     `CheckColorSpaceSupport` / `SetColorSpace(1000104002 = EXTENDED_SRGB_LINEAR)`, `SetHDRMetaData`,
     `GetCurrentOutputDesc` (EDID primaries, min/max/full-frame luminance).
   - Flow: `BeforeCreateDevice` (read config early if not loaded; only if Windows HDR is on for the window's monitor;
     query the extension, unlock, set FP16) -> on `CreateDevice` failure restore the original format and call again ->
     `AfterCreateDevice` / `AfterReset`: `ApplyColorSpace`. `BeforeReset` re-applies FP16 because the game asks for its own
     format again.
3. **System values.** Windows HDR on = DXGI `IDXGIOutput6::GetDesc1().ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020`
   for the output whose `Monitor` equals `MonitorFromWindow(game window)`; `MaxLuminance`, `MinLuminance`,
   `MaxFullFrameLuminance` from the same desc (includes a Windows HDR Calibration profile). SDR white:
   `DisplayConfigGetDeviceInfo(DISPLAYCONFIG_DEVICE_INFO_GET_SDR_WHITE_LEVEL)` on the path whose source GDI name matches
   the monitor, `nits = SDRWhiteLevel / 1000 x 80`. Monitor name from `GET_TARGET_NAME`. Refreshed at most every 2 s.
4. **A game gamma ramp breaks HDR.** If the game sets a non-identity `SetGammaRamp`, DXVK's final blit goes through an
   SDR lookup table and cuts everything above white (checked in `info.txt`; it was identity).
5. **Early config read.** The game can create its device before S3SS loads the config; `BeforeCreateDevice` parsed the
   TOML itself (as `BorderlessWindow::PeekEnabledEarly`). The standalone plan also required
   `ApexConfig::EnsureMigrated()` there.
6. **Tone mapping used** (luminance only, hue kept): `paper = brightness x (203 | SDR white | paper_nits)`,
   `peak = max(peak_auto && maxNits > 1 ? maxNits : peak_nits, paper)`, `white = 1 + expansion (peak/paper - 1)`,
   `t = saturate((L - knee)/max(1 - knee, 0.05))`, `y = L (1 + (white - 1) e t^2)` (e = limiter factor), roll-off above
   `k = 0.75 P`: `y = k + (P - k)(1 - exp(-(y - k)/(P - k)))`, P = peak/paper; highlights desaturated by
   `lerp(1, highlight_sat, t)`; scene out `x paper/80`, UI out `lin x ui/80`, mixed by the scene/UI mask.
7. **Scene/UI mask** (kept alive in Picture): scene copied at each depth-on -> depth-off back-buffer transition after
   >= 20 scene draws, last copy wins; a 2-primitive `D3DPT_TRIANGLESTRIP` `DrawPrimitive` right after the scene is the
   bloom composite (FrameCapture 24/09 #977), copy after it; mask `saturate(maxdiff x 64)`. The first version copied at
   the first depth-off draw: interiors have depth-off draws mid-scene, the copy missed most lamp light and 98.7% of the
   screen was treated as UI. Hooks must be `Priority::First` because lot_light_bridge returns `Skip` and cuts the chain.
8. **Extras (formulas).**
   - Sky: needs the readable INTZ depth (`DepthShare::Request(true)`, the Depth Blur swap), depth-stencil unbound while
     sampled; 5-tap cross (centre, +/-1.5 px x and y), 0.2 per tap with `depth >= 0.99999`;
     `g *= 1 + 2 sky_boost x e x sky x smoothstep(0.03, 0.4, L)` (dark skies stay dark).
   - Glow: from the 1/2 scene, `BrightPS` at 1/4 (4 bilinear taps, threshold 1.0 linear, soft knee `x^2/(x + 0.25)`,
     weight `1/(1 + L)` against flicker), `DownPS` to 1/8 and 1/16, `UpPS` (3x3 tent + add) back to 1/4, added as
     `glow/3 x result`. FP16 targets only.
   - Limiter (OLED ABL): after the pass the finished frame is reduced to 1x1 with linear `StretchRect`s; `ControlPS`
     updates a 1x1 FP16 ping-pong state `e = saturate(max(e0, 0.01) x (target/mean)^rate)`, `rate = 1 - exp(-dt/0.6 s)`,
     target = `MaxFullFrameLuminance / 80` (or `limit_nits`), initialised to 1, read by the next frame; scales the
     expansion and the sky; off while calibrating.
   - Gamut: Rec.2020->Rec.709 matrix applied to Rec.709 values (pushes vivid colours outward; scRGB keeps negatives),
     amount `gamut x smoothstep(0.1, 0.5, sat) x (1 - 0.85 skin)`, skin = hue near 30 deg with saturation < 0.4..0.8.
   - Calibration: black screen, a box at the chosen peak with an inner box at 10000 nits (disappears when the chosen peak
     reaches the real one); reference bars `100 x 1.32^n` nits, n = 0..10.
9. **Diagnostics** (dev build, HDR active only): `S3SS\HDR_Diag_N\` with `input_sdr.bmp`, `output_at_paper_white.bmp`,
   `output_at_peak.bmp`, `scene_copy.bmp`, `info.txt` (monitor values, used values, scene copy yes/no with the list of
   transitions, % treated as UI, gamma ramp identity/ACTIVE, back-buffer draws with sRGB write, input luminance
   percentiles, % of mid-tones on 8-bit levels, output nits percentiles, negative/invalid counts).
10. **Standalone plan, if revived** (PLANO-SEPARACAO.md 2c, 2d, step 6): the final pass would move to a registry Present
    hook at explicit priority about -500 (after the profiler's frame boundary at -1000, before the `Priority::First`
    frame-boundary hooks of post_scene, depth_blur and HDR itself); bind the back buffer and restore RT0/depth there
    (the current pass bails when RT0 is not the back buffer); no BeginScene/EndScene around it (would re-enter S3SS's
    EndScene and draw its menu twice); `BeforeOverlay` replaced by the same end-of-scene test at Present. Borderless
    (S3SS) and the FP16 format touch different `D3DPRESENT_PARAMETERS` fields, so both CreateDevice hook orders work.

#### Pitfalls (do not repeat)
- **HDR-mod DXVK fork.** On 28/09 the fork "HDR-mod" (Lilium/EndlesslyFlowering, v3.1-HDR-mod-v0.3.4) was installed in
  place of what was believed to be "DXVK 2.0" (backup folder `Backups Sims 3\10-DXVK 2.0 original`). That was a misread:
  the game's `d3d9.dll` was already the official DXVK 3.1.1 (hash identical to the release), and the backup is 3.1.1.
  The fork only upgrades formats (`d3d9.enableBackBufferUpgrade` / `upgradeBackBufferTo`, `enableSwapChainUpgrade` /
  `upgradeSwapChainFormatTo = rgba16_sfloat`, `upgradeSwapChainColorSpaceTo = scRGB`, `enableRenderTargetUpgrades` +
  `upgrade_*_renderTargetTo`); it does not convert SDR to HDR. It was removed and the original restored; everything was
  then done in the mod through the official extension. Do not reinstall a modified DXVK.
- The UI status text says "needs DXVK 2.3 or newer" (the version that added the extension interfaces).
- First-depth-off scene copy -> 98.7% UI (above). Hooks below `Priority::First` miss draws lot_light_bridge skips.
- The combined README still describes the grade controls under HDR > Advanced (stale since the Picture split).

## Native HDR

### What it was
The lamp light of the game and of the mod raised above white while HDR was active, so lamp-lit areas really exceeded
paper white. `HdrOutput::LampGain() = clamp(lamp_power, 0.25, 8)^(1/2.2)` (a gamma-space factor for a linear gain,
updated each frame in `OnEndScene`; 1 when HDR is inactive). Two parts:
- **"Stage B" (lot_light_bridge and friends):** each lamp-only constant or strength the Night Lighting code already sets
  was multiplied by `LampGain()`.
- **`hdr_native.cpp`:** the game's own shaders no other part of the mod touches (interiors), patched by pattern rules.

### Why removed

It only makes sense with HDR output and was removed with it by the 2026-09-28 scope decision.

### Revival notes

#### Where the code lives

Code: `hdr_native.cpp/.h`, the
`LampGain()` call sites below, `PatchFoliageVs(t, lampGain)` in `shader_patches.cpp`, `params.z` in
`water_lamps_ps.hlsl`, `g_lampGainOn` in `lot_light_bridge.cpp` (`OnPresent` keeps the bridge hooks on for the gain alone).
The standalone replaced these calls with 1 or removed them.

#### Lamp gain paths (Stage B, NOTAS "HDR nativo e saida HDR")

| Surface | Where the gain went | Source |
|---|---|---|
| Lot grass, summer | `c3.x` of the replacement lot pass (`kReplacementHlsl`) or of the game's pass (PS_29BEEB88, "mul r0.xyz, r0, c3.x", LightProbe-lote) | `lot_light_bridge.cpp` `OnDrawInner`, `DrawLampGainOnly` |
| Lot grass, snow | `c4.x` ("mul_pp r4.xyz, r0, c4.x", PS_2A13D200, LightProbe-neve) | `DrawLotSnow`, `DrawLampGainOnly` |
| World terrain chunks | constant found by pattern: `ShaderPatches::LightMapScaleConst` (single texld of the chunk light map sampler, next rgb reader `mul/mad ..., cK.x`, cK read nowhere else, not a def); c7 in PS_28CD3DB0 (summer, s8) and PS_295B4ED0 (winter, s11) | `TerrainLampConst` |
| Roads | exported `RoadPatch.scaleConst` (c4.x winter variants, c3.x summer), after the terrain max | `DrawRoad` |
| Floors / snow floors / baked-atlas floors | exported `FloorPatch.scaleConst` (e.g. c2.x in PS_1B3938E8, LightProbe-m08) | `DrawFloor`, `DrawSnowFloor`, `DrawFloorAtlas` |
| Lake water | `params.z` multiplies after `min(..., 0.8)` (the SDR clamp stays, HDR rises above it) | `DrawLake`, `water_lamps_ps.hlsl` |
| Summer foliage | VS `def c255` = (gain/1.5, 0.5 gain/1.5): gain folded into the lamp wrap weights r0.yzw (sun r0.x untouched); rebuilt after the gain holds 0.3 s; not for indoor rigs | `FoliageVsFor`, `PrecreateVs`, `PatchFoliageVs` |
| Outdoor walls, roofs, fences, per-pixel object lamps, object ground light | their strength constants x gain | `DrawWallGain`, `DrawRoof`, `DrawInstanced`, `DrawObjectLamp` |

The `ConstGain` helper scales `cK.x` for one draw and restores it; it does nothing when the gain is 1.

**Skipped paths:** the game's own light rigs (the per-rig brightness cap `FUN_006b92a0`, cap read at `0x006B9418` from
`0x011D0BA8`, includes the sun, so a lamp gain there is capped back or brightens the sun too); the winter bush
(`PatchFoliageVs` refuses: its VS passes r0.y to TEXCOORD1.w and its PS saturates it, so the gain would not be lamp-only).

#### Interiors (`hdr_native.cpp`)
- **The room light map is not lamp-only.** The CPU room solve (LightPointWithAllLights `0x0069FD60`, texels written by
  `FUN_006a31d0`) stores `rgb = min(sum of every light in the room list x room normalisation room+0x160, 1)` and
  `alpha` = an ambient weight. The room list also holds window lights (vtable `0x00FF43A8` "type 7" and `0x00FF4408`
  CircleWindowLight, seen in F8 dumps). So the gain was **night-weighted**: `interiorGain = 1 + (gain - 1) x night`, night
  = `lightMgr+0xF0` (0 day .. 1 night), read through the root getter at `0x006E97B0` verified by bytes
  `A1 <imm32> 85 C0 75 01 C3 8B 80 C0 01 00 00` (imm32 = &root; lightMgr = root+0x1C0), the same read as
  night_terrain_relight. Unknown night = day (no gain). The `min(sum, 1)` also means the map itself can never carry more
  than white.
- **Rule A** (196 PS: InteriorWall*, InteriorFloor, FloorTileCeiling, FloorWith*, Hideable, TerrainLight_Indoor*): a
  `texld rM` whose rgb is next read by `mad rX.xyz, rM.w, cAmbient, rM` (InteriorBuildingAmbientColor; c2 in
  InteriorWall, c4 in InteriorFloor, LightProbe-m80), optionally after the normal-map factor `mul rM, rK.s, rM`, with
  rM.w still the map's alpha.
- **Rules D + L** (72 PS: 40 Counters, 32 SingleObject/InstancedObject, none exterior): objects in room mode,
  `light = max(LightMap x sat(2 N.y) [x CounterLightingConfig.x], sum of 4 basis maps x sat(N.b) [x CLC.y], sky)`.
  D = exactly 4 texld weighted by a `dp2add_sat`/`dp3_sat` scalar; L = the one texld multiplied by the up-facing weight
  (`add_sat rW, rN.y, rN.y` or `mov_sat rW, rK.y`).
- A, D, L insert `mul rM.xyz, rM, cG.x` after each matched texld (rgb only). `cG` = highest used constant + 1 (limit 224
  SM3 / 32 SM2), free temp `rF` = highest temp + 1 (limit 32 / 12).
- **Rule S** (30 PS, TerrainLight variants such as the beach lot PS_29C97D28, LightProbe-arbusto2):
  `mul rQ.w, cA.x, cA.x; mul rM.xyz, rM, rQ.w`, cA read exactly twice: `cA.x x sqrt(gain)` around the draw, no shader
  change, not night-weighted. Not the 568-byte lot pass lot_light_bridge replaces.
- **Rule T, the floors' tanh curve.** LightingTweaks = (0.65, 1, 4.2, 0.25) exists only on the interior floors (6
  techniques, 104 PS, all also rule A; an earlier count of "13 families" was wrong). With `L = dot(rgb, 1/3)` the colour
  becomes `rgb x tanh(k L / 2) / L`, k = LightingTweaks.z, emitted as `mul rE, -rL, cT.z; exp; add 1; rcp; rcp rLi, rL;
  mad rE, 2, rE, -1; mul rS, rLi, rE`. It never reaches 1, so lamp light could not pass white on floors. Rule T inserts
  before the final mul `rcp rF.x, rLi; mad rF.y, rF.x, cG.y, cG.z; add rF.x, rF.x, -cG.w; cmp rE, rF.x, rF.y, rE`: equal
  to the game's curve up to y0 = 0.8, then its tangent. `cG.yzw = (slope, offset, L0)`, `x0 = atanh(0.8)`,
  `L0 = 2 x0 / k`, `slope = 0.5 k (1 - 0.64)`, `offset = 0.8 - slope L0` (for k = 4.2: L0 = 0.523, slope = 0.756,
  offset = 0.404), computed per draw from the bound LightingTweaks.
- Only shaders without flow control, predication or relative addressing; exact matches only. Validated offline against
  every PS of `Shaders_Win32.precomp` (scratchpad `hdrnative\sim.pl`, `sim2.pl`; names via `shd\knm.tsv`).
- **Mechanics.** Hooks: SetPixelShader `Priority::Last`, DIP/DP `Priority::Late` (after lot_light_bridge, whose
  replaced draws end with Skip and whose own shaders match no rule), CreatePixelShader `Priority::Last`. Patched copies
  are pre-created when the game creates the shader (usually during lot load) into a pool keyed by patched bytecode
  (cap 320; at most 268 PS can match), taken at the first draw. Per draw: set cG, bind the copy, draw, restore, `Skip`.
  Registered only while HDR is active and the gain is not 1. Status line: "Native HDR light: gain x, rooms y (night z) |
  shaders: ... room light, ... objects, ... tone curve, ... lot pass, ... refused | draws last frame: n". Log prefix
  `[HdrNative]`, batched to one line per second.

#### Pitfalls
- Treating the room light map as lamp light brightens daylight through windows: weight by the night level.
- Scaling the game's rigs (cap includes the sun) and the winter bush (PS saturation) was rejected, see Skipped paths.
- Do not assume the tanh curve is in many families: only the 6 interior-floor techniques (104 PS).

## Ambient Occlusion (HBAO, combined build)

### What it was

Screen-space ambient occlusion computed from the scene depth and applied to the finished 3D scene
before the UI: soft shading under furniture, in corners, around houses and trees. Patch name `AmbientOcclusion`,
Apex tab, settings `[patches.AmbientOcclusion]` (`intensidade` 1.0 [0-3], `raioM` 2.0 m [0.5-4], `protegerLuz` 0.5 [0-1],
`visualizar` 0 Normal / 1 Shading only), flagged experimental. The last version was installed 27/09 23:52 ("not tested in
game" in the notes); the log of the 28/09 14:20 session shows it running in game (`[AO] Installed`, `[AO] Resources
ready (3840x2160, pyramid 3840x2304)`, later `[AO] Uninstalled`), which is when it was judged in play.

### Why removed

Decision of 28/09/2026: the deterministic AO was judged **not good enough** in play, and AO (together
with HDR output and Native HDR) is not carried into the standalone. An earlier SSAO line had already been removed
on 27/09 after repeated twinkling complaints; maintainer feedback was that it did not work and should be removed completely.

### Revival notes

The standalone GTAO ([features/ambient-occlusion.md](features/ambient-occlusion.md)) replaced this implementation in 2.1.0; its study is in [history/ambient-occlusion.md](history/ambient-occlusion.md). These notes are kept for the HBAO design and the earlier SSAO line.

#### Where the code lives

Combined build, git tag `combined-final`, commit 45e36e2:
- `patches/ambient_occlusion_patch.cpp` (the deterministic AO, 710 lines): shader `kShaderSource` (lines 57-193:
  `LinearizePS`, `DownPS`, `AoPS`, `BlurPS`, `CompositePS`, compiled with `DIRS = 8`, `STEPS = 8`), `InitResources`
  (283-347), `RunAo` (472-569), `AoEffect` (572-583), patch class and UI (607-710).
- `post_scene.cpp/.h`: effect order `PostScene::kAmbientOcclusion = 10` (first, before Edge Smoothing 20 and Depth Blur
  30); `CameraNear()`, `CameraViewProj()`, `CameraDepthA()` were (re)added for it (they are not in the v0.1.0 commit
  b84d5f1 that the standalone's post-scene code is based on).
- `DepthShare::Request(true)` in `Install()` (released in `Uninstall()`) kept Depth Blur's INTZ depth swap running with
  Depth Blur off.
- The older SSAO/GTAO/HBIL line: `patches/ssao_patch.cpp` exists in commit b84d5f1 ("Night Remake alpha", the two-scale
  GTAO + HBIL version, 838 lines) and was deleted in f18cca8 (v0.2.0-alpha). The temporal versions exist only in the
  maintainer's backups: `Backups\9-S3SS compilado anterior\*.antes-remover-ssao`, `Sims3SettingsSetter.asi.ssao-folhas` (full
  source with the motion recorder and temporal v3), `ssao_patch.cpp.pre-folhas`.
- Offline labs (session scratchpad `ao\`): `lab.cpp` (aolab: seams / normals / sao / gtao / probe / gi), `motion.cpp`
  (analysis of in-game motion recordings), `msao.cpp` (deterministic study: still / shift / motion). Study data in the
  S3SS documents folder: `Profundidade\` (depth dumps `profundidade_N_WxH.f32`, `cor_N.bmp`, `info_N.txt`) and
  `Movimento_1..3\` (motion recordings).

#### History and why each version failed

Note: the notebook's date labels are inconsistent (some entries say 28/09 but precede "27/09, night" entries; the lab
files are all dated 27/09 17:39-23:52). The order below is the notebook's file order.

1. **SSAO, SAO style (26/09).** Half resolution, 8/12/16 spiral samples with per-pixel rotation (interleaved gradient
   noise), normal from depth, 9-tap depth-aware blur (10% tolerance), depth-guided upsample, multiply blend, fade from
   400 near-units. Worked in "near units" (z' = 1/(1-d)) on the belief that the projection had an infinite far plane and
   angles/ratios do not depend on scale. Lab verdict later: weak and blotchy, dotted on foliage.
2. **GTAO chosen in an offline lab (27/09).** A dev button saved `profundidade_N_WxH.f32` + colour + info; two scenes
   (upstairs room with furniture; street + mailbox). GTAO grounded objects (mailbox base, furniture feet, skirting,
   steps); radius 8.6 gave wide halos and arcs on walls, radius 3-3.5 with thickness 0-0.3 was clean; thickness 1.0 lost
   the contact under furniture. Shipped XeGTAO-style at half res (2x2/3x3/4x3 slices x steps, falloff 0.615, final power
   2.2, quantisation tolerance z'^2 x 1.2e-7, 2-px normal base, Jimenez multi-bounce).
3. **Two scales + HBIL (27/09).** Vertical stripes on a hill: the half-res pixel centre fell exactly between two depth
   texels and point sampling picked either (invisible in the lab, which read exact texels). Fix: snap every read to a
   texel centre, pixel centre `floor(uv*W - 0.25)`. Maintainer feedback asked for less dirty, more intense shade with more on trees: contact
   GTAO (r 3.5) + volume GTAO (r 20) + horizon-based indirect light (HBIL), MRT A16B16G16R16F x2, 1572/2750/4022 slots.
4. **Camera stability (28/09 label).** Complaint: blurry and "moving" when the camera moves. Cause 1: radius in near
   units while near varies 0.2-0.3 with zoom, so the shading grew and shrank; fixed with the per-frame near vote
   (`CameraNear`) and radii in metres (contact 0.8 m, volume 5 m, fade from 100 m). Cause 2: screen-fixed IGN noise not
   cancelled by a Gaussian; replaced by a 4x4 Bayer pattern + depth-aware 4x4 box (0.5,1,1,1,0.5 cancels it exactly) +
   a tent.
5. **Temporal accumulation + exact projection.** Found A = 1.00008, not 1 (far about 3 km): `z/near = A/(A-d)`; the old
   `1/(1-d)` erred little near and a lot far. c40..c43 confirmed as the camera view-projection. Reprojection with
   `prevVP * inv(view) * near` (0.05 px error with a still camera), history rejected off-screen or at >5% depth
   mismatch, clamped to the 3x3 min/max, weight 0.9.
6. **Temporal v2: "micro dots flickering".** Still present with bounce light off, so it was the shading itself. Lab
   (16 still frames): v1 clamped the history to the min/max of the already 4x4-averaged frame (range ~0), so the history
   followed each frame; per-frame rotation stayed within 1/16; 15-20% of pixels changed >2 levels per frame forever.
   Fix: accumulate the raw frame, clamp to the raw 3x3, per-pixel age (weight min(age/(age+1), 0.95)), full-circle
   rotation, age-adaptive 4x4 + tent. Lab: 0.28 mean change, 0.64% of pixels >2 levels. Screen videos (H.264) were
   useless for measuring (+/-5 levels of compression noise on grass).
7. **Temporal v3: leaves in the wind.** An in-game motion recording replayed in the lab showed the reprojection was
   right; the twinkling came from tree canopies: leaves sway, the depth test dropped the history at leaf edges and the raw
   frame showed through. v3 (4 point taps with a 5% depth test, bilinear fallback, age capped at 8) cut still-camera
   pixels jumping >6 levels from 1.2% to 0.24%, moving ~2% to ~1%. Not tested in game.
8. **SSAO removed (27/09 night)** at the maintainer's request.
9. **Deterministic AO, lab study (27/09 night, `msao.cpp`).** **Root cause of every "micro dots" / twinkling report:
   half resolution takes 1 of the 4 depth pixels of each 2x2; a 1-pixel camera move picks another one.** Even raw depth
   changed 1.7 levels (6% of pixels >2) on odd shifts, 0 on even; half-res AO 2.5-3.7 levels, 11-15% >6. Full resolution
   removes it. The "shift" test (move the image 1-4 px, compare with the shifted original, ideal 0) became the metric.
   A fixed spiral sum drew copies of outlines (discarded). A first HBAO candidate (16 fixed directions, 10 geometric
   steps, R 2.5 m, 5x5 tent) gave zero change with a still camera and 0.27-0.47 levels on shifts; maintainer feedback then found
   interiors strange and noisy, which led to the final interleave below.
10. **Deterministic AO shipped (27/09 23:52), tried in game 28/09, removed 28/09** (judged not good enough in play).

#### The final design (ambient_occlusion_patch.cpp), worth reviving as is

- Full resolution, no per-frame noise, no accumulation: identical output for identical depth (still camera = zero change).
- Passes per frame: scene depth -> 1/z (level 0), 8 downsample passes, the AO pass, 4 blur passes (box H, box V, tent H,
  tent V), one composite over a `StretchRect` copy of the scene (colour write RGB only).
- 1/z pyramid, 9 levels R32F (level 0 = `(A - d)/(near*A)`, sky 0), size padded to a multiple of 256 so each level is
  the exact 2x2 average (sky texels excluded); levels 1..8 rendered into one-level targets and `StretchRect`-copied in.
  1/z is linear across the screen on a plane, so planes stay planar at every level. Needs R32F filtering
  (`D3DUSAGE_QUERY_FILTER` check). AO targets R16F.
- HBAO with the surface normal (normal from the neighbour with the smaller depth difference per axis): **8 fixed
  directions x 8 geometric steps up to the radius R = 2 m** (metres), first step 3 px at 4K scaled with height, bias 0.08
  (sine), each step reads the pyramid level one finer than its spacing (bilinear within a level).
- **Per-direction golden-ratio phase (`frac(i x 0.618034 + b2)`) + 4x4 Bayer interleave** (direction rotation `b1` and
  step shift `b2` per pixel of the block), **cancelled by a depth-aware 4x4 box** (0.5,1,1,1,0.5 H and V: each
  interleave offset once) and then a tent (1,2,3,2,1), tolerance 3% of z.
- Thickness: the depth part of a sample's distance counts twice (removes the dark halo of a chair on the wall behind).
- Composite over a scene copy; lamp-lit pixels keep light: `lerp(ao, 1, saturate((luma-0.35)*2.5)*protect)`.
- Camera: near, A and tanX/tanY from `PostScene` (fallback near 0.25, `kTanHalfFovY = 1/4.293`).
- Lab result: indoor shift 0.23-0.32 levels, <= 0.16% of pixels >6; outdoor foliage about 0.5 levels, 0.9% >6 (the
  pattern does not cancel on leaves). Remaining: faint bands parallel to walls in close-ups (discrete horizon steps).
  fxc slots: AoPS about 257, BlurPS 105, DownPS 25, CompositePS 15, LinearizePS 8.

#### Facts it relied on (still valid, see engine/camera-and-map-view.md)

- `d = A - near*A/z`, A = 1.00008 (LightProbe-m80), near 0.2-0.3 per frame, voted from VS blocks c0/c4/c40/c180/c192/c216;
  camera view-projection in VS c40..c43; tanX/tanY = 1/|row0.xyz|, 1/|row1.xyz| (0.41421 / 0.23300 at 16:9; fallback
  `kTanHalfFovY = 1/4.293`).
- Depth precision: about 1.05e-5 per pixel on a road at z/near ~100-120 vs float ULP 6e-8 near 1 (+/-0.8% noise).
- The AO only ran when the bound depth-stencil was `DepthShare::Surface()` (the main scene, not a reflection pass).
- Needs the game's MSAA off; GPU cost was measured with timestamp queries and shown in the menu (no value recorded in
  the notes).
- Idea left open in the notes: check whether the backbuffer alpha is free, to separate ambient light from lamp light.

#### What reviving would need

1. Only on an explicit request (scope rule in CLAUDE.md). Superseded in practice by the GTAO of 2.1.0. Start from `ambient_occlusion_patch.cpp` at `combined-final`
   and from the lab (`msao.cpp`), not from the SSAO line.
2. Restore in the standalone's post-scene code: the camera votes (`CameraNear`, `CameraViewProj`, `CameraDepthA`) and
   the order slot 10; keep `DepthShare::Request` (the INTZ swap must run with Depth Blur off).
3. Add a fifth `preReset` user only after raising `RenderCallbacks::kSlots` (4 slots; with AO, Depth Blur, Edge
   Smoothing, Lot Map Probe and Night Lighting all on, one callback is silently dropped, see
   [features/depth-blur.md](features/depth-blur.md)).
4. Agree first on what "good enough" means (strength, radius, foliage behaviour), and measure with the
   shift test and a still-camera test before any in-game build. The known weak spots are foliage (pattern does not
   cancel on leaves) and faint bands near walls in close-ups.

## Shared evidence for the performance removals

Smooth Streaming, Script GC Scheduler and Service Frame Budget were removed together by the decision of 2026-09-28: no perceptible gain in game. The measurements agree with that:
- the 28/09 engine study found that limiting lot building and lot lighting did not reduce the spikes (NOTAS "Desempenho:
  mapa do motor");
- in the last profiler session (`S3SS_Hitches.txt`, session 2026-09-28 14:20:59), the render-thread services in the
  hitch frames cost well under 1.5 ms each. Typical values: Scene service 0.4-0.6 ms, service 00588A00 up to 1.26 ms,
  Swarm 0.2-0.4 ms, WorldManager about 0.2 ms, CAS SimService 0.03 ms, ResourceSystem 0.01-0.06 ms, TextureCompositor
  0.00 ms. So the budgets these patches cut were not where the hitches came from in that session.

The Frame Profiler that produced these numbers stays, dev build only ([features/frame-profiler.md](features/frame-profiler.md)).
Code for all three: combined build, tag `combined-final`. Engine background:
[engine/main-loop-and-services.md](engine/main-loop-and-services.md), [engine/lot-loading-and-streaming.md](engine/lot-loading-and-streaming.md),
[engine/mono-gc.md](engine/mono-gc.md).

Smooth Streaming, Script GC Scheduler and Service Frame Budget were measured with the frame profiler and gave no perceptible gain in play; some made lights and lots appear later. They were removed from the standalone. Code: combined build tag `combined-final` (`patches/smooth_streaming_patch.cpp`, `patches/gc_scheduler_patch.cpp`, `patches/frame_budget_patch.cpp`). The engine knowledge behind them is in `engine/` and in `S3SS-dev\SIMS3-PERFORMANCE-KNOWLEDGE.md`. Each entry below keeps the full feature documentation written before the removal, with headings demoted.

## Smooth Streaming

### What it was

Patch `patches/smooth_streaming_patch.cpp` (1248 lines), settings `[patches.SmoothStreaming]`, Steam only.

spread the per-lot build work of lot streaming over several frames. It had four parts:
1. A shared per-frame gate for lot load stages (`frameBudget` true, `frameBudgetMs` 6 ms).
2. Shorter slices per lot (`shortSlices` true, `sliceMs` 4 instead of the game's 20, `slicePriorityMs` 7 instead of 35).
3. A cap on the room lighting of loading lots (`lightingCap` / `lightingMs` 5, game 10), plus a cap on the current
   lot's lighting while the camera moves (`currentLotLightingMs` 6, game 15).
4. Spreading of the terrain light rebuild (`spreadTerrain`, `terrainChunksPerFrame` 2).

### Why removed

See [Shared evidence for the performance removals](#shared-evidence-for-the-performance-removals) It worked mechanically, but gave no measured benefit. In the last logged session its slice and lighting
values equalled the game's own, so only the 16 ms gate and the terrain queue were active.

### Revival notes

#### Findings that motivated it

- `FUN_00AEA680` runs lot load stages with a budget of **20 ms per lot** (mov at `0x00AEA6AC`), **35 ms for the
  current lot** (`0x00AEA6D0`, if `FUN_006FDC80(sceneObjMgr, lotId)`), and 2000 ms at `0x00AEA6E9` while loading. N
  loading lots cost N budgets in one frame.
- The lot lighting budget comes from `FUN_00ADB120` (10 ms loading / 30 ms current lot).
- The terrain light rebuild `FUN_00C845C0`, armed at `0xC84C3C`, flags every chunk at once.
- Install verified every site byte for byte and required it to be unique in the exe: the getter `0x006FDE10`, scene
  object manager `0x011D1CF8`, WorldManager `0x011ECBC4`, and the immediates 20 / 35.

#### Must survive the removal

`SmoothStreamingRelightTerrainRects` (smooth_streaming_patch.cpp, section from :822, entry at
:946). Night Lighting calls it (`night_terrain_relight_patch.cpp:70` declaration, `:592` call) for its localized
terrain relight. The standalone must move that function into Night Lighting, or Night Lighting loses the local relight.

Done: the standalone sets `chunk+0x55` on the chunks under a changed lamp from Night Lighting itself ([features/night-lighting/terrain-relight.md](features/night-lighting/terrain-relight.md), `features/terrain_chunk_relight.cpp`).

#### What a revival would need

re-validate the sites, then measure with the profiler before and after in a streaming-heavy scene. The
premise (lot builds cause the hitches) was not confirmed.

#### Full feature documentation (as written before removal)

> Spreads the per-lot build work of lot streaming over several frames. It covers four kinds of work: the lot renderer's
> load stages, the room lighting of loading lots, the room lighting of the current lot while the camera moves, and the
> terrain light rebuild. It also provides the localized terrain relight that Night Lighting calls
> (`SmoothStreamingRelightTerrainRects`). Patch name `SmoothStreaming`, display name "Smooth Streaming", category
> Performance, **experimental**, **Steam 1.67.2 only** (`VERSION_STEAM`; `Install` fails with "Needs the Steam version
> 1.67.2 of the game" on other builds). Same code in the public and dev builds. Code:
> `patches/smooth_streaming_patch.cpp` (combined tree, tag `combined-final`; the standalone copy in
> `S3SSApex/patches/` is byte-identical at the time of writing).
>
> **Status: works, but no measured benefit.** In the 28/09 engine study, limiting lot building and lot lighting did
> not reduce the frame spikes (NOTAS-ILUMINACAO.md, last section, "Medicoes"). The feature is kept because Night
> Lighting depends on its localized terrain relight, and the terrain spreading is harmless. Read "Pitfalls" before
> tuning it again.

##### Purpose

When lots stream in (camera moves, a lot is promoted to detailed view), the game does a lot of per-lot work in one frame:
- **Lot load stages.** `FUN_00AEA680` runs floors, walls, roofs, etc. with a 20 ms budget per lot per call, or 35 ms for
  the current lot. N loading lots cost N budgets in the same frame.
- **Room lighting of loading lots.** The budget comes from `FUN_00ADB120`: 10 ms while a lot loads, 30 ms for the
  current lot.
- **Terrain light rebuild.** When the terrain light countdown fires, every terrain chunk is flagged for a rebuild in
  one call.

Lot Streaming Optimizations (an S3SS patch) decides **when** lots load. Smooth Streaming limits **how much** of the
per-lot build runs in each frame. See the header comment of the source, lines 1-6.

##### User-facing settings

UI location: Apex tab > "Performance" > "Smooth Streaming" (collapsing header, open by default; `gui.cpp`
`RenderApexFeature("SmoothStreaming", "Smooth Streaming")`). Settings are saved in `S3SS.toml` under
`[patches.SmoothStreaming]` (plus `enabled`), through `OptimizationPatch::SaveToToml`. The standalone saves them in its
own config (see [architecture.md](architecture.md)). The source says: "Keys are the TOML names of saved configs:
never rename them."

| UI label | TOML key | Type | Default | Range (registered / UI) | Notes |
|---|---|---|---|---|---|
| Lot loading budget (ms per frame) | `frameBudgetMs` | int | 6 | 1-16 | Always visible. Disabled while `frameBudget` is off. The hook clamps it to 1..50 |
| Share one budget between all lots | `frameBudget` | bool | true | | Advanced. The frame gate (part 1b) |
| Shorter work slices per lot | `shortSlices` | bool | true | | Advanced. Patches the budget movs (part 1a). Toggling it reinstalls the patch |
| Slice per lot (ms) | `sliceMs` | int | 4 | 1-20 | Clamped to [1, 20], 20 being the game's value |
| Slice for the current lot (ms) | `slicePriorityMs` | int | 7 | 1-35 | Clamped to [1, 35], 35 being the game's value |
| Full speed while a world is loading | `fullSpeedWorldLoad` | bool | true | | The "burst" rule (see below) |
| Limit room lighting of loading lots | `lightingCap` | bool | true | | Part 2, ordinary lots |
| Room lighting per loading lot (ms) | `lightingMs` | int | 5 | 5-10 | The hook clamps it to 5..30. 10 = game |
| Limit the current lot's room lighting while the camera moves | `currentLotLightingCap` | bool | true | | Part 2, priority lots |
| Current lot room lighting while moving (ms) | `currentLotLightingMs` | int | 6 | 5-15 | Clamped to 5..15. 15 = game |
| Spread terrain light rebuilds | `spreadTerrain` | bool | true | | Part 3. Toggling it reinstalls |
| Terrain chunks per frame | `terrainChunksPerFrame` | int | 2 | 1-64 | The hook clamps it to 1..256 |

"Reset to defaults" (Advanced) restores the defaults above (`ResetDefaults`).

**Reinstall behaviour.** The hooks read every value live. `Update()` reinstalls the patch (through the base class's
2 s debounce) only when the set of installed parts changes. The parts are: lot hooks (`WantLot()` = frameBudget ||
shortSlices || lighting wanted), slices, lighting (`WantLight()` = lightingCap || currentLotLightingCap), and terrain
spread (`PartsMatchInstalled`).

**Values seen in the maintainer's config.** The session log of 2026-09-28 14:20 (`S3SS_LOG.txt`) has "frame budget true / 16 ms,
slices true / 20-35 ms, lighting cap true / 10 ms, terrain spread true / 64 per frame". Those slice and lighting values
equal the game's own, so in that session only the 16 ms shared frame gate and the terrain queue were doing anything.

##### How it works

###### Install (validate everything, then write)
1. `g_gameVersion != Steam`: fail.
2. For the lot parts, `VerifySite` checks each site. The pattern must match at the Steam address, and a scan of the
   whole module must find it there and nowhere else. The scan runs once per site per session and is cached. The sites
   are `kLotPass`, `kLoadStages`, `kBudgetMovs` and `kPriorityLot`. Then:
   - The call at 0xAEA6B4 (budget movs + 0x08) must reach 0x006FDE10, whose bytes are `A1 ?? ?? ?? ?? C3` (a getter).
     Its operand is the scene object manager pointer, which must be 0x011D1CF8.
   - The call at 0xAEA6C7 (+0x1B) must reach 0x006FDC80.
   - The `mov eax,[imm32]` operand at +0x2D must be 0x011ECBC4 (the WorldManager pointer).
   - The two budget immediates must be exactly 20 and 35.
3. For the lighting part, verify `kLightBudget`. Its `mov eax,[imm32]` at +2 must read the same WorldManager pointer.
4. For the terrain part, verify `kTerrainUpdate` and `kTerrainArm`.
5. `ResolveWorldGlobals()`:
   - It checks that 0xAEA6D8 is `A1 C4 CB 1E 01`.
   - It checks that 0xC6D68C is `8B 4E 58 E8` (`mov ecx,[esi+58h]; call`) and that the call reaches 0x00C845C0, or a
     target outside TS3W's image (for example the Frame Profiler's call-site redirect). This proves that
     WorldManager+0x58 is the terrain.
   - A different target inside TS3W sets `g_terrainCallOk = false`. That is not fatal: the camera then never counts as
     moving, and the terrain queue flush on uninstall is skipped.
6. Detours are installed on 0xC7CEA0 and 0xAEA680 (lot parts), 0xADB120 (lighting) and 0xC845C0 (terrain).
7. The code patches are tracked, and restored byte for byte on uninstall:
   - Slices: 0xAEA6AC and 0xAEA6D0, `C7 44 24 14 <imm32>` (8 bytes each), become `call CallBudget*Stub; nop x3`.
   - Terrain: 0xC84C43, `8B 87 B0 00 00 00` (6 bytes), becomes `jmp TerrainArmStub; nop`.
8. `FlushInstructionCache`, set the "live" atomics, and log
   `[SmoothStreaming] Installed (frame budget ..., slices ..., lighting cap ..., terrain spread ...)`.

Any failure after the first write calls `Rollback`, which restores the bytes, removes the hooks and sets `lastError`.

###### Per frame: the lot pass (render thread)
The chain is: WorldManager service `FUN_00C7E3C0` (runs only while WorldManager+0x41 != 0) → `WorldManager::Update`
0xC6D570 → lot pass `FUN_00C7CEA0(worldRenderer=[0x011ECE58], dt)` → for each lot renderer `FUN_00AEB2E0(node+8)` →
`FUN_00AEA680` while `+0x1E` (done) and `+0x1F` (failed) are both 0. See
[engine/lot-loading-and-streaming.md](engine/lot-loading-and-streaming.md).

`Hook_LotPass` (0xC7CEA0) wraps the original between `BeginPass` and `EndPass`. It records the pass thread and sets
`g_inPass`. The other hooks act only inside the pass and on that thread.

`BeginPass(now)`:
1. **Camera motion.** It reads the camera position that `FUN_00C845C0` copies to terrain+0x110
   (terrain = [WorldManager+0x58]). Speed > 0.5 m/s marks the camera as moving; it stays "moving" for 750 ms after the
   last motion (`kMoveSpeed`, `kMoveHoldMs`).
2. **World-load burst.** If `fullSpeedWorldLoad` is on and the previous lot pass was more than 1.5 s ago (or this is the
   first pass since install), `g_burst` is set. This works because the lot pass stops for the whole world load:
   `FUN_00C7E3C0` needs WorldManager+0x41, which `FUN_00C6CF80` sets at the end of a load (0xC6D430) and `FUN_00C6B780`
   clears on shutdown. The patch logs "Lot pass resumed after X s: full speed until the world's first lots are loaded".
   `EndPass` ends the burst when it has lasted at least 5 s and no lot did work for 1.5 s, or after 90 s, and logs
   "World's first lots loaded after X s: frame budget active".
3. **Per-call budgets.** When `shortSlices` is on and there is no burst, `g_callBudgetNormal` becomes
   clamp(sliceMs, 1, 20) and `g_callBudgetPriority` becomes clamp(slicePriorityMs, 1, 35). Otherwise both get the game's
   20/35.
4. **Anti-starvation.** It picks `g_guaranteed`: of the lots that asked last frame and were held back, the one that has
   waited longest. Slots not seen for 600 frames are pruned.

###### Part 1a: per-call slices (0xAEA6AC / 0xAEA6D0)
`FUN_00AEA680` sets its budget local `[esp+14h]` with three movs:
- 20 at 0xAEA6AC;
- 35 at 0xAEA6D0, if `FUN_006FDC80(sceneObjMgr, lotId)` is true;
- 2000 at 0xAEA6E9, if WorldManager+0x1B4 == 0 (tool mode).

The first two movs are replaced by `call` to a naked stub: `mov eax,[g_callBudget*]; mov [esp+18h],eax; ret` (the return
address shifts the offset by 4). EAX is dead at both sites, and `mov` does not touch the flags. The 2000 ms mov is never
touched.

The budget is in milliseconds of an EA stopwatch: unit 4, QPC scale `[0x011CB8FC]` = 1000/QPF (see
[engine/timers-and-sleeps.md](engine/timers-and-sleeps.md)). It is cumulative for one call and checked after each
stage (0xAEB102..0xAEB126), so one stage always runs and a slow stage overshoots.

###### Part 1b: the shared frame gate (`Hook_LoadStages`, 0xAEA680)
- A lot with work (`+0x1C` ready, `+0x1E` done == 0, `+0x1F` failed == 0) counts as loading.
- If the gate is off, or during a burst, or in tool mode, the original runs, timed.
- Otherwise the lot is served when any of these holds:
  - it is a priority lot (`FUN_006FDC80`);
  - it is `g_guaranteed`;
  - the frame's lot-building time so far is below `frameBudgetMs`;
  - it has waited 60 frames (`kMaxWaitFrames`).
- A lot that is not served gets `return 0` without running. That is the game's own "lock busy" result: `FUN_00AEA680`
  returns 0 when `TryEnterCriticalSection((LotRenderer+0x18)+0xD0)` fails (0xAEA783 → 0xAEB271), with no state
  changed. The caller `FUN_00AEB2E0` ignores the return value, so the lot continues on a later call.

###### Part 2: room lighting budget (`Hook_LightBudget`, 0xADB120)
The game values of `FUN_00ADB120` (ECX = lot lighting manager, result in ST0):
- ordinary lot: 5 ms, or 10 ms while loading (manager+0x4F);
- priority lot: 15 ms, or 30 ms while loading;
- tool mode: 1000 ms.

The hook calls the original, then lowers the result only when all of these hold: inside the lot pass, on the pass
thread, no burst, not tool mode.
- **Ordinary lot while loading** (`lightingCap`): cap at clamp(lightingMs, 5, 30). 5 ms is the game's own budget for
  every loaded ordinary lot, so the cap never goes below a value the game itself uses.
- **Priority lot** (`currentLotLightingCap`): only while the camera is moving; cap at clamp(currentLotLightingMs, 5, 15).

The budget is consumed by `FUN_00ADB8F0` (from `FUN_00AE4CB0` at the end of `FUN_00AEB2E0`), which always solves at
least one room (the elapsed check comes after `FUN_006A8BA0`). It passes the budget down to `FUN_006A3C90`, which
returns once elapsed >= budget while room+0x164 is set, and resumes from room+0xEC on the next call.

###### Part 3: terrain light rebuild spread (0xC84C43 + `Hook_TerrainUpdate`, 0xC845C0)
- **The game's behaviour.** In `FUN_00C845C0`, when the light countdown fires (consumed by `FUN_006B5770` at 0xC84C3E),
  the loop at 0xC84C43..0xC84C5E sets chunk+0x55 on every chunk of the vector terrain+0xB0/+0xB4. The per-chunk loop
  then rebuilds all of them in the same call (0xC85058..0xC850B5: `FUN_00C834F0`/`FUN_00C80E50`, `FUN_00C83060`,
  `FUN_00C7FA70`). That branch does not set the "work done this frame" flag `[esp+0Ch]`.
- **The replacement.** The arming loop is replaced with a jump to `TerrainArmStub`: `pushad; push edi; call
  OnTerrainArm; popad; jmp 0xC84C60` (resume = kTerrainArm + 0x24, a `call FUN_006FDE10`). At that point EAX/ECX/EDX
  are dead and EBX is live. `OnTerrainArm(terrain)` queues every chunk (`QueueAllLocked`) and releases up to K of them
  (`ReleaseLocked`).
- **Every later call.** `Hook_TerrainUpdate` releases more, nearest to the camera first. The distance uses chunk+0x0C/+0x10
  (int x/z) against the camera argument.
- **`ReleaseLocked`:**
  - It counts the chunks the game still has flagged ("in flight").
  - It releases `K - inFlight` new ones, where K = clamp(terrainChunksPerFrame, 1, 256).
  - Stall protection: if the in-flight count did not drop for 60 calls (`kMaxStallCalls`), one more chunk is released,
    never the whole queue.
- **Queue safety.**
  - A different terrain pointer (new world) clears the queue.
  - A changed chunk vector flags every current chunk (the game's behaviour) and clears the queue.
  - Turning the part off hands every pending chunk to the game.
  - `Uninstall` → `FlushTerrainQueue` flags the pending chunks, only if the terrain is still the world's, and logs
    "Handed N queued terrain chunks back to the game".
- **What the spread does and does not save.** The expensive part of a rebuild is not the +0x55 work but its
  consequence: +0x54 (`FUN_00C853B0`) makes the per-chunk loop re-render the chunk's four composited textures (mix,
  colour, light map, world leaf) through `FUN_00C7E7A0`. That branch *does* set the work-done flag, so the game already
  re-renders only one chunk per call (0xC85041). A full rebuild therefore costs roughly one texture re-render per frame,
  for as many frames as there are chunks. Spreading +0x55 removes the one-frame burst of the +0x55 work. Only rebuilding
  fewer chunks (the localized relight below) makes the whole rebuild shorter.
- **Arming test in live mode.** The test is "cells+0x3C == 0" (0xC84C14..0xC84C2B, BL = WorldManager+0x1B4 != 0),
  with no night condition. The +0x38 countdown alone (light register, remove or move) never triggers it. Details in
  [engine/terrain-and-light-bake.md](engine/terrain-and-light-bake.md).

###### Part 4: localized terrain relight for Night Lighting
`int SmoothStreamingRelightTerrainRects(const float* rects, int count, float maxFraction)` is exported to
`patches/night_terrain_relight_patch.cpp`, whose `Reconcile()` calls it with `kMaxLocalFraction = 0.4`.
- `rects` holds 4 floats per rect {minX, minZ, maxX, maxZ}, the same layout as light+0x134.
- It works **whether or not Smooth Streaming is enabled**. It resolves the WorldManager globals itself (Steam only). It
  queues through the terrain queue when the spread part is installed and on, and otherwise sets +0x55 directly.
- For every chunk it computes the chunk's bake rect and flags the chunk when that rect overlaps a lamp rect (plus a 1 m
  margin):
  - grid: `*(*(terrain+0x68)+8)`; cells at +0xBC; nx at +0xCC; nz at +0xD0;
  - cell index: (chunk+0x0C >> 8, chunk+0x10 >> 8);
  - cell record: {x0, z0, w, h} ints, read as unsigned, like `FUN_00C292B0` does.
- Return values:
  - the number of chunks flagged;
  - 0 when no chunk overlaps;
  - -1 when the terrain is unreadable;
  - -2 when more than `maxFraction` of the chunks would be hit. Nothing is flagged then; the caller does a full rebuild.
- Why it is correct: `FUN_00C292B0` bakes only the lights whose rect (light vfunc+0x2C = light+0x134, set by
  `FUN_006BDDF0`) overlaps the chunk's bake rect, and only lights with lot id 0 or light+0xD0 == 0 (the story gate that
  Night Lighting patches). So setting +0x55 on the overlapping chunks is exactly the game's full rebuild restricted to
  those chunks.
- The counters "local relights: N (M chunks)" are shown in the UI.

##### Files and functions

| File / function | Role |
|---|---|
| `patches/smooth_streaming_patch.cpp` header (lines 1-78) | RE notes 1-5, the ground truth for the analysis |
| `VerifySite`, `MatchAt`, `ParsePattern`, `CallTarget` | Site verification: pattern at the Steam address, unique in the module |
| `ResolveWorldGlobals` | WorldManager pointer, terrain link at 0xC6D68C |
| `CallBudgetNormalStub` / `CallBudgetPriorityStub` | Naked stubs replacing the 20/35 ms movs |
| `Hook_LotPass`, `BeginPass`, `EndPass`, `UpdateCameraMotion` | Frame boundary, burst detection, camera motion, statistics |
| `Hook_LoadStages`, `CallLoadStagesTimed` | Shared frame gate |
| `Hook_LightBudget`, `ReadLightingManager` | Room lighting caps |
| `TerrainArmStub`, `OnTerrainArm`, `QueueAllLocked`, `ReleaseLocked`, `Hook_TerrainUpdate`, `FlushTerrainQueue`, `MarkAllChunks` | Terrain queue |
| `RelightRectsImpl`, `ReadCellGrid`, `ChunkBakeRect`, `SmoothStreamingRelightTerrainRects` | Localized relight |
| `SmoothStreamingPatch::Install / Uninstall / Update / RenderCustomUI` | Lifecycle and UI |
| `patches/night_terrain_relight_patch.cpp` `Reconcile()` (approx. 564-619) | Only caller of the localized relight |
| `gui.cpp` `IsApexPatch`, `RenderApexFeature` | Keeps it out of the Patches tab and draws it in the Apex tab |

##### Game addresses and patterns

All addresses are TS3W.exe 1.67.2 Steam. Every pattern must match at the Steam address and be unique in the module
(`VerifySite`).

| Address | What | Pattern / check |
|---|---|---|
| 0x00AEA680 | `FUN_00AEA680` LotRenderer load stages, thiscall(lotRenderer), RET, returns AL | `81 EC BC 00 00 00 53 56 8B F1 33 DB 38 5E 1E 74 0B 5E B0 01 5B 81 C4 BC 00 00 00 C3 38 5E 1C 75 0B` |
| 0x00AEA6AC | Budget movs: +0x00 `mov [esp+14h],20`, +0x24 `mov [esp+14h],35`, +0x3D `mov [esp+14h],2000` | `C7 44 24 14 14 00 00 00 E8 ?? ?? ?? ?? 3B C3 74 1B 8B 4E 14 8B 56 10 51 52 8B C8 E8 ?? ?? ?? ?? 84 C0 74 08 C7 44 24 14 23 00 00 00 A1 ?? ?? ?? ?? 3B C3 74 10 39 98 B4 01 00 00 75 08 C7 44 24 14 D0 07 00 00` |
| 0x00AEA6D0 | 35 ms mov (patched with the priority stub) | inside the pattern above, +0x24 |
| 0x00AEA6E9 | 2000 ms mov (tool mode) | never written |
| 0x00AEA6D8 | `mov eax,[0x011ECBC4]` | bytes checked (`A1` + imm) |
| 0x00C7CEA0 | `FUN_00C7CEA0` lot renderer pass, thiscall(worldRenderer, float dt), RET 4 | `53 56 57 8B 79 1C 8B 37 85 F6 75 11 83 C7 04 39 37 75 08 83 C7 04 83 3F 00 74 F8 8B 37 8B 41 20 8B 49 1C 8B 1C 81 3B F3 74 34` |
| 0x006FDC80 | `FUN_006FDC80` priority-lot test, thiscall(sceneObjMgr, lo, hi), RET 8 | `8B 44 24 04 3B 81 D0 10 00 00 8B 54 24 08 75 08 3B 91 D4 10 00 00 74 15 3B 81 E0 10 00 00 75 08 3B 91 E4 10 00 00 74 05 33 C0 C2 08 00 B8 01 00 00 00 C2 08 00` |
| 0x006FDE10 | Scene object manager getter `mov eax,[0x011D1CF8]; ret` | `A1 ?? ?? ?? ?? C3`, operand must be 0x011D1CF8 |
| 0x00ADB120 | `FUN_00ADB120` lot lighting budget, ECX = manager, RET, ST0 | `51 A1 ?? ?? ?? ?? 85 C0 56 8B F1 74 12 83 B8 B4 01 00 00 00 75 09 D9 05 ?? ?? ?? ?? 5E 59 C3 8B 46 14 8B 48 48 8B 40 4C 50 51 E8 ?? ?? ?? ?? 8B C8 E8 ?? ?? ?? ?? 84 C0 74 30 80 7E 4F 00`; the imm at +2 must be 0x011ECBC4 |
| 0x00C845C0 | `FUN_00C845C0` terrain update, thiscall(terrain, float* camera, char), RET 8 | `55 8B EC 83 E4 F0 8A 45 0C 81 EC C4 00 00 00 53 56 57 8B F9 8B 4D 08 88 87 F4 00 00 00` |
| 0x00C84C3C | `mov ecx,esi; call FUN_006B5770` then the arming loop (+0x07 = 0xC84C43) and the resume (+0x24 = 0xC84C60) | `8B CE E8 ?? ?? ?? ?? 8B 87 B0 00 00 00 8B 8F B4 00 00 00 3B C1 74 0D 8B 10 83 C0 04 3B C1 C6 42 55 01 75 F3 E8` |
| 0x00C6D68C | `mov ecx,[esi+58h]; call FUN_00C845C0` in WorldManager::Update | `8B 4E 58 E8`; target 0xC845C0 or outside TS3W |
| 0x011ECBC4 | WorldManager singleton pointer | operand checks above |
| 0x011D1CF8 | SceneObjectManager pointer | operand of 0x6FDE10 |
| 0x011ECE58 | World renderer (ECX of the lot pass, from 0xC7E408) | disassembly of 0xC7E3C0 (engine_map) |

**Offsets used** (all read in the functions above):

| Object | Offset | Meaning |
|---|---|---|
| LotRenderer | +0x10/+0x14 | lot id (lo/hi) |
| LotRenderer | +0x1C / +0x1E / +0x1F | ready / done / failed |
| LotRenderer | +0x24 / +0x28 | stage counter (0..0x15) / sub-step |
| WorldManager | +0x41 | world active (lot pass runs) |
| WorldManager | +0x58 | terrain |
| WorldManager | +0x1B4 | world mode: 0 tool, 1 loaded (`FUN_00C6D970`), 2 edit / 3 save in-game (`FUN_00C5FE40`) |
| Lot lighting manager | +0x14 | lot; lot id at lot+0x48/+0x4C |
| Lot lighting manager | +0x4F | loading flag (set at stage 1, cleared at stage 0x14) |
| Terrain | +0xB0/+0xB4 | chunk pointer vector |
| Terrain | +0x110 | camera position (copied on entry of `FUN_00C845C0`) |
| Terrain | +0x68 → +0x08 | light-bake cell grid (cells +0xBC, nx +0xCC, nz +0xD0) |
| Chunk | +0x0C/+0x10 | int x/z |
| Chunk | +0x54 / +0x55 | re-render textures / relight |
| SceneObjectManager | +0x10D0 / +0x10E0 | the two "priority" lot ids (most likely the active or focused lot; not proven) |

##### Shader details
None.

##### Interactions

- **S3SS Lot Streaming Optimizations** (`patches/lot_streaming_optimizations_patch.cpp`, S3SS, all versions). The two
  are complementary, with no byte overlap (PLANO-SEPARACAO.md §3). LSO:
  - replaces `Lot::AddLotObjectsToScene` (0xAC1130) with a JMP to its object throttle (`objectsPerLot` 2, `delayMs` 16);
  - detours `WorldManager::Update` 0xC6D570 for the map-view blocker (sets WorldManager+0x258);
  - patches the camera-view bias JZ at 0xC63015 to a JMP;
  - sets the live settings "Throttle Lot LoD Transitions" = true, "... Max Active Lot Threshold" = 12 and "Camera speed
    threshold" = 5.0.

  With the LoD throttle on and fewer lots than the threshold, `FUN_00C6C290` promotes one lot at a time
  (`FUN_00C69FF0` blocks the next while a promoted lot is still loading). Smooth Streaming adds nothing there. Its frame
  gate is what keeps a burst cheap when that throttle is off. Smooth Streaming reads the bytes at 0xC6D68C (inside
  WorldManager::Update's body, not its prologue), so LSO's detour of 0xC6D570 does not disturb it. Details:
  [engine/lot-loading-and-streaming.md](engine/lot-loading-and-streaming.md).
- **Night Lighting** (`night_terrain_relight_patch.cpp`):
  - It uses `SmoothStreamingRelightTerrainRects` for local relights.
  - Its dusk and lamp "kicks" arm the game's countdown, and the resulting full rebuild goes through the queue when the
    spread is on.
  - Night Terrain Relight also calls `FUN_00C845C0` itself from its Present hook, on the same thread.
  - See [night-lighting/terrain-relight.md](features/night-lighting/terrain-relight.md).
- **Frame Profiler** (`frame_profiler.cpp`):
  - It times `FUN_00AEA680` and `FUN_00C845C0` at their per-frame call sites (0xAEB306, 0xC6D68F; 5-byte call
    redirects) instead of at the entries, precisely because Smooth Streaming verifies the entry bytes. With Smooth
    Streaming on, the profiler's "Lot load stages" and "Terrain update" times include the hooks.
  - Discrepancy: the profiler's header comment (approx. lines 59-61) says Smooth Streaming skips the terrain flush if it
    installs while the profiler is on. The current `ResolveWorldGlobals` accepts a call target outside TS3W, so this no
    longer happens. The code wins.
- **Service Frame Budget:** independent. The lot pass runs inside the WorldManager service (0xC7E3C0), which is not one
  of the four services Frame Budget limits. See [Service Frame Budget](#service-frame-budget).
- **Script GC Scheduler:** independent (simulation thread).
- **Standalone split:** PLANO-SEPARACAO.md §3 policy is "cooperate": defer the install until S3SS's startup patches are
  in, and byte-verify every site at install (already done by `VerifySite`).

##### Known limitations
- One load stage always runs per call, and a single stage (a big roof, walls) can take longer than any budget.
- The lot the game treats as current is never held back by the frame gate, and its load-stage slice keeps 35 ms unless
  `shortSlices` lowers it.
- The synchronous room solve of the lot **impostor** builder cannot be spread:
  - the chain is `FUN_00ADBAD0` → `FUN_006A80E0` → `FUN_006A3EC0`, while room+0xF0 == 3, with a 60000 ms budget;
  - it is reached only from `FUN_00AEB3F0` ← `FUN_00AD9E30`, state 2 of the impostor builder (jump table 0xADAF40;
    `FUN_00ADAF60` ← `FUN_00AE0540` ← 0xACB9A9, "Error during construction of imposter for lot %I64u");
  - it runs inside `FUN_00AD97E0`, which pumps the services until the job is done, with no Present in between;
  - the impostor snapshot needs finished lighting.
- "Priority lot" = the two lot ids at SceneObjectManager+0x10D0/+0x10E0. Most likely the active or focused lot; not
  proven.
- The world-load detection is a heuristic (a gap of more than 1.5 s between lot passes). No native loading-screen flag
  was found:
  - the loading screen is script driven (`GameUtils_SwapLoadScreen` → 0x7F1760 posts a UI callback);
  - `GameUtils_Begin/End/ResetLoadEvent` are empty stubs (0xC0FD60 = `ret`).

##### Pitfalls and failed approaches
- **Measured: no spike reduction.** NOTAS-ILUMINACAO.md, section "Desempenho: mapa do motor (28/09)":
  - about 5 ms per frame is "unattributed" (outside the render);
  - spikes above 50 ms create about 16 textures;
  - limiting lot and light work (Smooth Streaming) did not reduce the spikes.

  The profiler reports in `S3SS_Hitches.txt` (Documents\Electronic Arts\The Sims 3\S3SS) agree. Per-hitch averages /
  worst, in ms, over the last 200 hitches:

  | Report | Lot load stages | Lot lighting update | Terrain update |
  |---|---|---|---|
  | 2026-09-28 11:34 | 1.09 / 23.45 | 3.48 / 25.98 | 1.42 / 50.60 |
  | 2026-09-28 12:00 | 2.40 / 17.10 | 2.77 / 19.49 | 2.03 / 84.70 |

  In the 13:19 report, the dominant hitch cost is "Services (self)" (13.64 per hitch, worst 134.51), mostly CAS
  SimService, CAS TextureCompositor and main-thread resource jobs (job 0x7297C0). That led to
  [Service Frame Budget](#service-frame-budget). The Smooth Streaming settings active in those sessions were not recorded.
- **Tool-mode 2000 ms and 1000 ms budgets are not the loading screen.** WorldManager+0x1B4 == 0 is the engine's tool
  mode, never normal play. Do not "speed up loading" through them.
- **Releasing the whole terrain queue on a stall** re-creates the original one-frame rebuild. Release one chunk per
  stall only (`ReleaseLocked` comment).
- **Spreading +0x55 does not shorten a full terrain rebuild.** The per-frame cost is the one-chunk-per-call texture
  re-render (+0x54). Only relighting fewer chunks helps (Part 4).
- **Hook the per-frame lot pass only.** The lighting-budget hook checks `g_inPass` and the thread, so calls from the
  impostor builder or other paths keep the game's budgets.
- **SEH only in functions without C++ objects** (a file-wide rule, see the helper comment). The `__try` helpers are
  separate small functions for that reason.

##### Testing in game
- **Status line** (menu, under the header):
  - normal: "Status: N lots loading | lot building up to X ms/frame | N waits/s | terrain queue N";
  - during a burst: "Status: world loading, full speed (N lots loading)".

  Under Advanced: "Full terrain rebuilds: N | local relights: N (M chunks) | lighting capped: N/s | camera
  moving/still".
- **Log lines** (`S3SS_LOG.txt`):
  - `[SmoothStreaming] Installing...`
  - `[SmoothStreaming] Installed (frame budget ...)`
  - `Lot pass resumed after X s: full speed ...`
  - `World's first lots loaded after X s: frame budget active` (10.1 s in the 14:20 session)
  - `Handed N queued terrain chunks back to the game`
  - `[SmoothStreaming] Uninstalled`
- **Error texts:**
  - "<site> differs at 0x... (different game version, or another patch already hooks it)";
  - "pattern found at ..., expected ...";
  - "pattern is not unique";
  - "Scene object manager getter differs at 0x6FDE10";
  - "Priority lot call differs at 0xAEA6C7";
  - "Unexpected global addresses in the lot load code";
  - "Unexpected lot load budgets";
  - "Lighting budget code reads a different WorldManager";
  - "WorldManager access differs at 0xAEA6D8";
  - "Could not install the function hooks";
  - "Could not patch the lot load budget";
  - "Could not patch the terrain relight arming".
- **Frame Profiler** ([frame-profiler.md](features/frame-profiler.md)): compare "Lot load stages", "Lot lighting update" and
  "Terrain update" per hitch with the feature on and off, camera moving across a neighbourhood. "lots promoted" per hitch
  is in the hitch file.

##### Open items
- Decide whether parts 1-2 stay: no measured benefit so far. An A/B run with the profiler, same route, on/off, is
  missing.
- Prove what SceneObjectManager+0x10D0/+0x10E0 are.
- Replace the 1.5 s gap heuristic if a native "world loading" signal is found. Frame Budget uses WorldManager+0x41
  directly: both could share one definition.
- Update the Frame Profiler's outdated comment about the terrain flush.

## Script GC Scheduler

### What it was

Patch `patches/gc_scheduler_patch.cpp` (589 lines), settings `[patches.GcScheduler]`.

took over the explicit Mono/Boehm slice that the simulation thread runs on every
`MonoScriptHost::Simulate` pass (`call GC_try_to_collect` at `0x00D819AA`; target `0x00E4A050`, prologue-checked;
budget variable `0x011922A4`; heap globals `0x012225A0` / `0x012225B4`).
- It postponed the GC slice while the camera moved and let it run when the camera was still.
- The postponement was bounded by `maxPostponeSeconds` 3.0, `minFreeHeapMB` 64, `minFreeAddressSpaceMB` 384 and
  `catchUpMs` 250.
- Motion detection used `cameraSpeedThreshold` 0.25 m/s and `stillHoldMs` 300.
- It refused to install while S3SS's "GCTryToCollect" NOP patch was on.

### Why removed

See [Shared evidence for the performance removals](#shared-evidence-for-the-performance-removals). it was inert in the logged session: "GC calls run 47608, postponed 0, forced after the time limit 0,
forced by memory 240, longest 3.83 ms". Every time the camera moved, the 64 MB free-heap watermark forced the collection.

### Revival notes

#### Findings

- The game collects continuously, one time-boxed slice (0.5-1.5 ms) per Simulate pass on the simulation thread.
- The render thread does not wait for the simulation thread each frame, so GC time reaches the frame only indirectly.
- The long non-interruptible parts of a collection (`GC_finish_collection`, the forced finish, allocator-triggered
  collections) cannot be split by this patch.

#### What a revival would need

it would need evidence first that GC slices line up with visible hitches (profiler sampling of the
simulation thread), and a heap watermark that actually allows postponing.

#### Full feature documentation (as written before removal)

> Takes over the explicit Mono/Boehm garbage-collection slice that the simulation thread runs on every
> `MonoScriptHost::Simulate` pass (the `call GC_try_to_collect` at 0x00D819AA). While the camera moves, it postpones
> the slice. When the camera is still (which includes loading screens), it lets the slice run. Three limits always bound
> the postponement: a maximum time, a free-heap watermark and a free-address-space watermark.
>
> Patch name `GcScheduler`, display name "Script GC Scheduler", category Performance, **experimental**. It supports all
> versions (`VERSION_ALL`): the Steam addresses are cross-checked, other builds use patterns. Code:
> `patches/gc_scheduler_patch.cpp`. The standalone copy in `S3SSApex/patches/` is identical.
>
> **Status: works mechanically; no measured benefit yet, and it was inert in the one logged session.**
> - `S3SS_LOG.txt` of 2026-09-28 14:20 ends with "GC calls run 47608, postponed 0, forced after the time limit 0, forced
>   by memory 240, longest 3.83 ms".
> - With the default 64 MB free-heap watermark, the Boehm heap never had enough free space for a postponement: every
>   time the camera moved, the memory limit forced the collection.

##### Purpose

The game collects continuously: one time-boxed slice (0.5-1.5 ms budget) per Simulate pass on the simulation thread
(details in [engine/mono-gc.md](engine/mono-gc.md)). The idea is to move that work away from camera motion, when
hitches are most visible, and to catch up when the camera rests.

Two facts limit what this can achieve:
- The render thread does **not** wait for the simulation thread every frame. The UI runs on the simulation thread
  (NOTAS-ILUMINACAO.md, "Desempenho: mapa do motor (28/09)"). GC time therefore reaches the frame only indirectly.
- The long, non-interruptible parts of a collection (`GC_finish_collection`, the forced finish after 40 aborted
  attempts, allocator-triggered blocking collections) are not time-boxed by the game, and this patch cannot split them.

##### User-facing settings

UI: Apex tab > "Performance" > "Script GC Scheduler" (collapsed by default). Saved in `S3SS.toml` `[patches.GcScheduler]`
(plus `enabled`). The simulation thread reads every value live. `Update()` only clears `pendingReinstall`, so there is
never a reinstall.

| UI label | TOML key | Type | Default | Range | Notes |
|---|---|---|---|---|---|
| Maximum postponement (s) | `maxPostponeSeconds` | float | 3.0 | 0.5-10 | After this long of continuous postponement, one forced run plus the catch-up window |
| Minimum free script heap (MB) | `minFreeHeapMB` | int | 64 | 8-512 | Forced run when Boehm's free bytes `[0x012225B4]` fall below this |
| Camera speed threshold (m/s) | `cameraSpeedThreshold` | float | 0.25 | 0.01-5 | Advanced. Same key name as LSO's own setting, but in a different TOML table |
| Still time before collecting (ms) | `stillHoldMs` | int | 300 | 0-2000 | Advanced. How long "moving" lasts after the last motion sample |
| Catch-up after a forced collection (ms) | `catchUpMs` | int | 250 | 0-2000 | Advanced. No postponement during this window after a forced run |
| Minimum free address space (MB) | `minFreeAddressSpaceMB` | int | 384 | 64-2048 | Advanced. `GlobalMemoryStatusEx().ullAvailVirtual`, checked at most every 500 ms |

"Reset to defaults" (Advanced) restores the `Settings{}` defaults. "Reset counters" clears the statistics.

##### How it works

###### Install
1. **Refuse if the S3SS NOP patch is on.** If the patch "GCTryToCollect" is enabled, fail with: *Turn off "Chunky
   Patch - Disable GC_try_to_collect()" first: both patches take over the same GC_try_to_collect call.*
2. **`Resolve()`** (every failure sets `lastError`):
   - **Call site.** `gcTryToCollectCall`: Steam 0x00D819AA, `expectedBytes {E8}`; the pattern (offset 10) covers
     `push stopfunc; mov [start],eax; call; mov eax,[counter]; add esp,4; cmp ...; add eax,64h`.
   - **Target.** The call target must start with Boehm's `GC_try_to_collect` prologue
     `83 3D ?? ?? ?? ?? 00 74 06 FF 15 ?? ?? ?? ?? E8` (`cmp [GC_debugging_started],0; je; call [GC_print_all_smashed];
     call GC_notify_or_invoke_finalizers`). On Steam it must be exactly 0x00E4A050. Otherwise the error is "The call at
     0x... no longer targets GC_try_to_collect (another patch or tool changed it)".
   - **Budget variable** (display only): the `A1 imm32` at call + 0x15 (0x00D819BF); on Steam it must be 0x011922A4.
   - **Heap globals.** `mono_gc_get_used_size` (Steam 0x00E69670, pattern `56 E8 ?? ?? ?? ?? 8B F0 E8 ?? ?? ?? ?? 2B F0
     8B C6 33 D2 5E C3`). Its two callees are `mov eax,[imm32]; ret` getters, whose operands give heap size 0x012225A0
     and free bytes 0x012225B4 (checked on Steam).
   - **Camera source 1.** The WorldManager singleton store in the WorldManager constructor: Steam 0x00C67883
     `89 35 imm32`, giving 0x011ECBC4. The camera point store in the lot LOD scoring prologue: Steam 0x00C6C2B3,
     `0F 29 83 disp32` = `movaps [ebx+3A0h],xmm0`. A mismatch on Steam logs a warning and drops the source.
   - **Camera source 2.** The camera chain in `WorldManager::Update` + 0x4D (Steam 0x00C6D5BD):
     - `call 0x006E8330` (`mov eax,[0x011D1860]; ret`);
     - `mov ecx,eax; call 0x006E8400` (`mov eax,[ecx+24h]; ret`);
     - `movaps xmm0,[eax+60h]`.

     This gives [0x011D1860]+0x24 → camera, with the eye position at camera+0x60.
   - At least one camera source is required ("Could not find a camera position to watch on this game version").
3. **Reset state and redirect.** Reset the state, set `active`, and rewrite the call's rel32 to `HookedGcTryToCollect`
   (a tracked `WriteDWORD` at site+1). The patch logs
   `[GcScheduler] Installed at 0x00d819aa (GC_try_to_collect 0x00e4a050); heap globals 0x012225a0/0x012225b4;
   WorldManager 0x011ecbc4+0x3a0; camera [0x011d1860]+0x24+0x60` (exact line from the 14:20 log).

###### Every Simulate pass (simulation thread): `HookedGcTryToCollect(stopFunc)`
It is cdecl with one argument. The caller pops it (`add esp,4` at 0x00D819B4), and the return value is discarded (EAX
is overwritten at 0x00D819AF).
1. `UpdateCameraMotion(now)`, at most every 50 ms:
   - Sample both camera points: WorldManager+0x3A0 and the camera eye. Points that are non-finite or larger than 1e6 are
     rejected.
   - speed = max over the sources of |Δ| / Δt.
   - If speed > `cameraSpeedThreshold`, then `movingUntil = now + stillHoldMs + 50 ms`.
2. `moving = now < movingUntil`.
3. If moving and not inside a catch-up window:
   - Start or continue the postponement timer.
   - If it has not timed out (`maxPostponeSeconds`) and `MemoryLow()` is false, **return 0 without collecting**. 0 is
     what `GC_try_to_collect` itself returns when the stop callback aborts it.
   - Otherwise, count the reason (`forcedTimeout` or `forcedMemory`), open a catch-up window of `catchUpMs`, and
     collect.
4. Otherwise, reset the postponement and collect. `RunCollection` calls the original with the game's own stop callback
   0x00D717B0, so the game's time budget still applies. It times the call with QPC, keeping the last and longest call
   and counting calls longer than 4 ms.

`MemoryLow(now)`:
- Boehm free bytes `[0x012225B4]` < `minFreeHeapMB`; or
- the process's free virtual address space < `minFreeAddressSpaceMB`, re-checked at most every 500 ms (the game is
  32-bit).

###### Uninstall
1. Restore the original rel32 first.
2. Then clear `active`: a simulation thread already inside the hook simply collects.
3. Log the totals: `[GcScheduler] Uninstalled. GC calls run N, postponed N, forced after the time limit N, forced by
   memory N, longest X ms`.

##### Files and functions

| File / function | Role |
|---|---|
| `patches/gc_scheduler_patch.cpp` header (lines 1-53) | RE notes (what the game does, camera sources, compatibility) |
| `gcTryToCollectCall`, `monoGcGetUsedSize`, `worldManagerSingletonStore`, `worldManagerCameraPointStore`, `worldManagerUpdateCameraRead` | `AddressInfo` definitions: Steam address, pattern, expected bytes |
| `GcSchedulerPatch::Resolve` | Resolves and cross-checks everything |
| `HookedGcTryToCollect`, `RunCollection` | The scheduler |
| `UpdateCameraMotion`, `SampleCamera`, `PlausiblePoint` | Camera motion |
| `MemoryLow` | Heap and address-space watermarks |
| `GcSchedulerPatch::RenderCustomUI` | Status, counters, settings |
| `patches/gc_try_to_collect_patch.cpp` `Install` (approx. 33-37) | S3SS's NOP patch, with Apex's reverse refusal added in the combined build |
| `frame_profiler.cpp` `GcCallSiteText` (approx. 2825-2847), target `GC_try_to_collect (FUN_00E4A050)` | Profiler report of the call site and GC timing |

##### Game addresses and patterns

All TS3W.exe 1.67.2 Steam. Full GC background in [engine/mono-gc.md](engine/mono-gc.md).

| Address | What | How found / verified |
|---|---|---|
| 0x00D81840 | `MonoScriptHost::Simulate` (loops while [host+0xC08] == 1) | `dumpbin` disassembly; engine_map `f_D81840.asm` |
| 0x00D8199B | `call 0x004F32C0` on timer 0x011EE530 (start time) | `f_D81840.asm` |
| 0x00D819A0 | `push 0x00D717B0` (stop callback) | pattern `68 ?? ?? ?? ?? A3` |
| 0x00D819A5 | `mov [0x011EE524],eax` (start time) | same |
| **0x00D819AA** | `call 0x00E4A050` = `GC_try_to_collect`: **the redirected call** | `expectedBytes {E8}` + target prologue check |
| 0x00D819AF..0x00D81A03 | Budget adaptation of [0x011922A4] by ±100 µs when counter [0x011F2F98] hits [0x011947E8] = 20 or [0x011947EC] = -20, clamped to [0x011923A4] = 500 .. [0x01192324] = 1500 | disassembly; dwords.txt values |
| 0x00D819BF | `mov eax,[0x011922A4]` (budget load, call + 0x15) | read by `Resolve` |
| 0x00D717B0 | Stop callback: returns [0x011EE528] when elapsed > budget | disassembly (engine_map full.asm) |
| 0x011EE530 | Budget stopwatch, unit 3 = microseconds (created lazily; guard bit 0x011EE548 & 1) | disassembly |
| 0x011EE528 | "Stop allowed" flag, set to 1 by 0x00D70810 (GC_init's tail jumps there, 0x00E4E43A) | disassembly + source comment |
| 0x00E4A050 | `GC_try_to_collect` | prologue pattern |
| 0x00E69670 | `mono_gc_get_used_size` | pattern above |
| 0x00E4DD30 / 0x00E4DD40 | `GC_get_heap_size` / `GC_get_free_bytes` (`mov eax,[imm]; ret`) | `GetterGlobal` |
| 0x012225A0 / 0x012225B4 | Heap size / free bytes (`GC_large_free_bytes`) | getter operands |
| 0x00C67883 | WorldManager constructor `mov [0x011ECBC4],esi` | pattern `68 ?? ?? ?? ?? FF D0 89 35 ?? ?? ?? ?? E8 ?? ?? ?? ?? 3B C3 74 07 8B C8 E8`, +7 |
| 0x00C6C2B3 | `movaps [ebx+3A0h],xmm0` in lot LOD scoring `FUN_00C6C290` | pattern `55 8B EC 83 E4 F0 81 EC 84 08 00 00 A1 ?? ?? ?? ?? 53 8B D9 8B 4D 0C 0F 28 8B ?? ?? ?? ?? 0F 28 01 56 57 0F 29 83`, +35 |
| 0x00C6D5BD | WorldManager::Update + 0x4D camera read | prologue pattern `55 8B EC 83 E4 F0 83 EC 64 53 56 8B F1 83 BE B4 01 00 00 00 57 75` (shared with LSO), +0x4D |
| 0x011D1860 | App root. [root+0x24] = camera, camera+0x60 = eye | getter decoding |

WorldManager+0x3A0 holds the camera point that `FUN_00C6C290` stores. `FUN_00C6C290` also averages |Δ|/Δt of that point
over 10 samples (+0x310) as its own "Camera speed" and compares it with "Camera speed threshold" (WorldManager+0xEC).
`WorldManager::Update` skips that call while WorldManager+0x258 is set, which LSO's map-view blocker does in map view.
That is why the second camera source exists.

##### Shader details
None.

##### Interactions

- **S3SS "Chunky Patch - Disable GC_try_to_collect()"** (`GCTryToCollect`, `patches/gc_try_to_collect_patch.cpp`). It
  NOPs the **same 5 bytes** at 0x00D819AA. The two refuse each other in the combined build:
  - Scheduler → "Turn off \"Chunky Patch - Disable GC_try_to_collect()\" first: both patches take over the same
    GC_try_to_collect call."
  - GCTryToCollect → "Turn off \"Script GC Scheduler\" first: both patches take over the same GC_try_to_collect call."
    This check is an Apex edit to the upstream file (PLANO-SEPARACAO.md §1a, diff +30..35). **Upstream S3SS does not
    have it.**
- **S3SS "GC Finalizer Throttle"** (`GCFinalizeThrottle`). It patches the finalizer loop right after the call: the
  `cmp eax,0C8h` at 0x00D81A1D (200 → 32767) and the `jnz` at 0x00D81A37 (NOPed). PLANO lists the pattern starts
  0xD81A1B / 0xD81A30; the bytes actually written are at +2 and +7. Different bytes: they combine.
- **S3SS "GC_stop_world() Optimization"** (`GCStopWorld`). It patches 0x00E511F5 inside `GC_stop_world`. They combine.
  See [engine/mono-gc.md](engine/mono-gc.md) for what that patch really does.
- **Frame Profiler.** It Detours the **entry** of 0x00E4A050 ("Script GC" category) at its first frame boundary after
  being turned on, and reports the call site ("GC call site: redirected to 0x... by another patch (e.g. Script GC
  Scheduler); timed whenever it calls GC_try_to_collect", seen in every `S3SS_Hitches.txt` report of 28/09).
  - Scheduler installed first (the normal startup order), then profiler: works. The scheduler calls 0xE4A050, which
    lands in the profiler's detour.
  - Profiler on first, then the scheduler enabled: *(inferred from the code, not observed)* `Resolve()`'s prologue check
    sees the Detours `E9` at 0xE4A050 and fails with "The call at ... no longer targets GC_try_to_collect". Turn the
    profiler off, enable the scheduler, then turn the profiler on again.
  - On non-Steam builds, the profiler's detour of 0xC6C290 breaks camera source 1's pattern (it starts at the entry);
    the scheduler then falls back to source 2 (profiler comment, approx. lines 62-63).
- **LSO map-view blocker:** see camera source 2 above.
- **Smooth Patch (S3SS):** the tick rate sets how often Simulate passes, and therefore how often this hook runs.
- **Standalone split** (PLANO-SEPARACAO.md §3, table row "Script GC Scheduler"). The Apex guard inside S3SS's file is
  lost in the split, and upstream S3SS only checks `expectedBytes {E8}`, so it would NOP over Apex's redirected call.
  Planned policy, **not implemented in the combined code**:
  - **Refuse at install** if the 5 bytes are `90 x5`, or if `S3SS.toml [patches.GCTryToCollect].enabled = true`.
  - **Watchdog** (1 s, on Apex's pump thread): check that the E8 target is still Apex's hook. If it is not, show
    "overridden by S3SS", stop, and **do not restore** on uninstall (leave the foreign bytes).
  - **Upstream PR:** GCTryToCollect should verify the call target.

  See the common `ApexConflictGuard` design in PLANO §3.

##### Known limitations
- It only affects the explicit slice at 0x00D819AA. It does not affect:
  - allocator-triggered collections. An allocation that finds no free block goes `GC_allocobj` (0x00E4A2F9) →
    `GC_collect_or_expand` 0x00E4A120 → 0x00D70800 → 0x00E4A320: a blocking full collection on the allocating thread,
    finished with `GC_never_stop_func`;
  - the finalizer loop that follows the call;
  - `GC_finish_collection` or the forced finish after 40 aborted attempts, inside a slice that does run.
- **The default free-heap watermark can make the feature inert.** Evidence: 14:20 session, 240 forced-by-memory runs, 0
  postponements. The scheduler cannot postpone unless Boehm's free bytes stay above `minFreeHeapMB` while the camera
  moves. Lowering the watermark risks the blocking allocator path that the watermark exists to avoid.
- A postponement returns 0 on every pass. The game's counter [0x011F2F98] and the budget adaptation still run after the
  call *(what the counter counts is unverified)*.

##### Pitfalls and failed approaches
- **Do not NOP the call** (what Chunky Patch does). The game then relies on allocator-triggered collections, which are
  blocking full collections: the long stalls the scheduler tries to avoid. The scheduler returns "not collected" instead
  and keeps the limits.
- **Two patches on one call site.** Restoring one while the other is on silently undoes or bypasses the other. That is
  why the two refuse each other. Keep that behaviour in the standalone (watchdog, never restore foreign bytes).
- **Measured GC cost.** Frame Profiler "Script GC" is always on the simulation thread:

  | Report | Total | Calls | Average per call | Per hitch avg / worst |
  |---|---|---|---|---|
  | 2026-09-28 11:34 | 36804.6 ms | 23916 | 1.54 ms | 1.58 / 3.15 ms |
  | 2026-09-28 12:00 | 24456.1 ms | 14824 | 1.65 ms | 1.64 / 3.47 ms |
  | 2026-09-28 13:19 | 51469.2 ms | 40874 | 1.26 ms | 1.42 / 3.94 ms |

  The 14:20 log's "longest 3.83 ms" matches. These are small next to the render-thread service spikes (see
  [Service Frame Budget](#service-frame-budget)). Moving GC work around is unlikely to fix render hitches on its own.

##### Testing in game
- **Status lines:**
  - "Status: postponing (camera moving)" / "collecting (limit reached, catching up)" / "collecting (camera still)";
  - "Camera speed: X m/s";
  - "GC calls run: N | postponed: N | forced by the time limit: N | forced by memory: N";
  - "Last GC call: X ms | longest: X ms | over 4 ms: N";
  - "Script heap: X MB, free Y MB";
  - "Game's GC time budget per call: N us" (should move between 500 and 1500).
- **The key check:** watch "Script heap ... free" while moving the camera. If free < `minFreeHeapMB`, every moving
  sample is a forced run, and "postponed" stays 0.
- **Log:** the `Installed at ...` line (verifies every resolved address) and the `Uninstalled. GC calls run ...` totals.
  Failures in `Resolve` appear as the menu error text and `LOG_WARNING` lines for dropped camera sources.
- **Frame Profiler:** "Script GC" per hitch (simulation-thread column), and the "GC call site:" line in the report.

##### Open items
- Find a heap watermark that actually allows postponement without triggering allocator collections. Measure free bytes
  over a session first.
- Implement the standalone conflict guard and watchdog (PLANO §3).
- Measure whether postponing GC changes render hitches at all, given that the render thread does not wait for the
  simulation thread.
- Identify what [0x011F2F98] counts. The budget adaptation depends on it.

## Service Frame Budget

### What it was

Patch `patches/frame_budget_patch.cpp` (850 lines), settings `[patches.FrameBudget]`.

gave the four time-sliced render-thread services one shared budget per frame (`totalMs` 4, minimum
slice `minSliceMs` 0.5). The per-service caps were `capJobsMs` 2.5, `capResourcesMs` 1, `capCompositorMs` 1.5 and
`capSimBuildsMs` 2. It also deferred the Sim build step (`deferSimBuilds`, `maxSimDefers` 3) and forced Sim slicing
(`forceSimSlicing`). While a world was loading, and for `graceSec` 10 s after, every service got the game's own budget.

### Why removed

See [Shared evidence for the performance removals](#shared-evidence-for-the-performance-removals). no perceptible gain. Single build steps and finalize jobs cannot be split by a budget, and the rest were
already small.

### Revival notes

#### Findings (static RE, dumpbin of the raw exe)

- `ServiceManager::Update 0x00588E00` calls `0x0059ED20`, which calls each service's vtable +0x1C in registration
  order. Four services have their own **5 ms** slice, checked only after each work item, so the slices add up
  (10-20 ms when several have work).
- The four services:
  - JobManager: `0x00599A10`, budget `[svc+0xCC]`, arm at `0x00599A20`.
  - CAS SimService: `0x005F0E50`, `+0x2C` = 5 ms, `+0x139` = slicing on.
  - TextureCompositor: `0x00608630`, loop `0x00608270`.
  - ResourceSystem: `0x007377F0`, at least 250 ms while its flag `+0x1F0` is set.
- Budget timers are EA stopwatches: ctor `0x004F35B0`, arm `0x004F34F0`, unit 4 = ms, unit 3 = µs. The patch switched
  units to µs for finer budgets.
- **SimService phase B at `0x005F129D`** always runs at least one full build step (ModelBuilder `0x005DC800`, texture
  composite `0x005CED00`), even with the slice used up. So a slice-limited frame costs the slice plus one build step:
  the 9-15 ms frames measured were that, and the 40-147 ms frames were single build steps. The patch's `SimGateStub`
  (jump at `0x005F127D`) could skip phase B for a frame.
- The **async resource loader's finalize job `0x007297C0`** runs on the main thread (affinity mask 1, created at
  `0x00729C5B`; its read job `0x0072A4F0` runs on the worker threads). Resource construction (parse, decompress, GPU
  object creation) therefore happens on the render thread, one resource per job, inside the JobManager slice.
- World-load flag: WorldManager `[0x011ECBC4]+0x41`.

#### What a revival would need

the only lever with real potential is what a budget cannot cut: the phase B build step and the
per-resource finalize. That needs moving or splitting work, not budgets. The profiler integration still keys services
by vtable entry, so it keeps working with or without these detours.

#### Summary written at removal

- Purpose: stop the render-thread services' 5 ms slices from adding up in one frame. One shared budget (default `totalMs` 4, `minSliceMs` 0.5) split among JobManager `[svc+0xCC]` (int ms, read at 0x599A20), ResourceSystem `[svc+0x14]` (int ms, floor 250 ms while +0x1F0 is set), CAS TextureCompositor `[+0x18]` (unit byte at 0x60829D switched 4 -> 3 = µs) and CAS SimService `[+0x2C]` with slicing forced via `[+0x139]` (0x5F0EBF rewritten). Detours on the four update entries 0x599A10, 0x5F0E50, 0x608630, 0x7377F0; a "pass" restarts when a service runs a second time.
- SimService phase B: when the slice is used up, phase A (checked at 0x5F1262) jumps to phase B at 0x5F129D, which always runs one full build step before checking time at 0x5F139A. The patch replaced 0x5F127D (32 bytes) with a stub that could jump to the tail 0x5F13C6 instead, at most `maxSimDefers` (3) times in a row.
- Full game values while WorldManager+0x41 is off and for `graceSec` (10 s) after it turns on.
- Result: single work items (one Sim build step, one resource finalize job 0x7297C0, one composite 0x5FDEF0) still overshoot; no perceptible gain. Removed.

#### Full feature documentation (as written before removal)

> Makes four time-sliced engine services share **one** small time budget per frame, instead of 5 ms each. All four run
> every frame on the render thread:
> - JobManager main-thread jobs (resource loading finalize);
> - ResourceSystem (deferred resource callbacks);
> - CAS TextureCompositor (object texture compositing);
> - CAS SimService (Sim model and outfit builds).
>
> It also defers the Sim build step that the game runs after the Sim service's slice is used up.
>
> Patch name `FrameBudget`, display name "Service Frame Budget", category Performance, **experimental**, **Steam 1.67.2
> only** (`VERSION_STEAM`). Code: `patches/frame_budget_patch.cpp`. The standalone copy in `S3SSApex/patches/` is
> identical.
>
> **Status: implemented, not tested in game.** No `[FrameBudget]` line appears in the only session log kept
> (`S3SS_LOG.txt`, 2026-09-28 14:20), and the notes record no result. The data that motivated it is in the Frame
> Profiler report of 2026-09-28 13:19 (see Pitfalls).

For the main loop, the ServiceManager, the service list and the thread model, see
[engine/main-loop-and-services.md](engine/main-loop-and-services.md). This page covers only what this patch
changes.

##### Purpose

`ServiceManager::Update` (0x00588E00 → loop 0x0059ED20) calls each service's vtable +0x1C, in registration order, on the
render thread (the main thread). Four of the services time-slice their work, each with its own 5 ms slice, and each
checks its deadline **after** a work item. So:
- when several services have work in the same frame, the slices add up (4 x 5 ms);
- a single item can overshoot its slice.

In the CAS SimService it is worse:
- when phase A (the request list) uses up the slice, phase B still runs **one full build step** (ModelBuilder
  0x005DC800, texture composite 0x005CED00, ...) with no deadline check before it;
- a slice-limited frame therefore costs "phase A up to the slice + one full build step";
- the source comment attributes the 9-15 ms frames measured to that, and the 40-147 ms frames to single build steps.

NOTAS-ILUMINACAO.md, "Desempenho: mapa do motor (28/09)": "Services on the render thread with their own 5 ms budget
each (checked only between items) ... Several in the same frame add up to 10-20 ms = typical spikes." ObjectDesigner
0xB3A960 has no budget and is not handled.

##### User-facing settings

UI: Apex tab > "Performance" > "Service Frame Budget" (collapsed by default). Saved in `S3SS.toml`
`[patches.FrameBudget]` (plus `enabled`). The source says: "Keys are the TOML names of saved configs: never rename them."
The render thread reads every value live. `Update()` only clears `pendingReinstall`, so there is never a reinstall.

| UI label | TOML key | Type | Default | Range | Notes |
|---|---|---|---|---|---|
| Frame budget (ms) | `totalMs` | float | 4.0 | 1-16 | Shared by the four services per frame |
| Defer Sim build steps | `deferSimBuilds` | bool | true | | Skip phase B for this frame when the Sim slice is used up |
| Minimum slice per service (ms) | `minSliceMs` | float | 0.5 | 0.1-2.0 | Advanced. Floor for every service even when the frame budget is gone (code floor 0.05) |
| Main-thread jobs maximum (ms) | `capJobsMs` | float | 2.5 | 0.1-5 | Advanced |
| Resource callbacks maximum (ms) | `capResourcesMs` | float | 1.0 | 1-5 | Advanced. Rounded to whole ms, at least 1 (its timer counts whole milliseconds) |
| Texture compositor maximum (ms) | `capCompositorMs` | float | 1.5 | 0.1-5 | Advanced |
| Sim builds maximum (ms) | `capSimBuildsMs` | float | 2.0 | 0.1-5 | Advanced |
| Sim build deferral limit (frames) | `maxSimDefers` | int | 3 | 0-30 | Advanced. After N deferred frames in a row, the step runs |
| Time-slice Sim builds even when the game turns it off | `forceSimSlicing` | bool | true | | Advanced. Applies the budget even when SimService+0x139 == 0 |
| Game budgets while a world loads | `fullSpeedWhileLoading` | bool | true | | Advanced |
| Game budgets after a world load (s) | `graceSec` | int | 10 | 0-60 | Advanced. Grace period after WorldManager+0x41 turns on |

"Reset to defaults" restores `Settings{}`. "Reset counters" clears the statistics.

##### How it works

###### Budget units
The game's budget timers are EA stopwatches (`ctor 0x004F35B0(this, unit, 0)`; arm `0x004F34F0(this, value, flag)`,
deadline = now + value / scale). The value is read as an **unsigned** 32-bit integer, and 0xFFFFFFFF means unlimited.
- Unit 4 = milliseconds, scale `[0x011CB8FC]` = 1000/QPF.
- Unit 3 = microseconds, `[0x011CB900]` = 1e6/QPF.

The patch switches three of the services to **microseconds**, so budgets below 1 ms are possible. Timer details:
[engine/timers-and-sleeps.md](engine/timers-and-sleeps.md).

###### Install
1. Steam only.
2. `Validate()`:
   - every site in the table below: the pattern matches at the Steam address and is unique in the module;
   - each vtable's +0x1C still points to the expected update function;
   - the compositor update still calls 0x00608270 at +0x1E and +0x31;
   - the ResourceSystem update still calls 0x00737560 at +7;
   - the three Sim jump targets have their expected bytes.
3. Hooks start in pass-through mode (`g_passThrough = true`). `g_assumeSettled` is set when a world is already active
   (turned on mid-game: no grace period).
4. Detours are attached on the four update functions. Their entries are 6, 6, 9 and 7 relocatable bytes, with no
   branches. The trampoline pointers live in `g_slots[]`.
5. Code patches, all tracked and restored byte for byte:
   - **0x0060829D** (compositor loop, stopwatch unit): `04` → `03`. The compositor field still holds the game's 5 (ms)
     until the hook first writes it, so that is 5 µs for at most one frame.
   - **0x00599A20** (10 bytes): `8B BE CC 00 00 00 6A 00 6A 04` (`mov edi,[esi+0CCh]; push 0; push 4`) →
     `8B 3D <&g_jobBudgetUs> 6A 00 6A 03`. Load and unit change in one write.
   - **0x005F0EBF** (21 bytes): `80 BF 39 01 00 00 00 74 05 8B 77 2C EB 03 83 CE FF 6A 00 6A 04` (the slicing test,
     `mov esi,[edi+2Ch]` or `or esi,-1`, `push 0; push 4`) → `8B 35 <&g_simBudgetUs> EB 09 90x9 6A 00 6A 03`.
   - **0x005F127D** (32 bytes, the phase A loop test up to 0x005F129D) → `E9 <SimGateStub> 90 x27`.
6. `FlushInstructionCache`, pass-through off, then log `[FrameBudget] Installed (total X ms, minimum slice X ms, caps
   a/b/c/d ms, Sim build deferral on/off (max N), forced Sim slicing on/off, game budgets while loading on/off + N s)`.

###### Per service call (render thread): `RunService(i, ...)`
The call passes through unchanged when:
- `g_passThrough` is set;
- the object is null;
- the object's vtable is not the expected one.

Otherwise:
1. **`BeginService(i, now)`.** If this service already ran in the current "pass" (or no pass is open), the previous
   pass ends (`EndPass`: statistics, "over 2x budget" counter) and a new one starts. `UpdateMode()` runs at that point.
   This is independent of service order, and the nested ServiceManager passes of the lot impostor pump (0x00AD97E0)
   count as passes too.
2. **`UpdateMode()`**, with WorldManager = [0x011ECBC4] and +0x41 as "world active":
   - Not active: `g_limiting = !fullSpeedWhileLoading`. Status "world loading, game budgets" or "no world".
   - Active: the grace period starts at the first active pass, logging "World active: game budgets during the grace
     period". `g_limiting` turns on after `graceSec` (status "world loaded, game budgets for N s more"), then status
     "limiting (shared budget active)".
3. **`PrepareBudget(i, obj)`.** `PickUs(i, gameMs)` = min(totalMs - spentThisPass, capMs[i]), then at least `minSliceMs`,
   and never above the game's own value. The result is in µs.

   | Service | Game value | What the patch writes |
   |---|---|---|
   | JobManager (`kJobs`) | [svc+0xCC] int ms (default 5) | `g_jobBudgetUs` (µs), read by the patched load at 0x00599A20. Not limiting: game ms x 1000 |
   | SimService (`kSim`) | [svc+0x2C] ms if [svc+0x139] else unlimited | `g_simBudgetUs`. Not limiting: game ms x 1000, or 0xFFFFFFFF when slicing is off. Limiting and slicing off: 0xFFFFFFFF unless `forceSimSlicing` |
   | TextureCompositor (`kTex`) | [svc+0x18] int ms (ctor sets 5); [svc+0x15] = unlimited (only the `-generateThumbnails` option sets it, 0x006085A5) | **Writes the field** [svc+0x18] in µs (the loop's unit is now µs). The game's value is re-captured whenever the field differs from what the patch last wrote (`GameValue` / `FieldState`) |
   | ResourceSystem (`kRes`) | [svc+0x14] int ms (ctor 0x00739120 sets 5) | Writes the field in **ms** (unit unchanged, because 0x0072A730 is also called from 0x00737060 with its own value): round(µs/1000), at least 1 |
4. **Sim service:** `g_simGateArmed = g_limiting && deferSimBuilds` before the call. After the call, if the build step
   was not deferred, the consecutive-defer counter resets.
5. **`Account(i, ticks)`:** adds the service's time to the pass total, and records last, average (EMA 0.02) and maximum.

###### The Sim phase-B gate (`SimGateStub`, jumped to from 0x005F127D)
Entry state: EDX:EAX = deadline - now; EDI = the service. Only EDI and the stack are live at the three targets.

The stub reproduces the original test exactly:
- `mov [esp+54h],edx`;
- negative → time is up;
- otherwise `cmp byte [esp+0Fh],0; jne phaseB`;
- `lea eax,[edi+58h]; cmp [esp+10h],eax; jne loopTop (0x005F0F10)`;
- else phase B (0x005F129D).

When time is up, it calls `SimDeferBuild()` (preserving EAX/ECX/EDX):
- 1 → jump to 0x005F13C6, the function's normal tail ("more work?" flag, stats call, epilogue), which skips phase B for
  this frame;
- 0 → phase B as usual.

`SimDeferBuild` returns 0 when the gate is not armed. When `maxSimDefers` consecutive defers are reached, it returns 0,
counts a "forced" run and resets the counter.

###### Uninstall (order matters)
1. Pass-through on, gate disarmed, `SetGameBudgets()`. The two patched loads now hold the game's values: the Sim budget
   from +0x139/+0x2C in µs, the job budget 5000 µs until the original load is back.
2. Restore the code bytes (job, Sim budget, Sim gate).
3. `RestoreFields()`. Restores the compositor and ResourceSystem fields only if they still hold the patch's last value.
   The compositor field is restored while its stopwatch is still in µs, so a value in ms read as µs is harmless.
4. Restore the compositor unit byte.
5. Remove the hooks last. Then log `[FrameBudget] Uninstalled. Passes N, over 2x budget N, Sim build steps deferred N
   (forced N), longest pass X ms`.

Any failure during install goes through `Rollback` (the same order).

##### Files and functions

| File / function | Role |
|---|---|
| `patches/frame_budget_patch.cpp` header (lines 1-51) | RE notes for the four services and the patch design |
| `kJobUpdate`, `kSimUpdate`, `kSimBudget`, `kSimGate`, `kTexUpdate`, `kTexLoop`, `kResUpdate`, `kResInner`, `kWorldActive` | Verified sites |
| `VerifySite`, `MatchAt`, `CallTarget`, `Validate` | Install-time verification |
| `RunService`, `Hook_Jobs/Res/Tex/Sim`, `BeginService`, `EndPass`, `UpdateMode`, `PickUs`, `PrepareBudget`, `GameValue`, `WriteField`, `Account` | Per-frame budget logic |
| `SimGateStub`, `SimDeferBuild` | Phase-B deferral |
| `FrameBudgetPatch::Install / Uninstall / Rollback / RestoreFields / SetGameBudgets` | Lifecycle |
| `FrameBudgetPatch::RenderCustomUI` | Status, per-service table, settings |
| `frame_profiler.cpp` `kServiceNames` (approx. 606-622) | Per-service names in the profiler (keyed by the update function) |

##### Game addresses and patterns

TS3W.exe 1.67.2 Steam. Every pattern must match at the address and be unique in the module.

| Address | What | Pattern / check |
|---|---|---|
| 0x00599A10 | JobManager service update, thiscall(svc, float, float), RET 8 (vtable 0x00FD9810 +0x1C) | `83 EC 20 56 8B F1 83 BE C8 00 00 00 00 74 3A 57 8B BE CC 00 00 00 6A 00 6A 04 8D 4C 24 10 E8 ?? ?? ?? ?? 6A 01 57 8D 4C 24 10 E8 ?? ?? ?? ?? 8B 46 18 8B 40 24` |
| 0x00599A20 | `mov edi,[esi+0CCh]; push 0; push 4` (budget, ms) | patched (10 bytes) |
| 0x005F0E50 | CAS SimService update, thiscall(svc, float, float), RET 8 (vtable 0x00FE514C +0x1C) | `55 8B EC 83 E4 F0 81 EC 94 00 00 00 53 56 57 8B F9 89 7C 24 24 E8` |
| 0x005F0EBF | Slicing test + budget load + unit | `80 BF 39 01 00 00 00 74 05 8B 77 2C EB 03 83 CE FF 6A 00 6A 04 8D 4C 24 68 E8` |
| 0x005F1262 | QPC; deadline - now; phase A loop test (patched from +0x1B = 0x005F127D, 32 bytes) | `8D 4C 24 18 51 FF 15 ?? ?? ?? ?? 8B 44 24 78 2B 44 24 18 8B 54 24 7C 1B 54 24 1C 89 54 24 54 78 1A 7F 04 85 C0 72 14 80 7C 24 0F 00 75 0D 8D 47 58 39 44 24 10 0F 85 73 FC FF FF` |
| 0x005F0F10 | Phase A loop head `mov eax,[esp+10h]` | `8B 44 24 10 8D B0 90 01 00 00` |
| 0x005F129D | Phase B `cmp dword [edi+68h],0` (current build [svc+0x68]) | `83 7F 68 00` |
| 0x005F13C6 | Function tail ("more work?" flag, stats, epilogue) | `8D 47 58 39 40 04` |
| 0x005F1450 | SimService ctor: +0x2C = 5, +0x139 = 1 | source comment |
| 0x005A50F0 | `CASUtils_SimServiceEnableTimeSlicing` (script toggle of +0x139) | source comment |
| 0x00608630 | CAS TextureCompositor update, thiscall(svc, float, float), RET 8 (vtable 0x00FE6B08 +0x1C) | `56 8B F1 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 80 7E 15 00 74 13 8B 0D ?? ?? ?? ?? 83 C8 FF 50 E8 ?? ?? ?? ?? 5E C2 08 00 8B 46 18 8B 0D ?? ?? ?? ?? 50 E8`; calls 0x00608270 at +0x1E and +0x31 |
| 0x00608270 | Compositor work loop, thiscall(compositor, budget), RET 4 | `83 EC 28 53 8B D9 8B 43 08 3B 43 18 75 19 D9 EE 51 D9 1C 24 6A 00 6A 00 E8 ?? ?? ?? ?? 83 C4 0C 5B 83 C4 28 C2 04 00 55 56 57 6A 00 6A 04 8D 4C 24 20 E8 ?? ?? ?? ?? 8B 4C 24 3C 6A 01 51` |
| 0x0060829D | Unit byte of the loop's stopwatch (`push 4`) | patched `04` → `03` |
| 0x00607BD0 | Compositor ctor (+0x18 = 5) | source comment |
| 0x007377F0 | ResourceSystem service update `push [svc+14h]; call 0x00737560` on svc+0x18 (vtable 0x00FFEB10 +0x1C) | `8B 41 14 50 83 C1 18 E8 ?? ?? ?? ?? C2 08 00` |
| 0x00737560 | `ResourceSystem::Update(budgetMs)`: at least 250 ms while +0x1F0 is set (0x00737572) | `83 EC 20 53 55 56 8B F1 80 BE F0 01 00 00 00 57 74 0F B8 FA 00 00 00 39 44 24 34 7D 04 89 44 24 34` |
| 0x0072A730 | Deferred resource callbacks (maxCount, budget), also called from 0x00737060 | source comment, engine_map |
| 0x00C7E3CA | WorldManager service: `mov esi,[0x011ECBC4]; test; jz; cmp byte [esi+41h],0` | `8B 35 C4 CB 1E 01 85 F6 57 0F 84 ?? ?? ?? ?? 80 7E 41 00` |
| 0x011ECBC4 (+0x41) | WorldManager, "world active" | from the site above |
| 0x0059ED20 | ServiceManager main loop; indirect call at 0x0059ED57 | engine_map (profiler replaces this body) |

Resource loading on the render thread, as studied (source comment): the async loader creates a read job 0x0072A4F0 that
runs on the "JobThread" workers (affinity mask 2), and a finalize job 0x007297C0 (mask 1 = main thread, the second job
created at 0x00729C5B). Resource construction (factory parse, decompression through the stream, GPU object creation)
therefore happens on the render thread, one resource per job, inside the JobManager service.

##### Shader details
None.

##### Interactions

- **Frame Profiler.**
  - It replaces the ServiceManager loop body 0x0059ED20 with an identical C++ walk that calls vtable +0x1C, so its calls
    land in this patch's detours.
  - Its per-service names still work: it keys by the vtable entry, which is unchanged.
  - Profiler "Services (self)" includes the hooks' own cost.
  - No byte overlap: the profiler hooks neither these entries nor the patched bytes.
- **PLANO-SEPARACAO.md §3** lists "frame_budget_patch (another agent is writing it) | Unknown". Now resolved: it touches
  none of Smooth Patch's bytes (0xEC9FBA, 0xD81FC3..0xD81FE3) and not the 0xEC9F00 prologue. Per the plan, it goes
  through the same `ConflictGuard` and cooperates. No S3SS patch touches these sites (PLANO §3 list).
- **Smooth Streaming:** independent. The lot pass is inside the WorldManager service (0x00C7E3C0), which is not limited
  here. See [Smooth Streaming](#smooth-streaming).
- **Script GC Scheduler:** independent (simulation thread).
- **The game's scripts** toggle SimService+0x139 (`CASUtils_SimServiceEnableTimeSlicing`, 0x005A50F0). With
  `forceSimSlicing` on, the patch applies a budget even when the scripts turned slicing off (for example in Create a
  Sim, which may then update more slowly; see the UI hint).
- **World loading.** WorldManager+0x41 is set at the end of a world load (`FUN_00C6CF80`, 0xC6D430) and cleared on
  shutdown (`FUN_00C6B780`). Game budgets apply while it is off, and for `graceSec` after it turns on.

##### Known limitations
- One work item cannot be split: one resource finalize job, one Sim build step (after `maxSimDefers` frames), one
  compose step. The minimum slice guarantees progress, not a cap.
- The ResourceSystem timer counts whole milliseconds (so at least 1 ms). While the resource system's flag +0x1F0 is set,
  `ResourceSystem::Update` raises any budget to at least 250 ms itself.
- The per-pass accounting assumes one ServiceManager pass per frame. Nested passes from the lot impostor pump
  (0x00AD97E0) start new passes, so their time is not charged to the enclosing frame's budget.
- ObjectDesigner (0x00B3A960) and the other render-thread services (Swarm VFX, Scene service, ...) are not limited.

##### Pitfalls and failed approaches
- **Motivating data.** Frame Profiler report 2026-09-28 13:19 (`S3SS_Hitches.txt`, Documents\Electronic Arts\The Sims
  3\S3SS):
  - "Services (self)" is the largest hitch category (13.64 ms per hitch, worst 134.51 ms).
  - The per-hitch service lines show CAS SimService 9-41 ms, CAS TextureCompositor 4-5 ms and JobManager main-thread
    jobs up to 7 ms. Example: hitch #50193, CAS SimService 40.78 ms in one frame.
  - The main-thread jobs are job 0x007297C0, the resource finalize job.

  Separately, the notes record that limiting lot and light work (Smooth Streaming) did not reduce the spikes (NOTAS
  "Medicoes", 28/09). The services, not the lot pass, are the next suspect.
- **Unit changes need the value and the unit changed together.** 0x00599A20 is patched in one write (load + `push 3`).
  The compositor unit is patched before the first field write, which gives a harmless 5 µs for at most one frame. On
  uninstall, the fields are restored before the unit.
- **Never restore a field someone else changed.** `RestoreFields` only restores when the field still holds the patch's
  last value. `GameValue` re-captures the game's value whenever the field changed behind the patch's back.
- **Do not rely on flags across the gate.** `SimGateStub` re-tests the sign of EDX instead of trusting the SBB flags
  (source comment).
- Not tested in game: every item above is from static analysis. Expect surprises in Create a Sim and while Sims arrive
  (outfit builds).

##### Testing in game
- **Status line:** "Status: limiting (shared budget active)" / "world loading, game budgets" / "world loaded, game
  budgets for N s more" / "no world, game budgets".
- **Per frame:** "Last frame: X ms | average X ms | longest X ms | frames over 2x budget: N / N".
- **Table:** Service | Budget ("unlimited" for 0xFFFFFFFF) | Last | Average | Longest, for "Main-thread jobs (resource
  loading)", "Resource callbacks", "Object texture compositor", "Sim builds (CAS)".
- "Sim build steps deferred: N | run after the deferral limit: N | game's Sim time slicing: on/off/not seen yet".
- **Log:** `[FrameBudget] Installing...`, the `Installed (...)` line, "World active: game budgets during the grace
  period", and the `Uninstalled. Passes ...` totals.
- **Error texts:**
  - "<site> differs at 0x... (different game version, or another patch already changed it)";
  - "Service vtable 0x... +0x1C is not 0x...";
  - "The texture compositor update no longer calls FUN_00608270";
  - "The ResourceSystem update no longer calls FUN_00737560";
  - "CAS SimService loop targets differ";
  - "Could not install the service hooks";
  - "Could not patch the compositor timer unit / JobManager timeslice load / Sim build budget load / Sim build loop
    test".
- **Frame Profiler:** compare "Services (self)" and the per-service lines of the hitch file, on and off, with Sims
  arriving on a lot and during Create a Sim.

##### Open items
- The first in-game run: check Create a Sim responsiveness, Sim arrival pop-in and the loading time after a world load
  (grace period).
- Decide whether ObjectDesigner (no budget) needs handling.
- Consider sharing the "world loading" definition with Smooth Streaming, which uses a lot-pass gap heuristic.
