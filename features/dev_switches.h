#pragma once
// Switches of the numbered development builds (APEX_DEVELOPMENT_BUILD_FLAVOR), read once at start-up from the
// [dev_build] table of ApexRadiance.toml (kept by Save, which preserves tables it does not write). Every switch defaults
// to on, so a development build can turn each one off to compare. Released builds (2.14 on) have every switch that changes
// the game on (kShipped) and the diagnostics off (measurements, daylight_log, memory_owners), with foliage_sun_shadow (unfinished) off.
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
//   foliage_sun_shadow    the sun and moon shadow on trees (the game's tree shaders without one read the shadow map,
//                         FoliageVertexLamps::PatchTreeSunShadow)
//   auto_light_priority   a lamp switched on automatically (dusk, dawn, a light toggled often) puts its own room first in the
//                         room queue, with no extra budget and no hold (LevelLightShare::NoteQuietMark)
//   lamp_visibility_memo  the wall / slab test of a lamp and a point is kept while nothing it reads changed (same values)
//   daylight_log          log only: the sky state that sets how dark it is outdoors (sun height, weather blend, the night and
//                         manmade levels) every 5 s, and a section in the lighting snapshot (daylight_probe.h); no lighting change
//   indoor_lamp_walls     a lamp another story takes through a floor opening is also stopped by its own story's walls (real heights,
//                         doors, windows) between it and the opening; a veto only (LevelLightShare IndoorPassImpl)
//   arch_ambient          near an arch into another roofed room, a floor takes the part of that room's ambient over its own (in the
//                         floor shader, from the ambients drawn), fading over 1.5 m (arch_ambient.h); never darker
//   basis_wall_filter     the directional maps (1 texel per metre) are read without blending a texel across a wall into a surface against
//                         it (a stair top, an object at a wall): a per-story wall mask and a moved uv (ShaderPatches::PatchBasisSmooth)
//   hard_wall_tops        a ray leaving a tile under a ceiling crosses its full walls with a hard top (no soft rim over a top the slab
//                         stands on), in the grid walk of light through openings (LevelLightShare GridPassImpl)
//   stair_height_light    a lamp of the story above lights the directional maps of the tiles under the floor opening from 1.5 m higher
//                         (the stair's steps go up to that floor), not only 0.5 m over the floor (LevelLightShare BasisLightHook)
//   steady_indoor_light   indoor light through openings and between stories no longer tests the opaque instanced meshes (stairs,
//                         railings) collected from the screen: that set follows the camera (a triangle budget by on-screen
//                         coverage) and its test is hard, so the light changed with the view and every publish relit the lots
//   inner_norm_settle     a lamp taken through an opening of the same story is scaled by its room's normalisation over the taking
//                         room's; the taking room's is only known at the end of its solve (it comes from the brightest sample), so
//                         the one its previous solve ended with is used, and a solve that ends with another sends the room once more
//   soft_opening_edges    the cone of a lamp through an arch or door into the next room has its edge spread over 0.5 m at the floor
//                         (as bounce light would), not a one-texel step (LevelLightShare kInnerSoftSpread, InnerPass)
//   stair_room_fill       the fill of stairs and objects lit by the directional maps is the ambient of the room under them x the
//                         floor map's alpha, as floors and walls take it, not the world's ambient cube (grey or blue on a green room)
//   opening_bounce        a room joined to another story by a stairwell opening (too small for one atrium) takes a share of the
//                         other room's ambient through it (opening_bounce.h): the lit room below brightens the landing above
//   stair_floor_tone      the stair copy (stair_room_fill) also takes the floors' tone curve, so a stair with the floor's texture under
//                         the floor's light has the floor's colour (about x1.46 in the dark; ShaderPatches::PatchBasisSmooth floorTone)
//   stair_story_blend     the stair copy blends its light by the pixel's height with the story above's directional maps and ambient
//                         (its top steps stand at that story's floor), from 0.5 m over its own floor to 0.5 m over the next
//   no_opening_band       the floor beside a stairwell opening no longer takes the room below's ambient (the dev.31 band): a lamp
//                         under that floor's plane cannot reach its top face, so the band lit the one strip that stays in shadow
//   stairwell_bounce      a wall of the story above a stairwell takes the light the lit surfaces below reflect up through the
//                         opening (stairwell_bounce.h): each opening patch shows the lower room's floor or wall its line of sight meets
//   borrowed_light_adapt  a room lit only by lamps of another story is brought up the way the eye adapts to a dark room
//                         (partly: the square root of what its brightest texel lacks, at most x2; StairwellBounce::BorrowedAdapt,
//                         LevelLightShare RoomNormHook)
//   soft_slab_edges       a lamp of another story lights a point through a floor opening with the share of its disk that the slab's
//                         edge leaves (radius 0.25 m): the edge of its light on the story below is a penumbra, not a line
//   room_fill             a room lit only through an opening takes back, on its floor and walls, the light its own lit surfaces
//                         reflect: albedo x the mean light its texels took in its last solve (stairwell_bounce.h RoomFill)
//   lot_build_share       the load stages of every streaming lot share one build budget per frame (lot_build_share.h); off = each
//                         lot keeps its own 20 / 35 ms slice (6 ms while the camera moves)
#include <atomic>

