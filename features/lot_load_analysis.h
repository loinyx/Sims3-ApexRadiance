#pragma once
#include <cstdint>
namespace LotLoadAnalysis {
struct State {
    uint64_t lot = 0;
    uint32_t stage = 0, step = 0;
    bool done = false, failed = false, valid = false;
};
// Layout reads are enabled only for the statically checked Steam build.
State ReadRenderer(void* renderer) noexcept;
uint64_t Clock() noexcept; // QPC ticks; independent of the profiler's selectable clock
void Reset() noexcept; // new capture epoch when the profiler is toggled
void Request(void* lot, bool detailed) noexcept;
void Sample(const State& before, const State& after, uint64_t started, uint64_t ended) noexcept;
void Pump(); // existing init thread; all file I/O stays outside hooks
}
