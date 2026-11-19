#pragma once
// Sim capsules for the per-pixel shadows (real-time GI work package XR-08): where each Sim the game drew this frame stands,
// from the skinned draw's own vertex constants, and three capsules a Sim (legs, torso, head) for the bridge pass.
//
// Source (cheapest that is verified offline, wp/XR-08.md): every skinned vertex shader of the game (dcl_blendindices)
// reads its bone palette at c0[a0] (3 rows a bone, model space) and maps the skinned point to clip space through either
// one 4-register matrix or a 3-register model-to-world matrix and then a 4-register one, both by dp4 (or m4x4 / m4x3).
// Analyse() finds those registers in the shader's bytecode. With the camera's view-projection V of the frame
// (PostScene::CameraViewProj, rows c40..c43) the draw's model-to-world matrix is V^-1 x C, whatever the game's split of
// the chain. A draw of another camera is dropped: the sun's orthographic shadow camera gives no affine matrix, the mirrored
// water pass a mirror, another field of view a shear. A camera with this one's projection elsewhere would pass [I: no
// such pass is known to draw Sims].
// The Sim's root is the median of the palette's translations through that matrix (near the model origin, the feet,
// while the bones are near their bind pose).
// Header only, no Direct3D: tools/gi_server_test/pixel_check.cpp checks it on a synthetic draw and over the game's
// Shaders_Win32.precomp (--precomp).
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>
#include "native/remix-bridge/gi/gi_pixel_block.h"

