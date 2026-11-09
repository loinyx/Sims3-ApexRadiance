#pragma once
// Rooms open to the story above (double-height rooms), for room 0 of that story (level_light_share.cpp
// ShareRoomZeroFromBelow). A roofed room of story B - 1 with no floor ever placed over it on story B, and its walls built up
// on B, is one space two stories tall; on B that space is room 0 (no floor, no room), so the upper half of its walls takes
// room 0's light list, which never held the lamps below. A room that keeps its ceiling has the bare 0x40000000 key only along
// its walls and around the floor's edge (2026-09-29: counting that key as an opening gave 200 false openings instead of 40),
// so the rule is per room, not per quadrant. POD and templates only, no game reads: a read-only offline harness can test it
// (tools/outdoor_delta_test).
//
// Lot space as lamp_visibility.h: 1 m tiles, four quadrants each (0 toward -z, 1 +x, 2 +z, 3 -x).
#include <vector>

namespace OpenAbove {

constexpr int kMaxId = 4096; // room ids at or above it are never open (the per-id tallies stay small)

// roomAt(x, z, q): the roofed room of story B - 1 there (0 = room 0, a roofless room, outside the grid);
// placed(x, z, q): a floor a player placed on story B there (what cannot be read counts as placed);
// wallZ(x, Z): story B has a wall on the edge (x, Z)-(x + 1, Z), Z = 0..h; wallX(X, z): on (X, z)-(X, z + 1), X = 0..w.
// open: the ids open to above, ascending. A room is open when
//  - no floor is placed over at least half of its quadrants, and
//  - at least three quarters of its outline (the tile edges between its quadrants and another room's) carry a wall of
//    story B or lead into another room open by the floor rule: its walls go up through story B. A single-storey room
//    under its roof has no walls on B along its eaves, and the lighting side knows no roof: such a room stays closed, or
//    its lamps would light the walls of story B beside it above the roof line.
// ownWalls (the physical model's open tops): a room open by the floor rule is also open when story B has a wall of its own
// on its outline, one with no floor placed on B on either side of it (a gable or the upper half of a wall going up through
// B; a wall with a floor on its own side stands on the room's single-storey part, its outer face above that roof). A
// double-height room under a sloped roof has those only along its gables (02/11, lot C49C: story 2's rooms 5 and 7 under
// story 3); a single-storey wing has walls of B only where a floored room of B stands beside it (story 1's room 8: none of
// its own, 5 beside floored rooms): it stays closed. Diagonal walls of B count (wallD): a room whose outline runs along
// diagonals (C49C's room 6, a tall room) has its walls there. shortcut: the roof's height is not read, so a room whose upper walls
// all stand on its floored part stays closed even if it is two storeys tall; read the roof to open it.
// wallD(x, z, i): story B has the diagonal wall i of tile (x, z) (0: (x, z)-(x + 1, z + 1), 1: (x, z + 1)-(x + 1, z)): the edges
// between a tile's own quadrants across it count as the outline's, as the tile sides do (13-50-15: lot C49C's room 6, a tall
// room whose outline runs along diagonals, stayed closed with only the tile sides counted, its upper walls grey)
template <class RoomAt, class Placed, class WallZ, class WallX, class WallD>
void Classify(int w, int h, RoomAt roomAt, Placed placed, WallZ wallZ, WallX wallX, WallD wallD, std::vector<int>& open, bool ownWalls = false) {
    open.clear();
    if (w <= 0 || h <= 0) return;
    const auto room = [&](int x, int z, int q) {
        if (x < 0 || z < 0 || x >= w || z >= h) return 0;
        const int r = roomAt(x, z, q);
        return r > 0 && r < kMaxId ? r : 0;
    };
    int top = 0;
    for (int z = 0; z < h; z++)
        for (int x = 0; x < w; x++)
            for (int q = 0; q < 4; q++)
                if (const int r = room(x, z, q); r > top) top = r;
    if (!top) return;
    std::vector<int> quads(static_cast<size_t>(top) + 1, 0), bare(quads), edges(quads), walled(quads), own(quads);
    for (int z = 0; z < h; z++)
        for (int x = 0; x < w; x++)
            for (int q = 0; q < 4; q++)
                if (const int r = room(x, z, q)) {
                    quads[static_cast<size_t>(r)]++;
                    if (!placed(x, z, q)) bare[static_cast<size_t>(r)]++;
                }
    const auto floorOpen = [&](int r) { return r > 0 && bare[static_cast<size_t>(r)] * 2 >= quads[static_cast<size_t>(r)]; };
    // the two quadrants facing each other across one tile edge (aFloor / bFloor: a floor placed on B over a's / b's quadrant)
    const auto edge = [&](int a, int b, bool wall, bool aFloor, bool bFloor) {
        if (a == b) return;
        for (const int side : {a, b}) {
            if (!floorOpen(side)) continue;
            const int other = side == a ? b : a;
            edges[static_cast<size_t>(side)]++;
            if (wall || floorOpen(other)) walled[static_cast<size_t>(side)]++;
            if (wall && !aFloor && !bFloor) own[static_cast<size_t>(side)]++; // (over the open part: no floor on either side)
        }
    };
    // (outside the grid: no floor)
    const auto floorAt = [&](int x, int z, int q) { return x >= 0 && z >= 0 && x < w && z < h && placed(x, z, q); };
    for (int Z = 0; Z <= h; Z++)
        for (int x = 0; x < w; x++) edge(room(x, Z - 1, 2), room(x, Z, 0), wallZ(x, Z), floorAt(x, Z - 1, 2), floorAt(x, Z, 0));
    for (int z = 0; z < h; z++)
        for (int X = 0; X <= w; X++) edge(room(X - 1, z, 1), room(X, z, 3), wallX(X, z), floorAt(X - 1, z, 1), floorAt(X, z, 3));
    // inside a tile: quadrant q and q + 1 meet on the half diagonal from the tile's corner q + 1 to its centre; diagonal 0 runs
    // through corners 0 and 2 (it parts 1 | 2 and 3 | 0), diagonal 1 through corners 1 and 3 (it parts 0 | 1 and 2 | 3)
    for (int z = 0; z < h; z++)
        for (int x = 0; x < w; x++)
            for (int q = 0; q < 4; q++) {
                const int n = (q + 1) & 3;
                edge(room(x, z, q), room(x, z, n), wallD(x, z, (q & 1) ? 0 : 1), floorAt(x, z, q), floorAt(x, z, n));
            }
    for (int r = 1; r <= top; r++) {
        const size_t i = static_cast<size_t>(r);
        if (floorOpen(r) && edges[i] > 0 && (walled[i] * 4 >= edges[i] * 3 || (ownWalls && own[i] > 0))) open.push_back(r);
    }
}
// (no diagonal walls known)
template <class RoomAt, class Placed, class WallZ, class WallX>
void Classify(int w, int h, RoomAt roomAt, Placed placed, WallZ wallZ, WallX wallX, std::vector<int>& open, bool ownWalls = false) {
    Classify(w, h, roomAt, placed, wallZ, wallX, [](int, int, int) { return false; }, open, ownWalls);
}

// The lights of the open rooms the room 0 above takes (ShareRoomZeroFromBelow): their lamps, not their window lights (types 7
// and 8). A window light sits in its wall's plane at the window's top, so the points of the story above just over it (the
// walls lifted off the floor line) took it at a few centimetres: daylight hot spots over every door and window of the room
// below (14-06-26, lot C49C story 3: largest texel sum 21.8 by day, 0 at night), and a room-0 list of 36 lights by day
// instead of 17. The room's daylight reaches those texels through its own walls' top row (RtOutdoorLamps' carry).
inline bool SharedFromBelow(int lightType) { return lightType != 7 && lightType != 8; }

} // namespace OpenAbove
