// Object light bridge (part of Night Lighting)
//
// Outdoor objects (fences, bushes, Sims, props) are lit by per-object light rigs: slot 0 = sun, slots 1..3 = the three
// strongest point lights. The rig gather (FUN_006bbba0 -> FUN_006b5af0 -> FUN_006bb270) asks each light for its colour at
// the object centre through light vfunc+0x10. For street lamps (class vtable 0xFF42F8, slot 0xFF4308 = FUN_006c02a0) that
// colour falls off with the squared 3D distance from the lamp head, without the x3.33 the lot ground gets, so fences and
// bushes next to a lamp get ~30% of the light and dimmer lamps fall under the 0.1 luminance cut. A per-rig brightness cap
// (FUN_006b92a0, cap read at 0x6B9418 from 0x011D0BA8) then scales everything down again.
//
// Fix: replace that vtable slot with a wrapper that, only when called by the rig gather (return address 0x006BB2B3), uses
// the lamp's ground footprint instead: horizontal distance with the radius the game stores in the light bounds
// (+0x134..+0x140), like the terrain light stamp. The rig cap is raised so the result is not scaled back.
//
// A rig only gathers point lights when rig+0x224 bit 0x10 is set (FUN_006bbba0; the binder FUN_006b8b30 otherwise sends
// zero lamp constants). The rig constructor FUN_006bb8f0 copies that bit from the scene model (model+0x29C bit 4). The
// SceneModel constructor (FUN_006f5b40) sets it; the game clears it (FUN_006f4840(0)) for terrain, roads, roofs, the sea
// and for objects whose script asks so (message 0x827917ca). RigCtorForce sets it for every rig built by FUN_006f7880.
// Note: fences, railings and stairs already have it; their missing lamp light has another cause (they read only the
// rig's overflow "vertex light" slots) and is fixed in the pixel shader (lot_light_bridge.cpp DrawInstanced).

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "object_light_bridge.h"
#include "fast_math.h"
#include "level_light_share.h"
#include "lot_light_bridge.h"
#include "game_addresses.h"
#include "memory_patch.h"
#include "apex_log.h"
#include "hook_guard.h"
#include <windows.h>
#include <intrin.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <format>
#include <iterator>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "tester_toggles.h"
#include "rig_cut.h"
#include "terrain_lighting_policy.h"
#include "night_level.h"

