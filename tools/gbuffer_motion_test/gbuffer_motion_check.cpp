// Offline check of the camera motion vectors (features/gbuffer_motion.h, patches/gbuffer_patch.cpp). Console output only.
// Synthetic view-projections in the game's layout (rows = c40..c43, clip = VP · p, d = clip.z / clip.w) at world
// coordinates near (927, 70, 1200): a still camera gives zero motion, a pan / dolly gives the previous NDC of every
// test point (in double, and with D rounded to float as the shader gets it), a cut and a point behind the last camera
// are flagged. The pass's HLSL (taken from gbuffer_patch.cpp by run.ps1) must compile to ps_3_0.
#include "gbuffer_motion.h"
#include <d3dcompiler.h>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace {

int g_fail = 0, g_pass = 0;
void Check(bool ok, const char* what) {
    (ok ? g_pass : g_fail)++;
    if (!ok) std::printf("FAIL: %s\n", what);
}

// A D3D left-handed perspective camera at eye, yaw (radians around y) and pitch, as the game's c40..c43 rows
void Camera(double ex, double ey, double ez, double yaw, double pitch, double nearZ, double aspect, float out[4][4]) {
    const double fz[3] = {std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch)};
    double rx[3] = {fz[2], 0, -fz[0]};
    const double rl = std::hypot(rx[0], rx[2]);
    rx[0] /= rl, rx[2] /= rl;
    const double uy[3] = {fz[1] * rx[2] - fz[2] * rx[1], fz[2] * rx[0] - fz[0] * rx[2], fz[0] * rx[1] - fz[1] * rx[0]};
    const double e[3] = {ex, ey, ez};
    double view[4][4] = {};
    const double* axes[3] = {rx, uy, fz};
    for (int r = 0; r < 3; r++) {
        for (int k = 0; k < 3; k++) view[r][k] = axes[r][k];
        view[r][3] = -(axes[r][0] * e[0] + axes[r][1] * e[1] + axes[r][2] * e[2]);
    }
    view[3][3] = 1;
    const double A = 1.00008, ty = 1.0 / std::tan(0.5 * 0.8), tx = ty / aspect;
    const double proj[4][4] = {{tx, 0, 0, 0}, {0, ty, 0, 0}, {0, 0, A, -A * nearZ}, {0, 0, 1, 0}};
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < 4; k++) {
            double s = 0;
            for (int j = 0; j < 4; j++) s += proj[r][j] * view[j][k];
            out[r][k] = static_cast<float>(s);
        }
}

// NDC x, y and device depth of a world point; false behind the camera
bool Project(const float vp[4][4], const double p[3], double ndc[3]) {
    double c[4];
    for (int r = 0; r < 4; r++) c[r] = vp[r][0] * p[0] + vp[r][1] * p[1] + vp[r][2] * p[2] + vp[r][3];
    if (c[3] <= 1e-6) return false;
    for (int i = 0; i < 3; i++) ndc[i] = c[i] / c[3];
    return true;
}

// The largest error, in pixels of a 3840-wide screen, of the reprojected previous NDC over a grid of world points seen
// by cur; float D = the shader's arithmetic (D rounded to float, float products)
double MaxErrorPx(const float cur[4][4], const float prev[4][4], bool floatD, int* tested) {
    double D[4][4];
    if (!GBufferMotion::Reproject(cur, prev, D)) return 1e9;
    double worst = 0;
    *tested = 0;
    for (int i = -6; i <= 6; i++)
        for (int j = -3; j <= 3; j++)
            for (int k = 1; k <= 8; k++) {
                const double w[3] = {927.0 + i * 1.7, 70.0 + j * 1.3, 1200.0 + k * k * 1.9};
                double a[3], b[3];
                if (!Project(cur, w, a) || !Project(prev, w, b) || std::fabs(a[0]) > 1 || std::fabs(a[1]) > 1 || a[2] < 0 || a[2] > 1) continue;
                double got[2];
                if (floatD) {
                    const float p[4] = {float(a[0]), float(a[1]), float(a[2]), 1.0f};
                    float q[4];
                    for (int r = 0; r < 4; r++) q[r] = float(D[r][0]) * p[0] + float(D[r][1]) * p[1] + float(D[r][2]) * p[2] + float(D[r][3]) * p[3];
                    if (q[3] <= 1e-6f) return 1e9;
                    got[0] = q[0] / q[3], got[1] = q[1] / q[3];
                } else if (!GBufferMotion::PrevNdc(D, a[0], a[1], a[2], got)) {
                    return 1e9;
                }
                worst = std::fmax(worst, std::fmax(std::fabs(got[0] - b[0]), std::fabs(got[1] - b[1])) * 3840.0 / 2.0);
                ++*tested;
            }
    return worst;
}

