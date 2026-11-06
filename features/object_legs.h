#pragma once
// Table legs for the GPU room lamps' 3D test (physical model, docs/features/night-lighting/gpu-room-lamps.md). The game's blockers come
// from each model's LITE, and a table's lists only its top, so its feet cast nothing. A table generated offline from the models' shadow
// LODs (tools/object_legs_table) holds the legs as capsules by LITE instance; two hooks learn each object's LITE and matrix, the occluder
// marks its first shape, and Fill gives a room's records their object's legs (Record::posts) before BlockerTable::Append.
// Custom content is not in the table: its tables keep their top alone.
#include <algorithm>
#include <cstdint>
#include <iterator>
#include <vector>
#include "room_light_tests.h"

namespace ObjectLegs {

struct Capsule {
    int16_t x0, y0, z0, x1, y1, z1, r; // mm, the model's space
};
struct Model {
    uint64_t lite;
    uint16_t first, count;
};
#include "object_legs_data.inc"

// The model of a LITE instance (0: none)
inline const Model* Baked(uint64_t lite) {
    const auto at = std::lower_bound(std::begin(kModels), std::end(kModels), lite, [](const Model& m, uint64_t v) { return m.lite < v; });
    return at != std::end(kModels) && at->lite == lite ? at : nullptr;
}
// A model's capsules through the object's matrix as the blockers take it (0x0069AD80: rows x, y, z, translation, four floats each)
inline void Place(const Model& model, const float m[16], std::vector<RoomLightTests::Post>& out) {
    const auto at = [m](int x, int y, int z) -> RoomLightTests::V {
        const float l[3] = {x * 1e-3f, y * 1e-3f, z * 1e-3f};
        return {l[0] * m[0] + l[1] * m[4] + l[2] * m[8] + m[12], l[0] * m[1] + l[1] * m[5] + l[2] * m[9] + m[13], l[0] * m[2] + l[1] * m[6] + l[2] * m[10] + m[14]};
    };
    for (int i = model.first; i < model.first + model.count; i++) {
        const Capsule& c = kCapsules[i];
        out.push_back({at(c.x0, c.y0, c.z0), at(c.x1, c.y1, c.z1), c.r * 1e-3f});
    }
}

void Install();
void Uninstall();
void NoteShape(uint64_t id, uintptr_t shape); // an occluder entry's object and first shape (LampMarkFilter's mark)
// Any thread: moves one for each object move or occluder mark seen: the 3D records' blockers may have changed (OutdoorDelta's live
// tables). The move hook is what paces it: marks wait for the room update, gaps of 0.6 to 2 s on 03/11. hook: its moves alone and thread:
// the last one's (nothing proved yet that it fires on every drag step or how often in Live mode; the [LiveTable] line tells)
uint32_t Gen();
void MoveStats(uint32_t& hook, unsigned long& thread);
void Fill(RoomLightTests::Room& room);        // any thread

} // namespace ObjectLegs
