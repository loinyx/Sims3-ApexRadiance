// Looks: the D3D9 runner of the ported ReShade shaders (third_party/reshade_shaders). Pure D3D9 + STL.

#include "looks_runner.h"
#include "third_party/reshade_shaders/reshade_shaders_hlsl.h"

#include <algorithm>
#include <cstring>

namespace Looks {

// ============================================================================================== table
int ShaderCount() { return ReShadeShaders::kShaderCount; }
const LooksShader& Shader(int index) { return ReShadeShaders::kShaders[index]; }

static bool IEquals(const std::string& a, const std::string& b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return false;
    return true;
}

int FindShader(const std::string& technique, const std::string& file)
{
    // GShade renamed SweetFX's HDR technique of FakeHDR.fx to FakeHDR; its presets still name it so
    const std::string& t = technique == "FakeHDR" && IEquals(file, "FakeHDR.fx") ? std::string("HDR") : technique;
    for (int i = 0; i < ShaderCount(); ++i)
        if (t == Shader(i).technique && IEquals(file, Shader(i).file)) return i;
    return -1;
}

static void Append(std::string& out, const char* const* pieces)
{
    for (; pieces && *pieces; ++pieces) out += *pieces;
}

std::string FullSource(int shader)
{
    std::string s;
    Append(s, ReShadeShaders::kCommon);
    if (shader < 0)
    {
        // The runner's own copy / mix pass
        s += "\nfloat4 gs_copy(float2 uv : TEXCOORD0) : COLOR0 { return float4(tex2D(BackBuffer, uv).rgb, 1.0); }\n";
        return s;
    }
    Append(s, Shader(shader).src);
    return s;
}

// ============================================================================================== runner
// Device state the runner changes. The caller saves these before Run and restores them after.
const D3DRENDERSTATETYPE kTouchedRenderStates[] = {
    D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_ALPHATESTENABLE, D3DRS_STENCILENABLE,
    D3DRS_CULLMODE, D3DRS_SCISSORTESTENABLE, D3DRS_FOGENABLE, D3DRS_SRGBWRITEENABLE, D3DRS_CLIPPLANEENABLE,
    D3DRS_COLORWRITEENABLE, D3DRS_SRCBLEND, D3DRS_DESTBLEND, D3DRS_BLENDOP, D3DRS_BLENDFACTOR,
    D3DRS_SEPARATEALPHABLENDENABLE,
};
const int kTouchedRenderStateCount = sizeof(kTouchedRenderStates) / sizeof(kTouchedRenderStates[0]);
const int kTouchedSamplers = 6; // s0..s5: texture + the sampler states below
const D3DSAMPLERSTATETYPE kTouchedSamplerStates[] = {
    D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER, D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV,
    D3DSAMP_SRGBTEXTURE, D3DSAMP_MAXMIPLEVEL, D3DSAMP_MIPMAPLODBIAS,
};
const int kTouchedSamplerStateCount = sizeof(kTouchedSamplerStates) / sizeof(kTouchedSamplerStates[0]);
// Also: pixel shader, vertex shader (null), FVF, render target 0 (and its viewport), PS constants c0..c31.

static size_t BytesPerPixel(D3DFORMAT f)
{
    switch (f)
    {
    case D3DFMT_R16F: return 2;
    case D3DFMT_G16R16F: return 4;
    case D3DFMT_A16B16G16R16F: return 8;
    default: return 4;
    }
}

bool Runner::Ensure(IDirect3DDevice9* dev, Tex& t, UINT w, UINT h, D3DFORMAT fmt)
{
    if (t.tex && t.w == w && t.h == h && t.fmt == fmt) return true;
    Free(t);
    if (FAILED(dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, fmt, D3DPOOL_DEFAULT, &t.tex, nullptr)))
    {
        t.tex = nullptr;
        return false;
    }
    t.tex->GetSurfaceLevel(0, &t.surf);
    t.w = w; t.h = h; t.fmt = fmt;
    size_t bytes = (size_t)w * h * BytesPerPixel(fmt);
    gpuBytes_ += bytes;
    return t.surf != nullptr;
}

