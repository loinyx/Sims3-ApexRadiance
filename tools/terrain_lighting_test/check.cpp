// Read-only harness: production policies and patches, captured shaders and D3D9
// pixel readback. Prints to stdout; never writes into the game or capture folder.
#define NOMINMAX
#include "features/terrain_lighting_policy.h"
#include "features/native_terrain_sampler.h"
#include "features/shader_patches.h"
#include "features/lamp_reach.h"
#include "features/ground_lamp_shadow.h"
#include <algorithm>
#include <d3dcompiler.h>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <random>
#include <string>
#include <vector>
#pragma comment(lib, "d3d9.lib")
#pragma comment(lib, "d3dcompiler.lib")
#pragma comment(lib, "user32.lib")

int checks = 0, failures = 0;
void Check(bool ok, const char* message) {
    ++checks;
    if (!ok) { ++failures; if (failures < 30) std::printf("FAIL: %s\n", message); }
}
bool Near(float a, float b) { return std::abs(a - b) <= 2e-5f * std::max(1.f, std::abs(b)); }
template<class T> void Release(T*& p) { if (p) { p->Release(); p = nullptr; } }
std::vector<DWORD> Compile(const std::string& source, const char* profile) {
    ID3DBlob *code = nullptr, *errors = nullptr;
    const HRESULT hr = D3DCompile(source.data(), source.size(), nullptr, nullptr, nullptr, "main", profile,
                                 D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errors);
    if (FAILED(hr) && errors) std::printf("%s\n", static_cast<char*>(errors->GetBufferPointer()));
    Check(SUCCEEDED(hr), "HLSL compiles");
    std::vector<DWORD> result;
    if (code) result.assign(static_cast<DWORD*>(code->GetBufferPointer()), static_cast<DWORD*>(code->GetBufferPointer()) + code->GetBufferSize() / 4);
    Release(code); Release(errors);
    return result;
}
void Policies() {
    using namespace TerrainLightingPolicy;
    // One daytime curve (12/10): LampDay(n) = 0.03 + 0.97 smoothstep(0, 1, n), 1 exactly at night, monotonic, the same
    // factor on every outdoor surface
    Check(LampDay(0)==kLampDayFloor&&LampDay(-1)==kLampDayFloor&&LampDay(std::numeric_limits<float>::quiet_NaN())==kLampDayFloor,"daytime curve floor 3% (invalid levels too)");
    Check(LampDay(1)==1.f&&LampDay(2)==1.f,"daytime curve exactly 1 at night");
    Check(Near(LampDay(.5f),.515f)&&Near(LampDay(.25f),.03f+.97f*.15625f),"daytime curve follows smoothstep");
    for(int n=1;n<=1000;++n)Check(LampDay(n/1000.f)>=LampDay((n-1)/1000.f),"daytime curve monotonic");
    for(int n=0;n<=100;++n)for(int g=0;g<=80;++g){
        const float night=n/100.f,gain=g/10.f,effective=SurfaceLampGain(night,gain);
        Check(std::isfinite(effective)&&effective>=0,"surface gain finite and nonnegative");
        Check(effective<=gain&&effective>=gain*kLampDayFloor*.999999f,"surface gain stays within daytime and saved nighttime bounds");
        if(n==0)Check(effective==gain*kLampDayFloor,"daytime surfaces take the 3% floor of their night strength");
        if(n==100)Check(effective==gain,"nighttime surface gain stays bit exact");
        if(n<100)Check(effective==gain*LampDay(night),"surface gain follows the shared curve");
        // walls: before full night gain x LampDay whatever the native factor; at night native x gain bit exact
        for(float native:{0.f,.188235313f,1.f})Check(WallLampScale(native,night,gain)==(n==100?native*gain:gain*LampDay(night)),"wall lamp scale follows the shared curve");
        // the lot pass: c3.x = ground gain x LampDay before full night, the previous constants at night
        const LotPassConstants lot=LotPass(night,gain,1.3f);
        Check(lot.day==(n<100)&&(n==100||(lot.c3x==gain*LampDay(night)&&lot.lotGain==1.3f)),"lot pass follows the shared curve, previous constants at night");
    }
    Check(!LotPass(.5f,std::numeric_limits<float>::quiet_NaN(),1).day&&!LotPass(std::numeric_limits<float>::quiet_NaN(),1,1).day&&!LotPass(.5f,1,-1).day,"invalid lot pass input keeps the previous constants");
    Check(SolveLampDay(0)==kLampDayFloor&&SolveLampDay(.005f)==kLampDayFloor&&SolveLampDay(.995f)==1.f&&SolveLampDay(1)==1.f&&SolveLampDay(.5f)==LampDay(.5f),"room solves take exact endpoint factors, the curve at twilight");
    for (int n = 0; n <= 100; ++n)
        for (int g = 1; g <= 120; ++g)
            for (int v = 0; v <= 20; ++v) {
                const float night = n / 100.f, gain = g / 40.f, native = v / 20.f;
                const float single = LampScale(native, night, gain, false), multi = LampScale(native, night, gain, true);
                Check(single >= 0 && std::isfinite(single), "finite single-pass scale");
                Check(multi >= 0 && std::isfinite(multi), "finite squared scale");
                if (n == 100) {
                    const float previousWeighted = 1.f + (gain - 1.f) * night;
                    Check(single == native * previousWeighted, "exact previous night scale");
                    Check(multi == native * std::sqrt(previousWeighted), "exact previous squared night scale");
                }
                if (n < 100) {
                    Check(single == gain * LampDay(night), "terrain lamp scale follows the shared curve before full night");
                    Check(Near(multi * multi, gain * LampDay(night)), "squared terrain lamp scale follows the shared curve");
                }
                if (v == 0 && n == 0) {
                    Check(Near(single, gain * kLampDayFloor), "captured c7.x=0 by day: 3% of the night lamp light");
                    Check(Near(multi * multi, gain * kLampDayFloor), "captured c3.x=0 by day: 3% of the night lamp light");
                }
            }
    for (unsigned bits = 0; bits < 32; ++bits) {
        const bool night = bits & 1, automatic = bits & 2, user = bits & 4, force = bits & 8, world = bits & 16;
        Check(DeferDayEdit(night, automatic, user, force, world) == (!night && automatic && !user && !force && !world), "edit priority truth table");
    }
    Check(!DeferDayEdit(false, true, true, false, false), "regression: daytime lot placement must not be discarded");
    Check(!DeferDayEdit(false, true, false, true, false), "regression: daytime feature toggle must not be discarded");
    Check(LampScale(-1, 0, .75f, false) == -1, "unknown negative native scale unchanged");
    Check(std::isnan(LampScale(std::numeric_limits<float>::quiet_NaN(), 0, 1, true)), "unknown NaN scale unchanged");
    for (bool squared : {false, true}) {
        Check(LampScale(0, 0, 0, squared) == 0, "zero gain emits no daytime lamps");
        Check(LampScale(1, 1, 0, squared) == 0, "zero gain emits no night lamps");
        Check(LampScale(1, 0, -1, squared) == 1, "invalid negative gain leaves native shader scale");
        Check(LampScale(1, 0, std::numeric_limits<float>::infinity(), squared) == 1, "infinite gain leaves native shader scale");
    }
    Check(GroundLampTotal(0, std::numeric_limits<float>::quiet_NaN()) == 0, "invalid ground daylight gain cannot inject NaN");
    Check(GroundLampTotal(std::numeric_limits<float>::quiet_NaN(), 1) == 0, "invalid ground daylight level cannot inject NaN");
    Cycle c; Phase output;
    c.Reset(0); c.Observe(1, 0, 2000, true, false);
    Check(c.pending && !c.Consume(1999, output), "dusk debounce");
    Check(c.Consume(2000, output) && output == Phase::Night, "dusk endpoint consumed");
    Check(!c.Consume(2001, output), "dusk is consumed once");
    c.Observe(0, 2100, 2000, true, false);
    Check(c.Consume(4100, output) && output == Phase::Day, "regression: return to day reconciles maps");
    c.Observe(1, 4200, 2000, true, false); c.Observe(.5f, 4300, 2000, true, false);
    Check(!c.pending && !c.Consume(10000, output), "reversal cancels stale night rebuild");
    c.Observe(0, 10000, 2000, true, false); c.Observe(0, 10001, 2000, true, true);
    Check(!c.pending, "load merges phase rebuild");
    c.Observe(1, 10002, 0, false, false); Check(!c.pending, "disabled automatic rebuild remains disabled");
    Check(PhaseDelay(2000, false) == 2000, "Live mode retains saved phase delay");
    Check(PhaseDelay(2000, true) == 0, "Build preview removes the captured two-second wait");
    Check(PhaseDelay(-1, false) == 0, "negative configured delay is bounded");
    for (size_t chunks = 0; chunks <= 256; ++chunks) {
        Check(PreviewPriorityChunks(chunks, true, true) == std::min<size_t>(chunks, 4), "preview priority is bounded to four chunks");
        Check(PreviewPriorityChunks(chunks, false, true) == 0, "Live sweep receives no preview priority");
        Check(PreviewPriorityChunks(chunks, true, false) == 0, "unknown camera receives no arbitrary preview priority");
    }
    // Replay the actual captured endpoint sequence with its timestamps. Old
    // production held each endpoint for 2s; Build scheduling now consumes it in
    // the same observation, without speeding up the game's native TOD fade.
    c.Reset(1);
    c.Observe(.8f, 35782, PhaseDelay(2000, true), true, false);
    Check(!c.pending, "captured twilight does not request an intermediate rebuild");
    c.Observe(0, 36438, PhaseDelay(2000, true), true, false);
    Check(c.Consume(36438, output) && output == Phase::Day, "captured daylight endpoint schedules in the same frame");
    c.Observe(.2f, 39954, PhaseDelay(2000, true), true, false);
    c.Observe(1, 40501, PhaseDelay(2000, true), true, false);
    Check(c.Consume(40501, output) && output == Phase::Night, "captured night reversal schedules in the same frame");
    c.Reset(0);
    c.Observe(1, 10, PhaseDelay(2000, true), true, true);
    Check(!c.pending, "immediate Build scheduling still merges into world load");
    c.Reset(0);
    std::mt19937 rng(713);
    for (int64_t frame = 0; frame < 200000; ++frame) {
        const float level = (rng() % 3) * .5f;
        c.Observe(level, frame * 16, 100, true, false);
        if (c.Consume(frame * 16, output)) {
            Check(output == LevelPhase(level), "stress: only current endpoint may rebuild");
            Check(!c.Consume(frame * 16, output), "stress: repeated frames cannot duplicate work");
        }
        Check(!c.pending || c.target == LevelPhase(level), "stress: no stale target");
    }
}

