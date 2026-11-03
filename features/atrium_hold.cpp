// An atrium's stories change together (see atrium_hold.h).
#include "atrium_hold.h"
#include "apex_log.h"
#include "apex_util.h"
#include "hook_guard.h"
#include "lamp_mark_filter.h"
#include "level_light_share.h"
#include "outdoor_delta.h"
#include "rt_outdoor_lamps.h"
#include "room_light_queue.h"
#include "lightmap_smooth.h"
#include "recorder.h"
#include "memory_patch.h"
#include "room_ambient_policy.h"
#include "tester_toggles.h"
#include "drag_lite.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <format>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace {

using LockRect_t = HRESULT(__stdcall*)(IDirect3DTexture9*, UINT, D3DLOCKED_RECT*, const RECT*, DWORD);
using UnlockRect_t = HRESULT(__stdcall*)(IDirect3DTexture9*, UINT);
LockRect_t g_origLock = nullptr;
UnlockRect_t g_origUnlock = nullptr;

constexpr DWORD kHoldMs = 1500;  // the longest a map waits for the other stories of its atrium
constexpr DWORD kKeepMs = 3000;  // a map's buffers are kept this long after its last change
constexpr size_t kMaxMaps = 192; // maps kept at once (a switch of every lamp of the atrium house: about 150)
constexpr size_t kMaxBytes = 96u << 20; // their buffers together (2 x w x h x 4 each); past it a map is left to the game

// ---- Lamp edits all at once (12/10; was "lamp switches all at once", 06/10) ----
// The switch hold held every story, the lamp's own too, until the stories on screen were solved; after moves and value
// edits, which were never held, the stories changed one at a time (1-20 s). Every story should show the corrected light
// together and sooner. Now every player edit (switch, value, move, drag, a lamp into another story, a lamp
// added or removed: LampMarkFilter::EditLastTick) starts or extends an edit hold:
//   - the lamp's own story (LevelLightShare::OnEditStory: the story of an edited lamp's own room) is never held: its maps
//     show as soon as its rooms are solved, and follow a dragged lamp live. Holding it too was the rejected 05/10 "every
//     story together" (docs/history/performance-room-light-queue.md): the lamp's own story waited 2-4 s;
//   - every map an urgent room of another story writes (the rooms taking the lamp's light, tier 1, and those the edit's
//     safety net sends, tier 2) waits, as an atrium's maps do; a drag's other stories keep their maps from before it;
//   - all of them are released in one frame once every such room has ended a solve begun after its latest mark
//     (LevelLightShare::EditRoomsPending, every story, hidden ones included) and the ground's chunks are written
//     (LightmapSmooth::HoldPending); at the latest 2.5 s after the latest edit or 4 s after the first (a drag: 2.5 s after it
//     is let go, one hold per settled drag), or at once when the kept maps reach kMaxMaps / kMaxBytes ("cap");
//   - for a switch, the furniture of other stories keeps its rig lights (lot_light_bridge.cpp), the ground its smoothed maps
//     (LightmapSmooth) and the per-pixel lamps their list (LotLightBridge) until the release, as before (SwitchHolding).
std::atomic<bool> g_allAtOnce{true}, g_editHolding{false}, g_holdHasSwitch{false}, g_capHit{false};
std::atomic<unsigned long> g_editSeen{0};   // the latest edit handled (LampMarkFilter::EditLastTick)
std::atomic<unsigned long> g_switchSeen{0}; // the latest switch handled (LampMarkFilter::SwitchLastTick)
DWORD g_holdFirst = 0, g_holdLatest = 0;    // the hold's first and latest edit (render thread); a drag restarts the first
DWORD g_holdStart = 0;                      // the hold's first edit, never restarted (the kMapMaxMs bound)
unsigned long g_holdFrom = 0;               // rooms marked with an EditSerial above this belong to the hold
unsigned long g_doneSerial = 0;             // EditSerial before the last hold ended (the next hold's g_holdFrom)
long g_ownShownMs = -1;                     // this hold: the own story's rooms all solved, ms after the first edit
bool g_holdDragged = false;                 // this hold followed a drag
size_t g_holdPeakMaps = 0;                  // this hold: the most maps kept at once
constexpr DWORD kEditMinMs = 150;           // (RoomAmbientPolicy::EditHoldOutcome) the marks and the safety net come first
constexpr DWORD kMapMaxMs = 30000;          // a map never waits longer, whatever the hold (a very long drag)
constexpr DWORD kNetWaitMs = 600;           // the hold waits for a lot's safety net due within this
std::atomic<long> g_editHolds{0}, g_editTimeouts{0}, g_editCaps{0}, g_editLastMs{-1}, g_editLastOwnMs{-1}, g_editLastMaps{0},
    g_editLastRooms{0}, g_editPeakMaps{0};