bool CompileHlsl(const char* path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) { std::printf("FAIL: %s not found\n", path); return false; }
    std::stringstream ss;
    ss << f.rdbuf();
    const std::string src = ss.str();
    bool ok = true;
    for (const char* entry : {"MotionPS", "ViewPS"}) {
        ID3DBlob* code = nullptr;
        ID3DBlob* err = nullptr;
        const HRESULT hr = D3DCompile(src.data(), src.size(), "gbuffer.hlsl", nullptr, nullptr, entry, "ps_3_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &err);
        if (FAILED(hr)) std::printf("FAIL: %s: %s\n", entry, err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
        else std::printf("%s compiles: %zu bytes\n", entry, code->GetBufferSize());
        ok &= SUCCEEDED(hr);
        if (code) code->Release();
        if (err) err->Release();
    }
    return ok;
}

} // namespace

int main(int argc, char** argv) {
    const double kNear = 0.25, kAspect = 16.0 / 9.0;
    float still[4][4], pan[4][4], dolly[4][4], cut[4][4], turned[4][4];
    Camera(927.0, 78.0, 1180.0, 0.10, -0.35, kNear, kAspect, still);
    Camera(927.0, 78.0, 1180.0, 0.11, -0.35, kNear, kAspect, pan);   // ~0.6 degrees a frame
    Camera(927.3, 78.0, 1180.4, 0.10, -0.35, kNear, kAspect, dolly); // 0.5 m a frame
    Camera(927.0, 78.0, 1180.0, 0.60, -0.35, kNear, kAspect, cut);   // 29 degrees: a cut
    Camera(927.0, 78.0, 1180.0, 3.24, -0.35, kNear, kAspect, turned); // facing back

    double I[4][4];
    Check(GBufferMotion::Reproject(still, still, I), "still: D exists");
    double maxOff = 0;
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < 4; k++) maxOff = std::fmax(maxOff, std::fabs(I[r][k] - (r == k ? 1.0 : 0.0)));
    std::printf("still: D - identity max %.3g\n", maxOff);
    Check(maxOff < 1e-9, "still: D is the identity");
    Check(!GBufferMotion::IsCut(I), "still: no cut");

    int n = 0;
    const double eStill = MaxErrorPx(still, still, true, &n);
    std::printf("still (float D): max %.4f px over %d points\n", eStill, n);
    Check(n > 100 && eStill < 0.05, "still: zero motion within 0.05 px");
    for (const auto& [name, prev] : {std::pair{"pan", pan}, std::pair{"dolly", dolly}}) {
        const double ed = MaxErrorPx(still, prev, false, &n);
        const double ef = MaxErrorPx(still, prev, true, &n);
        std::printf("%s: max %.2g px (double D), %.4f px (float D) over %d points\n", name, ed, ef, n);
        Check(n > 100 && ed < 1e-4, "reprojection exact in double");
        Check(ef < 0.05, "reprojection within 0.05 px with the shader's float D");
        double D[4][4];
        GBufferMotion::Reproject(still, prev, D);
        Check(!GBufferMotion::IsCut(D), "small move: no cut");
    }
    double D[4][4];
    Check(GBufferMotion::Reproject(still, cut, D) && GBufferMotion::IsCut(D), "29 degree turn: cut");
    Check(GBufferMotion::Reproject(still, turned, D) && GBufferMotion::IsCut(D), "camera turned back: cut (centre behind)");
    float singular[4][4] = {};
    Check(!GBufferMotion::Reproject(singular, still, D), "singular camera rejected");
    Check(GBufferMotion::kInvalid > GBufferMotion::kInvalidAbove && GBufferMotion::kInvalid == float(32768), "invalid value above the threshold");

    if (argc > 1) Check(CompileHlsl(argv[1]), "pass HLSL compiles to ps_3_0");
    std::printf("%d passed, %d failed\n%s\n", g_pass, g_fail, g_fail ? "PROBLEMS" : "OK");
    return g_fail ? 1 : 0;
}
