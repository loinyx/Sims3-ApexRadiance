#pragma once
#include <array>
#include <cstdint>

namespace ThumbnailProgress {
// Each thumbnail retains its native readiness test. Recovery only supplies missing
// update turns, after a sustained wait on the same renderer and lot.
struct Wait {
    uintptr_t renderer = 0;
    uint64_t lot = 0, since = 0, seen = 0, pumped = 0;
    unsigned turns = 0;
    bool reported = false;
};
struct Waits {
    std::array<Wait, 16> slots{};
    Wait* Observe(uintptr_t renderer, uint64_t lot, uint64_t now, bool ready) {
        if (!renderer || !lot) return nullptr;
        Wait* free = nullptr;
        for (auto& w : slots) {
            if (w.renderer == renderer && w.lot == lot) {
                if (ready) { w = {}; return nullptr; }
                if (now - w.seen > 1000) w = {renderer, lot, now, now};
                w.seen = now;
                return &w;
            }
            if (!w.renderer || now - w.seen > 1000) free = &w;
        }
        if (ready || !free) return nullptr;
        *free = {renderer, lot, now, now};
        return free;
    }
};
inline bool Due(const Wait& w, uint64_t now, bool sameThread, bool sceneBusy,
                bool managerEnabled, bool managerSuspended, uint64_t nativeTurn) {
    return sameThread && !sceneBusy && managerEnabled && !managerSuspended &&
           now - w.since >= 1500 && now - w.since < 30000 &&
           (!w.pumped || now - w.pumped >= 33) &&
           (!nativeTurn || now - nativeTurn >= 100);
}
}
