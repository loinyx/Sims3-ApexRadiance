// Real-time global illumination: module skeleton and the feature "RealtimeGI" (see realtime_gi.h).
//
// Part of Apex Radiance. Credits: @loinyx

#include "realtime_gi.h"
#include "gi_pixel_shadows.h"
#include "patch_base.h"
#include "apex_log.h"
#include "apex_version.h"
#include "build_flavor.h"
#include "d3d9_hooks.h"
#include "game_addresses.h"
#include "gi_compose.h"
#include "gi_scene.h"
#include "gi_sched.h"
#include "level_light_share.h"
#include "lot_lighting_motion.h"
#include "lamp_mark_filter.h"
#include "mesh_caster_set.h"
#include "object_legs.h"
#include "outdoor_delta.h"
#include "penumbra.h"
#include "post_scene.h"
#include "rt_outdoor_lamps.h"
#include "shader_cache.h"
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
constexpr const char* kIndirectNames[] = {"off", "bounce1", "multiBounce"};
constexpr DWORD kLogEveryMs = 10000;

std::array<std::atomic<uint64_t>, static_cast<size_t>(Counter::Count)> g_counters{};
std::atomic<bool> g_running{false};
std::atomic<Backend> g_backend{Backend::D3d9};
std::atomic<int> g_lampIndexMode{0};
constexpr const char* kLampIndexNames[] = {"off", "compare", "draw"};
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
    // edit); target: the layer slot a solve writes; boxOf: the touch box when the server has none (no 0021 patch: the
    // lamp's reach); tilesOf: a world box's tiles and stories; landed: a layer arrived in its slot
    std::function<int(GiSched::Box& edit, bool& whole)> rebuild;
    std::vector<uint8_t> scene;
    std::function<IDirect3DTexture9*(uint32_t layer)> target;
    std::function<GiSched::Box(const GiSched::LampIn&)> boxOf;
    std::function<GiSched::Box(const float lo[3], const float hi[3])> tilesOf;
    std::function<void(uint32_t layer)> landed;
    // The region a solve is scoped to (R-06, patch 0021): the lamp's last touch box grown by kScopeGrow, only while
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
        readTouch = reinterpret_cast<ReadTouchFn>(GetProcAddress(m, "RemixBridge_GiReadTouch")); // (optional: patch 0021)
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

// ---- mesh casters (MC-2, wp/MC.md; render thread, developer mode) ----
// The object hooks' records go to the bridge client's RemixBridge_GiObjects (patch 0020) once a frame; the server reads the
// game's packages and resolves each LITE to its model's caster mesh. shortcut: objects placed before the sink is on (the
// first frame in developer mode) are not sent until they move; send the world's objects at the start if that matters.
using PFN_GiOpen = HRESULT(__stdcall*)(uint32_t*);
using PFN_GiObjects = HRESULT(__stdcall*)(const ObjectLegs::Placed*, uint32_t);
using PFN_GiReadCounters = HRESULT(__stdcall*)(uint32_t*, uint32_t);
using PFN_GiMeshFetch = HRESULT(__stdcall*)(uint64_t, uint32_t*);
using PFN_GiMeshRead = HRESULT(__stdcall*)(uint32_t, float*, uint32_t, uint32_t*, uint32_t, uint32_t*, uint32_t*);
struct MeshCasters {
    PFN_GiOpen open = nullptr;
    PFN_GiObjects send = nullptr;
    PFN_GiReadCounters counters = nullptr;
    PFN_GiMeshFetch fetch = nullptr; // (meshTiles, bridge patch 0022)
    PFN_GiMeshRead read = nullptr;
    bool started = false, opened = false, off = false;
    uint32_t ticket = 0;  // the mesh fetch in flight
    uint64_t lite = 0;
    uint32_t fetchFrames = 0;
} g_mesh;
std::vector<ObjectLegs::Placed> g_placed;

void MeshOff(const char* why) {
    g_mesh.off = true;
    MeshCasterSet::wanted.store(false);
    MeshCasterSet::g_set.Clear();
    ObjectLegs::SetSink(false);
    LOG_INFO(std::format("[GiMesh] off: {}", why));
}