namespace GiSimCapsules {

// The registers of a skinned draw's chain to the position: f the first of four (clip rows), w the first of three
// (model-to-world rows, applied before f) or -1
struct Chain {
    int f = -1, w = -1;
};

namespace detail {
enum : uint32_t { kTemp = 0, kInput = 1, kConst = 2, kRastOut = 4, kOutput = 6 };
enum : uint32_t { kMov = 1, kDp4 = 9, kM4x4 = 20, kM4x3 = 21, kDcl = 31, kEnd = 0xFFFF, kComment = 0xFFFE };
struct Reg {
    uint32_t type = ~0u, num = 0, mask = 0;
    bool rel = false;
};
inline Reg Param(uint32_t t) {
    Reg r;
    r.type = ((t >> 28) & 7) | ((t >> 8) & 0x18), r.num = t & 0x7FF, r.mask = (t >> 16) & 0xF, r.rel = (t & 0x2000) != 0;
    return r;
}
struct Ins {
    uint32_t op = 0;
    Reg dst, src[3];
    int nsrc = 0;
};
} // namespace detail

// false: not a skinned shader of the game's form (no dcl_blendindices, no palette at c0[a0], or no chain found)
inline bool Analyse(const uint32_t* code, size_t words, Chain& out) {
    using namespace detail;
    if (!code || words < 2 || (code[0] & 0xFFFF0000u) != 0xFFFE0000u || (code[0] & 0xFF00u) < 0x200u) return false;
    std::vector<Ins> ins;
    bool skinned = false, palette = false;
    Reg pos{kRastOut, 0, 0xF, false}; // vs_2_x: oPos; vs_3_0: the output declared as position 0
    for (size_t i = 1; i < words;) {
        const uint32_t t = code[i];
        const uint32_t op = t & 0xFFFF;
        if (op == kEnd) break;
        if (op == kComment) {
            i += 1 + ((t >> 16) & 0x7FFF);
            continue;
        }
        const size_t len = (t >> 24) & 0xF;
        if (i + 1 + len > words) return false;
        const uint32_t* p = code + i + 1;
        if (op == kDcl && len == 2) {
            const uint32_t usage = p[0] & 0x1F, index = (p[0] >> 16) & 0xF;
            const Reg d = Param(p[1]);
            if (d.type == kInput && usage == 2) skinned = true;             // D3DDECLUSAGE_BLENDINDICES
            if (d.type == kOutput && usage == 0 && index == 0) pos = d;     // D3DDECLUSAGE_POSITION
        } else if (op == kMov || op == kDp4 || op == kM4x4 || op == kM4x3 || op == 4 /* mad */ || op == 5 /* mul */ || op == 8 /* dp3 */) {
            Ins in;
            in.op = op;
            size_t k = 0;
            in.dst = Param(p[k++]);
            while (k < len && in.nsrc < 3) {
                const Reg s = Param(p[k++]);
                if (s.rel) {
                    k++; // the address register token (vs_2_0 and up)
                    if (s.type == kConst && s.num == 0) palette = true;
                }
                in.src[in.nsrc++] = s;
            }
            ins.push_back(in);
        }
        i += 1 + len;
    }
    if (!skinned || !palette) return false;

    // The last instruction before `before` that writes component c of register (type, num)
    const auto writer = [&](size_t before, uint32_t type, uint32_t num, int c) -> int {
        for (size_t j = before; j-- > 0;)
            if (ins[j].dst.type == type && ins[j].dst.num == num && (ins[j].dst.mask >> c & 1)) return static_cast<int>(j);
        return -1;
    };
    // One matrix: the rows that write components 0..n-1 of (type, num) before `before` from one temporary source.
    // Returns the first row register and the source, or -1
    const auto matrix = [&](size_t before, uint32_t type, uint32_t num, int n, Reg& src, size_t& first) -> int {
        int base = -1;
        first = before;
        for (int c = 0; c < n; c++) {
            int j = writer(before, type, num, c);
            if (j < 0) return -1;
            const Ins* w = &ins[j];
            if (w->op == kMov && w->src[0].type == kTemp) { // mov oPos, rN: follow rN (same component, plain swizzle assumed)
                const int k = writer(j, kTemp, w->src[0].num, c);
                if (k < 0) return -1;
                j = k, w = &ins[k];
            }
            int row;
            Reg s;
            if (w->op == kDp4 && w->nsrc == 2 && w->src[1].type == kConst && !w->src[1].rel && w->src[0].type != kConst) row = static_cast<int>(w->src[1].num), s = w->src[0];
            else if (w->op == kDp4 && w->nsrc == 2 && w->src[0].type == kConst && !w->src[0].rel && w->src[1].type != kConst) row = static_cast<int>(w->src[0].num), s = w->src[1];
            else if ((w->op == kM4x4 || w->op == kM4x3) && w->nsrc == 2 && w->src[1].type == kConst && !w->src[1].rel) row = static_cast<int>(w->src[1].num) + c, s = w->src[0];
            else return -1;
            if (c == 0) base = row, src = s;
            else if (row != base + c || s.type != src.type || s.num != src.num) return -1;
            first = std::min(first, static_cast<size_t>(j));
        }
        return base;
    };
    Reg s;
    size_t first = 0;
    const int f = matrix(ins.size(), pos.type, pos.num, 4, s, first);
    if (f < 0 || f < 3) return false; // (the palette lives from c0)
    out = {};
    out.f = f;
    if (s.type == kTemp) {
        Reg s2;
        size_t first2 = 0;
        const int w = matrix(first, kTemp, s.num, 3, s2, first2);
        if (w >= 3 && w != f) out.w = w;
    }
    return true;
}

using Rows = const float (*)[4]; // the vertex constant registers, c0 up

// clip = C p (column vectors) of a draw from its registers
inline void ClipOf(Rows reg, const Chain& ch, double C[4][4]) {
    for (int k = 0; k < 4; k++)
        for (int j = 0; j < 4; j++) {
            const float* F = reg[ch.f + k];
            if (ch.w < 0) C[k][j] = F[j];
            else C[k][j] = F[0] * reg[ch.w][j] + F[1] * reg[ch.w + 1][j] + F[2] * reg[ch.w + 2][j] + (j == 3 ? F[3] : 0.0);
        }
}

// The median of the bones' translations (model space) over `bones` palette entries
inline void BoneMedian(Rows reg, int bones, float out[3]) {
    bones = std::clamp(bones, 1, 60);
    float v[60];
    for (int a = 0; a < 3; a++) {
        for (int b = 0; b < bones; b++) v[b] = reg[3 * b + a][3];
        std::nth_element(v, v + bones / 2, v + bones);
        out[a] = v[bones / 2];
    }
}

inline bool Inverse(const double m[4][4], double out[4][4]) {
    double a[4][8];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 8; c++) a[r][c] = c < 4 ? m[r][c] : (c - 4 == r ? 1.0 : 0.0);
    for (int c = 0; c < 4; c++) {
        int p = c;
        for (int r = c + 1; r < 4; r++)
            if (std::fabs(a[r][c]) > std::fabs(a[p][c])) p = r;
        if (std::fabs(a[p][c]) < 1e-12) return false;
        for (int k = 0; k < 8; k++) std::swap(a[c][k], a[p][k]);
        const double d = a[c][c];
        for (int k = 0; k < 8; k++) a[c][k] /= d;
        for (int r = 0; r < 4; r++)
            if (r != c) {
                const double f = a[r][c];
                for (int k = 0; k < 8; k++) a[r][k] -= f * a[c][k];
            }
    }
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) out[r][c] = a[r][c + 4];
    return true;
}

