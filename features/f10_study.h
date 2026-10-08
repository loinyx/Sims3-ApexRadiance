#pragma once
// Bounded read-only experiment, compiled ONLY in APEX_F10_STUDY test artifacts.
// No readbacks or requests exist in the normal build. UI state is inferred from F10 messages, not proven by the driver.
#ifdef APEX_F10_STUDY
#include "apex_paths.h"
#include "apex_version.h"
#include <d3d9.h>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <string>

namespace F10Study {
inline std::atomic<unsigned> request{0}; // UI/render threads exchange only this encoded event
inline std::atomic<unsigned> issued{0};
inline unsigned event = 0;
inline bool active = false, ditherRecorded = false, copyRecorded = false;
inline unsigned stages = 0;
inline unsigned long long presentIndex = 0;
inline unsigned long long started = 0;
inline std::filesystem::path folder;

inline void Request(bool inferredHidden) {
    const unsigned n = issued.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 8) request.store(n * 2 + unsigned(inferredHidden), std::memory_order_release);
}
inline void Note(const std::string& line) {
    if (!active || folder.empty()) return;
    try {
        std::ofstream file(folder / L"stages.txt", std::ios::app);
        file << GetTickCount64() - started << " ms | presentCounter=" << presentIndex << " | " << line << '\n';
    } catch (...) {} // diagnostics must never interrupt the game
}
inline void Advance() {
    if (active) Note("frame completed");
    ++presentIndex;
    active = false;
    event = request.exchange(0, std::memory_order_acquire);
    if (!event) return;
    try {
        if (!ApexPaths::EnsureApexDirectory()) return;
        folder = std::filesystem::path(ApexPaths::ApexDirectory()) / L"F10 Study" /
                 (L"event-" + std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(event / 2));
        std::filesystem::create_directories(folder);
        active = true; ditherRecorded = false; copyRecorded = false; stages = 0; started = GetTickCount64();
        Note(std::string("version ") + APEX_VERSION_STRING + " | inferred UI hidden " + std::to_string(event & 1));
        Note("ONE frame per event; max 8 events/session; RGBA samples are POINT-reduced 64x64 BGRA bytes, not full images. "
             "Readbacks stall the GPU: do not use timings as performance evidence. UI inference is not actual engine state.");
    } catch (...) { active = false; }
}
inline void PhotoReceipt(unsigned width, unsigned height, bool toggledUi) {
    if (folder.empty()) return;
    try {
        std::ofstream file(folder / L"stages.txt", std::ios::app);
        file << GetTickCount64() - started << " ms | photo readback; presentCounter=" << presentIndex
             << " | size=" << width << 'x' << height << " | posted hide key=" << toggledUi
             << " | attached to most recent diagnostic event; Present callbacks may already have reset frame counters\n";
    } catch (...) {}
}
inline std::string States(IDirect3DDevice9* dev) {
    std::string text;
    for (const auto rs : {D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ZFUNC, D3DRS_COLORWRITEENABLE,
                         D3DRS_SRGBWRITEENABLE, D3DRS_ALPHABLENDENABLE}) {
        DWORD value = 0;
        text += " rs" + std::to_string(unsigned(rs)) + "=";
        text += SUCCEEDED(dev->GetRenderState(rs, &value)) ? std::to_string(value) : "unreadable";
    }
    IDirect3DSurface9* ds = nullptr;
    if (SUCCEEDED(dev->GetDepthStencilSurface(&ds))) {
        text += " reportedDS=" + std::to_string(reinterpret_cast<uintptr_t>(ds));
        if (ds) ds->Release();
    }
    return text;
}
template<class Shader> inline void ShaderFile(Shader* shader, const std::string& name) {
    if (!active || !shader) return;
    try {
        UINT size = 0;
        if (FAILED(shader->GetFunction(nullptr, &size)) || size == 0 || size > 65536) return;
        std::string bytes(size, '\0');
        if (SUCCEEDED(shader->GetFunction(bytes.data(), &size))) {
            std::ofstream file(folder / (name + ".bin"), std::ios::binary);
            file.write(bytes.data(), size);
        }
    } catch (...) {}
}
inline void CompositionShader(IDirect3DDevice9* dev, IDirect3DPixelShader9* original,
                              IDirect3DPixelShader9* used, IDirect3DVertexShader9* vs, bool patchBound) {
    if (!active || ditherRecorded) return;
    DWORD z = FALSE, write = TRUE, func = 0;
    if (FAILED(dev->GetRenderState(D3DRS_ZENABLE, &z)) || !z ||
        FAILED(dev->GetRenderState(D3DRS_ZWRITEENABLE, &write)) || write ||
        FAILED(dev->GetRenderState(D3DRS_ZFUNC, &func)) || func != D3DCMP_ALWAYS) return;
    ditherRecorded = true;
    Note("composition: SceneDither actually bound=" + std::to_string(patchBound) + States(dev));
    ShaderFile(original, "composition-original-ps"); ShaderFile(used, "composition-used-ps");
    IDirect3DVertexShader9* readVs = vs;
    if (!readVs) dev->GetVertexShader(&readVs);
    ShaderFile(readVs, "composition-used-vs");
    if (!vs && readVs) readVs->Release();
    try {
        float constants[224 * 4]{};
        if (SUCCEEDED(dev->GetPixelShaderConstantF(0, constants, 224))) {
            std::ofstream file(folder / L"composition-ps-constants.bin", std::ios::binary);
            file.write(reinterpret_cast<const char*>(constants), sizeof constants);
        }
    } catch (...) {}
}
inline void Sample(IDirect3DDevice9* dev, const std::string& name) {
    if (!active || !dev || stages >= 10) return;
    const unsigned long long begin = GetTickCount64();
    Note("sample " + name + States(dev));
    ++stages;
    IDirect3DSurface9 *bb = nullptr, *rt = nullptr, *sampleSurface = nullptr, *cpu = nullptr;
    if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb &&
        SUCCEEDED(dev->GetRenderTarget(0, &rt)) && rt == bb) {
        D3DSURFACE_DESC desc{};
        if (SUCCEEDED(bb->GetDesc(&desc)) && desc.Format == D3DFMT_A8R8G8B8 && desc.MultiSampleType == D3DMULTISAMPLE_NONE &&
            SUCCEEDED(dev->CreateRenderTarget(64, 64, desc.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &sampleSurface, nullptr)) && sampleSurface &&
            SUCCEEDED(dev->CreateOffscreenPlainSurface(64, 64, desc.Format, D3DPOOL_SYSTEMMEM, &cpu, nullptr)) && cpu &&
            SUCCEEDED(dev->StretchRect(bb, nullptr, sampleSurface, nullptr, D3DTEXF_POINT)) &&
            SUCCEEDED(dev->GetRenderTargetData(sampleSurface, cpu))) {
            D3DLOCKED_RECT locked{};
            if (SUCCEEDED(cpu->LockRect(&locked, nullptr, D3DLOCK_READONLY))) {
                try {
                    std::ofstream file(folder / (name + ".bgra"), std::ios::binary);
                    for (unsigned y = 0; y < 64; ++y)
                        file.write(static_cast<const char*>(locked.pBits) + y * locked.Pitch, 64 * 4);
                } catch (...) {}
                cpu->UnlockRect();
            }
        } else Note("sample unavailable: unsupported surface or failed readback");
    } else Note("sample unavailable: RT0 is not the back buffer");
    if (cpu) cpu->Release(); if (sampleSurface) sampleSurface->Release(); if (rt) rt->Release(); if (bb) bb->Release();
    Note("sample overhead ms=" + std::to_string(GetTickCount64() - begin));
}
inline void Copy(IDirect3DDevice9* dev, const RECT* src, const RECT* dst, D3DTEXTUREFILTERTYPE filter,
                 IDirect3DSurface9* destination, bool done, bool uiSeen, bool learned) {
    if (!active || copyRecorded || !src || !dst || src->left || src->top || src->right != 256 || src->bottom != 256) return;
    copyRecorded = true;
    D3DSURFACE_DESC desc{};
    if (destination) destination->GetDesc(&desc);
    Note("first tile copy: effectsDone=" + std::to_string(done) + " uiSeen=" + std::to_string(uiSeen) +
         " learnedMatch=" + std::to_string(learned) + " filter=" + std::to_string(unsigned(filter)) +
         " dst=" + std::to_string(desc.Width) + "x" + std::to_string(desc.Height) + States(dev));
    Sample(dev, "game-tile-input");
}
} // namespace F10Study
#endif
