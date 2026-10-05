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

**Context:** review at about 03:30, item 5, and a later review the same day.

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
