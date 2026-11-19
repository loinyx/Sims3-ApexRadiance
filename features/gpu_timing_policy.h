#pragma once
// Sample effect timings without issuing queries every public frame. Credits: @loinyx
#include <cstdint>
namespace GpuTiming {
inline bool Due(uint64_t now, uint64_t& next, bool publicBuild) noexcept {
    if (!publicBuild) return true;
    if (now < next) return false;
    next = now + 250;
    return true;
}
// The periodic log line of one effect's GPU time: mean and max of the samples read since the last line. Due() is true once
// per period (the first call arms it); the caller logs when n > 0 and calls Reset() either way.
struct LogWindow {
    double sum = 0.0;
    float max = 0.0f;
    unsigned n = 0;
    uint64_t next = 0;
    void Add(float ms) noexcept { sum += ms, max = ms > max ? ms : max, ++n; }
    float Mean() const noexcept { return n ? static_cast<float>(sum / n) : 0.0f; }
    bool Due(uint64_t now, uint64_t period) noexcept {
        if (!next) next = now + period;
        if (now < next) return false;
        next = now + period;
        return true;
    }
    void Reset() noexcept { sum = 0.0, max = 0.0f, n = 0; }
};
}
