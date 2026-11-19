# 13. Desk checks: bridge command batching, free shader constant ranges

Two desk checks behind `12` §2.2 (frame-constant block) and §2.4 / §7.5 rank 9 (bridge `commandBatchingEnabled`).
No game run. Sources: the bridge at Apex's pinned commit, `Shaders_Win32.precomp` (Steam 1.67.2, read-only), a
static scan of `TS3W.exe` 1.67 (`game-copy/bin/TS3W.exe`, capstone), and Apex's own source.

Labels: **V-src** (read in source at the named commit), **V-precomp** (parsed), **V-static** (disassembly),
**Inferred**.

## Verdicts

| Question | Answer |
|---|---|
| Does `commandBatchingEnabled = True` do anything? | **No.** The option is parsed and never read. Upstream's `bridge.conf` says so ("not fully implemented yet, so this setting has no effect"). Drop P0 of `12` §6; no run needed (**V-src**) |
| Is per-call state already batched? | **Yes.** `client.batchState` (default True) packs consecutive state setters, `Set*ShaderConstantF` included, into one packet that is flushed before the next non-setter command. `client.dropRedundantState` (default True) drops setters whose value the server already holds (**V-src**) |
| Does Apex Setup ship a `bridge.conf`? | **Yes, two copies** (`Bin\bridge.conf`, `Bin\.trex\bridge.conf`), from `native/remix-bridge/conf/bridge.conf`. Setup only rewrites `client.targetProcess`. The batching keys are not in that file, so the upstream defaults apply (**V-src**) |
| Highest PS constant the game reads | **`c24`** in `ps_3_0` (Phong, Night). `c25`-`c32` show up only as shader-local `def`s. `ps_2_0` reads at most `c20` (**V-precomp**) |
| Highest VS constant the game reads | **`c227`** (SimHorse). The VS file is dense up to `c227`, and 72 techniques address constants relatively (**V-precomp**) |
| Game writers at runtime | Two device call sites: `Shader_SetFloatConstants 0x79DFE0` (VS and PS; register from the parameter record) and `0x696980` (InstancingEmulation, VS only, `c0` and `c40`, counts computed). No other PS writer (**V-static**) |
| PS range safe for a once-per-frame block | **`c208`-`c223`** (16 registers), and `c128`-`c199` if more is needed. **Not `c200`-`c223`:** Apex's own terrain patch already uses PS `c200`-`c204` |
| VS range | **`c242`-`c251`** (10). The game's parameter tables write vertex registers up to `c241` (pet and ghost fur, §2.5), so the earlier `c228`-`c251` was wrong; the histogram run saw the game write `c228`-`c229` (**V-static**, **V-precomp**, run). Apex holds `c252` (jitter) and `c255` (foliage wrap) |

## 1. `commandBatchingEnabled` (pinned bridge)

**Pin.** `apex-repo/native/remix-bridge/build.cmd`: `COMMIT=f35b86a494712eba72177ddabe62a48ff4cb6e9c` (dxvk-remix
`main`, 2026-11-06), sparse checkout of `bridge public src/util`, then patches 0001-0016. A clone with that commit
is local: `game-copy/rewind-bridge-1115/apex-async/native/remix-bridge/src`. Its working tree carries patches and
async-readback WIP, so every quote below comes from `git show f35b86a4:<path>`, not from the working tree.

**What it does (V-src).**
- `bridge/src/util/config/global_options.h:416`: `commandBatchingEnabled = getOption<bool>("commandBatchingEnabled", false)`,
  with the comment "turned on by default" (stale). The getter is `getCommandBatchingEnabled()` (line 187).
- `git grep -i batching f35b86a4` finds **no caller** of `getCommandBatchingEnabled` in client, server or util.
- `bridge/bridge.conf:262-273`: "Note: Command batching is not fully implemented yet, so this setting has no effect
  until this note is removed."
- **Why it is off:** it is a placeholder for a design that was never built (one wait on the command-queue semaphore
  per frame instead of one per command). Off or on, the binary behaves the same.

