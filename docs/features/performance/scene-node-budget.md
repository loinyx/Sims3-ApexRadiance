# Spread New Objects Over Frames

While the camera moves, objects that just loaded or moved are placed in the scene a few hundred per frame instead of all
at once, so panning over a lot that streams in stutters less. An object may appear a frame or two later; everything is
placed at once as soon as the camera stops.

## Status

| | |
|---|---|
| Availability | Released in 2.1.0 or earlier; on by default since 2.5.5 |
| Default | On |
| Menu | System > Performance > Camera and lighting > *Spread new objects over frames* |
| Configuration | `[patches.SceneNodeBudget]` in `ApexRadiance.toml` |
| Source | [`features/scene_budget.{h,cpp}`](../../../features/scene_budget.cpp), [`patches/performance_patches.cpp`](../../../patches/performance_patches.cpp) |

## The problem

At the start of every frame, `Scene::BeginFrame` drains the scene's pending-node list: every scene node queued since the
last frame (new, moved or changed) is updated and placed in the spatial tree. Most of its time is the per-node update
(materials resolving textures through the resource manager). When a lot streams in, thousands of nodes can be drained in
one frame, which makes BeginFrame a dominant cause of 25 ms and longer hitches while the camera moves.

## How Apex Radiance solves it

Apex Radiance wraps BeginFrame's CALL of the drain:

1. **Camera still,** or a node has waited 500 ms: run the game's drain (everything).
2. **Camera moving:** run an exact copy of the game's loop that stops after at least 8 nodes once 512 nodes or 2 ms are
   reached. The nodes not reached stay in the game's own list, at the end the drain takes from, so they go first next
   frame.
3. **Guard every node left queued.** Hooks on the node destructor, AddNode and the scene holder teardown make sure a node
   held for later can never be freed or queued twice while it is still linked.

The other five callers of the drain are untouched; they process the nodes left exactly as they process nodes the game
queued after BeginFrame.

## Settings

| Menu label | TOML key | Type | Default | Range | Effect |
|---|---|---|---|---|---|
| Spread new objects over frames | `[patches.SceneNodeBudget] enabled` | bool | on | | Turns the budget on. A missing key reads as on |
| Developer > *Nodes per frame while moving* | `[developer.controls.scene_budget] nodes` | int | 512 | 8 to 4096 | |
| Developer > *ms per frame while moving* | `[developer.controls.scene_budget] time_ms` | float | 2.0 | 0.1 to 10 ms | |
| Developer > *Longest wait (ms)* | `[developer.controls.scene_budget] max_wait_ms` | int | 500 | 16 to 5000 | A node waiting this long makes the game's drain run |

## Compatibility and interactions

- **Spread lot lighting while moving:** both use the same camera eye sampler; this feature samples it once per frame
  whether or not that feature is on.
- **Frame Profiler:** "Scene pending nodes" is layer 0 of the call chain on 0x006EBC49. With the feature on it times the
  budgeted drain; `[this+0x18]` is the nodes processed this frame, and hitch lines add ", deferred N" (from
  `SceneBudget::TakeDrainNote`). Either install order works. The report adds "Spread new objects over frames: ...".
- **Official Sims3SettingsSetter:** its source patches none of 0x006EBC49, 0x006E4130, 0x006FB4B0, 0x006FAD70,
  0x006FD930, 0x006E6480, 0x006E4DE0 (the last three are also absent as literals from the installed
  `Sims3SettingsSetter.asi`, `Sims3Performance.asi` and `MonoPatcher.asi`). Its Lot Streaming "object streaming
  throttle" detours AddLotObjectsToScene 0x00AC1130 and limits how many objects are created per frame; this feature
  limits how many queued scene nodes are placed per frame. Both can be on.

## Limitations

- A node placed later is not drawn (new) or is culled at its old cell (moved) for the frames it waits. At the defaults a
  lot that streams in with about 2,000 nodes takes a few frames. Inferred look; if moving Sims or objects flicker at the
  edge of the screen while panning, lower the wait or raise the per-frame limits (Developer card).
