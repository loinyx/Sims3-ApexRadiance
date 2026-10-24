#pragma once
// Scene node budget (Apex Radiance, feature "SceneNodeBudget", menu "Spread new objects over frames"; enabled by default
// when no saved value exists; docs/features/performance.md, section "How it works: Spread New Objects Over Frames (C6)").
//
// Every frame Scene::BeginFrame (0x006EBB70 on Steam 1.67.2) calls the pending-node drain 0x006E4130 on its scene
// sub-object: every scene node that was added, moved or re-bounded since the last frame (queued on the intrusive list at
// holder+0x20 through the node's link at +0x18) gets its update (node vfunc +0x48), its world bounds (0x006FB4B0) and its
// move in the spatial tree (0x006FAD70). There is no limit: a lot that streams in queues thousands of nodes at once
// (measured once: 2224 nodes, 2.84 ms in one frame).
// While the camera moves this feature answers that one CALL (0x006EBC49) with an exact copy of the game's loop that stops
// after `nodesPerFrame` nodes or `msPerFrame` ms; the rest goes back to the tail of the scene's own list and is processed
// first the next frames, except nodes whose draw this frame depends on their update (the Swarm effect lists, fur models:
// always updated; scene_budget.cpp, "Draw state"); once the oldest waiting node waited `maxDeferMs` the budget doubles every 50 ms (up to x64). When
// the camera is still, when nothing was left, or when it waited 3 x `maxDeferMs`, the game's own drain runs (all nodes). Effect: an object can appear, or finish moving in the culling tree,
// one or a few frames later while the camera moves; nothing is skipped.
//
// Node lifetime (the reason it was suspended in v1.8.0, fixed 2026-09-29): the game never frees a node that is linked in a
// live holder's list (the holder holds a reference; RemoveNode unlinks before releasing), but its destructor and AddNode do
// not unlink. So every node this feature leaves queued is recorded, and three entry hooks keep the guarantee for them even
// if some unknown path broke it: the node destructor 0x006FD930 unlinks a recorded node that is still linked, AddNode
// 0x006E6480 unlinks a recorded, ownerless, still linked node before it pushes it again, and the holder teardown 0x006E4DE0
// forgets the holder's records before its list dies. Details in scene_budget.cpp.
//
// Hooks: framework/call_chain.h (site SceneDrain, layer SceneBudget; the Frame Profiler's "Scene pending nodes" counter is
// the outer layer) and framework/entry_chain.h (sites SceneNodeDtor, SceneAddNode, SceneHolderTeardown, layer SceneBudget).
// The drain runs on the render thread (the game's drain has no lock); the destructor hook may run on any thread.
#include <cstdint>
#include <string>

namespace SceneBudget {

// Checks the drain's code, hooks the CALL and the three node lifetime entries (feature on) / removes them (feature off).
// Any thread.
bool Start(std::string* error);
void Stop();
bool Running();

// Developer mode preferences (saved): nodes / ms per frame while the camera moves, and the longest a node may wait
void SetNodesPerFrame(int n);
void SetMsPerFrame(float ms);
void SetMaxDeferMs(int ms);

// For the Frame Profiler (its layer is outside this one): what the last drain of the calling thread left for later
// (read and cleared). seen = this layer handled the call; budgeted = the copy with a budget ran; left = nodes still queued.
struct DrainNote {
    bool seen = false;
    bool budgeted = false;
    uint32_t left = 0;
};
DrainNote TakeDrainNote();

struct Stats {
    uint64_t calls = 0;          // drains from Scene::BeginFrame while on
    uint64_t fullStill = 0;      // the game's own drain: camera still (or nothing was waiting)
    uint64_t fullForced = 0;     // the game's own drain: a node waited kHardFactor x maxDeferMs
    uint64_t grown = 0;          // budgeted frames whose budget grew (a node waited past maxDeferMs)
    uint64_t budgeted = 0;       // the budgeted copy ran (camera moving)
    uint64_t framesLeft = 0;     // budgeted frames that left nodes for later
    uint64_t nodesBudgeted = 0;  // nodes processed by the budgeted copy
    uint64_t nodesLeft = 0;      // sum over budgeted frames of the nodes left
    uint64_t drawState = 0;      // nodes updated past the budget because their draw needs it (Swarm lists, fur LOD)
    uint32_t maxLeft = 0;        // largest backlog seen
    uint32_t lastDone = 0, lastLeft = 0;
    float lastMs = 0.0f;         // time of the last budgeted drain
    // node lifetime guard (all but `tracked` are expected to stay 0: each one is a case the game's code was read not to have)
    uint32_t tracked = 0;           // nodes left queued by the last budgeted drains that are still recorded
    uint64_t dtorUnlinked = 0;      // recorded nodes destroyed while still linked: unlinked by the destructor hook
    uint64_t addUnlinked = 0;       // recorded nodes added again while ownerless and still linked: unlinked before AddNode
    uint64_t teardownDropped = 0;   // records forgotten because their holder was torn down (expected: > 0 after world changes)
    uint64_t otherThread = 0;       // destructor-hook hits on a recorded node from a thread other than the drain's
    uint64_t foreignOwner = 0;      // nodes left queued whose owner (+0x30) was not the holder that drained them
    uint64_t repaired = 0;          // (development build) recorded nodes found destroyed but still linked, unlinked before a drain
    bool stopped = false;           // a safety check failed: the game's own drain runs until the game restarts (see the log)
};
Stats GetStats();
std::string StatusText();
// Development build: status lines and the tuning sliders
void RenderDeveloperUI();

} // namespace SceneBudget
