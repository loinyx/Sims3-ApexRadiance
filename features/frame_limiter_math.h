#pragma once
// The frame limiter's schedule and its measurement, without Windows calls (offline test: tools/frame_limiter_test).
// Times are in performance-counter ticks. Credits: @loinyx
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace FrameLimiterMath {

// Start-of-frame pacing: each frame may start no earlier than one interval after the previous frame's planned start.
// A frame that is late by less than one interval keeps the schedule (the next one can be shorter, so the average holds);
// later than that, the schedule restarts from now (no burst of short frames after a hitch or a loading screen).
struct Schedule {
    int64_t next = 0; // planned start of the next frame; 0 = not started

    // Called when a frame ends (right after Present). Returns the tick to wait for, or 0 for no wait.
    int64_t Plan(int64_t now, int64_t interval) {
        if (interval <= 0) {
            next = 0;
            return 0;
        }
        if (next == 0 || now - next > interval || next - now > interval) next = now; // first frame, a long stall or a higher cap
        const int64_t target = next;
        next += interval;
        return target > now ? target : 0;
    }
    void Restart() { next = 0; }
};

// Frame intervals of the last kCount frames: average rate and spread (standard deviation), for the menu.
struct Stats {
    static constexpr int kCount = 240;
    double ms[kCount] = {};
    int count = 0, at = 0;
    void Add(double frameMs) {
        if (!std::isfinite(frameMs) || frameMs <= 0 || frameMs > 1000) return;
        ms[at] = frameMs;
        at = (at + 1) % kCount;
        count = std::min(count + 1, kCount);
    }
    void Clear() { count = at = 0; }
    double MeanMs() const {
        double s = 0;
        for (int i = 0; i < count; i++) s += ms[i];
        return count ? s / count : 0;
    }
    double SpreadMs() const {
        if (count < 2) return 0;
        const double m = MeanMs();
        double s = 0;
        for (int i = 0; i < count; i++) s += (ms[i] - m) * (ms[i] - m);
        return std::sqrt(s / (count - 1));
    }
};

// How much earlier than the deadline the timer wait ends (the rest is a spin): a little more than the timer's recent
// worst lateness, within [floor, 2 ms]. lateness and the result are in ticks.
struct SpinMargin {
    int64_t worst = 0;
    void Note(int64_t lateness, int64_t freq) {
        worst = std::max<int64_t>(worst - freq / 20000, std::max<int64_t>(lateness, 0)); // forgets 50 us per wait
    }
    int64_t Ticks(int64_t freq, bool highResolutionTimer) const {
        const int64_t floor = highResolutionTimer ? freq / 4000 : freq / 1000; // 0.25 ms, or 1 ms for the plain timer
        return std::clamp<int64_t>(worst + worst / 2 + freq / 10000, floor, freq / 500);
    }
};

} // namespace FrameLimiterMath
