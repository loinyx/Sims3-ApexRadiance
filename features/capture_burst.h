#pragma once
// The frames after a recording, developer mode only. Once Recording.txt is written, a few frames show what one picture cannot,
// in this order, into the recording's folder (Burst.txt says what came out of each step):
//  - Screenshot.png, the frame as it is shown, before anything else (Color's auto exposure adapts between frames);
//  - AO alone.png, the shade Ambient Occlusion would lay over the scene;
//  - Classes.png, every depth-tested surface in a flat colour by what draws it (frame_classes.h), with "reads the cube" among them;
//  - Light capture mouse\ and Light capture cube\, the light capture of the pixel under the mouse and of a pixel that reads the cube;
//  - Lighting snapshot.txt;
//  - Maps\, the light maps the classes frame bound, up to 2048 per side (larger ones are logged and skipped);
//  - White albedo.png, last: an experiment, the world's colour textures replaced by white, so light and shadow stand alone.
// Each view is one frame, read before the Apex menu and notices are drawn, so it is shown for a moment. The views are consecutive
// frames, not the same one: Sims move between them, and Burst.txt says so when the camera moved. A view whose frame had a light
// probe's cube written is dropped and taken again (see LotLightBridge::SetFrameView). The burst ends early when the world is left
// or the device is reset, and after 30 s, with what it has.
#include <d3d9.h>
#include <filesystem>
#include <string>

namespace CaptureBurst {
#ifdef APEX_NO_DEV_TOOLS
inline bool Begin(const std::filesystem::path&, const std::string&) { return false; }
inline bool Running() { return false; }
inline bool ViewFrame() { return false; }
inline void OnPresent(IDirect3DDevice9*) {}
inline bool ShadeAlone() { return false; }
inline void ShadeDrawn() {}
#else
// Render thread: the recording's text is saved. True: the burst runs and calls Captures::Finish(folder, what) when it ends
bool Begin(const std::filesystem::path& folder, const std::string& what);
bool Running();
bool ViewFrame();                      // render thread: this frame is a view, not the scene (what adapts between frames must not read it)
void OnPresent(IDirect3DDevice9* dev); // render thread, every Present
bool ShadeAlone();                     // this frame Ambient Occlusion shows its shade alone
void ShadeDrawn();                     // it did
#endif
} // namespace CaptureBurst
