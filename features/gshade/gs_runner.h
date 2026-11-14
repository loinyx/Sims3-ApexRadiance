#pragma once
// GShade looks: runs ported ReShade/GShade shaders on a D3D9 render target.
// No Apex dependencies: used by Apex (features/gshade_looks.cpp) and by the offline comparison bench.

#include <d3d9.h>
#include <functional>
#include <map>
#include <string>
#include <vector>
#include "gs_types.h"

namespace GShade {

// The generated table (gs_generated.h)
int ShaderCount();
const GsShaderDesc& Shader(int index);
int FindShader(const std::string& technique, const std::string& file); // -1 if not ported
std::string FullSource(int shader);                                     // prelude + body, ready for D3DCompile

// ---------------------------------------------------------------------------------------------- presets (.ini)
struct PresetEntry
{
    std::string technique; // "FilmicPass"
    std::string file;      // "FilmicPass.fx"
    int shader = -1;       // index into the table, -1 = not ported yet
};

struct Preset
{
    std::wstring path;
    std::vector<PresetEntry> order; // Techniques= in TechniqueSorting order
    std::map<std::string, std::map<std::string, std::string>> sections; // [File.fx] key=value
    std::string error;
};

bool LoadPreset(const std::wstring& path, Preset& out);
// Values of one shader as the .ini sets them (keys missing in the .ini keep the .fx default). 4 floats per param.
std::vector<float> PresetValues(const Preset& preset, int shader);
// PreprocessorDefinitions of a .fx section ("" if none); the port is compiled with the .fx defaults only.
std::string PresetDefines(const Preset& preset, const std::string& file);

// Depth settings from GShade.ini [GENERAL] PreprocessorDefinitions (defaults: far 1000, not reversed)
struct DepthSettings { float farPlane = 1000.0f; bool reversed = false; bool upsideDown = false; bool logarithmic = false; };
DepthSettings LoadDepthSettings(const std::wstring& gshadeIni);

// ---------------------------------------------------------------------------------------------- runner
// Device state Run() changes; the caller saves it before Run and restores it after.
// Also: pixel shader, vertex shader (null), FVF, render target 0 (+ viewport), PS constants c0..c31.
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
    DepthSettings depthSettings;
    float mix = 1.0f;                    // 0..1, blend of the stage over the input
    unsigned frame = 0;                  // the caller's frame number (framecount/frametime tick once per new value); 0 = every Run is a frame
};

class Runner
{
public:
    void SetFactory(PsFactory f) { factory_ = std::move(f); }
    // Optional: how render targets are bound (Apex passes its un-hooked SetRenderTarget). Default: dev->SetRenderTarget.
    void SetRenderTargetFn(std::function<HRESULT(IDirect3DDevice9*, DWORD, IDirect3DSurface9*)> f) { setRT_ = std::move(f); }
    // Runs the steps in order. Changes device state (see gs_runner.cpp: StatesTouched); the caller saves/restores.
    // Returns false if nothing was drawn (missing shader, resource failure); skipped steps are listed in skipped().
    bool Run(const Frame& frame, const std::vector<Step>& steps);
#ifdef GS_PERF
    std::function<void(int)> stepHook; // bench only (tools/bench/gs_perf.cpp): step index at its start, -1 before the write-back
#endif
    const std::vector<std::string>& skipped() const { return skipped_; }
    void Release(); // before device Reset / on shutdown
    size_t GpuBytes() const { return gpuBytes_; }

private:
    struct Tex { IDirect3DTexture9* tex = nullptr; IDirect3DSurface9* surf = nullptr; UINT w = 0, h = 0; D3DFORMAT fmt = D3DFMT_UNKNOWN; UINT levels = 1; };
    bool Ensure(IDirect3DDevice9* dev, Tex& t, UINT w, UINT h, D3DFORMAT fmt, UINT levels = 1);
    void Free(Tex& t);
    IDirect3DPixelShader9* Ps(IDirect3DDevice9* dev, int shader, int entry);
    IDirect3DPixelShader9* CopyPs(IDirect3DDevice9* dev);
    void Draw(IDirect3DDevice9* dev, IDirect3DPixelShader9* ps, IDirect3DSurface9* rt, UINT w, UINT h);
    void SetParams(IDirect3DDevice9* dev, int shader, const float* values);
    void BindTex(IDirect3DDevice9* dev, int stage, IDirect3DBaseTexture9* tex);
    void UnbindAll(IDirect3DDevice9* dev);
    bool RunClarity(const Frame& f, const Step& s);
    bool RunAdaptiveSharpen(const Frame& f, const Step& s);

    PsFactory factory_;
    std::function<HRESULT(IDirect3DDevice9*, DWORD, IDirect3DSurface9*)> setRT_;
    std::map<long long, IDirect3DPixelShader9*> ps_;
    IDirect3DPixelShader9* copyPs_ = nullptr;
    Tex ping_, pong_, half1_, half2_, quarter_, asTex_;
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

} // namespace GShade
