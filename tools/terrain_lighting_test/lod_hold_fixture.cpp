// The LOD hold of the terrain bake and its detail signal: literal production functions (ReadDetailSwitches, DetailOf,
// LodHolds, InBake, DiffBake, BakeTest, RefreshLotRects, LotGroundY) on fixture lamps, a fake lot renderer map, lot table
// and light records. No game process, file writes or GPU calls.
#define NOMINMAX
#include <Windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <format>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include "features/fast_math.h"
#include "features/lot_light_bridge.h"
#include "features/terrain_bake_rule.h"
#include "framework/transition_watch_native.h"
using Clock = std::chrono::steady_clock;
constexpr float kMoveTol = .05f, kLightRel = .05f, kLightAbs = .05f;
/* PRODUCTION_STRUCTS */
/* PRODUCTION_HELPERS */
// The lot renderers' map ReadDetailSwitches reads (FakeMap below)
uintptr_t g_fakeMap = 0;
uintptr_t LotRenderers() { return g_fakeMap; }
/* PRODUCTION_DETAIL */
namespace LotLightBridge {
/* PRODUCTION_DIFF */
}
// The hold set the bake's light test reads, and how often it was asked
int lodLookups = 0;
std::vector<std::pair<uintptr_t, uint64_t>> heldLights;
namespace LotLightBridge {
bool LodHeld(uintptr_t light, uint64_t lot, int) {
    lodLookups++;
    return std::find(heldLights.begin(), heldLights.end(), std::make_pair(light, lot)) != heldLights.end();
}
}
bool g_lotLamps = true;
using BoolVfn = bool(__thiscall*)(void*);
/* PRODUCTION_BAKE_TEST */
// The light tree's lot table (fixture lots) and how often RefreshLotRects read it
std::atomic<DWORD> g_lotDrawTick{0};
int lotTableReads = 0;
struct LotRect;
int ReadLotRects(LotRect* out, int max);
/* PRODUCTION_LOT_TABLE */
std::vector<LotRect> fakeLotRects;
int ReadLotRects(LotRect* out, int max) {
    lotTableReads++;
    const int n = std::min(max, static_cast<int>(fakeLotRects.size()));
    std::copy_n(fakeLotRects.begin(), n, out);
    return n;
}
namespace LotLightBridge {
/* PRODUCTION_GROUND */
}

int assertions = 0, failures = 0;
void Assert(bool ok, const char* name) {
    assertions++;
    if (!ok) failures++, std::printf("FAIL: %s\n", name);
}

// A lamp of lot 7 as ReadLotLamp gives it (type 3, at x 10, z 20, light rect 20 m wide)
LotLampState Lamp(BYTE flags, int type = 3, uint64_t lot = 7) {
    LotLampState s;
    s.lot = lot;
    s.type = type;
    s.flags = flags;
    s.col[0] = 1.0f, s.col[1] = .8f, s.col[2] = .6f;
    s.inten = 1.0f;
    s.range = 40.0f;
    s.fade = 1.0f;
    s.pos[0] = 10.0f, s.pos[2] = 20.0f;
    s.rect[0] = 0.0f, s.rect[1] = 10.0f, s.rect[2] = 20.0f, s.rect[3] = 30.0f;
    s.plain = IsPlainType(type);
    s.baked = InBake(s);
    return s;
}
// ReadEnumeratedLamps' step for one lamp, restated (run_lod_hold_checks.ps1 checks the production lines): the hold, then baked
LotLampState Read(const LotLampState& p, LotLampState s, bool hold, int detail) {
    if (hold && s.lot != 0 && (s.flags & 0x61) == 0x21 && LodHolds(p, s, detail)) s.held = true, s.heldFade = p.held ? p.heldFade : p.fade;
    s.baked = s.held || InBake(s);
    return s;
}
// The snapshot entry TrackLotLampEdits makes of a lamp
LotLightBridge::BakeLamp Bake(const LotLampState& s, uintptr_t id = 0x100) {
    LotLightBridge::BakeLamp b;
    b.lot = s.lot;
    b.type = s.type;
    std::memcpy(b.pos, s.pos, sizeof b.pos);
    std::memcpy(b.rect, s.rect, sizeof b.rect);
    for (int c = 0; c < 3; c++) b.light[c] = LampLight(s, c);
    b.baked = s.baked;
    b.id = id;
    return b;
}
LotLightBridge::BakeSnapshot Snap(const std::vector<LotLightBridge::BakeLamp>& lamps, const std::vector<uint64_t>& settled) {
    LotLightBridge::BakeSnapshot s;
    s.lamps = lamps;
    for (const auto& b : lamps) s.lots.push_back(b.lot);
    std::sort(s.lots.begin(), s.lots.end());
    s.lots.erase(std::unique(s.lots.begin(), s.lots.end()), s.lots.end());
    s.settledLots = settled;
    return s;
}
LotLightBridge::BakeDiff Diff(const LotLampState& before, const LotLampState& after) {
    return LotLightBridge::DiffBake(Snap({Bake(before)}, {7}), Snap({Bake(after)}, {7}), true);
}

