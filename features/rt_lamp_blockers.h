#pragma once
// GPU room lamps, physical model (docs/features/night-lighting/gpu-room-lamps.md): a receiving room's furniture blockers (the
// game's 3D test, room_light_tests.h) packed as float4 rows for an A32B32G32R32F texture read with point filtering in ps_3_0,
// row i at texel (i % kMaxWidth, i / kMaxWidth). Every value is the game's float as read (the room ids and
// counts are integers below 2^24, exact as floats). Pure: no game access, no device; the decoder here walks the rows as the
// shader will and gives RoomLightTests::occlude's answer bit for bit (tools/outdoor_delta_test, tools/room_lamp_3d_replay).
//
// Append(room) adds, from its returned header row:
//   header (first record row, records, 0, 0)
//   each record, 2 rows: (centre.xyz, r^2), (first blocker row, blockers, 0, 0)
//   each blocker, kBlockerRows rows (rectangle 0x00FF3118 | disc 0x00FF3C24):
//     0 (A1 +0x70 | c1 +0x20 .xyz, kind: 0 rectangle, 1 disc)
//     1 (A2 +0x80 | c2 +0x30 .xyz, +0x10 as read: != 0 selects the two-plane form)
//     2 (N +0x90 | N +0x40 .xyz, h0 +0xD0 | R^2 +0x70)
//     3 (U +0xA0 .xyz, h1 +0xD4 | 0)
//     4 (V +0xB0 .xyz, h2 +0xD8 | 0)
//     5 (miss 1 +0xE0 | +0x80 .xyz, h3 +0xDC | 0)
//     6 (miss 2 +0xF0 | +0x90 .xyz, 0)
// The "miss" offsets are what the two-plane form leaves for a plane its segment never reaches (hit2), read as the game does.
// A blocker of another vtable is left out: its share is 1 (occlude counts it and multiplies by 1), so T and the 0.01 stop
// are unchanged.
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>
#include "room_light_tests.h"

namespace RtLampGpu {

// The physical model's 3D test (furniture) as AccPhysPS takes it: light bounced off the room keeps 35% under furniture, the
// game's point-light test left the floor under a lamp's own table near black (14:35 capture of 03/11). The shader has the same numbers
inline float FurnitureFill(float t) { return 0.35f + 0.65f * t; }

// (physical, true-size furniture shadows: Params::edgeR) a blocker's edge at its real extent h, as wide as a lamp ball of radius
// edgeR seen from the receiver p2 past the blocker's plane point X = A + v (0 at contact, edgeR at the lamp). The game's band
// starts 0.15 m inside the extent and fades 0.35 m outside it at any distance, so a small table casts a round blob 2-3x its size
// on the floor (13:45 F8 of 03/11). The shader's EdgeW / EdgeOp are the same operations
inline float EdgeW(RoomLightTests::V A, RoomLightTests::V v, RoomLightTests::V p1, RoomLightTests::V p2, float edgeR) {
    using namespace RoomLightTests;
    const V x = {A.x + v.x, A.y + v.y, A.z + v.z}, d = sub(p2, p1), r = sub(p2, x);
    const float f = std::sqrt(dot(r, r) / std::max(dot(d, d), 1e-8f));
    return std::max(edgeR * clamp01(f), 1e-3f);
}
inline float EdgeOp(float c, float h, float w) { return RoomLightTests::clamp01((h - std::fabs(c)) / (2.0f * w) + 0.5f); }
// quadT / discT with true-size edges (the same hits and "miss" offsets as the game's)
inline float QuadEdgeT(const RoomLightTests::Blocker& k, RoomLightTests::V p1, RoomLightTests::V p2, float edgeR) {
    using namespace RoomLightTests;
    const V U = k.v(0xA0), Vv = k.v(0xB0);
    const float* h = &k.f[0xD0 / 4];
    if (k.f[4] != K_ZERO) {
        V v1 = k.v(0xE0), v2 = k.v(0xF0);
        if (!hit2(p1, p2, k.v(0x90), k.v(0x70), k.v(0x80), &v1, &v2)) return 1.0f;
        const float w1 = EdgeW(k.v(0x70), v1, p1, p2, edgeR), w2 = EdgeW(k.v(0x80), v2, p1, p2, edgeR);
        return (1.0f - EdgeOp(dot(Vv, v2), h[3], w2) * EdgeOp(dot(U, v2), h[2], w2)) * (1.0f - EdgeOp(dot(Vv, v1), h[1], w1) * EdgeOp(dot(U, v1), h[0], w1));
    }
    V v;
    if (!hit1(p1, p2, k.v(0x90), k.v(0x70), &v)) return 1.0f;
    const float w = EdgeW(k.v(0x70), v, p1, p2, edgeR);
    return 1.0f - EdgeOp(dot(Vv, v), h[1], w) * EdgeOp(dot(U, v), h[0], w);
}
inline float DiscEdgeT(const RoomLightTests::Blocker& k, RoomLightTests::V p1, RoomLightTests::V p2, float edgeR) {
    using namespace RoomLightTests;
    const float r = std::sqrt(std::max(k.f[0x70 / 4], 0.0f));
    const auto t = [&](V A, V v) { return 1.0f - EdgeOp(std::sqrt(dot(v, v)), r, EdgeW(A, v, p1, p2, edgeR)); };
    if (k.f[4] != K_ZERO) {
        V v1 = k.v(0x80), v2 = k.v(0x90);
        if (!hit2(p1, p2, k.v(0x40), k.v(0x20), k.v(0x30), &v1, &v2)) return 1.0f;
        return t(k.v(0x30), v2) * t(k.v(0x20), v1);
    }
    V v;
    if (!hit1(p1, p2, k.v(0x40), k.v(0x20), &v)) return 1.0f;
    return t(k.v(0x20), v);
}

struct BlockerTable {
    static constexpr int kMaxWidth = 4096;
    static constexpr int kBlockerRows = 7;
    struct Row {
        float v[4];
    };
    std::vector<Row> rows;
    int skipped = 0; // blockers of another vtable left out

