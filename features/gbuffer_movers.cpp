// Motion vectors of moving objects and Sims (G-buffer work package GB-05; docs/features/realtime-gi/wp/GB-05.md). See
// gbuffer_movers.h. Developer builds only.
//
// Part of Apex Radiance. Credits: @loinyx
#ifndef APEX_NO_DEV_TOOLS
#include "gbuffer_movers.h"
#include "d3d9_hooks.h"
#include "post_scene.h"
#include "shader_cache.h"
#include "shader_patches.h"
#include <d3dcompiler.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <unordered_map>
#include <vector>

namespace {

constexpr const char* kHookName = "GBufferMovers";
constexpr unsigned kMaxReplays = 512;             // per frame
constexpr float kDepthBias = -1.0f / (1 << 20);   // the replay passes the depth test where the draw itself won
constexpr D3DFORMAT kFormat = D3DFMT_A32B32G32R32F;

const char* kMoverPs = R"HLSL(
// cur / prev: this frame's and last frame's clip position of the same surface point, without jitter
float4 MoverPS(float4 cur : TEXCOORD0, float4 prev : TEXCOORD1) : COLOR0
{
    float d = cur.z / cur.w;
    if (prev.w <= 1e-6) return float4(32768, 32768, d, 1); // behind last frame's camera: no history
    return float4(cur.xy / cur.w - prev.xy / prev.w, d, 1);
}
)HLSL";
const ShaderCache::Id kMoverPsId = [] {
    ShaderCache::Desc d;
    d.tag = "GBuffer MoverPS";
    d.source = kMoverPs;
    d.sourceName = "gbuffer_movers.hlsl";
    d.entry = "MoverPS";
    d.target = "ps_3_0";
    d.flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
    d.priority = 1;
    return ShaderCache::Add(std::move(d));
}();

template <class T> void SafeRelease(T*& p) {
    if (p) { p->Release(); p = nullptr; }
}

struct Variant {
    IDirect3DVertexShader9* vs = nullptr;
    ShaderPatches::MotionReplayLayout layout;
    bool failed = false;
};
struct SliceEntry {
    UINT size = 0; // the shader's byte size: an address the game reused for another shader is noticed by it
    bool ok = false;
    ShaderPatches::MotionSlice slice;
    std::map<std::vector<int>, Variant> variants; // by extents
};
struct Entry {
    std::vector<int> ext;
    std::vector<float> v; // singles, then each region's extent, 4 floats a register
    bool taken = false;
};

std::atomic<bool> g_active{false};
UINT g_uploadCount[256] = {}; // the count of the last game upload starting at each register
std::unordered_map<IDirect3DVertexShader9*, std::unique_ptr<SliceEntry>> g_slices;
std::unordered_map<uint64_t, std::vector<Entry>> g_cur, g_prev;
float g_cam[4][4] = {}, g_prevCam[4][4] = {};
bool g_haveCam = false, g_havePrevCam = false, g_camRead = false; // this frame's camera (read at the first mover test)
IDirect3DTexture9* g_tex = nullptr;
IDirect3DSurface9* g_surf = nullptr;
UINT g_w = 0, g_h = 0;
bool g_cleared = false, g_written = false, g_createFailed = false;
IDirect3DPixelShader9* g_ps = nullptr;
bool g_psTried = false;
GBufferMovers::Counts g_count;
// Prepare -> Replay
const Variant* g_pendVariant = nullptr;
const ShaderPatches::MotionSlice* g_pendSlice = nullptr;
std::vector<float> g_pendPrev;

SliceEntry* SliceOf(IDirect3DVertexShader9* vs) {
    UINT size = 0;
    if (!vs || FAILED(vs->GetFunction(nullptr, &size)) || size < 8 || size % 4) return nullptr;
    auto& e = g_slices[vs];
    if (e && e->size == size) return e.get();
    if (e)
        for (auto& [k, v] : e->variants) SafeRelease(v.vs);
    e = std::make_unique<SliceEntry>();
    e->size = size;
    std::vector<DWORD> t(size / 4);
    if (SUCCEEDED(vs->GetFunction(t.data(), &size))) e->ok = ShaderPatches::AnalyzeMotionSlice(t, &e->slice) == ShaderPatches::MotionSliceResult::Ok;
    return e.get();
}

