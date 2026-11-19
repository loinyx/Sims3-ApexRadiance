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

// The world-to-view rotation of a view-projection, one unit row per view axis: x right, y up (rows 0 and 1 over their
// length), z forward (row 3: clip.w is the view depth). The frame of Ambient Occlusion's view positions. Exact for a
// centred projection (no jitter, no off-centre window); false when a row is degenerate.
inline bool ViewRows(const float vp[4][4], float rows[3][3]) {
    const int src[3] = {0, 1, 3};
    for (int i = 0; i < 3; i++) {
        const double l = std::sqrt(double(vp[src[i]][0]) * vp[src[i]][0] + double(vp[src[i]][1]) * vp[src[i]][1] + double(vp[src[i]][2]) * vp[src[i]][2]);
        if (l < 1e-6) return false;
        for (int k = 0; k < 3; k++) rows[i][k] = static_cast<float>(vp[src[i]][k] / l);
    }
    return true;
}

// The radical inverse of i in base b (Halton sequence; i >= 1): the temporal AA's sub-pixel offsets are (h2, h3) - 0.5
inline double Halton(unsigned i, unsigned b) {
    double f = 1.0, r = 0.0;
    for (; i; i /= b) f /= b, r += f * (i % b);
    return r;
}

// The temporal AA's jitter over a frame (GB-05): Present arms the next frame's offset for its scene draws; the scene
// boundary takes it for that frame's passes and disarms it, so the effects and the UI drawn after it are not moved.
// Offsets in clip units (2 / width = one pixel; y up), phases 4..16 of Halton (2, 3) - 0.5
struct JitterClock {
    unsigned index = 0;
    bool armed = false;
    float offset[2] = {};
    void Present(unsigned phases, unsigned width, unsigned height) {
        phases = phases < 4 ? 4 : phases > 16 ? 16 : phases;
        const unsigned i = index++ % phases + 1;
        offset[0] = static_cast<float>(2.0 * (Halton(i, 2) - 0.5) / width);
        offset[1] = static_cast<float>(-2.0 * (Halton(i, 3) - 0.5) / height);
        armed = true;
    }
    // this frame's offset (0 when none was armed); disarms
    bool Boundary(float frame[2]) {
        const bool was = armed;
        frame[0] = was ? offset[0] : 0.0f, frame[1] = was ? offset[1] : 0.0f;
        armed = false;
        return was;
    }
};

// Row 3 of D gives q.w = dot(row, (x, y, d, 1)) = previous view depth / this frame's view depth of the pixel's point:
// a history pixel of the same surface has 1/z_prev = (1/z) / q.w (the temporal depth test).

} // namespace GBufferMotion

// This frame's G-buffer for its consumers (patches/gbuffer_patch.cpp, developer builds; Ambient Occlusion's G-buffer
// normals and temporal filter). Null / false when the G-buffer is off or was not written this frame.
struct IDirect3DTexture9;
namespace GBuffer {
#ifndef APEX_NO_DEV_TOOLS
// render target 1 (albedo + material id, A8R8G8B8) and 2 (octahedral normal, roughness, motion class / 3, A2B10G10R10)
bool Targets(IDirect3DTexture9* out[2]);
// the motion target (A16B16G16R16F: rg = mv = current NDC - previous NDC, kInvalid when there is no history; b = 1 where a
// mover's own vector replaced the camera's, GB-05) and row 3 of the camera's D; null when the motion pass did not run this frame
IDirect3DTexture9* Motion(float dRow3[4]);
// (GB-05) this frame's scene jitter of the temporal AA in clip units (2 / width = one pixel): the depth at pixel p shows the
// surface point whose unjittered NDC is ndc(p) - jitter. False (and 0) when the scene was not jittered
bool Jitter(float clip[2]);
#else
inline bool Targets(IDirect3DTexture9*[2]) { return false; }
inline IDirect3DTexture9* Motion(float[4]) { return nullptr; }
inline bool Jitter(float clip[2]) { clip[0] = clip[1] = 0; return false; }
#endif
} // namespace GBuffer
