// Ambient Occlusion options, offline check (read-only: prints to stdout, writes nothing).
//
// Compiles every GtaoPS variant of patches/ambient_occlusion_patch.cpp (5 qualities x the options Fewer directions when far
// and Smooth flat surfaces), checks the instruction slots and that native D3D9 accepts each one, then renders a synthetic
// depth buffer (floor, wall, a counter whose top has a 0.03 degree crease, a few house-sized boxes; 24-bit device depth)
// through the real GtaoPS and BlurPS passes on a hidden D3D9 device and compares the options with the default pass:
//  - Fewer directions when far: up close the image equals the default pass; at mid and far views the difference stays
//    small and the flat floor stays as smooth as with the default (the 4x4 interleave is still cancelled);
//  - Smooth flat surfaces: stray shade on surfaces where nothing meets (counter top, open floor), contact shade kept;
//  - Steady direction count: the count does not switch while the view alternates around a threshold, and still
//    switches past the band;
//  - Fewer directions in the open: the share of 4x4 blocks at half the count, the contact shade kept, and the shade
//    step across block edges where the count changes against the same pixels of the default image (blotches raise it);
//  - GPU time of the AO pass at 3840 x 2160 for each variant (timestamp queries).
#define NOMINMAX
#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

int g_checks = 0, g_failures = 0;
void Check(bool ok, const std::string& what) {
    g_checks++;
    if (!ok) {
        g_failures++;
        std::printf("  FAIL %s\n", what.c_str());
    }
}
template <class T> void Release(T*& p) {
    if (p) p->Release();
    p = nullptr;
}

// ---- the shader source and its constants, as patches/ambient_occlusion_patch.cpp has them ----
constexpr int kQualityCount = 5;
constexpr int kQualitySlices[kQualityCount] = {4, 6, 8, 12, 2};
const char* const kQualityNames[kQualityCount] = {"Low", "Medium", "High", "Ultra", "Very Low"};
constexpr int kVariants = 4; // bit 0 = zoom, bit 1 = blended normals
constexpr int kLevels = 9;
constexpr float kNear = 0.25f, kA = 1.00008f, kTanY = 1.0f / 4.293f;

std::string g_source;
float g_zoomMid = 0, g_zoomFar = 0, g_zoomHysteresis = 0, g_blockBend = 0;

std::string Slurp(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}
float ConstantOf(const std::string& cpp, const char* name) {
    const size_t at = cpp.find(name);
    return at == std::string::npos ? 0.0f : std::stof(cpp.substr(cpp.find('=', at) + 1));
}
bool LoadSource(const char* exe) {
    std::string path = exe;
    path = path.substr(0, path.find_last_of("\\/") + 1);
    std::string cpp = Slurp(path + "../../patches/ambient_occlusion_patch.cpp");
    if (cpp.empty()) cpp = Slurp("patches/ambient_occlusion_patch.cpp");
    for (size_t a = 0, b = 0; (a = cpp.find("R\"HLSL(", b)) != std::string::npos && (b = cpp.find(")HLSL\"", a)) != std::string::npos;)
        g_source += cpp.substr(a + 7, b - a - 7); // the source is several literals in a row
    if (g_source.empty()) return false;
    g_zoomMid = ConstantOf(cpp, "kZoomMid =");
    g_zoomFar = ConstantOf(cpp, "kZoomFar =");
    g_zoomHysteresis = ConstantOf(cpp, "kZoomHysteresis =");
    g_blockBend = ConstantOf(cpp, "kBlockBend =");
    return g_zoomMid > 0 && g_zoomFar > g_zoomMid && g_zoomHysteresis > 0 && g_blockBend > 0;
}

std::vector<DWORD> Compile(const char* entry, int q, int v, std::string* err = nullptr, const char* define = nullptr) {
    const int s = q >= 0 ? kQualitySlices[q] : 8; // q < 0: a pass without slices
    const std::string sl = std::to_string(s), sMid = std::to_string(std::max(2, s * 3 / 4)), sFar = std::to_string(std::max(2, s / 2));
    std::vector<D3D_SHADER_MACRO> m;
    if (q >= 0) m.push_back({"SLICES", sl.c_str()});
    if (v & 1) {
        m.push_back({"ZOOM", "1"});
        m.push_back({"SLICES_MID", sMid.c_str()});
        m.push_back({"SLICES_FAR", sFar.c_str()});
    }
    if (v & 2) m.push_back({"BLEND_NORMALS", "1"});
    if (define) m.push_back({define, "1"});
    m.push_back({nullptr, nullptr});
    ID3DBlob *code = nullptr, *log = nullptr;
    const HRESULT hr = D3DCompile(g_source.data(), g_source.size(), "ambient_occlusion.hlsl", m.data(), nullptr, entry, "ps_3_0",
                                  D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &log);
    std::vector<DWORD> out;
    if (SUCCEEDED(hr) && code) out.assign(static_cast<const DWORD*>(code->GetBufferPointer()),
                                          static_cast<const DWORD*>(code->GetBufferPointer()) + code->GetBufferSize() / 4);
    if (err && log) *err = static_cast<const char*>(log->GetBufferPointer());
    Release(code);
    Release(log);
    return out;
}
int Slots(const std::vector<DWORD>& code) {
    ID3DBlob* text = nullptr;
    if (FAILED(D3DDisassemble(code.data(), code.size() * 4, 0, nullptr, &text))) return -1;
    const std::string s(static_cast<const char*>(text->GetBufferPointer()), text->GetBufferSize());
    Release(text);
    const size_t at = s.find("approximately ");
    return at == std::string::npos ? -1 : std::stoi(s.substr(at + 14));
}

