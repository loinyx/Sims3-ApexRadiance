// Real-time outdoor lamps (features/rt_lamp_gpu.cpp): the GPU passes. Compiled at run time with D3DCompile (vs_3_0 / ps_3_0,
// O3) from this string; the offline harness (tools/rt_lamps_test) compiles the same string. (Several raw literals: MSVC
// takes at most 16 KB in one.)
#pragma once
static const char* kRtLampsHlsl = R"RAW(
// ---- Real-time outdoor lamps: shadows of the lot's walls, roofs and slabs, and the relit room-0 texels ----
// Every operation mirrors features/rt_lamp_model.h (and gpu_lamp_law.h, outdoor_delta_model.h): keep them in step.
//
// ShadowVS / ShadowPS   one face of a lamp's cube shadow map: the proxy's triangles (lot space) seen from the lamp, the
//                       distance to the lamp / far into an R32F tile (1 = nothing), the nearest kept by the depth test
// AccPS                 one point per room-0 texel (pre-transformed, at the texel): the GPU lamps of this batch added to
//                       the sum of the batches before (A32B32G32R32F, read at the same texel): rgb = the light, a = the
//                       floor mask's walls-tested luminance
// ComposePS             one point per texel: base + the sum, the floor mask's alpha, the dither, packed to 8 bits as the
//                       game's pack (round to nearest even of x 255; a 16-bit float target: neither, Packed)
// CopyPS                the game's map as it is (point)
// BlurRowPS / BlurColPS one pass of the game's class-2 wall blur (FUN_0069f650 mode 0) along a block's rows / columns, on
//                       8-bit values (outdoor_delta_model.h BlurRow / BlurColumn: avg(p, avg(next, prev)), clamped to the
//                       block, avg = the per-byte (a | b) - ((a ^ b) >> 1) = ceil((a + b) / 2))
// BlockPS               a blurred block into the map: its first and last rows from the rows-only chain (an edge with
//                       nothing beyond it keeps its own light, blurred along the wall only), the others from the full chain

// The cube faces (+X, -X, +Y, -Y, +Z, -Z): right, up and forward as rt_lamp_gpu.cpp kFaces; Face() gives d in a face's frame
// (x right, y up, z forward) without indexing a table (ps_3_0 has no indexable temporaries)
float3 Face(float3 d, out float f) {
    const float3 a = abs(d);
    // (selects on scalars: three float3 branches, flattened, exceed ps_3_0's temporary registers inside Shadow's walk)
    const bool onX = a.x >= a.y && a.x >= a.z, onY = !onX && a.y >= a.z;
    const float s = onX ? d.x : onY ? d.y : d.z;
    f = (onX ? 0 : onY ? 2 : 4) + (s > 0 ? 0 : 1);
    const float zs = s > 0 ? -d.z : d.z;
    return float3(onX ? zs : (onY || s > 0) ? d.x : -d.x, onY ? zs : d.y, abs(s));
}

// ---------------------------------------------------------------- shadows
float4 sFaceR : register(c0);
float4 sFaceU : register(c1);
float4 sFaceF : register(c2);
float4 sLamp : register(c3);  // xyz lamp (lot space), w = 1 / far
float4 sProj : register(c4);  // x = A, y = B (z_clip = A z + B), w_clip = z

struct ShadowOut {
    float4 pos : POSITION;
    float4 d : TEXCOORD0; // lamp -> point / far, how far the triangle's plane passes from the lamp / far
};
ShadowOut ShadowVS(float3 p : POSITION, float3 n : NORMAL) {
    ShadowOut o;
    const float3 v = p - sLamp.xyz;
    const float z = dot(v, sFaceF.xyz);
    o.pos = float4(dot(v, sFaceR.xyz), dot(v, sFaceU.xyz), sProj.x * z + sProj.y, z);
    o.d = float4(v * sLamp.w, abs(dot(n, v)) * sLamp.w); // (/ far here: the pixel shader has no vertex constants)
    return o;
}
float4 ShadowPS(float4 d : TEXCOORD0) : COLOR0 { return float4(length(d.xyz), d.w, 0, 0); }

)RAW"
R"RAW(// ---------------------------------------------------------------- relight
// c0..c95: 16 lamps, one array per row kind (indexed by the loop counter): (pos, W) (colour, cmax) (axis, sc1)
// (off1, sc2, off2, r0^2) (spill, 1 / R^2) (tile row, mult, tile column's first face, 0)
float4 gPos[16] : register(c0);
float4 gCol[16] : register(c16);
float4 gAxis[16] : register(c32);
float4 gOff[16] : register(c48);
float4 gSpill[16] : register(c64);
float4 gMisc[16] : register(c80);
float4 gA : register(c96);  // lamps, threshold, fade (0/1), reach (0/1)
float4 gB : register(c97);  // lamp radius (m), face size, 1 / atlas width, 1 / atlas height
float4 gC : register(c98);  // half texel of the map at the receiver (m), normal offset (texels), bias (m), bias (texels)
float4 gD : register(c99);  // 1 / far, far, 1 / map width, 1 / map height
float4 gE : register(c100); // first batch (1: no sum before), mask (0/1), not used, debug lamp (-1 none)
sampler2D sAtlas : register(s0);
sampler2D sAcc : register(s1);

struct TexelIn {
    float4 p : TEXCOORD0;    // lot position, flags (1 masked, 2 gated)
    float4 n : TEXCOORD1;    // lot normal, the record's alpha
    float4 base : TEXCOORD2; // the texel without the GPU lamps, dither (levels / 255)
    float4 mask : TEXCOORD3; // the floor mask's luminances without the GPU lamps (walls tested, none)
    float4 dyn : TEXCOORD4;  // the GPU lamps' luminance with no walls, under (-1 none)
};

// The points of the texels: TexelVS puts each at its texel of the target (c0 = 2 / width, -2 / height; D3D9 samples pixel x
// at its integer coordinate, so texel x sits at x x 2 / width - 1) and hands the texel's data to the pixel shader
float4 gTarget : register(c0);
struct TexelVsIn {
    float4 t : POSITION; // the texel (x, y)
    float4 p : TEXCOORD0;
    float4 n : TEXCOORD1;
    float4 base : TEXCOORD2;
    float4 mask : TEXCOORD3;
    float4 dyn : TEXCOORD4;
};
struct TexelVsOut {
    float4 pos : POSITION;
    float4 p : TEXCOORD0;
    float4 n : TEXCOORD1;
    float4 base : TEXCOORD2;
    float4 mask : TEXCOORD3;
    float4 dyn : TEXCOORD4;
};
TexelVsOut TexelVS(TexelVsIn i) {
    TexelVsOut o;
    o.pos = float4(i.t.x * gTarget.x - 1.0, i.t.y * gTarget.y + 1.0, 0.5, 1.0);
    o.p = i.p, o.n = i.n, o.base = i.base, o.mask = i.mask, o.dyn = i.dyn;
    return o;
}

float Sat(float v) { return saturate(v); }

// gpu_lamp_law.h Eval (the same operations in the same order) for a lamp read into registers
struct LampRegs {
    float4 pos, col, axis, off, spill, misc;
};
LampRegs ReadLamp(int i) {
    LampRegs r;
    r.pos = gPos[i], r.col = gCol[i], r.axis = gAxis[i], r.off = gOff[i], r.spill = gSpill[i], r.misc = gMisc[i];
    return r;
}
float3 Law(LampRegs L, float3 p, float3 n, float wallReach) {
    float3 l = L.pos.xyz - p;
    const float plane = dot(n, l);
    const float d2 = max(dot(l, l), 1e-4);
    const float inv = rsqrt(d2);
    l *= inv;
    // (gpu_lamp_law.h MountedNdl, MountScale) a lamp within 0.4 m in front of a wall lights that wall at N.l 0.5 x (1 - plane / 0.4) at
    // least, raised after the Cmax cut as the game's hook does (at most 8x, none where N.l is 0)
    const float mount = 0.5 * Sat((0.5 - abs(n.y)) * 1e4) * Sat(plane * 1e4) * Sat(1.0 - plane * 2.5);
    const float ndl = Sat(dot(n, l)), raised = max(ndl, mount);
    const float k = raised > ndl && ndl > 1e-3 ? min(raised / ndl, 8.0) : 1.0;
    const float f = min(ndl * (inv * inv) * L.pos.w, L.col.w) * k;
    const float al = dot(L.axis.xyz, l);
    const float t1 = (al + L.off.x) * -L.axis.w, t2 = (al - L.off.z) * L.off.y;
    const float tsel = t1 > 0 ? t1 : t2;
    const float m = Sat((max(t1, t2) - 1.0) * 1e4 + 1.0);
    const float t = Sat(max(tsel, m));
    const float q = d2 * L.spill.w;
    float win = Sat(1.0 - q * q);
    win *= win;
    float share = min(win * ((1.0 / d2) * L.off.w), 1.0);
    share = (share - 1.0) * wallReach + 1.0;
    const float s = f * share;
    const float3 S = L.spill.xyz;
    return (S - S * t + t) * s * L.col.xyz;
}
// gpu_lamp_law.h Tube (a TubeLight; sc2 = gOff.y = kTube): its light from the segment A (pos) .. A + axis, len^2 = axis.w; no reach (the
// kept parts draw it with none)
float3 TubeLaw(LampRegs L, float3 p, float3 n) {
    const float3 v = p - L.pos.xyz, s = L.axis.xyz;
    const float t = dot(s, v);
    const float3 e = 0.0 > t ? v : (t > L.axis.w ? v - s : v - (t / dot(s, s)) * s);
    const float d2 = max(dot(e, e), 1e-4);
    const float inv = rsqrt(d2);
    const float facing = max(-dot(n, e) * inv, 0.0);
    return min((inv * inv) * L.pos.w, L.col.w) * facing * L.col.xyz;
}
// gpu_lamp_law.h Area (an area light; gOff.y = kRect -2 or kDisc -3): L = (U.v, V.v, N.v) for v = A - p (U = axis.xyz, V =
// spill.xyz, N = (axis.w, off.x, off.z)), its first two lanes held in the rectangle (+-(off.w, spill.w)) or the disc (length
// spill.w past off.w), D = L - held; no N.l, no reach
float3 AreaLaw(LampRegs L, float3 p) {
    const float3 v = L.pos.xyz - p;
    const float3 l = float3(dot(L.axis.xyz, v), dot(L.spill.xyz, v), dot(float3(L.axis.w, L.off.x, L.off.z), v));
    const float2 e = float2(L.off.w, L.spill.w);
    const float s = dot(l.xy, l.xy);
    const float2 c = L.off.y < -2.5 ? l.xy * (s > e.x ? rsqrt(s) * e.y : 1.0) : min(max(l.xy, -e), e);
    const float3 D = float3(l.xy - c, l.z);
    const float d2 = max(dot(D, D), 1e-20);
    const float inv = rsqrt(d2);
    return min((inv * inv) * L.pos.w, L.col.w) * Sat(D.z * inv) * L.col.xyz;
}
// A kept part's law (lamp 0): the colour alone (gA.z 1), an area light's, a tube's or the GPU's
float3 KeptLaw(float3 p, float3 n) {
    const LampRegs L = ReadLamp(0);
    return gA.z > 0.5 ? gCol[0].rgb : L.off.y < -1.5 ? AreaLaw(L, p) : L.off.y < -0.5 ? TubeLaw(L, p, n) : Law(L, p, n, 0.0);
}