// A light record as the bake's visitor passes it: vtable, type +0xB0, lot +0xC0/+0xC4, flags +0x100, room +0x08 (0)
bool __fastcall FakeOriginal(void* self, void*) { // vfunc+0x20: always true for type 11 and world lights, false for lot lamps
    const BYTE* L = static_cast<const BYTE*>(self);
    return *reinterpret_cast<const int*>(L + 0xB0) == 11 || (*reinterpret_cast<const uint32_t*>(L + 0xC0) | *reinterpret_cast<const uint32_t*>(L + 0xC4)) == 0;
}
void* g_vtable[16];
struct FakeLight {
    alignas(16) BYTE b[0x200];
};
void Fill(FakeLight& f, uint64_t lot, int type, BYTE flags) {
    std::memset(f.b, 0, sizeof f.b);
    void** vt = g_vtable;
    std::memcpy(f.b, &vt, sizeof vt);
    *reinterpret_cast<int*>(f.b + 0xB0) = type;
    *reinterpret_cast<uint32_t*>(f.b + 0xC0) = static_cast<uint32_t>(lot);
    *reinterpret_cast<uint32_t*>(f.b + 0xC4) = static_cast<uint32_t>(lot >> 32);
    f.b[0x100] = flags;
}
int Test(FakeLight& f, bool held) {
    heldLights.clear();
    if (held) heldLights.emplace_back(reinterpret_cast<uintptr_t>(f.b), *reinterpret_cast<const uint32_t*>(f.b + 0xC0));
    return BakeTest(f.b);
}

// The lot renderers' map as TransitionWatch::ReadLots walks it (Steam layout, 32-bit): buckets +0x1C, their count +0x20,
// the sentinel after the last bucket; a node's object +8 and next +0x10; the object's lot id +0x10 and lot +0x18; Lot+0xC1
static_assert(sizeof(void*) == 4, "the renderer map fixture is 32-bit, as the game");
struct FakeRenderer {
    alignas(16) BYTE map[0x40];
    uint32_t buckets[18], sentinel;
    BYTE nodes[17][0x14], objects[17][0x40], lots[17][0x100];
};
FakeRenderer g_renderer;
uint32_t Addr(const void* p) { return static_cast<uint32_t>(reinterpret_cast<uintptr_t>(p)); }
// The lots `ids`, one per bucket; the first `detailed` of them with +0xC1 set
void FakeMap(const std::vector<uint64_t>& ids, size_t detailed) {
    FakeRenderer& r = g_renderer;
    std::memset(&r, 0, sizeof r);
    *reinterpret_cast<uint32_t*>(r.map + 0x1C) = Addr(r.buckets);
    *reinterpret_cast<uint32_t*>(r.map + 0x20) = static_cast<uint32_t>(ids.size());
    r.buckets[ids.size()] = Addr(&r.sentinel);
    for (size_t i = 0; i < ids.size(); i++) {
        r.buckets[i] = Addr(r.nodes[i]);
        *reinterpret_cast<uint32_t*>(r.nodes[i] + 8) = Addr(r.objects[i]);
        std::memcpy(r.objects[i] + 0x10, &ids[i], 8);
        *reinterpret_cast<uint32_t*>(r.objects[i] + 0x18) = Addr(r.lots[i]);
        r.lots[i][0xC1] = static_cast<BYTE>(i < detailed);
    }
    g_fakeMap = reinterpret_cast<uintptr_t>(r.map);
}