- If one node dominates (a whole lot's model), a node budget cannot split it.
- Only scenes drained every frame (within 100 ms) are budgeted; others drain fully. Holder slots (8) are reused only
  after 1 s without a drain; with none free the scene drains fully.
- The drain list has no lock in the game; the budgeted copy runs where the game's drain runs (inferred: the render
  thread).

## Technical reference

### The game side (Steam 1.67.2)

Verified in the disassembly unless marked inferred.

- **Pending holder** = `[scene+8]` (0x68 bytes, constructor 0x006E4530, allocated in 0x006EB610): `+0x04..+0x08` the slot
  array of its nodes, `+0x18` counter (nodes processed by the last drain; nothing counts the list itself), `+0x20` /
  `+0x24` the sentinel {next, prev} of a circular intrusive list, `+0x2C` the spatial tree.
- **Scene node** (base constructor 0x006FD710, vtable 0x00FF9D00; refcounted: count at `+4` changed with `lock xadd`,
  vfunc +0 AddRef 0x005044C0, +4 Release 0x00692720, +8 deleting destructor): `+0x18` / `+0x1C` its pending link {next,
  prev}: 0 = processed by a drain, self-loop = not queued (constructor, RemoveNode), anything else = linked; `+0x30` its
  owner (the holder).
- **Queueing:** 0x006FAC70 MarkDirty, 0x006FCA20 (a node's children), 0x006FCB10 (LOD band change) and 0x006FD9F0
  SetOwner (vfunc +0x1C) all do `if (link.next == 0) { link = self-loop; if (owner) 0x006E42E0(owner, node); }`;
  0x006E42E0 is a plain push_back at the tail. Every push needs owner != 0 (children are queued on their own owner's
  list, only if they have one). SetOwner(0) self-loops a link that is 0 and never unlinks.
- **AddNode 0x006E6480** (thiscall(node, group), ret 8; only caller the scene thunk 0x006E84B0): returns at once when
  `node->owner != 0`; otherwise takes a slot, AddRef, SetOwner(holder), and at 0x006E64EF pushes the link again when it is
  non-zero, without unlinking it (correct for a self-loop, and for the stale link the teardown leaves).
- **RemoveNode 0x006E4920** (only when `node->owner == this`): clears the slot, unlinks a non-zero link and self-loops it
  (0x006E4984), leaves the spatial tree (0x006FB490), SetOwner(0,0,0), Release.
- **Holder teardown 0x006E4DE0** (only caller 0x006E97F0, which frees the holder right after): destroys the spatial tree,
  then SetOwner(0,0,0) + Release on every slot without unlinking; the list dies with the holder, and a surviving node
  keeps a stale link into freed memory.
- **Node base destructor 0x006FD930** (thiscall, ret): detaches its children (0x006FC860, which unlinks their `+0x20` and
  calls their vfunc +0x5C), leaves its spatial cell (0x00706200, only the cell's list at `+0x1EC`), owner = 0, unlinks its
  own `+0x20` (its parent's child list), destroys `+0x1F8` and stores vtable 0x00FA1B78 (the "destroyed node" vtable). It
  never touches `+0x18`. 0x006FAE00 only fills a local transform. The base vtable 0x00FF9D00 is written only by the base
  constructor and this destructor, so every node destructor ends in it (23 call / tail-jmp sites, the derived
  destructors).
- **Does the game ever free a node that is still linked?** Not while its list is live: a node can only be pushed with an
  owner, the owner comes from AddNode (which AddRefs the slot), and the holder's reference is released only by
  RemoveNode (after the unlink) or by the teardown (whose list dies with the holder). The guarantee is structural, not
  "drained within the frame", so holding a node across frames does not break it. Verified for every direct path;
  inferred that no other code calls SetOwner(0) on a linked node or drops the holder's reference (the indirect vfunc
  +0x1C calls with three zero arguments in the whole exe are RemoveNode, the teardown and a window class, 0x00588ED0).
- **The drain 0x006E4130**, thiscall(holder), ret, 0xD1 bytes: splices the whole list into a sentinel on its own stack and
  empties the holder's list (0x006E413E..0x006E4192); `[this+0x18] = 0`; then, newest first, while the local list is not
  empty: `l = local.prev` (re-read from the stack every iteration); unlink it; `l->prev = l->next = 0`; `node = l −
  0x18`; `node->vfunc+0x48()` (the base class's is `ret`); `b = 0x006FB4B0(node, &aligned32)`; `0x006FAD70(node, b)`
  (= `owner+0x2C -> 0x00705B70(node, b)`; skips a node whose owner is 0; its only caller is the drain);
  `[this+0x18] += 1` (0x006E41EF). Nodes queued during the loop go to the holder's (now empty) list.
- **Callers:** 0x006EBC49 Scene::BeginFrame (every frame), 0x006DF9B5, 0x006EDBC7 (a scene query that drains first so
  its answer is current), 0x006EF07D, 0x006F226B (a render-to-texture path, when asked), 0x006F3CF1. Each drains its own
  scene's holder (`[scene+8]`).
- **Threads:** none of the list functions takes a lock, so the game must use the list from one thread (the render
  thread, which runs BeginFrame; inferred). Release is atomic, so a node's destructor may run on any thread.

### The budgeted copy

- `Hook_SceneDrain` is layer 1 (`SceneBudget`) of `CallChain` site `SceneDrain` on the CALL 0x006EBC49 (the Frame
  Profiler is layer 0). Start compares the whole drain (0xD1 bytes, only the two CALL rel32s wildcarded) with the Steam
  code, checks that its two CALLs sit at +0xAE / +0xB6 and reach `SceneNodeBounds` / `SceneNodeSpatial`, checks the
  heads of AddNode (the owner test), the destructor (up to the base vtable store) and the teardown, and refuses
  otherwise.
- Per BeginFrame (render thread): samples the camera (`LotLightingMotion::SampleCameraMoving`). Moving ->
  `BudgetedDrain`: the game's loop instruction for instruction (same splice, same unlink order, same calls, same
  counter) plus a stop test before each node: at least `kMinNodes` (8), then stop at `nodesPerFrame` nodes or
  `msPerFrame` (QPC). The nodes not reached are spliced back at the tail of the holder's list, in order.
- The list is always well formed when Apex returns.

### The node lifetime guard

Every node `BudgetedDrain` leaves queued is recorded (`link -> holder`, an `unordered_map` under an SRW lock that is
never held across a game call; an atomic count gives the hooks a lock-free "nothing recorded" path). Three `EntryChain`
layers (`SceneBudget`) are installed before the drain hook and removed after it:

| Site | Entry | Prologue moved | What the hook does |
|---|---|---|---|
| `SceneNodeDtor` | 0x006FD930 (any thread) | `55 8B EC 83 E4 F0` | A recorded node whose link is neither 0 nor a self-loop, and whose neighbours point back at it, is unlinked and self-looped before the game destroys it; the record is dropped |
| `SceneAddNode` | 0x006E6480 | `56 8B 74 24 08` | For a node without owner (AddNode's only working case): a recorded node that is still linked is unlinked first (the game would push it over a live link); the record is dropped. A node with an owner is left alone |
| `SceneHolderTeardown` | 0x006E4DE0 | `53 55 56 57 8B F9` | The holder's records are dropped before its nodes are released (their links then point into a dying list, which the game leaves as it is) |

No branch in `.text` lands inside the moved bytes. The invariant the records keep: a recorded node whose link is
neither 0 nor a self-loop is in a live list (its holder's, or a drain's local list on the stack while that drain runs).
A recorded node can only be queued on its owner's list; its owner changes only through AddNode (record dropped) or the
teardown (records dropped); drains leave links at 0. So the hooks never write into a dead list. Records are replaced
after every budgeted drain (the nodes left now) and dropped after every game drain that goes through the hook; they are
kept while a drain runs, so a recorded node destroyed inside a drain is unlinked from that drain's local list, which
both the game's loop and the copy re-read every iteration.

Developer-mode checks (logged once; "stopped" = the game's drain runs until the game restarts):

- before each node of the budgeted copy: the tail's neighbours point back at it, its vtable is inside TS3W.exe `.rdata`,
  is not the destroyed-node vtable (read from the destructor's store at +0x8D, 0x00FA1B78 on Steam) and its +0x48 slot
  is inside `.text`; otherwise stopped;
- before each budgeted drain: the same for every recorded node of the holder that is still linked;
- a node found with the destroyed-node vtable but still well linked is taken out by writing only its neighbours (its
  freed memory is never written), never called, counted as "repaired" and logged once, without stopping.

Stop: `g_on` false (a thread still inside the drain hook runs the game's drain), the drain layer, then the three
lifetime layers are removed and the records cleared. Nodes still left are processed by the next BeginFrame.

### Address reference

| Id (`GameAddr`) | Steam 1.67.2 | Kind |
|---|---|---|
| SceneDrainCall / SceneDrain | 0x006EBC49 / 0x006E4130 | Sig / Target (fallback Sig) |
| SceneBoundsCall / SceneNodeBounds | 0x006E41DE / 0x006FB4B0 | InRange(SceneDrain, 0xD1) / Target (fallback Sig) |
| SceneSpatialCall / SceneNodeSpatial | 0x006E41E6 / 0x006FAD70 | InRange(SceneDrain, 0xD1) / Target (fallback Sig) |
| SceneNodeDtor / SceneAddNode / SceneHolderTeardown | 0x006FD930 / 0x006E6480 / 0x006E4DE0 | Sig (entry; alternates at +0x11 / +5 / +6) |

Group `SceneNodeBudget`.

### Source

`features/scene_budget.{h,cpp}`: `Start`, `Stop`, `Hook_SceneDrain`, `BudgetedDrain`, `Hook_NodeDtor`, `Hook_AddNode`,
`Hook_HolderTeardown`, `CheckRecords`, `NodeLooksAlive`, `LinkConsistent` (developer checks), `MatchAt` (the drain body
and hook head checks), `TakeDrainNote`, `Set*` (tuning), `GetStats`, `StatusText`, `RenderDeveloperUI`.

Developer card *Objects spread across frames*: drain call, drains (game's while still / game's after a too-long wait /
with a budget), nodes processed with a budget, frames that left nodes, node-frames waiting, largest backlog, the last
budgeted drain (done, left, ms); lifetime guard: nodes recorded, unlinked at destruction, unlinked before AddNode,
repaired (each expected 0), records dropped at teardown (expected after world or lot changes), recorded nodes destroyed
on another thread, left nodes whose owner is not the holder that drained them (expected 0). The Off line in the log sums
the same counters.

### Rules for maintainers

- Do not change the copy's order or the splice: it must stay the game's loop (newest first, unlink before the calls,
  the local sentinel re-read after every node because game code may unlink other nodes, the rest put back at the tail).
  Apex keeps no node pointer it dereferences outside the game's list: the records are keys (the link address), only read
  or written while the invariant says the link is in a live list.
- Do not remove the lifetime hooks, and do not turn them into unconditional unlinks: after a holder teardown the game
  leaves nodes with stale links into freed memory, and unlinking those would write into freed memory. Only recorded
  nodes are ever unlinked.
- Never write a destroyed node's memory: the "repaired" path writes only its neighbours.
- The teardown hook must drop a holder's records before the game releases the nodes; a new holder can reuse the address.
- Do not hook the drain CALL outside `CallChain`.

## Rejected approaches

- A budgeted drain without a node lifetime guard: a node held past its frame could be freed while still linked.
  Details in [history](../../history/performance-scene-node-budget.md).

## See also

- [Performance overview](README.md)
- [Validation](../../validation/performance-scene-node-budget.md)
- [History](../../history/performance-scene-node-budget.md)
- [Main loop and services](../../engine/main-loop-and-services.md)
