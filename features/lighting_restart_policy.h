#pragma once
#include <cstdint>

namespace LightingRestartPolicy {
// Completion needs observed idle work and a quiet interval. Merely queueing
// work or waiting a fixed delay does not establish that lighting is ready.
struct Run {
    bool active = false, done = false, interrupted = false;
    uint32_t started = 0, lastBusy = 0;
    bool Start(uint32_t now) {
        if (active) return false;
        active = true;
        done = interrupted = false;
        started = lastBusy = now;
        return true;
    }
    bool Observe(uint32_t now, bool busy) {
        if (!active) return false;
        if (busy) lastBusy = now;
        if (busy || now - started < 2000u || now - lastBusy < 750u) return false;
        active = false;
        done = true;
        return true;
    }
    void Cancel() {
        interrupted = active;
        active = done = false;
    }
};
}
