// Outdoor light through openings: the production story grid walk, the roofed rooms' doors and the door visibility test
// (extracted by run.ps1) on synthetic grids. A colonnade-like roofed room 4 x 4 m with an arch two cells wide on its -z
// wall, room 0 all around. Then the outside walls' copy (FillWallPatches, SegPass): the arch, a window and a half wall in a
// copied wall, the ground shade through a door (ground_lamp_shadow.h) and an arch's curve from a mask (ShapeFromAlpha).
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include "features/lamp_visibility.h"
#include "features/ground_lamp_shadow.h" // (wall_openings.h)
#include "features/ground_light_gate.h"
#include "features/lamp_cap_shadow.h"
#include "features/wall_sample_span.h"
#include "features/inner_retry.h"
#include "features/lamp_reach.h"
#include "features/rig_cut.h"
using DWORD = unsigned long;
struct Xform {
    float r[4][4];
};
void ToLocal(const Xform& x, const float* w, float* l) {
    for (int k = 0; k < 3; k++) l[k] = w[0] * x.r[0][k] + w[1] * x.r[1][k] + w[2] * x.r[2][k] + x.r[3][k];
}
inline int Quadrant(float fx, float fz) { return LampVis::Quadrant(fx, fz); }
enum : uint8_t { kCutUnknown = 0, kCutDoor = 1, kCutWindow = 2, kCutArch = 3, kCutGate = 4 };
#include "extracted_roofed.h"

int checks = 0, failures = 0;
void Check(bool ok, const char* text) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL: %s\n", text);
    }
}

// (2.15.3) the share a window's glass and frame pass (a glazed door's too)
bool Glass(float pass) { return std::fabs(pass - WallOpenings::kWindowPass) < 0.01f; }

struct Builder {
    StoryGrid g;
    Builder(int w, int h) {
        g.w = w, g.h = h;
        const size_t nt = static_cast<size_t>(w) * h;
        g.zl.assign(static_cast<size_t>(h + 1) * w, -1);
        g.xl.assign(static_cast<size_t>(w + 1) * h, -1);
        g.dg.assign(2 * nt, -1);
        g.flags.assign(nt, 0);
        g.ceil.assign(nt, 1e9f);
        g.quad.assign(4 * nt, 0);
    }
    void Room(int x0, int z0, int x1, int z1, int id, bool roofed) {
        for (int z = z0; z < z1; z++)
            for (int x = x0; x < x1; x++) {
                const size_t ti = static_cast<size_t>(z) * g.w + x;
                for (int q = 0; q < 4; q++) g.quad[4 * ti + q] = id;
                g.ceil[ti] = 3.0f; // floor 0 + 3 m
                if (roofed) g.flags[ti] |= 0xF0;
            }
    }
    int32_t Wall(bool zl, int a, int b) { // zl: the edge (a, b)-(a + 1, b); else (a, b)-(a, b + 1)
        const int32_t idx = static_cast<int32_t>(g.edges.size());
        GridEdge e;
        e.foot = 0.0f, e.base = 0.0f, e.topL = e.topM = e.topR = 3.0f, e.faces = 1;
        g.edges.push_back(e);
        (zl ? g.zl[static_cast<size_t>(b) * g.w + a] : g.xl[static_cast<size_t>(b) * (g.w + 1) + a]) = idx;
        g.minFoot = 0.0f;
        return idx;
    }
    void Open(int32_t idx, uint8_t kind, float sill, float head, float u0, float u1) {
        GridOpening o;
        o.kind = kind, o.sill = sill, o.u0 = u0, o.u1 = u1;
        for (float& h : o.head) h = head;
        g.openings.push_back(o);
        g.edges[static_cast<size_t>(idx)].open = static_cast<uint16_t>(g.openings.size());
    }
    // the roofed room 48 on tiles x 4..7, z 4..7, walled all round; arch: the cells x = 5 and 6 of its -z wall
    void Colonnade(bool arch) {
        Room(4, 4, 8, 8, 48, true);
        for (int x = 4; x < 8; x++) {
            const int32_t s = Wall(true, x, 4);
            Wall(true, x, 8);
            if (arch && x == 5) Open(s, kCutArch, 0.0f, 2.4f, 0.03f, 1.0f);
            if (arch && x == 6) Open(s, kCutArch, 0.0f, 2.4f, 0.0f, 0.97f);
        }
        for (int z = 4; z < 8; z++) Wall(false, 4, z), Wall(false, 8, z);
    }
};

float Pass(const StoryGrid& g, float ax, float ay, float az, float bx, float by, float bz, GridWhy* out = nullptr) {
    const float A[3] = {ax, ay, az}, B[3] = {bx, by, bz};
    GridWhy why{};
    const float p = GridPassImpl(g, A, B, why);
    if (out) *out = why;
    return p;
}

// The outside walls' copy (render thread tests and the ground shade): world = lot space here (identity matrix, lot origin 0)
Xform Identity() {
    Xform x{};
    x.r[0][0] = x.r[1][1] = x.r[2][2] = x.r[3][3] = 1.0f;
    return x;
}
float SegShare(const StoryWallCopy& c, size_t i, float lx, float ly, float lz, float px, float py, float pz) {
    const float L[3] = {lx, ly, lz}, P[3] = {px, py, pz};
    int why = 0;
    float u = 0, y = 0;
    return SegPass(c.segs[i], c.patches.data(), L, P, 1.0f, why, u, y);
}
void WallCopyChecks() {
    Builder b(12, 12);
    b.Colonnade(true);
    b.g.xf = Identity();
    // the arcade's -z wall copied both ways (a face run toward +x and one toward -x), and a diagonal wall off the grid
    StoryWallCopy c;
    c.segs.push_back(WallSeg{4.0f, 4.0f, 8.0f, 4.0f, 0.0f, 3.0f});
    c.segs.push_back(WallSeg{8.0f, 4.0f, 4.0f, 4.0f, 0.0f, 3.0f});
    c.segs.push_back(WallSeg{0.0f, 0.0f, 3.0f, 3.0f, 0.0f, 3.0f});
    FillWallPatches(b.g, c);
    Check(c.segs[0].pn == 2 && c.segs[1].pn == 2 && c.opened == 4, "the copied wall takes the arch's two cells, both ways");
    Check(c.segs[2].pn == 0 && c.skipped == 1, "a diagonal wall keeps no patch (whole)");
    for (size_t i = 0; i < 2; i++) {
        // a lamp inside the arcade (a garden post under its roof) and points on the grass outside
        Check(SegShare(c, i, 6.0f, 0.9f, 6.0f, 6.0f, 0.0f, 1.0f) >= 0.99f, "inside the arcade: the ground outside the arch is lit");
        Check(SegShare(c, i, 6.0f, 0.9f, 6.0f, 5.2f, 0.0f, 1.0f) >= 0.99f, "and a slanted ray through the arch too");
        Check(SegShare(c, i, 4.5f, 0.9f, 6.0f, 4.5f, 0.5f, 1.0f) == 0.0f, "a ray through the solid cell beside the arch is blocked");
        Check(SegShare(c, i, 6.0f, 2.9f, 6.0f, 6.0f, 2.9f, 1.0f) == 0.0f, "a ray above the arch's head is blocked");
        const float rim = SegShare(c, i, 5.03f, 1.0f, 6.0f, 5.03f, 1.0f, 1.0f);
        Check(rim > 0.3f && rim < 0.7f, "at the arch's jamb: half the light (the soft rim)");
        Check(SegShare(c, i, 6.0f, 1.0f, 6.0f, 6.0f, 1.0f, 1.0f) >= 0.99f, "the seam between the arch's two cells has no rim");
    }
    // a window (sill 0.85) and a half wall on another wall
    Builder w(12, 12);
    w.g.xf = Identity();
    const int32_t win = w.Wall(true, 2, 6);
    w.Wall(true, 3, 6);
    const int32_t half = w.Wall(true, 4, 6);
    w.Open(win, kCutWindow, 0.85f, 2.25f, 0.1f, 0.9f);
    GridEdge& he = w.g.edges[static_cast<size_t>(half)];
    he.half = true, he.topL = he.topM = he.topR = 1.0f;
    StoryWallCopy cw;
    cw.segs.push_back(WallSeg{2.0f, 6.0f, 5.0f, 6.0f, 0.0f, 3.0f});
    FillWallPatches(w.g, cw);
    Check(cw.segs[0].pn == 2 && cw.opened == 1 && cw.lowered == 1, "a window and a half wall: two patches");
    Check(SegShare(cw, 0, 2.5f, 1.5f, 3.0f, 2.5f, 1.5f, 9.0f) >= 0.99f, "through the window: lit");
    Check(SegShare(cw, 0, 2.5f, 0.4f, 3.0f, 2.5f, 0.4f, 9.0f) == 0.0f, "under the window's sill: blocked");
    Check(SegShare(cw, 0, 4.5f, 1.5f, 3.0f, 4.5f, 1.5f, 9.0f) >= 0.99f, "over the half wall: lit");
    Check(SegShare(cw, 0, 4.5f, 0.5f, 3.0f, 4.5f, 0.5f, 9.0f) == 0.0f, "under the half wall's top: blocked");
    Check(SegShare(cw, 0, 3.5f, 1.5f, 3.0f, 3.5f, 1.5f, 9.0f) == 0.0f, "the whole cell between: blocked");
    // a wall with no opening known (no grid edge under it): whole
    Builder e(12, 12);
    e.g.xf = Identity();
    StoryWallCopy ce;
    ce.segs.push_back(WallSeg{2.0f, 6.0f, 5.0f, 6.0f, 0.0f, 3.0f});
    FillWallPatches(e.g, ce);
    Check(ce.segs[0].pn == 0 && SegShare(ce, 0, 2.5f, 1.5f, 3.0f, 2.5f, 1.5f, 9.0f) == 0.0f, "no grid wall under the copy: the wall stays whole");

    // the ground shade through a door: a 6 m wall along z = 4 with a door in its cell x = 2..3, a post 2 m in front
    {
        using namespace GroundShadow;
        Builder d(12, 12);
        d.g.xf = Identity();
        for (int x = 0; x < 6; x++) {
            const int32_t s = d.Wall(true, x, 4);
            if (x == 2) d.Open(s, kCutDoor, 0.0f, 2.2f, 0.08f, 0.92f);
        }
        StoryWallCopy cd;
        cd.segs.push_back(WallSeg{0.0f, 4.0f, 6.0f, 4.0f, 0.0f, 3.0f});
        FillWallPatches(d.g, cd);
        Wall gw{0.0f, 4.0f, 6.0f, 4.0f, 0.0f, 3.0f};
        gw.p0 = cd.segs[0].p0, gw.pn = cd.segs[0].pn;
        const Lamp L{2.5f, 1.0f, 2.0f, 40.0f}; // (a strong post: its stamp outweighs the pass's other light behind the wall)
        Check(Pass(L, 2.5f, 0.0f, 5.5f, gw, cd.patches.data()) >= 0.99f, "ground behind the door: lit");
        Check(Pass(L, 4.5f, 0.0f, 7.5f, gw, cd.patches.data()) == 0.0f, "ground behind the wall beside it: dark");
        Check(Pass(L, 2.5f, 0.0f, 5.5f, gw, nullptr) == 0.0f, "the same wall with no patches: dark (as before)");
        Grid g;
        g.w = g.h = 32; // 8 x 8 m at 4 texels a metre, lot = world
        ShadeJob job;
        job.Init(g, {L}, {gw}, cd.patches);
        Check(job.Step([] { return true; }) && job.Result().size() == 32u * 32u, "the shade job finishes");
        const auto at = [&](float x, float z) { return job.Result()[static_cast<size_t>(z * 4.0f) * 32 + static_cast<size_t>(x * 4.0f)]; };
        Check(at(2.5f, 5.6f) > 200, "the shade: the patch of ground through the door keeps the lamp's light");
        Check(at(5.0f, 6.5f) < 60, "the shade: behind the wall it is dark");
        Check(job.Through() > 0, "the shade counts the texels lit through the opening");
        ShadeJob whole;
        Wall gw2 = gw;
        gw2.pn = 0;
        whole.Init(g, {L}, {gw2});
        whole.Step([] { return true; });
        Check(whole.Result()[static_cast<size_t>(5.6f * 4.0f) * 32 + 10] < 60, "the shade with a whole wall: dark behind the door (as before)");
    }

    // an arch's curve from its cutout mask (no mask is read in game yet): a round head, 32 x 64 alpha over 1 cell
    // and 3 m, cut below 128; the head 2.4 m at the middle, lower toward the jambs
    {
        const int W = 32, H = 64;
        std::vector<uint8_t> a(static_cast<size_t>(W) * H, 255);
        for (int col = 3; col < 29; col++) {
            const float x = (static_cast<float>(col) + 0.5f) / W - 0.5f; // -0.4 .. 0.4 m
            const float head = 1.9f + std::sqrt(std::max(0.0f, 0.41f * 0.41f - x * x)) * 1.2f; // m over the base
            for (int r = 0; r < H; r++) {
                const float y = 3.0f * (1.0f - (static_cast<float>(r) + 0.5f) / H);
                if (y < head) a[static_cast<size_t>(r) * W + col] = 0;
            }
        }
        WallOpenings::MaskShape m;
        Check(WallOpenings::ShapeFromAlpha(a.data(), W, H, 1, 0, 3.0f, 128, m), "the mask has a hole");
        Check(std::fabs(m.u0 - 3.0f / 32.0f) < 1e-3f && std::fabs(m.u1 - 29.0f / 32.0f) < 1e-3f && m.sill < 0.05f, "its span and sill");
        Check(m.head[2] > 2.3f && m.head[0] < m.head[2] - 0.3f && m.head[4] < m.head[2] - 0.3f, "its head is a curve: high in the middle, low at the jambs");
        Builder sb(12, 12);
        sb.g.xf = Identity();
        const int32_t idx = sb.Wall(true, 3, 4);
        sb.Open(idx, kCutArch, m.sill, 2.4f, m.u0, m.u1);
        for (int k = 0; k < WallOpenings::kProfile; k++) sb.g.openings.back().head[k] = m.head[k];
        StoryWallCopy cs;
        cs.segs.push_back(WallSeg{3.0f, 4.0f, 4.0f, 4.0f, 0.0f, 3.0f});
        FillWallPatches(sb.g, cs);
        const float corner = SegShare(cs, 0, 3.13f, 2.25f, 3.0f, 3.13f, 2.25f, 6.0f), middle = SegShare(cs, 0, 3.5f, 2.25f, 3.0f, 3.5f, 2.25f, 6.0f);
        Check(middle >= 0.99f && corner == 0.0f, "through the arch's curve: lit under its crown, blocked in its corner at the same height");
        GridWhy gwy{};
        const float A[3] = {3.5f, 2.25f, 3.0f}, B[3] = {3.5f, 2.25f, 6.0f}, C[3] = {3.13f, 2.25f, 3.0f}, D[3] = {3.13f, 2.25f, 6.0f};
        Check(GridPassImpl(sb.g, A, B, gwy) >= 0.99f && GridPassImpl(sb.g, C, D, gwy) == 0.0f, "the room solves' grid walk takes the same curve");
    }
}

