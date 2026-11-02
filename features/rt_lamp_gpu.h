#pragma once
// Real-time outdoor lamps (Lamp lighting > Instant, "lampLightingMode"; docs/features/night-lighting/rt-outdoor-lamps.md): the GPU side,
// with no game access, so the offline harness (tools/rt_lamps_test) runs exactly this code on its own device.
//
//   - the lot's occluder proxy (rt_lamp_proxy.h) in a vertex buffer;
//   - up to kAtlasLamps lamps' cube shadow maps: 6 faces of kFace x kFace texels per lamp, side by side in one G16R16F atlas
//     (row k = lamp k; r = the distance to the lamp / far, g = how far that surface's plane passes from the lamp / far; 1 =
//     nothing), the nearest kept by a D24X8 depth test;
//   - a room-0 map relit: one point per texel (pre-transformed, at its texel; stream 0 the texel, stream 1 what changes with
//     the lamps) accumulates the GPU lamps of each batch (AccPS, A32B32G32R32F ping-pong), then the map is copied from the
//     game's own texture and every texel of the record composed and packed over it (ComposePS); a class-2 wall atlas is
//     composed into a scratch map over its blocks' texels before the blur and blurred the game's way (BlurRowPS / BlurColPS,
//     BlockPS) into the output.
// Every device call goes through DeviceCalls: the game passes Apex's CallOriginal* (its own hooks never see these draws).
#include <d3d9.h>
#include <array>
#include <cstdint>
#include <string>
#include <vector>
#include "rt_lamp_model.h"
#include "rt_lamp_proxy.h"

namespace RtLampGpu {

constexpr int kAtlasLamps = 16;
constexpr int kFaceDefault = 256;

struct DeviceCalls {
    HRESULT (*SetTexture)(IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9*) = nullptr;
    HRESULT (*SetRenderTarget)(IDirect3DDevice9*, DWORD, IDirect3DSurface9*) = nullptr;
    HRESULT (*SetDepthStencil)(IDirect3DDevice9*, IDirect3DSurface9*) = nullptr;
    HRESULT (*GetDepthStencil)(IDirect3DDevice9*, IDirect3DSurface9**) = nullptr;
    HRESULT (*SetVertexShader)(IDirect3DDevice9*, IDirect3DVertexShader9*) = nullptr;
    HRESULT (*SetPixelShader)(IDirect3DDevice9*, IDirect3DPixelShader9*) = nullptr;
    HRESULT (*SetVsConst)(IDirect3DDevice9*, UINT, const float*, UINT) = nullptr;
    HRESULT (*SetPsConst)(IDirect3DDevice9*, UINT, const float*, UINT) = nullptr;
    HRESULT (*DrawPrimitive)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT) = nullptr;
    HRESULT (*SetViewport)(IDirect3DDevice9*, const D3DVIEWPORT9*) = nullptr;
    void (*StateRestored)() = nullptr; // after End applied the saved state block (shaders set without Set*Shader); optional
};
DeviceCalls PlainCalls(); // the device's own methods (the harness)

// The shader objects (made by the caller: the game from its shader cache, the harness with D3DCompile)
enum ShaderId {
    kShadowVs, kTexelVs, kShadowPs, kAccPs, kComposePs, kCopyPs, kBlurRowPs, kBlurColPs, kBlockPs, kAccFastPs, kAccRlgPs, kAccRlgFastPs, kComposeRlgPs,
    kAccKeptPs, kComposeKeptPs, kCopyAtPs, kBlockKeptPs, kAccKept0Ps, kAccKept0XPs, kComposeK0Ps, kShaderCount
};
const char* EntryOf(ShaderId id);
bool IsVertex(ShaderId id);

#pragma pack(push, 1)
struct TexelVertex {
    float x, y, z, w;   // the texel (x, y; TexelVS places it)
    float p[4];         // lot position, flags (1 masked, 2 gated)
    float n[4];         // lot normal, the record's alpha (0..1)
    float base[4];      // the texel without the GPU lamps, dither (levels / 255)
    float mask[4];      // the floor mask's luminances without the GPU lamps (walls tested, none)
};
struct TexelDyn {
    float v[4]; // the GPU lamps' luminance with no walls, under (-1 none)
};
#pragma pack(pop)
static_assert(sizeof(TexelVertex) == 80 && sizeof(TexelDyn) == 16);