// The draw's model-to-world matrix M = V^-1 C (V rows = the camera's c40..c43). false: not a draw of this camera
// (no affine result, a mirror, a shear, or a scale far from 1)
inline bool WorldOf(const double C[4][4], const float vp[4][4], double M[4][4]) {
    double V[4][4], Vi[4][4];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) V[r][c] = vp[r][c];
    if (!Inverse(V, Vi)) return false;
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) M[r][c] = Vi[r][0] * C[0][c] + Vi[r][1] * C[1][c] + Vi[r][2] * C[2][c] + Vi[r][3] * C[3][c];
    const double s = std::fabs(M[3][3]);
    if (s < 1e-6) return false;
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) M[r][c] /= M[3][3];
    if (std::fabs(M[3][0]) + std::fabs(M[3][1]) + std::fabs(M[3][2]) > 1e-3) return false;
    const double det = M[0][0] * (M[1][1] * M[2][2] - M[1][2] * M[2][1]) - M[0][1] * (M[1][0] * M[2][2] - M[1][2] * M[2][0]) +
                       M[0][2] * (M[1][0] * M[2][1] - M[1][1] * M[2][0]);
    if (!(det > 0.3 && det < 3.0)) return false;
    double len[3];
    for (int c = 0; c < 3; c++) len[c] = std::sqrt(M[0][c] * M[0][c] + M[1][c] * M[1][c] + M[2][c] * M[2][c]);
    for (int a = 0; a < 3; a++)
        for (int b = a + 1; b < 3; b++)
            if (std::fabs(M[0][a] * M[0][b] + M[1][a] * M[1][b] + M[2][a] * M[2][b]) > 0.05 * len[a] * len[b]) return false;
    return true;
}

inline void Apply(const double M[4][4], const float p[3], float out[3]) {
    for (int r = 0; r < 3; r++) out[r] = static_cast<float>(M[r][0] * p[0] + M[r][1] * p[1] + M[r][2] * p[2] + M[r][3]);
}

// ---- Capsules from the bones (XR-10) ----
// A rig names, for a palette of its size, the bones that carry its joints and each joint's bind place (model space, the
// place the joint has while the palette is the identity), and the capsules between joints. The palette entry of bone b
// maps a bind place to its place this frame (3 rows, model space), so a joint's place is its bone's rows applied to its
// bind place, whatever the pose: a child, a dog, a sitting or lying Sim get their own shapes. A draw with no rig (none for
// its palette, or more than one) keeps the fixed column below. A fit whose capsules come out longer or shorter than
// the rig's bind lengths by more than kLengthTolerance is a palette of another order: also the fixed column.
inline constexpr int kMaxPerSim = 8;             // capsules a Sim at most
inline constexpr float kLengthTolerance = 0.35f; // share of a capsule's bind length

struct RigJoint {
    int bone;      // palette entry
    float bind[3]; // model space, metres
};
struct RigCapsule {
    int a, b; // joints (a == b: a ball)
    float r;
};
struct Rig {
    const char* name;
    int bones; // palette entries of a draw of this rig
    std::vector<RigJoint> joints;
    std::vector<RigCapsule> capsules;
};

// The rigs known by palette size. Empty in game: the palette is in each drawn mesh's own joint order, with only the joints
// that mesh uses (the skin fill 0x0068F3F0, Steam 1.67.2; wp/XR-10.md), so a palette's size names no rig and every Sim
// keeps the fixed column. The source that fits in game is a rig made per draw from the skin's joint name hashes and bind
// poses (drawable +0x64 skin: +0x2C hashes, +0x30 bind poses), read by a hook on that fill: a follow-up package
inline const std::vector<Rig>& GameRigs() {
    static const std::vector<Rig> rigs;
    return rigs;
}

