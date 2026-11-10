#pragma once
// The no-scene frame trace (developer diagnostic; docs/features/rtx-remix-bridge.md). Armed by the recorder while a
// recording runs (development build, or any build under the RTX Remix bridge). For the frames whose back buffer gets fewer
// than 4 depth-tested draws (a loading screen, the save screen), it writes to Recording.txt what reached the device for
// each back-buffer draw, Clear, StretchRect and ColorFill: render target 0, viewport, scissor, the FVF or vertex
// declaration set last (with its position element), the shaders, the area the vertices of an UP draw cover; the Apex
// passes that saved the vertex input and viewport to restore them later (STALE / STALE vp when the device answered
// something other than what was set last); and how many calls came from another thread or device. The values are the
// ones SET (D3D9Hooks::Trace), never the device's Get* answers. It changes no call. Render thread.
#include <d3d9.h>
#include <string>

namespace NoSceneTrace {
void Begin();      // a recording started
std::string End(); // it stopped: the section for Recording.txt ("" when the trace was not armed)
// An Apex pass read the vertex input (GetVertexDeclaration / GetFVF) and viewport to put them back when it ends; site =
// its restore line. Call it before anything is set, right after those reads.
void Saved(const char* site, IDirect3DVertexDeclaration9* decl, DWORD fvf);
} // namespace NoSceneTrace