// (meshTiles) the placed objects into the tiles' set, and one caster mesh read back from the server at a time
void MeshTilesFrame() {
    if (!MeshCasterSet::wanted.load(std::memory_order_relaxed)) MeshCasterSet::wanted.store(true);
    for (const ObjectLegs::Placed& p : g_placed) {
        MeshCasterSet::Object o{p.id, p.lite, {}, (p.flags & 1) != 0};
        std::memcpy(o.m, p.m, sizeof o.m);
        MeshCasterSet::g_set.Apply(o);
    }
    static std::vector<float> s_pos;
    static std::vector<uint32_t> s_tri;
    if (g_mesh.ticket) {
        constexpr uint32_t kMax = 21846; // (what the bridge's 256 KB slot holds at most)
        s_pos.resize(kMax * 3), s_tri.resize(kMax * 3);
        uint32_t nv = 0, nt = 0;
        const HRESULT hr = g_mesh.read(g_mesh.ticket, s_pos.data(), kMax, s_tri.data(), kMax, &nv, &nt);
        if (hr == S_FALSE) return void(g_mesh.fetchFrames++);
        g_mesh.ticket = 0;
        if (hr == S_OK) {
            MeshCasterSet::Model m;
            m.pos.assign(s_pos.begin(), s_pos.begin() + nv * 3), m.tri.assign(s_tri.begin(), s_tri.begin() + nt * 3);
            MeshCasterSet::g_set.Fetched(g_mesh.lite, std::move(m));
        } else if (hr == D3DERR_NOTFOUND || hr == D3DERR_MOREDATA)
            MeshCasterSet::g_set.Missing(g_mesh.lite);
        else
            return MeshOff("RemixBridge_GiMeshRead failed");
        // (the read-back buffers, 512 KB, are dropped once nothing is left to fetch)
    }
    uint64_t lite = 0;
    if (!MeshCasterSet::g_set.NextFetch(lite)) {
        std::vector<float>().swap(s_pos), std::vector<uint32_t>().swap(s_tri);
        return;
    }
    uint32_t ticket = 0;
    const HRESULT hr = g_mesh.fetch(lite, &ticket);
    if (hr == S_OK) g_mesh.ticket = ticket, g_mesh.lite = lite;
    else if (hr != D3DERR_WASSTILLDRAWING) MeshOff("RemixBridge_GiMeshFetch failed");
}

void MeshFrame() {
    if (!g_options.meshCasters || g_mesh.off) return;
    if (!g_mesh.started) {
        g_mesh.started = true;
        const HMODULE bridge = GetModuleHandleW(L"d3d9.dll");
        g_mesh.open = bridge ? reinterpret_cast<PFN_GiOpen>(GetProcAddress(bridge, "RemixBridge_GiOpen")) : nullptr;
        g_mesh.send = bridge ? reinterpret_cast<PFN_GiObjects>(GetProcAddress(bridge, "RemixBridge_GiObjects")) : nullptr;
        g_mesh.counters = bridge ? reinterpret_cast<PFN_GiReadCounters>(GetProcAddress(bridge, "RemixBridge_GiReadCounters")) : nullptr;
        if (!g_mesh.open || !g_mesh.send || !g_mesh.counters) return MeshOff("the d3d9.dll in Bin has no RemixBridge_GiObjects (bridge patch 0020)");
        if (!ObjectLegs::Hooked()) return MeshOff("the object hooks are not installed (Night Lights off, or the game code differs)");
        if (g_options.meshTiles) {
            g_mesh.fetch = reinterpret_cast<PFN_GiMeshFetch>(GetProcAddress(bridge, "RemixBridge_GiMeshFetch"));
            g_mesh.read = reinterpret_cast<PFN_GiMeshRead>(GetProcAddress(bridge, "RemixBridge_GiMeshRead"));
            if (!g_mesh.fetch || !g_mesh.read) LOG_INFO("[GiMesh] meshTiles off: the d3d9.dll in Bin has no RemixBridge_GiMeshFetch (bridge patch 0022)");
        }
        ObjectLegs::SetSink(true);
    }
    if (!g_mesh.opened) {
        uint32_t level = 0;
        const HRESULT hr = g_mesh.open(&level);
        if (hr == S_FALSE) return;
        if (FAILED(hr)) return MeshOff(hr == E_NOTIMPL ? "client.giServer is off in the bridge's bridge.conf" : "the bridge's GI open failed");
        g_mesh.opened = true;
        LOG_INFO(std::format("[GiMesh] on: objects go to the bridge server (GI level {})", level));
    }
    ObjectLegs::TakePlaced(g_placed);
    if (!g_placed.empty() && FAILED(g_mesh.send(g_placed.data(), static_cast<uint32_t>(g_placed.size())))) return MeshOff("RemixBridge_GiObjects failed");
    if (g_mesh.fetch && g_mesh.read) MeshTilesFrame();
}

