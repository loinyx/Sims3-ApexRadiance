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
    std::printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
