#pragma once
// Mesh casters for the physical indoor lamps' shadow tiles (Direct3D 9 tier; real-time GI work package MC-3,
// docs/features/realtime-gi/wp/MC-3.md). Each placed object's caster mesh (its model's shadow LOD, else LOD 0) is read back
// from the bridge server (RemixBridge_GiMeshFetch, bridge patch 0022), which already indexes the game's packages, and kept
// here in model space: a cache with a byte budget that drops the least recently used model no placed object names. The
// tiles' occluder upload (rt_outdoor_lamps.cpp) appends every placed object's triangles in lot space. Pure: no game reads,
// no device, so tools/rt_lamps_test checks it offline.
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <list>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "rt_lamp_proxy.h"

namespace MeshCasterSet {
constexpr size_t kBudgetBytes = 16u << 20;  // model-space meshes kept (the 300 models of a lot: about 1.2-1.8 MB measured)
constexpr size_t kMaxModelTris = 2000;      // larger shadow meshes are left out (X-MESHCAST: keep their box until measured)
constexpr size_t kMaxTris = 32768;          // in the tiles' vertex buffer: 72 B each, 2.3 MB
constexpr float kLotPadM = 1.0f;            // objects this far past the lot's grid still cast
constexpr float kLampPadM = 0.05f;          // an object whose bounds hold a lamp's head within this is its lamp: no caster
inline std::atomic<bool> wanted{false};     // the Developer switch (RealtimeGI meshTiles) is on and the bridge answers

struct Model {
    std::vector<float> pos;    // xyz a vertex, model space
    std::vector<uint32_t> tri; // abc a triangle
    size_t Bytes() const { return pos.size() * 4 + tri.size() * 4; }
};
struct Object {
    uint64_t id = 0, lite = 0;
    float m[12] = {}; // three rows of four, model to world
    bool removed = false;
};
struct Stats {
    uint32_t objects = 0, models = 0, missing = 0, fetched = 0, evicted = 0, overBudget = 0, tooBig = 0;
    uint32_t lampSkips = 0; // objects left out of an upload because they hold a lamp (summed over uploads)
    size_t bytes = 0;
};

class Set {
    struct Entry {
        Model model;
        std::list<uint64_t>::iterator at;
    };
    mutable std::mutex mx_;
    std::unordered_map<uint64_t, Object> objects_;     // placed, by object id
    std::unordered_map<uint64_t, Entry> cache_;         // LITE -> its model
    std::list<uint64_t> lru_;                           // LITEs, most recent first
    std::unordered_set<uint64_t> missing_, skipped_;    // no mesh on the server; refused by the budget until an object leaves
    std::unordered_map<uint64_t, uint32_t> users_;      // LITE -> placed objects naming it
    std::vector<std::array<float, 3>> lamps_;            // the lamps' heads (world): their own objects cast nothing
    size_t budget_ = kBudgetBytes, bytes_ = 0;
    uint32_t gen_ = 0;
    Stats st_{};
    uint32_t lampSkips_ = 0;

    void Use(uint64_t lite, int d) {
        uint32_t& n = users_[lite];
        n = static_cast<uint32_t>(static_cast<int>(n) + d);
        if (!n) users_.erase(lite);
    }

