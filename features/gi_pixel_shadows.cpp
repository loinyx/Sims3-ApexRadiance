// Per-pixel delta shadows (see gi_pixel_shadows.h): the bridge pass of patch 0019, XR-05's filter and the compose.
//
// Part of Apex Radiance. Credits: @loinyx

#include "gi_pixel_shadows.h"
#include "realtime_gi.h"
#include "apex_log.h"
#include "build_flavor.h"
#include "d3d9_extra_hooks.h"
#include "render_callbacks.h"
#include "depth_share.h"
#include "post_scene.h"
#include "world_session.h"
#include "shader_cache.h"
#include "gbuffer_motion.h"
#include "gi_pixel_filter_hlsl.h"
#include "gi_sim_capsules.h"
#include "penumbra.h"
#include "mesh_caster_set.h"
#include "d3d9_hooks.h"
#include "sim_receiver_ids.h"
#include <cstddef>
#include "native/remix-bridge/gi/gi_pixel_block.h" // the bridge's block layout (patch 0019)
#include <d3d9.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <string>
#include <unordered_map>
#include <vector>
#include <windows.h>

namespace GiPixelShadows {
namespace {

constexpr int kOrder = PostScene::kAmbientOcclusion - 3; // after G-buffer motion (-5), before Ambient Occlusion
constexpr D3DFORMAT kFormat = D3DFMT_A16B16G16R16F;      // the bridge's outputs must be this
// Look constants (wp/XR-10.md). The lamp's ball is Shadow softness (Penumbra::Radius(), the radius the texel layers' solve
// gets too, S2-01b), so the pixel shadows' penumbra matches the walls' and the layers'. kBias: the ray's start off the
// surface along the guide's normal, in metres (1 cm: the quarter guide's depth error at 10 m stays under it). kAmbient: the
// share of a pixel's light no lamp ray accounts for (research 16 section 4.1, the floor of the multiplier). UNVERIFIED in game
constexpr float kBias = 0.01f, kAmbient = 0.05f;
constexpr float kClipCaster = 1.5f; // XR-05's sigma near a moved caster

// The filter's source with the INTZ device depth linearised for UpsamplePS (d = A - near A / z, so z / far = (A - 1) / (A - d)
// with far = near A / (A - 1), the zFar sent in the block), plus the passes only the game needs
const std::string kSource = std::string(R"HLSL(
float4 kLin : register(c5); // x = A of the projection
float GiLinear(float d) { return (kLin.x - 1) / max(kLin.x - d, 1e-6); }
#define GI_FULL_DEPTH(uv) GiLinear(tex2D(sDepth, uv).r)
)HLSL") + kGiPixelFilterHlsl + R"HLSL(
float4 kD0 : register(c6); // rows of D = prevViewProj * inverse(viewProj) (gbuffer_motion.h)
float4 kD1 : register(c7);
float4 kD2 : register(c8);
float4 kD3 : register(c9);
// Quarter motion for TemporalPS from the raw output's device depth (a): rg = previous uv - uv, b = class 0 (GB-02 is open)
float4 MotionQPS(float2 uv : TEXCOORD0) : COLOR0 {
    float4 p = float4(uv.x * 2 - 1, 1 - uv.y * 2, tex2D(sRaw, uv).a, 1);
    float4 q = float4(dot(kD0, p), dot(kD1, p), dot(kD2, p), dot(kD3, p));
    if (q.w <= 1e-6) return 0;
    float2 prev = float2(q.x / q.w * 0.5 + 0.5, 0.5 - q.y / q.w * 0.5);
    return float4(prev - uv, 0, 0);
}
float4 RawViewPS(float2 uv : TEXCOORD0) : COLOR0 { float v = tex2D(sRaw, uv).r; return float4(v, v, v, 1); }
)HLSL";

ShaderCache::Id AddShader(const char* tag, const char* entry) {
    ShaderCache::Desc d;
    d.tag = tag;
    d.source = kSource;
    d.sourceName = "gi_pixel_shadows.hlsl";
    d.entry = entry;
    d.target = "ps_3_0";
    d.flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
    d.priority = 1;
    return ShaderCache::Add(std::move(d));
}
enum Ps { kMotionQ, kTemporal, kAtrousFirst, kAtrous, kUpsample, kRawView, kPsCount };
const ShaderCache::Id kPsIds[kPsCount] = {AddShader("GiPixel MotionQPS", "MotionQPS"),     AddShader("GiPixel TemporalPS", "TemporalPS"),
                                          AddShader("GiPixel AtrousFirstPS", "AtrousFirstPS"), AddShader("GiPixel AtrousPS", "AtrousPS"),
                                          AddShader("GiPixel UpsamplePS", "UpsamplePS"),   AddShader("GiPixel RawViewPS", "RawViewPS")};

using OpenFn = HRESULT(__stdcall*)(uint32_t*);
using PassFn = HRESULT(__stdcall*)(const void*, uint32_t, IDirect3DTexture9*, IDirect3DTexture9*, IDirect3DTexture9* const*);
using CountersFn = HRESULT(__stdcall*)(uint32_t*, uint32_t);

template <class T> void SafeRelease(T*& p) {
    if (p) p->Release(), p = nullptr;
}

struct State {
    bool active = false, off = false, loggedOn = false, shadersTried = false, createFailed = false;
    Options o;
    OpenFn open = nullptr;
    PassFn pass = nullptr;
    CountersFn counters = nullptr;
    IDirect3DPixelShader9* ps[kPsCount] = {};
    IDirect3DStateBlock9* saved = nullptr;
    // quarter targets: raw (output 0), guide (output 1) and history twice (this frame, the last), the a-trous pair, motion
    IDirect3DTexture9 *raw = nullptr, *guide[2] = {}, *hist[2] = {}, *tmp[2] = {}, *motion = nullptr;
    UINT qw = 0, qh = 0;
    int cur = 0;
    bool havePrev = false;
    float prevVp[4][4] = {};
    uint32_t lampSerial = 0, frame = 0;
    float lampRadius = 0; // Shadow softness of the last pass: a change resets the history as a lamp change does
    unsigned sent = 0, refused = 0;
    unsigned long long lastLog = 0;
    GiPixelBlock block{};
    // Sim capsules (XR-08): the vertex constants as the game set them, its shaders' classification, this frame's draws
    float vsc[256][4] = {};
    int paletteRegs = 0;                       // registers of the last write that started at c0 (the bone palette)
    IDirect3DPixelShader9* curPs = nullptr;    // the bound shaders (compared only, not referenced)
    IDirect3DVertexShader9* curVs = nullptr;
    std::unordered_map<IDirect3DPixelShader9*, bool> simPs; // held (AddRef) so a reused address cannot inherit an answer
    std::unordered_map<IDirect3DVertexShader9*, GiSimCapsules::Chain> chains; // f < 0: not a skinned chain of the game's form
    struct SimDraw {
        double C[4][4];
        float median[3];
        int fit = 0; // capsules fitted to the bones (model space); 0: the fixed column (XR-10)
        GiSimCapsules::Capsule caps[GiSimCapsules::kMaxPerSim];
    };
    std::vector<SimDraw> simDraws;
    std::vector<GiSimCapsules::Sim> prevSims;
    uint32_t sims = 0, simsFit = 0;
    unsigned long long simDrawsSeen = 0, simDrawsNoChain = 0, simDrawsOtherCamera = 0, simDrawsFit = 0;
} g;

constexpr const char* kHookName = "GiPixelShadows";

template <class Shader> std::vector<DWORD> Code(Shader* sh) {
    UINT bytes = 0;
    if (!sh || FAILED(sh->GetFunction(nullptr, &bytes)) || !bytes || bytes % 4 || bytes > 65536) return {};
    std::vector<DWORD> code(bytes / 4);
    if (FAILED(sh->GetFunction(code.data(), &bytes))) return {};
    return code;
}
bool IsSimPs(IDirect3DPixelShader9* ps) {
    if (!ps) return false;
    if (const auto it = g.simPs.find(ps); it != g.simPs.end()) return it->second;
    if (g.simPs.size() >= 4096) return false;
    const std::vector<DWORD> code = Code(ps);
    const size_t bytes = code.size() * 4;
    const uint32_t hash = ShaderHash(code.data(), bytes);
    const bool sim = std::any_of(std::begin(kSimReceiverPs), std::end(kSimReceiverPs), [&](const ShaderId& id) { return id.size == bytes && id.hash == hash; });
    g.simPs.emplace(ps, sim);
    ps->AddRef();
    return sim;
}
const GiSimCapsules::Chain& ChainOf(IDirect3DVertexShader9* vs) {
    static const GiSimCapsules::Chain kNone{};
    if (!vs) return kNone;
    if (const auto it = g.chains.find(vs); it != g.chains.end()) return it->second;
    if (g.chains.size() >= 4096) return kNone;
    const std::vector<DWORD> code = Code(vs);
    GiSimCapsules::Chain ch;
    if (!GiSimCapsules::Analyse(reinterpret_cast<const uint32_t*>(code.data()), code.size(), ch)) ch = {};
    vs->AddRef();
    return g.chains.emplace(vs, ch).first->second;
}
void ReleaseSimShaders() {
    for (auto& [ps, sim] : g.simPs) ps->Release();
    for (auto& [vs, ch] : g.chains) vs->Release();
    g.simPs.clear(), g.chains.clear();
}

// A scene draw with one of the game's Sim materials (the AO's Sim receiver list): keep its model-to-clip chain and the
// median of its bones, turned into a root with this frame's camera when the effect runs
void NoteSimDraw(D3D9Hooks::DeviceContext& ctx) {
    if (!WorldSession::InWorld() || !IsSimPs(g.curPs)) return;
    g.simDrawsSeen++;
    const GiSimCapsules::Chain& ch = ChainOf(g.curVs);
    if (ch.f < 0 || g.paletteRegs < 3) {
        g.simDrawsNoChain++;
        return;
    }
    IDirect3DSurface9* ds = nullptr; // the scene's depth: not the reflection, a thumbnail or the UI
    ExtraHooks::RawGetDepthStencilSurface(ctx.device, &ds);
    const bool scene = ds && ds == DepthShare::Surface();
    SafeRelease(ds);
    if (!scene || g.simDraws.size() >= 1024) return;
    State::SimDraw d;
    GiSimCapsules::ClipOf(g.vsc, ch, d.C);
    const int bones = std::min(g.paletteRegs, 180) / 3;
    GiSimCapsules::BoneMedian(g.vsc, bones, d.median);
    if (const GiSimCapsules::Rig* rig = GiSimCapsules::RigOf(GiSimCapsules::GameRigs(), bones)) d.fit = GiSimCapsules::FitRig(g.vsc, bones, *rig, d.caps);
    g.simDrawsFit += d.fit > 0;
    g.simDraws.push_back(d);
}
void InstallSimHooks() {
    using namespace D3D9Hooks;
    RegisterSetVertexShaderConstantF(kHookName, [](DeviceContext&, UINT r, const float* c, UINT n) {
        if (c && r < 256) {
            n = std::min<UINT>(n, 256 - r);
            std::memcpy(g.vsc[r], c, n * 16);
            if (r == 0) g.paletteRegs = static_cast<int>(n);
        }
        return HookAction::Continue;
    });
    RegisterSetPixelShader(kHookName, [](DeviceContext&, IDirect3DPixelShader9* ps) {
        g.curPs = ps;
        return HookAction::Continue;
    });
    RegisterSetVertexShader(kHookName, [](DeviceContext&, IDirect3DVertexShader9* vs) {
        g.curVs = vs;
        return HookAction::Continue;
    });
    RegisterDrawIndexedPrimitive(kHookName, [](DeviceContext& ctx, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT) {
        NoteSimDraw(ctx);
        return HookAction::Continue;
    });
}

// This frame's Sims from its Sim draws (the camera is final now), into the block after any test capsule
void FillSims(const float vp[4][4]) {
    std::vector<GiSimCapsules::Sim> draws;
    for (const State::SimDraw& d : g.simDraws) {
        double M[4][4];
        if (!GiSimCapsules::WorldOf(d.C, vp, M)) {
            g.simDrawsOtherCamera++;
            continue;
        }
        GiSimCapsules::Sim& s = draws.emplace_back();
        GiSimCapsules::Apply(M, d.median, s.root.p);
        GiSimCapsules::ToWorld(M, d.caps, d.fit, s);
    }
    g.simDraws.clear();
    const std::vector<GiSimCapsules::Sim> sims = GiSimCapsules::Merge(draws, g.block.cam, kGiPixelCasters);
    g.sims = GiSimCapsules::Fill(g.block, sims, g.prevSims);
    g.simsFit = static_cast<uint32_t>(std::count_if(sims.begin(), sims.begin() + std::min<size_t>(g.sims, sims.size()), [](const GiSimCapsules::Sim& s) { return s.n > 0; }));
    g.prevSims = sims;
}

const char* StateName(uint32_t s) {
    switch (s) {
    case 2: return "stock renderer";
    case 3: return "no ray queries";
    default: return "other (see the bridge server's log)";
    }
}
void GoOff(const std::string& why) {
    g.off = true;
    MeshCasterSet::pixelCasts.store(false);
    LOG_INFO("[GiPixel] off: " + why);
}

// The three exports of the bridge client (patch 0019), and its answer: true once it said on
bool Opened() {
    if (g.off) return false;
    if (!g.open) {
        const HMODULE d3d9 = GetModuleHandleW(L"d3d9.dll");
        g.open = d3d9 ? reinterpret_cast<OpenFn>(GetProcAddress(d3d9, "RemixBridge_GiPixelOpen")) : nullptr;
        g.pass = d3d9 ? reinterpret_cast<PassFn>(GetProcAddress(d3d9, "RemixBridge_GiPixelPass")) : nullptr;
        g.counters = d3d9 ? reinterpret_cast<CountersFn>(GetProcAddress(d3d9, "RemixBridge_GiPixelReadCounters")) : nullptr;
        if (!g.open || !g.pass || !g.counters) return GoOff("no bridge export (needs the bridge pair of patch 0019)"), false;
    }
    uint32_t state = 0;
    const HRESULT hr = g.open(&state);
    if (hr == S_FALSE) return false; // the server answers with a later frame
    if (hr != S_OK) return GoOff(std::format("the bridge refused the open (0x{:08X})", static_cast<uint32_t>(hr))), false;
    if (state != 1) return GoOff(StateName(state)), false;
    if (!g.loggedOn) {
        g.loggedOn = true;
        LOG_INFO(std::format("[GiPixel] On: view={} testCaster={} meshCasters={}", g.o.view, g.o.testCaster ? 1 : 0, g.o.meshCasters ? 1 : 0));
        MeshCasterSet::pixelCasts.store(g.o.meshCasters); // (XR-10) objects the server casts leave the Direct3D 9 tests
    }
    return true;
}

void ReleaseTargets() {
    for (auto* t : {&g.raw, &g.guide[0], &g.guide[1], &g.hist[0], &g.hist[1], &g.tmp[0], &g.tmp[1], &g.motion}) SafeRelease(*t);
    SafeRelease(g.saved);
    g.qw = g.qh = 0;
    g.havePrev = false;
}
void ReleaseShaders() {
    for (auto*& p : g.ps) SafeRelease(p);
    g.shadersTried = false;
}

bool EnsureShaders(IDirect3DDevice9* dev) {
    if (g.ps[kPsCount - 1]) return true;
    if (g.shadersTried) return false;
    g.shadersTried = true;
    std::string msg;
    for (int i = 0; i < kPsCount; i++)
        if (ShaderCache::CreatePixelShader(dev, kPsIds[i], &g.ps[i], &msg) != ShaderCache::Result::Ok) {
            ReleaseShaders();
            g.shadersTried = true;
            GoOff("shaders unavailable: " + msg);
            return false;
        }
    return true;
}

bool EnsureTargets(IDirect3DDevice9* dev, UINT w, UINT h) {
    if (g.raw && g.qw == w && g.qh == h) return true;
    ReleaseTargets();
    if (g.createFailed) return false;
    bool ok = SUCCEEDED(dev->CreateStateBlock(D3DSBT_ALL, &g.saved)) && g.saved;
    for (auto* t : {&g.raw, &g.guide[0], &g.guide[1], &g.hist[0], &g.hist[1], &g.tmp[0], &g.tmp[1], &g.motion})
        ok = ok && SUCCEEDED(dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, kFormat, D3DPOOL_DEFAULT, t, nullptr)) && *t;
    if (!ok) {
        ReleaseTargets();
        g.createFailed = true;
        LOG_ERROR(std::format("[GiPixel] Could not create the {}x{} A16B16G16R16F targets; pixel shadows off", w, h));
        return false;
    }
    g.qw = w, g.qh = h;
    return true;
}

// The camera from the inverse view-projection (row vectors): the eye is row 2 projected, the far centre (0, 0, 1, 1)
bool FillCamera(const float vp[4][4], float& tanY) {
    double m[4][4], inv[4][4];
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) m[r][c] = vp[r][c];
    if (!GBufferMotion::Inverse(m, inv) || std::fabs(inv[2][3]) < 1e-12) return false;
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) g.block.invViewProj[r * 4 + c] = static_cast<float>(inv[r][c]);
    const auto at = [&](double x, double y, double out[3]) {
        const double w = x * inv[0][3] + y * inv[1][3] + inv[2][3] + inv[3][3];
        for (int i = 0; i < 3; i++) out[i] = (x * inv[0][i] + y * inv[1][i] + inv[2][i] + inv[3][i]) / w;
    };
    double eye[3], far0[3], far1[3];
    for (int i = 0; i < 3; i++) eye[i] = inv[2][i] / inv[2][3];
    at(0, 0, far0), at(0, 1, far1);
    const double fwd[3] = {far0[0] - eye[0], far0[1] - eye[1], far0[2] - eye[2]};
    const double len = std::sqrt(fwd[0] * fwd[0] + fwd[1] * fwd[1] + fwd[2] * fwd[2]);
    if (len < 1e-6) return false;
    for (int i = 0; i < 3; i++) g.block.cam[i] = static_cast<float>(eye[i]), g.block.viewZ[i] = static_cast<float>(fwd[i] / len);
    const double up = std::sqrt((far1[0] - far0[0]) * (far1[0] - far0[0]) + (far1[1] - far0[1]) * (far1[1] - far0[1]) + (far1[2] - far0[2]) * (far1[2] - far0[2]));
    tanY = static_cast<float>(up / len);
    const float A = PostScene::CameraDepthA(), nearZ = PostScene::CameraNear();
    g.block.zFar = nearZ > 0 && A > 1.0f ? nearZ * A / (A - 1.0f) : static_cast<float>(len);
    return true;
}

