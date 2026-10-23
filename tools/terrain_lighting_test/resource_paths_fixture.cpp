// Read-only fixture. Bodies in extracted_resource_paths.h are literal production
// blocks; macros below replace only the D3D interface types with fixtures.
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <array>
#include <algorithm>
#include <atomic>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <format>
#include <map>
#include <string>
#include <utility>
#include <vector>
#include "features/terrain_lighting_policy.h"
#include "features/lamp_reach.h"
#include "features/ground_light_gate.h"
#include "features/ground_lamp_shadow.h"
#include "features/fast_math.h"
#include "features/penumbra.h"

struct FakeTexture {
    int id = 0, refs = 1;
    ULONG AddRef() { return ULONG(++refs); }
    ULONG Release() { return ULONG(--refs); }
};
struct FakeShader { int id; };
struct FakeDevice {
    std::array<std::array<float,4>,224> pixel{};
    std::array<std::array<float,4>,256> vertex{};
    std::array<std::array<DWORD,11>,16> sampler{};
    std::array<FakeTexture*,16> textures{};
    std::array<DWORD,256> render{};
    HRESULT GetRenderState(D3DRENDERSTATETYPE t,DWORD* v){*v=render[t];return S_OK;}
    HRESULT SetRenderState(D3DRENDERSTATETYPE t,DWORD v){render[t]=v;++writes;return S_OK;}
    FakeShader* ps = nullptr;
    FakeShader* vs = nullptr;
    int writes = 0, pixelWrites = 0, vertexWrites = 0, textureWrites = 0, samplerWrites = 0, shaderWrites = 0;
    int samplerReads = 0, textureReads = 0;
    int failPixelRead = -1, failVertexRead = -1, failSamplerReadAt = -1, failSamplerWriteAt = -1;
    int failTextureReadAt = -1, failTextureWriteAt = -1;
    HRESULT GetPixelShaderConstantF(UINT r, float* v, UINT n) { if(int(r)==failPixelRead) return E_FAIL; std::memcpy(v,pixel[r].data(),n*16); return S_OK; }
    HRESULT GetVertexShaderConstantF(UINT r, float* v, UINT n) { if(int(r)==failVertexRead) return E_FAIL; std::memcpy(v,vertex[r].data(),n*16); return S_OK; }
    HRESULT GetTexture(DWORD s, FakeTexture** v) {
        if(textureReads++==failTextureReadAt) return E_FAIL;
        *v=textures[s]; if(*v) (*v)->AddRef(); return S_OK;
    }
    HRESULT GetSamplerState(DWORD s,D3DSAMPLERSTATETYPE t,DWORD* v) {
        if(samplerReads++==failSamplerReadAt) return E_FAIL;
        *v=sampler[s][t-1]; return S_OK;
    }
    HRESULT SetSamplerState(DWORD s,D3DSAMPLERSTATETYPE t,DWORD v) {
        ++writes; if(samplerWrites++==failSamplerWriteAt) return E_FAIL;
        sampler[s][t-1]=v; return S_OK;
    }
    HRESULT SetTexture(DWORD s,FakeTexture* v) {
        ++writes; if(textureWrites++==failTextureWriteAt) return E_FAIL;
        textures[s]=v; return S_OK;
    }
};
#define IDirect3DDevice9 FakeDevice
#define IDirect3DPixelShader9 FakeShader
#define IDirect3DVertexShader9 FakeShader
#define IDirect3DTexture9 FakeTexture
#define IDirect3DBaseTexture9 FakeTexture
#include "features/native_terrain_sampler.h"

namespace D3D9Hooks { enum class HookAction { Continue,Skip }; HRESULT GetBoundTexture(FakeDevice* d,DWORD s,FakeTexture** t){return d->GetTexture(s,t);} }
namespace ApexUtil {template<class T> void CountOne(T& x){++x;}}
uint32_t g_patchedGen=1;
enum class ClaimSource { LotLightAtlas };
D3D9Hooks::HookAction NoteClaim(ClaimSource){return D3D9Hooks::HookAction::Skip;}
std::atomic<int> g_wallBloomPreserved{0},g_wallBloomFallback{0};
std::atomic<float> g_wallGain{1},g_groundGain{1},g_roadGain{1},g_lotMapGain{1},g_night{1};
std::atomic<int> g_wallDrawn{0},g_groundGainDraws{0},g_lotDrawn{0},g_lotMissing{0},g_edgeMatched{0},g_edgeUnmatched{0};
std::atomic<bool> g_softEdges{false},g_wallEnabled{true};
bool g_inOwnCall=false,g_lotRectMiss=false,g_haveLastEdgeRect=false;
std::string g_objDrawInfo;
FakeShader nativePs{1},replacementPs{2};
FakeShader* g_curPs=&nativePs;
FakeShader* g_replacementPs=&replacementPs;
std::map<FakeShader*,DWORD> g_wallConst;
struct ChunkTex { FakeTexture* tex=nullptr; };
std::map<std::pair<int,int>,ChunkTex> g_chunks;
struct LotRect { uint32_t lotHi=0,lotLo=1; float w=32,d=64,tx=0,ty=0,tz=0; };
LotRect fixtureRect{},g_lastEdgeRect{};
bool fixtureHasRect=false,fixtureCapture=false,fixtureFeather=false;
FakeTexture* fixtureAtlas=nullptr;
std::array<float,4> fixtureAtlasC{.001f,.002f,-.25f,-.5f},lastEdgeUv{};
constexpr float kEdgeBand=2.5f;
namespace LightProbe { bool Capturing() { return fixtureCapture; } }
namespace LightmapSmooth {
FakeTexture* Atlas(float* c) {
    if(!fixtureAtlas) return nullptr; // reproduce production's untouched output on null
    std::memcpy(c,fixtureAtlasC.data(),16); return fixtureAtlas;
}
}
bool Near(float a,float b) { return std::fabs(a-b)<1e-5f; }
std::pair<int,int> Key(float x,float z) { return {int(std::round(x)),int(std::round(z))}; }
FakeTexture* ChunkTexture(const std::pair<int,int>&,FakeTexture* tex) { return tex; }
void EnsureReplacement(FakeDevice*) {}
const LotRect* FindLotRect(const float*,const float*) { return fixtureHasRect?&fixtureRect:nullptr; }
void NoteLotDraw(uint64_t) {}
bool LotEdgeConstants(const float* uv,const float*,const float*,const float*,const LotRect*,float* out) {
    std::memcpy(lastEdgeUv.data(),uv,16); for(int i=0;i<12;++i) out[i]=.02f*float(i+1); return fixtureFeather;
}
void SetPs(FakeDevice* d,FakeShader* ps) { ++d->writes; ++d->shaderWrites; d->ps=ps; }
void SetTex(FakeDevice* d,DWORD s,FakeTexture* tex) { d->SetTexture(s,tex); }
void SetPsConst(FakeDevice* d,UINT r,const float* v,UINT n) { ++d->writes; ++d->pixelWrites; std::memcpy(d->pixel[r].data(),v,n*16); }
void SetVsConst(FakeDevice* d,UINT r,const float* v,UINT n) { ++d->writes; ++d->vertexWrites; std::memcpy(d->vertex[r].data(),v,n*16); }
namespace RigTracker { int fixtureMode=2;int CurrentMode(){return fixtureMode;} }
int RigMode(){return RigTracker::CurrentMode();} // production: RigTracker::CurrentMode read once per draw
namespace ShaderPatches {
constexpr unsigned kObjectPixelLamps=8;
struct ObjectPatch {UINT atlasConst=40,strengthConst=41,lampParamConst=42,atlasSampler=9;bool rigLamps=true,pixelLamps=true;UINT groundWeightConst=39;UINT reachConst=0,capConst=0;int shadeSampler=-1,shadeConst=-1;};
bool PatchObjectLampPs(std::vector<DWORD>&,ObjectPatch&,bool=false){return true;}
bool PatchInstancedLamps(std::vector<DWORD>&,ObjectPatch&,bool=false,bool=true,bool=false){return true;}
}
struct PatchedPs { FakeShader* ps=&replacementPs;ShaderPatches::ObjectPatch obj,inst; } objectPatch,reachPatch,refusedPatch{nullptr};
int g_objLampPs=0,g_objLampPsReach=0;
int g_fencePs=0,g_fencePsReach=0,g_fenceCapPs[2]{};
FakeShader capShader{5};PatchedPs capPatch;
bool fixtureCaps=false,fixtureCapRefused=false;int fixtureCapCalls=0;
// realistic reach (12/10): the fixture's PatchedFor returns objectPatch for the first law and reachPatch (reachConst set,
// ResetGlobals) for the variant copy, or refusedPatch (no shader) while fixtureReachRefused
uint32_t g_reachObjDraws=0,g_reachFenceDraws=0,g_reachWindowDraws=0,g_reachLawRefused=0;
DWORD g_lotDrawTick=0;
std::atomic<float> g_nativeWallScale{1.f};
std::atomic<DWORD> g_nativeWallTick{0};
constexpr float kPixelLampTrimUnit=.75f;
bool fixtureReach=false,fixtureReachRefused=false,fixtureNoGroup=false;
std::string ReachLawText(bool,bool,bool,float,const float(*)[4],int){return {};}
std::atomic<bool> g_fenceFix{true};
std::atomic<float> g_fenceStrength{1};
std::atomic<int> g_fenceDrawn{0};
std::atomic<bool> g_objPixel{true},g_objPixelLamps{true};
std::atomic<float> g_objPixelStrength{1},g_objPixelLampStrength{1};
std::atomic<int> g_objLampDrawn{0};
struct ObjectVsInfo {bool contractedLotUv=false;struct {int vertexLight=20,worldK=8;} patched;} objectVsInfo;
ObjectVsInfo* g_curVsInfo=&objectVsInfo;
FakeShader nativeVs{3},replacementVs{4};FakeShader* g_curVs=&nativeVs;
float g_lampData[33][4]{};
template<class Patch> PatchedPs& PatchedFor(FakeDevice*,int& cache,const char*,Patch,bool variant=false){
    if(&cache==&g_fenceCapPs[0]||&cache==&g_fenceCapPs[1]) return fixtureCapRefused?refusedPatch:capPatch;
    return !variant?objectPatch:fixtureReachRefused?refusedPatch:reachPatch;
}
FakeShader* ObjectVsFor(FakeDevice*,FakeShader*){return &replacementVs;}
int SelectLamps(float,float,float,bool=false){return 1;}
struct LampCone {float c1[4]={0,0,0,1},c2[4]={0,0,0,1};float spill=1;float w=0;float entries=1;uintptr_t light=0;};
LampCone g_lampCone[16];
bool g_fenceConeLogged=false,g_lampConeLogged=false;

