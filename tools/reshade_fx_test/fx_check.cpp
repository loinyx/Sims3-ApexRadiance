// Offline check of the ReShade FX path (features/reshade_fx_compile.cpp): every .fx below the given folders is
// preprocessed, parsed and turned into shader model 3 code; on Windows each entry point is also compiled with D3DCompile
// (vs_3_0 / ps_3_0), as the game does. Read-only: it writes nothing but its report to stdout.
//
//   fx_check [--width W --height H] [--include DIR]... DIR...
//
// Exit code 0 when every effect in the folders given with --expect-ok compiled (the repository's own test effects);
// other folders (third-party packs on a developer machine) are reported only.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>
#include "features/reshade_fx_compile.h"
#ifdef _WIN32
#include <windows.h>
#include <d3dcompiler.h>
#pragma comment(lib, "d3dcompiler.lib")
#endif

namespace fs = std::filesystem;
using namespace ReShadeFxCompile;

#ifdef _WIN32
static bool CompileHlsl(const std::string& hlsl, const std::string& entry, const char* target, std::string& bytecode, std::string& errors) {
    ID3DBlob *code = nullptr, *msgs = nullptr;
    const HRESULT hr = D3DCompile(hlsl.data(), hlsl.size(), nullptr, nullptr, nullptr, entry.c_str(), target, D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, &code, &msgs);
    if (msgs) {
        errors.assign(static_cast<const char*>(msgs->GetBufferPointer()), msgs->GetBufferSize());
        msgs->Release();
    }
    if (code) {
        bytecode.assign(static_cast<const char*>(code->GetBufferPointer()), code->GetBufferSize());
        code->Release();
    }
    return SUCCEEDED(hr);
}
#endif

int main(int argc, char** argv) {
    Input base;
    base.width = 1920;
    base.height = 1080;
    base.definitions = {{"RESHADE_DEPTH_INPUT_IS_REVERSED", "0"}};
    std::vector<std::pair<fs::path, bool>> roots;
    bool expectNext = false;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--width" && i + 1 < argc) base.width = static_cast<uint32_t>(std::stoul(argv[++i]));
        else if (a == "--height" && i + 1 < argc) base.height = static_cast<uint32_t>(std::stoul(argv[++i]));
        else if (a == "--include" && i + 1 < argc) base.includes.emplace_back(argv[++i]);
        else if (a == "--expect-ok") expectNext = true;
        else {
            roots.emplace_back(a, expectNext);
            expectNext = false;
        }
    }
    if (roots.empty()) {
        std::printf("usage: fx_check [--width W --height H] [--include DIR]... [--expect-ok] DIR...\n");
        return 2;
    }
    int total = 0, parsed = 0, compiled = 0, expectedFailures = 0;
    for (const auto& [root, expectOk] : roots) {
        std::vector<fs::path> files;
        std::error_code ec;
        for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
            if (it->is_regular_file(ec) && it->path().extension() == ".fx") files.push_back(it->path());
        std::sort(files.begin(), files.end());
        // every folder of the root holding .fxh files is an include folder (as ReShade's "Shaders\**")
        Input in = base;
        for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
            if (it->is_directory(ec)) in.includes.push_back(it->path());
        in.includes.push_back(root);
        for (const auto& file : files) {
            total++;
            in.file = file;
            const auto t0 = std::chrono::steady_clock::now();
            Effect e = Parse(in);
            bool ok = e.parsed;
            if (ok) parsed++;
#ifdef _WIN32
            if (ok) {
                ok = Compile(e, CompileHlsl);
                if (ok) compiled++;
            }
#endif
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            size_t passes = 0;
            for (const auto& t : e.module.techniques) passes += t.passes.size();
            std::printf("%-4s %-60s %3zu techniques %3zu passes %3zu uniforms %6.0f ms%s%s\n", ok ? "ok" : "FAIL",
                        Utf8(fs::relative(file, root, ec)).c_str(), e.module.techniques.size(), passes, e.module.uniforms.size(), ms,
                        ok ? "" : "  ", ok ? "" : FirstError(e.errors).c_str());
            if (!ok && expectOk) expectedFailures++;
        }
    }
#ifdef _WIN32
    std::printf("\n%d effects: %d parsed, %d compiled to shader model 3\n", total, parsed, compiled);
#else
    std::printf("\n%d effects: %d parsed (no D3DCompile on this platform)\n", total, parsed);
#endif
    if (expectedFailures) std::printf("%d of the repository's test effects failed\n", expectedFailures);
    return expectedFailures ? 1 : 0;
}
