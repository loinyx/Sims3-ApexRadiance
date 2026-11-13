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
#include "build_flavor.h"
#include "apex_paths.h"
#include "terrain_bake_rule.h"
#include "rt_lamp_gpu.h"
#include "drag_lite.h"
#include "gpu_room_lamps.h"
#include "platform.h"
#include "rt_lamp_model.h"
#include "rt_lamp_proxy.h"
#include "stair_set.h"
#include "rt_lamps_hlsl.h"
#include "shader_cache.h"
#include "outdoor_delta.h"
#include "outdoor_delta_model.h"
#include "phys_lamp_pick.h"
#include "level_light_share.h"
#include "light_detail.h"
#include "lot_light_bridge.h"
#include "lamp_reach.h"
#include "penumbra.h"
#include "room_map_dither.h"
#include "lamp_mark_filter.h"
#include "lot_lighting_motion.h"
#include "room_light_queue.h"
#include "apex_log.h"
#include "hook_guard.h"
#include "dev_switches.h"
#include "apex_memory.h"
#include "game_addresses.h"
#include "d3d9_hooks.h"
#include "d3d9_extra_hooks.h"
#include "render_callbacks.h"
#include "terrain_stamps.h"
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
constexpr int kMaxWatch = 128;           // maps watched for writes (8 stories x 4 maps x 2 classes, the lots next to it from the upper half: a round relit 87 maps, and a map past the cap went unwatched)
constexpr float kFar = 64.0f;            // m: the shadow maps' range
constexpr DWORD kSolveAskMs = 10000;     // a room 0 with no kept solve is sent to its solve at most this often
constexpr DWORD kSolveAskActiveMs = 1500; // the active lot's: a solve that kept no parts (walls or floors changed while the lot
                                          // streamed in) waited 10 s for the next send, the lot on the game's maps meanwhile
constexpr DWORD kCoverFreshMs = 3000;    // a lot's coverage counts this long after the last check
constexpr int kSolveAsks = 3;            // a room 0 is sent to its solve for a kept solve at most this often in a row
constexpr DWORD kSolveRetryMs = 60000;   // then this seldom: a run of refused sends left the story on the game's maps until the lot changed
constexpr DWORD kDragQuietMs = 300;      // a GPU lamp that changed this recently is being dragged (or edited again and again)
constexpr DWORD kLeftLotGraceMs = 20000; // the lot left at a switch keeps its room 0s' kept solves this long (a return reuses them)
constexpr DWORD kActiveLotHoldMs = 3000; // (active_lot_hold) while the camera moves, another lot becomes the active one after this as the first lit at the high quality
constexpr DWORD kHoldQuietMs = 1500;     // the rooms that take room 0's lamps are sent this long after a story's last move
                                         // (a Build mode drag moves in grid steps with pauses of up to about a second)
constexpr DWORD kCheckEveryMs = 2000;    // the consistency check of the GPU lamps' stories
constexpr DWORD kRepairWaitMs = 4000;    // a story sent to gather a lamp is checked again after this
constexpr uint64_t kGameWriteGapMs = 500; // a relight made only for a game write: at most this often per map
// shortcut: a bake that keeps changing past this falls back to the share on the render thread, as before; tune from a recording
constexpr uint64_t kUnderWaitMs = 500; // a shown map waits this long at most for the worker's under-floor share
// shortcut: lamps that keep changing past this fall back to "all" on the render thread, as before; tune from a recording
constexpr uint64_t kAllWaitMs = 2000; // a map waits this long at most for the worker's floor mask "all" (one run: 105-255 ms)
// (2.15.1) A lamp lying on the lot's ground (its light 5 cm over the lawn, F8 capture) lit a tree above it and left the
// lawn dark: the law's light falls with its angle of incidence, and from 5 cm the lawn 2 m away takes about 1 % of what it
// would from above. On story 0's floor map a lamp whose light stands lower than kLowLampLift over the lot's ground is
// drawn from that height, as a fixture's glow would spread; walls, ceilings, objects and trees keep its real place. The
// terrain bake stamps it from the same height (TerrainBakeRule::StampHeight), so the lawn beyond the lot and the atlas agree.
std::atomic<long> g_lowLampLifted{0};
// The lots next to the active one whose room-0 maps take the active lot's lamp posts too (on the walls of the house next
// door): the nearest kMaxNeighbours lots whose box comes within kNeighbourReach of the active lot's (at most the 4 lots
// LevelLightShare::SetGpuNeighbourLots and OutdoorDelta::SetNeighbourLots hold)
constexpr int kMaxNeighbours = 4;
constexpr float kNeighbourReach = 20.0f; // m
constexpr double kFrameCpuMs = 2.0;      // a frame's relight work stops starting maps past this (the rest next frame)
constexpr double kEditFrameCpuMs = 8.0;  // (GPU room lamps) the maps of a lamp edit taken: relit in one frame up to this
constexpr double kFrameGpuMs = 6.0;      // ... or past this much GPU time, each map's own as last timed (MapGpuMs), else its units x kMsPerUnit
constexpr double kMsPerUnit = 2.0e-6;    // a unit: texel x lamp (outdoor: 7 exact, 1 dragged; physical: 7); about 2 ns of GPU each, tools/rt_lamps_test

// The GPU relight follows the bake this build makes: the lamp's ball of Shadow softness (Penumbra::Radius), Smooth light
// maps' soft light cut and map dither when they are on (no lamp shade shapes: the game's cones)

std::atomic<bool> g_option{false}, g_on{false}, g_watch{false};
bool g_restartRequested = false;
std::atomic<uint32_t> g_proxyEpoch{1};
std::atomic<long> c_relights{0}, c_mapsRelit{0}, c_tiles{0}, c_edits{0}, c_editsRefused{0}, c_sends{0}, c_binds{0}, c_proxyBuilds{0};
std::atomic<long> c_physUnsent{0}; // (physical indoor lamps) edits whose room-0 dependents were not sent (AfterRoomUpdate)
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
    AddRtLampShader(7), AddRtLampShader(8), AddRtLampShader(9), AddRtLampShader(10), AddRtLampShader(11), AddRtLampShader(12), AddRtLampShader(13),
    AddRtLampShader(14), AddRtLampShader(15), AddRtLampShader(16), AddRtLampShader(17), AddRtLampShader(18), AddRtLampShader(19), AddRtLampShader(20),
    AddRtLampShader(21), AddRtLampShader(22), AddRtLampShader(23), AddRtLampShader(24), AddRtLampShader(25), AddRtLampShader(26), AddRtLampShader(27)};
