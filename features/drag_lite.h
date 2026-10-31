#pragma once
// Lamp drags: coalesced room solves (ported from 2.12.4's drag fixes onto 2.12.3 + Real-time outdoor lamps).
// While a lamp (or the object holding one) is dragged in Build/Buy mode, Apex's extra room sends (the fan-out to rooms behind
// openings, room 0's dependents, the lifted-lamp retries, the openings' door checks, the audits) wait and are sent once each
// when the lamp is still, with no urgency; the room priority call holds a room solved moments ago; after the stop the frame's
// room solves stay under a 3.5 ms spread cap while the edit's rooms still wait. Sends that must stop a solve (a lamp removed
// from a list) and the game's own list rebuilds are never held.
// The drag is seen from the lamp's own change stream: position steps (LampMarkFilter, LevelLightShare::NoteLampEditing),
// lamp moves seen by the lamp list (LotLightBridge), and a type-11 light removed and added again on the same lot within
// 0.5 m (the same lamp moving).
// Each measure has a switch in the developer Tester tab (tester_toggles.h), all on by default.
//
// Classic: relight only after release (14/10, Tester switch classicAfterRelease, on; Lamp lighting Classic, or Instant on a
// device that cannot run it): while a lamp is dragged no room is solved at all. Every room the drag touches (any urgency
// tier: the lamp's own room and its story's room 0, the neighbours, the rooms lit through openings, the safety net's) and
// every room of the lot being played gets priority 0 in the priority call (HoldUntilRelease), so the scheduler picks none;
// the game's light-list rebuilds and invalidations go on untouched (only the solves wait; holding or replaying those lists
// broke lamps before). The drag is over once the lamp has been still for kReleaseMs (or let go). Then the rooms are solved
// once: the lamp's own story first with the edit budget (Faster room lighting's 12-16 ms), then the rest under the
// kSpreadCapMs spread cap. The terrain's local relight (NightTerrainRelight::EditReady) and the per-pixel lamp list
// (LotLightBridge) wait for the release too; the lamp list is still read at the drag pace for the drag's own detection.
// One log line per drag: rooms held, solves after the release and the time until every room of the edit was solved.
// Instant (GPU) keeps the measures above unchanged for the lamps the GPU draws.
//
// 2.13.1, fast own-room relight (Tester switch fastOwnRelight, on): the spread cap made the lamp's own rooms wait too (a
// drag's stop: 9 s over 810 frames at 5 ms a frame, log 14/10), and in Instant an indoor lamp, which the game still solves,
// took the outdoor drag rules (its rooms re-picked under the throttle during the drag). Now:
//   - the cap never applies while one of the lamps' own rooms (urgency tier 0, LevelLightShare::OwnRoomsPending) still waits:
//     those take Faster room lighting's normal edit budget, after any edit (switch, colour, intensity, a drag's stop); only
//     the rooms around them (the extra sends, other stories, through openings) are spread afterwards;
//   - a colour or intensity change is never taken for a drag (StepKind::Value): only moves hold the solves;
//   - with Lamp lighting Instant, a dragged lamp the game solves (not drawn by the GPU) follows the Classic rules above (no
//     solve during the drag, the own rooms first at the stop, all floors together); the GPU's lamps keep the Instant rules.
// One log line per edit: the own rooms' time until solved and the time until the rooms around them settled.
//
// 2.13.1, let go (Tester switch dragLetGo, on): the release rule waited kReleaseMs of stillness after the last step, and
// LevelLightShare::LampDragging 200 ms more for the edit hold (a recording, 14/10: the lamp's own room shown 0.8 s after the
// last step, of which 0.4 s the still time). Now the mouse button that held a lamp move being released ends the drag at
// once (LetGo): a move step seen while the button was down, then the button up, with the game's window in front. Steps
// within kPlaceGraceMs after it are the placement; a later step is a new drag. Lamps moved with the button up (picked up by
// a click, rotated with the keys) keep the still time.
#include <atomic>
#include <cstdint>
#include <string>

