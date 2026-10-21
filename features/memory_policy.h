#pragma once
// Memory-pressure decisions for the public test experiment. Credits: @loinyx
#include <algorithm>
#include <cstdint>
namespace MemoryPolicy {
constexpr uint64_t MiB = 1024ull*1024;
struct Space {
    uint64_t free=0, lowFree=0, largest=0, lowLargest=0, privateBytes=0;
    bool complete=false, processValid=false;
    void AddFree(uint64_t start, uint64_t end) noexcept {
        if (end<=start) return;
        const auto n=end-start;
        const auto low=start<0x80000000ull ? std::min(end,0x80000000ull)-start : 0;
        free+=n; lowFree+=low; largest=std::max(largest,n); lowLargest=std::max(lowLargest,low);
    }
};
struct Pressure {
    unsigned consecutive=0;
    bool active=false;
    bool Observe(const Space& s, bool recentFailure) noexcept {
        if (!s.complete || !s.processValid) { consecutive=0; active=false; return false; }
        // A small lower-region block alone is not a failing allocation.
        const bool stressed=s.largest<320*MiB || s.free<512*MiB ||
            (s.privateBytes>=2048*MiB && s.lowFree<192*MiB && s.lowLargest<16*MiB) || recentFailure;
        const bool recovered=s.largest>=448*MiB && s.free>=768*MiB &&
            (s.privateBytes<2048*MiB || s.lowFree>=256*MiB || s.lowLargest>=32*MiB) && !recentFailure;
        if (active && !recovered) return true;
        if (recovered) { consecutive=0; active=false; return false; }
        if (stressed) { if (consecutive<3) ++consecutive; active=consecutive>=3; }
        else consecutive=0;
        return active;
    }
};
// Native Trim subtracts its first argument from cache+0x8c, then compares that target
// with cache+0x94 plus idle cache+0x98. Work is entry-granular; this is a requested step.
inline uint32_t TrimArgument(uint32_t budget, uint32_t other, uint32_t idle) noexcept {
    constexpr uint64_t keep=64*MiB, step=16*MiB;
    const uint64_t total=uint64_t(other)+idle;
    if (idle<=keep || total>budget) return 0; // native update already handles exceeding its own budget
    const auto remove=std::min(uint64_t(idle)-keep,step);
    return static_cast<uint32_t>(uint64_t(budget)-total+remove);
}
inline bool Expired(uint64_t now, uint64_t last, uint64_t idleMs) noexcept {
    return last && now>=last && now-last>=idleMs;
}
}
