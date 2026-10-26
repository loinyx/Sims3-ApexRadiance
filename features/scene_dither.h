#pragma once
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
} // namespace SceneBinder
