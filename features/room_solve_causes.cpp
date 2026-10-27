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
struct CauseSlot {
    uintptr_t caller = 0;
    bool onFlag = false, viaQueue = false;
    uint32_t n[kCams] = {};
};
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
    for (auto& v : g_frames) v = 0;
}

std::string Split(const uint32_t* n) { return std::format("settled {} stopped {} moving {}", n[kSettled], n[kStopped], n[kMoving]); }

std::string Line(double seconds) {
    std::string s = std::format("{:.0f} s, camera frames {}; finished solves", seconds, Split(g_frames));
    const char* const names[kCams] = {"settled", "stopped", "moving"};
    for (int c = 0; c < kCams; c++)
        s += std::format("{} {} {} (class 0/1/2 {}/{}/{}{})", c ? "," : "", names[c], Sum(g_solves[c], 4), g_solves[c][0], g_solves[c][1], g_solves[c][2],
                         g_solves[c][3] ? std::format(" +{} other", g_solves[c][3]) : "");
    s += "; invalidates " + Split(g_invalidates);
    std::vector<const CauseSlot*> causes;
    for (const auto& c : g_causes)
        if (c.caller) causes.push_back(&c);
    std::sort(causes.begin(), causes.end(), [](const CauseSlot* a, const CauseSlot* b) { return Sum(a->n, kCams) > Sum(b->n, kCams); });
    if (!causes.empty()) s += " | invalidated by";
    for (size_t i = 0; i < causes.size() && i < 8; i++)
        s += std::format("{} {}{}{} {}", i ? ";" : "", causes[i]->viaQueue ? "QueueRoom from " : "", CallerText(causes[i]->caller), causes[i]->onFlag ? " (flag)" : "",
                         Split(causes[i]->n));
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

void NoteInvalidate(uintptr_t caller, bool onFlag, bool viaQueueRoom) {
    const int m = g_cam.load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(g_mx);
    g_invalidates[m]++;
    for (int k = 0, i = static_cast<int>((caller * 2654435761u + (onFlag ? 1u : 0u)) % kCauses); k < kCauses; k++, i = (i + 1) % kCauses) {
        CauseSlot& c = g_causes[i];
        if (!c.caller) c.caller = caller, c.onFlag = onFlag, c.viaQueue = viaQueueRoom;
        if (c.caller == caller && c.onFlag == onFlag && c.viaQueue == viaQueueRoom) {
            c.n[m]++;
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