// The 16 strongest lamps that are on, by colour over distance squared from the camera (the camera's lot in practice)
void FillLamps() {
    struct Pick { float w; const RealtimeGI::Lamp* l; };
    std::vector<Pick> picks;
    for (const RealtimeGI::Lamp& l : RealtimeGI::Lamps()) {
        if (!l.on) continue;
        const float d[3] = {l.pos[0] - g.block.cam[0], l.pos[1] - g.block.cam[1], l.pos[2] - g.block.cam[2]};
        picks.push_back({(l.colour[0] + l.colour[1] + l.colour[2]) / (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] + 1.0f), &l});
    }
    const size_t n = std::min<size_t>(picks.size(), kGiPixelLamps);
    std::partial_sort(picks.begin(), picks.begin() + n, picks.end(), [](const Pick& a, const Pick& b) { return a.w > b.w; });
    g.block.lamps = static_cast<uint32_t>(n);
    for (size_t i = 0; i < n; i++) {
        GiPixelLamp& o = g.block.lamp[i];
        std::memcpy(o.pos, picks[i].l->pos, sizeof o.pos), std::memcpy(o.col, picks[i].l->colour, sizeof o.col);
        o.radius = Penumbra::Radius(), o.pad = 0;
    }
    // No static boxes: furniture comes as mesh casters (MC-2, meshCasters), which the bridge server adds itself
    g.block.capsules = g.block.casters = g.block.boxes = 0;
    if (g.o.testCaster && n) {
        const float* p = g.block.lamp[0].pos;
        GiPixelCapsule& c = g.block.capsule[0];
        c = {{p[0] + 0.4f, p[1] - 0.4f, p[2]}, 0.12f, {p[0] + 0.4f, p[1] - 1.2f, p[2]}, 0};
        GiPixelCaster& b = g.block.caster[0];
        b = {{c.b[0] - c.r, c.b[1] - c.r, c.b[2] - c.r}, 0, {c.a[0] + c.r, c.a[1] + c.r, c.a[2] + c.r}, 0};
        g.block.capsules = g.block.casters = 1;
    }
}