const Variant* VariantOf(IDirect3DDevice9* dev, SliceEntry& se, const std::vector<int>& ext) {
    Variant& v = se.variants[ext];
    if (v.vs || v.failed) return &v;
    std::vector<DWORD> code;
    v.failed = ShaderPatches::MakeMotionReplayVs(se.slice, ext, &v.layout, &code) != ShaderPatches::MotionSliceResult::Ok ||
               FAILED(D3D9Hooks::CallOriginalCreateVertexShader(dev, code.data(), &v.vs)) || !v.vs;
    return &v;
}

void ReleaseAll() {
    for (auto& [vs, e] : g_slices)
        for (auto& [k, v] : e->variants) SafeRelease(v.vs);
    g_slices.clear();
    g_cur.clear();
    g_prev.clear();
    SafeRelease(g_surf);
    SafeRelease(g_tex);
    SafeRelease(g_ps);
    g_psTried = false;
    g_w = g_h = 0;
    g_written = g_cleared = false;
}

bool SameRow(const float* v, const float row[4]) { return std::memcmp(v, row, 16) == 0; }

} // namespace

namespace GBufferMovers {

void Acquire() {
    if (g_active.exchange(true)) return;
    std::memset(g_uploadCount, 0, sizeof g_uploadCount);
    D3D9Hooks::RegisterSetVertexShaderConstantF(kHookName, [](D3D9Hooks::DeviceContext&, UINT start, const float*, UINT count) {
        if (start < 256) g_uploadCount[start] = count;
        return D3D9Hooks::HookAction::Continue;
    }, D3D9Hooks::Priority::First);
}

void Release(IDirect3DDevice9*) {
    if (!g_active.exchange(false)) return;
    D3D9Hooks::UnregisterAll(kHookName);
    ReleaseAll();
}

bool Active() { return g_active.load(std::memory_order_relaxed); }

uint64_t DrawKey(IDirect3DDevice9* dev, IDirect3DVertexShader9* vs, uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e) {
    IDirect3DVertexBuffer9* vb = nullptr;
    IDirect3DIndexBuffer9* ib = nullptr;
    UINT offset = 0, stride = 0;
    dev->GetStreamSource(0, &vb, &offset, &stride);
    dev->GetIndices(&ib);
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](uint64_t x) { h = (h ^ x) * 1099511628211ull; };
    mix(reinterpret_cast<uintptr_t>(vs)), mix(reinterpret_cast<uintptr_t>(vb)), mix(offset), mix(reinterpret_cast<uintptr_t>(ib));
    mix(a), mix(b), mix(c), mix(d), mix(e);
    SafeRelease(vb);
    SafeRelease(ib);
    return h;
}

