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
template <class RoomAt, class Placed, class WallZ, class WallX>
void Classify(int w, int h, RoomAt roomAt, Placed placed, WallZ wallZ, WallX wallX, std::vector<int>& open) {
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
    std::vector<int> quads(static_cast<size_t>(top) + 1, 0), bare(quads), edges(quads), walled(quads);
    for (int z = 0; z < h; z++)
        for (int x = 0; x < w; x++)
            for (int q = 0; q < 4; q++)
                if (const int r = room(x, z, q)) {
                    quads[static_cast<size_t>(r)]++;
                    if (!placed(x, z, q)) bare[static_cast<size_t>(r)]++;
                }
    const auto floorOpen = [&](int r) { return r > 0 && bare[static_cast<size_t>(r)] * 2 >= quads[static_cast<size_t>(r)]; };
    const auto edge = [&](int a, int b, bool wall) { // the two quadrants facing each other across one tile edge
        if (a == b) return;
        for (const int side : {a, b}) {
            if (!floorOpen(side)) continue;
            const int other = side == a ? b : a;
            edges[static_cast<size_t>(side)]++;
            if (wall || floorOpen(other)) walled[static_cast<size_t>(side)]++;
        }
    };
    for (int Z = 0; Z <= h; Z++)
        for (int x = 0; x < w; x++) edge(room(x, Z - 1, 2), room(x, Z, 0), wallZ(x, Z));
    for (int z = 0; z < h; z++)
        for (int X = 0; X <= w; X++) edge(room(X - 1, z, 1), room(X, z, 3), wallX(X, z));
    for (int r = 1; r <= top; r++)
        if (floorOpen(r) && edges[static_cast<size_t>(r)] > 0 && walled[static_cast<size_t>(r)] * 4 >= edges[static_cast<size_t>(r)] * 3) open.push_back(r);
}

} // namespace OpenAbove
