# World lamp response: history

Chronological record of investigations and design decisions. Newest entries at the bottom. The current behaviour is
described in [features/night-lighting/world-lamp-response.md](../features/night-lighting/world-lamp-response.md).

### 2026-10-03: world posts ignored by the change tracking

**Context:** recolouring a world street post left the ground and objects with the old colour.

**Finding:** F8 identified the post's lamps as type 11 with lot id 0; `ReadLotLamp` rejected every lot id 0. The 01:09
F8 places the post's two lamps at (881.9, 62.2, 1250.1) and (882.1, 64.2, 1250.4) with a green current colour; F7 shows
green direct lamp constants, a blue native rig before replacement, and blue road output. Direct per-pixel selection
alone therefore does not establish correct native rigs or terrain maps; no change to lamp strength or falloff was
justified.

**Outcome:** world-owned type-11 lamps admitted, snapshot group 0, `AcceptEdit` guard, coalesced rig refresh.

### 2026-10-03: first world-lamp test build

**Context:** the first test removed only the lot-id-zero exclusion.

**Finding:** the remaining room-known gate (0x04) still rejected the captured world flags 0x73 / 0xF3. Testing only the
lot id did not cover the complete eligibility rule.

**Outcome:** `Eligible` does not require room-known for world-owned lamps. The second test build (test2) was accepted
and published in 2.5.6.

### 2026-10-03: six-second window retry removed

**Context:** a lamp-driven retry of the window light checks every six seconds was tried for the post delay.

**Finding:** it did not resolve the reported delay.

**Outcome:** removed; the startup window checks and indoor room invalidation remain. Do not restore it as a terrain or
rig response fix. The chunk completion through the native LOD path now sends `NoteChunkRendered` before completion, so
the smoothing cache does not rely on round-robin hash discovery.

### 2026-10-04: response review (PR #2)

**Context:** review of the edit completion path.

**Finding:** a native full terrain rebuild can finish before the 80 ms debounce; the terrain-covered path called
`FinishEdit`, ending the edit before its rig request was consumed, so objects could keep the older native light. On a
world change the direct lamp pool, memo generation and GPU rows survived until the next successful enumeration.

**Outcome:** `FinishEdit` consumes the rig request (each request once, reset on world change); the world change clears
the pool, memo, rows, scratch and animated count and requests a lamp read at once. No change to lamp colour, gain,
falloff or selection.
