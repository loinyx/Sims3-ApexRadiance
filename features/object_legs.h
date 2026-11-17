#pragma once
// Table legs for the GPU room lamps' 3D test (physical model, docs/features/night-lighting/gpu-room-lamps.md). The game's blockers come
// from each model's LITE, and a table's lists only its top, so its feet cast nothing. A table generated offline from the models' shadow
// LODs (tools/object_legs_table) holds the legs as capsules by LITE instance; two hooks learn each object's LITE and matrix, the occluder
// marks its first shape, and Fill gives a room's records their object's legs (Record::posts) before BlockerTable::Append.
// Custom content is not in the table: its tables keep their top alone.
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <unordered_map>
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
// tables). The move hook is what paces it: marks wait for the room update, gaps of 0.6 to 2 s in one recording. hook: its moves alone and thread:
// the last one's (nothing proved yet that it fires on every drag step or how often in Live mode; the [LiveTable] line tells)
uint32_t Gen();
void MoveStats(uint32_t& hook, unsigned long& thread);
void Fill(RoomLightTests::Room& room);        // any thread

// Mesh casters (RealtimeGI meshCasters, wp/MC.md): every object with a LITE, not only the baked tables, in the bridge's
// RemixBridge_GiObjects record (util_gi.h GiObject). The two hooks and the object removal fill it while the sink is on.
struct Placed {
    uint64_t id, lite;
    float m[12]; // three rows of four, model to world: the matrix's columns as Place reads it
    uint32_t flags, reserved; // flags 1: removed
};
static_assert(sizeof(Placed) == 72, "the bridge's GiObject layout");
// The objects' feed (pure; the caller locks; wp/MC-4.md): an object goes out once its LITE (attach) and a matrix (move) are both
// known, in either order, and again at each Seed (world entry, lot enter, the bridge opening): a loaded lot's still objects
// cast without moving. Its first occluder shape names it for Mark (LampMarkFilter's mark, as NoteShape). (MC-4b) The objects are
// known while the hooks are in, also with the output off (SetOn(false): nothing goes out); turning it on seeds every known
// object, so a feature turned on inside a world casts the objects attached before
class Feed {
    struct Known {
        uint64_t lite = 0;
        float m[12] = {};
        bool hasM = false;
    };
    std::unordered_map<uint64_t, Known> known_;
    std::unordered_map<uintptr_t, uint64_t> shapes_;
    std::unordered_map<uint64_t, Placed> out_;
    uint32_t seeded_ = 0, refused_ = 0;
    bool on_ = true;
    void Out(uint64_t id, const Known& k) {
        if (!on_) return;
        Placed& p = out_[id];
        p = {id, k.lite, {}, 0, 0};
        std::memcpy(p.m, k.m, sizeof p.m);
    }
    Known* Slot(uint64_t id) {
        if (const auto it = known_.find(id); it != known_.end()) return &it->second;
        if (known_.size() >= kMax) std::erase_if(known_, [](const auto& e) { return !e.second.lite; }); // (moves of objects with no LITE go first)
        if (known_.size() >= kMax) return refused_++, nullptr;
        return &known_[id];
    }

  public:
    static constexpr size_t kMax = 65536;
    explicit Feed(bool on = true) : on_(on) {}
    // On: every known object out at the next Take (returns how many); off: nothing goes out, the objects stay known
    size_t SetOn(bool on) {
        if (on == on_) return 0;
        on_ = on;
        if (!on) return out_.clear(), 0;
        return Seed();
    }
    bool On() const { return on_; }
    void Attach(uint64_t id, uint64_t lite) {
        Known* k = Slot(id);
        if (!k) return;
        k->lite = lite;
        if (k->hasM) Out(id, *k);
    }
    void Move(uint64_t id, const float m[16]) { // (the hook's float4x4: rows x, y, z, translation; kept as Placed's three rows)
        Known* k = Slot(id);
        if (!k) return;
        const float r[12] = {m[0], m[4], m[8], m[12], m[1], m[5], m[9], m[13], m[2], m[6], m[10], m[14]};
        std::memcpy(k->m, r, sizeof r), k->hasM = true;
        if (k->lite) Out(id, *k);
    }
    void Remove(uint64_t id) {
        const auto it = known_.find(id);
        if (it == known_.end()) return;
        if (on_ && it->second.lite) out_[id] = {id, 0, {}, 1, 0};
        known_.erase(it);
    }
    void Shape(uint64_t id, uintptr_t shape) {
        if (!shape) return;
        if (known_.count(id) && shapes_.size() < kMax) shapes_[shape] = id;
        else shapes_.erase(shape); // (a freed object's shape address, another object's now)
    }
    uint64_t ObjectOf(uintptr_t shape) const {
        const auto it = shapes_.find(shape);
        return it == shapes_.end() ? 0 : it->second;
    }
    // Every object with a LITE and a matrix out again; returns how many
    size_t Seed() {
        if (!on_) return 0;
        size_t n = 0;
        for (const auto& [id, k] : known_)
            if (k.lite && k.hasM) Out(id, k), n++;
        seeded_ += static_cast<uint32_t>(n);
        return n;
    }
    void Take(std::vector<Placed>& out) {
        out.clear();
        for (const auto& [id, p] : out_) out.push_back(p);
        out_.clear();
    }
    void Clear() { known_.clear(), shapes_.clear(), out_.clear(); }
    size_t KnownCount() const { return known_.size(); }
    uint32_t Seeded() const { return seeded_; }
    uint32_t Refused() const { return refused_; }
};
// (meshTiles) The records of objects whose own mesh is in the lamps' shadow tiles (inTiles(id)) leave the 3D test: the tiles
// darken them once (wp/MC-4.md). Unresolved objects keep the game's blockers. Returns the records marked
template <class InTiles>
size_t MarkMeshed(std::vector<RoomLightTests::Record>& rec, const Feed& f, InTiles inTiles) {
    size_t n = 0;
    for (RoomLightTests::Record& rc : rec) {
        const uint64_t id = rc.shape ? f.ObjectOf(rc.shape) : 0;
        rc.meshed = id && inTiles(id);
        if (rc.meshed) rc.posts.clear(), n++;
    }
    return n;
}
// (MC-4b) Which records are marked (their indices, hashed): a kept room's live table differs from its solve's when this does
inline uint64_t MeshedKey(const std::vector<RoomLightTests::Record>& rec) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < rec.size(); i++)
        if (rec[i].meshed) h = (h ^ (i + 1)) * 1099511628211ull;
    return h;
}

bool Hooked();                         // both hooks are in (LampMarkFilter installs them)
void SetSink(bool on);                 // off: the records are dropped
void TakePlaced(std::vector<Placed>& out); // the objects changed since the last call, the latest record of each
size_t Seed();                         // every known object out again at the next TakePlaced (Feed::Seed)
uint32_t Seeded();                     // objects sent by seeds, summed
uint32_t Meshed();                     // 3D records left to the tiles (MarkMeshed), summed over Fills
// (MC-4b, render thread) Moves when what MarkMeshed would mark may have changed: an object into or out of the tiles (moved,
// added, removed, its mesh fetched), a LITE the server resolved, the tiles or the per-pixel pass turned on or off. A kept
// room's table is marked again then (OutdoorDelta::KeptRooms), not only at its next solve
uint32_t MeshGen();
void NoteRemoved(uint64_t id);         // LampMarkFilter's object removal hook

} // namespace ObjectLegs
