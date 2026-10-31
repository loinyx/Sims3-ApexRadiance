// The extracted ShadeLamps and MovedApart (features/lot_light_bridge.cpp) and LampMoving (features/drag_lite.cpp): which lamps
// the walls' shade on the ground tests, and where, while a lamp the GPU draws is dragged. Fake lamp list, lights and clock;
// no game process, file writes or GPU calls.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include "features/fast_math.h"
#include "features/ground_lamp_shadow.h"
#include "features/lot_light_bridge.h"
using DWORD = unsigned long;
struct LotRect {
    float x0, z0, x1, z1, ty;
};
void LotBox(const LotRect& r, float& x0, float& z0, float& x1, float& z1) { x0 = r.x0, z0 = r.z0, x1 = r.x1, z1 = r.z1; }
LotLightBridge::BakeSnapshot g_snap;
namespace LotLightBridge {
const BakeSnapshot& CurrentBakeLamps() { return g_snap; }
}
struct Place {
    uintptr_t id;
    float pos[3], rect[4];
};
std::vector<uintptr_t> g_excluded, g_moved;
std::vector<Place> g_places;
namespace NightTerrainBake {
bool Excluded(uintptr_t L) { return std::find(g_excluded.begin(), g_excluded.end(), L) != g_excluded.end(); }
}
namespace TerrainStamps {
bool MovedWithin(uintptr_t L, unsigned long) { return std::find(g_moved.begin(), g_moved.end(), L) != g_moved.end(); }
}
DWORD g_now = 0;
DWORD GetTickCount() { return g_now; }
namespace DragLite {
constexpr unsigned long kStillMs = 750, kPlaceGraceMs = 300;
std::atomic<DWORD> g_lastMove{0}, g_letGoAt{0};
bool g_letGoOn = true;
bool LetGoOn() { return g_letGoOn; }
bool g_engaged = true;
bool Engaged() { return g_engaged; }
/* DRAG_FUNCTIONS */
}
// a lamp move 100 ms ago with the button held (on), or none for 4 s
void Moving(bool on) { DragLite::g_lastMove = 1000, DragLite::g_letGoAt = 0, g_now = on ? 1100 : 5000; }
bool LampPlaceNow(uintptr_t L, uint64_t, int, float pos[3], float rect[4]) {
    for (const Place& p : g_places)
        if (p.id == L) return std::memcpy(pos, p.pos, 12), std::memcpy(rect, p.rect, 16), true;
    return false;
}
/* PRODUCTION_FUNCTIONS */
int assertions = 0, failures = 0;
void Assert(bool ok, const char* name) {
    assertions++;
    if (!ok) failures++, std::printf("FAIL: %s\n", name);
}
LotLightBridge::BakeLamp Lamp(uintptr_t id, float x, float z, float light = 50.0f, float half = 20.0f) {
    LotLightBridge::BakeLamp b;
    b.lot = 1, b.type = 11, b.id = id, b.baked = true;
    b.pos[0] = x, b.pos[1] = 2.0f, b.pos[2] = z;
    b.rect[0] = x - half, b.rect[1] = z - half, b.rect[2] = x + half, b.rect[3] = z + half;
    for (float& c : b.light) c = light;
    return b;
}
Place At(uintptr_t id, float x, float z) { return Place{id, {x, 2.0f, z}, {x - 20.0f, z - 20.0f, x + 20.0f, z + 20.0f}}; }
bool Same(const std::vector<GroundShadow::Lamp>& a, const std::vector<GroundShadow::Lamp>& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](const GroundShadow::Lamp& p, const GroundShadow::Lamp& q) {
               return std::memcmp(&p, &q, sizeof p) == 0;
           });
}
bool Has(const std::vector<GroundShadow::Lamp>& v, float x) {
    return std::any_of(v.begin(), v.end(), [x](const GroundShadow::Lamp& L) { return L.x == x; });
}
int main() {
    const LotRect lot{0, 0, 40, 40, 0};
    const auto run = [&](std::vector<GroundShadow::Lamp>& others, size_t& live) {
        live = 99;
        return ShadeLamps(lot, &others, &live);
    };
    // A and B over the lot, C over the margin only
    g_snap.lamps = {Lamp(0xA, 10, 10), Lamp(0xB, 30, 30), Lamp(0xC, 60, 20, 50.0f, 12.0f)};
    std::vector<GroundShadow::Lamp> restOthers, others;
    size_t live = 0;
    const std::vector<GroundShadow::Lamp> rest = run(restOthers, live);
    Assert(rest.size() == 2 && restOthers.size() == 1 && restOthers[0].x == 60.0f && live == 0, "at rest: A and B tested, C in the total");

    g_excluded = {0xA}, g_places = {At(0xA, 12, 10)}, Moving(true), g_moved = {0xA};
    std::vector<GroundShadow::Lamp> lamps = run(others, live);
    Assert(lamps.size() == 1 && lamps[0].x == 30.0f, "moving: A leaves the wall test");
    Assert(live == 1 && others.size() == 2 && others[0].x == 12.0f && others[1].x == 60.0f, "moving: A first in the total, at its place now");

    { // the GPU path: A stays tested, at its place now; nothing in the total only
        size_t moving = 0;
        std::vector<GroundShadow::Lamp> gpuOthers;
        live = 99;
        const std::vector<GroundShadow::Lamp> gpuLamps = ShadeLamps(lot, &gpuOthers, &live, true, &moving);
        Assert(gpuLamps.size() == 2 && Has(gpuLamps, 12.0f) && Has(gpuLamps, 30.0f) && live == 0 && moving == 1 && Same(gpuOthers, restOthers),
               "GPU path, moving: A tested at its place now, counted as moving, the total as at rest");
    }

    g_moved.clear(); // A excluded and still while another lamp moves
    lamps = run(others, live);
    Assert(live == 0 && Has(lamps, 12.0f) && Same(others, restOthers), "still while another lamp moves: A tested at its place now");

    g_moved = {0xA}, Moving(false); // let go
    lamps = run(others, live);
    Assert(live == 0 && Has(lamps, 12.0f), "let go: A tested at its place now");

    g_places = {At(0xA, 10.03f, 10)}, g_moved.clear(); // the list within kMoveTol of its place now: the list's, as at rest
    lamps = run(others, live);
    Assert(live == 0 && Same(lamps, rest) && Same(others, restOthers), "the list within 5 cm: the same lamps as at rest, bit for bit");

    g_excluded.clear(), g_places.clear(), Moving(true), g_moved = {0xA};
    lamps = run(others, live);
    Assert(live == 0 && Same(lamps, rest) && Same(others, restOthers), "nothing excluded: the same lamps as at rest, bit for bit");

    // 34 lamps over the lot (the strongest first): at rest 32 tested; the strongest moving leaves 31, the 33rd stays untested
    g_snap.lamps.clear();
    for (int i = 0; i < 34; i++) g_snap.lamps.push_back(Lamp(0x100 + i, 1.0f + i, 20.0f, 100.0f - i));
    g_excluded.clear(), g_moved.clear(), g_places.clear();
    const std::vector<GroundShadow::Lamp> rest34 = run(restOthers, live);
    g_excluded = {0x100}, g_moved = {0x100}, g_places = {At(0x100, 1.0f, 20.0f)};
    lamps = run(others, live);
    std::vector<GroundShadow::Lamp> expected(rest34.begin(), rest34.end());
    std::erase_if(expected, [](const GroundShadow::Lamp& L) { return L.x == 1.0f; });
    Assert(rest34.size() == 32 && Same(lamps, expected), "past 32 lamps: the tested ones are those at rest less the moving one");
    Assert(live == 1 && others.size() == restOthers.size() + 1 && others[0].x == 1.0f, "past 32 lamps: the moving one first in the total");
    {
        std::vector<GroundShadow::Lamp> gpuOthers;
        const std::vector<GroundShadow::Lamp> gpuLamps = ShadeLamps(lot, &gpuOthers, &live, true);
        const auto fields = [](const std::vector<GroundShadow::Lamp>& a, const std::vector<GroundShadow::Lamp>& b) { // (by field: a struct's padding is not compared)
            return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](const GroundShadow::Lamp& p, const GroundShadow::Lamp& q) {
                       return p.x == q.x && p.y == q.y && p.z == q.z && p.k == q.k && p.liftable == q.liftable && p.rx0 == q.rx0 && p.rz0 == q.rz0 && p.rx1 == q.rx1 && p.rz1 == q.rz1;
                   });
        };
        Assert(fields(gpuLamps, rest34) && fields(gpuOthers, restOthers) && live == 0, "GPU path, past 32 lamps: the 32 strongest tested as at rest (the moving one at its place)");
    }

    // 8 moving and 100 over the margin: the moving ones first, 96 in all
    g_snap.lamps.clear(), g_excluded.clear(), g_moved.clear(), g_places.clear();
    for (int i = 0; i < 8; i++) {
        g_snap.lamps.push_back(Lamp(0x200 + i, 5.0f + i, 5.0f));
        g_excluded.push_back(0x200 + i), g_moved.push_back(0x200 + i), g_places.push_back(At(0x200 + i, 5.0f + i, 5.0f));
    }
    for (int i = 0; i < 100; i++) g_snap.lamps.push_back(Lamp(0x300 + i, 55.0f + 0.1f * i, 20.0f, 50.0f, 12.0f));
    lamps = run(others, live);
    Assert(lamps.empty() && live == 8 && others.size() == 96, "8 moving and 100 over the margin: 96 in the total");
    Assert(std::all_of(others.begin(), others.begin() + 8, [](const GroundShadow::Lamp& L) { return L.x < 13.0f; }), "the moving ones first");

    g_excluded = {0xA}, g_places = {At(0xA, 12, 10)}, g_moved = {0xA}, Moving(true), DragLite::g_engaged = false;
    g_snap.lamps = {Lamp(0xA, 10, 10), Lamp(0xB, 30, 30)};
    lamps = run(others, live);
    Assert(live == 0 && Has(lamps, 12.0f), "the drag over for its hold (Engaged false): tested at its place now, not in the total only");
    DragLite::g_engaged = true;
    { // a Classic drag hold seen lately (g_classicHeldAt): the worker's lots take no lamp as moving, the GPU path's still do
        g_classicHeldAt = g_now | 1;
        lamps = run(others, live);
        Assert(live == 0 && Has(lamps, 12.0f), "Classic hold seen lately: A tested at its place now, not in the total only");
        size_t moving = 0;
        std::vector<GroundShadow::Lamp> gpuOthers;
        ShadeLamps(lot, &gpuOthers, &live, true, &moving);
        Assert(moving == 1, "Classic hold seen lately, GPU path: A still counted as moving");
        g_now += DragLite::kStillMs + 1, DragLite::g_lastMove = g_now - 100;
        lamps = run(others, live);
        Assert(live == 1 && !Has(lamps, 12.0f), "kStillMs after the hold: a moving A is in the total only again");
        g_classicHeldAt = 0;
    }
    { // a GPU refusal holds while the occluder key is the same (lamp places are not in it)
        uint64_t refused = 0;
        Assert(RefusalHolds(refused, 0x51) && refused == 0x51, "refusal: the first check takes the occluder key");
        Assert(RefusalHolds(refused, 0x51), "refusal: the same occluders, still refused");
        Assert(!RefusalHolds(refused, 0x77) && refused == 0, "refusal: other occluders, the GPU is tried again");
    }
    // LampMoving: a move within 0.75 s that the button has not let go of since
    using namespace DragLite;
    g_lastMove = 1000, g_letGoAt = 0, g_now = 1500;
    Assert(LampMoving(), "a move 500 ms ago");
    g_now = 1800;
    Assert(!LampMoving(), "still 800 ms");
    g_now = 1200, g_letGoAt = 1100;
    Assert(!LampMoving(), "let go after the move");
    g_lastMove = 1390, g_now = 2100;
    Assert(!LampMoving(), "a placement step 290 ms after the let go does not bring it back, even past the let go's 1 s");
    g_lastMove = 1400, g_now = 1500;
    Assert(LampMoving(), "a move 300 ms after the let go: a new drag");
    g_letGoOn = false, g_lastMove = 1000, g_letGoAt = 1100, g_now = 1200;
    Assert(LampMoving(), "let go off: the still time only");
    std::printf("shade lamp checks: %d assertions, %d failures\n", assertions, failures);
    return failures ? 1 : 0;
}
