# Night Lighting, foliage: history

Chronological record of investigations and design decisions. Newest entries at the bottom. The current behaviour is
described in [features/night-lighting/foliage.md](../features/night-lighting/foliage.md).

### 2026-09-24: dark planters

**Context:** plants in the plaza planters looked dark on one side (`LightProbe-conjunto`, `LightProbe-arbusto2`).

**Finding:** the darkness was first attributed to the room-mode gather (objects inside the plaza "room"). The captures
showed per-instance lamp colours of 0.8 to 0.99: the lamps arrived, and the moon shadow multiplied them away.

**Outcome:** the 600-byte object-rig pixel shader was replaced by an HLSL copy that lifts the shadow at night. Confirmed
in game the same day.

### 2026-09-25: black back of winter bushes and dark winter foliage

**Context:** the back half of winter bushes was black (m22, screenshot 43); winter bushes stayed dark although lamp colours
arrived (m21). Snow fix list item 5; item 5b was a snowy tree (m28, `PS_2A83D588` / `VS_2A839190`, no shadow map).

**Finding:** the vertex shader zeroes the back side with `max r0, r0, c132.w`; winter pixel shaders multiply lamp light by
the moon shadow with an `lrp` fade.

**Outcome:** `PatchFoliageVs` (wrap lighting, also for the snowy tree) and `PatchLeafShadow`.

### 2026-09-25: review fixes

**Context:** an early review, item 5, and a later review the same day.

**Finding:**

- `PatchLeafShadow` emitted `lrp` with two constant registers: accepted by DXVK, invalid on native D3D9.
- `PatchFoliageVs` accepted only a runtime `cK.w`, so the winter bush m63 (`def c131 = 0`) was never classed as foliage.
- `PatchLeafShadow` required `t5.x` and a runtime `cK.y`; m63 uses `t6.x`, m78 `def c3.z = 1`.
- When `cK` is a `def` in `PatchLeafShadow`, its value must be exactly 1 (the "no shadow" end).

**Outcome:** `add` + `mad` instead of `lrp`; `def`-zero clamps and `def`-one fades accepted; the VS count matched rose from
6 to 8.

### 2026-09-25: plants on lots without lamp light

**Context:** per-instance lamp colours `c54..c80` were zero or 0.10 on lot plants (m38, m40), while correctly lit
vegetation had 0.42 to 0.58 (m41).

**Finding:** the rig boost did not include lot lamp classes.

**Outcome:** fixed on the CPU in `ObjectLightBridge` (9-class rig boost, see
[objects-and-rigs.md](../features/night-lighting/objects-and-rigs.md)).

### 2026-09-25: open items from the roadmap (date approximate)

**Context:** summer plants and flowers stayed darker than the lit ground (flower on a log wall, m79, rig 0.62 / 0.55 /
0.49); plant 2 (m65) stayed dark with patched shaders.

**Finding:**

- Roadmap 1.1: export the atlas UV from the `vs_2_0` VS through a free constant `c254` and take `max(light, atlas)` in the
  `ps_2_0` PS, in two formats: `add r3, r3, t4` in the bush, `mad r0, t2, shadow, sky` in the flower.
- Roadmap 1.5: ground light and per-pixel lamps for the summer `kObjectRigHlsl` objects.
- Roadmap 1.3: the 4-light foliage VS (46 draws in the census).
- Plant 2's PS multiplies all lamp light by `texld s2` (TEXCOORD3, 128x128 DXT1), while plant 1 has an 8x8 white texture.

**Outcome:** not started.

### 2026-09-28: background compilation and pre-created vertex shaders

**Context:** the combined build compiled `kObjectRigHlsl` at the first draw, created foliage vertex shader copies early
at `CreateVertexShader` (`PrecreateVs`, a 64-entry pool keyed by patched bytecode, `kMaxPendingVs`, log line "foliage
vertex shader copies made at shader load"), and folded an HDR lamp gain into the `c255` wrap constants (a second VS copy
per gain).

**Finding:** the standalone started from the v0.1.0 baseline, which creates the copy at the first draw and has no HDR.

**Outcome:** `kObjectRigHlsl` is compiled at start-up by `framework/shader_cache`. The early vertex shader pool and the
HDR variant are not in Apex Radiance (HDR: see [removed-features.md](../removed-features.md)).

### 2026-10-20: per-vertex lamps and crown shade (2.14.0 development builds)

**Context:** foliage took each lamp at one point of its rig (the box point nearest the lamp), so a small lamp at the
foot of a tree lit the whole crown like a street lamp. The work went in over five commits (`9e07077`, `f096e31`,
`7859a8c`, `1a6930a`, `c22e8d7`).

**Finding:**

- First build (`9e07077`): per-vertex lamps almost never applied (3,428 draws against about 377,000 sent back to the old
  shader), and the count of new instances kept growing: the space of the instance positions was wrong.