void Bind(IDirect3DDevice9* dev, DWORD s, IDirect3DBaseTexture9* t) {
    dev->SetTexture(s, t);
    dev->SetSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    dev->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(s, D3DSAMP_SRGBTEXTURE, FALSE);
}
void Draw(IDirect3DDevice9* dev, IDirect3DSurface9* target, IDirect3DPixelShader9* ps) {
    dev->SetRenderTarget(0, target); // (resets the viewport to the target)
    dev->SetPixelShader(ps);
    D3DSURFACE_DESC td{};
    target->GetDesc(&td);
    struct V { float x, y, z, rhw, u, v; };
    const float x1 = static_cast<float>(td.Width) - 0.5f, y1 = static_cast<float>(td.Height) - 0.5f;
    const V v[4] = {{-0.5f, -0.5f, 0, 1, 0, 0}, {x1, -0.5f, 0, 1, 1, 0}, {-0.5f, y1, 0, 1, 0, 1}, {x1, y1, 0, 1, 1, 1}};
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(V));
}
void DrawTo(IDirect3DDevice9* dev, IDirect3DTexture9* t, IDirect3DPixelShader9* ps) {
    IDirect3DSurface9* s = nullptr;
    if (SUCCEEDED(t->GetSurfaceLevel(0, &s)) && s) Draw(dev, s, ps), s->Release();
}

