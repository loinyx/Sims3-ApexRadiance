// Looks: the D3D9 runner of the ported ReShade shaders (third_party/reshade_shaders). Pure D3D9 + STL.

#include "looks_runner.h"
#include "looks_fx.h"
#include "third_party/reshade_shaders/reshade_shaders_hlsl.h"

#include <algorithm>
#include <cstring>

namespace Looks {

namespace {
// Milliseconds from a fine clock (ReShade's timer and frametime use a high-resolution clock)
double NowMs()
{
    static const double toMs = [] { LARGE_INTEGER f{}; QueryPerformanceFrequency(&f); return f.QuadPart ? 1000.0 / (double)f.QuadPart : 0.0; }();
    LARGE_INTEGER c{};
    QueryPerformanceCounter(&c);
    return (double)c.QuadPart * toMs;
}
} // namespace

// ============================================================================================== table
int ShaderCount() { return ReShadeShaders::kShaderCount; }
const LooksShader& Shader(int index)
{
    if (IsUserShader(index))
    {
        static const LooksShader kGone{ "", "", nullptr, nullptr, nullptr, 0, false, false, "", (LooksShaderKind)0 };
        const LooksShader* u = Fx::UserShader(index - kUserShaderBase);
        return u ? *u : kGone;
    }
    return ReShadeShaders::kShaders[index];
}

bool ValidShader(int index)
{
    return IsUserShader(index) ? Fx::UserShader(index - kUserShaderBase) != nullptr : index >= 0 && index < ShaderCount();
}

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
    const int user = Fx::FindUser(technique, file);
    return user < 0 ? -1 : kUserShaderBase + user;
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
        // The runner's own copy / mix pass; c3.x > 0 applies the game's later colour curve x (1 + c) / (x + c) (c3.y > 0) or
        // its inverse (c3.y < 0), see Frame::gameCurve
        s += "\nfloat4 gs_curve : register(c3);\n"
             "float4 gs_copy(float2 uv : TEXCOORD0) : COLOR0 {\n"
             "  float3 c = saturate(tex2D(BackBuffer, uv).rgb); float k = gs_curve.x;\n"
             "  if (k > 0) c = gs_curve.y > 0 ? c * (1 + k) / (c + k) : k * c / (1 + k - c);\n"
             "  return float4(c, 1.0); }\n";
        return s;
    }
    Append(s, Shader(shader).src);
    return s;
}

// ============================================================================================== runner
int PsConstsUsed(const std::vector<Step>& steps)
{
    int n = 4; // c0 picture size, c1 pass data, c2 depth, c3 the copy's curve
    for (const Step& s : steps)
    {
        if (IsUserShader(s.shader) || !ValidShader(s.shader)) continue;
        const LooksShader& d = Shader(s.shader);
        for (int i = 0; i < d.paramCount; ++i) n = (std::max)(n, d.params[i].reg + 1);
    }
    return n;
}

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
    const double now = NowMs();
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
    for (int s = 0; s < kBuiltInSamplers; ++s) BindTex(dev, s, nullptr);
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

// The fixed state of the built-in passes; also put back after a player's effect ran (it sets its own), with what this
// Run has sent forgotten
void Runner::FixedState(IDirect3DDevice9* dev)
{
    curRt_ = nullptr; curPs_ = nullptr; constsSent_ = false;
    for (int st = 0; st < kBuiltInSamplers; ++st) boundTex_[st] = nullptr; // the effect unbinds what it bound
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
    for (int st = 0; st < kBuiltInSamplers; ++st) SetSampler(dev, st, false);
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
    for (int st = 0; st < kBuiltInSamplers; ++st) { dev->SetTexture(st, nullptr); boundTex_[st] = nullptr; }
    buf_[0] = 1.0f / f.width; buf_[1] = 1.0f / f.height; buf_[2] = (float)f.width; buf_[3] = (float)f.height;
    aux_[0] = aux_[1] = aux_[2] = aux_[3] = 0.0f;
    if (f.frame == 0 || f.frame != lastFrame_) // a run point later in the same frame keeps the frame's count and time
    {
        lastFrame_ = f.frame;
        const double now = NowMs(); // a fine clock, as ReShade's (GetTickCount64 moved in 16 ms steps: frame time 0 or 16)
        if (!startMs_) startMs_ = lastMs_ = now;
        ++frameCount_;
        frameTimeMs_ = (float)(now - lastMs_);
        lastMs_ = now;
    }
    depthC_[0] = f.farPlane; depthC_[1] = depthC_[2] = depthC_[3] = 0.0f; // not reversed

    FixedState(dev);

    dev->StretchRect(f.target, nullptr, ping_.surf, nullptr, D3DTEXF_NONE);
    const bool curved = f.gameCurve > 0.0f;
    if (curved) // the picture as it looks after the game's curve, which the shaders are made for
    {
        const float c3[4] = { f.gameCurve, 1.0f, 0.0f, 0.0f };
        dev->SetPixelShaderConstantF(3, c3, 1);
        BindTex(dev, 0, ping_.tex);
        Draw(dev, copy, pong_.surf, f.width, f.height);
        UnbindAll(dev);
        std::swap(ping_, pong_);
    }


    // The last step draws straight into the target (no write-back pass) when that gives the same pixels: full mix, an
    // 8-bit target like the ping-pong textures, and no sRGB write (not every target format takes it)
    const Step* direct = nullptr;
    if (f.mix >= 0.999f && !curved)
    {
        D3DSURFACE_DESC td{};
        const bool same = SUCCEEDED(f.target->GetDesc(&td)) && (td.Format == D3DFMT_A8R8G8B8 || td.Format == D3DFMT_X8R8G8B8);
        for (const Step& s : steps)
            if (ValidShader(s.shader) && s.values && !(Shader(s.shader).needsDepth && !f.depth)) direct = &s;
        if (direct && (!same || IsUserShader(direct->shader) || Shader(direct->shader).srgb)) direct = nullptr;
    }
    bool wroteTarget = false;

    bool drew = false;
    for (const Step& s : steps)
    {
        if (!ValidShader(s.shader) || !s.values) continue;
        out_ = &s == direct ? f.target : pong_.surf;
        const LooksShader& d = Shader(s.shader);
        if (d.needsDepth && !f.depth) { skipped_.push_back(std::string(d.technique) + " (no depth)"); continue; }
        if (IsUserShader(s.shader))
        {
            // a player's .fx: its technique runs in place on the ping texture, as ReShade runs it on the back buffer
            Fx::RunFrame uf;
            uf.dev = dev; uf.target = ping_.surf; uf.depth = f.depth; uf.width = f.width; uf.height = f.height;
            uf.timerMs = (float)(lastMs_ - startMs_); uf.frametimeMs = frameTimeMs_; uf.frame = frameCount_;
            std::string why;
            const bool ran = Fx::Run(uf, s.shader - kUserShaderBase, s.values, &why);
            FixedState(dev); // the effect changed render states, samplers, shaders and the render target
            if (!ran) { skipped_.push_back(std::string(d.technique) + " (" + why + ")"); continue; }
            drew = true;
            continue;
        }

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
        const float c3[4] = { f.gameCurve, -1.0f, 0.0f, 0.0f }; // undone, so the game's curve gives the look's pixels
        dev->SetPixelShaderConstantF(3, c3, 1);
        BindTex(dev, 0, ping_.tex);
        Draw(dev, copy, f.target, f.width, f.height);
        UnbindAll(dev);
    }
    return drew;
}

} // namespace Looks
