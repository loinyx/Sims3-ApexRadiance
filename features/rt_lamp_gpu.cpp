// Real-time outdoor lamps: the GPU side (see rt_lamp_gpu.h). No game access: tools/rt_lamps_test compiles this file.
//
// Part of Apex Radiance. Credits: @loinyx

#include "rt_lamp_gpu.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>

namespace RtLampGpu {

// right, up, forward per face (+X, -X, +Y, -Y, +Z, -Z): the same table as shaders/rt_lamps_hlsl.h
const float kFaces[6][3][3] = {
    {{0, 0, -1}, {0, 1, 0}, {1, 0, 0}},  {{0, 0, 1}, {0, 1, 0}, {-1, 0, 0}}, {{1, 0, 0}, {0, 0, -1}, {0, 1, 0}},
    {{1, 0, 0}, {0, 0, 1}, {0, -1, 0}},  {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}},  {{-1, 0, 0}, {0, 1, 0}, {0, 0, -1}},
};
constexpr float kNear = 0.005f; // m: the near plane (a slab 5 cm under a lamp must be in its shadow map)

namespace {
HRESULT DevSetTexture(IDirect3DDevice9* d, DWORD s, IDirect3DBaseTexture9* t) { return d->SetTexture(s, t); }
HRESULT DevSetRt(IDirect3DDevice9* d, DWORD i, IDirect3DSurface9* s) { return d->SetRenderTarget(i, s); }
HRESULT DevSetDs(IDirect3DDevice9* d, IDirect3DSurface9* s) { return d->SetDepthStencilSurface(s); }
HRESULT DevGetDs(IDirect3DDevice9* d, IDirect3DSurface9** s) { return d->GetDepthStencilSurface(s); }
HRESULT DevSetVs(IDirect3DDevice9* d, IDirect3DVertexShader9* s) { return d->SetVertexShader(s); }
HRESULT DevSetPs(IDirect3DDevice9* d, IDirect3DPixelShader9* s) { return d->SetPixelShader(s); }
HRESULT DevVsc(IDirect3DDevice9* d, UINT r, const float* f, UINT n) { return d->SetVertexShaderConstantF(r, f, n); }
HRESULT DevPsc(IDirect3DDevice9* d, UINT r, const float* f, UINT n) { return d->SetPixelShaderConstantF(r, f, n); }
HRESULT DevDraw(IDirect3DDevice9* d, D3DPRIMITIVETYPE t, UINT s, UINT n) { return d->DrawPrimitive(t, s, n); }
HRESULT DevVp(IDirect3DDevice9* d, const D3DVIEWPORT9* v) { return d->SetViewport(v); }

template <class T> void Rel(T*& p) {
    if (p) p->Release();
    p = nullptr;
}
IDirect3DSurface9* Surf(IDirect3DTexture9* t) {
    IDirect3DSurface9* s = nullptr;
    return t && SUCCEEDED(t->GetSurfaceLevel(0, &s)) ? s : nullptr;
}
constexpr DWORD kQuadFvf = D3DFVF_XYZRHW | D3DFVF_TEX2 | D3DFVF_TEXCOORDSIZE2(0) | D3DFVF_TEXCOORDSIZE4(1);
struct QuadV {
    float x, y, z, rhw;
    float u, v;
    float r[4];
};
} // namespace

DeviceCalls PlainCalls() {
    DeviceCalls c;
    c.SetTexture = DevSetTexture, c.SetRenderTarget = DevSetRt, c.SetDepthStencil = DevSetDs, c.GetDepthStencil = DevGetDs, c.SetVertexShader = DevSetVs;
    c.SetPixelShader = DevSetPs, c.SetVsConst = DevVsc, c.SetPsConst = DevPsc, c.DrawPrimitive = DevDraw, c.SetViewport = DevVp;
    return c;
}

const char* EntryOf(ShaderId id) {
    static const char* const kNames[kShaderCount] = {"ShadowVS", "TexelVS", "ShadowPS", "AccPS", "ComposePS", "CopyPS", "BlurRowPS", "BlurColPS", "BlockPS", "AccFastPS", "AccRlgPS", "AccRlgFastPS", "ComposeRlgPS"};
    return id >= 0 && id < kShaderCount ? kNames[id] : "";
}
bool IsVertex(ShaderId id) { return id == kShadowVs || id == kTexelVs; }

void FaceTexel(const float d[3], int face, int& f, int& x, int& y) {
    const float ax = std::fabs(d[0]), ay = std::fabs(d[1]), az = std::fabs(d[2]);
    if (ax >= ay && ax >= az) f = d[0] > 0 ? 0 : 1;
    else if (ay >= az) f = d[1] > 0 ? 2 : 3;
    else f = d[2] > 0 ? 4 : 5;
    const auto dot = [&](const float* a) { return a[0] * d[0] + a[1] * d[1] + a[2] * d[2]; };
    const float z = dot(kFaces[f][2]);
    const float u = dot(kFaces[f][0]) / z * 0.5f + 0.5f, v = -dot(kFaces[f][1]) / z * 0.5f + 0.5f;
    const float F = static_cast<float>(face);
    x = std::clamp(static_cast<int>(std::floor(u * F + 0.5f)), 0, face - 1);
    y = std::clamp(static_cast<int>(std::floor(v * F + 0.5f)), 0, face - 1);
}

