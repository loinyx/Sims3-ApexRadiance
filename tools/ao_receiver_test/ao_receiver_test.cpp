#define NOMINMAX
#include "../../features/shader_patches.h"
#include "../../shaders/sim_receiver_ids.h"
#include <d3dcompiler.h>
#include <d3d9.h>
#include <algorithm>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <set>
#include <cstring>
#include <cmath>
#include <string>

static std::vector<DWORD> Compile(const char* source, const char* entry, const char* model) {
    ID3DBlob *code = nullptr, *error = nullptr;
    const HRESULT hr = D3DCompile(source, std::strlen(source), nullptr, nullptr, nullptr, entry, model, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &error);
    if (FAILED(hr)) { if (error) std::puts(static_cast<const char*>(error->GetBufferPointer())); std::exit(2); }
    std::vector<DWORD> out(code->GetBufferSize() / 4);
    std::memcpy(out.data(), code->GetBufferPointer(), code->GetBufferSize());
    code->Release(); if (error) error->Release(); return out;
}
static std::vector<float> ReadMask(IDirect3DDevice9* dev, IDirect3DSurface9* surface) {
    IDirect3DSurface9* cpu = nullptr;
    if (FAILED(dev->CreateOffscreenPlainSurface(32, 32, D3DFMT_G32R32F, D3DPOOL_SYSTEMMEM, &cpu, nullptr)) ||
        FAILED(dev->GetRenderTargetData(surface, cpu))) std::exit(2);
    D3DLOCKED_RECT lock{}; cpu->LockRect(&lock, nullptr, D3DLOCK_READONLY);
    std::vector<float> out(2048);
    for (int y = 0; y < 32; ++y) std::memcpy(out.data() + y * 64, static_cast<char*>(lock.pBits) + y * lock.Pitch, 256);
    cpu->UnlockRect(); cpu->Release(); return out;
}
static unsigned RenderChecks(IDirect3DDevice9* dev, unsigned& checks) {
    unsigned failures = 0;
    auto check = [&](bool ok) { ++checks; if (!ok) ++failures; };
    const char* material = "struct V { float4 pos:POSITION; float4 col:COLOR0; }; V VS(V v) { return v; } float4 PS(float4 col:COLOR0):COLOR0 { return col; }";
    IDirect3DTexture9* mask = nullptr; IDirect3DSurface9 *target = nullptr, *ds = nullptr;
    check(SUCCEEDED(dev->CreateTexture(32,32,1,D3DUSAGE_RENDERTARGET,D3DFMT_G32R32F,D3DPOOL_DEFAULT,&mask,nullptr)));
    check(mask && SUCCEEDED(mask->GetSurfaceLevel(0,&target)));
    check(SUCCEEDED(dev->CreateDepthStencilSurface(32,32,D3DFMT_D24S8,D3DMULTISAMPLE_NONE,0,FALSE,&ds,nullptr)));
    if (!target || !ds) return failures + 1;
    const D3DVERTEXELEMENT9 elements[] = {{0,0,D3DDECLTYPE_FLOAT4,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_POSITION,0},
        {0,16,D3DDECLTYPE_FLOAT4,D3DDECLMETHOD_DEFAULT,D3DDECLUSAGE_COLOR,0},D3DDECL_END()};
    IDirect3DVertexDeclaration9* decl = nullptr; dev->CreateVertexDeclaration(elements,&decl);
    struct V { float pos[4], col[4]; };
    const V vertices[] = {{{-1,1,.6f,1},{1,1,1,0}},{{1,1,.6f,1},{1,1,1,1}},
                          {{-1,-1,.6f,1},{1,1,1,0}},{{1,-1,.6f,1},{1,1,1,1}}};
    for (const int kind : {2,1,0}) for (const bool sm3 : {false,true}) {
        auto v = Compile(material,"VS",sm3?"vs_3_0":"vs_2_0"), p = Compile(material,"PS",sm3?"ps_3_0":"ps_2_0");
        check(ShaderPatches::MakeAoReceiverMask(v,p,kind!=0,kind==2));
        IDirect3DVertexShader9* vs = nullptr; IDirect3DPixelShader9* ps = nullptr;
        check(SUCCEEDED(dev->CreateVertexShader(v.data(),&vs))); check(SUCCEEDED(dev->CreatePixelShader(p.data(),&ps)));
        dev->SetRenderTarget(0,target); dev->SetDepthStencilSurface(ds); dev->SetVertexDeclaration(decl);
        dev->SetVertexShader(vs); dev->SetPixelShader(ps);
        dev->SetRenderState(D3DRS_ZENABLE,TRUE); dev->SetRenderState(D3DRS_ZWRITEENABLE,FALSE);
        dev->SetRenderState(D3DRS_ZFUNC,D3DCMP_LESSEQUAL); dev->SetRenderState(D3DRS_CULLMODE,D3DCULL_NONE);
        dev->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE); dev->SetRenderState(D3DRS_COLORWRITEENABLE,15);
        dev->SetRenderState(D3DRS_ALPHATESTENABLE,TRUE); dev->SetRenderState(D3DRS_ALPHAFUNC,D3DCMP_GREATER); dev->SetRenderState(D3DRS_ALPHAREF,128);
        dev->Clear(0,nullptr,D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER,0,1,0);
        dev->BeginScene(); check(SUCCEEDED(dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,vertices,sizeof(V)))); dev->EndScene();
        const auto pixels = ReadMask(dev,target);
        check(pixels[(16*32+4)*2] == 0);
        check(std::fabs(pixels[(16*32+27)*2]-(kind!=0?-.6f:.6f)) < 2.4e-7f);
        const float coverage = pixels[(16*32+27)*2+1];
        check(kind==2 ? coverage>.8f && coverage<1.0f : coverage==1.0f);
        // A depth-writing material using LESS passes once, then rejects its own replay.
        dev->SetRenderState(D3DRS_ZWRITEENABLE,TRUE);
        dev->SetRenderState(D3DRS_ZFUNC,D3DCMP_LESS);
        dev->Clear(0,nullptr,D3DCLEAR_TARGET|D3DCLEAR_ZBUFFER,0,1,0);
        dev->BeginScene(); check(SUCCEEDED(dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,vertices,sizeof(V)))); dev->EndScene();
        dev->SetRenderState(D3DRS_ZWRITEENABLE,FALSE);
        dev->Clear(0,nullptr,D3DCLEAR_TARGET,0,1,0);
        dev->BeginScene(); check(SUCCEEDED(dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,vertices,sizeof(V)))); dev->EndScene();
        check(ReadMask(dev,target)[(16*32+27)*2]==0);
        dev->SetRenderState(D3DRS_ZFUNC,D3DCMP_LESSEQUAL);
        dev->BeginScene(); check(SUCCEEDED(dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,vertices,sizeof(V)))); dev->EndScene();
        check(std::fabs(ReadMask(dev,target)[(16*32+27)*2]-(kind!=0?-.6f:.6f))<2.4e-7f);
        if (vs) vs->Release(); if (ps) ps->Release();
    }
    // Compile the actual AO shaders, then render the actual composite against the mask.
    std::ifstream source("patches/ambient_occlusion_patch.cpp",std::ios::binary);
    std::string full((std::istreambuf_iterator<char>(source)),{});
    const auto begin = full.find("R\"HLSL("), end = full.find(")HLSL\"",begin);
    if (begin == std::string::npos || end == std::string::npos) return failures+1;
    const std::string hlsl = full.substr(begin+7,end-begin-7);
    for (const char* entry : {"LinearizePS","DownPS","GtaoPS","BlurPS","CompositePS","DepthPS"}) {
        const auto code = Compile(hlsl.c_str(),entry,"ps_3_0");
        IDirect3DPixelShader9* ps = nullptr;
        check(SUCCEEDED(dev->CreatePixelShader(code.data(),&ps))); if(ps) ps->Release();
    }
    auto code = Compile(hlsl.c_str(),"CompositePS","ps_3_0");
    IDirect3DPixelShader9* composite = nullptr; dev->CreatePixelShader(code.data(),&composite);
    IDirect3DTexture9 *colour = nullptr, *ao = nullptr, *depth = nullptr;
    dev->CreateTexture(1,1,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&colour,nullptr);
    dev->CreateTexture(1,1,1,0,D3DFMT_G32R32F,D3DPOOL_MANAGED,&ao,nullptr);
    dev->CreateTexture(1,1,1,0,D3DFMT_R32F,D3DPOOL_MANAGED,&depth,nullptr);
    if (!colour || !ao || !depth) return failures+1;
    D3DLOCKED_RECT lock{}; colour->LockRect(0,&lock,nullptr,0); *static_cast<DWORD*>(lock.pBits)=0xFF808080; colour->UnlockRect(0);
    ao->LockRect(0,&lock,nullptr,0); static_cast<float*>(lock.pBits)[0]=.4f; static_cast<float*>(lock.pBits)[1]=1; ao->UnlockRect(0);
    IDirect3DSurface9 *result = nullptr, *cpu = nullptr;
    dev->CreateRenderTarget(32,32,D3DFMT_A8R8G8B8,D3DMULTISAMPLE_NONE,0,FALSE,&result,nullptr);
    dev->CreateOffscreenPlainSurface(32,32,D3DFMT_A8R8G8B8,D3DPOOL_SYSTEMMEM,&cpu,nullptr);
    dev->SetRenderTarget(0,result); dev->SetDepthStencilSurface(nullptr);
    dev->SetVertexShader(nullptr); dev->SetFVF(D3DFVF_XYZRHW|D3DFVF_TEX1);
    dev->SetPixelShader(composite); dev->SetRenderState(D3DRS_ZENABLE,FALSE); dev->SetRenderState(D3DRS_ALPHATESTENABLE,FALSE);
    dev->SetTexture(0,depth); dev->SetTexture(3,ao); dev->SetTexture(4,colour); dev->SetTexture(5,mask);
    for(DWORD s : {0u,3u,4u,5u}) {
        dev->SetSamplerState(s,D3DSAMP_MINFILTER,D3DTEXF_POINT); dev->SetSamplerState(s,D3DSAMP_MAGFILTER,D3DTEXF_POINT);
        dev->SetSamplerState(s,D3DSAMP_MIPFILTER,D3DTEXF_NONE); dev->SetSamplerState(s,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP);
        dev->SetSamplerState(s,D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP);
    }
    const float look[4]={.05f,1/.95f,0,0}, no[4]={};
    dev->SetPixelShaderConstantF(7,look,1); dev->SetPixelShaderConstantF(10,no,1); dev->SetPixelShaderConstantF(12,no,1);
    struct Q {float x,y,z,w,u,v;};
    const Q quad[]={{-.5f,-.5f,0,1,0,0},{31.5f,-.5f,0,1,1,0},{-.5f,31.5f,0,1,0,1},{31.5f,31.5f,0,1,1,1}};
    unsigned values[4][2]{};
    for(int i=0;i<4;++i) {
        depth->LockRect(0,&lock,nullptr,0); *static_cast<float*>(lock.pBits)=i==3?.3f:.6f; depth->UnlockRect(0);
        const float setting[4]={i==0?1.0f:i==1?0.0f:i==2?.5f:0.0f,1,1,1};
        dev->SetPixelShaderConstantF(11,setting,1);
        dev->BeginScene(); check(SUCCEEDED(dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,quad,sizeof(Q)))); dev->EndScene();
        check(SUCCEEDED(dev->GetRenderTargetData(result,cpu)));
        cpu->LockRect(&lock,nullptr,D3DLOCK_READONLY);
        const auto* row=reinterpret_cast<const DWORD*>(static_cast<const char*>(lock.pBits)+16*lock.Pitch);
        values[i][0]=row[4]&255; values[i][1]=row[27]&255; cpu->UnlockRect();
    }
    check(values[0][0]==values[0][1] && values[0][1]<128);
    check(values[1][0]==values[0][0] && values[1][1]==128);
    check(values[2][1]>values[0][1] && values[2][1]<128);
    check(values[3][0]==values[0][0] && values[3][1]==values[0][1]); // foreground is not a Sim receiver
    std::printf("Composite: original %u, excluded %u, half %u, foreground %u\n",values[0][1],values[1][1],values[2][1],values[3][1]);
    // Separate body/hair, transparent coverage, limits, disabled masks and foreground rejection.
    IDirect3DTexture9 *receiver=nullptr,*hairMask=nullptr;
    check(SUCCEEDED(dev->CreateTexture(1,1,1,0,D3DFMT_G32R32F,D3DPOOL_MANAGED,&receiver,nullptr)));
    check(SUCCEEDED(dev->CreateTexture(1,1,1,0,D3DFMT_G32R32F,D3DPOOL_MANAGED,&hairMask,nullptr)));
    auto put=[&](IDirect3DTexture9* t,float r,float green) {
        t->LockRect(0,&lock,nullptr,0); auto* f=static_cast<float*>(lock.pBits); f[0]=r; f[1]=green; t->UnlockRect(0);
    };
    auto render=[&](float bodyDepth,float body,float hair,float cap,float transparentDepth,float alpha,float scene,bool enabled) {
        put(receiver,bodyDepth,1); put(hairMask,transparentDepth,alpha);
        depth->LockRect(0,&lock,nullptr,0); *static_cast<float*>(lock.pBits)=scene; depth->UnlockRect(0);
        const float setting[4]={body,enabled?1.0f:0.0f,hair,cap};
        const float extra[4]={0,enabled&&transparentDepth!=0?1.0f:0.0f,0,0};
        dev->SetTexture(5,receiver); dev->SetTexture(6,hairMask);
        dev->SetSamplerState(6,D3DSAMP_MINFILTER,D3DTEXF_POINT); dev->SetSamplerState(6,D3DSAMP_MAGFILTER,D3DTEXF_POINT);
        dev->SetSamplerState(6,D3DSAMP_ADDRESSU,D3DTADDRESS_CLAMP); dev->SetSamplerState(6,D3DSAMP_ADDRESSV,D3DTADDRESS_CLAMP);
        dev->SetPixelShaderConstantF(11,setting,1); dev->SetPixelShaderConstantF(12,extra,1);
        dev->BeginScene(); check(SUCCEEDED(dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP,2,quad,sizeof(Q)))); dev->EndScene();
        check(SUCCEEDED(dev->GetRenderTargetData(result,cpu))); cpu->LockRect(&lock,nullptr,D3DLOCK_READONLY);
        unsigned value=*static_cast<const DWORD*>(lock.pBits)&255; cpu->UnlockRect(); return value;
    };
    const auto original=render(.6f,1,1,1,0,0,.6f,true);
    check(original<128);
    check(render(-.6f,1,0,1,0,0,.6f,true)==128); // hair off, body unchanged
    check(render(.6f,1,0,1,0,0,.6f,true)==original);
    check(render(.6f,0,1,1,0,0,.6f,true)==128); // body off, hair unchanged
    check(render(-.6f,0,1,1,0,0,.6f,true)==original);
    check(render(.6f,1,1,0,0,0,.6f,true)==128); // maximum shade zero
    check(render(-.6f,1,0,1,0,0,.6f,false)==original); // customization off
    check(render(0,1,0,1,-.5f,0,.6f,true)==original); // alpha zero
    check(render(0,1,0,1,-.5f,1,.6f,true)==128); // visible transparent hair
    const auto half=render(0,1,0,1,-.5f,.5f,.6f,true);
    check(half>original && half<128);
    check(render(0,1,0,1,-.5f,1,.3f,true)==original); // foreground occludes hair
    check(render(.6f,0,1,1,-.5f,0,.6f,true)==128); // alpha-zero hair retains body's adjustment
    check(render(.6f,0,1,1,-.5f,1,.6f,true)==original);
    check(render(0,0,1,1,.5f,1,.6f,true)==128); // blended Sim body, independent of hair strength
    check(render(0,1,0,1,.5f,1,.6f,true)==original);
    check(render(0,0,1,1,.5f,0,.6f,true)==original); // invisible body overlay
    const auto bodyHalf=render(0,0,1,1,.5f,.5f,.6f,true);
    check(bodyHalf>original && bodyHalf<128);
    check(render(0,0,1,1,.5f,1,.3f,true)==original); // foreground rejects body overlay
    dev->SetTexture(5,nullptr); dev->SetTexture(6,nullptr); receiver->Release(); hairMask->Release();
    dev->SetTexture(0,nullptr); dev->SetTexture(3,nullptr); dev->SetTexture(4,nullptr); dev->SetTexture(5,nullptr);
    composite->Release(); colour->Release(); ao->Release(); depth->Release(); result->Release(); cpu->Release();
    decl->Release(); mask->Release(); target->Release(); ds->Release();
    return failures;
}