// The one rig of `rigs` with `bones` palette entries, or nullptr (none, or more than one)
inline const Rig* RigOf(const std::vector<Rig>& rigs, int bones) {
    const Rig* found = nullptr;
    for (const Rig& r : rigs)
        if (r.bones == bones) {
            if (found) return nullptr;
            found = &r;
        }
    return found;
}

// A capsule in model or world space
struct Capsule {
    float a[3], b[3], r;
};

// The rig's capsules this frame in model space from the palette rows. Returns how many (0: no fit: a bone past the
// palette, a capsule's length off by more than kLengthTolerance, or more than kMaxPerSim capsules)
inline int FitRig(Rows reg, int bones, const Rig& rig, Capsule out[kMaxPerSim]) {
    if (rig.capsules.size() > static_cast<size_t>(kMaxPerSim) || rig.joints.size() > 64 || bones != rig.bones) return 0;
    float at[64][3];
    for (size_t j = 0; j < rig.joints.size(); j++) {
        const RigJoint& J = rig.joints[j];
        if (J.bone < 0 || J.bone >= bones || J.bone >= 60) return 0;
        for (int k = 0; k < 3; k++) {
            const float* row = reg[3 * J.bone + k];
            at[j][k] = row[0] * J.bind[0] + row[1] * J.bind[1] + row[2] * J.bind[2] + row[3];
        }
        if (!std::isfinite(at[j][0]) || !std::isfinite(at[j][1]) || !std::isfinite(at[j][2])) return 0;
    }
    const auto dist = [](const float* p, const float* q) {
        return std::sqrt((p[0] - q[0]) * (p[0] - q[0]) + (p[1] - q[1]) * (p[1] - q[1]) + (p[2] - q[2]) * (p[2] - q[2]));
    };
    int n = 0;
    for (const RigCapsule& c : rig.capsules) {
        if (c.a < 0 || c.b < 0 || static_cast<size_t>(c.a) >= rig.joints.size() || static_cast<size_t>(c.b) >= rig.joints.size()) return 0;
        const float bind = dist(rig.joints[c.a].bind, rig.joints[c.b].bind), now = dist(at[c.a], at[c.b]);
        if (std::fabs(now - bind) > kLengthTolerance * bind + 0.02f) return 0;
        Capsule& o = out[n++];
        for (int k = 0; k < 3; k++) o.a[k] = at[c.a][k], o.b[k] = at[c.b][k];
        o.r = c.r;
    }
    return n;
}

// The fixed column of a standing adult (world y up, metres above the root): the fallback for a draw with no rig fit.
// Look constants. UNVERIFIED in game
struct Part {
    float y0, y1, r;
};
inline constexpr Part kParts[] = {{0.14f, 0.82f, 0.14f}, {0.95f, 1.38f, 0.19f}, {1.62f, 1.62f, 0.12f}};
inline constexpr int kPerSim = 3;        // capsules of the fixed column
inline constexpr float kSameSim = 0.35f; // m: draws whose roots lie closer are one Sim (body parts, hair, accessories)

struct Root {
    float p[3];
};

// One Sim: its root and its capsules in world space (n 0: the fixed column at the root)
struct Sim {
    Root root{};
    int n = 0;
    Capsule c[kMaxPerSim] = {};
};

// The capsules a Sim casts: its fit, or the fixed column at its root
inline int CapsulesOf(const Sim& s, Capsule out[kMaxPerSim]) {
    if (s.n > 0) {
        for (int i = 0; i < s.n; i++) out[i] = s.c[i];
        return s.n;
    }
    for (int i = 0; i < kPerSim; i++) {
        const Part& part = kParts[i];
        out[i] = {{s.root.p[0], s.root.p[1] + part.y0, s.root.p[2]}, {s.root.p[0], s.root.p[1] + part.y1, s.root.p[2]}, part.r};
    }
    return kPerSim;
}

// The model-space fit moved to world space by the draw's matrix M
inline void ToWorld(const double M[4][4], const Capsule* in, int n, Sim& s) {
    s.n = n;
    for (int i = 0; i < n; i++) Apply(M, in[i].a, s.c[i].a), Apply(M, in[i].b, s.c[i].b), s.c[i].r = in[i].r;
}