bool Engine::Init(IDirect3DDevice9* dev, void* const* shaders, int face, std::string& err) {
    ReleaseAll();
    if (!dev || !shaders) {
        err = "no device or shaders";
        return false;
    }
    face_ = std::clamp(face, 64, 256);
    vs_ = static_cast<IDirect3DVertexShader9*>(shaders[kShadowVs]);
    for (int i = 0; i < kShaderCount; i++) {
        if (IsVertex(static_cast<ShaderId>(i))) continue;
        ps_[i] = static_cast<IDirect3DPixelShader9*>(shaders[i]);
        if (!ps_[i]) {
            err = std::format("shader {} missing", EntryOf(static_cast<ShaderId>(i)));
            ReleaseAll();
            return false;
        }
        ps_[i]->AddRef();
    }
    texelVs_ = static_cast<IDirect3DVertexShader9*>(shaders[kTexelVs]);
    if (!vs_ || !texelVs_) {
        vs_ = texelVs_ = nullptr;
        err = "a vertex shader is missing";
        ReleaseAll();
        return false;
    }
    vs_->AddRef();
    texelVs_->AddRef();
    const D3DVERTEXELEMENT9 texel[] = {{0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
                                       {0, 16, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
                                       {0, 32, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 1},
                                       {0, 48, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 2},
                                       {0, 64, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 3},
                                       {1, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 4},
                                       D3DDECL_END()};
    const D3DVERTEXELEMENT9 shadow[] = {{0, 0, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
                                        {0, 12, D3DDECLTYPE_FLOAT3, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_NORMAL, 0}, D3DDECL_END()};
    if (FAILED(dev->CreateVertexDeclaration(texel, &texelDecl_)) || FAILED(dev->CreateVertexDeclaration(shadow, &shadowDecl_))) {
        err = "CreateVertexDeclaration failed";
        ReleaseAll();
        return false;
    }
    ready_ = true;
    return true;
}

void Engine::ReleaseDefault() {
    Rel(atlas_);
    Rel(atlasDepth_);
    for (auto*& t : acc_) Rel(t);
    for (auto*& t : scratch_) Rel(t);
    scratchW_ = scratchH_ = 0;
    accCur_ = 0;
    Rel(tiles_);
    Rel(tilesDepth_);
    tileRows_ = 0;
    Rel(table_);
    tableRows_ = 0;
    Rel(saved_); // (a state block is a device object: made again after a reset)
}
void Engine::ReleaseAll() {
    ReleaseDefault();
    Rel(vs_);
    Rel(texelVs_);
    for (auto*& p : ps_) Rel(p);
    Rel(texelDecl_);
    Rel(shadowDecl_);
    Rel(proxy_);
    Rel(zeros_);
    Rel(tableSys_);
    tableCap_ = 0;
    zerosCount_ = 0;
    proxyTris_ = 0;
    for (auto*& s : savedRt_) Rel(s);
    Rel(savedDs_);
    ready_ = false;
}

size_t Engine::DefaultBytes() const {
    size_t b = 0;
    if (atlas_) b += static_cast<size_t>(6 * face_) * kAtlasLamps * face_ * ((atlasFmt_ == D3DFMT_G32R32F ? 8 : 4) + 4); // the atlas + D24X8
    if (acc_[0]) b += static_cast<size_t>(scratchW_) * scratchH_ * 16 * 2;
    if (scratch_[0]) b += static_cast<size_t>(scratchW_) * scratchH_ * 4 * 5;
    return b;
}

bool Engine::SetProxy(IDirect3DDevice9* dev, const RtLampProxy::Mesh& mesh, std::string& err) {
    Rel(proxy_);
    proxyTris_ = 0;
    if (mesh.tris.empty()) return true;
    // each vertex: its position and its triangle's normal (the shadow keeps how far the surface's plane passes from the lamp)
    const UINT bytes = static_cast<UINT>(mesh.tris.size() * 3 * 24);
    if (FAILED(dev->CreateVertexBuffer(bytes, D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &proxy_, nullptr)) || !proxy_) {
        err = "proxy vertex buffer not created";
        return false;
    }
    void* p = nullptr;
    if (FAILED(proxy_->Lock(0, bytes, &p, 0)) || !p) {
        Rel(proxy_);
        err = "proxy vertex buffer not locked";
        return false;
    }
    float* out = static_cast<float*>(p);
    for (const RtLampProxy::Tri& t : mesh.tris) {
        const float e1[3] = {t.v[1][0] - t.v[0][0], t.v[1][1] - t.v[0][1], t.v[1][2] - t.v[0][2]};
        const float e2[3] = {t.v[2][0] - t.v[0][0], t.v[2][1] - t.v[0][1], t.v[2][2] - t.v[0][2]};
        float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
        const float l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        for (float& c : n) c = l > 1e-12f ? c / l : 0.0f;
        for (int v = 0; v < 3; v++) {
            for (int c = 0; c < 3; c++) *out++ = t.v[v][c];
            for (int c = 0; c < 3; c++) *out++ = n[c];
        }
    }
    proxy_->Unlock();
    proxyTris_ = mesh.tris.size();
    return true;
}

bool Engine::EnsureAtlas(IDirect3DDevice9* dev, std::string& err) {
    if (atlas_ && atlasDepth_) return true;
    Rel(atlas_);
    Rel(atlasDepth_);
    const UINT w = static_cast<UINT>(6 * face_), h = static_cast<UINT>(kAtlasLamps * face_);
    // r = the distance to the lamp / far, g = how far the surface's plane passes from the lamp / far (G16R16F; G32R32F when
    // the card has no 16-bit float target)
    atlasFmt_ = D3DFMT_G16R16F;
    if (FAILED(dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_G16R16F, D3DPOOL_DEFAULT, &atlas_, nullptr)) || !atlas_) {
        atlas_ = nullptr;
        atlasFmt_ = D3DFMT_G32R32F;
        if (FAILED(dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_G32R32F, D3DPOOL_DEFAULT, &atlas_, nullptr)) || !atlas_) {
            atlas_ = nullptr;
            err = std::format("the {}x{} shadow atlas could not be created (G16R16F and G32R32F render targets refused)", w, h);
            return false;
        }
    }
    if (FAILED(dev->CreateDepthStencilSurface(w, h, D3DFMT_D24X8, D3DMULTISAMPLE_NONE, 0, TRUE, &atlasDepth_, nullptr)) || !atlasDepth_) {
        Rel(atlas_);
        err = std::format("the {}x{} D24X8 shadow depth could not be created", w, h);
        return false;
    }
    return true;
}
bool Engine::EnsureScratch(IDirect3DDevice9* dev, int w, int h, std::string& err) {
    if (acc_[0] && acc_[1] && scratch_[4] && w <= scratchW_ && h <= scratchH_) return true;
    const int nw = std::max(w, scratchW_), nh = std::max(h, scratchH_);
    for (auto*& t : acc_) Rel(t);
    for (auto*& t : scratch_) Rel(t);
    scratchW_ = scratchH_ = 0;
    for (auto*& t : acc_)
        if (FAILED(dev->CreateTexture(nw, nh, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A32B32G32R32F, D3DPOOL_DEFAULT, &t, nullptr)) || !t) {
            err = std::format("the {}x{} A32B32G32R32F sum could not be created", nw, nh);
            return false;
        }
    for (auto*& t : scratch_)
        if (FAILED(dev->CreateTexture(nw, nh, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &t, nullptr)) || !t) {
            err = std::format("the {}x{} scratch map could not be created", nw, nh);
            return false;
        }
    scratchW_ = nw, scratchH_ = nh;
    return true;
}
bool Engine::EnsureZeros(IDirect3DDevice9* dev, UINT count) {
    if (zeros_ && zerosCount_ >= count) return true;
    Rel(zeros_);
    zerosCount_ = 0;
    const UINT n = std::max<UINT>(count, 4096);
    if (FAILED(dev->CreateVertexBuffer(n * sizeof(TexelDyn), D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &zeros_, nullptr)) || !zeros_) return false;
    void* p = nullptr;
    if (FAILED(zeros_->Lock(0, 0, &p, 0)) || !p) {
        Rel(zeros_);
        return false;
    }
    std::memset(p, 0, n * sizeof(TexelDyn));
    zeros_->Unlock();
    zerosCount_ = n;
    return true;
}

bool Engine::MakeVb(IDirect3DDevice9* dev, const TexelVertex* v, UINT n, IDirect3DVertexBuffer9** out, bool defaultPool) {
    *out = nullptr;
    if (!n) return false;
    IDirect3DVertexBuffer9* vb = nullptr;
    if (FAILED(dev->CreateVertexBuffer(n * sizeof(TexelVertex), D3DUSAGE_WRITEONLY, 0, defaultPool ? D3DPOOL_DEFAULT : D3DPOOL_MANAGED, &vb, nullptr)) || !vb) return false;
    void* p = nullptr;
    if (FAILED(vb->Lock(0, 0, &p, 0)) || !p) {
        vb->Release();
        return false;
    }
    std::memcpy(p, v, n * sizeof(TexelVertex));
    vb->Unlock();
    *out = vb;
    return true;
}
bool Engine::MakeDyn(IDirect3DDevice9* dev, const TexelDyn* v, UINT n, IDirect3DVertexBuffer9** out, bool defaultPool) {
    *out = nullptr;
    if (!n) return false;
    IDirect3DVertexBuffer9* vb = nullptr;
    // (DEFAULT: dynamic, since GPU lamps for every room writes the floor mask and bounce parts again after it is made)
    if (FAILED(dev->CreateVertexBuffer(n * sizeof(TexelDyn), defaultPool ? D3DUSAGE_WRITEONLY | D3DUSAGE_DYNAMIC : D3DUSAGE_WRITEONLY, 0,
                                       defaultPool ? D3DPOOL_DEFAULT : D3DPOOL_MANAGED, &vb, nullptr)) || !vb)
        return false;
    void* p = nullptr;
    if (FAILED(vb->Lock(0, 0, &p, 0)) || !p) {
        vb->Release();
        return false;
    }
    std::memcpy(p, v, n * sizeof(TexelDyn));
    vb->Unlock();
    *out = vb;
    return true;
}
bool Engine::MakePreblur(IDirect3DDevice9* dev, int w, int h, const uint32_t* px, IDirect3DTexture9** out, bool defaultPool) {
    *out = nullptr;
    IDirect3DTexture9* sys = nullptr;
    if (FAILED(dev->CreateTexture(w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, nullptr)) || !sys) return false;
    D3DLOCKED_RECT lr{};
    if (FAILED(sys->LockRect(0, &lr, nullptr, 0)) || !lr.pBits) {
        sys->Release();
        return false;
    }
    for (int y = 0; y < h; y++) std::memcpy(static_cast<BYTE*>(lr.pBits) + static_cast<size_t>(lr.Pitch) * y, px + static_cast<size_t>(y) * w, static_cast<size_t>(w) * 4);
    sys->UnlockRect(0);
    // (14/10) MANAGED: only sampled, so it survives a device reset (Real-time outdoor lamps no longer rebuilds every story's
    // maps after an alt-tab or a resolution change); filled through UpdateTexture from the system copy as before
    IDirect3DTexture9* tex = nullptr;
    if (defaultPool) { // (filled from the system copy on the GPU: no copy of it stays in system memory)
        bool okD = SUCCEEDED(dev->CreateTexture(w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &tex, nullptr)) && tex;
        okD = okD && SUCCEEDED(dev->UpdateTexture(sys, tex));
        sys->Release();
        if (!okD) {
            if (tex) tex->Release();
            return false;
        }
        *out = tex;
        return true;
    }
    bool ok = SUCCEEDED(dev->CreateTexture(w, h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &tex, nullptr)) && tex;
    if (ok) {
        D3DLOCKED_RECT mr{};
        ok = SUCCEEDED(tex->LockRect(0, &mr, nullptr, 0)) && mr.pBits;
        if (ok) {
            for (int y = 0; y < h; y++) std::memcpy(static_cast<BYTE*>(mr.pBits) + static_cast<size_t>(mr.Pitch) * y, px + static_cast<size_t>(y) * w, static_cast<size_t>(w) * 4);
            tex->UnlockRect(0);
        }
    }
    sys->Release();
    if (!ok) {
        if (tex) tex->Release();
        return false;
    }
    *out = tex;
    return true;
}

void Engine::Begin(IDirect3DDevice9* dev) {
    if (!saved_) dev->CreateStateBlock(D3DSBT_ALL, &saved_);
    if (saved_) saved_->Capture();
    for (DWORD i = 0; i < 4; i++)
        if (FAILED(dev->GetRenderTarget(i, &savedRt_[i]))) savedRt_[i] = nullptr;
    if (!c_.GetDepthStencil || FAILED(c_.GetDepthStencil(dev, &savedDs_))) savedDs_ = nullptr;
    // nothing of ours stays bound while it becomes a render target (the swapped maps are bound by the game's draws)
    for (DWORD s = 0; s < 16; s++) c_.SetTexture(dev, s, nullptr);
    for (DWORD s = D3DVERTEXTEXTURESAMPLER0; s <= D3DVERTEXTEXTURESAMPLER3; s++) c_.SetTexture(dev, s, nullptr);
}
void Engine::End(IDirect3DDevice9* dev) {
    c_.SetRenderTarget(dev, 0, savedRt_[0]);
    for (DWORD i = 1; i < 4; i++) c_.SetRenderTarget(dev, i, savedRt_[i]);
    c_.SetDepthStencil(dev, savedDs_);
    if (saved_) saved_->Apply(); // (after the targets: setting target 0 resets the viewport)
    if (c_.StateRestored) c_.StateRestored();
    for (auto*& s : savedRt_) Rel(s);
    Rel(savedDs_);
}

bool Engine::Target(IDirect3DDevice9* dev, IDirect3DTexture9* t, int w, int h) {
    IDirect3DSurface9* s = Surf(t);
    if (!s) return false;
    const bool ok = SUCCEEDED(c_.SetRenderTarget(dev, 0, s));
    s->Release();
    for (DWORD i = 1; i < 4; i++) c_.SetRenderTarget(dev, i, nullptr);
    const D3DVIEWPORT9 vp{0, 0, static_cast<DWORD>(w), static_cast<DWORD>(h), 0.0f, 1.0f};
    c_.SetViewport(dev, &vp);
    return ok;
}

namespace {
void PassStates(IDirect3DDevice9* dev) {
    dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
    dev->SetRenderState(D3DRS_DEPTHBIAS, 0);
    dev->SetRenderState(D3DRS_SLOPESCALEDEPTHBIAS, 0);
    dev->SetRenderState(D3DRS_POINTSPRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_POINTSCALEENABLE, FALSE);
    const float one = 1.0f;
    DWORD ps;
    std::memcpy(&ps, &one, 4);
    dev->SetRenderState(D3DRS_POINTSIZE, ps);
    dev->SetRenderState(D3DRS_POINTSIZE_MIN, ps);
    dev->SetStreamSourceFreq(0, 1);
    dev->SetStreamSourceFreq(1, 1);
}
void PointSampler(IDirect3DDevice9* dev, DWORD s) {
    dev->SetSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    dev->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(s, D3DSAMP_SRGBTEXTURE, FALSE);
    dev->SetSamplerState(s, D3DSAMP_MAXMIPLEVEL, 0);
    dev->SetSamplerState(s, D3DSAMP_MIPMAPLODBIAS, 0);
}
} // namespace

bool Engine::RenderShadows(IDirect3DDevice9* dev, const float (*pos)[3], int n, float farDist, std::string& err) {
    if (!ready_ || !EnsureAtlas(dev, err)) return false;
    if (!proxy_ || !proxyTris_) n = 0;
    IDirect3DSurface9* s = Surf(atlas_);
    if (!s) {
        err = "the shadow atlas has no surface";
        return false;
    }
    c_.SetRenderTarget(dev, 0, s);
    s->Release();
    for (DWORD i = 1; i < 4; i++) c_.SetRenderTarget(dev, i, nullptr);
    c_.SetDepthStencil(dev, atlasDepth_);
    const D3DVIEWPORT9 full{0, 0, static_cast<DWORD>(6 * face_), static_cast<DWORD>(kAtlasLamps * face_), 0.0f, 1.0f};
    c_.SetViewport(dev, &full);
    PassStates(dev);
    dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xFFFFFFFF, 1.0f, 0); // 1 = nothing in the way
    if (n <= 0) return true;
    dev->SetRenderState(D3DRS_ZENABLE, TRUE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
    dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
    dev->SetVertexDeclaration(shadowDecl_);
    dev->SetStreamSource(0, proxy_, 0, 24);
    c_.SetVertexShader(dev, vs_);
    c_.SetPixelShader(dev, ps_[kShadowPs]);
    const float A = farDist / (farDist - kNear), B = -kNear * farDist / (farDist - kNear);
    const float proj[4] = {A, B, 0, 0};
    c_.SetVsConst(dev, 4, proj, 1);
    for (int k = 0; k < n && k < kAtlasLamps; k++) {
        const float lamp[4] = {pos[k][0], pos[k][1], pos[k][2], 1.0f / farDist};
        c_.SetVsConst(dev, 3, lamp, 1);
        for (int f = 0; f < 6; f++) {
            float rows[3][4];
            for (int r = 0; r < 3; r++) rows[r][0] = kFaces[f][r][0], rows[r][1] = kFaces[f][r][1], rows[r][2] = kFaces[f][r][2], rows[r][3] = 0.0f;
            c_.SetVsConst(dev, 0, &rows[0][0], 3);
            const D3DVIEWPORT9 vp{static_cast<DWORD>(f * face_), static_cast<DWORD>(k * face_), static_cast<DWORD>(face_), static_cast<DWORD>(face_), 0.0f, 1.0f};
            c_.SetViewport(dev, &vp);
            c_.DrawPrimitive(dev, D3DPT_TRIANGLELIST, 0, static_cast<UINT>(proxyTris_));
            stats_.shadowDraws++;
        }
        stats_.shadowTiles += 6;
    }
    c_.SetDepthStencil(dev, nullptr);
    return true;
}

bool Engine::Accumulate(IDirect3DDevice9* dev, const Map& m, const RtLampModel::Lamp* lamps, int n, const Params& p, bool first, std::string& err) {
    if (!ready_ || !m.vb || !m.count || m.w <= 0 || m.h <= 0) {
        err = "nothing to relight";
        return false;
    }
    if (!EnsureAtlas(dev, err) || !EnsureScratch(dev, m.w, m.h, err)) return false;
    if (!m.dyn && !EnsureZeros(dev, m.count)) {
        err = "the zero stream could not be created";
        return false;
    }
    n = std::clamp(n, 0, kAtlasLamps);
    const int dst = first ? 0 : 1 - accCur_;
    if (!Target(dev, acc_[dst], m.w, m.h)) {
        err = "the sum could not be targeted";
        return false;
    }
    c_.SetDepthStencil(dev, nullptr);
    PassStates(dev);
    float c[101][4] = {};
    for (int i = 0; i < n; i++) { // one array per row kind (the HLSL's gPos .. gMisc), indexed by the lamp
        float rows[5][4];
        GpuLampLaw::Rows(lamps[i].law, rows);
        for (int r = 0; r < 5; r++) std::memcpy(c[r * 16 + i], rows[r], sizeof rows[r]);
        c[80 + i][0] = static_cast<float>(i); // its atlas tile
        c[80 + i][1] = lamps[i].mult;
    }
    c[96][0] = static_cast<float>(n), c[96][1] = p.thr, c[96][2] = p.fade ? 1.0f : 0.0f, c[96][3] = p.reachOn ? 1.0f : 0.0f;
    c[97][0] = p.radius, c[97][1] = static_cast<float>(face_), c[97][2] = 1.0f / static_cast<float>(6 * face_), c[97][3] = 1.0f / static_cast<float>(kAtlasLamps * face_);
    c[98][0] = p.aa, c[98][1] = p.normalOffset, c[98][2] = p.biasM, c[98][3] = p.biasTexels;
    c[99][0] = 1.0f / p.farDist, c[99][1] = p.farDist, c[99][2] = 1.0f / static_cast<float>(scratchW_), c[99][3] = 1.0f / static_cast<float>(scratchH_);
    c[100][0] = first ? 1.0f : 0.0f, c[100][1] = p.mask ? 1.0f : 0.0f, c[100][2] = p.nearBlocker, c[100][3] = static_cast<float>(p.debugLamp);
    c_.SetPsConst(dev, 0, &c[0][0], 101);
    c_.SetTexture(dev, 0, atlas_);
    c_.SetTexture(dev, 1, first ? nullptr : acc_[accCur_]);
    PointSampler(dev, 0);
    PointSampler(dev, 1);
    dev->SetVertexDeclaration(texelDecl_);
    dev->SetStreamSource(0, m.vb, 0, sizeof(TexelVertex));
    dev->SetStreamSource(1, m.dyn ? m.dyn : zeros_, 0, sizeof(TexelDyn));
    TexelTarget(dev, m.w, m.h);
    c_.SetPixelShader(dev, ps_[p.fast ? kAccFastPs : kAccPs]);
    const HRESULT hr = c_.DrawPrimitive(dev, D3DPT_POINTLIST, 0, m.count);
    c_.SetTexture(dev, 0, nullptr);
    c_.SetTexture(dev, 1, nullptr);
    dev->SetStreamSource(1, nullptr, 0, 0);
    c_.SetVertexShader(dev, nullptr);
    stats_.accDraws++;
    accCur_ = dst;
    if (FAILED(hr)) {
        err = std::format("the sum's draw failed ({:08X})", static_cast<unsigned>(hr));
        return false;
    }
    return true;
}

// The texel points' vertex shader and its target size (the viewport is the map: w x h)
void Engine::TexelTarget(IDirect3DDevice9* dev, int w, int h) {
    const float t[4] = {2.0f / static_cast<float>(w), -2.0f / static_cast<float>(h), 0, 0};
    c_.SetVsConst(dev, 0, t, 1);
    c_.SetVertexShader(dev, texelVs_);
}
void Engine::Quad(IDirect3DDevice9* dev, int w, int h, float u1, float v1) {
    const float x0 = -0.5f, y0 = -0.5f, x1 = static_cast<float>(w) - 0.5f, y1 = static_cast<float>(h) - 0.5f;
    const QuadV q[4] = {{x0, y0, 0, 1, 0, 0, {}}, {x1, y0, 0, 1, u1, 0, {}}, {x0, y1, 0, 1, 0, v1, {}}, {x1, y1, 0, 1, u1, v1, {}}};
    dev->SetFVF(kQuadFvf);
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(QuadV));
}
void Engine::Blocks(IDirect3DDevice9* dev, const Map& m) {
    std::vector<QuadV> v;
    v.reserve(m.blocks.size() * 6);
    for (const Block& b : m.blocks) {
        const float x0 = static_cast<float>(b.x0) - 0.5f, y0 = static_cast<float>(b.y0) - 0.5f, x1 = static_cast<float>(b.x1) + 0.5f, y1 = static_cast<float>(b.y1) + 0.5f;
        const float r[4] = {static_cast<float>(b.x0), static_cast<float>(b.y0), static_cast<float>(b.x1), static_cast<float>(b.y1)};
        const auto P = [&](float x, float y) {
            QuadV q{x, y, 0, 1, x + 0.5f, y + 0.5f, {r[0], r[1], r[2], r[3]}};
            v.push_back(q);
        };
        P(x0, y0), P(x1, y0), P(x0, y1), P(x1, y0), P(x1, y1), P(x0, y1);
    }
    if (v.empty()) return;
    dev->SetFVF(kQuadFvf);
    // DrawPrimitiveUP: at most 65535 primitives a call on some drivers
    for (size_t at = 0; at < v.size(); at += 3 * 60000) {
        const UINT tris = static_cast<UINT>(std::min<size_t>((v.size() - at) / 3, 60000));
        dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, tris, v.data() + at, sizeof(QuadV));
    }
    stats_.blurDraws++;
}

