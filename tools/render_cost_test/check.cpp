#define NOMINMAX
#include <Windows.h>
#include <intrin.h>
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <vector>
#include "gpu_lamp_law.h"
#include "shader_constant_pair.h"
#include "refpack_candidate.h"
namespace GpuLampLaw {
__declspec(noinline) void EvalBefore(const Lamp& L, const float p[3], const float n[3], float wallReach, float out[3]) {
    float l[3] = {L.pos[0] - p[0], L.pos[1] - p[1], L.pos[2] - p[2]};
    const float d2 = std::max(l[0] * l[0] + l[1] * l[1] + l[2] * l[2], 1e-4f);
    const float inv = 1.0f / std::sqrt(d2);
    for (float& v : l) v *= inv;
    const float ndl = Sat(n[0] * l[0] + n[1] * l[1] + n[2] * l[2]);
    const float f = std::min(ndl * (inv * inv) * L.W, L.cmax);
    const float al = L.axis[0] * l[0] + L.axis[1] * l[1] + L.axis[2] * l[2];
    const float t1 = (al + L.off1) * -L.sc1, t2 = (al - L.off2) * L.sc2;
    const float tsel = t1 > 0.0f ? t1 : t2;
    const float m = Sat((std::max(t1, t2) - 1.0f) * 1e4f + 1.0f);
    const float t = Sat(std::max(tsel, m));
    const float q = d2 * L.invR2;
    float win = Sat(1.0f - q * q);
    win *= win;
    float share = std::min(win * ((1.0f / d2) * L.r0sq), 1.0f);
    share = (share - 1.0f) * wallReach + 1.0f;
    const float s = f * share;
    for (int c = 0; c < 3; c++) out[c] = (L.spill[c] - L.spill[c] * t + t) * s * L.colour[c];
}
}

void PairChecks() {
    uint32_t seed = 31;
    for (unsigned trial = 0; trial < 2048; ++trial) {
        float registers[16][4];
        for (auto& row : registers) for (auto& v : row) { seed = seed * 1664525u + 1013904223u; std::memcpy(&v, &seed, 4); }
        for (uint32_t b : {4u,5u,7u,9u}) for (int failRead=0;failRead<2;++failRead) for(int failWrite=0;failWrite<2;++failWrite) {
            auto original = std::vector<float>(&registers[0][0], &registers[0][0]+64);
            unsigned reads=0,writes=0;
            ShaderConstantPair::Block saved(4,b,[&](uint32_t reg,float* values,uint32_t count) {
                ++reads;
                if(count>1 && failRead) { values[0]=99; return false; }
                std::memcpy(values,registers[reg],count*4*sizeof(float));return true;
            });
            auto set=[&](uint32_t reg,const float* values,uint32_t count) {
                ++writes; if(count>1 && failWrite) return false;
                std::memcpy(registers[reg],values,count*4*sizeof(float));return true;
            };
            float x[4],y[4];std::memcpy(x,registers[11],sizeof x);std::memcpy(y,registers[12],sizeof y);
            auto expected=original;
            std::memcpy(expected.data()+4*4,x,sizeof x);std::memcpy(expected.data()+b*4,y,sizeof y);
            saved.Apply(x,y,set);
            assert(!std::memcmp(expected.data(),registers,sizeof registers));
            saved.Restore(set);
            assert(!std::memcmp(original.data(),registers,sizeof registers));
            const bool batch=b>4 && b<8;
            assert(reads==(batch?(failRead?3u:1u):2u));
            assert(writes==(batch && !failRead?(failWrite?6u:2u):4u));
        }
    }
    std::cout << "32768 constant blocks: exact bits during and after draw, intervening rows, failed reads/writes and call counts passed\n";
}
void LampChecks() {
    const unsigned saved = _mm_getcsr();
    uint32_t seed = 17;
    auto value = [&]() { seed = seed * 1664525u + 1013904223u; return float(seed & 65535u) / 65536.0f; };
    for (unsigned rounding : {0u, 0x2000u, 0x4000u, 0x6000u}) {
        _mm_setcsr((saved & ~0x6000u) | rounding);
        for (unsigned i = 0; i < 32768; ++i) {
            GpuLampLaw::Lamp l;
            float p[3], n[3];
            for(int c=0;c<3;++c) { l.pos[c]=value()*1000; p[c]=value()*1000; n[c]=value()*2-1; l.colour[c]=value(); l.axis[c]=value()*2-1; l.spill[c]=value(); }
            l.W=value()*100; l.cmax=value()*4; l.sc1=value()*2; l.off1=value()*2-1; l.sc2=value()*2; l.off2=value()*2-1;
            l.r0sq=value()*100; l.invR2=value()*.001f;
            if (i % 11 == 0) std::memcpy(p, l.pos, sizeof p);
            float a[3], b[3];
            const float reach = value();
            GpuLampLaw::EvalBefore(l,p,n,reach,a); GpuLampLaw::Eval(l,p,n,reach,b);
            assert(!std::memcmp(a,b,sizeof a));
        }
    }
    _mm_setcsr(saved);
    std::cout << "131072 lamp evaluations: exact RGB bits across four SSE rounding modes\n";
}
void CandidateChecks() {
    unsigned checks=0;
    std::vector<uint8_t> a(1029,17),b(1029,17);
    for (uint32_t len=4; len<=1028; ++len) {
        b[len]=23;
        for (uint32_t off : {1u,1024u,1025u,16384u,16385u,131071u})
            for (int gain=0; gain<=1028; ++gain) {
                if (!CanImproveMatch(a.data(),b.data(),1028,off,gain)) assert(static_cast<int>(len)-Cost(len,off)<=gain);
                ++checks;
            }
        b[len]=17;
    }
    for(uint32_t size=4;size<=1028;++size) {
        assert(!CanImproveMatch(a.data(),b.data(),size,131071,size));
        assert(CanImproveMatch(a.data(),b.data(),size,1,0));
    }
    std::cout << checks << " candidate bounds: no improving match rejected; scan limits passed\n";
}

