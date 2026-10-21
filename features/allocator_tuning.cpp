// Game allocator tuning (numbered development builds; see allocator_tuning.h).
//
// Part of Apex Radiance. Credits: @loinyx
#include "allocator_tuning.h"
#include "dev_switches.h"
#include "apex_log.h"
#include "memory_patch.h"
#include <windows.h>
#include <atomic>
#include <format>

namespace AllocatorTuning {
namespace {

constexpr uintptr_t kAllocatorGlobal = 0x011CB864, kTrim = 0x004E4F90;
constexpr BYTE kTrimPrologue[8] = {0x83, 0xEC, 0x10, 0x53, 0x55, 0x56, 0x8B, 0xE9}; // sub esp,10h; push ebx/ebp/esi; mov ebp,ecx
constexpr DWORD kPeriodMs = 60000;
constexpr unsigned kPad = 0x100000; // 1 MB kept committed at each core top (the allocator's own pad is 64 KB): fewer recommits

using Trim_t = unsigned(__thiscall*)(void* allocator, unsigned pad);
std::atomic<bool> g_trim{false};
DWORD g_lastTrim = 0;
std::atomic<uint64_t> g_total{0};
std::atomic<unsigned> g_calls{0};

void* Allocator() {
    uint32_t a = 0;
    return MemPatch::ReadBytes(kAllocatorGlobal, &a, sizeof a) ? reinterpret_cast<void*>(static_cast<uintptr_t>(a)) : nullptr;
}

bool SteamBytes() {
    BYTE* base = nullptr;
    size_t size = 0;
    return MemPatch::GetModuleInfo(GetModuleHandleW(nullptr), &base, &size) && reinterpret_cast<uintptr_t>(base) == 0x00400000 &&
           MemPatch::ValidateBytes(reinterpret_cast<LPCVOID>(kTrim), kTrimPrologue, sizeof kTrimPrologue);
}

} // namespace

void Install() {
    const bool trim = DevSwitches::AllocatorTrim(), topDown = DevSwitches::AllocatorTopDown();
    if (!trim && !topDown) return;
    if (!SteamBytes()) {
        LOG_INFO("[AllocatorTuning] Not the Steam 1.67.2 allocator: off");
        return;
    }
    BYTE* const a = static_cast<BYTE*>(Allocator());
    if (!a) {
        LOG_INFO("[AllocatorTuning] Game allocator not created yet: off");
        return;
    }
    if (topDown) {
        BYTE before = 0;
        MemPatch::ReadBytes(reinterpret_cast<uintptr_t>(a + 0x498), &before, 1);
        reinterpret_cast<volatile BYTE*>(a + 0x498)[0] = 1; // a data field of the allocator object (read when it reserves)
        const bool ok = true;
        LOG_INFO(std::format("[AllocatorTuning] Large blocks from the top of the address space: {} (flag was {})", ok ? "on" : "could not be set", before));
    }
    g_trim.store(trim);
    g_lastTrim = GetTickCount();
    if (trim) LOG_INFO("[AllocatorTuning] Native trim of the allocator's free core tops every 60 s and before saves: on");
}

unsigned TrimNow(const char* why) {
    if (!g_trim.load()) return 0;
    void* a = Allocator();
    if (!a) return 0;
    const auto trim = reinterpret_cast<Trim_t>(kTrim);
    const LARGE_INTEGER t0 = [] { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t; }();
    const unsigned bytes = trim(a, kPad);
    LARGE_INTEGER t1, f;
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&f);
    g_total.fetch_add(bytes);
    const unsigned n = g_calls.fetch_add(1) + 1;
    if (bytes >= (1u << 20) || n <= 3 || std::string_view(why) != "periodic")
        LOG_INFO(std::format("[AllocatorTuning] Trim ({}): {:.1f} MB decommitted in {:.2f} ms; {:.1f} MB in {} trims since start", why, bytes / 1048576.0,
                             double(t1.QuadPart - t0.QuadPart) * 1000.0 / double(f.QuadPart), g_total.load() / 1048576.0, n));
    return bytes;
}

void Pump() {
    if (!g_trim.load()) return;
    const DWORD now = GetTickCount();
    if (now - g_lastTrim < kPeriodMs) return;
    g_lastTrim = now;
    TrimNow("periodic");
}

} // namespace AllocatorTuning
