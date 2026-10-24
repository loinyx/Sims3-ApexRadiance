#define APEX_LOADING_DIAGNOSTICS
#include "../../framework/transition_calls.cpp"
#include <cassert>
#include <iostream>
#include <vector>

uint32_t g_exeTimestamp = 0;
std::vector<std::string> logLines;
namespace ApexLog {
void Write(Level, const std::string& text, const std::source_location&) noexcept { logLines.push_back(text); }
}
std::optional<uintptr_t> GameAddress::Resolve() const { return std::nullopt; }
namespace DetourBatch { bool InstallHooks(const std::vector<Hook>&) { return false; } }
unsigned routes = 0, thumbs = 0, heights = 0;
uintptr_t received = 0;
void __cdecl OriginalRouting() { ++routes; }
void __cdecl OriginalThumbnail(uintptr_t callback) {
    ++thumbs;
    received = callback;
    TransitionCalls::Routing(); // nested native work remains separately matched
}
void __cdecl OriginalHeight(uintptr_t value) { ++heights; received = value; }
bool Contains(const char* text) {
    for (const auto& line : logLines) if (line.find(text) != std::string::npos) return true;
    return false;
}
int main() {
    using namespace TransitionCalls;
    Start(); // unsupported executable: no patch attempt and no addresses read
    assert(logLines.empty());
    thumbnails = OriginalThumbnail;
    routing = OriginalRouting;
    heightMap = OriginalHeight;
    Thumbnails(0x12345678);
    assert(thumbs == 1 && routes == 1 && received == 0x12345678);
    HeightMap(1);
    assert(heights == 1 && received == 1);
    assert(count == 6 && !lost.load());
    assert(events[0].kind == Kind::Thumbnails && events[0].active);
    assert(events[1].kind == Kind::Routing && events[1].active);
    assert(events[2].id == events[1].id && !events[2].active);
    assert(events[3].id == events[0].id && !events[3].active);
    Pump();
    assert(!count && logLines.size() == 6);
    assert(Contains("UpdateDirtyLotThumbnails id 1 entered"));
    assert(Contains("UpdateDirtyLotThumbnails id 1 returned"));
    assert(Contains("argument 12345678"));
    assert(Contains("BuildRoutingData id 2 returned"));
    AcquireSRWLockExclusive(&lock);
    const auto droppedId = Begin(Kind::Routing, 0); // producers cannot block behind the pump
    ReleaseSRWLockExclusive(&lock);
    assert(!droppedId && lost.load() == 1);
    for (unsigned i = 0; i < 100; ++i) Routing();
    assert(count == events.size() && lost.load() > 1);
    for (const auto& slot : calls.slots) assert(!slot.active);
    std::cout << "Transition native wrappers: argument forwarding, nested calls, return pairing, pump output, lock contention and queue overflow passed\n";
}
