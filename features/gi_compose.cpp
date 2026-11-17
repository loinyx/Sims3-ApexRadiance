// Real-time GI compose pass (see gi_compose.h). Part of Apex Radiance. Credits: @loinyx

#include "gi_compose.h"
#include <algorithm>
#include <cstring>
#include <utility>

namespace GiCompose {

const char* const kPixelShader = "sampler2D shown : register(s0); sampler2D incoming : register(s1); float4 k : register(c0);\n"
                                 "float4 main(float2 uv : TEXCOORD0) : COLOR { return float4(k.rgb * lerp(tex2D(shown, uv).rgb, tex2D(incoming, uv).rgb, k.w), 0); }\n";
const char* const kScatterShader = "sampler2D src : register(s0);\n"
                                   "float4 main(float3 uv : TEXCOORD0) : COLOR { return float4(tex2D(src, uv.xy).rgb, uv.z); }\n";

namespace {
struct V {
    float x, y, z, rhw, u, v;
};
constexpr DWORD kFvf = D3DFVF_XYZRHW | D3DFVF_TEX1;
struct V3 {
    float x, y, z, rhw, u, v, w;
};
constexpr DWORD kFvf3 = D3DFVF_XYZRHW | D3DFVF_TEX1 | D3DFVF_TEXCOORDSIZE3(0);

// A dynamic DEFAULT-pool point buffer of at least n vertices of `stride` bytes, locked for n
template <class T> T* LockPoints(IDirect3DDevice9* dev, IDirect3DVertexBuffer9*& vb, UINT& verts, size_t n, DWORD fvf) {
    if (verts < n) {
        if (vb) vb->Release();
        vb = nullptr, verts = 0;
        const UINT cap = static_cast<UINT>(std::max<size_t>(n + n / 2, 4096));
        if (FAILED(dev->CreateVertexBuffer(cap * sizeof(T), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY | D3DUSAGE_POINTS, fvf, D3DPOOL_DEFAULT, &vb, nullptr))) return nullptr;
        verts = cap;
    }
    T* p = nullptr;
    return SUCCEEDED(vb->Lock(0, static_cast<UINT>(n * sizeof(T)), reinterpret_cast<void**>(&p), D3DLOCK_DISCARD)) ? p : nullptr;
}

// One point pass into `out`: the device's state kept (a state block) and its targets put back at the end; point samplers,
// no blend (the caller turns it on), one pixel a point
class Pass {
  public:
    Pass(IDirect3DDevice9* dev, IDirect3DTexture9* out, const Calls& calls, int* draws) : dev_(dev), calls_(calls), draws_(draws) {
        if (FAILED(out->GetSurfaceLevel(0, &rt_)) || FAILED(dev->CreateStateBlock(D3DSBT_ALL, &sb_))) return;
        for (DWORD i = 0; i < 4; i++) dev->GetRenderTarget(i, &oldRt_[i]);
        dev->GetDepthStencilSurface(&oldDepth_);
        dev->SetRenderTarget(0, rt_);
        for (DWORD i = 1; i < 4; i++) dev->SetRenderTarget(i, nullptr);
        dev->SetDepthStencilSurface(nullptr);
        for (auto [s, v] : {std::pair{D3DRS_ZENABLE, 0u}, {D3DRS_ZWRITEENABLE, 0u}, {D3DRS_ALPHABLENDENABLE, 0u}, {D3DRS_ALPHATESTENABLE, 0u},
                            {D3DRS_CULLMODE, static_cast<DWORD>(D3DCULL_NONE)}, {D3DRS_COLORWRITEENABLE, 0xFu}, {D3DRS_SCISSORTESTENABLE, 0u},
                            {D3DRS_STENCILENABLE, 0u}, {D3DRS_FOGENABLE, 0u}, {D3DRS_SRGBWRITEENABLE, 0u}, {D3DRS_CLIPPLANEENABLE, 0u},
                            {D3DRS_POINTSPRITEENABLE, 0u}, {D3DRS_POINTSCALEENABLE, 0u}, {D3DRS_SRCBLEND, static_cast<DWORD>(D3DBLEND_ONE)},
                            {D3DRS_DESTBLEND, static_cast<DWORD>(D3DBLEND_ONE)}, {D3DRS_BLENDOP, static_cast<DWORD>(D3DBLENDOP_ADD)}})
            dev->SetRenderState(s, v);
        const float one = 1.0f;
        DWORD pointSize;
        std::memcpy(&pointSize, &one, 4);
        dev->SetRenderState(D3DRS_POINTSIZE, pointSize);
        for (DWORD st = 0; st < 2; st++)
            for (auto [s, v] : {std::pair{D3DSAMP_MINFILTER, static_cast<DWORD>(D3DTEXF_POINT)}, {D3DSAMP_MAGFILTER, static_cast<DWORD>(D3DTEXF_POINT)},
                                {D3DSAMP_MIPFILTER, static_cast<DWORD>(D3DTEXF_NONE)}, {D3DSAMP_ADDRESSU, static_cast<DWORD>(D3DTADDRESS_CLAMP)},
                                {D3DSAMP_ADDRESSV, static_cast<DWORD>(D3DTADDRESS_CLAMP)}, {D3DSAMP_SRGBTEXTURE, 0ul}, {D3DSAMP_MAXMIPLEVEL, 0ul}})
                dev->SetSamplerState(st, s, v);
        dev->SetVertexShader(nullptr);
        began_ = SUCCEEDED(dev->BeginScene());
        ok_ = true;
    }
    ~Pass() {
        if (began_) dev_->EndScene();
        if (ok_) {
            SetTexture(0, nullptr), SetTexture(1, nullptr); // (the layers are render targets of the bridge's worker; nothing keeps them bound)
            for (DWORD i = 0; i < 4; i++) {
                if (i == 0 || oldRt_[i]) dev_->SetRenderTarget(i, oldRt_[i]);
                if (oldRt_[i]) oldRt_[i]->Release();
            }
            dev_->SetDepthStencilSurface(oldDepth_);
            if (oldDepth_) oldDepth_->Release();
            sb_->Apply();
        }
        if (sb_) sb_->Release();
        if (rt_) rt_->Release();
    }
    bool Ok() const { return ok_; }
    HRESULT SetTexture(DWORD st, IDirect3DBaseTexture9* t) { return calls_.setTexture ? calls_.setTexture(dev_, st, t) : dev_->SetTexture(st, t); }
    HRESULT Draw(UINT first, UINT count) {
        if (draws_) ++*draws_;
        return calls_.drawPrimitive ? calls_.drawPrimitive(dev_, D3DPT_POINTLIST, first, count) : dev_->DrawPrimitive(D3DPT_POINTLIST, first, count);
    }

