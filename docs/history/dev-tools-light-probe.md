# Light Probe: history

Chronological record of investigations and design decisions. Newest entries at the bottom. The current behaviour is
described in [features/dev-tools/light-probe.md](../features/dev-tools/light-probe.md).

Raw sources: `S3SS-dev\NOTAS-ILUMINACAO.md`, `PASSO3-PLANO.md`, `PLANO-SEPARACAO.md`. Captures were cited in the notes by
their archive folder names (`LightProbe-<name>`, `LightProbe-m01` to `-m80`).

### 2026-09-24: the probe as the main lighting instrument

**Context:** static reverse engineering of the lot-edge and lamp lighting paths failed repeatedly (maintainer memory note
`sims3-lot-edge-lighting.md`).

**Finding:** a per-pixel GPU probe answers which draws paint a pixel, in which order and blend state, which shader pair
and technique family, and where the light comes from: a baked light map on some sampler (256x256 DXT5 terrain chunk
map, A8R8G8B8 room/wall/floor atlases, 32x32 per-object maps), rig lamps in PS c1..c3 / c5..c7, vertex lights in VS
c4..c11, or no lamp term at all (roofs, water). NOTAS section 1c (`LightProbe-grama3-escura`, 23:54): the "dark" grass
was terrain paint, not light. Always compare with a correct pixel (`-clara`/`-escura`, `-lote`/`-mundo` pairs).

**Outcome:** kept. Almost every lighting fix in the notes starts from a capture.

### 2026-09-25: the capture archive ("watcher")

**Context:** each capture overwrote `S3SS_LightProbe.txt` in the S3SS folder.

**Finding:** captures that mattered were copied to sibling folders: `S3SS\LightProbe-<name>` (for example
`LightProbe-lote`, `LightProbe-andar2-b`), `LightProbe-seq-N-name` (the ordered sequence of notes section 4g), and from
00:23 automatically `LightProbe-m01`, `-m02` ... `-m80` (m80 dated 2026-09-27 17:39). The watcher script that made the
`mNN` copies was run by an earlier agent session and is not in the scratchpad (no `.ps1/.sh/.pl` refers to
`LightProbe-m`). Inferred from the folders: an `mNN` folder holds `S3SS_LightProbe.txt` plus the `.bin`/`.txt` of the
shaders it names (13 files in m01 and m80); some named folders (for example `LightProbe-m44`) also hold stale BMPs.
Offline indexers over the archives (scratchpad, read-only): `draws.ps1` (every `== DESENHO` line of every
`LightProbe-*` folder with MD5 of the VS/PS files, output `draws.csv`), `consts.ps1` (selected constants per shader
family), `passo3\index.ps1`, `census.ps1` (see [dev-tools-census.md](dev-tools-census.md)).

**Outcome:** superseded on 2026-09-30 by one capture folder per capture.

### 2026-09-25: DXT decoding and full constant arrays

**Context:** plant capture m65 (~14:15) showed no texture where a DXT1 texture multiplied the lamp light; skinned
objects showed no world matrix.

**Finding:** DXT textures were skipped by the dumper. Before 18:40 the probe stored only PS c0..c31 and VS c0..c95;
skinned objects keep the world matrix and vertex lights at VS c184..c199.

**Outcome:** `DecodeDxt` added; all PS c0..c223 and VS c0..c255 stored; the two-capture comparison added at 18:40
("Captura F7 aprofundada").

### 2026-09-25: MSAA revealed by the sample count

**Context:** capture m25 (`LightProbe-lago-preto`), black stains on a pond at night.

**Finding:** `amostras=8` showed the game's 8x MSAA was on, so the Depth Blur INTZ swap was inactive. The water's screen
reflection fell back to a 40 m guess sampling the wrong part of the scene copy.

**Outcome:** no screen reflection without depth.

### 2026-09-25: PASSO3 increment 0 and planned additions

**Context:** PASSO3 section 3.7 planned more probe detail.

**Finding:** only the sampler filters (all 16 samplers) were added. Replacement PS/VS pointers for mod draws, constants
c144..c149 and the lamp block id were not implemented.

**Outcome:** open; listed under Limitations in the feature page.

### 2026-09-27: nine significant digits

**Context:** "near = 1.0 on the road" read from 3-digit constants (w about 243) was rounding.

**Finding:** m80, with 9 digits and the depth-bias states (NOTAS "SSAO refeito com GTAO"), gave near = 0.250 identical
in c0/c40/c180/c192/c216 for every draw of the frame, varying 0.2 to 0.3 between frames; A = 1.00008.

**Outcome:** constants are written with 9 significant digits.

### 2026-09-28: HDR back buffers and the growing dump folder

