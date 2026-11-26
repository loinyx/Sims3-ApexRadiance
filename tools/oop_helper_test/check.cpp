// Out-of-process helper channel checks (native/ts3wrap/oop_channel.h, oop_host.cpp). Built twice by run.ps1:
//   x86 (the game's side): creates the section and events like features/oop_client.cpp, starts the x64 host and calls it
//   x64 (OOP_TEST_HOST):   the restart host's service part, OopHost::Start(<client pid>), serving until the client ends
#include "../../native/ts3wrap/oop_channel.h"
#include <cstdio>
#include <cstring>
#include <chrono>

#ifdef OOP_TEST_HOST
#include "../../native/ts3wrap/oop_host.h"
int wmain(int argc, wchar_t** argv) {
    const DWORD pid = argc > 1 ? wcstoul(argv[1], nullptr, 10) : 0;
    HANDLE game = pid ? OpenProcess(SYNCHRONIZE, FALSE, pid) : nullptr;
    if (!game || !OopHost::Start(pid)) return 3;
    WaitForSingleObject(game, INFINITE);
    return 0;
}
#else
static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) return std::printf("usage: oop_client <oop_host.exe>\n"), 2;
    // pure parts
    static uint8_t buf[sizeof(OopChannel)];
    auto* p = reinterpret_cast<OopChannel*>(buf);
    OopInit(p, 4);
    CHECK(OopValid(p, kOopSectionBytes) && !OopValid(p, kOopSectionBytes - 1));
    for (uint32_t i = 0; i < kOopSlots; i++) CHECK(OopClaim(p) == static_cast<int>(i));
    CHECK(OopClaim(p) == -1);
    CHECK(OopTake(p) == -1);  // claimed for writing is not a request
    p->slots[3].state = kOopRequest;
    CHECK(OopTake(p) == 3 && p->slots[3].state == kOopTaken);

    // the section, as the game creates it
    wchar_t name[96];
    const DWORD pid = GetCurrentProcessId();
    OopName(name, 96, L"", pid);
    HANDLE map = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, static_cast<DWORD>(kOopSectionBytes), name);
    auto* c = static_cast<OopChannel*>(MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, kOopSectionBytes));
    OopName(name, 96, L"Req", pid);
    HANDLE req = CreateEventW(nullptr, FALSE, FALSE, name), done[kOopSlots];
    for (int i = 0; i < static_cast<int>(kOopSlots); i++) OopName(name, 96, L"Done", pid, i), done[i] = CreateEventW(nullptr, FALSE, FALSE, name);
    OopInit(c, 1);

    uint32_t outLen = 0;
    char out[64];
    CHECK(OopCall(c, req, done, nullptr, kOopEcho, 0, 0, "x", 1, out, sizeof out, &outLen, 50) == kOopTimeout);  // no host yet
    c->slots[0].state = kOopFree;  // (the timed-out slot stays claimed: freed here for the rest of the checks)

    wchar_t line[MAX_PATH + 32];
    swprintf_s(line, L"\"%s\" %lu", argv[1], pid);
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    CHECK(CreateProcessW(argv[1], line, nullptr, nullptr, FALSE, 0, nullptr, nullptr, &si, &pi));
    for (int i = 0; i < 500 && !c->hostReady; i++) Sleep(10);
    CHECK(c->hostReady == 1 && c->hostPid == pi.dwProcessId);
    CHECK(OopCall(c, req, done, pi.hProcess, kOopEcho, 0, 0, "hello", 5, out, sizeof out, &outLen) == kOopOk && outLen == 5 && !memcmp(out, "hello", 5));
    CHECK(OopCall(c, req, done, pi.hProcess, kOopEcho, 0, 0, "hello", 5, out, 2, &outLen) == kOopFailed);  // answer too big
    CHECK(OopCall(c, req, done, pi.hProcess, kOopStbl + 5, 0, 0, "x", 1, out, sizeof out, &outLen) == kOopFailed);  // no such service
    // a full slot range round trip
    static uint8_t big[kOopPayload], back[kOopPayload];
    for (uint32_t i = 0; i < kOopPayload; i++) big[i] = static_cast<uint8_t>(i * 7);
    CHECK(OopCall(c, req, done, pi.hProcess, kOopEcho, 0, 0, big, kOopPayload, back, kOopPayload, &outLen) == kOopOk && outLen == kOopPayload && !memcmp(big, back, kOopPayload));
    CHECK(OopCall(c, req, done, pi.hProcess, kOopEcho, 0, 0, big, kOopPayload + 1, back, kOopPayload, &outLen) == kOopFailed);
    for (uint32_t i = 0; i < kOopSlots; i++) CHECK(c->slots[i].state == kOopFree);
    // latency
    const int n = 20000;
    const auto t0 = std::chrono::steady_clock::now();
    int ok = 0;
    for (int i = 0; i < n; i++) ok += OopCall(c, req, done, pi.hProcess, kOopEcho, 0, 0, "0123456789abcdef", 16, out, sizeof out, &outLen) == kOopOk;
    const double us = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - t0).count() / n;
    CHECK(ok == n);
    std::printf("round trip: %.2f us per call (%d calls), host served %u\n", us, n, c->calls[kOopEcho]);
    // the host ends: the next call says so at once
    TerminateProcess(pi.hProcess, 9);
    WaitForSingleObject(pi.hProcess, 5000);
    const auto t1 = std::chrono::steady_clock::now();
    CHECK(OopCall(c, req, done, pi.hProcess, kOopEcho, 0, 0, "x", 1, out, sizeof out, &outLen) == kOopHostGone);
    CHECK(std::chrono::steady_clock::now() - t1 < std::chrono::milliseconds(500));
    std::printf(g_fail ? "%d check(s) failed\n" : "all oop helper checks passed\n", g_fail);
    return g_fail ? 1 : 0;
}
#endif