// "Doors block light": the noted cuts of a story placed on its grid (PlaceGridOpenings, production) with doors closed or open
// for lamps. The roofed room 48 (x 4..7, z 4..7) with two cuts on its -z wall at cells x = 5 and 6.
struct DoorRule {
    Builder b{12, 12};
    std::vector<std::pair<uint32_t, CutRec>> cuts;
    std::unordered_set<uint32_t> glazed;
    // lights: window lights (lot space) the glass test takes (GlazedCuts, production) before the cuts are placed
    explicit DoorRule(uint8_t k5, uint8_t k6, bool doorsBlock, const std::vector<GlassLight>& lights = {}) {
        b.Room(4, 4, 8, 8, 48, true);
        for (int x = 4; x < 8; x++) b.Wall(true, x, 4), b.Wall(true, x, 8);
        for (int z = 4; z < 8; z++) b.Wall(false, 4, z), b.Wall(false, 8, z);
        b.g.xf = Identity();
        CutRec r5, r6;
        r5.kind = k5, r6.kind = k6;
        cuts.emplace_back(EdgeKey(5, 4, 6, 4), r5);
        cuts.emplace_back(EdgeKey(6, 4, 7, 4), r6);
        if (doorsBlock) GlazedCuts(b.g, cuts, lights, glazed);
        PlaceGridOpenings(b.g, cuts, doorsBlock, glazed, [](const CutRec&, GridOpening&) {});
    }
    size_t Doors() {
        YardOutline o;
        std::vector<RoofedDoor> doors;
        RoofedDoors(b.g, 48, nullptr, 0, o, doors);
        return doors.size();
    }
    float Through() { return Pass(b.g, 6.5f, 1.5f, 1.0f, 6.5f, 1.5f, 6.5f); } // a lamp in front, a point inside, through cell x = 6
    int Patches() {
        StoryWallCopy c;
        c.segs.push_back(WallSeg{4.0f, 4.0f, 8.0f, 4.0f, 0.0f, 3.0f});
        FillWallPatches(b.g, c);
        return c.opened;
    }
};
void DoorRuleChecks() {
    Check(CutOpensForLamps(kCutArch, true) && CutOpensForLamps(kCutWindow, true), "doors block light: arches and windows stay open");
    Check(!CutOpensForLamps(kCutDoor, true) && !CutOpensForLamps(kCutGate, true) && !CutOpensForLamps(kCutUnknown, true), "doors, gates and unknown cuts close");
    Check(CutOpensForLamps(kCutDoor, false) && CutOpensForLamps(kCutUnknown, false), "option off: every cut is open (as before)");
    {
        DoorRule d(kCutDoor, kCutDoor, true);
        Check(d.b.g.openings.empty() && d.b.g.cutsOnWall == 0 && d.b.g.cutsClosed == 2 && d.b.g.kinds[kCutDoor] == 2 && d.b.g.doorsBlock, "a double door: counted, closed");
        Check(d.Doors() == 0, "a roofed room whose only openings are doors has no door to the outside (no outdoor lamp)");
        Check(d.Through() == 0.0f, "the grid walk: no light through a closed door");
        Check(d.Patches() == 0, "the outside walls' copy: no patch through a closed door");
    }
    {
        DoorRule d(kCutDoor, kCutDoor, false);
        Check(d.b.g.openings.size() == 2 && d.b.g.cutsClosed == 0 && d.b.g.kinds[kCutDoor] == 2, "option off: the double door is open");
        Check(d.b.g.openings[0].u1 == 1.0f || d.b.g.openings[1].u0 == 0.0f, "option off: no jamb between the two door cells (as before)");
        Check(d.Doors() == 2 && d.Through() >= 0.99f && d.Patches() == 2, "option off: doors, walk and copy as before");
    }
    {
        DoorRule d(kCutArch, kCutArch, true);
        Check(d.b.g.openings.size() == 2 && d.Doors() == 2 && d.Through() >= 0.99f && d.Patches() == 2, "doors block light: an arch passes");
    }
    {
        DoorRule d(kCutWindow, kCutWindow, true);
        Check(d.b.g.openings.size() == 2 && d.Doors() == 2 && d.Patches() == 2, "doors block light: a window passes");
        Check(Glass(d.Through()) && Pass(d.b.g, 6.5f, 0.4f, 1.0f, 6.5f, 0.4f, 6.5f) == 0.0f, "through the window's glass (its share), not under its sill");
    }
    {
        DoorRule d(kCutUnknown, kCutGate, true);
        Check(d.b.g.openings.empty() && d.b.g.cutsClosed == 2 && d.b.g.kinds[kCutUnknown] == 1 && d.b.g.kinds[kCutGate] == 1, "an unknown cut and a gate close (fail closed)");
        Check(d.Doors() == 0 && d.Through() == 0.0f, "and let no light in");
    }
    {
        DoorRule d(kCutUnknown, kCutUnknown, false);
        Check(d.b.g.openings.size() == 2 && d.b.g.openings[0].sill > 0.8f, "option off: an unknown cut is the window-sized hole (as before)");
    }
    {
        DoorRule d(kCutWindow, kCutDoor, true); // a window at x = 5 beside a closed door at x = 6
        Check(d.b.g.openings.size() == 1 && d.b.g.cutsClosed == 1, "a window beside a door: only the window opens");
        Check(d.b.g.openings[0].u1 < 1.0f, "the window keeps its jamb toward the closed door");
        Check(d.Through() == 0.0f && Glass(Pass(d.b.g, 5.5f, 1.5f, 1.0f, 5.5f, 1.5f, 6.5f)), "light through the window, not through the door");
    }
}

// Glass doors (2.12.4): a door cut whose cell holds a window light of the door (the base game's French, patio and arched
// doors give each pane one, about 1.2 to 1.8 m up and 0.05 m in front of the wall) is open for lamps while doors block
// light, with a door's opening (sill 0); a solid door (no window light), a fan light over the head, a window light of a
// neighbouring window or of a perpendicular wall leaves it closed. The double door of DoorRule: cells x = 5 and 6 of the
// wall z = 4, the room behind it at z > 4.
void GlassDoorChecks() {
    const GlassLight pane5{5.58f, 1.4f, 4.05f}, pane6{6.40f, 1.4f, 4.05f};
    {
        DoorRule d(kCutDoor, kCutDoor, true, {pane5, pane6}); // a French door: one window light per pane
        Check(d.glazed.size() == 2 && d.b.g.glassDoors == 2 && d.b.g.cutsClosed == 0 && d.b.g.openings.size() == 2, "a glazed double door opens both cells");
        Check(d.b.g.openings[0].sill == 0.0f && std::fabs(d.b.g.openings[0].head[2] - 2.2f) < 1e-6f, "with a door's opening: sill 0, head 2.2 m");
        Check(d.b.g.openings[0].u1 == 1.0f || d.b.g.openings[1].u0 == 0.0f, "no jamb between its two glazed cells");
        Check(d.Doors() == 2 && Glass(d.Through()) && d.Patches() == 2, "lamp light through the glass: roofed doors, grid walk and walls' copy");
        Check(Glass(Pass(d.b.g, 6.5f, 0.3f, 1.0f, 6.5f, 0.3f, 6.5f)), "low through the glass, as through a door (no sill)");
        Check(d.b.g.doorCuts.size() == 2 && d.b.g.glassSig == GlazedSig(d.glazed) && d.b.g.glassSig != GlazedSig({}), "the grid keeps its door cuts and its glass stamp");
        std::unordered_set<uint32_t> again;
        GlazedCuts(d.b.g, d.b.g.doorCuts, {pane5, pane6}, again);
        Check(GlazedSig(again) == d.b.g.glassSig, "the stamp is found again from the kept door cuts (EnsureStoryGrid)");
        GlazedCuts(d.b.g, d.b.g.doorCuts, {pane5}, again);
        Check(GlazedSig(again) != d.b.g.glassSig, "a window light gone changes the stamp");
    }
    {
        DoorRule d(kCutDoor, kCutDoor, true, {pane6}); // glass in one cell only (a door with one glazed leaf)
        Check(d.glazed.size() == 1 && d.glazed.count(EdgeKey(6, 4, 7, 4)) && d.b.g.openings.size() == 1 && d.b.g.cutsClosed == 1, "only the glazed cell opens");
        Check(d.b.g.openings[0].u0 > 0.0f, "it keeps its jamb toward the closed leaf");
        Check(Glass(d.Through()) && Pass(d.b.g, 5.5f, 1.5f, 1.0f, 5.5f, 1.5f, 6.5f) == 0.0f, "light through the glazed leaf, not the solid one");
    }
    {
        DoorRule d(kCutDoor, kCutDoor, true); // a solid door: no window light
        Check(d.glazed.empty() && d.b.g.openings.empty() && d.b.g.glassDoors == 0 && d.Through() == 0.0f, "a solid door stays closed");
    }
    {
        // a window light above the door's head (a fan light), one 0.5 m off the wall line (a window of the perpendicular wall
        // at the corner), one in the next cell along the wall (a window beside the door)
        DoorRule d(kCutDoor, kCutDoor, true, {GlassLight{5.5f, 2.57f, 4.05f}, GlassLight{5.05f, 1.4f, 4.5f}, GlassLight{7.5f, 1.4f, 4.05f}});
        Check(d.glazed.empty() && d.b.g.openings.empty() && d.Through() == 0.0f, "a fan light, a window round the corner and a window beside it open no door");
    }
    {
        DoorRule d(kCutDoor, kCutDoor, false, {pane5, pane6}); // doors do not block light: every door open, as before
        Check(d.glazed.empty() && d.b.g.glassDoors == 0 && d.b.g.openings.size() == 2 && d.b.g.doorCuts.empty(), "option off: doors open, no glass test");
    }
    {
        DoorRule d(kCutWindow, kCutArch, true, {pane5, pane6}); // a window and an arch are open anyway
        Check(d.glazed.empty() && d.b.g.glassDoors == 0 && d.b.g.openings.size() == 2, "windows and arches are not counted as glass doors");
    }
    {
        DoorRule d(kCutGate, kCutUnknown, true, {pane5, GlassLight{6.4f, 1.0f, 4.05f}}); // a gate with glass; an unknown cut with a light in its window hole
        Check(d.glazed.size() == 2 && d.b.g.openings.size() == 2, "a gate or an unknown cut with a window light in its hole opens too");
        Check(d.b.g.openings[1].sill > 0.8f || d.b.g.openings[0].sill > 0.8f, "the unknown cut keeps its kind's window-sized hole");
    }
}