bool Prepare(IDirect3DDevice9* dev, IDirect3DVertexShader9* gameVs, uint64_t key) {
    if (!Active()) return false;
    g_count.draws++;
    SliceEntry* se = SliceOf(gameVs);
    if (!se || !se->ok) { g_count.noSlice++; return false; }
    const auto& s = se->slice;
    Entry cur;
    for (const auto& g : s.regions) cur.ext.push_back(std::clamp(g_uploadCount[g.base] ? static_cast<int>(g_uploadCount[g.base]) : g.maxExtent, g.lastOffset + 1,
                                                                 std::max(g.lastOffset + 1, g.maxExtent)));
    size_t n = s.singles.size();
    for (int e : cur.ext) n += static_cast<size_t>(e);
    cur.v.resize(n * 4);
    float* p = cur.v.data();
    for (int r : s.singles) dev->GetVertexShaderConstantF(static_cast<UINT>(r), p, 1), p += 4;
    for (size_t i = 0; i < s.regions.size(); i++) {
        dev->GetVertexShaderConstantF(static_cast<UINT>(s.regions[i].base), p, static_cast<UINT>(cur.ext[i]));
        p += 4 * cur.ext[i];
    }
    // last frame's draw with this key: the nearest of its entries (equal meshes drawn more than once)
    Entry* prev = nullptr;
    double best = 0;
    if (auto it = g_prev.find(key); it != g_prev.end())
        for (Entry& e : it->second) {
            if (e.taken || e.ext != cur.ext) continue;
            double dist = 0;
            for (size_t k = 0; k < e.v.size(); k++) dist += std::fabs(static_cast<double>(e.v[k]) - cur.v[k]);
            if (!prev || dist < best) prev = &e, best = dist;
        }
    bool moved = false;
    if (!g_camRead) g_camRead = true, g_haveCam = PostScene::CameraViewProj(g_cam);
    if (prev) {
        prev->taken = true;
        // a single equal to the camera's row k now and last frame's row k then is the camera, which the camera pass has
        for (size_t i = 0; i < s.singles.size() && !moved; i++) {
            const float *a = &cur.v[4 * i], *b = &prev->v[4 * i];
            if (std::memcmp(a, b, 16) == 0) continue;
            bool camera = false;
            for (int k = 0; k < 4 && !camera; k++) camera = g_haveCam && g_havePrevCam && SameRow(a, g_cam[k]) && SameRow(b, g_prevCam[k]);
            moved = !camera;
        }
        const size_t at = 4 * s.singles.size();
        moved = moved || std::memcmp(cur.v.data() + at, prev->v.data() + at, (cur.v.size() - at) * 4) != 0;
        if (moved) g_pendPrev = prev->v;
    }
    g_cur[key].push_back(std::move(cur));
    if (!prev) { g_count.firstSeen++; return false; }
    if (!moved) { g_count.still++; return false; }
    if (g_count.replayed >= kMaxReplays) { g_count.capped++; return false; }
    const Variant* v = VariantOf(dev, *se, g_cur[key].back().ext);
    if (!v->vs) { g_count.noRoom++; return false; }
    if (!g_ps && !g_psTried) {
        g_psTried = true;
        ShaderCache::CreatePixelShader(dev, kMoverPsId, &g_ps);
    }
    if (!g_ps) return false;
    g_pendVariant = v;
    g_pendSlice = &se->slice;
    return true;
}