void LogCounters() {
    const unsigned long long now = GetTickCount64();
    if (now - g.lastLog < 10000) return;
    g.lastLog = now;
    if (kPublicBuild) return;
    uint32_t w[9] = {}; // util_gi_pixel.h GiPixelShared: state, passes, faults, rays low / high, raysLast, gpuUs, width, height
    if (const HRESULT hr = g.counters(w, 9); hr != S_OK) {
        LOG_WARNING(std::format("[GiPixel] counters unread (0x{:08X}); sent={} refused={}", static_cast<uint32_t>(hr), g.sent, g.refused));
        return;
    }
    const std::string line = std::format("[GiPixel] passes={} rays={} raysLast={} gpuUs={} size={}x{} faults={} sent={} refused={} sims={} simDraws={} "
                                         "noChain={} otherCamera={} simsFit={} simDrawsFit={}", w[1], (static_cast<uint64_t>(w[4]) << 32) | w[3], w[5], w[6], w[7], w[8], w[2], g.sent,
                                         g.refused, g.sims, g.simDrawsSeen, g.simDrawsNoChain, g.simDrawsOtherCamera, g.simsFit, g.simDrawsFit);
    if (w[2] || g.refused) LOG_WARNING(line);
    else LOG_INFO(line);
}

void Effect(IDirect3DDevice9* dev) {
    struct ClearSimDraws { // whatever the path out: next frame's Sims come from next frame's draws
        ~ClearSimDraws() { g.simDraws.clear(); }
    } clearSimDraws;
    if (!g.active || g.off || !WorldSession::InWorld()) return;
    IDirect3DTexture9* depth = DepthShare::Texture();
    if (!depth) return;
    IDirect3DSurface9* ds = nullptr; // the scene's depth bound now: not a reflection or UI pass
    ExtraHooks::RawGetDepthStencilSurface(dev, &ds);
    const bool sceneDepth = ds && ds == DepthShare::Surface();
    SafeRelease(ds);
    float vp[4][4], tanY = 0;
    // after 600 world frames in a row without a finished pass (the first three times), the gate that held each of them, with
    // the camera vote's state (the open is not asked while camera=0). Bridge runs with the camera fallback printed neither
    // this nor a passes line: the pass returned at a later gate, or only some frames had a camera.
    static uint32_t waited = 0, held[4] = {}, waitNotes = 0; // held: no scene depth / camera, not open, no shaders, targets or camera fill
    const auto hold = [&](int gate) {
        held[gate]++;
        if (!g.off && ++waited == 600 && waitNotes < 3)
            ++waitNotes, LOG_INFO(std::format("[GiPixel] waiting 600 world frames: sceneDepth={} camera={} open={} shaders=0 (open 0: no server answer yet; not asked "
                                              "while camera=0); held by camera {} open {} shaders {} targets {}; sent={} refused={} | {}",
                                              sceneDepth ? 1 : 0, gate > 0 ? 1 : 0, gate > 1 ? 1 : 0, held[0], held[1], held[2], held[3], g.sent, g.refused, PostScene::DiagText()));
    };
    const bool cam = sceneDepth && PostScene::CameraViewProj(vp), open = cam && Opened(), shaders = open && EnsureShaders(dev);
    if (!shaders) return hold(!cam ? 0 : !open ? 1 : 2);
    D3DSURFACE_DESC dd{};
    depth->GetLevelDesc(0, &dd);
    if (dd.Width < 4 || dd.Height < 4 || !EnsureTargets(dev, dd.Width / 4, dd.Height / 4) || !FillCamera(vp, tanY)) return hold(3);
    waited = 0, held[0] = held[1] = held[2] = held[3] = 0;

    // reset: no history yet, a camera cut, or a lamp change; still: the camera did not move
    double D[4][4] = {};
    const bool haveD = g.havePrev && GBufferMotion::Reproject(vp, g.prevVp, D);
    const bool reset = !haveD || GBufferMotion::IsCut(D) || RealtimeGI::LampSerial() != g.lampSerial || Penumbra::Radius() != g.lampRadius;
    const bool still = haveD && std::memcmp(vp, g.prevVp, sizeof vp) == 0;
    g.lampSerial = RealtimeGI::LampSerial(), g.lampRadius = Penumbra::Radius();
    std::memcpy(g.prevVp, vp, sizeof vp);
    g.havePrev = true;
    g.cur ^= 1;

    FillLamps();
    FillSims(vp);
    g.block.ambient = kAmbient, g.block.bias = kBias, g.block.radiusScale = 1.0f;
    g.block.frame = g.frame++, g.block.samples = 1;
    IDirect3DTexture9* const outputs[kGiPixelOutputs] = {g.raw, g.guide[g.cur], nullptr, nullptr};

    IDirect3DSurface9 *rt0 = nullptr, *sceneDs = nullptr;
    if (FAILED(dev->GetRenderTarget(0, &rt0)) || !rt0) return;
    ExtraHooks::RawGetDepthStencilSurface(dev, &sceneDs);
    g.saved->Capture();
    const auto input = ExtraHooks::MarkVertexInput();
    ExtraHooks::RawSetDepthStencilSurface(dev, nullptr); // the depth is sampled, so it must not be bound

    if (g.pass(&g.block, sizeof g.block, depth, nullptr, outputs) == S_OK) g.sent++;
    else g.refused++;

    dev->SetVertexShader(nullptr);
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    for (const D3DRENDERSTATETYPE rs : {D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_SEPARATEALPHABLENDENABLE, D3DRS_ALPHATESTENABLE,
                                        D3DRS_STENCILENABLE, D3DRS_SCISSORTESTENABLE, D3DRS_FOGENABLE, D3DRS_SRGBWRITEENABLE, D3DRS_CLIPPLANEENABLE})
        dev->SetRenderState(rs, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    const float q[4] = {float(g.qw), float(g.qh), 1.0f / g.qw, 1.0f / g.qh};
    const float temporal[4] = {still ? 1.0f : 0.0f, reset ? 1.0f : 0.0f, kClipCaster, 0};
    const float tol[4] = {0.02f, 1.5f * 2.0f * tanY / g.qh, 0.9f, 4}; // XR-05's values
    const float viewZ[4] = {g.block.viewZ[0], g.block.viewZ[1], g.block.viewZ[2], 0};
    const float lin[4] = {PostScene::CameraDepthA(), 0, 0, 0};
    float d[16];
    for (int i = 0; i < 16; i++) d[i] = static_cast<float>(D[i / 4][i % 4]);
    dev->SetPixelShaderConstantF(0, q, 1), dev->SetPixelShaderConstantF(1, temporal, 1), dev->SetPixelShaderConstantF(2, tol, 1);
    dev->SetPixelShaderConstantF(3, viewZ, 1), dev->SetPixelShaderConstantF(5, lin, 1), dev->SetPixelShaderConstantF(6, d, 4);

    Bind(dev, 0, g.raw), Bind(dev, 1, g.guide[g.cur]), Bind(dev, 6, depth);
    DrawTo(dev, g.motion, g.ps[kMotionQ]);
    Bind(dev, 2, g.motion), Bind(dev, 3, g.hist[g.cur ^ 1]), Bind(dev, 4, g.guide[g.cur ^ 1]);
    DrawTo(dev, g.hist[g.cur], g.ps[kTemporal]);
    const auto atrous = [&](bool first, float step, float strength, float low, IDirect3DTexture9* in, IDirect3DTexture9* out) {
        const float c[4] = {step, strength, low, 0};
        dev->SetPixelShaderConstantF(4, c, 1);
        Bind(dev, first ? 3 : 5, in);
        DrawTo(dev, out, g.ps[first ? kAtrousFirst : kAtrous]);
        dev->SetTexture(first ? 3 : 5, nullptr);
    };
    atrous(true, 1, 1, 0, g.hist[g.cur], g.tmp[0]);
    atrous(false, 2, 1, 0, g.tmp[0], g.tmp[1]);
    atrous(false, 4, 0, 1, g.tmp[1], g.tmp[0]); // the wide pass only where the count is low
    Bind(dev, 5, g.tmp[0]);
    if (g.o.view == 0) { // the back buffer x the filtered visibility
        dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
        dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
        dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ZERO);
        dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_SRCCOLOR);
        dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0x7);
    }
    Draw(dev, rt0, g.ps[g.o.view == 2 ? kRawView : kUpsample]);
    for (DWORD s = 0; s < 7; s++) dev->SetTexture(s, nullptr); // nothing of ours stays bound while it is a target next frame

    dev->SetRenderTarget(0, rt0);
    ExtraHooks::RawSetDepthStencilSurface(dev, sceneDs);
    g.saved->Apply(); // (after the target: setting target 0 resets the viewport)
    ExtraHooks::RestoreVertexInput(input);
    SafeRelease(rt0), SafeRelease(sceneDs);
    LogCounters();
}

void OnPreReset(IDirect3DDevice9*) { ReleaseTargets(); }
void RestartEffects(IDirect3DDevice9*) {
    ReleaseTargets();
    ReleaseShaders();
    g.createFailed = false;
}

} // namespace

void Start(const Options& o) {
    if (g.active) return;
    g = State{};
    g.o = o;
    g.active = true;
    DepthShare::Request(true);
    PostScene::WantCamera(true);
    RenderCallbacks::Add(RenderCallbacks::preReset, OnPreReset);
    RenderCallbacks::Add(RenderCallbacks::restartEffects, RestartEffects);
    PostScene::Add(kOrder, Effect);
    InstallSimHooks();
}

void Stop() {
    if (!g.active) return;
    MeshCasterSet::pixelCasts.store(false);
    PostScene::Remove(Effect);
    D3D9Hooks::UnregisterAll(kHookName);
    ReleaseSimShaders();
    RenderCallbacks::Remove(RenderCallbacks::preReset, OnPreReset);
    RenderCallbacks::Remove(RenderCallbacks::restartEffects, RestartEffects);
    ReleaseTargets();
    ReleaseShaders();
    PostScene::WantCamera(false);
    DepthShare::Request(false);
    g.active = false;
}

} // namespace GiPixelShadows