// A colonnade on a foundation (13/10 in game): story 1 stands on the foundation's top (0.74), a garden post on the terrain
// (story 0) in front of an arch, its head at 1.29. Story 0's grid holds the foundation's side under the colonnade's wall,
// capped at the floor of story 1 beside it (CapLowerWalls does it in the build); uncapped it was 3 m tall and hid every
// point inside. The story above's walls drawn down past its floor are not walls of the story below (GridEdgePass's floor).
void FoundationChecks() {
    Builder s1(12, 12), s0(12, 12);
    s1.Room(4, 4, 8, 8, 48, true);
    for (int x = 4; x < 8; x++) {
        const int32_t e = s1.Wall(true, x, 4);
        s1.Wall(true, x, 8);
        if (x == 5) s1.Open(e, kCutArch, 0.0f, 2.4f, 0.03f, 1.0f);
        if (x == 6) s1.Open(e, kCutArch, 0.0f, 2.4f, 0.0f, 0.97f);
    }
    for (int z = 4; z < 8; z++) s1.Wall(false, 4, z), s1.Wall(false, 8, z);
    for (GridEdge& e : s1.g.edges) e.foot = e.base = 0.74f, e.topL = e.topM = e.topR = 3.74f;
    s1.g.minFoot = 0.74f;
    for (int x = 4; x < 8; x++) s0.Wall(true, x, 4); // the foundation's side under the arcade's front
    const float L[3] = {6.0f, 1.29f, 1.5f};
    const auto inside = [&](float x, float y, float z) {
        const float P[3] = {x, y, z};
        GridWhy w{};
        return GridPassImpl(s1.g, L, P, w) * GridPassImpl(s0.g, L, P, w);
    };
    Check(inside(6.0f, 0.74f, 6.5f) == 0.0f, "an uncapped foundation side hides the floor inside (the bug)");
    for (GridEdge& e : s0.g.edges) e.topL = e.topM = e.topR = 0.74f; // capped at story 1's floor beside it
    Check(inside(6.0f, 0.74f, 6.5f) >= 0.99f, "capped: the post lights the floor inside through the arch");
    Check(inside(6.2f, 1.5f, 7.95f) >= 0.99f, "and the back wall");
    Check(inside(4.3f, 0.74f, 5.0f) == 0.0f, "but not the floor behind the solid cell beside the arch");
    Check(inside(6.0f, 0.2f, 4.5f) == 0.0f, "a ray crossing the side under the foundation's top stays dark");
    float seen = 0.0f;
    YardOutline o;
    std::vector<RoofedDoor> doors;
    RoofedDoors(s1.g, 48, nullptr, 0, o, doors);
    for (const RoofedDoor& d : doors) seen = std::max(seen, DoorSeen(s1.g, nullptr, L, d, &s0.g));
    Check(doors.size() == 2 && seen >= 0.99f, "the post of the story below sees the arch past the capped side");
    // the story above: a wall over the arch whose drawn foot reached down to story 1's floor (0.74, under its own 3.74)
    Builder s2(12, 12);
    for (int x = 4; x < 8; x++) s2.Wall(true, x, 4);
    for (GridEdge& e : s2.g.edges) e.foot = 0.74f, e.base = 3.74f, e.topL = e.topM = e.topR = 6.74f;
    s2.g.minFoot = 0.74f, s2.g.plane = 3.74f;
    const float A[3] = {6.0f, 2.0f, 1.0f}, B[3] = {6.0f, 2.0f, 6.0f};
    GridWhy w{};
    Check(GridPassImpl(s2.g, A, B, w) == 0.0f, "the story above's wall drawn down past its floor: hit without its floor");
    Check(GridPassImpl(s2.g, A, B, w, s2.g.plane) >= 0.99f, "with its floor: no wall of the story above below it");
    float viaUp = 0.0f;
    for (const RoofedDoor& d : doors) viaUp = std::max(viaUp, DoorSeen(s1.g, &s2.g, L, d, &s0.g));
    Check(viaUp >= 0.99f, "the door test asks the story above only above its floor");
}

// Indoor light through openings: a lit lamp of the roofed room 48 shines out through its arch onto room 0, into the roofed
// room 49 through an arch between them, never through a closed door, a solid wall or a second opening in a row
void InnerChecks() {
    Builder b(12, 12);
    b.Colonnade(true); // room 48 on x 4..7, z 4..7, the arch on its -z wall at x = 5, 6
    std::vector<InnerDoor> out;
    InnerDoors(b.g, 0, nullptr, 0, out, 256);
    Check(out.size() == 2 && out[0].from == 48 && out[1].from == 48, "room 0 sees the arch's two cells into room 48");
    std::vector<InnerDoor> none;
    InnerDoors(b.g, 48, nullptr, 0, none, 256);
    Check(none.empty(), "room 48 has no opening into another roofed room (room 0 is not one)");
    const float lamp[3] = {6.0f, 2.2f, 6.0f}; // a ceiling lamp inside
    float seen = 0.0f;
    for (const InnerDoor& d : out) seen = std::max(seen, DoorSeen(b.g, nullptr, lamp, d.d));
    Check(seen >= 0.99f, "the lamp inside sees the arch's hole");
    Check(Pass(b.g, 6.0f, 2.2f, 6.0f, 6.0f, 0.0f, 1.5f) >= 0.99f, "it lights the ground outside the arch");
    Check(Pass(b.g, 6.0f, 2.2f, 6.0f, 2.0f, 0.0f, 6.0f) == 0.0f, "not the ground beside the room (its side wall)");
    Check(Pass(b.g, 6.0f, 2.2f, 6.0f, 6.0f, 0.0f, 10.0f) == 0.0f, "nor behind it (its back wall)");
    Check(Pass(b.g, 6.0f, 2.2f, 6.0f, 2.25f, 0.5f, 1.0f) == 0.0f, "nor through the solid cell beside the arch");
    Check(InnerSig(out) != 0 && InnerSig(none) == 0, "openings give a signature, none gives 0");
    // a closed door instead of the arch (doors block light): nothing to see, nothing taken
    {
        DoorRule d(kCutDoor, kCutDoor, true);
        std::vector<InnerDoor> shut;
        InnerDoors(d.b.g, 0, nullptr, 0, shut, 256);
        Check(shut.empty(), "a closed door is no opening for the lamp inside");
        DoorRule o(kCutDoor, kCutDoor, false);
        std::vector<InnerDoor> open;
        InnerDoors(o.b.g, 0, nullptr, 0, open, 256);
        Check(open.size() == 2, "doors open when they do not block light");
    }
    // room to room: room 49 (roofed) on z 0..3 in front of the arch, room 50 (roofed) on z 8..11 behind room 48's back wall,
    // which has an arch at x = 6 too
    Builder r(12, 12);
    r.Colonnade(true);
    r.Room(4, 0, 8, 4, 49, true);
    r.Room(4, 8, 8, 12, 50, true);
    r.Open(GridZL(r.g, 6, 8), kCutArch, 0.0f, 2.4f, 0.03f, 0.97f);
    std::vector<InnerDoor> into49, into48;
    InnerDoors(r.g, 49, nullptr, 0, into49, 256);
    InnerDoors(r.g, 48, nullptr, 0, into48, 256);
    Check(into49.size() == 2 && std::all_of(into49.begin(), into49.end(), [](const InnerDoor& d) { return d.from == 48; }),
          "room 49 takes room 48's lamps through the arch between them, not room 50's (two openings in a row)");
    Check(into48.size() == 3, "room 48 sees both neighbours: 2 cells into 49, 1 into 50");
    Check(Pass(r.g, 6.0f, 2.2f, 6.0f, 6.0f, 0.0f, 2.0f) >= 0.99f, "the lamp of room 48 lights the floor of room 49 through the arch");
    Check(Pass(r.g, 4.5f, 2.2f, 6.0f, 4.5f, 0.0f, 2.0f) == 0.0f, "not behind the solid cell beside it");
    const int roofless[1] = {49};
    std::vector<InnerDoor> yard;
    InnerDoors(r.g, 49, roofless, 1, yard, 256);
    Check(yard.size() == 2, "a roofless room 49 takes them too (light shining out into a yard)");
    std::vector<InnerDoor> fromYard;
    InnerDoors(r.g, 48, roofless, 1, fromYard, 256);
    Check(fromYard.size() == 1 && fromYard[0].from == 50, "a roofless room's lamps are no indoor lamps (only room 50's)");
    std::vector<InnerDoor> capped;
    InnerDoors(r.g, 48, nullptr, 0, capped, 1);
    Check(capped.size() == 1, "at most `max` openings");
}

// A neighbour that lost a lifted lamp (13/10 in game: room 56 gathered while the lamp of room 1 was lifted and stayed dark
// after it was put down): it gathers again until the lamp is back, a bounded number of times
void InnerRetryChecks() {
    using namespace InnerRetry;
    Check(Lost({1, 2}, {2}) == std::vector<int>{1} && Lost({}, {}).empty() && Lost({3, 3}, {}) == std::vector<int>{3}, "retry: the sources a gather lost");
    List l;
    const Key k{0x1000, 4, 56};
    uint32_t t = 0xFFFFF000u; // (across the tick counter's wrap)
    Check(l.Gathered(k, {1}, t) == kDelayMs[0] && l.Has(k), "retry: a gather that lost room 1's lamp gathers again soon");
    Check(l.TakeDue(t + kDelayMs[0] - 1).empty(), "retry: not before its time");
    std::vector<Entry> due = l.TakeDue(t + kDelayMs[0]);
    Check(due.size() == 1 && due[0].key == k && due[0].tries == 1 && due[0].lost == std::vector<int>{1}, "retry: sent at its time, once");
    Check(l.TakeDue(t + kDelayMs[0] + 10).empty(), "retry: not sent twice");
    // the lamp put down: the retry's gather takes it, nothing waits any more
    Check(l.Gathered(k, {}, t + 900) == 0 && !l.Has(k), "retry: the lamp back, the room is done");
    // a lamp held long: every retry's gather loses it again, until the tries are spent
    List h;
    uint32_t now = 1000;
    uint32_t d = h.Gathered(k, {1}, now);
    int sent = 0;
    while (d && sent < 10) {
        now += d;
        if (h.TakeDue(now).size() == 1) sent++;
        now += 5; // (its gather a little after the send)
        d = h.Gathered(k, {1}, now);
    }
    Check(sent == kTries && !h.Has(k), "retry: a lamp that never comes back costs kTries gathers, then the room is forgotten");
    // a gather sent by something else before the retry's time, with the lamp back: the retry is cancelled
    List c;
    c.Gathered(k, {1}, 0);
    Check(c.Gathered(k, {}, 100) == 0 && c.TakeDue(5000).empty(), "retry: cancelled when the room took its lamps back meanwhile");
    // a sent retry whose room never gathers again (the room went away) is dropped; the list stays bounded
    List g;
    g.Gathered(k, {1}, 0);
    g.TakeDue(kDelayMs[0]);
    g.TakeDue(kDelayMs[0] + kForgetMs);
    Check(!g.Has(k), "retry: a room that never gathers again is dropped");
    List b;
    for (int i = 0; i < 300; i++) b.Gathered(Key{0x1000, 0, i}, {1}, 0);
    Check(b.Size() == kMax, "retry: at most kMax rooms wait");
}

