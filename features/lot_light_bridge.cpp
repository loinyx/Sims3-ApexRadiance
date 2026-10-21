#include "shader_lookup_cache.h"
#include "world_lamp_policy.h"
#include "terrain_lighting_policy.h"
#include "lamp_reach.h"
#include "native_terrain_sampler.h"
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
#include "rt_outdoor_lamps.h"
#include "game_addresses.h"
#include "shader_ids.h"
#include "roof_ps_hlsl.h"
#include "water_lamps_hlsl.h"
#include "roof_snow_lamps_hlsl.h"
#include "wall_lamp_table.h"
#include "floor_atlas_table.h"
#include "shader_patches.h"
#include "mesh_occlusion.h"
#include "mesh_budget_policy.h"
#include "dev_switches.h"
#include "foliage_canopy.h"
#include "foliage_vertex_lamps.h"
#include "lot_lighting_motion.h"
#include "hook_guard.h"
#include <tuple>
#include <array>
#include <wrl/client.h>
#include "lightmap_smooth.h"
#include "terrain_chunk_relight.h"
#include "room_map_padding.h"
#include "level_light_share.h"
#include "ground_lamp_shadow.h"
#include "ground_light_gate.h"
#include "unlit_rooms.h"
#include "light_probe.h"
#include "rig_tracker.h"
#include "room_light_queue.h"
#include "object_light_bridge.h"
#include "lamp_mark_filter.h"
#include "atrium_hold.h"
#include "recorder.h"
#include "depth_share.h"
#include "render_callbacks.h"
#include "d3d9_extra_hooks.h"
#include "d3d9_hooks.h"
#include "shader_cache.h"
#include "frame_profiler.h"
#include "drag_lite.h"
#include "tester_toggles.h"
#include "perf_toggles.h"
#include "build_flavor.h"
#include "apex_paths.h"
#include "apex_log.h"
#include "apex_version.h"
#include "apex_memory.h"
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
#include <thread>
#include <condition_variable>
#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
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
// c31.x = "Lot lamps" brightness on the lot's own light map (night-weighted; 1 = the game). c31.y = 0 (12/10: before full
// night c3.x itself is set to ground gain x LampDay(night), TerrainLightingPolicy::LotPass, so it is no longer needed).
const char* kReplacementHlsl = R"(
float4 c0 : register(c0);
float4 c1 : register(c1);
float4 c2 : register(c2);
float4 c3 : register(c3);
float4 c4 : register(c4);
#ifdef WET
float4 c5 : register(c5);
float4 c6 : register(c6);
#endif
float4 cLotX : register(c28);
float4 cLotZ : register(c29);
float4 cEdge : register(c30);
float4 cLotGain : register(c31);
float4 cMaskX : register(c32);
float4 cMaskZ : register(c33);
samplerCUBE sSky : register(s0);
sampler2D sLot : register(s1);
sampler2D sTerrain : register(s2);
sampler2D sMask : register(s3);
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
    float3 uv1 = float3(i.terrainUv.xy, 1);
    // the terrain light stays outside the lot's rooms (a walled yard with grass): 1 outside, 0 inside, soft at the walls;
    // alpha: the share of the nearby outdoor lamps that reaches the texel past the outside walls (1 with no wall between)
    float4 maskTex = tex2D(sMask, float2(dot(uv1, cMaskX.xyz), dot(uv1, cMaskZ.xyz)));
    float mask = maskTex.r;
    float3 terrain = tex2D(sTerrain, i.terrainUv.xy).rgb * (c3.x + cLotGain.y) * mask;
    float2 lp = float2(dot(uv1, cLotX.xyz), dot(uv1, cLotZ.xyz)); // lot-local metres
    float2 e = min(lp, float2(cLotX.w, cLotZ.w) - lp);             // distance to the nearer edge on each axis
    float w = saturate(min(e.x, e.y) * cEdge.x + cEdge.y);
    w = w * w * (3 - 2 * w);
    // The studied VS maps local xz as (xz * 63/64 + 1/4) * VS c12.
    // Undo that contraction for the CPU floor map; zero z/w retains unsupported VS mapping.
    float2 lotUv = i.lotUv * (1 + cLotGain.z) + cLotGain.w;
    float3 lamps = lerp(terrain, max(tex2D(sLot, lotUv).rgb * cLotGain.x * c3.x, terrain), w) * maskTex.a;
    float3 col = sun * c0.rgb + lamps;
    col = texCUBE(sSky, i.normal).rgb * c4.x + col;
#ifdef WET
    col *= 1 + c5.x * (c6.x - 1); // the rain variant's wet darkening (kLotLightRainPs), with the game's c5.x / c6.x
#endif
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
const ShaderCache::Id kReplacementWetPsId = [] { // the same pass for the game's rain variant (kLotLightRainPs)
    ShaderCache::Desc d;
    d.tag = "NightLighting lot light pass (rain)";
    d.source = kReplacementHlsl;
    d.sourceName = "lot_light_bridge";
    d.macros = {{"WET", "1"}};
    d.priority = 1;
    return ShaderCache::Add(std::move(d));
}();
const ShaderCache::Id kObjectRigPsId = AddLotShader("NightLighting object rig (moon shadow)", kObjectRigHlsl, "ps_2_0", 0);
const ShaderCache::Id kRoofPsId = AddLotShader("NightLighting roofs", kRoofHlsl, "ps_3_0", 0);
const ShaderCache::Id kWaterPsId = AddLotShader("NightLighting lake water", kWaterLampsHlsl, "ps_3_0", 0);
const ShaderCache::Id kWaterSeaPsId = [] { // the lake pass for the sea that has no reflection (kSeaNoReflPs)
    ShaderCache::Desc d;
    d.tag = "NightLighting sea water (no game reflection)";
    d.source = kWaterLampsHlsl;
    d.sourceName = "lot_light_bridge";
    d.macros = {{"SEA", "1"}};
    d.priority = 1;
    return ShaderCache::Add(std::move(d));
}();
const ShaderCache::Id kWaterWeatherPsId = [] { // the lake pass for the lake water of rain and snow (kLakeRainPs, kLakeSnowPs)
    ShaderCache::Desc d;
    d.tag = "NightLighting lake water (rain and snow)";
    d.source = kWaterLampsHlsl;
    d.sourceName = "lot_light_bridge";
    d.macros = {{"WEATHER", "1"}};
    d.priority = 1;
    return ShaderCache::Add(std::move(d));
}();
const ShaderCache::Id kRoofSnowPsId = AddLotShader("NightLighting snowy roofs", kRoofSnowLampsHlsl, "ps_3_0", 1);

enum class PsClass : uint8_t { Unknown, Other, WorldCandidate, WorldMultiLight, WorldCompact, LotLight, ObjectRig, Roof, Lake, Sea, LakeWeather, LotLightSnow, LotLightSnowCut, LotLightMelt, RoofSnow, WallGain, FloorAtlas };

std::atomic<bool> g_enabled{false};
bool g_hooksRegistered = false;
bool g_inOwnCall = false;
IDirect3DPixelShader9* g_curPs = nullptr;
PsClass g_curClass = PsClass::Other;
std::unordered_map<IDirect3DPixelShader9*, PsClass> g_classCache;
std::unordered_set<IDirect3DPixelShader9*> g_basisPs; // pixel shaders that read the room basis maps (RoomMapPadding)
bool g_curPsBasis = false;
// Classify's answers per pixel shader in one entry (14/10: TrackPs made two hash lookups per shader change), with the
// samplers the shader declares and the one where DrawIndoorObject last found its room light map. Render thread.
struct PsFacts {
    PsClass cls = PsClass::Other;
    bool basis = false;
    bool roomMap = false;  // reads a room light map (Smooth light maps, g_roomMapPs)
    uint16_t declared = 0; // bit s = sampler s declared (0: not read)
    int8_t lastLm = -1;    // DrawIndoorObject: the sampler of the room light map at the last draw that found one
};
std::unordered_map<IDirect3DPixelShader9*, PsFacts> g_psFacts;
PsFacts* g_curFacts = nullptr; // g_curPs's entry (null for no shader)
bool g_curFactsKnown = false;  // g_curFacts follows g_curPs (TrackPs with PerfToggles::NightLightingPsFacts on)
// RigTracker::CurrentMode for the draw being handled (14/10: read 2-3 times a draw, each under SEH). Valid while
// g_drawRigValid (set around Night Lighting's draw callback); elsewhere RigMode reads it directly.
int g_drawRig = -1;
bool g_drawRigValid = false, g_drawRigRead = false;
inline int RigMode() {
    if (!g_drawRigValid || !PerfToggles::RigModeOncePerDraw.load(std::memory_order_relaxed)) return RigTracker::CurrentMode();
    if (!g_drawRigRead) g_drawRig = RigTracker::CurrentMode(), g_drawRigRead = true;
    return g_drawRig;
}
struct DrawRigScope { // around one draw callback of Night Lighting (not nested: its re-issued draws skip the callback)
    DrawRigScope() { g_drawRigValid = true, g_drawRigRead = false; }
    ~DrawRigScope() { g_drawRigValid = false, g_drawRigRead = false; }
};
std::unordered_set<IDirect3DPixelShader9*> g_roomMapPs; // room-lit pixel shaders (ShaderPatches::IsRoomMapPs): Smooth light maps
bool g_curPsRoomMap = false;
// The samplers each WorldCandidate pixel shader declares (bit s = s declared), from its bytecode in Classify.
// RecordWorldChunk looks for the chunk light map only there: a texture left bound in a sampler the shader never reads
// (e.g. the neighbour chunk's map in s8 while a 3-layer chunk reads s7) was taken as this chunk's map, depending on the
// draw order, i.e. on the camera; the road, fence and lot grass fixes then used a map without the lamps (a gameplay
// video: those fixes switching off and on together as the camera moved). g_chunkStraySkipped counts the declared-sampler
// textures skipped because they already are the map of another chunk.
std::unordered_map<IDirect3DPixelShader9*, uint16_t> g_worldSamplers;
std::atomic<int> g_chunkStraySkipped{0};
IDirect3DPixelShader9* g_replacementPs = nullptr;
IDirect3DPixelShader9* g_replacementWetPs = nullptr;            // kReplacementWetPsId, for the rain lot light pass
std::unordered_set<IDirect3DPixelShader9*> g_wetLotPs;          // game shaders classified LotLight that are kLotLightRainPs
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
// Roads and outdoor floors keep the game's own lamp factor (cK.x x the gain) and take the daytime curve on top of it (12/10):
// x LampDay(night), exactly 1 at night, so the previous night value stays bit for bit
float RoadDayGain() { return RoadGain() * TerrainLightingPolicy::LampDay(g_night.load(std::memory_order_relaxed)); }
float FloorDayGain() { return GroundGain() * TerrainLightingPolicy::LampDay(g_night.load(std::memory_order_relaxed)); }
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
// cK.x = value for one draw (on = false: nothing), restored afterwards
struct ConstX {
    IDirect3DDevice9* dev;
    int k = -1;
    float old[4] = {};
    ConstX(IDirect3DDevice9* d, int reg, bool on, float value) : dev(d) {
        if (!on || reg < 0 || !std::isfinite(value) || FAILED(dev->GetPixelShaderConstantF(static_cast<UINT>(reg), old, 1)) || old[0] == value) return;
        const float c[4] = {value, old[1], old[2], old[3]};
        SetPsConst(dev, static_cast<UINT>(reg), c, 1);
        k = reg;
        g_groundGainDraws.fetch_add(1, std::memory_order_relaxed);
    }
    ~ConstX() {
        if (k >= 0) SetPsConst(dev, static_cast<UINT>(k), old, 1);
    }
    ConstX(const ConstX&) = delete;
    ConstX& operator=(const ConstX&) = delete;
};

struct TerrainConst {
    IDirect3DDevice9* dev;
    int k = -1;
    float old[4]{};
    TerrainConst(IDirect3DDevice9* d, int reg, bool squared) : dev(d) {
        if (g_night.load(std::memory_order_relaxed) == 1.0f && g_groundGain.load(std::memory_order_relaxed) == 1.0f) return;
        if (reg < 0 || FAILED(dev->GetPixelShaderConstantF(reg, old, 1))) return;
        const float value = TerrainLightingPolicy::LampScale(old[0], g_night.load(std::memory_order_relaxed),
                                                            g_groundGain.load(std::memory_order_relaxed), squared);
        if (value == old[0] || !std::isfinite(value)) return;
        const float c[4] = {value, old[1], old[2], old[3]};
        if (SUCCEEDED(D3D9Hooks::CallOriginalSetPixelShaderConstantF(dev, reg, c, 1))) {
            k = reg;
            g_groundGainDraws.fetch_add(1, std::memory_order_relaxed);
        }
    }
    ~TerrainConst() { if (k >= 0) SetPsConst(dev, k, old, 1); }
};

std::atomic<int> g_objectDrawn{0};
bool g_compileTried = false;
bool g_wetCompileTried = false;
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
    float m2 = 0, m10 = 1;  // the rotation's other terms (world.z = m2 lx + m10 lz + tz; VS c10.x, c10.z)
    float ty = 0;           // the lot's origin height (m13): its ground for the lamp shade (GroundShadow)
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
std::atomic<DWORD> g_lotDrawTick{0}; // (14/10: atomic, NativeWallScale reads it from any thread)
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
                r.m2 = m[2];
                r.m10 = m[10];
                r.ty = m[13];
                if (std::isfinite(r.tx) && std::isfinite(r.tz) && std::isfinite(r.m0) && std::isfinite(r.m8) && std::isfinite(r.m2) && std::isfinite(r.m10) && std::isfinite(r.ty)) n++;
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

// ---- Room mask (the ground light went inside a room): the terrain light (world atlas, lamp stamps with
// no wall occlusion) reached the grass inside a walled room with no floor and no roof. Per lot, a texture of its rooms
// (LevelLightShare::LotGroundMask, 4 texels a metre, bilinear: soft at the walls) multiplies the terrain term of the lot
// light pass (and the snowy passes); 1 outside, 0 inside. Checked again every 500 ms per lot (walls placed or removed).
// A draw with no known lot binds a white 1 x 1 texture: the pass as before.
// Lamp shade on the lot ground (13/10: a free-standing wall next to a garden post left the grass behind it fully lit; the
// atlas is the terrain stamp, baked with no occluder, docs engine/terrain-and-light-bake.md). The same texture carries, in
// alpha, the share of the nearby outdoor lamps' stamp that reaches each texel past the outside walls (GroundShadow, 4
// texels a metre on the room mask's grid; 255 inside the rooms, whose own lamps light them). The lot pass multiplies its
// whole lamp term by it (the atlas, the lot map and the soft edge's target); the snowy passes their max(lot map, atlas).
// Built a slice at a time on the render thread (ShadeTick, on a worker thread since 15/10) from copies: the outside walls (the copy
// "Buildings block light on objects" tests) and the bake lamp snapshot; built again only when the walls' or slabs' copy,
// the lot's nearby lamps (place, about 20% of light, in or out of the bake) or the lot's place changed, at most every 2 s.
// A lot's shade over the world grass (15/10): the share per texel and the lamps' stamp weight it is a share of, so the lots
// over one texel combine as sum M w / sum w (the lowest of them darkened a lamp's light past a gap with a neighbour's shade
// of other lamps)
struct ExtData {
    std::vector<uint8_t> v;
    std::vector<float> w;
};
struct LotMask {
    IDirect3DTexture9* tex = nullptr;
    uint64_t sig = 0;
    DWORD checked = 0;
    int w = 0, h = 0;               // texels of the room mask
    std::vector<uint8_t> room;      // the room mask (255 outside rooms)
    std::vector<uint8_t> shade;     // the lamp shade (empty: none yet, 255 everywhere)
    uint64_t shadeKey = 0;          // the inputs the shade shown was made from (set when a build is applied)
    uint64_t queuedKey = 0;         // the inputs of the build queued or running for it (0 = none)
    DWORD shadeAt = 0;              // when the last build for this lot was queued
    DWORD lastUse = 0;              // when a draw last asked for its texture (the cache drops the oldest, EvictLotMasks)
    bool dirty = false;             // room or shade changed: the texture is made again at the next draw
    // (15/10) the shade on the grid it was built on: the lot and the world grass around it its lamps reach (ShadeGrid), for
    // the world terrain pass (ChunkShadeTexture); empty when nothing there is shaded
    GroundShadow::Grid extGrid;
    std::shared_ptr<const ExtData> ext;
    float ex0 = 0, ez0 = 0, ex1 = 0, ez1 = 0; // its world box
};
std::unordered_map<uint64_t, LotMask> g_lotMasks;
IDirect3DTexture9* g_whiteMask = nullptr;
std::atomic<long> g_maskBuilds{0}, g_maskWhite{0};
// The texture: A8R8G8B8, rgb = the room mask, a = the lamp shade (1 x 1 white when no lot)
IDirect3DTexture9* WhiteMask(IDirect3DDevice9* dev) {
    if (!g_whiteMask && SUCCEEDED(dev->CreateTexture(1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &g_whiteMask, nullptr))) {
        D3DLOCKED_RECT lr;
        if (SUCCEEDED(g_whiteMask->LockRect(0, &lr, nullptr, 0))) {
            *static_cast<DWORD*>(lr.pBits) = 0xFFFFFFFFu;
            g_whiteMask->UnlockRect(0);
        }
    }
    return g_whiteMask;
}

constexpr DWORD kShadeMinEveryMs = 2000;
struct ShadeWant {
    uint64_t lot = 0, key = 0;
    LotRect rect{};
    int w = 0, h = 0;
};
std::vector<ShadeWant> g_shadeQueue; // render thread; one entry per lot
std::atomic<long> g_shadeBuilt{0}, g_shadeDropped{0};
long long g_shadeTests = 0, g_shadeThrough = 0; // texel tests; of them crossing a wall with patches (openings, lower cells) and keeping light
int g_shadeLastPatches = 0, g_shadeLastFences = 0;
double g_shadeLastMs = 0.0, g_shadeMaxMs = 0.0;
int g_shadeLastLamps = 0, g_shadeLastWalls = 0;
float g_shadeLastMin = 1.0f;
bool g_shadeOn = true;

// ---- The builds run on a worker thread (15/10: they ran 1 ms a frame on the render thread, so a lot's shade took up to a
// couple of seconds to show and every frame paid for it). A lot's shade (GroundShadow::ShadeJob) and a chunk's texture
// (its mips) are made there from copies; the render thread only makes the inputs, uploads and applies. One thread, tasks
// in order; results carry the epoch they were asked in (ReleaseLotMasks starts a new one, older results are dropped).
struct ExtSrc { // one lot's shade over the world grass, for a chunk build (shared, never changed once made)
    GroundShadow::Grid grid;
    std::shared_ptr<const ExtData> v;
    float x0 = 0, z0 = 0, x1 = 0, z1 = 0;
};
void CrownsIn(float x0, float z0, float x1, float z1, std::vector<Canopy::Ellipsoid>& out); // canopy_ground_shadow (below)
struct ShadeTask {
    bool chunk = false;
    uint32_t epoch = 0;
    // a lot
    uint64_t lot = 0, key = 0;
    int w = 0, h = 0, ox = 0, oz = 0;
    GroundShadow::Grid grid;
    std::vector<GroundShadow::Lamp> lamps;
    std::vector<GroundShadow::Wall> walls;
    std::vector<WallOpenings::Patch> patches;
    std::vector<Canopy::Ellipsoid> crowns; // tree and bush crowns (canopy_ground_shadow)
    // a chunk
    std::pair<int, int> chunkKey{};
    std::vector<ExtSrc> srcs;
};
struct ShadeResult {
    bool chunk = false;
    uint32_t epoch = 0;
    uint64_t lot = 0, key = 0;
    int w = 0, h = 0, ox = 0, oz = 0;
    GroundShadow::Grid grid;
    std::vector<uint8_t> res;
    std::vector<float> den;
    double ms = 0.0;
    long long tests = 0, through = 0;
    std::pair<int, int> chunkKey{};
    std::vector<std::vector<uint8_t>> mips; // empty: no lot shades the chunk
};
constexpr int kChunkShadeTexels = 1024;
constexpr float kChunkSize = 256.0f;
// The shade of one lot at the world point (bilinear on its grid); false outside its grid
bool ExtShadeAt(const ExtSrc& m, float x, float z, float& v, float& wt) {
    float fi, fj;
    if (!GroundShadow::WorldTexel(m.grid, x, z, fi, fj)) return false;
    const int W = m.grid.w, H = m.grid.h;
    if (!(fi >= 0.0f) || !(fj >= 0.0f) || !(fi <= W) || !(fj <= H)) return false;
    const float a = std::clamp(fi - 0.5f, 0.0f, W - 1.0f), b = std::clamp(fj - 0.5f, 0.0f, H - 1.0f);
    const int i0 = static_cast<int>(a), j0 = static_cast<int>(b), i1 = std::min(i0 + 1, W - 1), j1 = std::min(j0 + 1, H - 1);
    const float u = a - i0, t = b - j0;
    const std::vector<uint8_t>& e = m.v->v;
    const std::vector<float>& ww = m.v->w;
    const auto at = [&](int i, int j) { return static_cast<float>(e[static_cast<size_t>(j) * W + i]); };
    const auto wa = [&](int i, int j) { return ww[static_cast<size_t>(j) * W + i]; };
    v = (at(i0, j0) * (1 - u) + at(i1, j0) * u) * (1 - t) + (at(i0, j1) * (1 - u) + at(i1, j1) * u) * t;
    wt = (wa(i0, j0) * (1 - u) + wa(i1, j0) * u) * (1 - t) + (wa(i0, j1) * (1 - u) + wa(i1, j1) * u) * t;
    return true;
}
// Worker: the chunk's texels (the lots' shades weighed by their lamps' light there, 255 where none) and their mips
void BuildChunkMips(const ShadeTask& t, ShadeResult& r) {
    constexpr int N = kChunkShadeTexels;
    constexpr float tpm = N / kChunkSize; // texels a metre
    const float wx0 = t.chunkKey.first - kChunkSize * 0.5f, wz0 = t.chunkKey.second - kChunkSize * 0.5f;
    std::vector<float> sumW, sumMW;
    bool any = false;
    for (const ExtSrc& m : t.srcs) {
        const size_t n = static_cast<size_t>(m.grid.w) * m.grid.h;
        if (!m.v || m.v->v.size() != n || m.v->w.size() != n) continue;
        if (sumW.empty()) sumW.assign(static_cast<size_t>(N) * N, 0.0f), sumMW.assign(static_cast<size_t>(N) * N, 0.0f);
        const int i0 = std::max(0, static_cast<int>(std::floor((m.x0 - wx0) * tpm))), i1 = std::min(N - 1, static_cast<int>(std::ceil((m.x1 - wx0) * tpm)));
        const int j0 = std::max(0, static_cast<int>(std::floor((m.z0 - wz0) * tpm))), j1 = std::min(N - 1, static_cast<int>(std::ceil((m.z1 - wz0) * tpm)));
        for (int j = j0; j <= j1; j++)
            for (int i = i0; i <= i1; i++) {
                float v, wt;
                if (!ExtShadeAt(m, wx0 + (i + 0.5f) / tpm, wz0 + (j + 0.5f) / tpm, v, wt)) continue;
                const float w = std::max(wt, 0.0f) + 1e-4f; // (a lot with no lamp here weighs almost nothing)
                const size_t at = static_cast<size_t>(j) * N + i;
                sumW[at] += w;
                sumMW[at] += w * v;
            }
    }
    if (sumW.empty()) return;
    std::vector<uint8_t> px(static_cast<size_t>(N) * N, 255);
    for (size_t k = 0; k < px.size(); k++)
        if (sumW[k] > 0.0f) {
            const uint8_t q = static_cast<uint8_t>(std::lround(std::clamp(sumMW[k] / sumW[k], 0.0f, 255.0f)));
            if (q < 255) px[k] = q, any = true;
        }
    if (!any) return; // nothing shaded: no texture
    int n = N;
    r.mips.push_back(std::move(px));
    while (n > 1) { // 2 x 2 averages down to 1 x 1
        const std::vector<uint8_t>& a = r.mips.back();
        const int h = n / 2;
        std::vector<uint8_t> next(static_cast<size_t>(h) * h);
        for (int j = 0; j < h; j++)
            for (int i = 0; i < h; i++) {
                const size_t p = static_cast<size_t>(2 * j) * n + 2 * i, q = p + n;
                next[static_cast<size_t>(j) * h + i] = static_cast<uint8_t>((a[p] + a[p + 1] + a[q] + a[q + 1] + 2) / 4);
            }
        r.mips.push_back(std::move(next));
        n = h;
    }
}
class ShadeWorker {
public:
    // (no join at unload: Shutdown stops it; a thread left at process exit is ended by the system)
    ~ShadeWorker() {
        if (th_.joinable()) th_.detach();
    }
    void Submit(ShadeTask&& t) {
        {
            std::lock_guard<std::mutex> lk(mx_);
            in_.push_back(std::move(t));
        }
        Start();
        cv_.notify_one();
    }
    // render thread: the finished results
    std::vector<ShadeResult> Take() {
        std::lock_guard<std::mutex> lk(mx_);
        std::vector<ShadeResult> r;
        r.swap(out_);
        return r;
    }
    size_t Pending() {
        std::lock_guard<std::mutex> lk(mx_);
        return in_.size() + busy_;
    }
    void Stop() {
        {
            std::lock_guard<std::mutex> lk(mx_);
            stop_ = true;
            in_.clear();
        }
        cv_.notify_all();
        if (th_.joinable()) th_.join();
        std::lock_guard<std::mutex> lk(mx_);
        stop_ = false;
        out_.clear();
        busy_ = 0;
    }

private:
    void Start() {
        if (th_.joinable()) return;
        th_ = std::thread([this] { Run(); });
        SetThreadPriority(th_.native_handle(), THREAD_PRIORITY_BELOW_NORMAL);
    }
    void Run() {
        for (;;) {
            ShadeTask t;
            {
                std::unique_lock<std::mutex> lk(mx_);
                cv_.wait(lk, [this] { return stop_ || !in_.empty(); });
                if (stop_) return;
                t = std::move(in_.front());
                in_.pop_front();
                busy_ = 1;
            }
            ShadeResult r;
            r.chunk = t.chunk;
            r.epoch = t.epoch;
            LARGE_INTEGER f, t0, t1;
            QueryPerformanceFrequency(&f);
            QueryPerformanceCounter(&t0);
            try {
                if (t.chunk) {
                    r.chunkKey = t.chunkKey;
                    BuildChunkMips(t, r);
                } else {
                    r.lot = t.lot, r.key = t.key, r.w = t.w, r.h = t.h, r.ox = t.ox, r.oz = t.oz, r.grid = t.grid;
                    GroundShadow::ShadeJob job;
                    job.Init(t.grid, std::move(t.lamps), std::move(t.walls), std::move(t.patches), std::move(t.crowns));
                    job.Step([this] { return !stop_; });
                    r.res = job.Result();
                    r.den = job.StampWeight();
                    r.tests = job.Tests();
                    r.through = job.Through();
                }
            } catch (...) {
                r.res.clear();
                r.mips.clear();
            }
            QueryPerformanceCounter(&t1);
            r.ms = static_cast<double>(t1.QuadPart - t0.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
            std::lock_guard<std::mutex> lk(mx_);
            busy_ = 0;
            if (!stop_) out_.push_back(std::move(r));
        }
    }
    std::thread th_;
    std::mutex mx_;
    std::condition_variable cv_;
    std::deque<ShadeTask> in_;
    std::vector<ShadeResult> out_;
    size_t busy_ = 0;
    std::atomic<bool> stop_{false};
};
ShadeWorker g_shadeWorker;
uint32_t g_shadeEpoch = 1;
int g_lotsInFlight = 0;

// ---- The shade on the world grass around lots (15/10). The world terrain pass lights the grass outside lots with the
// terrain map (its bake lamps, lot lamps among them, with no wall), so a wall's shadow stopped at the lot's edge. One
// texture per terrain chunk (L8, 4 texels a metre, 1024 x 1024 over the chunk's 256 m, full mips) holds the lowest of the
// lots' shades over it (LotMask::ext, 255 where none); it shares the chunk light map's uv (uv = (world xz - centre) / 256 +
// 0.5, RecordWorldChunk), and the patched terrain shader multiplies the lamp light by it (ShaderPatches::PatchTerrainShade).
// A chunk is made again on the worker when a lot's shade over it changed; it keeps its texture until the new one is ready.
struct ChunkShade {
    IDirect3DTexture9* tex = nullptr;
    bool dirty = true;
    bool building = false;
};
std::map<std::pair<int, int>, ChunkShade> g_chunkShades; // key: chunk centre (x, z), as g_chunks
std::atomic<long> g_chunkShadeBuilds{0};
double g_chunkShadeLastMs = 0.0;
std::map<std::pair<int, int>, ShadeResult> g_chunkReady; // finished chunk builds waiting for their chunk's draw
void ReleaseChunkShades() {
    for (auto& [k, c] : g_chunkShades)
        if (c.tex) c.tex->Release();
    g_chunkShades.clear();
    g_chunkReady.clear();
}
void ReleaseLotMasks() {
    for (auto& [lot, m] : g_lotMasks)
        if (m.tex) m.tex->Release();
    g_lotMasks.clear();
    ReleaseChunkShades();
    if (g_whiteMask) g_whiteMask->Release();
    g_whiteMask = nullptr;
    g_shadeQueue.clear();
    g_shadeEpoch++; // the builds in flight are dropped when they come back
    g_lotsInFlight = 0;
}
// The chunks over a world box are made again at their next draw
void MarkChunkShades(float x0, float z0, float x1, float z1) {
    if (!(x1 >= x0) || !(z1 >= z0) || x1 - x0 > 4096.0f || z1 - z0 > 4096.0f) return;
    const int i0 = static_cast<int>(std::floor(x0 / kChunkSize)), i1 = static_cast<int>(std::floor(x1 / kChunkSize));
    const int j0 = static_cast<int>(std::floor(z0 / kChunkSize)), j1 = static_cast<int>(std::floor(z1 / kChunkSize));
    for (int i = i0; i <= i1; i++)
        for (int j = j0; j <= j1; j++) g_chunkShades[{i * 256 + 128, j * 256 + 128}].dirty = true;
}
void MarkLotChunkShades(const LotMask& m) {
    if (m.ext) MarkChunkShades(m.ex0, m.ez0, m.ex1, m.ez1);
}
// The chunk's shade texture, or null where no lot shades it (the terrain draw is then left as it was). A changed chunk is
// sent to the worker; its texture (or none) stays until the new one is uploaded (ShadeTick).
void ApplyChunkResult(IDirect3DDevice9* dev, ShadeResult& r);
IDirect3DTexture9* ChunkShadeTexture(IDirect3DDevice9* dev, const std::pair<int, int>& key) {
    if (!g_shadeOn) return nullptr;
    if (const auto ready = g_chunkReady.find(key); ready != g_chunkReady.end()) {
        ShadeResult r = std::move(ready->second);
        g_chunkReady.erase(ready);
        ApplyChunkResult(dev, r);
    }
    const auto it = g_chunkShades.find(key);
    if (it == g_chunkShades.end()) return nullptr;
    ChunkShade& c = it->second;
    if (c.dirty && !c.building) {
        c.dirty = false;
        c.building = true;
        ShadeTask t;
        t.chunk = true;
        t.epoch = g_shadeEpoch;
        t.chunkKey = key;
        const float wx0 = key.first - kChunkSize * 0.5f, wz0 = key.second - kChunkSize * 0.5f;
        for (const auto& [lot, m] : g_lotMasks)
            if (m.ext && !(m.ex1 < wx0 || m.ez1 < wz0 || m.ex0 > wx0 + kChunkSize || m.ez0 > wz0 + kChunkSize))
                t.srcs.push_back(ExtSrc{m.extGrid, m.ext, m.ex0, m.ez0, m.ex1, m.ez1});
        g_shadeWorker.Submit(std::move(t));
    }
    return c.tex;
}
// Render thread: a chunk's new texels (mips) into its texture; none: its texture goes
void ApplyChunkResult(IDirect3DDevice9* dev, ShadeResult& r) {
    const auto it = g_chunkShades.find(r.chunkKey);
    if (it == g_chunkShades.end()) return;
    ChunkShade& c = it->second;
    c.building = false;
    g_chunkShadeLastMs = r.ms;
    g_chunkShadeBuilds.fetch_add(1, std::memory_order_relaxed);
    bool ok = !r.mips.empty();
    if (ok && !c.tex && FAILED(dev->CreateTexture(kChunkShadeTexels, kChunkShadeTexels, 0, 0, D3DFMT_L8, D3DPOOL_MANAGED, &c.tex, nullptr))) c.tex = nullptr, ok = false;
    if (ok) {
        const DWORD levels = std::min<DWORD>(c.tex->GetLevelCount(), static_cast<DWORD>(r.mips.size()));
        int n = kChunkShadeTexels;
        for (DWORD l = 0; l < levels && ok; l++, n = std::max(n / 2, 1)) {
            D3DLOCKED_RECT lr;
            if (r.mips[l].size() != static_cast<size_t>(n) * n || FAILED(c.tex->LockRect(l, &lr, nullptr, 0))) {
                ok = false;
                break;
            }
            for (int j = 0; j < n; j++) std::memcpy(static_cast<BYTE*>(lr.pBits) + static_cast<size_t>(j) * lr.Pitch, &r.mips[l][static_cast<size_t>(j) * n], n);
            c.tex->UnlockRect(l);
        }
    }
    if (!ok) {
        if (c.tex) c.tex->Release();
        c.tex = nullptr;
        if (!c.dirty) g_chunkShades.erase(it);
    }
}

// The lot's world box (its rectangle through the lot matrix)
void LotBox(const LotRect& r, float& x0, float& z0, float& x1, float& z1) {
    x0 = z0 = 1e30f;
    x1 = z1 = -1e30f;
    const float lx[4] = {0, r.w, 0, r.w}, lz[4] = {0, 0, r.d, r.d};
    for (int c = 0; c < 4; c++) {
        const float x = r.m0 * lx[c] + r.m8 * lz[c] + r.tx, z = r.m2 * lx[c] + r.m10 * lz[c] + r.tz;
        x0 = std::min(x0, x), x1 = std::max(x1, x), z0 = std::min(z0, z), z1 = std::max(z1, z);
    }
}
// The lamps of the bake snapshot that can shade this lot's ground (in the bake now, their rect over the lot's box), the
// strongest kMaxLamps by their stamp weight at the box's nearest point
std::vector<GroundShadow::Lamp> ShadeLamps(const LotRect& r) {
    float x0, z0, x1, z1;
    LotBox(r, x0, z0, x1, z1);
    std::vector<std::pair<float, GroundShadow::Lamp>> c;
    for (const LotLightBridge::BakeLamp& b : LotLightBridge::CurrentBakeLamps().lamps) {
        if (!b.baked || b.rect[0] > x1 || b.rect[2] < x0 || b.rect[1] > z1 || b.rect[3] < z0) continue;
        const float lum = b.light[0] * 0.2126f + b.light[1] * 0.7152f + b.light[2] * 0.0722f;
        GroundShadow::Lamp L{b.pos[0], b.pos[1], b.pos[2], 0.4f * lum};
        if (!(L.k > 0.0f) || !std::isfinite(L.k) || !std::isfinite(L.x) || !std::isfinite(L.y) || !std::isfinite(L.z)) continue;
        const float nx = std::clamp(L.x, x0, x1), nz = std::clamp(L.z, z0, z1);
        const float wgt = GroundShadow::Weight(L, nx, r.ty, nz);
        if (!(wgt >= GroundShadow::kMinWeight)) continue;
        c.emplace_back(wgt, L);
    }
    std::stable_sort(c.begin(), c.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    if (c.size() > static_cast<size_t>(GroundShadow::kMaxLamps)) c.resize(GroundShadow::kMaxLamps);
    std::vector<GroundShadow::Lamp> out;
    out.reserve(c.size());
    for (const auto& [w, L] : c) out.push_back(L);
    return out;
}
// The grid the shade is built on (15/10, F8/F7 2026-10-14 23-27: a wall's shadow stopped at the lot's edge and the lamp light
// that reaches past the lot lay over it; the world grass takes the bake lamps from the terrain map, which knows no wall): the
// lot's grid (w x h texels, 4 a metre) grown on each side as far as its lamps reach past it (GroundShadow::Reach), at most
// kShadeMargin; the lot's texels start at (ox, oz) on it, at the same texel centres
constexpr float kShadeMargin = 24.0f; // m
GroundShadow::Grid ShadeGrid(const LotRect& r, int w, int h, const std::vector<GroundShadow::Lamp>& lamps, int& ox, int& oz) {
    GroundShadow::Grid g;
    g.m0 = r.m0, g.m2 = r.m2, g.m8 = r.m8, g.m10 = r.m10, g.tx = r.tx, g.tz = r.tz, g.ty = r.ty;
    float lx0 = 0, lz0 = 0, lx1 = r.w, lz1 = r.d; // lot-local metres
    const double det = static_cast<double>(r.m0) * r.m10 - static_cast<double>(r.m8) * r.m2;
    if (std::fabs(det) > 1e-6)
        for (const GroundShadow::Lamp& L : lamps) {
            const double dx = L.x - r.tx, dz = L.z - r.tz;
            const float lx = static_cast<float>((r.m10 * dx - r.m8 * dz) / det), lz = static_cast<float>((-r.m2 * dx + r.m0 * dz) / det);
            const float R = GroundShadow::Reach(L, r.ty);
            if (!std::isfinite(lx) || !std::isfinite(lz) || !(R > 0.0f)) continue;
            lx0 = std::min(lx0, lx - R), lz0 = std::min(lz0, lz - R), lx1 = std::max(lx1, lx + R), lz1 = std::max(lz1, lz + R);
        }
    const auto texels = [](float m) { return static_cast<int>(std::ceil(std::clamp(m, 0.0f, kShadeMargin) * GroundShadow::kTexelsPerMetre)); };
    ox = texels(-lx0);
    oz = texels(-lz0);
    g.w = w + ox + texels(lx1 - r.w);
    g.h = h + oz + texels(lz1 - r.d);
    const float sx = -ox / GroundShadow::kTexelsPerMetre, sz = -oz / GroundShadow::kTexelsPerMetre;
    g.tx = r.tx + r.m0 * sx + r.m8 * sz;
    g.tz = r.tz + r.m2 * sx + r.m10 * sz;
    return g;
}
// A grid's world box
void GridBox(const GroundShadow::Grid& g, float& x0, float& z0, float& x1, float& z1) {
    x0 = z0 = 1e30f;
    x1 = z1 = -1e30f;
    const float W = g.w / GroundShadow::kTexelsPerMetre, H = g.h / GroundShadow::kTexelsPerMetre;
    const float lx[4] = {0, W, 0, W}, lz[4] = {0, 0, H, H};
    for (int c = 0; c < 4; c++) {
        const float x = g.m0 * lx[c] + g.m8 * lz[c] + g.tx, z = g.m2 * lx[c] + g.m10 * lz[c] + g.tz;
        x0 = std::min(x0, x), x1 = std::max(x1, x), z0 = std::min(z0, z), z1 = std::max(z1, z);
    }
}
// The box the walls that can shade the lot lie in: the lot, the ground around it the shade covers and its lamps' heads
void ShadeBox(const LotRect& r, const std::vector<GroundShadow::Lamp>& lamps, float& x0, float& z0, float& x1, float& z1) {
    LotBox(r, x0, z0, x1, z1);
    for (const GroundShadow::Lamp& L : lamps) x0 = std::min(x0, L.x), x1 = std::max(x1, L.x), z0 = std::min(z0, L.z), z1 = std::max(z1, L.z);
    int ox, oz;
    const GroundShadow::Grid g = ShadeGrid(r, static_cast<int>(std::lround(r.w * GroundShadow::kTexelsPerMetre)),
                                           static_cast<int>(std::lround(r.d * GroundShadow::kTexelsPerMetre)), lamps, ox, oz);
    float gx0, gz0, gx1, gz1;
    GridBox(g, gx0, gz0, gx1, gz1);
    x0 = std::min(x0, gx0), z0 = std::min(z0, gz0), x1 = std::max(x1, gx1), z1 = std::max(z1, gz1);
}

// ---- Garden walls and fences shade the ground too (15/10: a brick garden wall let the post's light over the grass
// behind it). They are fences for the game (FenceRenderManager), not in the outside-wall copy, and only their instanced
// draws know where they stand: each piece is an instance (POSITION1 = its place, POSITION2 = (cos, slope, sin) of its
// turn; the vertex shader's world = (px c - pz s, py + px k, px s + pz c) + place, VS_21C41B90 of F7 2026-10-13 11-50-47)
// of one mesh (POSITION0 / w). The mesh's box gives each piece a wall: along its longer side, from its base to its top, so
// a low wall casts a short shadow and a tall one a long one (ShadeJob tests the lamp over each wall's top). Posts (under
// 0.3 m both ways) and thick pieces (stairs, decks: over 0.6 m deep) are left out. Render thread (DrawInstanced notes the
// groups it draws, read again every 5 s; a group not drawn for 5 min is dropped).
struct FenceGroup {
    std::vector<GroundShadow::Wall> walls;
    float x0 = 0, z0 = 0, x1 = 0, z1 = 0;
    DWORD read = 0, seen = 0;
};
std::unordered_map<uint64_t, FenceGroup> g_fenceGroups;
std::unordered_map<uint64_t, uint8_t> g_fenceBusy; // draw_reads_nowait: busy answers per fence group since its last read
// mesh boxes by buffer, range and layout (a buffer address can be reused: emptied with the lots and on a world change)
std::unordered_map<uint64_t, std::array<float, 7>> g_meshBoxes;
std::atomic<long> g_fenceGroupReads{0}, g_fenceWalls{0};
UINT g_curDipBase = 0, g_curDipMin = 0, g_curDipNum = 0; // the current DrawIndexedPrimitive's vertex range (0 vertices: DrawPrimitive)
// The fence walls in a world box
// (15/10) an 8 m grid of the fence walls, made again only when a group changed (each lot's check walked every group)
uint32_t g_fenceGen = 1, g_fenceGridGen = 0;
constexpr float kFenceCell = 8.0f;
std::vector<GroundShadow::Wall> g_fenceAll;
std::unordered_map<uint64_t, std::vector<uint32_t>> g_fenceCells;
uint64_t FenceCellKey(int x, int z) { return (static_cast<uint64_t>(static_cast<uint32_t>(x)) << 32) | static_cast<uint32_t>(z); }
void FenceWallsIn(float x0, float z0, float x1, float z1, std::vector<GroundShadow::Wall>& out) {
    if (g_fenceGridGen != g_fenceGen) {
        g_fenceGridGen = g_fenceGen;
        g_fenceAll.clear();
        g_fenceCells.clear();
        for (const auto& [k, g] : g_fenceGroups)
            for (const GroundShadow::Wall& w : g.walls) {
                const uint32_t i = static_cast<uint32_t>(g_fenceAll.size());
                g_fenceAll.push_back(w);
                const int cx = static_cast<int>(std::floor(0.5f * (w.x0 + w.x1) / kFenceCell)), cz = static_cast<int>(std::floor(0.5f * (w.z0 + w.z1) / kFenceCell));
                g_fenceCells[FenceCellKey(cx, cz)].push_back(i);
            }
    }
    if (g_fenceAll.empty()) return;
    // (pieces are at most 8 m long: their middle is within one cell of any point of them)
    const int i0 = static_cast<int>(std::floor(x0 / kFenceCell)) - 1, i1 = static_cast<int>(std::floor(x1 / kFenceCell)) + 1;
    const int j0 = static_cast<int>(std::floor(z0 / kFenceCell)) - 1, j1 = static_cast<int>(std::floor(z1 / kFenceCell)) + 1;
    if (static_cast<int64_t>(i1 - i0 + 1) * (j1 - j0 + 1) > 4096) return;
    for (int i = i0; i <= i1; i++)
        for (int j = j0; j <= j1; j++) {
            const auto it = g_fenceCells.find(FenceCellKey(i, j));
            if (it == g_fenceCells.end()) continue;
            for (uint32_t k : it->second) {
                const GroundShadow::Wall& w = g_fenceAll[k];
                if (!(std::max(w.x0, w.x1) < x0 || std::min(w.x0, w.x1) > x1 || std::max(w.z0, w.z1) < z0 || std::min(w.z0, w.z1) > z1)) out.push_back(w);
            }
        }
}
// The walls that shade a lot's ground in a box: the outside walls' copy (with their openings) and the garden walls and fences
void ShadeWalls(float x0, float z0, float x1, float z1, std::vector<GroundShadow::Wall>& walls, std::vector<WallOpenings::Patch>& patches, int* fences = nullptr) {
    LevelLightShare::OutsideWallsIn(x0, z0, x1, z1, walls, &patches);
    const size_t before = walls.size();
    FenceWallsIn(x0, z0, x1, z1, walls);
    if (fences) *fences = static_cast<int>(walls.size() - before);
}
// One element of a vertex as 4 floats (types the instanced fences use; false for others)
bool ReadElement(const BYTE* p, BYTE type, float out[4]) {
    out[0] = out[1] = out[2] = 0.0f;
    out[3] = 1.0f;
    switch (type) {
    case D3DDECLTYPE_FLOAT3: std::memcpy(out, p, 12); return true;
    case D3DDECLTYPE_FLOAT4: std::memcpy(out, p, 16); return true;
    case D3DDECLTYPE_SHORT4: {
        const int16_t* s = reinterpret_cast<const int16_t*>(p);
        for (int i = 0; i < 4; i++) out[i] = s[i];
        return true;
    }
    case D3DDECLTYPE_SHORT4N: {
        const int16_t* s = reinterpret_cast<const int16_t*>(p);
        for (int i = 0; i < 4; i++) out[i] = std::max(s[i] / 32767.0f, -1.0f);
        return true;
    }
    case D3DDECLTYPE_FLOAT16_4: {
        const uint16_t* hf = reinterpret_cast<const uint16_t*>(p);
        for (int i = 0; i < 4; i++) {
            const uint32_t h = hf[i], sign = (h >> 15) & 1, ex = (h >> 10) & 0x1F, man = h & 0x3FF;
            float f = ex == 0 ? std::ldexp(static_cast<float>(man), -24) : ex == 31 ? INFINITY : std::ldexp(static_cast<float>(man | 0x400), static_cast<int>(ex) - 25);
            out[i] = sign ? -f : f;
        }
        return true;
    }
    default: return false;
    }
}
struct FenceLayout {
    WORD posOff = 0, placeOff = 0, turnOff = 0;
    BYTE posType = 0, placeType = 0, turnType = 0;
    bool ok = false;
};
FenceLayout ReadFenceLayout(IDirect3DDevice9* dev) {
    FenceLayout f;
    IDirect3DVertexDeclaration9* decl = nullptr;
    if (FAILED(dev->GetVertexDeclaration(&decl)) || !decl) return f;
    D3DVERTEXELEMENT9 el[MAXD3DDECLLENGTH + 1];
    UINT n = MAXD3DDECLLENGTH + 1;
    bool pos = false, place = false, turn = false;
    if (SUCCEEDED(decl->GetDeclaration(el, &n)))
        for (UINT i = 0; i < n && el[i].Stream != 0xFF; i++) {
            if (el[i].Usage != D3DDECLUSAGE_POSITION) continue;
            if (el[i].Stream == 0 && el[i].UsageIndex == 0) f.posOff = el[i].Offset, f.posType = el[i].Type, pos = true;
            if (el[i].Stream == 1 && el[i].UsageIndex == 1) f.placeOff = el[i].Offset, f.placeType = el[i].Type, place = true;
            if (el[i].Stream == 1 && el[i].UsageIndex == 2) f.turnOff = el[i].Offset, f.turnType = el[i].Type, turn = true;
        }
    decl->Release();
    f.ok = pos && place && turn;
    return f;
}
// POD only (SEH): the mesh's box over its vertex range
bool MeshBox(const BYTE* p, UINT count, UINT stride, const FenceLayout& L, float lo[3], float hi[3]) {
    __try {
        lo[0] = lo[1] = lo[2] = 1e30f;
        hi[0] = hi[1] = hi[2] = -1e30f;
        for (UINT i = 0; i < count; i++) {
            float v[4];
            if (!ReadElement(p + static_cast<size_t>(i) * stride + L.posOff, L.posType, v) || !(std::fabs(v[3]) > 1e-6f)) return false;
            for (int k = 0; k < 3; k++) {
                const float c = v[k] / v[3];
                if (!std::isfinite(c)) return false;
                lo[k] = std::min(lo[k], c), hi[k] = std::max(hi[k], c);
            }
        }
        return count > 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// Opaque instanced structures retain their triangles, rather than becoming a filled ramp or box.
// Collection is bounded and reuses the declaration/packing checked for instanced garden walls.
struct SolidGroup {std::vector<MeshOcclusion::Triangle> triangles;DWORD read=0,seen=0;double priority=0;};
std::unordered_map<uint64_t,SolidGroup> g_solidGroups;
std::unordered_map<uint64_t,uint64_t> g_solidGeometry;
std::unordered_map<uint64_t,DWORD> g_solidAttempts;
std::unordered_map<IDirect3DPixelShader9*,bool> g_solidPs;
constexpr size_t kSolidTriangleLimit=8192;
size_t g_solidTriangles=0;
DWORD g_solidDirty=0,g_solidPublished=0;
uint32_t g_solidReset=0;
unsigned g_solidReadBudget=1;
unsigned g_solidNotes=0;
unsigned g_solidReplaced=0,g_solidRejected=0;
DWORD g_solidNextScan=0;
bool g_solidScanning=false;
bool g_solidSteadyCleared=false; // (steady_indoor_light) the set was dropped since the switch went on (PumpSolidMeshes)
std::vector<MeshOcclusion::Triangle> g_solidLastPublished; // occlusion_relight: the last set published (PumpSolidMeshes)
// Dev switch draw_reads_nowait: a buffer the GPU still uses is not waited for inside the draw hook; the read is retried on
// a later scan frame, and after three busy answers it waits as before (every group is still read, so the result is the same).
std::unordered_map<uint64_t,uint8_t> g_solidBusy;
bool g_solidSawBusy=false;
template<class T> struct SolidRead {
    T* buffer;void* data=nullptr;bool noWait=false;
    explicit SolidRead(T* b,bool nowait=false):buffer(b),noWait(nowait){}
    bool Lock(UINT offset,UINT bytes){
        const HRESULT hr=buffer->Lock(offset,bytes,&data,D3DLOCK_READONLY|(noWait?D3DLOCK_DONOTWAIT:0));
        if(hr==D3DERR_WASSTILLDRAWING){data=nullptr;g_solidSawBusy=true;return false;}
        return SUCCEEDED(hr)&&data;
    }
    ~SolidRead(){if(data)buffer->Unlock();}
};
// FNV-1a over 64-bit words (the byte-wise loop hashed up to 147 KB per group on the render thread); only a lookup key:
// equality is still confirmed with memcmp.
uint64_t SolidGeometryHash(const void* data,size_t bytes){
    uint64_t h=1469598103934665603ull;const auto* p=static_cast<const BYTE*>(data);size_t i=0;
    for(;i+8<=bytes;i+=8){uint64_t w;std::memcpy(&w,p+i,8);h=(h^w)*1099511628211ull;}
    for(;i<bytes;++i)h=(h^p[i])*1099511628211ull;
    return h;
}
UINT SolidElementBytes(BYTE type){
    switch(type){case D3DDECLTYPE_FLOAT3:return 12;case D3DDECLTYPE_FLOAT4:return 16;
    case D3DDECLTYPE_SHORT4:case D3DDECLTYPE_SHORT4N:case D3DDECLTYPE_FLOAT16_4:return 8;default:return 0;}
}
bool SolidPixelShader(){
    if(!g_curPs)return false;
    if(auto i=g_solidPs.find(g_curPs);i!=g_solidPs.end())return i->second;
    bool opaque=true;UINT bytes=0;std::vector<DWORD> code;
    if(FAILED(g_curPs->GetFunction(nullptr,&bytes))||bytes<8||bytes>65536||bytes%4)opaque=false;
    else{code.resize(bytes/4);if(FAILED(g_curPs->GetFunction(code.data(),&bytes)))opaque=false;}
    for(size_t at=1;opaque&&at<code.size();){
        const auto op=code[at]&0xFFFFu;if(op==0xFFFFu)break;
        const size_t count=op==0xFFFEu?1+((code[at]>>16)&0x7FFFu):1+((code[at]>>24)&15u);
        if(op==0x41u||!count||at+count>code.size()){opaque=false;break;}at+=count;
    }
    if(g_solidPs.size()<512)g_solidPs.emplace(g_curPs,opaque);return opaque;
}
void NoteSolidMeshImpl(IDirect3DDevice9* dev,D3DPRIMITIVETYPE type,INT baseVertex,UINT minVertex,UINT vertices,UINT start,UINT primitives,bool instanced){
    if(!kMemoryOptimizationsEnabled||g_inOwnCall||!g_solidReadBudget||!instanced||type!=D3DPT_TRIANGLELIST||!vertices||vertices>4096||!primitives||primitives>4096)return;
    DWORD blend=0,alpha=0,depth=0;UINT frequency=0,step=0;
    if(FAILED(dev->GetRenderState(D3DRS_ALPHABLENDENABLE,&blend))||FAILED(dev->GetRenderState(D3DRS_ALPHATESTENABLE,&alpha))||
       FAILED(dev->GetRenderState(D3DRS_ZWRITEENABLE,&depth))||blend||alpha||!depth)return;
    if(FAILED(dev->GetStreamSourceFreq(0,&frequency))||!(frequency&D3DSTREAMSOURCE_INDEXEDDATA)||
       FAILED(dev->GetStreamSourceFreq(1,&step))||step!=(D3DSTREAMSOURCE_INSTANCEDATA|1u))return;
    const UINT instances=frequency&0x3FFFFFFFu;if(!instances||static_cast<uint64_t>(instances)*primitives>4096)return;
    using Microsoft::WRL::ComPtr;
    ComPtr<IDirect3DVertexBuffer9> vb,instanceBuffer;ComPtr<IDirect3DIndexBuffer9> indices;
    UINT offset=0,stride=0,instanceOffset=0,instanceStride=0;
    if(FAILED(dev->GetStreamSource(0,vb.GetAddressOf(),&offset,&stride))||!vb||
       FAILED(dev->GetStreamSource(1,instanceBuffer.GetAddressOf(),&instanceOffset,&instanceStride))||!instanceBuffer||
       FAILED(dev->GetIndices(indices.GetAddressOf()))||!indices)return;
    uint64_t key=1469598103934665603ull;
    for(uint64_t v:{static_cast<uint64_t>(reinterpret_cast<uintptr_t>(vb.Get())),static_cast<uint64_t>(reinterpret_cast<uintptr_t>(instanceBuffer.Get())),
        static_cast<uint64_t>(reinterpret_cast<uintptr_t>(indices.Get())),static_cast<uint64_t>(offset),static_cast<uint64_t>(instanceOffset),
        static_cast<uint64_t>(static_cast<uint32_t>(baseVertex)),static_cast<uint64_t>(minVertex),static_cast<uint64_t>(vertices),static_cast<uint64_t>(start),static_cast<uint64_t>(primitives),
        static_cast<uint64_t>(stride),static_cast<uint64_t>(instanceStride),static_cast<uint64_t>(instances),static_cast<uint64_t>(reinterpret_cast<uintptr_t>(g_curPs))})key=(key^v)*1099511628211ull;
    const DWORD now=GetTickCount();
    auto found=g_solidGroups.find(key);
    if(found!=g_solidGroups.end()){found->second.seen=now;if(now-found->second.read<5000)return;}
    // Rejected meshes also yield the next frame's read slot to later draws.
    if(auto attempt=g_solidAttempts.find(key);attempt!=g_solidAttempts.end()&&now-attempt->second<5000)return;
    if(!g_solidReadBudget)return;--g_solidReadBudget;
    if(g_solidAttempts.size()>=512&&!g_solidAttempts.contains(key)){
        auto oldest=g_solidAttempts.begin();
        for(auto it=g_solidAttempts.begin();it!=g_solidAttempts.end();++it)if(now-it->second>now-oldest->second)oldest=it;
        g_solidAttempts.erase(oldest);
    }
    g_solidAttempts[key]=now;
    if(!SolidPixelShader())return;
    const auto layout=ReadFenceLayout(dev);
    const UINT positionBytes=SolidElementBytes(layout.posType),placeBytes=SolidElementBytes(layout.placeType),turnBytes=SolidElementBytes(layout.turnType);
    if(!layout.ok||!positionBytes||!placeBytes||!turnBytes||!stride||stride>256||!instanceStride||instanceStride>256||
       layout.posOff+positionBytes>stride||layout.placeOff+placeBytes>instanceStride||layout.turnOff+turnBytes>instanceStride)return;
    D3DVERTEXBUFFER_DESC vd{},id{};D3DINDEXBUFFER_DESC indexDesc{};
    if(FAILED(vb->GetDesc(&vd))||FAILED(instanceBuffer->GetDesc(&id))||FAILED(indices->GetDesc(&indexDesc)))return;
    const UINT indexBytes=indexDesc.Format==D3DFMT_INDEX16?2:indexDesc.Format==D3DFMT_INDEX32?4:0;
    const int64_t first=static_cast<int64_t>(baseVertex)+minVertex;
    if(!indexBytes||first<0)return;
    const uint64_t vertexAt=offset+static_cast<uint64_t>(first)*stride,vertexBytes=static_cast<uint64_t>(vertices)*stride;
    const uint64_t indexAt=static_cast<uint64_t>(start)*indexBytes,indexSize=static_cast<uint64_t>(primitives)*3*indexBytes;
    const uint64_t instanceBytes=static_cast<uint64_t>(instances)*instanceStride;
    if(vertexAt+vertexBytes>vd.Size||indexAt+indexSize>indexDesc.Size||instanceOffset+instanceBytes>id.Size)return;
    const bool nowait=DevSwitches::DrawReadsNoWait();
    // A full busy table (groups no longer drawn keep their entries until a reset) waits as before: a group whose count can
    // no longer grow would otherwise never be read
    const bool busyFull=g_solidBusy.size()>=1024&&!g_solidBusy.contains(key);
    const bool tryNoWait=nowait&&!busyFull&&!(g_solidBusy.contains(key)&&g_solidBusy[key]>=3);
    g_solidSawBusy=false;
    const auto busy=[&]{
        if(!g_solidSawBusy)return;
        g_solidAttempts.erase(key); // retried on the next scan frame, not in 5 s
        ++g_solidBusy[key]; // only reached with tryNoWait, so the table is below 1024 or already holds the key
    };
    SolidRead vr(vb.Get(),tryNoWait);if(!vr.Lock(static_cast<UINT>(vertexAt),static_cast<UINT>(vertexBytes))){busy();return;}
    float lo[3],hi[3];
    if(!MeshBox(static_cast<const BYTE*>(vr.data),vertices,stride,layout,lo,hi))return;
    SolidRead ir(indices.Get(),tryNoWait);SolidRead sr(instanceBuffer.Get(),tryNoWait);
    if(!ir.Lock(static_cast<UINT>(indexAt),static_cast<UINT>(indexSize))||!sr.Lock(instanceOffset,static_cast<UINT>(instanceBytes))){busy();return;}
    if(nowait)g_solidBusy.erase(key);
    std::vector<MeshOcclusion::Triangle> triangles;triangles.reserve(static_cast<size_t>(instances)*primitives);
    std::vector<MeshOcclusion::Vec> points(vertices);
    // draw_reads_nowait: each vertex is read once, not once per instance (ReadElement is instance-independent; Decode
    // gets the very same inputs, so the triangles are bit-identical)
    std::vector<std::array<float,4>> positions;
    if(nowait){
        positions.resize(vertices);
        for(UINT i=0;i<vertices;++i)if(!ReadElement(static_cast<const BYTE*>(vr.data)+i*stride+layout.posOff,layout.posType,positions[i].data()))return;
    }
    float placementLow=1e30f,placementHigh=-1e30f;
    for(UINT instance=0;instance<instances;++instance){
        const auto* instanceData=static_cast<const BYTE*>(sr.data)+instance*instanceStride;float place[4],turn[4];
        if(!ReadElement(instanceData+layout.placeOff,layout.placeType,place)||!ReadElement(instanceData+layout.turnOff,layout.turnType,turn))return;
        placementLow=std::min(placementLow,place[1]);placementHigh=std::max(placementHigh,place[1]);
        if(nowait){
            for(UINT i=0;i<vertices;++i)if(!MeshOcclusion::Decode(positions[i].data(),place,turn,points[i]))return;
        }else for(UINT i=0;i<vertices;++i){float p[4];
            if(!ReadElement(static_cast<const BYTE*>(vr.data)+i*stride+layout.posOff,layout.posType,p)||!MeshOcclusion::Decode(p,place,turn,points[i]))return;
        }
        for(UINT i=0;i<primitives;++i){UINT index[3];
            for(UINT k=0;k<3;++k){const UINT raw=indexBytes==2?static_cast<const uint16_t*>(ir.data)[3*i+k]:static_cast<const uint32_t*>(ir.data)[3*i+k];
                if(raw<minVertex||raw-minVertex>=vertices)return;index[k]=raw-minVertex;}
            triangles.push_back({points[index[0]],points[index[1]],points[index[2]]});
        }
    }
    uint64_t geometry=1469598103934665603ull;
    if(nowait)geometry=SolidGeometryHash(triangles.data(),triangles.size()*sizeof(triangles[0]));
    else{const auto* bytes=reinterpret_cast<const BYTE*>(triangles.data());
        for(size_t i=0;i<triangles.size()*sizeof(triangles[0]);++i)geometry=(geometry^bytes[i])*1099511628211ull;}
    if(auto same=g_solidGeometry.find(geometry);same!=g_solidGeometry.end()&&same->second!=key){
        auto prior=g_solidGroups.find(same->second);
        if(prior!=g_solidGroups.end()&&prior->second.triangles.size()==triangles.size()&&
           !std::memcmp(prior->second.triangles.data(),triangles.data(),triangles.size()*sizeof(triangles[0]))){
            prior->second.seen=now;
            if(found!=g_solidGroups.end()){g_solidTriangles-=found->second.triangles.size();g_solidGroups.erase(found);g_solidDirty=now|1;std::erase_if(g_solidGeometry,[key](const auto& p){return p.second==key;});}
            return;
        }
    }
    const bool firstGroup=found==g_solidGroups.end();
    const size_t oldSize=firstGroup?0:found->second.triangles.size();
    const double priority=MeshBudget::Priority(triangles,placementHigh-placementLow);
    std::array<MeshBudget::Entry,128> entries{};size_t entryCount=0;
    for(const auto& [existing,group]:g_solidGroups)if(existing!=key){
        if(entryCount==entries.size())return;
        entries[entryCount++]={existing,group.triangles.size(),group.priority};
    }
    std::vector<uint64_t> victims;
    if(!MeshBudget::Plan(std::span(entries.data(),entryCount),g_solidTriangles-oldSize,triangles.size(),priority,kSolidTriangleLimit,128,victims)){
        if(g_solidRejected++<20)LOG_INFO(std::format("[MeshOcclusion] Budget deferred {} triangles, {} instances; coverage priority {:.6f}, placement rise {:.2f}; {} of {} triangles retained",
            triangles.size(),instances,priority,placementHigh-placementLow,g_solidTriangles,kSolidTriangleLimit));
        if(!firstGroup){g_solidTriangles-=oldSize;g_solidGroups.erase(found);g_solidDirty=now|1;std::erase_if(g_solidGeometry,[key](const auto& p){return p.second==key;});}
        return;
    }
    for(uint64_t victim:victims){
        const auto old=g_solidGroups.find(victim);if(old==g_solidGroups.end())continue;
        g_solidTriangles-=old->second.triangles.size();g_solidGroups.erase(old);
        std::erase_if(g_solidGeometry,[victim](const auto& p){return p.second==victim;});g_solidDirty=now|1;
    }
    if(!victims.empty()&&g_solidReplaced++<20)LOG_INFO(std::format("[MeshOcclusion] Budget replaced {} lower-coverage groups for {} triangles, {} instances; priority {:.6f}, placement rise {:.2f}",
        victims.size(),triangles.size(),instances,priority,placementHigh-placementLow));
    auto& group=g_solidGroups[key];group.read=group.seen=now;
    group.priority=priority;
    if(triangles.size()==group.triangles.size()&&(triangles.empty()||!std::memcmp(triangles.data(),group.triangles.data(),triangles.size()*sizeof(triangles[0]))))return;
    g_solidTriangles=g_solidTriangles-group.triangles.size()+triangles.size();group.triangles=std::move(triangles);g_solidDirty=now|1;
    std::erase_if(g_solidGeometry,[key](const auto& p){return p.second==key;});g_solidGeometry[geometry]=key;
    if(firstGroup&&g_solidNotes<20&&!group.triangles.empty()){
        ++g_solidNotes;const auto p=group.triangles.front().a;
        LOG_INFO(std::format("[MeshOcclusion] Accepted {} triangles, {} instances near ({:.2f}, {:.2f}, {:.2f}); local horizontal spans {:.3f}/{:.3f}, vertex usage {:#x}, instance stride {}",
            group.triangles.size(),instances,p.x,p.y,p.z,hi[0]-lo[0],hi[2]-lo[2],vd.Usage,instanceStride));
    }
}
void NoteSolidMesh(IDirect3DDevice9* dev,D3DPRIMITIVETYPE type,INT base,UINT min,UINT vertices,UINT start,UINT primitives,bool instanced){
    try{NoteSolidMeshImpl(dev,type,base,min,vertices,start,primitives,instanced);}catch(...){/* A failed optional read keeps the existing floor/wall tests. */}
}
void PumpSolidMeshes(){
    if(!kMemoryOptimizationsEnabled)return;
    const DWORD now=GetTickCount();
    const auto reset=MeshOcclusion::reset.load(std::memory_order_acquire);
    if(reset!=g_solidReset){g_solidGroups.clear();g_solidGeometry.clear();g_solidAttempts.clear();g_solidBusy.clear();g_solidPs.clear();g_solidTriangles=0;g_solidDirty=0;g_solidReset=reset;g_solidNextScan=0;g_solidScanning=false;g_solidReplaced=g_solidRejected=0;}
    // (steady_indoor_light) No indoor pass reads the mesh set (MeshOcclusion::Blocked) and no publish relights: nothing is
    // read in the draw hooks (budget 0: NoteSolidMeshImpl returns at once) and nothing is published. On the switch the set is
    // dropped once (the reset above clears the groups on the next pump); switched off again, discovery starts over.
    if(DevSwitches::SteadyIndoorLight()){
        g_solidReadBudget=0;g_solidScanning=false;
        if(!g_solidSteadyCleared){
            MeshOcclusion::Reset();
            g_solidLastPublished.clear();g_solidLastPublished.shrink_to_fit();
            g_solidSteadyCleared=true;
        }
        return;
    }
    g_solidSteadyCleared=false;
    // Once every visible group is cached, discovery need not query device state on every frame.
    if(g_solidScanning)g_solidNextScan=now+(g_solidReadBudget==1?100:25);
    g_solidReadBudget=!g_solidNextScan||static_cast<int32_t>(now-g_solidNextScan)>=0?1:0;
    g_solidScanning=g_solidReadBudget!=0;
    bool retired=false;
    for(auto it=g_solidGroups.begin();it!=g_solidGroups.end();)if(now-it->second.seen>300000){g_solidTriangles-=it->second.triangles.size();it=g_solidGroups.erase(it);g_solidDirty=now|1;retired=true;}else ++it;
    if(retired)std::erase_if(g_solidGeometry,[](const auto& p){return !g_solidGroups.contains(p.second);});
    if(!g_solidDirty||now-g_solidDirty<500||now-g_solidPublished<2000)return;
    try{
        std::vector<MeshOcclusion::Triangle> triangles;triangles.reserve(g_solidTriangles);
        if(DevSwitches::OcclusionRelight()){
            // occlusion_relight: groups in key order, so the same set always gives the same array; an unchanged set is not
            // published again (a publish bumps the occlusion generation, which relights every lot's indoor light)
            std::vector<uint64_t> keys;keys.reserve(g_solidGroups.size());
            for(const auto& [key,group]:g_solidGroups){(void)group;keys.push_back(key);}
            std::sort(keys.begin(),keys.end());
            for(uint64_t key:keys){const auto& group=g_solidGroups[key];triangles.insert(triangles.end(),group.triangles.begin(),group.triangles.end());}
            static uint32_t s_lastReset=0;
            if(s_lastReset==g_solidReset&&triangles.size()==g_solidLastPublished.size()&&
               (triangles.empty()||!std::memcmp(triangles.data(),g_solidLastPublished.data(),triangles.size()*sizeof(triangles[0])))){
                g_solidPublished=now;g_solidDirty=0;
                static unsigned s_skips=0;
                if(s_skips++<20)LOG_INFO(std::format("[MeshOcclusion] Unchanged set of {} triangles from {} groups not published again (no relight)",triangles.size(),g_solidGroups.size()));
                return;
            }
            g_solidLastPublished=triangles;s_lastReset=g_solidReset;
        }else for(const auto& [key,group]:g_solidGroups){(void)key;triangles.insert(triangles.end(),group.triangles.begin(),group.triangles.end());}
        MeshOcclusion::Publish(std::move(triangles));g_solidPublished=now;g_solidDirty=0;
        LOG_INFO(std::format("[MeshOcclusion] Published {} triangles from {} opaque instanced groups; open gaps retain no triangles",g_solidTriangles,g_solidGroups.size()));
    }catch(...){g_solidPublished=now;}
}
// POD only (SEH): one wall per piece of the group
bool FencePieceWalls(const BYTE* p, UINT count, UINT stride, const FenceLayout& L, const float lo[3], const float hi[3], FenceGroup& g) {
    __try {
        const float ex = hi[0] - lo[0], ez = hi[2] - lo[2], ey = hi[1] - lo[1];
        const bool alongX = ex >= ez;
        const float len = alongX ? ex : ez, depth = alongX ? ez : ex;
        if (len < 0.3f || len > 8.0f || depth > 0.6f || ey < 0.25f || ey > 6.0f) return true; // a post, a thick piece or no wall
        const float a0[2] = {alongX ? lo[0] : 0.5f * (lo[0] + hi[0]), alongX ? 0.5f * (lo[2] + hi[2]) : lo[2]};
        const float a1[2] = {alongX ? hi[0] : a0[0], alongX ? a0[1] : hi[2]};
        g.x0 = g.z0 = 1e30f;
        g.x1 = g.z1 = -1e30f;
        for (UINT i = 0; i < count; i++) {
            const BYTE* r = p + static_cast<size_t>(i) * stride;
            float t[4], q[4];
            if (!ReadElement(r + L.placeOff, L.placeType, t) || !ReadElement(r + L.turnOff, L.turnType, q)) return false;
            const float c = q[0], k = q[1], s = q[2];
            if (!std::isfinite(t[0]) || !std::isfinite(t[1]) || !std::isfinite(t[2]) || !std::isfinite(c) || !std::isfinite(s) || !std::isfinite(k)) return false;
            const auto world = [&](const float l[2], float& x, float& z) {
                x = l[0] * c - l[1] * s + t[0];
                z = l[0] * s + l[1] * c + t[2];
            };
            GroundShadow::Wall w{};
            world(a0, w.x0, w.z0);
            world(a1, w.x1, w.z1);
            const float lift = k * 0.5f * (a0[0] + a1[0]); // the slope at the piece's middle (a sloped run)
            w.y0 = t[1] + lo[1] + lift;
            w.y1 = t[1] + hi[1] + lift;
            g.walls.push_back(w);
            g.x0 = std::min({g.x0, w.x0, w.x1}), g.x1 = std::max({g.x1, w.x0, w.x1});
            g.z0 = std::min({g.z0, w.z0, w.z1}), g.z1 = std::max({g.z1, w.z0, w.z1});
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// DrawInstanced: the group being drawn (an instanced DrawIndexedPrimitive)
void NoteFenceGroup(IDirect3DDevice9* dev) {
    if (!g_shadeOn || !g_curDipNum) return;
    UINT freq = 0;
    if (FAILED(dev->GetStreamSourceFreq(0, &freq)) || !(freq & D3DSTREAMSOURCE_INDEXEDDATA)) return;
    const UINT count = freq & 0x3FFFFFFFu;
    if (!count || count > 4096) return;
    IDirect3DVertexBuffer9 *vb0 = nullptr, *vb1 = nullptr;
    UINT off0 = 0, stride0 = 0, off1 = 0, stride1 = 0;
    if (FAILED(dev->GetStreamSource(0, &vb0, &off0, &stride0)) || !vb0) return;
    if (FAILED(dev->GetStreamSource(1, &vb1, &off1, &stride1)) || !vb1) {
        vb0->Release();
        return;
    }
    const uint64_t key = (static_cast<uint64_t>(reinterpret_cast<uintptr_t>(vb1)) << 32) ^ (static_cast<uint64_t>(off1) << 12) ^ (static_cast<uint64_t>(count) << 1) ^ stride1 ^
                         (static_cast<uint64_t>(reinterpret_cast<uintptr_t>(vb0)) << 7) ^ (static_cast<uint64_t>(g_curDipBase + g_curDipMin) << 40) ^ g_curDipNum;
    const DWORD now = GetTickCount();
    FenceGroup& g = g_fenceGroups[key];
    g.seen = now ? now : 1;
    if (!g.read || now - g.read > 60000) {
        // draw_reads_nowait: the 60 s refresh of a group already read does not wait for the GPU; while its instance buffer is
        // busy the walls read last time stay in use and the next draw tries again (three busy answers: it waits, as before)
        if (DevSwitches::DrawReadsNoWait() && g.read && !g.walls.empty() && stride1 >= 12 && stride1 <= 256 && g_fenceBusy.size() < 4096) {
            uint8_t& busy = g_fenceBusy[key];
            if (busy < 3) {
                void* probe = nullptr;
                const HRESULT hr = vb1->Lock(off1, count * stride1, &probe, D3DLOCK_READONLY | D3DLOCK_DONOTWAIT);
                if (probe) vb1->Unlock();
                if (hr == D3DERR_WASSTILLDRAWING) {
                    ++busy;
                    vb0->Release();
                    vb1->Release();
                    return;
                }
            }
            busy = 0;
        }
        g.read = now ? now : 1;
        g_fenceGroupReads.fetch_add(1, std::memory_order_relaxed);
        const FenceLayout L = ReadFenceLayout(dev);
        FenceGroup fresh;
        fresh.read = g.read, fresh.seen = g.seen;
        float lo[3] = {}, hi[3] = {};
        void *p0 = nullptr, *p1 = nullptr;
        const UINT first = g_curDipBase + g_curDipMin;
        // the mesh's box, read once per mesh (15/10: a locked buffer the GPU draws from makes the CPU wait)
        if (g_meshBoxes.size() > 4096) g_meshBoxes.clear();
        const uint64_t meshKey = (static_cast<uint64_t>(reinterpret_cast<uintptr_t>(vb0)) << 32) ^ (static_cast<uint64_t>(off0 + first * stride0) << 8) ^ g_curDipNum ^
                                 (static_cast<uint64_t>(stride0) << 56) ^ (static_cast<uint64_t>(L.posType) << 50) ^ L.posOff;
        bool ok = L.ok && stride0 >= 12 && stride0 <= 256 && stride1 >= 12 && stride1 <= 256 && g_curDipNum <= 65536;
        if (ok) {
            if (const auto mb = g_meshBoxes.find(meshKey); mb != g_meshBoxes.end()) {
                ok = mb->second[6] > 0.5f;
                for (int k = 0; k < 3; k++) lo[k] = mb->second[k], hi[k] = mb->second[3 + k];
            } else {
                ok = SUCCEEDED(vb0->Lock(off0 + first * stride0, g_curDipNum * stride0, &p0, D3DLOCK_READONLY)) && p0 &&
                     MeshBox(static_cast<const BYTE*>(p0), g_curDipNum, stride0, L, lo, hi);
                if (p0) vb0->Unlock();
                g_meshBoxes[meshKey] = {lo[0], lo[1], lo[2], hi[0], hi[1], hi[2], ok ? 1.0f : 0.0f};
            }
        }
        ok = ok && SUCCEEDED(vb1->Lock(off1, count * stride1, &p1, D3DLOCK_READONLY)) && p1 && FencePieceWalls(static_cast<const BYTE*>(p1), count, stride1, L, lo, hi, fresh);
        if (p1) vb1->Unlock();
        if (!ok) fresh.walls.clear();
        g_fenceWalls.fetch_add(static_cast<long>(fresh.walls.size()) - static_cast<long>(g.walls.size()), std::memory_order_relaxed);
        const bool newGroup = g.walls.empty() && !fresh.walls.empty();
        if (fresh.walls.size() != g.walls.size() || (!fresh.walls.empty() && std::memcmp(fresh.walls.data(), g.walls.data(), fresh.walls.size() * sizeof(GroundShadow::Wall)) != 0))
            g_fenceGen++;
        g = std::move(fresh);
        // (15/10: the ground took minutes to change after a fence was removed) a new group replaces at once the groups over the
        // same place that are not drawn any more (the game makes a group again when a piece is added or removed)
        if (newGroup) {
            const FenceGroup& ng = g;
            for (auto it = g_fenceGroups.begin(); it != g_fenceGroups.end();) {
                const FenceGroup& o = it->second;
                const bool stale = it->first != key && now - o.seen > 500 && // (review: 100 ms let two groups erase each other below 10 fps)
                                    !(o.x1 < ng.x0 - 0.5f || o.x0 > ng.x1 + 0.5f || o.z1 < ng.z0 - 0.5f || o.z0 > ng.z1 + 0.5f);
                if (stale) {
                    g_fenceWalls.fetch_sub(static_cast<long>(o.walls.size()), std::memory_order_relaxed);
                    g_fenceGen++;
                    it = g_fenceGroups.erase(it);
                } else ++it;
            }
        }
    }
    vb0->Release();
    vb1->Release();
    // groups not drawn for 20 s go (a fence removed whole; at most once a second)
    static DWORD s_swept = 0;
    if (now - s_swept > 1000) {
        s_swept = now;
        for (auto it = g_fenceGroups.begin(); it != g_fenceGroups.end();)
            if (now - it->second.seen > 20000 || g_fenceGroups.size() > 4096) {
                g_fenceWalls.fetch_sub(static_cast<long>(it->second.walls.size()), std::memory_order_relaxed);
                if (!it->second.walls.empty()) g_fenceGen++;
                it = g_fenceGroups.erase(it);
            } else ++it;
    }
}
// What the shade is made from: the lot's place and grid, its lamps (place; light in about 20% steps) and the outside walls in
// its box (their copy, so walls changed on another lot do not build this one again). Render thread.
uint64_t ShadeKey(const LotRect& r, int w, int h) {
    uint64_t s = 1469598103934665603ull;
    const auto mix = [&s](uint64_t v) { s = (s ^ v) * 1099511628211ull; };
    const auto mixf = [&](float f, float q) { mix(static_cast<uint64_t>(static_cast<int64_t>(std::lround(f * q)))); };
    mix(static_cast<uint64_t>(w) << 32 | static_cast<uint32_t>(h));
    for (float f : {r.tx, r.tz, r.ty, r.m0, r.m2, r.m8, r.m10}) mixf(f, 100.0f);
    mixf(Penumbra::Radius(), 1000.0f); // the openings' penumbra (Shadow softness)
    mix(TesterToggles::groundSoftEdges.load(std::memory_order_relaxed) ? 0x50F7ull : 0ull); // soft wall ends and tops on the mask (GroundShadow::Grid::soft)
    const std::vector<GroundShadow::Lamp> lamps = ShadeLamps(r);
    {
        int ox, oz;
        const GroundShadow::Grid g = ShadeGrid(r, w, h, lamps, ox, oz);
        mix(static_cast<uint64_t>(g.w) << 48 | static_cast<uint64_t>(g.h) << 32 | static_cast<uint64_t>(ox) << 16 | static_cast<uint32_t>(oz));
    }
    for (const GroundShadow::Lamp& L : lamps) {
        mixf(L.x, 50.0f), mixf(L.y, 50.0f), mixf(L.z, 50.0f);
        mix(static_cast<uint64_t>(static_cast<int64_t>(std::lround(std::log2(std::max(L.k, 1e-6f)) * 4.0f)))); // about 20% steps
    }
    if (!lamps.empty()) {
        float x0, z0, x1, z1;
        ShadeBox(r, lamps, x0, z0, x1, z1);
        static std::vector<GroundShadow::Wall> walls;
        static std::vector<WallOpenings::Patch> patches;
        ShadeWalls(x0, z0, x1, z1, walls, patches);
        uint64_t sum = 0; // in any order (the copy's order follows a hash map)
        for (const GroundShadow::Wall& wl : walls) {
            uint64_t e = 1469598103934665603ull;
            for (float f : {wl.x0, wl.z0, wl.x1, wl.z1, wl.y0, wl.y1}) e = (e ^ static_cast<uint64_t>(static_cast<int64_t>(std::lround(f * 100.0f)))) * 1099511628211ull;
            // its openings and lower cells (a door placed or a grid read later builds the shade again)
            for (uint32_t k = 0; k < wl.pn && static_cast<uint64_t>(wl.p0) + k < patches.size(); k++) {
                const WallOpenings::Patch& pt = patches[wl.p0 + k];
                for (float f : {pt.u0, pt.u1, pt.top0, pt.top1, pt.ou0, pt.ou1, pt.sill, pt.head[0], pt.head[WallOpenings::kProfile - 1]})
                    e = (e ^ static_cast<uint64_t>(static_cast<int64_t>(std::lround(f * 100.0f)))) * 1099511628211ull;
                e = (e ^ (pt.open | pt.jamb0 << 1 | pt.jamb1 << 2)) * 1099511628211ull;
            }
            sum += e;
        }
        mix(sum);
        mix(walls.size());
        if (DevSwitches::CanopyGroundShadow()) { // the crowns over the lot (sorted by CrownsIn: the same set gives the same key)
            static std::vector<Canopy::Ellipsoid> crowns;
            crowns.clear();
            CrownsIn(x0, z0, x1, z1, crowns);
            for (const Canopy::Ellipsoid& e : crowns)
                for (float f : {e.cx, e.cy, e.cz, e.rx, e.ry, e.lad}) mixf(f, 10.0f);
            mix(crowns.size());
        }
    }
    return s | 1;
}
// Render thread, every frame: the worker's results applied, the next queued lots sent (at most 2 in flight, so the inputs
// stay fresh)
void ApplyLotResult(ShadeResult& r);
void ShadeTick() {
    for (ShadeResult& r : g_shadeWorker.Take()) {
        if (r.epoch != g_shadeEpoch) continue;
        if (r.chunk) g_chunkReady[r.chunkKey] = std::move(r); // uploaded at the chunk's next draw (ChunkShadeTexture)
        else {
            g_lotsInFlight = std::max(g_lotsInFlight - 1, 0);
            ApplyLotResult(r);
        }
    }
    if (!g_shadeOn) {
        g_chunkReady.clear(); // (review: results held for chunks while the shade is off)
        return;
    }
    while (g_lotsInFlight < 2 && !g_shadeQueue.empty()) {
        const ShadeWant want = g_shadeQueue.front();
        g_shadeQueue.erase(g_shadeQueue.begin());
        ShadeTask t;
        t.epoch = g_shadeEpoch;
        t.lamps = ShadeLamps(want.rect);
        t.grid = ShadeGrid(want.rect, want.w, want.h, t.lamps, t.ox, t.oz);
        t.grid.soft = TesterToggles::groundSoftEdges.load(std::memory_order_relaxed);
        float x0, z0, x1, z1;
        ShadeBox(want.rect, t.lamps, x0, z0, x1, z1);
        if (!t.lamps.empty()) ShadeWalls(x0, z0, x1, z1, t.walls, t.patches, &g_shadeLastFences);
        if (!t.lamps.empty() && DevSwitches::CanopyGroundShadow()) CrownsIn(x0, z0, x1, z1, t.crowns);
        g_shadeLastLamps = static_cast<int>(t.lamps.size());
        g_shadeLastWalls = static_cast<int>(t.walls.size());
        g_shadeLastPatches = static_cast<int>(t.patches.size());
        if (t.walls.empty() && t.crowns.empty()) t.lamps.clear(); // nothing shades: 255 everywhere at once
        t.lot = want.lot, t.key = want.key, t.w = want.w, t.h = want.h;
        g_shadeWorker.Submit(std::move(t));
        g_lotsInFlight++;
    }
}
void ApplyLotResult(ShadeResult& r) {
    g_shadeTests += r.tests;
    g_shadeThrough += r.through;
    g_shadeLastMs = r.ms;
    g_shadeMaxMs = std::max(g_shadeMaxMs, r.ms);
    const auto it = g_lotMasks.find(r.lot);
    if (it == g_lotMasks.end() || it->second.w != r.w || it->second.h != r.h) {
        g_shadeDropped.fetch_add(1, std::memory_order_relaxed); // the lot left, or its grid changed meanwhile
        return;
    }
    // (14/10) applied also when its inputs changed again meanwhile (a newer build is queued then)
    LotMask& m = it->second;
    m.shadeKey = r.key;
    if (m.queuedKey == r.key) m.queuedKey = 0;
    const GroundShadow::Grid& eg = r.grid;
    const int W = r.w, H = r.h, ox = r.ox, oz = r.oz;
    MarkLotChunkShades(m); // the chunks under the shade shown so far
    m.ext.reset();
    if (r.res.size() != static_cast<size_t>(eg.w) * eg.h || r.den.size() != r.res.size() || ox + W > eg.w || oz + H > eg.h) {
        m.shade.clear(); // (no texel: 255 everywhere)
        m.dirty = true;
        return;
    }
    // the lot's part for the lot passes; the whole grid for the world grass when it shades any texel
    std::vector<uint8_t> lotShade(static_cast<size_t>(W) * H);
    for (int j = 0; j < H; j++) std::memcpy(&lotShade[static_cast<size_t>(j) * W], &r.res[static_cast<size_t>(j + oz) * eg.w + ox], W);
    uint8_t lo = 255, extLo = 255;
    for (uint8_t v : lotShade) lo = std::min(lo, v);
    for (uint8_t v : r.res) extLo = std::min(extLo, v);
    g_shadeLastMin = lo / 255.0f;
    m.shade = std::move(lotShade);
    m.dirty = true;
    if (extLo < 255) {
        m.extGrid = eg;
        m.ext = std::make_shared<const ExtData>(ExtData{std::move(r.res), std::move(r.den)});
        GridBox(eg, m.ex0, m.ez0, m.ex1, m.ez1);
        MarkLotChunkShades(m);
    }
    g_shadeBuilt.fetch_add(1, std::memory_order_relaxed);
}
// The lot's shade is wanted with these inputs: queued (or its queued entry updated); a running build of an older key is dropped
// (14/10: the built key is set when the build is applied, the queued one apart, so a dropped or cancelled build is asked
// again; a running build is not cancelled by a key change: it finishes and the new one follows)
void WantShade(uint64_t lot, LotMask& m, const LotRect& r, DWORD now) {
    const uint64_t key = ShadeKey(r, m.w, m.h);
    if (key == m.shadeKey || key == m.queuedKey) return;
    if (m.shadeAt && now - m.shadeAt < kShadeMinEveryMs) return; // again at a later check
    m.shadeAt = now;
    for (ShadeWant& q : g_shadeQueue)
        if (q.lot == lot) {
            q = ShadeWant{lot, key, r, m.w, m.h};
            m.queuedKey = key;
            return;
        }
    if (g_shadeQueue.size() < 64) {
        g_shadeQueue.push_back(ShadeWant{lot, key, r, m.w, m.h});
        m.queuedKey = key;
        return;
    }
    m.queuedKey = 0; // (asked again at a later check)
    g_shadeDropped.fetch_add(1, std::memory_order_relaxed);
    static bool s_logged = false;
    if (!s_logged && !kPublicBuild) {
        s_logged = true;
        LOG_INFO("[LotLightBridge] Lamp shade on lot ground: the build queue is full (64 lots); more lots wait for a later check");
    }
}
// (14/10) more than kMaxLotMasks lots: the ones no draw asked for in the last 30 s go, oldest first (before, every lot's
// mask and shade went at once at 257 lots, and the shades of every visible lot flashed off together)
constexpr size_t kMaxLotMasks = 256;
void EvictLotMasks() {
    if (g_lotMasks.size() <= kMaxLotMasks) return;
    static DWORD s_evictedAt = 0; // (14/10 review: at most once a frame; each call sorts every lot)
    const DWORD frame = g_lotDrawTick.load(std::memory_order_relaxed);
    if (s_evictedAt == frame) return;
    s_evictedAt = frame;
    const DWORD now = GetTickCount();
    std::vector<std::pair<DWORD, uint64_t>> old;
    for (const auto& [lot, m] : g_lotMasks)
        if (now - m.lastUse > 30000) old.emplace_back(now - m.lastUse, lot);
    std::sort(old.begin(), old.end(), std::greater<>());
    const size_t target = kMaxLotMasks * 3 / 4;
    for (const auto& [age, lot] : old) {
        if (g_lotMasks.size() <= target) break;
        const auto it = g_lotMasks.find(lot);
        if (it->second.tex) it->second.tex->Release();
        MarkLotChunkShades(it->second);
        g_lotMasks.erase(it);
        std::erase_if(g_shadeQueue, [lot](const ShadeWant& q) { return q.lot == lot; });
    }
    if (g_lotMasks.size() > kMaxLotMasks * 2) ReleaseLotMasks(); // (every lot drawn in the last 30 s: as before, all at once)
}
// The texture from the room mask and the shade (255 inside rooms, and everywhere while no shade was built)
bool MakeMaskTexture(IDirect3DDevice9* dev, LotMask& m) {
    if (m.w <= 0 || m.h <= 0 || m.room.size() != static_cast<size_t>(m.w) * m.h) return false;
    const bool shaded = g_shadeOn && m.shade.size() == m.room.size();
    IDirect3DTexture9* tex = nullptr;
    m.dirty = false; // a failure waits for the next change or check (the room mask is read again: sig 0)
    D3DLOCKED_RECT lr;
    if (FAILED(dev->CreateTexture(m.w, m.h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, nullptr)) || FAILED(tex->LockRect(0, &lr, nullptr, 0))) {
        if (tex) tex->Release();
        m.sig = 0;
        return false;
    }
    for (int y = 0; y < m.h; y++) {
        DWORD* row = reinterpret_cast<DWORD*>(static_cast<BYTE*>(lr.pBits) + static_cast<size_t>(y) * lr.Pitch);
        for (int x = 0; x < m.w; x++) {
            const size_t i = static_cast<size_t>(y) * m.w + x;
            const DWORD l = m.room[i];
            const DWORD a = shaded && l ? m.shade[i] : 255u;
            row[x] = a << 24 | l << 16 | l << 8 | l;
        }
    }
    tex->UnlockRect(0);
    if (m.tex) m.tex->Release();
    m.tex = tex;
    m.dirty = false;
    g_maskBuilds.fetch_add(1, std::memory_order_relaxed);
    return true;
}
IDirect3DTexture9* LotMaskTexture(IDirect3DDevice9* dev, const LotRect& rect);
// rows = mask uv from the draw's terrain uv (as LotEdgeConstants, divided by the lot's size); the lot's texture or white
IDirect3DTexture9* LotMaskForImpl(IDirect3DDevice9* dev, const float k[4], const float c15[4], const float c8[4], const float c10[4], const LotRect* r, float rows[8]) {
    for (int i = 0; i < 8; i++) rows[i] = 0.0f;
    rows[2] = rows[6] = 0.5f;
    if (!r) return WhiteMask(dev);
    const double a = c8[0], b = c8[2], c = c10[0], d = c10[2];
    const double det = a * d - b * c;
    if (std::fabs(det) < 1e-4 || std::fabs(k[0]) < 1e-9f || std::fabs(k[1]) < 1e-9f || r->w <= 0 || r->d <= 0) return WhiteMask(dev);
    const double sx = 1.0 / k[0], sz = 1.0 / k[1];
    const double ox = c15[0] - k[2] * sx - c8[3], oz = c15[2] - k[3] * sz - c10[3];
    rows[0] = static_cast<float>(d * sx / det / r->w);
    rows[1] = static_cast<float>(-b * sz / det / r->w);
    rows[2] = static_cast<float>((d * ox - b * oz) / det / r->w);
    rows[4] = static_cast<float>(-c * sx / det / r->d);
    rows[5] = static_cast<float>(a * sz / det / r->d);
    rows[6] = static_cast<float>((-c * ox + a * oz) / det / r->d);
    return LotMaskTexture(dev, *r);
}
// The lot's mask texture (room mask, lamp shade in alpha), checked again every 500 ms; white while it has none
// (left_lot_release) The texture of a lot no draw asked for in kIdleMaskMs (managed: a copy in RAM besides video memory) is
// given back; its room mask and lamp shade stay, so its next draw makes the same texels again at once (LotMaskTexture:
// dirty -> MakeMaskTexture from m.room and m.shade, in that same draw) and the shade on the world grass (m.ext) is untouched.
// Render thread, every 10 s.
void ReleaseIdleMaskTextures() {
    constexpr DWORD kIdleMaskMs = 120000;
    static DWORD s_at = 0;
    const DWORD now = GetTickCount();
    if (!DevSwitches::LeftLotRelease() || (s_at && now - s_at < 10000)) return;
    s_at = now | 1;
    uint64_t bytes = 0;
    int n = 0;
    for (auto& [lot, m] : g_lotMasks)
        if (m.tex && now - m.lastUse > kIdleMaskMs) {
            m.tex->Release();
            m.tex = nullptr;
            m.dirty = true;
            bytes += static_cast<uint64_t>(m.w) * m.h * 4;
            n++;
        }
    if (n) ApexMemory::NoteReleased("Lot shade: mask textures of lots not drawn for 2 minutes (made again at their next draw)", bytes);
}
IDirect3DTexture9* LotMaskTexture(IDirect3DDevice9* dev, const LotRect& rect) {
    const LotRect* const r = &rect;
    const uint64_t lot = (static_cast<uint64_t>(r->lotHi) << 32) | r->lotLo;
    LotMask& m = g_lotMasks[lot];
    const DWORD now = GetTickCount();
    m.lastUse = now;
    // (14/10: every 500 ms; a lot with no texture yet every 100 ms, not at every draw: its ground mask is a walk of the
    // game's memory)
    if (!m.checked || now - m.checked >= (m.tex ? 500u : 100u)) {
        m.checked = now;
        LevelLightShare::GroundMask gm;
        if (LevelLightShare::LotGroundMask(lot, m.tex ? m.sig : 0, gm) && gm.w > 0 && gm.h > 0 && gm.px.size() == static_cast<size_t>(gm.w) * gm.h) {
            if (gm.w != m.w || gm.h != m.h) { // another grid: the old shade does not fit
                m.shade.clear();
                MarkLotChunkShades(m);
                m.ext.reset();
            }
            m.w = gm.w;
            m.h = gm.h;
            m.room = std::move(gm.px);
            m.sig = gm.sig;
            m.dirty = true;
        }
        // the shade's grid is the room mask's (4 texels a metre over room 0's tiles); the lot rectangle must agree with it
        if (g_shadeOn && m.w > 0 && m.w == static_cast<int>(std::lround(r->w * GroundShadow::kTexelsPerMetre)) &&
            m.h == static_cast<int>(std::lround(r->d * GroundShadow::kTexelsPerMetre)))
            WantShade(lot, m, *r, now);
    }
    if (m.dirty) MakeMaskTexture(dev, m);
    if (!m.tex) {
        g_maskWhite.fetch_add(1, std::memory_order_relaxed);
        return WhiteMask(dev);
    }
    return m.tex;
}
IDirect3DTexture9* LotMaskFor(IDirect3DDevice9* dev, const float k[4], const float c15[4], const float c8[4], const float c10[4], const LotRect* r, float rows[8]) {
    EvictLotMasks();
    return LotMaskForImpl(dev, k, c15, c8, c10, r, rows);
}
std::string LotShadeStatusText() {
    return std::format("{} | lots shaded {}, builds dropped {} (inputs changed or lot gone), queued {}{} | last build: {} lamps, {} walls ({} of garden walls and fences, {} noted), {:.1f} ms on the worker "
                       "(longest {:.1f} ms), darkest texel {:.0f}%, {} wall cells with an opening or a lower top | wall tests {} (crossing a wall through an opening or "
                       "over a lower cell {}) | world grass around lots: {} chunks shaded, {} chunk builds (last {:.1f} ms)",
                       g_shadeOn ? "on" : "off", g_shadeBuilt.load(), g_shadeDropped.load(), g_shadeQueue.size(), g_lotsInFlight ? " (building)" : "",
                       g_shadeLastLamps, g_shadeLastWalls, g_shadeLastFences, g_fenceWalls.load(), g_shadeLastMs, g_shadeMaxMs, g_shadeLastMin * 100.0f, g_shadeLastPatches, g_shadeTests,
                       g_shadeThrough, g_chunkShades.size(), g_chunkShadeBuilds.load(), g_chunkShadeLastMs);
}

// ---- The same shade on the outdoor floors of story 0 (13/10, F7 2026-10-13 08-16-00: a paved path behind a free-standing
// wall was fully lit by the garden post on the other side while the grass beside it was shaded). Their lamp term max(floor
// map, atlas) has no wall test: the floor map is room 0's solve, and level_light_share tests walls for outdoor floor texels
// from story 1 up only (MaskedFloorRoom0); the atlas has none. The floor shaders multiply that term by 1 + gate (M - 1)
// (ShaderPatches FloorPatch::shadeSampler / shadeConst), M = this mask's alpha, gate = 1 within about 1 m of the lot's ground
// (GroundShadow::FloorShadeRows): the grass's own factor, nothing more (the room-0 solve itself is unchanged: shading it as
// well would shade the grass twice through the lot map). The floor's lot: its world rows (the vertex shader's c[k], c[k + 2])
// matched as the lot pass's (FindLotRect), else a lot with the same rotation whose rectangle holds the rows' origin. No lot,
// or the shade off: gate 0 and the white mask (the floor as before). Render thread.
std::atomic<long> g_floorShadeLot{0}, g_floorShadeNoLot{0};
const LotRect* FindFloorLot(const float c8[4], const float c10[4]) {
    if (const LotRect* r = FindLotRect(c8, c10)) return r;
    for (const LotRect& r : g_lotRects) {
        if (std::fabs(r.m0 - c8[0]) >= 2e-3f || std::fabs(r.m8 - c8[2]) >= 2e-3f || std::fabs(r.m2 - c10[0]) >= 2e-3f || std::fabs(r.m10 - c10[2]) >= 2e-3f) continue;
        const double det = static_cast<double>(r.m0) * r.m10 - static_cast<double>(r.m8) * r.m2;
        if (std::fabs(det) < 1e-6) continue;
        const double dx = c8[3] - r.tx, dz = c10[3] - r.tz;
        const double lx = (r.m10 * dx - r.m8 * dz) / det, lz = (-r.m2 * dx + r.m0 * dz) / det;
        if (lx > -0.5 && lz > -0.5 && lx < r.w + 0.5 && lz < r.d + 0.5) return &r;
    }
    return nullptr;
}
// The lot whose rectangle holds the world point (x, z), any rotation (instanced groups: their centre); null when none
std::atomic<long> g_fenceShadeLot{0}, g_fenceShadeNoLot{0};
const LotRect* FindLotAt(float x, float z) {
    for (const LotRect& r : g_lotRects) {
        const double det = static_cast<double>(r.m0) * r.m10 - static_cast<double>(r.m8) * r.m2;
        if (std::fabs(det) < 1e-6) continue;
        const double dx = x - r.tx, dz = z - r.tz;
        const double lx = (r.m10 * dx - r.m8 * dz) / det, lz = (-r.m2 * dx + r.m0 * dz) / det;
        if (lx >= 0.0 && lz >= 0.0 && lx <= r.w && lz <= r.d) return &r;
    }
    return nullptr;
}
GroundShadow::Grid LotGrid(const LotRect& r) {
    GroundShadow::Grid g;
    g.w = static_cast<int>(std::lround(r.w * GroundShadow::kTexelsPerMetre));
    g.h = static_cast<int>(std::lround(r.d * GroundShadow::kTexelsPerMetre));
    g.m0 = r.m0, g.m2 = r.m2, g.m8 = r.m8, g.m10 = r.m10, g.tx = r.tx, g.tz = r.tz, g.ty = r.ty;
    return g;
}
// Binds a floor draw's shade (its patch's sampler and 3 constants) for the draw and puts both back after it. worldK = the
// first world row of the game vertex shader the draw runs with; xzScale = 2 where the shader reads world xz / 2. The second
// constructor takes the lot itself (instanced groups: the lot under their centre) and the counters to add the draw to.
class FloorShadeBind {
public:
    FloorShadeBind(IDirect3DDevice9* dev, const ShaderPatches::FloorPatch& f, int worldK, float xzScale)
        : FloorShadeBind(dev, f.shadeSampler, f.shadeConst, f.shadeSampler >= 0 && f.shadeConst >= 0 ? LotFromRows(dev, worldK) : nullptr, xzScale, g_floorShadeLot,
                         g_floorShadeNoLot) {}
    FloorShadeBind(IDirect3DDevice9* dev, int sampler, int constant, const LotRect* lot, float xzScale, std::atomic<long>& withLot, std::atomic<long>& noLot)
        : dev_(dev) {
        if (sampler < 0 || constant < 0) return;
        lot_ = lot;
        float rows[12];
        on_ = GroundShadow::FloorShadeRows(lot_ ? LotGrid(*lot_) : GroundShadow::Grid{}, xzScale, g_shadeOn, rows) && g_shadeOn;
        (lot_ ? withLot : noLot).fetch_add(1, std::memory_order_relaxed);
        EvictLotMasks();
        IDirect3DTexture9* const tex = on_ ? LotMaskTexture(dev, *lot_) : WhiteMask(dev);
        c_ = static_cast<UINT>(constant);
        dev->GetPixelShaderConstantF(c_, old_, 3);
        SetPsConst(dev, c_, rows, 3);
        bind_.emplace(dev, static_cast<DWORD>(sampler), tex, D3DTEXF_NONE);
        active_ = true;
    }
    ~FloorShadeBind() {
        bind_.reset();
        if (active_) SetPsConst(dev_, c_, old_, 3);
    }
    FloorShadeBind(const FloorShadeBind&) = delete;
    FloorShadeBind& operator=(const FloorShadeBind&) = delete;
    // developer capture text (LightProbe)
    std::string Text() const {
        if (!active_) return "ground lamp shade: none (no world y, or no sampler to spare in the shader)";
        if (!lot_) return "ground lamp shade: lot NOT found (white, gate 0)";
        return std::format("ground lamp shade: lot {:08X}{:08X}, ground y {:.2f} (gate 1 within {:.2f} m, 0 from {:.2f} m){}", lot_->lotHi, lot_->lotLo, lot_->ty,
                           GroundShadow::kFloorGateFull, GroundShadow::kFloorGateNone, on_ ? "" : ", OFF (Buildings block light on objects)");
    }

private:
    static const LotRect* LotFromRows(IDirect3DDevice9* dev, int worldK) {
        float w[12] = {};
        return worldK >= 0 && SUCCEEDED(dev->GetVertexShaderConstantF(static_cast<UINT>(worldK), w, 3)) ? FindFloorLot(&w[0], &w[8]) : nullptr;
    }
    IDirect3DDevice9* dev_;
    std::optional<SamplerBind> bind_;
    const LotRect* lot_ = nullptr;
    float old_[12] = {};
    UINT c_ = 0;
    bool active_ = false, on_ = false;
};

// ---- Outdoor walls. Their lamp light is only the game's baked wall atlas (room solve, lamps x k2 = 0.075), much
// dimmer than the rig lamps objects get, so walls look darker than the objects in front of them. The
// pixel shaders of the game's ExteriorWall technique (Shaders_Win32.precomp) add it with "texld rA, vT, s2" then
// "mad rB.xyz, rA, cK.x, rC"; K differs per variant (in others c3.x is the bloom threshold). wall_lamp_table.h lists
// every ExteriorWall pixel shader by size + FNV-1a (32-bit, over DWORDs) with its K, generated offline; a draw with
// one of them gets cK.x multiplied by "Forca nas paredes". No shader is changed; interior walls are never in the table.
std::unordered_map<IDirect3DPixelShader9*, DWORD> g_wallConst;
std::atomic<float> g_wallGain{1.0f};
std::atomic<bool> g_wallEnabled{false};

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
// Exact full-day cinema/theatre facade shaders. Keep them as orthogonal tags rather than a PsClass so their normal
// object classification and existing dispatch behaviour stay intact. The map stores the one constant that only controls
// the luminance-derived bloom alpha; the second set marks the tiny centre-panel variant for an extra primitive-count guard.
std::unordered_map<IDirect3DPixelShader9*, DWORD> g_cinemaMarqueeDayPs;
std::unordered_set<IDirect3DPixelShader9*> g_cinemaMarqueePanelDayPs;

PsClass Classify(IDirect3DPixelShader9* ps) {
    if (!ps) return PsClass::Other;
    auto it = g_classCache.find(ps);
    if (it != g_classCache.end()) return it->second;
    PsClass c = PsClass::Other;
    uint16_t declaredSamplers = 0;
    UINT size = 0;
    if (SUCCEEDED(ps->GetFunction(nullptr, &size)) && size >= 8 && size < 65536) {
        std::vector<BYTE> code(size);
        if (SUCCEEDED(ps->GetFunction(code.data(), &size))) {
            if (RoomMapPadding::IsBasisPs(reinterpret_cast<const DWORD*>(code.data()), size / 4)) g_basisPs.insert(ps); // reads the room basis maps
            {
                const auto* t = reinterpret_cast<const DWORD*>(code.data());
                const size_t n = size / 4;
                for (size_t k = 0; k + 2 < n; k++) // dcl + sampler register token (as RoomMapPadding::NoteDraw)
                    if ((t[k] & 0xFFFF) == 0x001F && (((t[k + 2] >> 28) & 7) | (((t[k + 2] >> 11) & 3) << 3)) == 10 && (t[k + 2] & 0x7FF) < 16)
                        declaredSamplers |= static_cast<uint16_t>(1u << (t[k + 2] & 0x7FF));
                std::vector<DWORD> tokens(n);
                std::memcpy(tokens.data(), code.data(), n * 4);
                if (ShaderPatches::IsRoomMapPs(tokens)) g_roomMapPs.insert(ps); // reads a room light map (Smooth light maps)
            }
            const bool cinemaMainDay = IsShader(kCinemaMarqueeDayPs, code.data(), size);
            const bool cinemaMainNight = IsShader(kCinemaMarqueeNightPs, code.data(), size);
            const bool cinemaMain = cinemaMainDay || cinemaMainNight;
            const bool cinemaPanelDay = IsShader(kCinemaMarqueePanelDayPs, code.data(), size);
            const bool cinemaPanelNight = IsShader(kCinemaMarqueePanelNightPs, code.data(), size);
            const bool cinemaPanel = cinemaPanelDay || cinemaPanelNight;
            if (cinemaMain || cinemaPanel) {
                std::vector<DWORD> tokens(size / 4);
                std::memcpy(tokens.data(), code.data(), size);
                const int threshold = ShaderPatches::BloomThresholdConst(tokens);
                if (threshold >= 0) {
                    g_cinemaMarqueeDayPs[ps] = static_cast<DWORD>(threshold);
                    if (cinemaPanel) g_cinemaMarqueePanelDayPs.insert(ps);
                } else {
                    LOG_WARNING(std::format("[LotLightBridge] Cinema bloom guard: exact PS {:08X}/{} found but its isolated bloom threshold was not proven",
                                            ShaderHash(code.data(), size), size));
                }
            }
            if (IsShader(kLotLightPs, code.data(), size)) c = PsClass::LotLight;
            else if (IsShader(kLotLightRainPs, code.data(), size)) {
                c = PsClass::LotLight;
                g_wetLotPs.insert(ps); // drawn with the wet replacement
                static bool logged = false;
                if (!logged) { logged = true; LOG_INFO("[LotLightBridge] Rain: wet lot light pass seen (replaced with its wet darkening kept)"); }
            }
            else if (IsShader(kWorldMultiLightPs, code.data(), size) || IsShader(kWorldMultiLightRainPs, code.data(), size)) {
                c = PsClass::WorldMultiLight; // the rain variant reads the lamp map and c3.x the same way; its wet tail uses only c5/c6
                g_worldSamplers[ps] = 1u << 2; // only s2 is the lamp map; s1 is the normal map
                if (size == kWorldMultiLightRainPs.size) {
                    static bool logged = false;
                    if (!logged) { logged = true; LOG_INFO("[LotLightBridge] Rain: wet world terrain light pass seen"); }
                }
            }
            else if (ShaderStructure::Match m; ShaderStructure::MatchId(reinterpret_cast<const uint32_t*>(code.data()), size / 4, kLotLightStruct, m)) {
                // another variant of the lot light pass (other weather, season or game build): same body, so the same replacement
                c = PsClass::LotLight;
                const bool wet = m.tail == ShaderStructure::Tail::Wet && m.wetScale == 5 && m.wetMix == 6;
                if (wet) g_wetLotPs.insert(ps);
                LOG_INFO(std::format("[LotLightBridge] Lot light pass: variant recognised by its structure (pixel shader {}, {} bytes, {})", static_cast<void*>(ps), size,
                                     wet ? "wet tail kept" : m.tail == ShaderStructure::Tail::Dry ? "same as the dry pass" : "its extra colour step is not reproduced"));
            }
            else if (ShaderStructure::Match m; ShaderStructure::MatchId(reinterpret_cast<const uint32_t*>(code.data()), size / 4, kWorldMultiLightStruct, m)) {
                c = PsClass::WorldMultiLight; // Apex patches the game's own shader here, so any tail is kept as it is
                g_worldSamplers[ps] = 1u << 2;
                LOG_INFO(std::format("[LotLightBridge] World terrain light pass: variant recognised by its structure (pixel shader {}, {} bytes)", static_cast<void*>(ps), size));
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
            else if (IsShader(kLakeRainPs, code.data(), size) || IsShader(kLakeSnowPs, code.data(), size)) {
                c = PsClass::LakeWeather;
                static bool logged = false;
                if (!logged) { logged = true; LOG_INFO("[LotLightBridge] Water: lake shader of rain or snow seen"); }
            }
            else if (IsShader(kSeaNoReflPs, code.data(), size) || IsShader(kSeaNoReflRainPs, code.data(), size)) {
                c = PsClass::Sea;
                static bool logged = false;
                if (!logged) { logged = true; LOG_INFO("[LotLightBridge] Water: sea without the game's reflection seen (gets the shore reflection pass)"); }
            }
            else if (IsShader(kSnowLotPs, code.data(), size)) c = PsClass::LotLightSnow;
            else if (IsShader(kSnowLotCutPs, code.data(), size)) c = PsClass::LotLightSnowCut;
            else if (IsShader(kMeltLotPs, code.data(), size)) c = PsClass::LotLightMelt;
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
    // (A size-only "possible weather variant" log was tried on 05/10 and removed: dozens of unrelated game shaders share
    // those sizes. Variants are found by structure instead, see shader_structure.h and tools/shader_structure_test.)
    g_classCache[ps] = c;
    PsFacts& facts = g_psFacts[ps];
    facts.cls = c;
    facts.basis = g_basisPs.count(ps) != 0;
    facts.roomMap = g_roomMapPs.count(ps) != 0;
    facts.declared = declaredSamplers;
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
bool g_curVsIsSea = false; // the vertex shader of the sea water without reflection (kSeaNoReflVs)
bool g_curVsIsLakeWeather = false; // the lake vertex shader of rain and snow (kLakeWeatherVs)
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
struct FoliageVs { // the patched copy (ShaderPatches::PatchFoliageVs / PatchObjectLampVs), made at its first draw
    IDirect3DVertexShader9* vs = nullptr;
    bool tried = false;
    int worldK = -1; // objects: first VS constant of the world triple (c[K..K+2].w = the object position)
    int vertexLight = -1; // objects: first colour constant of the rig's 4 vertex lights (COLOR0), -1 if not found
    IDirect3DVertexShader9* perVertex = nullptr; // foliage: the per-vertex lamp copy (FoliageVertexLamps::Patch), development builds
    bool perVertexTried = false;
    int perVertexBase = -1;
};
// Outdoor floors lit only by their baked floor map (summer; floor_atlas_table.h): the patched copy of the vertex shader
// (see DrawFloorAtlas)
struct FloorVs {
    IDirect3DVertexShader9* vs = nullptr;
    bool tried = false;
    int tc = 7;
    int worldConst = -1; // first row of the world matrix in the game VS (rows k .. k+2; the y row's w = the lot's base height)
};
bool g_curVsIsObject = false;
// Everything known about a game vertex shader, in one entry (2026-09-29; before: a class cache and five more maps keyed by
// the same pointer, each looked up per draw). Entries are only added (ClassifyVs) and all dropped together (Shutdown),
// so a pointer to the current one stays valid while that shader is tracked (unordered_map keeps element addresses).
struct VsInfo {
    bool packedRoomFloor = false;
    bool worldMultiLight = false; // exact captured summer multi-pass light VS
    bool worldCompact = false; // exact captured single-layer WORLD VS (not an object rig)
    bool contractedLotUv = false; // exact regular lot VS; CPU floor samples use uncontracted local xz
    bool cinemaMarqueeDay = false; // exact EA 1.69 cinema/theatre marquee VS paired with kCinemaMarqueeDayPs
    uint8_t cls = 0;      // 0 other, 1 roof, 2 lake, 3 snow lot, 4 road, 5 floor, 6 foliage, 7 fence/stairs, 8 snow on objects,
                          // 9 snow with relief (stair tops), 10 object lit by a rig, 11 snow on floor tiles, 12 sea without reflection
    DWORD roadMap = 0;    // cls 4: VS constant with the terrain uv mapping (c16 in winter, c14 in summer)
    int snowFloorTc = 7;  // cls 11: where it puts world xz / 2
    ShaderPatches::WorldY worldY; // cls 5 and 11: the output it writes world y to (the floors' ground lamp shade), texcoord -1 = none
    FoliageVs patched;    // cls 6: foliage copy (wrap light); cls 10: object copy (+ world xzy in TEXCOORD8, PatchObjectLampVs)
    FloorVs floor;        // copy for the outdoor floors of summer (any class; made at the first such draw)
    int lmSem = -1;       // DrawIndoorObject: the semantic (usage << 4 | index) lmRow was found for, -1 = not looked yet
    int lmRow[2] = {-1, -1}; // the VS constants whose dp4 with the position make the room light map uv .x / .y (-1: not found)
    int winSem = -1;      // DrawWindowOutdoor: the semantic winRows was looked for (usage << 4 | index), -1 = not looked yet
    bool winRowsOk = false;
    ShaderPatches::UvRows winRows; // how the basis uv is made from the world position (ShaderPatches::WorldUvRows)
};
std::unordered_map<IDirect3DVertexShader9*, VsInfo> g_vsInfo;
VsInfo* g_curVsInfo = nullptr; // entry of g_curVs (null for no shader)
std::atomic<int> g_roofDrawn{0};
float g_cam[3] = {};
float g_lampData[33][4] = {}; // 16 x pos+radius, 16 x colour, params
int g_lampCount = 0;
int g_lampFrame = 0;
// Lamp cones (a brick fence face and objects were lit by a wall sconce they sit far off its axis; F8 capture).
// The game's cone laws: type 5 FUN_006BC940 (t = s (a.u - o) per cone, a1 +0x1A0 / o1 +0x174, a2 +0x190 / o2 +0x170,
// s = [0x011D11A0] = 5 (static init 0x00F81EF0 from [0x00FBD498]), u = point - lamp normalised (0x006BE342); full at
// t >= 1 on either cone, the spill S (+0x150, per channel) when both t <= 0, S (1 - t) + t between with t = the larger),
// type 4 FUN_006BDA10 (sat(sc (a.u - o)), a +0x170, sc +0x154, o +0x158). Kept as c = (-s a, -s o):
// t = dp3(l, c.xyz) + c.w, l = point -> lamp normalised. The default (t = 1, spill 1) is a lamp with no cone.
struct LampCone {
    float c1[4] = {0.0f, 0.0f, 0.0f, 1.0f}, c2[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    float spill = 1.0f; // S as one value (its luminance) for the per-pixel lamps
    // (12/10) range +0x130 x intensity +0x10, the w of the outdoor walls' reach law (lamp_reach.h): the per-pixel lamps of
    // objects, fences and windows take its r0, R and weight while realistic reach is on. Kept here (not in g_allLamps, whose
    // rows roofs and water upload as they are) so the SelectLamps memo carries it.
    float w = 0.0f;
    // (12/10, review) the lamp's entries in the outdoor rooms' light lists (LampReach::Entries: 2 for a story-0 lot lamp,
    // else 1), so its reach weight is the one the walls give it
    float entries = 1.0f;
    // (2.12.4, indoor light through openings) an indoor lamp shining out of a house through an opening: its light, whose share
    // at a point LevelLightShare::ExitLampVisibility gives (the walls of its story through the openings); 0 = an outdoor lamp
    uintptr_t light = 0;
};
LampCone g_lampCone[16];     // the cones of the lamps SelectLamps picked (as g_lampData[0..15])
uintptr_t g_coneScaleAt = 0; // 0x011D11A0 on fixed Steam addresses, else s = 5 (the value the game sets)

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
            info.contractedLotUv = is(kLotLightVs);
            info.cinemaMarqueeDay = is(kCinemaMarqueeDayVs);
            if (is(kRoofVs)) cls = 1;
            else if (is(kLakeVs)) cls = 2;
            else if (is(kSeaNoReflVs)) cls = 12;
            else if (is(kLakeWeatherVs)) cls = 13;
            else if (is(kSnowLotVs) || is(kSnowLotCutVs)) cls = 3;
            else if (is(kFloorVs)) cls = 5;
            else {
                std::vector<DWORD> t(size / 4);
                std::memcpy(t.data(), code.data(), t.size() * 4);
                info.packedRoomFloor = ShaderPatches::IsPackedRoomFloorVs(t);
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
                    cls = 6; // its patched code is made again at its first foliage draw (PatchedVsCode)
                } else {
                    std::vector<DWORD> o = t;
                    int tc = 7, wk = -1, vl = -1;
                    if (ShaderPatches::PatchObjectLampVs(o, true, nullptr, &wk, &vl)) {
                        info.patched.worldK = wk; // (the patched code: again at its first object draw, PatchedVsCode)
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
    if (cls == 5 || cls == 11) { // the winter floors' ground lamp shade reads world y where the vertex shader writes it
        std::vector<DWORD> t(size / 4);
        if (size >= 8 && SUCCEEDED(vs->GetFunction(t.data(), &size))) ShaderPatches::WorldYOutput(t, info.worldY);
    }
    VsInfo& stored = g_vsInfo.emplace(vs, std::move(info)).first->second;
    vs->AddRef();
    g_pinned.push_back(vs);
    return &stored;
}

// The patched code of a foliage (cls 6) or object (cls 10) vertex shader, made again from the game shader when its copy is
// created (05/10: ClassifyVs kept it for every classified shader, drawn or not, until Shutdown). Empty when it cannot be.
std::vector<DWORD> PatchedVsCode(IDirect3DVertexShader9* vs, uint8_t cls) {
    UINT size = 0;
    if (!vs || FAILED(vs->GetFunction(nullptr, &size)) || size < 8 || size > 65536 || size % 4) return {};
    std::vector<DWORD> t(size / 4);
    if (FAILED(vs->GetFunction(t.data(), &size))) return {};
    int wk = -1, vl = -1;
    if (cls == 6 ? ShaderPatches::PatchFoliageVs(t, true) : cls == 10 && ShaderPatches::PatchObjectLampVs(t, true, nullptr, &wk, &vl)) return t;
    return {};
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
        g_coneScaleAt = GameAddr::Get(GameAddr::Id::ConeScale5); // the cone scale (LampCone; 0x011D11A0 on Steam 1.67.2)
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

bool ReadLamp(uintptr_t L, float out[8], LampCone& cone) { // POD only (SEH)
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
        LampCone c;
        if (type == 5) {
            float k = 5.0f;
            if (g_coneScaleAt) {
                const float v = *reinterpret_cast<const float*>(g_coneScaleAt);
                if (v > 0.0f && v < 100.0f) k = v;
            }
            const float *a1 = reinterpret_cast<const float*>(L + 0x1A0), *a2 = reinterpret_cast<const float*>(L + 0x190), *s = reinterpret_cast<const float*>(L + 0x150);
            for (int i = 0; i < 3; i++) c.c1[i] = -k * a1[i], c.c2[i] = -k * a2[i];
            c.c1[3] = -k * *reinterpret_cast<const float*>(L + 0x174);
            c.c2[3] = -k * *reinterpret_cast<const float*>(L + 0x170);
            c.spill = std::clamp(s[0] * 0.2126f + s[1] * 0.7152f + s[2] * 0.0722f, 0.0f, 1.0f);
        } else if (type == 4) {
            const float* a = reinterpret_cast<const float*>(L + 0x170);
            const float sc = *reinterpret_cast<const float*>(L + 0x154), o = *reinterpret_cast<const float*>(L + 0x158);
            for (int i = 0; i < 3; i++) c.c1[i] = c.c2[i] = -sc * a[i];
            c.c1[3] = c.c2[3] = -sc * o;
            c.spill = 0.0f;
        }
        bool finite = std::isfinite(c.spill);
        for (int i = 0; i < 4; i++) finite = finite && std::isfinite(c.c1[i]) && std::isfinite(c.c2[i]);
        cone = finite ? c : LampCone{};
        cone.w = range * inten; // the product ReachShareImpl reads (LampReach::Radii takes an unreadable one as 85)
        cone.entries = LampReach::Entries(lotOwned, *reinterpret_cast<const int*>(L + 0xD0)); // +0xD0: the lamp's story (light_diag "d0")
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// An indoor lamp shining out of a house through an opening (LevelLightShare::ExitLamps): the same rows as ReadLamp's, its
// light kept in the cone record (ExitLampVisibility tests it), one entry in the outdoor rooms' lists (it is added once)
bool ReadExitLamp(uintptr_t L, float out[8], LampCone& cone) { // POD only (SEH)
    __try {
        const BYTE f = *reinterpret_cast<const BYTE*>(L + 0x100);
        if (!(f & 0x01) || !(f & 0x20)) return false; // alive and lit
        const bool lotOwned = (*reinterpret_cast<const uint32_t*>(L + 0xC0) | *reinterpret_cast<const uint32_t*>(L + 0xC4)) != 0;
        if (lotOwned && !(f & 0x40)) return false;
        const int type = *reinterpret_cast<const int*>(L + 0xB0);
        if (type < 3 || type > 6) return false; // the lamp classes (no window lights, no street lamps)
        const float* head = reinterpret_cast<const float*>(L + 0x120);
        const float* col = reinterpret_cast<const float*>(L + 0xF0);
        const float inten = *reinterpret_cast<const float*>(L + 0x10);
        const float fade = *reinterpret_cast<const float*>(L + 0x20);
        const float range = *reinterpret_cast<const float*>(L + 0x130);
        if (!(range > 0.01f)) return false;
        float radius = std::sqrt(range) * 1.2f;
        radius = radius < 2.0f ? 2.0f : (radius > 25.0f ? 25.0f : radius);
        out[0] = head[0]; out[1] = head[1]; out[2] = head[2]; out[3] = radius;
        out[4] = col[0] * inten * fade; out[5] = col[1] * inten * fade; out[6] = col[2] * inten * fade; out[7] = 0;
        for (int i = 0; i < 7; i++)
            if (!std::isfinite(out[i])) return false;
        LampCone c;
        if (type == 5) {
            float k = 5.0f;
            if (g_coneScaleAt) {
                const float v = *reinterpret_cast<const float*>(g_coneScaleAt);
                if (v > 0.0f && v < 100.0f) k = v;
            }
            const float *a1 = reinterpret_cast<const float*>(L + 0x1A0), *a2 = reinterpret_cast<const float*>(L + 0x190), *s = reinterpret_cast<const float*>(L + 0x150);
            for (int i = 0; i < 3; i++) c.c1[i] = -k * a1[i], c.c2[i] = -k * a2[i];
            c.c1[3] = -k * *reinterpret_cast<const float*>(L + 0x174);
            c.c2[3] = -k * *reinterpret_cast<const float*>(L + 0x170);
            c.spill = std::clamp(s[0] * 0.2126f + s[1] * 0.7152f + s[2] * 0.0722f, 0.0f, 1.0f);
        } else if (type == 4) {
            const float* a = reinterpret_cast<const float*>(L + 0x170);
            const float sc = *reinterpret_cast<const float*>(L + 0x154), o = *reinterpret_cast<const float*>(L + 0x158);
            for (int i = 0; i < 3; i++) c.c1[i] = c.c2[i] = -sc * a[i];
            c.c1[3] = c.c2[3] = -sc * o;
            c.spill = 0.0f;
        }
        bool finite = std::isfinite(c.spill);
        for (int i = 0; i < 4; i++) finite = finite && std::isfinite(c.c1[i]) && std::isfinite(c.c2[i]);
        cone = finite ? c : LampCone{};
        cone.w = range * inten;
        cone.entries = 1.0f;
        cone.light = L;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Every lit outdoor lamp (pos, radius, colour), in enumeration order: what SelectLamps picks from. Rebuilt with the lot
// lamp tracking in one pass over the enumeration (ReadEnumeratedLamps, every 20 frames); a rebuild that changes it starts
// a new generation of the SelectLamps memo.
std::vector<std::array<float, 8>> g_allLamps;
std::vector<LampCone> g_allCones; // the cone of each lamp of g_allLamps, at the same index
// (2.12.4) the indoor lamps shining out of a house through an opening, for the objects and fences outside only (roofs, water
// and glass keep g_allLamps): SelectLamps(..., true)
std::vector<std::array<float, 8>> g_exitLampRows;
std::vector<LampCone> g_exitCones;
std::vector<uintptr_t> g_exitList; // LevelLightShare::ExitLamps at the last rebuild
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
bool g_lampsHeldForSwitch = false; // the per-pixel lamp list was kept for a lamp switch shown all at once (AtriumHold)
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

// A captured session toggles 0x40 on ordinary lot lamps (types 3..6).
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

// The trees' own lamp list: read at every refresh, also while the per-pixel lamps keep theirs for a lamp switch or a
// drag (AtriumHold, DragLite: up to 6 to 9 s in game, for the rooms of a house to show the new light, which
// trees outside never wait on). The outdoor lamps as g_allLamps reads them, then the indoor lamps shining out as listed.
std::vector<std::array<float, 8>> g_foliageLamps;
std::vector<LampCone> g_foliageCones;
uint32_t g_foliageLampGen = 1; // a new generation whenever the list is not the same bit for bit
// Where the list changed (a lamp's place before and after, per generation; the last 512): an instance picks again at once
// only when a changed lamp is within reach of its box, else it just takes the new generation
struct FoliageLampChange {
    uint32_t gen;
    float pos[3];
};
std::vector<FoliageLampChange> g_foliageChanges;
uint32_t g_foliageChangesFrom = 1; // the oldest generation g_foliageChanges still describes completely
uint64_t FoliageRowKey(const std::array<float, 8>& v, const LampCone& c) {
    uint64_t h = 1469598103934665603ull;
    const auto mix = [&h](const void* p, size_t n) {
        const auto* b = static_cast<const unsigned char*>(p);
        for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
    };
    mix(v.data(), sizeof(float) * 8);
    mix(&c, sizeof c);
    return h;
}
// The same without the colour (v[4..6]: F0 x intensity x fade): a lamp that only fades or changes colour keeps this key
uint64_t FoliageShapeKey(const std::array<float, 8>& v, const LampCone& c) {
    std::array<float, 8> s = v;
    s[4] = std::max({v[4], v[5], v[6]}) > 0.0f ? 1.0f : 0.0f; // (a lamp coming out of black or going to it picks again)
    s[5] = s[6] = 0.0f;
    return FoliageRowKey(s, c);
}
// The colour of each lamp of the list by its shape key, and a generation whenever a colour changed with no lamp moved,
// added or removed (as at dusk, every lamp fading): an instance then takes its lamps' new colours by their keys at its
// next draw, with no new pick (a pick tests every lamp against the walls)
std::unordered_map<uint64_t, std::array<float, 3>> g_foliageColour;
uint32_t g_foliageColourGen = 1;
// `rebuilt`: ReadEnumeratedLamps just rebuilt g_allLamps and the exit rows from this enumeration, which are then taken as
// they are; while those are held, the same rows are read here (the indoor lamps shining out too, from a list of their own)
void ReadFoliageLamps(bool rebuilt) {
    static std::vector<std::array<float, 8>> previous;
    static std::vector<LampCone> previousCones;
    static std::vector<uintptr_t> exitList;
    previous.swap(g_foliageLamps);
    previousCones.swap(g_foliageCones);
    g_foliageLamps.clear();
    g_foliageCones.clear();
    if (rebuilt) {
        g_foliageLamps = g_allLamps;
        g_foliageCones = g_allCones;
        g_foliageLamps.insert(g_foliageLamps.end(), g_exitLampRows.begin(), g_exitLampRows.end());
        g_foliageCones.insert(g_foliageCones.end(), g_exitCones.begin(), g_exitCones.end());
    } else {
        LevelLightShare::ExitLamps(exitList);
        std::vector<std::array<float, 8>> exitRows;
        std::vector<LampCone> exitCones;
        for (uintptr_t L : g_enumLights) {
            std::array<float, 8> v;
            LampCone cone;
            if (ReadLamp(L, v.data(), cone)) g_foliageLamps.push_back(v), g_foliageCones.push_back(cone);
            else if (!exitList.empty() && std::binary_search(exitList.begin(), exitList.end(), L) && exitRows.size() < 256 && ReadExitLamp(L, v.data(), cone))
                exitRows.push_back(v), exitCones.push_back(cone);
        }
        g_foliageLamps.insert(g_foliageLamps.end(), exitRows.begin(), exitRows.end());
        g_foliageCones.insert(g_foliageCones.end(), exitCones.begin(), exitCones.end());
    }
    if (g_foliageLamps.size() == previous.size() && g_foliageCones.size() == previousCones.size() &&
        (g_foliageLamps.empty() || std::memcmp(g_foliageLamps.data(), previous.data(), g_foliageLamps.size() * sizeof(g_foliageLamps[0])) == 0) &&
        (g_foliageCones.empty() || std::memcmp(g_foliageCones.data(), previousCones.data(), g_foliageCones.size() * sizeof(LampCone)) == 0))
        return;
    g_foliageColour.clear();
    for (size_t i = 0; i < g_foliageLamps.size(); i++)
        g_foliageColour.try_emplace(FoliageShapeKey(g_foliageLamps[i], g_foliageCones[i]), std::array<float, 3>{g_foliageLamps[i][4], g_foliageLamps[i][5], g_foliageLamps[i][6]});
    g_foliageColourGen++;
    // the rows in one list and not the other by everything but the colour (a lamp moved, switched, added or removed, its
    // reach or cone changed), by their exact bits; a colour alone goes by g_foliageColourGen
    std::unordered_map<uint64_t, int> count;
    for (size_t i = 0; i < previous.size() && i < previousCones.size(); i++) count[FoliageShapeKey(previous[i], previousCones[i])]--;
    for (size_t i = 0; i < g_foliageLamps.size(); i++) count[FoliageShapeKey(g_foliageLamps[i], g_foliageCones[i])]++;
    if (std::all_of(count.begin(), count.end(), [](const auto& p) { return p.second == 0; })) return;
    g_foliageLampGen++;
    const auto note = [](const std::array<float, 8>& v) {
        if (g_foliageChanges.size() >= 512) { // the oldest generations go: their instances pick again whole
            g_foliageChangesFrom = g_foliageChanges[g_foliageChanges.size() / 2].gen;
            g_foliageChanges.erase(g_foliageChanges.begin(), g_foliageChanges.begin() + static_cast<long>(g_foliageChanges.size() / 2));
        }
        g_foliageChanges.push_back({g_foliageLampGen, {v[0], v[1], v[2]}});
    };
    for (size_t i = 0; i < previous.size() && i < previousCones.size(); i++)
        if (count[FoliageShapeKey(previous[i], previousCones[i])] < 0) note(previous[i]);
    for (size_t i = 0; i < g_foliageLamps.size(); i++)
        if (count[FoliageShapeKey(g_foliageLamps[i], g_foliageCones[i])] > 0) note(g_foliageLamps[i]);
}
// Whether a lamp of the list changed within reach (20 m, the reach law's longest) of the box since generation `since`
// (true also when that is older than what g_foliageChanges keeps)
bool FoliageLampsChangedNear(uint32_t since, const float* boxC, const float* ext) {
    if (since < g_foliageChangesFrom) return true;
    for (const FoliageLampChange& c : g_foliageChanges)
        if (c.gen > since && FoliageVertexLamps::BoxDistance2(c.pos, boxC, ext) < 20.0f * 20.0f) return true;
    return false;
}

void ReadEnumeratedLamps(bool rebuildAll) {
    TrackLampSwitches();
    static std::vector<std::array<float, 8>> previous, previousExit;
    static std::vector<LampCone> previousCones, previousExitCones;
    if (rebuildAll) {
        previous.swap(g_allLamps);
        g_allLamps.clear();
        previousCones.swap(g_allCones);
        g_allCones.clear();
        previousExit.swap(g_exitLampRows);
        g_exitLampRows.clear();
        previousExitCones.swap(g_exitCones);
        g_exitCones.clear();
        LevelLightShare::ExitLamps(g_exitList);
    }
    g_lotLampCur.clear();
    for (uintptr_t L : g_enumLights) {
        if (rebuildAll) {
            std::array<float, 8> v;
            LampCone cone;
            if (ReadLamp(L, v.data(), cone)) {
                g_allLamps.push_back(v);
                g_allCones.push_back(cone);
            } else if (!g_exitList.empty() && std::binary_search(g_exitList.begin(), g_exitList.end(), L) && g_exitLampRows.size() < 256 && ReadExitLamp(L, v.data(), cone)) {
                g_exitLampRows.push_back(v);
                g_exitCones.push_back(cone);
            }
        }
        LotLampState s;
        if (ReadLotLamp(L, s)) {
            s.baked = InBake(s);
            g_lotLampCur.emplace_back(L, s);
        }
    }
    if (rebuildAll) {
        g_lampCount = static_cast<int>(g_allLamps.size());
        // the SelectLamps memo stays valid only while the list (and its cones) is the same, bit for bit and in the same order
        if (g_allLamps.size() != previous.size() || (!g_allLamps.empty() && std::memcmp(g_allLamps.data(), previous.data(), g_allLamps.size() * sizeof(g_allLamps[0])) != 0) ||
            g_allCones.size() != previousCones.size() || (!g_allCones.empty() && std::memcmp(g_allCones.data(), previousCones.data(), g_allCones.size() * sizeof(LampCone)) != 0) ||
            g_exitLampRows.size() != previousExit.size() ||
            (!g_exitLampRows.empty() && std::memcmp(g_exitLampRows.data(), previousExit.data(), g_exitLampRows.size() * sizeof(g_exitLampRows[0])) != 0) ||
            g_exitCones.size() != previousExitCones.size() ||
            (!g_exitCones.empty() && std::memcmp(g_exitCones.data(), previousExitCones.data(), g_exitCones.size() * sizeof(LampCone)) != 0))
            g_lampMemoGen++;
    }
    std::stable_sort(g_lotLampCur.begin(), g_lotLampCur.end(), [](const LampSig& a, const LampSig& b) { return a.first < b.first; });
    g_lotLampCur.erase(std::unique(g_lotLampCur.begin(), g_lotLampCur.end(), [](const LampSig& a, const LampSig& b) { return a.first == b.first; }), g_lotLampCur.end());
}

// Compares g_lotLampCur (just read) with g_lotLampSig (the previous enumeration), then keeps the new one.
std::vector<uint64_t> g_disabledLots; // lots with a lamp disabled in this enumeration (TrackLotLampEdits)
void TrackLotLampEdits() {
    const auto now = Clock::now();
    g_disabledLots.clear();
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
    // (14/10, drag solves) type-11 lights added in this enumeration: one removed on the same lot within 0.5 m is the same lamp
    // moving (a lamp drag, drag_lite.h)
    std::vector<const LotLampState*> added11;
    auto prev = g_lotLampSig.begin(); // both lists are sorted by light: one walk finds each lamp's previous state
    for (auto& [L, s] : cur) {
        while (prev != g_lotLampSig.end() && prev->first < L) ++prev;
        if (prev == g_lotLampSig.end() || prev->first != L) {
            if (s.type == 11 && added11.size() < 64) added11.push_back(&s);
            snapDirty = true;
            if (VisibleLot(s.lot)) g_lotArrivals.insert(s.lot); // late lamp registration on a drawn lot
            // a lamp added to a settled lot while editing (Ctrl+Z / redo brings a deleted lamp back as a new light): its lot gathers again
            if (editing && s.lot != 0) {
                const auto seen = g_lotSeen.find(s.lot);
                if (seen != g_lotSeen.end() && now - seen->second.firstSeen > std::chrono::seconds(10) &&
                    std::find(g_disabledLots.begin(), g_disabledLots.end(), s.lot) == g_disabledLots.end())
                    g_disabledLots.push_back(s.lot);
            }
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
        // a lot lamp disabled (sold or deleted in Build mode: it stays registered and lit, 07/10): every room of its lot
        // gathers again at once (also when it comes back enabled: Ctrl+Z / redo of a sale or delete), so rooms the lamp edit does not send (the diagonal outside walls) drop it too
        if (s.lot != 0 && p.lot == s.lot && ((p.flags ^ s.flags) & 0x40) &&
            std::find(g_disabledLots.begin(), g_disabledLots.end(), s.lot) == g_disabledLots.end())
            g_disabledLots.push_back(s.lot);
        const char* why = nullptr; // why a raw change does not count
        bool counts = false, moved = false;
        float pl[3], sl[3];
        for (int c = 0; c < 3; c++) {
            pl[c] = LampLight(p, c);
            sl[c] = LampLight(s, c);
        }
        if (p.baked && s.baked && MovedApart(p.pos, s.pos)) {
            counts = moved = true; // user-driven (Build mode)
            // (a lamp the GPU draws, Instant, is not a lamp the game solves: its drag keeps the Instant rules)
            DragLite::NoteLampStep("lamp list: moved", DragLite::StepKind::Move, !LampMarkFilter::HandledLately(static_cast<uintptr_t>(L), 1500));
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
        if (p.type == 11)
            for (const LotLampState* a : added11) {
                const float dx = a->pos[0] - p.pos[0], dy = a->pos[1] - p.pos[1], dz = a->pos[2] - p.pos[2];
                if (a->lot == p.lot && dx * dx + dy * dy + dz * dz < 0.25f) {
                    DragLite::NoteLampStep("lamp list: type 11 removed and added", DragLite::StepKind::Move,
                                           !LampMarkFilter::HandledLately(static_cast<uintptr_t>(L), 1500));
                    break;
                }
            }
        if (p.baked) {
            Change& c = changes[p.lot];
            c.removed++;
            total++;
            if (Recorder::Verbose()) AddDetail(c.detail, c.details, std::format("L{:08X} type {}: removed", L, p.type));
        }
    }
    g_lotLampSig.swap(cur); // g_lotLampCur keeps the old list's memory for the next read
    for (uint64_t lot : g_disabledLots) LevelLightShare::RelightLotById(lot, "a lamp disabled or enabled again (sold, deleted, undo)");

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
        // every room of the lot lights again (07/10, a deleted lamp post kept lighting its wall): a deleted lamp runs no lamp
        // entry update, so the outdoor rooms of the other stories (the walls) kept it in their light lists
        if (lot != 0) LevelLightShare::RelightLotById(lot, "a lamp removed");
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
            const bool manual = c.added > 0 || c.moved > 0 || c.userEdited;
            userDriven |= manual;
            if (manual) userLots.push_back(lot);
            what = std::format("lot {:016X}: {} added, {} edited ({} moved), {} removed ({})", lot, c.added, c.edited, c.moved, c.removed, manual ? "manual-driven" : "automatic");
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
    unsigned char cones[16 * sizeof(LampCone)] = {}; // g_lampCone as SelectLamps leaves it (bytes: LampCone's defaults would move the memo out of .bss)
};
constexpr uint32_t kLampMemoSize = 512; // direct-mapped, indexed by the top 9 bits of a hash
static_assert(kLampMemoSize == (1u << (32 - 23)));
static_assert(sizeof(LampMemo::cones) == sizeof(g_lampCone));
LampMemo g_lampMemo[kLampMemoSize];
uint32_t g_lampMemoHits = 0, g_lampMemoMisses = 0;

uint32_t FloatBits(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}

int SelectLampsScan(float x, float z, float maxScore, bool withExit);

// withExit (objects and fences outside): also the indoor lamps shining out of a house (g_exitLampRows); a separate memo key
int SelectLamps(float x, float z, float maxScore, bool withExit = false) {
    const uint32_t bx = FloatBits(x), bz = FloatBits(z), bm = FloatBits(maxScore) ^ (withExit ? 0x80000000u : 0u);
    LampMemo& e = g_lampMemo[((bx * 0x9E3779B1u) ^ (bz * 0x85EBCA77u) ^ (bm * 0xC2B2AE3Du)) >> 23]; // top 9 bits: 512 entries
    if (e.gen == g_lampMemoGen && e.x == bx && e.z == bz && e.maxScore == bm) {
        g_lampMemoHits++;
        g_lastLampCandidates = e.candidates;
        std::memcpy(g_lampData, e.rows, sizeof(e.rows));
        std::memcpy(g_lampCone, e.cones, sizeof(e.cones));
        return e.picked;
    }
    g_lampMemoMisses++;
    const int m = SelectLampsScan(x, z, maxScore, withExit);
    e.gen = g_lampMemoGen;
    e.x = bx;
    e.z = bz;
    e.maxScore = bm;
    e.picked = m;
    e.candidates = g_lastLampCandidates;
    std::memcpy(e.rows, g_lampData, sizeof(e.rows));
    std::memcpy(e.cones, g_lampCone, sizeof(e.cones));
    return m;
}

int SelectLampsScan(float x, float z, float maxScore, bool withExit) {
    struct Cand { float score; const std::array<float, 8>* v; const LampCone* cone; };
    Cand c[64];
    int n = 0;
    const auto consider = [&](const std::vector<std::array<float, 8>>& rows, const std::vector<LampCone>& cones) {
        for (size_t i = 0; i < rows.size(); i++) {
            const auto& v = rows[i];
            const float dx = v[0] - x, dz = v[2] - z;
            const float d = std::sqrt(dx * dx + dz * dz);
            const float score = d - v[3];
            if (score > maxScore) continue; // too far from the lamp's reach
            const Cand cand{score, &v, i < cones.size() ? &cones[i] : nullptr};
            if (n < 64) c[n++] = cand;
            else {
                int worst = 0;
                for (int k = 1; k < 64; k++)
                    if (c[k].score > c[worst].score) worst = k;
                if (score < c[worst].score) c[worst] = cand;
            }
        }
    };
    consider(g_allLamps, g_allCones);
    if (withExit) consider(g_exitLampRows, g_exitCones);
    g_lastLampCandidates = n;
    const int m = std::min(16, n);
    std::partial_sort(c, c + m, c + n, [](const Cand& a, const Cand& b) { return a.score < b.score; });
    std::memset(g_lampData, 0, sizeof(float) * 4 * 32);
    for (LampCone& lc : g_lampCone) lc = LampCone{};
    for (int k = 0; k < m; k++) {
        std::memcpy(g_lampData[k], c[k].v->data(), 16);
        std::memcpy(g_lampData[16 + k], c[k].v->data() + 4, 16);
        if (c[k].cone) g_lampCone[k] = *c[k].cone;
    }
    return m;
}

// The cone factor (LampCone) of a lamp at its head `head` for a point p: S + (1 - S) sat(max(t1, t2)); 1 for a lamp with no cone
float LampConeAt(const LampCone& c, const float* head, const float* p) {
    float l[3] = {head[0] - p[0], head[1] - p[1], head[2] - p[2]};
    const float d = std::sqrt(l[0] * l[0] + l[1] * l[1] + l[2] * l[2]);
    if (!(d > 1e-3f)) return 1.0f;
    for (float& v : l) v /= d;
    const float t1 = l[0] * c.c1[0] + l[1] * c.c1[1] + l[2] * c.c1[2] + c.c1[3], t2 = l[0] * c.c2[0] + l[1] * c.c2[1] + l[2] * c.c2[2] + c.c2[3];
    return c.spill + (1.0f - c.spill) * std::clamp(std::max(t1, t2), 0.0f, 1.0f);
}
bool HasCone(const LampCone& c) { return c.spill < 1.0f || c.c1[3] != 1.0f || c.c2[3] != 1.0f; }
// The share of picked lamp k (g_lampData[k], g_lampCone[k]) reaching p: an indoor lamp shining out of a house through an
// opening is tested past the walls of its story through the openings (LevelLightShare::ExitLampVisibility), any other lamp
// by the outside walls and slabs (LampVisibility)
float LampConeVisibility(const LampCone& cone, const float* pr, const float* p, float nearSkip);
float PickedLampVisibility(int k, const float* pr, const float* p, float nearSkip = 0.2f) { return LampConeVisibility(g_lampCone[k], pr, p, nearSkip); }
float LampConeVisibility(const LampCone& cone, const float* pr, const float* p, float nearSkip) {
    if (const uintptr_t light = cone.light) {
        const float v = LevelLightShare::ExitLampVisibility(reinterpret_cast<const void*>(light), pr, p, nearSkip);
        return v < 0.0f ? 0.0f : v; // (not listed any more: dark)
    }
    // (development builds, lamp_visibility_memo) objects are drawn at the same place frame after frame and lamps change
    // every 20 frames at most: the wall and slab test of a lamp and a point is kept, keyed by the exact float bits and by
    // everything else it reads (LampVisibilityState), so a hit returns exactly the value it would compute
    if (!DevSwitches::LampVisibilityMemo() || LevelLightShare::VisibilityDiagArmed()) return LevelLightShare::LampVisibility(pr, p, nearSkip);
    struct VisMemo {
        uint32_t k[7];
        uint64_t state;
        float v;
        bool used;
    };
    static VisMemo s_vis[4096];
    const uint64_t state = LevelLightShare::LampVisibilityState();
    uint32_t key[7];
    std::memcpy(&key[0], pr, 12);
    std::memcpy(&key[3], p, 12);
    std::memcpy(&key[6], &nearSkip, 4);
    uint32_t h = 2166136261u;
    for (uint32_t x : key) h = (h ^ x) * 16777619u;
    h ^= static_cast<uint32_t>(state) ^ static_cast<uint32_t>(state >> 32);
    VisMemo& e = s_vis[(h * 2654435761u) >> 20];
    if (e.used && e.state == state && std::memcmp(e.k, key, sizeof key) == 0) return e.v;
    const float v = LevelLightShare::LampVisibility(pr, p, nearSkip);
    std::memcpy(e.k, key, sizeof key);
    e.state = LevelLightShare::LampVisibilityState(); // (the call may have rebuilt a grid: keyed by what it read)
    e.v = v;
    e.used = e.state == state;
    return v;
}
bool g_lampConeLogged = false, g_fenceConeLogged = false; // render thread

template <typename DrawFn> bool DrawRoof(IDirect3DDevice9* dev, DrawFn draw);

void EnsureReplacement(IDirect3DDevice9* dev) {
    if (g_replacementPs || g_compileTried) return;
    g_compileTried = true;
    const std::string err = CompilePs(dev, kReplacementPsId, &g_replacementPs);
    g_status = err.empty() ? "Active" : "Failed: " + err;
    LOG_INFO("[LotLightBridge] " + g_status);
}

void EnsureWetReplacement(IDirect3DDevice9* dev) {
    if (g_replacementWetPs || g_wetCompileTried) return;
    g_wetCompileTried = true;
    const std::string err = CompilePs(dev, kReplacementWetPsId, &g_replacementWetPs);
    if (!err.empty()) LOG_WARNING("[LotLightBridge] Rain lot light pass: " + err + " (left as the game draws it)");
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
    g_lampData[32][0] = g_roofStrength.load(std::memory_order_relaxed) * TerrainLightingPolicy::LampDay(g_night.load(std::memory_order_relaxed)); // x the daytime curve (12/10)
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
IDirect3DPixelShader9* g_waterSeaPs = nullptr; // the same pass compiled for that sea (macro SEA)
bool g_waterSeaCompileTried = false;
IDirect3DPixelShader9* g_waterWeatherPs = nullptr; // compiled for the lake of rain and snow (macro WEATHER)
bool g_waterWeatherCompileTried = false;
std::atomic<int> g_waterDrawn{0};

// sea = the sea water without the game's reflection (kSeaNoReflPs + kSeaNoReflVs): world-view-projection in VS c0..c3, an
// identity world matrix in c4..c6, the camera in c7; the pass is the lake's, compiled with SEA (other input registers)
// weather = the lake water of rain and snow (kLakeRainPs / kLakeSnowPs + kLakeWeatherVs): the lake's matrices and inputs, the
// pass compiled with WEATHER (camera c3, wave scales c9, wave maps s2 / s3, scene copy s8)
template <typename DrawFn> bool DrawLake(IDirect3DDevice9* dev, DrawFn draw, bool sea = false, bool weather = false) {
    if (!g_waterFix.load(std::memory_order_relaxed)) return false;
    if (sea && !g_curVsIsSea) return false; // another vertex shader: its outputs are unknown
    if (weather && !g_curVsIsLakeWeather) return false;
    if (!sea && !weather && !g_curVsIsLake) {
        static bool logged = false; // the lake pixel shader with another vertex shader: the pass reads the lake one's outputs
        if (!logged) { logged = true; LOG_INFO("[LotLightBridge] Water: lake pixel shader drawn with another vertex shader, skipped"); }
        return false;
    }
    if (!g_waterPs && !g_waterCompileTried) {
        g_waterCompileTried = true;
        const std::string err = CompilePs(dev, kWaterPsId, &g_waterPs);
        LOG_INFO("[LotLightBridge] Water: " + (err.empty() ? std::string("active") : err));
    }
    if (sea && !g_waterSeaPs && !g_waterSeaCompileTried) {
        g_waterSeaCompileTried = true;
        const std::string err = CompilePs(dev, kWaterSeaPsId, &g_waterSeaPs);
        LOG_INFO("[LotLightBridge] Water (sea without reflection): " + (err.empty() ? std::string("active") : err));
    }
    if (weather && !g_waterWeatherPs && !g_waterWeatherCompileTried) {
        g_waterWeatherCompileTried = true;
        const std::string err = CompilePs(dev, kWaterWeatherPsId, &g_waterWeatherPs);
        LOG_INFO("[LotLightBridge] Water (lake in rain and snow): " + (err.empty() ? std::string("active") : err));
    }
    IDirect3DPixelShader9* const passPs = sea ? g_waterSeaPs : weather ? g_waterWeatherPs : g_waterPs;
    if (!passPs) return false;
    float world[12];
    if (FAILED(dev->GetVertexShaderConstantF(sea ? 4 : 8, world, 3))) return false;
    float cam[4]{}; // sea: lamps near the camera (the sea mesh is huge and has no translation)
    if (sea && FAILED(dev->GetVertexShaderConstantF(7, cam, 1))) return false;
    g_inOwnCall = true;
    draw(); // the game's water, unchanged
    const int picked = sea ? SelectLamps(cam[0], cam[2], 150.0f) : SelectLamps(world[3], world[11], 150.0f);
    {
        g_lampData[32][0] = g_waterStrength.load(std::memory_order_relaxed) * TerrainLightingPolicy::LampDay(g_night.load(std::memory_order_relaxed)); // x the daytime curve (12/10)
        g_lampData[32][1] = static_cast<float>(picked);
        g_lampData[32][2] = g_waterFilter.load(std::memory_order_relaxed) ? 1.0f : 0.0f;
        g_lampData[32][3] = g_waterColorCompression.load(std::memory_order_relaxed) ? 1.0f : 0.0f;
        float consts[40][4] = {};
        std::memcpy(consts, g_lampData, sizeof(float) * 4 * 33);
        dev->GetVertexShaderConstantF(sea ? 0 : 4, &consts[33][0], 4); // world-view-projection of the water mesh
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
        SetPs(dev, passPs);
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

// The same patch for the snowy pass (last sampler s11, terrain light in s12) and the melting-snow pass (kMeltLotPs: last
// sampler s13, terrain light in s14). r7, v7 and the new sampler must be unused (checked).
// With mask (room mask, 07/10): also "mad r6.x/y" (mask uv from v7 with PS c200 / c201), "texld r6, r6, s<new+1>" and
// "mul r7.xyz, r7, r6.x" before the max, so the terrain light stays outside the lot's rooms; r6, c200, c201 and that
// sampler must be unused too. After the max, "mul r0.xyz, r0, r6.w": the lamp shade (the mask's alpha) on both terms.
constexpr DWORD kMaskConst = 200;
bool PatchLotTerrainMax(std::vector<DWORD>& t, DWORD lastSampler, DWORD newSampler, bool mask = false) {
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
        for (size_t k = 1; k <= len; k++) {
            const DWORD r = t[i + k];
            if (op == 0x1F && k == 1) continue; // dcl usage token
            if (op == 0x51 && k > 1) continue;  // def values
            if (!(r & 0x80000000u)) continue;
            const DWORD ty = regType(r), n = regNum(r);
            if ((ty == 0 && n == 7) || (ty == 1 && n == 7) || (ty == 10 && n == newSampler)) return false; // r7 / v7 / new sampler taken
            if (mask && ((ty == 0 && n == 6) || (ty == 2 && (n == kMaskConst || n == kMaskConst + 1)) || (ty == 10 && n == newSampler + 1))) return false;
        }
        if (op == 0x1F && regType(t[i + 2]) == 10 && regNum(t[i + 2]) == lastSampler) dclEnd = i + 1 + len;
        if (op == 0x42 && !texldEnd && regType(t[i + 1]) == 0 && regNum(t[i + 1]) == 0 && regType(t[i + 2]) == 1 && regNum(t[i + 2]) == 3 && regType(t[i + 3]) == 10 && regNum(t[i + 3]) == 2)
            texldEnd = i + 1 + len;
        i += 1 + len;
    }
    if (!dclEnd || !texldEnd || texldEnd < dclEnd || texldEnd + 1 >= t.size()) return false;
    // next instruction must be "mul r0.xyz, r1.w, r0" (light basis factor)
    if ((t[texldEnd] & 0xFFFF) != 0x05 || regType(t[texldEnd + 1]) != 0 || regNum(t[texldEnd + 1]) != 0) return false;
    const size_t insertAt = texldEnd + 1 + ((t[texldEnd] >> 24) & 0xF);
    const DWORD fetch[] = {0x03000042, 0x802F0007, 0x90E40007, 0xA0E40800 | newSampler, // texld_pp r7, v7, s<new>
                           0x03000002, 0x80270007, 0x80E40007, 0x80E40007,              // add_pp r7.xyz, r7, r7
                           0x03000002, 0x80270007, 0x80E40007, 0x80E40007,              // add_pp r7.xyz, r7, r7
                           0x0300000B, 0x80270000, 0x80E40000, 0x80E40007};             // max_pp r0.xyz, r0, r7
    t.insert(t.begin() + insertAt, std::begin(fetch), std::end(fetch));
    if (mask) {
        const DWORD m = newSampler + 1, c0 = 0xA0000000 | kMaskConst, c1 = 0xA0000000 | (kMaskConst + 1);
        const DWORD ops[] = {0x04000004, 0x80010006, 0x90000007, c0,              c0 | 0x00AA0000, // mad r6.x, v7.x, c200.x, c200.z
                             0x04000004, 0x80010006, 0x90550007, c0 | 0x00550000, 0x80000006,      // mad r6.x, v7.y, c200.y, r6.x
                             0x04000004, 0x80020006, 0x90000007, c1,              c1 | 0x00AA0000, // mad r6.y, v7.x, c201.x, c201.z
                             0x04000004, 0x80020006, 0x90550007, c1 | 0x00550000, 0x80550006,      // mad r6.y, v7.y, c201.y, r6.y
                             0x03000042, 0x800F0006, 0x80E40006, 0xA0E40800 | m,                 // texld r6, r6, s<m>
                             0x03000005, 0x80070007, 0x80E40007, 0x80000006};                    // mul r7.xyz, r7, r6.x
        t.insert(t.begin() + insertAt + 12, std::begin(ops), std::end(ops)); // after the texld and the two adds, before the max
        // the lamp shade (13/10, the mask's alpha, 1 with no wall between): x the whole lamp term, after the max
        const DWORD shade[] = {0x03000005, 0x80270000, 0x80E40000, 0x80FF0006}; // mul_pp r0.xyz, r0, r6.w
        t.insert(t.begin() + insertAt + 12 + std::size(ops) + 4, std::begin(shade), std::end(shade));
        const DWORD mdecl[] = {0x0200001F, 0x90000000, 0xA00F0800 | m};
        t.insert(t.begin() + dclEnd, std::begin(mdecl), std::end(mdecl));
    }
    const DWORD decl[] = {0x0200001F, 0x80010005, 0x90230007, 0x0200001F, 0x90000000, 0xA00F0800 | newSampler}; // v7 = TEXCOORD1, s<new> 2D
    t.insert(t.begin() + dclEnd, std::begin(decl), std::end(decl));
    return true;
}
bool PatchSnowBytecode(std::vector<DWORD>& t) { return PatchLotTerrainMax(t, 11, 12); }

// The cut variant (kSnowLotCutPs, the ground under a rug): its last sampler is s12, the terrain light goes in s13
IDirect3DPixelShader9* g_snowCutPs = nullptr;
bool g_snowCutTried = false;
bool g_snowMasked = false, g_snowCutMasked = false, g_meltMasked = false; // the patched copies with the room mask
// The room mask for a snowy pass: the lot found by the world matrix (VS c8 / c10, else c9 / c11), PS c200 / c201 and the
// sampler set and put back (white, rows 0: mask 1, when the lot is unknown)
struct MaskBind {
    IDirect3DDevice9* dev;
    bool on = false, saved = false;
    float old[8] = {};
    std::optional<SamplerBind> bind;
    MaskBind(IDirect3DDevice9* d, bool masked, DWORD sampler, const float k[4], const float c15[4]) : dev(d), on(masked) {
        if (!on) return;
        float m[16] = {};
        const LotRect* r = nullptr;
        float rows[8];
        if (SUCCEEDED(dev->GetVertexShaderConstantF(8, m, 4))) {
            if ((r = FindLotRect(&m[0], &m[8]))) bind.emplace(dev, sampler, LotMaskFor(dev, k, c15, &m[0], &m[8], r, rows), D3DTEXF_NONE);
            else if ((r = FindLotRect(&m[4], &m[12]))) bind.emplace(dev, sampler, LotMaskFor(dev, k, c15, &m[4], &m[12], r, rows), D3DTEXF_NONE);
        }
        if (!r) bind.emplace(dev, sampler, LotMaskFor(dev, k, c15, m, m, nullptr, rows), D3DTEXF_NONE);
        saved = SUCCEEDED(dev->GetPixelShaderConstantF(kMaskConst, old, 2));
        SetPsConst(dev, kMaskConst, rows, 2);
    }
    ~MaskBind() {
        if (on && saved) SetPsConst(dev, kMaskConst, old, 2);
    }
};

// Melting snow (kMeltLotPs): the patched copy, made once from the first such shader drawn
IDirect3DPixelShader9* g_meltPs = nullptr;
bool g_meltTried = false;

template <typename DrawFn> bool DrawLotSnow(IDirect3DDevice9* dev, DrawFn draw, bool cut = false) {
    if (!g_curVsIsSnowLot) return false;
    IDirect3DPixelShader9*& snowPs = cut ? g_snowCutPs : g_snowPs;
    bool& tried = cut ? g_snowCutTried : g_snowTried;
    const DWORD terrainSampler = cut ? 13 : 12;
    if (!snowPs && !tried) {
        tried = true;
        // patched from the shader the game has bound (this runs only for PsClass::LotLightSnow, an exact match)
        std::vector<DWORD> t = ShaderCode(g_curPs);
        std::vector<DWORD> plain = t;
        bool& masked = cut ? g_snowCutMasked : g_snowMasked;
        masked = !t.empty() && PatchLotTerrainMax(t, terrainSampler - 1, terrainSampler, true) && SUCCEEDED(dev->CreatePixelShader(t.data(), &snowPs));
        if (!masked) snowPs = nullptr;
        const bool ok = masked || (!plain.empty() && PatchLotTerrainMax(plain, terrainSampler - 1, terrainSampler) && SUCCEEDED(dev->CreatePixelShader(plain.data(), &snowPs)));
        if (!ok) snowPs = nullptr;
        LOG_INFO(std::string("[LotLightBridge] Snow") + (cut ? " (ground under rugs)" : "") + ": " + (ok ? (masked ? "active (room mask)" : "active (no room mask)") : "failed"));
    }
    if (!snowPs) return false;
    float v[8];
    if (FAILED(dev->GetVertexShaderConstantF(15, v, 2)) || !Near(v[0], 1.0f / 256.0f) || !Near(v[1], 1.0f / 256.0f)) return false;
    // The terrain light the lot is compared with: the world atlas when it is ready (a lot can reach past its "home"
    // chunk, c16: LightProbe-m76, lot at z 1290 on the chunk ending at z 1280, drew the chunk's clamped edge row and
    // came out dark next to a lit sidewalk), else the home chunk's own map as before. The VS computes the uv as
    // (world.xz - c16.xz) * c15.xy + c15.zw, so for the atlas (uv = world.xz * a.xy + a.zw) c15 becomes
    // (a.xy, a.zw + c16.xz * a.xy) for this draw.
    float atlasC[4]{};
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
        SamplerBind terrainMap(dev, terrainSampler, terrain);
        const bool masked = cut ? g_snowCutMasked : g_snowMasked;
        MaskBind maskBind(dev, masked, terrainSampler + 1, atlas ? atlasMap : v, &v[4]);
        SetPs(dev, snowPs);
        if (atlas) SetVsConst(dev, 15, atlasMap, 1);
        // c4.x scales max(lot map, terrain) only. TerrainConst = the world terrain's LampScale (92582b6 gave terrain and dry lots
        // the daytime lamp term, not this pass: snowy lots lost the street-lamp pool at dusk, a cut at the lot edge, 06/10).
        // At full night LampScale equals the previous ConstGain(GroundGain()).
        TerrainConst lampGain(dev, 4, false);
        draw();
        if (atlas) SetVsConst(dev, 15, v, 1);
        SetPs(dev, original);
    }
    g_inOwnCall = false;
    g_snowDrawn.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// The lot light pass while snow melts (kMeltLotPs, F7: the lot kept the game's light next to Apex's world
// terrain, a cut at the lot edge). Same lamp term as the snowy pass, so the same patch (terrain light in s14); its VS
// gives the terrain uv in TEXCOORD1 from c14/c15 like the dry lot pass, so the atlas goes through VS c14 as there.
template <typename DrawFn> bool DrawLotMelt(IDirect3DDevice9* dev, DrawFn draw) {
    if (!g_meltPs && !g_meltTried) {
        g_meltTried = true;
        std::vector<DWORD> t = ShaderCode(g_curPs);
        std::vector<DWORD> plain = t;
        g_meltMasked = !t.empty() && PatchLotTerrainMax(t, 13, 14, true) && SUCCEEDED(dev->CreatePixelShader(t.data(), &g_meltPs));
        if (!g_meltMasked) g_meltPs = nullptr;
        const bool ok = g_meltMasked || (!plain.empty() && PatchLotTerrainMax(plain, 13, 14) && SUCCEEDED(dev->CreatePixelShader(plain.data(), &g_meltPs)));
        if (!ok) g_meltPs = nullptr;
        LOG_INFO(std::string("[LotLightBridge] Melting snow (lot light pass): ") + (ok ? "active" : "failed, left as the game draws it"));
    }
    if (!g_meltPs) return false;
    float v[8];
    if (FAILED(dev->GetVertexShaderConstantF(14, v, 2)) || !Near(v[0], 1.0f / 256.0f) || !Near(v[1], 1.0f / 256.0f)) return false;
    float atlasC[4]{};
    IDirect3DTexture9* atlas = LightmapSmooth::Atlas(atlasC);
    IDirect3DBaseTexture9* terrain = atlas;
    if (!atlas) {
        auto it = g_chunks.find(Key(v[4], v[6])); // c15.xz = chunk centre
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
        SamplerBind terrainMap(dev, 14, terrain);
        MaskBind maskBind(dev, g_meltMasked, 15, atlas ? atlasMap : v, &v[4]);
        SetPs(dev, g_meltPs);
        if (atlas) SetVsConst(dev, 14, atlasMap, 1);
        TerrainConst lampGain(dev, 4, false); // c4.x scales max(lot map, terrain), as in the snowy pass
        draw();
        if (atlas) SetVsConst(dev, 14, v, 1);
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
    ShaderPatches::RoomMapPatch room;
    ShaderPatches::WindowOutdoorPatch window;
    DWORD nightConst = 0;
    int cubeTint = -1; // PatchCubeTint
    DWORD nativeAlphaSampler = 0;
    DWORD nativeAlphaSource = 0;
    DWORD terrainDayConst = 0;
    DWORD shadeSampler = 0; // PatchTerrainShade
};
std::unordered_map<IDirect3DPixelShader9*, PatchedPs> g_roadPs, g_floorPs, g_snowFloorPs, g_snowFloorPs0, g_leafPs, g_fencePs, g_snowCoverPs, g_snowReliefPs, g_objLampPs;
std::unordered_map<IDirect3DPixelShader9*, PatchedPs> g_terrainAlphaPs;
// Winter floor and snow-on-floor copies with the ground lamp shade, per (kind, where the vertex shader writes world y):
// FloorShadeKey; the copies without it stay in g_floorPs / g_snowFloorPs / g_snowFloorPs0
std::map<int, std::unordered_map<IDirect3DPixelShader9*, PatchedPs>> g_floorShadePs;
int FloorShadeKey(int kind, const ShaderPatches::WorldY& y) { return (kind * 16 + y.texcoord) * 4 + y.comp; }
std::unordered_map<IDirect3DPixelShader9*, PatchedPs> g_terrainDayPs;
// the same two with the walls' shade on the world grass around lots (PatchTerrainShade, ChunkShadeTexture)
std::unordered_map<IDirect3DPixelShader9*, PatchedPs> g_terrainShadePs, g_terrainDayShadePs, g_roadShadePs;
std::atomic<int> g_roadDrawn{0}, g_floorDrawn{0}, g_snowFloorDrawn{0}, g_leafDrawn{0}, g_roofSnowDrawn{0}, g_foliageDrawn{0}, g_foliageDayScaled{0}, g_foliageReachLaw{0}, g_fenceDrawn{0}, g_snowCoverDrawn{0}, g_snowReliefDrawn{0}, g_objLampDrawn{0};
// Buildings block light on objects (12/10; LevelLightShare::LampVisibility), render thread only (plain counters, read by
// Status on the same thread): the lamps objects and windows (obj) and fences (fence) left out (a wall or a slab between, or
// under 1% left at a slab's edge) or kept dimmed at a slab's edge, and the object and window draws whose ground light faded
// because they stand above every lamp head around (the ground gate, below)
uint32_t g_objLampsLeftOut = 0, g_objLampsDimmed = 0, g_fenceLampsLeftOut = 0, g_fenceLampsDimmed = 0, g_objGroundGated = 0;
// Outdoor object draws that placed no per-pixel lamp and kept the game's rig, its lamp colours scaled by the daytime curve
// (DrawObjectLamp, 12/10), render thread
uint32_t g_objRigDayScaled = 0;
// Realistic lamp reach on objects, fences and windows (12/10, F7 20-31-19 / 20-31-21 at a red garden post: a balcony band
// and a column 5.5-9.8 m away went black at the first law's R = 7.82 m while the outdoor wall behind them, on the reach law,
// stayed lit). While LevelLightShare::RealisticReachActive(), the per-pixel lamps take the outdoor walls' law (lamp_reach.h,
// ShaderPatches AppendLampLaw) from patched copies of their own (the caches below; the first law's copies stay for the
// option off), and the walls' own scale (ReachLampScale). Render thread: the draws by the reach law (objects, fences,
// windows) and those that kept the first law because the shader had no room for the 2 more rows.
std::unordered_map<IDirect3DPixelShader9*, PatchedPs> g_objLampPsReach, g_fencePsReach;
uint32_t g_reachObjDraws = 0, g_reachFenceDraws = 0, g_reachWindowDraws = 0, g_reachLawRefused = 0;
// The game's lamp scale of the outdoor walls (the native cK.x DrawWallGain reads; F7 20-31-19 at night drew c3.x 0.309124 =
// 1 x the walls' gain 0.309124 of that capture's settings), render thread. The reach law gives objects the walls' scale.
// Read only while the walls' gain is on and a recognised wall is drawn (DrawWallGain), by day too, where it can be 0 (F7
// 10:28:03, validation page). (12/10 review: a value kept from the day left every reach object, fence and window without
// lamp light at night while no wall was drawn, or with the walls' gain off.) NativeWallScale() trusts it for 2 s after it
// was read (g_lotDrawTick, the Present tick), else takes the daytime curve's value LampDay(night), 1 at night (F7 20-31-19).
// (12/10, one daytime curve: the fallback was 1 at any level, so at dusk with no wall drawn the columns took the night
// scale while the walls took the day one; before full night WallLampScale no longer reads the native value anyway.)
std::atomic<float> g_nativeWallScale{1.0f}; // (14/10: atomics, read by ObjectLampColourAt on the light tree thread too)
std::atomic<DWORD> g_nativeWallTick{0};     // g_lotDrawTick when it was read; 0 = never
bool NativeWallFresh() { const DWORD at = g_nativeWallTick, draw = g_lotDrawTick; return at && draw - at <= 2000; }
float NativeWallScale() { return NativeWallFresh() ? static_cast<float>(g_nativeWallScale) : TerrainLightingPolicy::LampDay(g_night.load(std::memory_order_relaxed)); }
constexpr float kPixelLampTrimUnit = 0.75f; // the default of the per-pixel lamp and fence sliders: x1 of the walls' scale (reach)
std::atomic<bool> g_objPixel{true};
std::atomic<float> g_objPixelStrength{1.0f};
std::atomic<bool> g_objPixelLamps{true};          // outdoor rig objects: world lamps per pixel instead of the rig lamps
std::atomic<float> g_objPixelLampStrength{1.0f};
// Windows take outdoor light (experimental, off by default): see DrawWindowOutdoor
std::atomic<bool> g_windowOutdoor{true};
constexpr float kWindowWallSkip = 0.75f;  // a wall this close to an object's origin is its own wall (a turned window's origin is 0.5 m in)
constexpr float kWindowPlanReach = 0.75f; // metres along the normal at which the house plan is read
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

// Patched copy of the current pixel shader, made once per shader with `patch`. variant (12/10, realistic reach): a second
// copy of a shader another patch already took; refused, that first copy keeps drawing it, and the shader, saved under the
// first patch's name when it was refused there, is not saved again.
template <typename PatchFn>
PatchedPs& PatchedFor(IDirect3DDevice9* dev, std::unordered_map<IDirect3DPixelShader9*, PatchedPs>& cache, const char* what, PatchFn patch, bool variant = false) {
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
            if (matched)
                LOG_WARNING(std::format("[LotLightBridge] {}: shader {:08X} refused by D3D ({:08X}){}", what, reinterpret_cast<uintptr_t>(g_curPs), static_cast<uint32_t>(hr),
                                        variant ? ", the first copy draws it" : ""));
            else
                LOG_INFO(std::format("[LotLightBridge] {}: shader {:08X} does not have the expected pattern, {}", what, reinterpret_cast<uintptr_t>(g_curPs),
                                     variant ? "the first copy draws it" : "left as the game draws it"));
            if (!original.empty() && !variant) SaveRefused(what, original);
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
    // (15/10) the walls' shade on the lamp light around lots, as on the world grass next to the road (ChunkShadeTexture): the
    // copy with the shade read after the max, when the chunk has one and the copy is accepted
    IDirect3DTexture9* const shade = ChunkShadeTexture(dev, it->first);
    PatchedPs* const shaded = shade ? &PatchedFor(dev, g_roadShadePs, "Road with wall shade", [](std::vector<DWORD>& t, PatchedPs& pp) {
        return ShaderPatches::PatchRoad(t, pp.road, true);
    }, true) : nullptr;
    const bool useShade = shaded && shaded->ps && shaded->road.shadeSampler >= 0 && shaded->road.lightSampler == p.road.lightSampler &&
                          shaded->road.extraSampler == p.road.extraSampler && shaded->road.sidewalkConst == p.road.sidewalkConst &&
                          shaded->road.scaleConst == p.road.scaleConst;
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
        std::optional<SamplerBind> shadeMap;
        if (useShade) shadeMap.emplace(dev, static_cast<DWORD>(shaded->road.shadeSampler), shade, D3DTEXF_LINEAR);
        if (smooth) SetTex(dev, p.road.lightSampler, smooth);
        if (sidewalk) SetPsConst(dev, p.road.sidewalkConst, c, 1);
        SetPs(dev, useShade ? shaded->ps : p.ps);
        ConstGain lampGain(dev, p.road.scaleConst, RoadDayGain()); // the road's lamp scale, after the max with the terrain
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
// The centre and half extent (xz) of an instanced group: the mean of its instance positions (stream 1, the first float3
// of each record: world position, as the F7 capture reads them). Read once per (buffer, offset, count, stride) and again
// every 5 s, since the game may refill a buffer; render thread.
constexpr unsigned kGroupPieces = 32;
struct GroupCentre {
    float x = 0, y = 0, z = 0, ext = 0;
    float a[3] = {}, b[3] = {}; // the group's two ends (the instance farthest from the centre, then the one farthest from it)
    // (12/10, F7 18-17-00 #393: the centre of an L-shaped balcony band of 33 pieces stood in open air, 3.6 m from the nearest
    // piece) up to kGroupPieces of its pieces, evenly through the buffer: DrawInstanced tests each lamp from the one nearest it
    float piece[kGroupPieces][3] = {};
    unsigned pieces = 0;
    DWORD at = 0;
    bool ok = false;
};
std::unordered_map<uint64_t, GroupCentre> g_groupCentres;
bool ReadInstancePositions(const BYTE* p, UINT count, UINT stride, GroupCentre& g) {
    __try {
        double sx = 0, sy = 0, sz = 0;
        for (UINT i = 0; i < count; i++) {
            const float* v = reinterpret_cast<const float*>(p + static_cast<size_t>(i) * stride);
            if (!std::isfinite(v[0]) || !std::isfinite(v[1]) || !std::isfinite(v[2])) return false;
            sx += v[0];
            sy += v[1];
            sz += v[2];
        }
        g.x = static_cast<float>(sx / count);
        g.y = static_cast<float>(sy / count);
        g.z = static_cast<float>(sz / count);
        auto farthest = [&](float fx, float fz, float* out) {
            float best = -1.0f;
            for (UINT i = 0; i < count; i++) {
                const float* v = reinterpret_cast<const float*>(p + static_cast<size_t>(i) * stride);
                const float d = (v[0] - fx) * (v[0] - fx) + (v[2] - fz) * (v[2] - fz);
                if (d > best) {
                    best = d;
                    out[0] = v[0], out[1] = v[1], out[2] = v[2];
                }
            }
        };
        farthest(g.x, g.z, g.a);
        farthest(g.a[0], g.a[2], g.b);
        g.ext = 0.5f * std::sqrt((g.a[0] - g.b[0]) * (g.a[0] - g.b[0]) + (g.a[2] - g.b[2]) * (g.a[2] - g.b[2]));
        const UINT step = (count + kGroupPieces - 1) / kGroupPieces;
        g.pieces = 0;
        for (UINT i = 0; i < count && g.pieces < kGroupPieces; i += step) {
            const float* v = reinterpret_cast<const float*>(p + static_cast<size_t>(i) * stride);
            g.piece[g.pieces][0] = v[0], g.piece[g.pieces][1] = v[1], g.piece[g.pieces][2] = v[2];
            g.pieces++;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
std::unordered_map<uint64_t, uint8_t> g_centreBusy; // draw_reads_nowait: busy answers per group since its last read
const GroupCentre* InstancedGroupCentre(IDirect3DDevice9* dev) {
    UINT freq = 0;
    if (FAILED(dev->GetStreamSourceFreq(0, &freq)) || !(freq & D3DSTREAMSOURCE_INDEXEDDATA)) return nullptr;
    const UINT count = freq & 0x3FFFFFFFu;
    IDirect3DVertexBuffer9* vb = nullptr;
    UINT off = 0, stride = 0;
    if (FAILED(dev->GetStreamSource(1, &vb, &off, &stride)) || !vb) return nullptr;
    const GroupCentre* result = nullptr;
    if (count > 0 && count <= 4096 && stride >= 12 && stride <= 256) {
        if (g_groupCentres.size() > 4096) g_groupCentres.clear(), g_centreBusy.clear(); // groups come and go with lots
        const uint64_t key = (static_cast<uint64_t>(reinterpret_cast<uintptr_t>(vb)) << 32) ^ (static_cast<uint64_t>(off) << 12) ^ (static_cast<uint64_t>(count) << 1) ^ stride;
        GroupCentre& g = g_groupCentres[key];
        const DWORD now = GetTickCount();
        if (!g.at || now - g.at > 5000) {
            // draw_reads_nowait: a refresh of a group already read does not wait for the GPU; while the buffer is busy the
            // last positions stay in use and the next draw tries again (three busy answers: it waits, as before)
            uint8_t* busy = nullptr;
            if (DevSwitches::DrawReadsNoWait() && g.at && g.ok) busy = &g_centreBusy[key];
            const bool nowait = busy && *busy < 3;
            void* p = nullptr;
            const HRESULT hr = vb->Lock(off, count * stride, &p, D3DLOCK_READONLY | (nowait ? D3DLOCK_DONOTWAIT : 0));
            if (nowait && hr == D3DERR_WASSTILLDRAWING) {
                ++*busy;
                p = nullptr;
            } else {
                g.at = now ? now : 1;
                if (busy) *busy = 0;
                g.ok = SUCCEEDED(hr) && p && ReadInstancePositions(static_cast<const BYTE*>(p), count, stride, g);
            }
            if (p) vb->Unlock();
        }
        if (g.ok) result = &g;
    }
    vb->Release();
    return result;
}

// Light probe detail (Ctrl+Shift+F7) of the draw the mod is making (objects, windows, fences; DescribeDraw), render thread,
// only while a capture is recording; and the per-pixel lamps' law line (ReachLawText, below)
std::string g_objDrawInfo;
std::string ReachLawText(bool reach, bool refused, bool placed, float scale, const float (*r0Rows)[4], int nLamps);

template <typename DrawFn> bool DrawInstanced(IDirect3DDevice9* dev, DrawFn draw) {
    NoteFenceGroup(dev); // garden walls and fences shade the ground (ShadeWalls)
    if (!g_fenceFix.load(std::memory_order_relaxed)) return false;
    // (12/10, one daytime curve) No full-daylight gate any more: the pass runs at every level with its ground and lamp
    // strengths x LampDay(night) (3% by day), so a lamp left on by day fades on fences as on the walls behind them.
    const float night = g_night.load(std::memory_order_relaxed);
    float c[4];
    IDirect3DTexture9* atlas = LightmapSmooth::Atlas(c);
    if (!atlas) return false;
    PatchedPs* chosen = &PatchedFor(dev, g_fencePs, "Fence/stairs", [](std::vector<DWORD>& t, PatchedPs& pp) { return ShaderPatches::PatchInstancedLamps(t, pp.inst); });
    if (!chosen->ps) return false;
    // the group's pieces (per-pixel lamps on, a shader with them): read before the law is chosen, so a draw that will place
    // no lamp (option off, or a group that cannot be read) keeps the first law's lighter copy (12/10 review)
    const GroupCentre* g = chosen->inst.pixelLamps && g_objPixelLamps.load(std::memory_order_relaxed) ? InstancedGroupCentre(dev) : nullptr;
    // realistic reach (12/10): the copy with the outdoor walls' law, made only from a shader the first law takes; refused
    // (no room for its 2 rows), the first law stays (g_reachLawRefused)
    bool reachRefused = false;
    if (g && LevelLightShare::RealisticReachActive()) {
        PatchedPs& r = PatchedFor(
            dev, g_fencePsReach, "Fence/stairs (realistic reach)", [](std::vector<DWORD>& t, PatchedPs& pp) { return ShaderPatches::PatchInstancedLamps(t, pp.inst, true); }, true);
        if (r.ps) chosen = &r;
        else reachRefused = true, g_reachLawRefused++;
    }
    PatchedPs& p = *chosen;
    const bool reachLaw = p.inst.reachConst != 0;
    float oldA[4] = {}, oldB[4] = {};
    dev->GetPixelShaderConstantF(p.inst.atlasConst, oldA, 1);
    dev->GetPixelShaderConstantF(p.inst.strengthConst, oldB, 1);
    const float gain = TerrainLightingPolicy::SurfaceLampGain(g_night.load(std::memory_order_relaxed), g_fenceStrength.load(std::memory_order_relaxed));
    const float s[4] = {gain, 0, 0, 0};
    // Per-pixel lamps (ShaderPatches::PatchInstancedLamps pixelLamps): the lamps chosen around the group's centre, as for
    // objects; the atlas then only on faces turned up. Without a readable centre: no lamps and the atlas on every face, as
    // before.
    constexpr unsigned N = ShaderPatches::kObjectPixelLamps;
    // rows: (0, strength, 0, 1e-4), N x (pos, 1/R^2) (colour, spill), then N x the two cones (PatchInstancedLamps coneConst =
    // lampConst + 2N: one upload covers both blocks), then with reach the 2 rows of r0^2 (reachConst = coneConst + 2N)
    constexpr unsigned kLampRows = 1 + 4 * N;
    const unsigned rows = kLampRows + (reachLaw ? 2u : 0u);
    float lamps[kLampRows + 2][4] = {}, oldLamps[kLampRows + 2][4] = {}, weight[4] = {0.0f, 1.0f, 1.0f, 0.0f}, oldWeight[4] = {};
    int used = 0; // the lamps placed
    if (p.inst.pixelLamps) {
        lamps[0][3] = 1e-4f;
        for (unsigned k = 0; k < N; k++) lamps[1 + 2 * k][0] = lamps[1 + 2 * k][2] = 1e6f; // unused slot: far away, colour 0
        for (unsigned k = 0; k < N; k++) lamps[2 + 2 * k][3] = lamps[1 + 2 * N + 2 * k][3] = lamps[2 + 2 * N + 2 * k][3] = 1.0f; // no cone
        if (g) {
            const int n = SelectLamps(g->x, g->z, 40.0f + std::min(g->ext, 60.0f), true); // (with the lamps shining out of a house)
            // the highest head of the atlas lamps around: faces turned up above it see none of the atlas (below). A lamp shining
            // out of a house is not in the atlas and does not count (14/10: an upper story's indoor lamp raised the gate of the
            // parapets beside it, whose tops then took a ground post's whole pool)
            float lampTop = -1e9f;
            for (int k = 0; k < n; k++)
                GroundLightGate::NoteHead(lampTop, g_lampData[k][1], g_lampData[k][0] - g->x, g_lampData[k][2] - g->z, 15.0f + std::min(g->ext, 60.0f), !g_lampCone[k].light);
            GroundLightGate::Share groundShare; // the share of the atlas lamps' light the group sees (below)
            // Buildings block light on objects applies to fences too (a brick fence behind a tower took the wall
            // lamp on the other side). One lamp list serves the whole group, so a lamp leaves it only when a wall stands
            // between it and the group's centre and both its ends (0.5 m above the pieces): a fence wholly behind a wall
            // loses it, one that only runs past a wall keeps it. The placed floors of upper stories count too (12/10, the
            // rule objects follow, PickObjectLamps): the lamp keeps the largest share (LampVisibility) of the three points, so
            // a railing on a balcony loses a lamp under the slab and keeps one in front of it. (12/10, F7 18-17-00 #393: the
            // group's centre is not always a piece: an L-shaped band's stood in open air 3.6 m from its nearest piece, where a
            // lamp could be seen that no piece sees. The first point is the piece nearest the lamp instead; the centre only
            // when no piece was read.)
            const float mid[3] = {g->x, g->y + 0.5f, g->z}, endA[3] = {g->a[0], g->a[1] + 0.5f, g->a[2]}, endB[3] = {g->b[0], g->b[1] + 0.5f, g->b[2]};
            for (int k = 0; k < n; k++) { // (every lamp: the ground share counts those past the N slots too)
                const float* pr = g_lampData[k];
                const float* col = g_lampData[16 + k];
                float piece[3] = {mid[0], mid[1], mid[2]}, best = 3.0e38f;
                for (unsigned i = 0; i < g->pieces && i < kGroupPieces; i++) {
                    const float* q = g->piece[i];
                    const float dx = q[0] - pr[0], dy = q[1] + 0.5f - pr[1], dz = q[2] - pr[2], d = dx * dx + dy * dy + dz * dz;
                    if (d < best) {
                        best = d;
                        piece[0] = q[0], piece[1] = q[1] + 0.5f, piece[2] = q[2];
                    }
                }
                float v = PickedLampVisibility(k, pr, piece);
                if (v < 1.0f) v = std::max(v, PickedLampVisibility(k, pr, endA));
                if (v < 1.0f) v = std::max(v, PickedLampVisibility(k, pr, endB));
                { // its weight at the nearest piece, as PickObjectLamps weighs an object's ground share
                    const float r = pr[3] > 0.1f ? pr[3] : 0.1f, dx = pr[0] - piece[0], dy = pr[1] - piece[1], dz = pr[2] - piece[2];
                    const float w = (col[0] * 0.2126f + col[1] * 0.7152f + col[2] * 0.0722f) * LampConeAt(g_lampCone[k], pr, piece) / (1.0f + (dx * dx + dy * dy + dz * dz) / (r * r));
                    groundShare.Add(w, v < 0.01f ? 0.0f : v, !g_lampCone[k].light);
                }
                if (used >= static_cast<int>(N)) continue;
                if (v < 0.01f) { // under 1% left: the slot goes to the next lamp
                    g_fenceLampsLeftOut++;
                    continue;
                }
                if (v < 1.0f) g_fenceLampsDimmed++;
                const float r = pr[3] > 0.1f ? pr[3] : 0.1f;
                lamps[1 + 2 * used][0] = pr[0];
                lamps[1 + 2 * used][1] = pr[1];
                lamps[1 + 2 * used][2] = pr[2];
                lamps[1 + 2 * used][3] = 1.0f / (r * r);
                // its cones (07/10): the fence face 47 deg or more off a sconce's axis takes only its spill, as in the game
                const LampCone& cone = g_lampCone[k];
                float colourScale = v;
                if (reachLaw) { // the outdoor walls' law (lamp_reach.h): R, r0^2 and the game's weight of this lamp
                    float r0, R;
                    LampReach::Radii(cone.w, r0, R);
                    lamps[1 + 2 * used][3] = 1.0f / (R * R);
                    lamps[kLampRows + (used >> 2)][used & 3] = r0 * r0;
                    colourScale *= LampReach::Gain(cone.w, cone.entries);
                }
                lamps[2 + 2 * used][0] = col[0] * colourScale;
                lamps[2 + 2 * used][1] = col[1] * colourScale;
                lamps[2 + 2 * used][2] = col[2] * colourScale;
                lamps[2 + 2 * used][3] = cone.spill;
                std::memcpy(lamps[1 + 2 * N + 2 * used], cone.c1, 16);
                std::memcpy(lamps[2 + 2 * N + 2 * used], cone.c2, 16);
                if (!g_fenceConeLogged && HasCone(cone)) {
                    g_fenceConeLogged = true;
                    LOG_INFO(std::format("[LotLightBridge] Fence/stairs: per-pixel lamps follow their cones (first: the lamp at ({:.1f} {:.1f} {:.1f}), spill {:.3f})", pr[0], pr[1], pr[2], cone.spill));
                }
                used++;
            }
            lamps[0][1] = TerrainLightingPolicy::SurfaceLampGain(g_night.load(std::memory_order_relaxed), g_objPixelLampStrength.load(std::memory_order_relaxed)) *
                          std::max(g_fenceStrength.load(std::memory_order_relaxed), 0.0f);
            // reach: the outdoor walls' scale, the two sliders as trims over their defaults (x1 at the defaults: a band in
            // front of a wall takes the wall's light)
            if (reachLaw)
                lamps[0][1] = TerrainLightingPolicy::ReachLampScale(NativeWallScale(), night, g_wallEnabled.load(std::memory_order_relaxed), g_wallGain.load(std::memory_order_relaxed),
                                                                    g_objPixelLampStrength.load(std::memory_order_relaxed) / kPixelLampTrimUnit *
                                                                        (std::max(g_fenceStrength.load(std::memory_order_relaxed), 0.0f) / kPixelLampTrimUnit));
            // A valid per-instance group supplies world position/normal. Replace
            // its native group-wide estimate, including when wall tests reject
            // all selected lamps. Without a group or when pixel lights are off,
            // cS.x stays zero and the native vertex contribution is retained.
            lamps[0][0] = 1.0f;
            // No atlas at all with the lamps (the top of a deck's stone half wall took the ground light pool of
            // the wall lamp under the deck; the atlas is the light lying on the ground, at no height). Street and lot
            // lamps are in the lamp list, so fences are lit like walls: by their lamps, with angle and distance.
            // the atlas weight is sat(N.y x w0 + w1) (PatchInstancedLamps): (1, 0) keeps the ground light on faces turned up and
            // none on the sides, which take the lamps (with (0, 0) a curb's top went nearly black at night wherever
            // no lamp reached it, about 6 times darker than the game's own fence lighting, in blotches)
            weight[0] = 1.0f;
            weight[1] = 0.0f;
            // (12/10, screenshot with a red garden post: the top of a story-4 balustrade, 8 m above the post, glowed with the
            // ground's lamp pool) The atlas is the light on the ground: a group standing above every lamp head around takes
            // none of it on its tops, fading over the first 1.5 m above the highest head (lamp heads, not lanterns: game
            // data). Ground fences and curbs, below the heads, keep it all. (14/10) The atlas has no walls or slabs either: the
            // tops take it x the share of the atlas lamps' light the group sees (the objects' ground share), so a parapet over
            // the slab of an arcade takes none of the pool of the lamp under that slab.
            weight[0] = GroundLightGate::HeightGate(g->y, lampTop) * groundShare.Value();
        }
        dev->GetPixelShaderConstantF(p.inst.lampParamConst, &oldLamps[0][0], rows);
        dev->GetPixelShaderConstantF(p.inst.groundWeightConst, oldWeight, 1);
    }
    // Light probe (12/10 review: fence and balcony band draws showed only "mod draw (another fix)", so their law could not be
    // read from an F7): the lamps placed, the strengths and the lamp law
    if (LightProbe::Capturing())
        g_objDrawInfo = std::format("FENCE/STAIRS (instanced) lit by the mod | per-pixel lamps: {}{} | lamp strength {:.4f} | ground light strength {:.2f}, atlas weight sat(N.y x {:.2f} + {:.2f})",
                                    used, !p.inst.pixelLamps ? " (this shader has none: ground light only)" : g ? "" : " (group not read or option off: the game's vertex lights)",
                                    lamps[0][1], s[0], weight[0], weight[1]) +
                        (p.inst.pixelLamps ? ReachLawText(reachLaw, reachRefused, g != nullptr, lamps[0][1], lamps + kLampRows, used) : std::string());
    if (reachLaw) g_reachFenceDraws++;
    IDirect3DPixelShader9* original = g_curPs;
    g_inOwnCall = true;
    {
        SamplerBind bind(dev, p.inst.atlasSampler, atlas, D3DTEXF_NONE);
        SetPsConst(dev, p.inst.atlasConst, c, 1);
        SetPsConst(dev, p.inst.strengthConst, s, 1);
        if (p.inst.pixelLamps) {
            SetPsConst(dev, p.inst.lampParamConst, &lamps[0][0], rows);
            SetPsConst(dev, p.inst.groundWeightConst, weight, 1);
        }
        // The lot ground's lamp shade (13/10, F7 2026-10-13 09-31-30 #268: the curbs of a flowerbed behind a free-standing
        // wall stayed lit by the garden post on the other side). The group's one lamp list keeps a lamp that any of its points
        // sees, and the pixel has no wall test; the copy multiplies its lamp sum by the shade the lot pass gives the grass
        // (FloorShadeBind, as the story-0 floors), gated to about 1 m over the lot's ground per pixel, so balcony railings and
        // stair tops above it keep their own lamps. The lot: the one under the group's centre; none (or no group read): white
        // and gate 0, the draw as before.
        FloorShadeBind shade(dev, p.inst.shadeSampler, p.inst.shadeConst, g ? FindLotAt(g->x, g->z) : nullptr, 1.0f, g_fenceShadeLot, g_fenceShadeNoLot);
        if (LightProbe::Capturing()) g_objDrawInfo += "\n      " + shade.Text();
        SetPs(dev, p.ps);
        draw();
        SetPs(dev, original);
        if (p.inst.pixelLamps) {
            SetPsConst(dev, p.inst.groundWeightConst, oldWeight, 1);
            SetPsConst(dev, p.inst.lampParamConst, &oldLamps[0][0], rows);
        }
        SetPsConst(dev, p.inst.strengthConst, oldB, 1);
        SetPsConst(dev, p.inst.atlasConst, oldA, 1);
    }
    g_objDrawInfo.clear();
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
    const float night = g_night.load(std::memory_order_relaxed);
    if (night <= 0.01f) return false; // by day, keep the game's normal snow/object lighting; only the night ground light is added
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
    const float s[4] = {TerrainLightingPolicy::SurfaceLampGain(g_night.load(std::memory_order_relaxed),
                       g_fenceStrength.load(std::memory_order_relaxed)), 0, 0, 0};
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
        const std::vector<DWORD> code = PatchedVsCode(vs, g_curVsInfo->cls);
        if (code.empty() || FAILED(dev->CreateVertexShader(code.data(), &f.vs))) f.vs = nullptr;
        LOG_INFO(std::format("[LotLightBridge] Outdoor object: vertex shader {:08X} {}", reinterpret_cast<uintptr_t>(vs), f.vs ? "patched" : "failed"));
    }
    return f.vs;
}

// Light probe detail (Ctrl+Shift+F7) for an object the mod lights: its position, the lamps it got and the game's own
// rig values before they are zeroed, so two neighbouring pieces can be compared. Only while a capture is recording.
// (g_objDrawInfo itself is defined before DrawInstanced, which fills it for fences too.)
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

// Light probe detail of the per-pixel lamps' law (12/10): the reach law's scale and each lamp's r0 (its R is the radius the
// lamp list shows, its colour includes the lamp's weight), or why the first law stayed (refused: the reach copy had no room
// for its rows; placed: the lamps were placed for this draw)
std::string ReachLawText(bool reach, bool refused, bool placed, float scale, const float (*r0Rows)[4], int nLamps) {
    if (!reach)
        return refused                                  ? "\n      lamp law: first law, sat(1 - d^2/R^2)^2 (realistic reach on, but the reach copy of this shader was refused: no room for its rows, or D3D; see the log)"
               : LevelLightShare::RealisticReachActive() ? "\n      lamp law: first law, sat(1 - d^2/R^2)^2 (realistic reach on, per-pixel lamps off or not placed for this draw)"
                                                         : "\n      lamp law: first law, sat(1 - d^2/R^2)^2 (realistic reach off)";
    if (!placed) return "\n      lamp law: realistic reach copy, but no lamp placed for this draw (world position not read): lamp strength 0";
    std::string s = std::format("\n      lamp law: realistic reach, as outdoor walls: colour (F0 x intensity x fade x visible share x weight 0.25 w x the lamp's list "
                                "entries, 2 for a story-0 lot lamp, else 1) x sat(N.l) / d^2 x min(1, r0^2/d^2 x (1 - (d/R)^4)^2), sum held under 1 | scale {:.4f} = "
                                "the walls' native lamp scale {:.4f}{} (before full night the daytime curve instead) x their gain {:.2f} ({}) x the slider trims | r0:",
                                scale, NativeWallScale(), NativeWallFresh() ? "" : " (none read in the last 2 s: the daytime curve's value)", g_wallGain.load(),
                                g_wallEnabled.load() ? "walls on" : "walls off: gain unused");
    for (int k = 0; k < nLamps && k < static_cast<int>(ShaderPatches::kObjectPixelLamps); k++) s += std::format(" [{}] {:.2f} m", k, std::sqrt(r0Rows[k >> 2][k & 3]));
    return s;
}

// The per-pixel lamps of an object at translation (x, y, z): SelectLamps there, then Buildings block light on objects
// (LevelLightShare::LampVisibility): a lamp with an outside wall between it and the object's middle (its origin +
// 0.5 m), or with a placed floor of an upper story between it and a middle above it (12/10, F7 18-38-41: a window frame
// behind a balcony showed the red of a ground post under the slab), is left out, and the next one takes its slot; one seen
// past a slab's edge keeps the share v that reaches the middle (colour x v). groundShare (left as it is when no lamp is near)
// = the share of the nearby lamps' light that is not blocked: the ground light (the atlas, which has no walls) is scaled
// by it. lampTop, when given (left as it is when no lamp is near): the highest lamp head within 15 m along the ground
// (DrawInstanced's rule for fences). Fills lamps[1 + 2 k] = (pos, 1/R^2). With cone output, colour stays unscaled and
// the shader evaluates each cone per pixel; otherwise colour uses the origin cone. With reachRows (realistic reach, 12/10):
// the outdoor walls' law (lamp_reach.h), R its R, the colour x the lamp's weight Gain(w, entries), and r0^2 at reachRows[k / 4][k % 4].
// Returns the lamp count.
// exitLamps (outdoor objects, 2.12.4): also the indoor lamps shining out of a house through an opening (not for windows: their
// outdoor face would take the lamp of the room behind them)
int PickObjectLamps(float x, float y, float z, float (*lamps)[4], float& groundShare, float nearSkip, float (*cones)[4] = nullptr, float* lampTop = nullptr,
                    float (*reachRows)[4] = nullptr, bool exitLamps = false) {
    constexpr unsigned N = ShaderPatches::kObjectPixelLamps;
    int nLamps = 0;
    const int n = SelectLamps(x, z, 40.0f, exitLamps);
    const float obj[3] = {x, y + 0.5f, z};
    float seen = 0.0f, all = 0.0f;
    for (int k = 0; k < n; k++) {
        const float* pr = g_lampData[k];
        const float* col = g_lampData[16 + k];
        const float r = pr[3] > 0.1f ? pr[3] : 0.1f;
        const float dx = pr[0] - obj[0], dy = pr[1] - obj[1], dz = pr[2] - obj[2];
        if (lampTop) GroundLightGate::NoteHead(*lampTop, pr[1], dx, dz, 15.0f, !g_lampCone[k].light); // atlas lamps that can light its pool
        // the lamp's cones at the object's middle (07/10): one factor per object, as the game's own rule gives it
        const float cone = LampConeAt(g_lampCone[k], pr, obj);
        if (cone < 0.999f && !g_lampConeLogged) {
            g_lampConeLogged = true;
            LOG_INFO(std::format("[LotLightBridge] Per-pixel lamps follow their cones (first: the lamp at ({:.1f} {:.1f} {:.1f}) gives {:.2f} at the object at ({:.1f} {:.1f} {:.1f}))", pr[0], pr[1], pr[2], cone, obj[0], obj[1], obj[2]));
        }
        const float w = (col[0] * 0.2126f + col[1] * 0.7152f + col[2] * 0.0722f) * cone / (1.0f + (dx * dx + dy * dy + dz * dz) / (r * r));
        const bool inBake = !g_lampCone[k].light; // (a lamp shining out of a house is not in the ground atlas: not in its share)
        if (inBake) all += w;
        const float v = PickedLampVisibility(k, pr, obj, nearSkip); // 1 while the option is off (an outdoor lamp)
        if (v < 0.01f) { // under 1% left: the slot goes to the next lamp
            g_objLampsLeftOut++;
            continue;
        }
        if (v < 1.0f) g_objLampsDimmed++;
        if (inBake) seen += w * v;
        if (nLamps >= static_cast<int>(N)) continue;
        const int slot = nLamps++;
        lamps[1 + 2 * slot][0] = pr[0];
        lamps[1 + 2 * slot][1] = pr[1];
        lamps[1 + 2 * slot][2] = pr[2];
        lamps[1 + 2 * slot][3] = 1.0f / (r * r);
        float gain = 1.0f;
        if (reachRows) {
            float r0, R;
            LampReach::Radii(g_lampCone[k].w, r0, R);
            lamps[1 + 2 * slot][3] = 1.0f / (R * R);
            reachRows[slot >> 2][slot & 3] = r0 * r0;
            gain = LampReach::Gain(g_lampCone[k].w, g_lampCone[k].entries);
        }
        const float colorFactor = (cones ? 1.0f : cone) * v * gain;
        lamps[2 + 2 * slot][0] = col[0] * colorFactor;
        lamps[2 + 2 * slot][1] = col[1] * colorFactor;
        lamps[2 + 2 * slot][2] = col[2] * colorFactor;
        if (cones) {
            lamps[2 + 2 * slot][3] = g_lampCone[k].spill;
            std::memcpy(cones[2 * slot], g_lampCone[k].c1, 16);
            std::memcpy(cones[2 * slot + 1], g_lampCone[k].c2, 16);
        }
    }
    if (all > 1e-5f) groundShare = std::clamp(seen / all, 0.0f, 1.0f);
    return nLamps;
}

// The colour an outdoor object's per-pixel lamp gives at `point` (2.12.4): DrawObjectLamp's law and scale for the current
// night (the reach law with the outdoor walls' scale and the per-pixel lamp slider as a trim, else the first law with its
// strength), the lamp's cones, held under 1 per channel; before N.l and any wall test. False when the per-pixel lamps are
// off or the lamp is none of theirs (an outdoor lamp, a street lamp, or an indoor lamp of the lamp classes shining out). Any
// thread: the lamp is read under SEH, the settings atomically (the walls' native scale is the render thread's last read).
bool ObjectLampColourAtImpl(const void* light, const float* centre, float rgb[3], const float* ext = nullptr) {
    if (!g_objPixel.load(std::memory_order_relaxed) || !g_objPixelLamps.load(std::memory_order_relaxed) || !light || !centre) return false;
    float v[8];
    LampCone cone;
    const uintptr_t L = reinterpret_cast<uintptr_t>(light);
    if (!ReadLamp(L, v, cone) && !ReadExitLamp(L, v, cone)) return false;
    // with a box (ext = its half extents around centre): its point nearest the lamp's head
    float nearPt[3] = {centre[0], centre[1], centre[2]};
    if (ext)
        for (int k = 0; k < 3; k++) nearPt[k] = std::clamp(v[k], centre[k] - ext[k], centre[k] + ext[k]);
    const float* point = nearPt;
    const float dx = v[0] - point[0], dy = v[1] - point[1], dz = v[2] - point[2], d2 = dx * dx + dy * dy + dz * dz;
    if (!std::isfinite(d2)) return false;
    const float night = g_night.load(std::memory_order_relaxed);
    float law, scale;
    if (LevelLightShare::RealisticReachActive()) {
        law = LampReach::PixelLaw(d2, cone.w, cone.entries);
        scale = TerrainLightingPolicy::ReachLampScale(NativeWallScale(), night, g_wallEnabled.load(std::memory_order_relaxed), g_wallGain.load(std::memory_order_relaxed),
                                                      g_objPixelLampStrength.load(std::memory_order_relaxed) / kPixelLampTrimUnit);
    } else {
        law = LampReach::FirstLaw(d2, v[3]);
        scale = TerrainLightingPolicy::SurfaceLampGain(night, g_objPixelLampStrength.load(std::memory_order_relaxed));
    }
    const float k = law * scale * LampConeAt(cone, v, point);
    for (int c = 0; c < 3; c++) rgb[c] = std::clamp(v[4 + c] * k, 0.0f, 1.0f);
    return std::isfinite(rgb[0]) && std::isfinite(rgb[1]) && std::isfinite(rgb[2]);
}

// ---- Per-vertex lamp light on instanced foliage (numbered development builds, switch foliage_vertex_lamps;
// foliage_vertex_lamps.h) and the crowns as porous occluders of lamp light on lot ground (switch canopy_ground_shadow;
// foliage_canopy.h). The foliage vertex shader's instance constants (VS_9A10A3A8, the captured family): c0[3i] the
// instance's position relative to the camera, c2[3i].x its scale, c27..c29[3i] the rig's lamp directions, c54..c56[3i] its
// lamp colours (taken by ObjectLampColourAt at the rig box's point nearest each lamp). Each instance picks its own three
// lamps (PickFoliageLamps, the per-pixel lamps' rule) and the draw replaces c54..c80 with their colours; the rig's lamp
// directions only find the space of c0 (MatchLamps, both ways). ----
struct FoliageCrown {
    float lo[3] = {}, hi[3] = {}; // the mesh's local box (positions / w), read once per mesh
    int instances = 9;            // 1 + the largest instance index in the mesh (TEXCOORD3.w, a0.w = 3 x it)
    bool ok = false;
};
std::unordered_map<uint64_t, FoliageCrown> g_foliageMeshes;
struct FoliageInstance { // world space, so a moving camera reuses it; the colour terms are recomputed every draw
    float centre[3] = {}, rx = 0, ry = 0;
    size_t lamp[3] = {SIZE_MAX, SIZE_MAX, SIZE_MAX}; // the matched g_allLamps entry of each lit slot (finding the origin of c0)
    FoliageVertexLamps::OwnLamp own[3];              // the instance's own lamps (PickFoliageLamps), brightest first
    int owned = 0;
    uint32_t pickGen = 0; // the trees' lamp list (g_foliageLampGen) the lamps were picked from
    DWORD pickTick = 0;   // when
    uint64_t pickState = 0;                        // the walls and floors it read (LevelLightShare::LampVisibilityState)
    uint32_t pickColourGen = 0, colourGen = 0;     // the colours it was picked with, and the ones its lamps carry
    bool shaped = false, picked = false, counted = false;
    DWORD seen = 0;
};
std::unordered_map<uint64_t, FoliageInstance> g_foliageInstances;
std::atomic<long> g_foliageVertexDrawn{0}, g_foliageVertexRefused{0};
// lit slots of new instances: no lamp found by direction, no law (cone), drawn by the law alone (the rig's colour brighter
// than the law at the mesh box), drawn by the law with the rig's shade
// new instances by their own lamps (0, 1, 2, 3), lamps left out by the walls, instances without a crown shape
std::atomic<long> g_foliageOwned[4] = {}, g_foliageWallOut{0}, g_foliageNoShape{0};
std::atomic<long> g_foliageRepickChanged{0}, g_foliageRepickSame{0}; // picks made again: the light changed / stayed
// Crowns seen by foliage draws (world space) for the ground shade, kept while they are within kCrownKeep of the camera:
// not by view (a crown turned out of view still shades the ground), one entry per crown (merged by place, so the camera's
// small offsets never make two of the same tree). Render thread.
struct CrownSeen {
    Canopy::Ellipsoid e{};
    DWORD first = 0;
};
std::vector<CrownSeen> g_crowns;
std::atomic<long> g_crownCount{0};
constexpr float kCrownKeep = 250.0f; // m from the camera
constexpr size_t kMaxCrowns = 8192;

// Instance count and the local box of the current foliage mesh (stream 0, its draw range), read once per mesh
bool FoliageMeshBox(IDirect3DDevice9* dev, FoliageCrown& out) {
    IDirect3DVertexDeclaration9* decl = nullptr;
    if (FAILED(dev->GetVertexDeclaration(&decl)) || !decl) return false;
    D3DVERTEXELEMENT9 el[MAXD3DDECLLENGTH + 1];
    UINT n = MAXD3DDECLLENGTH + 1;
    FenceLayout L;
    bool pos = false, idx = false;
    WORD idxOff = 0;
    BYTE idxType = 0;
    if (SUCCEEDED(decl->GetDeclaration(el, &n)))
        for (UINT i = 0; i < n && el[i].Stream != 0xFF; i++) {
            if (el[i].Stream != 0) continue;
            if (el[i].Usage == D3DDECLUSAGE_POSITION && el[i].UsageIndex == 0) L.posOff = el[i].Offset, L.posType = el[i].Type, pos = true;
            if (el[i].Usage == D3DDECLUSAGE_TEXCOORD && el[i].UsageIndex == 3) idxOff = el[i].Offset, idxType = el[i].Type, idx = true;
        }
    decl->Release();
    if (!pos) return false;
    IDirect3DVertexBuffer9* vb = nullptr;
    UINT off = 0, stride = 0;
    if (FAILED(dev->GetStreamSource(0, &vb, &off, &stride)) || !vb) return false;
    const UINT first = g_curDipBase + g_curDipMin, count = g_curDipNum;
    const uint64_t key = (static_cast<uint64_t>(reinterpret_cast<uintptr_t>(vb)) << 32) ^ (static_cast<uint64_t>(off + first * stride) << 8) ^ count ^
                         (static_cast<uint64_t>(stride) << 56) ^ (static_cast<uint64_t>(L.posType) << 50) ^ L.posOff;
    auto it = g_foliageMeshes.find(key);
    if (it == g_foliageMeshes.end()) {
        if (g_foliageMeshes.size() > 4096) g_foliageMeshes.clear();
        FoliageCrown c;
        D3DVERTEXBUFFER_DESC vd{};
        void* p = nullptr;
        // a static buffer, read once per mesh
        if (count && count <= 65536 && stride >= 12 && stride <= 256 && SUCCEEDED(vb->GetDesc(&vd)) &&
            static_cast<uint64_t>(off) + (static_cast<uint64_t>(first) + count) * stride <= vd.Size &&
            SUCCEEDED(vb->Lock(off + first * stride, count * stride, &p, D3DLOCK_READONLY)) && p) {
            c.ok = MeshBox(static_cast<const BYTE*>(p), count, stride, L, c.lo, c.hi);
            if (c.ok && idx) {
                float top = -1.0f;
                for (UINT i = 0; i < count && c.ok; i++) {
                    float v[4];
                    if (!ReadElement(static_cast<const BYTE*>(p) + static_cast<size_t>(i) * stride + idxOff, idxType, v) || !std::isfinite(v[3])) break;
                    top = std::max(top, v[3]);
                }
                if (top >= 0.0f && top < 9.0f) c.instances = static_cast<int>(top + 0.5f) + 1;
            }
        }
        if (p) vb->Unlock();
        it = g_foliageMeshes.emplace(key, c).first;
    }
    vb->Release();
    out = it->second;
    return out.ok;
}

// The lamps of one foliage instance, by the per-pixel lamps' own rule instead of the rig's three slots: the lamps
// within reach of the crown's middle (SelectLamps, with the indoor lamps shining out of a house), each with its cones and
// its share past the outside walls and slabs at that middle (PickedLampVisibility), ranked by the light the reach law gives
// at the mesh box's point nearest the lamp (trunk included); a lamp whose reach ends before the box is left out. The rig's
// slots matched under 2% of the lit slots in game (log 20:34:32, public 2.14: 7,870 by the law against 483,925 without a
// lamp), and an unmatched slot lit the whole crown with one colour. Render thread (SelectLamps' memo, g_lampData).
int PickFoliageLamps(const float* mid, const float* boxC, const float* ext, FoliageVertexLamps::OwnLamp* out) {
    // the trees' own list (ReadFoliageLamps: never held for a switch or a drag); a lamp whose reach ends before the mesh box
    // gives none (the law is 0 past R, 20 m at most), so only the near ones are tested past the walls
    int kept = 0;
    for (size_t k = 0; k < g_foliageLamps.size() && k < g_foliageCones.size(); k++) {
        const float* pr = g_foliageLamps[k].data();
        const float* col = pr + 4;
        const LampCone& cone = g_foliageCones[k];
        const float d2 = FoliageVertexLamps::BoxDistance2(pr, boxC, ext);
        if (!(d2 < 20.0f * 20.0f)) continue;
        const float law = FoliageVertexLamps::FoliageLaw(d2, cone.w, cone.entries); // (inverse square, faded near R)
        if (!(law > 0.0f)) continue;
        const float cn = LampConeAt(cone, pr, mid);
        const float luma = (col[0] * 0.2126f + col[1] * 0.7152f + col[2] * 0.0722f) * cn * law;
        if (!(luma > 1e-5f) || !std::isfinite(luma)) continue;
        const float v = LampConeVisibility(cone, pr, mid, 0.2f);
        if (v < 0.01f) {
            g_foliageWallOut.fetch_add(1, std::memory_order_relaxed);
            continue;
        }
        FoliageVertexLamps::OwnLamp l;
        float r0, R;
        LampReach::Radii(cone.w, r0, R);
        for (int a = 0; a < 3; a++) l.pos[a] = pr[a], l.colour[a] = col[a] * cn * v;
        l.r0sq = R * R, l.invR2 = 1.0f / (R * R), l.gain = LampReach::Gain(cone.w, cone.entries), l.score = luma * v;
        l.key = FoliageShapeKey(g_foliageLamps[k], cone), l.share = cn * v;
        kept = FoliageVertexLamps::KeepBrightest(l, out, kept);
    }
    return kept;
}

// The crown of one instance from its mesh box: the leaves, not the trunk under them. A plant taller than 2.5 m keeps its
// upper 65% as the crown (a tree's clear trunk); smaller plants are leaves from the ground up. The box's centre over the
// instance (upright foliage, modelled around its trunk) and its widest horizontal half extent both ways (no yaw needed).
bool CrownShape(const float* inst, float scale, const float eye[3], const FoliageCrown& mesh, FoliageInstance& f) {
    if (!(scale > 0.01f && scale < 100.0f)) return false;
    float lo = mesh.lo[1], hi = mesh.hi[1];
    if ((hi - lo) * scale > 2.5f) lo += 0.35f * (hi - lo);
    f.centre[0] = inst[0] + eye[0];
    f.centre[1] = inst[1] + eye[1] + 0.5f * (lo + hi) * scale;
    f.centre[2] = inst[2] + eye[2];
    f.rx = std::max(0.5f * (mesh.hi[0] - mesh.lo[0]), 0.5f * (mesh.hi[2] - mesh.lo[2])) * scale;
    f.ry = 0.5f * (hi - lo) * scale;
    return f.rx > 0.1f && f.ry > 0.15f && f.rx < 30.0f && f.ry < 30.0f && std::isfinite(f.centre[0] + f.centre[1] + f.centre[2]);
}

// Each lit slot's lamp, by direction from the crown's centre (the rig's centre differs a little: 16 degrees at most)
// (each slot on its own: a slot that matches no lamp keeps lamp SIZE_MAX; true when one matched)
bool MatchLamps(const float (*dirs)[4], const float (*cols)[4], FoliageInstance& f) {
    bool any = false;
    for (int k = 0; k < 3; k++) {
        f.lamp[k] = SIZE_MAX;
        if (!(std::max({cols[k][0], cols[k][1], cols[k][2]}) > 1.0f / 512.0f)) continue; // unused slot
        const float dl = std::sqrt(dirs[k][0] * dirs[k][0] + dirs[k][1] * dirs[k][1] + dirs[k][2] * dirs[k][2]);
        if (!(dl > 0.5f)) continue;
        float best = 0.90f; // the rig's centre (its whole box) and ours (the crown) differ: about 25 degrees at most
        for (size_t j = 0; j < g_allLamps.size(); j++) {
            const auto& v = g_allLamps[j];
            const float lx = v[0] - f.centre[0], ly = v[1] - f.centre[1], lz = v[2] - f.centre[2], l2 = lx * lx + ly * ly + lz * lz;
            if (!(l2 > 1e-4f) || l2 > 40.0f * 40.0f) continue;
            const float c = (lx * dirs[k][0] + ly * dirs[k][1] + lz * dirs[k][2]) / (std::sqrt(l2) * dl);
            if (c > best) best = c, f.lamp[k] = j;
        }
        any = any || f.lamp[k] != SIZE_MAX;
    }
    return any;
}

// One crown into g_crowns: merged with a crown already there within 0.25 m and 15% of its size
void NoteCrown(const FoliageInstance& f) {
    for (CrownSeen& c : g_crowns) {
        const float dx = c.e.cx - f.centre[0], dy = c.e.cy - f.centre[1], dz = c.e.cz - f.centre[2];
        if (dx * dx + dy * dy + dz * dz < 0.25f * 0.25f && std::fabs(c.e.rx - f.rx) < 0.15f * f.rx && std::fabs(c.e.ry - f.ry) < 0.15f * f.ry) return;
    }
    if (g_crowns.size() >= kMaxCrowns) return;
    CrownSeen c;
    c.e = {f.centre[0], f.centre[1], f.centre[2], f.rx, f.ry, f.rx, 1.0f, 0.0f, Canopy::LadFor(2.0f * f.ry)};
    c.first = GetTickCount() | 1;
    g_crowns.push_back(c);
    g_crownCount.store(static_cast<long>(g_crowns.size()), std::memory_order_relaxed);
}

// Fills c[B .. B+81] for the draw (perVertex) and notes the crowns (ground shade). false: a lit slot could not be matched
// or confirmed (the draw keeps the old shader)
// The space of the instance positions c0[3i]: 1 = relative to the camera eye, 2 = world; 0 while unknown. Found by
// matching instances' lamp slots to the outdoor lamps both ways (at least 8 matches one way and at most 1 the other);
// nothing (no crown, no per-vertex draw) uses a position before.
std::atomic<int> g_foliageOrigin{0};
int g_originVotes[3] = {};
bool FoliageOriginFound(float origin[3]) {
    float eye[3];
    const bool haveEye = LotLightingMotion::CameraEye(eye);
    const int mode = g_foliageOrigin.load(std::memory_order_relaxed);
    if (mode == 2) return origin[0] = origin[1] = origin[2] = 0.0f, true;
    if (mode == 1 && haveEye) return std::memcpy(origin, eye, sizeof eye), true;
    return false;
}
void VoteFoliageOrigin(const float (*c)[4], int count, const FoliageCrown& mesh) {
    if (g_foliageOrigin.load(std::memory_order_relaxed)) return;
    float eye[3];
    const bool haveEye = LotLightingMotion::CameraEye(eye);
    const float zero[3] = {};
    for (int i = 0; i < count; i++) {
        bool lit = false;
        for (int k = 0; k < 3; k++) lit = lit || std::max({c[54 + 3 * i + k][0], c[54 + 3 * i + k][1], c[54 + 3 * i + k][2]}) > 0.05f;
        if (!lit) continue;
        for (int m = 1; m <= 2; m++) {
            if (m == 1 && !haveEye) continue;
            FoliageInstance f;
            if (CrownShape(c[3 * i], c[3 * i + 2][0], m == 1 ? eye : zero, mesh, f) && MatchLamps(&c[27 + 3 * i], &c[54 + 3 * i], f)) g_originVotes[m]++;
        }
    }
    for (int m = 1; m <= 2; m++)
        if (g_originVotes[m] >= 8 && g_originVotes[3 - m] <= 1) {
            g_foliageOrigin.store(m, std::memory_order_relaxed);
            LOG_INFO(std::format("[Foliage] Instance positions are {} ({} matches, {} the other way)", m == 1 ? "relative to the camera" : "world positions",
                                 g_originVotes[m], g_originVotes[3 - m]));
        }
    if (g_originVotes[1] + g_originVotes[2] > 200 && !g_foliageOrigin.load(std::memory_order_relaxed)) { // undecided: start over
        static int s_noted = 0;
        if (s_noted++ < 3) LOG_INFO(std::format("[Foliage] Instance position space undecided ({} / {} matches): per-vertex lamps and crowns wait", g_originVotes[1], g_originVotes[2]));
        g_originVotes[1] = g_originVotes[2] = 0;
    }
}

// The eye the game drew a foliage draw with (FoliageVertexLamps::EyeAnchors): while the eye read from the game's camera has
// not changed for half a second, both agree and the draw's instances are learned as fixed places; while it moves, the
// offset that puts the instances back on those places is added to it (one offset per eye read, reused by the draws that
// follow with the same read; the last one found when a draw knows no place yet). Render thread.
extern uint32_t g_roomMapFrame; // (below: OnPresent counts frames)
FoliageVertexLamps::EyeAnchors g_eyeAnchors;
float g_eyeLast[3] = {}, g_eyeOffset[3] = {}, g_eyeOffsetFor[3] = {1e30f, 0, 0};
DWORD g_eyeChanged = 0;
std::atomic<float> g_eyeDriftMax{0.0f}; // the largest correction since the last log line (m)
uint32_t FoliageMeshId(const FoliageCrown& mesh) {
    uint32_t h = 2166136261u;
    for (int k = 0; k < 3; k++) h = (h ^ static_cast<uint32_t>(std::lround(mesh.lo[k] * 100.0f))) * 16777619u, h = (h ^ static_cast<uint32_t>(std::lround(mesh.hi[k] * 100.0f))) * 16777619u;
    return (h ^ static_cast<uint32_t>(mesh.instances)) * 16777619u;
}
void CorrectFoliageEye(const float (*c)[4], int count, const FoliageCrown& mesh, float eye[3], DWORD now) {
    if (std::fabs(eye[0] - g_eyeLast[0]) + std::fabs(eye[1] - g_eyeLast[1]) + std::fabs(eye[2] - g_eyeLast[2]) > 1e-3f) {
        std::memcpy(g_eyeLast, eye, sizeof g_eyeLast);
        g_eyeChanged = now;
    }
    const uint32_t meshId = FoliageMeshId(mesh);
    float rel[9][3], scales[9];
    for (int i = 0; i < count; i++) {
        for (int k = 0; k < 3; k++) rel[i][k] = c[3 * i][k];
        scales[i] = c[3 * i + 2][0];
    }
    if (now - g_eyeChanged >= 300) { // still: the read eye is the game's; learn the places, no offset
        for (int i = 0; i < count; i++) {
            const float w[3] = {rel[i][0] + eye[0], rel[i][1] + eye[1], rel[i][2] + eye[2]};
            if (std::isfinite(w[0] + w[1] + w[2])) g_eyeAnchors.Learn(meshId, scales[i], w);
        }
        g_eyeOffset[0] = g_eyeOffset[1] = g_eyeOffset[2] = 0.0f;
        std::memcpy(g_eyeOffsetFor, eye, sizeof g_eyeOffsetFor);
        return;
    }
    // once per frame (the game's eye can move while the read one stays, as the camera settles): the offset this frame's
    // first draw with learned places finds (else the last one)
    static uint32_t s_frame = 0;
    if (s_frame != g_roomMapFrame) {
        float o[3];
        if (g_eyeAnchors.Offset(meshId, scales, rel, count, eye, o)) {
            s_frame = g_roomMapFrame;
            std::memcpy(g_eyeOffset, o, sizeof o);
            std::memcpy(g_eyeOffsetFor, eye, sizeof g_eyeOffsetFor);
            const float len = std::sqrt(o[0] * o[0] + o[1] * o[1] + o[2] * o[2]);
            if (len > g_eyeDriftMax.load(std::memory_order_relaxed)) g_eyeDriftMax.store(len, std::memory_order_relaxed);
        }
    }
    for (int k = 0; k < 3; k++) eye[k] += g_eyeOffset[k];
}

bool FoliageVertexConstants(IDirect3DDevice9* dev, bool perVertex, std::vector<float>& out) {
    FoliageCrown mesh;
    if (!FoliageMeshBox(dev, mesh)) return false;
    const int count = std::clamp(mesh.instances, 1, 9);
    float c[81][4];
    if (FAILED(dev->GetVertexShaderConstantF(0, &c[0][0], 81))) return false;
    float eye[3]; // the origin of c0 (named eye below: the camera eye, or 0 for world positions)
    if (!FoliageOriginFound(eye)) {
        VoteFoliageOrigin(c, count, mesh);
        return false;
    }
    // c[B .. B+81], then the colours that replace c54..c80 for the draw
    if (perVertex) out.assign((82 + 27) * 4, 0.0f);
    const DWORD now = GetTickCount() | 1;
    // too many instances (grass and flowers are instances too): the ones not drawn for 3 s go, never the ones in view (a
    // cleared map made every crown in view pick again in the same frame)
    if (g_foliageInstances.size() > 16384) {
        std::erase_if(g_foliageInstances, [now](const auto& p) { return now - p.second.seen > 3000; });
        // still over: the ones not drawn for 250 ms go (a cleared map on every draw made each instance pick again urgently)
        if (g_foliageInstances.size() > 16384) std::erase_if(g_foliageInstances, [now](const auto& p) { return now - p.second.seen > 250; });
        if (g_foliageInstances.size() > 32768) g_foliageInstances.clear();
    }
    if (g_foliageOrigin.load(std::memory_order_relaxed) == 1) CorrectFoliageEye(c, count, mesh, eye, now);
    // At most 96 instances pick their lamps again per 8 ms (a new lamp list, as at dusk, re-picks every crown in view) and
    // keep their older pick meanwhile; an instance's first pick is never put off (no draw falls back to the old light)
    static DWORD s_budgetAt = 0;
    static int s_budget = 0;
    if (now - s_budgetAt >= 8) s_budgetAt = now, s_budget = 96;
    const float lawScale = TerrainLightingPolicy::ReachLampScale(NativeWallScale(), g_night.load(std::memory_order_relaxed), g_wallEnabled.load(std::memory_order_relaxed),
                                                                 g_wallGain.load(std::memory_order_relaxed), g_objPixelLampStrength.load(std::memory_order_relaxed) / kPixelLampTrimUnit);
    for (int i = 0; i < count; i++) {
        const float* inst = c[3 * i];
        const float scale = c[3 * i + 2][0];
        if (!std::isfinite(inst[0] + inst[1] + inst[2])) return false;
        // the instance: its place (10 cm), scale and the mesh (its lamps are its own, picked again with each lamp list)
        uint64_t key = 1469598103934665603ull;
        const auto mix = [&key](uint64_t v) { key = (key ^ v) * 1099511628211ull; };
        for (int k = 0; k < 3; k++) mix(static_cast<uint64_t>(std::llround((inst[k] + eye[k]) * 10.0f))); // 10 cm: a corrected eye is not exact to the cm
        mix(static_cast<uint64_t>(std::llround(scale * 1000.0f)));
        for (int k = 0; k < 6; k++) mix(static_cast<uint64_t>(std::llround((k < 3 ? mesh.lo[k] : mesh.hi[k - 3]) * 100.0f)));
        FoliageInstance& f = g_foliageInstances[key];
        if (!f.seen) {
            f.shaped = CrownShape(inst, scale, eye, mesh, f);
            if (f.shaped && DevSwitches::CanopyGroundShadow()) NoteCrown(f);
        }
        f.seen = now;
        if (!perVertex) continue;
        // the whole mesh box (trunk included: a lamp at the foot of a tree lights its trunk) and the crown's middle (or the
        // box's, without a crown shape), where the cones and the walls' share are taken
        const float hx = 0.5f * std::max(mesh.hi[0] - mesh.lo[0], mesh.hi[2] - mesh.lo[2]) * scale;
        const float boxC[3] = {inst[0] + eye[0], inst[1] + eye[1] + 0.5f * (mesh.lo[1] + mesh.hi[1]) * scale, inst[2] + eye[2]};
        const float ext[3] = {hx, 0.5f * (mesh.hi[1] - mesh.lo[1]) * scale, hx};
        // Picked at once the first time and when a lamp within reach of its box changed (a drag shows on the tree in the
        // same refresh: it waited for the hold, 5 to 9 s); a new list that changed nothing near it only moves its
        // generation. Picked again every 1.5 s within the budget when what a pick reads changed since: the walls and floors
        // (they arrive after a lot's first frames: a pick made before them kept a wrong wall share), the lamps' colours (the
        // ranking), or with indoor lamps shining out (their openings). A colour alone is taken at once below, with no pick.
        bool urgent = !f.picked;
        if (f.picked && f.pickGen != g_foliageLampGen) {
            if (FoliageLampsChangedNear(f.pickGen, boxC, ext)) urgent = true;
            else f.pickGen = g_foliageLampGen;
        }
        const uint64_t visState = LevelLightShare::LampVisibilityState();
        const bool stale = f.pickState != visState || f.pickColourGen != g_foliageColourGen || !g_exitLampRows.empty();
        if (urgent || (stale && now - f.pickTick > 1500 && s_budget > 0)) {
            {
                if (!urgent) s_budget--;
                FoliageVertexLamps::OwnLamp before[3];
                const int ownedBefore = f.owned;
                std::copy(f.own, f.own + 3, before);
                f.owned = PickFoliageLamps(f.shaped ? f.centre : boxC, boxC, ext, f.own);
                if (f.picked) {
                    bool changed = f.owned != ownedBefore;
                    for (int k = 0; k < f.owned && !changed; k++)
                        for (int a = 0; a < 3 && !changed; a++)
                            changed = std::fabs(f.own[k].pos[a] - before[k].pos[a]) > 1e-3f || std::fabs(f.own[k].colour[a] - before[k].colour[a]) > 1.0f / 512.0f;
                    (changed ? g_foliageRepickChanged : g_foliageRepickSame).fetch_add(1, std::memory_order_relaxed);
                }
                f.pickGen = g_foliageLampGen, f.pickTick = now, f.pickState = visState;
                f.pickColourGen = f.colourGen = g_foliageColourGen;
                if (!f.counted) {
                    f.counted = true;
                    g_foliageOwned[std::clamp(f.owned, 0, 3)].fetch_add(1, std::memory_order_relaxed);
                    if (!f.shaped) g_foliageNoShape.fetch_add(1, std::memory_order_relaxed);
                }
                f.picked = true;
            }
        }
        if (f.colourGen != g_foliageColourGen) { // the lamps' new colours (a fade, a recolour), each times its share
            f.colourGen = g_foliageColourGen;
            for (int k = 0; k < f.owned; k++)
                if (const auto it = g_foliageColour.find(f.own[k].key); it != g_foliageColour.end())
                    for (int a = 0; a < 3; a++) f.own[k].colour[a] = it->second[a] * f.own[k].share;
        }
        const float groundY = inst[1] + eye[1];
        for (int k = 0; k < 3; k++) {
            float* lp = &out[(3 * i + k) * 4];
            float* law = &out[(27 + 3 * i + k) * 4];
            float* rgb = &out[(82 + 3 * i + k) * 4];
            rgb[3] = c[54 + 3 * i + k][3]; // the game's w
            if (k >= f.owned) { // no lamp: colour 0, far away (adds nothing)
                lp[0] = inst[0], lp[1] = inst[1] + 1000.0f, lp[2] = inst[2], lp[3] = 1.0f;
                law[0] = 0.0f, law[1] = 0.0f, law[2] = 1e-4f, law[3] = 0.0f;
                out[(56 + 3 * i) * 4 + k] = 1e30f;
                rgb[0] = rgb[1] = rgb[2] = 0.0f;
                continue;
            }
            FoliageVertexLamps::SlotConstants(f.own[k], eye, groundY, lawScale, lp, law, out[(56 + 3 * i) * 4 + k]);
            for (int a = 0; a < 3; a++) rgb[a] = f.own[k].colour[a];
        }
        float* cr = &out[(54 + 3 * i) * 4];
        float* cr2 = &out[(55 + 3 * i) * 4];
        if (f.shaped) { // the crown's own leaves on the way, and the light through the leaves
            cr[0] = f.centre[0] - eye[0], cr[1] = f.centre[1] - eye[1], cr[2] = f.centre[2] - eye[2], cr[3] = 1.0f / f.rx;
            cr2[0] = 1.0f / f.ry, cr2[1] = Canopy::Extinction() * Canopy::LadFor(2.0f * f.ry) / std::log(2.0f);
            out[(56 + 3 * i) * 4 + 3] = FoliageVertexLamps::kTransmitOverReflect;
        } else {
            cr[0] = inst[0], cr[1] = inst[1], cr[2] = inst[2], cr[3] = 1.0f;
            cr2[0] = 1.0f, cr2[1] = 0.0f; // no extinction
            out[(56 + 3 * i) * 4 + 3] = 0.0f;
        }
    }
    if (perVertex) {
        float* g = &out[81 * 4];
        g[0] = FoliageVertexLamps::kTransmitOverReflect, g[1] = FoliageVertexLamps::kGroundHalfAlbedo, g[3] = 1.0f;
    }
    // instances not seen for a minute go (render thread, at most once a second)
    static DWORD s_swept = 0;
    if (now - s_swept > 1000) {
        s_swept = now;
        std::erase_if(g_foliageInstances, [now](const auto& p) { return now - p.second.seen > 60000; });
    }
    return true;
}

// The crowns over a lot's box for its ground shade, sorted (the same set gives the same key); crowns far from the camera
// go (render thread)
void CrownsIn(float x0, float z0, float x1, float z1, std::vector<Canopy::Ellipsoid>& out) {
    float eye[3];
    if (LotLightingMotion::CameraEye(eye)) {
        const size_t before = g_crowns.size();
        std::erase_if(g_crowns, [&](const CrownSeen& c) {
            const float dx = c.e.cx - eye[0], dz = c.e.cz - eye[2];
            return dx * dx + dz * dz > kCrownKeep * kCrownKeep;
        });
        if (g_crowns.size() != before) g_crownCount.store(static_cast<long>(g_crowns.size()), std::memory_order_relaxed);
    }
    for (const CrownSeen& c : g_crowns) {
        const Canopy::Ellipsoid& e = c.e;
        if (!(e.cx + e.rx < x0 || e.cx - e.rx > x1 || e.cz + e.rz < z0 || e.cz - e.rz > z1)) out.push_back(e);
    }
    std::sort(out.begin(), out.end(), [](const Canopy::Ellipsoid& a, const Canopy::Ellipsoid& b) {
        return std::tie(a.cx, a.cz, a.cy, a.rx, a.ry) < std::tie(b.cx, b.cz, b.cy, b.rx, b.ry);
    });
}

// Windows take outdoor light: the point a wall object picks its lamps at. A window's origin is a corner of its tile, on
// one side of the wall or the other as it is turned (F7 capture, 06/10: the same window turned took other lamps
// and another ground share, a slightly different tone); the tile's centre, origin + (local X - local Z) / 2, is on the
// wall line both ways. Used only when an outside wall runs along one of the object's axes through that centre; else the origin.
// true when an unscaled object turned about y has its centre on an outside wall's line (a door or a window set in its wall);
// c = that centre
bool WallLinePoint(const float (&m)[3][4], float (&c)[3]) {
    const float xx = m[0][0], xz = m[2][0], zx = m[0][2], zz = m[2][2];
    const float lx = std::sqrt(xx * xx + xz * xz), lz = std::sqrt(zx * zx + zz * zz);
    if (!(lx > 0.9f && lx < 1.1f && lz > 0.9f && lz < 1.1f) || std::fabs(m[1][0]) + std::fabs(m[1][2]) > 0.05f) return false; // turned about y only, unscaled
    c[0] = m[0][3] + 0.5f * (xx - zx);
    c[1] = m[1][3];
    c[2] = m[2][3] + 0.5f * (xz - zz);
    return LevelLightShare::OnWallLine(c, xx / lx, xz / lx, 0.15f) || LevelLightShare::OnWallLine(c, zx / lz, zz / lz, 0.15f);
}

// true when the point was moved onto an outside wall's line (the object is set in that wall: a window or a door)
bool ObjectLampPoint(const float (&m)[3][4], float (&p)[3]) {
    p[0] = m[0][3];
    p[1] = m[1][3];
    p[2] = m[2][3];
    if (!g_windowOutdoor.load(std::memory_order_relaxed)) return false;
    float c[3];
    if (!WallLinePoint(m, c)) return false;
    p[0] = c[0];
    p[2] = c[2];
    return true;
}

template <typename DrawFn> bool DrawObjectLamp(IDirect3DDevice9* dev, DrawFn draw) {
    // rig modes 2 (outdoors) and 1 (roofless fenced areas) both draw with the exterior technique (rig report 25/09)
    const int rigMode = RigMode();
    if (!g_objPixel.load(std::memory_order_relaxed) || (rigMode != 2 && rigMode != 1)) return false;
    // (12/10, one daytime curve; a red garden post by day: salmon columns and window frames) No full-daylight gate any more:
    // by day the game's rig lamps (and Apex's rig boost, ObjectLightBridge BoostRec) lit outdoor objects at full night
    // strength. The pass runs at every level: the per-pixel lamps replace the rig at strength x LampDay(night), and a draw
    // that places none keeps the rig scaled by LampDay(night) for that draw (below).
    const float night = g_night.load(std::memory_order_relaxed);
    float c[4];
    IDirect3DTexture9* atlas = LightmapSmooth::Atlas(c);
    if (!atlas) return false;
    PatchedPs* chosen = &PatchedFor(dev, g_objLampPs, "Outdoor object (ground light)", [](std::vector<DWORD>& t, PatchedPs& pp) { return ShaderPatches::PatchObjectLampPs(t, pp.obj); });
    if (!chosen->ps) return false;
    const bool pixelLamps = g_objPixelLamps.load(std::memory_order_relaxed);
    // realistic reach (12/10): the copy with the outdoor walls' law, made only from a shader the first law takes; refused
    // (no room for its 2 rows), the first law stays (g_reachLawRefused)
    bool reachRefused = false;
    if (pixelLamps && LevelLightShare::RealisticReachActive()) {
        PatchedPs& r = PatchedFor(
            dev, g_objLampPsReach, "Outdoor object (realistic reach)", [](std::vector<DWORD>& t, PatchedPs& pp) { return ShaderPatches::PatchObjectLampPs(t, pp.obj, true); }, true);
        if (r.ps) chosen = &r;
        else reachRefused = true, g_reachLawRefused++;
    }
    PatchedPs& p = *chosen;
    const bool reach = p.obj.reachConst != 0;
    IDirect3DVertexShader9* vs = ObjectVsFor(dev, g_curVs);
    if (!vs) return false;
    float oldA[4] = {}, oldB[4] = {};
    dev->GetPixelShaderConstantF(p.obj.atlasConst, oldA, 1);
    dev->GetPixelShaderConstantF(p.obj.strengthConst, oldB, 1);
    float s[4] = {TerrainLightingPolicy::SurfaceLampGain(g_night.load(std::memory_order_relaxed),
                       g_objPixelStrength.load(std::memory_order_relaxed)), 0, 0, 0};
    float groundShare = 1.0f; // the share of the nearby lamps' light no wall blocks (below)
    float lampTop = -1e9f, originY = 0.0f; // the highest lamp head around and the object's origin (the ground gate, below)
    // Per-pixel lamps ("Counters" request): the same world lamps for every piece, chosen by the object's position (the
    // VS world triple's translation), so neighbouring pieces of a modular object get the same lamps.
    constexpr unsigned N = ShaderPatches::kObjectPixelLamps;
    // rows from cS: the parameters, N x (pos, 1/R^2) (colour, spill), N x the two cones, then with reach the 2 rows of r0^2
    // (ShaderPatches::ObjectLampPatch::reachConst = coneConst + 2N): one upload
    constexpr unsigned kLampRows = 1 + 4 * N;
    const unsigned rows = kLampRows + (reach ? 2u : 0u);
    float lamps[kLampRows + 2][4] = {};
    float oldLamps[kLampRows + 2][4] = {};
    bool zeroRig = false;
    float oldRig[3][4] = {}, oldVl[4][4] = {};
    const float noRig[4][4] = {};
    const int vl = g_curVsInfo->patched.vertexLight, wk = g_curVsInfo->patched.worldK; // ObjectVsFor succeeded: an object entry
    int nLamps = 0;
    lamps[0][3] = 1e-4f;
    for (unsigned k = 0; k < N; k++) lamps[1 + 2 * k][0] = lamps[1 + 2 * k][2] = 1e6f; // unused slot: far away, colour 0
    for (unsigned k = 0; k < N; ++k) {
        lamps[2 + 2 * k][3] = 1.0f;
        lamps[1 + 2 * N + 2 * k][3] = 1.0f;
        lamps[2 + 2 * N + 2 * k][3] = 1.0f;
    }
    if (pixelLamps) {
        float m[3][4];
        if (wk >= 0 && SUCCEEDED(dev->GetVertexShaderConstantF(static_cast<UINT>(wk), &m[0][0], 3))) {
            // Windows take outdoor light: a window or door sits in its wall with its origin up to half a tile from the wall
            // line (turned the other way, inside the house), so its own wall must not block every lamp in front of it
            // (F7 capture, 06/10: the turned window's frame got 0 of 19 lamps and no ground light). Off: the 0.2 m as before.
            // only an object set in its wall (ObjectLampPoint moved it onto the wall line) passes a wall within kWindowWallSkip;
            // loose objects keep 0.2 m, so a wall 0.4 m behind an armchair still blocks the next room's lamp (F7 capture, 07/10)
            float at[3];
            const bool inWall = ObjectLampPoint(m, at);
            const float nearSkip = inWall ? kWindowWallSkip : 0.2f;
            nLamps = PickObjectLamps(at[0], at[1], at[2], lamps, groundShare, nearSkip, lamps + 1 + 2 * N, &lampTop, reach ? lamps + kLampRows : nullptr, true);
            originY = at[1];
            // the rig goes: its 3 pixel lamps (PS c5..c7 = 0 below, diffuse and specular) and its 4 vertex lights (the VS
            // colour constants = 0; Phong's ambient term in COLOR0 stays)
            lamps[0][1] = TerrainLightingPolicy::SurfaceLampGain(g_night.load(std::memory_order_relaxed),
                              g_objPixelLampStrength.load(std::memory_order_relaxed));
            // reach: the outdoor walls' scale, the slider a trim over its default (x1 at the default: a column takes the
            // light of the wall behind it)
            if (reach)
                lamps[0][1] = TerrainLightingPolicy::ReachLampScale(NativeWallScale(), night, g_wallEnabled.load(std::memory_order_relaxed), g_wallGain.load(std::memory_order_relaxed),
                                                                    g_objPixelLampStrength.load(std::memory_order_relaxed) / kPixelLampTrimUnit);
            zeroRig = true;
            lamps[0][0] = 1.0f; // no directionless ground light on vertical faces
        }
    }
    // (12/10, F7 18-38-41: a window frame on story 2 stands 2.5 m above the only lamp head around, a red post's; how much of
    // its red came through this term rather than the per-pixel lamps is UNVERIFIED) The ground light (the atlas) is the
    // light lying on the ground, at no height: an object standing above every lamp head around takes none of it, fading over
    // the first 1.5 m above the highest head (lamp heads, not lanterns: game data), the rule fences follow (DrawInstanced).
    // Objects on the ground, below the heads, keep it all; with no lamp near it stays. Only with the per-pixel lamps (they
    // find lampTop).
    const float gate = lampTop > -1e8f ? std::clamp(1.0f - (originY - lampTop) / 1.5f, 0.0f, 1.0f) : 1.0f;
    if (gate < 1.0f) g_objGroundGated++;
    s[0] *= groundShare * gate;
    const float rigDayScale = TerrainLightingPolicy::LampDay(night);
    const bool rigDay = !zeroRig && rigDayScale < 1.0f && (p.obj.rigLamps || vl >= 0); // the kept rig, scaled for this draw
    if (rigDay) g_objRigDayScaled++;
    if (LightProbe::Capturing()) {
        g_objDrawInfo = DescribeObjectDraw(dev, p.obj, rigMode, zeroRig, wk, vl, lamps, nLamps);
        if (rigDay) g_objDrawInfo += std::format("\n      no per-pixel lamps placed: the game's rig lamp colours x {:.4f} (daytime curve) for this draw", rigDayScale);
        g_objDrawInfo += std::format("\n      ground light: share not behind buildings {:.2f}, height gate {:.2f} (object origin y {:.2f}, highest lamp head within 15 m {})",
                                     groundShare, gate, originY, lampTop > -1e8f ? std::format("{:.2f}", lampTop) : std::string("none"));
        g_objDrawInfo += ReachLawText(reach, reachRefused, zeroRig, lamps[0][1], lamps + kLampRows, nLamps);
    }
    if (reach && zeroRig) g_reachObjDraws++; // zeroRig: the lamps were placed (else cS.x = 0 and the sum is not used)
    dev->GetPixelShaderConstantF(p.obj.lampParamConst, &oldLamps[0][0], rows);
    IDirect3DPixelShader9* originalPs = g_curPs;
    IDirect3DVertexShader9* originalVs = g_curVs;
    g_inOwnCall = true;
    {
        SamplerBind bind(dev, p.obj.atlasSampler, atlas, D3DTEXF_NONE);
        SetPsConst(dev, p.obj.atlasConst, c, 1);
        SetPsConst(dev, p.obj.strengthConst, s, 1);
        SetPsConst(dev, p.obj.lampParamConst, &lamps[0][0], rows);
        if (zeroRig && p.obj.rigLamps) {
            dev->GetPixelShaderConstantF(5, &oldRig[0][0], 3);
            SetPsConst(dev, 5, &noRig[0][0], 3);
        }
        if (zeroRig && vl >= 0) {
            dev->GetVertexShaderConstantF(static_cast<UINT>(vl), &oldVl[0][0], 4);
            SetVsConst(dev, static_cast<UINT>(vl), &noRig[0][0], 4);
        }
        // No per-pixel lamps placed (option off, or no world triple): the game's rig stays, its lamp colours (the same PS
        // c5..c7 and VS vertex light colours zeroed above; rgb only) x LampDay(night) for this draw (12/10). The sun is not
        // in these registers (DescribeObjectDraw). Night: x 1, untouched.
        if (rigDay) {
            if (p.obj.rigLamps) {
                dev->GetPixelShaderConstantF(5, &oldRig[0][0], 3);
                float scaled[3][4];
                std::memcpy(scaled, oldRig, sizeof scaled);
                for (auto& r : scaled) r[0] *= rigDayScale, r[1] *= rigDayScale, r[2] *= rigDayScale;
                SetPsConst(dev, 5, &scaled[0][0], 3);
            }
            if (vl >= 0) {
                dev->GetVertexShaderConstantF(static_cast<UINT>(vl), &oldVl[0][0], 4);
                float scaled[4][4];
                std::memcpy(scaled, oldVl, sizeof scaled);
                for (auto& r : scaled) r[0] *= rigDayScale, r[1] *= rigDayScale, r[2] *= rigDayScale;
                SetVsConst(dev, static_cast<UINT>(vl), &scaled[0][0], 4);
            }
        }
        SetVs(dev, vs);
        SetPs(dev, p.ps);
        draw();
        SetPs(dev, originalPs);
        SetVs(dev, originalVs);
        if ((zeroRig || rigDay) && vl >= 0) SetVsConst(dev, static_cast<UINT>(vl), &oldVl[0][0], 4);
        if ((zeroRig || rigDay) && p.obj.rigLamps) SetPsConst(dev, 5, &oldRig[0][0], 3);
        SetPsConst(dev, p.obj.lampParamConst, &oldLamps[0][0], rows);
        SetPsConst(dev, p.obj.strengthConst, oldB, 1);
        SetPsConst(dev, p.obj.atlasConst, oldA, 1);
    }
    g_objDrawInfo.clear();
    g_inOwnCall = false;
    g_objLampDrawn.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// ---- Windows take outdoor light (experimental, "janelasLuzDeFora", off by default). F7 capture (06/10):
// a window's origin is the centre of the tile in front of it, half a metre from its wall; turned the other way, the origin
// is inside the room, and the window's sash and glass part gets a room-mode rig (RigTracker mode 0) and is drawn by a
// basis-reading object shader (VS class 10 with a basis PS: VS 1E93EFA8 / PS 8B83B1A0 in that capture; Apex drew it as
// the game does, with the Rooms at Night furniture constants, "mod draw (another fix)"). Its faces turned out of the house
// read the room's directional maps towards the outside, which hold no light: pure black at night, while the inner side
// looked lit, so the window looked inverted. ShaderPatches::PatchWindowOutdoor keeps the game's light and lets the faces
// that look out of the house plan (the basis maps' alpha read half a tile out along the normal) take the outdoor terms of
// the outdoor-object patch: the ground atlas and the per-pixel lamps around the window, at the same strengths, never
// darker than the game. By day those outdoor terms take the daytime curve (TerrainLightingPolicy::LampDay). The frame of the same window (an outdoor rig, mode 2)
// lost every lamp to its own wall; DrawObjectLamp passes a wall within kWindowWallSkip while the option is on. ----
std::unordered_map<IDirect3DPixelShader9*, PatchedPs> g_windowPs, g_windowPsReach;
std::atomic<int> g_windowDrawn{0}, g_windowRefused{0};

template <typename DrawFn> bool DrawWindowOutdoor(IDirect3DDevice9* dev, DrawFn draw) {
    if (!g_windowOutdoor.load(std::memory_order_relaxed) || !g_objPixel.load(std::memory_order_relaxed) || !g_curVsInfo || RigMode() != 0) return false;
    // (12/10, one daytime curve) runs at every level; its outdoor terms take x LampDay(night)
    const float night = g_night.load(std::memory_order_relaxed);
    float atlasMap[4];
    IDirect3DTexture9* atlas = LightmapSmooth::Atlas(atlasMap);
    if (!atlas) return false;
    PatchedPs* chosen = &PatchedFor(dev, g_windowPs, "Window (outdoor light)", [](std::vector<DWORD>& t, PatchedPs& pp) {
        // Rooms at Night's blue tint on the cube, as the game's draw of this part gets it (OnDrawFurniture), when the shader has it
        if (!ShaderPatches::PatchCubeTint(t, pp.cubeTint)) pp.cubeTint = -1;
        return ShaderPatches::PatchWindowOutdoor(t, pp.window);
    });
    if (!chosen->ps) return false;
    // realistic reach (12/10): the copy with the outdoor walls' law, made only from a shader the first law takes; refused
    // (no room for its 2 rows), the first law stays (g_reachLawRefused)
    bool reachRefused = false;
    if (g_objPixelLamps.load(std::memory_order_relaxed) && LevelLightShare::RealisticReachActive()) {
        PatchedPs& r = PatchedFor(
            dev, g_windowPsReach, "Window (outdoor light, realistic reach)",
            [](std::vector<DWORD>& t, PatchedPs& pp) {
                if (!ShaderPatches::PatchCubeTint(t, pp.cubeTint)) pp.cubeTint = -1;
                return ShaderPatches::PatchWindowOutdoor(t, pp.window, true);
            },
            true);
        if (r.ps) chosen = &r;
        else reachRefused = true, g_reachLawRefused++;
    }
    PatchedPs& p = *chosen;
    const bool reach = p.window.reachConst != 0;
    IDirect3DVertexShader9* vs = ObjectVsFor(dev, g_curVs);
    if (!vs) return false;
    VsInfo& vi = *g_curVsInfo;
    const int wk = vi.patched.worldK;
    // how the vertex shader makes the basis uv from the world position (rows x scale), looked up once per semantic
    const int sem = (p.window.uvUsage << 4) | p.window.uvIndex;
    if (vi.winSem != sem) {
        vi.winSem = sem;
        UINT size = 0;
        std::vector<DWORD> code;
        if (SUCCEEDED(g_curVs->GetFunction(nullptr, &size)) && size >= 8 && size <= 65536 && size % 4 == 0) {
            code.resize(size / 4);
            if (FAILED(g_curVs->GetFunction(code.data(), &size))) code.clear();
        }
        vi.winRowsOk = ShaderPatches::WorldUvRows(code, p.window.uvUsage, p.window.uvIndex, wk, vi.winRows);
        LOG_INFO(std::format("[LotLightBridge] Window (outdoor light): vertex shader {:08X} basis uv rows {}", reinterpret_cast<uintptr_t>(g_curVs),
                             vi.winRowsOk ? std::format("c{} / c{}", vi.winRows.row[0], vi.winRows.row[1]) : std::string("not found, the game draws it")));
    }
    if (!vi.winRowsOk) {
        g_windowRefused.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    // plan rows: d x (du/dx, du/dz) and d x (dv/dx, dv/dz) of the basis uv (the game's: 1/64 per metre along the lot axes)
    float plan[2][4] = {};
    for (int k = 0; k < 2; k++) {
        float row[4] = {}, sc[4] = {1, 1, 1, 1};
        if (FAILED(dev->GetVertexShaderConstantF(static_cast<UINT>(vi.winRows.row[k]), row, 1))) return false;
        if (vi.winRows.scale[k] >= 0 && FAILED(dev->GetVertexShaderConstantF(static_cast<UINT>(vi.winRows.scale[k]), sc, 1))) return false;
        const float s = vi.winRows.scale[k] >= 0 ? sc[vi.winRows.scaleComp[k] & 3] : 1.0f;
        const float ux = row[0] * s, uz = row[2] * s, len = std::sqrt(ux * ux + uz * uz);
        if (!std::isfinite(len) || len < 1.0f / 4096.0f || len > 1.0f) return false; // not a map over metres
        plan[k][0] = ux * kWindowPlanReach;
        plan[k][1] = uz * kWindowPlanReach;
    }
    // the same strengths and lamps as an outdoor object at this position (DrawObjectLamp)
    constexpr unsigned N = ShaderPatches::kObjectPixelLamps;
    float lamps[1 + 2 * N][4] = {};
    float reachRows[2][4] = {}, oldReach[2][4] = {}; // reach: r0^2 of the 8 lamps (WindowOutdoorPatch::reachConst = planConst + 2)
    lamps[0][3] = 1e-4f;
    for (unsigned k = 0; k < N; k++) lamps[1 + 2 * k][0] = lamps[1 + 2 * k][2] = 1e6f; // unused slot: far away, colour 0
    float groundShare = 1.0f, gate = 1.0f;
    int nLamps = 0;
    bool placed = false; // the lamps were placed (else the lamp strength stays 0)
    float m[3][4] = {};
    if (g_objPixelLamps.load(std::memory_order_relaxed) && wk >= 0 && SUCCEEDED(dev->GetVertexShaderConstantF(static_cast<UINT>(wk), &m[0][0], 3))) {
        placed = true;
        float at[3], lampTop = -1e9f;
        ObjectLampPoint(m, at);
        nLamps = PickObjectLamps(at[0], at[1], at[2], lamps, groundShare, kWindowWallSkip, nullptr, &lampTop, reach ? reachRows : nullptr);
        lamps[0][1] = TerrainLightingPolicy::SurfaceLampGain(night, g_objPixelLampStrength.load(std::memory_order_relaxed));
        // reach: the outdoor walls' scale, the slider a trim over its default (as DrawObjectLamp)
        if (reach)
            lamps[0][1] = TerrainLightingPolicy::ReachLampScale(NativeWallScale(), night, g_wallEnabled.load(std::memory_order_relaxed), g_wallGain.load(std::memory_order_relaxed),
                                                                g_objPixelLampStrength.load(std::memory_order_relaxed) / kPixelLampTrimUnit);
        // the ground gate of DrawObjectLamp (12/10): the sash's ground term (atlas x (0.5 + 0.5 N.y) on every face,
        // PatchWindowOutdoor) takes no height into account, so a window above every lamp head around takes none of the pool
        if (lampTop > -1e8f) gate = std::clamp(1.0f - (at[1] - lampTop) / 1.5f, 0.0f, 1.0f);
        if (gate < 1.0f) g_objGroundGated++;
    }
    const float strength[4] = {TerrainLightingPolicy::SurfaceLampGain(night, g_objPixelStrength.load(std::memory_order_relaxed)) * groundShare * gate, 0, 0, 0};
    float tint[4] = {1, 1, 1, 1};
    if (p.cubeTint >= 0 && UnlitRooms::FurnitureActive()) {
        float cube[3];
        UnlitRooms::FurnitureCubeColour(cube);
        tint[0] = UnlitRooms::FurnitureTint();
        tint[1] = cube[0];
        tint[2] = cube[1];
        tint[3] = cube[2];
    }
    if (LightProbe::Capturing()) {
        g_objDrawInfo = std::format("WINDOW lit outdoors by the mod (experimental) | rig mode 0 (room) | per-pixel lamps: {} used of {} | ground light strength {:.2f} "
                                    "(share not behind buildings {:.2f}, height gate {:.2f}) | lamp strength {:.2f} | object position ({:.2f} {:.2f} {:.2f}) | house plan read {:.2f} m out along the normal",
                                    nLamps, g_lastLampCandidates, strength[0], groundShare, gate, lamps[0][1], m[0][3], m[1][3], m[2][3], kWindowPlanReach);
        g_objDrawInfo += ReachLawText(reach, reachRefused, placed, lamps[0][1], reachRows, nLamps);
    }
    if (reach && placed) g_reachWindowDraws++;
    float oldA[4] = {}, oldB[4] = {}, oldPlan[2][4] = {}, oldTint[4] = {};
    float oldLamps[1 + 2 * N][4] = {};
    const ShaderPatches::WindowOutdoorPatch& w = p.window;
    dev->GetPixelShaderConstantF(w.atlasConst, oldA, 1);
    dev->GetPixelShaderConstantF(w.strengthConst, oldB, 1);
    dev->GetPixelShaderConstantF(w.lampParamConst, &oldLamps[0][0], 1 + 2 * N);
    dev->GetPixelShaderConstantF(w.planConst, &oldPlan[0][0], 2);
    if (reach) dev->GetPixelShaderConstantF(w.reachConst, &oldReach[0][0], 2);
    if (p.cubeTint >= 0) dev->GetPixelShaderConstantF(static_cast<UINT>(p.cubeTint), oldTint, 1);
    IDirect3DPixelShader9* originalPs = g_curPs;
    IDirect3DVertexShader9* originalVs = g_curVs;
    g_inOwnCall = true;
    {
        SamplerBind bind(dev, w.atlasSampler, atlas, D3DTEXF_NONE);
        SetPsConst(dev, w.atlasConst, atlasMap, 1);
        SetPsConst(dev, w.strengthConst, strength, 1);
        SetPsConst(dev, w.lampParamConst, &lamps[0][0], 1 + 2 * N);
        SetPsConst(dev, w.planConst, &plan[0][0], 2);
        if (reach) SetPsConst(dev, w.reachConst, &reachRows[0][0], 2);
        if (p.cubeTint >= 0) SetPsConst(dev, static_cast<UINT>(p.cubeTint), tint, 1);
        SetVs(dev, vs);
        SetPs(dev, p.ps);
        draw();
        SetPs(dev, originalPs);
        SetVs(dev, originalVs);
        if (p.cubeTint >= 0) SetPsConst(dev, static_cast<UINT>(p.cubeTint), oldTint, 1);
        if (reach) SetPsConst(dev, w.reachConst, &oldReach[0][0], 2);
        SetPsConst(dev, w.planConst, &oldPlan[0][0], 2);
        SetPsConst(dev, w.lampParamConst, &oldLamps[0][0], 1 + 2 * N);
        SetPsConst(dev, w.strengthConst, oldB, 1);
        SetPsConst(dev, w.atlasConst, oldA, 1);
    }
    g_objDrawInfo.clear();
    g_inOwnCall = false;
    g_windowDrawn.fetch_add(1, std::memory_order_relaxed);
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

// ---- Smooth light maps (2.12.4, docs/features/night-lighting/room-map-smoothing.md): the room light map read of the
// room-lit shaders (InteriorFloor, InteriorWall and their siblings; g_roomMapPs) as a cubic B-spline
// (ShaderPatches::PatchRoomMapSmooth). The vertex shader tells walls from floors by its uv scale (RoomMapUvScale: 1/4096 the
// wall atlas, 1/32767 the floor maps); a wall draw keeps its taps in its atlas block of 3n + 1 rows, n = atlas width / 256
// (the atlas is 256n x 128n at every class and Light detail, light_detail.cpp), and only while the room solves saw no block
// off that grid (LevelLightShare::WallBandsAligned). A ps_2_0 shader's copy is ps_2_x: drawn only when the device's PS20Caps
// take it (RoomMapCapsOk), else the game's shader draws. Off, unknown or refused: the game's draw, untouched.
std::atomic<bool> g_roomMapSmooth{true};
struct RoomMapSizeMemo {
    IDirect3DBaseTexture9* tex = nullptr;
    uint32_t frame = 0;
    float size[4] = {};
};
RoomMapSizeMemo g_roomMapSizes[64]; // render thread; valid for g_roomMapFrame only (OnPresent counts frames)
uint32_t g_roomMapFrame = 1;
std::unordered_map<IDirect3DPixelShader9*, PatchedPs> g_roomMapSmoothPs, g_basisRoomPs;
// (arch_ambient) the same copies with the arch block, bound only for a floor draw of a room with arches (DrawWithRoomMap)
std::unordered_map<IDirect3DPixelShader9*, PatchedPs> g_roomMapSmoothArchPs, g_basisRoomArchPs, g_basisRoomWallArchPs;
std::unordered_map<IDirect3DVertexShader9*, ShaderPatches::RoomMapUv> g_roomUvVs; // (pinned by ClassifyVs)
std::atomic<long> g_roomWallDrawn{0}, g_roomFloorDrawn{0}, g_roomCapsRefused{0}, g_roomUnknownVs{0}, g_roomBandsOff{0}, g_roomPointFilter{0};
IDirect3DDevice9* g_capsDev = nullptr;
D3DCAPS9 g_caps{};
bool RoomMapCapsOk(IDirect3DDevice9* dev, const ShaderPatches::RoomMapPatch& p) {
    if (dev != g_capsDev) {
        g_capsDev = dev;
        if (FAILED(dev->GetDeviceCaps(&g_caps))) g_caps = D3DCAPS9{};
    }
    if (!p.ps2x) return g_caps.PixelShaderVersion >= D3DPS_VERSION(3, 0);
    return g_caps.PixelShaderVersion >= D3DPS_VERSION(2, 0) && (g_caps.PS20Caps.Caps & D3DPS20CAPS_ARBITRARYSWIZZLE) && g_caps.PS20Caps.NumTemps >= p.temps &&
           g_caps.PS20Caps.NumInstructionSlots >= p.slots;
}
// The draw's room map constants: size (w, h, 1/w, 1/h) and band (1/S, S, 0.5, S - 1); false when this draw keeps the game's read
bool RoomMapConsts(IDirect3DDevice9* dev, const ShaderPatches::RoomMapPatch& p, float size[4], float band[4], bool& wall) {
    if (!g_curVs) return false;
    auto it = g_roomUvVs.find(g_curVs);
    if (it == g_roomUvVs.end()) {
        ShaderPatches::RoomMapUv uv;
        UINT n = 0;
        std::vector<DWORD> code;
        if (SUCCEEDED(g_curVs->GetFunction(nullptr, &n)) && n >= 8 && n <= 65536) {
            code.resize(n / 4);
            if (FAILED(g_curVs->GetFunction(code.data(), &n))) code.clear();
        }
        if (code.empty() || !ShaderPatches::RoomMapUvScale(code, uv)) uv.constReg = -1;
        it = g_roomUvVs.emplace(g_curVs, uv).first;
    }
    const ShaderPatches::RoomMapUv& uv = it->second;
    if (uv.constReg < 0) {
        g_roomUnknownVs.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    float scale = uv.value;
    if (!uv.defined) {
        float c[4] = {};
        if (FAILED(dev->GetVertexShaderConstantF(static_cast<UINT>(uv.constReg), c, 1))) return false;
        scale = c[uv.comp & 3];
    }
    wall = std::fabs(scale * 4096.0f - 1.0f) < 1e-3f;
    const bool floor = std::fabs(scale * 32767.0f - 1.0f) < 1e-3f;
    if (!wall && !floor) {
        g_roomUnknownVs.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    DWORD mag = 0;
    if (FAILED(dev->GetSamplerState(p.sampler, D3DSAMP_MAGFILTER, &mag)) || mag != D3DTEXF_LINEAR) { // the taps rely on the bilinear filter
        g_roomPointFilter.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    IDirect3DBaseTexture9* map = nullptr;
    bool haveSize = false;
    if (SUCCEEDED(dev->GetTexture(p.sampler, &map)) && map) {
        // (one level description per room map and frame: a story's maps are bound for many draws; kept for this frame only,
        // so a map freed and made again at the same address is read again next frame)
        RoomMapSizeMemo& m = g_roomMapSizes[(reinterpret_cast<uintptr_t>(map) >> 4) & (std::size(g_roomMapSizes) - 1)];
        if (m.tex == map && m.frame == g_roomMapFrame) {
            std::memcpy(size, m.size, sizeof m.size);
            haveSize = true;
        } else if (MapSize(map, size)) {
            m.tex = map, m.frame = g_roomMapFrame;
            std::memcpy(m.size, size, sizeof m.size);
            haveSize = true;
        }
    }
    if (map) map->Release();
    if (!haveSize) return false;
    float S = 65536.0f; // floors: one block
    if (wall) {
        const int w = static_cast<int>(size[0]), h = static_cast<int>(size[1]), n = w / 256;
        if (n < 1 || n > 8 || (n & (n - 1)) || w != 256 * n || h != 128 * n) return false; // not the wall atlas the game makes
        if (!LevelLightShare::WallBandsAligned()) {
            g_roomBandsOff.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        S = static_cast<float>(3 * n + 1);
    }
    band[0] = 1.0f / S, band[1] = S, band[2] = 0.5f, band[3] = S - 1.0f;
    return true;
}
// (development builds, arch_ambient) The arch rows of a draw for the copy with the arch block (room.archConst): the room's
// arches and its neighbours' ambient, on floor maps only (one block: S = 65536). Returns how many cells hold something; 0 for
// the wall atlas and a room with no arch, whose zero rows change nothing (the copy without the block then draws, pixel for
// pixel the same, without the block's ~106 ALU per pixel). basisTex: one of the draw's directional maps, 0 = none bound.
constexpr UINT kArchRows = 2 * ShaderPatches::kArchCells;
std::atomic<long> g_archCopyDraws{0}, g_archPlainDraws{0};
int ArchRowsFor(IDirect3DDevice9* dev, const ShaderPatches::RoomMapPatch& room, const float band[4], uintptr_t basisTex, float* rows) {
    if (room.archConst < 0 || room.ambConst < 0 || !(band[1] > 60000.0f)) return 0;
    float own[4] = {};
    if (FAILED(dev->GetPixelShaderConstantF(static_cast<UINT>(room.ambConst), own, 1))) return 0;
    IDirect3DBaseTexture9* map = nullptr; // the room light map: its story tells rooms with the same ambient apart
    dev->GetTexture(room.sampler, &map);
    const uintptr_t mapTex = reinterpret_cast<uintptr_t>(map);
    if (map) map->Release();
    return LevelLightShare::ArchAmbientConsts(own, mapTex, basisTex, rows, static_cast<int>(ShaderPatches::kArchCells));
}
// (arch_ambient) The arch copy of a room-map copy just made: none where it has no arch block (ps_2_x, no ambient constant),
// since it would draw exactly as the copy without it
PatchedPs* ArchCopyOrNull(PatchedPs& a) {
    if (a.ps && a.room.archConst < 0) {
        a.ps->Release();
        a.ps = nullptr;
    }
    return a.ps ? &a : nullptr;
}
// rows: the arch rows (kArchRows) for a copy with the arch block, nullptr for a copy without it
template <typename DrawFn> void DrawWithRoomMap(IDirect3DDevice9* dev, IDirect3DPixelShader9* copy, const ShaderPatches::RoomMapPatch& p, const float size[4],
                                                const float band[4], const float* rows, DrawFn draw) {
    float oldSize[4] = {}, oldBand[4] = {}, oldArch[kArchRows * 4] = {};
    dev->GetPixelShaderConstantF(p.sizeConst, oldSize, 1);
    dev->GetPixelShaderConstantF(p.bandConst, oldBand, 1);
    const bool arch = rows && p.archConst >= 0;
    if (arch) dev->GetPixelShaderConstantF(static_cast<UINT>(p.archConst), oldArch, kArchRows);
    IDirect3DPixelShader9* original = g_curPs;
    g_inOwnCall = true;
    SetPsConst(dev, p.sizeConst, size, 1);
    SetPsConst(dev, p.bandConst, band, 1);
    if (arch) SetPsConst(dev, static_cast<UINT>(p.archConst), rows, kArchRows);
    SetPs(dev, copy);
    draw();
    SetPs(dev, original);
    if (arch) SetPsConst(dev, static_cast<UINT>(p.archConst), oldArch, kArchRows);
    SetPsConst(dev, p.bandConst, oldBand, 1);
    SetPsConst(dev, p.sizeConst, oldSize, 1);
    g_inOwnCall = false;
    (arch ? g_archCopyDraws : g_archPlainDraws).fetch_add(1, std::memory_order_relaxed);
}
template <typename DrawFn> bool DrawRoomMapSmooth(IDirect3DDevice9* dev, DrawFn draw) {
    if (!g_roomMapSmooth.load(std::memory_order_relaxed)) return false;
    PatchedPs& p = PatchedFor(dev, g_roomMapSmoothPs, "Smooth light maps", [](std::vector<DWORD>& t, PatchedPs& pp) { return ShaderPatches::PatchRoomMapSmooth(t, pp.room, false); });
    PatchedPs* a = DevSwitches::ArchAmbient() ? ArchCopyOrNull(PatchedFor(dev, g_roomMapSmoothArchPs, "Smooth light maps (arches)", [](std::vector<DWORD>& t, PatchedPs& pp) { return ShaderPatches::PatchRoomMapSmooth(t, pp.room, true); }, true))
                                              : nullptr;
    const bool plainOk = p.ps && RoomMapCapsOk(dev, p.room);
    if (a && !RoomMapCapsOk(dev, a->room)) a = nullptr;
    if (!plainOk && !a) {
        if (p.ps) g_roomCapsRefused.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    float size[4], band[4];
    bool wall = false;
    if (!RoomMapConsts(dev, plainOk ? p.room : a->room, size, band, wall)) return false;
    float rows[kArchRows * 4] = {};
    const int cells = a ? ArchRowsFor(dev, a->room, band, 0, rows) : 0;
    if (a && (cells > 0 || !plainOk)) DrawWithRoomMap(dev, a->ps, a->room, size, band, rows, draw);
    else DrawWithRoomMap(dev, p.ps, p.room, size, band, nullptr, draw);
    (wall ? g_roomWallDrawn : g_roomFloorDrawn).fetch_add(1, std::memory_order_relaxed);
    return true;
}

// ---- Walls in the directional maps (development builds, basis_wall_filter) ----
// The draw's directional maps name their story (LevelLightShare::WallMaskFor); its wall mask is kept here as a 64 x 64
// texture (managed pool), uploaded again when the story's walls change, and bound at the patch's mask sampler (point, clamp)
// for the wall-aware copy of the shader (ShaderPatches::PatchBasisSmooth wallMask). No mask known: the plain copy draws.
std::unordered_map<IDirect3DPixelShader9*, PatchedPs> g_basisSmoothWallPs, g_basisRoomWallPs, g_basisSmoothFillPs;
struct WallMaskTex {
    IDirect3DTexture9* tex = nullptr;
    uint32_t gen = 0;
};
std::unordered_map<uintptr_t, WallMaskTex> g_wallMaskTex; // by the directional map's D3D texture
IDirect3DDevice9* g_wallMaskDev = nullptr;
std::atomic<long> g_wallMaskDraws{0};
// (development builds, stair_room_fill) the story's ambient per tile (LevelLightShare::AmbientMapFor), 64 x 64 A32B32G32R32F in the
// managed pool by the directional map's texture, written again when the rooms' ambients change; point-sampled
struct AmbMapTex {
    IDirect3DTexture9* tex = nullptr;
    uint64_t sig = 0;
};
std::unordered_map<uintptr_t, AmbMapTex> g_ambMapTex;
std::atomic<long> g_ambFillDraws{0};
void ReleaseWallMasks() {
    for (auto& [k, m] : g_wallMaskTex)
        if (m.tex) m.tex->Release();
    g_wallMaskTex.clear();
    for (auto& [k, m] : g_ambMapTex)
        if (m.tex) m.tex->Release();
    g_ambMapTex.clear();
}
IDirect3DTexture9* WallMaskTexture(IDirect3DDevice9* dev, DWORD basisSampler) {
    if (!DevSwitches::BasisWallFilter()) return nullptr;
    IDirect3DBaseTexture9* b = nullptr;
    if (FAILED(dev->GetTexture(basisSampler, &b)) || !b) return nullptr;
    const uintptr_t key = reinterpret_cast<uintptr_t>(b);
    b->Release();
    if (dev != g_wallMaskDev) {
        ReleaseWallMasks();
        g_wallMaskDev = dev;
    }
    if (g_wallMaskTex.size() > 256) ReleaseWallMasks();
    WallMaskTex& m = g_wallMaskTex[key];
    static uint8_t px[64 * 64 * 4];
    uint32_t gen = 0;
    if (!LevelLightShare::WallMaskFor(key, m.tex ? m.gen : 0, gen, px)) return nullptr;
    if (!m.tex) {
        if (FAILED(dev->CreateTexture(64, 64, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &m.tex, nullptr)) || !m.tex) return nullptr;
        m.gen = 0;
    }
    if (gen != m.gen) {
        D3DLOCKED_RECT lock{};
        if (FAILED(m.tex->LockRect(0, &lock, nullptr, 0)) || !lock.pBits) return nullptr;
        for (int y = 0; y < 64; y++) std::memcpy(static_cast<BYTE*>(lock.pBits) + y * lock.Pitch, px + y * 64 * 4, 64 * 4);
        m.tex->UnlockRect(0);
        m.gen = gen;
    }
    return m.tex;
}
// Binds the mask at `sampler` (point filter, clamp, linear read: no sRGB decode) for one draw and puts the stage back after it
struct WallMaskBind {
    static constexpr int kCount = 6;
    IDirect3DDevice9* dev;
    DWORD s;
    IDirect3DBaseTexture9* old = nullptr;
    DWORD st[kCount] = {};
    static constexpr D3DSAMPLERSTATETYPE kStates[kCount] = {D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER, D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_SRGBTEXTURE};
    WallMaskBind(IDirect3DDevice9* d, DWORD sampler, IDirect3DTexture9* mask) : dev(d), s(sampler) {
        dev->GetTexture(s, &old);
        for (int i = 0; i < kCount; i++) dev->GetSamplerState(s, kStates[i], &st[i]);
        const DWORD want[kCount] = {D3DTEXF_POINT, D3DTEXF_POINT, D3DTEXF_NONE, D3DTADDRESS_CLAMP, D3DTADDRESS_CLAMP, FALSE};
        SetTex(dev, s, mask);
        for (int i = 0; i < kCount; i++) dev->SetSamplerState(s, kStates[i], want[i]);
    }
    ~WallMaskBind() {
        SetTex(dev, s, old);
        if (old) old->Release();
        for (int i = 0; i < kCount; i++) dev->SetSamplerState(s, kStates[i], st[i]);
    }
};

IDirect3DTexture9* AmbientMapTextureFor(IDirect3DDevice9* dev, uintptr_t key) {
    if (!DevSwitches::StairRoomFill() || dev != g_wallMaskDev || !key) return nullptr; // (WallMaskTexture set the device for this draw)
    if (g_ambMapTex.size() > 256) {
        for (auto& [k, m] : g_ambMapTex)
            if (m.tex) m.tex->Release();
        g_ambMapTex.clear();
    }
    AmbMapTex& m = g_ambMapTex[key];
    static float rgba[64 * 64 * 4];
    uint64_t sig = 0;
    if (!LevelLightShare::AmbientMapFor(key, m.tex ? m.sig : 0, sig, rgba)) return nullptr;
    if (!m.tex) {
        if (FAILED(dev->CreateTexture(64, 64, 1, 0, D3DFMT_A32B32G32R32F, D3DPOOL_MANAGED, &m.tex, nullptr)) || !m.tex) return nullptr;
        m.sig = 0;
    }
    if (sig != m.sig) {
        D3DLOCKED_RECT lock{};
        if (FAILED(m.tex->LockRect(0, &lock, nullptr, 0)) || !lock.pBits) return nullptr;
        for (int y = 0; y < 64; y++) { // A32B32G32R32F: r, g, b, a floats in memory
            float* row = reinterpret_cast<float*>(static_cast<BYTE*>(lock.pBits) + y * lock.Pitch);
            std::memcpy(row, rgba + y * 64 * 4, 64 * 4 * sizeof(float));
        }
        m.tex->UnlockRect(0);
        m.sig = sig;
    }
    return m.tex;
}
IDirect3DTexture9* AmbientMapTexture(IDirect3DDevice9* dev, DWORD basisSampler) {
    IDirect3DBaseTexture9* b = nullptr;
    if (FAILED(dev->GetTexture(basisSampler, &b)) || !b) return nullptr;
    const uintptr_t key = reinterpret_cast<uintptr_t>(b);
    b->Release();
    return AmbientMapTextureFor(dev, key);
}
// (development builds, stair_story_blend) whether the current VS passes the world height in TEXCOORD2.w (by shader, render thread)
std::unordered_map<IDirect3DVertexShader9*, bool> g_vsWorldY;
bool CurVsPassesWorldY() {
    if (!g_curVs) return false;
    if (const auto it = g_vsWorldY.find(g_curVs); it != g_vsWorldY.end()) return it->second;
    UINT size = 0;
    bool yes = false;
    if (SUCCEEDED(g_curVs->GetFunction(nullptr, &size)) && size >= 8 && size <= 65536 && size % 4 == 0) {
        std::vector<DWORD> code(size / 4);
        if (SUCCEEDED(g_curVs->GetFunction(code.data(), &size))) yes = ShaderPatches::VsPassesWorldY(code);
    }
    if (g_vsWorldY.size() > 1024) g_vsWorldY.clear();
    g_vsWorldY[g_curVs] = yes;
    return yes;
}

// Every smooth basis copy caps its bicubic reads by the native bilinear read (BasisSmoothOptions::preserveShadows)
constexpr bool kBasisPreserveShadows = true;

template <typename DrawFn> bool DrawBasisSmooth(IDirect3DDevice9* dev, DrawFn draw) {
    if (!g_indoorSmooth.load(std::memory_order_relaxed)) return false;
    // (development builds, basis_wall_filter) the story's wall mask for the maps this draw binds, if known
    IDirect3DTexture9* wallMask = nullptr;
    if (DevSwitches::BasisWallFilter()) {
        // a basis sampler of this shader: the plain copy's sizeSampler (any of the four maps names the story)
        PatchedPs& probe = PatchedFor(dev, g_basisSmoothPs, "Indoor light (smooth)", [](std::vector<DWORD>& t, PatchedPs& pp) { return ShaderPatches::PatchBasisSmooth(t, pp.smooth, {.preserveShadows = kBasisPreserveShadows}); });
        if (probe.ps) wallMask = WallMaskTexture(dev, probe.smooth.sizeSampler);
    }
    // a room-lit shader that also reads the basis maps (the richest InteriorFloor variants): both reads smooth in one copy
    if (g_curPsRoomMap && g_roomMapSmooth.load(std::memory_order_relaxed)) {
        // (the copy with the wall mask needs more registers: where it cannot be made, the copy without it, never the basis-only
        // path below, which leaves the room map unsmoothed)
        PatchedPs* qw = wallMask ? &PatchedFor(dev, g_basisRoomWallPs, "Indoor light and room map (smooth, walls)", [](std::vector<DWORD>& t, PatchedPs& pp) {
                                       return ShaderPatches::PatchBasisSmooth(t, pp.smooth, {.preserveShadows = kBasisPreserveShadows, .wallMask = true}) && ShaderPatches::PatchRoomMapSmooth(t, pp.room, false);
                                   })
                                 : nullptr;
        if (qw && (!qw->ps || qw->smooth.maskSampler < 0)) qw = nullptr, wallMask = nullptr;
        // (arch_ambient) the same copy with the arch block, bound only for a floor of a room with arches (ArchRowsFor). Where the
        // copy with both the walls and the arches cannot be made, neither draw takes the walls (as when one copy held both)
        PatchedPs* qa = nullptr;
        if (DevSwitches::ArchAmbient()) {
            if (qw) {
                PatchedPs& aw = PatchedFor(dev, g_basisRoomWallArchPs, "Indoor light and room map (smooth, walls, arches)", [](std::vector<DWORD>& t, PatchedPs& pp) {
                    return ShaderPatches::PatchBasisSmooth(t, pp.smooth, {.preserveShadows = kBasisPreserveShadows, .wallMask = true}) && ShaderPatches::PatchRoomMapSmooth(t, pp.room, true);
                }, true);
                if (!aw.ps || aw.smooth.maskSampler < 0) qw = nullptr, wallMask = nullptr;
                else qa = ArchCopyOrNull(aw);
            }
            if (!qw)
                qa = ArchCopyOrNull(PatchedFor(dev, g_basisRoomArchPs, "Indoor light and room map (smooth, arches)", [](std::vector<DWORD>& t, PatchedPs& pp) {
                    return ShaderPatches::PatchBasisSmooth(t, pp.smooth, {.preserveShadows = kBasisPreserveShadows}) && ShaderPatches::PatchRoomMapSmooth(t, pp.room, true);
                }, true));
            if (qa && !RoomMapCapsOk(dev, qa->room)) qa = nullptr;
        }
        PatchedPs& qp = qw ? *qw : PatchedFor(dev, g_basisRoomPs, "Indoor light and room map (smooth)", [](std::vector<DWORD>& t, PatchedPs& pp) {
            return ShaderPatches::PatchBasisSmooth(t, pp.smooth, {.preserveShadows = kBasisPreserveShadows}) && ShaderPatches::PatchRoomMapSmooth(t, pp.room, false);
        });
        const bool plainOk = qp.ps && RoomMapCapsOk(dev, qp.room);
        float bsize[4], rsize[4], band[4], oldB[4] = {};
        bool wall = false;
        IDirect3DBaseTexture9* bmap = nullptr;
        const PatchedPs* any = plainOk ? &qp : qa;
        const bool ok = any && SUCCEEDED(dev->GetTexture(any->smooth.sizeSampler, &bmap)) && MapSize(bmap, bsize) && RoomMapConsts(dev, any->room, rsize, band, wall);
        const uintptr_t basisTex = reinterpret_cast<uintptr_t>(bmap); // (one of the draw's directional maps: its story, for the arches)
        if (bmap) bmap->Release();
        if (ok) {
            float rows[kArchRows * 4] = {};
            const int cells = qa ? ArchRowsFor(dev, qa->room, band, basisTex, rows) : 0;
            const bool useArch = qa && (cells > 0 || !plainOk);
            const PatchedPs& q = useArch ? *qa : qp;
            dev->GetPixelShaderConstantF(q.smooth.sizeConst, oldB, 1);
            g_inOwnCall = true;
            SetPsConst(dev, q.smooth.sizeConst, bsize, 1);
            g_inOwnCall = false;
            if (wallMask && q.smooth.maskSampler >= 0) {
                g_inOwnCall = true;
                const WallMaskBind bind(dev, static_cast<DWORD>(q.smooth.maskSampler), wallMask);
                g_inOwnCall = false;
                DrawWithRoomMap(dev, q.ps, q.room, rsize, band, useArch ? rows : nullptr, draw);
                g_inOwnCall = true; // (the bind's restore, at its end of scope)
                g_wallMaskDraws.fetch_add(1, std::memory_order_relaxed);
            } else
                DrawWithRoomMap(dev, q.ps, q.room, rsize, band, useArch ? rows : nullptr, draw);
            g_inOwnCall = false;
            g_inOwnCall = true;
            SetPsConst(dev, q.smooth.sizeConst, oldB, 1);
            g_inOwnCall = false;
            g_basisSmoothDrawn.fetch_add(1, std::memory_order_relaxed);
            (wall ? g_roomWallDrawn : g_roomFloorDrawn).fetch_add(1, std::memory_order_relaxed);
            return true;
        }
    }
    PatchedPs* pw = wallMask ? &PatchedFor(dev, g_basisSmoothWallPs, "Indoor light (smooth, walls)", [](std::vector<DWORD>& t, PatchedPs& pp) { return ShaderPatches::PatchBasisSmooth(t, pp.smooth, {.preserveShadows = kBasisPreserveShadows, .wallMask = true, .floorWall = true}); }) : nullptr;
    if (pw && (!pw->ps || pw->smooth.maskSampler < 0)) pw = nullptr;
    // (stair_room_fill) the copy whose fill is the room's ambient, where the story's ambient map is known
    IDirect3DTexture9* ambMap = pw ? AmbientMapTexture(dev, pw->smooth.sizeSampler) : nullptr;
    if (ambMap) {
        PatchedPs& pf = PatchedFor(dev, g_basisSmoothFillPs, "Indoor light (smooth, walls, room fill)",
                                   [](std::vector<DWORD>& t, PatchedPs& pp) {
                                       return ShaderPatches::PatchBasisSmooth(t, pp.smooth, {.preserveShadows = kBasisPreserveShadows, .wallMask = true, .floorWall = true, .stairRoomFill = true,
                                                                                             .floorTone = DevSwitches::StairFloorTone(), .storyBlend = DevSwitches::StairStoryBlend()});
                                   });
        if (pf.ps && pf.smooth.maskSampler >= 0 && pf.smooth.ambSampler >= 0) pw = &pf;
        else ambMap = nullptr;
    }
    PatchedPs& p = pw ? *pw : PatchedFor(dev, g_basisSmoothPs, "Indoor light (smooth)", [](std::vector<DWORD>& t, PatchedPs& pp) { return ShaderPatches::PatchBasisSmooth(t, pp.smooth, {.preserveShadows = kBasisPreserveShadows}); });
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
    if (pw) {
        // the floor light map read (a stair's top step): floor uv = basis uv x (64 m at 4 texels per metre / its size); its
        // check in the shader (the draw's own zw) turns it off where the VS made it otherwise
        float fsize[4], floorK[4] = {0, 0, 0, 0}, oldFloor[4] = {};
        IDirect3DBaseTexture9* fmap = nullptr;
        if (p.smooth.floorConst >= 0) {
            if (SUCCEEDED(dev->GetTexture(static_cast<DWORD>(p.smooth.floorSampler), &fmap)) && MapSize(fmap, fsize))
                floorK[0] = 4.0f * size[0] / fsize[0], floorK[1] = 4.0f * size[1] / fsize[1], floorK[2] = 1.0f;
            if (fmap) fmap->Release();
            dev->GetPixelShaderConstantF(static_cast<UINT>(p.smooth.floorConst), oldFloor, 1);
            SetPsConst(dev, static_cast<DWORD>(p.smooth.floorConst), floorK, 1);
        }
        if (ambMap) {
            const WallMaskBind bind(dev, static_cast<DWORD>(p.smooth.maskSampler), wallMask);
            const WallMaskBind fill(dev, static_cast<DWORD>(p.smooth.ambSampler), ambMap); // (point, clamp)
            // (stair_story_blend) the story above's maps by direction, its ambient map and the two floors' heights; off (const .z = 0)
            // when not known or the VS does not pass the height (the samplers then hold this story's own, unused)
            float storyK[4] = {0, 0, 0, 0}, oldStory[4] = {};
            IDirect3DBaseTexture9* upTex[5] = {};
            if (p.smooth.storyConst >= 0) {
                uintptr_t cur[4] = {}, up[4] = {};
                for (int d = 0; d < 4; d++) {
                    IDirect3DBaseTexture9* c = nullptr;
                    if (SUCCEEDED(dev->GetTexture(static_cast<DWORD>(p.smooth.basisSampler[d]), &c)) && c) {
                        cur[d] = reinterpret_cast<uintptr_t>(c);
                        c->Release();
                    }
                }
                float yLow = 0, yUp = 0;
                IDirect3DTexture9* upAmb = nullptr;
                if (CurVsPassesWorldY() && LevelLightShare::StoryAboveFor(cur, up, yLow, yUp) && (upAmb = AmbientMapTextureFor(dev, up[0])) != nullptr) {
                    storyK[0] = yLow + 0.5f, storyK[1] = 1.0f / (yUp - yLow), storyK[2] = 1.0f;
                    for (int d = 0; d < 4; d++) upTex[d] = reinterpret_cast<IDirect3DBaseTexture9*>(up[d]);
                    upTex[4] = upAmb;
                } else {
                    for (int d = 0; d < 4; d++) upTex[d] = reinterpret_cast<IDirect3DBaseTexture9*>(cur[d]);
                    upTex[4] = ambMap;
                }
                dev->GetPixelShaderConstantF(static_cast<UINT>(p.smooth.storyConst), oldStory, 1);
                SetPsConst(dev, static_cast<DWORD>(p.smooth.storyConst), storyK, 1);
            }
            const bool blend = p.smooth.storyConst >= 0 && upTex[0];
            std::optional<SamplerBind> u0, u1, u2, u3;
            std::optional<WallMaskBind> uA;
            if (blend) {
                const DWORD b = static_cast<DWORD>(p.smooth.upSampler);
                u0.emplace(dev, b, upTex[0], D3DTEXF_NONE), u1.emplace(dev, b + 1, upTex[1], D3DTEXF_NONE), u2.emplace(dev, b + 2, upTex[2], D3DTEXF_NONE), u3.emplace(dev, b + 3, upTex[3], D3DTEXF_NONE);
                uA.emplace(dev, static_cast<DWORD>(p.smooth.upAmbSampler), static_cast<IDirect3DTexture9*>(upTex[4]));
            }
            draw();
            if (blend) {
                u0.reset(), u1.reset(), u2.reset(), u3.reset(), uA.reset();
                SetPsConst(dev, static_cast<DWORD>(p.smooth.storyConst), oldStory, 1);
            }
            g_ambFillDraws.fetch_add(1, std::memory_order_relaxed);
        } else {
            const WallMaskBind bind(dev, static_cast<DWORD>(p.smooth.maskSampler), wallMask);
            draw(); // (as the plain copy: within the own-call span)
        }
        if (p.smooth.floorConst >= 0) SetPsConst(dev, static_cast<DWORD>(p.smooth.floorConst), oldFloor, 1);
        g_wallMaskDraws.fetch_add(1, std::memory_order_relaxed);
    } else
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

std::atomic<long> g_doorsKept{0}, g_doorsNoWorld{0}; // indoor smooth light: objects set in a wall left to the game's rig / no world triple
std::atomic<bool> g_doorsLogged{false}, g_doorsNoWorldLogged{false};
// An object set in a wall (DrawIndoorObject): unscaled, turned about y only, and its tile's centre (origin + (X - Z) / 2) on an
// outside wall's line, or on a tile edge of the room map along one axis (uv rows 3..4 after the world triple, 0..1 over the
// map, x its size: 1 texel per metre; F7 capture: c195 / c196, the door's centre at texel (12.99, 20.99))
bool SetInWall(const float (&m)[5][4], float sizeU, float sizeV) {
    const float xx = m[0][0], xz = m[2][0], zx = m[0][2], zz = m[2][2];
    const float lx = std::sqrt(xx * xx + xz * xz), lz = std::sqrt(zx * zx + zz * zz);
    if (!(lx > 0.9f && lx < 1.1f && lz > 0.9f && lz < 1.1f) || std::fabs(m[1][0]) + std::fabs(m[1][2]) > 0.05f) return false;
    const float c[3] = {m[0][3] + 0.5f * (xx - zx), m[1][3], m[2][3] + 0.5f * (xz - zz)};
    if (LevelLightShare::OnWallLine(c, xx / lx, xz / lx, 0.15f) || LevelLightShare::OnWallLine(c, zx / lz, zz / lz, 0.15f)) return true;
    const float u = (m[3][0] * c[0] + m[3][1] * c[1] + m[3][2] * c[2] + m[3][3]) * sizeU;
    const float v = (m[4][0] * c[0] + m[4][1] * c[1] + m[4][2] * c[2] + m[4][3]) * sizeV;
    if (!std::isfinite(u) || !std::isfinite(v) || u < -1.0f || v < -1.0f || u > sizeU + 1.0f || v > sizeV + 1.0f) return false;
    return std::fabs(u - std::round(u)) < 0.12f || std::fabs(v - std::round(v)) < 0.12f;
}

template <typename DrawFn> bool DrawIndoorObject(IDirect3DDevice9* dev, DrawFn draw) {
    if (!g_indoorSmooth.load(std::memory_order_relaxed) || !g_curVsInfo || RigMode() != 0) return false;
    IDirect3DTexture9* basis[4] = {};
    IDirect3DBaseTexture9* lightMap = nullptr; // held until the scale is read
    int lmS = -1;
    // 14/10 (draw cost): only the samplers the shader declares (a texture left bound elsewhere is not its room map), the
    // sampler that held it at this shader's last draw first (a room-lit shader reads one room light map)
    PsFacts* facts = nullptr;
    if (PerfToggles::IndoorSamplerScan.load(std::memory_order_relaxed)) {
        if (g_curFactsKnown) facts = g_curFacts;
        else if (g_curPs)
            if (const auto it = g_psFacts.find(g_curPs); it != g_psFacts.end()) facts = &it->second;
    }
    const unsigned declared = facts && facts->declared ? facts->declared & 0xFFu : 0xFFu;
    const int first = facts ? facts->lastLm : -1;
    const auto tryS = [&](DWORD s) {
        IDirect3DBaseTexture9* t = nullptr;
        if (FAILED(dev->GetTexture(s, &t)) || !t) return;
        if (t->GetType() == D3DRTYPE_TEXTURE && RoomMapPadding::BasisFor(static_cast<IDirect3DTexture9*>(t), basis)) {
            lmS = static_cast<int>(s);
            lightMap = t;
        } else
            t->Release();
    };
    if (first >= 0 && first < 8 && (declared & (1u << first))) tryS(static_cast<DWORD>(first));
    for (DWORD s = 0; s < 8 && lmS < 0; s++)
        if ((declared & (1u << s)) && static_cast<int>(s) != first) tryS(s);
    if (facts && lmS >= 0) facts->lastLm = static_cast<int8_t>(lmS);
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
    // A door or window set in its wall spans two rooms: the smooth read (a B-spline over 4 texels, +-2 m) mixed the room
    // behind the wall into the faces on this side (F7 capture, 07/10: a sconce's room at ~41% on the dark room's door frame).
    // The game's rig (one room, the camera's side) lights it, as before Apex.
    if (const int wk = g_curVsInfo->patched.worldK; wk >= 0) {
        float wm[5][4];
        if (SUCCEEDED(dev->GetVertexShaderConstantF(static_cast<UINT>(wk), &wm[0][0], 5)) && SetInWall(wm, size[0], size[1])) {
            if (g_doorsKept.fetch_add(1, std::memory_order_relaxed) == 0 && !g_doorsLogged.exchange(true))
                LOG_INFO("[LotLightBridge] Indoor smooth light: doors and windows set in a wall keep the game's room light");
            return false;
        }
    } else if (g_doorsNoWorld.fetch_add(1, std::memory_order_relaxed) == 0 && !g_doorsNoWorldLogged.exchange(true))
        LOG_INFO("[LotLightBridge] Indoor smooth light: an object shader with no world triple (doors in it cannot be told apart)");
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

// Recognized ExteriorWall variants only: lamp RGB gains a daytime contribution.
// ExteriorWall shaders derive output alpha from final luminance; that alpha is the game's bloom mask. Raising cK.x for
// RGB therefore also raised bloom on the whole wall/foundation. Keep the game's alpha, and apply Apex's gain only to RGB
// (found and validated by idavidveiga's fork, 04/10: a white bloom wash over lit walls and foundations).
std::atomic<int> g_wallDrawn{0};
std::atomic<int> g_wallBloomPreserved{0};
std::atomic<int> g_wallBloomFallback{0};
template <typename DrawFn> bool DrawWallGain(IDirect3DDevice9* dev, DrawFn draw) {
    const float gain = g_wallGain.load(std::memory_order_relaxed);
    if (!g_wallEnabled.load(std::memory_order_relaxed)) return false;
    auto it = g_wallConst.find(g_curPs);
    if (it == g_wallConst.end()) return false;

    float c[4], old[4];
    if (FAILED(dev->GetPixelShaderConstantF(it->second, old, 1))) return false;
    if (!std::isfinite(old[0]) || old[0] < 0) return false;
    g_nativeWallScale = old[0]; // the walls' native lamp scale, which the reach law gives objects too (NativeWallScale)
    const DWORD drawTick = g_lotDrawTick;
    g_nativeWallTick = drawTick ? drawTick : 1;
    std::memcpy(c, old, sizeof(c));
    c[0] = TerrainLightingPolicy::WallLampScale(old[0], g_night.load(std::memory_order_relaxed), gain);
    if (c[0] == old[0]) return false; // unchanged lamp scale: retain the native draw

    DWORD colorMask = 0, blend = FALSE, zWrite = FALSE, stencilWriteMask = 0;
    const bool haveStates =
        SUCCEEDED(dev->GetRenderState(D3DRS_COLORWRITEENABLE, &colorMask)) &&
        SUCCEEDED(dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend)) &&
        SUCCEEDED(dev->GetRenderState(D3DRS_ZWRITEENABLE, &zWrite)) &&
        SUCCEEDED(dev->GetRenderState(D3DRS_STENCILWRITEMASK, &stencilWriteMask));

    const DWORD alphaMask = colorMask & D3DCOLORWRITEENABLE_ALPHA;
    const DWORD rgbMask = colorMask & (D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE);

    g_inOwnCall = true;
    if (haveStates && alphaMask && !blend) {
        // Pass 1 writes only the vanilla bloom alpha. It must not consume depth/stencil writes because pass 2 is the
        // actual wall draw for geometry state. Opaque ExteriorWall draws have blending off; blended variants deliberately
        // stay on the old one-pass path below rather than risk changing their compositing semantics.
        dev->SetRenderState(D3DRS_COLORWRITEENABLE, alphaMask);
        if (zWrite) dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        if (stencilWriteMask) dev->SetRenderState(D3DRS_STENCILWRITEMASK, 0);
        draw(); // game's original cK.x -> original bloom mask
        if (stencilWriteMask) dev->SetRenderState(D3DRS_STENCILWRITEMASK, stencilWriteMask);
        if (zWrite) dev->SetRenderState(D3DRS_ZWRITEENABLE, zWrite);

        // Pass 2 is the normal geometry write, but RGB only, with Apex's stronger baked lamp term.
        dev->SetRenderState(D3DRS_COLORWRITEENABLE, rgbMask);
        SetPsConst(dev, it->second, c, 1);
        draw();
        SetPsConst(dev, it->second, old, 1);
        dev->SetRenderState(D3DRS_COLORWRITEENABLE, colorMask);
        g_wallBloomPreserved.fetch_add(1, std::memory_order_relaxed);
    } else {
        // If the wall is blended, alpha is not writable, or a state query failed, preserve the exact previous behaviour.
        SetPsConst(dev, it->second, c, 1);
        draw();
        SetPsConst(dev, it->second, old, 1);
        g_wallBloomFallback.fetch_add(1, std::memory_order_relaxed);
    }
    g_inOwnCall = false;
    g_wallDrawn.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// Full-day bloom guard for the base-game cinema/theatre facade. The exact VS is BFFCCC56/1060. The main marquee
// uses D5ED0EF3/864; the narrow centre panel uses 4E570819/500 by day and the F7-captured 36F5E915/1296 at night.
// Every PS must still independently prove the isolated TS3 luminance-bloom threshold through BloomThresholdConst();
// changing that cK.x only suppresses bloom alpha and leaves RGB intact. The centre panel stays limited to <=4 primitives.
std::atomic<int> g_cinemaDayBloomSuppressed{0};
std::atomic<int> g_cinemaPanelDayBloomSuppressed{0};
template <typename DrawFn> bool DrawCinemaMarqueeDayBloomGuard(IDirect3DDevice9* dev, UINT primitiveCount, DrawFn draw) {
    if (!g_curVsInfo || !g_curVsInfo->cinemaMarqueeDay) return false;
    const auto it = g_cinemaMarqueeDayPs.find(g_curPs);
    if (it == g_cinemaMarqueeDayPs.end() || RigMode() != 2) return false;

    const bool panel = g_cinemaMarqueePanelDayPs.count(g_curPs) != 0;
    if (panel && primitiveCount > 4) return false;

    const DWORD thresholdConst = it->second;
    float old[4] = {};
    if (FAILED(dev->GetPixelShaderConstantF(thresholdConst, old, 1))) return false;
    // The captures use 1.3. Keep a generous TS3 material range, but fail closed if the constant has an unexpected state.
    if (!(old[0] >= 0.5f && old[0] <= 5.0f)) return false;

    float noBloom[4] = {1000.0f, old[1], old[2], old[3]};
    g_inOwnCall = true;
    SetPsConst(dev, thresholdConst, noBloom, 1);
    draw();
    SetPsConst(dev, thresholdConst, old, 1);
    g_inOwnCall = false;
    g_cinemaDayBloomSuppressed.fetch_add(1, std::memory_order_relaxed);
    if (panel) g_cinemaPanelDayBloomSuppressed.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// Light through doors and windows (08/10): the winter floors weight the atlas by 1 - the floor map's alpha too, as the summer
// outdoor floors do (DrawFloorAtlas), while the option is active. The snowy floor tiles' shader (DrawFloor) is weighted only
// with kSnowTileMask: turn it off if an F7 capture shows that shader drawing an indoor floor (indoor texels carry the
// game's own alpha, 1 - their brightness, 0x006A33DD, which would dim the atlas there).
constexpr bool kSnowTileMask = true;
// The atlas weight constant of a winter floor shader, (1, -1) = 1 - map alpha, (1, 0) = whole (as before)
void WinterAtlasWeight(const PatchedPs& p, bool allowed, float out[4]) {
    out[0] = 1.0f;
    out[1] = allowed && p.floor.maskAlpha && LevelLightShare::FloorWallsActive() && LevelLightShare::RealisticOpeningsActive() ? -1.0f : 0.0f;
    out[2] = out[3] = 0.0f;
}
void LogWinterMask(PatchedPs& p, const char* what) {
    if (p.floor.loggedMask) return;
    p.floor.loggedMask = true;
    LOG_INFO(std::format("[LotLightBridge] {}: shader {:08X} {} the walls' mask (the floor map's alpha)", what, reinterpret_cast<uintptr_t>(g_curPs),
                         p.floor.heightConst < 0 ? "CANNOT READ" : "reads"));
}
template <typename DrawFn> bool DrawFloor(IDirect3DDevice9* dev, DrawFn draw) {
    float c[4];
    IDirect3DTexture9* atlas = LightmapSmooth::Atlas(c);
    if (!atlas) return false;
    const ShaderPatches::WorldY y = g_curVsInfo ? g_curVsInfo->worldY : ShaderPatches::WorldY{};
    PatchedPs& p = y.texcoord < 0 ? PatchedFor(dev, g_floorPs, "Floor", [](std::vector<DWORD>& t, PatchedPs& pp) { return ShaderPatches::PatchFloor(t, pp.floor); })
                                  : PatchedFor(dev, g_floorShadePs[FloorShadeKey(0, y)], "Floor (ground lamp shade)",
                                               [y](std::vector<DWORD>& t, PatchedPs& pp) { return ShaderPatches::PatchFloor(t, pp.floor, y); });
    if (!p.ps) return false;
    LogWinterMask(p, "Snowy floor");
    float oldC[4] = {}, oldH[4] = {}, weight[4];
    WinterAtlasWeight(p, kSnowTileMask, weight);
    dev->GetPixelShaderConstantF(p.floor.atlasConst, oldC, 1);
    if (p.floor.heightConst >= 0) dev->GetPixelShaderConstantF(static_cast<UINT>(p.floor.heightConst), oldH, 1);
    IDirect3DPixelShader9* original = g_curPs;
    g_inOwnCall = true;
    {
        SamplerBind bind(dev, p.floor.atlasSampler, atlas, D3DTEXF_NONE);
        SetPsConst(dev, p.floor.atlasConst, c, 1);
        if (p.floor.heightConst >= 0) SetPsConst(dev, static_cast<UINT>(p.floor.heightConst), weight, 1);
        FloorShadeBind shade(dev, p.floor, 8, 1.0f); // world xz in TEXCOORD0.zw (IsFloorVs: rows c8 .. c10)
        SetPs(dev, p.ps);
        ConstGain lampGain(dev, p.floor.scaleConst, FloorDayGain()); // the lamp scale of max(map, atlas)
        if (LightProbe::Capturing()) g_objDrawInfo = "mod draw: winter floor tile | " + shade.Text();
        draw();
        g_objDrawInfo.clear();
        SetPs(dev, original);
        SetPsConst(dev, p.floor.atlasConst, oldC, 1);
        if (p.floor.heightConst >= 0) SetPsConst(dev, static_cast<UINT>(p.floor.heightConst), oldH, 1);
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
    // world y: none in the snow-on-floor vertex shaders of the precomp (tools/floor_shade_check), so no shade there for now
    const ShaderPatches::WorldY y = g_curVsInfo ? g_curVsInfo->worldY : ShaderPatches::WorldY{};
    PatchedPs& p = y.texcoord < 0 ? PatchedFor(dev, g_curSnowFloorTc == 0 ? g_snowFloorPs0 : g_snowFloorPs, "Snow on floors", [tc = g_curSnowFloorTc](std::vector<DWORD>& t, PatchedPs& pp) { return ShaderPatches::PatchSnowFloor(t, tc, pp.floor); })
                                  : PatchedFor(dev, g_floorShadePs[FloorShadeKey(g_curSnowFloorTc == 0 ? 1 : 2, y)], "Snow on floors (ground lamp shade)",
                                               [tc = g_curSnowFloorTc, y](std::vector<DWORD>& t, PatchedPs& pp) { return ShaderPatches::PatchSnowFloor(t, tc, pp.floor, y); });
    if (!p.ps) return false;
    // the map it lights with must be a room light map (A8R8G8B8 managed: 32x64 in m69 / m71, 512x128 in m72), not a terrain map (DXT5)
    bool roomMap = false;
    IDirect3DBaseTexture9* map = nullptr;
    if (SUCCEEDED(dev->GetTexture(p.floor.mapSampler, &map)) && map) {
        D3DSURFACE_DESC d{};
        // (Lamp lighting: Instant: a relit copy of Real-time outdoor lamps is a DEFAULT target: the game's map behind it decides;
        // Classic: the map itself)
        IDirect3DBaseTexture9* game = static_cast<IDirect3DBaseTexture9*>(const_cast<void*>(RtOutdoorLamps::GameMapOf(map)));
        if (game->GetType() == D3DRTYPE_TEXTURE && SUCCEEDED(static_cast<IDirect3DTexture9*>(game)->GetLevelDesc(0, &d)))
            roomMap = d.Format == D3DFMT_A8R8G8B8 && d.Pool != D3DPOOL_DEFAULT && d.Width <= 1024 && d.Height <= 1024;
        map->Release();
    }
    if (!roomMap) return false;
    LogWinterMask(p, "Snow on floors");
    const float half[4] = {c[0] * 2.0f, c[1] * 2.0f, c[2], c[3]};
    float oldC[4] = {}, oldH[4] = {}, weight[4];
    WinterAtlasWeight(p, true, weight); // snow lies outdoors only
    dev->GetPixelShaderConstantF(p.floor.atlasConst, oldC, 1);
    if (p.floor.heightConst >= 0) dev->GetPixelShaderConstantF(static_cast<UINT>(p.floor.heightConst), oldH, 1);
    IDirect3DPixelShader9* original = g_curPs;
    g_inOwnCall = true;
    {
        SamplerBind bind(dev, p.floor.atlasSampler, atlas, D3DTEXF_NONE);
        SetPsConst(dev, p.floor.atlasConst, half, 1);
        if (p.floor.heightConst >= 0) SetPsConst(dev, static_cast<UINT>(p.floor.heightConst), weight, 1);
        FloorShadeBind shade(dev, p.floor, 8, 2.0f); // world xz / 2 (IsSnowFloorVs: rows c8 .. c10)
        SetPs(dev, p.ps);
        ConstGain lampGain(dev, p.floor.scaleConst, FloorDayGain()); // the lamp scale of max(map, atlas)
        if (LightProbe::Capturing()) g_objDrawInfo = "mod draw: snow on a floor | " + shade.Text();
        draw();
        g_objDrawInfo.clear();
        SetPs(dev, original);
        SetPsConst(dev, p.floor.atlasConst, oldC, 1);
        if (p.floor.heightConst >= 0) SetPsConst(dev, static_cast<UINT>(p.floor.heightConst), oldH, 1);
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
            int tc = -1, wk = -1;
            if (SUCCEEDED(g_curVs->GetFunction(t.data(), &size)) && ShaderPatches::PatchObjectLampVs(t, false, &tc, &wk) && SUCCEEDED(dev->CreateVertexShader(t.data(), &fv.vs))) {
                fv.tc = tc;
                fv.worldConst = wk;
            }
            else
                fv.vs = nullptr;
        }
        LOG_INFO(std::format("[LotLightBridge] Outdoor floor: vertex shader {:08X} {} (TEXCOORD{})", reinterpret_cast<uintptr_t>(g_curVs), fv.vs ? "patched" : "without the expected pattern", fv.tc));
    }
    if (!fv.vs) return false;
    PatchedPs& p = PatchedFor(dev, g_floorAtlasPs[fv.tc], "Outdoor floor", [tc = fv.tc](std::vector<DWORD>& t, PatchedPs& pp) { return ShaderPatches::PatchBakedAtlasPs(t, tc, pp.floor); });
    if (!p.ps) return false;
    if (!p.floor.loggedMask) {
        p.floor.loggedMask = true;
        LOG_INFO(std::format("[LotLightBridge] Outdoor floor: shader {:08X} {} the walls' mask (the floor map's alpha)", reinterpret_cast<uintptr_t>(g_curPs), p.floor.maskAlpha ? "reads" : "CANNOT READ"));
    }
    // The atlas has no walls: it is weighted by 1 - the floor map's alpha, where level_light_share stores the share of the
    // lamps' light that walls block for the outdoor floor texels of upper stories (0 everywhere else, as the game has it:
    // the atlas stays whole). Without a readable alpha the weight constant is (1, 0): always 1.
    const float height[4] = {1.0f, p.floor.maskAlpha && LevelLightShare::FloorWallsActive() ? -1.0f : 0.0f, 0.0f, 0.0f};
    float oldC[4] = {}, oldH[4] = {};
    dev->GetPixelShaderConstantF(p.floor.atlasConst, oldC, 1);
    dev->GetPixelShaderConstantF(static_cast<UINT>(p.floor.heightConst), oldH, 1);
    IDirect3DPixelShader9* originalPs = g_curPs;
    IDirect3DVertexShader9* originalVs = g_curVs;
    g_inOwnCall = true;
    {
        SamplerBind bind(dev, p.floor.atlasSampler, atlas, D3DTEXF_NONE);
        SetPsConst(dev, p.floor.atlasConst, c, 1);
        SetPsConst(dev, static_cast<UINT>(p.floor.heightConst), height, 1);
        // story 0: max(map, atlas) x the lot's ground lamp shade (world x, z, y = the copy's TEXCOORDn.xyz)
        FloorShadeBind shade(dev, p.floor, fv.worldConst, 1.0f);
        SetVs(dev, fv.vs);
        SetPs(dev, p.ps);
        ConstGain lampGain(dev, p.floor.scaleConst, FloorDayGain()); // the lamp scale of max(map, atlas)
        if (LightProbe::Capturing())
            g_objDrawInfo = std::format("mod draw: outdoor floor | atlas weight 1 - floor map alpha: {} | {}", height[1] < 0.0f ? "on" : "off", shade.Text());
        draw();
        g_objDrawInfo.clear();
        SetPs(dev, originalPs);
        SetVs(dev, originalVs);
        SetPsConst(dev, p.floor.atlasConst, oldC, 1);
        SetPsConst(dev, static_cast<UINT>(p.floor.heightConst), oldH, 1);
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
        const std::vector<DWORD> code = PatchedVsCode(vs, g_curVsInfo->cls);
        if (code.empty() || FAILED(dev->CreateVertexShader(code.data(), &f.vs))) f.vs = nullptr;
        LOG_INFO(std::format("[LotLightBridge] Foliage: vertex shader {:08X} {}", reinterpret_cast<uintptr_t>(vs), f.vs ? "patched" : "failed"));
    }
    return f.vs;
}
// The per-vertex lamp copy of the current foliage vertex shader (switch foliage_vertex_lamps), made from the wrap copy
IDirect3DVertexShader9* FoliageVertexVsFor(IDirect3DDevice9* dev, IDirect3DVertexShader9* vs, int& base) {
    base = -1;
    if (!g_curVsInfo || g_curVsInfo->cls != 6) return nullptr;
    FoliageVs& f = g_curVsInfo->patched;
    if (!f.perVertexTried) {
        f.perVertexTried = true;
        std::vector<DWORD> code = PatchedVsCode(vs, g_curVsInfo->cls);
        std::vector<uint32_t> t(code.begin(), code.end());
        int b = -1;
        if (!code.empty() && FoliageVertexLamps::Patch(t, b) && SUCCEEDED(dev->CreateVertexShader(reinterpret_cast<const DWORD*>(t.data()), &f.perVertex))) f.perVertexBase = b;
        else f.perVertex = nullptr;
        // a refused shader (the wrap copy the patch read: FNV-1a, size), kept once in Apex Radiance\Censo as a fixture for the
        // offline check (tools/foliage_vertex_test); development builds only
        std::string refused;
        if (!f.perVertex) {
            uint32_t h = 2166136261u;
            for (DWORD d : code) h = (h ^ d) * 16777619u;
            const size_t bytes = code.size() * sizeof(DWORD);
            refused = std::format("not of the captured family ({}): left as before; code VS_{:08X} ({} bytes)", code.empty() ? "no wrap copy" : FoliageVertexLamps::LastRefusal(), h, bytes);
            if (DevSwitches::kCompiled) try {
                const std::filesystem::path dir = std::filesystem::path(ApexPaths::ApexDirectory()) / L"Censo";
                const std::filesystem::path file = dir / std::format("foliage_VS_{:08X}.bin", h);
                if (!code.empty() && !std::filesystem::exists(file)) {
                    std::filesystem::create_directories(dir);
                    std::ofstream(file, std::ios::binary).write(reinterpret_cast<const char*>(code.data()), static_cast<std::streamsize>(bytes));
                }
            } catch (...) {
            }
        }
        LOG_INFO(std::format("[LotLightBridge] Foliage per-vertex lamps: vertex shader {:08X} {}", reinterpret_cast<uintptr_t>(vs),
                             f.perVertex ? std::format("patched (constants c{}..c{})", b, b + 81) : refused));
    }
    base = f.perVertexBase;
    return f.perVertex;
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
        consts[32][0] = g_roofStrength.load(std::memory_order_relaxed) * TerrainLightingPolicy::LampDay(g_night.load(std::memory_order_relaxed)); // x the daytime curve (12/10)
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

// ---- The sun's (and moon's) shadow on trees (switch foliage_sun_shadow; FoliageVertexLamps::PatchTreeSunShadow).
// The frame's shadow map is the depth texture on s5 of the draws whose pixel shader reads it (bushes, roofs, the lot pass,
// water, the game's shadowed tree variants); it is used only in the frame it was seen in or the next one, and forgotten
// before a Reset (render thread; the game owns it, no reference is kept).
IDirect3DBaseTexture9* g_sunShadowMap = nullptr;
UINT g_sunShadowSize = 0;
uint32_t g_sunShadowFrame = 0;
std::atomic<long> g_treeShadowDrawn{0};
// foliage draws that kept the game's pixel shader, by cause (log): no shadow map seen yet, none in this frame or the last,
// a vertex shader without oT5 / oT6.x, a pixel shader the patch refused (most: not a tree, or one already shadowed)
std::atomic<long> g_treeShadowNoMap{0}, g_treeShadowStale{0}, g_treeShadowNoVsOut{0}, g_treeShadowNotPatched{0};
// (log) draws into the shadow map, sampled one frame in 120: all of them, and the instanced foliage ones (class 6)
std::atomic<long> g_shadowPassDraws{0}, g_shadowPassFoliage{0};
void LogTreeShadow() {
    static DWORD s_logged = 0;
    const DWORD now = GetTickCount();
    if (now - s_logged < 30000) return;
    s_logged = now;
    LOG_INFO(std::format("[Foliage] Sun shadow on trees: {} draws with it; kept the game's: {} no shadow map seen, {} not seen this frame, {} vertex shader "
                         "without oT5/oT6, {} pixel shader not patched; shadow map {}x{}; draws into it (one frame in 120): {} ({} instanced foliage)",
                         g_treeShadowDrawn.load(), g_treeShadowNoMap.load(), g_treeShadowStale.load(), g_treeShadowNoVsOut.load(), g_treeShadowNotPatched.load(),
                         g_sunShadowSize, g_sunShadowSize, g_shadowPassDraws.load(), g_shadowPassFoliage.load()));
}
// (log) a draw into the shadow map, one frame in 120; each vertex shader drawing into it is named once (with whether it
// reads per-instance constants c0[a0]: an instanced caster such as trees)
void NoteShadowPassDraw(IDirect3DDevice9* dev) {
    // (only a map seen bound this frame or the last: an older pointer may be a texture the game released)
    if (!g_sunShadowMap || (g_roomMapFrame % 120) != 0 || g_roomMapFrame - g_sunShadowFrame > 1) return;
    static IDirect3DBaseTexture9* s_for = nullptr;
    static IDirect3DSurface9* s_surf = nullptr; // compared by address only (the game owns it)
    if (s_for != g_sunShadowMap) {
        s_for = g_sunShadowMap, s_surf = nullptr;
        IDirect3DSurface9* s = nullptr;
        if (SUCCEEDED(static_cast<IDirect3DTexture9*>(g_sunShadowMap)->GetSurfaceLevel(0, &s)) && s) s_surf = s, s->Release();
    }
    IDirect3DSurface9* ds = nullptr;
    if (!s_surf || FAILED(dev->GetDepthStencilSurface(&ds)) || !ds) return;
    const bool shadowPass = ds == s_surf;
    ds->Release();
    if (!shadowPass) return;
    g_shadowPassDraws.fetch_add(1, std::memory_order_relaxed);
    if (g_curVsIsFoliage) g_shadowPassFoliage.fetch_add(1, std::memory_order_relaxed);
    static std::unordered_map<IDirect3DVertexShader9*, bool> s_seen;
    if (!g_curVs || s_seen.size() > 64 || s_seen.count(g_curVs)) return;
    UINT size = 0;
    std::vector<DWORD> t;
    if (SUCCEEDED(g_curVs->GetFunction(nullptr, &size)) && size >= 8 && size <= 65536 && size % 4 == 0) {
        t.resize(size / 4);
        if (FAILED(g_curVs->GetFunction(t.data(), &size))) t.clear();
    }
    bool instanced = false; // a constant source with relative addressing (0x2000) on c0..c2
    for (size_t i = 1; i < t.size() && !instanced; i++) {
        const DWORD d = t[i];
        instanced = (d & 0x80000000u) && (d & 0x2000u) && (((d >> 28) & 7) | (((d >> 11) & 3) << 3)) == 2 && (d & 0x7FF) <= 2;
    }
    uint32_t h = 2166136261u;
    for (DWORD v : t) h = (h ^ v) * 16777619u;
    s_seen.emplace(g_curVs, instanced);
    LOG_INFO(std::format("[Foliage] Shadow pass vertex shader VS_{:08X} ({} bytes){}{}", h, size, instanced ? ": instanced (c0[a0])" : "", g_curVsIsFoliage ? ", foliage" : ""));
}
void ForgetSunShadow(IDirect3DDevice9*) { g_sunShadowMap = nullptr; }
void NoteSunShadow(IDirect3DDevice9* dev) {
    static const bool s_registered = (RenderCallbacks::preReset.Add(ForgetSunShadow), true);
    (void)s_registered;
    IDirect3DBaseTexture9* t = nullptr;
    if (FAILED(dev->GetTexture(5, &t)) || !t) return;
    if (t != g_sunShadowMap && t->GetType() == D3DRTYPE_TEXTURE) {
        D3DSURFACE_DESC d{};
        const bool depth = SUCCEEDED(static_cast<IDirect3DTexture9*>(t)->GetLevelDesc(0, &d)) &&
                           (d.Format == D3DFMT_D24S8 || d.Format == D3DFMT_D24X8 || d.Format == D3DFMT_D16 || d.Format == D3DFMT_D24FS8 || d.Format == D3DFMT_D32);
        if (depth && d.Width >= 256 && d.Width == d.Height) {
            if (!g_sunShadowMap) LOG_INFO(std::format("[Foliage] Sun shadow map seen: {}x{} (format {})", d.Width, d.Height, static_cast<int>(d.Format)));
            g_sunShadowMap = t, g_sunShadowSize = d.Width;
        }
    }
    if (t == g_sunShadowMap) g_sunShadowFrame = g_roomMapFrame;
    t->Release();
}
struct TreeShadowPs {
    IDirect3DPixelShader9* ps = nullptr;
    int base = -1;
    bool tried = false;
};
std::unordered_map<IDirect3DPixelShader9*, TreeShadowPs> g_treeShadowPs;
// Whether a vertex shader writes the shadow position oT5 (xyzw) and the edge fade oT6.x the patched pixel shader reads
// (VS_2C4FA3E0 does; the per-vertex lamp family VS_9A10A3A8 does not: such a pair keeps the game's pixel shader)
std::unordered_map<IDirect3DVertexShader9*, bool> g_vsWritesShadow;
bool VsWritesShadow(IDirect3DVertexShader9* vs) {
    if (!vs) return false;
    const auto it = g_vsWritesShadow.find(vs);
    if (it != g_vsWritesShadow.end()) return it->second;
    UINT size = 0;
    std::vector<DWORD> t;
    if (SUCCEEDED(vs->GetFunction(nullptr, &size)) && size >= 8 && size <= 65536 && size % 4 == 0) {
        t.resize(size / 4);
        if (FAILED(vs->GetFunction(t.data(), &size))) t.clear();
    }
    DWORD masks[2] = {};
    for (size_t i = 1; i < t.size();) { // the destination token of every instruction (an oTn: register type 6)
        const DWORD op = t[i] & 0xFFFF;
        if (op == 0xFFFF) break;
        const DWORD len = op == 0xFFFE ? (t[i] >> 16) & 0x7FFF : (t[i] >> 24) & 0xF;
        if (op != 0xFFFE && op != 0x1F && op != 0x51 && len >= 1 && i + 1 < t.size()) {
            const DWORD d = t[i + 1], type = ((d >> 28) & 7) | (((d >> 11) & 3) << 3), num = d & 0x7FF;
            if (type == 6 && (num == 5 || num == 6)) masks[num - 5] |= (d >> 16) & 0xF;
        }
        i += 1 + len;
    }
    const bool ok = masks[0] == 0xF && (masks[1] & 0x1);
    g_vsWritesShadow.emplace(vs, ok);
    return ok;
}
// Draws a tree whose pixel shader reads no shadow with the patched copy, the frame's shadow map on s5; false: as before
template <typename DrawFn> bool DrawTreeSunShadow(IDirect3DDevice9* dev, DrawFn draw) {
    if (!DevSwitches::FoliageSunShadow() || !g_curPs) return false;
    LogTreeShadow();
    if (!g_sunShadowMap) return g_treeShadowNoMap.fetch_add(1, std::memory_order_relaxed), false;
    if (g_roomMapFrame - g_sunShadowFrame > 1) return g_treeShadowStale.fetch_add(1, std::memory_order_relaxed), false;
    if (!VsWritesShadow(g_curVs)) return g_treeShadowNoVsOut.fetch_add(1, std::memory_order_relaxed), false;
    TreeShadowPs& e = g_treeShadowPs[g_curPs];
    if (!e.tried) {
        e.tried = true;
        UINT size = 0;
        std::vector<uint32_t> t;
        if (SUCCEEDED(g_curPs->GetFunction(nullptr, &size)) && size >= 8 && size <= 65536 && size % 4 == 0) {
            t.resize(size / 4);
            if (FAILED(g_curPs->GetFunction(t.data(), &size))) t.clear();
        }
        int b = -1;
        if (!t.empty() && FoliageVertexLamps::PatchTreeSunShadow(t, b) && SUCCEEDED(dev->CreatePixelShader(reinterpret_cast<const DWORD*>(t.data()), &e.ps))) e.base = b;
        else e.ps = nullptr;
        // (only the tree family is logged: other foliage pixel shaders are refused for having no sun term)
        if (e.ps || std::strcmp(FoliageVertexLamps::LastRefusal(), "no sun term (mov_sat t1.wzyx, mad .y c0)") != 0)
            LOG_INFO(std::format("[Foliage] Tree pixel shader {:08X}: {}", reinterpret_cast<uintptr_t>(g_curPs),
                                 e.ps ? std::format("reads the sun shadow (constants c{}..c{})", e.base, e.base + 4) : std::format("left as before ({})", FoliageVertexLamps::LastRefusal())));
    }
    if (!e.ps) return g_treeShadowNotPatched.fetch_add(1, std::memory_order_relaxed), false;
    const UINT base = static_cast<UINT>(e.base);
    float saved[5][4] = {};
    if (FAILED(dev->GetPixelShaderConstantF(base, &saved[0][0], 5))) return false;
    // half a texel; a depth bias of 1/2048 of the map's depth (about 0.1 m at the captured scale, against leaves shading
    // themselves); a floor for the light through a crown
    const float o = 0.5f / static_cast<float>(std::max<UINT>(g_sunShadowSize, 1)), bias = 1.0f / 2048.0f, fl = FoliageVertexLamps::kTreeShadowFloor;
    const float k[5][4] = {{-o, -o, -bias, 0.0f}, {o, -o, -bias, 0.0f}, {-o, o, -bias, 0.0f}, {o, o, -bias, 0.0f}, {0.25f, 1.0f, 1.0f - fl, fl}};
    IDirect3DPixelShader9* original = g_curPs;
    g_inOwnCall = true;
    {
        SamplerBind bind(dev, 5, g_sunShadowMap, D3DTEXF_NONE);
        SetPs(dev, e.ps);
        SetPsConst(dev, base, &k[0][0], 5);
        draw();
        SetPsConst(dev, base, &saved[0][0], 5);
        SetPs(dev, original);
    }
    g_inOwnCall = false;
    g_treeShadowDrawn.fetch_add(1, std::memory_order_relaxed);
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
struct TerrainLampScale { DWORD sampler; int constant; bool squared; };
std::unordered_map<IDirect3DPixelShader9*, TerrainLampScale> g_terrainLampConst;
int TerrainLampConst(IDirect3DPixelShader9* ps, DWORD sampler, bool& squared) {
    auto it = g_terrainLampConst.find(ps);
    if (it != g_terrainLampConst.end() && it->second.sampler == sampler) {
        squared = it->second.squared;
        return it->second.constant;
    }
    const int k = ShaderPatches::LightMapScaleConst(ShaderCode(ps), sampler, &squared);
    g_terrainLampConst[ps] = {sampler, k, squared};
    LOG_INFO(std::format("[LotLightBridge] Ground brightness: terrain shader {:08X}, light map s{} -> {}", reinterpret_cast<uintptr_t>(ps), sampler,
                         k >= 0 ? std::format("c{}.x", k) : std::string("no lamp-only scale found, left as the game")));
    return k;
}

// Development Lighting + Bloom census attribution. A single draw can pass through more than one Apex layer
// (for example Rooms at Night + a smoothed indoor-object draw, or the foliage VS + its pixel fix), so keep a
// bit mask for this draw rather than only the final family name. The mask is reset only by the top-level
// diagnostic draw hook and has no effect on rendering.
enum class ClaimSource : uint8_t {
    IndoorBasisSmooth,
    ObjectRigNightFix,
    RoofLampFix,
    RoofSnowLampFix,
    LakeLampFix,
    FoliageMoonShadowPS,
    ExteriorWallGain,
    CinemaMarqueeDayBloomGuard,
    RoadLight,
    FloorLight,
    OutdoorFloorLight,
    FenceStairGroundLight,
    SnowOnObjectGroundLight,
    SnowOnStairGroundLight,
    IndoorObjectSmooth,
    OutdoorObjectGroundLight,
    LotSnowLight,
    SnowFloorLight,
    WorldTerrainLight,
    LotLightAtlas,
    RoomsAtNightFurniture,
    FoliageVertex,
    WindowOutdoorLight,
    RoomMapSmooth,
    Count,
};
constexpr size_t kClaimSourceCount = static_cast<size_t>(ClaimSource::Count);
static_assert(kClaimSourceCount <= 32);
constexpr std::array<const char*, kClaimSourceCount> kClaimSourceNames = {
    "IndoorBasisSmooth",
    "ObjectRigNightFix",
    "RoofLampFix",
    "RoofSnowLampFix",
    "LakeLampFix",
    "FoliageMoonShadowPS",
    "ExteriorWallGain",
    "CinemaMarqueeDayBloomGuard",
    "RoadLight",
    "FloorLight",
    "OutdoorFloorLight",
    "FenceStairGroundLight",
    "SnowOnObjectGroundLight",
    "SnowOnStairGroundLight",
    "IndoorObjectSmooth",
    "OutdoorObjectGroundLight",
    "LotSnowLight",
    "SnowFloorLight",
    "WorldTerrainLight",
    "LotLightAtlas",
    "RoomsAtNightFurniture",
    "FoliageVertex",
    "WindowOutdoorLight",
    "RoomMapSmooth",
};
UINT g_curPrims = 0; // primitive count of the current hooked draw; set by both DrawPrimitive and DrawIndexedPrimitive
uint32_t g_claimSourceMask = 0; // render thread; meaningful only while a development census draw is being attributed
inline void MarkClaimSource(ClaimSource source) {
    g_claimSourceMask |= 1u << static_cast<unsigned>(source);
}
inline D3D9Hooks::HookAction NoteClaim(ClaimSource source) {
    MarkClaimSource(source);
    return D3D9Hooks::HookAction::Skip;
}

template <typename DrawFn> D3D9Hooks::HookAction OnDrawInnerCore(IDirect3DDevice9* dev, DrawFn draw) {
    constexpr auto kSkip = D3D9Hooks::HookAction::Skip;
    constexpr auto kContinue = D3D9Hooks::HookAction::Continue;
    if (g_curPsBasis) RoomMapPadding::NoteDraw(dev, g_curPs); // note the room light maps it binds (edge padding)
    // the frame's sun shadow map, from the draws that read it (the trees' shadow, DrawTreeSunShadow)
    if (DevSwitches::FoliageSunShadow() && (g_curClass == PsClass::ObjectRig || g_curClass == PsClass::Roof || g_curClass == PsClass::LotLight || g_curClass == PsClass::Lake))
        NoteSunShadow(dev);
    // (not for fences / snow relief / rig objects: those draws keep their own fixes)
    if (g_curPsBasis && !g_curVsIsInstanced && !g_curVsIsSnowRelief && !g_curVsIsObject && DrawBasisSmooth(dev, draw)) return NoteClaim(ClaimSource::IndoorBasisSmooth);
    const bool roomFloor=kMemoryOptimizationsEnabled && g_curVsInfo && g_curVsInfo->packedRoomFloor;
    if (g_curPsRoomMap && !g_curVsIsInstanced && !g_curVsIsSnowRelief && (!g_curVsIsObject || roomFloor) && DrawRoomMapSmooth(dev, draw)) return NoteClaim(ClaimSource::RoomMapSmooth);
    if (g_curClass == PsClass::ObjectRig) return DrawObjectRig(dev, draw) ? NoteClaim(ClaimSource::ObjectRigNightFix) : kContinue;
    if (g_curClass == PsClass::Roof) return DrawRoof(dev, draw) ? NoteClaim(ClaimSource::RoofLampFix) : kContinue;
    // The snowy roof pixel shader is also the one of snow on stair tops (same bytes, LightProbe-neve-escada); the vertex
    // shader tells them apart (the stair one takes the snow base in TEXCOORD2) and stairs go to DrawSnowRelief below.
    if (g_curClass == PsClass::RoofSnow && !g_curVsIsSnowRelief) return DrawRoofSnow(dev, draw) ? NoteClaim(ClaimSource::RoofSnowLampFix) : kContinue;
    if (g_curClass == PsClass::Lake) return DrawLake(dev, draw) ? NoteClaim(ClaimSource::LakeLampFix) : kContinue;
    if (g_curClass == PsClass::Sea) return DrawLake(dev, draw, true) ? NoteClaim(ClaimSource::LakeLampFix) : kContinue;
    if (g_curClass == PsClass::LakeWeather) return DrawLake(dev, draw, false, true) ? NoteClaim(ClaimSource::LakeLampFix) : kContinue;
    if (g_curVsIsFoliage) {
        if (DevSwitches::FoliageSunShadow()) NoteSunShadow(dev); // (the game's shadowed foliage variants bind it too)
        if (DrawLeafShadow(dev, draw)) return NoteClaim(ClaimSource::FoliageMoonShadowPS);
        return DrawTreeSunShadow(dev, draw) ? NoteClaim(ClaimSource::FoliageMoonShadowPS) : kContinue; // (the same claim: foliage shadow)
    }
    if (g_curClass == PsClass::WallGain) {
        return DrawWallGain(dev, draw) ? NoteClaim(ClaimSource::ExteriorWallGain) : kContinue;
    }
    if (!g_enabled.load(std::memory_order_relaxed)) return kContinue;
    if (DrawCinemaMarqueeDayBloomGuard(dev, g_curPrims, draw)) return NoteClaim(ClaimSource::CinemaMarqueeDayBloomGuard);
    if (g_curVsIsRoad) return DrawRoad(dev, draw) ? NoteClaim(ClaimSource::RoadLight) : kContinue;
    if (g_curVsIsFloor) return DrawFloor(dev, draw) ? NoteClaim(ClaimSource::FloorLight) : kContinue;
    if (g_curClass == PsClass::FloorAtlas && !g_curVsIsSnowFloor) return DrawFloorAtlas(dev, draw) ? NoteClaim(ClaimSource::OutdoorFloorLight) : kContinue;
    if (g_curVsIsInstanced) return DrawInstanced(dev, draw) ? NoteClaim(ClaimSource::FenceStairGroundLight) : kContinue;
    if (g_curVsIsSnowCover) return DrawSnowCover(dev, draw) ? NoteClaim(ClaimSource::SnowOnObjectGroundLight) : kContinue;
    if (g_curVsIsSnowRelief) return DrawSnowRelief(dev, draw) ? NoteClaim(ClaimSource::SnowOnStairGroundLight) : kContinue;
    // Class 10 also holds roof and snow vertex shaders: when the object patch does not apply, fall through to the rest.
    if (g_curClass == PsClass::LotLightMelt) return DrawLotMelt(dev, draw) ? NoteClaim(ClaimSource::LotSnowLight) : kContinue; // its VS looks like an object rig
    if (g_curVsIsObject && g_curPsBasis && DrawWindowOutdoor(dev, draw)) return NoteClaim(ClaimSource::WindowOutdoorLight); // experimental, off by default
    if (g_curVsIsObject && DrawIndoorObject(dev, draw)) return NoteClaim(ClaimSource::IndoorObjectSmooth);
    if (g_curVsIsObject && DrawObjectLamp(dev, draw)) return NoteClaim(ClaimSource::OutdoorObjectGroundLight);
    if (g_curClass == PsClass::LotLightSnow || g_curClass == PsClass::LotLightSnowCut)
        return DrawLotSnow(dev, draw, g_curClass == PsClass::LotLightSnowCut) ? NoteClaim(ClaimSource::LotSnowLight) : kContinue;
    if (g_curClass == PsClass::WorldCandidate || g_curClass == PsClass::WorldMultiLight || g_curClass == PsClass::WorldCompact) {
        const bool multi = g_curClass == PsClass::WorldMultiLight;
        if (multi && (!g_curVsInfo || !g_curVsInfo->worldMultiLight)) return kContinue;
        if (g_curClass == PsClass::WorldCompact && (!g_curVsInfo || !g_curVsInfo->worldCompact)) return kContinue;
        std::pair<int, int> key;
        ChunkTex* chunk = nullptr;
        const DWORD s = RecordWorldChunk(dev, key, chunk, multi ? 13 : 15);
        if (!s) {
            // not a world terrain chunk: the snow-on-floor pixel shaders (m69, m71) also declare s6+ and land here
            if (g_curVsIsSnowFloor && DrawSnowFloor(dev, draw)) return NoteClaim(ClaimSource::SnowFloorLight);
            return D3D9Hooks::HookAction::Continue;
        }
        IDirect3DTexture9* smooth = LightmapSmooth::Get(key, static_cast<IDirect3DTexture9*>(chunk->tex));
        // (15/10) the walls' shade on the lamp light of the world grass around lots (ChunkShadeTexture); with no smoothed map
        // the game's own map is drawn through the same patched shader
        IDirect3DTexture9* shade = ChunkShadeTexture(dev, key);
        // Native terrain alpha carries solar visibility. Smooth lamp RGB only.
        PatchedPs* alpha = nullptr;
        if (smooth || shade) {
            alpha = &PatchedFor(dev, g_terrainAlphaPs, "Terrain native solar alpha", [s](std::vector<DWORD>& t, PatchedPs& p) {
                p.nativeAlphaSource = s;
                return ShaderPatches::PatchTerrainNativeAlpha(t, s, p.nativeAlphaSampler);
            });
            if (!alpha->ps || alpha->nativeAlphaSource != s) smooth = nullptr, shade = nullptr; // unsupported variant: preserve the game's shadow map
        }
        const float gain = GroundGain();
        const float dayWeight = 1.0f - g_night.load(std::memory_order_relaxed);
        PatchedPs* daylight = nullptr;
        if ((smooth || shade) && !multi && dayWeight > 0.0f) {
            daylight = &PatchedFor(dev, g_terrainDayPs, "Terrain daylight range", [s](std::vector<DWORD>& t, PatchedPs& p) {
                p.nativeAlphaSource = s;
                return ShaderPatches::PatchTerrainNativeAlpha(t, s, p.nativeAlphaSampler) &&
                       ShaderPatches::PatchTerrainDaylightRange(t, p.terrainDayConst);
            });
            if (!daylight->ps || daylight->nativeAlphaSource != s) daylight = nullptr;
        }
        // the shaded copies of both (a variant: the copy without the shade draws when one is refused)
        PatchedPs* alphaShade = nullptr;
        PatchedPs* dayShade = nullptr;
        if (shade) {
            alphaShade = &PatchedFor(dev, g_terrainShadePs, "Terrain wall shade", [s](std::vector<DWORD>& t, PatchedPs& p) {
                p.nativeAlphaSource = s;
                return ShaderPatches::PatchTerrainNativeAlpha(t, s, p.nativeAlphaSampler) && ShaderPatches::PatchTerrainShade(t, s, p.shadeSampler);
            }, true);
            if (!alphaShade->ps || alphaShade->nativeAlphaSource != s || alphaShade->nativeAlphaSampler != alpha->nativeAlphaSampler) alphaShade = nullptr;
            if (daylight) {
                dayShade = &PatchedFor(dev, g_terrainDayShadePs, "Terrain daylight range with wall shade", [s](std::vector<DWORD>& t, PatchedPs& p) {
                    p.nativeAlphaSource = s;
                    return ShaderPatches::PatchTerrainNativeAlpha(t, s, p.nativeAlphaSampler) && ShaderPatches::PatchTerrainDaylightRange(t, p.terrainDayConst) &&
                           ShaderPatches::PatchTerrainShade(t, s, p.shadeSampler);
                }, true);
                if (!dayShade->ps || dayShade->nativeAlphaSource != s || dayShade->nativeAlphaSampler != alpha->nativeAlphaSampler ||
                    dayShade->terrainDayConst != daylight->terrainDayConst)
                    dayShade = nullptr;
            }
            if (!alphaShade) shade = nullptr;
        }
        bool squared = multi;
        const int k = multi ? 3 : TerrainLampConst(g_curPs, s, squared);
        if (!smooth && !shade && k < 0) return D3D9Hooks::HookAction::Continue;
        IDirect3DBaseTexture9* old = nullptr;
        if ((smooth || shade) && FAILED(dev->GetTexture(s, &old))) smooth = nullptr, shade = nullptr, old = nullptr;
        // the light map the patched shader reads: the smoothed one, else the game's own (bound already)
        IDirect3DBaseTexture9* const lampMap = smooth ? smooth : (shade ? old : nullptr);
        if (!lampMap) shade = nullptr;
        if (LightProbe::Capturing())
            g_objDrawInfo = std::format("mod draw: world terrain | compact {} | multi-pass {} | lamp sampler s{} | gain {:.3f} | smoothed {} | daylight range candidate {} | wall shade {}",
                                       g_curClass == PsClass::WorldCompact, multi, s, gain, smooth != nullptr, daylight != nullptr,
                                       shade ? std::format("chunk ({}, {}), sampler s{}", key.first, key.second, alphaShade->shadeSampler) : std::string("none"));
        g_inOwnCall = true;
        {
            TerrainConst lampGain(dev, k, squared);
            if (lampMap) {
                NativeTerrainSampler native(dev, s, alpha->nativeAlphaSampler, old, D3D9Hooks::CallOriginalSetTexture);
                if (native) {
                    float savedDay[4]{};
                    bool daySet = false;
                    if (daylight && SUCCEEDED(dev->GetPixelShaderConstantF(daylight->terrainDayConst, savedDay, 1))) {
                        const float range[4] = {dayWeight, 2.0f, 0, 0};
                        daySet = SUCCEEDED(D3D9Hooks::CallOriginalSetPixelShaderConstantF(dev, daylight->terrainDayConst, range, 1));
                    }
                    PatchedPs* const shaded = shade ? (daySet ? dayShade : alphaShade) : nullptr;
                    std::optional<SamplerBind> shadeMap;
                    if (shaded) shadeMap.emplace(dev, shaded->shadeSampler, shade, D3DTEXF_LINEAR);
                    SetPs(dev, shaded ? shaded->ps : daySet ? daylight->ps : alpha->ps);
                    SetTex(dev, s, lampMap);
                    draw();
                    SetTex(dev, s, old);
                    SetPs(dev, g_curPs);
                    shadeMap.reset();
                    if (daySet) SetPsConst(dev, daylight->terrainDayConst, savedDay, 1);
                } else draw();
            } else draw();
        }
        g_inOwnCall = false;
        g_objDrawInfo.clear();
        if (old) old->Release();
        return NoteClaim(ClaimSource::WorldTerrainLight);
    }
    if (g_curClass != PsClass::LotLight) {
        // snow lying on floor tiles (m69, m71): only draws no pixel-shader class claimed, see ClassifyVs
        if (g_curVsIsSnowFloor && DrawSnowFloor(dev, draw)) return NoteClaim(ClaimSource::SnowFloorLight);
        return kContinue;
    }

    EnsureReplacement(dev);
    if (!g_replacementPs) return D3D9Hooks::HookAction::Continue;
    const bool wet = g_wetLotPs.count(g_curPs) != 0;
    if (wet) EnsureWetReplacement(dev);
    IDirect3DPixelShader9* const replacement = wet ? g_replacementWetPs : g_replacementPs;
    if (!replacement) return D3D9Hooks::HookAction::Continue;
    float v[8];
    if (FAILED(dev->GetVertexShaderConstantF(14, v, 2)) || !Near(v[0], 1.0f / 256.0f) || !Near(v[1], 1.0f / 256.0f)) return D3D9Hooks::HookAction::Continue;
    // World atlas when ready, as in DrawLotSnow (a lot can reach past its home chunk). The summer lot VS computes the
    // light map uv as (world.xz - c15.xz) * c14.xy + c14.zw; c15 also feeds another uv (c13), so only c14 changes.
    float atlasC[4]{};
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
    float savedEdge[16]; // c28..c31: do not modify the draw unless restoration is possible
    if (FAILED(dev->GetPixelShaderConstantF(28, savedEdge, 4))) return kContinue;
    if (LightProbe::Capturing())
        g_objDrawInfo = feather ? std::format("mod draw: lot light pass | soft edges: lot {:08X}{:08X}, {:.0f} x {:.0f} m, origin ({:.2f}, {:.2f}), band {:.1f} m (PS c28..c30)",
                                              rect->lotHi, rect->lotLo, rect->w, rect->d, rect->tx, rect->tz, kEdgeBand)
                                : std::string("mod draw: lot light pass | soft edges: ") + (g_softEdges.load() ? "NOT applied, lot rectangle not found" : "off (option)");
    // Before full night (12/10, one daytime curve): c3.x = ground gain x LampDay(n) for the terrain and the lot map alike,
    // c31.x = the lot map's night gain; at full night the previous constants (the game's c3.x x GroundGain(), LotMapGain())
    const TerrainLightingPolicy::LotPassConstants dayPass = TerrainLightingPolicy::LotPass(
        g_night.load(std::memory_order_relaxed), g_groundGain.load(std::memory_order_relaxed), g_lotMapGain.load(std::memory_order_relaxed));
    float lotGain[4] = {dayPass.day ? dayPass.lotGain : LotMapGain(), 0, 0, 0};
    float lotUvScale[4]{};
    if (g_curVsInfo && g_curVsInfo->contractedLotUv && SUCCEEDED(dev->GetVertexShaderConstantF(12, lotUvScale, 1))
        && std::isfinite(lotUvScale[0]) && lotUvScale[0] > 0 && lotUvScale[0] <= 1
        && lotUvScale[0] == lotUvScale[1]) {
        lotGain[2] = 1.0f / 63.0f;
        lotGain[3] = -lotUvScale[0] * (16.0f / 63.0f);
    }

    IDirect3DPixelShader9* original = g_curPs;
    g_inOwnCall = true;
    {
        // s2 = the terrain light (clamp, linear, linear mips, no sRGB): only the states that differ are set and restored
        SamplerBind terrainMap(dev, 2, terrain);
        float maskRows[8];
        IDirect3DTexture9* const mask = LotMaskFor(dev, atlas ? atlasMap : v, &v[4], &lotM[0], &lotM[8], rect, maskRows);
        SamplerBind maskMap(dev, 3, mask, D3DTEXF_NONE);
        float savedMask[8];
        const bool maskSaved = SUCCEEDED(dev->GetPixelShaderConstantF(32, savedMask, 2));
        SetPsConst(dev, 32, maskRows, 2);
        SetPs(dev, replacement);
        if (atlas) SetVsConst(dev, 14, atlasMap, 1);
        SetPsConst(dev, 28, edge, 3);
        SetPsConst(dev, 31, lotGain, 1);
        // c3.x scales only the lamp terms of kReplacementHlsl
        ConstGain lampGain(dev, 3, dayPass.day ? 1.0f : GroundGain());
        ConstX dayScale(dev, 3, dayPass.day, dayPass.c3x);
        draw();
        if (atlas) SetVsConst(dev, 14, v, 1);
        SetPsConst(dev, 28, savedEdge, 4);
        if (maskSaved) SetPsConst(dev, 32, savedMask, 2);
    }
    g_objDrawInfo.clear();
    SetPs(dev, original);
    g_inOwnCall = false;
    g_lotDrawn.fetch_add(1, std::memory_order_relaxed);
    return NoteClaim(ClaimSource::LotLightAtlas);
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
    if (!ps || !PerfToggles::NightLightingPsFacts.load(std::memory_order_relaxed)) {
        g_curFacts = nullptr;
        g_curFactsKnown = false;
        g_curClass = Classify(ps);
        g_curPsBasis = ps && g_basisPs.count(ps);
        g_curPsRoomMap = ps && g_roomMapPs.count(ps);
        return;
    }
    auto it = g_psFacts.find(ps); // one lookup for a known shader
    if (it == g_psFacts.end()) {
        Classify(ps);
        it = g_psFacts.find(ps);
        if (it == g_psFacts.end()) { // (Classify always records one; kept safe)
            g_curFacts = nullptr;
            g_curFactsKnown = false;
            g_curClass = Classify(ps);
            g_curPsBasis = g_basisPs.count(ps) != 0;
            g_curPsRoomMap = g_roomMapPs.count(ps) != 0;
            return;
        }
    }
    g_curFacts = &it->second;
    g_curFactsKnown = true;
    g_curClass = it->second.cls;
    g_curPsBasis = it->second.basis;
    g_curPsRoomMap = it->second.roomMap;
}

void TrackVs(IDirect3DVertexShader9* vs, bool force = false) {
    if (vs == g_curVs && !force) return;
    g_curVs = vs;
    VsInfo* info = ClassifyVs(vs);
    g_curVsInfo = info;
    const uint8_t cls = info ? info->cls : 0;
    g_curVsIsRoof = cls == 1;
    g_curVsIsLake = cls == 2;
    g_curVsIsSea = cls == 12;
    g_curVsIsLakeWeather = cls == 13;
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

// Rooms at Night on furniture (unlit_rooms.h; objects that responded to nothing, mostly on the upper floor): an
// indoor object (room-mode rig, RigTracker mode 0) is lit by its rig's four lights plus the ambient cube. Room-mode rigs have no
// sun (FUN_006bbde0: start slot = (mode == 1)): in an unlit room the lights are the game's three [NoLight] lights and the fill
// light, the only blue on furniture (the cube, CASDiffuseProbe, is flat grey; a second study). While it
// draws, through the game's shader or Apex's, those lights (PS c4..c7 of shaders with that chain, recognised by direction /
// fill w, UnlitRooms::IsUnlitLight; dim bluish vertex lights) and the cube's weight get the room's Brightness and Blue tint;
// lamps keep their colour and strength. The draw is then made here and every
// constant put back. The game's own shader draws with a copy whose ambient cube colour is pulled towards its grey by the
// Blue tint too (PatchCubeTint; the blue tint was the only control missing on furniture), as
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
// The hue of a room's ambient (+0x110, the colour its walls and floors take as c4), at luma 1: each channel at most
// kFurnitureChromaMax (a far pure-blue lamp gave x3.6 blue), by bringing the hue towards white at the same luma (never
// darker); false when the room is unknown, unreadable or its ambient has no light. It replaces the Blue tint in a lit
// room: the tint is the unlit-room colour's
constexpr float kFurnitureChromaMax = 1.5f;
bool RoomAmbientChroma(const void* room, float* chroma) {
    if (!room) return false;
    float c[3];
    __try {
        std::memcpy(c, static_cast<const BYTE*>(room) + 0x110, sizeof c);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    const float luma = 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
    if (!(luma > 1e-5f) || !std::isfinite(luma)) return false;
    // the hue at luma 1, then brought towards white until no channel passes the cap (1 + s (chroma - 1) keeps luma 1)
    float top = 0.0f;
    for (int k = 0; k < 3; k++) top = std::max(top, chroma[k] = std::max(c[k], 0.0f) / luma);
    if (top > kFurnitureChromaMax) {
        const float s = (kFurnitureChromaMax - 1.0f) / (top - 1.0f);
        for (int k = 0; k < 3; k++) chroma[k] = 1.0f + s * (chroma[k] - 1.0f);
    }
    return std::isfinite(chroma[0] + chroma[1] + chroma[2]);
}
// The furniture draw with the rig's unlit-room lights, the vertex lights and the cube weight turned (rig = PS c0..c7 or
// null when the shader has no rig chain; chroma: the room's ambient hue for the fill, or null)
template <typename DrawFn> D3D9Hooks::HookAction OnDrawFurniture(IDirect3DDevice9* dev, DrawFn draw, const ShaderPatches::RigPsInfo& info, const float (*rig)[4], const float* chroma) {
    float psOld[4][4] = {}, vsOld[4][4] = {}, cubeOld[4] = {}, t[4][4], cube[4];
    bool psSet = false, vsSet = false, cubeSet = false;
    // PS c0..c3 = the slots' directions, c4..c7 their colours (each slot tested with its direction: fill and [NoLight]
    // lights are turned, lamps are not; room-mode rigs have no sun, slot 0 is the strongest room light)
    if (rig) {
        std::memcpy(psOld, rig[4], sizeof psOld);
        std::memcpy(t, psOld, sizeof t);
        for (int k = 0; k < 4; k++) {
            const bool turned = UnlitRooms::FurnitureColour(t[k], rig[k], chroma);
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
        for (int k = 0; k < 4; k++) vsSet |= UnlitRooms::FurnitureColour(t[k], vsDir[k], chroma);
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
        MarkClaimSource(ClaimSource::RoomsAtNightFurniture);
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
struct TraceSeen {
    std::vector<std::pair<uint64_t, DWORD>> states; // the part's recent states and when each was last seen (GetTickCount)
    uint64_t last = 0;                              // the state seen at its last draw
};
std::unordered_map<uint64_t, TraceSeen> g_traceLast; // object part (rig + pixel shader) -> its states (render thread)
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
    // Written when the part's state changes. A part drawn twice a frame (two passes with different maps) alternates every
    // frame: a state seen less than kTraceRecentMs ago is not written again. A state that comes back later is (06/10: lights
    // off, on, off again: the furniture's second "off" was never written, since every state was written once at most, and
    // the recording looked as if the objects kept the lamp light).
    constexpr DWORD kTraceRecentMs = 300;
    const DWORD now = GetTickCount();
    auto [it, fresh] = g_traceLast.try_emplace(key);
    TraceSeen& seen = it->second;
    const uint64_t last = seen.last;
    seen.last = state;
    auto s = std::find_if(seen.states.begin(), seen.states.end(), [state](const auto& x) { return x.first == state; });
    if (s != seen.states.end()) {
        const DWORD at = s->second;
        s->second = now;
        if (state == last || now - at < kTraceRecentMs) return;
    } else {
        if (seen.states.size() >= 8) seen.states.erase(seen.states.begin());
        seen.states.emplace_back(state, now);
    }
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

// ---- A lamp switch's furniture waits for its room (objects change together with the walls) ----
// At a switch the game updates the rigs of the lamp's room at once (a recording: rig lights changed 0.05 s after
// the switch), while the room's walls and its light map show the new light only when its solve ends (0.1 .. 0.5 s): the
// lamp's light left the furniture first, then the room followed. Every room-mode part draw remembers its rig's lights
// (PS c0..c7 and the vertex lights) while no switch is on; during a switch, while the rig's room still awaits its new
// light (RoomLightQueue::AwaitingSwitchLight; the rig's room from its last gather, ObjectLightBridge::RigRoom), the part is
// drawn with the lights it had before the switch (a snapshot taken at most 2 s before it), then with the game's.
// 2.13.1 (Tester switch furnitureWithRoom, on): every lamp edit, not only switches. A recording (14/10, an indoor lamp
// dragged): the furniture of the lamp's room followed the lamp at every step (+0.00 .. +0.66 s after the edit) while the
// room's maps showed the new light only at +0.69 .. +1.05 s, and nothing was held ("held for a lamp switch 0"): the
// snapshot had to be older than the latest switch, so a move or a colour never found one. Now the snapshot must be from a
// frame before the edit's episode began (the first frame a switch or an edit hold was seen; frames, not GetTickCount, whose
// 16 ms steps put a snapshot and the edit in the same tick), and the part waits while its room's maps do not show the edit
// (AtriumHold::RoomShowsOldLight: held by the edit hold, trailing after it, or the own story's room not solved yet).
struct RigSnap {
    float ps[8][4];
    float vs[8][4];
    bool haveVs;
    DWORD tick;
    uint32_t frame = 0;     // g_roomMapFrame when taken
    uint64_t heldGame = 0;  // the game's lights last seen while held (a hash: the changes held back are counted)
    // (furnitureWithRoom) the snapshot from before the current episode, kept when a part not waiting yet is drawn during it
    // (its room marked a few frames after the edit): used only while the part has shown no other lights since
    float prePs[8][4];
    float preVs[8][4];
    bool preHaveVs = false;
    DWORD preTick = 0;
    uint32_t preEpisode = 0; // the episode (its first frame) it was kept for
};
std::unordered_map<uint64_t, RigSnap> g_rigSnaps; // (rig, vertex-light register) -> its lights before a switch (render thread)
std::atomic<long> g_rigHeldDraws{0};
constexpr DWORD kRigSnapFreshMs = 2000;
// (HoldRig) the time a part waits from an edit's start for its room to be marked
constexpr DWORD kRigMarkGraceMs = 400;
std::atomic<long> g_rigGraceHeld{0};
// LevelLightShare::LampEditPending, read once a frame (it takes the urgent list's lock; HoldRig runs per draw)
bool EditPendingThisFrame() {
    static uint32_t s_frame = ~0u;
    static bool s_pending = false;
    if (s_frame != g_roomMapFrame) s_frame = g_roomMapFrame, s_pending = LevelLightShare::LampEditPending();
    return s_pending;
}
// g_rigSnaps bounds: past kRigSnapsMax entries they are all dropped, past kRigSnapsEpisodeMax during an episode (whose
// pre-episode snapshots a drop would lose). Outside an episode, OnPresent drops every kRigSnapSweepMs the parts not drawn
// for kRigSnapKeepMs: HoldRig uses a snapshot only up to kRigSnapFreshMs + 4000 ms before the episode or the switch.
constexpr size_t kRigSnapsMax = 16384, kRigSnapsEpisodeMax = 32768;
constexpr DWORD kRigSnapKeepMs = 30000, kRigSnapSweepMs = 5000;
static_assert(kRigSnapKeepMs > kRigSnapFreshMs + 4000);
DWORD g_rigSweepAt = 0;
// The current episode (render thread): a switch or an edit hold seen since g_rigEpisodeFrame; ended by OnPresent
bool g_rigEpisodeOn = false;
uint32_t g_rigEpisodeFrame = 0;
DWORD g_rigEpisodeTick = 0;
std::unordered_set<uint64_t> g_rigEpisodeParts; // parts held in this episode (the log)
long g_rigEpisodeChanges = 0;                  // the game's light changes of held parts not shown (the log)
long g_rigEpisodeLate = 0;                     // parts whose room waited only after they showed new lights: left as they are
DWORD g_rigEpisodeLastHeld = 0;                // the last held draw (GetTickCount)
bool g_rigTrailing = false; // AtriumHold::Trailing, read once a frame (OnPresent): it walks the urgent rooms
bool RigEpisodeWanted() {
    return LampMarkFilter::SwitchActive() || AtriumHold::SwitchHolding() ||
           (TesterToggles::furnitureWithRoom.load(std::memory_order_relaxed) && (AtriumHold::EditHolding() || g_rigTrailing));
}
// AtriumHold::RoomShowsOldLight once per room and frame (it locks the urgent list; thousands of furniture draws a frame)
std::unordered_map<const void*, bool> g_rigRoomWaits;
uint32_t g_rigRoomWaitsFrame = 0;
bool RoomShowsOldLightCached(const void* room) {
    if (g_rigRoomWaitsFrame != g_roomMapFrame) {
        g_rigRoomWaits.clear();
        g_rigRoomWaitsFrame = g_roomMapFrame;
    }
    const auto [it, fresh] = g_rigRoomWaits.try_emplace(room, false);
    if (fresh) it->second = AtriumHold::RoomShowsOldLight(room);
    return it->second;
}
uint64_t RigHash(const float (*rig)[4]) {
    uint64_t h = 1469598103934665603ull;
    const auto* b = reinterpret_cast<const unsigned char*>(rig);
    for (size_t i = 0; i < sizeof(float) * 32; i++) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}
struct RigHeld {
    bool ps = false, vs = false;
    int vl = -1;
    float psGame[8][4];
    float vsGame[8][4];
};
// rig: the pixel shader's rig lights c0..c7, or null for a part whose shader has no rig chain (its vertex lights alone)
void HoldRig(IDirect3DDevice9* dev, float (*rig)[4], RigHeld& h) {
    const uintptr_t rp = RigTracker::CurrentRig();
    if (!rp) return;
    const int vl = g_curVsInfo ? g_curVsInfo->patched.vertexLight : -1;
    // (a part without a rig chain has a snapshot of its own: one with a chain of the same rig must never restore its zeros)
    const uint64_t key = ((static_cast<uint64_t>(rp) << 8) ^ static_cast<uint64_t>(vl & 0xFF)) | (rig ? 0 : 1ull << 63);
    const DWORD now = GetTickCount();
    const unsigned long lastSwitch = LampMarkFilter::SwitchLastTick();
    const bool withRoom = TesterToggles::furnitureWithRoom.load(std::memory_order_relaxed);
    const bool episode = RigEpisodeWanted();
    if (episode && !g_rigEpisodeOn) { // (the first draw of the episode: snapshots from earlier frames only)
        g_rigEpisodeOn = true;
        g_rigEpisodeFrame = g_roomMapFrame;
        g_rigEpisodeTick = now;
        g_rigEpisodeParts.clear();
        g_rigEpisodeChanges = 0;
        g_rigEpisodeLate = 0;
        g_rigEpisodeLastHeld = 0;
    }
    bool wait = false;
    if (withRoom) {
        const void* room = episode ? ObjectLightBridge::RigRoom(rp) : nullptr;
        wait = room && (AtriumHold::AllAtOnce() ? RoomShowsOldLightCached(room) : RoomLightQueue::AwaitingSwitchLight(room));
        // The game updates a rig at once, while its room is marked only when LevelLightShare sends it (about 140 ms after the
        // edit): for kRigMarkGraceMs from the episode's start, while a lamp edit is pending, a part waits as if its room
        // were marked. A part whose room the edit does not change shows the same lights either way; after the grace a room
        // still not marked shows the new lights, as before.
        if (!wait && room && g_rigEpisodeOn && now - g_rigEpisodeTick < kRigMarkGraceMs && EditPendingThisFrame()) {
            wait = true;
            g_rigGraceHeld.fetch_add(1, std::memory_order_relaxed);
        }
    } else { // (2.13.0: switches only)
        const void* room = LampMarkFilter::SwitchActive() || AtriumHold::SwitchHolding() ? ObjectLightBridge::RigRoom(rp) : nullptr;
        // switches all at once: until the whole switch is released with the rooms' maps (AtriumHold); else until its own room ends.
        // 12/10: the lamp's own story is never held (its maps show once solved): its furniture waits for its own room's solve
        // (14/10, Classic: show all floors together: the own story is held too, so its furniture waits for the release)
        wait = !room ? false
               : AtriumHold::AllAtOnce() && (AtriumHold::OwnStoryHeld() || !LevelLightShare::OnEditStory(room))
                   ? AtriumHold::SwitchHolding() && LevelLightShare::LampUrgency(room) > 1.0f
                   : RoomLightQueue::AwaitingSwitchLight(room);
    }
    if (!wait) { // the lights as the game set them, for the next switch (not just after one: the rig may hold its new lights)
        if (lastSwitch && now - lastSwitch < 100) return;
        if (g_rigSnaps.size() > (g_rigEpisodeOn ? kRigSnapsEpisodeMax : kRigSnapsMax)) g_rigSnaps.clear();
        RigSnap& s = g_rigSnaps[key];
        if (withRoom && g_rigEpisodeOn && s.tick && static_cast<int32_t>(s.frame - g_rigEpisodeFrame) < 0 && s.preEpisode != g_rigEpisodeFrame) {
            std::memcpy(s.prePs, s.ps, sizeof s.prePs); // (the lights from before the episode, once per episode)
            std::memcpy(s.preVs, s.vs, sizeof s.preVs);
            s.preHaveVs = s.haveVs;
            s.preTick = s.tick;
            s.preEpisode = g_rigEpisodeFrame;
        }
        if (rig) std::memcpy(s.ps, rig, sizeof s.ps);
        else std::memset(s.ps, 0, sizeof s.ps);
        s.haveVs = vl >= 4 && SUCCEEDED(dev->GetVertexShaderConstantF(static_cast<UINT>(vl - 4), &s.vs[0][0], 8));
        if (s.preEpisode == g_rigEpisodeFrame && g_rigEpisodeOn &&
            (std::memcmp(s.ps, s.prePs, sizeof s.ps) != 0 || (s.haveVs && std::memcmp(s.vs, s.preVs, sizeof s.vs) != 0)))
            s.preEpisode = 0; // (it showed other lights in this episode: holding it now would turn it back)
        s.tick = now;
        s.frame = g_roomMapFrame;
        s.heldGame = 0;
        return;
    }
    const auto it = g_rigSnaps.find(key);
    if (it == g_rigSnaps.end()) return;
    RigSnap& s = it->second;
    const float (*snapPs)[4] = s.ps;
    const float (*snapVs)[4] = s.vs;
    bool snapHaveVs = s.haveVs;
    if (withRoom) { // taken in a frame before the episode began, and not long before it
        DWORD snapTick = s.tick;
        if (static_cast<int32_t>(s.frame - g_rigEpisodeFrame) >= 0) {
            // drawn during the episode before its room waited: its lights from before the episode, if it showed no others
            if (s.preEpisode != g_rigEpisodeFrame) {
                if (!s.heldGame) {
                    s.heldGame = 1; // (counted once)
                    g_rigEpisodeLate++;
                }
                return;
            }
            snapPs = s.prePs;
            snapVs = s.preVs;
            snapHaveVs = s.preHaveVs;
            snapTick = s.preTick;
        }
        if (static_cast<int32_t>(g_rigEpisodeTick - snapTick) > static_cast<int32_t>(kRigSnapFreshMs + 4000)) return; // (long before the edit)
        const uint64_t game = rig ? RigHash(rig) : 1;
        if (game != s.heldGame) { // the game's lights of this part changed while held: a change not shown
            if (s.heldGame) g_rigEpisodeChanges++;
            s.heldGame = game;
        }
        if (g_rigEpisodeParts.size() < 65536) g_rigEpisodeParts.insert(key);
        g_rigEpisodeLastHeld = now;
    } else if (static_cast<int32_t>(s.tick - lastSwitch) >= 0 || lastSwitch - s.tick > kRigSnapFreshMs + 4000)
        return; // taken before the latest switch (the hold covers every switch since its first), and not long before it
    if (rig) {
        std::memcpy(h.psGame, rig, sizeof h.psGame);
        SetPsConst(dev, 0, &snapPs[0][0], 8);
        std::memcpy(rig, snapPs, sizeof s.ps);
        h.ps = true;
    } else if (!snapHaveVs) return; // (nothing of it to hold)
    if (snapHaveVs && vl >= 4 && SUCCEEDED(dev->GetVertexShaderConstantF(static_cast<UINT>(vl - 4), &h.vsGame[0][0], 8))) {
        SetVsConst(dev, static_cast<UINT>(vl - 4), &snapVs[0][0], 8);
        h.vs = true;
        h.vl = vl;
    }
    g_rigHeldDraws.fetch_add(1, std::memory_order_relaxed);
}
void ReleaseRig(IDirect3DDevice9* dev, const RigHeld& h) {
    if (h.ps) SetPsConst(dev, 0, &h.psGame[0][0], 8);
    if (h.vs) SetVsConst(dev, static_cast<UINT>(h.vl - 4), &h.vsGame[0][0], 8);
}

// Room-mode furniture draws (RigTracker mode 0). The room counts as dark (Rooms at Night acts fully, whatever the night
// level) when the rig holds a [NoLight] light: the game adds those only to a dark room.
template <typename DrawFn> D3D9Hooks::HookAction OnDrawInner(IDirect3DDevice9* dev, DrawFn draw) {
    if (!g_curVsIsObject || !g_curPs || g_inOwnCall) return OnDrawInnerCore(dev, draw);
    if (RigMode() != 0) return OnDrawInnerCore(dev, draw);
    if (!kPublicBuild) g_fdMode0.fetch_add(1, std::memory_order_relaxed);
    const ShaderPatches::RigPsInfo& info = RigPsInfoFor(g_curPs);
    if (!kPublicBuild)
        if (!info.rigLights) g_fdNoChain.fetch_add(1, std::memory_order_relaxed);
    float rig[8][4] = {};
    const bool haveRig = info.rigLights && SUCCEEDED(dev->GetPixelShaderConstantF(0, &rig[0][0], 8));
    RigHeld held;
    if (haveRig) HoldRig(dev, rig, held); // a lamp switch's furniture: the lights from before it until its room shows the new light
    // a part whose shader has no rig chain is lit by its vertex lights alone (about a quarter of the room-mode draws): those
    // are held the same way, or the part led its room by up to a third of a second
    else if (!info.rigLights && g_curVsInfo && g_curVsInfo->patched.vertexLight >= 4) HoldRig(dev, nullptr, held);
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
    float chroma[3];
    const bool hued = active && !dark && RoomAmbientChroma(ObjectLightBridge::RigRoom(RigTracker::CurrentRig()), chroma);
    if (active) r = OnDrawFurniture(dev, draw, info, haveRig ? rig : nullptr, hued ? chroma : nullptr);
    else {
        if (!kPublicBuild) g_fdInactive.fetch_add(1, std::memory_order_relaxed);
        r = OnDrawInnerCore(dev, draw);
    }
    if (trace) {
        const int path = static_cast<long>(g_indoorDrawn.load()) != aBefore ? 1 : g_nightFurniture.load() != bBefore ? 2 : 0;
        TraceFurniture(dev, haveRig ? rig : nullptr, info.rigLights, path, dark, active, tint, cubeGame, cubeGame >= 0.0f ? cubeGame * ambient : cubeGame);
    }
    if (held.ps || held.vs) {
        if (r == D3D9Hooks::HookAction::Continue) { // the game's own draw, made here while its lights are held
            g_inOwnCall = true;
            draw();
            g_inOwnCall = false;
            r = D3D9Hooks::HookAction::Skip;
        }
        ReleaseRig(dev, held);
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
    if (DevSwitches::FoliageSunShadow()) NoteShadowPassDraw(dev); // (log only, one frame in 120)
    IDirect3DVertexShader9* foliage = g_curVsIsFoliage && g_objectFix.load(std::memory_order_relaxed) ? FoliageVsFor(dev, g_curVs) : nullptr;
    if (!foliage) return OnDrawInner(dev, draw);
    IDirect3DVertexShader9* original = g_curVs;
    // The wrap constants (VS c255, PatchFoliageVs runtimeWrap) carry the daytime curve (13/10): the per-instance lamp colours
    // c54..c56 (the rig, boosted at the night strength) were drawn at full strength by day; now x LampDay(night), 3% by day
    // and exactly the earlier defined constants at night. Restored after the draw (another shader may read c255).
    // (2.13.1) With the rigs' outdoor lamps on the reach law (ObjectReachLawScale, ObjectLightBridge ReachLawRec) the weight is
    // the plain Lambert N.L, as on every per-pixel lamp: leaves turned away from a lamp take none of it (the moon's sky term
    // stays), and the colours already carry the daytime curve (TerrainLightingPolicy::FoliageLampWeights).
    const bool reachLaw = LotLightBridge::ObjectReachLawScale() >= 0.0f;
    float weights[2];
    TerrainLightingPolicy::FoliageLampWeights(g_night.load(std::memory_order_relaxed), reachLaw, TesterToggles::foliageDayFix.load(std::memory_order_relaxed), weights);
    const bool dayScaled = !reachLaw && weights[0] < 1.0f / 1.5f; // the daytime curve scaled the wrap (status)
    const float wrap[4] = {weights[0], weights[1], 0.0f, 0.0f};
    if (reachLaw) g_foliageReachLaw.fetch_add(1, std::memory_order_relaxed);
    float oldWrap[4] = {};
    const bool wrapSaved = SUCCEEDED(dev->GetVertexShaderConstantF(ShaderPatches::kFoliageWrapConst, oldWrap, 1));
    // Development builds: per-vertex lamps (foliage_vertex_lamps) and the crowns for the ground shade (canopy_ground_shadow),
    // at night on reach-law rigs. A draw whose lamps cannot all be matched keeps the copy above.
    IDirect3DVertexShader9* drawVs = foliage;
    static std::vector<float> s_pvSaved(82 * 4);
    static float s_colSaved[27][4]; // the game's c54..c80 (lamp colours), put back after a per-vertex draw
    int pvBase = -1; // set: c[pvBase .. +81] were replaced for this draw and are put back after it
    if ((DevSwitches::FoliageVertexLamps() || DevSwitches::CanopyGroundShadow()) && reachLaw && g_night.load(std::memory_order_relaxed) > 0.01f) {
        int base = -1;
        IDirect3DVertexShader9* perVertex = DevSwitches::FoliageVertexLamps() ? FoliageVertexVsFor(dev, g_curVs, base) : nullptr;
        static std::vector<float> s_pv;
        const bool want = perVertex && base > 0;
        const bool matched = HookGuard::Run("Foliage per-vertex lamps", false, [&] { return FoliageVertexConstants(dev, want, s_pv); });
        if (want && matched && SUCCEEDED(dev->GetVertexShaderConstantF(static_cast<UINT>(base), s_pvSaved.data(), 82)) &&
            SUCCEEDED(dev->GetVertexShaderConstantF(54, &s_colSaved[0][0], 27))) {
            g_inOwnCall = true;
            SetVsConst(dev, static_cast<UINT>(base), s_pv.data(), 82);
            SetVsConst(dev, 54, s_pv.data() + 82 * 4, 27); // the instances' own lamp colours
            g_inOwnCall = false;
            pvBase = base;
            drawVs = perVertex;
            g_foliageVertexDrawn.fetch_add(1, std::memory_order_relaxed);
        } else if (perVertex) {
            g_foliageVertexRefused.fetch_add(1, std::memory_order_relaxed);
        }
        static DWORD s_logged = 0;
        if (const DWORD now = GetTickCount(); now - s_logged > 30000) {
            s_logged = now;
            LOG_INFO(std::format("[Foliage] Per-vertex lamps: {} draws, {} kept the old path; instances by their own lamps: {} with none, {} with 1, {} with 2, {} with 3 "
                                 "({} without a crown shape); lamps left out by walls: {}; crowns for the ground shade: {}; camera drift corrected: up to {:.2f} m (places learned {}); tree draws with the sun shadow: {}; picks made again: {} changed the light, {} kept it",
                                 g_foliageVertexDrawn.load(), g_foliageVertexRefused.load(), g_foliageOwned[0].load(), g_foliageOwned[1].load(), g_foliageOwned[2].load(),
                                 g_foliageOwned[3].load(), g_foliageNoShape.load(), g_foliageWallOut.load(), g_crownCount.load(), g_eyeDriftMax.exchange(0.0f), g_eyeAnchors.Count(), g_treeShadowDrawn.load(), g_foliageRepickChanged.load(), g_foliageRepickSame.load()));
        }
    }
    g_inOwnCall = true;
    SetVs(dev, drawVs);
    SetVsConst(dev, ShaderPatches::kFoliageWrapConst, wrap, 1);
    g_inOwnCall = false;
    if (dayScaled) g_foliageDayScaled.fetch_add(1, std::memory_order_relaxed);
    if (OnDrawInner(dev, draw) == D3D9Hooks::HookAction::Continue) {
        g_inOwnCall = true;
        draw();
        g_inOwnCall = false;
    }
    g_inOwnCall = true;
    SetVs(dev, original);
    if (wrapSaved) SetVsConst(dev, ShaderPatches::kFoliageWrapConst, oldWrap, 1);
    if (pvBase > 0) {
        SetVsConst(dev, static_cast<UINT>(pvBase), s_pvSaved.data(), 82); // the game's values of c[base .. base+81] again
        SetVsConst(dev, 54, &s_colSaved[0][0], 27);                       // and of its lamp colours
    }
    g_inOwnCall = false;
    g_foliageDrawn.fetch_add(1, std::memory_order_relaxed);
    MarkClaimSource(ClaimSource::FoliageVertex);
    return D3D9Hooks::HookAction::Skip;
}

// ---- Census and false colour (development build). A draw is a "lamp-lit candidate" when it binds a baked light map
// (A8R8G8B8 managed, one level, up to 1024: room / wall / floor / lot maps; or a 256x256 DXT5 terrain map) or is drawn
// with an outdoor rig (RigTracker mode 2). Candidates no fix claimed (OnDrawTracked returned Continue) are painted
// magenta in false-colour mode, and the census records them per (VS, PS) pair for ApexRadiance_Censo.txt. ----
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

// Lighting + Bloom census: development-only, read-only diagnostic. Unlike the legacy census (shader-pair coverage),
// this also keeps a coarse world position and the day/twilight/night split so we can see which Apex paths still claim
// draws in daylight, and which known scene families can feed the game's bloom mask.
struct LightingBloomRow {
    int draws = 0, claimed = 0;
    int dayDraws = 0, dayClaimed = 0;
    int twilightDraws = 0, twilightClaimed = 0;
    int nightDraws = 0, nightClaimed = 0;
    UINT prims = 0;
    int rig = -1;
    int bloom = 0; // 0=no known bloom-mask family, 1=known (walls/objects/roofs), 2=possible (instanced structures)
    float minNight = 1.0f, maxNight = 0.0f;
    bool havePos = false;
    float x = 0, y = 0, z = 0;
    std::string path;
    std::string tex;
    std::array<int, kClaimSourceCount> sourceTotal{};
    std::array<int, kClaimSourceCount> sourceDay{};
    std::array<int, kClaimSourceCount> sourceTwilight{};
    std::array<int, kClaimSourceCount> sourceNight{};
    int unknownSourceTotal = 0;
    int unknownSourceDay = 0;
    int unknownSourceTwilight = 0;
    int unknownSourceNight = 0;
};
std::atomic<int> g_lightingBloomFrames{0};
bool g_lightingBloomPending = false;
using LightingBloomKey = std::tuple<IDirect3DVertexShader9*, IDirect3DPixelShader9*, int, int>;
std::map<LightingBloomKey, LightingBloomRow> g_lightingBloom;

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

const char* LightingBloomPath(int rig) {
    if (g_curClass == PsClass::ObjectRig) return "ObjectRig";
    if (g_curClass == PsClass::Roof) return "Roof";
    if (g_curClass == PsClass::RoofSnow) return "RoofSnow";
    if (g_curClass == PsClass::Lake) return "Lake";
    if (g_curClass == PsClass::WallGain) return "ExteriorWall";
    if (g_curVsIsFoliage) return "Foliage";
    if (g_curVsIsRoad) return "Road";
    if (g_curVsIsFloor) return "Floor";
    if (g_curClass == PsClass::FloorAtlas && !g_curVsIsSnowFloor) return "OutdoorFloor";
    if (g_curVsIsInstanced) return "Fence/Stair/Instanced";
    if (g_curVsIsSnowCover) return "SnowOnObject";
    if (g_curVsIsSnowRelief) return "SnowOnStair";
    if (g_curVsIsObject) return rig == 0 ? "IndoorObject" : "OutdoorObject";
    if (g_curClass == PsClass::LotLightSnow || g_curClass == PsClass::LotLightSnowCut) return "SnowLot";
    if (g_curClass == PsClass::WorldCandidate || g_curClass == PsClass::WorldMultiLight) return "WorldCandidate";
    if (g_curClass == PsClass::LotLight) return "LotLight";
    return "Other";
}

int LightingBloomFamily() {
    // docs/engine/shaders.md: walls, objects and roofs write the game's bloom mask in output alpha.
    if (g_curClass == PsClass::WallGain || g_curClass == PsClass::Roof || g_curClass == PsClass::RoofSnow || g_curClass == PsClass::ObjectRig || g_curVsIsObject) return 1;
    // Instanced structures use object-like shader families; keep them visible as "possible" until their alpha path is
    // captured directly instead of claiming that every variant definitely writes the same mask.
    if (g_curVsIsInstanced) return 2;
    return 0;
}

bool LightingBloomPosition(IDirect3DDevice9* dev, float& x, float& y, float& z) {
    float m[3][4] = {};
    int k = 8;
    if (g_curVsIsObject && g_curVsInfo && g_curVsInfo->patched.worldK >= 0) k = g_curVsInfo->patched.worldK;
    if (FAILED(dev->GetVertexShaderConstantF(static_cast<UINT>(k), &m[0][0], 3))) return false;
    x = m[0][3];
    y = m[1][3];
    z = m[2][3];
    return std::isfinite(x) && std::isfinite(y) && std::isfinite(z) && std::fabs(x) < 1e7f && std::fabs(y) < 1e7f && std::fabs(z) < 1e7f;
}

// ---- Exterior surface capture (developer, read-only): how every visible outdoor wall, fence, half wall, stair, floor,
// ground and roof draw receives its light. One row per shader pair and 0.5 m position cell; the first draw of a row also
// keeps its bound textures, render states and non-zero shader constants. ----
struct ExteriorRow {
    std::string path, psClass, tex, consts, states;
    int draws = 0, claimed = 0, rig = 0, prims = 0;
    uint32_t sources = 0;
    float x = 0, y = 0, z = 0, night = 0;
    bool havePos = false, lit = false;
};
std::atomic<int> g_exteriorFrames{0};
bool g_exteriorPending = false;
using ExteriorKey = std::tuple<IDirect3DVertexShader9*, IDirect3DPixelShader9*, int, int, int>;
std::map<ExteriorKey, ExteriorRow> g_exterior;

const char* PsClassName(PsClass c) {
    static constexpr const char* n[] = {"Unknown", "Other", "WorldCandidate", "WorldMultiLight", "WorldCompact", "LotLight", "ObjectRig", "Roof", "Lake", "Sea",
                                        "LakeWeather", "LotLightSnow", "LotLightSnowCut", "LotLightMelt", "RoofSnow", "WallGain", "FloorAtlas"};
    const size_t i = static_cast<size_t>(c);
    return i < std::size(n) ? n[i] : "?";
}

// The outdoor structure/ground families; "Other"/"IndoorObject" draws that bind a light map are kept as Unclassified
// because unpatched wall variants (ExteriorWallAOSI, UnlitExteriorWall, ...) land there (docs/engine/shaders.md).
const char* ExteriorPath(const char* path, bool litCandidate) {
    static constexpr const char* keep[] = {"ExteriorWall", "Road", "Floor", "OutdoorFloor", "Fence/Stair/Instanced", "SnowOnStair", "SnowLot", "LotLight", "WorldCandidate", "Roof", "RoofSnow"};
    for (const char* k : keep)
        if (std::strcmp(path, k) == 0) return path;
    if (litCandidate && (std::strcmp(path, "Other") == 0 || std::strcmp(path, "IndoorObject") == 0)) return "Unclassified(lit)";
    return nullptr;
}

std::string DescribeAllTextures(IDirect3DDevice9* dev) {
    std::string s;
    for (DWORD st = 0; st < 16; st++) {
        IDirect3DBaseTexture9* b = nullptr;
        if (FAILED(dev->GetTexture(st, &b)) || !b) continue;
        D3DSURFACE_DESC d{};
        if (b->GetType() == D3DRTYPE_TEXTURE && SUCCEEDED(static_cast<IDirect3DTexture9*>(b)->GetLevelDesc(0, &d))) {
            const DWORD levels = b->GetLevelCount();
            const char* role = "";
            if (d.Format == D3DFMT_A8R8G8B8 && d.Pool != D3DPOOL_DEFAULT && levels == 1 && d.Width <= 1024 && d.Height <= 1024) role = "=LIGHTMAP";
            else if (d.Format == D3DFMT_DXT5 && d.Width == 256 && d.Height == 256 && levels <= 5) role = "=TERRAINMAP";
            s += std::format(" s{}:fmt{:X} {}x{} L{} P{}{}", st, static_cast<unsigned>(d.Format), d.Width, d.Height, levels, static_cast<int>(d.Pool), role);
        } else {
            s += std::format(" s{}:type{}", st, static_cast<int>(b->GetType()));
        }
        b->Release();
    }
    return s;
}

std::string DescribeConstants(IDirect3DDevice9* dev) {
    std::string s;
    float c[64][4] = {};
    auto dump = [&](const char* tag, int count, bool ps) {
        if (FAILED(ps ? dev->GetPixelShaderConstantF(0, &c[0][0], count) : dev->GetVertexShaderConstantF(0, &c[0][0], count))) return;
        for (int i = 0; i < count; i++) {
            if (c[i][0] == 0 && c[i][1] == 0 && c[i][2] == 0 && c[i][3] == 0) continue;
            s += std::format("\n      {}c{}=({:.9g},{:.9g},{:.9g},{:.9g})", tag, i, c[i][0], c[i][1], c[i][2], c[i][3]);
        }
    };
    dump("PS ", 32, true);
    dump("VS ", 64, false);
    return s;
}

std::string DescribeStates(IDirect3DDevice9* dev) {
    DWORD v[6] = {};
    const D3DRENDERSTATETYPE rs[6] = {D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND, D3DRS_DESTBLEND, D3DRS_ZWRITEENABLE, D3DRS_CULLMODE, D3DRS_ALPHATESTENABLE};
    for (int i = 0; i < 6; i++) dev->GetRenderState(rs[i], &v[i]);
    return std::format("blend={} src={} dst={} zwrite={} cull={} atest={}", v[0], v[1], v[2], v[3], v[4], v[5]);
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
    const bool fc = g_falseColor.load(std::memory_order_relaxed);
    const bool census = g_censusFrames.load(std::memory_order_relaxed) > 0;
    const bool lightingBloom = g_lightingBloomFrames.load(std::memory_order_relaxed) > 0;
    const bool exterior = g_exteriorFrames.load(std::memory_order_relaxed) > 0;
    if (kPublicBuild || (!fc && !census && !lightingBloom && !exterior) || g_inOwnCall) return OnDrawTracked(dev, draw);

    std::string desc;
    const int rig = RigMode();
    const bool litCandidate = LitCandidate(dev, desc) || rig == 2;
    const int bloomFamily = lightingBloom ? LightingBloomFamily() : 0;
    // Exterior capture: classify and read the state before the draw, since a handler may change it while re-issuing
    const char* exteriorPath = exterior ? ExteriorPath(LightingBloomPath(rig), litCandidate) : nullptr;
    ExteriorRow* exRow = nullptr;
    if (exteriorPath) {
        float x = 0, y = 0, z = 0;
        const bool havePos = LightingBloomPosition(dev, x, y, z);
        const ExteriorKey key{g_curVs, g_curPs, havePos ? static_cast<int>(std::lround(x * 2.0f)) : 0, havePos ? static_cast<int>(std::lround(y * 2.0f)) : 0,
                              havePos ? static_cast<int>(std::lround(z * 2.0f)) : 0};
        auto [it, added] = g_exterior.try_emplace(key);
        exRow = &it->second;
        if (added) {
            exRow->path = exteriorPath;
            exRow->psClass = PsClassName(g_curClass);
            exRow->rig = rig;
            exRow->lit = litCandidate;
            exRow->havePos = havePos;
            exRow->x = x, exRow->y = y, exRow->z = z;
            exRow->night = g_night.load(std::memory_order_relaxed);
            exRow->tex = DescribeAllTextures(dev);
            exRow->consts = DescribeConstants(dev);
            exRow->states = DescribeStates(dev);
        }
    }
    const bool candidate = litCandidate || bloomFamily != 0 || exRow;
    g_claimSourceMask = 0;
    const D3D9Hooks::HookAction r = OnDrawTracked(dev, draw);
    if (exRow) {
        exRow->draws++;
        exRow->claimed += r == D3D9Hooks::HookAction::Skip ? 1 : 0;
        exRow->prims += g_curPrims;
        exRow->sources |= g_claimSourceMask;
    }
    if (!candidate) return r;
    const bool claimed = r == D3D9Hooks::HookAction::Skip;

    if (census && litCandidate) {
        CensusRow& row = g_census[{g_curVs, g_curPs}];
        row.draws++;
        row.claimed += claimed ? 1 : 0;
        row.prims += g_curPrims;
        row.rig = rig;
        if (row.tex.empty()) row.tex = desc.empty() ? " (so rig)" : desc;
    }

    if (lightingBloom) {
        float x = 0, y = 0, z = 0;
        const bool havePos = LightingBloomPosition(dev, x, y, z);
        const int qx = havePos ? static_cast<int>(std::lround(x * 2.0f)) : 0;
        const int qz = havePos ? static_cast<int>(std::lround(z * 2.0f)) : 0;
        LightingBloomRow& row = g_lightingBloom[{g_curVs, g_curPs, qx, qz}];
        row.draws++;
        row.claimed += claimed ? 1 : 0;
        row.prims += g_curPrims;
        row.rig = rig;
        row.bloom = std::max(row.bloom, bloomFamily);
        const float night = g_night.load(std::memory_order_relaxed);
        row.minNight = std::min(row.minNight, night);
        row.maxNight = std::max(row.maxNight, night);
        if (night <= 0.01f) {
            row.dayDraws++;
            row.dayClaimed += claimed ? 1 : 0;
        } else if (night >= 0.99f) {
            row.nightDraws++;
            row.nightClaimed += claimed ? 1 : 0;
        } else {
            row.twilightDraws++;
            row.twilightClaimed += claimed ? 1 : 0;
        }
        if (claimed) {
            bool attributed = false;
            for (size_t i = 0; i < kClaimSourceCount; i++) {
                if (!(g_claimSourceMask & (1u << static_cast<unsigned>(i)))) continue;
                attributed = true;
                row.sourceTotal[i]++;
                if (night <= 0.01f) row.sourceDay[i]++;
                else if (night >= 0.99f) row.sourceNight[i]++;
                else row.sourceTwilight[i]++;
            }
            if (!attributed) {
                row.unknownSourceTotal++;
                if (night <= 0.01f) row.unknownSourceDay++;
                else if (night >= 0.99f) row.unknownSourceNight++;
                else row.unknownSourceTwilight++;
            }
        }
        if (!row.havePos && havePos) {
            row.havePos = true;
            row.x = x;
            row.y = y;
            row.z = z;
        }
        if (row.path.empty()) row.path = LightingBloomPath(rig);
        if (row.tex.empty()) row.tex = desc.empty() ? (rig == 2 ? " outdoor rig" : "") : desc;
    }

    if (claimed || !fc || !litCandidate || !g_curPs) return r;
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

void WriteLightingBloomCensus() {
    try {
        const std::filesystem::path base = std::filesystem::path(ApexPaths::ApexDirectory());
        std::ofstream out(base / L"ApexRadiance_LightingBloomCensus.txt", std::ios::trunc);
        out << APEX_PRODUCT_NAME " Lighting + Bloom Census (read-only diagnostic)\n"
               "Purpose: show visible draw families that Apex claims during day/twilight/night and known/possible game bloom-mask producers.\n"
               "Bloom labels are family-level diagnostics, not measured output alpha: YES = documented walls/objects/roofs; POSSIBLE = instanced structures pending direct alpha capture.\n"
               "Day <= 0.01, Twilight 0.01..0.99, Night >= 0.99 (g_night).\n"
               "claim sources are the exact Apex draw path(s) that acted; counts are total/day/twilight/night and one draw may have more than one source.\n\n"
               "columns: path | rig | bloom | draws/fixed | day/fixed | twilight/fixed | night/fixed | night range | triangles | position | VS hash/size | PS hash/size | claim sources total/day/twilight/night | textures\n\n";

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
        };

        int dayModifiedRows = 0, bloomRows = 0, totalDraws = 0, totalClaimed = 0, unknownSourceRows = 0;
        for (const auto& [key, row] : g_lightingBloom) {
            IDirect3DVertexShader9* vs = std::get<0>(key);
            IDirect3DPixelShader9* ps = std::get<1>(key);
            uint32_t vh = 0, ph = 0;
            UINT vsz = 0, psz = 0;
            code(vs, vh, vsz);
            code(ps, ph, psz);
            totalDraws += row.draws;
            totalClaimed += row.claimed;
            dayModifiedRows += row.dayClaimed > 0 ? 1 : 0;
            bloomRows += row.bloom != 0 ? 1 : 0;
            const char* bloom = row.bloom == 1 ? "YES" : row.bloom == 2 ? "POSSIBLE" : "no";
            const std::string pos = row.havePos ? std::format("({:.1f},{:.1f},{:.1f})", row.x, row.y, row.z) : "(unknown)";
            std::string sources;
            for (size_t i = 0; i < kClaimSourceCount; i++) {
                if (!row.sourceTotal[i]) continue;
                if (!sources.empty()) sources += ",";
                sources += std::format("{}={}/{}/{}/{}", kClaimSourceNames[i], row.sourceTotal[i], row.sourceDay[i], row.sourceTwilight[i], row.sourceNight[i]);
            }
            if (row.unknownSourceTotal) {
                if (!sources.empty()) sources += ",";
                sources += std::format("UNKNOWN={}/{}/{}/{}", row.unknownSourceTotal, row.unknownSourceDay, row.unknownSourceTwilight, row.unknownSourceNight);
                unknownSourceRows++;
            }
            if (sources.empty()) sources = "-";
            out << std::format("{:<22} | {:2} | {:8} | {:4}/{:<4} | {:4}/{:<4} | {:4}/{:<4} | {:4}/{:<4} | {:.3f}..{:.3f} | {:7} | {:>22} | VS {:08X}/{} | PS {:08X}/{} | {} |{}\n",
                               row.path, row.rig, bloom, row.draws, row.claimed, row.dayDraws, row.dayClaimed, row.twilightDraws, row.twilightClaimed,
                               row.nightDraws, row.nightClaimed, row.minNight, row.maxNight, row.prims, pos, vh, vsz, ph, psz, sources, row.tex);
        }
        out << std::format("\nSUMMARY: {} rows, {} draws, {} Apex-claimed draws, {} rows modified in full daylight, {} bloom-family rows, {} rows with an unattributed Apex claim.\n",
                           g_lightingBloom.size(), totalDraws, totalClaimed, dayModifiedRows, bloomRows, unknownSourceRows);
        out << "Interpretation: rows with day/fixed > 0 are the first candidates for daylight-lighting review. claim sources identify the Apex path that actually acted; Bloom=YES/POSSIBLE identifies families worth a direct alpha/composite probe next.\n";
        LOG_INFO(std::format("[LotLightBridge] Lighting + Bloom census written: {} rows, {} daylight-modified, {} unattributed", g_lightingBloom.size(), dayModifiedRows, unknownSourceRows));
    } catch (...) {
    }
    g_lightingBloom.clear();
}

void WriteExteriorCapture() {
    try {
        const std::filesystem::path base = std::filesystem::path(ApexPaths::ApexDirectory());
        const std::filesystem::path dir = base / L"ExteriorSurfaces";
        std::filesystem::create_directories(dir);
        std::ofstream out(base / L"ApexRadiance_ExteriorSurfaces.txt", std::ios::trunc);
        out << APEX_PRODUCT_NAME " Exterior Surface Capture (read-only diagnostic, 3 frames)\n"
               "One row per VS/PS pair and 0.5 m cell of the position read from VS c8..c10 (or the object's world constant); for wall,\n"
               "floor and terrain batches that position may be a batch origin, not a single surface.\n"
               "fixed = draws Apex re-issued (Skip); sources = the Apex paths that acted; textures: fmt hex D3DFORMAT, L levels, P pool,\n"
               "LIGHTMAP = A8R8G8B8 1-mip lot/room/wall map, TERRAINMAP = DXT5 256x256 terrain light map. Constants = first draw of the row.\n"
               "Unclassified(lit) = a light-mapped draw no Apex class recognises (unpatched wall variants; may include interior surfaces).\n"
               "For the CPU-side lights per lot/story, also save a Lighting snapshot (Light Diag) at the same moment.\n\n";
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
        std::map<std::string, std::vector<const std::pair<const ExteriorKey, ExteriorRow>*>> byPath;
        for (const auto& item : g_exterior) byPath[item.second.path].push_back(&item);
        out << "SUMMARY (rows / draws / fixed per family):\n";
        for (const auto& [path, rows] : byPath) {
            int d = 0, f = 0;
            for (const auto* r : rows) d += r->second.draws, f += r->second.claimed;
            out << std::format("  {:<22} {:5} rows {:6} draws {:6} fixed\n", path, rows.size(), d, f);
        }
        std::map<uint32_t, bool> saved;
        int files = 0;
        for (const auto& [path, rows] : byPath) {
            out << std::format("\n==== {} ====\n", path);
            for (const auto* item : rows) {
                const ExteriorRow& row = item->second;
                uint32_t vh, ph;
                UINT vsz, psz;
                const auto vs = code(std::get<0>(item->first), vh, vsz);
                const auto ps = code(std::get<1>(item->first), ph, psz);
                std::string sources;
                for (size_t i = 0; i < kClaimSourceCount; i++)
                    if (row.sources & (1u << static_cast<unsigned>(i))) sources += std::string(sources.empty() ? "" : ",") + kClaimSourceNames[i];
                if (sources.empty()) sources = row.claimed ? "UNKNOWN" : "game only";
                const std::string pos = row.havePos ? std::format("({:.2f},{:.2f},{:.2f})", row.x, row.y, row.z) : "(unknown)";
                out << std::format("  VS {:08X}/{} PS {:08X}/{} class={} rig={} lit={} night={:.3f} pos={} draws={} fixed={} tris={} sources={}\n    {}\n    textures:{}",
                                   vh, vsz, ph, psz, row.psClass, row.rig, row.lit ? 1 : 0, row.night, pos, row.draws, row.claimed, row.prims, sources, row.states, row.tex);
                out << "\n    constants:" << row.consts << "\n";
                for (auto [h, sz, t, tag] : {std::tuple{vh, vsz, &vs, "VS"}, std::tuple{ph, psz, &ps, "PS"}}) {
                    if (t->empty() || files >= 300 || !saved.try_emplace(h, true).second) continue;
                    std::ofstream(dir / std::format("{}_{:08X}.bin", tag, h), std::ios::binary).write(reinterpret_cast<const char*>(t->data()), sz);
                    files++;
                }
            }
        }
        out << std::format("\n{} rows, {} shader files in Apex Radiance\\ExteriorSurfaces\n", g_exterior.size(), files);
        LOG_INFO(std::format("[LotLightBridge] Exterior surface capture written: {} rows, {} shader files", g_exterior.size(), files));
    } catch (...) {
    }
    g_exterior.clear();
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

// A C++ exception inside a hook would unwind into the game: turn the whole bridge off instead (review 25/09). Called from catch
// blocks, so it only sets flags (07/10, players' Runtime Error: an allocation there, perhaps with no memory left, could throw
// again and end the game); the status and the log line follow at the next Present (ReportHookFailure).
void HookFailed() noexcept {
    g_hookFailed = true;
}

// Present: the status and the log line of a HookFailed, once
void ReportHookFailure() {
    static std::atomic<bool> reported{false};
    if (!g_hookFailed || reported.exchange(true)) return;
    g_status = "Off after an internal error (see ApexRadiance_LOG.txt)";
    LOG_ERROR("[LotLightBridge] Exception inside the draw hook: fixes off until the game restarts");
}

void UpdateHooks() {
    static std::mutex m; // Install runs off the render thread at startup while Present may call the setters
    std::lock_guard<std::mutex> lock(m);
    const bool on = !g_hookFailed && (g_enabled.load() || g_objectFix.load() || g_roofFix.load() || g_waterFix.load() || g_wallEnabled.load());
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
                bool drawn = false; // the game's draw made by OnDraw: after an exception it is not made a second time (07/10)
                DrawRigScope rigScope;
                try {
                    g_curPrims = prims;
                    g_curDipBase = static_cast<UINT>(bvi), g_curDipMin = minV, g_curDipNum = numV;
                    NoteSolidMesh(ctx.device,type,bvi,minV,numV,start,prims,g_curVsIsInstanced);
                    return OnDraw(ctx.device, [&]() {
                        drawn = true;
                        D3D9Hooks::ReissueScope reissue; // only the observers of re-issued draws run for it (d3d9_hooks.h)
                        ctx.device->DrawIndexedPrimitive(type, bvi, minV, numV, start, prims);
                    });
                } catch (...) {
                    g_inOwnCall = false;
                    HookFailed();
                    return drawn ? D3D9Hooks::HookAction::Skip : D3D9Hooks::HookAction::Continue;
                }
            });
        D3D9Hooks::RegisterDrawPrimitive(kHookName, [](D3D9Hooks::DeviceContext& ctx, D3DPRIMITIVETYPE type, UINT start, UINT prims) {
            if (g_hookFailed) return D3D9Hooks::HookAction::Continue;
            bool drawn = false; // as above
            DrawRigScope rigScope;
            try {
                g_curPrims = prims;
                g_curDipNum = 0;
                return OnDraw(ctx.device, [&]() {
                    drawn = true;
                    D3D9Hooks::ReissueScope reissue;
                    ctx.device->DrawPrimitive(type, start, prims);
                });
            } catch (...) {
                g_inOwnCall = false;
                HookFailed();
                return drawn ? D3D9Hooks::HookAction::Skip : D3D9Hooks::HookAction::Continue;
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
    g_rigRoomWaits.clear(); // (keyed by the old world's room addresses)
    // A direct lamp pool/memo belongs to this world too. Never illuminate new
    // geometry with old-world rows while waiting for a successful enumeration.
    g_allLamps.clear();
    g_allCones.clear();
    for (LampCone& c : g_lampCone) c = LampCone{};
    g_lampCount = g_lastLampCandidates = 0;
    std::memset(g_lampData, 0, sizeof g_lampData);
    g_lampMemoGen = 1;
    for (auto& memo : g_lampMemo) memo.gen = 0;
    g_enumLights.clear();
    g_lotLampCur.clear();
    g_lotsNow.clear();
    g_lampFrame = 0;
    g_lampRefreshNow = true;
    g_lampSwitchPrev.clear();
    g_lotLampSig.clear(); // the next enumeration starts the new world's lots from scratch (all new: nothing counted)
    g_lampsAnimated = 0; // current-world classification count, not a cumulative event total
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
    g_shadeQueue.clear(); // the lamp shades waiting or building belong to the previous world's lots
    g_fenceGroups.clear(); // (garden walls of the old world)
    g_fenceBusy.clear();
    g_fenceGen++;
    g_meshBoxes.clear();
    g_fenceWalls = 0;
    // (14/10) the rest of the old world: the lamps leaving roofed rooms (rows, cones, lights), the lots' masks and shades
    // (their keys would stop the same lot ids being built again), the instanced groups' centres (keyed by buffer address)
    g_exitLampRows.clear();
    g_exitCones.clear();
    g_exitList.clear();
    for (auto& [lot, m] : g_lotMasks)
        if (m.tex) m.tex->Release();
    g_lotMasks.clear();
    g_groupCentres.clear();
    g_centreBusy.clear();
}

void SetSoftLotEdges(bool on) { g_softEdges = on; }

void SetIndoorSmooth(bool on) { g_indoorSmooth = on; }
void SetSmoothRoomMaps(bool on) { g_roomMapSmooth = on; }
std::string RoomMapSmoothStatus() {
    return std::format("B-spline reads {}: {} room-lit shaders, walls {} draws, floors {} draws | left to the game: caps {}, unknown vertex shader {}, wall blocks off the grid {}, "
                       "point filter {} | draws with the arch copy {}, with the copy without arches {}",
                       g_roomMapSmooth.load() ? "on" : "off", g_roomMapPs.size(), g_roomWallDrawn.load(), g_roomFloorDrawn.load(), g_roomCapsRefused.load(),
                       g_roomUnknownVs.load(), g_roomBandsOff.load(), g_roomPointFilter.load(), g_archCopyDraws.load(), g_archPlainDraws.load());
}

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

std::string LotShadeStatus() { return LotShadeStatusText(); }

int ChunkCount() { return static_cast<int>(g_chunks.size()); }

bool ObjectLampColourAt(const void* light, const float* point, float rgb[3], const float* ext) { return ObjectLampColourAtImpl(light, point, rgb, ext); }
float ObjectReachLawScale() {
    if (!g_objPixel.load(std::memory_order_relaxed) || !g_objPixelLamps.load(std::memory_order_relaxed) || !LevelLightShare::RealisticReachActive()) return -1.0f;
    return TerrainLightingPolicy::ReachLampScale(NativeWallScale(), g_night.load(std::memory_order_relaxed), g_wallEnabled.load(std::memory_order_relaxed),
                                                 g_wallGain.load(std::memory_order_relaxed), g_objPixelLampStrength.load(std::memory_order_relaxed) / kPixelLampTrimUnit);
}
void SetObjectPixelLamps(bool on, float strength) {
    g_objPixel = on;
    g_objPixelStrength = strength;
}

void SetObjectPixelLights(bool on, float strength) {
    g_objPixelLamps = on;
    g_objPixelLampStrength = strength;
}

void SetWindowOutdoor(bool on) { g_windowOutdoor = on; }

std::string WindowOutdoorStatus() {
    return std::format("{} | window parts lit outdoors: {} | left to the game (no basis uv rows): {}", g_windowOutdoor.load() ? "on" : "off", g_windowDrawn.load(), g_windowRefused.load());
}

void SetFenceGroundLight(bool on, float strength) {
    g_fenceFix = on;
    g_fenceStrength = strength;
}

void SetWallGain(float gain, bool enabled) {
    gain = gain < 0.25f ? 0.25f : (gain > 8.0f ? 8.0f : gain);
    const bool was = g_wallEnabled.load();
    g_wallGain = gain;
    g_wallEnabled = enabled;
    if (was != enabled) UpdateHooks();
}

std::string WallStatus() {
    return std::format("outside walls: strength {:.2f} | draws: {} | vanilla bloom alpha preserved: {} | fallback: {} | variants seen: {}",
                       g_wallGain.load(), g_wallDrawn.load(), g_wallBloomPreserved.load(), g_wallBloomFallback.load(), g_wallConst.size());
}

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

void RequestLightingBloomCensus() {
    if (g_lightingBloomPending) return;
    g_lightingBloom.clear();
    g_lightingBloomPending = true;
    g_lightingBloomFrames = 3;
}

std::string LightingBloomCensusStatus() { return g_lightingBloomPending ? "capturing..." : "ready"; }

void RequestExteriorCapture() {
    if (g_exteriorPending) return;
    g_exterior.clear();
    g_exteriorPending = true;
    g_exteriorFrames = 3;
}

std::string ExteriorCaptureStatus() { return g_exteriorPending ? "capturing..." : "ready"; }

void OnPresent() {
    if (DevSwitches::kCompiled) MeasureMemory(); // development builds (memory_owners)
    PumpSolidMeshes();
    ReleaseIdleMaskTextures(); // (left_lot_release, every 10 s)
    ++g_roomMapFrame; // (the room map size memo is per frame)
    g_lotDrawTick = GetTickCount();
    // the furniture's lamp edit episode is over once no switch, edit hold or trailing room remains (2.13.1: one log line)
    g_rigTrailing = TesterToggles::furnitureWithRoom.load(std::memory_order_relaxed) && AtriumHold::Trailing();
    if (g_rigEpisodeOn && !RigEpisodeWanted()) {
        g_rigEpisodeOn = false;
        if (!g_rigEpisodeParts.empty() || g_rigEpisodeLate > 0) {
            const std::string text = std::format("Lamp edit furniture: {} object parts kept their lights until their room showed the new light, shown with it {} ms after the edit "
                                                 "({} light changes of theirs not shown; {} parts had shown new lights before their room waited, left as they were; "
                                                 "{} draws held before their room was marked)",
                                                 g_rigEpisodeParts.size(), g_rigEpisodeLastHeld ? g_rigEpisodeLastHeld - g_rigEpisodeTick : 0, g_rigEpisodeChanges,
                                                 g_rigEpisodeLate, g_rigGraceHeld.exchange(0));
            LOG_INFO("[LotLightBridge] " + text);
            if (Recorder::Active()) Recorder::Note("[switch] " + text);
        }
        g_rigEpisodeParts.clear();
    }
    if (const DWORD now = GetTickCount(); now - g_rigSweepAt >= kRigSnapSweepMs) {
        g_rigSweepAt = now;
        if (!g_rigEpisodeOn && !RigEpisodeWanted())
            std::erase_if(g_rigSnaps, [now](const auto& kv) { return now - kv.second.tick > kRigSnapKeepMs; });
    }
    ReportHookFailure();
    // the lamp shade on lot ground follows "Walls shade lamp light near the ground" (14/10; the same wall copy as "Buildings
    // block lamp light on trees and objects"); a switch remakes every lot's texture (without or with its shade)
    if (const bool shadeOn = LevelLightShare::GroundShadeOn() && TesterToggles::groundShade.load(std::memory_order_relaxed); shadeOn != g_shadeOn) {
        g_shadeOn = shadeOn;
        for (auto& [lot, m] : g_lotMasks) {
            m.dirty = true;
            m.shadeKey = 0;
            m.queuedKey = 0;
            m.shadeAt = 0;
        }
        if (!shadeOn) {
            g_shadeQueue.clear();
        }
    }
    if (g_enabled.load(std::memory_order_relaxed)) ShadeTick();
    if (g_lotDrawSeen.size() > 1024) std::erase_if(g_lotDrawSeen, [](const auto& item) { return g_lotDrawTick - item.second > 10000; });
    if (g_censusPending && g_censusFrames.load() > 0 && --g_censusFrames == 0) {
        WriteCensus();
        g_censusPending = false;
    }
    if (g_lightingBloomPending && g_lightingBloomFrames.load() > 0 && --g_lightingBloomFrames == 0) {
        WriteLightingBloomCensus();
        g_lightingBloomPending = false;
    }
    if (g_exteriorPending && g_exteriorFrames.load() > 0 && --g_exteriorFrames == 0) {
        WriteExteriorCapture();
        g_exteriorPending = false;
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
    // a lamp edited: about every other frame; (14/10, drag solves) while a lamp is dragged 4 times a second (each read re-lights
    // every rig, window and fence lamp list), and the read after the last step still comes (the request stays until served)
    // (14/10, Classic: relight only after release) while dragged the list is still read at that pace, for the drag's own
    // steps (a type-11 light registered again), but the per-pixel lamps keep their list until the release (below)
    const bool dragHeld = DragLite::ClassicHolding();
    const bool editReady = g_lampEditRefresh && g_lotDrawTick - g_lampReadTick >= ((TesterToggles::dragSlowRefresh.load(std::memory_order_relaxed) || dragHeld) && DragLite::Dragging() ? 250u : 33u);
    // the switch the per-pixel lamps waited for is shown: their new list now (the rooms' maps were written this frame too)
    if (g_lampsHeldForSwitch && !AtriumHold::SwitchHolding() && !dragHeld) {
        g_lampsHeldForSwitch = false;
        g_lampRefreshNow = true;
    }
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
    // a lamp switch shown all at once (AtriumHold): the per-pixel lamps (roofs, water, outdoor objects, trees) keep their
    // list until the switch is shown; the lamp changes are still tracked (the ground's relight starts from them)
    const bool held = AtriumHold::SwitchHolding() || dragHeld; // (a classic drag: until the release)
    if (held && wantLamps && enumerated) g_lampsHeldForSwitch = true;
    ReadEnumeratedLamps(wantLamps && enumerated && !held);
    if (wantLamps && enumerated) ReadFoliageLamps(!held); // (the trees never wait on a hold)
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
    s += std::format(" | buildings block light: objects and windows {} lamps left out, {} dimmed at a slab's edge; fences {} left out, {} dimmed; "
                     "ground light faded above the lamp heads: {} draws",
                     g_objLampsLeftOut, g_objLampsDimmed, g_fenceLampsLeftOut, g_fenceLampsDimmed, g_objGroundGated);
    s += std::format(" | lamps by the outdoor walls' law (realistic reach): objects {}, fences {}, windows {} draws; first law kept (no room in the shader) {}",
                     g_reachObjDraws, g_reachFenceDraws, g_reachWindowDraws, g_reachLawRefused);
    s += std::format(" | ground lamp shade on floors: {} draws with their lot, {} without it (as before); on fences/stairs: {} draws with their lot, {} without it",
                     g_floorShadeLot.load(), g_floorShadeNoLot.load(), g_fenceShadeLot.load(), g_fenceShadeNoLot.load());
    s += std::format(" | daytime curve: outdoor lamps x {:.3f} now; object draws with the game's rig scaled {}",
                     TerrainLightingPolicy::LampDay(g_night.load(std::memory_order_relaxed)), g_objRigDayScaled);
    if (!kPublicBuild) s += std::format(" | leftover textures skipped when looking for chunk light maps: {}", g_chunkStraySkipped.load());
    return s;
}

std::string DescribeDraw() {
    // (outdoor lamps a room-mode rig took through its room's openings, ObjectLightBridge)
    const std::string openings = ObjectLightBridge::RigOpeningNote(RigTracker::CurrentRig());
    if (g_inOwnCall) return (g_objDrawInfo.empty() ? std::string("mod draw (another fix)") : g_objDrawInfo) + openings;
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
    return s + openings;
}

void FurnitureTraceReset() { g_traceLast.clear(); }
uint64_t FurnitureTraceRelease() {
    const uint64_t bytes = ApexMemory::Est::Hash(g_traceLast, [](const auto& kv) { return ApexMemory::Est::Vec(kv.second.states); });
    std::unordered_map<uint64_t, TraceSeen>().swap(g_traceLast);
    return bytes;
}

// Development build (F6 recorder): room-mode furniture draws since the last call (the render thread)
std::string FurnitureDiag() {
    static long last[8] = {};
    const long now[8] = {g_fdMode0.load(), g_fdInactive.load(), g_fdNoChain.load(), g_nightFurniture.load(), static_cast<long>(g_indoorDrawn.load()),
                         g_fdUnlitSlots.load(), g_fdLampSlots.load(), g_rigHeldDraws.load()};
    long d[8];
    for (int k = 0; k < 8; k++) {
        d[k] = now[k] - last[k];
        last[k] = now[k];
    }
    return std::format("room-mode draws {} (Rooms at Night not acting {}, no rig chain {}, turned {}; Apex indoor-object shader {}; held for a lamp switch {}) | "
                       "rig slots turned {}, lamps kept {} | acting {}, brightness x{:.3f}, blue kept {:.3f}", d[0], d[1], d[2], d[3], d[4], d[7], d[5], d[6],
                       UnlitRooms::FurnitureActive() ? "yes" : "no", UnlitRooms::FurnitureAmbient(), UnlitRooms::FurnitureTint());
}

std::string ObjectStatus() {
    return std::format("moon shadow on objects: {} | draws fixed: {} | cinema day bloom suppressed: {} (centre panel: {}) | foliage (wrap light): {} (by the daytime curve: {}, Lambert on reach-law rigs: {}) | winter foliage without shadow: {} | Rooms at Night on furniture: {} draws ({} with the blue tint in the game's shader)",
                       g_objectFix.load() ? (g_objectPs ? "fixed" : "waiting") : "off", g_objectDrawn.load(), g_cinemaDayBloomSuppressed.load(),
                       g_cinemaPanelDayBloomSuppressed.load(), g_foliageDrawn.load(), g_foliageDayScaled.load(), g_foliageReachLaw.load(), g_leafDrawn.load(), g_nightFurniture.load(), g_nightFurnitureTinted.load());
}

void Shutdown(bool keepChunkMaps) {
    MeshOcclusion::Reset();
    g_solidGroups.clear();g_solidGeometry.clear();g_solidAttempts.clear();g_solidBusy.clear();g_solidPs.clear();g_solidTriangles=0;g_solidDirty=0;g_solidNotes=0;g_solidNextScan=0;g_solidScanning=false;
    g_keepChunks = keepChunkMaps;
    g_objectFix = false;
    g_wallGain = 1.0f;
    g_wallEnabled = false;
    g_roofFix = false;
    g_waterFix = false;
    g_objPixel = false;
    g_fenceFix = false;
    SetEnabled(false); // nothing left on: UpdateHooks unregisters the D3D hooks before the shaders are released
    if (g_replacementPs) {
        g_replacementPs->Release();
        g_replacementPs = nullptr;
    }
    g_shadeWorker.Stop();
    ReleaseLotMasks();
    g_fenceGroups.clear();
    g_fenceBusy.clear();
    g_fenceGen++;
    g_meshBoxes.clear();
    g_fenceWalls = 0;
    if (g_snowPs) {
        g_snowPs->Release();
        g_snowPs = nullptr;
    }
    g_snowTried = false;
    if (g_snowCutPs) {
        g_snowCutPs->Release();
        g_snowCutPs = nullptr;
    }
    g_snowCutTried = false;
    if (g_meltPs) {
        g_meltPs->Release();
        g_meltPs = nullptr;
    }
    g_meltTried = false;
    for (auto* cache : {&g_terrainAlphaPs, &g_terrainDayPs, &g_terrainShadePs, &g_terrainDayShadePs, &g_roadShadePs, &g_roadPs, &g_floorPs, &g_snowFloorPs, &g_snowFloorPs0, &g_leafPs, &g_fencePs, &g_snowCoverPs, &g_snowReliefPs, &g_objLampPs, &g_basisSmoothPs, &g_roomMapSmoothPs, &g_basisRoomPs, &g_basisSmoothWallPs, &g_basisRoomWallPs, &g_basisSmoothFillPs, &g_cubeTintPs, &g_roomMapSmoothArchPs, &g_basisRoomArchPs, &g_basisRoomWallArchPs, &g_windowPs,
                        &g_objLampPsReach, &g_fencePsReach, &g_windowPsReach}) {
        for (auto& [k, p] : *cache)
            if (p.ps) p.ps->Release();
        cache->clear();
    }
    for (auto& [s, cache] : g_indoorPs)
        for (auto& [k, p] : cache)
            if (p.ps) p.ps->Release();
    g_indoorPs.clear();
    ReleaseWallMasks(); // (development builds, basis_wall_filter)
    g_wallMaskDev = nullptr;
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
    if (g_waterSeaPs) {
        g_waterSeaPs->Release();
        g_waterSeaPs = nullptr;
    }
    g_waterSeaCompileTried = false;
    if (g_waterWeatherPs) {
        g_waterWeatherPs->Release();
        g_waterWeatherPs = nullptr;
    }
    g_waterWeatherCompileTried = false;
    g_waterFix = false;
    g_roofCompileTried = false;
    g_roofFix = false;
    g_compileTried = false;
    if (g_replacementWetPs) {
        g_replacementWetPs->Release();
        g_replacementWetPs = nullptr;
    }
    g_wetCompileTried = false;
    g_wetLotPs.clear();
    g_classCache.clear();
    g_psFacts.clear();
    g_curFacts = nullptr;
    g_curFactsKnown = false;
    g_cinemaMarqueeDayPs.clear();
    g_cinemaMarqueePanelDayPs.clear();
    g_basisPs.clear();
    g_roomMapPs.clear();
    g_roomUvVs.clear();
    g_capsDev = nullptr;
    g_rigLookup.Clear();
    g_rigPsInfo.clear(); // shader addresses are reused after a restart of the feature (review M3)
    g_curPsBasis = false;
    g_curPsRoomMap = false;
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
    for (auto& [key, cache] : g_floorShadePs)
        for (auto& [k, p] : cache)
            if (p.ps) p.ps->Release();
    g_floorShadePs.clear();
    g_wallConst.clear();
    for (IUnknown* p : g_pinned) p->Release();
    g_pinned.clear();
    TrackPs(nullptr, true); // g_curPs = null, class Other
    TrackVs(nullptr, true); // g_curVs = null, no class
    g_stateUnknown = true;
    g_keepChunks = false;
}

} // namespace LotLightBridge

// ---- Development builds (memory_owners): this file's containers by owner (apex_memory.h), measured on the render thread
// (OnPresent) that owns them, at most every 10 s and after an F8. Only sizes and capacities are read. The Direct3D textures
// and shader copies themselves are counted where they are created (D3D9Hooks); here the CPU side. ----
namespace {
using namespace ApexMemory::Est;
void MeasureMemoryImpl() {
    using ApexMemory::Register;
    using ApexMemory::Set;
    constexpr const char* G = "LotLightBridge";
    static auto* sPatched = Register(G, "patched shader copies (records by game shader)");
    static auto* sShaderFacts = Register(G, "shader facts (pixel and vertex shader classes)");
    static auto* sLotMasks = Register(G, "g_lotMasks: lot room masks and lamp shades");
    static auto* sChunkShade = Register(G, "chunk shades (results waiting, queue)");
    static auto* sSolid = Register(G, "solid mesh groups (instanced triangles)");
    static auto* sFence = Register(G, "fence groups, walls and cells; mesh boxes");
    static auto* sFoliage = Register(G, "foliage meshes, instances and crowns");
    static auto* sWallMask = Register(G, "wall-mask textures (records)");
    static auto* sOther = Register(G, "lamps, lots, chunks and developer notes");
    uint64_t patched = 0;
    size_t patchedItems = 0;
    const auto ps = [&](const std::unordered_map<IDirect3DPixelShader9*, PatchedPs>& m) { patched += Hash(m), patchedItems += m.size(); };
    for (const auto* m : {&g_roadPs, &g_floorPs, &g_snowFloorPs, &g_snowFloorPs0, &g_leafPs, &g_fencePs, &g_snowCoverPs, &g_snowReliefPs, &g_objLampPs, &g_terrainAlphaPs,
                          &g_terrainDayPs, &g_terrainShadePs, &g_terrainDayShadePs, &g_roadShadePs, &g_objLampPsReach, &g_fencePsReach, &g_windowPs, &g_windowPsReach,
                          &g_basisSmoothPs, &g_roomMapSmoothPs, &g_basisRoomPs, &g_basisSmoothWallPs, &g_basisRoomWallPs, &g_basisSmoothFillPs, &g_cubeTintPs, &g_roomMapSmoothArchPs, &g_basisRoomArchPs, &g_basisRoomWallArchPs})
        ps(*m);
    for (const auto* t : {&g_floorShadePs, &g_indoorPs, &g_floorAtlasPs}) {
        patched += Tree(*t);
        for (const auto& [k, m] : *t) ps(m);
    }
    Set(sPatched, patched, patchedItems);
    Set(sShaderFacts,
        Hash(g_classCache) + Hash(g_basisPs) + Hash(g_psFacts) + Hash(g_roomMapPs) + Hash(g_worldSamplers) + Hash(g_wetLotPs) + Hash(g_vsInfo) + Hash(g_roomUvVs) +
            Hash(g_terrainLampConst) + Hash(g_rigPsInfo) + Hash(g_solidPs) + Hash(g_psIs3) + Hash(g_wallConst) + Hash(g_cinemaMarqueeDayPs) + Hash(g_cinemaMarqueePanelDayPs) + Vec(g_pinned),
        g_classCache.size() + g_psFacts.size() + g_vsInfo.size());
    Set(sLotMasks, Hash(g_lotMasks, [](const auto& kv) {
            const LotMask& m = kv.second;
            return Vec(m.room) + Vec(m.shade) + (m.ext ? Shared<ExtData>() + Vec(m.ext->v) + Vec(m.ext->w) : 0);
        }), g_lotMasks.size());
    Set(sChunkShade, Tree(g_chunkShades) + Tree(g_chunkReady, [](const auto& kv) { return Vec(kv.second.res) + Vec(kv.second.den); }) + Vec(g_shadeQueue),
        g_chunkShades.size() + g_chunkReady.size());
    Set(sSolid, Hash(g_solidGroups, [](const auto& kv) { return Vec(kv.second.triangles); }) + Hash(g_solidGeometry) + Hash(g_solidAttempts) + Hash(g_solidBusy),
        g_solidTriangles);
    Set(sFence,
        Hash(g_fenceGroups, [](const auto& kv) { return Vec(kv.second.walls); }) + Hash(g_fenceBusy) + Hash(g_meshBoxes) + Vec(g_fenceAll) +
            Hash(g_fenceCells, [](const auto& kv) { return Vec(kv.second); }) + Hash(g_groupCentres) + Hash(g_centreBusy),
        g_fenceGroups.size() + g_meshBoxes.size() + g_groupCentres.size());
    Set(sFoliage, Hash(g_foliageMeshes) + Hash(g_foliageInstances) + Vec(g_crowns), g_foliageMeshes.size() + g_foliageInstances.size() + g_crowns.size());
    Set(sWallMask, Hash(g_wallMaskTex), g_wallMaskTex.size());
    Set(sOther,
        Tree(g_chunks) + Hash(g_chunkOfTexture) + Vec(g_lotRects) + Hash(g_lotDrawSeen) + Hash(g_lotArrivals) + Vec(g_enumLights) + Vec(g_allLamps) + Vec(g_allCones) +
            Vec(g_exitLampRows) + Vec(g_exitCones) + Vec(g_exitList) + Vec(g_lotLampSig) + Vec(g_lotLampCur) + Vec(g_lotsNow) + Tree(g_lotSeen) + Tree(g_quietLogAt) +
            Vec(g_lastUserLots) + Hash(g_lampSwitchPrev) + Vec(g_disabledLots) + Hash(g_traceLast, [](const auto& kv) { return Vec(kv.second.states); }) + Hash(g_rigSnaps) +
            Hash(g_rigEpisodeParts) + Hash(g_rigRoomWaits) + Tree(g_census) + Tree(g_lightingBloom) + Tree(g_exterior) + Str(g_objDrawInfo),
        g_chunks.size() + g_allLamps.size() + g_census.size() + g_traceLast.size() + g_rigSnaps.size());
}
} // namespace
namespace LotLightBridge {
void MeasureMemory() {
    static ApexMemory::Cadence cadence;
    if (!cadence.Due()) return;
    try {
        MeasureMemoryImpl();
    } catch (...) {
    }
}
} // namespace LotLightBridge
