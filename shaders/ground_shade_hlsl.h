// Walls shade lamp light near the ground on the GPU (features/ground_shade_gpu.cpp): the lot shade of
// features/ground_lamp_shadow.h (GroundShadow::ShadeJob) and the chunk shade over the world grass (GroundShadow::ChunkFull and
// ChunkUpdate) as ps_3_0 passes, function by function with the same float math. Inputs in float textures read with tex2Dlod
// (point, clamp); the per-lamp setup (reach, stamp height, boxes) is the CPU's (GroundShadeGpu::Prepare). Compiled by Apex's
// shader cache (ShadePS without optimisation: every fxc optimisation level runs it past ps_3_0's 32 temporaries); the offline
// harness (tools/ground_shade_gpu_test) compiles the same string. (Several raw literals: MSVC takes at most 16 KB in one.)
//
// ShadePS      per grid texel: M before the blurs (Finish's first line) and the lamps' stamp weight (r, g)
// BlurHPS/VPS  Finish's [1 2 1] along rows / columns, clamped at the grid's edges
// BytePS       the last column blur straight to ToByte's value (0..255) and the stamp weight: the lot's ext texture (r, g)
// MinPS        the lowest byte of 4 x 4 source texels (the lot's ext: its lowest byte at texel (w, 0))
// MaskPS       the lot mask: rgb the room mask, a the shade inside the lot (255 inside rooms), as MakeMaskTexture
// ChunkPS      a chunk texel's max lost / max seen over up to 6 lots (ChunkTexels), after the lots of the passes before
// ChunkBytePS  1 - lost / seen as a byte (255 where no lot is), in every channel
// MipPS        a mip texel: the rounded mean of the four under it ((a + b + c + d + 2) / 4 in bytes)
#pragma once
static const char* kGroundShadeHlsl = R"RAW(
float4 g_grid0 : register(c0);  // m0, m2, m8, m10
float4 g_grid1 : register(c1);  // tx, tz, ty, soft
float4 g_count : register(c2);  // tested lamps, other lamps, penumbra radius (< 0: penumbra off), Canopy::Extinction()
float4 g_inv0 : register(c3);   // 1 / lamp texture w, h; 1 / entry texture w, h
float4 g_inv1 : register(c4);   // 1 / wall texture w, h; 1 / patch texture w, h
float4 g_inv2 : register(c5);   // 1 / crown texture w, h; 1 / source w, h
float4 g_size : register(c6);   // used w, h of the source; ox, oz (MaskPS: the lot on the ext grid; MinPS: z the output's first column, w 1 = clip at 255)
float4 g_inv3 : register(c7);   // 1 / shade target w, h; 1 / room texture w, h
float4 g_chunk : register(c8);  // chunk corner x, z; lots in this pass; 1 = read the passes before
float4 g_chunk2 : register(c9); // 1 / scratch w, h; mip source lod; 0
float4 g_lot[24] : register(c16); // per lot of the pass, 4: (m0 m2 m8 m10) (tx tz W H) (i0 i1 j0 j1) (1 / texture w, h, 0, 0)

sampler2D sLamps : register(s0);  // 5 per lamp row: (x y z k) (rx0 rz0 rx1 rz1) (h reach walls crowns) (box i0 i1 j0 j1) (1 = a lamp's first row)
sampler2D sEnt : register(s1);    // per tested lamp row, 2 per entry: (index) (box i0 i1 j0 j1); walls first, then crowns (a lamp with
                                  // over 255 of either takes more rows, in the CPU's order)
