#pragma once
// The out-of-process helper channel: services that keep game data in the restart host (ts3wrap.exe, 64-bit) instead of
// the game's 32-bit address space. One named section per game, created by Apex Radiance inside the game (x86) before it
// starts the host, opened by the host (x64), so fixed-width fields only (one layout for both builds, checked below).
//   Local\ApexOopHelper-<game pid>            the section: header, request slots, one payload range per slot
//   Local\ApexOopHelperReq-<game pid>         auto-reset event: a slot holds a request
//   Local\ApexOopHelperDone-<game pid>-<n>    auto-reset event per slot: its answer is ready
// The game claims a free slot, writes the request into the slot's payload range, marks it and signals; a host worker
// takes it, runs the service's handler on the same range and marks it done. Nothing here is ever written into a save.
// A call that cannot be served (no host, host gone, a 2 s wait) returns false: the caller keeps its data in the game.
#include <windows.h>
#include <stdint.h>
#include <wchar.h>

static const uint32_t kOopMagic = 0x504F4F58;  // "XOOP"
static const uint32_t kOopVersion = 1;
static const uint32_t kOopSlots = 8;
static const uint32_t kOopPayload = 256u << 10; // per slot (2 MB in all: the view costs the game address space); a service chunks larger requests
static const uint32_t kOopHeader = 4096;        // header + slots; the payload ranges start here

enum OopService : uint32_t { kOopEcho = 0, kOopResRead = 1, kOopStbl = 2, kOopServices = 3 };
enum OopState : int32_t { kOopFree = 0, kOopRequest = 1, kOopTaken = 2, kOopDone = 3, kOopError = 4, kOopWriting = 5 };

struct OopSlot {
    volatile int32_t state;  // OopState
    uint32_t service, op, seq;
    uint32_t inLen, outLen;  // bytes in the slot's payload range: request, then answer
    uint32_t arg;            // a small service argument (no payload needed)
    uint32_t reserved;
};

struct OopChannel {
    uint32_t magic, version, size;
    uint32_t hostPid;            // set by the host once it opened the section
    volatile int32_t hostReady;  // 1: the host's workers run
    volatile uint32_t heartbeat; // the host bumps it every 250 ms (diagnostics: the game watches the host's process handle)
    uint32_t enabled;            // bit per service, written by the game before the host starts
    uint32_t calls[kOopServices];// served calls per service (host side)
    OopSlot slots[kOopSlots];
};
static_assert(sizeof(OopSlot) == 32, "OopSlot: one layout for the x86 game and the x64 host");
static_assert(sizeof(OopChannel) == 10 * 4 + kOopSlots * 32, "OopChannel: one layout for the x86 game and the x64 host");
static_assert(sizeof(OopChannel) <= kOopHeader, "OopChannel: header must fit before the payload ranges");

static const size_t kOopSectionBytes = kOopHeader + static_cast<size_t>(kOopSlots) * kOopPayload;

inline uint8_t* OopPayload(OopChannel* c, uint32_t slot) { return reinterpret_cast<uint8_t*>(c) + kOopHeader + static_cast<size_t>(slot) * kOopPayload; }

inline bool OopName(wchar_t* out, size_t cap, const wchar_t* kind, uint32_t pid, int slot = -1) {
    return (slot < 0 ? _snwprintf_s(out, cap, _TRUNCATE, L"Local\\ApexOopHelper%s-%lu", kind, (unsigned long)pid)
                     : _snwprintf_s(out, cap, _TRUNCATE, L"Local\\ApexOopHelper%s-%lu-%d", kind, (unsigned long)pid, slot)) > 0;
}

inline bool OopValid(const OopChannel* c, size_t mapped) {
    return c && mapped >= kOopSectionBytes && c->magic == kOopMagic && c->version == kOopVersion && c->size == sizeof(OopChannel);
}

inline void OopInit(OopChannel* c, uint32_t enabledMask) {
    ZeroMemory(c, sizeof *c);
    c->magic = kOopMagic, c->version = kOopVersion, c->size = sizeof(OopChannel), c->enabled = enabledMask;
}