std::atomic<DWORD> g_holdUpdated{0}; // the last UpdateEditHold: a hold not updated for 1 s (the queue stopped) is over
// On, and the game's solve hooks are in (its end and its map lock step): without them no room would ever count as solved
// and every edit would wait the whole timeout (other game builds: the stories change one by one)
bool AllAtOnceUsable() { return g_allAtOnce.load(std::memory_order_relaxed) && LevelLightShare::SolveHooksReady(); }
// ---- Classic: show all floors together (14/10, Tester switch classicTogether, on; Classic lamp lighting only) ----
// After a lamp edit (a drag's release, a switch, a colour) several stories still changed one after another: the lamp's own
// story first, then the others, and with the drag's deferred sends some rooms after the release of the hold. Now, in Classic
// (DragLite::ClassicLighting: Instant (GPU) keeps the rules above), the lamp's own story is held like the others (its maps
// keep what was on screen too), every edit holds the furniture, the ground and the per-pixel lamps as a switch did, and the
// hold ends only when every room of the edit, the own story's included, has ended a solve begun after its mark. A room that
// never finishes ends the hold kTogetherMaxMs after the edit (a drag: after its release): what is ready is shown, logged
// once. The kept maps stay in the same budget (kMaxMaps / kMaxBytes; past it everything is shown at once, "cap").
constexpr DWORD kTogetherMaxMs = 6000;
// 2.13.1 (Tester switch fastOwnRelight): the lamps' own rooms (tier 0) no longer wait for the rooms around them, which the
// spread cap solves over many frames: once every own room is solved, the hold waits at most kTogetherOwnGraceMs more for the
// others, then shows what is ready; the rest change as each is solved. With Instant, a drag of a lamp the game solves (an
// indoor lamp) takes these Classic rules too (DragLite::ClassicRulesForDrag)
constexpr DWORD kTogetherOwnGraceMs = 300;
DWORD g_ownReadyAt = 0; // render thread: this hold's own rooms all solved (GetTickCount | 1), 0 = not yet
// 2.13.1, own room at once (Tester switch ownRoomAtOnce, on). A recording (14/10, an indoor lamp dragged on story 2): the
// lamp's room was solved 31 ms after the drag's end and shown 375 ms later (the grace above and a frame), while the rooms
// around it, under the spread cap, were not solved 1.7 s later anyway. Now the grace is 0: the own rooms show in the frame
// after their solve. At any hold's end (not at the cap) a held map of a room whose solve has not ended since its mark
// (LevelLightShare::EditRoomWaiting) keeps what was on screen until it has ("trailing", at most kTrailMaxMs): never the
// content of a solve thrown away by an invalidate (the game unlocks its maps half written) or of one begun before the
// mark. The maps such a room's solve locks after the hold are kept the same way. Its furniture waits with it
// (RoomShowsOldLight, lot_light_bridge.cpp).
constexpr DWORD kTrailMaxMs = 8000;
std::atomic<DWORD> g_trailUntil{0}; // GetTickCount | 1: rooms of the last hold may still trail until then
std::atomic<long> g_holdRewrites{0}; // this hold: a held map written again (an earlier or thrown-away solve's content kept back)
std::atomic<int> g_trailMaps{0};     // maps trailing now (OnPresent)
bool OwnAtOnce() { return TesterToggles::ownRoomAtOnce.load(std::memory_order_relaxed); }
bool TrailingNow() {
    const DWORD until = g_trailUntil.load(std::memory_order_relaxed);
    return until && OwnAtOnce() && AllAtOnceUsable() && static_cast<int32_t>(until - GetTickCount()) > 0;
}
bool TogetherNow() {
    return DragLite::ClassicRulesForDrag() && TesterToggles::classicTogether.load(std::memory_order_relaxed);
}
std::atomic<bool> g_holdTogether{false}; // the current hold runs under "show all floors together" (fixed at its start)
// An edit is held, or one happened that UpdateEditHold has not seen yet: a room's solve may lock its maps in the very frame
// of the edit, before the next Present starts the hold (render thread). Never once the queue stopped (Clear) or stopped
// calling UpdateEditHold: nothing would release what waits.
bool EditPendingNow() {
    if (!AllAtOnceUsable()) return false;
    const DWORD now = GetTickCount();
    if (now - g_holdUpdated.load(std::memory_order_relaxed) >= 1000) return false;
    if (g_editHolding.load(std::memory_order_relaxed)) return true;
    const unsigned long last = LampMarkFilter::EditLastTick();
    return last && last != g_editSeen.load(std::memory_order_relaxed) && now - last < 1000;
}
// The lamp's own story waits with the others (show all floors together): fixed for a hold at its start, else as now
bool HoldsOwnStory() {
    return g_editHolding.load(std::memory_order_relaxed) ? g_holdTogether.load(std::memory_order_relaxed) : TogetherNow();
}
// The edit hold covers a switch (the furniture, the ground and the per-pixel lamps wait too); under "show all floors
// together" every edit does
bool SwitchPendingNow() {
    if (!EditPendingNow()) return false;
    if (g_holdHasSwitch.load(std::memory_order_relaxed) || HoldsOwnStory()) return true;
    const unsigned long last = LampMarkFilter::SwitchLastTick();
    return last && last != g_switchSeen.load(std::memory_order_relaxed) && GetTickCount() - last < 1000;
}

struct Map {
    UINT w = 0, h = 0;
    std::vector<uint32_t> shown, latest; // what is on screen while the map waits, the game's exact content
    DWORD last = 0, heldAt = 0;          // last change, start of the wait
    bool held = false;     // waiting for the other stories of its atrium: the screen shows "shown"
    bool release = false;  // its wait is over: "latest" is written at the next frame
    bool resume = false;   // the game's lock got "latest" back: its unlock decides again
    bool refining = false; // written by a refinement (another class's maps, not on screen): never waits
    bool sw = false;       // waits for a lamp edit shown all at once (12/10: any edit, was switches; not for its atrium)
    bool trail = false;    // 2.13.1: after its edit hold ended, waits for its own room's solve (TrailingNow)
    const void* room = nullptr; // the room whose solve wrote it last (a key: only passed to LevelLightShare)
    int gameLocks = 0;
    RECT rect{};
    DWORD flags = 0;
    BYTE* bits = nullptr; // the game's lock (its rect's top-left) while it holds the map
    INT pitch = 0;
};

std::mutex g_mx;
std::unordered_map<IDirect3DTexture9*, Map> g_maps; // AddRef'd while kept
size_t g_bytes = 0;                                 // their buffers (g_mx)
std::atomic<size_t> g_count{0};
std::atomic<bool> g_active{false}, g_hooked{false}, g_hookTried{false};
thread_local bool t_own = false;
std::atomic<long> g_gameLocks{0}, g_peak{0}, g_held{0}, g_released{0}, g_timeouts{0}, g_refinements{0}, g_writes{0};

