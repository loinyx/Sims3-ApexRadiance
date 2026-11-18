#pragma once
// Looks: runs the ported ReShade shaders (third_party/reshade_shaders) on a D3D9 render target, in a given order with
// given parameter values. Pure D3D9 + STL; the preset (.ini) side is in looks_preset.h, the feature in
// patches/looks_patch.cpp.

#include <d3d9.h>
#include <functional>
#include <map>
#include <string>
#include <vector>
#include "looks_shaders.h"

namespace Looks {

// The generated table (third_party/reshade_shaders/reshade_shaders_hlsl.h): indices 0..ShaderCount()-1. The player's
// own .fx techniques (looks_fx.h) follow from kUserShaderBase; their indices change when the Shaders folder is scanned
// again (Fx::Poll returns true), so callers look them up again then.
inline constexpr int kUserShaderBase = 1 << 16;
inline bool IsUserShader(int index) { return index >= kUserShaderBase; }
int ShaderCount();
const LooksShader& Shader(int index); // a user index no longer in the table gives an empty entry (no parameters)
bool ValidShader(int index);
// A built-in port first (they keep priority), then a runnable user technique; -1 if neither
int FindShader(const std::string& technique, const std::string& file);
std::string FullSource(int shader);                                     // prelude + body, ready for D3DCompile

// Device state a run changes; the caller saves it before Run and restores it after (patches/looks_patch.cpp, checked by
// tools/looks_fx_check --state). The built-in passes change the first kBuiltInRenderStates render states, the first
// kBuiltInSamplerStates sampler states of s0..s5, PS constants c0..PsConstsUsed()-1, the pixel shader, the vertex shader
// (null), the FVF, stream 0 and render target 0 (+ viewport). A step with a player's effect changes every state listed,
// samplers s0..s15, PS constants c0..223, VS constants c0..255, render targets 1..3, the depth-stencil surface and the
// vertex declaration (see Fx::Instance::Run).
inline constexpr D3DRENDERSTATETYPE kSavedRenderStates[] = { D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ALPHABLENDENABLE,
    D3DRS_SEPARATEALPHABLENDENABLE, D3DRS_SRCBLEND, D3DRS_DESTBLEND, D3DRS_BLENDOP, D3DRS_BLENDFACTOR, D3DRS_ALPHATESTENABLE,
    D3DRS_STENCILENABLE, D3DRS_CULLMODE, D3DRS_SCISSORTESTENABLE, D3DRS_FOGENABLE, D3DRS_SRGBWRITEENABLE, D3DRS_CLIPPLANEENABLE,
    D3DRS_COLORWRITEENABLE, D3DRS_SRCBLENDALPHA, D3DRS_DESTBLENDALPHA, D3DRS_BLENDOPALPHA, D3DRS_COLORWRITEENABLE1,
    D3DRS_COLORWRITEENABLE2, D3DRS_COLORWRITEENABLE3, D3DRS_STENCILFUNC, D3DRS_STENCILPASS, D3DRS_STENCILFAIL, D3DRS_STENCILZFAIL,
    D3DRS_STENCILREF, D3DRS_STENCILMASK, D3DRS_STENCILWRITEMASK, D3DRS_TWOSIDEDSTENCILMODE, D3DRS_FILLMODE, D3DRS_DITHERENABLE,
    D3DRS_LASTPIXEL }; // (the last four: a player's effect only, set as ReShade sets them on every pass)
inline constexpr D3DSAMPLERSTATETYPE kSavedSamplerStates[] = { D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER,
    D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_SRGBTEXTURE, D3DSAMP_MAXMIPLEVEL, D3DSAMP_MIPMAPLODBIAS, D3DSAMP_ADDRESSW,
    D3DSAMP_BORDERCOLOR, D3DSAMP_MAXANISOTROPY }; // (the last: a player's effect only)
inline constexpr int kSavedRenderStateCount = (int)(sizeof(kSavedRenderStates) / sizeof(kSavedRenderStates[0]));
inline constexpr int kSavedSamplerStateCount = (int)(sizeof(kSavedSamplerStates) / sizeof(kSavedSamplerStates[0]));
inline constexpr int kBuiltInRenderStates = 16, kBuiltInSamplerStates = 8, kBuiltInSamplers = 6;

// shader = -1 asks for the runner's copy pass (entry 0). Return nullptr while a shader is not ready.
using PsFactory = std::function<IDirect3DPixelShader9*(IDirect3DDevice9*, int shader, int entry)>;

struct Step
{
    int shader;
    const float* values; // 4 floats per param of that shader
};

// PS constant registers the built-in steps write (c0..c2 and their parameters' registers): c0..PsConstsUsed()-1
int PsConstsUsed(const std::vector<Step>& steps);

struct Frame
{
    IDirect3DDevice9* dev = nullptr;
    IDirect3DSurface9* target = nullptr; // read and written (RGB only); must not be bound as a texture
    UINT width = 0, height = 0;
    IDirect3DTexture9* depth = nullptr;  // raw device depth in .x (Apex: INTZ), or nullptr
    float farPlane = 1000.0f;            // ReShade's RESHADE_DEPTH_LINEARIZATION_FAR_PLANE default
    float mix = 1.0f;                    // 0..1, blend of the stage over the input (0: the caller skips the run, same pixels)
    unsigned frame = 0;                  // the caller's frame number (framecount/frametime tick once per new value); 0 = every Run is a frame
    float wheel = 0.0f;                  // the frame's mouse wheel notches (mousewheel)
    bool overlayOpen = false;            // the Apex menu is open (overlay_open)
    float gameCurve = 0.0f;              // > 0: the game later applies x (1 + c) / (x + c) to the target; the stage sees it
                                         // applied and writes its inverse, so the picture after the game's curve is the look's
};

class Runner
{
public:
    void SetFactory(PsFactory f) { factory_ = std::move(f); }
    // Optional: how render targets are bound (Apex passes its un-hooked SetRenderTarget). Default: dev->SetRenderTarget.
    void SetRenderTargetFn(std::function<HRESULT(IDirect3DDevice9*, DWORD, IDirect3DSurface9*)> f) { setRT_ = std::move(f); }
    // Runs the steps in order. Changes device state (see kSavedRenderStates); the caller saves/restores.
    // Returns false if nothing was drawn (missing shader, resource failure); skipped steps are listed in skipped().
    bool Run(const Frame& frame, const std::vector<Step>& steps);
    const std::vector<std::string>& skipped() const { return skipped_; }
    void Release(); // before device Reset / on shutdown
    size_t GpuBytes() const { return gpuBytes_; }

private:
    struct Tex { IDirect3DTexture9* tex = nullptr; IDirect3DSurface9* surf = nullptr; UINT w = 0, h = 0; D3DFORMAT fmt = D3DFMT_UNKNOWN; };
    bool Ensure(IDirect3DDevice9* dev, Tex& t, UINT w, UINT h, D3DFORMAT fmt);
    void Free(Tex& t);
    IDirect3DPixelShader9* Ps(IDirect3DDevice9* dev, int shader, int entry);
    IDirect3DPixelShader9* CopyPs(IDirect3DDevice9* dev);
    void Draw(IDirect3DDevice9* dev, IDirect3DPixelShader9* ps, IDirect3DSurface9* rt, UINT w, UINT h);
    void SetParams(IDirect3DDevice9* dev, int shader, const float* values);
    void BindTex(IDirect3DDevice9* dev, int stage, IDirect3DBaseTexture9* tex);
    void UnbindAll(IDirect3DDevice9* dev);
    void FixedState(IDirect3DDevice9* dev);
    bool RunAdaptiveSharpen(const Frame& f, const Step& s);

