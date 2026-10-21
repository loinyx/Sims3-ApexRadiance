// The second bounce through a stairwell opening (features/stairwell_bounce.h): the geometry of a wall point of the story
// above seeing an opening patch, and the line of sight followed below to the lower room's floor or wall; and the slab edge's
// penumbra (features/penumbra.h, PenumbraShare). Geometry of a house measured with F8 (lot space shifted to world: tiles
// x 22-26, z 30-33 of story 2 open over room 3 of story 1, x 18-30, z 25-38; story 1's floor at 20.49, story 2's at 23.49).
// Read-only; prints the checks and exits 1 on a failure.
#include "features/penumbra.h"
#include "features/stairwell_bounce.h"
#include <cstdio>

namespace {
int checks = 0, failures = 0;
void Check(bool ok, const char* text) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL: %s\n", text);
    }
}
constexpr float kOx = 430.0f, kOz = 510.0f, kLow = 20.49f, kSlab = 23.49f;
// story 1: room 3 over tiles x 18..30, z 25..38; its own stairwell (no floor) x 20..28, z 30..38
int RoomAt(float x, float z) {
    const float lx = x - kOx, lz = z - kOz;
    return lx >= 18.0f && lx < 31.0f && lz >= 25.0f && lz < 39.0f ? 3 : 0;
}
bool FloorAt(float x, float z) {
    const float lx = x - kOx, lz = z - kOz;
    return !(lx >= 20.0f && lx < 29.0f && lz >= 30.0f && lz < 39.0f);
}
} // namespace

