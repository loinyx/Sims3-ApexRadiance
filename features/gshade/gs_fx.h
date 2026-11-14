// gs_fx: generic GShade/ReShade effect ports for Apex (batch 2 on).
// Route A, built in: tools/bench/gs_gen.cpp converts each .fx OFFLINE with ReShade's own D3D9 (shader model 3.0) code
// generator and compiles it to bytecode. The game never compiles a .fx: it loads the finished bytecode and pass table
// from GShadeLooks.pak (one effect at a time, only when switched on) and runs it here the way ReShade's D3D9 runtime does.
// The screen size stays symbolic: GS_BW_U / GS_BH_U are uniforms, texture sizes are expressions evaluated at run time.
#pragma once
#include <d3d9.h>
#include <cstdint>
#include <string>
#include <vector>

namespace gsfx {

struct Ann { std::string name; bool isString = false; std::string s; float f[4] = { 0, 0, 0, 0 }; };
const Ann* FindAnn(const std::vector<Ann>& as, const char* name);

enum Base : uint8_t { kBool = 1, kInt = 2, kUint = 3, kFloat = 4 };
struct Uniform
{
    std::string name;
    uint8_t base = kFloat, rows = 1, cols = 1; // vectors: rows = N, cols = 1 (reshadefx); matrices: rows x cols
    uint32_t array = 0, offset = 0, size = 0;   // SM3: one float4 register per vector / matrix row / array element
    std::vector<float> init;                    // components (x array), as floats
    std::vector<Ann> ann;
    unsigned Comps() const { return rows * cols; }
};
struct Texture
{
    std::string name, unique, semantic, wExpr, hExpr, image; // image = annotation source="file.png"
    uint32_t w = 1, h = 1, levels = 1, format = D3DFMT_A8R8G8B8;
    int32_t psReg = -1;  // register of the generated pixel-size constant (size depends on the screen)
    int32_t semReg = -1; // register of <SEMANTIC>_PIXEL_SIZE
    std::vector<Ann> ann;
};
struct Sampler { uint32_t texture = 0, minF = 2, magF = 2, mipF = 2, addrU = 3, addrV = 3, addrW = 3; uint8_t srgb = 0; float lodBias = 0, minLod = 0; };
struct Binding { uint32_t sampler = 0, reg = 0; uint8_t srgb = 0; };
struct Pass
{
    std::string name;
    int32_t vs = -1, ps = -1;            // shader index
    int32_t rt[4] = { -1, -1, -1, -1 }; // texture index; rt[0] == -1 = back buffer
    uint8_t clear = 0, mips = 0, srgbWrite = 0, blend = 0;
    uint32_t src = D3DBLEND_ONE, dst = D3DBLEND_ZERO, op = D3DBLENDOP_ADD, srcA = D3DBLEND_ONE, dstA = D3DBLEND_ZERO, opA = D3DBLENDOP_ADD;
    uint8_t mask[4] = { 15, 15, 15, 15 };
    uint8_t stencil = 0;
    uint32_t sRef = 0, sRead = 255, sWrite = 255, sFunc = D3DCMP_ALWAYS, sPass = D3DSTENCILOP_KEEP, sFail = D3DSTENCILOP_KEEP, sZFail = D3DSTENCILOP_KEEP;
    uint32_t prim = D3DPT_TRIANGLELIST, vertices = 3;
    std::vector<Binding> bind;
};
struct Technique { std::string name; std::vector<Ann> ann; std::vector<Pass> passes; };
struct Shader { std::string entry; uint8_t vs = 0; std::vector<uint8_t> code; };
struct Effect
{
    std::string file;
    uint32_t uniformBytes = 0;
    int32_t gsW = -1, gsH = -1; // uniform index of GS_BW_U / GS_BH_U
    std::vector<Uniform> uniforms;
    std::vector<Texture> textures;
    std::vector<Sampler> samplers;
    std::vector<Technique> techniques;
    std::vector<Shader> shaders;
};

bool Read(const void* data, size_t size, Effect& out);
void Write(const Effect& fx, std::vector<uint8_t>& out);
// Texture size expression ("GS_BW_U / 4", "1024", ...) with ReShade's constant-folding rules (int / int truncates)
double EvalSize(const std::string& expr, int W, int H, bool* ok);

// Uniform storage = SM3 constant registers (uniformBytes / 4 floats, at least 4)
void DefaultValues(const Effect& fx, std::vector<float>& regs);
void SetUniform(const Effect& fx, size_t index, const float* comps, size_t count, std::vector<float>& regs);

// Uniform sources, as ReShade sets them each frame: timer / frametime (ms), framecount, random, pingpong, date,
// bufready_depth; input sources (keys, mouse, overlay) stay 0. still = bench: only timer/frametime/framecount move.
struct SourceState
{
    float timerMs = 0, frametimeMs = 0;
    uint32_t frame = 0;
    bool still = false, depthReady = false;
    uint32_t rng = 0x9E3779B9u;
    std::vector<float> pingpong; // per uniform: value, direction
};
void UpdateSources(const Effect& fx, std::vector<float>& regs, SourceState& st);

// ---- GShadeLooks.pak: "GSPK" v1, index of effects + image files, each stored once ----
struct PakEntry { std::string name; uint64_t offset = 0; uint32_t size = 0; };
struct PakIndex { std::vector<PakEntry> effects, images; };
bool ReadPakIndex(const wchar_t* path, PakIndex& out);
bool ReadPakBlob(const wchar_t* path, const PakEntry& e, std::vector<uint8_t>& out);

// .ini value text ("0.5,1,0") -> components of uniform u (missing components keep the initialiser)
std::vector<float> ParseIniValue(const Uniform& u, const std::string& text);
// WIC image decoder (PNG/JPG/BMP/DDS that WIC reads); COM must be initialised on the calling thread
bool DecodeWic(const uint8_t* bytes, size_t size, UINT wantW, UINT wantH, UINT& w, UINT& h, std::vector<uint8_t>& bgra);
// How Run() binds render targets / depth-stencil (a host with hooks passes its un-hooked calls). Default: the device's.
typedef HRESULT (*SetRenderTargetFn)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*);
typedef HRESULT (*SetDepthStencilFn)(IDirect3DDevice9*, IDirect3DSurface9*);
void SetBindFunctions(SetRenderTargetFn rt, SetDepthStencilFn ds);

