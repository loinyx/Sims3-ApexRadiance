// The restart host's out-of-process services: the host side of oop_channel.h.
#include "oop_host.h"

// Echo: the answer is the request (the channel's own check and the game side's first call).
bool SvcEcho(uint32_t, uint32_t, uint8_t*, uint32_t inLen, uint32_t* outLen) { return *outLen = inLen, true; }

namespace OopHost {
namespace {

OopHandler g_handlers[kOopServices] = {SvcEcho};  // 1 resread: svc_resread.cpp, 2 stbl: svc_stbl.cpp, 16-31: plugins (LoadPlugins)
OopChannel* g_chan = nullptr;
HANDLE g_req = nullptr, g_done[kOopSlots] = {};

DWORD WINAPI Worker(void*) {
    for (;;) {
        WaitForSingleObject(g_req, 250);
        OopServe(g_chan, g_handlers, g_done);
    }
}

DWORD WINAPI Heartbeat(void*) {
    for (;;) InterlockedIncrement(reinterpret_cast<volatile LONG*>(&g_chan->heartbeat)), Sleep(250);
}

void PluginLog(const wchar_t* line) { OutputDebugStringW(line); }

// Loads the DLLs the game listed ([oop] plugins, oop_plugin.h) from this executable's folder and from nowhere else.
// Every listed entry gets a state; only a loaded plugin gets a handler.
void LoadPlugins(OopChannel* c, DWORD pid) {
    static const ApexOopHostApi api{kApexOopAbi, kOopVersion, kOopPayload, pid, PluginLog};
    wchar_t path[MAX_PATH + kOopPluginName];
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    wchar_t* slash = n && n < MAX_PATH ? wcsrchr(path, L'\\') : nullptr;
    for (uint32_t i = 0; slash && i < kOopPlugins; i++) {
        OopPlugin& p = c->plugins[i];
        wchar_t name[kOopPluginName];  // a copy, terminated: the block is shared memory
        memcpy(name, p.name, sizeof name);
        name[kOopPluginName - 1] = 0;
        if (!name[0]) continue;
        p.serviceId = kOopPluginBase + i;
        if (wcspbrk(name, L"\\/:")) {
            p.state = kApexOopRefused;
            continue;
        }
        wcscpy_s(slash + 1, kOopPluginName, name);
        HMODULE dll = LoadLibraryExW(path, nullptr, LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        const auto init = dll ? reinterpret_cast<ApexOopPluginInitFn>(GetProcAddress(dll, "ApexOopPluginInit")) : nullptr;
        ApexOopPluginInfo info{};
        p.abi = init ? init(&api, &info) : 0;
        p.tag = info.tag, p.pluginVersion = info.pluginVersion;
        p.state = !init ? kApexOopMissing : !p.abi || !info.handler ? kApexOopInitFailed : p.abi >> 16 != kApexOopAbi >> 16 ? kApexOopWrongAbi : kApexOopLoaded;
        if (p.state == kApexOopLoaded) g_handlers[p.serviceId] = info.handler;
        else if (dll) FreeLibrary(dll);
    }
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
    LoadPlugins(c, pid);  // before the workers and hostReady: a caller that sees the host ready sees every plugin's state
    for (int i = 0; i < 2; i++)  // two workers: a long request (a table upload) never holds up a lookup
        if (HANDLE t = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr)) CloseHandle(t);
    if (HANDLE t = CreateThread(nullptr, 0, Heartbeat, nullptr, 0, nullptr)) CloseHandle(t);
    c->hostPid = GetCurrentProcessId();
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&c->hostReady), 1);
    return true;
}

} // namespace OopHost
