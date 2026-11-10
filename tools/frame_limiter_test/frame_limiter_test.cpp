// The frame limiter's schedule (features/frame_limiter_math.h): a steady cap, short and long late frames, cap changes,
// and the measured spread. Simulated clock in ticks (1 tick = 1 us).
#include "frame_limiter_math.h"
#include <cstdio>
#include <vector>

static int g_fail = 0;
static void Check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) g_fail++;
}

// Runs frames of the given work times through the schedule; returns the start time of every frame.
static std::vector<int64_t> Run(const std::vector<int64_t>& work, int64_t interval, FrameLimiterMath::Schedule& s, int64_t& now) {
    std::vector<int64_t> starts;
    for (int64_t w : work) {
        starts.push_back(now);
        now += w; // the frame's own work, up to Present
        const int64_t until = s.Plan(now, interval);
        if (until) now = std::max(now, until); // a perfect wait
    }
    return starts;
}

int main() {
    using namespace FrameLimiterMath;
    {
        Schedule s;
        int64_t now = 1000;
        const auto starts = Run(std::vector<int64_t>(100, 2000), 6061, s, now); // 165 fps, 2 ms of work
        bool steady = true;
        for (size_t i = 2; i < starts.size(); i++) steady &= starts[i] - starts[i - 1] == 6061;
        Check(steady, "a frame shorter than the cap starts exactly one interval after the previous one");
    }
    {
        Schedule s;
        int64_t now = 0;
        std::vector<int64_t> work(60, 1000);
        work[30] = 7000; // late by 1 ms against a 6.061 ms cap
        const auto starts = Run(work, 6061, s, now);
        Check(starts[32] - starts[31] < 6061 && starts[32] - starts[30] == 2 * 6061,
              "a slightly late frame keeps the schedule: the next one is shorter and the average holds");
    }
    {
        Schedule s;
        int64_t now = 0;
        std::vector<int64_t> work(60, 1000);
        work[30] = 100000; // a 100 ms hitch
        const auto starts = Run(work, 6061, s, now);
        bool noBurst = true;
        for (size_t i = 32; i < starts.size(); i++) noBurst &= starts[i] - starts[i - 1] == 6061;
        Check(noBurst, "after a long hitch the schedule restarts: no burst of short frames");
    }
    {
        Schedule s;
        int64_t now = 0;
        Run(std::vector<int64_t>(10, 1000), 33333, s, now); // 30 fps
        const auto starts = Run(std::vector<int64_t>(10, 1000), 4166, s, now); // then 240 fps
        Check(starts[3] - starts[2] == 4166, "raising the cap takes effect at once (no wait left over from the old one)");
        const auto later = Run(std::vector<int64_t>(10, 1000), 16667, s, now); // then 60 fps
        Check(later[3] - later[2] == 16667, "lowering the cap takes effect at once");
    }
    {
        Schedule s;
        Check(s.Plan(5000, 0) == 0 && s.next == 0, "off (interval 0) never waits");
        Check(s.Plan(5000, 1000) == 0, "the first frame after turning it on does not wait");
    }
    {
        Stats st;
        for (int i = 0; i < 500; i++) st.Add(i % 2 ? 6.0 : 6.2);
        Check(st.count == Stats::kCount && std::abs(st.MeanMs() - 6.1) < 1e-9, "the average covers the last 240 frames");
        Check(std::abs(st.SpreadMs() - 0.1002) < 1e-3, "the spread is the standard deviation of the intervals");
        st.Add(-1);
        st.Add(5000);
        Check(st.count == Stats::kCount, "invalid and absurd intervals are ignored");
    }
    {
        SpinMargin m;
        const int64_t f = 10000000;
        Check(m.Ticks(f, true) == f / 4000 && m.Ticks(f, false) == f / 1000, "the margin starts at 0.25 ms (1 ms for the plain timer)");
        m.Note(f / 1000 * 3, f); // the timer woke 3 ms late
        Check(m.Ticks(f, true) == f / 500, "a very late timer is capped at a 2 ms spin");
        for (int i = 0; i < 1000; i++) m.Note(0, f);
        Check(m.Ticks(f, true) == f / 4000, "the margin shrinks back when the timer is punctual again");
    }
    std::printf(g_fail ? "%d FAILED\n" : "all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
