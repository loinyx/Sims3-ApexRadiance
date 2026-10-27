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
//                       game's pack (round to nearest even of x 255)
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
    float3 r;
    if (a.x >= a.y && a.x >= a.z) {
        f = d.x > 0 ? 0 : 1;
        r = d.x > 0 ? float3(-d.z, d.y, d.x) : float3(d.z, d.y, -d.x);
    } else if (a.y >= a.z) {
        f = d.y > 0 ? 2 : 3;
        r = d.y > 0 ? float3(d.x, -d.z, d.y) : float3(d.x, d.z, -d.y);
    } else {
        f = d.z > 0 ? 4 : 5;
        r = d.z > 0 ? float3(d.x, d.y, d.z) : float3(-d.x, d.y, -d.z);
    }
    return r;
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
// (off1, sc2, off2, r0^2) (spill, 1 / R^2) (tile, mult, 0, 0)
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
float4 gE : register(c100); // first batch (1: no sum before), mask (0/1), nearest blocker (m), debug lamp (-1 none)
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
    // (gpu_lamp_law.h MountedNdl) a lamp within 0.4 m in front of a wall lights that wall at N.l 0.5 x (1 - plane / 0.4) at least
    const float mount = 0.5 * Sat((0.5 - abs(n.y)) * 1e4) * Sat(plane * 1e4) * Sat(1.0 - plane * 2.5);
    const float ndl = max(Sat(dot(n, l)), mount);
    const float f = min(ndl * (inv * inv) * L.pos.w, L.col.w);
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

