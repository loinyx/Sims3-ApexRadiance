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
    long dirAt[3] = {-1, -1, -1}, colAt[3] = {-1, -1, -1};
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
        // dp3 rL.c, c27+k[a0.s], rN: the per-instance lamp directions
        if (x.op == kDp3 && o.size() == 3 && Type(t[o[1]]) == kConst && (t[o[1]] & 0x2000u) && Num(t[o[1]]) >= 27 && Num(t[o[1]]) <= 29 && Type(t[o[2]]) == kTemp) {
            dirAt[Num(t[o[1]]) - 27] = static_cast<long>(n);
            addrTok = t[o[1] + 1];
            normal = Num(t[o[2]]);
            lampTemp = Num(t[o[0]]);
        }
        // the lamp colours c54+k[a0.s] (mul for k = 0, mad for 1 and 2)
        if ((x.op == kMul || x.op == kMad) && o.size() >= 3 && Type(t[o[2]]) == kConst && (t[o[2]] & 0x2000u) && Num(t[o[2]]) >= 54 && Num(t[o[2]]) <= 56)
            colAt[Num(t[o[2]]) - 54] = static_cast<long>(n);
        // dp4 oPos.x, rW, cK: the camera-relative world position
        if (x.op == kDp4 && Type(t[o[0]]) == kOutput && Num(t[o[0]]) == 0 && WMask(t[o[0]]) == 0x1 && Type(t[o[1]]) == kTemp) world = Num(t[o[1]]);
    }
    for (int k = 0; k < 3; k++)
        if (dirAt[k] < 0 || colAt[k] < 0) return false;
    if (world == ~0u || normal == ~0u || maxTemp < 0 || maxTemp + 6 > 11) return false; // 6 new temps; vs_2_0 has r0..r11
    if (!(ins[colAt[0]].op == kMul && ins[colAt[1]].op == kMad && ins[colAt[2]].op == kMad)) return false;
    const long first = std::min({dirAt[0], dirAt[1], dirAt[2]});
    // the world temp must still hold the position where the block goes (before the first lamp dp3)
    bool seenPos = false;
    for (long i = 0; i < first; i++) {
        if (ins[i].op == kDef || ins[i].op == kDcl) continue;
        const auto o = Operands(t, ins[i]);
        if (ins[i].op == kDp4 && Type(t[o[0]]) == kOutput) seenPos = true;
        else if (seenPos && Type(t[o[0]]) == kTemp && Num(t[o[0]]) == world && (WMask(t[o[0]]) & 0x7)) return false;
    }
    if (!seenPos) return false;
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
    const DWORD Acc = maxTemp + 1, Dir = Acc + 1, Sc = Acc + 2, P = Acc + 3, K = Acc + 4, Sv = Acc + 5;
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
    // per instance: P = (pos - C) / semi-axes, P.w = |P|^2 - 1; Sv = 1 / semi-axes; Sv.w = rho_g / 2 x (1 - n.y)
    E(kAdd, {d(P, 0x7), r(world), O{Neg(Src(kConst, B + 54)), true}});
    E(kMul, {d(P, 0x5), r(P), ci(B + 54, W)});
    E(kMul, {d(P, 0x2), r(P), ci(B + 55, X)});
    E(kDp3, {d(P, 0x8), r(P), r(P)});
    E(kAdd, {d(P, 0x8), r(P, W), cf(cM1, sM1)});
    E(kMov, {d(Sv, 0x5), ci(B + 54, W)});
    E(kMov, {d(Sv, 0x2), ci(B + 55, X)});
    E(kAdd, {d(Sv, 0x8), nr(normal, Y), one});
    E(kMul, {d(Sv, 0x8), r(Sv, W), cf(cG, Y)});
    E(kMov, {d(Acc, 0x7), zero});
    // law(d^2 = r[src].comp) relative to the law at the colour's point -> Sc.z (scratch K.w); capCopy: capped (direct light)
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
        const DWORD Cp = B + k, Q = B + 27 + k, Col = 54 + k;
        E(kAdd, {d(Dir, 0x7), ci(Cp), nr(world)});
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
        E(kMul, {d(Sc, 0x2), r(Sc, Y), r(Sc, Z)});
        E(kMad, {d(Acc, 0x7), r(Sc, Y), ci(Col), r(Acc)});
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
        E(kMul, {d(Sc, 0x4), r(Sc, Z), r(Sv, W)});
        E(kMad, {d(Acc, 0x7), r(Sc, Z), ci(Col), r(Acc)});
    }
    // The 3 dp3 c27..29 and the wrap on their result go; the colour products become movs (Acc enters once, at k = 0).
    struct Edit {
        size_t at, n;
        std::vector<DWORD> code;
    };
    std::vector<Edit> ed;
    for (int k = 0; k < 3; k++) ed.push_back({ins[dirAt[k]].at, ins[dirAt[k]].len + 1, {}});
    for (long i = first; i < colAt[0]; i++) {
        const Ins& x = ins[i];
        if ((x.op == kMad || x.op == kMax) && Type(t[x.at + 1]) == kTemp && Num(t[x.at + 1]) == lampTemp && WMask(t[x.at + 1]) == 0xE)
            ed.push_back({x.at, x.len + 1, {}});
    }
    for (int k = 0; k < 3; k++) {
        const Ins& x = ins[colAt[k]];
        const auto o = Operands(t, x);
        if (k == 0) ed.push_back({x.at, x.len + 1, {Op(kMov, 2), t[o[0]], Src(kTemp, Acc)}});
        else if (o.size() >= 4) ed.push_back({x.at, x.len + 1, {Op(kMov, 2), t[o[0]], t[o[3]]}}); // mad dD, rL, cCol, rS -> mov dD, rS
        else return false;
    }
    ed.push_back({ins[first].at, 0, c});
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