// ---- synthetic scene (world: y up; metres) ----
struct Box { float x0, y0, z0, x1, y1, z1; };
// counter (top with a crease along x = z), a wall 7 m behind it (out of the counter top's reach), houses around
const Box kCounter{-1.0f, 0.0f, -0.3f, 1.0f, 0.9f, 0.3f};
const Box kBoxes[] = {{-3.0f, 0.0f, 7.0f, 3.0f, 2.5f, 7.2f}, kCounter,
                      {-30, 0, -30, -22, 6, -22}, {22, 0, -30, 30, 5, -22}, {-30, 0, 22, -22, 7, 30}, {22, 0, 22, 30, 6, 30},
                      {-60, 0, 40, -48, 8, 52}, {48, 0, 40, 60, 6, 52}, {-8, 0, 60, 8, 9, 72}};
constexpr float kCrease = 0.0005f; // top rises 0.5 mm per metre away from the x = z line

bool HitBox(const Box& b, const float o[3], const float d[3], float& t) {
    float t0 = 0, t1 = 1e9f;
    const float lo[3] = {b.x0, b.y0, b.z0}, hi[3] = {b.x1, b.y1, b.z1};
    for (int k = 0; k < 3; k++) {
        if (std::fabs(d[k]) < 1e-12f) {
            if (o[k] < lo[k] || o[k] > hi[k]) return false;
            continue;
        }
        float a = (lo[k] - o[k]) / d[k], c = (hi[k] - o[k]) / d[k];
        if (a > c) std::swap(a, c);
        t0 = std::max(t0, a);
        t1 = std::min(t1, c);
    }
    if (t0 > t1) return false;
    t = t0;
    return true;
}
// surface classes for the metrics
enum Surface { Sky, OpenFloor, NearFloor, Contact, CounterTop, Other };

struct Frame {
    int W = 0, H = 0, padW = 0, padH = 0;
    float tanX = 0, tanY = kTanY;
    std::vector<float> w;          // 1/z, padded level 0 (0 = sky)
    std::vector<unsigned char> cls; // Surface per screen pixel
};

// camera looking at the counter from `dist` metres, 35 degrees down
Frame Render(int W, int H, float dist) {
    Frame f;
    f.W = W, f.H = H;
    f.padW = (W + 255) / 256 * 256, f.padH = (H + 255) / 256 * 256;
    f.tanX = kTanY * W / H;
    f.w.assign(size_t(f.padW) * f.padH, 0.0f);
    f.cls.assign(size_t(W) * H, Sky);
    const float pitch = 35.0f * 3.14159265f / 180.0f;
    const float fwd[3] = {0, -std::sin(pitch), std::cos(pitch)}, up[3] = {0, std::cos(pitch), std::sin(pitch)}, right[3] = {1, 0, 0};
    const float target[3] = {0.2f, 0.9f, 0.0f};
    const float eye[3] = {target[0] - fwd[0] * dist, target[1] - fwd[1] * dist, target[2] - fwd[2] * dist};
    const double scale = 16777215.0;
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) {
            const float vx = (2.0f * (x + 0.5f) / W - 1.0f) * f.tanX, vy = (1.0f - 2.0f * (y + 0.5f) / H) * f.tanY;
            const float d[3] = {right[0] * vx + up[0] * vy + fwd[0], right[1] * vx + up[1] * vy + fwd[1], right[2] * vx + up[2] * vy + fwd[2]};
            float t = 1e9f;
            int hit = -1;
            for (int i = 0; i < int(std::size(kBoxes)); i++) {
                float tb;
                if (HitBox(kBoxes[i], eye, d, tb) && tb < t) t = tb, hit = i;
            }
            const float tf = d[1] < 0 ? -eye[1] / d[1] : 1e9f; // floor y = 0
            bool floor = tf < t;
            if (floor) t = tf;
            if (t >= 3000.0f) continue; // sky
            float p[3] = {eye[0] + d[0] * t, eye[1] + d[1] * t, eye[2] + d[2] * t};
            Surface s = Other;
            if (hit == 1 && !floor && p[1] > kCounter.y1 - 1e-3f) {
                // the crease: the top rises away from the x = z line (exact for a ray hitting it from above)
                const float dy = kCrease * std::fabs(p[0] - p[2]);
                t += dy / d[1];
                p[0] = eye[0] + d[0] * t, p[1] = eye[1] + d[1] * t, p[2] = eye[2] + d[2] * t;
                const bool inner = p[0] > kCounter.x0 + 0.15f && p[0] < kCounter.x1 - 0.15f && p[2] > kCounter.z0 + 0.15f && p[2] < kCounter.z1 - 0.15f;
                s = inner ? CounterTop : Other;
            } else if (floor) {
                // distance on the floor to the nearest box footprint
                float dmin = 1e9f;
                for (const Box& b : kBoxes) {
                    const float dx = std::max({b.x0 - p[0], 0.0f, p[0] - b.x1}), dz = std::max({b.z0 - p[2], 0.0f, p[2] - b.z1});
                    dmin = std::min(dmin, std::sqrt(dx * dx + dz * dz));
                }
                s = dmin < 0.3f ? Contact : dmin > 6.0f ? OpenFloor : NearFloor;
            }
            const float z = t; // d has a view z of 1
            const double dev = std::round((kA - kNear * kA / z) * scale) / scale; // 24-bit device depth
            if (dev >= 0.99999) continue;
            f.w[size_t(y) * f.padW + x] = float(std::max(kA - dev, 1e-7) / (kNear * kA));
            f.cls[size_t(y) * W + x] = static_cast<unsigned char>(s);
        }
    return f;
}

// a wall facing the camera at `z` metres (the 1/z at the screen centre is exact at every pyramid level)
Frame Flat(int W, int H, float z) {
    Frame f;
    f.W = W, f.H = H;
    f.padW = (W + 255) / 256 * 256, f.padH = (H + 255) / 256 * 256;
    f.tanX = kTanY * W / H;
    f.w.assign(size_t(f.padW) * f.padH, 0.0f);
    f.cls.assign(size_t(W) * H, Other);
    for (int y = 0; y < H; y++) std::fill_n(&f.w[size_t(y) * f.padW], W, 1.0f / z);
    return f;
}