**What does batch today (V-src).**
- `client/d3d9_device_base.cpp:55`: `setStateBatching(client.batchState && !sendAllServerResponses)`.
- `util/util_bridgecommand.cpp` `StateMessage`: every setter in `IsBatchableState` (`util_commands.h:897`: transforms,
  render, sampler and texture-stage states, `SetTexture`, shaders, **`Set{Vertex,Pixel}ShaderConstantF/I/B`**, streams,
  indices, viewport, scissor) is appended as a record to an open state packet under the writer channel's mutex. The
  packet closes when the handle changes, at `kMaxPacketRecords` / `kMaxPacketWords`, or when the next non-setter
  command is built, so the server applies all setters in order before that command. Ordering and sync calls are kept
  by construction.
- `client.dropRedundantState` (`client_options.h:91`, default True): a setter equal to what the server holds is not
  sent. `invalidateSentState()` runs at `d3d9_device.cpp:437` and `:3592` (Reset paths) and in
  `d3d9_stateblock.cpp:216`, so a device reset does not leave a stale "already sent" register. That answers the
  `12` §2.2 caveat about dropped writes after a reset (**V-src**; not exercised live).
- Apex's `bridge.conf` sets neither key, so both run at their defaults (True).

**Risks if anyone builds real command batching (Inferred).** Readbacks (`GetRenderTargetData`, locks, `Present`
with the present semaphore) need replies, so they must flush the batch. Apex's own device calls go through the same
client (`D3D9Hooks::CallOriginal*`) and are ordered with the game's. The rewind patch (0015) snapshots the server
state at command boundaries. None of this applies today, because the switch does nothing.

**Smoke-job spec (only if Rodney still wants proof; Rodney: dev speed first).** It cannot show an effect, so the run
is better spent on §3. If it runs anyway:
- Bin state `apex` (tools/turbo-compat.ts), newest Apex, bridge installed by Apex.
- Arm B: add `commandBatchingEnabled = True` and `perfLogIntervalFrames = 600` to **both** `bridge.conf` copies.
  Arm A: only `perfLogIntervalFrames = 600`. Restore both files afterwards.
- `node tools/harness/apex-run.ts --apex-repo <apex> --job tools/ingame_runner/jobs/camera-bookmark.json --apex-asi <asi> --release --loads 1 --tag bridge-batch-smoke`
  (short load and pan, about 5 min per arm).
- Pass: no crash, `summary.txt` has no `DRAW BAD|HEAP|crash` lines, and the screenshot matches arm A. Bridge stats:
  the `BridgePerf` lines in `rtx-remix\logs\bridge32.log` / `bridge64.log` should be identical within noise. Expect
  no difference.

## 2. Free constant ranges

### 2.1 What the shaders read (V-precomp)

Method: `tools/precomp.cjs`'s blob and technique walk, with one extra pass per shader. Every constant operand
(register type 2, `def` destinations excluded) is collected per shader model; relatively addressed operands
(source bit 13) are kept apart. **CTAB is stripped in every shader** (0 entries, as `docs/53` §6 says), so declared
ranges come only from operands.

| Set | Read at runtime (not `def`) | Never read |
|---|---|---|
| `ps_3_0` (224 regs) | `c0`-`c24` (25 registers; `c24` only Phong, Night) | **`c25`-`c223`** |
| `ps_2_0` (32 regs) | `c0`-`c20` | `c21`-`c31` |
| vs (256 regs) | `c0`-`c227` except `c39` and `c154`-`c179` | `c39`, `c154`-`c179`, **`c228`-`c255`** |

- `ps_3_0` has no relative constant addressing at all, so the PS numbers are complete.
- One `ps_2_0` blob in TerrainLightProbe decodes to "c1791". That is a parser artifact (above any limit); it was
  ignored.
- The PS techniques that reach `c25`-`c32` (Phong 29, TerrainHigh 31, ExteriorFloors 29, Counters 28, Night and
  SimSkin 32) reach them only through `def`s. A `def` overrides the device register for that shader alone.
- VS relative addressing: 72 techniques. Bases are `c2` (skinning palettes, about 50 techniques) and `c3`-`c212`
  (trees `c28`-`c131`, billboards `c32`/`c36`, instancing `c40`, Sim shadow and fur `c196`-`c212`). The palette
  length is set at runtime (bone or instance count), so the operand scan cannot bound `c[a0+base]`. The
  `c228`-`c255` gap is safe only if no palette runs past `c227`. **Inferred**; one histogram run settles it (§3).
- `tools/precomp.cjs`'s `02` §1.1 figure "c24" agrees (it excludes `def`s). `scene_dither.cpp`'s comment "no game vertex
  shader uses a constant above c241" disagrees with `c227` here. The two methods differ (theirs may count relatively
  addressed reach); not resolved.

