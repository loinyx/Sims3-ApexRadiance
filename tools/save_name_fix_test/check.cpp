// Save name fix (features/save_name_patch.h) offline checks:
//   1. the sizes and places of the patch agree with the method's offsets; a pattern made here is found once, two are refused;
//   2. --dll <Sims3GameplaySystems.dll>: both patterns match once; --out writes the patched copy, to be decompiled.
//   check.exe [--dll <Sims3GameplaySystems.dll> [--out <file>]]
#include "../../features/save_name_patch.h"
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace SaveNamePatch;

static int g_failed = 0;
static void Check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) g_failed++;
}

static std::vector<uint8_t> Sample(const int* pattern, size_t n, uint8_t token) {
    std::vector<uint8_t> v;
    for (size_t i = 0; i < n; i++) v.push_back(pattern[i] == kAny ? token : static_cast<uint8_t>(pattern[i]));
    return v;
}

int main(int argc, char** argv) {
    std::string dll, out;
    for (int i = 1; i + 1 < argc; i += 2) (std::string(argv[i]) == "--dll" ? dll : out) = argv[i + 1];

    Check(kStartupSize == 0x151 - 0x110 && kBranch == 0x11C - 0x110 && kDead == 0x123 - 0x110 && kDead + kDeadSize == 0x14F - 0x110, "the places agree with the method's offsets");
    const auto startup = Sample(kStartup, kStartupSize, 0x11), tokens = Sample(kTokens, kTokensSize, 0x22);
    std::vector<uint8_t> blob(100, 0x28);
    blob.insert(blob.end(), startup.begin(), startup.end());
    blob.insert(blob.end(), 50, 0x00);
    const size_t tokensAt = blob.size();
    blob.insert(blob.end(), tokens.begin(), tokens.end());
    Check(FindOnly(blob.data(), blob.size(), kStartupSize, 0x06, IsStartup) == 100 && FindOnly(blob.data(), blob.size(), kTokensSize, 0x28, IsTokens) == tokensAt, "each is found");
    auto twice = blob;
    twice.insert(twice.end(), tokens.begin(), tokens.end());
    Check(FindOnly(twice.data(), twice.size(), kTokensSize, 0x28, IsTokens) == twice.size(), "two of them are refused");
    auto other = tokens;
    other[kOptions2] ^= 1;
    Check(!IsTokens(other.data()), "tokens that disagree are refused");
    uint8_t body[kDeadSize];
    Build(tokens.data(), body);
    Check(body[0] == 0x28 && body[5] == 0x6F && body[10] == 0x06 && body[11] == 0x6F && body[16] == 0x38 && body[kDeadSize - 1] == 0x00, "the new block");
    int32_t rel = 0;
    std::memcpy(&rel, body + 17, 4);
    Check(0x123 + 21 + rel == 0x1AE && 0x120 + kNewBranch == 0x123, "its branches land where they should");

    if (!dll.empty()) {
        std::ifstream f(dll, std::ios::binary);
        std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), {});
        const size_t at = FindOnly(d.data(), d.size(), kStartupSize, 0x06, IsStartup), from = FindOnly(d.data(), d.size(), kTokensSize, 0x28, IsTokens);
        Check(!d.empty() && at < d.size() && from < d.size(), "the assembly has each once");
        if (!g_failed) {
            std::printf("     startup 0x%zX, tokens 0x%zX\n", at, from);
            Build(d.data() + from, body);
            std::memcpy(d.data() + at + kDead, body, kDeadSize);
            std::memcpy(d.data() + at + kBranch, &kNewBranch, 4);
            if (!out.empty()) std::ofstream(out, std::ios::binary).write(reinterpret_cast<const char*>(d.data()), static_cast<std::streamsize>(d.size()));
        }
    }
    std::printf(g_failed ? "%d FAILED\n" : "ALL PASSED\n", g_failed);
    return g_failed ? 1 : 0;
}
