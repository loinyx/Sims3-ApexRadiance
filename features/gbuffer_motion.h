#pragma once
// Camera motion vectors (G-buffer work package GB-01, research 14 §2a): the matrix that takes a pixel of this frame
// (NDC x, y and the device depth d, w = 1) to its clip position in the previous frame, D = prevViewProj · inverse(viewProj),
// made in double so world coordinates near (927, 70, 1200) never reach a float shader. Rows of a view-projection are the
// game's vertex constants c40..c43 (PostScene::CameraViewProj): clip = VP · (x, y, z, 1).
// Motion convention: mv = current NDC xy - previous NDC xy, no jitter. Pure math, checked by tools/gbuffer_motion_test.
#include <cmath>

namespace GBufferMotion {

// The value a pixel gets when no previous position exists (camera cut, no last camera, point behind the last camera).
// Exact in half float; any |mv| above kInvalidAbove reads as invalid.
inline constexpr float kInvalid = 32768.0f;
inline constexpr float kInvalidAbove = 1000.0f;
// A cut: the screen centre at a mid depth moved more than this in NDC (the threshold the removed temporal SMAA used)
inline constexpr double kCutNdc = 0.3;
// The depth of that centre test: d = 0.99 is ~25 m with near 0.25 and A = 1.00008
inline constexpr double kCutDepth = 0.99;

// m^-1 by Gauss-Jordan with partial pivoting; false when m is singular
inline bool Inverse(const double m[4][4], double out[4][4]) {
    double a[4][8];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) a[r][c] = m[r][c], a[r][c + 4] = r == c ? 1.0 : 0.0;
    for (int c = 0; c < 4; c++) {
        int p = c;
        for (int r = c + 1; r < 4; r++)
            if (std::fabs(a[r][c]) > std::fabs(a[p][c])) p = r;
        if (std::fabs(a[p][c]) < 1e-300) return false;
        if (p != c)
            for (int k = 0; k < 8; k++) std::swap(a[p][k], a[c][k]);
        const double inv = 1.0 / a[c][c];
        for (int k = 0; k < 8; k++) a[c][k] *= inv;
        for (int r = 0; r < 4; r++) {
            if (r == c) continue;
            const double f = a[r][c];
            if (f != 0.0)
                for (int k = 0; k < 8; k++) a[r][k] -= f * a[c][k];
        }
    }
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) out[r][c] = a[r][c + 4];
    return true;
}

// D = prev · inverse(cur); false when cur is singular
inline bool Reproject(const float cur[4][4], const float prev[4][4], double D[4][4]) {
    double c[4][4], ci[4][4];
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < 4; k++) c[r][k] = cur[r][k];
    if (!Inverse(c, ci)) return false;
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < 4; k++) {
            double s = 0.0;
            for (int j = 0; j < 4; j++) s += static_cast<double>(prev[r][j]) * ci[j][k];
            D[r][k] = s;
        }
    return true;
}

// The previous NDC of this frame's pixel (x, y) at device depth d; false when it was behind the previous camera
inline bool PrevNdc(const double D[4][4], double x, double y, double d, double out[2]) {
    const double p[4] = {x, y, d, 1.0};
    double q[4];
    for (int r = 0; r < 4; r++) q[r] = D[r][0] * p[0] + D[r][1] * p[1] + D[r][2] * p[2] + D[r][3] * p[3];
    if (q[3] <= 1e-9) return false;
    out[0] = q[0] / q[3];
    out[1] = q[1] / q[3];
    return true;
}

// The whole frame has no usable history: the screen centre at kCutDepth moved more than kCutNdc, or came from behind
inline bool IsCut(const double D[4][4]) {
    double prev[2];
    if (!PrevNdc(D, 0.0, 0.0, kCutDepth, prev)) return true;
    return std::hypot(prev[0], prev[1]) > kCutNdc;
}

} // namespace GBufferMotion
