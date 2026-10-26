// Real-time outdoor lamps (Lamp lighting: Instant; see rt_outdoor_lamps.h).
//
// ---- The game side (Steam 1.67.2, read with dumpbin 13/10) ----
//   The unlock of a solve's maps 0x0069E950 thiscall(room, char commit), with commit: "mov ecx,[ebx+0F4h]; mov edx,[ebx+4];
//   shl ecx,6; push ebp; lea ecx,[ecx+edx+0Ch]; call 0x006A8FD0": the room's own texture set of its class, [room+4] + class x
//   64 + 0xC, takes the class's working maps (ebp = 0x011D0460 + class x 16, from 0x006A4640). 0x006A8FD0 thiscall(dst set,
//   src set): for i = 0..3, dst[i]->copy(src[i], 0) (0x00619A00). Index 0 floor (lock record +0x1C4), 1 ceiling (+0x2B4), 2
//   objects (+0x3A4), 3 walls (+0x680): the 0x006A8FC0 calls at 0x0069E97E .. 0x0069E9DE. A wrapper's texture is
//   [wrapper+0x30] while [wrapper+0x2C] == 0 (the lock 0x00618DF0: "cmp [edi+2Ch],eax; jne; cmp [edi+30h],eax; je; mov eax,
//   [edi+30h]; mov edx,[eax]; call [edx+4Ch]", IDirect3DTexture9::LockRect).
//   0x006A7700 binds a lot's "LightMap" from the story manager's set (+0x16C + class x 64, index 0 or 2); that is the room's
//   set when [room+4] = manager + 0x160: UNVERIFIED, logged once per story ("own set at manager +0x...").
//   Every room of a story writes the same maps (each its own texels): the copy starts from the game's map every time.
//
// Part of Apex Radiance. Credits: @loinyx

