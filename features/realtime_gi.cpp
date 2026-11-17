// Real-time global illumination: module skeleton and the feature "RealtimeGI" (see realtime_gi.h).
//
// Part of Apex Radiance. Credits: @loinyx

#include "realtime_gi.h"
#include "patch_base.h"
#include "apex_log.h"
#include "apex_version.h"
#include "build_flavor.h"
#include "d3d9_hooks.h"
#include "game_addresses.h"
#include "gi_compose.h"
#include "gi_indirect.h"
#include "gi_scene.h"
#include "gi_sched.h"
#include "level_light_share.h"
#include "lot_lighting_motion.h"
#include "lamp_mark_filter.h"
#include "outdoor_delta.h"
#include "penumbra.h"
#include "post_scene.h"
#include "render_callbacks.h"
#include "rig_tracker.h"
#include "rt_outdoor_lamps.h"
#include "shader_cache.h"
#include "unlit_rooms.h"
#include "world_session.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <format>
#include <functional>
#include <unordered_map>
#include <unordered_set>
#include <windows.h>

namespace RealtimeGI {
namespace {

constexpr const char* kLotsNames[] = {"active", "activeAndNeighbours", "all"};
constexpr const char* kCapNames[] = {"off", "class0", "hold"};
constexpr const char* kIndirectNames[] = {"off", "atlas", "atlasDebug"};
constexpr DWORD kLogEveryMs = 10000;

std::array<std::atomic<uint64_t>, static_cast<size_t>(Counter::Count)> g_counters{};
std::atomic<bool> g_running{false};
std::atomic<Backend> g_backend{Backend::D3d9};
uint64_t g_frames = 0; // render thread
DWORD g_lastLog = 0;
Options g_options;       // set by Start
std::string g_startLine; // logged at the first snapshot in a world (it carries lots=)

// The bridge's GI export (R-03 / R-04, patch 0018 of native/remix-bridge: RemixBridge_GiOpen, GiSubmit, GiSolve, GiPoll,
// GiCancel in the bridge client d3d9.dll) as the scheduler's back end. Wire layouts mirrored from the patch's util_gi.h.
struct GiLampWire {
    float position[3];
    float radius;
    float colour[3];
    uint32_t reserved;
    float boxMin[3], boxMax[3]; // the solve's region; min > max: the whole scene
};
static_assert(sizeof(GiLampWire) == 56, "GiLamp of util_gi.h");
constexpr uint32_t kGiSubmitScene = 1, kGiSubmitLamps = 2, kGiSubmitMore = 0x100, kGiSlotBytes = 448 * 1024, kGiLevelStarting = 0xffffffffu;

struct BridgeBackend final : GiSched::Backend {
    using OpenFn = HRESULT(__stdcall*)(uint32_t* level);
    using SubmitFn = HRESULT(__stdcall*)(const void* data, uint32_t size, uint32_t kind, uint32_t* ticket);
    using SolveFn = HRESULT(__stdcall*)(IDirect3DTexture9* texture, uint32_t layer, uint32_t sceneTicket, uint32_t* ticket);
    using TicketFn = HRESULT(__stdcall*)(uint32_t ticket);
    OpenFn open = nullptr;
    SubmitFn submit = nullptr;
    SolveFn solve = nullptr;
    TicketFn poll = nullptr, cancel = nullptr;
    uint32_t level = kGiLevelStarting; // 0 none, 1 compute, 2 rays (the server's answer to GiOpen)
    // rebuild: the active lot's scene built again into `scene` (-1 not readable yet, 0 unchanged, 1 a new blob, with its
    // edit); target: the layer slot a solve writes; boxOf: the touch box when the server has none (no 0020 patch: the
    // lamp's reach); tilesOf: a world box's tiles and stories; landed: a layer arrived in its slot
    std::function<int(GiSched::Box& edit, bool& whole)> rebuild;
    std::vector<uint8_t> scene;
    std::function<IDirect3DTexture9*(uint32_t layer)> target;
    std::function<GiSched::Box(const GiSched::LampIn&)> boxOf;
    std::function<GiSched::Box(const float lo[3], const float hi[3])> tilesOf;
    std::function<void(uint32_t layer)> landed;
    // The region a solve is scoped to (R-06, patch 0020): the lamp's last touch box grown by kScopeGrow, only while
    // that box was read for the same scene and the same lamp place. A first solve, a moved lamp and any scene change
    // solve the whole scene: a box clips the light that reaches through doors (R-06: room plus one room drops 8-11 % of
    // the lit texels), and an edit can open a new way out of the old box.
    using ReadTouchFn = HRESULT(__stdcall*)(uint32_t lamp, float* box, uint32_t* texels);
    ReadTouchFn readTouch = nullptr;
    struct Touch {
        float lo[3] = {}, hi[3] = {}, pos[3] = {};
        uint32_t scene = 0; // sceneTicket it was read for; 0: none
    };
    std::vector<Touch> touch; // a layer
    static constexpr float kScopeGrow = 0.5f; // metres; [I] a texel and a margin around the lit texels
    uint32_t sceneTicket = 0, scenePending = 0;
    size_t sceneSent = 0;
    bool sending = false, sendWhole = false;
    GiSched::Box sendEdit;
    std::unordered_map<uintptr_t, uint32_t> index; // lamp -> layer (lamp i of the table is layer i)
    std::vector<GiLampWire> table;
    bool tableDirty = false;
    std::unordered_map<uint32_t, GiSched::LampIn> tickets;