namespace DragLite {

// Why a room is being sent (QueueRoom reads it). Fanout .. Census are the extra sends left out while a lamp is dragged
enum class Why : int { None, Fanout, Retry, Dependents, Openings, Audit, DeferredBounce, Census, RelightLot, LampSwitch, QueueAgain, Walk, RelightAll, Correction };

constexpr unsigned long kStillMs = 750;    // the lamp still this long: the drag is over (the left-out sends go)
constexpr unsigned long kDragMs = 300;     // a step within this long: dragging
constexpr unsigned long kRepickMs = 400;   // while dragged: the same room is solved again at most this often
constexpr unsigned long kSolveGapMs = 120; // while dragged: at most one room solve begins this often
constexpr unsigned long kSettleMs = 15000; // the flush after a drag lasts at most this long
constexpr float kSpreadCapMs = 3.5f;       // the frame's room solves while the flush runs
constexpr unsigned long kReleaseMs = 400;  // Classic, relight only after release: the lamp still this long is let go
constexpr unsigned long kMaxHoldMs = 20000; // Classic: steps that go on longer than this without a pause no longer hold
                                            // the room solves (a lamp that keeps moving never starves the lot's rooms)
constexpr unsigned long kPlaceGraceMs = 300; // let go: steps this soon after the button's release are the placement (and
                                             // the lamp list's last read, every 250 ms while dragged)
constexpr unsigned long kLetGoMs = 1000;     // let go: counts this long at most (the still time has passed by then)

// A step of a lamp edit (any thread). source: for the log. kind: a move (a drag) or a value change (colour, intensity: never a
// drag while fastOwnRelight is on). gameSolved: the lamp's light is the game's room solve (false: a lamp the GPU draws)
enum class StepKind { Move, Value };
void NoteLampStep(const char* source, StepKind kind = StepKind::Move, bool gameSolved = true);
// Lamp lighting is Classic in effect (Classic chosen, or Instant unable to run); Night Lighting sets it every frame
void SetClassicLighting(bool on);
bool ClassicLighting();
// The Tester switch "Classic: relight only after release" is on and can apply: Lamp lighting Classic, or (fastOwnRelight)
// Instant, for the lamps the game solves (any thread)
bool ClassicReleaseOn();
// A lamp the game solves is being dragged now (a move within kReleaseMs; Classic: any lamp) and the switch above is on: no
// room solve begins (any thread)
bool ClassicHolding();
// The latest drag is of a lamp the game solves (a move within kStillMs): with Instant, its edit takes the Classic rules
// (AtriumHold's "show all floors together" too). Classic: true. Any thread
bool ClassicRulesForDrag();
// fastOwnRelight: the lamps' own rooms of the current edit still wait (render thread writes, any thread reads)
bool OwnRoomsWaiting();
// The priority call while ClassicHolding: true = this room waits for the release (an edit's room of any tier, or any room
// of the lot being played: onPlayedLot); the rooms held are counted for the drag's log line
bool HoldUntilRelease(const void* room, bool onPlayedLot);
// The latest step of this drag stream is within ms (any thread; LevelLightShare::LampDragging reads it)
bool StepWithin(unsigned long ms);
// dragLetGo: the button that held the latest lamp move was released and no move followed after kPlaceGraceMs: the drag is
// over now (ClassicHolding, Engaged and LevelLightShare::LampDragging end at once). Any thread
bool LetGo();
// The latest drag's end (GetTickCount | 1, 0 = none) and how it was seen (true: let go; false: the still time). Render thread
// writes it at the end of a drag (OnPresent); the edit hold's log reads it
unsigned long LastReleaseTick();
bool LastReleaseLetGo();
// A lamp step within kDragMs (any source, also LevelLightShare::LampDragging)
bool Dragging();
// A lamp move within kStillMs that the button has not let go of since (dragLetGo: the let go ends it at once, and the
// placement's steps after it do not bring it back; a move kPlaceGraceMs on is a new drag). Any thread
bool LampMoving();
// A lamp step within kStillMs (the drag and its settle time; kReleaseMs while ClassicReleaseOn)
bool Engaged();
// The flush after a drag (any thread): from the drag's end while a lamp edit's rooms still wait, at most kSettleMs; the
// frame's room solves stay under kSpreadCapMs meanwhile (LotLightingMotion, RoomLightQueue's drain), with no edit boosts
bool Settling();
// The frame's room solve cap now (ms; 0 = none): kSpreadCapMs while Settling and its Tester switch is on. None while the
// lamps' own rooms still wait (fastOwnRelight: OwnRoomsWaiting; without it, only after a Classic drag's release while the
// lamp's own story waits): the edit budget takes them first
float SolveCapMs();

// The re-pick throttle (RoomLightQueue's priority hook; LevelLightShare's solve start hook notes the starts): while a lamp
// is dragged a room that takes its light from elsewhere (urgency tier 1 or 2) waits for the stop, a room whose solve began
// less than kRepickMs ago, or any room within kSolveGapMs of the last solve start, is not picked now (priority 0: it keeps
// its state, its invalidations and its gathers, and is picked once the gap or the drag ends)
void NoteSolveStarted(const void* room);
bool HoldRepick(const void* room);

// Why a room is being sent (QueueRoom reads it)
extern thread_local Why t_why;
struct WhyScope {
    explicit WhyScope(Why w) : prev(t_why) { t_why = w; }
    ~WhyScope() { t_why = prev; }
    WhyScope(const WhyScope&) = delete;
    WhyScope& operator=(const WhyScope&) = delete;
    Why prev;
};
// QueueRoom (any thread): true = left out now and sent once the lamp is still (an extra send during a drag)
bool DeferSend(uintptr_t tracker, int story, int id);

// Render thread, every frame (LevelLightShare::OnPresent): the left-out sends once the lamp is still (urgent, tier 2, while
// "Classic: show all floors together" holds the edit, so they change with the other stories), the frame timers
void OnPresent();
std::string StatusText();
// Tester tab readout (render thread values, read anywhere): the last frame's time, and the average and worst frame of the
// last finished drag (0 when none yet)
float FrameMs();
float LastDragAvgMs();
float LastDragMaxMs();
int LastDragFrames();

} // namespace DragLite
