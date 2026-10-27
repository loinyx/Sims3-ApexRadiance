// Offline checks of the restart settle rule (features/restart_settle.h). Credits: @loinyx
#include "restart_settle.h"
#include <cstdio>

static int g_fail = 0;
static void Check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) g_fail++;
}

int main() {
    using namespace RestartSettle;
    {
        Table t;
        const uintptr_t a = 0x10000, b = 0x20000;
        Check(!t.Hold(a, 100), "a room never invalidated is not held");
        t.NoteInvalidate(a, 1000);
        Check(!t.Hold(a, 1010), "a single invalidate is not held (no burst)");
        t.NoteInvalidate(a, 1500); // again within 2 s: a burst
        Check(t.Hold(a, 1550), "a restart within 2 s of the previous one is held while it is under 300 ms old");
        Check(!t.Hold(a, 1800), "picked once the restarts are quiet for 300 ms");
        t.NoteInvalidate(a, 1900);
        Check(t.Hold(a, 2000), "the next restart of the burst is held again");
        Check(!t.Hold(b, 2000), "other rooms are not affected");
        t.NoteInvalidate(a, 5000); // 3.1 s after the last: a new single change
        Check(!t.Hold(a, 5010), "a change after a quiet 2 s is not held");
    }
    {
        Table t;
        const uintptr_t a = 0x30000;
        // restarts every 100 ms for 3 s: held for at most 1 s, then let through, then held again
        uint64_t firstFree = 0;
        int frees = 0;
        for (uint64_t now = 0; now <= 3000; now += 50) {
            if (now % 100 == 0) t.NoteInvalidate(a, 10000 + now);
            if (!t.Hold(a, 10000 + now)) {
                if (now >= 100 && !firstFree) firstFree = now;
                frees++;
            }
        }
        Check(firstFree > 0 && firstFree <= 1150, "a room that never stops changing is let through within about 1 s");
        Check(frees >= 3, "and again at least once a second after that");
        const Table::Stats s = t.Take();
        Check(s.held > 0 && s.capped >= 2, "held and let-through picks are counted");
        Check(t.Take().held == 0, "the counts start again after a take");
    }
    {
        Table t;
        // a reused room address after the entry went stale starts clean
        t.NoteInvalidate(0x40000, 100);
        t.NoteInvalidate(0x40000, 200);
        Check(t.Hold(0x40000, 250), "held in its burst");
        t.NoteInvalidate(0x40000, 10000);
        Check(!t.Hold(0x40000, 10010), "the same address 10 s later is a new room: not held");
        // 600 rooms restarted together all fit and are all held
        for (uintptr_t r = 1; r <= 600; r++) t.NoteInvalidate(0x1000000 + r * 4096, 20000);
        for (uintptr_t r = 1; r <= 600; r++) t.NoteInvalidate(0x1000000 + r * 4096, 20100);
        int held = 0;
        for (uintptr_t r = 1; r <= 600; r++) held += t.Hold(0x1000000 + r * 4096, 20150) ? 1 : 0;
        Check(held == 600, "600 rooms restarted together are all tracked");
        // far more rooms than the table holds: entries are taken over and a room without its entry is simply not held
        Table big;
        for (uintptr_t r = 1; r <= 5000; r++) big.NoteInvalidate(r * 4096, 30000);
        for (uintptr_t r = 1; r <= 5000; r++) big.NoteInvalidate(r * 4096, 30100);
        held = 0;
        for (uintptr_t r = 1; r <= 5000; r++) held += big.Hold(r * 4096, 30150) ? 1 : 0;
        Check(held <= 1024, "an overflowing burst holds at most the table's rooms, the others solve as before");
    }
    std::printf(g_fail ? "%d checks FAILED\n" : "Restart settle: all checks passed\n", g_fail);
    return g_fail ? 1 : 0;
}