void Runner::Free(Tex& t)
{
    if (t.tex)
    {
        const size_t used = (size_t)t.w * t.h * BytesPerPixel(t.fmt);
        gpuBytes_ = gpuBytes_ >= used ? gpuBytes_ - used : 0;
    }
    if (t.surf) t.surf->Release();
    if (t.tex) t.tex->Release();
    t = Tex{};
}

void Runner::Release()
{
    for (Tex* t : { &ping_, &pong_, &asTex_ })
        Free(*t);
    for (auto& kv : ps_) if (kv.second) kv.second->Release();
    ps_.clear();
    if (copyPs_) { copyPs_->Release(); copyPs_ = nullptr; }
    gpuBytes_ = 0;
}

IDirect3DPixelShader9* Runner::Ps(IDirect3DDevice9* dev, int shader, int entry)
{
    long long key = ((long long)shader << 16) | (entry & 0xFFFF);
    auto it = ps_.find(key);
    if (it != ps_.end()) return it->second;
    IDirect3DPixelShader9* ps = factory_ ? factory_(dev, shader, entry) : nullptr;
    if (ps) ps_[key] = ps; // a failed shader is asked for again next frame (Apex may still be compiling)
    return ps;
}

IDirect3DPixelShader9* Runner::CopyPs(IDirect3DDevice9* dev)
{
    if (!copyPs_ && factory_) copyPs_ = factory_(dev, -1, 0);
    return copyPs_;
}

void Runner::Draw(IDirect3DDevice9* dev, IDirect3DPixelShader9* ps, IDirect3DSurface9* rt, UINT w, UINT h)
{
    struct QuadVertex { float x, y, z, rhw, u, v; };
    const float x0 = -0.5f, y0 = -0.5f, x1 = (float)w - 0.5f, y1 = (float)h - 0.5f;
    const QuadVertex quad[4] = {
        { x0, y0, 0.0f, 1.0f, 0.0f, 0.0f },
        { x1, y0, 0.0f, 1.0f, 1.0f, 0.0f },
        { x0, y1, 0.0f, 1.0f, 0.0f, 1.0f },
        { x1, y1, 0.0f, 1.0f, 1.0f, 1.0f },
    };
    if (rt != curRt_) // a new render target also resets the viewport to its size
    {
        if (setRT_) setRT_(dev, 0, rt); else dev->SetRenderTarget(0, rt);
        curRt_ = rt;
        if (rt == target_) // the last step straight into the target: RGB only, like the write-back it replaces
            dev->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE);
    }
    if (!constsSent_)
    {
        dev->SetPixelShaderConstantF(0, buf_, 1);
        dev->SetPixelShaderConstantF(1, aux_, 1);
        dev->SetPixelShaderConstantF(2, depthC_, 1);
        memcpy(sentAux_, aux_, sizeof aux_);
        constsSent_ = true;
    }
    else if (memcmp(sentAux_, aux_, sizeof aux_) != 0)
    {
        dev->SetPixelShaderConstantF(1, aux_, 1);
        memcpy(sentAux_, aux_, sizeof aux_);
    }
    if (ps != curPs_) { dev->SetPixelShader(ps); curPs_ = ps; }
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(QuadVertex));
}

