// Bone index guard (features/bone_index_guard.cpp) offline checks, x86 only:
//   1. Get called as the game calls the getter (thiscall, one argument): a valid index gives its entry, an index past
//      the list gives the first bone, an empty list gives 0, and the stack is balanced;
//   2. --exe <TS3W.exe>: the pattern matches exactly once.
//   check.exe [--exe <TS3W.exe>]
#include "../../features/bone_index_guard.cpp"
#include <cstdio>
#include <fstream>
#include <iterator>
#include <vector>

namespace ApexLog { void Write(Level, const std::string&, const std::source_location&) noexcept {} }
namespace MemPatch {
bool GetModuleInfo(HMODULE, BYTE**, size_t*) { return false; }
uintptr_t ScanPattern(BYTE*, size_t, const char*) { return 0; }
int32_t CalculateRelativeOffset(uintptr_t, uintptr_t, size_t) { return 0; }
bool WriteCodeSuspended(uintptr_t, const BYTE*, size_t) { return false; }
}

static int g_fail = 0;
static void Expect(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    g_fail += !ok;
}

// The game's part: the bone list vector at +0x58 / +0x5C
struct Part {
    uint8_t pad[0x58] = {};
    const uint32_t* first;
    const uint32_t* last;
};

static uint32_t CallAsGame(const Part& p, uint32_t index) {
    using Getter = uint32_t(__thiscall*)(const void*, uint32_t);
    const auto get = reinterpret_cast<Getter>(&BoneIndexGuard::Get);
    uint32_t espBefore, espAfter, r;
    __asm mov espBefore, esp
    r = get(&p, index);
    __asm mov espAfter, esp
    Expect(espBefore == espAfter, "stack balanced after the call");
    return r;
}

int main(int argc, char** argv) {
    static_assert(offsetof(Part, first) == 0x58 && offsetof(Part, last) == 0x5C);
    const uint32_t bones[3] = {0x11, 0x22, 0x33};
    Part p;
    p.first = bones, p.last = bones + 3;
    Expect(CallAsGame(p, 2) == 0x33, "a valid index gives its entry");
    Expect(CallAsGame(p, 109) == 0x11, "an index past the list gives the first bone");
    p.last = bones + 1;
    Expect(CallAsGame(p, 109) == 0x11, "the crash's case (109 of 1) gives the only bone");
    p.last = bones;
    Expect(CallAsGame(p, 0) == 0, "an empty list gives 0");
    Expect(BoneIndexGuard::g_cases.load() == 3, "three cases counted");

    if (argc == 3 && std::strcmp(argv[1], "--exe") == 0) {
        std::ifstream f(argv[2], std::ios::binary);
        const std::vector<uint8_t> exe((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        const uint8_t pat[] = {0x8B, 0x41, 0x5C, 0x2B, 0x41, 0x58, 0xC1, 0xF8, 0x02, 0xC3, 0xCC, 0xCC, 0xCC, 0xCC, 0xCC,
                               0xCC, 0x8B, 0x41, 0x58, 0x8B, 0x4C, 0x24, 0x04, 0x8B, 0x04, 0x88, 0xC2, 0x04, 0x00};
        int matches = 0;
        for (size_t i = 0; i + sizeof pat <= exe.size(); i++) matches += std::memcmp(exe.data() + i, pat, sizeof pat) == 0;
        std::printf("pattern matches: %d\n", matches);
        Expect(matches == 1, "the pattern matches once in the executable");
    }
    std::printf(g_fail ? "%d FAILED\n" : "all passed\n", g_fail);
    return g_fail ? 1 : 0;
}
