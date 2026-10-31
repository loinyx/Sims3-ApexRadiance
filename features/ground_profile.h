#pragma once
// Ground light profile (part of the Light capture, light_probe.cpp). For the lot lamp nearest the ground point under the
// mouse, a line of points from 4 m behind the lamp to 12 m past it, through the mouse point (so across the lot edge when
// the mouse is beyond it). At each point: the lot it is on, the colour on screen, the game's terrain map and the smoothed
// copy the ground draws read, the dragged lamps stamped there, the lot's own story-0 floor map (what lot floors and grass
// read), and what each nearby lamp should give there by the terrain bake's law (with its cones) and by the room solve's
// law (once; the ground story's room 0 lists its own lamps twice). Then the nearby lamps' states (bake, exclusion, stamps)
// and the ground gains. Written into the capture's text and as "Ground profile.csv". Render thread, at the capture.
#include <windows.h>
#include <d3d9.h>
#include <filesystem>
#include <iosfwd>
#include <string>

namespace GroundProfile {
void Write(IDirect3DDevice9* dev, POINT pixel, unsigned bbWidth, unsigned bbHeight, const std::filesystem::path& folder, std::ostream& out);

// During a recording (ScreenWatch reads their screen pixels with its own): kWatchPoints ground points at t = -4 .. +12 m
// along the line from the lot lamp nearest the mouse's ground point (when the recording starts; a lamp being dragged first)
// to that point, following the lamp as it moves; each frame also the lamp's place, the versions of the chunk under it and
// its stamp state
// ("Ground timeline.csv"). Render thread.
constexpr int kWatchPoints = 17;
inline float WatchT(int i) { return -4.0f + static_cast<float>(i); }
void WatchBegin(POINT mouse, int bbWidth, int bbHeight);
bool WatchPoints(int x[kWatchPoints], int y[kWatchPoints]); // this frame's pixels (false: no lamp or camera yet)
void WatchEnd();
std::string WatchCsv(unsigned long startTick);
}