sampler2D sWalls : register(s2);  // 3 per wall: (x0 z0 x1 z1) (y0 y1 p0 pn) (free0 free1 pass 0); walls, patches, crowns: AtW
sampler2D sPatch : register(s3);  // 4 per patch: (u0 u1 top0 top1) (ou0 ou1 sill glass) (head0..3) (head4 open floorSill 0)
sampler2D sCrown : register(s4);  // 3 per crown: (cx cy cz lad) (rx ry rz 0) (cos sin 0 0)
sampler2D sSrc : register(s5);    // blur, byte, min source (r); MaskPS: the lot's ext
sampler2D sShade : register(s6);  // BytePS: the shade target (g: stamp weight)
sampler2D sRoom : register(s7);   // MaskPS: the room mask (L8)
sampler2D sPrev : register(s8);   // ChunkPS / ChunkBytePS: the passes before (lost, seen)
sampler2D sMip : register(s9);    // MipPS: the chunk texture, read at level g_chunk2.z
sampler2D sLot0 : register(s10);  // ChunkPS: the lots' ext textures
sampler2D sLot1 : register(s11);
sampler2D sLot2 : register(s12);
sampler2D sLot3 : register(s13);
sampler2D sLot4 : register(s14);
sampler2D sLot5 : register(s15);

static const float kLampSkip = 0.15, kMinWeight = 1.0 / 256.0, kOtherLight = 0.05, kSoftAA = 0.75 / 4.0;
static const float kMinHalf = 0.02, kMaxHalf = 1.0, kEdge = 0.02, kHalfThick = 0.05, kFaceShiftMax = 0.5;
#define MAX_PATCHES 32

float4 At(sampler2D s, float col, float row, float2 inv) { return tex2Dlod(s, float4((col + 0.5) * inv.x, (row + 0.5) * inv.y, 0, 0)); }
// Item idx of a list kept kWrap rows high, cols texels an item, in column blocks side by side (GroundShadeGpu::kWrapRows)
static const float kWrap = 4096.0;
float4 AtW(sampler2D s, float col, float idx, float cols, float2 inv) {
    float blk = floor(idx / kWrap);
    return At(s, col + cols * blk, idx - kWrap * blk, inv);
}
float2 Texel(float2 vpos) { return floor(vpos + 0.25); }

// ---- penumbra.h
float Cover(float x, float h) {
    float t = saturate(x / (2.0 * max(h, 1e-6)) + 0.5);
    return t * t * (3.0 - 2.0 * t);
}
float PWeight(float c, float h, float a, float b) { return Cover(c - a, h) - Cover(c - b, h); }
float Half(float radius, float t, float aa) {
    if (radius < 0.0) return kMinHalf; // (the Tester switch off: the narrowest rim)
    float s = saturate(t);
    return max(max(kMinHalf, radius * (1.0 - s)), aa * s);
}
// SlantOf(d, n).At(half): (along, up)
float2 RimOf(float dx, float dy, float dz, float nx, float nz, float half) {
    float sa = 1.0, su = 1.0;
    float nl = sqrt(nx * nx + nz * nz), dl = sqrt(dx * dx + dy * dy + dz * dz), hl = sqrt(dx * dx + dz * dz);
    [branch] if (nl > 1e-6 && dl > 1e-6) {
        nx /= nl;
        nz /= nl;
        float dn = abs(dx * nx + dz * nz), dt = abs(-dx * nz + dz * nx);
        sa = hl > 1e-6 ? hl / max(dn, 1e-3 * hl) : 1.0;
        su = sqrt(max(dl * dl - dt * dt, 0.0)) / max(dn, 1e-3 * dl);
    }
    return float2(clamp(half * sa, kMinHalf, kMaxHalf), clamp(half * su, kMinHalf, kMaxHalf));
}