#define LOG_INFO(...) ((void)0)
#define LOG_WARNING(...) ((void)0)
constexpr bool kPublicBuild=false;
uint32_t FloatBits(float x){return std::bit_cast<uint32_t>(x);}
uint32_t g_lampMemoGen=1;int g_lastLampCandidates=1,g_lastLampPicked=1;
namespace DevSwitches { bool LampVisibilityMemo(){return false;} }
void NoteFenceGroup(FakeDevice*){}
bool NoteGate(float,float,float){return false;}
float fixtureVisibility=1;
int visCalls=0;float firstVis[3]{}; // the first point a draw tests (fences: the piece nearest the lamp, 12/10)
namespace LevelLightShare {bool WallBlocks(const float*,const float*,float=0){return false;}
float LampVisibility(const float*,const float* p,float=.2f){if(visCalls++==0)std::memcpy(firstVis,p,12);return fixtureVisibility;}
float ExitLampVisibility(const void*,const float*,const float*,float=.2f){return -1;}
bool RealisticReachActive(){return fixtureReach;}
bool IndoorLightOnGroundActive(){return false;}
bool VisibilityDiagArmed(){return false;}uint64_t LampVisibilityState(){return 1;}
bool LampFloorCap(const float*,float rows[2][4]){++fixtureCapCalls;std::memset(rows,0,32);if(!fixtureCaps)return false;rows[0][0]=1;rows[0][3]=.5f;rows[1][1]=1;return true;}
}
uint32_t g_objLampsLeftOut=0,g_objLampsDimmed=0,g_fenceLampsLeftOut=0,g_fenceLampsDimmed=0,g_objGroundGated=0,g_objRigDayScaled=0;
constexpr float kWindowWallSkip=.75f;
bool ObjectLampPoint(const float (&m)[3][4],float (&at)[3]){for(int k=0;k<3;++k)at[k]=m[k][3];return false;}
constexpr unsigned kGroupPieces=32;
struct GroupCentre {float x=0,y=0,z=0,ext=0,minY=0,a[3]{},b[3]{};float piece[kGroupPieces][3]{};unsigned pieces=0;uint32_t read=0;};
GroupCentre fixtureGroup;
const GroupCentre* InstancedGroupCentre(FakeDevice*){return fixtureNoGroup?nullptr:&fixtureGroup;}
std::map<FakeShader*,int> g_wetLotPs;
FakeShader* g_replacementWetPs=&replacementPs;
void EnsureWetReplacement(FakeDevice*){}
FakeTexture* LotMaskFor(FakeDevice*,const float*,const float*,const float*,const float*,const LotRect*,float* rows){std::memset(rows,0,32);return nullptr;}
// the ground lamp shade of fences (13/10): the production FloorShadeBind (not extracted) binds the lot mask and 3 rows for the
// draw; the fixture writes marker rows and restores them, and records the point the lot was looked up at
std::atomic<long> g_fenceShadeLot{0},g_fenceShadeNoLot{0};
const LotRect* fixtureShadeLot=nullptr;float fixtureShadeAt[2]{};int fixtureShadeLookups=0;
const LotRect* FindLotAt(float x,float z){++fixtureShadeLookups;fixtureShadeAt[0]=x;fixtureShadeAt[1]=z;return fixtureShadeLot;}
struct FloorShadeBind {
    FakeDevice* d;UINT c=0;float old[3][4]{};bool active=false;
    FloorShadeBind(FakeDevice* dev,int sampler,int constant,const LotRect* lot,float,std::atomic<long>& withLot,std::atomic<long>& noLot,float=0):d(dev){
        if(sampler<0||constant<0) return;
        c=UINT(constant);(lot?withLot:noLot)++;d->GetPixelShaderConstantF(c,&old[0][0],3);
        const float rows[12]{7,7,7,7,7,7,7,7,7,7,7,7};SetPsConst(d,c,rows,3);active=true;
    }
    ~FloorShadeBind(){if(active)SetPsConst(d,c,&old[0][0],3);}
    std::string Text() const {return active?"ground lamp shade: fixture":"ground lamp shade: none";}
};
template<class LampArray> std::string DescribeObjectDraw(FakeDevice*,const ShaderPatches::ObjectPatch&,int,bool,int,int,const LampArray&,int){return "fixture";}
void SetVs(FakeDevice* d,FakeShader* shader){++d->writes;d->vs=shader;}
#include "extracted_resource_paths.h"

