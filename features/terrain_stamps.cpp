// Dragged lot lamps on the terrain light maps, drawn by the GPU (part of Night Lighting; see terrain_stamps.h).
//
// A lamp of the lot enters when it moves: it joins the bake's exclusion list, and the chunks that baked it at its
// last place are re-rendered without it (one local relight). Each of those chunks takes its stamp in the build that
// shows its re-rendered map; chunks the lamp moves onto, which never baked it, take it at once, and so do all of them for
// a lamp the bake never took (new from the catalog: no relight, nothing to wait for). While it keeps moving,
// the chunks under its old and new light rects are built again (at most every kBumpMs). Still for kQuietMs (and no drag
// engaged), it leaves the exclusion list and one local relight bakes it where it stays; each chunk under it keeps the
// stamp until the build that shows its re-rendered map (a re-render that leaves the map the same counts). A chunk the
// relight does not re-render within kGateMaxMs is taken as done. Dragged again while it returns, it enters again from
// there: a chunk the return's relight has not re-rendered yet (no lamp in its map) takes the stamp at once, one re-rendered
// since (the lamp baked where it was let go) waits for the new relight. A lamp out of the list (picked up again: for a
// few frames) stays as it was; out of it for kGoneMs (another lot, deleted) it goes back to the bake where it was last
// seen, as at a release (a deleted one is then baked nowhere). With the GPU smoothing off every lamp is back in the bake
// at once.
//
// Part of Apex Radiance. Credits: @loinyx

#include "terrain_stamps.h"
#include "lightmap_smooth.h"
#include "terrain_chunk_relight.h"
#include "drag_lite.h"
#include "apex_log.h"
#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <unordered_map>

