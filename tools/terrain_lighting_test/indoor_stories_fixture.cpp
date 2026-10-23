#include "../../features/fast_math.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>
#include <intrin.h>
#include "../../features/mesh_occlusion.h"
using BYTE = unsigned char;
constexpr bool kPublicBuild = true;
constexpr bool kMemoryOptimizationsEnabled = true;
bool g_ghostSolve=false;
struct Cross { uintptr_t light=0; int floor=0, room=0; uintptr_t level=0; bool outdoor=false; };
struct RayEntry {bool valid=false;float lamp[3]{},at[3]{};};
enum {kTpIndoor};
struct PartTimer {explicit PartTimer(int){}};
namespace OutdoorDelta {bool Enabled(){return false;}void NoteDirectional(void*){}}
unsigned g_pointGen=0;
std::atomic<uint64_t> g_apexSolveCycles{0};
struct RoomInfo { uintptr_t mgr=0, tracker=0; int level=0, id=0; bool indoor=true; std::vector<Cross> cross; bool openings=false; };
struct GridWhy {bool wall=false;};
struct PassDebug { int cx=-1,cz=-1,cq=-1,belowRoom=-2; uint32_t keyHi=0,keyLo=0; float ch=0,ct=-1; } g_passDbg;
struct Xform {};
using LightPos_t = void(__thiscall*)(void*,float*);
using WallTest_t = bool(__thiscall*)(void*,void*,const float*,const void*,float*);
using RoomById_t = void*(__thiscall*)(void*,int);
alignas(16) BYTE managers[8][0x300], tiles[8][0x100], rooms[8][0x700];
struct Floor { uintptr_t manager; bool closed=false; } floors[8];
bool missing[8], missingRooms[8], wallsEnabled=true;
int wallBlocked=-1, attenuated=-1, raisedWallStory=-1, checks=0, failures=0;
BYTE* g_swapAt=nullptr; BYTE g_swapSaved=0;
char wallFlags[1]={1};
struct SolveCtx { const RoomInfo* info=nullptr; void* list2D=nullptr; const char* flags=wallFlags;
    bool batch=false,basis=false; BYTE soft=3; float thr=0; } g_ctx;
