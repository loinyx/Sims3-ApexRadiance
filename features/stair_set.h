#pragma once
// Stairs for the physical indoor lamps' shadow tiles. The occluders hold walls, roofs and slabs only, so a stair cast nothing.
// The opaque instanced draws read by the mesh collector (lot_light_bridge.cpp) hand their triangles here; those over or
// beside a floor hole, between the hole's story and the one below it, stay in lot space until the lot's geometry generation
// changes. The set only grows, so it does not follow the camera, and it reaches the GPU tiles alone: the CPU builds of the
// occluders never hold it (samples under the treads would darken the stair), and no room goes to its solve for it.
#include <atomic>
#include <cstdint>
#include <cstring>
#include <unordered_set>
#include <vector>
#include "mesh_occlusion.h"
#include "rt_lamp_proxy.h"

namespace StairSet {
constexpr size_t kMaxTris = 4096;       // 36 B each here and 72 B in the vertex buffer
constexpr float kMaxSpanM = 8.0f;       // a triangle wider than this over the plan is no stair
constexpr float kBandPadM = 0.1f;       // m past the two floors a stair joins
constexpr float kStoryDropM = 3.5f;     // the story below's floor when it is not read
constexpr uint32_t kQuietMs = 2000;     // a grown set waits this long for the rest of the stair
constexpr uint32_t kUploadGapMs = 10000, kMaxUploads = 12; // each upload redraws the tiles and relights the physical maps
inline std::atomic<bool> wanted{false};     // the collector reads draws for the set
inline std::atomic<uint32_t> generation{0}; // a new set: the collector reads every group again

class Set {
    struct Band {
        float lo = 1e30f, hi = -1e30f;
    };
    float toLot_[4][4] = {};
    int w_ = 0, h_ = 0;
    std::vector<Band> bands_; // per tile (iz * w + ix): the heights of the holes within a tile of it
    std::vector<RtLampProxy::Tri> tris_;
    std::unordered_set<uint64_t> known_;
    uint32_t gen_ = 0, uploads_ = 0, grewAt_ = 0, uploadAt_ = 0, groups_ = 0, seen_ = 0;
    bool dirty_ = false, full_ = false;

    bool Near(const float lo[3], const float hi[3]) const {
        const int x0 = std::max(static_cast<int>(std::floor(lo[0])), 0), x1 = std::min(static_cast<int>(std::floor(hi[0])), w_ - 1);
        const int z0 = std::max(static_cast<int>(std::floor(lo[2])), 0), z1 = std::min(static_cast<int>(std::floor(hi[2])), h_ - 1);
        for (int z = z0; z <= z1; z++)
            for (int x = x0; x <= x1; x++)
                if (const Band& b = bands_[static_cast<size_t>(z) * w_ + x]; hi[1] >= b.lo && lo[1] <= b.hi) return true;
        return false;
    }

public:
    void Clear() {
        *this = Set{};
        wanted.store(false, std::memory_order_relaxed);
        generation.fetch_add(1, std::memory_order_relaxed);
    }
    // A lot's new geometry: its floor holes, and an empty set
    void Begin(const RtLampProxy::Lot& lot, uint32_t gen) {
        Clear();
        gen_ = gen;
        std::memcpy(toLot_, lot.worldToLot, sizeof toLot_);
        bool holes = false;
        for (const RtLampProxy::Story& s : lot.stories) {
            if (s.w <= 0 || s.h <= 0 || s.removed.size() != static_cast<size_t>(s.w) * s.h || !(s.plane < RtLampProxy::kNoFloor * 0.5f)) continue;
            if (bands_.empty()) w_ = s.w, h_ = s.h, bands_.resize(static_cast<size_t>(w_) * h_);
            if (s.w != w_ || s.h != h_) continue;
            float low = s.plane - kStoryDropM;
            for (const RtLampProxy::Story& b : lot.stories)
                if (b.story == s.story - 1 && b.plane < RtLampProxy::kNoFloor * 0.5f) low = b.plane;
            for (int z = 0; z < h_; z++)
                for (int x = 0; x < w_; x++) {
                    if (!s.removed[static_cast<size_t>(z) * w_ + x]) continue;
                    holes = true;
                    for (int j = std::max(z - 1, 0); j <= std::min(z + 1, h_ - 1); j++)
                        for (int i = std::max(x - 1, 0); i <= std::min(x + 1, w_ - 1); i++) {
                            Band& b = bands_[static_cast<size_t>(j) * w_ + i];
                            b.lo = std::min(b.lo, low - kBandPadM), b.hi = std::max(b.hi, s.plane + kBandPadM);
                        }
                }
        }
        wanted.store(holes, std::memory_order_relaxed); // (a lot with no hole reads no draw)
    }
    // One group's triangles (world space)
    void Add(const std::vector<MeshOcclusion::Triangle>& group, uint32_t now) {
        if (bands_.empty()) return;
        groups_++;
        RtLampProxy::Lot lot;
        std::memcpy(lot.worldToLot, toLot_, sizeof toLot_);
        for (const MeshOcclusion::Triangle& t : group) {
            seen_++;
            RtLampProxy::Tri l;
            const MeshOcclusion::Vec* const v[3] = {&t.a, &t.b, &t.c};
            float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
            uint64_t key = 1469598103934665603ull;
            for (int i = 0; i < 3; i++) {
                const float w[3] = {v[i]->x, v[i]->y, v[i]->z};
                RtLampProxy::ToLot(lot, w, l.v[i]);
                for (int k = 0; k < 3; k++) {
                    lo[k] = std::min(lo[k], l.v[i][k]), hi[k] = std::max(hi[k], l.v[i][k]);
                    key = (key ^ static_cast<uint64_t>(std::llround(l.v[i][k] * 1000.0f))) * 1099511628211ull; // (1 mm: the same tread read twice)
                }
            }
            if (!MeshOcclusion::Finite(t.a) || !MeshOcclusion::Finite(t.b) || !MeshOcclusion::Finite(t.c) || hi[0] - lo[0] > kMaxSpanM || hi[2] - lo[2] > kMaxSpanM || !Near(lo, hi)) continue;
            if (tris_.size() >= kMaxTris) {
                full_ = true;
                wanted.store(false, std::memory_order_relaxed);
                return;
            }
            if (!known_.insert(key).second) continue;
            tris_.push_back(l);
            dirty_ = true, grewAt_ = now;
        }
    }
    // The set into an occluder mesh (the tiles' own)
    void AppendTo(RtLampProxy::Mesh& m) const {
        if (tris_.empty()) return;
        m.curKind = RtLampProxy::Mesh::kShape;
        for (const RtLampProxy::Tri& t : tris_) RtLampProxy::AddTri(m, t.v[0], t.v[1], t.v[2]);
        RtLampProxy::BuildGrid(m);
    }
    bool Due(uint32_t now) const { return dirty_ && uploads_ < kMaxUploads && now - grewAt_ >= kQuietMs && (!uploads_ || now - uploadAt_ >= kUploadGapMs); }
    void Uploaded(uint32_t now) {
        dirty_ = false, uploads_++, uploadAt_ = now;
        if (uploads_ >= kMaxUploads) wanted.store(false, std::memory_order_relaxed); // (no upload left: the scan frames stop)
    }
    uint32_t Gen() const { return gen_; }
    size_t Tris() const { return tris_.size(); }
    uint32_t Groups() const { return groups_; }
    uint32_t Seen() const { return seen_; }
    uint32_t Uploads() const { return uploads_; }
    bool Full() const { return full_; }
};
inline Set g_set; // (render thread)
} // namespace StairSet