Per technique: the highest constant operand, including `def`s, as PS/VS. `-` = no shader; `r` = the VS uses
relative addressing, so its reach may be higher:

AOandSI 0/6, Additive 5/193r, AlphaTestShadows -/14, BasicShadows -/7, BasicShadowsReversedCull -/7, BasinWater 15/199r, Blit -/-, BlitCube 2/6, BlitOp 1/2r, Bloom -/-, Blueprint 7/4, BrushedMetal 18/206r, BurnedWindowGlass -/0, BurntTile 4/5, CASFinal 12/203r, CASLightMap 16/204r, CASMapBlur -/-, CASSkinSinglePass 17/203r, Ceiling 2/5, ChannelSelectOp 2/2r, ClampedBlitOp 1/2r, ClampedChannelSelectOp 2/2r, ClearBuffer -/-, ClearOp 1/2r, CloserSeaWater 14/29, CloserSeaWaterNoStencil 14/29, CloserSeaWaterReflective 16/27, CloserSeaWaterReflectiveNoStencil 16/27, CloserSeaWaterReflectivePlaceholder 14/29, CloserSeaWaterReflectivePlaceholderNoStencil 14/29, Clouds 5/20, Counters 28/213r, CreateThumbnail 1/2, CreateThumbnailR1 1/3, CreateThumbnailR2 1/3, CreateThumbnailR2Grey 1/3, CreateThumbnailR2Sepia 4/3, CubeMapToParaboloid 2/-, CurseLevel1 4/-, CurseLevel2 7/-, CurseLevel3 8/-, CurvedPoolPreview 0/4, Day 17/204r, Debug -/3, DecalTechnique 4/13, DistantTerrain 16/30, Downsample 3/10, DrawDips -/-, DrawRipples -/-, DropShadow -/185r, Dummy -/0, Error -/9, ErrorAsset -/9, ExteriorFloors 29/31, ExteriorWall 13/30, ExteriorWallAOSI 0/30, ExteriorWallShadow -/7, FailBlurry 2/3, FailCopyImage 0/0, FailNoisy 0/0, FillScreen 0/-, FilterCubeMap 0/-, FloorGrid -/5, FloorShadows -/11, FloorThickness 6/17, FloorTileCeiling 8/32, FloorWithBlacklight 15/18, FloorWithStrobelight 15/18, FloorWithStrobelightAndBlacklight 15/18, FluidEffect 23/11, Foliage 20/212r, FullBright 2/197r, GammaBlit 1/3, Gemstone 18/19, GhostFur -/-, GhostFurLowQuality -/-, GhostFurOpaque -/-, GhostHorse 14/219r, GlassForObjects 23/196r, GlassForPortals 13/195r, Greyscale 0/0, HSVToRGBOp 1/2r, HeatShimmer 0/3, Hideable 17/18, HouseboatUndergroundBackdrop 1/3, ImposterShadows 0/7, ImpostorWater 1/20, InitialCausticsPaints 1/17, InitialPaints 0/10, InitialPaintsCausticsWithCutout 1/17, InitialPaintsFrost 1/10, InitialPaintsFrostWithCutout 1/10, InitialPaintsSnowMelting 2/12, InitialPaintsSnowMeltingWithCutout 2/12, InitialPaintsThickSnow 0/14, InitialPaintsThickSnowWithCutout 0/14, InitialPaintsWithCutout 0/10, InstancedObject 19/113r, InstancedShadowCaster 0/92r, InteriorAOSIPlaceholder 0/6, InteriorFadePlaceholder 2/22, InteriorFloor 15/18, InteriorFloorUnlit 2/10, InteriorWall 14/22, InteriorWallWithBlacklight 13/22, InteriorWallWithStrobeAndBlacklight 13/22, InteriorWallWithStrobelight 14/22, InvisibleMeteor -/-, Jade 15/10, Landmark 23/202r, LeavesTile 6/5, LightProbeDebug 3/11, LotImposter 24/30, LotPondFogOfExploration 0/4, LotPondPick 2/5, LotSea 4/8, Masked 2/80r, MeteorShadowTechnique -/15, Night 32/204r, Normal 2/80r, ObjectPlacementOutline -/6, Occluder -/-, OutdoorProp 20/21, OverlayOnly 0/8, Painting 15/204r, ParticleAnim 18/34, ParticleAnimFullbright 2/32, ParticleJet 13/35, PassThrough 0/0, Phong 29/220r, PhongAlpha 20/213r, PhongVisualizer 24/202r, PickCASSim -/188r, PickDefault 2/185r, PickParticleClipAlpha 2/5, PickParticleTexture 2/5, PickPondWater 2/9, PickSeaWater 2/9, PickSim 2/186r, PickWallsWithCutout 2/8, PickWallsWithCutoutBacksideSolid 2/15, Plumbob 18/15, PointSampleBlitOp -/2r, PondImpostorMaker 0/4, PondMurk 0/5, PondWater 22/33, PoolImpostorColor 0/13, PoolImpostorColorAOSI 0/13, PoolPick 2/10, PoolWater 7/13, PoolWaterOutside 8/13, PoolWaterReflective 7/13, PoolWaterReflectiveOutside 7/13, PoolWindowTranslucent 5/4, Precipitation 15/37, PreviewWithAlpha 0/10, RGBToYCbCr 0/5, ReadDepthBuffer 0/0, RemappedChannelSelectOp 2/13, RenderParticleClipAlpha 1/13, RenderParticleClipAlpha_NotInBoatInterior 1/13, RenderParticleNormal 6/13, RenderParticleNormal_NotInBoatInterior 6/13, RenderParticlePoint 3/14, RenderParticlePointNormal 6/14, RenderParticlePointNormal_NotInBoatInterior 6/14, RenderParticlePoint_NotInBoatInterior 3/14, RenderParticleTexture 3/13, RenderParticleTexture_NotInBoatInterior 3/13, RenderSHToCube -/25, RenderSHToCubeSpecularAlpha -/22, RenderVectors 0/4, Road 24/19, RoadGrateDrain 20/215r, RoadsCompositor 0/0, Roof 17/23, RoofAO 0/23, RoofUnlit 1/8, Rug 14/17, RunEffect 5/-, Sculpture 23/195r, SculptureIce 16/191r, SeaBox 0/3, SeaWaterEdit -/5, Sepia 1/0, ShadowBlur -/-, ShadowCaster 0/93, ShadowDebug -/-, ShadowMerged -/183r, SimCensorEffect 0/0, SimDropShadow 0/12, SimEyelashes 1/200r, SimEyes 17/199r, SimHair 16/207r, SimHairVisualizer 20/195r, SimHorse 19/227r, SimPet -/-, SimPetFur -/-, SimPetFurExitData 0/213r, SimPetFurFinData 1/217r, SimPetFurLowQuality -/-, SimPetFurOpaque -/-, SimPetFurPeelTexture -/-, SimRobot 15/214r, SimShadow 0/209r, SimShadowDepth 1/188r, SimSimpleBlobShadow 0/12, SimSkin 32/215r, SimSkinVisualizer 21/203r, Simple 0/6, SimpleSim 1/189r, SingleObject 19/113, SketchEffect 2/-, SkinToneOp 2/13r, SkinnedShadow -/187r, Skirt 4/7, Sky 6/7, Sky_Reflection 7/7, SnowFences 8/17, SnowFloorEdges 8/26, Soulstone 16/18, StandingWater 20/199r, StaticTerrainCompositor 1/2, StaticTerrainCompositorPacked 1/2, StaticTerrainLightmap 4/3, StencilToColor 0/0, Subtractive 1/5, TerrainFog 6/17, TerrainFogNoSnowElev 6/17, TerrainHigh 31/24, TerrainHighInsideGreenhouse 3/20, TerrainHighNoSnowElev 6/20, TerrainIndoor 3/20, TerrainLight 18/26, TerrainLightAndFog 18/26, TerrainLightNoSnowElev 16/25, TerrainLightProbe 12/1, TerrainLight_Indoor 4/25, TerrainLight_IndoorWithStrobeLight 4/25, TerrainLight_InsideGreenhouse 8/25, TerrainLow 24/26, TerrainVisualization 11/14, Text -/-, ThumbnailShadowPlane 3/9, Trampoline 16/202r, TrampolinePick 2/191r, TrampolineShadow -/7, TreeShadowCompositor 1/4, UnderSea 2/8, UndergroundBackdrop 0/3, UnlitExteriorWall 0/30, VertexColor -/-, Video 4/1, Vignette 0/0, WallTop 5/15, Window 6/14, billboard 17/65r, billboardDepth 2/48r, billboardPick 2/49r, billboard_alpha 16/52r, branchMain 8/132r, drawGreen 2/2, edit_mode_sea 0/29, fadeDownPlaceholder 1/6, frond 7/132r, halo 0/7, leafCard 12/153r, leafMesh 12/148r, main 14/194r, mirror 1/8, mixKeyCompositor 0/0, moon 7/11, moon2 7/11, moon3 7/11, moon4 7/11, moon5 7/11, moon6 7/11, moon7 7/11, moon8 7/11, moonhalo 0/11, occluderviz 0/7r, pickFloor 2/6, pickTerrain 2/11, pickbranch 2/56r, pickleafcard 2/70r, pickleafmesh 2/64r, puddle 7/8, puddleIndoor 3/8, shadowbranch -/55r, shadowleafcard 2/79r, shadowleafmesh 2/74r, sun 1/7, tech0 1/3, terrainAOSI 0/14, unlit 0/6, unlitLotTerrain 0/26