int checks=0,failures=0;
void Check(bool value,const char* text) { ++checks; if(!value) { ++failures; if(failures<30) std::printf("FAIL: %s\n",text); } }
template<class T> bool Same(const T& a,const T& b) { return !std::memcmp(&a,&b,sizeof(T)); }
void ResetGlobals() {
    ++g_patchedGen;fixtureCaps=false;fixtureCapRefused=false;fixtureCapCalls=0;capPatch=PatchedPs{};capPatch.ps=&capShader;capPatch.inst.capConst=120;
    objectVsInfo.contractedLotUv=false;
    g_wallGain=1;g_groundGain=1;g_roadGain=1;g_lotMapGain=1;g_night=1;
    g_wallDrawn=0;g_groundGainDraws=0;g_lotDrawn=0;g_lotMissing=0;g_edgeMatched=0;g_edgeUnmatched=0;
    g_softEdges=false;g_wallEnabled=true;g_inOwnCall=false;g_lotRectMiss=false;g_haveLastEdgeRect=false;g_objDrawInfo.clear();
    g_curPs=&nativePs;g_replacementPs=&replacementPs;g_wallConst.clear();g_wallConst[g_curPs]=7;g_chunks.clear();
    fixtureHasRect=false;fixtureCapture=false;fixtureFeather=false;fixtureAtlas=nullptr;lastEdgeUv={};
    fixtureReach=false;fixtureReachRefused=false;fixtureNoGroup=false;objectPatch.obj.reachConst=objectPatch.inst.reachConst=0;
    reachPatch=PatchedPs{};reachPatch.obj.reachConst=reachPatch.inst.reachConst=42+1+4*ShaderPatches::kObjectPixelLamps;
    g_nativeWallScale=1.f;g_nativeWallTick=0;g_lotDrawTick=0;objectVsInfo.patched.worldK=8;
    g_reachObjDraws=g_reachFenceDraws=g_reachWindowDraws=g_reachLawRefused=0;visCalls=0;
    objectPatch.inst.shadeSampler=objectPatch.inst.shadeConst=-1;fixtureShadeLot=nullptr;fixtureShadeLookups=0;g_fenceShadeLot=0;g_fenceShadeNoLot=0;
}
FakeDevice Device() {
    FakeDevice d; d.ps=g_curPs;
    for(size_t r=0;r<d.pixel.size();++r) for(int c=0;c<4;++c) d.pixel[r][c]=float(r*4+c)/100.f;
    for(size_t r=0;r<d.vertex.size();++r) for(int c=0;c<4;++c) d.vertex[r][c]=float(r*4+c)/100.f;
    d.vertex[14]={1.f/256,1.f/256,.5f,.5f};d.vertex[15]={128,0,128,0};
    for(size_t s=0;s<d.sampler.size();++s) for(int c=0;c<11;++c) d.sampler[s][c]=DWORD(s*20+c+1);
    return d;
}
void WallChecks() {
    const int before=checks;
    for(float native: {0.f,-0.f}) for(float gain: {0.f,.25f,1.f,1.5f,4.f}) {
        ResetGlobals();auto d=Device();d.pixel[7]={native,.23f,-.5f,1.25f};const auto original=d.pixel;g_wallGain=gain;int draws=0;
        const bool claimed=DrawWallGain(&d,[&]{++draws;});
        Check(!claimed&&draws==0,"wall zero term leaves native caller to draw");Check(d.writes==0,"wall +/-zero creates no writes");
        Check(Same(d.pixel,original),"wall signed zero and all constants unchanged");Check(!g_inOwnCall&&g_wallDrawn==0,"wall zero does not claim a correction");
    }
    for(int v=1;v<=50;++v) for(int g=0;g<=40;++g) {
        ResetGlobals();auto d=Device();d.pixel[7]={v/20.f,.23f,-.5f,1.25f};const auto original=d.pixel;const float gain=g/10.f;g_wallGain=gain;int draws=0;
        auto expected=original[7];expected[0]*=gain;const bool change=expected[0]!=original[7][0];
        const bool claimed=DrawWallGain(&d,[&]{++draws;Check(g_inOwnCall,"wall draw marked as own call");Check(Same(d.pixel[7],expected),"wall positive term equals previous formula in all four floats");});
        Check(claimed==change&&draws==int(change),"wall exact no-op contract");Check(d.pixelWrites==(change?2:0),"wall changed term writes then restores once");
        Check(Same(d.pixel,original),"wall restores every pixel constant exactly");Check(!g_inOwnCall&&g_wallDrawn==int(change),"wall own-call guard reset and counter correct");
    }
    ResetGlobals();auto d=Device();g_wallGain=1;int draws=0;Check(!DrawWallGain(&d,[&]{++draws;})&&draws==0&&d.writes==0,"gain one skips writes");
    ResetGlobals();d=Device();g_wallGain=2;d.failPixelRead=7;const auto original=d.pixel;
    Check(!DrawWallGain(&d,[&]{++draws;})&&d.writes==0&&draws==0,"wall failed constant read leaves native draw unchanged");
    Check(Same(d.pixel,original)&&!g_inOwnCall&&g_wallDrawn==0,"wall failed read preserves all state and counter");
    ResetGlobals();d=Device();g_wallGain=2;g_wallConst.clear();Check(!DrawWallGain(&d,[]{})&&d.writes==0,"unrecognised wall keeps native draw");
    for(float night: {0.f,.25f,.5f,.75f,1.f}) for(float native: {0.f,.1f,.5f,1.f}) for(float gain: {.25f,1.f,2.021245f,4.f,8.f}) {
        ResetGlobals();d=Device();g_night=night;g_wallGain=gain;g_wallConst[g_curPs]=3;
        d.pixel[3]={native,.188235313f,0.f,0.f};const auto saved=d.pixel;int calls=0;
        const float expected=night==1.f ? native*gain : gain*TerrainLightingPolicy::LampDay(night);
        const bool claimed=DrawWallGain(&d,[&]{++calls;Check(d.pixel[3][0]==expected,"wall day/twilight scale includes lamp map");
            Check(d.pixel[3][1]==saved[3][1]&&d.pixel[3][2]==saved[3][2]&&d.pixel[3][3]==saved[3][3],"wall changes only lamp x");});
        const bool change=expected!=native;
        Check(claimed==change&&calls==int(change),"wall day/twilight draw contract");
        Check(Same(d.pixel,saved)&&d.pixelWrites==(change?2:0)&&!g_inOwnCall,"wall day/twilight restores constants and guard");
        g_wallEnabled=false;d.writes=0;calls=0;
        Check(!DrawWallGain(&d,[&]{++calls;})&&calls==0&&d.writes==0&&Same(d.pixel,saved),"disabled wall feature preserves native day/twilight/night");
    }
    for(float invalid: {NAN,INFINITY,-1.f}) {
        ResetGlobals();d=Device();g_wallGain=2;g_night=invalid;
        Check(!DrawWallGain(&d,[]{})&&d.writes==0,"invalid wall time factor fails closed");
        ResetGlobals();d=Device();g_wallGain=2;g_night=0;d.pixel[7][0]=invalid;
        Check(!DrawWallGain(&d,[]{})&&d.writes==0,"invalid native wall scale fails closed including NaN");
    }
    std::printf("Extracted DrawWallGain: %d checks\n",checks-before);
}
void LotChecks() {
    const int before=checks;FakeTexture chunk{3},atlas{4},oldTexture{5};
    for(bool recognised: {false,true}) for(int scenario=0;scenario<5;++scenario) {
        ResetGlobals();auto d=Device();objectVsInfo.contractedLotUv=recognised;
        g_chunks[{128,128}]={&chunk};d.vertex[12]={1.f/64,1.f/64,1,1};
        if(scenario==1)d.failVertexRead=12;
        if(scenario==2)d.vertex[12][1]=1.f/32;
        if(scenario==3)d.vertex[12][0]=NAN;
        if(scenario==4)d.vertex[12][0]=0;
        const auto savedPixel=d.pixel;const auto savedVertex=d.vertex;
        const bool validScale=recognised&&scenario!=1&&scenario!=3&&scenario!=4;
        const float yScale=scenario==2?1.f/32:1.f/64;
        Check(ExtractedLotBranch(&d,[&]{
            Check(d.pixel[31][2]==(validScale?1.f/63:0),"lot inverse scale guarded by verified VS and valid constants");
            Check(d.pixel[30][3]==(validScale?-yScale*(16.f/63):.24f),"lot y contraction goes to the matching edge offset");
            Check(d.pixel[31][3]==0,"lot inverse scale leaves unused gain channels unchanged");
        })==D3D9Hooks::HookAction::Skip,"UV fallback retains normal draw");
        Check(Same(d.pixel,savedPixel)&&Same(d.vertex,savedVertex),"lot UV correction restores constants including invalid values");
    }
    for(bool haveAtlas: {false,true}) for(bool feather: {false,true}) for(float night: {0.f,.25f,1.f}) for(float gain: {1.f,2.f}) {
        ResetGlobals();auto d=Device();g_night=night;g_groundGain=gain;g_lotMapGain=1.3f;fixtureAtlas=haveAtlas?&atlas:nullptr;
        fixtureHasRect=feather;fixtureFeather=feather;fixtureCapture=true;g_softEdges=true;
        g_chunks[{128,128}]={&chunk};d.textures[2]=&oldTexture;
        const auto originalPixel=d.pixel;const auto originalVertex=d.vertex;const auto originalSampler=d.sampler;const auto originalTextures=d.textures;int draws=0;
        const auto result=ExtractedLotBranch(&d,[&]{
            ++draws;Check(g_inOwnCall&&d.ps==&replacementPs,"lot uses replacement once with own-call guard");
            Check(d.textures[2]==(haveAtlas?&atlas:&chunk),"lot selects atlas or native chunk fallback");
            for(int c=0;c<4;++c) {
                const float expected=haveAtlas?(c<2?fixtureAtlasC[c]:fixtureAtlasC[c]+originalVertex[15][c==2?0:2]*fixtureAtlasC[c-2]):originalVertex[14][c];
                Check(d.vertex[14][c]==expected,"lot uses exact mapping or preserves chunk mapping");
            }
            // one daytime curve (12/10): before full night c3.x = ground gain x LampDay, c31.x = the lot map's night gain, c31.y = 0;
            // at night the previous constants
            Check(d.pixel[31][0]==(night<1?1.3f:LotMapGain())&&d.pixel[31][1]==0.f,"lot gain constants: night ones at night, the lot map's night gain before");
            auto expected=originalPixel[3];
            if(night<1)expected[0]=gain*TerrainLightingPolicy::LampDay(night);else expected[0]*=GroundGain();
            Check(Same(d.pixel[3],expected),"lot c3: previous night-weighted formula at night, ground gain x the daytime curve before");
        });
        Check(result==D3D9Hooks::HookAction::Skip&&draws==1,"lot claims exactly one draw");
        Check(Same(d.pixel,originalPixel)&&Same(d.vertex,originalVertex),"lot restores all PS/VS constants exactly");
        Check(Same(d.sampler,originalSampler)&&Same(d.textures,originalTextures)&&d.ps==g_curPs,"lot restores shaders, sampler states and texture binding");
        Check(oldTexture.refs==1&&chunk.refs==1&&atlas.refs==1,"lot capture reference released exactly");
        Check(!g_inOwnCall&&g_objDrawInfo.empty()&&g_lotDrawn==1,"lot own-call state and diagnostic reset");
        auto uv=haveAtlas?fixtureAtlasC:originalVertex[14];if(haveAtlas){uv[2]+=128*fixtureAtlasC[0];uv[3]+=128*fixtureAtlasC[1];}
        Check(Same(lastEdgeUv,uv),"lot edge fixture receives exact native fallback or atlas mapping");
    }
    ResetGlobals();auto d=Device();int draws=0;
    Check(ExtractedLotBranch(&d,[&]{++draws;})==D3D9Hooks::HookAction::Continue&&draws==0&&d.writes==0,"missing chunk leaves native draw with no writes");
    Check(g_lotMissing==1&&!g_inOwnCall&&g_lotDrawn==0,"missing chunk reports missing without claiming draw");
    ResetGlobals();d=Device();g_chunks[{128,128}]={nullptr};Check(ExtractedLotBranch(&d,[]{})==D3D9Hooks::HookAction::Continue&&d.writes==0,"null chunk texture leaves native draw");
    for(bool haveAtlas: {false,true}) {
        ResetGlobals();d=Device();fixtureAtlas=haveAtlas?&atlas:nullptr;g_chunks[{128,128}]={&chunk};d.failPixelRead=28;fixtureCapture=true;
        const auto pixel=d.pixel;const auto vertex=d.vertex;const auto textures=d.textures;const auto sampler=d.sampler;draws=0;
        Check(ExtractedLotBranch(&d,[&]{++draws;})==D3D9Hooks::HookAction::Continue&&draws==0&&d.writes==0,"saved edge read failure leaves native draw with zero writes");
        Check(Same(d.pixel,pixel)&&Same(d.vertex,vertex)&&Same(d.textures,textures)&&Same(d.sampler,sampler),"saved edge read failure preserves all device states");
        Check(!g_inOwnCall&&g_objDrawInfo.empty()&&g_lotDrawn==0,"saved edge failure has no stale own-call diagnostics");
    }
    ResetGlobals();d=Device();d.failVertexRead=14;Check(ExtractedLotBranch(&d,[]{})==D3D9Hooks::HookAction::Continue&&d.writes==0,"failed terrain UV read leaves native draw");
    ResetGlobals();d=Device();d.vertex[14][0]=.25f;Check(ExtractedLotBranch(&d,[]{})==D3D9Hooks::HookAction::Continue&&d.writes==0,"unsupported terrain UV shape remains native");
    std::printf("Extracted lot branch: %d checks\n",checks-before);
}
void ObjectChecks() {
    const int before=checks;FakeTexture atlas{30},old{31};
    for(float night:{0.f,.5f,1.f})for(float strength:{.5f,1.f,1.7065217f,4.f})for(bool pixel:{false,true})for(int mode:{1,2}){
        ResetGlobals();auto d=Device();fixtureAtlas=&atlas;d.textures[9]=&old;d.vs=&nativeVs;
        g_objPixel=true;g_objPixelLamps=pixel;g_objPixelStrength=strength;g_objPixelLampStrength=strength;
        g_night=night;RigTracker::fixtureMode=mode;g_objLampDrawn=0;
        // lamp head 2 m up, above the object (origin y .39): the ground gate stays 1
        g_lampData[0][1]=2;g_lampData[0][3]=10;g_lampData[16][0]=.2f;g_lampData[16][1]=.1f;g_lampData[16][2]=.05f;
        const auto ps=d.pixel;const auto vs=d.vertex;const auto states=d.sampler;const auto textures=d.textures;int calls=0;
        const float f=TerrainLightingPolicy::LampDay(night);
        const float expected=night==1?strength:strength*f;
        const bool claimed=DrawObjectLamp(&d,[&]{++calls;
            Check(d.pixel[41][0]==expected,"actual object draw receives the daytime curve's ground strength");
            Check(d.pixel[42][1]==(pixel?expected:0),"actual object draw receives the daytime curve's per-pixel strength");
            Check(d.pixel[42][0]==(pixel?1.f:0.f),"direct mode enables upward-only atlas; fallback retains old weighting");
            if(pixel){Check(d.pixel[44][3]==1.f,"non-cone lamp spill uploads unchanged");Check(d.pixel[59][3]==1.f&&d.pixel[60][3]==1.f,"two cone constants uploaded in expanded block");}
            // the rig: zeroed with per-pixel lamps placed; kept without, its lamp colours (rgb) x the curve for the draw (12/10)
            for(int r=0;r<3;++r){
                if(pixel)Check(d.pixel[5+r][0]==0.f&&d.pixel[5+r][3]==0.f,"placed lamps zero the PS rig");
                else Check(d.pixel[5+r][0]==ps[5+r][0]*f&&d.pixel[5+r][2]==ps[5+r][2]*f&&d.pixel[5+r][3]==ps[5+r][3],"kept PS rig colours x the daytime curve, w untouched");
            }
            for(int r=0;r<4;++r){
                if(pixel)Check(d.vertex[20+r][0]==0.f,"placed lamps zero the VS vertex lights");
                else Check(d.vertex[20+r][1]==vs[20+r][1]*f&&d.vertex[20+r][3]==vs[20+r][3],"kept VS vertex light colours x the daytime curve, w untouched");
            }
            if(night==1&&!pixel)Check(Same(d.pixel[5],ps[5])&&Same(d.vertex[20],vs[20]),"night: the kept rig is untouched");

            Check(Same(d.pixel[0],ps[0]),"object brightness does not change sunlight constants");
            Check(d.ps==&replacementPs&&d.vs==&replacementVs&&g_inOwnCall,"object uses one existing draw with guard");
        });
        Check(claimed&&calls==1,"object draws at every level (no full-daylight gate, 12/10)");
        Check(Same(d.pixel,ps)&&Same(d.vertex,vs)&&Same(d.sampler,states)&&Same(d.textures,textures),"object restores every constant sampler and texture");
        Check(d.ps==g_curPs&&d.vs==g_curVs&&!g_inOwnCall&&g_objDrawInfo.empty()&&g_objLampDrawn==1,"object restores shaders diagnostics and guard");
        Check(atlas.refs==1&&old.refs==1,"object retains exact texture ownership");
    }
    ResetGlobals();auto d=Device();fixtureAtlas=&atlas;g_objPixel=false;int calls=0;
    Check(!DrawObjectLamp(&d,[&]{++calls;})&&calls==0&&d.writes==0,"disabled object path retains native drawing");
    g_objPixel=true;RigTracker::fixtureMode=0;
    Check(!DrawObjectLamp(&d,[&]{++calls;})&&calls==0&&d.writes==0,"indoor objects excluded from daytime outdoor balance");
    std::printf("Extracted DrawObjectLamp: %d checks\n",checks-before);
}
void InstancedChecks() {
    const int before=checks;FakeTexture atlas{32},old{33};
    for(float night:{0.f,.5f,1.f})for(float gain:{.25f,.993083f,1.f,2.f}){
        ResetGlobals();auto d=Device();fixtureAtlas=&atlas;d.textures[9]=&old;
        g_fenceFix=true;g_fenceStrength=gain;g_night=night;g_fenceDrawn=0;
        const auto pixel=d.pixel;const auto vertex=d.vertex;const auto sampler=d.sampler;const auto textures=d.textures;int calls=0;
        const float expected=night==1?gain:gain*TerrainLightingPolicy::LampDay(night);
        const bool claimed=DrawInstanced(&d,[&]{++calls;
            Check(d.pixel[41][0]==expected,"captured bench path gets the shared daytime curve");
            Check(Same(d.pixel[0],pixel[0])&&Same(d.pixel[1],pixel[1]),"bench path retains sunlight colour and direction");
        });
        Check(claimed&&calls==1,"instanced surface draws at every level (no full-daylight gate, 12/10)");
        Check(Same(d.pixel,pixel)&&Same(d.vertex,vertex)&&Same(d.sampler,sampler)&&Same(d.textures,textures),"instanced surface restores all device states");
        Check(!g_inOwnCall&&d.ps==g_curPs&&g_fenceDrawn==1&&atlas.refs==1&&old.refs==1,"instanced surface restores shader guard counter and ownership");
    }
    ResetGlobals();auto d=Device();g_fenceFix=false;fixtureAtlas=&atlas;
    Check(!DrawInstanced(&d,[]{})&&d.writes==0,"disabled instanced path stays native");
    g_fenceFix=true;fixtureAtlas=nullptr;
    Check(!DrawInstanced(&d,[]{})&&d.writes==0,"missing instanced atlas stays native");
    // the ground lamp shade (13/10): a copy with it binds its rows for the draw with the lot under the group's centre, and
    // restores them; no group read: no lookup, counted without a lot; a copy without it binds nothing
    for(int mode=0;mode<3;++mode){
        ResetGlobals();d=Device();fixtureAtlas=&atlas;g_fenceFix=true;g_night=1;g_objPixelLamps=true;fixtureGroup=GroupCentre{};fixtureGroup.x=12;fixtureGroup.z=-3;
        if(mode<2){objectPatch.inst.shadeSampler=10;objectPatch.inst.shadeConst=200;}
        fixtureHasRect=true;fixtureShadeLot=&fixtureRect;fixtureNoGroup=mode==1;
        const auto pixel=d.pixel;const auto textures=d.textures;
        Check(DrawInstanced(&d,[&]{Check(mode<2?d.pixel[200][0]==7.f&&d.pixel[202][3]==7.f:Same(d.pixel[200],pixel[200]),"fence shade rows bound only by a copy with the shade");}),"fence with the shade draws");
        Check(Same(d.pixel[200],pixel[200])&&Same(d.pixel[202],pixel[202])&&Same(d.textures,textures),"fence shade rows restored after the draw");
        if(mode==0) Check(fixtureShadeLookups==1&&fixtureShadeAt[0]==12.f&&fixtureShadeAt[1]==-3.f&&g_fenceShadeLot==1&&g_fenceShadeNoLot==0,"fence shade: the lot under the group's centre");
        if(mode==1) Check(fixtureShadeLookups==0&&g_fenceShadeLot==0&&g_fenceShadeNoLot==1,"fence shade: no group read, no lot (white, gate 0)");
        if(mode==2) Check(g_fenceShadeLot==0&&g_fenceShadeNoLot==0,"fence copy without the shade binds nothing");
    }
    ResetGlobals();d=Device();fixtureAtlas=&atlas;fixtureCapture=true;g_fenceFix=true;g_night=1;g_objPixelLamps=true;fixtureGroup=GroupCentre{};objectPatch.inst.shadeSampler=10;objectPatch.inst.shadeConst=200;
    Check(DrawInstanced(&d,[&]{Check(g_objDrawInfo.find("ground lamp shade: fixture")!=std::string::npos,"F7 fence line shows the ground lamp shade");}),"captured shaded fence draws");
    ResetGlobals();d=Device();fixtureAtlas=&atlas;g_fenceFix=true;g_objPixelLamps=true;fixtureGroup=GroupCentre{};fixtureCaps=true;
    const auto capsBefore=d.pixel;int capDraws=0;
    Check(DrawInstanced(&d,[&]{++capDraws;
        Check(d.ps==&capShader,"verified emitter floor selects the cap shader");
        Check(d.pixel[120][0]==1.f&&d.pixel[120][3]==.5f,"per-lamp floor cap rows reach the selected shader");
        Check(d.pixel[136][0]==.5f&&d.pixel[136][2]==.5f&&d.pixel[136][3]==1e-4f,"shared cap constants reach the selected shader");
    })&&capDraws==1&&fixtureCapCalls==1,"verified emitter floor draws with the per-lamp cap shader");
    Check(d.ps==g_curPs&&Same(d.pixel,capsBefore),"cap shader and constants restore after draw");
    ResetGlobals();d=Device();fixtureAtlas=&atlas;g_fenceFix=true;g_objPixelLamps=true;fixtureGroup=GroupCentre{};fixtureCaps=true;fixtureCapRefused=true;
    const auto refusedCaps=d.pixel;
    Check(DrawInstanced(&d,[&]{Check(d.ps==&replacementPs,"refused cap shader retains the existing fence shader");})&&d.ps==g_curPs&&Same(d.pixel,refusedCaps),"refused cap patch falls back without changing constants");
    ResetGlobals();d=Device();fixtureAtlas=&atlas;g_fenceFix=true;g_objPixelLamps=true;fixtureGroup=GroupCentre{};fixtureCaps=true;d.failPixelRead=120;
    const auto unreadableCaps=d.pixel;
    Check(DrawInstanced(&d,[&]{Check(d.ps==&replacementPs,"unreadable cap constants retain the existing fence shader");})&&d.ps==g_curPs&&Same(d.pixel,unreadableCaps),"unreadable cap constants fail closed");
    ResetGlobals();
    std::printf("Extracted DrawInstanced: %d checks\n",checks-before);
}
HRESULT NativeSet(FakeDevice* d,DWORD s,FakeTexture* t) { return d->SetTexture(s,t); }
void NativeSamplerChecks() {
    const int before=checks;FakeTexture native{10},previous{11};
    // 22 state reads, 11 state sets, texture get, and texture set are faulted
    // individually. GetTexture returns an owned reference in this fixture.
    for(int fault=-1;fault<35;++fault) {
        auto d=Device();d.textures[3]=&previous;const auto original=d.sampler;const auto originalTextures=d.textures;
        if(fault>=0&&fault<22)d.failSamplerReadAt=fault;
        if(fault>=22&&fault<33)d.failSamplerWriteAt=fault-22;
        if(fault==33)d.failTextureReadAt=0;
        if(fault==34)d.failTextureWriteAt=0;
        {NativeTerrainSampler guard(&d,2,3,&native,NativeSet);
            Check(bool(guard)==(fault==-1),"native alias reports success only with no injected failure");
            if(guard)Check(d.textures[3]==&native&&Same(d.sampler[2],d.sampler[3]),"native alias texture and all eleven states match source");
        }
        Check(Same(d.sampler,original)&&Same(d.textures,originalTextures),"native alias restores texture and every state after all partial failure paths");
        Check(previous.refs==1&&native.refs==1,"native alias releases its captured texture reference after all failure paths");
    }
    auto d=Device();d.textures[3]=&native;d.sampler[3]=d.sampler[2];
    { NativeTerrainSampler guard(&d,2,3,&native,NativeSet);Check(bool(guard),"already aliased sampler succeeds"); }
    Check(d.writes==0&&native.refs==1,"already aliased sampler avoids every setter and releases captured reference");
    { NativeTerrainSampler guard(&d,2,2,&native,NativeSet);Check(!guard&&d.writes==0,"same source and spare sampler fail closed"); }
    { NativeTerrainSampler guard(&d,2,3,nullptr,NativeSet);Check(!guard&&d.writes==0,"null native map fails closed"); }
    std::printf("Production NativeTerrainSampler: %d checks\n",checks-before);
}
void ExpandedStateChecks() {
    const int before=checks;
    for(bool alpha:{false,true})for(bool blend:{false,true})for(bool z:{false,true})for(bool stencil:{false,true}) {
        ResetGlobals();auto d=Device();g_wallGain=2;
        d.render[D3DRS_COLORWRITEENABLE]=7|(alpha?8:0);
        d.render[D3DRS_ALPHABLENDENABLE]=blend;
        d.render[D3DRS_ZWRITEENABLE]=z;
        d.render[D3DRS_STENCILWRITEMASK]=stencil?255:0;
        const auto saved=d.render;const auto pixel=d.pixel;int draws=0;
        const bool dual=alpha&&!blend;
        Check(DrawWallGain(&d,[&]{++draws;
            if(dual&&draws==1){Check(d.render[D3DRS_COLORWRITEENABLE]==8,"bloom pass writes alpha only");Check(d.render[D3DRS_ZWRITEENABLE]==0&&d.render[D3DRS_STENCILWRITEMASK]==0,"bloom pass has no depth or stencil writes");Check(Same(d.pixel,pixel),"bloom pass retains native constants");}
            else Check(d.pixel[7][0]==pixel[7][0]*2,"geometry pass uses stronger wall term");
        }),"wall state scenario claims draw");
        Check(draws==(dual?2:1),"opaque alpha gets two passes; all other states one");
        Check(Same(saved,d.render)&&Same(pixel,d.pixel)&&!g_inOwnCall,"all wall render states and constants restored");
    }
    ResetGlobals();g_lampData[0][3]=10;g_lampData[16][0]=.7f;
    g_lampCone[0].spill=.2f;g_lampCone[0].c1[3]=0;g_lampCone[0].c2[3]=0;
    float direct[33][4]{},centred[33][4]{},rows[16][4]{};float share=1;
    Check(PickObjectLamps(0,0,0,direct,share,.2f,rows)==1,"direct picker retains selected lamp");
    Check(direct[2][0]==.7f&&direct[2][3]==.2f,"direct picker preserves raw color and spill");
    Check(Same(rows[0],g_lampCone[0].c1)&&Same(rows[1],g_lampCone[0].c2),"picker uploads both real cone rows exactly");
    Check(PickObjectLamps(0,0,0,centred,share,.2f)==1&&Near(centred[2][0],.14f),"legacy caller retains centre cone multiplication");
    g_lampCone[0]=LampCone{};
    std::printf("Expanded wall state and cone upload: %d checks\n",checks-before);
}
// Buildings block light on objects (LampVisibility stubbed): the share scales a kept lamp's colour and the ground share,
// under 1% drops it; the ground gate fades the ground term over 1.5 m above the highest lamp head within 15 m
void VisibilityChecks() {
    const int before=checks;FakeTexture atlas{40};
    ResetGlobals();g_lampCone[0]=LampCone{};
    g_lampData[0][0]=0;g_lampData[0][1]=2;g_lampData[0][2]=0;g_lampData[0][3]=10;g_lampData[16][0]=.7f;g_lampData[16][1]=.6f;g_lampData[16][2]=.5f;
    float rows[33][4]{},cones[16][4]{};float share=1,top=-1e9f;
    fixtureVisibility=.5f;
    Check(PickObjectLamps(0,0,0,rows,share,.2f,cones,&top)==1,"half-visible lamp kept");
    Check(Near(rows[2][0],.35f)&&Near(rows[2][1],.3f)&&Near(rows[2][2],.25f),"kept colour scaled by the visible share");
    Check(Near(share,.5f),"ground share follows the visible share");
    Check(top==2.f,"highest lamp head within 15 m reported");
    fixtureVisibility=.005f;share=1;
    Check(PickObjectLamps(0,0,0,rows,share,.2f,cones)==0&&share==0.f,"lamp under 1% visible left out and ground share zero");
    fixtureVisibility=1;share=1;top=-1e9f;g_lampData[0][0]=20;
    Check(PickObjectLamps(0,0,0,rows,share,.2f,cones,&top)==1&&top==-1e9f,"lamp beyond 15 m does not set the lamp top");
    g_lampData[0][0]=0;
    // ground gate: origin y .39 (VS c9.w), lamp head .75 below it: half the ground strength
    ResetGlobals();auto d=Device();fixtureAtlas=&atlas;g_objPixel=true;g_objPixelLamps=true;g_objPixelStrength=1;g_objPixelLampStrength=1;g_night=1;
    RigTracker::fixtureMode=2;g_lampData[0][1]=.39f-.75f;
    Check(DrawObjectLamp(&d,[&]{Check(Near(d.pixel[41][0],.5f),"object 0.75 m above the highest lamp head keeps half the ground light");}),"gated object draws");
    g_lampData[0][1]=.39f-2.f;d=Device();
    Check(DrawObjectLamp(&d,[&]{Check(d.pixel[41][0]==0.f,"object 1.5 m or more above every lamp head takes no ground light");}),"fully gated object draws");
    // fences: the colour of a kept lamp x the largest share of the three points; none left: the slot stays empty
    ResetGlobals();d=Device();fixtureAtlas=&atlas;g_fenceFix=true;g_fenceStrength=1;g_night=1;g_lampData[0][1]=2;fixtureGroup=GroupCentre{};
    fixtureVisibility=.5f;
    Check(DrawInstanced(&d,[&]{Check(Near(d.pixel[44][0],.35f)&&Near(d.pixel[44][2],.25f),"fence lamp colour scaled by the visible share");}),"fence draws with a dimmed lamp");
    fixtureVisibility=0;d=Device();
    Check(DrawInstanced(&d,[&]{Check(d.pixel[44][0]==0.f&&d.pixel[43][0]==1e6f,"blocked fence lamp leaves its slot empty");}),"fence draws with no lamp");
    fixtureVisibility=1;
    std::printf("Buildings block light on objects: %d checks\n",checks-before);
}
// Realistic reach (12/10): the reach variant gets the walls' scale x the slider trims, R and the lamp's weight in its rows
// and the r0^2 rows right after the cones; every row restored; reach off keeps the first law's values
void ReachChecks() {
    const int before=checks;FakeTexture atlas{50};
    constexpr unsigned N=ShaderPatches::kObjectPixelLamps;
    constexpr UINT reachRow=42+1+4*N; // lampParamConst + 1 + 4N
    for(bool walls:{false,true})for(float night:{.5f,1.f})for(float strength:{.75f,1.5f})for(bool reach:{false,true})for(float entries:{1.f,2.f}){
        float r0,R;LampReach::Radii(144.5f,r0,R);const float weight=LampReach::Gain(144.5f,entries);
        // the native wall scale read 1.5 s ago (fresh: NativeWallScale keeps it)
        auto setup=[&]{g_wallEnabled=walls;g_wallGain=.3f;g_nativeWallScale=1.0304f;g_lotDrawTick=5000;g_nativeWallTick=3500;fixtureReach=reach;g_night=night;
            g_lampData[0][0]=0;g_lampData[0][1]=2;g_lampData[0][2]=0;g_lampData[0][3]=10;g_lampData[16][0]=.2f;g_lampData[16][1]=.1f;g_lampData[16][2]=.05f;
            g_lampCone[0]=LampCone{};g_lampCone[0].w=144.5f;g_lampCone[0].entries=entries;fixtureVisibility=1;};
        ResetGlobals();auto d=Device();fixtureAtlas=&atlas;setup();
        g_objPixel=true;g_objPixelLamps=true;g_objPixelStrength=1;g_objPixelLampStrength=strength;RigTracker::fixtureMode=2;
        const float objScale=reach?TerrainLightingPolicy::ReachLampScale(1.0304f,night,walls,.3f,strength/.75f):TerrainLightingPolicy::SurfaceLampGain(night,strength);
        auto ps=d.pixel;int calls=0;
        Check(DrawObjectLamp(&d,[&]{++calls;
            Check(d.pixel[42][1]==objScale,reach?"reach object: the walls' scale x the slider trim":"first-law object: its own strength as before");
            Check(Near(d.pixel[43][3],reach?1.f/(R*R):1.f/100.f),reach?"reach object: the lamp row holds R of the reach law":"first-law object: 1/R^2 as before");
            Check(Near(d.pixel[44][0],.2f*(reach?weight:1.f))&&Near(d.pixel[44][2],.05f*(reach?weight:1.f)),"object colour x the lamp's weight with reach only");
            Check(reach?Near(d.pixel[reachRow][0],r0*r0)&&d.pixel[reachRow][1]==0.f&&d.pixel[reachRow+1][3]==0.f:Same(d.pixel[reachRow],ps[reachRow]),"r0^2 rows uploaded with reach only");
        }),"object draws");
        Check(calls==1&&Same(d.pixel,ps)&&g_reachObjDraws==uint32_t(reach),"object restores every row and counts reach draws");
        // fences: both trims
        ResetGlobals();d=Device();fixtureAtlas=&atlas;setup();
        g_fenceFix=true;g_fenceStrength=1.5f;g_objPixelLamps=true;g_objPixelLampStrength=strength;fixtureGroup=GroupCentre{};
        const float fenceScale=reach?TerrainLightingPolicy::ReachLampScale(1.0304f,night,walls,1.f,strength/.75f*(1.5f/.75f)):TerrainLightingPolicy::SurfaceLampGain(night,strength)*1.5f;
        ps=d.pixel;calls=0;
        Check(DrawInstanced(&d,[&]{++calls;
            Check(d.pixel[42][1]==fenceScale,reach?"reach fence: the walls' scale x both trims":"first-law fence: its strength as before");
            Check(Near(d.pixel[43][3],reach?1.f/(R*R):1.f/100.f)&&Near(d.pixel[44][0],.2f*(reach?weight:1.f)),"fence lamp row and colour follow the law");
            Check(reach?Near(d.pixel[reachRow][0],r0*r0):Same(d.pixel[reachRow],ps[reachRow]),"fence r0^2 rows with reach only");
        }),"fence draws");
        Check(calls==1&&Same(d.pixel,ps)&&g_reachFenceDraws==uint32_t(reach),"fence restores every row and counts reach draws");
    }
    // fences test each lamp from the piece nearest it (12/10, F7 18-17-00 #393), the centre only with no piece read
    ResetGlobals();auto d=Device();fixtureAtlas=&atlas;g_fenceFix=true;g_objPixelLamps=true;g_night=1;fixtureVisibility=1;
    g_lampData[0][0]=-6;g_lampData[0][1]=2;g_lampData[0][2]=0;g_lampData[0][3]=10;g_lampData[16][0]=.2f;
    fixtureGroup=GroupCentre{};fixtureGroup.y=1;fixtureGroup.pieces=2;
    fixtureGroup.piece[0][0]=5;fixtureGroup.piece[1][0]=-5;fixtureGroup.piece[0][1]=fixtureGroup.piece[1][1]=1;
    Check(DrawInstanced(&d,[]{})&&visCalls>=1&&firstVis[0]==-5.f&&firstVis[1]==1.5f&&firstVis[2]==0.f,"fence lamp tested from the piece nearest it, 0.5 m up");
    ResetGlobals();d=Device();fixtureAtlas=&atlas;g_fenceFix=true;g_objPixelLamps=true;g_night=1;fixtureGroup.pieces=0;fixtureGroup.x=3;
    Check(DrawInstanced(&d,[]{})&&firstVis[0]==3.f&&firstVis[1]==1.5f,"no piece read: the group's centre as before");
    fixtureGroup=GroupCentre{};
    // (12/10 review) the lamp's list entries: 0.25 w each, 2 for a story-0 lot lamp (the fitted 0.5 w, bit for bit), else 1
    Check(LampReach::Entries(true,0)==2.f&&LampReach::Entries(true,1)==1.f&&LampReach::Entries(true,-1)==1.f&&LampReach::Entries(false,0)==1.f,"entries: story-0 lot lamp 2, others 1");
    for(float w:{12.75f,85.f,144.5f,1e-9f}) Check(LampReach::Gain(w,2.f)==.5f*w&&LampReach::Gain(w,1.f)==.25f*w,"gain per entry 0.25 w, two entries the fitted 0.5 w exactly");
    Check(LampReach::Gain(-1.f,2.f)==42.5f&&LampReach::Gain(NAN,1.f)==21.25f,"unreadable w counts as 85");
    // the native wall scale: trusted for 2 s after a wall draw read it, else (never read, stale, a daytime 0) the night value 1
    ResetGlobals();g_nativeWallScale=0;
    Check(NativeWallScale()==1.f&&!NativeWallFresh(),"native wall scale never read: night value 1");
    g_nativeWallTick=1000;g_lotDrawTick=3000;Check(NativeWallScale()==0.f,"read 2 s ago: kept (a daytime 0 too)");
    g_lotDrawTick=3001;Check(NativeWallScale()==1.f,"read over 2 s ago: night value 1");
    g_night=0;Check(NativeWallScale()==TerrainLightingPolicy::kLampDayFloor,"read over 2 s ago by day: the daytime curve's 3%, not the night value (12/10)");
    g_night=.5f;Check(NativeWallScale()==TerrainLightingPolicy::LampDay(.5f),"read over 2 s ago at dusk: the daytime curve");g_night=1;
    g_nativeWallTick=0xFFFFFF00u;g_lotDrawTick=0x100u;Check(NativeWallScale()==0.f,"tick wrap handled");
    ResetGlobals();d=Device();g_wallGain=2;g_lotDrawTick=7000;d.pixel[7]={.6f,.23f,-.5f,1.25f};
    Check(DrawWallGain(&d,[]{})&&g_nativeWallScale==.6f&&g_nativeWallTick==7000u,"a wall draw records the native scale and its tick");
    ResetGlobals();d=Device();g_wallGain=2;g_lotDrawTick=0;d.pixel[7]={.6f,.23f,-.5f,1.25f};
    Check(DrawWallGain(&d,[]{})&&g_nativeWallTick==1u&&NativeWallScale()==1.f,"a read before the first Present tick is not trusted");
    // a stale daytime 0 no longer leaves a reach object unlit at night
    ResetGlobals();d=Device();fixtureAtlas=&atlas;fixtureReach=true;g_night=1;g_wallEnabled=false;g_nativeWallScale=0;g_nativeWallTick=100;g_lotDrawTick=100000;
    g_objPixel=true;g_objPixelLamps=true;g_objPixelLampStrength=.75f;RigTracker::fixtureMode=2;g_lampCone[0]=LampCone{};g_lampCone[0].w=85;
    g_lampData[0][0]=0;g_lampData[0][1]=2;g_lampData[0][2]=0;g_lampData[0][3]=10;g_lampData[16][0]=.2f;
    Check(DrawObjectLamp(&d,[&]{Check(d.pixel[42][1]==1.f,"stale native scale: the reach object takes the night value x its trim");}),"reach object draws");
    // fences take the reach copy only when they place lamps (12/10 review): per-pixel lamps off, or no group read -> first law
    for(int mode=0;mode<2;++mode){
        ResetGlobals();d=Device();fixtureAtlas=&atlas;g_fenceFix=true;g_night=1;fixtureReach=true;fixtureGroup=GroupCentre{};
        g_objPixelLamps=mode!=0;fixtureNoGroup=mode!=0;const auto ps=d.pixel;
        Check(DrawInstanced(&d,[&]{Check(Same(d.pixel[reachRow],ps[reachRow])&&d.pixel[42][0]==0.f,"no lamps placed: no reach rows, cS.x 0");}),"fence without lamps draws");
        Check(g_reachFenceDraws==0&&g_reachLawRefused==0,mode?"no group read: first law, not counted":"per-pixel lamps off: first law, not counted");
    }
    g_objPixelLamps=true;
    // the reach copy refused (no room for its rows): the first law draws and the refusal is counted
    ResetGlobals();d=Device();fixtureAtlas=&atlas;fixtureReach=true;fixtureReachRefused=true;g_night=1;
    g_objPixel=true;g_objPixelLamps=true;g_objPixelLampStrength=.75f;RigTracker::fixtureMode=2;g_lampCone[0]=LampCone{};g_lampCone[0].w=85;g_lampCone[0].entries=2;
    Check(DrawObjectLamp(&d,[&]{Check(d.pixel[42][1]==TerrainLightingPolicy::SurfaceLampGain(1,.75f)&&Near(d.pixel[43][3],1.f/100.f)&&Near(d.pixel[44][0],.2f),"refused reach: object keeps the first law");}),"object draws");
    Check(g_reachLawRefused==1&&g_reachObjDraws==0,"object refusal counted, no reach draw");
    ResetGlobals();d=Device();fixtureAtlas=&atlas;fixtureReach=true;fixtureReachRefused=true;g_fenceFix=true;g_fenceStrength=1;g_night=1;g_objPixelLamps=true;fixtureGroup=GroupCentre{};
    Check(DrawInstanced(&d,[&]{Check(Near(d.pixel[43][3],1.f/100.f)&&Near(d.pixel[44][0],.2f),"refused reach: fence keeps the first law");}),"fence draws");
    Check(g_reachLawRefused==1&&g_reachFenceDraws==0,"fence refusal counted, no reach draw");
    // reach copy chosen but no world position read (no lamps placed): not counted as a reach draw
    ResetGlobals();d=Device();fixtureAtlas=&atlas;fixtureReach=true;g_night=1;objectVsInfo.patched.worldK=-1;
    g_objPixel=true;g_objPixelLamps=true;RigTracker::fixtureMode=2;
    Check(DrawObjectLamp(&d,[&]{Check(d.pixel[42][0]==0.f&&d.pixel[42][1]==0.f,"no lamps placed: cS.x and cS.y 0");}),"object without world position draws");
    Check(g_reachObjDraws==0,"no lamps placed: not counted as a reach draw");
    objectVsInfo.patched.worldK=8;g_lampCone[0]=LampCone{};
    // F7 on a fence draw describes it (12/10 review), and the text does not outlive the draw
    ResetGlobals();d=Device();fixtureAtlas=&atlas;fixtureCapture=true;g_fenceFix=true;g_night=1;g_objPixelLamps=true;fixtureGroup=GroupCentre{};
    Check(DrawInstanced(&d,[&]{Check(g_objDrawInfo.starts_with("FENCE/STAIRS (instanced) lit by the mod | per-pixel lamps: 1"),"F7 fence line during the draw");}),"captured fence draws");
    Check(g_objDrawInfo.empty(),"F7 fence line cleared after the draw");
    fixtureCapture=false;
    std::printf("Realistic reach on objects and fences: %d checks\n",checks-before);
}
int main() {
    WallChecks();LotChecks();NativeSamplerChecks();ObjectChecks();InstancedChecks();ExpandedStateChecks();VisibilityChecks();ReachChecks();
    std::printf("Resource paths: %d checks, %d failures; extracted production, fake D3D9, no game/driver execution\n",checks,failures);
    return failures?1:0;
}
