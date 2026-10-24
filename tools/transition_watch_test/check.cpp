#include "../../framework/transition_watch_state.h"
#include "../../framework/transition_watch_native.h"
#include "../../framework/transition_watch.h"
#include <cassert>
#include <cstring>
#include <iostream>

using namespace TransitionWatch;
template<class T, size_t N> void Put(std::array<unsigned char, N>& memory, size_t offset, T value) {
    std::memcpy(memory.data() + offset, &value, sizeof value);
}
int main() {
    Start(); // The normal public flavor has no observer or diagnostic dependencies.
    Pump();
    Tracker t;
    assert(t.Update(0, 1, true, true, 0) == Event::None);
    assert(t.Update(10000, 1, true, true, 0) == Event::None); // initial loading is not an armed gap
    t.Update(10500, 1, true, true, 100);
    t.Update(11500, 1, true, true, 100);
    t.Update(12000, 1, true, true, 0);
    assert(t.Update(12500, 1, true, true, 0) == Event::None);
    assert(t.Update(13000, 1, true, true, 0) == Event::Begin);
    assert(t.missingAt == 12000);
    assert(t.Update(22500, 1, true, true, 0) == Event::None);
    assert(t.Update(23000, 1, true, true, 0) == Event::Waiting);
    t.Update(23500, 1, true, true, 1);
    t.Update(24000, 1, true, true, 0); // a transient draw does not end the gap
    t.Update(24500, 1, true, true, 100);
    assert(t.Update(25500, 1, true, true, 100) == Event::Resumed);
    assert(!t.tracking && t.armed);
    t.Update(26000, 1, true, true, 0);
    assert(t.Update(27000, 1, true, true, 0) == Event::Begin);
    assert(t.Update(27500, 2, true, true, 100) == Event::Interrupted);
    assert(!t.tracking && !t.armed);
    t.Update(28000, 2, true, true, 100);
    t.Update(29000, 2, true, true, 100);
    t.Update(30000, 2, true, true, 0);
    t.Update(31000, 2, true, true, 0);
    assert(t.Update(31500, 2, true, false, 0) == Event::Interrupted);
    assert(!t.tracking);
    for (unsigned i = 0; i < 10000; ++i) assert(t.Update(32000 + i * 500ull, 2, true, false, 0) == Event::None);

    std::array<unsigned char, 0x30> renderer{}, buckets{}, node{};
    std::array<unsigned char, 0x250> object{};
    std::array<unsigned char, 0xD0> lot{};
    Put(renderer, 0x1C, reinterpret_cast<uintptr_t>(buckets.data()));
    Put(renderer, 0x20, 1u);
    Put(buckets, 0, reinterpret_cast<uintptr_t>(node.data()));
    Put(node, 8, reinterpret_cast<uintptr_t>(object.data()));
    Put(object, 0x18, reinterpret_cast<uintptr_t>(lot.data()));
    Put(object, 0x10, uint64_t{0x123456789ABCDEF0});
    Put(object, 0x24, 20u);
    Put(object, 0x28, 7u);
    lot[0xC1] = 1;
    Native s;
    ReadLots(reinterpret_cast<uintptr_t>(renderer.data()), &s);
    assert(s.readable && s.requested == 1 && s.pending == 1 && s.stored == 1);
    assert(s.lots[0].stage == 20 && s.lots[0].step == 7 && s.lots[0].id == 0x123456789ABCDEF0);
    object[0x1E] = 1;
    s = {};
    ReadLots(reinterpret_cast<uintptr_t>(renderer.data()), &s);
    assert(s.readable && !s.pending && s.lots[0].done == 1); // native done cannot end the Tracker gap
    lot[0xC1] = 0;
    s = {};
    ReadLots(reinterpret_cast<uintptr_t>(renderer.data()), &s);
    assert(s.readable && !s.requested && !s.stored);
    lot[0xC1] = 1;
    Put(node, 0x10, reinterpret_cast<uintptr_t>(node.data())); // corrupt cycle is bounded
    s = {};
    ReadLots(reinterpret_cast<uintptr_t>(renderer.data()), &s);
    assert(!s.readable && s.truncated && s.stored == 16 && s.requested == 1024);
    s = {};
    ReadLots(1, &s);
    assert(!s.readable);
    Put(renderer, 0x20, 8193u);
    ReadLots(reinterpret_cast<uintptr_t>(renderer.data()), &s);
    assert(!s.readable);
    std::cout << "Transition watch: startup, debounce, wait, resumption, interruption, unavailable counting, native fields and bounded corrupt reads passed\n";
}