struct MockDevice {
    DWORD states[16][11]{};
    IDirect3DBaseTexture9* textures[16]{};
    int reads = 0, failRead = -1, writes = 0, failWrite = -1, textureWrites = 0;
    bool failTextureRead = false, failTextureWrite = false;
    HRESULT GetTexture(DWORD s, IDirect3DBaseTexture9** p) {
        if (failTextureRead) return E_FAIL;
        *p = textures[s]; return S_OK;
    }
    HRESULT GetSamplerState(DWORD s, D3DSAMPLERSTATETYPE t, DWORD* p) {
        if (reads++ == failRead) return E_FAIL;
        *p = states[s][t - 1]; return S_OK;
    }
    HRESULT SetSamplerState(DWORD s, D3DSAMPLERSTATETYPE t, DWORD v) {
        if (writes++ == failWrite) return E_FAIL;
        states[s][t - 1] = v; return S_OK;
    }
};
HRESULT MockSet(MockDevice* d, DWORD s, IDirect3DBaseTexture9* p) {
    ++d->textureWrites;
    if (d->failTextureWrite) return E_FAIL;
    d->textures[s] = p; return S_OK;
}
void SamplerRestoration() {
    for (int failure = -1; failure < 33; ++failure) {
        MockDevice d;
        for (int i = 0; i < 11; ++i) { d.states[2][i] = DWORD(i + 10); d.states[3][i] = DWORD(i + 50); }
        std::array<DWORD, 11> before; std::memcpy(before.data(), d.states[3], sizeof d.states[3]);
        if (failure < 22) d.failRead = failure; else d.failWrite = failure - 22;
        auto* native = reinterpret_cast<IDirect3DBaseTexture9*>(uintptr_t(1));
        {
            NativeTerrainSampler guard(&d, 2, 3, native, MockSet);
            Check(bool(guard) == (failure == -1), "sampler partial failures fail closed");
            if (guard) Check(d.textures[3] == native && !std::memcmp(d.states[2], d.states[3], sizeof d.states[2]), "exact native sampler alias");
        }
        Check(!std::memcmp(before.data(), d.states[3], sizeof d.states[3]), "every sampler state restored after success/failure");
        Check(d.textures[3] == nullptr, "texture binding restored");
    }
    MockDevice d; auto* native = reinterpret_cast<IDirect3DBaseTexture9*>(uintptr_t(1));
    { NativeTerrainSampler guard(&d, 2, 3, native, MockSet); Check(bool(guard), "same sampler states accepted"); }
    Check(d.writes == 0 && d.textureWrites == 2, "unchanged sampler states generate no writes");
    for (bool getFailure : {false, true}) {
        MockDevice failed;
        for (int i = 0; i < 11; ++i) { failed.states[2][i] = DWORD(i + 10); failed.states[3][i] = DWORD(i + 50); }
        std::array<DWORD, 11> before; std::memcpy(before.data(), failed.states[3], sizeof failed.states[3]);
        failed.failTextureRead = getFailure; failed.failTextureWrite = !getFailure;
        { NativeTerrainSampler guard(&failed, 2, 3, native, MockSet); Check(!guard, "texture Get/Set failure rejects native alias"); }
        Check(!std::memcmp(before.data(), failed.states[3], sizeof failed.states[3]) && failed.textures[3] == nullptr,
              "texture failure restores partial sampler changes without changing texture");
        Check(getFailure ? failed.writes == 0 && failed.textureWrites == 0 : failed.textureWrites == 1,
              "failed texture operation performs no extra texture writes");
    }
}

struct Gpu {
    HWND window = nullptr;
    IDirect3D9* api = nullptr;
    IDirect3DDevice9* dev = nullptr;
    IDirect3DSurface9 *target = nullptr, *readback = nullptr;
    Gpu() {
        window = CreateWindowExW(0, L"STATIC", L"Apex terrain checks", WS_OVERLAPPED, 0, 0, 64, 64, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        api = Direct3DCreate9(D3D_SDK_VERSION);
        D3DPRESENT_PARAMETERS p{}; p.Windowed = TRUE; p.SwapEffect = D3DSWAPEFFECT_DISCARD; p.hDeviceWindow = window;
        p.BackBufferFormat = D3DFMT_UNKNOWN; p.BackBufferWidth = 64; p.BackBufferHeight = 64;
        if (api) api->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &p, &dev);
        Check(dev != nullptr, "native D3D9 device available (required GPU validation)");
        if (!dev) return;
        D3DADAPTER_IDENTIFIER9 id{}; api->GetAdapterIdentifier(0, 0, &id); std::printf("D3D9 adapter: %s\n", id.Description);
        Check(SUCCEEDED(dev->CreateRenderTarget(64, 64, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &target, nullptr)), "readback target");
        Check(SUCCEEDED(dev->CreateOffscreenPlainSurface(64, 64, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &readback, nullptr)), "readback surface");
        std::printf("GPU targets ready\n");
        dev->SetRenderTarget(0, target); dev->SetRenderState(D3DRS_ZENABLE, FALSE); dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE); dev->SetRenderState(D3DRS_LIGHTING, FALSE);
        dev->SetRenderState(D3DRS_DITHERENABLE, FALSE); dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
        std::printf("GPU draw state ready\n");
    }
    ~Gpu() { Release(target); Release(readback); Release(dev); Release(api); if (window) DestroyWindow(window); }
    IDirect3DTexture9* Texture(bool smooth) {
        IDirect3DTexture9* t = nullptr;
        if (FAILED(dev->CreateTexture(64, 64, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, nullptr))) return nullptr;
        D3DLOCKED_RECT r{}; t->LockRect(0, &r, nullptr, 0);
        for (int y = 0; y < 64; ++y) for (int x = 0; x < 64; ++x)
            reinterpret_cast<DWORD*>(static_cast<char*>(r.pBits) + y * r.Pitch)[x] = smooth ? 0xDB336699u : ((DWORD(20 + x * 3) << 24) | 0xAABBCCu);
        t->UnlockRect(0); return t;
    }
    IDirect3DPixelShader9* Shader(const std::vector<DWORD>& code) {
        IDirect3DPixelShader9* s = nullptr;
        Check(!code.empty() && SUCCEEDED(dev->CreatePixelShader(code.data(), &s)), "D3D9 accepts shader bytecode"); return s;
    }
    std::vector<DWORD> Render(IDirect3DPixelShader9* shader, IDirect3DVertexShader9* vs = nullptr) {
        struct Vertex { float x, y, z, rhw, u, v; };
        Vertex q[4] = {{-.5f, -.5f, 0, 1, 0, 0}, {63.5f, -.5f, 0, 1, 1, 0}, {-.5f, 63.5f, 0, 1, 0, 1}, {63.5f, 63.5f, 0, 1, 1, 1}};
        IDirect3DVertexDeclaration9* decl = nullptr;
        if (vs) {
            q[0].x = q[2].x = -1; q[1].x = q[3].x = 1; q[0].y = q[1].y = 1; q[2].y = q[3].y = -1;
            const D3DVERTEXELEMENT9 elements[] = {{0, 0, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
                                                {0, 16, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0}, D3DDECL_END()};
            Check(SUCCEEDED(dev->CreateVertexDeclaration(elements, &decl)), "test vertex layout"); dev->SetVertexDeclaration(decl);
        } else dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
        dev->SetVertexShader(vs); dev->SetPixelShader(shader);
        dev->BeginScene(); Check(SUCCEEDED(dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(Vertex))), "test quad draw"); dev->EndScene();
        Check(SUCCEEDED(dev->GetRenderTargetData(target, readback)), "GPU readback");
        std::vector<DWORD> pixels(4096); D3DLOCKED_RECT r{};
        if (SUCCEEDED(readback->LockRect(&r, nullptr, D3DLOCK_READONLY))) {
            for (int y = 0; y < 64; ++y) std::memcpy(pixels.data() + y * 64, static_cast<char*>(r.pBits) + y * r.Pitch, 256);
            readback->UnlockRect();
        } else Check(false, "readback lock");
        Release(decl); return pixels;
    }
};

