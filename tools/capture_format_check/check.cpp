// The capture paths' assumption: StretchRect turns a 10-bit (A2R10G10B10) surface into X8R8G8B8 on the GPU
#include <d3d9.h>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
static int fails = 0;
static void Check(bool ok, const char* what) { printf("%s %s\n", ok ? "ok  " : "FAIL", what); if (!ok) fails++; }
static bool Read(IDirect3DDevice9* dev, IDirect3DSurface9* rt, DWORD& px) {
    IDirect3DSurface9* sys = nullptr; D3DLOCKED_RECT lr{}; bool ok = false;
    if (SUCCEEDED(dev->CreateOffscreenPlainSurface(64, 64, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, nullptr)) && SUCCEEDED(dev->GetRenderTargetData(rt, sys)) &&
        SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) { px = static_cast<DWORD*>(lr.pBits)[32 * lr.Pitch / 4 + 32]; sys->UnlockRect(); ok = true; }
    if (sys) sys->Release();
    return ok;
}
static bool Near(DWORD a, DWORD b) { for (int s = 0; s < 24; s += 8) if (abs(int((a >> s) & 255) - int((b >> s) & 255)) > 1) return false; return true; }
int main() {
    HWND wnd = CreateWindowA("STATIC", "fmt35", WS_OVERLAPPED, 0, 0, 64, 64, nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    const DWORD want = 0x00336699;
    for (int bbTen = 0; bbTen < 2; bbTen++) {
        D3DPRESENT_PARAMETERS pp{}; pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow = wnd;
        pp.BackBufferWidth = pp.BackBufferHeight = 64; pp.BackBufferFormat = bbTen ? D3DFMT_A2R10G10B10 : D3DFMT_X8R8G8B8;
        IDirect3DDevice9* dev = nullptr;
        if (FAILED(d3d->CreateDevice(0, D3DDEVTYPE_HAL, wnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev))) { printf("skip  device with a %s back buffer refused\n", bbTen ? "10-bit" : "8-bit"); continue; }
        IDirect3DSurface9 *src = nullptr, *dst = nullptr;
        if (bbTen) dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &src);
        else dev->CreateRenderTarget(64, 64, D3DFMT_A2R10G10B10, D3DMULTISAMPLE_NONE, 0, FALSE, &src, nullptr);
        Check(src != nullptr, bbTen ? "10-bit back buffer" : "10-bit render target");
        Check(SUCCEEDED(dev->CreateRenderTarget(64, 64, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &dst, nullptr)), "8-bit target");
        if (src && dst) {
            Check(SUCCEEDED(dev->ColorFill(src, nullptr, D3DCOLOR_XRGB(0x33, 0x66, 0x99))), "fill the 10-bit surface");
            for (D3DTEXTUREFILTERTYPE f : {D3DTEXF_NONE, D3DTEXF_LINEAR}) {
                DWORD px = 0;
                const bool copied = SUCCEEDED(dev->StretchRect(src, nullptr, dst, nullptr, f));
                Check(copied, f == D3DTEXF_NONE ? "StretchRect 10 -> 8 bit (screenshot, NONE)" : "StretchRect 10 -> 8 bit (video, LINEAR)");
                if (copied) { Check(Read(dev, dst, px) && Near(px & 0xFFFFFF, want), "the 8-bit copy keeps the colour"); printf("      read %06lX, want %06lX\n", px & 0xFFFFFF, want); }
            }
        }
        if (src) src->Release(); if (dst) dst->Release(); dev->Release();
    }
    d3d->Release(); DestroyWindow(wnd);
    printf("%s\n", fails ? "FAILED" : "PASS");
    return fails ? 1 : 0;
}
