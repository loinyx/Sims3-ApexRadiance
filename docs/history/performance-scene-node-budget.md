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

### 2026-10-05: a growing budget instead of a forced full drain; flat registry

**Context:** commit `a12f8ab`. Once a node had waited `max_wait_ms` (500 ms) while the camera kept moving, the game's
full drain ran: a single long frame in the middle of the move.

**Finding:** the backlog can be cleared over a few frames instead.

**Outcome:** past the longest wait the per-frame node and time limits double every 50 ms (x2 up to x64); the game's
drain runs only after three times the longest wait. The node registry became a vector sorted by link, rebuilt once per
budgeted drain without allocating, instead of an `unordered_map`. Released in 2.7.0.

### 2026-10-24: draw state never deferred (issue #37)

**Context:** issue #37: access violations at 0x0071D56E (Swarm effect list draw) and 0x006C93D0 (fur model draw) while
the camera pans over streaming lots. The crash log showed the Swarm entry removed after the last list rebuild, and that
rebuild run by the budgeted copy.

**Finding:** both draws read state that only the node's own update refreshes (the Swarm bucket lists; the fur model's
current LOD slot after an LOD unload). The game's full drain always runs those updates before the draw; a budget that
leaves the node queued does not.

**Outcome:** past the budget the copy still updates nodes whose update is the Swarm list gather or whose draw is the fur
draw; the others are deferred as before.
