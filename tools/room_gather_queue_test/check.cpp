#define NOMINMAX
#include <windows.h>
#include "../../features/room_gather_queue.h"
#include <cassert>
#include <set>
#include <iostream>
#include <array>
#include <vector>
#include <cstring>
using RoomGatherQueue::Membership;
using RoomById_t = void*(__thiscall*)(void*, int);
using SetInsert_t = void*(__thiscall*)(void*, void*, const int*, int);
uintptr_t kRoomById = 0, kSetInsert = 0;
#include "native_queue_fixture.h"

struct Model {
    std::set<int> pending, changed;
    int state = 2, ready = 0;
    unsigned countdown = 0, gathers = 0, solves = 0;
    void NativeUpdate(bool invalidate = false) {
        if (!pending.contains(0)) return;
        if (state == 1 && countdown && --countdown) return;
        state = 2; ++gathers;
        if (invalidate) { state = 1; countdown = 5; pending.insert(0); }
        pending.erase(0);
    }
    bool Repair() {
        return RoomGatherQueue::RestoreOutdoor(state, countdown,
            changed.contains(0) ? Membership::Present : Membership::Absent,
            pending.contains(0) ? Membership::Present : Membership::Absent,
            true, true, [&] { return pending.insert(0).second; });
    }
};
struct NativeSet { uintptr_t vtable = 0, table = 0; uint32_t buckets = 0, count = 0; uintptr_t tail[4]{}; };
struct Node { int key = -1; uintptr_t next = 0; };
static_assert(sizeof(void*) == 4 && sizeof(NativeSet) == 32 && sizeof(Node) == 8);
struct SetFixture {
    std::array<uintptr_t, 11> table{};
    std::array<Node, 64> nodes{};
    unsigned used = 0;
    NativeSet* set;
    explicit SetFixture(NativeSet* s) : set(s) { *set = {}; set->table = reinterpret_cast<uintptr_t>(table.data()); set->buckets = 11; }
    void Add(int key) {
        assert(used < nodes.size());
        const unsigned bucket = static_cast<unsigned>(key) % set->buckets;
        Node& node = nodes[used++]; node = {key, table[bucket]};
        table[bucket] = reinterpret_cast<uintptr_t>(&node); ++set->count;
    }
};
alignas(16) BYTE tl[0x1A4]{}, manager[0x300]{}, room[0x180]{};
SetFixture* pendingFixture = nullptr;
bool rejectInsert = false, missingRoom = false;
unsigned insertCalls = 0;
void* __fastcall MockRoom(void* mgr, void*, int id) {
    assert(mgr == manager && id == 0);
    return missingRoom ? nullptr : room;
}
void* __fastcall MockInsert(void* set, void*, void* out, const int* id, int) {
    assert(set == tl + 0x28 && *id == 0 && pendingFixture);
    ++insertCalls;
    const bool absent = OutdoorQueueMembership(static_cast<const BYTE*>(set)) == Membership::Absent;
    const bool inserted = absent && !rejectInsert;
    if (inserted) pendingFixture->Add(0);
    static_cast<BYTE*>(out)[8] = inserted ? 1 : 0;
    return out;
}
void Setup() {
    std::memset(tl, 0, sizeof tl); std::memset(manager, 0, sizeof manager); std::memset(room, 0, sizeof room);
    *reinterpret_cast<BYTE**>(tl) = manager;
    *reinterpret_cast<BYTE**>(room) = manager;
    *reinterpret_cast<int*>(room + 0xF0) = 1;
    *reinterpret_cast<uint32_t*>(room + 0x168) = 5;
    manager[0x280] = manager[0x81] = room[0x18] = 1;
    kRoomById = reinterpret_cast<uintptr_t>(&MockRoom); kSetInsert = reinterpret_cast<uintptr_t>(&MockInsert);
    rejectInsert = missingRoom = false; insertCalls = 0;
}
int main() {
    // Reproduce a lost key with unrelated work in either or both native sets.
    for (unsigned mask = 0; mask < 4; ++mask) {
        Model m; m.pending.insert(0); m.NativeUpdate(true);
        if (mask & 1) m.pending.insert(9);
        if (mask & 2) m.changed.insert(7);
        for (int n = 0; n < 100; ++n) m.NativeUpdate();
        assert(m.state == 1 && m.countdown == 5 && m.ready == 0);
        assert(m.Repair() && m.countdown == 5 && !m.Repair());
        for (int n = 0; n < 5; ++n) m.NativeUpdate();
        assert(m.state == 2 && m.gathers == 2);
        assert(m.pending.contains(9) == bool(mask & 1) && m.changed.contains(7) == bool(mask & 2));
    }
    unsigned inserts = 0;
    for (int state = 0; state <= 5; ++state)
        for (unsigned countdown : {0u, 1u, 5u})
            for (Membership changed : {Membership::Unknown, Membership::Absent, Membership::Present})
                for (Membership pending : {Membership::Unknown, Membership::Absent, Membership::Present})
                    for (bool active : {false, true}) for (bool outdoor : {false, true}) {
                        const bool expected = state == 1 && countdown && active && outdoor &&
                            changed == Membership::Absent && pending == Membership::Absent;
                        const unsigned before = inserts;
                        assert(RoomGatherQueue::RestoreOutdoor(state, countdown, changed, pending, active, outdoor,
                            [&] { ++inserts; return true; }) == expected);
                        assert(inserts == before + unsigned(expected));
                    }
    assert(!RoomGatherQueue::RestoreOutdoor(1, 5, Membership::Absent, Membership::Absent, true, true, [] { return false; }));
    // Execute extracted production readers and recovery, including SEH failures.
    Setup(); SetFixture changed(reinterpret_cast<NativeSet*>(tl + 8)), pending(reinterpret_cast<NativeSet*>(tl + 0x28));
    pendingFixture = &pending;
    changed.Add(11); changed.Add(22); pending.Add(33); pending.Add(44);
    assert(OutdoorQueueMembership(tl + 8) == Membership::Absent);
    assert(OutdoorQueueMembership(tl + 0x28) == Membership::Absent);
    const unsigned changedCount = changed.set->count, pendingCount = pending.set->count;
    assert(RestoreOutdoorGatherTry(tl) && insertCalls == 1);
    assert(changed.set->count == changedCount && pending.set->count == pendingCount + 1);
    assert(*reinterpret_cast<uint32_t*>(room + 0x168) == 5 && *reinterpret_cast<int*>(room + 0xF0) == 1 &&
           *reinterpret_cast<int*>(room + 0x104) == 0);
    assert(!RestoreOutdoorGatherTry(tl) && insertCalls == 1);
    // Changed room zero defers to native processing; other keys do not block it.
    Setup(); SetFixture c2(reinterpret_cast<NativeSet*>(tl + 8)), p2(reinterpret_cast<NativeSet*>(tl + 0x28)); pendingFixture = &p2;
    c2.Add(0); assert(!RestoreOutdoorGatherTry(tl) && insertCalls == 0);
    c2.table[0] = 0; c2.set->count = 0;
    rejectInsert = true; assert(!RestoreOutdoorGatherTry(tl) && insertCalls == 1 && p2.set->count == 0);
    rejectInsert = false;
    for (unsigned offset : {0x280u, 0x81u}) { manager[offset] = 0; assert(!RestoreOutdoorGatherTry(tl)); manager[offset] = 1; }
    room[0x18] = 0; assert(!RestoreOutdoorGatherTry(tl)); room[0x18] = 1;
    *reinterpret_cast<int*>(room + 0xF0) = 3; assert(!RestoreOutdoorGatherTry(tl)); *reinterpret_cast<int*>(room + 0xF0) = 1;
    *reinterpret_cast<uint32_t*>(room + 0x168) = 0; assert(!RestoreOutdoorGatherTry(tl)); *reinterpret_cast<uint32_t*>(room + 0x168) = 5;
    missingRoom = true; assert(!RestoreOutdoorGatherTry(tl)); missingRoom = false;
    *reinterpret_cast<int*>(room + 0xC) = 2; assert(!RestoreOutdoorGatherTry(tl)); *reinterpret_cast<int*>(room + 0xC) = 0;
    *reinterpret_cast<uintptr_t*>(room) = 0; assert(!RestoreOutdoorGatherTry(tl)); *reinterpret_cast<BYTE**>(room) = manager;
    assert(!RestoreOutdoorGatherTry(nullptr));
    const uintptr_t insertAt = kSetInsert; kSetInsert = 0; assert(!RestoreOutdoorGatherTry(tl)); kSetInsert = insertAt;
    const uintptr_t roomAt = kRoomById; kRoomById = 0; assert(!RestoreOutdoorGatherTry(tl)); kRoomById = roomAt;
    assert(OutdoorQueueMembership(nullptr) == Membership::Unknown);
    p2.set->count = 1; p2.table[0] = reinterpret_cast<uintptr_t>(&p2.nodes[0]);
    p2.nodes[0] = {11, p2.table[0]}; assert(OutdoorQueueMembership(tl + 0x28) == Membership::Unknown);
    p2.set->count = 0; assert(OutdoorQueueMembership(tl + 0x28) == Membership::Unknown);
    p2.set->count = 1; p2.set->buckets = 0; assert(OutdoorQueueMembership(tl + 0x28) == Membership::Unknown); p2.set->buckets = 11;
    p2.nodes[0] = {3, 0}; assert(OutdoorQueueMembership(tl + 0x28) == Membership::Unknown);
    p2.table[0] = 4; assert(OutdoorQueueMembership(tl + 0x28) == Membership::Unknown);
    p2.table[0] = 1; assert(OutdoorQueueMembership(tl + 0x28) == Membership::Unknown);
    assert(!RestoreOutdoorGatherTry(tl));
    // Compare the extracted native reader against independent set membership.
    uint32_t random = 0x913Bu;
    for (unsigned trial = 0; trial < 2048; ++trial) {
        NativeSet native;
        SetFixture fixture(&native);
        std::set<int> reference;
        for (unsigned i = 0; i < trial % 33; ++i) {
            random = random * 1664525u + 1013904223u;
            const int key = static_cast<int>((random >> 16) % 128);
            if (reference.insert(key).second) fixture.Add(key);
        }
        const Membership expected = reference.contains(0) ? Membership::Present : Membership::Absent;
        assert(OutdoorQueueMembership(reinterpret_cast<const BYTE*>(&native)) == expected);
    }
    // Long/cyclic collision chains are bounded even with inconsistent metadata.
    unsigned reads = 0;
    assert(RoomGatherQueue::LookupOutdoor(1048576, 11, 1, [&](uintptr_t n, int& key, uintptr_t& next) {
        ++reads; key = 11; next = n == 1 ? 2 : 1; return true;
    }) == Membership::Unknown && reads == 4096);
    assert(RoomGatherQueue::LookupOutdoor(1, 11, 1, [](uintptr_t, int&, uintptr_t&) { return false; }) == Membership::Unknown);
    auto noRead = [](uintptr_t, int&, uintptr_t&) { assert(false); return false; };
    assert(RoomGatherQueue::LookupOutdoor(1048577, 11, 0, noRead) == Membership::Unknown);
    assert(RoomGatherQueue::LookupOutdoor(0, 1048577, 0, noRead) == Membership::Unknown);
    assert(RoomGatherQueue::LookupOutdoor(1, 0, 0, noRead) == Membership::Unknown);
    std::cout << "Room gather queue: 648 policy combinations; 2048 native set comparisons; lost keys beside unrelated work; native-layout readers and insertion ABI; unchanged countdown/readiness; guarded corrupt and unreadable queues passed\n";
}
