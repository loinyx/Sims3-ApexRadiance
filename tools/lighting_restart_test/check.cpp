#define NOMINMAX
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <format>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include "features/lighting_restart_policy.h"
#define LOG_INFO(...) ((void)0)
unsigned checks = 0;
void Check(bool ok, const char* message) {
    ++checks;
    if (!ok) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); }
}
namespace Rooms {
struct Key { uintptr_t tracker; int level, room; };
struct RigRoomWatch { Key key; uintptr_t mgr; };
struct RestartProgress { unsigned scheduled, total, pending; bool active; };
std::map<std::pair<uintptr_t,int>,uintptr_t> managers;
std::map<uintptr_t,int> states;
std::vector<uintptr_t> sent;
unsigned polls = 0, invalidations = 0, bumps = 0;
bool deferred = false, t_queueForce = false;
std::atomic<bool> g_installed{true};
std::atomic<long> g_floorEditGen{0}, g_indoorGen{0};
std::atomic<unsigned> g_relightAllAt{0};
std::mutex g_relightWhyMx;
std::string g_relightWhy;
uintptr_t kRootPtr=1,kRoomById=1,kInvalidateRoom=1,kSetInsert=1;
void LoadAddresses() {}
int AllTrackers(uintptr_t* out,int) { out[0]=100;out[1]=200;out[2]=300;return 3; }
int ActiveLotTrackers(uintptr_t* out,int) { out[0]=200;return 1; }
uintptr_t SafeStoryManager(uintptr_t tracker,int story) {
    const auto it=managers.find({tracker,story});return it==managers.end()?0:it->second;
}
void BumpGeoGen(uintptr_t) { ++bumps; }
namespace OutdoorDelta {void InvalidateAll(const char*) { ++invalidations; }}
namespace MeshOcclusion {void Reset() {}}
namespace DragLite {enum class Why {RelightAll};struct WhyScope {explicit WhyScope(Why) {}};}
bool RigWorkPending(uintptr_t) { return deferred; }
int WatchedRoomState(const RigRoomWatch& room) {
    ++polls;
    const auto it=states.find(room.mgr);return it==states.end()?-1:it->second;
}
int RequeueStory(uintptr_t tracker,int story,unsigned,int*,std::vector<RigRoomWatch>* watch) {
    Check(t_queueForce,"restart bypasses cached delta solves");
    const uintptr_t mgr=SafeStoryManager(tracker,story);
    sent.push_back(mgr);states[mgr]=2;watch->push_back({{tracker,story,0},mgr});return 1;
}
#include "restart_extracted.h"
void Test() {
    managers={{{100,-1},11},{{100,0},12},{{200,1},21}};
    g_installed=false;Check(!BeginLightingRestart(),"disabled hooks reject restart");g_installed=true;
    Check(BeginLightingRestart(),"restart starts with loaded stories");
    Check(g_restartTotal==3 && invalidations==1,"only loaded stories retained, lighting cache invalidated once");
    Check(!BeginLightingRestart() && invalidations==1,"duplicate restart leaves pending work intact");
    PumpLightingRestart();Check(sent.size()==1 && sent[0]==21,"active lot queues first, one story per frame");
    Check(!t_queueForce,"force-solve scope restored after dispatch");
    PumpLightingRestart();Check(sent.size()==2 && sent[1]==11,"basements included after the active lot");
    PumpLightingRestart();Check(sent.size()==3 && sent[2]==12 && bumps==3,"each loaded story rebuilds geometry exactly once");
    PumpLightingRestart();Check(g_restartActive,"queued or running rooms keep restart active");
    states[11]=states[12]=states[21]=4;deferred=true;
    PumpLightingRestart();Check(g_restartActive,"deferred ambient or dependent work blocks completion");
    deferred=false;states[21]=0;PumpLightingRestart();Check(g_restartActive,"an unsolved native state is not completion");
    states.erase(21);PumpLightingRestart();
    Check(!g_restartActive && g_restartRooms.empty(),"unloaded rooms do not leave dangling waits");
    Check(g_restartRooms.capacity()==0 && g_restartStories.capacity()==0,"completed restart releases its queue storage");
    Check(LightingRestartProgress().total==3,"completion retains progress counters");
    BeginLightingRestart();g_restartRooms.assign(8192,{{100,0,0},12});states[12]=2;
    const auto before=sent.size();polls=0;PumpLightingRestart();
    Check(sent.size()==before && polls==64,"backpressure limits storage and room polling per frame");
    CancelLightingRestart();Check(!g_restartActive && g_restartStories.empty() && g_restartRooms.empty(),"world cancellation drops all queued references");
}
}
namespace ProxyCheck {
struct Story {std::vector<int> edges;};
struct Lot {std::vector<Story> stories;};
struct Proxy {uintptr_t tracker=0;uint64_t geo=0;uint32_t gen=0;Lot lot;};
std::mutex g_proxyMx;
std::shared_ptr<Proxy> g_proxy;
std::string g_proxyWhy;
std::atomic<uint32_t> g_proxyEpoch{1},g_proxyGen{1};
std::atomic<unsigned> c_proxyBuilds{0};
bool resetDuringBuild=false, buildOk=true;
namespace LevelLightShare {
uint64_t LotGeometryKey(const void*) {return 7;}
bool ExportLotProxy(uintptr_t,Lot&,std::string&) {
    if(resetDuringBuild)g_proxyEpoch.fetch_add(1);return buildOk;
}
}
double Ms(LARGE_INTEGER,LARGE_INTEGER) {return 0;}
#include "proxy_extracted.h"
void Test() {
    resetDuringBuild=true;BuildProxy(1,reinterpret_cast<void*>(1));
    Check(!g_proxy && !c_proxyBuilds,"in-flight proxy from before reset cannot publish");
    resetDuringBuild=false;BuildProxy(1,reinterpret_cast<void*>(1));
    Check(g_proxy && c_proxyBuilds==1,"fresh proxy publishes after reset");
    BuildProxy(1,reinterpret_cast<void*>(1));Check(c_proxyBuilds==1,"ordinary unchanged geometry still reuses its proxy");
    g_proxy.reset();buildOk=false;BuildProxy(1,reinterpret_cast<void*>(1));
    Check(!g_proxy && c_proxyBuilds==1,"failed rebuild never publishes a proxy");
}
}
namespace Coordinator {
LightingRestartPolicy::Run g_restart;
bool g_restartRequested=false,g_restartUnavailable=false,g_restartFinishing=false,g_live=true;
bool g_restartPartial=false;
std::atomic<bool> g_kickRequested{false};
int g_rebuildId=0,g_sweepId=0;
std::string g_pendingReason;
DWORD tick=1000;
DWORD GetTickCount() {return tick;}
namespace WorldSession {bool live=true;bool InWorld() {return live;}}
namespace LevelLightShare {
bool accepted=true;unsigned stops=0;
Rooms::RestartProgress progress{};
bool BeginLightingRestart() {return accepted;}
Rooms::RestartProgress LightingRestartProgress() {return progress;}
void StopLightingRestart() {++stops;}
}
namespace LightmapSmooth {unsigned clears=0;void Clear() {++clears;}bool HoldPending() {return false;}}
namespace ChunkRelight {bool Busy() {return false;}}
namespace RtOutdoorLamps {
unsigned resets=0;bool pending=false,incomplete=false;
void RestartLighting() {++resets;pending=true;}
bool RestartPending() {return pending && !incomplete;}
bool RestartIncomplete() {return incomplete;}
}
namespace ObjectLightBridge {
bool pending=false;void RequestRigRefresh() {pending=true;}bool RigRefreshPending() {return pending;}
}
namespace UnlitRooms {
bool pending=false;void RigsAgainIn(unsigned) {pending=true;}bool RigRefreshPending() {return pending;}
}
#include "coordinator_extracted.h"
void Test() {
    g_restartRequested=true;LevelLightShare::accepted=false;PumpLightingRestart();
    Check(g_restartUnavailable && !LightmapSmooth::clears,"rejected restart releases no maps");
    LevelLightShare::accepted=true;g_restartRequested=true;PumpLightingRestart();
    Check(g_restart.active && LightmapSmooth::clears==1 && RtOutdoorLamps::resets==1,"accepted restart discards terrain and outdoor maps once");
    Check(g_kickRequested.load(),"accepted restart arms the paced terrain rebuild");
    tick=5000;PumpLightingRestart();Check(!g_restart.done,"queued terrain and GPU work block completion despite elapsed time");
    g_kickRequested=false;RtOutdoorLamps::pending=false;LevelLightShare::progress.active=true;
    PumpLightingRestart();Check(!g_restart.done,"room dispatcher blocks completion");
    LevelLightShare::progress.active=false;PumpLightingRestart();
    Check(g_restartFinishing && UnlitRooms::pending,"idle maps trigger a final object refresh");
    tick=6000;PumpLightingRestart();Check(!g_restart.done,"undispatched object refresh blocks completion");
    ObjectLightBridge::pending=UnlitRooms::pending=false;tick=6750;PumpLightingRestart();
    Check(g_restart.done && !g_restart.active,"completion follows idle maps and dispatched object refresh");
    g_restart.Start(7000);WorldSession::live=false;PumpLightingRestart();
    Check(g_restart.interrupted && LevelLightShare::stops==1 && !g_restart.done,"leaving the world stops room dispatch without claiming success");
    WorldSession::live=true;g_restart.Start(7000);RtOutdoorLamps::incomplete=true;RtOutdoorLamps::pending=true;
    tick=8000;PumpLightingRestart();
    ObjectLightBridge::pending=UnlitRooms::pending=false;
    tick=9000;PumpLightingRestart();
    Check(g_restart.done && g_restartPartial && !g_restart.active,"terminal fallback finishes with an explicit partial outcome");
}
}
namespace Fallback {
constexpr int kSolveAsks=3;
struct Slot {bool used=true,built=true,ok=true,relit=true;};
std::vector<std::unique_ptr<Slot>> g_slots;
std::vector<int> g_jobs;
std::atomic<bool> g_option{true},g_watch{true},g_on{true};
bool g_restartRequested=false,g_budgetOut=false,g_proxyUsed=true;
struct Cover {bool full=true;} g_cover;
std::mutex g_coverMx;
#include "fallback_extracted.h"
void Test() {
    g_slots.push_back(std::make_unique<Slot>());
    Check(!RestartIncomplete() && !RestartPending(),"covered ready maps are idle");
    g_budgetOut=true;g_cover.full=false;
    Check(RestartIncomplete() && !RestartPending(),"memory-budget fallback is not endless queued work");
    g_jobs.push_back(1);Check(!RestartIncomplete() && RestartPending(),"in-flight work remains pending even with fallback");g_jobs.clear();
    g_budgetOut=false;g_slots[0]->ok=false;
    Check(RestartIncomplete() && !RestartPending(),"permanent prepared-map failure is terminal fallback");
    g_slots[0]->built=false;Check(!RestartIncomplete() && RestartPending(),"a retryable upload remains pending");
    g_slots[0]->built=true;
    g_slots.push_back(std::make_unique<Slot>());g_slots[1]->built=false;g_slots[1]->relit=false;
    g_restartGpuPending=true;
    Check(!RestartIncomplete() && RestartPending(),"a failed slot cannot hide another story upload backoff");
    g_slots[1]->built=true;g_slots[1]->relit=false;g_restartGpuPending=false;
    Check(!RestartIncomplete() && RestartPending(),"a terminal slot cannot hide another map waiting for relight");
    g_slots[1]->built=g_slots[1]->relit=true;g_restartGpuPending=false;
    Check(RestartIncomplete() && !RestartPending(),"fallback becomes terminal only after recoverable work ends");
    g_slots.clear();g_on=false;g_proxyUsed=false;g_cover.full=false;g_restartGpuFallback=true;
    Check(RestartIncomplete() && !RestartPending(),"exhausted proxy sends finish partially even without a slot");
    g_restartGpuPending=true;
    Check(!RestartIncomplete() && RestartPending(),"a native solve in flight still blocks exhausted proxy sends");
    g_restartGpuPending=false;g_proxyUsed=true;
    Check(RestartIncomplete() && !RestartPending(),"exhausted kept-solve sends finish partially without a slot");
    g_restartGpuFallback=false;
    Check(!RestartIncomplete() && RestartPending(),"a new attempt rearms pending work after fallback");
    Ask ask;ask.count=3;ask.logged=true;
    for(int state=0;state<6;state++)Check(RestartSolveTerminal(ask,state,false)==(state==4||state==5),"exhausted sends wait for a completed native solve");
    ask.logged=false;Check(!RestartSolveTerminal(ask,4,false),"the final dispatched request is not terminal yet");
    ask.count=0;Check(RestartSolveTerminal(ask,4,true),"memory budget uses the native completed solve as fallback");
    Check(!RestartSolveTerminal(ask,0,true),"memory budget does not finish a native solve early");
    g_watch=false;Check(!RestartIncomplete() && !RestartPending(),"inactive GPU path keeps the native solve behavior");
}
}
int main() {
    Rooms::Test();ProxyCheck::Test();Coordinator::Test();Fallback::Test();
    LightingRestartPolicy::Run run;
    Check(run.Start(100),"idle restart starts");
    Check(!run.Start(200),"active restart coalesces clicks");
    Check(!run.Observe(2100,true),"elapsed time does not complete busy work");
    Check(!run.Observe(2800,false) && run.Observe(2850,false),"completion requires a quiet interval after observed idle");
    Check(run.done && !run.active,"completion reported once");
    Check(!run.Observe(5000,false),"completed restart does not notify again");
    run.Start(6000);run.Cancel();Check(!run.done && run.interrupted,"world change cancels rather than reports success");
    run.Start(0xfffffff0u);Check(run.Observe(3000,false),"tick wrap preserves quiet and elapsed checks");
    std::printf("Lighting restart: %u checks passed\n",checks);
}
