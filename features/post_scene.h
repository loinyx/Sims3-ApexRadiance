#pragma once
#include <string>
// Shared trigger for effects that work on the finished 3D scene, before the game draws any UI: normally the first
// backbuffer draw with ZENABLE = FALSE after at least 4 depth-tested draws. If hidden game UI means that boundary never
// draws, EndSceneBeforeOverlay is the fallback. Effects run once in fixed order (ambient occlusion, edge smoothing,
// then Depth Blur); each saves and restores what it touches.
// Effects draw with DrawPrimitiveUP, which is not hooked, so they never re-trigger it.
// Its draw hooks run at Priority::First; Picture's scene copy runs after them (picture.cpp) so it contains the effects.
// If a shared scene depth exists, an incompatible bound surface rejects the boundary without consuming the effects.
// Only resumed depth-tested scene draws unlock another boundary. EndScene never retries over a rejected UI boundary.
//
// A colour-tile scratch target learned from a UI-visible frame can also mark the boundary before the
// game transforms the hidden-UI picture. Unknown targets/layouts keep the EndScene fallback. Reset drops the identity.
//
// Existing limitation: interiors can have depth-off backbuffer draws in the middle of the scene, so the first such draw
// can precede the finished scene (Picture re-copies at every depth-on -> depth-off transition for that reason). The
// EndScene fallback only handles frames with no qualifying depth-off draw; it does not change this interior behavior.
#include <d3d9.h>

namespace PostScene {
using Effect = void (*)(IDirect3DDevice9*);
enum Order : int { kAmbientOcclusion = 10, kEdgeSmoothing = 20, kDepthBlur = 30, kPicture = 40 };
// registers the draw hooks with the first effect. needsDepth = false: an effect that also runs when the scene was drawn with
// another depth-stencil than the shared INTZ one (Picture: Color needs the depth only for Emphasize, see SceneDepthValid)
void Add(int order, Effect fn, bool needsDepth = true);
void Remove(Effect fn);         // and unregisters them with the last one
// At a scene boundary: the shared INTZ depth holds this scene (false while a needsDepth = false effect runs without it)
bool SceneDepthValid();
// The camera, read from the scene draws' vertex constants while an effect asks for it (reference counted)
void WantCamera(bool on);
// The camera's near plane of the current frame (metres). Device depth d = A - near * A / z (A = 1.00008 measured, far
// plane ~3 km); near changes with the camera's zoom and height (0.2 .. 0.3). 0 until a frame was seen.
float CameraNear();
// Depth-writing back buffer draws so far this frame (render thread): the world being drawn. A frozen screen (the save
// screen shows a still image of the world) has almost none.
int DepthWritesThisFrame();
// The same for the last complete frame (stable whatever the time of the frame it is read), and whether draws are counted
// at all (the draw hooks are registered while any post-scene effect is on)
int DepthWritesLastFrame();
bool Counting();
// Diagnostics (render thread): this frame's scene count, its state and the effects registered, as one log fragment
std::string DiagText();
// This frame's effects have not run yet but will at a later scene boundary (Picture waits for them before its scene copy)
bool EffectsPending();
// The camera's view-projection of the current frame (world -> clip, rows = c40..c43), false when not seen this frame
bool CameraViewProj(float vp[4][4]);
// A of the projection (d = A - near * A / z, so view z / near = A / (A - d)); 1.00008 when unknown
float CameraDepthA();
}