// DownPS on the CPU: the 2x2 average of 1/z without sky; across an edge (10%) the farthest
std::vector<std::vector<float>> Pyramid(const Frame& f) {
    std::vector<std::vector<float>> lv(kLevels);
    lv[0] = f.w;
    for (int i = 1; i < kLevels; i++) {
        const int w = f.padW >> i, h = f.padH >> i, pw = f.padW >> (i - 1);
        lv[i].assign(size_t(w) * h, 0.0f);
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                const float* r0 = &lv[i - 1][size_t(2 * y) * pw + 2 * x];
                const float q[4] = {r0[0], r0[1], r0[pw], r0[pw + 1]};
                float n = 0, sum = 0, wMax = 0;
                for (float v : q) n += v > 0, sum += v, wMax = std::max(wMax, v);
                float wMin = wMax;
                for (float v : q) wMin = std::min(wMin, v > 0 ? v : wMax);
                lv[i][size_t(y) * w + x] = n > 0 && wMax > wMin * 1.1f ? wMin : n > 0 ? sum / n : 0.0f;
            }
    }
    return lv;
}

// ---- hidden D3D9 device ----
struct QuadVertex { float x, y, z, rhw, u, v; };
struct Gpu {
    HWND window = nullptr;
    IDirect3D9* api = nullptr;
    IDirect3DDevice9* dev = nullptr;
    Gpu() {
        window = CreateWindowExW(0, L"STATIC", L"Apex AO options check", WS_OVERLAPPED, 0, 0, 64, 64, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        api = Direct3DCreate9(D3D_SDK_VERSION);
        D3DPRESENT_PARAMETERS p{};
        p.Windowed = TRUE;
        p.SwapEffect = D3DSWAPEFFECT_DISCARD;
        p.hDeviceWindow = window;
        p.BackBufferWidth = p.BackBufferHeight = 64;
        if (api) api->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, window, D3DCREATE_HARDWARE_VERTEXPROCESSING, &p, &dev);
    }
    ~Gpu() {
        Release(dev);
        Release(api);
        if (window) DestroyWindow(window);
    }
    IDirect3DPixelShader9* Ps(const std::vector<DWORD>& code) {
        IDirect3DPixelShader9* s = nullptr;
        if (!code.empty()) dev->CreatePixelShader(code.data(), &s);
        return s;
    }
    void States() {
        dev->SetVertexShader(nullptr);
        dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
        dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
        dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        dev->SetRenderState(D3DRS_LIGHTING, FALSE);
        for (DWORD s = 0; s < 8; s++) { // as SetPassStates
            const bool march = s == 2;
            dev->SetSamplerState(s, D3DSAMP_MINFILTER, march ? D3DTEXF_LINEAR : D3DTEXF_POINT);
            dev->SetSamplerState(s, D3DSAMP_MAGFILTER, march ? D3DTEXF_LINEAR : D3DTEXF_POINT);
            dev->SetSamplerState(s, D3DSAMP_MIPFILTER, march ? D3DTEXF_POINT : D3DTEXF_NONE);
            dev->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
            dev->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
        }
    }
    void Quad(UINT w, UINT h) {
        const float x1 = float(w) - 0.5f, y1 = float(h) - 0.5f;
        const QuadVertex v[4] = {{-0.5f, -0.5f, 0, 1, 0, 0}, {x1, -0.5f, 0, 1, 1, 0}, {-0.5f, y1, 0, 1, 0, 1}, {x1, y1, 0, 1, 1, 1}};
        dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, v, sizeof(QuadVertex));
    }
};

float Half(unsigned short h) {
    const unsigned s = (h >> 15) & 1, e = (h >> 10) & 31, m = h & 1023;
    const float v = e == 0 ? std::ldexp(float(m), -24) : std::ldexp(float(m | 1024), int(e) - 25);
    return s ? -v : v;
}

// The options of one frame, as RunAo reads them from the settings
struct Opt {
    bool zoom = true;        // Fewer directions when far (a tiered GtaoPS variant without it stays at tier 0)
    bool hysteresis = false; // Steady direction count
    bool edges = false;      // Edge-aware smoothing (the scene's targets need 4 channels)
    bool block = false;      // Fewer directions in the open
};
struct Passes {
    IDirect3DPixelShader9 *blur = nullptr, *blurEdges = nullptr, *zoomTier = nullptr, *blockTier = nullptr, *blockSpread = nullptr;
    bool Ok() const { return blur && blurEdges && zoomTier && blockTier && blockSpread; }
};

