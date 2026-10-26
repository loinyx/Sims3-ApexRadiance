#define NOMINMAX
#include <Windows.h>
#include <cstring>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>
#include <chrono>
#include <iostream>
#include <cassert>
namespace ApexMath { float Fabs(float x){return std::fabs(x);} float Floor(float x){return std::floor(x);} }
namespace GroundShadow { struct Wall{float x0,z0,x1,z1,y0,y1;}; }
struct FenceGap {GroundShadow::Wall w;float length=0;uint64_t key;uint64_t ga=0,gb=0;bool closed=false;};
constexpr float kFenceCell=8.0f,kGapMax=2.5f,kGapMin=.4f,kGapNarrow=.02f;
std::vector<GroundShadow::Wall> g_fenceAll;std::vector<uint64_t> g_fenceAllGroup;
std::unordered_map<uint64_t,std::vector<uint32_t>> g_fenceCells,g_gapCells;
std::vector<FenceGap> g_fenceGaps;
int FenceCell(float x){return static_cast<int>(ApexMath::Floor(x/kFenceCell));}
uint64_t FenceCellKey(int x,int z){return(uint64_t(uint32_t(x))<<32)|uint32_t(z);}
uint64_t hypotCalls=0;
__declspec(noinline) float CountedHypot(float x,float z){++hypotCalls;return std::hypot(x,z);}
struct WallBlock { int x0, y0, x1, y1; };
struct WallJob { uintptr_t wall; WallBlock r; };
struct GhostRows { int x0=0,n=0; int filled[2]={}; std::vector<uint32_t> px; };
std::vector<WallJob> g_wallJobs;
std::atomic<long> g_alignEdges{0},g_ghostWalls{0};
#include "production.h"
#include "fence_reference.h"
bool Equal(const std::vector<FenceGap>& a,const std::vector<FenceGap>& b){
 if(a.size()!=b.size())return false;
 for(size_t i=0;i<a.size();++i){const auto&x=a[i];const auto&y=b[i];
 if(x.key!=y.key||x.ga!=y.ga||x.gb!=y.gb||x.closed!=y.closed||
 x.w.x0!=y.w.x0||x.w.z0!=y.w.z0||x.w.x1!=y.w.x1||x.w.z1!=y.w.z1||x.w.y0!=y.w.y0||x.w.y1!=y.w.y1)return false;}
 return true;
}
void FenceChecks(){
 double oldMs=0,newMs=0;uint64_t oldCalls=0,newCalls=0;uint32_t rng=17;unsigned cases=0;
 for(unsigned scenario=0;scenario<36;++scenario){
 g_fenceAll.clear();g_fenceAllGroup.clear();g_fenceCells.clear();
 unsigned n=scenario<30?128+(scenario%6)*32:1400;
 for(unsigned i=0;i<n;++i){rng=rng*1664525u+1013904223u;
 float x=1400.0f+float(i%40)*1.8f,z=900.0f+float(i/40)*2.0f;
 float length=(i%7==0)?.0f:1.0f+float(i%3)*.4f;
 float dz=scenario%3==1?.02f:scenario%3==2?length:0;
 float dx=scenario%3==2?0:length;
 if(scenario%5==0){x+=float(rng%9)*.001f;z+=float((rng>>4)%9)*.001f;}
 g_fenceAll.push_back({x,z,x+dx,z+dz,20.0f,22.0f});g_fenceAllGroup.push_back(i/4);
 auto&w=g_fenceAll.back();g_fenceCells[FenceCellKey(FenceCell((w.x0+w.x1)*.5f),FenceCell((w.z0+w.z1)*.5f))].push_back(i);
 }
 hypotCalls=0;auto t=std::chrono::steady_clock::now();ReferenceGapsBuild();oldMs+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-t).count();oldCalls+=hypotCalls;
 auto expected=g_fenceGaps;auto expectedCells=g_gapCells;
 hypotCalls=0;t=std::chrono::steady_clock::now();FenceGapsBuild();newMs+=std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-t).count();newCalls+=hypotCalls;
 assert(Equal(expected,g_fenceGaps)&&expectedCells==g_gapCells);
 for(const auto& gap:g_fenceGaps) assert(gap.length==std::hypot(gap.w.x1-gap.w.x0,gap.w.z1-gap.w.z0));
 ++cases;
 }
 std::cout<<cases<<" geometry cases exactly equal; baseline "<<oldMs<<" ms / "<<oldCalls<<" hypot calls; cache "<<newMs<<" ms / "<<newCalls<<" calls; speedup "<<oldMs/newMs<<"x (CPU fixture only)\n";
}

