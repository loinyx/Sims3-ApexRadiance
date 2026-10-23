#pragma once
// (2.15.3) Inline square root and absolute value for the hot loops of the room solve (level_light_share.cpp,
// stairwell_bounce.h, penumbra.h, lamp_visibility.h). With /fp:precise the compiler emits std::sqrt(float) and
// std::fabs(float) as calls to the CRT's _sqrtf / _fabsf, which were among the hottest Apex samples inside the game's room
// solve. sqrtss is correctly rounded like sqrtf, and clearing the sign bit is exactly fabsf: the results are bit for bit
// the same. The double overloads keep the standard functions.
// (2.15.3) Also floor / ceil (with SSE2 only, std::floor and std::ceil are CRT calls: floor_pentium4) and isfinite /
// isnan (out-of-line UCRT _fdtest / _fdclass). On 32-bit Windows every XMM register is caller-saved, so each such call
// also spills the caller's live floats. The replacements below give the same bits as the compiled originals for every one
// of the 2^32 float inputs (tools/development/fast_math_check.cpp), apart from Fabs and Floor returning a signalling NaN
// unquieted where fabsf and floorf quiet it (no arithmetic produces one). Forced inline: as plain inline functions the
// compiler kept some of them as out-of-line calls in the room solve (an in-game sample).
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <emmintrin.h>

namespace ApexMath {

__forceinline float Sqrt(float x) { return _mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(x))); }
inline double Sqrt(double x) { return std::sqrt(x); }
__forceinline float Fabs(float x) { return _mm_cvtss_f32(_mm_and_ps(_mm_set_ss(x), _mm_castsi128_ps(_mm_set1_epi32(0x7FFFFFFF)))); }
inline double Fabs(double x) { return std::fabs(x); }

// == std::floor(float): NaN, infinities and |x| >= 2^23 (already integers) returned as they are; -0.0 stays -0.0
__forceinline float Floor(float x) {
    const __m128 v = _mm_set_ss(x), abs = _mm_castsi128_ps(_mm_set1_epi32(0x7FFFFFFF));
    if (!(_mm_cvtss_f32(_mm_and_ps(v, abs)) < 8388608.0f)) return x;
    __m128 t = _mm_cvtepi32_ps(_mm_cvttps_epi32(v));                     // truncated, exact below 2^23
    t = _mm_sub_ss(t, _mm_and_ps(_mm_cmpgt_ss(t, v), _mm_set_ss(1.0f))); // a negative non-integer: one lower
    return _mm_cvtss_f32(_mm_or_ps(_mm_and_ps(t, abs), _mm_andnot_ps(abs, v))); // x's sign (-0.0 and (-1, 0) as floorf)
}
// == std::ceil(float): ceil(x) = -floor(-x) exactly (the negations only flip the sign bit, NaN payloads included)
__forceinline float Ceil(float x) {
    const __m128 sign = _mm_castsi128_ps(_mm_set1_epi32(static_cast<int>(0x80000000u)));
    return _mm_cvtss_f32(_mm_xor_ps(_mm_set_ss(Floor(_mm_cvtss_f32(_mm_xor_ps(_mm_set_ss(x), sign)))), sign));
}
// == static_cast<int>(std::floor(x)) / static_cast<int>(std::ceil(x)) as compiled (cvttss2si of the rounded value:
// 0x80000000 for NaN and out of range). INT_MIN is never adjusted, so out-of-range inputs cannot wrap.
__forceinline int FloorToInt(float x) {
    const int i = _mm_cvtt_ss2si(_mm_set_ss(x));
    return i - (i != INT_MIN && static_cast<float>(i) > x);
}
__forceinline int CeilToInt(float x) {
    const int i = _mm_cvtt_ss2si(_mm_set_ss(x));
    return i + (i != INT_MIN && static_cast<float>(i) < x);
}
// == std::isfinite(float) / std::isnan(float)
__forceinline bool IsFinite(float x) {
    uint32_t b;
    std::memcpy(&b, &x, sizeof b);
    return (b & 0x7F800000u) != 0x7F800000u;
}
__forceinline bool IsNaN(float x) {
    uint32_t b;
    std::memcpy(&b, &x, sizeof b);
    return (b & 0x7FFFFFFFu) > 0x7F800000u;
}
// Only for float: a double or integer argument would be narrowed (or promoted) and give other results than std::
template <typename T> float Floor(T) = delete;
template <typename T> float Ceil(T) = delete;
template <typename T> int FloorToInt(T) = delete;
template <typename T> int CeilToInt(T) = delete;
template <typename T> bool IsFinite(T) = delete;
template <typename T> bool IsNaN(T) = delete;

} // namespace ApexMath
