// Offline check of the shadow-mask filter and history (shaders/gi_pixel_filter_hlsl.h, real-time GI work package XR-05).
// Read-only: prints to stdout, writes no file. Needs a D3D9 GPU (native HAL device, hidden window).
// A CPU scene stands in for the tracer: floor, back wall, a lamp ball, a static caster and a class 3 caster (a Sim).
// Each frame it gives the quarter-resolution guide, motion, one visibility sample a pixel from the fixed 16-point set
// and the exact 16-sample mean; the production ps_3_0 passes run on the GPU and the picture is read back.
// Sequences: still camera, pan then stop, a moving caster then stop. Measured on static pixels: flicker (change between
// frames), error against the 16-sample reference through the same spatial passes, shadow trails, frames to settle.
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>
#include "../../shaders/gi_pixel_filter_hlsl.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

int g_checks = 0, g_failures = 0;
void Check(bool ok, const char* what) {
    g_checks++;
    if (!ok) g_failures++;
    std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
}

// ---- scene ----
const int FW = 640, FH = 360, QW = FW / 4, QH = FH / 4;
const double kFar = 20, kTanY = 0.57735026919, kTanX = kTanY * FW / FH;
struct V3 {
    double x, y, z;
};
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 a, double s) { return {a.x * s, a.y * s, a.z * s}; }
double Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
V3 Norm(V3 a) { return a * (1 / std::sqrt(Dot(a, a))); }

struct Cam {
    V3 pos, f, r, u;
    double yaw;
};
Cam MakeCam(double yawDeg) {
    const double yaw = yawDeg * 3.14159265358979 / 180, pitch = -12 * 3.14159265358979 / 180;
    Cam c;
    c.yaw = yawDeg;
    c.pos = {0, 1.6, -2.5};
    c.f = {std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch)};
    c.r = Norm(Cross({0, 1, 0}, c.f));
    c.u = Cross(c.f, c.r);
    return c;
}
const V3 kLamp = {0.6, 2.6, 2.0};
const double kLampR = 0.12;
struct Ball {
    V3 c;
    double r;
    int cls;
};
struct World {
    Ball sim{{-0.4, 0.45, 2.5}, 0.35, 3};  // class 3: unknown mover
    Ball shelf{{0.05, 1.58, 2.28}, 0.18, 0};  // static furniture detail, shadows the floor, the wall and the Sim
};
// Nearest hit of o + d * t with t in (0, tmax); d need not be unit
bool HitBall(const Ball& b, V3 o, V3 d, double tmax, double* t) {
    const V3 m = o - b.c;
    const double a = Dot(d, d), h = Dot(m, d), c = Dot(m, m) - b.r * b.r, disc = h * h - a * c;
    if (disc < 0) return false;
    const double t0 = (-h - std::sqrt(disc)) / a;
    if (t0 <= 0 || t0 >= tmax) return false;
    *t = t0;
    return true;
}
struct Hit {
    double t = kFar;
    V3 p{}, n{};
    int cls = 0, ball = -1;
};
Hit Primary(const Cam& c, const World& w, int x, int y) {
    const V3 d = c.f + c.r * (((x + 0.5) / FW * 2 - 1) * kTanX) + c.u * ((1 - (y + 0.5) / FH * 2) * kTanY);
    Hit h;
    h.n = c.f * -1;
    if (d.y < 0) h.t = -c.pos.y / d.y, h.n = {0, 1, 0};
    if (d.z > 0 && (6 - c.pos.z) / d.z < h.t) h.t = (6 - c.pos.z) / d.z, h.n = {0, 0, -1};
    h.t = std::min(h.t, kFar);
    const Ball* balls[2] = {&w.sim, &w.shelf};
    for (int i = 0; i < 2; i++) {
        double t;
        if (HitBall(*balls[i], c.pos, d, h.t, &t)) h.t = t, h.n = Norm(c.pos + d * t - balls[i]->c), h.cls = balls[i]->cls, h.ball = i;
    }
    h.p = c.pos + d * h.t;
    return h;
}
// Point i of the fixed 16-point set on the lamp ball (R2 sequence)
V3 LampPoint(int i) {
    const double u = std::fmod(0.5 + 0.7548776662466927 * i, 1.0), v = std::fmod(0.5 + 0.5698402909980532 * i, 1.0);
    const double z = 1 - 2 * u, s = std::sqrt(std::max(0.0, 1 - z * z)), phi = 6.283185307179586 * v;
    return kLamp + V3{s * std::cos(phi), s * std::sin(phi), z} * kLampR;
}
bool Lit(const World& w, const Hit& h, int i) {
    if (Dot(h.n, kLamp - h.p) <= 0) return true; // the lamp does not light this side: nothing to shadow
    const V3 o = h.p + h.n * 0.01, d = LampPoint(i) - o;
    const Ball* balls[2] = {&w.sim, &w.shelf};
    double t;
    for (int b = 0; b < 2; b++)
        if (b != h.ball && HitBall(*balls[b], o, d, 1, &t)) return false; // own proxy never shadows itself
    return true;
}
double SegDist(V3 a, V3 b, V3 p) {
    const V3 ab = b - a;
    const double t = std::clamp(Dot(p - a, ab) / Dot(ab, ab), 0.0, 1.0);
    const V3 d = a + ab * t - p;
    return std::sqrt(Dot(d, d));
}
const int kBayer[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};