int main() {
    using namespace StairwellBounce;
    using Penumbra::PenumbraShare;
    const float up[3] = {0, 1, 0}, white[3] = {1, 1, 1};
    // a patch of the opening, at the slab, under a wall point 1.5 m up and 2 m away facing it
    const float q[3] = {454.5f, kSlab, 541.5f};
    const float facing[3] = {0, 0, -1}, away[3] = {0, 0, 1};
    const float p[3] = {454.5f, kSlab + 1.5f, 543.5f};
    const float g = Geometry(p, facing, q, 0.25f);
    Check(g > 0.0f, "a wall facing the opening sees the patch");
    Check(Geometry(p, away, q, 0.25f) == 0.0f, "a wall facing away sees nothing");
    const float onSlab[3] = {454.5f, kSlab + 0.01f, 543.5f};
    Check(Geometry(onSlab, facing, q, 0.25f) == 0.0f, "a point on the slab's plane (the floor above) takes nothing");
    const float under[3] = {454.5f, kSlab - 0.5f, 543.5f};
    Check(Geometry(under, facing, q, 0.25f) == 0.0f, "a point under the slab takes nothing");
    // cos x cos / r^2: r = 2.5, cosP = 2 / 2.5, cosQ = 1.5 / 2.5
    const float expect = 0.8f * 0.6f * 0.25f / 6.25f;
    Check(std::fabs(g - expect) < 1e-6f, "the geometry is cos x cos x area / r^2");
    const float p2[3] = {454.5f, kSlab + 3.0f, 545.5f}; // twice as far along the same line
    Check(std::fabs(Geometry(p2, facing, q, 0.25f) - expect / 4.0f) < 1e-6f, "it falls as the square of the distance");
    const float beside[3] = {454.5f, kSlab + 0.1f, 541.6f};
    Check(Geometry(beside, facing, q, 0.25f) <= 1.0f, "a point right beside a patch takes a bounded share");
    float add[3] = {};
    Add(p, facing, q, 0.25f, white, add);
    Check(std::fabs(add[0] - expect * kAlbedo / kPi) < 1e-6f && add[0] == add[2], "the irradiance taken is albedo / pi x geometry x below");
    const float nan3[3] = {NAN, -1.0f, 0.0f};
    float none[3] = {};
    Add(p, facing, q, 0.25f, nan3, none);
    Check(none[0] == 0.0f && none[1] == 0.0f && none[2] == 0.0f, "an unreadable or negative colour below adds nothing");

    // the far wall of the house (z 547.95), 1.5 m over the slab, through the middle of the opening: the line goes down
    // towards the lamp's side and meets room 3's north wall (z 535) before its floor
    const float far[3] = {454.5f, kSlab + 1.5f, 547.95f};
    const float mid[3] = {454.5f, kSlab, 542.0f};
    Hit hit;
    const bool met = Follow(far, mid, kLow, 3, RoomAt, FloorAt, hit);
    std::printf("Far wall through the opening's middle: %s at (%.2f %.2f %.2f), normal (%.2f %.2f %.2f)\n", met ? (hit.wall ? "wall" : "floor") : "nothing",
                hit.at[0], hit.at[1], hit.at[2], hit.n[0], hit.n[1], hit.n[2]);
    Check(met && hit.wall, "a shallow line meets the lower room's wall");
    Check(met && hit.at[2] >= 535.0f && hit.at[2] < 535.6f && hit.at[1] > kLow && hit.at[1] < kSlab, "at room 3's north wall (z 535), between its floor and ceiling");
    Check(met && hit.n[2] > 0.99f, "the wall met faces back along the line (+z)");
    // a wall right over the opening's edge, 2 m up: a steep line reaches room 3's floor beside its stairwell
    const float steepP[3] = {452.5f, kSlab + 2.0f, 539.5f};
    const float steepQ[3] = {453.0f, kSlab, 540.5f};
    Hit floorHit;
    const bool down = Follow(steepP, steepQ, kLow, 3, RoomAt, FloorAt, floorHit);
    std::printf("Steep line: %s at (%.2f %.2f %.2f)\n", down ? (floorHit.wall ? "wall" : "floor") : "nothing", floorHit.at[0], floorHit.at[1], floorHit.at[2]);
    // it lands at x 454, z 542 (local 24, 32): inside story 1's own stairwell -> the stair in the well, 1.5 m up, facing up
    Check(down && floorHit.well && !floorHit.wall && std::fabs(floorHit.at[1] - (kLow + kWellLift)) < 1e-4f && floorHit.n[1] == 1.0f, "a line that lands in the lower story's own stairwell meets the stair in it");
    const float steepQ2[3] = {453.0f, kSlab, 539.9f};
    const float steepP2[3] = {453.0f, kSlab + 2.0f, 541.4f}; // looking back towards -z: lands on room 3's floor at z 537.65
    const bool down2 = Follow(steepP2, steepQ2, kLow, 3, RoomAt, FloorAt, floorHit);
    Check(down2 && !floorHit.well, "a line landing on the lower floor is not the well");
    Check(down2 && !floorHit.wall && std::fabs(floorHit.at[1] - kLow) < 1e-4f && floorHit.n[1] == 1.0f, "a line landing on the lower floor meets it, normal up");
    Hit unread;
    Check(!Follow(far, mid, kLow, 3, [](float, float) { return -1; }, FloorAt, unread), "an unreadable room below takes nothing");
    Check(!Follow(under, mid, kLow, 3, RoomAt, FloorAt, unread), "a point under the slab follows nothing");

    // the whole opening (80 quadrants of 0.25 m2) seen from the far wall, with what is below at the irradiance 1: the share
    float total[3] = {};
    for (int z = 30; z < 34; z++)
        for (int x = 22; x < 27; x++)
            for (int k = 0; k < 4; k++) {
                const float cq[3] = {kOx + x + (k == 1 ? 0.8333f : k == 3 ? 0.1667f : 0.5f), kSlab, kOz + z + (k == 0 ? 0.1667f : k == 2 ? 0.8333f : 0.5f)};
                Add(far, facing, cq, 0.25f, white, total);
            }
    std::printf("Far wall, 1.5 m up, the whole opening at irradiance 1 below: %.4f\n", total[0]);
    Check(total[0] > 0.005f && total[0] < 0.05f, "the far wall takes about 1-3% of the light below (it sees the opening at a grazing angle)");
    const float nearWall[3] = {kOx + 27.0f, kSlab + 1.0f, kOz + 32.0f}, nearN[3] = {-1, 0, 0}; // the landing's wall beside the opening
    float nearTotal[3] = {};
    for (int z = 30; z < 34; z++)
        for (int x = 22; x < 27; x++)
            for (int k = 0; k < 4; k++) {
                const float cq[3] = {kOx + x + (k == 1 ? 0.8333f : k == 3 ? 0.1667f : 0.5f), kSlab, kOz + z + (k == 0 ? 0.1667f : k == 2 ? 0.8333f : 0.5f)};
                Add(nearWall, nearN, cq, 0.25f, white, nearTotal);
            }
    std::printf("Wall beside the opening, 1 m up: %.4f\n", nearTotal[0]);
    Check(nearTotal[0] > total[0] * 3.0f && nearTotal[0] < kAlbedo, "a wall beside the opening takes much more, under the albedo");

    // the floor: the flux up through a patch and the ceiling's spread
    bool unit = true;
    for (int i = 0; i < kRings * kAzimuths; i++) {
        float d[3], w;
        Line(i, d, w);
        unit = unit && d[1] < 0.0f && std::fabs(d[0] * d[0] + d[1] * d[1] + d[2] * d[2] - 1.0f) < 1e-5f && w > 0.0f;
    }
    Check(unit, "every line looks down, unit length, weight > 0");
    float seenOne[kRings * kAzimuths][3], flux[3];
    for (auto& s : seenOne) s[0] = s[1] = s[2] = 1.0f;
    UpFlux(seenOne, 1.0f, flux);
    std::printf("Up flux of 1 m2 over a surface at irradiance 1 everywhere: %.4f (albedo %.2f)\n", flux[0], kAlbedo);
    Check(std::fabs(flux[0] - kAlbedo) < 0.12f * kAlbedo, "a patch over a uniformly lit surface sends up about albedo x E x area");
    float seenNone[kRings * kAzimuths][3] = {};
    UpFlux(seenNone, 1.0f, flux);
    Check(flux[0] == 0.0f, "nothing lit below sends nothing up");
    double integral = 0.0;
    for (double d = 0.005; d < 400.0; d += 0.01) integral += 2.0 * 3.14159265 * d * CeilingKernel(static_cast<float>(d)) * 0.01;
    std::printf("Ceiling spread over an endless floor: %.4f of the flux (albedo %.2f)\n", integral, kAlbedo);
    Check(std::fabs(integral - kAlbedo) < 0.01, "the ceiling sends back albedo x the flux, spread over the floor");
    Check(CeilingKernel(0.0f) > CeilingKernel(3.0f) && CeilingKernel(3.0f) > CeilingKernel(8.0f), "the floor takes most beside the opening");
    // the house: 80 quadrants, the stairwell seen at irradiance 0.5 -> the floor beside the opening, and 6 m away
    float near = 0.0f, farFloor = 0.0f;
    UpFlux(seenOne, 0.25f, flux);
    for (int z = 30; z < 34; z++)
        for (int x = 22; x < 27; x++)
            for (int k = 0; k < 4; k++) {
                const float cx = x + 0.5f, cz = z + 0.5f;
                near += 0.5f * flux[0] * CeilingKernel(std::hypot(cx - 27.5f, cz - 32.0f));
                farFloor += 0.5f * flux[0] * CeilingKernel(std::hypot(cx - 33.0f, cz - 32.0f));
            }
    std::printf("Floor of story 2 from a stairwell at irradiance 0.5: beside the opening %.4f, 6 m away %.4f\n", near, farFloor);
    Check(near / 0.5f > 0.01f && near / 0.5f < 0.05f && farFloor < near,"the floor beside the opening takes a few % of the light below, less farther away");
    // a room lit only through an opening takes back albedo x the mean light of its surfaces
    Check(std::fabs(RoomFill(0.1f) - kAlbedo * 0.1f) < 1e-7f && RoomFill(-1.0f) == 0.0f && RoomFill(NAN) == 0.0f, "the room's fill is albedo x its mean light");
    // a tile taken as one patch (area 1) far from the texel equals its four quadrants summed, within 5%
    float tileOne[3] = {}, tileFour[3] = {};
    const float tc[3] = {kOx + 24.5f, kSlab, kOz + 31.5f};
    Add(far, facing, tc, 1.0f, white, tileOne);
    for (int k = 0; k < 4; k++) {
        const float cq[3] = {kOx + 24 + (k == 1 ? 0.8333f : k == 3 ? 0.1667f : 0.5f), kSlab, kOz + 31 + (k == 0 ? 0.1667f : k == 2 ? 0.8333f : 0.5f)};
        Add(far, facing, cq, 0.25f, white, tileFour);
    }
    Check(std::fabs(tileOne[0] - tileFour[0]) < 0.05f * tileFour[0], "a far tile taken whole matches its quadrants");
    // the penumbra of the slab's edge: the opening is x >= 22 (lot); the lamp 2.2 m over the slab, the floor 3 m under it
    const auto openX = [](float x, float) { return x >= 22.0f; };
    const float tMid = 2.2f / 5.2f; // the crossing of a ray from the lamp to the floor below
    Check(PenumbraShare(25.0f, 31.0f, tMid, 0.25f, openX) == 1.0f, "well inside the opening: the whole lamp passes");
    Check(PenumbraShare(18.0f, 31.0f, tMid, 0.25f, openX) == 0.0f, "well under the slab: none of it");
    Check(PenumbraShare(22.05f, 31.0f, tMid, 0.25f, openX) > 0.0f && PenumbraShare(22.05f, 31.0f, tMid, 0.25f, openX) < 1.0f &&
              PenumbraShare(21.95f, 31.0f, tMid, 0.25f, openX) > 0.0f && PenumbraShare(21.95f, 31.0f, tMid, 0.25f, openX) < 1.0f,
          "at the edge, on both sides: a part of the lamp (a penumbra, not a line)");
    Check(PenumbraShare(21.95f, 31.0f, tMid, 0.25f, openX) < PenumbraShare(22.05f, 31.0f, tMid, 0.25f, openX), "less on the slab's side");
    Check(PenumbraShare(21.95f, 31.0f, 0.999f, 0.25f, openX) == 0.0f, "a point right at the slab's plane: no penumbra (radius x (1 - t) -> 0)");

    // room_fill: a room is sent again when its measured mean moved by more than a quarter of the larger
    Check(!FillChanged(1.0f, 0.75f) && !FillChanged(0.75f, 1.0f), "exactly 25% apart: the same fill");
    Check(FillChanged(1.0f, 0.7f) && FillChanged(0.7f, 1.0f), "just over 25% apart: changed");
    Check(!FillChanged(0.4f, 0.4f), "the same mean: the same fill");
    Check(!FillChanged(0.0f, 0.0f), "both 0: the same fill");
    Check(FillChanged(0.0f, 0.5f) && FillChanged(0.5f, 0.0f), "0 against a lit mean: changed (a lamp switched on or off)");
    Check(!FillChanged(5e-5f, 1e-5f) && !FillChanged(0.0f, 9e-5f), "both under 1e-4: dark either way, the same fill");
    Check(FillChanged(NAN, 0.5f) && FillChanged(0.5f, NAN) && FillChanged(NAN, NAN), "a mean that is not a number: changed");
    // the surface of a sample's normal
    Check(SurfaceKind(0.0f, 1.0f, 0.0f) == Surface::Floor && SurfaceKind(0.3f, 0.95f, 0.0f) == Surface::Floor, "facing up: a floor");
    Check(SurfaceKind(1.0f, 0.0f, 0.0f) == Surface::Wall && SurfaceKind(0.0f, -0.3f, -0.9f) == Surface::Wall, "horizontal: a wall");
    Check(SurfaceKind(0.0f, -1.0f, 0.0f) == Surface::None, "facing down (a ceiling): neither");
    Check(SurfaceKind(0.7f, 0.7f, 0.0f) == Surface::None && SurfaceKind(0.0f, 0.9f, 0.43f) == Surface::None, "a slope: neither");
    Check(SurfaceKind(0.0f, 0.0f, 0.0f) == Surface::None, "a zero normal: neither");
    Check(SurfaceKind(NAN, NAN, NAN) == Surface::None && SurfaceKind(1.0f, NAN, 0.0f) == Surface::None, "a normal that is not a number: neither");
    Check(SurfaceKind(NAN, 0.95f, 0.0f) == Surface::Floor, "a floor needs only its upward part");
    // borrowed_light_adapt: the square root of 1 / the light the texels took, between the capped game value and x2
    Check(BorrowedAdapt(3.0f, 0.25f) == 2.0f && std::fabs(BorrowedAdapt(3.0f, 0.5f) - std::sqrt(2.0f)) < 1e-6f, "took 0.25: x2, took 0.5: x1.41 (the square root of what it lacks)");
    Check(BorrowedAdapt(3.0f, 0.02f) == kBorrowedAdaptMax && BorrowedAdapt(INFINITY, 0.01f) == kBorrowedAdaptMax, "a faint measure: capped (x2)");
    Check(BorrowedAdapt(3.0f, 1e-6f) == 1.0f && BorrowedAdapt(3.0f, 0.002f) == 1.0f, "almost nothing taken: no adaptation (the game value capped at 1)");
    Check(std::fabs(BorrowedAdapt(3.0f, 0.006f) - 1.5f) < 1e-5f, "halfway in the fade (0.006): halfway to the cap");
    {
        float last = 0.0f;
        bool rising = true;
        for (float t = 0.0005f; t <= 0.02f; t += 0.0005f) {
            const float a = BorrowedAdapt(3.0f, t);
            rising = rising && a >= last - 1e-6f;
            last = a;
        }
        Check(rising, "the fade never steps down as the light taken grows (up to 0.02)");
    }
    Check(BorrowedAdapt(3.0f, 2.0f) == 1.0f, "a bright measure: never under the game value capped at 1");
    Check(BorrowedAdapt(0.2f, 4.0f) == 0.5f && BorrowedAdapt(0.2f, 100.0f) == 0.2f, "a game value under 1 is the floor");
    Check(BorrowedAdapt(3.0f, 0.0f) == 1.0f && BorrowedAdapt(0.5f, 0.0f) == 0.5f, "no measure (0): the game value capped at 1");
    Check(BorrowedAdapt(3.0f, NAN) == 1.0f && BorrowedAdapt(3.0f, INFINITY) == 1.0f && BorrowedAdapt(3.0f, -1.0f) == 1.0f,
          "a measure that is not a positive finite number: the game value capped at 1");
    Check(BorrowedAdapt(INFINITY, 0.0f) == 1.0f, "every lamp blocked (+inf), no measure: 1");

    std::printf("stairwell_bounce: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