    // (the texture is always kMaxWidth wide and only grows in height: one texture serves every room's table, made again only
    // for a taller one)
    int Width() const { return kMaxWidth; }
    int Height() const { return rows.empty() ? 1 : static_cast<int>((rows.size() + kMaxWidth - 1) / kMaxWidth); }
    static int TexHeight(size_t rows, int have) { return std::max({have, 1, static_cast<int>((rows + kMaxWidth - 1) / kMaxWidth)}); }
    void Texel(int row, int& x, int& y) const { x = row % kMaxWidth, y = row / kMaxWidth; }

    int Append(const RoomLightTests::Room& room) {
        using namespace RoomLightTests;
        const int header = static_cast<int>(rows.size());
        const int firstRec = header + 1;
        rows.push_back({{static_cast<float>(firstRec), static_cast<float>(room.rec.size()), 0.0f, 0.0f}});
        rows.resize(rows.size() + 2 * room.rec.size());
        for (size_t r = 0; r < room.rec.size(); r++) {
            const Record& rc = room.rec[r];
            const int first = static_cast<int>(rows.size());
            int n = 0;
            for (const Blocker& k : rc.b) {
                const bool quad = k.vt == kQuadVt;
                if (!quad && k.vt != kDiscVt) {
                    skipped++;
                    continue;
                }
                const auto put = [&](int off, float w) {
                    const V p = k.v(off);
                    rows.push_back({{p.x, p.y, p.z, w}});
                };
                const float* h = &k.f[0xD0 / 4];
                if (quad) {
                    put(0x70, 0.0f), put(0x80, k.f[4]), put(0x90, h[0]), put(0xA0, h[1]), put(0xB0, h[2]), put(0xE0, h[3]), put(0xF0, 0.0f);
                } else {
                    put(0x20, 1.0f), put(0x30, k.f[4]), put(0x40, k.f[0x70 / 4]);
                    rows.push_back({}), rows.push_back({});
                    put(0x80, 0.0f), put(0x90, 0.0f);
                }
                n++;
            }
            rows[firstRec + 2 * r] = {{rc.c.x, rc.c.y, rc.c.z, rc.r2}};
            rows[firstRec + 2 * r + 1] = {{static_cast<float>(first), static_cast<float>(n), 0.0f, 0.0f}};
        }
        return header;
    }

