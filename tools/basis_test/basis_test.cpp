// Offline check of the smooth room light patches (ShaderPatches::PatchBasisSmooth / PatchIndoorBasis) on captured game
// shaders: prints whether each patch applies, and the disassembly of the result (d3dcompiler_47 validates the tokens).
// Read-only: writes nothing, console output only.
// Usage: basis_test <ps.bin> [lmSampler [mask]]   (lmSampler given = PatchIndoorBasis, else PatchBasisSmooth; mask = its wallMask copy)
#include "features/shader_patches.h"
#include <d3dcompiler.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <vector>
#pragma comment(lib, "d3dcompiler.lib")

static bool Dis(const std::vector<DWORD>& t) {
    ID3DBlob* blob = nullptr;
    if (FAILED(D3DDisassemble(t.data(), t.size() * 4, 0, nullptr, &blob)) || !blob) {
        std::printf("DISASSEMBLY FAILED\n");
        return false;
    }
    std::printf("%.*s\n", static_cast<int>(blob->GetBufferSize()), static_cast<const char*>(blob->GetBufferPointer()));
    blob->Release();
    return true;
}

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    std::ifstream f(argv[1], std::ios::binary);
    std::vector<char> raw((std::istreambuf_iterator<char>(f)), {});
    std::vector<DWORD> t(raw.size() / 4);
    std::memcpy(t.data(), raw.data(), t.size() * 4);
    int s[4];
    std::printf("basis samplers: %s", ShaderPatches::BasisSamplers(t, s) ? "" : "none\n");
    if (ShaderPatches::BasisSamplers(t, s)) std::printf("+X s%d, -X s%d, +Z s%d, -Z s%d\n", s[0], s[1], s[2], s[3]);
    bool ok;
    if (argc >= 3) {
        ShaderPatches::IndoorBasisPatch p;
        const bool mask = argc >= 4 && std::strcmp(argv[3], "mask") == 0;
        ok = ShaderPatches::PatchIndoorBasis(t, static_cast<DWORD>(std::atoi(argv[2])), p, true, mask);
        ok = ok && (!mask || p.maskSampler == static_cast<int>(p.firstSampler) + 4);
        std::printf("PatchIndoorBasis%s: %s (basis samplers s%lu..s%lu, strength c%lu, wall mask s%d)\n", mask ? " (wall mask)" : "", ok ? "applied" : "not applied",
                    p.firstSampler, p.firstSampler + 3, p.strengthConst, p.maskSampler);
    } else {
        ShaderPatches::BasisSmoothPatch bp;
        ok = ShaderPatches::PatchBasisSmooth(t, bp);
        std::printf("PatchBasisSmooth: %s\n", ok ? "applied" : "not applied");
    }
    Dis(t); // the patched shader, or the original when the patch did not apply
    return ok ? 0 : 1;
}