Reproduction: the extra pass is about 60 lines on top of `precomp.cjs`'s `stat()` walk (instruction
length = token bits 24-27; register type = bits 28-30 | (bits 11-12 << 3); `def` = opcode 0x51; relative = bit 13).
It was not added to `tools/precomp.cjs` (the brief allowed one output file).

### 2.2 What the game writes (V-static, `TS3W.exe` 1.67)

The scan looked for `mov r, [vtbl + 0x178 | 0x1B4]` followed by `call r`, where the vtable came from `mov r, [x]`
shortly before. `call [r + disp]` forms: 0 in the image.

| Site | Writes | Start register and count |
|---|---|---|
| `0x79E0F1` / `0x79E150` (VS), `0x79E108` / `0x79E165` (PS) in `Shader_SetFloatConstants 0x79DFE0` | per contiguous run of changed parameters | start = `(record[+0x18] >> 8) & 0x3FF`; count = the sum of `ShaderParam_Pack 0x79D620` returns; staging buffer sized from the shader's register count (`+0x158` VS / `+0x164` PS). Registers therefore come from the shader's parameter table in the precomp |
| `0x696A7B`, `0x696A97` in InstancingEmulation `0x696980` | VS only | `SetVertexShaderConstantF(c0, data, n0)` and `(c40, data, n1)` with `n0 = [inst+0x14] * k`, `n1 = [inst+0x18] * k`, `k = min(remaining, [inst+0x20])`: computed per batch and bounded by the InstancedObject / SingleObject layout (operands reach `c113`) |

