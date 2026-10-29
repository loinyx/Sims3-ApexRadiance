#pragma once
// The ground profile's camera math (ground_profile.cpp; tools/ground_profile_test checks it): a 4x4 inverse, the pixel of a
// world point and the ground point under a pixel, for a world -> clip matrix of rows (PostScene::CameraViewProj).
#include <windows.h>
#include <cmath>
#include <utility>

namespace GroundProfileMath {
inline bool Invert4(const double m[4][4], double inv[4][4]) {
    double a[4][8];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 8; c++) a[r][c] = c < 4 ? m[r][c] : (c - 4 == r ? 1.0 : 0.0);
    for (int c = 0; c < 4; c++) {
        int piv = c;
        for (int r = c + 1; r < 4; r++)
            if (std::fabs(a[r][c]) > std::fabs(a[piv][c])) piv = r;
        if (std::fabs(a[piv][c]) < 1e-12) return false;
        for (int k = 0; k < 8; k++) std::swap(a[c][k], a[piv][k]);
        const double d = a[c][c];
        for (int k = 0; k < 8; k++) a[c][k] /= d;
        for (int r = 0; r < 4; r++)
            if (r != c) {
                const double f = a[r][c];
                for (int k = 0; k < 8; k++) a[r][k] -= f * a[c][k];
            }
    }
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) inv[r][c] = a[r][c + 4];
    return true;
}

inline bool Project(const double vp[4][4], const float p[3], unsigned w, unsigned h, LONG& x, LONG& y) {
    double c[4];
    for (int r = 0; r < 4; r++) c[r] = vp[r][0] * p[0] + vp[r][1] * p[1] + vp[r][2] * p[2] + vp[r][3];
    if (c[3] <= 1e-6) return false;
    x = static_cast<LONG>((c[0] / c[3] * 0.5 + 0.5) * w), y = static_cast<LONG>((0.5 - c[1] / c[3] * 0.5) * h);
    return x >= 0 && y >= 0 && x < static_cast<LONG>(w) && y < static_cast<LONG>(h);
}
inline bool GroundUnder(const double vp[4][4], POINT pixel, unsigned w, unsigned h, float ground, float out[3]) {
    double inv[4][4];
    if (!w || !h || !Invert4(vp, inv)) return false;
    const double nx = (pixel.x + 0.5) / w * 2.0 - 1.0, ny = 1.0 - (pixel.y + 0.5) / h * 2.0;
    double a[4], b[4];
    for (int r = 0; r < 4; r++) a[r] = inv[r][0] * nx + inv[r][1] * ny + inv[r][3], b[r] = a[r] + inv[r][2];
    if (std::fabs(a[3]) < 1e-12 || std::fabs(b[3]) < 1e-12) return false;
    const double o[3] = {a[0] / a[3], a[1] / a[3], a[2] / a[3]}, e[3] = {b[0] / b[3], b[1] / b[3], b[2] / b[3]};
    if (std::fabs(e[1] - o[1]) < 1e-9) return false;
    const double t = (ground - o[1]) / (e[1] - o[1]);
    out[0] = static_cast<float>(o[0] + (e[0] - o[0]) * t), out[1] = ground, out[2] = static_cast<float>(o[2] + (e[2] - o[2]) * t);
    return true;
}
} // namespace GroundProfileMath