// A MANAGED single-level A8R8G8B8 texture of a room map's size (docs/engine/room-light-maps.md, room_map_padding.cpp)
bool RoomMap(IDirect3DTexture9* t, UINT& w, UINT& h) {
    if (!t || t->GetLevelCount() != 1) return false;
    D3DSURFACE_DESC d{};
    if (FAILED(t->GetLevelDesc(0, &d))) return false;
    w = d.Width;
    h = d.Height;
    return d.Format == D3DFMT_A8R8G8B8 && d.Pool == D3DPOOL_MANAGED && w >= 8 && h >= 8 && w <= 1024 && h <= 1024;
}

RECT Clip(const Map& m, const RECT* r) {
    RECT c{0, 0, static_cast<LONG>(m.w), static_cast<LONG>(m.h)};
    if (r) {
        c.left = std::clamp<LONG>(r->left, 0, m.w);
        c.top = std::clamp<LONG>(r->top, 0, m.h);
        c.right = std::clamp<LONG>(r->right, c.left, m.w);
        c.bottom = std::clamp<LONG>(r->bottom, c.top, m.h);
    }
    return c;
}

// rows of a rect between a full-size buffer and locked memory (bits: the rect's top-left)
void ToMemory(const Map& m, const std::vector<uint32_t>& buf, const RECT& r, BYTE* bits, INT pitch) {
    const size_t n = static_cast<size_t>(r.right - r.left) * 4;
    for (LONG y = r.top; y < r.bottom; y++) std::memcpy(bits + static_cast<size_t>(y - r.top) * pitch, &buf[static_cast<size_t>(y) * m.w + r.left], n);
}
bool FromMemory(const Map& m, std::vector<uint32_t>& buf, const RECT& r, const BYTE* bits, INT pitch) {
    const size_t n = static_cast<size_t>(r.right - r.left) * 4;
    bool changed = false;
    for (LONG y = r.top; y < r.bottom; y++) {
        uint32_t* dst = &buf[static_cast<size_t>(y) * m.w + r.left];
        const BYTE* src = bits + static_cast<size_t>(y - r.top) * pitch;
        if (std::memcmp(dst, src, n) != 0) {
            std::memcpy(dst, src, n);
            changed = true;
        }
    }
    return changed;
}
// The whole map takes "latest" (Apex's own lock); false when the lock failed
bool WriteLatest(IDirect3DTexture9* tex, const Map& m) {
    D3DLOCKED_RECT lr{};
    t_own = true;
    const bool ok = SUCCEEDED(g_origLock(tex, 0, &lr, nullptr, 0));
    if (ok) {
        for (UINT y = 0; y < m.h; y++) std::memcpy(static_cast<BYTE*>(lr.pBits) + static_cast<size_t>(y) * lr.Pitch, &m.latest[static_cast<size_t>(y) * m.w], m.w * 4);
        g_origUnlock(tex, 0);
    }
    t_own = false;
    return ok;
}

// 07/10, players' Runtime Error: g_mx is never held across the game's own LockRect (another layer's hook may call back into
// this one on the same thread, and a std::mutex locked twice by one thread ends the game), and a new map's record is
// created inside the try, before anything it owns (its reference, its bytes) is counted. `called` = the game's lock of this
// call was made: LockHook never makes it twice.
HRESULT LockHookBody(IDirect3DTexture9* t, UINT level, D3DLOCKED_RECT* out, const RECT* rect, DWORD flags, bool& called) {
    UINT w = 0, h = 0;
    const bool refining = RoomLightQueue::SolveRefiningUp();
    const void* solving = RoomLightQueue::SolvingRoom();
    std::unique_lock<std::mutex> lk(g_mx);
    auto it = g_maps.find(t);
    if (it == g_maps.end()) {
        // a map a lamp edit's room (of another story than the lamp's) or an atrium room's solve locks in its step 1 while a
        // lamp edit is pending: its content as it is now (before the solve writes it). Only in that step
        // (LevelLightShare::InMapLockStep), as a room stays mid-solve across frames: never the UI or anything else the render
        // thread locks meanwhile (06/10 review). An edit's room is kept from the very frame of the edit, before the next
        // Present starts the hold (review, H2). 12/10: never a map of the lamp's own story (OnEditStory) while an edit is
        // held: that story shows its new light as soon as it is solved. (`solving` is always an urgent room: SolvingRoom.)
        const bool editing = solving && EditPendingNow();
        const bool own = editing && !HoldsOwnStory() && LevelLightShare::OnEditStory(solving);
        const bool editRoom = editing && !own;
        // 2.13.1: a room of the last hold that has not ended a solve since its mark (trailing; never the lamp's own story
        // while it is not held): its maps keep what is on screen until that solve ends
        const bool trailRoom = solving && !editing && TrailingNow() && !(!HoldsOwnStory() && LevelLightShare::OnEditStory(solving)) &&
                               LevelLightShare::EditRoomWaiting(solving);
        const bool want = editRoom || trailRoom || (solving && !own && g_active.load(std::memory_order_relaxed) && LevelLightShare::InAtrium(solving));
        bool fits = false;
        if (want && !refining && LevelLightShare::InMapLockStep() && RoomMap(t, w, h)) {
            fits = g_maps.size() < kMaxMaps && g_bytes + static_cast<size_t>(w) * h * 8 <= kMaxBytes;
            // 12/10: a map of the edit not kept would show alone: the hold releases everything at once instead ("cap")
            if (!fits && editRoom) g_capHit.store(true, std::memory_order_relaxed);
        }
        if (!fits) {
            lk.unlock();
            called = true;
            return g_origLock(t, level, out, rect, flags);
        }
        lk.unlock();
        D3DLOCKED_RECT lr{};
        if (FAILED(g_origLock(t, 0, &lr, nullptr, D3DLOCK_READONLY))) {
            called = true;
            return g_origLock(t, level, out, rect, flags);
        }
        Map m;
        m.w = w;
        m.h = h;
        m.trail = trailRoom;
        try {
            m.latest.resize(static_cast<size_t>(w) * h);
            FromMemory(m, m.latest, RECT{0, 0, static_cast<LONG>(w), static_cast<LONG>(h)}, static_cast<const BYTE*>(lr.pBits), lr.Pitch);
            m.shown = m.latest;
        } catch (...) { // no memory for its copies: the game keeps the map as it writes it (a throw here would end the game)
            g_origUnlock(t, 0);
            called = true;
            return g_origLock(t, level, out, rect, flags);
        }
        g_origUnlock(t, 0);
        m.last = GetTickCount();
        lk.lock();
        it = g_maps.find(t);
        if (it == g_maps.end()) { // (another thread may have taken it meanwhile: then that record is used)
            bool added = false;
            if (g_maps.size() < kMaxMaps && g_bytes + static_cast<size_t>(w) * h * 8 <= kMaxBytes) {
                try {
                    it = g_maps.emplace(t, std::move(m)).first;
                    added = true;
                } catch (...) {
                }
            } else if (editRoom)
                g_capHit.store(true, std::memory_order_relaxed);
            if (!added) {
                lk.unlock();
                called = true;
                return g_origLock(t, level, out, rect, flags);
            }
            t->AddRef();
            g_bytes += static_cast<size_t>(w) * h * 8;
            g_count.store(g_maps.size(), std::memory_order_relaxed);
            g_peak.store(std::max<long>(g_peak.load(), static_cast<long>(g_maps.size())), std::memory_order_relaxed);
        }
    }
    lk.unlock();
    called = true;
    const HRESULT hr = g_origLock(t, level, out, rect, flags);
    if (FAILED(hr) || !out || !out->pBits) return hr;
    lk.lock();
    it = g_maps.find(t);
    if (it == g_maps.end()) return hr; // released meanwhile: the game has its lock as without Apex
    Map& m = it->second;
    g_gameLocks.fetch_add(1, std::memory_order_relaxed);
    if (m.held || m.release) m.resume = true; // the screen shows "shown": the game gets its exact content
    if (refining) m.refining = true;
    if (solving) m.room = solving;
    m.rect = Clip(m, rect);
    m.flags = flags;
    m.gameLocks++;
    if (m.resume) ToMemory(m, m.latest, m.rect, static_cast<BYTE*>(out->pBits), out->Pitch);
    m.bits = static_cast<BYTE*>(out->pBits);
    m.pitch = out->Pitch;
    return hr;
}

