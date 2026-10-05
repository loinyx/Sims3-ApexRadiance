# Walls: history

Chronological record of investigations and design decisions. Newest entries at the bottom. The current behaviour is
described in [features/night-lighting/walls.md](../features/night-lighting/walls.md). The story-sharing investigations
are in [night-lighting-level-light-share.md](night-lighting-level-light-share.md).

### 2026-09-25: walls darker than objects

**Context:** m75, 16:25: a wall next to a lamp was much darker than the objects in front of it.

**Finding:** the wall's lamp light is only its baked map x cK.x from a room solve with k2 = 0.075. In 24 ExteriorWall
variants c3.x is the bloom threshold, not the lamp scale, so K must be found per variant.

**Outcome:** `wall_lamp_table.h` (58 variants with their K) and the per-draw wall gain, by pattern rather than exact-byte
HLSL replicas (decision around 15:10: pattern patches tested against the whole shader package).

### 2026-09-25: per-pixel wall plan (PASSO3), not implemented

**Context:** a per-pixel lamp model for walls was designed (`PASSO3-PLANO.md`): design A (bounded: lamp = B + s x V x
(P - R), clamped to [B/k, k x B]) was chosen over design B.

**Finding:** the critique listed items to fix before any implementation:

1. Cutaway and walls-down lower the wall in the VS but not the atlas UV: P and R at the wrong height, sliding pools.
   Export the full-height lot-local position (`mul o10.xyz, v0, c18.x`, tokens `03000005 E007000A 90E40000 A0000012`).
2. "SM2 wall families B and C" (`6B4833BB`, `8826CD07`) are InteriorWall; outdoor lamps there would leak. Guard with
   atlas alpha < 1/255.
3. Hooks would never register for a new option unless added to `UpdateHooks`.
4. Room lists are written on the light tree thread: sanitise lamp blocks or snapshot on the gather thread.
5. No AddRef'd texture cache (managed atlases per story per lot).
6. k = 2, not 4 (ghost cones up to 4x).
7. Count refused against replaced ExteriorWall draws before any public default.

**Outcome:** never implemented in any build. The roadmap kept it as phase 2, increment 1, and a single lamp model for
walls, objects and roofs as phase 3; the wall gain remains a manual correction. Revisit `PASSO3-PLANO.md` before
starting it.

### 2026-10-04: daytime lamp term

**Context:** the wall gain multiplied cK.x, which some variants set to zero by day, so walls lost their lamp light
entirely by day. Daytime F7 10:28:03:
`PS_265EBE18.bin` with `c3 = (0, 0.188235313, 0, 0)`.

**Finding:** the native daytime factor discards all baked lamp RGB. A first candidate used
`native x gain + (1 - night) x min(gain, 1) x 0.25`; the 11:13 follow-up confirmed full-day c3.x = 1 with that build, but
the wall was still too saturated.

**Outcome:** the shared daytime factor was lowered to 0.08 (`kDaySurfaceResponse`); full night keeps the exact previous
multiplication; saved strengths below 100% stay lower; no saved setting is rewritten. A separate enabled switch
(`paredesComLuz`) keeps Off native by day. Previously the gain applied only when it differed from 1.
