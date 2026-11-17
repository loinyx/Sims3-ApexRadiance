// dxvk_smoke: loads the given x64 d3d9.dll (a DXVK build), creates a windowed device on a hidden window, draws one
// cleared frame and prints the adapter, a CRC32 of D3DCAPS9 and the device's result codes. Two DXVK builds that print the
// same lines expose the same caps to the game. DXVK writes its own log (version, device extensions) to d3d9.log in the
// working folder unless DXVK_LOG_PATH says otherwise.
// Build (x64 developer prompt): cl /nologo /EHsc /O2 dxvk_smoke.cpp user32.lib
// Run: dxvk_smoke.exe <path to d3d9.dll>
#include <windows.h>
#include <d3d9.h>
#include <cstdio>
#include <cstdint>

static uint32_t crc32(const void* data, size_t n) {
  uint32_t c = 0xFFFFFFFFu;
  for (size_t i = 0; i < n; i++) {
    c ^= static_cast<const uint8_t*>(data)[i];
    for (int k = 0; k < 8; k++) c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1u)));
  }
  return ~c;
}

int main(int argc, char** argv) {
  if (argc < 2) { std::fprintf(stderr, "usage: dxvk_smoke <d3d9.dll>\n"); return 2; }
  HMODULE dll = LoadLibraryA(argv[1]);
  if (!dll) { std::fprintf(stderr, "LoadLibrary failed: %lu\n", GetLastError()); return 1; }
  using Create9 = IDirect3D9* (WINAPI*)(UINT);
  auto create = reinterpret_cast<Create9>(GetProcAddress(dll, "Direct3DCreate9"));
  IDirect3D9* d3d = create ? create(D3D_SDK_VERSION) : nullptr;
  if (!d3d) { std::fprintf(stderr, "Direct3DCreate9 failed\n"); return 1; }

  D3DADAPTER_IDENTIFIER9 id = {};
  d3d->GetAdapterIdentifier(D3DADAPTER_DEFAULT, 0, &id);
  D3DCAPS9 caps = {};
  HRESULT hrCaps = d3d->GetDeviceCaps(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, &caps);
  std::printf("adapter: %s (vendor %04lx device %04lx)\n", id.Description, id.VendorId, id.DeviceId);
  std::printf("caps: hr=%08lx crc32=%08x\n", static_cast<unsigned long>(hrCaps), crc32(&caps, sizeof(caps)));
  // the formats TS3 and Apex Radiance ask for
  const struct { D3DFORMAT fmt; DWORD usage; D3DRESOURCETYPE type; const char* name; } fmts[] = {
    { static_cast<D3DFORMAT>(MAKEFOURCC('I', 'N', 'T', 'Z')), D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_TEXTURE, "INTZ" },
    { D3DFMT_A16B16G16R16F, D3DUSAGE_RENDERTARGET, D3DRTYPE_TEXTURE, "A16B16G16R16F rt" },
    { D3DFMT_R32F, D3DUSAGE_RENDERTARGET, D3DRTYPE_TEXTURE, "R32F rt" },
  };
  for (auto& f : fmts)
    std::printf("format %s: %08lx\n", f.name, static_cast<unsigned long>(d3d->CheckDeviceFormat(
      D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, f.usage, f.type, f.fmt)));

  WNDCLASSA wc = {};
  wc.lpfnWndProc = DefWindowProcA;
  wc.hInstance = GetModuleHandleA(nullptr);
  wc.lpszClassName = "dxvk_smoke";
  RegisterClassA(&wc);
  HWND wnd = CreateWindowA(wc.lpszClassName, "dxvk_smoke", WS_OVERLAPPEDWINDOW, 0, 0, 256, 256, nullptr, nullptr, wc.hInstance, nullptr);
  D3DPRESENT_PARAMETERS pp = {};
  pp.Windowed = TRUE;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.BackBufferFormat = D3DFMT_X8R8G8B8;
  pp.hDeviceWindow = wnd;
  IDirect3DDevice9* dev = nullptr;
  HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev);
  std::printf("CreateDevice: %08lx\n", static_cast<unsigned long>(hr));
  if (FAILED(hr)) return 1;
  HRESULT hrClear = dev->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(32, 64, 128), 1.0f, 0);
  HRESULT hrPresent = dev->Present(nullptr, nullptr, nullptr, nullptr);
  std::printf("Clear: %08lx Present: %08lx\n", static_cast<unsigned long>(hrClear), static_cast<unsigned long>(hrPresent));
  dev->Release();
  d3d->Release();
  DestroyWindow(wnd);
  return 0;
}