// The columns inside an arcade take a garden post through its arches by the outdoor objects' per-pixel law (13/10 in game:
// they were almost white, the ground footprint's boost, while the columns facing the post took the reach law). The rig's
// colour at its centre (LampReach::PixelLaw, ObjectLampColourAt) against the per-pixel shader's steps at the same point
// (AppendLampLaw, step by step), and against the boost the record had.
void OpeningLawChecks() {
    const float w = 85.0f, entries = 2.0f; // the 12/10 garden post: range 42.5 x intensity 2, a story-0 lot lamp
    float r0, R;
    LampReach::Radii(w, r0, R);
    float worst = 0.0f, wall = 0.0f, boostOver = 1e9f;
    for (float d = 1.0f; d <= 14.0f; d += 0.25f) {
        const float d2 = d * d;
        // the shader: q = d^2 / R^2, sat(1 - q^2)^2, 1/d^2, r0^2/d^2 x that, min 1, x 1/d^2, x the colour row's Gain
        const float q = d2 / (R * R), win = std::clamp(1.0f - q * q, 0.0f, 1.0f), share = std::min(r0 * r0 / d2 * win * win, 1.0f);
        const float shader = share / d2 * LampReach::Gain(w, entries);
        const float rig = LampReach::PixelLaw(d2, w, entries);
        worst = std::max(worst, std::fabs(rig - shader) / shader);
        // the outdoor wall texel's law (LampReach::Share), beyond r0 where both are defined the same
        if (d > r0) wall = std::max(wall, std::fabs(rig - LampReach::Gain(w, entries) * LampReach::Share(d, r0, R) / d2) / rig);
        // the boost the record had: the lamp's colour x intensity x the object strength 0.75 x (1 - d^2 / 25^2)^2 (half its bounds,
        // about 25 m), against the law x the walls' scale 0.3 at the defaults, 6 to 10 m from the post (the inner columns)
        if (d >= 6.0f && d <= 10.0f) {
            const float b = 1.0f - d2 / (25.0f * 25.0f);
            boostOver = std::min(boostOver, 0.75f * b * b / std::min(rig * 0.3f, 1.0f)); // the walls' scale 0.3 at the defaults
        }
    }
    std::printf("opening lamp law: rig vs shader %.2g, vs the wall law beyond r0 %.2g, the old boost at 6-10 m at least %.1fx the law\n", worst, wall, boostOver);
    Check(worst < 1e-5f, "opening law: a rig through an arch takes the per-pixel lamps' law at its centre (inner = outer column)");
    Check(wall < 0.03f, "opening law: the same law as the outdoor wall texels behind the columns");
    Check(boostOver > 3.0f, "opening law: the old boost was far brighter than the law (the near-white columns)");
    Check(LampReach::FirstLaw(0.0f, 8.0f) == 1.0f && LampReach::FirstLaw(64.0f, 8.0f) == 0.0f, "opening law: the first law's ends");
    // 14/10 in game (F7): a column inside an arcade, behind the arch from the garden post, drew black. Its record, the law at
    // its centre x the walls' scale 0.3, fell under the rig test's cut ([0x01158D60] = 0.1 for a rig with +8 clear, read from
    // TS3W.exe), so the game dropped the lamp; every distance where the law still gives a visible level is kept now
    {
        const float cut = 0.1f, warm = 0.2f; // (the luminance of a dim warm colour per unit of the law)
        float firstDropped = -1.0f, lastVisible = -1.0f;
        bool allKept = true;
        for (float d = 1.0f; d <= 20.0f; d += 0.25f) {
            const float luma = std::min(LampReach::PixelLaw(d * d, w, entries) * 0.3f, 1.0f) * warm;
            if (luma < RigCut::kMinLuma) continue;
            lastVisible = d;
            if (!RigCut::GameKeeps(luma, cut) && firstDropped < 0.0f) firstDropped = d;
            allKept = allKept && RigCut::GameKeeps(RigCut::KeptLuma(luma, cut), cut);
        }
        std::printf("rig cut: the game dropped the lamp from %.2f m, its light stays visible to %.2f m\n", firstDropped, lastVisible);
        Check(firstDropped > 0.0f && firstDropped < lastVisible, "rig cut: the game dropped a lamp whose light was still visible");
        Check(allKept, "rig cut: kept now wherever its light is visible");
        Check(RigCut::KeptLuma(0.5f, cut) == 0.5f && RigCut::KeptLuma(0.001f, cut) == 0.001f && RigCut::KeptLuma(std::nanf(""), cut) != cut &&
                  RigCut::KeptLuma(0.05f, 0.0f) == 0.05f && RigCut::KeptLuma(0.005f, 0.01f) == 0.01f,
              "rig cut: a record over the cut, one with no visible light, NaN and an unreadable cut stay as they are; the 0.01 cut too");
    }
}

// A lamp shining out of a roofed room keeps the room's normalisation (13/10 in game: the balcony floor outside a window took
// half the light of the room's own floor at the same distance): the game scales an indoor room's sum by room+0x160 and an
// outdoor room's by nothing
void InnerNormChecks() {
    using InnerLight::NormScale;
    Check(NormScale(2.0f, 7.0f, true) == 2.0f, "inner norm: an outdoor floor takes the source room's factor");
    Check(NormScale(2.0f, 2.0f, false) == 1.0f && NormScale(2.0f, 1.0f, false) == 2.0f, "inner norm: an indoor neighbour divides by its own");
    // the same lamp, the same distance: the room's own floor (raw x its factor) and the floor outside (raw x the scale) match
    const float raw = 0.3f, srcNorm = 2.4f;
    Check(std::fabs(raw * srcNorm - raw * NormScale(srcNorm, 1.0f, true)) < 1e-6f, "inner norm: both sides of the window equal");
    Check(NormScale(std::nanf(""), 1.0f, true) == 1.0f && NormScale(1e9f, 1.0f, true) == 1.0f && NormScale(0.0f, 1.0f, true) == 1.0f, "inner norm: unreadable factors count as 1");
    Check(NormScale(50.0f, 0.05f, false) == 20.0f, "inner norm: bounded");
}

// The lamp's penumbra (penumbra.h, 2.12.4): a wall along z = 4 (cells x = 0..9) with a wide opening over cells 3..5 (jambs at
// x = 3.2 and 5.8, no rim between its cells), a lamp 3 m in front at the height of the points, the points d metres behind.
struct EdgeScan {
    float x10 = -1.0f, x90 = -1.0f, maxStep = 0.0f, lo = 1.0f, hi = 0.0f;
    bool rising = true;
    float Width() const { return x10 >= 0.0f && x90 >= 0.0f ? x90 - x10 : -1.0f; }
};
template <class F> EdgeScan ScanEdge(F pass, float x0, float x1, float step) {
    EdgeScan e;
    float prev = pass(x0);
    for (float x = x0 + step; x <= x1; x += step) {
        const float p = pass(x);
        e.maxStep = std::max(e.maxStep, std::fabs(p - prev));
        if (p < prev - 1e-5f) e.rising = false;
        if (e.x10 < 0.0f && p >= 0.1f) e.x10 = x;
        if (e.x90 < 0.0f && p >= 0.9f) e.x90 = x;
        e.lo = std::min(e.lo, p), e.hi = std::max(e.hi, p);
        prev = p;
    }
    return e;
}
void PenumbraChecks() {
    // the curve: the narrowest rim is the old 4 cm ramp, bit for bit
    float worst = 0.0f;
    for (float x = -0.05f; x <= 0.05f; x += 0.0005f) worst = std::max(worst, std::fabs(Penumbra::Cover(x, Penumbra::kMinHalf) - WallOpenings::Ramp(x)));
    Check(worst < 1e-6f, "penumbra: the narrowest rim is the old 4 cm ramp");
    Check(Penumbra::Half(Penumbra::kMinRadius, 0.0f) == Penumbra::kMinHalf && Penumbra::Half(Penumbra::kMinRadius, 0.7f) == Penumbra::kMinHalf,
          "penumbra: the smallest lamp keeps the old rim everywhere");
    Check(std::fabs(Penumbra::Half(0.15f, 0.5f) - 0.075f) < 1e-6f && Penumbra::Half(0.15f, 1.0f) == Penumbra::kMinHalf, "penumbra: R (1 - t), sharp at the edge");
    Check(std::fabs(Penumbra::Weight(0.3f, 0.1f, 0.0f, 0.3f) + Penumbra::Weight(0.3f, 0.1f, 0.3f, 1.0f) - Penumbra::Weight(0.3f, 0.1f, 0.0f, 1.0f)) < 1e-6f,
          "penumbra: stretches side by side add up");

    Builder b(12, 12);
    for (int x = 0; x < 10; x++) {
        const int32_t s = b.Wall(true, x, 4);
        if (x == 3) b.Open(s, kCutArch, 0.0f, 2.4f, 0.2f, 1.0f);
        if (x == 4) b.Open(s, kCutArch, 0.0f, 2.4f, 0.0f, 1.0f);
        if (x == 5) b.Open(s, kCutArch, 0.0f, 2.4f, 0.0f, 0.8f);
    }
    b.g.xf = Identity();
    const float lx = 4.5f, ly = 1.2f, lz = 1.0f;
    const auto grid = [&](float d) { return [&, d](float x) { return Pass(b.g, lx, ly, lz, x, ly, 4.0f + d); }; };
    const auto edgeAt = [&](float jamb, float d) { return lx + (jamb - lx) * (3.0f + d) / 3.0f; }; // the jamb's shadow on z = 4 + d
    Penumbra::SetRadius(0.15f);
    float w[4];
    const float ds[4] = {0.05f, 1.0f, 2.0f, 4.0f};
    bool rising = true;
    for (int i = 0; i < 4; i++) {
        const float e = edgeAt(3.2f, ds[i]);
        const EdgeScan sc = ScanEdge(grid(ds[i]), e - 0.6f, e + 0.6f, 0.001f);
        w[i] = sc.Width();
        rising = rising && sc.rising && sc.lo == 0.0f && sc.hi >= 0.999f;
    }
    std::printf("penumbra widths (10-90%%) behind the jamb at d = 0.05, 1, 2, 4 m: %.3f %.3f %.3f %.3f\n", w[0], w[1], w[2], w[3]);
    Check(rising, "penumbra: the light rises monotonically across the jamb's shadow, from dark to lit");
    Check(w[0] > 0.0f && w[0] < 0.035f, "penumbra: sharp right behind the jamb (the 4 cm rim)");
    Check(w[2] / w[1] > 1.7f && w[2] / w[1] < 2.3f && w[3] / w[2] > 1.7f && w[3] / w[2] < 2.3f, "penumbra: its width grows linearly with the distance behind the jamb");
    Check(std::fabs(w[3] - 0.066f * 4.0f) < 0.05f, "penumbra: width = 2 R d / (lamp to wall) x 0.6 (10-90%) x the slant");
    // continuity: a wide lamp, the whole stretch behind the opening and the walls beside it, no step anywhere
    Penumbra::SetRadius(0.5f);
    {
        const EdgeScan sc = ScanEdge(grid(4.0f), 0.5f, 4.5f, 0.002f);
        Check(sc.maxStep < 0.01f && sc.rising, "penumbra: no seam where the spot spans the solid cell and the opening's cell");
        float minOpen = 1.0f;
        for (float x = edgeAt(3.2f, 2.0f) + 0.5f; x < edgeAt(5.8f, 2.0f) - 0.5f; x += 0.002f) minOpen = std::min(minOpen, grid(2.0f)(x));
        Check(minOpen >= 0.999f, "penumbra: no dark line at the seams between the opening's cells");
    }
    // the outside walls' copy of the same wall takes the same penumbra
    {
        StoryWallCopy c;
        c.segs.push_back(WallSeg{0.0f, 4.0f, 10.0f, 4.0f, 0.0f, 3.0f});
        FillWallPatches(b.g, c);
        float diff = 0.0f;
        for (float r : {0.15f, 0.5f}) {
            Penumbra::SetRadius(r);
            for (float x = 2.0f; x < 8.0f; x += 0.005f) diff = std::max(diff, std::fabs(SegShare(c, 0, lx, ly, lz, x, ly, 6.0f) - grid(2.0f)(x)));
        }
        Check(diff < 0.03f, "penumbra: the outside walls' copy matches the grid walk");
        Penumbra::SetRadius(0.15f);
        const EdgeScan sc = ScanEdge([&](float x) { return SegShare(c, 0, lx, ly, lz, x, ly, 7.0f); }, 1.5f, 4.0f, 0.001f);
        Check(sc.rising && sc.maxStep < 0.02f, "penumbra: the copy's light rises smoothly across the jamb");
    }
    // a wall's free end softens too; a closed corner never leaks
    {
        Builder f(12, 12);
        for (int x = 0; x < 3; x++) f.Wall(true, x, 4);
        Penumbra::SetRadius(0.15f);
        const auto end = [&](float d) { return [&, d](float x) { return Pass(f.g, lx, ly, lz, x, ly, 4.0f + d); }; };
        const float e1 = edgeAt(3.0f, 1.0f), e4 = edgeAt(3.0f, 4.0f);
        const EdgeScan s1 = ScanEdge(end(1.0f), e1 - 0.6f, e1 + 0.6f, 0.001f), s4 = ScanEdge(end(4.0f), e4 - 0.6f, e4 + 0.6f, 0.001f);
        Check(s1.rising && s4.rising && s4.Width() > 1.7f * s1.Width(), "penumbra: a wall's free end casts a soft edge, wider farther behind");
        Check(end(4.0f)(e4 - 0.02f) > 0.05f && end(4.0f)(e4 + 0.02f) < 0.95f, "penumbra: the free end's spot straddles its shadow line");
        Builder c(12, 12);
        c.Colonnade(false);
        Penumbra::SetRadius(0.5f);
        float leak = 0.0f;
        for (float v = 2.0f; v <= 10.0f; v += 0.01f) {
            leak = std::max(leak, Pass(c.g, 6.0f, 1.5f, 6.0f, v, 1.0f, 3.0f));
            leak = std::max(leak, Pass(c.g, 6.0f, 1.5f, 6.0f, 3.0f, 1.0f, v));
            leak = std::max(leak, Pass(c.g, 6.0f, 1.5f, 6.0f, v, 1.0f, 9.0f));
            leak = std::max(leak, Pass(c.g, 6.0f, 1.5f, 6.0f, 9.0f, 1.0f, v));
        }
        Check(leak == 0.0f, "penumbra: a closed room's corners leak no light, however wide the lamp");
    }
    // a light map's texels: the edge is spread over 1.5 texels at the point even right behind the jamb
    {
        Penumbra::SetRadius(0.15f);
        t_mapAA = 0.75f * 0.25f; // class 2: 4 texels a metre
        const float e = edgeAt(3.2f, 0.3f);
        const EdgeScan sc = ScanEdge(grid(0.3f), e - 0.6f, e + 0.6f, 0.001f);
        t_mapAA = 0.0f;
        Check(sc.rising && sc.Width() > 0.2f, "penumbra: on a light map the edge spans 1.5 texels (no texel stairs)");
    }
    Penumbra::SetRadius(Penumbra::kDefaultRadius);
}

