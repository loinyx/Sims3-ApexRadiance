#include "../../features/room_gather_queue.h"
#include <cassert>
#include <set>
#include <iostream>

struct Model {
    std::set<int> pending;
    int state = 2;
    unsigned countdown = 0, gathers = 0, solves = 0;
    int ready = 0;
    void InvalidateDuringGather() {
        state = 1;
        countdown = 5;
        pending.insert(0);
    }
    void NativeUpdate(bool invalidate = false) {
        if (!pending.contains(0)) return;
        if (state == 1 && countdown && --countdown) return;
        state = 2;
        ++gathers;
        if (invalidate) InvalidateDuringGather();
        // Native gather completion erases the key after the gather callback.
        pending.erase(0);
    }
    bool Repair(bool active = true) {
        return RoomGatherQueue::RestoreOutdoor(state, countdown, 0,
            static_cast<unsigned>(pending.size()), active, true, [&] { pending.insert(0); });
    }
    void NativeSolve() {
        if (state != 2 || !gathers) return;
        ++solves;
        state = 5;
        ready = 2;
    }
};
int main() {
    Model m;
    m.pending.insert(0);
    m.NativeUpdate(true);
    assert(m.state == 1 && m.countdown == 5 && m.pending.empty() && m.ready == 0);
    for (int n = 0; n < 100; ++n) m.NativeUpdate();
    assert(m.countdown == 5 && m.ready == 0);
    assert(!m.Repair(false));
    assert(m.Repair() && m.countdown == 5 && m.ready == 0);
    for (int n = 0; n < 4; ++n) {
        assert(!m.Repair());
        m.NativeUpdate();
        assert(m.state == 1 && m.ready == 0);
    }
    m.NativeUpdate();
    assert(m.state == 2 && m.pending.empty() && m.ready == 0);
    m.NativeSolve();
    assert(m.ready == 2 && m.gathers == 2 && m.solves == 1 && !m.Repair());
    unsigned inserts = 0;
    auto insert = [&] { ++inserts; };
    for (int state : {0, 2, 3, 4, 5})
        assert(!RoomGatherQueue::RestoreOutdoor(state, 5, 0, 0, true, true, insert));
    assert(!RoomGatherQueue::RestoreOutdoor(1, 0, 0, 0, true, true, insert));
    assert(!RoomGatherQueue::RestoreOutdoor(1, 5, 1, 0, true, true, insert));
    assert(!RoomGatherQueue::RestoreOutdoor(1, 5, 0, 1, true, true, insert));
    assert(!RoomGatherQueue::RestoreOutdoor(1, 5, 0, 0, true, false, insert));
    assert(!RoomGatherQueue::RestoreOutdoor(1, 5, 0, 0, false, true, insert));
    assert(inserts == 0);
    std::cout << "Room gather queue: lost-key reproduction, countdown, native completion and idle guards passed\n";
}