struct Block {
    int x0, y0, x1, y1;
};

struct Params {
    float thr = 2.0f * 3.0f / 255.0f;
    bool fade = true, reachOn = true, mask = false;
    float radius = 0.15f;      // the lamp ball (Shadow softness)
    float aa = 0.0f;           // the map's half texel at the receiver (m; 0.75 texel)
    float normalOffset = 0.5f; // shadow texels (at the face centre)
    float biasM = 0.02f, biasTexels = 0.3f;
    float nearBlocker = 0.3f;  // m: the nearest blocker the penumbra search considers
    float farDist = 64.0f;
    bool fast = false;         // AccFastPS: fewer shadow taps (a lamp being dragged)
    bool maskAllGpu = false;   // Compose: the floor mask's "all" of the GPU lamps from the last Accumulate's lamps (its only batch), not dyn.x
    // Compose (GPU room lamps): room 0's kept lamps were added with their floor mask and exit parts (AddKeptParts, extra): ComposeK0PS
    // with the second sum (texels: mask.z = the exit lamps' light left in the base, mask.w = 1 an exit ground texel)
    bool keptX = false;
    int debugLamp = -1;        // >= 0: AccPS writes that lamp's shadow share (r) and law luminance (g), -1 when it adds nothing
};

struct Map {
    int w = 0, h = 0;
    IDirect3DVertexBuffer9* vb = nullptr;  // TexelVertex x count
    IDirect3DVertexBuffer9* dyn = nullptr; // TexelDyn x count, or null (zeros)
    UINT count = 0;
    bool blur = false;                    // a class-2 wall atlas: composed before the blur, then blurred
    UINT passes = 0;
    std::vector<Block> blocks;
    IDirect3DTexture9* preblur = nullptr; // DEFAULT, w x h: the blocks' texels before the blur as the solve left them
};

// ---- GPU room lamps (developer, gpu_room_lamps.cpp): one kept room solve's texels of one map (OutdoorDelta::BuildKeptMap) ----
// Every point is a TexelVertex at its target texel (t.xy):
//   parts (AccKeptPS, into the sum's layout: the map's kept texel j at (j % accW, j / accW)): p = (sample position, bits: 1
//   the early-out, 4 the soft light cut), n = (sample normal, mult), base = (A, B, thrScale, 0); grouped by lamp (ranges)
//   texels / wallTexels (ComposeKeptPS, into the map / the class-2 wall layout): p = (base rgb, dither), n = (ramp x ambient,
//   ramp; a roofless texel: its alpha), base = (norm, k, the alpha floor, roofed 0/1), mask.xy = its place in the sum's layout
//   copies (ComposeKeptPS, into the map): a tile-edge copy, its source texel's compose point at the copy's place
//   restores (CopyAtPS, into the map): mask.xy = the place itself in the game's map (a copy of a texel no capture holds)
struct KeptBlockOut {
    int x0, y0, x1, y1; // the block in the map
    int lx, ly0, ly1;   // its rows in the layout (x0 at lx)
    bool above, below;  // the rows lit beyond that edge are in the layout
};
struct KeptMapData {
    int story = 0, id = 0, cls = 0, m = 0;
    bool blur = false; // a class-2 wall atlas: drawn only into a map of the working map's exact size
    uint64_t lot = 0;
    float thr = 0.0f;
    int w = 0, h = 0, accW = 0, accH = 0;
    std::vector<TexelVertex> parts, texels, wallTexels, copies, restores;
    std::vector<uintptr_t> lamps;               // the lamp of each range of parts
    std::vector<char> colourOnly;               // its law is its colour alone (a TubeLight)
    std::vector<std::pair<UINT, UINT>> ranges;  // first, count
    int layoutW = 0, layoutH = 0;
    std::vector<uint32_t> layout;               // the class-2 blocks before the blur with their rows beyond, layoutW x layoutH
    std::vector<Block> extRects;                // each block with its rows beyond, in the layout
    std::vector<KeptBlockOut> outs;
    UINT passes = 0;
    std::vector<std::pair<uint32_t, uint32_t>> expect; // (y << 16 | x, the texel the game left there) for the self-check
    // every lamp of the room's kept solve the GPU draws, with its position then, or (its colour alone) the signature of what its
    // share reads: one changed since (moved, a tube's intensity), the room is not drawn
    struct RoomLamp {
        uintptr_t light;
        float pos[3];
        uint64_t geo;
        bool colourOnly;
    };
    std::vector<RoomLamp> roomLamps;
};
struct KeptMapGpu { // DEFAULT pool (made again after a device reset)
    int w = 0, h = 0, accW = 0, accH = 0, layoutW = 0, layoutH = 0;
    IDirect3DVertexBuffer9 *parts = nullptr, *texels = nullptr, *walls = nullptr, *copies = nullptr, *restores = nullptr;
    UINT nTexels = 0, nWalls = 0, nCopies = 0, nRestores = 0;
    std::vector<std::pair<UINT, UINT>> ranges;
    IDirect3DTexture9* layout = nullptr;
    std::vector<Block> extRects;
    std::vector<KeptBlockOut> outs;
    UINT passes = 0;
    bool blur = false;
    size_t bytes = 0;
    void Release();
};

