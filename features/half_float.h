#pragma once
// Exact powers of two for the half-float decoders. std::ldexp on x86 saves and restores the FPU control word on every
// call (__ctrlfp, the hottest Apex function in hitch samples while lots relight); a half's value is m x 2^k with k in
// -24..5, which a multiplication by 2^k gives with the same bits (checked over every half by tools/crt_math_test).
#include <bit>
#include <cstdint>

// 2^k for k in -126..127 (normal floats only)
inline float Pow2(int k) { return std::bit_cast<float>(static_cast<uint32_t>(k + 127) << 23); }