void Runner::SetParams(IDirect3DDevice9* dev, int shader, const float* values)
{
    const LooksShader& d = Shader(shader);
    const unsigned long long now = GetTickCount64();
    for (int i = 0; i < d.paramCount; ++i)
    {
        const LooksParam& p = d.params[i];
        if (p.kind != LooksParamKind::Source)
        {
            // consecutive registers with consecutive values go in one call
            int n = 1;
            while (i + n < d.paramCount && d.params[i + n].kind != LooksParamKind::Source && d.params[i + n].reg == p.reg + n) ++n;
            dev->SetPixelShaderConstantF(p.reg, values + i * 4, n);
            i += n - 1;
            continue;
        }
        // ReShade's uniform sources, same units: timer/frametime in ms, framecount, random in [min, max]
        float v[4] = {};
        if (!strcmp(p.items, "timer")) v[0] = (float)(now - startMs_);
        else if (!strcmp(p.items, "frametime")) v[0] = frameTimeMs_;
        else if (!strcmp(p.items, "framecount")) v[0] = (float)frameCount_;
        else if (!strcmp(p.items, "random"))
        {
            rng_ = rng_ * 1664525u + 1013904223u;
            v[0] = (float)(int)(p.min + (float)(rng_ >> 8) / 16777216.0f * (p.max - p.min + 1.0f));
        }
        dev->SetPixelShaderConstantF(p.reg, v, 1);
    }
}

void Runner::BindTex(IDirect3DDevice9* dev, int stage, IDirect3DBaseTexture9* tex)
{
    if (boundTex_[stage] == tex) return;
    dev->SetTexture(stage, tex);
    boundTex_[stage] = tex;
}