// ---- wall_openings.h
float HeadAt(float2 ou, float4 h03, float h4, float u) {
    if (!(ou.y > ou.x)) return h03.x;
    float f = saturate((u - ou.x) / (ou.y - ou.x)) * 4.0;
    float i = min(floor(f), 3.0);
    float a = i < 0.5 ? h03.x : i < 1.5 ? h03.y : i < 2.5 ? h03.z : h03.w;
    float b = i < 0.5 ? h03.y : i < 1.5 ? h03.z : i < 2.5 ? h03.w : h4;
    return a + (b - a) * (f - i);
}
// CrossPassAt and CrossPass as one loop over (face, patch): the ray's crossing (face 0), then, when its spot covered an
// opening and it crosses the wall, the wall's two faces (kHalfThick) and the smaller share of those. (A loop over faces
// around the loop over patches nests four deep under the lamp and wall loops and drew wrong on the test GPU; three inlined
// copies exceed the 32 temporaries.)
float CrossPass(float pBase, float pn, float segLen, float y0, float y1, float u, float y, float2 rim, float3 across) {
    float m = max(segLen, 1e-3), h = rim.x;
    float du = clamp(kHalfThick * across.x, -kFaceShiftMax, kFaceShiftMax) / m, dy = clamp(kHalfThick * across.y, -kFaceShiftMax, kFaceShiftMax);
    float res = 1.0, opened = 0.0, f = 0.0, pi = 0.0;
    float yy = y, c = 0.0, a = 0.0, b = 0.0, total = 0.0, over = 0.0, through = 0.0;
    bool band = false;
    [loop] [fastopt] for (int k = 0; k < 3 * MAX_PATCHES; k++) {
        if (pi == 0.0) { // a face starts
            if (f > 0.5 && !(opened > 0.5 && across.z > 0.5)) break;
            float uu = f < 0.5 ? u : f < 1.5 ? saturate(u - du) : saturate(u + du);
            yy = f < 0.5 ? y : f < 1.5 ? y - dy : y + dy;
            band = yy > y0 + kEdge && yy < y1 - kEdge;
            c = uu * m, a = max(0.0, c - h), b = min(m, c + h);
            total = PWeight(c, h, a, b);
            over = 0.0, through = 0.0;
        }
        float row = pBase + pi;
        float4 q0 = AtW(sPatch, 0, row, 4.0, g_inv1.zw);
        float p0 = max(q0.x * m, a), p1 = min(q0.y * m, b);
        [branch] if (band && p1 > p0) {
            float4 q1 = AtW(sPatch, 1, row, 4.0, g_inv1.zw), q2 = AtW(sPatch, 2, row, 4.0, g_inv1.zw), q3 = AtW(sPatch, 3, row, 4.0, g_inv1.zw);
            bool isOpen = q3.y > 0.5;
            // pieces [p0, o0] solid, [o0, o1] open, [o1, p1] solid (closed: [p0, p1] alone)
            float o0 = isOpen ? q1.x * m : p1, o1 = isOpen ? q1.y * m : p1;
            [unroll] for (int s = 0; s < 3; s++) {
                float s0 = s == 0 ? p0 : s == 1 ? o0 : o1;
                float s1 = s == 0 ? o0 : s == 1 ? o1 : p1;
                s0 = max(s0, p0);
                s1 = min(s1, p1);
                [branch] if (s1 > s0) {
                    float w = PWeight(c, h, s0, s1), v = clamp(c, s0, s1);
                    float fr = q0.y > q0.x ? saturate((v / m - q0.x) / (q0.y - q0.x)) : 0.0;
                    float ov = Cover(yy - (q0.z + (q0.w - q0.z) * fr), rim.y);
                    float inn = 0.0;
                    if (s == 1) {
                        if (f < 0.5) opened = 1.0;
                        inn = (q3.z > 0.5 ? 1.0 : Cover(yy - q1.z, rim.y)) * Cover(HeadAt(q1.xy, q2, q3.x, v / m) - yy, rim.y) * q1.w;
                    }
                    if (inn > ov) through += w * inn;
                    else over += w * ov;
                }
            }
        }
        pi += 1.0;
        if (pi >= pn) { // the face ends
            float p = band ? (total > 0.0 ? saturate((over + through) / total) : 0.0) : 1.0;
            res = f < 0.5 ? p : f < 1.5 ? p : min(res, p);
            pi = 0.0;
            f += 1.0;
            if (f > 2.5) break;
        }
    }
    return res;
}
float3 AcrossOf(float rx, float ry, float rz, float sx, float sz) {
    float m = sqrt(sx * sx + sz * sz);
    if (!(m > 1e-6)) return 0;
    float n = abs(rx * sz - rz * sx) / m;
    if (!(n > 1e-6)) return 0;
    return float3((rx * sx + rz * sz) / m / n, ry / n, 1.0);
}
)RAW"
R"RAW(
// ---- ground_lamp_shadow.h: Pass (hard) and SoftPass, one CrossPass call site for both
float WallPass(float3 L, float x, float z, float wi) {
    float4 w0 = AtW(sWalls, 0, wi, 3.0, g_inv1.xy), w1 = AtW(sWalls, 1, wi, 3.0, g_inv1.xy), w2 = AtW(sWalls, 2, wi, 3.0, g_inv1.xy);
    float rx = x - L.x, rz = z - L.z, sx = w0.z - w0.x, sz = w0.w - w0.y;
    float den = rx * sz - rz * sx;
    if (abs(den) < 1e-9) return 1.0;
    float qx = w0.x - L.x, qz = w0.y - L.z;
    float t = (qx * sz - qz * sx) / den, u = (qx * rz - qz * rx) / den;
    if (!(t > 0.0 && t < 1.0)) return 1.0;
    if (t * sqrt(rx * rx + rz * rz) < kLampSkip) return 1.0; // the lamp's own face
    float ty = g_grid1.z, y0 = w1.x, y1 = w1.y, wpass = w2.z;
    float py = abs(y0 - ty) < 1.0 ? max(ty, y0) : ty; // the wall at the ground: the point on its own base
    float y = L.y + t * (py - L.y), dy = py - L.y;
    float m = sqrt(sx * sx + sz * sz);
    bool soft = g_grid1.w > 0.5;
    float along = 0.0, inside = 1.0;
    float2 rim;
    bool cross;
    if (soft) {
        if (!(m > 1e-4)) return 1.0;
        if (!(y > y0 - 0.05)) return 1.0;
        float c = u * m;
        bool f0 = w2.x > 0.5, f1 = w2.y > 0.5;
        if ((!f0 && c < 0.0) || (!f1 && c > m) || (f0 && c <= -kMaxHalf) || (f1 && c >= m + kMaxHalf)) return 1.0;
        rim = RimOf(rx, dy, rz, sz, -sx, Half(g_count.z, t, kSoftAA));
        float h = rim.x, a = f0 ? 0.0 : -2.0 * h, b = f1 ? m : m + 2.0 * h;
        if (c <= a - h || c >= b + h) return 1.0;
        along = PWeight(c, h, a, b);
        if (!(along > 0.0)) return 1.0;
        cross = w1.w > 0.5 && u >= 0.0 && u <= 1.0;
        if (!cross) inside = 1.0 - Cover((y1 - 0.02) - y, rim.y);
    } else {
        if (!(u >= 0.0 && u <= 1.0)) return 1.0;
        if (!(y > y0 - 0.05 && y < y1 - 0.02)) return 1.0;
        if (!(w1.w > 0.5)) return wpass;
        // (WallOpenings::RimAt: with the penumbra off the 4 cm rim, no slant)
        rim = g_count.z < 0.0 ? float2(kMinHalf, kMinHalf) : RimOf(rx, dy, rz, sz, -sx, Half(g_count.z, t, 0.0));
        cross = true;
    }
    [branch] if (cross) inside = CrossPass(w1.z, w1.w, m, min(y0, y) - 1.0, y1, u, y, rim, AcrossOf(rx, dy, rz, sx, sz));
    return soft ? saturate(1.0 - along * (1.0 - inside) * (1.0 - wpass)) : inside + (1.0 - inside) * wpass;
}