std::atomic<bool> g_lockFailed{false}; // LockHookBody threw: the game's maps pass straight through from then on

HRESULT __stdcall LockHook(IDirect3DTexture9* t, UINT level, D3DLOCKED_RECT* out, const RECT* rect, DWORD flags) {
    if (t_own || level != 0 || g_lockFailed.load(std::memory_order_relaxed) ||
        (!g_active.load(std::memory_order_relaxed) && !g_count.load(std::memory_order_relaxed) && !EditPendingNow() && !TrailingNow()))
        return g_origLock(t, level, out, rect, flags);
    bool called = false;
    try {
        return LockHookBody(t, level, out, rect, flags, called);
    } catch (...) {
        g_lockFailed.store(true, std::memory_order_relaxed);
        HookGuard::Note("AtriumHold map lock");
    }
    // once the game's lock is made, only its success is followed by code that can throw
    return called ? D3D_OK : g_origLock(t, level, out, rect, flags);
}

HRESULT __stdcall UnlockHook(IDirect3DTexture9* t, UINT level) {
    if (level == 0 && RtOutdoorLamps::Watching()) RtOutdoorLamps::NoteUnlock(t); // (Instant: a relit copy starts from the map again)
    if (t_own || level != 0 || !g_count.load(std::memory_order_relaxed)) return g_origUnlock(t, level);
    {
        std::lock_guard<std::mutex> lk(g_mx);
        const auto it = g_maps.find(t);
        if (it != g_maps.end() && it->second.gameLocks > 0) {
            Map& m = it->second;
            m.gameLocks--;
            bool changed = false;
            if (m.bits && !(m.flags & D3DLOCK_READONLY)) changed = FromMemory(m, m.latest, m.rect, m.bits, m.pitch);
            if (m.refining) { // another class's maps: shown only when the solve ends, already with this content
                if (changed || m.resume) g_refinements.fetch_add(1, std::memory_order_relaxed);
                m.shown = m.latest;
                m.held = m.release = m.resume = m.refining = false;
            } else if (changed || m.resume) {
                // a lamp edit shown all at once: every map of its rooms on another story than the lamp's waits for the
                // edit hold's release (a drag too: those stories keep their maps from before it); else an atrium's for its
                // stories. 12/10: the lamp's own story never waits while an edit is held (OnEditStory).
                const bool editing = m.room && EditPendingNow();
                const bool own = editing && !HoldsOwnStory() && LevelLightShare::OnEditStory(m.room);
                // Quick outdoor lamp edits (Lamp lighting: Instant only, outdoor_delta.h): a room 0 brought up to date by a delta
                // shows at once, on every story (its solve costs no more than a frame, so nothing is gained by waiting)
                const bool delta = OutdoorDelta::Enabled() && (OutdoorDelta::DirectWriting() || (m.room && OutdoorDelta::DeltaSolving(m.room)));
                const bool wasEdit = m.sw || m.trail;
                m.sw = editing && !own && !delta && LevelLightShare::LampUrgency(m.room) > 1.0f;
                // 2.13.1: after the hold, a map of a room still waiting for its own solve keeps what is on screen (trailing)
                m.trail = m.room && !editing && !delta && wasEdit && TrailingNow() && LevelLightShare::EditRoomWaiting(m.room);
                m.sw = m.sw || m.trail;
                const bool wait = m.room && !delta && (m.sw || (!own && !LevelLightShare::LampDragging() && LevelLightShare::GroupPending(m.room)));
                if (wait) { // what was on screen goes back until the atrium's other stories are solved
                    if (m.bits) ToMemory(m, m.shown, m.rect, m.bits, m.pitch);
                    if (!m.held) {
                        g_held.fetch_add(1, std::memory_order_relaxed);
                        m.heldAt = GetTickCount();
                    } else if (changed && m.sw)
                        g_holdRewrites.fetch_add(1, std::memory_order_relaxed); // (written again while held: never shown)
                    m.held = true;
                    m.release = false;
                } else { // the game's content stays as it wrote it
                    m.shown = m.latest;
                    m.held = m.release = m.trail = false;
                }
                m.resume = false;
            }
            m.last = GetTickCount();
            m.bits = nullptr;
        }
    }
    return g_origUnlock(t, level);
}