    PsFactory factory_;
    std::function<HRESULT(IDirect3DDevice9*, DWORD, IDirect3DSurface9*)> setRT_;
    std::map<long long, IDirect3DPixelShader9*> ps_;
    IDirect3DPixelShader9* copyPs_ = nullptr;
    Tex ping_, pong_, asTex_;
    std::vector<std::string> skipped_;
    size_t gpuBytes_ = 0;
    // what the current Run has set (the RTX Remix bridge sends each call across processes)
    IDirect3DSurface9* curRt_ = nullptr;
    IDirect3DPixelShader9* curPs_ = nullptr;
    IDirect3DBaseTexture9* boundTex_[kBuiltInSamplers] = {};
    bool constsSent_ = false;
    float sentAux_[4] = {};
    IDirect3DSurface9* target_ = nullptr; // the frame's target: a draw into it writes RGB only
    IDirect3DSurface9* out_ = nullptr;    // where the current step's last draw goes: pong_, or the target for the last step
    float buf_[4] = {};
    float aux_[4] = {};
    float depthC_[4] = {};
    double startMs_ = 0, lastMs_ = 0; // uniform sources: timer / frametime (ms, NowMs)
    unsigned int frameCount_ = 0;
    unsigned int lastFrame_ = 0;       // Frame::frame of the last tick
    float frameTimeMs_ = 0.0f;
    unsigned int rng_ = 0x9E3779B9u;              // uniform source: random
};

} // namespace Looks
