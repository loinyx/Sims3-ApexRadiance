#pragma once
// High-precision scene (a switch on the Banding Fix card; on by default). The game draws its 3D scene into
// the 8-bit back buffer, so every draw and every blend rounds to 8 bits. While this is on, a 16-bit target
// (scene_target_logic.h) the size of the back buffer stands in for it: from the frame start until the scene is resolved,
// a SetRenderTarget(0, back buffer) binds the target (D3D9Hooks::SetRenderTargetSubstitute) and GetRenderTarget still
// reports the back buffer. The resolve draws the target into the back buffer once, with one triangular grain (the Banding
// Fix's Strength and phase), at the first of: the post-scene boundary (before Ambient Occlusion, Edge Smoothing, Depth Blur
// and Picture), a copy, fill or read-back touching the back buffer, the game's EndScene with the target bound, Present.
// Everything after it (the UI and its back-buffer copies, Apex's passes) works on the real 8-bit back buffer.
//
// Refused (status, nothing substituted) with the game's multisampled back buffer, a back buffer other than A8R8G8B8, or
// without 16-bit render-target blending. Its target is a D3DPOOL_DEFAULT texture, released before a Reset and made again
// at the next frame. Render thread except Setting / State reads.
#include <d3d9.h>

namespace SceneTarget {

void Init(); // registers its frame callbacks (framework, first EndScene); they return at once while the switch is off

bool* Setting();     // the switch (saved with the Banding Fix: [patches.SceneDither] cenaAltaPrecisao)
bool Substituting(); // this frame's scene is going into the 16-bit target (the Banding Fix's per-draw grain stays off)

enum class State { Off, Waiting, Active, Multisample, BackBufferFormat, NoSupport, NoMemory, NoHooks };
State CurrentState();
unsigned VideoMemoryMB(); // of the 16-bit target, 0 without one

// Developer page
struct Stats {
    unsigned atBoundary = 0, atCopy = 0, atEndScene = 0, atPresent = 0; // resolves by cause, this session
    float resolveMs = 0.0f;                                             // CPU time to issue one, smoothed
};
Stats ReadStats();

} // namespace SceneTarget
