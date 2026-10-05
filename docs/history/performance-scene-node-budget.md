# Spread New Objects Over Frames: history

Chronological record of investigations and design decisions. Newest entries at the bottom. The current behaviour is
described in [features/performance/scene-node-budget.md](../features/performance/scene-node-budget.md).

### 2026-09-29: scene node budget (plan candidate C6)

**Context:** plan section 2.1: Scene::BeginFrame is the dominant cause of 31% of the 25 to 50 ms camera-moving hitches
and 26% of the 50 ms and longer ones; the pending-node drain once processed 2,224 nodes in 2.84 ms in one frame (usually
few). Inside it most of the time is the per-node update (materials resolving textures, the lookups of C1).

**Outcome:** shipped experimental, off by default; merged with C8 in v1.5.0.

### 2026-09-29: suspended in v1.8.0 over node lifetime

**Context:** a review found that a node held past its frame could be freed while still linked: the node destructor does
not unlink `+0x18`, and AddNode re-pushes a linked node. A crash reported at the time (garbage EIP 0xB9497401) turned
out to be a different bug.

**Finding:** the facts were re-read in the disassembly: the game itself never frees a node in a live list (the guarantee
is structural), but a node Apex keeps queued across frames needs explicit protection.

**Outcome:** brought back the same day, still experimental and off by default, with the node lifetime guard (hooks on the
node destructor, AddNode and the holder teardown) and developer-mode consistency checks.

### 2026-10-02: on by default (2.5.5)

**Outcome:** default changed to on. The developer tuning sliders, unsaved at first, are now saved with the developer
preferences. See [group history](performance.md).
