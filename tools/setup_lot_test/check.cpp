// Lot capture test offline checks: the decision in features/setup_lot_policy.h. Read-only: writes nothing.
// Part of Apex Radiance. Credits: @loinyx
#include "setup_lot_policy.h"
#include <cstdio>

namespace P = SetupLotPolicy;
static int g_fail = 0;
#define CHECK(c, msg)                                                                                                  \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, msg);                                                  \
            g_fail++;                                                                                                  \
        }                                                                                                              \
    } while (0)

int main() {
    const uint64_t played = 0x0000000100000042ull, other = 0x0000000100000077ull;
    CHECK(!P::Gate(false, true, 1, played, played, false), "the played lot (the world's active id) is never gated");
    CHECK(!P::Gate(false, true, 1, other, 0, true), "a priority lot is never gated, even with no world active id");
    CHECK(!P::Gate(false, true, 1, other, played, true), "a priority lot is never gated with a stale world active id");
    CHECK(!P::Gate(true, true, 1, other, played, false), "nothing is gated with high quality on every lot");
    CHECK(!P::Gate(false, false, 1, other, played, false), "nothing is gated when the getter is not the expected code");
    CHECK(!P::Gate(false, true, 0, other, played, false), "a lot at the low quality is not gated");
    CHECK(!P::Gate(false, true, 1, 0, played, false), "a lot without an id is not gated");
    CHECK(P::Gate(false, true, 1, other, 0, false), "a captured lot is gated with no world active lot (family picker)");
    CHECK(P::Gate(false, true, 1, other, played, false), "a captured lot beside the played lot is gated");

    // one re-solve on gated -> not gated, none otherwise, and never across lots
    int due = 0;
    bool prev = true;
    const bool seq[] = {true, true, false, false, false};
    for (bool now : seq) {
        due += P::ResolveDue(other, prev, other, now);
        prev = now;
    }
    CHECK(due == 1, "a gated -> not gated transition gives exactly one re-solve");
    CHECK(!P::ResolveDue(other, true, played, false), "a tracker reused for another lot does not inherit the re-solve");
    CHECK(!P::ResolveDue(other, false, other, false), "a lot never gated needs no re-solve");
    CHECK(!P::ResolveDue(other, false, other, true), "becoming gated needs no re-solve");

    std::printf(g_fail ? "%d lot capture check(s) failed\n" : "lot capture checks passed\n", g_fail);
    return g_fail ? 1 : 0;
}
