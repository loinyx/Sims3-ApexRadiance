// The restart host's out-of-process services: the host side of oop_channel.h.
#include "oop_host.h"

// Echo: the answer is the request (the channel's own check and the game side's first call).
bool SvcEcho(uint32_t, uint32_t, uint8_t*, uint32_t inLen, uint32_t* outLen) { return *outLen = inLen, true; }

namespace OopHost {
namespace {

const OopHandler kHandlers[kOopServices] = {SvcEcho, nullptr /* resread: svc_resread.cpp */, nullptr /* stbl: svc_stbl.cpp */};
OopChannel* g_chan = nullptr;
HANDLE g_req = nullptr, g_done[kOopSlots] = {};

DWORD WINAPI Worker(void*) {
    for (;;) {
        WaitForSingleObject(g_req, 250);
        OopServe(g_chan, kHandlers, g_done);
    }
}

DWORD WINAPI Heartbeat(void*) {
    for (;;) InterlockedIncrement(reinterpret_cast<volatile LONG*>(&g_chan->heartbeat)), Sleep(250);
}

} // namespace

bool Start(DWORD pid) {
    wchar_t name[96];
    HANDLE map = OopName(name, 96, L"", pid) ? OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, name) : nullptr;
    auto* c = map ? static_cast<OopChannel*>(MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, 0)) : nullptr;
    MEMORY_BASIC_INFORMATION mbi{};
    if (!c || !VirtualQuery(c, &mbi, sizeof mbi) || !OopValid(c, mbi.RegionSize)) return false;
    g_req = OopName(name, 96, L"Req", pid) ? OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, name) : nullptr;
    for (int i = 0; i < static_cast<int>(kOopSlots) && g_req; i++)
        if (!OopName(name, 96, L"Done", pid, i) || !(g_done[i] = OpenEventW(SYNCHRONIZE | EVENT_MODIFY_STATE, FALSE, name))) return false;
    if (!g_req) return false;
    g_chan = c;
    for (int i = 0; i < 2; i++)  // two workers: a long request (a table upload) never holds up a lookup
        if (HANDLE t = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr)) CloseHandle(t);
    if (HANDLE t = CreateThread(nullptr, 0, Heartbeat, nullptr, 0, nullptr)) CloseHandle(t);
    c->hostPid = GetCurrentProcessId();
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&c->hostReady), 1);
    return true;
}

} // namespace OopHost