// One frame of the scene on the GPU: the pyramid, GtaoPS, the blur passes; the AO (R) read back. What RunAo keeps from
// frame to frame (the direction-count tier) lives here too: Load swaps the depth under it.
struct Scene {
    Gpu& gpu;
    Frame f;
    Passes ps;
    bool wide; // AO targets with 4 channels (Edge-aware smoothing)
    IDirect3DTexture9 *z = nullptr, *a = nullptr, *b = nullptr, *tier[2] = {}, *block[2] = {};
    IDirect3DSurface9 *as = nullptr, *bs = nullptr, *read = nullptr, *tierSurf[2] = {}, *tierRead = nullptr, *blockSurf[2] = {}, *blockRead = nullptr;
    int tierNow = 0, blockW = 0, blockH = 0;
    // another frame of the same size
    void Load(const Frame& frame) {
        f = frame;
        const auto lv = Pyramid(f);
        for (int i = 0; z && i < kLevels; i++) {
            D3DLOCKED_RECT r{};
            if (FAILED(z->LockRect(i, &r, nullptr, 0))) continue;
            const int w = f.padW >> i, h = f.padH >> i;
            for (int y = 0; y < h; y++) std::memcpy(static_cast<char*>(r.pBits) + size_t(y) * r.Pitch, &lv[i][size_t(y) * w], size_t(w) * 4);
            z->UnlockRect(i);
        }
    }
    Scene(Gpu& g, const Frame& frame, const Passes& passes, bool wideTargets = false) : gpu(g), ps(passes), wide(wideTargets) {
        auto* dev = gpu.dev;
        dev->CreateTexture(frame.padW, frame.padH, kLevels, 0, D3DFMT_R32F, D3DPOOL_MANAGED, &z, nullptr);
        Load(frame);
        for (int i = 0; i < 2; i++) {
            dev->CreateTexture(1, 1, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F, D3DPOOL_DEFAULT, &tier[i], nullptr);
            if (tier[i]) tier[i]->GetSurfaceLevel(0, &tierSurf[i]);
            if (tierSurf[i]) dev->ColorFill(tierSurf[i], nullptr, 0);
        }
        blockW = (f.W + 3) / 4, blockH = (f.H + 3) / 4;
        for (int i = 0; i < 2; i++) {
            dev->CreateTexture(blockW, blockH, 1, D3DUSAGE_RENDERTARGET, D3DFMT_R32F, D3DPOOL_DEFAULT, &block[i], nullptr);
            if (block[i]) block[i]->GetSurfaceLevel(0, &blockSurf[i]);
        }
        dev->CreateOffscreenPlainSurface(blockW, blockH, D3DFMT_R32F, D3DPOOL_SYSTEMMEM, &blockRead, nullptr);
        dev->CreateOffscreenPlainSurface(1, 1, D3DFMT_R32F, D3DPOOL_SYSTEMMEM, &tierRead, nullptr);
        const D3DFORMAT fmt = wide ? D3DFMT_A16B16G16R16F : D3DFMT_G16R16F;
        dev->CreateTexture(f.W, f.H, 1, D3DUSAGE_RENDERTARGET, fmt, D3DPOOL_DEFAULT, &a, nullptr);
        dev->CreateTexture(f.W, f.H, 1, D3DUSAGE_RENDERTARGET, fmt, D3DPOOL_DEFAULT, &b, nullptr);
        if (a) a->GetSurfaceLevel(0, &as);
        if (b) b->GetSurfaceLevel(0, &bs);
        dev->CreateOffscreenPlainSurface(f.W, f.H, fmt, D3DPOOL_SYSTEMMEM, &read, nullptr);
    }
    ~Scene() {
        Release(read), Release(as), Release(bs), Release(a), Release(b), Release(z);
        for (int i = 0; i < 2; i++) Release(tierSurf[i]), Release(tier[i]), Release(blockSurf[i]), Release(block[i]);
        Release(tierRead), Release(blockRead);
    }
    bool Ok() const { return z && as && bs && read && tierSurf[0] && tierSurf[1] && tierRead && blockSurf[0] && blockSurf[1] && blockRead && ps.Ok(); }
    // the tier of every 4x4 block (blockW x blockH), as the last AO pass with `block` read it
    std::vector<float> Blocks() {
        std::vector<float> out;
        D3DLOCKED_RECT r{};
        if (FAILED(gpu.dev->GetRenderTargetData(blockSurf[1], blockRead)) || FAILED(blockRead->LockRect(&r, nullptr, D3DLOCK_READONLY))) return out;
        out.resize(size_t(blockW) * blockH);
        for (int y = 0; y < blockH; y++) std::memcpy(&out[size_t(y) * blockW], static_cast<const char*>(r.pBits) + size_t(y) * r.Pitch, size_t(blockW) * 4);
        blockRead->UnlockRect();
        return out;
    }
    // the frame's direction-count tier (0, 1, 2), as the last AO pass read it
    float Tier() {
        D3DLOCKED_RECT r{};
        if (FAILED(gpu.dev->GetRenderTargetData(tierSurf[tierNow], tierRead)) || FAILED(tierRead->LockRect(&r, nullptr, D3DLOCK_READONLY))) return -1;
        const float t = *static_cast<const float*>(r.pBits);
        tierRead->UnlockRect();
        return t;
    }
    // the constants of RunAo (default settings: Reach 165.8%, Strength 158.2%, Distance 752 m, High's rotation is per variant)
    void Constants(int slices, const Opt& o) {
        const float W = float(f.W), H = float(f.H), tanX = f.tanX, tanY = f.tanY;
        const float reach = 1.658f, s = 1.5822f, distance = 752.35f, fadeStart = distance * (150.0f / 400.0f);
        const float pi = 3.14159265f;
        const float c[15][4] = {
            {tanX, tanY, H / (2.0f * tanY), 0.30f * H},
            {W, H, float(f.padW), float(f.padH)},
            {2.0f * H / 2160.0f, 2.0f, 1.3f * 1.3f, 1.0f / (distance - fadeStart)},
            {0.6f * reach, 2.0f * reach, 2.5f * reach, distance},
            {1.2f * s, 0.5f * s, 0.8f * s, 0.03f},
            {20.0f, 1.0f / 20.0f, 1.0f, 0.01f},
            {0, 0, 0, 0},
            {0.05f, 1.0f / 0.95f, 0.4647f, 0},
            {2.0f * tanX / W, -2.0f * tanY / H, -tanX, tanY},
            {std::cos(pi / slices), std::sin(pi / slices), 0, 0},
            {kA, 1.0f / (kNear * kA), 0, 0},
            {0, 0, 0, 0},
            {0, 0, 0, 0},
            {W * 0.5f / f.padW, H * 0.5f / f.padH, o.zoom ? 1.0f / g_zoomMid : 0.0f, o.zoom ? 1.0f / g_zoomFar : 0.0f},
            {o.block ? 0.25f / blockW : 0.0f, o.block ? 0.25f / blockH : 0.0f, o.hysteresis ? 1.0f / (1.0f + g_zoomHysteresis) : 1.0f, o.hysteresis ? 1.0f / (1.0f - g_zoomHysteresis) : 1.0f}};
        gpu.dev->SetPixelShaderConstantF(0, &c[0][0], 15);
    }
    // the tier pass and the AO pass, as RunAo's step 3 (`tiered`: a GtaoPS variant that reads the tier)
    void Ao(IDirect3DPixelShader9* gtao, int slices, bool tiered, const Opt& o = {}) {
        auto* dev = gpu.dev;
        gpu.States();
        Constants(slices, o);
        dev->SetTexture(2, z);
        if (tiered) {
            tierNow ^= 1;
            dev->SetRenderTarget(0, tierSurf[tierNow]);
            dev->SetTexture(7, tier[tierNow ^ 1]);
            dev->SetPixelShader(ps.zoomTier);
            gpu.Quad(1, 1);
            dev->SetTexture(7, tier[tierNow]);
            if (o.block) {
                const float find[4] = {4.0f * blockW, 4.0f * blockH, g_blockBend, 0}, spread[4] = {1.0f / blockW, 1.0f / blockH, 0, 0};
                dev->SetPixelShaderConstantF(6, find, 1);
                dev->SetRenderTarget(0, blockSurf[0]);
                dev->SetPixelShader(ps.blockTier);
                gpu.Quad(blockW, blockH);
                dev->SetPixelShaderConstantF(6, spread, 1);
                dev->SetRenderTarget(0, blockSurf[1]);
                dev->SetTexture(3, block[0]);
                dev->SetPixelShader(ps.blockSpread);
                gpu.Quad(blockW, blockH);
                dev->SetTexture(3, nullptr);
                dev->SetTexture(7, block[1]);
            }
        }
        dev->SetRenderTarget(0, as);
        dev->SetTexture(1, z);
        dev->SetPixelShader(gtao);
        gpu.Quad(f.W, f.H);
        dev->SetTexture(7, nullptr);
    }
    // every pass up to the blurred AO in `a`, as RunAo's steps 3 and 4
    void Chain(IDirect3DPixelShader9* gtao, int slices, bool tiered, const Opt& o = {}) {
        Ao(gtao, slices, tiered, o);
        Blur(o);
    }
    void Blur(const Opt& o) {
        auto* dev = gpu.dev;
        dev->SetPixelShader(o.edges ? ps.blurEdges : ps.blur);
        for (int pass = 0; pass < (o.edges ? 2 : 4); pass++) {
            const float dir[4] = {pass & 1 ? 0.0f : 1.0f / f.W, pass & 1 ? 1.0f / f.H : 0.0f, o.edges ? float(~pass & 1) : pass < 2 ? 0.0f : 1.0f, float(pass & 1)};
            dev->SetPixelShaderConstantF(6, dir, 1);
            dev->SetRenderTarget(0, pass & 1 ? as : bs);
            dev->SetTexture(3, pass & 1 ? b : a);
            gpu.Quad(f.W, f.H);
        }
        dev->SetTexture(3, nullptr);
    }
    std::vector<float> Run(IDirect3DPixelShader9* gtao, int slices, bool tiered, const Opt& o = {}) {
        auto* dev = gpu.dev;
        std::vector<float> out;
        if (FAILED(dev->BeginScene())) return out;
        Chain(gtao, slices, tiered, o);
        dev->EndScene();
        IDirect3DSurface9* back = nullptr;
        dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &back);
        dev->SetRenderTarget(0, back);
        Release(back);
        D3DLOCKED_RECT r{};
        if (FAILED(dev->GetRenderTargetData(as, read)) || FAILED(read->LockRect(&r, nullptr, D3DLOCK_READONLY))) return out;
        out.resize(size_t(f.W) * f.H);
        for (int y = 0; y < f.H; y++) {
            const auto* row = reinterpret_cast<const unsigned short*>(static_cast<const char*>(r.pBits) + size_t(y) * r.Pitch);
            for (int x = 0; x < f.W; x++) out[size_t(y) * f.W + x] = Half(row[(wide ? 4 : 2) * x]);
        }
        read->UnlockRect();
        return out;
    }
    // GPU time of `draw` (ms, median of `n` frames)
    template <class Draw> double Time(Draw draw, int n) {
        auto* dev = gpu.dev;
        IDirect3DQuery9 *dis = nullptr, *t0 = nullptr, *t1 = nullptr, *fq = nullptr;
        dev->CreateQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT, &dis);
        dev->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &t0);
        dev->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &t1);
        dev->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ, &fq);
        std::vector<double> ms;
        for (int i = 0; dis && t0 && t1 && fq && i < n + 2; i++) {
            dev->BeginScene();
            dis->Issue(D3DISSUE_BEGIN);
            t0->Issue(D3DISSUE_END);
            draw();
            t1->Issue(D3DISSUE_END);
            fq->Issue(D3DISSUE_END);
            dis->Issue(D3DISSUE_END);
            dev->EndScene();
            BOOL disjoint = TRUE;
            UINT64 a0 = 0, a1 = 0, freq = 0;
            while (dis->GetData(&disjoint, sizeof(disjoint), D3DGETDATA_FLUSH) == S_FALSE) {}
            while (t0->GetData(&a0, sizeof(a0), D3DGETDATA_FLUSH) == S_FALSE) {}
            while (t1->GetData(&a1, sizeof(a1), D3DGETDATA_FLUSH) == S_FALSE) {}
            while (fq->GetData(&freq, sizeof(freq), D3DGETDATA_FLUSH) == S_FALSE) {}
            if (i >= 2 && !disjoint && freq) ms.push_back(double(a1 - a0) * 1000.0 / double(freq));
        }
        Release(dis), Release(t0), Release(t1), Release(fq);
        if (ms.empty()) return -1;
        std::sort(ms.begin(), ms.end());
        return ms[ms.size() / 2];
    }
};

