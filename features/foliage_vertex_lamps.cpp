// Per-vertex lamp light on instanced foliage (see foliage_vertex_lamps.h). Credits: @loinyx
#include "foliage_vertex_lamps.h"
#include <algorithm>
#include <cstring>
#include <initializer_list>

namespace FoliageVertexLamps {
namespace {

using DWORD = uint32_t;
enum : DWORD { kTemp = 0, kConst = 2, kOutput = 4 };
enum : DWORD { kMov = 0x01, kAdd = 0x02, kMad = 0x04, kMul = 0x05, kRcp = 0x06, kRsq = 0x07, kDp3 = 0x08, kDp4 = 0x09, kMin = 0x0A, kMax = 0x0B,
               kExp = 0x0E, kDcl = 0x1F, kDef = 0x51, kComment = 0xFFFE, kEnd = 0xFFFF };
DWORD Num(DWORD r) { return r & 0x7FF; }
DWORD Type(DWORD r) { return ((r >> 28) & 7) | (((r >> 11) & 3) << 3); }
DWORD Reg(DWORD type, DWORD num) { return 0x80000000u | ((type & 7) << 28) | ((type & 0x18) << 8) | num; }
DWORD Dst(DWORD type, DWORD num, DWORD mask = 0xF) { return Reg(type, num) | (mask << 16); }
DWORD Src(DWORD type, DWORD num, DWORD swz = 0xE4) { return Reg(type, num) | (swz << 16); }
DWORD Op(DWORD op, DWORD len) { return op | (len << 24); }
DWORD Neg(DWORD s) { return s | 0x01000000u; }
DWORD WMask(DWORD d) { return (d >> 16) & 0xF; }
constexpr DWORD X = 0x00, Y = 0x55, Z = 0xAA, W = 0xFF, XYZW = 0xE4;

struct Ins {
    size_t at;
    DWORD op, len;
};
std::vector<Ins> Parse(const std::vector<DWORD>& t) {
    std::vector<Ins> v;
    for (size_t i = 1; i < t.size();) {
        const DWORD op = t[i] & 0xFFFF;
        if (op == kEnd) break;
        const DWORD len = op == kComment ? (t[i] >> 16) & 0x7FFF : (t[i] >> 24) & 0xF;
        if (i + 1 + len > t.size()) return {};
        if (op != kComment) v.push_back({i, op, len});
        i += 1 + len;
    }
    return v;
}
// operand token indices (the address token after a relative source is skipped)
std::vector<size_t> Operands(const std::vector<DWORD>& t, const Ins& x) {
    std::vector<size_t> o;
    for (size_t k = 1; k <= x.len; k++) {
        o.push_back(x.at + k);
        if (k > 1 && (t[x.at + k] & 0x2000u)) k++;
    }
    return o;
}

} // namespace

bool Patch(std::vector<uint32_t>& t, int& base) {
    base = -1;
    if (t.size() < 2 || t[0] != 0xFFFE0200u) return false; // vs_2_0 only (the captured family)
    const auto ins = Parse(t);
    if (ins.empty()) return false;
    int maxTemp = -1, maxConst = -1;
    long dirAt[3] = {-1, -1, -1}, lastPos = -1;
    DWORD addrTok = 0, normal = ~0u, world = ~0u, lampTemp = ~0u;
    for (size_t n = 0; n < ins.size(); n++) {
        const Ins& x = ins[n];
        if (x.op == kDef) {
            if (Num(t[x.at + 1]) != 255) maxConst = std::max<int>(maxConst, static_cast<int>(Num(t[x.at + 1])));
            continue;
        }
        if (x.op == kDcl) continue;
        const auto o = Operands(t, x);
        for (size_t i : o) {
            if (Type(t[i]) == kTemp) maxTemp = std::max<int>(maxTemp, static_cast<int>(Num(t[i])));
            if (Type(t[i]) == kConst && Num(t[i]) != 255 && !(t[i] & 0x2000u)) maxConst = std::max<int>(maxConst, static_cast<int>(Num(t[i])));
        }
        // dp3 rL.c, c27+k[a0.s], rN (either operand order): the per-instance lamp directions
        if (x.op == kDp3 && o.size() == 3 && Type(t[o[0]]) == kTemp) {
            for (int s = 1; s <= 2; s++) {
                const DWORD c = t[o[s]], other = t[o[3 - s]];
                if (Type(c) == kConst && (c & 0x2000u) && Num(c) >= 27 && Num(c) <= 29 && Type(other) == kTemp && !(other & 0x2000u)) {
                    const int k = static_cast<int>(Num(c)) - 27;
                    if (dirAt[k] >= 0) return false;
                    dirAt[k] = static_cast<long>(n);
                    addrTok = t[o[s] + 1];
                    normal = Num(other);
                    if (lampTemp != ~0u && lampTemp != Num(t[o[0]])) return false;
                    lampTemp = Num(t[o[0]]);
                    // lamp k is written to component k + 1 (y, z, w), as in every captured shader
                    if (WMask(t[o[0]]) != (2u << k)) return false;
                }
            }
        }
        // dp4 oPos.c, rW, cK: the (camera-relative) world position
        if (x.op == kDp4 && Type(t[o[0]]) == kOutput && Num(t[o[0]]) == 0 && Type(t[o[1]]) == kTemp) {
            if (world != ~0u && world != Num(t[o[1]])) return false;
            world = Num(t[o[1]]);
            lastPos = static_cast<long>(n);
        }
    }
    for (int k = 0; k < 3; k++)
        if (dirAt[k] < 0) return false;
    if (world == ~0u || normal == ~0u || lampTemp == ~0u || lastPos < 0 || maxTemp < 0 || maxTemp + 6 > 11) return false; // 6 new temps
    const long first = std::min({dirAt[0], dirAt[1], dirAt[2]}), last = std::max({dirAt[0], dirAt[1], dirAt[2]});
    if (lastPos > first) return false; // the position is computed before the lamps
    // The wrap on the lamp terms (PatchFoliageVs: mad rL.yzw, rL, c255.x, c255.y; max rL.yzw, rL, cZ), right after the dp3s
    long wrapMad = -1, wrapMax = -1;
    for (long i = last + 1; i < static_cast<long>(ins.size()) && (wrapMad < 0 || wrapMax < 0); i++) {
        const Ins& x = ins[i];
        if (x.op == kDcl || x.op == kDef) continue;
        const auto o = Operands(t, x);
        const bool toLamps = !o.empty() && Type(t[o[0]]) == kTemp && Num(t[o[0]]) == lampTemp && (WMask(t[o[0]]) & 0xE);
        if (toLamps && x.op == kMad && wrapMad < 0 && WMask(t[o[0]]) == 0xE) wrapMad = i;
        else if (toLamps && x.op == kMax && wrapMad >= 0 && WMask(t[o[0]]) == 0xE) wrapMax = i;
        else if (toLamps) return false; // something else writes the lamp terms first
    }
    if (wrapMad < 0 || wrapMax < 0) return false;
    // nothing reads the lamp terms between the dp3s and the wrap but the wrap
    for (long i = first; i < wrapMax; i++) {
        if (i == dirAt[0] || i == dirAt[1] || i == dirAt[2] || i == wrapMad) continue;
        const auto o = Operands(t, ins[i]);
        const DWORD op = ins[i].op;
        // a per-component op writing only the lamp term's x (the sun) reads only its x
        if (!o.empty() && Type(t[o[0]]) == kTemp && Num(t[o[0]]) == lampTemp && !(WMask(t[o[0]]) & 0xE) &&
            (op == kMov || op == kAdd || op == kMul || op == kMad || op == kMin || op == kMax))
            continue;
        for (size_t k = 1; k < o.size(); k++)
            if (Type(t[o[k]]) == kTemp && Num(t[o[k]]) == lampTemp) return false;
    }
    // above the per-instance arrays the shader reads relatively (c0..c80: c54 + 3 x 8 + 2), whatever its direct reads
    const DWORD B = static_cast<DWORD>(std::max(maxConst + 1, 81));
    if (B + 82 > 254) return false;
    // -1 and 0 from the shader's own defs, else a def at B + 82
    DWORD cM1 = ~0u, sM1 = 0, cZ = ~0u, sZ = 0;
    for (const Ins& x : ins)
        if (x.op == kDef)
            for (DWORD c = 0; c < 4; c++) {
                float v;
                std::memcpy(&v, &t[x.at + 2 + c], 4);
                if (v == -1.0f && cM1 == ~0u) cM1 = Num(t[x.at + 1]), sM1 = c * 0x55;
                if (v == 0.0f && cZ == ~0u) cZ = Num(t[x.at + 1]), sZ = c * 0x55;
            }
    std::vector<DWORD> pre;
    if (cM1 == ~0u || cZ == ~0u) {
        pre = {Op(kDef, 5), Dst(kConst, B + 82), 0xBF800000u /*-1*/, 0u, 0u, 0u};
        cM1 = B + 82, sM1 = X, cZ = B + 82, sZ = Y;
    }
    const DWORD Pos = maxTemp + 1, Dir = Pos + 1, Sc = Pos + 2, P = Pos + 3, K = Pos + 4, Sv = Pos + 5;
    std::vector<DWORD> c;
    struct O {
        DWORD tok;
        bool rel;
    };
    const auto E = [&](DWORD op, std::initializer_list<O> ops) {
        DWORD len = 0;
        for (const O& o : ops) len += o.rel ? 2 : 1;
        c.push_back(Op(op, len));
        for (const O& o : ops) {
            c.push_back(o.tok | (o.rel ? 0x2000u : 0u));
            if (o.rel) c.push_back(addrTok);
        }
    };
    const auto r = [](DWORD n, DWORD s = XYZW) { return O{Src(kTemp, n, s), false}; };
    const auto nr = [](DWORD n, DWORD s = XYZW) { return O{Neg(Src(kTemp, n, s)), false}; };
    const auto d = [](DWORD n, DWORD m) { return O{Dst(kTemp, n, m), false}; };
    const auto ci = [](DWORD n, DWORD s = XYZW) { return O{Src(kConst, n, s), true}; };  // c[n + a0.s]
    const auto cf = [](DWORD n, DWORD s = XYZW) { return O{Src(kConst, n, s), false}; }; // c[n]
    const O one = {Neg(Src(kConst, cM1, sM1)), false}, zero = {Src(kConst, cZ, sZ), false};
    const DWORD cG = B + 81, cWrap = 255;
    // the position, kept right after oPos (shaders reuse its register before the lamps)
    const std::vector<DWORD> keep = {Op(kMov, 2), Dst(kTemp, Pos, 0x7), Src(kTemp, world)};
    // per instance: P = (pos - C) / semi-axes, P.w = |P|^2 - 1; Sv = 1 / semi-axes; Sv.w = rho_g / 2 x (1 - n.y)
    E(kAdd, {d(P, 0x7), r(Pos), O{Neg(Src(kConst, B + 54)), true}});
    E(kMul, {d(P, 0x5), r(P), ci(B + 54, W)});
    E(kMul, {d(P, 0x2), r(P), ci(B + 55, X)});
    E(kDp3, {d(P, 0x8), r(P), r(P)});
    E(kAdd, {d(P, 0x8), r(P, W), cf(cM1, sM1)});
    E(kMov, {d(Sv, 0x5), ci(B + 54, W)});
    E(kMov, {d(Sv, 0x2), ci(B + 55, X)});
    E(kAdd, {d(Sv, 0x8), nr(normal, Y), one});
    E(kMul, {d(Sv, 0x8), r(Sv, W), cf(cG, Y)});
    // law(d^2 = r[src].comp) relative to the law at the colour's point -> Sc.z (scratch K.w); capped: the direct light
    const auto law = [&](DWORD src, DWORD comp, DWORD Q, DWORD Cp, DWORD capComp, bool capped) {
        E(kMul, {d(Sc, 0x4), r(src, comp), ci(Q, X)});
        E(kMad, {d(Sc, 0x4), nr(Sc, Z), r(Sc, Z), one});
        E(kMax, {d(Sc, 0x4), r(Sc, Z), zero});
        E(kMul, {d(Sc, 0x4), r(Sc, Z), r(Sc, Z)});
        E(kMul, {d(Sc, 0x4), r(Sc, Z), ci(Cp, W)});
        E(kRcp, {d(K, 0x8), r(src, comp)});
        E(kMul, {d(Sc, 0x4), r(Sc, Z), r(K, W)});
        E(kMin, {d(Sc, 0x4), r(Sc, Z), one});
        E(kMul, {d(Sc, 0x4), r(Sc, Z), r(K, W)});
        E(kMul, {d(Sc, 0x4), r(Sc, Z), ci(Q, Y)});
        if (capped) E(kMin, {d(Sc, 0x4), r(Sc, Z), ci(B + 56, capComp)});
    };
    for (DWORD k = 0; k < 3; k++) {
        const DWORD Cp = B + k, Q = B + 27 + k, out = 2u << k; // the lamp term's component: y, z, w
        E(kAdd, {d(Dir, 0x7), ci(Cp), nr(Pos)});
        E(kDp3, {d(Dir, 0x8), r(Dir), r(Dir)});
        E(kMax, {d(Dir, 0x8), r(Dir, W), ci(Q, Z)});
        E(kRsq, {d(Sc, 0x8), r(Dir, W)});
        E(kMul, {d(Dir, 0x7), r(Dir), r(Sc, W)});
        E(kDp3, {d(Sc, 0x1), r(Dir), r(normal)});                         // N.l per vertex
        E(kMad, {d(Sc, 0x2), r(Sc, X), cf(cWrap, X), cf(cWrap, Y)});
        E(kMax, {d(Sc, 0x2), r(Sc, Y), zero});                            // the wrap constants (c255)
        E(kMax, {d(Sc, 0x4), nr(Sc, X), zero});
        E(kMad, {d(Sc, 0x2), r(Sc, Z), ci(B + 56, W), r(Sc, Y)});         // + tau/rho x max(-N.l, 0) (per instance: 0 = none)
        law(Dir, W, Q, Cp, k * 0x55, true);
        // Beer-Lambert along the vertex -> lamp segment inside the crown ellipsoid (in its unit-sphere space)
        E(kMul, {d(K, 0x7), r(Dir), r(Sv)});
        E(kDp3, {d(K, 0x8), r(K), r(K)});                                 // a
        E(kDp3, {d(Sc, 0x8), r(P), r(K)});                                // b
        E(kMul, {d(K, 0x1), r(Sc, W), r(Sc, W)});
        E(kMad, {d(K, 0x1), nr(K, W), r(P, W), r(K, X)});                 // b^2 - a c
        E(kMax, {d(K, 0x1), r(K, X), zero});
        E(kRsq, {d(K, 0x1), r(K, X)});
        E(kRcp, {d(K, 0x1), r(K, X)});
        E(kRcp, {d(K, 0x8), r(K, W)});
        E(kAdd, {d(K, 0x2), nr(Sc, W), nr(K, X)});                        // entry
        E(kAdd, {d(K, 0x1), nr(Sc, W), r(K, X)});                         // exit
        E(kMul, {d(K, 0x3), r(K), r(K, W)});
        E(kMax, {d(K, 0x3), r(K), zero});
        E(kAdd, {d(K, 0x1), r(K, X), nr(K, Y)});                          // the path inside, ahead of the vertex
        E(kRsq, {d(K, 0x4), r(Dir, W)});
        E(kRcp, {d(K, 0x4), r(K, Z)});                                    // the distance to the lamp
        E(kMin, {d(K, 0x1), r(K, X), r(K, Z)});                           // a lamp inside the crown: only the leaves before it
        E(kMul, {d(K, 0x1), r(K, X), O{Neg(Src(kConst, B + 55, Y)), true}});
        E(kExp, {d(K, 0x1), r(K, X)});
        E(kMul, {d(Sc, 0x4), r(Sc, Z), r(K, X)});
        E(kMul, {d(lampTemp, out), r(Sc, Y), r(Sc, Z)});                  // the lamp's weight: its colour is applied as before
        if (k != 0) continue;
        // the ground's light under the vertex: law(dg^2) h / dg, x rho_g / 2 (1 - n.y) (strongest lamp only)
        E(kMad, {d(K, 0x1), nr(Dir, Y), r(Dir, Y), one});
        E(kMul, {d(K, 0x1), r(K, X), r(Dir, W)});                         // horizontal distance^2
        E(kMad, {d(K, 0x1), ci(Q, W), ci(Q, W), r(K, X)});
        E(kMax, {d(K, 0x1), r(K, X), ci(Q, Z)});
        law(K, X, Q, Cp, 0, false);
        E(kRsq, {d(K, 0x2), r(K, X)});
        E(kMul, {d(Sc, 0x4), r(Sc, Z), r(K, Y)});
        E(kMul, {d(Sc, 0x4), r(Sc, Z), ci(Q, W)});
        E(kMad, {d(lampTemp, out), r(Sc, Z), r(Sv, W), r(lampTemp, Y)});  // + the ground's light
    }
    // Edits: the 3 dp3s and the wrap go; the block computes the three weights where the first dp3 was; the position is
    // kept after the last oPos write.
    struct Edit {
        size_t at, n;
        std::vector<DWORD> code;
    };
    std::vector<Edit> ed;
    for (int k = 0; k < 3; k++) ed.push_back({ins[dirAt[k]].at, ins[dirAt[k]].len + 1, {}});
    ed.push_back({ins[wrapMad].at, ins[wrapMad].len + 1, {}});
    ed.push_back({ins[wrapMax].at, ins[wrapMax].len + 1, {}});
    ed.push_back({ins[first].at, 0, c});
    ed.push_back({ins[lastPos].at + ins[lastPos].len + 1, 0, keep});
    if (!pre.empty()) ed.push_back({1, 0, pre});
    std::sort(ed.begin(), ed.end(), [](const Edit& a, const Edit& b) { return a.at > b.at || (a.at == b.at && a.n > b.n); });
    for (const Edit& e : ed) {
        t.erase(t.begin() + static_cast<long>(e.at), t.begin() + static_cast<long>(e.at + e.n));
        t.insert(t.begin() + static_cast<long>(e.at), e.code.begin(), e.code.end());
    }
    base = static_cast<int>(B);
    return true;
}

} // namespace FoliageVertexLamps
