#pragma once
// Real-time GI, X-05: the radiance cascades of features/plan_cascades.h on synthetic stories, CPU only. Included after a
// Check(): the cases the GPU forms are compared on (tools/plan_cascades_gpu_test, tools/gi_cascades_test) and the kernel's own
// checks (PlanCascadesChecks: a closed room of unit radiosity returns 1, a closed wall passes exactly 0 with the fix, a
// bright patch in the open against whole rays for ringing, a doorway's penumbra against whole rays).
#include "features/plan_cascades.h"
#include "plan_field_check.h"

namespace PlanCascadesTest {
using PlanCascades::Emit;
using PlanCascades::Params;
using PlanField::Field;
using RtLampProxy::Lot;

inline Lot DoorRooms(bool door = true) {
    Lot lot = PlanFieldTest::TwoRooms();
    if (door) {
        RtLampProxy::Edge* e = PlanFieldTest::EdgeAt(lot.stories[0], 6, 3, 6, 4);
        e->open = true, e->o.u0 = 0.1f, e->o.u1 = 0.9f, e->o.sill = 0.0f;
    }
    return lot;
}
// Radiosity around a "lamp" at (lx, lz), inside the box x0..x1, z0..z1 only: floor v, ceiling v / 2, walls 0.8 v within 4 px
inline Emit LampEmit(const Field& f, int band, float lx, float lz, float x0, float x1, float z0, float z1, float power = 1.0f) {
    Emit e;
    e.w = f.w, e.h = f.h, e.v.assign(static_cast<size_t>(f.w) * f.h * 4, 0.0f);
    for (int iz = 0; iz < f.h; iz++)
        for (int ix = 0; ix < f.w; ix++) {
            const float x = (static_cast<float>(ix - PlanField::kPad) + 0.5f) / static_cast<float>(f.px), z = (static_cast<float>(iz - PlanField::kPad) + 0.5f) / static_cast<float>(f.px);
            if (x <= x0 || x >= x1 || z <= z0 || z >= z1) continue;
            const float v = power / (1.0f + (x - lx) * (x - lx) + (z - lz) * (z - lz));
            float* o = &e.v[(static_cast<size_t>(iz) * f.w + ix) * 4];
            o[0] = f.d[(static_cast<size_t>(band) * f.h + iz) * f.w + ix] < 4.0f ? 0.8f * v : 0.0f, o[1] = v, o[2] = 0.5f * v;
        }
    return e;
}
struct Diff {
    double sum = 0, worst = 0, peak = 0;
    long n = 0, within = 0;
    void Add(float a, float ref, float tol) {
        const double d = std::fabs(static_cast<double>(a) - ref);
        sum += d, worst = std::max(worst, d), peak = std::max(peak, static_cast<double>(std::fabs(ref))), n++, within += d <= tol;
    }
};
// Two images of the same size: every value against tol
inline Diff Compare(const std::vector<float>& a, const std::vector<float>& ref, float tol) {
    Diff d;
    for (size_t i = 0; i < ref.size() && i < a.size(); i++) d.Add(a[i], ref[i], tol);
    return d;
}
} // namespace PlanCascadesTest

