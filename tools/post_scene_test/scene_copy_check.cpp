// Native D3D9 lifetime checks for Picture's extracted lazy copy helper. Credits: @loinyx
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <cassert>
#include <cstdio>
template<class T> void SafeRelease(T*& p){if(p){p->Release();p=nullptr;}}
struct {
    UINT width=32,height=32;
    D3DFORMAT format=D3DFMT_A8R8G8B8;
    IDirect3DTexture9* sceneTex=nullptr;
    IDirect3DSurface9* sceneSurf=nullptr;
} gpu;
#include "picture-scene-copy.inc"
int main(){
    HWND window=CreateWindowExW(0,L"STATIC",L"Copy lifetime fixture",WS_OVERLAPPED,0,0,64,64,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    auto* d3d=Direct3DCreate9(D3D_SDK_VERSION);assert(window&&d3d);
    D3DPRESENT_PARAMETERS pp{};pp.Windowed=TRUE;pp.SwapEffect=D3DSWAPEFFECT_DISCARD;pp.hDeviceWindow=window;
    pp.BackBufferWidth=32;pp.BackBufferHeight=32;pp.BackBufferFormat=gpu.format;
    IDirect3DDevice9* dev=nullptr;
    assert(SUCCEEDED(d3d->CreateDevice(0,D3DDEVTYPE_HAL,window,D3DCREATE_SOFTWARE_VERTEXPROCESSING,&pp,&dev)));
    assert(!gpu.sceneTex&&!gpu.sceneSurf);
    assert(EnsureSceneCopy(dev));auto* first=gpu.sceneTex;
    assert(EnsureSceneCopy(dev)&&gpu.sceneTex==first);
    IDirect3DSurface9* back=nullptr;assert(SUCCEEDED(dev->GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,&back)));
    assert(SUCCEEDED(dev->ColorFill(back,nullptr,0xFF123456)));
    assert(SUCCEEDED(dev->StretchRect(back,nullptr,gpu.sceneSurf,nullptr,D3DTEXF_NONE)));
    IDirect3DSurface9* read=nullptr;
    assert(SUCCEEDED(dev->CreateOffscreenPlainSurface(32,32,gpu.format,D3DPOOL_SYSTEMMEM,&read,nullptr)));
    assert(SUCCEEDED(dev->GetRenderTargetData(gpu.sceneSurf,read)));
    D3DLOCKED_RECT lock{};assert(SUCCEEDED(read->LockRect(&lock,nullptr,D3DLOCK_READONLY)));
    for(unsigned y=0;y<32;++y)for(unsigned x=0;x<32;++x)
        assert(reinterpret_cast<const DWORD*>(static_cast<const char*>(lock.pBits)+y*lock.Pitch)[x]==0xFF123456);
    read->UnlockRect();SafeRelease(read);SafeRelease(back);
    SafeRelease(gpu.sceneSurf);SafeRelease(gpu.sceneTex);
    gpu.format=D3DFMT_UNKNOWN;assert(!EnsureSceneCopy(dev)&&!gpu.sceneSurf&&!gpu.sceneTex);
    gpu.format=D3DFMT_A8R8G8B8;gpu.width=16;gpu.height=16;
    assert(EnsureSceneCopy(dev));D3DSURFACE_DESC desc{};gpu.sceneSurf->GetDesc(&desc);assert(desc.Width==16&&desc.Height==16);
    SafeRelease(gpu.sceneSurf);SafeRelease(gpu.sceneTex);SafeRelease(dev);SafeRelease(d3d);DestroyWindow(window);
    puts("PASS: lazy copy allocation, reuse, 1024 exact copied pixels, failed creation cleanup and resize recreation");
}
