// Real-time GI compose (S2-01b), offline through the built bridge pair: a 32-bit Direct3D 9 app on the bridge client
// d3d9.dll submits the scene the game's builder made of R-02's synthetic lot (tools/gi_server_test --dump), has the server
// solve every lamp's indirect and direct layers into DEFAULT-pool textures, runs Apex's compose pass (features/gi_compose.cpp)
// into the irradiance atlas and reads it back. Checked against the same compose on the CPU over the offline solve's
// layers (GiCompose::ComposeCpu): every lamp at its colour; a crossfade between two slots; a partial compose (one lamp
// recoloured, only the tiles of one room composed again, the rest unchanged).
// Run by run.ps1 through the bridge's test launcher (it deploys this exe next to the client and the server).
// REMIX_GI_TEST_SCENE: the dump; REMIX_GI_TEST_CASE: on or compute. Prints to stdout; exit code 0 = all passed.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>
#include "features/gi_compose.h"
#include "features/gi_scene.h"

namespace {
using OpenFn = HRESULT(__stdcall*)(uint32_t*);
using SubmitFn = HRESULT(__stdcall*)(const void*, uint32_t, uint32_t, uint32_t*);
using SolveFn = HRESULT(__stdcall*)(IDirect3DTexture9*, uint32_t, uint32_t, uint32_t*);
using TicketFn = HRESULT(__stdcall*)(uint32_t);
OpenFn g_open;
SubmitFn g_submit;
SolveFn g_solve;
TicketFn g_poll;
IDirect3DDevice9* g_dev;
int g_failed = 0;

void Check(bool ok, const std::string& what) {
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) g_failed++;
}
void Hr(HRESULT hr, const char* what) {
    if (FAILED(hr)) throw std::runtime_error(std::string(what) + " failed: " + std::to_string(static_cast<long>(hr)));
}
// Presents between polls, as a game does, so commands and copies reach the server and back
HRESULT Wait(uint32_t ticket, const char* what) {
    const ULONGLONG deadline = GetTickCount64() + 60000;
    HRESULT hr;
    while ((hr = g_poll(ticket)) == S_FALSE) {
        if (GetTickCount64() > deadline) throw std::runtime_error(std::string(what) + ": no answer in 60 s");
        g_dev->Present(nullptr, nullptr, nullptr, nullptr);
        Sleep(1);
    }
    return hr;
}
// A blob in parts of a submit slot; the ticket of its last part once the server answered it
uint32_t Submit(const uint8_t* data, size_t size, uint32_t kind) {
    constexpr size_t kSlot = 448 * 1024;
    for (size_t sent = 0; sent < size;) {
        const uint32_t n = static_cast<uint32_t>(std::min(kSlot, size - sent));
        const bool last = sent + n == size;
        uint32_t t = 0;
        HRESULT hr;
        while ((hr = g_submit(data + sent, n, kind | (last ? 0 : 0x100), &t)) == D3DERR_WASSTILLDRAWING) g_dev->Present(nullptr, nullptr, nullptr, nullptr);
        Hr(hr, "RemixBridge_GiSubmit");
        sent += n;
        if (last) {
            Hr(Wait(t, "submit") == S_OK ? S_OK : E_FAIL, "the server's answer to the submit");
            return t;
        }
    }
    return 0;
}
IDirect3DTexture9* Target(uint32_t w, uint32_t h) {
    IDirect3DTexture9* t = nullptr;
    Hr(g_dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &t, nullptr), "CreateTexture");
    IDirect3DSurface9* s = nullptr;
    t->GetSurfaceLevel(0, &s);
    g_dev->ColorFill(s, nullptr, D3DCOLOR_ARGB(0, 0, 0, 0));
    s->Release();
    return t;
}
float Half(uint16_t h) {
    const int e = (h >> 10) & 31, m = h & 1023;
    const float v = e == 0 ? std::ldexp(float(m), -24) : std::ldexp(float(m | 1024), e - 25);
    return (h & 0x8000) ? -v : v;
}
std::vector<float> Read(IDirect3DTexture9* t, uint32_t w, uint32_t h) {
    IDirect3DSurface9 *s = nullptr, *sys = nullptr;
    t->GetSurfaceLevel(0, &s);
    Hr(g_dev->CreateOffscreenPlainSurface(w, h, D3DFMT_A16B16G16R16F, D3DPOOL_SYSTEMMEM, &sys, nullptr), "CreateOffscreenPlainSurface");
    Hr(g_dev->GetRenderTargetData(s, sys), "GetRenderTargetData");
    D3DLOCKED_RECT lr{};
    Hr(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY), "LockRect");
    std::vector<float> out(static_cast<size_t>(w) * h * 4);
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w * 4; x++) out[(static_cast<size_t>(y) * w * 4) + x] = Half(reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(lr.pBits) + y * lr.Pitch)[x]);
    sys->UnlockRect();
    sys->Release(), s->Release();
    return out;
}
// GPU against CPU over `pixels`: within tol (rgb)
void Compare(const std::vector<float>& gpu, const std::vector<float>& cpu, const std::vector<uint32_t>& pixels, float tol, const std::string& what) {
    size_t within = 0;
    float worst = 0;
    for (uint32_t p : pixels) {
        float d = 0;
        for (int c = 0; c < 3; c++) d = std::max(d, std::fabs(gpu[p * 4 + c] - cpu[p * 4 + c]));
        within += d <= tol;
        worst = std::max(worst, d);
    }
    Check(within == pixels.size(), what + ": " + std::to_string(within) + " of " + std::to_string(pixels.size()) + " texels within " + std::to_string(tol * 255) +
                                       "/255, worst " + std::to_string(worst * 255) + "/255");
}
} // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    char mode[16] = {}, scenePath[MAX_PATH] = {};
    GetEnvironmentVariableA("REMIX_GI_TEST_CASE", mode, sizeof mode);
    GetEnvironmentVariableA("REMIX_GI_TEST_SCENE", scenePath, sizeof scenePath);
    const std::string testCase = mode[0] ? mode : "on";
    try {
        std::ifstream f(scenePath, std::ios::binary);
        const std::vector<uint8_t> file((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        uint32_t head[5] = {};
        if (file.size() < sizeof head) throw std::runtime_error("no scene file in REMIX_GI_TEST_SCENE");
        std::memcpy(head, file.data(), sizeof head);
        const uint32_t T = head[1], L = head[2];
        const uint8_t* scene = file.data() + sizeof head;
        const uint8_t* lamps = scene + head[3];
        const float* direct = reinterpret_cast<const float*>(lamps + head[4] + static_cast<size_t>(T) * 4);
        const float* ind = direct + static_cast<size_t>(L) * T * 4;
        const auto* sh = reinterpret_cast<const GiScene::Header*>(scene);
        const auto* tx = reinterpret_cast<const GiScene::Texel*>(scene + sizeof *sh + sh->triangles * sizeof(GiScene::Tri) + sh->patches * sizeof(GiScene::Patch) + sh->mapEntries * 4);
        const uint32_t W = GiScene::kAtlasWidth, H = (T + W - 1) / W;

        std::ofstream config(".trex/bridge.conf", std::ios::app);
        config << "client.giServer = True\nserver.giServer = True\nserver.giMaxLevel = " << (testCase == "compute" ? 1 : 2) << "\nsendCreateFunctionServerResponses = True\n";
        config.close();
        wchar_t exe[MAX_PATH] = {};
        std::wstring client(exe, GetModuleFileNameW(nullptr, exe, MAX_PATH));
        client = client.substr(0, client.find_last_of(L"\\/") + 1) + L"d3d9.dll";
        const HMODULE m = LoadLibraryW(client.c_str());
        if (!m) throw std::runtime_error("the bridge client d3d9.dll is not next to the test");
        g_open = reinterpret_cast<OpenFn>(GetProcAddress(m, "RemixBridge_GiOpen"));
        g_submit = reinterpret_cast<SubmitFn>(GetProcAddress(m, "RemixBridge_GiSubmit"));
        g_solve = reinterpret_cast<SolveFn>(GetProcAddress(m, "RemixBridge_GiSolve"));
        g_poll = reinterpret_cast<TicketFn>(GetProcAddress(m, "RemixBridge_GiPoll"));
        const auto create = reinterpret_cast<IDirect3D9*(WINAPI*)(UINT)>(GetProcAddress(m, "Direct3DCreate9"));
        if (!g_open || !g_submit || !g_solve || !g_poll || !create) throw std::runtime_error("the client does not export the GI functions");
        WNDCLASSW wc{};
        wc.lpfnWndProc = DefWindowProcW, wc.hInstance = GetModuleHandleW(nullptr), wc.lpszClassName = L"ApexGiComposeTest";
        RegisterClassW(&wc);
        const HWND wnd = CreateWindowExW(0, wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
        IDirect3D9* d3d = create(D3D_SDK_VERSION);
        D3DPRESENT_PARAMETERS pp{};
        pp.Windowed = TRUE, pp.SwapEffect = D3DSWAPEFFECT_DISCARD, pp.BackBufferWidth = pp.BackBufferHeight = 64, pp.hDeviceWindow = wnd;
        Hr(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_MULTITHREADED, &pp, &g_dev), "CreateDevice");

        std::printf("Compose through the bridge pair, mode %s: %u texels (atlas %u x %u), %u lamps\n", testCase.c_str(), T, W, H, L);
        uint32_t level = 0;
        const ULONGLONG deadline = GetTickCount64() + 10000;
        HRESULT hr;
        while ((hr = g_open(&level)) == S_FALSE && GetTickCount64() < deadline) g_dev->Present(nullptr, nullptr, nullptr, nullptr), Sleep(5);
        Hr(hr == S_OK && level ? S_OK : E_FAIL, "RemixBridge_GiOpen");
        std::printf("  server level %u\n", level);
        const uint32_t sceneTicket = Submit(scene, head[3], 1);
        Submit(lamps, head[4], 2);

        // every lamp's indirect layer (slot A) and direct layer (slot B)
        std::vector<IDirect3DTexture9*> slotA(L), slotB(L);
        for (uint32_t l = 0; l < L; l++) {
            slotA[l] = Target(W, H), slotB[l] = Target(W, H);
            for (int what = 0; what < 2; what++) {
                uint32_t t = 0;
                Hr(g_solve(what ? slotB[l] : slotA[l], l | (static_cast<uint32_t>(what) << 16), sceneTicket, &t), "RemixBridge_GiSolve");
                Hr(Wait(t, "solve") == S_OK ? S_OK : E_FAIL, "a solve");
            }
        }
        // the offline layers by pixel (texel j at its pixel)
        const size_t N = static_cast<size_t>(W) * H;
        std::vector<std::vector<float>> offInd(L, std::vector<float>(N * 4)), offDir(L, std::vector<float>(N * 4));
        for (uint32_t l = 0; l < L; l++)
            for (uint32_t j = 0; j < T; j++)
                for (int c = 0; c < 4; c++)
                    offInd[l][tx[j].pixel * 4 + c] = ind[(static_cast<size_t>(l) * T + j) * 4 + c], offDir[l][tx[j].pixel * 4 + c] = direct[(static_cast<size_t>(l) * T + j) * 4 + c];
        // tiles: 1 m and 3 m stories by the texel's point (the scheduler's tile lists in the game)
        std::map<std::tuple<int, int, int>, std::vector<uint32_t>> tiles;
        std::vector<uint32_t> all;
        for (uint32_t j = 0; j < T; j++) {
            tiles[{static_cast<int>(std::floor(tx[j].pos[1] / 3)), static_cast<int>(std::floor(tx[j].pos[0])), static_cast<int>(std::floor(tx[j].pos[2]))}].push_back(tx[j].pixel);
            all.push_back(tx[j].pixel);
        }

        ID3DBlob *code = nullptr, *err = nullptr;
        Hr(D3DCompile(GiCompose::kPixelShader, std::strlen(GiCompose::kPixelShader), "gi_compose.hlsl", nullptr, nullptr, "main", "ps_2_0", 0, 0, &code, &err), "D3DCompile");
        IDirect3DPixelShader9* ps = nullptr;
        Hr(g_dev->CreatePixelShader(static_cast<const DWORD*>(code->GetBufferPointer()), &ps), "CreatePixelShader");
        IDirect3DTexture9* out = Target(W, H);
        GiCompose::Composer composer;
        const GiCompose::Calls calls;
        const float colours[3][3] = {{1.0f, 0.8f, 0.6f}, {0.5f, 0.7f, 1.0f}, {0.9f, 0.9f, 0.4f}};
        float tol = 0; // 1/255 a lamp's layer (the bridge against the offline solve, R-04) times its colour, plus half-float rounding
        for (uint32_t l = 0; l < L; l++) tol += std::max({colours[l % 3][0], colours[l % 3][1], colours[l % 3][2]}) / 255.0f;
        tol += 2e-3f;

        // 1. every lamp at its colour, all tiles
        std::vector<GiCompose::LampDraw> draws;
        std::vector<GiCompose::CpuLamp> cpus;
        for (uint32_t l = 0; l < L; l++) {
            GiCompose::LampDraw d;
            d.shown = slotA[l];
            std::memcpy(d.colour, colours[l % 3], 12);
            d.pixels = all;
            draws.push_back(d);
            cpus.push_back({offInd[l].data(), nullptr, {colours[l % 3][0], colours[l % 3][1], colours[l % 3][2]}, 1.0f, all});
        }
        int drawn = 0;
        const bool composed = composer.Compose(g_dev, ps, out, W, H, all, draws, calls, &drawn);
        Check(composed && drawn == static_cast<int>(L) + 1, "compose: every lamp, every tile (" + std::to_string(drawn) + " draws)");
        std::vector<float> cpu(N * 4, 0.0f);
        GiCompose::ComposeCpu(cpu, all, cpus);
        const std::vector<float> first = Read(out, W, H);
        Compare(first, cpu, all, tol, "irradiance against the CPU compose of the offline layers");
        double sum = 0;
        for (uint32_t p : all) sum += first[p * 4 + 1];
        std::printf("    mean green %.4f\n", sum / T);

        // 2. a crossfade: lamp 0 from its indirect layer to its direct layer at 0.3
        draws[0].incoming = slotB[0], draws[0].blend = 0.3f;
        cpus[0].incoming = offDir[0].data(), cpus[0].blend = 0.3f;
        Check(composer.Compose(g_dev, ps, out, W, H, all, draws, calls), "compose: lamp 0 crossfading");
        GiCompose::ComposeCpu(cpu, all, cpus);
        // (the direct layer's texels are within 1/255 of the offline one's too, R-04)
        Compare(Read(out, W, H), cpu, all, tol + 1.0f / 255, "crossfade 0.3 against the CPU");
        draws[0].incoming = nullptr, cpus[0].incoming = nullptr;
        Check(composer.Compose(g_dev, ps, out, W, H, all, draws, calls), "compose: back to the shown slot");

        // 3. lamp 1 recoloured, only the tiles with x < 16 composed again: those change, the others keep the first compose
        std::vector<uint32_t> part, rest;
        for (const auto& [k, px] : tiles) (std::get<1>(k) < 16 ? part : rest).insert((std::get<1>(k) < 16 ? part : rest).end(), px.begin(), px.end());
        for (uint32_t l = 0; l < L; l++) draws[l].pixels = part, cpus[l].pixels = part;
        if (L > 1) draws[1].colour[0] = cpus[1].colour[0] = 0.1f;
        Check(composer.Compose(g_dev, ps, out, W, H, part, draws, calls), "compose: " + std::to_string(part.size()) + " texels of the tiles at x < 16");
        GiCompose::ComposeCpu(cpu, part, cpus);
        const std::vector<float> partial = Read(out, W, H);
        Compare(partial, cpu, part, tol, "recomposed tiles against the CPU");
        size_t kept = 0;
        for (uint32_t p : rest) kept += std::memcmp(&partial[p * 4], &first[p * 4], 16) == 0;
        Check(kept == rest.size(), "the other " + std::to_string(rest.size()) + " texels unchanged (" + std::to_string(kept) + ")");

        // 4. (patch 0020) the game's scope rule: each lamp solved again in its touch box grown by 0.5 m against its
        // unscoped indirect layer
        using ReadTouchFn = HRESULT(__stdcall*)(uint32_t, float*, uint32_t*);
        if (const auto readTouch = reinterpret_cast<ReadTouchFn>(GetProcAddress(m, "RemixBridge_GiReadTouch"))) {
            std::vector<uint8_t> scoped(lamps, lamps + head[4]);
            for (uint32_t l = 0; l < L; l++) {
                float box[6];
                uint32_t texels = 0;
                Hr(readTouch(l, box, &texels), "RemixBridge_GiReadTouch");
                for (int k = 0; k < 3; k++) box[k] -= 0.5f, box[3 + k] += 0.5f;
                std::memcpy(scoped.data() + l * 56 + 32, box, 24); // GiLamp boxMin, boxMax
                std::printf("    lamp %u touch box: %u texels\n", l, texels);
            }
            Submit(scoped.data(), scoped.size(), 2);
            for (uint32_t l = 0; l < L; l++) {
                uint32_t t = 0;
                Hr(g_solve(slotB[l], l, sceneTicket, &t), "RemixBridge_GiSolve (scoped)");
                Hr(Wait(t, "scoped solve") == S_OK ? S_OK : E_FAIL, "a scoped solve");
                const std::vector<float> a = Read(slotA[l], W, H), b = Read(slotB[l], W, H);
                size_t within = 0, darkA = 0, darkB = 0;
                float worst = 0;
                for (uint32_t p : all) {
                    darkA += a[p * 4] == 0 && a[p * 4 + 1] == 0 && a[p * 4 + 2] == 0, darkB += b[p * 4] == 0 && b[p * 4 + 1] == 0 && b[p * 4 + 2] == 0;
                    float d = 0;
                    for (int c = 0; c < 3; c++) d = std::max(d, std::fabs(a[p * 4 + c] - b[p * 4 + c]));
                    within += d <= 1.0f / 255, worst = std::max(worst, d);
                }
                Check(within * 1000 >= all.size() * 995 && worst <= 4.0f / 255,
                      "lamp " + std::to_string(l) + " scoped to its touch box + 0.5 m: " + std::to_string(within) + " of " + std::to_string(all.size()) +
                          " texels within 1/255 of unscoped, worst " + std::to_string(worst * 255) + "/255, unlit " + std::to_string(darkA) + " -> " + std::to_string(darkB));
            }
        } else {
            std::printf("  (no RemixBridge_GiReadTouch: patch 0020 not in this bridge, scope check skipped)\n");
        }

        composer.Release();
        out->Release(), ps->Release(), code->Release();
        for (uint32_t l = 0; l < L; l++) slotA[l]->Release(), slotB[l]->Release();
        g_dev->Release(), d3d->Release();
    } catch (const std::exception& e) {
        std::printf("FAILED: %s\n", e.what());
        return 1;
    }
    std::printf(g_failed ? "FAILED: %d check(s)\n" : "PASSED\n", g_failed);
    return g_failed ? 1 : 0;
}