  public:
    explicit Set(size_t budget = kBudgetBytes) : budget_(budget) {}
    void Clear() {
        std::lock_guard<std::mutex> lk(mx_);
        objects_.clear(), cache_.clear(), lru_.clear(), missing_.clear(), skipped_.clear(), users_.clear();
        bytes_ = 0, st_ = {}, gen_++;
    }
    // The object hooks' records (ObjectLegs::TakePlaced), in order
    void Apply(const Object& o) {
        std::lock_guard<std::mutex> lk(mx_);
        const auto it = objects_.find(o.id);
        if (it != objects_.end()) Use(it->second.lite, -1);
        if (o.removed) {
            if (it != objects_.end()) objects_.erase(it), skipped_.clear(), gen_++;
            return;
        }
        objects_[o.id] = o;
        Use(o.lite, +1);
        if (cache_.count(o.lite)) gen_++;
    }
    // The lamps' heads (world), each frame: a lamp inside its own shade would see nothing past its own mesh
    void SetLamps(const std::vector<std::array<float, 3>>& heads) {
        std::lock_guard<std::mutex> lk(mx_);
        if (heads != lamps_) lamps_ = heads, gen_++;
    }
    // The next LITE to fetch: a placed object's, not cached, not known missing and not refused; false when none
    bool NextFetch(uint64_t& lite) const {
        std::lock_guard<std::mutex> lk(mx_);
        for (const auto& [l, n] : users_)
            if (!cache_.count(l) && !missing_.count(l) && !skipped_.count(l)) return lite = l, true;
        return false;
    }
    void Missing(uint64_t lite) {
        std::lock_guard<std::mutex> lk(mx_);
        missing_.insert(lite), st_.missing++;
    }
    // A fetched model: kept when it fits the budget after dropping least recently used models no placed object names
    void Fetched(uint64_t lite, Model&& m) {
        std::lock_guard<std::mutex> lk(mx_);
        st_.fetched++;
        if (m.tri.size() / 3 > kMaxModelTris) return missing_.insert(lite), void(st_.tooBig++);
        const size_t b = m.Bytes();
        for (auto it = lru_.end(); bytes_ + b > budget_ && it != lru_.begin();) {
            --it;
            if (users_.count(*it)) continue;
            bytes_ -= cache_[*it].model.Bytes(), cache_.erase(*it), st_.evicted++;
            it = lru_.erase(it);
        }
        if (bytes_ + b > budget_) return skipped_.insert(lite), void(st_.overBudget++);
        lru_.push_front(lite);
        cache_[lite] = {std::move(m), lru_.begin()};
        bytes_ += b, gen_++;
    }
    // Every placed object's triangles in lot space (worldToLot rows: lot = world x rows), within the lot's w x h metres plus
    // kLotPadM, as object shapes; at most maxTris. Touches the models it uses. Returns the triangles added
    size_t AppendTo(RtLampProxy::Mesh& out, const float toLot[4][4], float w, float h, size_t maxTris = kMaxTris) {
        std::lock_guard<std::mutex> lk(mx_);
        out.curKind = RtLampProxy::Mesh::kShape;
        size_t added = 0;
        for (const auto& [id, o] : objects_) {
            const auto c = cache_.find(o.lite);
            if (c == cache_.end()) continue;
            lru_.splice(lru_.begin(), lru_, c->second.at);
            const Model& md = c->second.model;
            if (added + md.tri.size() / 3 > maxTris) continue;
            // model to world (the object's rows), its bounds against the lamps, then world to lot
            std::vector<std::array<float, 3>> v(md.pos.size() / 3);
            if (v.empty()) continue;
            float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
            for (size_t i = 0; i < v.size(); i++) {
                const float* p = &md.pos[i * 3];
                for (int k = 0; k < 3; k++) {
                    v[i][k] = o.m[k * 4] * p[0] + o.m[k * 4 + 1] * p[1] + o.m[k * 4 + 2] * p[2] + o.m[k * 4 + 3];
                    lo[k] = std::min(lo[k], v[i][k]), hi[k] = std::max(hi[k], v[i][k]);
                }
            }
            bool holdsLamp = false;
            for (const auto& L : lamps_)
                holdsLamp = holdsLamp || (L[0] >= lo[0] - kLampPadM && L[0] <= hi[0] + kLampPadM && L[1] >= lo[1] - kLampPadM && L[1] <= hi[1] + kLampPadM &&
                                          L[2] >= lo[2] - kLampPadM && L[2] <= hi[2] + kLampPadM);
            if (holdsLamp) {
                lampSkips_++;
                continue;
            }
            for (auto& p : v) {
                const float wv[3] = {p[0], p[1], p[2]};
                for (int k = 0; k < 3; k++) p[k] = wv[0] * toLot[0][k] + wv[1] * toLot[1][k] + wv[2] * toLot[2][k] + toLot[3][k];
            }
            if (!(v[0][0] >= -kLotPadM && v[0][0] <= w + kLotPadM && v[0][2] >= -kLotPadM && v[0][2] <= h + kLotPadM)) continue;
            for (size_t t = 0; t + 2 < md.tri.size(); t += 3) {
                const uint32_t a = md.tri[t], b = md.tri[t + 1], cc = md.tri[t + 2];
                if (a >= v.size() || b >= v.size() || cc >= v.size()) continue;
                RtLampProxy::AddTri(out, v[a].data(), v[b].data(), v[cc].data());
                added++;
            }
        }
        return added;
    }
    uint32_t Gen() const {
        std::lock_guard<std::mutex> lk(mx_);
        return gen_;
    }
    Stats GetStats() const {
        std::lock_guard<std::mutex> lk(mx_);
        Stats s = st_;
        s.lampSkips = lampSkips_;
        s.objects = static_cast<uint32_t>(objects_.size()), s.models = static_cast<uint32_t>(cache_.size()), s.bytes = bytes_;
        return s;
    }
};
inline Set g_set; // filled on the render thread (RealtimeGI), read by the tiles' upload
} // namespace MeshCasterSet
