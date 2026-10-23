#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "features/fast_math.h"
#include "features/wall_sample_span.h"
#include "extracted_wall_span.h"

int checks = 0;
void Check(bool ok, const char* what) {
    ++checks;
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", what); std::exit(1); }
}
template<class T, size_t N> void Put(std::array<unsigned char, N>& bytes, size_t at, T value) {
    std::memcpy(bytes.data() + at, &value, sizeof value);
}
template<class T, size_t N> T Get(const std::array<unsigned char, N>& bytes, size_t at) {
    T value;
    std::memcpy(&value, bytes.data() + at, sizeof value);
    return value;
}
int main() {
    static_assert(sizeof(uintptr_t) == 4);
    alignas(4) std::array<unsigned char, 0x200> wall{};
    alignas(4) std::array<unsigned char, 13 * 0x30> samples{};
    alignas(4) std::array<unsigned char, 6 * 0x2c> vertices{};
    const float base = 20.708f, foot = 22.947f, height = 0.976f;
    Put(wall, 0x24, 1);
    Put(wall, 0x48, 1); // class 2, columns
    Put(wall, 0x104, base);
    const uintptr_t start = reinterpret_cast<uintptr_t>(samples.data());
    uintptr_t batch[] = {start, start + samples.size()};
    const WallBlock block{0, 0, 0, 12};
    WallCellH cell{foot, height, height, height, true};
    const auto reset = [&] {
        for (int k = 0; k < 13; ++k) {
            Put(samples, k * 0x30 + 4, base + static_cast<float>(k) * 0.25f);
            Put(samples, k * 0x30 + 0x20, uint16_t{0});
        }
    };
    reset();
    Check(MoveWallFootSamples(reinterpret_cast<uintptr_t>(wall.data()), 2, reinterpret_cast<uintptr_t>(batch), block, &cell, 1) == 13,
          "production sample mover accepts the raised gable");
    for (int k = 0; k < 13; ++k)
        Check(std::fabs(Get<float>(samples, k * 0x30 + 4) - (foot + k * 0.25f)) < 1e-5f, "production sample height follows the drawn base");
    const float y[6] = {foot, foot, foot, foot + height, foot + height, foot + height};
    uintptr_t geo[] = {0, 0, reinterpret_cast<uintptr_t>(vertices.data())};
    for (int mode = 0; mode < 2; ++mode) {
        const size_t at = mode ? 0x1e : 0x12;
        Put(vertices, at, int16_t{2000});
        Put(vertices, 5 * 0x2c + at, int16_t{1000});
        int16_t vb = 0, vt = 0;
        Check(RecutCellV(geo, mode, foot, y, vb, vt), "production UV writer accepts both map sets");
        for (int i = 0; i < 6; ++i) {
            const float u = static_cast<float>(Get<int16_t>(vertices, i * 0x2c + at) - vb) / static_cast<float>(vt - vb);
            Check(std::fabs(foot + 3.0f * u - y[i]) <= 0.0016f, "quantized UV and production sample rows agree in world height");
        }
    }
    cell.set = false;
    reset(); const auto untouched = samples;
    Check(MoveWallFootSamples(reinterpret_cast<uintptr_t>(wall.data()), 2, reinterpret_cast<uintptr_t>(batch), block, &cell, 1) == 0 && samples == untouched,
          "unmeasured cell remains untouched");
    cell = WallCellH{base, 3, 3, 3, true}; reset();
    const auto normal = samples;
    Check(MoveWallFootSamples(reinterpret_cast<uintptr_t>(wall.data()), 2, reinterpret_cast<uintptr_t>(batch), block, &cell, 1) == 0 && samples == normal,
          "ordinary wall sample bytes remain unchanged");
    cell.foot = base - 1.9f; reset();
    Check(MoveWallFootSamples(reinterpret_cast<uintptr_t>(wall.data()), 2, reinterpret_cast<uintptr_t>(batch), block, &cell, 1) == 13,
          "existing lower-foot sample mover is preserved");
    for (int k = 0; k < 13; ++k)
        Check(std::fabs(Get<float>(samples, k * 0x30 + 4) - (cell.foot + k * 0.25f)) < 1e-5f, "lower-foot sample height remains correct");
    std::printf("Native wall span: %d checks passed\n", checks);
}
