#pragma once
#include <cstdint>
#include "terrain_bake_rule.h"

namespace WorldLampPolicy {
inline bool Track(std::uint64_t lot, int type) { return lot != 0 || type == 11; }
inline bool Eligible(std::uint64_t lot, int type, unsigned flags, int room) {
    if (!(flags & 1) || !Track(lot, type) || (type != 11 && !TerrainBakeRule::LotLampType(type))) return false;
    return lot == 0 || ((flags & 4) && room == 0);
}
// ID zero groups all world lamps. Value edits may affect many known lamps;
// streaming additions/removals and unobserved switches are not priority edits.
inline bool AcceptEdit(std::uint64_t lot, int added, int removed, int edited, bool observed) {
    return lot != 0 || (added == 0 && removed == 0 && edited > 0 && observed);
}
} // namespace WorldLampPolicy