static DWORD U32(const std::vector<unsigned char>& bytes, size_t p) {
    DWORD v = 0; if (p + 4 <= bytes.size()) std::memcpy(&v, bytes.data() + p, 4); return v;
}
int main(int argc, char** argv) {
    if (argc != 2) { std::puts("Usage: ao_receiver_test Shaders_Win32.precomp (read-only)"); return 2; }
    std::ifstream file(argv[1], std::ios::binary);
    const std::vector<unsigned char> bytes((std::istreambuf_iterator<char>(file)), {});
    if (bytes.size() < 40000000) return 2;
    std::vector<std::vector<DWORD>> blobs;
    for (size_t p = 0; p + 12 < 36100000; ++p) {
        if (std::memcmp(bytes.data() + p, "VSHD", 4) && std::memcmp(bytes.data() + p, "PSHD", 4)) continue;
        const size_t size = U32(bytes, p + 4);
        size_t start = p + 8;
        for (; start <= p + 12; ++start) {
            const DWORD v = U32(bytes, start);
            if (((v >> 16) == 0xFFFE || (v >> 16) == 0xFFFF) && ((v >> 8) & 255) >= 1 && ((v >> 8) & 255) <= 3) break;
        }
        if (start > p + 12 || size > 100000) return 2;
        std::vector<DWORD> code;
        for (size_t j = start; j + 4 <= p + 8 + size; j += 4) code.push_back(U32(bytes, j));
        if (code.empty() || code.back() != 0xFFFF) return 2;
        blobs.push_back(std::move(code));
    }
    std::set<std::pair<DWORD, DWORD>> pairs;
    for (size_t p = 36100000; p + 20 < bytes.size(); ++p) {
        if (std::memcmp(bytes.data() + p, "PASS", 4)) continue;
        const DWORD v = U32(bytes, p + 12), f = U32(bytes, p + 16);
        if (!v || !f || v > blobs.size() || f > blobs.size()) continue;
        const auto& ps = blobs[f - 1];
        if (std::any_of(std::begin(kSimReceiverPs), std::end(kSimReceiverPs),
                        [&](const ShaderId& id) { return IsShader(id, ps.data(), ps.size() * 4); })) pairs.emplace(v - 1, f - 1);
    }
    const HWND window = CreateWindowA("STATIC", "AO receiver test", WS_OVERLAPPED, 0, 0, 64, 64, nullptr, nullptr, GetModuleHandle(nullptr), nullptr);
    auto* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    D3DPRESENT_PARAMETERS pp{}; pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow = window;
    IDirect3DDevice9* dev = nullptr;
    if (!d3d || FAILED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &dev))) return 2;
    unsigned accepted = 0, refused = 0, failed = 0, checks = 0;
    for (const auto& [vi, pi] : pairs) {
        auto vs = blobs[vi], ps = blobs[pi];
        const bool hair=std::any_of(std::begin(kSimHairReceiverPs),std::end(kSimHairReceiverPs),
                                    [&](const ShaderId& id){return IsShader(id,ps.data(),ps.size()*4);});
        if (!ShaderPatches::MakeAoReceiverMask(vs, ps,hair,false)) {
            ++refused; ++checks;
            if (vs != blobs[vi] || ps != blobs[pi]) ++failed;
            continue;
        }
        ++accepted;
        IDirect3DVertexShader9* v = nullptr; IDirect3DPixelShader9* p = nullptr;
        const HRESULT vh = dev->CreateVertexShader(vs.data(), &v), ph = dev->CreatePixelShader(ps.data(), &p);
        checks += 2;
        if (FAILED(vh) || FAILED(ph)) {
            ++failed;
            if (failed <= 5) std::printf("Invalid pair %lu/%lu VS=%08lX PS=%08lX\n", vi, pi, vh, ph);
            if (failed == 1) {
                ID3DBlob* dis = nullptr;
                if (SUCCEEDED(D3DDisassemble(ps.data(), ps.size() * 4, 0, nullptr, &dis))) {
                    std::printf("%s\n", static_cast<const char*>(dis->GetBufferPointer())); dis->Release();
                }
            }
        }
        if (v) v->Release(); if (p) p->Release();
    }
    for(const auto& [vi,pi]:pairs) {
        if(!std::any_of(std::begin(kSimHairReceiverPs),std::end(kSimHairReceiverPs),
                       [&](const ShaderId& id){return IsShader(id,blobs[pi].data(),blobs[pi].size()*4);}))continue;
        auto vs=blobs[vi],ps=blobs[pi];
        ++checks;
        if(!ShaderPatches::MakeAoReceiverMask(vs,ps,true,true)) {
            if(vs!=blobs[vi] || ps!=blobs[pi])++failed;
            continue;
        }
        IDirect3DVertexShader9* v=nullptr; IDirect3DPixelShader9* p=nullptr;
        checks+=2;
        if(FAILED(dev->CreateVertexShader(vs.data(),&v)) || FAILED(dev->CreatePixelShader(ps.data(),&p)))++failed;
        if(v)v->Release();if(p)p->Release();
    }
    // Adversarial unsupported/malformed inputs must be rejected without modifying either shader.
    for (const auto version : {0xFFFE0101u, 0xFFFE0300u, 0u}) {
        std::vector<DWORD> vs{version, 0xFFFF}, ps{0xFFFF0300, 0xFFFF};
        const auto beforeVs = vs, beforePs = ps;
        ++checks;
        if (ShaderPatches::MakeAoReceiverMask(vs, ps) || vs != beforeVs || ps != beforePs) ++failed;
    }
    failed += RenderChecks(dev,checks);
    dev->Release(); d3d->Release(); DestroyWindow(window);
    std::printf("Blobs: %zu; pairs: %zu; accepted: %u; safely refused: %u; checks: %u; failures: %u\n",
                blobs.size(), pairs.size(), accepted, refused, checks, failed);
    return failed || !accepted ? 1 : 0;
}
