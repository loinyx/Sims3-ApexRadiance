#pragma once
// Bound concurrent temporary verification storage. Credits: @loinyx
#include <atomic>
#include <cstdint>
namespace ScratchBudget {
inline std::atomic<uint64_t> used{0};
struct Lease {
    uint64_t bytes=0;
    explicit Lease(uint64_t want, bool enabled) noexcept {
        if (!enabled) return;
        constexpr uint64_t limit=64ull<<20;
        auto previous=used.load(std::memory_order_relaxed);
        while (want<=limit && previous<=limit-want) {
            if (used.compare_exchange_weak(previous,previous+want,std::memory_order_relaxed)) { bytes=want; return; }
        }
    }
    Lease(const Lease&)=delete;
    Lease& operator=(const Lease&)=delete;
    ~Lease() { if (bytes) used.fetch_sub(bytes,std::memory_order_relaxed); }
};
}