static void SetSampler(IDirect3DDevice9* dev, int stage, bool point)
{
    const DWORD filter = point ? D3DTEXF_POINT : D3DTEXF_LINEAR;
    dev->SetSamplerState(stage, D3DSAMP_MINFILTER, filter);
    dev->SetSamplerState(stage, D3DSAMP_MAGFILTER, filter);
    dev->SetSamplerState(stage, D3DSAMP_MIPFILTER, point ? D3DTEXF_POINT : D3DTEXF_LINEAR);
    dev->SetSamplerState(stage, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(stage, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(stage, D3DSAMP_SRGBTEXTURE, FALSE);
    dev->SetSamplerState(stage, D3DSAMP_MAXMIPLEVEL, 0);
    dev->SetSamplerState(stage, D3DSAMP_MIPMAPLODBIAS, 0);
}

void Runner::UnbindAll(IDirect3DDevice9* dev)
{
    for (int s = 0; s < kTouchedSamplers; ++s) BindTex(dev, s, nullptr);
}

// AdaptiveSharpen: pass 0 writes edge data to an RG16F texture, pass 1 sharpens.
bool Runner::RunAdaptiveSharpen(const Frame& f, const Step& s)
{
    IDirect3DDevice9* dev = f.dev;
    if (!Ensure(dev, asTex_, f.width, f.height, D3DFMT_G16R16F)) return false;
    IDirect3DPixelShader9* p0 = Ps(dev, s.shader, 0);
    IDirect3DPixelShader9* p1 = Ps(dev, s.shader, 1);
    if (!p0 || !p1) return false;
    UnbindAll(dev);
    BindTex(dev, 0, ping_.tex);
    Draw(dev, p0, asTex_.surf, f.width, f.height);
    BindTex(dev, 1, asTex_.tex);
    Draw(dev, p1, out_, f.width, f.height);
    return true;
}

bool Runner::Run(const Frame& f, const std::vector<Step>& steps)
{
    skipped_.clear();
    IDirect3DDevice9* dev = f.dev;
    if (!dev || !f.target || !f.width || !f.height || steps.empty()) return false;
    if (!Ensure(dev, ping_, f.width, f.height, D3DFMT_A8R8G8B8) || !Ensure(dev, pong_, f.width, f.height, D3DFMT_A8R8G8B8))
    {
        skipped_.push_back("render targets");
        return false;
    }
    IDirect3DPixelShader9* copy = CopyPs(dev);
    if (!copy) { skipped_.push_back("copy shader"); return false; }

    curRt_ = nullptr; curPs_ = nullptr; constsSent_ = false; target_ = f.target;
    for (int st = 0; st < kTouchedSamplers; ++st) { dev->SetTexture(st, nullptr); boundTex_[st] = nullptr; }
    buf_[0] = 1.0f / f.width; buf_[1] = 1.0f / f.height; buf_[2] = (float)f.width; buf_[3] = (float)f.height;
    aux_[0] = aux_[1] = aux_[2] = aux_[3] = 0.0f;
    if (f.frame == 0 || f.frame != lastFrame_) // a run point later in the same frame keeps the frame's count and time
    {
        lastFrame_ = f.frame;
        const unsigned long long now = GetTickCount64();
        if (!startMs_) startMs_ = lastMs_ = now;
        ++frameCount_;
        frameTimeMs_ = (float)(now - lastMs_);
        lastMs_ = now;
    }
    depthC_[0] = f.farPlane; depthC_[1] = depthC_[2] = depthC_[3] = 0.0f; // not reversed

    // Fixed state for every pass
    dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    dev->SetVertexShader(nullptr);
    dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    for (int st = 0; st < kTouchedSamplers; ++st) SetSampler(dev, st, false);

    dev->StretchRect(f.target, nullptr, ping_.surf, nullptr, D3DTEXF_NONE);

    // The last step draws straight into the target (no write-back pass) when that gives the same pixels: full mix, an
    // 8-bit target like the ping-pong textures, and no sRGB write (not every target format takes it)
    const Step* direct = nullptr;
    if (f.mix >= 0.999f)
    {
        D3DSURFACE_DESC td{};
        const bool same = SUCCEEDED(f.target->GetDesc(&td)) && (td.Format == D3DFMT_A8R8G8B8 || td.Format == D3DFMT_X8R8G8B8);
        for (const Step& s : steps)
            if (s.shader >= 0 && s.shader < ShaderCount() && s.values && !(Shader(s.shader).needsDepth && !f.depth)) direct = &s;
        if (direct && (!same || Shader(direct->shader).srgb)) direct = nullptr;
    }
    bool wroteTarget = false;

    bool drew = false;
    for (const Step& s : steps)
    {
        if (s.shader < 0 || s.shader >= ShaderCount() || !s.values) continue;
        out_ = &s == direct ? f.target : pong_.surf;
        const LooksShader& d = Shader(s.shader);
        if (d.needsDepth && !f.depth) { skipped_.push_back(std::string(d.technique) + " (no depth)"); continue; }

        SetParams(dev, s.shader, s.values);
        bool ok;
        if (!strcmp(d.technique, "AdaptiveSharpen")) ok = RunAdaptiveSharpen(f, s);
        else
        {
            IDirect3DPixelShader9* ps = Ps(dev, s.shader, 0);
            ok = ps != nullptr;
            if (ok)
            {
                UnbindAll(dev);
                BindTex(dev, 0, ping_.tex);
                if (d.srgb)
                {
                    dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, TRUE);
                    dev->SetRenderState(D3DRS_SRGBWRITEENABLE, TRUE);
                }
                Draw(dev, ps, out_, f.width, f.height);
                if (d.srgb)
                {
                    dev->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, FALSE);
                    dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
                }
            }
        }
        if (!ok) { skipped_.push_back(std::string(d.technique) + " (shader or texture missing)"); continue; }
        drew = true;
        if (out_ == f.target) { wroteTarget = true; continue; }
        std::swap(ping_, pong_);
    }

    UnbindAll(dev);
    SetSampler(dev, 4, false);
    if (drew && !wroteTarget)
    {
        // Write the result back: RGB only, blended over the input by mix.
        const float mix = (std::min)((std::max)(f.mix, 0.0f), 1.0f);
        dev->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE);
        if (mix < 0.999f)
        {
            const DWORD m = (DWORD)(mix * 255.0f + 0.5f);
            dev->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
            dev->SetRenderState(D3DRS_BLENDOP, D3DBLENDOP_ADD);
            dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_BLENDFACTOR);
            dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_INVBLENDFACTOR);
            dev->SetRenderState(D3DRS_BLENDFACTOR, D3DCOLOR_ARGB(m, m, m, m));
        }
        BindTex(dev, 0, ping_.tex);
        Draw(dev, copy, f.target, f.width, f.height);
        UnbindAll(dev);
    }
    return drew;
}

} // namespace Looks