// The lot ground shade's soft edges (ground_lamp_shadow.h, Grid::soft; 14/10: its edges on grass stepped with the mask's 4
// texels a metre): two walls in a row along z = 5 (x 0..3 and 3..6, a joint at x = 3, free ends at 0 and 6), 3 m tall, a
// lamp 2 m high at (3, 2); the ground 3 m behind the wall
void GroundSoftChecks() {
    using namespace GroundShadow;
    std::vector<Wall> walls = {Wall{0.0f, 5.0f, 3.0f, 5.0f, 0.0f, 3.0f}, Wall{3.0f, 5.0f, 6.0f, 5.0f, 0.0f, 3.0f}};
    MarkFreeEnds(walls);
    Check(walls[0].free0 && !walls[0].free1 && !walls[1].free0 && walls[1].free1, "ground soft: free ends and the joint found");
    const Lamp L{3.0f, 2.0f, 2.0f, 1.0f};
    Penumbra::SetRadius(0.15f);
    const auto vis = [&](float x, float z, bool soft) {
        float v = 1.0f;
        for (const Wall& w : walls) v *= std::max(Pass(L, x, 0.0f, z, w, nullptr, soft), 0.0f);
        return v;
    };
    // the free end's shadow line on z = 8: from the lamp through (6, 5): x = 3 + 3 x 6 / 3 = 9
    const EdgeScan hard = ScanEdge([&](float x) { return vis(x, 8.0f, false); }, 8.0f, 10.0f, 0.001f);
    const EdgeScan soft = ScanEdge([&](float x) { return vis(x, 8.0f, true); }, 8.0f, 10.0f, 0.001f);
    std::printf("ground soft: the free end's edge 3 m behind the wall: hard %.3f m, soft %.3f m (10-90%%)\n", hard.Width(), soft.Width());
    Check(hard.Width() >= 0.0f && hard.Width() < 0.01f, "ground soft: off, the edge stays hard (as before)");
    Check(soft.rising && soft.lo == 0.0f && soft.hi >= 0.999f && soft.maxStep < 0.01f, "ground soft: the light rises smoothly past the free end");
    Check(soft.Width() > 2.0f * kSoftAA * 0.6f, "ground soft: the edge spans at least the mask's 1.5 texels");
    Check(std::fabs(vis(9.0f, 8.0f, true) - 0.5f) < 0.1f, "ground soft: the shadow line lies at the middle of the penumbra");
    float joint = 0.0f, behind = 0.0f;
    for (float x = 0.5f; x <= 5.5f; x += 0.01f) behind = std::max(behind, vis(x, 5.6f, true));
    for (float z = 5.2f; z <= 12.0f; z += 0.01f) joint = std::max(joint, vis(3.0f, z, true));
    Check(joint == 0.0f && behind == 0.0f, "ground soft: no light through the joint, none right behind the wall");
    // the wall's top: a lamp above a low wall (1 m), the ground behind it near where the top's shadow ends
    std::vector<Wall> low = {Wall{-5.0f, 5.0f, 15.0f, 5.0f, 0.0f, 1.0f}};
    MarkFreeEnds(low);
    const Lamp hi{3.0f, 3.0f, 2.0f, 1.0f};
    const EdgeScan top = ScanEdge([&](float z) { return std::max(Pass(hi, 3.0f, 0.0f, z, low[0], nullptr, true), 0.0f); }, 5.1f, 9.0f, 0.001f);
    Check(top.rising && top.maxStep < 0.01f && top.Width() > 0.1f, "ground soft: the shadow of a low wall's top ends softly");
    // the job: soft off gives the mask it gave before (the same Pass), on fills a whole mask without NaN
    Grid g;
    g.w = g.h = 48;
    std::vector<Wall> jw = walls;
    for (bool s : {false, true}) {
        g.soft = s;
        ShadeJob job;
        job.Init(g, {Lamp{3.0f, 2.0f, 2.0f, 2.0f}}, jw);
        job.Step([] { return true; });
        int dark = 0;
        for (uint8_t v : job.Result()) dark += v < 128;
        Check(job.Done() && job.Result().size() == 48u * 48u && dark > 20, s ? "ground soft: the soft mask builds" : "ground soft: the hard mask builds");
    }
    Penumbra::SetRadius(Penumbra::kDefaultRadius);
}

// The ground light (atlas) on fence tops and outdoor objects (ground_light_gate.h; 14/10, F8 20-50-03: a red post under an
// arcade's slab, head 54.2 m; the story-2 parapets over it, about 57.7 m, beside indoor lamps at 59.1 m shining out of the
// house, took the post's ground pool on their tops)
void GroundGateChecks() {
    using namespace GroundLightGate;
    const float lotGround = 16.7266541f, cornice = 26.85f;
    const float pool = GroundShadow::GroundPoolHeightGate(cornice, lotGround);
    Check(pool == 0.0f && pool * HeightGate(cornice, 25.58f) == pool * HeightGate(cornice, 28.58f),
          "captured cornice neighbours cannot receive different ground pools from their head lists");
    Check(GroundShadow::GroundPoolHeightGate(lotGround, lotGround) == 1.0f &&
          GroundShadow::GroundPoolHeightGate(lotGround + 0.75f, lotGround) == 1.0f,
          "ground objects and columns on a foundation retain their pool strength");
    Check(std::fabs(GroundShadow::GroundPoolHeightGate(lotGround + 1.125f, lotGround) - 0.5f) < 1e-5f,
          "ground pool fades continuously at the existing ground-height boundary");
    const float capturedGround = 16.7266541f, capturedRun = 22.7266846f;
    const float upperPool = GroundShadow::GroundPoolHeightGate(capturedRun, capturedGround);
    Check(upperPool == 0.0f && GroundShadow::StructureLampGain(upperPool, 0.30f) == 0.30f,
          "captured upper instanced trim matches the adjacent object's lamp gain without a ground pool");
    for (float y : {capturedGround, capturedGround + 0.75f, capturedGround + 1.0f})
        Check(GroundShadow::StructureLampGain(GroundShadow::GroundPoolHeightGate(y, capturedGround), 0.30f) == 1.0f,
              "ground runs and foundation stairs retain their fence lamp gain");
    Check(std::fabs(GroundShadow::StructureLampGain(0.5f, 0.30f) - 0.65f) < 1e-6f,
          "structure gain blends continuously across the ground-height band");
    Check(GroundShadow::StructureLampGain(upperPool, 1.0f) == 1.0f,
          "neutral architectural gain preserves elevated direct lamps");
    const float post[3] = {1034.0f, 54.2f, 1274.5f}, indoor[3] = {1036.0f, 59.1f, 1270.0f};
    const float parapetY = 57.7f, curbY = 52.9f, reach = 15.0f;
    // the old rule: every head around raised the gate
    float oldTop = -1e9f;
    NoteHead(oldTop, post[1], 0.0f, 2.0f, reach, true);
    NoteHead(oldTop, indoor[1], 2.0f, -2.5f, reach, true);
    Check(HeightGate(parapetY, oldTop) == 1.0f, "ground gate: an indoor lamp counted as an atlas lamp opens the parapet tops (the bug)");
    float top = -1e9f;
    NoteHead(top, post[1], 0.0f, 2.0f, reach, true);
    NoteHead(top, indoor[1], 2.0f, -2.5f, reach, false); // shining out of a house: not in the atlas
    Check(top == post[1], "ground gate: only the atlas lamps' heads count");
    Check(HeightGate(parapetY, top) == 0.0f, "ground gate: a parapet 3.5 m over the post's head takes none of its pool");
    Check(HeightGate(curbY, top) == 1.0f, "ground gate: a curb under the head keeps it all");
    Check(std::fabs(HeightGate(post[1] + 0.75f, top) - 0.5f) < 1e-5f, "ground gate: half way through the 1.5 m fade");
    float far = -1e9f;
    NoteHead(far, post[1], 20.0f, 0.0f, reach, true);
    Check(HeightGate(parapetY, far) == 1.0f, "ground gate: a lamp beyond reach notes no head (no lamp near: as before)");
    // the share past walls and slabs: a railing behind the arcade's slab (the post left out, v 0) takes none of the atlas
    Share slab;
    slab.Add(0.8f, 0.0f, true);
    Check(slab.Value() == 0.0f, "ground share: a group that sees no atlas lamp takes no ground light");
    Share half;
    half.Add(1.0f, 1.0f, true);
    half.Add(1.0f, 0.0f, true);
    half.Add(5.0f, 1.0f, false); // an indoor lamp shining out: not in the atlas, not in its share
    Check(std::fabs(half.Value() - 0.5f) < 1e-5f, "ground share: weighted by the atlas lamps only");
    Share none;
    Check(none.Value() == 1.0f, "ground share: no atlas lamp near keeps the atlas (the game's ground light)");
    Share edge;
    edge.Add(1.0f, 0.4f, true);
    Check(std::fabs(edge.Value() - 0.4f) < 1e-5f, "ground share: a slab's edge (penumbra) keeps its share");
}

