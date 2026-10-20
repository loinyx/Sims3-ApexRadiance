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
}