bool Engine::Compose(IDirect3DDevice9* dev, const Map& m, IDirect3DBaseTexture9* game, IDirect3DTexture9* out, const Params& p, std::string& err,
                     IDirect3DTexture9* scale) {
    return ComposeImpl(dev, m, game, out, p, kComposePs, err, scale);
}
bool Engine::ComposeRlg(IDirect3DDevice9* dev, const Map& m, IDirect3DBaseTexture9* game, IDirect3DTexture9* out, const Params& p, std::string& err) {
    if (!table_ || !tableRows_) {
        err = "no room table";
        return false;
    }
    return ComposeImpl(dev, m, game, out, p, kComposeRlgPs, err);
}
bool Engine::ComposeImpl(IDirect3DDevice9* dev, const Map& m, IDirect3DBaseTexture9* game, IDirect3DTexture9* out, const Params& p, ShaderId psId, std::string& err,
                         IDirect3DTexture9* scale) {
    if (!ready_ || !out || !m.vb || !m.count) {
        err = "nothing to compose";
        return false;
    }
    if (!EnsureScratch(dev, m.w, m.h, err)) return false;
    if (m.blur && (!m.preblur || m.blocks.empty())) {
        err = "a class-2 wall atlas without its blocks";
        return false;
    }
    if (!m.dyn && !EnsureZeros(dev, m.count)) {
        err = "the zero stream could not be created";
        return false;
    }
    c_.SetDepthStencil(dev, nullptr);
    PassStates(dev);
    PointSampler(dev, 0);
    PointSampler(dev, 1);
    c_.SetVertexShader(dev, nullptr);
    if (m.blur || psId != kComposePs) scale = nullptr; // (the scale follows no blur: the walls' draws do not read it)
    // 0. the outdoor scale: 0 (x 1) where no texel of the record lands
    if (scale) {
        if (!Target(dev, scale, m.w, m.h)) {
            err = "the outdoor scale could not be targeted";
            return false;
        }
        dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1.0f, 0);
    }
    // 1. the game's map as it is (every room of the story writes it; room 0's texels follow)
    if (!Target(dev, out, m.w, m.h)) {
        err = "the relit map could not be targeted";
        return false;
    }
    if (game) {
        c_.SetPixelShader(dev, ps_[kCopyPs]);
        c_.SetTexture(dev, 0, game);
        Quad(dev, m.w, m.h);
        c_.SetTexture(dev, 0, nullptr);
    } else
        dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1.0f, 0);
    // 2. the texels: straight into the map, or (a blurred atlas) into the copy of the blocks before the blur
    IDirect3DTexture9* target = out;
    if (m.blur) {
        target = scratch_[0];
        if (!Target(dev, target, m.w, m.h)) {
            err = "the map before the blur could not be targeted";
            return false;
        }
        c_.SetPixelShader(dev, ps_[kCopyPs]);
        c_.SetTexture(dev, 0, m.preblur);
        Quad(dev, m.w, m.h);
        c_.SetTexture(dev, 0, nullptr);
    }
    {
        float c[100][4] = {};
        c[99][2] = 1.0f / static_cast<float>(scratchW_), c[99][3] = 1.0f / static_cast<float>(scratchH_);
        c_.SetPsConst(dev, 99, c[99], 1);
        const float flags[4] = {p.maskAllGpu ? 1.0f : 0.0f, 0, 0, 0};
        c_.SetPsConst(dev, 101, flags, 1);
    }
    c_.SetPixelShader(dev, ps_[psId]);
    c_.SetTexture(dev, 1, acc_[accCur_]);
    if (psId == kComposeRlgPs) {
        const float t[4] = {1.0f / static_cast<float>(kTableCols), 1.0f / static_cast<float>(tableRows_), 0, 0};
        c_.SetPsConst(dev, 102, t, 1);
        c_.SetTexture(dev, 2, table_);
        PointSampler(dev, 2);
    }
    TexelTarget(dev, m.w, m.h);
    IDirect3DSurface9* scaleSurf = scale ? Surf(scale) : nullptr;
    if (scaleSurf) {
        if (FAILED(c_.SetRenderTarget(dev, 1, scaleSurf))) {
            scaleSurf->Release();
            err = "the outdoor scale could not be a second target";
            return false;
        }
        dev->SetRenderState(D3DRS_COLORWRITEENABLE1, 0xF);
    }
    dev->SetVertexDeclaration(texelDecl_);
    dev->SetStreamSource(0, m.vb, 0, sizeof(TexelVertex));
    dev->SetStreamSource(1, m.dyn ? m.dyn : zeros_, 0, sizeof(TexelDyn));
    const HRESULT hr = c_.DrawPrimitive(dev, D3DPT_POINTLIST, 0, m.count);
    if (scaleSurf) {
        c_.SetRenderTarget(dev, 1, nullptr);
        scaleSurf->Release();
    }
    dev->SetStreamSource(1, nullptr, 0, 0);
    c_.SetTexture(dev, 1, nullptr);
    c_.SetTexture(dev, 2, nullptr);
    c_.SetVertexShader(dev, nullptr);
    stats_.composeDraws++;
    if (FAILED(hr)) {
        err = std::format("the texels' draw failed ({:08X})", static_cast<unsigned>(hr));
        return false;
    }
    if (!m.blur) return true;
    // 3. the game's wall blur: [passes] x (rows, then columns), and apart the rows only (each block's first and last rows)
    const float inv[4] = {1.0f / static_cast<float>(scratchW_), 1.0f / static_cast<float>(scratchH_), 0, 0};
    c_.SetPsConst(dev, 0, inv, 1);
    const auto pass = [&](IDirect3DTexture9* src, IDirect3DTexture9* dst, ShaderId ps) {
        if (!Target(dev, dst, m.w, m.h)) return false;
        c_.SetPixelShader(dev, ps_[ps]);
        c_.SetTexture(dev, 0, src);
        Blocks(dev, m);
        c_.SetTexture(dev, 0, nullptr);
        return true;
    };
    IDirect3DTexture9* rows = scratch_[0];
    for (UINT k = 0; k < m.passes; k++) {
        IDirect3DTexture9* dst = scratch_[1 + (k & 1)];
        if (!pass(rows, dst, kBlurRowPs)) {
            err = "a blur pass could not be targeted";
            return false;
        }
        rows = dst;
    }
    IDirect3DTexture9* full = scratch_[0];
    for (UINT k = 0; k < m.passes; k++) {
        if (!pass(full, scratch_[3], kBlurRowPs) || !pass(scratch_[3], scratch_[4], kBlurColPs)) {
            err = "a blur pass could not be targeted";
            return false;
        }
        full = scratch_[4];
    }
    if (!Target(dev, out, m.w, m.h)) {
        err = "the relit map could not be targeted";
        return false;
    }
    c_.SetPixelShader(dev, ps_[kBlockPs]);
    c_.SetTexture(dev, 0, full);
    c_.SetTexture(dev, 1, rows);
    Blocks(dev, m);
    c_.SetTexture(dev, 0, nullptr);
    c_.SetTexture(dev, 1, nullptr);
    return true;
}

