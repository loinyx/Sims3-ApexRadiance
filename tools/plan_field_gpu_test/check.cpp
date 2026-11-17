// Real-time GI's wall-plan visibility field on the GPU (shaders/plan_field_hlsl.h) against its CPU form
// (features/plan_field.h), work package S2-03: the flood and distance passes texel by texel against PlanField::BuildField, and
// the sphere trace pair by pair against PlanField::TraceStory and the twin, on the cases of tools/rt_lamps_test/plan_field_check.h
// (closed wall, doorway, diagonal wall, window and half wall). Needs a D3D9 GPU; read-only, prints to stdout.
#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
static int checks = 0, failures = 0;
static void Check(bool ok, const std::string& text) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL: %s\n", text.c_str());
    }
}
#include "tools/rt_lamps_test/plan_field_check.h"
#include "shaders/plan_field_hlsl.h"

using namespace PlanFieldTest;
using PlanField::Field;

static IDirect3D9* g_d3d = nullptr;
static IDirect3DDevice9* g_dev = nullptr;
static D3DFORMAT g_seedFmt = D3DFMT_G32R32F;

static bool MakeDevice() {
    WNDCLASSA wc{};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "ApexPlanFieldGpu";
    RegisterClassA(&wc);
    HWND wnd = CreateWindowA(wc.lpszClassName, "plan field gpu", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    g_d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!g_d3d || !wnd) return false;
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE, pp.SwapEffect = D3DSWAPEFFECT_DISCARD, pp.BackBufferWidth = 64, pp.BackBufferHeight = 64, pp.hDeviceWindow = wnd;
    const HRESULT hr = g_d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd, D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &pp, &g_dev);
    D3DADAPTER_IDENTIFIER9 id{};
    g_d3d->GetAdapterIdentifier(D3DADAPTER_DEFAULT, 0, &id);
    char path[MAX_PATH] = {};
    GetModuleFileNameA(GetModuleHandleA("d3d9.dll"), path, MAX_PATH);
    std::printf("device: %s | adapter %s | d3d9.dll %s\n", SUCCEEDED(hr) ? "HAL" : "none", id.Description, path);
    if (FAILED(hr) || !g_dev) return false;
    D3DCAPS9 caps{};
    g_dev->GetDeviceCaps(&caps);
    const auto usable = [](D3DFORMAT f, DWORD usage) { return SUCCEEDED(g_d3d->CheckDeviceFormat(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, usage, D3DRTYPE_TEXTURE, f)); };
    if (!usable(D3DFMT_G32R32F, D3DUSAGE_RENDERTARGET)) g_seedFmt = D3DFMT_A32B32G32R32F;
    std::printf("caps: ps_3_0 instruction slots %lu | targets: G32R32F %d, A32B32G32R32F %d, A16B16G16R16F %d | bilinear: A16B16G16R16F %d, A32B32G32R32F %d\n",
                caps.MaxPixelShader30InstructionSlots, usable(D3DFMT_G32R32F, D3DUSAGE_RENDERTARGET), usable(D3DFMT_A32B32G32R32F, D3DUSAGE_RENDERTARGET),
                usable(D3DFMT_A16B16G16R16F, D3DUSAGE_RENDERTARGET), usable(D3DFMT_A16B16G16R16F, D3DUSAGE_QUERY_FILTER), usable(D3DFMT_A32B32G32R32F, D3DUSAGE_QUERY_FILTER));
    return true;
}

// Compiles an entry of kPlanFieldHlsl for ps_3_0 and prints the compiler's slot counts
static IDirect3DPixelShader9* Compile(const char* entry, int steps = 0, bool filtered = false) {
    const std::string stepText = std::to_string(steps);
    std::vector<D3D_SHADER_MACRO> defs;
    if (steps) defs.push_back({"PLAN_STEPS", stepText.c_str()});
    if (filtered) defs.push_back({"PLAN_FILTERED", "1"});
    defs.push_back({nullptr, nullptr});
    ID3DBlob *code = nullptr, *err = nullptr, *text = nullptr;
    if (FAILED(D3DCompile(kPlanFieldHlsl, std::strlen(kPlanFieldHlsl), "plan_field", defs.data(), nullptr, entry, "ps_3_0", 0, 0, &code, &err))) {
        std::printf("%s (%d steps) does not compile: %s\n", entry, steps, err ? static_cast<const char*>(err->GetBufferPointer()) : "");
        return nullptr;
    }
    std::string slots = "?";
    if (SUCCEEDED(D3DDisassemble(code->GetBufferPointer(), code->GetBufferSize(), 0, nullptr, &text))) {
        const std::string all(static_cast<const char*>(text->GetBufferPointer()));
        const size_t at = all.find("approximately");
        if (at != std::string::npos) slots = all.substr(at, all.find('\n', at) - at);
        slots += all.find(" rep i") != std::string::npos ? " | a loop (slots a pass of it)" : "";
        text->Release();
    }
    std::printf("  %-8s %s%s: %s\n", entry, steps ? (stepText + " steps").c_str() : "", filtered ? ", filtered" : "", slots.c_str());
    IDirect3DPixelShader9* ps = nullptr;
    g_dev->CreatePixelShader(static_cast<const DWORD*>(code->GetBufferPointer()), &ps);
    code->Release();
    return ps;
}

