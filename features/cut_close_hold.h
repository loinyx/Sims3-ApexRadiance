#pragma once
// Openings of walls (level_light_share.cpp NoteCutoutPostImpl): a cut closed again counts for lamp light only once it stays
// closed for kHoldMs. Moving the camera made the game post a lot's cuts again (in game, three camera turns, 21 posts each):
// its window cuts closed and opened again 0.18 to 0.67 s later. Each close took the windows out of the story's grid, the
// roofed rooms behind them gathered and solved with 2 doors to the outside instead of 5, then again with 5: the light of a
// room depended on how the camera had moved. An add of the same edge within the hold cancels the close; a window really
// removed lets its light out kHoldMs later. Pure, no game reads (tools/roofed_openings_test includes it).
#include <algorithm>
#include <cstdint>
#include <vector>

namespace CutCloseHold {

constexpr uint32_t kHoldMs = 2000;
constexpr size_t kMax = 1024;
struct Pending {
    uint64_t lot = 0;
    int story = 0;
    uint32_t edge = 0;
    uint32_t at = 0; // GetTickCount of the first close of a run
};
inline auto Find(std::vector<Pending>& p, uint64_t lot, int story, uint32_t edge) {
    return std::find_if(p.begin(), p.end(), [&](const Pending& e) { return e.lot == lot && e.story == story && e.edge == edge; });
}
// A close of an edge that holds a cut: false when the table is full (the caller closes it at once, as before)
inline bool NoteClose(std::vector<Pending>& p, uint64_t lot, int story, uint32_t edge, uint32_t now) {
    if (Find(p, lot, story, edge) != p.end()) return true; // (a repeated close keeps the first one's time)
    if (p.size() >= kMax) return false;
    p.push_back(Pending{lot, story, edge, now});
    return true;
}
// An add of the edge: true when a close was waiting (it is dropped)
inline bool Cancel(std::vector<Pending>& p, uint64_t lot, int story, uint32_t edge) {
    const auto it = Find(p, lot, story, edge);
    if (it == p.end()) return false;
    p.erase(it);
    return true;
}
// The closes held kHoldMs: apply(lot, story, edge) for each, then dropped. Returns how many.
template <class F> int Due(std::vector<Pending>& p, uint32_t now, F&& apply) {
    int n = 0;
    for (auto it = p.begin(); it != p.end();)
        if (now - it->at >= kHoldMs) {
            apply(it->lot, it->story, it->edge);
            it = p.erase(it);
            ++n;
        } else
            ++it;
    return n;
}

} // namespace CutCloseHold