// The atlas along direction d from lamp tile k (m): x = the nearest surface's distance, y = how far its plane passes from the
// lamp; dc = the direction of that texel's centre (the receiver's own depth is taken along it, so a surface and its own
// shadow texel agree to rounding)
float2 Stored(float k, float3 d, out float3 dc) {
    float f;
    const float3 v = Face(d, f);
    const float2 uv = float2(v.x / v.z * 0.5 + 0.5, -v.y / v.z * 0.5 + 0.5);
    const float F = gB.y;
    const float2 t = clamp(floor(uv * F + 0.5), 0, F - 1);
    const float2 texel = float2(f * F, k * F) + t;
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

// The share of the lamp's ball the point sees: PCSS over the cube map. Blockers are searched in the cone through which the
// point sees the ball (a texel at angle a from the point's direction holding a surface at distance z blocks only when
// a <= R (dr - z) / (dr z), the cone's half angle at z); the penumbra from their mean distance; then the share of taps in
// that disc that see past every surface (at least the map's half texel and one shadow texel wide). The receiver's depth at
// each tap is its own plane's along the tap's texel centre, so a receiver is never shadowed by its own surface. The emitter is
// the part of the ball its centre sees (rt_lamp_proxy.h Visibility): a surface whose plane passes through the ball (a ceiling
// 5 cm over a hanging lamp, the wall a sconce is mounted on) bounds the light and shades nothing on the lamp's side of it;
// a point behind it (its centre ray blocked by such a surface) sees none of the lamp.
// kSearch, kEmit, kWalk: 24, 16, 16 (AccPS); 8, 4, 8 while a lamp is dragged (AccFastPS, then AccPS again on release)
float Shadow(LampRegs lamp, float3 P, float3 N, int kSearch, int kEmit, int kWalk) {
    const float k = lamp.misc.x;
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
    const float dmin = max(min(gE.z, dr * 0.5), nearM);
    const float searchA = min(R * max(dr - dmin, 0.0) / (dr * dmin), 0.6) + texelA;
    float bSum = 0, bN = 0;
    [loop] for (int s = 0; s <= kSearch; s++) {
        const float r = s == kSearch ? 0.0 : sqrt((s + 0.5) / kSearch) * searchA; // (the last: the centre)
        const float a = s * 2.39996323;
        const float3 d = normalize(dir + (t1 * cos(a) + t2 * sin(a)) * r);
        const float2 sp = Stored(k, d, dc);
        const float st = sp.x;
        if (st >= nearM && sp.y >= R && st < Recv(N, dc, planeD, dr) - bias && r <= R * max(dr - st, 0.0) / (dr * max(st, 1e-3)) + 2.0 * texelA) {
            bSum += st;
            bN += 1;
        }
    }
    // Where something may shade the point, the share is measured as the reference measures it: kEmit points of the ball's
    // disc across the ray (equal-area rings, golden-angle turns); a point the lamp's centre does not see (behind a surface
    // within the ball) gives no light; for each other one the segment to the point is walked through the shadow map in
    // equal angular steps (1 / z evenly): it is blocked where its point lies behind the nearest surface seen from the lamp.
    float share = 1.0;
    if (bN > 0.5) {
        float emit = 0, seen = 0;
        const float zEnd = dr - max(2.0 * texelW, 0.05); // (the point's own surface is not walked into)
        [loop] for (int j = 0; j < kEmit; j++) {
            const float rho = R * sqrt((j + 0.5) / kEmit);
            const float a = j * 2.39996323;
            const float3 o = (t1 * cos(a) + t2 * sin(a)) * rho;
            float3 dq;
            const float2 sq = Stored(k, o, dq);
            const bool valid = sq.x >= rho; // the lamp's centre sees this point of its ball
            // the walk: the lamp's half with 1 / z evenly spaced (the direction turns evenly there), the point's half evenly in
            // z up to zEnd (a surface just in front of the point)
            const float zHalf = max(dr * 0.5, nearM + 1e-3);
            const float u0 = 1.0 / max(nearM, 1e-3), u1 = 1.0 / zHalf;
            float blocked = 0;
            [loop] for (int m = 0; m < kWalk; m++) {
                const float z = m < kWalk / 2 ? 1.0 / lerp(u0, u1, (m + 0.5) / (kWalk / 2)) : lerp(zHalf, max(zEnd, zHalf), (m - kWalk / 2 + 1.0) / (kWalk / 2));
                const float3 X = dir * z + o * (1.0 - z / dr); // the segment's point, from the lamp
                const float dX = length(X);
                const float2 sx = Stored(k, X, dq);
                blocked += (sx.y >= R && sx.x >= nearM && sx.x < dX - bias) ? 1.0 : 0.0;
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
    const bool ground = i.p.w >= 4.0;
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
        if (l < count && ((law.r + law.g + law.b) > 0 || maskOn)) {
            const float sh = ground ? 1.0 : Shadow(lamp, P, N, kSearch, kEmit, kWalk);
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
float4 gF : register(c101);
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
// 8 bits (1 to 8 times, 0.8 % a level), 0 within 1. A blurred atlas's goes through a scratch map (RtLampGpu::ComposeImpl)
struct ComposeOut {
    float4 c : COLOR0;
    float4 k : COLOR1;
};
ComposeOut ComposePS(TexelIn i, float2 vpos : VPOS) {
    const float4 acc = tex2Dlod(sAcc, float4((vpos + 0.5) * gD.zw, 0, 0));
    const float3 S = i.base.rgb + acc.rgb;
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
    const float sm = max(s.r, max(s.g, s.b));
    if (sm > 1.0) s /= sm;
    float4 c = float4(min(s, 1.0), min(max(Fin(alpha), 0.0), 1.0));
    if (i.base.w != 0) c.rgb = clamp(c.rgb + i.base.w, 0.0, 1.0);
    ComposeOut o;
    o.c = float4(Rne(c.r * 255.0), Rne(c.g * 255.0), Rne(c.b * 255.0), Rne(c.a * 255.0)) / 255.0;
    o.k = (sm > 1.0 ? Rne(saturate(log2(sm) / 3.0) * 255.0) / 255.0 : 0.0).xxxx;
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
            const float sh = Shadow(lamp, P, N, kSearch, kEmit, kWalk);
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
)RAW";
