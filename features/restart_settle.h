#pragma once
// Restart settle (development builds, switch restart_settle): a room restarted again and again while the lots around the
// camera settle is picked for its solve once its restarts stop, not after each one.
//
// Measured (docs/history/performance-room-light-queue.md, 2026-10-27): with the camera still for 10 s no room is solved;
// within 10 s of a camera move the same room was restarted (invalidated) up to 15 times a minute, each restart throwing
// away a class-0 solve before it reached class 2. The game's invalidate still runs at once (a solve in progress stops, as
// before: its lamp list may point to a lamp being removed); only the pick of the restarted room for a new solve waits.
//
// Rule, per room: an invalidate within kBurstMs of the room's previous one starts or continues a burst. While a burst runs
// and the last invalidate is under kQuietMs old, the room's priority is 0 (not picked); never longer than kMaxHoldMs from
// the burst's first held pick, so a room whose lights never stop changing is still solved at least once a second. A single
// invalidate (no burst) is never held. The caller leaves out a lamp edit's rooms (LampUrgency > 1). The last solve comes
// after the last restart, so the maps the room ends with are the ones it ends with today; only the in-between class-0
// solves are skipped. Thread-safe; fixed table, no allocation.
//
// Part of Apex Radiance. Credits: @loinyx
#include <cstdint>
#include <mutex>

namespace RestartSettle {

constexpr uint64_t kBurstMs = 2000, kQuietMs = 300, kMaxHoldMs = 1000;

class Table {
public:
    // Every invalidate of a room (any thread)
    void NoteInvalidate(uintptr_t room, uint64_t now) {
        std::lock_guard<std::mutex> lk(mx_);
        Entry& e = Find(room, now);
        e.burst = e.last && now - e.last < kBurstMs;
        if (!e.burst) e.holdFrom = 0;
        e.last = now;
    }
    // At the room's pick (the scheduler's priority): true = not picked now
    bool Hold(uintptr_t room, uint64_t now) {
        std::lock_guard<std::mutex> lk(mx_);
        Entry* e = Lookup(room, now);
        if (!e || !e->burst || now - e->last >= kQuietMs) return false;
        if (!e->holdFrom) e->holdFrom = now;
        if (now - e->holdFrom >= kMaxHoldMs) {
            e->holdFrom = now; // let this pick through; a burst that goes on is held again from here
            capped_++;
            return false;
        }
        held_++;
        return true;
    }
    struct Stats {
        uint64_t held = 0, capped = 0;
    };
    Stats Take() {
        std::lock_guard<std::mutex> lk(mx_);
        Stats s{held_, capped_};
        held_ = capped_ = 0;
        return s;
    }

private:
    struct Entry {
        uintptr_t room = 0;
        uint64_t last = 0, holdFrom = 0;
        bool burst = false;
    };
    static constexpr int kSlots = 1024;
    static int Hash(uintptr_t room) { return static_cast<int>((static_cast<uint32_t>(room) * 2654435761u) >> 22); } // 10 bits
    // An entry idle for kBurstMs is free again (a room address reused later starts clean)
    bool Stale(const Entry& e, uint64_t now) const { return !e.room || now - e.last >= kBurstMs + kMaxHoldMs; }
    Entry* Lookup(uintptr_t room, uint64_t now) {
        for (int k = 0, i = Hash(room); k < 16; k++, i = (i + 1) & (kSlots - 1))
            if (slots_[i].room == room) return Stale(slots_[i], now) ? nullptr : &slots_[i];
        return nullptr;
    }
    Entry& Find(uintptr_t room, uint64_t now) {
        Entry* free = nullptr;
        for (int k = 0, i = Hash(room); k < 16; k++, i = (i + 1) & (kSlots - 1)) {
            Entry& e = slots_[i];
            if (e.room == room) {
                if (Stale(e, now)) e = Entry{room};
                return e;
            }
            if (!free && Stale(e, now)) free = &e;
        }
        Entry& e = free ? *free : slots_[Hash(room)]; // all 16 probed slots busy: the home slot is taken over
        e = Entry{room};
        return e;
    }
    std::mutex mx_;
    Entry slots_[kSlots];
    uint64_t held_ = 0, capped_ = 0;
};

// The process's table (the invalidate hooks of LevelLightShare and the priority hook of RoomLightQueue)
inline Table& Global() {
    static Table t;
    return t;
}

} // namespace RestartSettle