struct Frame {
    std::vector<float> guide, motion, depth, refHist; // quarter rgba, quarter rgba, full r, quarter rgba
    std::vector<uint32_t> raw;                        // quarter A8R8G8B8
    std::vector<uint8_t> cls;                         // full: motion class
    V3 viewZ{};
    bool still = false;
};
// fixedMoverSample: a class 3 pixel has no history, so it always traces the same point of its set
Frame MakeFrame(const Cam& cam, const Cam& prev, const World& w, const World& wPrev, int index, bool fixedMoverSample = true) {
    Frame f;
    f.guide.resize(QW * QH * 4), f.motion.assign(QW * QH * 4, 0), f.refHist.resize(QW * QH * 4), f.raw.resize(QW * QH);
    f.depth.resize(FW * FH), f.cls.resize(FW * FH);
    f.viewZ = cam.f;
    f.still = cam.yaw == prev.yaw;
    const bool simMoved = w.sim.c.x != wPrev.sim.c.x;
    for (int y = 0; y < FH; y++)
        for (int x = 0; x < FW; x++) {
            const Hit h = Primary(cam, w, x, y);
            f.depth[y * FW + x] = float(h.t / kFar);
            f.cls[y * FW + x] = uint8_t(h.cls);
        }
    for (int qy = 0; qy < QH; qy++)
        for (int qx = 0; qx < QW; qx++) {
            int bx = qx * 4, by = qy * 4; // guide: the pixel of the 4x4 block nearest the camera
            for (int y = qy * 4; y < qy * 4 + 4; y++)
                for (int x = qx * 4; x < qx * 4 + 4; x++)
                    if (f.depth[y * FW + x] < f.depth[by * FW + bx]) bx = x, by = y;
            const Hit h = Primary(cam, w, bx, by);
            const int q = qy * QW + qx;
            const float g[4] = {float(h.n.x), float(h.n.y), float(h.n.z), float(h.t / kFar)};
            std::memcpy(&f.guide[q * 4], g, sizeof g);
            f.motion[q * 4 + 2] = h.cls / 3.0f;
            if (!f.still && h.cls == 0) {
                const V3 d = h.p - prev.pos;
                const double z = Dot(d, prev.f);
                f.motion[q * 4] = float((Dot(d, prev.r) / (z * kTanX) * 0.5 + 0.5) - (bx + 0.5) / FW);
                f.motion[q * 4 + 1] = float((0.5 - Dot(d, prev.u) / (z * kTanY) * 0.5) - (by + 0.5) / FH);
            }
            const int phase = kBayer[(qy & 3) * 4 + (qx & 3)];
            const bool sky = h.t >= kFar;
            const bool lit = sky || Lit(w, h, (phase + (h.cls == 3 && fixedMoverSample ? 0 : index)) & 15);
            int sum = 0;
            for (int i = 0; i < 16; i++) sum += sky || Lit(w, h, i);
            // Reach: the shadow ray's corridor to the lamp passes the moved caster at its old or new place
            const double reach = w.sim.r + kLampR;
            const bool flag = simMoved && !sky && h.ball != 0 &&
                (SegDist(h.p, kLamp, w.sim.c) < reach || SegDist(h.p, kLamp, wPrev.sim.c) < reach);
            f.raw[q] = 0xFF000000u | (lit ? 0xFF0000u : 0) | (flag ? 0xFF00u : 0);
            // Reference: the exact 16-sample mean; a class 3 pixel has none, its settled value is its fixed sample
            const float fixed = float(Lit(w, h, phase));
            const float r[4] = {h.cls == 3 ? fixed : float(sum), h.cls == 3 ? 1.f : 16.f, 0, 0};
            std::memcpy(&f.refHist[q * 4], r, sizeof r);
        }
    return f;
}

