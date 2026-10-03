#include "shader_lookup_cache.h"
#include "world_lamp_policy.h"
// Lot light bridge (part of Night Lighting)
//
// Why lot grass has a hard edge next to street lamps (measured with light_probe.cpp):
//  - World grass: the terrain chunk shader adds tex2D(terrainLightMap, uv) * c7.x (s8), where the terrain light map is the
//    world's baked lamp "stamp" (wide diffuse circles). uv = (worldXZ - chunkCenter) / 256 + 0.5.
//  - Lot grass: drawn by the lot terrain passes. Its light pass (modulate2x) adds tex2D(lotLightMap, uv) * c3.x (s1). The
//    lot light map is solved on the CPU (FUN_006be020): street lamps fall off with the squared distance from the lamp head,
//    so they arrive very faint. The two formulas meet at the lot border.
//  - The lot light-pass vertex shader already outputs the terrain light map uv in TEXCOORD1 and gets the chunk center in
//    c15 (xz).
// Fix: when the game draws that exact lot light pass, draw it with a copy of the shader that also samples the terrain light
// map of the same chunk (recorded from the world chunk draws: s8 texture, key = world translation c8.w / c10.w) and uses
// max(lot light, terrain light). Both sides of the border then show the same street-lamp light.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "lot_light_bridge.h"
#include "game_addresses.h"
#include "shader_ids.h"
#include "roof_ps_hlsl.h"
#include "water_lamps_hlsl.h"
#include "roof_snow_lamps_hlsl.h"
#include "wall_lamp_table.h"
#include "floor_atlas_table.h"
#include "shader_patches.h"
#include "lightmap_smooth.h"
#include "terrain_chunk_relight.h"
#include "room_map_padding.h"
#include "level_light_share.h"
#include "unlit_rooms.h"
#include "light_probe.h"
#include "rig_tracker.h"
#include "recorder.h"
#include "depth_share.h"
#include "d3d9_extra_hooks.h"
#include "d3d9_hooks.h"
#include "shader_cache.h"
#include "frame_profiler.h"
#include "build_flavor.h"
#include "apex_paths.h"
#include "apex_log.h"
#include "apex_version.h"
#include <windows.h>
#include <d3dcommon.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace {

constexpr const char* kHookName = "LotLightBridge";

// The bridge's own state changes around a draw it replaces go straight to the device below Apex's detours
// (D3D9Hooks::CallOriginal*, 2026-09-29): before, each one re-entered Apex's own chains (15-25 dispatches per replaced
// object draw) where the only callbacks were the bridge's own Set*Shader tracking, which skips them (g_inOwnCall), and the
// Frame Profiler's state-call counts. The device state they set is the same; hooks of other mods that sit below Apex in
// the Detours chain still see them. The replaced draw itself is still re-issued through the device (its observers:
// Post-scene and Picture trigger counts, Light Probe, Frame Capture, Frame Profiler). SetSamplerState / SetRenderState are
// not hooked by the registry: plain device calls.
inline void SetPs(IDirect3DDevice9* d, IDirect3DPixelShader9* s) { D3D9Hooks::CallOriginalSetPixelShader(d, s); }
inline void SetVs(IDirect3DDevice9* d, IDirect3DVertexShader9* s) { D3D9Hooks::CallOriginalSetVertexShader(d, s); }
inline void SetTex(IDirect3DDevice9* d, DWORD stage, IDirect3DBaseTexture9* t) { D3D9Hooks::CallOriginalSetTexture(d, stage, t); }
inline void SetPsConst(IDirect3DDevice9* d, UINT reg, const float* c, UINT n) { D3D9Hooks::CallOriginalSetPixelShaderConstantF(d, reg, c, n); }
inline void SetVsConst(IDirect3DDevice9* d, UINT reg, const float* c, UINT n) { D3D9Hooks::CallOriginalSetVertexShaderConstantF(d, reg, c, n); }

// Binds `tex` to sampler s with linear filtering and clamp for one draw; restores everything afterwards.
// Only what differs is set, and only that is restored (2026-09-29): the state before and after the draw, and during it,
// is the same as with the six unconditional sets and restores (a state already at its value is left alone).
struct SamplerBind {
    IDirect3DDevice9* dev;
    DWORD s;
    IDirect3DBaseTexture9* old = nullptr;
    bool texSet = false;
    DWORD st[6] = {};
    uint8_t changed = 0; // bit i: kStates[i] was set
    static constexpr D3DSAMPLERSTATETYPE kStates[6] = {D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER, D3DSAMP_SRGBTEXTURE};
    SamplerBind(IDirect3DDevice9* d, DWORD sampler, IDirect3DBaseTexture9* tex, DWORD mip = D3DTEXF_LINEAR) : dev(d), s(sampler) {
        dev->GetTexture(s, &old);
        for (int i = 0; i < 6; i++) dev->GetSamplerState(s, kStates[i], &st[i]);
        const DWORD v[6] = {D3DTADDRESS_CLAMP, D3DTADDRESS_CLAMP, D3DTEXF_LINEAR, D3DTEXF_LINEAR, mip, FALSE};
        if (old != tex) {
            SetTex(dev, s, tex);
            texSet = true;
        }
        for (int i = 0; i < 6; i++)
            if (st[i] != v[i]) {
                dev->SetSamplerState(s, kStates[i], v[i]);
                changed |= static_cast<uint8_t>(1u << i);
            }
    }
    ~SamplerBind() {
        for (int i = 5; i >= 0; i--)
            if (changed & (1u << i)) dev->SetSamplerState(s, kStates[i], st[i]);
        if (texSet) SetTex(dev, s, old);
        if (old) old->Release();
    }
    SamplerBind(const SamplerBind&) = delete;
    SamplerBind& operator=(const SamplerBind&) = delete;
};

//
// Soft lot edges (28/09, research\borda2 + borda3): the lot map saturates at 1.0 within ~2.5 m of a street lamp while
// the terrain stamp outside the lot peaks at ~0.7-0.9, so a lamp near a lot edge left a step at the edge. Within the
// band c30.x = 1/band metres of the lot rectangle, the lamp term blends from max(lot, terrain) to the terrain term, which
// is exactly what the world grass shows on the other side. Lot-local position = affine map of the terrain uv (c28, c29,
// built on the CPU from VS c14/c15 and the lot matrix VS c8/c10); lot size W x D (tiles = metres) from room 0 of the
// lot (+0xC0/+0xC4). c30 = (0, 1) turns it off (w = 1 everywhere): unmatched lot or option off.
//   c28 = (dLx/du, dLx/dv, Lx0, W)   c29 = (dLz/du, dLz/dv, Lz0, D)   c30 = (1/band, bias, 0, 0)
// c31.x = "Lot lamps" brightness on the lot's own light map (night-weighted; 1 = the game).
const char* kReplacementHlsl = R"(
float4 c0 : register(c0);
float4 c1 : register(c1);
float4 c2 : register(c2);
float4 c3 : register(c3);
float4 c4 : register(c4);
float4 cLotX : register(c28);
float4 cLotZ : register(c29);
float4 cEdge : register(c30);
float4 cLotGain : register(c31);
samplerCUBE sSky : register(s0);
sampler2D sLot : register(s1);
sampler2D sTerrain : register(s2);
sampler2D sShadow : register(s5);
struct PSIn {
    float4 shadowPos : TEXCOORD2;
    float3 normal : TEXCOORD4;
    float2 lotUv : TEXCOORD5;
    float3 terrainUv : TEXCOORD1;
};
float4 main(PSIn i) : COLOR0 {
    float4 p = float4(i.shadowPos.xy - 0.5 * c2.y, i.shadowPos.zw);
    float4 s;
    s.y = tex2Dproj(sShadow, p + c2.yzww).x;
    s.z = tex2Dproj(sShadow, p + c2.zyww).x;
    s.x = tex2Dproj(sShadow, p).x;
    s.w = tex2Dproj(sShadow, p + c2.yyww).x;
    float avg = dot(s, 0.25);
    float2 d = i.shadowPos.xy - 0.5;
    float edge = saturate(max(abs(d.x), abs(d.y)) * 8 - 3);
    float sun = lerp(avg, 1, edge) * saturate(dot(i.normal, c1.xyz));
    float3 terrain = tex2D(sTerrain, i.terrainUv.xy).rgb;
    float3 uv1 = float3(i.terrainUv.xy, 1);
    float2 lp = float2(dot(uv1, cLotX.xyz), dot(uv1, cLotZ.xyz)); // lot-local metres
    float2 e = min(lp, float2(cLotX.w, cLotZ.w) - lp);             // distance to the nearer edge on each axis
    float w = saturate(min(e.x, e.y) * cEdge.x + cEdge.y);
    w = w * w * (3 - 2 * w);
    float3 lamps = lerp(terrain, max(tex2D(sLot, i.lotUv).rgb * cLotGain.x, terrain), w) * c3.x;
    float3 col = sun * c0.rgb + lamps;
    col = texCUBE(sSky, i.normal).rgb * c4.x + col;
    return float4(col * 0.5, 0);
}
)";

// Instanced outdoor objects (fences, shrubs): the vertex shader sums sun + the 3 rig lamps into TEXCOORD2 and this pixel
// shader multiplies that sum by the sun/moon shadow, so at night lamp light vanishes wherever the moon shadow falls
// (e.g. the side of a hedge or planter wall). Replacement: identical, but the shadow fades to 1 by c3.x (night level).
const char* kObjectRigHlsl = R"(
float4 c0 : register(c0);
float4 c1 : register(c1);
float4 c3 : register(c3);
samplerCUBE sSky : register(s0);
sampler2D sTex : register(s1);
sampler2D sShadow : register(s5);
struct PSIn {
    float4 fog : COLOR0;
    float3 t0 : TEXCOORD0;
    float4 t1 : TEXCOORD1;
    float3 t2 : TEXCOORD2;
    float4 t4 : TEXCOORD4;
    float t5 : TEXCOORD5;
};
float4 main(PSIn i) : COLOR0 {
    float4 p = float4(i.t4.xy - 0.5 * c0.y, i.t4.zw);
    float4 a = float4(p.x + c0.y, p.y + c0.z, p.z + c0.w, p.w + c0.w);
    float4 b = float4(p.x + c0.z, p.y + c0.y, p.z + c0.w, p.w + c0.w);
    float4 d = float4(p.x + c0.y, p.y + c0.y, p.z + c0.w, p.w + c0.w);
    float4 s = float4(tex2Dproj(sShadow, p).x, tex2Dproj(sShadow, a).x, tex2Dproj(sShadow, b).x, tex2Dproj(sShadow, d).x);
    float sh = lerp(dot(s, 0.25), 1, i.t5);
    sh = lerp(sh, 1, c3.x);
    float3 light = (i.t2 * sh + texCUBE(sSky, i.t0).rgb * c1.w) * i.t1.z;
    float4 tex = tex2D(sTex, i.t1.xy);
    float3 col = lerp(tex.rgb * light, i.fog.rgb, i.fog.w);
    return float4(col, i.t1.w - tex.a);
}
)";

// The five replacement shaders are compiled at start-up on a background thread (framework/shader_cache.h), with the
// options they always had (entry "main", flags 0); the draw hooks only create the shader objects at their first use.
// Before 2026-09-28 each was compiled with D3DCompile inside the first draw that needed it: a one-time hitch on the render
// thread (research\perf2\plan.md, item 7).
ShaderCache::Id AddLotShader(const char* tag, const char* hlsl, const char* target, int priority) {
    ShaderCache::Desc d;
    d.tag = tag;
    d.source = hlsl;
    d.sourceName = "lot_light_bridge";
    d.entry = "main";
    d.target = target;
    d.flags = 0;
    d.priority = priority;
    return ShaderCache::Add(std::move(d));
}
const ShaderCache::Id kReplacementPsId = AddLotShader("NightLighting lot light pass", kReplacementHlsl, "ps_3_0", 0);
const ShaderCache::Id kObjectRigPsId = AddLotShader("NightLighting object rig (moon shadow)", kObjectRigHlsl, "ps_2_0", 0);
const ShaderCache::Id kRoofPsId = AddLotShader("NightLighting roofs", kRoofHlsl, "ps_3_0", 0);
const ShaderCache::Id kWaterPsId = AddLotShader("NightLighting lake water", kWaterLampsHlsl, "ps_3_0", 0);
const ShaderCache::Id kRoofSnowPsId = AddLotShader("NightLighting snowy roofs", kRoofSnowLampsHlsl, "ps_3_0", 1);

enum class PsClass : uint8_t { Unknown, Other, WorldCandidate, WorldMultiLight, WorldCompact, LotLight, ObjectRig, Roof, Lake, LotLightSnow, RoofSnow, WallGain, FloorAtlas };

std::atomic<bool> g_enabled{false};
bool g_hooksRegistered = false;
bool g_inOwnCall = false;
IDirect3DPixelShader9* g_curPs = nullptr;
PsClass g_curClass = PsClass::Other;
std::unordered_map<IDirect3DPixelShader9*, PsClass> g_classCache;
std::unordered_set<IDirect3DPixelShader9*> g_basisPs; // pixel shaders that read the room basis maps (RoomMapPadding)
bool g_curPsBasis = false;
// The samplers each WorldCandidate pixel shader declares (bit s = s declared), from its bytecode in Classify.
// RecordWorldChunk looks for the chunk light map only there: a texture left bound in a sampler the shader never reads
// (e.g. the neighbour chunk's map in s8 while a 3-layer chunk reads s7) was taken as this chunk's map, depending on the
// draw order, i.e. on the camera; the road, fence and lot grass fixes then used a map without the lamps (user video
// 29/09: those fixes switching off and on together as the camera moved). g_chunkStraySkipped counts the declared-sampler
// textures skipped because they already are the map of another chunk.
std::unordered_map<IDirect3DPixelShader9*, uint16_t> g_worldSamplers;
std::atomic<int> g_chunkStraySkipped{0};
IDirect3DPixelShader9* g_replacementPs = nullptr;
IDirect3DPixelShader9* g_objectPs = nullptr;
bool g_objectCompileTried = false;
std::atomic<bool> g_objectFix{false};
std::atomic<float> g_night{0.0f};

// ---- Lamp brightness on the ground ("Ground brightness", "Roads and sidewalks"): the game's lamp scale cK.x of a light
// map term (lamp light only: the terrain map rgb, the lot map, max(map, atlas) of floors and roads) times the gain for
// one draw, restored afterwards. Weighted by the night level: by day the lot maps also hold the window light. ----
std::atomic<float> g_groundGain{1.0f}, g_roadGain{1.0f}; // road = a factor on top of the ground gain
std::atomic<float> g_lotMapGain{1.0f};                   // "Lot lamps": the lot light map in the lot pass (kReplacementHlsl c31.x)
std::atomic<int> g_groundGainDraws{0};
float NightWeighted(float gain) { return 1.0f + (gain - 1.0f) * g_night.load(std::memory_order_relaxed); }
float GroundGain() { return NightWeighted(g_groundGain.load(std::memory_order_relaxed)); }
float RoadGain() { return NightWeighted(g_groundGain.load(std::memory_order_relaxed) * g_roadGain.load(std::memory_order_relaxed)); }
float LotMapGain() { return NightWeighted(g_lotMapGain.load(std::memory_order_relaxed)); }
struct ConstGain {
    IDirect3DDevice9* dev;
    int k = -1;
    float old[4] = {};
    ConstGain(IDirect3DDevice9* d, int reg, float gain) : dev(d) {
        if (reg < 0 || gain == 1.0f || FAILED(dev->GetPixelShaderConstantF(static_cast<UINT>(reg), old, 1))) return;
        const float c[4] = {old[0] * gain, old[1], old[2], old[3]};
        SetPsConst(dev, static_cast<UINT>(reg), c, 1);
        k = reg;
        g_groundGainDraws.fetch_add(1, std::memory_order_relaxed);
    }
    ~ConstGain() {
        if (k >= 0) SetPsConst(dev, static_cast<UINT>(k), old, 1);
    }
    ConstGain(const ConstGain&) = delete;
    ConstGain& operator=(const ConstGain&) = delete;
};

std::atomic<int> g_objectDrawn{0};
bool g_compileTried = false;
std::string g_status = "Off";

struct ChunkTex {
    IDirect3DBaseTexture9* tex = nullptr; // AddRef'd
};
std::map<std::pair<int, int>, ChunkTex> g_chunks; // key: chunk center (x, z) rounded
std::unordered_map<IDirect3DBaseTexture9*, std::pair<int, int>> g_chunkOfTexture; // each registered map -> its chunk
std::atomic<int> g_worldSeen{0}, g_lotDrawn{0}, g_lotMissing{0};

std::pair<int, int> Key(float x, float z) { return {static_cast<int>(std::lround(x)), static_cast<int>(std::lround(z))}; }

// The smoothed version of a chunk light map when it is ready (lightmap_smooth.cpp), else the game's own.
IDirect3DBaseTexture9* ChunkTexture(const std::pair<int, int>& key, IDirect3DBaseTexture9* original) {
    if (IDirect3DTexture9* s = LightmapSmooth::Find(key)) return s;
    return original;
}

bool Near(float a, float b) { return std::fabs(a - b) < 1e-5f; }

// ---- Soft lot edges: the rectangle of every loaded lot, for the lot pass feather (kReplacementHlsl c28..c30). ----
// Walk (render thread, Present): *(0x011D1860)+0x1C0 = lightMgr; +0xD4 light update tree (buckets +0x58, count +0x5C,
// node +8 tracker, next +0x10); tracker+0x6A0 + level*0x1A4 = tree level, whose +0 is the story's manager; manager
// +0x90/+0x94 lot id, room hash +0x234 / +0x238 (node: +0 room id, +0x10 room, +0x80 next). Room 0:
//  - +0xF8 -> 4x4 lot->world matrix, row vectors: translation m[12], m[14]; the lot pass VS has the same matrix in
//    c8 = (m0, m4, m8, m12), c10 = (m2, m6, m10, m14) (LightDiag "matriz[+0xF8]" of lot 09080020A1D28860 = VS c8/c10
//    of the lot pass in research\borda3, rotation and translation);
//  - +0xC0 / +0xC4 = tile extent (x, z) of the room. For room 0 the rebuild FUN_006a2740 copies them from the manager's
//    tile grid size +0x264 / +0x268 (the room-id grid +0x260 bounds-checked with them everywhere, and "LotSizeParameters"
//    = size / 64 in FUN_006a4c10), FUN_0069efc0 walks tiles [0, C0) x [0, C4), and FUN_006c6ab0 gathers world lights
//    at the lot centre (C0 / 2, 0, C4 / 2) through +0xF8. So the lot covers lot-local [0, C0] x [0, C4] metres.
struct LotRect {
    float tx, tz;         // lot origin (world x, z)
    float m0, m8;         // first row of the rotation (VS c8.x, c8.z) for the match
    float w, d;           // size in metres along lot-local x and z
    uint32_t lotLo, lotHi;
};
std::vector<LotRect> g_lotRects;
std::atomic<bool> g_softEdges{true};
constexpr float kEdgeBand = 3.0f; // metres of feather inside the lot edge
std::atomic<int> g_edgeMatched{0}, g_edgeUnmatched{0};
bool g_lotRectMiss = false; // a lot pass found no rectangle: refresh the table at the next Present
int g_lotRectFrame = 0;
LotRect g_lastEdgeRect{};        // the last lot the feather was applied to (status line)
bool g_haveLastEdgeRect = false;
// Render-thread visibility, from the verified lot-pass matrix. Registration
// elsewhere in the world is not evidence that a lot is visible to the player.
DWORD g_lotDrawTick = 0;
std::unordered_map<uint64_t, DWORD> g_lotDrawSeen;
std::unordered_set<uint64_t> g_lotArrivals;
bool VisibleLot(uint64_t lot) {
    const auto it = g_lotDrawSeen.find(lot);
    return it != g_lotDrawSeen.end() && g_lotDrawTick - it->second < 500;
}
void NoteLotDraw(uint64_t lot) {
    const auto [it, first] = g_lotDrawSeen.try_emplace(lot, g_lotDrawTick);
    if (first || g_lotDrawTick - it->second >= 2000) {
        g_lotArrivals.insert(lot);
        LotLightBridge::RequestLampEditRefresh();
    }
    it->second = g_lotDrawTick;
}

// Story order: level 0 first (the terrain story, which draws the lot ground); every story has the same matrix and size.
constexpr int kLotLevels[] = {0, 1, 2, 3, 4, 5, 6, 7, -1, -2, -3, -4};

// SEH only (no C++ objects): fills out[0..max), returns the count or -1 on a fault / no world.
int ReadLotRects(LotRect* out, int max) {
    int n = 0;
    const uintptr_t rootPtr = GameAddr::Get(GameAddr::Id::RootPtr); // 0x011D1860 on Steam, found by signature elsewhere
    if (!rootPtr) return -1;
    __try {
        const uintptr_t root = *reinterpret_cast<const uintptr_t*>(rootPtr);
        const uintptr_t lightMgr = root ? *reinterpret_cast<const uintptr_t*>(root + 0x1C0) : 0;
        if (!lightMgr) return -1;
        const uintptr_t tree = *reinterpret_cast<const uintptr_t*>(lightMgr + 0xD4);
        if (!tree) return -1;
        const uintptr_t buckets = *reinterpret_cast<const uintptr_t*>(tree + 0x58);
        const uint32_t bucketCount = *reinterpret_cast<const uint32_t*>(tree + 0x5C);
        if (!buckets || !bucketCount || bucketCount >= (1u << 20)) return -1;
        const uintptr_t endNode = *reinterpret_cast<const uintptr_t*>(buckets + bucketCount * 4);
        uintptr_t slot = buckets;
        uintptr_t node = *reinterpret_cast<const uintptr_t*>(slot);
        int guard = 0;
        while (node == 0 && guard++ < (1 << 20)) node = *reinterpret_cast<const uintptr_t*>(slot += 4);
        guard = 0;
        while (node && node != endNode && guard++ < 100000 && n < max) {
            const uintptr_t tracker = *reinterpret_cast<const uintptr_t*>(node + 8);
            for (int li = 0; tracker && li < 12; li++) {
                const uintptr_t mgr = *reinterpret_cast<const uintptr_t*>(tracker + 0x6A0 + static_cast<intptr_t>(kLotLevels[li]) * 0x1A4);
                if (!mgr || *reinterpret_cast<const uintptr_t*>(mgr) != lightMgr) continue;
                const uintptr_t rb = *reinterpret_cast<const uintptr_t*>(mgr + 0x234);
                const uint32_t rc = *reinterpret_cast<const uint32_t*>(mgr + 0x238);
                uintptr_t room0 = 0;
                for (uint32_t b = 0; rb && rc < 100000 && b < rc && !room0; b++) {
                    int g2 = 0;
                    for (uintptr_t rn = *reinterpret_cast<const uintptr_t*>(rb + b * 4); rn && g2++ < 10000; rn = *reinterpret_cast<const uintptr_t*>(rn + 0x80))
                        if (*reinterpret_cast<const int*>(rn) == 0) {
                            room0 = *reinterpret_cast<const uintptr_t*>(rn + 0x10);
                            break;
                        }
                }
                if (!room0) continue;
                const uint32_t w = *reinterpret_cast<const uint32_t*>(room0 + 0xC0), d = *reinterpret_cast<const uint32_t*>(room0 + 0xC4);
                const float* m = *reinterpret_cast<const float* const*>(room0 + 0xF8);
                if (!m || w == 0 || d == 0 || w > 256 || d > 256) continue; // room not rebuilt yet, or not a lot grid
                LotRect& r = out[n];
                r.tx = m[12];
                r.tz = m[14];
                r.m0 = m[0];
                r.m8 = m[8];
                r.w = static_cast<float>(w);
                r.d = static_cast<float>(d);
                r.lotLo = *reinterpret_cast<const uint32_t*>(mgr + 0x90);
                r.lotHi = *reinterpret_cast<const uint32_t*>(mgr + 0x94);
                if (std::isfinite(r.tx) && std::isfinite(r.tz) && std::isfinite(r.m0) && std::isfinite(r.m8)) n++;
                break; // one story per lot is enough
            }
            node = *reinterpret_cast<const uintptr_t*>(node + 0x10);
            int g3 = 0;
            while (node == 0 && g3++ < (1 << 20)) node = *reinterpret_cast<const uintptr_t*>(slot += 4);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
    return n;
}

void RefreshLotRects() {
    static LotRect buf[1024];
    const int n = ReadLotRects(buf, 1024);
    if (n < 0) {
        g_lotRects.clear();
        return;
    }
    g_lotRects.assign(buf, buf + n);
}

// The rectangle of the lot the current lot pass draws, matched by its matrix (VS c8, c10). nullptr = unknown lot.
const LotRect* FindLotRect(const float c8[4], const float c10[4]) {
    for (const LotRect& r : g_lotRects)
        if (std::fabs(r.tx - c8[3]) < 0.05f && std::fabs(r.tz - c10[3]) < 0.05f && std::fabs(r.m0 - c8[0]) < 2e-3f && std::fabs(r.m8 - c8[2]) < 2e-3f) return &r;
    return nullptr;
}

// PS c28..c30 of kReplacementHlsl. k = the VS c14 the draw runs with (terrain uv = (world.xz - c15.xz) * k.xy + k.zw),
// c15 = chunk centre, c8 / c10 = lot matrix rows (world.x = c8.x lx + c8.z lz + c8.w, world.z = c10.x lx + c10.z lz +
// c10.w). Inverts both into lot-local = A * uv + b (double precision on the CPU; the shader only does two dot products).
bool LotEdgeConstants(const float k[4], const float c15[4], const float c8[4], const float c10[4], const LotRect* r, float out[12]) {
    const float off[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 1, 0, 0}; // w = 1 everywhere: the plain max()
    std::memcpy(out, off, sizeof(off));
    if (!r || !g_softEdges.load(std::memory_order_relaxed)) return false;
    const double a = c8[0], b = c8[2], c = c10[0], d = c10[2];
    const double det = a * d - b * c;
    if (std::fabs(det) < 1e-4 || std::fabs(k[0]) < 1e-9f || std::fabs(k[1]) < 1e-9f) return false;
    const double sx = 1.0 / k[0], sz = 1.0 / k[1];
    const double ox = c15[0] - k[2] * sx - c8[3]; // world.x - tx = uv.x * sx + ox
    const double oz = c15[2] - k[3] * sz - c10[3];
    out[0] = static_cast<float>(d * sx / det);
    out[1] = static_cast<float>(-b * sz / det);
    out[2] = static_cast<float>((d * ox - b * oz) / det);
    out[3] = r->w;
    out[4] = static_cast<float>(-c * sx / det);
    out[5] = static_cast<float>(a * sz / det);
    out[6] = static_cast<float>((-c * ox + a * oz) / det);
    out[7] = r->d;
    out[8] = 1.0f / kEdgeBand;
    out[9] = 0.0f;
    return true;
}

// ---- Outdoor walls. Their lamp light is only the game's baked wall atlas (room solve, lamps x k2 = 0.075), much
// dimmer than the rig lamps objects get, so walls look darker than the objects in front of them (user, 25/09). The
// pixel shaders of the game's ExteriorWall technique (Shaders_Win32.precomp) add it with "texld rA, vT, s2" then
// "mad rB.xyz, rA, cK.x, rC"; K differs per variant (in others c3.x is the bloom threshold). wall_lamp_table.h lists
// every ExteriorWall pixel shader by size + FNV-1a (32-bit, over DWORDs) with its K, generated offline; a draw with
// one of them gets cK.x multiplied by "Forca nas paredes". No shader is changed; interior walls are never in the table.
std::unordered_map<IDirect3DPixelShader9*, DWORD> g_wallConst;
std::atomic<float> g_wallGain{1.0f};

int WallLampConst(const DWORD* t, size_t bytes) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < bytes / 4; i++) h = (h ^ t[i]) * 16777619u;
    for (const WallLampEntry& e : kWallLampTable)
        if (e.size == bytes && e.hash == h) return static_cast<int>(e.constant);
    return -1;
}