// The atlas along direction d from lamp tile k (k.x its row, k.y its column's first face; m): x = the nearest surface's distance, y = how far its plane passes from the
// lamp; dc = the direction of that texel's centre (the receiver's own depth is taken along it, so a surface and its own
// shadow texel agree to rounding)
float2 Stored(float2 k, float3 d, out float3 dc) {
    float f;
    const float3 v = Face(d, f);
    const float2 uv = float2(v.x / v.z * 0.5 + 0.5, -v.y / v.z * 0.5 + 0.5);
    const float F = gB.y;
    const float2 t = clamp(floor(uv * F + 0.5), 0, F - 1);
    const float2 texel = float2((f + k.y) * F, k.x * F) + t;
    // the centre of texel t is at uv t / F (D3D9 samples a pixel at its integer coordinate): back to a direction
    const float2 c = float2(t.x / F * 2.0 - 1.0, 1.0 - t.y / F * 2.0);
    float3 w;
    if (f < 0.5) w = float3(1, c.y, -c.x);
    else if (f < 1.5) w = float3(-1, c.y, c.x);
    else if (f < 2.5) w = float3(c.x, 1, -c.y);
    else if (f < 3.5) w = float3(c.x, -1, c.y);
    else if (f < 4.5) w = float3(c.x, c.y, 1);
    else w = float3(-c.x, c.y, -1);
    dc = normalize(w);
    return tex2Dlod(sAtlas, float4((texel + 0.5) * gB.zw, 0, 0)).rg * gD.y;
}
// The receiver's depth along dc: its plane's (planeD / N.dc), dr when the plane is seen edge on
float Recv(float3 N, float3 dc, float planeD, float dr) {
    const float dn = dot(N, dc);
    return abs(dn) > 0.05 ? clamp(planeD / dn, dr * 0.5, dr * 2.0) : dr;
}

// The share of the lamp's ball the point sees: PCSS over the cube map. Blockers are searched on the rim of the cone through
// which the point sees the ball, at the walk's depths (a disc sized for the nearest blocker put no tap inside the narrow cone of
// a far one: the share jumped to 1 where the centre ray cleared a jamb, 05/11); the penumbra from their mean distance; then the
// share of taps in that disc that see past every surface (at least the map's half texel and one shadow texel wide). The receiver's depth at
// each tap is its own plane's along the tap's texel centre, so a receiver is never shadowed by its own surface. The emitter is
// the part of the ball its centre sees (rt_lamp_proxy.h Visibility): a surface whose plane passes through the ball (a ceiling
// 5 cm over a hanging lamp, the wall a sconce is mounted on) bounds the light and shades nothing on the lamp's side of it;
// a point behind it (its centre ray blocked by such a surface) sees none of the lamp.
// kSearch, kEmit, kWalk: 24, 16, 16 (AccPS); 8, 4, 8 while a lamp is dragged (AccFastPS, then AccPS again on release)
// fr (m, 0 none: a sharper floor's sub-texel, PhysLampPick::SubTexelFootprint): the receiver's footprint half width; the share is
// the footprint's mean (the emitter widened by it as seen through the blockers), so a shadow edge sharper than the map's texel (a
// door jamb's near the door) does not step from texel to texel
float Shadow(LampRegs lamp, float3 P, float3 N, int kSearch, int kEmit, int kWalk, float fr) {
    const float2 k = lamp.misc.xz;
    const float3 L = lamp.pos.xyz;
    const float R = gB.x;
    const float3 v0 = P - L;
    const float d0 = max(length(v0), 1e-3);
    const float texelA = 2.0 / gB.y;      // a shadow texel's angle at a face's centre (up to twice at its edge)
    const float texelW = d0 * texelA;     // its width at the point
    const float3 Pn = P + N * (gC.y * texelW);
    const float3 v = Pn - L;
    const float dr = max(length(v), 1e-3);
    const float3 dir = v / dr;
    const float bias = gC.z + gC.w * texelW;
    const float3 up = abs(dir.y) < 0.9 ? float3(0, 1, 0) : float3(1, 0, 0);
    const float3 t1 = normalize(cross(dir, up));
    const float3 t2 = cross(dir, t1);
    const float planeD = dot(N, v);
    const float nearM = R + 0.02;
    float3 dc;
    // (one return at the end: no early exit inside the caller's lamp loop)
    const float2 c0 = Stored(k, dir, dc);
    const bool behindNear = c0.x < Recv(N, dc, planeD, dr) - bias && (c0.x < nearM || c0.y < R); // behind a surface the lamp touches
    const float zEnd = dr - max(2.0 * texelW, 0.05); // (the point's own surface is not walked into)
    const float lnZ = log(max(zEnd, nearM * 1.01) / nearM);
    float bSum = 0, bN = 0;
    if (c0.x >= nearM && c0.y >= R && c0.x < Recv(N, dc, planeD, dr) - bias) {
        bSum = c0.x;
        bN = 1;
    }
    [loop] for (int s = 0; s < kSearch; s++) {
        const float z = nearM * exp(lnZ * (s + 0.5) / kSearch);
        const float a = s * 2.39996323;
        const float3 X = dir * z + (t1 * cos(a) + t2 * sin(a)) * (R * (1.0 - z / dr) + fr * z / dr);
        const float2 sp = Stored(k, X, dc);
        if (sp.y >= R && sp.x >= nearM && sp.x < length(X) - bias) {
            bSum += sp.x;
            bN += 1;
        }
    }
    // Where something may shade the point, the share is measured as the reference measures it: kEmit points of the ball's
    // disc across the ray (equal-area rings, golden-angle turns); a point the lamp's centre does not see (behind a surface
    // within the ball) gives no light; for each other one the segment to the point is walked through the shadow map in
    // steps of one ratio of z up to zEnd (even steps left a wall 3 m off, near the cone's rim, between two of them): it is blocked
    // where its point lies behind the nearest surface seen from the lamp.
    float share = 1.0;
    if (bN > 0.5) {
        float emit = 0, seen = 0;
        // (fr) the footprint seen through the blockers' mean distance zb is a disc fr zb / (dr - zb) wide at the lamp: the emitter
        // disc widened by it (in quadrature) gives the footprint's mean share without a receiver point per tap
        const float zb = bSum / bN;
        const float fe = fr * zb / max(dr - zb, 0.05);
        const float Re = fr > 0.0 ? min(sqrt(R * R + fe * fe), R + 0.5) : R;
        [loop] for (int j = 0; j < kEmit; j++) {
            const float ring = sqrt((j + 0.5) / kEmit);
            const float rho = R * ring;
            const float a = j * 2.39996323;
            const float3 o = (t1 * cos(a) + t2 * sin(a)) * (Re * ring);
            float3 dq;
            const float2 sq = Stored(k, o, dq);
            const bool valid = sq.x >= rho; // the lamp's centre sees this point of its ball
            // Behind what the lamp's centre sees: that alone cuts nothing (a segment passing a door jamb lies behind the jamb as the
            // centre sees it, which lost the outer half of the penumbra, 05/11). A step entering a surface's shadow cuts when the
            // segment really crosses that surface: the same plane as the step before (a solid wall), or, at that depth along the
            // segment, a surface in front of the point's own plane (that surface, or past a jamb a wall that runs on behind the jamb, hidden from
            // the centre: the segment meets it too, 05/11) or the plane still seen
            float blocked = 0, hPrev = 1e9; // (hPrev: the step before's plane, negative when that step was behind it)
            [loop] for (int m = 0; m < kWalk; m++) {
                [branch] if (blocked < 0.5) {
                    const float z = nearM * exp(lnZ * (m + 1.0) / kWalk);
                    const float3 X = dir * z + o * (1.0 - z / dr); // the segment's point, from the lamp
                    const float dX = length(X);
                    const float2 sx = Stored(k, X, dq);
                    const bool behind = sx.y >= R && sx.x >= nearM && sx.x < dX - bias;
                    const bool same = abs(sx.y - abs(hPrev)) < 0.02;
                    [branch] if (behind && (!same || hPrev >= 0)) {
                        bool cut = m == 0 || (same && hPrev >= 0);
                        [branch] if (!cut) {
                            const float3 Xc = dir * sx.x + o * (1.0 - sx.x / dr);
                            const float2 sc = Stored(k, Xc, dq);
                            cut = sc.x < Recv(N, normalize(Xc), planeD, dr) - bias || abs(sc.y - sx.y) < 0.02;
                        }
                        blocked += cut ? 1.0 : 0.0;
                    }
                    hPrev = behind ? -sx.y : sx.y;
                }
            }
            emit += valid ? 1.0 : 0.0;
            seen += (valid && blocked < 0.5) ? 1.0 : 0.0;
        }
        share = emit > 0.5 ? seen / emit : 1.0;
    }
    return behindNear ? 0.0 : share;
}