// ---- foliage_canopy.h: Transmission
float3 CrownLocal(float4 c0, float4 c1, float4 c2, float3 p) {
    float dx = p.x - c0.x, dz = p.z - c0.z;
    return float3((dx * c2.x + dz * c2.y) / c1.x, (p.y - c0.y) / c1.y, (-dx * c2.y + dz * c2.x) / c1.z);
}
float CrownPass(float3 L, float3 P, float ci) {
    float4 c0 = AtW(sCrown, 0, ci, 3.0, g_inv2.xy), c1 = AtW(sCrown, 1, ci, 3.0, g_inv2.xy), c2 = AtW(sCrown, 2, ci, 3.0, g_inv2.xy);
    float3 a = CrownLocal(c0, c1, c2, L), b = CrownLocal(c0, c1, c2, P), d = b - a;
    float A = d.x * d.x + d.y * d.y + d.z * d.z;
    if (!(A > 1e-12)) return 1.0;
    float B = a.x * d.x + a.y * d.y + a.z * d.z, C = a.x * a.x + a.y * a.y + a.z * a.z - 1.0;
    float disc = B * B - A * C;
    if (!(disc > 0.0)) return 1.0;
    float r = sqrt(disc), t0 = max((-B - r) / A, 0.0), t1 = min((-B + r) / A, 1.0);
    if (!(t1 > t0)) return 1.0;
    float3 w = P - L;
    float tau = g_count.w * c0.w * ((t1 - t0) * sqrt(w.x * w.x + w.y * w.y + w.z * w.z));
    return tau < 0.01 ? 1.0 : exp(-tau);
}

