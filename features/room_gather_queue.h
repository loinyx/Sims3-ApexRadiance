#pragma once
#include <cstdint>
#include <utility>

namespace RoomGatherQueue {
enum class Membership { Unknown, Absent, Present };

// Native integer sets hash a key by unsigned remainder. Room zero therefore
// belongs to bucket zero. Read only that chain, with bounded, fallible reads.
template<class ReadNode>
Membership LookupOutdoor(uint32_t count, uint32_t buckets, uintptr_t head, ReadNode&& read) {
    if (!buckets || buckets > 1048576 || count > 1048576) return Membership::Unknown;
    if (!count) return head ? Membership::Unknown : Membership::Absent;
    uintptr_t node = head;
    for (uint32_t n = 0; n < count && n < 4096; ++n) {
        if (!node) return Membership::Absent;
        int key = -1;
        uintptr_t next = 0;
        if (!read(node, key, next)) return Membership::Unknown;
        if (static_cast<uint32_t>(key) % buckets != 0) return Membership::Unknown;
        if (key == 0) return Membership::Present;
        if (next == node) return Membership::Unknown;
        node = next;
    }
    return node ? Membership::Unknown : Membership::Absent;
}

// A pending native gather needs its queue entry. An outdoor room in state 1
// without its key cannot advance its countdown. Unrelated queued rooms do not
// replace that key. The originating invalidation is not yet identified;
// restoring the key retains the native gather and solve.
template<class Insert>
bool RestoreOutdoor(int state, uint32_t countdown, Membership changed,
                    Membership pending, bool active, bool outdoor, Insert&& insert) {
    if (!active || !outdoor || state != 1 || countdown == 0 ||
        changed != Membership::Absent || pending != Membership::Absent) return false;
    return std::forward<Insert>(insert)();
}
}