float Lum3(float3 c) { return c.r * 0.2126 + c.g * 0.7152 + c.b * 0.0722; }

)RAW"
R"RAW(float4 Acc(TexelIn i, float2 vpos, int kSearch, int kEmit, int kWalk) {
    float4 sum = gE.x > 0.5 ? float4(0, 0, 0, 0) : tex2Dlod(sAcc, float4((vpos + 0.5) * gD.zw, 0, 0));
    const float3 P = i.p.xyz;
    const float3 N = normalize(i.n.xyz);
    const bool masked = fmod(i.p.w, 2.0) >= 1.0 && gE.y > 0.5;
    // (15/10) the lot's ground (bit 4): no wall test, as the game's own solve of ground texels; the lot passes shade it with the
    // lot's smooth ground shade (lot_light_bridge.cpp LotMask), which stepped against the map's texel-sized shadow edges
    // (physical, bit 8) the upper half of a double-height room's wall (RtLampProxy::OverOpenBelow): the room's, no outdoor lamp
    const bool inner = i.p.w >= 8.0, ground = !inner && i.p.w >= 4.0;
    const float wallReach = gA.w > 0.5 ? Sat((0.5 - abs(N.y)) * 1e4) : 0.0;
    const int count = (int)gA.x;
    const float thr = gA.y;
    // (no continue, break or return in the loop: a continue after a lamp that adds nothing ended the loop on the GPU tested)
    float4 dbg = float4(-1, 0, 0, 1);
    [loop] for (int l = 0; l < 16; l++) {
        const LampRegs lamp = ReadLamp(l);
        const float3 law = Law(lamp, P, N, wallReach);
        float3 wrapped = 0;
        if (masked) {
            const float3 toL = normalize(lamp.pos.xyz - P);
            wrapped = Law(lamp, P, toL, 0.0);
        }
        const bool maskOn = masked && thr <= (wrapped.b + wrapped.g) + wrapped.r;
        if (l < count && !inner && ((law.r + law.g + law.b) > 0 || maskOn)) {
            const float sh = ground ? 1.0 : Shadow(lamp, P, N, kSearch, kEmit, kWalk, 0.0);
            if (abs(gE.w - l) < 0.5) dbg = float4(sh, Lum3(law), 0, 1); // debug: the share alone
            float3 c = law * sh;
            const float cs = (c.b + c.g) + c.r;
            if (!(cs >= thr)) c = 0;
            else if (gA.z > 0.5 && cs < 2.0 * thr) {
                const float x = (cs - thr) / thr;
                c *= x * x * (3.0 - 2.0 * x);
            }
            const float mult = lamp.misc.y;
            sum.rgb += mult * c;
            if (maskOn) sum.a += mult * Lum3(wrapped) * sh;
        }
    }
    return gE.w >= 0 ? dbg : sum; // (debug: -1 = the lamp did not reach this texel)
}
float4 AccPS(TexelIn i, float2 vpos : VPOS) : COLOR0 { return Acc(i, vpos, 24, 16, 16); }
float4 AccFastPS(TexelIn i, float2 vpos : VPOS) : COLOR0 { return Acc(i, vpos, 8, 4, 8); }

// Round to nearest, ties to even (the game's pack: x 255 + 1.5 x 2^23)
float Rne(float x) {
    const float f = floor(x);
    const float r = x - f;
    if (r > 0.5) return f + 1;
    if (r < 0.5) return f;
    return fmod(f, 2.0) == 0 ? f : f + 1;
}
float Fin(float v) { return (v == v && abs(v) < 1e30) ? v : 0.0; }

// c101.x = 1: the floor mask's luminance of the GPU lamps with no walls ("all") is the lamps' of the last batch (c0..c96, the
// relight's only batch), evaluated here as AccPS evaluates "vis" without the shadow; 0: the CPU's (dyn.x)
// c101.y = 1: the target is a 16-bit float one (a sharper floor, RtLampGpu::SharperFormat): no dither and no pack (Packed)
float4 gF : register(c101);
// A composed texel into its target: the dither and the game's 8-bit pack, or (gF.y) the texel as it is: the dither is there for 8
// bits, and read at 16 texels a metre it showed as a lattice in dim light (05/11)
float4 Packed(float4 c, float dither) {
    if (gF.y > 0.5) return c;
    if (dither != 0) c.rgb = clamp(c.rgb + dither, 0.0, 1.0);
    return float4(Rne(c.r * 255.0), Rne(c.g * 255.0), Rne(c.b * 255.0), Rne(c.a * 255.0)) / 255.0;
}
float MaskAll(float3 P) {
    float all = 0;
    const int count = (int)gA.x;
    [loop] for (int l = 0; l < 16; l++) {
        const LampRegs lamp = ReadLamp(l);
        const float3 toL = normalize(lamp.pos.xyz - P);
        const float3 wrapped = Law(lamp, P, toL, 0.0);
        if (l < count && gA.y <= (wrapped.b + wrapped.g) + wrapped.r) all += lamp.misc.y * Lum3(wrapped);
    }
    return all;
}
// COLOR1: the outdoor scale (rt_outdoor_lamps.h ScaleOf), the factor KeepHue took off the sum: log2(largest channel) / 3 in
// 8 bits (1 to 8 times, 0.8 % a level), 0 within 1, in x, z and w. Bound only on maps the draws multiply back (no wall blur).
// y = 1: a room-0 texel inside a room of the story below open to this one (bit 8, RtLampProxy::OverOpenBelow), which the
// outside walls' copy (ShaderPatches::PatchWallScale) lights as an indoor wall; z of a bit-16 texel: its fall-off ratio (IndoorRatio)
struct ComposeOut {
    float4 c : COLOR0;
    float4 k : COLOR1;
};
// (physical, room 0) a texel of the space a room of the story below continues into (bit 8, mask.z its slot, mask.w world y) as
// that room's wall texel (gpu_lamp_law.h IndoorTexel with the pass's ambient left to the draw, as the game leaves a wall's):
// rgb the toned sum, alpha 1 - sat(k x the toned sum) x the room's ramp at its height (GpuLampLaw::SplitFill line). The outside
// walls' and floors' copies add alpha x that room's ambient at night (ShaderPatches::PatchWallScale), as InteriorWall adds
// alpha x its ambient (c4 = +0x110). gF.z = k (0: off), gF.w = 1 ToneSoft; c104..c107 the slots' ramp lines (a, b, lo, hi)
float4 gInFit[4] : register(c104);
float4 IndoorOf(float3 S, float4 m) {
    const float4 f = m.z < 0.5 ? gInFit[0] : m.z < 1.5 ? gInFit[1] : m.z < 2.5 ? gInFit[2] : gInFit[3];
    const float3 x = max(float3(Fin(S.r), Fin(S.g), Fin(S.b)), 0.0);
    const float mean = max((x.b * 0.33333334 + x.g * 0.33333334) + x.r * 0.33333334, 1e-6);
    const float tanhOver = (2.0 / (exp(-mean) + 1.0) - 1.0) / mean;
    const float3 t = x * (gF.w > 0.5 ? lerp(1.0, tanhOver, smoothstep(0.004, 0.02, mean)) : (mean >= 0.01 ? tanhOver : 1.0));
    const float w = 1.0 - saturate(gF.z * ((t.b + t.g) + t.r));
    return float4(t, w * clamp(f.x + f.y * m.w, f.z, f.w));
}
float4 ScaleOut(float sm, float flags) {
    const float k = sm > 1.0 ? Rne(saturate(log2(sm) / 3.0) * 255.0) / 255.0 : 0.0;
    return float4(k, flags >= 8.0 ? 1.0 : 0.0, k, k);
}
// (bit 16, with bit 8: a wall texel matched to a texel of its room's map, mask.xy the lowest marked texel of its column) the GPU
// lamps' fall-off up the wall, lum(base + lamps here) / lum(base + lamps at the floor line) (1 there, where it is not darker and
// where no lamp reaches it), in the outdoor scale's .z, which the outside walls' copy multiplies its room's texel by (ShaderPatches
// IndoorPick rooms). base: the matched top-row texel's light the GPU lamps do not draw (its daylight, baked lights, bounce), so the
// ratio shapes the lamps' part only: by day (no lamp light) it is exactly 1 and the attic is its room's top texel (16-17-55: the
// lamps' ratio alone took the daylight to near black at the top). The map itself holds the texel's light as bit 8 composes it
// (its room's matched top-row texel carried), so every other reader of the map (the game's own shader, the bloom pass, the
// class-2 wall blur, a draw whose rooms are not read) sees light, never a ratio
float IndoorRatio(float3 base, float3 acc, float2 bottom) {
    const float b = Lum3(base + tex2Dlod(sAcc, float4((bottom + 0.5) * gD.zw, 0, 0)).rgb), h = Lum3(base + acc);
    return b >= 1e-4 && h < b ? saturate(h / b) : 1.0;
}
// (bit 8) the light of a texel of a space a room of the story below continues into that the GPU lamps do not draw, as its room's
// kept solve gives it now (RtOutdoorLamps InBaseNow: a wall texel its matched top-row texel's, a floor texel its room's top-row
// mean; alpha 1 where written), else the vertex's base as the prepare carried it: a new kept solve of the room reaches the ratio
// and the floors in the frame it is kept, never a prepare later. gIB = 1 / width, 1 / height of that map, z 1 when bound
sampler2D sInBase : register(s10);
float4 gIB : register(c127);
float3 InBase(float3 b, float2 vpos, float flags) {
    if (flags < 8.0 || gIB.z < 0.5) return b;
    const float4 t = tex2Dlod(sInBase, float4((vpos + 0.5) * gIB.xy, 0, 0));
    return t.a > 0.5 ? t.rgb : b;
}
ComposeOut ComposePS(TexelIn i, float2 vpos : VPOS) {
    const float4 acc = tex2Dlod(sAcc, float4((vpos + 0.5) * gD.zw, 0, 0));
    const float3 base = InBase(i.base.rgb, vpos, i.p.w);
    const float3 S = base + acc.rgb;
    float alpha = i.n.w;
    const float flags = i.p.w;
    if (fmod(flags, 2.0) >= 1.0) {
        const float under = fmod(flags, 4.0) >= 2.0 ? i.dyn.y : -1.0;
        const float vis = i.mask.x + acc.a, all = i.mask.y + (gF.x > 0.5 ? MaskAll(i.p.xyz) : i.dyn.x);
        if (under >= 0.999) alpha = 1.0;
        else {
            float a = 0.0;
            if (all > 1e-4) a = clamp(1.0 - vis / all, 0.0, 1.0);
            if (under > a) a = under;
            alpha = a;
        }
    }
    // (outdoor_delta_model.h PackInput) the sum's hue kept past 1 (KeepHue), then cut at 1
    float3 s = max(float3(Fin(S.r), Fin(S.g), Fin(S.b)), 0.0);
    if (flags >= 8.0 && gF.z > 0.0) {
        const float4 in4 = IndoorOf(S, i.mask);
        s = max(in4.rgb, 0.0), alpha = in4.a;
    }
    const float sm = max(s.r, max(s.g, s.b));
    if (sm > 1.0) s /= sm;
    float4 c = float4(min(s, 1.0), min(max(Fin(alpha), 0.0), 1.0));
    ComposeOut o;
    o.c = Packed(c, i.base.w);
    o.k = ScaleOut(sm, flags);
    if (flags >= 16.0) o.k.z = IndoorRatio(base, acc.rgb, i.mask.xy);
    return o;
}

)RAW"
R"RAW(// ---------------------------------------------------------------- copies and the wall blur
float4 gM : register(c0); // 1 / width, 1 / height of the source
sampler2D sSrc : register(s0);
sampler2D sSrc2 : register(s1);

