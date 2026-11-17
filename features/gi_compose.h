#pragma once
// Real-time GI compose (work package S2-01b; design.md 3.3 "Compose"): the irradiance atlas the lighting reads is, per
// texel, the sum over the lamps whose layer touches its tile of colour x lerp(shown slot, incoming slot, blend). Light is
// linear, so a switch, dim or recolour is one compose of the lamp's tiles and no solve.
// Each layer and the irradiance atlas share the scene's pixel layout (GiScene::Built: one pixel a texel, kAtlasWidth
// wide); a tile's texels are a pixel list, so the pass draws one point a texel: the frame's composed pixels set to 0, then
// per lamp its pixels added (blend ONE, ONE) from its two slots. Render thread; no lock, no read back.
// tools/gi_compose_test runs this pass through the bridge pair on the server's layers against ComposeCpu.
#include <d3d9.h>
#include <cstdint>
#include <vector>

namespace GiCompose {

// ps_2_0: s0 shown slot, s1 incoming slot, c0 = (colour, blend)
extern const char* const kPixelShader;
// ps_2_0 (Scatter): s0 the source; a point's TEXCOORD0 = (source uv, alpha written)
extern const char* const kScatterShader;

// One pixel of Scatter: dst (a pixel of out) = (src's rgb, alpha)
struct ScatterPoint {
    uint32_t dst = 0, src = 0;
    float alpha = 0.0f;
};

struct LampDraw {
    IDirect3DTexture9* shown = nullptr;
    IDirect3DTexture9* incoming = nullptr; // null when not fading
    float colour[3] = {};
    float blend = 1.0f; // incoming's weight
    std::vector<uint32_t> pixels;
};

// The device calls the pass makes past Apex's own hooks (the game passes D3D9Hooks::CallOriginal*); null: the device's
struct Calls {
    HRESULT (*setTexture)(IDirect3DDevice9*, DWORD, IDirect3DBaseTexture9*) = nullptr;
    HRESULT (*drawPrimitive)(IDirect3DDevice9*, D3DPRIMITIVETYPE, UINT, UINT) = nullptr;
};

class Composer {
  public:
    // out: a render target texture width x height (A16B16G16R16F); clear: the pixels composed again this frame; draws: the
    // draw calls made. The device's state is kept (a state block) and its targets put back.
    bool Compose(IDirect3DDevice9* dev, IDirect3DPixelShader9* ps, IDirect3DTexture9* out, uint32_t width, uint32_t height, const std::vector<uint32_t>& clear,
                 const std::vector<LampDraw>& lamps, const Calls& calls, int* draws = nullptr);
    // out (a render target texture width wide, A16B16G16R16F) cleared to 0, then each point's pixel set to (src's pixel rgb,
    // the point's alpha): the irradiance atlas copied into another layout (GiIndirect's grid). Same state rules as Compose.
    bool Scatter(IDirect3DDevice9* dev, IDirect3DPixelShader9* ps, IDirect3DTexture9* src, uint32_t srcWidth, uint32_t srcHeight, IDirect3DTexture9* out, uint32_t width,
                 const std::vector<ScatterPoint>& points, const Calls& calls, int* draws = nullptr);
    void Release(); // the vertex buffers (DEFAULT pool)

  private:
    IDirect3DVertexBuffer9* vb_ = nullptr;
    IDirect3DVertexBuffer9* vb3_ = nullptr;
    UINT vbVerts_ = 0, vb3Verts_ = 0;
};

// The same sum on the CPU (rgba float layers, width x height each): out[pixel] for every pixel in `clear`
struct CpuLamp {
    const float* shown;
    const float* incoming;
    float colour[3];
    float blend;
    std::vector<uint32_t> pixels;
};
void ComposeCpu(std::vector<float>& out, const std::vector<uint32_t>& clear, const std::vector<CpuLamp>& lamps);

} // namespace GiCompose
