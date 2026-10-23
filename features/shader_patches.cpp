// Bytecode edits of the game's shaders (part of Night Lighting). The game's shaders are found by
// pattern (instruction and register shape), not by exact bytes, so variants of the same shader are covered too. Each
// edit inserts a few instructions, uses the first free temp register / sampler / constant, and fails (leaving the shader
// alone) when the expected pattern is not there. Findings behind each one: NOTAS-ILUMINACAO.md.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "shader_patches.h"
#include <algorithm>
#include <cmath>
#include <cstring>

namespace {

// ---- token helpers (D3D9 SM2/SM3) ----
enum : DWORD { kTemp = 0, kInput = 1, kConst = 2, kTexture = 3, kOutput = 6, kColorOut = 8, kSampler = 10 };
enum : DWORD { kMov = 0x01, kAdd = 0x02, kMova = 0x2E, kMad = 0x04, kMul = 0x05, kDp3 = 0x08, kDp4 = 0x09, kMax = 0x0B, kSlt = 0x0C, kLrp = 0x12, kTexkill = 0x41, kDcl = 0x1F, kDefB = 0x2F, kDefI = 0x30, kDef = 0x51, kTexld = 0x42 };
constexpr DWORD kSwzX = 0x00, kSwzY = 0x55, kSwzW = 0xFF, kSwzXYZW = 0xE4, kSwzZWZW = 0xEE, kSwzXYXZ = 0x84, kSwzXZZW = 0xE8, kSwzXYXY = 0x44;

DWORD Num(DWORD r) { return r & 0x7FF; }
DWORD Type(DWORD r) { return ((r >> 28) & 7) | (((r >> 11) & 3) << 3); }
DWORD Swz(DWORD r) { return (r >> 16) & 0xFF; }
DWORD WMask(DWORD r) { return (r >> 16) & 0xF; }
DWORD Reg(DWORD type, DWORD num) { return 0x80000000u | ((type & 7) << 28) | (((type >> 3) & 3) << 11) | (num & 0x7FF); }
DWORD Dst(DWORD type, DWORD num, DWORD mask = 0xF, bool sat = false) { return Reg(type, num) | (mask << 16) | (sat ? 0x00100000u : 0u); }
DWORD Src(DWORD type, DWORD num, DWORD swz = kSwzXYZW) { return Reg(type, num) | (swz << 16); }
DWORD Op(DWORD op, DWORD len) { return op | (len << 24); }
DWORD F(float f) { DWORD d; memcpy(&d, &f, 4); return d; }

struct Ins {
    size_t at; // opcode token index; operands at at+1 .. at+len
    DWORD op;
    size_t len;
};

std::vector<Ins> Parse(const std::vector<DWORD>& t) {
    std::vector<Ins> v;
    for (size_t i = 1; i < t.size();) {
        const DWORD tok = t[i];
        if (tok == 0x0000FFFF) return v;
        if ((tok & 0xFFFF) == 0xFFFE) {
            i += 1 + ((tok >> 16) & 0x7FFF);
            continue;
        }
        const size_t len = (tok >> 24) & 0xF;
        if (i + len >= t.size()) return {};
        v.push_back({i, tok & 0xFFFF, len});
        i += 1 + len;
    }
    return {};
}

struct Usage {
    int maxTemp = -1, maxSampler = -1, maxConst = -1;
    size_t afterLastSamplerDcl = 0;
};

Usage Scan(const std::vector<DWORD>& t, const std::vector<Ins>& ins) {
    Usage u;
    for (const Ins& x : ins) {
        if (x.op == kDcl) {
            const DWORD r = t[x.at + 2];
            if (Type(r) == kSampler) {
                u.maxSampler = std::max(u.maxSampler, static_cast<int>(Num(r)));
                u.afterLastSamplerDcl = x.at + 1 + x.len;
            }
            continue;
        }
        if (x.op == kDef || x.op == kDefI || x.op == kDefB) {
            if (x.op == kDef) u.maxConst = std::max(u.maxConst, static_cast<int>(Num(t[x.at + 1])));
            continue;
        }
        for (size_t k = 1; k <= x.len; k++) {
            const DWORD r = t[x.at + k];
            if (!(r & 0x80000000u)) continue;
            if (Type(r) == kTemp) u.maxTemp = std::max(u.maxTemp, static_cast<int>(Num(r)));
            else if (Type(r) == kConst) u.maxConst = std::max(u.maxConst, static_cast<int>(Num(r)));
        }
    }
    return u;
}

struct Edit {
    size_t at;
    std::vector<DWORD> tok;
};

void Apply(std::vector<DWORD>& t, std::vector<Edit> edits) {
    std::stable_sort(edits.begin(), edits.end(), [](const Edit& a, const Edit& b) { return a.at > b.at; });
    for (const Edit& e : edits) t.insert(t.begin() + e.at, e.tok.begin(), e.tok.end());
}

bool IsReg(DWORD r, DWORD type, DWORD num) { return (r & 0x80000000u) && Type(r) == type && Num(r) == num; }
size_t End(const Ins& x) { return x.at + 1 + x.len; }
bool IsFlow(DWORD op) { return (op >= 0x19 && op <= 0x1E) || (op >= 0x26 && op <= 0x2D); }

// Lamp brightness (Night Lights: ground, roads and sidewalks): the game's lamp scale applied to a light map value in rL.
// The next instruction after ins[from] that reads rL.rgb, before rL.rgb is written again and before any flow control,
// must be "mul rY.xyz, rL, cK.x" or "mad rY.xyz, rL, cK.x, rZ" (no saturate, no source modifier), and cK must be read by
// no other instruction, never through relative addressing and not come from a def: then cK.x scales that light map term
// only. Returns K, or -1.
int LampScaleAfter(const std::vector<DWORD>& t, const std::vector<Ins>& ins, size_t from, DWORD L) {
    int K = -1;
    for (size_t j = from + 1; j < ins.size(); j++) {
        const Ins& y = ins[j];
        if (IsFlow(y.op)) return -1;
        bool reads = false;
        for (size_t k = 2; k <= y.len; k++) reads |= IsReg(t[y.at + k], kTemp, L) && Swz(t[y.at + k]) != kSwzW;
        if (reads) {
            if (!((y.op == kMul && y.len == 3) || (y.op == kMad && y.len == 4))) return -1;
            const DWORD d = t[y.at + 1], a = t[y.at + 2], c = t[y.at + 3];
            if (Type(d) != kTemp || WMask(d) != 0x7 || (d & 0x00100000u) || !IsReg(a, kTemp, L) || Swz(a) != kSwzXYZW || (a & 0x0F002000u) ||
                Type(c) != kConst || Swz(c) != kSwzX || (c & 0x0F002000u))
                return -1;
            if (y.op == kMad && IsReg(t[y.at + 4], kTemp, L)) return -1; // the light map added again, unscaled
            K = static_cast<int>(Num(c));
            break;
        }
        if (y.len >= 1 && IsReg(t[y.at + 1], kTemp, L) && (WMask(t[y.at + 1]) & 0x7)) return -1;
    }
    if (K < 0) return -1;
    int uses = 0;
    for (const Ins& x : ins) {
        if (x.op == kDcl || x.op == kDefI || x.op == kDefB) continue;
        if (x.op == kDef) {
            if (IsReg(t[x.at + 1], kConst, static_cast<DWORD>(K))) return -1;
            continue;
        }
        for (size_t k = 1; k <= x.len; k++) {
            const DWORD r = t[x.at + k];
            if (!(r & 0x80000000u) || Type(r) != kConst) continue;
            if (r & 0x2000) return -1; // relative addressing could reach cK
            uses += Num(r) == static_cast<DWORD>(K) ? 1 : 0;
        }
    }
    return uses == 1 ? K : -1;
}

// (2.15.3) The reach law's lamp sum held under 1 by its brightest channel, as v / max(1, max(v.r, v.g, v.b)): the lamps' hue
// stays where they are bright (a per-channel clamp turned a warm or coloured lamp white near its head). tmp.x is overwritten.
// rcp of 0 is +inf, which the saturate makes 1.
void AppendHueClamp(std::vector<DWORD>& v, DWORD q, DWORD tmp) {
    v.insert(v.end(), {Op(kMax, 3), Dst(kTemp, tmp, 0x1), Src(kTemp, q, kSwzX), Src(kTemp, q, 0x55),
                       Op(kMax, 3), Dst(kTemp, tmp, 0x1), Src(kTemp, tmp, kSwzX), Src(kTemp, q, 0xAA),
                       Op(0x06 /* rcp */, 2), Dst(kTemp, tmp, 0x1), Src(kTemp, tmp, kSwzX),
                       Op(kMov, 2), Dst(kTemp, tmp, 0x1, true), Src(kTemp, tmp, kSwzX),
                       Op(kMul, 3), Dst(kTemp, q, 0x7), Src(kTemp, q), Src(kTemp, tmp, kSwzX)});
}

// One per-pixel lamp of the object, fence and window patches (ps_3_0): A.xyz = l (pixel to lamp, normalised), A.w = d^2,
// B.x = the lamp's share before its cones; B.y, B.z and B.w are free after it. pos = the pixel's world position (negated
// here), nrm = its normal, cp = the lamp row (pos, 1/R^2), cS.w = the smallest d^2, cH.z = 1. One constant per instruction
// (native D3D9 refuses two).
//  - the first law: B.x = sat(N.l) x sat(1 - d^2/R^2)^2 (R = 1.2 sqrt(range), lot_light_bridge ReadLamp);
//  - reach (12/10, F7 20-31-19 / 20-31-21: a band and a column 5.5-9.8 m from a red post went black at R = 7.82 m while the
//    outdoor wall behind them, on the realistic reach law, was lit): the law of an outdoor wall texel (level_light_share
//    ReachShareImpl), B.x = sat(N.l) / d^2 x min(1, r0^2 / d^2 x sat(1 - (d^2/R^2)^2)^2), r0^2 at reachRow.<k % 4>, R from
//    the lamp row's 1/R^2; the colour row holds the game's weight (lamp_reach.h Gain). min(1, ..) differs from
//    ReachShareImpl's "1 up to r0" only just inside r0, by at most 1 - (1 - (r0/R)^4)^2: 0.6% for the 12/10 garden post
//    (3.5 / 15 m), 0.8% for any w of 28 or more, 2.5% for a weak lamp with both radii at their floors (2 / 6 m, w under 14),
//    where the wall texel need not be at its clamp (12/10 review: 0.98 against 1, about 3/255 at most).
void AppendLampLaw(std::vector<DWORD>& code, DWORD A, DWORD B, DWORD nrm, DWORD posNeg, DWORD cp, DWORD cS, DWORD cH, bool reach, DWORD reachRow, unsigned k) {
    constexpr DWORD kNeg = 0x01000000u, kRsq = 0x07, kMin = 0x0A;
    code.insert(code.end(), {Op(kAdd, 3), Dst(kTemp, A, 0x7), Src(kConst, cp), posNeg,             // l = lamp - pixel
                             Op(kDp3, 3), Dst(kTemp, A, 0x8), Src(kTemp, A), Src(kTemp, A),         // d^2
                             Op(kMax, 3), Dst(kTemp, A, 0x8), Src(kTemp, A, kSwzW), Src(kConst, cS, kSwzW),
                             Op(kRsq, 2), Dst(kTemp, B, 0x8), Src(kTemp, A, kSwzW),
                             Op(kMul, 3), Dst(kTemp, A, 0x7), Src(kTemp, A), Src(kTemp, B, kSwzW),  // normalize
                             Op(kDp3, 3), Dst(kTemp, B, 0x1, true), Src(kTemp, nrm), Src(kTemp, A), // sat(N.l)
                             // q = d^2 / R^2 (two steps: one constant per instruction, final review 30/09)
                             Op(kMul, 3), Dst(kTemp, B, 0x2), Src(kTemp, A, kSwzW), Src(kConst, cp, kSwzW)});
    if (!reach) {
        code.insert(code.end(), {Op(kAdd, 3), Dst(kTemp, B, 0x2, true), Src(kTemp, B, kSwzY) | kNeg, Src(kConst, cH, 0xAA /* 1 */),
                                 Op(kMul, 3), Dst(kTemp, B, 0x2), Src(kTemp, B, kSwzY), Src(kTemp, B, kSwzY),
                                 Op(kMul, 3), Dst(kTemp, B, 0x1), Src(kTemp, B, kSwzX), Src(kTemp, B, kSwzY)});
        return;
    }
    const DWORD comp = (k & 3u) * 0x55u; // .x .y .z .w replicated
    code.insert(code.end(), {Op(kMad, 4), Dst(kTemp, B, 0x2, true), Src(kTemp, B, kSwzY) | kNeg, Src(kTemp, B, kSwzY), Src(kConst, cH, 0xAA), // sat(1 - q^2)
                             Op(kMul, 3), Dst(kTemp, B, 0x2), Src(kTemp, B, kSwzY), Src(kTemp, B, kSwzY),                                    // its square
                             Op(kMul, 3), Dst(kTemp, B, 0x8), Src(kTemp, B, kSwzW), Src(kTemp, B, kSwzW),                                    // 1 / d^2
                             Op(kMul, 3), Dst(kTemp, B, 0x4), Src(kTemp, B, kSwzW), Src(kConst, reachRow + (k >> 2), comp),                 // r0^2 / d^2
                             Op(kMul, 3), Dst(kTemp, B, 0x4), Src(kTemp, B, 0xAA), Src(kTemp, B, kSwzY),
                             Op(kMin, 3), Dst(kTemp, B, 0x4), Src(kTemp, B, 0xAA), Src(kConst, cH, 0xAA),                                    // reach share
                             Op(kMul, 3), Dst(kTemp, B, 0x1), Src(kTemp, B, kSwzX), Src(kTemp, B, kSwzW),                                    // sat(N.l) / d^2
                             Op(kMul, 3), Dst(kTemp, B, 0x1), Src(kTemp, B, kSwzX), Src(kTemp, B, 0xAA)});
}


} // namespace

