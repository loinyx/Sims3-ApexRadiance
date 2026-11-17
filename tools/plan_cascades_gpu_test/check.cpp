// Real-time GI's radiance cascades on the GPU (shaders/plan_cascades_hlsl.h, ps_3_0) against their CPU form
// (features/plan_cascades.h), work package X-05: every level's image and the gather, value by value, on the doorway case of
// tools/rt_lamps_test/plan_cascades_case.h, with and without the fix, into float and half-float targets; then the time of
// the passes on a 64 x 64 tile story by level count. Needs a D3D9 GPU; read-only, prints to stdout.
#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>
#include <chrono>
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
#include "tools/rt_lamps_test/plan_cascades_case.h"
#include "shaders/plan_cascades_hlsl.h"

using namespace PlanCascadesTest;
using namespace PlanCascades;

static IDirect3D9* g_d3d = nullptr;
static IDirect3DDevice9* g_dev = nullptr;

static bool Usable(D3DFORMAT f, DWORD usage) { return SUCCEEDED(g_d3d->CheckDeviceFormat(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, D3DFMT_X8R8G8B8, usage, D3DRTYPE_TEXTURE, f)); }
static bool MakeDevice() {
    WNDCLASSA wc{};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "ApexPlanCascadesGpu";
    RegisterClassA(&wc);
    HWND wnd = CreateWindowA(wc.lpszClassName, "plan cascades gpu", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
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
    std::printf("caps: ps_3_0 instruction slots %lu | blended targets: A16B16G16R16F %d, A32B32G32R32F %d | bilinear A16B16G16R16F %d\n", caps.MaxPixelShader30InstructionSlots,
                Usable(D3DFMT_A16B16G16R16F, D3DUSAGE_RENDERTARGET | D3DUSAGE_QUERY_POSTPIXELSHADER_BLENDING), Usable(D3DFMT_A32B32G32R32F, D3DUSAGE_RENDERTARGET | D3DUSAGE_QUERY_POSTPIXELSHADER_BLENDING),
                Usable(D3DFMT_A16B16G16R16F, D3DUSAGE_QUERY_FILTER));
    return true;
}

// Compiles an entry of kPlanCascadesHlsl for ps_3_0 and prints the compiler's slot counts
static IDirect3DPixelShader9* Compile(const char* entry, const char* def = nullptr) {
    const D3D_SHADER_MACRO defs[] = {{def ? def : "CC_NONE", "1"}, {nullptr, nullptr}};
    ID3DBlob *code = nullptr, *err = nullptr, *text = nullptr;
    if (FAILED(D3DCompile(kPlanCascadesHlsl, std::strlen(kPlanCascadesHlsl), "plan_cascades", defs, nullptr, entry, "ps_3_0", 0, 0, &code, &err))) {
        std::printf("%s does not compile: %s\n", entry, err ? static_cast<const char*>(err->GetBufferPointer()) : "");
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
    std::printf("  %-10s %-12s: %s\n", entry, def ? def : "", slots.c_str());
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
static float HalfToFloat(uint16_t v) {
    const int e = (v >> 10) & 31, m = v & 1023;
    const float f = e == 0 ? std::ldexp(static_cast<float>(m), -24) : std::ldexp(static_cast<float>(m + 1024), e - 25);
    return v & 0x8000 ? -f : f;
}
static uint16_t FloatToHalf(float f) { // (0 <= f < 65504, to nearest)
    if (!(f > 0.0f)) return 0;
    int e;
    const float m = std::frexp(f, &e); // f = m 2^e, m in [0.5, 1)
    if (e < -13) return static_cast<uint16_t>(std::lround(std::ldexp(f, 24)));
    const long q = std::lround(std::ldexp(m, 11)); // 1024 .. 2048
    return q == 2048 ? static_cast<uint16_t>((e + 15) << 10) : static_cast<uint16_t>(((e + 14) << 10) | (q - 1024));
}
// w x h x 4 floats into a managed half-float texture; the floats are left as the texture holds them
static IDirect3DTexture9* HalfTex(std::vector<float>& v, int w, int h) {
    IDirect3DTexture9* t = MakeTex(w, h, D3DFMT_A16B16G16R16F, false);
    D3DLOCKED_RECT lr{};
    if (!t || FAILED(t->LockRect(0, &lr, nullptr, 0))) return nullptr;
    for (int y = 0; y < h; y++) {
        uint16_t* row = reinterpret_cast<uint16_t*>(static_cast<uint8_t*>(lr.pBits) + static_cast<size_t>(y) * lr.Pitch);
        for (int i = 0; i < w * 4; i++) row[i] = FloatToHalf(v[static_cast<size_t>(y) * w * 4 + i]), v[static_cast<size_t>(y) * w * 4 + i] = HalfToFloat(row[i]);
    }
    t->UnlockRect(0);
    return t;
}
// A float or half-float target as w x h x 4 floats
static std::vector<float> Read(IDirect3DTexture9* t) {
    D3DSURFACE_DESC d{};
    t->GetLevelDesc(0, &d);
    const size_t w = d.Width, h = d.Height;
    std::vector<float> out;
    IDirect3DSurface9 *sys = nullptr, *rt = nullptr;
    g_dev->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr);
    t->GetSurfaceLevel(0, &rt);
    D3DLOCKED_RECT lr{};
    if (sys && SUCCEEDED(g_dev->GetRenderTargetData(rt, sys)) && SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
        out.resize(w * h * 4);
        for (size_t y = 0; y < h; y++) {
            const uint8_t* row = static_cast<const uint8_t*>(lr.pBits) + y * lr.Pitch;
            if (d.Format == D3DFMT_A32B32G32R32F) std::memcpy(&out[y * w * 4], row, w * 16);
            else
                for (size_t i = 0; i < w * 4; i++) out[y * w * 4 + i] = HalfToFloat(reinterpret_cast<const uint16_t*>(row)[i]);
        }
        sys->UnlockRect();
    }
    if (rt) rt->Release();
    if (sys) sys->Release();
    return out;
}
// One pass over the whole of `target` (the passes read VPOS); clear: to 0 first; add: blended one, one
static void Pass(IDirect3DTexture9* target, IDirect3DPixelShader9* ps, bool clear, bool add) {
    D3DSURFACE_DESC d{};
    target->GetLevelDesc(0, &d);
    IDirect3DSurface9* rt = nullptr;
    target->GetSurfaceLevel(0, &rt);
    g_dev->SetRenderTarget(0, rt);
    rt->Release();
    if (clear) g_dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1.0f, 0);
    g_dev->SetRenderState(D3DRS_ZENABLE, FALSE), g_dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    g_dev->SetRenderState(D3DRS_ALPHABLENDENABLE, add), g_dev->SetRenderState(D3DRS_SRCBLEND, D3DBLEND_ONE), g_dev->SetRenderState(D3DRS_DESTBLEND, D3DBLEND_ONE);
    g_dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE), g_dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
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
// Waits until the device has run everything issued
static void Finish() {
    IDirect3DQuery9* q = nullptr;
    if (FAILED(g_dev->CreateQuery(D3DQUERYTYPE_EVENT, &q)) || !q) return;
    q->Issue(D3DISSUE_END);
    while (q->GetData(nullptr, 0, D3DGETDATA_FLUSH) == S_FALSE) {}
    q->Release();
}

static IDirect3DPixelShader9 *g_plain = nullptr, *g_fix = nullptr, *g_gather = nullptr, *g_gatherB = nullptr;

struct GpuRun {
    std::vector<IDirect3DTexture9*> lv;
    std::vector<double> ms; // per level
    int passes = 0;
};
// Every level's passes, last to first, into targets of `fmt`
static GpuRun RunGpu(const Field& f, IDirect3DTexture9* field, IDirect3DTexture9* emit, const Params& p, const Layout& l, D3DFORMAT fmt) {
    GpuRun r;
    r.lv.assign(p.levels, nullptr), r.ms.assign(p.levels, 0.0);
    for (IDirect3DTexture9*& t : r.lv) t = MakeTex(l.tw, l.th, fmt, true);
    Const(0, static_cast<float>(f.px), static_cast<float>(PlanField::kPad), static_cast<float>(f.w), static_cast<float>(f.h));
    Const(1, 1.0f / f.w, 1.0f / f.h, 0, 0);
    Const(2, p.band == 0, p.band == 1, p.band == 2, p.band == 3);
    Sampler(0, field, true), Sampler(1, emit);
    Finish();
    for (int i = p.levels - 1; i >= 0; i--) {
        const Level& g = l.lv[i];
        const bool top = i + 1 == p.levels;
        const Level up = top ? Level{1, 1, 1, 1, 1.0f, 0, 0} : l.lv[i + 1];
        Const(3, g.spacing, static_cast<float>(g.side), g.t0, g.t1);
        Const(4, up.spacing, static_cast<float>(up.side), static_cast<float>(up.nx), static_cast<float>(up.nz));
        Const(5, 1.0f / l.tw, 1.0f / l.th, static_cast<float>(g.dirs * 4), top ? 0.0f : 1.0f);
        Sampler(2, top ? emit : r.lv[i + 1]);
        const auto t0 = std::chrono::steady_clock::now();
        bool first = true;
        for (int c = 0; c < 4; c++)
            for (int k = 0; k < (p.fix && !top ? 4 : 1); k++) {
                Const(6, static_cast<float>(c), static_cast<float>(k & 1), static_cast<float>(k >> 1), p.capFar);
                Pass(r.lv[i], p.fix && !top ? g_fix : g_plain, first, true);
                first = false, r.passes++;
            }
        Finish();
        r.ms[i] = 1000.0 * std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    }
    return r;
}
static void Release(GpuRun& r) {
    for (DWORD s = 0; s < 3; s++) g_dev->SetTexture(s, nullptr);
    for (IDirect3DTexture9* t : r.lv)
        if (t) t->Release();
}
// The field's bands in the channels of a half-float texture ("no wall" as shaders/plan_field_hlsl.h writes it)
static IDirect3DTexture9* FieldTex(const Field& f) {
    const size_t n = static_cast<size_t>(f.w) * f.h, bands = f.cuts.size() + 1;
    std::vector<float> v(n * 4, 32768.0f);
    for (size_t b = 0; b < bands && b < 4; b++)
        for (size_t i = 0; i < n; i++) v[i * 4 + b] = std::min(f.d[b * n + i], 32768.0f);
    return HalfTex(v, f.w, f.h);
}

int main() {
    (void)&PlanFieldChecks, (void)&PlanCascadesChecks; // (the CPU cases run in tools/rt_lamps_test)
    if (!MakeDevice()) return std::printf("no D3D9 device\n"), 2;
    std::printf("\n[shaders] ps_3_0, the compiler's counts\n");
    g_plain = Compile("CascadePS"), g_fix = Compile("CascadePS", "CC_FIX"), g_gather = Compile("GatherPS"), g_gatherB = Compile("GatherPS", "CC_GATHER_B");
    Check(g_plain && g_fix && g_gather && g_gatherB, "plan cascades gpu: a shader does not compile");
    if (failures) return 1;

    { // the doorway: the GPU's images against the CPU's
        const RtLampProxy::Lot lot = DoorRooms();
        const Field f = PlanField::BuildField(lot.stories[0]);
        Emit e = LampEmit(f, 1, 3.5f, 4.0f, 1.0f, 6.0f, 1.0f, 7.0f);
        IDirect3DTexture9 *field = FieldTex(f), *emit = HalfTex(e.v, f.w, f.h); // (the CPU reads the emitter as the texture holds it)
        Check(field && emit, "plan cascades gpu: no input textures");
        for (const D3DFORMAT fmt : {D3DFMT_A32B32G32R32F, D3DFMT_A16B16G16R16F})
            for (const bool fix : {true, false}) {
                Params p;
                p.fix = fix;
                Layout l;
                std::vector<std::vector<float>> cpu;
                const std::vector<float> cpuOut = Solve(f, e, p, &l, &cpu);
                GpuRun g = RunGpu(f, field, emit, p, l, fmt);
                const std::string name = std::string(fmt == D3DFMT_A32B32G32R32F ? "float" : "half float") + (fix ? ", fix" : ", plain");
                std::printf("\n[doorway, %s] %d x %d a level, %d passes\n", name.c_str(), l.tw, l.th, g.passes);
                for (int i = p.levels - 1; i >= 0; i--) {
                    const std::vector<float> img = Read(g.lv[i]);
                    float peak = 0;
                    for (size_t k = 0; k < cpu[i].size(); k++) peak = std::max(peak, cpu[i][k]);
                    std::vector<float> a, b; // (rgb only)
                    for (size_t k = 0; k + 3 < cpu[i].size() && k + 3 < img.size(); k += 4) a.insert(a.end(), &img[k], &img[k] + 3), b.insert(b.end(), &cpu[i][k], &cpu[i][k] + 3);
                    const Diff d = Compare(a, b, 0.01f * peak);
                    std::printf("  level %d: mean |GPU - CPU| %.4f %% of the peak %.4f, largest %.2f %%, %.3f %% of %ld values within 1 %%\n", i, 100 * d.sum / d.n / peak, peak, 100 * d.worst / peak,
                                100.0 * d.within / d.n, d.n);
                    Check(!img.empty() && d.within >= 0.995 * d.n && d.sum / d.n < 0.002 * peak, name + ": level " + std::to_string(i) + " is off the CPU's");
                }
                IDirect3DTexture9 *ga = MakeTex(l.nx, l.nz, fmt, true), *gb = MakeTex(l.nx, l.nz, fmt, true);
                Const(5, 1.0f / l.tw, 1.0f / l.th, 0, 0);
                Sampler(2, g.lv[0]);
                Pass(ga, g_gather, false, false), Pass(gb, g_gatherB, false, false);
                const std::vector<float> ia = Read(ga), ib = Read(gb);
                Diff d;
                float peak = 0;
                for (const float v : cpuOut) peak = std::max(peak, v);
                long lit = 0;
                float litMax = 0;
                for (int k = 0; k < l.nx * l.nz && !ia.empty() && !ib.empty(); k++) {
                    for (int c = 0; c < 4; c++) d.Add(ia[k * 4 + c], cpuOut[k * kOut + c], 0.01f * peak);
                    for (int c = 0; c < 2; c++) d.Add(ib[k * 4 + c], cpuOut[k * kOut + 4 + c], 0.01f * peak);
                    const float x = (k % l.nx + 0.5f) * p.spacing, z = (k / l.nx + 0.5f) * p.spacing;
                    if (x > 11 || z < 1 || z > 7 || x < 1) lit += ib[k * 4] != 0.0f, litMax = std::max(litMax, ib[k * 4]); // (outside both rooms: nothing emits)
                }
                std::printf("  gather: mean |GPU - CPU| %.4f %% of the peak %.4f, largest %.2f %%, %.3f %% within 1 %%; %ld probes outside the rooms lit, largest %.5f %% of the peak\n",
                            d.n ? 100 * d.sum / d.n / peak : 0.0, peak, 100 * d.worst / peak, d.n ? 100.0 * d.within / d.n : 0.0, lit, 100 * litMax / peak);
                Check(d.n > 0 && d.within >= 0.995 * d.n, name + ": the gather is off the CPU's");
                // (not 0: beside a corner the field's pixel holds one of the two walls' seeds, and a ray entering there reads the sheets a quarter pixel deep)
                if (fix) Check(litMax < 1e-4f * peak, name + ": light outside the closed rooms on the GPU");
                ga->Release(), gb->Release();
                Release(g);
            }
        field->Release(), emit->Release();
    }
    { // the cost on a large story: 64 x 64 tiles of 8 x 8 tile rooms with doors, by level count
        RtLampProxy::Lot lot;
        lot.stories.push_back(PlanFieldTest::MakeStory(0, 64, 64, 0.0f));
        RtLampProxy::Story& s = lot.stories[0];
        for (int k = 0; k <= 64; k += 8) PlanFieldTest::Line(s, k, 0, k, 64), PlanFieldTest::Line(s, 0, k, 64, k);
        for (RtLampProxy::Edge& e : s.edges)
            if ((static_cast<int>(e.x0 + e.z0)) % 8 == 3) e.open = true, e.o.u0 = 0.1f, e.o.u1 = 0.9f, e.o.sill = 0.0f;
        const Field f = PlanField::BuildField(s);
        Emit e = LampEmit(f, 1, 30.0f, 30.0f, 0.0f, 64.0f, 0.0f, 64.0f, 50.0f);
        IDirect3DTexture9 *field = FieldTex(f), *emit = HalfTex(e.v, f.w, f.h);
        std::printf("\n[64 x 64 tiles, 8 x 8 tile rooms with doors] field %d x %d; ms a level, last level first (this machine, other work running: order of magnitude)\n", f.w, f.h);
        for (const float spacing : {0.25f, 0.5f})
            for (const bool fix : {true, false})
                for (const int levels : {1, 2, 3, 4}) {
                    Params p;
                    p.fix = fix, p.levels = levels, p.spacing = spacing, p.l0 = 4.0f * spacing;
                    const Layout l = LayoutOf(f, p);
                    double best = 1e9;
                    std::string per;
                    for (int rep = 0; rep < 3; rep++) {
                        GpuRun g = RunGpu(f, field, emit, p, l, D3DFMT_A16B16G16R16F);
                        double sum = 0;
                        std::string text;
                        for (int i = levels - 1; i >= 0; i--) sum += g.ms[i], text += (text.empty() ? "" : " + ") + std::to_string(g.ms[i]).substr(0, 5);
                        if (sum < best) best = sum, per = text;
                        Release(g);
                    }
                    std::printf("  %.2f m probes, l0 %.1f m, %s, %d level%s (to %3.0f m): %4d x %4d a level, %2d traces a texel: %7.2f ms (%s)\n", spacing, p.l0, fix ? "fix  " : "plain", levels,
                                levels > 1 ? "s" : " ", l.lv.back().t1, l.tw, l.th, fix ? 16 : 4, best, per.c_str());
                }
        field->Release(), emit->Release();
    }
    std::printf("\n%d checks, %d failed\n", checks, failures);
    return failures != 0;
}
