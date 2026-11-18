#pragma once
// The ground profile's camera math (ground_profile.cpp; tools/terrain_stamp_test/ground_math_check.cpp checks it): a 4x4 inverse, the pixel of a
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
// World -> clip rows (the form of PostScene::CameraViewProj: clip = vp . (x, y, z, 1)) of the game's camera object
// (LotLightingMotion::CameraMatrices): camera-to-world rows right, up, back, eye and the projection, both row-vector form.
// false unless the three axes are orthonormal (a misread camera).
inline bool ViewProjFromCamera(const float c2w[4][4], const float proj[4][4], float vp[4][4]) {
    for (int a = 0; a < 3; a++)
        for (int b = 0; b < 3; b++)
            if (std::fabs(c2w[a][0] * c2w[b][0] + c2w[a][1] * c2w[b][1] + c2w[a][2] * c2w[b][2] - (a == b ? 1.0f : 0.0f)) > 1e-3f) return false;
    double v[4][4] = {}; // world -> view, row-vector form: the rigid inverse of c2w
    for (int a = 0; a < 3; a++) {
        for (int k = 0; k < 3; k++) v[k][a] = c2w[a][k];
        v[3][a] = -(double(c2w[3][0]) * c2w[a][0] + double(c2w[3][1]) * c2w[a][1] + double(c2w[3][2]) * c2w[a][2]);
    }
    v[3][3] = 1;
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            double s = 0;
            for (int k = 0; k < 4; k++) s += v[j][k] * proj[k][i];
            vp[i][j] = static_cast<float>(s); // vp row i = column i of view . proj
        }
    return true;
}
} // namespace GroundProfileMath
