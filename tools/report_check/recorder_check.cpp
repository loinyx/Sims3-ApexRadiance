// Exercise the production request/OnPresent bodies, extracted without rewriting them.
#define NOMINMAX
#include <windows.h>
#include <atomic>
#include <format>
#include <map>
#include <mutex>
#include <string>
#include <vector>
#include <cassert>
#include <cstdio>
#include "recorder_limits_under_test.inc"
bool g_on=false;
std::atomic<bool> g_toggleRequest{false},g_stopRequest{false},g_cancelRequest{false};
DWORD g_startTick=0,g_lastStatus=0,g_savedAt=0,g_settleAt=0;
constexpr size_t kMaxNotes=40000;
struct Line { DWORD tick;std::string text; };
std::vector<Line> g_lines;
std::string g_settingsAtStart,g_saved;
size_t g_notes=0,g_roomNotes=0;
int starts=0,saves=0,notices=0,settles=0;
std::mutex g_editMx;
struct Edit { DWORD tick;uint32_t lot;int story;bool on,moved; };
std::vector<Edit> g_edits;
struct GroundShown { DWORD tick;int ix,iz;bool together; };
std::vector<GroundShown> g_ground;
struct CubeNote { DWORD tick=0;int writes=0;std::string text;std::vector<unsigned char> strip;unsigned side=0; };
std::map<std::pair<uintptr_t,std::string>,CubeNote> g_cubes;
std::map<const char*,long> g_counts;
void Start(){g_on=true;g_settleAt=0;g_startTick=GetTickCount();++starts;}
void Stop(){g_on=false;g_settleAt=0;++saves;}
void BeginSettle(){g_settleAt=GetTickCount();++settles;}
void Status(DWORD){}
void MeasureMemory(){}
void ReleaseBuffers(){}
#define LOG_INFO(text) ((void)0)
namespace DevSwitches { constexpr bool kCompiled=false; }
namespace LevelLightShare { std::string EndSeamRecording(bool){return {};} bool TreeLevelLot(uintptr_t,uint32_t&,int&){return false;} }
namespace I18n { const char* Tr(const char* text){return text;} }
namespace Captures { void Notify(const std::string&){++notices;} }
namespace CaptureBurst { bool running=false;bool Running(){return running;} }
namespace Hotkeys {enum class Action{Recorder};std::atomic<bool> pressed{false};bool Take(Action){return pressed.exchange(false);} }
#include "recorder_under_test.inc"
int main(){
    Recorder::RequestStop();Recorder::OnPresent();assert(starts==0&&saves==0&&!Recorder::Active());
    Recorder::RequestCancel();Recorder::OnPresent();assert(starts==0&&saves==0);
    Recorder::RequestToggle();Recorder::OnPresent();assert(starts==1&&Recorder::Active());
    g_lines.push_back({0,"recorded detail"});g_settingsAtStart="settings";g_saved="older saved recording";
    g_notes=g_roomNotes=3;
    g_startTick=GetTickCount()-kMaxMs-1000; // cancellation wins even when auto-save is due
    Recorder::RequestCancel();Recorder::RequestStop();Recorder::RequestToggle();Hotkeys::pressed=true;
    Recorder::OnPresent();assert(!Recorder::Active()&&saves==0&&g_lines.empty()&&g_settingsAtStart.empty()&&g_saved.empty()&&g_notes==0&&g_roomNotes==0&&notices==1);
    Recorder::OnPresent();assert(starts==1&&saves==0); // queued toggle and key must not restart it
    Recorder::RequestToggle();Recorder::OnPresent();assert(starts==2);
    Recorder::RequestStop();Recorder::OnPresent();assert(saves==1&&!Recorder::Active()); // Stop saves at once, with no wait
    Recorder::RequestStop();Recorder::OnPresent();assert(saves==1&&starts==2);
    Recorder::RequestToggle();Recorder::OnPresent();g_startTick=GetTickCount()-kMaxMs-1000;
    Recorder::OnPresent();assert(saves==2&&!Recorder::Active()); // the deadline saves at once too
    Recorder::RequestToggle();Hotkeys::pressed=true;Recorder::OnPresent();assert(starts==4&&Recorder::Active());
    Recorder::OnPresent();assert(starts==4&&saves==2); // simultaneous menu/key input consumed once
    Recorder::RequestToggle();Recorder::OnPresent();assert(settles==1&&saves==2&&Recorder::Active()&&Recorder::Settling()); // the stop key waits
    Recorder::OnPresent();assert(saves==2&&Recorder::Active());
    g_settleAt=GetTickCount()-kSettleMs+1000;Recorder::OnPresent();assert(saves==2); // not yet
    g_settleAt=GetTickCount()-kSettleMs;Recorder::OnPresent();assert(saves==3&&!Recorder::Active()&&!Recorder::Settling()); // the wait saves
    Recorder::RequestToggle();Recorder::OnPresent();assert(starts==5);
    Hotkeys::pressed=true;Recorder::OnPresent();assert(settles==2&&saves==3);
    Hotkeys::pressed=true;Recorder::OnPresent();assert(saves==4&&!Recorder::Active()); // a further press saves at once
    Recorder::RequestToggle();Recorder::OnPresent();Recorder::RequestToggle();Recorder::OnPresent();assert(starts==6&&settles==3);
    g_startTick=GetTickCount()-kMaxMs-1000;Recorder::OnPresent();assert(saves==4&&Recorder::Active()); // the deadline does not cut the wait
    Recorder::RequestCancel();Recorder::OnPresent();assert(saves==4&&!Recorder::Active()&&!Recorder::Settling()); // cancelled in the wait
    Recorder::OnPresent();assert(saves==4&&starts==6);
    CaptureBurst::running=true;Recorder::RequestToggle();Hotkeys::pressed=true;Recorder::OnPresent();assert(starts==6&&!Recorder::Active()); // not during the frames after one
    CaptureBurst::running=false;Recorder::OnPresent();assert(starts==6);
    std::puts("16 recording request/cancel/wait/auto-save scenarios passed using production bodies");
}