// Outdoor floors lit only by their baked floor map (floor_atlas_table.h; nearly black in summer, LightProbe-m61).
bool IsFloorAtlasPs(const DWORD* t, size_t bytes) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < bytes / 4; i++) h = (h ^ t[i]) * 16777619u;
    for (const FloorAtlasEntry& e : kFloorAtlasTable)
        if (e.size == bytes && e.hash == h) return true;
    return false;
}

// Every shader we classify is pinned (AddRef) until Shutdown: all caches here are keyed by the shader pointer, and a
// released shader's address could otherwise be reused by a new one that would then get the old one's class or patched
// copy (review 25/09).
std::vector<IUnknown*> g_pinned;

PsClass Classify(IDirect3DPixelShader9* ps) {
    if (!ps) return PsClass::Other;
    auto it = g_classCache.find(ps);
    if (it != g_classCache.end()) return it->second;
    PsClass c = PsClass::Other;
    UINT size = 0;
    if (SUCCEEDED(ps->GetFunction(nullptr, &size)) && size >= 8 && size < 65536) {
        std::vector<BYTE> code(size);
        if (SUCCEEDED(ps->GetFunction(code.data(), &size))) {
            if (RoomMapPadding::IsBasisPs(reinterpret_cast<const DWORD*>(code.data()), size / 4)) g_basisPs.insert(ps); // reads the room basis maps
            if (IsShader(kLotLightPs, code.data(), size)) c = PsClass::LotLight;
            else if (IsShader(kWorldMultiLightPs, code.data(), size)) {
                c = PsClass::WorldMultiLight;
                g_worldSamplers[ps] = 1u << 2; // only s2 is the lamp map; s1 is the normal map
            }
            else if (IsShader(kWorldCompactPs, code.data(), size)) {
                c = PsClass::WorldCompact;
                g_worldSamplers[ps] = 1u << 3; // exact captured compact variant: only s3 is the lamp map
            }
            else if (IsShader(kObjectRigPs, code.data(), size)) c = PsClass::ObjectRig;
            else if (IsShader(kRoofPs, code.data(), size)) c = PsClass::Roof;
            else if (IsShader(kLakePs, code.data(), size)) c = PsClass::Lake;
            else if (IsShader(kLakePs2, code.data(), size)) {
                c = PsClass::Lake;
                static bool logged = false;
                if (!logged) { logged = true; LOG_INFO("[LotLightBridge] Water: second lake shader seen (sun shadow without a depth compare)"); }
            }
            else if (IsShader(kSnowLotPs, code.data(), size)) c = PsClass::LotLightSnow;
            else if (IsShader(kRoofSnowPs, code.data(), size)) c = PsClass::RoofSnow;
            else if (const int k = WallLampConst(reinterpret_cast<const DWORD*>(code.data()), size); k >= 0) {
                g_wallConst[ps] = static_cast<DWORD>(k);
                c = PsClass::WallGain;
            }
            else if (IsFloorAtlasPs(reinterpret_cast<const DWORD*>(code.data()), size)) c = PsClass::FloorAtlas;
            else {
                // Declares a sampler s6 or higher (terrain shaders; dcl token 0x0200001F followed by 0x90000000 | type, then register token s8)
                const auto* t = reinterpret_cast<const DWORD*>(code.data());
                const size_t n = size / 4;
                for (size_t k = 0; k + 2 < n; k++)
                    // register token: number in bits 0-10, type = bits 28-30 | bits 11-12 << 3 (sampler = 10)
                    if ((t[k] & 0xFFFF) == 0x001F && (t[k + 2] & 0x7FF) >= 6 && (((t[k + 2] >> 28) & 7) | (((t[k + 2] >> 11) & 3) << 3)) == 10) {
                        c = PsClass::WorldCandidate;
                        break;
                    }
                if (c == PsClass::WorldCandidate) { // the samplers it declares: where RecordWorldChunk looks for the light map
                    uint16_t mask = 0;
                    for (size_t k = 0; k + 2 < n; k++)
                        if ((t[k] & 0xFFFF) == 0x001F && (((t[k + 2] >> 28) & 7) | (((t[k + 2] >> 11) & 3) << 3)) == 10 && (t[k + 2] & 0x7FF) < 16)
                            mask |= static_cast<uint16_t>(1u << (t[k + 2] & 0x7FF));
                    g_worldSamplers[ps] = mask;
                }
            }
        }
    }
    g_classCache[ps] = c;
    ps->AddRef();
    g_pinned.push_back(ps);
    return c;
}

// Creates the pixel shader from its precompiled bytecode (shader_cache.h; compiled at start-up off the render thread).
// Returns an error text, empty on success.
std::string CompilePs(IDirect3DDevice9* dev, ShaderCache::Id id, IDirect3DPixelShader9** out) {
    std::string msg;
    switch (ShaderCache::CreatePixelShader(dev, id, out, &msg)) {
    case ShaderCache::Result::Ok:
        return {};
    case ShaderCache::Result::CompileFailed:
        return std::format("compile failed: {}", msg.empty() ? std::string("?") : msg);
    case ShaderCache::Result::CreateFailed:
        break;
    }
    *out = nullptr;
    return "could not create the shader";
}

// ---- Roofs: the game's roof shader has no lamp light at all (sun/moon + sky only). ----
std::atomic<bool> g_roofFix{false};
std::atomic<float> g_roofStrength{1.0f};
IDirect3DPixelShader9* g_roofPs = nullptr;
bool g_roofCompileTried = false;
IDirect3DVertexShader9* g_curVs = nullptr;
bool g_stateUnknown = true;               // the bound shaders were not seen by our Set*Shader hooks (see OnDraw)
std::atomic<bool> g_hookFailed{false};    // an exception escaped a hook: everything off (HookFailed)
bool g_curVsIsRoof = false;
bool g_curVsIsLake = false;
bool g_curVsIsSnowLot = false;
bool g_curVsIsRoad = false;
DWORD g_curRoadMap = 16;         // VS constant with the road's terrain uv mapping
bool g_curVsIsFloor = false;
bool g_curVsIsSnowFloor = false; // snow lying on lot floor tiles (LightProbe-m69, m71)
int g_curSnowFloorTc = 7;
bool g_curVsIsSnowCover = false;
bool g_curVsIsSnowRelief = false;
bool g_curVsIsFoliage = false;
bool g_curVsIsInstanced = false; // fence rails/posts, railings, stairs (SceneModelArray)
struct FoliageVs {
    std::vector<DWORD> code; // patched (ShaderPatches::PatchFoliageVs)
    IDirect3DVertexShader9* vs = nullptr;
    bool tried = false;
    int worldK = -1; // objects: first VS constant of the world triple (c[K..K+2].w = the object position)
    int vertexLight = -1; // objects: first colour constant of the rig's 4 vertex lights (COLOR0), -1 if not found
};
// Outdoor floors lit only by their baked floor map (summer; floor_atlas_table.h): the patched copy of the vertex shader
// (see DrawFloorAtlas)
struct FloorVs {
    IDirect3DVertexShader9* vs = nullptr;
    bool tried = false;
    int tc = 7;
};
bool g_curVsIsObject = false;
// Everything known about a game vertex shader, in one entry (2026-09-29; before: a class cache and five more maps keyed by
// the same pointer, each looked up per draw). Entries are only added (ClassifyVs) and all dropped together (Shutdown),
// so a pointer to the current one stays valid while that shader is tracked (unordered_map keeps element addresses).
struct VsInfo {
    bool worldMultiLight = false; // exact captured summer multi-pass light VS
    bool worldCompact = false; // exact captured single-layer WORLD VS (not an object rig)
    uint8_t cls = 0;      // 0 other, 1 roof, 2 lake, 3 snow lot, 4 road, 5 floor, 6 foliage, 7 fence/stairs, 8 snow on objects,
                          // 9 snow with relief (stair tops), 10 object lit by a rig, 11 snow on floor tiles
    DWORD roadMap = 0;    // cls 4: VS constant with the terrain uv mapping (c16 in winter, c14 in summer)
    int snowFloorTc = 7;  // cls 11: where it puts world xz / 2
    FoliageVs patched;    // cls 6: foliage copy (wrap light); cls 10: object copy (+ world xzy in TEXCOORD8, PatchObjectLampVs)
    FloorVs floor;        // copy for the outdoor floors of summer (any class; made at the first such draw)
    int lmSem = -1;       // DrawIndoorObject: the semantic (usage << 4 | index) lmRow was found for, -1 = not looked yet
    int lmRow[2] = {-1, -1}; // the VS constants whose dp4 with the position make the room light map uv .x / .y (-1: not found)
};
std::unordered_map<IDirect3DVertexShader9*, VsInfo> g_vsInfo;
VsInfo* g_curVsInfo = nullptr; // entry of g_curVs (null for no shader)
std::atomic<int> g_roofDrawn{0};
float g_cam[3] = {};
float g_lampData[33][4] = {}; // 16 x pos+radius, 16 x colour, params
int g_lampCount = 0;
int g_lampFrame = 0;

VsInfo* ClassifyVs(IDirect3DVertexShader9* vs) {
    if (!vs) return nullptr;
    auto it = g_vsInfo.find(vs);
    if (it != g_vsInfo.end()) return &it->second;
    VsInfo info;
    uint8_t cls = 0;
    UINT size = 0;
    if (SUCCEEDED(vs->GetFunction(nullptr, &size)) && size >= 8 && size <= 65536) {
        std::vector<BYTE> code(size);
        if (SUCCEEDED(vs->GetFunction(code.data(), &size))) {
            auto is = [&](const ShaderId& id) { return IsShader(id, code.data(), size); };
            info.worldMultiLight = is(kWorldMultiLightVs);
            info.worldCompact = is(kWorldCompactVs);
            if (is(kRoofVs)) cls = 1;
            else if (is(kLakeVs)) cls = 2;
            else if (is(kSnowLotVs)) cls = 3;
            else if (is(kFloorVs)) cls = 5;
            else {
                std::vector<DWORD> t(size / 4);
                std::memcpy(t.data(), code.data(), t.size() * 4);
                DWORD mapConst = 0;
                if (ShaderPatches::IsRoadVs(t, mapConst)) {
                    info.roadMap = mapConst; // c16 in winter, c14 in summer
                    cls = 4;
                } else if (ShaderPatches::IsInstancedStructureVs(t))
                    cls = 7;
                else if (ShaderPatches::IsSnowCoverVs(t))
                    cls = 8;
                else if (ShaderPatches::IsSnowReliefVs(t))
                    cls = 9;
                else if (ShaderPatches::IsFloorVs(t)) // floor variants, e.g. the curved pool edge (LightProbe-m66)
                    cls = 5;
                else if (ShaderPatches::PatchFoliageVs(t)) {
                    info.patched.code = std::move(t);
                    cls = 6;
                } else {
                    std::vector<DWORD> o = t;
                    int tc = 7, wk = -1, vl = -1;
                    if (ShaderPatches::PatchObjectLampVs(o, true, nullptr, &wk, &vl)) {
                        info.patched.code = std::move(o);
                        info.patched.worldK = wk;
                        info.patched.vertexLight = vl;
                        cls = 10;
                    } else if (ShaderPatches::IsSnowFloorVs(t, tc)) {
                        // last: the TEXCOORD0.zw shape is shared by terrain and lot passes, which the draw dispatch
                        // handles first (DrawSnowFloor only runs when no pixel-shader class claimed the draw)
                        info.snowFloorTc = tc;
                        cls = 11;
                    }
                }
            }
        }
    }
    info.cls = cls;
    VsInfo& stored = g_vsInfo.emplace(vs, std::move(info)).first->second;
    vs->AddRef();
    g_pinned.push_back(vs);
    return &stored;
}

// Light enumeration (FUN_006acf70, stdcall(visitor), visitor vtable[0] = thiscall(visitor, Light*))
std::vector<uintptr_t> g_enumLights;
void __fastcall EnumVisit(void*, void*, uintptr_t light) {
    if (g_enumLights.size() < 100000) g_enumLights.push_back(light);
}
void* g_enumVtbl[1] = {reinterpret_cast<void*>(&EnumVisit)};
struct EnumVisitor {
    void** vtbl;
} g_enumVisitor{g_enumVtbl};
bool g_enumChecked = false, g_enumOk = false;

uintptr_t g_enumFn = 0; // 0x006ACF70 on Steam, found by signature on other builds (game_addresses.h)

