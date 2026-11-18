// The ground profile's camera math, offline (features/ground_profile_math.h): a camera like the game's (world -> clip rows,
// D3D depth 0..1), ground points projected to pixels and back to the ground through GroundUnder, and the inverse itself.
#include "features/ground_profile_math.h"
#include <cmath>
#include <cstdio>

using namespace GroundProfileMath;

int main() {
    int checks = 0, failures = 0;
    const auto check = [&](bool ok, const char* text, double v) {
        ++checks;
        if (!ok) ++failures;
        std::printf("%s%s (%.3g)\n", ok ? "  ok   " : "  FAIL ", text, v);
    };
    // view: eye above and behind the ground, looking at a point on it; perspective 45 degrees, 16:9, near 0.5, far 3000
    const double eye[3] = {1530, 60, 1010}, at[3] = {1556, 23.4, 1052}, up[3] = {0, 1, 0};
    double f[3] = {at[0] - eye[0], at[1] - eye[1], at[2] - eye[2]};
    double n = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
    for (double& v : f) v /= n;
    double r[3] = {up[1] * f[2] - up[2] * f[1], up[2] * f[0] - up[0] * f[2], up[0] * f[1] - up[1] * f[0]};
    n = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
    for (double& v : r) v /= n;
    const double u[3] = {f[1] * r[2] - f[2] * r[1], f[2] * r[0] - f[0] * r[2], f[0] * r[1] - f[1] * r[0]};
    const double view[4][4] = {{r[0], r[1], r[2], -(r[0] * eye[0] + r[1] * eye[1] + r[2] * eye[2])},
                               {u[0], u[1], u[2], -(u[0] * eye[0] + u[1] * eye[1] + u[2] * eye[2])},
                               {f[0], f[1], f[2], -(f[0] * eye[0] + f[1] * eye[1] + f[2] * eye[2])},
                               {0, 0, 0, 1}};
    const double zn = 0.5, zf = 3000, ys = 1 / std::tan(3.14159265358979 / 8), xs = ys / (16.0 / 9.0);
    const double proj[4][4] = {{xs, 0, 0, 0}, {0, ys, 0, 0}, {0, 0, zf / (zf - zn), -zn * zf / (zf - zn)}, {0, 0, 1, 0}};
    double vp[4][4] = {};
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            for (int k = 0; k < 4; k++) vp[i][j] += proj[i][k] * view[k][j];

    double inv[4][4], worst = 0;
    check(Invert4(vp, inv), "the view-projection inverts", 0);
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            double s = 0;
            for (int k = 0; k < 4; k++) s += vp[i][k] * inv[k][j];
            worst = std::fmax(worst, std::fabs(s - (i == j ? 1 : 0)));
        }
    check(worst < 1e-9, "M x inverse = identity", worst);

    // ground points around the look-at point: pixel and back to the ground (3840 x 2160)
    const unsigned w = 3840, h = 2160;
    const float ground = 23.4f;
    double back = 0;
    int seen = 0;
    for (int i = -10; i <= 10; i++)
        for (int j = -10; j <= 10; j++) {
            const float p[3] = {static_cast<float>(at[0] + i * 1.7), ground, static_cast<float>(at[2] + j * 1.3)};
            LONG x = 0, y = 0;
            if (!Project(vp, p, w, h, x, y)) continue;
            seen++;
            float q[3];
            if (!GroundUnder(vp, POINT{x, y}, w, h, ground, q)) {
                back = 1e9;
                continue;
            }
            back = std::fmax(back, std::hypot(q[0] - p[0], q[2] - p[2]));
        }
    check(seen > 300, "most ground points around the look-at point are on screen", seen);
    check(back < 0.05, "a ground point's pixel leads back to it within 5 cm (one pixel at this distance)", back);
    // behind the camera: no pixel
    const float behind[3] = {static_cast<float>(eye[0] - f[0] * 20), ground, static_cast<float>(eye[2] - f[2] * 20)};
    LONG x = 0, y = 0;
    check(!Project(vp, behind, w, h, x, y), "a point behind the camera has no pixel", 0);
    // the game camera object's matrices (rows right, up, back, eye; right-handed projection, near 0.25, far 3000, both
    // row-vector form) to world -> clip rows: the eye has clip x = y = w = 0, a point ahead is centred at its distance,
    // right and up land on +x and +y, and a scaled (misread) axis is rejected
    {
        const float b[3] = {static_cast<float>(-f[0]), static_cast<float>(-f[1]), static_cast<float>(-f[2])};
        const float uu[3] = {static_cast<float>(u[0]), static_cast<float>(u[1]), static_cast<float>(u[2])};
        const float rr[3] = {uu[1] * b[2] - uu[2] * b[1], uu[2] * b[0] - uu[0] * b[2], uu[0] * b[1] - uu[1] * b[0]};
        float c2w[4][4] = {{rr[0], rr[1], rr[2], 0}, {uu[0], uu[1], uu[2], 0}, {b[0], b[1], b[2], 0},
                           {static_cast<float>(eye[0]), static_cast<float>(eye[1]), static_cast<float>(eye[2]), 1}};
        const float gn = 0.25f, gf = 3000.0f, ga = gf / (gf - gn);
        const float gp[4][4] = {{2.41f, 0, 0, 0}, {0, 3.22f, 0, 0}, {0, 0, -ga, -1}, {0, 0, -ga * gn, 0}};
        float g[4][4];
        check(ViewProjFromCamera(c2w, gp, g), "the game camera's matrices give a view-projection", 0);
        const auto clip = [&](const double p[3], double c[4]) {
            for (int i = 0; i < 4; i++) c[i] = g[i][0] * p[0] + g[i][1] * p[1] + g[i][2] * p[2] + g[i][3];
        };
        double c[4];
        clip(eye, c);
        check(std::fabs(c[0]) + std::fabs(c[1]) + std::fabs(c[3]) < 1e-2, "the eye has clip x = y = w = 0", std::fabs(c[0]) + std::fabs(c[1]) + std::fabs(c[3]));
        const double ahead[3] = {eye[0] + f[0] * 10, eye[1] + f[1] * 10, eye[2] + f[2] * 10};
        clip(ahead, c);
        check(std::fabs(c[0] / c[3]) + std::fabs(c[1] / c[3]) < 1e-4 && std::fabs(c[3] - 10) < 1e-3, "a point 10 m ahead is centred, w = 10", c[3]);
        check(std::fabs(c[2] / c[3] - ga * (1 - gn / 10)) < 1e-5, "its depth is far/(far-near) (1 - near/10)", c[2] / c[3]);
        const double right[3] = {ahead[0] + rr[0], ahead[1] + rr[1], ahead[2] + rr[2]}, upp[3] = {ahead[0] + uu[0], ahead[1] + uu[1], ahead[2] + uu[2]};
        clip(right, c);
        check(std::fabs(c[0] / c[3] - 0.241) < 1e-4, "1 m right at 10 m: x = 2.41 / 10", c[0] / c[3]);
        clip(upp, c);
        check(std::fabs(c[1] / c[3] - 0.322) < 1e-4, "1 m up at 10 m: y = 3.22 / 10", c[1] / c[3]);
        for (float& v : c2w[0]) v *= 1.1f;
        check(!ViewProjFromCamera(c2w, gp, g), "a scaled axis is rejected", 0);
    }
    std::printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
