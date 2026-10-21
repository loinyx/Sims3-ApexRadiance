// Pruning of the per-manager caches by liveness (features/liveness_prune.h): the hysteresis of the sweeps, the reset of a
// key written again, the counts of keys gone from the map, a value-based liveness (a room record's lot) and the managers a
// rebuilt lot no longer holds. Read-only; prints the checks and exits 1 on a failure.
#include "features/liveness_prune.h"
#include <cstdio>
#include <memory>
#include <unordered_map>

namespace {
int checks = 0, failures = 0;
void Check(bool ok, const char* text) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL: %s\n", text);
    }
}
LivenessPrune::LiveSet Live(std::initializer_list<uintptr_t> keys) {
    LivenessPrune::LiveSet s;
    for (uintptr_t k : keys) s.Add(k);
    s.Seal();
    return s;
}
} // namespace

int main() {
    using namespace LivenessPrune;
    {
        const LiveSet s = Live({0x30, 0x10, 0, 0x20, 0x10});
        Check(s.Size() == 3, "the live set drops 0 and duplicates");
        Check(s.Contains(0x10) && s.Contains(0x20) && s.Contains(0x30), "the live set holds every key added");
        Check(!s.Contains(0) && !s.Contains(0x18), "0 and a key never added are not live");
    }
    {
        // two sweeps in a row: the first miss keeps the entry, the second erases it
        std::unordered_map<uintptr_t, int> map{{0x10, 1}, {0x20, 2}, {0x30, 3}};
        Misses misses;
        const LiveSet live = Live({0x10, 0x30});
        Check(PruneByLiveness(map, live, misses) == 0, "a first miss erases nothing");
        Check(map.size() == 3 && misses.size() == 1 && misses[0x20] == 1, "a first miss is counted");
        Check(PruneByLiveness(map, live, misses) == 1, "a second miss in a row erases the entry");
        Check(map.size() == 2 && !map.contains(0x20) && misses.empty(), "the erased key keeps no count");
        Check(PruneByLiveness(map, live, misses) == 0 && map.size() == 2, "live entries are never erased");
    }
    {
        // a key live again between two misses starts over
        std::unordered_map<uintptr_t, int> map{{0x10, 1}, {0x20, 2}};
        Misses misses;
        PruneByLiveness(map, Live({0x10}), misses);
        PruneByLiveness(map, Live({0x10, 0x20}), misses);
        Check(misses.empty(), "a key found live again loses its count");
        Check(PruneByLiveness(map, Live({0x10}), misses) == 0 && map.contains(0x20), "a miss after a live sweep is a first miss again");
        Check(PruneByLiveness(map, Live({0x10}), misses) == 1 && !map.contains(0x20), "then the next miss erases it");
    }
    {
        // a key written again (Touch) between two misses is kept
        std::unordered_map<uintptr_t, int> map{{0x10, 1}, {0x20, 2}};
        Misses misses;
        const LiveSet live = Live({0x10});
        PruneByLiveness(map, live, misses);
        Touch(misses, 0x20);
        Check(misses.empty(), "Touch drops the count");
        Check(PruneByLiveness(map, live, misses) == 0 && map.contains(0x20), "a key written since the last sweep is not erased");
        Touch(misses, 0x99); // (a key with no count: nothing happens)
        Check(misses.size() == 1, "Touch of an uncounted key changes nothing");
    }
    {
        // counts of keys erased from the map elsewhere are dropped; an empty map stays empty
        std::unordered_map<uintptr_t, int> map{{0x10, 1}, {0x20, 2}};
        Misses misses;
        PruneByLiveness(map, Live({}), misses);
        Check(misses.size() == 2, "every key of an empty live set is missed once");
        map.erase(0x10);
        PruneByLiveness(map, Live({0x20}), misses);
        Check(misses.empty(), "a count of a key gone from the map is dropped");
        std::unordered_map<uintptr_t, int> none;
        Check(PruneByLiveness(none, Live({0x10}), misses) == 0 && none.empty(), "an empty map: nothing to do");
    }
    {
        // shared_ptr values (the story grids): the erased value is released, a live one stays
        auto g1 = std::make_shared<int>(1), g2 = std::make_shared<int>(2);
        std::unordered_map<uintptr_t, std::shared_ptr<const int>> map{{0x10, g1}, {0x20, g2}};
        std::weak_ptr<int> w2 = g2;
        g2.reset();
        Misses misses;
        for (int i = 0; i < 2; i++) PruneByLiveness(map, Live({0x10}), misses);
        Check(w2.expired() && map.size() == 1 && map.at(0x10) == g1, "an erased grid is released, a live one kept");
    }
    {
        // liveness by value: a room record whose lot is no longer loaded; a record with no lot is left alone
        struct Rec {
            uintptr_t tracker;
        };
        std::unordered_map<uintptr_t, Rec> rooms{{0x1000, {0xA}}, {0x2000, {0xB}}, {0x3000, {0}}};
        Misses misses;
        const LiveSet lots = Live({0xA});
        const auto loaded = [&](uintptr_t, const Rec& r) { return !r.tracker || lots.Contains(r.tracker); };
        Check(PruneByLivenessIf(rooms, loaded, misses) == 0 && misses.size() == 1, "a record of an unloaded lot is missed once");
        Check(PruneByLivenessIf(rooms, loaded, misses) == 1, "and erased at the second sweep");
        Check(rooms.contains(0x1000) && rooms.contains(0x3000) && !rooms.contains(0x2000), "the loaded lot's and the lotless records stay");
    }
    {
        // a higher limit keeps the entry longer
        std::unordered_map<uintptr_t, int> map{{0x10, 1}};
        Misses misses;
        const LiveSet live = Live({});
        Check(PruneByLiveness(map, live, misses, 3) == 0 && PruneByLiveness(map, live, misses, 3) == 0, "limit 3: two misses keep it");
        Check(PruneByLiveness(map, live, misses, 3) == 1, "limit 3: the third erases it");
    }
    {
        // a rebuilt lot: the old managers that the lot no longer holds anywhere
        const uintptr_t before[8] = {0x10, 0x20, 0x30, 0, 0, 0x30, 0, 0};
        const uintptr_t after[12] = {0x40, 0x20, 0x50, 0, 0, 0, 0, 0, 0, 0, 0, 0x10}; // 0x10 moved to another story
        uintptr_t out[8] = {};
        const int n = Gone(before, 8, after, 12, out);
        Check(n == 1 && out[0] == 0x30, "only a manager in none of the new stories is gone, once");
        const uintptr_t same[8] = {0x10, 0x20, 0x30, 0, 0, 0, 0, 0};
        Check(Gone(before, 8, same, 8, out) == 0, "the same managers: none gone");
        const uintptr_t empty[8] = {};
        Check(Gone(empty, 8, after, 12, out) == 0, "a first sight (no managers before): none gone");
        Check(Gone(before, 8, empty, 8, out) == 3, "every old manager gone when the lot holds none");
    }
    {
        // the miss count kept in the value (OutdoorDelta's records, keyed by room and class): a record whose story manager no
        // loaded lot holds goes at the second sweep in a row, with its bytes given back by onErase; a live one keeps its count at 0
        struct Rec {
            uintptr_t mgr;
            size_t bytes;
            uint8_t gone = 0;
        };
        std::unordered_map<uint64_t, std::unique_ptr<Rec>> recs;
        recs[(uint64_t{0x1000} << 2) | 2] = std::make_unique<Rec>(Rec{0xA0, 100});
        recs[(uint64_t{0x1000} << 2) | 0] = std::make_unique<Rec>(Rec{0xA0, 10});
        recs[(uint64_t{0x2000} << 2) | 2] = std::make_unique<Rec>(Rec{0xB0, 300});
        size_t bytes = 410, erasedBytes = 0;
        const auto missesOf = [](std::unique_ptr<Rec>& r) -> uint8_t& { return r->gone; };
        const auto onErase = [&](uint64_t, std::unique_ptr<Rec>& r) { erasedBytes += r->bytes, bytes -= r->bytes; };
        LiveSet mgrs = Live({0xA0});
        const auto live = [&](uint64_t, const std::unique_ptr<Rec>& r) { return mgrs.Contains(r->mgr); };
        Check(PruneInPlace(recs, live, missesOf, onErase) == 0 && recs.size() == 3, "in place: a missing record is kept after one sweep");
        Check(recs.at((uint64_t{0x2000} << 2) | 2)->gone == 1, "in place: its count is 1");
        mgrs = Live({0xA0, 0xB0}); // (the lot was streaming: back in the list)
        Check(PruneInPlace(recs, live, missesOf, onErase) == 0 && recs.at((uint64_t{0x2000} << 2) | 2)->gone == 0, "in place: seen again, its count starts over");
        mgrs = Live({0xA0});
        Check(PruneInPlace(recs, live, missesOf, onErase) == 0, "in place: one miss again");
        Check(PruneInPlace(recs, live, missesOf, onErase) == 1 && erasedBytes == 300 && bytes == 110, "in place: erased at the second miss in a row, its bytes given back");
        Check(recs.size() == 2 && recs.contains((uint64_t{0x1000} << 2) | 2) && recs.contains((uint64_t{0x1000} << 2) | 0), "in place: both classes of the live room stay");
        mgrs = Live({});
        Check(PruneInPlace(recs, live, missesOf, onErase) == 0 && PruneInPlace(recs, live, missesOf, onErase) == 2 && recs.empty() && bytes == 0,
              "in place: every record of a lot that left goes after two sweeps");
    }
    std::printf("liveness prune: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
