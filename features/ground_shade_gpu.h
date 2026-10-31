#pragma once
// Walls shade lamp light near the ground, on the GPU (docs/features/night-lighting/lot-light-pass.md, "Lamp shade on lot
// ground"): the lot shade of GroundShadow::ShadeJob and the chunk shade of GroundShadow::ChunkFull / ChunkUpdate as ps_3_0
// passes (shaders/ground_shade_hlsl.h), with no game access, so the offline harness (tools/ground_shade_gpu_test) runs exactly
// this code on its own device. Every device call that Apex hooks goes through RtLampGpu::DeviceCalls.
//
//   - a lot: ShadeJob's inputs as float textures (Prepare, the CPU's per-lamp setup), the shade pass, Finish's blurs and bytes
//     into the lot's ext texture (G32R32F, (w + 1) x h: r = ShadeJob::Result's byte, g = StampWeight; texel (w, 0) r = the
//     grid's lowest byte, 255 when it shades nothing), and the lot mask from it (A8R8G8B8, rgb the room mask, a the shade);
//   - a chunk: level 0 over a changed rectangle from the lots' ext textures (max lost / max seen), then each mip over the
//     texels above it, giving the same bytes as the whole chunk made again.
#include <d3d9.h>
#include <cstdint>
#include <string>
#include <vector>
#include "ground_lamp_shadow.h"
#include "rt_lamp_gpu.h"

namespace GroundShadeGpu {

using Calls = RtLampGpu::DeviceCalls;

enum ShaderId { kFullVs, kShadePs, kBlurHPs, kBlurVPs, kBytePs, kMinPs, kMaskPs, kChunkPs, kChunkBytePs, kMipPs, kShaderCount };
const char* EntryOf(ShaderId id);
bool IsVertex(ShaderId id);
unsigned FlagsOf(ShaderId id); // D3DCOMPILE_* flags (the shade pass compiles only without optimisation)

// The shader's fixed bounds: past them a lot is built on the CPU (Prepare says why). kMaxTested counts lamp rows: a tested lamp
// with over kMaxOccluders walls or crowns in its reach takes more rows (Prepare)
constexpr int kMaxTested = 255, kMaxOthers = 255, kMaxOccluders = 255, kMaxPatches = 32;
// lamp-occluder pairs over all tested lamps (past it, a CPU build): a texel in every lamp's reach loops over them all, an estimated
// 12-16 ps_3_0 instructions a culled pair (unverified on hardware that enforces the cap), so 4000 stays under the 65535
// executed-instruction cap; the harness's 1000-pair lamp builds in 0.3 ms
constexpr int kMaxEntries = 4000;
constexpr size_t kWrapRows = 4096; // walls, patches, crowns: rows a column block of their textures (AtW in the shader)
constexpr int kLotsPerPass = 6; // ChunkPS's lot samplers

// ShadeJob::Init's inputs as the shade pass's float texels (RGBA rows)
struct Inputs {
    GroundShadow::Grid grid;
    std::vector<float> lamps, ent, walls, patches, crowns;
    int lampRows = 0, entW = 1, entRows = 1, wallW = 3, wallRows = 1, patchW = 4, patchRows = 1, crownW = 3, crownRows = 1;
    int tested = 0, others = 0, entries = 0; // tested: lamp rows (ShadePS), entries: lamp-occluder pairs
};
// false (why): past a bound, or a degenerate grid
bool Prepare(const GroundShadow::Grid& g, const std::vector<GroundShadow::Lamp>& lamps, const std::vector<GroundShadow::Lamp>& others, std::vector<GroundShadow::Wall> walls,
             const std::vector<WallOpenings::Patch>& patches, const std::vector<Canopy::Ellipsoid>& crowns, Inputs& out, std::string& why);

// One lot's ext texture over a chunk
struct ChunkSrc {
    IDirect3DTexture9* ext = nullptr;
    GroundShadow::Grid grid;
    GroundShadow::TexRect box; // ChunkRectOf(its world box)
};

class Engine {
public:
    explicit Engine(const Calls& calls) : c_(calls) {}
    ~Engine() { ReleaseAll(); }
    // The shaders (AddRef'd); false (err) when the device lacks a format or the shaders
    bool Init(IDirect3DDevice9* dev, void* const* shaders, std::string& err, bool forceArgbChunk = false);
    void ReleaseDefault(); // a device reset: the scratch targets and the state block (made again on use)
    void ReleaseAll();
    bool Ready() const { return ready_; }
    D3DFORMAT ChunkFormat() const { return chunkFmt_; } // L8, or A8R8G8B8 where L8 is no render target

