// Game fix: zero-parameter delegates no longer read past their Mono signature (see delegate_sig_fix.h).
//
// Part of Apex Radiance. Credits: @loinyx
#include "delegate_sig_fix.h"
#include "apex_log.h"
#include "memory_patch.h"
#include <windows.h>
#include <cstring>
#include <format>

namespace DelegateSigFix {
namespace {

// mov edi,[esi+10]; push edi; call mono_type_get_type; add esp,4; cmp word [esi+4],1; jne; cmp eax,1D; jne
// Unique in Steam 1.67.2 TS3W.exe (0x00D70FB2). EA 1.69.47: unverified (no copy of that build was scanned).
constexpr const char* kPattern = "8B 7E 10 57 E8 ?? ?? ?? ?? 83 C4 04 66 83 7E 04 01 75 ?? 83 F8 1D 75";
constexpr int kCallOffset = 4;
// mono_type_get_type: mov eax,[esp+4]; movzx eax,byte [eax+6]; ret (Steam 1.67.2: 0x00F4AC60)
constexpr BYTE kTypeGetType[] = {0x8B, 0x44, 0x24, 0x04, 0x0F, 0xB6, 0x40, 0x06, 0xC3};

uintptr_t g_typeGetType = 0; // the call's original target

// Replaces "call mono_type_get_type" (cdecl, the caller pops); esi = the Invoke's MonoMethodSignature (param_count at +4)
__declspec(naked) void ParamTypeStub() {
    __asm {
        cmp word ptr [esi + 4], 1
        jne none
        jmp dword ptr [g_typeGetType]
    none:
        xor eax, eax // not SZARRAY; the caller's own param_count test then returns false, as before
        ret
    }
}

} // namespace

void Install() {
    BYTE* base = nullptr;
    size_t size = 0;
    if (!MemPatch::GetModuleInfo(GetModuleHandleW(nullptr), &base, &size)) return;
    const uintptr_t hit = MemPatch::ScanPattern(base, size, kPattern);
    if (!hit) {
        LOG_WARNING("[DelegateSigFix] Delegate check not found in this game build: off");
        return;
    }
    const uintptr_t call = hit + kCallOffset;
    int32_t rel = 0;
    if (!MemPatch::ReadBytes(call + 1, &rel, sizeof rel)) return;
    const uintptr_t target = call + 5 + rel;
    // another module may own the call already (its target is no longer mono_type_get_type): leave it alone
    if (!MemPatch::ValidateBytes(reinterpret_cast<LPCVOID>(target), kTypeGetType, sizeof kTypeGetType)) {
        LOG_WARNING(std::format("[DelegateSigFix] The call at {:#010x} leads to {:#010x}, not the expected type helper: off", call, target));
        return;
    }
    g_typeGetType = target; // before the CALL can reach the stub
    BYTE bytes[5] = {0xE8};
    const int32_t stubRel = MemPatch::CalculateRelativeOffset(call, reinterpret_cast<uintptr_t>(&ParamTypeStub));
    std::memcpy(bytes + 1, &stubRel, 4);
    if (MemPatch::WriteCodeSuspended(call, bytes, sizeof bytes))
        LOG_INFO(std::format("[DelegateSigFix] On at {:#010x}: delegates with no parameters no longer read past their signature", call));
    else
        LOG_WARNING(std::format("[DelegateSigFix] Could not patch the call at {:#010x}: off", call));
}

} // namespace DelegateSigFix