namespace {

// Addresses: the fixed Steam 1.67.2 ones (in the comments), or found by signature on other builds (game_addresses.h).
// Set by LoadAddresses before anything uses them; 0 = not found on this build.
uintptr_t kVtableSlot = 0;       // 0x00FF4308: class 0xFF42F8 (type 11, street lamp) vfunc+0x10
uintptr_t kOriginalFn = 0;       // 0x006C02A0: FUN_006c02a0
uintptr_t kRigGatherReturn = 0;  // 0x006BB2B3: after "call edx" in FUN_006bb270
uintptr_t kCapOperandSite = 0;   // 0x006B9418: mov ecx, offset 0x011D0BA8
std::vector<BYTE> kCapOrig;      // B9 <kCapGlobal>
uintptr_t kCapGlobal = 0;        // 0x011D0BA8
uintptr_t kLumaWeights = 0;      // 0x011D1140
uintptr_t kDirtyAllRigs = 0;     // 0x006B58F0: __fastcall(cells)
const BYTE kDirtyAllBytes[] = {0x83, 0xEC, 0x10, 0x55, 0x8B, 0xE9, 0x33, 0xC9, 0x33, 0xC0, 0x39, 0x4D, 0x30}; // Steam's prologue (checked on Steam)
uintptr_t kRootPtr = 0;          // 0x011D1860

using LightColour_t = void(__fastcall*)(void* light, void* edx, const float* pos, float* rec);
using DirtyAll_t = void(__fastcall*)(void* cells);

std::vector<MemPatch::PatchLocation> g_patches;
std::atomic<bool> g_installed{false};
float g_strength = 1.0f;
float g_capScaled = 1.0f;
std::atomic<bool> g_refreshRequested{false};
std::atomic<DWORD> g_renderThread{0}; // the game's light system may only be touched from the render thread
std::atomic<int> g_boosted{0};

uintptr_t kRigCtor = 0;            // 0x006BB8F0: FUN_006bb8f0 (thiscall rig, flag, model, kind)
uintptr_t kRigCtorCalls[3] = {};   // 0x006F7905, 0x006F795C, 0x006F799D: call sites in FUN_006f7880
std::vector<MemPatch::PatchLocation> g_rigPatches;
std::atomic<bool> g_forceAll{true};
std::atomic<int> g_forcedRigs{0};

using RigCtor_t = void*(__thiscall*)(void* rig, unsigned flag, void* model, int kind);

void* __fastcall RigCtorForce(void* rig, void*, unsigned flag, void* model, int kind) {
    if (!(flag & 1) && g_forceAll.load(std::memory_order_relaxed)) {
        flag |= 1;
        g_forcedRigs.fetch_add(1, std::memory_order_relaxed);
    }
    return reinterpret_cast<RigCtor_t>(kRigCtor)(rig, flag, model, kind);
}

// Fences and railings that close an area turn it into a roofless "room" (room+0x18 = 1). Objects inside get room mode 1
// (FUN_006c7cf0 -> FUN_006baa70) and their lamps come only from that room's light list (FUN_006bbde0 -> FUN_006bb2f0);
// the per-light test FUN_006bb270 only takes lights of the object's own room (light+8 == rig+0x1e0). Street lamps belong
// to no room, so a fenced yard never gets their light. Fix: after the room gather, for mode 1 rigs also run the world
// cell gather (FUN_006b5af0, the one outdoor objects use) with rig+0x1e0 briefly set to 0. The game then keeps the three
// strongest of both lists.
uintptr_t kRoomGatherCall = 0; // 0x006BBE70: CALL FUN_006bb2f0 in FUN_006bbde0
uintptr_t kRoomGather = 0;     // 0x006BB2F0
uintptr_t kCellGather = 0;     // 0x006B5AF0
std::vector<MemPatch::PatchLocation> g_roomPatches;
std::atomic<int> g_roomRigs{0};

using RoomGather_t = void(__thiscall*)(void* rig, void* a, void* b, void* c, void* d);
using CellGather_t = void(__thiscall*)(void* cells, void* rig);

void CellGatherForRoomRig(BYTE* rig) {
    __try {
        const uintptr_t lightMgr = *reinterpret_cast<const uintptr_t*>(rig + 0x1B4);
        if (!lightMgr || !(rig[0x224] & 0x10)) return;
        const uintptr_t cells = *reinterpret_cast<const uintptr_t*>(lightMgr + 0x104);
        if (!cells) return;
        uint32_t& room = *reinterpret_cast<uint32_t*>(rig + 0x1E0);
        const uint32_t saved = room;
        room = 0;
        __try {
            reinterpret_cast<CellGather_t>(kCellGather)(reinterpret_cast<void*>(cells), rig);
        } __finally {
            room = saved;
        }
        g_roomRigs.fetch_add(1, std::memory_order_relaxed);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// Room-mode rigs are taken out of the world light cells (FUN_006baa70), and the game re-gathers only in-cell rigs when
// lamps switch on at dusk (FUN_006b58f0 walks the cells). So the extra cell gather above would keep the lamps of the
// moment the rig was placed (none, for a save loaded by day). The rigs it served are remembered and re-gathered on the
// render thread whenever the night level moves by 0.1 (FUN_006bbf90 = unconditional rig update, which comes back here).
uintptr_t kRigUpdate = 0; // 0x006BBF90: __fastcall(rig)
uint32_t kRigVtable = 0;  // 0x00FF4218: set by the rig constructor FUN_006bb8f0
std::mutex g_roomRigMx;
std::unordered_set<BYTE*> g_roomRigSet;
float g_regatherLevel = -1.0f;
uintptr_t g_regatherCells = 0;

// The lamps of another story for room-mode rigs (the light between stories was not right; a Light
// capture: the frame of an atrium's upper window was black, 0.02, right over the lower window's frame lit red by a sconce
// of the story below). FUN_006bbde0 gathers a roofed room's rig from that room's light list (room+0xC8), which holds the
// lamps of the stories next to it that the light between stories took near an opening (level_light_share part 4: the
// room's walls and floors are lit by them), but FUN_006bb2f0 keeps only lights whose room id is the object's (light+8 ==
// rig+0x1E0 at 0x6BB333, and again in FUN_006bb270 at 0x6BB283). So such a lamp lit the walls and the directional maps of
// the room but never its objects' rigs: the upper frame's rig held only the three [NoLight] lights and the fill light,
// and its directional map towards the room was dark there. Now, after the game's gather, the lamps of the list that
// belong to another room and reach the rig's centre (LevelLightShare::CrossLampReach: through an opening, past the walls
// on the way; at least kCrossReachMin) go through the game's own gather once more, per room id, with rig+0x1E0 set to
// that id for the call: the same colour at the centre, the same 0.1 luminance cut, the same wall test against the
// object's room walls (arguments a..d are passed through), into the same candidate list the game then sorts.
constexpr float kCrossReachMin = 0.25f;
constexpr float kCrossRangeM = 15.0f; // farther lamps are not tested (the reach test is the costly part)
constexpr int kCrossMax = 32; // lamps of another story looked at per rig
std::atomic<long> g_crossRigs{0}, g_crossLamps{0};
struct LightVector {
    uintptr_t* begin;
    uintptr_t* end;
    uintptr_t* cap;
};
// The rigs with a lamp of another story within kCrossRangeM: the game marks a room-mode rig for a new gather only when a
// lamp of its own room changes (0x006B9230: light+8 == rig+0x1E0), and FUN_006b58f0 (all rigs) walks only the world cells,
// which room-mode rigs leave (FUN_006baa70). They are gathered again with the other rigs after a lamp edit
// (RequestRigRefresh), so the lamp switching on or off below reaches them too.
std::mutex g_crossRigMx;
std::unordered_set<BYTE*> g_crossRigSet;
// Outdoor light through openings (2.12.4): a roofed room also holds the outdoor lamps of room 0 of its story that it takes
// through its doors, windows and arches (LevelLightShare::OpeningLampReach, an immutable copy published by the room gathers,
// so any gather thread may read it). Their id is 0, and the game's room-list gather FUN_006bb2f0 cannot take them: it drops
// a light whose vfunc+0x20 is true (the street-lamp class, type 11: a garden post), then runs the 2D wall test 0x0069D4C0
// against the room's walls (room+0x30) from the rig's centre to the lamp, which sees the arch's wall as a whole run and pops
// the candidate (0x006BB3B7: [0x01158D24] -= 0x30; decompiled 13/10). So such a lamp goes through the game's per-light test
// FUN_006bb270 alone (thiscall(rig, light) with rig+0x1E0 = 0: the same colour at the centre through ClassColour, the same
// luminance cut, the same candidate list), and its record is scaled by its share through the openings instead of the
// outdoor walls' test (ShadeRec): t_openShade carries that share to ClassColour on this thread.
uintptr_t kRigLightTest = 0; // 0x006BB270: FUN_006bb270, from the call at FUN_006bb2f0 + 0x6D (its return address + 0x43 checked)
using RigLightTest_t = bool(__thiscall*)(void* rig, void* light);
struct OpenShade {
    const void* light = nullptr;
    float share = 1.0f;
    const BYTE* rig = nullptr; // the rig being gathered (the cut it is tested against, KeepOverCut)
};
thread_local OpenShade t_openShade;
std::atomic<long> g_openRigs{0}, g_openLamps{0}, g_openKept{0}, g_openLaw{0}, g_openOverCut{0}; // g_openLaw: records set by the per-pixel lamps' law (OpeningLawRec)
struct RigOpenNote {
    DWORD tick = 0;
    int n = 0;
    uintptr_t light[4] = {};
    float share[4] = {};
    bool kept[4] = {};
};
std::mutex g_rigOpenMx;
std::unordered_map<uintptr_t, RigOpenNote> g_rigOpen; // rig -> its last gather's lamps through openings (F7 notes)
std::atomic<bool> g_rigOpenAny{false}; // g_rigOpen has held a note (the gathers with none skip its lock until then)
// The list's lamps of another room that reach the rig's centre: how many were written to lamps / ids / shares (id 0: an
// outdoor lamp the room takes through its openings); nearby: one of them lies within kCrossRangeM, reached or not
// open[k]: the lamp is one the room took through an opening (an outdoor lamp of room 0, or since 2.12.4 a lamp of a roofed
// neighbour through an opening between them, LevelLightShare::OpeningLampReach): it goes through the game's per-light test
// alone (OpeningLamps), as the room-list gather's wall test against the room's walls would pop it
int CrossCandidates(BYTE* rig, BYTE* room, const LightVector* list, uintptr_t* lamps, int* ids, float* shares, bool* open, bool& nearby) {
    int n = 0;
    nearby = false;
    __try {
        const int own = *reinterpret_cast<const int*>(rig + 0x1E0);
        const float* centre = reinterpret_cast<const float*>(rig + 0x140);
        if (!list->begin || list->end < list->begin || list->end - list->begin > 4096) return 0;
        for (const uintptr_t* p = list->begin; p < list->end && n < kCrossMax; p++) {
            const uintptr_t light = *p;
            if (!light) continue;
            const int id = *reinterpret_cast<const int*>(light + 8);
            if (id == own || id < 0 || (id == 0 && !kRigLightTest)) continue;
            if (id == 0 && std::find(lamps, lamps + n, light) != lamps + n) continue; // (story 0's room 0 lamps are in the list twice)
            const float* head = reinterpret_cast<const float*>(light + 0x120); // the lamp's position
            const float dx = head[0] - centre[0], dy = head[1] - centre[1], dz = head[2] - centre[2];
            if (!(dx * dx + dy * dy + dz * dz < kCrossRangeM * kCrossRangeM)) continue; // the game's 0.1 cut drops it anyway
            float reach = id == 0 ? LevelLightShare::OpeningLampReach(room, reinterpret_cast<const void*>(light), centre)
                                  : LevelLightShare::CrossLampReach(room, reinterpret_cast<const void*>(light), centre);
            bool throughOpening = id == 0;
            if (reach < 0.0f && id > 0 && kRigLightTest) { // a lamp of a roofed neighbour taken through an opening
                reach = LevelLightShare::OpeningLampReach(room, reinterpret_cast<const void*>(light), centre);
                throughOpening = reach >= 0.0f;
            }
            if (reach >= 0.0f) nearby = true; // a lamp of another story (or through an opening) this room takes
            if (reach < kCrossReachMin) continue;
            lamps[n] = light;
            ids[n] = id;
            shares[n] = reach;
            open[n] = throughOpening;
            n++;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    return n;
}
// The game's per-light test for one lamp, rig+0x1E0 = the lamp's room id (0 for room 0's) for the call; true = kept as a candidate
bool RigLightTestAsRoom(BYTE* rig, uintptr_t light, int id) {
    bool kept = false;
    __try {
        int& room = *reinterpret_cast<int*>(rig + 0x1E0);
        const int saved = room;
        room = id;
        __try {
            kept = reinterpret_cast<RigLightTest_t>(kRigLightTest)(rig, reinterpret_cast<void*>(light));
        } __finally {
            room = saved;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        kept = false;
    }
    return kept;
}
void OpeningLamps(BYTE* rig, const uintptr_t* lamps, const float* shares, int count, int id) {
    RigOpenNote note{GetTickCount()};
    for (int i = 0; i < count; i++) {
        t_openShade = OpenShade{reinterpret_cast<const void*>(lamps[i]), shares[i], rig};
        const bool kept = RigLightTestAsRoom(rig, lamps[i], id);
        t_openShade = OpenShade{};
        if (kept) g_openKept.fetch_add(1, std::memory_order_relaxed);
        if (note.n < 4) {
            note.light[note.n] = lamps[i], note.share[note.n] = shares[i], note.kept[note.n] = kept;
            note.n++;
        }
    }
    g_openRigs.fetch_add(1, std::memory_order_relaxed);
    g_openLamps.fetch_add(count, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(g_rigOpenMx);
    if (g_rigOpen.size() > 16384) g_rigOpen.clear();
    g_rigOpen[reinterpret_cast<uintptr_t>(rig)] = note;
    g_rigOpenAny.store(true, std::memory_order_relaxed);
}
// The game's gather for one room id's lamps, rig+0x1E0 set to that id for the call
void GatherAsRoom(BYTE* rig, int id, void* a, LightVector* list, void* c, void* d) {
    __try {
        int& room = *reinterpret_cast<int*>(rig + 0x1E0);
        const int saved = room;
        room = id;
        __try {
            reinterpret_cast<RoomGather_t>(kRoomGather)(rig, a, list, c, d);
        } __finally {
            room = saved;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}
void CrossStoryLamps(BYTE* rig, void* a, void* b, void* c, void* d) {
    uintptr_t lamps[kCrossMax];
    int ids[kCrossMax];
    float shares[kCrossMax];
    bool open[kCrossMax] = {};
    BYTE* room = static_cast<BYTE*>(a) - 0x30; // a = room+0x30, the room's walls (FUN_006bbde0 at 0x6BBE6A)
    bool nearby = false;
    const int n = CrossCandidates(rig, room, static_cast<const LightVector*>(b), lamps, ids, shares, open, nearby);
    if (nearby) {
        std::lock_guard<std::mutex> lk(g_crossRigMx);
        if (g_crossRigSet.size() < 8192) g_crossRigSet.insert(rig);
    }
    if (n <= 0) {
        if (g_rigOpenAny.load(std::memory_order_relaxed)) {
            std::lock_guard<std::mutex> lk(g_rigOpenMx);
            g_rigOpen.erase(reinterpret_cast<uintptr_t>(rig)); // (F7 notes: none through openings at this gather)
        }
        return;
    }
    bool done[kCrossMax] = {};
    bool opened = false;
    for (int i = 0; i < n; i++) {
        if (done[i]) continue;
        uintptr_t group[kCrossMax];
        float groupShare[kCrossMax];
        int count = 0;
        for (int j = i; j < n; j++)
            if (!done[j] && ids[j] == ids[i] && open[j] == open[i]) {
                groupShare[count] = shares[j];
                group[count++] = lamps[j];
                done[j] = true;
            }
        if (open[i]) { // lamps through the room's openings (above): outdoor ones (id 0) and those of a roofed neighbour
            OpeningLamps(rig, group, groupShare, count, ids[i]);
            opened = true;
            continue;
        }
        LightVector one{group, group + count, group + count};
        GatherAsRoom(rig, ids[i], a, &one, c, d);
    }
    if (!opened && g_rigOpenAny.load(std::memory_order_relaxed)) {
        std::lock_guard<std::mutex> lk(g_rigOpenMx);
        g_rigOpen.erase(reinterpret_cast<uintptr_t>(rig));
    }
    g_crossRigs.fetch_add(1, std::memory_order_relaxed);
    g_crossLamps.fetch_add(n, std::memory_order_relaxed);
}

// The room of each room-mode rig, as its last gather saw it (a = room+0x30): a lamp switch's furniture waits for that room
// (RigRoom, lot_light_bridge.cpp). Any thread (gathers), read on the render thread.
std::mutex g_rigRoomMx;
std::unordered_map<uintptr_t, const void*> g_rigRoom;
std::atomic<uint32_t> g_rigRoomGen{0}; // (2.15.3) bumped under g_rigRoomMx at every change of g_rigRoom (RigRoom's last answer)

// Apex's part after the game's gather (RoomGatherThunk)
void AfterRoomGather(BYTE* rig, void* a, void* b, void* c, void* d) {
    const int mode = *reinterpret_cast<const int*>(rig + 0x1D4);
    if (mode == 0 && a) {
        std::lock_guard<std::mutex> lk(g_rigRoomMx);
        if (g_rigRoom.size() > 16384) g_rigRoom.clear(), g_rigRoomGen.fetch_add(1, std::memory_order_release);
        const void*& room = g_rigRoom[reinterpret_cast<uintptr_t>(rig)];
        const void* const now = static_cast<const BYTE*>(a) - 0x30;
        if (room != now) room = now, g_rigRoomGen.fetch_add(1, std::memory_order_release);
    }
    if (mode == 0 && a && b) CrossStoryLamps(rig, a, b, c, d);
    if (!g_forceAll.load(std::memory_order_relaxed) || mode != 1) return;
    CellGatherForRoomRig(rig);
    std::lock_guard<std::mutex> lk(g_roomRigMx);
    if (g_roomRigSet.size() < 8192) g_roomRigSet.insert(rig);
}

void __fastcall RoomGatherThunk(BYTE* rig, void*, void* a, void* b, void* c, void* d) {
    reinterpret_cast<RoomGather_t>(kRoomGather)(rig, a, b, c, d);
    // 07/10, players' Runtime Error: Apex's part allocates (maps, sets); a C++ exception there is caught and that part stays
    // off (HookGuard), the game's gather has already run, once
    HookGuard::Run("ObjectLightBridge room gather", [=] { AfterRoomGather(rig, a, b, c, d); });
}

// True when the rig is still a live room-mode rig and was updated.
bool RegatherRoomRig(BYTE* rig, int mode = 1) {
    if (!kRigUpdate || !kRigVtable) return false;
    __try {
        if (*reinterpret_cast<const uint32_t*>(rig) != kRigVtable || *reinterpret_cast<const int*>(rig + 0x1D4) != mode) return false;
        reinterpret_cast<void(__fastcall*)(void*)>(kRigUpdate)(rig);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void RegatherRoomRigs() {
    std::vector<BYTE*> list;
    {
        std::lock_guard<std::mutex> lk(g_roomRigMx);
        list.assign(g_roomRigSet.begin(), g_roomRigSet.end());
    }
    std::vector<BYTE*> gone;
    for (BYTE* rig : list)
        if (!RegatherRoomRig(rig)) gone.push_back(rig);
    std::lock_guard<std::mutex> lk(g_roomRigMx);
    for (BYTE* rig : gone) g_roomRigSet.erase(rig);
}
// The room-mode rigs near a lamp of another story gather again (after a lamp edit, see g_crossRigSet)
void RegatherCrossRigs() {
    std::vector<BYTE*> list;
    {
        std::lock_guard<std::mutex> lk(g_crossRigMx);
        list.assign(g_crossRigSet.begin(), g_crossRigSet.end());
    }
    std::vector<BYTE*> gone;
    for (BYTE* rig : list)
        if (!RegatherRoomRig(rig, 0)) gone.push_back(rig);
    std::lock_guard<std::mutex> lk(g_crossRigMx);
    for (BYTE* rig : gone) g_crossRigSet.erase(rig);
}

// The room-mode rigs of the rooms whose outdoor lamps through openings changed (LevelLightShare::TakeOpeningRigRooms): the
// game marks a room-mode rig only for a lamp of its own room, so they gather again here (render thread)
void RegatherOpeningRooms() {
    std::vector<const void*> rooms;
    LevelLightShare::TakeOpeningRigRooms(rooms);
    if (rooms.empty()) return;
    std::sort(rooms.begin(), rooms.end());
    std::vector<BYTE*> rigs;
    {
        std::lock_guard<std::mutex> lk(g_rigRoomMx);
        for (const auto& [rig, room] : g_rigRoom)
            if (std::binary_search(rooms.begin(), rooms.end(), room)) rigs.push_back(reinterpret_cast<BYTE*>(rig));
    }
    for (BYTE* rig : rigs) RegatherRoomRig(rig, 0);
}

// Render thread, every frame: current night level and light cells (0 when no world).
bool ReadNight(float& level, uintptr_t& cells) {
    if (!kRootPtr) return false;
    uintptr_t mgr = 0;
    __try {
        const uintptr_t root = *reinterpret_cast<const uintptr_t*>(kRootPtr);
        const uintptr_t lightMgr = root ? *reinterpret_cast<const uintptr_t*>(root + 0x1C0) : 0;
        if (!lightMgr) return false;
        cells = *reinterpret_cast<const uintptr_t*>(lightMgr + 0x104);
        mgr = lightMgr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return NightLevel::Read(mgr, level); // +0xF0, kept while Build mode's seasonless sun times are in use
}

void UpdateRoomRigs() {
    float level = 0.0f;
    uintptr_t cells = 0;
    if (!ReadNight(level, cells)) return;
    if (cells != g_regatherCells) { // another world: the remembered rigs are gone
        g_regatherCells = cells;
        g_regatherLevel = level;
        {
            std::lock_guard<std::mutex> lk(g_crossRigMx);
            g_crossRigSet.clear();
        }
        {
            std::lock_guard<std::mutex> lk(g_rigRoomMx);
            g_rigRoom.clear();
            g_rigRoomGen.fetch_add(1, std::memory_order_release);
        }
        std::lock_guard<std::mutex> lk(g_roomRigMx);
        g_roomRigSet.clear();
        return;
    }
    if (ApexMath::Fabs(level - g_regatherLevel) < 0.1f) return;
    g_regatherLevel = level;
    RegatherRoomRigs();
}

// FUN_006b5af0 returns with "ret 4" (thiscall(cells, rig)): at +0x145 on Steam; on other builds within its first 0x400
// bytes after the stack cleanup "add esp,1Ch" (its frame, same signature)
bool CellGatherReturns4() {
    if (GameAddr::IsFixed()) return std::memcmp(reinterpret_cast<const void*>(kCellGather + 0x145), "\xC2\x04\x00", 3) == 0;
    BYTE f[0x400];
    if (!MemPatch::ReadBytes(kCellGather, f, sizeof f)) return false;
    for (size_t i = 0; i + 6 <= sizeof f; i++)
        if (std::memcmp(f + i, "\x83\xC4\x1C\xC2\x04\x00", 6) == 0) return true;
    return false;
}

// FUN_006bb270 (outdoor lamps through openings): the call at FUN_006bb2f0 + 0x6D, accepted only when the light colour call's
// return address (kRigGatherReturn, 0x006BB2B3) lies 0x43 into it as on Steam 1.67.2; else those lamps stay out of the rigs
void ResolveRigLightTest() {
    kRigLightTest = 0;
    const uintptr_t site = kRoomGather + 0x6D;
    BYTE call[5] = {};
    if (!kRoomGather || !kRigGatherReturn || !MemPatch::ReadBytes(site, call, sizeof call) || call[0] != 0xE8) return;
    int32_t rel = 0;
    std::memcpy(&rel, call + 1, 4);
    const uintptr_t target = site + 5 + static_cast<uintptr_t>(static_cast<intptr_t>(rel));
    if (target + 0x43 == kRigGatherReturn) kRigLightTest = target;
    LOG_INFO(kRigLightTest ? std::format("[ObjectLightBridge] Outdoor lamps through openings: the per-light test at 0x{:08X}", kRigLightTest)
                           : std::string("[ObjectLightBridge] Outdoor lamps through openings: the per-light test differs; those lamps stay out of the objects' light"));
}
bool InstallRoomGatherPatch() {
    std::string missing;
    if (!GameAddr::Have({GameAddr::Id::RoomGatherCall, GameAddr::Id::RoomGather, GameAddr::Id::CellGather, GameAddr::Id::RigUpdate, GameAddr::Id::RigVtable}, &missing)) {
        LOG_WARNING("[ObjectLightBridge] Fenced areas: " + GameAddr::NotAvailable(missing));
        return false;
    }
    if (*reinterpret_cast<const BYTE*>(kRoomGatherCall) != 0xE8 || kRoomGatherCall + 5 + *reinterpret_cast<const int32_t*>(kRoomGatherCall + 1) != kRoomGather ||
        !CellGatherReturns4())
        return false;
    DWORD orig = static_cast<DWORD>(kRoomGather - (kRoomGatherCall + 5));
    const DWORD rel = static_cast<DWORD>(reinterpret_cast<uintptr_t>(&RoomGatherThunk) - (kRoomGatherCall + 5));
    if (!MemPatch::WriteDWORD(kRoomGatherCall + 1, rel, &g_roomPatches, &orig)) return false;
    ResolveRigLightTest();
    return true;
}

bool InstallRigCtorPatch() {
    std::string missing;
    if (!GameAddr::Have({GameAddr::Id::RigCtor, GameAddr::Id::RigCtorCall0, GameAddr::Id::RigCtorCall1, GameAddr::Id::RigCtorCall2}, &missing)) {
        LOG_WARNING("[ObjectLightBridge] Object light creation: " + GameAddr::NotAvailable(missing));
        return false;
    }
    for (uintptr_t site : kRigCtorCalls) {
        if (*reinterpret_cast<const BYTE*>(site) != 0xE8 ||
            site + 5 + *reinterpret_cast<const int32_t*>(site + 1) != kRigCtor) return false;
    }
    for (uintptr_t site : kRigCtorCalls) {
        DWORD orig = static_cast<DWORD>(kRigCtor - (site + 5));
        const DWORD rel = static_cast<DWORD>(reinterpret_cast<uintptr_t>(&RigCtorForce) - (site + 5));
        if (!MemPatch::WriteDWORD(site + 1, rel, &g_rigPatches, &orig)) {
            MemPatch::RestoreAll(g_rigPatches);
            g_rigPatches.clear();
            return false;
        }
    }
    return true;
}

// All 9 light classes (light_vtables.txt listed 7; 0xFF4408 CircleWindowLight and 0xFF4468 TubeLight, both made by the
// factory FUN_006ac590, were missing) answer "colour of this light at a point" with vfunc+0x10, all in
// the same record format (FUN_006bdb00). The street lamp class got the ground-footprint boost first; lot lamps (the
// other classes) kept the 1/d2 falloff, so objects on lots (fences, plants, mailboxes) got ~0.1 or fell under the 0.1
// luminance cut (captures m32-m41). Every class goes through the boost now, but only lit, outdoor (room 0) lamps of
// lamp types (3..6; street lamps 0xB), so interiors are unchanged.
struct LightClass {
    uintptr_t slot; // vtable + 0x10
    uintptr_t orig;
};
// Steam: {0x00FF42B0, 0x006C02A0}, {0x00FF4308, 0x006C02A0}, {0x00FF4360, 0x006C0690}, {0x00FF43B8, 0x006C0AF0},
// {0x00FF44D0, 0x006C16D0}, {0x00FF4528, 0x006C1980}, {0x00FF4580, 0x006C1BC0}, {0x00FF4418, 0x006C0FE0}, {0x00FF4478, 0x006C1320}.
// Light types of those classes (the light factory FUN_006ac590 makes type N with the vtable GameAddr::LightVtableN):
constexpr int kClassTypes[] = {3, 11, 5, 7, 9, 10, 4, 8, 6};
LightClass kClasses[std::size(kClassTypes)] = {}; // set by LoadAddresses; {0, 0} = class not found on this build
constexpr int kStreetClass = 1; // vtable 0xFF42F8 (type 11)
std::atomic<int> g_classesPatched{0};
// Cone lamps brighten objects only where their cones reach (light reached the plants far off the cone). An F7
// capture of a flower bed: instanced foliage (VS 285429B8, wrap-lit, per-instance lamp colours c54..c56), instance 0
// c55 = (0.594, 0, 0) = the red wall sconce #4140 (type 5, cones of ~40 degrees around -Y and +Y, 6.4 m over the garden) boosted
// below: 0.75 x (1 - d^2 / R^2)^2, R = half the light's rect = sqrt(range / 0.0392 [0x00FF45C4 via 0x011D1180]) = 34.6 m for
// range 47, i.e. d = 11.5 m, far outside both cones. The game's own record has the cone (0x006C0690 -> FUN_006BC940); the flat
// footprint replaced it (luma 0.21 x red, cut 0.1: kept up to ~15.6 m, HD objects ~30 m). The footprint is now scaled by the
// cone's share towards the object, by the game's laws: type 5 FUN_006BC940 (t = (a . u - o) x s, s = [0x011D11A0]; 1 at t >= 1;
// at t <= 0 only the spill, so no boost and the game's record stays) and type 4 FUN_006BDA10 (sat((a . u - o) x scale)); u = the
// unit vector from the lamp's head to the object's centre.
constexpr bool kBoostInsideCones = true;
constexpr bool kBoostConeSpots = true; // type 4 too: objects beside a spot's beam lose the boost
std::atomic<long> g_coneOutside{0}, g_coneEdge{0};
bool g_coneLogged = false;    // render thread (OnPresent)
uintptr_t kTypeFiveScale = 0; // 0x011D11A0 on fixed Steam addresses (5 in every F8), else 5
float BoostConeShare(const BYTE* L, int type, const float* pos) { // reads the lamp: called inside BoostRec's __try
    if (type != 5 && !(kBoostConeSpots && type == 4)) return 1.0f;
    const float* head = reinterpret_cast<const float*>(L + 0x120);
    float u[3] = {pos[0] - head[0], pos[1] - head[1], pos[2] - head[2]};
    const float len = ApexMath::Sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
    if (!(len > 1e-3f)) return 1.0f;
    u[0] /= len;
    u[1] /= len;
    u[2] /= len;
    if (type == 4) {
        const float* a = reinterpret_cast<const float*>(L + 0x170);
        const float t = (a[0] * u[0] + a[1] * u[1] + a[2] * u[2] - *reinterpret_cast<const float*>(L + 0x158)) * *reinterpret_cast<const float*>(L + 0x154);
        return std::clamp(t, 0.0f, 1.0f);
    }
    float k = 5.0f;
    if (kTypeFiveScale) {
        const float v = *reinterpret_cast<const float*>(kTypeFiveScale);
        if (v > 0.0f && v < 100.0f) k = v;
    }
    const float* a1 = reinterpret_cast<const float*>(L + 0x1A0);
    const float* a2 = reinterpret_cast<const float*>(L + 0x190);
    const float t1 = (a1[0] * u[0] + a1[1] * u[1] + a1[2] * u[2] - *reinterpret_cast<const float*>(L + 0x174)) * k;
    const float t2 = (a2[0] * u[0] + a2[1] * u[1] + a2[2] * u[2] - *reinterpret_cast<const float*>(L + 0x170)) * k;
    if (t1 >= 1.0f || t2 >= 1.0f) return 1.0f;
    if (t1 <= 0.0f && t2 <= 0.0f) return 0.0f;
    return t1 > 0.0f ? t1 : t2; // FUN_006BC940's lerp parameter (it takes t1 when t1 is above 0)
}

void BoostRec(BYTE* L, const float* pos, float* rec, bool street) {
    __try {
        const BYTE f = L[0x100];
        // lamp off: lit 0x20 and enabled 0x40, the terrain bake's own test (LotLightBridge InBake; a lit lamp a player
        // disabled kept its boost on objects while the ground lost it)
        if (!(f & 0x20) || (!(f & 0x40) && TesterToggles::disabledLampFix.load(std::memory_order_relaxed))) return;
        const int type = *reinterpret_cast<const int*>(L + 0xB0);
        if (!street) {
            if (!(f & 0x04) || *reinterpret_cast<const int*>(L + 0x08) != 0) return; // outdoor lamps only
            if (type < 3 || type > 6) return;
        }
        const float* head = reinterpret_cast<const float*>(L + 0x120);
        const float* bounds = reinterpret_cast<const float*>(L + 0x134); // minX, minZ, maxX, maxZ
        const float radius = 0.5f * (bounds[2] - bounds[0]);
        if (!(radius > 0.5f && radius < 100.0f)) return;
        const float dx = head[0] - pos[0], dz = head[2] - pos[2];
        float w = 1.0f - (dx * dx + dz * dz) / (radius * radius);
        if (w <= 0.0f) return;
        w *= w;
        if (kBoostInsideCones && !street) { // a cone lamp brightens only what its cones reach (BoostConeShare)
            const float cone = BoostConeShare(L, type, pos);
            if (!(cone > 0.0f)) {
                g_coneOutside.fetch_add(1, std::memory_order_relaxed);
                return; // outside both cones: the game's own record (with its spill) stays
            }
            if (cone < 1.0f) {
                w *= cone;
                g_coneEdge.fetch_add(1, std::memory_order_relaxed);
            }
        }
        const float intensity = *reinterpret_cast<const float*>(L + 0x10);
        const float fade = *reinterpret_cast<const float*>(L + 0x20);
        const float s = g_strength * intensity * fade * w;
        const float* base = reinterpret_cast<const float*>(L + 0xF0);
        bool changed = false;
        for (int c = 0; c < 3; c++) {
            const float v = base[c] * s;
            if (v > rec[4 + c]) {
                rec[4 + c] = v;
                changed = true;
            }
        }
        if (changed) {
            const float* luma = reinterpret_cast<const float*>(kLumaWeights);
            rec[10] = rec[4] * luma[0] + rec[5] * luma[1] + rec[6] * luma[2];
            g_boosted.fetch_add(1, std::memory_order_relaxed);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// Buildings block lamp light on rig objects (13/10: cypress trees behind a manor took a garden post's light through the
// whole building, and a bush behind a free-standing wall was lit by the post in front of it). The rig gather has no wall
// test for outdoor rigs (mode 2, the world cells). After the game's colour and the boost, an outdoor lamp's record (a street
// lamp, or a lit outdoor lamp of room 0, types 3..6) is scaled by the share of the lamp that reaches the rig's centre
// (LevelLightShare::LampVisibilityShared: the outside walls, and the slabs above the lamp; 1 while "Buildings block light
// on objects" is off). A blocked lamp falls under the game's 0.1 luminance cut and the next lamp takes its rig slot. The
// gather runs on more than one thread, so the test reads the immutable copy the render thread publishes.
std::atomic<long> g_shadeTests{0}, g_shadeBlocked{0}, g_shadeDimmed{0};
constexpr float kShadeNearSkip = 0.5f;
// POD only (SEH): the lamp's head when its record is an outdoor lamp worth testing
bool ShadeCandidate(const BYTE* L, const float* rec, bool street, float head[3]) {
    __try {
        if (!(rec[10] > 1e-4f)) return false; // nothing to scale (also NaN)
        const BYTE f = L[0x100];
        if (!(f & 0x20)) return false;
        if (!street) {
            const int type = *reinterpret_cast<const int*>(L + 0xB0);
            if (!(f & 0x04) || *reinterpret_cast<const int*>(L + 0x08) != 0 || type < 3 || type > 6) return false;
        }
        std::memcpy(head, L + 0x120, 12);
        return ApexMath::IsFinite(head[0]) && ApexMath::IsFinite(head[1]) && ApexMath::IsFinite(head[2]);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
void ShadeRec(const BYTE* L, const float* pos, float* rec, bool street) {
    if (!LevelLightShare::ObjectWallsOn() || !TesterToggles::rigBuildingBlock.load(std::memory_order_relaxed)) return;
    float head[3];
    if (!ShadeCandidate(L, rec, street, head)) return;
    const float p[3] = {pos[0], pos[1], pos[2]};
    if (!ApexMath::IsFinite(p[0]) || !ApexMath::IsFinite(p[1]) || !ApexMath::IsFinite(p[2])) return;
    g_shadeTests.fetch_add(1, std::memory_order_relaxed);
    // a wall crossed within 0.5 m of the centre is the wall the object is set in or stands against (doors, windows, wall
    // decor; a turned window's centre is half a tile inside its wall): it does not block
    const float v = LevelLightShare::LampVisibilityShared(head, p, kShadeNearSkip);
    if (!(v < 1.0f)) return;
    for (int c = 4; c <= 6; c++) rec[c] *= v;
    rec[10] *= v;
    (v <= 0.0f ? g_shadeBlocked : g_shadeDimmed).fetch_add(1, std::memory_order_relaxed);
}

// An outdoor lamp taken through a roofed room's openings (OpeningLamps, this thread): its share past the walls of its story
// through the openings' holes replaces the outside walls' test (which knows no opening); applied whatever "Buildings block
// light on objects" is, as the room's own maps apply it
void OpeningShade(float* rec, float share) {
    if (!(share < 1.0f)) return;
    const float v = std::max(share, 0.0f);
    for (int c = 4; c <= 6; c++) rec[c] *= v;
    rec[10] *= v;
}
// A lamp taken through a roofed room's openings (13/10, in game: the columns inside an arcade, lit by a garden post through
// its arches, were almost white while the columns and the facade facing the post stayed at its realistic light): the record
// had the game's colour raised by the ground footprint's boost (BoostRec: about the lamp's full colour within half its
// bounds), while the outdoor objects take the per-pixel lamps' law. Its colour is now that law at the rig's centre
// (LotLightBridge::ObjectLampColourAt: the realistic reach law, the walls' scale and the slider, the daytime curve, the
// cones), times its share through the openings; the record keeps the game's direction. False when that law is not in use
// (per-pixel lamps off): the boost stays, as on the outdoor objects then. Only an outdoor lamp (room 0's, a street lamp): a
// roofed neighbour's lamp keeps the game's colour, as on the objects of its own room.
bool OutdoorRoomLamp(const void* L) { // POD only (SEH)
    __try {
        return *reinterpret_cast<const int*>(static_cast<const BYTE*>(L) + 0x08) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool OpeningLawRec(const void* L, const float* pos, float* rec) {
    if (!OutdoorRoomLamp(L)) return false;
    float rgb[3];
    if (!LotLightBridge::ObjectLampColourAt(L, pos, rgb)) return false;
    for (int c = 0; c < 3; c++) rec[4 + c] = rgb[c];
    const float* luma = reinterpret_cast<const float*>(kLumaWeights);
    rec[10] = rec[4] * luma[0] + rec[5] * luma[1] + rec[6] * luma[2];
    g_openLaw.fetch_add(1, std::memory_order_relaxed);
    return true;
}
// Outdoor lamps on rigs by the realistic reach law (2.13.1, trees and bushes at night lit almost evenly by a garden post): with
// the per-pixel lamps and Realistic lamp reach on, an outdoor lamp's record (the lamps BoostRec raises: a street lamp, or a
// lit and enabled room-0 lamp of type 3..6) is the per-pixel lamps' law at the rig's centre (ObjectLampColourAt: the outdoor
// walls' law and scale, the daytime curve, the cones, at most 1 per channel), as the outdoor objects, fences and the lamps
// through openings take it, instead of the ground footprint's boost (about the lamp's whole colour x the object strength
// anywhere within half the lamp's bounds, horizontal distance only). The game's direction is kept. Foliage is drawn by its
// rig (per-instance colours c54..c56, foliage.md), so a bush 2 m from a post now differs from a tree 9 m away by the walls'
// law. Outdoor objects that draw per-pixel lamps zero their rig (DrawObjectLamp) and are unchanged. ShadeRec follows.
std::atomic<bool> g_reachLawOn{false}; // ObjectReachLawScale() >= 0, refreshed by OnPresent
std::atomic<long> g_reachLaw{0};
float g_reachLawScale = -1.0f;         // render thread: the scale the rigs were gathered with
bool ReachLawCandidate(const BYTE* L, bool street) { // POD only (SEH): BoostRec's lamp rule
    __try {
        const BYTE f = L[0x100];
        if (!(f & 0x20) || (!(f & 0x40) && TesterToggles::disabledLampFix.load(std::memory_order_relaxed))) return false;
        if (street) return true;
        const int type = *reinterpret_cast<const int*>(L + 0xB0);
        return (f & 0x04) && *reinterpret_cast<const int*>(L + 0x08) == 0 && type >= 3 && type <= 6;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// (15/10, the player's report: a big tree next to a post took no light, its centre being metres from the lamp while its
// canopy touched it) The rig keeps its model's box (rig+0x150 min, +0x160 max, local) and world rows (+0x170 x, +0x180 y,
// +0x190 z, +0x1A0 translation), from which FUN_006B76B0 makes the centre +0x140 (Steam 1.67.2, dumpbin). When pos is that
// centre: the box's world half extents, sum |row_i| x half size_i, so the lamp is evaluated at the box's point nearest its
// head. False (the centre, as before) when pos is not a rig's centre or the box is unreadable. POD only (SEH).
bool RigBoxExtents(const float* pos, float e[3]) {
    __try {
        const BYTE* rig = reinterpret_cast<const BYTE*>(pos) - 0x140;
        if (!kRigVtable || *reinterpret_cast<const uint32_t*>(rig) != kRigVtable) return false;
        const float* lo = reinterpret_cast<const float*>(rig + 0x150);
        const float* hi = reinterpret_cast<const float*>(rig + 0x160);
        e[0] = e[1] = e[2] = 0.0f;
        for (int i = 0; i < 3; i++) {
            const float* row = reinterpret_cast<const float*>(rig + 0x170 + 0x10 * i);
            const float h = 0.5f * (hi[i] - lo[i]);
            if (!(h >= 0.0f && h < 200.0f)) return false;
            for (int k = 0; k < 3; k++) e[k] += ApexMath::Fabs(row[k]) * h;
        }
        return ApexMath::IsFinite(e[0]) && ApexMath::IsFinite(e[1]) && ApexMath::IsFinite(e[2]) && e[0] < 400.0f && e[1] < 400.0f && e[2] < 400.0f;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool ReachLawRec(void* L, const float* pos, float* rec, bool street) {
    if (!kLumaWeights || !ReachLawCandidate(static_cast<const BYTE*>(L), street)) return false;
    float rgb[3], ext[3];
    if (!LotLightBridge::ObjectLampColourAt(L, pos, rgb, RigBoxExtents(pos, ext) ? ext : nullptr)) return false;
    for (int c = 0; c < 3; c++) rec[4 + c] = rgb[c];
    const float* luma = reinterpret_cast<const float*>(kLumaWeights);
    rec[10] = rec[4] * luma[0] + rec[5] * luma[1] + rec[6] * luma[2];
    g_reachLaw.fetch_add(1, std::memory_order_relaxed);
    return true;
}
// A lamp through a roofed room's openings keeps its light under the game's luminance cut (14/10, F7 in game: a column inside an
// arcade, 4 m behind the arch from a garden post, drew black while the floor around it and the columns outside were lit).
// Its record is the per-pixel lamps' law at the rig's centre (OpeningLawRec), which the outdoor objects take with no cut; the
// rig test FUN_006bb270 then drops a record whose luminance rec[10] is under its cut (0x006BB2B9..0x006BB2D0: [0x01158D64]
// for a rig with +8 set, else [0x01158D60]; 0.1 per the captures m32-m41), so the column lost the lamp altogether. A record
// whose law gives at least one 8-bit level of light gets rec[10] raised to the cut: it is kept, last in the rank, with the
// law's own colour (rec[4..6] untouched). After the cut rec[10] only ranks the candidates (FUN_006bb190 copies the record,
// the sort's compare 0x006B8807 reads +0x28); the rig's colours come from rec[4..6]. UNVERIFIED: that no later step weighs
// a light by rec[10]. The two cuts from GameAddr RigCutFlagged / RigCut (0x01158D64 / 0x01158D60 on Steam 1.67.2);
// TesterToggles::openingLampCut switches it off.
void KeepOverCut(const BYTE* rig, float* rec) {
    const uintptr_t cutFlagged = GameAddr::Get(GameAddr::Id::RigCutFlagged), cutOther = GameAddr::Get(GameAddr::Id::RigCut);
    if (!rig || !cutFlagged || !cutOther || !TesterToggles::openingLampCut.load(std::memory_order_relaxed)) return;
    if (!(rec[10] >= RigCut::kMinLuma)) return; // (also NaN)
    __try {
        const bool flagged = *reinterpret_cast<const int*>(rig + 8) != 0;
        const float cut = *reinterpret_cast<const float*>(flagged ? cutFlagged : cutOther);
        const float kept = RigCut::KeptLuma(rec[10], cut); // (the game keeps it: "comiss cut, rec[10]; ja drop" is false at equality)
        if (kept == rec[10]) return;
        rec[10] = kept;
        g_openOverCut.fetch_add(1, std::memory_order_relaxed);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}
template <int I> void __fastcall ClassColour(void* L, void* edx, const float* pos, float* rec) {
    reinterpret_cast<LightColour_t>(kClasses[I].orig)(L, edx, pos, rec);
    if (_ReturnAddress() != reinterpret_cast<void*>(kRigGatherReturn)) return;
    if (t_openShade.light == L) {
        bool law = false;
        HookGuard::Run("ObjectLightBridge opening lamp law", [&] { law = OpeningLawRec(L, pos, rec); });
        if (!law) BoostRec(static_cast<BYTE*>(L), pos, rec, I == kStreetClass);
        OpeningShade(rec, t_openShade.share);
        if (law) KeepOverCut(t_openShade.rig, rec);
        return;
    }
    bool law = false;
    if (g_reachLawOn.load(std::memory_order_relaxed)) HookGuard::Run("ObjectLightBridge reach law", [&] { law = ReachLawRec(L, pos, rec, I == kStreetClass); });
    if (!law) BoostRec(static_cast<BYTE*>(L), pos, rec, I == kStreetClass);
    HookGuard::Run("ObjectLightBridge lamp visibility", [=] { ShadeRec(static_cast<const BYTE*>(L), pos, rec, I == kStreetClass); });
    // (2.15.3) a ceiling lamp of the story below listed on this story: no light through the floor (as the room solve,
    // LevelLightShare::LampUnderRoomFloor); the record's luminance under the rig's cut drops it
    float ext[3];
    if (RigBoxExtents(pos, ext) && (rec[4] > 0.0f || rec[5] > 0.0f || rec[6] > 0.0f)) {
        const uintptr_t rig = reinterpret_cast<uintptr_t>(pos) - 0x140;
        if (LevelLightShare::LampUnderRoomFloor(ObjectLightBridge::RigRoom(rig), L, pos)) rec[4] = rec[5] = rec[6] = rec[10] = 0.0f;
    }
}

const LightColour_t kClassThunks[] = {&ClassColour<0>, &ClassColour<1>, &ClassColour<2>, &ClassColour<3>, &ClassColour<4>, &ClassColour<5>, &ClassColour<6>,
                                     &ClassColour<7>, &ClassColour<8>};
static_assert(std::size(kClassThunks) == std::size(kClasses));

void DirtyAllRigs() {
    if (!kRootPtr || !kDirtyAllRigs) return; // not found on this build
    __try {
        const uintptr_t root = *reinterpret_cast<const uintptr_t*>(kRootPtr);
        if (!root) return;
        const uintptr_t lightMgr = *reinterpret_cast<const uintptr_t*>(root + 0x1C0);
        if (!lightMgr) return;
        const uintptr_t cells = *reinterpret_cast<const uintptr_t*>(lightMgr + 0x104);
        if (!cells) return;
        reinterpret_cast<DirtyAll_t>(kDirtyAllRigs)(reinterpret_cast<void*>(cells));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// ---- Lamp colour. The game's stock lamps (street lamps and lot lamps) are pink: base colour (1, 0.75, 0.79), measured
// in the light objects and in the lot light maps (G/R 0.754, B/R 0.793), with or without snow. On green grass it barely
// shows, on white snow it does. The colour reaches a light two ways:
//  - at creation, from its definition: FUN_006bda90(light, def+0x10), 7 callers (one per light type);
//  - from the lamp object's script (stock colour and colours picked in build mode): FUN_006b0b50(objId, r, g, b) ->
//    FUN_006bc3e0(light, rgba) for every light of the object (call at 0x6B0BDE).
// Both write +0xF0 (base colour) and +0xE0 (intensity * colour when lit). Both go through TintStockColour, which turns
// the stock pink into warm white (tungsten-like) with the same brightness. Other colours are left alone.
uintptr_t kSetLightColour = 0;      // 0x006BDA90
uintptr_t kSetColourCalls[7] = {};  // 0x006C047D, 0x006C051D, 0x006C05C1, 0x006C1251, 0x006C15D1, 0x006C1891, 0x006C1B11
uintptr_t kScriptSetColour = 0;     // 0x006BC3E0
uintptr_t kScriptSetColourCall = 0; // 0x006B0BDE
std::vector<MemPatch::PatchLocation> g_colourPatches;
std::atomic<float> g_lampTint{1.0f};    // street lamps (and lot lamps unless they have their own)
std::atomic<float> g_lotLampTint{1.0f}; // lot lamps (= g_lampTint when "own colour for lot lamps" is off)
std::atomic<int> g_tinted{0};

using SetColour_t = void(__thiscall*)(void* light, const float* rgb);

// Every light that got the stock pink: its pink and what was written, so a tint change can re-colour it live
// (RetintLamps). Written by the two colour thunks (game thread), read by RetintLamps (render thread = the same thread in
// this game; the mutex keeps it safe either way). Entries of freed lights are dropped by the next RetintLamps (they are
// not in the light enumeration); a light created again at the same address replaces its entry.
struct TintedLight {
    float pink[3];
    float written[3];
    bool street;
};
std::mutex g_tintedMutex;
std::unordered_map<uintptr_t, TintedLight> g_tintedLights;

bool IsStockPink(const float c[3]) {
    if (c[0] <= 0.05f) return false;
    const float g = c[1] / c[0], b = c[2] / c[0];
    return ApexMath::Fabs(g - 0.75f) < 0.03f && ApexMath::Fabs(b - 0.79f) < 0.03f;
}

// The stock pink turned towards warm white (1, 0.80, 0.62) by t, with the luminance of the original
void TintPink(const float pink[3], float t, float out[3]) {
    const float g = pink[1] / pink[0], b = pink[2] / pink[0];
    const float lumPink = 0.2126f + 0.7152f * g + 0.0722f * b;
    const float lumWarm = 0.2126f + 0.7152f * 0.80f + 0.0722f * 0.62f;
    const float k = pink[0] * lumPink / lumWarm;
    const float warm[3] = {k, 0.80f * k, 0.62f * k};
    for (int i = 0; i < 3; i++) out[i] = pink[i] + (warm[i] - pink[i]) * t;
}

float TintFor(bool street) { return (street ? g_lampTint : g_lotLampTint).load(std::memory_order_relaxed); }

// A street lamp: the street-lamp class (0xB) outside any lot (lot id +0xC0/+0xC4 = 0)
bool IsStreetLight(const BYTE* light) {
    __try {
        return *reinterpret_cast<const int*>(light + 0xB0) == 0xB && *reinterpret_cast<const uint32_t*>(light + 0xC0) == 0 &&
               *reinterpret_cast<const uint32_t*>(light + 0xC4) == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Tints c in place when it is the stock pink and records the light; any other colour forgets the light
void TintStockColour(void* light, float c[3], bool street) {
    const uintptr_t key = reinterpret_cast<uintptr_t>(light);
    if (!IsStockPink(c)) {
        std::lock_guard lock(g_tintedMutex);
        g_tintedLights.erase(key);
        return;
    }
    TintedLight rec{{c[0], c[1], c[2]}, {}, street};
    const float t = TintFor(street);
    if (t > 0.0f) {
        TintPink(rec.pink, t, c);
        g_tinted.fetch_add(1, std::memory_order_relaxed);
    }
    std::memcpy(rec.written, c, sizeof(rec.written));
    std::lock_guard lock(g_tintedMutex);
    g_tintedLights[key] = rec;
}

// 07/10, players' Runtime Error: the colour setters run inside the game's lamp code. A C++ exception in the tint (its map)
// is caught, the game's own colour is kept and the tint stays off (HookGuard); the game's setter always runs, once.
void TintOrKeep(void* light, float c[3], bool street) {
    const float game[3] = {c[0], c[1], c[2]};
    if (!HookGuard::Run("ObjectLightBridge lamp tint", [&] { TintStockColour(light, c, street); })) std::memcpy(c, game, sizeof game);
}

// Creation (one call site per light class): the class decides street or lot (site 1 = the street-lamp class 0xB; its
// lot id may not be set yet, the lamp's script colour, which comes later, decides with the lot id)
template <bool Street> void __fastcall LampColourSet(void* light, void*, const float* rgb) {
    float c[4] = {rgb[0], rgb[1], rgb[2], 0.0f}; // this setter reads only rgb
    TintOrKeep(light, c, Street);
    reinterpret_cast<SetColour_t>(kSetLightColour)(light, c);
}

void __fastcall LampColourSetScript(void* light, void*, const float* rgba) {
    // FUN_006bc3e0 reads its argument with MOVAPS: the buffer must be 16-byte aligned (the game's caller aligns its frame).
    alignas(16) float c[4] = {rgba[0], rgba[1], rgba[2], rgba[3]}; // w = r (FUN_006b0b50 passes r, g, b, r)
    TintOrKeep(light, c, IsStreetLight(static_cast<const BYTE*>(light)));
    c[3] = c[0];
    reinterpret_cast<SetColour_t>(kScriptSetColour)(light, c);
}

bool RedirectCall(uintptr_t site, uintptr_t target, const void* thunk) {
    if (*reinterpret_cast<const BYTE*>(site) != 0xE8 || site + 5 + *reinterpret_cast<const int32_t*>(site + 1) != target) return false;
    DWORD orig = static_cast<DWORD>(target - (site + 5));
    const DWORD rel = static_cast<DWORD>(reinterpret_cast<uintptr_t>(thunk) - (site + 5));
    return MemPatch::WriteDWORD(site + 1, rel, &g_colourPatches, &orig);
}

// Fills the addresses above from GameAddr once it resolved (Steam: the fixed values in the comments).
void LoadAddresses() {
    static bool loaded = false;
    if (loaded || !GameAddr::Resolved()) return;
    loaded = true;
    using GameAddr::Get;
    using GameAddr::Id;
    const uintptr_t streetVtable = Get(Id::LightVtable11);
    kVtableSlot = streetVtable ? streetVtable + 0x10 : 0;
    kOriginalFn = Get(Id::LightColour11);
    kRigGatherReturn = Get(Id::RigGatherReturn);
    kCapOperandSite = Get(Id::CapOperandSite);
    kCapGlobal = Get(Id::CapGlobal);
    const uint32_t cap = static_cast<uint32_t>(kCapGlobal);
    kCapOrig = {0xB9, static_cast<BYTE>(cap), static_cast<BYTE>(cap >> 8), static_cast<BYTE>(cap >> 16), static_cast<BYTE>(cap >> 24)};
    kLumaWeights = Get(Id::LumaWeights);
    kTypeFiveScale = Get(Id::ConeScale5); // the type-5 cone sharpness FUN_006BC940 reads (0x011D11A0 on Steam 1.67.2)
    kDirtyAllRigs = Get(Id::DirtyAllRigs);
    kRootPtr = Get(Id::RootPtr);
    kRigCtor = Get(Id::RigCtor);
    for (int i = 0; i < 3; i++) kRigCtorCalls[i] = Get(static_cast<Id>(static_cast<int>(Id::RigCtorCall0) + i));
    kRoomGatherCall = Get(Id::RoomGatherCall);
    kRoomGather = Get(Id::RoomGather);
    kCellGather = Get(Id::CellGather);
    kRigUpdate = Get(Id::RigUpdate);
    kRigVtable = static_cast<uint32_t>(Get(Id::RigVtable));
    for (size_t i = 0; i < std::size(kClassTypes); i++) {
        const int t = kClassTypes[i] - 3;
        const uintptr_t vt = Get(static_cast<Id>(static_cast<int>(Id::LightVtable3) + t));
        const uintptr_t fn = Get(static_cast<Id>(static_cast<int>(Id::LightColour3) + t));
        kClasses[i] = vt && fn ? LightClass{vt + 0x10, fn} : LightClass{0, 0};
    }
    kSetLightColour = Get(Id::SetLightColour);
    for (int i = 0; i < 7; i++) kSetColourCalls[i] = Get(static_cast<Id>(static_cast<int>(Id::SetColourCall0) + i));
    kScriptSetColour = Get(Id::ScriptSetColour);
    kScriptSetColourCall = Get(Id::ScriptSetColourCall);
}

// The return address follows "call reg" (FF D0..FF D7): FF D2 (call edx) on Steam
bool AfterCallReg(uintptr_t ret) {
    const BYTE* p = reinterpret_cast<const BYTE*>(ret - 2);
    return GameAddr::IsFixed() ? std::memcmp(p, "\xFF\xD2", 2) == 0 : (p[0] == 0xFF && (p[1] & 0xF8) == 0xD0);
}

} // namespace

namespace ObjectLightBridge {

bool InstallLampColour() {
    if (!g_colourPatches.empty()) return true;
    LoadAddresses();
    std::string missing;
    if (!GameAddr::Have({GameAddr::Id::SetLightColour, GameAddr::Id::SetColourCall0, GameAddr::Id::SetColourCall1, GameAddr::Id::SetColourCall2, GameAddr::Id::SetColourCall3,
                         GameAddr::Id::SetColourCall4, GameAddr::Id::SetColourCall5, GameAddr::Id::SetColourCall6, GameAddr::Id::ScriptSetColour, GameAddr::Id::ScriptSetColourCall},
                        &missing)) {
        LOG_WARNING("[ObjectLightBridge] Lamp colour: " + GameAddr::NotAvailable(missing));
        return false;
    }
    bool ok = true;
    for (int i = 0; i < 7; i++) // site 1 (0x006C051D) is the street-lamp class's constructor
        ok = ok && RedirectCall(kSetColourCalls[i], kSetLightColour, i == 1 ? reinterpret_cast<const void*>(&LampColourSet<true>) : reinterpret_cast<const void*>(&LampColourSet<false>));
    ok = ok && RedirectCall(kScriptSetColourCall, kScriptSetColour, reinterpret_cast<const void*>(&LampColourSetScript));
    if (!ok) {
        MemPatch::RestoreAll(g_colourPatches);
        g_colourPatches.clear();
        LOG_WARNING("[ObjectLightBridge] Lamp colour: call site differs");
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    LOG_INFO("[ObjectLightBridge] Lamp colour: installed (creation + script)");
    return true;
}

void UninstallLampColour() {
    if (g_colourPatches.empty()) return;
    MemPatch::RestoreAll(g_colourPatches);
    g_colourPatches.clear();
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
}

void SetLampTint(float street, float lot) {
    g_lampTint = street < 0 ? 0.0f : (street > 1 ? 1.0f : street);
    g_lotLampTint = lot < 0 ? 0.0f : (lot > 1 ? 1.0f : lot);
}

} // namespace ObjectLightBridge

namespace {
// One tracked light with its current tint: -1 = not ours any more (another colour, or a light at a reused address),
// 0 = already right, 1 = re-coloured (rec.written updated). No C++ objects here (SEH).
int RetintLight(uintptr_t L, TintedLight& rec) {
    __try {
        if (std::memcmp(reinterpret_cast<const void*>(L + 0xF0), rec.written, sizeof(rec.written)) != 0) return -1;
        alignas(16) float c[4];
        const float t = TintFor(rec.street);
        if (t > 0.0f) TintPink(rec.pink, t, c);
        else std::memcpy(c, rec.pink, sizeof(rec.pink));
        c[3] = c[0];
        if (std::memcmp(c, rec.written, sizeof(rec.written)) == 0) return 0;
        reinterpret_cast<SetColour_t>(kScriptSetColour)(reinterpret_cast<void*>(L), c); // +0xF0, and +0xE0 when lit
        std::memcpy(rec.written, c, sizeof(rec.written));
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
} // namespace

namespace ObjectLightBridge {

int RetintLamps(const std::vector<uintptr_t>& lights) {
    if (g_colourPatches.empty() || !kScriptSetColour) return 0;
    std::lock_guard lock(g_tintedMutex);
    std::unordered_map<uintptr_t, TintedLight> alive;
    alive.reserve(g_tintedLights.size());
    int changed = 0;
    for (uintptr_t L : lights) {
        auto it = g_tintedLights.find(L);
        if (it == g_tintedLights.end()) continue;
        TintedLight rec = it->second;
        const int r = RetintLight(L, rec);
        if (r < 0) continue;
        changed += r;
        alive.emplace(L, rec);
    }
    g_tintedLights.swap(alive);
    if (changed > 0) {
        g_refreshRequested = true; // the rigs gather the new colour (next OnPresent)
        LOG_INFO(std::format("[ObjectLightBridge] Lamp colour changed live: {} lights re-coloured (street {:.2f}, lot {:.2f})", changed, g_lampTint.load(),
                             g_lotLampTint.load()));
    }
    return changed;
}

std::string LampColourStatus() {
    std::lock_guard lock(g_tintedMutex);
    return std::format("{} | lights with corrected colour: {} | stock lamps tracked: {}", g_colourPatches.empty() ? "off" : "active", g_tinted.load(),
                       g_tintedLights.size());
}

bool Install(std::string& error) {
    if (g_installed) return true;
    LoadAddresses();
    std::string missing;
    if (!GameAddr::Have({GameAddr::Id::LightVtable11, GameAddr::Id::LightColour11, GameAddr::Id::RigGatherReturn, GameAddr::Id::CapOperandSite, GameAddr::Id::CapGlobal,
                         GameAddr::Id::LumaWeights, GameAddr::Id::DirtyAllRigs, GameAddr::Id::RootPtr},
                        &missing)) {
        error = "Objects: " + GameAddr::NotAvailable(missing);
        return false;
    }
    if (*reinterpret_cast<const uint32_t*>(kVtableSlot) != kOriginalFn) {
        error = std::format("Street lamp light function differs at 0x{:X}", kVtableSlot);
        return false;
    }
    if (!MemPatch::ValidateBytes(reinterpret_cast<LPVOID>(kCapOperandSite), kCapOrig.data(), kCapOrig.size()) ||
        (GameAddr::IsFixed() && std::memcmp(reinterpret_cast<const void*>(kDirtyAllRigs), kDirtyAllBytes, sizeof(kDirtyAllBytes)) != 0) ||
        !AfterCallReg(kRigGatherReturn)) {
        error = "Object light code differs (different game version?)";
        return false;
    }
    g_capScaled = *reinterpret_cast<const float*>(kCapGlobal);
    const DWORD capAddr = static_cast<DWORD>(reinterpret_cast<uintptr_t>(&g_capScaled));
    std::vector<BYTE> capBytes = {0xB9, 0, 0, 0, 0};
    std::memcpy(capBytes.data() + 1, &capAddr, 4);
    if (!MemPatch::WriteBytes(kCapOperandSite, capBytes, &g_patches, &kCapOrig)) {
        MemPatch::RestoreAll(g_patches);
        error = "Could not patch the object light";
        return false;
    }
    int classes = 0;
    for (int i = 0; i < static_cast<int>(std::size(kClasses)); i++) {
        if (!kClasses[i].slot) continue; // class not found on this build
        if (*reinterpret_cast<const uint32_t*>(kClasses[i].slot) != kClasses[i].orig) continue; // not the expected function: leave it
        DWORD orig = static_cast<DWORD>(kClasses[i].orig);
        if (MemPatch::WriteDWORD(kClasses[i].slot, static_cast<DWORD>(reinterpret_cast<uintptr_t>(kClassThunks[i])), &g_patches, &orig)) classes++;
    }
    g_classesPatched = classes;
    LOG_INFO(std::format("[ObjectLightBridge] Light classes boosted: {} of {}", classes, std::size(kClasses)));
    if (!InstallRigCtorPatch()) LOG_WARNING("[ObjectLightBridge] Object light creation differs; stairs and railings stay without lamps");
    if (!InstallRoomGatherPatch()) LOG_WARNING("[ObjectLightBridge] Light gathering in fenced areas differs; fences of enclosed areas stay without street lamps");
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    g_installed = true;
    g_refreshRequested = true;
    LOG_INFO("[ObjectLightBridge] Installed");
    return true;
}

void Uninstall() {
    if (!g_installed) return;
    MemPatch::RestoreAll(g_patches);
    g_patches.clear();
    MemPatch::RestoreAll(g_rigPatches);
    g_rigPatches.clear();
    MemPatch::RestoreAll(g_roomPatches);
    g_roomPatches.clear();
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    g_installed = false;
    if (GetCurrentThreadId() == g_renderThread.load()) DirtyAllRigs();
    else g_refreshRequested = true; // done by the next OnPresent on the render thread
    LOG_INFO("[ObjectLightBridge] Uninstalled");
}

void SetStrength(float s) {
    if (s != g_strength) {
        g_strength = s;
        g_refreshRequested = true;
    }
}

void LogConeOnce() {
    g_coneLogged = true;
    LOG_INFO(std::format("[ObjectLightBridge] Cone lamps: objects outside a lamp's cones keep the game's own colour (no boost; {} so far)", g_coneOutside.load()));
}

void OnPresent() {
    g_renderThread = GetCurrentThreadId();
    LoadAddresses();
    if (!g_installed) {
        if (g_refreshRequested.exchange(false)) DirtyAllRigs();
        return;
    }
    __try {
        g_capScaled = *reinterpret_cast<const float*>(kCapGlobal) * std::max(1.0f, g_strength); // original cap unless the user raises the strength
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    // the reach law on rigs (ReachLawRec): gather again when it is turned on or off, or its scale (night, sliders) moved by 2%
    const float lawScale = LotLightBridge::ObjectReachLawScale();
    g_reachLawOn.store(lawScale >= 0.0f, std::memory_order_relaxed);
    if (TerrainLightingPolicy::RigLawChanged(g_reachLawScale, lawScale)) {
        g_reachLawScale = lawScale;
        g_refreshRequested = true;
    }
    if (g_refreshRequested.exchange(false)) {
        DirtyAllRigs();
        g_regatherLevel = -1.0f; // also the room-mode rigs
        RegatherCrossRigs();     // and those near a lamp of another story (the game never marks them for it)
    }
    RegatherOpeningRooms(); // and those of the rooms whose outdoor lamps through openings changed
    UpdateRoomRigs();
    if (kBoostInsideCones && !g_coneLogged && g_coneOutside.load(std::memory_order_relaxed) > 0) LogConeOnce(); // (a string: kept out of this __try function)
}

void SetAllObjects(bool on) { g_forceAll = on; }

void RequestRigRefresh() { g_refreshRequested = true; }
bool RigRefreshPending() { return g_refreshRequested.load(); }

std::string RigOpeningNote(uintptr_t rig) {
    if (!rig) return std::string();
    RigOpenNote n;
    {
        std::lock_guard<std::mutex> lk(g_rigOpenMx);
        const auto it = g_rigOpen.find(rig);
        if (it == g_rigOpen.end()) return std::string();
        n = it->second;
    }
    std::string s = std::format(" | outdoor lamps through the room's openings at the rig's last gather ({:.1f} s ago):", (GetTickCount() - n.tick) / 1000.0);
    for (int i = 0; i < n.n; i++) s += std::format(" L{:08X} share {:.2f} {}", n.light[i], n.share[i], n.kept[i] ? "kept" : "under the cut");
    return s;
}

const void* RigRoom(uintptr_t rig) {
    if (!rig) return nullptr;
    // (2.15.3) the last answer on this thread while g_rigRoom did not change (asked on every furniture draw at night; the
    // game draws a rig's parts one after another): no lock and no lookup
    thread_local uintptr_t t_rig = 0;
    thread_local const void* t_room = nullptr;
    thread_local uint32_t t_gen = 0;
    const uint32_t gen = g_rigRoomGen.load(std::memory_order_acquire); // (before the lookup: a change after it makes the answer stale)
    if (rig == t_rig && gen == t_gen) return t_room;
    std::lock_guard<std::mutex> lk(g_rigRoomMx);
    const auto it = g_rigRoom.find(rig);
    const void* room = it == g_rigRoom.end() ? nullptr : it->second;
    t_rig = rig, t_room = room, t_gen = gen;
    return room;
}

std::string Status() {
    return std::format("{} | light classes: {}/{} | lights boosted on objects: {} | objects opened to lamps (stairs, railings...): {}{} | in fenced areas: {} | "
                       "indoor objects given lamps of another story: {} ({} lamps offered) | indoor objects given outdoor lamps through openings: {} ({} lamps "
                       "offered, {} kept, {} by the per-pixel lamps' law, {} kept under the game's cut{}) | outdoor lamps on rigs by the reach law: {} | outdoor lamps behind buildings: tested {}, left out {}, dimmed {}",
                       g_installed ? "Active" : "Off", g_classesPatched.load(), std::size(kClasses), g_boosted.load(), g_forcedRigs.load(),
                       g_rigPatches.empty() && g_installed ? " (not installed)" : "", g_roomRigs.load(), g_crossRigs.load(), g_crossLamps.load(), g_openRigs.load(),
                       g_openLamps.load(), g_openKept.load(), g_openLaw.load(), g_openOverCut.load(), kRigLightTest || !g_installed ? "" : "; the per-light test differs: off", g_reachLaw.load(), g_shadeTests.load(),
                       g_shadeBlocked.load(), g_shadeDimmed.load()) +
           " (" + LevelLightShare::SharedVisibilityStatus() + ")";
}

} // namespace ObjectLightBridge