    // The device's state around a series of builds
    bool Begin(IDirect3DDevice9* dev); // false: no state block could be made, nothing was changed (End is not called)
    void End(IDirect3DDevice9* dev);

    // The lot's ext texture (DEFAULT, made or made again at its size). shades: an occlusion query (made when null) that counts 1
    // when a texel of the grid is under 255 (read later, without waiting)
    bool BuildLot(IDirect3DDevice9* dev, const Inputs& in, IDirect3DTexture9*& ext, std::string& err, IDirect3DQuery9** shades = nullptr);
    // A CPU build's shade (ShadeJob::Result, StampWeight) as an ext texture (MANAGED)
    static bool UploadExt(IDirect3DDevice9* dev, const GroundShadow::Grid& g, const std::vector<uint8_t>& v, const std::vector<float>& w, IDirect3DTexture9*& ext);
    // The lot mask (DEFAULT A8R8G8B8 w x h) from the room mask and the ext texture's texels from (ox, oz)
    bool BuildMask(IDirect3DDevice9* dev, IDirect3DTexture9* ext, int extW, int extH, int ox, int oz, int w, int h, const std::vector<uint8_t>& room, IDirect3DTexture9*& mask,
                   std::string& err);
    // The chunk (corner wx0, wz0) over level-0 texels r0 and the mips over them; tex made (and r0 the whole chunk) when null
    bool BuildChunk(IDirect3DDevice9* dev, const std::vector<ChunkSrc>& srcs, float wx0, float wz0, GroundShadow::TexRect r0, IDirect3DTexture9*& tex, std::string& err);

    // GPU time of the last timed lot build (timestamp queries, read without waiting; -1 none yet)
    double LastGpuMs() { Poll(); return gpuMs_; }

private:
    bool Ensure(IDirect3DDevice9* dev, IDirect3DTexture9*& t, int& w, int& h, int needW, int needH, D3DFORMAT f, D3DPOOL pool, DWORD usage);
    bool Upload(IDirect3DDevice9* dev, IDirect3DTexture9*& t, int& w, int& h, const std::vector<float>& rgba, int cols, int rows);
    bool Target(IDirect3DDevice9* dev, IDirect3DTexture9* t, UINT level = 0);
    void Draw(IDirect3DDevice9* dev, ShaderId ps, int x0, int y0, int x1, int y1);
    void Point(IDirect3DDevice9* dev, DWORD s, IDirect3DBaseTexture9* t, bool mips = false);
    void Poll();

    Calls c_;
    bool ready_ = false;
    D3DFORMAT chunkFmt_ = D3DFMT_L8;
    IDirect3DVertexShader9* vs_ = nullptr;
    IDirect3DPixelShader9* ps_[kShaderCount] = {};
    IDirect3DVertexDeclaration9* decl_ = nullptr;
    IDirect3DVertexBuffer9* tri_ = nullptr; // MANAGED: one triangle over the target
    // inputs (MANAGED A32B32G32R32F, grown as needed) and the room mask (MANAGED L8)
    IDirect3DTexture9* in_[5] = {};
    int inW_[5] = {}, inH_[5] = {};
    IDirect3DTexture9* room_ = nullptr;
    int roomW_ = 0, roomH_ = 0;
    // scratch (DEFAULT): the shade (G32R32F), two R32F for the blurs and the minimum, the chunk's two (lost, seen) G32R32F and
    // one target of the chunk's format for its mips
    IDirect3DTexture9 *shade_ = nullptr, *a_ = nullptr, *b_ = nullptr, *acc_[2] = {}, *mip_ = nullptr;
    int shadeW_ = 0, shadeH_ = 0, aW_ = 0, aH_ = 0, bW_ = 0, bH_ = 0, accW_[2] = {}, accH_[2] = {}, mipW_ = 0, mipH_ = 0;
    IDirect3DStateBlock9* saved_ = nullptr;
    IDirect3DSurface9* savedRt_[4] = {};
    IDirect3DSurface9* savedDs_ = nullptr;
    IDirect3DQuery9 *qDis_ = nullptr, *qFreq_ = nullptr, *qT0_ = nullptr, *qT1_ = nullptr;
    bool qBusy_ = false;
    double gpuMs_ = -1.0;
};

} // namespace GroundShadeGpu
