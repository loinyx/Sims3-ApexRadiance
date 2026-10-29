#pragma once
// Screen pixels during a recording (measures what the player sees when the lights update). While a recording (F8) runs, 33 pixels are read from the back buffer at every frame, without stalling it
// (StretchRect into a small render target, read back a few frames later once an event query says the copy is done): a
// column of 9 around the mouse (the screen's centre when the mouse is over the Apex menu or outside the game), so pointing
// at the line between two floors measures both stories at once, and a grid of 6 x 4 over the whole screen. They are copied
// before the Apex menu and notices are drawn (the first recording measured the menu's own pixels). The
// recorder writes the samples ("Screen pixels.csv") and, for every lamp edit, when each point began to change, when it
// settled and whether it went the wrong way first (Recording.txt, "Light updates").
#include "ground_profile.h"
#include <d3d9.h>
#include <string>
#include <vector>

namespace ScreenWatch {
constexpr int kColumn = 9;            // around the mouse: points 0..8, the mouse's is kColumn / 2
constexpr int kGridX = 6, kGridY = 4; // over the screen: points kColumn.., row by row
constexpr int kGrid = kColumn + kGridX * kGridY; // then the ground points around a lamp (GroundProfile::WatchPoints), moving with it
constexpr int kPoints = kGrid + GroundProfile::kWatchPoints;
struct Sample {
    unsigned long tick; // GetTickCount of the frame the pixels were copied in
    unsigned char rgb[kPoints][3];
};
// Render thread, every Present: starts with a recording, samples, stops with it
void OnPresent(IDirect3DDevice9* dev);
// The points (back-buffer pixels) and the samples of the last recording (render thread)
std::vector<Sample> Samples();
int PointX(int i);
int PointY(int i);
int Width();
int Height();
bool ColumnAtCentre(); // the mouse was over the Apex menu or outside the game when the recording started
bool Watching();
// Releases the device objects (device lost / reset)
void Release();
}
