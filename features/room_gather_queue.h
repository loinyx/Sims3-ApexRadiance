#pragma once
#include <cstdint>
#include <utility>

namespace RoomGatherQueue {
// A pending native gather needs its queue entry. An outdoor room in state 1
// with an empty queue cannot advance its countdown. The originating invalidation
// is not yet identified; restoring the key retains the native gather and solve.
template<class Insert>
bool RestoreOutdoor(int state, uint32_t countdown, uint32_t changed,
                    uint32_t pending, bool active, bool outdoor, Insert&& insert) {
    if (!active || !outdoor || state != 1 || countdown == 0 || changed || pending) return false;
    std::forward<Insert>(insert)();
    return true;
}
}