// A lamp of a lower story taken by a room above through a stairwell (IndoorPassImpl, indoor_lamp_walls): the stretch from the
// bulb to where its ray crosses the floor above is walked on the lamp story's grid. Lamp room x 0..4, stair room x 4..8 with
// the opening over tiles x 5..6, z 1..2 (no ceiling there); wall x = 4 between them with an open passage in its cell z 0..1.
void StairwellChecks() {
    Builder b(10, 6);
    b.Room(0, 0, 4, 4, 8, true);
    b.Room(4, 0, 8, 4, 4, true);
    for (int z = 1; z < 3; z++)
        for (int x = 5; x < 7; x++) b.g.flags[static_cast<size_t>(z) * b.g.w + x] &= 0x0F; // the opening: no ceiling
    for (int z = 0; z < 4; z++) {
        const int32_t w = b.Wall(false, 4, z);
        if (z == 0) b.Open(w, kCutArch, 0.0f, 2.1f, 0.05f, 0.95f);
    }
    const auto hit = [&](float ax, float ay, float az, float bx, float by, float bz) {
        const float A[3] = {ax, ay, az}, B[3] = {bx, by, bz};
        return GridWallHitHard(b.g, A, B);
    };
    // red lamp of the neighbouring room, 0.8 m under the floor above, to the middle of the opening: crosses the wall at 2.66 m
    Check(hit(2.0f, 2.2f, 2.0f, 5.5f, 3.0f, 2.0f), "stairwell: a lamp behind the wall beside the opening is stopped");
    Check(hit(3.6f, 2.2f, 1.2f, 5.2f, 3.0f, 1.3f), "stairwell: also from right behind the wall");
    // the case the soft walk let through (a capture): the wall crossed 6 cm under its top, where the floor above begins
    Check(hit(3.4f, 2.2f, 2.0f, 4.05f, 3.0f, 2.0f), "stairwell: a ray crossing the wall just under its top is stopped");
    // the stair room's own lamp: nothing in the way, the light goes up
    Check(!hit(6.5f, 2.2f, 3.5f, 5.5f, 3.0f, 2.0f), "stairwell: a lamp of the room under the opening keeps its light");
    // through the passage at 1.44 m (head 2.1 m): the light goes on
    Check(!hit(3.0f, 1.0f, 0.5f, 7.5f, 3.0f, 0.5f), "stairwell: a ray through a real passage keeps its light");
    // the same passage, a ray over its head: stopped
    Check(hit(2.0f, 2.2f, 0.5f, 5.5f, 3.0f, 0.5f), "stairwell: a ray over the passage's head is stopped");
    // a full wall drawn 1 cm short of the floor above (a capture): a ray 4 mm under that floor is still stopped when the
    // stretch's end is on it (capY); without capY it would pass over the drawn top
    Builder s(10, 6);
    s.Room(0, 0, 8, 4, 4, true);
    for (int z = 0; z < 4; z++) {
        const int32_t w = s.Wall(false, 4, z);
        GridEdge& e = s.g.edges[static_cast<size_t>(w)];
        e.topL = e.topM = e.topR = 2.99f;
    }
    const float g0[3] = {0.0f, 2.2f, 2.0f}, g1[3] = {4.02f, 3.0f, 2.0f};
    Check(!GridWallHitHard(s.g, g0, g1), "stairwell: (the drawn top alone lets a ray 4 mm under the floor above over the wall)");
    Check(GridWallHitHard(s.g, g0, g1, 3.0f), "stairwell: a full wall counts up to the floor above: that ray is stopped");
    // a half wall (top 1 m): a ray over it keeps its light, one under its top is stopped
    Builder h(10, 6);
    h.Room(0, 0, 8, 4, 4, true);
    for (int z = 0; z < 4; z++) {
        const int32_t w = h.Wall(false, 4, z);
        GridEdge& e = h.g.edges[static_cast<size_t>(w)];
        e.topL = e.topM = e.topR = 1.0f, e.half = true;
    }
    const float o0[3] = {2.0f, 2.2f, 2.0f}, o1[3] = {5.5f, 3.0f, 2.0f}, u0[3] = {2.0f, 0.5f, 2.0f}, u1[3] = {5.5f, 0.6f, 2.0f};
    Check(!GridWallHitHard(h.g, o0, o1), "stairwell: a ray over a half wall keeps its light");
    Check(!GridWallHitHard(h.g, o0, o1, 3.0f), "stairwell: also with the floor above given (a half wall keeps its own top)");
    Check(GridWallHitHard(h.g, u0, u1), "stairwell: a ray under a half wall's top is stopped");
}

// A wall the slab stands on (hard_wall_tops): a lamp of the room beside a stairwell, 0.8 m under the slab, and a point of the
// stairwell's ceiling just past the wall (a capture: 6 to 36 % through the wall's soft top, the map texel's
// spread). Lamp room x 0..4 under a ceiling at 3 m; wall x = 4 drawn to 2.99 m; the stairwell tile beyond has no ceiling.
void HardTopChecks() {
    Builder b(10, 6);
    b.Room(0, 0, 4, 4, 3, true);
    b.Room(4, 0, 8, 4, 4, false);
    for (int z = 0; z < 4; z++) {
        GridEdge& e = b.g.edges[static_cast<size_t>(b.Wall(false, 4, z))];
        e.topL = e.topM = e.topR = 2.99f;
    }
    t_mapAA = 0.75f * 0.25f;
    g_hardWallTops.store(false);
    const float soft = Pass(b.g, 2.0f, 2.2f, 2.0f, 4.12f, 2.99f, 1.6f);
    g_hardWallTops.store(true);
    GridWhy why{};
    const float hard = Pass(b.g, 2.0f, 2.2f, 2.0f, 4.12f, 2.99f, 1.6f, &why);
    std::printf("hard tops: through the soft top %.3f, with the slab on the wall %.3f\n", soft, hard);
    Check(soft > 0.05f, "hard tops: the soft top lets the light through the slab's edge (reproduced)");
    Check(hard == 0.0f && why.wall, "hard tops: a full wall under the lamp side's ceiling stops it");
    // over a half wall the soft top stays; and a lamp in a roofless yard keeps the soft top of a full wall
    Builder h(10, 6);
    h.Room(0, 0, 4, 4, 3, true);
    h.Room(4, 0, 8, 4, 4, false);
    for (int z = 0; z < 4; z++) {
        GridEdge& e = h.g.edges[static_cast<size_t>(h.Wall(false, 4, z))];
        e.topL = e.topM = e.topR = 1.0f, e.half = true;
    }
    Check(Pass(h.g, 2.0f, 0.95f, 2.0f, 6.0f, 1.05f, 2.0f) > 0.0f, "hard tops: a half wall keeps its soft top");
    Builder y(10, 6);
    y.Room(0, 0, 4, 4, 3, false);
    for (int z = 0; z < 4; z++) {
        GridEdge& e = y.g.edges[static_cast<size_t>(y.Wall(false, 4, z))];
        e.topL = e.topM = e.topR = 2.99f;
    }
    Check(Pass(y.g, 2.0f, 2.2f, 2.0f, 4.12f, 2.99f, 1.6f) > 0.05f, "hard tops: from a roofless tile the soft top stays");
    g_hardWallTops.store(false);
    t_mapAA = 0.0f;
}

// An arch whose sill is the floor (a capture: the light map of the room past an arch fell 25 % at its line,
// only at the camera story's class, where the map texel's spread t_mapAA widens every rim): a lamp 2.2 m up, 2 m before
// the arch, and points of the floor just past it. Room x 0..4 and 4..8 roofed; wall x = 4 with an arch over all of z 1..3.
void FloorSillChecks() {
    Builder b(10, 6);
    b.Room(0, 0, 4, 6, 3, true);
    b.Room(4, 0, 8, 6, 4, true);
    for (int z = 0; z < 6; z++) {
        const int32_t w = b.Wall(false, 4, z);
        if (z == 1) b.Open(w, kCutArch, 0.0f, 2.4f, 0.03f, 1.0f);
        if (z == 2) b.Open(w, kCutArch, 0.0f, 2.4f, 0.0f, 0.97f);
    }
    t_mapAA = 0.75f * 0.25f; // class 2
    float worst = 1.0f;
    for (float x : {4.02f, 4.06f, 4.125f, 4.25f, 4.5f}) worst = std::min(worst, Pass(b.g, 2.0f, 2.2f, 2.0f, x, 0.0f, 2.0f));
    std::printf("floor sill: the floor just past the arch keeps at least %.3f of the lamp\n", worst);
    Check(worst >= 0.99f, "floor sill: light through an arch onto the floor right past it is not shaded by a sill at floor level");
    // the head and the jambs keep their soft rims
    const float head = Pass(b.g, 2.0f, 2.2f, 2.0f, 6.0f, 2.6f, 2.0f);
    Check(head < 0.99f, "floor sill: the head keeps its soft rim");
    const float jamb = Pass(b.g, 2.0f, 1.0f, 1.0f, 6.0f, 1.0f, 1.16f); // crosses the wall at z 1.08, 5 cm from the jamb
    Check(jamb > 0.0f && jamb < 1.0f, "floor sill: a jamb keeps its soft rim");
    // a window's sill above the floor stays soft
    Builder wdw(10, 6);
    wdw.Room(0, 0, 4, 6, 3, true);
    wdw.Room(4, 0, 8, 6, 4, true);
    for (int z = 0; z < 6; z++) {
        const int32_t w = wdw.Wall(false, 4, z);
        if (z == 2) wdw.Open(w, kCutWindow, 0.9f, 2.1f, 0.1f, 0.9f);
    }
    const float sill = Pass(wdw.g, 2.0f, 2.2f, 2.5f, 6.0f, -0.4f, 2.5f);
    Check(sill > 0.0f && sill < 1.0f, "floor sill: a window's raised sill keeps its soft rim");
    t_mapAA = 0.0f;
}

// The cone of a lamp's light through an arch onto the next room's floor (a capture: arch (4..6, 25), the green
// lamp 3 m behind it at 2.18 m, the floor map's 0.25 m texels): its edge from the jambs. Returns the largest step between
// texel centres 0.25 m apart across the edge, on the line 1 m and 2 m past the arch, at class 2 (t_mapAA).
float ArchConeStep(const Builder& b, float minSpread, bool print) {
    t_mapAA = 0.75f * 0.25f;
    t_gridMinSpread = minSpread;
    float worst = 0.0f;
    for (float past : {1.0f, 2.0f}) {
        float prev = -1.0f;
        char row[512] = {};
        size_t used = 0;
        for (int i = 0; i <= 24; i++) {
            const float z = -0.875f + 0.25f * static_cast<float>(i); // texel centres
            const float p = Pass(b.g, 1.0f, 2.18f, 2.0f, 4.0f + past, 0.0f, z);
            if (prev >= 0.0f) worst = std::max(worst, std::fabs(p - prev));
            prev = p;
            used += static_cast<size_t>(std::snprintf(row + used, sizeof row - used, " %.2f", p));
        }
        if (print) std::printf("arch cone, spread %.2f, %.0f m past: %s\n", minSpread, past, row);
    }
    t_gridMinSpread = 0.0f;
    t_mapAA = 0.0f;
    return worst;
}
void ArchConeChecks() {
    Builder b(10, 8);
    b.Room(0, 0, 4, 8, 3, true);
    b.Room(4, 0, 8, 8, 4, true);
    for (int z = 0; z < 8; z++) {
        const int32_t w = b.Wall(false, 4, z);
        if (z == 2) b.Open(w, kCutArch, 0.0f, 2.4f, 0.03f, 1.0f);
        if (z == 3) b.Open(w, kCutArch, 0.0f, 2.4f, 0.0f, 0.97f);
    }
    const float hard = ArchConeStep(b, 0.0f, true), soft = ArchConeStep(b, kInnerSoftSpread, true);
    std::printf("arch cone: largest step between floor texels %.3f (lamp radius %.2f), %.3f (soft opening edges, spread %.2f)\n", hard, Penumbra::Radius(), soft,
                kInnerSoftSpread);
    Check(hard > 0.3f, "arch cone: the edge from the jambs is a hard step at the lamp's own radius (reproduced)");
    Check(soft < 0.75f * hard, "arch cone: soft opening edges take a third of the step off (no texel stair)");
    Check(kInnerSoftSpread <= 0.3f, "arch cone: the spread stays narrow enough to keep the shape of the arch or window");
    // the middle of the cone keeps the whole light
    t_gridMinSpread = kInnerSoftSpread;
    const float mid = Pass(b.g, 1.0f, 2.18f, 2.0f, 5.0f, 0.0f, 3.0f);
    t_gridMinSpread = 0.0f;
    Check(mid >= 0.99f, "arch cone: the middle of the cone keeps the whole light");
}