// ---- the stamp's weight (Weight), h = max(StampHeight, kMinHead) from the CPU
float LampWeight(float4 l0, float4 l1, float h, float x, float z) {
    if (x < l1.x || x > l1.z || z < l1.y || z > l1.w) return 0.0;
    float dx = l0.x - x, dz = l0.z - z;
    float d2 = dx * dx + dz * dz + h * h;
    return saturate(l0.w * (h / sqrt(d2)) / d2);
}
bool InBox(float4 b, float i, float j) { return i >= b.x && i <= b.y && j >= b.z && j <= b.w; }

float4 ShadePS(float2 vpos : VPOS) : COLOR {
    float2 ij = Texel(vpos);
    float i = ij.x, j = ij.y;
    float lx = (i + 0.5) * 0.25, lz = (j + 0.5) * 0.25;
    float x = g_grid0.x * lx + g_grid0.z * lz + g_grid1.x, z = g_grid0.y * lx + g_grid0.w * lz + g_grid1.y, ty = g_grid1.z;
    // pw, pv: the lamp in progress (its weight here, its visibility so far), summed when its next lamp starts. A lamp with
    // over 255 walls or crowns in reach spans rows (not a loop over blocks: four nested loops drew wrong on the test GPU)
    float den = 0.0, num = 0.0, pw = 0.0, pv = 1.0;
    [loop] [fastopt] for (int l = 0; l < 255; l++) {
        if (l >= g_count.x) break;
        [branch] if (At(sLamps, 4, l, g_inv0.xy).x > 0.5) { // a lamp starts
            den += pw;
            num += pw * pv;
            pw = 0.0, pv = 1.0;
        }
        float4 l3 = At(sLamps, 3, l, g_inv0.xy);
        [branch] if (InBox(l3, i, j)) {
            float4 l0 = At(sLamps, 0, l, g_inv0.xy), l1 = At(sLamps, 1, l, g_inv0.xy), l2 = At(sLamps, 2, l, g_inv0.xy);
            float wgt = LampWeight(l0, l1, l2.x, x, z);
            [branch] if (wgt >= kMinWeight) {
                pw = wgt;
                float dx = x - l0.x, dz = z - l0.z;
                [branch] if (dx * dx + dz * dz < l2.y * l2.y) {
                    [loop] [fastopt] for (int e = 0; e < 255; e++) {
                        if (e >= l2.z || !(pv > 0.0)) break;
                        float4 b = At(sEnt, 2 * e + 1, l, g_inv0.zw);
                        [branch] if (InBox(b, i, j)) {
                            float p = WallPass(l0.xyz, x, z, At(sEnt, 2 * e, l, g_inv0.zw).x);
                            if (p < 1.0) pv *= max(p, 0.0);
                        }
                    }
                    [loop] [fastopt] for (int e2 = 0; e2 < 255; e2++) {
                        if (e2 >= l2.w || !(pv > 0.0)) break;
                        float col = 2 * (l2.z + e2);
                        float4 b = At(sEnt, col + 1, l, g_inv0.zw);
                        [branch] if (InBox(b, i, j)) pv *= CrownPass(l0.xyz, float3(x, ty, z), At(sEnt, col, l, g_inv0.zw).x);
                    }
                }
            }
        }
    }
    den += pw;
    num += pw * pv;
    [loop] [fastopt] for (int o = 0; o < 255; o++) { // lit everywhere they reach: in the total only
        if (o >= g_count.y) break;
        float row = g_count.x + o;
        float4 l3 = At(sLamps, 3, row, g_inv0.xy);
        [branch] if (InBox(l3, i, j)) {
            float4 l0 = At(sLamps, 0, row, g_inv0.xy), l1 = At(sLamps, 1, row, g_inv0.xy), l2 = At(sLamps, 2, row, g_inv0.xy);
            float wgt = LampWeight(l0, l1, l2.x, x, z);
            if (wgt >= kMinWeight) {
                den += wgt;
                num += wgt;
            }
        }
    }
    return float4(saturate(1.0 - (den - num) / max(den, kOtherLight)), den, 0.0, 0.0);
}
)RAW"
R"RAW(
// [1 2 1], clamped at the grid's edges (Finish)
float Blur(float a, float b, float c) { return ((a + 2.0 * b) + c) * 0.25; }
float BlurV(float i, float j) {
    return Blur(At(sSrc, i, max(j - 1.0, 0.0), g_inv2.zw).x, At(sSrc, i, j, g_inv2.zw).x, At(sSrc, i, min(j + 1.0, g_size.y - 1.0), g_inv2.zw).x);
}
float4 BlurHPS(float2 vpos : VPOS) : COLOR {
    float2 ij = Texel(vpos);
    return Blur(At(sSrc, max(ij.x - 1.0, 0.0), ij.y, g_inv2.zw).x, At(sSrc, ij.x, ij.y, g_inv2.zw).x, At(sSrc, min(ij.x + 1.0, g_size.x - 1.0), ij.y, g_inv2.zw).x);
}
float4 BlurVPS(float2 vpos : VPOS) : COLOR {
    float2 ij = Texel(vpos);
    return BlurV(ij.x, ij.y);
}
// the last blur to ToByte's value (round half up of the clamped value x 255) and the stamp weight: the lot's ext texture
float4 BytePS(float2 vpos : VPOS) : COLOR {
    float2 ij = Texel(vpos);
    float v = BlurV(ij.x, ij.y);
    return float4(floor(saturate(v) * 255.0 + 0.5), At(sShade, ij.x, ij.y, g_inv3.xy).y, 0.0, 0.0);
}
// the lowest r of the source's 4 x 4 texels under this one (past the used size: 255)
float4 MinPS(float2 vpos : VPOS) : COLOR {
    float2 ij = Texel(vpos) - float2(g_size.z, 0.0); // (the output's first texel is on row 0; w is only the clip flag)
    float lo = 255.0;
    [unroll] for (int b = 0; b < 4; b++)
        [unroll] for (int a = 0; a < 4; a++) {
            float si = 4.0 * ij.x + a, sj = 4.0 * ij.y + b;
            if (si < g_size.x && sj < g_size.y) lo = min(lo, At(sSrc, si, sj, g_inv2.zw).x);
        }
    if (g_size.w > 0.5) clip(254.5 - lo); // (the lot's occlusion query: kept only when a texel is under 255)
    return float4(lo, 0.0, 0.0, 0.0);
}
// the lot mask (MakeMaskTexture): the room mask in rgb; in alpha the shade where the room mask is not 0, 255 inside rooms
float4 MaskPS(float2 vpos : VPOS) : COLOR {
    float2 ij = Texel(vpos);
    float room = At(sRoom, ij.x, ij.y, g_inv3.zw).x;
    float shade = At(sSrc, ij.x + g_size.z, ij.y + g_size.w, g_inv2.zw).x;
    return float4(room, room, room, room > 0.5 / 255.0 ? shade / 255.0 : 1.0);
}

