#pragma once
// Layered hooks on one CALL instruction (E8 rel32) of the game that several Apex modules wrap (the call-site twin of
// entry_chain.h and slot_chain.h).
//
// The CALL's rel32 points to the outermost installed layer's hook; each layer calls Next(site, layer): the next inner
// installed layer's hook, or the game function the CALL reached originally. Layers have fixed positions (lower = outer),
// whatever order the modules install in:
//   - Install: the new layer's next pointer first; then either the CALL is rewritten to it (it becomes the outermost) or
//     the next outer layer's next pointer is re-pointed to it (one atomic store).
//   - Remove: the reverse; when the last layer goes, the original CALL bytes are written back.
//   - The 5 bytes are written only with every other thread suspended and none of them stopped inside them
//     (MemPatch::WriteCodeSuspended). A removed hook keeps its next pointer, so a thread still inside it finishes normally;
//     hooks stay in memory for the process lifetime.
//   - Before the first write the CALL must reach the game function the address table gives; later, the hook Apex wrote.
//     Anything else (another module redirected it) makes Install fail and is never overwritten by Remove.
// Used by the Frame Profiler (dev build, the outer layer, "Scene pending nodes" counter) and the scene node budget
// (features/scene_budget.h) on Scene::BeginFrame's CALL of the pending-node drain, and by the compositor readback part of
// Faster Sim building (features/compositor_readback.h) on four calls of the texture builder, and by the shared lot build
// time (features/lot_build_share.h, outer) and the Frame Profiler's "Lot load stages" on the lot renderer's load stages.
// Thread-safe (Install / Remove serialise on one mutex; Next is lock-free).
#include <atomic>
#include <cstdint>
#include <string>

namespace CallChain {

enum class Site : int {
    SceneDrain, // CALL 0x006EBC49 in Scene::BeginFrame 0x006EBB70 -> 0x006E4130 thiscall() (the pending-node drain)
    CompQueue,    // CALL 0x006082F8 in the texture compositor queue loop 0x00608270 -> 0x005FDEF0 thiscall(job), ret 4
    CompState2,   // CALL 0x005FDFEC in 0x005FDEF0 -> 0x005FDDE0 thiscall(job), ret 4 (render the tile, read it back)
    CompState3,   // CALL 0x005FDFFF in 0x005FDEF0 -> 0x005FD420 thiscall(job), ret 4 (use the locked tile)
    CompReadback, // CALL 0x005FDC8A in 0x005FDBF0 -> 0x005FBCE0 thiscall(format), ret 4 (GetRenderTargetData)
    LotLoadStages, // CALL 0x00AEB306 in the lot renderer update 0x00AEB2E0 -> 0x00AEA680 thiscall() -> al (a lot's load stages)
    LotPassRenderer, // CALL 0x00C7CEDF in the lot pass 0x00C7CEA0 -> 0x00AEB2E0 thiscall(float dt), ret 4 (the renderer update)
    Count
};
enum class Layer : int { LotBuildGate, FrameProfiler, SceneBudget, CompositorReadback, Count }; // lower = outer

// Installs `hook` as `layer` of `site` (true when installed, or already installed). error: why not.
bool Install(Site site, Layer layer, void* hook, std::string* error);
// Removes the layer (nothing when it is not installed). False only when the original bytes could not be written back.
bool Remove(Site site, Layer layer);
bool Installed(Site site, Layer layer);
// The CALL instruction and the game function it reaches (0 until the first Install resolved them)
uintptr_t CallAddress(Site site);
uintptr_t GameFunction(Site site);

// What `layer`'s hook must call: never null while the layer is installed (and afterwards, for threads still inside it).
extern std::atomic<void*> g_next[static_cast<int>(Site::Count)][static_cast<int>(Layer::Count)];
inline void* Next(Site site, Layer layer) { return g_next[static_cast<int>(site)][static_cast<int>(layer)].load(std::memory_order_acquire); }

} // namespace CallChain
