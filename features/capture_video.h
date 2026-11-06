#pragma once
// The video of a recording, developer mode only (ScreenWatch drives it, the recorder ends it). Every second frame, as the game
// made it before the Apex menu and notices, goes to at most 1080 lines and through a ring of 3 slots: the GPU scales it, it is
// read back a few frames later without waiting, and a worker encodes it. A frame with no free slot is dropped and counted,
// the render thread never waits. The JPEGs are written one after the other in Video.tmp, which becomes Video.mjpeg in the
// capture folder (tools/capture_pack.py makes a playable file of it). For timing and flicker: JPEG spoils dark gradients, so
// shadows are read in the lossless PNGs.
#include <d3d9.h>
#include <filesystem>
#include <string>

namespace CaptureVideo {
#ifdef APEX_NO_DEV_TOOLS
inline void Begin(IDirect3DDevice9*, const D3DSURFACE_DESC&, const std::filesystem::path&) {}
inline int Grab(IDirect3DDevice9*) { return -1; }
inline void Pump(IDirect3DDevice9*) {}
inline std::string End(const std::filesystem::path&) { return {}; }
inline void Release() {}
#else
// Render thread: the recording starts. bb = the back buffer's description; temp = the file written meanwhile (a leftover of a
// crash is replaced). Failing only turns the video off
void Begin(IDirect3DDevice9* dev, const D3DSURFACE_DESC& bb, const std::filesystem::path& temp);
// Render thread, once a frame with the frame finished: takes it when it is its turn. Its index in the video, or -1
int Grab(IDirect3DDevice9* dev);
void Pump(IDirect3DDevice9* dev); // render thread, every Present: moves the slots on, never waits
// Render thread: waits for the frames already read (at most 3), closes the file and moves it to `to` (empty: deletes it).
// One line on the video, "" when there was none
std::string End(const std::filesystem::path& to);
void Release(); // releases the device objects (device reset, or the recording's end): what was written stays
#endif
} // namespace CaptureVideo