namespace ShaderPatches {

int BloomThresholdConst(const std::vector<DWORD>& t) {
    if (t.empty() || (t[0] & 0xFFFF0000u) != 0xFFFF0000u) return -1;
    const auto ins = Parse(t);
    if (ins.empty()) return -1;

    int threshold = -1, matches = 0, alphaWrites = 0;
    for (const Ins& x : ins) {
        if (x.op == kDcl || x.op == kDef || x.op == kDefI || x.op == kDefB || x.len < 1) continue;
        const DWORD dst = t[x.at + 1];
        if (!IsReg(dst, kColorOut, 0) || !(WMask(dst) & 0x8)) continue;
        alphaWrites++;

        // Captured TS3 bloom tail: add_sat oC0.w, rL.w, -cK.x
        if (x.op != kAdd || x.len != 3 || WMask(dst) != 0x8 || !(dst & 0x00100000u)) continue;
        const DWORD a = t[x.at + 2], b = t[x.at + 3];
        if (Type(a) != kTemp || Swz(a) != kSwzW || (a & 0x0F002000u)) continue;
        if (Type(b) != kConst || Swz(b) != kSwzX || (b & 0x2000u) || (b & 0x0F000000u) != 0x01000000u) continue;
        threshold = static_cast<int>(Num(b));
        matches++;
    }
    if (alphaWrites != 1 || matches != 1 || threshold < 0) return -1;

    int uses = 0;
    for (const Ins& x : ins) {
        if (x.op == kDef && IsReg(t[x.at + 1], kConst, static_cast<DWORD>(threshold))) return -1;
        if (x.op == kDcl || x.op == kDefI || x.op == kDefB) continue;
        for (size_t k = 1; k <= x.len; k++) {
            const DWORD r = t[x.at + k];
            if (!(r & 0x80000000u) || Type(r) != kConst) continue;
            if (r & 0x2000u) return -1;
            if (Num(r) == static_cast<DWORD>(threshold)) uses++;
        }
    }
    return uses == 1 ? threshold : -1;
}

bool IsRoadVs(const std::vector<DWORD>& t, DWORD& mapConst) {
    if (t.empty() || t[0] != 0xFFFE0300) return false;
    const auto ins = Parse(t);
    if (ins.empty()) return false;
    int tc1 = -1;
    DWORD tc1Mask = 0;
    bool c8 = false, c10 = false;
    for (const Ins& x : ins) {
        if (x.op == kDcl) {
            const DWORD use = t[x.at + 1] & 0x1F, idx = (t[x.at + 1] >> 16) & 0xF, r = t[x.at + 2];
            if (Type(r) == kOutput && use == 5 && idx == 1) {
                if (tc1 >= 0 || (WMask(r) != 0x3 && WMask(r) != 0xF)) return false;
                tc1 = static_cast<int>(Num(r));
                tc1Mask = WMask(r);
            }
            continue;
        }
        if (x.op == 0x09 && x.len == 3) { // dp4 rW, rA, cK: world matrix rows
            if (IsReg(t[x.at + 3], kConst, 8)) c8 = true;
            if (IsReg(t[x.at + 3], kConst, 10)) c10 = true;
        }
    }
    if (tc1 < 0 || !c8 || !c10) return false;
    int found = 0;
    int alphaUv = 0;
    for (const Ins& x : ins) {
        if (tc1Mask == 0xF && x.op != kDcl && x.len >= 1 && IsReg(t[x.at + 1], kOutput, static_cast<DWORD>(tc1))) {
            // Alpha-blended sidewalks pack the opacity UV into zw beside the terrain UV in xy.
            if (WMask(t[x.at + 1]) == 0xC && x.op == kMul && x.len == 3 &&
                Type(t[x.at + 2]) == kConst && Swz(t[x.at + 2]) == 0xC4 && !(t[x.at + 2] & 0x0F002000u) &&
                Type(t[x.at + 3]) == kInput && Swz(t[x.at + 3]) == kSwzXYXY && !(t[x.at + 3] & 0x0F002000u)) {
                bool unitScale = false, textureUv = false;
                for (const Ins& d : ins) {
                    if (d.op == kDef && d.len == 5 && IsReg(t[d.at + 1], kConst, Num(t[x.at + 2])))
                        unitScale = t[d.at + 2] == F(1.0f) && t[d.at + 3] == F(2.0f);
                    if (d.op == kDcl && d.len == 2 && IsReg(t[d.at + 2], kInput, Num(t[x.at + 3])))
                        textureUv = (t[d.at + 1] & 0x1F) == 5;
                }
                if (!unitScale || !textureUv) return false;
                alphaUv++;
                continue;
            }
            if (x.op != kMad || x.len != 4 || WMask(t[x.at + 1]) != 0x3) return false;
        }
        // mad oT1.xy, rA.xzzw, cM, cM.zwzw
        if (x.op != kMad || !IsReg(t[x.at + 1], kOutput, static_cast<DWORD>(tc1)) || WMask(t[x.at + 1]) != 0x3) continue;
        if (Type(t[x.at + 2]) != kTemp || Swz(t[x.at + 2]) != 0xE8 || Type(t[x.at + 3]) != kConst || Swz(t[x.at + 3]) != kSwzXYZW ||
            !IsReg(t[x.at + 4], kConst, Num(t[x.at + 3])) || Swz(t[x.at + 4]) != kSwzZWZW)
            return false;
        mapConst = Num(t[x.at + 3]);
        found++;
    }
    return found == 1 && (tc1Mask == 0x3 || alphaUv == 1);
}

bool PatchRoad(std::vector<DWORD>& t, RoadPatch& out, bool shade) {
    if (t.empty() || t[0] != 0xFFFF0300) return false;
    const auto ins = Parse(t);
    if (ins.empty()) return false;
    const Usage u = Scan(t, ins);
    // light map fetch: "texld rX, v1, sL" whose result is next used by the lamp scale "mul rY.xyz, rX, cN.x" (winter: right
    // after it with rY = rX and c4.x; summer: a few instructions later, rY != rX, c3.x), before rX is written again.
    int light = -1;
    for (size_t i = 0; i < ins.size() && light < 0; i++) {
        const Ins& x = ins[i];
        if (x.op != kTexld || Type(t[x.at + 1]) != kTemp || !IsReg(t[x.at + 2], kInput, 1) || Type(t[x.at + 3]) != kSampler) continue;
        const DWORD X = Num(t[x.at + 1]);
        for (size_t j = i + 1; j < ins.size() && j <= i + 8; j++) {
            const Ins& m = ins[j];
            if (m.op == kMul && Type(t[m.at + 1]) == kTemp && (WMask(t[m.at + 1]) & 0x7) == 0x7 && IsReg(t[m.at + 2], kTemp, X) &&
                Swz(t[m.at + 2]) == kSwzXYZW && Type(t[m.at + 3]) == kConst && Swz(t[m.at + 3]) == kSwzX) {
                light = static_cast<int>(i);
                break;
            }
            bool uses = false; // any other use of rX first: not the light map
            for (size_t k = 2; k <= m.len; k++)
                if (IsReg(t[m.at + k], kTemp, X)) uses = true;
            if (uses || (m.len >= 1 && IsReg(t[m.at + 1], kTemp, X) && (WMask(t[m.at + 1]) & 0x7))) break; // rX.xyz overwritten (rX.w is fine)
        }
    }
    if (light < 0 || u.maxSampler < 0 || u.maxSampler >= (shade ? 14 : 15) || !u.afterLastSamplerDcl) return false;
    const Ins& L = ins[light];
    const DWORD X = Num(t[L.at + 1]);
    const DWORD E = static_cast<DWORD>(u.maxSampler + 1);
    const DWORD T0 = static_cast<DWORD>(u.maxTemp + 1);
    out.lightSampler = Num(t[L.at + 3]);
    out.extraSampler = E;
    out.sidewalkConst = -1;
    out.shadeSampler = shade ? static_cast<int>(E + 1) : -1;
    // the lamp scale after the max inserted below (c4.x in the 4 winter variants, c3.x in summer; each read once)
    out.scaleConst = LampScaleAfter(t, ins, static_cast<size_t>(light), X);

    std::vector<Edit> edits;
    edits.push_back({u.afterLastSamplerDcl, {Op(kDcl, 2), 0x90000000u, Dst(kSampler, E)}});
    Edit lightEdit{End(L), {Op(kTexld, 3), Dst(kTemp, T0), Src(kInput, 1), Src(kSampler, E),
                            Op(kMax, 3), Dst(kTemp, X, 0x7), Src(kTemp, X), Src(kTemp, T0)}};
    if (shade) { // (in the same edit: edits at one place are inserted in reverse order)
        edits.push_back({u.afterLastSamplerDcl, {Op(kDcl, 2), 0x90000000u, Dst(kSampler, E + 1)}});
        lightEdit.tok.insert(lightEdit.tok.end(), {Op(kTexld, 3), Dst(kTemp, T0), Src(kInput, 1), Src(kSampler, E + 1),
                                                   Op(kMul, 3), Dst(kTemp, X, 0x7), Src(kTemp, X), Src(kTemp, T0, kSwzX)});
    }
    edits.push_back(std::move(lightEdit));

    // Sidewalk under snow: "mul r1.w, rA.x, rA.y" (brightness of the road texture rA) ... "lrp r1.xyz, v2.w, r0, r3" (snow).
    int albedo = -1, blend = -1;
    for (size_t i = 0; i < ins.size(); i++) {
        const Ins& x = ins[i];
        if (albedo < 0 && x.op == kMul && IsReg(t[x.at + 1], kTemp, 1) && WMask(t[x.at + 1]) == 0x8 && Type(t[x.at + 2]) == kTemp &&
            Swz(t[x.at + 2]) == kSwzX && Type(t[x.at + 3]) == kTemp && Num(t[x.at + 3]) == Num(t[x.at + 2]) && Swz(t[x.at + 3]) == kSwzY)
            albedo = static_cast<int>(i);
        if (blend < 0 && x.op == kLrp && IsReg(t[x.at + 1], kTemp, 1) && WMask(t[x.at + 1]) == 0x7 && IsReg(t[x.at + 2], kInput, 2) && Swz(t[x.at + 2]) == kSwzW)
            blend = static_cast<int>(i);
    }
    if (albedo >= 0 && blend > albedo && u.maxConst + 3 < 224) {
        const DWORD A = Num(t[ins[albedo].at + 2]);
        const DWORD T1 = T0 + 1, T2 = T0 + 2;
        const DWORD cS = static_cast<DWORD>(u.maxConst + 1), cL = cS + 1, cK = cS + 2;
        out.sidewalkConst = static_cast<int>(cS);
        edits.push_back({1, {Op(kDef, 5), Dst(kConst, cL), F(0.3f), F(0.59f), F(0.11f), F(0.0f),
                             Op(kDef, 5), Dst(kConst, cK), F(4.0f), F(-1.0f), F(0.0f), F(0.0f)}});
        edits.push_back({ins[albedo].at, {Op(kMov, 2), Dst(kTemp, T1), Src(kTemp, A)}});
        edits.push_back({End(ins[blend]), {Op(kDp3, 3), Dst(kTemp, T1, 0x8), Src(kTemp, T1), Src(kConst, cL),
                                           Op(kMad, 4), Dst(kTemp, T1, 0x8, true), Src(kTemp, T1, kSwzW), Src(kConst, cK, kSwzX), Src(kConst, cK, kSwzY),
                                           Op(kMul, 3), Dst(kTemp, T1, 0x8), Src(kTemp, T1, kSwzW), Src(kConst, cS, kSwzX),
                                           Op(kLrp, 4), Dst(kTemp, T2, 0x7), Src(kTemp, T1, kSwzW), Src(kTemp, T1), Src(kTemp, 1),
                                           Op(kMov, 2), Dst(kTemp, 1, 0x7), Src(kTemp, T2)}});
    }
    Apply(t, std::move(edits));
    return true;
}

// World terrain chunks (LightProbe-mundo PS_28CD3DB0: "texld_pp r0, v1, s8" ... "mul_pp r7.xyz, r0, c7.x"; winter
// LightProbe-m05 PS_295B4ED0: "texld_pp r0, v1, s11" ... "mul_pp r0.xyz, r0, c7.x", with r0.w used in between).
int LightMapScaleConst(const std::vector<DWORD>& t, DWORD sampler, bool* squared) {
    if (squared) *squared = false;
    if (t.empty() || (t[0] & 0xFFFF0000u) != 0xFFFF0000u || ((t[0] >> 8) & 0xFF) < 2) return -1;
    const auto ins = Parse(t);
    if (ins.empty()) return -1;
    int fetch = -1, reads = 0;
    for (size_t i = 0; i < ins.size(); i++) {
        const Ins& x = ins[i];
        if (x.op == kDcl) continue;
        bool usesSampler = false;
        for (size_t k = 1; k <= x.len; k++) usesSampler |= IsReg(t[x.at + k], kSampler, sampler);
        if (!usesSampler) continue;
        reads++;
        if (x.op == kTexld && x.len == 3 && Type(t[x.at + 1]) == kTemp && (Type(t[x.at + 2]) == kInput || Type(t[x.at + 2]) == kTexture)) fetch = static_cast<int>(i);
    }
    if (reads != 1 || fetch < 0) return -1;
    const DWORD light = Num(t[ins[fetch].at + 1]);
    const int linear = LampScaleAfter(t, ins, static_cast<size_t>(fetch), light);
    if (linear >= 0) return linear;
    // The captured winter multipass material squares its lamp-only c4.x into
    // a temporary before scaling RGB. Trace that exact dependency, never infer
    // the exponent from the number of material passes or the shader's name.
    const Ins* square = nullptr;
    int constant = -1;
    for (size_t j = static_cast<size_t>(fetch) + 1; j < ins.size(); ++j) {
        const Ins& x = ins[j];
        if (IsFlow(x.op)) return -1;
        bool readsLight = false;
        for (size_t k = 2; k <= x.len; ++k)
            readsLight |= IsReg(t[x.at + k], kTemp, light) && Swz(t[x.at + k]) != kSwzW;
        if (readsLight) {
            if (!square || x.op != kMul || x.len != 3 || Type(t[x.at+1]) != kTemp ||
                WMask(t[x.at+1]) != 7 || (t[x.at+1] & 0x00100000u) ||
                !IsReg(t[x.at+2], kTemp, light) || Swz(t[x.at+2]) != kSwzXYZW || (t[x.at+2] & 0x0F002000u)) return -1;
            const DWORD factor = t[x.at+3], dst = t[square->at+1];
            const DWORD mask = WMask(dst);
            if (!IsReg(factor, kTemp, Num(dst)) || (factor & 0x0F002000u)) return -1;
            const DWORD component = Swz(factor) & 3;
            if (Swz(factor) != component * 0x55u || mask != (1u << component)) return -1;
            int uses = 0;
            for (const Ins& y : ins) {
                if (y.op == kDcl || y.op == kDefI || y.op == kDefB) continue;
                if (y.op == kDef && IsReg(t[y.at+1], kConst, static_cast<DWORD>(constant))) return -1;
                for (size_t k = 2; k <= y.len; ++k) {
                    const DWORD r = t[y.at+k];
                    if (Type(r) == kConst) {
                        if (r & 0x2000u) return -1;
                        if (Num(r) == static_cast<DWORD>(constant)) ++uses;
                    }
                }
            }
            if (uses != 2) return -1;
            if (squared) *squared = true;
            return constant;
        }
        if (x.len >= 1 && IsReg(t[x.at+1], kTemp, light) && (WMask(t[x.at+1]) & 7)) return -1;
        if (square && x.len >= 1 && IsReg(t[x.at+1], kTemp, Num(t[square->at+1])) &&
            (WMask(t[x.at+1]) & WMask(t[square->at+1]))) { square = nullptr; constant = -1; }
        if (x.op == kMul && x.len == 3 && Type(t[x.at+1]) == kTemp && !(t[x.at+1] & 0x00100000u) &&
            Type(t[x.at+2]) == kConst && Swz(t[x.at+2]) == kSwzX && !(t[x.at+2] & 0x0F002000u) &&
            t[x.at+2] == t[x.at+3]) {
            square = &x;
            constant = static_cast<int>(Num(t[x.at+2]));
        }
    }
    return -1;
}

bool PatchTerrainNativeAlpha(std::vector<DWORD>& t, DWORD sampler, DWORD& extraSampler) {
    if (t.empty() || (t[0] != 0xFFFF0200 && t[0] != 0xFFFF0300) || sampler >= 16) return false;
    const auto ins = Parse(t);
    if (ins.empty()) return false;
    // Scan assumes normal instruction sizes. Reject malformed declarations first.
    for (const Ins& x : ins)
        if ((x.op == kDcl && x.len != 2) || ((x.op == kDef || x.op == kDefI) && x.len != 5) ||
            (x.op == kDefB && x.len != 2) || IsFlow(x.op) || (t[x.at] & 0x10000000u)) return false;
    const Usage u = Scan(t, ins);
    const int tempLimit = t[0] == 0xFFFF0200 ? 12 : 32;
    if (u.maxTemp + 1 >= tempLimit || u.maxSampler + 1 >= 16 || !u.afterLastSamplerDcl) return false;
    const Ins* fetch = nullptr;
    bool declared = false;
    for (const Ins& x : ins) {
        if (x.op == kDcl) {
            if (IsReg(t[x.at + 2], kSampler, sampler)) {
                if ((t[x.at + 1] & 0x78000000u) != 0x10000000u || declared) return false; // 2D only
                declared = true;
            }
            continue;
        }
        if (x.op == kDef || x.op == kDefI || x.op == kDefB) continue;
        for (size_t k = 1; k <= x.len; ++k) {
            const DWORD r = t[x.at + k];
            if ((r & 0x80000000u) && (r & 0x2000u)) return false; // relative addressing
            if (!IsReg(r, kSampler, sampler)) continue;
            if (fetch || x.op != kTexld || x.len != 3 || k != 3 || (t[x.at] & 0x00FF0000u)) return false;
            fetch = &x;
        }
    }
    if (!declared || !fetch) return false;
    const DWORD dest = t[fetch->at + 1], uv = t[fetch->at + 2];
    if (Type(dest) != kTemp || WMask(dest) != 0xF || (dest & 0x00100000u) ||
        (Type(uv) != kInput && Type(uv) != kTexture) || (uv & 0x0F002000u) || Swz(uv) != kSwzXYZW) return false;
    const DWORD spare = static_cast<DWORD>(u.maxSampler + 1), temp = static_cast<DWORD>(u.maxTemp + 1);
    // Match the original read's partial precision, then replace only alpha.
    Apply(t, {{u.afterLastSamplerDcl, {Op(kDcl, 2), 0x90000000u, Dst(kSampler, spare)}},
              {End(*fetch), {Op(kTexld, 3), Dst(kTemp, temp) | (dest & 0x00200000u), uv, Src(kSampler, spare),
                             Op(kMov, 2), Dst(kTemp, Num(dest), 8), Src(kTemp, temp, kSwzW)}}});
    extraSampler = spare;
    return true;
}

bool PatchTerrainShade(std::vector<DWORD>& t, DWORD sampler, DWORD& shadeSampler) {
    if (t.empty() || (t[0] != 0xFFFF0200 && t[0] != 0xFFFF0300) || sampler >= 16) return false;
    const auto ins = Parse(t);
    if (ins.empty()) return false;
    for (const Ins& x : ins)
        if ((x.op == kDcl && x.len != 2) || ((x.op == kDef || x.op == kDefI) && x.len != 5) ||
            (x.op == kDefB && x.len != 2) || IsFlow(x.op) || (t[x.at] & 0x10000000u)) return false;
    const Usage u = Scan(t, ins);
    const int tempLimit = t[0] == 0xFFFF0200 ? 12 : 32;
    if (u.maxTemp + 1 >= tempLimit || u.maxSampler + 1 >= 16 || !u.afterLastSamplerDcl) return false;
    const Ins* fetch = nullptr;
    for (const Ins& x : ins) {
        if (x.op != kTexld || x.len != 3 || !IsReg(t[x.at + 3], kSampler, sampler)) continue;
        if (fetch) return false; // one read only
        fetch = &x;
    }
    if (!fetch || (t[fetch->at] & 0x00FF0000u)) return false;
    const DWORD dest = t[fetch->at + 1], uv = t[fetch->at + 2];
    if (Type(dest) != kTemp || WMask(dest) != 0xF || (dest & 0x00100000u) ||
        (Type(uv) != kInput && Type(uv) != kTexture) || (uv & 0x0F002000u) || Swz(uv) != kSwzXYZW) return false;
    const DWORD S = static_cast<DWORD>(u.maxSampler + 1), T = static_cast<DWORD>(u.maxTemp + 1), pp = dest & 0x00200000u;
    Apply(t, {{u.afterLastSamplerDcl, {Op(kDcl, 2), 0x90000000u, Dst(kSampler, S)}},
              {End(*fetch), {Op(kTexld, 3), Dst(kTemp, T) | pp, uv, Src(kSampler, S),
                             Op(kMul, 3), Dst(kTemp, Num(dest), 0x7) | pp, Src(kTemp, Num(dest)), Src(kTemp, T, kSwzX)}}});
    shadeSampler = S;
    return true;
}

bool PatchTerrainDaylightRange(std::vector<DWORD>& t, DWORD& blendConst) {
    // Exact captured native-alpha variant plus the light accumulator pattern.
    // Unstudied variants keep their existing shader, including compact terrain.
    if (t.size() != 387 || t[0] != 0xFFFF0300) return false;
    DWORD hash = 2166136261u;
    for (DWORD word : t) hash = (hash ^ word) * 16777619u;
    if (hash != 0x3A0A3E52u) return false;
    const auto ins = Parse(t);
    const Usage u = Scan(t, ins);
    if (u.maxTemp + 2 >= 32 || u.maxConst >= 223) return false;
    const Ins* sum = nullptr;
    for (const Ins& x : ins) {
        if (x.op == kMad && x.len == 4 && IsReg(t[x.at+1], kTemp, 1) && WMask(t[x.at+1]) == 7 &&
            IsReg(t[x.at+2], kTemp, 1) && IsReg(t[x.at+3], kConst, 8) && Swz(t[x.at+3]) == kSwzX &&
            IsReg(t[x.at+4], kTemp, 5)) {
            if (sum) return false;
            sum = &x;
        }
    }
    if (!sum) return false;
    const DWORD tmp = static_cast<DWORD>(u.maxTemp + 1);
    Apply(t, {{End(*sum), {Op(0x0A, 3), Dst(kTemp, tmp, 7), Src(kTemp, 1), Src(kConst, 223, kSwzY),
                          Op(kLrp, 4), Dst(kTemp, tmp + 1, 7), Src(kConst, 223, kSwzX), Src(kTemp, tmp), Src(kTemp, 1),
                          Op(kMov, 2), Dst(kTemp, 1, 7), Src(kTemp, tmp + 1)}}});
    blendConst = 223;
    return true;
}

namespace {
// Winter floors (PatchFloor, PatchSnowFloor): the atlas weighted by the floor map's alpha, as PatchBakedAtlasPs does for the
// summer floors (level_light_share stores there, for outdoor floor texels, the share of the lamps' light that walls block;
// 1 on a closed yard's floor). The alpha is copied right after the map's texld (a write to L.w may follow it: the m69 snow
// floor), then weight = saturate(alpha * cH.y + cH.x) ((1, 0) = always 1) scales the atlas read into temp T before the max.
// Only when the texld writes .w and a temp and a constant are free; out.heightConst / out.maskAlpha tell the draw to set cH.
void AtlasWeight(const std::vector<DWORD>& t, const Ins& fetch, DWORD T, DWORD cA, const Usage& u, std::vector<Edit>& edits, std::vector<DWORD>& atlas, FloorPatch& out) {
    out.maskAlpha = false;
    out.heightConst = -1;
    const DWORD dest = t[fetch.at + 1];
    if (!(WMask(dest) & 0x8) || u.maxConst + 2 >= 224 || u.maxTemp + 2 >= 32) return;
    const DWORD L = Num(dest), T2 = T + 1, cH = cA + 1;
    edits.push_back({End(fetch), {Op(kMov, 2), Dst(kTemp, T2, 0x1), Src(kTemp, L, kSwzW)}});
    atlas.insert(atlas.end(), {Op(kMad, 4), Dst(kTemp, T2, 0x1, true), Src(kTemp, T2, kSwzX), Src(kConst, cH, kSwzY), Src(kConst, cH, kSwzX),
                               Op(kMul, 3), Dst(kTemp, T, 0x7), Src(kTemp, T), Src(kTemp, T2, kSwzX)});
    out.maskAlpha = true;
    out.heightConst = static_cast<int>(cH);
}

// Ground lamp shade of story-0 outdoor floors (13/10, F7 2026-10-13 08-16-00: a paved path behind a free-standing wall was
// fully lit by a garden post on the other side while the grass beside it was shaded). Story 0's floor map is room 0's solve,
// which tests no wall for floor texels, and the atlas knows no walls either; the lot pass multiplies the grass's whole lamp
// term by the lot's lamp shade M (ground_lamp_shadow.h, the lot mask's alpha). Appended after the max into rB:
//   rB.xyz *= 1 + gate (M - 1), M = mask.a at uv = x cM.xy + z cM1.xy + cM.zw,
//   gate = sat(|y + cM1.z| cM2.y + cM2.x) (1 within about 1 m of the lot's ground, 0 on the stories above), cM1.w = 1.
// x, z, y: source tokens (an input component, replicated). T, T2: free temps. One constant per instruction.
void AppendGroundShade(std::vector<DWORD>& code, DWORD B, DWORD T, DWORD T2, DWORD x, DWORD z, DWORD y, DWORD cM, DWORD S) {
    constexpr DWORD kNeg = 0x01000000u, kAbs = 0x0B000000u, kXyyy = 0x54, kZwww = 0xFE, kZzzz = 0xAA;
    code.insert(code.end(), {Op(kMad, 4), Dst(kTemp, T, 0x3), x, Src(kConst, cM, kXyyy), Src(kConst, cM, kZwww),           // uv from x
                             Op(kMad, 4), Dst(kTemp, T, 0x3), z, Src(kConst, cM + 1, kXyyy), Src(kTemp, T),               // + from z
                             Op(kTexld, 3), Dst(kTemp, T), Src(kTemp, T), Src(kSampler, S),                                // M = T.w
                             Op(kAdd, 3), Dst(kTemp, T2, 0x1), y, Src(kConst, cM + 1, kZzzz),                              // y - ground
                             Op(kMad, 4), Dst(kTemp, T2, 0x1, true), Src(kTemp, T2, kSwzX) | kAbs, Src(kConst, cM + 2, kSwzY), Src(kConst, cM + 2, kSwzX),
                             Op(kAdd, 3), Dst(kTemp, T, 0x8), Src(kTemp, T, kSwzW), Src(kConst, cM + 1, kSwzW) | kNeg,     // M - 1
                             Op(kMad, 4), Dst(kTemp, T2, 0x1), Src(kTemp, T2, kSwzX), Src(kTemp, T, kSwzW), Src(kConst, cM + 1, kSwzW),
                             Op(kMul, 3), Dst(kTemp, B, 0x7), Src(kTemp, B), Src(kTemp, T2, kSwzX)});
}

// (2.15.3) Indoor light on the ground of story-0 floors (LevelLightShare::SetIndoorLightOnGround): the floor map's alpha a of
// room 0 on story 0 holds E, the luminance of the light lit rooms send onto the texel through their openings (1 = a roofless
// room's own floor, a share of walls' blocked light on the stories above). It replaces the atlas read, the max and the ground
// shade: with the per-pixel ground gate g of AppendGroundShade (1 within about 1 m of the lot's ground, 0 above),
//   tw = sat(sat-less(a cH.y + cH.x) (1 + 254 g)),  s = g sat(a / max(lum(map), 1/255)),  ex = B s,
//   B = max(B - ex, atlas tw) (1 + g (M - 1)) + ex.
// At g = 0 (the stories above, a draw with no lot) it is the block of before: the atlas weight sat(a cH.y + cH.x), no ex, the
// shade 1. On the ground a <= 254/255 leaves the atlas whole (a = 1 none, as before). a, lum: replicated sources (the summer
// floors read the map's own .w and a dp3 into Q.w; the winter floors copies taken right after the map's texld). uv: the
// atlas uv source (with cA as before); x, z, y: world sources. Temps P (mask, shade), Q (g, weights), R (atlas), U (ex);
// constants cA, cH, cM .. cM + 2 as before, cX = (lum weights, 1/255), cY = (254, 1) (ExitDefs). One constant per instruction.
void AppendExitGround(std::vector<DWORD>& code, DWORD B, DWORD a, DWORD lum, DWORD uv, DWORD P, DWORD Q, DWORD R, DWORD U, DWORD x, DWORD z, DWORD y,
                      DWORD cA, DWORD cH, DWORD cM, DWORD cX, DWORD cY, DWORD E, DWORD S) {
    constexpr DWORD kNeg = 0x01000000u, kAbs = 0x0B000000u, kXyyy = 0x54, kZwww = 0xFE, kZzzz = 0xAA, kRcpOp = 0x06;
    code.insert(code.end(), {Op(kMad, 4), Dst(kTemp, P, 0x3), x, Src(kConst, cM, kXyyy), Src(kConst, cM, kZwww),           // mask uv from x
                             Op(kMad, 4), Dst(kTemp, P, 0x3), z, Src(kConst, cM + 1, kXyyy), Src(kTemp, P),               // + from z
                             Op(kTexld, 3), Dst(kTemp, P), Src(kTemp, P), Src(kSampler, S),                                // M = P.w
                             Op(kAdd, 3), Dst(kTemp, Q, 0x1), y, Src(kConst, cM + 1, kZzzz),                               // y - ground
                             Op(kMad, 4), Dst(kTemp, Q, 0x1, true), Src(kTemp, Q, kSwzX) | kAbs, Src(kConst, cM + 2, kSwzY), Src(kConst, cM + 2, kSwzX), // g
                             Op(kAdd, 3), Dst(kTemp, P, 0x8), Src(kTemp, P, kSwzW), Src(kConst, cM + 1, kSwzW) | kNeg,     // M - 1
                             Op(kMad, 4), Dst(kTemp, P, 0x8), Src(kTemp, Q, kSwzX), Src(kTemp, P, kSwzW), Src(kConst, cM + 1, kSwzW), // 1 + g (M - 1)
                             Op(kMad, 4), Dst(kTemp, Q, 0x2), a, Src(kConst, cH, kSwzY), Src(kConst, cH, kSwzX),          // a cH.y + cH.x
                             Op(kMad, 4), Dst(kTemp, Q, 0x4), Src(kTemp, Q, kSwzX), Src(kConst, cY, kSwzX), Src(kConst, cY, kSwzY), // 1 + 254 g
                             Op(kMul, 3), Dst(kTemp, Q, 0x2, true), Src(kTemp, Q, kSwzY), Src(kTemp, Q, kZzzz),           // tw
                             Op(kMax, 3), Dst(kTemp, Q, 0x4), lum, Src(kConst, cX, kSwzW),                                 // max(lum, 1/255)
                             Op(kRcpOp, 2), Dst(kTemp, Q, 0x4), Src(kTemp, Q, kZzzz),
                             Op(kMul, 3), Dst(kTemp, Q, 0x4, true), a, Src(kTemp, Q, kZzzz),                               // sat(a / lum)
                             Op(kMul, 3), Dst(kTemp, Q, 0x4), Src(kTemp, Q, kZzzz), Src(kTemp, Q, kSwzX),                  // s = g sat(a / lum)
                             Op(kMad, 4), Dst(kTemp, R, 0x3), uv, Src(kConst, cA), Src(kConst, cA, kSwzZWZW),             // atlas uv
                             Op(kTexld, 3), Dst(kTemp, R), Src(kTemp, R), Src(kSampler, E),
                             Op(kMul, 3), Dst(kTemp, R, 0x7), Src(kTemp, R), Src(kTemp, Q, kSwzY),                         // atlas tw
                             Op(kMul, 3), Dst(kTemp, U, 0x7), Src(kTemp, B), Src(kTemp, Q, kZzzz),                         // ex
                             Op(kAdd, 3), Dst(kTemp, B, 0x7), Src(kTemp, B), Src(kTemp, U) | kNeg,
                             Op(kMax, 3), Dst(kTemp, B, 0x7), Src(kTemp, B), Src(kTemp, R),
                             Op(kMul, 3), Dst(kTemp, B, 0x7), Src(kTemp, B), Src(kTemp, P, kSwzW),                         // the shade
                             Op(kAdd, 3), Dst(kTemp, B, 0x7), Src(kTemp, B), Src(kTemp, U)});                              // + ex
}
std::vector<DWORD> ExitDefs(DWORD cX, DWORD cY) {
    return {Op(kDef, 5), Dst(kConst, cX), F(0.2126f), F(0.7152f), F(0.0722f), F(1.0f / 255.0f),
            Op(kDef, 5), Dst(kConst, cY), F(254.0f), F(1.0f), F(0.0f), F(0.0f)};
}

// The input register that holds TEXCOORDn.c in a pixel shader: its declaration widened to the whole register (and to full
// precision: world coordinates) when it does not read c, or a new input maxIn + 1 declared after the last one. False when no
// input register is left (10 in ps_3_0). firstNew: the lowest register a new input may take (one another edit declares).
bool InputFor(std::vector<DWORD>& t, const std::vector<Ins>& ins, int texcoord, std::vector<Edit>& edits, DWORD& reg, int firstNew = 0) {
    int maxIn = -1;
    size_t afterLastInDcl = 0;
    for (const Ins& x : ins) {
        if (x.op != kDcl || Type(t[x.at + 2]) != kInput) continue;
        if ((t[x.at + 1] & 0x1F) == 5 && ((t[x.at + 1] >> 16) & 0xF) == static_cast<DWORD>(texcoord)) {
            t[x.at + 2] = (t[x.at + 2] & ~0x002F0000u) | 0x000F0000u; // .xyzw, no _pp
            reg = Num(t[x.at + 2]);
            return true;
        }
        maxIn = std::max(maxIn, static_cast<int>(Num(t[x.at + 2])));
        afterLastInDcl = End(x);
    }
    maxIn = std::max(maxIn, firstNew - 1);
    if (maxIn < 0 || maxIn >= 9 || !afterLastInDcl) return false;
    reg = static_cast<DWORD>(maxIn + 1);
    edits.push_back({afterLastInDcl, {Op(kDcl, 2), 0x80000005u | (static_cast<DWORD>(texcoord) << 16), Dst(kInput, reg)}});
    return true;
}
} // namespace

namespace {
// The ground lamp shade for the winter floors (PatchFloor, PatchSnowFloor): world y from the vertex shader's output y (its
// TEXCOORD and component, WorldYOutput), world x / z at the given sources; a sampler after E and cA + 2 .. cA + 4. Nothing
// (out.shadeSampler -1) without y or without room.
void WinterShade(std::vector<DWORD>& t, const std::vector<Ins>& ins, const Usage& u, const WorldY& y, DWORD B, DWORD T, DWORD E, DWORD cA, DWORD x,
                 DWORD z, std::vector<Edit>& edits, std::vector<DWORD>& code, FloorPatch& out, int firstNew = 0) {
    out.shadeSampler = out.shadeConst = -1;
    if (y.texcoord < 0 || y.texcoord > 15 || y.comp < 0 || y.comp > 3 || E + 1 > 15 || cA + 4 >= 224 || u.maxTemp + 2 >= 32) return;
    DWORD reg = 0;
    if (!InputFor(t, ins, y.texcoord, edits, reg, firstNew)) return;
    const DWORD S = E + 1, cM = cA + 2;
    edits.push_back({u.afterLastSamplerDcl, {Op(kDcl, 2), 0x90000000u, Dst(kSampler, S)}});
    AppendGroundShade(code, B, T, T + 1, x, z, Src(kInput, reg, static_cast<DWORD>(y.comp) * 0x55u), cM, S);
    out.shadeSampler = static_cast<int>(S);
    out.shadeConst = static_cast<int>(cM);
}
// (2.15.3) The winter floors with the indoor light on the ground (AppendExitGround) in place of AtlasWeight, the max and
// WinterShade: the map's alpha and the luminance of its rgb copied into T + 1 right after its texld, then the block at the
// end of the lamp term B. Needs the texld to write .w, world y, a sampler after E, cA + 1 .. cA + 6 and five temps. uv: the
// atlas uv source of the patch.
bool WinterExit(std::vector<DWORD>& t, const std::vector<Ins>& ins, const Usage& u, const WorldY& y, const Ins& fetch, DWORD B, DWORD T, DWORD E, DWORD cA,
                DWORD uv, DWORD x, DWORD z, std::vector<Edit>& edits, std::vector<DWORD>& code, FloorPatch& out, int firstNew = 0) {
    const DWORD dest = t[fetch.at + 1];
    if (!(WMask(dest) & 0x8) || y.texcoord < 0 || y.texcoord > 15 || y.comp < 0 || y.comp > 3 || E + 1 > 15 || cA + 6 >= 224 || u.maxTemp + 5 >= 32) return false;
    DWORD reg = 0;
    if (!InputFor(t, ins, y.texcoord, edits, reg, firstNew)) return false;
    const DWORD L = Num(dest), T1 = T + 1, S = E + 1, cH = cA + 1, cM = cA + 2, cX = cA + 5, cY = cA + 6;
    edits.push_back({End(fetch), {Op(kMov, 2), Dst(kTemp, T1, 0x1), Src(kTemp, L, kSwzW),
                                  Op(kDp3, 3), Dst(kTemp, T1, 0x2), Src(kTemp, L), Src(kConst, cX)}});
    edits.push_back({u.afterLastSamplerDcl, {Op(kDcl, 2), 0x90000000u, Dst(kSampler, S)}});
    edits.push_back({u.afterLastSamplerDcl, ExitDefs(cX, cY)});
    AppendExitGround(code, B, Src(kTemp, T1, kSwzX), Src(kTemp, T1, kSwzY), uv, T, T + 2, T + 3, T + 4, x, z, Src(kInput, reg, static_cast<DWORD>(y.comp) * 0x55u),
                     cA, cH, cM, cX, cY, E, S);
    out.maskAlpha = true;
    out.heightConst = static_cast<int>(cH);
    out.shadeSampler = static_cast<int>(S);
    out.shadeConst = static_cast<int>(cM);
    out.exitLight = true;
    return true;
}
} // namespace

bool WorldYOutput(const std::vector<DWORD>& t, WorldY& out) {
    out = WorldY{};
    if (t.empty() || t[0] != 0xFFFE0300) return false;
    const auto ins = Parse(t);
    if (ins.empty()) return false;
    std::vector<int> tcOf(12, -1); // output register -> TEXCOORD index
    for (const Ins& x : ins)
        if (x.op == kDcl && Type(t[x.at + 2]) == kOutput && (t[x.at + 1] & 0x1F) == 5 && Num(t[x.at + 2]) < 12) tcOf[Num(t[x.at + 2])] = static_cast<int>((t[x.at + 1] >> 16) & 0xF);
    for (const Ins& x : ins)
        if (IsFlow(x.op)) return false; // a write in a branch could be skipped
    int found = 0;
    WorldY y;
    for (size_t i = 0; i < ins.size(); i++) {
        const Ins& x = ins[i];
        if (x.op != kMov || x.len != 2) continue;
        const DWORD d = t[x.at + 1], s = t[x.at + 2], m = WMask(d);
        if (Type(d) != kOutput || Num(d) >= 12 || tcOf[Num(d)] < 0 || (m != 0x1 && m != 0x2 && m != 0x4 && m != 0x8) || (d & 0x00F00000u)) continue;
        const int comp = m == 0x1 ? 0 : m == 0x2 ? 1 : m == 0x4 ? 2 : 3;
        // the component read for that write: .y of a temp, no source modifier, no relative addressing
        if (Type(s) != kTemp || (s & 0x0F002000u) || ((Swz(s) >> (2 * comp)) & 3) != 1) continue;
        const DWORD w = Num(s);
        // the last write to that temp's .y before it: "dp4 rW.y, rP, c9"; and the same temp gets .x / .z from c8 / c10 (a
        // world position)
        bool fromC9 = false;
        for (size_t j = i; j-- > 0;) {
            const Ins& v = ins[j];
            if (v.len < 1 || v.op == kDcl || v.op == kDef || v.op == kDefI || v.op == kDefB || !IsReg(t[v.at + 1], kTemp, w) || !(WMask(t[v.at + 1]) & 0x2)) continue;
            fromC9 = v.op == kDp4 && WMask(t[v.at + 1]) == 0x2 && IsReg(t[v.at + 3], kConst, 9) && !(t[v.at + 1] & 0x00F00000u);
            break;
        }
        bool dx = false, dz = false;
        for (const Ins& v : ins) {
            if (v.op != kDp4 || !IsReg(t[v.at + 1], kTemp, w)) continue;
            dx |= WMask(t[v.at + 1]) == 0x1 && IsReg(t[v.at + 3], kConst, 8);
            dz |= WMask(t[v.at + 1]) == 0x4 && IsReg(t[v.at + 3], kConst, 10);
        }
        if (!(fromC9 && dx && dz)) continue;
        found++;
        y.texcoord = tcOf[Num(d)];
        y.comp = comp;
        y.reg = static_cast<int>(Num(d));
    }
    if (found != 1) return false;
    // and nothing else writes that output component
    int writes = 0;
    for (const Ins& v : ins) {
        if (v.op == kDcl || v.op == kDef || v.op == kDefI || v.op == kDefB || v.len < 1) continue;
        const DWORD d = t[v.at + 1];
        if ((d & 0x80000000u) && Type(d) == kOutput && Num(d) == static_cast<DWORD>(y.reg) && (WMask(d) & (1u << y.comp))) writes++;
    }
    if (writes != 1) return false;
    out = y;
    return true;
}

namespace {
bool PatchFloorImpl(std::vector<DWORD>& t, FloorPatch& out, const WorldY& worldY, bool exitLight) {
    if (t.empty() || t[0] != 0xFFFF0300) return false;
    const auto ins = Parse(t);
    if (ins.empty()) return false;
    const Usage u = Scan(t, ins);
    size_t v0Dcl = 0;
    int fetch = -1, scale = -1;
    for (size_t i = 0; i < ins.size(); i++) {
        const Ins& x = ins[i];
        if (x.op == kDcl && IsReg(t[x.at + 2], kInput, 0) && (t[x.at + 1] & 0x1F) == 5 /* texcoord */ && ((t[x.at + 1] >> 16) & 0xF) == 0) v0Dcl = x.at + 2;
        if (fetch < 0 && x.op == kTexld && Type(t[x.at + 1]) == kTemp && IsReg(t[x.at + 2], kInput, 2) && IsReg(t[x.at + 3], kSampler, 2)) fetch = static_cast<int>(i);
        if (fetch >= 0 && scale < 0 && static_cast<int>(i) > fetch && x.op == kMul && Type(t[x.at + 1]) == kTemp && WMask(t[x.at + 1]) == 0x7 &&
            Type(t[x.at + 2]) == kTemp && Swz(t[x.at + 2]) == kSwzW && IsReg(t[x.at + 3], kTemp, Num(t[ins[fetch].at + 1])))
            scale = static_cast<int>(i);
    }
    if (!v0Dcl || fetch < 0 || scale < 0 || u.maxSampler < 0 || u.maxSampler >= 15 || !u.afterLastSamplerDcl || u.maxConst + 1 >= 224) return false;
    const DWORD B = Num(t[ins[scale].at + 1]);
    const DWORD E = static_cast<DWORD>(u.maxSampler + 1);
    const DWORD T = static_cast<DWORD>(u.maxTemp + 1);
    const DWORD cA = static_cast<DWORD>(u.maxConst + 1);
    out.atlasSampler = E;
    out.atlasConst = cA;
    // the game's lamp scale of max(map, atlas) (LightProbe-m08 PS_1B3938E8: "mul r0.xyz, r2, c2.x", c2 read once)
    out.scaleConst = LampScaleAfter(t, ins, static_cast<size_t>(scale), B);
    t[v0Dcl] = (t[v0Dcl] & ~0x000F0000u) | 0x000F0000u; // v0.xy -> v0 (zw = world xz)
    std::vector<Edit> edits;
    edits.push_back({u.afterLastSamplerDcl, {Op(kDcl, 2), 0x90000000u, Dst(kSampler, E)}});
    std::vector<DWORD> atlas;
    if (exitLight) {
        if (!WinterExit(t, ins, u, worldY, ins[fetch], B, T, E, cA, Src(kInput, 0, kSwzZWZW), Src(kInput, 0, 0xAA), Src(kInput, 0, kSwzW), edits, atlas, out)) return false;
    } else {
        atlas = {Op(kMad, 4), Dst(kTemp, T, 0x3), Src(kInput, 0, kSwzZWZW), Src(kConst, cA), Src(kConst, cA, kSwzZWZW),
                 Op(kTexld, 3), Dst(kTemp, T), Src(kTemp, T), Src(kSampler, E)};
        AtlasWeight(t, ins[fetch], T, cA, u, edits, atlas, out);
        atlas.insert(atlas.end(), {Op(kMax, 3), Dst(kTemp, B, 0x7), Src(kTemp, B), Src(kTemp, T)});
        WinterShade(t, ins, u, worldY, B, T, E, cA, Src(kInput, 0, 0xAA), Src(kInput, 0, kSwzW), edits, atlas, out);
    }
    if (out.shadeSampler >= 0) t[v0Dcl] &= ~0x00200000u; // world xz at full precision
    edits.push_back({End(ins[scale]), std::move(atlas)});
    Apply(t, std::move(edits));
    return true;
}
} // namespace
// (2.15.3) exitLight: the indoor light on the ground (WinterExit) when the shader has room for it, else the copy of before
bool PatchFloor(std::vector<DWORD>& t, FloorPatch& out, const WorldY& worldY, bool exitLight) {
    if (exitLight) {
        std::vector<DWORD> c = t;
        FloorPatch o;
        if (PatchFloorImpl(c, o, worldY, true)) {
            t.swap(c);
            out = o;
            return true;
        }
    }
    return PatchFloorImpl(t, out, worldY, false);
}

// Winter floor tile vertex shaders: TEXCOORD0.zw = world xz ("mov o1.zw, rW.xyxz", rW.x / rW.z from dp4 with c8 / c10).
// kFloorVsBytecode is one; the curved pool edge (LightProbe-m66, VS_29991640) is another, with a pool mask in TEXCOORD7.
bool IsFloorVs(const std::vector<DWORD>& t) {
    if (t.empty() || t[0] != 0xFFFE0300) return false;
    const auto ins = Parse(t);
    if (ins.empty()) return false;
    int tc0 = -1;
    for (const Ins& x : ins)
        if (x.op == kDcl && Type(t[x.at + 2]) == kOutput && (t[x.at + 1] & 0x1F) == 5 && ((t[x.at + 1] >> 16) & 0xF) == 0 && WMask(t[x.at + 2]) == 0xF)
            tc0 = static_cast<int>(Num(t[x.at + 2]));
    if (tc0 < 0) return false;
    int found = 0;
    for (const Ins& x : ins) {
        if (x.op != kMov || !IsReg(t[x.at + 1], kOutput, static_cast<DWORD>(tc0)) || WMask(t[x.at + 1]) != 0xC || Type(t[x.at + 2]) != kTemp ||
            Swz(t[x.at + 2]) != kSwzXYXZ)
            continue;
        const DWORD w = Num(t[x.at + 2]);
        bool dx = false, dz = false;
        for (const Ins& y : ins) {
            if (y.op != kDp4 || !IsReg(t[y.at + 1], kTemp, w)) continue;
            if (WMask(t[y.at + 1]) == 0x1 && IsReg(t[y.at + 3], kConst, 8)) dx = true;
            if (WMask(t[y.at + 1]) == 0x4 && IsReg(t[y.at + 3], kConst, 10)) dz = true;
        }
        if (dx && dz) found++;
    }
    return found == 1;
}

// Snow lying on lot floor tiles (the big snow mesh around the pool, LightProbe-m69: VS_2FA6DE10 / PS_2FA6D640). The VS
// writes TEXCOORD7.xy = world xz * 0.5 ("mul oN.xy, rW.xzzw, cH.x", rW.x / rW.z from dp4 with c8 / c10) that the PS
// never reads; the PS lights with the room map only: "texld rL, vK, sM" then "mul rB.xyz, rS.s, rL".
bool IsSnowFloorVs(const std::vector<DWORD>& t, int& texcoord) {
    if (t.empty() || t[0] != 0xFFFE0300) return false;
    const auto ins = Parse(t);
    if (ins.empty()) return false;
    // output register -> texcoord index, for TEXCOORD0 (full) and TEXCOORD7 (.xy)
    std::vector<int> tcOf(12, -1);
    for (const Ins& x : ins) {
        if (x.op != kDcl || Type(t[x.at + 2]) != kOutput || (t[x.at + 1] & 0x1F) != 5 || Num(t[x.at + 2]) >= 12) continue;
        const DWORD idx = (t[x.at + 1] >> 16) & 0xF, m = WMask(t[x.at + 2]);
        if ((idx == 7 && m == 0x3) || (idx == 0 && m == 0xF)) tcOf[Num(t[x.at + 2])] = static_cast<int>(idx);
    }
    // Terrain, lot and water vertex shaders share the TEXCOORD0.zw shape but also compute a terrain-map uv,
    // "mad oN.xy, rX, cK, cK.zwzw" (review 25/09: 5 captured terrain/water VS have it, the snow floors do not).
    for (const Ins& x : ins)
        if (x.op == kMad && Type(t[x.at + 1]) == kOutput && WMask(t[x.at + 1]) == 0x3 && Type(t[x.at + 2]) == kTemp && Type(t[x.at + 3]) == kConst &&
            Swz(t[x.at + 3]) == kSwzXYZW && IsReg(t[x.at + 4], kConst, Num(t[x.at + 3])) && Swz(t[x.at + 4]) == kSwzZWZW)
            return false;
    int found = 0;
    for (const Ins& x : ins) {
        // "mul oT7.xy, rW.xzzw, cH.s" (m69) or "mul oT0.zw, rW.xyxz, cH.s" (m71), cH.s a shader-defined 0.5 (replicate)
        if (x.op != kMul || Type(t[x.at + 1]) != kOutput || Num(t[x.at + 1]) >= 12 || Type(t[x.at + 2]) != kTemp || Type(t[x.at + 3]) != kConst) continue;
        const int tc = tcOf[Num(t[x.at + 1])];
        const DWORD m = WMask(t[x.at + 1]), s = Swz(t[x.at + 2]);
        if (!((tc == 7 && m == 0x3 && s == kSwzXZZW) || (tc == 0 && m == 0xC && s == kSwzXYXZ))) continue;
        const DWORD hs = Swz(t[x.at + 3]);
        if (hs != 0x00 && hs != 0x55 && hs != 0xAA && hs != 0xFF) continue; // the 0.5 must be one replicated component
        float half = 0;
        bool isHalf = false;
        for (const Ins& d : ins)
            if (d.op == kDef && IsReg(t[d.at + 1], kConst, Num(t[x.at + 3]))) {
                memcpy(&half, &t[d.at + 2 + (Swz(t[x.at + 3]) & 3)], 4);
                isHalf = half == 0.5f;
            }
        const DWORD w = Num(t[x.at + 2]);
        bool dx = false, dz = false;
        for (const Ins& y : ins) {
            if (y.op != kDp4 || !IsReg(t[y.at + 1], kTemp, w)) continue;
            if (WMask(t[y.at + 1]) == 0x1 && IsReg(t[y.at + 3], kConst, 8)) dx = true;
            if (WMask(t[y.at + 1]) == 0x4 && IsReg(t[y.at + 3], kConst, 10)) dz = true;
        }
        if (isHalf && dx && dz) {
            texcoord = tc;
            found++;
        }
    }
    return found == 1;
}

namespace {
bool PatchSnowFloorImpl(std::vector<DWORD>& t, int texcoord, FloorPatch& out, const WorldY& worldY, bool exitLight) {
    if (texcoord != 0 && texcoord != 7) return false;
    if (t.empty() || t[0] != 0xFFFF0300) return false;
    const auto ins = Parse(t);
    if (ins.empty()) return false;
    const Usage u = Scan(t, ins);
    int maxIn = -1, tcIn = -1;
    size_t afterLastInDcl = 0, tcDcl = 0;
    for (const Ins& x : ins) {
        if ((x.op >= 0x19 && x.op <= 0x1E) || (x.op >= 0x26 && x.op <= 0x2D)) return false; // flow control
        if (x.op != kDcl || Type(t[x.at + 2]) != kInput) continue;
        if ((t[x.at + 1] & 0x1F) == 5 && ((t[x.at + 1] >> 16) & 0xF) == static_cast<DWORD>(texcoord)) {
            if (texcoord == 7) return false; // TEXCOORD7 already read: not the shape we know
            tcIn = static_cast<int>(Num(t[x.at + 2]));
            tcDcl = x.at + 2;
        }
        maxIn = std::max(maxIn, static_cast<int>(Num(t[x.at + 2])));
        afterLastInDcl = End(x);
    }
    // the single "texld rL, vK, sM" whose next reader is "mul rB.xyz, rS.s, rL" (the room map scaled by the bump factor)
    int scale = -1, tex = -1, found = 0;
    DWORD mapSampler = 0;
    for (size_t i = 0; i < ins.size(); i++) {
        const Ins& x = ins[i];
        if (x.op != kTexld || Type(t[x.at + 1]) != kTemp || Type(t[x.at + 2]) != kInput || Type(t[x.at + 3]) != kSampler) continue;
        const DWORD L = Num(t[x.at + 1]);
        for (size_t j = i + 1; j < ins.size(); j++) {
            const Ins& y = ins[j];
            bool reads = false;
            for (size_t k = 2; k <= y.len; k++) reads |= IsReg(t[y.at + k], kTemp, L);
            if (reads) {
                const DWORD s = Type(t[y.at + 2]) == kTemp ? Swz(t[y.at + 2]) : 0x1B;
                if (y.op == kMul && Type(t[y.at + 1]) == kTemp && WMask(t[y.at + 1]) == 0x7 && Type(t[y.at + 2]) == kTemp && (s == 0x00 || s == 0x55 || s == 0xAA || s == 0xFF) &&
                    IsReg(t[y.at + 3], kTemp, L) && Swz(t[y.at + 3]) == kSwzXYZW) {
                    scale = static_cast<int>(j);
                    tex = static_cast<int>(i);
                    mapSampler = Num(t[x.at + 3]);
                    found++;
                }
                break;
            }
            if (IsReg(t[y.at + 1], kTemp, L) && (WMask(t[y.at + 1]) & 0x7)) break; // rgb overwritten (a .w write, as in m69, is fine)
        }
    }
    if (found != 1 || maxIn < 0 || maxIn >= 9 || !afterLastInDcl || u.maxSampler < 0 || u.maxSampler >= 15 || !u.afterLastSamplerDcl || u.maxConst + 1 >= 224 ||
        u.maxTemp + 1 >= 32)
        return false;
    const DWORD B = Num(t[ins[scale].at + 1]), E = static_cast<DWORD>(u.maxSampler + 1), T = static_cast<DWORD>(u.maxTemp + 1);
    const DWORD cA = static_cast<DWORD>(u.maxConst + 1);
    out.atlasSampler = E;
    out.atlasConst = cA;
    out.mapSampler = mapSampler;
    // the game's lamp scale of max(map, atlas) (m69 PS_2FA6D640: "mul r0.xyz, r1, c3.x"; m71 PS_2A044CD0: "mul r4.xyz, r0, c2.x")
    out.scaleConst = LampScaleAfter(t, ins, static_cast<size_t>(scale), B);
    std::vector<Edit> edits;
    DWORD V, swz;
    if (texcoord == 7) { // m69: a new input, world xz / 2 in .xy
        V = static_cast<DWORD>(maxIn + 1);
        swz = kSwzXYXY;
        edits.push_back({afterLastInDcl, {Op(kDcl, 2), 0x80070005u /* texcoord7 */, Dst(kInput, V, 0x3)}});
    } else { // m71: TEXCOORD0.zw, world xz / 2; widen the declaration if the shader reads only .xy
        if (tcIn < 0) return false;
        V = static_cast<DWORD>(tcIn);
        swz = kSwzZWZW;
        t[tcDcl] |= 0x000F0000u;
    }
    edits.push_back({u.afterLastSamplerDcl, {Op(kDcl, 2), 0x90000000u, Dst(kSampler, E)}});
    // world xz / 2 at V (the caller doubles the shade's x and z rows)
    const DWORD cx = swz == kSwzXYXY ? 0x00 : 0xAA, cz = swz == kSwzXYXY ? kSwzY : kSwzW;
    std::vector<DWORD> atlas;
    if (exitLight) {
        // (TEXCOORD7 is the input this patch declares: world y must come from another one)
        if ((texcoord == 7 && worldY.texcoord == 7) ||
            !WinterExit(t, ins, u, worldY, ins[tex], B, T, E, cA, Src(kInput, V, swz), Src(kInput, V, cx), Src(kInput, V, cz), edits, atlas, out,
                        texcoord == 7 ? static_cast<int>(V) + 1 : 0))
            return false;
    } else {
        atlas = {Op(kMad, 4), Dst(kTemp, T, 0x3), Src(kInput, V, swz), Src(kConst, cA), Src(kConst, cA, kSwzZWZW),
                 Op(kTexld, 3), Dst(kTemp, T), Src(kTemp, T), Src(kSampler, E)};
        AtlasWeight(t, ins[tex], T, cA, u, edits, atlas, out);
        atlas.insert(atlas.end(), {Op(kMax, 3), Dst(kTemp, B, 0x7), Src(kTemp, B), Src(kTemp, T)});
        // (TEXCOORD7 is the input this patch declares: world y must come from another one)
        if (!(texcoord == 7 && worldY.texcoord == 7))
            WinterShade(t, ins, u, worldY, B, T, E, cA, Src(kInput, V, cx), Src(kInput, V, cz), edits, atlas, out, texcoord == 7 ? static_cast<int>(V) + 1 : 0);
        else
            out.shadeSampler = out.shadeConst = -1;
    }
    edits.push_back({End(ins[scale]), std::move(atlas)});
    Apply(t, std::move(edits));
    return true;
}
} // namespace
// (2.15.3) exitLight: as PatchFloor
bool PatchSnowFloor(std::vector<DWORD>& t, int texcoord, FloorPatch& out, const WorldY& worldY, bool exitLight) {
    if (exitLight) {
        std::vector<DWORD> c = t;
        FloorPatch o;
        if (PatchSnowFloorImpl(c, texcoord, o, worldY, true)) {
            t.swap(c);
            out = o;
            return true;
        }
    }
    return PatchSnowFloorImpl(t, texcoord, out, worldY, false);
}

// Surfaces lit only by a baked light map (summer outdoor floors, ExteriorFloors technique; census 25/09): the single
// "texld rL, vK, sM" (2D sampler, input coordinate) whose next rgb reader is "mad rX.xyz, rL, cK.x, rY", before any
// flow control. Inserts max(rL, atlas) there, the atlas read at TEXCOORD7.xy = world xz (PatchObjectLampVs exports it).
namespace {
bool PatchBakedAtlasPsImpl(std::vector<DWORD>& t, int texcoord, FloorPatch& out, bool groundShade, bool exitLight) {
    if (texcoord < 0 || texcoord > 15) return false;
    if (t.empty() || t[0] != 0xFFFF0300) return false;
    const auto all = Parse(t);
    if (all.empty()) return false;
    const Usage u = Scan(t, all);
    size_t firstFlow = all.size();
    for (size_t i = 0; i < all.size(); i++)
        if ((all[i].op >= 0x19 && all[i].op <= 0x1E) || (all[i].op >= 0x26 && all[i].op <= 0x2D)) {
            firstFlow = i;
            break;
        }
    int maxIn = -1;
    size_t afterLastInDcl = 0;
    std::vector<int> is2d(16, 0);
    for (const Ins& x : all) {
        if (x.op != kDcl) continue;
        const DWORD r = t[x.at + 2];
        if (Type(r) == kSampler && Num(r) < 16) is2d[Num(r)] = ((t[x.at + 1] >> 27) & 0xF) == 2;
        if (Type(r) != kInput) continue;
        if ((t[x.at + 1] & 0x1F) == 5 && ((t[x.at + 1] >> 16) & 0xF) == static_cast<DWORD>(texcoord)) return false; // that TEXCOORD already read
        maxIn = std::max(maxIn, static_cast<int>(Num(r)));
        afterLastInDcl = End(x);
    }
    if (maxIn < 0 || maxIn >= 9 || !afterLastInDcl) return false;
    int mad = -1, tex = -1, found = 0;
    DWORD L = 0, K = 0;
    for (size_t i = 0; i < firstFlow; i++) {
        const Ins& x = all[i];
        if (x.op != kTexld || Type(t[x.at + 1]) != kTemp || Type(t[x.at + 2]) != kInput || Type(t[x.at + 3]) != kSampler || !is2d[Num(t[x.at + 3]) & 15]) continue;
        const DWORD A = Num(t[x.at + 1]);
        for (size_t j = i + 1; j < firstFlow; j++) {
            const Ins& y = all[j];
            bool reads = false;
            for (size_t k = 2; k <= y.len; k++) reads |= IsReg(t[y.at + k], kTemp, A) && Swz(t[y.at + k]) != kSwzW;
            if (reads) {
                if (y.op == kMad && Type(t[y.at + 1]) == kTemp && WMask(t[y.at + 1]) == 0x7 && IsReg(t[y.at + 2], kTemp, A) && Swz(t[y.at + 2]) == kSwzXYZW &&
                    Type(t[y.at + 3]) == kConst && Swz(t[y.at + 3]) == kSwzX && Type(t[y.at + 4]) == kTemp) {
                    mad = static_cast<int>(j);
                    tex = static_cast<int>(i);
                    L = A;
                    K = Num(t[y.at + 3]);
                    found++;
                }
                break;
            }
            if (IsReg(t[y.at + 1], kTemp, A) && (WMask(t[y.at + 1]) & 0x7)) break;
        }
    }
    if (found != 1) return false;
    // The map's alpha (the floor visibility mask of level_light_share) must still be in L.w at the mad: the texld writes w
    // and nothing in between writes it. Otherwise the weight constant stays (1, 0) and the atlas whole.
    out.maskAlpha = (WMask(t[all[tex].at + 1]) & 0x8) != 0;
    for (int i = tex + 1; i < mad && out.maskAlpha; i++)
        if (all[i].len > 0 && (t[all[i].at + 1] & 0x80000000u) && IsReg(t[all[i].at + 1], kTemp, L) && (WMask(t[all[i].at + 1]) & 0x8)) out.maskAlpha = false;
    int kUses = 0; // the scale constant is read nowhere else (not a shared constant)
    for (const Ins& x : all) {
        if (x.op == kDef || x.op == kDcl) continue;
        for (size_t k = 1; k <= x.len; k++) kUses += IsReg(t[x.at + k], kConst, K) ? 1 : 0;
    }
    if (kUses != 1 || u.maxSampler < 0 || u.maxSampler >= 15 || !u.afterLastSamplerDcl || u.maxConst + 2 >= 224 || u.maxTemp + 2 >= 32) return false;
    const DWORD E = static_cast<DWORD>(u.maxSampler + 1), T = static_cast<DWORD>(u.maxTemp + 1), T2 = T + 1;
    const DWORD cA = static_cast<DWORD>(u.maxConst + 1), cH = cA + 1;
    const DWORD V = static_cast<DWORD>(maxIn + 1);
    out.atlasSampler = E;
    out.atlasConst = cA;
    out.heightConst = static_cast<int>(cH);
    // K scales the map alone (read once, checked above); a def of cK would override what is set
    out.scaleConst = static_cast<int>(K);
    for (const Ins& x : all)
        if (x.op == kDef && IsReg(t[x.at + 1], kConst, K)) out.scaleConst = -1;
    std::vector<DWORD> code = {Op(kMad, 4), Dst(kTemp, T, 0x3), Src(kInput, V, kSwzXYXY), Src(kConst, cA), Src(kConst, cA, kSwzZWZW),
                               Op(kTexld, 3), Dst(kTemp, T), Src(kTemp, T), Src(kSampler, E),
                               Op(kMad, 4), Dst(kTemp, T2, 0x1, true), Src(kTemp, L, kSwzW), Src(kConst, cH, kSwzY), Src(kConst, cH, kSwzX),
                               Op(kMul, 3), Dst(kTemp, T, 0x7), Src(kTemp, T), Src(kTemp, T2, kSwzX),
                               Op(kMax, 3), Dst(kTemp, L, 0x7), Src(kTemp, L), Src(kTemp, T)};
    std::vector<Edit> edits = {{afterLastInDcl, {Op(kDcl, 2), 0x80000005u | (static_cast<DWORD>(texcoord) << 16) /* texcoordN */, Dst(kInput, V, 0x7)}},
                               {u.afterLastSamplerDcl, {Op(kDcl, 2), 0x90000000u, Dst(kSampler, E)}}};
    // the ground lamp shade (story 0): world (x, z, y) = V.xyz; only with a sampler and 3 constants to spare
    out.shadeSampler = out.shadeConst = -1;
    if (exitLight) {
        // (2.15.3) the indoor light on the ground (AppendExitGround) in place of the atlas, the max and the shade: the map's
        // alpha must be readable, a sampler, cH + 1 .. cH + 5 and four temps to spare
        if (!out.maskAlpha || E + 1 > 15 || cH + 5 >= 224 || u.maxTemp + 4 >= 32) return false;
        const DWORD S = E + 1, cM = cH + 1, cX = cH + 4, cY = cH + 5;
        code = {Op(kDp3, 3), Dst(kTemp, T2, 0x8), Src(kTemp, L), Src(kConst, cX)}; // lum(map) in Q.w
        AppendExitGround(code, L, Src(kTemp, L, kSwzW), Src(kTemp, T2, kSwzW), Src(kInput, V, kSwzXYXY), T, T2, T + 2, T + 3, Src(kInput, V, kSwzX),
                         Src(kInput, V, kSwzY), Src(kInput, V, 0xAA), cA, cH, cM, cX, cY, E, S);
        edits.push_back({u.afterLastSamplerDcl, {Op(kDcl, 2), 0x90000000u, Dst(kSampler, S)}});
        edits.push_back({u.afterLastSamplerDcl, ExitDefs(cX, cY)});
        out.shadeSampler = static_cast<int>(S);
        out.shadeConst = static_cast<int>(cM);
        out.exitLight = true;
    } else if (groundShade && E + 1 <= 15 && cH + 3 < 224) {
        const DWORD S = E + 1, cM = cH + 1;
        edits.push_back({u.afterLastSamplerDcl, {Op(kDcl, 2), 0x90000000u, Dst(kSampler, S)}});
        AppendGroundShade(code, L, T, T2, Src(kInput, V, kSwzX), Src(kInput, V, kSwzY), Src(kInput, V, 0xAA), cM, S);
        out.shadeSampler = static_cast<int>(S);
        out.shadeConst = static_cast<int>(cM);
    }
    edits.push_back({all[mad].at, std::move(code)});
    Apply(t, std::move(edits));
    return true;
}
} // namespace
// (2.15.3) exitLight (with groundShade): as PatchFloor
bool PatchBakedAtlasPs(std::vector<DWORD>& t, int texcoord, FloorPatch& out, bool groundShade, bool exitLight) {
    if (exitLight && groundShade) {
        std::vector<DWORD> c = t;
        FloorPatch o;
        if (PatchBakedAtlasPsImpl(c, texcoord, o, true, true)) {
            t.swap(c);
            out = o;
            return true;
        }
    }
    return PatchBakedAtlasPsImpl(t, texcoord, out, groundShade, false);
}

bool PatchLeafShadow(std::vector<DWORD>& t, DWORD& nightConst) {
    if (t.empty() || (t[0] & 0xFFFF0000u) != 0xFFFF0000u) return false;
    const auto ins = Parse(t);
    if (ins.empty()) return false;
    const Usage u = Scan(t, ins);
    for (const Ins& x : ins) {
        // lrp rD.w, tN.x, cK.y, rS.w: the shadow fade is t5.x in the bushes of 24/09, t6.x in the winter bush of
        // LightProbe-m63 (VS_32779B38 / PS_3277A240, one more texcoord)
        // lrp rD.w, tN.x, cK.s, rS.w: cK.y at runtime in the bushes of 24/09; a shader-defined 1 in any component in the
        // summer bush of LightProbe-m78 (PS_32DDB220: "lrp r1.w, t6.x, c3.z, r2.w", def c3 = 0.5, 0.25, 1, 0)
        if (x.op != kLrp || Type(t[x.at + 1]) != kTemp || WMask(t[x.at + 1]) != 0x8 || Type(t[x.at + 2]) != kTexture || Swz(t[x.at + 2]) != kSwzX ||
            Type(t[x.at + 3]) != kConst || Type(t[x.at + 4]) != kTemp)
            continue;
        const DWORD D = Num(t[x.at + 1]), K = Num(t[x.at + 3]), ks = Swz(t[x.at + 3]);
        if (ks != 0x00 && ks != 0x55 && ks != 0xAA && ks != 0xFF) continue;
        bool isDef = false, one = false; // the "no shadow" end of the lerp must be 1
        for (const Ins& d : ins)
            if (d.op == kDef && IsReg(t[d.at + 1], kConst, K)) {
                float v;
                memcpy(&v, &t[d.at + 2 + (ks & 3)], 4);
                isDef = true;
                one = v == 1.0f;
            }
        if (isDef ? !one : ks != kSwzY) continue;
        const DWORD T = static_cast<DWORD>(u.maxTemp + 1);
        const DWORD cN = static_cast<DWORD>(u.maxConst + 1);
        if (T >= 12 || cN >= 32) return false; // ps_2_0 limits
        nightConst = cN;
        // rD.w += cN.x * (cK.y - rD.w), as two instructions: ps_2_0 allows only one constant register per instruction
        Apply(t, {{End(x), {Op(kAdd, 3), Dst(kTemp, T, 0x8), Src(kConst, K, ks), Src(kTemp, D, kSwzW) | 0x01000000u /* negate */,
                            Op(kMad, 4), Dst(kTemp, D, 0x8), Src(kTemp, T, kSwzW), Src(kConst, cN, kSwzX), Src(kTemp, D, kSwzW)}}});
        return true;
    }
    return false;
}

bool PatchFoliageVs(std::vector<DWORD>& t, bool runtimeWrap) {
    if (t.empty() || (t[0] & 0xFFFF0000u) != 0xFFFE0000u) return false;
    const auto ins = Parse(t);
    if (ins.empty()) return false;
    bool lampArray = false;
    for (const Ins& x : ins)
        for (size_t k = 1; k <= x.len; k++)
            if (IsReg(t[x.at + k], kConst, 27) && (t[x.at + k] & 0x2000)) lampArray = true; // c27[a0.z]: per-instance lamp directions
    if (!lampArray) return false;
    // "def cK, ...": the component `swz` (a replicate swizzle) of a shader-defined constant, if there is one
    auto defined = [&](DWORD k, DWORD swz, float& v) {
        for (const Ins& d : ins)
            if (d.op == kDef && IsReg(t[d.at + 1], kConst, k)) {
                memcpy(&v, &t[d.at + 2 + (swz & 3)], 4);
                return true;
            }
        return false;
    };
    for (const Ins& x : ins) {
        // max r0, r0, cK.w (a runtime zero), or max r0, r0, cK.s with cK.s a shader-defined 0 (the winter bush of
        // LightProbe-m63, VS_32779B38: "max r0, r0, c131.x", def c131 = 0, ...)
        if (x.op != kMax || !IsReg(t[x.at + 1], kTemp, 0) || WMask(t[x.at + 1]) != 0xF || !IsReg(t[x.at + 2], kTemp, 0) || Swz(t[x.at + 2]) != kSwzXYZW ||
            Type(t[x.at + 3]) != kConst || (t[x.at + 3] & 0x2000))
            continue;
        const DWORD K = Num(t[x.at + 3]), swz = Swz(t[x.at + 3]);
        float v = 1.0f;
        const bool isDef = defined(K, swz, v);
        if (isDef ? !((swz == 0x00 || swz == 0x55 || swz == 0xAA || swz == 0xFF) && v == 0.0f) : swz != kSwzW) continue;
        constexpr DWORD cW = kFoliageWrapConst;
        t[x.at + 1] = Dst(kTemp, 0, 0x1); // the sun keeps the plain clamp
        const std::vector<DWORD> wrap = {Op(kMad, 4), Dst(kTemp, 0, 0xE), Src(kTemp, 0), Src(kConst, cW, kSwzX), Src(kConst, cW, kSwzY),
                                         Op(kMax, 3), Dst(kTemp, 0, 0xE), Src(kTemp, 0), Src(kConst, K, swz)};
        if (runtimeWrap) Apply(t, {{End(x), wrap}}); // c255 set by the draw: (s / 1.5, s 0.5 / 1.5) with s the daytime curve
        else Apply(t, {{1, {Op(kDef, 5), Dst(kConst, cW), F(1.0f / 1.5f), F(0.5f / 1.5f), F(0.0f), F(0.0f)}}, {End(x), wrap}});
        return true;
    }
    return false;
}

// Instanced lot structures (fence rails/posts, railings, stairs). Found by the fence analysis (NOTAS-ILUMINACAO.md):
// their VS reads only the rig's "vertex light" arrays (VertexLightDirections/Colors, rig+0x90/+0xD0), which the game
// fills only with overflow lights, and the whole group shares one rig placed at its centre. So fences are lit per pixel
// from the ground light atlas instead.
bool IsInstancedStructureVs(const std::vector<DWORD>& t) {
    if (t.empty() || t[0] != 0xFFFE0300) return false;
    const auto ins = Parse(t);
    if (ins.empty()) return false;
    bool pos1 = false, pos2 = false;
    int tc1Out = -1, col0Out = -1;
    for (const Ins& x : ins) {
        if (x.op == kMova) return false;
        if (x.op != kDef && x.op != kDefI && x.op != kDefB && x.op != kDcl)
            for (size_t k = 1; k <= x.len; k++)
                if ((t[x.at + k] & 0x80000000u) && (t[x.at + k] & 0x2000)) return false; // relative addressing
        if (x.op != kDcl) continue;
        const DWORD use = t[x.at + 1] & 0x1F, idx = (t[x.at + 1] >> 16) & 0xF, r = t[x.at + 2];
        if (Type(r) == kInput && use == 0 && idx == 1) pos1 = true; // POSITION1: instance translation
        if (Type(r) == kInput && use == 0 && idx == 2) pos2 = true; // POSITION2: instance rotation
        if (Type(r) == kOutput && use == 5 && idx == 1) tc1Out = static_cast<int>(Num(r));
        if (Type(r) == kOutput && use == 10 && idx == 0) col0Out = static_cast<int>(Num(r));
    }
    if (!pos1 || !pos2 || tc1Out < 0 || col0Out < 0) return false;
    bool world = false, lamps = false;
    for (const Ins& x : ins) {
        // mov oT.zw, rW.xyxz: world xz into TEXCOORD1.zw
        if (x.op == kMov && IsReg(t[x.at + 1], kOutput, static_cast<DWORD>(tc1Out)) && WMask(t[x.at + 1]) == 0xC && Type(t[x.at + 2]) == kTemp &&
            Swz(t[x.at + 2]) == kSwzXYXZ)
            world = true;
        // mad oC.xyz, rX.w, c11, rY: the last of the 4 vertex lights into COLOR0
        if (x.op == kMad && IsReg(t[x.at + 1], kOutput, static_cast<DWORD>(col0Out)) && IsReg(t[x.at + 3], kConst, 11)) lamps = true;
    }
    return world && lamps;
}

bool PatchInstancedLamps(std::vector<DWORD>& t, InstancedPatch& out, bool reach, bool groundShade) {
    if (t.empty() || t[0] != 0xFFFF0300) return false;
    const auto ins = Parse(t);
    if (ins.empty()) return false;
    const Usage u = Scan(t, ins);
    int colorReg = -1, tc1Reg = -1;
    size_t tc1Dcl = 0;
    for (const Ins& x : ins) {
        if (x.op != kDcl) continue;
        const DWORD use = t[x.at + 1] & 0x1F, idx = (t[x.at + 1] >> 16) & 0xF, r = t[x.at + 2];
        if (Type(r) != kInput) continue;
        if (use == 10 && idx == 0 && (WMask(r) & 0x7) == 0x7) colorReg = static_cast<int>(Num(r)); // COLOR0: vertex lights
        if (use == 5 && idx == 1) {                                                                 // TEXCOORD1: zw = world xz
            tc1Reg = static_cast<int>(Num(r));
            tc1Dcl = x.at + 2;
        }
    }
    if (colorReg < 0 || tc1Reg < 0) return false;
    // TEXCOORD2: the world normal (xyz) and the world height (w), from the instanced VS ("mov oT2.xyz, rN", "mov oT2.w, rW.y")
    int tc2Reg = -1;
    size_t tc2Dcl = 0;
    for (const Ins& x : ins) {
        if (x.op != kDcl) continue;
        const DWORD use = t[x.at + 1] & 0x1F, idx = (t[x.at + 1] >> 16) & 0xF, r = t[x.at + 2];
        if (Type(r) == kInput && use == 5 && idx == 2) {
            tc2Reg = static_cast<int>(Num(r));
            tc2Dcl = x.at + 2;
        }
    }
    // the single "add rD.xyz, rS, vC" (lamps added to sun x shadow + ambient)
    int add = -1, found = 0, slot = 0;
    for (size_t i = 0; i < ins.size(); i++) {
        const Ins& x = ins[i];
        if (x.op != kAdd || Type(t[x.at + 1]) != kTemp || WMask(t[x.at + 1]) != 0x7) continue;
        for (int s = 2; s <= 3; s++)
            if (IsReg(t[x.at + s], kInput, static_cast<DWORD>(colorReg)) && Swz(t[x.at + s]) == kSwzXYZW && Type(t[x.at + (s == 2 ? 3 : 2)]) == kTemp) {
                add = static_cast<int>(i);
                slot = s;
                found++;
            }
    }
    if (found != 1 || u.maxSampler < 0 || u.maxSampler >= 15 || !u.afterLastSamplerDcl || u.maxConst + 2 >= 224 || u.maxTemp + 1 >= 32) return false;
    // the reach variant is the per-pixel lamps with the wall law and 2 more rows (r0^2 of the 8 lamps) or nothing: refused here,
    // before any change, the draw keeps the first variant
    if (reach && !(tc2Reg >= 0 && u.maxConst + 6 + 4 * kObjectPixelLamps < 224 && u.maxTemp + 7 < 32)) return false;
    const DWORD E = static_cast<DWORD>(u.maxSampler + 1), T = static_cast<DWORD>(u.maxTemp + 1);
    const DWORD cA = static_cast<DWORD>(u.maxConst + 1), cB = cA + 1;
    const DWORD C = static_cast<DWORD>(colorReg), V = static_cast<DWORD>(tc1Reg);
    out.atlasSampler = E;
    out.atlasConst = cA;
    out.strengthConst = cB;
    out.pixelLamps = false;
    out.coneConst = 0;
    out.reachConst = 0;
    out.shadeSampler = out.shadeConst = -1;
    t[tc1Dcl] = (t[tc1Dcl] & ~0x000F0000u) | 0x000F0000u; // vT.xy -> vT (zw = world xz)
    const Ins& A = ins[add];
    t[A.at + slot] = Src(kTemp, T); // add rD.xyz, rS, vC  ->  add rD.xyz, rS, rT
    // Per-pixel lamps (fences looked lit far from a lamp, faces turned away included): with the normal and the
    // height in TEXCOORD2, the same lamps as objects (PatchObjectLampPs: colour x sat(N.l) x sat(1 - d^2/R^2)^2 at the
    // pixel; the reach variant: the outdoor walls' law, AppendLampLaw), and the ground atlas only on faces turned up (the
    // fence tops: x sat(N.y)), since the atlas is the light lying on the ground and has no direction.
    // T = max(vC + lamps x cS.y, atlas x sat(N.y) x cB.x).
    if (tc2Reg >= 0 && u.maxConst + 4 + 4 * kObjectPixelLamps < 224 && u.maxTemp + 7 < 32) {
        const DWORD cH = cA + 2, cS = cA + 3, cL = cA + 4, cK = cL + 2 * kObjectPixelLamps;
        const DWORD N = T + 1, G = T + 2, P = T + 3, LA = T + 4, LB = T + 5, Q = T + 6, V2 = static_cast<DWORD>(tc2Reg);
        constexpr DWORD kNeg = 0x01000000u, kNrmOp = 0x24;
        out.pixelLamps = true;
        out.lampParamConst = cS;
        out.lampConst = cL;
        out.groundWeightConst = cH;
        out.coneConst = cK;
        const DWORD cR = cK + 2 * kObjectPixelLamps; // reach: r0^2 of lamps 0..3 / 4..7 (one upload with the rows above)
        out.reachConst = reach ? cR : 0;
        t[tc2Dcl] = (t[tc2Dcl] & ~0x000F0000u) | 0x000F0000u; // vT2.xyz -> vT2 (w = world y)
        std::vector<DWORD> code = {Op(kMad, 4), Dst(kTemp, T, 0x3), Src(kInput, V, kSwzZWZW), Src(kConst, cA), Src(kConst, cA, kSwzZWZW),
                                   Op(kTexld, 3), Dst(kTemp, T), Src(kTemp, T), Src(kSampler, E),
                                   Op(kNrmOp, 2), Dst(kTemp, N, 0x7), Src(kInput, V2),
                                   Op(kMad, 4), Dst(kTemp, G, 0x8, true), Src(kTemp, N, kSwzY), Src(kConst, cH, kSwzX), Src(kConst, cH, kSwzY), // sat(N.y)
                                   Op(kMul, 3), Dst(kTemp, G, 0x8), Src(kTemp, G, kSwzW), Src(kConst, cB, kSwzX),
                                   Op(kMul, 3), Dst(kTemp, T, 0x7), Src(kTemp, T), Src(kTemp, G, kSwzW),
                                   Op(kMov, 2), Dst(kTemp, P, 0x5), Src(kInput, V, 0xFA /* zzww: x = world x, z = world z */),
                                   Op(kMov, 2), Dst(kTemp, P, 0x2), Src(kInput, V2, kSwzW)};
        for (DWORD k = 0; k < kObjectPixelLamps; k++) {
            const DWORD cp = cL + 2 * k, cc = cp + 1, k1 = cK + 2 * k, k2 = k1 + 1;
            // l = lamp - pixel, d^2, sat(N.l) and the falloff (AppendLampLaw; the reach variant: the outdoor walls' law)
            AppendLampLaw(code, LA, LB, N, Src(kTemp, P) | kNeg, cp, cS, cH, reach, cR, k);
            code.insert(code.end(), {
                                     // the lamp's cones (a brick fence face was lit by a sconce it sits 47 deg or more
                                     // off; the game gives it only the spill there, FUN_006BC940): f = S + (1 - S) sat(max(t1, t2)),
                                     // t = l.k.xyz + k.w (lot_light_bridge LampCone), S = cc.w. LB.z / LB.w are free after the
                                     // law; each instruction reads one constant register.
                                     Op(kDp3, 3), Dst(kTemp, LB, 0x4), Src(kTemp, LA), Src(kConst, k1),
                                     Op(kAdd, 3), Dst(kTemp, LB, 0x4), Src(kTemp, LB, 0xAA /* z */), Src(kConst, k1, kSwzW),
                                     Op(kDp3, 3), Dst(kTemp, LB, 0x8), Src(kTemp, LA), Src(kConst, k2),
                                     Op(kAdd, 3), Dst(kTemp, LB, 0x8), Src(kTemp, LB, kSwzW), Src(kConst, k2, kSwzW),
                                     Op(kMax, 3), Dst(kTemp, LB, 0x4, true), Src(kTemp, LB, 0xAA), Src(kTemp, LB, kSwzW),
                                     Op(kMad, 4), Dst(kTemp, LB, 0x8), Src(kTemp, LB, 0xAA), Src(kConst, cc, kSwzW) | kNeg, Src(kTemp, LB, 0xAA), // t (1 - S)
                                     Op(kAdd, 3), Dst(kTemp, LB, 0x4), Src(kTemp, LB, kSwzW), Src(kConst, cc, kSwzW),                            // S + t (1 - S)
                                     Op(kMul, 3), Dst(kTemp, LB, 0x1), Src(kTemp, LB, kSwzX), Src(kTemp, LB, 0xAA)});
            if (k == 0) code.insert(code.end(), {Op(kMul, 3), Dst(kTemp, Q, 0x7), Src(kConst, cc), Src(kTemp, LB, kSwzX)});
            else code.insert(code.end(), {Op(kMad, 4), Dst(kTemp, Q, 0x7), Src(kConst, cc), Src(kTemp, LB, kSwzX), Src(kTemp, Q)});
        }
        // reach: the sum is held under 1 as an outdoor wall's texel is (the room map's min(sum, 1)), by its brightest channel
        if (reach) AppendHueClamp(code, Q, LB);
        // Native rig and pixel lamps are alternative estimates; adding both can
        // amplify overlapping illumination. Use pixel lamps when the group is valid,
        // otherwise retain the native rig.
        code.insert(code.end(), {Op(kMul, 3), Dst(kTemp, LA, 0x7), Src(kTemp, Q), Src(kConst, cS, kSwzY),
                                 Op(kLrp, 4), Dst(kTemp, LA, 0x7), Src(kConst, cS, kSwzX), Src(kTemp, LA), Src(kInput, C),
                                 Op(kMax, 3), Dst(kTemp, T, 0x7), Src(kTemp, LA), Src(kTemp, T)});
        std::vector<Edit> edits = {{u.afterLastSamplerDcl, {Op(kDcl, 2), 0x90000000u, Dst(kSampler, E)}}};
        // The ground lamp shade (13/10, F7 2026-10-13 09-31-30 #268: the wooden curbs of a flowerbed behind a free-standing wall
        // stayed lit by the garden post on the other side, while the grass and the paths beside them were shaded). One lamp
        // list serves the whole group, so a group running past the wall keeps the lamp for every piece, and neither the lamps
        // here nor the atlas test a wall. The sum (atlas on faces turned up and the lamps) is multiplied by the lot ground's
        // shade, as the outdoor floors' (AppendGroundShade): x 1 + gate (M - 1), M = the lot mask's alpha at the pixel's world
        // xz, gate 1 within about 1 m of the lot's ground (P = world x, y, z). LA and LB are free after the max. Only with a
        // sampler and 3 constants to spare (after the reach rows): otherwise the bytes of the patch without it.
        const DWORD cM = cR + (reach ? 2u : 0u);
        if (groundShade && E + 1 <= 15 && cM + 2 < 224) {
            const DWORD S = E + 1;
            edits.push_back({u.afterLastSamplerDcl, {Op(kDcl, 2), 0x90000000u, Dst(kSampler, S)}});
            // (2.15.3) the mask in front of the face: xz + N.xz x rows[10] (cM + 2 .z; 0.6 m, kFenceShadeReach). The fence is a
            // wall in the mask, so its own footprint lies on the edge of its shadow; tops (N.xz = 0) read where they stand
            code.insert(code.end(), {Op(kMad, 4), Dst(kTemp, LA, 0x5), Src(kTemp, N), Src(kConst, cM + 2, 0xAA), Src(kTemp, P)});
            AppendGroundShade(code, T, LA, LB, Src(kTemp, LA, kSwzX), Src(kTemp, LA, 0xAA), Src(kTemp, P, kSwzY), cM, S);
            out.shadeSampler = static_cast<int>(S);
            out.shadeConst = static_cast<int>(cM);
        }
        edits.push_back({A.at, std::move(code)});
        Apply(t, std::move(edits));
        return true;
    }
    Apply(t, {{u.afterLastSamplerDcl, {Op(kDcl, 2), 0x90000000u, Dst(kSampler, E)}},
              {A.at, {Op(kMad, 4), Dst(kTemp, T, 0x3), Src(kInput, V, kSwzZWZW), Src(kConst, cA), Src(kConst, cA, kSwzZWZW),
                      Op(kTexld, 3), Dst(kTemp, T), Src(kTemp, T), Src(kSampler, E),
                      Op(kMul, 3), Dst(kTemp, T, 0x7), Src(kTemp, T), Src(kConst, cB, kSwzX),
                      Op(kMax, 3), Dst(kTemp, T, 0x7), Src(kTemp, T), Src(kInput, C)}}});
    return true;
}

bool IsSnowCoverVs(const std::vector<DWORD>& t) {
    if (t.empty() || t[0] != 0xFFFE0300) return false;
    const auto ins = Parse(t);
    if (ins.empty()) return false;
    int tc3Out = -1, tc1In = -1;
    for (const Ins& x : ins) {
        if (x.op != kDcl) continue;
        const DWORD use = t[x.at + 1] & 0x1F, idx = (t[x.at + 1] >> 16) & 0xF, r = t[x.at + 2];
        if (Type(r) == kOutput && use == 5 && idx == 3 && WMask(r) == 0x3) tc3Out = static_cast<int>(Num(r)); // TEXCOORD3.xy
        if (Type(r) == kInput && use == 5 && idx == 1) tc1In = static_cast<int>(Num(r));                      // TEXCOORD1
    }
    if (tc3Out < 0 || tc1In < 0) return false;
    bool morph = false;
    int world = -1, found = 0;
    for (const Ins& x : ins) {
        // slt rX, cK, vT1.x: the snow cover's position morph flags
        if (x.op == kSlt && IsReg(t[x.at + 3], kInput, static_cast<DWORD>(tc1In)) && Swz(t[x.at + 3]) == kSwzX) morph = true;
        // mov oT3.xy, rW.xzzw: world xz
        if (x.op == kMov && IsReg(t[x.at + 1], kOutput, static_cast<DWORD>(tc3Out)) && WMask(t[x.at + 1]) == 0x3 && Type(t[x.at + 2]) == kTemp &&
            Swz(t[x.at + 2]) == kSwzXZZW) {
            world = static_cast<int>(Num(t[x.at + 2]));
            found++;
        }
    }
    if (!morph || found != 1) return false;
    bool dx = false, dz = false; // rW.x = dp4(pos, c8), rW.z = dp4(pos, c10): world matrix rows
    for (const Ins& x : ins) {
        if (x.op != kDp4 || !IsReg(t[x.at + 1], kTemp, static_cast<DWORD>(world))) continue;
        if (WMask(t[x.at + 1]) == 0x1 && IsReg(t[x.at + 3], kConst, 8)) dx = true;
        if (WMask(t[x.at + 1]) == 0x4 && IsReg(t[x.at + 3], kConst, 10)) dz = true;
    }
    return dx && dz;
}

bool PatchSnowCover(std::vector<DWORD>& t, SnowCoverPatch& out) {
    if (t.empty() || t[0] != 0xFFFF0300) return false;
    const auto ins = Parse(t);
    if (ins.empty()) return false;
    const Usage u = Scan(t, ins);
    int tc3 = -1;
    for (const Ins& x : ins) {
        if ((x.op >= 0x19 && x.op <= 0x1E) || (x.op >= 0x26 && x.op <= 0x2D)) return false; // flow control (call..label, rep..breakc): leave it alone
        if (x.op != kDcl) continue;
        const DWORD use = t[x.at + 1] & 0x1F, idx = (t[x.at + 1] >> 16) & 0xF, r = t[x.at + 2];
        if (Type(r) == kInput && use == 5 && idx == 3 && (WMask(r) & 0x3) == 0x3) tc3 = static_cast<int>(Num(r)); // TEXCOORD3: world xz
    }
    if (tc3 < 0) return false;
    // the single "mul oC0.xyz, rP, rQ"
    int mul = -1, found = 0;
    for (size_t i = 0; i < ins.size(); i++) {
        const Ins& x = ins[i];
        if (x.op == kMul && IsReg(t[x.at + 1], kColorOut, 0) && WMask(t[x.at + 1]) == 0x7 && Type(t[x.at + 2]) == kTemp && Type(t[x.at + 3]) == kTemp) {
            mul = static_cast<int>(i);
            found++;
        }
    }
    if (found != 1) return false;
    auto lastWriter = [&](DWORD reg) {
        for (int i = mul - 1; i >= 0; i--) {
            const Ins& x = ins[i];
            if (x.op == kDcl || x.op == kDef || x.op == kDefI || x.op == kDefB || x.op == kTexkill || x.len == 0) continue;
            if (IsReg(t[x.at + 1], kTemp, reg)) return i;
        }
        return -1;
    };
    const DWORD P = Num(t[ins[mul].at + 2]), Q = Num(t[ins[mul].at + 3]);
    const int wP = lastWriter(P), wQ = lastWriter(Q);
    if (wP < 0 || wQ < 0) return false;
    // the snow texture comes from a texld, the light from "mad rL.xyz, rX.w, c0, rY" (moon x shadow + sky)
    int wL = -1;
    DWORD L = 0;
    if (ins[wQ].op == kTexld) {
        wL = wP;
        L = P;
    } else if (ins[wP].op == kTexld) {
        wL = wQ;
        L = Q;
    } else
        return false;
    const Ins& M = ins[wL];
    if (M.op != kMad || WMask(t[M.at + 1]) != 0x7 || Type(t[M.at + 2]) != kTemp || Swz(t[M.at + 2]) != kSwzW || !IsReg(t[M.at + 3], kConst, 0)) return false;
    if (u.maxSampler < 0 || u.maxSampler >= 15 || !u.afterLastSamplerDcl || u.maxConst + 2 >= 224 || u.maxTemp + 1 >= 32) return false;
    const DWORD E = static_cast<DWORD>(u.maxSampler + 1), T = static_cast<DWORD>(u.maxTemp + 1);
    const DWORD cA = static_cast<DWORD>(u.maxConst + 1), cB = cA + 1, V = static_cast<DWORD>(tc3);
    out.atlasSampler = E;
    out.atlasConst = cA;
    out.strengthConst = cB;
    Apply(t, {{u.afterLastSamplerDcl, {Op(kDcl, 2), 0x90000000u, Dst(kSampler, E)}},
              {ins[mul].at, {Op(kMad, 4), Dst(kTemp, T, 0x3), Src(kInput, V, kSwzXYXY), Src(kConst, cA), Src(kConst, cA, kSwzZWZW),
                             Op(kTexld, 3), Dst(kTemp, T), Src(kTemp, T), Src(kSampler, E),
                             Op(kMad, 4), Dst(kTemp, L, 0x7), Src(kTemp, T), Src(kConst, cB, kSwzX), Src(kTemp, L)}}});
    return true;
}

bool IsFlowControl(DWORD op) { return (op >= 0x19 && op <= 0x1E) || (op >= 0x26 && op <= 0x2D); }

bool PatchObjectLampVs(std::vector<DWORD>& t, bool needColor0, int* texcoordOut, int* worldConstOut, int* vertexLightOut) {
    if (t.empty() || t[0] != 0xFFFE0300) return false;
    const auto ins = Parse(t);
    if (ins.empty()) return false;
    int maxOut = -1, col0 = -1;
    size_t afterLastOutDcl = 0;
    uint32_t tcUsed = 0, posIn = 0, nrmIn = 0; // TEXCOORD indices already written; POSITIONn / NORMALn inputs
    for (const Ins& x : ins) {
        // Skinned objects (an animated door: mova + c[a0] bones inside "if b0", LightProbe-m57) are fine as long as the
        // world position below is computed outside any branch; loops, subroutines and labels are not.
        if (IsFlowControl(x.op) && x.op != 0x28 && x.op != 0x29 && x.op != 0x2A && x.op != 0x2B && x.op != 0x1C) return false;
        if (x.op != kDcl) continue;
        const DWORD use = t[x.at + 1] & 0x1F, idx = (t[x.at + 1] >> 16) & 0xF, r = t[x.at + 2];
        if (Type(r) == kInput) {
            if (use == 0) posIn |= 1u << idx;
            if (use == 3) nrmIn |= 1u << idx;
            continue;
        }
        if (Type(r) != kOutput) continue;
        maxOut = std::max(maxOut, static_cast<int>(Num(r)));
        afterLastOutDcl = End(x);
        if (use == 10 && idx == 0) col0 = static_cast<int>(Num(r));
        if (use == 5) tcUsed |= 1u << idx;
    }
    // POSITIONn together with NORMALn: morph targets (176 Phong VS blend POSITION1..3 / NORMAL1..3 with c26), fine;
    // POSITION1/2 alone would be instancing.
    if (posIn & ~nrmIn & ~1u) return false;
    // Objects: TEXCOORD8, which none of the 2139 Counters/Phong shaders use (Counters has the sink cut-out uv in
    // TEXCOORD7, Phong a projective coordinate); PatchObjectLampPs reads 8. Floors (texcoordOut) take the first free one
    // from 7 (census 25/09: many ExteriorFloors vertex shaders already write TEXCOORD7).
    int tcIdx = 8;
    if (texcoordOut) {
        tcIdx = 7;
        while (tcIdx < 16 && (tcUsed & (1u << tcIdx))) tcIdx++;
    }
    if ((needColor0 && col0 < 0) || tcIdx >= 16 || (tcUsed & (1u << tcIdx)) || maxOut < 0 || maxOut >= 11 || !afterLastOutDcl) return false;
    auto hasDst = [&](const Ins& x) {
        return x.len > 0 && x.op != kDcl && x.op != kDef && x.op != kDefI && x.op != kDefB && x.op != kTexkill && !IsFlowControl(x.op) && (t[x.at + 1] & 0x80000000u);
    };
    // the world position: dp4 rW.x / .y / .z, rP, cK / cK+1 / cK+2 outside branches. A triple stays open until something
    // else writes rP, or a component of rW it already has (split triples, Phong_VS_3875: dp4 r1.x ... 16 instructions ...
    // dp4 r1.z, dp4 r1.y).
    struct Tri {
        DWORD w, p;
        int k[3] = {-1, -1, -1};
        DWORD filled = 0;
        bool open = true;
        size_t first = SIZE_MAX, last = 0, lastEnd = 0;
    };
    std::vector<Tri> tris;
    int depth = 0;
    for (size_t i = 0; i < ins.size(); i++) {
        const Ins& x = ins[i];
        if (x.op == 0x28 || x.op == 0x29) depth++; // if, ifc
        if (x.op == 0x2B) depth--;                 // endif
        if (!hasDst(x)) continue;
        const DWORD d = t[x.at + 1], m = WMask(d);
        const bool temp = Type(d) == kTemp;
        int c = -1;
        if (depth == 0 && x.op == kDp4 && x.len == 3 && temp && Type(t[x.at + 2]) == kTemp && Swz(t[x.at + 2]) == kSwzXYZW && Type(t[x.at + 3]) == kConst &&
            !(t[x.at + 3] & 0x2000) /* c[a0 + n]: not a fixed matrix */)
            c = m == 0x1 ? 0 : m == 0x2 ? 1 : m == 0x4 ? 2 : -1;
        size_t tri = SIZE_MAX;
        if (c >= 0)
            for (size_t q = 0; q < tris.size(); q++)
                if (tris[q].open && tris[q].w == Num(d) && tris[q].p == Num(t[x.at + 2]) && tris[q].k[c] < 0) tri = q;
        if (temp)
            for (size_t q = 0; q < tris.size(); q++)
                if (q != tri && tris[q].open && (Num(d) == tris[q].p || (Num(d) == tris[q].w && (m & tris[q].filled)))) tris[q].open = false;
        if (c < 0) continue;
        if (tri == SIZE_MAX) {
            tris.push_back({Num(d), Num(t[x.at + 2])});
            tri = tris.size() - 1;
        }
        Tri& q = tris[tri];
        q.k[c] = static_cast<int>(Num(t[x.at + 3]));
        q.filled |= m;
        q.first = std::min(q.first, i);
        if (i >= q.last) {
            q.last = i;
            q.lastEnd = End(x);
        }
        if (q.w == q.p) q.open = false; // "dp4 r0.x, r0, cK": the source is gone
    }
    std::vector<const Tri*> full;
    for (const Tri& q : tris)
        if (q.k[0] >= 0 && q.k[1] == q.k[0] + 1 && q.k[2] == q.k[0] + 2) full.push_back(&q);
    const Tri* world = full.size() == 1 ? full[0] : nullptr;
    if (full.size() > 1) {
        // Several consecutive-constant triples (census 25/09, Phong VS_E3A718D3: world c19..c21, then a view triple
        // c12..c14 computed from the world position): keep the one whose source comes straight from the POSITION input.
        int posReg = -1;
        for (const Ins& x : ins)
            if (x.op == kDcl && Type(t[x.at + 2]) == kInput && (t[x.at + 1] & 0x1F) == 0 && ((t[x.at + 1] >> 16) & 0xF) == 0) posReg = static_cast<int>(Num(t[x.at + 2]));
        const Tri* fromPos = nullptr;
        int nFromPos = 0;
        for (const Tri* q : full) {
            if (posReg < 0) break;
            // last writer of the source register before the triple
            for (size_t i = q->first; i-- > 0;) {
                const Ins& x = ins[i];
                if (!hasDst(x) || !IsReg(t[x.at + 1], kTemp, q->p)) continue;
                bool readsPos = false;
                for (size_t k = 2; k <= x.len; k++) readsPos |= IsReg(t[x.at + k], kInput, static_cast<DWORD>(posReg));
                if (readsPos) {
                    fromPos = q;
                    nFromPos++;
                }
                break;
            }
        }
        if (nFromPos == 1) world = fromPos;
        else {
            // Otherwise the root: the one triple whose source does not depend on another triple's result (morphs and
            // skinning build the position first; Phong_VS_3810: world c195..c197, then a view-projection c188..c190 from
            // it). Forward taint from each other triple's result up to this triple's first dp4.
            int nRoot = 0;
            for (const Tri* q : full) {
                bool dep = false;
                for (const Tri* s : full) {
                    if (s == q || s->last >= q->first || s->w >= 32) continue;
                    uint32_t taint = 1u << s->w;
                    for (size_t i = s->last + 1; i < q->first; i++) {
                        const Ins& x = ins[i];
                        if (!hasDst(x) || Type(t[x.at + 1]) != kTemp || Num(t[x.at + 1]) >= 32) continue;
                        bool src = false;
                        for (size_t k = 2; k <= x.len; k++) {
                            const DWORD r = t[x.at + k];
                            if ((r & 0x80000000u) && Type(r) == kTemp && Num(r) < 32 && ((taint >> Num(r)) & 1)) src = true;
                        }
                        const DWORD r = Num(t[x.at + 1]);
                        if (src) taint |= 1u << r;
                        else if (WMask(t[x.at + 1]) == 0xF) taint &= ~(1u << r);
                    }
                    if (q->p < 32 && ((taint >> q->p) & 1)) dep = true;
                }
                if (!dep) {
                    world = q;
                    nRoot++;
                }
            }
            if (nRoot != 1) return false;
        }
    }
    if (!world) return false;
    // The rig's 4 vertex lights (every Counters/Phong vs_3_0, 25/09): "dp3_sat rS.s, cD, rN" (either order) then
    // "mul/mad rX.xyz, rS.s, cD+4[, rP]" for 4 consecutive directions cD0..cD0+3; the colours cD0+4..cD0+7 are c4, c8, c184
    // or c188. The last step writes COLOR0.xyz or feeds its last writer (Phong adds "mad oC0.xyz, vNormal.w, cAmbient, r").
    int vlK = -1;
    if (col0 >= 0) {
        auto isReplicate = [](DWORD swz) { return swz == 0x00 || swz == 0x55 || swz == 0xAA || swz == 0xFF; };
        int dirOf[32][4];
        for (auto& a : dirOf)
            for (int& v : a) v = -1;
        std::vector<char> pair(256, 0);
        std::vector<std::pair<size_t, int>> steps;
        for (size_t i = 0; i < ins.size(); i++) {
            const Ins& x = ins[i];
            if (!hasDst(x)) continue;
            const DWORD d = t[x.at + 1];
            if (((x.op == kMul && x.len == 3) || (x.op == kMad && x.len == 4)) && Type(t[x.at + 2]) == kTemp && Num(t[x.at + 2]) < 32 && isReplicate(Swz(t[x.at + 2])) &&
                Type(t[x.at + 3]) == kConst && !(t[x.at + 3] & 0x2000)) {
                const int dir = dirOf[Num(t[x.at + 2])][Swz(t[x.at + 2]) & 3], k = static_cast<int>(Num(t[x.at + 3]));
                if (dir >= 0 && dir < 252 && k == dir + 4) {
                    pair[dir] = 1;
                    steps.push_back({i, k});
                }
            }
            if (Type(d) != kTemp || Num(d) >= 32) continue;
            for (int c = 0; c < 4; c++)
                if ((WMask(d) >> c) & 1) dirOf[Num(d)][c] = -1;
            if (x.op == kDp3 && x.len == 3 && (d & 0x00100000u)) {
                const DWORD a = t[x.at + 2], b = t[x.at + 3], cr = Type(a) == kConst ? a : Type(b) == kConst ? b : 0;
                if (cr && !(cr & 0x2000))
                    for (int c = 0; c < 4; c++)
                        if ((WMask(d) >> c) & 1) dirOf[Num(d)][c] = static_cast<int>(Num(cr));
            }
        }
        int base = -1, groups = 0;
        for (int dd = 0; dd + 3 < 256; dd++)
            if (pair[dd] && pair[dd + 1] && pair[dd + 2] && pair[dd + 3]) {
                base = dd;
                groups++;
            }
        if (groups == 1) {
            const int K = base + 4;
            size_t li = SIZE_MAX, lw = SIZE_MAX;
            for (const auto& [i, k] : steps)
                if (k == K + 3) li = i;
            for (size_t i = 0; i < ins.size(); i++)
                if (hasDst(ins[i]) && IsReg(t[ins[i].at + 1], kOutput, static_cast<DWORD>(col0)) && (WMask(t[ins[i].at + 1]) & 0x7)) lw = i;
            bool feeds = li != SIZE_MAX && lw != SIZE_MAX && li == lw;
            if (li != SIZE_MAX && lw != SIZE_MAX && lw > li && Type(t[ins[li].at + 1]) == kTemp)
                for (size_t k = 2; k <= ins[lw].len; k++) feeds |= IsReg(t[ins[lw].at + k], kTemp, Num(t[ins[li].at + 1]));
            if (feeds) vlK = K;
        }
    }
    const DWORD N = static_cast<DWORD>(maxOut + 1), W = world->w;
    const size_t at = world->lastEnd;
    if (texcoordOut) *texcoordOut = tcIdx;
    if (worldConstOut) *worldConstOut = world->k[0];
    if (vertexLightOut) *vertexLightOut = vlK;
    // .xy = world xz (ground atlas uv), .z = world y (per-pixel lamps on objects)
    Apply(t, {{afterLastOutDcl, {Op(kDcl, 2), 0x80000005u | (static_cast<DWORD>(tcIdx) << 16) /* texcoordN */, Dst(kOutput, N, 0x7)}},
              {at, {Op(kMov, 2), Dst(kOutput, N, 0x7), Src(kTemp, W, 0xD8 /* xzyw */)}}});
    return true;
}

bool PatchObjectLampPs(std::vector<DWORD>& t, ObjectLampPatch& out, bool reach) {
    if (t.empty() || t[0] != 0xFFFF0300) return false;
    const auto all = Parse(t);
    if (all.empty()) return false;
    const Usage u = Scan(t, all);
    int colorReg = -1, maxIn = -1;
    size_t afterLastInDcl = 0;
    bool tc8 = false;
    std::vector<int> cubeSampler(16, 0);
    // Everything this patch reads and inserts must come before the first flow-control instruction (census 25/09: the
    // Phong PS_C249A5C0 ends with an if/else block after the lighting).
    size_t firstFlow = all.size();
    for (size_t i = 0; i < all.size(); i++)
        if (IsFlowControl(all[i].op)) {
            firstFlow = i;
            break;
        }
    const std::vector<Ins> ins(all.begin(), all.begin() + firstFlow);
    for (const Ins& x : all) {
        if (x.op != kDcl) continue;
        const DWORD use = t[x.at + 1] & 0x1F, idx = (t[x.at + 1] >> 16) & 0xF, r = t[x.at + 2];
        if (Type(r) == kSampler && Num(r) < 16) cubeSampler[Num(r)] = ((t[x.at + 1] >> 27) & 0xF) == 3;
        if (Type(r) != kInput) continue;
        maxIn = std::max(maxIn, static_cast<int>(Num(r)));
        afterLastInDcl = End(x);
        if (use == 10 && idx == 0 && (WMask(r) & 0x7) == 0x7) colorReg = static_cast<int>(Num(r)); // COLOR0: vertex lights
        if (use == 5 && idx == 8) tc8 = true;
    }
    if (tc8 || maxIn < 0 || maxIn >= 9 || !afterLastInDcl) return false;
    // last instruction before `before` that writes any of `mask`'s components of temp `reg`
    auto lastWriter = [&](DWORD reg, int before, DWORD mask) {
        for (int i = before - 1; i >= 0; i--) {
            const Ins& x = ins[i];
            if (x.op == kDcl || x.op == kDef || x.op == kDefI || x.op == kDefB || x.op == kTexkill || x.len == 0) continue;
            if (IsReg(t[x.at + 1], kTemp, reg) && (WMask(t[x.at + 1]) & mask)) return i;
        }
        return -1;
    };
    auto isReplicate = [](DWORD swz) { return swz == 0x00 || swz == 0x55 || swz == 0xAA || swz == 0xFF; };
    // the last rig-lamp step: "mad rD.xyz, rS.s, c7, rP" (the third lamp; rP = rD or another accumulator)
    auto chainEnd = [&](int i) {
        const Ins& L = ins[i];
        return L.op == kMad && L.len == 4 && Type(t[L.at + 1]) == kTemp && WMask(t[L.at + 1]) == 0x7 && Type(t[L.at + 2]) == kTemp && isReplicate(Swz(t[L.at + 2])) &&
               IsReg(t[L.at + 3], kConst, 7) && Type(t[L.at + 4]) == kTemp;
    };
    // "mad rA.xyz, rCube, cK.s, rD": the sky (a cube texld at the normal) plus the diffuse lamp chain rD, whose last step is
    // chainEnd. Returns the cube texld's index.
    auto isCubeTexld = [&](int i) {
        const Ins& K = ins[i];
        return K.op == kTexld && Type(t[K.at + 2]) == kTemp && Swz(t[K.at + 2]) == kSwzXYZW && Type(t[K.at + 3]) == kSampler && cubeSampler[Num(t[K.at + 3]) & 15];
    };
    auto skyAt = [&](int i) -> int {
        const Ins& S = ins[i];
        if (S.op != kMad || Type(t[S.at + 1]) != kTemp || WMask(t[S.at + 1]) != 0x7 || Type(t[S.at + 2]) != kTemp || Type(t[S.at + 3]) != kConst ||
            !isReplicate(Swz(t[S.at + 3])) || Type(t[S.at + 4]) != kTemp)
            return -1;
        const int wCube = lastWriter(Num(t[S.at + 2]), i, 0x7), wD = lastWriter(Num(t[S.at + 4]), i, 0x7);
        if (wCube < 0 || wD < 0 || !isCubeTexld(wCube) || !chainEnd(wD)) return -1;
        return wCube;
    };
    // Three shapes (offline analysis of the Counters/Phong shaders):
    //  A: rig lamps + sky cube (+ "add rX.xyz, rA, vC"): insert at the sky mad; the normal is the cube coordinate.
    //  B: the 4-light chain c0..c3 / c4..c7 with no cube and no COLOR0 (8 Counters): insert right after the chain end.
    //  C: no lamps in the pixel shader, light = max(vC, per-object light map, sky) (32 Phong): the lamps go into vC.
    enum { kA, kB, kC } shape = kA;
    int gi = -1, ref = -1, anchor = -1;
    DWORD Nrm = 0, D = 0;
    if (colorReg >= 0) {
        // with vertex lights: the single "add rX.xyz, rA, vC" (the destination may be another register: generic objects
        // write r4 from r0); the sky mad is rA's last writer
        const DWORD C = static_cast<DWORD>(colorReg);
        int add = -1, found = 0;
        DWORD A = 0;
        for (size_t i = 0; i < ins.size(); i++) {
            const Ins& x = ins[i];
            if (x.op != kAdd || Type(t[x.at + 1]) != kTemp || WMask(t[x.at + 1]) != 0x7) continue;
            const bool c2 = IsReg(t[x.at + 2], kInput, C) && Swz(t[x.at + 2]) == kSwzXYZW && Type(t[x.at + 3]) == kTemp;
            const bool c3 = IsReg(t[x.at + 3], kInput, C) && Swz(t[x.at + 3]) == kSwzXYZW && Type(t[x.at + 2]) == kTemp;
            if (c2 || c3) {
                add = static_cast<int>(i);
                A = Num(t[x.at + (c2 ? 3 : 2)]);
                found++;
            }
        }
        if (found == 1) {
            anchor = lastWriter(A, add, 0x7);
            if (anchor >= 0) ref = skyAt(anchor);
        }
        if (found != 1 || ref < 0) {
            // shape C: the single "max rX.xyz, vC, rY" (either order) and the single cube texld (its coordinate: the normal)
            if (found != 0) return false;
            int mx = -1, nMax = 0, cube = -1, nCube = 0;
            for (size_t i = 0; i < ins.size(); i++) {
                const Ins& x = ins[i];
                if (isCubeTexld(static_cast<int>(i))) {
                    cube = static_cast<int>(i);
                    nCube++;
                }
                if (x.op != kMax || x.len != 3 || Type(t[x.at + 1]) != kTemp || WMask(t[x.at + 1]) != 0x7) continue;
                for (int k = 2; k <= 3; k++)
                    if (IsReg(t[x.at + k], kInput, C) && Swz(t[x.at + k]) == kSwzXYZW && !(t[x.at + k] & 0x0F000000u) && Type(t[x.at + 5 - k]) == kTemp) {
                        mx = static_cast<int>(i);
                        nMax++;
                    }
            }
            if (nMax != 1 || nCube != 1) return false;
            shape = kC;
            anchor = mx;
            // the lamp code goes at the cube texld when it comes first (Phong_PS_3113 overwrites the normal before the max)
            gi = cube < mx ? cube : mx;
            ref = cube;
            Nrm = Num(t[ins[cube].at + 2]);
        } else {
            gi = ref;
            Nrm = Num(t[ins[ref].at + 2]);
            D = Num(t[ins[anchor].at + 4]);
        }
    } else {
        // no vertex lights (objects lit by the rig only, e.g. PS_298DF5B8): the single sky mad of that shape
        int found = 0;
        for (size_t i = 0; i < ins.size(); i++)
            if (skyAt(static_cast<int>(i)) >= 0) {
                anchor = static_cast<int>(i);
                found++;
            }
        if (found == 1) {
            ref = gi = skyAt(anchor);
            Nrm = Num(t[ins[ref].at + 2]);
            D = Num(t[ins[anchor].at + 4]);
        } else {
            // shape B (Counters_PS_440): no sky; the single chain end, whose result is the light
            if (found != 0) return false;
            int nEnd = 0;
            for (size_t i = 0; i < ins.size(); i++)
                if (chainEnd(static_cast<int>(i))) {
                    anchor = static_cast<int>(i);
                    nEnd++;
                }
            if (nEnd != 1) return false;
            // the normal: the one register lit by both the first lamp (c1) and the sun / first light (c0, c9 or c13)
            int nN = 0;
            for (int r = 0; r < 32; r++) {
                int r1 = -1;
                bool sunR = false;
                for (int i = 0; i < anchor; i++) {
                    const Ins& x = ins[i];
                    if (x.op != kDp3 || x.len != 3) continue;
                    for (int k = 2; k <= 3; k++) {
                        if (!IsReg(t[x.at + k], kTemp, static_cast<DWORD>(r)) || Swz(t[x.at + k]) != kSwzXYZW) continue;
                        const DWORD o = t[x.at + 5 - k];
                        if (IsReg(o, kConst, 1)) r1 = i;
                        if (IsReg(o, kConst, 0) || IsReg(o, kConst, 9) || IsReg(o, kConst, 13)) sunR = true;
                    }
                }
                if (r1 >= 0 && sunR) {
                    Nrm = static_cast<DWORD>(r);
                    ref = r1;
                    nN++;
                }
            }
            if (nN != 1) return false;
            shape = kB;
            gi = anchor;
            D = Num(t[ins[anchor].at + 1]);
        }
    }
    // the normal must still hold the same value where the new code reads it
    if (lastWriter(Nrm, gi, 0x7) != lastWriter(Nrm, ref, 0x7)) return false;
    if (shape != kC) {
        // that normal also lights the first lamp ("dp3 ..., rN, c1") and the sun: c9 in the 3-lamp shaders, c0 in the
        // 4-light ones (PS_2D041628: c0..c3 directions, c4..c7 colours), c13 in Phong (census 25/09)
        bool sun = false, lamp1 = false;
        for (size_t i = 0; i < ins.size() && static_cast<int>(i) < std::max(gi, anchor); i++) {
            const Ins& x = ins[i];
            if (x.op != kDp3) continue;
            for (int k = 2; k <= 3; k++) {
                if (!IsReg(t[x.at + k], kTemp, Nrm)) continue;
                const DWORD o = t[x.at + 5 - k];
                if (IsReg(o, kConst, 9) || IsReg(o, kConst, 0) || IsReg(o, kConst, 13)) sun = true;
                if (IsReg(o, kConst, 1)) lamp1 = true;
            }
        }
        if (!sun || !lamp1) return false;
    }
    // reach: 2 more rows (the r0^2 of the 8 lamps) after the cones
    if (u.maxSampler < 0 || u.maxSampler >= 15 || !u.afterLastSamplerDcl || u.maxConst + 4 + 4 * kObjectPixelLamps + (reach ? 2 : 0) >= 224 || u.maxTemp + 5 >= 32) return false;
    const DWORD E = static_cast<DWORD>(u.maxSampler + 1), T = static_cast<DWORD>(u.maxTemp + 1), Fr = T + 1;
    const DWORD cA = static_cast<DWORD>(u.maxConst + 1), cB = cA + 1, cH = cA + 2, V = static_cast<DWORD>(maxIn + 1);
    const DWORD cS = cA + 3, cL = cA + 4; // per-pixel lamps: parameters, then kObjectPixelLamps x (pos + 1/R^2, colour)
    const DWORD A = T + 2, B = T + 3, Q = T + 4;
    const DWORD cR = cL + 4 * kObjectPixelLamps; // reach: r0^2 of lamps 0..3 / 4..7, right after the cones (one upload from cS)
    out.atlasSampler = E;
    out.atlasConst = cA;
    out.strengthConst = cB;
    out.lampParamConst = cS;
    out.lampConst = cL;
    out.coneConst = cL + 2 * kObjectPixelLamps;
    out.reachConst = reach ? cR : 0;
    out.rigLamps = shape != kC;
    std::vector<DWORD> ground = {Op(kMad, 4), Dst(kTemp, T, 0x3), Src(kInput, V, kSwzXYXY), Src(kConst, cA), Src(kConst, cA, kSwzZWZW),
                                 Op(kTexld, 3), Dst(kTemp, T), Src(kTemp, T), Src(kSampler, E),
                                 Op(kMad, 4), Dst(kTemp, Fr, 0x8), Src(kTemp, Nrm, kSwzY), Src(kConst, cH, kSwzX), Src(kConst, cH, kSwzY),
                                 // In direct-pixel mode the directionless terrain
                                 // atlas belongs to upward surfaces, not gate faces.
                                 Op(kMov, 2), Dst(kTemp, A, 0x8, true), Src(kTemp, Nrm, kSwzY),
                                 Op(kLrp, 4), Dst(kTemp, A, 0x8), Src(kConst, cS, kSwzX), Src(kTemp, A, kSwzW), Src(kTemp, Fr, kSwzW),
                                 Op(kMul, 3), Dst(kTemp, Fr, 0x8), Src(kTemp, A, kSwzW), Src(kConst, cB, kSwzX),
                                 Op(kMul, 3), Dst(kTemp, T, 0x7), Src(kTemp, T), Src(kTemp, Fr, kSwzW)};
    if (shape == kA && colorReg >= 0) // the vertex lights are added after the max: take them out of the ground light first
        ground.insert(ground.end(), {Op(kAdd, 3), Dst(kTemp, T, 0x7), Src(kTemp, T), Src(kInput, static_cast<DWORD>(colorReg)) | 0x01000000u /* -vC */});
    // Per-pixel lamps (requested for Counters): each piece of a modular object gets its own rig
    // evaluated at the piece's centre (3 lamps in c5..c7 here, 4 more as vertex lights), so neighbouring pieces differ.
    // Q = the same world lamps for every pixel: sum of colour * sat(N.l) * sat(1 - d^2/R^2)^2 at the pixel's world
    // position (TEXCOORD8.xzy); the reach variant: the outdoor walls' law (AppendLampLaw), held under 1 like their texels.
    // The draw zeroes the rig (PS c5..c7 and the VS vertex-light colours) while cS.y > 0.
    // cS = (valid direct-pixel mode, lamp strength, unused, 1e-4).
    constexpr DWORD kNeg = 0x01000000u;
    const DWORD pw = Src(kInput, V, 0xD8 /* xzyw: world x, y, z */);
    for (DWORD k = 0; k < kObjectPixelLamps; k++) {
        const DWORD cp = cL + 2 * k, cc = cp + 1;
        // l = lamp - pixel, d^2, sat(N.l) and the falloff, one constant register per instruction (final review 30/09: a
        // two-constant mad made native D3D9 refuse the shader); the reach variant: the outdoor walls' law (AppendLampLaw)
        AppendLampLaw(ground, A, B, Nrm, pw | kNeg, cp, cS, cH, reach, cR, k);
        const DWORD k1 = out.coneConst + 2 * k, k2 = k1 + 1;
        ground.insert(ground.end(), {
            Op(kDp3, 3), Dst(kTemp, B, 0x4), Src(kTemp, A), Src(kConst, k1),
            Op(kAdd, 3), Dst(kTemp, B, 0x4), Src(kTemp, B, 0xAA), Src(kConst, k1, kSwzW),
            Op(kDp3, 3), Dst(kTemp, B, 0x8), Src(kTemp, A), Src(kConst, k2),
            Op(kAdd, 3), Dst(kTemp, B, 0x8), Src(kTemp, B, kSwzW), Src(kConst, k2, kSwzW),
            Op(kMax, 3), Dst(kTemp, B, 0x4, true), Src(kTemp, B, 0xAA), Src(kTemp, B, kSwzW),
            Op(kMad, 4), Dst(kTemp, B, 0x8), Src(kTemp, B, 0xAA), Src(kConst, cc, kSwzW) | kNeg, Src(kTemp, B, 0xAA),
            Op(kAdd, 3), Dst(kTemp, B, 0x4), Src(kTemp, B, kSwzW), Src(kConst, cc, kSwzW),
            Op(kMul, 3), Dst(kTemp, B, 0x1), Src(kTemp, B, kSwzX), Src(kTemp, B, 0xAA)});
        if (k == 0) ground.insert(ground.end(), {Op(kMul, 3), Dst(kTemp, Q, 0x7), Src(kConst, cc), Src(kTemp, B, kSwzX)});
        else ground.insert(ground.end(), {Op(kMad, 4), Dst(kTemp, Q, 0x7), Src(kConst, cc), Src(kTemp, B, kSwzX), Src(kTemp, Q)});
    }
    // reach: the sum is held under 1 as an outdoor wall's texel is (the room map's min(sum, 1)), by its brightest channel
    if (reach) AppendHueClamp(ground, Q, B);
    std::vector<Edit> edits = {{1, {Op(kDef, 5), Dst(kConst, cH), F(0.5f), F(0.5f), F(1.0f), F(0.0f)}},
                               {afterLastInDcl, {Op(kDcl, 2), 0x80080005u /* texcoord8 */, Dst(kInput, V, 0x7)}},
                               {u.afterLastSamplerDcl, {Op(kDcl, 2), 0x90000000u, Dst(kSampler, E)}}};
    if (shape == kC) {
        // before "max rX.xyz, vC, rY": rA = max(vC + Q * cS.y, ground), and the max reads rA instead of vC
        const Ins& M = ins[anchor];
        const std::vector<DWORD> atMax = {Op(kMad, 4), Dst(kTemp, A, 0x7), Src(kTemp, Q), Src(kConst, cS, kSwzY), Src(kInput, static_cast<DWORD>(colorReg)),
                                          Op(kMax, 3), Dst(kTemp, A, 0x7), Src(kTemp, A), Src(kTemp, T)};
        for (size_t k = 2; k <= 3; k++)
            if (IsReg(t[M.at + k], kInput, static_cast<DWORD>(colorReg))) t[M.at + k] = Src(kTemp, A);
        if (gi == anchor) {
            ground.insert(ground.end(), atMax.begin(), atMax.end());
            edits.push_back({M.at, ground});
        } else {
            edits.push_back({ins[gi].at, ground});
            edits.push_back({M.at, atMax});
        }
        Apply(t, edits);
        return true;
    }
    // A: at the sky mad (before it), B: right after the chain end: rD = max(rD + Q * cS.y, ground)
    const std::vector<DWORD> atEnd = {Op(kMad, 4), Dst(kTemp, D, 0x7), Src(kTemp, Q), Src(kConst, cS, kSwzY), Src(kTemp, D),
                                      Op(kMax, 3), Dst(kTemp, D, 0x7), Src(kTemp, D), Src(kTemp, T)};
    if (shape == kB) {
        // the chain end may overwrite the normal (Counters_PS_440: r1 = normal): the ground and lamp code goes before it,
        // only the final mad / max after it
        edits.push_back({ins[gi].at, ground});
        edits.push_back({End(ins[anchor]), atEnd});
    } else {
        edits.push_back({ins[gi].at, ground});
        edits.push_back({ins[anchor].at, atEnd});
    }
    Apply(t, edits);
    return true;
}

bool IsSnowReliefVs(const std::vector<DWORD>& t) {
    if (t.empty() || t[0] != 0xFFFE0300) return false;
    const auto ins = Parse(t);
    if (ins.empty()) return false;
    int tc4Out = -1;
    bool tc2In = false;
    for (const Ins& x : ins) {
        if (x.op != kDcl) continue;
        const DWORD use = t[x.at + 1] & 0x1F, idx = (t[x.at + 1] >> 16) & 0xF, r = t[x.at + 2];
        if (Type(r) == kOutput && use == 5 && idx == 4 && (WMask(r) & 0xC) == 0xC) tc4Out = static_cast<int>(Num(r)); // TEXCOORD4 with zw
        if (Type(r) == kInput && use == 5 && idx == 2) tc2In = true; // TEXCOORD2: the snow's base position (roofs, same family, have none)
    }
    if (tc4Out < 0 || !tc2In) return false;
    int world = -1, found = 0;
    DWORD half = 0;
    for (const Ins& x : ins) {
        // mul oT4.zw, rW.xyxz, cD.x
        if (x.op == kMul && IsReg(t[x.at + 1], kOutput, static_cast<DWORD>(tc4Out)) && WMask(t[x.at + 1]) == 0xC && Type(t[x.at + 2]) == kTemp &&
            Swz(t[x.at + 2]) == kSwzXYXZ && Type(t[x.at + 3]) == kConst && Swz(t[x.at + 3]) == kSwzX) {
            world = static_cast<int>(Num(t[x.at + 2]));
            half = Num(t[x.at + 3]);
            found++;
        }
    }
    if (found != 1) return false;
    bool halfOk = false, dx = false, dz = false;
    for (const Ins& x : ins) {
        if (x.op == kDef && IsReg(t[x.at + 1], kConst, half)) {
            float v;
            memcpy(&v, &t[x.at + 2], 4);
            halfOk = v == 0.5f;
        }
        if (x.op != kDp4 || !IsReg(t[x.at + 1], kTemp, static_cast<DWORD>(world))) continue;
        if (WMask(t[x.at + 1]) == 0x1 && IsReg(t[x.at + 3], kConst, 8)) dx = true;
        if (WMask(t[x.at + 1]) == 0x4 && IsReg(t[x.at + 3], kConst, 10)) dz = true;
    }
    return halfOk && dx && dz;
}

bool PatchSnowRelief(std::vector<DWORD>& t, SnowCoverPatch& out) {
    if (t.empty() || t[0] != 0xFFFF0300) return false;
    const auto ins = Parse(t);
    if (ins.empty()) return false;
    const Usage u = Scan(t, ins);
    int tc4 = -1;
    bool cube0 = false;
    for (const Ins& x : ins) {
        if (x.op != kDcl) continue;
        const DWORD use = t[x.at + 1] & 0x1F, idx = (t[x.at + 1] >> 16) & 0xF, r = t[x.at + 2];
        if (Type(r) == kInput && use == 5 && idx == 4 && (WMask(r) & 0xC) == 0xC) tc4 = static_cast<int>(Num(r)); // TEXCOORD4: zw = world xz / 2
        if (IsReg(r, kSampler, 0) && ((t[x.at + 1] >> 27) & 0xF) == 3) cube0 = true;                            // dcl_cube s0
    }
    if (tc4 < 0 || !cube0) return false;
    // "texld rC, rN, s0" and the single "mad rL.xyz, rC, cK.x, rS" outside any loop
    int mad = -1, found = 0, depth = 0;
    int cubeReg = -1;
    for (size_t i = 0; i < ins.size(); i++) {
        const Ins& x = ins[i];
        if (x.op == 0x26 || x.op == 0x1B) depth++; // rep, loop
        if (x.op == 0x27 || x.op == 0x1D) depth--; // endrep, endloop
        if (x.op == kTexld && Type(t[x.at + 1]) == kTemp && IsReg(t[x.at + 3], kSampler, 0)) {
            cubeReg = static_cast<int>(Num(t[x.at + 1]));
            continue;
        }
        if (cubeReg >= 0 && depth == 0 && x.op == kMad && Type(t[x.at + 1]) == kTemp && WMask(t[x.at + 1]) == 0x7 &&
            IsReg(t[x.at + 2], kTemp, static_cast<DWORD>(cubeReg)) && Type(t[x.at + 3]) == kConst && Swz(t[x.at + 3]) == kSwzX && Type(t[x.at + 4]) == kTemp) {
            mad = static_cast<int>(i);
            found++;
        }
    }
    if (found != 1 || u.maxSampler < 0 || u.maxSampler >= 15 || !u.afterLastSamplerDcl || u.maxConst + 2 >= 224 || u.maxTemp + 1 >= 32) return false;
    const DWORD L = Num(t[ins[mad].at + 1]);
    const DWORD E = static_cast<DWORD>(u.maxSampler + 1), T = static_cast<DWORD>(u.maxTemp + 1);
    const DWORD cA = static_cast<DWORD>(u.maxConst + 1), cB = cA + 1, V = static_cast<DWORD>(tc4);
    out.atlasSampler = E;
    out.atlasConst = cA;
    out.strengthConst = cB;
    Apply(t, {{u.afterLastSamplerDcl, {Op(kDcl, 2), 0x90000000u, Dst(kSampler, E)}},
              {End(ins[mad]), {Op(kMad, 4), Dst(kTemp, T, 0x3), Src(kInput, V, kSwzZWZW), Src(kConst, cA), Src(kConst, cA, kSwzZWZW),
                               Op(kTexld, 3), Dst(kTemp, T), Src(kTemp, T), Src(kSampler, E),
                               Op(kMad, 4), Dst(kTemp, L, 0x7), Src(kTemp, T), Src(kConst, cB, kSwzX), Src(kTemp, L)}}});
    return true;
}


// ---- Smooth room light maps indoors (Night Lights "Even light along walls", 2026-09-29). The 4 directional room light
// maps ("basis maps", 64x64 over the 64 m lot: 1 texel per metre) are read with the bilinear filter, so their 1 m grid
// shows as bands and steps on stairs and furniture. A bicubic B-spline read (4 bilinear taps, GPU Gems 2 ch. 20) makes
// the light change in smooth curves. Shared by both patches below; everything in xy (the map uv), 64 texels. ----
namespace {
constexpr DWORD kFrc = 0x13, kRcp = 0x06, kDp2add = 0x5A, kNrm = 0x24, kMin = 0x0A;
// PatchIndoorBasis: the basis light is kept under this many times the room light map at the same place (see there)
constexpr float kBasisCap = 2.0f;
DWORD Sw(int a, int b, int c, int d) { return static_cast<DWORD>(a | (b << 2) | (c << 4) | (d << 6)); }
DWORD Neg(DWORD src) { return src | 0x01000000u; }

// Temps used: T .. T+10. Leaves the 4 tap uvs in T+6 .. T+9 and the 4 tap weights in T+10 (x..w).
// cS = (w, h, 1/w, 1/h) of the map (set per draw), cK = (1/6, 2/3, 0.5, 1), cH = (-0.5, 1.5, 1/64, 0). uv = input or
// temp register (type, num), read as .xy. beforeScale: instructions placed where B holds the tap coordinates in texels
// (B.xy = u0 v0, B.zw = u1 v1; P = the integer texel, F = the fraction; A and Q are free there), before they become uvs.
std::vector<DWORD> BicubicSetup(DWORD uvType, DWORD uvNum, DWORD cS, DWORD cK, DWORD cH, DWORD T, const std::vector<DWORD>& beforeScale = {}) {
    const DWORD P = T, F = T + 1, Q = T + 2, A = T + 3, B = T + 4, G = T + 5, W1 = T + 10; // W1 ends as the tap weights
    const DWORD C0 = T + 6, C1 = T + 7, C2 = T + 8, C3 = T + 9;
    const DWORD X = Sw(0, 0, 0, 0), Y = Sw(1, 1, 1, 1), Z = Sw(2, 2, 2, 2), W = Sw(3, 3, 3, 3);
    std::vector<DWORD> head = {
        // p = uv*size - 0.5, in two steps: native D3D9 takes one constant register per instruction (30/09 review)
        Op(kMul, 3), Dst(kTemp, P, 0x3), Src(uvType, uvNum), Src(kConst, cS, Sw(0, 1, 0, 1)),
        Op(kAdd, 3), Dst(kTemp, P, 0x3), Src(kTemp, P), Src(kConst, cH, X),
        Op(kFrc, 2), Dst(kTemp, F, 0x3), Src(kTemp, P),                                              // f
        Op(kAdd, 3), Dst(kTemp, P, 0x3), Src(kTemp, P), Neg(Src(kTemp, F)),                          // i = p - f
        Op(kMul, 3), Dst(kTemp, Q, 0x3), Src(kTemp, F), Src(kTemp, F),                               // Q.xy = f^2
        Op(kMul, 3), Dst(kTemp, Q, 0xC), Src(kTemp, Q, Sw(0, 0, 0, 1)), Src(kTemp, F, Sw(0, 0, 0, 1)), // Q.zw = f^3
        // w1 = 0.5 f^3 - f^2 + 2/3 (W1.xy)
        Op(kMad, 4), Dst(kTemp, W1, 0x3), Src(kTemp, Q, Sw(2, 3, 2, 3)), Src(kConst, cK, Z), Neg(Src(kTemp, Q, Sw(0, 1, 0, 1))),
        Op(kAdd, 3), Dst(kTemp, W1, 0x3), Src(kTemp, W1), Src(kConst, cK, Y),
        // w0 = (1-f)^3 / 6 (B.xy)
        Op(kAdd, 3), Dst(kTemp, A, 0x3), Neg(Src(kTemp, F)), Src(kConst, cK, W),
        Op(kMul, 3), Dst(kTemp, B, 0x3), Src(kTemp, A), Src(kTemp, A),
        Op(kMul, 3), Dst(kTemp, B, 0x3), Src(kTemp, B), Src(kTemp, A),
        Op(kMul, 3), Dst(kTemp, B, 0x3), Src(kTemp, B), Src(kConst, cK, X),
        // g0 = w0 + w1 (G.xy), g1 = 1 - g0 (G.zw)
        Op(kAdd, 3), Dst(kTemp, G, 0x3), Src(kTemp, B), Src(kTemp, W1),
        Op(kAdd, 3), Dst(kTemp, G, 0xC), Neg(Src(kTemp, G, Sw(0, 1, 0, 1))), Src(kConst, cK, W),
        // 1/g (A.xyzw)
        Op(kRcp, 2), Dst(kTemp, A, 0x1), Src(kTemp, G, X),
        Op(kRcp, 2), Dst(kTemp, A, 0x2), Src(kTemp, G, Y),
        Op(kRcp, 2), Dst(kTemp, A, 0x4), Src(kTemp, G, Z),
        Op(kRcp, 2), Dst(kTemp, A, 0x8), Src(kTemp, G, W),
        // offsets: B.xy = w1/g0, B.zw = w3/g1 with w3 = f^3/6
        Op(kMul, 3), Dst(kTemp, B, 0x3), Src(kTemp, W1), Src(kTemp, A),
        Op(kMul, 3), Dst(kTemp, B, 0xC), Src(kTemp, Q), Src(kConst, cK, X),
        Op(kMul, 3), Dst(kTemp, B, 0xC), Src(kTemp, B), Src(kTemp, A),
        // tap coordinates in texels: xy = i - 0.5 + w1/g0, zw = i + 1.5 + w3/g1; then / size
        Op(kAdd, 3), Dst(kTemp, B, 0x3), Src(kTemp, B), Src(kTemp, P),
        Op(kAdd, 3), Dst(kTemp, B, 0x3), Src(kTemp, B), Src(kConst, cH, X),
        Op(kAdd, 3), Dst(kTemp, B, 0xC), Src(kTemp, B), Src(kTemp, P, Sw(0, 1, 0, 1)),
        Op(kAdd, 3), Dst(kTemp, B, 0xC), Src(kTemp, B), Src(kConst, cH, Y),
    };
    head.insert(head.end(), beforeScale.begin(), beforeScale.end());
    const std::vector<DWORD> tail = {
        Op(kMul, 3), Dst(kTemp, B, 0xF), Src(kTemp, B), Src(kConst, cS, Sw(2, 3, 2, 3)),
        // the 4 tap uvs: (u0 v0) (u1 v0) (u0 v1) (u1 v1)
        Op(kMov, 2), Dst(kTemp, C0, 0xF), Src(kTemp, B, Sw(0, 1, 0, 1)),
        Op(kMov, 2), Dst(kTemp, C1, 0xF), Src(kTemp, B, Sw(2, 1, 2, 1)),
        Op(kMov, 2), Dst(kTemp, C2, 0xF), Src(kTemp, B, Sw(0, 3, 0, 3)),
        Op(kMov, 2), Dst(kTemp, C3, 0xF), Src(kTemp, B, Sw(2, 3, 2, 3)),
        // weights: (g0x g0y, g1x g0y, g0x g1y, g1x g1y)
        Op(kMul, 3), Dst(kTemp, W1, 0xF), Src(kTemp, G, Sw(0, 2, 0, 2)), Src(kTemp, G, Sw(1, 1, 3, 3)),
    };
    head.insert(head.end(), tail.begin(), tail.end());
    return head;
}

// dst.xyz = bicubic read of sampler S, averaged over the texels inside the house only: the maps are black with alpha 0
// outside and alpha 1 inside (Light Probe captures of 29/09), so the filtered rgb / filtered alpha is the mean of the
// inside texels under the filter; the edge no longer pulls towards black, without touching the game's textures.
// (Taps in T+6..T+9, weights in T+10; T+2, T+3, T+4 as scratch; cH.z = the smallest alpha divided by.)
std::vector<DWORD> BicubicTaps(DWORD S, DWORD dst, DWORD T, DWORD cH, bool preserveShadows=false) {
    const DWORD R = T + 2, Acc = T + 3, Inv = T + 4, Wt = T + 10;
    std::vector<DWORD> v;
    if (preserveShadows) {
        // The original texld remains before these taps. Normalize its house-plan
        // mask just like the smooth read, then keep it as a per-channel ceiling.
        v.insert(v.end(), {Op(kMax,3),Dst(kTemp,Inv,0x8),Src(kTemp,dst,Sw(3,3,3,3)),Src(kConst,cH,Sw(2,2,2,2)),
                           Op(kRcp,2),Dst(kTemp,Inv,0x8),Src(kTemp,Inv,Sw(3,3,3,3)),
                           Op(kMul,3),Dst(kTemp,Inv,0x7),Src(kTemp,dst),Src(kTemp,Inv,Sw(3,3,3,3))});
    }
    for (int k = 0; k < 4; k++) {
        v.insert(v.end(), {Op(kTexld, 3), Dst(kTemp, R), Src(kTemp, T + 6 + static_cast<DWORD>(k)), Src(kSampler, S)});
        if (k == 0) v.insert(v.end(), {Op(kMul, 3), Dst(kTemp, Acc), Src(kTemp, R), Src(kTemp, Wt, Sw(0, 0, 0, 0))});
        else v.insert(v.end(), {Op(kMad, 4), Dst(kTemp, Acc), Src(kTemp, R), Src(kTemp, Wt, Sw(k, k, k, k)), Src(kTemp, Acc)});
    }
    v.insert(v.end(), {Op(kMax, 3), Dst(kTemp, Inv, 0x8), Src(kTemp, Acc, Sw(3, 3, 3, 3)), Src(kConst, cH, Sw(2, 2, 2, 2)),
                       Op(kRcp, 2), Dst(kTemp, Inv, 0x8), Src(kTemp, Inv, Sw(3, 3, 3, 3)),
                       Op(kMul, 3), Dst(kTemp, dst, 0x7), Src(kTemp, Acc), Src(kTemp, Inv, Sw(3, 3, 3, 3))});
    if (preserveShadows) v.insert(v.end(),{Op(kMin,3),Dst(kTemp,dst,0x7),Src(kTemp,dst),Src(kTemp,Inv)});
    return v;
}

// The 4 basis reads of a basis-reading pixel shader, from its dp2add weights (the def (0.8944, 0.4472, 0, -0.8944) =
// cD): "dp2add rW.c, rN.xy|yz, cD.<swz>, cD.z", then "mul/mad rX.xyz, rT, rW.c, .." with rT from "texld rT, vU, sK".
// dir 0 = +X, 1 = -X, 2 = +Z, 3 = -Z.
struct BasisReads {
    int sampler[4] = {-1, -1, -1, -1};
    int texld[4] = {-1, -1, -1, -1};
    int uvType = -1, uvNum = -1;
};
bool FindBasisReads(const std::vector<DWORD>& t, const std::vector<Ins>& ins, BasisReads& out) {
    int cD = -1;
    for (const Ins& x : ins)
        if (x.op == kDef) {
            float f[4];
            std::memcpy(f, &t[x.at + 2], sizeof f);
            if (std::fabs(f[0] - 0.8944f) < 1e-3f && std::fabs(f[1] - 0.4472f) < 1e-3f && f[2] == 0.0f && std::fabs(f[3] + 0.8944f) < 1e-3f)
                cD = static_cast<int>(Num(t[x.at + 1]));
        }
    if (cD < 0) return false;
    // Follow the direction weights through the shader: dirOf[temp][component] = the direction whose dp2add weight that
    // component holds (copied by mov / mov_sat, dropped when overwritten by anything else). A mul / mad that multiplies a
    // map read by such a weight (replicated component) gives that map's direction. Both forms seen in game: the plain one
    // ("dp2add_sat r0.w, ..; mul r2.xyz, r2, r0.w") and the glossy one (4 dp2add into r1.xyzw, "mov_sat r1, r1", then the
    // diffuse "mul r2.xyz, r1.y, r0"; its specular uses pow(weights), which are not followed).
    int dirOf[32][4];
    for (auto& r : dirOf)
        for (int& c : r) c = -1;
    auto writeMask = [&](const Ins& x) { return x.len >= 1 && Type(t[x.at + 1]) == kTemp && Num(t[x.at + 1]) < 32 ? WMask(t[x.at + 1]) : 0u; };
    for (size_t i = 0; i < ins.size(); i++) {
        const Ins& x = ins[i];
        // a map read multiplied by a direction weight
        if ((x.op == kMul && x.len == 3) || (x.op == kMad && x.len == 4)) {
            for (int wArg = 2; wArg <= 3; wArg++) {
                const DWORD w = t[x.at + static_cast<size_t>(wArg)];
                if (Type(w) != kTemp || Num(w) >= 32 || (w & 0x0F000000u)) continue;
                const DWORD sw = Swz(w);
                const int c = static_cast<int>(sw & 3);
                if (sw != static_cast<DWORD>(Sw(c, c, c, c)) || dirOf[Num(w)][c] < 0) continue;
                const int dir = dirOf[Num(w)][c];
                const DWORD mapReg = t[x.at + static_cast<size_t>(wArg == 2 ? 3 : 2)];
                if (Type(mapReg) != kTemp) continue;
                for (int k = static_cast<int>(i) - 1; k >= 0; k--) {
                    const Ins& z = ins[static_cast<size_t>(k)];
                    if (z.len >= 1 && IsReg(t[z.at + 1], kTemp, Num(mapReg)) && (WMask(t[z.at + 1]) & 0x7)) {
                        if (z.op == kTexld && Type(t[z.at + 3]) == kSampler && Swz(t[z.at + 2]) == kSwzXYZW && out.texld[dir] < 0) {
                            out.sampler[dir] = static_cast<int>(Num(t[z.at + 3]));
                            out.texld[dir] = k;
                        }
                        break;
                    }
                }
            }
        }
        // then this instruction's own write
        const DWORD m = writeMask(x);
        if (!m) continue;
        const DWORD D = Num(t[x.at + 1]);
        if (x.op == kDp2add && x.len == 4 && IsReg(t[x.at + 3], kConst, static_cast<DWORD>(cD))) {
            const DWORD n = t[x.at + 2], c = t[x.at + 3];
            const int n0 = static_cast<int>(Swz(n) & 3), c0 = static_cast<int>(Swz(c) & 3), c1 = static_cast<int>((Swz(c) >> 2) & 3);
            // normal pair (x,y) or (y,z) with constant pair (.x .y) = +X, (.w .y) = -X, (.y .x) = +Z, (.y .w) = -Z
            int dir = -1;
            if (n0 == 0 && c0 == 0 && c1 == 1) dir = 0;
            else if (n0 == 0 && c0 == 3 && c1 == 1) dir = 1;
            else if (n0 == 1 && c0 == 1 && c1 == 0) dir = 2;
            else if (n0 == 1 && c0 == 1 && c1 == 3) dir = 3;
            for (int k = 0; k < 4; k++)
                if (m & (1u << k)) dirOf[D][k] = dir;
        } else if (x.op == kMov && x.len == 2 && Type(t[x.at + 2]) == kTemp && Num(t[x.at + 2]) < 32 && !(t[x.at + 2] & 0x0F000000u)) {
            const DWORD S = Num(t[x.at + 2]), sw = Swz(t[x.at + 2]);
            int copy[4];
            for (int k = 0; k < 4; k++) copy[k] = dirOf[S][(sw >> (2 * k)) & 3];
            for (int k = 0; k < 4; k++)
                if (m & (1u << k)) dirOf[D][k] = copy[k];
        } else
            for (int k = 0; k < 4; k++)
                if (m & (1u << k)) dirOf[D][k] = -1;
    }
    for (int d = 0; d < 4; d++)
        if (out.sampler[d] < 0) return false;
    // four different maps, read at the same uv (the full register: the room light map is read at its .zw)
    for (int d = 0; d < 4; d++) {
        for (int e = d + 1; e < 4; e++)
            if (out.sampler[d] == out.sampler[e]) return false;
        const Ins& z = ins[static_cast<size_t>(out.texld[d])];
        const int ut = static_cast<int>(Type(t[z.at + 2])), un = static_cast<int>(Num(t[z.at + 2]));
        if (d == 0) {
            out.uvType = ut;
            out.uvNum = un;
        } else if (out.uvType != ut || out.uvNum != un)
            return false;
    }
    return true;
}
} // namespace

bool BasisSamplers(const std::vector<DWORD>& t, int samplers[4]) {
    const auto ins = Parse(t);
    BasisReads r;
    if (ins.empty() || !FindBasisReads(t, ins, r)) return false;
    for (int d = 0; d < 4; d++) samplers[d] = r.sampler[d];
    return true;
}

// Walls in the directional maps (development builds, basis_wall_filter): the maps hold one texel per metre, each the light of
// its own tile, and the game's bilinear read blends the texel of the tile beyond a wall into a surface standing against it
// (a capture: the top step of a stair beside the wall of a lit room took that room's red lamp, with every Apex lamp
// share off). The block below moves the basis uv, before every basis read, so that it never blends towards a neighbour
// across a wall: in texels, d = the uv's offset from its own texel's centre (-0.5 .. 0.5 per axis); towards a side the mask
// marks walled, d becomes 0 (the texel's own value there). Mask (sampler maskSampler, the maps' layout, read at the texel's
// centre): r = a wall on its -x side, g = +x, b = -z, a = +z (0 or 1). Temps U (the moved uv), P, F, M.
// A point within kWallBand texels of a walled side of its own texel lies in the wall's thickness (a wall's faces stand 5 cm
// or more off its line): which room's surface it is cannot be told there (a capture: the top step of a stair
// ends on the wall's line, its last millimetres in the next room's texel, a red sliver; a millimetre the other way it is in
// its own). Such a point takes the lower of the two texels, channel by channel: after the block, P.xy = the uv of the texel
// beyond that side and P.z = 1 in the band (else 0), and every native basis read is followed by BasisWallMin.
// cB = (0.5 - kWallBand, -1, 1, 0).
// 6 cm: the top step of a stair ends 4 to 5 cm past the wall line, its lip at the slab (a capture:
// lot z 24.95 to 24.96, the wall at 25, the red room's texel), so a 3 cm band missed it.
constexpr float kWallBand = 0.06f;
std::vector<DWORD> BasisWallMin(DWORD S, DWORD D, DWORD P, DWORD R2) {
    constexpr DWORD kMinOp = 0x0A;
    return {Op(kTexld, 3), Dst(kTemp, R2), Src(kTemp, P), Src(kSampler, S),
            Op(kMinOp, 3), Dst(kTemp, R2), Src(kTemp, R2), Src(kTemp, D),
            Op(kAdd, 3), Dst(kTemp, R2), Src(kTemp, R2), Neg(Src(kTemp, D)),                  // min - D (lrp may not write a source)
            Op(kMad, 4), Dst(kTemp, D), Src(kTemp, P, Sw(2, 2, 2, 2)), Src(kTemp, R2), Src(kTemp, D)}; // D + band x (min - D)
}
// The floor light map read through the same uv's zw (the stair's top step and other upward faces: "texld r0, v1.zwzw, s1",
// a capture, PS 2CB7BD58; it is 4 texels per metre, the walls on its 1 m tile edges): the same rule at that
// map's resolution. Towards a walled side the point stays at least half a floor texel (kFloorHalf m) inside its own tile, so
// the bilinear read never blends the texel beyond the wall; in the wall's thickness it takes the lower of its own texel and
// the first one beyond. The floor uv = the basis uv x cG.xy (set per draw from the two maps' sizes); where the draw's
// own zw is not that (cG.z = 0, or a VS that makes it otherwise) the read is the game's. After the block: G = the floor
// read's uv, H.xy = the uv beyond, H.z = in the band (0 when off). cF = (0.5 - kFloorHalf, 0.5 + kFloorHalf, 1e-4, 0).
struct FloorWallRegs {
    DWORD G, H, cF, cG;
};
constexpr float kFloorHalf = 0.125f;
std::vector<DWORD> BasisWallUv(DWORD uvType, DWORD uvNum, DWORD cS, DWORD cK, DWORD cB, DWORD mask, DWORD U, DWORD P, DWORD Fm, DWORD M,
                               const FloorWallRegs* fl = nullptr) {
    constexpr DWORD kCmp = 0x58;
    constexpr DWORD kAbsMod = 0x0B000000u;
    const DWORD Z = Sw(2, 2, 2, 2);
    const DWORD One = Sw(3, 3, 3, 3); // cK.w = 1: the zw of the uvs read (a texld reads all four; the uv input declares xy only)
    std::vector<DWORD> floorAtWalls, floorAtSign, floorAtEnd;
    if (fl) {
        const DWORD G = fl->G, H = fl->H, cF = fl->cF, cG = fl->cG;
        const DWORD XY = Sw(0, 1, 0, 1), ZW = Sw(2, 3, 2, 3);
        floorAtWalls = { // P.xy = the texel's centre, Fm.xy = d, P.zw = walled on d's side
            Op(kMin, 3), Dst(kTemp, G, 0x3), Src(kTemp, Fm), Src(kConst, cF, Sw(0, 0, 0, 0)),
            Op(kMax, 3), Dst(kTemp, G, 0x3), Src(kTemp, G), Neg(Src(kConst, cF, Sw(0, 0, 0, 0))), // d clamped to the inner part
            Op(kAdd, 3), Dst(kTemp, G, 0x3), Src(kTemp, G), Neg(Src(kTemp, Fm)),
            Op(kMad, 4), Dst(kTemp, G, 0x3), Src(kTemp, G), Src(kTemp, P, ZW), Src(kTemp, Fm),   // walled side: clamped, else d
            Op(kAdd, 3), Dst(kTemp, G, 0x3), Src(kTemp, G), Src(kTemp, P)};                      // the floor point (texels)
        floorAtSign = { // M.xy = in the band per axis, M.zw = sign(d)
            Op(kMul, 3), Dst(kTemp, H, 0x3), Src(kTemp, M, ZW), Src(kTemp, M),
            Op(kMad, 4), Dst(kTemp, H, 0x3), Src(kTemp, H), Src(kConst, cF, Sw(1, 1, 1, 1)), Src(kTemp, P)}; // the first floor texel beyond
        floorAtEnd = { // M.x = in the band
            Op(kMul, 3), Dst(kTemp, G, 0x3), Src(kTemp, G), Src(kConst, cS, ZW),
            Op(kMul, 3), Dst(kTemp, H, 0x3), Src(kTemp, H), Src(kConst, cS, ZW),
            Op(kMul, 3), Dst(kTemp, G, 0x3), Src(kTemp, G), Src(kConst, cG),
            Op(kMul, 3), Dst(kTemp, H, 0x3), Src(kTemp, H), Src(kConst, cG),
            // on = cG.z where the draw's zw is the basis uv x cG.xy, else 0
            Op(kMad, 4), Dst(kTemp, G, 0xC), Neg(Src(uvType, uvNum, XY)), Src(kConst, cG, XY), Src(uvType, uvNum, ZW),
            Op(kMax, 3), Dst(kTemp, G, 0x4), Src(kTemp, G, Z) | kAbsMod, Src(kTemp, G, Sw(3, 3, 3, 3)) | kAbsMod,
            Op(kAdd, 3), Dst(kTemp, G, 0x4), Src(kTemp, G, Z), Neg(Src(kConst, cF, Z)),
            Op(kCmp, 4), Dst(kTemp, G, 0x4), Src(kTemp, G, Z), Src(kConst, cB, Sw(3, 3, 3, 3)), Src(kConst, cB, Z), // (one constant per instruction)
            Op(kMul, 3), Dst(kTemp, G, 0x4), Src(kTemp, G, Z), Src(kConst, cG, Z),
            Op(kAdd, 3), Dst(kTemp, G, 0x3), Src(kTemp, G), Neg(Src(uvType, uvNum, ZW)),
            Op(kMad, 4), Dst(kTemp, G, 0x3), Src(kTemp, G), Src(kTemp, G, Z), Src(uvType, uvNum, ZW), // off: the game's own uv
            Op(kMul, 3), Dst(kTemp, H, 0x4), Src(kTemp, M, Sw(0, 0, 0, 0)), Src(kTemp, G, Z),
            Op(kMov, 2), Dst(kTemp, H, 0x8), Src(kConst, cK, One)};
    }
    std::vector<DWORD> code = {
        Op(kMov, 2), Dst(kTemp, U, 0x3), Src(uvType, uvNum),
        Op(kMov, 2), Dst(kTemp, U, 0xC), Src(kConst, cK, One),
        Op(kMov, 2), Dst(kTemp, M, 0xC), Src(kConst, cK, One),
        Op(kMul, 3), Dst(kTemp, P, 0x3), Src(uvType, uvNum), Src(kConst, cS, Sw(0, 1, 0, 1)), // s = uv x size
        Op(kFrc, 2), Dst(kTemp, Fm, 0x3), Src(kTemp, P),                                       // fr
        Op(kAdd, 3), Dst(kTemp, P, 0x3), Src(kTemp, P), Neg(Src(kTemp, Fm)),                    // its texel
        Op(kAdd, 3), Dst(kTemp, P, 0x3), Src(kTemp, P), Src(kConst, cK, Z),                     // its centre (+0.5)
        Op(kMul, 3), Dst(kTemp, M, 0x3), Src(kTemp, P), Src(kConst, cS, Sw(2, 3, 2, 3)),        // the centre's uv
        Op(kTexld, 3), Dst(kTemp, M), Src(kTemp, M), Src(kSampler, mask),
        Op(kAdd, 3), Dst(kTemp, Fm, 0x3), Src(kTemp, Fm), Neg(Src(kConst, cK, Z)),              // d = fr - 0.5
        Op(kCmp, 4), Dst(kTemp, P, 0x4), Src(kTemp, Fm, Sw(0, 0, 0, 0)), Src(kTemp, M, Sw(1, 1, 1, 1)), Src(kTemp, M, Sw(0, 0, 0, 0)), // d.x >= 0 ? +x : -x
        Op(kCmp, 4), Dst(kTemp, P, 0x8), Src(kTemp, Fm, Sw(1, 1, 1, 1)), Src(kTemp, M, Sw(3, 3, 3, 3)), Src(kTemp, M, Sw(2, 2, 2, 2)), // d.y >= 0 ? +z : -z
    };
    code.insert(code.end(), floorAtWalls.begin(), floorAtWalls.end());
    const std::vector<DWORD> band = {
        // in the wall's thickness: |d| >= 0.5 - band on a walled side (M.xy = 1, else 0); M.zw = sign(d)
        Op(kAdd, 3), Dst(kTemp, M, 0x3), Src(kTemp, Fm) | kAbsMod, Neg(Src(kConst, cB, Sw(0, 0, 0, 0))),
        Op(kCmp, 4), Dst(kTemp, M, 0x3), Src(kTemp, M), Src(kConst, cB, Sw(2, 2, 2, 2)), Src(kConst, cB, Sw(3, 3, 3, 3)),
        Op(kMul, 3), Dst(kTemp, M, 0x3), Src(kTemp, M), Src(kTemp, P, Sw(2, 3, 2, 3)),
        Op(kCmp, 4), Dst(kTemp, M, 0xC), Src(kTemp, Fm, Sw(0, 1, 0, 1)), Src(kConst, cB, Sw(2, 2, 2, 2)), Src(kConst, cB, Sw(1, 1, 1, 1)),
    };
    code.insert(code.end(), band.begin(), band.end());
    code.insert(code.end(), floorAtSign.begin(), floorAtSign.end());
    const std::vector<DWORD> rest = {
        Op(kMad, 4), Dst(kTemp, Fm, 0x3), Neg(Src(kTemp, Fm)), Src(kTemp, P, Sw(2, 3, 2, 3)), Src(kTemp, Fm), // d (1 - wall)
        Op(kMad, 4), Dst(kTemp, M, 0xC), Src(kTemp, M), Src(kTemp, M, Sw(0, 1, 0, 1)), Src(kTemp, P, Sw(0, 1, 0, 1)), // beyond: centre + sign in the band
        Op(kMax, 3), Dst(kTemp, M, 0x1), Src(kTemp, M, Sw(0, 0, 0, 0)), Src(kTemp, M, Sw(1, 1, 1, 1)),   // in the band (either axis)
        Op(kAdd, 3), Dst(kTemp, P, 0x3), Src(kTemp, P), Src(kTemp, Fm),
        Op(kMul, 3), Dst(kTemp, U, 0x3), Src(kTemp, P), Src(kConst, cS, Sw(2, 3, 2, 3)),
        Op(kMul, 3), Dst(kTemp, P, 0x3), Src(kTemp, M, Sw(2, 3, 2, 3)), Src(kConst, cS, Sw(2, 3, 2, 3)), // P.xy = the uv beyond
        Op(kMov, 2), Dst(kTemp, P, 0x4), Src(kTemp, M, Sw(0, 0, 0, 0)),                                     // P.z = in the band
    };
    code.insert(code.end(), rest.begin(), rest.end());
    code.insert(code.end(), floorAtEnd.begin(), floorAtEnd.end());
    return code;
}
// After the floor read (dest D, write mask Wm): in the band, the lower of it and the texel beyond (H, FloorWallRegs)
std::vector<DWORD> FloorWallMin(DWORD S, DWORD D, DWORD Wm, DWORD H, DWORD R) {
    return {Op(kTexld, 3), Dst(kTemp, R), Src(kTemp, H), Src(kSampler, S),
            Op(kMin, 3), Dst(kTemp, R, Wm), Src(kTemp, R), Src(kTemp, D),
            Op(kAdd, 3), Dst(kTemp, R, Wm), Src(kTemp, R), Neg(Src(kTemp, D)),
            Op(kMad, 4), Dst(kTemp, D, Wm), Src(kTemp, H, Sw(2, 2, 2, 2)), Src(kTemp, R), Src(kTemp, D)};
}

int CubeWeightRead(const std::vector<DWORD>& t, const std::vector<Ins>& ins, int& weightConst); // below
bool PatchBasisSmooth(std::vector<DWORD>& t, BasisSmoothPatch& out, const BasisSmoothOptions& options) {
    const bool preserveShadows = options.preserveShadows, wallMask = options.wallMask, floorWall = options.floorWall;
    const bool stairRoomFill = options.stairRoomFill, floorTone = options.floorTone, storyBlend = options.storyBlend;
    if (t.empty() || t[0] != 0xFFFF0300) return false;
    const auto ins = Parse(t);
    if (ins.empty()) return false;
    BasisReads r;
    if (!FindBasisReads(t, ins, r) || (r.uvType != static_cast<int>(kInput) && r.uvType != static_cast<int>(kTexture))) return false;
    const Usage u = Scan(t, ins);
    if (u.maxTemp + (wallMask ? 17 : 12) >= 32 || u.maxConst + (wallMask ? 4 : 3) >= 224) return false;
    if (wallMask && (u.maxSampler + 1 >= 16 || !u.afterLastSamplerDcl)) return false;
    const DWORD T = static_cast<DWORD>(u.maxTemp + 1), cS = static_cast<DWORD>(u.maxConst + 1), cK = cS + 1, cH = cS + 2;
    out.sizeConst = cS;
    out.sizeSampler = static_cast<DWORD>(r.sampler[0]);
    out.maskSampler = wallMask ? u.maxSampler + 1 : -1;
    out.floorSampler = out.floorConst = -1;
    out.ambSampler = -1;
    int first = r.texld[0];
    for (int d = 1; d < 4; d++) first = std::min(first, r.texld[d]);
    // (floorWall) the floor light map read through the uv's zw after the basis reads (FloorWallRegs): where it fits
    int floorRead = -1;
    if (wallMask && floorWall && u.maxTemp + 19 < 32 && u.maxConst + 6 < 224)
        for (size_t i = static_cast<size_t>(first) + 1; i < ins.size(); i++) {
            const Ins& x = ins[i];
            if (x.op != kTexld || x.len != 3 || Type(t[x.at + 1]) != kTemp || Type(t[x.at + 3]) != kSampler) continue;
            const DWORD uv = t[x.at + 2];
            if (Type(uv) != static_cast<DWORD>(r.uvType) || Num(uv) != static_cast<DWORD>(r.uvNum) || Swz(uv) != kSwzZWZW || (uv & 0x0F000000u)) continue;
            const int s = static_cast<int>(Num(t[x.at + 3]));
            if (s == r.sampler[0] || s == r.sampler[1] || s == r.sampler[2] || s == r.sampler[3]) continue;
            floorRead = static_cast<int>(i);
            break;
        }
    // (stairRoomFill) the ambient cube's weighted read ("mul rC.xyz, rC, cW.w" right after the cube texld, after the first basis read):
    // its result becomes the room's ambient x the floor map's alpha (the fill floors and walls take); the floor read gets it too
    int cubeMul = -1;
    if (stairRoomFill && floorRead >= 0 && u.maxTemp + 21 < 32 && u.maxSampler + 2 < 16) {
        int w = -1;
        const int c = CubeWeightRead(t, ins, w);
        if (c > first && w >= 0) {
            const DWORD C = Num(t[ins[static_cast<size_t>(c)].at + 1]);
            for (size_t i = static_cast<size_t>(c) + 1; i < ins.size(); i++) {
                const Ins& x = ins[i];
                if (x.op == kMul && x.len == 3 && Type(t[x.at + 1]) == kTemp && IsReg(t[x.at + 2], kTemp, C) && Type(t[x.at + 3]) == kConst &&
                    Num(t[x.at + 3]) == static_cast<DWORD>(w) && Swz(t[x.at + 3]) == kSwzW) {
                    cubeMul = static_cast<int>(i);
                    break;
                }
                if (x.len >= 1 && x.op != kDcl && IsReg(t[x.at + 1], kTemp, C)) break;
            }
        }
    }
    DWORD uvType = static_cast<DWORD>(r.uvType), uvNum = static_cast<DWORD>(r.uvNum);
    std::vector<DWORD> wallUv;
    FloorWallRegs fl{T + 16, T + 17, cS + 4, cS + 5};
    if (wallMask) {
        const DWORD U = T + 11;
        const DWORD cB = cS + 3;
        wallUv = BasisWallUv(uvType, uvNum, cS, cK, cB, static_cast<DWORD>(out.maskSampler), U, T + 12, T + 13, T + 14, floorRead >= 0 ? &fl : nullptr);
        // the game's own reads (kept: the guard's reference) read the moved uv too (same length: before the inserts)
        for (int d = 0; d < 4; d++) t[ins[static_cast<size_t>(r.texld[d])].at + 2] = Src(kTemp, U);
        if (floorRead >= 0) {
            const Ins& x = ins[static_cast<size_t>(floorRead)];
            out.floorSampler = static_cast<int>(Num(t[x.at + 3]));
            out.floorConst = static_cast<int>(fl.cG);
            t[x.at + 2] = Src(kTemp, fl.G);
        }
        if (cubeMul >= 0) { // A = the tile's room ambient x the floor map's alpha at the floor read's uv
            const DWORD A = T + 18, FL = T + 19;
            out.ambSampler = u.maxSampler + 2;
            const std::vector<DWORD> fill = {Op(kTexld, 3), Dst(kTemp, A), Src(kTemp, U), Src(kSampler, static_cast<DWORD>(out.ambSampler)),
                                             Op(kTexld, 3), Dst(kTemp, FL), Src(kTemp, fl.G), Src(kSampler, static_cast<DWORD>(out.floorSampler)),
                                             Op(kMul, 3), Dst(kTemp, A, 0x7), Src(kTemp, A), Src(kTemp, FL, Sw(3, 3, 3, 3))};
            wallUv.insert(wallUv.end(), fill.begin(), fill.end());
        }
        uvType = kTemp, uvNum = U;
    }
    std::vector<Edit> edits;
    edits.push_back({1, {Op(kDef, 5), Dst(kConst, cK), F(1.0f / 6.0f), F(2.0f / 3.0f), F(0.5f), F(1.0f),
                         Op(kDef, 5), Dst(kConst, cH), F(-0.5f), F(1.5f), F(1.0f / 64.0f), F(0.0f)}});
    if (wallMask) edits.push_back({1, {Op(kDef, 5), Dst(kConst, cS + 3), F(0.5f - kWallBand), F(-1.0f), F(1.0f), F(0.0f)}}); // cB (BasisWallUv)
    if (floorRead >= 0) edits.push_back({1, {Op(kDef, 5), Dst(kConst, fl.cF), F(0.5f - kFloorHalf), F(0.5f + kFloorHalf), F(1e-4f), F(0.0f)}});
    edits.push_back({ins[static_cast<size_t>(first)].at, BicubicSetup(uvType, uvNum, cS, cK, cH, T)});
    if (wallMask) {
        // edits at one position land in the reverse order of this list: the uv block before the setup, and the mask's dcl
        // (its position can be that same one, the first basis read right after the last dcl) before both
        edits.push_back({ins[static_cast<size_t>(first)].at, wallUv});
        edits.push_back({u.afterLastSamplerDcl, {Op(kDcl, 2), 0x90000000u, Dst(kSampler, static_cast<DWORD>(out.maskSampler))}});
        if (out.ambSampler >= 0) edits.push_back({u.afterLastSamplerDcl, {Op(kDcl, 2), 0x90000000u, Dst(kSampler, static_cast<DWORD>(out.ambSampler))}});
    }
    for (int d = 0; d < 4; d++) {
        const Ins& x = ins[static_cast<size_t>(r.texld[d])];
        edits.push_back({End(x), BicubicTaps(static_cast<DWORD>(r.sampler[d]), Num(t[x.at + 1]), T, cH,preserveShadows)});
        if (wallMask) edits.push_back({End(x), BasisWallMin(static_cast<DWORD>(r.sampler[d]), Num(t[x.at + 1]), T + 12, T + 15)}); // (before the taps: their reference)
    }
    if (floorRead >= 0) {
        const Ins& x = ins[static_cast<size_t>(floorRead)];
        if (out.ambSampler >= 0) // (pushed first: lands after the min) the floor's law: map.rgb + map.a x ambient
            edits.push_back({End(x), {Op(kAdd, 3), Dst(kTemp, Num(t[x.at + 1]), WMask(t[x.at + 1]) & 0x7u), Src(kTemp, Num(t[x.at + 1])), Src(kTemp, T + 18)}});
        edits.push_back({End(x), FloorWallMin(Num(t[x.at + 3]), Num(t[x.at + 1]), WMask(t[x.at + 1]), fl.H, T + 15)});
    }
    if (out.ambSampler >= 0) { // the cube fill: the room's ambient x alpha instead
        const Ins& x = ins[static_cast<size_t>(cubeMul)];
        edits.push_back({End(x), {Op(kMov, 2), Dst(kTemp, Num(t[x.at + 1]), WMask(t[x.at + 1])), Src(kTemp, T + 18)}});
    }
    // (floorTone, with stairRoomFill) the floors' own tone curve on the colour (InteriorFloor PS, an F7 capture: "dp3 l, c, 0.333;
    // f = (2 / (1 + exp(-4.2 l)) - 1) / l; c x f", about x1.46 in the dark: D3D exp is 2^x); the object shaders a stair is drawn with have none,
    // so the same texture under the same light was half as bright on the steps as on the floor beside them. The last colour
    // write to oC0 goes to a temp, then oC0.rgb = it x f. cT = (0.333, 2, -1, 4.2), cT1 = (1, 1e-4, 0, 0).
    if (floorTone && out.ambSampler >= 0 && u.maxTemp + 23 < 32 && u.maxConst + 8 < 224) {
        int last = -1;
        for (size_t i = 0; i < ins.size(); i++)
            if (ins[i].len >= 1 && ins[i].op != kDcl && ins[i].op != kDef && IsReg(t[ins[i].at + 1], kColorOut, 0) && (WMask(t[ins[i].at + 1]) & 0x7))
                last = static_cast<int>(i);
        if (last >= 0) {
            constexpr DWORD kExp = 0x0E;
            const DWORD TC = T + 21, TT = T + 22, cT = cS + 6, cT1 = cS + 7;
            const Ins& x = ins[static_cast<size_t>(last)];
            const DWORD old = t[x.at + 1], full = WMask(old), mask = full & 0x7;
            t[x.at + 1] = Dst(kTemp, TC, full) | (old & 0x00F00000u);
            const DWORD X = Sw(0, 0, 0, 0), Y = Sw(1, 1, 1, 1), Z = Sw(2, 2, 2, 2), W = Sw(3, 3, 3, 3);
            edits.push_back({1, {Op(kDef, 5), Dst(kConst, cT), F(0.333f), F(2.0f), F(-1.0f), F(4.2f), Op(kDef, 5), Dst(kConst, cT1), F(1.0f), F(1e-4f), F(0.0f), F(0.0f)}});
            edits.push_back({End(x), {Op(kDp3, 3), Dst(kTemp, TT, 0x8), Src(kTemp, TC), Src(kConst, cT, X),
                                      Op(kMax, 3), Dst(kTemp, TT, 0x8), Src(kTemp, TT, W), Src(kConst, cT1, Y),
                                      Op(kMul, 3), Dst(kTemp, TT, 0x4), Neg(Src(kTemp, TT, W)), Src(kConst, cT, W),
                                      Op(kExp, 2), Dst(kTemp, TT, 0x4), Src(kTemp, TT, Z),
                                      Op(kAdd, 3), Dst(kTemp, TT, 0x4), Src(kTemp, TT, Z), Src(kConst, cT1, X),
                                      Op(kRcp, 2), Dst(kTemp, TT, 0x4), Src(kTemp, TT, Z),
                                      Op(kRcp, 2), Dst(kTemp, TT, 0x2), Src(kTemp, TT, W),
                                      Op(kMad, 4), Dst(kTemp, TT, 0x4), Src(kConst, cT, Y), Src(kTemp, TT, Z), Src(kConst, cT, Z),
                                      Op(kMul, 3), Dst(kTemp, TT, 0x4), Src(kTemp, TT, Y), Src(kTemp, TT, Z),
                                      Op(kMul, 3), Dst(kColorOut, 0, mask), Src(kTemp, TC), Src(kTemp, TT, Z)}});
            if (full & 0x8) edits.push_back({End(x), {Op(kMov, 2), Dst(kColorOut, 0, 0x8), Src(kTemp, TC, W)}}); // (pushed after: lands before the rgb, any order)
            out.floorTone = true;
        }
    }
    // (storyBlend, with stairRoomFill) A stair is lit by its own story's maps at every height, but it climbs to the next floor: its
    // top steps took the light of 0.5 m over the floor below (F7: the top steps in tile z 30, under the floor of
    // the lit story above, black: that texel's point is under the slab; F7: the other way, a lamp below lit the steps up
    // to the dark landing). The light L (the max that ends the lighting, after the floor read) is blended by the pixel's height
    // with the story above's: its 4 directional maps (samplers upSampler + d, d = +X -X +Z -Z, the same directions) weighted by
    // the pixel's normal as the game weights them (sat(+-0.8944 n.x|z + 0.4472 n.y)), maxed with its ambient fill (its ambient
    // map x the floor's alpha). t = smoothstep(sat((y - (yLow + 0.5)) / (yUp - yLow))): the two maps' own sample heights. y is
    // the world height the stair VS passes in its normal's w (VS 22EA00C8 "mov o5.w, r0.y"; checked per draw, storyConst.z = 0
    // otherwise). storyConst = (yLow + 0.5, 1 / (yUp - yLow), on, 0); cD = (0.8944, 0.4472, -0.8944, 0), cE = (3, -2, 0, 0).
    if (storyBlend && out.ambSampler >= 0 && floorRead >= 0 && !(u.maxConst + 11 < 224 && u.maxSampler + 7 < 16)) out.blendRefused = 1;
    if (storyBlend && out.ambSampler >= 0 && floorRead >= 0 && u.maxConst + 11 < 224 && u.maxSampler + 7 < 16) {
        int nrmAt = -1, normalIn = -1, lAt = -1;
        for (size_t i = 0; i < ins.size(); i++)
            if (ins[i].op == kNrm && ins[i].len == 2 && Type(t[ins[i].at + 2]) == kInput) {
                nrmAt = static_cast<int>(i);
                normalIn = static_cast<int>(Num(t[ins[i].at + 2]));
                break;
            }
        for (size_t i = static_cast<size_t>(floorRead) + 1; i < ins.size(); i++)
            if (ins[i].op == kMax && ins[i].len == 3 && Type(t[ins[i].at + 1]) == kTemp && (WMask(t[ins[i].at + 1]) & 0x7) == 0x7) {
                lAt = static_cast<int>(i);
                break;
            }
        int dclAt = -1;
        for (size_t i = 0; i < ins.size() && normalIn >= 0; i++)
            if (ins[i].op == kDcl && ins[i].len == 2 && IsReg(t[ins[i].at + 2], kInput, static_cast<DWORD>(normalIn))) dclAt = static_cast<int>(i);
        out.blendRefused = nrmAt < 0 ? 2 : lAt < 0 ? 3 : dclAt < 0 ? 4 : 0;
        if (nrmAt >= 0 && lAt >= 0 && dclAt >= 0) {
            // (temps: the bicubic taps' T .. T+3, free once the last directional read is done; U and FL are still live)
            const DWORD N = T, Wt = T + 1, B = T + 2, Lu = T + 3, U = T + 11, FL = T + 19;
            const DWORD cD = cS + 8, cE = cS + 9, cY = cS + 10, L = Num(t[ins[static_cast<size_t>(lAt)].at + 1]), V = static_cast<DWORD>(normalIn);
            const DWORD upBase = static_cast<DWORD>(u.maxSampler + 3), upAmb = upBase + 4;
            t[ins[static_cast<size_t>(dclAt)].at + 2] = (t[ins[static_cast<size_t>(dclAt)].at + 2] & ~0x000F0000u) | 0x000F0000u; // its w too
            const DWORD X = Sw(0, 0, 0, 0), Y = Sw(1, 1, 1, 1), Z = Sw(2, 2, 2, 2), W = Sw(3, 3, 3, 3);
            edits.push_back({1, {Op(kDef, 5), Dst(kConst, cD), F(0.8944f), F(0.4472f), F(-0.8944f), F(0.0f), Op(kDef, 5), Dst(kConst, cE), F(3.0f), F(-2.0f), F(0.0f), F(0.0f)}});
            std::vector<DWORD> blend = {
                Op(kNrm, 2), Dst(kTemp, N, 0x7), Src(kInput, V),
                Op(kDp2add, 4), Dst(kTemp, Wt, 0x1, true), Src(kTemp, N, Sw(0, 1, 0, 1)), Src(kConst, cD, Sw(0, 1, 0, 1)), Src(kConst, cD, W), // +X
                Op(kDp2add, 4), Dst(kTemp, Wt, 0x2, true), Src(kTemp, N, Sw(0, 1, 0, 1)), Src(kConst, cD, Sw(2, 1, 2, 1)), Src(kConst, cD, W), // -X
                Op(kDp2add, 4), Dst(kTemp, Wt, 0x4, true), Src(kTemp, N, Sw(2, 1, 2, 1)), Src(kConst, cD, Sw(0, 1, 0, 1)), Src(kConst, cD, W), // +Z
                Op(kDp2add, 4), Dst(kTemp, Wt, 0x8, true), Src(kTemp, N, Sw(2, 1, 2, 1)), Src(kConst, cD, Sw(2, 1, 2, 1)), Src(kConst, cD, W), // -Z
            };
            const DWORD wSw[4] = {X, Y, Z, W};
            for (DWORD d = 0; d < 4; d++) {
                blend.insert(blend.end(), {Op(kTexld, 3), Dst(kTemp, B), Src(kTemp, U), Src(kSampler, upBase + d)});
                if (d == 0) blend.insert(blend.end(), {Op(kMul, 3), Dst(kTemp, Lu, 0x7), Src(kTemp, B), Src(kTemp, Wt, wSw[d])});
                else blend.insert(blend.end(), {Op(kMad, 4), Dst(kTemp, Lu, 0x7), Src(kTemp, B), Src(kTemp, Wt, wSw[d]), Src(kTemp, Lu)});
            }
            const std::vector<DWORD> rest = {
                Op(kTexld, 3), Dst(kTemp, B), Src(kTemp, U), Src(kSampler, upAmb),                              // the story above's ambient
                Op(kMul, 3), Dst(kTemp, B, 0x7), Src(kTemp, B), Src(kTemp, FL, W),                               // x the floor's alpha
                Op(kMax, 3), Dst(kTemp, Lu, 0x7), Src(kTemp, Lu), Src(kTemp, B),
                Op(kAdd, 3), Dst(kTemp, Wt, 0x1), Src(kInput, V, W), Neg(Src(kConst, cY, X)),                    // y - (yLow + 0.5)
                Op(kMul, 3), Dst(kTemp, Wt, 0x1, true), Src(kTemp, Wt, X), Src(kConst, cY, Y),                   // s
                Op(kMad, 4), Dst(kTemp, Wt, 0x2), Src(kTemp, Wt, X), Src(kConst, cE, Y), Src(kConst, cE, X),      // 3 - 2 s
                Op(kMul, 3), Dst(kTemp, Wt, 0x2), Src(kTemp, Wt, Y), Src(kTemp, Wt, X),
                Op(kMul, 3), Dst(kTemp, Wt, 0x2), Src(kTemp, Wt, Y), Src(kTemp, Wt, X),                          // s^2 (3 - 2 s)
                Op(kMul, 3), Dst(kTemp, Wt, 0x2), Src(kTemp, Wt, Y), Src(kConst, cY, Z),                         // x on
                Op(kAdd, 3), Dst(kTemp, Lu, 0x7), Src(kTemp, Lu), Neg(Src(kTemp, L)),
                Op(kMad, 4), Dst(kTemp, L, 0x7), Src(kTemp, Lu), Src(kTemp, Wt, Y), Src(kTemp, L),               // L + t (Lup - L)
            };
            blend.insert(blend.end(), rest.begin(), rest.end());
            edits.push_back({End(ins[static_cast<size_t>(lAt)]), blend});
            for (DWORD s = upBase; s <= upAmb; s++) edits.push_back({u.afterLastSamplerDcl, {Op(kDcl, 2), 0x90000000u, Dst(kSampler, s)}});
            out.upSampler = static_cast<int>(upBase);
            out.upAmbSampler = static_cast<int>(upAmb);
            out.storyConst = static_cast<int>(cY);
            for (int d = 0; d < 4; d++) out.basisSampler[d] = r.sampler[d];
        }
    }
    Apply(t, std::move(edits));
    return true;
}

// The ambient cube read: the first cube texld whose rgb is then multiplied by a constant's .w (the weight; -1 = none).
// Returns the index of that texld in `ins`, -1 when there is none.
int CubeWeightRead(const std::vector<DWORD>& t, const std::vector<Ins>& ins, int& weightConst) {
    weightConst = -1;
    std::vector<DWORD> cubes;
    for (const Ins& x : ins)
        if (x.op == kDcl && x.len == 2 && (t[x.at + 1] & 0x78000000u) == 0x18000000u && Type(t[x.at + 2]) == kSampler) cubes.push_back(Num(t[x.at + 2]));
    for (size_t i = 0; i < ins.size(); i++) {
        const Ins& x = ins[i];
        if (x.op != kTexld || x.len < 3 || Type(t[x.at + 3]) != kSampler || std::find(cubes.begin(), cubes.end(), Num(t[x.at + 3])) == cubes.end()) continue;
        const DWORD C = Num(t[x.at + 1]);
        for (size_t j = i + 1; j < ins.size(); j++) {
            const Ins& y = ins[j];
            if (((y.op == kMad && y.len == 4) || (y.op == kMul && y.len == 3)) && IsReg(t[y.at + 2], kTemp, C) && Type(t[y.at + 3]) == kConst && Swz(t[y.at + 3]) == kSwzW) {
                weightConst = static_cast<int>(Num(t[y.at + 3]));
                return static_cast<int>(i);
            }
            if (y.len >= 1 && y.op != kDcl && IsReg(t[y.at + 1], kTemp, C) && (WMask(t[y.at + 1]) & 0x7)) break;
        }
    }
    return -1;
}

bool PatchCubeTint(std::vector<DWORD>& t, int& tintConst) {
    tintConst = -1;
    if (t.empty() || t[0] != 0xFFFF0300) return false;
    const auto ins = Parse(t);
    if (ins.empty()) return false;
    const Usage u = Scan(t, ins);
    if (u.maxTemp + 1 >= 32 || u.maxConst + 2 >= 224) return false;
    int weight = -1;
    const int cube = CubeWeightRead(t, ins, weight);
    if (cube < 0) return false;
    const Ins& x = ins[static_cast<size_t>(cube)];
    if (Type(t[x.at + 1]) != kTemp) return false;
    const DWORD Tmp = static_cast<DWORD>(u.maxTemp + 1), cT = static_cast<DWORD>(u.maxConst + 1), cL = cT + 1, C = Num(t[x.at + 1]);
    std::vector<Edit> edits;
    edits.push_back({1, {Op(kDef, 5), Dst(kConst, cL), F(0.2126f), F(0.7152f), F(0.0722f), F(0.0f)}});
    edits.push_back({End(x), {Op(kDp3, 3), Dst(kTemp, Tmp, 0x1), Src(kTemp, C), Src(kConst, cL),
                              Op(kLrp, 4), Dst(kTemp, C, 0x7), Src(kConst, cT, Sw(0, 0, 0, 0)), Src(kTemp, C), Src(kTemp, Tmp, Sw(0, 0, 0, 0)),
                              Op(kMul, 3), Dst(kTemp, C, 0x7), Src(kTemp, C), Src(kConst, cT, Sw(1, 2, 3, 3))}});
    Apply(t, std::move(edits));
    tintConst = static_cast<int>(cT);
    return true;
}

bool UvRowConsts(const std::vector<DWORD>& t, int usage, int index, int& cX, int& cY) {
    cX = cY = -1;
    if (t.empty() || t[0] != 0xFFFE0300 || usage < 0 || index < 0) return false;
    const auto ins = Parse(t);
    int out = -1;
    for (const Ins& x : ins)
        if (x.op == kDcl && x.len == 2 && Type(t[x.at + 2]) == kOutput && static_cast<int>(t[x.at + 1] & 0x1F) == usage &&
            static_cast<int>((t[x.at + 1] >> 16) & 0xF) == index)
            out = static_cast<int>(Num(t[x.at + 2]));
    if (out < 0) return false;
    // "dp4 oN.x, rP, cX" / "dp4 oN.y, rP, cY" (either operand order); any other write of .x / .y: not this shape
    for (const Ins& x : ins) {
        if (x.op == kDcl || x.len < 1 || !IsReg(t[x.at + 1], kOutput, static_cast<DWORD>(out))) continue;
        const DWORD m = WMask(t[x.at + 1]) & 0x3;
        if (!m) continue;
        int c = -1;
        if (x.op == kDp4 && x.len == 3 && (m == 0x1 || m == 0x2)) {
            if (Type(t[x.at + 2]) == kConst && Type(t[x.at + 3]) != kConst) c = static_cast<int>(Num(t[x.at + 2]));
            else if (Type(t[x.at + 3]) == kConst && Type(t[x.at + 2]) != kConst) c = static_cast<int>(Num(t[x.at + 3]));
        }
        if (c < 0) {
            cX = cY = -1;
            return false;
        }
        (m == 0x1 ? cX : cY) = c;
    }
    return cX >= 0 && cY >= 0;
}

bool AnalyzeRigPs(const std::vector<DWORD>& t, RigPsInfo& out) {
    out = RigPsInfo{};
    if (t.empty() || t[0] != 0xFFFF0300) return false;
    const auto ins = Parse(t);
    if (ins.empty()) return false;
    // four mul / mad in a row writing rgb, each reading one of c4..c7, together all four (the rig light chain)
    for (size_t i = 0; i + 3 < ins.size() && !out.rigLights; i++) {
        int seen = 0;
        for (size_t k = 0; k < 4; k++) {
            const Ins& y = ins[i + k];
            if (!((y.op == kMul && y.len == 3) || (y.op == kMad && y.len == 4))) break;
            const DWORD d = t[y.at + 1], cst = t[y.at + 3];
            if (Type(d) != kTemp || (WMask(d) & 0x7) != 0x7 || Type(cst) != kConst || Num(cst) < 4 || Num(cst) > 7) break;
            seen |= 1 << (Num(cst) - 4);
        }
        out.rigLights = seen == 0xF;
    }
    // Matte furniture shaders compute the next N.L between the steps ("mul r0.xyz, r0.z, c5 / dp3_sat r0.w, r2, c0 / mad
    // r0.xyz, r0.w, c4, r0 / ..."): followed through the accumulator instead. Strict (30/09 review): the colour an
    // unswizzled c4..c7, the multiplier a temp with one replicated component, the accumulator an unswizzled temp without
    // a modifier; census of the game's SM3 shaders: 60 of 628 match, all real rig chains
    const auto replicated = [&](DWORD s) {
        const DWORD w = Swz(s);
        return Type(s) == kTemp && (w == 0x00 || w == 0x55 || w == 0xAA || w == 0xFF) && !((s >> 24) & 0xF);
    };
    const auto rigColour = [&](DWORD s) { return Type(s) == kConst && Num(s) >= 4 && Num(s) <= 7 && Swz(s) == kSwzXYZW && !((s >> 24) & 0xF); };
    for (size_t i = 0; i < ins.size() && !out.rigLights; i++) {
        const Ins& x = ins[i];
        if (x.op != kMul || x.len != 3 || Type(t[x.at + 1]) != kTemp || (WMask(t[x.at + 1]) & 0x7) != 0x7 || !replicated(t[x.at + 2]) || !rigColour(t[x.at + 3])) continue;
        int seen = 1 << (Num(t[x.at + 3]) - 4);
        DWORD acc = Num(t[x.at + 1]);
        for (size_t j = i + 1; j < ins.size() && seen != 0xF; j++) {
            const Ins& y = ins[j];
            if (y.op == kDcl || y.op == kDef || y.len < 1) continue;
            const DWORD d = t[y.at + 1];
            if (y.op == kMad && y.len == 4 && Type(d) == kTemp && (WMask(d) & 0x7) == 0x7 && replicated(t[y.at + 2]) && rigColour(t[y.at + 3]) &&
                !(seen & (1 << (Num(t[y.at + 3]) - 4))) && IsReg(t[y.at + 4], kTemp, acc) && Swz(t[y.at + 4]) == kSwzXYZW && !((t[y.at + 4] >> 24) & 0xF)) {
                seen |= 1 << (Num(t[y.at + 3]) - 4);
                acc = Num(d);
                continue;
            }
            if (IsReg(d, kTemp, acc) && (WMask(d) & 0x7)) break; // the accumulator's rgb overwritten otherwise: not a chain
        }
        out.rigLights = seen == 0xF;
    }
    // the ambient cube's weight: the first cube read whose rgb is multiplied by a constant's .w
    CubeWeightRead(t, ins, out.cubeWeightConst);
    return out.rigLights || out.cubeWeightConst >= 0;
}

bool PatchIndoorBasis(std::vector<DWORD>& t, DWORD lmSampler, IndoorBasisPatch& out, bool capToFloorMap) {
    if (t.empty() || t[0] != 0xFFFF0300) return false;
    const auto ins = Parse(t);
    if (ins.empty()) return false;
    const Usage u = Scan(t, ins);
    if (u.maxTemp + 14 >= 32 || u.maxConst + 12 >= 224 || u.maxSampler < 0 || u.maxSampler + 4 > 15 || !u.afterLastSamplerDcl) return false;
    int nrm = -1, lm = -1;
    for (size_t i = 0; i < ins.size(); i++) {
        const Ins& x = ins[i];
        if (nrm < 0 && x.op == kNrm && Type(t[x.at + 2]) == kInput) nrm = static_cast<int>(i);
        if (lm < 0 && x.op == kTexld && IsReg(t[x.at + 3], kSampler, lmSampler) && (Type(t[x.at + 2]) == kInput || Type(t[x.at + 2]) == kTexture) && Swz(t[x.at + 2]) == kSwzXYZW) // its uv, as read (not .zw)
            lm = static_cast<int>(i);
    }
    // Normal-mapped furniture (30/09, the sofa of F7 093: "texld r0, v8, s6" ... "nrm_pp r4.xyz, r0" ... "texld_pp r0, r4,
    // s0"): the world normal is normalised from a temp. Taken when that nrm's result is what the ambient cube is read with
    // (the last write of the cube coordinate before the cube read), so a view or light vector is never taken for it.
    if (nrm < 0) {
        int weight = -1;
        const int cube = CubeWeightRead(t, ins, weight);
        const DWORD coord = cube >= 0 ? t[ins[static_cast<size_t>(cube)].at + 2] : 0;
        if (cube >= 0 && Type(coord) == kTemp)
            for (int k = cube - 1; k >= 0; k--) {
                const Ins& z = ins[static_cast<size_t>(k)];
                if (z.op == kDcl || z.op == kDef || z.len < 1 || !IsReg(t[z.at + 1], kTemp, Num(coord)) || !(WMask(t[z.at + 1]) & 0x7)) continue;
                if (z.op == kNrm && (WMask(t[z.at + 1]) & 0x7) == 0x7) nrm = k;
                break;
            }
    }
    if (nrm < 0 || lm < 0) return false;
    // the diffuse chain: 4 mul/mad into one dest reading c4..c7, multiplier written by a saturated mov (sat N.L)
    int chainEnd = -1;
    DWORD D = 0;
    for (size_t i = 0; i + 3 < ins.size() && chainEnd < 0; i++) {
        bool ok = true;
        DWORD dst = 0, src = 0;
        int seen = 0;
        for (size_t k = 0; k < 4 && ok; k++) {
            const Ins& y = ins[i + k];
            if (!((y.op == kMul && y.len == 3) || (y.op == kMad && y.len == 4))) {
                ok = false;
                break;
            }
            const DWORD d = t[y.at + 1], a = t[y.at + 2], c = t[y.at + 3];
            if (Type(d) != kTemp || (WMask(d) & 0x7) != 0x7 || Type(a) != kTemp || Type(c) != kConst || Num(c) < 4 || Num(c) > 7) {
                ok = false;
                break;
            }
            // the multiplier source is the same register; each mad adds the previous instruction's result (the game may
            // move the accumulator to another register mid-chain: "mad r6 .. ; mad r2.xyz, r2.z, c6, r6")
            if (k == 0) {
                src = Num(a);
            } else if (Num(a) != src || !IsReg(t[y.at + 4], kTemp, dst))
                ok = false;
            dst = Num(d);
            seen |= 1 << (Num(c) - 4);
        }
        if (!ok || seen != 0xF) continue;
        for (int k = static_cast<int>(i) - 1; k >= 0; k--) {
            const Ins& z = ins[static_cast<size_t>(k)];
            if (z.len >= 1 && IsReg(t[z.at + 1], kTemp, src)) {
                if (z.op == kMov && (t[z.at + 1] & 0x00100000u)) {
                    chainEnd = static_cast<int>(i + 3);
                    D = dst;
                }
                break;
            }
        }
    }
    // the basis light is computed right after the nrm (the normal register is reused later, e.g. as the cube lookup)
    // into registers above the shader's own; the chain's result is replaced by it
    if (chainEnd < 0 || chainEnd <= nrm) return false;
    const DWORD N = Num(t[ins[static_cast<size_t>(nrm)].at + 1]);
    const DWORD uvT = Type(t[ins[static_cast<size_t>(lm)].at + 2]), uvN = Num(t[ins[static_cast<size_t>(lm)].at + 2]);
    out.uvUsage = out.uvIndex = -1;
    if (uvT == kInput)
        for (const Ins& x : ins)
            if (x.op == kDcl && x.len == 2 && IsReg(t[x.at + 2], kInput, uvN)) {
                out.uvUsage = static_cast<int>(t[x.at + 1] & 0x1F);
                out.uvIndex = static_cast<int>((t[x.at + 1] >> 16) & 0xF);
            }
    const DWORD T = static_cast<DWORD>(u.maxTemp + 1), Wn = T + 11, Acc = T + 12, Tmp = T + 13;
    const DWORD cS = static_cast<DWORD>(u.maxConst + 1), cK = cS + 1, cD = cS + 2, cStr = cS + 3, cH = cS + 4;
    out.sizeConst = cS;
    const DWORD E = static_cast<DWORD>(u.maxSampler + 1);
    out.firstSampler = E;
    int cubeTex = -1; // the index of that cube read (Rooms at Night tints its colour: tintConst)
    // The ambient cube's weight (Rooms at Night scales it per draw): the first cube read whose result is multiplied by a
    // constant's .w ("texld_pp r1, r1, s0 ... mad_pp r6.xyz, r1, c12.w, r2" in the captured shaders)
    out.cubeWeightConst = -1;
    {
        std::vector<DWORD> cubes;
        for (const Ins& x : ins)
            if (x.op == kDcl && x.len == 2 && (t[x.at + 1] & 0x78000000u) == 0x18000000u && Type(t[x.at + 2]) == kSampler) cubes.push_back(Num(t[x.at + 2]));
        for (size_t i = 0; i < ins.size() && out.cubeWeightConst < 0; i++) {
            const Ins& x = ins[i];
            if (x.op != kTexld || x.len < 3 || Type(t[x.at + 3]) != kSampler || std::find(cubes.begin(), cubes.end(), Num(t[x.at + 3])) == cubes.end()) continue;
            const DWORD C = Num(t[x.at + 1]);
            for (size_t j = i + 1; j < ins.size(); j++) {
                const Ins& y = ins[j];
                if (((y.op == kMad && y.len == 4) || (y.op == kMul && y.len == 3)) && IsReg(t[y.at + 2], kTemp, C) && Type(t[y.at + 3]) == kConst &&
                    Swz(t[y.at + 3]) == kSwzW) {
                    out.cubeWeightConst = static_cast<int>(Num(t[y.at + 3]));
                    cubeTex = static_cast<int>(i);
                    break;
                }
                if (y.len >= 1 && y.op != kDcl && IsReg(t[y.at + 1], kTemp, C) && (WMask(t[y.at + 1]) & 0x7)) break; // its rgb overwritten ("mul_pp r1.w, ..." in the captured shaders is fine)
            }
        }
    }
    out.strengthConst = cStr;
    // The rig diffuse chain reads its own copy of c4..c7 (cU .. cU+3, set per draw by DrawIndoorObject: only the rig's
    // unlit-room lights, the fill and [NoLight] ones, with the lamps set to 0, since the basis light holds them per pixel).
    // The specular chain keeps reading c4..c7 (lamp highlights as before). 30/09, a second study: replacing the
    // whole diffuse by the basis light left unlit furniture with the grey cube only, so the Blue tint had nothing to act on.
    const DWORD cU = cS + 7;
    out.diffuseConst = static_cast<int>(cU);
    for (int k = chainEnd - 3; k <= chainEnd; k++) {
        DWORD& c = t[ins[static_cast<size_t>(k)].at + 3];
        c = (c & ~0x7FFu) | (cU + (Num(c) - 4));
    }
    std::vector<Edit> edits;
    edits.push_back({1, {Op(kDef, 5), Dst(kConst, cH), F(-0.5f), F(1.5f), F(1.0f / 64.0f), F(0.0f),
                         Op(kDef, 5), Dst(kConst, cK), F(1.0f / 6.0f), F(2.0f / 3.0f), F(0.5f), F(1.0f),
                         Op(kDef, 5), Dst(kConst, cD), F(0.8944f), F(0.4472f), F(-0.8944f), F(0.0f),
                         Op(kDef, 5), Dst(kConst, cS + 11), F(kBasisCap), F(0.0f), F(0.0f), F(0.0f)}});
    std::vector<DWORD> dcl;
    for (DWORD s = 0; s < 4; s++) dcl.insert(dcl.end(), {Op(kDcl, 2), 0x90000000u, Dst(kSampler, E + s)});
    edits.push_back({u.afterLastSamplerDcl, dcl});
    std::vector<DWORD> body = BicubicSetup(uvT, uvN, cS, cK, cH, T);
    // weights from the world normal: +X, -X, +Z, -Z, as the basis-reading shaders compute them
    body.insert(body.end(), {
        Op(kDp2add, 4), Dst(kTemp, Wn, 0x1, true), Src(kTemp, N, Sw(0, 1, 0, 1)), Src(kConst, cD, Sw(0, 1, 0, 1)), Src(kConst, cD, Sw(3, 3, 3, 3)),
        Op(kDp2add, 4), Dst(kTemp, Wn, 0x2, true), Src(kTemp, N, Sw(0, 1, 0, 1)), Src(kConst, cD, Sw(2, 1, 2, 1)), Src(kConst, cD, Sw(3, 3, 3, 3)),
        Op(kDp2add, 4), Dst(kTemp, Wn, 0x4, true), Src(kTemp, N, Sw(1, 2, 1, 2)), Src(kConst, cD, Sw(1, 0, 1, 0)), Src(kConst, cD, Sw(3, 3, 3, 3)),
        Op(kDp2add, 4), Dst(kTemp, Wn, 0x8, true), Src(kTemp, N, Sw(1, 2, 1, 2)), Src(kConst, cD, Sw(1, 2, 1, 2)), Src(kConst, cD, Sw(3, 3, 3, 3)),
    });
    for (DWORD d = 0; d < 4; d++) {
        const std::vector<DWORD> taps = BicubicTaps(E + d, Tmp, T, cH);
        body.insert(body.end(), taps.begin(), taps.end());
        if (d == 0) body.insert(body.end(), {Op(kMul, 3), Dst(kTemp, Acc, 0x7), Src(kTemp, Tmp), Src(kTemp, Wn, Sw(0, 0, 0, 0))});
        else {
            const int c = static_cast<int>(d);
            body.insert(body.end(), {Op(kMad, 4), Dst(kTemp, Acc, 0x7), Src(kTemp, Tmp), Src(kTemp, Wn, Sw(c, c, c, c)), Src(kTemp, Acc)});
        }
    }
    // The basis light never above kBasisCap x the room light map at the same place (30/09, F7 128 + F8: a TV in a closed
    // room upstairs took the green lamp of the story below). The room's light list holds lamps of another story near an opening
    // (the indoor light between floors); the room light map is solved point by point with the floor test (IndoorShadow: black
    // there), the 4 basis maps are filled by another routine of the game that the test never sees (green there). Where both are
    // right they agree (captures 096-103: light map 0.239, basis 0.157), so the cap leaves them; where the light map is dark
    // (behind a floor, a wall) the basis light goes too.
    const DWORD cG = cS + 11;
    // The 2D map includes furniture shadows at floor height. It must not cap elevated objects when
    // the directional-map solver already checks cross-story floors. Retain the old path as fallback.
    if (capToFloorMap) body.insert(body.end(), {Op(kTexld, 3), Dst(kTemp, Tmp), uvT == kInput ? Src(kInput, uvN) : Src(kTexture, uvN), Src(kSampler, lmSampler),
                             Op(kMul, 3), Dst(kTemp, Tmp, 0x7), Src(kTemp, Tmp), Src(kConst, cG, Sw(0, 0, 0, 0)),
                             Op(kMin, 3), Dst(kTemp, Acc, 0x7), Src(kTemp, Acc), Src(kTemp, Tmp)});
    edits.push_back({End(ins[static_cast<size_t>(nrm)]), body});
    // diffuse = the unlit-room lights (above) + the basis light x strength (lamps per pixel), as walls take lamp + base
    edits.push_back({End(ins[static_cast<size_t>(chainEnd)]), {Op(kMad, 4), Dst(kTemp, D, 0x7), Src(kTemp, Acc), Src(kConst, cStr, Sw(0, 0, 0, 0)), Src(kTemp, D)}});
    if (cubeTex >= 0) { // the cube's colour towards its grey by tintConst.x: lrp(t, cube, luma(cube)), then x tintConst.yzw (the room's colour)
        const DWORD cT = cS + 5, cL = cS + 6, C = Num(t[ins[static_cast<size_t>(cubeTex)].at + 1]);
        out.tintConst = static_cast<int>(cT);
        edits.push_back({1, {Op(kDef, 5), Dst(kConst, cL), F(0.2126f), F(0.7152f), F(0.0722f), F(0.0f)}});
        edits.push_back({End(ins[static_cast<size_t>(cubeTex)]), {Op(kDp3, 3), Dst(kTemp, Tmp, 0x1), Src(kTemp, C), Src(kConst, cL),
                                                                  Op(kLrp, 4), Dst(kTemp, C, 0x7), Src(kConst, cT, Sw(0, 0, 0, 0)), Src(kTemp, C), Src(kTemp, Tmp, Sw(0, 0, 0, 0)),
                                                                  Op(kMul, 3), Dst(kTemp, C, 0x7), Src(kTemp, C), Src(kConst, cT, Sw(1, 2, 3, 3))}});
    }
    Apply(t, std::move(edits));
    return true;
}

// ---- Smooth light maps (2.12.4): the room light map read of the room-lit pixel shaders (InteriorFloor, InteriorWall and
// their siblings FloorWith*, InteriorWallWith*, Hideable, FloorTileCeiling) as a cubic B-spline (shader_patches.h). ----
namespace {
struct RoomMapRead {
    int texld = -1;              // index in ins
    DWORD L = 0, S = 0;          // its destination temp and sampler
    DWORD uvType = 0, uvNum = 0; // TEXCOORD1: t1 (ps_2_0) or the v# declared texcoord1 (ps_3_0)
    int ambConst = -1;           // cA of "mad rD.xyz, rL.w, cA, rL" (the room's ambient colour)
};
bool Replicated(DWORD src) {
    const DWORD sw = Swz(src);
    return sw == 0x00 || sw == 0x55 || sw == 0xAA || sw == 0xFF;
}
bool FindRoomMapRead(const std::vector<DWORD>& t, const std::vector<Ins>& ins, RoomMapRead& out) {
    if (t.empty() || (t[0] != 0xFFFF0200 && t[0] != 0xFFFF0300)) return false;
    const bool ps3 = t[0] == 0xFFFF0300;
    int tc1 = ps3 ? -1 : 1;
    DWORD samplerType[16] = {};
    for (const Ins& x : ins) {
        if (x.op != kDcl || x.len != 2) continue;
        const DWORD d = t[x.at + 1], r = t[x.at + 2];
        if (ps3 && Type(r) == kInput && (d & 0x1F) == 5 && ((d >> 16) & 0xF) == 1) tc1 = static_cast<int>(Num(r));
        if (Type(r) == kSampler && Num(r) < 16) samplerType[Num(r)] = (d >> 27) & 0xF;
    }
    if (tc1 < 0) return false;
    const DWORD uvType = ps3 ? kInput : kTexture;
    int reads[16] = {};
    for (const Ins& x : ins)
        if (x.op == kTexld && x.len == 3 && Type(t[x.at + 3]) == kSampler && Num(t[x.at + 3]) < 16) reads[Num(t[x.at + 3])]++;
    int found = 0;
    for (size_t i = 0; i < ins.size(); i++) {
        const Ins& x = ins[i];
        if (x.op != kTexld || x.len != 3 || ((t[x.at] >> 16) & 0xFF)) continue; // a plain texld (not texldp / texldb)
        const DWORD d = t[x.at + 1], uv = t[x.at + 2], sm = t[x.at + 3];
        if (Type(d) != kTemp || WMask(d) != 0xF || (d & 0x00D00000u) || !IsReg(uv, uvType, static_cast<DWORD>(tc1)) || Swz(uv) != kSwzXYZW || (uv & 0x0F000000u) ||
            Type(sm) != kSampler || Num(sm) >= 16 || samplerType[Num(sm)] != 2 || reads[Num(sm)] != 1)
            continue;
        const DWORD L = Num(d);
        bool ok = false, mulSeen = false;
        for (size_t j = i + 1; j < ins.size(); j++) {
            const Ins& y = ins[j];
            if (y.op == kDcl || y.op == kDef || y.op == kDefI || y.op == kDefB) continue;
            if (IsFlow(y.op)) break;
            bool touches = false;
            for (size_t k = 1; k <= y.len; k++) touches |= IsReg(t[y.at + k], kTemp, L);
            if (!touches) continue;
            // at most one "mul rL, rS.c, rL" (a normal-map or basis factor)
            if (!mulSeen && y.op == kMul && y.len == 3 && IsReg(t[y.at + 1], kTemp, L) && Type(t[y.at + 2]) == kTemp && Num(t[y.at + 2]) != L && Replicated(t[y.at + 2]) &&
                IsReg(t[y.at + 3], kTemp, L)) {
                mulSeen = true;
                continue;
            }
            // "mad rD.xyz, rL.w, cA, rL": rgb + alpha x the ambient colour (a constant the game sets)
            if (y.op == kMad && y.len == 4 && Type(t[y.at + 1]) == kTemp && WMask(t[y.at + 1]) == 0x7 && IsReg(t[y.at + 2], kTemp, L) && Swz(t[y.at + 2]) == kSwzW &&
                !(t[y.at + 2] & 0x0F000000u) && Type(t[y.at + 3]) == kConst && IsReg(t[y.at + 4], kTemp, L)) {
                const DWORD cA = Num(t[y.at + 3]);
                bool defined = false;
                for (const Ins& z : ins) defined |= z.op == kDef && IsReg(t[z.at + 1], kConst, cA);
                ok = !defined;
                if (ok) out.ambConst = static_cast<int>(cA);
            }
            break;
        }
        if (!ok) continue;
        found++;
        out.texld = static_cast<int>(i);
        out.L = L;
        out.S = Num(sm);
        out.uvType = uvType;
        out.uvNum = static_cast<DWORD>(tc1);
    }
    return found == 1;
}
} // namespace

bool VsPassesWorldY(const std::vector<DWORD>& vs) {
    if (vs.empty() || vs[0] != 0xFFFE0300) return false; // vs_3_0
    const auto ins = Parse(vs);
    int tc2 = -1;
    for (const Ins& x : ins) // dcl_texcoord2 oN: usage 5 (TEXCOORD), index 2
        if (x.op == kDcl && x.len == 2 && (vs[x.at + 1] & 0x1F) == 5 && ((vs[x.at + 1] >> 16) & 0xF) == 2 && Type(vs[x.at + 2]) == kOutput) tc2 = static_cast<int>(Num(vs[x.at + 2]));
    if (tc2 < 0) return false;
    int yFrom = -1;
    for (const Ins& x : ins) // mov oN.w, rX.y
        if (x.op == kMov && x.len == 2 && IsReg(vs[x.at + 1], kOutput, static_cast<DWORD>(tc2)) && WMask(vs[x.at + 1]) == 0x8 && Type(vs[x.at + 2]) == kTemp &&
            Swz(vs[x.at + 2]) == kSwzY)
            yFrom = static_cast<int>(Num(vs[x.at + 2]));
    if (yFrom < 0) return false;
    for (const Ins& x : ins) // the same temp's x and z are the world position the room maps are read at (mov oM.zw, rX.xyxz)
        if (x.op == kMov && x.len == 2 && Type(vs[x.at + 1]) == kOutput && WMask(vs[x.at + 1]) == 0xC && IsReg(vs[x.at + 2], kTemp, static_cast<DWORD>(yFrom)) &&
            Swz(vs[x.at + 2]) == kSwzXYXZ)
            return true;
    return false;
}

bool IsRoomMapPs(const std::vector<DWORD>& t) {
    const auto ins = Parse(t);
    RoomMapRead r;
    return !ins.empty() && FindRoomMapRead(t, ins, r);
}

// Room ambient through arches (development builds, arch_ambient; arch_ambient.h has the same law on the CPU, for the
// tests). After the room map read rL (alpha = the ambient weight the shader multiplies the room's ambient cA by): the
// pixel's lot position from the map uv (floor maps: 4 texels a metre, p = uv x size / 4), and for each of kArchCells arch
// cells the draw passes, seg = (a.x, a.z, e.x, e.z) (e = b - a) and D = (the neighbour's ambient over cA, 0 where lower;
// 1 / |e|^2): w = 1 - smoothstep(dist(p, segment) / kReach), add = max over the cells of D w, rL.xyz += rL.w add. Unused
// cells hold D = 0. Temps: the three given (free after the read). cP = (3, -2, 1, 0.25), cQ = (1e-8, 1 / kReach, 0, 0).
std::vector<DWORD> ArchAmbientCode(DWORD uvType, DWORD uvNum, DWORD cS, DWORD cP, DWORD cQ, DWORD cArch, DWORD L, DWORD Pp, DWORD Q, DWORD Acc) {
    constexpr DWORD kRsq = 0x07;
    const DWORD X = Sw(0, 0, 0, 0), Y = Sw(1, 1, 1, 1), Z = Sw(2, 2, 2, 2), W = Sw(3, 3, 3, 3);
    std::vector<DWORD> v = {
        Op(kMul, 3), Dst(kTemp, Pp, 0x3), Src(uvType, uvNum), Src(kConst, cS, Sw(0, 1, 0, 1)), // texels
        Op(kMul, 3), Dst(kTemp, Pp, 0x3), Src(kTemp, Pp), Src(kConst, cP, W),                  // metres (lot)
        Op(kMov, 2), Dst(kTemp, Acc, 0x7), Src(kConst, cQ, Z),                                  // 0
    };
    for (DWORD i = 0; i < kArchCells; i++) {
        const DWORD cSeg = cArch + 2 * i, cD = cSeg + 1;
        const std::vector<DWORD> cell = {
            Op(kAdd, 3), Dst(kTemp, Q, 0x3), Src(kTemp, Pp), Neg(Src(kConst, cSeg, Sw(0, 1, 0, 1))),   // p - a
            Op(kMul, 3), Dst(kTemp, Q, 0x4), Src(kTemp, Q, X), Src(kConst, cSeg, Z),
            Op(kMad, 4), Dst(kTemp, Q, 0x4), Src(kTemp, Q, Y), Src(kConst, cSeg, W), Src(kTemp, Q, Z),     // (p - a).e
            Op(kMul, 3), Dst(kTemp, Q, 0x4, true), Src(kTemp, Q, Z), Src(kConst, cD, W),                   // t = sat(/ |e|^2)
            Op(kMad, 4), Dst(kTemp, Q, 0x3), Src(kConst, cSeg, Sw(2, 3, 2, 3)), Src(kTemp, Q, Z), Neg(Src(kTemp, Q)), // a + t e - p
            Op(kMul, 3), Dst(kTemp, Q, 0x4), Src(kTemp, Q, X), Src(kTemp, Q, X),
            Op(kMad, 4), Dst(kTemp, Q, 0x4), Src(kTemp, Q, Y), Src(kTemp, Q, Y), Src(kTemp, Q, Z),         // dist^2
            Op(kMax, 3), Dst(kTemp, Q, 0x4), Src(kTemp, Q, Z), Src(kConst, cQ, X),
            Op(kRsq, 2), Dst(kTemp, Q, 0x8), Src(kTemp, Q, Z),
            Op(kMul, 3), Dst(kTemp, Q, 0x4), Src(kTemp, Q, Z), Src(kTemp, Q, W),                           // dist
            Op(kMul, 3), Dst(kTemp, Q, 0x4, true), Src(kTemp, Q, Z), Src(kConst, cQ, Y),                   // s = sat(dist / reach)
            Op(kMad, 4), Dst(kTemp, Q, 0x8), Src(kTemp, Q, Z), Src(kConst, cP, Y), Src(kConst, cP, X),      // 3 - 2 s
            Op(kMul, 3), Dst(kTemp, Q, 0x8), Src(kTemp, Q, W), Src(kTemp, Q, Z),
            Op(kMul, 3), Dst(kTemp, Q, 0x8), Src(kTemp, Q, W), Src(kTemp, Q, Z),                           // s^2 (3 - 2 s)
            Op(kAdd, 3), Dst(kTemp, Q, 0x8), Neg(Src(kTemp, Q, W)), Src(kConst, cP, Z),                    // w
            Op(kMul, 3), Dst(kTemp, Q, 0x7), Src(kConst, cD), Src(kTemp, Q, W),
            Op(kMax, 3), Dst(kTemp, Acc, 0x7), Src(kTemp, Acc), Src(kTemp, Q),
        };
        v.insert(v.end(), cell.begin(), cell.end());
    }
    v.insert(v.end(), {Op(kMad, 4), Dst(kTemp, L, 0x7), Src(kTemp, L, W), Src(kTemp, Acc), Src(kTemp, L)});
    return v;
}

bool PatchRoomMapSmooth(std::vector<DWORD>& t, RoomMapPatch& out, bool archAmb) {
    out = RoomMapPatch{};
    auto ins = Parse(t);
    RoomMapRead r;
    if (ins.empty() || !FindRoomMapRead(t, ins, r)) return false;
    const bool ps3 = t[0] == 0xFFFF0300;
    const Usage u = Scan(t, ins);
    const int constLimit = ps3 ? 224 : 32; // ps_2_x: c0..c31
    if (u.maxTemp + 11 >= 32 || u.maxConst + 4 >= constLimit) return false;
    const DWORD T = static_cast<DWORD>(u.maxTemp + 1);
    const DWORD cS = static_cast<DWORD>(u.maxConst + 1), cK = cS + 1, cH = cS + 2, cR = cS + 3;
    const DWORD P = T, Fr = T + 1, R = T + 2, A = T + 3, B = T + 4, Wt = T + 10;
    const DWORD X = Sw(0, 0, 0, 0), Y = Sw(1, 1, 1, 1), Z = Sw(2, 2, 2, 2), W = Sw(3, 3, 3, 3);
    // The wall atlas's blocks: row = floor(v H / S) (S = 3n + 1 texel rows a block), the taps' v kept within
    // [row S + 0.5, row S + S - 0.5] so no tap reaches the block above or below; floors take S = 65536 (one block).
    const std::vector<DWORD> clampV = {
        Op(kAdd, 3), Dst(kTemp, A, 0x1), Src(kTemp, P, Y), Src(kTemp, Fr, Y),          // p.y = i + f
        Op(kAdd, 3), Dst(kTemp, A, 0x1), Src(kTemp, A, X), Src(kConst, cR, Z),         // v H = p.y + 0.5
        Op(kMul, 3), Dst(kTemp, A, 0x1), Src(kTemp, A, X), Src(kConst, cR, X),         // / S
        Op(kFrc, 2), Dst(kTemp, A, 0x2), Src(kTemp, A, X),
        Op(kAdd, 3), Dst(kTemp, A, 0x1), Src(kTemp, A, X), Neg(Src(kTemp, A, Y)),      // the block
        Op(kMad, 4), Dst(kTemp, A, 0x1), Src(kTemp, A, X), Src(kConst, cR, Y), Src(kConst, cR, Z), // lo = block S + 0.5
        Op(kAdd, 3), Dst(kTemp, A, 0x2), Src(kTemp, A, X), Src(kConst, cR, W),         // hi = lo + S - 1
        Op(kMax, 3), Dst(kTemp, B, 0xA), Src(kTemp, B), Src(kTemp, A, X),
        Op(kMin, 3), Dst(kTemp, B, 0xA), Src(kTemp, B), Src(kTemp, A, Y),
    };
    std::vector<DWORD> body = BicubicSetup(r.uvType, r.uvNum, cS, cK, cH, T, clampV);
    // plain weighted taps, all four channels (the alpha is the ambient weight the shader reads next, not a mask)
    for (DWORD k = 0; k < 4; k++) {
        body.insert(body.end(), {Op(kTexld, 3), Dst(kTemp, R), Src(kTemp, T + 6 + k), Src(kSampler, r.S)});
        const int c = static_cast<int>(k);
        if (k == 0) body.insert(body.end(), {Op(kMul, 3), Dst(kTemp, r.L), Src(kTemp, R), Src(kTemp, Wt, X)});
        else body.insert(body.end(), {Op(kMad, 4), Dst(kTemp, r.L), Src(kTemp, R), Src(kTemp, Wt, Sw(c, c, c, c)), Src(kTemp, r.L)});
    }
    // (development builds, arch_ambient) the neighbours' ambient near the room's arches, right after the read (ps_3_0 only;
    // a copy that cannot hold it is made without it)
    std::vector<DWORD> defs = {Op(kDef, 5), Dst(kConst, cK), F(1.0f / 6.0f), F(2.0f / 3.0f), F(0.5f), F(1.0f),
                               Op(kDef, 5), Dst(kConst, cH), F(-0.5f), F(1.5f), F(0.0f), F(0.0f)};
    if (archAmb && ps3 && r.ambConst >= 0 && u.maxConst + 6 + 2 * static_cast<int>(kArchCells) < constLimit) {
        const DWORD cP = cS + 4, cQ = cS + 5, cArch = cS + 6;
        const std::vector<DWORD> code = ArchAmbientCode(r.uvType, r.uvNum, cS, cP, cQ, cArch, r.L, T, T + 1, T + 2);
        body.insert(body.end(), code.begin(), code.end());
        defs.insert(defs.end(), {Op(kDef, 5), Dst(kConst, cP), F(3.0f), F(-2.0f), F(1.0f), F(0.25f),
                                 Op(kDef, 5), Dst(kConst, cQ), F(1e-8f), F(1.0f / kArchReach), F(0.0f), F(0.0f)});
        out.archConst = static_cast<int>(cArch);
        out.ambConst = r.ambConst;
    }
    const Ins x = ins[static_cast<size_t>(r.texld)];
    t.erase(t.begin() + static_cast<ptrdiff_t>(x.at), t.begin() + static_cast<ptrdiff_t>(End(x))); // the game's read goes
    Apply(t, {{1, defs}, {x.at, body}});
    if (!ps3) t[0] = 0xFFFF0201; // ps_2_x: more temps and instruction slots than ps_2_0, arbitrary swizzles
    out.sampler = r.S;
    out.sizeConst = cS;
    out.bandConst = cR;
    out.temps = static_cast<int>(T + 11);
    ins = Parse(t);
    for (const Ins& y : ins) out.slots += y.op != kDcl && y.op != kDef && y.op != kDefI && y.op != kDefB ? 1 : 0;
    out.ps2x = !ps3;
    return !ins.empty();
}

bool RoomMapUvScale(const std::vector<DWORD>& vs, RoomMapUv& out) {
    out = RoomMapUv{};
    if (vs.empty() || (vs[0] != 0xFFFE0300 && vs[0] != 0xFFFE0200 && vs[0] != 0xFFFE0201)) return false;
    const auto ins = Parse(vs);
    if (ins.empty()) return false;
    int o = vs[0] == 0xFFFE0300 ? -1 : 1; // vs_2_x: oT1
    if (vs[0] == 0xFFFE0300)
        for (const Ins& x : ins)
            if (x.op == kDcl && x.len == 2 && Type(vs[x.at + 2]) == kOutput && (vs[x.at + 1] & 0x1F) == 5 && ((vs[x.at + 1] >> 16) & 0xF) == 1) o = static_cast<int>(Num(vs[x.at + 2]));
    if (o < 0) return false;
    int writes = 0, k = -1, comp = -1;
    for (const Ins& x : ins) {
        if (x.op == kDcl || x.len < 1 || !IsReg(vs[x.at + 1], kOutput, static_cast<DWORD>(o)) || !(WMask(vs[x.at + 1]) & 0x3)) continue;
        writes++;
        if (x.op != kMul || x.len != 3 || (WMask(vs[x.at + 1]) & 0x3) != 0x3 || (vs[x.at + 1] & 0x00F00000u)) return false;
        for (int a = 2; a <= 3; a++) {
            const DWORD c = vs[x.at + static_cast<size_t>(a)], other = vs[x.at + static_cast<size_t>(5 - a)];
            if (Type(c) == kConst && !(c & 0x0F002000u) && Replicated(c) && Type(other) != kConst) {
                k = static_cast<int>(Num(c));
                comp = static_cast<int>(Swz(c) & 3);
            }
        }
    }
    if (writes != 1 || k < 0) return false;
    out.constReg = k;
    out.comp = comp;
    for (const Ins& x : ins)
        if (x.op == kDef && IsReg(vs[x.at + 1], kConst, static_cast<DWORD>(k))) {
            std::memcpy(&out.value, &vs[x.at + 2 + static_cast<size_t>(comp)], 4);
            out.defined = true;
        }
    return true;
}

bool IsPackedRoomFloorVs(const std::vector<DWORD>& vs) {
    RoomMapUv uv;
    if (vs.empty() || vs[0]!=0xFFFE0300 || !RoomMapUvScale(vs,uv) || !uv.defined || uv.comp!=2 ||
        std::fabs(uv.value*32767.0f-1.0f)>1e-5f) return false;
    const auto ins=Parse(vs);
    bool packedConstants=false;
    for(const auto& x:ins) if(x.op==kDef && IsReg(vs[x.at+1],kConst,static_cast<DWORD>(uv.constReg))) {
        float values[4];std::memcpy(values,&vs[x.at+2],sizeof values);
        packedConstants=values[0]==1.0f/256.0f && values[1]==-128.0f && values[3]==0.5f;
    }
    if(!packedConstants) return false;
    // Height combines the packed position's z and w before subtracting the packing bias.
    int height=-1;bool unpacked=false;
    for(const auto& x:ins) {
        if(x.op==kMad && Type(vs[x.at+1])==kTemp && WMask(vs[x.at+1])==8 &&
           IsReg(vs[x.at+2],kInput,0) && Swz(vs[x.at+2])==Sw(2,2,2,2) &&
           IsReg(vs[x.at+3],kConst,static_cast<DWORD>(uv.constReg)) && Swz(vs[x.at+3])==kSwzX &&
           IsReg(vs[x.at+4],kInput,0) && Swz(vs[x.at+4])==kSwzW) height=static_cast<int>(Num(vs[x.at+1]));
        if(height>=0 && x.op==kAdd && Type(vs[x.at+1])==kTemp && WMask(vs[x.at+1])==2 &&
           IsReg(vs[x.at+2],kTemp,static_cast<DWORD>(height)) && Swz(vs[x.at+2])==kSwzW &&
           IsReg(vs[x.at+3],kConst,static_cast<DWORD>(uv.constReg)) && Swz(vs[x.at+3])==kSwzY) unpacked=true;
    }
    return unpacked;
}

bool FindWallMatrices(const std::vector<DWORD>& vs, WallMatrices& out) {
    out={};RoomMapUv uv;
    if(!RoomMapUvScale(vs,uv) || !uv.defined || std::fabs(uv.value*4096.0f-1.0f)>1e-5f) return false;
    const auto ins=Parse(vs);
    int matches=0;
    for(const auto& seed:ins) {
        if(seed.op!=kDp4 || Type(vs[seed.at+2])!=kTemp || Type(vs[seed.at+3])!=kConst) continue;
        const DWORD position=vs[seed.at+2],first=Num(vs[seed.at+3]);
        if(first>249) continue;
        unsigned found=0;int worldTemp=-1;bool valid=true;
        for(const auto& x:ins) {
            if(x.op!=kDp4 || vs[x.at+2]!=position || Type(vs[x.at+3])!=kConst) continue;
            const DWORD k=Num(vs[x.at+3]);if(k<first || k>first+6) continue;
            const auto bit=1u<<(k-first);if(found&bit){valid=false;break;}found|=bit;
            if(k>=first+4) {
                if(Type(vs[x.at+1])!=kTemp || WMask(vs[x.at+1])!=(1u<<(k-first-4))) {valid=false;break;}
                const int reg=static_cast<int>(Num(vs[x.at+1]));
                if(worldTemp>=0 && worldTemp!=reg){valid=false;break;}worldTemp=reg;
            }
        }
        if(valid && found==127){out.clip=static_cast<int>(first);out.world=out.clip+4;++matches;}
    }
    if(matches!=1){out={};return false;}return true;
}

// ---- Windows take outdoor light (experimental, see the header) ----
bool PatchWindowOutdoor(std::vector<DWORD>& t, WindowOutdoorPatch& out, bool reach) {
    out = WindowOutdoorPatch{};
    if (t.empty() || t[0] != 0xFFFF0300) return false;
    const auto ins = Parse(t);
    if (ins.empty()) return false;
    for (const Ins& x : ins)
        if (IsFlow(x.op)) return false; // straight-line shaders only (every captured window part is)
    BasisReads r;
    if (!FindBasisReads(t, ins, r) || r.uvType != static_cast<int>(kInput)) return false;
    // inputs: TEXCOORD8 free, a register left for it, the basis uv's semantic
    int maxIn = -1;
    size_t afterLastInDcl = 0;
    for (const Ins& x : ins) {
        if (x.op != kDcl) continue;
        const DWORD use = t[x.at + 1] & 0x1F, idx = (t[x.at + 1] >> 16) & 0xF, reg = t[x.at + 2];
        if (Type(reg) != kInput) continue;
        if (use == 5 && idx == 8) return false;
        maxIn = std::max(maxIn, static_cast<int>(Num(reg)));
        afterLastInDcl = End(x);
        if (static_cast<int>(Num(reg)) == r.uvNum) {
            out.uvUsage = static_cast<int>(use);
            out.uvIndex = static_cast<int>(idx);
        }
    }
    if (maxIn < 0 || maxIn >= 9 || !afterLastInDcl || out.uvUsage < 0) return false;
    // the world normal: the one input the shader normalizes
    int nIn = -1, nNrm = 0;
    for (const Ins& x : ins)
        if (x.op == kNrm && x.len == 2 && Type(t[x.at + 2]) == kInput && !(t[x.at + 2] & 0x0F000000u) && Swz(t[x.at + 2]) == kSwzXYZW) {
            if (nIn != static_cast<int>(Num(t[x.at + 2]))) nNrm++;
            nIn = static_cast<int>(Num(t[x.at + 2]));
        }
    if (nNrm != 1) return false;
    // the anchor: the last "max rL.xyz, .., .." before the first colour output write, after the 4 basis reads, whose result
    // is next read (rgb) by a plain mul / mad (the light times the material)
    size_t firstOut = ins.size();
    for (size_t i = 0; i < ins.size() && firstOut == ins.size(); i++)
        if (ins[i].op != kDcl && ins[i].op != kDef && ins[i].len >= 1 && Type(t[ins[i].at + 1]) == kColorOut) firstOut = i;
    if (firstOut == ins.size()) return false;
    int anchor = -1;
    for (size_t i = 0; i < firstOut; i++) {
        const Ins& x = ins[i];
        if (x.op == kMax && x.len == 3 && Type(t[x.at + 1]) == kTemp && (WMask(t[x.at + 1]) & 0x7) == 0x7 && !(t[x.at + 1] & 0x00100000u /* no saturate (_pp is fine) */)) anchor = static_cast<int>(i);
    }
    if (anchor < 0) return false;
    for (int d = 0; d < 4; d++)
        if (r.texld[d] > anchor) return false;
    const DWORD L = Num(t[ins[static_cast<size_t>(anchor)].at + 1]);
    bool usedAsLight = false;
    for (size_t i = static_cast<size_t>(anchor) + 1; i < ins.size(); i++) {
        const Ins& x = ins[i];
        if (x.op == kDcl || x.op == kDef || x.len < 2) continue;
        bool reads = false;
        for (size_t k = 2; k <= x.len; k++)
            if (IsReg(t[x.at + k], kTemp, L)) reads = true;
        if (reads) {
            usedAsLight = ((x.op == kMul && x.len == 3) || (x.op == kMad && x.len == 4)) && (IsReg(t[x.at + 2], kTemp, L) || IsReg(t[x.at + 3], kTemp, L)) &&
                          Type(t[x.at + 1]) == kTemp && !(t[x.at + 1] & 0x00100000u);
            break;
        }
        if (IsReg(t[x.at + 1], kTemp, L) && (WMask(t[x.at + 1]) & 0x7)) break;
    }
    if (!usedAsLight) return false;
    const Usage u = Scan(t, ins);
    // reach: 2 more rows (the r0^2 of the 8 lamps) after the plan rows
    if (u.maxSampler < 0 || u.maxSampler >= 15 || !u.afterLastSamplerDcl || u.maxConst + 6 + 2 * static_cast<int>(kObjectPixelLamps) + (reach ? 2 : 0) >= 224 || u.maxTemp + 7 >= 32) return false;
    const DWORD E = static_cast<DWORD>(u.maxSampler + 1), V = static_cast<DWORD>(maxIn + 1), Nin = static_cast<DWORD>(nIn), U = static_cast<DWORD>(r.uvNum);
    const DWORD T = static_cast<DWORD>(u.maxTemp + 1), Fr = T + 1, A = T + 2, B = T + 3, Q = T + 4, N = T + 5, P = T + 6;
    const DWORD cA = static_cast<DWORD>(u.maxConst + 1), cB = cA + 1, cH = cA + 2, cS = cA + 3, cL = cA + 4;
    const DWORD cP = cL + 2 * kObjectPixelLamps; // cP, cP + 1
    const DWORD cR = cP + 2;                     // reach: r0^2 of lamps 0..3 / 4..7
    out.atlasSampler = E;
    out.atlasConst = cA;
    out.strengthConst = cB;
    out.lampParamConst = cS;
    out.lampConst = cL;
    out.planConst = cP;
    out.reachConst = reach ? cR : 0;
    constexpr DWORD kNeg = 0x01000000u;
    std::vector<DWORD> v = {
        Op(kNrm, 2), Dst(kTemp, N, 0x7), Src(kInput, Nin),
        // w = sat(1 - plan alpha at the basis uv moved by the normal (d in the rows))
        Op(kDp2add, 4), Dst(kTemp, P, 0x1), Src(kTemp, N, Sw(0, 2, 2, 2)), Src(kConst, cP), Src(kConst, cP, Sw(2, 2, 2, 2)),
        Op(kDp2add, 4), Dst(kTemp, P, 0x2), Src(kTemp, N, Sw(0, 2, 2, 2)), Src(kConst, cP + 1), Src(kConst, cP + 1, Sw(2, 2, 2, 2)),
        Op(kAdd, 3), Dst(kTemp, P, 0x3), Src(kTemp, P), Src(kInput, U),
        Op(kTexld, 3), Dst(kTemp, P), Src(kTemp, P), Src(kSampler, static_cast<DWORD>(r.sampler[0])),
        Op(kAdd, 3), Dst(kTemp, P, 0x8, true), Neg(Src(kTemp, P, kSwzW)), Src(kConst, cH, 0xAA /* 1 */),
        // ground = atlas(world.xz) x (0.5 + 0.5 N.y) x cB.x (as the outdoor-object patch)
        Op(kMad, 4), Dst(kTemp, T, 0x3), Src(kInput, V, kSwzXYXY), Src(kConst, cA), Src(kConst, cA, kSwzZWZW),
        Op(kTexld, 3), Dst(kTemp, T), Src(kTemp, T), Src(kSampler, E),
        Op(kMad, 4), Dst(kTemp, Fr, 0x8), Src(kTemp, N, kSwzY), Src(kConst, cH, kSwzX), Src(kConst, cH, kSwzY),
        Op(kMul, 3), Dst(kTemp, Fr, 0x8), Src(kTemp, Fr, kSwzW), Src(kConst, cB, kSwzX),
        Op(kMul, 3), Dst(kTemp, T, 0x7), Src(kTemp, T), Src(kTemp, Fr, kSwzW),
    };
    // per-pixel lamps Q (same law as PatchObjectLampPs: colour x sat(N.l) x sat(1 - d^2/R^2)^2, one constant per instruction;
    // the reach variant: the outdoor walls' law, AppendLampLaw, held under 1 like their texels)
    const DWORD pw = Src(kInput, V, 0xD8 /* xzyw: world x, y, z */);
    for (DWORD k = 0; k < kObjectPixelLamps; k++) {
        const DWORD cp = cL + 2 * k, cc = cp + 1;
        AppendLampLaw(v, A, B, N, pw | kNeg, cp, cS, cH, reach, cR, k);
        if (k == 0) v.insert(v.end(), {Op(kMul, 3), Dst(kTemp, Q, 0x7), Src(kConst, cc), Src(kTemp, B, kSwzX)});
        else v.insert(v.end(), {Op(kMad, 4), Dst(kTemp, Q, 0x7), Src(kConst, cc), Src(kTemp, B, kSwzX), Src(kTemp, Q)});
    }
    if (reach) AppendHueClamp(v, Q, B); // (held under 1 by its brightest channel)
    // outdoor = ground + lamps x cS.y, x w; the game's light is never lowered
    v.insert(v.end(), {Op(kMad, 4), Dst(kTemp, T, 0x7), Src(kTemp, Q), Src(kConst, cS, kSwzY), Src(kTemp, T),
                       Op(kMul, 3), Dst(kTemp, T, 0x7), Src(kTemp, T), Src(kTemp, P, kSwzW),
                       Op(kMax, 3), Dst(kTemp, L, 0x7), Src(kTemp, L), Src(kTemp, T)});
    std::vector<Edit> edits = {{1, {Op(kDef, 5), Dst(kConst, cH), F(0.5f), F(0.5f), F(1.0f), F(0.0f)}},
                               {afterLastInDcl, {Op(kDcl, 2), 0x80080005u /* texcoord8 */, Dst(kInput, V, 0x7)}},
                               {u.afterLastSamplerDcl, {Op(kDcl, 2), 0x90000000u, Dst(kSampler, E)}},
                               {End(ins[static_cast<size_t>(anchor)]), v}};
    Apply(t, std::move(edits));
    return true;
}

bool WorldUvRows(const std::vector<DWORD>& t, int usage, int index, int worldK, UvRows& out) {
    out = UvRows{};
    if (t.empty() || t[0] != 0xFFFE0300 || usage < 0 || index < 0 || worldK < 0) return false;
    const auto ins = Parse(t);
    if (ins.empty()) return false;
    int o = -1, W = -1;
    for (const Ins& x : ins) {
        if (x.op == kDcl && x.len == 2 && Type(t[x.at + 2]) == kOutput && static_cast<int>(t[x.at + 1] & 0x1F) == usage && static_cast<int>((t[x.at + 1] >> 16) & 0xF) == index)
            o = static_cast<int>(Num(t[x.at + 2]));
        // the world-position temp: "dp4 rW.x, rP, c[worldK]"
        if (x.op == kDp4 && x.len == 3 && Type(t[x.at + 1]) == kTemp && WMask(t[x.at + 1]) == 0x1 && IsReg(t[x.at + 3], kConst, static_cast<DWORD>(worldK))) {
            if (W >= 0 && W != static_cast<int>(Num(t[x.at + 1]))) return false;
            W = static_cast<int>(Num(t[x.at + 1]));
        }
    }
    if (o < 0 || W < 0) return false;
    // "dp4 dst.c, rW, cK" (either order): K, else -1
    auto rowOf = [&](const Ins& x) -> int {
        if (x.op != kDp4 || x.len != 3) return -1;
        const DWORD a = t[x.at + 2], b = t[x.at + 3];
        if (IsReg(a, kTemp, static_cast<DWORD>(W)) && Type(b) == kConst && !(b & 0x0F000000u)) return static_cast<int>(Num(b));
        if (IsReg(b, kTemp, static_cast<DWORD>(W)) && Type(a) == kConst && !(a & 0x0F000000u)) return static_cast<int>(Num(a));
        return -1;
    };
    for (size_t i = 0; i < ins.size(); i++) {
        const Ins& x = ins[i];
        if (x.op == kDcl || x.len < 1 || !IsReg(t[x.at + 1], kOutput, static_cast<DWORD>(o))) continue;
        const DWORD m = WMask(t[x.at + 1]) & 0x3;
        if (!m) continue;
        if (t[x.at + 1] & 0x00100000u) return false; // a saturated uv
        if (x.op == kDp4 && (m == 0x1 || m == 0x2)) {
            const int k = rowOf(x);
            if (k < 0) return false;
            out.row[m == 0x1 ? 0 : 1] = k;
            out.scale[m == 0x1 ? 0 : 1] = -1;
            continue;
        }
        if (x.op != kMul || x.len != 3) return false;
        DWORD a = t[x.at + 2], c = t[x.at + 3];
        if (Type(a) == kConst) std::swap(a, c);
        if (Type(a) != kTemp || Type(c) != kConst || (a & 0x0F000000u) || (c & 0x0F000000u)) return false;
        for (int comp = 0; comp < 2; comp++) {
            if (!(m & (1u << comp))) continue;
            const DWORD ac = (Swz(a) >> (2 * comp)) & 3, cc = (Swz(c) >> (2 * comp)) & 3;
            int row = -1;
            for (int k = static_cast<int>(i) - 1; k >= 0; k--) { // the last writer of rA.ac
                const Ins& z = ins[static_cast<size_t>(k)];
                if (z.op == kDcl || z.op == kDef || z.len < 1 || !IsReg(t[z.at + 1], kTemp, Num(a)) || !(WMask(t[z.at + 1]) & (1u << ac))) continue;
                if (WMask(t[z.at + 1]) == (1u << ac)) row = rowOf(z);
                break;
            }
            if (row < 0) return false;
            out.row[comp] = row;
            out.scale[comp] = static_cast<int>(Num(c));
            out.scaleComp[comp] = static_cast<int>(cc);
        }
    }
    return out.row[0] >= 0 && out.row[1] >= 0;
}

DitherResult AddDither(std::vector<DWORD>& t, int* amountConst) {
    if (t.empty() || t[0] != 0xFFFF0300) return DitherResult::NotPs30;
    if (t.back() != 0x0000FFFFu) return DitherResult::Unreadable;
    const auto ins = Parse(t);
    if (ins.empty()) return DitherResult::Unreadable;
    constexpr DWORD kMisc = 17, kDp2add = 0x5A, kFrc = 0x13, kCall = 0x19, kCallnz = 0x1A, kRet = 0x1C, kLabel = 0x1E;
    constexpr DWORD kRegBits = 0x70001800u | 0x7FFu, kRelative = 0x2000u;
    bool vPos = false;
    DWORD mask = 0;                // components of oC0 the shader writes
    std::vector<size_t> writes;    // token indices of the oC0 destinations
    for (const Ins& x : ins) {
        if (x.op == kCall || x.op == kCallnz || x.op == kRet || x.op == kLabel) return DitherResult::Subroutines;
        if (x.op == kDcl) {
            const DWORD r = t[x.at + 2];
            if (Type(r) == kMisc && Num(r) == 0) vPos = true;
            continue;
        }
        if (x.op == kDef || x.op == kDefI || x.op == kDefB) continue;
        for (size_t k = 1; k <= x.len; k++) {
            const DWORD r = t[x.at + k];
            if (!(r & 0x80000000u)) continue;
            if (Type(r) == kConst && (r & kRelative)) return DitherResult::RelativeConstants;
            if (Type(r) == kColorOut && Num(r) == 0) { // an output: only ever a destination
                writes.push_back(x.at + k);
                mask |= WMask(r);
            }
        }
    }
    if (writes.empty() || !(mask & 0x7)) return DitherResult::NoColorWrite;
    const Usage u = Scan(t, ins);
    if (u.maxTemp + 2 >= 32 || u.maxConst + 3 >= 224) return DitherResult::NoFreeRegister;
    const DWORD O = static_cast<DWORD>(u.maxTemp + 1), N = O + 1, cK = static_cast<DWORD>(u.maxConst + 1), cL = cK + 1, cA = cK + 2;
    for (size_t at : writes) t[at] = (t[at] & ~kRegBits) | (Reg(kTemp, O) & kRegBits); // oC0 -> rO, same mask and modifiers
    constexpr DWORD kAbs = 0x23, kRsq = 0x07, kRcp = 0x06, kCmp = 0x58;
    std::vector<Edit> edits;
    std::vector<DWORD> head = {Op(kDef, 5), Dst(kConst, cK), F(0.06711056f), F(0.00583715f), F(0.0f), F(52.9829189f),
                               Op(kDef, 5), Dst(kConst, cL), F(2.0f), F(-1.0f), F(1.0f), F(0.0f)};
    if (!vPos) head.insert(head.end(), {Op(kDcl, 2), 0x80000000u, Dst(kMisc, 0, 0x3)});
    edits.push_back({1, head});
    // u = IGN(vPos) in [0, 1); r = 2u - 1; triangular t = sign(r) (1 - sqrt(1 - |r|)) in (-1, 1): the inverse CDF of the
    // triangular distribution, so the dither is TPDF (no noise modulation: the steps vanish instead of thinning out)
    std::vector<DWORD> tail = {Op(kDp2add, 4), Dst(kTemp, N, 0x1), Src(kMisc, 0), Src(kConst, cK), Src(kConst, cK, 0xAA),
                               Op(kAdd, 3), Dst(kTemp, N, 0x1), Src(kTemp, N, kSwzX), Src(kConst, cA, kSwzW), // + cA.w: 0 still, per frame moving
                               Op(kFrc, 2), Dst(kTemp, N, 0x1), Src(kTemp, N, kSwzX),
                               Op(kMul, 3), Dst(kTemp, N, 0x1), Src(kTemp, N, kSwzX), Src(kConst, cK, kSwzW),
                               Op(kFrc, 2), Dst(kTemp, N, 0x1), Src(kTemp, N, kSwzX),                                                // u
                               Op(kMad, 4), Dst(kTemp, N, 0x1), Src(kTemp, N, kSwzX), Src(kConst, cL, kSwzX), Src(kConst, cL, kSwzY), // r
                               Op(kAbs, 2), Dst(kTemp, N, 0x2), Src(kTemp, N, kSwzX),
                               Op(kAdd, 3), Dst(kTemp, N, 0x2), Neg(Src(kTemp, N, kSwzY)), Src(kConst, cL, 0xAA),                  // 1 - |r|
                               Op(kRsq, 2), Dst(kTemp, N, 0x4), Src(kTemp, N, kSwzY),
                               Op(kRcp, 2), Dst(kTemp, N, 0x4), Src(kTemp, N, 0xAA),                                                 // sqrt
                               Op(kAdd, 3), Dst(kTemp, N, 0x4), Neg(Src(kTemp, N, 0xAA)), Src(kConst, cL, 0xAA),                  // 1 - sqrt
                               Op(kCmp, 4), Dst(kTemp, N, 0x1), Src(kTemp, N, kSwzX), Src(kTemp, N, 0xAA), Neg(Src(kTemp, N, 0xAA)), // t
                               Op(kMad, 4), Dst(kColorOut, 0, mask & 0x7), Src(kTemp, N, kSwzX), Src(kConst, cA, kSwzX), Src(kTemp, O)};
    if (mask & 0x8) tail.insert(tail.end(), {Op(kMov, 2), Dst(kColorOut, 0, 0x8), Src(kTemp, O)});
    edits.push_back({t.size() - 1, tail}); // before the end token
    Apply(t, std::move(edits));
    if (amountConst) *amountConst = static_cast<int>(cA);
    return DitherResult::Ok;
}

DitherResult AddDither2(std::vector<DWORD>& t, int* amountConst, int* texcoordOut) {
    if (t.empty() || (t[0] != 0xFFFF0200 && t[0] != 0xFFFF0201)) return DitherResult::NotPs30;
    if (t.back() != 0x0000FFFFu) return DitherResult::Unreadable;
    const auto ins = Parse(t);
    if (ins.empty()) return DitherResult::Unreadable;
    constexpr DWORD kDp2add = 0x5A, kFrc = 0x13, kCall = 0x19, kCallnz = 0x1A, kRet = 0x1C, kLabel = 0x1E;
    constexpr DWORD kAbs = 0x23, kRsq = 0x07, kRcp = 0x06, kCmp = 0x58;
    constexpr DWORD kRegBits = 0x70001800u | 0x7FFu;
    std::vector<size_t> writes;
    unsigned usedT = 0; // texture coordinates the shader declares or reads
    for (const Ins& x : ins) {
        if (x.op == kCall || x.op == kCallnz || x.op == kRet || x.op == kLabel) return DitherResult::Subroutines;
        if (x.op == kDef || x.op == kDefI || x.op == kDefB) continue;
        for (size_t k = 1; k <= x.len; k++) {
            const DWORD r = t[x.at + k];
            if (!(r & 0x80000000u)) continue;
            if (Type(r) == kTexture && Num(r) < 8) usedT |= 1u << Num(r);
            if (x.op != kDcl && Type(r) == kColorOut && Num(r) == 0) writes.push_back(x.at + k);
        }
    }
    if (writes.empty()) return DitherResult::NoColorWrite;
    int free = -1; // the highest texture coordinate the shader does not use
    for (int k = 7; k >= 0 && free < 0; k--)
        if (!(usedT & (1u << k))) free = k;
    if (free < 0) return DitherResult::NoFreeRegister;
    const DWORD kT = static_cast<DWORD>(free);
    const Usage u = Scan(t, ins);
    // the copy is ps_2_x (more temps and instruction slots than ps_2_0: the grain does not fit in some ps_2_0 shaders)
    if (u.maxTemp + 2 >= 32 || u.maxConst + 3 >= 32) return DitherResult::NoFreeRegister;
    const DWORD O = static_cast<DWORD>(u.maxTemp + 1), N = O + 1, cK = static_cast<DWORD>(u.maxConst + 1), cL = cK + 1, cA = cK + 2;
    for (size_t at : writes) t[at] = (t[at] & ~kRegBits) | (Reg(kTemp, O) & kRegBits); // oC0 -> rO, same mask and modifiers
    std::vector<Edit> edits;
    edits.push_back({1, {Op(kDef, 5), Dst(kConst, cK), F(0.06711056f), F(0.00583715f), F(0.0f), F(52.9829189f),
                         Op(kDef, 5), Dst(kConst, cL), F(2.0f), F(-1.0f), F(1.0f), F(0.0f),
                         Op(kDcl, 2), 0x80000000u, Dst(kTexture, kT)}});
    // pixel = (ndc.x * w/2 + w/2, -ndc.y * h/2 + h/2), ndc = t7.xy / t7.w; then the same triangular grain as AddDither.
    // One constant per instruction (ps_2_0).
    std::vector<DWORD> tail = {Op(kRcp, 2), Dst(kTemp, N, 0x8), Src(kTexture, kT, kSwzW),
                               Op(kMul, 3), Dst(kTemp, N, 0x3), Src(kTemp, N, kSwzW), Src(kTexture, kT),
                               Op(kMad, 4), Dst(kTemp, N, 0x1), Src(kTemp, N, kSwzX), Src(kConst, cA, kSwzY), Src(kConst, cA, kSwzY),
                               Op(kMad, 4), Dst(kTemp, N, 0x2), Neg(Src(kTemp, N, kSwzY)), Src(kConst, cA, 0xAA), Src(kConst, cA, 0xAA),
                               Op(kDp2add, 4), Dst(kTemp, N, 0x1), Src(kTemp, N), Src(kConst, cK), Src(kConst, cK, 0xAA),
                               Op(kAdd, 3), Dst(kTemp, N, 0x1), Src(kTemp, N, kSwzX), Src(kConst, cA, kSwzW),
                               Op(kFrc, 2), Dst(kTemp, N, 0x1), Src(kTemp, N, kSwzX),
                               Op(kMul, 3), Dst(kTemp, N, 0x1), Src(kTemp, N, kSwzX), Src(kConst, cK, kSwzW),
                               Op(kFrc, 2), Dst(kTemp, N, 0x1), Src(kTemp, N, kSwzX),
                               Op(kMad, 4), Dst(kTemp, N, 0x1), Src(kTemp, N, kSwzX), Src(kConst, cL, kSwzX), Src(kConst, cL, kSwzY),
                               Op(kAbs, 2), Dst(kTemp, N, 0x2), Src(kTemp, N, kSwzX),
                               Op(kAdd, 3), Dst(kTemp, N, 0x2), Neg(Src(kTemp, N, kSwzY)), Src(kConst, cL, 0xAA),
                               Op(kRsq, 2), Dst(kTemp, N, 0x4), Src(kTemp, N, kSwzY),
                               Op(kRcp, 2), Dst(kTemp, N, 0x4), Src(kTemp, N, 0xAA),
                               Op(kAdd, 3), Dst(kTemp, N, 0x4), Neg(Src(kTemp, N, 0xAA)), Src(kConst, cL, 0xAA),
                               Op(kCmp, 4), Dst(kTemp, N, 0x1), Src(kTemp, N, kSwzX), Src(kTemp, N, 0xAA), Neg(Src(kTemp, N, 0xAA)),
                               Op(kMad, 4), Dst(kTemp, O, 0x7), Src(kTemp, N, kSwzX), Src(kConst, cA, kSwzX), Src(kTemp, O),
                               Op(kMov, 2), Dst(kColorOut, 0), Src(kTemp, O)};
    edits.push_back({t.size() - 1, tail});
    Apply(t, std::move(edits));
    t[0] = 0xFFFF0201; // ps_2_x
    if (amountConst) *amountConst = static_cast<int>(cA);
    if (texcoordOut) *texcoordOut = free;
    return DitherResult::Ok;
}

bool MakeAoReceiverMask(std::vector<DWORD>& vs, std::vector<DWORD>& ps, bool hair, bool transparent) {
    if (vs.empty() || ps.empty() || vs.back() != 0xFFFFu || ps.back() != 0xFFFFu) return false;
    const bool v3 = vs[0] == 0xFFFE0300u, p3 = ps[0] == 0xFFFF0300u;
    if ((!v3 && vs[0] != 0xFFFE0200u && vs[0] != 0xFFFE0201u) ||
        (!p3 && ps[0] != 0xFFFF0200u && ps[0] != 0xFFFF0201u)) return false;
    const auto vi = Parse(vs), pi = Parse(ps);
    if (vi.empty() || pi.empty()) return false;
    constexpr DWORD regBits = 0x70001800u | 0x7FFu;
    DWORD posType = 4, posNum = 0;
    bool havePosition = !v3, coord[8] = {}, output[12] = {}, input[10] = {};
    std::vector<size_t> positionWrites, colourWrites;
    DWORD colourMask = 0;
    for (const auto& x : vi) {
        if (x.op == 0x19 || x.op == 0x1A || x.op == 0x1C || x.op == 0x1E) return false;
        if (x.op == kDcl && x.len == 2 && Type(vs[x.at + 2]) == kOutput && v3) {
            const DWORD sem = vs[x.at + 1], r = Num(vs[x.at + 2]);
            if (r >= 12) return false;
            output[r] = true;
            if ((sem & 31) == 0 && ((sem >> 16) & 15) == 0) { posType = kOutput; posNum = r; havePosition = true; }
            if ((sem & 31) == 5 && ((sem >> 16) & 15) < 8) coord[(sem >> 16) & 15] = true;
        }
        if (!v3 && x.op != kDcl && x.op != kDef && x.op != kDefI && x.op != kDefB)
            for (size_t j = 1; j <= x.len; ++j)
                if (Type(vs[x.at + j]) == kOutput && Num(vs[x.at + j]) < 8) coord[Num(vs[x.at + j])] = true;
    }
    if (!havePosition) return false;
    for (const auto& x : vi)
        if (x.op != kDcl && x.op != kDef && x.op != kDefI && x.op != kDefB && x.len &&
            IsReg(vs[x.at + 1], posType, posNum)) positionWrites.push_back(x.at + 1);
    for (const auto& x : pi) {
        if (x.op == 0x19 || x.op == 0x1A || x.op == 0x1C || x.op == 0x1E) return false;
        if (x.op == kDcl && x.len == 2) {
            const DWORD r = ps[x.at + 2], sem = ps[x.at + 1];
            if (p3 && Type(r) == kInput) {
                if (Num(r) >= 10) return false;
                input[Num(r)] = true;
                if ((sem & 31) == 5 && ((sem >> 16) & 15) < 8) coord[(sem >> 16) & 15] = true;
            }
        }
        if (x.op == kDef || x.op == kDefI || x.op == kDefB || x.op == kDcl) continue;
        for (size_t j = 1; j <= x.len; ++j) {
            const DWORD r = ps[x.at + j];
            if (!p3 && Type(r) == kTexture && Num(r) < 8) coord[Num(r)] = true;
            if (Type(r) == 9 || (Type(r) == kColorOut && Num(r) != 0)) return false;
        }
        if (x.len && Type(ps[x.at + 1]) == kColorOut) {
            colourWrites.push_back(x.at + 1);
            colourMask |= WMask(ps[x.at + 1]);
        }
    }
    if (positionWrites.empty() || colourWrites.empty() || colourMask != 15) return false;
    const auto vu = Scan(vs, vi), pu = Scan(ps, pi);
    if (vu.maxTemp + 1 >= (v3 ? 32 : 12) || pu.maxTemp + 2 >= 32 || pu.maxConst + 1 >= 224) return false;
    int tc = -1, vo = -1, pin = -1;
    for (int i = 0; i < 8; ++i) if (!coord[i]) { tc = i; break; }
    if (tc < 0) return false;
    if (v3) { for (int i = 0; i < 12; ++i) if (!output[i]) { vo = i; break; } }
    else vo = tc;
    if (p3) { for (int i = 0; i < 10; ++i) if (!input[i]) { pin = i; break; } }
    else pin = tc;
    if (vo < 0 || pin < 0) return false;
    auto v = vs, p = ps;
    const DWORD vp = vu.maxTemp + 1, po = pu.maxTemp + 1, tmp = po + 1, cMask = pu.maxConst + 1;
    for (const auto at : positionWrites) v[at] = (v[at] & ~regBits) | (Reg(kTemp, vp) & regBits);
    for (const auto at : colourWrites) p[at] = (p[at] & ~regBits) | (Reg(kTemp, po) & regBits);
    std::vector<Edit> ve, pe;
    if (v3) ve.push_back({1, {Op(kDcl, 2), 0x80000005u | (static_cast<DWORD>(tc) << 16), Dst(kOutput, vo)}});
    ve.push_back({v.size() - 1, {Op(kMov, 2), Dst(posType, posNum), Src(kTemp, vp),
                               Op(kMov, 2), Dst(kOutput, vo), Src(kTemp, vp)}});
    pe.push_back({1, {Op(kDcl, 2), p3 ? (0x80000005u | (static_cast<DWORD>(tc) << 16)) : 0x80000000u,
                     Dst(p3 ? kInput : kTexture, pin)}});
    pe.push_back({1, {Op(kDef, 5), Dst(kConst, cMask), F(hair ? -1.0f : 1.0f), F(1.0f), F(0.0f), F(0.0f)}});
    std::vector<DWORD> tail = {Op(0x06, 2), Dst(kTemp, tmp, 1), Src(p3 ? kInput : kTexture, pin, kSwzW),
                              Op(kMul, 3), Dst(kTemp, tmp, 1), Src(p3 ? kInput : kTexture, pin, 0xAA), Src(kTemp, tmp, kSwzX)};
    if (transparent) {
        tail.insert(tail.end(), {Op(kMul, 3), Dst(kTemp, po, 1), Src(kTemp, tmp, kSwzX), Src(kConst, cMask, kSwzX),
                                 Op(kMov, 2), Dst(kTemp, po, 2, true), Src(kTemp, po, kSwzW)});
    } else {
        // MIN blending selects the nearest body and hair depths independently.
        // Keep the original alpha in W so the native alpha test still rejects pixels.
        tail.insert(tail.end(), {Op(kMov, 2), Dst(kTemp, po, 1), hair ? Src(kConst, cMask, 0x55) : Src(kTemp, tmp, kSwzX),
                                 Op(kMov, 2), Dst(kTemp, po, 2), hair ? Src(kTemp, tmp, kSwzX) : Src(kConst, cMask, 0x55)});
    }
    tail.insert(tail.end(), {Op(kMov, 2), Dst(kColorOut, 0), Src(kTemp, po)});
    pe.push_back({p.size() - 1, std::move(tail)});
    if (!p3) p[0] = 0xFFFF0201u;
    Apply(v, std::move(ve)); Apply(p, std::move(pe));
    vs = std::move(v); ps = std::move(p);
    return true;
}

bool AddScreenPosVs(std::vector<DWORD>& t, int texcoord) {
    if (t.empty() || (t[0] != 0xFFFE0101 && t[0] != 0xFFFE0200 && t[0] != 0xFFFE0201)) return false;
    if (t.back() != 0x0000FFFFu) return false;
    const auto ins = Parse(t);
    if (ins.empty()) return false;
    constexpr DWORD kRastOut = 4, kCall = 0x19, kCallnz = 0x1A, kRet = 0x1C, kLabel = 0x1E;
    constexpr DWORD kRegBits = 0x70001800u | 0x7FFu;
    if (texcoord < 0 || texcoord > 7) return false;
    const DWORD kT = static_cast<DWORD>(texcoord);
    std::vector<size_t> writes;
    for (const Ins& x : ins) {
        if (x.op == kCall || x.op == kCallnz || x.op == kRet || x.op == kLabel) return false;
        if (x.op == kDcl || x.op == kDef || x.op == kDefI || x.op == kDefB) continue;
        for (size_t k = 1; k <= x.len; k++) {
            const DWORD r = t[x.at + k];
            if (!(r & 0x80000000u)) continue;
            if (Type(r) == kOutput && Num(r) == kT) return false; // that oT is the game's
            if (Type(r) == kRastOut && Num(r) == 0) writes.push_back(x.at + k);
        }
    }
    if (writes.empty()) return false;
    const Usage u = Scan(t, ins);
    if (u.maxTemp + 1 >= 12) return false;
    const DWORD P = static_cast<DWORD>(u.maxTemp + 1);
    for (size_t at : writes) t[at] = (t[at] & ~kRegBits) | (Reg(kTemp, P) & kRegBits); // oPos -> rP
    Apply(t, {{t.size() - 1, {Op(kMov, 2), Dst(kRastOut, 0), Src(kTemp, P), Op(kMov, 2), Dst(kOutput, kT), Src(kTemp, P)}}});
    return true;
}

JitterResult AddJitterVs(std::vector<DWORD>& t, int jitterConst) {
    if (t.empty() || (t[0] & 0xFFFF0000u) != 0xFFFE0000u) return JitterResult::NotVertexShader;
    if (t.back() != 0x0000FFFFu) return JitterResult::Unreadable;
    const auto ins = Parse(t);
    if (ins.empty()) return JitterResult::Unreadable;
    // Pool refraction VS captured 2026-10-02: compressed position decoding plus a
    // perspective-divided screen coordinate in oT3. Its independently rendered
    // reflection/refraction inputs are not jittered with the main scene. Keep the
    // existing game/grain vertex path for this narrowly identified shader family.
    bool poolDecode = false, poolScale = false, poolProject = false;
    for (const Ins& x : ins) {
        if (x.op == kDef && x.len == 5) {
            const DWORD* f = &t[x.at + 2];
            poolDecode |= f[0] == F(256.0f) && f[1] == F(7.96875f) && f[2] == F(-200.0f);
            poolScale |= f[0] == F(63.75f) && f[1] == F(0.0f) && f[2] == F(1.0f);
        }
        poolProject |= x.op == kMad && x.len == 4 && IsReg(t[x.at + 1], kOutput, 3) && WMask(t[x.at + 1]) == 3;
    }
    if (poolDecode && poolScale && poolProject) return JitterResult::ProjectedWater;
    constexpr DWORD kRastOut = 4, kCall = 0x19, kCallnz = 0x1A, kRet = 0x1C, kLabel = 0x1E;
    constexpr DWORD kRegBits = 0x70001800u | 0x7FFu;
    const bool v3 = ((t[0] >> 8) & 0xFF) == 3;
    // the position register: oPos (vs_1_1 / vs_2_x), or the output declared POSITION0 (vs_3_0)
    DWORD posType = kRastOut, posNum = 0;
    if (v3) {
        bool found = false;
        for (const Ins& x : ins)
            if (x.op == kDcl && x.len == 2 && Type(t[x.at + 2]) == kOutput && (t[x.at + 1] & 0x1F) == 0 && ((t[x.at + 1] >> 16) & 0xF) == 0) {
                posType = kOutput;
                posNum = Num(t[x.at + 2]);
                found = true;
            }
        if (!found) return JitterResult::NoPosition;
    }
    std::vector<size_t> writes;
    bool onlyCopies = true; // every position write is a plain copy of an input: a pre-transformed full-screen pass
    for (const Ins& x : ins) {
        if (x.op == kCall || x.op == kCallnz || x.op == kRet || x.op == kLabel) return JitterResult::Subroutines;
        if (x.op == kDcl || x.op == kDef || x.op == kDefI || x.op == kDefB) continue;
        if (x.len >= 1 && IsReg(t[x.at + 1], posType, posNum)) {
            writes.push_back(x.at + 1);
            if (!(x.op == kMov && x.len == 2 && Type(t[x.at + 2]) == kInput)) onlyCopies = false;
        }
    }
    if (writes.empty()) return JitterResult::NoPosition;
    if (onlyCopies) return JitterResult::PassThrough;
    const Usage u = Scan(t, ins);
    if (u.maxTemp + 1 >= (v3 ? 32 : 12)) return JitterResult::NoFreeRegister;
    if (jitterConst <= u.maxConst) return JitterResult::ConstantInUse;
    const DWORD P = static_cast<DWORD>(u.maxTemp + 1), J = static_cast<DWORD>(jitterConst);
    for (size_t at : writes) t[at] = (t[at] & ~kRegBits) | (Reg(kTemp, P) & kRegBits); // position -> rP, same mask and modifiers
    // clip.xy += jitter.xy * clip.w: the whole image moves by a fraction of a pixel, the depth does not change
    Apply(t, {{t.size() - 1, {Op(kMad, 4), Dst(kTemp, P, 0x3), Src(kTemp, P, kSwzW), Src(kConst, J), Src(kTemp, P),
                              Op(kMov, 2), Dst(posType, posNum), Src(kTemp, P)}}});
    return JitterResult::Ok;
}

// ---- The snowy and melting-snow lot light passes (lot_light_bridge DrawLotSnow / DrawLotMelt; moved here from
// lot_light_bridge.cpp in 2.15.3 to be checked offline by tools/floor_shade_check). The same patch for the snowy pass (last
// sampler s11, terrain light in s12) and the melting-snow pass (kMeltLotPs: last sampler s13, terrain light in s14): after
// "texld r0, v3, s2" (lot light map) and the following "mul r0.xyz, r1.w, r0" (light basis factor) insert
// "texld r7, v7(TEXCOORD1), s<new>", r7 x4 and "max r0.xyz, r0, r7". r7, v7 and the new sampler must be unused (checked).
// With mask (room mask): also "mad rM.x/y" (mask uv from v7 with PS c200 / c201), "texld rM, rM, s<new+1>" and
// "mul r7.xyz, r7, rM.x" before the max, so the terrain light stays outside the lot's rooms; c200, c201 and that sampler
// must be unused too. After the max, "mul r0.xyz, r0, rM.w": the lamp shade (the mask's alpha) on both terms. (2.15.3) rM
// is a temp the shader does not use (its highest + 1, at least r8): it was r6, which the game's snowy lot shader uses, so
// every snowy pass drew without the mask and the lamp shade while the world grass beside it took the shade (a step at the
// lot border at night in winter).
// (2.15.3) With exit (and mask): indoor light on the ground, as in the dry pass (kReplacementHlsl c34). The lot map's alpha
// a (the game's shader does not read it here: the texld writes r0.w and the basis factor's mul does not, checked) times the
// gate c202.x gives s = sat(a / max(lum(map), 1/255)) right after the texld, before the basis factor; then, before the max,
// ex = r0 s (rF), r0 -= ex, r7 *= sat((1 - a) 255), and after the shade r0 += ex. rE = rM + 1, rF = rM + 2; the luminance
// weights and 255 are defs in c203 / c204 (c202..c204 must be unused). Gate 0: s = 0, ex = 0 and the weight 1, the same
// values as without it. The snowy pass has no soft lot edge (w = 1).
bool PatchLotTerrainMax(std::vector<DWORD>& t, DWORD lastSampler, DWORD newSampler, bool mask, bool exit) {
    auto regNum = [](DWORD r) { return r & 0x7FF; };
    auto regType = [](DWORD r) { return ((r >> 28) & 7) | (((r >> 11) & 3) << 3); };
    if (exit && !mask) return false;
    size_t dclEnd = 0, texldEnd = 0, texldAt = 0;
    DWORD maxTemp = 0;
    for (size_t i = 1; i < t.size();) {
        const DWORD tok = t[i];
        if (tok == 0x0000FFFF) break;
        if ((tok & 0xFFFF) == 0xFFFE) { i += 1 + ((tok >> 16) & 0x7FFF); continue; }
        const size_t len = (tok >> 24) & 0xF;
        const DWORD op = tok & 0xFFFF;
        if (i + len >= t.size()) return false;
        for (size_t k = 1; k <= len; k++) {
            const DWORD r = t[i + k];
            if (op == 0x1F && k == 1) continue; // dcl usage token
            if (op == 0x51 && k > 1) continue;  // def values
            if (!(r & 0x80000000u)) continue;
            const DWORD ty = regType(r), n = regNum(r);
            if ((ty == 0 && n == 7) || (ty == 1 && n == 7) || (ty == 10 && n == newSampler)) return false; // r7 / v7 / new sampler taken
            if (mask && ((ty == 2 && (n == kLotMaskConst || n == kLotMaskConst + 1)) || (ty == 10 && n == newSampler + 1))) return false;
            if (exit && ty == 2 && n >= kLotMaskConst + 2 && n <= kLotMaskConst + 4) return false; // the gate and the defs
            if (ty == 0) maxTemp = std::max(maxTemp, n);
        }
        if (op == 0x1F && regType(t[i + 2]) == 10 && regNum(t[i + 2]) == lastSampler) dclEnd = i + 1 + len;
        if (op == 0x42 && !texldEnd && regType(t[i + 1]) == 0 && regNum(t[i + 1]) == 0 && regType(t[i + 2]) == 1 && regNum(t[i + 2]) == 3 && regType(t[i + 3]) == 10 && regNum(t[i + 3]) == 2) {
            texldEnd = i + 1 + len;
            texldAt = i;
        }
        i += 1 + len;
    }
    if (!dclEnd || !texldEnd || texldEnd < dclEnd || texldEnd + 1 >= t.size()) return false;
    // next instruction must be "mul r0.xyz, r1.w, r0" (light basis factor)
    if ((t[texldEnd] & 0xFFFF) != 0x05 || regType(t[texldEnd + 1]) != 0 || regNum(t[texldEnd + 1]) != 0) return false;
    const DWORD r = std::max<DWORD>(maxTemp + 1, 8); // the mask's temp (see above)
    // (exit) the lot map's alpha: the texld writes r0.w and the basis factor's mul does not; two more temps
    if (exit && (!(t[texldAt + 1] & 0x00080000u) || (t[texldEnd + 1] & 0x00080000u) || r + 2 > 31)) return false;
    const size_t insertAt = texldEnd + 1 + ((t[texldEnd] >> 24) & 0xF);
    const DWORD fetch[] = {0x03000042, 0x802F0007, 0x90E40007, 0xA0E40800 | newSampler, // texld_pp r7, v7, s<new>
                           0x03000002, 0x80270007, 0x80E40007, 0x80E40007,              // add_pp r7.xyz, r7, r7
                           0x03000002, 0x80270007, 0x80E40007, 0x80E40007,              // add_pp r7.xyz, r7, r7
                           0x0300000B, 0x80270000, 0x80E40000, 0x80E40007};             // max_pp r0.xyz, r0, r7
    t.insert(t.begin() + insertAt, std::begin(fetch), std::end(fetch));
    if (mask) {
        const DWORD m = newSampler + 1, c0 = 0xA0000000 | kLotMaskConst, c1 = 0xA0000000 | (kLotMaskConst + 1);
        const DWORD ops[] = {0x04000004, 0x80010000 | r, 0x90000007, c0,              c0 | 0x00AA0000, // mad rM.x, v7.x, c200.x, c200.z
                             0x04000004, 0x80010000 | r, 0x90550007, c0 | 0x00550000, 0x80000000 | r,  // mad rM.x, v7.y, c200.y, rM.x
                             0x04000004, 0x80020000 | r, 0x90000007, c1,              c1 | 0x00AA0000, // mad rM.y, v7.x, c201.x, c201.z
                             0x04000004, 0x80020000 | r, 0x90550007, c1 | 0x00550000, 0x80550000 | r,  // mad rM.y, v7.y, c201.y, rM.y
                             0x03000042, 0x800F0000 | r, 0x80E40000 | r, 0xA0E40800 | m,             // texld rM, rM, s<m>
                             0x03000005, 0x80070007, 0x80E40007, 0x80000000 | r};                    // mul r7.xyz, r7, rM.x
        t.insert(t.begin() + insertAt + 12, std::begin(ops), std::end(ops)); // after the texld and the two adds, before the max
        size_t beforeMax = insertAt + 12 + std::size(ops);
        const DWORD E = r + 1, F = r + 2, cG = kLotMaskConst + 2, cL = kLotMaskConst + 3, cK = kLotMaskConst + 4;
        if (exit) {
            const DWORD pre[] = {0x03000005, 0x80070000 | F, 0x80E40000, 0x80000000 | E,                   // mul rF.xyz, r0, rE.x (ex)
                                 0x03000002, 0x80070000, 0x80E40000, 0x81E40000 | F,                       // add r0.xyz, r0, -rF
                                 0x04000004, 0x80140000 | E, 0x81550000 | E, 0xA0000000 | cK, 0xA0000000 | cK, // mad_sat rE.z, -rE.y, c204.x, c204.x
                                 0x03000005, 0x80070007, 0x80E40007, 0x80AA0000 | E};                     // mul r7.xyz, r7, rE.z
            t.insert(t.begin() + beforeMax, std::begin(pre), std::end(pre));
            beforeMax += std::size(pre);
        }
        // the lamp shade (the mask's alpha, 1 with no wall between): x the whole lamp term, after the max
        const DWORD shade[] = {0x03000005, 0x80270000, 0x80E40000, 0x80FF0000 | r}; // mul_pp r0.xyz, r0, rM.w
        t.insert(t.begin() + beforeMax + 4, std::begin(shade), std::end(shade));
        if (exit) {
            const DWORD post[] = {0x03000002, 0x80070000, 0x80E40000, 0x80E40000 | F}; // add r0.xyz, r0, rF (ex, after the shade)
            t.insert(t.begin() + beforeMax + 8, std::begin(post), std::end(post));
            const DWORD share[] = {0x03000008, 0x80010000 | E, 0x80E40000, 0xA0E40000 | cL,             // dp3 rE.x, r0, c203 (lum of the map)
                                   0x0300000B, 0x80010000 | E, 0x80000000 | E, 0xA0FF0000 | cL,         // max rE.x, rE.x, c203.w
                                   0x02000006, 0x80010000 | E, 0x80000000 | E,                          // rcp rE.x, rE.x
                                   0x03000005, 0x80020000 | E, 0x80FF0000, 0xA0000000 | cG,             // mul rE.y, r0.w, c202.x (a)
                                   0x03000005, 0x80110000 | E, 0x80550000 | E, 0x80000000 | E};         // mul_sat rE.x, rE.y, rE.x (s)
            t.insert(t.begin() + texldEnd, std::begin(share), std::end(share)); // before the basis factor's mul
            const auto bits = [](float f) {
                DWORD d;
                std::memcpy(&d, &f, 4);
                return d;
            };
            const DWORD defs[] = {0x05000051, 0xA00F0000 | cL, bits(0.2126f), bits(0.7152f), bits(0.0722f), bits(1.0f / 255.0f),
                                  0x05000051, 0xA00F0000 | cK, bits(255.0f), bits(1.0f), 0, 0};
            t.insert(t.begin() + dclEnd, std::begin(defs), std::end(defs));
        }
        const DWORD mdecl[] = {0x0200001F, 0x90000000, 0xA00F0800 | m};
        t.insert(t.begin() + dclEnd, std::begin(mdecl), std::end(mdecl));
    }
    const DWORD decl[] = {0x0200001F, 0x80010005, 0x90230007, 0x0200001F, 0x90000000, 0xA00F0800 | newSampler}; // v7 = TEXCOORD1, s<new> 2D
    t.insert(t.begin() + dclEnd, std::begin(decl), std::end(decl));
    return true;
}
} // namespace ShaderPatches
