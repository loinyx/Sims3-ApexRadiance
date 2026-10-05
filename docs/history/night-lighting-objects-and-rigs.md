# Objects and rigs: history

Chronological record of investigations and design decisions. Newest entries at the bottom. The current behaviour is
described in [features/night-lighting/objects-and-rigs.md](../features/night-lighting/objects-and-rigs.md). Raw sources:
`NOTAS-ILUMINACAO.md` sections 3, 4, 5, "Porta escura" and "Counters".

### 2026-09-24: rig cap times strength

**Context:** note 1b. The rig cap was raised to 3x the game's cap times *Brightness*.

**Finding:** objects glued to a lamp head blew out.

**Outcome:** reverted; the cap is `cap x max(1, strength)`, the game's cap unless the strength is raised.

### 2026-09-25: rig boost for every light class

**Context:** captures m32-m41: fences, bushes and props next to a street lamp got about 30% of the ground light. Boosting
only the street-lamp class left lot lamps of other classes at about 0.1 or nothing on objects (m38/m40/m41).

**Outcome:** `ClassColour<I>` on the `vfunc+0x10` slot of all 9 light classes, restricted to outdoor lamps of type 3..6
for non-street classes.

### 2026-09-25: RigCtorForce does not fix fences and stairs

**Context:** stairs and railings outside had zero rig lamp constants (m03); `RigCtorForce` was added to set the
"accepts dynamic lights" bit at rig construction.

**Finding:** correction recorded at 02:45: the SceneModel constructor `FUN_006f5b40` already sets the model bit; the game
clears it only for terrain, roads, roofs, ceiling, sea, lot skirt and objects whose script asks. The dark fences and stairs
come from the vertex-light slots ([fences history](night-lighting-fences.md)).

**Outcome:** `RigCtorForce` kept for deliberately closed objects only.

### 2026-09-25: class-10 dispatch starved roofs and snow

**Context:** around 11:15, roofs and snow drawn with a vertex shader that also classifies as an object VS lost their lamp
light.

**Finding:** the class-10 branch returned early even when the object patch did not apply.

**Outcome:** the object branch falls through to the remaining branches when it declines. A review the same day also
introduced AddRef pinning of every classified shader until shutdown, so a freed address cannot inherit a stale class,
and moved the uninstall refresh to the render thread.

### 2026-09-25: doors, windows and counters confirmed

**Context:** doors and windows darker than the lit wall beside them (m52, m56); modular counter pieces with colour steps
(m55, "Counters vs Phong"; the interior Counters technique reads the room light map, so it is continuous).

**Outcome:** the ground-light patch on outdoor Counters/Phong objects. Confirmed in game around 11:35; upper-story windows
picking up ground light judged acceptable, so no height correction was added.

### 2026-09-25: accepted and refused object shaders

**Context:** around 13:50, refused shaders dumped to `S3SS\ShadersRecusados` and examined with scratch `test5.cpp`.

**Outcome:** generalising the recogniser accepted `PS_298DF5B8`, `PS_298DFD88` (no COLOR0), `PS_2D03FF80` and
`PS_2D041628` (4-light specular). HD objects with their own per-pixel lamps need nothing; `2BE88B48`, `2E279A20` and
`2E255A58` stay vanilla; `PS_2A74E378` belongs to the wall family.

### 2026-09-25: per-pixel lamps (v5.6)

**Context:** around 17:50, per-pixel world lamps were added to the object patch, with the rig zeroed during the draw so
the per-pixel term replaced it.

**Finding:** during development:

- TEXCOORD7 collided with Counters' sink/stove cut-out UV and Phong's projective coordinate; TEXCOORD8 is used by none of
  the 2139 shaders and became the fixed index.
- Treating POSITION1..3 as instancing refused 176 Phong morph-target VS; POSITIONn with NORMALn is now accepted.
- Subtracting vC in the PS to remove the vertex lights also removed the Phong ambient term `mad oC0.xyz, vNormal.w, cAmb,
  r`; the vertex-light base is found in the VS instead.

**Outcome:** v5.6 coverage: VS 588/588, PS Counters 136/184, PS Phong 426/444, 0 invalid. This is the form in the
standalone v0.1.0 baseline (b84d5f1) and in the current code.

### Date not recorded: standalone split (v0.1.0 baseline)

**Context:** the standalone took the CPU part (`object_light_bridge.cpp`: 9-class boost, cap, `RigCtorForce`, fenced
yards), `RigTracker::CurrentMode` and the per-pixel patch in its v5.6 form: `SelectLamps(x, z, 40 m)` nearest by
distance minus radius, kernel `sat(N.l) x sat(1 - d²/R²)²` with `R = clamp(1.2 sqrt(range), 2, 25)`, rig zeroed,
`rD = max(rD + Q x s, ground)`, ground facing `0.5 + 0.5 N.y`, ground strength *Brightness* alone.

**Outcome:** the combined build's later changes (next entry) were not ported; they were to return one at a time after
in-game tests.

