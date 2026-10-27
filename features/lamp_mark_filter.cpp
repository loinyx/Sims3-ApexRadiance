// Rooms keep their light when their lamps did not change (part of Night Lighting, 2026-09-30).
//
// The indoor light broke for a moment at every floor switch; the light information should stay cached and change only
// when something changes. F6 capture: at every floor switch the game restarted room 0 of every story and every room holding
// a lamp (return address 0x006C7451 in FUN_006c7250), twice within 16 ms, and solved them again in 110-140 ms, with the same
// ambient, light counts and normalisation as before. Two studies (30/09):
// - The mark comes from the lamp entry update FUN_006c7ba0 (the entry's vtable slot +8, run by FUN_006c7250 for the
//   entries flagged dirty): for a lamp (light vfunc+0x18 = 0x00620D60, false, in 7 of the 9 light classes) it finds the
//   lamp's room (FUN_006c7b20), rewrites its lit bit and lit colour (FUN_006bdca0) and then marks the room changed
//   (FUN_006c7160: 0x006C7CCA for the room it left when it moved, 0x006C7CD6 always) without comparing anything. The
//   entries are flagged by FUN_006c4cf0 from the light manager's messages (transform 0x3361F6C9 and colour 0x966EC80A
//   always, intensity, enable and alpha only on a change); which one a floor switch sends is not known yet (the counters
//   below tell).
// - Nothing of the room solve reads the shown story or a light's visibility: the gather (FUN_006c7820) takes the lamp by
//   its room, flags, lit bit, lit colour sum and type; the point solve by its values. So a lamp whose values are all as they
//   were gives its rooms exactly the light they already have.
// So the call at 0x006C7CD6 goes through MarkThunk: the lamp's values (Signature) are compared with the last time it marked
// its rooms (per story tree level and light); the same values keep the rooms' solve (the mark is dropped), anything else
// marks as before. First sight, window lights (types 7 and 8: the long path, they follow the sky) and anything unreadable
// always mark. Left as they are: the mark of the room a lamp left (0x006C7CCA), the occluder entries (0x006C7939: objects
// that fade or hide block light differently, their rooms must be solved again), room creation and object removal marks.
#include "lamp_mark_filter.h"
#include "lot_light_bridge.h"
#include "game_addresses.h"
#include "memory_patch.h"
#include "apex_log.h"
#include "hook_guard.h"
#include "build_flavor.h"
#include "level_light_share.h"
#include "rt_outdoor_lamps.h"
#include "recorder.h"
#include "object_light_bridge.h"
#include "unlit_rooms.h"
#include "room_ambient_policy.h"
#include "drag_lite.h"
#include "dev_switches.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <format>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace {

// Steam 1.67.2 values; Install takes them from GameAddr on every build (every site still checked byte by byte)
uintptr_t kMarkCall = 0x006C7CD6;             // in FUN_006c7ba0: CALL FUN_006c7160, ecx = entry+0x14 (tree level), push entry+0x1C
uintptr_t kMark = 0x006C7160;                 // FUN_006c7160: thiscall(treeLevel, int room) ret 4: the room goes into tl+8
uintptr_t kLitCall = 0x006C7CB6;              // "mov ecx, edi; call FUN_006bdca0": edi = the light, esi = the entry
uintptr_t kEntryFlag = 0x006C4CF0;            // FUN_006c4cf0(objId): flags every light and occluder entry of an object
// The light manager's message handlers that flag entries (development build: counted, then the game's call)
// (GameAddr EntryFlagCall0..4, by address; Steam 1.67.2: 0x006B0A8D, 0x006B0B33, 0x006B0BFA, 0x006B0C9A, 0x006B0D26)
uintptr_t kFlagSites[5] = {};
constexpr const char* kFlagNames[5] = {"transform", "intensity", "colour", "enable", "alpha"};

std::vector<MemPatch::PatchLocation> g_patches;
bool g_installed = false, g_flagsCounted = false;
std::atomic<bool> g_on{false}; // (15/10: off by default, the maintainer's tested setting for 2.13.1; Developer page "Rooms keep their light...")
uintptr_t g_markTarget = kMark;
uintptr_t g_flagTarget = kEntryFlag;
std::atomic<long> g_first{0}, g_changed{0}, g_kept{0}, g_windows{0}, g_unread{0}, g_off{0}, g_held{0};
volatile long g_flagCount[5] = {};

struct Key {
    uintptr_t tl, light;
    bool operator==(const Key&) const = default;
};
struct KeyHash {
    size_t operator()(const Key& k) const { return std::hash<uintptr_t>()(k.tl) ^ (std::hash<uintptr_t>()(k.light) * 31u); }
};
std::mutex g_mx;
// A lamp at its last mark: its values, and what a player sees change (on or off, where it is, its room)
struct LampState {
    uint64_t sig;
    bool on;
    float pos[3];
    int room;
    DWORD eventsFrom = 0; // switches on or off seen since this tick (a light that keeps switching itself is left out)
    int events = 0;
    float editable[5] = {}; // base RGB, intensity, enabled; excludes animated fade
    DWORD editUntil = 0;    // an edit (move, switch, colour) is going on until then: its next changes are part of it
    DWORD lastAuto = 0;     // (auto_light_priority) its latest quiet mark: at most one every 5 s
};
std::atomic<long> g_autoSelfOn{0}, g_autoDuskOn{0}; // automatic switch-ons left out of the lamp edits (counters)
constexpr DWORD kEditHoldMs = 2000;
std::unordered_map<Key, LampState, KeyHash> g_sig; // (tree level, light) -> the lamp at its last mark
// The story (tree level) each lamp marked last: a first mark on another story of the same lot is the lamp moved between
// stories (05/10, F8: nothing sent its lot again, and the rooms that gathered while the game registered it missed it)
std::unordered_map<uintptr_t, uintptr_t> g_lampStory; // light -> tree level (under g_mx)

