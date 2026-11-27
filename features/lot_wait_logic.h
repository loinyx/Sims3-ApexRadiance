#pragma once
// Open lot waits for the full lot budget (features/lot_wait.cpp): plain code, checked offline by tools/lot_wait_test.
#include <cstdint>

namespace LotWaitLogic {

constexpr int kSlots = 8;
// A wait open this long no longer keeps the budget and its slot is freed: a waiter that never gets its done (its task
// ended another way) cannot keep the camera-motion cuts off for the rest of the session
constexpr uint64_t kCeilingMs = 120000;

// One World.WaitForDetailedLots call between its start and its done, keyed by lot id and flags (the waiter keeps both);
// two waits with the same key close oldest first
struct Wait {
    bool open = false;
    uint64_t lot = 0;
    uint32_t flags = 0, seq = 0;
    uint64_t startMs = 0;
};

struct Table {
    Wait w[kSlots];
    uint32_t nextSeq = 0;
    bool Open(uint64_t lot, uint32_t flags, uint64_t ms) { // false when every slot is taken (that wait is not tracked)
        for (Wait& x : w)
            if (!x.open) { x = Wait{true, lot, flags, nextSeq++, ms}; return true; }
        return false;
    }
    bool Close(uint64_t lot, uint32_t flags) { // false when no open wait has this key
        Wait* best = nullptr;
        for (Wait& x : w)
            if (x.open && x.lot == lot && x.flags == flags && (!best || x.seq < best->seq)) best = &x;
        if (best) best->open = false;
        return best != nullptr;
    }
    void Expire(uint64_t nowMs) {
        for (Wait& x : w)
            if (x.open && nowMs - x.startMs > kCeilingMs) x.open = false;
    }
    int Count() const {
        int n = 0;
        for (const Wait& x : w) n += x.open;
        return n;
    }
    uint64_t OldestStart() const { // ~0 when none is open
        uint64_t t = ~0ull;
        for (const Wait& x : w)
            if (x.open && x.startMs < t) t = x.startMs;
        return t;
    }
};

} // namespace LotWaitLogic
