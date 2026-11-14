#pragma once
// Looks: the player's own ReShade FX effects (.fx), compiled by ReShade's own effect compiler (third_party/reshadefx,
// the shader model 3.0 HLSL code generator ReShade uses for Direct3D 9) and run the way ReShade's D3D9 runtime runs them.
//
// looks_fx.cpp: compile (any thread; reads files, never touches a device) and the GPU instance (render thread).
// looks_fx_catalog.cpp: the Shaders folder, the background scan, and the user techniques as LooksShader entries.
//
// .fx, .fxh and texture files are untrusted input: every size and count is capped, includes and textures must stay
// inside the Shaders folder, and a technique that cannot run on Direct3D 9 gets a reason instead of running.
#include <d3d9.h>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>
#include "looks_shaders.h"

namespace Looks::Fx {

inline constexpr size_t kMaxFxBytes = 2u << 20;             // one .fx or included file
inline constexpr size_t kMaxTextureFileBytes = 64u << 20;   // one texture file
inline constexpr size_t kMaxIncludes = 64;                  // #include directives followed per effect (bounds the depth)
inline constexpr size_t kMaxPreprocessedBytes = 16u << 20;  // preprocessor output
inline constexpr uint64_t kMaxEffectGpuBytes = 256ull << 20; // textures and render targets of one effect
inline constexpr size_t kMaxTextures = 64;                  // textures of one effect
inline constexpr size_t kMaxPasses = 64;                    // passes of one technique
inline constexpr uint32_t kMaxVertices = 1u << 20;          // vertices of one pass
inline constexpr int kMaxRenderTargets = 4;                 // Direct3D 9: simultaneous render targets
inline constexpr int kConstRegs = 224;                      // ps_3_0 float constants

struct Ann
{
    std::string name;
    bool isString = false;
    std::string s;
    float f[4] = { 0, 0, 0, 0 };
};
const Ann* FindAnn(const std::vector<Ann>& as, const char* name);

enum Base : uint8_t { kBool = 1, kInt = 2, kUint = 3, kFloat = 4 };
struct Uniform
{
    std::string name;
    uint8_t base = kFloat, rows = 1, cols = 1; // vectors: rows = N, cols = 1; matrices: rows x cols
    uint32_t array = 0, offset = 0;            // offset: bytes of the SM3 register file (register = offset / 16)
    std::vector<float> init;                   // components (x array elements)
    std::vector<Ann> ann;
    unsigned Comps() const { return rows * cols; }
};
struct Texture
{
    std::string name, unique, semantic;
    std::wstring image;      // annotation source = "file.png", resolved inside the Shaders folder ("" = none)
    std::string error;       // why this texture cannot be made here ("" = it can)
    uint32_t w = 1, h = 1, levels = 1;
    D3DFORMAT format = D3DFMT_A8R8G8B8;
    int32_t semReg = -1;     // register of <SEMANTIC>_PIXEL_SIZE
    uint64_t bytes = 0;      // GPU memory
};
struct Sampler { uint32_t texture = 0, minF = 2, magF = 2, mipF = 2, addrU = 3, addrV = 3, addrW = 3; float lodBias = 0, minLod = 0; };
struct Binding { uint32_t sampler = 0, reg = 0; uint8_t srgb = 0; };
struct Pass
{
    int32_t vs = -1, ps = -1;            // shader index
    int32_t rt[4] = { -1, -1, -1, -1 }; // texture index; rt[0] == -1 = the picture
    uint8_t clear = 0, mips = 0, srgbWrite = 0, blend = 0;
    uint32_t src = D3DBLEND_ONE, dst = D3DBLEND_ZERO, op = D3DBLENDOP_ADD, srcA = D3DBLEND_ONE, dstA = D3DBLEND_ZERO, opA = D3DBLENDOP_ADD;
    uint8_t mask[4] = { 15, 15, 15, 15 };
    uint8_t stencil = 0;
    uint32_t sRef = 0, sRead = 255, sWrite = 255, sFunc = D3DCMP_ALWAYS, sPass = D3DSTENCILOP_KEEP, sFail = D3DSTENCILOP_KEEP, sZFail = D3DSTENCILOP_KEEP;
    uint32_t prim = D3DPT_TRIANGLELIST, vertices = 3;
    std::vector<Binding> bind;
};
struct Technique
{
    std::string name;
    std::vector<Ann> ann;
    std::vector<Pass> passes;
    std::string reason;      // why it cannot run here ("" = it can)
    bool needsDepth = false; // samples the DEPTH texture
    bool hidden = false;     // annotation hidden = true
};
struct Shader { std::string entry; uint8_t vs = 0; std::vector<uint8_t> code; std::string error; };
struct Effect
{
    std::string file;        // file name, as presets name it ("Bloom.fx")
    std::wstring path;
    UINT width = 0, height = 0; // BUFFER_WIDTH / BUFFER_HEIGHT it was compiled for
    uint32_t constRegs = 0;     // float constant registers the uniforms use
    std::vector<Uniform> uniforms;
    std::vector<Texture> textures;
    std::vector<Sampler> samplers;
    std::vector<Technique> techniques;
    std::vector<Shader> shaders;
    std::string error;          // why the whole file cannot run ("" = see each technique)
    std::vector<std::string> notes; // D3D9 fallbacks used (lowered texture formats, other compile flags)
    uint64_t gpuBytes = 0;
};

// Where a compile may read: root = the Shaders folder (with a trailing backslash); includes are searched in
// includeDirs after the including file's own folder, texture files in textureDirs (and their subfolders).
struct Folders
{
    std::wstring root;
    std::vector<std::wstring> includeDirs, textureDirs;
};

// Preprocess, parse, generate shader model 3.0 HLSL and D3DCompile every entry point the techniques use, at the given
// buffer size. Never throws (an exception becomes out.error). False when the file cannot run at all (out.error).
bool Compile(const std::wstring& fxPath, const Folders& folders, UINT width, UINT height, Effect& out);
// True when path (absolute) is inside root and has no ".." part
bool InsideRoot(const std::wstring& root, const std::wstring& path);

// ---------------------------------------------------------------------------------------------- uniforms
// Uniform storage = SM3 float constant registers (constRegs x 4 floats)
void DefaultValues(const Effect& fx, std::vector<float>& regs);
void SetUniform(const Effect& fx, size_t index, const float* comps, size_t count, std::vector<float>& regs);
// ReShade's uniform sources, set each frame: timer / frametime (ms), framecount, random, pingpong, date,
// bufready_depth; input sources (keys, mouse, overlay) stay 0.
struct SourceState
{
    float timerMs = 0, frametimeMs = 0;
    uint32_t frame = 0;
    bool depthReady = false;
    uint32_t rng = 0x9E3779B9u;
    std::vector<float> pingpong; // per uniform: value, direction
};
void UpdateSources(const Effect& fx, std::vector<float>& regs, SourceState& st);

// ---------------------------------------------------------------------------------------------- GPU
// How Run() binds render targets and depth-stencil (Apex passes its un-hooked calls). Default: the device's.
typedef HRESULT (*SetRenderTargetFn)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
typedef HRESULT (*SetDepthStencilFn)(IDirect3DDevice9*, IDirect3DSurface9*);
void SetBindFunctions(SetRenderTargetFn rt, SetDepthStencilFn ds);

// One effect's textures, render targets and shaders on one device. Create reads the effect's texture files (once, not
// per frame). Run changes device state (render states, samplers s0..s15, textures, shaders, VS/PS constants, render
// targets 0..3, depth-stencil, viewport, vertex declaration, stream 0): the caller saves and restores it.
class Instance
{
public:
    ~Instance() { Release(); }
    bool Create(IDirect3DDevice9* dev, std::shared_ptr<const Effect> fx, std::string* err);
    void Release();
    bool Ready() const { return fx_ != nullptr; }
    const Effect* Fx() const { return fx_.get(); }
    uint64_t GpuBytes() const { return gpuBytes_; }
    // target: read (COLOR) and written by the technique in place; the size the effect was compiled for. depth: DEPTH.
    bool Run(IDirect3DDevice9* dev, size_t technique, IDirect3DSurface9* target, IDirect3DTexture9* depth, const std::vector<float>& regs);

private:
    std::shared_ptr<const Effect> fx_;
    std::vector<IDirect3DTexture9*> tex_; // per texture (COLOR -> colorTex_, DEPTH and refused ones -> null)
    std::vector<IUnknown*> shaders_;
    IDirect3DTexture9* colorTex_ = nullptr;
    IDirect3DSurface9* colorSurf_ = nullptr;
    IDirect3DSurface9* stencil_ = nullptr;
    IDirect3DVertexDeclaration9* decl_ = nullptr;
    IDirect3DVertexBuffer9* vb_ = nullptr; // vertex ids 0..max-1 as float TEXCOORD0
    UINT maxPrims_ = 1;
    uint64_t gpuBytes_ = 0;
    std::vector<float> c_;
};

// ---------------------------------------------------------------------------------------------- catalog (looks_fx_catalog.cpp)
std::wstring ShadersFolder();   // Apex Radiance\Shaders\ (trailing backslash)
bool EnsureShadersFolder();     // with its Shaders\ and Textures\ subfolders
void RequestScan(bool force = false); // any thread: compile the folder again (unchanged files are kept unless force)
bool Scanning();
// Render thread, once a frame: the picture size user effects compile for, and adopts a finished scan. True when the
// user shader table changed (shader indices must be looked up again).
bool Poll(UINT width, UINT height);
int Generation();               // changes with each adopted scan
bool Scanned();                 // a scan has been adopted

// User techniques are shader indices from kUserShaderBase (looks_runner.h)
const LooksShader* UserShader(int index); // nullptr when out of range
int FindUser(const std::string& technique, const std::string& file); // a runnable user technique, or -1
// Why a technique of a user file cannot run ("" when no such technique or file exists in the folder)
std::string Reason(const std::string& technique, const std::string& file);
bool HasFile(const std::string& file); // a .fx of that name is in the folder
bool Pending(const std::string& file); // that file is not compiled yet (or the first scan has not finished)

// The Add a shader list: every user technique, runnable (index >= 0) or not (reason)
struct Listed { int index = -1; std::string technique, file, reason; };
std::vector<Listed> List();

// Copies picked files into the Shaders folder (any thread; not the render thread): .fx / .fxh into Shaders\Shaders\,
// images (.png .jpg .jpeg .bmp .dds) into Shaders\Textures\. Other files and files over the size caps are refused. A
// file with the same bytes is reused; a different file of that name gets a numbered name ("Name-1.fx"). Then a scan.
struct AddResult { int added = 0, reused = 0, refused = 0; };
AddResult AddFiles(const std::vector<std::wstring>& paths);

// Runs a user technique in place on target (render thread); values: 4 floats per LooksShader param.
struct RunFrame
{
    IDirect3DDevice9* dev = nullptr;
    IDirect3DSurface9* target = nullptr;
    IDirect3DTexture9* depth = nullptr;
    UINT width = 0, height = 0;
    float timerMs = 0, frametimeMs = 0;
    uint32_t frame = 0;
};
bool Run(const RunFrame& f, int index, const float* values, std::string* why);
void ReleaseGpu();
uint64_t GpuBytes();

} // namespace Looks::Fx
