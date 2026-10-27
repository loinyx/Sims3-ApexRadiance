// Who sends rooms to the solve (see room_solve_causes.h).
//
// Part of Apex Radiance. Credits: @loinyx
#include "room_solve_causes.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <format>
#include <mutex>
#include <vector>

namespace RoomSolveCauses {
namespace {

// The camera state each count is split by: still for kSettledMs or more, still for less (the rooms of a move still
// settling), moving
enum Cam { kSettled, kStopped, kMoving, kCams };
constexpr uint64_t kSettledMs = 10000;

// Fixed tables (no allocation while counting): a caller or a room that finds its table full is counted in the overflow
// The time since the same room's previous invalidate (any caller): under 0.3 s, 0.3-1 s, 1-3 s, 3-10 s, 10 s or more / first
enum Gap { kGap03, kGap1, kGap3, kGap10, kGapLong, kGaps };
struct CauseSlot {
    uintptr_t caller = 0;
    bool onFlag = false, viaQueue = false;
    uint32_t n[kCams] = {};
    uint32_t gap[kGaps] = {};
    uint32_t room0 = 0; // invalidates of a story's room 0 (the outdoor room; the game's room-0 cascade restarts them)
    uint32_t state[kRoomStates] = {};
};
// The last invalidate of each room (fixed table; a room address reused 10 s later starts as "first")
struct LastSlot {
    uintptr_t room = 0;
    uint64_t at = 0;
};
constexpr int kLast = 2048;
LastSlot g_last[kLast];
struct RoomSlot {
    uintptr_t mgr = 0;
    int id = 0, level = 0;
    uint32_t lot = 0;
    uint32_t n[kCams] = {};
    uint32_t cls[3] = {};
};
constexpr int kCauses = 64, kRooms = 128;

std::mutex g_mx;
CauseSlot g_causes[kCauses];
RoomSlot g_rooms[kRooms];
uint32_t g_causeOverflow = 0, g_roomOverflow = 0;
uint32_t g_solves[kCams][4] = {}; // [camera][class 0, 1, 2, other]
uint32_t g_invalidates[kCams] = {};
uint32_t g_states[kRoomStates] = {};
uint32_t g_frames[kCams] = {};
uint64_t g_windowStart = 0, g_lastMoving = 0;
bool g_everMoved = false;
std::atomic<int> g_cam{kSettled};

uint32_t Sum(const uint32_t* v, int n) {
    uint32_t s = 0;
    for (int i = 0; i < n; i++) s += v[i];
    return s;
}

void Clear() {
    for (auto& c : g_causes) c = {};
    for (auto& r : g_rooms) r = {};
    g_causeOverflow = g_roomOverflow = 0;
    for (auto& s : g_solves) for (auto& v : s) v = 0;
    for (auto& v : g_invalidates) v = 0;
    for (auto& v : g_states) v = 0;
    for (auto& v : g_frames) v = 0;
}

int GapOf(uintptr_t room, uint64_t now) { // under g_mx
    const int home = static_cast<int>((static_cast<uint32_t>(room) * 2654435761u) >> 21); // 11 bits
    LastSlot *slot = nullptr, *free = nullptr;
    for (int k = 0, i = home; k < 16 && !slot; k++, i = (i + 1) & (kLast - 1))
        if (g_last[i].room == room) slot = &g_last[i];
        else if (!free && (!g_last[i].room || now - g_last[i].at >= 10000)) free = &g_last[i];
    if (!slot) slot = free ? free : &g_last[home];
    const uint64_t gap = slot->room == room && slot->at ? now - slot->at : ~0ull;
    slot->room = room, slot->at = now;
    return gap < 300 ? kGap03 : gap < 1000 ? kGap1 : gap < 3000 ? kGap3 : gap < 10000 ? kGap10 : kGapLong;
}

std::string Split(const uint32_t* n) { return std::format("settled {} stopped {} moving {}", n[kSettled], n[kStopped], n[kMoving]); }

std::string Line(double seconds) {
    std::string s = std::format("{:.0f} s, camera frames {}; finished solves", seconds, Split(g_frames));
    const char* const names[kCams] = {"settled", "stopped", "moving"};
    for (int c = 0; c < kCams; c++)
        s += std::format("{} {} {} (class 0/1/2 {}/{}/{}{})", c ? "," : "", names[c], Sum(g_solves[c], 4), g_solves[c][0], g_solves[c][1], g_solves[c][2],
                         g_solves[c][3] ? std::format(" +{} other", g_solves[c][3]) : "");
    s += "; invalidates " + Split(g_invalidates) +
         std::format(" (room waiting {}, mid-solve {}, solved {}, other {})", g_states[kWaiting], g_states[kMidSolve], g_states[kSolved], g_states[kOtherState]);
    std::vector<const CauseSlot*> causes;
    for (const auto& c : g_causes)
        if (c.caller) causes.push_back(&c);
    std::sort(causes.begin(), causes.end(), [](const CauseSlot* a, const CauseSlot* b) { return Sum(a->n, kCams) > Sum(b->n, kCams); });
    if (!causes.empty()) s += " | invalidated by";
    for (size_t i = 0; i < causes.size() && i < 8; i++)
        s += std::format("{} {}{}{} {}, after the room's last <0.3/0.3-1/1-3/3-10/10+ s {}/{}/{}/{}/{}, room 0 {}", i ? ";" : "",
                         causes[i]->viaQueue ? "QueueRoom from " : "", CallerText(causes[i]->caller), causes[i]->onFlag ? " (flag)" : "", Split(causes[i]->n),
                         causes[i]->gap[kGap03], causes[i]->gap[kGap1], causes[i]->gap[kGap3], causes[i]->gap[kGap10], causes[i]->gap[kGapLong], causes[i]->room0) +
             std::format(", waiting/mid-solve/solved/other {}/{}/{}/{}", causes[i]->state[kWaiting], causes[i]->state[kMidSolve], causes[i]->state[kSolved],
                         causes[i]->state[kOtherState]);
    if (g_causeOverflow) s += std::format("; {} invalidates by callers not listed", g_causeOverflow);
    std::vector<const RoomSlot*> rooms;
    for (const auto& r : g_rooms)
        if (r.mgr) rooms.push_back(&r);
    std::sort(rooms.begin(), rooms.end(), [](const RoomSlot* a, const RoomSlot* b) { return Sum(a->n, kCams) > Sum(b->n, kCams); });
    if (!rooms.empty()) s += std::format(" | {} rooms solved; most", rooms.size());
    for (size_t i = 0; i < rooms.size() && i < 4; i++)
        s += std::format("{} lot {} story {} room {} {} (class 0/1/2 {}/{}/{})", i ? ";" : "", rooms[i]->lot, rooms[i]->level, rooms[i]->id, Split(rooms[i]->n),
                         rooms[i]->cls[0], rooms[i]->cls[1], rooms[i]->cls[2]);
    if (g_roomOverflow) s += std::format("; {} solves of rooms not listed", g_roomOverflow);
    return s;
}

} // namespace

void NoteInvalidate(uintptr_t caller, bool onFlag, bool viaQueueRoom, uintptr_t room, int roomId, uint64_t nowMs, RoomState state) {
    const int m = g_cam.load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(g_mx);
    g_invalidates[m]++;
    g_states[state]++;
    const int gap = room ? GapOf(room, nowMs) : kGapLong;
    for (int k = 0, i = static_cast<int>((caller * 2654435761u + (onFlag ? 1u : 0u)) % kCauses); k < kCauses; k++, i = (i + 1) % kCauses) {
        CauseSlot& c = g_causes[i];
        if (!c.caller) c.caller = caller, c.onFlag = onFlag, c.viaQueue = viaQueueRoom;
        if (c.caller == caller && c.onFlag == onFlag && c.viaQueue == viaQueueRoom) {
            c.n[m]++;
            c.gap[gap]++;
            c.state[state]++;
            if (roomId == 0) c.room0++;
            return;
        }
    }
    g_causeOverflow++;
}

void NoteSolve(uintptr_t mgr, int id, int level, uint32_t lot, int cls) {
    const int m = g_cam.load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(g_mx);
    g_solves[m][cls >= 0 && cls <= 2 ? cls : 3]++;
    for (int k = 0, i = static_cast<int>((mgr * 2654435761u ^ static_cast<uint32_t>(id) * 0x85EBCA77u) % kRooms); k < kRooms; k++, i = (i + 1) % kRooms) {
        RoomSlot& r = g_rooms[i];
        if (!r.mgr) r.mgr = mgr, r.id = id, r.level = level, r.lot = lot;
        if (r.mgr == mgr && r.id == id) {
            r.n[m]++;
            if (cls >= 0 && cls <= 2) r.cls[cls]++;
            return;
        }
    }
    g_roomOverflow++;
}

bool Frame(bool cameraMoving, uint64_t nowMs, std::string* line, uint64_t windowMs) {
    if (cameraMoving) g_lastMoving = nowMs, g_everMoved = true;
    const int cam = cameraMoving ? kMoving : g_everMoved && nowMs - g_lastMoving < kSettledMs ? kStopped : kSettled;
    g_cam.store(cam, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(g_mx);
    if (!g_windowStart) g_windowStart = nowMs;
    g_frames[cam]++;
    if (nowMs - g_windowStart < windowMs) return false;
    if (line) *line = Line(static_cast<double>(nowMs - g_windowStart) / 1000.0);
    Clear();
    g_windowStart = nowMs;
    return true;
}

std::string CallerText(uintptr_t caller) {
    HMODULE self = nullptr, at = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&CallerText), &self);
    if (caller && GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(caller), &at) &&
        at == self && self != GetModuleHandleW(nullptr))
        return std::format("ApexRadiance.asi+{:#x}", caller - reinterpret_cast<uintptr_t>(self));
    return std::format("{:#010x}", caller);
}

} // namespace RoomSolveCauses