- `f096e31`: the space (camera-relative or world) is found by matching instances to outdoor lamps both ways; crowns
  and per-vertex lamps wait until it is known. Constants are restored after the draw; crowns are merged by place and
  start above a tree's clear trunk.
- `7859a8c`: sending a whole draw back when one lamp slot could not be confirmed lost per-vertex light on most trees.
  A slot that cannot be confirmed now keeps the old light for that lamp only.
- `1a6930a`: the trunk and branch variant is patched by replacing the three lamp weights and keeping each variant's own
  colour sums. Lamps are confirmed by direction; the rig colour carries wall shade and caps, so colour refuses only gross
  mismatches.
- `c22e8d7`: a lamp at the foot of a tree was still refused because its colour was compared on the crown only. It is
  compared over the whole mesh box, trunk included.

**Outcome:** kept and shipped on in 2.14.0. Offline checks: `tools/foliage_vertex_test` and `tools/canopy_test`. Not
confirmed in game; the crown may look darker than in earlier builds with a lamp at its foot.

### 2026-10-20: a small lantern lit a whole crown red (2.14.0 development builds)

**Context:** in a snowy park lot a red garden lantern (type 3, range 15, intensity 1, colour (1, 0, 0), 5 cm above the
snow; reach law r0 2 m, R 6.3 m) lit the whole crown of a large bare tree strongly red, up to its top about 15 m away,
while the snow 2 m from the lantern stayed dark (F8 `2026-10-20 19-45-21` and `19-46-11`).

**Finding:**

- The log counted 1,500 instances with every lamp confirmed against 475,290 with a neutral lamp: almost every foliage
  instance drew its lamps by the old per-instance light (one colour and one direction for the whole instance).
- That colour is the reach law at the rig box's point nearest the lamp. The rig's box is the whole model's world box,
  down to the ground and widened by the yaw; the lantern was within about 2 m of it, so the colour clamped to (1, 0, 0).
- The slot went neutral because the colour recomputed on the mesh box (no yaw) found no light there ("colour not
  computed"), exactly for lamps at the edge of their reach.

**First outcome:** a lamp found by its direction was always drawn by its law at each vertex, the rig's colour only
dimming it. Installed as the public 2.14 build, its log showed that this was not enough: 9,058 lit slots by the law
against 607,905 with no lamp found in their direction at all, and a second capture (`20-36-15`: a red cube lamp, type 3,
range 70, 0.2 m above the grass, 4 to 5 m from a leafy tree; reach law r0 3.2 m, R 13.6 m) lit the crown in red patches
up to its top, where the law gives about 19% at 6 m, 3.5% at 9 m and nothing past 13.6 m.

**Outcome:** each foliage instance picks its own three lamps by the per-pixel lamps' rule (`PickFoliageLamps`: lamps
within reach, indoor lamps shining out, cones and walls' share at the crown's middle, ranked by the law at the mesh box)
and the draw replaces the per-instance colours `c54..c80` with theirs; the rig's slots are no longer used for light.
Colour x min(law, 1 / brightest channel) keeps the hue. Offline: 19 checks (red lantern crown 0; a warm post next to a
leaf capped at its own hue). The log counts instances by their own lamps (0 to 3), lamps left out by walls and
instances without a crown shape, and names why a foliage vertex shader is refused (its code kept in `Censo`). Not
confirmed in game.

### 2026-10-22: a lot tree lit white by an area light

**Context:** commit `b863bce`. In one session a lot tree was lit bright white at load while the trees around it were
not.

**Finding:**

- In a lighting snapshot of that session, the only non-lamp light the lamp reader (`ReadLamp`) accepted was a lot's
  rectangle area light: white, range 100, intensity 1, at ground height. The lamp reader keeps any outdoor light, and
  area lights (types 9 and 10) entered the per-pixel lamp list as lamps lighting all round, so such a light lit a whole
  tree crown at full strength.
- The game lights only one side of an area light: its law (`0x006BFBA0` / `0x006BFDC0` -> `0x006BFAF0`) weighs the
  light by `sat(dir.z)`, the direction to the area in the light's axes, with no `N.L`
  ([engine](../engine/light-objects-and-rigs.md)). The game's own room solve gave that light 0.11 to 0.27 at samples
  about 3 m away.
- The foliage mesh boxes are kept by vertex buffer address and were not cleared on a world change, so a buffer of a new
  world made at an old one's address took that mesh's box and instance count; the instances' picks of the old world's
  lamps were kept too.

**Outcome:** an area light carries its facing as its cone (the third axis, no spill), for objects, fences and trees;
roofs and water still take it without a cone. A world change clears the mesh boxes and the instance picks. Not yet
checked in game ([validation](../validation/night-lighting-foliage.md)).
