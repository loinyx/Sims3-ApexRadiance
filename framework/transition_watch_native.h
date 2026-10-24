#pragma once
#include <Windows.h>
#include <array>
#include <cstdint>

namespace TransitionWatch {
struct Lot {
    uint64_t id = 0;
    uint32_t stage = 0, step = 0;
    unsigned char done = 0, failed = 0, jobDone = 0;
};
struct Native {
    unsigned requested = 0, pending = 0, stored = 0;
    bool readable = false, truncated = false;
    std::array<Lot, 16> lots{};
};
// Steam layout: renderer map buckets +1C/count +20, node value +8/next +10.
// Traversal stays on the render thread and is bounded even if a link is corrupt.
inline void ReadLots(uintptr_t renderer, Native* sample) noexcept {
    __try {
        if (!renderer) return;
        const auto buckets = *reinterpret_cast<const uintptr_t*>(renderer + 0x1C);
        const unsigned bucketCount = *reinterpret_cast<const unsigned*>(renderer + 0x20);
        if (!buckets || !bucketCount || bucketCount > 8192) return;
        const auto sentinel = *reinterpret_cast<const uintptr_t*>(buckets + bucketCount * 4);
        unsigned visited = 0;
        for (unsigned b = 0; b < bucketCount; ++b) {
            auto node = *reinterpret_cast<const uintptr_t*>(buckets + b * 4);
            while (node && node != sentinel) {
                if (++visited > 1024) { sample->truncated = true; return; }
                const auto object = *reinterpret_cast<const uintptr_t*>(node + 8);
                if (object) {
                    const auto lot = *reinterpret_cast<const uintptr_t*>(object + 0x18);
                    if (lot && *reinterpret_cast<const unsigned char*>(lot + 0xC1)) {
                        ++sample->requested;
                        Lot entry;
                        entry.id = *reinterpret_cast<const uint64_t*>(object + 0x10);
                        entry.stage = *reinterpret_cast<const unsigned*>(object + 0x24);
                        entry.step = *reinterpret_cast<const unsigned*>(object + 0x28);
                        entry.done = *reinterpret_cast<const unsigned char*>(object + 0x1E);
                        entry.failed = *reinterpret_cast<const unsigned char*>(object + 0x1F);
                        entry.jobDone = *reinterpret_cast<const unsigned char*>(object + 0x30);
                        if (!entry.done) ++sample->pending;
                        if (sample->stored < sample->lots.size()) sample->lots[sample->stored++] = entry;
                        else sample->truncated = true;
                    }
                }
                node = *reinterpret_cast<const uintptr_t*>(node + 0x10);
            }
        }
        sample->readable = true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { sample->readable = false; }
}
}
