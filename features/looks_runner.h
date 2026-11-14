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

// Device state Run() changes; the caller saves it before Run and restores it after.
// Also: pixel shader, vertex shader (null), FVF, render target 0 (+ viewport), PS constants. A step with a player's
// effect (looks_fx.h) changes more: see Fx::Instance::Run.
extern const D3DRENDERSTATETYPE kTouchedRenderStates[];
extern const int kTouchedRenderStateCount;
extern const int kTouchedSamplers; // s0..s5
extern const D3DSAMPLERSTATETYPE kTouchedSamplerStates[];
extern const int kTouchedSamplerStateCount;

// shader = -1 asks for the runner's copy pass (entry 0). Return nullptr while a shader is not ready.
using PsFactory = std::function<IDirect3DPixelShader9*(IDirect3DDevice9*, int shader, int entry)>;

struct Step
{
    int shader;
    const float* values; // 4 floats per param of that shader
};

struct Frame
{
    IDirect3DDevice9* dev = nullptr;
    IDirect3DSurface9* target = nullptr; // read and written (RGB only); must not be bound as a texture
    UINT width = 0, height = 0;
    IDirect3DTexture9* depth = nullptr;  // raw device depth in .x (Apex: INTZ), or nullptr
    float farPlane = 1000.0f;            // ReShade's RESHADE_DEPTH_LINEARIZATION_FAR_PLANE default
    float mix = 1.0f;                    // 0..1, blend of the stage over the input
    unsigned frame = 0;                  // the caller's frame number (framecount/frametime tick once per new value); 0 = every Run is a frame
};

class Runner
{
public:
    void SetFactory(PsFactory f) { factory_ = std::move(f); }
    // Optional: how render targets are bound (Apex passes its un-hooked SetRenderTarget). Default: dev->SetRenderTarget.
    void SetRenderTargetFn(std::function<HRESULT(IDirect3DDevice9*, DWORD, IDirect3DSurface9*)> f) { setRT_ = std::move(f); }
    // Runs the steps in order. Changes device state (see kTouched*); the caller saves/restores.
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
    IDirect3DBaseTexture9* boundTex_[6] = {};
    bool constsSent_ = false;
    float sentAux_[4] = {};
    IDirect3DSurface9* target_ = nullptr; // the frame's target: a draw into it writes RGB only
    IDirect3DSurface9* out_ = nullptr;    // where the current step's last draw goes: pong_, or the target for the last step
    float buf_[4] = {};
    float aux_[4] = {};
    float depthC_[4] = {};
    unsigned long long startMs_ = 0, lastMs_ = 0; // uniform sources: timer / frametime
    unsigned int frameCount_ = 0;
    unsigned int lastFrame_ = 0;       // Frame::frame of the last tick
    float frameTimeMs_ = 0.0f;
    unsigned int rng_ = 0x9E3779B9u;              // uniform source: random
};

} // namespace Looks