struct Stats {
    long shadowTiles = 0, shadowDraws = 0, accDraws = 0, composeDraws = 0, blurDraws = 0;
};

class Engine {
  public:
    explicit Engine(const DeviceCalls& calls) : c_(calls) {}
    ~Engine() { ReleaseAll(); }
    // The shaders (AddRef'd), the face size; DEFAULT resources are made on first use
    bool Init(IDirect3DDevice9* dev, void* const* shaders, int face, std::string& err);
    void ReleaseDefault(); // a device reset: every DEFAULT resource (made again on use)
    void ReleaseAll();
    bool Ready() const { return ready_; }
    D3DFORMAT AtlasFormat() const { return atlasFmt_; }
    int Face() const { return face_; }

    // The proxy (lot space); false when it has no triangle
    bool SetProxy(IDirect3DDevice9* dev, const RtLampProxy::Mesh& mesh, std::string& err);
    size_t ProxyTris() const { return proxyTris_; }
    // Lamps 0 .. n - 1 (n <= kAtlasLamps; lot space) into the atlas tiles
    bool RenderShadows(IDirect3DDevice9* dev, const float (*pos)[3], int n, float farDist, std::string& err);

    // The texel buffers of a map (MANAGED: survive a reset; defaultPool: DEFAULT, no system-memory copy below the 2 GB line,
    // made again by the caller after a reset)
    static bool MakeVb(IDirect3DDevice9* dev, const TexelVertex* v, UINT n, IDirect3DVertexBuffer9** out, bool defaultPool = false);
    static bool MakeDyn(IDirect3DDevice9* dev, const TexelDyn* v, UINT n, IDirect3DVertexBuffer9** out, bool defaultPool = false);
    // The blocks' texels before the blur (w x h, A8R8G8B8 rows) into a MANAGED texture (defaultPool: DEFAULT)
    static bool MakePreblur(IDirect3DDevice9* dev, int w, int h, const uint32_t* px, IDirect3DTexture9** out, bool defaultPool = false);

    // One batch of lamps (their shadows in tiles 0 .. n - 1) added to the map's sum; first = the sum starts at 0
    bool Accumulate(IDirect3DDevice9* dev, const Map& m, const RtLampModel::Lamp* lamps, int n, const Params& p, bool first, std::string& err);
    // The map composed into out (DEFAULT render target, w x h): the game's texture copied, the texels over it (and the blur).
    // scale (DEFAULT render target, w x h, or null): cleared to 0 and given each texel's outdoor scale (ComposePS COLOR1) as a
    // second target of the texels' draw (a blurred atlas: through a scratch map; the scale is not blurred)
    bool Compose(IDirect3DDevice9* dev, const Map& m, IDirect3DBaseTexture9* game, IDirect3DTexture9* out, const Params& p, std::string& err,
                 IDirect3DTexture9* scale = nullptr);
    // The harness: the current sum (w x h float4, row major)
    bool ReadSum(IDirect3DDevice9* dev, int w, int h, std::vector<float>& out);