### 2026-09-28: dark gate and the bake-matched law (combined build)

**Context:** LightProbe `probe3_cerca` / `probe4_portao`: with per-pixel lamps replacing the rig, a gate 9 to 26 m from 8
lamps was darker than vanilla. *Brightness* had been lowered to 0.38 to tame objects next to lamps, which left the gate
with 0.19x the ground light while the fence beside it got all of it.

**Finding:**

- Zeroing the rig made objects far from lamps darker than the game.
- The kernel `sat(1 - d²/R²)²` with `R = 1.2 sqrt(range)` (11.8 m for a street lamp) is 0 beyond R, and gave 13% of the
  nearest lamp at 9.4 m, while the atlas still shows the lamp there.
- Fitted on the light atlas around a street-lamp pair (range 97 at 1.7 m and range 40 at 3.7 m), the ground follows
  `2 x w x cos / d²` with `w = 0.2 x range x intensity` (the bake's per-light value, `0xC29526..0xC29533`), with the same
  factor (1.7 to 2.4) from 5 to 16 m.
- probe6: the facing factor `0.5 + 0.5 N.y` gave an upright gate half of what the fence beside it got.

**Outcome (combined build only):** `W = 0.4 x range` with the intensity in the colour and the per-lamp cap 1 (the atlas'
own saturation next to a lamp): `Q = sum colour x min(1, W x sat(N.l) / d²)`; an upright face turned to a lamp gets the
cosine the ground lacks (a fence 10 m from a lamp 3 m up: about 3x the ground value). `SelectPixelLamps(x, z)` scored lamps
by `W / (dx² + dz² + 1)`, dropped scores below 0.002 and kept the best 8 (a street lamp at 15 m beats a small lamp at 6 m).
The rig was kept, with the rule `rD += max(Q x s - Rg [- vC], 0)` then `max(rD, G)`, where `Rg = c5 sat(N.c1) + c6 sat(N.c2)
+ c7 sat(N.c3)`: `max(sun + max(rig + vC, Q x s), ground)`. Facing `sat(N.y + 1)`; ground strength `max(1, Brightness)`.
None of this is in the standalone.

### 2026-09-29: lamp enumeration

**Outcome:** `ReadEnumeratedLamps` (light enumeration `FUN_006acf70`) replaced `UpdateLampList` as the source of the lamp
list ([roofs](../features/night-lighting/roofs.md) holds the `SelectLamps` memo).

### 2026-09-30: facing factor tried again

**Context:** F7 captures 120-121: an upright object got half the ground light of the fence of the same material beside
it.

**Outcome:** `sat(N.y + 1)` was brought back (installed build 81934361) and undone the same morning on maintainer
feedback; `0.5 + 0.5 N.y` stays.

### 2026-10-01: test008, paintings inheriting furniture floor shadows

**Context:** session 20-37-31: a wall painting darkened when a chair was placed below it. The four directional maps were
byte-identical, while the floor-map texel near its origin changed from RGB 30 to 1; the indoor object's `min(basis, 2 x
floor map)` transferred this floor-height shadow to the painting.

**Outcome:** test008 omits the cap only while `LevelLightShare` reports its validated `BasisLightHook` installed and
indoor sharing ready; otherwise the original cap remains. Separate cached variants preserve the fallback. This does not
make object occlusion height-aware: the maps remain 2D and removing the cap may expose directional-map limitations near
walls. Packaged in 2.5.3 in both build flavours (since unified); the earlier release candidate was kept as a reference.

### 2026-10-04: daytime lamp response

**Context:** the furniture F7 of the 11:13 session identified the instanced-structure atlas path, not the CPU rig boost,
as the source of a bright daytime lamp term.

**Outcome:** `DrawObjectLamp` uses `TerrainLightingPolicy::SurfaceLampGain` for the ground-map and per-pixel strengths,
and the same policy reaches `DrawInstanced` and snow on objects. The full-day value was first `0.25 x min(strength, 1)`;
maintainer feedback found it too bright, and it became `0.08 x min(strength, 1)`, fading to the exact configured strength
at full night. Lower strengths are never raised; sunlight, ambient, lamp RGB, falloff and saved settings are unchanged; the
CPU rig boost has no day factor. GPU fixtures verify the object, wall and instanced paths under controlled inputs.

### Date not recorded: two-constant instructions

**Finding:** `PatchLeafShadow` / bicubic-setup style instructions reading two constant registers are accepted by DXVK
but invalid on native D3D9.

**Outcome:** every object patch instruction reads one constant register; `1 - d²/R²` takes two instructions.

### Combined build: HDR lamp gain

The combined build also multiplied the ground strength and the per-pixel lamp strength by `HdrOutput::LampGain()`. That
path does not exist in the standalone ([removed features](../removed-features.md)).

### 2.5.6: world-owned lamp edits

World-owned type-11 lamp edits reconcile terrain and native object rigs; see
[world lamp response](../features/night-lighting/world-lamp-response.md).
