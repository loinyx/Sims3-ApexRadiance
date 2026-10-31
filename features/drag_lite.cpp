// Lamp drags: coalesced room solves. See drag_lite.h.
#include "drag_lite.h"
#include "tester_toggles.h"
#include "level_light_share.h"
#include "lamp_mark_filter.h"
#include "apex_log.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <bit>
#include <format>
#include <iterator>
#include <mutex>
#include <set>
#include <tuple>
#include <unordered_set>

namespace DragLite {

thread_local Why t_why = Why::None;

namespace {
std::atomic<DWORD> g_lastStep{0}; // GetTickCount | 1 of the latest lamp step
std::atomic<DWORD> g_runFrom{0};  // GetTickCount | 1 of the first step of the current run (steps less than kReleaseMs apart)
std::atomic<bool> g_classic{true}; // Lamp lighting is Classic in effect (SetClassicLighting)
// Classic, relight only after release: the rooms held during the current drag (g_solveMx), the drag's log line after it
std::unordered_set<const void*> g_heldRooms;
bool g_classicDrag = false;               // render thread: this drag ran with the classic hold
std::atomic<bool> g_ownFirst{false};      // render thread writes: after the release, the lamp's own story still waits
std::atomic<bool> g_tracking{false};      // after a classic drag's release until its rooms are solved
std::atomic<long> g_trackSolves{0};       // room solves begun since that release
DWORD g_trackFrom = 0;                    // render thread: the release (GetTickCount)
size_t g_trackHeld = 0;                   // render thread: rooms held during that drag
std::string g_trackDrag;                  // render thread: that drag's own summary (frames, sends), joined to its one log line
std::mutex g_mx;
std::set<std::tuple<uintptr_t, int, int>> g_rooms; // (tracker, story, id): a send left out during the drag
std::atomic<long> g_deferredSends{0}, g_repicksHeld{0}, g_dragSolves{0};
long g_drags = 0, g_totalResent = 0, g_totalHeld = 0; // render thread (the totals of every drag so far)
bool g_wasEngaged = false;
std::atomic<DWORD> g_settleFrom{0}; // GetTickCount | 1 when the last drag ended
std::string g_last = "no drag yet";

// The room solves begun lately (render / light tree thread, under g_solveMx): the re-pick throttle
struct Started {
    const void* room = nullptr;
    DWORD at = 0;
};
std::mutex g_solveMx;
Started g_started[64];
int g_startedPos = 0;
std::atomic<DWORD> g_lastSolveAt{0};

// Frame timers for the Tester tab (render thread writes, float bits)
LARGE_INTEGER g_prevFrame{}, g_freq{};
std::atomic<uint32_t> g_frameMs{0}, g_dragAvgMs{0}, g_dragMaxMs{0};
std::atomic<int> g_dragFrames{0};
double g_runSum = 0.0, g_runMax = 0.0;
int g_runFrames = 0;
} // namespace

namespace {
std::atomic<DWORD> g_lastMove{0};     // GetTickCount | 1 of the latest move step (StepKind::Move)
std::atomic<DWORD> g_lastGameMove{0}; // ... of a lamp the game solves
std::atomic<bool> g_ownWaiting{false}; // fastOwnRelight: the lamps' own rooms of the current edit still wait (render thread)
bool Within(const std::atomic<DWORD>& at, unsigned long ms) {
    const DWORD t = at.load(std::memory_order_relaxed);
    return t && GetTickCount() - t < ms;
}
bool FastOwn() { return TesterToggles::fastOwnRelight.load(std::memory_order_relaxed); }

// Let go (dragLetGo): the mouse button that held a lamp move, polled once a frame on the render thread
std::atomic<DWORD> g_letGoAt{0};          // GetTickCount | 1: the button released after a move made while it was down
DWORD g_buttonDownAt = 0;                 // render thread: the button went down (GetTickCount | 1; 0 = up)
std::atomic<DWORD> g_releaseAt{0};        // the latest drag's end (GetTickCount | 1)
std::atomic<bool> g_releaseLetGo{false};  // ... seen as let go (else the still time)
std::atomic<DWORD> g_releaseStepGap{0};   // ... ms between its last move step and its end
bool LetGoOn() { return TesterToggles::dragLetGo.load(std::memory_order_relaxed); }
bool GameInFront() {
    const HWND front = GetForegroundWindow();
    DWORD pid = 0;
    return front && GetWindowThreadProcessId(front, &pid) && pid == GetCurrentProcessId();
}
void PollButton() {
    if (!LetGoOn()) {
        g_letGoAt.store(0, std::memory_order_relaxed);
        g_buttonDownAt = 0;
        return;
    }
    // (GetAsyncKeyState reads the physical buttons: the primary one is the right one when they are swapped)
    const int vk = GetSystemMetrics(SM_SWAPBUTTON) ? VK_RBUTTON : VK_LBUTTON;
    const bool down = GameInFront() && (GetAsyncKeyState(vk) & 0x8000) != 0;
    const DWORD now = GetTickCount() | 1;
    if (down) {
        if (!g_buttonDownAt) g_buttonDownAt = now;
        return;
    }
    if (!g_buttonDownAt) return;
    const DWORD from = g_buttonDownAt;
    g_buttonDownAt = 0;
    const DWORD move = g_lastMove.load(std::memory_order_relaxed);
    // (held at least kMinHoldMs: a click that picks a lamp up, then moved with the button up, is no let go)
    constexpr DWORD kMinHoldMs = 150;
    if (move && static_cast<int32_t>(move - from) >= 0 && now - move < kReleaseMs && now - from >= kMinHoldMs)
        g_letGoAt.store(now, std::memory_order_relaxed);
}
} // namespace

bool LetGo() {
    const DWORD at = g_letGoAt.load(std::memory_order_relaxed);
    if (!at || !LetGoOn() || GetTickCount() - at >= kLetGoMs) return false;
    const DWORD move = g_lastMove.load(std::memory_order_relaxed);
    return !move || static_cast<int32_t>(move - at) < static_cast<int32_t>(kPlaceGraceMs); // (a later step: a new drag)
}
unsigned long LastReleaseTick() { return g_releaseAt.load(std::memory_order_relaxed); }
bool LastReleaseLetGo() { return g_releaseLetGo.load(std::memory_order_relaxed); }

void NoteLampStep(const char*, StepKind kind, bool gameSolved) {
    const DWORD now = GetTickCount() | 1;
    const DWORD last = g_lastStep.exchange(now, std::memory_order_relaxed);
    if (!last || now - last >= kReleaseMs) g_runFrom.store(now, std::memory_order_relaxed);
    if (kind != StepKind::Move) return;
    g_lastMove.store(now, std::memory_order_relaxed);
    if (gameSolved) g_lastGameMove.store(now, std::memory_order_relaxed);
}
bool StepWithin(unsigned long ms) { return Within(g_lastStep, ms); }

void SetClassicLighting(bool on) { g_classic.store(on, std::memory_order_relaxed); }
bool ClassicLighting() { return g_classic.load(std::memory_order_relaxed); }
bool ClassicReleaseOn() {
    return TesterToggles::classicAfterRelease.load(std::memory_order_relaxed) && (g_classic.load(std::memory_order_relaxed) || FastOwn());
}
bool ClassicRulesForDrag() { return ClassicLighting() || (FastOwn() && Within(g_lastGameMove, kStillMs)); }
bool OwnRoomsWaiting() { return g_ownWaiting.load(std::memory_order_relaxed); }
bool ClassicHolding() {
    if (!ClassicReleaseOn() || LetGo()) return false;
    bool dragged;
    if (!FastOwn()) dragged = StepWithin(kReleaseMs) || LevelLightShare::LampDragging(); // (2.13.0: any step, values too)
    else { // moves only (a colour is no drag); Instant: of a lamp the game solves. A drag's marks keep it while they go on
        const std::atomic<DWORD>& move = ClassicLighting() ? g_lastMove : g_lastGameMove;
        dragged = Within(move, kReleaseMs) || (LevelLightShare::LampDragging() && Within(move, 2000));
    }
    if (!dragged) return false;
    // (a run of steps longer than kMaxHoldMs: the rooms are solved as the lamp moves, as without the hold)
    const DWORD from = g_runFrom.load(std::memory_order_relaxed);
    return !(from && StepWithin(kReleaseMs) && GetTickCount() - from > kMaxHoldMs);
}
bool HoldUntilRelease(const void* room, bool onPlayedLot) {
    if (!room || !ClassicHolding()) return false;
    if (!onPlayedLot && LevelLightShare::UrgentTierOf(room) < 0) return false; // another lot's own work goes on
    std::lock_guard<std::mutex> lk(g_solveMx);
    if (g_heldRooms.size() < 4096) g_heldRooms.insert(room);
    return true;
}

bool Dragging() { return StepWithin(kDragMs) || LevelLightShare::LampDragging(); }
bool LampMoving() {
    if (!Within(g_lastMove, kStillMs)) return false;
    const DWORD at = g_letGoAt.load(std::memory_order_relaxed), move = g_lastMove.load(std::memory_order_relaxed);
    return !(at && LetGoOn() && static_cast<int32_t>(move - at) < static_cast<int32_t>(kPlaceGraceMs));
}
bool Engaged() {
    // (the release rule's shorter still time: Classic, or with fastOwnRelight a drag of a lamp the game solves)
    const bool release = ClassicReleaseOn() && (!FastOwn() || ClassicRulesForDrag());
    if (release && LetGo()) return false; // (the button let go of the lamp: no still time)
    return StepWithin(release ? kReleaseMs : kStillMs) || LevelLightShare::LampDragging();
}
bool Settling() {
    const DWORD from = g_settleFrom.load(std::memory_order_relaxed);
    return from && GetTickCount() - from < kSettleMs && !Engaged() && LevelLightShare::LampEditPending();
}
float SolveCapMs() {
    if (!TesterToggles::dragSpreadCap.load(std::memory_order_relaxed) || !Settling()) return 0.0f;
    // 2.13.1: the lamps' own rooms (tier 0) take the edit budget first, after any edit; the rooms around them the spread cap
    if (FastOwn()) return g_ownWaiting.load(std::memory_order_relaxed) ? 0.0f : kSpreadCapMs;
    // (2.13.0) Classic, relight only after release: the lamp's own story takes the edit budget first, the others the spread cap
    return ClassicReleaseOn() && g_ownFirst.load(std::memory_order_relaxed) ? 0.0f : kSpreadCapMs;
}

void NoteSolveStarted(const void* room) {
    const DWORD now = GetTickCount() | 1;
    g_lastSolveAt.store(now, std::memory_order_relaxed);
    if (Engaged()) g_dragSolves.fetch_add(1, std::memory_order_relaxed);
    else if (g_tracking.load(std::memory_order_relaxed)) g_trackSolves.fetch_add(1, std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(g_solveMx);
    for (Started& s : g_started)
        if (s.room == room) {
            s.at = now;
            return;
        }
    g_started[g_startedPos] = Started{room, now};
    g_startedPos = (g_startedPos + 1) % static_cast<int>(std::size(g_started));
}

bool HoldRepick(const void* room) {
    if (!room || !TesterToggles::dragRepickThrottle.load(std::memory_order_relaxed) || !Engaged()) return false;
    const DWORD now = GetTickCount();
    // the rooms that take the dragged lamp's light from elsewhere (urgency tier 1: through an opening, another story; tier 2:
    // the edit's safety net) keep their light until the lamp is still, as the game's own outdoor light settles after a move;
    // the lamp's own room and story (tier 0) and any other room follow it at most every kRepickMs, and at most one room solve
    // begins every kSolveGapMs (the lamp's own room is picked first: its priority is the highest)
    bool hold = LevelLightShare::UrgentTierOf(room) >= 1;
    const DWORD last = g_lastSolveAt.load(std::memory_order_relaxed);
    hold = hold || (last && now - last < kSolveGapMs);
    if (!hold) {
        std::lock_guard<std::mutex> lk(g_solveMx);
        for (const Started& s : g_started)
            if (s.room == room) {
                hold = now - s.at < kRepickMs;
                break;
            }
    }
    if (hold) g_repicksHeld.fetch_add(1, std::memory_order_relaxed);
    return hold;
}

bool DeferSend(uintptr_t tracker, int story, int id) {
    const Why why = t_why;
    if (why < Why::Fanout || why > Why::Census || !TesterToggles::dragDeferSends.load(std::memory_order_relaxed) || !Engaged()) return false;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        if (g_rooms.size() >= 4096) return false; // (full: sent now, as without the coalescing)
        g_rooms.emplace(tracker, story, id);
    }
    g_deferredSends.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// ---- 2.13.1: one lamp edit's relight (render thread). From the edit (its latest mark) or a drag's release until no room of
// the edit waits: the lamps' own rooms' time until solved (OwnRoomsWaiting lifts the spread cap meanwhile) and the time
// until the rooms around them settled. Logged once per edit that has own rooms; a watchdog line when they still wait 3 s on.
namespace {
constexpr DWORD kOwnWatchMs = 3000;
constexpr DWORD kTrackMaxMs = kSettleMs + 5000; // (stops following an edit whose rooms are still marked this long after it)
struct EditTrack {
    bool active = false, warned = false;
    DWORD from = 0;
    long ownMs = -1;
    int ownMax = 0;
    long frames = 0, capFrames = 0;
};
EditTrack g_edit;

void EndEditTrack(const char* how) {
    if (!g_edit.active) return;
    g_edit.active = false;
    g_ownWaiting.store(false, std::memory_order_relaxed);
    if (g_edit.ownMax <= 0) return; // (no room of a lamp's own: an edit the GPU drew, or the safety net alone)
    const DWORD ms = GetTickCount() - g_edit.from;
    LOG_INFO(std::format("[DragLite] Lamp edit relight: the lamp's own rooms ({}) {}, the rooms around them {} {} ms after the edit; {} frames, {} under the spread cap",
                         g_edit.ownMax, g_edit.ownMs >= 0 ? std::format("solved after {} ms", g_edit.ownMs) : std::string("not all solved"), how, ms,
                         g_edit.frames, g_edit.capFrames));
}

void TrackEdit() {
    if (!FastOwn()) {
        g_edit.active = false;
        g_ownWaiting.store(false, std::memory_order_relaxed);
        return;
    }
    const DWORD now = GetTickCount();
    const bool pending = LevelLightShare::LampEditPending();
    if (!g_edit.active) {
        if (!pending) {
            g_ownWaiting.store(false, std::memory_order_relaxed);
            return;
        }
        g_edit = EditTrack{};
        g_edit.active = true;
        // the edit: the later of its latest mark and a drag's release, when recent; else now
        g_edit.from = now;
        DWORD latest = 0;
        if (const DWORD rel = g_settleFrom.load(std::memory_order_relaxed); rel && now - rel < 2000) latest = rel;
        if (const DWORD mark = static_cast<DWORD>(LampMarkFilter::EditLastTick()); mark && now - mark < 2000 && (!latest || static_cast<int32_t>(mark - latest) > 0))
            latest = mark;
        if (latest) g_edit.from = latest;
    }
    int total = 0;
    const int own = LevelLightShare::OwnRoomsPending(&total);
    g_ownWaiting.store(own > 0, std::memory_order_relaxed);
    g_edit.ownMax = std::max(g_edit.ownMax, total);
    const DWORD ms = now - g_edit.from;
    if (g_edit.ownMs < 0 && own == 0 && (total > 0 || ms >= 150)) g_edit.ownMs = static_cast<long>(ms);
    g_edit.frames++;
    if (SolveCapMs() > 0.0f) g_edit.capFrames++;
    if (own > 0 && ms >= kOwnWatchMs && !g_edit.warned) { // (once per edit: the reason, for the next log)
        g_edit.warned = true;
        LOG_INFO(std::format("[DragLite] Lamp edit: {} of the lamp's own {} rooms still not solved {} ms after the edit (drag hold {}, spread cap {}, "
                             "lamp being edited {}); they keep the edit budget",
                             own, total, ms, ClassicHolding() ? "on" : "off", SolveCapMs() > 0.0f ? "on" : "off",
                             LevelLightShare::LampDragging() ? "yes" : "no"));
    }
    if (!pending) EndEditTrack("settled");
    else if (ms >= kTrackMaxMs) EndEditTrack("still not settled");
}
} // namespace

void OnPresent() {
    // frame timers (the Tester tab's readout)
    {
        LARGE_INTEGER now{};
        QueryPerformanceCounter(&now);
        if (!g_freq.QuadPart) QueryPerformanceFrequency(&g_freq);
        if (g_prevFrame.QuadPart && g_freq.QuadPart) {
            const double ms = static_cast<double>(now.QuadPart - g_prevFrame.QuadPart) * 1000.0 / static_cast<double>(g_freq.QuadPart);
            g_frameMs.store(std::bit_cast<uint32_t>(static_cast<float>(ms)), std::memory_order_relaxed);
            if (Dragging() && ms < 2000.0) {
                g_runSum += ms, g_runMax = std::max(g_runMax, ms), g_runFrames++;
            }
        }
        g_prevFrame = now;
    }
    PollButton();
    if (Engaged()) {
        g_wasEngaged = true;
        if (ClassicHolding()) g_classicDrag = true;
        EndEditTrack("no longer followed (a new lamp edit)");
        return;
    }
    const struct TrackAtEnd {
        ~TrackAtEnd() { TrackEdit(); }
    } trackAtEnd; // (after this frame's release and sends, whichever way OnPresent returns)
    // Classic, relight only after release: the lamp's own story first (SolveCapMs), and the drag's log line once every room
    // of the edit is solved (or kSettleMs after the release)
    if (g_tracking.load(std::memory_order_relaxed)) {
        int ownPending = 0;
        const bool editPending = LevelLightShare::LampEditPending();
        if (editPending) LevelLightShare::EditRoomsPending(0, nullptr, &ownPending);
        g_ownFirst.store(editPending && ownPending > 0, std::memory_order_relaxed);
        const DWORD ms = GetTickCount() - g_trackFrom;
        if (!editPending || ms >= kSettleMs) {
            g_tracking.store(false, std::memory_order_relaxed);
            g_ownFirst.store(false, std::memory_order_relaxed);
            LOG_INFO(std::format("[DragLite] Classic drag: {} rooms deferred during the drag, {} solves at the stop, {} {} ms after the release",
                                 g_trackHeld, g_trackSolves.load(std::memory_order_relaxed),
                                 editPending ? "rooms of the edit still waiting (timeout)" : "settled", ms) + g_trackDrag);
        }
    }
    // still: every send left out goes once, with no urgency (the spread cap holds the frame's solves meanwhile)
    std::set<std::tuple<uintptr_t, int, int>> due;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        due.swap(g_rooms);
    }
    if (!g_wasEngaged && due.empty()) return;
    if (g_wasEngaged) {
        g_settleFrom.store(GetTickCount() | 1, std::memory_order_relaxed);
        {
            const DWORD now = GetTickCount() | 1, move = g_lastMove.load(std::memory_order_relaxed);
            g_releaseAt.store(now, std::memory_order_relaxed);
            g_releaseLetGo.store(LetGo(), std::memory_order_relaxed);
            g_releaseStepGap.store(move ? now - move : 0, std::memory_order_relaxed);
        }
        if (FastOwn()) g_ownWaiting.store(true, std::memory_order_relaxed); // (TrackEdit measures it at the end of this frame)
        if (g_runFrames > 0) {
            g_dragAvgMs.store(std::bit_cast<uint32_t>(static_cast<float>(g_runSum / g_runFrames)), std::memory_order_relaxed);
            g_dragMaxMs.store(std::bit_cast<uint32_t>(static_cast<float>(g_runMax)), std::memory_order_relaxed);
            g_dragFrames.store(g_runFrames, std::memory_order_relaxed);
        }
        g_runSum = g_runMax = 0.0, g_runFrames = 0;
        if (g_classicDrag) { // the release of a drag held in Classic: its rooms are solved from now on
            {
                std::lock_guard<std::mutex> lk(g_solveMx);
                g_trackHeld = g_heldRooms.size();
                g_heldRooms.clear();
            }
            g_trackFrom = GetTickCount();
            g_trackSolves.store(0, std::memory_order_relaxed);
            g_ownFirst.store(true, std::memory_order_relaxed); // (the next frame measures it)
            g_tracking.store(true, std::memory_order_relaxed);
        }
    }
    const bool classicRelease = g_classicDrag && g_wasEngaged;
    g_classicDrag = false;
    g_wasEngaged = false;
    int sent = 0;
    const WhyScope why(Why::Correction);
    // "Classic: show all floors together" holds the edit's stories until every one of its rooms is solved: the sends left
    // out are then the edit's too (tier 2, like its safety net), so they change in the same frame as the others
    // (2.13.1, fastOwnRelight: with Instant, a drag of a lamp the game solves takes the Classic rules too)
    const bool classicRules = ClassicLighting() || (FastOwn() && classicRelease);
    const int tier = classicRules && TesterToggles::classicTogether.load(std::memory_order_relaxed) ? 2 : -1;
    for (const auto& [tracker, story, id] : due) sent += LevelLightShare::QueueRoomId(tracker, story, id, tier) ? 1 : 0;
    const long sends = g_deferredSends.exchange(0), held = g_repicksHeld.exchange(0), dragSolves = g_dragSolves.exchange(0);
    g_totalResent += sent, g_totalHeld += held, g_drags++;
    g_last = std::format("Lamp drag ended: {} room solves during the drag ({} re-picks held), {} extra sends left out, {} rooms sent at the stop; "
                         "frames during the drag: {:.2f} ms on average, {:.1f} ms the worst ({} frames)",
                         dragSolves, held, sends, sent, LastDragAvgMs(), LastDragMaxMs(), LastDragFrames());
    if (classicRelease) g_trackDrag = std::format("; {} extra sends left out, {} sent at the stop; frames during the drag: {:.2f} ms on average, {:.1f} ms the worst ({} frames); "
                                                 "ended {} {} ms after its last step",
                                                 sends, sent, LastDragAvgMs(), LastDragMaxMs(), LastDragFrames(),
                                                 g_releaseLetGo.load(std::memory_order_relaxed) ? "when let go," : "by the still time,",
                                                 g_releaseStepGap.load(std::memory_order_relaxed));
    else LOG_INFO("[DragLite] " + g_last); // (a classic drag logs once, when its rooms are solved)
}

std::string StatusText() {
    return std::format("{}; last: {} | totals: {} drags, {} rooms sent at the stops, {} re-picks held", Engaged() ? "DRAGGING" : "idle", g_last, g_drags, g_totalResent,
                       g_totalHeld);
}

float FrameMs() { return std::bit_cast<float>(g_frameMs.load(std::memory_order_relaxed)); }
float LastDragAvgMs() { return std::bit_cast<float>(g_dragAvgMs.load(std::memory_order_relaxed)); }
float LastDragMaxMs() { return std::bit_cast<float>(g_dragMaxMs.load(std::memory_order_relaxed)); }
int LastDragFrames() { return g_dragFrames.load(std::memory_order_relaxed); }

} // namespace DragLite
