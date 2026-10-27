# Night Lighting, fences: history

Chronological record of investigations and design decisions. Newest entries at the bottom. The current behaviour is
described in [features/night-lighting/fences.md](../features/night-lighting/fences.md).

### 2026-09-25: fence root cause analysis

**Context:** fences stayed dark next to lit ground (first fence capture `LightProbe-cerca` draw #106, alpha test,
cull 1; the same shaders as bushes `LightProbe-arbusto` #103: `VS_2BC7F188` + `PS_2BC82F40`, PS = `kObjectRigPs`, 600
bytes; each instance, index `a0.z`, gets 3 directional lamps, directions `c27/c28/c29[i]`, colours `c54/c55/c56[i]`, plus
the sun `c137/c138`). A four-part analysis plus a synthesis studied the
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

### 2026-10-05: per-pixel lamps on fences, walls blocking them

**Context:** an F7 capture showed a brick fence group of 57 instances taking the red lamp's ground atlas, which has no
direction, 15 to 30 m from it; the top of a deck's stone half wall took the ground pool of a wall lamp under the deck; a
brick fence behind a tower took the wall lamp on the other side.

**Finding:** the instanced vertex shader already passes the world normal and height (TEXCOORD2), so the fence pixel
patch can take the objects' per-pixel lamps chosen around the group's centre (read from instance stream 1).

**Outcome:** commits `ed575ee`, `664dac1` and `d14fee4`: per-pixel lamps, no atlas on any face when they are present
(groups without a readable centre keep the atlas on every face), and a lamp dropped when an outside wall hides it from
the group's centre and both ends. Released in 2.7.0.

### 2026-10-06: the game's own fence lighting in full daylight

**Context:** port of idavidveiga's day/night isolation fixes (commit `6427d96`).

**Outcome:** with a night level of 0.01 or less the fence patch is not drawn, so fences keep the game's daylight look;
`SurfaceLampGain` still fades the lamp term at dusk and dawn. Released in 2.7.0.

### 2026-10-12: spatial lighting and generation update preparation

**Context:** outdoor walls update between stories; fences and gates use different lighting compositions.

**Finding:** shared fence-group lamp brightness and directionless ground light on vertical object faces can obscure spatial lamp response. Deferred wall refresh requests can already be covered by a newer generation.

**Outcome:** retain spatial fence lighting with native fallback, per-pixel object cones, vertical ground-light isolation and generation-aware wall refresh. Offline CPU/GPU exploration remains research rather than a shipped companion DLL. Final release identity and gameplay coverage are recorded in the validation page.

### 2026-10-22: the fence group's lamp pick kept

**Context:** commit `d625404`. Every draw of an instanced group (fences, stairs, railings) picked its lamps again: the
lamp choice, the wall and slab test of each lamp, the cones, the reach rows and the ground share.

**Outcome:** the pick moves into `PickFenceLampsImpl` and is kept in a 256-entry memo keyed by the group and the
generation of its positions, with the outdoor objects' pick memo rules: valid while the lamp list and the wall and slab
test state are the same before and after the pick, never for a pick with a lamp shining out of a house, and not used
under F8 recording or a light capture. Development builds compare one hit in 64 with a new pick and turn the memo off on
a difference. Not yet checked in game ([validation](../validation/night-lighting-fences.md)).

### 2026-10-23: gates and narrow gaps in the lot shade

**Context:** an F8 at night: a garden post's light passed through a closed wooden gate onto the ground behind it, and the
gate took far less light than the fence beside it (F7: 0.285 against 1.0 at pixels 0.7 m apart).

**Finding:** a gate is an object, not an instanced fence piece, so the fence line had a gap in the lot shade mask. The
first gap filler (`415ff06`) took a gate within 0.35 m of the fence line, but the developer log of `1109e53` showed both
gates of a lot 0.50 m off it: a gate's origin is its tile's centre. A gap was then kept only while some other object
stood on its line, and after a shade update a group made again over the same fence was listed twice, its second copy's
end beside the gap made the gap look joined, and gaps were found only from the lower-index piece, in a hash map's order.
In-line gaps a few centimetres wide (a post, short pieces) let light round them in a wedge. On the reach law the gate
took the walls' gain 0.30 like a window frame, and fences took it on top of their own strength. The fence copy read the
mask at its own footprint, on the edge of its own shadow: a dark band under 1 m on the face turned to the lamp.

**Outcome:** gaps from either end, kept once by key; a gate within 0.6 m, kept until not drawn for 30 s while the fence on
both sides was drawn; gaps under 0.4 m always closed (`baf0ad0`, `1fe6714`). Loose objects and fences take gain 1, objects
set in their wall keep the walls' gain (`26bacaf`, `1dc4da7`); faces read the mask 0.6 m in front. Not verified in game.

### 2026-10-27: picks with lamps shining out of a house kept

At night in a neighbourhood the fence fix cost 22 us a draw, 15 us of it in the lamp pick, although the pick was memoised:
every pick that weighed a lamp shining out of a house was left out of the memo, because that lamp's test reads its own copy
of the walls. That copy is a snapshot replaced whole when it changes (`g_exitSnap`), so its generation (`g_exitGen`) now
moves with every replacement and is part of `LampVisibilityState`; the fence and object pick memos keep those picks.
Development builds still compare one hit in 64 with a fresh pick bit for bit.