// ---- GPU ----
template <class T> void Release(T*& p) {
    if (p) p->Release();
    p = nullptr;
}
struct Options {
    bool reset = false;      // drop all history this frame
    bool spatial = true;     // a-trous passes on
    float clipCaster = 1.5f; // variance clip of the fast history near a moved caster (sigma, 0 = off)
    float clipCamera = 0;    // the same while the camera moves
};
struct Gpu {
    HWND window = nullptr;
    IDirect3D9* api = nullptr;
    IDirect3DDevice9* dev = nullptr;
    IDirect3DTexture9 *raw = nullptr, *guide[2] = {}, *motion = nullptr, *depth = nullptr, *refHist = nullptr;
    IDirect3DTexture9 *hist[2] = {}, *tmp[2] = {}, *out = nullptr;
    IDirect3DSurface9* readback = nullptr;
    IDirect3DPixelShader9 *temporal = nullptr, *atrousFirst = nullptr, *atrous = nullptr, *upsample = nullptr;
    int cur = 0; // index of the newest hist / guide

    IDirect3DPixelShader9* Compile(const char* entry) {
        ID3DBlob *code = nullptr, *err = nullptr;
        IDirect3DPixelShader9* ps = nullptr;
        if (SUCCEEDED(D3DCompile(kGiPixelFilterHlsl, std::strlen(kGiPixelFilterHlsl), "gi_pixel_filter", nullptr, nullptr, entry, "ps_3_0", 0, 0, &code, &err))) {
            dev->CreatePixelShader(static_cast<const DWORD*>(code->GetBufferPointer()), &ps);
            std::printf("  %s: %u bytes of ps_3_0\n", entry, unsigned(code->GetBufferSize()));
        } else if (err) std::printf("%s\n", static_cast<const char*>(err->GetBufferPointer()));
        Release(code), Release(err);
        return ps;
    }
    IDirect3DTexture9* Tex(int w, int h, D3DFORMAT fmt, bool target) {
        IDirect3DTexture9* t = nullptr;
        dev->CreateTexture(w, h, 1, target ? D3DUSAGE_RENDERTARGET : 0, fmt, target ? D3DPOOL_DEFAULT : D3DPOOL_MANAGED, &t, nullptr);
        return t;
    }
    bool Init() {
        window = CreateWindowExW(0, L"STATIC", L"Apex GI pixel filter check", WS_OVERLAPPED, 0, 0, 64, 64, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        api = Direct3DCreate9(D3D_SDK_VERSION);
        D3DPRESENT_PARAMETERS p{};
        p.Windowed = TRUE, p.SwapEffect = D3DSWAPEFFECT_DISCARD, p.BackBufferWidth = p.BackBufferHeight = 64, p.BackBufferFormat = D3DFMT_X8R8G8B8, p.hDeviceWindow = window;
        if (!api || FAILED(api->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, D3DCREATE_SOFTWARE_VERTEXPROCESSING, &p, &dev))) return false;
        raw = Tex(QW, QH, D3DFMT_A8R8G8B8, false), motion = Tex(QW, QH, D3DFMT_A32B32G32R32F, false);
        depth = Tex(FW, FH, D3DFMT_R32F, false), refHist = Tex(QW, QH, D3DFMT_A32B32G32R32F, false);
        out = Tex(FW, FH, D3DFMT_A8R8G8B8, true);
        for (int i = 0; i < 2; i++) {
            guide[i] = Tex(QW, QH, D3DFMT_A32B32G32R32F, false);
            hist[i] = Tex(QW, QH, D3DFMT_A16B16G16R16F, true), tmp[i] = Tex(QW, QH, D3DFMT_A16B16G16R16F, true);
        }
        dev->CreateOffscreenPlainSurface(FW, FH, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &readback, nullptr);
        temporal = Compile("TemporalPS"), atrousFirst = Compile("AtrousFirstPS"), atrous = Compile("AtrousPS"), upsample = Compile("UpsamplePS");
        if (!raw || !motion || !depth || !refHist || !out || !guide[1] || !hist[1] || !tmp[1] || !readback || !temporal || !atrousFirst || !atrous || !upsample) return false;
        dev->SetRenderState(D3DRS_ZENABLE, FALSE), dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE), dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
        for (DWORD s = 0; s < 7; s++) {
            dev->SetSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_POINT), dev->SetSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_POINT), dev->SetSamplerState(s, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
            dev->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP), dev->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        }
        return true;
    }
    void Upload(IDirect3DTexture9* t, const void* data, int h, size_t rowBytes) {
        D3DLOCKED_RECT r;
        if (FAILED(t->LockRect(0, &r, nullptr, 0))) return;
        for (int y = 0; y < h; y++) std::memcpy(static_cast<char*>(r.pBits) + y * r.Pitch, static_cast<const char*>(data) + y * rowBytes, rowBytes);
        t->UnlockRect(0);
    }
    void Pass(IDirect3DPixelShader9* ps, IDirect3DTexture9* target, int w, int h) {
        IDirect3DSurface9* s = nullptr;
        target->GetSurfaceLevel(0, &s);
        dev->SetRenderTarget(0, s);
        Release(s);
        const float v[4][6] = {{-0.5f, -0.5f, 0, 1, 0, 0}, {w - 0.5f, -0.5f, 0, 1, 1, 0}, {-0.5f, h - 0.5f, 0, 1, 0, 1}, {w - 0.5f, h - 0.5f, 0, 1, 1, 1}};
        dev->SetPixelShader(ps);
        dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
        dev->BeginScene();
        dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof v[0]);
        dev->EndScene();
    }
    void Atrous(bool first, float step, float strength, float low, IDirect3DTexture9* in, IDirect3DTexture9* target) {
        const float c[4] = {step, strength, low, 0};
        dev->SetPixelShaderConstantF(4, c, 1);
        dev->SetTexture(first ? 3 : 5, in);
        Pass(first ? atrousFirst : atrous, target, QW, QH);
        dev->SetTexture(first ? 3 : 5, nullptr);
    }
    // Spatial passes and upsample from a history texture, then the read-back picture (one byte a pixel)
    std::vector<uint8_t> Show(IDirect3DTexture9* history, bool spatial) {
        Atrous(true, 1, spatial ? 1.f : 0.f, 0, history, tmp[0]);
        if (spatial) {
            Atrous(false, 2, 1, 0, tmp[0], tmp[1]);
            Atrous(false, 4, 0, 1, tmp[1], tmp[0]); // the wide pass only where the count is low
        }
        dev->SetTexture(5, tmp[0]);
        Pass(upsample, out, FW, FH);
        dev->SetTexture(5, nullptr);
        IDirect3DSurface9* s = nullptr;
        out->GetSurfaceLevel(0, &s);
        dev->GetRenderTargetData(s, readback);
        Release(s);
        std::vector<uint8_t> px(FW * FH);
        D3DLOCKED_RECT r;
        if (SUCCEEDED(readback->LockRect(&r, nullptr, D3DLOCK_READONLY))) {
            for (int y = 0; y < FH; y++)
                for (int x = 0; x < FW; x++) px[y * FW + x] = static_cast<const uint8_t*>(r.pBits)[y * r.Pitch + x * 4];
            readback->UnlockRect();
        }
        return px;
    }
    void Bind(const Frame& f) {
        Upload(raw, f.raw.data(), QH, QW * 4), Upload(guide[cur], f.guide.data(), QH, QW * 16), Upload(motion, f.motion.data(), QH, QW * 16);
        Upload(depth, f.depth.data(), FH, FW * 4), Upload(refHist, f.refHist.data(), QH, QW * 16);
        dev->SetTexture(0, raw), dev->SetTexture(1, guide[cur]), dev->SetTexture(2, motion), dev->SetTexture(6, depth);
        const float size[4] = {float(QW), float(QH), 1.f / QW, 1.f / QH};
        const float tol[4] = {0.02f, float(1.5 * 2 * kTanY / QH), 0.9f, 4};
        const float viewZ[4] = {float(f.viewZ.x), float(f.viewZ.y), float(f.viewZ.z), 0};
        dev->SetPixelShaderConstantF(0, size, 1), dev->SetPixelShaderConstantF(2, tol, 1), dev->SetPixelShaderConstantF(3, viewZ, 1);
    }
    // One frame of the filter; `reference` gets the exact 16-sample mean through the same spatial passes
    std::vector<uint8_t> Run(const Frame& f, const Options& o, std::vector<uint8_t>* reference = nullptr) {
        cur ^= 1;
        Bind(f);
        const float t[4] = {f.still ? 1.f : 0.f, o.reset ? 1.f : 0.f, o.clipCaster, o.clipCamera};
        dev->SetPixelShaderConstantF(1, t, 1);
        dev->SetTexture(3, hist[cur ^ 1]), dev->SetTexture(4, guide[cur ^ 1]);
        Pass(temporal, hist[cur], QW, QH);
        dev->SetTexture(3, nullptr);
        if (reference) *reference = Show(refHist, true);
        return Show(hist[cur], o.spatial);
    }
};