// A lamp switched on or off, or moved (the lighting is refreshed then too, kept cheap): a switch waits 120 ms; a move still waits 200 ms after its last change (a drag:
// once when it stops). At most once per RoomAmbientPolicy::LampLotGap per lot (2 s; 500 ms for a lamp into another room or
// story, or many lamps, 12/10); never while dusk/dawn switches every lamp at once.
// Not the whole world as the shortcut: only the rooms of that lot (every story), on the light tree thread, and the rigs.
// Since 05/10 the rooms that take an edited lamp are sent at once by the edit itself (LevelLightShare, "Lamp edits first"),
// so this is the safety net: rooms gathered after the change keep their solve, and for lamps that only moved (same room, on
// or off as before) only the rooms whose list holds them are sent.
constexpr float kMoveMin = 0.10f; // m: animated lamps wobble less
constexpr DWORD kSelfWindow = 10000; // ms
constexpr int kSelfMax = 3;          // more switches on or off than this within kSelfWindow: a light switching itself, left out
constexpr int kLotLamps = 8;         // lamps that moved, per lot refresh (more: every room of the lot)
struct LotDue {
    DWORD due = 0, last = 0, changed = 0;
    bool switchOnly = false;
    bool allRooms = false; // a lamp into another room, a lamp moved and switched, or more than kLotLamps lamps: every room
    bool relocated = false; // (07/10) a lamp into another room, or moved and switched: its rooms are sent (RelightLampSwitch), none kept
    int lampCount = 0;
    uintptr_t lamps[kLotLamps] = {};
    // lamps switched where they are (06/10): only the rooms they can reach (LevelLightShare::RelightLampSwitch)
    int switchCount = 0;
    LevelLightShare::LampSwitch switched[kLotLamps] = {};
};
std::unordered_map<uintptr_t, LotDue> g_lotDue; // tracker -> when its refresh is due (under g_mx); due 0 = none pending
std::atomic<long> g_lampEvents{0}, g_lotRefreshes{0}, g_lotSkippedDusk{0};
std::atomic<bool> g_editRefresh{false};
// Mass switches (switching all the lights of a big lot took 3-7 s to settle): the player's switches are counted
// in a window; MassSwitchActive lets the room queue give the rooms a quick pass first (room_light_queue.cpp)
constexpr int kMassSwitches = 3;
constexpr DWORD kMassWindowMs = 1500, kMassHoldMs = 1500;
std::atomic<DWORD> g_massLast{0};
std::atomic<long> g_massEvents{0};
std::atomic<float>& NightLevelRef();
// Every switch is counted here, the light's own self-switching test aside (06/10 capture: after testing "all the lights"
// four times within 10 s every lamp of the lot counted as switching itself, so the burst was neither a lamp edit nor a mass
// switch). kMassSwitches DIFFERENT lights within kMassWindowMs make a burst; a light flickering alone never does. True while
// a burst is on (the switch is the player's even when the light switched often).
std::mutex g_massMx;
struct RecentSwitch {
    uintptr_t light;
    DWORD tick;
};
RecentSwitch g_recent[16] = {};
int g_recentNext = 0;
bool NoteSwitch(uintptr_t light, DWORD tick) {
    // dusk / dawn switches every lamp itself: not the player
    const float night = NightLevelRef().load(std::memory_order_relaxed);
    if (night < 0.0f || (night > 0.02f && night < 0.98f)) return false;
    int distinct = 0;
    {
        std::lock_guard<std::mutex> lk(g_massMx);
        g_recent[g_recentNext] = RecentSwitch{light, tick};
        g_recentNext = (g_recentNext + 1) % static_cast<int>(std::size(g_recent));
        uintptr_t seen[std::size(g_recent)];
        for (const RecentSwitch& r : g_recent) {
            if (!r.light || tick - r.tick > kMassWindowMs) continue;
            if (std::find(seen, seen + distinct, r.light) == seen + distinct) seen[distinct++] = r.light;
        }
    }
    if (distinct < kMassSwitches) return GetTickCount() - g_massLast.load(std::memory_order_relaxed) <= kMassHoldMs && g_massLast.load() != 0;
    if (tick - g_massLast.load(std::memory_order_relaxed) > kMassHoldMs) g_massEvents.fetch_add(1, std::memory_order_relaxed);
    g_massLast.store(tick | 1, std::memory_order_relaxed);
    return true;
}
// Any switch of the player's (the quick pass for one lamp too): a lamp switched on or off where it is, not a
// light switching itself (past kSelfMax switches in kSelfWindow), not at dusk or dawn. Switches less than kMassHoldMs apart
// are one event, so "all the lights" is one too.
std::atomic<DWORD> g_switchLast{0};
std::atomic<long> g_switchEvents{0};
std::atomic<DWORD> g_switchStart{0}; // the first switch of the current event
std::atomic<unsigned long> g_switchSerial{0}; // counts the switches: a solve begun after one has a serial at least its own (no tick step to share)
void NoteUserSwitch(DWORD tick) {
    const float night = NightLevelRef().load(std::memory_order_relaxed);
    if (night < 0.0f || (night > 0.02f && night < 0.98f)) return;
    if (tick - g_switchLast.load(std::memory_order_relaxed) > kMassHoldMs) {
        g_switchStart.store(tick, std::memory_order_relaxed);
        g_switchEvents.fetch_add(1, std::memory_order_relaxed);
    }
    g_switchLast.store(tick | 1, std::memory_order_relaxed);
    g_switchSerial.fetch_add(1, std::memory_order_relaxed);
}
// The night level at the last Present (-1 = no world): while dusk or dawn switches every lamp, switches are not lamp edits
std::atomic<float> g_nightLevel{-1.0f};
std::atomic<float>& NightLevelRef() { return g_nightLevel; }
// 12/10, lamp edits all at once (AtriumHold): every player edit (switch, value, move, drag step, a lamp added or removed)
// starts or extends the edit hold. The serial grows by one at each: the room solves are stamped with it (RoomLightQueue
// NoteSolveStart), so a room counts as solved for an edit only by a solve begun after it, even in the same tick step.
std::atomic<DWORD> g_editLast{0};
std::atomic<unsigned long> g_editSerial{0};
std::atomic<long> g_editEvents{0};
void NoteEditNow(DWORD tick) {
    const float night = g_nightLevel.load(std::memory_order_relaxed);
    if (night < 0.0f || (night > 0.02f && night < 0.98f)) return; // no world, or dusk / dawn switching every lamp
    g_editSerial.fetch_add(1, std::memory_order_relaxed);
    g_editLast.store(tick | 1, std::memory_order_release); // after the serial: a reader of the tick sees its serial
    g_editEvents.fetch_add(1, std::memory_order_relaxed);
}

