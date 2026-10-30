// Executes the real DecideEdit dispatch guards without a game process.
#include "features/terrain_lighting_policy.h"
#include <chrono>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <format>
#include <string>
#include <vector>
using Clock = std::chrono::steady_clock;
enum class EditWait { Snapshot, Relight };
bool g_autoDusk = true, g_editUser = false, g_editForce = false;
bool g_loadKickPending = false, g_scheduled = false, g_bakedDue = false;
std::string g_editReason = "edit", g_pendingReason;
std::vector<uint64_t> g_editUserLots;
TerrainLightingPolicy::Cycle g_cycle;
bool busy = false;
int finished = 0, waited = 0, dispatched = 0;
int assertions = 0, failures = 0;
namespace ChunkRelight { bool Busy() { return busy; } bool RebuildActive() { return false; } } // (no paced full rebuild running)
void FinishEdit(const std::string&) { finished++; }
void WaitEdit(EditWait, const std::string&) { waited++; }
/* PRODUCTION_DISPATCH */
/* BASELINE_DISPATCH */
void Assert(bool ok, const char* name) {
    assertions++; if (!ok) { failures++; std::printf("FAIL: %s\n", name); }
}
int main() {
    for (bool user : {false, true}) for (bool force : {false, true})
    for (bool pending : {false, true}) for (bool queued : {false, true})
    for (bool snapshot : {false, true}) for (int countdown : {0, 2}) {
        g_editUser = user; g_editForce = force;
        g_pendingReason = pending ? "older rebuild" : "";
        busy = queued; g_bakedDue = snapshot;
        finished = waited = dispatched = 0;
        DecideEdit(0, 1.f, true, countdown, Clock::time_point{});
        const bool merged = !user && !force && pending && countdown > 0;
        const bool blocked = !merged && (snapshot || (queued && !user));
        Assert(finished == int(merged), "only automatic edits may be consumed by an armed countdown");
        Assert(waited == int(blocked), "snapshot and automatic serialization guards remain");
        Assert(dispatched == int(!merged && !blocked), "user footprints reach the dispatcher ahead of unrelated local work");
    }
    // Reproduce the two guard-level failures in the captured revision.
#ifdef HAVE_BASELINE
    g_editUser = true; g_editForce = false; g_bakedDue = false;
    g_pendingReason = "older rebuild"; busy = false;
    finished = waited = dispatched = 0;
    OldDecideEdit(0, 1.f, true, 2, Clock::time_point{});
    Assert(finished == 1 && !dispatched, "unchanged baseline consumes a user edit at an armed countdown");
    g_pendingReason.clear(); busy = true;
    finished = waited = dispatched = 0;
    OldDecideEdit(0, 1.f, true, 0, Clock::time_point{});
    Assert(waited == 1 && !dispatched, "unchanged baseline serializes a user edit behind a local batch");
#endif
    // Load/phase work still covers the edit, rather than starting a duplicate bake.
    for (bool loading : {false, true}) {
        g_loadKickPending = loading; g_scheduled = !loading; g_editUser = true;
        finished = waited = dispatched = 0;
        DecideEdit(0, 1.f, true, 2, Clock::time_point{});
        Assert(finished == 1 && !waited && !dispatched, "load and scheduled phase reconstruction remain coalesced");
    }
    std::printf("Edit dispatch fixtures: %d assertions, %d failures; real production guards, no native bake execution.\n", assertions, failures);
    return failures ? 1 : 0;
}
