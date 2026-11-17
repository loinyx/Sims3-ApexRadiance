// dxvk_gi_pixel_pass: the bridge server's per-pixel shadow pass (native/remix-bridge/gi/gi_pixel_pass.h, real-time GI
// work package XR-06) without the bridge and without the game. On the given x64 d3d9.dll it draws a floor into an INTZ
// depth texture through Direct3D 9, then has the pass trace one lamp against two capsules and one static box and
// reads the four outputs back.
// Expected: the floor under the first capsule's shadow is blocked, a floor point away from every caster traces no
// ray, a floor point behind the static box is traced and gets no light to shadow, the guide holds the floor's normal
// and depth; the same after 100 one-sample passes and after a device reset; a wrong output is refused as a fault.
// Stock DXVK: prints "[GiPixel] off: ..." and exits 0.
// Build and run: tools/gi_server_test/run.ps1 -BridgePass <path to d3d9.dll>
#include <windows.h>
#include <d3d9.h>
#include <DirectXMath.h>
#include <cmath>
#include <cstdio>
#include <new>
#include "gi_pixel_spv.h"
#include "../../remix-bridge/gi/gi_pixel_pass.h"
using namespace DirectX;

static const UINT kW = 640, kH = 360, kQW = kW / 4, kQH = kH / 4;
#define CHECK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { std::printf("FAIL %s: %08lx\n", #x, (unsigned long)hr_); return 1; } } while (0)

static float Half(uint16_t h) {
  const int e = h >> 10 & 31, m = h & 1023;
  const float v = e == 0 ? std::ldexp(static_cast<float>(m), -24) : e == 31 ? 65504.0f : std::ldexp(static_cast<float>(m + 1024), e - 25);
  return h & 0x8000 ? -v : v;
}

struct Targets {
  IDirect3DTexture9* depth = nullptr;
  IDirect3DSurface9* depthSurface = nullptr;
  IDirect3DTexture9* color = nullptr;
  IDirect3DSurface9* colorSurface = nullptr;
  IDirect3DTexture9* out[kGiPixelOutputs] = {};
  IDirect3DTexture9* wrong = nullptr;  // a wrong output
  void Release() {
    for (IUnknown* u : {static_cast<IUnknown*>(depthSurface), static_cast<IUnknown*>(depth), static_cast<IUnknown*>(colorSurface), static_cast<IUnknown*>(color),
                        static_cast<IUnknown*>(out[0]), static_cast<IUnknown*>(out[1]), static_cast<IUnknown*>(out[2]), static_cast<IUnknown*>(out[3]), static_cast<IUnknown*>(wrong)})
      if (u) u->Release();
    *this = Targets();
  }
  HRESULT Create(IDirect3DDevice9* dev) {
    HRESULT hr = dev->CreateTexture(kW, kH, 1, D3DUSAGE_DEPTHSTENCIL, static_cast<D3DFORMAT>(MAKEFOURCC('I', 'N', 'T', 'Z')), D3DPOOL_DEFAULT, &depth, nullptr);
    if (SUCCEEDED(hr)) hr = depth->GetSurfaceLevel(0, &depthSurface);
    if (SUCCEEDED(hr)) hr = dev->CreateTexture(kW, kH, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &color, nullptr);
    if (SUCCEEDED(hr)) hr = color->GetSurfaceLevel(0, &colorSurface);
    for (uint32_t i = 0; i < kGiPixelOutputs && SUCCEEDED(hr); i++)
      hr = dev->CreateTexture(kQW, kQH, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &out[i], nullptr);
    if (SUCCEEDED(hr)) hr = dev->CreateTexture(kQW / 2, kQH, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &wrong, nullptr);
    return hr;
  }
};

