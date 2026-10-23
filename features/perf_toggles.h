#pragma once
// Live switches for the per-frame performance work, so a frame rate can be compared with and without each item (A/B).
// All default on; off restores the earlier per-frame path, with the same results. Read with relaxed loads from the
// render thread (draw and state callbacks) and the light tree thread (room solves); written by the developer menu.
// The dispatch structure itself (plain function-pointer callbacks, one exception frame per dispatch, the flags read once
// per dispatch, Frame Capture's callbacks registered only while it records) has no switch.
#include <atomic>

namespace PerfToggles {
// D3D9 registry: inside a re-issued draw (ReissueScope) only the callbacks that observe re-issued draws run (d3d9_hooks.cpp Run)
inline std::atomic<bool> ReissueFilter{true};
// D3D9 registry: BoundPixelShader / BoundVertexShader answer from the followed shaders, not a device read-back (d3d9_hooks.cpp)
inline std::atomic<bool> FollowBoundShaders{true};
// D3D9 registry, development build: one outermost dispatch in 16 is timed and scaled, not every one (d3d9_hooks.cpp ModTimeGuard)
inline std::atomic<bool> SampledDispatchTiming{true};
// Shader cache: PrecompileComplete keeps its answer once every job is done (shader_cache.cpp)
inline std::atomic<bool> PrecompileAnswerCache{true};
// Ambient Occlusion: the last bound pixel shader's Sim-receiver answer is kept (ambient_occlusion_patch.cpp)
inline std::atomic<bool> AoLastShaderCache{true};
// Banding Fix: the lock-free front caches of the shader copies are checked before the locked lookup (scene_dither.cpp)
inline std::atomic<bool> SceneDitherFrontCache{true};
// Night Lighting: one lookup per pixel shader change for its class and facts (lot_light_bridge.cpp TrackPs)
inline std::atomic<bool> NightLightingPsFacts{true};
// Night Lighting: the rig mode is read once per draw (lot_light_bridge.cpp RigMode)
inline std::atomic<bool> RigModeOncePerDraw{true};
// Night Lighting: indoor objects look for their room light map only in declared samplers, the last hit first (DrawIndoorObject)
inline std::atomic<bool> IndoorSamplerScan{true};
// Room solves: the room record lookup is kept while no record can have been erased or added (level_light_share.cpp SolveInfo)
inline std::atomic<bool> SolveInfoCache{true};
// (2.15.3) D3D9 registry: BoundTexture answers from the textures followed through SetTexture, not a device read-back
// (d3d9_hooks.cpp); checked against the device now and then, and no longer used for the session after repeated differences
inline std::atomic<bool> FollowBoundTextures{true};
// (2.15.3) Room solves: a window light's channel test is kept for the sample batch (level_light_share.cpp WindowChannelHook)
inline std::atomic<bool> WindowChannelMemo{true};
// (2.15.3) Lot lighting while the camera moves: the raised budgets right after a load and after a lamp switch are dosed by
// the frame time, as the settle budget with the camera still (lot_lighting_motion.cpp); off = the game's full budget while
// moving, as before. Only how fast those rooms relight while panning changes, not their light
inline std::atomic<bool> MotionPacedRaise{true};
// (2.15.3) Lot lighting with the camera still: the game's room solving budget (and Faster room lighting's drain) dosed by
// the frame time when frames run long (lot_lighting_motion.cpp SteadyPace); lamp edits, the settle window after a load and
// a lamp switch keep their own budgets. Off: the game's budget every frame, as before. Only how soon rooms finish changes
inline std::atomic<bool> SteadyRoomPacing{true};
// (2.15.3) File list cache: the writable packages whose writes are counted (DPF, DDF, packed stream; resource_cache.cpp
// write epochs) also answer from memory while their write epoch is the same (resource_cache.cpp EmulateKeyList). Off: only
// the read-only packages, as before
inline std::atomic<bool> KeyListCountedPackages{true};
} // namespace PerfToggles
