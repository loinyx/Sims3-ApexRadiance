// Native D3D9 boundary regression check. No game hooks or game files are used.
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <cstdio>
#include "d3d9_hooks.h"
#include "apex_log.h"
#include "depth_share.h"
#include "d3d9_extra_hooks.h"
namespace ApexLog {
void Write(Level, const std::string&, const std::source_location&) noexcept {}
bool DebugOn() noexcept { return false; }
}
namespace HookGuard {
void Note(const char*) noexcept {}
void NoteAt(const char*, const void*) noexcept {}
}
namespace D3D9Hooks {
bool RegisterPresent(const std::string&, PresentHook, Priority) { return true; }
bool RegisterSetRenderTarget(const std::string&, SetRenderTargetHook, Priority) { return true; }
bool RegisterDrawIndexedPrimitive(const std::string&, DrawIndexedPrimitiveHook, Priority) { return true; }
bool RegisterDrawPrimitive(const std::string&, DrawPrimitiveHook, Priority) { return true; }
void UnregisterAll(const std::string&) {}
}
IDirect3DSurface9* expectedDepth = nullptr;
bool internalPass = false;
namespace DepthShare { IDirect3DSurface9* Surface() { return expectedDepth; } bool InternalPass() { return internalPass; } }
namespace ExtraHooks {
bool EnsureInstalled(IDirect3DDevice9*) { return true; }
void SetBeforeStretchRect(BeforeStretchRect) {}
HRESULT RawGetDepthStencilSurface(IDirect3DDevice9* d, IDirect3DSurface9** s) { return d->GetDepthStencilSurface(s); }
}
#include "game_addresses.h"
namespace GameAddr { uintptr_t Get(Id) { return 0; } }
namespace ShaderCache { bool PrecompileComplete() { return true; } }
#include "features/post_scene.cpp"
std::vector<int> applied;
void Ao(IDirect3DDevice9*) { applied.push_back(10); }
void Aa(IDirect3DDevice9*) { applied.push_back(20); }
void Blur(IDirect3DDevice9*) { applied.push_back(30); }
void Colour(IDirect3DDevice9*) { applied.push_back(40); }
int checks = 0;
void Check(bool ok, const char* text) { ++checks; if (!ok) { std::fprintf(stderr,"FAIL: %s\n",text); std::exit(1); } }
int main() {
    HWND window = CreateWindowExW(0,L"STATIC",L"Apex boundary check",WS_OVERLAPPED,0,0,64,64,nullptr,nullptr,GetModuleHandleW(nullptr),nullptr);
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!window || !d3d) return 2;
    D3DPRESENT_PARAMETERS pp{}; pp.Windowed=TRUE; pp.SwapEffect=D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow=window;
    pp.BackBufferWidth=512; pp.BackBufferHeight=512; pp.BackBufferFormat=D3DFMT_A8R8G8B8; pp.EnableAutoDepthStencil=TRUE; pp.AutoDepthStencilFormat=D3DFMT_D16;
    IDirect3DDevice9* dev=nullptr;
    if (FAILED(d3d->CreateDevice(D3DADAPTER_DEFAULT,D3DDEVTYPE_HAL,window,D3DCREATE_SOFTWARE_VERTEXPROCESSING,&pp,&dev))) return 2;
    dev->GetDepthStencilSurface(&expectedDepth);
    IDirect3DSurface9* other=nullptr;
    Check(SUCCEEDED(dev->CreateDepthStencilSurface(512,512,D3DFMT_D16,D3DMULTISAMPLE_NONE,0,TRUE,&other,nullptr)),"alternate depth surface");
    g_effects={{10,Ao},{20,Aa},{30,Blur},{40,Colour}};
    auto draw=[&] { D3D9Hooks::DeviceContext ctx{dev};OnGameDraw(ctx); };
    auto frame=[&](int draws) { applied.clear();dev->SetDepthStencilSurface(expectedDepth);OnFrameBoundary(dev);dev->SetRenderState(D3DRS_ZENABLE,D3DZB_TRUE);dev->SetRenderState(D3DRS_ZWRITEENABLE,TRUE);dev->SetRenderState(D3DRS_ZFUNC,D3DCMP_LESSEQUAL);for(int i=0;i<draws;++i)draw(); };
    auto boundary=[&] { dev->SetRenderState(D3DRS_ZENABLE,D3DZB_FALSE);draw(); };
    frame(20);boundary();Check(applied==std::vector<int>({10,20,30,40}),"normal order");boundary();AtEndSceneBeforeOverlay(dev);Check(applied.size()==4,"normal frame runs once");
    frame(20);AtEndSceneBeforeOverlay(dev);Check(applied==std::vector<int>({10,20,30,40}),"hidden UI fallback order");AtEndSceneBeforeOverlay(dev);Check(applied.size()==4,"fallback runs once");
    frame(20);dev->SetDepthStencilSurface(other);boundary();Check(applied.empty()&&!g_done,"invalid depth does not consume effects");
    dev->SetDepthStencilSurface(expectedDepth);boundary();AtEndSceneBeforeOverlay(dev);Check(applied.empty(),"no retry over already drawn UI");
    dev->SetRenderState(D3DRS_ZENABLE,D3DZB_TRUE);draw();boundary();Check(applied==std::vector<int>({10,20,30,40}),"resumed real scene accepts later boundary");
    frame(kMinSceneDraws-1);boundary();AtEndSceneBeforeOverlay(dev);Check(applied.empty(),"incomplete scene skipped");
    frame(20);internalPass=true;boundary();Check(applied.empty(),"internal draw ignored");internalPass=false;boundary();Check(applied.size()==4,"real boundary after internal draw");
    frame(20);OnPreReset(dev);AtEndSceneBeforeOverlay(dev);Check(applied.empty(),"reset prevents stale surfaces");
    IDirect3DSurface9* bb=nullptr;dev->GetBackBuffer(0,0,D3DBACKBUFFER_TYPE_MONO,&bb);

    IDirect3DSurface9* snapshot=nullptr;
    Check(SUCCEEDED(dev->CreateRenderTarget(2048,1024,D3DFMT_A8R8G8B8,D3DMULTISAMPLE_NONE,0,FALSE,&snapshot,nullptr)),"game-size colour scratch target");
    RECT tile{0,0,256,256};
    auto copyRect=[&](const RECT* s,const RECT* d,D3DTEXTUREFILTERTYPE f=D3DTEXF_POINT) { BeforeColourTile(dev,bb,s,snapshot,d,f); };
    auto copy=[&] { copyRect(&tile,&tile); };
    auto hidden=[&] { dev->SetRenderState(D3DRS_ZWRITEENABLE,FALSE);dev->SetRenderState(D3DRS_ZFUNC,D3DCMP_LESSEQUAL);dev->SetRenderState(D3DRS_COLORWRITEENABLE,7); };
    frame(20);WorldSession::InWorld();Sleep(501);Check(WorldSession::InWorld(),"fixture passes actual unknown-world readiness gate");
    frame(20);hidden();copy();Check(applied.empty()&&!g_done,"unknown target keeps hidden fallback");
    AtEndSceneBeforeOverlay(dev);Check(applied==std::vector<int>({10,20,30,40}),"unknown target EndScene fallback preserved");
    frame(20);boundary();copy();Check(g_knownTileTarget==snapshot&&applied.size()==4,"visible reference learns target without extra effects");
    frame(20);hidden();copy();Check(applied==std::vector<int>({10,20,30,40})&&g_tileBoundary,"hidden chain runs before learned colour tile");
    copy();boundary();AtEndSceneBeforeOverlay(dev);Check(applied.size()==4,"tile and later hooks never run twice");
    frame(20);hidden();copyRect(nullptr,nullptr);Check(applied.empty(),"full surface copies excluded");
    frame(20);hidden();copyRect(&tile,&tile,D3DTEXF_LINEAR);Check(applied.empty(),"refraction linear copies excluded");
    RECT next{256,0,512,256};frame(20);hidden();copyRect(&next,&tile);Check(applied.empty(),"later source tile excluded");
    RECT smallerTile{0,0,128,128};frame(20);hidden();copyRect(&smallerTile,&smallerTile);Check(applied.empty(),"other partial copy excluded");
    frame(20);hidden();copyRect(&tile,&next);Check(applied.empty(),"different destination rectangle excluded");
    frame(20);hidden();internalPass=true;copy();internalPass=false;Check(applied.empty(),"internal copy excluded");
    frame(kMinSceneDraws-1);hidden();copy();Check(applied.empty(),"short scene copy excluded");
    frame(20);hidden();dev->SetDepthStencilSurface(other);copy();Check(applied.empty()&&!g_done,"incompatible depth leaves fallback pending");
    frame(20);hidden();dev->SetRenderState(D3DRS_ZWRITEENABLE,TRUE);copy();Check(applied.empty(),"depth-writing copy excluded");
    frame(20);hidden();dev->SetRenderState(D3DRS_ZFUNC,D3DCMP_ALWAYS);copy();Check(applied.empty(),"unrecorded depth function excluded");
    frame(20);hidden();dev->SetRenderState(D3DRS_COLORWRITEENABLE,15);copy();Check(applied==std::vector<int>({10,20,30,40})&&g_tileBoundary,"recorded RGBA mask runs before colour tile");
    copy();boundary();AtEndSceneBeforeOverlay(dev);Check(applied.size()==4,"RGBA boundary runs once");
    for(DWORD mask=0;mask<16;++mask) { if(mask==7||mask==15)continue;frame(20);hidden();dev->SetRenderState(D3DRS_COLORWRITEENABLE,mask);copy();Check(applied.empty(),"partial colour masks excluded"); }
    frame(20);hidden();g_uiDrawSeen=true;copy();Check(applied.empty(),"cannot run over already-drawn UI");
    frame(20);hidden();g_rejectedBoundary=true;copy();Check(applied.empty(),"rejected boundary cannot retry");
    IDirect3DSurface9* unknown=nullptr;dev->CreateRenderTarget(2048,1024,D3DFMT_A8R8G8B8,D3DMULTISAMPLE_NONE,0,FALSE,&unknown,nullptr);
    frame(20);hidden();BeforeColourTile(dev,bb,&tile,unknown,&tile,D3DTEXF_POINT);Check(applied.empty(),"same dimensions but unknown target excluded");unknown->Release();
    frame(20);hidden();OnPreReset(dev);copy();Check(!g_knownTileTarget&&applied.empty(),"Reset invalidates learnt target identity");
    snapshot->Release();bb->Release();
    other->Release();expectedDepth->Release();expectedDepth=nullptr;dev->Release();d3d->Release();DestroyWindow(window);
    std::printf("PASS: %d native post-scene checks\n",checks);
}