    // (physical) record r of the room at `header` drawn moved by (dx, 0, dz): a piece of furniture dragged with the lamp standing on
    // it, ahead of the solve that reads its records again (video of 03/11: the side table's shadow left behind, on the side away
    // from the drag). Its sphere moves; its record's index row carries the offset (z, w; Append writes 0) for the blocker tests,
    // which test the segment moved back. Once per record of a fresh copy (the centre is moved in place)
    void Offset(int header, int r, float dx, float dz) {
        const int firstRec = static_cast<int>(rows[static_cast<size_t>(header)].v[0]);
        Row& c = rows[static_cast<size_t>(firstRec + 2 * r)];
        Row& ix = rows[static_cast<size_t>(firstRec + 2 * r + 1)];
        c.v[0] += dx, c.v[2] += dz;
        ix.v[2] = dx, ix.v[3] = dz;
    }
    // Record r of the room at `header` left out (its sphere's r^2 < 0: the broad phase of Occlude and Occlude3D never meets it)
    void Drop(int header, int r) { rows[static_cast<size_t>(static_cast<int>(rows[static_cast<size_t>(header)].v[0]) + 2 * r)].v[3] = -1.0f; }
    // The blocker at `row` back in the game's layout (only the fields its test reads; the rest 0)
    RoomLightTests::Blocker BlockerAt(int row) const {
        RoomLightTests::Blocker k{};
        const Row* b = &rows[static_cast<size_t>(row)];
        const auto get = [&](int off, int i) { std::memcpy(&k.f[off / 4], b[i].v, 3 * sizeof(float)); };
        k.f[4] = b[1].v[3];
        if (b[0].v[3] == 0.0f) {
            k.vt = RoomLightTests::kQuadVt;
            get(0x70, 0), get(0x80, 1), get(0x90, 2), get(0xA0, 3), get(0xB0, 4), get(0xE0, 5), get(0xF0, 6);
            k.f[0xD0 / 4] = b[2].v[3], k.f[0xD4 / 4] = b[3].v[3], k.f[0xD8 / 4] = b[4].v[3], k.f[0xDC / 4] = b[5].v[3];
        } else {
            k.vt = RoomLightTests::kDiscVt;
            get(0x20, 0), get(0x30, 1), get(0x40, 2), get(0x80, 5), get(0x90, 6);
            k.f[0x70 / 4] = b[2].v[3];
        }
        return k;
    }

    // RoomLightTests::occlude from the rows of the room at `header`, in its order (the shader's walk); edgeR > 0: true-size edges
    float Occlude(int header, RoomLightTests::V a, RoomLightTests::V b, float edgeR = 0.0f) const {
        using namespace RoomLightTests;
        V d = sub(b, a);
        float len2 = dot(d, d);
        V dn = {d.x / len2, d.y / len2, d.z / len2};
        float T = 1.0f;
        const Row& h = rows[static_cast<size_t>(header)];
        const int firstRec = static_cast<int>(h.v[0]), recs = static_cast<int>(h.v[1]);
        for (int r = 0; r < recs; r++) {
            const Row& c = rows[static_cast<size_t>(firstRec + 2 * r)];
            const Row& ix = rows[static_cast<size_t>(firstRec + 2 * r + 1)];
            V ca = sub({c.v[0], c.v[1], c.v[2]}, a);
            float s = dot(ca, d);
            V q = sub({s * dn.x, s * dn.y, s * dn.z}, ca);
            if (!(dot(q, q) - c.v[3] < 0.0f)) continue; // broad phase: infinite line vs sphere
            const int first = static_cast<int>(ix.v[0]), n = static_cast<int>(ix.v[1]);
            const V ra = {a.x - ix.v[2], a.y, a.z - ix.v[3]}, rb = {b.x - ix.v[2], b.y, b.z - ix.v[3]};
            for (int i = 0; i < n; i++) {
                const Blocker k = BlockerAt(first + i * kBlockerRows);
                const float t = edgeR > 0.0f ? (k.vt == kQuadVt ? QuadEdgeT(k, ra, rb, edgeR) : DiscEdgeT(k, ra, rb, edgeR))
                                             : (k.vt == kQuadVt ? quadT(k, ra, rb) : discT(k, ra, rb));
                T = T * t;
                if (K_STOP >= T) return T;
            }
        }
        return T;
    }
};

} // namespace RtLampGpu