static IDirect3DTexture9* MakeTex(int w, int h, D3DFORMAT f, bool target) {
    IDirect3DTexture9* t = nullptr;
    g_dev->CreateTexture(w, h, 1, target ? D3DUSAGE_RENDERTARGET : 0, f, target ? D3DPOOL_DEFAULT : D3DPOOL_MANAGED, &t, nullptr);
    return t;
}
static int Bytes(D3DFORMAT f) { return f == D3DFMT_A32B32G32R32F ? 16 : f == D3DFMT_G32R32F || f == D3DFMT_A16B16G16R16F ? 8 : 4; }
// Rows of `bytes` a texel into a managed texture
static void Fill(IDirect3DTexture9* t, const void* src, int w, int h, int bytes) {
    D3DLOCKED_RECT lr{};
    t->LockRect(0, &lr, nullptr, 0);
    for (int y = 0; y < h; y++) std::memcpy(static_cast<uint8_t*>(lr.pBits) + static_cast<size_t>(y) * lr.Pitch, static_cast<const uint8_t*>(src) + static_cast<size_t>(y) * w * bytes, static_cast<size_t>(w) * bytes);
    t->UnlockRect(0);
}
static std::vector<uint8_t> Read(IDirect3DTexture9* t) {
    D3DSURFACE_DESC d{};
    t->GetLevelDesc(0, &d);
    const int w = static_cast<int>(d.Width), h = static_cast<int>(d.Height), bytes = Bytes(d.Format);
    std::vector<uint8_t> out(static_cast<size_t>(w) * h * bytes);
    IDirect3DSurface9 *sys = nullptr, *rt = nullptr;
    g_dev->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr);
    t->GetSurfaceLevel(0, &rt);
    D3DLOCKED_RECT lr{};
    if (sys && SUCCEEDED(g_dev->GetRenderTargetData(rt, sys)) && SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
        for (int y = 0; y < h; y++) std::memcpy(&out[static_cast<size_t>(y) * w * bytes], static_cast<const uint8_t*>(lr.pBits) + static_cast<size_t>(y) * lr.Pitch, static_cast<size_t>(w) * bytes);
        sys->UnlockRect();
    } else
        out.clear();
    if (rt) rt->Release();
    if (sys) sys->Release();
    return out;
}
static float HalfToFloat(uint16_t v) {
    const int e = (v >> 10) & 31, m = v & 1023;
    const float f = e == 0 ? std::ldexp(static_cast<float>(m), -24) : e == 31 ? PlanField::kNone : std::ldexp(static_cast<float>(m + 1024), e - 25);
    return v & 0x8000 ? -f : f;
}

