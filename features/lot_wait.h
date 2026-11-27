#pragma once
// Full lot budget during lot waits (PerfToggles::LotWaitFullBudget; docs/features/performance.md).
//
// World.WaitForDetailedLots is the script wait that holds live mode until a lot is loaded in detail (with its Sims and
// objects); the waiter is woken by its done once the lot is ready. Two thin Detours hooks, found by signature in Steam
// 1.67.2 and EA app 1.69.47 (one match each; nothing hooked otherwise), count the waits open between start and done.
// While one is open (at most LotWaitLogic::kCeilingMs each), Lot Lighting While Moving and Wall Shading While Moving
// keep the game's own budget instead of their camera-motion cuts, so the waited lot finishes at the game's pace. The
// hooks only count and call the original unchanged; with the switch off or nothing open the cuts run as before.
namespace LotWait {

void Install(); // init thread, after GameAddr::Resolve

// Any thread, where a camera-motion cut is about to happen: true = keep the game's budget
bool KeepFullBudget();

} // namespace LotWait
