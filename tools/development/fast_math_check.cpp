// Exhaustive check of features/fast_math.h against the functions it replaces: every one of the 2^32 float bit patterns,
// compiled as the mod is (x86, /O2 /Oi /fp:precise, SSE2). Exits 0 when every result has the same bits, except that a
// signalling NaN input may come back unquieted (fabsf and floorf quiet it; no arithmetic produces one).
//   . tools\terrain_lighting_test\compiler_env.ps1; Initialize-TerrainCompiler
//   cl /nologo /O2 /Oi /fp:precise /EHsc /std:c++20 /I features tools\development\fast_math_check.cpp /Fe:%TEMP%\fast_math_check.exe
// Part of Apex Radiance. Credits: @loinyx

#include "fast_math.h"
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>
#include <vector>

namespace {

// The originals, kept out of line so the compiler cannot fold them into the loop
__declspec(noinline) float RefSqrt(float x) { return std::sqrt(x); }
__declspec(noinline) float RefFabs(float x) { return std::fabs(x); }
__declspec(noinline) float RefFloor(float x) { return std::floor(x); }
__declspec(noinline) float RefCeil(float x) { return std::ceil(x); }
__declspec(noinline) int RefFloorToInt(float x) { return static_cast<int>(std::floor(x)); }
__declspec(noinline) int RefCeilToInt(float x) { return static_cast<int>(std::ceil(x)); }
__declspec(noinline) bool RefIsFinite(float x) { return std::isfinite(x); }
__declspec(noinline) bool RefIsNaN(float x) { return std::isnan(x); }

uint32_t Bits(float f) {
    uint32_t b;
    std::memcpy(&b, &f, sizeof b);
    return b;
}
bool IsNaNBits(uint32_t b) { return (b & 0x7FFFFFFFu) > 0x7F800000u; }
bool IsSignallingNaN(uint32_t b) { return IsNaNBits(b) && !(b & 0x00400000u); }

std::atomic<uint64_t> g_bad[8];
std::atomic<uint32_t> g_firstBad[8];

void Check(uint64_t from, uint64_t to) {
    uint64_t bad[8] = {};
    uint32_t first[8] = {};
    for (uint64_t u = from; u < to; u++) {
        const uint32_t b = static_cast<uint32_t>(u);
        float x;
        std::memcpy(&x, &b, sizeof x);
        const auto note = [&](int k) {
            if (!bad[k]++) first[k] = b;
        };
        // (only a signalling NaN input may give another NaN: it comes back unquieted)
        const bool sNaN = IsSignallingNaN(b);
        const auto same = [&](float a, float r) { return Bits(a) == Bits(r) || (sNaN && IsNaNBits(Bits(a)) && IsNaNBits(Bits(r))); };
        if (!same(ApexMath::Sqrt(x), RefSqrt(x))) note(0);
        if (!same(ApexMath::Fabs(x), RefFabs(x))) note(1);
        if (!same(ApexMath::Floor(x), RefFloor(x))) note(2);
        if (ApexMath::FloorToInt(x) != RefFloorToInt(x)) note(3);
        if (ApexMath::CeilToInt(x) != RefCeilToInt(x)) note(4);
        if (ApexMath::IsFinite(x) != RefIsFinite(x)) note(5);
        if (ApexMath::IsNaN(x) != RefIsNaN(x)) note(6);
        if (!same(ApexMath::Ceil(x), RefCeil(x))) note(7);
    }
    for (int k = 0; k < 8; k++)
        if (bad[k]) {
            g_bad[k] += bad[k];
            uint32_t expected = 0;
            g_firstBad[k].compare_exchange_strong(expected, first[k]);
        }
}

} // namespace

int main() {
    const unsigned threads = std::max(1u, std::thread::hardware_concurrency());
    const uint64_t total = 1ull << 32, step = total / threads;
    std::vector<std::thread> pool;
    for (unsigned i = 0; i < threads; i++) pool.emplace_back(Check, i * step, i + 1 == threads ? total : (i + 1) * step);
    for (auto& t : pool) t.join();
    const char* names[8] = {"Sqrt", "Fabs", "Floor", "FloorToInt", "CeilToInt", "IsFinite", "IsNaN", "Ceil"};
    int failures = 0;
    for (int k = 0; k < 8; k++) {
        const uint64_t n = g_bad[k].load();
        if (n) failures++;
        std::printf("%-10s %s", names[k], n ? "DIFFERS" : "same bits for all 2^32 inputs");
        if (n) std::printf(" (%llu inputs, first 0x%08X)", static_cast<unsigned long long>(n), g_firstBad[k].load());
        std::printf("\n");
    }
    return failures ? 1 : 0;
}