// The file name of the module holding that code ("d3d9.dll" for DXVK and Windows' own)
std::wstring ModuleOf(const void* code) {
    HMODULE m = nullptr;
    wchar_t path[MAX_PATH] = {};
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, static_cast<LPCWSTR>(code), &m) ||
        !GetModuleFileNameW(m, path, MAX_PATH))
        return L"";
    std::wstring name = path;
    const size_t slash = name.find_last_of(L"\\/");
    if (slash != std::wstring::npos) name.erase(0, slash + 1);
    for (wchar_t& c : name) c = towlower(c);
    return name;
}

bool InstallHooks(IDirect3DDevice9* dev) {
    IDirect3DTexture9* probe = nullptr;
    if (FAILED(dev->CreateTexture(1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &probe, nullptr)) || !probe) return false;
    void** vt = *reinterpret_cast<void***>(probe);
    g_origLock = reinterpret_cast<LockRect_t>(vt[19]);
    g_origUnlock = reinterpret_cast<UnlockRect_t>(vt[20]);
    probe->Release();
    // Only the texture code of a d3d9.dll (DXVK, Windows' own) is patched: a wrapper's (dxwrapper.dll, 06/10: a player's
    // game crashed in it after the first loading screen) may fold identical methods of other interfaces into these two,
    // and the device hooks refuse folded slots for the same reason (D3D9Hooks::Install)
    const std::wstring lockIn = ModuleOf(reinterpret_cast<const void*>(g_origLock)), unlockIn = ModuleOf(reinterpret_cast<const void*>(g_origUnlock));
    if (lockIn != L"d3d9.dll" || unlockIn != L"d3d9.dll" || reinterpret_cast<void*>(g_origLock) == reinterpret_cast<void*>(g_origUnlock)) {
        LOG_INFO(std::format("[AtriumHold] Texture locks are in {}, not a d3d9.dll: left alone (lamp switches change room by room)",
                             ApexUtil::ToUtf8(lockIn)));
        g_origLock = nullptr;
        g_origUnlock = nullptr;
        return false;
    }
    std::vector<DetourBatch::Hook> hooks = {{reinterpret_cast<void**>(&g_origLock), reinterpret_cast<void*>(&LockHook)},
                                            {reinterpret_cast<void**>(&g_origUnlock), reinterpret_cast<void*>(&UnlockHook)}};
    return DetourBatch::InstallHooks(hooks);
}

// Under g_mx (OnPresent): the released maps take their content, idle maps are let go (dropAll, after a hold ended at the
// cap: every map not waiting, so the next edit does not find the table still full for kKeepMs)
void WriteReleased(DWORD now, bool dropAll) {
    for (auto it = g_maps.begin(); it != g_maps.end();) {
        Map& m = it->second;
        IDirect3DTexture9* tex = it->first;
        if (m.gameLocks > 0 || m.held) { ++it; continue; } // never written while the game holds it; held: shows "shown"
        if (m.release) {
            if (WriteLatest(tex, m)) g_writes.fetch_add(1, std::memory_order_relaxed);
            m.shown = m.latest;
            m.release = false;
            if (!dropAll) {
                ++it;
                continue;
            }
        }
        if (dropAll || now - m.last > kKeepMs) { // idle: buffers and the reference go
            tex->Release();
            g_bytes -= std::min(g_bytes, static_cast<size_t>(m.w) * m.h * 8);
            it = g_maps.erase(it);
            continue;
        }
        ++it;
    }
    g_count.store(g_maps.size(), std::memory_order_relaxed);
}

// How an edit hold ended (the log line, after its maps are released in the same frame)
enum class HoldEnd { None, Complete, Timeout, Cap, Off };
struct HoldResult {
    HoldEnd end = HoldEnd::None;
    DWORD sinceFirst = 0;
    long ownMs = -1;
    int others = 0, pending = 0;
    bool together = false; // the hold ran under "show all floors together" (others then counts the own story's rooms too)
    bool ownFirst = false; // (together) ended once the own rooms were solved, before the rooms around them
    long afterMs = -1;     // ms from the drag's end (after: 1 let go, 2 the still time) or else the latest edit
    int after = 0;
    long rewrites = 0;     // held maps written again during the hold (content never shown)
};

