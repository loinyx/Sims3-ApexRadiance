#pragma once
// Physical indoor lamps, stage 4 (docs/features/night-lighting/gpu-room-lamps.md "Physical model"): which lot lamps a kept
// map draws, with which room row, and whether a lamp edit skips the room solves. No game access (tools/outdoor_delta_test).
//   - A kept map draws every lamp of the active lot with a GPU law whose reach meets its texels' box, not only the lamps its
//     solve listed: a lamp moved into the room, switched on, or new lights it at once.
//   - Never twice: a light in the solve's base (`baked`: no law when kept) is never drawn; a listed lamp's measured part left the
//     base, so it is drawn; a lamp the solve did not list has nothing in the base, so it is drawn too.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <map>
#include <set>
#include <tuple>
#include <utility>
#include <vector>
#include "gpu_lamp_law.h"
#include "inner_retry.h"
#include "outdoor_delta_model.h"
#include "room_light_tests.h"
#include "rt_lamp_proxy.h"

namespace PhysLampPick {

// A map's texels' box (world space)
struct Box {
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    void Grow(const float p[3]) {
        for (int k = 0; k < 3; k++) lo[k] = std::min(lo[k], p[k]), hi[k] = std::max(hi[k], p[k]);
    }
    bool Empty() const { return lo[0] > hi[0]; }
};

// The squared distance from the lamp's position beyond which it adds nothing to a texel: ReachBound2 of its law with W times
// `scale` (its room row's mult x norm scale, at least 1), a TubeLight's segment and an area light's size added to the distance.
// -1: no bound (always drawn); 0: it adds nothing (off)
inline float Reach2(const GpuLampLaw::Lamp& L, float scale, float thr) {
    const float esum = (L.colour[0] + L.colour[1]) + L.colour[2];
    const float spill = std::max({L.spill[0], L.spill[1], L.spill[2]});
    const float d2 = OutdoorDeltaModel::ReachBound2(L.W * std::max(scale, 1.0f), esum, spill, thr);
    if (d2 <= 0.0f) return d2;
    float ext = 0.0f;
    if (L.sc2 == GpuLampLaw::kTube) ext = std::sqrt(L.axis[0] * L.axis[0] + L.axis[1] * L.axis[1] + L.axis[2] * L.axis[2]);
    else if (L.sc2 < -1.5f) ext = std::sqrt(L.r0sq * L.r0sq + L.invR2 * L.invR2) + std::sqrt(std::fabs(L.r0sq)); // (area: its sizes, either form)
    if (!std::isfinite(ext)) return -1.0f;
    const float r = std::sqrt(d2) + ext;
    return r * r;
}
// The lamp's reach (r2 as Reach2 gives it) meets the box
inline bool Meets(const Box& b, const float c[3], float r2) {
    if (r2 < 0.0f) return true;
    if (r2 == 0.0f || b.Empty()) return false;
    float d2 = 0.0f;
    for (int k = 0; k < 3; k++) {
        const float d = c[k] < b.lo[k] ? b.lo[k] - c[k] : c[k] > b.hi[k] ? c[k] - b.hi[k] : 0.0f;
        d2 += d * d;
    }
    return d2 <= r2;
}

// A lot lamp in a kept solve: in its base (never drawn), listed (its part left the base: drawn with the solve's row), or fresh
// (not in the solve: nothing of it in the base, drawn with a row of its own)
enum class Use { Base, Listed, Fresh };
inline Use UseOf(uintptr_t light, const std::vector<uintptr_t>& listed, const std::vector<uintptr_t>& baked) {
    if (std::find(baked.begin(), baked.end(), light) != baked.end()) return Use::Base;
    return std::find(listed.begin(), listed.end(), light) != listed.end() ? Use::Listed : Use::Fresh;
}
// A fresh lamp's norm scale in a room (outdoor_delta.cpp NormKOf's lookup without the record's innerSrc, which holds only
// lamps it gathered): a lamp registered in another room of the same story shines at that room's normalisation against this
// one's; room 0 also takes a lamp of a roofed room of the story above at that room's (the record's innerUp, InnerShadow's
// source of a lamp shining down out of the story above); else 1. (Whether the lamp is in sight through an opening is the
// lamp's shadow tile's: the gather's own test only leaves out lamps it cannot see.)
inline float NormKFresh(int lampStory, int lampHome, float homeNorm, int roomStory, int roomId, float hereNorm, bool hereOutdoor, bool homeOutdoor = true) {
    const bool up = roomId == 0 && lampStory == roomStory + 1 && !homeOutdoor;
    if ((lampStory != roomStory && !up) || lampHome <= 0 || (!up && lampHome == roomId) || !std::isfinite(homeNorm) || !std::isfinite(hereNorm)) return 1.0f;
    return InnerLight::NormScale(homeNorm, hereNorm, hereOutdoor);
}
// A fresh lamp's realistic reach share (LevelLightShare::ReachAppliesIn without the record's sets): an outdoor room takes it
// from every lamp, a roofed room from an outdoor lamp (one through its openings)
// shortcut: ReachAppliesIn's openings test (FindSame) and open-top test (FindCross) read the lists the room's gather made, which
// hold no lamp it did not gather (a roofed room with no opening in sight of an outdoor lamp, or a roofless room over a
// double-height room lighting a lamp below, gets the reach here); the room's next solve (a switch's settle, a move into another
// room's settle, or any later one) gives the exact row. Upgrade: run the gather's per-lamp tests for a fresh lamp on the light
// tree thread
// fromOpenBelow: the lamp's home is a roofed room the open-top build opened into this room (room 0 of the story above), one
// space with it: no reach there, as ReachAppliesIn's open-top test leaves a cross lamp out (03/11, lot C49C: a red tube of story
// 2 room 5 lit the upper half of its own wall at 36% of the lower, a hard step at the story line)
inline bool ReachFresh(bool lampOutdoor, bool roomOutdoor, bool fromOpenBelow = false) { return lampOutdoor || (roomOutdoor && !fromOpenBelow); }
// A fresh lamp's exit row in room 0 (LevelLightShare::ExitLampOf: room 0 of story 0, a lamp of its record's inner list): a lamp
// of a roofed room of story 0 or 1 (InnerShadow's lamps of the story and of the one above; the exit sum takes the lamp's shadow,
// so one out of sight adds nothing there either)
inline bool ExitFresh(int roomStory, int lampStory, int lampHome, bool homeOutdoor) { return roomStory == 0 && lampHome > 0 && !homeOutdoor && (lampStory == 0 || lampStory == 1); }
// The lamps of the last registry snapshot that are not in this one (deleted, or moved to a basement or off the lot) join
// `gone`: a kept solve listing one no longer draws it (its light may be freed); one seen again leaves it (a new light at a freed
// address). At most `max`, the oldest dropped
inline void NoteGone(const std::vector<uintptr_t>& before, const std::vector<uintptr_t>& now, std::vector<uintptr_t>& gone, size_t max = 256) {
    std::erase_if(gone, [&](uintptr_t l) { return std::find(now.begin(), now.end(), l) != now.end(); });
    for (const uintptr_t l : before)
        if (std::find(now.begin(), now.end(), l) == now.end() && std::find(gone.begin(), gone.end(), l) == gone.end()) gone.push_back(l);
    if (gone.size() > max) gone.erase(gone.begin(), gone.end() - static_cast<std::ptrdiff_t>(max));
}
// A lamp of the last snapshot (`before`) missing from the registry now stays in `now` with its last entry until it has been
// missing for graceMs (the registry drops a dragged lamp for a few frames, and every lamp while build mode edits the lot), so
// it is neither undrawn nor gone meanwhile. `missing`: light -> the tick it was first missed; a lamp seen again or past its
// grace leaves it. Another lot (sameLot false): nothing carried
template <class Lamp>
inline void CarryMissing(const std::vector<Lamp>& before, std::vector<Lamp>& now, bool sameLot, uint32_t tick, uint32_t graceMs, std::map<uintptr_t, uint32_t>& missing) {
    if (!sameLot) return missing.clear();
    const size_t seen = now.size();
    const auto inNow = [&](uintptr_t l) { return std::any_of(now.begin(), now.begin() + static_cast<std::ptrdiff_t>(seen), [&](const Lamp& x) { return x.light == l; }); };
    std::erase_if(missing, [&](const auto& kv) { return inNow(kv.first); });
    for (const Lamp& l : before) {
        if (inNow(l.light)) continue;
        if (tick - missing.try_emplace(l.light, tick).first->second < graceMs) now.push_back(l);
        else missing.erase(l.light);
    }
}

// (stage 5) a lamp's physical share at a directional sample, as the room's maps take it: the open-top proxy's visibility of the
// lamp ball (radius R) from the sample (lot space: Ll, Pl; 16 rays, the stamp as RtLampProxy::Visibility takes it) x the game's
// 3D test over the room's records from the lamp to the sample (world space: Lw, Pw); a blocker of another vtable counts as 1
// in *unknown (as the GPU's blocker table leaves it out). OutdoorDeltaModel::BasisPartPhys applies the game's drop
inline void BasisShare(const RtLampProxy::Mesh& mesh, const float Ll[3], float R, const float Pl[3], const RoomLightTests::Room& tests, const float Lw[3],
                       const float Pw[3], std::vector<uint32_t>& stamp, uint32_t& mark, float& vis, float& t3d, int* unknown) {
    vis = RtLampProxy::Visibility(mesh, Ll, R, Pl, 16, &stamp, &mark);
    t3d = vis > 0.0f ? RoomLightTests::occlude(tests, {Lw[0], Lw[1], Lw[2]}, {Pw[0], Pw[1], Pw[2]}, unknown) : 1.0f;
}
// A lamp's BasisShare over a room's samples, kept between the directional maps' writes: a sample's share depends only on the
// lamp's position, the room's samples and records and the proxy (`same`: the same kept room, proxy and radius), so a lamp that
// did not move reuses it and only a moved or edited lamp is traced again (an edit 2 lamps of ~200, the 21:18 log of 02/11:
// ~520 ms for every lamp). Writes vis / t3d as BasisShare over every lit sample (0 elsewhere); returns the samples traced
struct ShareCache {
    float Ll[3] = {}, Lw[3] = {};
    std::vector<float> vis, t3d;
    std::vector<uint8_t> done;
};
template <class Lit, class PwAt>
inline size_t BasisSharesCached(ShareCache& c, bool same, size_t n, Lit&& lit, const RtLampProxy::Mesh& mesh, const float Ll[3], float R, const float* Pl,
                                const RoomLightTests::Room& tests, const float Lw[3], PwAt&& pw, std::vector<uint32_t>& stamp, uint32_t& mark, std::vector<float>& vis,
                                std::vector<float>& t3d, int* unknown) {
    if (!same || c.done.size() != n || std::memcmp(c.Ll, Ll, sizeof c.Ll) || std::memcmp(c.Lw, Lw, sizeof c.Lw)) {
        std::memcpy(c.Ll, Ll, sizeof c.Ll), std::memcpy(c.Lw, Lw, sizeof c.Lw);
        c.vis.assign(n, 0.0f), c.t3d.assign(n, 0.0f), c.done.assign(n, 0);
    }
    size_t traced = 0;
    vis.assign(n, 0.0f), t3d.assign(n, 0.0f);
    for (size_t i = 0; i < n; i++) {
        if (!lit(i)) continue;
        if (!c.done[i]) BasisShare(mesh, Ll, R, &Pl[i * 3], tests, Lw, pw(i), stamp, mark, c.vis[i], c.t3d[i], unknown), c.done[i] = 1, traced++;
        vis[i] = c.vis[i], t3d[i] = c.t3d[i];
    }
    return traced;
}

// ---- a lamp edit without the room solves ----
// One room of the lot as the edit finds it (story -4 .. 7)
struct RoomFact {
    int story = 0, id = 0;
    bool lists = false;  // its light list holds the lamp
    bool kept = false;   // (id > 0) a kept physical solve of the class it shows
    bool drawn = false;  // that solve drawn on every map it wrote (room 0: drawn physically, every story covered)
    bool baked = false;  // that solve has the lamp's light in its base
};
// A kept room's 3D blockers key from RoomGeoKey's parts (the lot's, the walls', the 3D blockers'): the blockers' part alone, 0
// when unread. The lot's part changes with any door or cut of the lot, and the walls' part (the default wall list, the wall
// mode) changed between a room's solve and the next edit with nothing moved (story 0 room 13 of 03/11: every physical edit
// refused); a moved piece of furniture changes its record's blockers. A differing key sends the room to a solve (the edit is
// still taken: its blocker table is the old one until then)
inline uint64_t BlockersKey(const uint64_t parts[3]) { return parts[0] ? parts[2] | 1 : 0; }
// Null when every story 0 .. 7 draws the lamp from its law now (the edit marks no room); else why not, `at` the room
inline const char* EditRefusal(const std::vector<RoomFact>& rooms, int& at) {
    bool any = false;
    for (at = 0; at < static_cast<int>(rooms.size()); at++) {
        const RoomFact& f = rooms[at];
        if (f.story < 0 || f.story > 7) {
            if (f.lists) return "a basement room lists it (basements keep the game's solves)";
            continue;
        }
        any = true;
        if (f.id == 0) {
            if (!f.drawn) return "its room 0 is not drawn physically on every story";
            continue;
        }
        if (!f.kept) return "no kept physical solve of the class it shows";
        if (!f.drawn) return "a map of its kept solve not drawn on the GPU";
        if (f.baked) return "its kept solve has the lamp in its base";
    }
    at = -1;
    return any ? nullptr : "no story of the lot found";
}

// The rooms (tracker, story, id) whose game's maps lack a lamp edit taken without their solve: the rooms listing the lamp, its
// own room and every room 0 of stories 0 .. 7 (each draws the lot's lamps by reach), and any map that drew the lamp since. A
// room's next solve forgets it (Solved); a room whose map falls back to the game's is sent to its solve (Take, TakeAll)
struct TouchedRooms {
    using Key = std::tuple<uintptr_t, int, int>;
    std::set<Key> rooms;
    void Note(const Key& k) {
        if (rooms.size() < 4096) rooms.insert(k);
    }
    void NoteEdit(uintptr_t tracker, int story, const std::vector<std::pair<int, int>>& listing, int reg) {
        for (const auto& [s, id] : listing) Note({tracker, s, id});
        if (reg > 0) Note({tracker, story, reg});
        for (int s = 0; s <= 7; s++) Note({tracker, s, 0});
    }
    void Solved(const Key& k) { rooms.erase(k); }
    bool Take(const Key& k) { return rooms.erase(k) != 0; }
    std::vector<Key> TakeAll() {
        std::vector<Key> all(rooms.begin(), rooms.end());
        rooms.clear();
        return all;
    }
};

// (LampMarkFilter) a light entry's update as an edit: -1 none, 0 a value, 1 a move, 2 into another room. A move is moveMin or
// more; physically a shorter step is one only while an edit of the lamp goes on (a slow drag's steps), never an animated
// lamp's wobble (which would keep the fast shadows and rewrite the directional maps at every update). Physically, an update
// that changed the light's signature in place (sigChanged: a rotation, or its entry's flags as a drag starts or the lamp is
// put down) is a value: the GPU draws its law as it is now, where the game's path solved the lamp's rooms, then the rooms of
// other stories taking it (09:23 log of 03/11: its own rooms re-solved 45 times in a drag, other objects and stairs flickering)
inline int EditKind(bool otherRoom, float d2, float moveMin, bool valueChanged, bool physical, bool editing, bool sigChanged = false) {
    if (otherRoom) return 2;
    if (d2 > moveMin * moveMin) return 1;
    if (valueChanged) return 0;
    if (physical && editing && d2 > 1e-6f) return 1;
    return physical && sigChanged && d2 <= 1e-6f ? 0 : -1;
}
// (LampMarkFilter, an occluder mark) a light new to the lot: no update of it seen yet, and absent from the lot's registry
// snapshot (OutdoorDelta::LotLamps, read every 250 ms) while that snapshot is of its lot. A lamp already in it is an old one
// whose entry never updated: its neighbours' marks stay the game's
inline bool NewLampEvidence(bool seen, bool snapshotOfLot, bool inSnapshot) { return !seen && snapshotOfLot && !inSnapshot; }
// (LampMarkFilter, an occluder mark) the occluder is the lamp's own object: its point (x, z) lies within nearM of the lamp's head
// now (horizontally), and moved as the head did since both were last seen (within tol), when its point was seen before. An
// object moved near a lamp, or a still one next to a lamp whose values change, is not
inline bool OwnObjectStep(bool hadPoint, const float occWas[2], const float occNow[2], const float lampWas[3], const float lampNow[3], float tol, float nearM) {
    const float nx = occNow[0] - lampNow[0], nz = occNow[1] - lampNow[2];
    if (!(nx * nx + nz * nz <= nearM * nearM)) return false;
    if (!hadPoint) return true;
    const float dx = (occNow[0] - occWas[0]) - (lampNow[0] - lampWas[0]), dz = (occNow[1] - occWas[1]) - (lampNow[2] - lampWas[2]);
    return dx * dx + dz * dz <= tol * tol;
}
// (LampMarkFilter) an occluder entry whose point moved at a mark: where it is now (world x, z), when, and its object's first
// occluder shape (entry+0x20 -> +0x40: the first blocker of the object's 3D record in its room, Record::shape; 0 none)
struct OccMove {
    uintptr_t entry = 0;
    uint32_t at = 0;
    float x = 0.0f, z = 0.0f;
    uintptr_t shape = 0;
};
// The object whose first occluder shape is `shape` moved lately (a record's own object: the table). Its identity, not its place,
// tells a table dragged with its lamp from the lamp dragged alone over it: in the table drags of 03/11 (recordings 11:47 to
// 15:25, all table drags) one entry moved, by the lamp's head. UNVERIFIED in the game: that the moving entry's shape is the table
// record's first blocker, and that a lamp dragged alone moves no entry with that shape ([CarryTable] and the 'O' journal lines)
inline bool ShapeMoved(const OccMove* m, int n, uintptr_t shape, uint32_t now, uint32_t withinMs) {
    for (int i = 0; i < n && shape; i++)
        if (m[i].entry && m[i].shape == shape && now - m[i].at <= withinMs) return true;
    return false;
}
// (GpuRoomLamps' carry, its [CarryTable] line) a record under a moved lamp (StandsOn) is carried when its own object moved lately
// (ShapeMoved), else left in place; no record under it (a lamp on the floor, the 16:25 recording of 03/11) is neither: 0 none,
// 1 left in place, 2 carried
inline int CarryOf(bool standsOn, bool shapeMoved) { return !standsOn ? 0 : shapeMoved ? 2 : 1; }
// Physical capture mode, while a drag goes on (16:25 recording of 03/11: the game's solves finished mid-drag wrote over the GPU's
// maps, 1199 times). MarkUnchanged: the game's mark of a lamp whose signature did not change goes through only while the filter is
// off and outside the physical mode (same values, same gather). TailFanout: the walk's sends of other rooms for a GPU edit's tail,
// never in the physical mode (every kept room draws the lot's lamps from their laws by reach)
inline bool MarkUnchanged(bool filterOn, bool physical) { return !filterOn && !physical; }
inline bool TailFanout(bool gpuTail, bool physical) { return !(gpuTail && physical); }
// (WritePhysCarry) the samples whose 3D test may differ between two solves of a room (`was`, `now`), from the records one holds
// and the other does not hold alike (moved, come or gone: a table dragged with its lamp). The last write was traced over `was`'s
// records with each lamp somewhere between where the two solves saw it (x, y, z then, x, y, z now): a sample a gone record
// shades from any of 5 points of that way, or a come one from the lamp now, has a wrong furniture shadow in that write. The
// game's own test (RoomLightTests::occlude: its soft bands too), so a table top never counts against stairs above its lamp
template <class Pos>
std::vector<char> CarryStale(const RoomLightTests::Room& was, const RoomLightTests::Room& now, const std::vector<std::array<float, 6>>& lamps, size_t n, Pos pos) {
    using RoomLightTests::Record;
    // (alike: the fields the test reads, RecordKey; the raw bytes hold the game's own state)
    std::vector<uint64_t> kw, kn;
    for (const Record& r : was.rec) kw.push_back(RoomLightTests::RecordKey(r));
    for (const Record& r : now.rec) kn.push_back(RoomLightTests::RecordKey(r));
    RoomLightTests::Room gone, came;
    for (size_t i = 0; i < was.rec.size(); i++)
        if (std::find(kn.begin(), kn.end(), kw[i]) == kn.end()) gone.rec.push_back(was.rec[i]);
    for (size_t i = 0; i < now.rec.size(); i++)
        if (std::find(kw.begin(), kw.end(), kn[i]) == kw.end()) came.rec.push_back(now.rec[i]);
    std::vector<char> out(n, 0);
    if (gone.rec.empty() && came.rec.empty()) return out;
    int unknown = 0;
    for (size_t i = 0; i < n && !lamps.empty(); i++) {
        const float* p = pos(i);
        const RoomLightTests::V s{p[0], p[1], p[2]};
        for (size_t l = 0; l < lamps.size() && !out[i]; l++) {
            const auto& L = lamps[l];
            const RoomLightTests::V to{L[3], L[4], L[5]};
            out[i] = !came.rec.empty() && RoomLightTests::occlude(came, to, s, &unknown) < 1.0f;
            for (int k = 0; k <= 4 && !out[i] && !gone.rec.empty(); k++) {
                const float f = 0.25f * static_cast<float>(k);
                out[i] = RoomLightTests::occlude(gone, {L[0] + f * (L[3] - L[0]), L[1] + f * (L[4] - L[1]), L[2] + f * (L[5] - L[2])}, s, &unknown) < 1.0f;
            }
        }
    }
    return out;
}
// A lamp's furniture moved with it: an occluder beside its head moved lately. Dragging a table with its lamp moves one occluder
// entry, never two (03/11 recordings 11:47 and 13:56: 57B1E040, then 9D2D0970 after the drop); asking for two never carried the
// table, and its shadow stayed where the last solve read it while the floor under the dragged table lit up
inline bool FurnitureMovedWith(const OccMove* m, int n, float lx, float lz, uint32_t now, uint32_t withinMs, float nearM) {
    for (int i = 0; i < n; i++) {
        const float dx = m[i].x - lx, dz = m[i].z - lz;
        if (m[i].entry && now - m[i].at <= withinMs && dx * dx + dz * dz <= nearM * nearM) return true;
    }
    return false;
}
// A room's light list length from its vector's bounds (room+0xC8, +0xCC), -1 when unreadable. A room no lamp ever lit holds no
// buffer (both null): an empty list, which a kept physical solve needs read (else every edit is refused, 2026-11-02 20:50 log)
inline int ListLen(const uintptr_t* b, const uintptr_t* e, int max) {
    if (!b) return e ? -1 : 0;
    return e < b || e - b > max ? -1 : static_cast<int>(e - b);
}

// (GpuRoomLamps::ComposeOver's partial relight) no place (y << 16 | x) within w x h is written by two of the sets: a set drawn
// again alone then leaves the map as drawing them all would (a place past the map is never written: the viewport cuts it)
inline bool WritesDisjoint(int w, int h, const std::vector<const std::vector<uint32_t>*>& sets) {
    if (w <= 0 || h <= 0 || sets.size() >= 0xFFFF) return false;
    std::vector<uint16_t> owner(static_cast<size_t>(w) * h, 0);
    for (size_t i = 0; i < sets.size(); i++)
        for (const uint32_t at : *sets[i]) {
            const uint32_t x = at & 0xFFFFu, y = at >> 16;
            if (x >= static_cast<uint32_t>(w) || y >= static_cast<uint32_t>(h)) continue;
            uint16_t& o = owner[static_cast<size_t>(y) * w + x];
            if (o && o != i + 1) return false;
            o = static_cast<uint16_t>(i + 1);
        }
    return true;
}
// (sharper floors) q (a neighbour's sample; null: none) on s's plane: normals within ~8 degrees, within 1 cm of it, at most 0.6 m
// away (never across a wall or a step). A sample: position at [0..2], normal at [4..6]
inline bool SamePlane(const float* s, const float* q) {
    if (!q) return false;
    const float d[3] = {q[0] - s[0], q[1] - s[1], q[2] - s[2]};
    const float nn = s[4] * q[4] + s[5] * q[5] + s[6] * q[6], plane = s[4] * d[0] + s[5] * d[1] + s[6] * d[2];
    return nn > 0.99f && std::fabs(plane) <= 0.01f && d[0] * d[0] + d[1] * d[1] + d[2] * d[2] <= 0.36f;
}
// (sharper floors) A kept texel's sub-texel position, (fx, fy) texel steps from its own sample s along the steps to its neighbours
// nb = {right, left, down, up} (null: none) on its plane (SamePlane); an edge texel takes the other side's step, a texel with
// neither keeps its own sample along that axis
inline void SubTexelPos(const float* s, const float* const nb[4], float fx, float fy, float out[3]) {
    const auto same = [&](const float* q) { return SamePlane(s, q); };
    const auto step = [&](const float* fwd, const float* back, float st[3]) {
        for (int k = 0; k < 3; k++) st[k] = same(fwd) ? fwd[k] - s[k] : same(back) ? s[k] - back[k] : 0.0f;
    };
    float sx[3], sy[3];
    step(nb[0], nb[1], sx), step(nb[2], nb[3], sy);
    for (int k = 0; k < 3; k++) out[k] = s[k] + fx * sx[k] + fy * sy[k];
}
// (sharper floors) The half width (m) of a sub-texel's footprint the shadow is averaged over (AccPhys: mask.w), frac of the texel
// step: only where all 4 neighbours lie on the texel's plane, so a footprint never reaches past a wall or a step (a texel by a wall
// keeps its point sample: the wall's side would darken it); 0 elsewhere
inline float SubTexelFootprint(const float* s, const float* const nb[4], float frac) {
    float m = 1e30f;
    for (int i = 0; i < 4; i++) {
        if (!SamePlane(s, nb[i])) return 0.0f;
        const float d[3] = {nb[i][0] - s[0], nb[i][1] - s[1], nb[i][2] - s[2]};
        m = std::min(m, std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]));
    }
    return frac * m;
}
// (sharper floors) A floor texel's 4 sub-texel positions (out[q]: x step q & 1, y step q >> 1, a quarter texel each way) and its
// footprint (SubTexelFootprint), over the neighbours at(x, y) gives (its sample; null: none). `at` holds one room's texels only, so
// a texel of the room past a wall, a diagonal wall's other half tile included, is never a neighbour
// The split tiles' atlas (a diagonal wall's half tiles, u past the tile columns) packs each half tile's texels in short rotated
// strips: a map neighbour across a strip's end lies on the plane but 1.4-1.5 steps away, another half tile's (03/11 14:36 scene,
// story 1 room 8: steps 0.24-0.25 m, 0.36 m across). A neighbour farther than 1.25 times the nearest on the plane is none
template <class At>
inline float SubTexels(const float* s, int x, int y, At&& at, float out[4][3]) {
    const float* nb[4] = {at(x + 1, y), at(x - 1, y), at(x, y + 1), at(x, y - 1)};
    float d2[4], nearest = 1e30f;
    for (int i = 0; i < 4; i++) {
        d2[i] = 1e30f;
        if (!SamePlane(s, nb[i])) continue;
        const float d[3] = {nb[i][0] - s[0], nb[i][1] - s[1], nb[i][2] - s[2]};
        nearest = std::min(nearest, d2[i] = d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    }
    for (int i = 0; i < 4; i++)
        if (d2[i] > 1.5625f * nearest) nb[i] = nullptr; // (1.25 squared)
    for (int q = 0; q < 4; q++) SubTexelPos(s, nb, (q & 1) ? 0.25f : -0.25f, (q >> 1) ? 0.25f : -0.25f, out[q]);
    return SubTexelFootprint(s, nb, 0.25f);
}

} // namespace PhysLampPick