// ---- measures (values are bytes: 1 = 1/255 of full visibility) ----
struct Diff {
    double mean = 0, within4 = 0;
    int max = 0;
};
Diff Compare(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, const std::vector<uint8_t>& clsA, const std::vector<uint8_t>& clsB) {
    Diff d;
    long long n = 0, sum = 0, in4 = 0;
    for (size_t i = 0; i < a.size(); i++) {
        if (clsA[i] || clsB[i]) continue; // static pixels only
        const int e = std::abs(int(a[i]) - int(b[i]));
        n++, sum += e, in4 += e <= 4, d.max = std::max(d.max, e);
    }
    d.mean = double(sum) / n, d.within4 = 100.0 * in4 / n;
    return d;
}

struct Row {
    double flicker = 0, error = 0, within4 = 100; // means over the moving frames; within4 is the worst frame
    int trail = 0, lastChange = 0, endMax = 0;
};
// 100 frames: frame 0 starts without history, 0-19 stand still, 20-59 move (camera yaw or the Sim), 60-99 stand still.
// kind: 0 = still, 1 = pan (0.4 degrees a frame), 2 = the Sim walks (0.06 m a frame)
Row Sequence(Gpu& g, int kind, const char* name, Options o, bool resetEachFrame, bool fixedMoverSample = true) {
    Row row;
    std::vector<uint8_t> prev, prevCls, ref, age(FW * FH, 0);
    Cam prevCam = MakeCam(0);
    World prevWorld;
    if (kind == 2) prevWorld.sim.c.x = -1.6;
    double popSum = 0;
    for (int k = 0; k < 100; k++) {
        const int step = std::clamp(k - 19, 0, 40);
        const Cam cam = MakeCam(kind == 1 ? 0.4 * step : 0);
        World w;
        if (kind == 2) w.sim.c.x = -1.6 + 0.06 * step;
        const Frame f = MakeFrame(cam, prevCam, w, prevWorld, k, fixedMoverSample);
        o.reset = k == 0 || resetEachFrame;
        const std::vector<uint8_t> out = g.Run(f, o, &ref);
        const std::vector<uint8_t> none(FW * FH, 0);
        if (k) {
            // Flicker: change between frames. Still and walk: every pixel that is static in both frames (still: all,
            // a standing Sim must not twinkle either). Pan: not defined on screen pixels, the error column stands in.
            const Diff change = Compare(out, prev, kind == 0 ? none : f.cls, kind == 0 ? none : prevCls);
            if (change.max) row.lastChange = k;
            if (kind == 0 ? k < 16 : k >= 20 && k < 60) row.flicker += change.mean / (kind == 0 ? 15 : 40);
            if (kind == 0 ? k < 16 : k >= 20 && k < 60) {
                const Diff e = Compare(out, ref, f.cls, f.cls);
                row.error += e.mean / (kind == 0 ? 15 : 40), row.within4 = std::min(row.within4, e.within4);
            }
        }
        // Trail: a static pixel out of the moved caster's reach that the reference shows fully lit and that is still
        // 8/255 or more darker, in frames in a row. (In reach the wider kernel of a low count is a halo, not a trail.)
        for (int i = 0; i < FW * FH; i++) {
            const bool inReach = (f.raw[(i / FW / 4) * QW + i % FW / 4] & 0xFF00u) != 0;
            age[i] = (!f.cls[i] && !inReach && ref[i] >= 253 && out[i] + 8 <= ref[i]) ? uint8_t(age[i] + 1) : 0;
            if (k >= 20) row.trail = std::max(row.trail, int(age[i]));
        }
        if (k == 99) row.endMax = Compare(out, ref, f.cls, f.cls).max;
        prev = out, prevCls = f.cls, prevCam = cam, prevWorld = w;
    }
    std::printf("  %-40s change/frame %.3f | error %.2f, worst frame %.1f %% within 4/255 | trail %d frames | last change frame %d | end max error %d\n",
        name, row.flicker, row.error, row.within4, row.trail, row.lastChange, row.endMax);
    return row;
}

} // namespace