inline void Mix(uint64_t& h, uint32_t v) { h = (h ^ v) * 1099511628211ull; }
inline void MixDwords(uint64_t& h, uintptr_t at, int n) {
    for (int k = 0; k < n; k++) Mix(h, *reinterpret_cast<const uint32_t*>(at + k * 4));
}
// The lamp as the room solve takes it: its room, the lit bit (+0x100 & 0x20) and the object's flags (entry+0x20 record,
// +0x90), the values +0x10 (4), the type +0xB0, +0xC0..+0xDC, the lit colour +0xE0..+0xEC (FUN_006bdca0 has just written
// it), the first base-colour component +0xF0, the position +0x120 (3) and range +0x130, the cone of spot lights
// (types 4 and 5: +0x170, 13 dwords). The separate editable sample wakes terrain reconciliation without changing
// this room-mark signature; animated fade +0x20 does not wake it. False when it cannot be read.
bool Signature(uintptr_t entry, uintptr_t light, int room, uint64_t& h, int& type, bool& on, float* pos, float* editable) {
    __try {
        h = 1469598103934665603ull;
        const float* lit = reinterpret_cast<const float*>(light + 0xE0); // the lit colour: 0 when the game switched it off
        on = (*reinterpret_cast<const BYTE*>(light + 0x100) & 0x20) && lit[0] + lit[1] + lit[2] > 1e-3f;
        std::memcpy(pos, reinterpret_cast<const void*>(light + 0x120), 3 * sizeof(float));
        Mix(h, static_cast<uint32_t>(room));
        Mix(h, *reinterpret_cast<const BYTE*>(light + 0x100) & 0x20);
        if (const uintptr_t info = *reinterpret_cast<const uintptr_t*>(entry + 0x20)) Mix(h, *reinterpret_cast<const BYTE*>(info + 0x90) & 0x6);
        type = *reinterpret_cast<const int*>(light + 0xB0);
        if ((type >= 3 && type <= 6) || type == 11) {
            std::memcpy(editable, reinterpret_cast<const void*>(light + 0xF0), 3 * sizeof(float));
            editable[3] = *reinterpret_cast<const float*>(light + 0x10);
            editable[4] = (*reinterpret_cast<const BYTE*>(light + 0x100) & 0x40) ? 1.0f : 0.0f;
        }
        Mix(h, static_cast<uint32_t>(type));
        MixDwords(h, light + 0x10, 4);
        MixDwords(h, light + 0xC0, 8);
        MixDwords(h, light + 0xE0, 5);
        MixDwords(h, light + 0x120, 3);
        MixDwords(h, light + 0x130, 1);
        if (type == 4 || type == 5) MixDwords(h, light + 0x170, 13);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// ---- Lamp lighting: Instant (2.12.3 + RT). The occluder mark of a GPU-handled lamp's own object (14/10 review, in-game) ----
// Dragging a lamp the GPU draws marked no room through its lamp entry, yet room 0 of every story and the rooms through its
// openings were solved 0.7-3 s after the release. The lamp's object also has an occluder entry: its update FUN_006c78e0
// (esi = the entry) finds the room under the object ([entry+0x24] +0x2C / +0x30, x and z, as FUN_006a4ee0 takes them;
// dumpbin 14/10) and marks it at 0x006C7939 ("mov ecx,[esi+14h]; push eax; mov [esi+1Ch],eax; call 0x006C7160") on every
// drag step, whatever moved. An occluder whose point is within kOwnObjectM (horizontally) of a lamp head the GPU handled on
// the same story within kOwnObjectMs is taken as that lamp's object: its mark is dropped (the GPU draws the lamp). While
// Lamp lighting is Classic no edit is GPU-handled: every occluder mark goes through as the game makes it.
// UNVERIFIED that every lamp object has an occluder entry: the counters tell.
uintptr_t kOccMarkCall = 0x006C7939; // GameAddr OccMarkCall
constexpr float kOwnObjectM = 1.5f;
constexpr DWORD kOwnObjectMs = 600;
struct HandledEdit {
    uintptr_t tl = 0;
    float pos[3] = {};
    DWORD at = 0;
};
HandledEdit g_handled[16]; // (under g_mx)
int g_handledNext = 0;
std::atomic<DWORD> g_handledAt{0};
std::atomic<long> g_occMarks{0}, g_occDropped{0};
bool g_occInstalled = false;
// The lights whose edits the GPU took lately (any kind): the changed-room walk tells a GPU edit's rooms from a real edit's
struct HandledLight {
    std::atomic<uintptr_t> light{0};
    std::atomic<DWORD> at{0};
};
HandledLight g_handledLights[64];
std::atomic<uint32_t> g_handledLightNext{0};
void NoteHandledLight(uintptr_t light) {
    HandledLight& h = g_handledLights[g_handledLightNext.fetch_add(1, std::memory_order_relaxed) % std::size(g_handledLights)];
    h.at.store(0, std::memory_order_relaxed);
    h.light.store(light, std::memory_order_relaxed);
    h.at.store(GetTickCount() | 1, std::memory_order_release);
}
void NoteHandledEdit(uintptr_t tl, const float pos[3]) {
    const DWORD now = GetTickCount() | 1;
    std::lock_guard<std::mutex> lk(g_mx);
    HandledEdit& h = g_handled[g_handledNext];
    g_handledNext = (g_handledNext + 1) % static_cast<int>(std::size(g_handled));
    h.tl = tl, h.at = now;
    std::memcpy(h.pos, pos, sizeof h.pos);
    g_handledAt.store(now, std::memory_order_relaxed);
}
bool OccluderPoint(uintptr_t entry, float& x, float& z) { // POD (SEH)
    __try {
        const uintptr_t occ = *reinterpret_cast<const uintptr_t*>(entry + 0x24);
        if (!occ) return false;
        x = *reinterpret_cast<const float*>(occ + 0x2C);
        z = *reinterpret_cast<const float*>(occ + 0x30);
        return std::isfinite(x) && std::isfinite(z);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// true = mark the room (the game's call)
bool OccDecideImpl(uintptr_t tl, int room, uintptr_t entry) {
    const DWORD last = g_handledAt.load(std::memory_order_relaxed);
    if (!last) return true; // (no GPU-handled edit yet: always while Classic)
    g_occMarks.fetch_add(1, std::memory_order_relaxed);
    const DWORD now = GetTickCount();
    if (now - last > kOwnObjectMs || !g_on.load(std::memory_order_relaxed)) return true;
    float x = 0.0f, z = 0.0f;
    if (!OccluderPoint(entry, x, z)) return true;
    bool own = false;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        for (const HandledEdit& h : g_handled)
            if (h.tl == tl && h.at && now - h.at <= kOwnObjectMs) {
                const float dx = h.pos[0] - x, dz = h.pos[2] - z;
                if (dx * dx + dz * dz <= kOwnObjectM * kOwnObjectM) own = true;
            }
    }
    if (!own) return true;
    const long n = g_occDropped.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 3 || (n & (n - 1)) == 0)
        LOG_INFO(std::format("[LampMarkFilter] Occluder mark of room {} dropped: the object of a lamp the GPU draws, moved with it ({} so far)", room, n));
    return false;
}
bool __cdecl OccDecide(uintptr_t tl, int room, uintptr_t entry) {
    if (!g_handledAt.load(std::memory_order_relaxed)) return true; // (Classic: the game's mark, nothing else)
    return HookGuard::Run("LampMarkFilter occluder mark", true, [=] { return OccDecideImpl(tl, room, entry); });
}

// The lot of a story tree level (tl+4: its tracker, as level_light_share reads it); 0 when unreadable
uintptr_t TrackerOf(uintptr_t tl) {
    __try {
        return *reinterpret_cast<const uintptr_t*>(tl + 4);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}

// Marks deferred by BeginDeferMarks on this thread (LevelLightShare's window activation recheck): kept back, then made as
// the game makes them (EndDeferMarks) only when the entry's update changed what the room's gather reads
thread_local int t_deferMarks = 0;
struct DeferredMark {
    uintptr_t tl;
    int room;
    uintptr_t entry; // the light entry whose update made it
};
thread_local std::vector<DeferredMark> t_deferred;
std::atomic<long> g_deferredDropped{0}, g_deferredReplayed{0};

// true = mark the room (the game's call), false = keep its solve
bool MarkDecideImpl(uintptr_t tl, int room, uintptr_t entry, uintptr_t light) {
    if (t_deferMarks) {
        if (t_deferred.size() < 64) {
            t_deferred.push_back(DeferredMark{tl, room, entry});
            return false;
        }
        return true; // (too many to keep: the rest as the game makes them)
    }
    uint64_t h = 0;
    int type = 0;
    bool on = false;
    float pos[3] = {};
    float editable[5] = {};
    if (!entry || !light || !Signature(entry, light, room, h, type, on, pos, editable)) {
        g_unread.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    if (type == 7 || type == 8) { // window lights: the long path, they follow the sky
        g_windows.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    // Real-time outdoor lamps (Lamp lighting: Instant only, rt_outdoor_lamps.h): an outdoor lamp the GPU draws on every story of
    // its lot from its current values. Its edit (moved in room 0, recoloured, dimmed, switched) marks no room: no room 0 of any
    // story is solved again, and no edit hold, quick pass or lot safety net starts. The rooms that take room 0's lamps are sent
    // after the room update (RtOutdoorLamps::AfterRoomUpdate). A move into another room is the game's own mark (the lamp leaves
    // room 0's list). A lamp switched on is the exception: while it was off its rooms gathered again without it (the other
    // stories' room 0s no longer list it), so its switch takes the usual path below, which sends those stories to gather it.
    bool rtSwitchOn = false;
    if (RtOutdoorLamps::Watching()) {
        std::lock_guard<std::mutex> lk(g_mx);
        const auto it = g_sig.find(Key{tl, light});
        rtSwitchOn = it != g_sig.end() && !it->second.on && on;
    }
    if (!rtSwitchOn && RtOutdoorLamps::Watching() && RtOutdoorLamps::HandlesLamp(tl, light, room)) {
        int kind = -1;
        bool known = false, playerEdit = false;
        {
            std::lock_guard<std::mutex> lk(g_mx);
            const auto it = g_sig.find(Key{tl, light});
            if (it != g_sig.end()) {
                known = true;
                LampState& was = it->second;
                const float dx = pos[0] - was.pos[0], dy = pos[1] - was.pos[1], dz = pos[2] - was.pos[2];
                const bool moved = dx * dx + dy * dy + dz * dz > kMoveMin * kMoveMin;
                const bool valueChanged = std::memcmp(was.editable, editable, sizeof editable) != 0 || was.on != on;
                kind = was.room != room ? 2 : moved ? 1 : valueChanged ? 0 : -1;
                // (14/10 review) a player's edit: a move, or a colour, intensity or enable change (a switch by a Sim or a
                // timer, or an animated fade, is not one)
                playerEdit = moved || std::memcmp(was.editable, editable, sizeof editable) != 0;
                if (moved || kind == 2) std::memcpy(was.pos, pos, sizeof was.pos);
                std::memcpy(was.editable, editable, sizeof editable);
                was.on = on;
                was.room = room;
                was.sig = h;
                was.editUntil = 0;
            }
        }
        if (known) {
            if (kind == 2) return true; // into another room: the game's mark of that room
            if (kind >= 0) {
                if (kind == 1) g_editRefresh.store(true, std::memory_order_relaxed); // the per-pixel object lamps read the list again
                RtOutdoorLamps::NoteLampEdit(tl, light, kind);
                NoteHandledLight(light);
                if (playerEdit) LevelLightShare::NoteGpuEdit();
                if (kind == 1) NoteHandledEdit(tl, pos); // (only a move carries the lamp's object along)
            }
            g_kept.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
    }
    if (rtSwitchOn) RtOutdoorLamps::NoteSwitchOn(tl, light);
    // Lamp edits first (05/10, LevelLightShare): an edit (moved, recoloured, dimmed, switched) marks its room as urgent;
    // `user` = a value a player or a Sim sets (colour, intensity, on / off), sent to the other stories at once; `pure` = the
    // same room, on or off as before (a move or a value change): a solve in progress keeps going, the mark is held;
    // `stayed` = the same room (switched too): the lamp is registered there, its rooms gather at once
    bool same = false, edit = false, user = false, pure = false, stayed = false;
    bool autoSelf = false, switchedOn = false; // a light switching itself on (no edit); switched on in place (dusk below)
    {
        std::lock_guard<std::mutex> lk(g_mx);
        if (g_sig.size() > 65536) g_sig.clear();
        const LampState now{h, on, {pos[0], pos[1], pos[2]}, room, 0, 0};
        const auto [it, fresh] = g_sig.try_emplace(Key{tl, light}, now);
        const bool lamp = (type >= 3 && type <= 6) || type == 11;
        if (fresh) {
            std::memcpy(it->second.editable, editable, sizeof editable);
            if (lamp) g_editRefresh.store(true, std::memory_order_relaxed);
            g_first.fetch_add(1, std::memory_order_relaxed);
            if (lamp) { // moved in from another story of the same lot: every room of the lot again, none kept
                if (g_lampStory.size() > 65536) g_lampStory.clear();
                uintptr_t& story = g_lampStory[light];
                const uintptr_t tracker = TrackerOf(tl);
                if (story && story != tl && tracker && TrackerOf(story) == tracker) {
                    if (g_lotDue.size() > 1024) g_lotDue.clear();
                    LotDue& pending = g_lotDue[tracker];
                    const DWORD tick = GetTickCount();
                    pending.allRooms = true;
                    // 12/10: marked as a relocation, so its safety net keeps the full resend (changedAt 0, the F8 bug
                    // below) while other whole-lot refreshes now keep their fresh solves
                    pending.relocated = true;
                    pending.switchOnly = false;
                    pending.changed = tick;
                    pending.due = (tick + RoomAmbientPolicy::LampRefreshDelay(false)) | 1;
                    g_lampEvents.fetch_add(1, std::memory_order_relaxed);
                    NoteEditNow(tick); // a player's edit too: the edit hold covers the stories it leaves and enters
                }
                story = tl;
            }
            return true;
        }
        LampState& was = it->second;
        const DWORD tick = GetTickCount();
        if (std::memcmp(was.editable, editable, sizeof editable) != 0) {
            std::memcpy(was.editable, editable, sizeof editable);
            g_editRefresh.store(true, std::memory_order_relaxed);
            user = edit = true;
            if (was.on == on) Recorder::NoteLampEdit(tl, on, true); // a value changed (a switch is noted below)
        }
        same = was.sig == h;
        const float dx = pos[0] - was.pos[0], dy = pos[1] - was.pos[1], dz = pos[2] - was.pos[2];
        const bool moved = dx * dx + dy * dy + dz * dz > kMoveMin * kMoveMin; // NaN: not a move
        if (moved) DragLite::NoteLampStep("lamp entry: moved"); // (a lamp drag, drag_lite.h)
        stayed = was.room == room;
        pure = was.on == on && stayed;
        if (was.on != on || moved || was.room != room) { // the player sees this: its lot lights again (OnPresent)
            if (tick - was.eventsFrom > kSelfWindow) {
                was.eventsFrom = tick;
                was.events = 0;
            }
            // a light that switches itself on and off (a flickering TV or effect light): not the player (a drag only moves: never left out)
            // ... unless several different lights switch together: the player's "all the lights", however often (NoteSwitch)
            const bool burst = was.on != on && NoteSwitch(light, tick);
            const bool selfSwitching = was.on != on && ++was.events > kSelfMax && !burst;
            switchedOn = on && !was.on && !moved && was.room == room;
            if (selfSwitching && switchedOn && tick - was.lastAuto > 5000) {
                autoSelf = true;
                was.lastAuto = tick | 1;
            }
            if (was.on != on && !selfSwitching && !moved && was.room == room) NoteUserSwitch(tick); // the room queue's quick pass
            if (!selfSwitching) g_lampEvents.fetch_add(1, std::memory_order_relaxed);
            if (!selfSwitching) Recorder::NoteLampEdit(tl, on, was.on == on); // the recording's light update summary
            edit = edit || !selfSwitching;
            user = user || (was.on != on && !selfSwitching);
            // the per-pixel lamps (objects, fences, roofs, water) and the ground bake read the lamp list again soon, not at
            // the next 20-frame enumeration: a dragged lamp's light follows it
            if (moved && ((type >= 3 && type <= 6) || type == 11)) g_editRefresh.store(true, std::memory_order_relaxed);
            if (const uintptr_t tracker = selfSwitching ? 0 : TrackerOf(tl)) {
                if (g_lotDue.size() > 1024) g_lotDue.clear();
                LotDue& pending = g_lotDue[tracker];
                const bool switchOnly = was.on != on && !moved && was.room == room;
                if (!pending.due) { // a new refresh: nothing collected yet
                    pending.allRooms = false;
                    pending.relocated = false;
                    pending.lampCount = 0;
                    pending.switchCount = 0;
                }
                if (switchOnly || !pure) { // switched, moved and switched, or into another room (07/10: those rooms, not the whole lot)
                    LevelLightShare::LampSwitch* end = pending.switched + pending.switchCount;
                    LevelLightShare::LampSwitch* sw = std::find_if(pending.switched, end, [light](const LevelLightShare::LampSwitch& w) { return w.light == light; });
                    if (sw != end) *sw = LevelLightShare::LampSwitch{light, tl, room}; // a drag: where it is now
                    else if (pending.switchCount < kLotLamps) pending.switched[pending.switchCount++] = LevelLightShare::LampSwitch{light, tl, room};
                    else pending.allRooms = true;
                    if (!pure) pending.relocated = true;
                } else if (!pending.allRooms && std::find(pending.lamps, pending.lamps + pending.lampCount, light) == pending.lamps + pending.lampCount) {
                    if (pending.lampCount < kLotLamps) pending.lamps[pending.lampCount++] = light;
                    else pending.allRooms = true;
                }
                pending.switchOnly = pending.due ? pending.switchOnly && switchOnly : switchOnly;
                pending.changed = tick;
                pending.due = (tick + RoomAmbientPolicy::LampRefreshDelay(pending.switchOnly)) | 1;
            }
            was.on = on;
            std::memcpy(was.pos, pos, sizeof was.pos); // a slow drag: each 10 cm step counts, the refresh waits for the last
            was.room = room;
        }
        // the steps of a drag between its 10 cm events are the same edit (an animated lamp never moves 10 cm: never one)
        if (edit) was.editUntil = (tick + kEditHoldMs) | 1;
        else if (!same && was.editUntil && static_cast<int32_t>(tick - was.editUntil) < 0) {
            edit = true;
            DragLite::NoteLampStep("lamp entry: a step of the same edit");
        }
        was.sig = h;
    }
    // dusk and dawn switch every lamp of every lot: those switches go through the game's own pace (an edit's rooms are solved
    // all at once, which here would be every lit room of the lot in one pause)
    bool autoDusk = false;
    if (const float night = g_nightLevel.load(std::memory_order_relaxed); edit && !pure && stayed && night > 0.02f && night < 0.98f) {
        edit = false;
        autoDusk = switchedOn;
    }
    // (development builds, auto_light_priority) those automatic switch-ons put the lamp's own room first in the room queue
    // (no edit, no hold, no extra budget: LevelLightShare::NoteQuietMark); counted either way
    if (autoSelf) g_autoSelfOn.fetch_add(1, std::memory_order_relaxed);
    if (autoDusk) g_autoDuskOn.fetch_add(1, std::memory_order_relaxed);
    if ((autoSelf || autoDusk) && DevSwitches::AutoLightPriority())
        HookGuard::Run("LampMarkFilter quiet mark", [&] { LevelLightShare::NoteQuietMark(tl, room, autoDusk); });
    if (autoSelf || autoDusk) {
        static std::atomic<DWORD> s_logged{0};
        const DWORD tick = GetTickCount();
        DWORD last = s_logged.load(std::memory_order_relaxed);
        if (tick - last > 60000 && s_logged.compare_exchange_strong(last, tick))
            LOG_INFO(std::format("[LampMarkFilter] Automatic switch-ons (not lamp edits): {} by lights toggling often, {} at dusk or dawn; {} rooms put first (auto_light_priority)",
                                 g_autoSelfOn.load(), g_autoDuskOn.load(), LevelLightShare::QuietMarks()));
    }
    if (same) {
        if (!g_on.load(std::memory_order_relaxed)) { // off: every mark goes through (the values are still followed)
            g_off.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        g_kept.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    // 12/10: every edit that reaches its rooms (switch, value, move, drag step) starts or extends the edit hold, before the
    // rooms are marked (MarkUrgent stamps them with this serial)
    if (edit) NoteEditNow(GetTickCount());
    if (!g_on.load(std::memory_order_relaxed)) {
        g_off.fetch_add(1, std::memory_order_relaxed);
        if (edit) {
            LevelLightShare::NoteLampEditing(pure, TrackerOf(tl));
            LevelLightShare::NoteLampMark(tl, room, user, stayed);
        }
        return true;
    }
    g_changed.fetch_add(1, std::memory_order_relaxed);
    if (edit) LevelLightShare::NoteLampEditing(pure, TrackerOf(tl)); // a drag (moved, value) or a switch: how the edit's rooms are solved
    if (edit && pure && LevelLightShare::HoldLampMark(tl, room, user)) {
        g_held.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    if (edit) LevelLightShare::NoteLampMark(tl, room, user, stayed);
    return true;
}

// 07/10, players' Runtime Error: MarkDecideImpl allocates (its maps) inside the game's lamp code. A C++ exception is caught, the
// room is marked as the game would mark it (true), and the filter stays off from then on (HookGuard).
bool __cdecl MarkDecide(uintptr_t tl, int room, uintptr_t entry, uintptr_t light) {
    return HookGuard::Run("LampMarkFilter room mark", true, [=] { return MarkDecideImpl(tl, room, entry, light); });
}

// At the call: ecx = the tree level, [esp+4] = the room, esi = the entry, edi = the light (the game's registers, kept
// across the calls since "mov ecx, edi; call FUN_006bdca0")
__declspec(naked) void MarkThunk() {
    __asm {
        push ecx
        push edi
        push esi
        push dword ptr [esp + 16]
        push ecx
        call MarkDecide
        add esp, 16
        pop ecx
        test al, al
        jz keep
        jmp dword ptr [g_markTarget]
    keep:
        ret 4
    }
}

// At 0x006C7939 (Lamp lighting: Instant; Classic lets every mark through): ecx = the tree level, [esp+4] = the room, esi =
// the occluder entry
__declspec(naked) void OccThunk() {
    __asm {
        push ecx
        push esi
        push dword ptr [esp + 12]
        push ecx
        call OccDecide
        add esp, 12
        pop ecx
        test al, al
        jz keep
        jmp dword ptr [g_markTarget]
    keep:
        ret 4
    }
}

// The light manager's messages that flag light entries: counted, then the game's call
__declspec(naked) void FlagThunk0() { __asm { lock inc dword ptr [g_flagCount + 0] } __asm { jmp dword ptr [g_flagTarget] } }
__declspec(naked) void FlagThunk1() { __asm { lock inc dword ptr [g_flagCount + 4] } __asm { jmp dword ptr [g_flagTarget] } }
__declspec(naked) void FlagThunk2() { __asm { lock inc dword ptr [g_flagCount + 8] } __asm { jmp dword ptr [g_flagTarget] } }
__declspec(naked) void FlagThunk3() { __asm { lock inc dword ptr [g_flagCount + 12] } __asm { jmp dword ptr [g_flagTarget] } }
__declspec(naked) void FlagThunk4() { __asm { lock inc dword ptr [g_flagCount + 16] } __asm { jmp dword ptr [g_flagTarget] } }
void (*const kFlagThunks[5])() = {&FlagThunk0, &FlagThunk1, &FlagThunk2, &FlagThunk3, &FlagThunk4};

bool CallsTarget(uintptr_t site, uintptr_t target) {
    return *reinterpret_cast<const BYTE*>(site) == 0xE8 && site + 5 + *reinterpret_cast<const int32_t*>(site + 1) == target;
}
bool Redirect(uintptr_t site, uintptr_t target, const void* to) {
    const DWORD orig = static_cast<DWORD>(target - (site + 5));
    return MemPatch::WriteDWORD(site + 1, static_cast<DWORD>(reinterpret_cast<uintptr_t>(to) - (site + 5)), &g_patches, &orig);
}

} // namespace

namespace LampMarkFilter {

bool Install(std::string& why) {
    if (g_installed) return true;
    {
        using GameAddr::Id;
        std::string missing;
        if (!GameAddr::Have({Id::LampLitCall, Id::LampMarkCall, Id::LampMark}, &missing)) {
            why = "Rooms keep their light: " + GameAddr::NotAvailable(missing);
            return false;
        }
        kLitCall = GameAddr::Get(Id::LampLitCall);
        kMarkCall = GameAddr::Get(Id::LampMarkCall);
        kMark = GameAddr::Get(Id::LampMark);
        g_markTarget = kMark;
    }
    // 0x6C7CB6: 8B CF E8 (mov ecx, edi; call FUN_006bdca0); 0x6C7CCF: 8B 4E 1C 51 8B 4E 14 (mov ecx,[esi+1C]; push ecx; mov ecx,[esi+14])
    const BYTE lit[] = {0x8B, 0xCF, 0xE8}, arg[] = {0x8B, 0x4E, 0x1C, 0x51, 0x8B, 0x4E, 0x14};
    const bool ok = std::memcmp(reinterpret_cast<const void*>(kLitCall), lit, sizeof lit) == 0 &&
                    std::memcmp(reinterpret_cast<const void*>(kMarkCall - sizeof arg), arg, sizeof arg) == 0 && CallsTarget(kMarkCall, kMark);
    if (!ok) {
        why = "Rooms keep their light: the game code differs";
        return false;
    }
    if (!Redirect(kMarkCall, kMark, reinterpret_cast<const void*>(&MarkThunk))) {
        MemPatch::RestoreAll(g_patches);
        g_patches.clear();
        why = "Rooms keep their light: could not patch the game";
        return false;
    }
    // the occluder mark (GameAddr OccMarkCall; checked byte by byte): with Lamp lighting: Instant, a GPU-drawn lamp's own object
    // marks no room while it is dragged; with Classic every occluder mark goes through as the game makes it (OccDecide)
    g_occInstalled = false;
    kOccMarkCall = GameAddr::Get(GameAddr::Id::OccMarkCall);
    if (kOccMarkCall) {
        const BYTE occArg[] = {0x8B, 0x4E, 0x14, 0x50, 0x89, 0x46, 0x1C};
        if (std::memcmp(reinterpret_cast<const void*>(kOccMarkCall - sizeof occArg), occArg, sizeof occArg) == 0 && CallsTarget(kOccMarkCall, kMark))
            g_occInstalled = Redirect(kOccMarkCall, kMark, reinterpret_cast<const void*>(&OccThunk));
        if (!g_occInstalled) LOG_WARNING(std::format("[LampMarkFilter] The occluder mark ({:#010x}) differs: a GPU-drawn lamp's own object still marks its room when it moves", kOccMarkCall));
    } else {
        LOG_WARNING("[LampMarkFilter] The occluder mark: " + GameAddr::NotAvailable("OccMarkCall") + "; a GPU-drawn lamp's own object still marks its room when it moves");
    }
    g_flagsCounted = false;
    if (!kPublicBuild) { // which messages flag the entries (the floor switch's trigger): all five or none
        using GameAddr::Id;
        bool all = GameAddr::Have({Id::EntryFlag, Id::EntryFlagCall0, Id::EntryFlagCall1, Id::EntryFlagCall2, Id::EntryFlagCall3, Id::EntryFlagCall4});
        kEntryFlag = GameAddr::Get(Id::EntryFlag);
        g_flagTarget = kEntryFlag;
        for (int i = 0; i < 5; i++) kFlagSites[i] = GameAddr::Get(static_cast<Id>(static_cast<int>(Id::EntryFlagCall0) + i));
        for (uintptr_t s : kFlagSites) all = all && CallsTarget(s, kEntryFlag);
        std::vector<MemPatch::PatchLocation> before = g_patches;
        for (int i = 0; i < 5 && all; i++) all = Redirect(kFlagSites[i], kEntryFlag, reinterpret_cast<const void*>(kFlagThunks[i]));
        if (!all) {
            std::vector<MemPatch::PatchLocation> added(g_patches.begin() + static_cast<std::ptrdiff_t>(before.size()), g_patches.end());
            MemPatch::RestoreAll(added);
            g_patches = before;
            LOG_WARNING("[LampMarkFilter] The light-entry message counters could not be installed (the game code differs)");
        }
        g_flagsCounted = all;
    }
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    g_installed = true;
    LOG_INFO(std::format("[LampMarkFilter] Installed: a lamp marks its rooms changed only when it changed{}", g_flagsCounted ? " (message counters on)" : ""));
    return true;
}

void Uninstall() {
    if (!g_installed) return;
    MemPatch::RestoreAll(g_patches);
    g_patches.clear();
    FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
    g_installed = false;
    std::lock_guard<std::mutex> lk(g_mx);
    g_sig.clear(); // lamps freed while it was out would leave stale keys
    g_lampStory.clear();
    g_lotDue.clear();
    g_editRefresh.store(false, std::memory_order_relaxed);
}

bool IsInstalled() { return g_installed; }

void OnPresent(float nightLevel) {
    g_nightLevel.store(nightLevel, std::memory_order_relaxed);
    if (!g_installed) return;
    if (g_editRefresh.exchange(false, std::memory_order_relaxed)) LotLightBridge::RequestLampEditRefresh();
    const DWORD now = GetTickCount();
    struct Run {
        uintptr_t tracker;
        DWORD changed;
        bool switchOnly, allRooms, relocated;
        int lampCount;
        uintptr_t lamps[kLotLamps];
        int switchCount;
        LevelLightShare::LampSwitch switched[kLotLamps];
    };
    std::vector<Run> run;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        for (auto& [tracker, d] : g_lotDue) {
            if (!d.due || static_cast<int32_t>(now - d.due) < 0) continue;
            // refreshed a moment ago: once more when the gap is over (12/10: 500 ms for a relocation or many lamps, 2 s for switches)
            if (const DWORD gap = RoomAmbientPolicy::LampLotGap(d.relocated || d.allRooms); d.last && now - d.last < gap) {
                d.due = (d.last + gap) | 1;
                continue;
            }
            d.due = 0;
            if (nightLevel < 0.0f || (nightLevel > 0.02f && nightLevel < 0.98f)) { // no world, or dusk / dawn switching every lamp
                g_lotSkippedDusk.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            d.last = now | 1;
            Run r{tracker, d.changed, d.switchOnly, d.allRooms || (d.lampCount == 0 && d.switchCount == 0), d.relocated, d.lampCount, {}, d.switchCount, {}};
            std::copy(d.lamps, d.lamps + d.lampCount, r.lamps);
            std::copy(d.switched, d.switched + d.switchCount, r.switched);
            run.push_back(r);
        }
    }
    // the rooms gathered after the change keep their solve (the lamp edit sent them already) for a switch or a lamp that
    // only moved, and lamps that only moved send only the rooms holding them; a lamp moved into another room or story sends
    // every room again: a gather made while the game was still registering it missed it (F8)
    // (lamps switched where they are: only the rooms they can reach, LevelLightShare::RelightLampSwitch)
    for (const Run& r : run) {
        const int sent = !r.allRooms && r.switchCount > 0
                             ? LevelLightShare::RelightLampSwitch(r.tracker, r.relocated ? "a lamp moved into another room" : r.lampCount ? "a lamp switched or moved" : "a lamp switched", r.relocated ? 0 : r.changed,
                                                                  r.switched, r.switchCount, r.lamps, r.lampCount)
                             // 12/10: only a relocation (a lamp into another room or story) resends every room (the F8
                             // bug above); the other whole-lot refreshes (more than kLotLamps lamps) keep the fresh solves
                             : LevelLightShare::RelightLot(r.tracker, r.switchOnly ? "a lamp switched" : r.relocated ? "a lamp moved into another room or story" : r.allRooms ? "a lamp switched or moved" : "a lamp moved",
                                                           r.relocated ? 0 : r.changed, r.allRooms ? nullptr : r.lamps, r.allRooms ? 0 : r.lampCount, true);
        if (sent >= 0) g_lotRefreshes.fetch_add(1, std::memory_order_relaxed);
    }
    if (!run.empty()) {
        ObjectLightBridge::RequestRigRefresh();
        // RelightLot watches completion and retains its own bounded fallback.
    }
}
long MassEventId() { return g_massEvents.load(std::memory_order_relaxed); }
long SwitchEventId() { return g_switchEvents.load(std::memory_order_relaxed); }
unsigned long SwitchEventStart() { return g_switchStart.load(std::memory_order_relaxed); }
unsigned long SwitchLastTick() { return g_switchLast.load(std::memory_order_relaxed); }
unsigned long SwitchSerial() { return g_switchSerial.load(std::memory_order_relaxed); }
bool LotRefreshDueSoon(unsigned long withinMs) {
    if (!g_installed) return false;
    const DWORD now = GetTickCount();
    std::lock_guard<std::mutex> lk(g_mx);
    return std::any_of(g_lotDue.begin(), g_lotDue.end(), [&](const auto& kv) {
        return kv.second.due && static_cast<int32_t>(kv.second.due - now) < static_cast<int32_t>(withinMs);
    });
}
unsigned long EditLastTick() { return g_editLast.load(std::memory_order_acquire); }
unsigned long EditSerial() { return g_editSerial.load(std::memory_order_relaxed); }
void NoteEdit() { NoteEditNow(GetTickCount()); }
bool SwitchActive() {
    const DWORD last = g_switchLast.load(std::memory_order_relaxed);
    return g_installed && last && GetTickCount() - last <= kMassHoldMs;
}
bool MassSwitchActive() {
    const DWORD last = g_massLast.load(std::memory_order_relaxed);
    return g_installed && last && GetTickCount() - last <= kMassHoldMs;
}
void SetEnabled(bool on) { g_on.store(on, std::memory_order_relaxed); }
bool Enabled() { return g_on.load(std::memory_order_relaxed); }
void BeginDeferMarks() {
    if (!t_deferMarks++) t_deferred.clear();
}
void EndDeferMarks(bool replay, uintptr_t entry) {
    if (!t_deferMarks || --t_deferMarks) return;
    for (const DeferredMark& m : t_deferred) {
        const bool make = replay || m.entry != entry; // (a mark of another entry the update touched goes through)
        if (make) reinterpret_cast<void(__thiscall*)(void*, int)>(g_markTarget)(reinterpret_cast<void*>(m.tl), m.room); // the game's mark
        (make ? g_deferredReplayed : g_deferredDropped).fetch_add(1, std::memory_order_relaxed);
    }
    t_deferred.clear();
}
long DeferredMarksDropped() { return g_deferredDropped.load(std::memory_order_relaxed); }
bool HandledLately(uintptr_t light, unsigned long withinMs) {
    if (!light) return false;
    const DWORD now = GetTickCount();
    for (const HandledLight& h : g_handledLights) {
        const DWORD at = h.at.load(std::memory_order_acquire);
        if (at && now - at < withinMs && h.light.load(std::memory_order_relaxed) == light) return true;
    }
    return false;
}

std::string Status() {
    if (!g_installed) return "not installed";
    std::string s = std::format("{} | lamp marks kept (the lamp as it was) {}, let through: first sight {}, changed {}, window lights {}, unreadable {}, "
                                "while off {}, held while their room was solved {}",
                                g_on.load() ? "on" : "off", g_kept.load(), g_first.load(), g_changed.load(), g_windows.load(), g_unread.load(), g_off.load(), g_held.load());
    s += std::format(" | lamps switched or moved {}, lots lit again {} (skipped at dusk or dawn {})", g_lampEvents.load(), g_lotRefreshes.load(), g_lotSkippedDusk.load());
    if (g_occInstalled) s += std::format(" | occluder marks seen after a GPU edit {}, dropped as a GPU-drawn lamp's own object {}", g_occMarks.load(), g_occDropped.load());
    if (g_flagsCounted) {
        s += " | light entries flagged by message:";
        for (int i = 0; i < 5; i++) s += std::format(" {} {}", kFlagNames[i], static_cast<long>(g_flagCount[i]));
    }
    return s;
}

} // namespace LampMarkFilter
