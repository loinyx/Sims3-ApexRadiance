// Rooms with every lamp off (part of Night Lighting, 2026-09-29).
//
// A room whose light list is empty gets a fixed ambient colour instead of light from lamps: FUN_006a0f50 (the room's
// ambient, state 0 of the room solve) sets room+0x110 and +0x120 to the vector at 0x011D0B60 or 0x011D0B40 (which one
// depends on the lot: FUN_00c63140 tests the lot's type and its byte +0x430) with normalisation 1. A room whose lamps
// give less light than that colour is topped up to it (FUN_006a00a0, twice at the end of FUN_006a0f50: under 0.0005 the
// colour alone, else the lamps plus the shortfall). Walls and floors show it as light map alpha x that ambient, so every
// unlit room glows with it: blue (0.15, 0.15, 0.30 by default, the numbers Sims3SettingsSetter's "Brady Bunch BEGONE"
// shows). The object rigs of rooms get a fill light too (FUN_006b7e70, called from FUN_006ba340 while the byte at
// 0x01158D5C is set, for room-mode rigs and rigs with flag 0x20): opposite the main light, the colour at 0x011D0E10
// (0.8, 0.8, 1.0, 0.8, set once from the constants 0.8 and 1.0) times the rig's light.
//
// The 6 reads (4 "mov ecx, imm32" of the two colours, the gate byte's "cmp", the fill colour's "movaps") point to Apex's
// own values instead: the unlit-room colour scaled ("Light left") and moved towards a grey of the same luminance ("Blue
// tint"), the fill colour and its gate stay the game's (furniture is turned per draw instead: FurnitureColour, since 30/09). The
// game's colours are read through the pointers the code had (another mod may have changed them; all zero = the game's
// defaults).
//
// Applying a change (user 29/09: "it takes forever to apply; the controls do not respond"; the first build relit every room
// of every lot, one room at a time through the game's queue). The game hands the shader a POINTER to room+0x110 when it
// draws a room (0x0069EB70 / 0x006A06BF: the "InteriorBuildingAmbientColor" parameter, id [0x01158AEC], takes room+0x110),
// and a room lit by the unlit colour alone holds exactly that colour there (and at +0x120). So every room whose +0x110 is
// one of the colours Apex (or the game) set is given the new colour directly: walls and floors follow at the next frame,
// while a slider moves. Only rooms whose lamps were topped up (lamps plus the shortfall, FUN_006a00a0) need a new solve:
// they are sent once the slider rests. The object rigs gather again after each (at most 4 per second while dragging).
#include "unlit_rooms.h"
#include "game_addresses.h"
#include "memory_patch.h"
#include "apex_log.h"
#include "level_light_share.h"
#include "object_light_bridge.h"
#include "room_ambient_policy.h"
#include "s3ss_detect.h"
#include <windows.h>
#include <cmath>
#include <cstring>
#include <format>
#include <vector>
#include <atomic>
#include <mutex>
#include <unordered_map>

namespace {

struct alignas(16) Vec4 {
    float v[4];
};
Vec4 g_colour[2];               // Apex's unlit-room colours: [0] for the code's first pointer (0x011D0B60), [1] the second
Vec4 g_fill;                    // Apex's fill light colour
volatile BYTE g_fillGate = 1;   // Apex's gate byte
Vec4 g_base[2];                 // the game's unlit-room colours (read through the original pointers)
bool g_baseDefault[2] = {};     // the default was used (the game's vector was all zero or unreadable)
S3SSDetect::RoomAmbientCorrection g_compat;
constexpr float kDefaultColour[4] = {0.15f, 0.15f, 0.30f, 0.0f};
constexpr float kGameFill[4] = {0.8f, 0.8f, 1.0f, 0.8f}; // FUN_006b7e70: (0x00F9D514, 0x00F9D514, 1.0, 0x00F9D514)

enum Site { ColourA, ColourB, DimA, DimB, Gate, Fill, kSites };
uintptr_t g_site[kSites] = {};
DWORD g_orig[kSites] = {}; // what each read pointed to when Apex started
std::vector<MemPatch::PatchLocation> g_patches;
bool g_ready = false, g_patched = false;
bool g_on = false;
float g_light = 1.0f, g_blue = 1.0f;
// Furniture (30/09, second multi-agent study): its unlit-room light is the game's [NoLight] rig lights (CustomLightRigging.ini,
// three fixed lights FUN_006bb3e0 adds to room-mode rigs, x0.21 in the captured rooms) plus the fill light (FUN_006b7e70,
// (0.8, 0.8, 1.0)); the ambient cube (CASDiffuseProbe) is flat grey. Brightness and Blue tint turn it exactly as the walls
// (30/09, user: "the furniture brightness and the brightness itself get in each other's way, they should be one thing": the
// "On furniture" slider, how far furniture followed the Brightness, was removed; at 38.5% furniture kept x0.65 of the
// game's light in rooms whose walls kept x0.09).
// Both follow the night level (0 = day, 1 = night; set every frame by Night Lighting): by day nothing changes.
float g_night = 0.0f;
// Set per draw by the furniture guard (render thread): the object's rig holds [NoLight] lights, which the game adds only
// to a dark room (FUN_006bb3e0: sum of the gathered lights below [0x11D0BB8]), so the room is dark whatever the clock
// says (user 30/09: "sometimes the brightness stops working on the objects" at dawn / by day, while walls follow it)
bool g_drawDark = false;
float NightNow() { return g_drawDark ? 1.0f : std::fmin(std::fmax(g_night, 0.0f), 1.0f); }
// The same linear brightness controls room ambient and furniture's background light.
float FurnitureShareNow() { return RoomAmbientPolicy::BackgroundShare(g_light, NightNow()); }
float FurnitureTintNow() { return 1.0f + (g_blue - 1.0f) * NightNow(); }
bool g_relightDue = false;
// The colours rooms may hold for each of the two slots: the game's and those Apex set (newest last)
std::vector<Vec4> g_known[2];
Vec4 g_target[2];                  // what rooms should hold now (Apex's colours when on, the game's when off)
bool g_retintDue = false, g_restDue = false;
DWORD g_lastRetint = 0, g_lastRig = 0, g_rigAgainAt = 0;
long g_retinted = 0, g_dimSent = 0;
DWORD g_changedAt = 0, g_nextBaseCheck = 0;
DWORD g_nextReconcile = 0;
bool g_retryRetint = false;
struct RetintSent { uintptr_t mgr; int id; };
std::unordered_map<uintptr_t, RetintSent> g_retintSent; // render thread, once per target and room identity
long g_relights = 0;

float Luma(const float* c) { return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2]; }

