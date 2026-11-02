#pragma once
// GPU room lamps, physical model (docs/features/night-lighting/gpu-room-lamps.md): a receiving room's furniture blockers (the
// game's 3D test, room_light_tests.h) packed as float4 rows for an A32B32G32R32F texture read with point filtering in ps_3_0,
// row i at texel (i % Width(), i / Width()), Width() <= kMaxWidth. Every value is the game's float as read (the room ids and
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

struct BlockerTable {
    static constexpr int kMaxWidth = 4096;
    static constexpr int kBlockerRows = 7;
    struct Row {
        float v[4];
    };
    std::vector<Row> rows;
    int skipped = 0; // blockers of another vtable left out

    int Width() const { return rows.empty() ? 1 : static_cast<int>(std::min<size_t>(rows.size(), kMaxWidth)); }
    int Height() const { return rows.empty() ? 1 : static_cast<int>((rows.size() + kMaxWidth - 1) / kMaxWidth); }
    void Texel(int row, int& x, int& y) const { x = row % Width(), y = row / Width(); }

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

    // RoomLightTests::occlude from the rows of the room at `header`, in its order (the shader's walk)
    float Occlude(int header, RoomLightTests::V a, RoomLightTests::V b) const {
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
            for (int i = 0; i < n; i++) {
                const Blocker k = BlockerAt(first + i * kBlockerRows);
                const float t = k.vt == kQuadVt ? quadT(k, a, b) : discT(k, a, b);
                T = T * t;
                if (K_STOP >= T) return T;
            }
        }
        return T;
    }
};

} // namespace RtLampGpu
