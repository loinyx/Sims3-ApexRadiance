#pragma once
// Smooth light maps, part 2 (2.12.4, option "mapasDosComodosSuaves"; docs/features/night-lighting/room-map-smoothing.md):
// a fine dither on the room light maps' 8-bit texels. The batch solve of wall and floor samples (FUN_006a31d0, Steam 1.67.2)
// packs each texel's colour, clamped to 1, with 0x0060CD10 (x 255, rounded to nearest by the 1.5 x 2^23 add, packed as
// A8R8G8B8) and stores it at the texel (`mov [edi],ecx` at 0x006A34EE). Dim lamp light then steps by a whole level every
// few texels: on a lamp's slow falloff those steps are rings and bands. The CALL at 0x006A34DB goes through Apex, which adds
// up to half a level (interleaved gradient noise of the texel's position in its map, the same for r, g and b; alpha kept)
// before the game's own pack: the average light is the same, the steps become a fine grain the bilinear and B-spline reads
// smooth out. The offset depends only on the texel's position, so a room lit again with the same light gets the same
// texels (no shimmer).
#include <cmath>
#include <string>

namespace RoomMapDither {

// Steam 1.67.2 only: checks the pack call and the solve's code around it, then redirects the call (any thread)
bool Start(std::string& error);
void Stop();
bool Running();
// The menu switch (part of Smooth light maps); the texels change at the rooms' next solve
void SetEnabled(bool on);
bool Enabled();
std::string Status();

// A roofed room's texel keeps an ambient weight (alpha) of at least this (the pack's thunk, room_map_dither.cpp): the smoothed
// read's "beyond a wall" mark is alpha < 4/255
constexpr float kRoofedAlphaMin = 5.0f / 255.0f;

// The offset (in 8-bit levels, -0.5 .. 0.5) at a texel of a map: interleaved gradient noise (Jimenez 2014), a blue-noise-like
// pattern from the position alone
inline float Offset(unsigned x, unsigned y) {
    const double f = std::fmod(0.06711056 * static_cast<double>(x) + 0.00583715 * static_cast<double>(y), 1.0);
    const double g = std::fmod(52.9829189 * f, 1.0);
    return static_cast<float>(g - 0.5);
}

} // namespace RoomMapDither