int main() {
    // The detail signal of the Detailed View route and the hold
    const auto at = [](int s) { return Clock::time_point{} + std::chrono::seconds(1000 + s); };
    FakeMap({0xA, 0xB, 0xC}, 2);
    ReadDetailSwitches(at(0));
    Assert(g_detailKnown && DetailOf(0xA, at(0)) == 1 && DetailOf(0xC, at(0)) == 0, "the lots with +0xC1 are the detailed ones");
    FakeMap({0xB, 0xA}, 1);
    ReadDetailSwitches(at(1));
    Assert(g_detailSwitchAt.contains(0xA) && DetailOf(0xA, at(1)) == 0, "a lot that leaves the set is a recorded demotion, not detailed");
    Assert(DetailOf(0xB, at(1)) == 1 && DetailOf(0xB, at(30)) == 2, "a lot detailed throughout keeps its time: 30 s of detail");
    std::vector<uint64_t> many;
    for (uint64_t i = 0; i < 17; i++) many.push_back(0x100 + i);
    FakeMap(many, 17);
    ReadDetailSwitches(at(2));
    Assert(!g_detailKnown && DetailOf(0xB, at(2)) == -1 && g_detailSwitchAt.size() == 1, "more than 16 detailed lots: unknown, no switch recorded");
    g_fakeMap = 0;
    ReadDetailSwitches(at(2));
    Assert(!g_detailKnown && DetailOf(0xC, at(2)) == -1, "no renderer map: unknown");
    FakeMap({0xA, 0xB}, 2);
    ReadDetailSwitches(at(5));
    Assert(DetailOf(0xA, at(34)) == 1 && DetailOf(0xB, at(34)) == 2, "a lot detailed again starts its time again; the others keep theirs");
    Assert(g_detailSwitchAt.empty(), "a recorded demotion expires after 2 s");

    g_vtable[0x20 / 4] = reinterpret_cast<void*>(&FakeOriginal);
    const LotLampState on = Lamp(0x77), onF7 = Lamp(0xF7);
    Assert(on.baked && onF7.baked, "an enabled lit lamp is in the bake");

    // The game demotes a lot that is not detailed any more: 0x77 -> 0x35 (and 0xF7 -> 0xB5)
    const LotLampState held = Read(on, Lamp(0x35), true, 0);
    Assert(held.held && held.baked && held.heldFade == 1.0f, "a demotion of a lot not detailed is held in the bake at its enabled fade");
    Assert(!Diff(on, held).Any(), "a held demotion makes no difference in the snapshot: no relight");
    Assert(Read(onF7, Lamp(0xB5), true, 0).held, "0xF7 -> 0xB5 is held as well");
    const LotLampState kept = Read(held, Lamp(0x35), true, 0);
    Assert(kept.held && kept.baked && !RawChanged(held, kept) && held.baked == kept.baked, "an unchanged held lamp stays held, with nothing to compare");
    Assert(Read(held, Lamp(0x35), true, -1).held, "a held lamp stays held while the detail set cannot be read");

    // The same flip on a lot still detailed: a sale or delete in Build mode
    const LotLampState sold = Read(on, Lamp(0x35), true, 1);
    const auto soldDiff = Diff(on, sold);
    Assert(!sold.held && !sold.baked, "a lamp switched off on a detailed lot leaves the bake");
    Assert(soldDiff.switchedOff == 1 && soldDiff.changes.size() == 1 && soldDiff.changes[0].hasOld && !soldDiff.changes[0].hasNew,
           "a sale's switch-off is a listed difference with its old rect");
    Assert(!Read(on, Lamp(0x35), true, -1).held, "no hold starts while the detail set cannot be read");
    Assert(!Read(on, Lamp(0x35), false, 0).held, "no hold starts by day or in the edit mode");

    // Promotion: the enabled bit comes back
    const LotLampState promoted = Read(held, Lamp(0x77), true, 1);
    Assert(!promoted.held && promoted.baked, "a promoted lamp is in the bake by its own flags");
    Assert(!Diff(held, promoted).Any() && !Diff(on, promoted).Any(), "a promotion after a hold makes no difference: no relight");

    // Releases: each one is a switch-off the snapshot lists
    const auto released = [&](const LotLampState& s, const char* name) {
        Assert(!s.held && !s.baked && Diff(held, s).switchedOff + Diff(held, s).removed >= 1, name);
    };
    released(Read(held, Lamp(0x35), false, 0), "holds end by day and in the edit mode, as a counted switch-off");
    released(Read(held, Lamp(0x35), true, 2), "a lot detailed for 30 s whose lamp stays disabled ends the hold");
    released(Read(held, Lamp(0x15), true, 0), "the lit bit going out ends the hold");
    LotLampState dimmed = Lamp(0x35);
    dimmed.inten = .5f;
    Assert(!Read(held, dimmed, true, 0).held, "a light change ends the hold");
    LotLampState movedAway = Lamp(0x35);
    movedAway.pos[0] += 1.0f;
    Assert(!Read(held, movedAway, true, 0).held, "a move ends the hold");
    Assert(!Read(held, Lamp(0x35, 3, 8), true, 0).held, "a lot change ends the hold");

    // What is never held
    const LotLampState never = Lamp(0x35);
    Assert(!never.baked && !Read(never, Lamp(0x35), true, 0).held, "a lamp never enabled this session (a lot never detailed) is not held");
    Assert(!Read(on, Lamp(0x31), true, 0).held, "a demotion read with another flag changed is not held");
    Assert(!Read(on, Lamp(0x15), true, 0).held, "a lamp that goes out at its demotion is not held");
    Assert(!Read(Lamp(0x77, 11, 0), Lamp(0x35, 11, 0), true, 0).held, "a world lamp (lot 0) is never held");
    Assert(Read(Lamp(0x77, 11), Lamp(0x35, 11), true, 0).held, "a lot-owned type-11 lamp is held as the lamp classes are");

    // A held lamp's lot leaves the lamp list: DiffBake sees its lamps gone only while the lot is among the settled lots
    const auto gone = LotLightBridge::DiffBake(Snap({Bake(held)}, {7}), Snap({}, {7}), true);
    Assert(gone.removed == 1 && gone.changes.size() == 1 && gone.changes[0].user && gone.changes[0].hasOld &&
               gone.changes[0].oldRect[2] == 20.0f, "a held lamp gone with its lot is a removal with its old rect");
    Assert(!LotLightBridge::DiffBake(Snap({Bake(held)}, {7}), Snap({}, {}), true).Any(), "without the gone lot among the settled lots nothing is seen");

    // The bake's light test
    FakeLight f;
    Fill(f, 7, 3, 0x77);
    lodLookups = 0;
    Assert(Test(f, false) == 3 && lodLookups == 0, "an enabled lot lamp is taken without asking the hold");
    Fill(f, 7, 3, 0x35);
    Assert(Test(f, true) == 3, "a held lamp is taken without its enabled bit");
    lodLookups = 0;
    Assert(Test(f, false) == 0 && lodLookups == 1, "a disabled lamp not held is refused");
    g_lotLamps = false;
    Assert(Test(f, true) == 0, "a held lamp of the lamp classes stays out with lot lamps off the street");
    g_lotLamps = true;
    Fill(f, 7, 11, 0x35);
    Assert(Test(f, true) == 1 && Test(f, false) == 0, "a lot-owned type-11 lamp: taken when held, refused when not");
    Fill(f, 7, 3, 0x15);
    lodLookups = 0;
    Assert(Test(f, true) == 0 && lodLookups == 0, "an unlit lamp never asks the hold");
    Fill(f, 0, 11, 0x35);
    lodLookups = 0;
    Assert(Test(f, false) == 1 && lodLookups == 0, "a world lamp keeps the original test");

    // The ground a held lamp is lifted from (LotGroundY): its lot left the light tree with Detailed View
    LotRect r{};
    r.lotLo = 7, r.base = 3.0f;
    fakeLotRects = {r};
    RefreshLotRects();
    float y = 0.0f;
    int reads = lotTableReads;
    Assert(LotLightBridge::LotGroundY(7, y) && y == 3.0f && lotTableReads == reads, "ground: a lot in the light tree");
    fakeLotRects.clear();
    RefreshLotRects();
    g_lotDrawTick++;
    reads = lotTableReads, y = 0.0f;
    Assert(LotLightBridge::LotGroundY(7, y) && y == 3.0f && lotTableReads == reads, "ground: a lot out of the light tree keeps its last base, with no read");
    Assert(!LotLightBridge::LotGroundY(9, y) && !LotLightBridge::LotGroundY(9, y) && lotTableReads == reads + 1, "ground: a lot never read reads the tree once a frame");

    std::printf("LOD hold fixtures: %d assertions, %d failures; the bake and the lamp list are fixture data.\n", assertions, failures);
    return failures ? 1 : 0;
}
