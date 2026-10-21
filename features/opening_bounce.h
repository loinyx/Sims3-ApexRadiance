#pragma once
// Bounce light through a floor opening (development builds, switch opening_bounce in [dev_build]; pure part,
// tools/opening_bounce_test). The game gives every indoor room one ambient colour (room+0x110, the bounce of its own lamps,
// drawn as map.a x c4 on its floors and walls). A room over a stairwell took nothing of the lit room below (a capture:
// the landing's c4 the unlit colour (0.0068 0.0068 0.0068), the room under the opening lit by a lamp), so its floor
// beside the opening was black while light went up the stairs. Rooms joined by a removed floor too small to make them one
// atrium (LevelLightShare MergeStackedAmbient: under 30% of the smaller room) now each take a share of the other's ambient:
// kShare x (the opening's quadrants / the room's own quadrants) x the other's colour brought to this room's normalisation
// (a colour carries its room's normalisation, RoomAmbientPolicy::AccumulateAmbient). The sum stays under the larger of the
// two colours per channel, and a room never gets darker. One step only: each room takes the others' own values (as the
// game computed them), never one with this share in it.
#include <algorithm>
#include <cmath>

namespace OpeningBounce {

constexpr float kShare = 0.5f; // of the light reaching the opening, the part spread back over the room (its walls and ceiling)

struct Neighbour {
    float c4[3] = {};
    float norm = 1.0f;
    int shared = 0; // quadrants of the opening between the two rooms
};

// own: this room's colour (rgb) and normalisation; area: its quadrants. out = own + the share. True when it changed.
inline bool Add(const float own[3], float ownNorm, int area, const Neighbour* nb, int n, float out[3]) {
    for (int c = 0; c < 3; c++) out[c] = own[c];
    if (area <= 0 || n <= 0 || !(ownNorm > 0.0f) || !std::isfinite(ownNorm)) return false;
    float add[3] = {}, cap[3] = {own[0], own[1], own[2]};
    for (int i = 0; i < n; i++) {
        if (nb[i].shared <= 0 || !(nb[i].norm > 0.0f) || !std::isfinite(nb[i].norm)) continue;
        const float f = std::min(1.0f, static_cast<float>(nb[i].shared) / static_cast<float>(area));
        const float scale = ownNorm / nb[i].norm;
        for (int c = 0; c < 3; c++) {
            const float other = nb[i].c4[c] * scale;
            if (!std::isfinite(other) || other <= 0.0f) continue;
            add[c] += kShare * f * other;
            cap[c] = std::max(cap[c], other);
        }
    }
    bool changed = false;
    for (int c = 0; c < 3; c++) {
        const float v = std::min(own[c] + add[c], cap[c]);
        if (v > own[c]) out[c] = v, changed = true;
    }
    return changed;
}

} // namespace OpeningBounce