struct Scene {
  XMMATRIX view, proj;
  XMVECTOR eye;
  GiPixelBlock block{};
  Scene() {
    eye = XMVectorSet(0, 6, -6, 1);
    view = XMMatrixLookAtLH(eye, XMVectorZero(), XMVectorSet(0, 1, 0, 0));
    proj = XMMatrixPerspectiveFovLH(XM_PI / 3, static_cast<float>(kW) / kH, 0.1f, 50.0f);
    XMStoreFloat4x4(reinterpret_cast<XMFLOAT4X4*>(block.invViewProj), XMMatrixInverse(nullptr, view * proj));
    XMStoreFloat3(reinterpret_cast<XMFLOAT3*>(block.cam), eye);
    XMStoreFloat3(reinterpret_cast<XMFLOAT3*>(block.viewZ), XMVector3Normalize(XMVectorNegate(eye)));
    block.ambient = 0.05f, block.zFar = 50.0f, block.bias = 0.01f, block.radiusScale = 1.0f, block.samples = 16;
    block.lamps = 1, block.lamp[0] = {{2, 3, 0}, 0.05f, {10, 10, 10}, 0};
    block.capsules = 2, block.casters = 2, block.boxes = 1;
    block.capsule[0] = {{0, 0.3f, 0}, 0.25f, {0, 1.3f, 0}, 0};
    block.caster[0] = {{-0.25f, 0.05f, -0.25f}, 0, {0.25f, 1.55f, 0.25f}, 0};
    // The second capsule stands where the static box hides the lamp: its floor is traced, with no light left to shadow
    block.capsule[1] = {{3.77f, 0.2f, 0}, 0.15f, {3.77f, 0.5f, 0}, 0};
    block.caster[1] = {{3.62f, 0.05f, -0.15f}, 0, {3.92f, 0.65f, 0.15f}, 0};
    const float box[12] = {0.6f, 0, 0, 3.0f, 0, 0.6f, 0, 0.5f, 0, 0, 0.6f, -0.3f};
    std::memcpy(block.box[0].m, box, sizeof box);
  }
  // The quarter-resolution pixel of a world point
  void Pixel(float x, float y, float z, int* px, int* py, float* linear) const {
    const XMVECTOR p = XMVectorSet(x, y, z, 1), c = XMVector4Transform(p, view * proj);
    const float w = XMVectorGetW(c);
    *px = static_cast<int>((XMVectorGetX(c) / w * 0.5f + 0.5f) * kQW), *py = static_cast<int>((0.5f - XMVectorGetY(c) / w * 0.5f) * kQH);
    *linear = XMVectorGetX(XMVector3Dot(XMVectorSubtract(p, eye), XMVector3Normalize(XMVectorNegate(eye)))) / block.zFar;
  }
};

static HRESULT Draw(IDirect3DDevice9* dev, const Targets& t, const Scene& s) {
  static const float floor[4][3] = {{-8, 0, -8}, {-8, 0, 8}, {8, 0, -8}, {8, 0, 8}};
  dev->SetRenderTarget(0, t.colorSurface);
  dev->SetDepthStencilSurface(t.depthSurface);
  dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, 0, 1.0f, 0);
  dev->BeginScene();
  XMFLOAT4X4 m;
  XMStoreFloat4x4(&m, s.view), dev->SetTransform(D3DTS_VIEW, reinterpret_cast<D3DMATRIX*>(&m));
  XMStoreFloat4x4(&m, s.proj), dev->SetTransform(D3DTS_PROJECTION, reinterpret_cast<D3DMATRIX*>(&m));
  dev->SetRenderState(D3DRS_LIGHTING, FALSE), dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE), dev->SetRenderState(D3DRS_ZENABLE, TRUE);
  dev->SetFVF(D3DFVF_XYZ);
  const HRESULT hr = dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, floor, 12);
  dev->EndScene();
  return hr;
}

// Reads the outputs back and checks the three floor points; returns the number of failed checks
static int Check(IDirect3DDevice9* dev, const Targets& t, const Scene& s, IDirect3DSurface9* sys, const char* when) {
  float px[kGiPixelOutputs][3][4];  // output, point, channel
  const float at[3][3] = {{-0.73f, 0, 0}, {-4, 0, 3}, {3.77f, 0, 0}};  // in the first capsule's shadow; away from all; behind the box
  float linear[3];
  for (uint32_t o = 0; o < kGiPixelOutputs; o++) {
    IDirect3DSurface9* rt = nullptr;
    D3DLOCKED_RECT lr;
    if (FAILED(t.out[o]->GetSurfaceLevel(0, &rt)) || FAILED(dev->GetRenderTargetData(rt, sys)) || FAILED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) return 100;
    for (int i = 0; i < 3; i++) {
      int x, y;
      s.Pixel(at[i][0], at[i][1], at[i][2], &x, &y, &linear[i]);
      const uint16_t* h = reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(lr.pBits) + y * lr.Pitch) + x * 4;
      for (int c = 0; c < 4; c++) px[o][i][c] = Half(h[c]);
    }
    sys->UnlockRect();
    rt->Release();
  }
  int fails = 0;
  const auto expect = [&](bool ok, const char* what) { if (!ok) std::printf("FAIL %s: %s\n", when, what), fails++; };
  expect(px[0][0][0] < 0.5f && px[0][0][2] == 1.0f, "shadow point: blocked and traced");
  expect(px[2][0][0] > 0 && px[3][0][0] > 0 && px[3][0][3] > 0, "shadow point: sums and ray count");
  expect(px[0][1][0] == 1.0f && px[0][1][2] == 0 && px[3][1][3] == 0, "far point: lit, no ray");
  expect(px[0][2][2] == 1.0f && px[3][2][0] == 0 && px[3][2][3] > 0, "point behind the static box: traced, no light reaches");
  for (int i = 0; i < 3; i++)
    expect(px[1][i][1] > 0.99f && std::fabs(px[1][i][3] - linear[i]) < 0.004f && px[0][i][3] > 0 && px[0][i][3] < 1, "guide: the floor's normal and depth");
  std::printf("%s: raw %.3f/%.3f/%.3f  den.r %.4f/%.4f/%.4f  rays %.0f/%.0f/%.0f  guide depth %.4f (floor %.4f)  %s\n", when, px[0][0][0], px[0][1][0], px[0][2][0],
              px[3][0][0], px[3][1][0], px[3][2][0], px[3][0][3], px[3][1][3], px[3][2][3], px[1][0][3], linear[0], fails ? "FAIL" : "ok");
  return fails;
}