// ---- GPU lamps for every room ----
bool Engine::EnsureTiles(IDirect3DDevice9* dev, int rows, std::string& err) {
    rows = std::clamp(rows, 1, 64);
    if (tiles_ && tilesDepth_ && tileRows_ == rows) return true;
    Rel(tiles_);
    Rel(tilesDepth_);
    tileRows_ = 0;
    const UINT w = static_cast<UINT>(6 * face_), h = static_cast<UINT>(rows * face_);
    tilesFmt_ = D3DFMT_G16R16F;
    if (FAILED(dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_G16R16F, D3DPOOL_DEFAULT, &tiles_, nullptr)) || !tiles_) {
        tiles_ = nullptr;
        tilesFmt_ = D3DFMT_G32R32F;
        if (FAILED(dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_G32R32F, D3DPOOL_DEFAULT, &tiles_, nullptr)) || !tiles_) {
            tiles_ = nullptr;
            err = std::format("the {}x{} kept shadow atlas could not be created", w, h);
            return false;
        }
    }
    if (FAILED(dev->CreateDepthStencilSurface(w, h, D3DFMT_D24X8, D3DMULTISAMPLE_NONE, 0, TRUE, &tilesDepth_, nullptr)) || !tilesDepth_) {
        Rel(tiles_);
        err = std::format("the {}x{} D24X8 depth of the kept shadow atlas could not be created", w, h);
        return false;
    }
    tileRows_ = rows;
    return true;
}
bool Engine::RenderShadowTile(IDirect3DDevice9* dev, const float pos[3], int tile, float farDist, std::string& err) {
    return RenderShadowTileRanges(dev, pos, tile, farDist, {{0u, static_cast<UINT>(proxyTris_)}}, err);
}
bool Engine::RenderShadowTileRanges(IDirect3DDevice9* dev, const float pos[3], int tile, float farDist, const std::vector<std::pair<UINT, UINT>>& ranges, std::string& err) {
    if (!ready_ || !tiles_ || !tilesDepth_ || tile < 0 || tile >= tileRows_) {
        err = "the kept shadow atlas is not ready";
        return false;
    }
    IDirect3DSurface9* s = Surf(tiles_);
    if (!s) {
        err = "the kept shadow atlas has no surface";
        return false;
    }
    c_.SetRenderTarget(dev, 0, s);
    s->Release();
    for (DWORD i = 1; i < 4; i++) c_.SetRenderTarget(dev, i, nullptr);
    c_.SetDepthStencil(dev, tilesDepth_);
    PassStates(dev);
    // the tile's row: cleared to "nothing in the way" (Clear honours the viewport)
    const D3DVIEWPORT9 row{0, static_cast<DWORD>(tile * face_), static_cast<DWORD>(6 * face_), static_cast<DWORD>(face_), 0.0f, 1.0f};
    c_.SetViewport(dev, &row);
    dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0xFFFFFFFF, 1.0f, 0);
    if (proxy_ && proxyTris_) {
        dev->SetRenderState(D3DRS_ZENABLE, TRUE);
        dev->SetRenderState(D3DRS_ZWRITEENABLE, TRUE);
        dev->SetRenderState(D3DRS_ZFUNC, D3DCMP_LESSEQUAL);
        dev->SetVertexDeclaration(shadowDecl_);
        dev->SetStreamSource(0, proxy_, 0, 24);
        c_.SetVertexShader(dev, vs_);
        c_.SetPixelShader(dev, ps_[kShadowPs]);
        const float A = farDist / (farDist - kNear), B = -kNear * farDist / (farDist - kNear);
        const float proj[4] = {A, B, 0, 0};
        c_.SetVsConst(dev, 4, proj, 1);
        const float lamp[4] = {pos[0], pos[1], pos[2], 1.0f / farDist};
        c_.SetVsConst(dev, 3, lamp, 1);
        for (int f = 0; f < 6; f++) {
            float rows[3][4];
            for (int r = 0; r < 3; r++) rows[r][0] = kFaces[f][r][0], rows[r][1] = kFaces[f][r][1], rows[r][2] = kFaces[f][r][2], rows[r][3] = 0.0f;
            c_.SetVsConst(dev, 0, &rows[0][0], 3);
            const D3DVIEWPORT9 vp{static_cast<DWORD>(f * face_), static_cast<DWORD>(tile * face_), static_cast<DWORD>(face_), static_cast<DWORD>(face_), 0.0f, 1.0f};
            c_.SetViewport(dev, &vp);
            for (const auto& [first, count] : ranges) {
                if (!count || first >= proxyTris_) continue;
                c_.DrawPrimitive(dev, D3DPT_TRIANGLELIST, first * 3, static_cast<UINT>(std::min<size_t>(count, proxyTris_ - first)));
                stats_.shadowDraws++;
            }
        }
        stats_.shadowTiles += 6;
    }
    c_.SetDepthStencil(dev, nullptr);
    return true;
}
bool Engine::SetTable(IDirect3DDevice9* dev, const float* rgba, int rows, std::string& err) {
    rows = std::max(rows, 1);
    if (!table_ || !tableSys_ || rows > tableCap_) {
        Rel(table_);
        Rel(tableSys_);
        tableCap_ = 0;
        const int cap = std::max(rows, 64);
        if (FAILED(dev->CreateTexture(kTableCols, cap, 1, 0, D3DFMT_A32B32G32R32F, D3DPOOL_SYSTEMMEM, &tableSys_, nullptr)) || !tableSys_ ||
            FAILED(dev->CreateTexture(kTableCols, cap, 1, 0, D3DFMT_A32B32G32R32F, D3DPOOL_DEFAULT, &table_, nullptr)) || !table_) {
            Rel(table_);
            Rel(tableSys_);
            err = "the room table (A32B32G32R32F) could not be created";
            return false;
        }
        tableCap_ = cap;
    }
    D3DLOCKED_RECT lr{};
    if (FAILED(tableSys_->LockRect(0, &lr, nullptr, 0)) || !lr.pBits) {
        err = "the room table could not be locked";
        return false;
    }
    for (int y = 0; y < tableCap_; y++) {
        float* dst = reinterpret_cast<float*>(static_cast<BYTE*>(lr.pBits) + static_cast<size_t>(lr.Pitch) * y);
        if (y < rows) std::memcpy(dst, rgba + static_cast<size_t>(y) * kTableCols * 4, static_cast<size_t>(kTableCols) * 16);
        else std::memset(dst, 0, static_cast<size_t>(kTableCols) * 16);
    }
    tableSys_->UnlockRect(0);
    if (FAILED(dev->UpdateTexture(tableSys_, table_))) {
        err = "the room table could not be uploaded";
        return false;
    }
    tableRows_ = tableCap_;
    return true;
}
bool Engine::AccumulateRlg(IDirect3DDevice9* dev, const Map& m, const RtLampModel::Lamp* lamps, const int* tiles, int n, const Params& p, bool first, std::string& err) {
    if (!ready_ || !m.vb || !m.count || m.w <= 0 || m.h <= 0) {
        err = "nothing to relight";
        return false;
    }
    if (!tiles_ || !table_) {
        err = "the kept shadow atlas or the room table is missing";
        return false;
    }
    if (!EnsureScratch(dev, m.w, m.h, err)) return false;
    if (!m.dyn && !EnsureZeros(dev, m.count)) {
        err = "the zero stream could not be created";
        return false;
    }
    n = std::clamp(n, 0, kAtlasLamps);
    const int dst = first ? 0 : 1 - accCur_;
    if (!Target(dev, acc_[dst], m.w, m.h)) {
        err = "the sum could not be targeted";
        return false;
    }
    c_.SetDepthStencil(dev, nullptr);
    PassStates(dev);
    float c[103][4] = {};
    for (int i = 0; i < n; i++) {
        float rows[5][4];
        GpuLampLaw::Rows(lamps[i].law, rows);
        for (int r = 0; r < 5; r++) std::memcpy(c[r * 16 + i], rows[r], sizeof rows[r]);
        c[80 + i][0] = static_cast<float>(tiles[i]);
        c[80 + i][1] = lamps[i].mult;
    }
    c[96][0] = static_cast<float>(n), c[96][1] = p.thr, c[96][2] = p.fade ? 1.0f : 0.0f, c[96][3] = p.reachOn ? 1.0f : 0.0f;
    c[97][0] = p.radius, c[97][1] = static_cast<float>(face_), c[97][2] = 1.0f / static_cast<float>(6 * face_), c[97][3] = 1.0f / static_cast<float>(tileRows_ * face_);
    c[98][0] = p.aa, c[98][1] = p.normalOffset, c[98][2] = p.biasM, c[98][3] = p.biasTexels;
    c[99][0] = 1.0f / p.farDist, c[99][1] = p.farDist, c[99][2] = 1.0f / static_cast<float>(scratchW_), c[99][3] = 1.0f / static_cast<float>(scratchH_);
    c[100][0] = first ? 1.0f : 0.0f, c[100][1] = p.mask ? 1.0f : 0.0f, c[100][2] = p.nearBlocker, c[100][3] = -1.0f;
    c[102][0] = 1.0f / static_cast<float>(kTableCols), c[102][1] = 1.0f / static_cast<float>(tableRows_);
    c_.SetPsConst(dev, 0, &c[0][0], 103);
    c_.SetTexture(dev, 0, tiles_);
    c_.SetTexture(dev, 1, first ? nullptr : acc_[accCur_]);
    c_.SetTexture(dev, 2, table_);
    PointSampler(dev, 0);
    PointSampler(dev, 1);
    PointSampler(dev, 2);
    dev->SetVertexDeclaration(texelDecl_);
    dev->SetStreamSource(0, m.vb, 0, sizeof(TexelVertex));
    dev->SetStreamSource(1, m.dyn ? m.dyn : zeros_, 0, sizeof(TexelDyn));
    TexelTarget(dev, m.w, m.h);
    c_.SetPixelShader(dev, ps_[p.fast ? kAccRlgFastPs : kAccRlgPs]);
    const HRESULT hr = c_.DrawPrimitive(dev, D3DPT_POINTLIST, 0, m.count);
    c_.SetTexture(dev, 0, nullptr);
    c_.SetTexture(dev, 1, nullptr);
    c_.SetTexture(dev, 2, nullptr);
    dev->SetStreamSource(1, nullptr, 0, 0);
    c_.SetVertexShader(dev, nullptr);
    stats_.accDraws++;
    accCur_ = dst;
    if (FAILED(hr)) {
        err = std::format("the sum's draw failed ({:08X})", static_cast<unsigned>(hr));
        return false;
    }
    return true;
}
bool Engine::SeedAcc(IDirect3DDevice9* dev, IDirect3DTexture9* from, int w, int h, std::string& err) {
    if (!ready_ || !from) {
        err = "no kept sum";
        return false;
    }
    if (!EnsureScratch(dev, w, h, err)) return false;
    if (!Target(dev, acc_[0], w, h)) {
        err = "the sum could not be targeted";
        return false;
    }
    c_.SetDepthStencil(dev, nullptr);
    PassStates(dev);
    PointSampler(dev, 0);
    c_.SetVertexShader(dev, nullptr);
    c_.SetPixelShader(dev, ps_[kCopyPs]);
    c_.SetTexture(dev, 0, from);
    Quad(dev, w, h);
    c_.SetTexture(dev, 0, nullptr);
    accCur_ = 0;
    return true;
}
bool Engine::StoreAcc(IDirect3DDevice9* dev, IDirect3DTexture9* to, int w, int h, std::string& err) {
    if (!ready_ || !to || !acc_[accCur_] || w > scratchW_ || h > scratchH_) {
        err = "no sum to keep";
        return false;
    }
    if (!Target(dev, to, w, h)) {
        err = "the kept sum could not be targeted";
        return false;
    }
    c_.SetDepthStencil(dev, nullptr);
    PassStates(dev);
    PointSampler(dev, 0);
    c_.SetVertexShader(dev, nullptr);
    c_.SetPixelShader(dev, ps_[kCopyPs]);
    c_.SetTexture(dev, 0, acc_[accCur_]);
    Quad(dev, w, h, static_cast<float>(w) / static_cast<float>(scratchW_), static_cast<float>(h) / static_cast<float>(scratchH_));
    c_.SetTexture(dev, 0, nullptr);
    return true;
}
bool Engine::ReadTexture(IDirect3DDevice9* dev, IDirect3DTexture9* t, int w, int h, std::vector<uint32_t>& out) {
    if (!t) return false;
    IDirect3DSurface9* src = Surf(t);
    IDirect3DSurface9* sys = nullptr;
    bool ok = src && SUCCEEDED(dev->CreateOffscreenPlainSurface(w, h, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, nullptr)) && sys && SUCCEEDED(dev->GetRenderTargetData(src, sys));
    if (ok) {
        D3DLOCKED_RECT lr{};
        ok = SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY));
        if (ok) {
            out.resize(static_cast<size_t>(w) * h);
            for (int y = 0; y < h; y++) std::memcpy(out.data() + static_cast<size_t>(y) * w, static_cast<const BYTE*>(lr.pBits) + static_cast<size_t>(lr.Pitch) * y, static_cast<size_t>(w) * 4);
            sys->UnlockRect();
        }
    }
    if (sys) sys->Release();
    if (src) src->Release();
    return ok;
}