**Context:** probe2 and probe4 captures (`probe2_clara.txt`, `probe4_portao.txt` ... `probe_borda_escura.txt`, copied
to the scratchpad) under the then-current HDR output.

**Finding:** with an A16B16G16R16F back buffer, `ReadScreenPixel` wrote `COR NA TELA NO PIXEL: (nao lida)` (cause
inferred). The shared `LightProbe\` folder was never cleared and held 1,177 files; shader files named by pointer and
`T<n>` BMPs mixed captures. NOTAS item 6 of the 2026-09-25 deep review listed "LightProbe without a size limit and
holding render targets" as a risk.

**Outcome:** HDR output was later removed. The folder problem was fixed on 2026-09-30.

### 2026-09-28: pitfalls recorded

**Context:** review of how captures had been misread.

**Finding:**

- Pointer names are not identities: the same shader has a different address every session and an address can be reused.
  Deduplicate by bytecode hash (MD5 in `draws.ps1`; FNV-1a in the census and in `ShadersRecusados`). PASSO3 F-J5 shows
  the same trap for lighting-snapshot light indices.
- Different draws paint the same pixel: m50, the snow on the stairs is draws #303/#315 on top of the stair #243 that was
  already fixed; the pixel's final look came from the last draw.
- The F8 writer is `light_diag.cpp`, not the untracked `patches/light_diag_patch.cpp`; the probe has no such duplicate.

**Outcome:** recorded as rules in the feature page.

### 2026-09-24 to 2026-09-28: captures behind the lighting fixes

**Context:** index of captures named in `NOTAS-ILUMINACAO.md`. The fixes themselves are documented in the Night
Lighting pages.

**Finding:**

| Capture | What it showed | Outcome |
|---|---|---|
| `LightProbe-lote` / `-mundo` | world grass adds `tex2D(s8 terrainLightMap)*c7.x`; lot grass is a modulate2x pass adding `tex2D(s1 lotLightMap)*c3.x` (568-byte PS); the two meet at the lot edge | bridge draws lot light with `max(lot, terrain)` ([lot-light-pass.md](../features/night-lighting/lot-light-pass.md)) |
| `-grama2` / `-grama2-claro` | chunk seam at x = 1280: the baked chunk map lacks the neighbour's lamp overflow; PS_294418E0 keeps the map on s7, not s8 | automatic terrain rebuild after load; search s1..s15 for the 256x256 map |
| `-cerca`, `-arbusto`, `-arvore`, `-arvore2` | instanced VS: 3 lamps per instance c27..c29 / c54..c56[a0.z], nearly all zero | object gather boost ([objects-and-rigs.md](../features/night-lighting/objects-and-rigs.md)) |
| `-chafariz`, `-lago`, `-oceano` | fountain uses per-pixel rig lamps; lake PS has no lamp term; ocean has a real-time planar reflection | lake additive lamp pass ([water.md](../features/night-lighting/water.md)) |
| `-conjunto`, `-arbusto2` | lamp light multiplied by the moon shadow `(t2*shadow + sky)*t1.z` | shadow lerp to 1 by night level (`ObjectRig`) |
| `-escada`, m03 | vertex lights VS c4..c11 all zero | first wrong hypothesis (bit 0x10), see m32..m41 |
| `-telhado`, `-telhado2` | roof PS has no lamp term; world position in TEXCOORD4.zw/5.w | `roof_ps.hlsl` with 16 lamps ([roofs.md](../features/night-lighting/roofs.md)) |
| `-neve` | snow lot light variant PS_2A13D200, lot light via `texld r0, v3, s2` | `PatchSnowBytecode` ([snow.md](../features/night-lighting/snow.md)) |
| m01/m02 | snow ground: a road/sidewalk mesh with its own chunk map copy | road bridge |
| m04/m05/m06 | snow formulas: lot x0.25 (def c9.x), world, road own map in s6 | max(lot, terrain x4); road patch |
| m12/m14, m17 | lamp colour (1, 0.75, 0.79) is the factory pink; script sets it again later | `LampColourSet` + 0x6B0BDE ([lamp-colour.md](../features/night-lighting/lamp-colour.md)) |
| m15 | fence in an enclosed area: rig mode 1, room-list only | `RoomGatherThunk` at 0x6BBE70 |
| m16/m18, m24, m42/m43 | 3rd and 4th road variants; summer road VS | generic `PatchRoad`, `IsRoadVs` ([roads.md](../features/night-lighting/roads.md)) |
| m21, m22, m28, m63/m64, m78 | winter/summer foliage: moon shadow, back-face lamp cut `max r0, r0, cK.w` | `PatchLeafShadow`, `PatchFoliageVs` ([foliage.md](../features/night-lighting/foliage.md)) |
| m23, m29 | roof snow PS_2793C3E8 (no lamps); m29 VS c19 = 0 put the roof at the world origin | additive `roof_snow_lamps_ps.hlsl`, position from COLOR0 x c15.x |
| m25 | black pond, `amostras=8` | MSAA on: no screen reflection without depth |
| m32..m41 | fences, stone, mailbox, window, plants: rig lamp colours zero or weak; m41 correct vegetation | root cause in the rig gather (VertexLight slots only get the excess); street-lamp-only boost extended to all light classes |
| m44/m45, m46/m47 (with F8) | walls lit from a per-story 256x128 map; cut at the floor line | [level-light-share.md](../features/night-lighting/level-light-share.md) |
| m48 | snow cover on fences (VS_2F27C9C0 / PS_2F27C510), no lamp | `PatchSnowCover` |
| m50/m51 | stair snow shares the roof-snow PS byte for byte | route by VS class 9, `PatchSnowRelief` |
| m52..m57 | doors/windows/sofa/counter: rig lamps arrive (~0.6) but from above; Counters vs Phong outdoors; animated door VS | `PatchObjectLampVs/Ps`, per-pixel lamps |
| m59/m60 | PASSO3 test wall (ExteriorWall PS_2734BB80, 1024x512 atlas) and ground | PASSO3 plan |
| m61/m62, m66, m69, m71/m72 | summer floor baked family; pool edge VS; snow floor and door sill variants | `PatchBakedAtlasPs`, `IsFloorVs`, `IsSnowFloorVs` ([floors.md](../features/night-lighting/floors.md)) |
| m65 | plant lamp light multiplied by a real DXT1 texture | DXT decoding added |
| m73/m74, m76 | dark lot beside a lit sidewalk: lot pass reads the "home chunk" map with CLAMP | lots read the world atlas ([world-atlas-and-smoothed-maps.md](../features/night-lighting/world-atlas-and-smoothed-maps.md)) |
| m75 | ExteriorWall lamp light is atlas x cK.x only | "Lamp light on outside walls", `wall_lamp_table.h` ([walls.md](../features/night-lighting/walls.md)) |
| m70..m80 | projection: row2 = A*row3 + (0,0,0,-near), A = 1.00008 (m80); c40..c43 = world view-projection | PostScene camera vote, AO, Lot Map Probe |
| `probe2_*`, `probe4_portao` | lot edge on houses with foundations (story gate); dark gate with the rig zeroed | story gate 0xC294D9 ([terrain-relight.md](../features/night-lighting/terrain-relight.md)); keep rig, max() |

**Outcome:** kept as reference.

### 2026-09-28: standalone separation plan

**Context:** `PLANO-SEPARACAO.md` section 4 listed the probe's renames for the standalone.

**Finding:** planned output `Documents\Electronic Arts\<localized>\Apex Radiance\ApexRadiance_LightProbe.txt` and a
`LightProbe\` folder inside `Apex Radiance\`; the menu key Ctrl+Shift+F11 next to the dev chords F7 to F10. Open items:
move the probe off the Night Lighting Present hook if Night Lighting is not the host; clear or version `LightProbe\`
per capture and archive automatically; accept FP16 back buffers in `ReadScreenPixel`.

**Outcome:** the per-capture folder replaced the rename (2026-09-30). The probe is still hosted by Night Lighting.
FP16 support became moot when HDR output was removed.

### 2026-09-30: player capture, one folder per capture, follow-ups after a floor change

**Context:** the Report a problem page gathered the recorder, probe and snapshot for players.

**Finding:** the probe moved to both builds. Each capture gets its own `Captures\<date time> Light capture\` folder; the
old 20-capture pruning and the shared "latest" file were removed. Automatic captures 1 s and 3 s after a shown-story
change (up to 6 within 2 minutes) were added for furniture floor-switch problems. The alpha channel of textures is
written as a separate `_alpha.bmp`.

**Outcome:** kept; released in 2.5.0.

### 2026-10-02: one-click pointer selection

**Context:** a guided Report page let players point at an object instead of using a shortcut.

**Finding:** `LightProbe::Aim` showed a violet target at the mouse; one left click confirmed the client pixel
(`ConfirmAim`), the mouse down and up were consumed so the game did not also select or place an object, and the Report
page reopened after the probe and screenshot finished. Esc, focus loss or opening the menu cancelled. Guided captures
schedule no floor-change follow-ups. The target is a screen pixel, not an object identity.

**Outcome:** the API and overlay handling remain, but the restored 2.5.3 Report layout (2.5.5) has no control that calls
`Aim`; captures start from the shortcut.