float4 Byte(float2 texel) { return floor(tex2Dlod(sSrc, float4((texel + 0.5) * gM.xy, 0, 0)) * 255.0 + 0.5); }
float4 Avg(float4 a, float4 b) { return floor((a + b + 1.0) * 0.5); }

float4 CopyPS(float2 uv : TEXCOORD0) : COLOR0 { return tex2Dlod(sSrc, float4(uv, 0, 0)); }

// texcoord0 = the pixel (x + 0.5, y + 0.5), texcoord1 = the block (x0, y0, x1, y1)
float4 BlurRowPS(float2 pix : TEXCOORD0, float4 rect : TEXCOORD1) : COLOR0 {
    const float2 p = floor(pix);
    const float2 a = float2(max(p.x - 1, rect.x), p.y), b = float2(min(p.x + 1, rect.z), p.y);
    return Avg(Byte(p), Avg(Byte(b), Byte(a))) / 255.0;
}
float4 BlurColPS(float2 pix : TEXCOORD0, float4 rect : TEXCOORD1) : COLOR0 {
    const float2 p = floor(pix);
    const float2 a = float2(p.x, max(p.y - 1, rect.y)), b = float2(p.x, min(p.y + 1, rect.w));
    return Avg(Avg(Byte(a), Byte(b)), Byte(p)) / 255.0;
}
float4 BlockPS(float2 pix : TEXCOORD0, float4 rect : TEXCOORD1) : COLOR0 {
    const float2 p = floor(pix);
    const float2 uv = (p + 0.5) * gM.xy;
    const bool edge = p.y <= rect.y + 0.5 || p.y >= rect.w - 0.5;
    return edge ? tex2Dlod(sSrc2, float4(uv, 0, 0)) : tex2Dlod(sSrc, float4(uv, 0, 0));
}
)RAW"
R"RAW(// ---------------------------------------------------------------- GPU lamps for every room (room_lamps_gpu.cpp)
// The texels of every room of a story map. p.w = bits + 16 x the texel's room slot (bits: 1 masked, 2 gated, 4 toned).
// The room table (A32B32G32R32F, 18 x rooms): columns 0..15 = the batch's lamps for that room (x = how often the room's light
// list holds the lamp x the bake's per-room scale, y = 1 when the realistic reach applies to the lamp in that room), column
// 16 = (normalisation +0x160, k [0x011D0BA0], 0, 0), column 17 = (ambient +0x110 rgb, 0). Toned texels (roofed rooms,
// gpu_lamp_law.h IndoorTexel): base = the solve's sum without the GPU lamps, n.w = the ambient weight's ramp, mask.x = the
// pass adds the ambient (0/1), dyn.xyz = the bounce light added to the sum. Raw texels: as ComposePS, "all" of the GPU
// lamps from dyn.x (the CPU) and the under-floor share from dyn.y.
sampler2D sTable : register(s2);
float4 gT : register(c102); // 1 / table width, 1 / table height
float4 TableAt(float col, float slot) { return tex2Dlod(sTable, float4((col + 0.5) * gT.x, (slot + 0.5) * gT.y, 0, 0)); }
float SlotOf(float pw) { return floor(pw / 16.0 + 1e-3); }
float BitsOf(float pw) { return pw - SlotOf(pw) * 16.0; }

float4 AccRlg(TexelIn i, float2 vpos, int kSearch, int kEmit, int kWalk) {
    float4 sum = gE.x > 0.5 ? float4(0, 0, 0, 0) : tex2Dlod(sAcc, float4((vpos + 0.5) * gD.zw, 0, 0));
    const float3 P = i.p.xyz;
    const float3 N = normalize(i.n.xyz);
    const float slot = SlotOf(i.p.w), bits = BitsOf(i.p.w);
    const bool masked = fmod(bits, 2.0) >= 1.0 && gE.y > 0.5;
    const float wallReach0 = gA.w > 0.5 ? Sat((0.5 - abs(N.y)) * 1e4) : 0.0;
    const int count = (int)gA.x;
    const float thr = gA.y;
    [loop] for (int l = 0; l < 16; l++) {
        const LampRegs lamp = ReadLamp(l);
        const float4 t = TableAt(l, slot);
        const float3 law = Law(lamp, P, N, wallReach0 * t.y);
        float3 wrapped = 0;
        if (masked) {
            const float3 toL = normalize(lamp.pos.xyz - P);
            wrapped = Law(lamp, P, toL, 0.0);
        }
        const bool maskOn = masked && thr <= (wrapped.b + wrapped.g) + wrapped.r;
        if (l < count && t.x != 0 && ((law.r + law.g + law.b) > 0 || maskOn)) { // (t.x < 0: a lamp taken out of a kept sum)
            const float sh = Shadow(lamp, P, N, kSearch, kEmit, kWalk, 0.0);
            float3 c = law * sh;
            const float cs = (c.b + c.g) + c.r;
            if (!(cs >= thr)) c = 0;
            else if (gA.z > 0.5 && cs < 2.0 * thr) {
                const float x = (cs - thr) / thr;
                c *= x * x * (3.0 - 2.0 * x);
            }
            sum.rgb += t.x * c;
            if (maskOn) sum.a += t.x * Lum3(wrapped) * sh;
        }
    }
    return sum;
}
float4 AccRlgPS(TexelIn i, float2 vpos : VPOS) : COLOR0 { return AccRlg(i, vpos, 24, 16, 16); }
float4 AccRlgFastPS(TexelIn i, float2 vpos : VPOS) : COLOR0 { return AccRlg(i, vpos, 8, 4, 8); }