    // ---- GPU lamps for every room (room_lamps_gpu.cpp): a shadow atlas of `rows` lamp tiles kept across relights (each
    // lamp keeps its tile while it stays put), per-room lamp tables, and the toned compose ----
    bool EnsureTiles(IDirect3DDevice9* dev, int rows, std::string& err);
    int TileRows() const { return tileRows_; }
    // One lamp's cube shadow map (lot space) into tile `tile` of the kept atlas
    bool RenderShadowTile(IDirect3DDevice9* dev, const float pos[3], int tile, float farDist, std::string& err);
    // The same from the proxy's triangle ranges [first, first + count) only (the object shapes that hold the lamp left out)
    bool RenderShadowTileRanges(IDirect3DDevice9* dev, const float pos[3], int tile, float farDist, const std::vector<std::pair<UINT, UINT>>& ranges, std::string& err);
    // The room table (A32B32G32R32F, kTableCols x rows; DEFAULT, written from a SYSTEMMEM copy) for the next draws
    static constexpr int kTableCols = 18;
    bool SetTable(IDirect3DDevice9* dev, const float* rgba, int rows, std::string& err);
    // One batch of lamps (lamps[i]'s shadow in tile tiles[i]) added to the map's sum, each texel taking its room's row
    bool AccumulateRlg(IDirect3DDevice9* dev, const Map& m, const RtLampModel::Lamp* lamps, const int* tiles, int n, const Params& p, bool first, std::string& err);
    // The map composed into out (raw and toned texels; the class-2 wall blur as Compose)
    bool ComposeRlg(IDirect3DDevice9* dev, const Map& m, IDirect3DBaseTexture9* game, IDirect3DTexture9* out, const Params& p, std::string& err);
    // A map's sum kept between relights (A32B32G32R32F, w x h): the next AccumulateRlg(first = false) adds to `from`; the sum
    // after the last batch is copied into `to` (a lamp edit then evaluates only the lamps that changed)
    bool SeedAcc(IDirect3DDevice9* dev, IDirect3DTexture9* from, int w, int h, std::string& err);
    bool StoreAcc(IDirect3DDevice9* dev, IDirect3DTexture9* to, int w, int h, std::string& err);
    // A render target's texels (A8R8G8B8 rows, w x h) read back (the self-check; stalls the GPU)
    static bool ReadTexture(IDirect3DDevice9* dev, IDirect3DTexture9* t, int w, int h, std::vector<uint32_t>& out);
    // A texture copied texel for texel into a render target of its size (between Begin and End)
    bool CopyTexture(IDirect3DDevice9* dev, IDirect3DBaseTexture9* src, IDirect3DTexture9* out, int w, int h, std::string& err);
    // An A32B32G32R32F render target read back (4 floats per texel, r g b a); waits for the GPU
    static bool ReadFloat4(IDirect3DDevice9* dev, IDirect3DTexture9* t, int w, int h, std::vector<float>& out);

    // ---- GPU room lamps: a kept room solve drawn over a map (between Begin and End) ----
    static bool UploadKept(IDirect3DDevice9* dev, const KeptMapData& d, KeptMapGpu& g);
    // laws / lawOk: per range of parts (0: no law, adds nothing; 1 the GPU's law; 2 the lamp's colour alone, rt_lamp_model.h
    // KeptAdd's law = that colour); game: the game's own map (the restores' source)
    // Room 0's kept lamps (Real-time outdoor lamps, between its Accumulate and Compose): their parts (TexelVertex as `parts`
    // above, at their texels' places in a w x h map) added into the current sum; laws / lawOk per range (lawOk 2: the colour alone).
    // extra: their floor mask parts (mask.x, mask.y) and exit lamps' parts (mask.z) too: AccKept0PS, and AccKept0XPS into the
    // other sum ("all", the exit light) that Compose with Params::keptX reads
    bool AddKeptParts(IDirect3DDevice9* dev, IDirect3DVertexBuffer9* vb, const std::vector<std::pair<UINT, UINT>>& ranges, const GpuLampLaw::Lamp* laws,
                      const char* lawOk, float thr, int w, int h, std::string& err, bool extra = false);
    // out: w x h, the room's own map (at most the working map's size: its top-left corner; a blurred wall atlas exactly)
    bool RelightKept(IDirect3DDevice9* dev, const KeptMapGpu& k, const GpuLampLaw::Lamp* laws, const char* lawOk, float thr, IDirect3DBaseTexture9* game,
                     IDirect3DTexture9* out, int w, int h, std::string& err);

