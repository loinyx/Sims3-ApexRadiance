#pragma once
// Where Sims and their hair are in this frame's picture, for other features than Ambient Occlusion. Credits: @loinyx
// Ambient Occlusion's Sim controls replay each recognised Sim draw (916 pixel-shader fingerprints, 48 of them hair) into
// two G32R32F targets (patches/ambient_occlusion_patch.cpp): the nearest body depth (r) and hair depth (g) of opaque
// draws, and the signed depth (negative: hair) and coverage of blended ones. A pixel is a Sim's hair where that depth is
// the scene's own (the shared INTZ depth). Made while Ambient Occlusion is on, and here only while something wants it.
#include <d3d9.h>

namespace SimMask {

// Render thread. Looks asks for the mask while one of its shaders leaves Sims' hair as it is.
void Want(bool on);

struct View
{
    IDirect3DTexture9* opaque = nullptr;      // r: body depth, g: hair depth (1 = none); null: not this frame
    IDirect3DTexture9* transparent = nullptr; // r: signed depth (< 0: hair), g: coverage; null: not this frame
    UINT width = 0, height = 0;
};
// This frame's mask (render thread, after the scene). A target no Sim drew into this frame is left out.
View Current();
// Why it cannot be made (English, for the menu), or nullptr
const char* Unavailable();

} // namespace SimMask
