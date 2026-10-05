# Night Lighting, fences: history

Chronological record of investigations and design decisions. Newest entries at the bottom. The current behaviour is
described in [features/night-lighting/fences.md](../features/night-lighting/fences.md).

### 2026-09-25: fence root cause analysis

**Context:** fences stayed dark next to lit ground (first fence capture `LightProbe-cerca` draw #106, alpha test,
cull 1; the same shaders as bushes `LightProbe-arbusto` #103: `VS_2BC7F188` + `PS_2BC82F40`, PS = `kObjectRigPs`, 600
bytes; each instance, index `a0.z`, gets 3 directional lamps, directions `c27/c28/c29[i]`, colours `c54/c55/c56[i]`, plus
the sun `c137/c138`). A workflow of four analysis passes plus a synthesis ("fence-lamp-rca", about 02:45) studied the
`vs_3_0` family.

**Finding:**

- Cause 1 (certain): the `vs_3_0` shader reads only the vertex-light slots, which `FUN_006BA340` fills with overflow
  lights. Proof in the same capture (window m36): PS `c0` = sun, PS `c1`/`c5` = a lamp, VS `c4..c11` = 0.
- Cause 2 (certain): one rig per `SceneModelArray` group, at the group centre.
- Cause 3 (probable): the room queried at the group centre puts the rig in mode 0 in roofed rooms; matches m32 (one grey
  0.354 light with w != 0).
- Fenced areas (m15): roofless rooms, rig mode 1, room-only lamp gather.
- Premise corrected: earlier (note m03) the missing bit `model+0x29C & 4` / `rig+0x224 & 0x10` was blamed and
  `RigCtorForce` was added ("Also stairs, railings and columns"). The SceneModel constructor `FUN_006F5B40` already writes
  0x10, and the fence and stair code never calls `FUN_006F4840`; `FUN_006CEB20` is the CAS compositor, not the model
  constructor. `RigCtorForce` changes nothing for fences and stairs.

**Outcome:** the pixel-shader ground term `max(atlas x strength, vertex lights)`; fenced yards fixed on the CPU by
`RoomGatherThunk` at `0x006BBE70`. Filling the main rig slots was rejected for this family.

### 2026-09-25: weak fence m32

**Context:** a fence with one grey light was much weaker than the ground.

**Finding:** low intensity (1/d^2 from the lamp head plus the rig cap) and per-vertex `N.L` on side faces.

**Outcome:** solved by the atlas term, not by the rig.

### 2026-09-25: recognition coverage

**Context:** `IsInstancedStructureVs` scan over the captured vertex shaders.

**Finding:** at first only 3 of 116 captured VS matched (stairs `216C8910`, fence `21793C00`, railing `21849CA0`); later
captures brought 9 variants (`VS_216C8910` ... `VS_28228DD8`).

**Outcome:** kept.

### 2026-09-25: plans not implemented

**Context:** open items from the notes and roadmap.

**Finding:**

- An optional rig-mode filter for the atlas term (indoor railings), with a binder thunk at `0x006F68C3`; RigTracker
  patches the CALL at `0x006F68C5` and offers `CurrentMode()`.
- Roadmap 1.4: instanced objects (grids, columns, repeated fences with per-instance positions) beyond the nine known VS.

**Outcome:** not started.

### 2026-09-28: per-pixel lamps on fences (combined build)

**Context:** probe7_cerca: a fence 18 m from the nearest lamp got only the atlas at its foot (0.055) although its upright
faces looked at that lamp.

**Finding:** all nine captured instanced VS also write `mov oT2.xyz, rN` (world normal) and `mov oT2.w, rW.y`, giving the
pixel shader the full world position.

**Outcome:** the combined build added an optional `worldY` to `IsInstancedStructureVs`, a lamp variant of
`PatchInstancedLamps` (8 lamps through `AppendPixelLamps`, bake-matched law `colour x min(1, W x sat(N.l)/d^2)`,
`W = 0.4 x range`, constants `cS = cA + 2`, `cL = cA + 3..cA + 18`, temps `T..T+5`, refused if `maxConst + 2 + 18 >= 224`
or `maxTemp + 6 >= 32`), lamps chosen once per group with `SelectPixelLamps` around the rig centre (`rig+0x140`,
`RigTracker::CurrentCentre`) in rig modes 1 and 2 only, and two caches (`g_fenceLampPs`, `g_fencePs`). The variant was
planned as the first change to re-add to Apex Radiance, but it is not in the current code: Apex Radiance started from the v0.1.0
baseline (`b84d5f1`), which has the ground term only. Whether rails drawn through the instanced batch flush
`FUN_006CF920` see a rig at all was never verified (RigTracker clears the rig inside that flush). The combined build also
multiplied both fence strengths by the HDR lamp gain (see [removed-features.md](../removed-features.md)).

### 2026-10-04: daytime lamp response (PR #2)

**Context:** an F7 of an outdoor bench at 11:13:54, by day, showed full lamp-strength atlas light.

**Finding:** the bench is an instanced-structure draw (three instances, 160 primitives) with patched `PS_3019F738.bin`
(912 bytes, FNV-over-DWORDs `C0F5CCD7`). Its lamp term is `max(atlas x c13.x, COLOR0)`; `c13.x` was 0.993083 while the
night level was zero. VS `c8..c11` were zero, so the lamp colour came from the atlas. An earlier object-only adjustment
did not reach this path.

**Outcome:** `DrawInstanced` (and snow on objects) uses the shared `SurfaceLampGain`. A first candidate used a quarter
response at full day (`0.25 x min(strength, 1)`); after gameplay feedback on it the endpoint was lowered to 0.08. Full
night math, the native vertex-light lower bound and solar inputs are unchanged. Commit `92582b6`.