bool Engine::CopyTexture(IDirect3DDevice9* dev, IDirect3DBaseTexture9* src, IDirect3DTexture9* out, int w, int h, std::string& err) {
    if (!ready_ || !src || !out || w <= 0 || h <= 0) {
        err = "nothing to copy";
        return false;
    }
    c_.SetDepthStencil(dev, nullptr);
    PassStates(dev);
    PointSampler(dev, 0);
    c_.SetVertexShader(dev, nullptr);
    if (!Target(dev, out, w, h)) {
        err = "the copy could not be targeted";
        return false;
    }
    c_.SetPixelShader(dev, ps_[kCopyPs]);
    c_.SetTexture(dev, 0, src);
    Quad(dev, w, h);
    c_.SetTexture(dev, 0, nullptr);
    return true;
}

bool Engine::ReadFloat4(IDirect3DDevice9* dev, IDirect3DTexture9* t, int w, int h, std::vector<float>& out) {
    if (!t || w <= 0 || h <= 0) return false;
    IDirect3DSurface9* src = Surf(t);
    IDirect3DSurface9* sys = nullptr;
    bool ok = src && SUCCEEDED(dev->CreateOffscreenPlainSurface(w, h, D3DFMT_A32B32G32R32F, D3DPOOL_SYSTEMMEM, &sys, nullptr)) && sys && SUCCEEDED(dev->GetRenderTargetData(src, sys));
    if (ok) {
        D3DLOCKED_RECT lr{};
        ok = SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY));
        if (ok) {
            try {
                out.resize(static_cast<size_t>(w) * h * 4);
                for (int y = 0; y < h; y++)
                    std::memcpy(out.data() + static_cast<size_t>(y) * w * 4, static_cast<const BYTE*>(lr.pBits) + static_cast<size_t>(lr.Pitch) * y, static_cast<size_t>(w) * 16);
            } catch (...) {
                ok = false;
            }
            sys->UnlockRect();
        }
    }
    if (sys) sys->Release();
    if (src) src->Release();
    return ok;
}

bool Engine::ReadSum(IDirect3DDevice9* dev, int w, int h, std::vector<float>& out) {
    if (!acc_[accCur_]) return false;
    IDirect3DSurface9* src = Surf(acc_[accCur_]);
    IDirect3DSurface9* sys = nullptr;
    bool ok = src && SUCCEEDED(dev->CreateOffscreenPlainSurface(scratchW_, scratchH_, D3DFMT_A32B32G32R32F, D3DPOOL_SYSTEMMEM, &sys, nullptr)) && sys &&
              SUCCEEDED(dev->GetRenderTargetData(src, sys));
    if (ok) {
        D3DLOCKED_RECT lr{};
        ok = SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY));
        if (ok) {
            out.resize(static_cast<size_t>(w) * h * 4);
            for (int y = 0; y < h; y++) std::memcpy(out.data() + static_cast<size_t>(y) * w * 4, static_cast<const BYTE*>(lr.pBits) + static_cast<size_t>(lr.Pitch) * y, static_cast<size_t>(w) * 16);
            sys->UnlockRect();
        }
    }
    if (sys) sys->Release();
    if (src) src->Release();
    return ok;
}

} // namespace RtLampGpu