// The server's object counters (words 14-21 of RemixBridge_GiReadCounters), read back by the runner job mesh-casters-smoke
void MeshLog() {
    uint32_t w[22] = {};
    if (!g_mesh.opened || g_mesh.off || FAILED(g_mesh.counters(w, 22))) return;
    const uint32_t objects = w[14], resolved = w[15], missing = w[16];
    LOG_INFO(std::format("[GiMesh] objects={} resolved={} missing={} missingShare={:.1f}% indexReady={} links={} indexMs={} dropped={}", objects, resolved,
                         missing, resolved + missing ? 100.0 * missing / (resolved + missing) : 0.0, w[17], w[18], w[19], w[20]));
    if (!g_mesh.fetch) return;
    const MeshCasterSet::Stats s = MeshCasterSet::g_set.GetStats();
    LOG_INFO(std::format("[GiMeshTiles] objects={} models={} bytes={} fetched={} missing={} tooBig={} evicted={} overBudget={} lampSkips={} serverFetches={} waitFrames={} tileTris={}",
                         s.objects, s.models, s.bytes, s.fetched, s.missing, s.tooBig, s.evicted, s.overBudget, s.lampSkips, w[21], g_mesh.fetchFrames,
                         RtOutdoorLamps::MeshTileTris()));
}

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
                if (c.kind == LampChange::Value && g_changeLines >= kChangeLinesPerLog / 2) continue; // value noise (load, ~700 a window) must not hide a switch
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
    if (bridge) ComposeFrame();
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
    if (g_options.meshTiles && MeshCasterSet::wanted.load(std::memory_order_relaxed)) {
        std::vector<std::array<float, 3>> heads;
        for (const Lamp& l : g_lamps) heads.push_back({l.pos[0], l.pos[1], l.pos[2]});
        MeshCasterSet::g_set.SetLamps(heads);
    }
    MeshFrame();
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
    const std::string line = std::format("[RealtimeGI] Counters: frames={} backend={} draws={} deviceCalls={} lamps={} lots={} lampChanges={}; must be 0: "
                                         "capSolves={} syncReadbacks={} locks={} redrawDiffs={} readFaults={} lampTableDiffs={}",
                                         frames, kBackendNames[static_cast<int>(g_backend.load())], Value(Counter::Draws), Value(Counter::DeviceCalls),
                                         g_lamps.size(), g_giLots, Value(Counter::LampChanges), cap, sync, locks, diffs, faults, table);
    if (cap || sync || locks || diffs || faults || table) LOG_WARNING(line);
    else LOG_INFO(line);
    MeshLog();
    // read back by the lamp-index-smoke job: unexplained must stay 0
    if (const int mode = g_lampIndexMode.load()) {
        const uint64_t unexplained = Value(Counter::LampIndexUnexplained);
        const std::string li = std::format("[RealtimeGI] Lamp index counters: mode={} builds={} cells={} draws={} fallbacks={} compared={} mismatches: offCentre={} "
                                           "height={} cellFull={} group={} unexplained={} fences: draws={} compared={} windows: draws={} compared={}",
                                           kLampIndexNames[mode], Value(Counter::LampIndexBuilds), Value(Counter::LampIndexCells), Value(Counter::LampIndexDraws),
                                           Value(Counter::LampIndexFallbacks), Value(Counter::LampIndexCompared), Value(Counter::LampIndexOffCentre),
                                           Value(Counter::LampIndexHeight), Value(Counter::LampIndexCellFull), Value(Counter::LampIndexGroup), unexplained,
                                           Value(Counter::LampIndexFenceDraws), Value(Counter::LampIndexFenceCompared), Value(Counter::LampIndexWindowDraws),
                                           Value(Counter::LampIndexWindowCompared));
        if (unexplained) LOG_WARNING(li);
        else LOG_INFO(li);
    }
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
int LampIndexMode() { return g_lampIndexMode.load(std::memory_order_relaxed); }
Backend InUse() { return g_backend.load(); }

