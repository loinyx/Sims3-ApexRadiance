#define NOMINMAX
#include "features/shader_patches.h"
#include <d3d9.h>
#include <fstream>
#include <cstring>
#include <iostream>
#include <map>
#include <cassert>
#pragma comment(lib, "d3d9.lib")
#pragma comment(lib, "user32.lib")

std::vector<DWORD> Load(const char* path) {
    std::ifstream f(path, std::ios::binary);
    std::vector<char> b((std::istreambuf_iterator<char>(f)), {});
    assert(!b.empty() && b.size() % 4 == 0);
    std::vector<DWORD> out(b.size() / 4);
    std::memcpy(out.data(), b.data(), b.size());
    return out;
}
int main(int argc, char** argv) {
    assert(argc == 5);
    const auto vs = Load(argv[1]), base = Load(argv[2]), overlay = Load(argv[3]);
    const auto saved = vs;
    assert(ShaderPatches::WorldTerrainMapConstant(vs) == 16);
    assert(vs == saved);
    auto duplicate = vs;
    assert((vs[vs.size() - 6] & 0xFFFF) == 4);
    duplicate.insert(duplicate.end() - 1, vs.end() - 6, vs.end() - 1);
    assert(ShaderPatches::WorldTerrainMapConstant(duplicate) < 0);
    auto noOrigin = vs;
    bool removed = false;
    for (auto& token : noOrigin) if (token == 0xA0FF0008u) { token = 0xA0FF0009u; removed = true; break; }
    assert(removed && ShaderPatches::WorldTerrainMapConstant(noOrigin) < 0);
    auto truncated = vs; truncated.pop_back();
    assert(ShaderPatches::WorldTerrainMapConstant(truncated) < 0);
    assert(ShaderPatches::WorldTerrainLampSamplers(base, 0xFF) == (1u << 6));
    assert(ShaderPatches::WorldTerrainLampSamplers(overlay, 0x7F) == (1u << 3));
    assert(ShaderPatches::LightMapScaleConst(overlay, 4) < 0);
    assert(ShaderPatches::LightMapScaleConst(base, 6) == 7);
    assert(ShaderPatches::LightMapScaleConst(overlay, 3) == 7);
    assert(ShaderPatches::WorldTerrainLampSamplers({}, 0x1FE) == 0x1FE);
    auto paintBias = vs;
    paintBias[paintBias.size() - 2] = (paintBias[paintBias.size() - 2] & ~0x7FFu) | 15u;
    assert(ShaderPatches::WorldTerrainMapConstant(paintBias) < 0);

    HWND window = CreateWindowA("STATIC", "terrain checks", WS_OVERLAPPED, 0, 0, 64, 64, nullptr, nullptr, GetModuleHandleA(nullptr), nullptr);
    auto* api = Direct3DCreate9(D3D_SDK_VERSION);
    assert(api && window);
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE; pp.SwapEffect = D3DSWAPEFFECT_DISCARD; pp.hDeviceWindow = window;
    IDirect3DDevice9* device = nullptr;
    assert(SUCCEEDED(api->CreateDevice(0, D3DDEVTYPE_HAL, window, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &device)));
    for (const auto* input : {&base, &overlay}) {
        auto code = *input;
        const DWORD lamp = input == &base ? 6 : 3;
        DWORD native = 0, shade = 0;
        assert(ShaderPatches::PatchTerrainNativeAlpha(code, lamp, native));
        assert(ShaderPatches::PatchTerrainShade(code, lamp, shade));
        assert(native != lamp && shade != native && shade != lamp);
        IDirect3DPixelShader9* shader = nullptr;
        assert(SUCCEEDED(device->CreatePixelShader(code.data(), &shader)) && shader);
        shader->Release();
    }
    device->Release(); api->Release(); DestroyWindow(window);

    std::ifstream gameFile(argv[4], std::ios::binary);
    const std::vector<unsigned char> game((std::istreambuf_iterator<char>(gameFile)), {});
    assert(!game.empty());
    std::map<int, unsigned> mappings;
    unsigned shaders = 0;
    for (size_t i = 0; i + 8 < game.size(); ++i) {
        DWORD version = 0;
        std::memcpy(&version, &game[i], 4);
        if (version != 0xFFFE0300u && version != 0xFFFE0200u) continue;
        size_t end = i + 4;
        DWORD token = 0;
        while (end + 4 <= game.size() && end - i < 65536) {
            std::memcpy(&token, &game[end], 4);
            if (token == 0xFFFFu) break;
            const size_t n = (token & 0xFFFF) == 0xFFFE ? (token >> 16) & 0x7FFF : (token >> 24) & 15;
            if ((token & 0x80000000u) || end + (n + 1) * 4 > game.size()) break;
            end += (n + 1) * 4;
        }
        if (end + 4 > game.size() || token != 0xFFFFu) continue;
        std::vector<DWORD> code((end + 4 - i) / 4);
        std::memcpy(code.data(), &game[i], end + 4 - i);
        ++shaders;
        const int mapping = ShaderPatches::WorldTerrainMapConstant(code);
        if (mapping >= 0) ++mappings[mapping];
        i = end + 3;
    }
    assert(shaders && mappings[16]);
    std::cout << "Captured frozen terrain: c16; lamp samplers s6 and s3; alpha-only s4 excluded; malformed layouts refused; native-alpha and shade copies load\n";
    std::cout << shaders << " game vertex shaders scanned:";
    for (const auto& [constant, count] : mappings) std::cout << " c" << constant << '=' << count;
    std::cout << '\n';
}