double Median(std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v.empty() ? -1 : v[v.size() / 2];
}

struct Stats { double mean = 0, max = 0; long n = 0; };
Stats Diff(const std::vector<float>& a, const std::vector<float>& b, const Frame& f) {
    Stats s;
    for (size_t i = 0; i < a.size(); i++) {
        if (f.cls[i] == Sky) continue;
        const double d = std::fabs(double(a[i]) - b[i]) * 255.0;
        s.mean += d, s.max = std::max(s.max, d), s.n++;
    }
    if (s.n) s.mean /= s.n;
    return s;
}
// mean shade (levels of 255) on one surface class, and the share of its pixels shaded by more than one level
void Shade(const std::vector<float>& ao, const Frame& f, Surface c, double& mean, double& over1, long& n) {
    mean = over1 = 0, n = 0;
    for (size_t i = 0; i < ao.size(); i++)
        if (f.cls[i] == c) {
            const double s = (1.0 - ao[i]) * 255.0;
            mean += s, over1 += s > 1.0, n++;
        }
    if (n) mean /= n, over1 /= n;
}
double MeanShade(const std::vector<float>& ao, const Frame& f, Surface c) {
    double mean, over1;
    long n;
    Shade(ao, f, c, mean, over1, n);
    return mean;
}
// mean |difference| between horizontal neighbours on the open floor (levels): blotches or a left-over interleave raise it
double Roughness(const std::vector<float>& ao, const Frame& f) {
    double sum = 0;
    long n = 0;
    for (int y = 0; y < f.H; y++)
        for (int x = 0; x + 1 < f.W; x++) {
            const size_t i = size_t(y) * f.W + x;
            if (f.cls[i] == OpenFloor && f.cls[i + 1] == OpenFloor) sum += std::fabs(double(ao[i]) - ao[i + 1]) * 255.0, n++;
        }
    return n ? sum / n : 0;
}
// Mean |difference| of the shade between a floor pixel and a neighbour on another surface nearer to the camera by more
// than 0.5% (levels): the outlines of the counter and the boxes against the floor behind them. A blur that carries shade
// across a silhouette lowers it.
double SilhouetteStep(const std::vector<float>& ao, const Frame& f) {
    double sum = 0;
    long n = 0;
    for (int y = 0; y + 1 < f.H; y++)
        for (int x = 0; x + 1 < f.W; x++) {
            const size_t i = size_t(y) * f.W + x;
            for (size_t j : {i + 1, i + size_t(f.W)}) {
                const bool fi = f.cls[i] == OpenFloor || f.cls[i] == NearFloor, fj = f.cls[j] == OpenFloor || f.cls[j] == NearFloor;
                if (fi == fj || f.cls[fi ? j : i] != Other) continue;
                const float wFloor = f.w[(fi ? i : j) / f.W * f.padW + (fi ? i : j) % f.W], wOther = f.w[(fi ? j : i) / f.W * f.padW + (fi ? j : i) % f.W];
                if (wOther > wFloor * 1.005f) sum += std::fabs(double(ao[i]) - ao[j]) * 255.0, n++;
            }
        }
    return n ? sum / n : 0;
}
// Block edges where the direction count changes: the mean |difference| of the shade between the two pixels either side
// (levels), in `ao` and in the default image `ref`. Blotches at block edges raise the first over the second.
void BlockEdges(const std::vector<float>& ao, const std::vector<float>& ref, const std::vector<float>& tiers, const Frame& f, int bw, int bh,
                double& step, double& refStep, long& n) {
    step = refStep = 0, n = 0;
    for (int by = 0; by < bh; by++)
        for (int bx = 0; bx < bw; bx++)
            for (int down = 0; down < 2; down++) {
                const int nx = bx + !down, ny = by + down;
                if (nx >= bw || ny >= bh || tiers[size_t(by) * bw + bx] == tiers[size_t(ny) * bw + nx]) continue;
                for (int k = 0; k < 4; k++) {
                    const int x0 = down ? 4 * bx + k : 4 * bx + 3, y0 = down ? 4 * by + 3 : 4 * by + k, x1 = x0 + !down, y1 = y0 + down;
                    if (x1 >= f.W || y1 >= f.H) continue;
                    const size_t i = size_t(y0) * f.W + x0, j = size_t(y1) * f.W + x1;
                    step += std::fabs(double(ao[i]) - ao[j]) * 255.0, refStep += std::fabs(double(ref[i]) - ref[j]) * 255.0, n++;
                }
            }
    if (n) step /= n, refStep /= n;
}

} // namespace