struct Segment {int story; float from,to;};
std::vector<Segment> wallSegments;
uintptr_t StoryManager(uintptr_t,int s) {return s>=0&&s<8 ? reinterpret_cast<uintptr_t>(managers[s]) : 0;}
uintptr_t LevelFor(uintptr_t m) {for(int s=0;s<8;++s)if(m==StoryManager(0,s))return missing[s]?0:reinterpret_cast<uintptr_t>(&floors[s]);return 0;}
uintptr_t LevelManager(uintptr_t f) {return reinterpret_cast<Floor*>(f)->manager;}
uintptr_t LevelForPoint(uintptr_t m) {return LevelFor(m);}
bool ReadXform(uintptr_t,Xform&) {return true;}
void ToLocal(const Xform&,const float* p,float* q) {std::memcpy(q,p,3*sizeof(float));}
uintptr_t LightTile(uintptr_t m,int x,int z) {if(x<0||x>=8||z<0||z>=8)return 0;for(int s=0;s<8;++s)if(m==StoryManager(0,s))return reinterpret_cast<uintptr_t>(tiles[s]);return 0;}
int TileRoom(uintptr_t,int) {return 1;}
int FloorAt(uintptr_t f,int,int,int) {return reinterpret_cast<Floor*>(f)->closed?1:0;}
int Quadrant(float x,float z) {return (x>=.5f?1:0)+(z>=.5f?2:0);}
struct Light { float position[4]; void Get(float* out) {std::memcpy(out,position,sizeof position);} };
struct ManagerMock {void* Find(int) {for(int s=0;s<8;++s)if(reinterpret_cast<uintptr_t>(this)==StoryManager(0,s))return missingRooms[s]?nullptr:rooms[s];return nullptr;}};
struct RoomMock {bool Wall(void*,const float* from,const void* target,float* keep) {
    for(int s=0;s<8;++s)if(reinterpret_cast<void*>(this)==rooms[s]) {
        const auto* to=static_cast<const float*>(target);
        wallSegments.push_back({s,from[1],to[1]});
        *keep=s==attenuated?.5f:1.f;
        return s!=wallBlocked && !(s==raisedWallStory && from[1]>6.2f && to[1]<6.2f);
    }return false;
}};
template<class T> uintptr_t Address(T p) {static_assert(sizeof p==sizeof(uintptr_t));uintptr_t result;std::memcpy(&result,&p,sizeof result);return result;}
uintptr_t kLightPos=Address(&Light::Get), kRoomById=Address(&ManagerMock::Find), kWallTest=Address(&RoomMock::Wall);
unsigned lampPositionReads=0;
void LampPos(void* p,float* out){++lampPositionReads;static_cast<Light*>(p)->Get(out);}
void* RoomByIdCached(void* m,int id){return static_cast<ManagerMock*>(m)->Find(id);}
std::atomic<bool> g_installed{true}; bool g_indoorReady=true;
std::atomic<unsigned long> g_gatherThread{7}; unsigned long ThreadId(){return 7;}
std::atomic<long> g_basisTests{0},g_basisBlocked{0};
const RoomInfo* basisInfo=nullptr;
const RoomInfo* SolveInfo(BYTE*) {return basisInfo;}
const Cross* FindCross(const RoomInfo& info,uintptr_t light) {for(const auto& c:info.cross)if(c.light==light)return &c;return nullptr;}
using BasisLight_t=void(__stdcall*)(const float*,void*,float*);
int nativeBasisCalls=0;
void __stdcall NativeBasis(const float*,void*,float* acc){++nativeBasisCalls;for(int i=0;i<16;++i)acc[i]+=float(i+1);}
uintptr_t kBasisLight=reinterpret_cast<uintptr_t>(&NativeBasis);
void IndoorShadow(const RoomInfo&,void*,const float*,float*);
bool innerBasisLamp=false;
float innerBasisPass=1,innerBasisNorm=1;
bool FindSame(const RoomInfo&,uintptr_t){return false;}
bool FindInner(const RoomInfo&,uintptr_t){return innerBasisLamp;}
float InnerPass(const RoomInfo&,void*,const float*,float*,GridWhy&){return innerBasisPass;}
float OpenLampPass(const RoomInfo&,void*,const float*,float*,GridWhy&){return 1;}
float InnerNormScaleAt(const RoomInfo&,void*,const BYTE*){return innerBasisNorm;}
std::atomic<long> g_innerBasisTests{0},g_roofedBasisTests{0},g_innerBasisBlocked{0},g_roofedBasisBlocked{0},g_dayScaled{0};
std::atomic<float> g_roomLampDay{1};
BYTE* RoofedRoom(BYTE*){return nullptr;}
bool OutdoorLamp(void*){return false;}
bool LampLit(void*){return true;}
void NoteDayRoom(BYTE*,float){}
#include "extracted_indoor_stories.h"
void IndoorShadow(const RoomInfo& info,void* light,const float* sample,float* colour) {
    const auto* c=FindCross(info,reinterpret_cast<uintptr_t>(light));int why=0;
    const float pass=c?IndoorPassImpl(info,*c,light,sample,why,nullptr):1;
    for(int i=0;i<4;++i)colour[i]*=pass;
}
void Check(bool ok,const char* what) {++checks;if(!ok){++failures;std::printf("FAIL: %s\n",what);}}
void Reset() {
    g_ghostSolve=false;
    innerBasisLamp=false;innerBasisPass=innerBasisNorm=1;
    lampPositionReads=0;
    wallSegments.clear();wallBlocked=attenuated=raisedWallStory=-1;g_ctx=SolveCtx{};basisInfo=nullptr;nativeBasisCalls=0;
    for(int s=0;s<8;++s) {
        std::memset(managers[s],0,sizeof managers[s]);std::memset(tiles[s],0,sizeof tiles[s]);std::memset(rooms[s],0,sizeof rooms[s]);
        *reinterpret_cast<float*>(managers[s]+0x98)=s*3.f;
        *reinterpret_cast<float*>(tiles[s]+0x78)=s*3.f;
        floors[s]={StoryManager(0,s),false};missing[s]=missingRooms[s]=false;rooms[s][0x639]=7;
    }
}
float Evaluate(int from,int to,int& why,float x=2.25f,bool reference=false,float heightOffset=1.5f,float lampHeightOffset=1.5f) {
    Light light{{x,from*3.f+lampHeightOffset,2.25f,1}};
    float sample[12]={x,to*3.f+heightOffset,2.25f};
    RoomInfo info{StoryManager(0,to),1,to,1,true,{}};
    Cross c{reinterpret_cast<uintptr_t>(&light),from,1,LevelFor(StoryManager(0,std::max(from,to)))};
    why=0;
#ifdef APEX_INDOOR_REFERENCE
    if(reference)return ReferenceIndoorPass(info,c,&light,sample,why,nullptr);
#else
    (void)reference;
#endif
    return IndoorPassImpl(info,c,&light,sample,why,nullptr);
}
int main() {
    {
        Reset();int why=0;g_ghostSolve=true;floors[1].closed=true;
        Check(Evaluate(0,1,why,2.25f,false,-.05f)==0&&why==1,"upper wall support row below a closed slab cannot import lower lamp light");
        floors[1].closed=false;Check(Evaluate(0,1,why,2.25f,false,-.05f)==1,"support row below a real floor opening keeps continuity");
        Reset();g_ghostSolve=true;floors[1].closed=true;
        Check(Evaluate(1,0,why,2.25f,false,3.05f)==0&&why==1,"lower wall support row above a closed slab cannot import upper lamp light");
        floors[1].closed=false;Check(Evaluate(1,0,why,2.25f,false,3.05f)==1,"upper support row through a real floor opening remains valid");
    }
    for(float share:{0.f,.25f,1.f})for(float norm:{.5f,1.f,1.5f,3.f}){
        Reset();Light light{{2.25f,1.5f,2.25f,1}};float point[4]={2.25f,1.5f,2.25f,1},acc[16];std::fill_n(acc,16,100.f);
        RoomInfo info{StoryManager(0,0),1,0,1,true,{}};basisInfo=&info;
        innerBasisLamp=true;innerBasisPass=share;innerBasisNorm=norm;
        BasisLightHook(rooms[0],nullptr,point,&light,acc);
        for(int i=0;i<16;++i)Check(acc[i]==100.f+float(i+1)*share*norm,"opening basis preserves earlier lamps and source normalisation above/below one");
        Check(nativeBasisCalls==(share>0?1:0),"blocked opening basis never evaluates native lamp");
    }
    {
        Reset();int why=0;
        MeshOcclusion::Publish({{{1,3,1},{4,3,1},{4,3,4}},{{1,3,1},{4,3,4},{1,3,4}}});
        Check(Evaluate(0,1,why)==0&&why==2,"real mesh blocks an otherwise open story boundary");
        MeshOcclusion::Reset();Check(Evaluate(0,1,why)==1,"empty tread gap retains native opening transmission");
        floors[1].closed=true;Check(Evaluate(0,1,why)==0,"native slab still blocks without collected mesh");
        MeshOcclusion::Publish({{{1,3,1},{4,3,1},{4,3,4}},{{1,3,1},{4,3,4},{1,3,4}}});
        Reset();Light light{{2.25f,1.5f,2.25f,1}};float point[4]={2.25f,4.5f,2.25f,1},acc[16]{};
        RoomInfo info{StoryManager(0,1),1,1,1,true,{{reinterpret_cast<uintptr_t>(&light),0,1,LevelFor(StoryManager(0,1))}}};
        basisInfo=&info;BasisLightHook(rooms[1],nullptr,point,&light,acc);
        Check(nativeBasisCalls==0,"directional basis rejects the same mesh-blocked imported lamp");
        MeshOcclusion::Reset();
    }
    for(int from=0;from<8;++from)for(int to=0;to<8;++to)if(from!=to) {
        Reset();int why=0;Check(Evaluate(from,to,why)==1&&why==0,"open atrium passes in both directions");
        Check(wallSegments.size()==size_t(std::abs(from-to)),"one wall segment per foreign story");
        const int direction=to>from?1:-1;
        for(size_t i=0;i<wallSegments.size();++i) {
            Check(wallSegments[i].story==from+direction*int(i),"wall segment uses correct story");
            Check(direction*(wallSegments[i].to-wallSegments[i].from)>0,"wall segment follows ray");
        }
        for(int b=std::min(from,to)+1;b<=std::max(from,to);++b) {
            Reset();floors[b].closed=true;Check(Evaluate(from,to,why)==0&&why==1,"every solid floor blocks light");
            Reset();missing[b]=true;Check(Evaluate(from,to,why)==0&&why==1,"unknown floor fails closed");
        }
        for(int story=from;story!=to;story+=direction) {
            Reset();wallBlocked=story;Check(Evaluate(from,to,why)==0&&why==2,"every foreign story wall can block");
            Check(rooms[story][0x639]==7&&!g_swapAt,"wall mode restored after refusal");
            Reset();attenuated=story;Check(Evaluate(from,to,why)==.5f,"wall transmission is preserved");
        }
        Reset();g_ctx.flags=nullptr;Check(Evaluate(from,to,why)==1&&wallSegments.size()==size_t(std::abs(from-to)+1),"borrowed lamp keeps wall tests without native flag");
        floors[std::max(from,to)].closed=true;Check(Evaluate(from,to,why)==0&&why==1,"floor test remains enabled without wall flag");
        Reset();Check(Evaluate(from,to,why,20)==0&&why==1,"ray outside floor grid fails closed");
        Reset();floors[std::max(from,to)].manager=0;Check(Evaluate(from,to,why)==0&&why==1,"stale floor ownership fails closed");
        Reset();
        for(int b=std::min(from,to)+1;b<=std::max(from,to);++b) *reinterpret_cast<float*>(tiles[b]+0x78)+=.25f;
        Check(Evaluate(from,to,why)==1,"tile floor heights are respected across stories");
        for(const auto& segment:wallSegments) {
            Check(rooms[segment.story][0x639]==7,"wall modes restored after successful ray");
        }
    }
#ifdef APEX_INDOOR_REFERENCE
    // Exercise wall rows on either side of a story plane, ghost rows and lamps
    // placed above/below their registry's nominal story. Adjacent behavior must
    // remain equivalent to the original native integration, including refusals.
    const float edgeOffsets[] = {-3.05f,-.25f,-.02f,0.f,.02f,.25f,1.5f,2.75f,2.98f,3.f,3.02f,3.25f,6.05f};
    for(int from=0;from<8;++from)for(int direction:{-1,1}) {
        const int to=from+direction;if(to<0||to>7)continue;
        for(float lamp:edgeOffsets)for(float sample:edgeOffsets)for(int scenario=0;scenario<5;++scenario) {
            Reset();int oldWhy=0,newWhy=0;
            if(scenario==1)floors[std::max(from,to)].closed=true;
            if(scenario==2)wallBlocked=from;
            if(scenario==3)attenuated=from;
            if(scenario==4)g_ctx.flags=nullptr;
            const float oldResult=Evaluate(from,to,oldWhy,2.25f,true,sample,lamp);
            const float newResult=Evaluate(from,to,newWhy,2.25f,false,sample,lamp);
            Check(oldResult==newResult&&oldWhy==newWhy,"adjacent wall/ghost edge behavior matches original production");
        }
    }
    for(int from=0;from<8;++from)for(int direction:{-1,1}) {
        const int to=from+direction;if(to<0||to>7)continue;
        for(int scenario=0;scenario<5;++scenario) {
            Reset();int oldWhy=0,newWhy=0;
            if(scenario==1)floors[std::max(from,to)].closed=true;
            if(scenario==2)wallBlocked=from;
            if(scenario==3)attenuated=from;
            if(scenario==4)g_ctx.flags=nullptr;
            const float oldResult=Evaluate(from,to,oldWhy,2.25f,true);
            const float newResult=Evaluate(from,to,newWhy);
            Check(oldResult==newResult&&oldWhy==newWhy,"adjacent results match previous production implementation");
        }
    }
#endif
    {
        Reset();int why=0;
        Check(Evaluate(2,0,why,2.25f,false,3.05f)==1,"ghost row above recipient boundary is not rejected by story count");
        Check(wallSegments.size()==2&&wallSegments[1].story==1,"partially crossed ray tests its terminal intermediate story");
        Reset();floors[1].closed=true;
        Check(Evaluate(2,0,why,2.25f,false,3.05f)==1,"floor below ghost sample does not block that ray");
        Reset();wallBlocked=1;
        Check(Evaluate(2,0,why,2.25f,false,3.05f)==0&&why==2,"terminal intermediate walls still block ghost rays");
        Reset();Check(Evaluate(2,0,why,2.25f,false,6.05f)==1,"ray entirely on home side of floors passes home walls");
        Reset();Check(Evaluate(0,2,why,2.25f,false,-3.05f)==1,"upward ghost row below recipient plane remains valid");
    }
    for(int from=0;from<8;++from)for(int to=0;to<8;++to)if(from!=to) {
        for(int scenario=0;scenario<4;++scenario)for(int story=std::min(from,to);story<=std::max(from,to);++story) {
            Reset();Light light{{2.25f,from*3.f+1.5f,2.25f,1}};
            float pos[4]={2.25f,to*3.f+1.5f,2.25f,1},acc[16];std::fill_n(acc,16,100.f);
            RoomInfo info{StoryManager(0,to),1,to,1,true,{{reinterpret_cast<uintptr_t>(&light),from,1,LevelFor(StoryManager(0,std::max(from,to)))}}};
            basisInfo=&info;if(scenario==1)wallBlocked=story;if(scenario==2)attenuated=story;if(scenario==3)missingRooms[story]=true;
            g_ctx.flags=nullptr;g_ctx.soft=19;g_ctx.thr=.75f;
            BasisLightHook(rooms[to],nullptr,pos,&light,acc);
            const float transmission=scenario==0?1:scenario==2?.5f:0;
            for(int i=0;i<16;++i)Check(acc[i]==100.f+float(i+1)*transmission,"basis changes only the imported lamp contribution");
            Check(nativeBasisCalls==(transmission>0?1:0),"blocked basis lamp does not evaluate native contribution");
            Check(g_ctx.flags==nullptr&&!g_ctx.basis&&g_ctx.soft==19&&g_ctx.thr==.75f,"basis restores calling context");
            Check(std::all_of(std::begin(rooms),std::end(rooms),[](const auto& r){return r[0x639]==7;}),"basis restores every story wall mode");
            if(scenario==0)Check(wallSegments.size()==2*size_t(std::abs(from-to)+1),"basis includes full-height veto and source, intermediate and recipient segments");
        }
    }
    {
        Reset(); raisedWallStory=1;
        Light light{{2.25f,7.3f,2.25f,1}};
        float pos[4]={2.25f,.5f,2.25f,1},acc[16]={};
        RoomInfo info{StoryManager(0,0),1,0,1,true,{{reinterpret_cast<uintptr_t>(&light),2,1,LevelFor(StoryManager(0,2))}}};
        basisInfo=&info;
        BasisLightHook(rooms[0],nullptr,pos,&light,acc);
        Check(nativeBasisCalls==0,"raised intermediate wall blocks basis outside the nominal story segment");
        Check(std::all_of(acc,acc+16,[](float v){return v==0;}),"isolated raised room adds no red to recipient basis map");
    }
    {
        int why=0;
        Reset();g_ctx.flags=nullptr;wallBlocked=0;
        Check(Evaluate(0,1,why)==0&&why==2,"borrowed lamp tests source walls when native batch omits them");
        Reset();g_ctx.flags=nullptr;wallBlocked=1;
        Check(Evaluate(0,1,why)==0&&why==2,"borrowed lamp tests recipient walls when native batch omits them");
        Reset();missingRooms[0]=true;
        Check(Evaluate(0,1,why)==0&&why==2,"missing source room fails closed for adjacent stories");
        Reset();Check(Evaluate(0,7,why)==1,"all-story shaft remains open");
        Check(lampPositionReads==1,"one lamp-position read for every boundary in a point solve");
    }
    std::printf("Indoor stories and basis: %d checks, %d failures\n",checks,failures);return failures?1:0;
}