namespace DevSwitches {

#ifdef APEX_DEVELOPMENT_BUILD_FLAVOR
inline constexpr bool kCompiled = true;
#else
inline constexpr bool kCompiled = false;
#endif
// The value of a switch that changes the game in a build without the [dev_build] table (released builds): on
inline constexpr bool kShipped = true;

// Memory (2.14.0-dev.27; apex_memory.h):
//   memory_owners          Apex's own memory by owner: the C++ heap by calling site, the large containers of the lighting code,
//                          the Direct3D resources it creates; an [ApexMemory] log line every minute and a section in the lighting
//                          snapshot (F8). Measures only
//   release_debug_buffers  buffers of the developer tools are given back once they are no longer needed: the Light Probe's draw
//                          records and occlusion queries after a capture, a recording's lines and tracer states after it is saved,
//                          the F8 light records when their recording is switched off. Nothing that is lit reads them
//   left_lot_release       (2.14.0-dev.47) what only a lot that left the game's lot list can use is given back: Quick outdoor
//                          lamp edits' kept parts of rooms whose story manager no loaded lot holds (two 10 s sweeps in a row),
//                          Real-time outdoor lamps' CPU copy of the occluders once uploaded, and the lot mask texture of a lot
//                          not drawn for 2 minutes (made again from its kept masks at its next draw, the same texels)
//   room_solve_causes      (measures only) who sends rooms to the solve: every invalidate by its caller, every finished solve by
//                          class and by room, camera still or moving; a [RoomSolveCauses] log line every minute
//                          (room_solve_causes.h)
inline std::atomic<bool> memoryOwners{false};
inline std::atomic<bool> releaseDebugBuffers{false};
inline std::atomic<bool> leftLotRelease{false};
inline bool MemoryOwners() { return kCompiled && memoryOwners.load(std::memory_order_relaxed); }
inline std::atomic<bool> roomSolveCauses{false};
inline bool RoomSolveCauses() { return kCompiled && roomSolveCauses.load(std::memory_order_relaxed); }
//   restart_settle         (not shipped; off unless the key says true: in game a room's restarts came seconds apart and the
//                          hold did not reduce its solves) a room restarted again within 2 s is picked for its solve once its
//                          restarts stop for 300 ms (at most 1 s of hold); a lamp edit's rooms never wait (restart_settle.h)
inline std::atomic<bool> restartSettle{false};
//   opening_skip_check     (measures only) the rooms near openings sent again whose finished solve saw the inputs they have now,
//                          counted and checked by room_solve_causes' hash of their next solve (level_light_share.cpp, OpeningInputs)
//   opening_skip           (not shipped; off unless the key says true) those rooms are not sent again
inline std::atomic<bool> openingSkipCheck{false}, openingSkip{false};
inline bool OpeningSkipCheck() { return kCompiled && openingSkipCheck.load(std::memory_order_relaxed); }
inline bool OpeningSkip() { return kCompiled && openingSkip.load(std::memory_order_relaxed); }
inline bool RestartSettle() { return kCompiled && restartSettle.load(std::memory_order_relaxed); }
inline bool ReleaseDebugBuffers() { return kCompiled ? releaseDebugBuffers.load(std::memory_order_relaxed) : kShipped; }
inline bool LeftLotRelease() { return kCompiled ? leftLotRelease.load(std::memory_order_relaxed) : kShipped; }

inline std::atomic<bool> measurements{false};
inline std::atomic<bool> drawReadsNoWait{false};
inline std::atomic<bool> occlusionRelight{false};
inline std::atomic<bool> roomDrainBudget{false};
inline std::atomic<bool> allocatorTrim{false};
inline std::atomic<bool> allocatorTopDown{false};
inline std::atomic<bool> foliageVertexLamps{false};
inline std::atomic<bool> canopyGroundShadow{false};
inline std::atomic<bool> foliageSunShadow{false};
inline std::atomic<bool> autoLightPriority{false};
inline std::atomic<bool> lampVisibilityMemo{false};
inline std::atomic<bool> daylightLog{false};
inline std::atomic<bool> indoorLampWalls{false};
inline std::atomic<bool> archAmbient{false};
inline std::atomic<bool> basisWallFilter{false};
inline std::atomic<bool> hardWallTops{false};
inline std::atomic<bool> stairHeightLight{false};
inline std::atomic<bool> steadyIndoorLight{false};
inline std::atomic<bool> innerNormSettle{false};
inline std::atomic<bool> softOpeningEdges{false};
inline std::atomic<bool> stairRoomFill{false};
inline std::atomic<bool> openingBounce{false};
inline std::atomic<bool> stairFloorTone{false};
inline std::atomic<bool> stairStoryBlend{false};
inline std::atomic<bool> noOpeningBand{false};
inline std::atomic<bool> stairwellBounce{false};
inline std::atomic<bool> borrowedLightAdapt{false};
inline std::atomic<bool> softSlabEdges{false};
inline std::atomic<bool> roomFill{false};
inline std::atomic<bool> lotBuildShare{false};

void Load(); // init thread, before the features start; logs the values

inline bool Measurements() { return kCompiled && measurements.load(std::memory_order_relaxed); }
inline bool DrawReadsNoWait() { return kCompiled ? drawReadsNoWait.load(std::memory_order_relaxed) : kShipped; }
inline bool OcclusionRelight() { return kCompiled ? occlusionRelight.load(std::memory_order_relaxed) : kShipped; }
inline bool RoomDrainBudget() { return kCompiled ? roomDrainBudget.load(std::memory_order_relaxed) : kShipped; }
inline bool AllocatorTrim() { return kCompiled ? allocatorTrim.load(std::memory_order_relaxed) : kShipped; }
inline bool AllocatorTopDown() { return kCompiled ? allocatorTopDown.load(std::memory_order_relaxed) : kShipped; }
inline bool FoliageVertexLamps() { return kCompiled ? foliageVertexLamps.load(std::memory_order_relaxed) : kShipped; }
inline bool CanopyGroundShadow() { return kCompiled ? canopyGroundShadow.load(std::memory_order_relaxed) : kShipped; }
inline bool FoliageSunShadow() { return kCompiled && foliageSunShadow.load(std::memory_order_relaxed); }
inline bool AutoLightPriority() { return kCompiled ? autoLightPriority.load(std::memory_order_relaxed) : kShipped; }
inline bool LampVisibilityMemo() { return kCompiled ? lampVisibilityMemo.load(std::memory_order_relaxed) : kShipped; }
inline bool DaylightLog() { return kCompiled && daylightLog.load(std::memory_order_relaxed); }
inline bool IndoorLampWalls() { return kCompiled ? indoorLampWalls.load(std::memory_order_relaxed) : kShipped; }
inline bool ArchAmbient() { return kCompiled ? archAmbient.load(std::memory_order_relaxed) : kShipped; }
inline bool BasisWallFilter() { return kCompiled ? basisWallFilter.load(std::memory_order_relaxed) : kShipped; }
inline bool HardWallTops() { return kCompiled ? hardWallTops.load(std::memory_order_relaxed) : kShipped; }
inline bool StairHeightLight() { return kCompiled ? stairHeightLight.load(std::memory_order_relaxed) : kShipped; }
inline bool SteadyIndoorLight() { return kCompiled ? steadyIndoorLight.load(std::memory_order_relaxed) : kShipped; }
inline bool InnerNormSettle() { return kCompiled ? innerNormSettle.load(std::memory_order_relaxed) : kShipped; }
inline bool SoftOpeningEdges() { return kCompiled ? softOpeningEdges.load(std::memory_order_relaxed) : kShipped; }
inline bool StairRoomFill() { return kCompiled ? stairRoomFill.load(std::memory_order_relaxed) : kShipped; }
inline bool OpeningBounce() { return kCompiled ? openingBounce.load(std::memory_order_relaxed) : kShipped; }
inline bool StairFloorTone() { return kCompiled ? stairFloorTone.load(std::memory_order_relaxed) : kShipped; }
inline bool StairStoryBlend() { return kCompiled ? stairStoryBlend.load(std::memory_order_relaxed) : kShipped; }
inline bool NoOpeningBand() { return kCompiled ? noOpeningBand.load(std::memory_order_relaxed) : kShipped; }
inline bool StairwellBounce() { return kCompiled ? stairwellBounce.load(std::memory_order_relaxed) : kShipped; }
inline bool BorrowedLightAdapt() { return kCompiled ? borrowedLightAdapt.load(std::memory_order_relaxed) : kShipped; }
inline bool SoftSlabEdges() { return kCompiled ? softSlabEdges.load(std::memory_order_relaxed) : kShipped; }
inline bool RoomFill() { return kCompiled ? roomFill.load(std::memory_order_relaxed) : kShipped; }
inline bool LotBuildShare() { return kCompiled ? lotBuildShare.load(std::memory_order_relaxed) : kShipped; }

} // namespace DevSwitches
