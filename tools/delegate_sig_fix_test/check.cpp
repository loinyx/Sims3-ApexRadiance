// Delegate signature fix (features/delegate_sig_fix.cpp) offline checks, x86 only:
//   1. the stub, copied from delegate_sig_fix.cpp, replayed inside the game's own check sequence: the crash's memory gives
//      false with no fault, (Object[]) still gives true, any other single parameter still gives false;
//   2. --exe <TS3W.exe>: the pattern matches once and its call leads to mono_type_get_type.
//   check.exe [--exe <TS3W.exe>]
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>

extern "C" uint32_t __cdecl FakeTypeGetType(const uint8_t* t) { return t[6]; } // movzx eax, byte [eax+6]
uintptr_t g_typeGetType = reinterpret_cast<uintptr_t>(&FakeTypeGetType);

// Same as delegate_sig_fix.cpp
__declspec(naked) void ParamTypeStub() {
    __asm {
        cmp word ptr [esi + 4], 1
        jne none
        jmp dword ptr [g_typeGetType]
    none:
        xor eax, eax
        ret
    }
}

// The game's 0x00D70FB2..0x00D70FC9 with the call retargeted: edi = params[0]; push; call; add esp,4; then its own tests
int GameCheck(const uint8_t* sig) {
    int r;
    __asm {
        push esi
        push edi
        mov esi, sig
        mov edi, [esi + 0x10]
        push edi
        call ParamTypeStub
        add esp, 4
        xor ecx, ecx
        cmp word ptr [esi + 4], 1
        jne out_
        cmp eax, 0x1D
        jne out_
        mov ecx, 1
    out_:
        mov r, ecx
        pop edi
        pop esi
    }
    return r;
}

int ScanExe(const char* path) {
    std::ifstream f(path, std::ios::binary);
    const std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), {});
    if (d.size() < 0x400) { printf("cannot read %s\n", path); return 1; }
    // "8B 7E 10 57 E8 ?? ?? ?? ?? 83 C4 04 66 83 7E 04 01 75 ?? 83 F8 1D 75" (-1 = any byte)
    const int pat[] = {0x8B, 0x7E, 0x10, 0x57, 0xE8, -1, -1, -1, -1, 0x83, 0xC4, 0x04, 0x66, 0x83, 0x7E, 0x04, 0x01, 0x75, -1, 0x83, 0xF8, 0x1D, 0x75};
    const uint8_t helper[] = {0x8B, 0x44, 0x24, 0x04, 0x0F, 0xB6, 0x40, 0x06, 0xC3};
    uint32_t pe, imageBase;
    std::memcpy(&pe, &d[0x3C], 4);
    std::memcpy(&imageBase, &d[pe + 52], 4);
    uint16_t nsec, opt;
    std::memcpy(&nsec, &d[pe + 6], 2);
    std::memcpy(&opt, &d[pe + 20], 2);
    auto section = [&](int i, uint32_t& rva, uint32_t& raw, uint32_t& off) {
        const size_t s = pe + 24 + opt + i * 40;
        std::memcpy(&rva, &d[s + 12], 4); std::memcpy(&raw, &d[s + 16], 4); std::memcpy(&off, &d[s + 20], 4);
    };
    auto toVa = [&](size_t o) -> uint32_t {
        for (int i = 0; i < nsec; ++i) { uint32_t rva, raw, off; section(i, rva, raw, off); if (o >= off && o < off + raw) return imageBase + rva + uint32_t(o - off); }
        return 0;
    };
    auto toOff = [&](uint32_t va) -> size_t {
        for (int i = 0; i < nsec; ++i) { uint32_t rva, raw, off; section(i, rva, raw, off); if (va >= imageBase + rva && va < imageBase + rva + raw) return off + (va - imageBase - rva); }
        return 0;
    };
    int hits = 0, bad = 0;
    for (size_t i = 0; i + std::size(pat) <= d.size(); ++i) {
        size_t k = 0;
        while (k < std::size(pat) && (pat[k] < 0 || d[i + k] == pat[k])) ++k;
        if (k != std::size(pat)) continue;
        ++hits;
        int32_t rel;
        std::memcpy(&rel, &d[i + 5], 4);
        const uint32_t call = toVa(i) + 4, target = call + 5 + rel;
        const size_t t = toOff(target);
        const bool ok = t && t + sizeof helper <= d.size() && !std::memcmp(&d[t], helper, sizeof helper);
        bad += !ok;
        printf("%s: call at 0x%08X -> 0x%08X %s\n", path, call, target, ok ? "(mono_type_get_type)" : "(NOT the type helper)");
    }
    printf("%s: %d match(es)\n", path, hits);
    return hits == 1 && !bad ? 0 : 1;
}

int main(int argc, char** argv) {
    int fails = 0;
    // the crash: a 16-byte instance void() signature followed by a signature whose first dword is 1
    uint8_t pool[32] = {1, 0, 0, 0, 0, 0, 0xff, 0xff, 0, 0, 0, 0, 0xf0, 0xc8, 0x0e, 0x01, 1, 0, 0, 0, 3, 0, 0xff, 0xff};
    fails += GameCheck(pool) != 0;
    uint8_t sz[8] = {0, 0, 0, 0, 0, 0, 0x1D, 0}, cls[8] = {0, 0, 0, 0, 0, 0, 0x12, 0};
    uint8_t one[20] = {1, 0, 0, 0, 1, 0, 0, 0};
    const uint8_t* p = sz;
    std::memcpy(one + 16, &p, 4);
    fails += GameCheck(one) != 1; // (Object[]) -> true, as before
    p = cls;
    std::memcpy(one + 16, &p, 4);
    fails += GameCheck(one) != 0; // (SomeClass) -> false, as before
    printf(fails ? "stub: FAIL %d\n" : "stub: ok\n", fails);
    for (int i = 1; i + 1 < argc; ++i)
        if (!std::strcmp(argv[i], "--exe")) fails += ScanExe(argv[i + 1]);
    return fails ? 1 : 0;
}