// Render thread, every frame: a new player edit starts (or extends) the hold; it ends once every room the edits marked on
// another story than the lamps' own has ended a solve begun after its mark (and, for a switch, the ground's chunks are
// written), 2.5 s after the latest edit or 4 s after the first (a drag: from its release), or when the kept maps hit the cap
HoldResult UpdateEditHold() {
    HoldResult r;
    if (!AllAtOnceUsable()) {
        if (g_editHolding.exchange(false, std::memory_order_relaxed)) r.end = HoldEnd::Off;
        g_holdHasSwitch.store(false, std::memory_order_relaxed);
        return r;
    }
    const DWORD now = GetTickCount();
    g_holdUpdated.store(now, std::memory_order_relaxed);
    // read before the tick: an edit arriving meanwhile is then counted in the next hold too, never lost from it
    const unsigned long serialNow = LampMarkFilter::EditSerial();
    const unsigned long last = LampMarkFilter::EditLastTick();
    bool holding = g_editHolding.load(std::memory_order_relaxed);
    if (last && last != g_editSeen.load(std::memory_order_relaxed)) { // an edit since the last frame (an old one, e.g. before the option was on, starts nothing)
        g_editSeen.store(last, std::memory_order_relaxed);
        if (now - last < 1000) {
            if (!holding) {
                g_holdFirst = g_holdStart = last;
                g_holdFrom = g_doneSerial;
                g_ownShownMs = -1;
                g_holdDragged = false;
                g_holdPeakMaps = 0;
                g_ownReadyAt = 0;
                g_holdRewrites.store(0, std::memory_order_relaxed);
                g_capHit.store(false, std::memory_order_relaxed);
                g_holdTogether.store(TogetherNow(), std::memory_order_relaxed);
                g_editHolds.fetch_add(1, std::memory_order_relaxed);
            } else if (g_holdTogether.load(std::memory_order_relaxed) && !TogetherNow()) {
                // (one-way) a drag the GPU takes leaves "all floors together" 750 ms after its last refused step: latched, the
                // whole drag and its release showed a frozen mid-drag map for up to 5 s, then snapped back (10:06 recording, 03/11)
                g_holdTogether.store(false, std::memory_order_relaxed);
            }
            g_holdLatest = last;
            holding = true;
            g_editHolding.store(true, std::memory_order_relaxed);
        }
    }
    if (const unsigned long sw = LampMarkFilter::SwitchLastTick(); sw && sw != g_switchSeen.load(std::memory_order_relaxed)) {
        g_switchSeen.store(sw, std::memory_order_relaxed);
        if (holding && now - sw < 1000) g_holdHasSwitch.store(true, std::memory_order_relaxed);
    }
    if (!holding) return r;
    const bool dragging = LevelLightShare::LampDragging();
    if (dragging) { // one hold per settled drag: its 4 s cap counts from its release, its other stories keep their maps
        g_holdDragged = true;
        g_holdFirst = now;
    }
    const DWORD sinceLatest = now - g_holdLatest, sinceFirst = now - g_holdFirst;
    const bool cap = g_capHit.load(std::memory_order_relaxed);
    // a hold kept on by edits that never stop (a drag of more than kMapMaxMs, or a lamp whose values change by themselves
    // every update, which counts as a drag: unverified that any does) ends as a timeout; the next edit starts another
    const bool endless = now - g_holdStart >= kMapMaxMs;
    if (!cap && !endless && dragging) return r; // (EditHoldOutcome waits too: no room walk needed)
    int others = 0, ownPending = 0, ownTotal = 0;
    const int pending = LevelLightShare::EditRoomsPending(g_holdFrom, &others, &ownPending, &ownTotal);
    // the own story's time (the log): its rooms marked and all solved (before kEditMinMs too, once one of them is marked)
    if (g_ownShownMs < 0 && ownPending == 0 && (ownTotal > 0 || sinceLatest >= kEditMinMs)) g_ownShownMs = static_cast<long>(now - g_holdFirst);
    // held only for switches (and every edit under "show all floors together")
    const bool ground = (g_holdHasSwitch.load(std::memory_order_relaxed) || g_holdTogether.load(std::memory_order_relaxed)) && LightmapSmooth::HoldPending();
    // the edit's safety net (LampMarkFilter::OnPresent, 120-500 ms after the edit) still to send its rooms: they are tagged
    // urgent then and held with the others (a net later than kNetWaitMs, a lot refreshed a moment ago, is not waited for)
    const bool net = LampMarkFilter::LotRefreshDueSoon(kNetWaitMs);
    const bool together = g_holdTogether.load(std::memory_order_relaxed);
    // show all floors together: the own story's rooms count too, with the longer bound (kTogetherMaxMs from the first edit,
    // a drag: from its release)
    const int waiting = pending + (together ? ownPending : 0);
    int outcome = cap ? 3 : endless ? 2
                : together ? RoomAmbientPolicy::EditHoldOutcomeTogether(waiting + (net ? 1 : 0), ground, sinceLatest, sinceFirst, dragging, kTogetherMaxMs)
                           : RoomAmbientPolicy::EditHoldOutcome(pending + (net ? 1 : 0), ground, sinceLatest, sinceFirst, dragging);
    // (14/10, drag solves) a dragged lamp's hold ends once its own story's rooms show: the rooms that waited for the stop
    // (other stories, through openings; DragLite) are solved under the spread cap afterwards and show as each is solved
    // (not under "show all floors together": those rooms are the edit's then, and the hold waits for them)
    if (!outcome && !together && g_holdDragged && !dragging && TesterToggles::dragDeferSends.load(std::memory_order_relaxed) && ownTotal > 0 && ownPending == 0)
        outcome = 1;
    // (2.13.1) show all floors together: the own rooms (tier 0) solved, the rooms around them get kTogetherOwnGraceMs more
    bool ownFirst = false;
    if (!outcome && together && !dragging && TesterToggles::fastOwnRelight.load(std::memory_order_relaxed)) {
        int tierTotal = 0;
        const int tierPending = LevelLightShare::OwnRoomsPending(&tierTotal);
        if (tierTotal > 0 && tierPending == 0) {
            if (!g_ownReadyAt) g_ownReadyAt = now | 1;
        } else
            g_ownReadyAt = 0;
        const DWORD grace = OwnAtOnce() ? 0 : kTogetherOwnGraceMs;
        if (g_ownReadyAt && static_cast<int32_t>(now - g_ownReadyAt) >= static_cast<int32_t>(grace) && sinceLatest >= kEditMinMs)
            outcome = 1, ownFirst = waiting > 0 || net || ground;
    }
    if (!outcome) return r;
    r.ownFirst = ownFirst;
    // (2.13.1) the log: the time from the drag's end (let go or the still time) or the latest edit to this frame
    r.afterMs = static_cast<long>(sinceLatest);
    if (const DWORD rel = DragLite::LastReleaseTick(); g_holdDragged && rel && now - rel < 10000) {
        r.afterMs = static_cast<long>(now - rel);
        r.after = DragLite::LastReleaseLetGo() ? 1 : 2;
    }
    r.rewrites = g_holdRewrites.exchange(0, std::memory_order_relaxed);
    // maps of rooms not solved since their mark keep what is on screen until they are (not at the cap: it shows everything)
    if (OwnAtOnce() && outcome != 3) g_trailUntil.store((now + kTrailMaxMs) | 1, std::memory_order_relaxed);
    r.end = outcome == 1 ? HoldEnd::Complete : outcome == 2 ? HoldEnd::Timeout : HoldEnd::Cap;
    r.sinceFirst = sinceFirst;
    r.ownMs = g_ownShownMs;
    r.others = others + (together ? ownTotal : 0);
    r.pending = waiting + (ground ? 1 : 0);
    r.together = together;
    g_doneSerial = serialNow;
    g_editHolding.store(false, std::memory_order_relaxed);
    g_holdHasSwitch.store(false, std::memory_order_relaxed);
    g_capHit.store(false, std::memory_order_relaxed);
    return r;
}

} // namespace