void Patches(Gpu& gpu, const std::filesystem::path& captures) {
    for (const char* profile : {"ps_2_0", "ps_3_0"}) {
        std::printf("Testing profile %s\n", profile);
        auto code = Compile("sampler2D light:register(s2); float4 gain:register(c0); float4 main(float2 uv:TEXCOORD0):COLOR0{float4 v=tex2D(light,uv);return float4(v.rgb*gain.x,v.a);}", profile);
        if (code.empty()) continue;
        const auto original = code;
        DWORD spare = 999;
        const bool matched = ShaderPatches::PatchTerrainNativeAlpha(code, 2, spare);
        Check(matched, "SM2/SM3 native alpha patch");
        if (!matched) continue;
        Check(spare == 3, "first free sampler used");
        auto bad = original; bad.pop_back(); const auto saved = bad;
        Check(!ShaderPatches::PatchTerrainNativeAlpha(bad, 2, spare) && bad == saved, "truncated shader untouched");
        for (size_t size = 0; size < original.size(); ++size) {
            bad.assign(original.begin(), original.begin() + size); const auto before = bad;
            Check(!ShaderPatches::PatchTerrainNativeAlpha(bad, 2, spare) && bad == before, "every truncated instruction fails closed");
        }
        bad = original; Check(!ShaderPatches::PatchTerrainNativeAlpha(bad, 15, spare) && bad == original, "missing sampler unchanged");
        auto refuse = [&](std::vector<DWORD> candidate, const char* why) {
            const auto before = candidate; DWORD unchanged = 999;
            Check(!ShaderPatches::PatchTerrainNativeAlpha(candidate, 2, unchanged) && candidate == before && unchanged == 999, why);
        };
        bad = original; bad[0] = 0xFFFE0300; refuse(bad, "vertex shader never patched");
        bad = original; bad[0] = 0xFFFF0101; refuse(bad, "SM1 never patched");
        for (size_t i = 1; i < original.size();) {
            const DWORD tok = original[i];
            if (tok == 0xFFFF) break;
            if ((tok & 0xFFFF) == 0xFFFE) { i += 1 + ((tok >> 16) & 0x7FFF); continue; }
            const size_t len = (tok >> 24) & 15;
            if ((tok & 0xFFFF) == 0x42 && len == 3) {
                bad = original; bad[i + 1] = (bad[i + 1] & ~0xF0000u) | 0x70000u; refuse(bad, "partial light fetch rejected");
                bad = original; bad[i + 2] |= 0x2000; refuse(bad, "relative UV addressing rejected");
                bad = original; bad[i] |= 0x10000; refuse(bad, "projected texture fetch rejected");
                bad = original; bad[i + 1] = (bad[i + 1] & ~0x7FFu) | (std::string(profile) == "ps_2_0" ? 11u : 31u); refuse(bad, "exhausted temporary registers rejected");
                bad = original; bad.insert(bad.begin() + i, original.begin() + i, original.begin() + i + 4); refuse(bad, "ambiguous repeated light-map fetch rejected");
                break;
            }
            i += 1 + len;
        }
        if (!gpu.dev) continue;
        auto* base = gpu.Shader(original); auto* patched = gpu.Shader(code);
        const float gain[4] = {1, 0, 0, 0}; gpu.dev->SetPixelShaderConstantF(0, gain, 1);
        auto* native = gpu.Texture(false); auto* smooth = gpu.Texture(true);
        std::printf("GPU shader and textures ready\n");
        gpu.dev->SetTexture(2, native);
        for (DWORD state = 1; state <= 11; ++state) {
            // Native filters differ from the smooth sampler; the alias must clone them.
            if (state == D3DSAMP_MINFILTER || state == D3DSAMP_MAGFILTER) gpu.dev->SetSamplerState(2, static_cast<D3DSAMPLERSTATETYPE>(state), D3DTEXF_POINT);
        }
        gpu.dev->SetSamplerState(2, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        const auto nativePixels = gpu.Render(base);
        gpu.dev->SetTexture(2, smooth); const auto smoothPixels = gpu.Render(base); gpu.dev->SetTexture(2, native);
        gpu.dev->SetTexture(spare, smooth); // exercise a real previous texture reference, not just an empty spare slot
        {
            NativeTerrainSampler guard(gpu.dev, 2, spare, native, [](auto* d, DWORD slot, auto* t) { return d->SetTexture(slot, t); });
            Check(bool(guard), "real device native sampler alias"); gpu.dev->SetTexture(2, smooth);
            const auto result = gpu.Render(patched);
            for (size_t p = 0; p < result.size(); ++p) {
                Check((result[p] & 0xFFFFFFu) == (smoothPixels[p] & 0xFFFFFFu), "lamp RGB unchanged by native-alpha patch");
                Check((result[p] & 0xFF000000u) == (nativePixels[p] & 0xFF000000u), "solar alpha equals original at every pixel");
            }
            gpu.dev->SetTexture(2, native);
        }
        IDirect3DBaseTexture9* restored = nullptr;
        Check(SUCCEEDED(gpu.dev->GetTexture(spare, &restored)) && restored == smooth, "real previous spare texture binding restored");
        Release(restored);
        // Walls' shade on the world grass (PatchTerrainShade, on top of the native alpha patch): lamp RGB x the shade map's
        // value at the light map's uv, solar alpha untouched
        {
            auto shaded = code; DWORD shadeSampler = 999, ignored = 999;
            Check(ShaderPatches::PatchTerrainShade(shaded, 2, shadeSampler) && shadeSampler == spare + 1, "shade patch after native alpha, next free sampler");
            auto missing = code; const auto before = missing;
            Check(!ShaderPatches::PatchTerrainShade(missing, 15, ignored) && missing == before && ignored == 999, "shade patch: missing sampler unchanged");
            for (size_t size = 0; size < code.size(); ++size) {
                std::vector<DWORD> cut(code.begin(), code.begin() + size); const auto kept = cut;
                Check(!ShaderPatches::PatchTerrainShade(cut, 2, ignored) && cut == kept, "shade patch: every truncated instruction fails closed");
            }
            if (gpu.dev) {
                IDirect3DTexture9* map = nullptr;
                Check(SUCCEEDED(gpu.dev->CreateTexture(4, 4, 1, 0, D3DFMT_L8, D3DPOOL_MANAGED, &map, nullptr)), "L8 shade texture");
                D3DLOCKED_RECT lr;
                if (map && SUCCEEDED(map->LockRect(0, &lr, nullptr, 0))) {
                    for (int j = 0; j < 4; j++) std::memset(static_cast<BYTE*>(lr.pBits) + j * lr.Pitch, 128, 4);
                    map->UnlockRect(0);
                }
                auto* shadedPs = gpu.Shader(shaded);
                {
                    NativeTerrainSampler guard(gpu.dev, 2, spare, native, [](auto* d, DWORD slot, auto* t) { return d->SetTexture(slot, t); });
                    gpu.dev->SetTexture(2, smooth);
                    gpu.dev->SetTexture(shadeSampler, map);
                    const auto result = gpu.Render(shadedPs);
                    int worst = 0;
                    for (size_t px = 0; px < result.size(); ++px) {
                        for (int ch = 0; ch < 3; ch++) {
                            const int want = static_cast<int>(std::lround(((smoothPixels[px] >> (8 * ch)) & 0xFF) * 128.0 / 255.0));
                            worst = std::max(worst, std::abs(static_cast<int>((result[px] >> (8 * ch)) & 0xFF) - want));
                        }
                        Check((result[px] & 0xFF000000u) == (nativePixels[px] & 0xFF000000u), "shade patch: solar alpha equals original");
                    }
                    Check(worst <= 2, "shade patch: lamp RGB x shade (within 2 of 255)");
                    std::printf("Wall shade patch (%s): sampler s%lu, worst RGB error %d of 255\n", profile, shadeSampler, worst);
                    gpu.dev->SetTexture(shadeSampler, nullptr);
                    gpu.dev->SetTexture(2, native);
                }
                Release(shadedPs); Release(map);
            }
        }
        // CPU guard cost with the real driver, no game hooks. The render tests
        // above provide correctness; this small benchmark cannot predict FPS.
        const auto begin = std::chrono::steady_clock::now();
        for (int i = 0; i < 5000; ++i) {
            NativeTerrainSampler guard(gpu.dev, 2, spare, native, [](auto* d, DWORD slot, auto* t) { return d->SetTexture(slot, t); });
            if (!guard) { Check(false, "benchmark sampler alias"); break; }
        }
        const double micros = std::chrono::duration<double, std::micro>(std::chrono::steady_clock::now() - begin).count() / 5000;
        std::printf("Native alpha sampler guard (%s): %.3f us/call, real-driver CPU only; one extra shader texture read\n", profile, micros);
        gpu.dev->SetTexture(2, nullptr); gpu.dev->SetTexture(spare, nullptr);
        Release(base); Release(patched); Release(native); Release(smooth);
    }
    // Validate the actual world single/multi-pass shaders from in-game captures.
    int found = 0;
    if (std::filesystem::exists(captures)) for (const auto& e : std::filesystem::recursive_directory_iterator(captures)) {
        const std::string name = e.path().filename().string();
        DWORD sampler;
        if (name == "PS_8D0F5300.bin") { // 2026-10-14 23-27-59 F7: the multi-pass world shader as drawn (native alpha in, s2)
            std::ifstream f(e.path(), std::ios::binary); std::vector<char> raw((std::istreambuf_iterator<char>(f)), {});
            std::vector<DWORD> code(raw.size() / 4); if (!raw.empty()) std::memcpy(code.data(), raw.data(), code.size() * 4);
            DWORD shadeSampler = 999;
            Check(ShaderPatches::PatchTerrainShade(code, 2, shadeSampler) && shadeSampler == 7, "drawn multi-pass shader takes the wall shade (s7)");
            if (gpu.dev) { auto* shader = gpu.Shader(code); Release(shader); }
            std::printf("Drawn shader %s: wall shade s%lu, %zu -> %zu tokens\n", name.c_str(), shadeSampler, raw.size() / 4, code.size());
            continue;
        }
        if (name == "PS_2BA33118.bin") sampler = 7;
        else if (name == "PS_2BA62940.bin") sampler = 2;
        else continue;
        std::ifstream f(e.path(), std::ios::binary); std::vector<char> raw((std::istreambuf_iterator<char>(f)), {});
        Check(raw.size() % 4 == 0 && raw.size() >= 8, "capture bytecode size");
        std::vector<DWORD> code(raw.size() / 4); std::memcpy(code.data(), raw.data(), raw.size());
        DWORD spare = 999; const auto original = code;
        Check(ShaderPatches::PatchTerrainNativeAlpha(code, sampler, spare), "captured terrain shader matches native alpha patch");
        if (name == "PS_2BA33118.bin") Check(ShaderPatches::LightMapScaleConst(original, sampler) == 7, "captured exclusive lamp constant c7");
        ID3DBlob* dis = nullptr;
        Check(SUCCEEDED(D3DDisassemble(code.data(), code.size() * 4, 0, nullptr, &dis)), "captured patched bytecode disassembles"); Release(dis);
        if (gpu.dev) { auto* shader = gpu.Shader(code); Release(shader); }
        DWORD shadeSampler = 999; auto shaded = code;
        Check(ShaderPatches::PatchTerrainShade(shaded, sampler, shadeSampler) && shadeSampler == spare + 1, "captured terrain shader takes the wall shade");
        if (gpu.dev) { auto* shader = gpu.Shader(shaded); Release(shader); }
        std::printf("Captured shader %s: %zu -> %zu tokens; native alpha sampler s%lu; wall shade s%lu (%zu tokens)\n", name.c_str(), original.size(), code.size(), spare,
                    shadeSampler, shaded.size());
        ++found;
    }
    Check(found >= 2, "both captured terrain variants tested");
}

std::string LotHlsl(const std::filesystem::path& source) {
    std::ifstream f(source); const std::string text((std::istreambuf_iterator<char>(f)), {});
    const auto at = text.find("const char* kReplacementHlsl = R\"(");
    if (at == std::string::npos) { Check(false, "production lot shader source found"); return {}; }
    const auto start = text.find('\n', at), end = text.find(")\";", start);
    if (end == std::string::npos) { Check(false, "production lot shader delimiter"); return {}; }
    return text.substr(start, end - start);
}
void LotPixels(Gpu& gpu, const std::filesystem::path& current, const std::filesystem::path& reference) {
    auto oldCode = Compile(LotHlsl(reference), "ps_3_0"), newCode = Compile(LotHlsl(current), "ps_3_0");
    if (!gpu.dev || oldCode.empty() || newCode.empty()) return;
    auto* oldPs = gpu.Shader(oldCode); auto* newPs = gpu.Shader(newCode);
    const auto vertex = Compile(R"(
struct Out {float4 pos:POSITION; float4 shadow:TEXCOORD2; float3 normal:TEXCOORD4; float2 lot:TEXCOORD5; float3 terrain:TEXCOORD1;};
Out main(float4 pos:POSITION,float2 uv:TEXCOORD0){Out o;o.pos=pos;o.shadow=float4(.5,.5,0,1);o.normal=float3(0,1,0);o.lot=uv;o.terrain=float3(uv,0);return o;}
)", "vs_3_0");
    IDirect3DVertexShader9* vs = nullptr; Check(!vertex.empty() && SUCCEEDED(gpu.dev->CreateVertexShader(vertex.data(), &vs)), "lot test vertex shader");
    auto* terrain = gpu.Texture(true); auto* lot = gpu.Texture(false); auto* shadow = gpu.Texture(false);
    gpu.dev->SetTexture(1, lot); gpu.dev->SetTexture(2, terrain); gpu.dev->SetTexture(5, shadow);
    for (DWORD slot : {1ul, 2ul, 5ul}) {
        gpu.dev->SetSamplerState(slot, D3DSAMP_MINFILTER, D3DTEXF_POINT); gpu.dev->SetSamplerState(slot, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        gpu.dev->SetSamplerState(slot, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    }
    const float c0[4] = {.1f, .2f, .3f, 0}, c1[4] = {0, 1, 0, 0}, zero[4]{};
    const float x[4] = {1, 0, 0, 1}, z[4] = {0, 1, 0, 1};
    gpu.dev->SetPixelShaderConstantF(0, c0, 1); gpu.dev->SetPixelShaderConstantF(1, c1, 1);
    gpu.dev->SetPixelShaderConstantF(2, zero, 1); gpu.dev->SetPixelShaderConstantF(4, zero, 1);
    gpu.dev->SetPixelShaderConstantF(28, x, 1); gpu.dev->SetPixelShaderConstantF(29, z, 1);
    for (float scale : {0.f, .125f, .5f, 1.f, 2.f}) for (float gain : {.25f, .75f, 1.f, 2.f}) for (int feather = 0; feather < 3; ++feather) {
        const float c3[4] = {scale, 0, 0, 0}, c31[4] = {gain, 0, 0, 0};
        const float edge[4] = {feather == 2 ? 3.f : 0.f, feather == 1 ? 1.f : 0.f, 0, 0};
        gpu.dev->SetPixelShaderConstantF(3, c3, 1); gpu.dev->SetPixelShaderConstantF(30, edge, 1); gpu.dev->SetPixelShaderConstantF(31, c31, 1);
        const auto before = gpu.Render(oldPs, vs), after = gpu.Render(newPs, vs);
        for (size_t p = 0; p < before.size(); ++p) for (int channel = 0; channel < 3; ++channel)
            Check(std::abs(int((before[p] >> (channel * 8)) & 255) - int((after[p] >> (channel * 8)) & 255)) <= 1, "actual lot HLSL preserves night pixels (one LSB tolerance)");
    }
    // At day, native c3 remains zero: the window/lot map must stay native, while
    // the added terrain lamp contribution must be independent of lot-edge weight.
    const float c3[4] = {0, 0, 0, 0}, c31[4] = {1, .75f, 0, 0};
    gpu.dev->SetPixelShaderConstantF(3, c3, 1); gpu.dev->SetPixelShaderConstantF(31, c31, 1);
    std::vector<DWORD> boundary;
    for (float weight : {0.f, 1.f}) {
        const float edge[4] = {0, weight, 0, 0}; gpu.dev->SetPixelShaderConstantF(30, edge, 1);
        const auto pixels = gpu.Render(newPs, vs);
        if (boundary.empty()) boundary = pixels; else Check(boundary == pixels, "daylight lamp term continuous across lot border");
        const auto old = gpu.Render(oldPs, vs);
        Check(pixels[2000] != old[2000], "daylight terrain lamps no longer multiplied by zero");
    }
    // The lamp shade (13/10, the mask's alpha): 0 leaves exactly the pass without lamps (the lot map, the atlas and the soft
    // edge's target all go), 255 the full lamp term (as with no shade)
    {
        IDirect3DTexture9* mask = nullptr;
        Check(SUCCEEDED(gpu.dev->CreateTexture(1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &mask, nullptr)), "lot shade mask texture");
        const auto fillMask = [&](DWORD v) { D3DLOCKED_RECT r{}; mask->LockRect(0, &r, nullptr, 0); *static_cast<DWORD*>(r.pBits) = v; mask->UnlockRect(0); };
        gpu.dev->SetTexture(3, mask);
        gpu.dev->SetSamplerState(3, D3DSAMP_MINFILTER, D3DTEXF_POINT); gpu.dev->SetSamplerState(3, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
        const float lit3[4] = {1, 0, 0, 0}, lit31[4] = {1.5f, 0, 0, 0}, dark3[4] = {0, 0, 0, 0}, dark31[4] = {0, 0, 0, 0};
        for (float feather : {0.f, 1.f}) {
            const float edge[4] = {feather, 1 - feather, 0, 0}; gpu.dev->SetPixelShaderConstantF(30, edge, 1);
            fillMask(0x00FFFFFFu);
            gpu.dev->SetPixelShaderConstantF(3, lit3, 1); gpu.dev->SetPixelShaderConstantF(31, lit31, 1);
            const auto shaded = gpu.Render(newPs, vs);
            fillMask(0xFFFFFFFFu);
            gpu.dev->SetPixelShaderConstantF(3, dark3, 1); gpu.dev->SetPixelShaderConstantF(31, dark31, 1);
            const auto noLamps = gpu.Render(newPs, vs);
            gpu.dev->SetPixelShaderConstantF(3, lit3, 1); gpu.dev->SetPixelShaderConstantF(31, lit31, 1);
            const auto full = gpu.Render(newPs, vs);
            Check(shaded == noLamps, "lot shade 0 removes the whole lamp term");
            Check(full != noLamps, "lot shade 1 keeps the lamp term");
        }
        gpu.dev->SetTexture(3, nullptr);
        Release(mask);
    }
    for (DWORD slot : {1ul, 2ul, 5ul}) gpu.dev->SetTexture(slot, nullptr);
    gpu.dev->SetVertexShader(nullptr); gpu.dev->SetPixelShader(nullptr);
    Release(oldPs); Release(newPs); Release(vs); Release(terrain); Release(lot); Release(shadow);
    std::printf("Production lot HLSL: night equivalence, daytime border continuity and the lamp shade checked\n");
}

void WallPixels(Gpu& gpu, const std::filesystem::path& capture) {
    if (!gpu.dev) return;
    std::ifstream file(capture / "PS_265EBE18.bin", std::ios::binary | std::ios::ate);
    Check(bool(file), "actual daytime wall F7 shader available");
    if (!file) return;
    const auto size = file.tellg();
    Check(size == 1372, "captured ExteriorWall shader size");
    if (size != 1372) return;
    std::vector<DWORD> code(static_cast<size_t>(size) / 4);
    file.seekg(0); file.read(reinterpret_cast<char*>(code.data()), size);
    DWORD hash = 2166136261u; for (DWORD word : code) hash = (hash ^ word) * 16777619u;
    Check(hash == 0x04956FE9u, "actual wall matches recognized ExteriorWall_PS_1119, c3");
    if (hash != 0x04956FE9u) return;
    auto* ps = gpu.Shader(code);
    const auto vertex = Compile(R"(
struct Out {float4 pos:POSITION;float2 uv:TEXCOORD0;float4 map:TEXCOORD1;float4 normal:TEXCOORD2;
float4 mask:TEXCOORD3;float3 eye:TEXCOORD5;float3 tangent:TEXCOORD6;float4 shadow:TEXCOORD7;float4 a:COLOR0;float4 b:COLOR1;};
Out main(float4 pos:POSITION,float2 uv:TEXCOORD0){Out o;o.pos=pos;o.uv=uv;o.map=float4(uv,uv);
o.normal=float4(0,1,0,1);o.mask=float4(.5,.5,1,0);o.eye=float3(0,1,0);o.tangent=float3(1,0,0);
o.shadow=float4(.5,.5,0,1);o.a=float4(0,0,0,1);o.b=0;return o;}
)", "vs_3_0");
    IDirect3DVertexShader9* vs = nullptr;
    Check(!vertex.empty() && SUCCEEDED(gpu.dev->CreateVertexShader(vertex.data(), &vs)), "wall fixture vertex shader");
    IDirect3DCubeTexture9* cube = nullptr;
    Check(SUCCEEDED(gpu.dev->CreateCubeTexture(1,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&cube,nullptr)), "wall black sky cube");
    if (!ps || !vs || !cube) { Release(ps);Release(vs);Release(cube);return; }
    for(int face=0;face<6;++face){D3DLOCKED_RECT r{};cube->LockRect(static_cast<D3DCUBEMAP_FACES>(face),0,&r,nullptr,0);
        *static_cast<DWORD*>(r.pBits)=0xff000000u;cube->UnlockRect(static_cast<D3DCUBEMAP_FACES>(face),0);}
    auto* white=gpu.Texture(true);auto* lamp=gpu.Texture(true);auto* normal=gpu.Texture(true);
    auto fill=[](IDirect3DTexture9* texture,DWORD value){D3DLOCKED_RECT r{};texture->LockRect(0,&r,nullptr,0);
        for(int y=0;y<64;++y)for(int x=0;x<64;++x)reinterpret_cast<DWORD*>(static_cast<char*>(r.pBits)+y*r.Pitch)[x]=value;texture->UnlockRect(0);};
    fill(white,0xffffffffu);fill(normal,0x80808080u);
    gpu.dev->SetTexture(0,cube);gpu.dev->SetTexture(1,cube);
    for(DWORD slot=2;slot<=8;++slot){gpu.dev->SetTexture(slot,slot==2?lamp:slot==7?normal:white);
        gpu.dev->SetSamplerState(slot,D3DSAMP_MINFILTER,D3DTEXF_POINT);gpu.dev->SetSamplerState(slot,D3DSAMP_MAGFILTER,D3DTEXF_POINT);gpu.dev->SetSamplerState(slot,D3DSAMP_MIPFILTER,D3DTEXF_NONE);}
    float constants[11][4]{};constants[7][0]=1;constants[8][0]=1;
    gpu.dev->SetPixelShaderConstantF(0,constants[0],11);
    for(bool lit:{false,true})for(float night:{0.f,.5f,1.f})for(float gain:{1.f,2.f,4.f}){
        fill(lamp,lit?0xff201008u:0xff000000u);
        float scale[4]={night*gain,.188235313f,0,0};gpu.dev->SetPixelShaderConstantF(3,scale,1);
        const auto previous=gpu.Render(ps,vs);
        scale[0]=TerrainLightingPolicy::WallLampScale(night,night,gain);gpu.dev->SetPixelShaderConstantF(3,scale,1);
        const auto current=gpu.Render(ps,vs);
        if(!lit||night==1)Check(previous==current,"captured wall GPU: empty map and night pixels unchanged");
        const float expectedScale=night==1.f?night*gain:gain*TerrainLightingPolicy::LampDay(night);
        for(DWORD pixel:current)for(int channel=0;channel<3;++channel){const int expected=lit?int((32>>channel)*expectedScale):0;
            Check(std::abs(int((pixel>>(16-channel*8))&255)-expected)<=2,"captured wall GPU: lamp RGB follows the daytime curve");}
        if(lit&&night==0)Check((previous[2000]&0xffffffu)==0&&int((current[2000]>>16)&255)<=int(32*gain*TerrainLightingPolicy::kLampDayFloor)+1,"captured wall GPU by day: native 0, Apex at most the 3% floor");
    }
    for(DWORD slot=0;slot<=8;++slot)gpu.dev->SetTexture(slot,nullptr);
    gpu.dev->SetVertexShader(nullptr);gpu.dev->SetPixelShader(nullptr);
    Release(ps);Release(vs);Release(cube);Release(white);Release(lamp);Release(normal);
    std::printf("Actual F7 wall shader: day/twilight/night RGB checks complete\n");
}

void InstancedPixels(Gpu& gpu, const std::filesystem::path& capture) {
    if(!gpu.dev)return;
    std::ifstream file(capture/"PS_3019F738.bin",std::ios::binary|std::ios::ate);
    Check(bool(file),"captured instanced bench shader available");if(!file)return;
    const auto bytes=file.tellg();Check(bytes==912,"captured instanced bench bytecode size");if(bytes!=912)return;
    std::vector<DWORD> code(static_cast<size_t>(bytes)/4);file.seekg(0);file.read(reinterpret_cast<char*>(code.data()),bytes);
    DWORD hash=2166136261u;for(DWORD word:code)hash=(hash^word)*16777619u;
    Check(hash==0xC0F5CCD7u,"exact F7 instanced bench shader identity");if(hash!=0xC0F5CCD7u)return;
    auto* ps=gpu.Shader(code);
    const auto vertex=Compile(R"(
struct Out{float4 pos:POSITION;float4 a:COLOR0;float4 shadow:TEXCOORD0;float b:COLOR1;float4 uv:TEXCOORD1;
float3 normal:TEXCOORD2;float4 eye:TEXCOORD3;float2 spec:TEXCOORD6;};
Out main(float4 pos:POSITION,float2 uv:TEXCOORD0){Out o;o.pos=pos;o.a=0;o.shadow=float4(.5,.5,0,1);o.b=0;
o.uv=float4(uv,uv);o.normal=float3(0,1,0);o.eye=float4(0,1,0,0);o.spec=uv;return o;}
)","vs_3_0");
    IDirect3DVertexShader9* vs=nullptr;Check(!vertex.empty()&&SUCCEEDED(gpu.dev->CreateVertexShader(vertex.data(),&vs)),"instanced bench fixture VS");
    IDirect3DCubeTexture9* cube=nullptr;
    Check(SUCCEEDED(gpu.dev->CreateCubeTexture(1,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&cube,nullptr)),"instanced black sky cube");
    if(!ps||!vs||!cube){Release(ps);Release(vs);Release(cube);return;}
    for(int face=0;face<6;++face){D3DLOCKED_RECT r{};cube->LockRect(static_cast<D3DCUBEMAP_FACES>(face),0,&r,nullptr,0);
        *static_cast<DWORD*>(r.pBits)=0xff000000u;cube->UnlockRect(static_cast<D3DCUBEMAP_FACES>(face),0);}
    auto* white=gpu.Texture(true);auto* lamp=gpu.Texture(true);
    auto fill=[](IDirect3DTexture9* t,DWORD value){D3DLOCKED_RECT r{};t->LockRect(0,&r,nullptr,0);
        for(int y=0;y<64;++y)for(int x=0;x<64;++x)reinterpret_cast<DWORD*>(static_cast<char*>(r.pBits)+y*r.Pitch)[x]=value;t->UnlockRect(0);};
    fill(white,0xffffffffu);
    gpu.dev->SetTexture(0,cube);gpu.dev->SetTexture(1,cube);
    for(DWORD slot:{2ul,3ul,5ul,6ul}){gpu.dev->SetTexture(slot,slot==6?lamp:white);
        gpu.dev->SetSamplerState(slot,D3DSAMP_MINFILTER,D3DTEXF_POINT);gpu.dev->SetSamplerState(slot,D3DSAMP_MAGFILTER,D3DTEXF_POINT);gpu.dev->SetSamplerState(slot,D3DSAMP_MIPFILTER,D3DTEXF_NONE);}
    float constants[14][4]{};constants[0][0]=constants[0][1]=constants[0][2]=.2f;constants[1][1]=1;
    constants[2][0]=constants[2][1]=constants[2][2]=1;constants[4][0]=20;constants[5][0]=1;constants[7][0]=.5f;constants[12][0]=constants[12][1]=1;
    gpu.dev->SetPixelShaderConstantF(0,constants[0],14);
    for(bool lit:{false,true})for(float night:{0.f,.5f,1.f})for(float gain:{.993083f,1.f,2.f}){
        fill(lamp,lit?0xff201008u:0xff000000u);
        float strength[4]={gain,0,0,0};gpu.dev->SetPixelShaderConstantF(13,strength,1);const auto previous=gpu.Render(ps,vs);
        strength[0]=TerrainLightingPolicy::SurfaceLampGain(night,gain);gpu.dev->SetPixelShaderConstantF(13,strength,1);const auto current=gpu.Render(ps,vs);
        if(night==1||!lit)Check(previous==current,"actual bench GPU preserves night and lamp-free sunlight pixels");
        for(DWORD pixel:current)for(int channel=0;channel<3;++channel){const int expected=51+(lit?int((32>>channel)*strength[0]):0);
            Check(std::abs(int((pixel>>(16-channel*8))&255)-expected)<=2,"actual bench GPU separates lamp attenuation from sunlight");}
        if(lit&&night==0)Check((current[2000]&0xffffffu)<(previous[2000]&0xffffffu),"actual bench daytime lamp term is reduced");
    }
    for(DWORD slot:{0ul,1ul,2ul,3ul,5ul,6ul})gpu.dev->SetTexture(slot,nullptr);
    gpu.dev->SetPixelShader(nullptr);gpu.dev->SetVertexShader(nullptr);
    Release(ps);Release(vs);Release(cube);Release(white);Release(lamp);
    std::printf("Actual F7 instanced bench: daytime lamp reduction, native sunlight and exact night pixels checked\n");
}

void TerrainCompositionPixels(Gpu& gpu, const std::filesystem::path& capture) {
    if (!gpu.dev) return;
    auto read=[](const std::filesystem::path& p){std::ifstream f(p,std::ios::binary);std::vector<char> raw((std::istreambuf_iterator<char>(f)),{});
        std::vector<DWORD> t(raw.size()/4);if(!raw.empty())std::memcpy(t.data(),raw.data(),t.size()*4);return t;};
    auto singleCode=read(capture/"PS_4BE77B30.bin"), multiCode=read(capture/"PS_4BE76D20.bin");
    Check(singleCode.size()==387&&multiCode.size()==199,"latest F7 terrain shader sizes");
    auto* single=gpu.Shader(singleCode);auto* multi=gpu.Shader(multiCode);
    auto patchedCode=singleCode;DWORD dayConst=0;
    Check(ShaderPatches::PatchTerrainDaylightRange(patchedCode,dayConst)&&dayConst==223,"captured single matches studied light range patch");
    auto unsupported=multiCode;const auto untouched=unsupported;DWORD ignored=999;
    Check(!ShaderPatches::PatchTerrainDaylightRange(unsupported,ignored)&&unsupported==untouched&&ignored==999,"multi-pass remains untouched");
    auto malformed=singleCode;malformed[15]^=1;const auto unchanged=malformed;
    Check(!ShaderPatches::PatchTerrainDaylightRange(malformed,ignored)&&malformed==unchanged,"unstudied bytecode fails closed");
    auto* patched=gpu.Shader(patchedCode);
    if(!patched){ID3DBlob* listing=nullptr;D3DDisassemble(patchedCode.data(),patchedCode.size()*4,0,nullptr,&listing);
        if(listing){std::printf("%s\n",static_cast<char*>(listing->GetBufferPointer()));listing->Release();}}
    const auto sv=Compile(R"(
struct O{float4 p:POSITION;float4 a:TEXCOORD0;float4 b:TEXCOORD1;float2 c:TEXCOORD2;float4 d:TEXCOORD3;float3 e:TEXCOORD4;};
O main(float4 p:POSITION,float2 uv:TEXCOORD0){O o;o.p=p;o.a=float4(uv,uv);o.b=float4(uv,1,0);o.c=uv;o.d=float4(.5,.5,0,1);o.e=0;return o;}
)","vs_3_0");
    const auto mv=Compile(R"(
struct O{float4 p:POSITION;float2 a:TEXCOORD0;float3 b:TEXCOORD1;float4 c:TEXCOORD2;};
O main(float4 p:POSITION,float2 uv:TEXCOORD0){O o;o.p=p;o.a=uv;o.b=float3(uv,1);o.c=float4(.5,.5,0,1);return o;}
)","vs_3_0");
    IDirect3DVertexShader9 *singleVs=nullptr,*multiVs=nullptr;
    Check(SUCCEEDED(gpu.dev->CreateVertexShader(sv.data(),&singleVs)),"terrain single fixture VS");
    Check(SUCCEEDED(gpu.dev->CreateVertexShader(mv.data(),&multiVs)),"terrain multi fixture VS");
    IDirect3DCubeTexture9* cube=nullptr;
    Check(SUCCEEDED(gpu.dev->CreateCubeTexture(1,1,0,D3DFMT_A8R8G8B8,D3DPOOL_MANAGED,&cube,nullptr)),"terrain sky cube");
    if(!single||!multi||!patched||!singleVs||!multiVs||!cube){Release(single);Release(multi);Release(patched);Release(singleVs);Release(multiVs);Release(cube);return;}
    for(int face=0;face<6;++face){D3DLOCKED_RECT r{};cube->LockRect(static_cast<D3DCUBEMAP_FACES>(face),0,&r,nullptr,0);
        *static_cast<DWORD*>(r.pBits)=0xff000000;cube->UnlockRect(static_cast<D3DCUBEMAP_FACES>(face),0);}
    auto* normal=gpu.Texture(true);auto* material=gpu.Texture(true);auto* mask=gpu.Texture(true);
    auto* white=gpu.Texture(true);auto* detail=gpu.Texture(true);auto* lamp=gpu.Texture(true);
    auto fill=[](IDirect3DTexture9* t,DWORD v){D3DLOCKED_RECT r{};t->LockRect(0,&r,nullptr,0);
        for(int y=0;y<64;++y)for(int x=0;x<64;++x)reinterpret_cast<DWORD*>(static_cast<char*>(r.pBits)+y*r.Pitch)[x]=v;t->UnlockRect(0);};
    fill(normal,0x80808080);fill(material,0x00333333);fill(mask,0);fill(white,0xffffffff);fill(detail,0xff808080);
    for(DWORD slot=0;slot<10;++slot){gpu.dev->SetSamplerState(slot,D3DSAMP_MINFILTER,D3DTEXF_POINT);
        gpu.dev->SetSamplerState(slot,D3DSAMP_MAGFILTER,D3DTEXF_POINT);gpu.dev->SetSamplerState(slot,D3DSAMP_MIPFILTER,D3DTEXF_NONE);}
    for(int rgb:{0,32,64,128,192,255}){
        fill(lamp,0xff000000u|DWORD(rgb)*0x010101u);
        float c[32][4]{};c[0][0]=c[0][1]=c[0][2]=1.35f;c[1][1]=1;c[4][1]=1;c[4][3]=2.5f;c[7][0]=1.21467388f;c[11][1]=1;c[12][2]=1;
        gpu.dev->SetPixelShaderConstantF(0,c[0],32);gpu.dev->SetTexture(0,cube);gpu.dev->SetTexture(1,normal);
        for(DWORD slot:{2ul,3ul,4ul})gpu.dev->SetTexture(slot,material);
        gpu.dev->SetTexture(5,white);gpu.dev->SetTexture(6,mask);gpu.dev->SetTexture(7,lamp);gpu.dev->SetTexture(8,detail);gpu.dev->SetTexture(9,white);
        gpu.dev->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE);const auto a=gpu.Render(single,singleVs);
        float range[4]={1,2,0,0};gpu.dev->SetPixelShaderConstantF(dayConst,range,1);const auto corrected=gpu.Render(patched,singleVs);
        range[0]=0;gpu.dev->SetPixelShaderConstantF(dayConst,range,1);const auto night=gpu.Render(patched,singleVs);
        Check(night==a,"zero daylight weight preserves every original shader pixel");
        for(float weight:{.25f,.5f,.75f}){
            range[0]=weight;gpu.dev->SetPixelShaderConstantF(dayConst,range,1);const auto twilight=gpu.Render(patched,singleVs);
            const float light=1.35f+(rgb/255.f)*1.21467388f;
            const int expected=int((light*(1-weight)+std::min(light,2.f)*weight)*.2f*255);
            Check(std::abs(int(twilight[2000]&255)-expected)<=3,"terrain range recovers continuously through twilight");
        }
        c[3][0]=1.10212243f;gpu.dev->SetPixelShaderConstantF(0,c[0],32);
        gpu.dev->SetTexture(1,normal);gpu.dev->SetTexture(2,lamp);gpu.dev->SetTexture(6,white);
        gpu.dev->SetRenderState(D3DRS_ALPHABLENDENABLE,TRUE);gpu.dev->SetRenderState(D3DRS_SRCBLEND,D3DBLEND_DESTCOLOR);gpu.dev->SetRenderState(D3DRS_DESTBLEND,D3DBLEND_SRCCOLOR);
        gpu.dev->Clear(0,nullptr,D3DCLEAR_TARGET,0xff333333,1,0);const auto b=gpu.Render(multi,multiVs);
        const int av=a[2000]&255,bv=b[2000]&255;
        const float light=1.35f+(rgb/255.f)*1.21467388f;
        std::printf("Captured terrain composition: map %d, lighting %.4f, single %d, multi %d\n",rgb,light,av,bv);
        Check(std::abs(av-int(light*.2f*255))<=3,"captured single pass applies material before target clipping");
        Check(std::abs(bv-int(std::min(light,2.f)*.2f*255))<=3,"captured multi pass clips illumination before material blending");
        Check(rgb>=192 ? av>bv+5 : std::abs(av-bv)<=3,"captured two paths diverge only above light buffer range");
        for(size_t pixel=0;pixel<corrected.size();++pixel)for(int shift:{0,8,16})
            Check(std::abs(int((corrected[pixel]>>shift)&255)-int((b[pixel]>>shift)&255))<=2,"corrected single and native multi composition agree at every pixel");
        if(rgb==0)Check(corrected==a,"lamp-free solar pixels stay identical");
    }
    gpu.dev->SetRenderState(D3DRS_ALPHABLENDENABLE,FALSE);
    for(DWORD slot=0;slot<10;++slot)gpu.dev->SetTexture(slot,nullptr);
    gpu.dev->SetPixelShader(nullptr);gpu.dev->SetVertexShader(nullptr);
    Release(single);Release(multi);Release(patched);Release(singleVs);Release(multiVs);Release(cube);
    Release(normal);Release(material);Release(mask);Release(white);Release(detail);Release(lamp);
}

// ---- Per-pixel lamp law of outdoor objects, fences and windows (12/10, realistic reach). ----
// The law of an outdoor wall texel (lamp_reach.h) at distance d, before the texel clamp: Gain x N.l / d^2 x Share
void ReachLawTable() {
    const int before = checks;
    float r0, R;
    LampReach::Radii(85.f, r0, R);
    // the garden post is a story-0 lot lamp: 2 entries in the outdoor rooms' light lists (LampReach::Entries)
    Check(Near(r0, 3.5f) && Near(R, 15.f) && Near(LampReach::Gain(85.f, 2.f), 42.5f), "garden post (range 42.5 x intensity 2): r0 3.5 m, R 15 m, gain 42.5");
    Check(Near(LampReach::Gain(42.5f * 3.4f, 2.f) * 3.4f, 245.65f), "the same post at intensity 3.40: E0 x gain 245.65 (fitted 246)");
    Check(LampReach::Entries(true, 0) == 2.f && LampReach::Entries(true, 1) == 1.f && LampReach::Entries(false, 0) == 1.f && Near(LampReach::Gain(85.f, 1.f), 21.25f),
          "one entry (a world lamp, a lot lamp of another story): half the story-0 lot lamp's gain");
    // wall row (c3.x 0.309) of the object-law analysis for the post at intensity 2: .309 / .236 / .066 / .005 at 3 / 6 / 8 / 12 m
    const float wall[4] = {.309f, .236f, .066f, .005f}, at[4] = {3, 6, 8, 12};
    for (int i = 0; i < 4; ++i) {
        const float texel = std::min(1.f, 2.f * LampReach::Gain(85.f, 2.f) / (at[i] * at[i]) * LampReach::Share(at[i], r0, R));
        Check(std::fabs(texel * .309f - wall[i]) < .0015f, "reach law reproduces the analysis' wall row");
    }
    for (int i = 0; i <= 400; ++i) { // the form the shader evaluates: min(1, r0^2/d^2 x win^2) against Share
        const float d = .05f + i * .05f, q = d * d / (R * R), win = std::clamp(1.f - q * q, 0.f, 1.f);
        const float shader = std::min(1.f, r0 * r0 / (d * d) * win * win), cpu = LampReach::Share(d, r0, R);
        Check(d <= r0 ? shader >= .975f * cpu : std::fabs(shader - cpu) <= 1e-5f + 1e-5f * cpu, "shader form of the reach share within 2.5% of the walls' (only up to r0)");
    }
    for (float w : {0.f, -1.f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity()}) {
        LampReach::Radii(w, r0, R);
        Check(Near(r0, 3.5f) && Near(R, 15.f) && Near(LampReach::Gain(w, 2.f), 42.5f), "unreadable lamp weight counts as 85");
    }
    std::printf("Reach law table: %d checks\n", checks - before);
}

uint32_t Fnv(const std::vector<DWORD>& t) { DWORD h = 2166136261u; for (DWORD w : t) h = (h ^ w) * 16777619u; return h; }
// "approximately N instruction slots used" of D3DDisassemble, -1 when it does not disassemble
int Slots(const std::vector<DWORD>& t) {
    ID3DBlob* dis = nullptr;
    if (FAILED(D3DDisassemble(t.data(), t.size() * 4, 0, nullptr, &dis)) || !dis) return -1;
    const std::string text(static_cast<const char*>(dis->GetBufferPointer()), dis->GetBufferSize());
    Release(dis);
    const auto at = text.find("approximately ");
    return at == std::string::npos ? 0 : std::atoi(text.c_str() + at + 14);
}
// Both variants of the three patchers over every pixel shader under the roots (read-only: Light Probe captures, refused
// shaders, census). Prints one FIRST line per shader with the first-law outputs' hashes, to compare with the same walk of
// the previous source.
void ObjectLawPatches(Gpu& gpu, const std::wstring& roots) {
    const int before = checks;
    std::vector<std::vector<DWORD>> shaders;
    std::vector<uint32_t> seen;
    size_t files = 0;
    for (size_t from = 0; from <= roots.size();) {
        size_t to = roots.find(L';', from);
        if (to == std::wstring::npos) to = roots.size();
        const std::filesystem::path root = roots.substr(from, to - from);
        from = to + 1;
        if (root.empty()) continue;
        Check(std::filesystem::exists(root), "object law shader root exists");
        std::error_code ec;
        for (auto it = std::filesystem::recursive_directory_iterator(root, std::filesystem::directory_options::skip_permission_denied, ec);
             !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            if (!it->is_regular_file() || it->path().extension() != ".bin") continue;
            const auto bytes = it->file_size();
            if (bytes < 8 || bytes > 262144 || bytes % 4) continue;
            std::ifstream f(it->path(), std::ios::binary);
            std::vector<DWORD> code(static_cast<size_t>(bytes) / 4);
            f.read(reinterpret_cast<char*>(code.data()), static_cast<std::streamsize>(bytes));
            if (!f || code[0] != 0xFFFF0300u) continue;
            ++files;
            const uint32_t h = Fnv(code);
            if (std::find(seen.begin(), seen.end(), h) != seen.end()) continue;
            seen.push_back(h);
            shaders.push_back(std::move(code));
        }
    }
    std::printf("Object law: %zu ps_3_0 files, %zu distinct shaders\n", files, shaders.size());
    Check(!shaders.empty(), "object law shaders found");
    constexpr unsigned N = ShaderPatches::kObjectPixelLamps;
    constexpr size_t kDelta = N * (5 + 7 * 4 - 3 * 4) + 3; // per lamp 3 instructions (12 tokens) become 8 (a mad and 7 of 4 tokens), then mov_sat
    struct Tally { int first = 0, reach = 0, refusedReach = 0, created = 0, maxSlots = 0; } tally[3];
    const char* names[3] = {"fence", "object", "window"};
    for (const auto& original : shaders) {
        std::string line = std::format("FIRST {:08X}", Fnv(original));
        for (int p = 0; p < 3; ++p) {
            std::vector<DWORD> variant[2];
            bool ok[2] = {};
            DWORD reachConst = 0, expectReach = 0;
            for (int reach = 0; reach < 2; ++reach) {
                auto t = original;
                const bool r = reach != 0;
                // fences without the ground lamp shade (13/10): the FIRST hashes stay those of the law alone (the shade has
                // its own check, tools/floor_shade_check)
                if (p == 0) { ShaderPatches::InstancedPatch o; ok[reach] = ShaderPatches::PatchInstancedLamps(t, o, r, false) && (!r || o.pixelLamps);
                    if (r && ok[reach]) { reachConst = o.reachConst; expectReach = o.coneConst + 2 * N; } }
                else if (p == 1) { ShaderPatches::ObjectLampPatch o; ok[reach] = ShaderPatches::PatchObjectLampPs(t, o, r);
                    if (r && ok[reach]) { reachConst = o.reachConst; expectReach = o.lampParamConst + 1 + 4 * N; } }
                else { ShaderPatches::WindowOutdoorPatch o; ok[reach] = ShaderPatches::PatchWindowOutdoor(t, o, r);
                    if (r && ok[reach]) { reachConst = o.reachConst; expectReach = o.planConst + 2; } }
                if (!ok[reach]) { Check(t == original, "refused law patch leaves the shader unchanged"); continue; }
                variant[reach] = std::move(t);
                const int slots = Slots(variant[reach]);
                Check(slots > 0, "patched law shader disassembles");
                Check(slots <= 512, "patched law shader within the ps_3_0 512 instruction slots");
                tally[p].maxSlots = std::max(tally[p].maxSlots, slots);
                if (gpu.dev) { IDirect3DPixelShader9* s = nullptr; const bool created = SUCCEEDED(gpu.dev->CreatePixelShader(variant[reach].data(), &s));
                    Check(created, "native D3D9 creates the patched law shader"); tally[p].created += created; Release(s); }
                ++(reach ? tally[p].reach : tally[p].first);
            }
            Check(!ok[1] || ok[0], "the reach variant is accepted only where the first law is");
            if (ok[0] && !ok[1]) ++tally[p].refusedReach;
            if (ok[1]) {
                Check(reachConst && reachConst == expectReach, "reach rows right after the lamp rows (one upload)");
                Check(variant[1].size() == variant[0].size() + kDelta, "reach variant = first law + 5 instructions per lamp + the clamp");
            }
            line += std::format(" {}={}", names[p], ok[0] ? std::format("{:08X}", Fnv(variant[0])) : std::string("-"));
        }
        std::printf("%s\n", line.c_str());
    }
    for (int p = 0; p < 3; ++p)
        std::printf("Object law %s: first law %d, reach %d (refused where the first is accepted: %d), created on D3D9 %d, most slots %d\n", names[p], tally[p].first,
                    tally[p].reach, tally[p].refusedReach, tally[p].created, tally[p].maxSlots);
    std::printf("Object law patches: %d checks\n", checks - before);
}

// GPU law: a synthetic fence-shaped shader (COLOR0 added once, TEXCOORD1.zw world xz, TEXCOORD2 normal and world y), patched
// both ways, one lamp, a black atlas: the output is the lamp term, compared with the CPU law at every pixel (2 LSB).
void ObjectLawPixels(Gpu& gpu) {
    if (!gpu.dev) return;
    const int before = checks;
    constexpr unsigned N = ShaderPatches::kObjectPixelLamps;
    auto ps = Compile(R"(
sampler2D base:register(s0); float4 k:register(c0);
float4 main(float4 c:COLOR0,float4 uv:TEXCOORD1,float4 n:TEXCOORD2):COLOR0{
float3 s=max(tex2D(base,uv.xy).rgb,k.xyz);float3 t=s+c.rgb;return float4(t*k.w,n.w*k.w);}
)", "ps_3_0");
    const auto vsCode = Compile(R"(
float4 rows[3]:register(c0);
struct O{float4 p:POSITION;float4 c:COLOR0;float4 uv:TEXCOORD1;float4 n:TEXCOORD2;};
O main(float4 p:POSITION,float2 uv:TEXCOORD0){O o;o.p=p;o.c=0;
float3 w=rows[0].xyz+uv.x*rows[1].xyz+uv.y*rows[2].xyz;o.uv=float4(uv,w.x,w.z);o.n=float4(rows[0].w,rows[1].w,rows[2].w,w.y);return o;}
)", "vs_3_0");
    IDirect3DVertexShader9* vs = nullptr;
    Check(!vsCode.empty() && SUCCEEDED(gpu.dev->CreateVertexShader(vsCode.data(), &vs)), "law fixture VS");
    IDirect3DTexture9* black = nullptr;
    Check(SUCCEEDED(gpu.dev->CreateTexture(1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &black, nullptr)), "law black texture");
    if (black) { D3DLOCKED_RECT r{}; black->LockRect(0, &r, nullptr, 0); *static_cast<DWORD*>(r.pBits) = 0; black->UnlockRect(0); }
    for (bool reach : {false, true}) {
        auto code = ps;
        ShaderPatches::InstancedPatch o;
        const bool ok = !code.empty() && ShaderPatches::PatchInstancedLamps(code, o, reach, false) && o.pixelLamps; // the law alone
        Check(ok, "synthetic fence shader takes the per-pixel lamps");
        if (!ok) { ID3DBlob* d = nullptr; if (!ps.empty() && SUCCEEDED(D3DDisassemble(ps.data(), ps.size() * 4, 0, nullptr, &d))) { std::printf("%s\n", static_cast<char*>(d->GetBufferPointer())); d->Release(); } continue; }
        auto* shader = gpu.Shader(code);
        if (!shader || !vs || !black) { Release(shader); continue; }
        gpu.dev->SetTexture(0, black); gpu.dev->SetTexture(o.atlasSampler, black);
        // pixel world position: x 0.5 .. 25 m along the image, z 0 .. 8 m down it, y 0; normal -x (N.l = x / d); lamp head at (0, 2, 0)
        const float rows[3][4] = {{.5f, 0, 0, -1}, {24.5f, 0, 0, 0}, {0, 0, 8, 0}};
        gpu.dev->SetVertexShaderConstantF(0, rows[0], 3);
        for (float w : {85.f, 144.5f}) {
            // reach: E0 x gain through the 8-bit range (w 85) and over the texel clamp near the lamp (w 144.5, red 4x over it);
            // first law: the colour itself
            const float reachMid[3] = {.05f, .02f, .01f}, reachHigh[3] = {1.f, .3f, .05f}, firstE0[3] = {.9f, .5f, .25f};
            const float* e0 = !reach ? firstE0 : w < 100.f ? reachMid : reachHigh;
            float r0, R; LampReach::Radii(w, r0, R);
            const float g = reach ? LampReach::Gain(w, 2.f) : 1.f, radius = reach ? R : 7.8f;
            const float base[4] = {0, 0, 0, 1}; // k: the base material term 0, the output scale 1
            gpu.dev->SetPixelShaderConstantF(0, base, 1);
            float atlas[4] = {1, 1, 0, 0}, strength[4] = {1, 0, 0, 0}, weight[4] = {1, 0, 1, 0};
            gpu.dev->SetPixelShaderConstantF(o.atlasConst, atlas, 1); gpu.dev->SetPixelShaderConstantF(o.strengthConst, strength, 1);
            gpu.dev->SetPixelShaderConstantF(o.groundWeightConst, weight, 1);
            float lamps[1 + 4 * N + 2][4] = {};
            lamps[0][0] = 1; lamps[0][1] = 1; lamps[0][3] = 1e-4f;
            for (unsigned k = 0; k < N; ++k) { lamps[1 + 2 * k][0] = lamps[1 + 2 * k][2] = 1e6f; lamps[2 + 2 * k][3] = lamps[1 + 2 * N + 2 * k][3] = lamps[2 + 2 * N + 2 * k][3] = 1; }
            lamps[1][0] = 0; lamps[1][1] = 2; lamps[1][2] = 0; lamps[1][3] = 1.f / (radius * radius);
            for (int i = 0; i < 3; ++i) lamps[2][i] = e0[i] * g;
            lamps[1 + 4 * N][0] = r0 * r0;
            gpu.dev->SetPixelShaderConstantF(o.lampParamConst, lamps[0], 1 + 4 * N + (reach ? 2 : 0));
            const auto px = gpu.Render(shader, vs);
            int worst = 0, wGot = 0, wWant = 0;
            float wD = 0;
            for (int y = 0; y < 64; ++y) for (int x = 0; x < 64; ++x) {
                // D3D9 pixel centres lie on whole screen coordinates: clip -1 is pixel 0's centre (uv 0), not its left edge
                const float u = x / 64.f, v = y / 64.f, wx = .5f + 24.5f * u, wz = 8 * v;
                const float d2 = wx * wx + 4 + wz * wz, d = std::sqrt(d2), nl = std::clamp(wx / d, 0.f, 1.f);
                for (int ch = 0; ch < 3; ++ch) {
                    float q = reach ? e0[ch] * g * nl / d2 * std::min(1.f, r0 * r0 / d2 * std::pow(std::clamp(1.f - (d2 / (R * R)) * (d2 / (R * R)), 0.f, 1.f), 2.f))
                                    : e0[ch] * nl * std::pow(std::clamp(1.f - d2 / (radius * radius), 0.f, 1.f), 2.f);
                    q = std::clamp(q, 0.f, 1.f);
                    const int got = int((px[y * 64 + x] >> (16 - 8 * ch)) & 255), want = int(std::lround(q * 255));
                    if (std::abs(got - want) > worst) worst = std::abs(got - want), wGot = got, wWant = want, wD = d;                }
            }
            Check(worst <= 2, reach ? "GPU reach law matches the walls' law at every pixel (2 LSB)" : "GPU first law unchanged at every pixel (2 LSB)");
            std::printf("GPU %s law, w %.1f: largest difference %d LSB over 64x64 pixels (d 2-25.9 m; at d %.2f m: %d for %d)\n", reach ? "reach" : "first", w, worst, wD, wGot, wWant);
        }
        gpu.dev->SetTexture(0, nullptr); gpu.dev->SetTexture(o.atlasSampler, nullptr);
        gpu.dev->SetPixelShader(nullptr); gpu.dev->SetVertexShader(nullptr);
        Release(shader);
    }
    Release(vs); Release(black);
    std::printf("Object law pixels: %d checks\n", checks - before);
}

// Walls shade lamp light on lot ground (13/10, GroundShadow): the production shade job on synthetic lots
void GroundShadowChecks() {
    using namespace GroundShadow;
    const int before = checks;
    const auto run = [](const Grid& g, std::vector<Lamp> lamps, std::vector<Wall> walls, int slices, std::vector<Lamp> others = {}) {
        ShadeJob job;
        job.Init(g, std::move(lamps), std::move(walls), {}, {}, std::move(others));
        int budget = 0, guard = 0;
        // a few steps a slice, as the render thread's budget would cut it
        while (!job.Step([&] { return ++budget % slices != 0; }) && guard++ < 1000000) {}
        return job.Result();
    };
    Grid g; g.w = 160; g.h = 160; // a 40 x 40 m lot at the origin, ground at y 10
    g.ty = 10.0f;
    const auto at = [&](const std::vector<uint8_t>& m, float x, float z) { return m[static_cast<size_t>(z * 4) * g.w + static_cast<size_t>(x * 4)]; };
    const Lamp post{10.0f, 12.0f, 20.0f, 0.4f * 30.0f}; // a garden post: head 2 m up
    const Wall wall{14.0f, 15.0f, 14.0f, 25.0f, 10.0f, 13.0f}; // a free-standing wall 4 m east of it, 3 m tall, 10 m long
    // no wall: 255 everywhere, exactly (the pass is unchanged)
    const auto open = run(g, {post}, {}, 7);
    Check(open.size() == 160u * 160u && std::all_of(open.begin(), open.end(), [](uint8_t v) { return v == 255; }), "ground shade: no wall, no shade");
    const auto none = run(g, {}, {wall}, 7);
    Check(std::all_of(none.begin(), none.end(), [](uint8_t v) { return v == 255; }), "ground shade: no lamp, no shade");
    const auto m = run(g, {post}, {wall}, 3);
    Check(at(m, 16.0f, 20.0f) < 8, "ground shade: the grass behind the wall is shaded");
    // farther, where the post's stamp is under kOtherLight, only its own share goes (the lot map's other light stays)
    Check(at(m, 18.0f, 20.0f) > 8 && at(m, 18.0f, 20.0f) < 96, "ground shade: faint blocked light takes only its share");
    Check(at(m, 6.0f, 20.0f) == 255 && at(m, 12.0f, 20.0f) == 255, "ground shade: the lamp's side keeps its light");
    Check(at(m, 18.0f, 5.0f) == 255, "ground shade: grass beside the wall's shadow keeps its light");
    Check(at(m, 14.5f, 20.0f) < 255 && at(m, 13.5f, 20.0f) > 0, "ground shade: soft at the wall");
    // the same lot built in one go: slicing changes nothing
    Check(run(g, {post}, {wall}, 1000000) == m, "ground shade: sliced build equals a single build");
    // a second lamp on the far side lights the grass behind the wall: a share, not black
    const Lamp other{24.0f, 12.0f, 20.0f, 0.4f * 30.0f};
    const auto two = run(g, {post, other}, {wall}, 5);
    Check(at(two, 16.0f, 20.0f) > 64 && at(two, 16.0f, 20.0f) < 255, "ground shade: two lamps, one blocked, a share stays");
    // (2.15.3) a lamp not tested against the walls stays in the total: east of the wall, where the wall does not block it,
    // the same shade as when it is tested
    const auto untested = run(g, {post}, {wall}, 5, {other});
    bool same = untested.size() == two.size();
    for (size_t i = 0; same && i < two.size(); i++)
        if (static_cast<float>(i % g.w) / 4.0f > 15.0f) same = std::abs(int(untested[i]) - int(two[i])) <= 1;
    Check(same, "ground shade: an untested lamp's light stays in the shade's total");
    // a lamp's shadow ends where its stamp's rectangle ends (nothing of it lies there to block)
    Lamp clipped = post;
    clipped.rx0 = 0.0f, clipped.rz0 = 0.0f, clipped.rx1 = 17.0f, clipped.rz1 = 40.0f;
    const auto cl = run(g, {clipped}, {wall}, 5);
    Check(at(cl, 16.0f, 20.0f) < 8 && at(cl, 18.0f, 20.0f) == 255, "ground shade: no shadow past the lamp's stamp");
    // a low wall (0.5 m) under a 2 m head shades only near its foot
    const Wall low{14.0f, 15.0f, 14.0f, 25.0f, 10.0f, 10.5f};
    const auto lowM = run(g, {post}, {low}, 5);
    Check(at(lowM, 14.6f, 20.0f) < 255 && at(lowM, 19.0f, 20.0f) == 255, "ground shade: a low wall's shadow ends near it");
    // a wall of an upper story (base over the head) shades nothing
    const Wall upper{14.0f, 15.0f, 14.0f, 25.0f, 13.0f, 16.0f};
    const auto up = run(g, {post}, {upper}, 5);
    Check(std::all_of(up.begin(), up.end(), [](uint8_t v) { return v == 255; }), "ground shade: walls above the head do not shade the ground");
    // a sconce on the wall's face (head 0.1 m in front): its own face does not shade the lamp's side; the far face does
    const Wall faceA{14.0f, 15.0f, 14.0f, 25.0f, 10.0f, 13.0f}, faceB{14.2f, 15.0f, 14.2f, 25.0f, 10.0f, 13.0f};
    const Lamp sconce{13.9f, 12.0f, 20.0f, 0.4f * 30.0f};
    const auto sc = run(g, {sconce}, {faceA, faceB}, 5);
    Check(at(sc, 10.0f, 20.0f) == 255 && at(sc, 16.0f, 20.0f) < 8, "ground shade: a sconce lights its side, not behind its wall");
    // a rotated lot (90 degrees) gives the same shade at the same world points
    Grid r = g; r.m0 = 0; r.m2 = 1; r.m8 = -1; r.m10 = 0; r.tx = 40.0f; r.tz = 0.0f; // world x = 40 - lz, world z = lx
    const auto rot = run(r, {post}, {wall}, 5);
    const auto atWorld = [&](const std::vector<uint8_t>& mm, const Grid& gg, float x, float z) {
        float fi, fj; WorldTexel(gg, x, z, fi, fj); return mm[static_cast<size_t>(fj) * gg.w + static_cast<size_t>(fi)]; };
    Check(atWorld(rot, r, 16.0f, 20.0f) < 8 && atWorld(rot, r, 6.0f, 20.0f) == 255 && atWorld(rot, r, 18.0f, 5.0f) == 255,
          "ground shade: rotated lots shade the same world points");
    // weights: the stamp's law, saturated next to the lamp, nothing past the reach
    Check(Weight(post, 10.0f, 10.0f, 20.0f) == 1.0f && Weight(post, 10.0f + Reach(post, 10.0f) + 1.0f, 10.0f, 20.0f) < kMinWeight, "ground shade: stamp weight and reach");
    std::printf("Ground shade: %d checks\n", checks - before);
}
// Foliage wrap with the daytime curve (13/10): the runtime variant is the defined one without its def (c255 set per draw)
void FoliageWrapChecks(const std::wstring& roots) {
    const int before = checks;
    int found = 0;
    for (size_t from = 0; from <= roots.size();) {
        size_t to = roots.find(L';', from);
        if (to == std::wstring::npos) to = roots.size();
        const std::filesystem::path root = roots.substr(from, to - from);
        from = to + 1;
        if (root.empty() || !std::filesystem::exists(root)) continue;
        std::error_code ec;
        for (auto it = std::filesystem::recursive_directory_iterator(root, std::filesystem::directory_options::skip_permission_denied, ec);
             !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            if (!it->is_regular_file() || it->path().extension() != ".bin") continue;
            const auto bytes = it->file_size();
            if (bytes < 8 || bytes > 262144 || bytes % 4) continue;
            std::ifstream f(it->path(), std::ios::binary);
            std::vector<DWORD> code(static_cast<size_t>(bytes) / 4);
            f.read(reinterpret_cast<char*>(code.data()), static_cast<std::streamsize>(bytes));
            if (!f || (code[0] & 0xFFFF0000u) != 0xFFFE0000u) continue;
            auto a = code, b = code;
            const bool pa = ShaderPatches::PatchFoliageVs(a), pb = ShaderPatches::PatchFoliageVs(b, true);
            Check(pa == pb, "foliage wrap: both variants recognise the same shaders");
            if (!pa || !pb) continue;
            ++found;
            Check(a.size() == b.size() + 6 && std::equal(b.begin() + 1, b.end(), a.begin() + 7) && a[0] == b[0], "foliage wrap: the runtime variant drops only the def");
        }
    }
    std::printf("Foliage wrap: %d foliage vertex shaders, %d checks\n", found, checks - before);
}

// Foliage under the reach law (2.13.1): the rigs' outdoor lamps take the per-pixel lamps' law at the rig's centre instead of
// the ground footprint's boost, and the foliage VS weight (c255) becomes the plain Lambert N.L. Pure policy checks.
void FoliageReachChecks() {
    const int before = checks;
    using namespace TerrainLightingPolicy;
    float w[2];
    // the shader's weight: max(N.L x c255.x + c255.y, 0)
    auto weight = [&](float ndl) { return std::max(ndl * w[0] + w[1], 0.0f); };
    for (float night : {0.0f, 0.5f, 1.0f})
        for (bool dayFix : {false, true}) {
            FoliageLampWeights(night, true, dayFix, w);
            Check(weight(1.0f) == 1.0f && weight(0.5f) == 0.5f && weight(0.0f) == 0.0f && weight(-0.5f) == 0.0f, "foliage reach: Lambert weight, leaves turned away take none");
        }
    FoliageLampWeights(1.0f, false, true, w);
    Check(w[0] == 1.0f / 1.5f && w[1] == 0.5f / 1.5f, "foliage reach off: the 1.0.0 wrap at night, unchanged");
    FoliageLampWeights(0.0f, false, true, w);
    Check(std::fabs(w[0] - LampDay(0.0f) / 1.5f) < 1e-7f && std::fabs(weight(-1.0f) - LampDay(0.0f) * 0.0f) < 1e-7f, "foliage reach off: the wrap x the daytime curve by day");
    FoliageLampWeights(0.0f, false, false, w);
    Check(w[0] == 1.0f / 1.5f, "foliage reach off, day fix off: the wrap at full strength");
    // regather policy
    Check(RigLawChanged(-1.0f, 0.8f) && RigLawChanged(0.8f, -1.0f), "rig law: on / off gathers again");
    Check(!RigLawChanged(-1.0f, -1.0f) && !RigLawChanged(0.8f, 0.8f) && !RigLawChanged(1.0f, 1.04f) && RigLawChanged(1.0f, 1.06f) && RigLawChanged(1.0f, 0.94f),
          "rig law: gathers again past a 5% change only");
    Check(!RigLawChanged(1.0f, std::numeric_limits<float>::quiet_NaN()), "rig law: a NaN scale changes nothing");
    int steps = 0;
    float gathered = LampDay(0.0f);
    for (int i = 0; i <= 1000; i++) {
        const float s = LampDay(i / 1000.0f);
        if (RigLawChanged(gathered, s)) gathered = s, steps++;
    }
    Check(steps > 10 && steps < 120 && std::fabs(gathered - 1.0f) <= kRigLawStep, "rig law: day to night in a bounded number of gathers, ending within 5% of night");
    // the law against the footprint boost on the 12/10 garden post (w = 85, a story-0 lot lamp: 2 entries; bounds half-width
    // 34.6 m for range 47 in BoostRec's note): a bush 2 m from the post and a tree 9 m away
    const float lawNear = LampReach::PixelLaw(2.0f * 2.0f, 85.0f, 2.0f), lawFar = LampReach::PixelLaw(9.0f * 9.0f, 85.0f, 2.0f);
    auto boost = [](float h) { const float q = 1.0f - h * h / (34.6f * 34.6f); return q * q; };
    Check(lawNear / lawFar > 5.0f && boost(2.0f) / boost(9.0f) < 1.2f, "foliage reach: the law separates 2 m and 9 m (the boost did not)");
    Check(LampReach::PixelLaw(15.5f * 15.5f, 85.0f, 2.0f) == 0.0f, "foliage reach: nothing past R");
    std::printf("Foliage reach law: %d checks (2 m / 9 m: law x%.1f, footprint boost x%.2f; %d gathers day to night)\n", checks - before, lawNear / lawFar, boost(2.0f) / boost(9.0f), steps);
}

// Roads and sidewalks with the walls' shade (PatchRoad shade = true): max(road map, terrain) x shade x the lamp scale
void RoadShadePixels(Gpu& gpu) {
    if (!gpu.dev) return;
    auto code = Compile("sampler2D light:register(s0); float4 k:register(c0); float4 main(float4 c:COLOR0, float2 uv:TEXCOORD0):COLOR0{float3 l=tex2D(light,uv).rgb*k.x;return float4(l/(1+c.rgb*k.y),c.a);}", "ps_3_0");
    if (code.empty()) { Check(false, "road fixture compiles"); return; }
    auto plain = code, shaded = code;
    ShaderPatches::RoadPatch a, b;
    const bool pa = ShaderPatches::PatchRoad(plain, a), pb = ShaderPatches::PatchRoad(shaded, b, true);
    Check(pa && pb, "road fixture patched with and without the shade");
    if (!pa || !pb) {
        ID3DBlob* dis = nullptr; D3DDisassemble(code.data(), code.size() * 4, 0, nullptr, &dis);
        if (dis) { std::printf("%s\n", static_cast<char*>(dis->GetBufferPointer())); dis->Release(); }
        return;
    }
    Check(a.shadeSampler == -1 && b.shadeSampler == static_cast<int>(b.extraSampler) + 1 && a.extraSampler == b.extraSampler && a.scaleConst == b.scaleConst,
          "road shade: next sampler, the rest as the plain copy");
    auto* light = gpu.Texture(false); auto* terrain = gpu.Texture(true);
    IDirect3DTexture9* map = nullptr;
    Check(SUCCEEDED(gpu.dev->CreateTexture(4, 4, 1, 0, D3DFMT_L8, D3DPOOL_MANAGED, &map, nullptr)), "road shade texture");
    D3DLOCKED_RECT lr;
    if (map && SUCCEEDED(map->LockRect(0, &lr, nullptr, 0))) {
        for (int j = 0; j < 4; j++) std::memset(static_cast<BYTE*>(lr.pBits) + j * lr.Pitch, 128, 4);
        map->UnlockRect(0);
    }
    const float k[4] = {1, 0, 0, 0}; gpu.dev->SetPixelShaderConstantF(0, k, 1);
    gpu.dev->SetTexture(0, light); gpu.dev->SetTexture(a.extraSampler, terrain); gpu.dev->SetTexture(b.shadeSampler, map);
    auto* psA = gpu.Shader(plain); auto* psB = gpu.Shader(shaded);
    const auto ra = gpu.Render(psA), rb = gpu.Render(psB);
    int worst = 0;
    for (size_t px = 0; px < ra.size(); ++px)
        for (int ch = 0; ch < 3; ch++) {
            const int want = static_cast<int>(std::lround(((ra[px] >> (8 * ch)) & 0xFF) * 128.0 / 255.0));
            worst = std::max(worst, std::abs(static_cast<int>((rb[px] >> (8 * ch)) & 0xFF) - want));
        }
    Check(worst <= 2 && (ra[0] & 0xFFFFFF) != 0, "road shade: plain road light x shade (within 2 of 255)");
    std::printf("Road wall shade: s%d, worst RGB error %d of 255 (plain pixel %08lX, shaded %08lX)\n", b.shadeSampler, worst, ra[0], rb[0]);
    for (DWORD st : {0ul, a.extraSampler, static_cast<DWORD>(b.shadeSampler)}) gpu.dev->SetTexture(st, nullptr);
    Release(psA); Release(psB); Release(light); Release(terrain); Release(map);
}

int wmain(int argc, wchar_t** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    const auto start = std::chrono::steady_clock::now();
    Policies(); std::printf("Policies: %d checks, %d failures\n", checks, failures);
    GroundShadowChecks();
    ReachLawTable();
    FoliageReachChecks();
    SamplerRestoration(); std::printf("Sampler restoration complete\n"); Gpu gpu;
    Patches(gpu, argc > 1 ? std::filesystem::path(argv[1]) : std::filesystem::path());
    RoadShadePixels(gpu);
    if (argc > 3 && argv[2][0] && argv[3][0]) LotPixels(gpu, argv[2], argv[3]);
    if (argc > 4 && argv[4][0]) WallPixels(gpu, argv[4]);
    if (argc > 5 && argv[5][0]) InstancedPixels(gpu, argv[5]);
    if (argc > 6 && argv[6][0]) TerrainCompositionPixels(gpu, argv[6]);
    ObjectLawPixels(gpu);
    // the shader roots of the object law coverage: argument 7, or APEX_OBJECT_LAW_ROOTS (run.ps1 -ObjectLawRoots); ';'-separated
    std::wstring lawRoots = argc > 7 ? argv[7] : L"";
    if (lawRoots.empty()) {
        wchar_t buffer[4096] = {};
        const DWORD n = GetEnvironmentVariableW(L"APEX_OBJECT_LAW_ROOTS", buffer, 4096);
        if (n > 0 && n < 4096) lawRoots = buffer;
    }
    if (!lawRoots.empty()) ObjectLawPatches(gpu, lawRoots);
    if (!lawRoots.empty()) FoliageWrapChecks(lawRoots);
    else std::printf("Object law coverage over captured shaders not run (no -ObjectLawRoots)\n");
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    std::printf("Terrain lighting: %d checks, %d failures; harness %.1f ms (not game FPS)\n", checks, failures, ms);
    return failures ? 1 : 0;
}