// The game's colour behind one of the original pointers (false: unreadable or all zero)
bool ReadColour(uintptr_t at, Vec4& out) {
    __try {
        std::memcpy(out.v, reinterpret_cast<const void*>(at), sizeof out.v);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    for (float f : out.v)
        if (!std::isfinite(f) || f < 0.0f || f > 10.0f) return false;
    return out.v[0] + out.v[1] + out.v[2] > 0.0f;
}
// True when a base colour changed
bool ReadBases() {
    bool changed = false;
    for (int i = 0; i < 2; i++) {
        Vec4 c{};
        const bool ok = ReadColour(g_orig[i], c);
        if (!ok) std::memcpy(c.v, kDefaultColour, sizeof c.v);
        if (std::memcmp(c.v, g_base[i].v, sizeof c.v) != 0 || ok == g_baseDefault[i]) changed = true;
        g_base[i] = c;
        g_baseDefault[i] = !ok;
    }
    return changed;
}

bool Same(const float* a, const float* b) {
    return RoomAmbientPolicy::SameRgb(a, b);
}
void Remember(int slot, const Vec4& c) {
    auto& v = g_known[slot];
    for (const Vec4& k : v)
        if (Same(k.v, c.v)) return;
    if (v.size() >= 64) v.erase(v.begin()); // 64: a long drag stays matched, the per-room scan stays short
    v.push_back(c);
}
// A new target: the rooms holding a known colour get it now (Retint), the others once the change rests
void Retarget() {
    g_retintSent.clear();
    g_retryRetint = true;
    for (int i = 0; i < 2; i++) {
        Remember(i, g_base[i]);
        g_target[i] = g_on ? g_colour[i] : g_base[i];
    }
    g_retintDue = g_restDue = true;
    g_changedAt = GetTickCount();
}
// ---- The unlit colour as a base under the lamps (29/09, user: "the controls should set the room's tone also with a lamp
// on, or turning that lamp off changes the whole room") ----
// FUN_006a00a0 (thiscall(room, out, lamp, char pow) ret 0xC, called twice at the end of FUN_006a0f50: 0x006A13F0 with
// pow 1, its result stored at room+0x110; 0x006A1410 with pow 0, stored at +0x120): with C = the unlit colour (slot by the
// lot, FUN_00c63140 on the WorldManager) x the scalar at 0x011D0B88, and sums over x, y, z (weights 1.0 at 0x0107A538):
//   sum(lamp) < 0.0005 (0x00FE3500): out = C;
//   else lamp' = lamp (x pow(k1 x sum, k2) / sum when pow), and out = lamp' + (sum(C) - sum(lamp')) x C when sum(lamp') < sum(C).
// With Rooms at Night on, the call goes through BaseHook: out = lamp' + C (the colour always added). lamp' comes back from
// the game's result: no top-up when sum(out) >= sum(C); else sum(out) = s(1 - sum C) + (sum C)^2 gives s = sum(lamp').
// What each room got (its C and both results) is kept, so a slider change moves every lit room at once too: the room's
// value + (new C - old C), while it still holds what BaseHook wrote (the stacked-rooms merge may have changed it: those
// rooms are solved again when the change rests).
using TopUp_t = float*(__thiscall*)(void* room, float* out, const float* lamp, int pow);
using LotTest_t = bool(__thiscall*)(void* wm, uint32_t lo, uint32_t hi);
uintptr_t g_topUp = 0, g_topUpCalls[2] = {}, g_wmPtr = 0, g_lotTest = 0, g_scalePtr = 0;
std::vector<MemPatch::PatchLocation> g_basePatches;
bool g_baseReady = false;
struct BaseRec {
    uintptr_t mgr;
    int id, slot;
    float ce[4];     // the colour added (C, scaled)
    float res[2][4]; // what was written: [0] the +0x110 result, [1] the +0x120 one
    bool has[2];
    bool discardOnQueue = false; // retire this base only after its replacement solve was accepted
};
std::mutex g_baseMx;
std::unordered_map<uintptr_t, BaseRec> g_baseRooms; // by room address
std::atomic<long> g_baseAdded{0}, g_baseMoved{0};

bool Near(const float* a, const float* b, float tol) {
    for (int k = 0; k < 4; k++)
        if (std::fabs(a[k] - b[k]) > tol) return false;
    return true;
}
// Adds the colour to the game's result; false when the room could not be read
bool AddBase(BYTE* room, float* r, int& slot, float* ce) {
    __try {
        // indoor rooms only: room 0 (the outside) and roofless rooms (+0x18, fenced yards) keep the game's value (their walls
        // take no interior ambient, and the outside must look exactly as before)
        if (*reinterpret_cast<const int*>(room + 0xC) <= 0 || *reinterpret_cast<const BYTE*>(room + 0x18)) return false;
        const uintptr_t wm = *reinterpret_cast<const uintptr_t*>(g_wmPtr);
        const bool first = reinterpret_cast<LotTest_t>(g_lotTest)(reinterpret_cast<void*>(wm), *reinterpret_cast<const uint32_t*>(room + 0x10),
                                                                 *reinterpret_cast<const uint32_t*>(room + 0x14));
        slot = first ? 0 : 1; // the code's first pointer (0x011D0B60) when the lot test is true
        const float s = *reinterpret_cast<const float*>(g_scalePtr);
        for (int k = 0; k < 4; k++) ce[k] = g_colour[slot].v[k] * s;
        const float sumC = ce[0] + ce[1] + ce[2], sumR = r[0] + r[1] + r[2];
        if (!std::isfinite(sumR) || !(sumC > 0.0f) || sumC >= 0.999f) return false;
        if (Near(r, ce, 1e-6f)) return true; // no lamp light: the colour alone, as before
        float t = 0.0f;                      // the shortfall the game added
        if (sumR < sumC) {
            const float sumL = (sumR - sumC * sumC) / (1.0f - sumC);
            t = std::fmax(sumC - sumL, 0.0f);
        }
        for (int k = 0; k < 4; k++) r[k] += (1.0f - t) * ce[k]; // lamp' + C
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool RoomKey(const BYTE* room, uintptr_t& mgr, int& id) {
    __try {
        mgr = *reinterpret_cast<const uintptr_t*>(room);
        id = *reinterpret_cast<const int*>(room + 0xC);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
void NoteBase(BYTE* room, const float* r, int slot, const float* ce, int which) {
    uintptr_t mgr = 0;
    int id = 0;
    if (!RoomKey(room, mgr, id)) return;
    std::lock_guard<std::mutex> lk(g_baseMx);
    if (g_baseRooms.size() > 16384) g_baseRooms.clear();
    BaseRec& rec = g_baseRooms[reinterpret_cast<uintptr_t>(room)];
    if (rec.mgr != mgr || rec.id != id) rec = BaseRec{mgr, id, slot, {}, {}, {false, false}};
    rec.slot = slot;
    std::memcpy(rec.ce, ce, sizeof rec.ce);
    std::memcpy(rec.res[which], r, sizeof rec.res[which]);
    rec.has[which] = true;
    rec.discardOnQueue = false; // a fresh native result must survive an older queue acknowledgement
}
template <int Which> float* __fastcall BaseHook(BYTE* room, void*, float* out, const float* lamp, int pow) {
    float* r = reinterpret_cast<TopUp_t>(g_topUp)(room, out, lamp, pow);
    if (!g_patched || !g_on || !room || !r) return r;
    int slot = 0;
    float ce[4];
    if (!AddBase(room, r, slot, ce)) return r;
    g_baseAdded.fetch_add(1, std::memory_order_relaxed);
    NoteBase(room, r, slot, ce, Which);
    return r;
}

struct VisitCtx {
    bool rest;   // the change rests: rooms that need a new solve are sent
    bool baseOn; // lit rooms carry the colour as a base (Rooms at Night on)
    float scale; // [0x011D0B88]
    int retinted, moved;
};
// A lit room given the base by BaseHook: moved to the new colour (1), sent for a new solve (2) or left (0). Caller holds g_baseMx.
int MoveBase(BYTE* room, BaseRec& rec, const VisitCtx& ctx) {
    __try {
        if (*reinterpret_cast<const uintptr_t*>(room) != rec.mgr || *reinterpret_cast<const int*>(room + 0xC) != rec.id) return 3;
        if (!ctx.baseOn) return 2; // Rooms at Night off: the game's own top-up again, by a new solve
        float ce[4];
        for (int k = 0; k < 4; k++) ce[k] = g_target[rec.slot].v[k] * ctx.scale;
        float* at[2] = {reinterpret_cast<float*>(room + 0x110), reinterpret_cast<float*>(room + 0x120)};
        bool held = true;
        for (int w = 0; w < 2; w++)
            if (rec.has[w] && !Near(at[w], rec.res[w], 1e-5f)) held = false;
        if (rec.has[0] && rec.has[1] && !Near(ce, rec.ce, 1e-7f)) {
            float next[2][4];
            for (int w = 0; w < 2; w++)
                for (int k = 0; k < 4; k++) next[w][k] = RoomAmbientPolicy::MoveBackground(rec.res[w][k], rec.ce[k], ce[k]);
            if (LevelLightShare::StageAmbientBaseChange(room, rec.res[0], next[0], rec.res[1], next[1])) {
                std::memcpy(rec.res, next, sizeof next);
                std::memcpy(rec.ce, ce, sizeof ce);
                return 1;
            }
        }
        if (!held && rec.has[0] && rec.has[1] && Near(ce, rec.ce, 1e-7f)
            && LevelLightShare::HoldsAmbientBase(room, rec.res[0], rec.res[1])) return 0;
        if (!held) { g_retryRetint = true; return ctx.rest ? 2 : 0; } // changed after BaseHook (stacked rooms merged): a new solve
        if (Near(ce, rec.ce, 1e-7f)) return 0;
        for (int w = 0; w < 2; w++) {
            if (!rec.has[w]) continue;
            for (int k = 0; k < 4; k++) rec.res[w][k] += ce[k] - rec.ce[k];
            std::memcpy(at[w], rec.res[w], sizeof rec.res[w]);
        }
        std::memcpy(rec.ce, ce, sizeof ce);
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
// An unlit room holding a colour set before gets the new one (true, state 1); false: not one of them (state 2: it has lamps)
// The colour family of a room: the game's lot test (FUN_00c63140, the same the top-up uses: true = the first colour,
// 0x011D0B60), by the room's lot id (+0x10 / +0x14), cached per lot on the render thread; -1 when it cannot be run.
// 30/09 (F6 092629, user: "the first slider leaves the room extremely dark, even at 100%"): the retint used to tell the
// family from the room's current colour; near Brightness 0 both families are almost black and match each other within
// the tolerance, so a room of the second colour (0.15 0.15 0.30) took the first's (0.01 0.01 0.01, 15-30x darker) and
// stayed there, and the other way round a room turned 15-30x too bright.
std::unordered_map<uint64_t, int> g_slotOfLot;
DWORD g_slotCacheAt = 0;
bool ReadRoomLot(const unsigned char* room, uint32_t& lo, uint32_t& hi) {
    __try {
        lo = *reinterpret_cast<const uint32_t*>(room + 0x10);
        hi = *reinterpret_cast<const uint32_t*>(room + 0x14);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
int LotTestSafe(uint32_t lo, uint32_t hi) {
    __try {
        const uintptr_t wm = *reinterpret_cast<const uintptr_t*>(g_wmPtr);
        return reinterpret_cast<LotTest_t>(g_lotTest)(reinterpret_cast<void*>(wm), lo, hi) ? 0 : 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
int SlotOfRoom(const unsigned char* room) {
    if (!g_lotTest || !g_wmPtr) return -1;
    uint32_t lo = 0, hi = 0;
    if (!ReadRoomLot(room, lo, hi)) return -1;
    const DWORD now = GetTickCount();
    if (now - g_slotCacheAt > 5000) { // the lot's flag may change (a lot moved into, a world change)
        g_slotOfLot.clear();
        g_slotCacheAt = now;
    }
    const uint64_t key = (static_cast<uint64_t>(hi) << 32) | lo;
    if (const auto it = g_slotOfLot.find(key); it != g_slotOfLot.end()) return it->second;
    const int slot = LotTestSafe(lo, hi);
    if (slot >= 0) g_slotOfLot[key] = slot;
    return slot;
}

// 30/09 (F6 092629): only a room without lamps is matched by its colour. Near Brightness 0 a lit room's value (its lamps'
// share + the base) came within the tolerance of a colour set during the drag: rooms of 20 and 29 lamps (lot E4606BD0)
// were taken for unlit ones, lost their lamps' share and followed the unlit colour from then on (+0x120 left behind). A
// room with lamps is moved by MoveBase or solved again once the change rests.
bool RetintUnlit(unsigned char* room, int& state, int slot) {
    __try {
        if (*reinterpret_cast<const int*>(room + 0xC) <= 0 || *reinterpret_cast<const BYTE*>(room + 0x18)) {
            state = -1;
            return false;
        }
        // Do not change ambient while the solver owns this room.
        const int solve = *reinterpret_cast<const int*>(room + 0xF0);
        if (RoomAmbientPolicy::SolverOwnsAmbient(solve)) {
            state = -2;
            return false;
        }
        float* amb = reinterpret_cast<float*>(room + 0x110);
        float* amb2 = reinterpret_cast<float*>(room + 0x120);
        const uintptr_t b = *reinterpret_cast<const uintptr_t*>(room + 0xC8), e = *reinterpret_cast<const uintptr_t*>(room + 0xCC);
        const bool roofless = *reinterpret_cast<const BYTE*>(room + 0x18) != 0;
        if (e > b && !roofless) { // an indoor room with lamps (roofless ones never get the base: matched as before)
            state = 2;
            return false;
        }
        bool changed = false;
        if (slot >= 0 && LevelLightShare::StageUnlitAmbientChange(room, g_target[slot].v, changed)) {
            state = changed ? 1 : 0;
            return true;
        }
        for (int i = 0; i < 2; i++) {
            const int to = slot >= 0 ? slot : i; // the room's own family (lot test); without it, the list that matched
            // Same's loose tolerance identifies owned colours; it must not swallow small slider steps.
            if (Same(amb, g_target[to].v) && Near(amb, g_target[to].v, 1e-7f)) return true;
            for (const Vec4& k : g_known[i])
                if (Same(amb, k.v)) { // a colour Apex or the game set (either family: a room taken into the wrong one comes back)
                    const bool second = Same(amb2, k.v);
                    std::memcpy(amb, g_target[to].v, sizeof g_target[to].v);
                    if (second) std::memcpy(amb2, g_target[to].v, sizeof g_target[to].v);
                    state = 1;
                    return true;
                }
        }
        state = 0;
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return true;
    }
}
// A skipped/unknown room remains eligible after the slider settles. The same
// identity is sent only once per target, so the recovery scan cannot form a loop.
bool RequestRetintSolve(unsigned char* room, bool rest) {
    g_retryRetint = true;
    if (!rest) return false;
    uintptr_t mgr = 0; int id = 0;
    if (!RoomKey(room, mgr, id) || !mgr || id <= 0) return false;
    const uintptr_t key = reinterpret_cast<uintptr_t>(room);
    const auto old = g_retintSent.find(key);
    if (old != g_retintSent.end() && old->second.mgr == mgr && old->second.id == id) return false;
    if (g_retintSent.size() > 16384) return false;
    g_retintSent[key] = RetintSent{mgr, id};
    return true;
}
bool VisitRoom(unsigned char* room, void* p) {
    auto& ctx = *static_cast<VisitCtx*>(p);
    int state = 0;
    if (RetintUnlit(room, state, SlotOfRoom(room))) {
        ctx.retinted += state == 1;
        return false;
    }
    // A merged ambient or an old colour evicted from g_known must still follow
    // the controls. Let the game's solver rebuild it instead of guessing its RGB.
    if (state == -2) { g_retryRetint = true; return false; } // do not invalidate an active solve
    if (state == 0) return RequestRetintSolve(room, ctx.rest);
    if (state != 2) return false;
    {
        std::lock_guard<std::mutex> lk(g_baseMx);
        const auto it = g_baseRooms.find(reinterpret_cast<uintptr_t>(room));
        if (it != g_baseRooms.end()) {
            const int r = MoveBase(room, it->second, ctx);
            if (r == 1) ctx.moved++;
            if (r == 2) {
                const bool send = RequestRetintSolve(room, ctx.rest);
                it->second.discardOnQueue = send;
                return send;
            }
            if (r == 3) g_baseRooms.erase(it);
            if (r != 3) return false;
        }
    }
    // Lit without the base (solved before Rooms at Night was on): a new solve once the change rests
    return ctx.baseOn ? RequestRetintSolve(room, ctx.rest) : false;
}
void RetintQueueResult(unsigned char* room, bool queued, void*) {
    const uintptr_t key = reinterpret_cast<uintptr_t>(room);
    const auto sent = g_retintSent.find(key);
    if (sent != g_retintSent.end()) {
        std::lock_guard<std::mutex> lk(g_baseMx);
        const auto base = g_baseRooms.find(key);
        if (base != g_baseRooms.end() && base->second.discardOnQueue
            && base->second.mgr == sent->second.mgr && base->second.id == sent->second.id) {
            if (queued) g_baseRooms.erase(base);
            else base->second.discardOnQueue = false;
        }
    }
    if (!queued) {
        g_retintSent.erase(key);
        g_retryRetint = true;
    }
}
bool Retint(bool rest) {
    VisitCtx ctx{rest, g_on && g_patched && g_baseReady, 1.0f, 0, 0};
    __try {
        if (g_scalePtr) ctx.scale = *reinterpret_cast<const float*>(g_scalePtr);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    int sent = 0;
    LevelLightShare::ForEachRoom(&VisitRoom, &ctx, &sent, &RetintQueueResult);
    LevelLightShare::ApplyAmbientBaseChanges();
    for (int i = 0; i < 2; i++) Remember(i, g_target[i]);
    g_retinted += ctx.retinted;
    g_baseMoved.fetch_add(ctx.moved, std::memory_order_relaxed);
    g_dimSent += sent;
    return ctx.retinted != 0 || ctx.moved != 0 || sent != 0;
}

Vec4 ControlBase(int slot) {
    Vec4 base = g_base[slot];
    // S3SS may already have written the saved RGB before Apex starts. Correct only that exact
    // value in the blue family while this feature is enabled; never write native game globals.
    if (slot == 1 && g_compat.found) {
        bool matches = true;
        for (int k = 0; k < 3; ++k) matches &= std::fabs(base.v[k] - g_compat.rgb[k]) < 1e-6f;
        if (matches) for (int k = 0; k < 3; ++k) base.v[k] = kDefaultColour[k];
    }
    return base;
}

void Compute() {
    for (int i = 0; i < 2; i++) {
        const Vec4 base = ControlBase(i);
        const float grey = Luma(base.v);
        for (int k = 0; k < 3; k++) g_colour[i].v[k] = g_light * (grey + g_blue * (base.v[k] - grey));
        g_colour[i].v[3] = g_light * base.v[3];
    }
    // The game's fill light stays as it is here: at draw time it is one of the rig's dim bluish slots, turned (night-gated)
    // with the others by FurnitureColour (review 30/09, M2: it was turned twice at night and dimmed by day)
    std::memcpy(g_fill.v, kGameFill, sizeof g_fill.v);
    g_fillGate = 1;
    for (int i = 0; i < 2; i++) Remember(i, g_colour[i]); // every colour rooms may take while a slider moves (M6)
}

bool Patch() {
    if (g_patched) return true;
    const uintptr_t ours[kSites] = {reinterpret_cast<uintptr_t>(&g_colour[0]), reinterpret_cast<uintptr_t>(&g_colour[1]),
                                    reinterpret_cast<uintptr_t>(&g_colour[0]), reinterpret_cast<uintptr_t>(&g_colour[1]),
                                    reinterpret_cast<uintptr_t>(&g_fillGate),  reinterpret_cast<uintptr_t>(&g_fill)};
    bool ok = true;
    for (int s = 0; s < kSites && ok; s++) ok = MemPatch::WriteDWORD(g_site[s], static_cast<DWORD>(ours[s]), &g_patches, &g_orig[s]);
    if (!ok) {
        MemPatch::RestoreAll(g_patches); // all six or none
        g_patches.clear();
        LOG_WARNING("[UnlitRooms] Could not patch the game code; rooms stay as the game lights them");
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    g_patched = true;
    return true;
}
void Unpatch() {
    if (!g_patched) return;
    MemPatch::RestoreAll(g_patches);
    g_patches.clear();
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    g_patched = false;
}

} // namespace

namespace UnlitRooms {

bool Install(std::string& error) {
    if (g_ready) return true;
    using GameAddr::Id;
    std::string missing;
    if (!GameAddr::Have({Id::UnlitColourA, Id::UnlitColourB, Id::DimColourA, Id::DimColourB, Id::FillGate, Id::FillColour}, &missing)) {
        error = "Rooms at night: " + GameAddr::NotAvailable(missing);
        return false;
    }
    const Id ids[kSites] = {Id::UnlitColourA, Id::UnlitColourB, Id::DimColourA, Id::DimColourB, Id::FillGate, Id::FillColour};
    for (int s = 0; s < kSites; s++) {
        g_site[s] = GameAddr::Get(ids[s]);
        g_orig[s] = *reinterpret_cast<const DWORD*>(g_site[s]);
    }
    const auto at = [](uintptr_t a) { return *reinterpret_cast<const BYTE*>(a); };
    // "mov ecx, imm32" x4 (the same two colours in both functions), "cmp byte [imm32], 0", "movaps xmm2, [imm32]"
    const bool ok = at(g_site[ColourA] - 1) == 0xB9 && at(g_site[ColourB] - 1) == 0xB9 && at(g_site[DimA] - 1) == 0xB9 && at(g_site[DimB] - 1) == 0xB9 &&
                    g_orig[ColourA] == g_orig[DimA] && g_orig[ColourB] == g_orig[DimB] && g_orig[ColourA] != g_orig[ColourB] && at(g_site[Gate] - 2) == 0x80 &&
                    at(g_site[Gate] - 1) == 0x3D && at(g_site[Gate] + 4) == 0x00 && at(g_site[Fill] - 3) == 0x0F && at(g_site[Fill] - 2) == 0x28 &&
                    at(g_site[Fill] - 1) == 0x15 && (g_orig[Fill] & 15) == 0;
    if (!ok) {
        error = "Rooms at night: the game code differs (different game version?)";
        return false;
    }
    ReadBases();
    // The base under the lamps (optional part): FUN_006a00a0's two calls go through BaseHook. Checked: both calls reach
    // it, "mov ecx,[WorldManager]" at +0x11, the lot test "call" at +0x1A, "mov ecx, scalar" at +0x32
    if (GameAddr::Have({Id::DimAmbient, Id::DimAmbientCall0, Id::DimAmbientCall1, Id::WorldManagerPtr})) {
        g_topUp = GameAddr::Get(Id::DimAmbient);
        g_topUpCalls[0] = GameAddr::Get(Id::DimAmbientCall0);
        g_topUpCalls[1] = GameAddr::Get(Id::DimAmbientCall1);
        g_wmPtr = GameAddr::Get(Id::WorldManagerPtr);
        const auto callsTo = [](uintptr_t site, uintptr_t target) {
            return *reinterpret_cast<const BYTE*>(site) == 0xE8 && site + 5 + *reinterpret_cast<const int32_t*>(site + 1) == target;
        };
        bool base = callsTo(g_topUpCalls[0], g_topUp) && callsTo(g_topUpCalls[1], g_topUp) && g_topUpCalls[0] < g_topUpCalls[1] &&
                    at(g_topUp + 0x11) == 0x8B && at(g_topUp + 0x12) == 0x0D && *reinterpret_cast<const uint32_t*>(g_topUp + 0x13) == g_wmPtr &&
                    at(g_topUp + 0x1A) == 0xE8 && at(g_topUp + 0x32) == 0xB9;
        if (base) {
            g_lotTest = g_topUp + 0x1F + *reinterpret_cast<const int32_t*>(g_topUp + 0x1B);
            g_scalePtr = *reinterpret_cast<const uint32_t*>(g_topUp + 0x33);
            const uintptr_t hooks[2] = {reinterpret_cast<uintptr_t>(&BaseHook<0>), reinterpret_cast<uintptr_t>(&BaseHook<1>)};
            for (int k = 0; k < 2 && base; k++) {
                const DWORD orig = *reinterpret_cast<const DWORD*>(g_topUpCalls[k] + 1);
                base = MemPatch::WriteDWORD(g_topUpCalls[k] + 1, static_cast<DWORD>(hooks[k] - (g_topUpCalls[k] + 5)), &g_basePatches, &orig);
            }
            if (!base) {
                MemPatch::RestoreAll(g_basePatches);
                g_basePatches.clear();
            }
        }
        g_baseReady = base;
    }
    if (!g_baseReady) LOG_WARNING("[UnlitRooms] Base under the lamps: the game code differs; lit rooms keep the game's top-up");
    // S3SS applies its saved room colour when it starts. Recognise that exact value from the start (read-only), so
    // Rooms at Night uses the game's own blue as its base instead of the saved grey; S3SS.toml is not touched.
    if (const auto saved = S3SSDetect::SavedRoomAmbientOverride()) {
        g_compat.found = true;
        g_compat.rgb = *saved;
        LOG_INFO(std::format("[UnlitRooms] S3SS saves a room colour ({:.3f} {:.3f} {:.3f}); Rooms at Night uses the game's blue as its base",
                             (*saved)[0], (*saved)[1], (*saved)[2]));
    }
    g_ready = true;
    LOG_INFO(std::format("[UnlitRooms] Ready: unlit-room colours at {:#x} ({:.3f} {:.3f} {:.3f}{}) and {:#x} ({:.3f} {:.3f} {:.3f}{}), fill gate {:#x}, fill colour {:#x}",
                         g_orig[ColourA], g_base[0].v[0], g_base[0].v[1], g_base[0].v[2], g_baseDefault[0] ? ", not set yet: default" : "", g_orig[ColourB],
                         g_base[1].v[0], g_base[1].v[1], g_base[1].v[2], g_baseDefault[1] ? ", not set yet: default" : "", g_orig[Gate], g_orig[Fill]));
    return true;
}

void Uninstall() {
    if (!g_ready) return;
    MemPatch::RestoreAll(g_basePatches); // lit rooms keep the base until their next solve (the retint below sends them)
    g_basePatches.clear();
    const bool was = g_patched;
    Unpatch();
    g_on = false;
    if (was) { // the game's colours again, in the rooms at once (render thread), topped-up rooms solved again
        for (int i = 0; i < 2; i++) g_target[i] = g_base[i];
        Retint(true);
        ObjectLightBridge::RequestRigRefresh();
    }
    g_baseReady = false;
    g_ready = false; // Install checks and patches everything again
}

void Set(bool on, float light, float blue) {
    if (!g_ready) return;
    light = std::fmin(std::fmax(light, 0.0f), 1.0f);
    blue = std::fmin(std::fmax(blue, 0.0f), 1.0f);
    if (on == g_on && (!on || (std::fabs(light - g_light) < 1e-4f && std::fabs(blue - g_blue) < 1e-4f))) return;
    g_on = on;
    g_light = light, g_blue = blue;
    if (on) {
        Compute(); // before the code points at them
        if (!Patch()) g_on = false;
    } else
        Unpatch();
    Retarget();
}

S3SSDetect::RoomAmbientCorrection CorrectS3SSConflict() {
    auto result = S3SSDetect::CorrectRoomAmbientOverride();
    if (result.saved) {
        g_compat = result;
        if (g_on) {
            Compute();
            Retarget();
        }
    }
    return result;
}

void OnPresent() {
    if (!g_ready) return;
    const DWORD now = GetTickCount();
    // Floor/lot streaming can replace rooms without changing either slider.
    // Retry known colours, including rooms skipped while their solve was active.
    if ((g_on || g_retryRetint) && (!g_nextReconcile || static_cast<int32_t>(now - g_nextReconcile) >= 0)) {
        g_nextReconcile = now + 1000;
        const bool retry = g_retryRetint && !g_retintDue && now - g_changedAt > 400;
        g_retryRetint = false;
        if (Retint(retry)) ObjectLightBridge::RequestRigRefresh();
    }
    // The game may set its colours after Apex started (a world load): follow them
    if (!g_nextBaseCheck || static_cast<int32_t>(now - g_nextBaseCheck) >= 0) {
        g_nextBaseCheck = now + 2000;
        if (ReadBases()) {
            if (g_on) Compute();
            Retarget();
        }
    }
    // Walls and floors: the new colour in the rooms at once (every 50 ms at most while a slider moves)
    if (g_retintDue && now - g_lastRetint >= 50) {
        g_retintDue = false;
        g_lastRetint = now;
        Retint(false);
        if (now - g_lastRig >= 250) { // objects: their rigs gather again (at most 4 times a second)
            g_lastRig = now;
            ObjectLightBridge::RequestRigRefresh();
        } else
            g_rigAgainAt = now + 250;
    }
    // At rest: rooms topped up from their lamps get a new solve; the rigs once more after those solves
    if (g_restDue && !g_retintDue && now - g_changedAt > 400) {
        g_restDue = false;
        g_relights++;
        Retint(true);
        g_lastRig = now;
        ObjectLightBridge::RequestRigRefresh();
        g_rigAgainAt = now + 1500;
    }
    if (g_rigAgainAt && static_cast<int32_t>(now - g_rigAgainAt) >= 0) {
        g_rigAgainAt = 0;
        g_lastRig = now;
        ObjectLightBridge::RequestRigRefresh();
    }
}

void OnWorldChanged() {
    g_retintSent.clear();
    g_retryRetint = true;
    {
        std::lock_guard<std::mutex> lk(g_baseMx);
        g_baseRooms.clear();
    }
    g_slotOfLot.clear();
    g_slotCacheAt = 0;
    g_nextBaseCheck = g_nextReconcile = 0;
    g_rigAgainAt = 0;
    g_drawDark = false;
    if (g_ready) Retarget();
}

void OnRoomsChanged() {
    if (!g_ready || !g_on) return;
    g_retintDue = g_restDue = true;
    g_changedAt = GetTickCount();
}

void OnRoomChanged(uintptr_t room) {
    if (!g_ready || !g_on) return;
    // Allow one fresh recovery after a real structure change, even if the game
    // kept the same room pointer/id. Do not discard its valid ambient sources.
    g_retintSent.erase(room);
    g_nextReconcile = 0;
    OnRoomsChanged();
}

float FurnitureAmbient() { return g_patched && g_on ? FurnitureShareNow() : 1.0f; }
bool FurnitureActive() { return g_patched && g_on && NightNow() > 0.001f; }
float FurnitureTint() { return FurnitureActive() ? FurnitureTintNow() : 1.0f; }
// 30/09 (F7 110-113, user: "the blue tint again is not applied to almost any furniture"): the ambient cube (CASDiffuseProbe) is
// grey, so pulling it towards its grey did nothing; in a room with every lamp off most of a sofa is lit by that cube alone (the
// three [NoLight] lights are directional), while walls, floors and rugs take the room's blue. The cube now takes the chroma of
// the game's unlit-room blue (0x011D0B40, (0.15 0.15 0.30): x0.93 0.93 1.86 at luma 1), by the Blue tint: grey at 0%.
void FurnitureCubeColour(float* rgb) {
    rgb[0] = rgb[1] = rgb[2] = 1.0f;
    if (!FurnitureActive()) return;
    const Vec4 base = ControlBase(1);
    const float grey = Luma(base.v);
    if (!(grey > 1e-6f)) return;
    const float t = FurnitureTintNow();
    for (int k = 0; k < 3; k++) rgb[k] = std::fmax(1.0f + t * (base.v[k] / grey - 1.0f), 0.0f);
}
void SetNightLevel(float level) { g_night = level; }
void SetDrawDark(bool dark) { g_drawDark = dark; }
void RigsAgainIn(unsigned ms) { g_rigAgainAt = (GetTickCount() + ms) | 1; }
// The directions of the three [NoLight] lights (normalised CustomLightRigging.ini directions; PS c0 / c3 / c2 of every
// captured unlit room-mode furniture draw, 088-093)
constexpr float kNoLightDir[3][3] = {{0.09535f, 0.95346f, 0.28604f}, {0.0f, 0.70711f, -0.70711f}, {-0.66667f, -0.33333f, -0.66667f}};
// Exact tests only (final review, 30/09): a colour test ("dim and bluish") also took blue, purple and dim cool-white lamps,
// greying them on furniture and, on Apex's indoor-object shader, counting them twice (rig + basis maps). Over every
// room-mode draw of captures 075-094 the fill is found by w (18 slots) and [NoLight] by direction (46), the colour test
// alone never.
bool IsUnlitLight(const float* colour, const float* dir) {
    if (!dir) return false;
    if (colour[3] > 1e-6f) return true; // the fill light: FUN_006b7e70 writes w = 0.8 x its strength, every other slot w = 0
    for (const auto& d : kNoLightDir)
        if (std::fabs(dir[0] - d[0]) < 1e-3f && std::fabs(dir[1] - d[1]) < 1e-3f && std::fabs(dir[2] - d[2]) < 1e-3f) return true;
    return false;
}
// One of the [NoLight] lights with some strength left (luma > 0.01): the rig's room is dark
bool IsDarkRoomLight(const float* colour, const float* dir) {
    if (!dir || colour[3] > 1e-6f) return false;
    bool noLight = false;
    for (const auto& d : kNoLightDir)
        noLight |= std::fabs(dir[0] - d[0]) < 1e-3f && std::fabs(dir[1] - d[1]) < 1e-3f && std::fabs(dir[2] - d[2]) < 1e-3f;
    return noLight && 0.2126f * colour[0] + 0.7152f * colour[1] + 0.0722f * colour[2] > 0.01f;
}
bool FurnitureColour(float* rgb, const float* dir) {
    if (!FurnitureActive() || !IsUnlitLight(rgb, dir)) return false; // a lamp: its own colour and strength
    const float grey = 0.2126f * rgb[0] + 0.7152f * rgb[1] + 0.0722f * rgb[2];
    const float share = FurnitureShareNow(), tint = FurnitureTintNow();
    for (int k = 0; k < 3; k++) rgb[k] = share * (grey + tint * (rgb[k] - grey));
    return true;
}

// Development tools (F6 recorder): the sliders as this module has them now and what they give furniture outside a dark room
std::string SettingsText() {
    return std::format("on {} (code patched {}) | Brightness {:.3f} | Blue tint {:.3f} | night level {:.2f} | furniture outside a dark room: "
                       "acting {}, brightness x{:.3f}, blue kept {:.3f} | in a dark room ([NoLight] in the rig): brightness x{:.3f}, blue kept {:.3f}",
                       g_on, g_patched, g_light, g_blue, g_night, FurnitureActive(), FurnitureShareNow(), FurnitureTint(), g_light, g_blue);
}

std::string Status() {
    if (!g_ready) return "not installed";
    return std::format("{} | the game's unlit-room colours ({:.3f} {:.3f} {:.3f}){} and ({:.3f} {:.3f} {:.3f}){} | Apex's ({:.3f} {:.3f} {:.3f}) and ({:.3f} {:.3f} {:.3f}), "
                       "furniture fill ({:.3f} {:.3f} {:.3f}, {}) | changes applied: {}, rooms given the new colour at once: {}, rooms solved again: {} | base under the lamps: {} (given in solves: {}, moved at once: {})",
                       g_patched ? "on" : "off", g_base[0].v[0], g_base[0].v[1], g_base[0].v[2], g_baseDefault[0] ? " (default)" : "", g_base[1].v[0], g_base[1].v[1],
                       g_base[1].v[2], g_baseDefault[1] ? " (default)" : "", g_colour[0].v[0], g_colour[0].v[1], g_colour[0].v[2], g_colour[1].v[0], g_colour[1].v[1],
                       g_colour[1].v[2], g_fill.v[0], g_fill.v[1], g_fill.v[2], g_fillGate ? "on" : "off", g_relights, g_retinted, g_dimSent, g_baseReady ? (g_basePatches.empty() ? "off" : "on") : "not installed", g_baseAdded.load(), g_baseMoved.load());
}

} // namespace UnlitRooms
