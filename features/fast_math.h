#pragma once
// (2.15.3) Inline square root and absolute value for the hot loops of the room solve (level_light_share.cpp,
// stairwell_bounce.h, penumbra.h, lamp_visibility.h). With /fp:precise the compiler emits std::sqrt(float) and
// std::fabs(float) as calls to the CRT's _sqrtf / _fabsf, which were among the hottest Apex samples inside the game's room
// solve. sqrtss is correctly rounded like sqrtf, and clearing the sign bit is exactly fabsf: the results are bit for bit
// the same. The double overloads keep the standard functions.
#include <cmath>
#include <emmintrin.h>

namespace ApexMath {

inline float Sqrt(float x) { return _mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(x))); }
inline double Sqrt(double x) { return std::sqrt(x); }
inline float Fabs(float x) { return _mm_cvtss_f32(_mm_and_ps(_mm_set_ss(x), _mm_castsi128_ps(_mm_set1_epi32(0x7FFFFFFF)))); }
inline double Fabs(double x) { return std::fabs(x); }

} // namespace ApexMath
