#pragma once
#include <algorithm>
#include <cmath>

namespace LampCapShadow {
inline constexpr float kBias = 0.02f;
// Two rows project world xz onto a complete one-metre floor tile. Row zero's
// w is the lamp head's height above that tile; zero disables this blocker.
inline float Pass(const float rows[2][4], const float lamp[3], const float point[3], float radius) {
    const float gap = rows[0][3];
    if (!(gap > 0.0001f)) return 1.0f;
    const float t = gap / std::max(lamp[1] - point[1], 0.0001f);
    if (t >= 1.0f) return 1.0f;
    const float x = lamp[0] + t * (point[0] - lamp[0]), z = lamp[2] + t * (point[2] - lamp[2]);
    const float scale = 0.5f / std::max((1.0f - t) * radius, 0.0001f);
    const float u = x * rows[0][0] + z * rows[0][1] + rows[0][2];
    const float v = x * rows[1][0] + z * rows[1][1] + rows[1][2];
    const float a = std::clamp((0.5f - std::fabs(u)) * scale + 0.5f, 0.0f, 1.0f);
    const float b = std::clamp((0.5f - std::fabs(v)) * scale + 0.5f, 0.0f, 1.0f);
    return 1.0f - (a * a * (3.0f - 2.0f * a)) * (b * b * (3.0f - 2.0f * b));
}
}