// (2.15.3) Real window sizes, wall thickness, floor-level sills in the outside walls' copy and the window's glass. A wall on
// z = 4 from x = 1 to 11 with one opening in cell x = 5 (u 0.1 .. 0.9: jambs at x 5.1 and 5.9).
struct ThickWall {
    Builder b{12, 12};
    StoryWallCopy c;
    explicit ThickWall(uint8_t kind, float sill = 0.0f, float head = 2.4f) {
        for (int x = 1; x < 11; x++) {
            const int32_t s = b.Wall(true, x, 4);
            if (x == 5) b.Open(s, kind, sill, head, 0.1f, 0.9f);
        }
        b.g.openings[0].glass = kind == kCutWindow ? WallOpenings::kWindowPass : 1.0f; // (as PlaceGridOpenings sets it)
        b.g.xf = Identity();
        c.segs.push_back(WallSeg{1.0f, 4.0f, 11.0f, 4.0f, 0.0f, 3.0f});
        FillWallPatches(b.g, c);
    }
    // the light through the opening for rays at `deg` off the wall's normal, integrated over where they cross the line (m):
    // the beam's width; grid: the room solves' walk, else the render thread's copy
    float Width(float deg, bool grid) {
        const float k = std::tan(deg * 3.14159265f / 180.0f);
        float sum = 0.0f;
        for (float x = 4.6f; x < 6.4f; x += 0.002f) {
            const float A[3] = {x - 2.0f * k, 1.2f, 2.0f}, B[3] = {x + 0.5f * k, 1.2f, 4.5f};
            GridWhy why{};
            sum += 0.002f * (grid ? GridPassImpl(b.g, A, B, why) : SegShare(c, 0, A[0], A[1], A[2], B[0], B[1], B[2]));
        }
        return sum;
    }
};
void OpeningShapeChecks() {
    Penumbra::SetRadius(0.02f);
    {
        // thickness: 0.1 m (two faces 0.05 m off the line, FUN_006AB280); at 60 degrees each jamb takes 0.05 x tan 60 = 0.087 m
        ThickWall t(kCutArch);
        const float g0 = t.Width(0.0f, true), g60 = t.Width(60.0f, true), c0 = t.Width(0.0f, false), c60 = t.Width(60.0f, false);
        std::printf("wall thickness: beam through a 0.8 m arch at 0 / 60 degrees: grid walk %.3f / %.3f m, walls' copy %.3f / %.3f m (expected 0.800 / 0.627)\n", g0, g60, c0,
                    c60);
        Check(std::fabs(g0 - 0.8f) < 0.01f && std::fabs(c0 - 0.8f) < 0.01f, "wall thickness: no narrowing at normal incidence");
        Check(std::fabs(g60 - (0.8f - 2.0f * 0.05f * std::sqrt(3.0f))) < 0.02f, "wall thickness: the grid walk's beam narrows by 2 x 0.05 x tan 60 at 60 degrees");
        Check(std::fabs(c60 - g60) < 0.02f, "wall thickness: the walls' copy narrows the beam as the grid walk does");
        // a ray crossing the line 5 cm inside the jamb at 60 degrees meets the jamb at the far face; straight on it passes
        const float k = std::sqrt(3.0f);
        Check(Pass(t.b.g, 5.85f - 2.0f * k, 1.2f, 2.0f, 5.85f + 0.5f * k, 1.2f, 4.5f) < 0.05f, "wall thickness: a grazing ray near the jamb is stopped by the wall's depth");
        Check(Pass(t.b.g, 5.85f, 1.2f, 2.0f, 5.85f, 1.2f, 4.5f) >= 0.99f, "wall thickness: the same crossing straight on passes");
        // a crossing far from any opening gives what it did
        Check(Pass(t.b.g, 8.5f - 2.0f * k, 3.5f, 2.0f, 8.5f + 0.5f * k, 3.5f, 4.5f) >= 0.99f && Pass(t.b.g, 8.5f - 2.0f * k, 1.2f, 2.0f, 8.5f + 0.5f * k, 1.2f, 4.5f) == 0.0f,
              "wall thickness: walls without an opening as before (over the top passes, through the wall stops)");
    }
    {
        // glass: a window passes kWindowPass at every crossing of its hole, the beam's width x that share
        ThickWall t(kCutWindow, 0.85f, 2.25f);
        const float g0 = t.Width(0.0f, true), c0 = t.Width(0.0f, false);
        std::printf("window glass: %.3f of the light (glass %.2f x frame %.2f); beam x share %.3f grid walk, %.3f walls' copy\n", WallOpenings::kWindowPass,
                    WallOpenings::kGlassPass, WallOpenings::kFrameOpen, g0, c0);
        Check(std::fabs(g0 - 0.8f * WallOpenings::kWindowPass) < 0.01f && std::fabs(c0 - 0.8f * WallOpenings::kWindowPass) < 0.01f, "window glass: the window passes its share");
        ThickWall a(kCutArch);
        Check(a.Width(0.0f, true) > 0.79f && a.Width(0.0f, false) > 0.79f, "window glass: an arch passes all of it");
    }
    {
        // floor-level sill in the walls' copy: an outdoor lamp through an arch onto the ground just past it (render thread and
        // the ground shade), with the flag FillWallPatches sets and without it (as before)
        ThickWall t(kCutArch);
        Check(t.c.patches.size() == 1 && t.c.patches[0].floorSill == 1 && std::fabs(t.c.patches[0].glass - 1.0f) < 1e-6f, "floor sill: the arch's patch stands on the floor");
        StoryWallCopy old = t.c;
        old.patches[0].floorSill = 0;
        GroundShadow::Lamp L{5.5f, 2.2f, 2.0f, 1.0f};
        GroundShadow::Wall w{1.0f, 4.0f, 11.0f, 4.0f, 0.0f, 3.0f, 0, 1};
        float worst = 1.0f, worstOld = 1.0f, soft = 1.0f, softOld = 1.0f;
        for (float z : {4.02f, 4.06f, 4.125f, 4.25f, 4.5f}) {
            worst = std::min(worst, SegShare(t.c, 0, 5.5f, 2.2f, 2.0f, 5.5f, 0.0f, z));
            worstOld = std::min(worstOld, SegShare(old, 0, 5.5f, 2.2f, 2.0f, 5.5f, 0.0f, z));
            soft = std::min(soft, GroundShadow::Pass(L, 5.5f, 0.0f, z, w, t.c.patches.data(), true));
            softOld = std::min(softOld, GroundShadow::Pass(L, 5.5f, 0.0f, z, w, old.patches.data(), true));
        }
        std::printf("floor sill (walls' copy): the ground just past the arch keeps %.3f of the lamp (before %.3f); ground shade %.3f (before %.3f)\n", worst, worstOld, soft,
                    softOld);
        Check(worst >= 0.99f && soft >= 0.99f, "floor sill: the walls' copy and the ground shade do not shade the ground past an arch with a sill at the floor");
        ThickWall wdw(kCutWindow, 0.85f, 2.25f);
        Check(wdw.c.patches[0].floorSill == 0 && std::fabs(wdw.c.patches[0].glass - WallOpenings::kWindowPass) < 1e-6f, "floor sill: a window's raised sill keeps its rim");
    }
    Penumbra::SetRadius(Penumbra::kDefaultRadius);
    {
        // real window sizes: a window in cell x = 5 of DoorRule's wall z = 4 (a closed door beside it) with a pane in front of
        // each face (half extents 0.3 along, 0.5 up, centred 1.5 m up: u 0.2 .. 0.8, sill 1.0, head 2.0)
        const GlassLight in{5.5f, 1.5f, 3.95f, 0.3f, 0.5f, 0.0f}, out{5.5f, 1.5f, 4.05f, 0.3f, 0.5f, 0.0f};
        DoorRule plain(kCutWindow, kCutDoor, true);
        PaneShapes panes;
        WindowPanes(plain.b.g, plain.cuts, {in, out}, panes);
        Check(panes.size() == 1 && panes.count(EdgeKey(5, 4, 6, 4)), "window size: the window's cell takes its panes' shape");
        DoorRule d(kCutWindow, kCutDoor, true);
        d.b.g.openings.clear(), d.b.g.cutsOnWall = d.b.g.cutsClosed = d.b.g.glassDoors = 0;
        for (int& k : d.b.g.kinds) k = 0;
        for (GridEdge& e : d.b.g.edges) e.open = 0;
        PlaceGridOpenings(d.b.g, d.cuts, true, {}, [](const CutRec&, GridOpening&) {}, &panes);
        const GridOpening& o = d.b.g.openings[0];
        std::printf("window size from its panes: u %.2f .. %.2f, sill %.2f, head %.2f (defaults u 0.10 .. 0.90, sill 0.85, head 2.25)\n", o.u0, o.u1, o.sill, o.head[2]);
        Check(o.shaped == 2 && std::fabs(o.u0 - 0.2f) < 1e-4f && std::fabs(o.u1 - 0.8f) < 1e-4f && std::fabs(o.sill - 1.0f) < 1e-4f && std::fabs(o.head[0] - 2.0f) < 1e-4f &&
                  std::fabs(o.head[4] - 2.0f) < 1e-4f,
              "window size: jambs, sill and head from the panes' rectangle");
        Check(d.b.g.paneSized == 1 && d.b.g.windowCuts.size() == 1 && d.b.g.paneSig == PaneSig(panes) && d.b.g.paneSig != PaneSig({}), "window size: counted and stamped");
        // a ray 0.9 m up passes the default sill (0.85) but not the panes' (1.0); 2.1 m up the default head (2.25) but not the panes'
        Check(Glass(Pass(d.b.g, 5.5f, 1.5f, 1.0f, 5.5f, 1.5f, 6.5f)) && Pass(d.b.g, 5.5f, 0.9f, 1.0f, 5.5f, 0.9f, 6.5f) < 0.05f &&
                  Pass(d.b.g, 5.5f, 2.1f, 1.0f, 5.5f, 2.1f, 6.5f) < 0.05f && Pass(d.b.g, 5.15f, 1.5f, 1.0f, 5.15f, 1.5f, 6.5f) < 0.05f,
              "window size: light through the glass only, not the defaults' larger hole");
        Check(Pass(plain.b.g, 5.5f, 0.9f, 1.0f, 5.5f, 0.9f, 6.5f) > 0.5f, "window size: with no pane the defaults stay (0.9 m up passes)");
        // panes that do not belong to the window: on a perpendicular wall (extent across the line), in the next cell, a size
        // out of the limits, no size read; a pane moved changes the stamp (EnsureStoryGrid builds the grid again)
        PaneShapes none;
        WindowPanes(plain.b.g, plain.cuts, {GlassLight{5.5f, 1.5f, 3.95f, 0.0f, 0.5f, 0.3f}}, none);
        Check(none.empty(), "window size: a pane across the wall's line is not this window's");
        WindowPanes(plain.b.g, plain.cuts, {GlassLight{6.5f, 1.5f, 4.05f, 0.3f, 0.5f, 0.0f}}, none);
        Check(none.empty(), "window size: a pane in the next cell is not this window's");
        WindowPanes(plain.b.g, plain.cuts, {GlassLight{5.5f, 1.5f, 4.05f, 0.3f, 0.05f, 0.0f}}, none);
        Check(none.empty(), "window size: a pane under 0.15 m high leaves the defaults");
        WindowPanes(plain.b.g, plain.cuts, {GlassLight{5.5f, 1.5f, 4.05f}}, none);
        Check(none.empty(), "window size: a window light with no size read leaves the defaults");
        PaneShapes moved;
        WindowPanes(plain.b.g, plain.cuts, {GlassLight{5.5f, 1.6f, 4.05f, 0.3f, 0.5f, 0.0f}}, moved);
        Check(moved.size() == 1 && PaneSig(moved) != PaneSig(panes), "window size: a pane moved changes the stamp");
    }
}