namespace TerrainStamps {
namespace {
constexpr DWORD kQuietMs = 1500;   // still this long (and no drag engaged): back to the bake
constexpr DWORD kGateMaxMs = 3000; // a chunk the relight did not re-render by then is taken as done
constexpr DWORD kBumpMs = 50;      // the chunks under a moving lamp are built again at most this often
constexpr DWORD kGoneMs = 1000;    // out of the lot's list this long: back to the bake (a lamp picked up again is out for a few frames)
constexpr int kChunk = 256;        // metres (a chunk light map texel is a metre)
using Key = std::pair<int, int>;

enum class Phase { Active, Releasing };
struct State {
    Phase phase = Phase::Active;
    Lamp lamp;
    float rect[4] = {};      // its light rect now (Active) or where it stays (Releasing)
    float builtRect[4] = {}; // the rect the chunks were last built again for
    std::vector<std::pair<Key, uint64_t>> gate; // the chunks the relight re-renders, with their re-render counts then
    float gateRect[4] = {}; // the rect the gate was taken for
    size_t gateLeft = 0;    // gate chunks not re-rendered yet, as last counted
    DWORD since = 0, movedAt = 0, bumpAt = 0, goneAt = 0;
    bool movedSinceBump = false, gone = false; // gone: out of the lot's list since goneAt
};
std::unordered_map<uintptr_t, State> g_states;
struct Seen {
    float pos[3] = {}, rect[4] = {};
};
std::unordered_map<uintptr_t, Seen> g_seen; // each GPU lamp's place last frame
std::vector<uintptr_t> g_last;              // the lights of the last Update (GroundProfile)
long g_entered = 0, g_released = 0, g_refused = 0;
std::string g_lastRefusal;

bool Touches(const Key& k, const float rect[4]) {
    const float x0 = static_cast<float>(k.first) - kChunk / 2, z0 = static_cast<float>(k.second) - kChunk / 2;
    return rect[0] - 1 <= x0 + kChunk && x0 <= rect[2] + 1 && rect[1] - 1 <= z0 + kChunk && z0 <= rect[3] + 1;
}
void PushExcluded() {
    std::vector<uintptr_t> ex;
    for (const auto& [L, s] : g_states)
        if (s.phase == Phase::Active) ex.push_back(L);
    NightTerrainBake::SetExcluded(ex);
}
bool Relight(const float pos[3], const float rect[4], std::string& why) {
    ChunkRelight::Lamp cl;
    cl.x = pos[0], cl.z = pos[2], cl.rects = 1;
    std::memcpy(cl.rect[0], rect, sizeof cl.rect[0]);
    std::string chunks;
    return ChunkRelight::QueueLocal({cl}, why, chunks, true) != 0;
}
void SetGate(State& s, const float rect[4]) {
    std::memcpy(s.gateRect, rect, sizeof s.gateRect);
    s.gate = LightmapSmooth::ChunkVersions(rect);
    s.gateLeft = s.gate.size();
}
// The gate's chunks not re-rendered since (none once the wait is over)
size_t GateLeft(const State& s, DWORD now) {
    if (now - s.since >= kGateMaxMs) return 0;
    size_t left = 0;
    for (const auto& [k, v] : LightmapSmooth::ChunkVersions(s.gateRect))
        for (const auto& [gk, gv] : s.gate) left += gk == k && v <= gv ? 1 : 0;
    return left;
}
} // namespace

void Update(const std::vector<uintptr_t>& all) {
    const DWORD now = GetTickCount();
    // the stamps are drawn by the GPU smoothing only: without it, every lamp stays in the bake
    static const std::vector<uintptr_t> kNone;
    const bool gpu = LightmapSmooth::Enabled() && LightmapSmooth::GpuActive();
    const std::vector<uintptr_t>& lights = gpu ? all : kNone;
    g_last = all;
    bool exclusion = false;
    for (auto it = g_states.begin(); it != g_states.end();) { // a lamp out of the list: picked up again, deleted, another lot
        State& s = it->second;
        const bool listed = std::find(lights.begin(), lights.end(), it->first) != lights.end();
        if (listed) s.gone = false;
        else if (!s.gone) s.gone = true, s.goneAt = now;
        if (listed || (gpu && (s.phase == Phase::Releasing || now - s.goneAt < kGoneMs))) { // (a returning one ends with its gate)
            ++it;
            continue;
        }
        LightmapSmooth::BumpStamps(s.builtRect);
        if (gpu) { // out for kGoneMs: back in the bake where it was last seen, as at a release (gone: the relight takes it out)
            {
                Lamp now_;
                float rect[4];
                const bool takes = NightTerrainBake::StampOf(it->first, now_.col, now_.pos, rect, now_.ground, now_.cone);
                static int s_logs = 0;
                if (++s_logs <= 40)
                    LOG_INFO(std::format("[TerrainStamps] Lamp {:08X} out of the lamp list for {} ms: back to the bake where it was last seen ({:.2f} {:.2f} {:.2f}); the bake "
                                         "{}",
                                         it->first, now - s.goneAt, s.lamp.pos[0], s.lamp.pos[1], s.lamp.pos[2],
                                         takes ? std::format("takes it now at ({:.2f} {:.2f} {:.2f})", now_.pos[0], now_.pos[1], now_.pos[2]) : std::string("no longer takes it")));
            }
            std::string why;
            if (!Relight(s.lamp.pos, s.rect, why)) g_lastRefusal = why;
            s.phase = Phase::Releasing;
            s.since = now;
            SetGate(s, s.rect);
            exclusion = true;
            LightmapSmooth::BumpStamps(s.rect);
            std::memcpy(s.builtRect, s.rect, sizeof s.builtRect);
            ++it;
            continue;
        }
        if (s.phase == Phase::Active) { // the GPU smoothing off: back in the bake at once
            std::string why;
            exclusion = true;
            if (!Relight(s.lamp.pos, s.rect, why)) g_lastRefusal = why;
        }
        it = g_states.erase(it);
    }
    for (auto it = g_seen.begin(); it != g_seen.end();)
        it = std::find(lights.begin(), lights.end(), it->first) == lights.end() ? g_seen.erase(it) : std::next(it);
    for (uintptr_t L : lights) {
        Lamp lamp;
        float rect[4] = {};
        const bool takes = NightTerrainBake::StampOf(L, lamp.col, lamp.pos, rect, lamp.ground, lamp.cone);
        const auto seen = g_seen.find(L);
        const bool moved = takes && seen != g_seen.end() &&
                           std::fabs(seen->second.pos[0] - lamp.pos[0]) + std::fabs(seen->second.pos[1] - lamp.pos[1]) + std::fabs(seen->second.pos[2] - lamp.pos[2]) > 0.01f;
        const Seen before = seen != g_seen.end() ? seen->second : Seen{};
        const bool back = takes && seen == g_seen.end(); // (with a state: back in the list, maybe elsewhere)
        if (takes && (seen == g_seen.end() || moved)) { // (from the last counted place: a slow drag moves less a frame)
            Seen& now_ = g_seen[L];
            std::memcpy(now_.pos, lamp.pos, sizeof now_.pos);
            std::memcpy(now_.rect, rect, sizeof now_.rect);
        }
        auto st = g_states.find(L);
        State released; // (dragged again while it returns: its release state, gate included)
        bool regrab = false;
        if (st != g_states.end() && st->second.phase == Phase::Releasing && moved) { // dragged again: enters again from there
            LightmapSmooth::BumpStamps(st->second.builtRect);
            released = std::move(st->second), regrab = true;
            g_states.erase(st);
            st = g_states.end();
        }
        if (st == g_states.end()) {
            if (!moved || g_states.size() >= NightTerrainBake::kMaxExcluded || !ChunkRelight::LikelyAvailable()) continue;
            // enter: excluded first, so the relight of its last place bakes the chunks without it; a lamp the bake never
            // took is in no chunk's map (nothing to relight or wait for)
            const bool unbaked = !NightTerrainBake::EverBaked(L);
            State s;
            s.lamp = lamp;
            std::memcpy(s.rect, rect, sizeof s.rect);
            std::memcpy(s.builtRect, rect, sizeof s.builtRect);
            if (!unbaked) SetGate(s, before.rect);
            if (regrab) { // a chunk the release relight has not re-rendered yet still bakes it nowhere: stamped at once; one
                          // re-rendered since bakes it at its last place and waits for the relight below (queued again for all)
                std::erase_if(s.gate, [&](const std::pair<Key, uint64_t>& g) {
                    return std::any_of(released.gate.begin(), released.gate.end(), [&](const std::pair<Key, uint64_t>& r) { return r.first == g.first && g.second <= r.second; });
                });
                s.gateLeft = s.gate.size();
            }
            s.since = s.movedAt = s.bumpAt = now;
            g_states[L] = std::move(s);
            PushExcluded();
            std::string why;
            if (!unbaked && !Relight(before.pos, before.rect, why)) {
                g_states.erase(L);
                if (regrab) g_states[L] = std::move(released); // still drawn where the release bake has not landed
                PushExcluded();
                g_refused++;
                g_lastRefusal = why;
                continue;
            }
            g_entered++;
            LightmapSmooth::BumpStamps(before.rect);
            LightmapSmooth::BumpStamps(rect);
            if (g_entered <= 20)
                LOG_INFO(std::format("[TerrainStamps] Lamp {:08X} dragged: its ground light drawn on the GPU ({})", L,
                                     unbaked ? std::string("never baked: nothing re-rendered") : std::format("{} chunks re-rendered without it", g_states[L].gate.size())));
            continue;
        }
        State& s = st->second;
        if (s.phase == Phase::Active) {
            if (takes) {
                s.lamp = lamp;
                std::memcpy(s.rect, rect, sizeof s.rect);
            }
            if (moved || back) s.movedAt = now, s.movedSinceBump = true;
            if (s.movedSinceBump && now - s.bumpAt >= kBumpMs) {
                LightmapSmooth::BumpStamps(s.builtRect);
                LightmapSmooth::BumpStamps(s.rect);
                std::memcpy(s.builtRect, s.rect, sizeof s.builtRect);
                s.bumpAt = now;
                s.movedSinceBump = false;
            }
            if (takes && (now - s.movedAt < kQuietMs || DragLite::Engaged())) continue;
            // release: out of the exclusion list, one relight bakes it where it stays (switched off: nothing to bake); refused,
            // it stays drawn here and is tried again kQuietMs later
            std::string why;
            if (takes && !Relight(s.lamp.pos, s.rect, why)) {
                g_lastRefusal = why;
                s.movedAt = now;
                continue;
            }
            s.phase = Phase::Releasing;
            s.since = now;
            SetGate(s, s.rect);
            if (!takes) s.gate.clear(), s.gateLeft = 0;
            exclusion = true;
            if (++g_released <= 20)
                LOG_INFO(std::format("[TerrainStamps] Lamp {:08X} still for {} ms: back to the bake ({} chunks re-rendered with it)", L, now - s.movedAt,
                                     s.gate.size()));
            LightmapSmooth::BumpStamps(s.builtRect);
            LightmapSmooth::BumpStamps(s.rect);
            std::memcpy(s.builtRect, s.rect, sizeof s.builtRect);
        }
    }
    if (exclusion) PushExcluded();
    for (auto it = g_states.begin(); it != g_states.end();) {
        State& s = it->second;
        // a gate chunk re-rendered (a same map is not built again by itself) or the wait over: its stamp changes
        if (const size_t left = GateLeft(s, now); left != s.gateLeft) {
            s.gateLeft = left;
            LightmapSmooth::BumpStamps(s.rect);
        }
        if (s.phase == Phase::Releasing && s.gateLeft == 0) { // every chunk shows the bake again
            LightmapSmooth::BumpStamps(s.builtRect);
            it = g_states.erase(it);
        } else
            ++it;
    }
}

uint64_t ForChunk(const std::pair<int, int>& key, uint64_t rver, std::vector<Lamp>& out) {
    const DWORD now = GetTickCount();
    uint64_t sig = 0;
    for (const auto& [L, s] : g_states) {
        if (!Touches(key, s.rect)) continue;
        bool waiting = false; // a gate chunk not re-rendered yet
        if (now - s.since < kGateMaxMs)
            for (const auto& [gk, gv] : s.gate)
                if (gk == key && rver <= gv) waiting = true;
        // Active: a chunk that still bakes the lamp at its last place waits; Releasing: a chunk keeps the stamp until it bakes it
        const bool stamp = s.phase == Phase::Active ? !waiting : waiting;
        if (!stamp) continue;
        out.push_back(s.lamp);
        sig = sig * 1099511628211ull ^ static_cast<uint64_t>(L);
    }
    return sig;
}

bool MovedWithin(uintptr_t light, unsigned long ms) {
    const auto it = g_states.find(light);
    return it != g_states.end() && it->second.phase == Phase::Active && GetTickCount() - it->second.movedAt < ms;
}

void Clear() {
    g_states.clear();
    g_seen.clear();
    PushExcluded();
}

const std::vector<uintptr_t>& LastLights() { return g_last; }
std::string LampState(uintptr_t light) {
    const auto it = g_states.find(light);
    if (it == g_states.end()) return "";
    const State& s = it->second;
    const DWORD now = GetTickCount();
    return std::format("{} {} ms, rect ({:.1f} {:.1f} {:.1f} {:.1f}), gate {} chunks ({} not re-rendered){}", s.phase == Phase::Active ? "drawn on the GPU" : "returning to the bake",
                       now - s.since, s.rect[0], s.rect[1], s.rect[2], s.rect[3], s.gate.size(), s.gateLeft, s.gone ? std::format(", out of the list {} ms", now - s.goneAt) : "");
}

std::string Status() {
    size_t active = 0;
    for (const auto& [L, s] : g_states) active += s.phase == Phase::Active ? 1 : 0;
    return std::format("terrain stamps: {} lamps drawn on the GPU ({} returning to the bake); entered {}, released {}, refused {}{}", active,
                       g_states.size() - active, g_entered, g_released, g_refused, g_lastRefusal.empty() ? "" : " (last: " + g_lastRefusal + ")");
}
} // namespace TerrainStamps
