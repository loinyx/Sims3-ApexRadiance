// Real-time GI compose pass (see gi_compose.h). Part of Apex Radiance. Credits: @loinyx

#include "gi_compose.h"
#include <algorithm>
#include <cstring>
#include <utility>

namespace GiCompose {

const char* const kPixelShader = "sampler2D shown : register(s0); sampler2D incoming : register(s1); float4 k : register(c0);\n"
                                 "float4 main(float2 uv : TEXCOORD0) : COLOR { return float4(k.rgb * lerp(tex2D(shown, uv).rgb, tex2D(incoming, uv).rgb, k.w), 0); }\n";

namespace {
struct V {
    float x, y, z, rhw, u, v;
};
constexpr DWORD kFvf = D3DFVF_XYZRHW | D3DFVF_TEX1;
} // namespace

void Composer::Release() {
    if (vb_) vb_->Release();
    vb_ = nullptr, vbVerts_ = 0;
}

bool Composer::Compose(IDirect3DDevice9* dev, IDirect3DPixelShader9* ps, IDirect3DTexture9* out, uint32_t width, uint32_t height, const std::vector<uint32_t>& clear,
                       const std::vector<LampDraw>& lamps, const Calls& calls, int* draws) {
    if (draws) *draws = 0;
    if (!dev || !ps || !out || !width || !height || clear.empty()) return clear.empty();
    size_t total = clear.size();
    for (const LampDraw& l : lamps) total += l.pixels.size();
    if (total > 0x1000000) return false;
    if (vbVerts_ < total) {
        Release();
        const UINT n = static_cast<UINT>(std::max<size_t>(total + total / 2, 4096));
        if (FAILED(dev->CreateVertexBuffer(n * sizeof(V), D3DUSAGE_DYNAMIC | D3DUSAGE_WRITEONLY | D3DUSAGE_POINTS, kFvf, D3DPOOL_DEFAULT, &vb_, nullptr))) return false;
        vbVerts_ = n;
    }
    V* p = nullptr;
    if (FAILED(vb_->Lock(0, static_cast<UINT>(total * sizeof(V)), reinterpret_cast<void**>(&p), D3DLOCK_DISCARD))) return false;
    const float fw = static_cast<float>(width), fh = static_cast<float>(height);
    const auto put = [&](uint32_t px) { // a point at the pixel's centre, sampling the same pixel of the layers
        const float x = static_cast<float>(px % width), y = static_cast<float>(px / width);
        *p++ = {x, y, 0.0f, 1.0f, (x + 0.5f) / fw, (y + 0.5f) / fh};
    };
    for (uint32_t px : clear) put(px);
    for (const LampDraw& l : lamps)
        for (uint32_t px : l.pixels) put(px);
    vb_->Unlock();

    IDirect3DSurface9* rt = nullptr;
    IDirect3DStateBlock9* sb = nullptr;
    if (FAILED(out->GetSurfaceLevel(0, &rt)) || FAILED(dev->CreateStateBlock(D3DSBT_ALL, &sb))) {
        if (rt) rt->Release();
        return false;
    }
    IDirect3DSurface9* oldRt[4] = {};
    IDirect3DSurface9* oldDepth = nullptr;
    for (DWORD i = 0; i < 4; i++) dev->GetRenderTarget(i, &oldRt[i]);
    dev->GetDepthStencilSurface(&oldDepth);
    dev->SetRenderTarget(0, rt);
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
    const auto setTexture = [&](DWORD st, IDirect3DBaseTexture9* t) { return calls.setTexture ? calls.setTexture(dev, st, t) : dev->SetTexture(st, t); };
    const auto draw = [&](UINT first, UINT count) {
        if (draws) ++*draws;
        return calls.drawPrimitive ? calls.drawPrimitive(dev, D3DPT_POINTLIST, first, count) : dev->DrawPrimitive(D3DPT_POINTLIST, first, count);
    };
    dev->SetVertexShader(nullptr);
    dev->SetPixelShader(ps);
    dev->SetFVF(kFvf);
    dev->SetStreamSource(0, vb_, 0, sizeof(V));
    bool ok = true;
    const bool began = SUCCEEDED(dev->BeginScene());
    // the composed pixels to 0 (no texture bound reads 0; colour 0)
    const float zero[4] = {};
    setTexture(0, nullptr), setTexture(1, nullptr);
    dev->SetPixelShaderConstantF(0, zero, 1);
    ok &= SUCCEEDED(draw(0, static_cast<UINT>(clear.size())));
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
    UINT first = static_cast<UINT>(clear.size());
    for (const LampDraw& l : lamps) {
        if (!l.pixels.empty() && l.shown) {
            const float k[4] = {l.colour[0], l.colour[1], l.colour[2], l.incoming ? l.blend : 0.0f};
            setTexture(0, l.shown), setTexture(1, l.incoming ? l.incoming : l.shown);
            dev->SetPixelShaderConstantF(0, k, 1);
            ok &= SUCCEEDED(draw(first, static_cast<UINT>(l.pixels.size())));
        }
        first += static_cast<UINT>(l.pixels.size());
    }
    if (began) dev->EndScene();
    setTexture(0, nullptr), setTexture(1, nullptr); // (the layers are render targets of the bridge's worker; nothing keeps them bound)
    for (DWORD i = 0; i < 4; i++) {
        if (i == 0 || oldRt[i]) dev->SetRenderTarget(i, oldRt[i]);
        if (oldRt[i]) oldRt[i]->Release();
    }
    dev->SetDepthStencilSurface(oldDepth);
    if (oldDepth) oldDepth->Release();
    sb->Apply();
    sb->Release();
    rt->Release();
    return ok;
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