int main(int argc, char** argv) {
    std::setvbuf(stdout, nullptr, _IONBF, 0); // a run cut short still shows what it printed
    if (!LoadSource(argv[0])) {
        std::printf("FAIL: patches/ambient_occlusion_patch.cpp not found or its HLSL / kZoomMid / kZoomFar not read\n");
        return 1;
    }
    std::printf("Slices by zoom: 3/4 from %.0f m, 1/2 from %.0f m\n", g_zoomMid, g_zoomFar);
    Gpu gpu;
    Check(gpu.dev != nullptr, "native D3D9 HAL device");

    // 1. Every variant compiles within ps_3_0's 512 instruction slots and native D3D9 accepts it; the default pass is the
    //    shipped one (no option macro changes it)
    std::printf("GtaoPS instruction slots (default / tiered / smooth / both):\n");
    std::vector<DWORD> code[kVariants][kQualityCount];
    for (int q = 0; q < kQualityCount; q++) {
        std::printf("  %-8s (%2d slices):", kQualityNames[q], kQualitySlices[q]);
        for (int v = 0; v < kVariants; v++) {
            std::string err;
            code[v][q] = Compile("GtaoPS", q, v, &err);
            const int slots = code[v][q].empty() ? -1 : Slots(code[v][q]);
            std::printf(" %4d", slots);
            Check(!code[v][q].empty(), std::string("compiles: ") + kQualityNames[q] + " variant " + std::to_string(v) + " " + err);
            Check(slots > 0 && slots <= 512, std::string("<= 512 slots: ") + kQualityNames[q] + " variant " + std::to_string(v));
            if (gpu.dev) {
                IDirect3DPixelShader9* ps = gpu.Ps(code[v][q]);
                Check(ps != nullptr, std::string("native D3D9 accepts ") + kQualityNames[q] + " variant " + std::to_string(v));
                Release(ps);
            }
        }
        std::printf("\n");
    }
    // the other passes of the options
    struct Other { const char* entry; const char* define; };
    const Other kOther[] = {{"BlurPS", nullptr}, {"BlurPS", "EDGES"}, {"ZoomTierPS", nullptr}, {"BlockTierPS", nullptr}, {"BlockSpreadPS", nullptr}};
    std::vector<DWORD> otherCode[std::size(kOther)];
    std::printf("Other passes, instruction slots:");
    for (size_t i = 0; i < std::size(kOther); i++) {
        std::string err;
        otherCode[i] = Compile(kOther[i].entry, -1, 0, &err, kOther[i].define);
        const int slots = otherCode[i].empty() ? -1 : Slots(otherCode[i]);
        std::printf(" %s%s%s %d", kOther[i].entry, kOther[i].define ? " " : "", kOther[i].define ? kOther[i].define : "", slots);
        Check(slots > 0 && slots <= 512, std::string("compiles within 512 slots: ") + kOther[i].entry + " " + err);
    }
    std::printf("\n");
    if (!gpu.dev) {
        std::printf("%d checks, %d failed\n", g_checks, g_failures);
        return g_failures ? 1 : 0;
    }

    // 2. Images: High, 1920 x 1080, views at 40, 80 and 150 m (the counter at the screen centre)
    const int q = 2, slices = kQualitySlices[q];
    Passes passes;
    passes.blur = gpu.Ps(otherCode[0]), passes.blurEdges = gpu.Ps(otherCode[1]), passes.zoomTier = gpu.Ps(otherCode[2]);
    passes.blockTier = gpu.Ps(otherCode[3]), passes.blockSpread = gpu.Ps(otherCode[4]);
    IDirect3DPixelShader9* ps[kVariants] = {};
    for (int v = 0; v < kVariants; v++) ps[v] = gpu.Ps(code[v][q]);
    Check(passes.Ok() && ps[0] && ps[1] && ps[2] && ps[3], "passes created");
    Opt edgeOpt;
    edgeOpt.zoom = false, edgeOpt.edges = true;
    Opt blockOpt;
    blockOpt.zoom = false, blockOpt.block = true;
    const float views[] = {40.0f, 80.0f, 150.0f};
    std::printf("High, 1920x1080, blurred AO in levels of 255:\n");
    for (float dist : views) {
        const Frame f = Render(1920, 1080, dist);
        Scene sc(gpu, f, passes), wide(gpu, f, passes, true);
        Check(sc.Ok() && wide.Ok(), "scene resources");
        if (!sc.Ok() || !wide.Ok()) continue;
        const std::string at = " at " + std::to_string(int(dist)) + " m";
        std::vector<float> img[kVariants];
        for (int v = 0; v < kVariants; v++) img[v] = sc.Run(ps[v], slices, v & 1);
        const std::vector<float> again = sc.Run(ps[1], slices, true);
        Check(again == img[1], "zoom variant deterministic");
        const Stats z = Diff(img[0], img[1], f), n = Diff(img[0], img[2], f);
        const double r0 = Roughness(img[0], f), r1 = Roughness(img[1], f);
        std::printf("  %3.0f m: zoom vs default mean %.3f max %.2f; floor roughness %.3f -> %.3f\n", dist, z.mean, z.max, r0, r1);
        const char* kNames[] = {"", "open floor", "near floor", "contact", "counter top"};
        for (Surface c : {OpenFloor, Contact, CounterTop}) {
            double m0, o0, m2, o2;
            long n0, n2;
            Shade(img[0], f, c, m0, o0, n0);
            Shade(img[2], f, c, m2, o2, n2);
            if (n0 < 50) continue;
            std::printf("         %-11s (%7ld px): shade mean %.3f -> %.3f, pixels > 1 level %.2f%% -> %.2f%% (smooth flat surfaces)\n",
                        kNames[c], n0, m0, m2, 100 * o0, 100 * o2);
            if (c == Contact) Check(m2 > 0.75 * m0, "Smooth flat surfaces keeps the contact shade" + at);
            else Check(m2 <= m0 + 0.05, "Smooth flat surfaces adds no stray shade on flat surfaces" + at);
        }
        std::printf("         smooth vs default mean %.3f max %.2f\n", n.mean, n.max);
        if (dist < g_zoomMid) Check(z.max <= 1.0, "zoom equals the default pass up close (max <= 1 level)");
        else {
            Check(z.mean <= 1.0, "zoom differs from the default by at most 1 level on average" + at);
            Check(r1 <= 1.1 * r0 + 0.02, "zoom keeps the open floor as smooth as the default (interleave cancelled)" + at);
        }
        // Edge-aware smoothing: the looped pass at tier 0, 4-channel targets, two box passes
        const std::vector<float> edge = wide.Run(ps[1], slices, true, edgeOpt);
        const Stats e = Diff(img[0], edge, f);
        const double re = Roughness(edge, f), c0 = MeanShade(img[0], f, Contact), ce = MeanShade(edge, f, Contact);
        const double b0 = SilhouetteStep(img[0], f), be = SilhouetteStep(edge, f);
        std::printf("         edge-aware smoothing vs default: mean %.3f max %.2f; floor roughness %.3f -> %.3f; contact shade kept %.1f%%; shade step at silhouettes %.2f -> %.2f\n",
                    e.mean, e.max, r0, re, c0 > 0 ? 100 * ce / c0 : 0.0, b0, be);
        Check(ce > 0.9 * c0, "Edge-aware smoothing keeps the contact shade" + at);
        Check(be >= b0 - 0.05, "Edge-aware smoothing carries no more shade across silhouettes" + at);
        Check(re <= r0 + 0.25, "Edge-aware smoothing leaves no interleave pattern on the open floor (within 0.25 of a level)" + at);
        // Fewer directions in the open: a tier per 4x4 block
        const std::vector<float> blk = sc.Run(ps[1], slices, true, blockOpt), tiers = sc.Blocks();
        Check(!tiers.empty(), "block tiers read back");
        if (!tiers.empty()) {
            const Stats d = Diff(img[0], blk, f);
            const double half = double(std::count(tiers.begin(), tiers.end(), 2.0f)) / tiers.size(), rb = Roughness(blk, f), cb = MeanShade(blk, f, Contact);
            double step, refStep;
            long pairs;
            BlockEdges(blk, img[0], tiers, f, sc.blockW, sc.blockH, step, refStep, pairs);
            std::printf("         fewer directions in the open vs default: mean %.3f max %.2f; %.1f%% of the blocks at half the count; floor roughness %.3f -> %.3f; contact shade kept %.1f%%; shade step at %ld count-change block edges %.3f (default image %.3f)\n",
                        d.mean, d.max, 100 * half, r0, rb, c0 > 0 ? 100 * cb / c0 : 0.0, pairs, step, refStep);
            Check(half > 0.2, "open surfaces: at least a fifth of the blocks at half the count" + at);
            Check(d.mean <= 1.0, "open surfaces: at most 1 level from the default on average" + at);
            Check(cb > 0.95 * c0 && cb < 1.05 * c0, "open surfaces: contact shade within 5% of the default" + at);
            Check(rb <= 1.1 * r0 + 0.02, "open surfaces: the open floor as smooth as the default" + at);
            Check(step <= 1.1 * refStep + 0.05, "open surfaces: no blotches at block edges where the count changes" + at);
        }
    }

    // 3. Steady direction count: walls 2% either side of each threshold in turn, then well past the band and back
    {
        Scene sc(gpu, Flat(256, 256, 10.0f), passes);
        Check(sc.Ok(), "tier scene resources");
        for (int hysteresis = 0; sc.Ok() && hysteresis < 2; hysteresis++) {
            Opt o;
            o.hysteresis = hysteresis != 0;
            auto tierAt = [&](float z) {
                sc.Load(Flat(256, 256, z));
                sc.Run(ps[1], slices, true, o);
                return int(sc.Tier());
            };
            int switches = 0;
            for (float th : {g_zoomMid, g_zoomFar}) {
                int last = tierAt(th * 0.98f);
                for (int i = 1; i < 8; i++) {
                    const int t = tierAt(th * (i & 1 ? 1.02f : 0.98f));
                    switches += t != last, last = t;
                }
            }
            const int far2 = tierAt(g_zoomFar * 1.3f), mid1 = tierAt(g_zoomFar * 0.75f), near0 = tierAt(g_zoomMid * 0.7f);
            std::printf("Direction count switches over 2 x 8 frames 2%% either side of a threshold, %s: %d\n",
                        hysteresis ? "steady direction count" : "as before", switches);
            if (hysteresis) Check(switches == 0, "steady direction count: no switch within the band");
            else Check(switches == 14, "without it the count switches every frame");
            Check(far2 == 2 && mid1 == 1 && near0 == 0, "the count still follows the view past the band");
        }
    }

    if (argc > 1 && !std::strcmp(argv[1], "--no-timing")) {
        std::printf("%d checks, %d failed (GPU time skipped)\n", g_checks, g_failures);
        return g_failures ? 1 : 0;
    }
    // 4. GPU time at 3840 x 2160: 12 rounds of the arms in turn (clock drift hits all alike), each round the median of
    //    5 frames; the median round
    std::printf("AO pass GPU time, High, 3840x2160 (ms):\n");
    for (float dist : views) {
        const Frame f = Render(3840, 2160, dist);
        Scene sc(gpu, f, passes);
        if (!sc.Ok()) continue;
        std::vector<double> t[kVariants + 1];
        for (int r = 0; r < 12; r++) {
            for (int v = 0; v < kVariants; v++) t[v].push_back(sc.Time([&] { sc.Ao(ps[v], slices, v & 1); }, 5));
            t[kVariants].push_back(sc.Time([&] { sc.Ao(ps[1], slices, true, blockOpt); }, 5));
        }
        std::printf("  %3.0f m: default %.3f, zoom %.3f, smooth %.3f, both %.3f, open surfaces %.3f\n", dist, Median(t[0]), Median(t[1]), Median(t[2]), Median(t[3]),
                    Median(t[kVariants]));
    }
    // the AO pass and its blur together (what an option that changes the blur or adds a pass costs or saves)
    std::printf("AO and blur passes GPU time, High, 3840x2160, 40 m (ms):\n");
    {
        const Frame f = Render(3840, 2160, 40.0f);
        Scene sc(gpu, f, passes), wide(gpu, f, passes, true);
        if (sc.Ok() && wide.Ok()) {
            Opt looped;
            looped.zoom = false;
            std::vector<double> t[5];
            for (int r = 0; r < 12; r++) {
                t[3].push_back(sc.Time([&] { sc.Blur({}); }, 5));
                t[4].push_back(wide.Time([&] { wide.Blur(edgeOpt); }, 5));
                t[0].push_back(sc.Time([&] { sc.Chain(ps[0], slices, false); }, 5));
                t[1].push_back(sc.Time([&] { sc.Chain(ps[1], slices, true, looped); }, 5));
                t[2].push_back(wide.Time([&] { wide.Chain(ps[1], slices, true, edgeOpt); }, 5));
            }
            std::printf("  default %.3f, looped pass alone %.3f, edge-aware smoothing %.3f\n", Median(t[0]), Median(t[1]), Median(t[2]));
            std::printf("  blur passes alone: default (box and tent) %.3f, edge-aware (box) %.3f\n", Median(t[3]), Median(t[4]));
        }
    }
    for (auto*& p : ps) Release(p);
    Release(passes.blur), Release(passes.blurEdges), Release(passes.zoomTier), Release(passes.blockTier), Release(passes.blockSpread);
    std::printf("%d checks, %d failed\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