// One pass over the whole of `target` with the pixel shader `ps` (the passes read VPOS, so the triangle carries nothing)
static void Pass(IDirect3DTexture9* target, IDirect3DPixelShader9* ps, DWORD writeMask = 0xF) {
    D3DSURFACE_DESC d{};
    target->GetLevelDesc(0, &d);
    IDirect3DSurface9* rt = nullptr;
    target->GetSurfaceLevel(0, &rt);
    g_dev->SetRenderTarget(0, rt);
    rt->Release();
    g_dev->SetRenderState(D3DRS_ZENABLE, FALSE), g_dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE), g_dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    g_dev->SetRenderState(D3DRS_COLORWRITEENABLE, writeMask);
    const float w = static_cast<float>(d.Width), h = static_cast<float>(d.Height);
    const float tri[3][4] = {{-0.5f, -0.5f, 0.5f, 1.0f}, {2.0f * w - 0.5f, -0.5f, 0.5f, 1.0f}, {-0.5f, 2.0f * h - 0.5f, 0.5f, 1.0f}};
    g_dev->SetVertexShader(nullptr), g_dev->SetFVF(D3DFVF_XYZRHW), g_dev->SetPixelShader(ps);
    g_dev->BeginScene();
    g_dev->DrawPrimitiveUP(D3DPT_TRIANGLELIST, 1, tri, sizeof(tri[0]));
    g_dev->EndScene();
}
static void Sampler(DWORD s, IDirect3DTexture9* t, bool linear = false) {
    g_dev->SetTexture(s, t);
    g_dev->SetSamplerState(s, D3DSAMP_MINFILTER, linear ? D3DTEXF_LINEAR : D3DTEXF_POINT), g_dev->SetSamplerState(s, D3DSAMP_MAGFILTER, linear ? D3DTEXF_LINEAR : D3DTEXF_POINT);
    g_dev->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    g_dev->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP), g_dev->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
}
static void Const(UINT r, float x, float y, float z, float w) {
    const float v[4] = {x, y, z, w};
    g_dev->SetPixelShaderConstantF(r, v, 1);
}

static IDirect3DPixelShader9 *g_flood = nullptr, *g_dist = nullptr;
static float FarOf(D3DFORMAT f) { return f == D3DFMT_A16B16G16R16F ? 32768.0f : 1e6f; } // (over any lot's diagonal in pixels; a half float ends at 65504)

// A story's field on the GPU: per band the CPU's seeds uploaded, the flood passes between two targets, the distance into the
// band's channel. cpu: the same story's CPU field, compared texel by texel.
static IDirect3DTexture9* BuildGpu(const Story& s, const Field& cpu, D3DFORMAT fmt, const char* name) {
    const int w = cpu.w, h = cpu.h, bands = static_cast<int>(cpu.cuts.size()) + 1;
    const size_t n = static_cast<size_t>(w) * h;
    IDirect3DTexture9 *seed = MakeTex(w, h, g_seedFmt, false), *a = MakeTex(w, h, g_seedFmt, true), *b = MakeTex(w, h, g_seedFmt, true), *field = MakeTex(w, h, fmt, true);
    if (!seed || !a || !b || !field) return std::printf("  %s: no textures\n", name), nullptr;
    const int sb = Bytes(g_seedFmt) / 4;
    std::vector<PlanField::Seed> seeds;
    std::vector<float> up(n * sb, 0.0f);
    const std::vector<int> steps = PlanField::JumpSteps(w, h);
    for (int band = 0; band < bands; band++) {
        PlanField::SeedBand(s, w, h, cpu.px, PlanField::BandMid(band, cpu.base, cpu.cuts), seeds);
        for (size_t i = 0; i < n; i++) {
            const bool none = seeds[i].x == PlanField::kNone;
            up[i * sb] = none ? -1e15f : seeds[i].x, up[i * sb + 1] = none ? -1e15f : seeds[i].z;
        }
        Fill(seed, up.data(), w, h, sb * 4);
        Const(1, static_cast<float>(w), static_cast<float>(h), 0, 0);
        IDirect3DTexture9 *src = seed, *dst = a;
        for (const int st : steps) {
            Const(0, 1.0f / w, 1.0f / h, static_cast<float>(st), FarOf(fmt));
            Sampler(0, src);
            Pass(dst, g_flood);
            src = dst, dst = dst == a ? b : a;
        }
        Sampler(0, src);
        Pass(field, g_dist, 1u << band); // (D3DCOLORWRITEENABLE: red 1, green 2, blue 4, alpha 8)
    }
    g_dev->SetTexture(0, nullptr);
    seed->Release(), a->Release(), b->Release();
    // against the CPU's field
    const std::vector<uint8_t> raw = Read(field);
    long equal = 0, walls = 0;
    float worst = 0.0f;
    bool farSame = true;
    for (int band = 0; band < bands && !raw.empty(); band++)
        for (size_t i = 0; i < n; i++) {
            const float c = cpu.d[static_cast<size_t>(band) * n + i];
            float g;
            if (fmt == D3DFMT_A16B16G16R16F) {
                uint16_t v;
                std::memcpy(&v, &raw[(i * 4 + band) * 2], 2);
                g = HalfToFloat(v);
            } else
                std::memcpy(&g, &raw[(i * 4 + band) * 4], 4);
            if (c >= 1e6f) {
                farSame = farSame && g == FarOf(fmt);
                continue;
            }
            walls++, equal += std::memcmp(&g, &c, 4) == 0, worst = std::max(worst, std::fabs(g - c));
        }
    std::printf("  %s: field %d x %d, %d bands, %zu passes a band; %ld of %ld texels bit-equal to the CPU's, largest difference %.3g px\n", name, w, h, bands, steps.size() + 1, equal,
                walls, worst);
    Check(!raw.empty() && farSame, std::string(name) + ": a band without walls does not read the far distance");
    // (float: the square root may differ in the last bits; half float: 11 bits, 1 / 32 px at 64 px and beyond)
    Check(fmt == D3DFMT_A16B16G16R16F ? worst <= cpu.w * 1.5f / 2048.0f : worst <= 1e-3f, std::string(name) + ": the GPU's field is off the CPU's");
    return field;
}

