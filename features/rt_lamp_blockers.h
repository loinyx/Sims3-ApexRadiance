#pragma once
// GPU room lamps, physical model (docs/features/night-lighting/gpu-room-lamps.md): a receiving room's furniture blockers (the
// game's 3D test, room_light_tests.h) packed as float4 rows for an A32B32G32R32F texture read with point filtering in ps_3_0,
// row i at texel (i % kMaxWidth, i / kMaxWidth). Every value is the game's float as read (the room ids and
// counts are integers below 2^24, exact as floats). Pure: no game access, no device; the decoder here walks the rows as the
// shader will and gives RoomLightTests::occlude's answer bit for bit (tools/outdoor_delta_test, tools/room_lamp_3d_replay).
//
// Append(room) adds, from its returned header row:
//   header (first record row, records, its first record's index in `shapes`, 1 / kCutFadeM as FadeCut sets it, else 0: the game's step)
//   each record, 2 rows: (centre.xyz, r^2), (first blocker row, blockers, its offset dx, dz: 0 as read, Offset)
//   each blocker, kBlockerRows rows (rectangle 0x00FF3118 | disc 0x00FF3C24):
//     0 (A1 +0x70 | c1 +0x20 .xyz, kind: 0 rectangle, 1 disc)
//     1 (A2 +0x80 | c2 +0x30 .xyz, +0x10 as read: != 0 selects the two-plane form)
//     2 (N +0x90 | N +0x40 .xyz, h0 +0xD0 | R^2 +0x70)
//     3 (U +0xA0 .xyz, h1 +0xD4 | 0)
//     4 (V +0xB0 .xyz, h2 +0xD8 | 0)
//     5 (miss 1 +0xE0 | +0x80 .xyz, h3 +0xDC | 0)
//     6 (miss 2 +0xF0 | +0x90 .xyz, its record's sphere r^2 as the game's, whatever posts made of the record's row)
//   then each of the record's posts (Record::posts), the same stride, after its blockers:
//     0 (one end .xyz, kind: 2), 1 (other end .xyz, 0), 2 (0, 0, 0, radius), the rest 0
// The "miss" offsets are what the two-plane form leaves for a plane its segment never reaches (hit2), read as the game does.
// A blocker of another vtable is left out: its share is 1 (occlude counts it and multiplies by 1), so T and the 0.01 stop
// are unchanged.
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <vector>
#include "room_light_tests.h"
#include "rt_lamp_proxy.h"

