// Shaders that used to be refused and now take their patch, on hand-built bytecode (no game shader is kept here):
// a vertex shader that writes the free TEXCOORDk itself (AddScreenPosVs) and the indoor diffuse chain in the two shapes of
// the saved refusals (PatchIndoorBasis), with the cases that must still fail closed.
#include "shader_patches.h"
#include <cstdio>
#include <vector>

using ShaderPatches::AddScreenPosVs;
using ShaderPatches::IndoorBasisPatch;
using ShaderPatches::PatchIndoorBasis;

namespace {

enum : DWORD { kTemp = 0, kInput = 1, kConst = 2, kRast = 4, kOut = 6, kColorOut = 8, kSampler = 10 };
enum : DWORD { kMov = 1, kMad = 4, kMul = 5, kDp3 = 8, kDcl = 0x1F, kNrm = 0x24, kTexld = 0x42 };
constexpr DWORD kXYZW = 0xE4, kXXXX = 0x00, kYYYY = 0x55, kZZZZ = 0xAA, kWWWW = 0xFF;

DWORD Op(DWORD op, DWORD len) { return op | (len << 24); }
DWORD Reg(DWORD type, DWORD num) { return 0x80000000u | ((type & 7) << 28) | (((type >> 3) & 3) << 11) | num; }
DWORD Dst(DWORD type, DWORD num, DWORD mask = 0xF, bool sat = false) { return Reg(type, num) | (mask << 16) | (sat ? 0x00100000u : 0u); }
DWORD Src(DWORD type, DWORD num, DWORD swz = kXYZW) { return Reg(type, num) | (swz << 16); }
void Add(std::vector<DWORD>& t, std::initializer_list<DWORD> v) { t.insert(t.end(), v); }

int g_failed = 0, g_passed = 0;
void Check(bool ok, const char* what) {
    (ok ? g_passed : g_failed)++;
    if (!ok) std::printf("FAILED: %s\n", what);
}

std::vector<DWORD> VertexShader(DWORD version, bool writesTexcoord7) {
    std::vector<DWORD> t = {version};
    Add(t, {Op(kDcl, 2), 0x80000000u, Dst(kInput, 0)});
    Add(t, {Op(kMov, 2), Dst(kRast, 0), Src(kInput, 0)});
    if (writesTexcoord7) Add(t, {Op(kMov, 2), Dst(kOut, 7, 0x3), Src(kInput, 0)});
    t.push_back(0x0000FFFFu);
    return t;
}

// ps_3_0 up to the normal and the room map read; the diffuse chain goes after it, the colour write closes it
std::vector<DWORD> PixelHead() {
    std::vector<DWORD> t = {0xFFFF0300u};
    Add(t, {Op(kDcl, 2), 0x80000005u, Dst(kInput, 1)});
    Add(t, {Op(kDcl, 2), 0x80010005u, Dst(kInput, 3)});
    Add(t, {Op(kDcl, 2), 0x90000000u, Dst(kSampler, 1)});
    Add(t, {Op(kNrm, 2), Dst(kTemp, 2, 0x7), Src(kInput, 3)});
    Add(t, {Op(kTexld, 3), Dst(kTemp, 1), Src(kInput, 1), Src(kSampler, 1)});
    return t;
}
void Close(std::vector<DWORD>& t, DWORD result) {
    Add(t, {Op(kMov, 2), Dst(kColorOut, 0), Src(kTemp, result)});
    t.push_back(0x0000FFFFu);
}
bool Reads(const std::vector<DWORD>& t, DWORD constant) {
    for (DWORD d : t)
        if ((d & 0xF00007FFu) == (0xA0000000u | constant)) return true; // a constant register token
    return false;
}

// mov_sat rM, then the specular chain's steps between the diffuse steps
std::vector<DWORD> Interleaved(bool sat) {
    std::vector<DWORD> t = PixelHead();
    Add(t, {Op(kMov, 2), Dst(kTemp, 3, 0xF, sat), Src(kTemp, 2)});
    Add(t, {Op(kMul, 3), Dst(kTemp, 4, 0x7), Src(kTemp, 3, kYYYY), Src(kConst, 5)});
    Add(t, {Op(kMul, 3), Dst(kTemp, 5, 0x7), Src(kTemp, 1, kYYYY), Src(kConst, 5)}); // specular: its multiplier is not sat N.L
    Add(t, {Op(kMad, 4), Dst(kTemp, 6, 0x7), Src(kTemp, 3, kXXXX), Src(kConst, 4), Src(kTemp, 4)});
    Add(t, {Op(kMad, 4), Dst(kTemp, 5, 0x7), Src(kTemp, 1, kXXXX), Src(kConst, 4), Src(kTemp, 5)});
    Add(t, {Op(kMad, 4), Dst(kTemp, 3, 0x7), Src(kTemp, 3, kZZZZ), Src(kConst, 6), Src(kTemp, 6)}); // the rgb of the multiplier, .w stays
    Add(t, {Op(kMad, 4), Dst(kTemp, 3, 0x7), Src(kTemp, 3, kWWWW), Src(kConst, 7), Src(kTemp, 3)});
    Close(t, 3);
    return t;
}

// a dp3_sat before each step, into a register that also holds the accumulator
std::vector<DWORD> DotPerStep() {
    std::vector<DWORD> t = PixelHead();
    Add(t, {Op(kDp3, 3), Dst(kTemp, 0, 0x4, true), Src(kTemp, 2), Src(kConst, 1)});
    Add(t, {Op(kMul, 3), Dst(kTemp, 0, 0x7), Src(kTemp, 0, kZZZZ), Src(kConst, 5)});
    for (DWORD c : {4u, 6u, 7u}) {
        Add(t, {Op(kDp3, 3), Dst(kTemp, 0, 0x8, true), Src(kTemp, 2), Src(kConst, c - 4)});
        Add(t, {Op(kMad, 4), Dst(kTemp, 0, 0x7), Src(kTemp, 0, kWWWW), Src(kConst, c), Src(kTemp, 0)});
    }
    Close(t, 0);
    return t;
}

} // namespace

int main() {
    for (int k : {7, 6}) {
        std::vector<DWORD> t = VertexShader(0xFFFE0200u, true);
        Check(AddScreenPosVs(t, k), "a vertex shader writing its own TEXCOORDk still carries the position");
        Check(t[t.size() - 3] == Dst(kOut, static_cast<DWORD>(k)), "the position is the last write of TEXCOORDk");
    }
    {
        std::vector<DWORD> t = VertexShader(0xFFFE0300u, false);
        const auto before = t;
        Check(!AddScreenPosVs(t, 7) && t == before, "vs_3_0 stays refused");
    }
    for (const auto& shader : {Interleaved(true), DotPerStep()}) {
        std::vector<DWORD> t = shader;
        IndoorBasisPatch p;
        Check(PatchIndoorBasis(t, 1, p), "the diffuse chain is found");
        Check(p.diffuseConst > 7 && !Reads(t, 6), "the diffuse steps read their own copy of the rig");
    }
    {
        std::vector<DWORD> t = Interleaved(true);
        IndoorBasisPatch p;
        PatchIndoorBasis(t, 1, p);
        Check(Reads(t, 5) && Reads(t, 4), "the specular chain keeps c4 and c5");
    }
    {
        std::vector<DWORD> t = Interleaved(false);
        const auto before = t;
        IndoorBasisPatch p;
        Check(!PatchIndoorBasis(t, 1, p) && t == before, "a multiplier that is not sat N.L is refused");
    }
    std::printf("shader refusals: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