#include "rt_outdoor_lamps.h"
#include "terrain_bake_rule.h"
#include "rt_lamp_gpu.h"
#include "platform.h"
#include "rt_lamp_model.h"
#include "rt_lamp_proxy.h"
#include "rt_lamps_hlsl.h"
#include "shader_cache.h"
#include "outdoor_delta.h"
#include "outdoor_delta_model.h"
#include "level_light_share.h"
#include "lamp_reach.h"
#include "penumbra.h"
#include "room_map_dither.h"
#include "lamp_mark_filter.h"
#include "apex_log.h"
#include "hook_guard.h"
#include "dev_switches.h"
#include "apex_memory.h"
#include "game_addresses.h"
#include "d3d9_hooks.h"
#include "d3d9_extra_hooks.h"
#include "render_callbacks.h"
#include <windows.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <format>
#include <map>
#include <set>
#include <tuple>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace RtOutdoorLamps {
namespace {

constexpr int kSetIdx[4] = {3, 0, 1, 2}; // the record's maps (wall, floor, ceiling, objects) in a texture set
constexpr const char* kMapName[4] = {"wall atlas", "floor map", "ceiling map", "object map"};
constexpr int kMaxWatch = 64;            // maps watched for writes (8 stories x 4 maps x 2 classes)
constexpr float kFar = 64.0f;            // m: the shadow maps' range
constexpr DWORD kSolveAskMs = 10000;     // a room 0 with no kept solve is sent to its solve at most this often
constexpr DWORD kCoverFreshMs = 3000;    // a lot's coverage counts this long after the last check
constexpr int kSolveAsks = 3;            // a room 0 is sent to its solve for a kept solve at most this often in a row
constexpr DWORD kDragQuietMs = 300;      // a GPU lamp that changed this recently is being dragged (or edited again and again)
constexpr DWORD kHoldQuietMs = 1500;     // the rooms that take room 0's lamps are sent this long after a story's last move
                                         // (a Build mode drag moves in grid steps with pauses of up to about a second)
constexpr DWORD kCheckEveryMs = 2000;    // the consistency check of the GPU lamps' stories
constexpr DWORD kRepairWaitMs = 4000;    // a story sent to gather a lamp is checked again after this
constexpr DWORD kDragRelightMs = 200;    // while dragged: at most one relight start this often (14/10, drag solves: was 66; 5 a second,
                                         // the exact relight once the drag ends; a recording: 1-13 ms of CPU a relight)
constexpr uint64_t kGameWriteGapMs = 500; // a relight made only for a game write: at most this often per map
// (2.15.1) A lamp lying on the lot's ground (its light 5 cm over the lawn, F8 capture) lit a tree above it and left the
// lawn dark: the law's light falls with its angle of incidence, and from 5 cm the lawn 2 m away takes about 1 % of what it
// would from above. On story 0's floor map a lamp whose light stands lower than kLowLampLift over the lot's ground is
// drawn from that height, as a fixture's glow would spread; walls, ceilings, objects and trees keep its real place. The
// terrain bake stamps it from the same height (TerrainBakeRule::StampHeight), so the lawn beyond the lot and the atlas agree.
std::atomic<long> g_lowLampLifted{0};
constexpr double kFrameCpuMs = 2.0;      // a frame's relight work stops starting maps past this (the rest next frame)
constexpr double kFrameWork = 1.0e6;     // ... or past this many units (texel x lamp: 7 exact, 1 dragged; about 2 ns of GPU each, tools/rt_lamps_test)

// The GPU relight follows the bake this build makes: the lamp's ball of Shadow softness (Penumbra::Radius), Smooth light
// maps' soft light cut and map dither when they are on (no lamp shade shapes: the game's cones)

std::atomic<bool> g_option{false}, g_on{false}, g_watch{false};
bool g_restartRequested = false;
std::atomic<uint32_t> g_proxyEpoch{1};
std::atomic<long> c_relights{0}, c_mapsRelit{0}, c_tiles{0}, c_edits{0}, c_editsRefused{0}, c_sends{0}, c_binds{0}, c_proxyBuilds{0};
std::atomic<float> g_lastMs{0.0f}, g_lastGpuMs{-1.0f}, g_lastEditMs{-1.0f};
std::string g_inactive;             // render thread: why the option does nothing now ("" = active)
std::string g_inactiveLogged;       // the last reason logged

// ---- the shaders (made on the render thread from the bytecode Apex's shader cache precompiled) ----
// (2.15.3) They were compiled with D3DCompile on a worker of their own at every start (3.6 s, and the compiler's heap peak
// the shader cache avoids): they are now registered with ShaderCache (same source, entries, targets and flags, so the same
// bytecode), taken from its disk cache or the copy built into the ASI like every other Apex shader.
struct Compiled {
    std::atomic<int> state{0}; // 0 not started, 1 taking the bytecode, 2 done, 3 failed
    std::vector<DWORD> code[RtLampGpu::kShaderCount];
    std::string error;
};
Compiled g_compiled;
ShaderCache::Id AddRtLampShader(int i) {
    const auto id = static_cast<RtLampGpu::ShaderId>(i);
    ShaderCache::Desc d;
    d.tag = RtLampGpu::EntryOf(id);
    d.source = kRtLampsHlsl;
    d.sourceName = "rt_lamps";
    d.entry = RtLampGpu::EntryOf(id);
    d.target = RtLampGpu::IsVertex(id) ? "vs_3_0" : "ps_3_0";
    d.flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
    return ShaderCache::Add(std::move(d));
}
const ShaderCache::Id kRtLampShaderIds[RtLampGpu::kShaderCount] = {
    AddRtLampShader(0), AddRtLampShader(1), AddRtLampShader(2), AddRtLampShader(3), AddRtLampShader(4), AddRtLampShader(5), AddRtLampShader(6),
    AddRtLampShader(7), AddRtLampShader(8), AddRtLampShader(9), AddRtLampShader(10), AddRtLampShader(11), AddRtLampShader(12)};
static_assert(RtLampGpu::kShaderCount == 13, "kRtLampShaderIds lists every RtLampGpu::ShaderId");
void CompileAll() {
    for (int i = 0; i < RtLampGpu::kShaderCount; i++) {
        std::string err;
        if (!ShaderCache::Bytecode(kRtLampShaderIds[i], g_compiled.code[i], &err)) {
            g_compiled.error = std::format("{} does not compile: {}", RtLampGpu::EntryOf(static_cast<RtLampGpu::ShaderId>(i)), err);
            g_compiled.state.store(3);
            return;
        }
    }
    g_compiled.state.store(2);
}

// ---- device calls that Apex's own hooks never see ----
HRESULT OrigSetTexture(IDirect3DDevice9* d, DWORD s, IDirect3DBaseTexture9* t) { return D3D9Hooks::CallOriginalSetTexture(d, s, t); }
HRESULT OrigSetRt(IDirect3DDevice9* d, DWORD i, IDirect3DSurface9* s) { return D3D9Hooks::CallOriginalSetRenderTarget(d, i, s); }
HRESULT OrigSetDs(IDirect3DDevice9* d, IDirect3DSurface9* s) { return ExtraHooks::RawSetDepthStencilSurface(d, s); }
HRESULT OrigGetDs(IDirect3DDevice9* d, IDirect3DSurface9** s) { return ExtraHooks::RawGetDepthStencilSurface(d, s); }
HRESULT OrigSetVs(IDirect3DDevice9* d, IDirect3DVertexShader9* s) { return D3D9Hooks::CallOriginalSetVertexShader(d, s); }
HRESULT OrigSetPs(IDirect3DDevice9* d, IDirect3DPixelShader9* s) { return D3D9Hooks::CallOriginalSetPixelShader(d, s); }
HRESULT OrigVsc(IDirect3DDevice9* d, UINT r, const float* f, UINT n) { return D3D9Hooks::CallOriginalSetVertexShaderConstantF(d, r, f, n); }
HRESULT OrigPsc(IDirect3DDevice9* d, UINT r, const float* f, UINT n) { return D3D9Hooks::CallOriginalSetPixelShaderConstantF(d, r, f, n); }
HRESULT OrigDraw(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT s, UINT n) { return D3D9Hooks::CallOriginalDrawPrimitive(d, t, s, n); }
HRESULT OrigVp(IDirect3DDevice9* d, const D3DVIEWPORT9* v) { return D3D9Hooks::CallOriginalSetViewport(d, v); }
RtLampGpu::DeviceCalls GameCalls() {
    RtLampGpu::DeviceCalls c;
    c.SetTexture = OrigSetTexture, c.SetRenderTarget = OrigSetRt, c.SetDepthStencil = OrigSetDs, c.GetDepthStencil = OrigGetDs, c.SetVertexShader = OrigSetVs;
    c.SetPixelShader = OrigSetPs, c.SetVsConst = OrigVsc, c.SetPsConst = OrigPsc, c.DrawPrimitive = OrigDraw, c.SetViewport = OrigVp;
    c.StateRestored = D3D9Hooks::ShaderStateChanged;
    return c;
}
std::unique_ptr<RtLampGpu::Engine> g_eng; // render thread
void* g_texVt = nullptr;                  // the device's texture vtable (a game map must have it)

// ---- game reads (POD, SEH) ----
struct Xform {
    float r[4][4];
};
struct RoomNow {
    uintptr_t mgr = 0, lod = 0;
    int id = -1, cls = -1, shown = -1, state = -1, story = -99;
    bool outdoor = false;
    uint64_t lot = 0;
    float lotBase = 0.0f;
    Xform xf{};
};
bool ReadRoomNow(const BYTE* room, RoomNow& r) {
    __try {
        r.mgr = *reinterpret_cast<const uintptr_t*>(room);
        r.lod = *reinterpret_cast<const uintptr_t*>(room + 4);
        r.id = *reinterpret_cast<const int*>(room + 0xC);
        r.outdoor = room[0x18] != 0;
        r.state = *reinterpret_cast<const int*>(room + 0xF0);
        r.cls = *reinterpret_cast<const int*>(room + 0xF4);
        r.shown = *reinterpret_cast<const int*>(room + 0x100);
        if (!r.mgr) return false;
        r.story = *reinterpret_cast<const int*>(r.mgr + 0x88);
        r.lot = *reinterpret_cast<const uint32_t*>(r.mgr + 0x90) | static_cast<uint64_t>(*reinterpret_cast<const uint32_t*>(r.mgr + 0x94)) << 32;
        r.lotBase = *reinterpret_cast<const float*>(r.mgr + 0xD4);
        std::memcpy(&r.xf, reinterpret_cast<const void*>(r.mgr + 0xE0), sizeof r.xf);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// The room's own map of class cls (set index idx): its wrapper and texture; null when it has none or is not a texture
IDirect3DTexture9* OwnMap(const BYTE* room, int cls, int idx, uintptr_t& setAt) {
    if (cls < 0 || cls > 2 || idx < 0 || idx > 3) return nullptr;
    __try {
        const uintptr_t lod = *reinterpret_cast<const uintptr_t*>(room + 4);
        if (!lod) return nullptr;
        setAt = lod + static_cast<uintptr_t>(cls) * 64 + 0xC;
        const uintptr_t wrapper = *reinterpret_cast<const uintptr_t*>(setAt + static_cast<uintptr_t>(idx) * 4);
        if (!wrapper || *reinterpret_cast<const uint32_t*>(wrapper + 0x2C) != 0) return nullptr;
        const uintptr_t tex = *reinterpret_cast<const uintptr_t*>(wrapper + 0x30);
        if (!tex || !g_texVt || *reinterpret_cast<void* const*>(tex) != g_texVt) return nullptr;
        return reinterpret_cast<IDirect3DTexture9*>(tex);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return nullptr;
    }
}
bool ListHeads(const BYTE* room, uintptr_t& mgr, uintptr_t& begin) { // POD (SEH): the room's manager and its list's begin
    __try {
        mgr = *reinterpret_cast<const uintptr_t*>(room);
        begin = *reinterpret_cast<const uintptr_t*>(room + 0xC8);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
int ReadList(const BYTE* room, uintptr_t* out, int max) {
    __try {
        const uintptr_t* b = *reinterpret_cast<const uintptr_t* const*>(room + 0xC8);
        const uintptr_t* e = *reinterpret_cast<const uintptr_t* const*>(room + 0xCC);
        if (!b || e < b || e - b > max) return -1;
        int n = 0;
        for (const uintptr_t* p = b; p < e; p++) out[n++] = *p;
        return n;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
// A light as the GPU and the bake stamp need it (gpu_lamps.cpp ReadLamp's reads)
struct LampRead {
    RtLampModel::LightFields f;
    void* vt = nullptr;
    float pos[3] = {}, colour[3] = {}, f0[3] = {}, range = 0.0f, intensity = 0.0f;
    float axis4[3] = {}, off4 = 0.0f, scale4 = 0.0f;
    float a1[3] = {}, a2[3] = {}, o1 = 1.0f, o2 = 1.0f, S[3] = {};
    float areaShare = 1.0f; // an area light (9, 10): the share of the ground it faces (TerrainBakeRule)
};
bool ReadLamp(uintptr_t L, LampRead& r) {
    if (!L) return false;
    __try {
        const BYTE* p = reinterpret_cast<const BYTE*>(L);
        r.vt = *reinterpret_cast<void* const*>(p);
        r.f.type = *reinterpret_cast<const int*>(p + 0xB0);
        r.f.lot = *reinterpret_cast<const uint32_t*>(p + 0xC0) | static_cast<uint64_t>(*reinterpret_cast<const uint32_t*>(p + 0xC4)) << 32;
        r.f.room = *reinterpret_cast<const int*>(p + 0x08);
        r.f.flags = p[0x100];
        for (int i = 0; i < 3; i++) {
            r.pos[i] = reinterpret_cast<const float*>(p + 0x120)[i];
            r.colour[i] = reinterpret_cast<const float*>(p + 0xE0)[i];
            r.f0[i] = reinterpret_cast<const float*>(p + 0xF0)[i];
        }
        r.range = *reinterpret_cast<const float*>(p + 0x130);
        r.intensity = *reinterpret_cast<const float*>(p + 0x10);
        if (r.f.type == 4) {
            for (int i = 0; i < 3; i++) r.axis4[i] = reinterpret_cast<const float*>(p + 0x170)[i];
            r.off4 = *reinterpret_cast<const float*>(p + 0x158);
            r.scale4 = *reinterpret_cast<const float*>(p + 0x154);
        } else if (r.f.type == 5) {
            for (int i = 0; i < 3; i++) {
                r.a1[i] = reinterpret_cast<const float*>(p + 0x1A0)[i];
                r.a2[i] = reinterpret_cast<const float*>(p + 0x190)[i];
                r.S[i] = reinterpret_cast<const float*>(p + 0x150)[i];
            }
            r.o1 = *reinterpret_cast<const float*>(p + 0x174);
            r.o2 = *reinterpret_cast<const float*>(p + 0x170);
            r.f.unmodelled = !RtLampModel::ShadedModelled(r.a1, r.a2, r.o1, r.o2);
        } else if (TerrainBakeRule::AreaType(r.f.type)) {
            r.areaShare = TerrainBakeRule::AreaGroundShare(*reinterpret_cast<const float*>(p + TerrainBakeRule::kAreaAxisYOffset));
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    for (float v : r.pos)
        if (!std::isfinite(v)) return false;
    return std::isfinite(r.range) && std::isfinite(r.intensity);
}
// The terrain bake's rule for the lamps that stamp the ground (LevelLightShare InTerrainBake)
bool InTerrainBake(const LampRead& r) {
    const bool lot = r.f.lot != 0;
    if (r.f.type == 11) return !(lot && (r.f.flags & 0x04) && r.f.room == 0 && (r.f.flags & 0x61) != 0x61);
    return lot && TerrainBakeRule::LotLampType(r.f.type) && (r.f.flags & 0x65) == 0x65 && r.f.room == 0;
}
struct GameConsts {
    float k1 = 1.0f, k2 = 0.075f, cmax = 2.0f, street = 3.3333f, s5 = 5.0f;
};
GameConsts ReadConsts() {
    GameConsts g;
    // the light law constants (Steam 1.67.2: 0x011D0A60, 0x011D0A68, 0x011D1160, 0x01158DA8, 0x011D11A0)
    using GameAddr::Id;
    const uintptr_t k1At = GameAddr::Get(Id::LampK1), k2At = GameAddr::Get(Id::LampK2), cmaxAt = GameAddr::Get(Id::LampCmax), streetAt = GameAddr::Get(Id::StreetLampScale),
                    s5At = GameAddr::Get(Id::ConeScale5);
    if (!k1At || !k2At || !cmaxAt || !streetAt || !s5At) return g;
    __try {
        const float k1 = *reinterpret_cast<const float*>(k1At), k2 = *reinterpret_cast<const float*>(k2At);
        const float cmax = *reinterpret_cast<const float*>(cmaxAt), street = *reinterpret_cast<const float*>(streetAt), s5 = *reinterpret_cast<const float*>(s5At);
        if (k1 > 1e-6f && k1 < 1e6f) g.k1 = k1;
        if (k2 > 0.0f && k2 < 1e3f) g.k2 = k2;
        if (cmax > 0.0f && cmax < 1e3f) g.cmax = cmax;
        if (street > 0.0f && street < 100.0f) g.street = street;
        if (s5 > 0.0f && s5 < 100.0f) g.s5 = s5;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return g;
}
void ToLot(const Xform& x, const float w[3], float out[3]) {
    for (int k = 0; k < 3; k++) out[k] = w[0] * x.r[0][k] + w[1] * x.r[1][k] + w[2] * x.r[2][k] + x.r[3][k];
}
void DirToLot(const Xform& x, const float w[3], float out[3]) {
    for (int k = 0; k < 3; k++) out[k] = w[0] * x.r[0][k] + w[1] * x.r[1][k] + w[2] * x.r[2][k];
}
// The law of a lamp (gpu_lamps.cpp ToLaw), in lot space
void ToLaw(const LampRead& r, const GameConsts& g, const Xform& xf, bool reachOn, GpuLampLaw::Lamp& L) {
    ToLot(xf, r.pos, L.pos);
    for (int i = 0; i < 3; i++) L.colour[i] = std::isfinite(r.colour[i]) ? std::max(r.colour[i], 0.0f) : 0.0f;
    const float w = r.range * r.intensity;
    L.W = std::isfinite(w) && w > 0.0f ? g.k2 * w / g.k1 * (r.f.type == 11 ? g.street : 1.0f) : 0.0f;
    L.cmax = g.cmax;
    GpuLampLaw::NoCone(L);
    if (r.f.type == 4 && std::isfinite(r.scale4) && std::isfinite(r.off4)) {
        float a[3];
        DirToLot(xf, r.axis4, a);
        GpuLampLaw::SpotCone(L, a, r.off4, r.scale4);
    } else if (r.f.type == 5) {
        const bool opposite = std::fabs(r.a1[0] + r.a2[0]) + std::fabs(r.a1[1] + r.a2[1]) + std::fabs(r.a1[2] + r.a2[2]) < 1e-3f;
        if (opposite && std::isfinite(r.o1) && std::isfinite(r.o2)) {
            // the game's cones (t = s (a . u - o), FUN_006bc940): 2.12.3 has no lamp shade shapes
            float a1[3];
            DirToLot(xf, r.a1, a1);
            GpuLampLaw::ShadedCone(L, a1, g.s5, r.o1, g.s5, r.o2, r.S);
        }
    }
    L.r0sq = 1e30f, L.invR2 = 0.0f;
    if (reachOn) {
        float r0, R;
        LampReach::Radii(w, r0, R);
        L.r0sq = r0 * r0;
        L.invR2 = 1.0f / (R * R);
    }
}
double Ms(const LARGE_INTEGER& a, const LARGE_INTEGER& b) {
    LARGE_INTEGER f{};
    QueryPerformanceFrequency(&f);
    return f.QuadPart ? 1000.0 * static_cast<double>(b.QuadPart - a.QuadPart) / static_cast<double>(f.QuadPart) : 0.0;
}
inline void Mix(uint64_t& h, const void* p, size_t n) {
    const BYTE* b = static_cast<const BYTE*>(p);
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
}

// ---- writes noted from any thread ----
std::atomic<void*> g_watchTex[kMaxWatch];
std::atomic<uint64_t> g_watchDirty{0};
std::mutex g_noteMx;
std::vector<std::pair<uintptr_t, int>> g_solvedNotes; // (story manager, class) of rooms whose solve ended (under g_noteMx)

// ---- the occluders (built on the light tree thread, read on the render thread) ----
struct Proxy {
    uintptr_t tracker = 0;
    uint64_t geo = 0;
    uint32_t gen = 0;
    RtLampProxy::Lot lot;
};
std::mutex g_proxyMx;
std::shared_ptr<const Proxy> g_proxy;         // (under g_proxyMx)
std::atomic<uintptr_t> g_proxyWanted{0};      // the lot whose occluders the render thread waits for
std::atomic<uint32_t> g_proxyGen{1};
std::string g_proxyWhy;                       // (under g_proxyMx) why the last build failed

// ---- coverage and lamp edits (render thread publishes, the light tree thread reads) ----
struct Cover {
    uintptr_t tracker = 0;
    bool full = false;
    DWORD at = 0;
    std::unordered_set<uintptr_t> lamps;
};
std::mutex g_coverMx;
Cover g_cover;
struct EditNote {
    uintptr_t tracker = 0, light = 0;
    int story = 0, kind = 0;
    LARGE_INTEGER at{};
};
std::mutex g_editMx;
std::vector<EditNote> g_edits;                  // edits not yet shown (under g_editMx)
// (tracker, story) -> its edits since the last send: their dependents are sent kHoldQuietMs after the last one, or kDragQuietMs
// when none moved a lamp (a value or a switch: no drag pauses to wait out); light tree thread
struct SendDue {
    DWORD at = 0;                 // the last edit
    bool moved = false;           // one of them moved a lamp
    std::vector<uintptr_t> lamps; // the lamps edited (the roofed rooms that saw a door of one are sent)
};
std::map<std::pair<uintptr_t, int>, SendDue> g_sendDue;
std::atomic<long> c_repairs{0}; // story room 0s sent to gather a GPU lamp they lost (the consistency check)
std::atomic<long> c_listKept{0}; // frames a story's room-0 list could not be read and its last list was kept
// The last room-0 list read per story (render thread): kept for a few frames when a list cannot be read
constexpr int kKeptListFrames = 4;
struct KeptList {
    uintptr_t tracker = 0, mgr = 0;
    const void* room = nullptr;
    int misses = 0;
    std::vector<uintptr_t> list;
};
KeptList g_keptList[8];
struct ExportBackoff {
    DWORD at = 0;
    int fails = 0;
};
std::map<const void*, ExportBackoff> g_exportBackoff; // render thread: room -> the wait after its kept solve could not be read
std::atomic<uintptr_t> g_switchOn{0}; // a GPU lamp switched on (LampMarkFilter, light tree thread; the log on the render thread)

// ---- the maps (render thread) ----
struct Slot {
    int story = 0, m = 0, cls = 0;
    int retries = 0;   // uploads that failed for a buffer in a row (UploadSlot)
    DWORD retryAt = 0; // not built again before this tick (0 = no wait)
    const BYTE* room = nullptr;
    IDirect3DTexture9* game = nullptr; // the room's own map (AddRef'd while kept)
    IDirect3DTexture9* copy = nullptr; // DEFAULT render target: what the draws read instead
    IDirect3DTexture9* scale = nullptr; // DEFAULT render target: each texel's outdoor scale (maps with no blur; ScaleOf)
    bool scaled = false;                // the last relight wrote it
    IDirect3DVertexBuffer9 *vb = nullptr, *dyn = nullptr;
    IDirect3DVertexBuffer9* dynNext = nullptr; // the other floor mask buffer: a write fills the one the last draws did not read
    IDirect3DTexture9* preblur = nullptr;
    RtLampGpu::Map map;
    uint32_t version = 0;
    bool built = false, ok = false, relit = false, logged = false;
    bool used = true; // the kept solve wrote texels into this map
    std::string why;
    float c[3] = {}, rad = 0.0f; // the texels' bounding sphere (lot space)
    std::vector<uint32_t> masked; // texels of the floor mask (their index)
    std::vector<float> maskedPos; // their lot positions (x, y, z), world y for the gate
    std::vector<uint8_t> gated;
    float thr = 0.0f;
    bool dither = false;
    std::unordered_set<uintptr_t> kept, drawn;
    uint64_t sig = 0; // what it was relit with
    long binds = 0;
    int watch = -1;
    bool fast = false;              // relit with the dragged lamp's cheaper shadows (relit exactly once the drag ends)
    bool pendingWrite = false;      // the game wrote its map (a solve, an unlock) since it was relit
    uint64_t relitTick = 0;         // GetTickCount64 of its last relight
    uint64_t lampsSig = 0, paramsSig = 0; // what its last relight took (the reasons logged)
    uint32_t meshGen = 0, relitVersion = 0;
    // the masked texels' share of the bake under the floor (-1 none): exact from a worker thread (UnderShare's float sums),
    // brought up to date between those by each baked lamp's stamp taken out and put in again (uA, uU: the sums in double)
    std::vector<float> under;
    std::vector<double> uA, uU;
    std::vector<RtLampModel::BakeLamp> uSnap; // the baked lamps under, uA and uU stand for
    std::vector<uint64_t> uIds;               // their keys (light, its place among the light's entries)
    float uBase = 0.0f;
    uint64_t underSig = 0;
    bool underExact = false, underDirty = false;
    std::shared_ptr<struct UnderJob> uJob;
    bool dynCurrent = false;        // the mask buffer holds the under-floor share with "all" left to the shader
};
struct Ask {
    DWORD at = 0;
    int count = 0;
    bool logged = false;
};
std::vector<std::unique_ptr<Slot>> g_slots;
uintptr_t g_tracker = 0;
uint64_t g_lotId = 0;
// A completed native solve with no remaining send is an explicit fallback, not queued GPU work.
bool RestartSolveTerminal(const Ask& ask, int state, bool budgetOut) {
    return (state == 4 || state == 5) && (budgetOut || (ask.count >= kSolveAsks && ask.logged));
}
bool g_restartGpuPending = false, g_restartGpuFallback = false;
std::unordered_map<const void*, Ask> g_askedSolve; // room -> its sends to its solve for a kept solve
std::unordered_map<const IDirect3DBaseTexture9*, Slot*> g_swap; // the game's map -> its slot (the SetTexture swap; render thread)
uintptr_t g_swapLo = ~uintptr_t{0}, g_swapHi = 0;               // the address range of its keys (most binds end at this test)
uint64_t g_swapSig = 0;
uint64_t g_lampSigAll = 0;
DWORD g_lampChangedAt = 0, g_lastRelightAt = 0;
size_t g_rr = 0;                 // the first slot looked at (round robin over frames)
long g_pinDrops = 0;
bool g_budgetOut = false;        // the active lot's kept parts do not fit the memory budget (logged once)
int g_coverLogged = -1;          // the coverage last logged (0 incomplete, 1 every story)
double g_frameMsSum = 0;         // Frame's own CPU time a frame (status: its running average)
long g_frameCount = 0;
struct Phases {
    double slots = 0, lamps = 0, mask = 0, draw = 0, shadows = 0;
};
bool g_roundOpen = false;        // a relight spread over frames is under way
double g_roundCpuMs = 0;         // its CPU time so far
int g_roundMaps = 0, g_roundFrames = 0, g_roundTiles = 0;
std::vector<int> g_roundStories;
// What made maps relit, and the frame's own cost, over the last 10 s (the self-measure line; render thread)
struct Why {
    long first = 0, lamp = 0, base = 0, params = 0, occluders = 0, gameWrite = 0, exact = 0, build = 0, writeGap = 0;
    long relights = 0;
    double frameMs = 0, frameMax = 0;
    long frames = 0;
    DWORD since = 0;
};
Why g_why;
double g_buildMs = 0; // worker time of the maps prepared since the last self-measure line
struct LampSnap {
    float pos[3], colour[3], range, intensity;
    int type;
    uint8_t flags;
};
std::unordered_map<uintptr_t, LampSnap> g_lampSnap; // the GPU lamps as last read (what changed, logged)
DWORD g_lampLogAt = 0;
Phases g_ph;                     // the phases of the relight being logged (CPU ms, added over the frames of one relight)
std::unordered_set<uint64_t> g_lotNoted;            // lots whose lamp lot id differed (logged once)
std::shared_ptr<const Proxy> g_proxyUsed;
RtLampProxy::Mesh g_mesh;
uint32_t g_meshGen = 0;
uint64_t g_atlasKey = 0;
uint64_t g_lastParams = 0;
DWORD g_frame = 0, g_lastFrame = 0;
bool g_installed = false;
IDirect3DDevice9* g_dev = nullptr; // the game's device (render thread)
int g_ps3 = -1; // the device runs shader model 3 (1), not (0), not read yet (-1) (render thread)
int g_mrt = -1; // the device draws into two render targets at once (the outdoor scale), not (0), not read yet (-1)
DWORD g_makeFailAt = 0; // (render thread) the shader objects' last failed making
int g_makeFails = 0;     // and how many in a row (5: given up until the next device reset)
IDirect3DQuery9 *g_qDisjoint = nullptr, *g_qFreq = nullptr, *g_qBegin = nullptr, *g_qEnd = nullptr;
bool g_qPending = false;
long g_logs = 0;
DWORD g_logSecond = 0;
int g_logInSecond = 0;
bool LogAllowed() {
    const DWORD now = GetTickCount();
    if (now - g_logSecond >= 1000) g_logSecond = now, g_logInSecond = 0;
    return ++g_logInSecond <= 6;
}

void Inactive(const std::string& why) {
    g_inactive = why;
    if (why != g_inactiveLogged) {
        g_inactiveLogged = why;
        LOG_INFO(why.empty() ? "[RtOutdoorLamps] Active: outdoor lamps of the active lot are drawn on the GPU into every story's room-0 maps"
                             : "[RtOutdoorLamps] Inactive: " + why);
    }
}

void ReleaseSlot(Slot& s) {
    if (s.watch >= 0) g_watchTex[s.watch].store(nullptr), s.watch = -1;
    if (s.copy) s.copy->Release(), s.copy = nullptr;
    if (s.scale) s.scale->Release(), s.scale = nullptr;
    s.scaled = false;
    if (s.vb) s.vb->Release(), s.vb = nullptr;
    if (s.dyn) s.dyn->Release(), s.dyn = nullptr;
    if (s.dynNext) s.dynNext->Release(), s.dynNext = nullptr;
    if (s.preblur) s.preblur->Release(), s.preblur = nullptr;
    if (s.game) s.game->Release(), s.game = nullptr;
    s.map = RtLampGpu::Map{};
}
// Every stage that holds a copy takes the game's map back (render thread)
void UnbindCopies(IDirect3DDevice9* dev) {
    if (!dev) return;
    for (DWORD st = 0; st < 16; st++) {
        IDirect3DBaseTexture9* t = nullptr;
        if (FAILED(dev->GetTexture(st, &t)) || !t) continue;
        for (const auto& s : g_slots)
            if (s->copy && t == s->copy) D3D9Hooks::CallOriginalSetTexture(dev, st, s->game);
        t->Release();
    }
}
// The SetTexture swap's table: the game's map -> its relit copy (rebuilt when a slot's maps changed; render thread)
void RebuildSwap() {
    uint64_t h = 1469598103934665603ull;
    for (const auto& s : g_slots) {
        const bool on = s->copy && s->relit && s->game;
        const void* v[2] = {on ? s->game : nullptr, on ? s->copy : nullptr};
        Mix(h, v, sizeof v);
    }
    if (h == g_swapSig && !(g_slots.empty() && !g_swap.empty())) return;
    g_swapSig = h;
    g_swap.clear();
    g_swapLo = ~uintptr_t{0}, g_swapHi = 0;
    for (const auto& s : g_slots)
        if (s->copy && s->relit && s->game) {
            g_swap[s->game] = s.get();
            const uintptr_t a = reinterpret_cast<uintptr_t>(s->game);
            g_swapLo = std::min(g_swapLo, a), g_swapHi = std::max(g_swapHi, a);
        }
}
struct Job;
std::vector<std::shared_ptr<Job>> g_jobs; // render thread: the stories whose maps are being prepared (a worker thread each, one at a time)
void DropAll(IDirect3DDevice9* dev, const char* why, bool requeue = true) {
    g_jobs.clear(); // (a job still running finishes on its own)
    g_swap.clear();
    g_swapLo = ~uintptr_t{0}, g_swapHi = 0;
    g_swapSig = 0;
    OutdoorDelta::PinRooms(nullptr, 0); // (no relit map stands for a kept room any more: the budget pins none)
    if (g_slots.empty() && !g_eng) return;
    UnbindCopies(dev);
    const size_t n = g_slots.size();
    for (auto& s : g_slots) ReleaseSlot(*s);
    g_slots.clear();
    g_watchDirty.store(0);
    g_atlasKey = 0;
    {
        std::lock_guard<std::mutex> lk(g_coverMx);
        g_cover = Cover{};
    }
    // the game's room-0 maps never took the edits the GPU stood in for: each story's room 0 of that lot is solved again
    // (only while the game still has that lot: after a world change or a lot's teardown its tracker is gone)
    int sent = 0;
    if (requeue && n && g_tracker && LevelLightShare::TrackerLive(g_tracker))
        for (int S = 0; S <= 7; S++)
            if (LevelLightShare::Room0Of(g_tracker, S)) sent += LevelLightShare::QueueRoom0(g_tracker, S) ? 1 : 0;
    if (n) LOG_INFO(std::format("[RtOutdoorLamps] {} relit maps dropped ({}): the game's maps are bound again, {} rooms 0 sent to their solve", n, why, sent));
}

// ---- the floor mask's under-floor share ----
constexpr float kUnderEps = 0.02f;
// UnderShare's float result at each gated masked texel, and its two sums in double (the base for later lamp changes)
void FullUnder(const std::vector<float>& maskedPos, const std::vector<uint8_t>& gated, const std::vector<RtLampModel::BakeLamp>& bake, float base,
               std::vector<float>& under, std::vector<double>& A, std::vector<double>& U) {
    const size_t n = gated.size();
    under.assign(n, -1.0f), A.assign(n, 0.0), U.assign(n, 0.0);
    for (size_t i = 0; i < n; i++) {
        if (!gated[i]) continue;
        const float* s = &maskedPos[i * 6 + 3];
        under[i] = RtLampModel::UnderShare(bake.data(), static_cast<int>(bake.size()), s, base, kUnderEps);
        for (const auto& L : bake) {
            float v;
            bool u;
            if (!RtLampModel::BakeStamp(L, s, base, kUnderEps, v, u)) continue;
            A[i] += v;
            if (u) U[i] += v;
        }
    }
}
struct UnderJob {
    std::vector<float> pos;
    std::vector<uint8_t> gated;
    std::vector<RtLampModel::BakeLamp> bake;
    std::vector<uint64_t> ids;
    float base = 0.0f;
    uint64_t sig = 0;
    std::atomic<int> state{1}; // 1 running, 2 done
    std::vector<float> under;
    std::vector<double> A, U;
};
uint64_t UnderSig(uint64_t bakeSig, float base) {
    uint64_t h = bakeSig;
    Mix(h, &base, sizeof base);
    const bool lift = TerrainBakeRule::LowLampLift(); // the stamps' heights (BakeStamp)
    Mix(h, &lift, sizeof lift);
    return h;
}
// The keys of a story's baked lamps: the light and its place among that light's entries (a lamp listed twice stamps twice)
std::vector<uint64_t> BakeKeys(const std::vector<uintptr_t>& lights) {
    std::vector<uint64_t> k(lights.size());
    for (size_t i = 0; i < lights.size(); i++) {
        uint64_t occ = 0;
        for (size_t j = 0; j < i; j++) occ += lights[j] == lights[i] ? 1 : 0;
        k[i] = static_cast<uint64_t>(lights[i]) << 8 | occ;
    }
    return k;
}
// The share brought up to date with the baked lamps now: each changed, gone or new lamp's stamp out of or into the sums
bool UnderDelta(Slot& s, const std::vector<RtLampModel::BakeLamp>& now, const std::vector<uint64_t>& ids) {
    std::vector<std::pair<RtLampModel::BakeLamp, double>> ch;
    for (size_t i = 0; i < s.uIds.size(); i++) {
        const auto it = std::find(ids.begin(), ids.end(), s.uIds[i]);
        if (it == ids.end() || std::memcmp(&now[static_cast<size_t>(it - ids.begin())], &s.uSnap[i], sizeof(RtLampModel::BakeLamp)) != 0) ch.emplace_back(s.uSnap[i], -1.0);
    }
    for (size_t i = 0; i < ids.size(); i++) {
        const auto it = std::find(s.uIds.begin(), s.uIds.end(), ids[i]);
        if (it == s.uIds.end() || std::memcmp(&s.uSnap[static_cast<size_t>(it - s.uIds.begin())], &now[i], sizeof(RtLampModel::BakeLamp)) != 0) ch.emplace_back(now[i], 1.0);
    }
    if (!ch.empty())
        for (size_t i = 0; i < s.gated.size(); i++) {
            if (!s.gated[i]) continue;
            const float* p = &s.maskedPos[i * 6 + 3];
            for (const auto& [L, sign] : ch) {
                float v;
                bool u;
                if (!RtLampModel::BakeStamp(L, p, s.uBase, kUnderEps, v, u)) continue;
                s.uA[i] += sign * v;
                if (u) s.uU[i] += sign * v;
            }
            s.under[i] = s.uA[i] > 1e-4 ? static_cast<float>(std::clamp(s.uU[i] / s.uA[i], 0.0, 1.0)) : -1.0f;
        }
    s.uSnap = now, s.uIds = ids;
    return !ch.empty();
}
// The baked lamps of each story as last read (a story's maps prepared on the worker start from them)
std::vector<RtLampModel::BakeLamp> g_bakeLast[8];
uint64_t g_bakeSigLast[8] = {};
std::vector<uint64_t> g_bakeKeysLast[8];
float g_lotBaseLast[8] = {};

// ---- a story's maps from its kept solve: prepared on a worker thread (the export and the texel streams), uploaded on the
// render thread one map a frame ----
struct Prepared {
    bool used = false, ok = false;
    std::string why;
    uint32_t w = 0, h = 0;
    std::vector<RtLampGpu::TexelVertex> v;
    std::vector<uint32_t> masked;
    std::vector<float> maskedPos;
    std::vector<uint8_t> gated;
    float c[3] = {}, rad = 0.0f;
    bool blur = false;
    UINT passes = 0;
    std::vector<RtLampGpu::Block> blocks;
    std::vector<uint32_t> px; // the blocks' texels before the blur (w x h)
    std::vector<float> under;  // the floor mask's under-floor share (exact) and its sums
    std::vector<double> uA, uU;
};
struct Job {
    const void* room = nullptr;
    int story = 0, cls = 0;
    uint32_t ver = 0;
    RoomNow rn;
    std::atomic<int> state{0}; // 1 running, 2 done, 3 failed
    uint32_t version = 0;
    float thr = 0.0f;
    bool dither = false;
    std::vector<uintptr_t> kept, drawn;
    Prepared maps[4];
    double ms = 0;
    std::vector<RtLampModel::BakeLamp> bake; // the story's baked lamps when it started (the under-floor share)
    std::vector<uint64_t> bakeKeys;
    float base = 0.0f;
};
void PrepareJob(Job& j) {
    const bool groundShade = LevelLightShare::GroundShadeOn();
    LARGE_INTEGER a{}, b{};
    QueryPerformanceCounter(&a);
    OutdoorDelta::Room0Export ex;
    if (!OutdoorDelta::ExportRoom0(j.room, j.cls, ex)) {
        j.state.store(3);
        return;
    }
    j.version = ex.version, j.thr = ex.thr, j.dither = ex.dither;
    j.kept = std::move(ex.kept), j.drawn = std::move(ex.drawn);
    for (int m = 0; m < 4; m++) {
        Prepared& p = j.maps[m];
        p.used = ex.mapUsed[m];
        p.w = ex.mapW[m], p.h = ex.mapH[m];
        if (!p.used) continue;
        p.blur = m == 0 && ex.cls == 2 && ex.passes > 0;
        if (p.blur && !ex.wallsOk) {
            p.why = "its walls were blurred by the game's own blur (no blocks before the blur)";
            continue;
        }
        double cx = 0, cy = 0, cz = 0;
        for (const OutdoorDelta::Room0Export::Texel& t : ex.texels) {
            if (t.map != m) continue;
            RtLampGpu::TexelVertex tv{};
            tv.x = t.x, tv.y = t.y, tv.w = 1.0f;
            ToLot(j.rn.xf, t.pos, tv.p);
            float n[3];
            DirToLot(j.rn.xf, t.nrm, n);
            const float nl = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            for (int k = 0; k < 3; k++) tv.n[k] = nl > 1e-6f ? n[k] / nl : 0.0f;
            tv.p[3] = static_cast<float>(t.bits & 3);
            // (15/10) the lot's ground on story 0 (turned up, within 1 m of the lot's ground, not a masked floor): bit 4, no wall
            // test on the GPU; the lot passes shade it with the smooth ground shade (unverified: lot space y 0 = the lot's ground)
            if (groundShade && j.story == 0 && !(t.bits & 1) && tv.n[1] > 0.9f && std::fabs(tv.p[1]) < 1.0f) tv.p[3] += 4.0f;
            tv.n[3] = static_cast<float>(t.packed >> 24) / 255.0f;
            for (int k = 0; k < 3; k++) tv.base[k] = std::isfinite(t.base[k]) ? t.base[k] : 0.0f;
            tv.base[3] = ex.dither ? RoomMapDither::Offset(t.x, t.y) / 255.0f : 0.0f;
            tv.mask[0] = t.vis, tv.mask[1] = t.all;
            if (t.bits & 1) {
                p.masked.push_back(static_cast<uint32_t>(p.v.size()));
                p.maskedPos.insert(p.maskedPos.end(), {tv.p[0], tv.p[1], tv.p[2], t.pos[0], t.pos[1], t.pos[2]});
                p.gated.push_back((t.bits & 2) ? 1 : 0);
            }
            cx += tv.p[0], cy += tv.p[1], cz += tv.p[2];
            p.v.push_back(tv);
        }
        if (p.v.empty()) {
            p.why = "no texel of the kept solve in it";
            continue;
        }
        p.c[0] = static_cast<float>(cx / p.v.size()), p.c[1] = static_cast<float>(cy / p.v.size()), p.c[2] = static_cast<float>(cz / p.v.size());
        for (const auto& tv : p.v) {
            const float dx = tv.p[0] - p.c[0], dy = tv.p[1] - p.c[1], dz = tv.p[2] - p.c[2];
            p.rad = std::max(p.rad, std::sqrt(dx * dx + dy * dy + dz * dz));
        }
        if (p.blur) {
            p.px.assign(static_cast<size_t>(p.w) * p.h, 0);
            for (const auto& bl : ex.blocks) {
                if (!bl.ok) continue;
                const int n = bl.x1 - bl.x0 + 1;
                for (int y = bl.y0; y <= bl.y1; y++)
                    for (int x = bl.x0; x <= bl.x1; x++) {
                        const size_t src = bl.off + static_cast<size_t>(y - bl.y0) * n + (x - bl.x0);
                        if (x >= 0 && y >= 0 && x < static_cast<int>(p.w) && y < static_cast<int>(p.h) && src < ex.preblur.size())
                            p.px[static_cast<size_t>(y) * p.w + x] = ex.preblur[src];
                    }
                p.blocks.push_back(RtLampGpu::Block{bl.x0, bl.y0, bl.x1, bl.y1});
            }
            if (p.blocks.empty()) {
                p.why = "its wall blocks before the blur could not be kept";
                continue;
            }
            p.passes = ex.passes;
        }
        if (!p.masked.empty()) FullUnder(p.maskedPos, p.gated, j.bake, j.base, p.under, p.uA, p.uU);
        p.ok = true;
    }
    QueryPerformanceCounter(&b);
    j.ms = Ms(a, b);
    j.state.store(2);
}
void StartJob(const std::shared_ptr<Job>& j) {
    j->state.store(1);
    if (!HookGuard::StartDetached("RtOutdoorLamps map job", [j] {
            try {
                PrepareJob(*j);
            } catch (...) {
                j->state.store(3);
            }
        }))
        j->state.store(3); // (no thread: the job failed)
}
// The slot from its prepared map (render thread: the buffers and textures only)
bool UploadSlot(IDirect3DDevice9* dev, Slot& s, const Job& j, Prepared& p) {
    s.built = true;
    s.ok = false;
    s.version = j.version;
    s.thr = j.thr;
    s.dither = j.dither;
    s.kept = std::unordered_set<uintptr_t>(j.kept.begin(), j.kept.end());
    s.drawn = std::unordered_set<uintptr_t>(j.drawn.begin(), j.drawn.end());
    if (s.vb) s.vb->Release(), s.vb = nullptr;
    if (s.dyn) s.dyn->Release(), s.dyn = nullptr;
    if (s.dynNext) s.dynNext->Release(), s.dynNext = nullptr;
    if (s.preblur) s.preblur->Release(), s.preblur = nullptr;
    s.masked.clear(), s.maskedPos.clear(), s.gated.clear(), s.under.clear(), s.uA.clear(), s.uU.clear(), s.uSnap.clear(), s.uIds.clear();
    s.underSig = 0, s.underExact = false, s.underDirty = false, s.uJob.reset();
    s.dynCurrent = false;
    s.map = RtLampGpu::Map{};
    D3DSURFACE_DESC d{};
    if (!s.game || FAILED(s.game->GetLevelDesc(0, &d)) || d.Format != D3DFMT_A8R8G8B8 || s.game->GetLevelCount() != 1 || d.Width != p.w || d.Height != p.h) {
        s.why = std::format("its own map is not the kept solve's ({}x{} format {} levels {}; kept {}x{})", d.Width, d.Height, static_cast<int>(d.Format),
                            s.game ? s.game->GetLevelCount() : 0, p.w, p.h);
        return false;
    }
    if (!p.ok) {
        s.why = p.why;
        return false;
    }
    // (14/10) a buffer that could not be made (device memory, a lost device): the slot is built again later, with a backoff
    const auto retryLater = [&s](const char* why) {
        s.why = why;
        s.built = false;
        s.retries = std::min(s.retries + 1, 6);
        s.retryAt = (GetTickCount() + (1000u << (s.retries - 1))) | 1; // 1 s, 2 s ... 32 s
        return false;
    };
    if (!RtLampGpu::Engine::MakeVb(dev, p.v.data(), static_cast<UINT>(p.v.size()), &s.vb)) return retryLater("its texel buffer could not be made");
    s.map.w = static_cast<int>(d.Width), s.map.h = static_cast<int>(d.Height);
    s.map.vb = s.vb;
    s.map.count = static_cast<UINT>(p.v.size());
    s.masked = std::move(p.masked), s.maskedPos = std::move(p.maskedPos), s.gated = std::move(p.gated);
    s.under = std::move(p.under), s.uA = std::move(p.uA), s.uU = std::move(p.uU);
    s.uSnap = j.bake, s.uIds = j.bakeKeys, s.uBase = j.base;
    s.underExact = true;
    for (int k = 0; k < 3; k++) s.c[k] = p.c[k];
    s.rad = p.rad;
    if (!s.masked.empty()) {
        std::vector<RtLampGpu::TexelDyn> z(p.v.size(), RtLampGpu::TexelDyn{{0, -1, 0, 0}});
        if (!RtLampGpu::Engine::MakeDyn(dev, z.data(), static_cast<UINT>(z.size()), &s.dyn)) return retryLater("its floor mask buffer could not be made");
        // (both start from the same zeros, and every write fills all the masked texels, so either one is complete; a lock
        // of the one the GPU may still read waits for it under DXVK: the writes alternate. Without the second, as before)
        // (DXVK only: native D3D9 copies a MANAGED buffer at its lock without waiting, and the copy costs RAM)
        if (Platform::DxvkD3D9() && !RtLampGpu::Engine::MakeDyn(dev, z.data(), static_cast<UINT>(z.size()), &s.dynNext)) s.dynNext = nullptr;
        s.map.dyn = s.dyn;
    }
    if (p.blur) {
        if (!RtLampGpu::Engine::MakePreblur(dev, static_cast<int>(d.Width), static_cast<int>(d.Height), p.px.data(), &s.preblur))
            return retryLater("its wall blocks before the blur could not be kept");
        s.map.blocks = p.blocks;
        s.map.blur = true;
        s.map.passes = p.passes;
        s.map.preblur = s.preblur;
    }
    p.v = {}, p.px = {};
    s.ok = true;
    s.retries = 0, s.retryAt = 0;
    s.why.clear();
    s.sig = 0; // relit again
    return true;
}

// ---- the GPU lamps of the lot now ----
struct GpuLamp {
    uintptr_t light;
    RtLampModel::Lamp lamp; // lot space; mult per story is set per map
    float esum, spill;
    int mult[8] = {};
    bool liftable = false; // drawn from kLowLampLift on story 0's floor map when it lies on the ground
};
struct BakeLampW {
    RtLampModel::BakeLamp b; // world space (the gate is measured in world y against the lot base)
};

} // namespace

bool LowLampLift() { return TerrainBakeRule::LowLampLift(); }
void SetLowLampLift(bool on) { TerrainBakeRule::g_lowLampLift.store(on, std::memory_order_relaxed); }
long LowLampLifted() { return g_lowLampLifted.load(std::memory_order_relaxed); }

void SetEnabled(bool on) {
    if (g_option.exchange(on) == on) return;
    LOG_INFO(on ? "[RtOutdoorLamps] On: outdoor lamps of the active lot are drawn on the GPU into the room-0 maps of every story"
                : "[RtOutdoorLamps] Off: the room-0 maps are the game's again");
    if (on) g_inactiveLogged = "?"; // (the next state is logged)
}
bool Enabled() { return g_option.load(std::memory_order_relaxed); }
bool Watching() { return g_watch.load(std::memory_order_relaxed); }

namespace {
// The SetTexture swap (render thread, lock-free chain): the game's map -> its copy
HRESULT SwapCopy(IDirect3DDevice9* dev, DWORD stage, IDirect3DBaseTexture9* t, bool& swapped) {
    swapped = false;
    if (!t || !g_on.load(std::memory_order_relaxed) || g_swap.empty()) return S_OK;
    const uintptr_t a = reinterpret_cast<uintptr_t>(t);
    if (a < g_swapLo || a > g_swapHi) return S_OK;
    const auto it = g_swap.find(t);
    if (it == g_swap.end()) return S_OK;
    Slot* s = it->second;
    s->binds++;
    swapped = true;
    return D3D9Hooks::CallOriginalSetTexture(dev, stage, s->copy);
}
void OnPreReset(IDirect3DDevice9* dev) {
    UnbindCopies(dev);
    for (auto& s : g_slots) {
        if (s->copy) s->copy->Release(), s->copy = nullptr;
        if (s->scale) s->scale->Release(), s->scale = nullptr;
        s->scaled = false;
        s->relit = false; // (14/10: its preblur texture is a MANAGED one: kept, and the slot stays built; only the copy is made again)
    }
    if (g_eng) g_eng->ReleaseDefault();
    for (IDirect3DQuery9** q : {&g_qDisjoint, &g_qFreq, &g_qBegin, &g_qEnd})
        if (*q) (*q)->Release(), *q = nullptr;
    g_qPending = false;
    g_atlasKey = 0;
    RebuildSwap();
    // shader objects that could not be made (a reset may have been under way): tried again after the reset, as the code compiled
    if (g_compiled.state.load() == 3 && !g_compiled.code[0].empty()) {
        g_compiled.state.store(2);
        g_makeFails = 0;
        LOG_INFO("[RtOutdoorLamps] Device reset: the shader objects that could not be made are tried again");
    }
    LOG_INFO("[RtOutdoorLamps] Device reset: the relit maps and shadow maps are made again");
}

// The frame's work (EndScene, once a frame): find the active lot's maps, rebuild what changed, relight what changed
void Frame(IDirect3DDevice9* dev);
void OnEndScene(IDirect3DDevice9* dev) {
    if (g_frame == g_lastFrame) return;
    g_lastFrame = g_frame;
    LARGE_INTEGER a{}, b{};
    QueryPerformanceCounter(&a);
    if (!HookGuard::Run("RtOutdoorLamps frame", [dev] { Frame(dev); }) && (g_on.load() || !g_slots.empty())) {
        // Frame threw (now or before: it is off for the session): the game's maps are bound again and edits go to its solves
        g_on.store(false);
        g_watch.store(false);
        HookGuard::Try("RtOutdoorLamps frame drop", [dev] {
            {
                std::lock_guard<std::mutex> lk(g_editMx); // (AfterRoomUpdate no longer runs: nothing would send them)
                g_sendDue.clear();
            }
            DropAll(dev, "its frame work stopped after an error");
            g_inactive = "stopped after an error (see ApexRadiance_LOG.txt)";
        });
    }
    RebuildSwap();
    QueryPerformanceCounter(&b);
    const double fm = Ms(a, b);
    g_frameMsSum = g_frameCount ? g_frameMsSum * 0.98 + fm * 0.02 : fm;
    g_frameCount++;
    g_why.frameMs += fm, g_why.frames++;
    g_why.frameMax = std::max(g_why.frameMax, fm);
    const DWORD now = GetTickCount();
    if (!g_why.since) g_why.since = now;
    if (now - g_why.since >= 10000) {
        if (g_option.load(std::memory_order_relaxed)) {
            const Why& w = g_why;
            LOG_INFO(std::format("[RtOutdoorLamps] Cost over {:.0f} s: {:.3f} ms a frame on average (largest {:.2f} ms) on the render thread; {} relights, maps relit "
                                 "because: first {}, a lamp within reach changed {}, the kept base changed {}, the game wrote the map {}, exact after a drag {}, "
                                 "options {}, occluders {}; {} relights for repeated game writes moved later; {} story maps prepared ({:.1f} ms on a worker thread){}",
                                 (now - w.since) / 1000.0, w.frames ? w.frameMs / static_cast<double>(w.frames) : 0.0, w.frameMax, w.relights, w.first, w.lamp,
                                 w.base, w.gameWrite, w.exact, w.params, w.occluders, w.writeGap, w.build, g_buildMs,
                                 g_inactive.empty() ? std::string() : " | inactive: " + g_inactive));
        }
        g_why = Why{};
        g_why.since = now;
        g_buildMs = 0;
    }
}

void ReadGpuTime() {
    if (!g_qPending || !g_qDisjoint) return;
    BOOL disjoint = TRUE;
    UINT64 freq = 0, t0 = 0, t1 = 0;
    if (g_qDisjoint->GetData(&disjoint, sizeof disjoint, 0) != S_OK || g_qFreq->GetData(&freq, sizeof freq, 0) != S_OK || g_qBegin->GetData(&t0, sizeof t0, 0) != S_OK ||
        g_qEnd->GetData(&t1, sizeof t1, 0) != S_OK)
        return; // not yet
    g_qPending = false;
    if (!disjoint && freq && t1 >= t0) g_lastGpuMs.store(static_cast<float>(1000.0 * static_cast<double>(t1 - t0) / static_cast<double>(freq)));
}

void Frame(IDirect3DDevice9* dev) {
    g_restartGpuPending = false;
    g_restartGpuFallback = false;
    ReadGpuTime();
    // what makes the option do nothing now, logged once each
    if (!g_option.load(std::memory_order_relaxed)) {
        if (!g_slots.empty()) {
            // the game's room-0 maps may lack lamp edits the GPU stood in for (OutdoorDelta left their deltas out): each
            // story's room 0 is solved again
            int sent = 0;
            for (int S = 0; S <= 7 && g_tracker && LevelLightShare::TrackerLive(g_tracker); S++)
                if (LevelLightShare::Room0Of(g_tracker, S)) sent += LevelLightShare::QueueRoom0(g_tracker, S) ? 1 : 0;
            if (sent) LOG_INFO(std::format("[RtOutdoorLamps] Switched off: {} room 0s of the active lot sent to their solve (their maps are the game's again)", sent));
            DropAll(dev, "switched off");
        }
        g_on.store(false);
        g_watch.store(false);
        g_inactive = "off";
        return;
    }
    const auto off = [&](const std::string& why) {
        if (!g_slots.empty()) DropAll(dev, why.c_str());
        g_on.store(false);
        g_watch.store(false);
        Inactive(why);
    };
    if (g_ps3 == 0) return off("the device has no shader model 3");
    // (the room maps' texture sets are read at the struct offsets studied on Steam 1.67.2; UNVERIFIED on other builds)
    if (std::string missing; !GameAddr::Have({GameAddr::Id::LampK1, GameAddr::Id::LampK2, GameAddr::Id::LampCmax, GameAddr::Id::StreetLampScale, GameAddr::Id::ConeScale5}, &missing))
        return off(GameAddr::NotAvailable(missing));
    if (!LevelLightShare::IsInstalled() || !LevelLightShare::DeltaReady()) return off("the room solve hooks are not in (Night Lighting's Outdoor light between floors)");
    if (!OutdoorDelta::Enabled()) return off("Quick outdoor lamp edits keep no room-0 parts (it is off or not installed)");
    if (!g_texVt) {
        IDirect3DTexture9* probe = nullptr;
        if (SUCCEEDED(dev->CreateTexture(1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &probe, nullptr)) && probe) {
            g_texVt = *reinterpret_cast<void**>(probe);
            probe->Release();
        }
        if (!g_texVt) return off("no texture could be made to identify the game's maps");
    }
    // the shaders: compiled once on a worker thread
    const int cs = g_compiled.state.load();
    if (cs == 0) {
        g_compiled.state.store(1);
        if (!HookGuard::StartDetached("RtOutdoorLamps shader bytecode", [] { CompileAll(); })) {
            g_compiled.error = "the compile thread could not start";
            g_compiled.state.store(3);
        }
        LOG_INFO("[RtOutdoorLamps] Taking the shadow and relight shaders from the shader cache");
        return off("its shaders are being compiled");
    }
    if (cs == 1) return off("its shaders are being compiled");
    if (cs == 3) return off("its shaders do not compile: " + g_compiled.error);
    if (!g_eng && g_makeFails && GetTickCount() - g_makeFailAt < 5000) return off("its shader objects are made again shortly");
    if (!g_eng) {
        void* made[RtLampGpu::kShaderCount] = {};
        bool ok = true;
        for (int i = 0; i < RtLampGpu::kShaderCount && ok; i++) {
            const auto id = static_cast<RtLampGpu::ShaderId>(i);
            ok = RtLampGpu::IsVertex(id) ? SUCCEEDED(dev->CreateVertexShader(g_compiled.code[i].data(), reinterpret_cast<IDirect3DVertexShader9**>(&made[i])))
                                         : SUCCEEDED(dev->CreatePixelShader(g_compiled.code[i].data(), reinterpret_cast<IDirect3DPixelShader9**>(&made[i])));
        }
        auto eng = std::make_unique<RtLampGpu::Engine>(GameCalls());
        std::string err;
        if (ok && !eng->Init(dev, made, RtLampGpu::kFaceDefault, err)) ok = false;
        for (int i = 0; i < RtLampGpu::kShaderCount; i++)
            if (made[i]) static_cast<IUnknown*>(made[i])->Release(); // (the engine holds its own reference)
        if (!ok) { // (the objects, not the code: made again 5 s later, up to 5 times; a device reset may have been under way)
            const std::string why = err.empty() ? "a shader object could not be made" : err;
            if (++g_makeFails >= 5) g_compiled.state.store(3), g_compiled.error = why;
            g_makeFailAt = GetTickCount();
            return off("its shaders could not be made (tried " + std::to_string(g_makeFails) + " of 5 times): " + why);
        }
        g_eng = std::move(eng);
        LOG_INFO("[RtOutdoorLamps] Shaders made: cube shadow maps 6 x 256^2 per lamp, 16 lamps an atlas");
    }
    // the active lot
    uintptr_t trackers[4];
    const int nt = LevelLightShare::ActiveLotTrackers(trackers, 4);
    if (nt <= 0) return off("no active lot (no lot lit at the high quality)");
    // (the active lot stays the one already active while it is still lit at the high quality: the list's order changes as
    // the camera moves, and each switch dropped and rebuilt every relit map)
    uintptr_t tracker = trackers[0];
    for (int i = 0; i < nt; i++)
        if (trackers[i] == g_tracker) tracker = g_tracker;
    if (tracker != g_tracker) {
        DropAll(dev, "another active lot");
        {
            std::lock_guard<std::mutex> lk(g_editMx);
            g_edits.clear(); // (the edits of the lot that was active)
        }
        g_tracker = tracker;
        g_askedSolve.clear();
        g_budgetOut = false;
        g_proxyWanted.store(tracker);
        LOG_INFO(std::format("[RtOutdoorLamps] Active lot tracker {:08X}{}", tracker, nt > 1 ? std::format(" (the first of {} lots lit at the high quality)", nt) : std::string()));
    }
    g_watch.store(true);
    // the occluders of the lot
    std::shared_ptr<const Proxy> proxy;
    std::string proxyWhy;
    {
        std::lock_guard<std::mutex> lk(g_proxyMx);
        proxy = g_proxy && g_proxy->tracker == tracker ? g_proxy : nullptr;
        proxyWhy = g_proxyWhy;
    }
    if (!proxy) {
        g_restartGpuPending = true;
        g_proxyWanted.store(tracker);
        g_on.store(false);
        Inactive("waiting for the lot's occluders (built on the light tree thread at its next room update or room-0 solve)" +
                 (proxyWhy.empty() ? std::string() : " - last attempt: " + proxyWhy));
        // a room 0 sent to its solve builds them at its end (once per room every 10 s)
        for (int S = 0; S <= 7; S++)
            if (const void* room = LevelLightShare::Room0Of(tracker, S)) {
                Ask& a = g_askedSolve[room];
                if (a.count < kSolveAsks && (!a.at || GetTickCount() - a.at > kSolveAskMs)) {
                    a.at = GetTickCount() | 1;
                    a.count++;
                    LevelLightShare::QueueRoomAgain(room);
                } else if (a.count >= kSolveAsks && !a.logged) {
                    a.logged = true;
                    LOG_INFO(std::format("[RtOutdoorLamps] Story {} room 0 sent to its solve {} times with no occluders read: no more sends (idle until the lot's next room update)", S, kSolveAsks));
                }
                RoomNow current;
                if (ReadRoomNow(static_cast<const BYTE*>(room), current) && RestartSolveTerminal(a, current.state, false)) {
                    g_restartGpuPending = false;
                    g_restartGpuFallback = true;
                }
                break;
            }
        return;
    }
    if (proxy != g_proxyUsed) {
        g_proxyUsed = proxy;
        RtLampProxy::Build(proxy->lot, g_mesh);
        std::string err;
        if (!g_eng->SetProxy(dev, g_mesh, err)) return off("the occluders could not be uploaded: " + err);
        g_meshGen = proxy->gen;
        g_atlasKey = 0;
        LOG_INFO(std::format("[RtOutdoorLamps] Occluders of lot {:08X}: {} triangles ({} walls with {} openings, {} roof and {} slab triangles), generation {}", tracker,
                             g_mesh.tris.size(), g_mesh.walls, g_mesh.openings, g_mesh.roofTris, g_mesh.slabTris, proxy->gen));
        // (left_lot_release) the triangles are in the proxy vertex buffer now (SetProxy copies them) and nothing else reads this
        // copy; the next Build starts from an empty mesh anyway (RtLampProxy::Build: m = Mesh{})
        if (DevSwitches::LeftLotRelease()) {
            const uint64_t bytes = ApexMemory::Est::Vec(g_mesh.tris) + ApexMemory::Est::Vec(g_mesh.cellStart) + ApexMemory::Est::Vec(g_mesh.cellTris) +
                                   ApexMemory::Est::Vec(g_mesh.kind) + ApexMemory::Est::Vec(g_mesh.story);
            g_mesh = RtLampProxy::Mesh{};
            ApexMemory::NoteReleased("Real-time outdoor lamps: the occluders' CPU copy once uploaded", bytes);
        }
    }
    // the rules every relit texel follows (a change relights every map)
    const GameConsts gc = ReadConsts();
    const bool reachOn = LevelLightShare::RealisticReachActive();
    const bool fade = LevelLightShare::SoftLightCutOn();
    const float radius = Penumbra::Radius();
    const bool coneSpill = LevelLightShare::BakeConeSpillOn();
    uint64_t params = 1469598103934665603ull;
    Mix(params, &gc, sizeof gc), Mix(params, &reachOn, 1), Mix(params, &fade, 1), Mix(params, &radius, 4), Mix(params, &coneSpill, 1);
    const bool paramsChanged = params != g_lastParams;
    g_lastParams = params;
    // ---- each story's room 0: its maps of the class it shows, from its kept solve of that class ----
    LARGE_INTEGER tp0{};
    QueryPerformanceCounter(&tp0);
    {
        // the active lot's room 0s are the last the memory budget of the kept parts drops
        const void* pin[8];
        int np = 0;
        for (int S = 0; S <= 7; S++)
            if (const void* r = LevelLightShare::Room0Of(tracker, S)) pin[np++] = r;
        const long drops = OutdoorDelta::PinRooms(pin, np);
        if (drops != g_pinDrops) {
            g_pinDrops = drops;
            if (!g_budgetOut)
                LOG_INFO(std::format("[RtOutdoorLamps] The active lot's kept room-0 parts do not fit the memory budget ({:.1f} MB kept): a story whose solve "
                                     "could not be kept is no longer sent to its solve (the kept stories stay); its lamp edits go through the game's solve",
                                     static_cast<double>(OutdoorDelta::KeptBytes()) / (1 << 20)));
            g_budgetOut = true;
        }
    }
    std::vector<std::string> waiting;
    bool allReady = true;
    int uploads = 0;
    // (14/10, drag solves) while a lamp is dragged no slot is uploaded (a slot upload makes its buffers and textures on this
    // thread: 12-14 ms in a recording); the old copies stay bound and the uploads go once the lamp is still
    const bool dragNow = g_lampChangedAt && GetTickCount() - g_lampChangedAt < kDragQuietMs;
    int stories = 0;
    const void* rooms[8] = {};
    RoomNow rns[8];
    for (int S = 0; S <= 7; S++) {
        const BYTE* room = static_cast<const BYTE*>(LevelLightShare::Room0Of(tracker, S));
        if (!room) continue;
        RoomNow rn;
        if (!ReadRoomNow(room, rn) || rn.id != 0 || !rn.outdoor) continue;
        stories++;
        rooms[S] = room;
        rns[S] = rn;
        g_lotId = rn.lot;
        const int cls = rn.shown;
        const uint32_t ver = cls >= 0 && cls <= 2 ? OutdoorDelta::RecordVersion(room, cls) : 0;
        if (ver) {
            auto at = g_askedSolve.find(room);
            if (at != g_askedSolve.end()) g_askedSolve.erase(at); // (kept: its sends start again if it loses it)
        }
        if (!ver) {
            // its kept solve is being renewed (a solve of room 0 drops it until its end): its maps relit from the last one stay
            // bound and count while every one of them is relit
            bool had = false, all = true;
            for (const auto& sl : g_slots)
                if (sl->story == S && sl->room == room && sl->cls == cls && sl->used) had = true, all = all && sl->ok && sl->relit;
            if (!(had && all)) {
                allReady = false;
                const std::string last = OutdoorDelta::LastDrop(S, cls);
                waiting.push_back(std::format("story {}: no kept solve of class {}{}", S, cls, last.empty() ? std::string() : " - last: " + last));
            }
            // idle: sent to its solve, which keeps its parts (a few times; never while the lot's parts overflow the budget, which
            // would drop another story's parts in turn)
            Ask& a = g_askedSolve[room];
            if ((rn.state == 4 || rn.state == 5) && !g_budgetOut && a.count < kSolveAsks && (!a.at || GetTickCount() - a.at > kSolveAskMs)) {
                a.at = GetTickCount() | 1;
                a.count++;
                const bool sent = LevelLightShare::QueueRoomAgain(room);
                LOG_INFO(std::format("[RtOutdoorLamps] Story {} room 0 shows class {} with no kept solve: {} ({} of {})", S, cls,
                                     sent ? "sent to its solve" : "could not be sent to its solve", a.count, kSolveAsks));
            } else if ((a.count >= kSolveAsks || g_budgetOut) && !a.logged) {
                a.logged = true;
                LOG_INFO(std::format("[RtOutdoorLamps] Story {} room 0 class {}: no kept solve and no more sends to its solve ({}); its maps stay the game's and lamp "
                                     "edits go through the game's solve until it keeps one",
                                     S, cls, g_budgetOut ? "the lot's kept parts overflow the memory budget" : std::format("sent {} times", kSolveAsks)));
            }
            if (!(had && all)) {
                if (RestartSolveTerminal(a, rn.state, g_budgetOut)) g_restartGpuFallback = true;
                else g_restartGpuPending = true;
            }
            continue;
        }
        // the slots of this story: built again when its kept base or its class changed (prepared on a worker thread, then
        // uploaded a map a frame; the old copies stay bound meanwhile)
        bool needs[4] = {}, need = false, covered = true;
        for (int m = 0; m < 4; m++) {
            Slot* slot = nullptr;
            for (auto& sl : g_slots)
                if (sl->story == S && sl->m == m) slot = sl.get();
            if (slot && (slot->room != room || slot->cls != cls)) { // another class on screen, or the room was made again
                ReleaseSlot(*slot);
                slot->built = false, slot->relit = false, slot->room = room, slot->cls = cls;
                slot->retries = 0, slot->retryAt = 0; // (14/10 review: the backoff was the old room's or class's)
            }
            uintptr_t setAt = 0;
            if (slot && !slot->built && slot->retryAt && static_cast<int32_t>(GetTickCount() - slot->retryAt) < 0) { // (an upload's backoff)
                covered = false;
                g_restartGpuPending = true;
                continue;
            }
            needs[m] = !(slot && slot->built && slot->version == ver && (!slot->used || OwnMap(room, cls, kSetIdx[m], setAt) == slot->game));
            need = need || needs[m];
            if (needs[m] && !(slot && (!slot->used || (slot->relit && slot->ok)))) covered = false; // (the old copy stays bound meanwhile)
        }
        auto jit = std::find_if(g_jobs.begin(), g_jobs.end(), [room](const std::shared_ptr<Job>& j) { return j->room == room; });
        if (!need) {
            if (jit != g_jobs.end() && (*jit)->state.load() != 1) g_jobs.erase(jit);
            continue;
        }
        g_restartGpuPending = true; // includes preparation/export backoff without an in-flight job
        if (jit != g_jobs.end() && (*jit)->state.load() != 1 && ((*jit)->cls != cls || (*jit)->ver != ver)) { // (stale: the base changed again)
            g_jobs.erase(jit);
            jit = g_jobs.end();
        }
        if (jit == g_jobs.end()) {
            const bool running = std::any_of(g_jobs.begin(), g_jobs.end(), [](const std::shared_ptr<Job>& j) { return j->state.load() == 1; });
            const auto back = g_exportBackoff.find(room); // (14/10: a failed export waits before the next one)
            const bool waitExport = back != g_exportBackoff.end() && static_cast<int32_t>(GetTickCount() - back->second.at) < 0;
            if (!running && !waitExport) {
                auto j = std::make_shared<Job>();
                j->room = room, j->story = S, j->cls = cls, j->ver = ver, j->rn = rn;
                j->bake = g_bakeLast[S], j->bakeKeys = g_bakeKeysLast[S], j->base = rn.lotBase;
                g_jobs.push_back(j);
                StartJob(j);
                g_why.build++;
            }
            if (!covered) allReady = false, waiting.push_back(std::format("story {}: its maps are being prepared", S));
            continue;
        }
        Job& job = **jit;
        const int st = job.state.load();
        if (st == 1) {
            if (!covered) allReady = false, waiting.push_back(std::format("story {}: its maps are being prepared", S));
            continue;
        }
        if (st == 3) {
            g_jobs.erase(jit);
            allReady = false;
            waiting.push_back(std::format("story {}: its kept solve could not be read", S));
            ExportBackoff& b = g_exportBackoff[room];
            b.fails = std::min(b.fails + 1, 6);
            b.at = (GetTickCount() + (250u << (b.fails - 1))) | 1; // 0.25 s ... 8 s
            if (b.fails == 1 || b.fails == 6) LOG_INFO(std::format("[RtOutdoorLamps] Story {}: its kept solve could not be read ({} time{}); tried again later", S, b.fails, b.fails == 1 ? "" : "s"));
            continue;
        }
        g_exportBackoff.erase(room);
        g_buildMs += job.ms;
        job.ms = 0;
        bool done = true;
        for (int m = 0; m < 4; m++) {
            if (!needs[m]) continue;
            if (uploads >= 1 || dragNow) {
                done = false;
                break;
            }
            Slot* slot = nullptr;
            for (auto& sl : g_slots)
                if (sl->story == S && sl->m == m) slot = sl.get();
            Prepared& p = job.maps[m];
            if (!p.used) { // (no texel of room 0 in this map: nothing to relight there; a slot notes it, so it is not prepared again)
                if (!slot) {
                    g_slots.push_back(std::make_unique<Slot>());
                    slot = g_slots.back().get();
                    slot->story = S, slot->m = m, slot->cls = cls, slot->room = room;
                }
                ReleaseSlot(*slot);
                slot->built = true, slot->used = false, slot->version = job.version, slot->ok = false, slot->relit = false;
                continue;
            }
            if (!slot) {
                g_slots.push_back(std::make_unique<Slot>());
                slot = g_slots.back().get();
                slot->story = S, slot->m = m, slot->cls = cls, slot->room = room;
            }
            slot->used = true;
            uintptr_t setAt = 0;
            IDirect3DTexture9* tex = OwnMap(room, cls, kSetIdx[m], setAt);
            if (tex != slot->game) {
                if (slot->game) slot->game->Release();
                slot->game = tex;
                if (tex) tex->AddRef();
                if (slot->copy) slot->copy->Release(), slot->copy = nullptr;
                if (slot->scale) slot->scale->Release(), slot->scale = nullptr;
                slot->scaled = false;
                slot->relit = false;
            }
            if (!slot->logged) {
                slot->logged = true;
                LOG_INFO(std::format("[RtOutdoorLamps] Story {} class {} {}: the room's own map {:08X} (texture set at {:08X}, manager +0x{:X}), {}x{}", S, cls, kMapName[m],
                                     reinterpret_cast<uintptr_t>(tex), setAt, setAt - rn.mgr, p.w, p.h));
            }
            uploads++;
            if (!UploadSlot(dev, *slot, job, p)) {
                allReady = false;
                LOG_INFO(std::format("[RtOutdoorLamps] Story {} class {} {}: not relit ({})", S, cls, kMapName[m], slot->why));
            }
            // watched for writes
            if (slot->watch < 0)
                for (int w = 0; w < kMaxWatch; w++)
                    if (!g_watchTex[w].load()) {
                        slot->watch = w;
                        break;
                    }
            if (slot->watch >= 0) g_watchTex[slot->watch].store(slot->game);
        }
        if (done) g_jobs.erase(std::find_if(g_jobs.begin(), g_jobs.end(), [room](const std::shared_ptr<Job>& j) { return j->room == room; }));
        else if (!covered) allReady = false, waiting.push_back(std::format("story {}: its maps are being uploaded", S));
    }
    // slots of stories gone
    for (auto it = g_slots.begin(); it != g_slots.end();)
        if (!rooms[(*it)->story] || (*it)->room != rooms[(*it)->story]) {
            ReleaseSlot(**it);
            it = g_slots.erase(it);
        } else
            ++it;
    LARGE_INTEGER tp1{};
    QueryPerformanceCounter(&tp1);
    const double slotsMs = Ms(tp0, tp1);
    // writes noted since the last frame (each map keeps its own until it is relit)
    const uint64_t dirtyBits = g_watchDirty.exchange(0);
    std::vector<std::pair<uintptr_t, int>> solved;
    {
        std::lock_guard<std::mutex> lk(g_noteMx);
        solved.swap(g_solvedNotes);
    }
    // ---- the lamps of the lot now: per story's list, the ones the GPU draws ----
    std::vector<GpuLamp> lamps;
    std::vector<RtLampModel::BakeLamp> bake[8];
    std::vector<uintptr_t> bakeLights[8];
    uintptr_t list[1024];
    uint64_t bakeSig[8] = {};
    uint64_t allSig = 1469598103934665603ull;
    // (14/10) a list that cannot be read this frame: the story keeps the last one read (its lamps were dropped for one frame,
    // its copy relit without them and again the next frame: a flicker); only for a few frames in a row, while the list's
    // begin is set, and for the same tracker, manager and room (14/10 review)
    for (int S = 0; S <= 7; S++) {
        if (!rooms[S]) continue;
        int n = ReadList(static_cast<const BYTE*>(rooms[S]), list, 1024);
        KeptList& kl = g_keptList[S];
        uintptr_t mgr = 0, begin = 0;
        const bool heads = ListHeads(static_cast<const BYTE*>(rooms[S]), mgr, begin);
        if (n >= 0) {
            kl.list.assign(list, list + n);
            kl.tracker = tracker, kl.mgr = heads ? mgr : 0, kl.room = rooms[S], kl.misses = 0;
        } else if (heads && begin && kl.tracker == tracker && kl.mgr == mgr && kl.room == rooms[S] && kl.misses < kKeptListFrames && !kl.list.empty()) {
            kl.misses++;
            n = static_cast<int>(kl.list.size());
            std::copy(kl.list.begin(), kl.list.end(), list);
            c_listKept.fetch_add(1, std::memory_order_relaxed);
        } else
            kl = KeptList{};
        if (n < 0) continue;
        for (int i = 0; i < n; i++) {
            const uintptr_t L = list[i];
            LampRead r;
            if (!L || !ReadLamp(L, r)) continue;
            if (InTerrainBake(r)) {
                RtLampModel::BakeLamp b;
                for (int k = 0; k < 3; k++) b.head[k] = r.pos[k];
                b.range = r.range, b.intensity = r.intensity;
                const bool spill = coneSpill && r.f.type == 5;
                for (int k = 0; k < 3; k++) b.colour += std::max(r.f0[k], 0.0f) * (spill ? std::clamp(r.S[k], 0.0f, 1.0f) : 1.0f);
                b.colour *= r.areaShare;
                b.liftable = TerrainBakeRule::Liftable(r.f.lot != 0, r.f.type) ? 1u : 0u;
                bake[S].push_back(b);
                bakeLights[S].push_back(L);
            }
            if (!RtLampModel::Drawn(r.f, rns[S].lot)) {
                // a lamp of the lamp classes, outdoors, of another lot id than the story's manager carries: noted once a lot
                if ((r.f.type >= 3 && r.f.type <= 5) && r.f.lot && r.f.room == 0 && r.f.lot != rns[S].lot && g_lotNoted.insert(rns[S].lot).second)
                    LOG_INFO(std::format("[RtOutdoorLamps] Story {} room 0 lists an outdoor lamp of lot {:016X} while its manager carries lot {:016X}: such lamps stay baked",
                                         S, r.f.lot, rns[S].lot));
                continue;
            }
            auto it = std::find_if(lamps.begin(), lamps.end(), [L](const GpuLamp& g) { return g.light == L; });
            if (it == lamps.end()) {
                GpuLamp g{};
                g.light = L;
                ToLaw(r, gc, rns[S].xf, reachOn, g.lamp.law);
                g.esum = std::max(r.colour[0], 0.0f) + std::max(r.colour[1], 0.0f) + std::max(r.colour[2], 0.0f);
                g.spill = r.f.type == 5 ? std::max({1.0f, r.S[0], r.S[1], r.S[2]}) : 1.0f;
                g.liftable = TerrainBakeRule::Liftable(r.f.lot != 0, r.f.type); // a lot's street-lamp class (11) is not
                lamps.push_back(g);
                it = lamps.end() - 1;
            }
            it->mult[S]++;
            Mix(allSig, &L, sizeof L);
            Mix(allSig, &it->lamp.law, sizeof it->lamp.law);
        }
        uint64_t h = 1469598103934665603ull;
        for (const auto& b : bake[S]) Mix(h, &b, sizeof b);
        for (uintptr_t L : bakeLights[S]) Mix(h, &L, sizeof L);
        bakeSig[S] = h;
    }
    std::vector<uint64_t> bakeKeys[8];
    for (int S = 0; S <= 7; S++) {
        if (!rooms[S]) continue;
        if (UnderSig(bakeSig[S], rns[S].lotBase) != UnderSig(g_bakeSigLast[S], g_lotBaseLast[S]) || g_bakeKeysLast[S].size() != bake[S].size()) {
            bakeKeys[S] = BakeKeys(bakeLights[S]);
            g_bakeLast[S] = bake[S], g_bakeKeysLast[S] = bakeKeys[S], g_lotBaseLast[S] = rns[S].lotBase, g_bakeSigLast[S] = bakeSig[S];
        } else
            bakeKeys[S] = g_bakeKeysLast[S];
    }
    // the stories each GPU lamp is drawn on: a change is logged (a switch, a lamp leaving or joining a story's list)
    {
        static std::unordered_map<uintptr_t, uint32_t> s_drawn;
        std::unordered_map<uintptr_t, uint32_t> now;
        for (const GpuLamp& g : lamps) {
            uint32_t m = 0;
            for (int S = 0; S <= 7; S++)
                if (g.mult[S]) m |= 1u << S;
            now[g.light] = m;
        }
        const auto stories = [](uint32_t m) {
            std::string s;
            for (int S = 0; S <= 7; S++)
                if (m >> S & 1) s += std::format("{}{}", s.empty() ? "" : " ", S);
            return s.empty() ? std::string("none") : s;
        };
        const uintptr_t sw = g_switchOn.exchange(0);
        for (const auto& [L, m] : now) {
            const auto it = s_drawn.find(L);
            const uint32_t was = it == s_drawn.end() ? 0 : it->second;
            if ((was != m || L == sw) && !s_drawn.empty() && LogAllowed())
                LOG_INFO(std::format("[RtOutdoorLamps] GPU lamp {:08X}{}: drawn on stories {} -> {}", L, L == sw ? " switched on (its stories gather it again)" : "",
                                     stories(was), stories(m)));
        }
        for (const auto& [L, m] : s_drawn)
            if (!now.count(L) && LogAllowed()) LOG_INFO(std::format("[RtOutdoorLamps] GPU lamp {:08X}: drawn on stories {} -> none (switched off, or out of the lists)", L, stories(m)));
        if (sw && !now.count(sw) && LogAllowed())
            LOG_INFO(std::format("[RtOutdoorLamps] GPU lamp {:08X} switched on: not in any story's list yet (its stories gather it again)", sw));
        s_drawn.swap(now);
    }
    // ---- consistency (every 2 s): a GPU lamp that is on and was drawn on a story must still be in that story's room-0 list.
    // The game's gather leaves a switched-off lamp out of a room's list; when it is on again, a story that gathered while it
    // was off no longer has it. Such a story's room 0 is sent to gather again (once; a second miss is logged and left).
    {
        struct Seen {
            uint32_t mask = 0;     // the stories it was drawn on
            uint32_t sent = 0;     // the stories sent to gather it (not again until it is drawn there)
            uint32_t missed = 0;   // the stories it was missing from at the last check (14/10: a repair needs two checks in a row)
            DWORD sentAt = 0;
            bool given = false;    // a second miss logged
            // what it was when first seen (14/10: another light at a freed lamp's address starts again; a lamp moved to another
            // room or place starts its stories again)
            int type = -1, room = -1;
            uint64_t lot = 0;
            float pos[3] = {};
        };
        static std::unordered_map<uintptr_t, Seen> s_seen;
        static DWORD s_checkAt = 0;
        static uintptr_t s_tracker = 0;
        if (s_tracker != tracker) s_seen.clear(), s_tracker = tracker;
        const DWORD checkNow = GetTickCount();
        std::unordered_map<uintptr_t, uint32_t> nowMask;
        for (const GpuLamp& g : lamps) {
            uint32_t m = 0;
            for (int S = 0; S <= 7; S++)
                if (g.mult[S]) m |= 1u << S;
            nowMask[g.light] = m;
            Seen& e = s_seen[g.light];
            LampRead gr;
            if (ReadLamp(g.light, gr)) {
                const bool moved = e.type >= 0 && (gr.f.room != e.room || std::fabs(gr.pos[0] - e.pos[0]) + std::fabs(gr.pos[1] - e.pos[1]) + std::fabs(gr.pos[2] - e.pos[2]) > 0.01f);
                if (e.type >= 0 && (gr.f.type != e.type || gr.f.lot != e.lot)) e = Seen{}; // another light at that address
                else if (moved) e.mask = 0, e.missed = 0;
                e.type = gr.f.type, e.lot = gr.f.lot, e.room = gr.f.room;
                std::memcpy(e.pos, gr.pos, sizeof e.pos);
            }
            e.mask |= m;
            e.sent &= ~m; // (drawn there again)
            if (!e.sent) e.given = false;
        }
        if (checkNow - s_checkAt >= kCheckEveryMs) {
            s_checkAt = checkNow;
            if (s_seen.size() > 512) s_seen.clear();
            for (auto it = s_seen.begin(); it != s_seen.end();) {
                const uintptr_t L = it->first;
                Seen& e = it->second;
                LampRead r;
                // gone (deleted: alive 0x01 or enabled 0x40 clear; unreadable): forgotten
                if (!ReadLamp(L, r) || !(r.f.flags & 0x01) || !(r.f.flags & 0x40)) {
                    it = s_seen.erase(it);
                    continue;
                }
                const bool on = (r.f.flags & 0x20) && r.colour[0] + r.colour[1] + r.colour[2] > 1e-3f;
                const auto nm = nowMask.find(L);
                const uint32_t m = nm == nowMask.end() ? 0 : nm->second;
                // (14/10) only the stories where it is still a lamp the GPU draws (one that left them legitimately, e.g. moved
                // indoors, is not missing), and only when missing at two checks in a row (a list read mid-gather)
                uint32_t drawable = 0;
                for (int S = 0; S <= 7; S++)
                    if (rooms[S] && RtLampModel::Drawn(r.f, rns[S].lot)) drawable |= 1u << S;
                // a lamp listed on some stories is expected on every story where it is drawable (a new or moved lamp the game
                // has listed on its own story only: the other stories' room 0s never gathered it)
                const uint32_t expect = m ? (e.mask | drawable) : e.mask;
                const uint32_t missNow = on ? (expect & ~m & drawable) : 0;
                const uint32_t missing = missNow & e.missed;
                e.missed = missNow;
                if (missing) {
                    std::string list;
                    for (int S = 0; S <= 7; S++)
                        if (missing >> S & 1) list += std::format("{}{}", list.empty() ? "" : " ", S);
                    const uint32_t fresh = missing & ~e.sent;
                    if (fresh) {
                        int sent = 0;
                        for (int S = 0; S <= 7; S++)
                            if ((fresh >> S & 1) && rooms[S]) sent += LevelLightShare::QueueRoom0(tracker, S) ? 1 : 0;
                        e.sent |= fresh;
                        e.sentAt = checkNow;
                        c_repairs.fetch_add(sent);
                        LOG_INFO(std::format("[RtOutdoorLamps] Consistency: GPU lamp {:08X} is on but missing from stories {} where it is drawable: their room 0s "
                                             "gather again ({} sent)", L, list, sent));
                    } else if (!e.given && checkNow - e.sentAt >= kRepairWaitMs) {
                        e.given = true;
                        LOG_INFO(std::format("[RtOutdoorLamps] Consistency: GPU lamp {:08X} still not listed on stories {} after their room 0s gathered again (the "
                                             "gather leaves it out there)", L, list));
                    }
                }
                ++it;
            }
        }
    }
    // a GPU lamp that changed in the last kDragQuietMs is being dragged (or edited again and again)
    const DWORD nowTick = GetTickCount();
    if (allSig != g_lampSigAll) {
        if (g_lampSigAll) {
            g_lampChangedAt = nowTick | 1;
            // what changed (at most a line every 2 s): an edit, or the lamp by itself (an animated lamp, a fade)
            if (nowTick - g_lampLogAt >= 2000) {
                std::string what;
                for (const GpuLamp& g : lamps) {
                    LampRead r;
                    if (!ReadLamp(g.light, r)) continue;
                    const auto it = g_lampSnap.find(g.light);
                    if (it == g_lampSnap.end()) {
                        what += std::format("{}{:08X} new", what.empty() ? "" : "; ", g.light);
                        continue;
                    }
                    const LampSnap& o = it->second;
                    std::string f;
                    const auto d3 = [](const float* a, const float* b) { return std::fabs(a[0] - b[0]) + std::fabs(a[1] - b[1]) + std::fabs(a[2] - b[2]) > 1e-6f; };
                    if (d3(o.pos, r.pos)) f += std::format(" position ({:.2f} {:.2f} {:.2f} -> {:.2f} {:.2f} {:.2f})", o.pos[0], o.pos[1], o.pos[2], r.pos[0], r.pos[1], r.pos[2]);
                    if (d3(o.colour, r.colour)) f += std::format(" colour ({:.4f} {:.4f} {:.4f} -> {:.4f} {:.4f} {:.4f})", o.colour[0], o.colour[1], o.colour[2], r.colour[0], r.colour[1], r.colour[2]);
                    if (o.range != r.range) f += std::format(" range ({:.3f} -> {:.3f})", o.range, r.range);
                    if (o.intensity != r.intensity) f += std::format(" intensity ({:.4f} -> {:.4f})", o.intensity, r.intensity);
                    if (o.flags != r.f.flags) f += std::format(" flags ({:02X} -> {:02X})", o.flags, r.f.flags);
                    if (!f.empty()) what += std::format("{}{:08X} type {}:{}", what.empty() ? "" : "; ", g.light, r.f.type, f);
                }
                if (what.empty()) what = "a lamp left the lists, or its law (cones, reach, options) changed";
                const unsigned long edit = LampMarkFilter::EditLastTick();
                g_lampLogAt = nowTick;
                LOG_INFO(std::format("[RtOutdoorLamps] GPU lamp changed ({}): {}", edit && nowTick - edit < 1500 ? "a lamp edit" : "no lamp edit in the last 1.5 s", what));
            }
        }
        g_lampSigAll = allSig;
        g_lampSnap.clear();
        for (const GpuLamp& g : lamps) {
            LampRead r;
            if (!ReadLamp(g.light, r)) continue;
            LampSnap sn{};
            std::memcpy(sn.pos, r.pos, sizeof sn.pos), std::memcpy(sn.colour, r.colour, sizeof sn.colour);
            sn.range = r.range, sn.intensity = r.intensity, sn.type = r.f.type, sn.flags = r.f.flags;
            g_lampSnap[g.light] = sn;
        }
    }
    const bool dragging = g_lampChangedAt && nowTick - g_lampChangedAt < kDragQuietMs;
    LARGE_INTEGER tp2{};
    QueryPerformanceCounter(&tp2);
    const double lampsMs = Ms(tp1, tp2);
    if (g_slots.empty() || !allReady) {
        g_on.store(!g_slots.empty());
        Inactive(waiting.empty() ? (g_slots.empty() ? "no room-0 map of the active lot could be relit (see the lines above)" : "")
                                 : "waiting for kept room-0 solves: " + waiting.front() + (waiting.size() > 1 ? std::format(" (+{} more)", waiting.size() - 1) : ""));
        if (g_slots.empty()) return;
    } else
        Inactive("");
    g_on.store(true);
    // ---- relight what changed ----
    LARGE_INTEGER t0{};
    QueryPerformanceCounter(&t0);
    int relit = 0, tilesBefore = static_cast<int>(g_eng->GetStats().shadowTiles), storiesRelit = 0, left = 0;
    std::vector<int> storyList;
    bool begun = false, timing = false;
    // (an exception between Begin and End still restores the device state Begin saved)
    struct EndOnThrow {
        IDirect3DDevice9* dev;
        bool& begun;
        ~EndOnThrow() {
            try {
                if (begun && g_eng) g_eng->End(dev);
            } catch (...) {
            }
        }
    } endOnThrow{dev, begun};
    std::string failNote;
    // while a lamp is dragged: a relight starts at most every kDragRelightMs, with the cheaper shadows; every frame's work
    // stops past kFrameCpuMs or kFrameWork (the maps left are relit the next frames)
    const bool throttled = !g_roundOpen && dragging && g_lastRelightAt && nowTick - g_lastRelightAt < kDragRelightMs;
    double work = 0, maskMs = 0, drawMs = 0;
    const size_t nSlots = g_slots.size();
    if (g_rr >= nSlots) g_rr = 0;
    for (size_t si = 0; si < nSlots; si++) {
        Slot& s = *g_slots[(g_rr + si) % nSlots];
        if (!s.ok || !s.game) continue;
        // its GPU lamps: drawn now, in its story's list, and in its kept solve only as lamps whose parts left the base
        std::vector<RtLampModel::Lamp> mine;
        std::vector<const GpuLamp*> mineRef;
        for (const GpuLamp& g : lamps) {
            if (!g.mult[s.story]) continue;
            if (s.kept.count(g.light) && !s.drawn.count(g.light)) continue; // baked in its base
            // within its reach of the texels (the lamp law's distance where it falls under the threshold)
            const float b2 = OutdoorDeltaModel::ReachBound2(g.lamp.law.W, g.esum, g.spill, s.thr);
            if (b2 == 0.0f) continue;
            if (b2 > 0.0f) {
                const float dx = g.lamp.law.pos[0] - s.c[0], dy = g.lamp.law.pos[1] - s.c[1], dz = g.lamp.law.pos[2] - s.c[2];
                const float d = std::sqrt(dx * dx + dy * dy + dz * dz) - s.rad;
                if (d > 0.0f && d * d > b2) continue;
            }
            RtLampModel::Lamp l = g.lamp;
            l.mult = static_cast<float>(g.mult[s.story]);
            if (s.story == 0 && s.m == 1 && g.liftable && TerrainBakeRule::LowLampLift()) {
                // the lot's ground in lot space (its base is world y; lots turn about y only)
                const float base[3] = {0.0f, rns[0].lotBase, 0.0f};
                float baseLot[3];
                ToLot(rns[0].xf, base, baseLot);
                const float least = baseLot[1] + TerrainBakeRule::kLowLampLift;
                if (std::isfinite(least) && l.law.pos[1] < least && l.law.pos[1] > baseLot[1] - TerrainBakeRule::kLowLampBelow) {
                    l.law.pos[1] = least;
                    g_lowLampLifted.fetch_add(1, std::memory_order_relaxed);
                }
            }
            mine.push_back(l);
            mineRef.push_back(&g);
        }
        // what it is relit with: its lamps within reach (a lamp out of its reach changes nothing in it), the bake's lamps for its
        // floor mask, the rules, the occluders, its kept solve
        uint64_t sig = 1469598103934665603ull;
        Mix(sig, &params, 8), Mix(sig, &g_meshGen, 4), Mix(sig, &s.version, 4);
        if (!s.masked.empty()) Mix(sig, &bakeSig[s.story], 8);
        for (const auto& l : mine) Mix(sig, &l, sizeof l);
        // the under-floor share from a worker (exact, UnderShare's sums): once no lamp is dragged, and (14/10) while one is,
        // in place of the render thread's UnderDelta below (up to 152 ms of a drag's relight; 26.8 ms in one frame): the last
        // share is kept until the worker's arrives, and a share of an earlier bake is taken during a drag (it follows the drag
        // one worker run behind; exact again once the drag ends)
        const auto startUnderJob = [&] {
            auto j = std::make_shared<UnderJob>();
            j->pos = s.maskedPos, j->gated = s.gated, j->bake = bake[s.story], j->ids = bakeKeys[s.story], j->base = rns[s.story].lotBase;
            j->sig = UnderSig(bakeSig[s.story], j->base);
            s.uJob = j;
            if (!HookGuard::StartDetached("RtOutdoorLamps under-floor job", [j] {
                    try {
                        FullUnder(j->pos, j->gated, j->bake, j->base, j->under, j->A, j->U);
                    } catch (...) {
                        j->under.clear();
                    }
                    j->state.store(2);
                })) {
                j->under.clear(); // (no thread: the job ends empty, as after an exception)
                j->state.store(2);
            }
        };
        if (!s.masked.empty() && s.uJob && s.uJob->state.load() == 2) {
            const auto j = std::move(s.uJob);
            const bool current = j->sig == UnderSig(bakeSig[s.story], rns[s.story].lotBase);
            if ((current || (dragging && j->sig != s.underSig)) && j->under.size() == s.masked.size()) {
                s.under = j->under, s.uA = j->A, s.uU = j->U, s.uSnap = j->bake, s.uIds = j->ids, s.uBase = j->base;
                s.underSig = j->sig, s.underExact = current, s.underDirty = true, s.dynCurrent = false;
            }
        }
        if (!s.masked.empty() && !s.underExact && !dragging && !s.uJob) startUnderJob();
        const bool wrote = s.watch >= 0 && (dirtyBits >> s.watch & 1);
        bool solvedHere = false;
        for (const auto& [mgr, cls] : solved)
            if (rns[s.story].mgr == mgr && cls == s.cls) solvedHere = true;
        if (solvedHere) s.pendingWrite = true;
        if (wrote) s.pendingWrite = true;
        const bool exactUnder = s.underDirty;
        const bool dirty = !s.relit || !s.copy || sig != s.sig || paramsChanged || (s.fast && !dragging) || s.pendingWrite || s.underDirty;
        if (!dirty) continue;
        // (2.15.1) the game writing the map again and again (a lot that stays loading: 105 writes in 10 s, each relight
        // 45 ms of floor mask with 28 lamps): a relight made only for a game write waits kGameWriteGapMs after the last one; a
        // single write after a rest is relit at once, as before
        const bool onlyWrite = s.relit && s.copy && sig == s.sig && !paramsChanged && !(s.fast && !dragging) && !s.underDirty;
        if (onlyWrite && GetTickCount64() - s.relitTick < kGameWriteGapMs) {
            // (it keeps its last copy and stays dirty until its turn; not counted in `left`: a lot whose game keeps writing
            // would keep the round open, and with it the drag throttle off)
            g_why.writeGap++;
            continue;
        }
        uint64_t lampsSig = 1469598103934665603ull;
        for (const auto& l : mine) Mix(lampsSig, &l, sizeof l);
        if (!s.masked.empty()) Mix(lampsSig, &bakeSig[s.story], 8);
        // the budget: the first map of a frame always goes; later ones while there is time and work left
        LARGE_INTEGER tn{};
        QueryPerformanceCounter(&tn);
        const double units = static_cast<double>(s.map.count) * static_cast<double>(std::max<size_t>(mine.size(), 1)) * (dragging ? 1.0 : 7.0);
        if (throttled || (relit > 0 && (Ms(t0, tn) > kFrameCpuMs || work + units > kFrameWork))) {
            left++; // (it keeps its last copy and stays dirty until its turn)
            continue;
        }
        work += units;
        if (!begun) {
            if (!g_qDisjoint && SUCCEEDED(dev->CreateQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT, nullptr))) {
                dev->CreateQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT, &g_qDisjoint), dev->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ, &g_qFreq);
                dev->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &g_qBegin), dev->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &g_qEnd);
            }
            timing = !g_qPending && g_qDisjoint && g_qFreq && g_qBegin && g_qEnd;
            if (timing) g_qDisjoint->Issue(D3DISSUE_BEGIN), g_qBegin->Issue(D3DISSUE_END);
            g_eng->Begin(dev);
            begun = true;
        }
        if (!s.copy &&
            (FAILED(dev->CreateTexture(s.map.w, s.map.h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &s.copy, nullptr)) || !s.copy)) {
            s.copy = nullptr;
            failNote = std::format("story {} {}: its copy could not be made", s.story, kMapName[s.m]);
            continue;
        }
        // the outdoor scale beside it (a device with two render targets; a map the draws multiply back: no blur). None: the
        // draws read the copy as before, the light past 1 cut
        if (!s.scale && !s.map.blur && g_mrt > 0 &&
            (FAILED(dev->CreateTexture(s.map.w, s.map.h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &s.scale, nullptr)) || !s.scale))
            s.scale = nullptr;
        if (!s.preblur && s.map.blur) { // after a device reset
            s.built = false;
            continue;
        }
        // the floor mask: the GPU lamps' luminance with no walls and the share of the bake under the floor (CPU; the share is
        // kept while the bake's lamps stay; only the masked texels are written)
        LARGE_INTEGER tm0{}, tm1{};
        QueryPerformanceCounter(&tm0);
        bool maskWait = false; // (a drag: the share's CPU work waits for the worker; this map keeps its last copy meanwhile)
        // one batch of lamps (the usual case): "all" is evaluated by the compose shader, and the buffer is written only when the
        // under-floor share changed; more lamps: "all" from the CPU every relight
        const bool maskGpu = mine.size() <= static_cast<size_t>(RtLampGpu::kAtlasLamps);
        if (s.dyn && !s.masked.empty()) {
            const uint64_t us = UnderSig(bakeSig[s.story], rns[s.story].lotBase);
            bool write = !maskGpu || !s.dynCurrent;
            if (s.underSig != us) {
                if (dragging && (s.under.size() != s.masked.size() || s.uA.size() != s.masked.size() || s.uBase != rns[s.story].lotBase)) {
                    // (14/10, drag solves) never the whole share on this thread while a lamp is dragged (10-30 ms a relight in a
                    // recording): the worker computes it, this map waits for it with its last copy bound
                    if (!s.uJob) startUnderJob();
                    maskWait = true;
                } else if (s.under.size() != s.masked.size() || s.uA.size() != s.masked.size() || s.uBase != rns[s.story].lotBase) {
                    // (no sums to start from: the whole share, once; a slot from the worker always has them)
                    s.uBase = rns[s.story].lotBase;
                    FullUnder(s.maskedPos, s.gated, bake[s.story], s.uBase, s.under, s.uA, s.uU);
                    s.uSnap = bake[s.story], s.uIds = bakeKeys[s.story];
                    s.underExact = true;
                    s.underSig = us;
                    write = true;
                } else if (dragging) {
                    if (!s.uJob) startUnderJob(); // (the share follows from the worker; the last one stays meanwhile)
                } else {
                    if (UnderDelta(s, bake[s.story], bakeKeys[s.story])) {
                        s.underExact = false; // (exact again from the worker)
                        s.uJob.reset();
                    }
                    s.underSig = us;
                    write = true;
                }
            }
            if (!maskWait) s.underDirty = false;
            // (a drag: with more lamps than one batch the shader cannot evaluate "all"; the CPU's per-texel pass waits for the
            // lamp to be still, the last values stay meanwhile)
            if (dragging && !maskGpu) write = false;
            void* q = nullptr;
            IDirect3DVertexBuffer9* const target = s.dynNext ? s.dynNext : s.dyn; // (the draws keep the current one until this one is filled)
            if (!maskWait && write && SUCCEEDED(target->Lock(0, 0, &q, 0)) && q) {
                auto* dyn = static_cast<RtLampGpu::TexelDyn*>(q);
                for (size_t i = 0; i < s.masked.size(); i++) {
                    float vis = 0, all = 0;
                    if (!maskGpu)
                        for (const auto& l : mine) RtLampModel::AddMask(l, &s.maskedPos[i * 6], 1.0f, s.thr, vis, all);
                    dyn[s.masked[i]] = RtLampGpu::TexelDyn{{all, s.under[i], 0, 0}};
                }
                target->Unlock();
                if (s.dynNext) {
                    std::swap(s.dyn, s.dynNext);
                    s.map.dyn = s.dyn;
                }
                s.dynCurrent = maskGpu;
            }
        }
        QueryPerformanceCounter(&tm1);
        maskMs += Ms(tm0, tm1);
        if (maskWait) {
            left++; // (relit once the worker's share arrives)
            continue;
        }
        RtLampGpu::Params p;
        p.thr = s.thr, p.fade = fade, p.reachOn = reachOn, p.mask = !s.masked.empty(), p.radius = radius, p.aa = Penumbra::g_on.load(std::memory_order_relaxed) ? LevelLightShare::MapHalfTexelOf(s.room) : 0.0f, p.farDist = kFar;
        p.fast = dragging;
        p.maskAllGpu = maskGpu;
        std::string err;
        bool ok = true;
        // batches of the atlas's lamps (their shadows rendered unless the atlas holds them already)
        for (size_t b = 0; ok && (b < mine.size() || b == 0); b += RtLampGpu::kAtlasLamps) {
            const int n = static_cast<int>(std::min<size_t>(RtLampGpu::kAtlasLamps, mine.size() - std::min(b, mine.size())));
            float pos[RtLampGpu::kAtlasLamps][3] = {};
            uint64_t key = 1469598103934665603ull;
            Mix(key, &g_meshGen, 4), Mix(key, &radius, 4);
            for (int k = 0; k < n; k++) {
                for (int c = 0; c < 3; c++) pos[k][c] = mine[b + k].law.pos[c];
                Mix(key, pos[k], 12);
            }
            if (n > 0 && key != g_atlasKey) {
                LARGE_INTEGER ts0{}, ts1{};
                QueryPerformanceCounter(&ts0);
                ok = g_eng->RenderShadows(dev, pos, n, kFar, err);
                g_atlasKey = ok ? key : 0;
                QueryPerformanceCounter(&ts1);
                g_ph.shadows += Ms(ts0, ts1);
            }
            ok = ok && g_eng->Accumulate(dev, s.map, mine.data() + std::min(b, mine.size()), n, p, b == 0, err);
            if (mine.empty()) break;
        }
        ok = ok && g_eng->Compose(dev, s.map, s.game, s.copy, p, err, s.scale);
        s.scaled = ok && s.scale;
        if (!ok) {
            s.relit = false;
            failNote = std::format("story {} {}: {}", s.story, kMapName[s.m], err);
            continue;
        }
        // why it was relit (the self-measure line)
        if (!s.lampsSig && !s.relitVersion) g_why.first++;
        else {
            if (s.lampsSig != lampsSig) g_why.lamp++;
            if (s.relitVersion != s.version) g_why.base++;
            if (s.paramsSig != params) g_why.params++;
            if (s.meshGen != g_meshGen) g_why.occluders++;
            if (s.pendingWrite) g_why.gameWrite++;
            if ((s.fast && !dragging) || exactUnder) g_why.exact++;
        }
        s.lampsSig = lampsSig, s.paramsSig = params, s.meshGen = g_meshGen, s.relitVersion = s.version;
        s.relit = true;
        s.sig = sig;
        s.fast = dragging;
        s.pendingWrite = false;
        s.relitTick = GetTickCount64();
        relit++;
        LARGE_INTEGER td{};
        QueryPerformanceCounter(&td);
        drawMs += Ms(tm1, td);
        if (std::find(storyList.begin(), storyList.end(), s.story) == storyList.end()) storyList.push_back(s.story);
    }
    if (begun) {
        begun = false; // (ended here, not by endOnThrow)
        g_eng->End(dev);
        if (timing) g_qEnd->Issue(D3DISSUE_END), g_qDisjoint->Issue(D3DISSUE_END), g_qFreq->Issue(D3DISSUE_END), g_qPending = true;
    }
    if (!failNote.empty() && LogAllowed()) LOG_INFO("[RtOutdoorLamps] Not relit: " + failNote);
    LARGE_INTEGER t1{};
    QueryPerformanceCounter(&t1);
    const double ms = Ms(t0, t1);
    // coverage: every story's maps relit and swapped (lamp edits mark no room then)
    bool full = allReady && stories > 0;
    for (const auto& s : g_slots) full = full && (!s->used || (s->ok && s->relit));
    {
        // (14/10) not a lamp a covering slot keeps baked in its base (kept when it was not drawn): its edits must reach the
        // game's solve, the GPU does not draw it there
        // nor a lamp missing from the list of a story where it is drawable: the GPU does not draw it there either
        std::vector<uintptr_t> covered;
        covered.reserve(lamps.size());
        uint32_t storyBits = 0;
        for (int S = 0; S <= 7; S++)
            if (rooms[S]) storyBits |= 1u << S;
        for (const GpuLamp& g : lamps) {
            bool baked = false;
            for (const auto& s : g_slots) baked = baked || (s->used && s->kept.count(g.light) && !s->drawn.count(g.light));
            uint32_t listed = 0;
            for (int S = 0; S <= 7; S++)
                if (g.mult[S]) listed |= 1u << S;
            if (!baked && (listed & storyBits) != storyBits) {
                LampRead r;
                if (!ReadLamp(g.light, r)) baked = true;
                for (int S = 0; S <= 7 && !baked; S++)
                    if ((storyBits & ~listed) >> S & 1) baked = RtLampModel::Drawn(r.f, rns[S].lot);
            }
            if (!baked) covered.push_back(g.light);
        }
        std::lock_guard<std::mutex> lk(g_coverMx);
        g_cover.tracker = tracker;
        g_cover.full = full;
        g_cover.at = GetTickCount();
        g_cover.lamps.clear();
        g_cover.lamps.insert(covered.begin(), covered.end());
    }
    // the coverage, logged when it changes (lamp edits mark no room only while every story is covered)
    if (static_cast<int>(full) != g_coverLogged) {
        g_coverLogged = full ? 1 : 0;
        std::string why;
        if (!full) {
            if (!waiting.empty()) why = waiting.front() + (waiting.size() > 1 ? std::format(" (+{} more)", waiting.size() - 1) : "");
            for (const auto& s : g_slots)
                if (why.empty() && s->used && !(s->ok && s->relit))
                    why = std::format("story {} {} {}", s->story, kMapName[s->m], s->ok ? "not relit yet" : "not relit: " + s->why);
        }
        LOG_INFO(full ? std::string("[RtOutdoorLamps] Every story covered: edits of the lot's GPU lamps mark no room")
                      : "[RtOutdoorLamps] Not every story covered (" + (why.empty() ? std::string("?") : why) +
                            "): edits of the lot's lamps go through the game's room solve until it is");
    }
    if (relit) {
        const int tiles = static_cast<int>(g_eng->GetStats().shadowTiles) - tilesBefore;
        c_mapsRelit.fetch_add(relit, std::memory_order_relaxed);
        c_tiles.fetch_add(tiles, std::memory_order_relaxed);
        if (!g_roundOpen) {
            g_roundOpen = true;
            g_lastRelightAt = nowTick | 1;
            g_roundCpuMs = 0, g_roundMaps = 0, g_roundFrames = 0, g_roundTiles = 0;
            g_roundStories.clear();
            g_ph = Phases{};
        }
        g_roundCpuMs += ms + slotsMs + lampsMs;
        g_roundMaps += relit, g_roundFrames++, g_roundTiles += tiles;
        g_ph.slots += slotsMs, g_ph.lamps += lampsMs, g_ph.mask += maskMs, g_ph.draw += drawMs;
        for (int S : storyList)
            if (std::find(g_roundStories.begin(), g_roundStories.end(), S) == g_roundStories.end()) g_roundStories.push_back(S);
    }
    g_rr += relit ? static_cast<size_t>(relit) : 0;
    if (g_roundOpen && left == 0) {
        // the relight is complete: every dirty map was relit
        g_roundOpen = false;
        g_why.relights++;
        c_relights.fetch_add(1, std::memory_order_relaxed);
        g_lastMs.store(static_cast<float>(g_roundCpuMs));
        std::sort(g_roundStories.begin(), g_roundStories.end());
        storiesRelit = static_cast<int>(g_roundStories.size());
        const int s0 = g_roundStories.empty() ? 0 : g_roundStories.front(), s1 = g_roundStories.empty() ? 0 : g_roundStories.back();
        const std::string gpu = g_lastGpuMs.load() >= 0.0f ? std::format("{:.2f}", g_lastGpuMs.load()) : std::string("-");
        const std::string phases = std::format("phases: maps read {:.2f}, lamps read {:.2f}, floor mask {:.2f}, shadows {:.2f}, draws {:.2f} ms; {} frame{}{}",
                                               g_ph.slots, g_ph.lamps, g_ph.mask, g_ph.shadows, g_ph.draw - g_ph.shadows, g_roundFrames,
                                               g_roundFrames == 1 ? "" : "s", dragging ? ", dragged (cheaper shadows)" : "");
        // the lamp edits this relight shows
        std::vector<EditNote> shown;
        {
            std::lock_guard<std::mutex> lk(g_editMx);
            for (auto it = g_edits.begin(); it != g_edits.end();)
                if (it->tracker == tracker && full) {
                    shown.push_back(*it);
                    it = g_edits.erase(it);
                } else
                    ++it;
        }
        if (!shown.empty()) {
            const double editMs = Ms(shown.front().at, t1);
            g_lastEditMs.store(static_cast<float>(editMs));
            if (LogAllowed())
                LOG_INFO(std::format("[RtOutdoorLamps] Lamp edit ({} lamp{}, {}): every story relit {:.1f} ms after the edit ({} maps of stories {}..{}), no room 0 solved; "
                                     "{:.2f} ms CPU, GPU {} ms; {}",
                                     shown.size(), shown.size() == 1 ? "" : "s", shown.front().kind ? "moved" : "a value or a switch", editMs, g_roundMaps, s0, s1,
                                     g_roundCpuMs, gpu, phases));
        } else if (LogAllowed())
            LOG_INFO(std::format("[RtOutdoorLamps] Lot {:08X}: {} maps relit (stories {}..{}), {} lamps handled, {} shadow tiles rendered, {:.2f} ms CPU (last GPU {} ms); {}{}",
                                 static_cast<uint32_t>(g_lotId), g_roundMaps, s0, s1, lamps.size(), g_roundTiles, g_roundCpuMs, gpu, phases,
                                 full ? "" : "; not every story covered"));
    }
}
} // namespace

void OnPresent(IDirect3DDevice9* dev) {
    g_frame++;
    if (dev) g_dev = dev;
    if (dev && g_restartRequested) {
        g_restartRequested = false;
        DropAll(dev, "lighting restart", false); // the paced restart owns room scheduling
        g_on.store(false);
        g_proxyUsed.reset();
        {
            std::lock_guard<std::mutex> lock(g_proxyMx);
            g_proxyEpoch.fetch_add(1, std::memory_order_relaxed);
            g_proxy.reset();
            g_proxyWhy.clear();
        }
        g_proxyWanted.store(g_tracker);
        g_askedSolve.clear();
        g_exportBackoff.clear();
        for (KeptList& list : g_keptList) list = KeptList{};
        g_budgetOut = false;
        if (g_eng) g_eng->ReleaseDefault();
        LOG_INFO("[RtOutdoorLamps] Lighting restart: maps, pending jobs and shadow targets discarded");
    }
    if (dev && g_ps3 < 0) { // (read once, also while off: the menu says when Instant cannot run here)
        D3DCAPS9 caps{};
        if (SUCCEEDED(dev->GetDeviceCaps(&caps))) {
            g_ps3 = caps.PixelShaderVersion >= D3DPS_VERSION(3, 0) && caps.VertexShaderVersion >= D3DVS_VERSION(3, 0) ? 1 : 0;
            g_mrt = caps.NumSimultaneousRTs >= 2 ? 1 : 0;
            if (!g_mrt) LOG_INFO("[RtOutdoorLamps] The device draws into one render target at a time: outdoor light stays cut at 1");
            if (!g_ps3) LOG_WARNING("[RtOutdoorLamps] The device has no shader model 3: Instant lamp lighting cannot run");
        }
    }
    // (nothing is installed before the option is first switched on: off, every draw is the game's as before)
    if (!dev || g_installed || !g_option.load(std::memory_order_relaxed)) return;
    g_installed = true;
    for (auto& w : g_watchTex) w.store(nullptr);
    RenderCallbacks::Add(RenderCallbacks::endSceneBeforeOverlay, OnEndScene);
    RenderCallbacks::Add(RenderCallbacks::preReset, OnPreReset);
    // the swap: one SetTexture callback, registered once the option is first switched on (never before: off, no SetTexture of
    // the frame goes through it)
    D3D9Hooks::RegisterSetTexture("RtOutdoorLampsMapSwap", [](D3D9Hooks::DeviceContext& ctx, DWORD stage, IDirect3DBaseTexture9* t) {
        if (!t) return D3D9Hooks::HookAction::Continue;
        return SwapTexture(ctx.device, stage, t) ? D3D9Hooks::HookAction::Skip : D3D9Hooks::HookAction::Continue;
    }, D3D9Hooks::Priority::Late);
}

bool SwapTexture(IDirect3DDevice9* dev, DWORD stage, IDirect3DBaseTexture9* t) {
    if (!g_on.load(std::memory_order_relaxed) || g_slots.empty()) return false;
    bool swapped = false;
    SwapCopy(dev, stage, t, swapped);
    return swapped;
}

void OnWorldChanged() {
    // (render thread) every map, the occluders and the edits of the previous world go; nothing is sent to a solve (the
    // previous world's lots are gone, and the new world's lots solve their rooms as they load)
    g_tracker = 0;
    g_restartRequested = false;
    DropAll(g_dev, "the world changed");
    g_proxyUsed.reset();
    {
        std::lock_guard<std::mutex> lk(g_proxyMx);
        g_proxyEpoch.fetch_add(1, std::memory_order_relaxed);
        g_proxy.reset();
        g_proxyWhy.clear();
    }
    {
        std::lock_guard<std::mutex> lk(g_editMx);
        g_edits.clear();
        g_sendDue.clear(); // (14/10: trackers of the old world)
    }
    g_askedSolve.clear();
    g_exportBackoff.clear();
    for (KeptList& kl : g_keptList) kl = KeptList{};
    g_budgetOut = false;
}

void RestartLighting() { g_restartRequested = true; }
bool RestartIncomplete() {
    if (!g_option.load(std::memory_order_relaxed) || !g_watch.load(std::memory_order_relaxed) || !g_jobs.empty() || g_restartGpuPending) return false;
    if (std::any_of(g_slots.begin(), g_slots.end(), [](const auto& slot) { return slot->used && slot->built && slot->ok && !slot->relit; })) return false;
    if (g_restartGpuFallback) return true;
    if (std::any_of(g_slots.begin(), g_slots.end(), [](const auto& slot) { return slot->used && slot->built && !slot->ok; })) return true;
    std::lock_guard<std::mutex> lock(g_coverMx);
    return g_budgetOut && !g_cover.full;
}
bool RestartPending() {
    if (g_restartRequested) return true;
    if (RestartIncomplete()) return false;
    if (!g_option.load(std::memory_order_relaxed) || !g_watch.load(std::memory_order_relaxed)) return false;
    if (g_restartGpuPending || !g_jobs.empty() || !g_proxyUsed || !g_on.load(std::memory_order_relaxed)) return true;
    if (std::any_of(g_slots.begin(), g_slots.end(), [](const auto& slot) { return slot->used && !slot->relit; })) return true;
    std::lock_guard<std::mutex> lock(g_coverMx);
    return !g_cover.full;
}

// ---- the light tree thread ----
namespace {
uintptr_t TrackerOfTl(uintptr_t tl, int& story) {
    __try {
        story = *reinterpret_cast<const int*>(tl + 0x1A0);
        return *reinterpret_cast<const uintptr_t*>(tl + 4);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
uintptr_t TrackerOfRoom(const void* room, int& story, int& id, int& cls, uintptr_t& mgr) {
    __try {
        const BYTE* r = static_cast<const BYTE*>(room);
        mgr = *reinterpret_cast<const uintptr_t*>(r);
        id = *reinterpret_cast<const int*>(r + 0xC);
        cls = *reinterpret_cast<const int*>(r + 0xF4);
        if (!mgr) return 0;
        story = *reinterpret_cast<const int*>(mgr + 0x88);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
    const void* r0 = nullptr;
    // the lot's tracker: the one whose room 0 of that story is this story's room 0
    const uintptr_t want = g_proxyWanted.load();
    if (want && story >= 0 && story <= 7 && (r0 = LevelLightShare::Room0Of(want, story)) != nullptr) {
        uintptr_t m0 = 0;
        __try {
            m0 = *static_cast<const uintptr_t*>(r0);
        } __except (EXCEPTION_EXECUTE_HANDLER) {
            return 0;
        }
        if (m0 == mgr) return want;
    }
    return 0;
}
// The lot's occluders (light tree thread): built when wanted or when its walls, floors or openings changed
void BuildProxy(uintptr_t tracker, const void* room0) {
    const uint32_t epoch = g_proxyEpoch.load(std::memory_order_relaxed);
    const uint64_t geo = room0 ? LevelLightShare::LotGeometryKey(room0) : 0;
    {
        std::lock_guard<std::mutex> lk(g_proxyMx);
        if (g_proxy && g_proxy->tracker == tracker && geo && g_proxy->geo == geo) return;
    }
    auto p = std::make_shared<Proxy>();
    p->tracker = tracker;
    p->geo = geo;
    std::string why;
    LARGE_INTEGER a{}, b{};
    QueryPerformanceCounter(&a);
    const bool ok = LevelLightShare::ExportLotProxy(tracker, p->lot, why);
    QueryPerformanceCounter(&b);
    std::lock_guard<std::mutex> lk(g_proxyMx);
    if (epoch != g_proxyEpoch.load(std::memory_order_relaxed)) return;
    if (!ok) {
        g_proxyWhy = why;
        return;
    }
    p->gen = g_proxyGen.fetch_add(1) + 1;
    size_t edges = 0;
    for (const auto& s : p->lot.stories) edges += s.edges.size();
    g_proxy = p;
    g_proxyWhy.clear();
    c_proxyBuilds.fetch_add(1, std::memory_order_relaxed);
    LOG_INFO(std::format("[RtOutdoorLamps] Occluders read for tracker {:08X}: {} stories, {} wall edges, in {:.2f} ms (light tree thread)", tracker, p->lot.stories.size(), edges, Ms(a, b)));
}
} // namespace

bool CoversRoom(const void* room) {
    if (!g_on.load(std::memory_order_relaxed) || !room) return false;
    int story = -99, id = -1, cls = -1;
    uintptr_t mgr = 0;
    const uintptr_t tracker = TrackerOfRoom(room, story, id, cls, mgr);
    if (!tracker || id != 0) return false;
    std::lock_guard<std::mutex> lk(g_coverMx);
    return g_cover.tracker == tracker && g_cover.full && GetTickCount() - g_cover.at < kCoverFreshMs;
}
bool HandlesLamp(uintptr_t tl, uintptr_t light, int room) {
    if (!g_on.load(std::memory_order_relaxed) || room != 0 || !light || !tl) return false;
    int story = -99;
    const uintptr_t tracker = TrackerOfTl(tl, story);
    if (!tracker) return false;
    std::lock_guard<std::mutex> lk(g_coverMx);
    if (g_cover.tracker != tracker) return false;
    const bool handled = g_cover.full && GetTickCount() - g_cover.at < kCoverFreshMs && g_cover.lamps.count(light);
    if (!handled && g_cover.lamps.count(light)) c_editsRefused.fetch_add(1, std::memory_order_relaxed);
    return handled;
}
void NoteLampEdit(uintptr_t tl, uintptr_t light, int kind) {
    int story = -99;
    const uintptr_t tracker = TrackerOfTl(tl, story);
    if (!tracker) return;
    c_edits.fetch_add(1, std::memory_order_relaxed);
    EditNote e;
    e.tracker = tracker, e.light = light, e.story = story, e.kind = kind;
    QueryPerformanceCounter(&e.at);
    {
        std::lock_guard<std::mutex> lk(g_editMx);
        if (g_edits.size() < 256) g_edits.push_back(e);
        if (story >= 0 && story <= 7) { // (light tree thread; cleared by the render thread)
            SendDue& d = g_sendDue[{tracker, story}];
            d.at = GetTickCount() | 1;
            d.moved = d.moved || kind != 0;
            if (std::find(d.lamps.begin(), d.lamps.end(), light) == d.lamps.end() && d.lamps.size() < 64) d.lamps.push_back(light);
        }
    }
}

void NoteSwitchOn(uintptr_t, uintptr_t light) { g_switchOn.store(light); }
void AfterRoomUpdate(const void* tl) {
    const uintptr_t want = g_proxyWanted.load();
    if (want && tl) {
        int story = -99;
        if (TrackerOfTl(reinterpret_cast<uintptr_t>(tl), story) == want) {
            std::shared_ptr<const Proxy> have;
            {
                std::lock_guard<std::mutex> lk(g_proxyMx);
                have = g_proxy;
            }
            if (!have || have->tracker != want) HookGuard::Run("RtOutdoorLamps occluders", [want] { BuildProxy(want, LevelLightShare::Room0Of(want, 0)); });
        }
    }
    // the rooms that take room 0's lamps, after an edit the GPU took: once, kHoldQuietMs after the story's last edit (a drag
    // sends them when it is over), kDragQuietMs after a value or a switch
    // (14/10 review: as the tail of the edit, with no urgency and under the spread budget; the stories of a lot due together
    // in one send, each room once; the roofed rooms that saw a door of an edited lamp with the edit's tier 1)
    const DWORD now = GetTickCount();
    struct Due {
        unsigned mask = 0;
        std::vector<uintptr_t> lamps;
    };
    std::map<uintptr_t, Due> due;
    {
        std::lock_guard<std::mutex> lk(g_editMx); // (OnWorldChanged clears it on the render thread)
        if (g_sendDue.empty()) return;
        for (auto it = g_sendDue.begin(); it != g_sendDue.end();) {
            if (now - it->second.at < (it->second.moved ? kHoldQuietMs : kDragQuietMs)) {
                ++it;
                continue;
            }
            Due& d = due[it->first.first];
            d.mask |= 1u << it->first.second;
            for (uintptr_t l : it->second.lamps)
                if (std::find(d.lamps.begin(), d.lamps.end(), l) == d.lamps.end()) d.lamps.push_back(l);
            it = g_sendDue.erase(it);
        }
    }
    for (const auto& [tracker, d] : due) c_sends.fetch_add(LevelLightShare::SendRoom0DependentsTail(tracker, d.mask, &d.lamps), std::memory_order_relaxed);
}
void NoteRoomSolved(const void* room, bool gather) {
    if (!g_watch.load(std::memory_order_relaxed) || !room) return;
    int story = -99, id = -1, cls = -1;
    uintptr_t mgr = 0;
    const uintptr_t tracker = TrackerOfRoom(room, story, id, cls, mgr);
    if (mgr) {
        std::lock_guard<std::mutex> lk(g_noteMx);
        if (g_solvedNotes.size() < 512) g_solvedNotes.emplace_back(mgr, cls);
    }
    if (gather && tracker && id == 0) HookGuard::Run("RtOutdoorLamps occluders", [tracker, room] { BuildProxy(tracker, room); });
}
void NoteMapsWritten(const void* room) {
    if (!g_watch.load(std::memory_order_relaxed) || !room) return;
    int story = -99, id = -1, cls = -1;
    uintptr_t mgr = 0;
    TrackerOfRoom(room, story, id, cls, mgr);
    if (!mgr) return;
    std::lock_guard<std::mutex> lk(g_noteMx);
    if (g_solvedNotes.size() < 512) g_solvedNotes.emplace_back(mgr, cls);
}
void NoteUnlock(const void* texture) {
    if (!g_watch.load(std::memory_order_relaxed) || !texture) return;
    for (int i = 0; i < kMaxWatch; i++)
        if (g_watchTex[i].load(std::memory_order_relaxed) == texture) g_watchDirty.fetch_or(1ull << i, std::memory_order_relaxed);
}
IDirect3DTexture9* ScaleOf(const void* texture) {
    if (!texture || !g_on.load(std::memory_order_relaxed)) return nullptr;
    for (const auto& s : g_slots)
        if (s->copy == texture) return s->relit && s->scaled ? s->scale : nullptr;
    return nullptr;
}
const void* GameMapOf(const void* texture) {
    if (!texture || !g_on.load(std::memory_order_relaxed)) return texture;
    for (const auto& s : g_slots)
        if (s->copy == texture) return s->game;
    return texture;
}

std::string InactiveNote() {
    if (!g_option.load(std::memory_order_relaxed)) return {};
    if (g_inactive.rfind("the room solve hooks are not in", 0) == 0) return "Instant needs \"Outdoor light between floors\" (Lighting > Stories)";
    if (g_inactive.rfind("Quick outdoor lamp edits keep no", 0) == 0) return "Instant could not start its outdoor lamp edits; see ApexRadiance_LOG.txt";
    return {}; // (active, or a passing state: no active lot yet, shaders compiling)
}
float GpuMs() { return g_option.load(std::memory_order_relaxed) ? g_lastGpuMs.load(std::memory_order_relaxed) : -1.0f; }
std::string Unsupported() {
    if (std::string missing; GameAddr::Resolved() &&
                             !GameAddr::Have({GameAddr::Id::LampK1, GameAddr::Id::LampK2, GameAddr::Id::LampCmax, GameAddr::Id::StreetLampScale, GameAddr::Id::ConeScale5}, &missing))
        return GameAddr::NotAvailable(missing);
    if (g_ps3 == 0) return "Not available: your graphics card cannot draw the lamps' shadows";
    if (g_compiled.state.load() == 3) return "Not available: Instant could not start on this graphics card (see ApexRadiance_LOG.txt)";
    return {};
}
std::string Status() {
    if (!g_option.load()) return "off";
    std::string st = g_inactive.empty() ? std::string("on") : "inactive: " + g_inactive;
    long binds = 0;
    int ok = 0, relit = 0;
    std::string maps;
    for (const auto& s : g_slots) {
        binds += s->binds;
        ok += s->ok ? 1 : 0;
        relit += s->relit ? 1 : 0;
    }
    bool full = false;
    size_t lamps = 0;
    {
        std::lock_guard<std::mutex> lk(g_coverMx);
        full = g_cover.full;
        lamps = g_cover.lamps.size();
    }
    return std::format("{} | lot {:08X}: {} maps ({} relit and bound in place of the game's, {} usable), {} lamps drawn on the GPU, every story covered: {} | relights {} "
                       "({} maps, {} shadow tiles), last {:.2f} ms CPU, GPU {} ms | lamp edits taken {} (refused while not covered {}), last edit relit {} ms after it, "
                       "rooms sent after edits {} | room 0s sent to gather a lamp they lost {} | occluders read {} | copies bound {} times | its own cost {:.3f} ms a frame{}",
                       st, static_cast<uint32_t>(g_lotId), g_slots.size(), relit, ok, lamps, full ? "yes" : "no", c_relights.load(), c_mapsRelit.load(), c_tiles.load(),
                       g_lastMs.load(), g_lastGpuMs.load() >= 0.0f ? std::format("{:.2f}", g_lastGpuMs.load()) : std::string("-"), c_edits.load(), c_editsRefused.load(),
                       g_lastEditMs.load() >= 0.0f ? std::format("{:.1f}", g_lastEditMs.load()) : std::string("-"), c_sends.load(), c_repairs.load(), c_proxyBuilds.load(), binds,
                       g_frameMsSum, g_budgetOut ? " | the lot's kept parts overflow the memory budget" : "");
}

} // namespace RtOutdoorLamps