- `0x527E60` (listed in `docs/53` §5) is a `thiscall` on another interface (`ecx = ebx`, two pushes), **not** the
  device's `SetVertexShaderConstantF`. `0x77C030` (UI) contains no `+0x178` / `+0x1B4` call in its body
  (`0x77C030`-`0x77C94D`). Neither writes shader constants. Correct `docs/53` §5 when it is next edited.
- **Inferred:** a parameter record could name a register the shader optimised away, giving a write above `c24` that no
  shader reads. Harmless to the game, but it would overwrite an Apex block. Not excluded statically, because the
  precomp's parameter tables were not parsed. The histogram in §3 covers it.

### 2.3 Apex's own use of high registers (V-src, `apex-repo` `cb2e62d`)

- PS: lot pass `c28`-`c35` (`lot_light_bridge.cpp`), lightmap smoothing `c0`-`c43`, picture `c0`-`c60` (`c56`),
  `register(c..)` declarations up to `c127` in features, and **terrain room-mask patch `c200`-`c204`**
  (`shader_patches.cpp:4550-4560`: `c200` / `c201` mask uv, `c202` gate, `c203` / `c204` defs).
- VS: `c252` (scene dither jitter), `c255` (foliage wrap; it is restored after the draw because "another shader may
  read c255").
- Full-screen passes (`looks_fx`, `picture`, `light_probe` dumps) save and restore all 224 / 256 registers. A frame
  block in their range survives them.

### 2.4 Conclusion

- **PS frame block: `c208`-`c223`** (16 float4). The game never reads `ps_3_0` registers above `c24`, and no game path
  writes outside the shaders' own tables. It also clears Apex's `c200`-`c204`. If more room is needed, `c128`-`c199`
  is free in both the game and the `register()` declarations of Apex's features; first grep Apex's raw-token patches
  (`0xA0000000 | cN`) for that range.
- `ps_2_0` shaders cannot see a block above `c31`. The frame block serves `ps_3_0` copies only; `ps_2_0` families keep
  per-draw constants in `c21`-`c31` or go through the rig route (`12` §2.3).
- **VS: `c242`-`c251`** (10 float4; §2.5). `c228`-`c241` belongs to the pet and ghost fur shaders. `c154`-`c179` is a
  tempting gap, but it lies inside the reach of the tree and Sim palettes. Do not use it.
- Write the block once per frame with one ranged call. With `client.dropRedundantState`, the bridge skips an
  unchanged block for free, and Reset invalidates its sent-state cache (§1).

### 2.5 The game's parameter tables (correction, after the S0-G2 run)

The first `const-range-smoke` run failed on `VS c0-c229 by TS3W.exe+0x39E152 (game), 2 of its 177920 calls` (16
calls in the session). `0x79E152` is the return address of the final flush in `Shader_SetFloatConstants`
(`call [vtbl+0x178]` at `0x79E150`, after the record loop), so the write came from a vertex parameter table.

- **V-static.** The loader (`0x79F05A`, record constructor `0x79D030`) reads a `PARM` table per pass, vertex first, then
  pixel: count, then per record (name hash, `w1`, `w2`). Type is `(w1 >> 11) & 7`; the record field at `+0x18` holds
  the start register (bits 8-17) and the count (bits 0-7). The loader sets the shader's register count (`+0x158` VS,
  `+0x164` PS) to the maximum of start + count over its non-type-2 records, and the setter sizes its staging buffer from
  it. `ShaderParam_Pack 0x79D620` returns the count or less on every path, and the setter merges a record into the
  open run only when its start equals the run's end. Every write of `0x79DFE0` therefore lies inside some record's
  `[start, start + count)`.