// Image decoder supplied by the host (WIC in the game and the bench): file bytes -> BGRA at want size (0 = native)
typedef bool (*ImageDecodeFn)(const uint8_t* bytes, size_t size, UINT wantW, UINT wantH, UINT& w, UINT& h, std::vector<uint8_t>& bgra);
typedef bool (*ImageFetchFn)(void* ctx, const std::string& name, std::vector<uint8_t>& bytes);

// One effect's GPU resources at one screen size. Run() leaves render states changed: the caller saves/restores them.
class Instance
{
public:
    ~Instance() { Release(); }
    bool Create(IDirect3DDevice9* dev, const Effect& fx, UINT W, UINT H, ImageFetchFn fetch, void* fetchCtx, ImageDecodeFn decode, std::string* err);
    void Release();
    bool Ready() const { return fx_ != nullptr; }
    UINT Width() const { return W_; }
    UINT Height() const { return H_; }
    // target: the image the technique reads (COLOR) and writes (back buffer); same size as Create's W x H
    bool Run(IDirect3DDevice9* dev, size_t technique, IDirect3DSurface9* target, IDirect3DTexture9* depth, const std::vector<float>& regs);
private:
    const Effect* fx_ = nullptr;
    UINT W_ = 0, H_ = 0;
    std::vector<IDirect3DTexture9*> tex_;     // per texture (COLOR -> colorTex_, DEPTH -> null, set at Run)
    std::vector<UINT> texW_, texH_;
    std::vector<IUnknown*> shaders_;
    IDirect3DTexture9* colorTex_ = nullptr;
    IDirect3DSurface9* colorSurf_ = nullptr;
    IDirect3DSurface9* stencil_ = nullptr;
    IDirect3DVertexDeclaration9* decl_ = nullptr;
    UINT maxPrims_ = 1;                   // device MaxPrimitiveCount, read once at Create (not once per pass)
    std::vector<float> c_;                // shader constants of the current Run (kept, so no allocation per frame)
    IDirect3DVertexBuffer9* vb_ = nullptr; // vertex ids 0..max-1 as float TEXCOORD0 (DEFAULT pool: video memory, not the game's address space)
};

} // namespace gsfx