// gpu_lamp_law.h Tone (the mean in the game's order; tanh(m / 2) / m as (2 / (e^-m + 1) - 1) / m)
float3 ToneRlg(float3 x) {
    const float m = (x.b * 0.33333334 + x.g * 0.33333334) + x.r * 0.33333334;
    if (!(m >= 0.01)) return x;
    return x * ((2.0 / (exp(-m) + 1.0) - 1.0) / m);
}
// gpu_lamp_law.h ToneSoft: the same curve with its step at mean 0.01 spread over 0.004 .. 0.02
float3 ToneSoft(float3 x) {
    const float m = max((x.b * 0.33333334 + x.g * 0.33333334) + x.r * 0.33333334, 1e-6);
    return x * lerp(1.0, (2.0 / (exp(-m) + 1.0) - 1.0) / m, smoothstep(0.004, 0.02, m));
}
float4 ComposeRlgPS(TexelIn i, float2 vpos : VPOS) : COLOR0 {
    const float4 acc = tex2Dlod(sAcc, float4((vpos + 0.5) * gD.zw, 0, 0));
    const float slot = SlotOf(i.p.w), bits = BitsOf(i.p.w);
    float4 c;
    if (bits >= 4.0) { // a roofed room's texel: toned as the solve tones it
        const float4 room = TableAt(16, slot);
        const float4 amb = TableAt(17, slot);
        const float3 S = i.base.rgb + acc.rgb + i.dyn.xyz;
        const float3 x = room.x * S;
        const float3 t = ToneRlg(x);
        const float w = 1.0 - Sat(room.y * ((t.b + t.g) + t.r));
        const float a = w * i.n.w;
        const float3 rgb = t + (a * i.mask.x) * amb.rgb;
        c = float4(min(max(Fin(rgb.r), 0.0), 1.0), min(max(Fin(rgb.g), 0.0), 1.0), min(max(Fin(rgb.b), 0.0), 1.0), min(max(Fin(a), 0.0), 1.0));
    } else {
        const float3 S = i.base.rgb + acc.rgb;
        float alpha = i.n.w;
        if (fmod(bits, 2.0) >= 1.0) {
            const float under = fmod(floor(bits / 2.0), 2.0) >= 1.0 ? i.dyn.y : -1.0;
            const float vis = i.mask.x + acc.a, all = i.mask.y + i.dyn.x;
            if (under >= 0.999) alpha = 1.0;
            else {
                float a = 0.0;
                if (all > 1e-4) a = clamp(1.0 - vis / all, 0.0, 1.0);
                if (under > a) a = under;
                alpha = a;
            }
        }
        float3 s = max(float3(Fin(S.r), Fin(S.g), Fin(S.b)), 0.0); // (as ComposePS: the hue kept past 1)
        const float sm = max(s.r, max(s.g, s.b));
        if (sm > 1.0) s /= sm;
        c = float4(min(s, 1.0), min(max(Fin(alpha), 0.0), 1.0));
    }
    if (i.base.w != 0) c.rgb = clamp(c.rgb + i.base.w, 0.0, 1.0);
    return float4(Rne(c.r * 255.0), Rne(c.g * 255.0), Rne(c.b * 255.0), Rne(c.a * 255.0)) / 255.0;
}
)RAW"
R"RAW(// ---------------------------------------------------------------- GPU room lamps (gpu_room_lamps.cpp): kept room solves
// Every operation mirrors rt_lamp_model.h KeptAdd / KeptTexel: keep them in step.
// AccKeptPS     one point per (kept texel, lamp) at the texel's place in the sum's layout (A32B32G32R32F, added: blend one,
//               one); the lamp in gPos[0] .. gSpill[0], gA.y the room's threshold, gA.z 1: the law is the
//               lamp's colour alone (gCol[0], a TubeLight without a segment; KeptLaw). p = (sample position, bits: 1 the early-out, 4 the soft light cut ran), n = (sample normal, mult), base = (A, B, thrScale, 0): mult x fade(law x A) x B
// ComposeKeptPS one point per kept texel at its place in the map (or in the wall layout): KeptTexel on base + the sum, packed
//               as the game's pack. p = (base rgb, dither), n = (ramp x ambient rgb, ramp; a roofless texel: its alpha),
//               base = (norm, k, the alpha floor, roofed 0/1), mask.xy = its place in the sum's layout (gD.zw: 1 / its size);
//               gAmb.rgb scales the ramp x ambient (RtLampModel::AmbScale: the room's ambient now over its solve's; 1 replica), gAmb.w 1:
//               ToneSoft (physical rooms); gBc.rgb the hue of the bounce through arches (the sum's alpha; added after the tone: over a
//               gradient the tone would draw a contour, and more light would show darker)
// CopyAtPS      one point per place: sSrc's texel at mask.xy (gM.xy: 1 / sSrc's size)
// BlockKeptPS   a blurred wall block from the layout into the map: tc0 = the layout pixel, tc1 = (its first and last rows in the
//               layout, rows beyond above 0/1, below 0/1): an edge row with nothing beyond it from the rows-only chain (sSrc2)
float4 gAmb : register(c108);
float4 gBc : register(c125);
float4 AccKeptPS(TexelIn i) : COLOR0 {
    const float3 law = KeptLaw(i.p.xyz, i.n.xyz);
    float3 c = law * i.base.x;
    const float thr = gA.y;
    const float cs = (c.b + c.g) + c.r;
    const bool early = fmod(i.p.w, 2.0) >= 1.0, fade = fmod(floor(i.p.w / 4.0), 2.0) >= 1.0;
    const float t = fade ? thr * i.base.z : thr;
    if ((early && thr > cs) || !(cs >= t)) c = 0;
    else if (fade && cs < 2.0 * t) {
        const float x = (cs - t) / t;
        c *= x * x * (3.0 - 2.0 * x);
    }
    return float4(i.n.w * c * i.base.y, 0);
}
float4 ComposeKeptPS(TexelIn i) : COLOR0 {
    const float4 acc = tex2Dlod(sAcc, float4((i.mask.xy + 0.5) * gD.zw, 0, 0));
    const float3 R = i.p.rgb + acc.rgb;
    float4 c;
    if (i.base.w > 0.5) {
        const float3 x = i.base.x * max(R, 0.0);
        const float3 t = gAmb.w > 0.5 ? ToneSoft(x) : ToneRlg(x);
        const float w = 1.0 - Sat(i.base.y * ((t.b + t.g) + t.r));
        const float3 rgb = t + w * (i.n.rgb * gAmb.rgb + i.n.w * gBc.rgb * acc.a);
        const float a = w * i.n.w;
        c = float4(saturate(Fin(rgb.r)), saturate(Fin(rgb.g)), saturate(Fin(rgb.b)), saturate(Fin(a)));
        if (c.a < i.base.z) c.a = i.base.z;
    } else {
        float3 s = max(float3(Fin(R.r), Fin(R.g), Fin(R.b)), 0.0);
        const float sm = max(s.r, max(s.g, s.b));
        if (sm > 1.0) s /= sm;
        c = float4(min(s, 1.0), min(max(Fin(i.n.w), 0.0), 1.0));
    }
    return Packed(c, i.p.w);
}
float4 CopyAtPS(TexelIn i) : COLOR0 { return tex2Dlod(sSrc, float4((i.mask.xy + 0.5) * gM.xy, 0, 0)); }
float4 BlockKeptPS(float2 pix : TEXCOORD0, float4 rect : TEXCOORD1) : COLOR0 {
    const float2 p = floor(pix);
    const float2 uv = (p + 0.5) * gM.xy;
    const bool edge = (p.y <= rect.x + 0.5 && rect.z < 0.5) || (p.y >= rect.y - 0.5 && rect.w < 0.5);
    return edge ? tex2Dlod(sSrc2, float4(uv, 0, 0)) : tex2Dlod(sSrc, float4(uv, 0, 0));
}
// GhostKeptPS   a row lit beyond a block's edge in the layout follows its edge texel (outdoor_delta_model.h FollowGhosts): sSrc
//               the layout with the walls composed, sSrc2 the layout as the solve left it, tc1.x = the edge row
float4 GhostKeptPS(float2 pix : TEXCOORD0, float4 rect : TEXCOORD1) : COLOR0 {
    const float2 p = floor(pix), e = float2(p.x, rect.x);
    const float4 then = floor(tex2Dlod(sSrc2, float4((e + 0.5) * gM.xy, 0, 0)) * 255.0 + 0.5);
    const float4 g = floor(tex2Dlod(sSrc2, float4((p + 0.5) * gM.xy, 0, 0)) * 255.0 + 0.5);
    return clamp(g + Byte(e) - then, 0.0, 255.0) / 255.0;
}
)RAW"
R"RAW(// ---------------------------------------------------------------- GPU room lamps: room 0's kept lamps (rt_outdoor_lamps.cpp)
// Real-time outdoor lamps' passes with room 0's kept lamps of the floor mask or exits (rt_lamp_model.h KeptWrapLum, ExitAlpha,
// Compose with gExit: keep them in step). Points as AccKeptPS's (one range per lamp); mask.x / mask.y = a floor mask part's
// walls-tested / no-walls luminance over its law's with N.l ignored (its A then 0), mask.z = 1 an exit lamp's part.
// AccKept0PS   into the sum: AccKeptPS's light, a = the walls-tested luminance (the floor mask's "vis")
// AccKept0XPS  into the second sum (cleared first): x = the no-walls luminance ("all"), y = an exit lamp's light's luminance
// ComposeK0PS  ComposePS with the second sum (sAcc2): "all" + its x, "vis" + its z (AccPhysXPS); an exit ground texel (mask.w = 1, mask.z = the exit lamps'
//              light left in the base) takes ExitAlpha(mask.z + its y) as its alpha
sampler2D sAcc2 : register(s3);
float3 KeptLight(TexelIn i) {
    const float3 law = KeptLaw(i.p.xyz, i.n.xyz);
    float3 c = law * i.base.x;
    const float thr = gA.y;
    const float cs = (c.b + c.g) + c.r;
    const bool early = fmod(i.p.w, 2.0) >= 1.0, fade = fmod(floor(i.p.w / 4.0), 2.0) >= 1.0;
    const float t = fade ? thr * i.base.z : thr;
    if ((early && thr > cs) || !(cs >= t)) c = 0;
    else if (fade && cs < 2.0 * t) {
        const float x = (cs - t) / t;
        c *= x * x * (3.0 - 2.0 * x);
    }
    return i.n.w * c * i.base.y;
}
float KeptWrapLum(TexelIn i) {
    const float3 toL = normalize(gPos[0].xyz - i.p.xyz);
    return Lum3(KeptLaw(i.p.xyz, toL));
}
float4 AccKept0PS(TexelIn i) : COLOR0 { return float4(KeptLight(i), i.mask.x != 0 ? KeptWrapLum(i) * i.mask.x : 0.0); }
float4 AccKept0XPS(TexelIn i) : COLOR0 { return float4(i.mask.y != 0 ? KeptWrapLum(i) * i.mask.y : 0.0, i.mask.z > 0.5 ? Lum3(KeptLight(i)) : 0.0, 0, 0); }
ComposeOut ComposeK0PS(TexelIn i, float2 vpos : VPOS) {
    const float4 acc = tex2Dlod(sAcc, float4((vpos + 0.5) * gD.zw, 0, 0));
    const float4 acc2 = tex2Dlod(sAcc2, float4((vpos + 0.5) * gD.zw, 0, 0));
    const float3 base = InBase(i.base.rgb, vpos, i.p.w);
    const float3 S = base + acc.rgb;
    float alpha = i.n.w;
    const float flags = i.p.w;
    if (fmod(flags, 2.0) >= 1.0) {
        const float under = fmod(flags, 4.0) >= 2.0 ? i.dyn.y : -1.0;
        const float vis = i.mask.x + acc.a + acc2.z, all = i.mask.y + (gF.x > 0.5 ? MaskAll(i.p.xyz) : i.dyn.x) + acc2.x;
        if (under >= 0.999) alpha = 1.0;
        else {
            float a = 0.0;
            if (all > 1e-4) a = clamp(1.0 - vis / all, 0.0, 1.0);
            if (under > a) a = under;
            alpha = a;
        }
    } else if (i.mask.w > 0.5 && flags < 8.0) { // (bit 8: mask.zw are its room's slot and height)
        const float E = i.mask.z + acc2.y;
        alpha = (E > 0 && E == E && E < 1e30) ? min(E, 254.0 / 255.0) : 0.0;
    }
    float3 s = max(float3(Fin(S.r), Fin(S.g), Fin(S.b)), 0.0);
    if (flags >= 8.0 && gF.z > 0.0) {
        const float4 in4 = IndoorOf(S, i.mask);
        s = max(in4.rgb, 0.0), alpha = in4.a;
    }
    const float sm = max(s.r, max(s.g, s.b));
    if (sm > 1.0) s /= sm;
    float4 c = float4(min(s, 1.0), min(max(Fin(alpha), 0.0), 1.0));
    ComposeOut o;
    o.c = Packed(c, i.base.w);
    o.k = ScaleOut(sm, flags);
    if (flags >= 16.0) o.k.z = IndoorRatio(base, acc.rgb, i.mask.xy);
    return o;
}
)RAW"
R"RAW(// ---------------------------------------------------------------- physical indoor lamps (rt_lamp_gpu.cpp RelightKeptPhys)
// The game's 3D (furniture) test, room_light_tests.h occlude / quadT / discT, over the blocker table (rt_lamp_blockers.h: point
// sampled A32B32G32R32F, row i at (i % width, i / width)): the same operations in the same order (sums (z + y) + x, the
// reciprocal with one Newton step, the 0.01 stop), with the constants the game holds (RoomLightTests K_* defaults).
sampler2D sBlk : register(s4);
float4 gK : register(c103); // the blocker table: width, 1 / width, 1 / height, 0
float4 BlkRow(float i) {
    const float y = floor((i + 0.5) * gK.y);
    return tex2Dlod(sBlk, float4((i - y * gK.x + 0.5) * gK.y, (y + 0.5) * gK.z, 0, 0));
}
float GDot(float3 a, float3 b) {
    const float3 p = a * b;
    return (p.z + p.y) + p.x;
}
// 0x0069B4C0: plane (A, N) against the segment B -> C (direction D, inv = 1 / D.N); o = X - A written as soon as t >= 0
bool SegPlane(float3 A, float3 N, float3 B, float3 C, float3 D, float inv, inout float3 o) {
    const float t = (0.0 - GDot(B - A, N)) * inv;
    const float3 X = D * t + B;
    const bool on = !(0.0 > t);
    if (on) o = X - A;
    return on && !(0.0 > GDot(C - X, D));
}
float SoftOp(float c, float h) { return saturate(1.0 - ((max(-c, c) - h) + 0.15) * 2.0); }
float SoftDisc(float3 v, float R) { return min(max(0.0, ((GDot(v, v) - R) + 0.0625) * 4.0), 1.0); }
// (e > 0: true-size edges, rt_lamp_blockers.h EdgeW / EdgeOp) the edge at the real extent, as wide as the lamp ball of radius e seen past it
float EdgeW(float3 X, float3 p1, float3 p2, float e) {
    const float3 d = p2 - p1, r = p2 - X;
    return max(e * saturate(sqrt(dot(r, r) / max(dot(d, d), 1e-8))), 1e-3);
}
float EdgeOp(float c, float h, float w) { return smoothstep(0.0, 1.0, (h - abs(c)) / (2.0 * w) + 0.5); }
// A table's leg (kind 2, rt_lamp_blockers.h PostT): the same operations
float PostT(float3 A, float3 B, float r, float3 p1, float3 p2) {
    float t = 1.0;
    const float hi = saturate((p1.y - max(A.y, B.y) - 1.5) / 1.5);
    [branch] if (hi < 1.0) {
        const float3 d1 = p2 - p1, d2 = B - A, o = p1 - A;
        const float a = GDot(d1, d1), e = GDot(d2, d2), f = GDot(d2, o), c = GDot(d1, o), b = GDot(d1, d2), den = a * e - b * b;
        float s = den > 1e-12 ? saturate((b * f - c * e) / den) : 0.0;
        const float u = (b * s + f) / max(e, 1e-12), v = saturate(u);
        if (v != u) s = saturate((b * v - c) / a);
        const float3 g = (p1 + d1 * s) - (A + d2 * v);
        const float d = sqrt(GDot(g, g)), w = max(max(gB.x * (1.0 - s), (0.075 + 0.125 * hi) * s), 1e-3);
        t = 1.0 - (1.0 - hi * hi * (3.0 - 2.0 * hi)) * (r >= w ? EdgeOp(d, r, w) : r / w * EdgeOp(d, w, w * 0.75));
    }
    return t;
}
// One blocker (its 7 rows from b) on the segment p1 -> p2: quadT (kind 0) or discT (kind 1), one plane or two (r1.w != 0), or a leg
// (kind 2). q2: the line's distance^2 from the record's centre; r6.w > 0: the game's sphere, which it must pass where posts enlarged the
// record's; f > 0: the band ends over that sphere's last 1 / f (rt_lamp_blockers.h CutFade)
float BlockerT(float b, float3 p1, float3 p2, float e, float q2, float f) {
    const float4 r2 = BlkRow(b + 2), r0 = BlkRow(b), r1 = BlkRow(b + 1), r6 = BlkRow(b + 6);
    const float3 d = p2 - p1;
    const float dn = GDot(r2.xyz, d);
    float t = 1.0;
    [branch] if (r0.w > 1.5) t = PostT(r0.xyz, r1.xyz, r2.w, p1, p2);
    else [branch] if (!(1e-4 > max(-dn, dn)) && !(r6.w > 0 && !(q2 - r6.w < 0))) {
        const float r = 1.0 / dn;
        const float inv = (2.0 - r * dn) * r;
        float3 v1 = BlkRow(b + 5).xyz, v2 = r6.xyz; // (the "miss" offsets a plane the segment never reaches leaves)
        const bool two = r1.w != 0;
        const bool h1 = SegPlane(r0.xyz, r2.xyz, p1, p2, d, inv, v1);
        bool h2 = false;
        if (two) h2 = SegPlane(r1.xyz, r2.xyz, p1, p2, d, inv, v2);
        [branch] if ((h1 || h2) && e > 0) {
            const float w1 = EdgeW(r0.xyz + v1, p1, p2, e), w2 = EdgeW(r1.xyz + v2, p1, p2, e);
            [branch] if (r0.w < 0.5) {
                const float4 r3 = BlkRow(b + 3), r4 = BlkRow(b + 4);
                t = 1.0 - EdgeOp(GDot(r4.xyz, v1), r3.w, w1) * EdgeOp(GDot(r3.xyz, v1), r2.w, w1);
                if (two) t = (1.0 - EdgeOp(GDot(r4.xyz, v2), BlkRow(b + 5).w, w2) * EdgeOp(GDot(r3.xyz, v2), r4.w, w2)) * t;
            } else {
                const float rd = sqrt(max(r2.w, 0.0));
                t = 1.0 - EdgeOp(sqrt(GDot(v1, v1)), rd, w1);
                if (two) t = (1.0 - EdgeOp(sqrt(GDot(v2, v2)), rd, w2)) * t;
            }
        } else [branch] if (h1 || h2) {
            [branch] if (r0.w < 0.5) {
                const float4 r3 = BlkRow(b + 3), r4 = BlkRow(b + 4);
                t = 1.0 - SoftOp(GDot(r4.xyz, v1), r3.w) * SoftOp(GDot(r3.xyz, v1), r2.w);
                if (two) t = (1.0 - SoftOp(GDot(r4.xyz, v2), BlkRow(b + 5).w) * SoftOp(GDot(r3.xyz, v2), r4.w)) * t;
            } else
                t = two ? SoftDisc(v2, r2.w) * SoftDisc(v1, r2.w) : SoftDisc(v1, r2.w);
            if (f > 0) {
                const float c = saturate((sqrt(r6.w) - sqrt(q2)) * f);
                t = 1.0 - (1.0 - t) * (c * c * (3.0 - 2.0 * c));
            }
        }
    }
    return t;
}
// 0x0069D910: the product of the transmissions of the blockers of every record whose sphere the line a -> b passes, from the
// room's header row h. Records and their blockers are walked as one sequence: a ps_3_0 loop runs at most 255 times, so two
// nested loops give 65025 steps (a record or a blocker each)
float Occlude3D(float3 a, float3 b, float h, float e) {
    const float4 head = BlkRow(h);
    const float len2 = GDot(b - a, b - a);
    float T = 1.0, rec = 0.0, blk = 0.0, nb = -1.0, first = 0.0, q2 = 0.0;
    float3 sh = float3(0, 0, 0); // (the record's offset: a table dragged with its lamp ahead of its solve, BlockerTable::Offset)
    bool done = !(head.y > 0.5);
    [loop] for (int o = 0; o < 255; o++) {
        if (done) break;
        [loop] for (int k = 0; k < 255; k++) {
            if (done) break;
            if (nb < 0) { // a record: the broad phase (the infinite line against its sphere)
                const float row = head.x + 2.0 * rec;
                const float4 c = BlkRow(row);
                const float3 ca = c.xyz - a, d = b - a;
                const float s = GDot(ca, d);
                const float3 q = s * (d / len2) - ca;
                q2 = GDot(q, q);
                if (q2 - c.w < 0) {
                    const float4 ix = BlkRow(row + 1);
                    first = ix.x;
                    nb = ix.y;
                    blk = 0;
                    sh = float3(ix.z, 0, ix.w);
                }
                if (!(nb > 0.5)) {
                    rec += 1;
                    nb = -1;
                }
            } else {
                T = T * BlockerT(first + blk * 7.0, a - sh, b - sh, e, q2, head.w);
                blk += 1;
                if (0.01 >= T) done = true;
                if (blk >= nb) {
                    rec += 1;
                    nb = -1;
                }
            }
            if (rec >= head.y) done = true;
        }
    }
    return T;
}
)RAW"
R"RAW(// A lamp's law of any kind (an area light's, a tube's or the GPU's)
float3 LawAt(LampRegs L, float3 p, float3 n, float reach) {
    float3 c;
    [branch] if (L.off.y < -1.5) c = AreaLaw(L, p);
    else [branch] if (L.off.y < -0.5) c = TubeLaw(L, p, n);
    else c = Law(L, p, n, reach);
    return c;
}
// T3dPS: the 3D test of the batch's lamps 0 .. 3 (gA.x of them; 1 past them) from the lamp (+0x120: gW3; w > 0: a vertical tube, the
// mean of 4 points that far apart, GpuLampLaw::TubeStep) to each phys point
// (base.xyz), both in world space as the game's records, as AccPhysPS takes it (a pass of its own: inside AccPhysPS's lamp
// loop the walk exceeds ps_3_0's temporary registers)
float4 gW3[4] : register(c104);
// One lamp's 3D test (L: its gW3 row; fill: it stands on furniture) from the record row `head` to b
float T3dLamp(float4 L, bool fill, float head, float3 b) {
    float t = 1.0;
    // (a lamp standing on furniture, its fill: true-size edges. The game's band made a 0.36 m table's floor shadow a blob with a
    // crease at 1.3 m, 05/11; every other lamp keeps the band, the soft furniture shadows of ceiling lamps)
    // (rt_lamp_blockers.h OccludeFill: the light the floor bounces under the table, more towards the shadow's edge than at its
    // middle. One call in a loop: two inlined exceed ps_3_0's temporary registers)
    const float rise = L.w;
    float a = 0.0;
    [loop] for (int q = 0; q < 4; q++) {
        [branch] if (q == 0 || rise > 0 || (q == 1 && fill && t < 1.0)) {
            const float o = Occlude3D(float3(L.x, L.y + rise * q, L.z), b, head, q == 1 && rise == 0 ? gB.x * 4.0 : (fill ? gB.x : gK.w));
            a = q == 0 ? o : a + (rise > 0 ? o : (1.0 - a) * 0.2 * o);
            t = a;
        }
    }
    return rise > 0 ? a * 0.25 : a;
}
float4 T3dPS(TexelIn i) : COLOR0 {
    const float head = TableAt(16, SlotOf(i.p.w)).x;
    const bool tested = BitsOf(i.p.w) >= 8.0 && head >= 0;
    float4 r = 1.0;
    [loop] for (int j = 0; j < 4; j++) {
        float t = 1.0;
        [branch] if (tested && j < (int)gA.x) t = T3dLamp(gW3[j], TableAt(j, SlotOf(i.p.w)).w > 0, head, i.base.xyz);
        r = float4(r.yzw, t); // (no indexable temporaries: after the 4 turns r = lamps 0 .. 3)
    }
    return r;
}
// A kept map's phys points as textures in the sum's layout, p, n, mask, base as TexelIn, drawn as dense quads (a point fills a 2 x 2
// pixel quad of the GPU with one live pixel; the same pixels, bit for bit, rt_lamps_test)
float4 gQ : register(c109);  // 1 / the textures' width, 1 / height, T3dDensePS's lamp, 0
float4 gQ3 : register(c110); // its gW3 row
float4 gQ4 : register(c126); // its lot position
float4 gGl : register(c111); // the lot's glazed spans in the blocker table (2 rows each: x0, z0, x1, z1; y0, y1): first row, count, 0, log2 of a pane's pass
sampler2D sPtP : register(s6);
sampler2D sPtN : register(s7);
sampler2D sPtM : register(s8);
sampler2D sPtB : register(s9);
// T3dPS over those textures, one lamp a pass (gQ.z) into its channel (the colour write mask): inside T3dPS's lamp loop the fetched
// position exceeds ps_3_0's temporary registers. Only where AccPhys will read it: the same lamp row and law test, thr a shade lower
// so that another compile's rounding of the law cannot leave out a texel AccPhys takes (a texel left out reads 1, never used).
// Written as t + 2 x the panes the segment crosses (AccPhys takes the pane share after the soft cut; rt_lamp_proxy.h PaneCount), on an
// untested texel too. A lamp of another room (the table's z = 2) reads the second header: the joined rooms' records too (BlockerTable::Join)
float PaneCount(float3 a, float3 b) {
    float n = 0, i = 0;
    const float2 d = b.xz - a.xz;
    [loop] for (int g = 0; g < 255; g++) { // (a ps_3_0 loop runs at most 255 times, as Occlude3D: a big lot has more glazed spans)
        if (i >= gGl.y) break;
        [loop] for (int k = 0; k < 255; k++) {
            if (i >= gGl.y) break;
            const float4 r = BlkRow(gGl.x + 2.0 * i), h = BlkRow(gGl.x + 2.0 * i + 1.0);
            const float2 e = r.zw - r.xy, o = r.xy - a.xz;
            const float den = d.x * e.y - d.y * e.x;
            const float t = (o.x * e.y - o.y * e.x) / den, u = (o.x * d.y - o.y * d.x) / den;
            const float y = a.y + t * (b.y - a.y);
            if (t > 0 && t < 1 && u >= 0 && u < 1 && y >= h.x && y <= h.y) n += 1;
            i += 1;
        }
    }
    return n;
}
float4 T3dDensePS(float2 vpos : VPOS) : COLOR0 {
    const float4 uv = float4((vpos + 0.5) * gQ.xy, 0, 0);
    const float4 p = tex2Dlod(sPtP, uv);
    const float slot = SlotOf(p.w), head = TableAt(16, slot).x;
    bool used = false, fill = false;
    float cross = 0;
    [branch] if ((BitsOf(p.w) >= 8.0 && head >= 0) || gGl.y > 0.5) {
        const float3 N = normalize(tex2Dlod(sPtN, uv).xyz);
        const float wallReach0 = gA.w > 0.5 ? Sat((0.5 - abs(N.y)) * 1e4) : 0.0;
        [loop] for (int l = 0; l < 4; l++) { // (the lamp's constants take the loop's index)
            [branch] if (l == (int)gQ.z) {
                const float4 r = TableAt(l, slot);
                const float3 law = LawAt(ReadLamp(l), p.xyz, N, wallReach0 * r.y);
                used = r.x != 0 && (law.b + law.g) + law.r >= gA.y * 0.99, fill = r.w > 0, cross = r.z > 1.5 ? 1.0 : 0.0;
            }
        }
    }
    float t = 1.0;
    [branch] if (used && BitsOf(p.w) >= 8.0 && head >= 0) t = T3dLamp(gQ3, fill, head + cross, tex2Dlod(sPtB, uv).xyz);
    [branch] if (used) t += 2.0 * PaneCount(gQ4.xyz, p.xyz);
    return t;
}
sampler2D sT3 : register(s5);
// A tested texel's 3D tests of the batch's lamps 0 .. 3 (sT3) lifted by their furniture fills (the room table's columns' w):
// RtLampGpu::FurnitureFill, taken before the lamp loop (inside it a live fill exceeds ps_3_0's temporary registers). The panes
// T3dDensePS packed over t stay on top (2 n; values under 2 hold none)
float4 FillT3(float2 vpos, float slot) {
    const float4 f = float4(TableAt(0, slot).w, TableAt(1, slot).w, TableAt(2, slot).w, TableAt(3, slot).w);
    const float4 v = tex2Dlod(sT3, float4((vpos + 0.5) * gD.zw, 0, 0)), n2 = 2.0 * floor(v * 0.5);
    return n2 + f + (1.0 - f) * (v - n2);
}
// The light a lit room sends through its arches (a kept map's first batch, into the sum's alpha; arch_ambient.h BounceAt): each opening
// (gBo[2k] = a.xz, b.xz; gBo[2k + 1] = y0, y1, weight, 1 / reach), its exact form factor from the texel, on a floor x 1 - ArchAmbient::Weight (the
// arch's own term owns its reach at the line). shortcut: abs() and no occlusion, back-facing walls in an L-shaped room take a little
// light and the bounce passes the inner corner; upgrade by orienting the rows
float4 gBo[12] : register(c112);
float4 gBn : register(c124); // the openings
float BounceEdge(float3 a, float3 b, float3 n) {
    const float3 c = cross(a, b);
    return acos(clamp(dot(a, b), -1.0, 1.0)) * dot(c, n) * rsqrt(max(dot(c, c), 1e-12));
}
float Bounce(float3 P, float3 N) {
    float s = 0;
    [loop] for (int k = 0; k < (int)gBn.x; k++) {
        const float4 r = gBo[2 * k], h = gBo[2 * k + 1];
        const float3 v0 = normalize(float3(r.x, h.x, r.y) - P), v1 = normalize(float3(r.z, h.x, r.w) - P), v2 = normalize(float3(r.z, h.y, r.w) - P), v3 = normalize(float3(r.x, h.y, r.y) - P);
        const float2 e = r.zw - r.xy, q = P.xz - r.xy;
        const float sv = saturate(length(q - e * saturate(dot(q, e) / dot(e, e))) * h.w);
        const float own = N.y > 0.5 ? sv * sv * (3.0 - 2.0 * sv) : 1.0;
        s += h.z * own * abs(BounceEdge(v0, v1, N) + BounceEdge(v1, v2, N) + BounceEdge(v2, v3, N) + BounceEdge(v3, v0, N)) * 0.15915494;
    }
    return s;
}
// AccPhysPS: one point per kept texel at its place in the sum's layout: p = (lot position, bits + 16 x its room slot; bit 8:
// the texel's batch ran the game's 3D test), n = the lot normal, base.xyz = its world position (T3dPS). Each lamp of the batch (at most 4; the room table's column l:
// x = mult, 0 not lit there; y = 1 the reach share applies; w = the furniture fill's floor) adds mult x fade(law x share), share =
// the cube shadow share (Shadow) and, on a tested texel, x its 3D test (T3dPS's output for the batch: sT3) lifted to at least w
// (RtLampGpu::FurnitureFill), the product dropped under 0.01 as the game drops it
float4 AccPhys(TexelIn i, float2 vpos, int kSearch, int kEmit, int kWalk, bool atMask) {
    float4 sum = gE.x > 0.5 ? float4(0, 0, 0, 0) : tex2Dlod(sAcc, float4((vpos + 0.5) * gD.zw, 0, 0));
    const float3 P = i.p.xyz;
    const float3 N = normalize(i.n.xyz);
    const float slot = SlotOf(i.p.w);
    const bool tested = BitsOf(i.p.w) >= 8.0;
    if (atMask && gE.x > 0.5) sum.a = Bounce(P, N);
    float4 T3 = atMask || tested ? FillT3(vpos, slot) : 1.0; // (lamp l in T3.x at its turn; a kept map's texel holds its panes, tested or not)
    const float wallReach0 = gA.w > 0.5 ? Sat((0.5 - abs(N.y)) * 1e4) : 0.0;
    const int count = (int)gA.x;
    const float thr = gA.y;
    [loop] for (int l = 0; l < 4; l++) {
        const LampRegs lamp = ReadLamp(l);
        const float pn = atMask ? floor(T3.x * 0.5) : 0.0, t3 = T3.x - 2.0 * pn;
        T3 = T3.yzwx;
        const float4 t = TableAt(l, slot);
        const float3 law = LawAt(lamp, P, N, wallReach0 * t.y);
        [branch] if (l < count && t.x != 0 && (law.b + law.g) + law.r >= thr) { // (share <= 1: under thr the lamp adds nothing, no Shadow to pay for)
            float sh = Shadow(lamp, atMask ? i.mask.xyz : P, N, kSearch, kEmit, kWalk, i.mask.w); // (gF.y: a room's points, RtLampProxy::IntoRoom)
            if (tested) {
                const float prod = sh * t3; // (t3 with its furniture fill: FillT3)
                sh = 0.01 > prod ? 0.0 : prod;
            }
            float3 c = law * sh;
            const float cs = (c.b + c.g) + c.r;
            if (!(cs >= thr)) c = 0;
            else if (gA.z > 0.5 && cs < 2.0 * thr) {
                const float x = (cs - thr) / thr;
                c *= x * x * (3.0 - 2.0 * x);
            }
            sum.rgb += t.x * c * exp2(pn * gGl.w); // (the glass after the soft cut: before it the faint light past two panes went black)
        }
    }
    return sum;
}
float4 AccPhysPS(TexelIn i, float2 vpos : VPOS) : COLOR0 { return AccPhys(i, vpos, 24, 16, 16, false); }
float4 AccPhysFastPS(TexelIn i, float2 vpos : VPOS) : COLOR0 { return AccPhys(i, vpos, 8, 4, 8, false); }
// AccPhysInPS: a kept map's phys points (the textures above), the shadow looked up at mask.xyz (RtLampProxy::IntoRoom: a texel past its
// room's diagonal wall taken inside the room)
TexelIn PointAt(float2 vpos) {
    const float4 uv = float4((vpos + 0.5) * gQ.xy, 0, 0);
    TexelIn i;
    i.p = tex2Dlod(sPtP, uv), i.n = tex2Dlod(sPtN, uv), i.mask = tex2Dlod(sPtM, uv), i.base = 0, i.dyn = 0;
    return i;
}
float4 AccPhysInPS(float2 vpos : VPOS) : COLOR0 { return AccPhys(PointAt(vpos), vpos, 24, 16, 16, true); }
float4 AccPhysInFastPS(float2 vpos : VPOS) : COLOR0 { return AccPhys(PointAt(vpos), vpos, 8, 4, 8, true); }
// AccPhysXPS (room 0, Real-time outdoor lamps' map): the batch's lamps at the same points into the second sum (added: blend one,
// one), as AccPhysPS takes them (the share x the 3D test, the 0.01 drop): on a masked texel (bit 1) the floor mask's luminances
// with N.l ignored (rt_lamp_model.h AddMask: x = "all", z = "vis" with the share), on an exit ground texel (bit 4) an exit
// lamp's light (the table's z = 1: y = its luminance, as ComposeK0PS's E). One law and one shadow per lamp
float4 AccPhysXPS(TexelIn i, float2 vpos : VPOS) : COLOR0 {
    const float3 P = i.p.xyz;
    const float3 N = normalize(i.n.xyz);
    const float slot = SlotOf(i.p.w), bits = BitsOf(i.p.w);
    const bool masked = fmod(bits, 2.0) >= 1.0 && gE.y > 0.5;
    const bool exitG = !masked && fmod(floor(bits / 4.0), 2.0) >= 1.0;
    const bool tested = bits >= 8.0;
    float4 T3 = tested ? FillT3(vpos, slot) : 1.0;
    const float wallReach0 = gA.w > 0.5 ? Sat((0.5 - abs(N.y)) * 1e4) : 0.0;
    const int count = (int)gA.x;
    const float thr = gA.y;
    float4 sum = 0;
    [loop] for (int l = 0; l < 4; l++) {
        const LampRegs lamp = ReadLamp(l);
        const float t3 = T3.x;
        T3 = T3.yzwx;
        const float4 t = TableAt(l, slot);
        const float3 law = LawAt(lamp, P, masked ? normalize(lamp.pos.xyz - P) : N, masked ? 0.0 : wallReach0 * t.y);
        const float ls = (law.b + law.g) + law.r;
        [branch] if (l < count && t.x != 0 && (masked ? thr <= ls : exitG && t.z > 0.5 && ls > 0)) {
            float sh = Shadow(lamp, P, N, 24, 16, 16, i.mask.w); // (mask.w: room 0's sharper floor sub-texel footprint, else 0)
            if (tested) {
                const float prod = sh * t3; // (t3 with its furniture fill: FillT3)
                sh = 0.01 > prod ? 0.0 : prod;
            }
            if (masked) {
                const float lum = t.x * Lum3(law);
                sum.x += lum;
                sum.z += lum * sh;
            } else {
                float3 c = law * sh;
                const float cs = (c.b + c.g) + c.r;
                if (!(cs >= thr)) c = 0;
                else if (gA.z > 0.5 && cs < 2.0 * thr) {
                    const float x = (cs - thr) / thr;
                    c *= x * x * (3.0 - 2.0 * x);
                }
                sum.y += Lum3(t.x * c);
            }
        }
    }
    return sum;
}
)RAW";