namespace AtriumHold {

void SetAllAtOnce(bool on) {
    if (!g_allAtOnce.exchange(on) || on) return;
    g_editHolding.store(false, std::memory_order_relaxed); // switched off during a hold: it ends now (OnPresent releases)
    g_holdHasSwitch.store(false, std::memory_order_relaxed);
}
bool AllAtOnce() { return AllAtOnceUsable() && g_hooked.load(std::memory_order_relaxed); }
// held, or a switch in this very frame, before the next Present starts the hold: the furniture and lamps already wait
bool SwitchHolding() { return g_hooked.load(std::memory_order_relaxed) && SwitchPendingNow(); }
bool EditHolding() { return g_hooked.load(std::memory_order_relaxed) && EditPendingNow(); }
bool OwnStoryHeld() { return g_hooked.load(std::memory_order_relaxed) && EditPendingNow() && HoldsOwnStory(); }
bool Trailing() {
    return g_hooked.load(std::memory_order_relaxed) && TrailingNow() && (g_trailMaps.load(std::memory_order_relaxed) > 0 || LevelLightShare::LampEditPending());
}
bool RoomShowsOldLight(const void* room) {
    if (!room || !g_hooked.load(std::memory_order_relaxed)) return false;
    if (EditPendingNow()) {
        if (HoldsOwnStory() || !LevelLightShare::OnEditStory(room)) return LevelLightShare::LampUrgency(room) > 1.0f; // its maps are held
        return LevelLightShare::EditRoomWaiting(room); // the own story shows each room once solved
    }
    if (!TrailingNow() || (!HoldsOwnStory() && LevelLightShare::OnEditStory(room))) return false;
    return LevelLightShare::EditRoomWaiting(room);
}

void OnPresent(IDirect3DDevice9* dev) {
    if (!g_hookTried.exchange(true)) {
        const bool ok = dev && InstallHooks(dev);
        g_hooked.store(ok);
        LOG_INFO(ok ? "[AtriumHold] Texture lock hooks installed: an atrium's stories take their new light together during lamp edits"
                    : "[AtriumHold] Texture lock hooks not available: an atrium's stories change as each is solved");
    }
    if (!g_hooked.load()) {
        g_editHolding.store(false, std::memory_order_relaxed);
        g_holdHasSwitch.store(false, std::memory_order_relaxed);
        return;
    }
    const HoldResult ended = UpdateEditHold();
    const bool editing = g_editHolding.load(std::memory_order_relaxed);
    g_active.store(LevelLightShare::LampEditPending() || LampMarkFilter::MassSwitchActive() || editing, std::memory_order_relaxed);
    int releasedMaps = 0, trailMaps = 0, trailNow = 0;
    size_t releasedBytes = 0;
    if (ended.end == HoldEnd::Cap) g_trailUntil.store(0, std::memory_order_relaxed); // (the cap shows everything at once)
    if (g_count.load(std::memory_order_relaxed)) {
        std::lock_guard<std::mutex> lk(g_mx);
        const DWORD now = GetTickCount();
        if (editing) g_holdPeakMaps = std::max(g_holdPeakMaps, g_maps.size());
        const bool trailing = !editing && TrailingNow();
        // every waiting map whose atrium is solved, or whose lamp edit hold is over, takes its content in this frame
        for (auto& [tex, m] : g_maps) {
            if (!m.held || m.gameLocks != 0) continue;
            if (m.sw) { // the edit hold decides (its own timeouts); kMapMaxMs only bounds a map through a very long drag
                // (an own-story map is released once the hold no longer runs together: nothing writes it again to free it)
                if (editing && now - m.heldAt < kMapMaxMs && !(m.room && !HoldsOwnStory() && LevelLightShare::OnEditStory(m.room))) continue;
                // 2.13.1: its room has not ended a solve since its mark: what is on screen stays until it has
                if (trailing && m.room && now - m.heldAt < kMapMaxMs && LevelLightShare::EditRoomWaiting(m.room)) {
                    if (!m.trail) trailMaps++;
                    m.trail = true;
                    trailNow++;
                    continue;
                }
                m.trail = false;
                if (editing) g_timeouts.fetch_add(1, std::memory_order_relaxed);
                releasedMaps++;
                releasedBytes += static_cast<size_t>(m.w) * m.h * 4;
            } else if (now - m.heldAt >= kHoldMs)
                g_timeouts.fetch_add(1, std::memory_order_relaxed);
            else if (m.room && LevelLightShare::GroupPending(m.room))
                continue;
            m.held = false;
            m.release = true;
            g_released.fetch_add(1, std::memory_order_relaxed);
        }
        WriteReleased(now, ended.end == HoldEnd::Cap);
    }
    g_trailMaps.store(trailNow, std::memory_order_relaxed);
    if (ended.end == HoldEnd::None) return;
    const char* how = ended.end == HoldEnd::Complete ? "complete" : ended.end == HoldEnd::Timeout ? "timeout" : ended.end == HoldEnd::Cap ? "cap" : "option off";
    if (ended.end == HoldEnd::Timeout) g_editTimeouts.fetch_add(1, std::memory_order_relaxed);
    if (ended.end == HoldEnd::Cap) g_editCaps.fetch_add(1, std::memory_order_relaxed);
    g_editLastMs.store(static_cast<long>(ended.sinceFirst), std::memory_order_relaxed);
    g_editLastOwnMs.store(ended.ownMs, std::memory_order_relaxed);
    g_editLastMaps.store(releasedMaps, std::memory_order_relaxed);
    g_editLastRooms.store(ended.others, std::memory_order_relaxed);
    g_editPeakMaps.store(std::max<long>(g_editPeakMaps.load(std::memory_order_relaxed), static_cast<long>(g_holdPeakMaps)), std::memory_order_relaxed);
    const std::string own = ended.ownMs >= 0 ? std::format("{} ms", ended.ownMs) : std::string("not yet");
    // (2.13.1) when it was shown from the drag's end or the latest edit, the content kept back, the rooms still trailing
    std::string shown = ended.afterMs >= 0 ? std::format("; shown {} ms after {}", ended.afterMs,
                                                          ended.after == 1 ? "the lamp was let go" : ended.after == 2 ? "the drag's still time" : "the latest edit")
                                           : std::string();
    if (ended.rewrites > 0) shown += std::format("; {} later map writes kept back (never shown)", ended.rewrites);
    if (trailMaps > 0) shown += std::format("; {} maps keep their light until their room's own solve ends", trailMaps);
    if (ended.together) // Classic: show all floors together (one line per edit; a timeout is this line, once)
        LOG_INFO(std::format("[AtriumHold] Classic lamp edit{}, all floors together: {} maps held ({:.1f} MB), {} after {} ms{}{}",
                             g_holdDragged ? " (drag, from its release)" : "", releasedMaps, static_cast<double>(releasedBytes) / (1024.0 * 1024.0),
                             ended.ownFirst ? "the lamp's own rooms ready" : ended.end == HoldEnd::Complete ? "everything ready" : ended.end == HoldEnd::Timeout ? "timed out" : how,
                             ended.sinceFirst,
                             ended.pending <= 0 ? std::string()
                             : ended.ownFirst   ? std::format(" ({} of {} rooms around them follow as each is solved)", ended.pending, ended.others)
                                                : std::format(" ({} of {} rooms not ready: what was ready is shown)", ended.pending, ended.others),
                             shown));
    else LOG_INFO(std::format("[AtriumHold] Lamp edit{}: own story shown {}, others released together {} ms ({} maps, {:.1f} MB; {}){}{}",
                         g_holdDragged ? " (drag, from its release)" : "", own, ended.sinceFirst, releasedMaps,
                         static_cast<double>(releasedBytes) / (1024.0 * 1024.0), how,
                         ended.pending > 0 ? std::format(", {} of {} rooms still waiting", ended.pending, ended.others) : std::string(), shown));
    if (Recorder::Active()) // the recording's timeline: when the edit was shown
        Recorder::Note(std::format("[switch] lamp edit shown all at once, {} ms after its first edit ({} rooms of other stories, {} maps; {})", ended.sinceFirst,
                                   ended.others, releasedMaps, how));
}

void Clear() {
    std::lock_guard<std::mutex> lk(g_mx);
    for (auto& [tex, m] : g_maps) {
        if ((m.held || m.release) && m.gameLocks == 0) WriteLatest(tex, m); // a wait cut short: the map gets its content
        tex->Release();
    }
    g_maps.clear();
    g_bytes = 0;
    g_count.store(0, std::memory_order_relaxed);
    // the queue stopped (or the world changed): nothing waits or is kept until the next Present runs the hold again (review
    // 06/10: a switch after Faster Room Lighting went off kept capturing maps nobody released)
    g_active.store(false, std::memory_order_relaxed);
    g_editHolding.store(false, std::memory_order_relaxed);
    g_holdHasSwitch.store(false, std::memory_order_relaxed);
    g_capHit.store(false, std::memory_order_relaxed);
    g_trailUntil.store(0, std::memory_order_relaxed);
    g_holdUpdated.store(GetTickCount() - 1000, std::memory_order_relaxed);
}

std::string Status() {
    if (!g_hooked.load()) return g_hookTried.load() ? "not available" : "waiting for the device";
    const long lastMs = g_editLastMs.load(), ownMs = g_editLastOwnMs.load();
    return std::format("maps kept {} (at most {} at once), maps that waited for their other stories or their edit {} (released together {}, "
                       "after the longest wait {}), refinements left alone {}, writes {}, game locks seen {} | lamp edits all at once: {}, {} shown{}, "
                       "timeouts {}, released at the map cap {}, most maps kept by one edit {}{}",
                       g_count.load(), g_peak.load(), g_held.load(), g_released.load(), g_timeouts.load(), g_refinements.load(),
                       g_writes.load(), g_gameLocks.load(), g_allAtOnce.load() ? "on" : "off", g_editHolds.load(),
                       lastMs >= 0 ? std::format(" (the last: own story after {}, the others together after {} ms, {} maps, {} rooms of other stories)",
                                                 ownMs >= 0 ? std::format("{} ms", ownMs) : std::string("not measured"), lastMs, g_editLastMaps.load(), g_editLastRooms.load())
                                   : std::string(),
                       g_editTimeouts.load(), g_editCaps.load(), g_editPeakMaps.load(),
                       g_editHolding.load() ? (g_holdHasSwitch.load() ? " | holding a switch" : " | holding an edit") : "");
}

} // namespace AtriumHold
