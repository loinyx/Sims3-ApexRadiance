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
#include "lamp_mark_filter.h"
#include "object_legs.h"
#include "outdoor_delta.h"
#include "rt_outdoor_lamps.h"
#include "world_session.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <format>
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
uint64_t g_frames = 0; // render thread
DWORD g_lastLog = 0;
Options g_options;       // set by Start
std::string g_startLine; // logged at the first snapshot in a world (it carries lots=)

// shortcut: no bridge exports the GI interface yet, so the server back ends are never available; the server packages
// (R-02 onward) replace this with the export lookup in the bridge client d3d9.dll.
bool ServerExport() { return false; }
bool ServerRayQuery() { return false; }

// ---- mesh casters (MC-2, wp/MC.md; render thread, developer mode) ----
// The object hooks' records go to the bridge client's RemixBridge_GiObjects (patch 0020) once a frame; the server reads the
// game's packages and resolves each LITE to its model's caster mesh. shortcut: objects placed before the sink is on (the
// first frame in developer mode) are not sent until they move; send the world's objects at the start if that matters.
using PFN_GiOpen = HRESULT(__stdcall*)(uint32_t*);
using PFN_GiObjects = HRESULT(__stdcall*)(const ObjectLegs::Placed*, uint32_t);
using PFN_GiReadCounters = HRESULT(__stdcall*)(uint32_t*, uint32_t);
struct MeshCasters {
    PFN_GiOpen open = nullptr;
    PFN_GiObjects send = nullptr;
    PFN_GiReadCounters counters = nullptr;
    bool started = false, opened = false, off = false;
} g_mesh;
std::vector<ObjectLegs::Placed> g_placed;

void MeshOff(const char* why) {
    g_mesh.off = true;
    ObjectLegs::SetSink(false);
    LOG_INFO(std::format("[GiMesh] off: {}", why));
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
    if (!g_placed.empty() && FAILED(g_mesh.send(g_placed.data(), static_cast<uint32_t>(g_placed.size())))) MeshOff("RemixBridge_GiObjects failed");
}

// The server's object counters (words 14-21 of RemixBridge_GiReadCounters), read back by the runner job mesh-casters-smoke
void MeshLog() {
    uint32_t w[22] = {};
    if (!g_mesh.opened || g_mesh.off || FAILED(g_mesh.counters(w, 22))) return;
    const uint32_t objects = w[14], resolved = w[15], missing = w[16];
    LOG_INFO(std::format("[GiMesh] objects={} resolved={} missing={} missingShare={:.1f}% indexReady={} links={} indexMs={} dropped={}", objects, resolved,
                         missing, resolved + missing ? 100.0 * missing / (resolved + missing) : 0.0, w[17], w[18], w[19], w[20]));
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
                if (g_changeLines++ >= kChangeLinesPerLog) break;
                LOG_INFO(std::format("[RealtimeGI] Lamp change: {} light={:08X} lot={:X}", kLampChangeNames[static_cast<int>(c.kind)], static_cast<uint32_t>(c.light), c.lot));
            }
    }
    const int lotsBefore = g_giLots;
    CountLots();
    if (g_startLine.empty() && g_giLots != lotsBefore && !kPublicBuild) LOG_INFO(std::format("[RealtimeGI] Lots: {}", g_giLots));
}

// Render thread, every frame: a snapshot when a lamp edit was noted (LampMarkFilter's edit serial), when an on state waits
// for its second read, or every kRefreshFrames frames; nothing outside a world
void UpdateLamps() {
    if (!WorldSession::InWorld()) {
        g_lamps.clear(), g_lastRead.clear(), g_changes.clear();
        g_giLots = 0, g_activeLot = 0, g_worldFrames = 0, g_sinceSnap = 0, g_pending = false;
        return;
    }
    ++g_worldFrames;
    const unsigned long edit = LampMarkFilter::EditSerial();
    if (g_lamps.empty() || g_pending || edit != g_editSerial || ++g_sinceSnap >= kRefreshFrames) {
        g_editSerial = edit;
        g_sinceSnap = 0;
        Snapshot();
    }
    // the start line, once: when the active lot is known, or after kStartWaitFrames in the world without one
    if (!g_startLine.empty() && (g_activeLot || g_worldFrames >= kStartWaitFrames)) {
        LOG_INFO(std::format("[RealtimeGI] On: lots={} {}", g_giLots, g_startLine));
        g_startLine.clear();
    }
    if (g_dumpDue && !g_pending && g_startLine.empty() && g_options.debugLampTable && !kPublicBuild) {
        g_dumpDue = false;
        DumpTable();
    }
}

// Developer mode: the counters every 10 s, with the frames since the last line
void OnPresent() {
    ++g_frames;
    UpdateLamps();
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
    using namespace D3D9Hooks;
    if (!RegisterPresent(kPatchName, [](DeviceContext&, const RECT*, const RECT*, HWND, const RGNDATA*) {
            OnPresent();
            return HookAction::Continue;
        })) {
        if (error) *error = "Could not register the Present callback";
        return false;
    }
    g_running = true;
    // read back by the test harness (design.md section 6): logged by UpdateLamps in a world, with lots = the GI lots then
    g_startLine = std::format("cap={} scale={} indirect={} backend={}", kCapNames[o.solveCap], o.scale, kIndirectNames[o.indirect],
                              kBackendNames[static_cast<int>(backend)]);
    if (backend != o.backend && o.backend != Backend::Auto)
        LOG_INFO(std::format("[RealtimeGI] Back end {} is not available here; using {}", kBackendNames[static_cast<int>(o.backend)],
                             kBackendNames[static_cast<int>(backend)]));
    return true;
}

void Stop() {
    if (!g_running.load()) return;
    D3D9Hooks::UnregisterAll(kPatchName);
    if (g_options.meshCasters) ObjectLegs::SetSink(false);
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
        RegisterBoolSetting(&meshCasters_, "meshCasters", false, "Developer mode: give the bridge server every object's own mesh as a shadow caster");
    }

    bool Install() override {
        if (isEnabled) return true;
        lastError.clear();
        std::string error;
        const RealtimeGI::Options o{lots_, solveCap_, 1 << scale_, indirect_, static_cast<RealtimeGI::Backend>(backend_), debugLampTable_, meshCasters_ && !kPublicBuild};
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
    bool debugLampTable_ = false, meshCasters_ = false;
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