int main() {
    Gpu g;
    if (!g.Init()) {
        std::printf("FAIL: D3D9 device, float targets or shader compilation unavailable\n");
        return 1;
    }
    Options raw, spatial, full, clipCamera, noClip;
    raw.spatial = false;
    clipCamera.clipCamera = 1.5f;
    noClip.clipCaster = 0;
    std::printf("Values in 1/255 of full visibility, %dx%d picture, %dx%d mask, static pixels.\n", FW, FH, QW, QH);

    std::printf("Still camera (change/frame and error over frames 2-16, the settling):\n");
    const Row stillRaw = Sequence(g, 0, "raw sample, no filter", raw, true);
    const Row stillSpatial = Sequence(g, 0, "spatial filter only", spatial, true);
    const Row still = Sequence(g, 0, "spatial + history", full, false);
    const Row stillCycling = Sequence(g, 0, "same, class 3 pixels cycle their set", full, false, false);
    Check(still.lastChange <= 15, "still: frames 17-80 byte-equal while the passes keep running");
    Check(still.endMax == 0, "still: the settled picture equals the 16-sample reference");
    Check(still.flicker < stillSpatial.flicker && stillSpatial.flicker < stillRaw.flicker, "still: history flickers less than spatial only, spatial only less than raw");
    Check(stillCycling.lastChange > 15, "still: a class 3 pixel that cycles its sample set never settles (so the tracer must not cycle it)");

    std::printf("Pan 0.4 degrees a frame for 40 frames after 20 still frames, then still (columns over the moving frames):\n");
    Sequence(g, 1, "spatial filter only", spatial, true);
    const Row pan = Sequence(g, 1, "spatial + history", full, false);
    Sequence(g, 1, "same + variance clip while panning", clipCamera, false);
    Check(pan.within4 >= 99, "pan: 99 % of static pixels within 4/255 of the reference in every frame");
    Check(pan.lastChange <= 75 && pan.endMax == 0, "pan: 16 frames after the stop the picture is the reference and byte-equal from then on");

    std::printf("Sim walks 0.06 m a frame for 40 frames after 20 still frames, then stands (columns over the moving frames):\n");
    Sequence(g, 2, "spatial filter only", spatial, true);
    Sequence(g, 2, "spatial + history, no clip", noClip, false);
    const Row walk = Sequence(g, 2, "spatial + history", full, false);
    Check(walk.trail <= 4, "walk: no static pixel keeps a shadow for more than 4 frames after the caster left");
    Check(walk.lastChange <= 75 && walk.endMax == 0, "walk: 16 frames after the stop the picture is the reference and byte-equal from then on");

    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
