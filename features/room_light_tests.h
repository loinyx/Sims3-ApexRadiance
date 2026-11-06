#pragma once
// The game's own light tests of a room's solve, as pure functions over a copy of what they read (GPU room lamps stage 3,
// docs/features/night-lighting/gpu-room-lamps.md; the offline replays tools/room_lamp_3d_replay and tools/apex_paths_replay
// run these same functions over the room lamp probe's dumps, 100% of the recorded texels within 2/255):
//   - the 3D (furniture) test 0x0069FCE0 -> 0x0069D910 -> blocker vfunc+0x14 (0x0069BEA0 soft rectangles, 0x0069C1A0 soft
//     discs) and their helpers 0x0069B590 / 0x0069B650 / 0x0069B4C0;
//   - the 2D wall test 0x0069FC40 -> 0x0069D4C0 -> 0x0069AA90 / 0x0069A8C0, with the per-batch wall list 0x006A30B0 ->
//     0x0069DFF0 (the walls whose cull segment the batch centroid -> light ray crosses).
// No game memory is read here: the walls (0x100 bytes each), the 3D records and their blockers (0x100 bytes each) and the
// constants are copies. Any thread.
#include <xmmintrin.h>
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace RoomLightTests {

struct V {
    float x, y, z;
};
inline V sub(V a, V b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
// the game's horizontal sum of a 4-lane product: (z + y) + x
inline float dot(V a, V b) {
    float px = a.x * b.x, py = a.y * b.y, pz = a.z * b.z;
    return (pz + py) + px;
}
inline float rcp(float x) { return _mm_cvtss_f32(_mm_rcp_ss(_mm_set_ss(x))); }
inline float clamp01(float x) { return std::min(std::max(x, 0.0f), 1.0f); }

// the constants the tests read (defaults = the values seen in the game; the probe's dump and the live game give them: Consts)
inline float K_ZERO = 0.0f, K_TWO = 2.0f, K_NEG1 = -1.0f, K_FOUR = 4.0f, K_ONE = 1.0f, K_015 = 0.15f, K_00625 = 0.0625f, K_EPS = 1e-4f, K_STOP = 0.01f,
             K_DROP = 0.01f, K_HALF = 0.5f, K_PEN_C = 0.5f, K_WALL_Y = 1.0f; // [00F9A5AC], [00FF307C], [01158A50]
// Sets the constant at game address `at` (false: not one of them)
inline bool SetConst(uint32_t at, float v) {
    switch (at) {
    case 0x00F98964: K_ZERO = v; return true;
    case 0x00F98A58: K_TWO = v; return true;
    case 0x00FAA210: K_NEG1 = v; return true;
    case 0x00FB2F6C: K_FOUR = v; return true;
    case 0x00FECE18: K_00625 = v; return true;
    case 0x00FF31AC: K_STOP = v; return true;
    case 0x0106B4B4: K_015 = v; return true;
    case 0x0106D6CC: K_DROP = v; return true;
    case 0x0107A538: K_ONE = v; return true;
    case 0x011D0240: K_EPS = v; return true;
    case 0x00F9A5AC: K_HALF = v; return true;
    case 0x00FF307C: K_PEN_C = v; return true;
    case 0x01158A50: K_WALL_Y = v; return true;
    }
    return false;
}
inline constexpr uint32_t kConstAt[] = {0x00F98964, 0x00F98A58, 0x00FAA210, 0x00FB2F6C, 0x00FECE18, 0x00FF31AC, 0x0106B4B4,
                                        0x0106D6CC, 0x0107A538, 0x011D0240, 0x00F9A5AC, 0x00FF307C, 0x01158A50};
inline constexpr uint32_t kQuadVt = 0x00FF3118, kDiscVt = 0x00FF3C24; // the blockers the 3D test knows
inline constexpr uint32_t kPostVt = 2; // (no game vtable) a table's leg of the GPU's copy: RtLampGpu::BlockerTable

struct Blocker {
    uint32_t vt;
    float f[64];
    V v(int off) const { return {f[off / 4], f[off / 4 + 1], f[off / 4 + 2]}; }
};
// A table's leg as a capsule, in the record's space (features/object_legs.h). Only the GPU's blocker table reads it
struct Post {
    V a, b;
    float r;
};
struct Record {
    V c;
    float r2;
    std::vector<Blocker> b;
    float cull2 = 0.0f; // record+0x20: the per-batch list's cull radius^2 (the tests here read all records)
    // its first blocker's address in the game: its object's first occluder shape (0x0069E3F0 pushes the object's lighting
    // record's shapes, +0x40..+0x44, into the record's blockers), which names the object (OccMove::shape); 0 unknown
    uintptr_t shape = 0;
    // (a copy only: a table dragged with its lamp ahead of its solve) the record drawn moved by (off[0], 0, off[1]): `c` moved
    // with it, its blockers tested on the segment moved back (occlude; RtLampGpu::BlockerTable::Offset)
    float off[2] = {0.0f, 0.0f};
    std::vector<Post> posts; // its object's legs (RtLampGpu::BlockerTable::Append); the game's tests here ignore them
    std::vector<uintptr_t> at; // its blockers' addresses in the game, as read: the object's move writes them in place (OutdoorDelta's live tables)
};
// A record's sphere as the gather makes it (0x0069E455..0x0069E4DB): the box of its blockers' bounds, its middle and half diagonal. A
// rectangle's bound is its first plane's corners (0x0069B1D0), a disc's its first circle along x and z (not read in the bytes: all 252
// records of the 19-37-16 scene of 05/11, 24 with discs, come out within 1 cm of the game's centre and radius)
inline void Bound(Record& rc) {
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    const auto add = [&](V p) {
        const float c[3] = {p.x, p.y, p.z};
        for (int i = 0; i < 3; i++) lo[i] = std::min(lo[i], c[i]), hi[i] = std::max(hi[i], c[i]);
    };
    for (const Blocker& k : rc.b)
        if (k.vt == kQuadVt) {
            const V A = k.v(0x70), U = k.v(0xA0), W = k.v(0xB0);
            const float h0 = k.f[0xD0 / 4], h1 = k.f[0xD4 / 4];
            for (const float su : {-h0, h0})
                for (const float sw : {-h1, h1}) add({A.x + su * U.x + sw * W.x, A.y + su * U.y + sw * W.y, A.z + su * U.z + sw * W.z});
        } else if (k.vt == kDiscVt) {
            const V A = k.v(0x20);
            const float r = std::sqrt(std::max(k.f[0x70 / 4], 0.0f));
            add({A.x - r, A.y, A.z - r}), add({A.x + r, A.y, A.z + r});
        }
    if (hi[0] < lo[0]) return;
    const V h = {(hi[0] - lo[0]) * 0.5f, (hi[1] - lo[1]) * 0.5f, (hi[2] - lo[2]) * 0.5f};
    rc.c = {lo[0] + h.x, lo[1] + h.y, lo[2] + h.z}, rc.r2 = dot(h, h);
}
// The fields of a record the tests read, as one key: its centre and squared radii, each blocker's vtable, two-plane flag and the
// fields its transmission reads (RoomGeoKey's blockers part). Never the raw bytes: they hold the game's own state (the gather
// AddRefs each shape) and the vectors' addresses
inline uint64_t RecordKey(const Record& rc) {
    uint64_t h = 1469598103934665603ull;
    const auto mix = [&](const void* src, size_t n) {
        for (size_t i = 0; i < n; i++) h = (h ^ static_cast<const uint8_t*>(src)[i]) * 1099511628211ull;
    };
    const auto mixAt = [&](const float* f, int off, int n) { mix(f + off / 4, static_cast<size_t>(n) * 4); };
    mix(&rc.c, sizeof rc.c), mix(&rc.r2, 4), mix(&rc.cull2, 4);
    const uint32_t nb = static_cast<uint32_t>(rc.b.size());
    mix(&nb, 4);
    for (const Blocker& k : rc.b) {
        mix(&k.vt, 4);
        const uint8_t two = k.f[4] != 0.0f; // (+0x10: any non-zero value selects the two-plane form)
        if (k.vt == kQuadVt) {
            mix(&two, 1);
            for (const int off : {0x70, 0x80, 0x90, 0xA0, 0xB0, 0xE0, 0xF0}) mixAt(k.f, off, 3);
            mixAt(k.f, 0xD0, 4);
        } else if (k.vt == kDiscVt) {
            mix(&two, 1);
            for (const int off : {0x20, 0x30, 0x40, 0x80, 0x90}) mixAt(k.f, off, 3);
            mixAt(k.f, 0x70, 1);
        } else
            mix(k.f, sizeof k.f); // (a kind the copy does not know: all of what it may read)
    }
    return h;
}
// (OutdoorDelta's live tables) the kept records whose blockers now read otherwise. read(address, blocker): the blocker there, false when
// it cannot be read; an old copy stands for one unreadable, of another vtable (its memory is someone else's now) or of a kind RecordKey
// hashes whole (its raw bytes change with no move). Returns their key, 0: none moved
struct LiveMove {
    size_t rec;
    std::vector<Blocker> b;
};
template <typename Read> uint64_t LiveMoves(const std::vector<Record>& kept, Read&& read, std::vector<LiveMove>& moved) {
    Record now;
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < kept.size(); i++) {
        const Record& kr = kept[i];
        if (kr.at.size() != kr.b.size()) continue;
        now.c = kr.c, now.r2 = kr.r2, now.cull2 = kr.cull2, now.b = kr.b;
        for (size_t j = 0; j < kr.b.size(); j++) {
            Blocker t;
            if (read(kr.at[j], t) && t.vt == kr.b[j].vt && (t.vt == kQuadVt || t.vt == kDiscVt)) now.b[j] = t;
        }
        const uint64_t rk = RecordKey(now);
        if (rk == RecordKey(kr)) continue;
        const uint64_t both[2] = {rk, i};
        for (size_t b = 0; b < sizeof both; b++) h = (h ^ reinterpret_cast<const uint8_t*>(both)[b]) * 1099511628211ull;
        moved.push_back({i, now.b});
    }
    return moved.empty() ? 0 : h | 1;
}
// When a kept room's blockers are read again: a read of all rooms is paced (gen, 100 ms), a room with a record off its solve's place follows every
// frame an object moved (step). At 100 ms a table dragged with its lamp stood up to 0.9 m behind the lamp, its floor shadow swung by 2.3 times
// that (the lamp's height over the floor against over the top), and its fill flipped off past the top's edge (23-38-26 recording of 05/11)
inline bool LiveDue(uint32_t gen, uint32_t readGen, uint32_t step, uint32_t readStep, uint64_t key) { return gen != readGen || (key && step != readStep); }
// A lamp at `lamp` standing on the blocker: a level top (a disc or rectangle facing up or down: a table's, not a sofa's back), the
// lamp over it inside its edge and at most kOnTopM above it (a ceiling lamp or sconce over the furniture stands higher). Not the
// record's sphere: the sofa's (r 1.69 m, three upright quads) held the table lamp 1.2 m off it, and its shadow was carried along
// (03/11 13:56 scene). UNVERIFIED: kOnTopM against taller table lamps (this one's head is 0.57 m over its top) and low chandeliers
constexpr float kOnTopM = 1.0f;
// (a drawn copy only: RtLampGpu::DrawnBlockers) +0xF0 x of a seat's cap. A one-plane rectangle never reads it, BlockerAt gives it back
constexpr float kCapMark = -9999.5f;
inline bool StandsOn(const Blocker& k, const float lamp[3]) {
    const bool disc = k.vt == kDiscVt;
    if ((!disc && k.vt != kQuadVt) || (!disc && k.f[0xF0 / 4] == kCapMark)) return false; // (nothing stands on a cap: it fills no shadow, carries no record)
    const V c = k.v(disc ? 0x20 : 0x70), d{lamp[0] - c.x, lamp[1] - c.y, lamp[2] - c.z};
    if (std::fabs(k.v(disc ? 0x40 : 0x90).y) < 0.95f || !(d.y > 0.0f && d.y < kOnTopM)) return false;
    if (disc) return d.x * d.x + d.z * d.z < k.f[0x70 / 4];
    return std::fabs(dot(d, k.v(0xA0))) < k.f[0xD0 / 4] && std::fabs(dot(d, k.v(0xB0))) < k.f[0xD4 / 4];
}
inline bool StandsOn(const Record& rc, const float lamp[3]) {
    return std::any_of(rc.b.begin(), rc.b.end(), [&](const Blocker& k) { return StandsOn(k, lamp); });
}
struct Wall {
    float f[64];
    uint32_t u(int off) const {
        uint32_t x;
        std::memcpy(&x, &f[off / 4], 4);
        return x;
    }
    V v(int off) const { return {f[off / 4], f[off / 4 + 1], f[off / 4 + 2]}; }
};
struct Room {
    std::vector<Record> rec;
    std::vector<Wall> walls;
    std::vector<int> idx2d;
    int mode = 0;
};

// 0x0069B4C0: plane (A, N) vs segment B->C with direction D and invDen = 1/(D.N).
// out = X - A is written as soon as t >= 0, even if X lies beyond C (game quirk).
inline bool segPlane(V A, V N, V B, V C, V D, float invDen, V* out) {
    float t = (0.0f - dot(sub(B, A), N)) * invDen;
    if (0.0f > t) return false;
    V X = {D.x * t + B.x, D.y * t + B.y, D.z * t + B.z};
    *out = sub(X, A);
    return !(0.0f > dot(sub(C, X), D));
}
inline bool invDenOf(V p1, V p2, V N, V* d, float* inv) {
    *d = sub(p2, p1);
    float dn = dot(N, *d);
    float a = std::max(K_NEG1 * dn, dn);
    if (K_EPS > a) return false; // segment parallel to the plane(s)
    float r = rcp(dn);
    *inv = (K_TWO - r * dn) * r; // one Newton step
    return true;
}
// 0x0069B590: one plane
inline bool hit1(V p1, V p2, V N, V A, V* out) {
    V d;
    float inv;
    if (!invDenOf(p1, p2, N, &d, &inv)) return false;
    return segPlane(A, N, p1, p2, d, inv, out);
}
// 0x0069B650: two parallel planes sharing N
inline bool hit2(V p1, V p2, V N, V A1, V A2, V* o1, V* o2) {
    V d;
    float inv;
    if (!invDenOf(p1, p2, N, &d, &inv)) return false;
    bool h1 = segPlane(A1, N, p1, p2, d, inv, o1);
    bool h2 = segPlane(A2, N, p1, p2, d, inv, o2);
    return h1 || h2;
}
// 0x0069BEA0 (vtable 0x00FF3118): soft-edged rectangle(s). +0x70/+0x80 plane points, +0x90 normal, +0xA0/+0xB0 in-plane
// axes U,V, +0xD0 half extents (hu,hv,hu,hv), +0xE0/+0xF0 default "miss" offsets, +0x10 != 0 selects the two-plane form.
inline float quadT(const Blocker& k, V p1, V p2) {
    V U = k.v(0xA0), Vv = k.v(0xB0);
    const float* h = &k.f[0xD0 / 4];
    auto op = [&](float c, float hh) {
        float a = (std::max(K_NEG1 * c, c) - hh + K_015) * K_TWO;
        return clamp01(K_ONE - a);
    };
    if (k.f[4] != K_ZERO) {
        V v1 = k.v(0xE0), v2 = k.v(0xF0);
        if (!hit2(p1, p2, k.v(0x90), k.v(0x70), k.v(0x80), &v1, &v2)) return 1.0f;
        float s0 = op(dot(U, v1), h[0]), s1 = op(dot(Vv, v1), h[1]);
        float s2 = op(dot(U, v2), h[2]), s3 = op(dot(Vv, v2), h[3]);
        return (K_ONE - s3 * s2) * (K_ONE - s1 * s0);
    }
    V v;
    if (!hit1(p1, p2, k.v(0x90), k.v(0x70), &v)) return 1.0f;
    float s0 = op(dot(U, v), h[0]), s1 = op(dot(Vv, v), h[1]);
    return K_ONE - s1 * s0;
}
// 0x0069C1A0 (vtable 0x00FF3C24): soft-edged disc(s). +0x20/+0x30 centres, +0x40 normal, +0x70 radius^2, +0x80/+0x90 default
// "miss" offsets, +0x10 != 0 selects two discs.
inline float discT(const Blocker& k, V p1, V p2) {
    float R = k.f[0x70 / 4];
    auto tr = [&](V v) { return std::min(std::max(0.0f, ((dot(v, v) - R) + K_00625) * K_FOUR), K_ONE); };
    if (k.f[4] != K_ZERO) {
        V v1 = k.v(0x80), v2 = k.v(0x90);
        if (!hit2(p1, p2, k.v(0x40), k.v(0x20), k.v(0x30), &v1, &v2)) return 1.0f;
        return tr(v2) * tr(v1);
    }
    V v;
    if (!hit1(p1, p2, k.v(0x40), k.v(0x20), &v)) return 1.0f;
    return tr(v);
}
// 0x0069D910 with the cache path off (0x0069FCE0 clears it): product of blocker transmissions. A blocker of another vtable
// counts in *unknownVt (its share is taken as 1: not the game's answer)
inline float occlude(const Room& r, V a, V b, int* unknownVt) {
    V d = sub(b, a);
    float len2 = dot(d, d);
    V dn = {d.x / len2, d.y / len2, d.z / len2};
    float T = 1.0f;
    for (const Record& rc : r.rec) {
        V ca = sub(rc.c, a);
        float s = dot(ca, d);
        V q = sub({s * dn.x, s * dn.y, s * dn.z}, ca);
        if (!(dot(q, q) - rc.r2 < 0.0f)) continue; // broad phase: infinite line vs sphere
        const V ra = {a.x - rc.off[0], a.y, a.z - rc.off[1]}, rb = {b.x - rc.off[0], b.y, b.z - rc.off[1]};
        for (const Blocker& k : rc.b) {
            float t;
            if (k.vt == kQuadVt) t = quadT(k, ra, rb);
            else if (k.vt == kDiscVt) t = discT(k, ra, rb);
            else {
                ++*unknownVt;
                t = 1.0f;
            }
            T = T * t;
            if (K_STOP >= T) return T;
        }
    }
    return T;
}

// 0x0069AA90 thiscall(wall, m, a, b, float* s): ray a->b against wall segment set m (x/z plane).
// m: 0 hard (+0x40 P, +0x60 E), 1 soft (+0x70, +0x90), 2 cull (+0xA0, +0xC0). *s is written before any check.
inline bool wallHit(const Wall& w, int m, V a, V b, float* s) {
    const int base = 0x40 + m * 0x30;
    V P = w.v(base), E = w.v(base + 0x20);
    V D = sub(b, a), W = sub(a, P);
    float den = E.x * D.z - E.z * D.x;
    float n1 = E.z * W.x - E.x * W.z;
    float n2 = W.z * D.x - W.x * D.z;
    if (den == 0.0f) return false;
    float r = rcp(den);
    float k = (K_TWO - r * den) * r;
    float t = k * n1, ss = k * n2;
    *s = ss;
    // [00FF30A0] negates lanes 2,3; [011D0190] adds {0,0,1,1}; movmskps = any sign bit
    bool outside = std::signbit(t) || std::signbit(ss) || std::signbit(k * -n1 + 1.0f) || std::signbit(k * -n2 + 1.0f);
    if (m != 0 && (w.u(0xEC) & 2)) {
        if (outside) return false;
        // height test: crossing of plane (P, N) with W.y replaced by [01158A50]; hit while below P.y + 1
        V N = w.v(0xD0);
        V Wy = {W.x, K_WALL_Y, W.z};
        float den2 = dot(N, D);
        float r2 = rcp(den2);
        float k2 = (K_TWO - den2 * r2) * r2;
        float q = k2 * (0.0f - dot(N, Wy));
        float h = (q * D.y + a.y) - P.y;
        return K_ONE > h;
    }
    return !outside;
}
// 0x0069A8C0: soft penumbra near the wall's ends; +0xE0 = wall length
inline float wallPenumbra(const Wall& w, float s) {
    float u = std::fabs(s - K_PEN_C);
    float v = (K_PEN_C - u) * w.f[0xE0 / 4] / K_HALF;
    float mm = std::signbit(v - K_ONE) ? v : K_ONE;
    return K_ONE - mm;
}
// 0x0069FC40 -> 0x0069D4C0 (skip +0xEC bit 0, out = null). Returns ok; *t = transmission as the game leaves it.
inline bool wallTest(const Room& r, const std::vector<int>& list, V a, V b, float* tOut) {
    float T = 1.0f;
    unsigned count = 0, maxJ = 0;
    bool hit = false, early = false;
    for (int i : list) {
        if (i < 0 || i >= (int)r.walls.size()) continue;
        const Wall& w = r.walls[i];
        if (w.u(0xEC) & 1) continue;
        float s;
        if (!wallHit(w, r.mode ? 1 : 0, a, b, &s)) continue;
        float x = r.mode ? wallPenumbra(w, s) : 0.0f;
        hit = true;
        if (x > K_STOP) {
            ++count;
            maxJ = std::max(maxJ, w.u(0.5f > s ? 0xE4 : 0xE8));
        }
        T = x > T ? T : x;
        if (K_STOP > x) {
            early = true;
            break;
        }
    }
    if (!early && maxJ > count) T = 0.0f;
    float t = 1.0f;
    if (hit) t *= T;
    *tOut = t;
    return !(K_DROP > t);
}
// 0x006A30B0 -> 0x0069DFF0: walls whose cull segment (m = 2) the batch-centroid -> light ray hits
inline std::vector<int> wallCull(const Room& r, V C, V Lp) {
    std::vector<int> out;
    for (int i = 0; i < (int)r.walls.size(); ++i) {
        float s;
        if (wallHit(r.walls[i], 2, C, Lp, &s)) out.push_back(i);
    }
    return out;
}
// The share of a lamp at L the game's two tests leave a texel at p (the part over the colour its evaluation left): the 2D wall
// test from `from` with the batch's list (built = the solve built the per-light lists: a batch of 4 samples or more with a
// test on; list = the walls wallCull gave, else the room's default list), when flags[0] is set; then the 3D test from L when
// flags[1] is set, the product dropped under K_DROP
inline float GameShare(const Room& r, bool built, const std::vector<int>& list, bool f0, bool f1, V L, V from, V p, int* unknownVt) {
    float share = 1.0f;
    if (f0) {
        float t = 1.0f;
        share = wallTest(r, built ? list : r.idx2d, from, p, &t) ? t : 0.0f;
    }
    if (share > 0.0f && f1) {
        const float prod = share * occlude(r, L, p, unknownVt);
        share = K_DROP > prod ? 0.0f : prod;
    }
    return share;
}

} // namespace RoomLightTests