    // The state of the device around a series of passes (render targets, depth, every state a state block holds)
    bool Begin(IDirect3DDevice9* dev); // false: no state block could be made, nothing was changed (End is not called)
    void End(IDirect3DDevice9* dev);

    const Stats& GetStats() const { return stats_; }
    size_t DefaultBytes() const;

  private:
    bool EnsureAtlas(IDirect3DDevice9* dev, std::string& err);
    bool EnsureScratch(IDirect3DDevice9* dev, int w, int h, std::string& err);
    bool EnsureZeros(IDirect3DDevice9* dev, UINT count);
    bool Target(IDirect3DDevice9* dev, IDirect3DTexture9* t, int w, int h);
    void Quad(IDirect3DDevice9* dev, int w, int h, float u1 = 1.0f, float v1 = 1.0f);
    void TexelTarget(IDirect3DDevice9* dev, int w, int h);
    void Blocks(IDirect3DDevice9* dev, const Map& m);
    bool DrawKeptParts(IDirect3DDevice9* dev, IDirect3DVertexBuffer9* vb, const std::vector<std::pair<UINT, UINT>>& ranges, const GpuLampLaw::Lamp* laws,
                       const char* lawOk, float thr, int w, int h, ShaderId ps = kAccKeptPs);

    DeviceCalls c_;
    bool ready_ = false;
    int face_ = kFaceDefault;
    IDirect3DVertexShader9* vs_ = nullptr, *texelVs_ = nullptr;
    IDirect3DPixelShader9* ps_[kShaderCount] = {};
    IDirect3DVertexDeclaration9* texelDecl_ = nullptr;
    IDirect3DVertexDeclaration9* shadowDecl_ = nullptr;
    IDirect3DVertexBuffer9* proxy_ = nullptr; // MANAGED
    size_t proxyTris_ = 0;
    IDirect3DTexture9* atlas_ = nullptr;      // G16R16F (or G32R32F) render target
    D3DFORMAT atlasFmt_ = D3DFMT_UNKNOWN;
    IDirect3DSurface9* atlasDepth_ = nullptr; // D24X8
    IDirect3DTexture9* acc_[2] = {};          // A32B32G32R32F
    IDirect3DTexture9* scratch_[5] = {};      // A8R8G8B8: before the blur, rows-only x 2, full x 2
    int scratchW_ = 0, scratchH_ = 0;
    int accCur_ = 0;
    IDirect3DVertexBuffer9* zeros_ = nullptr;
    UINT zerosCount_ = 0;
    IDirect3DTexture9* tiles_ = nullptr;      // the kept atlas (G16R16F or G32R32F), 6 faces x rows tiles
    IDirect3DSurface9* tilesDepth_ = nullptr;
    D3DFORMAT tilesFmt_ = D3DFMT_UNKNOWN;
    int tileRows_ = 0;
    IDirect3DTexture9* table_ = nullptr;      // DEFAULT A32B32G32R32F kTableCols x tableRows_
    IDirect3DTexture9* tableSys_ = nullptr;   // its SYSTEMMEM copy
    int tableRows_ = 0, tableCap_ = 0;
    int blendOk_ = -1; // the device adds into an A32B32G32R32F target (RelightKept; -1 not asked yet)
    bool ComposeImpl(IDirect3DDevice9* dev, const Map& m, IDirect3DBaseTexture9* game, IDirect3DTexture9* out, const Params& p, ShaderId ps, std::string& err,
                     IDirect3DTexture9* scale = nullptr);
    IDirect3DStateBlock9* saved_ = nullptr;
    IDirect3DSurface9* savedRt_[4] = {};
    IDirect3DSurface9* savedDs_ = nullptr;
    Stats stats_;
};

// The cube faces (right, up, forward): the same table as the HLSL
extern const float kFaces[6][3][3];
// The lamp's direction d (lot space) to its atlas texel (face, x, y) as AccPS reads it (the harness)
void FaceTexel(const float d[3], int face, int& f, int& x, int& y);

} // namespace RtLampGpu
