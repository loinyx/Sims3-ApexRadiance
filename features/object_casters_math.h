#pragma once
// Object casters (object_casters.h, physical indoor lamps' Real shapes): the pure parts, checked by tools/outdoor_delta_test.
// An object vertex's position as its vertex shader decodes it (short4 at offset 0: xyz / w, the probe of 03/11), the
// object's world rows (VS c[K .. K + 2]: world = rows . (p, 1)), a lot transform (gpu_room_lamps Entry::xf: lot[k] = x xf[k] +
// y xf[4 + k] + z xf[8 + k] + xf[12 + k]), a half float, the camera's clip test and a blocker record a caster stands in.
#include <cmath>
#include <cstdint>
#include <cstring>

namespace ObjectCastersMath {
inline void DecodeShort4(const int16_t s[4], float out[3]) {
    const float w = s[3] ? static_cast<float>(s[3]) : 1.0f;
    for (int k = 0; k < 3; k++) out[k] = static_cast<float>(s[k]) / w;
}
inline void ApplyRows(const float rows[12], const float p[3], float out[3]) {
    for (int r = 0; r < 3; r++) out[r] = rows[4 * r] * p[0] + rows[4 * r + 1] * p[1] + rows[4 * r + 2] * p[2] + rows[4 * r + 3];
}
inline void ApplyXf(const float xf[16], const float p[3], float out[3]) {
    for (int k = 0; k < 3; k++) out[k] = p[0] * xf[k] + p[1] * xf[4 + k] + p[2] * xf[8 + k] + xf[12 + k];
}
inline float Half(uint16_t h) {
    const uint32_t s = (h >> 15) & 1u, e = (h >> 10) & 0x1Fu, m = h & 0x3FFu;
    float v;
    if (e == 0) v = std::ldexp(static_cast<float>(m), -24);
    else if (e == 31) v = m ? NAN : INFINITY;
    else v = std::ldexp(static_cast<float>(m | 0x400u), static_cast<int>(e) - 25);
    return s ? -v : v;
}
// p (world) inside the camera's view: clip = vp rows . (p, 1), with a margin of m x w on x and y
inline bool InView(const float vp[4][4], const float p[3], float m = 0.1f) {
    float c[4];
    for (int r = 0; r < 4; r++) c[r] = vp[r][0] * p[0] + vp[r][1] * p[1] + vp[r][2] * p[2] + vp[r][3];
    if (!(c[3] > 1e-4f)) return false;
    const float lim = c[3] * (1.0f - m);
    return std::fabs(c[0]) <= lim && std::fabs(c[1]) <= lim && c[2] >= 0.0f && c[2] <= c[3];
}
// a blocker record (centre, r^2: rt_lamp_blockers.h) holds a caster whose world box centre is p
inline bool Covers(const float centre[3], float r2, const float p[3]) {
    const float dx = p[0] - centre[0], dy = p[1] - centre[1], dz = p[2] - centre[2];
    return dx * dx + dy * dy + dz * dz <= r2;
}
// a lot-space box (lo, hi) holds the lamp at q (grown by g): the lamp's own object, left out of its own tile
inline bool Holds(const float lo[3], const float hi[3], const float q[3], float g = 0.02f) {
    for (int k = 0; k < 3; k++)
        if (q[k] < lo[k] - g || q[k] > hi[k] + g) return false;
    return true;
}
} // namespace ObjectCastersMath
