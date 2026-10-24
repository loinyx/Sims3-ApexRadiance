#pragma once
#include <array>
#include <cstdint>

namespace TransitionCalls {
enum class Kind : unsigned { Thumbnails, Routing, HeightMap };
struct Call {
    uint64_t id = 0, started = 0, ended = 0;
    uintptr_t argument = 0;
    uint32_t thread = 0;
    Kind kind = Kind::Thumbnails;
    bool active = false;
};
struct Calls {
    std::array<Call, 16> slots{};
    uint64_t serial = 0;
    uint64_t Begin(Kind kind, uint64_t now, uintptr_t argument, uint32_t thread) {
        for (auto& slot : slots) if (!slot.active) {
            slot = {++serial, now, 0, argument, thread, kind, true};
            return slot.id;
        }
        return 0;
    }
    bool End(uint64_t id, uint64_t now, Call* out) {
        for (auto& slot : slots) if (slot.active && slot.id == id && now >= slot.started) {
            slot.ended = now;
            slot.active = false;
            *out = slot;
            return true;
        }
        return false;
    }
};
}