bool EnumerateLights() {
    if (!g_enumChecked && GameAddr::Resolved()) {
        g_enumChecked = true;
        g_enumFn = GameAddr::Get(GameAddr::Id::EnumLights);
        static const BYTE expect[] = {0xE8, 0x2B, 0x36, 0x00, 0x00, 0x8B, 0x4C, 0x24, 0x04, 0x51, 0x68, 0x40, 0xCF, 0x6A, 0x00}; // Steam (checked on Steam)
        g_enumOk = g_enumFn && (!GameAddr::IsFixed() || std::memcmp(reinterpret_cast<const void*>(g_enumFn), expect, sizeof(expect)) == 0);
    }
    if (!g_enumOk) return false;
    g_enumLights.clear();
    __try {
        reinterpret_cast<void(__stdcall*)(void*)>(g_enumFn)(&g_enumVisitor);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ReadLamp(uintptr_t L, float out[8]) {
    __try {
        const BYTE f = *reinterpret_cast<const BYTE*>(L + 0x100);
        if (!(f & 0x01) || !(f & 0x20)) return false; // alive and lit
        // A disabled lot lamp must leave the direct roof/water/object pool too,
        // rather than remaining there until the game's fade reaches zero.
        const bool lotOwned = (*reinterpret_cast<const uint32_t*>(L + 0xC0) | *reinterpret_cast<const uint32_t*>(L + 0xC4)) != 0;
        if (lotOwned && !(f & 0x40)) return false;
        const int type = *reinterpret_cast<const int*>(L + 0xB0);
        const int room = *reinterpret_cast<const int*>(L + 0x08);
        if (type != 0xB && !((f & 0x04) && room == 0)) return false; // world lamp or outdoor lot lamp
        const float* head = reinterpret_cast<const float*>(L + 0x120);
        const float* bounds = reinterpret_cast<const float*>(L + 0x134);
        const float* col = reinterpret_cast<const float*>(L + 0xF0);
        const float inten = *reinterpret_cast<const float*>(L + 0x10);
        const float fade = *reinterpret_cast<const float*>(L + 0x20);
        // Visual reach of the lamp's light pool: from its range value +0x130 (97, 40, 100...), about 7-12 m. The light
        // bounds (+0x134) are much larger (~50 m) and made roofs and water blow out to white.
        (void)bounds;
        const float range = *reinterpret_cast<const float*>(L + 0x130);
        if (!(range > 0.01f)) return false;
        float radius = std::sqrt(range) * 1.2f;
        radius = radius < 2.0f ? 2.0f : (radius > 25.0f ? 25.0f : radius);
        out[0] = head[0]; out[1] = head[1]; out[2] = head[2]; out[3] = radius;
        out[4] = col[0] * inten * fade; out[5] = col[1] * inten * fade; out[6] = col[2] * inten * fade; out[7] = 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Every lit outdoor lamp (pos, radius, colour), in enumeration order: what SelectLamps picks from. Rebuilt with the lot
// lamp tracking in one pass over the enumeration (ReadEnumeratedLamps, every 20 frames); a rebuild that changes it starts
// a new generation of the SelectLamps memo.
std::vector<std::array<float, 8>> g_allLamps;
uint32_t g_lampMemoGen = 1; // generation of the SelectLamps memo (see LampMemo); 0 marks an empty entry

// ---- Changes of outdoor lot lamps (Build mode, lamps switching by themselves) and the snapshot of what the terrain
// bake can take. The terrain light has to be rebuilt for changes of lamps IN the bake; the game does it only when the
// lot is reloaded. What the bake draws per lamp (docs/engine/terrain-and-light-bake.md 4.2): every light accepted by the
// visitor (street-lamp class, type 0xB; with Apex also outdoor lot lamps of type 3..6 that are enabled and lit,
// TerrainLightTest) whose rect +0x134 overlaps the chunk, at its position (vfunc+0x24, +0x120), with colour +0xF0 and
// weight range +0x130 x intensity +0x10 x 0.2. The fade +0x20 and the effective colour +0xE0 are NOT read by the bake.
// So a change counts only when:
//  - it changes the bake: the lamp enters or leaves it (lit flag, enabled flag, intensity to or from 0), moves by more
//    than 5 cm, or its light (colour x intensity x range) changes by more than 5 % in a channel. Lamps the bake never
//    takes (window lights 7/8, type 9, disabled or unlit lamps) never count. 29/09 (ApexRadiance_LOG + LightDiag): the
//    repeated "7 edited" of lot 7D6F0019FAF78910 were its 7 DISABLED type-3 lamps (flags 0x35 / 0xB5), never baked;
//  - automatic changes (lit state, dimming, recolouring) of a lamp that already changed 3 times within 60 s are ignored:
//    the lamp is "animated" (motion or timer lights, colour-cycling lights) and its current state goes into the next
//    rebuild made for any other reason; after 2 min with no automatic change it is a plain lamp again (30/09);
//    Confirmed ordinary enable switches and type-11 edits bypass that filter (private 02/10 follow-up).
//  - unless this is a value-only edit of observed lamps, its lot is settled: seen in every enumeration for at least 10 s,
//    and no uncounted change on it for 5 s (a lot that
//    is still loading keeps adding lamps and so never becomes settled while it trickles in);
//  - at most 8 changes in the enumeration (more = lamps switching at dusk / dawn, or streaming in bulk), unless they are
//    all switches of one lot (30/09: a town square's 57 lamps); confirmed value-only edits also bypass bulk suppression;
//  - removals: the lot is still there in the NEXT enumeration and lost no more lamps (a lot unloading lamp by lamp, or
//    vanishing, is streaming out). The removal of a lot's last lamp is therefore never counted (the lot vanishes).
// Lots streaming in and out must never look like edits (NOTAS 1c: a rebuild every ~30 s from streaming lamps).
// Additions, removals and moves are "user-driven" (Build mode: in the 28-29/09 logs no lamp was ever added or removed
// by itself); the rest is "automatic". The dev build logs what changed on each lamp.
using Clock = std::chrono::steady_clock;
struct LotLampState {
    uint64_t lot = 0;
    int type = 0;
    BYTE flags = 0;
    float col[3] = {}, inten = 0.0f, range = 0.0f, pos[3] = {};
    float rect[4] = {}; // light rect +0x134 {minX, minZ, maxX, maxZ} (FUN_006BDDF0: position +- sqrt(range / k))
    bool plain = false; // type 3..6
    bool baked = false; // InBake
    // automatic changes of this lamp in the current 60 s window (carried from enumeration to enumeration)
    int autoChanges = 0;
    Clock::time_point autoWindow{};
    Clock::time_point lastAuto{}; // the last automatic change (an animated lamp quiet for kAnimatedExpire counts again)
    bool animated = false;
};
struct LotSeen {
    Clock::time_point firstSeen{}, lastUncounted{};
    bool removalPending = false;
    bool explicitEdit = false; // value edit of an already observed lamp, not a streaming addition
};
// Tracked lot lamps, sorted by the light's address (unique): the previous enumeration and the one being read. Two
// vectors reused (swapped) at every refresh instead of a std::map rebuilt node by node (2026-09-29); iterated in the same
// ascending order as the map was, so counts, logs and the bake snapshot are the same.
using LampSig = std::pair<uintptr_t, LotLampState>;
std::vector<LampSig> g_lotLampSig, g_lotLampCur;
std::vector<uint64_t> g_lotsNow; // lots of g_lotLampCur, sorted, unique
std::map<uint64_t, LotSeen> g_lotSeen;
std::atomic<int> g_lotLampEdits{0}, g_lotLampUserEdits{0};
int g_lotChangesCounted = 0, g_lotChangesIgnored = 0;
int g_lampChangesOutside = 0, g_lampChangesNoise = 0, g_lampChangesAnimated = 0, g_lampsAnimated = 0;
std::string g_lastLotChange = "none";
std::map<uint64_t, Clock::time_point> g_quietLogAt; // dev log throttle of the changes that do not count, per lot
LotLightBridge::BakeSnapshot g_bakeSnap;
int g_lampEnumerations = 0;
bool g_lampRefreshNow = false;
bool g_lampEditRefresh = false;
DWORD g_lampReadTick = 0;
std::vector<uint64_t> g_lastUserLots; // lots of the last counted user-driven changes

constexpr float kMoveTol = 0.05f;   // metres
constexpr float kLightRel = 0.05f;  // relative change of colour x intensity x range, per channel
constexpr float kLightAbs = 0.05f;  // absolute floor (colour x intensity x range: typical lamps give 5..200)
constexpr int kAnimatedChanges = 3; // automatic changes within kAnimatedWindow: the lamp is animated
constexpr auto kAnimatedWindow = std::chrono::seconds(60);
constexpr auto kAnimatedExpire = std::chrono::seconds(120); // an animated lamp with no automatic change for this long is a plain lamp again
constexpr auto kQuietLogEvery = std::chrono::seconds(60);

bool IsPlainType(int type) { return type >= 3 && type <= 6; }

// The captured 02:03 session toggles 0x40 on ordinary lot lamps (types 3..6).
// Do not confuse that discrete enable switch with an animated colour/intensity.
// Both observations must identify the same lamp on the same lot; first sights,
// pointer reuse on another lot and streaming additions are never fast edits.
bool ObservedEnableSwitch(const LotLampState& before, const LotLampState& after) {
    return before.lot == after.lot && before.type == after.type
        && IsPlainType(after.type) && (((before.flags ^ after.flags) & 0x40) != 0
            || ((before.inten <= 1e-3f) != (after.inten <= 1e-3f)));
}

// Outdoor bake lamps: lot types 3..6/11, plus world-owned type-11 street lamps.
bool ReadLotLamp(uintptr_t L, LotLampState& out) {
    __try {
        const uint32_t lo = *reinterpret_cast<const uint32_t*>(L + 0xC0), hi = *reinterpret_cast<const uint32_t*>(L + 0xC4);
        const BYTE f = *reinterpret_cast<const BYTE*>(L + 0x100);
        const int type = *reinterpret_cast<const int*>(L + 0xB0);
        const uint64_t lot = (static_cast<uint64_t>(hi) << 32) | lo;
        if (!WorldLampPolicy::Eligible(lot, type, f, *reinterpret_cast<const int*>(L + 0x08))) return false;
        std::memcpy(out.col, reinterpret_cast<const void*>(L + 0xF0), 12);  // base colour
        out.inten = *reinterpret_cast<const float*>(L + 0x10);               // intensity (x)
        out.range = *reinterpret_cast<const float*>(L + 0x130);              // range
        std::memcpy(out.pos, reinterpret_cast<const void*>(L + 0x120), 12); // position
        std::memcpy(out.rect, reinterpret_cast<const void*>(L + 0x134), 16); // light rect (the bake's chunk test, 0xC29480)
        out.lot = (static_cast<uint64_t>(hi) << 32) | lo;
        out.type = type;
        out.flags = f;
        out.plain = IsPlainType(type);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

float LampLight(const LotLampState& s, int c) { return s.col[c] * s.inten * s.range; }

// The terrain bake takes this lamp now (see the block comment). Lot lamps 3..6: TerrainLightTest (enabled 0x40, lit
// 0x20). The private follow-up also filters lot-owned type-11 lamps by those flags before the
// original always-true vfunc+0x20 accepts them (captured disabled lamp, 2026-10-02). A lamp whose
// light is zero (intensity or range 0, black colour) draws nothing.
bool InBake(const LotLampState& s) {
    if (!(s.flags & 0x20)) return false;
    if (!(s.flags & 0x40)) return false;
    const float w = s.inten * s.range;
    if (!std::isfinite(w) || !(w > 1e-3f)) return false;
    return std::max({s.col[0], s.col[1], s.col[2]}) > 1e-3f;
}

bool MovedApart(const float* a, const float* b) {
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return !(dx * dx + dy * dy + dz * dz <= kMoveTol * kMoveTol); // NaN counts as moved
}

bool LightDiffers(const float* a, const float* b) {
    for (int c = 0; c < 3; c++) {
        const float tol = std::max(kLightAbs, kLightRel * std::max(std::fabs(a[c]), std::fabs(b[c])));
        if (!(std::fabs(a[c] - b[c]) <= tol)) return true;
    }
    return false;
}

// The user can adjust a visible lamp by less than the automatic 5% threshold.
// Compare its effective values exactly; edit debounce and the bounded chunk
// queue coalesce the resulting notices. Animated fade is not part of this value.
bool LightChanged(const float* a, const float* b) {
    for (int c = 0; c < 3; ++c) if (a[c] != b[c]) return true;
    return false;
}

bool RawChanged(const LotLampState& a, const LotLampState& b) {
    return a.flags != b.flags || a.type != b.type || std::memcmp(a.col, b.col, sizeof a.col) != 0 || std::memcmp(&a.inten, &b.inten, 4) != 0 ||
           std::memcmp(&a.range, &b.range, 4) != 0 || std::memcmp(a.pos, b.pos, sizeof a.pos) != 0;
}

// Developer log: what changed on one lamp ("L1234ABCD type 3: lit 1->0, intensity 1.00->0.00 [leaves the bake]")
std::string LampChangeText(uintptr_t L, const LotLampState& a, const LotLampState& b) {
    std::string t = std::format("L{:08X} type {}:", L, b.type);
    const BYTE df = a.flags ^ b.flags;
    if (df & 0x20) t += std::format(" lit {}->{},", (a.flags & 0x20) ? 1 : 0, (b.flags & 0x20) ? 1 : 0);
    if (df & 0x40) t += std::format(" enabled {}->{},", (a.flags & 0x40) ? 1 : 0, (b.flags & 0x40) ? 1 : 0);
    if (df & ~0x60) t += std::format(" flags {:02X}->{:02X},", a.flags, b.flags);
    if (std::memcmp(&a.inten, &b.inten, 4) != 0) t += std::format(" intensity {:.3f}->{:.3f},", a.inten, b.inten);
    if (std::memcmp(a.col, b.col, sizeof a.col) != 0)
        t += std::format(" colour ({:.2f} {:.2f} {:.2f})->({:.2f} {:.2f} {:.2f}),", a.col[0], a.col[1], a.col[2], b.col[0], b.col[1], b.col[2]);
    if (std::memcmp(&a.range, &b.range, 4) != 0) t += std::format(" range {:.2f}->{:.2f},", a.range, b.range);
    if (std::memcmp(a.pos, b.pos, sizeof a.pos) != 0) {
        const float dx = b.pos[0] - a.pos[0], dy = b.pos[1] - a.pos[1], dz = b.pos[2] - a.pos[2];
        t += std::format(" moved {:.3f} m,", std::sqrt(dx * dx + dy * dy + dz * dz));
    }
    if (t.back() == ',') t.pop_back();
    if (a.baked != b.baked) t += b.baked ? " [enters the bake]" : " [leaves the bake]";
    return t;
}

void AddDetail(std::string& d, int& n, const std::string& t) {
    if (n < 6) d += (d.empty() ? "" : "; ") + t;
    else if (n == 6) d += "; ...";
    n++;
}

// One pass over the enumeration g_enumLights (every 20 frames; before 2026-09-29 two passes, UpdateLampList and
// TrackLotLampEdits): the lit outdoor lamps for SelectLamps (only when rebuildAll, as UpdateLampList did) and the tracked
// lot lamps (g_lotLampCur, sorted by light, the first reading of a light kept, as std::map::emplace did).
// Lamp switches on any lot, indoors too (lit flag or intensity of a lot lamp of type 3..6 changed between two
// enumerations): the night patch gives the rooms the game's own lighting budget for a moment (LotLightingMotion::Boost),
// so a room relights at once when a Sim switches its lamp, even while the camera moves.
// Only real on / off toggles (lit flag 0x20) of alive lamps count; a lamp that toggled 3 times within 60 s (party, timed
// or flickering lamps) is ignored from then on, so it cannot keep the boost on (review 29/09).
struct LampSwitchState {
    bool lit = false;
    int toggles = 0;           // within the current 60 s window
    Clock::time_point since{}; // start of that window
};
std::unordered_map<uintptr_t, LampSwitchState> g_lampSwitchPrev;
std::atomic<int> g_lampSwitches{0};
bool LampLit(uintptr_t L, bool& lit) {
    __try {
        if ((*reinterpret_cast<const uint32_t*>(L + 0xC0) | *reinterpret_cast<const uint32_t*>(L + 0xC4)) == 0) return false; // not a lot lamp
        const BYTE f = *reinterpret_cast<const BYTE*>(L + 0x100);
        if (!(f & 0x01) || !IsPlainType(*reinterpret_cast<const int*>(L + 0xB0))) return false; // alive, lamp classes
        lit = (f & 0x20) != 0;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
void TrackLampSwitches() {
    const auto now = Clock::now();
    std::unordered_map<uintptr_t, LampSwitchState> cur;
    cur.reserve(g_lampSwitchPrev.size() + 16);
    bool switched = false;
    for (uintptr_t L : g_enumLights) {
        bool lit = false;
        if (!LampLit(L, lit)) continue;
        LampSwitchState s;
        s.lit = lit;
        s.since = now;
        if (const auto it = g_lampSwitchPrev.find(L); it != g_lampSwitchPrev.end()) {
            s = it->second;
            if (now - s.since > std::chrono::seconds(60)) {
                s.toggles = 0;
                s.since = now;
            }
            if (s.lit != lit) {
                s.lit = lit;
                if (++s.toggles < 3) switched = true; // 3+ toggles in 60 s: animated, ignored
            }
        }
        cur[L] = s;
    }
    g_lampSwitchPrev.swap(cur);
    if (switched) g_lampSwitches.fetch_add(1, std::memory_order_relaxed);
}

void ReadEnumeratedLamps(bool rebuildAll) {
    TrackLampSwitches();
    static std::vector<std::array<float, 8>> previous;
    if (rebuildAll) {
        previous.swap(g_allLamps);
        g_allLamps.clear();
    }
    g_lotLampCur.clear();
    for (uintptr_t L : g_enumLights) {
        if (rebuildAll) {
            std::array<float, 8> v;
            if (ReadLamp(L, v.data())) g_allLamps.push_back(v);
        }
        LotLampState s;
        if (ReadLotLamp(L, s)) {
            s.baked = InBake(s);
            g_lotLampCur.emplace_back(L, s);
        }
    }
    if (rebuildAll) {
        g_lampCount = static_cast<int>(g_allLamps.size());
        // the SelectLamps memo stays valid only while the list is the same, bit for bit and in the same order
        if (g_allLamps.size() != previous.size() || (!g_allLamps.empty() && std::memcmp(g_allLamps.data(), previous.data(), g_allLamps.size() * sizeof(g_allLamps[0])) != 0))
            g_lampMemoGen++;
    }
    std::stable_sort(g_lotLampCur.begin(), g_lotLampCur.end(), [](const LampSig& a, const LampSig& b) { return a.first < b.first; });
    g_lotLampCur.erase(std::unique(g_lotLampCur.begin(), g_lotLampCur.end(), [](const LampSig& a, const LampSig& b) { return a.first == b.first; }), g_lotLampCur.end());
}

// Compares g_lotLampCur (just read) with g_lotLampSig (the previous enumeration), then keeps the new one.
void TrackLotLampEdits() {
    const auto now = Clock::now();
    const bool editing = ChunkRelight::Editing();
    std::vector<LampSig>& cur = g_lotLampCur;
    g_lotsNow.clear();
    for (const auto& [L, s] : cur) g_lotsNow.push_back(s.lot);
    std::sort(g_lotsNow.begin(), g_lotsNow.end());
    g_lotsNow.erase(std::unique(g_lotsNow.begin(), g_lotsNow.end()), g_lotsNow.end());
    const auto lotNow = [](uint64_t lot) { return std::binary_search(g_lotsNow.begin(), g_lotsNow.end(), lot); };
    // Anything the bake snapshot is built from changed: a lamp added or removed, its lot, or a raw change (flags, type,
    // colour, intensity, range, position; baked and animated follow from those). Otherwise the snapshot is kept as it is.
    bool snapDirty = false;
    // per lot: counted additions / edits / removals in this enumeration, and (dev) what changed
    struct Change {
        int added = 0, edited = 0, moved = 0, removed = 0;
        bool userEdited = false;
        std::string detail;
        int details = 0;
    };
    std::map<uint64_t, Change> changes;
    struct Quiet { // dev log: changes that do not count
        std::string detail;
        int details = 0;
    };
    std::map<uint64_t, Quiet> quiet;
    int total = 0;
    auto prev = g_lotLampSig.begin(); // both lists are sorted by light: one walk finds each lamp's previous state
    for (auto& [L, s] : cur) {
        while (prev != g_lotLampSig.end() && prev->first < L) ++prev;
        if (prev == g_lotLampSig.end() || prev->first != L) {
            snapDirty = true;
            if (VisibleLot(s.lot)) g_lotArrivals.insert(s.lot); // late lamp registration on a drawn lot
            if (s.baked) {
                Change& c = changes[s.lot];
                c.added++;
                total++;
                if (Recorder::Verbose()) AddDetail(c.detail, c.details, std::format("L{:08X} type {}: added", L, s.type));
            }
            continue;
        }
        const LotLampState& p = prev->second;
        s.autoChanges = p.autoChanges;
        s.autoWindow = p.autoWindow;
        s.lastAuto = p.lastAuto;
        s.animated = p.animated;
        if (p.lot != s.lot) snapDirty = true;
        if (!RawChanged(p, s)) {
            // the rect follows position and range (FUN_006BDDF0); should the game update it an enumeration later, the
            // snapshot still gets the current one (it never counts as a change by itself)
            if (std::memcmp(p.rect, s.rect, sizeof s.rect) != 0) snapDirty = true;
            continue;
        }
        snapDirty = true;
        const char* why = nullptr; // why a raw change does not count
        bool counts = false, moved = false;
        float pl[3], sl[3];
        for (int c = 0; c < 3; c++) {
            pl[c] = LampLight(p, c);
            sl[c] = LampLight(s, c);
        }
        if (p.baked && s.baked && MovedApart(p.pos, s.pos)) {
            counts = moved = true; // user-driven (Build mode)
        } else if ((editing || (s.type == 11 && p.type == 11 && p.lot == s.lot &&
                    (s.lot != 0 || (p.baked && s.baked && LightChanged(pl, sl)) || ((p.flags ^ s.flags) & 0x40)
                     || ((p.inten <= 1e-3f) != (s.inten <= 1e-3f)))) || ObservedEnableSwitch(p, s)
                    || (p.lot == s.lot && p.type == s.type && IsPlainType(s.type) && VisibleLot(s.lot))) &&
                   (p.baked != s.baked || (p.baked && s.baked && LightChanged(pl, sl)))) {
            // Known enable/zero-intensity switches, visible ordinary value edits and type-11 edits
            // take the bounded local path, including a switch just after lot entry.
            // Repeated manual switches must not become ignored "animated" lights.
            // Engine editInGameMode (2) is not assumed to identify every Build/Buy UI state.
            if (s.animated && g_lampsAnimated > 0) g_lampsAnimated--;
            s.animated = false;
            s.autoChanges = 0;
            s.autoWindow = now;
            counts = true;
            changes[s.lot].userEdited = true;
        } else if (p.baked != s.baked || (p.baked && s.baked && LightDiffers(pl, sl))) {
            // automatic: switched on / off, dimmed, recoloured
            if (s.animated && now - s.lastAuto > kAnimatedExpire) { // quiet long enough: a switch again (30/09: lamps toggled while testing stayed ignored)
                s.animated = false;
                s.autoChanges = 0;
                s.autoWindow = now;
                if (g_lampsAnimated > 0) g_lampsAnimated--;
            }
            s.lastAuto = now;
            if (now - s.autoWindow > kAnimatedWindow) {
                s.autoWindow = now;
                s.autoChanges = 0;
            }
            if (++s.autoChanges >= kAnimatedChanges && !s.animated) {
                s.animated = true;
                g_lampsAnimated++;
                if (Recorder::Verbose())
                    LOG_INFO(std::format("[LotLightBridge] Lamp L{:08X} (type {}) on lot {:016X} switches or dims by itself ({} changes within {} s): its changes no longer "
                                         "rebuild the terrain (the next rebuild takes its state)",
                                         L, s.type, s.lot, s.autoChanges, kAnimatedWindow.count()));
            }
            if (s.animated) {
                why = "animated";
                g_lampChangesAnimated++;
            } else
                counts = true;
        } else if (p.baked || s.baked) {
            why = "below the threshold";
            g_lampChangesNoise++;
        } else {
            why = "not in the bake";
            g_lampChangesOutside++;
        }
        if (counts) {
            Change& c = changes[s.lot];
            c.edited++;
            if (moved) c.moved++;
            total++;
            if (Recorder::Verbose()) AddDetail(c.detail, c.details, LampChangeText(L, p, s));
        } else if (Recorder::Verbose()) {
            Quiet& q = quiet[s.lot];
            AddDetail(q.detail, q.details, LampChangeText(L, p, s) + " (" + why + ")");
        }
    }
    auto inCur = cur.cbegin(); // removals: lamps of the previous enumeration missing from this one (same walk)
    for (const auto& [L, p] : g_lotLampSig) {
        while (inCur != cur.cend() && inCur->first < L) ++inCur;
        if (inCur != cur.cend() && inCur->first == L) continue;
        snapDirty = true;
        if (p.baked) {
            Change& c = changes[p.lot];
            c.removed++;
            total++;
            if (Recorder::Verbose()) AddDetail(c.detail, c.details, std::format("L{:08X} type {}: removed", L, p.type));
        }
    }
    g_lotLampSig.swap(cur); // g_lotLampCur keeps the old list's memory for the next read

    // lots seen: new lots start their settle time, vanished lots are forgotten (with any pending removal)
    for (uint64_t lot : g_lotsNow)
        if (!g_lotSeen.count(lot)) g_lotSeen[lot] = LotSeen{now, now, false};
    for (auto it = g_lotSeen.begin(); it != g_lotSeen.end();) {
        if (!lotNow(it->first)) it = g_lotSeen.erase(it);
        else ++it;
    }
    for (auto it = g_quietLogAt.begin(); it != g_quietLogAt.end();) {
        if (!lotNow(it->first)) it = g_quietLogAt.erase(it);
        else ++it;
    }

    int counted = 0, ignored = 0;
    bool userDriven = false;
    std::vector<uint64_t> userLots;
    std::string what;
    // removals seen last time: confirmed when the lot is still here and lost no more lamps
    for (auto& [lot, seen] : g_lotSeen) {
        if (!seen.removalPending) continue;
        auto c = changes.find(lot);
        if (c != changes.end() && c->second.removed > 0) continue; // still losing lamps: wait
        seen.removalPending = false;
        counted++;
        userDriven = true;
        userLots.push_back(lot);
        what = std::format("lamp removed on lot {:016X} (user-driven)", lot);
    }
    // More than 8 changes = lamps switching at dusk / dawn or streaming in bulk; except when they are all switches (no lamp
    // added or removed) of ONE lot: a lot's own lamps switched together (30/09: a town square's 57 lamps were ignored here,
    // so only the slow stuck-countdown fallback took them, ~15-30 s late). Dusk / dawn is still left to the dusk rebuild
    // by the terrain relight (lamp changes by day are not rebuilt), and an unsettled lot is still ignored below.
    const bool oneLotSwitch = changes.size() == 1 && changes.begin()->second.added == 0 && changes.begin()->second.removed == 0;
    const bool bulk = total > 8 && !oneLotSwitch;
    for (const auto& [lot, c] : changes) {
        // World lamps share ID zero. Streaming and the town-wide dawn/dusk switch
        // are not individual edits; only changes of already observed lamps qualify.
        if (!WorldLampPolicy::AcceptEdit(lot, c.added, c.removed, c.edited, c.userEdited)) {
            ignored++;
            continue;
        }
        auto s = g_lotSeen.find(lot);
        if (s == g_lotSeen.end()) { // the lot vanished: streaming out
            ignored++;
            continue;
        }
        LotSeen& seen = s->second;
        const bool settled = now - seen.firstSeen >= std::chrono::seconds(10) && now - seen.lastUncounted >= std::chrono::seconds(5);
        const bool observedValueEdit = c.userEdited && c.added == 0 && c.removed == 0;
        // Streaming on another lot must not hide a real edit of this lot's known
        // lamps. Additions/removals on this lot still require the streaming guards.
        if (observedValueEdit) seen.explicitEdit = true;
        if ((!settled || bulk) && !observedValueEdit) {
            seen.lastUncounted = now;
            seen.removalPending = false;
            ignored++;
            continue;
        }
        if (c.removed > 0) seen.removalPending = true; // confirmed at the next enumeration
        if (c.added > 0 || c.edited > 0) {
            counted++;
            const bool user = c.added > 0 || c.moved > 0 || c.userEdited;
            userDriven |= user;
            if (user) userLots.push_back(lot);
            what = std::format("lot {:016X}: {} added, {} edited ({} moved), {} removed ({})", lot, c.added, c.edited, c.moved, c.removed, user ? "user-driven" : "automatic");
            if (!c.detail.empty()) what += ": " + c.detail;
        }
    }
    g_lotChangesIgnored += ignored;
    if (counted > 0) {
        g_lotChangesCounted += counted;
        g_lastLotChange = what;
        if (userDriven) {
            g_lastUserLots = std::move(userLots);
            g_lotLampUserEdits.fetch_add(1, std::memory_order_relaxed);
        }
        g_lotLampEdits.fetch_add(1, std::memory_order_relaxed);
        if (Recorder::Verbose()) LOG_INFO("[LotLightBridge] Lot lamp change: " + what);
    } else if (ignored > 0) {
        if (Recorder::Verbose())
            LOG_DEBUG(std::format("[LotLightBridge] Lot lamp changes ignored ({} lots; {}): streaming, lots still loading or lamps switching together", ignored,
                                  bulk ? "bulk" : "not settled"));
    }
    if (Recorder::Verbose()) {
        // what changed but does not rebuild (at most once a minute per lot): the diagnosis of lamps that keep changing
        for (const auto& [lot, q] : quiet) {
            auto& at = g_quietLogAt[lot];
            if (at != Clock::time_point{} && now - at < kQuietLogEvery) continue;
            at = now;
            LOG_INFO(std::format("[LotLightBridge] Lamp changes that do not rebuild the terrain, lot {:016X}: {}", lot, q.detail));
        }
    }

    // snapshot for the terrain relight: every tracked lamp (baked or not, so a lamp switched off still matches its
    // baked self by position), sorted by lot; the lots and the settled lots. The lamps and lots are rebuilt only when
    // something they are made of changed (snapDirty): unchanged, the rebuild would produce the same vectors.
    if (snapDirty) {
        g_bakeSnap.lamps.clear();
        g_bakeSnap.lots.clear();
        for (const auto& [L, s] : g_lotLampSig) {
            LotLightBridge::BakeLamp b;
            b.lot = s.lot;
            b.type = s.type;
            std::memcpy(b.pos, s.pos, sizeof b.pos);
            std::memcpy(b.rect, s.rect, sizeof b.rect);
            for (int c = 0; c < 3; c++) b.light[c] = LampLight(s, c);
            b.baked = s.baked;
            b.animated = s.animated;
            g_bakeSnap.lamps.push_back(b);
        }
        std::stable_sort(g_bakeSnap.lamps.begin(), g_bakeSnap.lamps.end(), [](const LotLightBridge::BakeLamp& a, const LotLightBridge::BakeLamp& b) { return a.lot < b.lot; });
        g_bakeSnap.lots = g_lotsNow; // sorted, unique
    }
    g_bakeSnap.settledLots.clear(); // time-dependent: every refresh
    for (const auto& [lot, seen] : g_lotSeen)
        if (seen.explicitEdit || (now - seen.firstSeen >= std::chrono::seconds(10) && now - seen.lastUncounted >= std::chrono::seconds(5))) g_bakeSnap.settledLots.push_back(lot);
    g_lampEnumerations++;
}

// Picks up to 16 lamps whose light can reach the roof piece at world position (x, z): chosen per draw from the roof
// position, so the camera (zoom, rotation) never changes which lamps light a roof.
int g_lastLampCandidates = 0; // lamps within reach at the last SelectLamps (light probe detail)

// Memo of SelectLamps (2026-09-29, render thread). Its result is a function of (x, z, maxScore) and g_allLamps alone, and
// g_allLamps changes only in ReadEnumeratedLamps (every 20 frames), which starts a new generation whenever the list is not
// the same bit for bit: a hit returns exactly the rows, count and candidate number the scan would produce (keys are the
// exact float bits, so no two different inputs share an entry). Most roof pieces and objects are drawn at the same place
// frame after frame. The memo is cleared (new generation) and never needs a size limit (direct-mapped, 512 entries).
struct LampMemo {
    uint32_t gen = 0; // 0 = empty
    uint32_t x = 0, z = 0, maxScore = 0;
    int picked = 0, candidates = 0;
    float rows[32][4] = {}; // g_lampData[0..31] as SelectLamps leaves them
};
constexpr uint32_t kLampMemoSize = 512; // direct-mapped, indexed by the top 9 bits of a hash
static_assert(kLampMemoSize == (1u << (32 - 23)));
LampMemo g_lampMemo[kLampMemoSize];
uint32_t g_lampMemoHits = 0, g_lampMemoMisses = 0;

uint32_t FloatBits(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}

int SelectLampsScan(float x, float z, float maxScore);

int SelectLamps(float x, float z, float maxScore) {
    const uint32_t bx = FloatBits(x), bz = FloatBits(z), bm = FloatBits(maxScore);
    LampMemo& e = g_lampMemo[((bx * 0x9E3779B1u) ^ (bz * 0x85EBCA77u) ^ (bm * 0xC2B2AE3Du)) >> 23]; // top 9 bits: 512 entries
    if (e.gen == g_lampMemoGen && e.x == bx && e.z == bz && e.maxScore == bm) {
        g_lampMemoHits++;
        g_lastLampCandidates = e.candidates;
        std::memcpy(g_lampData, e.rows, sizeof(e.rows));
        return e.picked;
    }
    g_lampMemoMisses++;
    const int m = SelectLampsScan(x, z, maxScore);
    e.gen = g_lampMemoGen;
    e.x = bx;
    e.z = bz;
    e.maxScore = bm;
    e.picked = m;
    e.candidates = g_lastLampCandidates;
    std::memcpy(e.rows, g_lampData, sizeof(e.rows));
    return m;
}

int SelectLampsScan(float x, float z, float maxScore) {
    struct Cand { float score; const std::array<float, 8>* v; };
    Cand c[64];
    int n = 0;
    for (const auto& v : g_allLamps) {
        const float dx = v[0] - x, dz = v[2] - z;
        const float d = std::sqrt(dx * dx + dz * dz);
        const float score = d - v[3];
        if (score > maxScore) continue; // too far from the lamp's reach
        if (n < 64) c[n++] = {score, &v};
        else {
            int worst = 0;
            for (int k = 1; k < 64; k++)
                if (c[k].score > c[worst].score) worst = k;
            if (score < c[worst].score) c[worst] = {score, &v};
        }
    }
    g_lastLampCandidates = n;
    const int m = std::min(16, n);
    std::partial_sort(c, c + m, c + n, [](const Cand& a, const Cand& b) { return a.score < b.score; });
    std::memset(g_lampData, 0, sizeof(float) * 4 * 32);
    for (int k = 0; k < m; k++) {
        std::memcpy(g_lampData[k], c[k].v->data(), 16);
        std::memcpy(g_lampData[16 + k], c[k].v->data() + 4, 16);
    }
    return m;
}

template <typename DrawFn> bool DrawRoof(IDirect3DDevice9* dev, DrawFn draw);

void EnsureReplacement(IDirect3DDevice9* dev) {
    if (g_replacementPs || g_compileTried) return;
    g_compileTried = true;
    const std::string err = CompilePs(dev, kReplacementPsId, &g_replacementPs);
    g_status = err.empty() ? "Active" : "Failed: " + err;
    LOG_INFO("[LotLightBridge] " + g_status);
}

void EnsureObjectReplacement(IDirect3DDevice9* dev) {
    if (g_objectPs || g_objectCompileTried) return;
    g_objectCompileTried = true;
    const std::string err = CompilePs(dev, kObjectRigPsId, &g_objectPs);
    LOG_INFO("[LotLightBridge] Object shadow fix: " + (err.empty() ? std::string("active") : err));
}

template <typename DrawFn> bool DrawRoof(IDirect3DDevice9* dev, DrawFn draw) {
    if (!g_roofFix.load(std::memory_order_relaxed) || !g_curVsIsRoof) return false;
    float world[12];
    if (FAILED(dev->GetVertexShaderConstantF(8, world, 3))) return false; // c8..c10 = world matrix rows, .w = translation
    if (!g_roofPs && !g_roofCompileTried) {
        g_roofCompileTried = true;
        const std::string err = CompilePs(dev, kRoofPsId, &g_roofPs);
        LOG_INFO("[LotLightBridge] Roofs: " + (err.empty() ? std::string("active") : err));
    }
    if (!g_roofPs) return false;
    const int picked = SelectLamps(world[3], world[11], 80.0f);
    g_lampData[32][0] = g_roofStrength.load(std::memory_order_relaxed);
    g_lampData[32][1] = static_cast<float>(picked);
    float saved[33][4];
    dev->GetPixelShaderConstantF(20, &saved[0][0], 33);
    IDirect3DPixelShader9* original = g_curPs;
    g_inOwnCall = true;
    SetPs(dev, g_roofPs);
    SetPsConst(dev, 20, &g_lampData[0][0], 33);
    draw();
    SetPsConst(dev, 20, &saved[0][0], 33);
    SetPs(dev, original);
    g_inOwnCall = false;
    g_roofDrawn.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// ---- Lakes: the game's lake water only reflects a fixed sky cube and gets no lamp light. After the game draws it, an
// additive pass on the same geometry adds lamp reflections and glow (the game's shader and states stay untouched). ----
std::atomic<bool> g_waterFix{false};
std::atomic<float> g_waterStrength{1.0f};
std::atomic<float> g_waterRefl{1.0f};
std::atomic<bool> g_waterFilter{true}, g_waterColorCompression{true};
IDirect3DPixelShader9* g_waterPs = nullptr;
bool g_waterCompileTried = false;
std::atomic<int> g_waterDrawn{0};

template <typename DrawFn> bool DrawLake(IDirect3DDevice9* dev, DrawFn draw) {
    if (!g_waterFix.load(std::memory_order_relaxed)) return false;
    if (!g_curVsIsLake) {
        static bool logged = false; // the lake pixel shader with another vertex shader: the pass reads the lake one's outputs
        if (!logged) { logged = true; LOG_INFO("[LotLightBridge] Water: lake pixel shader drawn with another vertex shader, skipped"); }
        return false;
    }
    if (!g_waterPs && !g_waterCompileTried) {
        g_waterCompileTried = true;
        const std::string err = CompilePs(dev, kWaterPsId, &g_waterPs);
        LOG_INFO("[LotLightBridge] Water: " + (err.empty() ? std::string("active") : err));
    }
    if (!g_waterPs) return false;
    float world[12];
    if (FAILED(dev->GetVertexShaderConstantF(8, world, 3))) return false;
    g_inOwnCall = true;
    draw(); // the game's water, unchanged
    const int picked = SelectLamps(world[3], world[11], 150.0f);
    {
        g_lampData[32][0] = g_waterStrength.load(std::memory_order_relaxed);
        g_lampData[32][1] = static_cast<float>(picked);
        g_lampData[32][2] = g_waterFilter.load(std::memory_order_relaxed) ? 1.0f : 0.0f;
        g_lampData[32][3] = g_waterColorCompression.load(std::memory_order_relaxed) ? 1.0f : 0.0f;
        float consts[40][4] = {};
        std::memcpy(consts, g_lampData, sizeof(float) * 4 * 33);
        dev->GetVertexShaderConstantF(4, &consts[33][0], 4); // world-view-projection of the water mesh
        // The shader projects world positions, so turn local->clip into world->clip: M * inverse(World). The water mesh
        // of a rotated lot has a rotated world matrix (rows c8..c10), so subtracting the translation is not enough.
        {
            const float a = world[0], b = world[1], c = world[2], d = world[4], e = world[5], f = world[6], g = world[8], h = world[9], k = world[10];
            const float det = a * (e * k - f * h) - b * (d * k - f * g) + c * (d * h - e * g);
            if (std::fabs(det) > 1e-8f) {
                const float id = 1.0f / det;
                float inv[4][4] = {{(e * k - f * h) * id, (c * h - b * k) * id, (b * f - c * e) * id, 0},
                                   {(f * g - d * k) * id, (a * k - c * g) * id, (c * d - a * f) * id, 0},
                                   {(d * h - e * g) * id, (b * g - a * h) * id, (a * e - b * d) * id, 0},
                                   {0, 0, 0, 1}};
                const float t[3] = {world[3], world[7], world[11]};
                for (int r = 0; r < 3; r++) inv[r][3] = -(inv[r][0] * t[0] + inv[r][1] * t[1] + inv[r][2] * t[2]);
                float vp[4][4];
                for (int r = 0; r < 4; r++)
                    for (int col = 0; col < 4; col++)
                        vp[r][col] = consts[33 + r][0] * inv[0][col] + consts[33 + r][1] * inv[1][col] + consts[33 + r][2] * inv[2][col] + consts[33 + r][3] * inv[3][col];
                std::memcpy(consts[33], vp, sizeof(vp)); // translation c57 stays 0: positions are world positions
            } else {
                consts[37][0] = world[3]; consts[37][1] = world[7]; consts[37][2] = world[11];
            }
        }
        consts[38][0] = g_waterRefl.load(std::memory_order_relaxed);
        // Scene depth for the reflection ray march: Depth Blur's INTZ, readable once it is unbound as depth-stencil.
        IDirect3DTexture9* depthTex = DepthShare::Texture();
        IDirect3DSurface9* curDs = nullptr;
        ExtraHooks::RawGetDepthStencilSurface(dev, &curDs);
        const bool useDepth = depthTex && curDs && curDs == DepthShare::Surface();
        if (useDepth) {
            // device z = A + B / w, from the projection rows: row2 = A * row3 + (0, 0, 0, B)
            const float* r2 = consts[35];
            const float* r3 = consts[36];
            const float d33 = r3[0] * r3[0] + r3[1] * r3[1] + r3[2] * r3[2];
            const float A = d33 > 1e-12f ? (r2[0] * r3[0] + r2[1] * r3[1] + r2[2] * r3[2]) / d33 : 1.0f;
            consts[39][0] = A;
            consts[39][1] = r2[3] - A * r3[3];
            consts[39][2] = 1.0f;
        }
        float saved[40][4];
        dev->GetPixelShaderConstantF(20, &saved[0][0], 40);
        IDirect3DBaseTexture9* old7 = nullptr;
        DWORD s7u = 0, s7v = 0, s7min = 0, s7mag = 0, s7mip = 0, s7srgb = 0, zen = 0;
        if (useDepth) {
            dev->GetTexture(7, &old7);
            dev->GetSamplerState(7, D3DSAMP_ADDRESSU, &s7u);
            dev->GetSamplerState(7, D3DSAMP_ADDRESSV, &s7v);
            dev->GetSamplerState(7, D3DSAMP_MINFILTER, &s7min);
            dev->GetSamplerState(7, D3DSAMP_MAGFILTER, &s7mag);
            dev->GetSamplerState(7, D3DSAMP_MIPFILTER, &s7mip);
            dev->GetSamplerState(7, D3DSAMP_SRGBTEXTURE, &s7srgb);
            dev->GetRenderState(D3DRS_ZENABLE, &zen);
            dev->SetRenderState(D3DRS_ZENABLE, FALSE);
            ExtraHooks::RawSetDepthStencilSurface(dev, nullptr);
            SetTex(dev, 7, depthTex);
            dev->SetSamplerState(7, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
            dev->SetSamplerState(7, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
            dev->SetSamplerState(7, D3DSAMP_MINFILTER, D3DTEXF_POINT);
            dev->SetSamplerState(7, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
            dev->SetSamplerState(7, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
            dev->SetSamplerState(7, D3DSAMP_SRGBTEXTURE, FALSE);
        }
        DWORD ab, sb, db, bo, sep, zw, cw, at;
        dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &ab);
        dev->GetRenderState(D3DRS_SRCBLEND, &sb);
        dev->GetRenderState(D3DRS_DESTBLEND, &db);
        dev->GetRenderState(D3DRS_BLENDOP, &bo);
        dev->GetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, &sep);
        dev->GetRenderState(D3DRS_ZWRITEENABLE, &zw);
        dev->GetRenderState(D3DRS_COLORWRITEENABLE, &cw);
        dev->GetRenderState(D3DRS_ALPHATESTENABLE, &at);
        IDirect3DPixelShader9* original = g_curPs;
        dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE); // premultiplied: reflection * a + lamps
        dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVSRCALPHA);
        dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
        dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
        dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        dev->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE);
        dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        SetPs(dev, g_waterPs);
        SetPsConst(dev, 20, &consts[0][0], 40);
        DepthShare::SetInternalPass(true); // ZENABLE is off here: not the first UI draw for Depth Blur
        draw();
        DepthShare::SetInternalPass(false);
        SetPsConst(dev, 20, &saved[0][0], 40);
        if (useDepth) {
            SetTex(dev, 7, old7);
            if (old7) old7->Release();
            dev->SetSamplerState(7, D3DSAMP_ADDRESSU, s7u);
            dev->SetSamplerState(7, D3DSAMP_ADDRESSV, s7v);
            dev->SetSamplerState(7, D3DSAMP_MINFILTER, s7min);
            dev->SetSamplerState(7, D3DSAMP_MAGFILTER, s7mag);
            dev->SetSamplerState(7, D3DSAMP_MIPFILTER, s7mip);
            dev->SetSamplerState(7, D3DSAMP_SRGBTEXTURE, s7srgb);
            ExtraHooks::RawSetDepthStencilSurface(dev, curDs);
            dev->SetRenderState(D3DRS_ZENABLE, zen);
        }
        if (curDs) curDs->Release();
        SetPs(dev, original);
        dev->SetRenderState(D3DRS_ALPHATESTENABLE, at);
        dev->SetRenderState(D3DRS_COLORWRITEENABLE, cw);
        dev->SetRenderState(D3DRS_ZWRITEENABLE, zw);
        dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, sep);
        dev->SetRenderState(D3DRS_BLENDOP, bo);
        dev->SetRenderState(D3DRS_DESTBLEND, db);
        dev->SetRenderState(D3DRS_SRCBLEND, sb);
        dev->SetRenderState(D3DRS_ALPHABLENDENABLE, ab);
        g_waterDrawn.fetch_add(1, std::memory_order_relaxed);
    }
    g_inOwnCall = false;
    return true;
}

// ---- Snow: the lot light pass has a much bigger snow variant (snow normals, snow mask). Instead of rewriting it, its
// bytecode is patched: after "texld r0, v3, s2" (lot light map) and the following "mul r0.xyz, r1.w, r0" (light basis
// factor) insert "texld r7, v7(TEXCOORD1), s12", r7 x4 and "max r0.xyz, r0, r7", so it takes the brighter of the lot
// light map and the terrain light map. The x4 undoes the "mul r0.xyz, r4, c9.x" (0.25) this variant applies to lamp
// light further down; the world snow terrain uses the light map x c7.x (1). Its VS outputs the terrain uv in TEXCOORD1
// (c15) and gets the chunk centre in c16. ----
std::vector<DWORD> ShaderCode(IDirect3DPixelShader9* ps);
IDirect3DPixelShader9* g_snowPs = nullptr;
bool g_snowTried = false;
std::atomic<int> g_snowDrawn{0};

bool PatchSnowBytecode(std::vector<DWORD>& t) {
    auto regNum = [](DWORD r) { return r & 0x7FF; };
    auto regType = [](DWORD r) { return ((r >> 28) & 7) | (((r >> 11) & 3) << 3); };
    size_t dclEnd = 0, texldEnd = 0;
    for (size_t i = 1; i < t.size();) {
        const DWORD tok = t[i];
        if (tok == 0x0000FFFF) break;
        if ((tok & 0xFFFF) == 0xFFFE) { i += 1 + ((tok >> 16) & 0x7FFF); continue; }
        const size_t len = (tok >> 24) & 0xF;
        const DWORD op = tok & 0xFFFF;
        if (i + len >= t.size()) return false;
        if (op == 0x1F && regType(t[i + 2]) == 10 && regNum(t[i + 2]) == 11) dclEnd = i + 1 + len;
        if (op == 0x42 && !texldEnd && regType(t[i + 1]) == 0 && regNum(t[i + 1]) == 0 && regType(t[i + 2]) == 1 && regNum(t[i + 2]) == 3 && regType(t[i + 3]) == 10 && regNum(t[i + 3]) == 2)
            texldEnd = i + 1 + len;
        i += 1 + len;
    }
    if (!dclEnd || !texldEnd || texldEnd < dclEnd || texldEnd + 1 >= t.size()) return false;
    // next instruction must be "mul r0.xyz, r1.w, r0" (light basis factor)
    if ((t[texldEnd] & 0xFFFF) != 0x05 || regType(t[texldEnd + 1]) != 0 || regNum(t[texldEnd + 1]) != 0) return false;
    const size_t insertAt = texldEnd + 1 + ((t[texldEnd] >> 24) & 0xF);
    const DWORD fetch[] = {0x03000042, 0x802F0007, 0x90E40007, 0xA0E4080C,  // texld_pp r7, v7, s12
                           0x03000002, 0x80270007, 0x80E40007, 0x80E40007,  // add_pp r7.xyz, r7, r7
                           0x03000002, 0x80270007, 0x80E40007, 0x80E40007,  // add_pp r7.xyz, r7, r7
                           0x0300000B, 0x80270000, 0x80E40000, 0x80E40007}; // max_pp r0.xyz, r0, r7
    t.insert(t.begin() + insertAt, std::begin(fetch), std::end(fetch));
    const DWORD decl[] = {0x0200001F, 0x80010005, 0x90230007, 0x0200001F, 0x90000000, 0xA00F080C};
    t.insert(t.begin() + dclEnd, std::begin(decl), std::end(decl));
    return true;
}

template <typename DrawFn> bool DrawLotSnow(IDirect3DDevice9* dev, DrawFn draw) {
    if (!g_curVsIsSnowLot) return false;
    if (!g_snowPs && !g_snowTried) {
        g_snowTried = true;
        // patched from the shader the game has bound (this runs only for PsClass::LotLightSnow, an exact match)
        std::vector<DWORD> t = ShaderCode(g_curPs);
        const bool ok = !t.empty() && PatchSnowBytecode(t) && SUCCEEDED(dev->CreatePixelShader(t.data(), &g_snowPs));
        if (!ok) g_snowPs = nullptr;
        LOG_INFO(std::string("[LotLightBridge] Snow: ") + (ok ? "active" : "failed"));
    }
    if (!g_snowPs) return false;
    float v[8];
    if (FAILED(dev->GetVertexShaderConstantF(15, v, 2)) || !Near(v[0], 1.0f / 256.0f) || !Near(v[1], 1.0f / 256.0f)) return false;
    // The terrain light the lot is compared with: the world atlas when it is ready (a lot can reach past its "home"
    // chunk, c16: LightProbe-m76, lot at z 1290 on the chunk ending at z 1280, drew the chunk's clamped edge row and
    // came out dark next to a lit sidewalk), else the home chunk's own map as before. The VS computes the uv as
    // (world.xz - c16.xz) * c15.xy + c15.zw, so for the atlas (uv = world.xz * a.xy + a.zw) c15 becomes
    // (a.xy, a.zw + c16.xz * a.xy) for this draw.
    float atlasC[4];
    IDirect3DTexture9* atlas = LightmapSmooth::Atlas(atlasC);
    IDirect3DBaseTexture9* terrain = atlas;
    if (!atlas) {
        auto it = g_chunks.find(Key(v[4], v[6])); // c16.xz = chunk centre
        if (it == g_chunks.end() || !it->second.tex) {
            g_lotMissing.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        terrain = ChunkTexture(it->first, it->second.tex);
    }
    const float atlasMap[4] = {atlasC[0], atlasC[1], atlasC[2] + v[4] * atlasC[0], atlasC[3] + v[6] * atlasC[1]};
    IDirect3DPixelShader9* original = g_curPs;
    g_inOwnCall = true;
    {
        // s12 = the terrain light (clamp, linear, linear mips, no sRGB): only the states that differ are set and restored
        SamplerBind terrainMap(dev, 12, terrain);
        SetPs(dev, g_snowPs);
        if (atlas) SetVsConst(dev, 15, atlasMap, 1);
        ConstGain lampGain(dev, 4, GroundGain()); // c4.x (read once) scales max(lot map, terrain) only
        draw();
        if (atlas) SetVsConst(dev, 15, v, 1);
        SetPs(dev, original);
    }
    g_inOwnCall = false;
    g_snowDrawn.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// ---- Roads. Every road piece (straight road + sidewalk, lane markings, corners, the edge blended into the terrain: 4
// shader variants seen so far) is drawn with the same vertex shader (shader_ids.h kRoadVs). It samples its own
// copy of the chunk light map with the terrain uv (local xz / 256 + 0.5, VS c16) and the chunk centre in the world
// matrix (VS c8.w, c10.w). That copy does not get the lamps the world terrain map has, so roads stayed dark next to lit
// ground. Any pixel shader drawn with that vertex shader is patched by pattern (ShaderPatches::PatchRoad): max with the
// terrain map on a free sampler; with the smoothed map ready it also replaces the road's own copy. Where the shader has
// the sidewalk snow blend, "Calcada com neve pisada" mixes the plain road texture back on the bright parts.
struct PatchedPs {
    IDirect3DPixelShader9* ps = nullptr;
    bool tried = false;
    ShaderPatches::RoadPatch road;
    ShaderPatches::FloorPatch floor;
    ShaderPatches::InstancedPatch inst;
    ShaderPatches::SnowCoverPatch snow;
    ShaderPatches::ObjectLampPatch obj;
    ShaderPatches::IndoorBasisPatch indoor;
    ShaderPatches::BasisSmoothPatch smooth;
    DWORD nightConst = 0;
    int cubeTint = -1; // PatchCubeTint
};
std::unordered_map<IDirect3DPixelShader9*, PatchedPs> g_roadPs, g_floorPs, g_snowFloorPs, g_snowFloorPs0, g_leafPs, g_fencePs, g_snowCoverPs, g_snowReliefPs, g_objLampPs;
std::atomic<int> g_roadDrawn{0}, g_floorDrawn{0}, g_snowFloorDrawn{0}, g_leafDrawn{0}, g_roofSnowDrawn{0}, g_foliageDrawn{0}, g_fenceDrawn{0}, g_snowCoverDrawn{0}, g_snowReliefDrawn{0}, g_objLampDrawn{0};
std::atomic<bool> g_objPixel{true};
std::atomic<float> g_objPixelStrength{1.0f};
std::atomic<bool> g_objPixelLamps{true};          // outdoor rig objects: world lamps per pixel instead of the rig lamps
std::atomic<float> g_objPixelLampStrength{1.0f};
std::atomic<bool> g_fenceFix{true};
std::atomic<float> g_fenceStrength{1.0f};
std::atomic<float> g_sidewalkClear{0.5f};

std::vector<DWORD> ShaderCode(IDirect3DPixelShader9* ps) {
    UINT size = 0;
    if (!ps || FAILED(ps->GetFunction(nullptr, &size)) || size < 8 || size > 65536) return {};
    std::vector<DWORD> t(size / 4);
    if (FAILED(ps->GetFunction(t.data(), &size))) return {};
    return t;
}

// Development build: keeps the bytecode of every shader pair a fix refused, in Apex Radiance\ShadersRecusados, so it can be
// studied offline (the log only has the shader's address, which changes between sessions).
void SaveRefused(const char* what, const std::vector<DWORD>& ps) {
    if (kPublicBuild) return;
    try {
        std::string tag;
        for (const char* c = what; *c; ++c) tag += std::isalnum(static_cast<unsigned char>(*c)) ? *c : '_';
        // Named by content (the address changes every session), written once, at most 300 files per session.
        static int written = 0;
        if (written >= 300) return;
        auto hash = [](const std::vector<DWORD>& t) {
            uint32_t h = 2166136261u;
            for (DWORD d : t) h = (h ^ d) * 16777619u;
            return h;
        };
        const std::filesystem::path dir = std::filesystem::path(ApexPaths::ApexDirectory()) / L"ShadersRecusados";
        const auto save = [&](const std::string& name, const std::vector<DWORD>& data) {
            if (std::filesystem::exists(dir / name)) return;
            std::filesystem::create_directories(dir);
            std::ofstream(dir / name, std::ios::binary).write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size() * 4));
            written++;
        };
        const uint32_t psHash = hash(ps);
        save(std::format("{}_PS_{:08X}.bin", tag, psHash), ps);
        UINT size = 0;
        if (g_curVs && SUCCEEDED(g_curVs->GetFunction(nullptr, &size)) && size >= 8 && size <= 65536) {
            std::vector<DWORD> vs(size / 4);
            if (SUCCEEDED(g_curVs->GetFunction(vs.data(), &size))) save(std::format("{}_PS_{:08X}_VS_{:08X}.bin", tag, psHash, hash(vs)), vs);
        }
    } catch (...) {
    }
}

// Patched copy of the current pixel shader, made once per shader with `patch`.
template <typename PatchFn> PatchedPs& PatchedFor(IDirect3DDevice9* dev, std::unordered_map<IDirect3DPixelShader9*, PatchedPs>& cache, const char* what, PatchFn patch) {
    PatchedPs& p = cache[g_curPs];
    if (!p.tried) {
        p.tried = true;
        std::vector<DWORD> t = ShaderCode(g_curPs);
        const std::vector<DWORD> original = t;
        const bool matched = !t.empty() && patch(t, p);
        const HRESULT hr = matched ? dev->CreatePixelShader(t.data(), &p.ps) : E_FAIL;
        if (matched && SUCCEEDED(hr)) LOG_INFO(std::format("[LotLightBridge] {}: shader {:08X} patched", what, reinterpret_cast<uintptr_t>(g_curPs)));
        else {
            p.ps = nullptr;
            if (matched) LOG_WARNING(std::format("[LotLightBridge] {}: shader {:08X} refused by D3D ({:08X})", what, reinterpret_cast<uintptr_t>(g_curPs), static_cast<uint32_t>(hr)));
            else LOG_INFO(std::format("[LotLightBridge] {}: shader {:08X} does not have the expected pattern, left as the game draws it", what, reinterpret_cast<uintptr_t>(g_curPs)));
            if (!original.empty()) SaveRefused(what, original);
        }
    }
    return p;
}

template <typename DrawFn> bool DrawRoad(IDirect3DDevice9* dev, DrawFn draw) {
    PatchedPs& p = PatchedFor(dev, g_roadPs, "Road", [](std::vector<DWORD>& t, PatchedPs& pp) { return ShaderPatches::PatchRoad(t, pp.road); });
    if (!p.ps) return false;
    float m[4], w[12];
    if (FAILED(dev->GetVertexShaderConstantF(g_curRoadMap, m, 1)) || !Near(m[0], 1.0f / 256.0f) || !Near(m[1], 1.0f / 256.0f) || !Near(m[2], 0.5f) || !Near(m[3], 0.5f)) return false;
    if (FAILED(dev->GetVertexShaderConstantF(8, w, 3))) return false;
    auto it = g_chunks.find(Key(w[3], w[11])); // world matrix translation = chunk centre
    if (it == g_chunks.end() || !it->second.tex) {
        g_lotMissing.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    // With the smoothed map ready, the road's own copy (the blocky DXT5 one) is replaced by it too, so roads and
    // sidewalks get exactly the same clean light as the ground next to them.
    IDirect3DTexture9* smooth = LightmapSmooth::Find(it->first);
    IDirect3DBaseTexture9* oldLight = nullptr;
    if (smooth) dev->GetTexture(p.road.lightSampler, &oldLight);
    float oldC[4] = {};
    const bool sidewalk = p.road.sidewalkConst >= 0;
    if (sidewalk) dev->GetPixelShaderConstantF(p.road.sidewalkConst, oldC, 1);
    const float c[4] = {g_sidewalkClear.load(std::memory_order_relaxed), 0, 0, 0};
    IDirect3DPixelShader9* original = g_curPs;
    g_inOwnCall = true;
    {
        SamplerBind terrain(dev, p.road.extraSampler, ChunkTexture(it->first, it->second.tex));
        if (smooth) SetTex(dev, p.road.lightSampler, smooth);
        if (sidewalk) SetPsConst(dev, p.road.sidewalkConst, c, 1);
        SetPs(dev, p.ps);
        ConstGain lampGain(dev, p.road.scaleConst, RoadGain()); // the road's lamp scale, after the max with the terrain
        draw();
        SetPs(dev, original);
        if (sidewalk) SetPsConst(dev, p.road.sidewalkConst, oldC, 1);
        if (smooth) SetTex(dev, p.road.lightSampler, oldLight);
    }
    if (oldLight) oldLight->Release();
    g_inOwnCall = false;
    g_roadDrawn.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// ---- Snowy floor tiles (shader_ids.h kFloorVs). The pixel shader lights them only with the lot's own light
// map (x the light basis x 0.25), so street lamps never reach them. Its vertex shader already outputs world xz in
// TEXCOORD0.zw, so the patched shader (ShaderPatches::PatchFloor) reads the world light atlas there
// (lightmap_smooth.cpp): brightest of the lot map and the ground light, like the snowy lot ground. ----
// ---- Fences, railings, posts, stairs (instanced lot structures, ShaderPatches::IsInstancedStructureVs). Their vertex
// shader takes lamp light only from the rig's "vertex light" arrays, which the game fills only with overflow lights
// (usually none: VS c4..c11 = 0 in every fence capture), and one rig serves a whole group from its centre. The patched
// pixel shader (ShaderPatches::PatchInstancedLamps) uses max(vertex lights, ground light atlas at the pixel) instead,
// so every rail gets the same lamp light as the ground next to it. ----
template <typename DrawFn> bool DrawInstanced(IDirect3DDevice9* dev, DrawFn draw) {
    if (!g_fenceFix.load(std::memory_order_relaxed)) return false;
    float c[4];
    IDirect3DTexture9* atlas = LightmapSmooth::Atlas(c);
    if (!atlas) return false;
    PatchedPs& p = PatchedFor(dev, g_fencePs, "Fence/stairs", [](std::vector<DWORD>& t, PatchedPs& pp) { return ShaderPatches::PatchInstancedLamps(t, pp.inst); });
    if (!p.ps) return false;
    float oldA[4] = {}, oldB[4] = {};
    dev->GetPixelShaderConstantF(p.inst.atlasConst, oldA, 1);
    dev->GetPixelShaderConstantF(p.inst.strengthConst, oldB, 1);
    const float s[4] = {g_fenceStrength.load(std::memory_order_relaxed), 0, 0, 0};
    IDirect3DPixelShader9* original = g_curPs;
    g_inOwnCall = true;
    {
        SamplerBind bind(dev, p.inst.atlasSampler, atlas, D3DTEXF_NONE);
        SetPsConst(dev, p.inst.atlasConst, c, 1);
        SetPsConst(dev, p.inst.strengthConst, s, 1);
        SetPs(dev, p.ps);
        draw();
        SetPs(dev, original);
        SetPsConst(dev, p.inst.strengthConst, oldB, 1);
        SetPsConst(dev, p.inst.atlasConst, oldA, 1);
    }
    g_inOwnCall = false;
    g_fenceDrawn.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// ---- Snow lying on objects (fence tops, rails, props; ShaderPatches::IsSnowCoverVs). Its pixel shader lights the snow
// with the moon and the sky only, no lamp at all (LightProbe-neve-cerca, PS_2F27C510). The patched shader
// (ShaderPatches::PatchSnowCover) adds the ground light atlas at the pixel x the fence strength, like the ground does. ----
// ---- Snow with relief on stair tops (ShaderPatches::IsSnowReliefVs, LightProbe-neve-escada, PS_2E036820): same idea,
// patched by ShaderPatches::PatchSnowRelief; its world position arrives halved, so the atlas scale is doubled. ----
template <typename DrawFn, typename PatchFn>
bool DrawSnowOnObject(IDirect3DDevice9* dev, DrawFn draw, std::unordered_map<IDirect3DPixelShader9*, PatchedPs>& cache, const char* what, PatchFn patch, float posScale,
                      std::atomic<int>& drawn) {
    if (!g_fenceFix.load(std::memory_order_relaxed)) return false;
    float c[4];
    IDirect3DTexture9* atlas = LightmapSmooth::Atlas(c);
    if (!atlas) return false;
    c[0] *= posScale;
    c[1] *= posScale;
    PatchedPs& p = PatchedFor(dev, cache, what, patch);
    if (!p.ps) return false;
    float oldA[4] = {}, oldB[4] = {};
    dev->GetPixelShaderConstantF(p.snow.atlasConst, oldA, 1);
    dev->GetPixelShaderConstantF(p.snow.strengthConst, oldB, 1);
    const float s[4] = {g_fenceStrength.load(std::memory_order_relaxed), 0, 0, 0};
    IDirect3DPixelShader9* original = g_curPs;
    g_inOwnCall = true;
    {
        SamplerBind bind(dev, p.snow.atlasSampler, atlas, D3DTEXF_NONE);
        SetPsConst(dev, p.snow.atlasConst, c, 1);
        SetPsConst(dev, p.snow.strengthConst, s, 1);
        SetPs(dev, p.ps);
        draw();
        SetPs(dev, original);
        SetPsConst(dev, p.snow.strengthConst, oldB, 1);
        SetPsConst(dev, p.snow.atlasConst, oldA, 1);
    }
    g_inOwnCall = false;
    drawn.fetch_add(1, std::memory_order_relaxed);
    return true;
}
template <typename DrawFn> bool DrawSnowCover(IDirect3DDevice9* dev, DrawFn draw) {
    return DrawSnowOnObject(dev, draw, g_snowCoverPs, "Snow on objects", [](std::vector<DWORD>& t, PatchedPs& pp) { return ShaderPatches::PatchSnowCover(t, pp.snow); },
                            1.0f, g_snowCoverDrawn);
}
// ---- Outdoor objects lit by a rig (doors, windows, sofas, counters; LightProbe-porta/janela/sofa/balcao). The rig lights
// a whole object (or part) with the 3 strongest lamps at its centre, often from above at a grazing angle, while the walls
// around sum every lamp point by point: a front door looked dark next to its lit wall, and pieces of modular counters
// outside get different light. The patched shaders (ShaderPatches::PatchObjectLampVs / PatchObjectLampPs) use, per pixel,
// max(rig lamps + vertex lights, ground light atlas * (0.5 + 0.5 N.y) * strength). Only when the bound rig is outdoor
// (RigTracker: rig+0x1D4 == 2): indoor objects would pick up ground light from under the house. ----
// The patched copy of the current vertex shader (g_curVs / g_curVsInfo) when it is an object lit by a rig
IDirect3DVertexShader9* ObjectVsFor(IDirect3DDevice9* dev, IDirect3DVertexShader9* vs) {
    if (!g_curVsInfo || g_curVsInfo->cls != 10) return nullptr;
    FoliageVs& f = g_curVsInfo->patched;
    if (!f.tried) {
        f.tried = true;
        if (FAILED(dev->CreateVertexShader(f.code.data(), &f.vs))) f.vs = nullptr;
        LOG_INFO(std::format("[LotLightBridge] Outdoor object: vertex shader {:08X} {}", reinterpret_cast<uintptr_t>(vs), f.vs ? "patched" : "failed"));
    }
    return f.vs;
}

// Light probe detail (Ctrl+Shift+F7) for an object the mod lights: its position, the lamps it got and the game's own
// rig values before they are zeroed, so two neighbouring pieces can be compared. Only while a capture is recording.
std::string g_objDrawInfo;
std::string DescribeObjectDraw(IDirect3DDevice9* dev, const ShaderPatches::ObjectLampPatch& obj, int rigMode, bool pixelLamps, int wk, int vl, const float (*lamps)[4],
                               int nLamps) {
    auto v4 = [](const float* v) { return std::format("({:.4g} {:.4g} {:.4g} {:.4g})", v[0], v[1], v[2], v[3]); };
    std::string s = std::format("OBJECT fixed by the mod | {} | rig mode {} ({}) | per-pixel light {} (strength {:.2f}) | ground light strength {:.2f}",
                                obj.rigLamps ? "rig lamps in the PS (form A/B)" : "no lamps in the PS (form C)", rigMode,
                                rigMode == 2 ? "outdoors" : rigMode == 1 ? "roofless area" : "?", pixelLamps ? "on" : "OFF", g_objPixelLampStrength.load(),
                                g_objPixelStrength.load());
    float w[3][4] = {};
    if (wk >= 0) dev->GetVertexShaderConstantF(static_cast<UINT>(wk), &w[0][0], 3);
    s += std::format("\n      object position (VS c{}..c{} .w) = ({:.2f} {:.2f} {:.2f})", wk, wk + 2, w[0][3], w[1][3], w[2][3]);
    s += std::format("\n      per-pixel lamps: {} used of {} in range", nLamps, g_lastLampCandidates);
    for (int k = 0; k < nLamps; k++) {
        const float *p = lamps[1 + 2 * k], *c = lamps[2 + 2 * k];
        const float r = p[3] > 0 ? 1.0f / std::sqrt(p[3]) : 0.0f, dx = p[0] - w[0][3], dy = p[1] - w[1][3], dz = p[2] - w[2][3];
        s += std::format("\n        [{}] pos ({:.2f} {:.2f} {:.2f}) radius {:.2f} colour ({:.3f} {:.3f} {:.3f}) distance to the object centre {:.2f}", k, p[0], p[1], p[2], r, c[0], c[1],
                         c[2], std::sqrt(dx * dx + dy * dy + dz * dz));
    }
    float pc[14][4] = {};
    dev->GetPixelShaderConstantF(0, &pc[0][0], 14);
    s += "\n      the game's rig in the PS, before zeroing (c1..c3 direction and c5..c7 colour of the 3 lamps; sun in c8/c9, c0/c4 or c12/c13 depending on the variant):";
    for (int i = 0; i < 14; i++) s += std::format(" [{}]{}", i, v4(pc[i]));
    if (vl >= 4) {
        float v[8][4] = {};
        dev->GetVertexShaderConstantF(static_cast<UINT>(vl - 4), &v[0][0], 8);
        s += std::format("\n      luzes de vertice do jogo, antes de zerar (direcoes c{}..c{}, cores c{}..c{}):", vl - 4, vl - 1, vl, vl + 3);
        for (int i = 0; i < 8; i++) s += std::format(" [{}]{}", vl - 4 + i, v4(v[i]));
    } else
        s += "\n      vertex lights: base not found in the shader (not zeroed)";
    return s;
}

template <typename DrawFn> bool DrawObjectLamp(IDirect3DDevice9* dev, DrawFn draw) {
    // rig modes 2 (outdoors) and 1 (roofless fenced areas) both draw with the exterior technique (rig report 25/09)
    const int rigMode = RigTracker::CurrentMode();
    if (!g_objPixel.load(std::memory_order_relaxed) || (rigMode != 2 && rigMode != 1)) return false;
    float c[4];
    IDirect3DTexture9* atlas = LightmapSmooth::Atlas(c);
    if (!atlas) return false;
    PatchedPs& p = PatchedFor(dev, g_objLampPs, "Outdoor object (ground light)", [](std::vector<DWORD>& t, PatchedPs& pp) { return ShaderPatches::PatchObjectLampPs(t, pp.obj); });
    if (!p.ps) return false;
    IDirect3DVertexShader9* vs = ObjectVsFor(dev, g_curVs);
    if (!vs) return false;
    float oldA[4] = {}, oldB[4] = {};
    dev->GetPixelShaderConstantF(p.obj.atlasConst, oldA, 1);
    dev->GetPixelShaderConstantF(p.obj.strengthConst, oldB, 1);
    const float s[4] = {g_objPixelStrength.load(std::memory_order_relaxed), 0, 0, 0};
    // Per-pixel lamps ("Counters" request): the same world lamps for every piece, chosen by the object's position (the
    // VS world triple's translation), so neighbouring pieces of a modular object get the same lamps.
    constexpr unsigned N = ShaderPatches::kObjectPixelLamps;
    float lamps[1 + 2 * N][4] = {};
    float oldLamps[1 + 2 * N][4] = {};
    const bool pixelLamps = g_objPixelLamps.load(std::memory_order_relaxed);
    bool zeroRig = false;
    float oldRig[3][4] = {}, oldVl[4][4] = {};
    const float noRig[4][4] = {};
    const int vl = g_curVsInfo->patched.vertexLight, wk = g_curVsInfo->patched.worldK; // ObjectVsFor succeeded: an object entry
    int nLamps = 0;
    lamps[0][3] = 1e-4f;
    for (unsigned k = 0; k < N; k++) lamps[1 + 2 * k][0] = lamps[1 + 2 * k][2] = 1e6f; // unused slot: far away, colour 0
    if (pixelLamps) {
        float m[3][4];
        if (wk >= 0 && SUCCEEDED(dev->GetVertexShaderConstantF(static_cast<UINT>(wk), &m[0][0], 3))) {
            const int n = SelectLamps(m[0][3], m[2][3], 40.0f);
            nLamps = std::min(n, static_cast<int>(N));
            for (int k = 0; k < n && k < static_cast<int>(N); k++) {
                const float* pr = g_lampData[k];
                const float* col = g_lampData[16 + k];
                const float r = pr[3] > 0.1f ? pr[3] : 0.1f;
                lamps[1 + 2 * k][0] = pr[0];
                lamps[1 + 2 * k][1] = pr[1];
                lamps[1 + 2 * k][2] = pr[2];
                lamps[1 + 2 * k][3] = 1.0f / (r * r);
                lamps[2 + 2 * k][0] = col[0];
                lamps[2 + 2 * k][1] = col[1];
                lamps[2 + 2 * k][2] = col[2];
            }
            // the rig goes: its 3 pixel lamps (PS c5..c7 = 0 below, diffuse and specular) and its 4 vertex lights (the VS
            // colour constants = 0; Phong's ambient term in COLOR0 stays)
            lamps[0][1] = g_objPixelLampStrength.load(std::memory_order_relaxed);
            zeroRig = true;
        }
    }
    if (LightProbe::Capturing()) g_objDrawInfo = DescribeObjectDraw(dev, p.obj, rigMode, zeroRig, wk, vl, lamps, nLamps);
    dev->GetPixelShaderConstantF(p.obj.lampParamConst, &oldLamps[0][0], 1 + 2 * N);
    IDirect3DPixelShader9* originalPs = g_curPs;
    IDirect3DVertexShader9* originalVs = g_curVs;
    g_inOwnCall = true;
    {
        SamplerBind bind(dev, p.obj.atlasSampler, atlas, D3DTEXF_NONE);
        SetPsConst(dev, p.obj.atlasConst, c, 1);
        SetPsConst(dev, p.obj.strengthConst, s, 1);
        SetPsConst(dev, p.obj.lampParamConst, &lamps[0][0], 1 + 2 * N);
        if (zeroRig && p.obj.rigLamps) {
            dev->GetPixelShaderConstantF(5, &oldRig[0][0], 3);
            SetPsConst(dev, 5, &noRig[0][0], 3);
        }
        if (zeroRig && vl >= 0) {
            dev->GetVertexShaderConstantF(static_cast<UINT>(vl), &oldVl[0][0], 4);
            SetVsConst(dev, static_cast<UINT>(vl), &noRig[0][0], 4);
        }
        SetVs(dev, vs);
        SetPs(dev, p.ps);
        draw();
        SetPs(dev, originalPs);
        SetVs(dev, originalVs);
        if (zeroRig && vl >= 0) SetVsConst(dev, static_cast<UINT>(vl), &oldVl[0][0], 4);
        if (zeroRig && p.obj.rigLamps) SetPsConst(dev, 5, &oldRig[0][0], 3);
        SetPsConst(dev, p.obj.lampParamConst, &oldLamps[0][0], 1 + 2 * N);
        SetPsConst(dev, p.obj.strengthConst, oldB, 1);
        SetPsConst(dev, p.obj.atlasConst, oldA, 1);
    }
    g_objDrawInfo.clear();
    g_inOwnCall = false;
    g_objLampDrawn.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// ---- Smooth indoor light ("Even light along walls"). Stairs and other instanced objects read the room's 4 directional
// light maps (1 texel per metre) with the bilinear filter: their 1 m grid shows as bands, and against an outer wall one
// side went dark (the maps' edges are padded by RoomMapPadding). DrawBasisSmooth reads them with a bicubic filter
// instead (ShaderPatches::PatchBasisSmooth). Indoor objects lit by a rig (curtains, furniture: RigTracker mode 0) got
// one light for the whole object, measured at its centre, so two identical neighbours differed a lot; DrawIndoorObject
// gives them the same smooth directional maps per pixel (ShaderPatches::PatchIndoorBasis: the rig's lamps in the diffuse
// are replaced by the basis light, its unlit-room lights kept (diffuseConst, 30/09), its specular kept, its per-object
// vertex lights zeroed), found from the room light map the draw binds; the basis read has its own scale (IndoorBasisScale). ----
std::unordered_map<IDirect3DPixelShader9*, PatchedPs> g_basisSmoothPs;
std::map<int, std::unordered_map<IDirect3DPixelShader9*, PatchedPs>> g_indoorPs; // sampler 0..7, +8 for guarded maps without floor cap
std::atomic<bool> g_indoorSmooth{true};
std::atomic<int> g_basisSmoothDrawn{0}, g_indoorDrawn{0};

// (w, h, 1/w, 1/h) of a bound map, for the smooth reads' size constant; false when it is not a 2D texture
bool MapSize(IDirect3DBaseTexture9* t, float out[4]) {
    D3DSURFACE_DESC d{};
    if (!t || t->GetType() != D3DRTYPE_TEXTURE || FAILED(static_cast<IDirect3DTexture9*>(t)->GetLevelDesc(0, &d)) || !d.Width || !d.Height) return false;
    out[0] = static_cast<float>(d.Width);
    out[1] = static_cast<float>(d.Height);
    out[2] = 1.0f / out[0];
    out[3] = 1.0f / out[1];
    return true;
}

template <typename DrawFn> bool DrawBasisSmooth(IDirect3DDevice9* dev, DrawFn draw) {
    if (!g_indoorSmooth.load(std::memory_order_relaxed)) return false;
    PatchedPs& p = PatchedFor(dev, g_basisSmoothPs, "Indoor light (smooth)", [](std::vector<DWORD>& t, PatchedPs& pp) { return ShaderPatches::PatchBasisSmooth(t, pp.smooth); });
    if (!p.ps) return false;
    float size[4], oldSize[4] = {};
    IDirect3DBaseTexture9* map = nullptr;
    const bool haveSize = SUCCEEDED(dev->GetTexture(p.smooth.sizeSampler, &map)) && MapSize(map, size);
    if (map) map->Release();
    if (!haveSize) return false;
    dev->GetPixelShaderConstantF(p.smooth.sizeConst, oldSize, 1);
    IDirect3DPixelShader9* original = g_curPs;
    g_inOwnCall = true;
    SetPsConst(dev, p.smooth.sizeConst, size, 1);
    SetPs(dev, p.ps);
    draw();
    SetPs(dev, original);
    SetPsConst(dev, p.smooth.sizeConst, oldSize, 1);
    g_inOwnCall = false;
    g_basisSmoothDrawn.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// The basis read's scale (DrawIndoorObject). The patched shader reads the basis maps with the room light map's uv
// (ShaderPatches::PatchIndoorBasis), but the two maps do not cover the same area: the room light map covers the lot's
// power-of-two size at 4 texels per metre (lot CF2DEA20, 30 x 40 m: 128 x 256 = 32 x 64 m, VS uv rows c15 / c16 with
// |xz| = 1/32 and 1/64), while the basis maps always cover 64 x 64 m (the game's basis VS: uv = lot half-metres x 1/128,
// "def c14, 0.0078125 ..." in every captured one). Reading them with texel = uv x 64 sampled x at twice the object's
// position, outside the house plan (alpha 0), so the basis light was exactly 0 and furniture went dark (F7 091, 30/09:
// sofa at lot (13, 32.5) read column 26; floor switches made it come and go, RoomMapPadding). The size constant's .xy
// (uv -> texel; .zw stays texel -> uv) is therefore coverage_m x basis texels / 64, the coverage taken from the VS
// constants that make the uv (1 / |row.xyz|), else from the room light map at 4 texels per metre.
// Development tools (F6 furniture tracer): what the last path-A draw bound
struct TraceA {
    uintptr_t lightMap = 0, basis0 = 0;
    float scaleX = 0, scaleY = 0;
};
TraceA g_traceA;
constexpr float kBasisCoverM = 64.0f;
std::atomic<long> g_indoorUvFromVs{0}, g_indoorUvFallback{0};
bool IndoorBasisScale(IDirect3DDevice9* dev, const ShaderPatches::IndoorBasisPatch& ip, IDirect3DBaseTexture9* lightMap, IDirect3DTexture9* basis0, float out[4]) {
    float b[4], lm[4];
    if (!MapSize(basis0, b) || !MapSize(lightMap, lm)) return false;
    if (b[0] != kBasisCoverM || b[1] != kBasisCoverM) return false; // only 64x64 basis maps were ever captured: others keep the game's shader
    float cover[2] = {lm[0] / 4.0f, lm[1] / 4.0f};
    const int sem = ip.uvUsage >= 0 ? (ip.uvUsage << 4) | ip.uvIndex : -1;
    if (g_curVsInfo && g_curVs && sem >= 0) {
        if (g_curVsInfo->lmSem != sem) {
            g_curVsInfo->lmSem = sem;
            UINT size = 0;
            std::vector<DWORD> code;
            if (SUCCEEDED(g_curVs->GetFunction(nullptr, &size)) && size >= 8 && size <= 65536) {
                code.resize(size / 4);
                if (FAILED(g_curVs->GetFunction(code.data(), &size))) code.clear();
            }
            if (!ShaderPatches::UvRowConsts(code, ip.uvUsage, ip.uvIndex, g_curVsInfo->lmRow[0], g_curVsInfo->lmRow[1])) g_curVsInfo->lmRow[0] = g_curVsInfo->lmRow[1] = -1;
        }
        float rows[2][4];
        bool fromVs = g_curVsInfo->lmRow[0] >= 0 && g_curVsInfo->lmRow[1] >= 0;
        for (int k = 0; k < 2 && fromVs; k++) {
            fromVs = SUCCEEDED(dev->GetVertexShaderConstantF(static_cast<UINT>(g_curVsInfo->lmRow[k]), rows[k], 1));
            const float len = fromVs ? std::sqrt(rows[k][0] * rows[k][0] + rows[k][1] * rows[k][1] + rows[k][2] * rows[k][2]) : 0.0f;
            fromVs = fromVs && len > 1.0f / 1024.0f && len < 1.0f; // 1 m .. 1 km of uv per 1.0
            if (fromVs) rows[k][3] = 1.0f / len;
        }
        if (fromVs) {
            cover[0] = rows[0][3];
            cover[1] = rows[1][3];
        }
        (fromVs ? g_indoorUvFromVs : g_indoorUvFallback).fetch_add(1, std::memory_order_relaxed);
    } else
        g_indoorUvFallback.fetch_add(1, std::memory_order_relaxed);
    out[0] = cover[0] * b[0] / kBasisCoverM;
    out[1] = cover[1] * b[1] / kBasisCoverM;
    out[2] = b[2];
    out[3] = b[3];
    return true;
}

template <typename DrawFn> bool DrawIndoorObject(IDirect3DDevice9* dev, DrawFn draw) {
    if (!g_indoorSmooth.load(std::memory_order_relaxed) || !g_curVsInfo || RigTracker::CurrentMode() != 0) return false;
    IDirect3DTexture9* basis[4] = {};
    IDirect3DBaseTexture9* lightMap = nullptr; // held until the scale is read
    int lmS = -1;
    for (DWORD s = 0; s < 8 && lmS < 0; s++) {
        IDirect3DBaseTexture9* t = nullptr;
        if (FAILED(dev->GetTexture(s, &t)) || !t) continue;
        if (t->GetType() == D3DRTYPE_TEXTURE && RoomMapPadding::BasisFor(static_cast<IDirect3DTexture9*>(t), basis)) {
            lmS = static_cast<int>(s);
            lightMap = t;
        } else
            t->Release();
    }
    if (lmS < 0) return false; // no room light map with known directional maps (other lots, low lighting quality)
    const bool capToFloorMap = !LevelLightShare::BasisFloorGuardReady();
    // Separate cache entries: live disable/re-enable must never reuse the unguarded variant.
    const int shaderVariant = lmS + (capToFloorMap ? 0 : 8);
    PatchedPs& p = PatchedFor(dev, g_indoorPs[shaderVariant], "Indoor object (smooth room light)",
                              [lmS, capToFloorMap](std::vector<DWORD>& t, PatchedPs& pp) {
                                  return ShaderPatches::PatchIndoorBasis(t, static_cast<DWORD>(lmS), pp.indoor, capToFloorMap);
                              });
    float size[4], oldSize[4] = {};
    const bool scaled = p.ps && IndoorBasisScale(dev, p.indoor, lightMap, basis[0], size);
    const uintptr_t lightMapPtr = reinterpret_cast<uintptr_t>(lightMap);
    lightMap->Release();
    if (!scaled) return false;
    // The rig's lights for the diffuse chain (ShaderPatches::PatchIndoorBasis diffuseConst): its unlit-room lights (fill,
    // [NoLight], as the furniture guard has just turned them with Brightness and Blue tint) and 0 for its lamps, which
    // the basis light holds per pixel (so a lamp never counts twice and its light stays smooth across the object). The
    // vertex lights are zeroed as before: the game moves overflow lamps there, and the basis maps hold those too.
    float rig[8][4] = {}, unlit[4][4] = {}, oldUnlit[4][4] = {}, oldVl[4][4] = {}, oldS[4] = {};
    const float zero[4][4] = {}, strength[4] = {1.0f, 0, 0, 0};
    if (SUCCEEDED(dev->GetPixelShaderConstantF(0, &rig[0][0], 8)))
        for (int k = 0; k < 4; k++)
            if (UnlitRooms::IsUnlitLight(rig[4 + k], rig[k])) std::memcpy(unlit[k], rig[4 + k], sizeof unlit[k]);
    const int vl = g_curVsInfo->patched.vertexLight;
    dev->GetPixelShaderConstantF(p.indoor.strengthConst, oldS, 1);
    dev->GetPixelShaderConstantF(p.indoor.sizeConst, oldSize, 1);
    if (p.indoor.diffuseConst >= 0) dev->GetPixelShaderConstantF(static_cast<UINT>(p.indoor.diffuseConst), &oldUnlit[0][0], 4);
    float oldTint[4] = {};
    float cubeColour[3];
    UnlitRooms::FurnitureCubeColour(cubeColour);
    // Rooms at Night: the ambient cube towards grey, then x the room's colour (always set: (1, 1, 1, 1) = as it is)
    const float tint[4] = {UnlitRooms::FurnitureTint(), cubeColour[0], cubeColour[1], cubeColour[2]};
    if (p.indoor.tintConst >= 0) dev->GetPixelShaderConstantF(static_cast<UINT>(p.indoor.tintConst), oldTint, 1);
    IDirect3DPixelShader9* original = g_curPs;
    g_inOwnCall = true;
    {
        SamplerBind b0(dev, p.indoor.firstSampler, basis[0]), b1(dev, p.indoor.firstSampler + 1, basis[1]);
        SamplerBind b2(dev, p.indoor.firstSampler + 2, basis[2]), b3(dev, p.indoor.firstSampler + 3, basis[3]);
        SetPsConst(dev, p.indoor.strengthConst, strength, 1);
        SetPsConst(dev, p.indoor.sizeConst, size, 1);
        if (p.indoor.diffuseConst >= 0) SetPsConst(dev, static_cast<UINT>(p.indoor.diffuseConst), &unlit[0][0], 4);
        if (p.indoor.tintConst >= 0) SetPsConst(dev, static_cast<UINT>(p.indoor.tintConst), tint, 1);
        if (vl >= 0) {
            dev->GetVertexShaderConstantF(static_cast<UINT>(vl), &oldVl[0][0], 4);
            SetVsConst(dev, static_cast<UINT>(vl), &zero[0][0], 4);
        }
        SetPs(dev, p.ps);
        draw();
        SetPs(dev, original);
        if (vl >= 0) SetVsConst(dev, static_cast<UINT>(vl), &oldVl[0][0], 4);
        SetPsConst(dev, p.indoor.strengthConst, oldS, 1);
        SetPsConst(dev, p.indoor.sizeConst, oldSize, 1);
        if (p.indoor.diffuseConst >= 0) SetPsConst(dev, static_cast<UINT>(p.indoor.diffuseConst), &oldUnlit[0][0], 4);
        if (p.indoor.tintConst >= 0) SetPsConst(dev, static_cast<UINT>(p.indoor.tintConst), oldTint, 1);
    }
    g_traceA = TraceA{lightMapPtr, reinterpret_cast<uintptr_t>(basis[0]), size[0], size[1]};
    g_inOwnCall = false;
    g_indoorDrawn.fetch_add(1, std::memory_order_relaxed);
    return true;
}

template <typename DrawFn> bool DrawSnowRelief(IDirect3DDevice9* dev, DrawFn draw) {
    return DrawSnowOnObject(dev, draw, g_snowReliefPs, "Snow with relief (stairs)", [](std::vector<DWORD>& t, PatchedPs& pp) { return ShaderPatches::PatchSnowRelief(t, pp.snow); },
                            2.0f, g_snowReliefDrawn);
}

// Outdoor walls (wall_lamp_table.h): the baked lamp light's scale cK.x times "Forca nas paredes" for this draw.
std::atomic<int> g_wallDrawn{0};
template <typename DrawFn> bool DrawWallGain(IDirect3DDevice9* dev, DrawFn draw) {
    const float gain = g_wallGain.load(std::memory_order_relaxed);
    if (gain == 1.0f) return false;
    auto it = g_wallConst.find(g_curPs);
    if (it == g_wallConst.end()) return false;
    float c[4], old[4];
    if (FAILED(dev->GetPixelShaderConstantF(it->second, old, 1))) return false;
    std::memcpy(c, old, sizeof(c));
    c[0] *= gain;
    g_inOwnCall = true;
    SetPsConst(dev, it->second, c, 1);
    draw();
    SetPsConst(dev, it->second, old, 1);
    g_inOwnCall = false;
    g_wallDrawn.fetch_add(1, std::memory_order_relaxed);
    return true;
}

template <typename DrawFn> bool DrawFloor(IDirect3DDevice9* dev, DrawFn draw) {
    float c[4];
    IDirect3DTexture9* atlas = LightmapSmooth::Atlas(c);
    if (!atlas) return false;
    PatchedPs& p = PatchedFor(dev, g_floorPs, "Floor", [](std::vector<DWORD>& t, PatchedPs& pp) { return ShaderPatches::PatchFloor(t, pp.floor); });
    if (!p.ps) return false;
    float oldC[4] = {};
    dev->GetPixelShaderConstantF(p.floor.atlasConst, oldC, 1);
    IDirect3DPixelShader9* original = g_curPs;
    g_inOwnCall = true;
    {
        SamplerBind bind(dev, p.floor.atlasSampler, atlas, D3DTEXF_NONE);
        SetPsConst(dev, p.floor.atlasConst, c, 1);
        SetPs(dev, p.ps);
        ConstGain lampGain(dev, p.floor.scaleConst, GroundGain()); // the lamp scale of max(map, atlas)
        draw();
        SetPs(dev, original);
        SetPsConst(dev, p.floor.atlasConst, oldC, 1);
    }
    g_inOwnCall = false;
    g_floorDrawn.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// Snow lying on lot floor tiles (LightProbe-m69): same max(room map, atlas) as the floors. Its TEXCOORD7 is world xz / 2,
// so the atlas scale is doubled.
template <typename DrawFn> bool DrawSnowFloor(IDirect3DDevice9* dev, DrawFn draw) {
    float c[4];
    IDirect3DTexture9* atlas = LightmapSmooth::Atlas(c);
    if (!atlas) return false;
    PatchedPs& p = PatchedFor(dev, g_curSnowFloorTc == 0 ? g_snowFloorPs0 : g_snowFloorPs, "Snow on floors", [tc = g_curSnowFloorTc](std::vector<DWORD>& t, PatchedPs& pp) { return ShaderPatches::PatchSnowFloor(t, tc, pp.floor); });
    if (!p.ps) return false;
    // the map it lights with must be a room light map (A8R8G8B8 managed: 32x64 in m69 / m71, 512x128 in m72), not a terrain map (DXT5)
    bool roomMap = false;
    IDirect3DBaseTexture9* map = nullptr;
    if (SUCCEEDED(dev->GetTexture(p.floor.mapSampler, &map)) && map) {
        D3DSURFACE_DESC d{};
        if (map->GetType() == D3DRTYPE_TEXTURE && SUCCEEDED(static_cast<IDirect3DTexture9*>(map)->GetLevelDesc(0, &d)))
            roomMap = d.Format == D3DFMT_A8R8G8B8 && d.Pool != D3DPOOL_DEFAULT && d.Width <= 1024 && d.Height <= 1024;
        map->Release();
    }
    if (!roomMap) return false;
    const float half[4] = {c[0] * 2.0f, c[1] * 2.0f, c[2], c[3]};
    float oldC[4] = {};
    dev->GetPixelShaderConstantF(p.floor.atlasConst, oldC, 1);
    IDirect3DPixelShader9* original = g_curPs;
    g_inOwnCall = true;
    {
        SamplerBind bind(dev, p.floor.atlasSampler, atlas, D3DTEXF_NONE);
        SetPsConst(dev, p.floor.atlasConst, half, 1);
        SetPs(dev, p.ps);
        ConstGain lampGain(dev, p.floor.scaleConst, GroundGain()); // the lamp scale of max(map, atlas)
        draw();
        SetPs(dev, original);
        SetPsConst(dev, p.floor.atlasConst, oldC, 1);
    }
    g_inOwnCall = false;
    g_snowFloorDrawn.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// Outdoor floors lit only by their baked floor map (summer; floor_atlas_table.h): the vertex shader gets a copy that
// also writes world xz to the first free TEXCOORD (7 or up: many floor vertex shaders already use 7), the pixel shader
// a copy with max(floor map, atlas) before the map's scale (ShaderPatches::PatchBakedAtlasPs), as the winter floors.
// The vertex shader copy lives in the shader's VsInfo (FloorVs).
std::map<int, std::unordered_map<IDirect3DPixelShader9*, PatchedPs>> g_floorAtlasPs; // per TEXCOORD index
std::atomic<int> g_floorAtlasDrawn{0};

template <typename DrawFn> bool DrawFloorAtlas(IDirect3DDevice9* dev, DrawFn draw) {
    float c[4];
    IDirect3DTexture9* atlas = LightmapSmooth::Atlas(c);
    if (!atlas || !g_curVs || !g_curVsInfo) return false;
    FloorVs& fv = g_curVsInfo->floor;
    if (!fv.tried) {
        fv.tried = true;
        UINT size = 0;
        if (SUCCEEDED(g_curVs->GetFunction(nullptr, &size)) && size >= 8 && size <= 65536) {
            std::vector<DWORD> t(size / 4);
            int tc = -1;
            if (SUCCEEDED(g_curVs->GetFunction(t.data(), &size)) && ShaderPatches::PatchObjectLampVs(t, false, &tc) && SUCCEEDED(dev->CreateVertexShader(t.data(), &fv.vs)))
                fv.tc = tc;
            else
                fv.vs = nullptr;
        }
        LOG_INFO(std::format("[LotLightBridge] Outdoor floor: vertex shader {:08X} {} (TEXCOORD{})", reinterpret_cast<uintptr_t>(g_curVs), fv.vs ? "patched" : "without the expected pattern", fv.tc));
    }
    if (!fv.vs) return false;
    PatchedPs& p = PatchedFor(dev, g_floorAtlasPs[fv.tc], "Outdoor floor", [tc = fv.tc](std::vector<DWORD>& t, PatchedPs& pp) { return ShaderPatches::PatchBakedAtlasPs(t, tc, pp.floor); });
    if (!p.ps) return false;
    float oldC[4] = {};
    dev->GetPixelShaderConstantF(p.floor.atlasConst, oldC, 1);
    IDirect3DPixelShader9* originalPs = g_curPs;
    IDirect3DVertexShader9* originalVs = g_curVs;
    g_inOwnCall = true;
    {
        SamplerBind bind(dev, p.floor.atlasSampler, atlas, D3DTEXF_NONE);
        SetPsConst(dev, p.floor.atlasConst, c, 1);
        SetVs(dev, fv.vs);
        SetPs(dev, p.ps);
        ConstGain lampGain(dev, p.floor.scaleConst, GroundGain()); // the lamp scale of max(map, atlas)
        draw();
        SetPs(dev, originalPs);
        SetVs(dev, originalVs);
        SetPsConst(dev, p.floor.atlasConst, oldC, 1);
    }
    g_inOwnCall = false;
    g_floorAtlasDrawn.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// ---- Foliage. Trees and bushes (summer and winter) are lit per vertex: sun + 3 lamps per instance, N.L clamped at 0
// ("max r0, r0, cK.w"), so the side of a bush away from a lamp is black. Patched vertex shader (ShaderPatches::
// PatchFoliageVs): lamps get wrap lighting. Winter bushes also multiply the lamp light by the moon shadow (like the
// summer object shader fixed in DrawObjectRig): patched pixel shader (ShaderPatches::PatchLeafShadow) lifts that shadow
// to 1 at night. ----
// The patched copy of the current vertex shader (g_curVs / g_curVsInfo) when it is foliage
IDirect3DVertexShader9* FoliageVsFor(IDirect3DDevice9* dev, IDirect3DVertexShader9* vs) {
    if (!g_curVsInfo || g_curVsInfo->cls != 6) return nullptr;
    FoliageVs& f = g_curVsInfo->patched;
    if (!f.tried) {
        f.tried = true;
        if (FAILED(dev->CreateVertexShader(f.code.data(), &f.vs))) f.vs = nullptr;
        LOG_INFO(std::format("[LotLightBridge] Foliage: vertex shader {:08X} {}", reinterpret_cast<uintptr_t>(vs), f.vs ? "patched" : "failed"));
    }
    return f.vs;
}

template <typename DrawFn> bool DrawLeafShadow(IDirect3DDevice9* dev, DrawFn draw) {
    const float night = g_night.load(std::memory_order_relaxed);
    if (!g_objectFix.load(std::memory_order_relaxed) || night <= 0.01f) return false;
    PatchedPs& p = PatchedFor(dev, g_leafPs, "Foliage (moon shadow)", [](std::vector<DWORD>& t, PatchedPs& pp) { return ShaderPatches::PatchLeafShadow(t, pp.nightConst); });
    if (!p.ps) return false;
    float oldC[4] = {};
    dev->GetPixelShaderConstantF(p.nightConst, oldC, 1);
    const float c[4] = {night, 0, 0, 0};
    IDirect3DPixelShader9* original = g_curPs;
    g_inOwnCall = true;
    SetPsConst(dev, p.nightConst, c, 1);
    SetPs(dev, p.ps);
    draw();
    SetPs(dev, original);
    SetPsConst(dev, p.nightConst, oldC, 1);
    g_inOwnCall = false;
    g_leafDrawn.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// ---- Snowy roofs (shader_ids.h kRoofSnowPs): the winter roof shader has no lamp light and is too big to
// rewrite (snow relief noise). After the game draws it, an additive pass (roof_snow_lamps_ps.hlsl) on the same geometry
// adds the nearby lamps on the same snowy albedo. ----
IDirect3DPixelShader9* g_roofSnowPs = nullptr;
bool g_roofSnowTried = false;

template <typename DrawFn> bool DrawRoofSnow(IDirect3DDevice9* dev, DrawFn draw) {
    if (!g_roofFix.load(std::memory_order_relaxed)) return false;
    if (!g_roofSnowPs && !g_roofSnowTried) {
        g_roofSnowTried = true;
        const std::string err = CompilePs(dev, kRoofSnowPsId, &g_roofSnowPs);
        LOG_INFO("[LotLightBridge] Snowy roofs: " + (err.empty() ? std::string("active") : err));
    }
    if (!g_roofSnowPs) return false;
    float world[12], vs15[4];
    if (FAILED(dev->GetVertexShaderConstantF(8, world, 3)) || FAILED(dev->GetVertexShaderConstantF(15, vs15, 1))) return false;
    g_inOwnCall = true;
    draw(); // the game's roof, unchanged
    const int picked = SelectLamps(world[3], world[11], 80.0f);
    if (picked > 0 && std::fabs(vs15[0]) > 1e-6f) {
        float consts[34][4] = {};
        std::memcpy(consts, g_lampData, sizeof(float) * 4 * 33);
        consts[32][0] = g_roofStrength.load(std::memory_order_relaxed);
        consts[32][1] = static_cast<float>(picked);
        consts[33][0] = vs15[0]; // COLOR0 = world xz / VS c15.x
        float saved[34][4];
        dev->GetPixelShaderConstantF(20, &saved[0][0], 34);
        constexpr D3DRENDERSTATETYPE kRs[8] = {D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND, D3DRS_DESTBLEND, D3DRS_BLENDOP, D3DRS_ZWRITEENABLE, D3DRS_ALPHATESTENABLE, D3DRS_COLORWRITEENABLE, D3DRS_SEPARATEALPHABLENDENABLE};
        const DWORD set[8] = {TRUE, D3DBLEND_ONE, D3DBLEND_ONE, D3DBLENDOP_ADD, FALSE, FALSE, 0x7, FALSE};
        DWORD rs[8];
        for (int i = 0; i < 8; i++) dev->GetRenderState(kRs[i], &rs[i]);
        for (int i = 0; i < 8; i++) dev->SetRenderState(kRs[i], set[i]);
        IDirect3DPixelShader9* original = g_curPs;
        SetPs(dev, g_roofSnowPs);
        SetPsConst(dev, 20, &consts[0][0], 34);
        draw();
        SetPsConst(dev, 20, &saved[0][0], 34);
        SetPs(dev, original);
        for (int i = 7; i >= 0; i--) dev->SetRenderState(kRs[i], rs[i]);
        g_roofSnowDrawn.fetch_add(1, std::memory_order_relaxed);
    }
    g_inOwnCall = false;
    return true;
}

// Draws an outdoor object with the moon-shadow-free shader (c3.x = night level), then restores the game state.
template <typename DrawFn> bool DrawObjectRig(IDirect3DDevice9* dev, DrawFn draw) {
    const float night = g_night.load(std::memory_order_relaxed);
    if (!g_objectFix.load(std::memory_order_relaxed) || night <= 0.01f) return false;
    EnsureObjectReplacement(dev);
    if (!g_objectPs) return false;
    float oldC3[4] = {};
    dev->GetPixelShaderConstantF(3, oldC3, 1);
    const float c3[4] = {night, 0, 0, 0};
    IDirect3DPixelShader9* original = g_curPs;
    g_inOwnCall = true;
    SetPs(dev, g_objectPs);
    SetPsConst(dev, 3, c3, 1);
    draw();
    SetPsConst(dev, 3, oldC3, 1);
    SetPs(dev, original);
    g_inOwnCall = false;
    g_objectDrawn.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// Returns the sampler that holds the chunk light map (0 = not a terrain chunk draw), the chunk key and its g_chunks entry
// (std::map: the reference stays valid; the caller no longer looks the key up a second time).
DWORD RecordWorldChunk(IDirect3DDevice9* dev, std::pair<int, int>& key, ChunkTex*& chunk, UINT mapConst = 15) {
    float c[16];
    if (FAILED(dev->GetVertexShaderConstantF(8, c, 3))) return 0; // c8..c10 = world matrix rows
    float m[4];
    if (FAILED(dev->GetVertexShaderConstantF(mapConst, m, 1)) || !Near(m[0], 1.0f / 256.0f) || !Near(m[1], 1.0f / 256.0f) || !Near(m[2], 0.5f) || !Near(m[3], 0.5f)) return 0;
    // The terrain light map is the 256x256 texture with few mips: the world file's map and a rebuilt one are both DXT5 with
    // 4 mips (the rebuild reads the render target back and CPU-encodes 4 mips into the same texture, 0x00C29AB6 ->
    // 0x00618CD0; research\perf2\chunkrelight.md 1.3, 512 DXT calls = 128 textures x 4 mips). Which sampler holds it
    // depends on the shader variant (s8 with 4 paint layers, s7 with 3, ...). The normal map is also 256x256 but has 9
    // mips and format Q8W8V8U8, the paint layers are 1024x1024.
    // Only the samplers this pixel shader declares: the game binds all of them for this draw, while the others may still
    // hold textures of earlier draws (another chunk's map). Unknown shader (not classified yet): every sampler, as before.
    const auto declared = g_worldSamplers.find(g_curPs);
    const uint16_t mask = declared != g_worldSamplers.end() ? declared->second : 0xFFFE;
    const std::pair<int, int> here = Key(c[3], c[11]);
    IDirect3DBaseTexture9* t = nullptr;
    DWORD sampler = 0;
    for (DWORD s = 15; s >= 1 && !t; s--) {
        if (!(mask & (1u << s))) continue;
        IDirect3DBaseTexture9* cand = nullptr;
        if (FAILED(dev->GetTexture(s, &cand)) || !cand) continue;
        bool ok = cand->GetType() == D3DRTYPE_TEXTURE && cand->GetLevelCount() <= 5;
        if (ok) {
            D3DSURFACE_DESC d{};
            ok = SUCCEEDED(static_cast<IDirect3DTexture9*>(cand)->GetLevelDesc(0, &d)) && d.Width == 256 && d.Height == 256 && d.Format != D3DFMT_Q8W8V8U8;
        }
        // Already the map of another chunk (g_chunks holds a reference, so its address cannot belong to a new texture):
        // a leftover, not this chunk's map
        if (ok) {
            const auto owner = g_chunkOfTexture.find(cand);
            if (owner != g_chunkOfTexture.end() && owner->second != here) {
                ok = false;
                g_chunkStraySkipped.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (ok) {
            t = cand;
            sampler = s;
        }
        else cand->Release();
    }
    if (!t) return 0;
    key = here;
    auto& slot = g_chunks[key];
    if (slot.tex != t) {
        if (slot.tex) {
            g_chunkOfTexture.erase(slot.tex);
            slot.tex->Release();
        }
        slot.tex = t; // keep the reference from GetTexture
        g_chunkOfTexture[t] = key;
    } else
        t->Release();
    chunk = &slot;
    g_worldSeen.fetch_add(1, std::memory_order_relaxed);
    return sampler;
}

// "Ground brightness" on the world terrain chunks: the constant that scales the chunk light map in this pixel shader,
// found by pattern (ShaderPatches::LightMapScaleConst: c7 in the captured summer and winter chunks, but other variants
// read the map from other samplers), -1 when there is none that scales the map alone (that shader keeps the game's
// brightness). Cached per shader (shaders are pinned); render thread only.
std::unordered_map<IDirect3DPixelShader9*, std::pair<DWORD, int>> g_terrainLampConst; // PS -> (light map sampler, K)
int TerrainLampConst(IDirect3DPixelShader9* ps, DWORD sampler) {
    auto it = g_terrainLampConst.find(ps);
    if (it != g_terrainLampConst.end() && it->second.first == sampler) return it->second.second;
    const int k = ShaderPatches::LightMapScaleConst(ShaderCode(ps), sampler);
    g_terrainLampConst[ps] = {sampler, k};
    LOG_INFO(std::format("[LotLightBridge] Ground brightness: terrain shader {:08X}, light map s{} -> {}", reinterpret_cast<uintptr_t>(ps), sampler,
                         k >= 0 ? std::format("c{}.x", k) : std::string("no lamp-only scale found, left as the game")));
    return k;
}

template <typename DrawFn> D3D9Hooks::HookAction OnDrawInnerCore(IDirect3DDevice9* dev, DrawFn draw) {
    constexpr auto kSkip = D3D9Hooks::HookAction::Skip;
    constexpr auto kContinue = D3D9Hooks::HookAction::Continue;
    if (g_curPsBasis) RoomMapPadding::NoteDraw(dev, g_curPs); // note the room light maps it binds (edge padding)
    // (not for fences / snow relief / rig objects: those draws keep their own fixes)
    if (g_curPsBasis && !g_curVsIsInstanced && !g_curVsIsSnowRelief && !g_curVsIsObject && DrawBasisSmooth(dev, draw)) return kSkip;
    if (g_curClass == PsClass::ObjectRig) return DrawObjectRig(dev, draw) ? kSkip : kContinue;
    if (g_curClass == PsClass::Roof) return DrawRoof(dev, draw) ? kSkip : kContinue;
    // The snowy roof pixel shader is also the one of snow on stair tops (same bytes, LightProbe-neve-escada); the vertex
    // shader tells them apart (the stair one takes the snow base in TEXCOORD2) and stairs go to DrawSnowRelief below.
    if (g_curClass == PsClass::RoofSnow && !g_curVsIsSnowRelief) return DrawRoofSnow(dev, draw) ? kSkip : kContinue;
    if (g_curClass == PsClass::Lake) return DrawLake(dev, draw) ? kSkip : kContinue;
    if (g_curVsIsFoliage) return DrawLeafShadow(dev, draw) ? kSkip : kContinue;
    if (g_curClass == PsClass::WallGain) return DrawWallGain(dev, draw) ? kSkip : kContinue;
    if (!g_enabled.load(std::memory_order_relaxed)) return kContinue;
    if (g_curVsIsRoad) return DrawRoad(dev, draw) ? kSkip : kContinue;
    if (g_curVsIsFloor) return DrawFloor(dev, draw) ? kSkip : kContinue;
    if (g_curClass == PsClass::FloorAtlas && !g_curVsIsSnowFloor) return DrawFloorAtlas(dev, draw) ? kSkip : kContinue;
    if (g_curVsIsInstanced) return DrawInstanced(dev, draw) ? kSkip : kContinue;
    if (g_curVsIsSnowCover) return DrawSnowCover(dev, draw) ? kSkip : kContinue;
    if (g_curVsIsSnowRelief) return DrawSnowRelief(dev, draw) ? kSkip : kContinue;
    // Class 10 also holds roof and snow vertex shaders: when the object patch does not apply, fall through to the rest.
    if (g_curVsIsObject && DrawIndoorObject(dev, draw)) return kSkip;
    if (g_curVsIsObject && DrawObjectLamp(dev, draw)) return kSkip;
    if (g_curClass == PsClass::LotLightSnow) return DrawLotSnow(dev, draw) ? kSkip : kContinue;
    if (g_curClass == PsClass::WorldCandidate || g_curClass == PsClass::WorldMultiLight || g_curClass == PsClass::WorldCompact) {
        const bool multi = g_curClass == PsClass::WorldMultiLight;
        if (multi && (!g_curVsInfo || !g_curVsInfo->worldMultiLight)) return kContinue;
        if (g_curClass == PsClass::WorldCompact && (!g_curVsInfo || !g_curVsInfo->worldCompact)) return kContinue;
        std::pair<int, int> key;
        ChunkTex* chunk = nullptr;
        const DWORD s = RecordWorldChunk(dev, key, chunk, multi ? 13 : 15);
        if (!s) {
            // not a world terrain chunk: the snow-on-floor pixel shaders (m69, m71) also declare s6+ and land here
            if (g_curVsIsSnowFloor && DrawSnowFloor(dev, draw)) return kSkip;
            return D3D9Hooks::HookAction::Continue;
        }
        IDirect3DTexture9* smooth = LightmapSmooth::Get(key, static_cast<IDirect3DTexture9*>(chunk->tex));
        // "Ground brightness": the chunk map's lamp scale times the gain, with or without the smoothed map
        const float gain = GroundGain();
        const int k = gain != 1.0f ? (multi ? 3 : TerrainLampConst(g_curPs, s)) : -1;
        if (!smooth && k < 0) return D3D9Hooks::HookAction::Continue;
        IDirect3DBaseTexture9* old = nullptr;
        if (smooth) dev->GetTexture(s, &old);
        if (LightProbe::Capturing())
            g_objDrawInfo = std::format("mod draw: world terrain | compact {} | multi-pass {} | lamp sampler s{} | gain {:.3f} | smoothed {}",
                                       g_curClass == PsClass::WorldCompact, multi, s, gain, smooth != nullptr);
        g_inOwnCall = true;
        {
            // Captured multi-pass PS squares c3.x before multiplying lamp RGB.
            ConstGain lampGain(dev, k, multi ? std::sqrt(gain) : gain);
            if (smooth) SetTex(dev, s, smooth);
            draw();
            if (smooth) SetTex(dev, s, old);
        }
        g_inOwnCall = false;
        g_objDrawInfo.clear();
        if (old) old->Release();
        return D3D9Hooks::HookAction::Skip;
    }
    if (g_curClass != PsClass::LotLight) {
        // snow lying on floor tiles (m69, m71): only draws no pixel-shader class claimed, see ClassifyVs
        if (g_curVsIsSnowFloor && DrawSnowFloor(dev, draw)) return kSkip;
        return kContinue;
    }

    EnsureReplacement(dev);
    if (!g_replacementPs) return D3D9Hooks::HookAction::Continue;
    float v[8];
    if (FAILED(dev->GetVertexShaderConstantF(14, v, 2)) || !Near(v[0], 1.0f / 256.0f) || !Near(v[1], 1.0f / 256.0f)) return D3D9Hooks::HookAction::Continue;
    // World atlas when ready, as in DrawLotSnow (a lot can reach past its home chunk). The summer lot VS computes the
    // light map uv as (world.xz - c15.xz) * c14.xy + c14.zw; c15 also feeds another uv (c13), so only c14 changes.
    float atlasC[4];
    IDirect3DTexture9* atlas = LightmapSmooth::Atlas(atlasC);
    IDirect3DBaseTexture9* terrain = atlas;
    if (!atlas) {
        auto it = g_chunks.find(Key(v[4], v[6])); // c15.xz = chunk center
        if (it == g_chunks.end() || !it->second.tex) {
            g_lotMissing.fetch_add(1, std::memory_order_relaxed);
            return D3D9Hooks::HookAction::Continue;
        }
        terrain = ChunkTexture(it->first, it->second.tex);
    }
    const float atlasMap[4] = {atlasC[0], atlasC[1], atlasC[2] + v[4] * atlasC[0], atlasC[3] + v[6] * atlasC[1]};

    // Soft lot edges: PS c28..c30 (see kReplacementHlsl). Without a known lot rectangle the pass is the plain max().
    float edge[12];
    float lotM[12] = {};
    const LotRect* rect = nullptr;
    if (SUCCEEDED(dev->GetVertexShaderConstantF(8, lotM, 3))) {
        rect = FindLotRect(&lotM[0], &lotM[8]);
        if (!rect) g_lotRectMiss = true;
        else NoteLotDraw((static_cast<uint64_t>(rect->lotHi) << 32) | rect->lotLo);
    }
    const bool feather = LotEdgeConstants(atlas ? atlasMap : v, &v[4], &lotM[0], &lotM[8], rect, edge);
    if (feather) {
        g_edgeMatched.fetch_add(1, std::memory_order_relaxed);
        g_lastEdgeRect = *rect;
        g_haveLastEdgeRect = true;
    } else if (g_softEdges.load(std::memory_order_relaxed))
        g_edgeUnmatched.fetch_add(1, std::memory_order_relaxed);
    if (LightProbe::Capturing())
        g_objDrawInfo = feather ? std::format("mod draw: lot light pass | soft edges: lot {:08X}{:08X}, {:.0f} x {:.0f} m, origin ({:.2f}, {:.2f}), band {:.1f} m (PS c28..c30)",
                                              rect->lotHi, rect->lotLo, rect->w, rect->d, rect->tx, rect->tz, kEdgeBand)
                                : std::string("mod draw: lot light pass | soft edges: ") + (g_softEdges.load() ? "NOT applied, lot rectangle not found" : "off (option)");
    float savedEdge[16]; // c28..c31
    dev->GetPixelShaderConstantF(28, savedEdge, 4);
    const float lotGain[4] = {LotMapGain(), 0, 0, 0};

    IDirect3DPixelShader9* original = g_curPs;
    g_inOwnCall = true;
    {
        // s2 = the terrain light (clamp, linear, linear mips, no sRGB): only the states that differ are set and restored
        SamplerBind terrainMap(dev, 2, terrain);
        SetPs(dev, g_replacementPs);
        if (atlas) SetVsConst(dev, 14, atlasMap, 1);
        SetPsConst(dev, 28, edge, 3);
        SetPsConst(dev, 31, lotGain, 1);
        ConstGain lampGain(dev, 3, GroundGain()); // c3.x scales only the lamp term of kReplacementHlsl
        draw();
        if (atlas) SetVsConst(dev, 14, v, 1);
        SetPsConst(dev, 28, savedEdge, 4);
    }
    g_objDrawInfo.clear();
    SetPs(dev, original);
    g_inOwnCall = false;
    g_lotDrawn.fetch_add(1, std::memory_order_relaxed);
    return D3D9Hooks::HookAction::Skip;
}

// Foliage vertex shaders are swapped around everything else (the pixel side may be patched too): set the patched VS,
// let the pixel-shader handling draw (or draw here), restore the game's VS.
// The game sets the same shader again and again (every draw of a batch): an unchanged pointer keeps what was derived from
// it (2026-09-29). Exact: the class caches only grow while the hooks are registered, and Shutdown, which empties them,
// also resets g_curPs / g_curVs and sets g_stateUnknown, whose path (OnDrawTracked) calls these with force = true before
// the next draw uses anything.
void TrackPs(IDirect3DPixelShader9* ps, bool force = false) {
    if (ps == g_curPs && !force) return;
    g_curPs = ps;
    g_curClass = Classify(ps);
    g_curPsBasis = ps && g_basisPs.count(ps);
}

void TrackVs(IDirect3DVertexShader9* vs, bool force = false) {
    if (vs == g_curVs && !force) return;
    g_curVs = vs;
    VsInfo* info = ClassifyVs(vs);
    g_curVsInfo = info;
    const uint8_t cls = info ? info->cls : 0;
    g_curVsIsRoof = cls == 1;
    g_curVsIsLake = cls == 2;
    g_curVsIsSnowLot = cls == 3;
    g_curVsIsRoad = cls == 4;
    if (g_curVsIsRoad) g_curRoadMap = info->roadMap;
    g_curVsIsFloor = cls == 5;
    g_curVsIsFoliage = cls == 6;
    g_curVsIsInstanced = cls == 7;
    g_curVsIsSnowCover = cls == 8;
    g_curVsIsSnowRelief = cls == 9;
    g_curVsIsObject = cls == 10;
    g_curVsIsSnowFloor = cls == 11;
    if (g_curVsIsSnowFloor) g_curSnowFloorTc = info->snowFloorTc;
}

// Rooms at Night on furniture (unlit_rooms.h, user 30/09: "objects that respond to nothing, mostly on the upper floor"): an
// indoor object (room-mode rig, RigTracker mode 0) is lit by its rig's four lights plus the ambient cube. Room-mode rigs have no
// sun (FUN_006bbde0: start slot = (mode == 1)): in an unlit room the lights are the game's three [NoLight] lights and the fill
// light, the only blue on furniture (the cube, CASDiffuseProbe, is flat grey; second multi-agent study, 30/09). While it
// draws, through the game's shader or Apex's, those lights (PS c4..c7 of shaders with that chain, recognised by direction /
// fill w, UnlitRooms::IsUnlitLight; dim bluish vertex lights) and the cube's weight get the room's Brightness and Blue tint;
// lamps keep their colour and strength. The draw is then made here and every
// constant put back. The game's own shader draws with a copy whose ambient cube colour is pulled towards its grey by the
// Blue tint too (PatchCubeTint; user 30/09: "the only thing missing on the furniture that works is the blue tint"), as
// Apex's indoor-object shader does.
ShaderLookupCache<const ShaderPatches::RigPsInfo*> g_rigLookup;
std::unordered_map<IDirect3DPixelShader9*, ShaderPatches::RigPsInfo> g_rigPsInfo;
std::unordered_map<IDirect3DPixelShader9*, PatchedPs> g_cubeTintPs;
std::atomic<long> g_nightFurniture{0}, g_nightFurnitureTinted{0};
const ShaderPatches::RigPsInfo& RigPsInfoFor(IDirect3DPixelShader9* ps) {
    const auto key = reinterpret_cast<std::uintptr_t>(ps);
    const ShaderPatches::RigPsInfo* cached = nullptr;
    if (g_rigLookup.Find(key, cached)) return *cached;
    auto [it, fresh] = g_rigPsInfo.try_emplace(ps);
    if (fresh) ShaderPatches::AnalyzeRigPs(ShaderCode(ps), it->second);
    g_rigLookup.Store(key, &it->second);
    return it->second;
}
// Development build: what happened to room-mode furniture draws (for the F6 recorder, FurnitureDiag)
std::atomic<long> g_fdMode0{0}, g_fdInactive{0}, g_fdNoChain{0}, g_fdUnlitSlots{0}, g_fdLampSlots{0};
// The furniture draw with the rig's unlit-room lights, the vertex lights and the cube weight turned (rig = PS c0..c7 or
// null when the shader has no rig chain)
template <typename DrawFn> D3D9Hooks::HookAction OnDrawFurniture(IDirect3DDevice9* dev, DrawFn draw, const ShaderPatches::RigPsInfo& info, const float (*rig)[4]) {
    float psOld[4][4] = {}, vsOld[4][4] = {}, cubeOld[4] = {}, t[4][4], cube[4];
    bool psSet = false, vsSet = false, cubeSet = false;
    // PS c0..c3 = the slots' directions, c4..c7 their colours (each slot tested with its direction: fill and [NoLight]
    // lights are turned, lamps are not; room-mode rigs have no sun, slot 0 is the strongest room light)
    if (rig) {
        std::memcpy(psOld, rig[4], sizeof psOld);
        std::memcpy(t, psOld, sizeof t);
        for (int k = 0; k < 4; k++) {
            const bool turned = UnlitRooms::FurnitureColour(t[k], rig[k]);
            psSet |= turned;
            if (!kPublicBuild)
                if (psOld[k][0] + psOld[k][1] + psOld[k][2] > 1e-6f) (turned ? g_fdUnlitSlots : g_fdLampSlots).fetch_add(1, std::memory_order_relaxed);
        }
        if (psSet) SetPsConst(dev, 4, &t[0][0], 4);
    }
    // the vertex lights with their directions (PatchObjectLampVs: direction c(vl-4+k) with colour c(vl+k)); the fill moves
    // a [NoLight] light there when it takes slot 1 (FUN_006b7e70)
    const int vl = g_curVsInfo ? g_curVsInfo->patched.vertexLight : -1;
    float vsDir[4][4] = {};
    if (vl >= 4 && SUCCEEDED(dev->GetVertexShaderConstantF(static_cast<UINT>(vl - 4), &vsDir[0][0], 4)) &&
        SUCCEEDED(dev->GetVertexShaderConstantF(static_cast<UINT>(vl), &vsOld[0][0], 4))) {
        std::memcpy(t, vsOld, sizeof t);
        for (int k = 0; k < 4; k++) vsSet |= UnlitRooms::FurnitureColour(t[k], vsDir[k]);
        if (vsSet) SetVsConst(dev, static_cast<UINT>(vl), &t[0][0], 4);
    }
    if (info.cubeWeightConst >= 0 && SUCCEEDED(dev->GetPixelShaderConstantF(static_cast<UINT>(info.cubeWeightConst), cubeOld, 1))) {
        std::memcpy(cube, cubeOld, sizeof cube);
        cube[3] *= UnlitRooms::FurnitureAmbient();
        cubeSet = true;
        SetPsConst(dev, static_cast<UINT>(info.cubeWeightConst), cube, 1);
    }
    D3D9Hooks::HookAction r = OnDrawInnerCore(dev, draw);
    if (psSet || vsSet || cubeSet) {
        if (r == D3D9Hooks::HookAction::Continue) { // the game's own draw, made here while the constants are turned
            const float tint = UnlitRooms::FurnitureTint();
            float cubeColour[3];
            UnlitRooms::FurnitureCubeColour(cubeColour);
            const bool coloured = std::fabs(cubeColour[0] - 1.0f) > 1e-3f || std::fabs(cubeColour[1] - 1.0f) > 1e-3f || std::fabs(cubeColour[2] - 1.0f) > 1e-3f;
            PatchedPs* tinted = nullptr;
            if (cubeSet && (std::fabs(tint - 1.0f) > 1e-3f || coloured)) {
                PatchedPs& p = PatchedFor(dev, g_cubeTintPs, "Indoor object (blue tint)", [](std::vector<DWORD>& t, PatchedPs& pp) { return ShaderPatches::PatchCubeTint(t, pp.cubeTint); });
                if (p.ps && p.cubeTint >= 0) tinted = &p;
            }
            IDirect3DPixelShader9* original = g_curPs;
            float tintOld[4] = {};
            g_inOwnCall = true;
            if (tinted) {
                const float c[4] = {tint, cubeColour[0], cubeColour[1], cubeColour[2]};
                dev->GetPixelShaderConstantF(static_cast<UINT>(tinted->cubeTint), tintOld, 1);
                SetPsConst(dev, static_cast<UINT>(tinted->cubeTint), c, 1);
                SetPs(dev, tinted->ps);
            }
            draw();
            if (tinted) {
                SetPs(dev, original);
                SetPsConst(dev, static_cast<UINT>(tinted->cubeTint), tintOld, 1);
                g_nightFurnitureTinted.fetch_add(1, std::memory_order_relaxed);
            }
            g_inOwnCall = false;
            r = D3D9Hooks::HookAction::Skip;
        }
        if (psSet) SetPsConst(dev, 4, &psOld[0][0], 4);
        if (vsSet) SetVsConst(dev, static_cast<UINT>(vl), &vsOld[0][0], 4);
        if (cubeSet) SetPsConst(dev, static_cast<UINT>(info.cubeWeightConst), cubeOld, 1);
        g_nightFurniture.fetch_add(1, std::memory_order_relaxed);
    }
    return r;
}

// ---- Development tools: the F6 furniture tracer. While a recording runs, every room-mode object part (world position +
// pixel shader) is written once at its first draw and again whenever its drawing changes: the path, the rig lights as the
// game set them, the vertex lights, the ambient cube weight, the blue kept, and for path A the maps it read. ----
std::unordered_map<uint64_t, std::vector<uint64_t>> g_traceLast; // object part (rig + pixel shader) -> the states already written (render thread)
uint64_t TraceHash(uint64_t h, int64_t v) { return (h ^ static_cast<uint64_t>(v)) * 1099511628211ull; }
int64_t TraceQ(float v) { return std::isfinite(v) ? static_cast<int64_t>(std::llround(static_cast<double>(v) * 10000.0)) : 0x7FFFFFFF; }
char SlotKind(const float* colour, const float* dir) {
    if (colour[0] + colour[1] + colour[2] <= 1e-6f && colour[3] <= 1e-6f) return '-';
    if (colour[3] > 1e-6f) return 'F';
    return UnlitRooms::IsUnlitLight(colour, dir) ? 'N' : 'L';
}
void TraceFurniture(IDirect3DDevice9* dev, const float (*rigIn)[4], bool rigChain, int path, bool dark, bool active, float tint, float cubeGame, float cubeDrawn) {
    const uintptr_t rigPtr = RigTracker::CurrentRig();
    float w[3][4] = {}, rig[8][4] = {}, vlc[4][4] = {};
    const int wk = g_curVsInfo ? g_curVsInfo->patched.worldK : -1;
    if (wk >= 0) dev->GetVertexShaderConstantF(static_cast<UINT>(wk), &w[0][0], 3);
    const float pos[3] = {w[0][3], w[1][3], w[2][3]};
    if (rigIn) std::memcpy(rig, rigIn, sizeof rig);
    else dev->GetPixelShaderConstantF(0, &rig[0][0], 8);
    const int vl = g_curVsInfo ? g_curVsInfo->patched.vertexLight : -1;
    float vlSum = 0.0f;
    if (vl >= 0 && SUCCEEDED(dev->GetVertexShaderConstantF(static_cast<UINT>(vl), &vlc[0][0], 4)))
        for (const auto& c : vlc) vlSum += c[0] + c[1] + c[2];
    // the object: its rig (one per object; two objects may stand at the same position) and the part's pixel shader
    uint64_t key = 1469598103934665603ull;
    if (rigPtr) key = TraceHash(key, static_cast<int64_t>(rigPtr));
    else
        for (float p : pos) key = TraceHash(key, static_cast<int64_t>(std::llround(p * 100.0f)));
    key = TraceHash(key, static_cast<int64_t>(reinterpret_cast<uintptr_t>(g_curPs)));
    uint64_t state = 1469598103934665603ull;
    for (int64_t v : {static_cast<int64_t>(path), static_cast<int64_t>(dark), static_cast<int64_t>(active), TraceQ(tint), TraceQ(cubeGame), TraceQ(cubeDrawn), TraceQ(vlSum),
                      static_cast<int64_t>(reinterpret_cast<uintptr_t>(g_curVs))})
        state = TraceHash(state, v);
    for (int k = 0; k < 4; k++)
        for (int c = 0; c < 4; c++) state = TraceHash(state, TraceQ(rig[4 + k][c]));
    if (path == 1) {
        state = TraceHash(state, static_cast<int64_t>(g_traceA.lightMap));
        state = TraceHash(state, static_cast<int64_t>(g_traceA.basis0));
        state = TraceHash(state, TraceQ(g_traceA.scaleX));
        state = TraceHash(state, TraceQ(g_traceA.scaleY));
    }
    if (g_traceLast.size() > 50000) g_traceLast.clear();
    // a state already written for this part is not written again (a part drawn twice a frame, e.g. by two passes with
    // different maps, would otherwise alternate every frame)
    auto [it, fresh] = g_traceLast.try_emplace(key);
    auto& seen = it->second;
    if (std::find(seen.begin(), seen.end(), state) != seen.end()) return;
    if (seen.size() >= 8) seen.erase(seen.begin());
    seen.push_back(state);
    std::string rigText = rigChain ? std::string() : std::string(" (this shader has no rig light chain: c4..c7 are other values)");
    for (int k = 0; k < 4 && rigChain; k++)
        rigText += std::format(" {}({:.3f} {:.3f} {:.3f})", SlotKind(rig[4 + k], rig[k]), rig[4 + k][0], rig[4 + k][1], rig[4 + k][2]);
    std::string text = std::format("[furniture] {} rig {:08X} ({:.2f} {:.2f} {:.2f}) PS {:08X} VS {:08X} | path {} | dark {} acting {} | rig{} | vertex lights {:.3f} | cube {:.3f} -> {:.3f} | blue kept {:.3f}",
                                   fresh ? "new" : "changed", rigPtr, pos[0], pos[1], pos[2], reinterpret_cast<uintptr_t>(g_curPs), reinterpret_cast<uintptr_t>(g_curVs),
                                   path == 1 ? "A" : path == 2 ? "B" : "game", dark ? 1 : 0, active ? 1 : 0, rigText, vlSum, cubeGame, cubeDrawn, tint);
    if (path == 1)
        text += std::format(" | A: light map {:08X}, directional map {:08X}, read scale ({:.2f}, {:.2f})", g_traceA.lightMap, g_traceA.basis0, g_traceA.scaleX, g_traceA.scaleY);
    Recorder::Note(text);
}

// Room-mode furniture draws (RigTracker mode 0). The room counts as dark (Rooms at Night acts fully, whatever the night
// level) when the rig holds a [NoLight] light: the game adds those only to a dark room.
template <typename DrawFn> D3D9Hooks::HookAction OnDrawInner(IDirect3DDevice9* dev, DrawFn draw) {
    if (!g_curVsIsObject || !g_curPs || g_inOwnCall) return OnDrawInnerCore(dev, draw);
    if (RigTracker::CurrentMode() != 0) return OnDrawInnerCore(dev, draw);
    if (!kPublicBuild) g_fdMode0.fetch_add(1, std::memory_order_relaxed);
    const ShaderPatches::RigPsInfo& info = RigPsInfoFor(g_curPs);
    if (!kPublicBuild)
        if (!info.rigLights) g_fdNoChain.fetch_add(1, std::memory_order_relaxed);
    float rig[8][4] = {};
    const bool haveRig = info.rigLights && SUCCEEDED(dev->GetPixelShaderConstantF(0, &rig[0][0], 8));
    bool dark = false;
    for (int k = 0; k < 4 && haveRig && !dark; k++) dark = UnlitRooms::IsDarkRoomLight(rig[4 + k], rig[k]);
    UnlitRooms::SetDrawDark(dark);
    // the F6 furniture tracer (development build, only while a recording runs)
    const bool trace = Recorder::Active();
    const long aBefore = trace ? static_cast<long>(g_indoorDrawn.load()) : 0, bBefore = trace ? g_nightFurniture.load() : 0;
    float cubeGame = -1.0f;
    if (trace && info.cubeWeightConst >= 0) {
        float c[4] = {};
        if (SUCCEEDED(dev->GetPixelShaderConstantF(static_cast<UINT>(info.cubeWeightConst), c, 1))) cubeGame = c[3];
    }
    const bool active = UnlitRooms::FurnitureActive();
    const float tint = active ? UnlitRooms::FurnitureTint() : 1.0f, ambient = active ? UnlitRooms::FurnitureAmbient() : 1.0f;
    D3D9Hooks::HookAction r;
    if (active) r = OnDrawFurniture(dev, draw, info, haveRig ? rig : nullptr);
    else {
        if (!kPublicBuild) g_fdInactive.fetch_add(1, std::memory_order_relaxed);
        r = OnDrawInnerCore(dev, draw);
    }
    if (trace) {
        const int path = static_cast<long>(g_indoorDrawn.load()) != aBefore ? 1 : g_nightFurniture.load() != bBefore ? 2 : 0;
        TraceFurniture(dev, haveRig ? rig : nullptr, info.rigLights, path, dark, active, tint, cubeGame, cubeGame >= 0.0f ? cubeGame * ambient : cubeGame);
    }
    UnlitRooms::SetDrawDark(false);
    return r;
}

template <typename DrawFn> D3D9Hooks::HookAction OnDrawTracked(IDirect3DDevice9* dev, DrawFn draw) {
    if (g_inOwnCall) return D3D9Hooks::HookAction::Continue;
    if (g_stateUnknown) {
        // hooks registered mid-session (or after Shutdown): the shaders bound now never went through our Set*Shader
        // hooks, so read them from the device once (review 25/09)
        IDirect3DPixelShader9* ps = nullptr;
        IDirect3DVertexShader9* vs = nullptr;
        dev->GetPixelShader(&ps);
        dev->GetVertexShader(&vs);
        TrackPs(ps, true);
        TrackVs(vs, true);
        if (ps) ps->Release(); // Classify / ClassifyVs pinned them
        if (vs) vs->Release();
        g_stateUnknown = false;
    }
    IDirect3DVertexShader9* foliage = g_curVsIsFoliage && g_objectFix.load(std::memory_order_relaxed) ? FoliageVsFor(dev, g_curVs) : nullptr;
    if (!foliage) return OnDrawInner(dev, draw);
    IDirect3DVertexShader9* original = g_curVs;
    g_inOwnCall = true;
    SetVs(dev, foliage);
    g_inOwnCall = false;
    if (OnDrawInner(dev, draw) == D3D9Hooks::HookAction::Continue) {
        g_inOwnCall = true;
        draw();
        g_inOwnCall = false;
    }
    g_inOwnCall = true;
    SetVs(dev, original);
    g_inOwnCall = false;
    g_foliageDrawn.fetch_add(1, std::memory_order_relaxed);
    return D3D9Hooks::HookAction::Skip;
}

// ---- Census and false colour (development build). A draw is a "lamp-lit candidate" when it binds a baked light map
// (A8R8G8B8 managed, one level, up to 1024: room / wall / floor / lot maps; or a 256x256 DXT5 terrain map) or is drawn
// with an outdoor rig (RigTracker mode 2). Candidates no fix claimed (OnDrawTracked returned Continue) are painted
// magenta in false-colour mode, and the census records them per (VS, PS) pair for ApexRadiance_Censo.txt. ----
UINT g_curPrims = 0;
std::atomic<bool> g_falseColor{false};
std::atomic<int> g_censusFrames{0}; // frames left to record
bool g_censusPending = false;       // write the report when the frames are done
struct CensusRow {
    int draws = 0, claimed = 0;
    UINT prims = 0;
    int rig = -1;
    std::string tex;
};
std::map<std::pair<IDirect3DVertexShader9*, IDirect3DPixelShader9*>, CensusRow> g_census;
std::unordered_map<IDirect3DPixelShader9*, bool> g_psIs3;
IDirect3DPixelShader9* g_magenta[2] = {}; // ps_2_0, ps_3_0
bool g_magentaTried = false;

bool LitCandidate(IDirect3DDevice9* dev, std::string& desc) {
    bool lit = false;
    for (DWORD s = 0; s < 16; s++) {
        IDirect3DBaseTexture9* b = nullptr;
        if (FAILED(dev->GetTexture(s, &b)) || !b) continue;
        D3DSURFACE_DESC d{};
        if (b->GetType() == D3DRTYPE_TEXTURE && SUCCEEDED(static_cast<IDirect3DTexture9*>(b)->GetLevelDesc(0, &d))) {
            const DWORD levels = b->GetLevelCount();
            if (d.Format == D3DFMT_A8R8G8B8 && d.Pool != D3DPOOL_DEFAULT && levels == 1 && d.Width <= 1024 && d.Height <= 1024) {
                lit = true;
                desc += std::format(" s{}:map{}x{}", s, d.Width, d.Height);
            } else if (d.Format == D3DFMT_DXT5 && d.Width == 256 && d.Height == 256 && levels <= 5) {
                lit = true;
                desc += std::format(" s{}:terrain", s);
            }
        }
        b->Release();
    }
    return lit;
}

bool PsIs3(IDirect3DPixelShader9* ps) {
    if (!ps) return false;
    auto it = g_psIs3.find(ps);
    if (it != g_psIs3.end()) return it->second;
    DWORD tok = 0;
    UINT size = 0;
    bool is3 = false;
    if (SUCCEEDED(ps->GetFunction(nullptr, &size)) && size >= 8) {
        std::vector<DWORD> t(size / 4);
        if (SUCCEEDED(ps->GetFunction(t.data(), &size))) tok = t[0];
        is3 = (tok & 0xFF00) == 0x0300;
    }
    g_psIs3[ps] = is3;
    return is3;
}

template <typename DrawFn> D3D9Hooks::HookAction OnDraw(IDirect3DDevice9* dev, DrawFn draw) {
    const bool fc = g_falseColor.load(std::memory_order_relaxed), census = g_censusFrames.load(std::memory_order_relaxed) > 0;
    if (kPublicBuild || (!fc && !census) || g_inOwnCall) return OnDrawTracked(dev, draw);
    std::string desc;
    const int rig = RigTracker::CurrentMode();
    const bool candidate = LitCandidate(dev, desc) || rig == 2;
    const D3D9Hooks::HookAction r = OnDrawTracked(dev, draw);
    if (!candidate) return r;
    const bool claimed = r == D3D9Hooks::HookAction::Skip;
    if (census) {
        CensusRow& row = g_census[{g_curVs, g_curPs}];
        row.draws++;
        row.claimed += claimed ? 1 : 0;
        row.prims += g_curPrims;
        row.rig = rig;
        if (row.tex.empty()) row.tex = desc.empty() ? " (so rig)" : desc;
    }
    if (claimed || !fc || !g_curPs) return r;
    if (!g_magentaTried) {
        g_magentaTried = true;
        for (int v = 0; v < 2; v++) {
            const DWORD code[] = {v ? 0xFFFF0300u : 0xFFFF0200u, 0x05000051u, 0xA00F0000u, 0x3F800000u, 0u, 0x3F800000u, 0x3F800000u, // def c0, 1, 0, 1, 1
                                  0x02000001u, 0x800F0800u, 0xA0E40000u,                                                           // mov oC0, c0
                                  0x0000FFFFu};
            if (FAILED(dev->CreatePixelShader(code, &g_magenta[v]))) g_magenta[v] = nullptr;
        }
    }
    IDirect3DPixelShader9* m = g_magenta[PsIs3(g_curPs) ? 1 : 0];
    if (!m) return r;
    IDirect3DPixelShader9* original = g_curPs;
    g_inOwnCall = true;
    SetPs(dev, m);
    draw();
    SetPs(dev, original);
    g_inOwnCall = false;
    return D3D9Hooks::HookAction::Skip;
}

void WriteCensus() {
    try {
        const std::filesystem::path base = std::filesystem::path(ApexPaths::ApexDirectory());
        const std::filesystem::path dir = base / L"Censo";
        std::filesystem::create_directories(dir);
        std::ofstream out(base / L"ApexRadiance_Censo.txt", std::ios::trunc);
        out << APEX_PRODUCT_NAME " census: draws that get baked light (light map) or an outdoor rig, per shader pair\n"
               "columns: draws | fixed | triangles | rig | VS hash/size | PS hash/size | textures\n\n";
        auto code = [](auto* sh, uint32_t& hash, UINT& size) {
            std::vector<DWORD> t;
            size = 0;
            hash = 0;
            if (sh && SUCCEEDED(sh->GetFunction(nullptr, &size)) && size >= 8 && size <= 65536) {
                t.resize(size / 4);
                if (FAILED(sh->GetFunction(t.data(), &size))) t.clear();
            }
            uint32_t h = 2166136261u;
            for (DWORD d : t) h = (h ^ d) * 16777619u;
            hash = t.empty() ? 0 : h;
            return t;
        };
        int unclaimed = 0;
        for (auto& [key, row] : g_census) {
            uint32_t vh, ph;
            UINT vsz, psz;
            const auto vs = code(key.first, vh, vsz);
            const auto ps = code(key.second, ph, psz);
            const bool none = row.claimed == 0;
            unclaimed += none ? 1 : 0;
            out << std::format("{} {:5} | {:5} | {:7} | {:2} | VS {:08X}/{} | PS {:08X}/{} |{}\n", none ? "SEM" : "ok ", row.draws, row.claimed, row.prims, row.rig, vh, vsz, ph,
                               psz, row.tex);
            if (none) {
                if (!vs.empty()) std::ofstream(dir / std::format("VS_{:08X}.bin", vh), std::ios::binary).write(reinterpret_cast<const char*>(vs.data()), vsz);
                if (!ps.empty()) std::ofstream(dir / std::format("PS_{:08X}.bin", ph), std::ios::binary).write(reinterpret_cast<const char*>(ps.data()), psz);
            }
        }
        out << std::format("\n{} pairs, {} without a fix (code in Apex Radiance\\Censo)\n", g_census.size(), unclaimed);
        LOG_INFO(std::format("[LotLightBridge] Census written: {} pairs, {} without a fix", g_census.size(), unclaimed));
    } catch (...) {
    }
    g_census.clear();
}

bool g_keepChunks = false; // Shutdown(true): a reinstall keeps the chunk maps, smoothed maps and atlas (same world)

void ClearChunks() {
    for (auto& [k, v] : g_chunks)
        if (v.tex) v.tex->Release();
    g_chunks.clear();
    g_chunkOfTexture.clear();
    LightmapSmooth::Clear();
}

} // namespace

namespace LotLightBridge {

// A C++ exception inside a hook would unwind into the game: turn the whole bridge off instead (review 25/09).
void HookFailed() {
    g_hookFailed = true;
    g_status = "Off after an internal error (see ApexRadiance_LOG.txt)";
    LOG_ERROR("[LotLightBridge] Exception inside the draw hook: fixes off until the game restarts");
}

void UpdateHooks() {
    static std::mutex m; // Install runs off the render thread at startup while Present may call the setters
    std::lock_guard<std::mutex> lock(m);
    const bool on = !g_hookFailed && (g_enabled.load() || g_objectFix.load() || g_roofFix.load() || g_waterFix.load() || g_wallGain.load() != 1.0f);
    if (on && !g_hooksRegistered) {
        g_stateUnknown = true;
        D3D9Hooks::RegisterSetPixelShader(kHookName, [](D3D9Hooks::DeviceContext&, IDirect3DPixelShader9* ps) {
            if (!g_inOwnCall && !g_hookFailed) try {
                    TrackPs(ps);
                } catch (...) {
                    HookFailed();
                }
            return D3D9Hooks::HookAction::Continue;
        });
        D3D9Hooks::RegisterSetVertexShader(kHookName, [](D3D9Hooks::DeviceContext&, IDirect3DVertexShader9* vs) {
            if (!g_inOwnCall && !g_hookFailed) try {
                    TrackVs(vs);
                } catch (...) {
                    HookFailed();
                }
            return D3D9Hooks::HookAction::Continue;
        });
        D3D9Hooks::RegisterDrawIndexedPrimitive(kHookName,
            [](D3D9Hooks::DeviceContext& ctx, D3DPRIMITIVETYPE type, INT bvi, UINT minV, UINT numV, UINT start, UINT prims) {
                if (g_hookFailed) return D3D9Hooks::HookAction::Continue;
                try {
                    g_curPrims = prims;
                    return OnDraw(ctx.device, [&]() { ctx.device->DrawIndexedPrimitive(type, bvi, minV, numV, start, prims); });
                } catch (...) {
                    g_inOwnCall = false;
                    HookFailed();
                    return D3D9Hooks::HookAction::Continue;
                }
            });
        D3D9Hooks::RegisterDrawPrimitive(kHookName, [](D3D9Hooks::DeviceContext& ctx, D3DPRIMITIVETYPE type, UINT start, UINT prims) {
            if (g_hookFailed) return D3D9Hooks::HookAction::Continue;
            try {
                g_curPrims = prims;
                return OnDraw(ctx.device, [&]() { ctx.device->DrawPrimitive(type, start, prims); });
            } catch (...) {
                g_inOwnCall = false;
                HookFailed();
                return D3D9Hooks::HookAction::Continue;
            }
        });
        g_hooksRegistered = true;
        if (g_status == "Off") g_status = "Waiting for the first draw";
    } else if (!on && g_hooksRegistered) {
        D3D9Hooks::UnregisterAll(kHookName);
        g_hooksRegistered = false;
        if (!g_keepChunks) ClearChunks();
        g_status = "Off";
    }
}

void SetEnabled(bool on) {
    g_enabled = on;
    if (!on) {
        if (!g_keepChunks) ClearChunks();
        g_status = "Off";
    }
    UpdateHooks();
}

void SetObjectShadowFix(bool on) {
    g_objectFix = on;
    UpdateHooks();
}

void SetNightLevel(float level) { g_night = level < 0 ? 0.0f : (level > 1 ? 1.0f : level); }

void SetRoofFix(bool on, float strength) {
    g_roofStrength = strength;
    if (g_roofFix.load() != on) {
        g_roofFix = on;
        UpdateHooks();
    }
}

void SetWaterFix(bool on, float strength, float reflection, bool filter, bool preserveColors) {
    g_waterFilter = filter;
    g_waterColorCompression = preserveColors;
    g_waterStrength = strength;
    g_waterRefl = reflection;
    if (g_waterFix.load() != on) {
        g_waterFix = on;
        UpdateHooks();
    }
}

void SetSidewalkClear(float amount) { g_sidewalkClear = amount < 0 ? 0.0f : (amount > 1 ? 1.0f : amount); }

void OnWorldChanged() {
    ClearChunks();
    RoomMapPadding::Clear();
    g_lampSwitchPrev.clear();
    g_lotLampSig.clear(); // the next enumeration starts the new world's lots from scratch (all new: nothing counted)
    g_lotSeen.clear();
    g_quietLogAt.clear();
    g_bakeSnap = LotLightBridge::BakeSnapshot{};
    g_lastUserLots.clear();
    g_lotRects.clear(); // soft lot edges: the next Present reads the new world's lots
    g_lotDrawSeen.clear();
    g_lotArrivals.clear();
    g_lampEditRefresh = false;
    g_lotRectMiss = true;
    g_haveLastEdgeRect = false;
}

void SetSoftLotEdges(bool on) { g_softEdges = on; }

void SetIndoorSmooth(bool on) { g_indoorSmooth = on; }

std::string IndoorSmoothStatus() {
    return std::format("{} | smooth stairs / instanced draws: {} | indoor objects: {} (map scale from the vertex shader {}, from the map size {})", g_indoorSmooth.load() ? "on" : "off",
                       g_basisSmoothDrawn.load(), g_indoorDrawn.load(), g_indoorUvFromVs.load(), g_indoorUvFallback.load());
}

void SetGroundBrightness(float ground, float roads, float lotLamps) {
    g_lotMapGain = lotLamps < 0.25f ? 0.25f : (lotLamps > 3.0f ? 3.0f : lotLamps);
    g_groundGain = ground < 0.25f ? 0.25f : (ground > 3.0f ? 3.0f : ground);
    g_roadGain = roads < 0.25f ? 0.25f : (roads > 3.0f ? 3.0f : roads);
}

std::string GroundBrightnessStatus() {
    return std::format("ground x{:.2f}, roads x{:.2f}, lot lamps on lot grass x{:.2f} (now x{:.2f} / x{:.2f} at night level {:.2f}) | draws with the gain: {} | terrain shaders: {}", g_groundGain.load(),
                       g_roadGain.load(), g_lotMapGain.load(), GroundGain(), RoadGain(), g_night.load(), g_groundGainDraws.load(), g_terrainLampConst.size());
}

std::string LotEdgeStatus() {
    if (!g_softEdges.load()) return "off";
    std::string s = std::format("on, band {:.1f} m | lots known: {} | lot passes feathered: {} | without a lot rectangle: {}", kEdgeBand, g_lotRects.size(),
                                g_edgeMatched.load(), g_edgeUnmatched.load());
    if (g_haveLastEdgeRect)
        s += std::format(" | last: lot {:08X}{:08X} {:.0f} x {:.0f} m at ({:.1f}, {:.1f})", g_lastEdgeRect.lotHi, g_lastEdgeRect.lotLo, g_lastEdgeRect.w, g_lastEdgeRect.d,
                         g_lastEdgeRect.tx, g_lastEdgeRect.tz);
    return s;
}

int ChunkCount() { return static_cast<int>(g_chunks.size()); }

void SetObjectPixelLamps(bool on, float strength) {
    g_objPixel = on;
    g_objPixelStrength = strength;
}

void SetObjectPixelLights(bool on, float strength) {
    g_objPixelLamps = on;
    g_objPixelLampStrength = strength;
}

void SetFenceGroundLight(bool on, float strength) {
    g_fenceFix = on;
    g_fenceStrength = strength;
}

void SetWallGain(float gain) {
    gain = gain < 0.25f ? 0.25f : (gain > 8.0f ? 8.0f : gain);
    const bool was = g_wallGain.load() != 1.0f;
    g_wallGain = gain;
    if (was != (gain != 1.0f)) UpdateHooks();
}

std::string WallStatus() { return std::format("outside walls: strength {:.2f} | draws: {} | variants seen: {}", g_wallGain.load(), g_wallDrawn.load(), g_wallConst.size()); }

std::string WaterStatus() {
    return std::format("water: {} | draws with reflection: {}", g_waterFix.load() ? (g_waterPs ? "active" : "waiting") : "off", g_waterDrawn.load());
}

void SetFalseColor(bool on) { g_falseColor = on; }
bool FalseColor() { return g_falseColor.load(); }

void RequestCensus() {
    if (g_censusPending) return;
    g_census.clear();
    g_censusPending = true;
    g_censusFrames = 3;
}

std::string CensusStatus() { return g_censusPending ? "writing..." : "ready"; }

void OnPresent() {
    g_lotDrawTick = GetTickCount();
    if (g_lotDrawSeen.size() > 1024) std::erase_if(g_lotDrawSeen, [](const auto& item) { return g_lotDrawTick - item.second > 10000; });
    if (g_censusPending && g_censusFrames.load() > 0 && --g_censusFrames == 0) {
        WriteCensus();
        g_censusPending = false;
    }
    // Lot rectangles for the soft lot edges: every 20 frames, or 5 frames after a lot pass found none (a lot that
    // streamed in) so a new lot gets its feather within a few frames.
    if (g_enabled.load(std::memory_order_relaxed)) {
        ++g_lotRectFrame;
        if (g_lotRectFrame >= 20 || (g_lotRectMiss && g_lotRectFrame >= 5)) {
            g_lotRectFrame = 0;
            g_lotRectMiss = false;
            RefreshLotRects();
        }
    }
    const bool editReady = g_lampEditRefresh && g_lotDrawTick - g_lampReadTick >= 50;
    if (++g_lampFrame < 20 && !g_lampRefreshNow && !editReady) return;
    g_lampFrame = 0;
    g_lampRefreshNow = false;
    g_lampEditRefresh = false;
    g_lampReadTick = g_lotDrawTick;
    FrameProfiler::ModTimeScope timed(FrameProfiler::ModTime::LampRefresh); // development build: "Lamp refresh (mod)"
    // As before 2026-09-29: with roofs / water / per-pixel object lamps on, the lit lamp list is rebuilt from a successful
    // enumeration, and the lot lamps are tracked even when the enumeration failed (over what it left in g_enumLights);
    // with those off, a failed enumeration skips the tracking.
    const bool wantLamps = g_roofFix.load() || g_waterFix.load() || g_objPixelLamps.load();
    const bool enumerated = EnumerateLights();
    if (!wantLamps && !enumerated) return;
    ReadEnumeratedLamps(wantLamps && enumerated);
    TrackLotLampEdits();
}

int LotLampEdits() { return g_lotLampEdits.load(std::memory_order_relaxed); }
int LotLampUserEdits() { return g_lotLampUserEdits.load(std::memory_order_relaxed); }
int LampSwitches() { return g_lampSwitches.load(std::memory_order_relaxed); }

std::string LotLampStatus() {
    return std::format("changes counted: {} (user-driven: {}) | ignored (streaming, still loading, bulk): {} | not counted: outside the bake {}, below the threshold {}, "
                       "animated {} ({} lamps) | lots tracked: {} | last: {}",
                       g_lotChangesCounted, g_lotLampUserEdits.load(), g_lotChangesIgnored, g_lampChangesOutside, g_lampChangesNoise, g_lampChangesAnimated,
                       g_lampsAnimated, g_lotSeen.size(), g_lastLotChange);
}

const BakeSnapshot& CurrentBakeLamps() { return g_bakeSnap; }
int LampEnumerations() { return g_lampEnumerations; }
const std::vector<uint64_t>& LastUserChangeLots() { return g_lastUserLots; }
void RequestLampRefresh() { g_lampRefreshNow = true; }
void RequestLampEditRefresh() { g_lampEditRefresh = true; }

std::vector<uint64_t> TakeLotArrivals() {
    std::vector<uint64_t> out(g_lotArrivals.begin(), g_lotArrivals.end());
    g_lotArrivals.clear();
    std::sort(out.begin(), out.end());
    return out;
}
bool LotVisible(uint64_t lot) { return VisibleLot(lot); }

bool EnumerateAllLights(std::vector<uintptr_t>& out) {
    if (!EnumerateLights()) return false;
    out = g_enumLights;
    return true;
}

// Lamps of `lot` in a snapshot sorted by lot: [first, last)
static std::pair<std::vector<BakeLamp>::const_iterator, std::vector<BakeLamp>::const_iterator> LotLamps(const BakeSnapshot& s, uint64_t lot) {
    const auto first = std::lower_bound(s.lamps.begin(), s.lamps.end(), lot, [](const BakeLamp& b, uint64_t v) { return b.lot < v; });
    const auto last = std::upper_bound(first, s.lamps.end(), lot, [](uint64_t v, const BakeLamp& b) { return v < b.lot; });
    return {first, last};
}

bool BakeTakes(const BakeLamp& b, bool plainLamps) { return b.baked && (plainLamps || !IsPlainType(b.type)); }

BakeDiff DiffBake(const BakeSnapshot& baked, const BakeSnapshot& now, bool plainLamps, const std::vector<uint64_t>& priorityLots) {
    BakeDiff d;
    auto inBake = [plainLamps](const BakeLamp& b) { return BakeTakes(b, plainLamps); };
    // one listed change: old = the lamp as baked (a, if it was in the bake), new = as it is now (b, if it is in the bake)
    auto note = [&d](const BakeLamp* a, const BakeLamp* b, bool user) {
        BakeChange c;
        const BakeLamp& at = b ? *b : *a;
        c.lot = at.lot;
        c.type = at.type;
        std::memcpy(c.pos, at.pos, sizeof c.pos);
        c.user = user;
        if (a) {
            c.hasOld = true;
            std::memcpy(c.oldRect, a->rect, sizeof c.oldRect);
        }
        if (b) {
            c.hasNew = true;
            std::memcpy(c.newRect, b->rect, sizeof c.newRect);
        }
        d.changes.push_back(c);
    };
    for (uint64_t lot : now.settledLots) {
        // a lot that streamed in after that bake was never in it: the game does not rebuild for streaming, nor does Apex
        if (!std::binary_search(baked.lots.begin(), baked.lots.end(), lot)) continue;
        const auto [a0, a1] = LotLamps(baked, lot);
        const auto [b0, b1] = LotLamps(now, lot);
        const int before = d.added + d.removed + d.switchedOn + d.switchedOff + d.light;
        std::vector<char> used(static_cast<size_t>(b1 - b0), 0);
        for (auto a = a0; a != a1; ++a) {
            // the same lamp: same type, within 5 cm (matched by place, not by pointer: a lot that streamed out and back in
            // has new light objects for the same lamps)
            auto m = b1;
            for (auto b = b0; b != b1; ++b)
                if (!used[static_cast<size_t>(b - b0)] && b->type == a->type && !MovedApart(a->pos, b->pos)) {
                    m = b;
                    break;
                }
            if (m == b1) {
                if (inBake(*a)) { // removed, or moved away
                    d.removed++;
                    note(&*a, nullptr, true);
                }
                continue;
            }
            used[static_cast<size_t>(m - b0)] = 1;
            const bool ia = inBake(*a), ib = inBake(*m);
            if (ia != ib) {
                if (m->animated) d.animated++;
                else {
                    (ib ? d.switchedOn : d.switchedOff)++;
                    note(ia ? &*a : nullptr, ib ? &*m : nullptr, false);
                }
            } else if (ia && (std::find(priorityLots.begin(), priorityLots.end(), lot) != priorityLots.end()
                                ? LightChanged(a->light, m->light) : LightDiffers(a->light, m->light))) {
                if (m->animated) d.animated++;
                else {
                    d.light++;
                    note(&*a, &*m, false);
                }
            }
        }
        for (auto b = b0; b != b1; ++b)
            if (!used[static_cast<size_t>(b - b0)] && inBake(*b)) { // placed, or moved here
                d.added++;
                note(nullptr, &*b, true);
            }
        if (d.added + d.removed + d.switchedOn + d.switchedOff + d.light != before) d.lots++;
    }
    return d;
}

std::vector<uint64_t> BakeDiff::Lots() const {
    std::vector<uint64_t> lots;
    for (const BakeChange& c : changes) lots.push_back(c.lot);
    std::sort(lots.begin(), lots.end());
    lots.erase(std::unique(lots.begin(), lots.end()), lots.end());
    return lots;
}

int AdoptNewLots(BakeSnapshot& baked, const BakeSnapshot& now, const std::vector<uint64_t>& editedLots) {
    std::vector<uint64_t> add;
    // Keep the first observed lamp state before an early user switch can be lost to
    // the 10 s streaming settle delay. This does not request any streaming rebuild.
    for (uint64_t lot : now.lots)
        if (!std::binary_search(baked.lots.begin(), baked.lots.end(), lot)
            && std::find(editedLots.begin(), editedLots.end(), lot) == editedLots.end()) add.push_back(lot);
    if (add.empty()) return 0;
    for (uint64_t lot : add) {
        const auto [first, last] = LotLamps(now, lot);
        baked.lamps.insert(baked.lamps.end(), first, last);
        baked.lots.insert(std::upper_bound(baked.lots.begin(), baked.lots.end(), lot), lot);
    }
    std::stable_sort(baked.lamps.begin(), baked.lamps.end(), [](const BakeLamp& a, const BakeLamp& b) { return a.lot < b.lot; });
    return static_cast<int>(add.size());
}

std::vector<BakeLamp> LampsOfLots(const BakeSnapshot& s, const std::vector<uint64_t>& lots) {
    std::vector<BakeLamp> out;
    for (uint64_t lot : lots) {
        const auto [first, last] = LotLamps(s, lot);
        out.insert(out.end(), first, last);
    }
    return out; // lots ascending, each lot's lamps in snapshot order: sorted by lot
}

void CoverLots(BakeSnapshot& baked, const std::vector<BakeChange>& changes, const std::vector<BakeLamp>& lamps) {
    if (changes.empty()) return;
    // only the changed lamps: the others keep their baked state, so small changes that were not relit still add up
    std::vector<BakeLamp> merged = baked.lamps;
    std::vector<char> taken(lamps.size(), 0);
    for (const BakeChange& c : changes) {
        const bool addition = c.user && !c.hasOld, removal = c.user && !c.hasNew;
        if (!addition) { // the lamp as baked (a removal, a switch or a relight)
            const auto it = std::find_if(merged.begin(), merged.end(), [&c](const BakeLamp& b) { return b.lot == c.lot && b.type == c.type && !MovedApart(b.pos, c.pos); });
            if (it != merged.end()) merged.erase(it);
        }
        if (!removal) // the lamp as it was when the relight was decided
            for (size_t i = 0; i < lamps.size(); i++)
                if (!taken[i] && lamps[i].lot == c.lot && lamps[i].type == c.type && !MovedApart(lamps[i].pos, c.pos)) {
                    taken[i] = 1;
                    merged.push_back(lamps[i]);
                    break;
                }
    }
    std::stable_sort(merged.begin(), merged.end(), [](const BakeLamp& a, const BakeLamp& b) { return a.lot < b.lot; });
    baked.lamps.swap(merged);
}

std::string BakeDiff::Text() const {
    if (!Any()) return animated ? std::format("no change (animated lamps ignored: {})", animated) : std::string("no change");
    std::string t = std::format("{} lots: {} added, {} removed, {} switched on, {} switched off, {} relit", lots, added, removed, switchedOn, switchedOff, light);
    if (animated) t += std::format(" (animated lamps ignored: {})", animated);
    return t;
}

std::string RoofStatus() {
    std::string s = std::format("roofs: {} | lamps on: {} | draws fixed: {} | with snow: {}", g_roofFix.load() ? (g_roofPs ? "fixed" : "waiting") : "off",
                                g_lampCount, g_roofDrawn.load(), g_roofSnowDrawn.load());
    if (!kPublicBuild) s += std::format(" | lamp choice memo: {} reused, {} computed", g_lampMemoHits, g_lampMemoMisses);
    return s;
}

std::string Status() {
    std::string s = std::format("{} | terrain chunks seen: {} | lot light fixed: {} draws (snow: {}, roads: {}, floors: {}, outdoor floors (summer): {}, snow on floors: {}, fences/stairs: {}, snow on objects: {}, snow with relief: {}, outdoor objects: {}) | without terrain texture: {}", g_status,
        g_chunks.size(), g_lotDrawn.load(), g_snowDrawn.load(), g_roadDrawn.load(), g_floorDrawn.load(), g_floorAtlasDrawn.load(), g_snowFloorDrawn.load(), g_fenceDrawn.load(), g_snowCoverDrawn.load(), g_snowReliefDrawn.load(), g_objLampDrawn.load(), g_lotMissing.load());
    if (!kPublicBuild) s += std::format(" | leftover textures skipped when looking for chunk light maps: {}", g_chunkStraySkipped.load());
    return s;
}

std::string DescribeDraw() {
    if (g_inOwnCall) return g_objDrawInfo.empty() ? std::string("mod draw (another fix)") : g_objDrawInfo;
    // without the hooks the remembered shaders are stale (possibly released)
    if (!g_hooksRegistered || g_hookFailed || g_stateUnknown) return "Night Lighting has no active hooks";
    static const char* kVs[] = {"other", "roof", "lake", "snowy lot", "road", "floor", "foliage", "fence/stairs (instanced)", "snow on object", "snow with relief",
                                "object with rig", "snowy floor"};
    static const char* kPs[] = {"unknown", "other", "world candidate", "lot light", "object rig", "roof", "lake", "snowy lot", "snowy roof",
                                "outside wall", "outdoor floor"};
    const auto itv = g_vsInfo.find(g_curVs);
    const int vc = itv == g_vsInfo.end() ? -1 : itv->second.cls;
    const int pc = static_cast<int>(Classify(g_curPs));
    const int rig = RigTracker::CurrentMode();
    std::string s = std::format("game (the mod did not replace this draw) | VS {} | PS {} | rig mode {}", vc < 0 || vc > 11 ? "not classified" : kVs[vc], pc >= 0 && pc < 11 ? kPs[pc] : "?",
                                rig);
    if (vc == 10) {
        const FoliageVs& o = itv->second.patched;
        s += std::format(" | object: world c{}, vertex lights c{}", o.worldK, o.vertexLight);
        const auto it = g_objLampPs.find(g_curPs);
        s += it == g_objLampPs.end() ? " | PS not tested yet" : it->second.ps ? " | PS accepted" : " | PS REFUSED (outside the pattern)";
        if (!g_objPixel.load(std::memory_order_relaxed)) s += " | object option off";
        if (rig != 1 && rig != 2) s += " | indoor rig (the game uses the lot light map; the mod leaves it)";
        float c[4];
        if (!LightmapSmooth::Atlas(c, false)) s += " | no ground light atlas";
    }
    return s;
}

void FurnitureTraceReset() { g_traceLast.clear(); }

// Development build (F6 recorder): room-mode furniture draws since the last call (the render thread)
std::string FurnitureDiag() {
    static long last[7] = {};
    const long now[7] = {g_fdMode0.load(), g_fdInactive.load(), g_fdNoChain.load(), g_nightFurniture.load(), static_cast<long>(g_indoorDrawn.load()),
                         g_fdUnlitSlots.load(), g_fdLampSlots.load()};
    long d[7];
    for (int k = 0; k < 7; k++) {
        d[k] = now[k] - last[k];
        last[k] = now[k];
    }
    return std::format("room-mode draws {} (Rooms at Night not acting {}, no rig chain {}, turned {}; Apex indoor-object shader {}) | rig slots turned {}, "
                       "lamps kept {} | acting {}, brightness x{:.3f}, blue kept {:.3f}", d[0], d[1], d[2], d[3], d[4], d[5], d[6], UnlitRooms::FurnitureActive() ? "yes" : "no",
                       UnlitRooms::FurnitureAmbient(), UnlitRooms::FurnitureTint());
}

std::string ObjectStatus() {
    return std::format("moon shadow on objects: {} | draws fixed: {} | foliage (wrap light): {} | winter foliage without shadow: {} | Rooms at Night on furniture: {} draws ({} with the blue tint in the game's shader)",
                       g_objectFix.load() ? (g_objectPs ? "fixed" : "waiting") : "off", g_objectDrawn.load(), g_foliageDrawn.load(), g_leafDrawn.load(), g_nightFurniture.load(),
                       g_nightFurnitureTinted.load());
}

void Shutdown(bool keepChunkMaps) {
    g_keepChunks = keepChunkMaps;
    g_objectFix = false;
    g_wallGain = 1.0f;
    g_roofFix = false;
    g_waterFix = false;
    g_objPixel = false;
    g_fenceFix = false;
    SetEnabled(false); // nothing left on: UpdateHooks unregisters the D3D hooks before the shaders are released
    if (g_replacementPs) {
        g_replacementPs->Release();
        g_replacementPs = nullptr;
    }
    if (g_snowPs) {
        g_snowPs->Release();
        g_snowPs = nullptr;
    }
    g_snowTried = false;
    for (auto* cache : {&g_roadPs, &g_floorPs, &g_snowFloorPs, &g_snowFloorPs0, &g_leafPs, &g_fencePs, &g_snowCoverPs, &g_snowReliefPs, &g_objLampPs, &g_basisSmoothPs, &g_cubeTintPs}) {
        for (auto& [k, p] : *cache)
            if (p.ps) p.ps->Release();
        cache->clear();
    }
    for (auto& [s, cache] : g_indoorPs)
        for (auto& [k, p] : cache)
            if (p.ps) p.ps->Release();
    g_indoorPs.clear();
    for (auto& [k, info] : g_vsInfo) { // patched copies: foliage / objects, and the outdoor floors of summer
        if (info.patched.vs) info.patched.vs->Release();
        if (info.floor.vs) info.floor.vs->Release();
    }
    g_curVsInfo = nullptr;
    g_vsInfo.clear();
    if (g_roofSnowPs) {
        g_roofSnowPs->Release();
        g_roofSnowPs = nullptr;
    }
    g_roofSnowTried = false;
    if (g_objectPs) {
        g_objectPs->Release();
        g_objectPs = nullptr;
    }
    g_objectCompileTried = false;
    g_objectFix = false;
    if (g_roofPs) {
        g_roofPs->Release();
        g_roofPs = nullptr;
    }
    if (g_waterPs) {
        g_waterPs->Release();
        g_waterPs = nullptr;
    }
    g_waterCompileTried = false;
    g_waterFix = false;
    g_roofCompileTried = false;
    g_roofFix = false;
    g_compileTried = false;
    g_classCache.clear();
    g_basisPs.clear();
    g_rigLookup.Clear();
    g_rigPsInfo.clear(); // shader addresses are reused after a restart of the feature (review M3)
    g_curPsBasis = false;
    RoomMapPadding::Clear();
    g_worldSamplers.clear();
    g_terrainLampConst.clear();
    for (auto*& m : g_magenta)
        if (m) {
            m->Release();
            m = nullptr;
        }
    g_magentaTried = false;
    g_psIs3.clear();
    g_census.clear();
    g_falseColor = false;
    for (auto& [tc, cache] : g_floorAtlasPs)
        for (auto& [k, p] : cache)
            if (p.ps) p.ps->Release();
    g_floorAtlasPs.clear();
    g_wallConst.clear();
    for (IUnknown* p : g_pinned) p->Release();
    g_pinned.clear();
    TrackPs(nullptr, true); // g_curPs = null, class Other
    TrackVs(nullptr, true); // g_curVs = null, no class
    g_stateUnknown = true;
    g_keepChunks = false;
}

} // namespace LotLightBridge
