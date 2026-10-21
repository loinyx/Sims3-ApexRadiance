#pragma once
// Caches keyed by a story manager or a lot tracker (level_light_share.cpp: the outside walls' and slabs' copies, the story
// grid caches, the room records) are pruned by liveness: an entry whose key no loaded lot holds is erased once it has been
// missing from the live set in kMissesToErase sweeps in a row, so a lot still streaming in, or a list read while the game
// rebuilds it, never loses its entries on a single miss. Pure, header-only (tools/liveness_prune_test).
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace LivenessPrune {

constexpr uint8_t kMissesToErase = 2;

// A sorted set of live keys; 0 is never live
struct LiveSet {
    std::vector<uintptr_t> keys;
    void Add(uintptr_t k) {
        if (k) keys.push_back(k);
    }
    // Sorts the keys and drops duplicates (call once, after the last Add)
    void Seal() {
        std::sort(keys.begin(), keys.end());
        keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    }
    bool Contains(uintptr_t k) const { return k && std::binary_search(keys.begin(), keys.end(), k); }
    size_t Size() const { return keys.size(); }
};

using Misses = std::unordered_map<uintptr_t, uint8_t>; // key -> sweeps in a row it was missing

// One sweep over `map`: an entry for which isLive(key, value) is false counts one more miss and is erased at `limit` misses
// in a row; a live entry loses its count. Counts of keys no longer in the map are dropped. Returns the number erased.
template <class Map, class IsLive>
size_t PruneByLivenessIf(Map& map, IsLive&& isLive, Misses& misses, uint8_t limit = kMissesToErase) {
    size_t erased = 0;
    for (auto it = map.begin(); it != map.end();) {
        const uintptr_t key = static_cast<uintptr_t>(it->first);
        if (isLive(it->first, it->second)) {
            if (!misses.empty()) misses.erase(key);
            ++it;
            continue;
        }
        uint8_t& m = misses[key];
        if (m < 0xFF) ++m;
        if (m >= limit) {
            misses.erase(key);
            it = map.erase(it);
            ++erased;
        } else
            ++it;
    }
    for (auto it = misses.begin(); it != misses.end();) {
        if (map.find(static_cast<typename Map::key_type>(it->first)) == map.end()) it = misses.erase(it);
        else ++it;
    }
    return erased;
}

// The same, live when the map's key is in `live`
template <class Map>
size_t PruneByLiveness(Map& map, const LiveSet& live, Misses& misses, uint8_t limit = kMissesToErase) {
    return PruneByLivenessIf(map, [&live](const auto& key, const auto&) { return live.Contains(static_cast<uintptr_t>(key)); }, misses, limit);
}

// The same rule with the miss count kept in each value (`misses(value)` gives a uint8_t&), for maps whose keys are not a
// pointer (OutdoorDelta's records: room and class). onErase(key, value) runs just before an entry is erased. Returns how many.
template <class Map, class IsLive, class MissesOf, class OnErase>
size_t PruneInPlace(Map& map, IsLive&& isLive, MissesOf&& misses, OnErase&& onErase, uint8_t limit = kMissesToErase) {
    size_t erased = 0;
    for (auto it = map.begin(); it != map.end();) {
        uint8_t& m = misses(it->second);
        if (isLive(it->first, it->second)) {
            m = 0;
            ++it;
            continue;
        }
        if (m < 0xFF) ++m;
        if (m >= limit) {
            onErase(it->first, it->second);
            it = map.erase(it);
            ++erased;
        } else
            ++it;
    }
    return erased;
}

// A key written again (its cache entry refreshed from a live source): its miss count starts over
inline void Touch(Misses& misses, uintptr_t key) {
    if (!misses.empty()) misses.erase(key);
}

// The keys of before[0..nb) that are in none of after[0..na), each once, 0 skipped: written to out (room for nb keys).
// Returns how many. POD only.
inline int Gone(const uintptr_t* before, int nb, const uintptr_t* after, int na, uintptr_t* out) {
    int n = 0;
    for (int i = 0; i < nb; i++) {
        const uintptr_t k = before[i];
        if (!k || std::find(after, after + na, k) != after + na || std::find(out, out + n, k) != out + n) continue;
        out[n++] = k;
    }
    return n;
}

} // namespace LivenessPrune