namespace TextureHookCheck {
struct IDirect3DDevice9 {};
struct IDirect3DTexture9 {};
using D3DFORMAT = int;
using D3DPOOL = int;
namespace DevSwitches { bool kCompiled=false; }
std::vector<int> events;
bool callbackContinue=true, driverSuccess=true, measuring=true;
namespace FrameProfiler {
uint64_t BeginTextureAllocation() { events.push_back(2);return measuring?123:0; }
void EndTextureAllocation(uint64_t start,bool succeeded) { events.push_back(4);assert(start==(measuring?123u:0u));assert(succeeded==driverSuccess); }
}
namespace ApexMemory { void NoteTexture(void*,IDirect3DTexture9*,UINT,UINT,UINT,DWORD,D3DFORMAT,D3DPOOL) { assert(false); } }
int g_ctex=0;
bool Run(int,IDirect3DDevice9*,HRESULT& hr,UINT,UINT,UINT,DWORD,D3DFORMAT,D3DPOOL,IDirect3DTexture9**,HANDLE*) {
    events.push_back(1);if(!callbackContinue) hr=E_FAIL;return callbackContinue;
}
HRESULT o_ctex(IDirect3DDevice9*,UINT w,UINT h,UINT l,DWORD u,D3DFORMAT f,D3DPOOL p,IDirect3DTexture9** out,HANDLE* shared) {
    events.push_back(3);assert(w==64 && h==128 && l==3 && u==7 && f==5 && p==2 && !shared);
    if(driverSuccess) *out=reinterpret_cast<IDirect3DTexture9*>(uintptr_t{1234});
    return driverSuccess?S_OK:E_OUTOFMEMORY;
}
#include "texture_allocation_hook.h"
void Check() {
    for(bool observe:{false,true}) for(bool success:{false,true}) for(bool allow:{false,true}) {
        events.clear();measuring=observe;driverSuccess=success;callbackContinue=allow;
        IDirect3DTexture9* out=nullptr;
        const HRESULT hr=H_CreateTexture(nullptr,64,128,3,7,5,2,&out,nullptr);
        if(!allow) { assert(hr==E_FAIL && !out);assert(events==std::vector<int>({1})); }
        else {
            assert(hr==(success?S_OK:E_OUTOFMEMORY));assert(bool(out)==success);
            assert(events==std::vector<int>({1,2,3,4}));
        }
    }
    std::cout << "Texture observer: exactly one device call, unchanged arguments/output/error and callback exclusion passed\n";
}
}
int main() { PairChecks(); LampChecks(); CandidateChecks(); TextureHookCheck::Check(); }