int main(int argc, char** argv) {
  if (argc < 2) { std::fprintf(stderr, "usage: dxvk_gi_pixel_pass <d3d9.dll>\n"); return 2; }
  HMODULE dll = LoadLibraryA(argv[1]);
  auto create = dll ? reinterpret_cast<IDirect3D9* (WINAPI*)(UINT)>(GetProcAddress(dll, "Direct3DCreate9")) : nullptr;
  IDirect3D9* d3d = create ? create(D3D_SDK_VERSION) : nullptr;
  if (!d3d) { std::printf("FAIL: no Direct3D 9 from %s\n", argv[1]); return 1; }
  WNDCLASSA wc = {};
  wc.lpfnWndProc = DefWindowProcA, wc.hInstance = GetModuleHandleA(nullptr), wc.lpszClassName = "dxvk_gi_pixel_pass";
  RegisterClassA(&wc);
  HWND wnd = CreateWindowA(wc.lpszClassName, wc.lpszClassName, WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
  D3DPRESENT_PARAMETERS pp = {};
  pp.Windowed = TRUE, pp.SwapEffect = D3DSWAPEFFECT_DISCARD, pp.BackBufferFormat = D3DFMT_X8R8G8B8, pp.hDeviceWindow = wnd;
  IDirect3DDevice9* dev = nullptr;
  CHECK(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev));

  int fails = 0;
  {
    static GiPixelPass pass;  // (static: it holds the record of a pass, about 14 KB)
    if (const char* why = pass.init(dev)) {
      std::printf("[GiPixel] off: %s\n", why);
      return 0;
    }
    Scene scene;
    Targets t;
    IDirect3DSurface9* sys = nullptr;
    CHECK(dev->CreateOffscreenPlainSurface(kQW, kQH, D3DFMT_A16B16G16R16F, D3DPOOL_SYSTEMMEM, &sys, nullptr));
    char line[256];
    for (int round = 0; round < 2; round++) {  // the second round runs after a device reset
      CHECK(t.Create(dev));
      IDirect3DBaseTexture9* out[kGiPixelOutputs] = {t.out[0], t.out[1], t.out[2], t.out[3]};
      // the whole sample set in one pass: the checks do not depend on which sample a frame takes
      scene.block.samples = 16;
      CHECK(Draw(dev, t, scene));
      CHECK(pass.run(scene.block, t.depth, nullptr, out));
      fails += Check(dev, t, scene, sys, round ? "after reset, 16 samples" : "16 samples");
      // frames as the game sends them: draw, pass, present; one sample a frame
      scene.block.samples = 1;
      for (uint32_t f = 0; f < 100; f++) {
        scene.block.frame = f;
        CHECK(Draw(dev, t, scene));
        CHECK(pass.run(scene.block, t.depth, nullptr, out));
        dev->Present(nullptr, nullptr, nullptr, nullptr);
      }
      const GiPixelCounters c = pass.counters();
      if (!c.raysLast || c.faults) std::printf("FAIL: rays of the last pass %u, faults %u\n", c.raysLast, c.faults), fails++;
      pass.line(line, sizeof line);
      std::printf("%s\n", line);
      if (round == 1) {  // an output of the wrong size is refused and counted
        out[0] = t.wrong;
        const HRESULT hr = pass.run(scene.block, t.depth, nullptr, out);
        const bool ok = FAILED(hr) && pass.counters().faults == 1;
        std::printf("wrong output: %08lx, faults %u (%s) %s\n", static_cast<unsigned long>(hr), pass.counters().faults, pass.lastFault(), ok ? "ok" : "FAIL");
        fails += !ok;
      }
      pass.drop();  // the rule of the bridge: before the targets go and the device resets
      t.Release();
      if (round == 0) CHECK(dev->Reset(&pp));
    }
    sys->Release();
    pass.close();
  }
  dev->Release(), d3d->Release();
  DestroyWindow(wnd);
  std::printf("%s\n", fails ? "FAIL" : "PASS");
  return fails ? 1 : 0;
}
