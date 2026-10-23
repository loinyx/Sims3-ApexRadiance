#pragma once
// One full-resolution copy of the back buffer, shared by the passes that copy the picture and then read that copy
// (Picture, Ambient Occlusion, Depth Blur, Edge Smoothing).
//
// Each of them copies the back buffer into it right before its own pass and reads it only inside that pass: the copy holds
// nothing from one pass to the next, so one texture serves all of them (it was one each: up to 4 full-resolution textures,
// about 33 MB each at 3840x2160). A pass that needs a copy kept for later in the frame (Picture's copy of the finished
// scene) keeps its own texture.
//
// Acquire() gives the shared texture to a holder (created on the first one); Drop() gives it back, and the texture is
// released with the last holder. Every holder drops it where it releases its other D3DPOOL_DEFAULT textures (before a
// device Reset, on uninstall), so nothing of it is left at a Reset. A holder that asks for another size or format while
// the shared one is held gets a texture of its own, as before (Drop releases that one).
#include <d3d9.h>

namespace SceneCopy {

// A render-target texture of that size and format and its surface (render thread); false when it cannot be created
bool Acquire(IDirect3DDevice9* dev, UINT width, UINT height, D3DFORMAT format, IDirect3DTexture9** texture, IDirect3DSurface9** surface);
// Gives back what Acquire gave and clears both pointers; nothing when they are null
void Drop(IDirect3DTexture9** texture, IDirect3DSurface9** surface);

} // namespace SceneCopy
