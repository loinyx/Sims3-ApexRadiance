# Shader census and false colour: history

Chronological record of investigations and design decisions. Newest entries at the bottom. The current behaviour is
described in [features/dev-tools/census.md](../features/dev-tools/census.md).

### 2026-09-25: token-level census of captured shaders

**Context:** before the in-game census, at 10:44, `census.ps1` (scratchpad) studied the shader patterns behind
`PatchObjectLampPs`.

**Finding:** a token-level PowerShell census of captured shader `.bin` files (temps, constants used, `def`s, relative
addressing, flow control, inputs, samplers; for PS the first `dp3 ?, rN, c9`, the `texld s1` cube read, the ambient
`mad rA, rCube, cK.x, rD` and the diffuse chain c7 -> c6 -> c8 -> c5 behind it, and `add rX, rA, v0`; for VS the `dp4`
world triples and the object position `mul rP, r?.w, v0`). Its output `census_ps.txt` covers 72 shaders from the
`LightProbe-*` folders.

**Outcome:** it was the pattern study behind `PatchObjectLampPs`.

### 2026-09-25: refused shaders dump

**Context:** the first version (about 13:50) named refused-shader files by pointer (`<fix>_PS_<ptr>.bin`) in the combined
build's `S3SS\ShadersRecusados\`. Log messages were in Portuguese (`NoteShader "<fix>: sem o padrao esperado, fica como
o jogo"`, `"[LotLightBridge] <fix>: shader <ptr> recusado pelo D3D (<hr>)"`); a file example is
`Folhagem__sombra_da_lua__PS_0556959F.bin` from the fix "Folhagem (sombra da lua)".

**Finding:** pointers change every session. The deep review at about 14:35 switched to content names and a cap of 300
files per session. Its first use diagnosed 14 refused "Objeto de fora (luz do chao)" PS
(`scratchpad\snowcover\test5.cpp`).

**Outcome:** 4 newly accepted variants and 6 identified as HD "Night" shaders that already have per-pixel lamps. On
2026-09-28 the folder held 405 files from several sessions.

### 2026-09-25: offline validity

**Context:** review at about 03:30.

**Finding:** DXVK accepts some invalid SM2 code that native D3D9 refuses (a `lrp` with two constants in ps_2_0).

**Outcome:** validity of a patched shader is judged by `D3DDisassemble` offline, not by "it ran in game"; instruction
slots (ps_3_0 512 minimum) are also checked.

### 2026-09-25: census and false colour built

**Context:** the deep review (about 14:35, "Pendente, proposto pelos revisores", item 2) asked for an automatic census:
every outdoor VS/PS pair no fix took, with the refusal reason and pixel coverage, and a false-colour mode (magenta = no
fix). Built at about 17:00 (NOTAS "Censo + cor falsa") in the combined build: Apex tab > Night Lighting > Developer >
Tools, below "Save light diagnostic"; a function-static, unsaved checkbox "False colour: magenta = gets lamp light but
no fix applied"; button "Census: write S3SS_Censo.txt" with the tooltip "Lists every shader pair that draws with baked
lamp light or an outdoor rig, and which ones the mod fixes."; output `S3SS\S3SS_Censo.txt` and `S3SS\Censo\` with a
Portuguese header (`S3SS Censo: desenhos que recebem luz assada (mapa de luz) ou rig de fora, por par de shaders`,
columns `desenhos | corrigidos | triangulos | rig | VS hash/tamanho | PS hash/tamanho | texturas`, texture labels
`s<n>:mapa<W>x<H>` and `s<n>:terreno`, footer `N pares, M sem correcao (codigo em S3SS\Censo)`, log `Censo gravado`).
The hooks were also registered by a wall strength or HDR lamp gain different from 1.

**Finding:** census of about 17:05: 84 pairs, 49 without a fix. Mapped to technique names with
`scratchpad\passo3\ground\techof.pl` (precomp + `scratchpad\shd\knm.tsv`), output `scratchpad\censo_tech.txt` (one line
per census PS: `PS_<fnv>.bin md5=<8 hex> blobs=<precomp blob indices> techs=<Technique:passes ...>`).

| Verdict | Pairs / techniques |
|---|---|
| Need nothing | Picking passes (Pick*, LotPondPick, PoolPick...), shadows and imposters (ImposterShadows, LotImposter), TerrainFog, glass (GlassForObjects/Portals); interiors (InteriorWall/Floor, Ceiling, Rug, FloorThickness, Masked/Normal); TerrainHigh (layers without light); "Night" (FFAEA7F4 and others, 7 PS in `censo_tech.txt`: the HD mode with 6 per-pixel lamps); Phong with a 32x32 map and rig 0 (94 draws: indoor objects, left as is) |
| Fixed right after | Outdoor Phong C249A5C0 and BC7DE009 (rig 2): `PatchObjectLampVs` picks the triple fed directly by POSITION when several exist (VS_E3A718D3: world c19..c21, view c12..c14); `PatchObjectLampPs` accepts sun in c13; flow control allowed after everything the patch reads and inserts |
| Pending at the time | OutdoorProp C0C6E0FF (and 2BE88B48): 1 lamp + sun structure, needs its own recogniser, VS already uses TEXCOORD7; foliage vs_2_0 VS_4375A3EE/EAB58655 with PS 936D7C02 (46 draws, 4-light matrix c8..c13, `max r0, r0, c31.w`, unknown which is the sun); instanced (POSITION1) SingleObject D4FD3CB3 / VS 9E59FCE3 and 8F40BA1C; TerrainLow FF6760A8 (distant terrain, DXT5 map in s3, not smoothed); ExteriorFloors 9F1F5543 (fixed at about 17:25 with `floor_atlas_table.h`, 261 PS); Sims |

**Outcome:** the census drove the later generalisations (Phong outdoor objects, summer ExteriorFloors, Counters/Phong
coverage). ROADMAP-NIGHT-REMAKE.md makes it the acceptance measure. NOTAS (PASSO3 MUST-FIX 8) asks to count refused vs
replaced ExteriorWall/ExteriorFloors draws before enabling them by default; the census can be reused for that.

### 2026-09-28: a later census

**Context:** census file of 12:47, another scene (187 files in `Censo\`).

**Finding:** 62 pairs, 38 `SEM`.

**Outcome:** numbers depend on the scene; compare censuses of the same save and camera only.

### 2026-09-28: standalone plan

**Context:** PLANO-SEPARACAO.md section 4.

**Finding:** the plan renamed the file to `ApexRadiance_Censo.txt` but did not name the `Censo\` and `ShadersRecusados\`
folders.

**Outcome:** both folders live in the Apex Radiance folder; the file header, texture labels and log line are in English;
false colour is saved in `[developer] controls` and the tools run only in developer mode.