void MapChecks() {
    alignas(4) unsigned char manager[0x300] = {}, wrappers[16][0x40] = {};
    const auto mgr = reinterpret_cast<uintptr_t>(manager);
    auto set = [](uintptr_t p, uintptr_t v) { *reinterpret_cast<uintptr_t*>(p) = v; };
    for (int c = 0; c < 4; ++c) for (int i = 0; i < 4; ++i) {
        const int k = c * 4 + i;
        const auto address = mgr + 0x16C + c * 64 + i * 4;
        const auto wrapper = reinterpret_cast<uintptr_t>(wrappers[k]);
        set(address, wrapper); set(wrapper + 0x30, 0x1000u + k);
        StoryMapSlot slot;
        assert(StoryMapsInclude(mgr, 0x1000u + k, slot));
        assert(slot.address == address && slot.wrapper == wrapper);
        assert(StoryMapSlotMatches(slot, 0x1000u + k));
        set(wrapper + 0x2C, 1); assert(!StoryMapSlotMatches(slot, 0x1000u + k));
        set(wrapper + 0x2C, 0); set(wrapper + 0x30, 0x2000u + k);
        assert(!StoryMapSlotMatches(slot, 0x1000u + k));
        set(wrapper + 0x30, 0x1000u + k);
        set(address, 0); assert(!StoryMapSlotMatches(slot, 0x1000u + k));
        // The same texture moves to another native slot; the old shortcut must fail.
        const auto moved = mgr + 0x16C + ((c + 1) % 4) * 64 + i * 4;
        set(moved, wrapper); assert(!StoryMapSlotMatches(slot, 0x1000u + k));
        assert(StoryMapsInclude(mgr, 0x1000u + k, slot) && slot.address == moved);
        set(moved, 0);
        alignas(4) unsigned char replacement[0x40] = {};
        const auto repl = reinterpret_cast<uintptr_t>(replacement);
        set(repl + 0x30, 0x1000u + k); set(address, repl);
        assert(!StoryMapSlotMatches(slot, 0x1000u + k));
        assert(StoryMapsInclude(mgr, 0x1000u + k, slot) && slot.wrapper == repl);
        set(address, 0);
    }
    void* inaccessible = VirtualAlloc(nullptr, 4096, MEM_COMMIT | MEM_RESERVE, PAGE_NOACCESS);
    assert(inaccessible);
    StoryMapSlot invalid{reinterpret_cast<uintptr_t>(inaccessible), 1};
    assert(!StoryMapSlotMatches(invalid, 123));
    assert(!StoryMapsInclude(reinterpret_cast<uintptr_t>(inaccessible), 123, invalid));
    assert(!invalid.address && !invalid.wrapper);
    assert(VirtualFree(inaccessible, 0, MEM_RELEASE));
    std::cout << "16 native slots: replacement, relocation, type, texture and unreadable memory passed\n";
}
void BlurChecks() {
    unsigned cases = 0;
    for (int n : {1, 2, 7, 64}) for (int rows : {2, 3, 17})
        for (int sides = 0; sides < 4; ++sides) for (uint32_t passes : {1u, 2u, 4u}) {
            std::vector<uint32_t> atlas(static_cast<size_t>(n + 4) * (rows + 4), 0x89ABCDEF);
            const uint32_t pitch = (n + 4) * 4;
            g_wallJobs = {{1, {2, 2, n + 1, rows + 1}}};
            std::vector<std::vector<uint32_t>> before(1, std::vector<uint32_t>(static_cast<size_t>(n) * rows));
            uint32_t seed = 17;
            for (auto& v : before[0]) { seed = seed * 1664525u + 1013904223u; v = seed; }
            std::vector<GhostRows> ghosts(1);
            auto& g = ghosts[0]; g.n = n; g.x0 = 2; g.px.assign(static_cast<size_t>(n) * 4, 0xFF563412);
            g.filled[0] = sides & 1 ? n : 0; g.filled[1] = sides & 2 ? n : 0;
            std::vector<uint32_t> ext(static_cast<size_t>(rows + 4) * n + 64, 0xDEADBEEF);
            auto expected = atlas;
            ReferenceBlur(reinterpret_cast<uintptr_t>(expected.data()), pitch, before, ghosts, ext, passes);
            ext.assign(static_cast<size_t>(rows + 4) * n + 128, 0xBAD0C0DE);
            PreparedBlur(reinterpret_cast<uintptr_t>(atlas.data()), pitch, before, ghosts, ext, passes);
            assert(atlas == expected); ++cases;
            before[0].clear();
            PreparedBlur(reinterpret_cast<uintptr_t>(atlas.data()), pitch, before, ghosts, ext, passes);
            assert(atlas == expected);
        }
    std::cout << cases << " wall blur cases pixel-exact, including reused dirty buffers and failed row reads\n";
}
int main() { FenceChecks(); MapChecks(); BlurChecks(); }