void RaisedWallSpanChecks() {
    using namespace WallSampleSpan;
    const float base = 20.708f, foot = 22.947f, top = 23.923f;
    const float height = top - foot;
    Check(RaisedCell(foot, base, height, height, height), "raised gable exceeds the native light span");
    Check(!RaisedCell(19.951f, 19.958f, 2.996f, 2.996f, 2.996f), "wall below the gable keeps its native span");
    Check(RaisedCell(1.0f, 0.0f, 1.0f, 1.0f, 1.0f), "short raised cells use their foot even below the native top");
    Check(!RaisedCell(1.0f, 0.0f, 3.1f, 3.1f, 3.1f), "tall wall retains the existing stretch path");
    Check(!RaisedCell(2.3f, 0.0f, 0.1f, 1.0f, 1.0f), "unsupported thin edge retains the native mapping");
    Check(!RaisedCell(4.0f, 0.0f, 1.0f, 1.0f, 1.0f), "distant span is not reinterpreted");
    Check(ShiftFoot(-1.9f, 0.0f, 1.0f, 1.0f, 1.0f), "existing lower-foot correction remains active");
    Check(!ShiftFoot(0.0f, 0.0f, 3.0f, 3.0f, 3.0f), "ordinary wall keeps its span");
    Check(RaisedCell(2.3f, 0.0f, 0.5f, 0.5f, 0.5f) && RaisedCell(2.3f, 0.0f, 1.0f, 1.0f, 1.0f),
          "a gable height edit retains the same reference at its neighbours");
    // The native clamp cannot represent the captured top. Moving the sample base
    // and UV base together preserves world height through the whole drawn face.
    const float oldTop = base + 3.0f * std::clamp((top - base) / 3.0f, 0.0f, 1.0f);
    Check(top - oldTop > 0.2f, "native top clamps more than twenty centimetres below the face");
    for (int i = 0; i <= 1000; ++i) {
        const float y = foot + height * static_cast<float>(i) / 1000.0f;
        const float uv = std::clamp((y - foot) / 3.0f, 0.0f, 1.0f);
        const float sampledY = foot + 3.0f * uv;
        Check(std::fabs(sampledY - y) < 1e-5f, "raised wall reads light at its drawn world height");
    }
    for (int i = -400; i <= 400; ++i) {
        const float f = static_cast<float>(i) / 100.0f;
        const bool old = f < -0.05f && f > -3.2f;
        if (f <= 0.0f) Check(ShiftFoot(f, 0.0f, 1.0f, 1.0f, 1.0f) == old, "lower-foot policy remains equivalent");
    }
}

void PillarLightChecks() {
    const float cap[2][4] = {{1, 0, 0, 0.497f}, {0, 1, 0, 0}};
    const float head[3] = {0, 2.372f, 0};
    const float behind[3] = {0.6946f, 0.75f, 0.7919f}, clear[3] = {3.0f, 0.75f, 3.0f}, above[3] = {0, 2.0f, 0};
    Check(LampCapShadow::Pass(cap, head, behind, 0.15f) < 0.01f, "captured deck ray crosses the pillar's opaque cap");
    Check(LampCapShadow::Pass(cap, head, clear, 0.15f) > 0.99f, "a stair pixel past the cap's shadow keeps its lamp");
    Check(LampCapShadow::Pass(cap, head, above, 0.15f) == 1.0f, "the cap never blocks a receiver above it");
    const float disabled[2][4]{};
    Check(LampCapShadow::Pass(disabled, head, behind, 0.15f) == 1.0f, "an unknown or incomplete cap leaves lighting unchanged");
    WallTopCell top;
    Check(!MeasuredShortTop(top), "unmeasured wall retains its native blocker");
    top.set = true;
    top.foot = 0.739f;
    Check(KeepWallTop(top, 1.125f, 1.125f, 1.125f) && MeasuredShortTop(top), "captured pillar keeps its real short top");
    Check(!KeepWallTop(top, 0.5f, 0.5f, 0.5f) && top.hl == 1.125f, "lower geometry cannot shrink a known blocker");
    Builder b(8, 8);
    const auto idx = b.Wall(true, 3, 4);
    auto& e = b.g.edges[idx];
    e.foot = e.base = top.foot;
    e.topL = top.foot + top.hl, e.topM = top.foot + top.hm, e.topR = top.foot + top.hr;
    e.shortTop = MeasuredShortTop(top);
    GridWhy why;
    Check(Pass(b.g, 3.5f, 2.372f, 3.0f, 3.5f, 1.5f, 5.0f, &why) > 0.5f && why.shortTop,
          "lamp above captured pillar reaches deck past its physical top");
    why = {};
    Check(Pass(b.g, 3.5f, 1.5f, 3.0f, 3.5f, 1.0f, 5.0f, &why) < 0.01f && !why.shortTop,
          "the same pillar still blocks light through its solid side");
    StoryWallCopy copy;
    copy.segs.push_back(WallSeg{});
    copy.segs[0].x0 = 3.0f, copy.segs[0].x1 = 4.0f;
    copy.segs[0].z0 = copy.segs[0].z1 = 4.0f;
    copy.segs[0].y0 = 0.0f, copy.segs[0].y1 = 3.0f;
    b.g.xf = Identity();
    FillWallPatches(b.g, copy);
    Check(copy.lowered == 1 && copy.patches.size() == 1 && std::fabs(copy.patches[0].top0 - 1.864f) < 0.001f,
          "ground and object shadow copy uses the captured pillar top");
    Check(KeepWallTop(top, 3.0f, 3.0f, 3.0f) && !MeasuredShortTop(top), "full wall replaces a previously short measure");
    e.topL = e.topM = e.topR = 3.0f, e.shortTop = false;
    why = {};
    Check(Pass(b.g, 3.5f, 2.372f, 3.0f, 3.5f, 1.5f, 5.0f, &why) < 0.01f && !why.shortTop,
          "ordinary full wall still blocks the porch ray");
    Check(KeepWallTop(top, 4.0f, 3.5f, 3.2f) && top.tall && !MeasuredShortTop(top), "tall gable protection remains active");
    top = {};
    KeepWallTop(top, 3.01f, 3.01f, 3.01f);
    Check(KeepWallTop(top, 3.025f, 3.025f, 3.025f) && top.tall, "crossing the tall threshold always invalidates the grid");
}

int main() {
    PillarLightChecks();
    RaisedWallSpanChecks();
    ArchConeChecks();
    FloorSillChecks();
    StairwellChecks();
    HardTopChecks();
    GroundGateChecks();
    PenumbraChecks();
    GroundSoftChecks();
    DoorRuleChecks();
    GlassDoorChecks();
    FoundationChecks();
    InnerChecks();
    InnerRetryChecks();
    OpeningLawChecks();
    InnerNormChecks();
    Builder b(12, 12);
    b.Colonnade(true);
    const StoryGrid& g = b.g;
    YardOutline o;
    std::vector<RoofedDoor> doors;
    RoofedDoors(g, 48, nullptr, 0, o, doors);
    Check(o.any && o.boundary.size() == 16, "the room's outline: its 16 wall edges");
    Check(doors.size() == 2, "the arch's two cells are the room's doors to room 0");
    Check(o.x0 == 4.0f && o.x1 == 8.0f && o.z0 == 4.0f && o.z1 == 8.0f, "the outline's box");
    const uint64_t sig = DoorSig(g, doors);
    Check(sig != 0, "doors give a signature");

    // a garden post in front of the arch sees its hole; one behind the room or beside it does not
    const float front[3] = {6.0f, 1.8f, 1.5f}, behind[3] = {6.0f, 1.8f, 10.5f}, side[3] = {1.5f, 1.8f, 6.0f};
    float seen = 0.0f, seenBehind = 0.0f, seenSide = 0.0f;
    for (const RoofedDoor& d : doors) {
        seen = std::max(seen, DoorSeen(g, nullptr, front, d));
        seenBehind = std::max(seenBehind, DoorSeen(g, nullptr, behind, d));
        seenSide = std::max(seenSide, DoorSeen(g, nullptr, side, d));
    }
    Check(seen >= 0.99f, "a lamp in front of the arch sees it");
    Check(seenBehind == 0.0f, "a lamp behind the room does not (its back wall)");
    Check(seenSide == 0.0f, "a lamp beside the room does not (its side wall)");
    // a lamp of the story below (a garden post beside a house on a foundation): also tested on that story's walls. Open
    // ground there: it sees the arch; a wall of the story below between it and the arch (a garden wall): it does not
    {
        Builder below(12, 12);
        float open = 0.0f, walled = 0.0f;
        for (const RoofedDoor& d : doors) open = std::max(open, DoorSeen(g, nullptr, front, d, &below.g));
        for (int x = 3; x < 9; x++) below.Wall(true, x, 2);
        for (const RoofedDoor& d : doors) walled = std::max(walled, DoorSeen(g, nullptr, front, d, &below.g));
        Check(open >= 0.99f, "a lamp of the story below with open ground before the arch sees it");
        Check(walled == 0.0f, "a wall of the story below between that lamp and the arch hides it");
    }

    // the lit patch on the floor through the arch, bounded by the jambs: lamp 8 m in front, points 2.5 m inside
    int lit = 0, darkOutside = 0, litOutside = 0, darkInside = 0;
    for (int i = 1; i < 40; i++) {
        const float x = 4.0f + 0.1f * static_cast<float>(i);
        const float p = Pass(g, 6.0f, 1.8f, -4.0f, x, 0.5f, 6.5f);
        if (x <= 4.6f || x >= 7.4f) (p == 0.0f ? darkOutside : litOutside)++;
        else if (x >= 4.85f && x <= 7.15f) (p >= 0.99f ? lit : darkInside)++;
    }
    Check(lit > 0 && darkInside == 0, "every point in line with the hole is lit");
    Check(darkOutside > 0 && litOutside == 0, "points behind the jambs stay dark");

    // a lamp high over the wall: its ray passes over the wall top, then meets the room's own roof
    GridWhy why{};
    Check(Pass(g, 6.0f, 6.0f, 1.5f, 6.0f, 0.5f, 7.0f, &why) == 0.0f && why.roof, "over the wall top the roof closes the room");
    // a ray under the arch's head from a lamp at head height reaches the back wall's foot
    Check(Pass(g, 6.0f, 2.0f, 1.0f, 6.0f, 0.2f, 7.9f, &why) >= 0.99f && why.opening, "through the arch: why.opening");
    // the same room with no opening: no door (fail closed), every inside point dark
    Builder c(12, 12);
    c.Colonnade(false);
    YardOutline o2;
    std::vector<RoofedDoor> none;
    RoofedDoors(c.g, 48, nullptr, 0, o2, none);
    Check(none.empty() && DoorSig(c.g, none) == 0, "a closed room has no door");
    Check(Pass(c.g, 6.0f, 1.8f, -4.0f, 6.0f, 0.5f, 6.5f) == 0.0f, "a closed room is dark inside");
    // a window (sill 0.85): a low ray under its sill is blocked, one through it passes
    Builder w(12, 12);
    w.Room(4, 4, 8, 8, 48, true);
    for (int x = 4; x < 8; x++) {
        const int32_t s = w.Wall(true, x, 4);
        w.Wall(true, x, 8);
        if (x == 6) w.Open(s, kCutWindow, 0.85f, 2.25f, 0.1f, 0.9f);
    }
    for (int z = 4; z < 8; z++) w.Wall(false, 4, z), w.Wall(false, 8, z);
    Check(Pass(w.g, 6.5f, 0.3f, 1.0f, 6.5f, 0.3f, 6.0f) == 0.0f, "under a window's sill: blocked");
    Check(Pass(w.g, 6.5f, 1.5f, 1.0f, 6.5f, 1.5f, 6.0f) >= 0.99f, "through a window: lit");
    // an opening into another roofed room is no door to the outside; into a roofless room it is
    Builder r(12, 12);
    r.Colonnade(true);
    r.Room(4, 0, 8, 4, 49, true);
    YardOutline o3;
    std::vector<RoofedDoor> inner;
    RoofedDoors(r.g, 48, nullptr, 0, o3, inner);
    Check(inner.empty(), "an arch into another roofed room is not a door to the outside");
    const int roofless[1] = {49};
    YardOutline o4;
    std::vector<RoofedDoor> yard;
    RoofedDoors(r.g, 48, roofless, 1, o4, yard);
    Check(yard.size() == 2, "an arch into a roofless room is a door to the outside");
    // the signature follows the doors (one arch cell closed again: another signature)
    Builder half(12, 12);
    half.Room(4, 4, 8, 8, 48, true);
    for (int x = 4; x < 8; x++) {
        const int32_t s = half.Wall(true, x, 4);
        half.Wall(true, x, 8);
        if (x == 5) half.Open(s, kCutArch, 0.0f, 2.4f, 0.03f, 1.0f);
    }
    for (int z = 4; z < 8; z++) half.Wall(false, 4, z), half.Wall(false, 8, z);
    YardOutline o5;
    std::vector<RoofedDoor> one;
    RoofedDoors(half.g, 48, nullptr, 0, o5, one);
    Check(one.size() == 1 && DoorSig(half.g, one) != sig && DoorSig(half.g, one) != 0, "one arch cell closed: another signature");
    WallCopyChecks();
    OpeningShapeChecks();
    std::printf("Roofed openings: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
