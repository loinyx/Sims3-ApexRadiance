// The Sims 3's CAS triangle sort, rewritten: see cas_tri_sort.h.
//
// Part of Apex Radiance. Credits: @loinyx
#include "cas_tri_sort.h"
#include <xmmintrin.h>
#include <emmintrin.h>
#include <algorithm>
#include <cstring>
#include <limits>
#include <numeric>

namespace CasTriSort {
namespace {

const __m128 kOne = _mm_set1_ps(1.0f);                    // 0x0107A538
const __m128 kHalf = _mm_set1_ps(0.5f);                   // 0x00F9A5AC
const float kEps = 1.1920928955078125e-07f;               // 0x00FE3474 (FLT_EPSILON)

int16_t Short(const uint8_t* p) {
    int16_t v;
    std::memcpy(&v, p, 2);
    return v;
}

// 0x005D19F0..0x005D1A51 / 0x005D1B72..0x005D1BC5: (x, y, z, x) / w of vertex `index`
__m128 Position(const Input& in, uint32_t index) {
    const uint8_t* a = in.vertices + index * in.stride + in.positionOffset; // 32-bit arithmetic as in the game
    const float x = static_cast<float>(Short(a)), y = static_cast<float>(Short(a + 2)), z = static_cast<float>(Short(a + 4)), w = static_cast<float>(Short(a + 6));
    return _mm_div_ps(_mm_setr_ps(x, y, z, x), _mm_set1_ps(w));
}

// 0x005D1A72..0x005D1AD4 (and the same code twice more): d scaled to unit length with rsqrtps and two Newton steps
__m128 Unit(__m128 d) {
    const __m128 sq = _mm_mul_ps(d, d);
    __m128 len = _mm_add_ps(_mm_add_ps(_mm_shuffle_ps(sq, sq, 2), _mm_shuffle_ps(sq, sq, 1)), sq); // (z + y) + x in lane 0
    len = _mm_shuffle_ps(len, len, 0);
    const __m128 r = _mm_rsqrt_ps(len);
    const __m128 r1 = _mm_add_ps(_mm_mul_ps(_mm_sub_ps(kOne, _mm_mul_ps(_mm_mul_ps(r, r), len)), _mm_mul_ps(kHalf, r)), r);
    const __m128 r2 = _mm_add_ps(_mm_mul_ps(_mm_sub_ps(kOne, _mm_mul_ps(_mm_mul_ps(r1, r1), len)), _mm_mul_ps(kHalf, r1)), r1);
    return _mm_mul_ps(r2, d);
}

struct Tri {
    __m128 p0, c;
};

// 0x005D19E0..0x005D1B55: P0 and C of triangle t
Tri Setup(const Input& in, uint32_t t) {
    const uint16_t* i = in.indices + 3 * t;
    const __m128 p0 = Position(in, i[0]);
    const __m128 n1 = Unit(_mm_sub_ps(Position(in, i[1]), p0));
    const __m128 n2 = Unit(_mm_sub_ps(Position(in, i[2]), p0));
    const __m128 a = _mm_mul_ps(_mm_shuffle_ps(n2, n2, 9), _mm_shuffle_ps(n1, n1, 0x12));
    const __m128 b = _mm_mul_ps(_mm_shuffle_ps(n2, n2, 0x12), _mm_shuffle_ps(n1, n1, 9));
    return Tri{p0, _mm_sub_ps(b, a)};
}

float Lane(__m128 v, int k) {
    alignas(16) float f[4];
    _mm_store_ps(f, v);
    return f[k];
}

// The game's order: a stable sort, larger counts first (the merge 0x005C9DE0 takes a node of the second half first only
// when its count is larger, unsigned), then the indices written back in that order
void SortAndWrite(const Input& in, const uint32_t* counts, uint16_t* out) {
    const uint32_t n = Triangles(in);
    bool ordered=true;
    for(uint32_t k=1;k<n;++k)if(counts[k]>counts[k-1]){ordered=false;break;}
    if(ordered){
        if(n&&out!=in.indices)std::memmove(out,in.indices,3*static_cast<size_t>(n)*sizeof(uint16_t));
        return;
    }
    std::vector<uint32_t> order(n);
    std::iota(order.begin(), order.end(), 0u);
    std::stable_sort(order.begin(), order.end(), [counts](uint32_t a, uint32_t b) { return counts[a] > counts[b]; });
    std::vector<uint16_t> src(in.indices, in.indices + 3 * static_cast<size_t>(n));
    for (uint32_t k = 0; k < n; k++) std::memcpy(out + 3 * static_cast<size_t>(k), src.data() + 3 * static_cast<size_t>(order[k]), 6);
}

} // namespace

namespace Ref {

uint32_t Count(const Input& in, uint32_t t) {
    const Tri tri = Setup(in, t);
    const __m128 eps = _mm_set_ss(kEps);
    uint32_t count = 0;
    for (uint32_t v = 0; v < in.vertexCount; v++) { // 0x005D1B72..0x005D1C6E
        const __m128 p = _mm_mul_ps(Unit(_mm_sub_ps(Position(in, v & 0xFFFFu), tri.p0)), tri.c);
        const __m128 s = _mm_add_ps(_mm_add_ps(_mm_shuffle_ps(p, p, 2), _mm_shuffle_ps(p, p, 1)), p);
        if (_mm_comigt_ss(s, eps)) count++; // comiss; jbe: never for NaN
    }
    return count;
}

void Sort(const Input& in, uint16_t* indices) {
    const uint32_t n = Triangles(in);
    std::vector<uint32_t> counts(n);
    for (uint32_t t = 0; t < n; t++) counts[t] = Count(in, t);
    SortAndWrite(in, counts.data(), indices);
}

} // namespace Ref

namespace Fast {

void Prepare(const Input& in, Prepared& p) {
    const uint32_t n = in.vertexCount < 65536u ? in.vertexCount : 65536u;
    p.vertices = n;
    p.padded = (n + 3) & ~3u;
    p.storage.assign(3 * static_cast<size_t>(p.padded) + 4, 0.0f);
    float* base = p.storage.data();
    while (reinterpret_cast<uintptr_t>(base) & 15) base++;
    p.x = base;
    p.y = base + p.padded;
    p.z = base + 2 * static_cast<size_t>(p.padded);
    const float nan = std::numeric_limits<float>::quiet_NaN();
    for (uint32_t v = n; v < p.padded; v++) p.x[v] = p.y[v] = p.z[v] = nan; // the padding never counts
    for (uint32_t v = 0; v < n; v += 4) {
        alignas(16) float x[4], y[4], z[4], w[4];
        for (uint32_t k = 0; k < 4; k++) {
            if (v + k < n) {
                const uint8_t* a = in.vertices + (v + k) * in.stride + in.positionOffset;
                x[k] = static_cast<float>(Short(a)), y[k] = static_cast<float>(Short(a + 2)), z[k] = static_cast<float>(Short(a + 4)), w[k] = static_cast<float>(Short(a + 6));
            } else {
                x[k] = y[k] = z[k] = w[k] = 1.0f;
            }
        }
        const __m128 W = _mm_load_ps(w);
        alignas(16) float qx[4], qy[4], qz[4];
        _mm_store_ps(qx, _mm_div_ps(_mm_load_ps(x), W)); // the game's divps: x / w per lane, the same IEEE division
        _mm_store_ps(qy, _mm_div_ps(_mm_load_ps(y), W));
        _mm_store_ps(qz, _mm_div_ps(_mm_load_ps(z), W));
        for (uint32_t k = 0; k < 4 && v + k < n; k++) p.x[v + k] = qx[k], p.y[v + k] = qy[k], p.z[v + k] = qz[k];
    }
}

namespace {

// Vertices [0, m) of the prepared table against one triangle, four per step; lanes at or past m never count
uint32_t CountPrefix(const Prepared& p, uint32_t m, __m128 p0x, __m128 p0y, __m128 p0z, __m128 c0, __m128 c1, __m128 c2) {
    const __m128 eps = _mm_set1_ps(kEps);
    uint32_t count = 0;
    const uint32_t full = m & ~3u;
    auto step = [&](uint32_t v, int laneMask) {
        const __m128 dx = _mm_sub_ps(_mm_load_ps(p.x + v), p0x);
        const __m128 dy = _mm_sub_ps(_mm_load_ps(p.y + v), p0y);
        const __m128 dz = _mm_sub_ps(_mm_load_ps(p.z + v), p0z);
        const __m128 len = _mm_add_ps(_mm_add_ps(_mm_mul_ps(dz, dz), _mm_mul_ps(dy, dy)), _mm_mul_ps(dx, dx)); // (z + y) + x
        const __m128 r = _mm_rsqrt_ps(len);
        const __m128 r1 = _mm_add_ps(_mm_mul_ps(_mm_sub_ps(kOne, _mm_mul_ps(_mm_mul_ps(r, r), len)), _mm_mul_ps(kHalf, r)), r);
        const __m128 r2 = _mm_add_ps(_mm_mul_ps(_mm_sub_ps(kOne, _mm_mul_ps(_mm_mul_ps(r1, r1), len)), _mm_mul_ps(kHalf, r1)), r1);
        const __m128 px = _mm_mul_ps(_mm_mul_ps(r2, dx), c0);
        const __m128 py = _mm_mul_ps(_mm_mul_ps(r2, dy), c1);
        const __m128 pz = _mm_mul_ps(_mm_mul_ps(r2, dz), c2);
        const __m128 s = _mm_add_ps(_mm_add_ps(pz, py), px); // (z + y) + x
        const int bits = _mm_movemask_ps(_mm_cmpgt_ps(s, eps)) & laneMask; // ordered: never for NaN
        count += static_cast<uint32_t>((bits & 1) + ((bits >> 1) & 1) + ((bits >> 2) & 1) + ((bits >> 3) & 1));
    };
    for (uint32_t v = 0; v < full; v += 4) step(v, 0xF);
    if (full < m) step(full, (1 << (m - full)) - 1);
    return count;
}

} // namespace

void CountRange(const Input& in, const Prepared& p, uint32_t first, uint32_t last, uint32_t* counts) {
    const uint32_t passes = in.vertexCount >> 16, rest = in.vertexCount & 0xFFFFu; // the game reads vertex v & 0xFFFF
    for (uint32_t t = first; t < last; t++) {
        const Tri tri = Setup(in, t);
        const __m128 p0x = _mm_set1_ps(Lane(tri.p0, 0)), p0y = _mm_set1_ps(Lane(tri.p0, 1)), p0z = _mm_set1_ps(Lane(tri.p0, 2));
        const __m128 c0 = _mm_set1_ps(Lane(tri.c, 0)), c1 = _mm_set1_ps(Lane(tri.c, 1)), c2 = _mm_set1_ps(Lane(tri.c, 2));
        uint32_t count = 0;
        if (passes) count = passes * CountPrefix(p, 65536u, p0x, p0y, p0z, c0, c1, c2);
        if (rest) count += CountPrefix(p, rest, p0x, p0y, p0z, c0, c1, c2);
        counts[t] = count;
    }
}

void Finish(const Input& in, const uint32_t* counts, uint16_t* indices) { SortAndWrite(in, counts, indices); }

} // namespace Fast

} // namespace CasTriSort