// ---- the chunk (ChunkTexels: max lost / max seen over the lots, ExtShadeAt bilinear by hand)
void LotAt(sampler2D s, int k, float x, float z, float i, float j, inout float lost, inout float seen) {
    float4 g0 = g_lot[4 * k], g1 = g_lot[4 * k + 1], r = g_lot[4 * k + 2], inv = g_lot[4 * k + 3];
    [branch] if (k < g_chunk.z && InBox(r, i, j) && At(s, g1.z, 0.0, inv.xy).x < 254.5) { // (texel (W, 0): the lot's lowest byte; 255: no ext)
        float det = g0.x * g0.w - g0.z * g0.y;
        float dx = x - g1.x, dz = z - g1.y;
        float fi = ((g0.w * dx - g0.z * dz) / det) * 4.0, fj = ((-g0.y * dx + g0.x * dz) / det) * 4.0;
        [branch] if (abs(det) > 1e-6 && fi >= 0.0 && fj >= 0.0 && fi <= g1.z && fj <= g1.w) {
            float a = clamp(fi - 0.5, 0.0, g1.z - 1.0), b = clamp(fj - 0.5, 0.0, g1.w - 1.0);
            float i0 = floor(a), j0 = floor(b), i1 = min(i0 + 1.0, g1.z - 1.0), j1 = min(j0 + 1.0, g1.w - 1.0);
            float u = a - i0, t = b - j0;
            float2 e00 = At(s, i0, j0, inv.xy).xy, e10 = At(s, i1, j0, inv.xy).xy, e01 = At(s, i0, j1, inv.xy).xy, e11 = At(s, i1, j1, inv.xy).xy;
            float2 vw = (e00 * (1 - u) + e10 * u) * (1 - t) + (e01 * (1 - u) + e11 * u) * t;
            float w = max(vw.y, kOtherLight);
            lost = max(lost, (1.0 - vw.x / 255.0) * w);
            seen = max(seen, w);
        }
    }
}
float4 ChunkPS(float2 vpos : VPOS) : COLOR {
    float2 ij = Texel(vpos);
    float x = g_chunk.x + (ij.x + 0.5) / 4.0, z = g_chunk.y + (ij.y + 0.5) / 4.0;
    float2 ls = g_chunk.w > 0.5 ? At(sPrev, ij.x, ij.y, g_chunk2.xy).xy : float2(0.0, 0.0);
    LotAt(sLot0, 0, x, z, ij.x, ij.y, ls.x, ls.y);
    LotAt(sLot1, 1, x, z, ij.x, ij.y, ls.x, ls.y);
    LotAt(sLot2, 2, x, z, ij.x, ij.y, ls.x, ls.y);
    LotAt(sLot3, 3, x, z, ij.x, ij.y, ls.x, ls.y);
    LotAt(sLot4, 4, x, z, ij.x, ij.y, ls.x, ls.y);
    LotAt(sLot5, 5, x, z, ij.x, ij.y, ls.x, ls.y);
    return float4(ls, 0.0, 0.0);
}
float4 ChunkBytePS(float2 vpos : VPOS) : COLOR {
    float2 ij = Texel(vpos);
    float2 ls = At(sPrev, ij.x, ij.y, g_chunk2.xy).xy;
    float q = 255.0;
    if (ls.y > 0.0) q = min(floor(clamp(255.0 * (1.0 - ls.x / ls.y), 0.0, 255.0) + 0.5), 255.0);
    return q / 255.0;
}
float4 MipPS(float2 vpos : VPOS) : COLOR {
    float2 ij = Texel(vpos);
    float s = 0.0;
    [unroll] for (int k = 0; k < 4; k++) {
        const float2 o = float2(k == 1 || k == 3 ? 1.0 : 0.0, k >= 2 ? 1.0 : 0.0);
        s += floor(tex2Dlod(sMip, float4((2 * ij.x + o.x + 0.5) * g_chunk2.x, (2 * ij.y + o.y + 0.5) * g_chunk2.y, 0, g_chunk2.z)).r * 255.0 + 0.5);
    }
    return floor((s + 2.0) / 4.0) / 255.0;
}

float4 FullVS(float2 p : POSITION) : POSITION { return float4(p, 0.0, 1.0); }
)RAW";
