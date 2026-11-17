#pragma once
#include <d3d9.h>
// Banding Fix (scene_dither.cpp): the grain in the scene's pixel shaders.
namespace SceneDither {
bool On();         // the Banding Fix is on
float Strength();  // its grain in 8-bit steps (1 = +-1 step); the AO composite uses the same
float GrainPhase(); // the phase of its grain pattern: 0, a fixed grain (Moving grain, which changed it every frame, is removed)
}

// The scene-draw binder shared with the temporal anti-aliasing (30/09): one final draw hook (after every other module)
// binds, for each 3D-scene draw (render target 0 = the back buffer, depth test on), the Banding Fix's pixel copy and / or
// a vertex copy moved by the frame's jitter (ShaderPatches::AddJitterVs, constant c252; composed with the screen-position
// copy for ps_2_x grain). Draws into other targets (shadows, reflections, the mouse pick, impostors) and the UI are never
// moved. Render thread except Acquire / Release.
namespace SceneBinder {
void AcquireJitter(); // temporal AA on: jittered vertex copies are made (at creation and at first draw)
void ReleaseJitter();
void SetFrameJitter(bool on, float clipX, float clipY); // this frame's offset in clip units (2 / width = one pixel)
struct Coverage {
    unsigned jittered = 0, refused = 0, noShader = 0; // last frame's scene draws: moved / vertex shader refused / fixed-function
};
Coverage LastCoverage();

// G-buffer (developer builds, patches/gbuffer_patch.cpp): while acquired, the ps_3_0 and ps_2_x copies also write oC1 / oC2
// (ShaderPatches::AddGBufferPs with the row of shaders/gbuffer_ids.h) and are bound with the High-precision scene on too
// (grain amount 0). With targets set, they are render targets 1 and 2 from the first scene draw into the back buffer
// until render target 0 changes, the frame ends or GBufferUnbind; cleared to 0 once a frame. COLORWRITEENABLE1 / 2 are
// 0xF only for an opaque draw of a copy with the outputs, 0 for every other draw seen while bound.
#ifndef APEX_NO_DEV_TOOLS
void AcquireGBuffer();
void ReleaseGBuffer(IDirect3DDevice9* dev); // dev may be null (nothing unbound then)
void SetGBufferTargets(IDirect3DDevice9* dev, IDirect3DSurface9* rt1, IDirect3DSurface9* rt2); // null = none; identities
void GBufferUnbind(IDirect3DDevice9* dev);  // before a pass that reads the targets or draws over the back buffer
void GBufferMaskOff(IDirect3DDevice9* dev); // before a draw of another module made while the scene is drawn
struct GBufferCounts { // since the last Take
    unsigned frames = 0;
    unsigned patched = 0, unknown = 0; // opaque scene draws that wrote: with the table's row / with the unknown values
    unsigned ps2 = 0, replaced = 0;    // of those: ps_2_x copies / replacements (Night Lighting) with their game shader's row
    unsigned blended = 0;              // draws of such a copy with blending on: not written
    unsigned masked = 0;               // other back-buffer draws while bound (refused, ps_2_x without a vertex pair, fixed function, UI)
    unsigned binds = 0, unbinds = 0, bindFailed = 0, maskToggles = 0;
};
GBufferCounts TakeGBufferCounts();
#else
inline void GBufferMaskOff(IDirect3DDevice9*) {}
#endif
} // namespace SceneBinder
