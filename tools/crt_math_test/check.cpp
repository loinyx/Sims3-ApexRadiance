// CRT math calls replaced in hot code, against the originals: same bits.
//   - every half value decoded with Pow2 (features/half_float.h) against the std::ldexp decoders it replaced;
//   - ApexMath::Hypot (features/fast_math.h) against std::hypot(float, float) over random pairs: world coordinates,
//     small offsets and random bit patterns (infinities and NaN included).
// Part of Apex Radiance. Credits: @loinyx
#include "fast_math.h"
#include "half_float.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <random>

static uint32_t Bits(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}

int main(int argc, char** argv) {
    int fails = 0;
    for (uint32_t h = 0; h < 0x10000; h++) {
        const uint32_t e = (h >> 10) & 0x1F, m = h & 0x3FF;
        // light_probe HalfToFloat, outdoor_delta_model FromHalf, lot_light_bridge ReadElement (normal and subnormal halves)
        const float oldV = e == 0 ? std::ldexp(static_cast<float>(m), -24) : std::ldexp(static_cast<float>(m | 0x400), static_cast<int>(e) - 25);
        const float newV = e == 0 ? static_cast<float>(m) * Pow2(-24) : static_cast<float>(m | 0x400) * Pow2(static_cast<int>(e) - 25);
        // light_probe's capture reader (e up to 31)
        const float oldC = e == 0 ? m / 1024.0f / 16384.0f : std::ldexp(1.0f + m / 1024.0f, static_cast<int>(e) - 15);
        const float newC = e == 0 ? m / 1024.0f / 16384.0f : (1.0f + m / 1024.0f) * Pow2(static_cast<int>(e) - 15);
        if (e != 31 && Bits(oldV) != Bits(newV)) fails++;
        if (Bits(oldC) != Bits(newC)) fails++;
    }
    for (int k = -126; k <= 127; k++)
        if (Pow2(k) != std::ldexp(1.0f, k)) fails++;
    std::printf("half float decode: %s\n", fails ? "differences" : "all 65536 halves and 254 powers bit-identical");

    const long pairs = argc > 1 ? std::atol(argv[1]) : 20000000;
    std::mt19937_64 r(12345);
    std::uniform_real_distribution<float> world(-2000.0f, 2000.0f), small(-2.0f, 2.0f);
    long n = 0, diff = 0;
    auto test = [&](float x, float y) {
        n++;
        const float a = std::hypot(x, y), b = ApexMath::Hypot(x, y);
        if (Bits(a) != Bits(b) && !(std::isnan(a) && std::isnan(b))) diff++;
    };
    for (long i = 0; i < pairs; i++) {
        test(world(r), world(r));
        if ((i & 3) == 0) test(small(r), small(r));
        if ((i & 15) == 0) {
            const uint32_t a = static_cast<uint32_t>(r()), c = static_cast<uint32_t>(r());
            float x, y;
            std::memcpy(&x, &a, 4);
            std::memcpy(&y, &c, 4);
            test(x, y);
        }
    }
    std::printf("hypot: %ld pairs, %ld differences\n", n, diff);
    return fails || diff ? 1 : 0;
}