namespace RtLampGpu {

// The physical model's 3D test (furniture) as AccPhysPS and AccPhysXPS take it: t lifted to at least `fill`, the lamp's room
// table column w (BlockerTable::FillOf). A lamp standing on furniture keeps kFurnitureFill of its light under it (the game's
// point-light test left the floor under a lamp's own table near black in a side-table capture); every other lamp 0, its
// furniture shadows the game's. UNVERIFIED in game: an estimate (floor albedo about 0.3 x about half the sky open under a table),
// at the shadow's middle, kFillGlow more towards its edge (BlockerTable::OccludeFill: a flat fill looked a grey plate); the
// directional maps keep the game's dark (BasisPartPhys). tools/rt_lamps_test checks the shader against this
constexpr float kFurnitureFill = 0.10f, kFillGlow = 0.2f, kFillGlowR = 4.0f;
inline float FurnitureFill(float t, float fill) { return fill + (1.0f - fill) * t; }

// (physical, true-size furniture shadows: Params::edgeR, and every lamp with a furniture fill) a blocker's edge at its real extent h, as wide as a lamp ball of radius
// edgeR seen from the receiver p2 past the blocker's plane point X = A + v (0 at contact, edgeR at the lamp). The game's band
// starts 0.15 m inside the extent and fades 0.35 m outside it at any distance, so a small table casts a round blob 2-3x its size
// on the floor. The shader's EdgeW / EdgeOp are the same operations
inline float EdgeW(RoomLightTests::V A, RoomLightTests::V v, RoomLightTests::V p1, RoomLightTests::V p2, float edgeR) {
    using namespace RoomLightTests;
    const V x = {A.x + v.x, A.y + v.y, A.z + v.z}, d = sub(p2, p1), r = sub(p2, x);
    const float f = std::sqrt(dot(r, r) / std::max(dot(d, d), 1e-8f));
    return std::max(edgeR * clamp01(f), 1e-3f);
}
// (sphere_cut_fade) The game drops a record the moment the line leaves its sphere, about 0.13 m past a chair's top where the 0.35 m band
// still has 0.45 of its opacity: the shadow stops at a step along the sphere's silhouette, a crisp round arc on the floor (0.73
// of T in 3 cm). The band, never a true-size edge (no step there), fades over the sphere's last kCutFadeM instead.
// The core of a shadow is unchanged: the map stops equalling the game's solve only in that zone. UNVERIFIED in game
constexpr float kCutFadeM = 0.12f;
inline float CutFade(float t, float r2, float q2, float invW) {
    const float c = RoomLightTests::clamp01((std::sqrt(r2) - std::sqrt(q2)) * invW);
    return 1.0f - (1.0f - t) * (c * c * (3.0f - 2.0f * c));
}
inline float EdgeOp(float c, float h, float w) { // (an S: a straight ramp leaves creases on the floor at both its ends)
    const float t = RoomLightTests::clamp01((h - std::fabs(c)) / (2.0f * w) + 0.5f);
    return t * t * (3.0f - 2.0f * t);
}
// (physical) a table's leg, a capsule (Record::posts): 5 cm is under a floor texel, so it casts a soft streak, not an edge. The closest
// point of the segment p1 -> p2 to the capsule, there the width of a lamp ball of radius lampR seen past it (as EdgeW) and never under
// half a texel seen from the lamp (without it the streak dots). A leg thinner than that width shades by their ratio. A lamp kPostHighM
// over its top starts to fade (a smoothstep, gone at kPostGateM) and its streak widens by up to kPostHighW at the floor: under a ceiling
// lamp a leg adds a faint soft foot to the furniture's soft shadow, never a hard streak (before: no foot shadow at all under
// a lamp above). The widest edge, 1.75 x (kPostFloorW + kPostHighW), stays within kPostReach. UNVERIFIED in game: the heights and the
// streak's strength (offline against the shadow mesh: 0.52 mean light where the mesh gives 0.55)
constexpr float kPostHighM = 1.5f, kPostGateM = 3.0f, kPostFloorW = 0.075f, kPostHighW = 0.125f, kPostReach = 0.35f; // kPostReach: past a leg's radius, the sphere that must still reach it
inline float PostT(const RoomLightTests::Blocker& k, RoomLightTests::V p1, RoomLightTests::V p2, float lampR) {
    using namespace RoomLightTests;
    const V A = k.v(0x20), B = k.v(0x30);
    const float r = k.f[0x70 / 4];
    const float hi = clamp01((p1.y - std::max(A.y, B.y) - kPostHighM) / (kPostGateM - kPostHighM));
    if (hi >= 1.0f) return 1.0f;
    const V d1 = sub(p2, p1), d2 = sub(B, A), o = sub(p1, A);
    const float a = dot(d1, d1), e = dot(d2, d2), f = dot(d2, o), c = dot(d1, o), b = dot(d1, d2), den = a * e - b * b;
    float s = den > 1e-12f ? clamp01((b * f - c * e) / den) : 0.0f;
    const float u = (b * s + f) / std::max(e, 1e-12f), t = clamp01(u);
    if (t != u) s = clamp01((b * t - c) / a);
    const V g = sub({p1.x + d1.x * s, p1.y + d1.y * s, p1.z + d1.z * s}, {A.x + d2.x * t, A.y + d2.y * t, A.z + d2.z * t});
    const float d = std::sqrt(dot(g, g)), w = std::max({lampR * (1.0f - s), (kPostFloorW + kPostHighW * hi) * s, 1e-3f});
    return 1.0f - (1.0f - hi * hi * (3.0f - 2.0f * hi)) * (r >= w ? EdgeOp(d, r, w) : r / w * EdgeOp(d, w, w * 0.75f));
}
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

// (physical, chair_arm_caps) A box-type chair or sofa's LITE has its arms as zero-thickness sheets at the box's full height, a slant for
// the seat and back. Seen almost edge-on from a lamp beside it, the slant passes the light and the sheets' 0.35 m band alone draws a thin
// streak far past the shadow (a separate spike 2.0 to 3.2 m from the lamp, none on the mesh's shadow). Only a
// record of exactly that shape changes: one slant, one back, arms whose base and top are the back's within 2 cm, all of one depth; 22 of
// the 72 slant records of that scene's 28 solves, the arms a real model stepped are left alone. Its arms stand kArmHeight of the box tall
// and a level cap at their top (kCapMark) holds the mass the sheets were standing for. On a copy: the 3D test as read stays what
// CarryStale, the directional maps and RecordKey read, so the map no longer equals the game's on these records (the seat's shadow is
// closer to the mesh: summed error 32 to 28 m2 over 5 objects at this lamp, 92 to 63 from a side lamp, level for a high ceiling lamp).
// UNVERIFIED in game: kArmHeight is tuned, the real arm is about 0.61 of the box, the band adds about 0.1 m at its top
constexpr float kArmHeight = 0.5f;
inline void DrawnBlockers(std::vector<RoomLightTests::Record>& records) {
    using namespace RoomLightTests;
    const auto flat = [](V a, V b) { return std::fabs(a.x * b.x + a.z * b.z); };
    for (Record& rc : records) {
        const Blocker* slant = nullptr;
        int slants = 0, backs = 0;
        for (const Blocker& k : rc.b)
            if (const float ny = std::fabs(k.v(0x90).y); k.vt == kQuadVt && ny > 0.2f && ny < 0.95f) slant = &k, slants++;
        if (slants != 1) continue;
        const bool uFlat = std::fabs(slant->v(0xA0).y) < 0.05f;
        const V w = slant->v(uFlat ? 0xA0 : 0xB0); // (the seat's width)
        const float halfW = slant->f[(uFlat ? 0xD0 : 0xD4) / 4];
        std::vector<Blocker*> arms;
        const Blocker* back = nullptr;
        bool ok = true;
        const auto upright = [](const Blocker& k) { return std::fabs(k.v(0xA0).y) > 0.9f ? 0 : std::fabs(k.v(0xB0).y) > 0.9f ? 1 : -1; }; // (which half extent is its height)
        for (Blocker& k : rc.b) {
            if (k.vt != kQuadVt || std::fabs(k.v(0x90).y) >= 0.05f || upright(k) < 0) continue;
            const float along = flat(k.v(0x90), w);
            if (along > 0.9f) arms.push_back(&k);
            else if (along < 0.1f) back = &k, backs++;
            else ok = false;
        }
        if (!ok || arms.empty() || backs != 1) continue;
        const auto halfUp = [&](const Blocker& k) { return k.f[(upright(k) ? 0xD4 : 0xD0) / 4]; };
        const auto halfDeep = [&](const Blocker& k) { return k.f[(upright(k) ? 0xD0 : 0xD4) / 4]; };
        const float base = back->f[0x70 / 4 + 1] - halfUp(*back), top = back->f[0x70 / 4 + 1] + halfUp(*back);
        for (const Blocker* a : arms) {
            const float y = a->f[0x70 / 4 + 1], h = halfUp(*a);
            ok = ok && std::fabs(y - h - base) <= 0.02f && std::fabs(y + h - top) <= 0.02f && std::fabs(halfDeep(*a) - halfDeep(*arms[0])) < 0.01f;
        }
        if (!ok) continue;
        const float deep = halfDeep(*arms[0]);
        const V across = arms[0]->v(upright(*arms[0]) ? 0xA0 : 0xB0);
        float cx = 0.0f, cz = 0.0f, planes = 0.0f, capY = 0.0f;
        for (Blocker* a : arms) {
            const float h = halfUp(*a), lower = h * (1.0f - kArmHeight);
            const int v = upright(*a);
            a->f[(v ? 0xD4 : 0xD0) / 4] = a->f[(v ? 0xDC : 0xD8) / 4] = h * kArmHeight;
            a->f[0x70 / 4 + 1] -= lower, a->f[0x80 / 4 + 1] -= lower;
            capY = a->f[0x70 / 4 + 1] + h * kArmHeight;
            cx += a->f[0x70 / 4], cz += a->f[0x70 / 4 + 2], planes += 1.0f;
            if (a->f[4] != K_ZERO) cx += a->f[0x80 / 4], cz += a->f[0x80 / 4 + 2], planes += 1.0f;
        }
        Blocker cap{};
        cap.vt = kQuadVt;
        const auto put = [&](int off, float x, float y, float z) { cap.f[off / 4] = x, cap.f[off / 4 + 1] = y, cap.f[off / 4 + 2] = z; };
        put(0x70, cx / planes, capY, cz / planes), put(0x80, cx / planes, capY, cz / planes), put(0x90, 0.0f, 1.0f, 0.0f);
        put(0xA0, w.x, 0.0f, w.z), put(0xB0, across.x, 0.0f, across.z), put(0xF0, kCapMark, 0.0f, 0.0f);
        cap.f[0xD0 / 4] = cap.f[0xD8 / 4] = halfW, cap.f[0xD4 / 4] = cap.f[0xDC / 4] = deep;
        rc.b.push_back(cap); // (after `slant` and `arms` are done with: this may move them)
    }
}

struct BlockerTable {
    static constexpr int kMaxWidth = 4096;
    static constexpr int kBlockerRows = 7;
    struct Row {
        float v[4];
    };
    std::vector<Row> rows;
    std::vector<uintptr_t> shapes; // each record's Record::shape in Append's order (a room's first at its header row's z)
    int skipped = 0;               // blockers of another vtable left out

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
        rows.push_back({{static_cast<float>(firstRec), static_cast<float>(room.rec.size()), static_cast<float>(shapes.size()), 0.0f}});
        rows.resize(rows.size() + 2 * room.rec.size());
        for (size_t r = 0; r < room.rec.size(); r++) {
            const Record& rc = room.rec[r];
            shapes.push_back(rc.shape);
            const int first = static_cast<int>(rows.size());
            int n = 0;
            // (posts: the sphere reaches its feet, and its blockers keep the game's: a line it let pass was never tested against them)
            float r2 = rc.r2;
            for (const Post& p : rc.posts)
                for (const V e : {p.a, p.b}) {
                    const V d = sub(e, rc.c);
                    const float reach = std::sqrt(dot(d, d)) + p.r + kPostReach;
                    r2 = std::max(r2, reach * reach);
                }
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
                    put(0x70, 0.0f), put(0x80, k.f[4]), put(0x90, h[0]), put(0xA0, h[1]), put(0xB0, h[2]), put(0xE0, h[3]), put(0xF0, rc.r2);
                } else {
                    put(0x20, 1.0f), put(0x30, k.f[4]), put(0x40, k.f[0x70 / 4]);
                    rows.push_back({}), rows.push_back({});
                    put(0x80, 0.0f), put(0x90, rc.r2);
                }
                n++;
            }
            for (const Post& p : rc.posts) {
                rows.push_back({{p.a.x, p.a.y, p.a.z, 2.0f}}), rows.push_back({{p.b.x, p.b.y, p.b.z, 0.0f}}), rows.push_back({{0.0f, 0.0f, 0.0f, p.r}});
                rows.resize(rows.size() + kBlockerRows - 3);
                n++;
            }
            rows[firstRec + 2 * r] = {{rc.c.x, rc.c.y, rc.c.z, r2}};
            rows[firstRec + 2 * r + 1] = {{static_cast<float>(first), static_cast<float>(n), 0.0f, 0.0f}};
        }
        return header;
    }

    // (physical, cross_room_shadows) a copy of own's room with the records of `others` after its own. Each room tested only its own
    // furniture, so a shadow stopped dead at an arch (a 7.6x step, 1.3x with both rooms' records; offline). Row 0 is own's header,
    // as Append leaves it: its own lamps walk what they walked, the soft furniture shadows of ceiling lamps bit for bit. Row 1, the
    // same first record, counts all of them: a lamp of another room (T3dDensePS). Without that gate a sofa behind a wall would shade
    // the next room: the game's soft band reaches 0.35 m past an object. Records are copied with their first blocker rows moved
    struct Part {
        const BlockerTable* t;
        int header;
        int Recs() const { return t && header >= 0 ? static_cast<int>(t->rows[static_cast<size_t>(header)].v[1]) : 0; }
        float Fade() const { return t && header >= 0 ? t->rows[static_cast<size_t>(header)].v[3] : 0.0f; }
    };
    // (cross-story furniture shadows) homes: up to 2 rooms of other stories whose lamps light this room (a lamp of story 2
    // reaching story 1's wall top through a stairwell). Each room tested only its own story's furniture, so a dresser's shadow on the
    // upper wall stopped dead at the floor line. Row 2 + i counts own's and the arch rooms' records plus home i's:
    // the records are laid out [homes[0]][own][others][homes[1]], so each row's are contiguous. With no homes the table is as before.
    static BlockerTable Join(const Part& own, const std::vector<Part>& others, const std::vector<Part>& homes = {}) {
        BlockerTable o;
        const size_t H = 2 + std::min<size_t>(homes.size(), 2);
        const int nA = homes.size() > 0 ? homes[0].Recs() : 0, nB = homes.size() > 1 ? homes[1].Recs() : 0;
        int total = own.Recs();
        float fade = own.Fade();
        for (const Part& p : others) total += p.Recs(), fade = std::max(fade, p.Fade());
        const float first = static_cast<float>(H + 2 * static_cast<size_t>(nA)), fadeAll = std::max({fade, nA ? homes[0].Fade() : 0.0f, nB ? homes[1].Fade() : 0.0f});
        o.rows.push_back({{first, static_cast<float>(own.Recs()), static_cast<float>(nA), fade}});
        o.rows.push_back({{first, static_cast<float>(total), static_cast<float>(nA), fade}});
        if (H > 2) o.rows.push_back({{static_cast<float>(H), static_cast<float>(nA + total), 0.0f, fadeAll}});
        if (H > 3) o.rows.push_back({{first, static_cast<float>(total + nB), static_cast<float>(nA), fadeAll}});
        o.rows.resize(H + 2 * static_cast<size_t>(nA + total + nB));
        int at = 0;
        const auto add = [&](const Part& p) {
            for (int r = 0; r < p.Recs(); r++, at++) {
                const size_t first = static_cast<size_t>(p.t->rows[static_cast<size_t>(p.header)].v[0]) + 2 * static_cast<size_t>(r);
                Row ix = p.t->rows[first + 1];
                const size_t from = static_cast<size_t>(ix.v[0]), n = static_cast<size_t>(ix.v[1]) * kBlockerRows;
                o.rows[H + 2 * static_cast<size_t>(at)] = p.t->rows[first];
                ix.v[0] = static_cast<float>(o.rows.size());
                o.rows[H + 1 + 2 * static_cast<size_t>(at)] = ix;
                o.rows.insert(o.rows.end(), p.t->rows.begin() + static_cast<std::ptrdiff_t>(from), p.t->rows.begin() + static_cast<std::ptrdiff_t>(from + n));
                o.shapes.push_back(p.t->ShapeOf(p.header, r));
            }
        };
        if (nA) add(homes[0]);
        add(own);
        for (const Part& p : others) add(p);
        if (nB) add(homes[1]);
        return o;
    }
    // (glass_share) the lot's glazed spans after the rooms, 2 rows each (T3dDensePS PaneCount): their first row
    int AppendPanes(const std::vector<RtLampProxy::Pane>& panes) {
        const int first = static_cast<int>(rows.size());
        for (const RtLampProxy::Pane& p : panes) rows.push_back({{p.x0, p.z0, p.x1, p.z1}}), rows.push_back({{p.y0, p.y1, 0.0f, 0.0f}});
        return first;
    }

    // (sphere_cut_fade) the room at `header` ends its bands with CutFade; the tables the rest compares with the game keep the step
    void FadeCut(int header) { rows[static_cast<size_t>(header)].v[3] = 1.0f / kCutFadeM; }

    // (physical) record r of the room at `header` drawn moved by (dx, 0, dz) from where its solve read it: a table dragged with
    // the lamp standing on it, ahead of the solve that reads it again. Its sphere moves; its index row carries the offset (z, w;
    // Append writes 0), and the blocker tests take the segment moved back. Once per record of a fresh copy
    void Offset(int header, int r, float dx, float dz) {
        const int firstRec = static_cast<int>(rows[static_cast<size_t>(header)].v[0]);
        Row& c = rows[static_cast<size_t>(firstRec + 2 * r)];
        Row& ix = rows[static_cast<size_t>(firstRec + 2 * r + 1)];
        c.v[0] += dx, c.v[2] += dz;
        ix.v[2] = dx, ix.v[3] = dz;
    }
    // Record r's object's first occluder shape (Record::shape; 0 unknown)
    uintptr_t ShapeOf(int header, int r) const {
        const size_t i = static_cast<size_t>(rows[static_cast<size_t>(header)].v[2]) + static_cast<size_t>(r);
        return i < shapes.size() ? shapes[i] : 0;
    }
    // A lamp at `lamp` (world) stands on a top of record r of the room at `header` where it is drawn (RoomLightTests::StandsOn)
    bool StandsOn(int header, int r, const float lamp[3]) const {
        const Row& ix = rows[static_cast<size_t>(static_cast<int>(rows[static_cast<size_t>(header)].v[0]) + 2 * r + 1)];
        const float at[3] = {lamp[0] - ix.v[2], lamp[1], lamp[2] - ix.v[3]};
        for (int k = 0; k < static_cast<int>(ix.v[1]); k++)
            if (RoomLightTests::StandsOn(BlockerAt(static_cast<int>(ix.v[0]) + k * kBlockerRows), at)) return true;
        return false;
    }
    // FurnitureFill's floor for a lamp at `lamp`: kFurnitureFill when it stands on a record of the room at `header` (its own
    // table), else 0
    float FillOf(int header, const float lamp[3]) const {
        if (header < 0 || static_cast<size_t>(header) >= rows.size()) return 0.0f;
        const int recs = static_cast<int>(rows[static_cast<size_t>(header)].v[1]);
        for (int r = 0; r < recs; r++)
            if (StandsOn(header, r, lamp)) return kFurnitureFill;
        return 0.0f;
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
        } else if (b[0].v[3] == 1.0f) {
            k.vt = RoomLightTests::kDiscVt;
            get(0x20, 0), get(0x30, 1), get(0x40, 2), get(0x80, 5), get(0x90, 6);
            k.f[0x70 / 4] = b[2].v[3];
        } else {
            k.vt = RoomLightTests::kPostVt;
            get(0x20, 0), get(0x30, 1);
            k.f[0x70 / 4] = b[2].v[3];
        }
        return k;
    }

    // (a lamp with a furniture fill, T3dPS) true-size edges, the shade lifted by the same shadow as a lamp ball kFillGlowR wider
    float OccludeFill(int header, RoomLightTests::V a, RoomLightTests::V b, float edgeR, float lampR = 0.0f) const {
        const float t = Occlude(header, a, b, edgeR, lampR);
        return t < 1.0f ? t + (1.0f - t) * kFillGlow * Occlude(header, a, b, edgeR * kFillGlowR, lampR) : t;
    }
    // RoomLightTests::occlude from the rows of the room at `header`, in its order (the shader's walk), a = the lamp; edgeR > 0: true-size
    // edges; lampR: the lamp ball's radius a leg's streak takes (PostT), whatever edgeR
    float Occlude(int header, RoomLightTests::V a, RoomLightTests::V b, float edgeR = 0.0f, float lampR = 0.0f) const {
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
            const float q2 = dot(q, q);
            if (!(q2 - c.v[3] < 0.0f)) continue; // broad phase: infinite line vs sphere
            const int first = static_cast<int>(ix.v[0]), n = static_cast<int>(ix.v[1]);
            const V ra = {a.x - ix.v[2], a.y, a.z - ix.v[3]}, rb = {b.x - ix.v[2], b.y, b.z - ix.v[3]};
            for (int i = 0; i < n; i++) {
                const float cull = rows[static_cast<size_t>(first + i * kBlockerRows + 6)].v[3];
                if (cull > 0.0f && !(q2 - cull < 0.0f)) continue; // (the game's sphere, where posts enlarged the record's)
                const Blocker k = BlockerAt(first + i * kBlockerRows);
                float t = k.vt == kPostVt ? PostT(k, ra, rb, lampR)
                          : edgeR > 0.0f  ? (k.vt == kQuadVt ? QuadEdgeT(k, ra, rb, edgeR) : DiscEdgeT(k, ra, rb, edgeR))
                                          : (k.vt == kQuadVt ? quadT(k, ra, rb) : discT(k, ra, rb));
                if (h.v[3] > 0.0f && k.vt != kPostVt && !(edgeR > 0.0f)) t = CutFade(t, cull, q2, h.v[3]);
                T = T * t;
                if (K_STOP >= T) return T;
            }
        }
        return T;
    }
};

} // namespace RtLampGpu
