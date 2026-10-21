#pragma once
// The second bounce through a stairwell opening onto the walls of the story above (development builds, switch
// stairwell_bounce in [dev_build]; pure part, tools/stairwell_bounce_test). Credits: @loinyx
//
// Measured in game (F8, a red lamp 0.8 m under the floor of story 2, beside its 5 x 4 m opening): the lamp's own
// light reaches the walls of story 2 that see it through the opening (IndoorPass), at the game's fall-off (0.02-0.04 of the
// lamp at 10 m), and nothing else does: the walls round the opening stayed black while the stairwell under them was lit.
// In a house the light that rises through a stairwell is mostly what the lit surfaces below reflect: a wall of the story
// above looks down through the opening at the lower room's floor and walls. Each opening patch at the slab is treated as
// a window onto the surface its line of sight meets below (the lower room's floor, or the wall where that room ends),
// whose radiance is albedo x irradiance / pi; the wall point takes radiance x cos(at the point) x cos(at the patch) x area
// / distance^2. The irradiance below is the game's own evaluation of the lower room's lamps there. The floor of the story
// above lies in the opening's plane and takes nothing (cos 0 at the patch), as it should: only a third bounce lights it.
#include <algorithm>
#include <cmath>

namespace StairwellBounce {

constexpr float kAlbedo = 0.4f;     // of the surfaces below (an interior's floors and walls)
constexpr float kPi = 3.14159265f;
constexpr float kStep = 0.5f;       // m along the ground when the line of sight is followed below the opening (a tile: rooms change only at tile edges)
constexpr float kMaxRun = 16.0f;    // m along the ground at most (beyond it nothing is taken)
constexpr float kMinAbove = 0.02f;  // m: a point on the slab's plane (or under it) sees no patch of it

// The share of a patch's radiance (area at q, normal up) that reaches the point p with normal n (all world, y up):
// cos(p) x cos(q) x area / r^2, with r^2 at least half the area (a point beside a small patch, not a pole)
inline float Geometry(const float p[3], const float n[3], const float q[3], float area) {
    const float d[3] = {q[0] - p[0], q[1] - p[1], q[2] - p[2]};
    const float above = p[1] - q[1];
    if (!(above > kMinAbove) || !(area > 0.0f)) return 0.0f;
    const float r2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
    const float r = std::sqrt(r2);
    if (!(r > 1e-4f)) return 0.0f;
    const float cosP = (n[0] * d[0] + n[1] * d[1] + n[2] * d[2]) / r, cosQ = above / r;
    if (!(cosP > 0.0f)) return 0.0f;
    return cosP * cosQ * area / std::max(r2, 0.5f * area);
}

// The irradiance the point takes from one patch: geometry x albedo / pi x the irradiance of the surface seen below (rgb)
inline void Add(const float p[3], const float n[3], const float q[3], float area, const float below[3], float out[3]) {
    const float g = Geometry(p, n, q, area) * kAlbedo / kPi;
    if (!(g > 0.0f)) return;
    for (int c = 0; c < 3; c++)
        if (std::isfinite(below[c]) && below[c] > 0.0f) out[c] += g * below[c];
}

// The floor of the story above (its plane holds the opening, so it sees none of it): the light that rises through an
// opening patch reaches its ceiling and comes back down. A patch whose lower side is seen at irradiance E (radiance
// albedo E / pi over the downward hemisphere) sends up the flux area x albedo x E (UpFlux: the hemisphere in kRings rings
// of kAzimuths lines, each weighted by cos x its solid angle); the ceiling reflects albedo of it; the floor point at ground
// distance d takes it through the two legs (opening -> ceiling -> floor), each of height h, as one Lambertian spread of
// height 2h: flux x albedo x (2h)^2 / (pi (d^2 + (2h)^2)^2), which sums to the whole flux over an endless floor.
constexpr int kRings = 3, kAzimuths = 8;
constexpr float kRingTheta[kRings] = {0.349066f, 0.785398f, 1.221730f};                   // 20, 45, 70 degrees from straight down
constexpr float kRingOmega[kRings] = {0.983882f / kAzimuths, 1.923083f / kAzimuths, 3.376220f / kAzimuths}; // sr per line
constexpr float kStoryHeight = 3.0f; // m from a floor to the ceiling over it
// The downward direction of line i (0 .. kRings x kAzimuths - 1) and its weight cos x solid angle
inline void Line(int i, float dir[3], float& weight) {
    const int ring = i / kAzimuths, k = i % kAzimuths;
    const float th = kRingTheta[ring], az = 6.2831853f * (static_cast<float>(k) + 0.5f * static_cast<float>(ring & 1)) / kAzimuths;
    dir[0] = std::sin(th) * std::cos(az), dir[1] = -std::cos(th), dir[2] = std::sin(th) * std::sin(az);
    weight = std::cos(th) * kRingOmega[ring];
}
// The flux a patch of `area` sends up when the lines below meet surfaces at irradiance seen[i] (rgb; 0 where none met)
inline void UpFlux(const float (*seen)[3], float area, float flux[3]) {
    flux[0] = flux[1] = flux[2] = 0.0f;
    for (int i = 0; i < kRings * kAzimuths; i++) {
        float dir[3], w;
        Line(i, dir, w);
        for (int c = 0; c < 3; c++)
            if (std::isfinite(seen[i][c]) && seen[i][c] > 0.0f) flux[c] += area * kAlbedo / kPi * seen[i][c] * w;
    }
}
// The ceiling's share of one patch's upward flux that a floor point takes per unit flux, at ground distance d (m)
inline float CeilingKernel(float d, float h = kStoryHeight) {
    const float H = 2.0f * h, s = d * d + H * H;
    return kAlbedo * H * H / (kPi * s * s);
}

// m over the lower floor at which a line into a stairwell with no floor of its own meets the stair's treads (Follow)
constexpr float kWellLift = 1.5f;
// (room_fill) A room lit only through an opening (a lamp of story 2 lights a patch of story 1's floor through the opening,
// and the rest of the room stays black): its surfaces take back what its lit ones reflect, one bounce averaged over the
// room, albedo x the mean light of its floor and wall texels (a closed room's first bounce; the further ones are left to the
// game's own ambient)
inline float RoomFill(float mean) { return std::isfinite(mean) && mean > 0.0f ? kAlbedo * mean : 0.0f; }
// (room_fill) Whether the mean light a solve measured (now) differs from the one its fill used (used) by more than a quarter
// of the larger; two means both under 1e-4 count as the same (dark), and a mean that is not a number counts as changed
inline bool FillChanged(float used, float now) {
    return !(std::fabs(now - used) <= 0.25f * std::max(now, used) || std::max(now, used) < 1e-4f);
}
// The surface a sample's normal (world, y up) belongs to: a floor (facing up), a wall (horizontal) or neither (a ceiling,
// a slope, a normal that is not a number)
enum class Surface { None, Floor, Wall };
inline Surface SurfaceKind(float nx, float ny, float nz) {
    if (ny > 0.9f) return Surface::Floor;
    if (std::fabs(ny) < 0.5f && std::fabs(nx) + std::fabs(nz) > 0.5f) return Surface::Wall;
    return Surface::None;
}
// (borrowed_light_adapt) The normalisation of a room lit only by lamps of another story. The eye adapts to a dark room only
// in part (and to the whole view, not room by room), so the room is brought up by the square root of what its brightest
// texel lacks: sqrt(1 / took) (took: the brightest light its texels took in its last solve), at least the game's value capped
// at 1 (game; +inf when its lamps are all blocked), at most kBorrowedAdaptMax. A full 1 / took up to x4 made a room lit by a
// sliver of a lamp on another story look lit by a lamp of its own. A measure that is not a positive finite number leaves
// the capped game value.
constexpr float kBorrowedAdaptMax = 2.0f;
inline float BorrowedAdapt(float game, float took) {
    const float keep = std::min(game, 1.0f);
    if (took > 0.0f && std::isfinite(took)) return std::clamp(std::sqrt(1.0f / took), keep, kBorrowedAdaptMax);
    return keep;
}
// Where a line of sight below the opening ends (Follow): the point, its surface's normal, whether it is a wall, and whether it
// is the stair in a well with no floor (kWellLift)
struct Hit {
    float at[3] = {}, n[3] = {};
    bool wall = false, well = false;
};
// What the line of sight from p through q meets below the opening: the lower room's floor (normal up) or, where that room
// ends before the floor is reached, its wall (normal back along the line, horizontal). roomAt(x, z) gives the lower story's
// room at a world point (0 = outside, -1 = unreadable), floorAt(x, z) whether that story has a floor there. yLow: the lower
// story's floor (world). False when nothing is met (unreadable, too far).
// A line that reaches the lower floor's plane where that story has no floor of its own looks into a stairwell that goes on
// down, with a stair in it: it meets the stair's treads, taken at kWellLift over the lower floor, facing up (where the
// opening of story 2 lies over story 1's own, the lines under 45 degrees met nothing without it, and the floor above took
// the ceiling's light of the 70-degree lines only).
template <class RoomAt, class FloorAt>
bool Follow(const float p[3], const float q[3], float yLow, int room, RoomAt roomAt, FloorAt floorAt, Hit& hit) {
    const float d[3] = {q[0] - p[0], q[1] - p[1], q[2] - p[2]};
    if (!(d[1] < -1e-4f) || room <= 0) return false;
    const float tFloor = (yLow - q[1]) / d[1]; // > 0: q is above the lower floor
    if (!(tFloor > 0.0f)) return false;
    const float run = std::sqrt(d[0] * d[0] + d[2] * d[2]);
    const int steps = run > 1e-4f ? static_cast<int>(std::ceil(tFloor * run / kStep)) : 1;
    if (static_cast<float>(steps) * kStep > kMaxRun + kStep) return false;
    float prev[3] = {q[0], q[1], q[2]};
    for (int i = 1; i <= steps; i++) {
        const float t = std::min(tFloor, tFloor * static_cast<float>(i) / static_cast<float>(steps));
        const float x[3] = {q[0] + t * d[0], q[1] + t * d[1], q[2] + t * d[2]};
        const int r = roomAt(x[0], x[2]);
        if (r < 0) return false;
        if (r != room) { // the lower room ends here: its wall, at the height the line had
            if (!(run > 1e-4f)) return false;
            for (int k = 0; k < 3; k++) hit.at[k] = prev[k];
            hit.n[0] = -d[0] / run, hit.n[1] = 0.0f, hit.n[2] = -d[2] / run;
            hit.wall = true;
            return true;
        }
        for (int k = 0; k < 3; k++) prev[k] = x[k];
    }
    const bool floor = floorAt(prev[0], prev[2]);
    for (int k = 0; k < 3; k++) hit.at[k] = prev[k];
    hit.at[1] = floor ? yLow : yLow + kWellLift;
    hit.n[0] = 0.0f, hit.n[1] = 1.0f, hit.n[2] = 0.0f;
    hit.wall = false;
    hit.well = !floor;
    return true;
}

} // namespace StairwellBounce