static void PlanCascadesChecks() {
    using namespace PlanCascadesTest;
    using namespace PlanCascades;
    std::printf("\n[radiance cascades over the plan field, CPU]\n");
    const auto name = [](const Params& p) {
        char b[96];
        std::snprintf(b, sizeof b, "%d levels, %2d directions, %.2f m, l0 %.2f m, %s", p.levels, p.d0, p.spacing, p.l0, p.fix ? "fix  " : "plain");
        return std::string(b);
    };
    std::vector<Params> set;
    for (const bool fix : {true, false})
        for (const int d0 : {16, 4}) {
            Params p;
            p.fix = fix, p.d0 = d0, p.l0 = d0 == 16 ? 1.0f : 0.25f, p.levels = d0 == 16 ? 3 : 4;
            set.push_back(p);
        }
    { // 1. a closed room whose walls, floor and ceiling all have radiosity 1: every face receives 1
        Lot lot;
        lot.stories.push_back(PlanFieldTest::MakeStory(0, 12, 8, 0.0f));
        PlanFieldTest::Box(lot.stories[0], 1, 1, 11, 7);
        const Field f = PlanField::BuildField(lot.stories[0]);
        Emit e;
        e.w = f.w, e.h = f.h, e.v.assign(static_cast<size_t>(f.w) * f.h * 4, 1.0f);
        float b[kOut];
        Brute(f, e, 1, 4.3f, 3.1f, 1024, 40.0f, b);
        std::printf("  unit room, whole rays: faces %.4f %.4f %.4f %.4f, floor %.4f, ceiling %.4f\n", b[0], b[1], b[2], b[3], b[4], b[5]);
        Check(*std::min_element(b, b + 6) > 0.995f && *std::max_element(b, b + 6) < 1.005f, "cascades: whole rays in a room of unit radiosity do not return 1");
        for (const Params& p : set) {
            Layout l;
            const std::vector<float> out = Solve(f, e, p, &l);
            float lo = 1e9f, hi = -1e9f;
            for (int pz = 0; pz < l.nz; pz++)
                for (int px = 0; px < l.nx; px++) {
                    const float x = (px + 0.5f) * p.spacing, z = (pz + 0.5f) * p.spacing;
                    if (x < 1 || x > 11 || z < 1 || z > 7) continue;
                    for (int k = 0; k < 6; k++) lo = std::min(lo, out[(static_cast<size_t>(pz) * l.nx + px) * kOut + k]), hi = std::max(hi, out[(static_cast<size_t>(pz) * l.nx + px) * kOut + k]);
                }
            std::printf("  unit room, %s: %.4f .. %.4f\n", name(p).c_str(), lo, hi);
            Check(lo > 0.97f && hi < 1.03f, "cascades: a room of unit radiosity is not 1 within 3 % (" + name(p) + ")");
        }
    }
    for (const bool door : {false, true}) { // 2. a closed wall; 3. a doorway: room B (x > 6) lit from room A only
        const Lot lot = DoorRooms(door);
        const Field f = PlanField::BuildField(lot.stories[0]);
        const Emit e = LampEmit(f, 1, 3.5f, 4.0f, 1.0f, 6.0f, 1.0f, 7.0f);
        for (const Params& p : set) {
            Layout l;
            const std::vector<float> out = Solve(f, e, p, &l);
            double worstB = 0, sum = 0, sumRef = 0, err = 0, peak = 0;
            long lit = 0, n = 0;
            for (int pz = 0; pz < l.nz; pz++)
                for (int px = 0; px < l.nx; px++) {
                    const float x = (px + 0.5f) * p.spacing, z = (pz + 0.5f) * p.spacing;
                    if (x < 6 || x > 11 || z < 1 || z > 7) continue;
                    const float* o = &out[(static_cast<size_t>(pz) * l.nx + px) * kOut];
                    float b[kOut] = {};
                    if (door) Brute(f, e, 1, x, z, 1024, 40.0f, b);
                    for (int k = 0; k < 6; k++) {
                        worstB = std::max(worstB, static_cast<double>(o[k])), lit += o[k] != 0.0f, n++;
                        sum += o[k], sumRef += b[k], err += std::fabs(o[k] - b[k]), peak = std::max(peak, static_cast<double>(b[k]));
                    }
                }
            if (!door) {
                std::printf("  closed wall, %s: %ld of %ld values behind it lit, largest %.5f\n", name(p).c_str(), lit, n, worstB);
                if (p.fix) Check(lit == 0, "cascades: light crosses a closed wall with the fix (" + name(p) + ")");
            } else {
                std::printf("  doorway, %s: behind it sum / whole rays %.3f, mean |diff| %.2f %% of the peak %.4f\n", name(p).c_str(), sumRef > 0 ? sum / sumRef : 0.0, 100.0 * err / n / peak, peak);
                if (p.fix) Check(sumRef > 0 && sum / sumRef > 0.8 && sum / sumRef < 1.25 && err / n / peak < 0.05, "cascades: the doorway's light is off the whole rays (" + name(p) + ")");
            }
        }
    }
    // 4. ringing: a bright floor and ceiling patch (0.5 m, 2 m) in the open, probes on rings around it against whole rays
    for (const float size : {0.5f, 2.0f}) {
        Lot lot;
        lot.stories.push_back(PlanFieldTest::MakeStory(0, 24, 24, 0.0f));
        const Field f = PlanField::BuildField(lot.stories[0]);
        Emit e;
        e.w = f.w, e.h = f.h, e.v.assign(static_cast<size_t>(f.w) * f.h * 4, 0.0f);
        for (int iz = 0; iz < f.h; iz++)
            for (int ix = 0; ix < f.w; ix++) {
                const float x = (ix - PlanField::kPad + 0.5f) / f.px, z = (iz - PlanField::kPad + 0.5f) / f.px;
                if (std::fabs(x - 12.0f) < 0.5f * size && std::fabs(z - 12.0f) < 0.5f * size) e.v[(static_cast<size_t>(iz) * f.w + ix) * 4 + 1] = e.v[(static_cast<size_t>(iz) * f.w + ix) * 4 + 2] = 100.0f;
            }
        std::vector<Params> ring = set;
        for (const float share : {0.25f, 0.05f}) ring.push_back(set[0]), ring.back().capFar = share, ring.back().steps = 64;
        std::vector<float> ref; // whole rays at every 0.25 m probe near a ring, once
        const float rings[] = {1.5f, 3.0f, 6.0f, 10.0f};
        const auto ringOf = [&](float x, float z) {
            for (int k = 0; k < 4; k++)
                if (std::fabs(std::hypot(x - 12.0f, z - 12.0f) - rings[k]) <= 0.125f) return k;
            return -1;
        };
        ref.assign(96 * 96, -1.0f);
        Rows(96, [&](int pz) {
            for (int px = 0; px < 96; px++) {
                const float x = (px + 0.5f) * 0.25f, z = (pz + 0.5f) * 0.25f;
                if (ringOf(x, z) < 0) continue;
                float b[kOut];
                Brute(f, e, 1, x, z, 1024, 34.0f, b);
                ref[static_cast<size_t>(pz) * 96 + px] = b[0] + b[1] + b[2] + b[3]; // (the four faces: all the light in the plan)
            }
        });
        for (Params p : ring) {
            p.levels += 1; // (to 85 m: the whole story)
            Layout l;
            const std::vector<float> out = Solve(f, e, p, &l);
            std::printf("  %.1f m source, %s, steps to %.2f of the distance:", size, name(p).c_str(), p.capFar);
            double worstAll = 0;
            for (int k = 0; k < 4; k++) {
                double sum = 0, sumRef = 0, lo = 1e9, hi = 0;
                for (int pz = 0; pz < 96; pz++)
                    for (int px = 0; px < 96; px++) {
                        if (ringOf((px + 0.5f) * 0.25f, (pz + 0.5f) * 0.25f) != k) continue;
                        const float* o = &out[(static_cast<size_t>(pz) * l.nx + px) * kOut];
                        const double rc = o[0] + o[1] + o[2] + o[3], rf = ref[static_cast<size_t>(pz) * 96 + px];
                        sum += rc, sumRef += rf, lo = std::min(lo, rc / rf), hi = std::max(hi, rc / rf);
                    }
                std::printf(" %g m: %.2f (%.2f .. %.2f)", rings[k], sum / sumRef, lo, hi);
                worstAll = std::max({worstAll, hi - 1.0, 1.0 - lo});
            }
            std::printf("  of the whole rays\n");
            if (p.fix && p.d0 == 16 && size > 1.0f && p.capFar < 0.2f) Check(worstAll < 0.3, "cascades: a 2 m source rings by more than 30 % around it (" + name(p) + ")");
        }
    }
}