struct TraceOut {
    std::vector<uint8_t> clear, used;
};
// PlanTrace for every (A[i], B[j]): index j * A.size() + i
static TraceOut TraceGpu(IDirect3DTexture9* field, const Field& f, const std::vector<P3>& A, const std::vector<P3>& B, IDirect3DPixelShader9* ps, bool filtered) {
    const int na = static_cast<int>(A.size()), nb = static_cast<int>(B.size());
    IDirect3DTexture9 *ta = MakeTex(na, 1, D3DFMT_A32B32G32R32F, false), *tb = MakeTex(nb, 1, D3DFMT_A32B32G32R32F, false), *out = MakeTex(na, nb, D3DFMT_A8R8G8B8, true);
    const auto fill = [](IDirect3DTexture9* t, const std::vector<P3>& p) {
        std::vector<float> v(p.size() * 4, 0.0f);
        for (size_t i = 0; i < p.size(); i++) v[i * 4] = p[i][0], v[i * 4 + 1] = p[i][1], v[i * 4 + 2] = p[i][2];
        Fill(t, v.data(), static_cast<int>(p.size()), 1, 16);
    };
    fill(ta, A), fill(tb, B);
    float cuts[4] = {1e9f, 1e9f, 1e9f, 1e9f};
    for (size_t i = 0; i < f.cuts.size() && i < 3; i++) cuts[i] = f.cuts[i];
    Const(2, static_cast<float>(f.px), static_cast<float>(PlanField::kPad), static_cast<float>(f.w), static_cast<float>(f.h));
    Const(3, 1.0f / f.w, 1.0f / f.h, f.base, static_cast<float>(f.cuts.size()));
    g_dev->SetPixelShaderConstantF(4, cuts, 1);
    Const(5, 1.0f / na, 1.0f / nb, 0, 0);
    Sampler(1, field, filtered), Sampler(2, ta), Sampler(3, tb);
    Pass(out, ps);
    const std::vector<uint8_t> raw = Read(out);
    TraceOut r;
    r.clear.resize(raw.size() / 4), r.used.resize(raw.size() / 4);
    for (size_t i = 0; i < r.clear.size(); i++) r.clear[i] = raw[i * 4 + 2] > 127, r.used[i] = raw[i * 4 + 1]; // (B8G8R8A8 in memory)
    g_dev->SetTexture(1, nullptr), g_dev->SetTexture(2, nullptr), g_dev->SetTexture(3, nullptr);
    ta->Release(), tb->Release(), out->Release();
    return r;
}

struct Case {
    const char* name;
    Lot lot;
    std::vector<P3> A, B;
    bool half = false; // A is B: the pairs i < j only
    bool door = false; // the doorway: the cone checks of S0-06
};
struct Variant {
    const char* name;
    D3DFORMAT fmt;
    bool filtered;
};