  private:
    IDirect3DDevice9* dev_;
    const Calls& calls_;
    int* draws_;
    IDirect3DSurface9* rt_ = nullptr;
    IDirect3DStateBlock9* sb_ = nullptr;
    IDirect3DSurface9* oldRt_[4] = {};
    IDirect3DSurface9* oldDepth_ = nullptr;
    bool began_ = false, ok_ = false;
};
} // namespace

void Composer::Release() {
    if (vb_) vb_->Release();
    if (vb3_) vb3_->Release();
    vb_ = vb3_ = nullptr, vbVerts_ = vb3Verts_ = 0;
}

bool Composer::Compose(IDirect3DDevice9* dev, IDirect3DPixelShader9* ps, IDirect3DTexture9* out, uint32_t width, uint32_t height, const std::vector<uint32_t>& clear,
                       const std::vector<LampDraw>& lamps, const Calls& calls, int* draws) {
    if (draws) *draws = 0;
    if (!dev || !ps || !out || !width || !height || clear.empty()) return clear.empty();
    size_t total = clear.size();
    for (const LampDraw& l : lamps) total += l.pixels.size();
    if (total > 0x1000000) return false;
    V* p = LockPoints<V>(dev, vb_, vbVerts_, total, kFvf);
    if (!p) return false;
    const float fw = static_cast<float>(width), fh = static_cast<float>(height);
    const auto put = [&](uint32_t px) { // a point at the pixel's centre, sampling the same pixel of the layers
        const float x = static_cast<float>(px % width), y = static_cast<float>(px / width);
        *p++ = {x, y, 0.0f, 1.0f, (x + 0.5f) / fw, (y + 0.5f) / fh};
    };
    for (uint32_t px : clear) put(px);
    for (const LampDraw& l : lamps)
        for (uint32_t px : l.pixels) put(px);
    vb_->Unlock();

    Pass pass(dev, out, calls, draws);
    if (!pass.Ok()) return false;
    dev->SetPixelShader(ps);
    dev->SetFVF(kFvf);
    dev->SetStreamSource(0, vb_, 0, sizeof(V));
    bool ok = true;
    // the composed pixels to 0 (no texture bound reads 0; colour 0)
    const float zero[4] = {};
    pass.SetTexture(0, nullptr), pass.SetTexture(1, nullptr);
    dev->SetPixelShaderConstantF(0, zero, 1);
    ok &= SUCCEEDED(pass.Draw(0, static_cast<UINT>(clear.size())));
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    UINT first = static_cast<UINT>(clear.size());
    for (const LampDraw& l : lamps) {
        if (!l.pixels.empty() && l.shown) {
            const float k[4] = {l.colour[0], l.colour[1], l.colour[2], l.incoming ? l.blend : 0.0f};
            pass.SetTexture(0, l.shown), pass.SetTexture(1, l.incoming ? l.incoming : l.shown);
            dev->SetPixelShaderConstantF(0, k, 1);
            ok &= SUCCEEDED(pass.Draw(first, static_cast<UINT>(l.pixels.size())));
        }
        first += static_cast<UINT>(l.pixels.size());
    }
    return ok;
}

bool Composer::Scatter(IDirect3DDevice9* dev, IDirect3DPixelShader9* ps, IDirect3DTexture9* src, uint32_t srcWidth, uint32_t srcHeight, IDirect3DTexture9* out,
                       uint32_t width, const std::vector<ScatterPoint>& points, const Calls& calls, int* draws) {
    if (draws) *draws = 0;
    if (!dev || !ps || !src || !out || !srcWidth || !srcHeight || !width || points.size() > 0x1000000) return false;
    IDirect3DSurface9* s = nullptr;
    if (FAILED(out->GetSurfaceLevel(0, &s))) return false;
    const bool cleared = SUCCEEDED(dev->ColorFill(s, nullptr, 0)); // (texels no point writes: no light, not ready)
    s->Release();
    if (!cleared) return false;
    if (points.empty()) return true;
    V3* p = LockPoints<V3>(dev, vb3_, vb3Verts_, points.size(), kFvf3);
    if (!p) return false;
    const float fw = static_cast<float>(srcWidth), fh = static_cast<float>(srcHeight);
    for (const ScatterPoint& q : points)
        *p++ = {static_cast<float>(q.dst % width), static_cast<float>(q.dst / width), 0.0f, 1.0f, (static_cast<float>(q.src % srcWidth) + 0.5f) / fw,
                (static_cast<float>(q.src / srcWidth) + 0.5f) / fh, q.alpha};
    vb3_->Unlock();
    Pass pass(dev, out, calls, draws);
    if (!pass.Ok()) return false;
    dev->SetPixelShader(ps);
    dev->SetFVF(kFvf3);
    dev->SetStreamSource(0, vb3_, 0, sizeof(V3));
    pass.SetTexture(0, src);
    return SUCCEEDED(pass.Draw(0, static_cast<UINT>(points.size())));
}

void ComposeCpu(std::vector<float>& out, const std::vector<uint32_t>& clear, const std::vector<CpuLamp>& lamps) {
    for (uint32_t px : clear) std::fill_n(out.begin() + static_cast<size_t>(px) * 4, 4, 0.0f);
    for (const CpuLamp& l : lamps)
        for (uint32_t px : l.pixels)
            for (int c = 0; c < 3; c++) {
                const float a = l.shown[static_cast<size_t>(px) * 4 + c], b = l.incoming ? l.incoming[static_cast<size_t>(px) * 4 + c] : a;
                out[static_cast<size_t>(px) * 4 + c] += l.colour[c] * (a + (b - a) * (l.incoming ? l.blend : 0.0f));
            }
}

} // namespace GiCompose
