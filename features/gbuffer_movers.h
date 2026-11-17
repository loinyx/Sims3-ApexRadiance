#pragma once
// Motion vectors of moving objects and Sims (G-buffer work package GB-05, research 14 WP8 + WP13 S2; developer builds).
// For every opaque scene draw that wrote the G-buffer, the constants its position reads (ShaderPatches::AnalyzeMotionSlice)
// are kept per draw key; when they changed since the same draw last frame (other than the camera's rows) the draw is
// replayed at once with a replay vertex shader (ShaderPatches::MakeMotionReplayVs: this frame's and last frame's values)
// and an Apex pixel shader into an A32B32G32R32F target: (current NDC - previous NDC, device depth, 1). The camera motion
// pass (patches/gbuffer_patch.cpp) takes that vector where the depth still matches (the mover is the visible surface) and
// flags the pixel as an exact mover (class 1 for the temporal consumers). Render thread.
#include <d3d9.h>
#include <cstdint>
#include <functional>

namespace GBufferMovers {
struct Counts {
    unsigned draws = 0;      // opaque G-buffer scene draws seen
    unsigned noSlice = 0;    // their vertex shader has no replay (fixed function, refused)
    unsigned firstSeen = 0;  // no draw with that key last frame
    unsigned still = 0;      // the same position constants (or only the camera's rows changed)
    unsigned replayed = 0;   // movers drawn into the target
    unsigned noRoom = 0;     // bank B does not fit beside this draw's arrays
    unsigned capped = 0;     // over the per-frame replay cap
};
#ifndef APEX_NO_DEV_TOOLS
void Acquire();                       // the upload tracking hook and the replay
void Release(IDirect3DDevice9* dev);  // dev may be null
bool Active();
// Key of a draw: vertex shader, stream 0, indices and the call's arguments
uint64_t DrawKey(IDirect3DDevice9* dev, IDirect3DVertexShader9* vs, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e);
// After an opaque scene draw of a G-buffer copy (the game's vertex shader gameVs; the scene draw moved by jitter[2] in
// clip units). True: the draw is a mover and Replay must follow (the binder unbinds its extra targets around it)
bool Prepare(IDirect3DDevice9* dev, IDirect3DVertexShader9* gameVs, uint64_t key);
void Replay(IDirect3DDevice9* dev, const float jitter[2], const std::function<void()>& draw);
void FrameBoundary();                 // Present: this frame's draws become last frame's
// This frame's mover target (null when no mover was drawn); the depth tolerance of its match
IDirect3DTexture9* Target();
inline constexpr float kDepthMatch = 1e-6f; // |target depth - scene depth| at most this: the mover is the visible surface
Counts TakeCounts();
void OnPreReset();
#else
inline bool Active() { return false; }
inline IDirect3DTexture9* Target() { return nullptr; }
#endif
} // namespace GBufferMovers