// One Sim per root merged within kSameSim, nearest the camera first, at most `most`. The first draw's root stays (a Sim's
// parts share one matrix); the first draw with a fit gives the capsules
inline std::vector<Sim> Merge(const std::vector<Sim>& draws, const float cam[3], size_t most) {
    std::vector<Sim> sims;
    for (const Sim& d : draws) {
        size_t i = 0;
        for (; i < sims.size(); i++) {
            const float dx = d.root.p[0] - sims[i].root.p[0], dy = d.root.p[1] - sims[i].root.p[1], dz = d.root.p[2] - sims[i].root.p[2];
            if (dx * dx + dz * dz < kSameSim * kSameSim && std::fabs(dy) < 0.5f) break;
        }
        if (i == sims.size()) sims.push_back(d);
        else if (sims[i].n == 0 && d.n > 0) {
            sims[i].n = d.n;
            for (int k = 0; k < d.n; k++) sims[i].c[k] = d.c[k];
        }
    }
    const auto dist = [&](const Sim& s) {
        const float dx = s.root.p[0] - cam[0], dy = s.root.p[1] - cam[1], dz = s.root.p[2] - cam[2];
        return dx * dx + dy * dy + dz * dz;
    };
    std::sort(sims.begin(), sims.end(), [&](const Sim& a, const Sim& b) { return dist(a) < dist(b); });
    if (sims.size() > most) sims.resize(most);
    return sims;
}

// The box around a Sim's capsules
inline void Bounds(const Sim& s, float lo[3], float hi[3]) {
    Capsule c[kMaxPerSim];
    const int n = CapsulesOf(s, c);
    for (int k = 0; k < 3; k++) lo[k] = 1e30f, hi[k] = -1e30f;
    for (int i = 0; i < n; i++)
        for (int k = 0; k < 3; k++) {
            lo[k] = std::min({lo[k], c[i].a[k] - c[i].r, c[i].b[k] - c[i].r});
            hi[k] = std::max({hi[k], c[i].a[k] + c[i].r, c[i].b[k] + c[i].r});
        }
}

// The block's capsules and caster bounds for this frame's Sims (after any capsules already in it); `prev` are the last
// pass's Sims: a Sim with no previous root within kSameSim, one whose root moved by more than 5 mm, or one whose capsules
// changed (another pose in place: sitting down, a fit gained or lost) is marked moved, its bounds holding both shapes.
// Returns the Sims added
inline uint32_t Fill(GiPixelBlock& b, const std::vector<Sim>& sims, const std::vector<Sim>& prev) {
    uint32_t added = 0;
    for (const Sim& s : sims) {
        Capsule caps[kMaxPerSim];
        const int n = CapsulesOf(s, caps);
        if (b.capsules + n > kGiPixelCapsules || b.casters >= kGiPixelCasters) break;
        for (int i = 0; i < n; i++) {
            const Capsule& c = caps[i];
            b.capsule[b.capsules++] = {{c.a[0], c.a[1], c.a[2]}, c.r, {c.b[0], c.b[1], c.b[2]}, 0};
        }
        GiPixelCaster& k = b.caster[b.casters++];
        k = {};
        Bounds(s, k.lo, k.hi);
        const Sim* was = nullptr;
        float best = kSameSim * kSameSim;
        for (const Sim& q : prev) {
            const float dx = s.root.p[0] - q.root.p[0], dz = s.root.p[2] - q.root.p[2], d2 = dx * dx + dz * dz;
            if (d2 < best && std::fabs(s.root.p[1] - q.root.p[1]) < 0.5f) best = d2, was = &q;
        }
        bool moved = !was || std::fabs(was->root.p[0] - s.root.p[0]) + std::fabs(was->root.p[1] - s.root.p[1]) + std::fabs(was->root.p[2] - s.root.p[2]) > 0.005f;
        if (!moved) {
            Capsule old[kMaxPerSim];
            const int m = CapsulesOf(*was, old);
            moved = m != n;
            for (int i = 0; i < n && !moved; i++)
                for (int a = 0; a < 3; a++) moved = moved || std::fabs(old[i].a[a] - caps[i].a[a]) + std::fabs(old[i].b[a] - caps[i].b[a]) > 0.005f;
        }
        if (moved && was) {
            float lo[3], hi[3];
            Bounds(*was, lo, hi);
            for (int a = 0; a < 3; a++) k.lo[a] = std::min(k.lo[a], lo[a]), k.hi[a] = std::max(k.hi[a], hi[a]);
        }
        k.moved = moved ? 1.0f : 0.0f;
        added++;
    }
    return added;
}

} // namespace GiSimCapsules