void Replay(IDirect3DDevice9* dev, const float jitter[2], const std::function<void()>& draw) {
    const Variant* v = g_pendVariant;
    const ShaderPatches::MotionSlice* s = g_pendSlice;
    g_pendVariant = nullptr;
    if (!v || !s) return;
    IDirect3DSurface9* rt0 = nullptr;
    if (FAILED(dev->GetRenderTarget(0, &rt0)) || !rt0) return;
    D3DSURFACE_DESC rd{};
    rt0->GetDesc(&rd);
    if (!g_tex || g_w != rd.Width || g_h != rd.Height) {
        SafeRelease(g_surf);
        SafeRelease(g_tex);
        g_w = g_h = 0;
        if (g_createFailed || FAILED(dev->CreateTexture(rd.Width, rd.Height, 1, D3DUSAGE_RENDERTARGET, kFormat, D3DPOOL_DEFAULT, &g_tex, nullptr)) || !g_tex ||
            FAILED(g_tex->GetSurfaceLevel(0, &g_surf))) {
            SafeRelease(g_tex);
            g_createFailed = true;
            rt0->Release();
            return;
        }
        g_w = rd.Width, g_h = rd.Height;
        g_cleared = false;
    }
    if (!g_cleared) dev->ColorFill(g_surf, nullptr, 0), g_cleared = true; // alpha 0: no mover at that pixel
    // bank B: last frame's values at the layout's registers (the device's values there are put back after)
    const auto& L = v->layout;
    std::vector<std::pair<UINT, std::array<float, 4>>> upload, saved;
    const float* p = g_pendPrev.data();
    for (size_t i = 0; i < s->singles.size(); i++, p += 4) upload.push_back({static_cast<UINT>(L.singleTo[i]), {p[0], p[1], p[2], p[3]}});
    for (size_t i = 0; i < s->regions.size(); i++)
        for (int k = 0; k < L.extents[i]; k++, p += 4) upload.push_back({static_cast<UINT>(L.regionTo[i] + k), {p[0], p[1], p[2], p[3]}});
    upload.push_back({static_cast<UINT>(L.jitter), {jitter[0], jitter[1], 0, 0}});
    for (const auto& [r, val] : upload) {
        std::array<float, 4> old{};
        dev->GetVertexShaderConstantF(r, old.data(), 1);
        saved.push_back({r, old});
    }
    constexpr D3DRENDERSTATETYPE kStates[] = {D3DRS_ZWRITEENABLE, D3DRS_ZFUNC, D3DRS_ALPHABLENDENABLE, D3DRS_ALPHATESTENABLE, D3DRS_COLORWRITEENABLE,
                                              D3DRS_FOGENABLE, D3DRS_SRGBWRITEENABLE, D3DRS_STENCILENABLE, D3DRS_DEPTHBIAS, D3DRS_SLOPESCALEDEPTHBIAS};
    DWORD rs[std::size(kStates)] = {};
    for (size_t i = 0; i < std::size(kStates); i++) dev->GetRenderState(kStates[i], &rs[i]);
    D3DVIEWPORT9 vp{};
    dev->GetViewport(&vp);
    IDirect3DVertexShader9* vs = D3D9Hooks::BoundVertexShader(dev);
    IDirect3DPixelShader9* ps = D3D9Hooks::BoundPixelShader(dev);

    D3D9Hooks::CallOriginalSetRenderTarget(dev, 0, g_surf);
    D3D9Hooks::CallOriginalSetViewport(dev, &vp);
    float bias = kDepthBias;
    DWORD biasBits;
    std::memcpy(&biasBits, &bias, 4);
    const DWORD zfunc = rs[1] == D3DCMP_EQUAL ? D3DCMP_EQUAL : D3DCMP_LESSEQUAL;
    const DWORD set[std::size(kStates)] = {FALSE, zfunc, FALSE, FALSE, 0xF, FALSE, FALSE, FALSE, biasBits, 0};
    for (size_t i = 0; i < std::size(kStates); i++) dev->SetRenderState(kStates[i], set[i]);
    for (const auto& [r, val] : upload) D3D9Hooks::CallOriginalSetVertexShaderConstantF(dev, r, val.data(), 1);
    D3D9Hooks::CallOriginalSetVertexShader(dev, v->vs);
    D3D9Hooks::CallOriginalSetPixelShader(dev, g_ps);
    draw();
    D3D9Hooks::CallOriginalSetPixelShader(dev, ps);
    D3D9Hooks::CallOriginalSetVertexShader(dev, vs);
    for (const auto& [r, val] : saved) D3D9Hooks::CallOriginalSetVertexShaderConstantF(dev, r, val.data(), 1);
    for (size_t i = 0; i < std::size(kStates); i++) dev->SetRenderState(kStates[i], rs[i]);
    D3D9Hooks::CallOriginalSetRenderTarget(dev, 0, rt0);
    D3D9Hooks::CallOriginalSetViewport(dev, &vp);
    rt0->Release();
    g_count.replayed++;
    g_written = true;
}

void FrameBoundary() {
    if (!Active()) return;
    g_prev.swap(g_cur);
    g_cur.clear();
    g_written = g_cleared = false;
    if (!g_camRead) g_haveCam = PostScene::CameraViewProj(g_cam);
    std::memcpy(g_prevCam, g_cam, sizeof g_cam); // the frame just drawn: last frame's values were read with this camera
    g_havePrevCam = g_haveCam;
    g_haveCam = g_camRead = false;
}

IDirect3DTexture9* Target() { return Active() && g_written ? g_tex : nullptr; }

Counts TakeCounts() {
    const Counts c = g_count;
    g_count = {};
    return c;
}

void OnPreReset() {
    SafeRelease(g_surf);
    SafeRelease(g_tex);
    g_w = g_h = 0;
    g_written = g_cleared = false;
    g_createFailed = false;
}

} // namespace GBufferMovers
#endif