static_assert(RtLampGpu::kShaderCount == 28, "kRtLampShaderIds lists every RtLampGpu::ShaderId");
void CompileAll() {
    for (int i = 0; i < RtLampGpu::kShaderCount; i++) {
        std::string err;
        if (!ShaderCache::Bytecode(kRtLampShaderIds[i], g_compiled.code[i], &err)) {
            if (RtLampGpu::IsPhysical(static_cast<RtLampGpu::ShaderId>(i))) { // (optional: the physical indoor lamps stay off, the game's maps shown)
                g_compiled.code[i].clear();
                LOG_WARNING(std::format("[RtOutdoorLamps] {} is not available ({}): physical indoor lamps keep the game's maps", RtLampGpu::EntryOf(static_cast<RtLampGpu::ShaderId>(i)), err));
                continue;
            }
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
        // (an outdoor delta replay points the list at its one-lamp copy for a moment: not the room's, the kept list stands in)
        if (!b || e < b || e - b > max || LevelLightShare::IsReplayList(b)) return -1;
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
    float tubeEnd[3] = {}, tubeLen = 0.0f; // a TubeLight (6): its end B +0x170 and its length +0x1A0 (GpuLampLaw::Tube)
    float areaU[3] = {}, areaV[3] = {}, areaN[3] = {}, areaE[2] = {}; // an area light: +0x170, +0x190, +0x180, +0x1A0 (GpuLampLaw::Area)
};
// What a TubeLight's share reads (FUN_006bd100, its evaluation 0x006BFA70's colour: min(Cmax, k2 x range +0x130 x intensity
// +0x10 / (k1 d^2)) to the segment from +0x120 to +0x170 of length +0x1A0, times its colour +0xE0; dumpbin): the raw
// values. POD (SEH)
bool ReadTubeShare(uintptr_t L, uint32_t out[10]) {
    __try {
        const BYTE* p = reinterpret_cast<const BYTE*>(L);
        std::memcpy(out, p + 0x10, 4);
        std::memcpy(out + 1, p + 0x120, 16); // (+0x120 .. +0x12C, and +0x130)
        std::memcpy(out + 5, p + 0x170, 12);
        std::memcpy(out + 8, p + 0x1A0, 4);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
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
        } else if (r.f.type == 6) {
            for (int i = 0; i < 3; i++) r.tubeEnd[i] = reinterpret_cast<const float*>(p + 0x170)[i];
            r.tubeLen = *reinterpret_cast<const float*>(p + 0x1A0);
        } else if (TerrainBakeRule::AreaType(r.f.type)) {
            r.areaShare = TerrainBakeRule::AreaGroundShare(*reinterpret_cast<const float*>(p + TerrainBakeRule::kAreaAxisYOffset));
            for (int i = 0; i < 3; i++) {
                r.areaU[i] = reinterpret_cast<const float*>(p + 0x170)[i];
                r.areaV[i] = reinterpret_cast<const float*>(p + 0x190)[i];
                r.areaN[i] = reinterpret_cast<const float*>(p + 0x180)[i];
            }
            r.areaE[0] = *reinterpret_cast<const float*>(p + 0x1A0);
            r.areaE[1] = *reinterpret_cast<const float*>(p + 0x1A4);
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
    L.cmax = g.cmax * (r.f.type == 11 ? g.street : 1.0f); // the game scales the capped light: street x min(Cmax, .)
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
    } else if (r.f.type == 6) { // (a tube: its segment, GpuLampLaw::Tube)
        float b[3], s[3];
        ToLot(xf, r.tubeEnd, b);
        for (int i = 0; i < 3; i++) s[i] = b[i] - L.pos[i];
        GpuLampLaw::TubeShape(L, s, r.tubeLen);
    }
    L.r0sq = 1e30f, L.invR2 = 0.0f;
    if (reachOn) {
        float r0, R;
        LampReach::Radii(w, r0, R);
        L.r0sq = r0 * r0;
        L.invR2 = 1.0f / (R * R);
    }
    if (TerrainBakeRule::AreaType(r.f.type)) { // (an area light: its axes and size, GpuLampLaw::Area; its last two floats, no reach)
        float u[3], v[3], nrm[3];
        DirToLot(xf, r.areaU, u);
        DirToLot(xf, r.areaV, v);
        DirToLot(xf, r.areaN, nrm);
        GpuLampLaw::AreaShape(L, u, v, nrm, r.areaE[0], r.areaE[1], r.f.type == 10);
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
std::atomic<uint64_t> g_watchDirty[kMaxWatch / 64] = {};
std::mutex g_noteMx;
std::vector<std::pair<uintptr_t, int>> g_solvedNotes; // (story manager, class) of rooms whose solve ended (under g_noteMx)

// ---- the occluders (built on the light tree thread, read on the render thread) ----
struct Proxy {
    uintptr_t tracker = 0;
    uint32_t gen = 0;
    RtLampProxy::Lot lot;
};
std::mutex g_proxyMx;
std::shared_ptr<const Proxy> g_proxy;         // (under g_proxyMx)
std::atomic<uintptr_t> g_proxyWanted{0};      // the lot whose occluders the render thread waits for
std::atomic<uint32_t> g_proxyGen{1};
std::string g_proxyWhy;                       // (under g_proxyMx) why the last build failed
uint64_t g_proxyGeo = 0;                      // (under g_proxyMx) the geometry key g_proxy's lot was last read at (BuildProxy)

// ---- coverage and lamp edits (render thread publishes, the light tree thread reads) ----
struct Cover {
    uintptr_t tracker = 0;
    bool full = false;
    DWORD at = 0;
    std::unordered_set<uintptr_t> lamps;
    std::set<std::pair<int, uintptr_t>> kept0; // (GPU room lamps) (story, lamp): a lamp room 0's kept parts draw there
    std::set<const void*> phys0; // (physical indoor lamps) the room 0s whose maps were all relit physically (PhysCoversRoom0)
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
std::atomic<uintptr_t> g_tookEdit{0};           // the tracker whose room-0 maps lack an edit the GPU took (DropAll)
// (tracker, story) -> its edits since the last send: their dependents are sent kHoldQuietMs after the last one, or kDragQuietMs
// when none moved a lamp (a value or a switch: no drag pauses to wait out); light tree thread
struct SendDue {
    DWORD at = 0;                 // the last edit
    bool moved = false;           // one of them moved a lamp
    bool settle = false;          // one of them switched a lamp or changed its intensity
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
// (physical indoor lamps; render thread) room -> the kept solve whose physical draw failed (prepared again with its lamps in the
// base), and the last reason its room 0 was not drawn physically (logged when it changes)
std::map<const void*, uint32_t> g_physRefused;
std::map<const void*, std::string> g_physNote;
void PhysNote(const void* room, int story, const std::string& why) {
    if (g_physNote.size() > 64) g_physNote.clear(), g_physRefused.clear(); // (rooms of lots long left)
    std::string& was = g_physNote[room];
    if (was == why) return;
    was = why;
    if (!why.empty()) LOG_INFO(std::format("[RtOutdoorLamps] Story {} room 0: its lot lamps stay in its base ({})", story, why));
}
std::atomic<uintptr_t> g_switchOn{0}; // a GPU lamp switched on (LampMarkFilter, light tree thread; the log on the render thread)

// ---- the maps (render thread) ----
// (GPU room lamps) a lamp of room 0's list kept as a kept room's: its law when kept, and what tells a move (its place; a tube's
// share signature)
struct KeptInfo {
    uintptr_t light = 0;
    bool colourOnly = false;
    uint64_t geo = 0;
    float pos[3] = {};
    GpuLampLaw::Lamp law;
};
using RtLampProxy::InUv; // (the UV map's texels: Slot::inUv)
struct Slot {
    int story = 0, m = 0, cls = 0;
    uintptr_t mgr = 0; // 0: a map of the active lot; else of a lot next to it (its story manager), relit with the active lot's lamp posts
    int retries = 0;   // uploads that failed for a buffer in a row (UploadSlot)
    DWORD retryAt = 0; // not built again before this tick (0 = no wait)
    const BYTE* room = nullptr;
    IDirect3DTexture9* game = nullptr; // the room's own map (AddRef'd while kept)
    IDirect3DTexture9* copy = nullptr; // DEFAULT render target: what the draws read instead
    IDirect3DTexture9* scale = nullptr; // DEFAULT render target: each texel's outdoor scale (ScaleOf; a blurred atlas's not blurred)
    bool scaled = false;                // the last relight wrote it
    // (physical) the rooms of the story below its texels' spaces continue into (Job::inRooms) and their wall response at the last
    // relight's signature (OutdoorDelta::IndoorFitOf; version 0: none kept)
    std::vector<int> inRooms;
    uint32_t inVer[4] = {}; // (Job::inVer) a new kept solve of one prepares this map again
    DWORD inLagAt = 0;      // (log) since when inVer differs from a room's kept solve (0: the same)
    int inLagFrames = 0;
    OutdoorDelta::IndoorFit inFit[4];
    bool inFlags = false; // scale holds its indoor marks (.y) from a relight of this map's layout (IndoorFlagsOf)
    // the rooms' class its texels were matched in (Job::inCls) and those texels (Prepared::inUv); this frame (InRoomsNow) each
    // room's own wall map (AddRef'd) and ambient (+0x110), and the UV map (A16B16G16R16, this map's size: (u, v) of the room's
    // texel, slot / 3, 1) built for those maps' sizes (w << 16 | h)
    int inCls[4] = {-1, -1, -1, -1};
    std::vector<InUv> inUvSrc;
    IDirect3DTexture9* inGame[4] = {};
    float inAmb[4][3] = {};
    bool inAmbOk = false; // (IndoorRoomsOf)
    IDirect3DTexture9* inUv = nullptr;
    uint32_t inUvSize[4] = {};
    std::vector<InUv> inUvBuilt; // the texels inUv holds (kept bound through a prepare of the same layout, built anew only when they change)
    int inShown = -1;            // (the 10 s summary) its relit copy shown last frame (1) or the game's map (0)
    // (InBaseNow) its marked texels' light without the GPU lamps as their rooms' kept solves of the class matched give it now (one per
    // inUvSrc entry), the versions it was read at, and the map the compose reads it from (A16B16G16R16F, this map's size)
    std::vector<std::array<float, 3>> inBaseVal;
    uint32_t inBaseVer[4] = {};
    bool inBaseDirty = false;
    IDirect3DTexture9* inBase = nullptr;
    // (sharper floors, a floor map) DEFAULT render targets at twice the map's size: what the draws read instead of the copy while
    // hiOk (the copy upsampled, the physical rooms' sub-texels over it), and the rooms' sub-texels kept between relights
    IDirect3DTexture9 *hi = nullptr, *hiOver = nullptr;
    // (physical) room 0's own floor at twice the size: composed into hiRoom0 (DEFAULT render target), its places copied into hi;
    // its sub-texels' compose points, phys points and places (DEFAULT, Prepared::vHi)
    IDirect3DTexture9* hiRoom0 = nullptr;
    IDirect3DVertexBuffer9 *vbHi = nullptr, *pvbHi = nullptr, *placesHi = nullptr;
    UINT nHi = 0;
    bool hiLogged = false; // its room-0 sharper floor's failure was logged
    bool hiOk = false;
    uint64_t hiRules = 0; // the rules the overlay's sub-texels were drawn with
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
    double gpuMs = 0.0;             // its relight's GPU time as last timed (ReadGpuTime; 0: not yet), the frame budget's estimate
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
    uint64_t underWaitSince = 0; // tick at which this shown map began waiting for the worker's share
    std::shared_ptr<struct UnderJob> uJob;
    bool dynCurrent = false;        // the mask buffer holds the under-floor share with "all" left to the shader
    // (more lamps than one batch) the CPU's "all" per masked texel, for the lamps and points of allSig; allCurrent: the mask buffer
    // holds them (a 45-lamp lot computed them again at every relight, 60-187 ms of the render thread, 88 hitches)
    std::vector<float> allVals;
    uint64_t allSig = 0;
    bool allCurrent = false;
    std::shared_ptr<struct AllJob> aJob; // "all" for new lamps on a worker; the map keeps what it shows until they arrive
    uint64_t allWaitSince = 0;           // tick at which this map began waiting for them
    // (GPU room lamps, developer) the lamps of room 0's list it does not draw that are kept as a kept room's (outdoor_delta.h
    // Room0Export::kept0): their parts (MANAGED, at their texels' places, by lamp), and each one's law now
    IDirect3DVertexBuffer9* kvb = nullptr;
    std::vector<std::pair<UINT, UINT>> kRanges;
    std::vector<KeptInfo> kLamps;
    std::vector<GpuLampLaw::Lamp> kLaws;
    bool kX = false; // their floor mask or exit parts too (AddKeptParts extra, ComposeK0PS)
    uint64_t roomsRelit = 0; // GpuRoomLamps::Sig at its last relight
    uint64_t partKey = 0;    // (rooms only) the rules and copy its last relight drew with: a relight with the same draws only the rooms that changed
    std::vector<char> kOk;
    // (physical indoor lamps) the lot lamps with a law out of its base (OutdoorDelta::ExportRoom0 physical), drawn from their laws
    // now over its phys points (DEFAULT, one per place of the map: built again after a device reset); each one's room row, room
    // 0's 3D test records; their laws now (lot space, mult = the row's), world positions and whether they have one
    IDirect3DVertexBuffer9* pvb = nullptr;
    UINT nPhys = 0;
    std::vector<uintptr_t> pLights;
    std::vector<std::array<float, 3>> pRow;
    std::shared_ptr<const RtLampGpu::BlockerTable> pBlk;
    float pHeader = -1.0f;
    std::vector<RtLampModel::Lamp> pLaws;
    std::vector<std::array<float, 3>> pWorld;
    std::vector<char> pOk;
    // (stage 4) the lamps drawn this frame: every lot lamp with a law whose reach meets the map (phys_lamp_pick.h), those out of
    // its base with their rows and the ones its solve did not list with fresh rows; the world box of its points
    std::vector<uintptr_t> qLights;
    std::vector<std::array<float, 3>> qRow;
    float pBox[6] = {1e30f, 1e30f, 1e30f, -1e30f, -1e30f, -1e30f};
    bool physLater = false; // prepared with its lamps in the base only because the physical draw was not ready yet
};
struct Ask {
    DWORD at = 0;
    int count = 0;
    bool logged = false;
    DWORD keptAt = 0; // when its kept solve was first seen (its sends start again only once it held it kKeptHoldMs)
};
// a kept solve the memory budget evicts at once gave the room a fresh round of sends each time: about 195
// sends a minute to neighbour lots, each '(1 of 3)', each a full room-0 solve of the game. Its count resets only once held this long
constexpr DWORD kKeptHoldMs = 10000;
std::vector<std::unique_ptr<Slot>> g_slots;
uintptr_t g_tracker = 0;
uint64_t g_lotId = 0;
// (active_lot_hold) the lot waiting to become the active one, since when it has been the first lit at the high quality, and
// since when the switch has been held (0: none)
uintptr_t g_holdFor = 0;
DWORD g_holdForAt = 0, g_holdAt = 0;
DWORD g_switchAt = 0; // the last active-lot switch, until a room-0 solve of the new lot ends after it (0: none)
// The new active lot's kept room-0 solves come one story at a time (the game solves one room at a time): for kKeptRushMs
// after a switch its ground story goes first (RoomLightQueue::GroundFirst, renewed while it has none) and the queue takes
// the edit budget while any story waits (KeptRush). The town square waited 18 s, its ground story last
constexpr DWORD kKeptRushMs = 8000;
DWORD g_rushFrom = 0;                  // (render thread) the switch the rush follows (0: none)
std::atomic<bool> g_rush{false};       // the active lot waits for kept solves within the rush
// (physical indoor lamps, render thread) a lot lamp as each room-0 map's pick reads it, once a round (Frame clears it): every
// story's maps read every lot lamp again, with nothing edited
struct LampNow {
    bool read = false, drawn = false, law = false;
    GpuLampLaw::Lamp w;
};
std::unordered_map<uintptr_t, LampNow> g_lampNow;
// A completed native solve with no remaining send is an explicit fallback, not queued GPU work.
bool RestartSolveTerminal(const Ask& ask, int state, bool budgetOut) {
    return (state == 4 || state == 5) && (budgetOut || (ask.count >= kSolveAsks && ask.logged));
}
bool AskDue(const Ask& ask, DWORD now, DWORD askMs = kSolveAskMs) { return !ask.at || now - ask.at > (ask.count < kSolveAsks ? askMs : kSolveRetryMs); }
bool g_restartGpuPending = false, g_restartGpuFallback = false;
std::unordered_map<const void*, Ask> g_askedSolve; // room -> its sends to its solve for a kept solve
std::unordered_map<const IDirect3DBaseTexture9*, Slot*> g_swap; // the game's map -> its slot (the SetTexture swap; render thread)
// (render thread) the relit copies of the lots left at active lot switches: kept bound (frozen, not relit) while the game
// does not write those maps again (a lamp switched there, the day's light), for up to kRetiredLots lots, the least recently
// left dropped first. A lot left does not fall back to the game's maps at the switch, nor a lot visited again while its
// own maps are prepared: with Instant on, the lots seen keep the GPU's light
struct Retired {
    uintptr_t lot = 0; // the tracker it was the active lot of
    DWORD at = 0;      // when it was left
    std::unique_ptr<Slot> s;
};
std::vector<Retired> g_retired;
std::unordered_map<const IDirect3DBaseTexture9*, Slot*> g_swapRetired;
uintptr_t g_retiredLo = ~uintptr_t{0}, g_retiredHi = 0;
constexpr size_t kRetiredLots = 3;
// (any thread: the game's map unlock) the retired copies' game maps, and those the game wrote since (their copies are stale)
std::mutex g_retiredMx;
std::unordered_set<const void*> g_retiredGame;
std::vector<const void*> g_retiredStale;
std::atomic<bool> g_retiredAny{false};
// (the 10 s summary, render thread) the frames of the maps with texels over open rooms of the story below: their rooms read now,
// the last binding kept (why: a room's ambient, a room's map, the UV map), none (the draw shows the texels' carried light), a switch
// between the relit copy and the game's map, a room showing another class than the one matched, a new kept solve not yet prepared
struct InSum {
    long frames = 0, now = 0, keptAmb = 0, keptMap = 0, keptUv = 0, none = 0, toggles = 0, otherClass = 0, lag = 0;
};
InSum g_inSum;
// (render thread writes, the prepare's worker reads) the class each room of a story below an attic shows (+0x100): its texels are
// matched in that class's map, the one its draw and the attic read
std::mutex g_shownMx;
std::map<std::tuple<uint64_t, int, int>, int> g_shownCls;
int ShownOf(uint64_t lot, int story, int id) {
    std::lock_guard<std::mutex> lk(g_shownMx);
    const auto it = g_shownCls.find(std::make_tuple(lot, story, id));
    return it != g_shownCls.end() ? it->second : -1;
}
uintptr_t g_swapLo = ~uintptr_t{0}, g_swapHi = 0;               // the address range of its keys (most binds end at this test)
uint64_t g_swapSig = 0;
uint64_t g_lampSigAll = 0;
DWORD g_lampChangedAt = 0;
size_t g_rr = 0;                 // the first slot looked at (round robin over frames)
long g_pinDrops = 0;
bool g_budgetOut = false;        // the active lot's kept parts do not fit the memory budget (logged once)
int g_coverLogged = -1;          // the coverage last logged (0 incomplete, 1 every story)
double g_frameMsSum = 0;         // Frame's own CPU time a frame (status: its running average)
long g_frameCount = 0;
struct Phases {
    double slots = 0, lamps = 0, mask = 0, draw = 0, shadows = 0, over = 0, rooms = 0; // (over: of draw, the kept rooms' ComposeOver; rooms: GpuRoomLamps::Sig)
    double maskJobs = 0, maskShare = 0, maskLock = 0, maskFill = 0; // the floor mask's parts (render thread)
};
bool g_roundOpen = false;        // a relight spread over frames is under way
double g_roundCpuMs = 0;         // its CPU time so far
int g_roundMaps = 0, g_roundFrames = 0, g_roundTiles = 0, g_roundNb = 0; // (g_roundNb: of the maps, those of the lots next to it)
std::vector<int> g_roundStories;
// What made maps relit, and the frame's own cost, over the last 10 s (the self-measure line; render thread)
struct Why {
    long first = 0, lamp = 0, base = 0, params = 0, occluders = 0, gameWrite = 0, exact = 0, build = 0, writeGap = 0;
    long partial = 0; // (rooms-only copies relit by their changed rooms alone)
    long relights = 0;
    double frameMs = 0, frameMax = 0;
    double nbMs = 0, loopMs = 0; // of frameMs: the lots next to it, the relight loop (its checks of every map included)
    long frames = 0;
    DWORD since = 0;
    long gpuFrames = 0, deferred = 0; // relight frames timed on the GPU; maps left to the next frame by the GPU budget
    double gpuMax = 0;
};
Why g_why;
// (the relight's GPU parts, timed by timestamps between the parts in a frame whose queries are free; the Cost line)
enum GpuPart { kGpRoom0, kGpOutdoor, kGpCompose, kGpCarry, kGpRooms, kGpHi, kGpOther, kGpParts };
double g_gpuPart[kGpParts] = {};
constexpr int kMarks = 256;
IDirect3DQuery9* g_qMark[kMarks] = {};
int g_markPart[kMarks] = {}, g_marks = 0;
const void* g_markSlot[kMarks] = {};
bool g_marking = false; // (this frame's relight is timed)
std::string GpuParts();
void Mark(IDirect3DDevice9* dev, GpuPart part, const void* slot) {
    if (!g_marking || g_marks >= kMarks) return;
    if (!g_qMark[g_marks] && (FAILED(dev->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &g_qMark[g_marks])) || !g_qMark[g_marks])) {
        g_qMark[g_marks] = nullptr, g_marking = false;
        return;
    }
    g_qMark[g_marks]->Issue(D3DISSUE_END);
    g_markPart[g_marks] = part, g_markSlot[g_marks] = slot, g_marks++;
}
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
std::shared_ptr<const Proxy> g_openUsed; // (physical indoor lamps) the proxy whose open-top build is the kept atlas's occluders
std::atomic<uint32_t> g_openGen{0};      // its generation (0: none uploaded)
std::atomic<uint32_t> g_stairUploads{0}; // uploads made for a grown stair set (KeptMeshGen)
std::vector<std::pair<int, int>> g_openRooms; // (render thread) the uploaded open-top build's openRooms (RoomOpenAbove)
std::shared_ptr<const std::vector<RtLampProxy::Inside>> g_inside; // (render thread) its spaces the open rooms continue into (InsideOf)
bool ReadRoomAmbient(const void* room, float* c); // (below)
void InRoomsNow(IDirect3DDevice9* dev, Slot& s, uintptr_t tracker); // (below)
DWORD g_openFailAt = 0;                  // (its last upload failed then)
uint64_t g_atlasKey = 0;
uint64_t g_lastParams = 0;
DWORD g_frame = 0, g_lastFrame = 0;
std::vector<uintptr_t> g_stampLights; // this frame's lamps of the lot, for TerrainStamps (none when Frame stops before it reads them)
bool g_installed = false;
IDirect3DDevice9* g_dev = nullptr; // the game's device (render thread)
int g_ps3 = -1; // the device runs shader model 3 (1), not (0), not read yet (-1) (render thread)
int g_mrt = -1; // the device draws into two render targets at once (the outdoor scale), not (0), not read yet (-1)
DWORD g_makeFailAt = 0; // (render thread) the shader objects' last failed making
int g_makeFails = 0;     // and how many in a row (5: tried once a minute from then on; it stopped for the session, and only a device reset brought it back)
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

void ReleaseHi(Slot& s) {
    if (s.hi) s.hi->Release(), s.hi = nullptr;
    if (s.hiOver) s.hiOver->Release(), s.hiOver = nullptr;
    if (s.hiRoom0) s.hiRoom0->Release(), s.hiRoom0 = nullptr;
    s.hiOk = false;
}
void ReleaseHiPoints(Slot& s) {
    for (IDirect3DVertexBuffer9** vb : {&s.vbHi, &s.pvbHi, &s.placesHi})
        if (*vb) (*vb)->Release(), *vb = nullptr;
    s.nHi = 0;
}
void ReleaseSlot(Slot& s) {
    if (s.watch >= 0) g_watchTex[s.watch].store(nullptr), s.watch = -1;
    if (s.copy) s.copy->Release(), s.copy = nullptr;
    ReleaseHi(s);
    s.partKey = 0;
    if (s.scale) s.scale->Release(), s.scale = nullptr;
    s.scaled = false, s.inFlags = false;
    if (s.vb) s.vb->Release(), s.vb = nullptr;
    if (s.dyn) s.dyn->Release(), s.dyn = nullptr;
    if (s.dynNext) s.dynNext->Release(), s.dynNext = nullptr;
    if (s.preblur) s.preblur->Release(), s.preblur = nullptr;
    if (s.kvb) s.kvb->Release(), s.kvb = nullptr;
    if (s.pvb) s.pvb->Release(), s.pvb = nullptr;
    ReleaseHiPoints(s);
    if (s.game) s.game->Release(), s.game = nullptr;
    if (s.inUv) s.inUv->Release(), s.inUv = nullptr;
    s.inUvBuilt.clear();
    if (s.inBase) s.inBase->Release(), s.inBase = nullptr;
    for (IDirect3DTexture9*& g : s.inGame)
        if (g) g->Release(), g = nullptr;
    s.inAmbOk = false;
    s.map = RtLampGpu::Map{};
}
// Every stage that holds a copy takes the game's map back (render thread)
void UnbindCopies(IDirect3DDevice9* dev) {
    if (!dev) return;
    for (DWORD st = 0; st < 16; st++) {
        IDirect3DBaseTexture9* t = nullptr;
        if (FAILED(dev->GetTexture(st, &t)) || !t) continue;
        for (const auto& s : g_slots)
            if ((s->copy && t == s->copy) || (s->hi && t == s->hi)) D3D9Hooks::CallOriginalSetTexture(dev, st, s->game);
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
void RebuildRetired() {
    g_swapRetired.clear();
    g_retiredLo = ~uintptr_t{0}, g_retiredHi = 0;
    std::lock_guard<std::mutex> lk(g_retiredMx);
    g_retiredGame.clear();
    for (const Retired& r : g_retired) {
        g_swapRetired[r.s->game] = r.s.get();
        g_retiredGame.insert(r.s->game);
        const uintptr_t a = reinterpret_cast<uintptr_t>(r.s->game);
        g_retiredLo = std::min(g_retiredLo, a), g_retiredHi = std::max(g_retiredHi, a);
    }
    g_retiredAny.store(!g_retired.empty(), std::memory_order_relaxed);
}
// Releases the retired copies `drop` picks (their stages take the game's maps back) and logs them with why
template <typename Pred> void ReleaseRetiredIf(IDirect3DDevice9* dev, Pred drop, const char* why) {
    if (std::none_of(g_retired.begin(), g_retired.end(), [&](const Retired& r) { return drop(r); })) return;
    if (dev)
        for (DWORD st = 0; st < 16; st++) {
            IDirect3DBaseTexture9* t = nullptr;
            if (FAILED(dev->GetTexture(st, &t)) || !t) continue;
            for (const Retired& r : g_retired)
                if (drop(r) && (t == r.s->copy || (r.s->hi && t == r.s->hi))) D3D9Hooks::CallOriginalSetTexture(dev, st, r.s->game);
            t->Release();
        }
    size_t n = 0;
    std::unordered_set<uintptr_t> lots;
    for (auto it = g_retired.begin(); it != g_retired.end();)
        if (drop(*it)) {
            lots.insert(it->lot);
            ReleaseSlot(*it->s);
            it = g_retired.erase(it), n++;
        } else
            ++it;
    RebuildRetired();
    LOG_INFO(std::format("[RtOutdoorLamps] {} relit maps of {} lot{} left released ({}); {} kept", n, lots.size(), lots.size() == 1 ? "" : "s", why, g_retired.size()));
}
void ReleaseRetired(IDirect3DDevice9* dev, const char* why) {
    ReleaseRetiredIf(dev, [](const Retired&) { return true; }, why);
}
// The lot left at a switch (lot: its tracker): its relit copies stay bound (g_swapRetired), the rest of its slots are released.
// Only kRetiredLots lots are kept: the least recently left goes first
void RetireSlots(IDirect3DDevice9* dev, uintptr_t lot) {
    const DWORD now = std::max<DWORD>(GetTickCount(), 1);
    for (auto& s : g_slots) {
        if (!(s->copy && s->relit && s->game)) {
            ReleaseSlot(*s);
            continue;
        }
        if (s->watch >= 0) g_watchTex[s->watch].store(nullptr), s->watch = -1;
        for (auto it = g_retired.begin(); it != g_retired.end(); ++it) // (an older copy of the same map: the newer one stands in)
            if (it->s->game == s->game) {
                ReleaseSlot(*it->s);
                g_retired.erase(it);
                break;
            }
        g_retired.push_back({lot, now, std::move(s)});
    }
    g_slots.clear();
    std::vector<std::pair<DWORD, uintptr_t>> lots;
    for (const Retired& r : g_retired) {
        auto l = std::find_if(lots.begin(), lots.end(), [&](const auto& x) { return x.second == r.lot; });
        if (l == lots.end()) lots.push_back({r.at, r.lot});
        else l->first = std::max(l->first, r.at);
    }
    RebuildRetired();
    if (lots.size() > kRetiredLots) {
        std::sort(lots.begin(), lots.end());
        std::unordered_set<uintptr_t> old;
        for (size_t i = 0; i + kRetiredLots < lots.size(); i++) old.insert(lots[i].second);
        ReleaseRetiredIf(dev, [&](const Retired& r) { return old.count(r.lot) != 0; }, "the least recently left");
    }
}
// Render thread, each frame: retired copies whose game map the game wrote (stale), or whose lot the game no longer has
void ServiceRetired(IDirect3DDevice9* dev) {
    if (g_retired.empty()) return;
    std::unordered_set<const void*> stale;
    {
        std::lock_guard<std::mutex> lk(g_retiredMx);
        stale.insert(g_retiredStale.begin(), g_retiredStale.end());
        g_retiredStale.clear();
    }
    if (!stale.empty()) ReleaseRetiredIf(dev, [&](const Retired& r) { return stale.count(r.s->game) != 0; }, "the game wrote its maps again");
    static DWORD s_liveAt = 0;
    if (GetTickCount() - s_liveAt > 1000) {
        s_liveAt = GetTickCount();
        ReleaseRetiredIf(dev, [](const Retired& r) { return !LevelLightShare::TrackerLive(r.lot); }, "its lot is no longer loaded");
    }
}
// (developer) GPU against the game: every 10 s one relit map of the active lot is read back and compared texel by texel with
// the game's own map of the same room 0, as its last solve wrote it (the copy is drawn instead of it; that the solve had the
// same lamps on is not verified: a lamp edit since the solve differs by design). Where
// the game's texel holds light, the copy's luma over the game's, and the texels at the top of the range on each side: the
// data a calibration of the GPU lamps against the game's solve starts from. Render thread; a short GPU wait (one map).
void CompareWithGame(IDirect3DDevice9* dev) {
    static DWORD s_last = 0;
    static size_t s_next = 0;
    const DWORD now = GetTickCount();
    if (kPublicBuild || !dev || g_slots.empty() || now - s_last < 10000) return;
    s_last = now;
    Slot* s = nullptr;
    for (size_t i = 0; i < g_slots.size() && !s; i++) {
        Slot* c = g_slots[(s_next + i) % g_slots.size()].get();
        if (c->copy && c->game && c->relit && !c->mgr) s = c, s_next = (s_next + i + 1) % g_slots.size();
    }
    if (!s) return;
    D3DSURFACE_DESC dc{}, dg{};
    if (FAILED(s->copy->GetLevelDesc(0, &dc)) || FAILED(s->game->GetLevelDesc(0, &dg)) || dc.Width != dg.Width || dc.Height != dg.Height ||
        dc.Format != D3DFMT_A8R8G8B8 || dg.Format != D3DFMT_A8R8G8B8)
        return;
    IDirect3DSurface9 *rt = nullptr, *sys = nullptr;
    if (FAILED(s->copy->GetSurfaceLevel(0, &rt))) return;
    if (FAILED(dev->CreateOffscreenPlainSurface(dc.Width, dc.Height, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, nullptr)) || FAILED(dev->GetRenderTargetData(rt, sys))) {
        if (sys) sys->Release();
        rt->Release();
        return;
    }
    rt->Release();
    D3DLOCKED_RECT lc{}, lg{};
    if (FAILED(sys->LockRect(&lc, nullptr, D3DLOCK_READONLY))) return void(sys->Release());
    if (FAILED(s->game->LockRect(0, &lg, nullptr, D3DLOCK_READONLY))) {
        sys->UnlockRect();
        return void(sys->Release());
    }
    const auto luma = [](const BYTE* p) { return 0.0722 * p[0] + 0.7152 * p[1] + 0.2126 * p[2]; };
    std::vector<double> ratios;
    long lit = 0, topCopy = 0, topGame = 0;
    double sumC = 0, sumG = 0;
    for (UINT y = 0; y < dc.Height; y++) {
        const BYTE* rc = static_cast<const BYTE*>(lc.pBits) + y * lc.Pitch;
        const BYTE* rg = static_cast<const BYTE*>(lg.pBits) + y * lg.Pitch;
        for (UINT x = 0; x < dc.Width; x++) {
            const double g = luma(rg + 4 * x), c = luma(rc + 4 * x);
            if (g < 8.0) continue; // (no light the game put there worth a ratio)
            lit++, sumC += c, sumG += g;
            ratios.push_back(c / g);
            topCopy += std::max({rc[4 * x], rc[4 * x + 1], rc[4 * x + 2]}) >= 254;
            topGame += std::max({rg[4 * x], rg[4 * x + 1], rg[4 * x + 2]}) >= 254;
        }
    }
    // a map that differs (p90 over 1.3 or p10 under 0.7): both written as BMP next to the log once, for a look at where
    std::vector<double> sorted = ratios;
    std::sort(sorted.begin(), sorted.end());
    static int s_dumps = 0;
    if (!sorted.empty() && s_dumps < 12 && (sorted[sorted.size() * 9 / 10] > 1.3 || sorted[sorted.size() / 10] < 0.7)) {
        s_dumps++;
        const auto bmp = [&](const char* tag, const D3DLOCKED_RECT& lr) {
            const std::wstring dir = ApexPaths::ApexDirectory();
            const std::wstring path = dir + std::format(L"\\calib_{}_s{}_m{}_c{}_{}.bmp", s_dumps, s->story, s->m, s->cls, std::wstring(tag, tag + std::strlen(tag)));
            FILE* f = _wfopen(path.c_str(), L"wb");
            if (!f) return;
            const uint32_t w = dc.Width, h = dc.Height, row = w * 3, size = 54 + row * h;
            uint8_t hd[54] = {'B', 'M'};
            std::memcpy(hd + 2, &size, 4);
            const uint32_t off = 54, hs = 40, bits = row * h;
            const uint16_t planes = 1, bpp = 24;
            const int32_t iw = static_cast<int32_t>(w), ih = -static_cast<int32_t>(h);
            std::memcpy(hd + 10, &off, 4), std::memcpy(hd + 14, &hs, 4), std::memcpy(hd + 18, &iw, 4), std::memcpy(hd + 22, &ih, 4);
            std::memcpy(hd + 26, &planes, 2), std::memcpy(hd + 28, &bpp, 2), std::memcpy(hd + 34, &bits, 4);
            fwrite(hd, 1, 54, f);
            std::vector<uint8_t> line(row);
            for (uint32_t y = 0; y < h; y++) {
                const BYTE* src = static_cast<const BYTE*>(lr.pBits) + y * lr.Pitch;
                for (uint32_t x = 0; x < w; x++) line[3 * x] = src[4 * x], line[3 * x + 1] = src[4 * x + 1], line[3 * x + 2] = src[4 * x + 2];
                fwrite(line.data(), 1, row, f);
            }
            fclose(f);
        };
        bmp("gpu", lc);
        bmp("game", lg);
        LOG_INFO(std::format("[RtOutdoorLamps] GPU against the game: maps written as calib_{}_s{}_m{}_c{}_gpu.bmp / _game.bmp", s_dumps, s->story, s->m, s->cls));
    }
    s->game->UnlockRect(0);
    sys->UnlockRect();
    sys->Release();
    if (ratios.empty()) return;
    std::sort(ratios.begin(), ratios.end());
    const auto pct = [&](double q) { return ratios[std::min(ratios.size() - 1, static_cast<size_t>(q * ratios.size()))]; };
    LOG_INFO(std::format("[RtOutdoorLamps] GPU against the game: story {} {} class {} ({}x{}): {} lit texels, copy over game luma: mean {:.2f}, "
                         "p10 {:.2f}, median {:.2f}, p90 {:.2f}; at the top of the range: copy {} texels, game {}",
                         s->story, kMapName[s->m], s->cls, dc.Width, dc.Height, lit, sumG > 0 ? sumC / sumG : 0.0, pct(0.1), pct(0.5), pct(0.9), topCopy, topGame));
}
// ---- the lots next to the active one (render thread) ----
// Their room-0 maps are relit with the active lot's lamp posts: their gathers leave those posts out (LevelLightShare::GatherLotId)
// and their kept solves are the base alone (OutdoorDelta::SetNeighbourLots). A story the GPU cannot relight gathers them again.
struct Neighbour {
    uint64_t lot = 0;
    uintptr_t tracker = 0;
    uint8_t cpu = 0; // its stories the GPU cannot relight (their gathers take the posts)
};
std::vector<Neighbour> g_nb; // nearest first
uint64_t g_nbActive = 0;
// A story of one of them: its lamps of the terrain bake, for its outdoor floors' under-floor share (as the active lot's
// stories: bake, keys, signature, lot base)
struct NbBake {
    std::vector<RtLampModel::BakeLamp> bake;
    std::vector<uint64_t> keys;
    uint64_t sig = 0;
    float base = 0.0f;
    DWORD at = 0;         // read last (0: never)
    uint32_t kept = 0;    // with this kept solve's version
    uint64_t lampSig = 0; // and the active lot's lamps (Frame's allSig)
};
std::unordered_map<uintptr_t, NbBake> g_nbBake; // story manager ->
void SendNeighbours() {
    uint64_t lots[kMaxNeighbours];
    uint8_t cpu[kMaxNeighbours];
    const int n = static_cast<int>(std::min<size_t>(g_nb.size(), kMaxNeighbours));
    for (int i = 0; i < n; i++) lots[i] = g_nb[i].lot, cpu[i] = g_nb[i].cpu;
    LevelLightShare::SetGpuNeighbourLots(g_nbActive, lots, cpu, n);
    OutdoorDelta::SetNeighbourLots(lots, n);
}
// The lots next to `active` relit now (nearest first); a lot joining or leaving gathers again (regather false: a paced restart
// solves every room anyway)
void PublishNeighbours(uint64_t active, const std::vector<std::pair<uint64_t, uintptr_t>>& want, bool regather = true) {
    // (a lot is the lot and its tracker: a lot loaded again under the same id is a new one, its old tracker dead)
    bool same = active == g_nbActive && want.size() == g_nb.size();
    for (size_t i = 0; same && i < want.size(); i++) same = want[i].first == g_nb[i].lot && want[i].second == g_nb[i].tracker;
    if (same) return;
    const auto had = [](const std::vector<Neighbour>& v, const Neighbour& m) {
        return std::any_of(v.begin(), v.end(), [&m](const Neighbour& n) { return n.lot == m.lot && n.tracker == m.tracker; });
    };
    std::vector<Neighbour> next;
    for (const auto& [lot, tracker] : want) {
        const auto it = std::find_if(g_nb.begin(), g_nb.end(), [lot, tracker](const Neighbour& n) { return n.lot == lot && n.tracker == tracker; });
        next.push_back(it != g_nb.end() && active == g_nbActive ? *it : Neighbour{lot, tracker, 0});
    }
    std::vector<Neighbour> moved;
    for (const auto& n : g_nb)
        if (active != g_nbActive || !had(next, n)) moved.push_back(n);
    // (the lots leaving show the game's maps again at once, gathered without the active lot's posts: urgent; a lot joining keeps
    // the game's light until its relit copy shows the posts, so it goes at the game's pace)
    const size_t leavers = moved.size();
    for (const auto& n : next)
        if (active != g_nbActive || !had(g_nb, n)) moved.push_back(n);
    g_nb = std::move(next);
    g_nbActive = active;
    SendNeighbours();
    int sent = 0;
    for (size_t i = 0; i < moved.size(); i++)
        if (regather && LevelLightShare::TrackerLive(moved[i].tracker)) sent += LevelLightShare::RegatherOutdoorById(moved[i].lot, i < leavers) ? 1 : 0;
    if (!moved.empty() && LogAllowed()) {
        std::string list;
        for (const auto& n : g_nb) list += std::format("{}{:08X}", list.empty() ? "" : " ", static_cast<uint32_t>(n.lot));
        LOG_INFO(std::format("[RtOutdoorLamps] Lots next to the active one relit with its lamp posts: {} ({} lot{} gather their outdoor rooms again)",
                             list.empty() ? std::string("none") : list, sent, sent == 1 ? "" : "s"));
    }
}
struct Job;
std::vector<std::shared_ptr<Job>> g_jobs; // render thread: the stories whose maps are being prepared (a worker thread each, one at a time)
void DropAll(IDirect3DDevice9* dev, const char* why, bool requeue = true, bool retire = false) {
    if (!retire) ReleaseRetired(dev, why);
    PublishNeighbours(0, {}, requeue); // (their maps are the game's again: their gathers take the posts back)
    if (requeue) OutdoorDelta::SolveTouched(nullptr); // (and the rooms with physical lamp edits taken since their solve go to one)
    g_jobs.clear(); // (a job still running finishes on its own)
    g_swap.clear();
    g_swapLo = ~uintptr_t{0}, g_swapHi = 0;
    g_swapSig = 0;
    OutdoorDelta::PinRooms(nullptr, 0); // (no relit map stands for a kept room any more: the budget pins none)
    GpuRoomLamps::ReleaseAll();
    if (g_slots.empty() && !g_eng) return;
    const size_t n = g_slots.size();
    if (retire) RetireSlots(dev, g_tracker); // (its copies stay bound while the game does not write those maps)
    else {
        UnbindCopies(dev);
        for (auto& s : g_slots) ReleaseSlot(*s);
        g_slots.clear();
    }
    for (auto& d : g_watchDirty) d.store(0);
    g_atlasKey = 0;
    {
        std::lock_guard<std::mutex> lk(g_coverMx);
        g_cover = Cover{};
    }
    // the game's room-0 maps never took the edits the GPU stood in for: each story's room 0 of that lot is solved again
    // (only while the game still has that lot: after a world change or a lot's teardown its tracker is gone). Urgent only
    // when the GPU took an edit on that lot (its maps lack it); with none they hold the right light and go at the game's pace:
    // as urgent rooms of the lot just left they opened an 8 s "lamp edit" of room solves with no edit
    int sent = 0;
    const bool edited = g_tracker && g_tookEdit.exchange(0) == g_tracker;
    if (requeue && n && g_tracker && LevelLightShare::TrackerLive(g_tracker))
        for (int S = 0; S <= 7; S++)
            if (LevelLightShare::Room0Of(g_tracker, S)) sent += LevelLightShare::QueueRoom0(g_tracker, S, edited) ? 1 : 0;
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
// The floor mask's "all" (more lamps than one batch): the GPU lamps' luminance with no walls at n masked points, each
// `stride` floats after the last (x, y, z first). The render thread and AllJob's worker run this same code
void AllMask(const std::vector<RtLampModel::Lamp>& lamps, const float* pos, size_t stride, size_t n, float thr, std::vector<float>& out) {
    out.assign(n, 0.0f);
    for (size_t i = 0; i < n; i++) {
        float vis = 0;
        for (const auto& l : lamps) RtLampModel::AddMask(l, &pos[i * stride], 1.0f, thr, vis, out[i]);
    }
}
// The floating-point state the floats depend on, as in dxt_codec.cpp: the x87 control word and MXCSR (rounding, FTZ, DAZ).
// AllJob's worker computes with the render thread's, so its values are the ones that thread would compute
uint16_t X87Cw() {
    uint16_t cw;
    __asm fnstcw cw
    return cw;
}
void SetFp(uint16_t x87, unsigned mxcsr) {
    uint16_t cw = x87;
    __asm {
        fnclex
        fldcw cw
    }
    _mm_setcsr(mxcsr & ~0x3Fu); // control bits only; the sticky exception flags start clear
}
// What the worker reads, copied when it starts (the lamps, the points' x, y, z, the threshold, the render thread's
// floating-point state), and Frame's allSig for them
struct AllJob {
    std::vector<RtLampModel::Lamp> lamps;
    std::vector<float> pos;
    float thr = 0.0f;
    uint16_t x87 = 0;
    unsigned mxcsr = 0;
    uint64_t sig = 0;
    std::atomic<int> state{1}; // 1 running, 2 done
    std::vector<float> vals;
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
    // 4 texels at a time (BakeStamp4, bit for bit BakeStamp's); each texel's sums take the changed lamps in the same order
    // as before, so they are the same doubles (about 1.6 ms a relight on the render thread in a recording, one texel at a time)
    if (!ch.empty())
        for (size_t i0 = 0; i0 < s.gated.size(); i0 += 4) {
            const size_t m = std::min<size_t>(4, s.gated.size() - i0);
            float sx[4], sy[4], sz[4];
            bool any = false;
            for (size_t k = 0; k < 4; k++) {
                const float* p = &s.maskedPos[(i0 + std::min(k, m - 1)) * 6 + 3]; // (a short last block repeats its last texel)
                sx[k] = p[0], sy[k] = p[1], sz[k] = p[2];
                any = any || (k < m && s.gated[i0 + k]);
            }
            if (!any) continue;
            for (const auto& [L, sign] : ch) {
                float v[4];
                bool u[4], ok[4];
                RtLampModel::BakeStamp4(L, sx, sy, sz, s.uBase, kUnderEps, v, u, ok);
                for (size_t k = 0; k < m; k++) {
                    const size_t i = i0 + k;
                    if (!s.gated[i] || !ok[k]) continue;
                    s.uA[i] += sign * v[k];
                    if (u[k]) s.uU[i] += sign * v[k];
                }
            }
            for (size_t k = 0; k < m; k++) {
                const size_t i = i0 + k;
                if (s.gated[i]) s.under[i] = s.uA[i] > 1e-4 ? static_cast<float>(std::clamp(s.uU[i] / s.uA[i], 0.0, 1.0)) : -1.0f;
            }
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
    std::vector<InUv> inUv; // (physical) its texels matched to a texel of their room's wall map (RtLampProxy::TopMatch)
    // and each one's light without the GPU lamps as carried now (rx, ry 0xFFFF: a floor texel, its room's top-row mean): InBaseNow
    std::vector<std::array<float, 3>> inRgb;
    std::vector<float> under;  // the floor mask's under-floor share (exact) and its sums
    std::vector<double> uA, uU;
    std::vector<RtLampGpu::TexelVertex> pv; // (physical indoor lamps) one phys point per place: its last texel's (the sum's and compose's)
    // (physical, sharper floors) room 0's floor at twice the map's size: each unmasked floor texel's 4 sub-texels, their compose
    // points, phys points and places (CopyAtPS) in the 2w x 2h map (rooms > 0: OutdoorDelta::BuildKeptMap's physHi)
    std::vector<RtLampGpu::TexelVertex> vHi, pvHi, placesHi;
    float pBox[6] = {1e30f, 1e30f, 1e30f, -1e30f, -1e30f, -1e30f}; // (physical, stage 4) their world box (min xyz, max xyz)
    std::vector<RtLampGpu::TexelVertex> kv; // (GPU room lamps) room 0's kept lamps' parts, by lamp
    std::vector<std::pair<UINT, UINT>> kRanges;
    std::vector<KeptInfo> kLamps;
    bool kX = false;
};
constexpr size_t kHiRoom0Texels = 16384; // (sharper floors) room 0's floor texels a map takes at most: 4 x 3 points of 80 B, 15 MB
struct Job {
    const void* room = nullptr;
    int story = 0, cls = 0;
    int lot = 0; // 1: a lot next to the active one
    uint32_t ver = 0;
    RoomNow rn;
    Xform space{}; // the texels' space: the active lot's (rn.xf for its own stories)
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
    // (physical indoor lamps) asked of the export (phys), and what it gave: the lamps out of the base and their rows, room 0's
    // 3D test records; physLater: not asked only because the physical draw was not ready; physWhy: why not drawn physically
    bool phys = false, physLater = false;
    std::string physWhy;
    std::vector<uintptr_t> physLights;
    std::vector<std::array<float, 3>> physRow;
    std::shared_ptr<const RtLampGpu::BlockerTable> blk;
    float blkHeader = -1.0f;
    // (physical indoor lamps) the open-top build's lot and its open rooms when the job started: room 0 texels inside a room open
    // below take no outdoor lamp (RtLampProxy::OverOpenBelow, bit 8 of the texel flags)
    std::shared_ptr<const Proxy> open;
    std::vector<std::pair<int, int>> openRooms;
    // (physical) the spaces open rooms of the story below continue into (RtLampProxy::InsideAt), and the rooms its texels took,
    // in their slot order (mask.z; at most 4: Params::inFit)
    std::shared_ptr<const std::vector<RtLampProxy::Inside>> inside;
    std::vector<int> inRooms;
    uint32_t inVer[4] = {}; // the kept solve of each whose top row its texels were matched to (OutdoorDelta::IndoorTopOf)
    int inCls[4] = {-1, -1, -1, -1}; // its class then (-1: none matched)
};
void PrepareJob(Job& j) {
    struct InStat {
        long n = 0;
        double sum[3] = {};
        float max = 0.0f;
    } inStat[2]; // (walls, floors) of the spaces rooms of the story below continue into: their light without lamps, for the log
    long inCarried = 0;
    // (physical) the top row of the walls of each room the job's texels continue (its slot: Job::inRooms), read once a job
    using TopTexel = OutdoorDelta::TopTexel;
    std::vector<TopTexel> tops[4];
    bool topRead[4] = {};
    // (a room whose kept solve is gone for a while, a geometry change or the budget: its last top row, as its map keeps that
    // layout, so the space above never drops to its own solve)
    struct LastTop {
        std::vector<TopTexel> top;
        int cls;
    };
    static std::mutex s_lastTopMx;
    static std::map<std::tuple<uint64_t, int, int>, LastTop> s_lastTop;
    const auto TopOf = [&](size_t r) -> const std::vector<TopTexel>& {
        if (r < 4 && !topRead[r] && r < j.inRooms.size()) {
            topRead[r] = true;
            const auto key = std::make_tuple(j.rn.lot, j.story - 1, j.inRooms[r]);
            j.inCls[r] = ShownOf(j.rn.lot, j.story - 1, j.inRooms[r]); // (the class the room shows: its map's layout; -1 not seen yet)
            std::lock_guard<std::mutex> lk(s_lastTopMx);
            if (OutdoorDelta::IndoorTopOf(j.rn.lot, j.story - 1, j.inRooms[r], tops[r], j.inVer[r], j.inCls[r])) {
                if (s_lastTop.size() > 256) s_lastTop.clear();
                s_lastTop[key] = {tops[r], j.inCls[r]};
            } else if (const auto it = s_lastTop.find(key); it != s_lastTop.end()) { // (inVer stays 0: the next kept solve prepares it again)
                tops[r] = it->second.top, j.inCls[r] = it->second.cls;
            } else
                tops[r].clear(), j.inCls[r] = -1;
        }
        return tops[r < 4 ? r : 0];
    };
    const bool groundShade = LevelLightShare::GroundShadeOn();
    LARGE_INTEGER a{}, b{};
    QueryPerformanceCounter(&a);
    OutdoorDelta::Room0Export ex;
    if (!OutdoorDelta::ExportRoom0(j.room, j.cls, ex, j.phys)) {
        j.state.store(3);
        return;
    }
    if (j.phys && !ex.physical) j.physWhy = ex.physWhy ? ex.physWhy : "?";
    // (stage 4: with no lamp out of the base too: a lamp moved, switched on or new near it is drawn there from its law)
    j.phys = ex.physical;
    if (j.phys) {
        j.physLights = std::move(ex.phys), j.physRow = std::move(ex.physRow), j.blkHeader = ex.blkHeader;
        j.blk = ex.blockers ? std::move(ex.blockers) : std::make_shared<const RtLampGpu::BlockerTable>(); // (no records read: none)
    }
    j.version = ex.version, j.thr = ex.thr, j.dither = ex.dither;
    j.kept = std::move(ex.kept), j.drawn = std::move(ex.drawn);
    // shortcut: kept (Replica) parts name the solve's texels, so such a map keeps the steps along diagonal walls; give the copies parts if it matters
    if (ex.cls == 2 && ex.mapUsed[1] && ex.kept0.empty())
        PhysLampPick::AddFloorGridTwins(ex.texels, 1, LightDetail::Active() ? 8 : 4, static_cast<int>(ex.mapW[1]), static_cast<int>(ex.mapH[1]),
                                        [&](const OutdoorDelta::Room0Export::Texel& t, float out[3]) { ToLot(j.space, t.pos, out); });
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
        // (physical, sharper floors) room 0's unmasked floor texels: their compose and phys points, lot and world samples (position,
        // then the normal at [4..6]: PhysLampPick::SubTexels)
        struct HiSrc {
            RtLampGpu::TexelVertex v, q;
            float lot[8], world[8];
        };
        std::vector<HiSrc> hiSrc;
        const bool hi = m == 1 && j.phys && !p.blur && OutdoorDelta::HiFloors() && 2 * p.w <= 4096 && 2 * p.h <= 4096;
        for (const OutdoorDelta::Room0Export::Texel& t : ex.texels) {
            if (t.map != m) continue;
            RtLampGpu::TexelVertex tv{};
            tv.x = t.x, tv.y = t.y, tv.w = 1.0f;
            ToLot(j.space, t.pos, tv.p);
            float n[3];
            DirToLot(j.space, t.nrm, n);
            const float nl = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            for (int k = 0; k < 3; k++) tv.n[k] = nl > 1e-6f ? n[k] / nl : 0.0f;
            tv.p[3] = static_cast<float>(t.bits & 3);
            // (15/10) the lot's ground on story 0 (turned up, within 1 m of the lot's ground, not a masked floor): bit 4, no wall
            // test on the GPU; the lot passes shade it with the smooth ground shade (unverified: lot space y 0 = the lot's ground;
            // a lot next to the active one: its own ground, and its lot pass's shade takes the active lot's posts too, unverified)
            float own[3] = {0.0f, tv.p[1], 0.0f};
            if (j.lot > 0) ToLot(j.rn.xf, t.pos, own);
            int inRoom = 0;
            float inSlot = -1.0f;
            if (groundShade && j.story == 0 && !(t.bits & 1) && tv.n[1] > 0.9f && std::fabs(own[1]) < 1.0f) tv.p[3] += 4.0f;
            // (physical) a wall of a double-height room's upper half is that room's, not outdoors: no outdoor lamp (AccPS bit 8)
            else if (j.inside && (inRoom = RtLampProxy::InsideAt(*j.inside, j.story, tv.p, tv.n)) > 0) {
                auto at = std::find(j.inRooms.begin(), j.inRooms.end(), inRoom);
                if (at == j.inRooms.end() && j.inRooms.size() < 4) at = j.inRooms.insert(j.inRooms.end(), inRoom);
                if (at != j.inRooms.end()) tv.p[3] += 8.0f, inSlot = static_cast<float>(at - j.inRooms.begin());
            }
            tv.n[3] = static_cast<float>(t.packed >> 24) / 255.0f;
            // (bit 8) the street lamps the game's room-0 solve left in its base are no light of the room below either (before:
            // the upper half beige from them, the lower half its room's red)
            for (int k = 0; k < 3; k++) tv.base[k] = RtLampModel::ComposeBase(t.base[k], t.worldBase[k], tv.p[3] >= 8.0f);
            if (inSlot >= 0.0f) { // (the log: the game's solve of these texels without the street lamps, by day the windows' daylight)
                const int f = std::fabs(tv.n[1]) < 0.5f ? 0 : 1;
                inStat[f].n++;
                for (int k = 0; k < 3; k++) inStat[f].sum[k] += tv.base[k];
                inStat[f].max = std::max(inStat[f].max, tv.base[0] + tv.base[1] + tv.base[2]);
            }
            // the continued room's own map at its matched top-row texel, read by the draw in the same frame as the room's own draw
            // (bit 16: the compose writes the lamps' fall-off up the wall there in the scale's .z, IndoorRatio): what room 0's solve held there changed
            // with each solve of it, and a light carried here at a prepare lagged the room's (the attic matched its room only after a
            // load). With no top row known yet (a room never kept), its own solve stays
            // shortcut: a floor takes its room's top row's mean, carried at the prepare (24 of the 72 outdoor floor copies have no
            // samplers left for the rooms' maps); sample the room's map on floors too if their copies get the samplers
            if (inSlot >= 0.0f && m == 0 && !(t.bits & 1)) {
                if (const TopTexel* top = RtLampProxy::TopMatch(TopOf(static_cast<size_t>(inSlot)), t.pos, t.nrm, true)) {
                    // (the map keeps light, the matched texel's as carried at this prepare, for every reader that does not take the
                    // ratio: the game's shader, the bloom pass, the class-2 blur, a draw whose rooms are not read; the ratio goes in the
                    // outdoor scale's .z: IndoorRatio)
                    tv.p[3] += 16.0f, inCarried++;
                    for (int k = 0; k < 3; k++) tv.base[k] = top->rgb[k];
                    tv.mask[0] = static_cast<float>(t.x), tv.mask[1] = static_cast<float>(t.y); // (its column's lowest, below)
                    p.inUv.push_back({static_cast<uint16_t>(t.x), static_cast<uint16_t>(t.y), top->x, top->y, static_cast<int>(inSlot)});
                    p.inRgb.push_back({top->rgb[0], top->rgb[1], top->rgb[2]});
                }
            } else if (inSlot >= 0.0f && std::fabs(tv.n[1]) >= 0.5f) {
                const auto& top = TopOf(static_cast<size_t>(inSlot));
                if (!top.empty()) {
                    inCarried++;
                    for (int k = 0; k < 3; k++) tv.base[k] = 0.0f;
                    for (const TopTexel& c : top)
                        for (int k = 0; k < 3; k++) tv.base[k] += c.rgb[k] / static_cast<float>(top.size());
                    p.inUv.push_back({static_cast<uint16_t>(t.x), static_cast<uint16_t>(t.y), 0xFFFF, 0xFFFF, static_cast<int>(inSlot)}); // (never in the UV map)
                    p.inRgb.push_back({tv.base[0], tv.base[1], tv.base[2]});
                }
            }
            tv.base[3] = ex.dither ? RoomMapDither::Offset(t.x, t.y) / 255.0f : 0.0f;
            if (tv.p[3] < 16.0f) tv.mask[0] = t.vis, tv.mask[1] = t.all; // (bit 16: never masked, mask.xy its column's lowest)
            if (t.bits & 4) tv.mask[2] = t.exitBase, tv.mask[3] = 1.0f; // (GPU room lamps) an exit ground texel: ComposeK0PS
            if (inSlot >= 0.0f) tv.mask[2] = inSlot, tv.mask[3] = t.pos[1]; // (bit 8) its room's slot and its world height: ComposePS's IndoorOf
            if (t.bits & 1) {
                p.masked.push_back(static_cast<uint32_t>(p.v.size()));
                p.maskedPos.insert(p.maskedPos.end(), {tv.p[0], tv.p[1], tv.p[2], t.pos[0], t.pos[1], t.pos[2]});
                p.gated.push_back((t.bits & 2) ? 1 : 0);
            }
            cx += tv.p[0], cy += tv.p[1], cz += tv.p[2];
            p.v.push_back(tv);
            if (j.phys) { // (physical) lot position and normal, bits 1 masked, 2 gated, 4 exit ground, 8 tested; the world position
                RtLampGpu::TexelVertex q{};
                q.x = tv.x, q.y = tv.y, q.w = 1.0f;
                for (int k = 0; k < 3; k++) q.p[k] = tv.p[k], q.n[k] = tv.n[k], q.base[k] = t.pos[k];
                q.p[3] = static_cast<float>(t.bits & 15);
                p.pv.push_back(q);
                for (int k = 0; k < 3; k++) p.pBox[k] = std::min(p.pBox[k], t.pos[k]), p.pBox[3 + k] = std::max(p.pBox[3 + k], t.pos[k]);
                if (hi && !(t.bits & 1) && tv.n[1] > 0.9f) {
                    HiSrc h{tv, q, {tv.p[0], tv.p[1], tv.p[2], 0, tv.n[0], tv.n[1], tv.n[2], 0}, {}};
                    const float wl = std::sqrt(t.nrm[0] * t.nrm[0] + t.nrm[1] * t.nrm[1] + t.nrm[2] * t.nrm[2]);
                    for (int k = 0; k < 3; k++) h.world[k] = t.pos[k], h.world[4 + k] = wl > 1e-6f ? t.nrm[k] / wl : 0.0f;
                    hiSrc.push_back(h);
                }
            }
        }
        // (bit 16) a matched wall texel's column: the lowest matched texel at its place in plan facing its way (IndoorRatio's floor line)
        {
            std::map<std::array<long, 4>, std::pair<float, std::array<float, 2>>> low;
            const auto keyOf = [](const RtLampGpu::TexelVertex& v) {
                return std::array<long, 4>{std::lround(v.p[0] * 100.0f), std::lround(v.p[2] * 100.0f), std::lround(v.n[0] * 10.0f), std::lround(v.n[2] * 10.0f)};
            };
            for (const auto& v : p.v)
                if (v.p[3] >= 16.0f && std::fabs(v.n[1]) < 0.5f) {
                    auto [it, fresh] = low.try_emplace(keyOf(v), v.p[1], std::array<float, 2>{v.mask[0], v.mask[1]});
                    if (!fresh && v.p[1] < it->second.first) it->second = {v.p[1], {v.mask[0], v.mask[1]}};
                }
            for (auto& v : p.v)
                if (v.p[3] >= 16.0f && std::fabs(v.n[1]) < 0.5f) {
                    const auto& b = low[keyOf(v)].second;
                    v.mask[0] = b[0], v.mask[1] = b[1];
                }
        }
        // (physical, sharper floors) each of those texels' 4 sub-texels at their own places on the floor (the room's own texels as
        // neighbours only: never across a wall, a diagonal wall's half tiles included), the shadow their footprint's mean (mask.w)
        // shortcut: a map with more than kHiRoom0Texels such texels keeps the game's texels (memory), bound them by reach if it matters
        if (!hiSrc.empty() && hiSrc.size() <= kHiRoom0Texels) {
            std::unordered_map<uint32_t, size_t> at;
            for (size_t k = 0; k < hiSrc.size(); k++) at[static_cast<uint32_t>(hiSrc[k].v.y) << 16 | static_cast<uint32_t>(hiSrc[k].v.x)] = k;
            const auto look = [&](bool world) {
                return [&, world](int x, int y) -> const float* {
                    if (x < 0 || y < 0) return nullptr;
                    const auto it = at.find(static_cast<uint32_t>(y) << 16 | static_cast<uint32_t>(x));
                    return it == at.end() ? nullptr : world ? hiSrc[it->second].world : hiSrc[it->second].lot;
                };
            };
            for (const auto& [place, k] : at) {
                const HiSrc& h = hiSrc[k];
                const int x = static_cast<int>(h.v.x), y = static_cast<int>(h.v.y);
                float lot[4][3], world[4][3];
                const float foot = PhysLampPick::SubTexels(h.lot, x, y, look(false), lot);
                PhysLampPick::SubTexels(h.world, x, y, look(true), world);
                for (int q = 0; q < 4; q++) {
                    const float sx = static_cast<float>(2 * x + (q & 1)), sy = static_cast<float>(2 * y + (q >> 1));
                    RtLampGpu::TexelVertex c = h.v, ph = h.q, pl{};
                    c.x = ph.x = pl.x = sx, c.y = ph.y = pl.y = sy, pl.w = 1.0f;
                    if (c.base[3] != 0.0f) c.base[3] = RoomMapDither::Offset(static_cast<unsigned>(sx), static_cast<unsigned>(sy)) / 255.0f; // (each sub-texel its own: OutdoorDelta's `sub`; a 16-bit target takes none, Packed)
                    std::memcpy(c.p, lot[q], sizeof lot[q]), std::memcpy(ph.p, lot[q], sizeof lot[q]), std::memcpy(ph.base, world[q], sizeof world[q]);
                    ph.mask[3] = foot;
                    pl.mask[0] = sx, pl.mask[1] = sy;
                    p.vHi.push_back(c), p.pvHi.push_back(ph), p.placesHi.push_back(pl);
                }
            }
        }
        if (!p.pv.empty()) { // one point per place, its last texel's (the sum is one per place; the compose's last point wins)
            std::unordered_map<uint32_t, size_t> last;
            const auto placeOf = [](const RtLampGpu::TexelVertex& q) { return static_cast<uint32_t>(q.y) << 16 | static_cast<uint32_t>(q.x); };
            for (size_t k = 0; k < p.pv.size(); k++) last[placeOf(p.pv[k])] = k;
            size_t o = 0;
            for (size_t k = 0; k < p.pv.size(); k++)
                if (last[placeOf(p.pv[k])] == k) p.pv[o++] = p.pv[k];
            p.pv.resize(o);
        }
        // (GPU room lamps) the lamps of its list kept as a kept room's, out of the base: each part at its texel's place (the
        // place's last write only: the sum is one per place), with the sample's world position and normal as when kept
        if (!ex.kept0.empty()) {
            std::unordered_map<uint32_t, uint32_t> lastAt;
            for (uint32_t i = 0; i < ex.texels.size(); i++)
                if (ex.texels[i].map == m) lastAt[static_cast<uint32_t>(ex.texels[i].y) << 16 | ex.texels[i].x] = i;
            for (const OutdoorDelta::Room0Export::KeptLamp0& kl : ex.kept0) {
                const UINT first = static_cast<UINT>(p.kv.size());
                for (const OutdoorDelta::Room0Export::KeptPart0& kp : kl.parts) {
                    if (kp.texel >= ex.texels.size()) continue;
                    const OutdoorDelta::Room0Export::Texel& t = ex.texels[kp.texel];
                    const auto at = lastAt.find(static_cast<uint32_t>(t.y) << 16 | t.x);
                    if (t.map != m || at == lastAt.end() || at->second != kp.texel) continue;
                    RtLampGpu::TexelVertex v{};
                    v.x = t.x, v.y = t.y, v.w = 1.0f;
                    for (int k = 0; k < 3; k++) v.p[k] = t.pos[k], v.n[k] = t.nrm[k];
                    v.p[3] = kp.bits, v.n[3] = kp.mult;
                    v.base[0] = kp.A, v.base[1] = kp.B, v.base[2] = kp.thrScale;
                    v.mask[2] = kl.exit ? 1.0f : 0.0f;
                    p.kv.push_back(v);
                }
                // its floor mask parts: points of no light (A 0) carrying the walls-tested and no-walls shares
                for (const OutdoorDelta::Room0Export::KeptLamp0::Mask0& mp : kl.mask) {
                    if (mp.texel >= ex.texels.size()) continue;
                    const OutdoorDelta::Room0Export::Texel& t = ex.texels[mp.texel];
                    const auto at = lastAt.find(static_cast<uint32_t>(t.y) << 16 | t.x);
                    if (t.map != m || at == lastAt.end() || at->second != mp.texel) continue;
                    RtLampGpu::TexelVertex v{};
                    v.x = t.x, v.y = t.y, v.w = 1.0f;
                    for (int k = 0; k < 3; k++) v.p[k] = t.pos[k], v.n[k] = t.nrm[k];
                    v.n[3] = 1.0f;
                    v.mask[0] = mp.vis, v.mask[1] = mp.all;
                    p.kv.push_back(v);
                    p.kX = true;
                }
                p.kX = p.kX || (kl.exit && p.kv.size() > first);
                if (p.kv.size() == first) continue;
                p.kRanges.emplace_back(first, static_cast<UINT>(p.kv.size()) - first);
                KeptInfo ki;
                ki.light = kl.light, ki.colourOnly = kl.colourOnly, ki.geo = kl.geo, ki.law = kl.law;
                std::memcpy(ki.pos, kl.pos, sizeof ki.pos);
                p.kLamps.push_back(ki);
            }
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
    // (the log) what those texels take from the game's solve (by day: the windows' daylight of the rooms they continue, which
    // that room-0 list holds), beside the room's own wall light in its top 0.5 m: the evidence for their light by day
    if (inStat[0].n + inStat[1].n > 0) {
        std::string rooms;
        for (const int r : j.inRooms) {
            OutdoorDelta::IndoorFit f;
            const bool kept = OutdoorDelta::IndoorFitOf(j.rn.lot, j.story - 1, r, f);
            rooms += kept ? std::format("; room {}: its walls' top 0.5 m ({:.4f} {:.4f} {:.4f}) over {} texels", r, f.topMean[0], f.topMean[1], f.topMean[2], f.topN)
                          : std::format("; room {}: no kept solve", r);
        }
        const auto mean = [](const InStat& s, int k) { return s.n ? s.sum[k] / static_cast<double>(s.n) : 0.0; };
        LOG_INFO(std::format("[RtOutdoorLamps] Story {} room 0, indoor texels: walls {} (light without lamps ({:.4f} {:.4f} {:.4f}), largest sum {:.4f}), "
                             "floors {} (({:.4f} {:.4f} {:.4f}), largest {:.4f}), {} drawn from their room's own map or (floors) its top row{}",
                             j.story, inStat[0].n, mean(inStat[0], 0), mean(inStat[0], 1), mean(inStat[0], 2), inStat[0].max, inStat[1].n, mean(inStat[1], 0),
                             mean(inStat[1], 1), mean(inStat[1], 2), inStat[1].max, inCarried, rooms));
    }
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
    // (a prepare for a new kept solve of a room below keeps the same texels matched: its UV map and rooms stay bound, never a frame
    // without them; another layout drops them, built again for it by InRoomsNow)
    const bool sameIn = s.inRooms == j.inRooms && !std::memcmp(s.inCls, j.inCls, sizeof s.inCls) && s.inUvBuilt == p.inUv;
    s.inRooms = j.inRooms;
    std::memcpy(s.inVer, j.inVer, sizeof s.inVer);
    std::memcpy(s.inCls, j.inCls, sizeof s.inCls);
    s.inUvSrc = std::move(p.inUv);
    s.inBaseVal = std::move(p.inRgb);
    std::memcpy(s.inBaseVer, j.inVer, sizeof s.inBaseVer);
    s.inBaseDirty = true;
    if (!sameIn) {
        if (s.inUv) s.inUv->Release(), s.inUv = nullptr;
        s.inUvBuilt.clear();
        s.inAmbOk = false;
    }
    s.thr = j.thr;
    s.dither = j.dither;
    s.kept = std::unordered_set<uintptr_t>(j.kept.begin(), j.kept.end());
    s.drawn = std::unordered_set<uintptr_t>(j.drawn.begin(), j.drawn.end());
    if (s.vb) s.vb->Release(), s.vb = nullptr;
    if (s.dyn) s.dyn->Release(), s.dyn = nullptr;
    if (s.dynNext) s.dynNext->Release(), s.dynNext = nullptr;
    if (s.preblur) s.preblur->Release(), s.preblur = nullptr;
    if (s.kvb) s.kvb->Release(), s.kvb = nullptr;
    if (s.pvb) s.pvb->Release(), s.pvb = nullptr;
    ReleaseHiPoints(s);
    s.nPhys = 0, s.pLights.clear(), s.pRow.clear(), s.pBlk.reset(), s.pHeader = -1.0f, s.physLater = j.physLater;
    s.kRanges.clear(), s.kLamps.clear(), s.kX = false;
    // "all" depends only on the points' x, y, z and allSig's lamps and threshold: the same points keep its values and its job
    const bool samePts = !s.maskedPos.empty() && s.maskedPos.size() == p.maskedPos.size() &&
                         !std::memcmp(s.maskedPos.data(), p.maskedPos.data(), s.maskedPos.size() * sizeof(float));
    s.masked.clear(), s.maskedPos.clear(), s.gated.clear(), s.under.clear(), s.uA.clear(), s.uU.clear(), s.uSnap.clear(), s.uIds.clear();
    s.underSig = 0, s.underExact = false, s.underDirty = false, s.underWaitSince = 0, s.uJob.reset();
    s.dynCurrent = false, s.allCurrent = false; // (the new buffers start from zeros: written in full)
    if (!samePts) s.allVals.clear(), s.aJob.reset(), s.allWaitSince = 0; // (a job read the old points)
    s.map = RtLampGpu::Map{};
    D3DSURFACE_DESC d{};
    // A room's own map may be smaller than the class's working map the solve wrote: the commit's copy 0x006191D0 takes
    // min(width) x min(height) and copies those rows (memcpy per row, no filter), so the own map is the working map's
    // top-left corner, texel for texel. The texels are drawn at their place in a map of the own size (the ones past it
    // fall outside the viewport). A larger own map is refused: the commit leaves the rest of it as it was. A blurred map
    // (the class-2 wall atlas) keeps the exact size: its wall blocks before the blur are laid out at the working map's.
    // (A 20 x 30 m lot: own floor maps 256x128 and 256x256 under a 512x256 working map, so the lot was never covered.)
    if (!s.game || FAILED(s.game->GetLevelDesc(0, &d)) || d.Format != D3DFMT_A8R8G8B8 || s.game->GetLevelCount() != 1 ||
        !(p.blur ? d.Width == p.w && d.Height == p.h : d.Width <= p.w && d.Height <= p.h)) {
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
    if (!p.kv.empty() && !RtLampGpu::Engine::MakeVb(dev, p.kv.data(), static_cast<UINT>(p.kv.size()), &s.kvb)) return retryLater("its kept lamps' parts could not be made");
    s.kRanges = std::move(p.kRanges), s.kLamps = std::move(p.kLamps), s.kX = p.kX;
    p.kv = {};
    if (j.phys && !p.pv.empty()) {
        if (!RtLampGpu::Engine::MakeVb(dev, p.pv.data(), static_cast<UINT>(p.pv.size()), &s.pvb, true)) return retryLater("its physical points could not be made");
        s.nPhys = static_cast<UINT>(p.pv.size());
        s.pLights = j.physLights, s.pRow = j.physRow, s.pBlk = j.blk, s.pHeader = j.blkHeader;
        std::memcpy(s.pBox, p.pBox, sizeof s.pBox);
        // (sharper floors) room 0's floor sub-texels; not made: the copy's upsample stands for them, as with the option off
        const UINT nHi = static_cast<UINT>(p.vHi.size());
        if (nHi && (!RtLampGpu::Engine::MakeVb(dev, p.vHi.data(), nHi, &s.vbHi, true) || !RtLampGpu::Engine::MakeVb(dev, p.pvHi.data(), nHi, &s.pvbHi, true) ||
                    !RtLampGpu::Engine::MakeVb(dev, p.placesHi.data(), nHi, &s.placesHi, true)))
            ReleaseHiPoints(s);
        else
            s.nHi = nHi;
    }
    p.pv = {}, p.vHi = {}, p.pvHi = {}, p.placesHi = {};
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
    uint32_t listed = 0; // the stories whose room-0 list holds it (mult also covers the stories it is drawn on before they gather it)
    bool liftable = false; // drawn from kLowLampLift on story 0's floor map when it lies on the ground
    bool post = false;     // a lamp post (the street-lamp class 11): drawn on the lots next to the active one too
    bool inBake = false;   // (a post) in the terrain bake: a lot next to it counts it under its outdoor floors as its gather did
    RtLampModel::BakeLamp bake;
};
// A lamp of the terrain bake as the floor mask's under-floor share takes it
void FillBake(const LampRead& r, bool coneSpill, RtLampModel::BakeLamp& b) {
    for (int k = 0; k < 3; k++) b.head[k] = r.pos[k];
    b.range = r.range, b.intensity = r.intensity;
    const bool spill = coneSpill && r.f.type == 5;
    for (int k = 0; k < 3; k++) b.colour += std::max(r.f0[k], 0.0f) * (spill ? std::clamp(r.S[k], 0.0f, 1.0f) : 1.0f);
    b.colour *= r.areaShare;
    b.liftable = TerrainBakeRule::Liftable(r.f.lot != 0, r.f.type) ? 1u : 0u;
}

} // namespace

RtLampGpu::DeviceCalls DeviceCallsOfGame() { return GameCalls(); }

bool WorldLaw(uintptr_t light, uint64_t lot, bool reachOn, GpuLampLaw::Lamp& out, int& type, int& regRoom, const float* lotXf) {
    LampRead r;
    if (!ReadLamp(light, r)) return false;
    // (a tube: a segment the law reads, else its colour alone, ColourLaw; Real-time outdoor lamps' own draw takes no tube, Drawn)
    const float* e = r.tubeEnd;
    const float s2 = (e[0] - r.pos[0]) * (e[0] - r.pos[0]) + (e[1] - r.pos[1]) * (e[1] - r.pos[1]) + (e[2] - r.pos[2]) * (e[2] - r.pos[2]);
    const bool tube = r.f.type == 6 && std::isfinite(s2) && s2 > 1e-6f && std::isfinite(r.tubeLen) && r.tubeLen >= 0.0f && r.tubeLen < 1e3f;
    // (an area light: finite axes and a size from 0 to 1e6, else it stays in the base; Real-time outdoor lamps' own draw takes none, Drawn)
    bool area = TerrainBakeRule::AreaType(r.f.type);
    for (const float x : {r.areaU[0], r.areaU[1], r.areaU[2], r.areaV[0], r.areaV[1], r.areaV[2], r.areaN[0], r.areaN[1], r.areaN[2]}) area = area && std::isfinite(x);
    for (const float x : r.areaE) area = area && std::isfinite(x) && x >= 0.0f && x < 1e6f;
    const bool cls = r.f.type == 3 || r.f.type == 4 || r.f.type == 5 || r.f.type == 11 || tube || area;
    if (!cls || r.f.unmodelled || r.f.lot == 0 || r.f.lot != lot || !(r.f.flags & 0x01)) return false;
    Xform id{};
    for (int k = 0; k < 4; k++) id.r[k][k] = 1.0f;
    if (lotXf) std::memcpy(&id, lotXf, sizeof id);
    ToLaw(r, ReadConsts(), id, reachOn, out);
    type = r.f.type, regRoom = r.f.room;
    return true;
}

bool OwnLotLaw(uintptr_t light, GpuLampLaw::Lamp& out) {
    LampRead r;
    int type = 0, reg = -1;
    return ReadLamp(light, r) && r.f.lot != 0 && WorldLaw(light, r.f.lot, false, out, type, reg);
}

bool LotXform(uintptr_t mgr, float out[16]) {
    if (!mgr) return false;
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(mgr + 0xE0), 64);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    for (int i = 0; i < 16; i++)
        if (!std::isfinite(out[i])) return false;
    return true;
}
// (a stair upload changes what the tiles hold and not the lot, so KeptLot keeps g_openGen; proxy generations stay under 2^20)
uint32_t KeptMeshGen() {
    const uint32_t gen = g_openGen.load(std::memory_order_relaxed);
    return gen ? gen ^ g_stairUploads.load(std::memory_order_relaxed) << 20 : 0;
}
bool RoomOpenAbove(int story, int room) { return std::binary_search(g_openRooms.begin(), g_openRooms.end(), std::make_pair(story, room)); }
std::shared_ptr<const RtLampProxy::Lot> KeptLot(uint32_t& gen) {
    gen = g_openGen.load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> lk(g_proxyMx);
    // (the light tree thread may have built a newer proxy the render thread has not uploaded yet: then none)
    if (!gen || !g_proxy || g_proxy->gen != gen) return nullptr;
    return std::shared_ptr<const RtLampProxy::Lot>(g_proxy, &g_proxy->lot);
}

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
    if (!t) return S_OK;
    const uintptr_t a = reinterpret_cast<uintptr_t>(t);
    Slot* s = nullptr;
    if (g_on.load(std::memory_order_relaxed) && !g_swap.empty() && a >= g_swapLo && a <= g_swapHi)
        if (const auto it = g_swap.find(t); it != g_swap.end()) s = it->second;
    if (!s && !g_swapRetired.empty() && a >= g_retiredLo && a <= g_retiredHi) // (the lots left at earlier switches)
        if (const auto it = g_swapRetired.find(t); it != g_swapRetired.end()) s = it->second;
    if (!s) return S_OK;
    s->binds++;
    swapped = true;
    return D3D9Hooks::CallOriginalSetTexture(dev, stage, s->hiOk && s->hi ? s->hi : s->copy);
}
void OnPreReset(IDirect3DDevice9* dev) {
    ReleaseRetired(dev, "device reset"); // (DEFAULT render targets)
    UnbindCopies(dev);
    for (auto& s : g_slots) {
        if (s->copy) s->copy->Release(), s->copy = nullptr;
        ReleaseHi(*s);
        s->partKey = 0;
        if (s->scale) s->scale->Release(), s->scale = nullptr;
        s->scaled = false;
        s->relit = false; // (14/10: its preblur texture is a MANAGED one: kept, and the slot stays built; only the copy is made again)
        if (s->pvb) s->pvb->Release(), s->pvb = nullptr, s->built = s->ok = false; // (DEFAULT: prepared again; its base lacks their lamps)
        ReleaseHiPoints(*s);
        // (DEFAULT: filled again from inBaseVal / inUvSrc)
        if (s->inBase) s->inBase->Release(), s->inBase = nullptr, s->inBaseDirty = true;
        if (s->inUv) s->inUv->Release(), s->inUv = nullptr, s->inUvBuilt.clear();
    }
    if (g_eng) g_eng->ReleaseDefault();
    GpuRoomLamps::ReleaseAll(); // (their buffers are DEFAULT: made again on use)
    for (IDirect3DQuery9** q : {&g_qDisjoint, &g_qFreq, &g_qBegin, &g_qEnd})
        if (*q) (*q)->Release(), *q = nullptr;
    for (IDirect3DQuery9*& q : g_qMark)
        if (q) q->Release(), q = nullptr;
    g_qPending = false, g_marks = 0, g_marking = false;
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
    g_stampLights.clear();
    if (!HookGuard::Run("RtOutdoorLamps frame", [dev] { Frame(dev); }) && (g_on.load() || !g_slots.empty() || !g_nb.empty())) {
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
    HookGuard::Try("TerrainStamps update", [] { TerrainStamps::Update(g_stampLights); });
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
                                 "options {}, occluders {}; {} relights for repeated game writes moved later; {} story maps prepared ({:.1f} ms on a worker thread); "
                                 "of a frame: the lots next to it {:.3f} ms, the maps' checks and relights {:.3f} ms; {} rooms-only relights drew only their changed rooms{}{}",
                                 (now - w.since) / 1000.0, w.frames ? w.frameMs / static_cast<double>(w.frames) : 0.0, w.frameMax, w.relights, w.first, w.lamp,
                                 w.base, w.gameWrite, w.exact, w.params, w.occluders, w.writeGap, w.build, g_buildMs,
                                 w.frames ? w.nbMs / static_cast<double>(w.frames) : 0.0, w.frames ? w.loopMs / static_cast<double>(w.frames) : 0.0, w.partial,
                                 GpuParts(), g_inactive.empty() ? std::string() : " | inactive: " + g_inactive));
        }
        if (g_option.load(std::memory_order_relaxed) && g_inSum.frames) {
            const InSum& n = g_inSum;
            LOG_INFO(std::format("[RtOutdoorLamps] Texels over open rooms below, over {:.0f} s: {} map frames, their rooms read now {}, the last binding kept {} (a room's "
                                 "ambient not read {}, a room's map not read {}, the UV map not built {}), none (carried light shown) {}; relit copy <-> game's map "
                                 "switches {}; a room showing another class than matched {}; frames waiting for a prepare in the class shown {}",
                                 (now - g_why.since) / 1000.0, n.frames, n.now, n.keptAmb + n.keptMap + n.keptUv, n.keptAmb, n.keptMap, n.keptUv, n.none, n.toggles,
                                 n.otherClass, n.lag));
        }
        g_inSum = InSum{};
        g_why = Why{};
        std::fill(std::begin(g_gpuPart), std::end(g_gpuPart), 0.0);
        g_why.since = now;
        g_buildMs = 0;
    }
}

// (the Cost line) the relights' GPU time by part over the timed frames
std::string GpuParts() {
    const Why& w = g_why;
    if (!w.gpuFrames) return std::format("; {} maps left to the next frame by the GPU budget", w.deferred);
    const double n = static_cast<double>(w.gpuFrames), * g = g_gpuPart;
    return std::format("; GPU of the relights ({} frames timed, largest {:.2f} ms), ms a timed frame: room 0's physical lamps {:.2f}, outdoor lamps and their cubes {:.2f}, "
                       "room 0's compose {:.2f} (of maps carrying a room below's top row {:.2f}), kept rooms over it {:.2f}, sharper floors {:.2f}, other {:.2f}; "
                       "{} maps left to the next frame by the GPU budget",
                       w.gpuFrames, w.gpuMax, g[kGpRoom0] / n, g[kGpOutdoor] / n, (g[kGpCompose] + g[kGpCarry]) / n, g[kGpCarry] / n, g[kGpRooms] / n, g[kGpHi] / n,
                       g[kGpOther] / n, w.deferred);
}

void ReadGpuTime() {
    if (!g_qPending || !g_qDisjoint) return;
    BOOL disjoint = TRUE;
    UINT64 freq = 0, t0 = 0, t1 = 0;
    if (g_qDisjoint->GetData(&disjoint, sizeof disjoint, 0) != S_OK || g_qFreq->GetData(&freq, sizeof freq, 0) != S_OK || g_qBegin->GetData(&t0, sizeof t0, 0) != S_OK ||
        g_qEnd->GetData(&t1, sizeof t1, 0) != S_OK)
        return; // not yet
    g_qPending = false;
    const int marks = std::exchange(g_marks, 0);
    if (disjoint || !freq || t1 < t0) return;
    const auto ms = [&](UINT64 a, UINT64 b) { return b > a ? 1000.0 * static_cast<double>(b - a) / static_cast<double>(freq) : 0.0; };
    g_lastGpuMs.store(static_cast<float>(ms(t0, t1)));
    // the parts: each mark ends the part it names (timestamps of one frame: all there once its end is)
    double part[kGpParts] = {};
    std::vector<std::pair<const void*, double>> perSlot;
    UINT64 prev = t0;
    for (int i = 0; i < marks; i++) {
        UINT64 t = 0;
        if (!g_qMark[i] || g_qMark[i]->GetData(&t, sizeof t, 0) != S_OK) return;
        const double d = ms(prev, t);
        prev = std::max(prev, t);
        part[g_markPart[i]] += d;
        if (const void* sl = g_markSlot[i]) {
            if (perSlot.empty() || perSlot.back().first != sl) perSlot.emplace_back(sl, 0.0);
            perSlot.back().second += d;
        }
    }
    part[kGpOther] += ms(prev, t1);
    for (int k = 0; k < kGpParts; k++) g_gpuPart[k] += part[k];
    g_why.gpuFrames++, g_why.gpuMax = std::max(g_why.gpuMax, ms(t0, t1));
    for (const auto& [sl, d] : perSlot)
        for (const auto& sp : g_slots)
            if (sp.get() == sl) sp->gpuMs = sp->gpuMs > 0.0 ? 0.5 * (sp->gpuMs + d) : d;
}

void Frame(IDirect3DDevice9* dev) {
    g_restartGpuPending = false;
    g_restartGpuFallback = false;
    ReadGpuTime();
    // what makes the option do nothing now, logged once each
    if (!g_option.load(std::memory_order_relaxed)) {
        PublishNeighbours(0, {}); // (before any slot: a lot next to it may be listed with none built yet)
        OutdoorDelta::SolveTouched(nullptr);
        if (!g_slots.empty()) {
            // the game's room-0 maps may lack lamp edits the GPU stood in for (OutdoorDelta left their deltas out): each
            // story's room 0 is solved again
            int sent = 0;
            for (int S = 0; S <= 7 && g_tracker && LevelLightShare::TrackerLive(g_tracker); S++)
                if (LevelLightShare::Room0Of(g_tracker, S)) sent += LevelLightShare::QueueRoom0(g_tracker, S) ? 1 : 0;
            if (sent) LOG_INFO(std::format("[RtOutdoorLamps] Switched off: {} room 0s of the active lot sent to their solve (their maps are the game's again)", sent));
            DropAll(dev, "switched off");
        }
        ReleaseRetired(dev, "switched off");
        g_on.store(false);
        g_watch.store(false);
        g_holdAt = 0, g_holdFor = 0, g_switchAt = 0; // (a hold ends unlogged: its length would span the time off)
        g_inactive = "off";
        return;
    }
    ServiceRetired(dev);
    const auto off = [&](const std::string& why) {
        PublishNeighbours(0, {});
        OutdoorDelta::SolveTouched(nullptr);
        if (!g_slots.empty()) DropAll(dev, why.c_str());
        g_on.store(false);
        g_watch.store(false);
        g_holdAt = 0, g_holdFor = 0, g_switchAt = 0;
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
    if (!g_eng && g_makeFails && GetTickCount() - g_makeFailAt < (g_makeFails < 5 ? 5000u : 60000u)) return off("its shader objects are made again shortly");
    if (!g_eng) {
        void* made[RtLampGpu::kShaderCount] = {};
        bool ok = true;
        for (int i = 0; i < RtLampGpu::kShaderCount && ok; i++) {
            const auto id = static_cast<RtLampGpu::ShaderId>(i);
            if (g_compiled.code[i].empty() && RtLampGpu::IsPhysical(id)) continue; // (CompileAll logged it)
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
            g_makeFails++;
            g_makeFailAt = GetTickCount();
            return off("its shaders could not be made (tried " + std::to_string(g_makeFails) + " times, again in a moment): " + why);
        }
        g_eng = std::move(eng);
        g_makeFails = 0;
        g_openUsed.reset(), g_openGen.store(0), g_openFailAt = 0, StairSet::g_set.Clear(); // (a new engine: no kept atlas occluders yet)
        LOG_INFO("[RtOutdoorLamps] Shaders made: cube shadow maps 6 x 256^2 per lamp, 16 lamps an atlas");
    }
    // the active lot
    uintptr_t trackers[4];
    const int lit = LevelLightShare::ActiveLotTrackers(trackers, 4);
    // (a lot lit only for its picture is not the active lot: no relit map is made for it, LevelLightShare::LotCapture)
    const int nt = static_cast<int>(std::remove_if(trackers, trackers + std::max(lit, 0), LevelLightShare::LotCapture) - trackers);
    if (nt <= 0) return off(lit > 0 ? "a lot capture (the lot is lit only for its picture)" : "no active lot (no lot lit at the high quality)");
    // (the active lot stays the one already active while it is still lit at the high quality: the list's order changes as
    // the camera moves, and each switch dropped and rebuilt every relit map)
    uintptr_t tracker = trackers[0];
    for (int i = 0; i < nt; i++)
        if (trackers[i] == g_tracker) tracker = g_tracker;
    // (active_lot_hold) while the camera moves, the active lot stays so until another has been the first lit at the high
    // quality for kActiveLotHoldMs, or the camera has been still for 300 ms: a pan across lot borders switched back and forth
    // (12 switches in 22 s), each dropping every relit map and sending the lots next to it to gather at a lamp edit's priority.
    // Its maps keep relighting meanwhile (nothing below needs the high quality; OutdoorDelta::ActiveLot follows this lot while
    // the switch is on, so its kept rooms stay). A lot the game no longer has switches at once, and so does the waiting lot
    // once it shows a map no copy stands in for (serve). An active lot still preparing its maps (occluders or kept solves) is
    // held too: a switch made it so, and letting the next border switch again at once chained them (4 switches in 750 ms of
    // one pan, each new lot waiting 3 to 5 s for its maps, so no lot was ever relit while the camera moved)
    const DWORD holdNow = std::max<DWORD>(GetTickCount(), 1);
    if (tracker == g_tracker || tracker != g_holdFor) g_holdFor = tracker, g_holdForAt = holdNow;
    const bool hold = tracker != g_tracker && g_tracker && DevSwitches::ActiveLotHold() && holdNow - g_holdForAt < kActiveLotHoldMs &&
                      LevelLightShare::TrackerLive(g_tracker) && LotLightingMotion::SampleCameraMoving();
    if (hold && !g_holdAt) g_holdAt = holdNow;
    if (!hold && g_holdAt) {
        LOG_INFO(std::format("[RtOutdoorLamps] Active lot switch held {} ms while the camera moved ({})", holdNow - g_holdAt,
                             tracker != g_tracker ? std::format("then tracker {:08X}", tracker) : std::string("the lot stayed the active one")));
        g_holdAt = 0;
    }
    if (hold) tracker = g_tracker;
    if (tracker != g_tracker) {
        // the lot left keeps its room 0s' kept solves a while (a return reuses them instead of a full solve), and the new lot's
        // ground room 0 is solved first (lot grass reads story 0's room-0 map; the occluder send below regathers it)
        const void* left[8];
        int nl = 0;
        if (g_tracker && LevelLightShare::TrackerLive(g_tracker))
            for (int S = 0; S <= 7; S++)
                if (const void* r = LevelLightShare::Room0Of(g_tracker, S)) left[nl++] = r;
        if (g_tracker) RoomLightQueue::GroundFirst(LevelLightShare::Room0Of(tracker, 0));
        g_rushFrom = std::max<DWORD>(GetTickCount(), 1);
        DropAll(dev, "another active lot", true, true);
        OutdoorDelta::GraceRooms(left, nl, GetTickCount() + kLeftLotGraceMs);
        {
            std::lock_guard<std::mutex> lk(g_editMx);
            g_edits.clear(); // (the edits of the lot that was active)
        }
        g_tracker = tracker;
        g_askedSolve.clear();
        g_budgetOut = false;
        g_proxyWanted.store(tracker);
        LOG_INFO(std::format("[RtOutdoorLamps] Active lot tracker {:08X}{}", tracker, nt > 1 ? std::format(" (the first of {} lots lit at the high quality)", nt) : std::string()));
        g_switchAt = std::max<DWORD>(GetTickCount(), 1);
    }
    // The switch's sends and the lots leaving it are quiet marks, which take no lamp edit's budget: how long the new lot's first
    // room-0 solve takes after the switch, and how many quiet rooms still wait then
    if (g_switchAt && (!tracker || GetTickCount() - g_switchAt > 30000)) g_switchAt = 0; // (no lot, or it never solved: stop looking)
    if (g_switchAt)
        for (int S = 0; S <= 7; S++) {
            const void* room = LevelLightShare::Room0Of(tracker, S);
            const DWORD end = room ? RoomLightQueue::SolveEndedAt(room) : 0;
            if (!end || static_cast<int32_t>(end - g_switchAt) < 0) continue;
            LOG_INFO(std::format("[RtOutdoorLamps] Active lot tracker {:08X}: story {} room 0 solved {} ms after the switch, {} quiet rooms still waiting", tracker, S,
                                 end - g_switchAt, LevelLightShare::QuietRoomsWaiting()));
            g_switchAt = 0;
            break;
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
        PublishNeighbours(0, {});
        g_restartGpuPending = true;
        g_proxyWanted.store(tracker);
        g_on.store(false);
        Inactive("waiting for the lot's occluders (built on the light tree thread at its next room update or room-0 solve)" +
                 (proxyWhy.empty() ? std::string() : " - last attempt: " + proxyWhy));
        // a room 0 sent to its solve builds them at its end (once per room every 10 s)
        for (int S = 0; S <= 7; S++)
            if (const void* room = LevelLightShare::Room0Of(tracker, S)) {
                Ask& a = g_askedSolve[room];
                if (AskDue(a, GetTickCount())) {
                    a.at = std::max<DWORD>(GetTickCount(), 1);
                    a.count++;
                    LevelLightShare::QueueRoomAgain(room, true); // (for the occluders: no edit behind it)
                } else if (a.count >= kSolveAsks && !a.logged) {
                    a.logged = true;
                    LOG_INFO(std::format("[RtOutdoorLamps] Story {} room 0 sent to its solve {} times with no occluders read: once a minute from now, or the lot's next room update", S, kSolveAsks));
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
    // (physical indoor lamps) the kept shadow tiles' occluders: the same lot with open tops over rooms open to above
    // (an upload that failed is tried again 5 s later)
    const DWORD stairNow = GetTickCount();
    if (!OutdoorDelta::RoomLampPhysical()) {
        if (StairSet::g_set.Gen()) StairSet::g_set.Clear();
    } else if (StairSet::g_set.Gen() != proxy->gen)
        StairSet::g_set.Begin(proxy->lot, proxy->gen);
    // (a grown stair set: the tiles' occluders again, not while a lamp is dragged; a redraw of the tiles, no room goes to its solve)
    const bool stairsDue = proxy == g_openUsed && StairSet::g_set.Due(stairNow) && !(g_lampChangedAt && stairNow - g_lampChangedAt < kDragQuietMs);
    if (OutdoorDelta::RoomLampPhysical() && (proxy != g_openUsed || stairsDue) && (!g_openFailAt || GetTickCount() - g_openFailAt >= 5000)) {
        RtLampProxy::Mesh open;
        RtLampProxy::Build(proxy->lot, open, true);
        StairSet::g_set.AppendTo(open);
        std::string err;
        const bool up = g_eng->SetProxy(dev, open, err, true);
        g_openUsed = up ? proxy : nullptr, g_openFailAt = up ? 0 : std::max<DWORD>(GetTickCount(), 1);
        g_openGen.store(up ? proxy->gen : 0);
        if (up) g_openRooms = std::move(open.openRooms), g_inside = std::make_shared<const std::vector<RtLampProxy::Inside>>(std::move(open.inside));
        if (up && stairsDue) StairSet::g_set.Uploaded(stairNow), g_stairUploads.fetch_add(1, std::memory_order_relaxed);
        LOG_INFO(up ? std::format("[RtOutdoorLamps] Physical indoor lamps' occluders: {} triangles with open tops ({} quadrants opened, {} stair triangles), generation {}", open.tris.size(),
                                 open.openTops, StairSet::g_set.Tris(), proxy->gen)
                    : "[RtOutdoorLamps] Physical indoor lamps' occluders could not be uploaded: " + err);
    }
    // (what the collector found: whether a stair reaches this set is known only in game)
    static DWORD s_at = 0;
    static uint32_t s_groups = 0;
    if (StairSet::g_set.Groups() != s_groups && stairNow - s_at >= 30000) {
        s_at = stairNow, s_groups = StairSet::g_set.Groups();
        LOG_INFO(std::format("[RtOutdoorLamps] Stair set: {} groups read, {} triangles seen, {} kept by the floor holes{}, {} uploads", s_groups, StairSet::g_set.Seen(),
                             StairSet::g_set.Tris(), StairSet::g_set.Full() ? " (full)" : "", StairSet::g_set.Uploads()));
    }
    // the rules every relit texel follows (a change relights every map)
    const GameConsts gc = ReadConsts();
    const bool reachOn = LevelLightShare::RealisticReachActive();
    const bool fade = LevelLightShare::SoftLightCutOn();
    const float radius = Penumbra::Radius();
    const bool coneSpill = LevelLightShare::BakeConeSpillOn();
    const float edgeR = OutdoorDelta::TrueFurnitureEdges() ? radius : 0.0f; // (physical: the furniture blockers' true-size edges)
    uint64_t params = 1469598103934665603ull;
    Mix(params, &gc, sizeof gc), Mix(params, &reachOn, 1), Mix(params, &fade, 1), Mix(params, &radius, 4), Mix(params, &coneSpill, 1), Mix(params, &edgeR, 4);
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
        } else if (g_budgetOut && OutdoorDelta::KeptBytes() < OutdoorDelta::PinnedBudgetBytes() / 100 * 80) { // (parts given back since: its stories may keep a solve again)
            g_budgetOut = false;
            LOG_INFO("[RtOutdoorLamps] The active lot's kept room-0 parts fit the memory budget again");
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
    // A story's room 0: its slots built from its kept solve of the class it shows. mgr 0: the active lot's; else the story
    // manager of a lot next to it, its texels placed in the active lot's space (`space`). False: (a lot next to it) the GPU
    // cannot relight that story (no kept solve after its sends, a map refused); its gather takes the lamp posts again
    const auto serve = [&](const BYTE* room, const RoomNow& rn, int S, uintptr_t mgr, const Xform& space) -> bool {
        const bool act = !mgr;
        const auto who = [&] { return act ? std::format("Story {}", S) : std::format("Lot {:08X} story {}", static_cast<uint32_t>(rn.lot), S); };
        const int cls = rn.shown;
        bool baseOnly = false;
        uint32_t ver = cls >= 0 && cls <= 2 ? OutdoorDelta::RecordVersion(room, cls, &baseOnly) : 0;
        // a kept solve of the other kind (the lot was next to the active one, or active): its maps wait for the next one
        if (baseOnly == act) ver = 0;
        if (ver) {
            auto at = g_askedSolve.find(room);
            if (at != g_askedSolve.end()) { // (kept: its sends start again if it loses it after holding it a while)
                const DWORD now = std::max<DWORD>(GetTickCount(), 1);
                if (!at->second.keptAt) at->second.keptAt = now;
                else if (now - at->second.keptAt >= kKeptHoldMs) g_askedSolve.erase(at);
            }
        } else if (auto at = g_askedSolve.find(room); at != g_askedSolve.end())
            at->second.keptAt = 0; // (lost before it held it: its count stands)
        if (!ver) {
            // its kept solve is being renewed (a solve of room 0 drops it until its end): its maps relit from the last one stay
            // bound and count while every one of them is relit
            bool had = false, all = true;
            for (const auto& sl : g_slots)
                if (sl->story == S && sl->room == room && sl->cls == cls && sl->used) had = true, all = all && sl->ok && sl->relit;
            if (!(had && all) && act) {
                allReady = false;
                if (S == 0 && g_rushFrom && GetTickCount() - g_rushFrom < kKeptRushMs) RoomLightQueue::GroundFirst(room); // (renewed each frame)
                const std::string last = OutdoorDelta::LastDrop(S, cls);
                waiting.push_back(std::format("story {}: no kept solve of class {}{}", S, cls, last.empty() ? std::string() : " - last: " + last));
            }
            // (active_lot_hold) the lot waiting to become the active one: not sent to its solve during the hold (an urgent solve on
            // the lot played, which the switch makes again in full); with no copy of its class left the hold ends (the switch runs
            // next frame), so its ground does not stay without the posts. Class 4 (no solve shown yet) has no map a copy stands in
            // for, so no posts to lose: the hold goes on and the game solves it meanwhile
            if (!act && g_holdAt && LevelLightShare::TrackerOfLot(rn.lot) == g_holdFor) {
                if (!had && cls >= 0 && cls <= 2 && holdNow - g_holdForAt < kActiveLotHoldMs) {
                    g_holdForAt = holdNow - kActiveLotHoldMs;
                    LOG_INFO(std::format("[RtOutdoorLamps] Active lot hold ends: {} (the lot waiting) shows class {} with no copy to stand in", who(), cls));
                }
                return true;
            }
            // idle: sent to its solve, which keeps its parts (a few times; never while the lot's parts overflow the budget, which
            // would drop another story's parts in turn)
            Ask& a = g_askedSolve[room];
            // a room too large to keep stays too large at every solve: asking again only costs the game a full room-0 solve
            // (about 18 s on a big lot), and a lot photographed for its thumbnail waits for every solve (New Game never ended)
            // (LastDrop is per story, not per room: only once this room was sent at least once and came back with that reason)
            const bool tooLarge = a.count >= 1 && OutdoorDelta::LastDrop(S, cls).find("too large to keep") != std::string::npos;
            if (tooLarge && a.count < kSolveAsks) a.count = kSolveAsks;
            if ((rn.state == 4 || rn.state == 5) && !g_budgetOut && !tooLarge && AskDue(a, GetTickCount(), act ? kSolveAskActiveMs : kSolveAskMs)) {
                a.at = std::max<DWORD>(GetTickCount(), 1);
                a.count++;
                const bool sent = LevelLightShare::QueueRoomAgain(room, true); // (an edit's light comes with the edit's own sends)
                if (a.count <= kSolveAsks)
                    LOG_INFO(std::format("[RtOutdoorLamps] {} room 0 shows class {} with no kept solve: {} ({} of {})", who(), cls,
                                         sent ? "sent to its solve" : "could not be sent to its solve", a.count, kSolveAsks));
            } else if ((a.count >= kSolveAsks || g_budgetOut) && !a.logged) {
                a.logged = true;
                LOG_INFO(std::format("[RtOutdoorLamps] {} room 0 class {}: no kept solve and {} ({}); its maps stay the game's and lamp "
                                     "edits go through the game's solve until it keeps one",
                                     who(), cls, tooLarge ? "not sent to its solve again" : "sends to its solve only once a minute now",
                                     tooLarge ? "too large to keep" : g_budgetOut ? "the lot's kept parts overflow the memory budget" : std::format("sent {} times", kSolveAsks)));
            }
            // (a lot next to it: its last copies stand in only while a solve renews its kept solve; idle with none, e.g. dropped
            // by the memory budget, it gathers the posts again once its sends are spent)
            if (!act) return (had && rn.state != 4 && rn.state != 5) || !(a.count >= kSolveAsks || g_budgetOut);
            if (!(had && all)) {
                if (RestartSolveTerminal(a, rn.state, g_budgetOut)) g_restartGpuFallback = true;
                else g_restartGpuPending = true;
            }
            return true;
        }
        // the slots of this story: built again when its kept base or its class changed (prepared on a worker thread, then
        // uploaded a map a frame; the old copies stay bound meanwhile)
        bool needs[4] = {}, need = false, covered = true;
        // (physical indoor lamps) room 0's lot lamps with a law drawn from their laws (its kept solve without them), unless that
        // solve's physical draw failed
        const bool wantPhys = act && OutdoorDelta::RoomLampCapture() && OutdoorDelta::RoomLampPhysical();
        const bool physReady = wantPhys && g_eng && g_eng->PhysReady() && KeptMeshGen() != 0;
        const auto refused = g_physRefused.find(room);
        const bool physRefused = refused != g_physRefused.end() && refused->second == ver;
        for (int m = 0; m < 4; m++) {
            Slot* slot = nullptr;
            for (auto& sl : g_slots)
                if (sl->mgr == mgr && sl->story == S && sl->m == m) slot = sl.get();
            if (slot && (slot->room != room || slot->cls != cls)) { // another class on screen, or the room was made again
                ReleaseSlot(*slot);
                slot->built = false, slot->relit = false, slot->room = room, slot->cls = cls;
                slot->retries = 0, slot->retryAt = 0; // (14/10 review: the backoff was the old room's or class's)
            }
            uintptr_t setAt = 0;
            if (slot && !slot->built && slot->retryAt && static_cast<int32_t>(GetTickCount() - slot->retryAt) < 0) { // (an upload's backoff)
                covered = false;
                if (act) g_restartGpuPending = true;
                continue;
            }
            needs[m] = !(slot && slot->built && slot->version == ver && (!slot->used || OwnMap(room, cls, kSetIdx[m], setAt) == slot->game)) ||
                       (slot && slot->physLater && physReady); // (physical indoor lamps: drawn from their laws once it can)
            need = need || needs[m];
            if (needs[m] && !(slot && (!slot->used || (slot->relit && slot->ok)))) covered = false; // (the old copy stays bound meanwhile)
            if (!act && slot && slot->built && slot->used && !slot->ok) return false; // (refused: UploadSlot's backoff leaves it unbuilt)
        }
        auto jit = std::find_if(g_jobs.begin(), g_jobs.end(), [room](const std::shared_ptr<Job>& j) { return j->room == room; });
        if (!need) {
            if (jit != g_jobs.end() && (*jit)->state.load() != 1) g_jobs.erase(jit);
            return true;
        }
        if (act) g_restartGpuPending = true; // includes preparation/export backoff without an in-flight job
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
                j->room = room, j->story = S, j->cls = cls, j->ver = ver, j->rn = rn, j->space = space, j->lot = act ? 0 : 1;
                if (act) j->bake = g_bakeLast[S], j->bakeKeys = g_bakeKeysLast[S];
                else if (const auto nk = g_nbBake.find(mgr); nk != g_nbBake.end()) j->bake = nk->second.bake, j->bakeKeys = nk->second.keys;
                j->base = rn.lotBase;
                j->phys = physReady && !physRefused, j->physLater = wantPhys && !physReady;
                if (act && S >= 1 && OutdoorDelta::RoomLampPhysical() && g_openUsed) j->open = g_openUsed, j->openRooms = g_openRooms, j->inside = g_inside;
                if (wantPhys && !j->phys) { // (its lamps back in its base: a lamp edit taken since its solve sends it to one)
                    PhysNote(room, S, physRefused ? "its physical draw failed" : !g_eng || !g_eng->PhysReady() ? "the physical shaders are missing" : "no occluders with open tops yet");
                    OutdoorDelta::SolveTouched(room);
                }
                g_jobs.push_back(j);
                StartJob(j);
                g_why.build++;
            }
            if (!covered && act) allReady = false, waiting.push_back(std::format("story {}: its maps are being prepared", S));
            return true;
        }
        Job& job = **jit;
        const int st = job.state.load();
        if (st == 1) {
            if (!covered && act) allReady = false, waiting.push_back(std::format("story {}: its maps are being prepared", S));
            return true;
        }
        if (st == 3) {
            g_jobs.erase(jit);
            if (act) allReady = false, waiting.push_back(std::format("story {}: its kept solve could not be read", S));
            ExportBackoff& b = g_exportBackoff[room];
            b.fails = std::min(b.fails + 1, 6);
            b.at = (GetTickCount() + (250u << (b.fails - 1))) | 1; // 0.25 s ... 8 s
            if (b.fails == 1 || b.fails == 6) LOG_INFO(std::format("[RtOutdoorLamps] {}: its kept solve could not be read ({} time{}); tried again later", who(), b.fails, b.fails == 1 ? "" : "s"));
            return act || b.fails < 6;
        }
        g_exportBackoff.erase(room);
        if (wantPhys && !job.physWhy.empty()) PhysNote(room, S, job.physWhy);
        else if (job.phys) PhysNote(room, S, {});
        g_buildMs += job.ms;
        job.ms = 0;
        bool done = true;
        for (int m = 0; m < 4; m++) {
            if (!needs[m]) continue;
            Slot* slot = nullptr;
            for (auto& sl : g_slots)
                if (sl->mgr == mgr && sl->story == S && sl->m == m) slot = sl.get();
            // (a prepare for a new kept solve of a room below, the same base: its walls and floors take it in the same frame, never
            // one a frame after the other at the floor line)
            uintptr_t inAt = 0;
            const bool inOnly = slot && slot->ok && slot->relit && !slot->inRooms.empty() && slot->version == job.version && slot->game &&
                                OwnMap(room, cls, kSetIdx[m], inAt) == slot->game;
            if ((uploads >= 1 && !inOnly) || dragNow) {
                done = false;
                break;
            }
            Prepared& p = job.maps[m];
            if (!p.used) { // (no texel of room 0 in this map: nothing to relight there; a slot notes it, so it is not prepared again)
                if (!slot) {
                    g_slots.push_back(std::make_unique<Slot>());
                    slot = g_slots.back().get();
                    slot->story = S, slot->m = m, slot->cls = cls, slot->room = room, slot->mgr = mgr;
                }
                ReleaseSlot(*slot);
                slot->built = true, slot->used = false, slot->version = job.version, slot->ok = false, slot->relit = false;
                continue;
            }
            if (!slot) {
                g_slots.push_back(std::make_unique<Slot>());
                slot = g_slots.back().get();
                slot->story = S, slot->m = m, slot->cls = cls, slot->room = room, slot->mgr = mgr;
            }
            slot->used = true;
            uintptr_t setAt = 0;
            IDirect3DTexture9* tex = OwnMap(room, cls, kSetIdx[m], setAt);
            if (tex != slot->game) {
                if (slot->game) slot->game->Release();
                slot->game = tex;
                if (tex) tex->AddRef();
                if (slot->copy) slot->copy->Release(), slot->copy = nullptr;
                ReleaseHi(*slot);
                slot->partKey = 0;
                // (an indoor space's marks stay over the game's new map of the same size until it is relit: IndoorFlagsOf)
                if (slot->scale && (p.w != slot->map.w || p.h != slot->map.h)) slot->scale->Release(), slot->scale = nullptr, slot->inFlags = false;
                slot->scaled = false;
                slot->relit = false;
            }
            if (!slot->logged) {
                slot->logged = true;
                LOG_INFO(std::format("[RtOutdoorLamps] {} class {} {}: the room's own map {:08X} (texture set at {:08X}, manager +0x{:X}), {}x{}", who(), cls, kMapName[m],
                                     reinterpret_cast<uintptr_t>(tex), setAt, setAt - rn.mgr, p.w, p.h));
            }
            uploads++;
            if (!UploadSlot(dev, *slot, job, p)) {
                if (act) allReady = false;
                LOG_INFO(std::format("[RtOutdoorLamps] {} class {} {}: not relit ({})", who(), cls, kMapName[m], slot->why));
            }
            // watched for writes (a lot next to it from the upper half: the active lot's maps keep theirs)
            if (slot->watch < 0)
                for (int w = act ? 0 : kMaxWatch / 2; w < kMaxWatch; w++)
                    if (!g_watchTex[w].load()) {
                        slot->watch = w;
                        break;
                    }
            if (slot->watch >= 0) g_watchTex[slot->watch].store(slot->game);
        }
        if (done) g_jobs.erase(std::find_if(g_jobs.begin(), g_jobs.end(), [room](const std::shared_ptr<Job>& j) { return j->room == room; }));
        else if (!covered && act) allReady = false, waiting.push_back(std::format("story {}: its maps are being uploaded", S));
        return true;
    };
    for (int S = 0; S <= 7; S++) {
        const BYTE* room = static_cast<const BYTE*>(LevelLightShare::Room0Of(tracker, S));
        if (!room) continue;
        RoomNow rn;
        if (!ReadRoomNow(room, rn) || rn.id != 0 || !rn.outdoor) continue;
        stories++;
        rooms[S] = room;
        rns[S] = rn;
        g_lotId = rn.lot;
        serve(room, rn, S, 0, rn.xf);
    }
    // slots of stories gone (the active lot's; the lots next to it below)
    for (auto it = g_slots.begin(); it != g_slots.end();)
        if (!(*it)->mgr && (!rooms[(*it)->story] || (*it)->room != rooms[(*it)->story])) {
            ReleaseSlot(**it);
            it = g_slots.erase(it);
        } else
            ++it;
    LARGE_INTEGER tp1{};
    QueryPerformanceCounter(&tp1);
    double slotsMs = Ms(tp0, tp1); // (and the lots next to it, below)
    // writes noted since the last frame (each map keeps its own until it is relit)
    uint64_t dirtyBits[kMaxWatch / 64];
    for (int i = 0; i < kMaxWatch / 64; i++) dirtyBits[i] = g_watchDirty[i].exchange(0);
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
                FillBake(r, coneSpill, b);
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
                g.post = r.f.type == 11;
                if (g.post && InTerrainBake(r)) g.inBake = true, FillBake(r, coneSpill, g.bake);
                lamps.push_back(g);
                it = lamps.end() - 1;
            }
            it->mult[S]++;
            it->listed |= 1u << S;
            Mix(allSig, &L, sizeof L);
            Mix(allSig, &it->lamp.law, sizeof it->lamp.law);
        }
        uint64_t h = 1469598103934665603ull;
        for (const auto& b : bake[S]) Mix(h, &b, sizeof b);
        for (uintptr_t L : bakeLights[S]) Mix(h, &L, sizeof L);
        bakeSig[S] = h;
    }
    // A lamp that is on and listed on some story is drawn on every story where it is drawable, as its gather will list it
    // there (the consistency repair below never found a gather that left one out). A new or picked-up lamp is listed on its
    // own story at once, but the other stories' room 0s gather it only at their next solve, which a drag defers: before,
    // the walls between floors showed no lamp for the whole drag and up to 4 s after it
    for (GpuLamp& g : lamps) {
        uint32_t missing = 0;
        int mult = 0;
        for (int S = 0; S <= 7; S++) {
            if (rooms[S] && !g.mult[S]) missing |= 1u << S;
            mult = std::max(mult, g.mult[S]);
        }
        LampRead r;
        if (!missing || !ReadLamp(g.light, r) || !(r.f.flags & 0x20)) continue;
        for (int S = 0; S <= 7; S++)
            if ((missing >> S & 1) && RtLampModel::Drawn(r.f, rns[S].lot)) g.mult[S] = mult;
    }
    // every lamp listed here, drawn on every story or not: its terrain stamp needs no room map (a picked-up lamp drawn
    // only on the upper stories kept the CPU terrain relight for its whole drag)
    for (const GpuLamp& g : lamps) g_stampLights.push_back(g.light);
    // and the lot's lamps the terrain bake takes that no room-0 list holds now (a copy held from the catalog, a post over the
    // house): out of the stamps' list for a second they went back to the bake, and every later re-render of their chunk baked
    // them wherever they stood then, the light left there while they moved on
    for (const LotLightBridge::BakeLamp& b : LotLightBridge::CurrentBakeLamps().lamps) {
        if (b.lot != g_lotId || !b.baked || !b.id || std::find(g_stampLights.begin(), g_stampLights.end(), b.id) != g_stampLights.end()) continue;
        LampRead r; // (the list is read every 20 frames: still that lot's lamp of that type)
        if (ReadLamp(b.id, r) && r.f.lot == b.lot && r.f.type == b.type) g_stampLights.push_back(b.id);
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
    // ---- the lots next to it: their room-0 maps take the active lot's lamp posts (the nearest kMaxNeighbours lots within
    // kNeighbourReach of the active lot, picked by the lots' places only: a post placed or moved changes nothing of the set,
    // which would make a lot joining it solve its outdoor rooms again; checked once a second, never while a lamp is dragged) ----
    LARGE_INTEGER tn0{};
    QueryPerformanceCounter(&tn0);
    {
        const Xform* space = nullptr;
        for (int S = 0; S <= 7 && !space; S++)
            if (rooms[S]) space = &rns[S].xf;
        static DWORD s_pickAt = 0;
        float box[4];
        if (!dragNow && (g_nbActive != g_lotId || GetTickCount() - s_pickAt >= 1000) && space && LotLightBridge::LotWorldBox(g_lotId, box)) {
            s_pickAt = GetTickCount();
            // (none while the active lot has no lamp post: nothing to draw there, and each lot joining or leaving solves its
            // outdoor rooms again, which a camera moving between lots did over and over)
            const bool posts = std::any_of(lamps.begin(), lamps.end(), [](const GpuLamp& g) { return g.post; });
            uint64_t hits[kMaxNeighbours + 1];
            const int n = posts ? LotLightBridge::LotsNear(box[0] - kNeighbourReach, box[1] - kNeighbourReach, box[2] + kNeighbourReach,
                                                           box[3] + kNeighbourReach, hits, kMaxNeighbours + 1)
                                : 0;
            std::vector<std::pair<uint64_t, uintptr_t>> want;
            for (int i = 0; i < n && static_cast<int>(want.size()) < kMaxNeighbours; i++)
                if (hits[i] != g_lotId)
                    if (const uintptr_t t = LevelLightShare::TrackerOfLot(hits[i])) want.emplace_back(hits[i], t);
            PublishNeighbours(g_lotId, want);
        }
        std::vector<std::pair<const void*, uintptr_t>> nbRooms; // (room, story manager) of their stories relit
        std::vector<std::pair<uintptr_t, int>> toCpu;
        for (Neighbour& nb : g_nb) {
            if (!space || !LevelLightShare::TrackerLive(nb.tracker)) continue;
            for (int S = 0; S <= 7; S++) {
                const BYTE* room = static_cast<const BYTE*>(LevelLightShare::Room0Of(nb.tracker, S));
                RoomNow rn;
                if (!room || (nb.cpu >> S & 1) || !ReadRoomNow(room, rn) || rn.id != 0 || !rn.outdoor || rn.lot != nb.lot) continue;
                nbRooms.emplace_back(room, rn.mgr);
                // its lamps of the bake, and the active lot's posts in the bake as its gather took them before (they left its list):
                // read again when its kept solve or the active lot's lamps changed, else once a second (a list changes at a gather)
                NbBake& nk = g_nbBake[rn.mgr];
                const uint32_t kept = rn.shown >= 0 && rn.shown <= 2 ? OutdoorDelta::RecordVersion(room, rn.shown) : 0;
                if (!nk.at || kept != nk.kept || allSig != nk.lampSig || GetTickCount() - nk.at >= 1000) {
                    nk.at = std::max<DWORD>(GetTickCount(), 1), nk.kept = kept, nk.lampSig = allSig;
                    std::vector<RtLampModel::BakeLamp> bk;
                    std::vector<uintptr_t> bl;
                    const int n = ReadList(room, list, 1024);
                    for (int i = 0; i < n; i++) {
                        LampRead r;
                        if (!list[i] || !ReadLamp(list[i], r) || !InTerrainBake(r)) continue;
                        RtLampModel::BakeLamp b;
                        FillBake(r, coneSpill, b);
                        bk.push_back(b), bl.push_back(list[i]);
                    }
                    for (const GpuLamp& g : lamps) {
                        if (!g.inBake || std::find(bl.begin(), bl.end(), g.light) != bl.end()) continue;
                        int mult = 0;
                        for (int k = 0; k <= 7; k++) mult = std::max(mult, g.mult[k]);
                        for (int k = 0; k < mult; k++) bk.push_back(g.bake), bl.push_back(g.light);
                    }
                    uint64_t h = 1469598103934665603ull;
                    for (const auto& b : bk) Mix(h, &b, sizeof b);
                    for (uintptr_t L : bl) Mix(h, &L, sizeof L);
                    if (n < 0) nk.at = 0; // (read again next frame)
                    else if (UnderSig(h, rn.lotBase) != UnderSig(nk.sig, nk.base) || nk.keys.size() != bk.size())
                        nk.keys = BakeKeys(bl), nk.bake = std::move(bk), nk.sig = h, nk.base = rn.lotBase;
                }
                if (!serve(room, rn, S, rn.mgr, *space)) nb.cpu |= 1u << S, toCpu.emplace_back(nb.tracker, S);
            }
        }
        if (!toCpu.empty()) {
            SendNeighbours(); // (before their gathers)
            for (const auto& [t, S] : toCpu) {
                const bool sent = LevelLightShare::QueueRoom0(t, S);
                LOG_INFO(std::format("[RtOutdoorLamps] Tracker {:08X} story {}: its maps cannot be relit here; its room 0 takes the active lot's lamp posts in its own "
                                     "gather again ({})", t, S, sent ? "sent" : "could not be sent"));
            }
        }
        for (auto it = g_slots.begin(); it != g_slots.end();)
            if ((*it)->mgr && std::find(nbRooms.begin(), nbRooms.end(), std::make_pair(static_cast<const void*>((*it)->room), (*it)->mgr)) == nbRooms.end()) {
                ReleaseSlot(**it);
                it = g_slots.erase(it);
            } else
                ++it;
        // (a story no longer served: its finished job would stay with its prepared buffers)
        std::erase_if(g_jobs, [&](const std::shared_ptr<Job>& j) {
            return j->lot && j->state.load() != 1 && std::none_of(nbRooms.begin(), nbRooms.end(), [&](const auto& r) { return r.first == j->room; });
        });
        for (auto it = g_nbBake.begin(); it != g_nbBake.end();)
            if (std::none_of(nbRooms.begin(), nbRooms.end(), [&](const auto& r) { return r.second == it->first; })) it = g_nbBake.erase(it);
            else ++it;
    }
    LARGE_INTEGER tn1{};
    QueryPerformanceCounter(&tn1);
    slotsMs += Ms(tn0, tn1);
    g_why.nbMs += Ms(tn0, tn1);
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
            if (!now.count(L) && LogAllowed()) {
                LampRead r;
                const bool read = ReadLamp(L, r);
                LOG_INFO(std::format("[RtOutdoorLamps] GPU lamp {:08X}: drawn on stories {} -> none (switched off, or out of the lists){}", L, stories(m),
                                     read ? std::format("; now room {}, flags {:02X}, lot {:016X}, at ({:.2f} {:.2f} {:.2f}), {}", r.f.room, r.f.flags, r.f.lot, r.pos[0], r.pos[1],
                                                        r.pos[2], InTerrainBake(r) ? "in the terrain bake" : "out of the terrain bake")
                                          : std::string("; unreadable")));
            }
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
            const uint32_t m = g.listed; // (the lists: a story it is drawn on before its gather still gathers it again)
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
    // (physical indoor lamps, stage 4: an indoor lamp moved without a solve is a drag too: fast taps, one exact pass once still)
    // (physical) a drag the GPU takes stays a drag until the lamp is still (DragLite::kStillMs, as its rooms' solves): each pause of
    // 300 ms drew every touched map exact and the next step fast again, the furniture shadows' softness flipping (677 exact
    // relights in 10 s of a drag)
    const bool dragging = (g_lampChangedAt && nowTick - g_lampChangedAt < kDragQuietMs) || GpuRoomLamps::PhysMoveRecent(DragLite::kStillMs);
    // fewer shadow taps while dragged, but not for a drag the GPU takes: the fast taps'
    // harder shadows were the light the drag showed, the exact ones at the release another (users saw the drop "flicker")
    const bool fast = dragging && !GpuRoomLamps::PhysMoveRecent(DragLite::kStillMs);
    LARGE_INTEGER tp2{};
    QueryPerformanceCounter(&tp2);
    const double lampsMs = Ms(tp1, tp2) - Ms(tn0, tn1); // (the lots next to it count in slotsMs)
    g_rush.store(!allReady && g_rushFrom && GetTickCount() - g_rushFrom < kKeptRushMs, std::memory_order_relaxed);
    if (allReady) g_rushFrom = 0;
    if (g_slots.empty() || !allReady) {
        g_on.store(!g_slots.empty());
        Inactive(waiting.empty() ? (g_slots.empty() ? "no room-0 map of the active lot could be relit (see the lines above)" : "")
                                 : "waiting for kept room-0 solves: " + waiting.front() + (waiting.size() > 1 ? std::format(" (+{} more)", waiting.size() - 1) : ""));
        if (g_slots.empty()) return;
    } else {
        Inactive("");
        ReleaseRetiredIf(dev, [](const Retired& r) { return r.lot == g_tracker; }, "its lot is the active one again and relit");
        CompareWithGame(dev);
    }
    g_on.store(true);
    // ---- relight what changed ----
    LARGE_INTEGER t0{};
    QueryPerformanceCounter(&t0);
    int relit = 0, tilesBefore = static_cast<int>(g_eng->GetStats().shadowTiles), storiesRelit = 0, left = 0, nbRelit = 0;
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
    // while a lamp is dragged the maps are relit at every move with the cheaper shadows, as the ground outside the lot is
    // (a relight start every 200 ms left the lot's floors and walls up to that far behind the ground beside them; a relight
    // is 0.3-1.1 ms of CPU and under 1 ms of GPU in a recording of outdoor lamps; a drag the GPU takes draws every kept map, over
    // 400,000 points and 29 ms of GPU a round on a 13-room lot before its dense quads); every frame's work stops past kFrameCpuMs or kFrameGpuMs
    // (the maps left are relit the next frames)
    double gpuPlanned = 0, maskMs = 0, drawMs = 0, overMs = 0;
    double maskJobsMs = 0, maskShareMs = 0, maskLockMs = 0, maskFillMs = 0; // (the floor mask's parts)
    const size_t nSlots = g_slots.size();
    if (g_rr >= nSlots) g_rr = 0;
    // the maps without floor-mask texels (wall atlases, ceilings) first: they cost little, so every story's walls are relit in
    // the same frame and the seam between stories never shows two lamp positions. A floor map's under-floor share (about 1.6 ms
    // in a recording) went first in some rounds and sent the next stories' walls to the next frames (a flicker at the seam).
    // In each, the active lot's maps before those of the lots next to it, which never take its frame budget first
    std::vector<Slot*> order;
    order.reserve(nSlots);
    for (int pass = 0; pass < 4; pass++)
        for (size_t si = 0; si < nSlots; si++)
            if (Slot* sp = g_slots[(g_rr + si) % nSlots].get(); sp->masked.empty() == (pass < 2) && (sp->mgr != 0) == (pass % 2 == 1)) order.push_back(sp);
    // GPU room lamps (developer): each active-lot map's kept rooms and their lamps now; the maps whose kept rooms changed (a lamp
    // edit the GPU took) or whose room-0 kept lamps hold a lamp of an edit not yet shown go first, and past the frame budget up
    // to kEditFrameCpuMs, so an edit shows in all its maps in the round of its frame
    GpuRoomLamps::BeginFrame();
    g_lampNow.clear();
    std::unordered_map<const Slot*, uint64_t> roomsSigs;
    LARGE_INTEGER tr0{}, tr1{};
    QueryPerformanceCounter(&tr0);
    for (const auto& sp : g_slots)
        if (!sp->mgr && sp->built) roomsSigs[sp.get()] = GpuRoomLamps::Sig(dev, rns[sp->story].mgr, sp->cls, sp->m);
    QueryPerformanceCounter(&tr1);
    const double roomsMs = Ms(tr0, tr1);
    std::unordered_set<const Slot*> edited;
    for (const auto& sp : g_slots) {
        const auto it = roomsSigs.find(sp.get());
        if ((it != roomsSigs.end() && it->second != sp->roomsRelit) ||
            std::any_of(sp->kLamps.begin(), sp->kLamps.end(), [](const KeptInfo& k) { return GpuRoomLamps::EditPending(k.light); }))
            edited.insert(sp.get());
    }
    // the maps of an edit not yet shown there (a move's copy, a switch) before them, the floor first, and past every budget: a
    // moved lamp's floor waited 6 rounds (260 ms) behind other rooms' new kept solves and the wall atlases (2026-11-02)
    std::unordered_set<const Slot*> urgent;
    for (const auto& sp : g_slots)
        if (!sp->mgr && sp->built && GpuRoomLamps::ShowsEdit(rns[sp->story].mgr, sp->cls, sp->m)) urgent.insert(sp.get());
    const auto rank = [&](const Slot* sp) { return urgent.count(sp) ? (sp->m == 1 ? 0 : 1) : edited.count(sp) ? 2 : 3; };
    std::stable_sort(order.begin(), order.end(), [&](const Slot* a, const Slot* b) { return rank(a) < rank(b); });
    // (S6) the lot's registered lamps for the physical room-0 maps: copied once a frame, not once a map
    std::vector<OutdoorDelta::LotLamp> physLot;
    std::vector<uintptr_t> physGone;
    bool physLotRead = false;
    // (the GPU budget) a dragged lamp's own story: its maps of the edit go past every budget, the light under the cursor in the
    // frame; the edit's maps of other stories go first but within the GPU budget
    int dragStory = -1;
    if (const uintptr_t dl = GpuRoomLamps::PhysDragLight(DragLite::kStillMs)) {
        OutdoorDelta::LotLamps(physLot, &physGone), physLotRead = true;
        for (const OutdoorDelta::LotLamp& L : physLot)
            if (L.light == dl) dragStory = L.story;
    }
    for (Slot* sp : order) {
        Slot& s = *sp;
        // GPU room lamps (developer): a map of the active lot without room-0 texels that kept rooms wrote gets a copy of its own
        // (the game's map with those rooms over it), bound by the same swap
        const bool roomsOnly = !s.used && !s.mgr && s.built;
        const auto rsIt = roomsSigs.find(sp);
        const uint64_t roomsSig = rsIt != roomsSigs.end() ? rsIt->second : 0;
        if (roomsOnly) {
            if (!roomsSig) {
                if (s.copy) s.copy->Release(), s.copy = nullptr;
                ReleaseHi(s);
                s.partKey = 0;
                if (s.game) s.game->Release(), s.game = nullptr;
                s.relit = false;
                continue;
            }
            if (!s.game) {
                uintptr_t setAt = 0;
                IDirect3DTexture9* tex = OwnMap(s.room, s.cls, kSetIdx[s.m], setAt);
                D3DSURFACE_DESC d{};
                if (!tex || FAILED(tex->GetLevelDesc(0, &d)) || d.Format != D3DFMT_A8R8G8B8 || tex->GetLevelCount() != 1) continue;
                s.game = tex;
                tex->AddRef();
                s.map.w = static_cast<int>(d.Width), s.map.h = static_cast<int>(d.Height);
                s.relit = false;
            }
        } else if (!s.ok || !s.game)
            continue;
        // its story's lamps of the bake (the floor mask's under-floor share): the active lot's story, or a lot next to it's
        static const NbBake s_noBake;
        const auto nkIt = s.mgr ? g_nbBake.find(s.mgr) : g_nbBake.end();
        const NbBake* nk = nkIt != g_nbBake.end() ? &nkIt->second : s.mgr ? &s_noBake : nullptr;
        const std::vector<RtLampModel::BakeLamp>& sBake = nk ? nk->bake : bake[s.story];
        const std::vector<uint64_t>& sKeys = nk ? nk->keys : bakeKeys[s.story];
        const uint64_t sBakeSig = nk ? nk->sig : bakeSig[s.story];
        const float sBase = nk ? nk->base : rns[s.story].lotBase;
        // its GPU lamps: drawn now, in its story's list, and in its kept solve only as lamps whose parts left the base
        std::vector<RtLampModel::Lamp> mine;
        std::vector<const GpuLamp*> mineRef;
        for (const GpuLamp& g : lamps) {
            if (roomsOnly) break;
            // (a lot next to it: the posts only, as many times as the active lot lists them, as its gather took them)
            int mult = s.mgr ? 0 : g.mult[s.story];
            if (s.mgr && g.post)
                for (int S = 0; S <= 7; S++) mult = std::max(mult, g.mult[S]);
            if (!mult) continue;
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
            // a floor map counts the lamp once, as the terrain bake does under the grass, sidewalks and roads beside it (the
            // list's second entry made patios up to 45% brighter than the ground next to them)
            l.mult = s.m == 1 ? 1.0f : static_cast<float>(mult);
            if (!s.mgr && s.story == 0 && s.m == 1 && g.liftable && TerrainBakeRule::LowLampLift()) {
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
        if (!s.masked.empty()) Mix(sig, &sBakeSig, 8);
        for (const auto& l : mine) Mix(sig, &l, sizeof l);
        // GPU room lamps (developer, gpu_room_lamps.h): the kept rooms drawn over this map and their lamps as they are now, and room
        // 0's kept lamps' laws now (one moved, or a tube whose share changed: its law when kept, the light its solve gave)
        Mix(sig, &roomsSig, sizeof roomsSig);
        // (physical) the wall response of the rooms its texels' spaces continue into: a solve of one relights it with them; the
        // rooms' ambients and own wall maps now (+0x110, the InteriorWall's c4: Rooms at Night and a lamp edit move it) for the draws (InRoomsNow)
        for (size_t r = 0; r < s.inRooms.size() && r < 4; r++) {
            s.inFit[r] = OutdoorDelta::IndoorFit{};
            OutdoorDelta::IndoorFitOf(g_lotId, s.story - 1, s.inRooms[r], s.inFit[r]);
            Mix(sig, &s.inFit[r], sizeof s.inFit[r]);
        }
        InRoomsNow(dev, s, tracker);
        for (size_t r = 0; r < s.inRooms.size() && r < 4; r++) Mix(sig, &s.inBaseVer[r], sizeof s.inBaseVer[r]); // (InBaseNow: a new kept solve relights it)
        // a room its texels continue now shows another class, kept, than the one they were matched in: prepared again in that class's
        // layout (the old copy and binding stay meanwhile). A new kept solve of the same class needs none: InBaseNow
        bool inLag = false;
        for (size_t r = 0; r < s.inRooms.size() && r < 4 && !s.mgr; r++) {
            const int shown = ShownOf(g_lotId, s.story - 1, s.inRooms[r]);
            if (shown >= 0 && shown != s.inCls[r] && OutdoorDelta::IndoorVersionOf(g_lotId, s.story - 1, s.inRooms[r], shown)) s.built = false, inLag = true;
        }
        // how long its texels were matched in another class than their room shows
        if (inLag) {
            g_inSum.lag++;
            if (!s.inLagAt) s.inLagAt = GetTickCount(), s.inLagFrames = 0;
            s.inLagFrames++;
        } else if (s.inLagAt) {
            const DWORD ms = GetTickCount() - s.inLagAt;
            if (s.inLagFrames > 1)
                LOG_INFO(std::format("[RtOutdoorLamps] Story {} room 0: its texels over open rooms of story {} were matched in another class than their room shows for {} frames ({} ms)",
                                     s.story, s.story - 1, s.inLagFrames, ms));
            s.inLagAt = 0, s.inLagFrames = 0;
        }
        s.kLaws.resize(s.kLamps.size());
        s.kOk.assign(s.kLamps.size(), 0);
        for (size_t l = 0; l < s.kLamps.size(); l++) {
            const KeptInfo& k = s.kLamps[l];
            int type = 0, reg = -1;
            if (!k.colourOnly) {
                GpuLampLaw::Lamp now;
                if (WorldLaw(k.light, g_lotId, false, now, type, reg)) {
                    const bool moved = GpuLampLaw::Moved(now, k.law, 1e-4f); // (a tube: its end too)
                    s.kLaws[l] = moved ? k.law : now, s.kOk[l] = 1;
                }
            } else {
                float colour[3] = {};
                uint64_t geo = 0;
                if (ColourLaw(k.light, g_lotId, colour, geo)) {
                    s.kLaws[l] = k.law, s.kOk[l] = 2;
                    if (geo == k.geo || colour[0] + colour[1] + colour[2] <= 0.0f) std::memcpy(s.kLaws[l].colour, colour, sizeof colour); // (off: a switch)
                }
            }
            Mix(sig, &s.kOk[l], 1);
            if (s.kOk[l]) Mix(sig, &s.kLaws[l], sizeof s.kLaws[l]);
        }
        // (physical indoor lamps) room 0's lamps out of its base, each with its law now (a lamp gone or without a law adds nothing),
        // and the kept atlas's occluders
        if (s.pvb) {
            const uint32_t gen = KeptMeshGen();
            Mix(sig, &gen, sizeof gen);
            // (stage 4) every lot lamp with a law whose reach meets the map: the solve's lamps out of the base with their rows, and
            // the lamps its solve did not list (moved here, switched on, new) with fresh rows; never one the base holds (listed, not
            // out of it) nor an outdoor lamp the batches above draw
            if (!physLotRead) OutdoorDelta::LotLamps(physLot, &physGone), physLotRead = true;
            const std::vector<OutdoorDelta::LotLamp>& lot = physLot;
            const std::vector<uintptr_t>& gone = physGone;
            s.qLights.clear(), s.qRow.clear();
            std::vector<uintptr_t> cand;
            std::vector<std::array<float, 3>> candRow;
            for (size_t l = 0; l < s.pLights.size() && l < s.pRow.size(); l++) { // (a lamp deleted since: its light may be freed)
                if (std::find(gone.begin(), gone.end(), s.pLights[l]) != gone.end()) continue;
                cand.push_back(s.pLights[l]), candRow.push_back(s.pRow[l]);
                // (an indoor lamp the solve did not take as an exit lamp lights the ground too where its shadow tile sees it: the
                // exit ground is drawn from the lamps, ExportRoom0)
                const auto L = std::find_if(lot.begin(), lot.end(), [&](const OutdoorDelta::LotLamp& o) { return o.light == s.pLights[l]; });
                if (L != lot.end() && PhysLampPick::ExitFresh(s.story, L->story, L->home, L->homeOutdoor)) candRow.back()[2] = 1.0f;
            }
            for (const OutdoorDelta::LotLamp& L : lot) {
                // (a lamp this map's batches drew, out of room 0 since: out of the base too, drawn fresh here now)
                if ((s.kept.count(L.light) && !s.drawn.count(L.light)) || std::find(s.pLights.begin(), s.pLights.end(), L.light) != s.pLights.end()) continue;
                auto [now, first] = g_lampNow.try_emplace(L.light);
                if (first) {
                    LampRead r;
                    int type = 0, reg = -1;
                    now->second.read = ReadLamp(L.light, r);
                    now->second.drawn = now->second.read && RtLampModel::Drawn(r.f, g_lotId);
                    now->second.law = now->second.read && WorldLaw(L.light, g_lotId, false, now->second.w, type, reg);
                }
                if (!now->second.read || now->second.drawn) continue;
                cand.push_back(L.light);
                candRow.push_back({PhysLampPick::NormKFresh(L.story, L.home, L.homeNorm, s.story, 0, 1.0f, true, L.homeOutdoor), PhysLampPick::ReachFresh(L.homeOutdoor, true, L.story + 1 == s.story && RoomOpenAbove(L.story, L.home)) ? 1.0f : 0.0f,
                                   PhysLampPick::ExitFresh(s.story, L.story, L.home, L.homeOutdoor) ? 1.0f : 0.0f});
            }
            PhysLampPick::Box box;
            std::memcpy(box.lo, s.pBox, sizeof box.lo), std::memcpy(box.hi, s.pBox + 3, sizeof box.hi);
            s.pLaws.clear(), s.pWorld.clear(), s.pOk.clear();
            for (size_t l = 0; l < cand.size(); l++) {
                GpuLampLaw::Lamp w;
                RtLampModel::Lamp lotLaw;
                int type = 0, reg = -1;
                // (the law with its reach radii: the row's reach column and Params::reachOn decide where they apply)
                const auto now = g_lampNow.find(cand[l]);
                const bool law = now != g_lampNow.end() ? (w = now->second.w, now->second.law) : WorldLaw(cand[l], g_lotId, false, w, type, reg);
                if (!law || !PhysLampPick::Meets(box, w.pos, PhysLampPick::Reach2(w, candRow[l][0], s.thr)) ||
                    !WorldLaw(cand[l], g_lotId, true, lotLaw.law, type, reg, &rns[s.story].xf.r[0][0]))
                    continue;
                lotLaw.mult = candRow[l][0];
                s.qLights.push_back(cand[l]), s.qRow.push_back(candRow[l]), s.pLaws.push_back(lotLaw), s.pWorld.push_back({w.pos[0], w.pos[1], w.pos[2]}), s.pOk.push_back(1);
                Mix(sig, &cand[l], sizeof cand[l]), Mix(sig, &lotLaw, sizeof lotLaw), Mix(sig, candRow[l].data(), sizeof candRow[l]);
            }
            const size_t nq = s.qLights.size();
            Mix(sig, &nq, sizeof nq);
        }
        // the under-floor share from a worker (exact, UnderShare's sums): once no lamp is dragged, and (14/10) while one is,
        // in place of the render thread's UnderDelta below (up to 152 ms of a drag's relight; 26.8 ms in one frame): the last
        // share is kept until the worker's arrives, and a share of an earlier bake is taken during a drag (it follows the drag
        // one worker run behind; exact again once the drag ends)
        const auto startUnderJob = [&] {
            LARGE_INTEGER jt0{}, jt1{};
            QueryPerformanceCounter(&jt0);
            auto j = std::make_shared<UnderJob>();
            j->pos = s.maskedPos, j->gated = s.gated, j->bake = sBake, j->ids = sKeys, j->base = sBase;
            j->sig = UnderSig(sBakeSig, j->base);
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
            QueryPerformanceCounter(&jt1);
            maskJobsMs += Ms(jt0, jt1);
        };
        if (!s.masked.empty() && s.uJob && s.uJob->state.load() == 2) {
            LARGE_INTEGER jt0{}, jt1{};
            QueryPerformanceCounter(&jt0);
            const auto j = std::move(s.uJob);
            const bool current = j->sig == UnderSig(sBakeSig, sBase);
            if ((current || (dragging && j->sig != s.underSig)) && j->under.size() == s.masked.size()) {
                s.under = j->under, s.uA = j->A, s.uU = j->U, s.uSnap = j->bake, s.uIds = j->ids, s.uBase = j->base;
                s.underSig = j->sig, s.underExact = current, s.underDirty = true, s.dynCurrent = false, s.allCurrent = false;
            }
            QueryPerformanceCounter(&jt1);
            maskJobsMs += Ms(jt0, jt1);
        }
        if (!s.masked.empty() && !s.underExact && !dragging && !s.uJob) startUnderJob();
        const bool wrote = s.watch >= 0 && (dirtyBits[s.watch / 64] >> (s.watch % 64) & 1);
        bool solvedHere = false;
        for (const auto& [mgr, cls] : solved)
            if ((s.mgr ? s.mgr : rns[s.story].mgr) == mgr && cls == s.cls) solvedHere = true;
        if (solvedHere) s.pendingWrite = true;
        if (wrote) s.pendingWrite = true;
        const bool exactUnder = s.underDirty;
        // (the floor mask's "all" values that arrived on a drag frame were not written: relit once the drag ends)
        const bool allStale = !dragging && mine.size() > static_cast<size_t>(RtLampGpu::kAtlasLamps) && s.dyn && !s.masked.empty() && !s.allCurrent;
        const bool dirty = !s.relit || !s.copy || sig != s.sig || paramsChanged || (s.fast && !fast) || s.pendingWrite || s.underDirty || allStale;
        if (!dirty) {
            s.allWaitSince = 0; // (a waiting map is always dirty: it is not relit)
            continue;
        }
        // (2.15.1) the game writing the map again and again (a lot that stays loading: 105 writes in 10 s, each relight
        // 45 ms of floor mask with 28 lamps): a relight made only for a game write waits kGameWriteGapMs after the last one; a
        // single write after a rest is relit at once, as before
        const bool onlyWrite = s.relit && s.copy && sig == s.sig && !paramsChanged && !(s.fast && !fast) && !s.underDirty && !allStale;
        if (onlyWrite && GetTickCount64() - s.relitTick < kGameWriteGapMs) {
            // (it keeps its last copy and stays dirty until its turn; not counted in `left`: a lot whose game keeps writing
            // would keep the round open, and with it the drag throttle off)
            g_why.writeGap++;
            continue;
        }
        uint64_t lampsSig = 1469598103934665603ull;
        for (const auto& l : mine) Mix(lampsSig, &l, sizeof l);
        if (!s.masked.empty()) Mix(lampsSig, &sBakeSig, 8);
        // the budget: the first map of a frame always goes; later ones while there is time and work left
        LARGE_INTEGER tn{};
        QueryPerformanceCounter(&tn);
        // (GPU: the map's own time as last timed, else its texels x lamps; the lamp's own maps (an edit not shown there yet) always
        // go, so the light under the cursor follows in the frame; the other maps of an edit past the CPU budget up to kEditFrameCpuMs,
        // never past the GPU's: a drag the GPU takes drew 13 maps a frame in 98 ms of GPU, 9-12 fps)
        const double units = static_cast<double>(s.map.count) * static_cast<double>(std::max<size_t>(mine.size(), 1)) * (fast ? 1.0 : 7.0) +
                             (s.pvb ? static_cast<double>(s.nPhys) * static_cast<double>(s.qLights.size()) * 7.0 : 0.0);
        const double gpuEst = s.gpuMs > 0.0 ? s.gpuMs : units * kMsPerUnit;
        const bool cpuFits = Ms(t0, tn) <= (urgent.count(sp) || edited.count(sp) ? kEditFrameCpuMs : kFrameCpuMs);
        const bool mustGo = urgent.count(sp) && (dragStory < 0 || s.story == dragStory);
        if (relit > 0 && !mustGo && (!cpuFits || gpuPlanned + gpuEst > kFrameGpuMs)) {
            if (cpuFits) g_why.deferred++;
            left++; // (it keeps its last copy and stays dirty until its turn)
            continue;
        }
        gpuPlanned += gpuEst;
        if (!begun) {
            if (!g_eng->Begin(dev)) { // (no state block: nothing drawn this frame, the stories stay dirty)
                left++;
                break;
            }
            if (!g_qDisjoint && SUCCEEDED(dev->CreateQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT, nullptr))) {
                dev->CreateQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT, &g_qDisjoint), dev->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ, &g_qFreq);
                dev->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &g_qBegin), dev->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &g_qEnd);
            }
            timing = !g_qPending && g_qDisjoint && g_qFreq && g_qBegin && g_qEnd;
            if (timing) g_qDisjoint->Issue(D3DISSUE_BEGIN), g_qBegin->Issue(D3DISSUE_END);
            g_marking = timing, g_marks = 0;
            begun = true;
        }
        Mark(dev, kGpOther, nullptr); // (what came before this map)
        if (!s.copy &&
            (FAILED(dev->CreateTexture(s.map.w, s.map.h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &s.copy, nullptr)) || !s.copy)) {
            s.copy = nullptr;
            failNote = std::format("story {} {}: its copy could not be made", s.story, kMapName[s.m]);
            continue;
        }
        // the outdoor scale beside it (a device with two render targets). None: the draws read the copy as before, the light
        // past 1 cut
        if (!s.scale && g_mrt > 0 && !roomsOnly &&
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
        bool maskWait = false; // (the share's or "all"'s CPU work waits for a worker; this map keeps what it shows meanwhile)
        // one batch of lamps (the usual case): "all" is evaluated by the compose shader, and the buffer is written only when the
        // under-floor share changed; more lamps: "all" from the CPU (a worker) when the lamps change
        const bool maskGpu = mine.size() <= static_cast<size_t>(RtLampGpu::kAtlasLamps);
        if (s.dyn && !s.masked.empty()) {
            const uint64_t us = UnderSig(sBakeSig, sBase);
            uint64_t allSig = 0;
            if (!maskGpu) { // (what "all" depends on: the lamps' laws and mults, the threshold; the masked points only change with
                            // the slot's rebuild, which clears allVals unless its points are the same)
                allSig = 1469598103934665603ull;
                for (const auto& l : mine) Mix(allSig, &l, sizeof l);
                Mix(allSig, &s.thr, sizeof s.thr);
            }
            // the worker's "all", taken only for the lamps and threshold now (a slot's rebuild with new points drops its job); a
            // job that ended empty (no thread, an exception) leaves them to this thread
            bool allFailed = false;
            if (s.aJob && s.aJob->state.load() == 2) {
                const auto j = std::move(s.aJob);
                if (!maskGpu && j->sig == allSig) {
                    if (j->vals.size() == s.masked.size()) s.allVals = std::move(j->vals), s.allSig = allSig, s.allCurrent = false, s.allWaitSince = 0;
                    else allFailed = true;
                }
            }
            const bool allNew = !maskGpu && (allSig != s.allSig || s.allVals.size() != s.masked.size());
            bool write = maskGpu ? !s.dynCurrent : (allNew || !s.allCurrent);
            if (s.underSig != us) {
                if (dragging && (s.under.size() != s.masked.size() || s.uA.size() != s.masked.size() || s.uBase != sBase)) {
                    // (14/10, drag solves) never the whole share on this thread while a lamp is dragged (10-30 ms a relight in a
                    // recording): the worker computes it, this map waits for it with its last copy bound
                    if (!s.uJob) startUnderJob();
                    maskWait = true;
                } else if (!dragging && s.relit && s.copy && (!s.underWaitSince || GetTickCount64() - s.underWaitSince < kUnderWaitMs)) {
                    // a shown map: the share from the worker (the same FullUnder sums), not this thread (22-31 ms in one frame,
                    // 2026-11-10); its last copy stays bound meanwhile
                    if (!s.underWaitSince) s.underWaitSince = GetTickCount64();
                    if (!s.uJob) startUnderJob();
                    maskWait = true;
                } else if (s.under.size() != s.masked.size() || s.uA.size() != s.masked.size() || s.uBase != sBase) {
                    // (no sums to start from: the whole share, once; a slot from the worker always has them)
                    LARGE_INTEGER st0{}, st1{};
                    QueryPerformanceCounter(&st0);
                    s.uBase = sBase;
                    FullUnder(s.maskedPos, s.gated, sBake, s.uBase, s.under, s.uA, s.uU);
                    s.uSnap = sBake, s.uIds = sKeys;
                    s.underExact = true;
                    s.underSig = us;
                    write = true;
                    QueryPerformanceCounter(&st1);
                    maskShareMs += Ms(st0, st1);
                } else if (dragging) {
                    if (!s.uJob) startUnderJob(); // (the share follows from the worker; the last one stays meanwhile)
                } else {
                    LARGE_INTEGER st0{}, st1{};
                    QueryPerformanceCounter(&st0);
                    if (UnderDelta(s, sBake, sKeys)) {
                        s.underExact = false; // (exact again from the worker)
                        s.uJob.reset();
                    }
                    s.underSig = us;
                    write = true;
                    QueryPerformanceCounter(&st1);
                    maskShareMs += Ms(st0, st1);
                }
            }
            // new lamps: "all" from a worker (105-255 ms of this thread for a 45-lamp story's floor, one hitch each), never partial
            // values: the map keeps what it shows (its last copy; before its first relight, the game's map, as while it is
            // prepared) until they arrive. None during a drag (the last values stay, below)
            if (allNew && !dragging && !allFailed && (!s.allWaitSince || GetTickCount64() - s.allWaitSince < kAllWaitMs)) {
                if (!s.allWaitSince) s.allWaitSince = GetTickCount64();
                if (!s.aJob) {
                    LARGE_INTEGER jt0{}, jt1{};
                    QueryPerformanceCounter(&jt0);
                    auto j = std::make_shared<AllJob>();
                    j->lamps = mine, j->thr = s.thr, j->x87 = X87Cw(), j->mxcsr = _mm_getcsr(), j->sig = allSig, j->pos.resize(s.masked.size() * 3);
                    for (size_t i = 0; i < s.masked.size(); i++) std::memcpy(&j->pos[i * 3], &s.maskedPos[i * 6], 3 * sizeof(float));
                    s.aJob = j;
                    if (!HookGuard::StartDetached("RtOutdoorLamps floor mask job", [j] {
                            SetFp(j->x87, j->mxcsr); // (a new thread that ends after the job: nothing to restore)
                            try {
                                AllMask(j->lamps, j->pos.data(), 3, j->pos.size() / 3, j->thr, j->vals);
                            } catch (...) {
                                j->vals.clear();
                            }
                            std::vector<float>().swap(j->pos); // (its points' copy freed before the job is taken)
                            j->state.store(2);
                        })) {
                        j->vals.clear(); // (no thread: the job ends empty, as after an exception)
                        j->state.store(2);
                    }
                    QueryPerformanceCounter(&jt1);
                    maskJobsMs += Ms(jt0, jt1);
                }
                s.allCurrent = false; // (the share may change meanwhile: the buffer is written in full when they arrive)
                maskWait = true;
            }
            if (!maskWait) s.underDirty = false, s.underWaitSince = 0;
            // (the wait ends only when the values match the lamps, not on a drag frame: lamps that keep changing still reach kAllWaitMs)
            if (!allNew) s.allWaitSince = 0;
            // (a drag: with more lamps than one batch the shader cannot evaluate "all"; the CPU's per-texel pass waits for the
            // lamp to be still, the last values stay meanwhile)
            if (dragging && !maskGpu) write = false;
            void* q = nullptr;
            IDirect3DVertexBuffer9* const target = s.dynNext ? s.dynNext : s.dyn; // (the draws keep the current one until this one is filled)
            LARGE_INTEGER lk0{}, lk1{};
            QueryPerformanceCounter(&lk0);
            const bool locked = !maskWait && write && SUCCEEDED(target->Lock(0, 0, &q, 0)) && q;
            QueryPerformanceCounter(&lk1);
            maskLockMs += Ms(lk0, lk1);
            if (locked) {
                auto* dyn = static_cast<RtLampGpu::TexelDyn*>(q);
                if (allNew) { // (past kAllWaitMs or after a failed job; a job still running is dropped)
                    AllMask(mine, s.maskedPos.data(), 6, s.masked.size(), s.thr, s.allVals);
                    s.allSig = allSig;
                    s.allWaitSince = 0;
                    s.aJob.reset();
                }
                for (size_t i = 0; i < s.masked.size(); i++) dyn[s.masked[i]] = RtLampGpu::TexelDyn{{maskGpu ? 0.0f : s.allVals[i], s.under[i], 0, 0}};
                target->Unlock();
                if (s.dynNext) {
                    std::swap(s.dyn, s.dynNext);
                    s.map.dyn = s.dyn;
                }
                s.dynCurrent = maskGpu;
                s.allCurrent = !maskGpu;
                LARGE_INTEGER fl1{};
                QueryPerformanceCounter(&fl1);
                maskFillMs += Ms(lk1, fl1);
            }
        }
        QueryPerformanceCounter(&tm1);
        maskMs += Ms(tm0, tm1);
        if (maskWait) {
            left++; // (relit once the worker's values arrive)
            continue;
        }
        RtLampGpu::Params p;
        p.thr = s.thr, p.fade = fade, p.reachOn = reachOn, p.mask = !s.masked.empty(), p.radius = radius, p.aa = Penumbra::g_on.load(std::memory_order_relaxed) ? LevelLightShare::MapHalfTexelOf(s.room) : 0.0f, p.farDist = kFar;
        p.fast = fast;
        p.maskAllGpu = maskGpu;
        p.edgeR = edgeR;
        // (physical) its spaces' texels as their rooms' walls (ComposePS IndoorOf): the k of a kept room (all rooms share the game's)
        for (size_t r = 0; r < s.inRooms.size() && r < 4; r++) {
            const OutdoorDelta::IndoorFit& f = s.inFit[r];
            if (f.version) p.inK = f.k;
            std::memcpy(p.inFit[r], f.fit, sizeof f.fit);
        }
        p.inSoft = DevSwitches::ToneSoft();
        std::string err;
        bool ok = true;
        // (physical indoor lamps) room 0's lamps out of its base first, from their laws (the kept atlas's tiles, room 0's 3D test
        // records), their floor mask and exit light into the second sum; the outdoor batches then add to the sum (their constants
        // stay bound for the compose's floor mask). Failed: this map shows the game's and the room is prepared again with its
        // lamps in the base
        const bool phys = !roomsOnly && s.pvb;
        std::vector<RtLampModel::Lamp> pl;
        std::vector<std::array<float, 3>> pw, prow;
        std::vector<uintptr_t> plights;
        // (the sums at a map's points: its own, or room 0's floor at the sharper size)
        const auto physSums = [&](const RtLampGpu::Map& pm, std::string& e) {
            return g_eng->SetBlockers(dev, s.pBlk, e) &&
                   GpuRoomLamps::PhysPasses(dev, *g_eng, plights.data(), pl.data(), static_cast<int>(pl.size()), kFar, e, [&](int f, int c, const int* tiles, bool) {
                       RtLampGpu::Engine::PhysRoom room;
                       room.header = s.pHeader;
                       room.lamp.assign(prow.begin() + f, prow.begin() + f + c);
                       for (int i = 0; i < c; i++) {
                           room.fill.push_back(s.pBlk ? s.pBlk->FillOf(static_cast<int>(s.pHeader), pw[f + i].data()) : 0.0f);
                       }
                       return g_eng->AccumulatePhys(dev, pm, pl.data() + f, tiles, reinterpret_cast<const float (*)[3]>(pw.data() + f), c, room, p, f == 0, e);
                   });
        };
        // batches of the atlas's lamps (their shadows rendered unless the atlas holds them already)
        const auto outdoorSums = [&](const RtLampGpu::Map& tm, std::string& e) {
            bool r = true;
            for (size_t b = 0; r && (b < mine.size() || b == 0); b += RtLampGpu::kAtlasLamps) {
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
                    r = g_eng->RenderShadows(dev, pos, n, kFar, e);
                    g_atlasKey = r ? key : 0;
                    QueryPerformanceCounter(&ts1);
                    g_ph.shadows += Ms(ts0, ts1);
                }
                r = r && g_eng->Accumulate(dev, tm, mine.data() + std::min(b, mine.size()), n, p, b == 0 && !phys, e);
                if (mine.empty()) break;
            }
            return r;
        };
        if (phys) {
            for (size_t l = 0; l < s.qLights.size(); l++)
                if (s.pOk[l]) pl.push_back(s.pLaws[l]), pw.push_back(s.pWorld[l]), prow.push_back(s.qRow[l]), plights.push_back(s.qLights[l]);
            RtLampGpu::Map pm;
            pm.w = s.map.w, pm.h = s.map.h, pm.vb = s.pvb, pm.count = s.nPhys;
            ok = physSums(pm, err);
            Mark(dev, kGpRoom0, sp);
            if (!ok) {
                g_physRefused[s.room] = s.version;
                PhysNote(s.room, s.story, "its physical draw failed: " + err);
                GpuRoomLamps::NoteRoom0(s.story, "from its solve (its physical draw failed)");
                s.relit = false, s.version = 0; // (the game's map until it is prepared again)
                continue;
            }
            GpuRoomLamps::NoteRoom0(s.story, std::format("physical ({} lamps)", pl.size()));
        }
        if (!roomsOnly && ok) ok = outdoorSums(s.map, err);
        Mark(dev, kGpOutdoor, sp);
        // (GPU room lamps) room 0's kept lamps added to the sum before the compose
        if (!roomsOnly && ok && s.kvb && !s.kRanges.empty())
            ok = g_eng->AddKeptParts(dev, s.kvb, s.kRanges, s.kLaws.data(), s.kOk.data(), s.thr, s.map.w, s.map.h, err, s.kX);
        Mark(dev, kGpOther, sp);
        p.keptX = (!roomsOnly && s.kvb && !s.kRanges.empty() && s.kX) || phys;
        p.physX = phys;
        // (rooms only) the copy as its last relight left it, the game's map not written since and the same rules: only the rooms
        // whose draws changed are drawn again over it (GpuRoomLamps::ComposeOver: identical to a full draw, else it refuses)
        uint64_t partKey = 0;
        if (roomsOnly) {
            partKey = 1469598103934665603ull;
            const float pf[] = {p.thr, p.radius, p.aa, p.normalOffset, p.biasM, p.biasTexels, p.farDist, p.edgeR};
            const bool pb[] = {p.fade, p.reachOn, p.mask, p.fast, p.maskAllGpu, p.keptX, p.physX};
            Mix(partKey, pf, sizeof pf), Mix(partKey, pb, sizeof pb), Mix(partKey, &p.debugLamp, sizeof p.debugLamp), Mix(partKey, &s.copy, sizeof s.copy);
            Mix(partKey, &s.game, sizeof s.game), Mix(partKey, &s.map.w, sizeof s.map.w), Mix(partKey, &s.map.h, sizeof s.map.h), Mix(partKey, &s.version, sizeof s.version);
        }
        LARGE_INTEGER to0{}, to1{};
        QueryPerformanceCounter(&to0);
        const bool partial = roomsOnly && ok && s.relit && s.partKey == partKey && !s.pendingWrite &&
                             GpuRoomLamps::ComposeOver(dev, *g_eng, rns[s.story].mgr, s.cls, s.m, s.game, s.copy, s.map.w, s.map.h, p, true);
        QueryPerformanceCounter(&to1);
        overMs += Ms(to0, to1);
        if (!partial) {
            // (the copy as its last relight left it, the game's map not written since: the kept rooms whose draws did not change are
            // copied back from it after room 0's compose instead of drawn again, GpuRoomLamps::ComposeOver)
            IDirect3DTexture9* before = nullptr;
            if (ok && !s.mgr && s.relit && !s.pendingWrite && GpuRoomLamps::KeepsRooms(rns[s.story].mgr, s.cls, s.m, s.copy, s.game, s.map.w, s.map.h, p)) {
                std::string kerr;
                before = g_eng->KeepCopy(dev, s.copy, s.map.w, s.map.h, kerr);
            }
            RtLampGpu::Params pc = p;
            pc.inBase = s.inBase; // (this map's texels only: not the sharper floor's)
            ok = ok && (roomsOnly ? g_eng->CopyTexture(dev, s.game, s.copy, s.map.w, s.map.h, err) : g_eng->Compose(dev, s.map, s.game, s.copy, pc, err, s.scale));
            Mark(dev, s.inRooms.empty() ? kGpCompose : kGpCarry, sp);
            QueryPerformanceCounter(&to0);
            if (ok && !s.mgr) GpuRoomLamps::ComposeOver(dev, *g_eng, rns[s.story].mgr, s.cls, s.m, s.game, s.copy, s.map.w, s.map.h, p, false, before);
            QueryPerformanceCounter(&to1);
            overMs += Ms(to0, to1);
        } else
            g_why.partial++;
        Mark(dev, kGpRooms, sp);
        s.partKey = ok ? partKey : 0;
        s.scaled = ok && s.scale;
        if (ok) s.inFlags = s.scaled && !s.inRooms.empty();
        // (sharper floors) the physical rooms' floor at twice the map's size, read instead of the copy while it holds any room's
        s.hiOk = false;
        if (ok && !s.mgr && s.m == 1 && OutdoorDelta::HiFloors() && OutdoorDelta::RoomLampPhysical()) {
            const int w2 = 2 * s.map.w, h2 = 2 * s.map.h;
            bool fresh = false;
            const auto make = [&](IDirect3DTexture9*& t) {
                if (t) return true;
                fresh = true;
                for (const D3DFORMAT f : {RtLampGpu::SharperFormat(dev), D3DFMT_A8R8G8B8}) // (out of memory for 16 bits: the 8-bit target, its dither and pack)
                    if (SUCCEEDED(dev->CreateTexture(w2, h2, 1, D3DUSAGE_RENDERTARGET, f, D3DPOOL_DEFAULT, &t, nullptr)) && t) return true;
                t = nullptr;
                return false;
            };
            if (make(s.hi) && make(s.hiOver)) {
                if (fresh) {
                    D3DSURFACE_DESC hd{};
                    s.hi->GetLevelDesc(0, &hd);
                    const bool wide = hd.Format == D3DFMT_A16B16G16R16F;
                    LOG_INFO(std::format("[RtOutdoorLamps] Story {} floor map: sharper copy {}x{} {} ({:.1f} MB with its overlay)", s.story, w2, h2, wide ? "16-bit float" : "8-bit",
                                         2.0 * w2 * h2 * (wide ? 8 : 4) / (1024.0 * 1024.0)));
                }
                // (the overlay holds the rooms' own sub-texels, never the game's map: drawn in full again only when the rules
                // RelightPhys reads change, not on every full relight of the copy)
                uint64_t hiRules = 1469598103934665603ull;
                const float hf[] = {p.radius, p.aa, p.normalOffset, p.biasM, p.biasTexels, p.farDist, p.edgeR};
                const bool hb[] = {p.fade, p.reachOn, p.fast};
                Mix(hiRules, hf, sizeof hf), Mix(hiRules, hb, sizeof hb);
                const int rooms = GpuRoomLamps::ComposeHi(dev, *g_eng, rns[s.story].mgr, s.cls, s.m, s.copy, s.map.w, s.map.h, s.hiOver, fresh || s.hiRules != hiRules, s.hi, p);
                s.hiRules = hiRules;
                // room 0's own floor at that size too (physical, no kept parts: their points are the game's texels'): its sums at the
                // sub-texels, composed into hiRoom0, its places copied over the upsampled copy
                bool own = false;
                if (rooms >= 0 && phys && s.vbHi && s.kRanges.empty() && make(s.hiRoom0)) {
                    RtLampGpu::Map hm;
                    hm.w = w2, hm.h = h2, hm.vb = s.vbHi, hm.count = s.nHi;
                    RtLampGpu::Map hp = hm;
                    hp.vb = s.pvbHi;
                    RtLampGpu::KeptMapGpu places;
                    places.placesHi = s.placesHi, places.nPlacesHi = s.nHi;
                    std::string e;
                    own = physSums(hp, e) && outdoorSums(hm, e) && g_eng->Compose(dev, hm, nullptr, s.hiRoom0, p, e) &&
                          g_eng->CopyPlaces(dev, places, s.hiRoom0, s.hi, w2, h2, e);
                    if (!own && !s.hiLogged)
                        s.hiLogged = true, LOG_INFO(std::format("[RtOutdoorLamps] Story {} floor map: room 0's sharper floor not drawn ({})", s.story, e));
                }
                s.hiOk = rooms > 0 || own;
            } else
                ReleaseHi(s);
        } else if (s.hi && !OutdoorDelta::HiFloors())
            ReleaseHi(s);
        Mark(dev, kGpHi, sp);
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
            if ((s.fast && !fast) || exactUnder) g_why.exact++;
        }
        s.lampsSig = lampsSig, s.paramsSig = params, s.meshGen = g_meshGen, s.relitVersion = s.version;
        s.roomsRelit = roomsSig;
        s.relit = true;
        s.sig = sig;
        s.fast = fast;
        s.pendingWrite = false;
        s.relitTick = GetTickCount64();
        relit++;
        LARGE_INTEGER td{};
        QueryPerformanceCounter(&td);
        drawMs += Ms(tm1, td);
        if (s.mgr) nbRelit++;
        else if (std::find(storyList.begin(), storyList.end(), s.story) == storyList.end()) storyList.push_back(s.story);
    }
    if (begun) {
        begun = false; // (ended here, not by endOnThrow)
        g_eng->End(dev);
        if (timing) g_qEnd->Issue(D3DISSUE_END), g_qDisjoint->Issue(D3DISSUE_END), g_qFreq->Issue(D3DISSUE_END), g_qPending = true;
        g_marking = false;
    }
    if (!failNote.empty() && LogAllowed()) LOG_INFO("[RtOutdoorLamps] Not relit: " + failNote);
    LARGE_INTEGER t1{};
    QueryPerformanceCounter(&t1);
    const double ms = Ms(t0, t1);
    g_why.loopMs += ms;
    // coverage: every story's maps relit and swapped (lamp edits mark no room then)
    bool full = allReady && stories > 0;
    for (const auto& s : g_slots) full = full && (s->mgr || !s->used || (s->ok && s->relit));
    {
        // (14/10) not a lamp a covering slot keeps baked in its base (kept when it was not drawn): its edits must reach the
        // game's solve, the GPU does not draw it there
        // nor a lamp not drawn on a story where it is drawable (switched off there). A lamp drawn on a story before it gathers
        // it counts: before, a picked-up lamp took the game's solve until the consistency repair (about 4 s), and a drag of it
        // held the per-pixel lamps of doors, windows and outdoor objects, and the furniture, until the release
        std::vector<uintptr_t> covered;
        covered.reserve(lamps.size());
        uint32_t storyBits = 0;
        for (int S = 0; S <= 7; S++)
            if (rooms[S]) storyBits |= 1u << S;
        for (const GpuLamp& g : lamps) {
            bool baked = false;
            for (const auto& s : g_slots) baked = baked || (!s->mgr && s->used && s->kept.count(g.light) && !s->drawn.count(g.light));
            uint32_t drawnOn = 0;
            for (int S = 0; S <= 7; S++)
                if (g.mult[S]) drawnOn |= 1u << S;
            if (!baked && (drawnOn & storyBits) != storyBits) {
                LampRead r;
                if (!ReadLamp(g.light, r)) baked = true;
                for (int S = 0; S <= 7 && !baked; S++)
                    if ((storyBits & ~drawnOn) >> S & 1) baked = RtLampModel::Drawn(r.f, rns[S].lot);
            }
            if (!baked) covered.push_back(g.light);
        }
        std::lock_guard<std::mutex> lk(g_coverMx);
        g_cover.tracker = tracker;
        g_cover.full = full;
        g_cover.at = GetTickCount();
        g_cover.lamps.clear();
        g_cover.lamps.insert(covered.begin(), covered.end());
        g_cover.kept0.clear();
        for (const auto& s : g_slots)
            if (!s->mgr && s->used && s->ok && s->relit)
                for (const KeptInfo& k : s->kLamps) g_cover.kept0.insert({s->story, k.light});
        g_cover.phys0.clear();
        for (int S = 0; S <= 7; S++)
            if (rooms[S] && std::all_of(g_slots.begin(), g_slots.end(), [&](const auto& s) { return s->mgr || s->story != S || s->room != rooms[S] || !s->used || (s->pvb && s->ok && s->relit); }))
                g_cover.phys0.insert(rooms[S]);
    }
    // the coverage, logged when it changes (lamp edits mark no room only while every story is covered)
    if (static_cast<int>(full) != g_coverLogged) {
        g_coverLogged = full ? 1 : 0;
        std::string why;
        if (!full) {
            if (!waiting.empty()) why = waiting.front() + (waiting.size() > 1 ? std::format(" (+{} more)", waiting.size() - 1) : "");
            for (const auto& s : g_slots)
                if (why.empty() && !s->mgr && s->used && !(s->ok && s->relit))
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
            g_roundCpuMs = 0, g_roundMaps = 0, g_roundFrames = 0, g_roundTiles = 0, g_roundNb = 0;
            g_roundStories.clear();
            g_ph = Phases{};
        }
        g_roundCpuMs += ms + slotsMs + lampsMs;
        g_roundMaps += relit, g_roundFrames++, g_roundTiles += tiles, g_roundNb += nbRelit;
        g_ph.slots += slotsMs, g_ph.lamps += lampsMs, g_ph.mask += maskMs, g_ph.draw += drawMs, g_ph.over += overMs, g_ph.rooms += roomsMs;
        g_ph.maskJobs += maskJobsMs, g_ph.maskShare += maskShareMs, g_ph.maskLock += maskLockMs, g_ph.maskFill += maskFillMs;
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
        const std::string phases = std::format("phases: maps read {:.2f}, lamps read {:.2f}, floor mask {:.2f} (worker jobs started and taken {:.2f}, share here {:.2f}, "
                                               "buffer lock {:.2f}, buffer fill {:.2f}), shadows {:.2f}, draws {:.2f} (of them the kept rooms over room 0 {:.2f}), "
                                               "the kept rooms' checks {:.2f}, the maps' checks and the rest {:.2f} ms; {} frame{}{}{}",
                                               g_ph.slots, g_ph.lamps, g_ph.mask, g_ph.maskJobs, g_ph.maskShare, g_ph.maskLock, g_ph.maskFill,
                                               g_ph.shadows, g_ph.draw - g_ph.shadows, g_ph.over, g_ph.rooms,
                                               std::max(0.0, g_roundCpuMs - g_ph.slots - g_ph.lamps - g_ph.mask - g_ph.draw - g_ph.rooms), g_roundFrames,
                                               g_roundFrames == 1 ? "" : "s", fast ? ", dragged (cheaper shadows)" : dragging ? ", dragged" : "",
                                               g_roundNb ? std::format("; {} of the maps on the lots next to it", g_roundNb) : std::string());
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
        g_openUsed.reset(), g_openGen.store(0), g_openFailAt = 0, StairSet::g_set.Clear();
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
            if (!g_mrt) LOG_INFO("[RtOutdoorLamps] The device draws into one render target at a time: outdoor light past 1 is not shown");
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
    g_holdAt = 0, g_holdFor = 0, g_switchAt = 0;
    g_restartRequested = false;
    PublishNeighbours(0, {}, false);
    DropAll(g_dev, "the world changed");
    g_proxyUsed.reset();
    g_openUsed.reset(), g_openGen.store(0), g_openFailAt = 0, StairSet::g_set.Clear();
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
// The active lot's stories being prepared (the lots next to it never hold the restart)
bool ActiveJobs() { return std::any_of(g_jobs.begin(), g_jobs.end(), [](const std::shared_ptr<Job>& j) { return !j->lot; }); }
bool RestartIncomplete() {
    if (!g_option.load(std::memory_order_relaxed) || !g_watch.load(std::memory_order_relaxed) || ActiveJobs() || g_restartGpuPending) return false;
    if (std::any_of(g_slots.begin(), g_slots.end(), [](const auto& slot) { return !slot->mgr && slot->used && slot->built && slot->ok && !slot->relit; })) return false;
    if (g_restartGpuFallback) return true;
    if (std::any_of(g_slots.begin(), g_slots.end(), [](const auto& slot) { return !slot->mgr && slot->used && slot->built && !slot->ok; })) return true;
    std::lock_guard<std::mutex> lock(g_coverMx);
    return g_budgetOut && !g_cover.full;
}
bool RestartPending() {
    if (g_restartRequested) return true;
    if (RestartIncomplete()) return false;
    if (!g_option.load(std::memory_order_relaxed) || !g_watch.load(std::memory_order_relaxed)) return false;
    if (g_restartGpuPending || ActiveJobs() || !g_proxyUsed || !g_on.load(std::memory_order_relaxed)) return true;
    if (std::any_of(g_slots.begin(), g_slots.end(), [](const auto& slot) { return !slot->mgr && slot->used && !slot->relit; })) return true;
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
        if (g_proxy && g_proxy->tracker == tracker && geo && g_proxyGeo == geo) return;
    }
    auto p = std::make_shared<Proxy>();
    p->tracker = tracker;
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
    // (read again the same: the proxy kept, its generation too, so no map is relit nor tile rendered again for occluders that did
    // not change; its geometry key the new one)
    if (g_proxy && g_proxy->tracker == tracker && g_proxy->lot == p->lot) {
        g_proxyGeo = geo;
        g_proxyWhy.clear();
        LOG_INFO(std::format("[RtOutdoorLamps] Occluders read again for tracker {:08X} in {:.2f} ms: the same as generation {}, kept (light tree thread)", tracker, Ms(a, b),
                             g_proxy->gen));
        return;
    }
    p->gen = g_proxyGen.fetch_add(1) + 1;
    size_t edges = 0;
    for (const auto& s : p->lot.stories) edges += s.edges.size();
    g_proxy = p;
    g_proxyGeo = geo;
    g_proxyWhy.clear();
    c_proxyBuilds.fetch_add(1, std::memory_order_relaxed);
    LOG_INFO(std::format("[RtOutdoorLamps] Occluders read for tracker {:08X}: {} stories, {} wall edges, in {:.2f} ms (light tree thread)", tracker, p->lot.stories.size(), edges, Ms(a, b)));
}
} // namespace

uintptr_t TrackerOf(const void* room) {
    int story = -99, id = -1, cls = -1;
    uintptr_t mgr = 0;
    return room ? TrackerOfRoom(room, story, id, cls, mgr) : 0;
}
bool CoversRoom(const void* room) {
    if (!g_on.load(std::memory_order_relaxed) || !room) return false;
    int story = -99, id = -1, cls = -1;
    uintptr_t mgr = 0;
    const uintptr_t tracker = TrackerOfRoom(room, story, id, cls, mgr);
    if (!tracker || id != 0) return false;
    std::lock_guard<std::mutex> lk(g_coverMx);
    return g_cover.tracker == tracker && g_cover.full && GetTickCount() - g_cover.at < kCoverFreshMs;
}
uintptr_t ActiveTracker() { return g_proxyWanted.load(); }
bool KeptRush() { return g_rush.load(std::memory_order_relaxed); }
bool PhysCoversRoom0(const void* room0) {
    if (!CoversRoom(room0)) return false;
    std::lock_guard<std::mutex> lk(g_coverMx);
    return g_cover.phys0.count(room0) != 0;
}
bool CoversLamp(const void* room0, uintptr_t light) {
    if (!g_on.load(std::memory_order_relaxed) || !room0 || !light) return false;
    int story = -99, id = -1, cls = -1;
    uintptr_t mgr = 0;
    const uintptr_t tracker = TrackerOfRoom(room0, story, id, cls, mgr);
    if (!tracker || id != 0) return false;
    std::lock_guard<std::mutex> lk(g_coverMx);
    return g_cover.tracker == tracker && g_cover.full && GetTickCount() - g_cover.at < kCoverFreshMs &&
           (g_cover.lamps.count(light) || g_cover.kept0.count({story, light}));
}
bool ColourLaw(uintptr_t light, uint64_t lot, float colour[3], uint64_t& geo) {
    LampRead r;
    if (!ReadLamp(light, r) || r.f.type != 6 || r.f.lot == 0 || r.f.lot != lot || !(r.f.flags & 0x01)) return false;
    uint32_t g[10] = {};
    if (!ReadTubeShare(light, g)) return false;
    geo = 1469598103934665603ull;
    Mix(geo, g, sizeof g);
    // switched off (its lit bit +0x100 & 0x20 clear, or its intensity 0: the share is 0): no colour, whatever its share reads
    const bool lit = (r.f.flags & 0x20) && r.intensity > 0.0f;
    for (int k = 0; k < 3; k++) {
        if (!std::isfinite(r.colour[k])) return false;
        colour[k] = lit ? r.colour[k] : 0.0f;
    }
    return true;
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
void NoteLampEdit(uintptr_t tl, uintptr_t light, int kind, bool settle) {
    int story = -99;
    const uintptr_t tracker = TrackerOfTl(tl, story);
    if (!tracker) return;
    g_tookEdit.store(tracker, std::memory_order_relaxed);
    c_edits.fetch_add(1, std::memory_order_relaxed);
    EditNote e;
    e.tracker = tracker, e.light = light, e.story = story, e.kind = kind;
    QueryPerformanceCounter(&e.at);
    {
        std::lock_guard<std::mutex> lk(g_editMx);
        if (g_edits.size() < 256) g_edits.push_back(e);
        if (story >= 0 && story <= 7) { // (light tree thread; cleared by the render thread)
            SendDue& d = g_sendDue[{tracker, story}];
            d.at = std::max<DWORD>(GetTickCount(), 1);
            d.moved = d.moved || kind != 0;
            d.settle = d.settle || settle;
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
        bool settle = false;
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
            d.settle = d.settle || it->second.settle;
            for (uintptr_t l : it->second.lamps)
                if (std::find(d.lamps.begin(), d.lamps.end(), l) == d.lamps.end()) d.lamps.push_back(l);
            it = g_sendDue.erase(it);
        }
    }
    for (const auto& [tracker, d] : due) {
        // (physical indoor lamps, stage 4) every story 0 .. 7 of the active lot draws the edited lamps from their laws in every room
        // > 0 (OutdoorDelta::PhysCoversLamp): a move or a colour change sends none of those rooms. A switch or an intensity change
        // still sends them, as an indoor lamp's settle (their normalisation, ambient and bounce in their bases take its light)
        if (!d.settle && !d.lamps.empty() && d.lamps.size() < 64 && OutdoorDelta::RoomLampPhysical() &&
            std::all_of(d.lamps.begin(), d.lamps.end(), [tracker](uintptr_t l) { return OutdoorDelta::PhysCoversLamp(tracker, l); })) {
            c_physUnsent.fetch_add(1, std::memory_order_relaxed);
            if (LogAllowed())
                LOG_INFO(std::format("[RtOutdoorLamps] physical: the rooms that take room 0's lamps not sent after the edit of {} lamps (every story draws them from their laws)",
                                     d.lamps.size()));
            continue;
        }
        c_sends.fetch_add(LevelLightShare::SendRoom0DependentsTail(tracker, d.mask, &d.lamps), std::memory_order_relaxed);
    }
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
    if (!texture) return;
    if (g_retiredAny.load(std::memory_order_relaxed)) { // (a retired copy's game map written: the copy is stale)
        std::lock_guard<std::mutex> lk(g_retiredMx);
        if (g_retiredGame.count(texture)) g_retiredStale.push_back(texture);
    }
    if (!g_watch.load(std::memory_order_relaxed)) return;
    for (int i = 0; i < kMaxWatch; i++)
        if (g_watchTex[i].load(std::memory_order_relaxed) == texture) g_watchDirty[i / 64].fetch_or(1ull << (i % 64), std::memory_order_relaxed);
}
namespace {
// a room's ambient (+0x110), POD (SEH)
bool ReadRoomAmbient(const void* room, float* c) {
    if (!room) return false;
    __try {
        std::memcpy(c, static_cast<const BYTE*>(room) + 0x110, 3 * sizeof(float));
        return c[0] == c[0] && c[1] == c[1] && c[2] == c[2];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// (physical) the marked texels' light without the GPU lamps as their rooms' kept solves give it now, for the compose (InBase): a
// new kept solve of a room in the class its texels were matched in updates their entries (a wall texel its matched top-row texel's,
// by its place in the room's map; a floor texel the top row's mean) and the map, in the frame it is kept, so the ratio's base and
// the floors' light follow their room with no prepare (a prepare lagged them 100-500 ms per room solve: the "older kept solve" lines)
// A texture the compose reads, filled on the CPU: made DEFAULT and filled through a SYSTEMMEM texture freed at once. MANAGED
// kept a full copy in the game's address space (under the RTX Remix bridge, freed after 2 s idle and fetched back, a wait, at the
// next lock). DEFAULT ones are released at a device reset (OnPreReset) and filled again. fill(bits, pitch); false: none made
template <typename Fill> IDirect3DTexture9* MakeFilledTexture(IDirect3DDevice9* dev, UINT w, UINT h, D3DFORMAT fmt, IDirect3DTexture9* reuse, Fill fill) {
    IDirect3DTexture9 *stage = nullptr, *tex = reuse;
    if (FAILED(dev->CreateTexture(w, h, 1, 0, fmt, D3DPOOL_SYSTEMMEM, &stage, nullptr)) || !stage) return nullptr;
    D3DLOCKED_RECT lr{};
    bool ok = SUCCEEDED(stage->LockRect(0, &lr, nullptr, 0));
    if (ok) fill(static_cast<BYTE*>(lr.pBits), static_cast<size_t>(lr.Pitch)), stage->UnlockRect(0);
    if (ok && !tex && (FAILED(dev->CreateTexture(w, h, 1, 0, fmt, D3DPOOL_DEFAULT, &tex, nullptr)) || !tex)) tex = nullptr, ok = false;
    ok = ok && SUCCEEDED(dev->UpdateTexture(stage, tex));
    stage->Release();
    if (!ok && tex && tex != reuse) tex->Release();
    return ok ? tex : nullptr;
}
void InBaseNow(IDirect3DDevice9* dev, Slot& s) {
    if (s.inBaseVal.size() != s.inUvSrc.size() || s.inUvSrc.empty()) return;
    for (size_t r = 0; r < s.inRooms.size() && r < 4; r++) {
        if (s.inCls[r] < 0) continue;
        const uint32_t v = OutdoorDelta::IndoorVersionOf(g_lotId, s.story - 1, s.inRooms[r], s.inCls[r]);
        if (!v || v == s.inBaseVer[r]) continue;
        std::vector<OutdoorDelta::TopTexel> top;
        uint32_t ver = 0;
        int cls = s.inCls[r];
        if (!OutdoorDelta::IndoorTopOf(g_lotId, s.story - 1, s.inRooms[r], top, ver, cls) || cls != s.inCls[r]) continue;
        std::unordered_map<uint32_t, const OutdoorDelta::TopTexel*> at;
        double mean[3] = {};
        for (const auto& t : top) {
            at[static_cast<uint32_t>(t.x) << 16 | t.y] = &t;
            for (int k = 0; k < 3; k++) mean[k] += t.rgb[k] / static_cast<double>(top.size());
        }
        for (size_t i = 0; i < s.inUvSrc.size(); i++) {
            const InUv& e = s.inUvSrc[i];
            if (static_cast<size_t>(e.slot) != r) continue;
            if (e.rx == 0xFFFF) {
                for (int k = 0; k < 3; k++) s.inBaseVal[i][k] = static_cast<float>(mean[k]);
            } else if (const auto it = at.find(static_cast<uint32_t>(e.rx) << 16 | e.ry); it != at.end())
                std::memcpy(s.inBaseVal[i].data(), it->second->rgb, sizeof s.inBaseVal[i]); // (a texel not in the new top row keeps its last)
        }
        s.inBaseVer[r] = ver, s.inBaseDirty = true;
    }
    D3DSURFACE_DESC ad{}, bd{};
    if (!s.inBaseDirty || FAILED(s.game->GetLevelDesc(0, &ad))) return;
    if (s.inBase && (FAILED(s.inBase->GetLevelDesc(0, &bd)) || bd.Width != ad.Width || bd.Height != ad.Height)) s.inBase->Release(), s.inBase = nullptr;
    IDirect3DTexture9* const made = MakeFilledTexture(dev, ad.Width, ad.Height, D3DFMT_A16B16G16R16F, s.inBase, [&](BYTE* bits, size_t pitch) {
        for (UINT y = 0; y < ad.Height; y++) std::memset(bits + static_cast<size_t>(y) * pitch, 0, static_cast<size_t>(ad.Width) * 8);
        for (size_t i = 0; i < s.inUvSrc.size(); i++) {
            const InUv& e = s.inUvSrc[i];
            if (e.x >= ad.Width || e.y >= ad.Height) continue;
            uint16_t* px = reinterpret_cast<uint16_t*>(bits + static_cast<size_t>(e.y) * pitch) + static_cast<size_t>(e.x) * 4;
            px[0] = RtLampProxy::Half(s.inBaseVal[i][0]), px[1] = RtLampProxy::Half(s.inBaseVal[i][1]), px[2] = RtLampProxy::Half(s.inBaseVal[i][2]), px[3] = 0x3C00; // (alpha 1)
        }
    });
    if (!made) return; // (the vertices' bases, as the prepare carried them)
    s.inBase = made;
    s.inBaseDirty = false;
}
// (physical) this frame's rooms of a map whose texels continue rooms of the story below (render thread): their own wall maps
// and ambients, and the UV map built again when its texels or those maps' sizes change (a room's own map is the kept solve's
// working map's top-left corner: UploadSlot), into a new texture swapped in once filled. A frame whose rooms cannot all be read keeps
// the last binding of the same layout (RtLampProxy::PickIndoor): a frame of the carried texels between two of the room's own
// showed as a flicker. Not ready (inAmbOk false): never read for this layout
void InRoomsNow(IDirect3DDevice9* dev, Slot& s, uintptr_t tracker) {
    if (s.inRooms.empty() || s.mgr || !s.game) {
        s.inAmbOk = false;
        return;
    }
    g_inSum.frames++;
    // (the summary) the relit copy or the game's map shown, against last frame
    const int shown = g_swap.count(s.game) ? 1 : 0;
    if (s.inShown >= 0 && shown != s.inShown) g_inSum.toggles++;
    s.inShown = shown;
    uint32_t size[4] = {};
    float amb[4][3] = {};
    IDirect3DTexture9* got[4] = {};
    int fail = 0; // 1 a room's ambient, 2 a room's map, 3 the UV map
    bool otherClass = false;
    for (size_t r = 0; r < s.inRooms.size() && r < 4 && !fail; r++) {
        const void* room = LevelLightShare::RoomOf(tracker, s.story - 1, s.inRooms[r]);
        if (!ReadRoomAmbient(room, amb[r])) {
            fail = 1;
            break;
        }
        if (RoomNow rn; ReadRoomNow(static_cast<const BYTE*>(room), rn) && rn.shown >= 0 && rn.shown <= 2) {
            {
                std::lock_guard<std::mutex> lk(g_shownMx);
                if (g_shownCls.size() > 256) g_shownCls.clear();
                g_shownCls[std::make_tuple(g_lotId, s.story - 1, s.inRooms[r])] = rn.shown;
            }
            if (s.inCls[r] >= 0 && rn.shown != s.inCls[r]) otherClass = true; // (prepared again in its class: the frame loop)
        }
        if (s.inCls[r] < 0) continue; // (none of its texels matched)
        uintptr_t setAt = 0;
        D3DSURFACE_DESC d{};
        IDirect3DTexture9* const t = OwnMap(static_cast<const BYTE*>(room), s.inCls[r], kSetIdx[0], setAt);
        if (!t || FAILED(t->GetLevelDesc(0, &d))) {
            fail = 2;
            break;
        }
        got[r] = t, size[r] = d.Width << 16 | d.Height;
    }
    if (otherClass) g_inSum.otherClass++;
    InBaseNow(dev, s);
    if (!fail && (!s.inUv || std::memcmp(size, s.inUvSize, sizeof size) || s.inUvBuilt != s.inUvSrc)) {
        IDirect3DTexture9* uv = nullptr;
        D3DSURFACE_DESC ad{};
        if (FAILED(s.game->GetLevelDesc(0, &ad)) || !(uv = MakeFilledTexture(dev, ad.Width, ad.Height, D3DFMT_A16B16G16R16, nullptr, [&](BYTE* bits, size_t pitch) {
                RtLampProxy::FillUv(s.inUvSrc, size, ad.Width, ad.Height, pitch, static_cast<unsigned char*>(bits));
            })))
            fail = 3;
        else {
            if (s.inUv) s.inUv->Release();
            s.inUv = uv, s.inUvBuilt = s.inUvSrc;
            std::memcpy(s.inUvSize, size, sizeof size);
        }
    }
    // the kept binding is of this layout: the UV map of these texels for rooms' maps of these sizes (a size not read: kept as it was)
    const bool sameLayout = s.inUv && s.inUvBuilt == s.inUvSrc && (fail != 0 || !std::memcmp(size, s.inUvSize, sizeof size));
    switch (RtLampProxy::PickIndoor(!fail, s.inAmbOk && s.inUv, sameLayout)) {
    case RtLampProxy::InPick::Now:
        g_inSum.now++;
        for (int r = 0; r < 4; r++) {
            if (got[r]) got[r]->AddRef();
            if (s.inGame[r]) s.inGame[r]->Release();
            s.inGame[r] = got[r];
        }
        std::memcpy(s.inAmb, amb, sizeof amb);
        s.inAmbOk = true;
        break;
    case RtLampProxy::InPick::Kept: // (the rooms' maps and ambients of the last frame that read them all)
        (fail == 1 ? g_inSum.keptAmb : fail == 2 ? g_inSum.keptMap : g_inSum.keptUv)++;
        break;
    case RtLampProxy::InPick::None:
        g_inSum.none++;
        for (IDirect3DTexture9*& g : s.inGame)
            if (g) g->Release(), g = nullptr;
        s.inAmbOk = false;
        break;
    }
}
} // namespace
IDirect3DTexture9* ScaleOf(const void* texture) {
    if (!texture) return nullptr;
    if (g_on.load(std::memory_order_relaxed))
        for (const auto& s : g_slots)
            if (s->copy == texture || (s->hi && s->hi == texture)) return s->relit && s->scaled ? s->scale : nullptr;
    for (const Retired& r : g_retired) // (the lots left: their copies are still bound)
        if (r.s->copy == texture || (r.s->hi && r.s->hi == texture)) return r.s->relit && r.s->scaled ? r.s->scale : nullptr;
    return nullptr;
}
bool IndoorRoomsOf(const void* texture, IndoorRooms& out) {
    if (!texture || !g_on.load(std::memory_order_relaxed)) return false;
    for (const auto& s : g_slots)
        if ((s->copy == texture || (s->hi && s->hi == texture) || s->game == texture) && s->inAmbOk && s->inUv) {
            out.uv = s->inUv;
            // (the game's own map, its relit copy not shown: the marks-only scale IndoorFlagsOf binds is its last relight's of this layout,
            // ratios included; x 1 there made the attic switch between the fall-off and its room's top texel up the whole wall)
            out.ratio = s->game != texture || s->inFlags;
            for (int r = 0; r < 4; r++) {
                IDirect3DTexture9* g = s->inGame[r];
                if (const auto it = g ? g_swap.find(g) : g_swap.end(); it != g_swap.end()) // (as SwapCopy: what the room's own draw reads)
                    g = it->second->hiOk && it->second->hi ? it->second->hi : it->second->copy;
                out.map[r] = g ? g : s->inUv; // (no texel of that slot matched: never read)
                std::memcpy(out.amb[r], s->inAmb[static_cast<size_t>(r) < s->inRooms.size() ? r : 0], 3 * sizeof(float));
                // shortcut: InteriorWall's hemisphere tint on the sky reflection as the light capture of the room below gave it at night
                // (c5 0.170, c6 0.064 grey, their mean); read the interior draw's c5 / c6 to follow the time of day
                out.amb[r][3] = kIndoorReflection;
            }
            return true;
        }
    return false;
}
IDirect3DTexture9* IndoorFlagsOf(const void* texture) {
    if (!texture || !g_on.load(std::memory_order_relaxed)) return nullptr;
    for (const auto& s : g_slots)
        if (s->game == texture && !(s->copy && s->relit) && s->scale && s->inFlags) return s->scale;
    return nullptr;
}
const void* GameMapOf(const void* texture) {
    if (!texture) return texture;
    if (g_on.load(std::memory_order_relaxed))
        for (const auto& s : g_slots)
            if (s->copy == texture || (s->hi && s->hi == texture)) return s->game;
    for (const Retired& r : g_retired)
        if (r.s->copy == texture || (r.s->hi && r.s->hi == texture)) return r.s->game;
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
                       "rooms sent after edits {} (edits whose rooms the physical model drew instead {}) | room 0s sent to gather a lamp they lost {} | occluders read {} | copies bound {} times | its own cost {:.3f} ms a frame{}",
                       st, static_cast<uint32_t>(g_lotId), g_slots.size(), relit, ok, lamps, full ? "yes" : "no", c_relights.load(), c_mapsRelit.load(), c_tiles.load(),
                       g_lastMs.load(), g_lastGpuMs.load() >= 0.0f ? std::format("{:.2f}", g_lastGpuMs.load()) : std::string("-"), c_edits.load(), c_editsRefused.load(),
                       g_lastEditMs.load() >= 0.0f ? std::format("{:.1f}", g_lastEditMs.load()) : std::string("-"), c_sends.load(), c_physUnsent.load(), c_repairs.load(), c_proxyBuilds.load(), binds,
                       g_frameMsSum, g_budgetOut ? " | the lot's kept parts overflow the memory budget" : "");
}

} // namespace RtOutdoorLamps
