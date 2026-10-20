#pragma once
// Switches of the numbered development builds (APEX_DEVELOPMENT_BUILD_FLAVOR), read once at start-up from the
// [dev_build] table of ApexRadiance.toml (kept by Save, which preserves tables it does not write). Every switch defaults
// to on; all of them off = the behaviour of the released build. Other builds compile every switch to off.
//   measurements        address-space owners, game allocation categories and the memory CSVs
//   draw_reads_nowait   buffer reads inside draw hooks never wait for the GPU (retried, then waited for after three busy
//                       answers); opaque instanced meshes read each vertex once
//   occlusion_relight   an occlusion scene identical to the last published one is not published again (no relight)
//   room_drain_budget   the extra room solves inside the game's room pick never start a step the budget cannot hold
//   allocator_trim      the game allocator's own trim every 60 s and before saves
//   allocator_top_down  the game allocator's large blocks from the top of the address space
//   foliage_vertex_lamps  lamp light on instanced foliage per vertex: distance, height, direction, leaf transmission, the crown's
//                         own leaves, ground bounce (foliage_vertex_lamps.h); a draw whose lamps cannot be matched keeps the old path
//   canopy_ground_shadow  tree and bush crowns as porous occluders of lamp light on lot ground (foliage_canopy.h)
//   auto_light_priority   a lamp switched on automatically (dusk, dawn, a light toggled often) puts its own room first in the
//                         room queue, with no extra budget and no hold (LevelLightShare::NoteQuietMark)
#include <atomic>

namespace DevSwitches {

#ifdef APEX_DEVELOPMENT_BUILD_FLAVOR
inline constexpr bool kCompiled = true;
#else
inline constexpr bool kCompiled = false;
#endif

inline std::atomic<bool> measurements{false};
inline std::atomic<bool> drawReadsNoWait{false};
inline std::atomic<bool> occlusionRelight{false};
inline std::atomic<bool> roomDrainBudget{false};
inline std::atomic<bool> allocatorTrim{false};
inline std::atomic<bool> allocatorTopDown{false};
inline std::atomic<bool> foliageVertexLamps{false};
inline std::atomic<bool> canopyGroundShadow{false};
inline std::atomic<bool> autoLightPriority{false};

void Load(); // init thread, before the features start; logs the values

inline bool Measurements() { return kCompiled && measurements.load(std::memory_order_relaxed); }
inline bool DrawReadsNoWait() { return kCompiled && drawReadsNoWait.load(std::memory_order_relaxed); }
inline bool OcclusionRelight() { return kCompiled && occlusionRelight.load(std::memory_order_relaxed); }
inline bool RoomDrainBudget() { return kCompiled && roomDrainBudget.load(std::memory_order_relaxed); }
inline bool AllocatorTrim() { return kCompiled && allocatorTrim.load(std::memory_order_relaxed); }
inline bool AllocatorTopDown() { return kCompiled && allocatorTopDown.load(std::memory_order_relaxed); }
inline bool FoliageVertexLamps() { return kCompiled && foliageVertexLamps.load(std::memory_order_relaxed); }
inline bool CanopyGroundShadow() { return kCompiled && canopyGroundShadow.load(std::memory_order_relaxed); }
inline bool AutoLightPriority() { return kCompiled && autoLightPriority.load(std::memory_order_relaxed); }

} // namespace DevSwitches