    bool Resolve() {
        const HMODULE m = GetModuleHandleW(L"d3d9.dll");
        if (!m) return false;
        open = reinterpret_cast<OpenFn>(GetProcAddress(m, "RemixBridge_GiOpen"));
        submit = reinterpret_cast<SubmitFn>(GetProcAddress(m, "RemixBridge_GiSubmit"));
        solve = reinterpret_cast<SolveFn>(GetProcAddress(m, "RemixBridge_GiSolve"));
        poll = reinterpret_cast<TicketFn>(GetProcAddress(m, "RemixBridge_GiPoll"));
        cancel = reinterpret_cast<TicketFn>(GetProcAddress(m, "RemixBridge_GiCancel"));
        readTouch = reinterpret_cast<ReadTouchFn>(GetProcAddress(m, "RemixBridge_GiReadTouch")); // (optional: patch 0020)
        return open && submit && solve && poll && cancel;
    }
    bool Ready() const { return level != kGiLevelStarting && level != 0; }
    // A new active lot: the lamp table, the tickets and the scene start over (the scheduler cancelled its bakes)
    void ResetLot() {
        index.clear(), table.clear(), tickets.clear(), scene.clear(), touch.clear();
        sceneTicket = scenePending = 0, sceneSent = 0, sending = false, tableDirty = false;
    }
    void BeginFrame(uint32_t) override {
        if (level == kGiLevelStarting && open) { // GiOpen never waits: S_FALSE until the worker has its device
            uint32_t l = 0;
            const HRESULT hr = open(&l);
            if (hr == S_OK) level = l;
            else if (FAILED(hr)) level = 0;
        }
        if (scenePending) {
            const HRESULT hr = poll(scenePending);
            if (hr != S_FALSE) sceneTicket = hr == S_OK ? scenePending : 0, scenePending = 0;
        }
    }
    // The scene built again and set against the last (an unchanged one is not sent and re-bakes nothing; the edit narrows
    // to what changed), then sent in parts of a submit slot; false (the scheduler asks again next frame) while the lot
    // cannot be read or the slots are busy
    bool SubmitScene(GiSched::Box& edit, bool& whole) override {
        if (!Ready() || !rebuild) return false;
        if (!sending) {
            GiSched::Box e;
            bool w = false;
            const int r = rebuild(e, w);
            if (r < 0) return false;
            if (r == 0 && (sceneTicket || scenePending)) {
                edit = {}, whole = false;
                return true;
            }
            sending = true, sendEdit = e, sendWhole = w, sceneSent = 0;
        }
        sceneTicket = 0;
        while (sceneSent < scene.size()) {
            const uint32_t n = static_cast<uint32_t>(std::min<size_t>(kGiSlotBytes, scene.size() - sceneSent));
            const bool last = sceneSent + n == scene.size();
            uint32_t t = 0;
            const HRESULT hr = submit(scene.data() + sceneSent, n, kGiSubmitScene | (last ? 0 : kGiSubmitMore), &t);
            if (hr == D3DERR_WASSTILLDRAWING) return false;
            if (FAILED(hr)) {
                sceneSent = 0, sending = false;
                return false;
            }
            sceneSent += n;
            if (last) scenePending = t;
        }
        sceneSent = 0, sending = false;
        edit = sendEdit, whole = sendWhole;
        return true;
    }
    // The lamp table first when it changed (unit colour: switch, dim and colour stay in the compose; R-04 solves a lamp
    // again when its 56 bytes change), then the indirect layer into its target
    bool Solve(const GiSched::LampIn& lamp, uint32_t& ticket) override {
        if (!sceneTicket || !target) return false;
        auto [it, fresh] = index.try_emplace(lamp.id, static_cast<uint32_t>(table.size()));
        if (fresh) table.push_back({}), touch.push_back({});
        if (it->second > 0xFFFF) return false;
        // the lamp ball of Shadow softness, and the region its layer may light (texels outside get none and cost no gather)
        GiLampWire w{{lamp.pos[0], lamp.pos[1], lamp.pos[2]}, Penumbra::Radius(), {1, 1, 1}, 0, {1, 1, 1}, {0, 0, 0}};
        if (const Touch& t = touch[it->second]; t.scene == sceneTicket && !std::memcmp(t.pos, lamp.pos, sizeof t.pos))
            for (int k = 0; k < 3; k++) w.boxMin[k] = t.lo[k] - kScopeGrow, w.boxMax[k] = t.hi[k] + kScopeGrow;
        if (std::memcmp(&w, &table[it->second], sizeof w)) table[it->second] = w, tableDirty = true;
        if (tableDirty) {
            uint32_t t = 0;
            const uint32_t bytes = static_cast<uint32_t>(table.size() * sizeof(GiLampWire));
            if (bytes > kGiSlotBytes || submit(table.data(), bytes, kGiSubmitLamps, &t) != S_OK) return false;
            tableDirty = false; // (the server takes the table before the solve sent after it)
        }
        IDirect3DTexture9* tex = target(it->second);
        if (!tex || solve(tex, it->second, sceneTicket, &ticket) != S_OK) return false;
        tickets[ticket] = lamp;
        return true;
    }
    int Poll(uint32_t ticket, GiSched::Box& touched) override {
        const HRESULT hr = poll(ticket);
        if (hr == S_FALSE) return 0;
        const auto it = tickets.find(ticket);
        if (it != tickets.end()) {
            const auto l = index.find(it->second.id);
            float box[6];
            uint32_t texels = 0;
            if (hr == S_OK && l != index.end() && readTouch && readTouch(l->second, box, &texels) == S_OK) {
                Touch& t = touch[l->second];
                std::memcpy(t.lo, box, sizeof t.lo), std::memcpy(t.hi, box + 3, sizeof t.hi);
                std::memcpy(t.pos, it->second.pos, sizeof t.pos), t.scene = sceneTicket;
                touched = !texels ? GiSched::Box{} : tilesOf ? tilesOf(t.lo, t.hi) : boxOf ? boxOf(it->second) : GiSched::Box{};
            } else if (boxOf) {
                touched = boxOf(it->second);
            }
            if (hr == S_OK && l != index.end() && landed) landed(l->second);
            tickets.erase(it);
        }
        return hr == S_OK ? 1 : -1;
    }
    void Cancel(uint32_t ticket) override {
        cancel(ticket);
        tickets.erase(ticket);
    }
};
BridgeBackend g_bridge;

bool ServerExport() { return g_bridge.Resolve(); }
// Ray queries once GiOpen answered with the rays level; false before (Start takes serverCompute then, and the back end is
// chosen again when the answer comes: OnPresent)
bool ServerRayQuery() { return g_bridge.level == 2; }

// ---- update scheduler (S2-01, render thread): the active lot's lamp layers, on the bridge's back end, or with developer
// debugFakeSolve on the fake one (a layer lands 3 frames after its bake starts, touching the lamp's reach, at most 6 m)
GiSched::FakeBackend g_fake;
GiSched::Scheduler g_sched;
bool g_schedOn = false;
uint64_t g_schedLot = 0;
uint32_t g_schedSerial = 0, g_schedFrame = 0;

// ---- lamp snapshot (render thread) ----
constexpr int kRefreshFrames = 20;     // a snapshot at least every 20 frames, as Apex's other lamp readers
constexpr int kChangeLinesPerLog = 16; // developer log: change lines a 10 s window
constexpr int kStartWaitFrames = 600;  // the start line waits this long in a world for the active lot
std::vector<Lamp> g_lamps;
std::vector<Change> g_changes;
std::unordered_map<uintptr_t, bool> g_lastRead; // each light's on state at the last read (DebounceOn)
uint32_t g_serial = 0;
unsigned long g_editSerial = 0;
int g_sinceSnap = 0, g_giLots = 0, g_changeLines = 0, g_worldFrames = 0;
bool g_pending = false, g_dumpDue = false;
uint64_t g_activeLot = 0;

// Light enumeration (0x006ACF70, stdcall(visitor), visitor vtable[0] = thiscall(visitor, Light*)), as LotLightBridge's
std::vector<uintptr_t> g_enum;
void __fastcall EnumVisit(void*, void*, uintptr_t light) {
    if (g_enum.size() < 100000) g_enum.push_back(light);
}
void* g_enumVtbl[1] = {reinterpret_cast<void*>(&EnumVisit)};
struct EnumVisitor {
    void** vtbl;
} g_enumVisitor{g_enumVtbl};
uintptr_t g_enumFn = 0;
bool g_enumChecked = false, g_enumOk = false;

bool CallEnum() { // POD only (SEH)
    __try {
        reinterpret_cast<void(__stdcall*)(void*)>(g_enumFn)(&g_enumVisitor);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// 1 read, 0 no enumeration on this build, -1 a fault
int Enumerate() {
    if (!g_enumChecked && GameAddr::Resolved()) {
        g_enumChecked = true;
        g_enumFn = GameAddr::Get(GameAddr::Id::EnumLights);
        static const BYTE expect[] = {0xE8, 0x2B, 0x36, 0x00, 0x00, 0x8B, 0x4C, 0x24, 0x04, 0x51, 0x68, 0x40, 0xCF, 0x6A, 0x00}; // Steam 1.67.2
        g_enumOk = g_enumFn && (!GameAddr::IsFixed() || std::memcmp(reinterpret_cast<const void*>(g_enumFn), expect, sizeof(expect)) == 0);
        if (!g_enumOk) LOG_WARNING("[RealtimeGI] The light enumeration was not found: no lamp snapshot");
    }
    if (!g_enumOk) return 0;
    g_enum.clear();
    return CallEnum() ? 1 : -1;
}

// The fields of research/01-lamp-data.md section 2. 1 read, 0 not a light class (3..11), -1 a fault. POD only (SEH)
int ReadRaw(uintptr_t L, Lamp& o) {
    __try {
        const BYTE* p = reinterpret_cast<const BYTE*>(L);
        o.light = L;
        o.type = *reinterpret_cast<const int*>(p + 0xB0);
        if (o.type < 3 || o.type > 11) return 0;
        o.lot = *reinterpret_cast<const uint32_t*>(p + 0xC0) | static_cast<uint64_t>(*reinterpret_cast<const uint32_t*>(p + 0xC4)) << 32;
        o.room = *reinterpret_cast<const int*>(p + 0x08);
        o.story = *reinterpret_cast<const int*>(p + 0xD0);
        o.flags = p[0x100];
        const float* f = reinterpret_cast<const float*>(p);
        for (int i = 0; i < 3; i++) o.pos[i] = f[0x120 / 4 + i], o.base[i] = f[0xF0 / 4 + i];
        o.dimmer = f[0x10 / 4], o.fade = f[0x20 / 4], o.range = f[0x130 / 4];
        if (o.type == 4) {
            for (int i = 0; i < 3; i++) o.axis[i] = f[0x170 / 4 + i];
            o.off1 = f[0x158 / 4], o.scale = f[0x154 / 4];
        } else if (o.type == 5) {
            for (int i = 0; i < 3; i++) o.axis[i] = f[0x1A0 / 4 + i], o.spill[i] = f[0x150 / 4 + i];
            o.off1 = f[0x174 / 4], o.off2 = f[0x170 / 4];
        }
        return 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
bool RawOn(const Lamp& l) { return (l.flags & 0x61) == 0x61 && l.dimmer > 0.0f && l.fade > 0.0f; }
bool Same(float a, float b) { return a == b || std::fabs(a - b) <= 1e-4f * std::max(1.0f, std::fabs(b)); }
bool Same3(const float* a, const float* b) { return Same(a[0], b[0]) && Same(a[1], b[1]) && Same(a[2], b[2]); }
bool SameValue(const Lamp& a, const Lamp& b) {
    return a.type == b.type && a.lot == b.lot && a.room == b.room && a.story == b.story && Same3(a.base, b.base) && a.dimmer == b.dimmer && a.fade == b.fade &&
           a.range == b.range && Same3(a.axis, b.axis) && a.off1 == b.off1 && a.off2 == b.off2 && a.scale == b.scale && Same3(a.spill, b.spill);
}

// The lots covered. The active lot: the lot of Apex's registered lamps of the active lot (OutdoorDelta::LotLamps)
void CountLots() {
    std::vector<OutdoorDelta::LotLamp> table;
    OutdoorDelta::LotLamps(table);
    std::unordered_map<uintptr_t, uint64_t> lotOf;
    for (const Lamp& l : g_lamps) lotOf[l.light] = l.lot;
    g_activeLot = 0;
    for (const auto& t : table)
        if (const auto it = lotOf.find(t.light); it != lotOf.end() && it->second) {
            g_activeLot = it->second;
            break;
        }
    if (g_options.lots == 0) {
        g_giLots = g_activeLot ? 1 : 0;
        return;
    }
    // shortcut: activeAndNeighbours counts every lot with a lamp, as all does; S4-03 adds the neighbour set
    std::unordered_set<uint64_t> lots;
    for (const Lamp& l : g_lamps)
        if (l.lot) lots.insert(l.lot);
    g_giLots = static_cast<int>(lots.size());
}

// debugLampTable (developer mode): every lamp of the active lot (every lot lamp while it is not known) read again and set
// against the snapshot (the game's fields now), against Apex's GPU law of it (RtOutdoorLamps::OwnLotLaw: position,
// effective colour +0xE0, cone), and every lamp of Apex's table of the active lot (OutdoorDelta::LotLamps) looked up in
// the snapshot. Q2: 0 differences.
void DumpTable() {
    std::unordered_set<uintptr_t> inSnap;
    int lamps = 0, laws = 0, diffs = 0;
    for (const Lamp& l : g_lamps) {
        inSnap.insert(l.light);
        if (!l.lot || (g_activeLot && l.lot != g_activeLot)) continue;
        ++lamps;
        std::string why;
        Lamp now;
        if (ReadRaw(l.light, now) != 1) why += " gone";
        else {
            if (!Same3(now.pos, l.pos)) why += " pos";
            if (!SameValue(now, l)) why += " fields";
            if (RawOn(now) != l.on) why += " on";
        }
        if (l.hasLaw) {
            ++laws;
            const bool lit = (l.flags & 0x20) != 0; // +0xE0 = +0xF0 x +0x10 while lit, else 0 (0x006BDCA0, 0x006BDD10)
            float e0[3];
            for (int i = 0; i < 3; i++) e0[i] = lit ? std::max(l.base[i] * l.dimmer, 0.0f) : 0.0f;
            if (!Same3(l.law.pos, l.pos)) why += " lawPos";
            if (!Same3(l.law.colour, e0)) why += " lawColour";
            if (l.type == 4 && !(Same3(l.law.axis, l.axis) && Same(l.law.sc1, l.scale) && Same(l.law.off1, l.off1))) why += " lawCone";
            if (l.type == 5 && l.law.sc2 != 0.0f && !(Same3(l.law.axis, l.axis) && Same(l.law.off1, l.off1) && Same(l.law.off2, l.off2) && Same3(l.law.spill, l.spill)))
                why += " lawCone";
        }
        if (!why.empty()) ++diffs;
        LOG_INFO(std::format("[RealtimeGI] Lamp {:08X} type={} lot={:X} room={} story={} on={} pos=({:.3f},{:.3f},{:.3f}) colour=({:.3f},{:.3f},{:.3f}) "
                             "range={:.1f} cone=({:.3f},{:.3f},{:.3f};{:.3f},{:.3f},{:.3f}) law={}{}{}",
                             static_cast<uint32_t>(l.light), l.type, l.lot, l.room, l.story, l.on ? 1 : 0, l.pos[0], l.pos[1], l.pos[2], l.colour[0], l.colour[1],
                             l.colour[2], l.range, l.axis[0], l.axis[1], l.axis[2], l.off1, l.off2, l.scale, l.hasLaw ? 1 : 0, why.empty() ? "" : " diff:", why));
    }
    std::vector<OutdoorDelta::LotLamp> table;
    OutdoorDelta::LotLamps(table);
    for (const auto& t : table)
        if (!inSnap.count(t.light)) {
            ++diffs;
            LOG_INFO(std::format("[RealtimeGI] Lamp {:08X} diff: in Apex's lamp table, not in the snapshot", static_cast<uint32_t>(t.light)));
        }
    if (diffs) Add(Counter::LampTableDiffs, static_cast<uint32_t>(diffs));
    const std::string line =
        std::format("[RealtimeGI] Lamp table: lights={} lot={:X} lamps={} laws={} table={} diffs={}", g_lamps.size(), g_activeLot, lamps, laws, table.size(), diffs);
    if (diffs) LOG_WARNING(line);
    else LOG_INFO(line);
}

void Snapshot() {
    const int e = Enumerate();
    if (e < 0) Add(Counter::ReadFaults);
    if (e <= 0) return;
    std::unordered_map<uintptr_t, size_t> was;
    for (size_t i = 0; i < g_lamps.size(); i++) was[g_lamps[i].light] = i;
    std::vector<Lamp> next;
    next.reserve(g_enum.size());
    std::unordered_map<uintptr_t, bool> reads;
    std::vector<Change> changes;
    g_pending = false;
    for (const uintptr_t L : g_enum) {
        Lamp n;
        const int r = ReadRaw(L, n);
        if (r < 0) Add(Counter::ReadFaults);
        if (r <= 0) continue;
        const bool raw = RawOn(n);
        reads[L] = raw;
        const auto it = was.find(L);
        const Lamp* old = it != was.end() ? &g_lamps[it->second] : nullptr;
        const auto lr = g_lastRead.find(L);
        n.on = old ? DebounceOn(old->on, lr != g_lastRead.end() ? lr->second : old->on, raw) : raw;
        g_pending |= n.on != raw; // (read again next frame)
        for (int i = 0; i < 3; i++) n.colour[i] = n.on ? n.base[i] * n.dimmer * n.fade : 0.0f;
        n.hasLaw = n.lot && RtOutdoorLamps::OwnLotLaw(L, n.law);
        if (!old) changes.push_back({L, n.lot, LampChange::Added});
        else if (old->on != n.on) changes.push_back({L, n.lot, LampChange::Switched});
        else if (!Same3(old->pos, n.pos)) changes.push_back({L, n.lot, LampChange::Moved});
        else if (!SameValue(*old, n)) changes.push_back({L, n.lot, LampChange::Value});
        if (old) was.erase(it);
        next.push_back(n);
    }
    for (const auto& [L, i] : was) changes.push_back({L, g_lamps[i].lot, LampChange::Removed});
    const bool first = g_lamps.empty();
    g_lamps.swap(next);
    g_lastRead.swap(reads);
    if (!changes.empty()) {
        g_changes.swap(changes);
        ++g_serial;
        Add(Counter::LampChanges, static_cast<uint32_t>(g_changes.size()));
        g_dumpDue = true;
        if (!kPublicBuild && !first)
            for (const Change& c : g_changes) {
                if (g_changeLines++ >= kChangeLinesPerLog) break;
                LOG_INFO(std::format("[RealtimeGI] Lamp change: {} light={:08X} lot={:X}", kLampChangeNames[static_cast<int>(c.kind)], static_cast<uint32_t>(c.light), c.lot));
            }
    }
    const int lotsBefore = g_giLots;
    CountLots();
    if (g_startLine.empty() && g_giLots != lotsBefore && !kPublicBuild) LOG_INFO(std::format("[RealtimeGI] Lots: {}", g_giLots));
}

// A lamp's reach for its touch box and solve region: its range, 1 to 6 m
float Reach(uintptr_t id) {
    float range = 6.0f;
    for (const Lamp& x : g_lamps)
        if (x.light == id) range = std::min(range, std::max(x.range, 1.0f));
    return range;
}
GiSched::LampIn SchedLamp(const Lamp& l) { return {l.light, {l.pos[0], l.pos[1], l.pos[2]}, l.story, l.on}; }

// ---- scene, layer slots and compose (S2-01b, render thread, bridge back end) ----
// The scene: GiScene::Build of the GPU room lamps' kept occluders (RtOutdoorLamps::KeptLot: the active lot's open-top
// proxy, any thread) with the floor colours as floor reflectance. Each layer has two slots (shown, incoming for the
// crossfade) in the scene's atlas layout; the compose sums them into the irradiance atlas the lighting reads.
// shortcut: KeptLot is taken as the active lot's (both follow the GPU room lamps' active lot); its tracker is not compared.
constexpr float kFloorScale = 0.6f; // reflectance of a white floor colour (a bare tile); [I] until S6-01's look constants
GiScene::Built g_built;
uint32_t g_seenGen = 0;
uint64_t g_seenFloors = 0;
std::vector<std::pair<int, std::vector<uint32_t>>> g_floors; // (story, FloorColours)
struct LayerSlots {
    IDirect3DTexture9* slot[2] = {};
    int shown = 0;
    bool fading = false; // slot[1 - shown] landed and fades in (GiSched Blend)
};
std::vector<LayerSlots> g_layers;
IDirect3DTexture9* g_irradiance = nullptr;
IDirect3DDevice9* g_dev = nullptr;
IDirect3DPixelShader9* g_composePs = nullptr;
GiCompose::Composer g_composer;
// (S2-05) the atlas's floor and ceiling texels by place (gi_indirect.h), filled from the atlas after each compose
GiIndirect::Grid g_grid;
IDirect3DTexture9* g_gridTex = nullptr;
IDirect3DTexture9* g_flagsTex = nullptr; // (S2-06) the grid's bleed flags (MANAGED, made from g_grid.flags on first use)
float g_lotRows[3][4] = {};               // (S2-06) the GI lot's worldToLot by column, for the wall VS copy
std::unordered_map<int, std::pair<uint32_t, std::array<float, 4>>> g_baseOf; // story -> (frame read, base colour, .w 1 = known)
IDirect3DPixelShader9* g_scatterPs = nullptr;
bool g_gridDirty = false;
bool g_gridFilled = false;
uint64_t g_readyHash = 0;
uint32_t g_readyTexels = 0;
std::unordered_map<const void*, std::pair<int, int>> g_mapLayer; // a game map -> (story, kind) of the GI lot
uint32_t g_mapLayerFrame = 0;
// (S2-06 item 8) The rig write: a rig-lit model (object, Sim) reads the grid at its rig centre (GiIndirect::RigPixel). Its atlas
// texel goes through a probe pass (Scatter into g_probeTex, one pixel a rig) read by the bridge's asynchronous read-back (patch
// 0017); the value lands in the rig's vertex-light slots at its next bind (OnRigBind, render thread), never between binds.
std::shared_ptr<const RtLampProxy::Lot> g_rigLot;     // the lot the grid was built from
std::unordered_map<uint32_t, uint32_t> g_pointOfGrid; // grid pixel -> index in g_grid.points
std::vector<float> g_pointReady;                      // FillGrid's ready weight a point
uint32_t g_gridSerial = 1, g_gridGen = 1;             // grow at each grid fill, at each new grid
struct RigEntry {
    float centre[3] = {};
    int32_t point = -1;  // g_grid.points index, -1 none (off the lot, no floor layer)
    uint32_t gen = 0;    // g_gridGen the point was found in
    uint32_t serial = 0; // g_gridSerial the value was read at (0 none yet)
    uint32_t seen = 0;   // g_rigFrame of the last bind
    bool probing = false, wrote = false;
    float value[3] = {};
    float written[8][4] = {}; // the last write (the slots at +0x90)
};
std::unordered_map<uintptr_t, RigEntry> g_rigs;
uint32_t g_rigFrame = 0;
IDirect3DTexture9* g_probeTex = nullptr;   // kProbeW x kProbeH, A16B16G16R16F render target
IDirect3DSurface9* g_probeMem = nullptr;   // its SYSTEMMEM copy
std::vector<uintptr_t> g_probeRigs;        // the rigs of the read in flight, by pixel
uint32_t g_probeTicket = 0, g_probeWait = 0, g_probeSerial = 0;
using ReadAsyncFn = HRESULT(__stdcall*)(IDirect3DSurface9*, IDirect3DSurface9*, uint32_t*);
using ReadTicketFn = HRESULT(__stdcall*)(uint32_t);
ReadAsyncFn g_readAsync = nullptr;
ReadTicketFn g_pollRead = nullptr, g_cancelRead = nullptr;
bool g_rigOff = false; // no asynchronous read-back here: the rigs keep the game's values
constexpr uint32_t kProbeW = 1024, kProbeH = 16; // 16384 rigs a read, 128 KB (one bridge slot holds 256 KB)
constexpr float kRigCeiling = 1.0f;              // [I] a channel; the game's cap [0x011D0BA8] is not read yet
const ShaderCache::Id kScatterPs = [] {
    ShaderCache::Desc d;
    d.tag = "RealtimeGI ScatterPS";
    d.source = GiCompose::kScatterShader;
    d.sourceName = "gi_scatter.hlsl";
    d.target = "ps_2_0";
    d.priority = 2;
    return ShaderCache::Add(std::move(d));
}();
const ShaderCache::Id kComposePs = [] {
    ShaderCache::Desc d;
    d.tag = "RealtimeGI ComposePS";
    d.source = GiCompose::kPixelShader;
    d.sourceName = "gi_compose.hlsl";
    d.target = "ps_2_0";
    d.priority = 2;
    return ShaderCache::Add(std::move(d));
}();

void ReleaseLayers() {
    for (LayerSlots& l : g_layers)
        for (IDirect3DTexture9*& t : l.slot)
            if (t) t->Release(), t = nullptr;
    g_layers.clear();
    if (g_irradiance) g_irradiance->Release(), g_irradiance = nullptr;
    if (g_gridTex) g_gridTex->Release(), g_gridTex = nullptr;
    g_gridFilled = false, g_gridDirty = true;
}
// A DEFAULT-pool float target of the atlas's size, cleared (a first layer fades in from 0)
IDirect3DTexture9* NewAtlas() {
    if (!g_dev || !g_built.height) return nullptr;
    IDirect3DTexture9* t = nullptr;
    if (FAILED(g_dev->CreateTexture(GiScene::kAtlasWidth, g_built.height, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &t, nullptr))) return nullptr;
    IDirect3DSurface9* s = nullptr;
    if (SUCCEEDED(t->GetSurfaceLevel(0, &s))) g_dev->ColorFill(s, nullptr, 0), s->Release();
    Add(Counter::DeviceCalls, 2);
    return t;
}
// The slot a solve of `layer` writes: the one not shown (the scheduler issues no solve while a layer fades)
IDirect3DTexture9* SlotFor(uint32_t layer) {
    if (g_layers.size() <= layer) g_layers.resize(layer + 1);
    LayerSlots& l = g_layers[layer];
    for (IDirect3DTexture9*& t : l.slot)
        if (!t) t = NewAtlas();
    return l.slot[1 - l.shown];
}

// Each story's floor colours of the kept lot; their hash (a repaint changes it, the proxy does not)
uint64_t ReadFloors(const RtLampProxy::Lot& lot, std::vector<std::pair<int, std::vector<uint32_t>>>& out) {
    out.clear();
    const uintptr_t tracker = LevelLightShare::TrackerOfLot(g_schedLot);
    uint64_t h = 1469598103934665603ull;
    for (const RtLampProxy::Story& s : lot.stories) {
        std::vector<uint32_t> c;
        if (!LevelLightShare::FloorColours(tracker, s.story, s.w, s.h, c)) continue;
        for (const uint32_t v : c) h = (h ^ v) * 1099511628211ull;
        out.emplace_back(s.story, std::move(c));
    }
    return h;
}
float Linear(uint32_t c, int shift) { return std::pow(static_cast<float>((c >> shift) & 0xFF) / 255.0f, 2.2f); }

// The tiles and stories of the scene's texels inside a world box (a touch box the server read back)
GiSched::Box TilesOf(const float lo[3], const float hi[3]) {
    GiSched::Box b;
    for (const auto& [key, slot] : g_built.slots) {
        const float* p = slot.world;
        if (p[0] < lo[0] || p[1] < lo[1] || p[2] < lo[2] || p[0] > hi[0] || p[1] > hi[1] || p[2] > hi[2]) continue;
        b = b.Union(GiSched::Around(p, slot.story, 0.0f));
    }
    return b;
}

// BridgeBackend::rebuild: -1 no kept lot yet, 0 the same scene, 1 a new blob (edit: what changed; whole: the atlas grew)
int RebuildScene(GiSched::Box& edit, bool& whole) {
    uint32_t gen = 0;
    const std::shared_ptr<const RtLampProxy::Lot> lot = RtOutdoorLamps::KeptLot(gen);
    if (!lot) return -1;
    g_seenFloors = ReadFloors(*lot, g_floors);
    g_seenGen = gen;
    GiScene::Input in;
    in.lot = lot.get();
    // floors: the quadrant's colour, linear, scaled (a story's colours are by tile of its grid width)
    std::vector<std::pair<int, int>> widths;
    for (const RtLampProxy::Story& s : lot->stories) widths.emplace_back(s.story, s.w);
    in.reflect = [&widths](GiScene::Surface s, int story, int ix, int iz, int q, const float*, const float*, float rgb[3]) {
        if (s != GiScene::Surface::Floor) return; // (walls and ceilings: no colour in the lighting data, S0-12)
        int w = 0;
        for (const auto& [st, sw] : widths)
            if (st == story) w = sw;
        for (const auto& [st, c] : g_floors) {
            const size_t i = (static_cast<size_t>(iz) * w + ix) * 4 + q;
            if (st != story || ix < 0 || iz < 0 || ix >= w || i >= c.size() || !c[i]) continue;
            for (int k = 0; k < 3; k++) rgb[k] = std::min(0.9f, kFloorScale * Linear(c[i], 16 - 8 * k));
        }
    };
    GiScene::Options o;
    GiScene::Built next;
    GiSched::Box e;
    bool w = false;
    GiScene::Build(in, o, g_built.height ? &g_built : nullptr, next, &e, &w);
    const bool same = g_built.height && e.Empty() && !w;
    if (w) ReleaseLayers(); // (the atlas changed size: every slot again; the scheduler re-bakes every layer)
    g_built = std::move(next);
    if (!same && g_options.indirect) {
        const uint32_t gw = g_grid.w, gh = g_grid.h;
        int lotW = 0, lotH = 0;
        for (const RtLampProxy::Story& st : lot->stories) lotW = std::max(lotW, st.w), lotH = std::max(lotH, st.h);
        if (!GiIndirect::Build(g_built, o.density, lotW, lotH, g_grid, lot.get())) g_grid = {};
        g_pointOfGrid.clear(), g_pointReady.clear(), ++g_gridGen, g_rigLot = g_grid.Empty() ? nullptr : lot;
        for (uint32_t i = 0; i < g_grid.points.size(); i++) g_pointOfGrid[g_grid.points[i].grid] = i;
        for (int k = 0; k < 3; k++)
            for (int j = 0; j < 4; j++) g_lotRows[k][j] = lot->worldToLot[j][k];
        if (g_flagsTex) g_flagsTex->Release(), g_flagsTex = nullptr;
        if ((g_grid.w != gw || g_grid.h != gh) && g_gridTex) g_gridTex->Release(), g_gridTex = nullptr, g_gridFilled = false;
        g_gridDirty = true, g_mapLayer.clear();
    }
    if (same) return 0;
    g_bridge.scene = g_built.blob;
    edit = e, whole = w;
    return 1;
}

// After the scheduler's Tick: ended fades show their new slot, then the frame's tiles are composed again
void ComposeFrame() {
    for (const auto& [id, layer] : g_bridge.index)
        if (layer < g_layers.size() && g_layers[layer].fading && g_sched.Blend(id) >= 1.0f) g_layers[layer].shown ^= 1, g_layers[layer].fading = false;
    const std::vector<uint64_t>& tiles = g_sched.ComposeTiles();
    if (tiles.empty() || !g_dev || !g_built.height) return;
    if (!g_irradiance) g_irradiance = NewAtlas();
    if (!g_composePs && ShaderCache::CreatePixelShader(g_dev, kComposePs, &g_composePs) != ShaderCache::Result::Ok) g_composePs = nullptr;
    if (!g_irradiance || !g_composePs) return;
    std::unordered_map<uintptr_t, const Lamp*> lamps;
    for (const Lamp& l : g_lamps) lamps[l.light] = &l;
    std::vector<uint32_t> clear;
    std::unordered_map<uintptr_t, size_t> at;
    std::vector<GiCompose::LampDraw> draws;
    for (const uint64_t tile : tiles) {
        const auto px = g_built.tiles.find(tile);
        if (px == g_built.tiles.end()) continue;
        clear.insert(clear.end(), px->second.begin(), px->second.end());
        const std::vector<uintptr_t>* ids = g_sched.TileLamps(tile);
        for (size_t k = 0; ids && k < ids->size(); k++) {
            const uintptr_t id = (*ids)[k];
            const auto layer = g_bridge.index.find(id);
            const auto lamp = lamps.find(id);
            if (layer == g_bridge.index.end() || layer->second >= g_layers.size() || lamp == lamps.end() || !g_sched.HasLayer(id)) continue;
            const Lamp& L = *lamp->second;
            if (!(L.colour[0] > 0 || L.colour[1] > 0 || L.colour[2] > 0)) continue; // (off: adds nothing)
            auto [it, fresh] = at.try_emplace(id, draws.size());
            if (fresh) {
                const LayerSlots& s = g_layers[layer->second];
                GiCompose::LampDraw d;
                d.shown = s.slot[s.shown];
                d.incoming = s.fading ? s.slot[1 - s.shown] : nullptr;
                d.blend = g_sched.Blend(id);
                // shortcut: the snapshot's colour (base x dimmer x fade) times the unit-colour layer; the game's law and the
                // look constants (S6-01) are not folded in yet
                std::memcpy(d.colour, L.colour, sizeof d.colour);
                draws.push_back(std::move(d));
            }
            draws[it->second].pixels.insert(draws[it->second].pixels.end(), px->second.begin(), px->second.end());
        }
    }
    int n = 0;
    GiCompose::Calls calls;
    calls.setTexture = &D3D9Hooks::CallOriginalSetTexture, calls.drawPrimitive = &D3D9Hooks::CallOriginalDrawPrimitive;
    g_composer.Compose(g_dev, g_composePs, g_irradiance, GiScene::kAtlasWidth, g_built.height, clear, draws, calls, &n);
    Add(Counter::Draws, static_cast<uint32_t>(n));
    g_gridDirty = true;
}

// (S2-05) The grid the lighting reads (gi_indirect.h): every floor and ceiling texel's atlas pixel copied to its place, with
// its tile's ready weight: 1 when every lamp of the lot that is on and may light the tile (its reach, as the touch box
// without the server's) has a layer, else 0 (the copy keeps the game's ambient there). Filled again after a compose, a
// new scene or a change of the ready tiles.
void FillGrid() {
    if (!g_options.indirect || g_grid.Empty() || !g_dev || !g_irradiance || !g_built.height) return;
    std::unordered_set<uint64_t> waiting;
    for (const Lamp& l : g_lamps) {
        if (l.lot != g_schedLot || !(l.colour[0] > 0 || l.colour[1] > 0 || l.colour[2] > 0) || g_sched.HasLayer(l.light)) continue;
        const GiSched::Box b = GiSched::Around(l.pos, l.story, Reach(l.light));
        for (int st = b.s0; st <= b.s1; st++)
            for (int z = b.z0; z <= b.z1; z++)
                for (int x = b.x0; x <= b.x1; x++) waiting.insert(GiSched::TileKey(st, x, z));
    }
    uint64_t h = 1469598103934665603ull;
    for (const uint64_t t : waiting) h += t * 1099511628211ull; // (order-free)
    if (!g_gridDirty && h == g_readyHash && g_gridFilled) return;
    g_readyHash = h;
    if (!g_gridTex) {
        if (FAILED(g_dev->CreateTexture(g_grid.w, g_grid.h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &g_gridTex, nullptr))) g_gridTex = nullptr;
        Add(Counter::DeviceCalls);
    }
    if (!g_scatterPs && ShaderCache::CreatePixelShader(g_dev, kScatterPs, &g_scatterPs) != ShaderCache::Result::Ok) g_scatterPs = nullptr;
    if (!g_gridTex || !g_scatterPs) return;
    std::vector<GiCompose::ScatterPoint> pts;
    pts.reserve(g_grid.points.size());
    uint32_t ready = 0;
    for (const GiIndirect::Point& p : g_grid.points) {
        const float a = waiting.count(p.tile) ? 0.0f : 1.0f;
        ready += a > 0 ? 1 : 0;
        pts.push_back({p.grid, p.atlas, a});
    }
    int n = 0;
    GiCompose::Calls calls;
    calls.setTexture = &D3D9Hooks::CallOriginalSetTexture, calls.drawPrimitive = &D3D9Hooks::CallOriginalDrawPrimitive;
    g_gridFilled = g_composer.Scatter(g_dev, g_scatterPs, g_irradiance, GiScene::kAtlasWidth, g_built.height, g_gridTex, g_grid.w, pts, calls, &n);
    Add(Counter::Draws, static_cast<uint32_t>(n));
    Add(Counter::DeviceCalls); // (the ColorFill)
    g_readyTexels = g_gridFilled ? ready : 0;
    if (g_gridFilled) {
        g_pointReady.resize(pts.size());
        for (size_t i = 0; i < pts.size(); i++) g_pointReady[i] = pts[i].alpha;
        ++g_gridSerial;
    }
    g_gridDirty = false;
}

// The story and kind (GiIndirect::Kind) of a game map: the floor (set index 0) or ceiling (1) map of any class of a story of
// the GI lot, read from each story's room 0 (its manager +0x88 the story, its texture set +4: class x 64 + 0xC, a wrapper
// whose +0x30 is the texture; RtOutdoorLamps OwnMap). SEH: plain data only.
int MapLayerRead(uintptr_t room0, const void* map, int& story) {
    __try {
        const uintptr_t mgr = *reinterpret_cast<const uintptr_t*>(room0), lod = *reinterpret_cast<const uintptr_t*>(room0 + 4);
        if (!mgr || !lod) return -1;
        story = *reinterpret_cast<const int*>(mgr + 0x88);
        for (uintptr_t cls = 0; cls < 3; cls++)
            for (uintptr_t idx = 0; idx < 4; idx++) {
                const int kind = GiIndirect::KindOfSetIndex(static_cast<int>(idx));
                if (kind < 0) continue;
                const uintptr_t w = *reinterpret_cast<const uintptr_t*>(lod + cls * 64 + 0xC + idx * 4);
                if (w >= 0x10000 && *reinterpret_cast<const uint32_t*>(w + 0x2C) == 0 && *reinterpret_cast<const uintptr_t*>(w + 0x30) == reinterpret_cast<uintptr_t>(map))
                    return kind;
            }
        return -1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -2;
    }
}
bool MapLayer(const void* map, int& story, int& kind) {
    if (const auto it = g_mapLayer.find(map); it != g_mapLayer.end()) return story = it->second.first, kind = it->second.second, kind >= 0;
    // shortcut: a miss is looked up again once 30 frames passed (a map the game made since); the cache goes with every new scene
    if (g_mapLayer.size() > 256 || g_schedFrame - g_mapLayerFrame >= 30) g_mapLayer.clear(), g_mapLayerFrame = g_schedFrame;
    kind = -1, story = 0;
    const uintptr_t tracker = LevelLightShare::TrackerOfLot(g_schedLot);
    std::unordered_set<int> stories;
    for (const auto& [key, row] : g_grid.rows) stories.insert(key.first);
    for (const int s : stories) {
        const uintptr_t room0 = tracker ? reinterpret_cast<uintptr_t>(LevelLightShare::Room0Of(tracker, s)) : 0;
        int st = 0;
        const int k = room0 ? MapLayerRead(room0, map, st) : -1;
        if (k == -2) Add(Counter::ReadFaults);
        if (k >= 0 && st == s) {
            story = s, kind = k;
            break;
        }
    }
    g_mapLayer[map] = {story, kind};
    return kind >= 0;
}

// ---- (S2-06 item 8) the rig write ----
void ReleaseProbe() {
    if (g_probeTicket && g_cancelRead) g_cancelRead(g_probeTicket);
    g_probeTicket = 0, g_probeWait = 0, g_probeRigs.clear();
    for (auto& [rig, e] : g_rigs) e.probing = false;
    if (g_probeTex) g_probeTex->Release(), g_probeTex = nullptr;
    if (g_probeMem) g_probeMem->Release(), g_probeMem = nullptr;
}
// The rig's centre (+0x140, world) and its vertex-light slots (+0x90 directions, +0xD0 colours). SEH: plain data only
bool ReadRig(uintptr_t rig, float centre[3], float slots[8][4]) {
    __try {
        std::memcpy(centre, reinterpret_cast<const void*>(rig + 0x140), 12);
        std::memcpy(slots, reinterpret_cast<const void*>(rig + 0x90), 128);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool WriteRig(uintptr_t rig, const float slots[8][4]) {
    __try {
        std::memcpy(reinterpret_cast<void*>(rig + 0x90), slots, 128);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// RigTracker's bind hook (the drawing thread = the render thread): the rig's grid point found again when it moved or the grid
// is new, then its vertex-light slots set to the grid's light (or back to 0) unless the game's overflow lamps hold them. The
// binder binds pointers to rig memory, so the draw that follows reads the write (S0-09). Every rig of the GI lot, the
// household's Sims too: this is lighting, not behaviour.
void OnRigBind(uintptr_t rig) {
    if (!g_running.load() || !g_options.indirect || g_rigOff || !g_rigLot) return;
    float c[3], cur[8][4];
    if (!ReadRig(rig, c, cur)) return Add(Counter::ReadFaults);
    RigEntry& e = g_rigs[rig];
    e.seen = g_rigFrame;
    const float dx = c[0] - e.centre[0], dy = c[1] - e.centre[1], dz = c[2] - e.centre[2];
    if (e.gen != g_gridGen || dx * dx + dy * dy + dz * dz > 0.0625f) { // (a quarter metre: a new point to read)
        std::memcpy(e.centre, c, sizeof c);
        e.gen = g_gridGen, e.point = -1, e.serial = 0, e.probing = false;
        float p[3];
        uint32_t px = 0;
        RtLampProxy::ToLot(*g_rigLot, c, p);
        if (GiIndirect::RigPixel(g_grid, *g_rigLot, p, px))
            if (const auto it = g_pointOfGrid.find(px); it != g_pointOfGrid.end()) e.point = static_cast<int32_t>(it->second);
    }
    const int state = GiIndirect::RigSlotState(cur, e.wrote ? e.written : nullptr);
    if (state == GiIndirect::kRigGame) return Add(Counter::RigSkipped);
    float want[8][4] = {};
    const bool lit = e.serial && e.point >= 0 && (e.value[0] > 0 || e.value[1] > 0 || e.value[2] > 0);
    if (lit) GiIndirect::RigSlots(e.value, kRigCeiling, want);
    if (std::memcmp(cur, want, sizeof want) != 0 && !WriteRig(rig, want)) return Add(Counter::ReadFaults);
    std::memcpy(e.written, want, sizeof want), e.wrote = true;
    if (lit) Add(Counter::RigDraws);
}
// The probe answer: each rig's pixel (rgb premultiplied by its ready weight) as its value
void TakeProbe() {
    D3DLOCKED_RECT r{};
    if (FAILED(g_probeMem->LockRect(&r, nullptr, D3DLOCK_READONLY))) return; // (the bridge copied the rows in already: no wait)
    Add(Counter::RigReads);
    for (size_t i = 0; i < g_probeRigs.size(); i++) {
        const auto it = g_rigs.find(g_probeRigs[i]);
        if (it == g_rigs.end() || !it->second.probing) continue;
        const uint16_t* h = reinterpret_cast<const uint16_t*>(static_cast<const char*>(r.pBits) + (i / kProbeW) * r.Pitch) + (i % kProbeW) * 4;
        for (int k = 0; k < 3; k++) it->second.value[k] = std::max(0.0f, GiIndirect::HalfToFloat(h[k]));
        it->second.serial = g_probeSerial, it->second.probing = false;
    }
    g_probeMem->UnlockRect();
}
// Render thread, every frame: take an answered read, else read the rigs whose point or the grid changed (one read in flight)
void UpdateRigs() {
    ++g_rigFrame;
    if (!g_options.indirect || g_rigOff) return;
    if (g_rigFrame % 300 == 0)
        for (auto it = g_rigs.begin(); it != g_rigs.end();) it = g_rigFrame - it->second.seen > 600 ? g_rigs.erase(it) : std::next(it);
    if (g_probeTicket) {
        const HRESULT hr = g_pollRead(g_probeTicket);
        if (hr == S_FALSE && ++g_probeWait < 60) return;
        if (hr == S_OK) TakeProbe();
        else if (hr == S_FALSE) g_cancelRead(g_probeTicket);
        for (const uintptr_t rig : g_probeRigs)
            if (const auto it = g_rigs.find(rig); it != g_rigs.end()) it->second.probing = false;
        g_probeTicket = 0, g_probeWait = 0, g_probeRigs.clear();
        return;
    }
    if (!g_dev || !g_irradiance || !g_scatterPs || !g_gridFilled || g_pointReady.size() != g_grid.points.size()) return;
    std::vector<GiCompose::ScatterPoint> pts;
    for (auto& [rig, e] : g_rigs) {
        if (e.probing || e.serial == g_gridSerial || e.gen != g_gridGen) continue;
        if (e.point < 0) { // (nothing to read: the slots go back to 0)
            e.serial = g_gridSerial, e.value[0] = e.value[1] = e.value[2] = 0.0f;
            continue;
        }
        if (pts.size() >= kProbeW * kProbeH) break;
        pts.push_back({static_cast<uint32_t>(pts.size()), g_grid.points[static_cast<size_t>(e.point)].atlas, g_pointReady[static_cast<size_t>(e.point)]});
        g_probeRigs.push_back(rig);
    }
    if (pts.empty()) return;
    if (!g_readAsync) {
        const HMODULE m = GetModuleHandleW(L"d3d9.dll");
        g_readAsync = m ? reinterpret_cast<ReadAsyncFn>(GetProcAddress(m, "RemixBridge_ReadRenderTargetAsync")) : nullptr;
        g_pollRead = m ? reinterpret_cast<ReadTicketFn>(GetProcAddress(m, "RemixBridge_PollReadback")) : nullptr;
        g_cancelRead = m ? reinterpret_cast<ReadTicketFn>(GetProcAddress(m, "RemixBridge_CancelReadback")) : nullptr;
        // shortcut: without the bridge's asynchronous read-back (native D3D9, plain DXVK) objects and Sims keep the game's
        // light; an event-query ring (design.md, wait at the lock) would cover them
        if (!g_readAsync || !g_pollRead || !g_cancelRead) {
            g_readAsync = nullptr, g_rigOff = true, g_probeRigs.clear();
            LOG_INFO("[RealtimeGI] Objects and Sims: no asynchronous read-back in this d3d9.dll (RTX Remix bridge patch 0017), they keep the game's light");
            return;
        }
    }
    if (!g_probeTex) {
        Add(Counter::DeviceCalls, 2);
        if (FAILED(g_dev->CreateTexture(kProbeW, kProbeH, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &g_probeTex, nullptr))) g_probeTex = nullptr;
        if (FAILED(g_dev->CreateOffscreenPlainSurface(kProbeW, kProbeH, D3DFMT_A16B16G16R16F, D3DPOOL_SYSTEMMEM, &g_probeMem, nullptr))) g_probeMem = nullptr;
        if (!g_probeTex || !g_probeMem) return g_probeRigs.clear(), ReleaseProbe();
    }
    int n = 0;
    GiCompose::Calls calls;
    calls.setTexture = &D3D9Hooks::CallOriginalSetTexture, calls.drawPrimitive = &D3D9Hooks::CallOriginalDrawPrimitive;
    const bool drawn = g_composer.Scatter(g_dev, g_scatterPs, g_irradiance, GiScene::kAtlasWidth, g_built.height, g_probeTex, kProbeW, pts, calls, &n);
    Add(Counter::Draws, static_cast<uint32_t>(n)), Add(Counter::DeviceCalls);
    IDirect3DSurface9* s = nullptr;
    const HRESULT hr = drawn && SUCCEEDED(g_probeTex->GetSurfaceLevel(0, &s)) ? g_readAsync(s, g_probeMem, &g_probeTicket) : E_FAIL;
    if (s) s->Release();
    if (hr == S_OK) {
        g_probeSerial = g_gridSerial;
        for (const uintptr_t rig : g_probeRigs) g_rigs[rig].probing = true;
        return;
    }
    g_probeTicket = 0, g_probeRigs.clear();
    if (hr == D3DERR_WASSTILLDRAWING || !drawn) return; // (the bridge's slots are busy: again next frame)
    g_rigOff = true;
    LOG_INFO(std::format("[RealtimeGI] Objects and Sims: the asynchronous read-back is not available (0x{:08X}; client.asyncReadback in bridge.conf), they "
                         "keep the game's light",
                         static_cast<uint32_t>(hr)));
}

void OnPreReset(IDirect3DDevice9*) { // (DEFAULT pool: the layers, the atlas, the grid and the point buffers; every layer again after)
    if (!g_running.load()) return;
    if (g_schedLot) g_bridge.ResetLot(), g_schedLot = 0; // (UpdateSched loads the lot again: the scene sent, every lamp solved)
    ReleaseLayers();
    g_composer.Release();
    if (g_flagsTex) g_flagsTex->Release(), g_flagsTex = nullptr;
    g_built = {}, g_grid = {}, g_mapLayer.clear();
    ReleaseProbe();
    g_pointOfGrid.clear(), g_pointReady.clear(), ++g_gridGen, g_rigLot = nullptr;
}

// Render thread, after the snapshot: the active lot's lamps and changes, its build edits and the camera to the scheduler
void UpdateSched() {
    if (!g_schedOn) return;
    ++g_schedFrame;
    const bool bridge = !g_options.debugFakeSolve || kPublicBuild;
    if (g_activeLot != g_schedLot) { // a new active lot: every layer again
        g_schedLot = g_activeLot, g_schedSerial = g_serial;
        g_bridge.ResetLot(), ReleaseLayers();
        g_built = {}, g_seenGen = 0, g_seenFloors = 0;
        std::vector<GiSched::LampIn> in;
        for (const Lamp& l : g_lamps)
            if (g_schedLot && l.lot == g_schedLot) in.push_back(SchedLamp(l));
        g_sched.Load(in, g_schedFrame);
    } else if (g_serial != g_schedSerial) {
        g_schedSerial = g_serial;
        std::unordered_map<uintptr_t, const Lamp*> now;
        for (const Lamp& l : g_lamps) now[l.light] = &l;
        for (const Change& c : g_changes) {
            if (!g_schedLot || c.lot != g_schedLot) continue;
            const auto it = now.find(c.light);
            const GiSched::LampIn in = it != now.end() ? SchedLamp(*it->second) : GiSched::LampIn{c.light};
            g_sched.OnLamp(static_cast<GiSched::Event>(c.kind), in, g_schedFrame);
        }
    }
    // build edits: a new generation of the kept occluders (walls, floors, openings) every 10 frames, a repaint of the
    // floors every 30. Signalled as the whole lot; the bridge back end narrows it to what its rebuilt scene changed
    if (g_schedLot && g_schedFrame % 10 == 0) {
        uint32_t gen = 0;
        const std::shared_ptr<const RtLampProxy::Lot> lot = RtOutdoorLamps::KeptLot(gen);
        bool edited = false;
        if (lot) edited = g_seenGen && gen != g_seenGen, g_seenGen = gen; // (once a change)
        if (lot && bridge && g_built.height && g_schedFrame % 30 == 0) {
            std::vector<std::pair<int, std::vector<uint32_t>>> floors;
            const uint64_t h = ReadFloors(*lot, floors);
            edited |= g_seenFloors && h != g_seenFloors;
            g_seenFloors = h;
        }
        if (edited) g_sched.OnBuildEdit({}, static_cast<double>(GetTickCount64()));
    }
    GiSched::View view;
    view.known = LotLightingMotion::CameraEye(view.eye);
    view.hasVp = PostScene::RecentCameraViewProj(view.vp);
    g_sched.Tick(g_schedFrame, static_cast<double>(GetTickCount64()), view);
    if (bridge) ComposeFrame(), FillGrid();
}

// Render thread, every frame: a snapshot when a lamp edit was noted (LampMarkFilter's edit serial), when an on state waits
// for its second read, or every kRefreshFrames frames; nothing outside a world
void UpdateLamps() {
    if (!WorldSession::InWorld()) {
        g_lamps.clear(), g_lastRead.clear(), g_changes.clear();
        g_giLots = 0, g_activeLot = 0, g_worldFrames = 0, g_sinceSnap = 0, g_pending = false;
        if (g_schedLot) g_sched.Clear(), g_schedLot = 0;
        return;
    }
    ++g_worldFrames;
    const unsigned long edit = LampMarkFilter::EditSerial();
    if (g_lamps.empty() || g_pending || edit != g_editSerial || ++g_sinceSnap >= kRefreshFrames) {
        g_editSerial = edit;
        g_sinceSnap = 0;
        Snapshot();
    }
    UpdateSched();
    // the start line, once: when the active lot is known, or after kStartWaitFrames in the world without one
    if (!g_startLine.empty() && (g_activeLot || g_worldFrames >= kStartWaitFrames)) {
        LOG_INFO(std::format("[RealtimeGI] On: lots={} {} backend={}", g_giLots, g_startLine, kBackendNames[static_cast<int>(g_backend.load())]));
        g_startLine.clear();
    }
    if (g_dumpDue && !g_pending && g_startLine.empty() && g_options.debugLampTable && !kPublicBuild) {
        g_dumpDue = false;
        DumpTable();
    }
}

// Developer mode: the counters every 10 s, with the frames since the last line
void OnPresent(IDirect3DDevice9* dev) {
    ++g_frames;
    g_dev = dev;
    // GiOpen answered (BridgeBackend::BeginFrame): the back end again with the server's level (none: d3d9, the scheduler off)
    static uint32_t s_level = kGiLevelStarting;
    if (g_bridge.level != s_level && g_bridge.level != kGiLevelStarting) {
        s_level = g_bridge.level;
        const Backend b = SelectBackend(g_options.backend, s_level != 0, ServerRayQuery());
        if (b != g_backend.load()) {
            g_backend = b;
            if (b == Backend::D3d9 && !(g_options.debugFakeSolve && !kPublicBuild)) g_sched.Clear(), g_schedOn = false, g_schedLot = 0;
            LOG_INFO(std::format("[RealtimeGI] Back end {} (the server's level {})", kBackendNames[static_cast<int>(b)], s_level));
        }
    }
    UpdateLamps();
    UpdateRigs();
    const DWORD now = GetTickCount();
    if (g_lastLog == 0) g_lastLog = now;
    if (now - g_lastLog < kLogEveryMs) return;
    g_lastLog = now;
    const uint64_t frames = g_frames;
    g_frames = 0;
    g_changeLines = 0;
    if (kPublicBuild) return;
    const uint64_t cap = Value(Counter::CapSolvesOnGiLots), sync = Value(Counter::SyncReadbacks), locks = Value(Counter::ResourceLocks),
                   diffs = Value(Counter::RedrawDiffs), faults = Value(Counter::ReadFaults), table = Value(Counter::LampTableDiffs);
    const std::string line = std::format("[RealtimeGI] Counters: frames={} backend={} draws={} deviceCalls={} lamps={} lots={} lampChanges={} "
                                         "indirect={} atlasReady={} gridTexels={} indirectDraws={} wallDraws={} rigs={} rigDraws={} rigReads={} rigSkipped={}; must be 0: "
                                         "capSolves={} syncReadbacks={} locks={} redrawDiffs={} readFaults={} lampTableDiffs={}",
                                         frames, kBackendNames[static_cast<int>(g_backend.load())], Value(Counter::Draws), Value(Counter::DeviceCalls),
                                         g_lamps.size(), g_giLots, Value(Counter::LampChanges), kIndirectNames[g_options.indirect], g_readyTexels,
                                         g_grid.points.size(), Value(Counter::IndirectDraws), Value(Counter::WallDraws), g_rigs.size(),
                                         Value(Counter::RigDraws), Value(Counter::RigReads), Value(Counter::RigSkipped), cap, sync, locks, diffs, faults, table);
    if (cap || sync || locks || diffs || faults || table) LOG_WARNING(line);
    else LOG_INFO(line);
    if (!g_schedOn) return;
    const GiSched::Counters s = g_sched.Stats();
    using GiSched::Cause;
    const std::string sched = std::format("[GiSched] lot={:X} queued={} inFlight={} idle={} fading={} issued: moved={} added={} build={} load={}; landed={} failed={} "
                                          "composeOnly={} tilesComposed={} restarts={} dropped={} sceneSubmits={} removed={} latency={}/{} frames "
                                          "maxInFlight={}; must be 0: overBudget={}",
                                          g_schedLot, s.queued, s.inFlight, s.idle, s.fading, s.issued[static_cast<int>(Cause::Moved)],
                                          s.issued[static_cast<int>(Cause::Added)], s.issued[static_cast<int>(Cause::Build)], s.issued[static_cast<int>(Cause::Load)],
                                          s.landed, s.failed, s.composeOnly, s.tilesComposed, s.restarts, s.dropped, s.sceneSubmits, s.removed, s.latencyP50,
                                          s.latencyMax, s.maxInFlight, s.overBudget);
    if (s.overBudget) LOG_WARNING(sched);
    else LOG_INFO(sched);
}

} // namespace

void Add(Counter c, uint32_t n) { g_counters[static_cast<size_t>(c)].fetch_add(n, std::memory_order_relaxed); }
uint64_t Value(Counter c) { return g_counters[static_cast<size_t>(c)].load(std::memory_order_relaxed); }
bool Running() { return g_running.load(); }
const std::vector<Lamp>& Lamps() { return g_lamps; }
uint32_t LampSerial() { return g_serial; }
const std::vector<Change>& Changes() { return g_changes; }
int GiLots() { return g_giLots; }
Backend InUse() { return g_backend.load(); }

bool IndirectFor(const void* map, const float ambient[3], float night, IndirectBind& out) {
    if (!g_running.load() || !g_options.indirect || !g_gridFilled || !g_gridTex || !map || g_grid.flags.empty()) return false;
    int story = 0, kind = 0;
    if (!MapLayer(map, story, kind)) return false;
    out.wall = kind == GiIndirect::kWall;
    if (out.wall) {
        if (!GiIndirect::WallRows(g_grid, out.vs[0], out.vs[1])) return false;
        std::memcpy(out.vs[3], g_lotRows, sizeof g_lotRows);
        const float id[4] = {1, 1, 0, 0};
        std::memcpy(out.uv, id, sizeof id);
    } else if (!GiIndirect::UvRow(g_grid, story, kind, out.uv))
        return false;
    if (!g_flagsTex) { // (S2-06) once a grid: a MANAGED upload, not a lock of a GPU resource
        if (FAILED(g_dev->CreateTexture(g_grid.w, g_grid.h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_flagsTex, nullptr))) return g_flagsTex = nullptr, false;
        D3DLOCKED_RECT r{};
        if (FAILED(g_flagsTex->LockRect(0, &r, nullptr, 0))) return g_flagsTex->Release(), g_flagsTex = nullptr, false;
        for (uint32_t y = 0; y < g_grid.h; y++) std::memcpy(static_cast<char*>(r.pBits) + y * r.Pitch, &g_grid.flags[static_cast<size_t>(y) * g_grid.w * 4], g_grid.w * 4);
        g_flagsTex->UnlockRect(0);
        Add(Counter::DeviceCalls, 3);
    }
    out.grid = g_gridTex, out.flags = g_flagsTex;
    const float size[4] = {static_cast<float>(g_grid.w), static_cast<float>(g_grid.h), 1.0f / g_grid.w, 1.0f / g_grid.h};
    std::memcpy(out.size, size, sizeof size);
    // shortcut: gain 1 (the compose's colour is the snapshot's; S6-01 calibrates it against the game's law)
    const bool debug = g_options.indirect == 2 && !kPublicBuild;
    const float k[4] = {1.0f, debug ? 0.0f : 1.0f, debug ? 0.0f : 1.0f, 0.0f};
    std::memcpy(out.k, k, sizeof k);
    std::fill(std::begin(out.lamp), std::end(out.lamp), 0.0f);
    if (debug) return true;
    // the lamp share (S2-06): the room's ambient over the story's base colour (Rooms at Night), read once a frame a story
    const auto [it, fresh] = g_baseOf.try_emplace(story);
    auto& [frame, base] = it->second;
    if (fresh || frame != g_schedFrame) {
        frame = g_schedFrame, base = {0, 0, 0, 0};
        const uintptr_t tracker = LevelLightShare::TrackerOfLot(g_schedLot);
        float c[3];
        if (const void* room0 = tracker ? LevelLightShare::Room0Of(tracker, story) : nullptr; room0 && UnlitRooms::BaseColour(room0, c)) base = {c[0], c[1], c[2], 1};
    }
    GiIndirect::LampShare(ambient, base[3] > 0 ? base.data() : nullptr, night, out.lamp);
    return true;
}

bool Start(const Options& o, std::string* error) {
    if (g_running.load()) return true;
    const Backend backend = SelectBackend(o.backend, ServerExport(), ServerRayQuery());
    g_backend = backend;
    g_frames = 0;
    g_lastLog = 0;
    g_options = o;
    g_lamps.clear(), g_lastRead.clear(), g_changes.clear();
    g_giLots = 0, g_activeLot = 0, g_worldFrames = 0, g_sinceSnap = 0, g_pending = false, g_dumpDue = true;
    g_sched.Clear(), g_schedLot = 0;
    g_fake.boxOf = g_bridge.boxOf = [](const GiSched::LampIn& l) { return GiSched::Around(l.pos, l.story, Reach(l.id)); };
    g_bridge.tilesOf = TilesOf;
    g_bridge.rebuild = RebuildScene;
    g_bridge.target = SlotFor;
    g_bridge.landed = [](uint32_t layer) {
        if (layer < g_layers.size()) g_layers[layer].fading = true;
    };
    const bool fake = !kPublicBuild && o.debugFakeSolve, server = backend == Backend::ServerRays || backend == Backend::ServerCompute;
    g_sched.SetBackend(fake ? static_cast<GiSched::Backend*>(&g_fake) : &g_bridge);
    g_schedOn = fake || server;
    using namespace D3D9Hooks;
    if (!RegisterPresent(kPatchName, [](DeviceContext& ctx, const RECT*, const RECT*, HWND, const RGNDATA*) {
            OnPresent(ctx.device);
            return HookAction::Continue;
        })) {
        if (error) *error = "Could not register the Present callback";
        return false;
    }
    RenderCallbacks::preReset.Add(OnPreReset);
    g_rigOff = false;
    // shortcut: the rig tracker is shared with the lot light bridge; if that feature uninstalls it, the rig write stops too
    if (o.indirect && RigTracker::Install()) RigTracker::SetBindHook(OnRigBind);
    g_running = true;
    // read back by the test harness (design.md section 6): logged by UpdateLamps in a world, with lots = the GI lots then
    g_startLine = std::format("cap={} scale={} indirect={}", kCapNames[o.solveCap], o.scale, kIndirectNames[o.indirect]); // (+ the back end then)
    if (backend != o.backend && o.backend != Backend::Auto)
        LOG_INFO(std::format("[RealtimeGI] Back end {} is not available here; using {}", kBackendNames[static_cast<int>(o.backend)],
                             kBackendNames[static_cast<int>(backend)]));
    return true;
}

void Stop() {
    if (!g_running.load()) return;
    D3D9Hooks::UnregisterAll(kPatchName);
    RenderCallbacks::preReset.Remove(OnPreReset);
    g_sched.Clear(), g_schedOn = false, g_schedLot = 0;
    g_bridge.ResetLot(), ReleaseLayers(), g_composer.Release();
    if (g_composePs) g_composePs->Release(), g_composePs = nullptr;
    if (g_scatterPs) g_scatterPs->Release(), g_scatterPs = nullptr;
    if (g_flagsTex) g_flagsTex->Release(), g_flagsTex = nullptr;
    RigTracker::SetBindHook(nullptr);
    ReleaseProbe();
    // shortcut: rigs keep the last write until the game gathers them again (a lamp change, a move, the night level)
    g_rigs.clear(), g_rigLot = nullptr, g_pointOfGrid.clear(), g_pointReady.clear();
    g_built = {}, g_grid = {}, g_mapLayer.clear(), g_baseOf.clear(), g_dev = nullptr;
    g_running = false;
    LOG_INFO("[RealtimeGI] Off");
}

} // namespace RealtimeGI

namespace {

class RealtimeGiPatch : public ApexPatch {
  public:
    RealtimeGiPatch() : ApexPatch(RealtimeGI::kPatchName, nullptr) {
        // TOML keys: never rename (design.md section 5)
        RegisterNamedEnumSetting(&backend_, "backend", 0, "Back end: auto, serverRays, serverCompute or d3d9",
                                 {std::begin(RealtimeGI::kBackendNames), std::end(RealtimeGI::kBackendNames)});
        RegisterNamedEnumSetting(&lots_, "lots", 0, "Lots covered", {std::begin(RealtimeGI::kLotsNames), std::end(RealtimeGI::kLotsNames)});
        RegisterNamedEnumSetting(&solveCap_, "solveCap", 0, "The game's room solves on GI lots", {std::begin(RealtimeGI::kCapNames), std::end(RealtimeGI::kCapNames)});
        RegisterNamedEnumSetting(&scale_, "bufferScale", 0, "Light buffer scale", {"1", "2", "4"});
        RegisterNamedEnumSetting(&indirect_, "indirect", 0, "Indirect light: off, atlas (the lighting reads the irradiance atlas where ready) or atlasDebug (the atlas alone, developer mode)", {std::begin(RealtimeGI::kIndirectNames), std::end(RealtimeGI::kIndirectNames)});
        RegisterBoolSetting(&debugLampTable_, "debugLampTable", false, "Developer mode: log the lamp snapshot against the game's fields after each change");
        RegisterBoolSetting(&debugFakeSolve_, "debugFakeSolve", false, "Developer mode: run the layer update scheduler on a fake back end ([GiSched] line)");
    }

    bool Install() override {
        if (isEnabled) return true;
        lastError.clear();
        std::string error;
        const RealtimeGI::Options o{lots_, solveCap_, 1 << scale_, indirect_, static_cast<RealtimeGI::Backend>(backend_), debugLampTable_,
                                    debugFakeSolve_};
        if (!RealtimeGI::Start(o, &error)) return Fail(error);
        isEnabled = true;
        return true;
    }

    bool Uninstall() override {
        if (!isEnabled) return true;
        RealtimeGI::Stop();
        isEnabled = false;
        lastError.clear();
        return true;
    }

  private:
    int backend_ = 0, lots_ = 0, solveCap_ = 0, scale_ = 0, indirect_ = 0;
    bool debugLampTable_ = false, debugFakeSolve_ = false;
};

} // namespace

APEX_REGISTER_FEATURE(RealtimeGiPatch, {.displayName = "Real-Time Lighting",
                                        .description = "Lamps light the lot through a light buffer Apex keeps up to date every frame, with light bouncing between "
                                                       "surfaces. In development: the switches are for testing only. Part of " APEX_PRODUCT_NAME ". Credits: @loinyx",
                                        .category = "Graphics",
                                        .experimental = true,
                                        .enabledByDefault = false,
                                        .supportedVersions = VERSION_STEAM,
                                        .technicalDetails = {"Off: no hook and no resource. On: one Present callback (profiler line \"RealtimeGI\"), the lamp "
                                                             "snapshot of every light read there, and the start line read back by the test harness."},
                                        .gameCodeGroup = nullptr});