int main() {
    (void)&PlanFieldChecks; // (the CPU cases run in tools/rt_lamps_test)
    if (!MakeDevice()) return std::printf("no D3D9 device\n"), 2;
    std::printf("\n[shaders] ps_3_0, the compiler's counts\n");
    g_flood = Compile("FloodPS"), g_dist = Compile("DistPS");
    const int budgets[] = {16, 32, 128}; // (128: over the 104 steps of the longest clear trace of the cases, for S0-06's checks)
    IDirect3DPixelShader9* trace[3][2] = {};
    for (int i = 0; i < 3; i++)
        for (int fl = 0; fl < 2; fl++) trace[i][fl] = Compile("TracePS", budgets[i], fl != 0);
    Check(g_flood && g_dist && trace[0][0] && trace[1][0] && trace[2][0] && trace[0][1] && trace[1][1] && trace[2][1], "plan field gpu: a shader does not compile");
    if (failures) return 1;

    const std::initializer_list<float> ys = {0.05f, 0.8f, 1.5f, 2.5f, 2.9f};
    std::vector<P3> ptsA = GridPts(1.2f, 5.6f, 1.2f, 6.8f, 0.4f, ys), ptsB = GridPts(6.4f, 10.8f, 1.2f, 6.8f, 0.4f, ys);
    for (const P3& p : GridPts(5.9f, 5.9f, 1.2f, 6.8f, 0.4f, ys)) ptsA.push_back(p);
    for (const P3& p : GridPts(6.1f, 6.1f, 1.2f, 6.8f, 0.4f, ys)) ptsB.push_back(p);
    std::vector<Case> cases;
    cases.push_back({"closed wall", TwoRooms(), ptsA, ptsB});
    {
        Case c{"doorway", TwoRooms(), ptsA, ptsB, false, true};
        Edge* door = EdgeAt(c.lot.stories[0], 6, 3, 6, 4);
        door->open = true, door->o.u0 = 0.1f, door->o.u1 = 0.9f, door->o.sill = 0.0f;
        cases.push_back(std::move(c));
    }
    {
        Case c{"diagonal wall", {}, {}, {}, true};
        Story s = MakeStory(0, 7, 7, 0.0f);
        Box(s, 1, 1, 6, 6);
        Line(s, 1, 1, 6, 6);
        c.lot.stories.push_back(std::move(s));
        for (const P3& p : GridPts(1.1f, 5.9f, 1.1f, 5.9f, 0.2f, {1.0f}))
            if (std::fabs(p[0] - p[2]) * 0.70710678f >= 0.08f) c.A.push_back(p);
        c.B = c.A;
        cases.push_back(std::move(c));
    }
    Lot window = TwoRooms();
    {
        Edge* win = EdgeAt(window.stories[0], 6, 3, 6, 4);
        win->open = true, win->glazed = 1, win->o.u0 = 0.1f, win->o.u1 = 0.9f, win->o.sill = 0.9f;
        for (float& hd : win->o.head) hd = 2.1f;
        for (const float z : {5.0f, 6.0f}) {
            Edge* half = EdgeAt(window.stories[0], 6, z, 6, z + 1);
            half->topL = half->topM = half->topR = 1.0f;
        }
        cases.push_back({"window + half wall", window, ptsA, ptsB});
    }
    const Variant variants[] = {{"float, 4 point reads", D3DFMT_A32B32G32R32F, false}, {"half float, 4 point reads", D3DFMT_A16B16G16R16F, false}, {"half float, filtered", D3DFMT_A16B16G16R16F, true}};
    const bool halfFilter = SUCCEEDED(g_d3d->CheckDeviceFormat(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, D3DUSAGE_QUERY_FILTER, D3DRTYPE_TEXTURE, D3DFMT_A16B16G16R16F));

    for (const int px : {16, 32}) {
        std::printf("\n[%d px a tile]\n", px);
        for (const Case& c : cases) {
            const Story& s = c.lot.stories[0];
            const Field cpu = PlanField::BuildField(s, PlanField::kCuts, px);
            const size_t na = c.A.size(), nb = c.B.size();
            std::vector<uint8_t> twin(na * nb), cpuClear[3], cpuUsed[3];
            for (int k = 0; k < 3; k++) cpuClear[k].resize(na * nb), cpuUsed[k].resize(na * nb);
            for (size_t j = 0; j < nb; j++)
                for (size_t i = 0; i < na; i++) {
                    twin[j * na + i] = PlanField::TwinClear(c.lot, c.A[i].data(), c.B[j].data());
                    for (int k = 0; k < 3; k++) {
                        int steps = budgets[k];
                        cpuClear[k][j * na + i] = PlanField::TraceStory(cpu, c.A[i].data(), c.B[j].data(), steps);
                        cpuUsed[k][j * na + i] = static_cast<uint8_t>(budgets[k] - steps);
                    }
                }
            for (const Variant& v : variants) {
                if (v.filtered && !halfFilter) continue;
                const std::string name = std::string(c.name) + ", " + v.name;
                IDirect3DTexture9* field = BuildGpu(s, cpu, v.fmt, name.c_str());
                if (!field) {
                    Check(false, name + ": no field");
                    continue;
                }
                for (int k = 0; k < 3; k++) {
                    const TraceOut g = TraceGpu(field, cpu, c.A, c.B, trace[k][v.filtered], v.filtered);
                    if (g.clear.size() != na * nb) {
                        Check(false, name + ": no trace result");
                        continue;
                    }
                    long pairs = 0, differ = 0, opened = 0, stepsDiffer = 0, outside = 0, well = 0, wellSeen = 0;
                    Tally vsTwin;
                    const float p16 = 1.0f / 16.0f;
                    for (size_t j = 0; j < nb; j++)
                        for (size_t i = c.half ? j + 1 : 0; i < na; i++) {
                            const size_t at = j * na + i;
                            const bool f = g.clear[at] != 0;
                            pairs++, differ += f != (cpuClear[k][at] != 0), opened += f && !cpuClear[k][at], stepsDiffer += g.used[at] != cpuUsed[k][at];
                            vsTwin.Add(f, twin[at] != 0);
                            if (!c.door) continue;
                            const P3 &a = c.A[i], &b = c.B[j]; // (the cone and "well inside" of plan_field_check.h's doorway)
                            const float r = (6.0f - a[0]) / (b[0] - a[0]), zc = a[2] + (b[2] - a[2]) * r, yc = a[1] + (b[1] - a[1]) * r;
                            const bool cone = zc > 3.1f - 0.5f * p16 && zc < 3.9f + 0.5f * p16 && yc < 2.2f + 1e-3f;
                            const float yr = std::fabs(b[1] - a[1]) * 2.0f * p16 / std::max(std::hypot(b[0] - a[0], b[2] - a[2]), 1e-6f);
                            const bool in = yc + yr < 2.15f && zc > 3.1f && zc < 3.9f && PlanDist(a, b, 6.0f, 3.1f) > 2.0f * p16 && PlanDist(a, b, 6.0f, 3.9f) > 2.0f * p16;
                            outside += f && !cone, well += in, wellSeen += in && f;
                        }
                    std::printf("    %3d steps: %ld pairs, %ld differ from the CPU's trace (%ld seen where the CPU blocks), %ld differ in steps; against the twin: %ld seen, leak %.3f %% dark %.3f %%",
                                budgets[k], pairs, differ, opened, stepsDiffer, vsTwin.clear - vsTwin.dark + vsTwin.leak, vsTwin.Leak(), vsTwin.Dark());
                    if (c.door) std::printf("; %ld outside the cone, %ld of %ld well inside seen", outside, wellSeen, well);
                    std::printf("\n");
                    // The tolerances. Exact field and reads: the verdict may flip where a step lands within rounding of a threshold.
                    // Half float or filtered reads: up to 1 / 32 px off near the hit radius, on the pairs that graze a jamb.
                    Check(differ * (v.fmt == D3DFMT_A32B32G32R32F ? 10000 : 500) <= pairs, name + ": the GPU's trace is off the CPU's");
                    Check(vsTwin.Leak() < 0.5, name + ": the GPU's trace passes what the twin blocks");
                    if (c.door) Check(outside == 0 && (k < 2 || wellSeen == well) && vsTwin.clear > vsTwin.dark, name + ": the doorway's cone on the GPU");
                    else if (!twin.empty() && vsTwin.clear == 0) Check(vsTwin.leak == 0, name + ": light crosses a closed wall on the GPU");
                    if (c.half) Check(vsTwin.leak == 0, name + ": light crosses the diagonal wall on the GPU");
                }
                if (&c == &cases.back()) { // S0-06's window and half wall probes: pair i is (a[i], b[i])
                    const std::vector<P3> a = {{4, 1.5f, 3.5f}, {4, 0.3f, 3.5f}, {4, 2.6f, 3.5f}, {4, 1.5f, 6}, {4, 0.3f, 6}, {4, 2.5f, 6}};
                    const std::vector<P3> b = {{8, 1.5f, 3.5f}, {8, 0.3f, 3.5f}, {8, 2.6f, 3.5f}, {8, 1.5f, 6}, {8, 0.3f, 6}, {8, 0.05f, 6}};
                    const TraceOut g = TraceGpu(field, cpu, a, b, trace[1][v.filtered], v.filtered);
                    const auto see = [&](int i) { return g.clear.size() == 36 && g.clear[i * 6 + i] != 0; };
                    Check(see(0) && !see(1) && !see(2), name + ": a window passes light between its sill and head only");
                    Check(see(3) && !see(4) && see(5), name + ": a half wall blocks under its top only");
                }
                field->Release();
            }
        }
    }
    std::printf("\n%d checks, %d failed\n", checks, failures);
    return failures != 0;
}
