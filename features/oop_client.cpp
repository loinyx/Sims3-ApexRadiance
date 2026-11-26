// Out-of-process helper, the game side (see oop_client.h).
//
// Part of Apex Radiance. Credits: @loinyx
#include "oop_client.h"
#include "apex_config.h"
#include "apex_log.h"
#include <mutex>
#include <string>

namespace OopClient {
namespace {

std::mutex g_mu;
OopChannel* g_chan = nullptr;
HANDLE g_req = nullptr, g_done[kOopSlots] = {}, g_host = nullptr;
volatile LONG g_lost = 0;

bool CreateSection(uint32_t mask) {
    wchar_t name[96];
    const DWORD pid = GetCurrentProcessId();
    HANDLE map = OopName(name, 96, L"", pid) ? CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, static_cast<DWORD>(kOopSectionBytes), name) : nullptr;
    // shortcut: the 2 MB view stays mapped in the game for the session; map per call only if a service ever needs it gone
    auto* c = map ? static_cast<OopChannel*>(MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, kOopSectionBytes)) : nullptr;
    HANDLE req = c && OopName(name, 96, L"Req", pid) ? CreateEventW(nullptr, FALSE, FALSE, name) : nullptr;
    bool ok = req != nullptr;
    for (int i = 0; ok && i < static_cast<int>(kOopSlots); i++) ok = OopName(name, 96, L"Done", pid, i) && (g_done[i] = CreateEventW(nullptr, FALSE, FALSE, name));
    if (!ok) {
        LOG_WARNING("[OopHelper] Helper section not created (error " + std::to_string(GetLastError()) + "): services stay in the game");
        if (c) UnmapViewOfFile(c);
        if (map) CloseHandle(map);
        return false;
    }
    OopInit(c, mask);
    g_chan = c, g_req = req;
    return true;
}

void Lose(const char* why) {
    if (InterlockedExchange(&g_lost, 1)) return;
    LOG_WARNING(std::string("[OopHelper] Host lost (") + why + "): every service stays in the game for this session");
}

} // namespace

bool Enable(OopService service, const char* name) {
    if (!ApexConfig::GetUi().restartHost) {
        LOG_WARNING(std::string("[OopHelper] ") + name + " is on but the restart host is off: staying in process. Turn on the restart host ([ui] restart_host) so " +
                    name + " can move out of the game");
        return false;
    }
    std::lock_guard<std::mutex> lock(g_mu);
    if (!g_chan) return CreateSection(1u << service);
    InterlockedOr(reinterpret_cast<volatile LONG*>(&g_chan->enabled), static_cast<LONG>(1u << service));
    return true;
}

bool WaitReady(DWORD ms) {
    if (!g_chan || g_lost) return false;
    for (DWORD waited = 0; !g_chan->hostReady; waited += 10) {
        if (waited >= ms) return false;
        Sleep(10);
    }
    std::lock_guard<std::mutex> lock(g_mu);
    if (!g_host && !(g_host = OpenProcess(SYNCHRONIZE, FALSE, g_chan->hostPid))) return Lose("its process could not be opened"), false;
    return true;
}

OopResult Call(OopService service, uint32_t op, uint32_t arg, const void* in, uint32_t inLen, void* out, uint32_t outCap, uint32_t* outLen) {
    if (!g_host || g_lost) return kOopHostGone;
    const OopResult r = OopCall(g_chan, g_req, g_done, g_host, service, op, arg, in, inLen, out, outCap, outLen);
    if (r == kOopTimeout) Lose("no answer within 2 s");
    if (r == kOopHostGone) Lose("its process ended");
    return r;
}

Host State() {
    if (g_lost) return Host::Lost;
    if (!g_chan) return Host::Off;
    return g_host ? Host::Ready : Host::Starting;
}

} // namespace OopClient
