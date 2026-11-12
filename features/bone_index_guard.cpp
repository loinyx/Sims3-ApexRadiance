// Game fix: bone indices past a Sim part's bone list (see bone_index_guard.h).
//
// Part of Apex Radiance. Credits: @loinyx
#include "bone_index_guard.h"
#include "apex_log.h"
#include "memory_patch.h"
#include <windows.h>
#include <intrin.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <format>

namespace BoneIndexGuard {
namespace {

// count: mov eax,[ecx+5C]; sub eax,[ecx+58]; sar eax,2; ret; int3 x6 | get: mov eax,[ecx+58]; mov ecx,[esp+4];
// mov eax,[eax+ecx*4]; ret 4. Unique in Steam 1.67.2 TS3W.exe (0x005C6DF0). EA 1.69: unverified.
constexpr const char* kPattern = "8B 41 5C 2B 41 58 C1 F8 02 C3 CC CC CC CC CC CC 8B 41 58 8B 4C 24 04 8B 04 88 C2 04 00";
constexpr int kGetterOffset = 16;
constexpr unsigned kLoggedCases = 8;

std::atomic<unsigned> g_cases{0};

// In place of the getter (thiscall: ecx = the part, one stack argument, ret 4); __fastcall takes the same registers and
// pops the same argument
uint32_t __fastcall Get(const uint8_t* part, void*, uint32_t index) {
    const auto* first = *reinterpret_cast<const uint32_t* const*>(part + 0x58);
    const auto* last = *reinterpret_cast<const uint32_t* const*>(part + 0x5C);
    const uint32_t count = static_cast<uint32_t>(last - first);
    if (index < count) return first[index];
    const unsigned n = g_cases.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= kLoggedCases)
        LOG_WARNING(std::format("[BoneIndexGuard] A Sim part asked for bone {} of {} (caller {:#010x}): {}{}", index, count,
                                reinterpret_cast<uintptr_t>(_ReturnAddress()), count ? "its first bone used" : "0 used",
                                n == kLoggedCases ? "; further cases are not logged" : ""));
    return count ? first[0] : 0;
}

} // namespace

void Install() {
    BYTE* base = nullptr;
    size_t size = 0;
    if (!MemPatch::GetModuleInfo(GetModuleHandleW(nullptr), &base, &size)) return;
    const uintptr_t hit = MemPatch::ScanPattern(base, size, kPattern);
    if (!hit) {
        LOG_WARNING("[BoneIndexGuard] The Sim part bone getter was not found in this game build: off");
        return;
    }
    const uintptr_t getter = hit + kGetterOffset;
    BYTE bytes[5] = {0xE9};
    const int32_t rel = MemPatch::CalculateRelativeOffset(getter, reinterpret_cast<uintptr_t>(&Get));
    std::memcpy(bytes + 1, &rel, 4);
    if (MemPatch::WriteCodeSuspended(getter, bytes, sizeof bytes))
        LOG_INFO(std::format("[BoneIndexGuard] On at {:#010x}: a bone index past a Sim part's bone list takes its first bone", getter));
    else
        LOG_WARNING(std::format("[BoneIndexGuard] Could not patch the getter at {:#010x}: off", getter));
}

} // namespace BoneIndexGuard