// Game side: claim a free slot (-1 when all are busy).
inline int OopClaim(OopChannel* c) {
    for (uint32_t i = 0; i < kOopSlots; i++)
        if (InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(&c->slots[i].state), kOopWriting, kOopFree) == kOopFree) return static_cast<int>(i);
    return -1;
}

// Host side: take one requested slot (-1 when none).
inline int OopTake(OopChannel* c) {
    for (uint32_t i = 0; i < kOopSlots; i++)
        if (InterlockedCompareExchange(reinterpret_cast<volatile LONG*>(&c->slots[i].state), kOopTaken, kOopRequest) == kOopRequest) return static_cast<int>(i);
    return -1;
}

// A service handler (host side): reads `inLen` bytes from `io`, writes the answer over it (at most kOopPayload bytes) and
// sets *outLen. False: an error answer.
typedef bool (*OopHandler)(uint32_t op, uint32_t arg, uint8_t* io, uint32_t inLen, uint32_t* outLen);

// Host side: serve every requested slot; returns how many were served.
inline int OopServe(OopChannel* c, const OopHandler* handlers, HANDLE const* done) {
    int n = 0;
    for (int i; (i = OopTake(c)) >= 0; n++) {
        OopSlot& s = c->slots[i];
        uint32_t outLen = 0;
        const OopHandler h = s.service < kOopServices ? handlers[s.service] : nullptr;
        const bool ok = h && s.inLen <= kOopPayload && h(s.op, s.arg, OopPayload(c, i), s.inLen, &outLen) && outLen <= kOopPayload;
        s.outLen = ok ? outLen : 0;
        if (ok && s.service < kOopServices) InterlockedIncrement(reinterpret_cast<volatile LONG*>(&c->calls[s.service]));
        InterlockedExchange(reinterpret_cast<volatile LONG*>(&s.state), ok ? kOopDone : kOopError);
        SetEvent(done[i]);
    }
    return n;
}

enum OopResult { kOopOk = 0, kOopBusy, kOopFailed, kOopTimeout, kOopHostGone };

// Game side: one call. `in` (inLen bytes) is copied into the slot; the answer (up to outCap bytes) into `out`. A timeout
// or a dead host leaves the slot claimed for good: the caller latches to in process after any result but Ok/Busy/Failed.
inline OopResult OopCall(OopChannel* c, HANDLE req, HANDLE const* done, HANDLE host, uint32_t service, uint32_t op, uint32_t arg,
                         const void* in, uint32_t inLen, void* out, uint32_t outCap, uint32_t* outLen, DWORD timeoutMs = 2000) {
    if (inLen > kOopPayload) return kOopFailed;
    const int i = OopClaim(c);
    if (i < 0) return kOopBusy;
    OopSlot& s = c->slots[i];
    s.service = service, s.op = op, s.arg = arg, s.inLen = inLen, s.outLen = 0, s.seq++;
    if (inLen) memcpy(OopPayload(c, i), in, inLen);
    ResetEvent(done[i]);
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&s.state), kOopRequest);
    SetEvent(req);
    for (int spin = 0; spin < 2000 && s.state != kOopDone && s.state != kOopError; spin++) YieldProcessor();  // ~20 us
    // the done event can still carry the previous call's late signal (set after its state was read): wait until the state says so
    const ULONGLONG until = GetTickCount64() + timeoutMs;
    while (s.state != kOopDone && s.state != kOopError) {
        const ULONGLONG now = GetTickCount64();
        if (now >= until) return kOopTimeout;
        HANDLE w[2] = {done[i], host};
        const DWORD r = WaitForMultipleObjects(host ? 2 : 1, w, FALSE, static_cast<DWORD>(until - now));
        if (r == WAIT_OBJECT_0 + 1 && s.state != kOopDone && s.state != kOopError) return kOopHostGone;
    }
    const bool ok = s.state == kOopDone && s.outLen <= outCap;
    if (ok && outLen) *outLen = s.outLen;
    if (ok && s.outLen) memcpy(out, OopPayload(c, i), s.outLen);
    InterlockedExchange(reinterpret_cast<volatile LONG*>(&s.state), kOopFree);
    return ok ? kOopOk : kOopFailed;
}