bool Start(const Options& o, std::string* error) {
    if (g_running.load()) return true;
    const Backend backend = SelectBackend(o.backend, ServerExport(), ServerRayQuery());
    g_backend = backend;
    g_frames = 0;
    g_lastLog = 0;
    g_options = o;
    g_lamps.clear(), g_lastRead.clear(), g_changes.clear();
    g_giLots = 0, g_activeLot = 0, g_worldFrames = 0, g_sinceSnap = 0, g_pending = false, g_dumpDue = true;
    g_mesh = {};
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
    g_running = true;
    if (o.pixelShadows) GiPixelShadows::Start({o.pixelView, o.pixelTestCaster});
    g_lampIndexMode = kDevToolsBuild ? std::clamp(o.lampIndex, 0, 2) : 0;
    if (g_lampIndexMode.load()) LOG_INFO(std::format("[RealtimeGI] Lamp index: mode={}", kLampIndexNames[g_lampIndexMode.load()]));
    // read back by the test harness (design.md section 6): logged by UpdateLamps in a world, with lots = the GI lots then
    g_startLine = std::format("cap={} scale={} indirect={}", kCapNames[o.solveCap], o.scale, kIndirectNames[o.indirect]); // (+ the back end then)
    if (backend != o.backend && o.backend != Backend::Auto)
        LOG_INFO(std::format("[RealtimeGI] Back end {} is not available here; using {}", kBackendNames[static_cast<int>(o.backend)],
                             kBackendNames[static_cast<int>(backend)]));
    return true;
}

void Stop() {
    if (!g_running.load()) return;
    GiPixelShadows::Stop();
    D3D9Hooks::UnregisterAll(kPatchName);
    if (g_options.meshCasters) ObjectLegs::SetSink(false);
    g_sched.Clear(), g_schedOn = false, g_schedLot = 0;
    g_bridge.ResetLot(), ReleaseLayers(), g_composer.Release();
    if (g_composePs) g_composePs->Release(), g_composePs = nullptr;
    g_built = {}, g_dev = nullptr;
    g_lampIndexMode = 0;
    if (g_options.meshTiles) MeshCasterSet::wanted.store(false), MeshCasterSet::g_set.Clear();
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
        RegisterNamedEnumSetting(&indirect_, "indirect", 0, "Indirect light", {std::begin(RealtimeGI::kIndirectNames), std::end(RealtimeGI::kIndirectNames)});
        RegisterBoolSetting(&debugLampTable_, "debugLampTable", false, "Developer mode: log the lamp snapshot against the game's fields after each change");
        RegisterBoolSetting(&pixelShadows_, "pixelShadows", false, "Per-pixel delta shadows through the bridge (patch 0019 and the DXVK of native/dxvk)");
        RegisterIntSetting(&pixelView_, "pixelView", 0, 0, 2, "Developer view of the per-pixel shadows: 0 = off, 1 = filtered mask, 2 = raw mask");
        RegisterBoolSetting(&pixelTestCaster_, "pixelTestCaster", false, "Developer: one test capsule under the strongest lamp");
        RegisterBoolSetting(&meshCasters_, "meshCasters", false, "Developer mode: give the bridge server every object's own mesh as a shadow caster");
        RegisterBoolSetting(&debugFakeSolve_, "debugFakeSolve", false, "Developer mode: run the layer update scheduler on a fake back end ([GiSched] line)");
        // (S3-02b) developer builds only; ints, not names: 0 off, 1 compare, 2 draw
        if constexpr (kDevToolsBuild)
            RegisterIntSetting(&lampIndex_, "lampIndex", 0, 0, 2, "Lamp index cells: 0 off, 1 compare each per-draw pick with its cell, 2 draw from the cells");
        RegisterBoolSetting(&meshTiles_, "meshTiles", false, "Developer mode: draw every object's own mesh into the lamps' shadow tiles (needs the bridge)");
    }

    bool Install() override {
        if (isEnabled) return true;
        lastError.clear();
        std::string error;
        const RealtimeGI::Options o{lots_, solveCap_, 1 << scale_, indirect_, static_cast<RealtimeGI::Backend>(backend_), debugLampTable_,
                                    pixelShadows_, pixelView_, pixelTestCaster_, (meshCasters_ || meshTiles_) && !kPublicBuild, debugFakeSolve_, lampIndex_,
                                    meshTiles_ && !kPublicBuild};
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
    int backend_ = 0, lots_ = 0, solveCap_ = 0, scale_ = 0, indirect_ = 0, lampIndex_ = 0;
    bool debugLampTable_ = false, pixelShadows_ = false, pixelTestCaster_ = false, meshCasters_ = false, debugFakeSolve_ = false, meshTiles_ = false;
    int pixelView_ = 0;
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