- **V-precomp.** `node research/tools/precomp.cjs <precomp> parm` decodes every table (196,645 passes, none missed):
  highest register written VS **`c241`**, PS `c24`. Past `c227`: SimPetFur and SimPetFurLowQuality `c241`, GhostFur
  and GhostFurLowQuality `c241`, SimPetFurOpaque `c233` (records such as `FurTextureDimensions`, `kMaxFurdist`,
  `curlDeltas`). §2.1 missed them because the pet fur passes carry a longer pass header, so `precomp.cjs` read no shader
  for them (the `-/-` entries above). `scene_dither.cpp`'s "c241" was right.
- **Inferred.** The run's 16 calls are a pet drawing with fur; `c229` is the highest register its dirty run reached.
  The instancing writer `0x696980` (`c0`, `c40`) was not seen in the run.
- Not covered: shaders a mod loads from outside `Shaders_Win32.precomp`. The histogram still reports any such write.

Result: the vertex reserved range moves to `c242`-`c251`. The motion-vector rows of `14` (`c248`-`c251`) stay inside it.

## 3. Smoke job that settles the open items (recommended instead of the batching run)

One run, Bin state `apex`, `apex-run.ts --release --loads 1`, about 8 min: a short load plus `camera-bookmark.json`
(or `night-terrain-pan.json`) and one CAS entry so that Sim, horse and pet palettes draw.
- Apex (or a probe) wraps `SetPixelShaderConstantF` / `SetVertexShaderConstantF` and logs a histogram of
  `[start, start + count)` per caller return address. Log only; no change to the values.
- `perfLogIntervalFrames = 600` in both `bridge.conf` copies (restore afterwards) for `BridgePerf` packet and record
  counts.
- Pass: no crash, no `DRAW BAD|HEAP` lines, PS writes from game return addresses (`0x79E10A` / `0x79E167`) stay at
  `c0`-`c24`, and VS writes stay at or below `c227`. Any write into `c208`-`c223` (PS) or `c228`-`c251` (VS) from a
  non-Apex caller fails that range.

## 4. Not checked

- The precomp's parameter tables (the source of `Shader_SetFloatConstants`' registers) were not parsed.
- No relatively addressed palette length was bounded.
- The EA 1.69 exe was not scanned.
- Other mods' D3D hooks (S3SS, ReShade, D3DShaderReplacer replacements) may write their own registers.
- `scene_dither.cpp`'s "c241" claim was not reconciled.
