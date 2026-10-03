// GPU light probe (see light_probe.h)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "light_probe.h"
#include "captures.h"
#include "ui/i18n.h"
#include "hotkeys.h"
#include "lot_light_bridge.h"
#include "level_light_share.h"
#include "recorder.h"
#include "d3d9_hooks.h"
#include "apex_log.h"
#include "apex_paths.h"
#include "imgui.h"
#include <windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>
#include <d3dcommon.h>

namespace {

constexpr const char* kHookName = "LightProbe";
std::atomic<bool> g_aiming{false};
// Valid bit plus signed Win32 client coordinates. Only the render thread starts a GPU capture.
std::atomic<uint64_t> g_aimClick{0};
constexpr size_t kMaxDraws = 4000;
constexpr int kPsConsts = 224; // ps_3_0
constexpr int kVsConsts = 256; // vs_3_0 (skinned objects keep the world matrix and vertex lights at c184..c199)
using ConstList = std::vector<std::pair<int, std::array<float, 4>>>; // non-zero registers only

enum class State { Idle, Armed, Capturing };

struct DrawRec {
    int index = 0;
    const char* kind = "";
    D3DPRIMITIVETYPE type{};
    UINT prims = 0;
    IDirect3DQuery9* query = nullptr;
    IDirect3DBaseTexture9* tex[16] = {};
    DWORD filt[16][3] = {}; // MIN, MAG, MIP filter per sampler
    IDirect3DVertexShader9* vs = nullptr;
    IDirect3DPixelShader9* ps = nullptr;
    DWORD rs[14] = {};
    ConstList psc, vsc;
    std::string bridge; // LotLightBridge::DescribeDraw
    std::string instances; // instanced draws: the per-instance data (read while the draw is recorded)
    DWORD samples = 0;
    bool resolved = false;
};

// Instanced draw (stream 0 frequency = INDEXEDDATA | count): the vertex declaration's per-instance elements and the
// instance stream contents (first 64 instances, up to 16 floats each), read with a read-only lock. Pieces of one
// modular object drawn in one call differ only here (e.g. the stairs' per-piece value that brightens a piece).
std::string ReadInstances(IDirect3DDevice9* dev) {
    UINT f0 = 0;
    if (FAILED(dev->GetStreamSourceFreq(0, &f0)) || !(f0 & D3DSTREAMSOURCE_INDEXEDDATA)) return {};
    const UINT count = f0 & 0x3FFFFFFF;
    std::string s = std::format("{} instancias", count);
    IDirect3DVertexDeclaration9* decl = nullptr;
    if (SUCCEEDED(dev->GetVertexDeclaration(&decl)) && decl) {
        D3DVERTEXELEMENT9 el[MAXD3DDECLLENGTH + 1] = {};
        UINT n = MAXD3DDECLLENGTH + 1;
        if (SUCCEEDED(decl->GetDeclaration(el, &n)))
            for (UINT i = 0; i < n && el[i].Stream != 0xFF; i++)
                if (el[i].Stream != 0)
                    s += std::format(" | stream {} +{} tipo {} uso {}{}", el[i].Stream, el[i].Offset, el[i].Type, el[i].Usage, el[i].UsageIndex);
        decl->Release();
    }
    for (UINT stream = 1; stream < 8; stream++) {
        UINT f = 0;
        if (FAILED(dev->GetStreamSourceFreq(stream, &f)) || !(f & D3DSTREAMSOURCE_INSTANCEDATA)) continue;
        IDirect3DVertexBuffer9* vb = nullptr;
        UINT off = 0, stride = 0;
        if (FAILED(dev->GetStreamSource(stream, &vb, &off, &stride)) || !vb) continue;
        D3DVERTEXBUFFER_DESC d{};
        vb->GetDesc(&d);
        const UINT want = std::min<UINT>(count, 64) * stride;
        const UINT bytes = std::min<UINT>(want, d.Size > off ? d.Size - off : 0);
        void* p = nullptr;
        if (stride >= 4 && bytes >= stride && SUCCEEDED(vb->Lock(off, bytes, &p, D3DLOCK_READONLY)) && p) {
            s += std::format("\n      stream {} (stride {}, pool {}, usage {:#x}):", stream, stride, static_cast<int>(d.Pool), d.Usage);
            const float* fl = static_cast<const float*>(p);
            const UINT per = std::min<UINT>(stride / 4, 16);
            for (UINT i = 0; (i + 1) * stride <= bytes; i++) {
                s += std::format("\n        #{}:", i);
                for (UINT k = 0; k < per; k++) s += std::format(" {:.5g}", fl[i * (stride / 4) + k]);
            }
            vb->Unlock();
        } else
            s += std::format("\n      stream {} (stride {}): not readable", stream, stride);
        vb->Release();
    }
    return s;
}

const D3DRENDERSTATETYPE kRecordedStates[14] = {D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ZFUNC, D3DRS_ALPHABLENDENABLE, D3DRS_SRCBLEND, D3DRS_DESTBLEND,
    D3DRS_BLENDOP, D3DRS_ALPHATESTENABLE, D3DRS_COLORWRITEENABLE, D3DRS_STENCILENABLE, D3DRS_CULLMODE, D3DRS_SRGBWRITEENABLE, D3DRS_DEPTHBIAS, D3DRS_SLOPESCALEDEPTHBIAS};
const char* kRecordedStateNames[14] = {"z", "zwrite", "zfunc", "blend", "src", "dst", "blendop", "atest", "cw", "stencil", "cull", "srgb", "depthbias(bits)", "slopebias(bits)"};

State g_state = State::Idle;
bool g_keyWasDown = false;
bool g_hooksRegistered = false;
bool g_inProbeCall = false;
POINT g_pixel{};
std::string g_captureWhy = "requested";  // why this capture was taken (the shortcut, or automatic after a floor change)
// After a capture by the shortcut: the same pixel is measured again 1 s and 3 s after each change of the camera's story
// (up to 6 automatic captures within 2 minutes), for a before / after of the same object across floor switches
struct Watch {
    bool on = false;
    POINT pixel{};
    std::map<uint32_t, int> stories; // the story each loaded lot showed (lot id low half -> story)
    DWORD until = 0;
    int left = 0;
    std::vector<std::pair<DWORD, std::string>> due; // tick, why
} g_watch;
std::map<uint32_t, int> ShownStories() {
    uint32_t lots[256];
    int stories[256];
    const int n = LevelLightShare::DisplayLevels(lots, stories, 256);
    std::map<uint32_t, int> m;
    for (int i = 0; i < n; i++) m[lots[i]] = stories[i];
    return m;
}
UINT g_bbWidth = 0, g_bbHeight = 0;
std::vector<DrawRec> g_draws;
std::vector<IDirect3DQuery9*> g_queryPool;
std::string g_status = "Ready. Mouse over the ground and Ctrl+Shift+F7.";

// the object draw of the last capture (WriteComparison)
struct Pick {
    bool valid = false;
    POINT pixel{};
    int index = -1;
    uintptr_t vs = 0, ps = 0;
    std::string bridge;
    ConstList psc, vsc;
    bool hasColor = false;
    float color[3] = {};
};
Pick g_prevPick;

struct TexInfo {
    IDirect3DBaseTexture9* tex = nullptr; // AddRef'd while listed
    std::string desc;
    std::string file;
    bool blank = false;
};
std::vector<TexInfo> g_textures; // textures used by the draws covering the pixel
bool g_blankWhite = false;
IDirect3DTexture9* g_blackTex = nullptr;
IDirect3DTexture9* g_whiteTex = nullptr;

std::filesystem::path OutDir() { return std::filesystem::path(ApexPaths::ApexDirectory()); }

// Every capture gets its own folder in Captures\ ("<date time> Light capture", features/captures.h: the report, the
// textures and shaders, the log and settings), so a capture never overwrites another one; nothing is deleted by itself.
std::filesystem::path g_capDir; // the capture being written (empty: none yet)
std::string g_capName;

std::filesystem::path CapDir() { return g_capDir.empty() ? Captures::Root() : g_capDir; }

void StartCaptureFolder() {
    const bool automatic = g_captureWhy.rfind("automatic", 0) == 0;
    g_capDir = Captures::NewFolder(automatic ? "Light capture (automatic)" : "Light capture");
    g_capName = g_capDir.filename().string();
}

const char* FmtName(D3DFORMAT f) {
    switch (static_cast<DWORD>(f)) {
    case D3DFMT_A8R8G8B8: return "A8R8G8B8";
    case D3DFMT_X8R8G8B8: return "X8R8G8B8";
    case D3DFMT_A16B16G16R16F: return "A16B16G16R16F";
    case D3DFMT_A32B32G32R32F: return "A32B32G32R32F";
    case D3DFMT_R32F: return "R32F";
    case D3DFMT_R16F: return "R16F";
    case D3DFMT_G16R16F: return "G16R16F";
    case D3DFMT_G32R32F: return "G32R32F";
    case D3DFMT_L8: return "L8";
    case D3DFMT_A8L8: return "A8L8";
    case D3DFMT_A8: return "A8";
    case D3DFMT_R5G6B5: return "R5G6B5";
    case D3DFMT_A2R10G10B10: return "A2R10G10B10";
    case D3DFMT_A16B16G16R16: return "A16B16G16R16";
    case D3DFMT_DXT1: return "DXT1";
    case D3DFMT_DXT3: return "DXT3";
    case D3DFMT_DXT5: return "DXT5";
    case MAKEFOURCC('I', 'N', 'T', 'Z'): return "INTZ";
    case MAKEFOURCC('A', 'T', 'I', '2'): return "ATI2";
    default: return nullptr;
    }
}
std::string FmtStr(D3DFORMAT f) {
    const char* n = FmtName(f);
    return n ? n : std::format("fmt{}", static_cast<unsigned>(f));
}

float HalfToFloat(uint16_t h) {
    const uint32_t s = (h >> 15) & 1, e = (h >> 10) & 0x1F, m = h & 0x3FF;
    float v;
    if (e == 0) v = std::ldexp(static_cast<float>(m), -24);
    else if (e == 31) v = m ? NAN : INFINITY;
    else v = std::ldexp(static_cast<float>(m | 0x400), static_cast<int>(e) - 25);
    return s ? -v : v;
}

// Converts one texel to RGBA floats. Returns false for unsupported formats.
bool Texel(D3DFORMAT f, const BYTE* p, float out[4]) {
    switch (static_cast<DWORD>(f)) {
    case D3DFMT_A8R8G8B8:
    case D3DFMT_X8R8G8B8:
        out[0] = p[2] / 255.f; out[1] = p[1] / 255.f; out[2] = p[0] / 255.f; out[3] = f == D3DFMT_X8R8G8B8 ? 1.f : p[3] / 255.f;
        return true;
    case D3DFMT_A16B16G16R16F: {
        auto h = reinterpret_cast<const uint16_t*>(p);
        for (int i = 0; i < 4; i++) out[i] = HalfToFloat(h[i]);
        return true;
    }
    case D3DFMT_A32B32G32R32F: std::memcpy(out, p, 16); return true;
    case D3DFMT_R32F: out[0] = out[1] = out[2] = *reinterpret_cast<const float*>(p); out[3] = 1; return true;
    case D3DFMT_R16F: out[0] = out[1] = out[2] = HalfToFloat(*reinterpret_cast<const uint16_t*>(p)); out[3] = 1; return true;
    case D3DFMT_G16R16F: {
        auto h = reinterpret_cast<const uint16_t*>(p);
        out[0] = HalfToFloat(h[0]); out[1] = HalfToFloat(h[1]); out[2] = 0; out[3] = 1;
        return true;
    }
    case D3DFMT_G32R32F: {
        auto v = reinterpret_cast<const float*>(p);
        out[0] = v[0]; out[1] = v[1]; out[2] = 0; out[3] = 1;
        return true;
    }
    case D3DFMT_L8: out[0] = out[1] = out[2] = p[0] / 255.f; out[3] = 1; return true;
    case D3DFMT_A8L8: out[0] = out[1] = out[2] = p[0] / 255.f; out[3] = p[1] / 255.f; return true;
    case D3DFMT_A8: out[0] = out[1] = out[2] = 0; out[3] = p[0] / 255.f; return true;
    case D3DFMT_A16B16G16R16: {
        auto v = reinterpret_cast<const uint16_t*>(p);
        for (int i = 0; i < 4; i++) out[i] = v[i] / 65535.f;
        return true;
    }
    case D3DFMT_A2R10G10B10: {
        const uint32_t v = *reinterpret_cast<const uint32_t*>(p);
        out[0] = ((v >> 20) & 0x3FF) / 1023.f; out[1] = ((v >> 10) & 0x3FF) / 1023.f; out[2] = (v & 0x3FF) / 1023.f; out[3] = (v >> 30) / 3.f;
        return true;
    }
    case D3DFMT_R5G6B5: {
        const uint16_t v = *reinterpret_cast<const uint16_t*>(p);
        out[0] = ((v >> 11) & 31) / 31.f; out[1] = ((v >> 5) & 63) / 63.f; out[2] = (v & 31) / 31.f; out[3] = 1;
        return true;
    }
    default: return false;
    }
}
UINT Bpp(D3DFORMAT f) {
    switch (static_cast<DWORD>(f)) {
    case D3DFMT_A16B16G16R16F: case D3DFMT_G32R32F: case D3DFMT_A16B16G16R16: return 8;
    case D3DFMT_A32B32G32R32F: return 16;
    case D3DFMT_R16F: case D3DFMT_A8L8: case D3DFMT_R5G6B5: return 2;
    case D3DFMT_L8: case D3DFMT_A8: return 1;
    default: return 4;
    }
}

bool IsDxt(D3DFORMAT f) { return f == D3DFMT_DXT1 || f == D3DFMT_DXT3 || f == D3DFMT_DXT5; }

// Decodes a locked DXT1/3/5 level into A8R8G8B8 (B, G, R, A bytes), w*4 bytes per row.
std::vector<BYTE> DecodeDxt(D3DFORMAT f, const BYTE* bits, INT pitch, UINT w, UINT h) {
    std::vector<BYTE> out(static_cast<size_t>(w) * h * 4);
    const UINT bw = (w + 3) / 4, bh = (h + 3) / 4, blockBytes = f == D3DFMT_DXT1 ? 8 : 16;
    for (UINT by = 0; by < bh; by++) {
        const BYTE* row = bits + static_cast<size_t>(by) * pitch;
        for (UINT bx = 0; bx < bw; bx++) {
            const BYTE* b = row + static_cast<size_t>(bx) * blockBytes;
            const BYTE* colour = f == D3DFMT_DXT1 ? b : b + 8;
            const uint16_t c0 = colour[0] | (colour[1] << 8), c1 = colour[2] | (colour[3] << 8);
            BYTE pal[4][4]; // B G R A
            auto expand = [](uint16_t c, BYTE* o) {
                o[2] = static_cast<BYTE>(((c >> 11) & 31) * 255 / 31);
                o[1] = static_cast<BYTE>(((c >> 5) & 63) * 255 / 63);
                o[0] = static_cast<BYTE>((c & 31) * 255 / 31);
                o[3] = 255;
            };
            expand(c0, pal[0]);
            expand(c1, pal[1]);
            const bool fourColour = f != D3DFMT_DXT1 || c0 > c1;
            for (int k = 0; k < 3; k++) {
                pal[2][k] = static_cast<BYTE>(fourColour ? (2 * pal[0][k] + pal[1][k]) / 3 : (pal[0][k] + pal[1][k]) / 2);
                pal[3][k] = static_cast<BYTE>(fourColour ? (pal[0][k] + 2 * pal[1][k]) / 3 : 0);
            }
            pal[2][3] = 255;
            pal[3][3] = fourColour ? 255 : 0;
            const uint32_t idx = colour[4] | (colour[5] << 8) | (colour[6] << 16) | (static_cast<uint32_t>(colour[7]) << 24);
            BYTE alpha[16];
            if (f == D3DFMT_DXT3) {
                for (int i = 0; i < 16; i++) alpha[i] = static_cast<BYTE>(((b[i / 2] >> ((i & 1) * 4)) & 15) * 17);
            } else if (f == D3DFMT_DXT5) {
                BYTE a[8] = {b[0], b[1]};
                for (int i = 2; i < 8; i++)
                    a[i] = static_cast<BYTE>(b[0] > b[1] ? ((8 - i) * b[0] + (i - 1) * b[1]) / 7 : i < 6 ? ((6 - i) * b[0] + (i - 1) * b[1]) / 5 : i == 6 ? 0 : 255);
                uint64_t bitsA = 0;
                for (int i = 0; i < 6; i++) bitsA |= static_cast<uint64_t>(b[2 + i]) << (8 * i);
                for (int i = 0; i < 16; i++) alpha[i] = a[(bitsA >> (3 * i)) & 7];
            }
            for (int i = 0; i < 16; i++) {
                const UINT x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                if (x >= w || y >= h) continue;
                BYTE* o = &out[(static_cast<size_t>(y) * w + x) * 4];
                std::memcpy(o, pal[(idx >> (2 * i)) & 3], 4);
                if (f != D3DFMT_DXT1) o[3] = alpha[i];
            }
        }
    }
    return out;
}

void WriteBmp(const std::filesystem::path& path, UINT w, UINT h, const std::vector<uint32_t>& bgra) {
    std::ofstream f(path, std::ios::binary);
    const uint32_t dataSize = w * h * 4;
    BITMAPFILEHEADER fh{};
    BITMAPINFOHEADER ih{};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(fh) + sizeof(ih);
    fh.bfSize = fh.bfOffBits + dataSize;
    ih.biSize = sizeof(ih);
    ih.biWidth = static_cast<LONG>(w);
    ih.biHeight = -static_cast<LONG>(h); // top-down
    ih.biPlanes = 1;
    ih.biBitCount = 32;
    ih.biCompression = BI_RGB;
    f.write(reinterpret_cast<const char*>(&fh), sizeof(fh));
    f.write(reinterpret_cast<const char*>(&ih), sizeof(ih));
    f.write(reinterpret_cast<const char*>(bgra.data()), dataSize);
}

// Saves level 0 of a 2D texture as BMP (RGB, scaled so the brightest channel is visible) and returns a stats line.
std::string DumpTexture(IDirect3DDevice9* dev, IDirect3DBaseTexture9* base, const std::string& name, std::string& fileOut) {
    if (base->GetType() != D3DRTYPE_TEXTURE) return "not a 2D texture (cube/volume), not saved";
    auto* tex = static_cast<IDirect3DTexture9*>(base);
    D3DSURFACE_DESC d{};
    if (FAILED(tex->GetLevelDesc(0, &d))) return "GetLevelDesc failed";
    const bool dxt = IsDxt(d.Format) && d.Pool != D3DPOOL_DEFAULT && d.Width <= 2048 && d.Height <= 2048;
    if (!dxt && (!FmtName(d.Format) || IsDxt(d.Format) || Bpp(d.Format) == 0 || d.Format == static_cast<D3DFORMAT>(MAKEFOURCC('I', 'N', 'T', 'Z')) ||
                 d.Format == static_cast<D3DFORMAT>(MAKEFOURCC('A', 'T', 'I', '2'))))
        return "compressed/depth format, not saved";
    float probe[4];
    BYTE zero[16] = {};
    if (!dxt && !Texel(d.Format, zero, probe)) return "format not supported, not saved";

    IDirect3DSurface9* src = nullptr;
    IDirect3DSurface9* sys = nullptr;
    if (FAILED(tex->GetSurfaceLevel(0, &src))) return "GetSurfaceLevel failed";
    IDirect3DSurface9* readable = src;
    if (d.Pool == D3DPOOL_DEFAULT) {
        if (!(d.Usage & D3DUSAGE_RENDERTARGET)) {
            src->Release();
            return "DEFAULT texture without render target, not readable";
        }
        if (FAILED(dev->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr)) || FAILED(dev->GetRenderTargetData(src, sys))) {
            if (sys) sys->Release();
            src->Release();
            return "GPU copy failed";
        }
        readable = sys;
    }
    D3DLOCKED_RECT lr{};
    if (FAILED(readable->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
        if (sys) sys->Release();
        src->Release();
        return "LockRect failed";
    }
    // DXT: decode to A8R8G8B8 first, then read it like any other texture
    const std::vector<BYTE> decoded = dxt ? DecodeDxt(d.Format, static_cast<const BYTE*>(lr.pBits), lr.Pitch, d.Width, d.Height) : std::vector<BYTE>{};
    const D3DFORMAT fmt = dxt ? D3DFMT_A8R8G8B8 : d.Format;
    const BYTE* bits = dxt ? decoded.data() : static_cast<const BYTE*>(lr.pBits);
    const size_t pitch = dxt ? static_cast<size_t>(d.Width) * 4 : static_cast<size_t>(lr.Pitch);
    const UINT bpp = Bpp(fmt);
    std::vector<float> rgb(static_cast<size_t>(d.Width) * d.Height * 3);
    std::vector<float> alpha(static_cast<size_t>(d.Width) * d.Height); // saved as a second (grey) image when it varies
    double sum[4] = {};
    float mx[4] = {-1e30f, -1e30f, -1e30f, -1e30f}, mn[4] = {1e30f, 1e30f, 1e30f, 1e30f};
    for (UINT y = 0; y < d.Height; y++) {
        const BYTE* row = bits + static_cast<size_t>(y) * pitch;
        for (UINT x = 0; x < d.Width; x++) {
            float t[4];
            Texel(fmt, row + x * bpp, t);
            for (int c = 0; c < 4; c++) {
                if (!std::isfinite(t[c])) t[c] = 0;
                sum[c] += t[c];
                mx[c] = std::max(mx[c], t[c]);
                mn[c] = std::min(mn[c], t[c]);
            }
            float* o = &rgb[(static_cast<size_t>(y) * d.Width + x) * 3];
            o[0] = t[0]; o[1] = t[1]; o[2] = t[2];
            alpha[static_cast<size_t>(y) * d.Width + x] = t[3];
        }
    }
    readable->UnlockRect();
    if (sys) sys->Release();
    src->Release();

    const float peak = std::max({mx[0], mx[1], mx[2], 1e-6f});
    const float scale = peak > 1.0f ? 1.0f / peak : 1.0f;
    std::vector<uint32_t> bgra(static_cast<size_t>(d.Width) * d.Height);
    for (size_t i = 0; i < bgra.size(); i++) {
        auto q = [&](float v) { return static_cast<uint32_t>(std::clamp(v * scale, 0.f, 1.f) * 255.f + 0.5f); };
        bgra[i] = 0xFF000000u | (q(rgb[i * 3]) << 16) | (q(rgb[i * 3 + 1]) << 8) | q(rgb[i * 3 + 2]);
    }
    std::filesystem::create_directories(CapDir());
    const std::string file = std::format("{}_{}x{}_{}.bmp", name, d.Width, d.Height, FmtStr(d.Format));
    WriteBmp(CapDir() / file, d.Width, d.Height, bgra);
    fileOut = file;
    if (mx[3] > mn[3]) { // the alpha channel carries data (e.g. room light maps): T<n>_..._alpha.bmp, grey = alpha
        std::vector<uint32_t> grey(alpha.size());
        for (size_t i = 0; i < grey.size(); i++) {
            const uint32_t a = static_cast<uint32_t>(std::clamp(alpha[i], 0.f, 1.f) * 255.f + 0.5f);
            grey[i] = 0xFF000000u | (a << 16) | (a << 8) | a;
        }
        WriteBmp(CapDir() / (file.substr(0, file.size() - 4) + "_alpha.bmp"), d.Width, d.Height, grey);
    }
    const double n = static_cast<double>(d.Width) * d.Height;
    return std::format("salva {}; media RGBA=({:.3f} {:.3f} {:.3f} {:.3f}) min=({:.3f} {:.3f} {:.3f}) max=({:.3f} {:.3f} {:.3f} {:.3f}){}", file, sum[0] / n, sum[1] / n,
        sum[2] / n, sum[3] / n, mn[0], mn[1], mn[2], mx[0], mx[1], mx[2], mx[3], scale < 1.0f ? std::format(" (imagem escalada por 1/{:.2f})", peak) : "");
}

std::string TexDesc(IDirect3DBaseTexture9* t) {
    if (!t) return "nenhuma";
    if (t->GetType() == D3DRTYPE_TEXTURE) {
        D3DSURFACE_DESC d{};
        static_cast<IDirect3DTexture9*>(t)->GetLevelDesc(0, &d);
        return std::format("2D {}x{} {} {}{} mips={}", d.Width, d.Height, FmtStr(d.Format), d.Pool == D3DPOOL_DEFAULT ? "DEFAULT" : "MANAGED/SYS",
            (d.Usage & D3DUSAGE_RENDERTARGET) ? " RT" : "", t->GetLevelCount());
    }
    if (t->GetType() == D3DRTYPE_CUBETEXTURE) return "cubo";
    return "volume";
}

IDirect3DTexture9* SolidTexture(IDirect3DDevice9* dev, IDirect3DTexture9*& slot, DWORD color) {
    if (slot) return slot;
    if (FAILED(dev->CreateTexture(1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &slot, nullptr))) return nullptr;
    D3DLOCKED_RECT lr{};
    if (SUCCEEDED(slot->LockRect(0, &lr, nullptr, 0))) {
        *static_cast<DWORD*>(lr.pBits) = color;
        slot->UnlockRect(0);
    }
    return slot;
}

bool AnyBlank() {
    for (auto& t : g_textures)
        if (t.blank) return true;
    return false;
}

// ---- per-draw work ----
template <typename DrawFn> D3D9Hooks::HookAction OnDraw(IDirect3DDevice9* dev, const char* kind, D3DPRIMITIVETYPE type, UINT prims, DrawFn draw) {
    if (g_inProbeCall) return D3D9Hooks::HookAction::Continue;

    if (g_state == State::Capturing && g_draws.size() < kMaxDraws) {
        IDirect3DSurface9* rt = nullptr;
        D3DSURFACE_DESC rd{};
        bool screenSized = false;
        if (SUCCEEDED(dev->GetRenderTarget(0, &rt)) && rt) {
            rt->GetDesc(&rd);
            screenSized = rd.Width == g_bbWidth && rd.Height == g_bbHeight;
            rt->Release();
        }
        if (screenSized) {
            DrawRec r;
            r.index = static_cast<int>(g_draws.size());
            r.kind = kind;
            r.type = type;
            r.prims = prims;
            for (DWORD s = 0; s < 16; s++) dev->GetTexture(s, &r.tex[s]); // AddRef'd, released later
            for (DWORD s = 0; s < 16; s++) {
                dev->GetSamplerState(s, D3DSAMP_MINFILTER, &r.filt[s][0]);
                dev->GetSamplerState(s, D3DSAMP_MAGFILTER, &r.filt[s][1]);
                dev->GetSamplerState(s, D3DSAMP_MIPFILTER, &r.filt[s][2]);
            }
            dev->GetVertexShader(&r.vs);
            dev->GetPixelShader(&r.ps);
            for (int i = 0; i < 14; i++) dev->GetRenderState(kRecordedStates[i], &r.rs[i]);
            static float tmp[kVsConsts][4];
            std::memset(tmp, 0, sizeof tmp);
            dev->GetPixelShaderConstantF(0, &tmp[0][0], kPsConsts);
            for (int i = 0; i < kPsConsts; i++)
                if (tmp[i][0] != 0 || tmp[i][1] != 0 || tmp[i][2] != 0 || tmp[i][3] != 0) r.psc.push_back({i, {tmp[i][0], tmp[i][1], tmp[i][2], tmp[i][3]}});
            std::memset(tmp, 0, sizeof tmp);
            dev->GetVertexShaderConstantF(0, &tmp[0][0], kVsConsts);
            for (int i = 0; i < kVsConsts; i++)
                if (tmp[i][0] != 0 || tmp[i][1] != 0 || tmp[i][2] != 0 || tmp[i][3] != 0) r.vsc.push_back({i, {tmp[i][0], tmp[i][1], tmp[i][2], tmp[i][3]}});
            r.bridge = LotLightBridge::DescribeDraw();
            if (std::strcmp(kind, "DIP") == 0) r.instances = ReadInstances(dev);

            IDirect3DQuery9* q = nullptr;
            if (r.index < static_cast<int>(g_queryPool.size())) q = g_queryPool[r.index];
            else if (SUCCEEDED(dev->CreateQuery(D3DQUERYTYPE_OCCLUSION, &q))) g_queryPool.push_back(q);
            if (q) {
                DWORD cw0, cw1, cw2, cw3, zw, swm, sc;
                RECT oldRect{};
                dev->GetRenderState(D3DRS_COLORWRITEENABLE, &cw0);
                dev->GetRenderState(D3DRS_COLORWRITEENABLE1, &cw1);
                dev->GetRenderState(D3DRS_COLORWRITEENABLE2, &cw2);
                dev->GetRenderState(D3DRS_COLORWRITEENABLE3, &cw3);
                dev->GetRenderState(D3DRS_ZWRITEENABLE, &zw);
                dev->GetRenderState(D3DRS_STENCILWRITEMASK, &swm);
                dev->GetRenderState(D3DRS_SCISSORTESTENABLE, &sc);
                dev->GetScissorRect(&oldRect);
                RECT px{g_pixel.x, g_pixel.y, g_pixel.x + 1, g_pixel.y + 1};
                dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0);
                dev->SetRenderState(D3DRS_COLORWRITEENABLE1, 0);
                dev->SetRenderState(D3DRS_COLORWRITEENABLE2, 0);
                dev->SetRenderState(D3DRS_COLORWRITEENABLE3, 0);
                dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
                dev->SetRenderState(D3DRS_STENCILWRITEMASK, 0);
                dev->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
                dev->SetScissorRect(&px);
                q->Issue(D3DISSUE_BEGIN);
                g_inProbeCall = true;
                draw();
                g_inProbeCall = false;
                q->Issue(D3DISSUE_END);
                dev->SetScissorRect(&oldRect);
                dev->SetRenderState(D3DRS_SCISSORTESTENABLE, sc);
                dev->SetRenderState(D3DRS_STENCILWRITEMASK, swm);
                dev->SetRenderState(D3DRS_ZWRITEENABLE, zw);
                dev->SetRenderState(D3DRS_COLORWRITEENABLE3, cw3);
                dev->SetRenderState(D3DRS_COLORWRITEENABLE2, cw2);
                dev->SetRenderState(D3DRS_COLORWRITEENABLE1, cw1);
                dev->SetRenderState(D3DRS_COLORWRITEENABLE, cw0);
                r.query = q;
            }
            g_draws.push_back(r);
        }
    }

    if (AnyBlank()) {
        IDirect3DBaseTexture9* saved[16] = {};
        bool swapped = false;
        IDirect3DTexture9* repl = g_blankWhite ? SolidTexture(dev, g_whiteTex, 0xFFFFFFFF) : SolidTexture(dev, g_blackTex, 0xFF000000);
        if (repl) {
            for (DWORD s = 0; s < 16; s++) {
                IDirect3DBaseTexture9* t = nullptr;
                dev->GetTexture(s, &t);
                if (!t) continue;
                bool hit = false;
                for (auto& info : g_textures)
                    if (info.blank && info.tex == t) hit = true;
                if (hit) {
                    saved[s] = t; // keep the reference until restored
                    dev->SetTexture(s, repl);
                    swapped = true;
                } else
                    t->Release();
            }
        }
        if (swapped) {
            g_inProbeCall = true;
            draw();
            g_inProbeCall = false;
            for (DWORD s = 0; s < 16; s++)
                if (saved[s]) {
                    dev->SetTexture(s, saved[s]);
                    saved[s]->Release();
                }
            return D3D9Hooks::HookAction::Skip;
        }
    }
    return D3D9Hooks::HookAction::Continue;
}

void RegisterHooks() {
    if (g_hooksRegistered) return;
    D3D9Hooks::RegisterDrawIndexedPrimitive(kHookName,
        [](D3D9Hooks::DeviceContext& ctx, D3DPRIMITIVETYPE type, INT bvi, UINT minV, UINT numV, UINT start, UINT prims) {
            return OnDraw(ctx.device, "DIP", type, prims, [&]() { ctx.device->DrawIndexedPrimitive(type, bvi, minV, numV, start, prims); });
        },
        D3D9Hooks::Priority::Last);
    D3D9Hooks::RegisterDrawPrimitive(kHookName,
        [](D3D9Hooks::DeviceContext& ctx, D3DPRIMITIVETYPE type, UINT start, UINT prims) {
            return OnDraw(ctx.device, "DP", type, prims, [&]() { ctx.device->DrawPrimitive(type, start, prims); });
        },
        D3D9Hooks::Priority::Last);
    g_hooksRegistered = true;
}

void UnregisterHooks() {
    if (!g_hooksRegistered) return;
    D3D9Hooks::UnregisterAll(kHookName);
    g_hooksRegistered = false;
}

void ReleaseDraws() {
    for (auto& d : g_draws) {
        for (auto*& t : d.tex)
            if (t) {
                t->Release();
                t = nullptr;
            }
        if (d.vs) d.vs->Release();
        if (d.ps) d.ps->Release();
        d.vs = nullptr;
        d.ps = nullptr;
    }
    g_draws.clear();
}

using D3DDisassemble_t = HRESULT(WINAPI*)(LPCVOID, SIZE_T, UINT, LPCSTR, ID3DBlob**);

// Saves the shader bytecode and its disassembly (d3dcompiler_47) as LightProbe\<name>.txt
template <typename S> std::string DumpShader(S* shader, const std::string& name) {
    if (!shader) return "nenhum";
    UINT size = 0;
    if (FAILED(shader->GetFunction(nullptr, &size)) || size == 0) return "GetFunction failed";
    std::vector<BYTE> code(size);
    if (FAILED(shader->GetFunction(code.data(), &size))) return "GetFunction failed";
    std::filesystem::create_directories(CapDir());
    {
        std::ofstream bin(CapDir() / (name + ".bin"), std::ios::binary);
        bin.write(reinterpret_cast<const char*>(code.data()), size);
    }
    static HMODULE mod = LoadLibraryA("d3dcompiler_47.dll");
    static auto dis = mod ? reinterpret_cast<D3DDisassemble_t>(GetProcAddress(mod, "D3DDisassemble")) : nullptr;
    if (!dis) return name + ".bin (no disassembler)";
    ID3DBlob* blob = nullptr;
    if (FAILED(dis(code.data(), size, 0, nullptr, &blob)) || !blob) return name + ".bin (disassembly failed)";
    std::ofstream txt(CapDir() / (name + ".txt"));
    txt.write(static_cast<const char*>(blob->GetBufferPointer()), static_cast<std::streamsize>(strnlen(static_cast<const char*>(blob->GetBufferPointer()), blob->GetBufferSize())));
    blob->Release();
    return name + ".txt";
}

void ClearTextures() {
    for (auto& t : g_textures)
        if (t.tex) t.tex->Release();
    g_textures.clear();
}

// The final colour on screen at the probed pixel (back buffer, 8 bits per channel).
bool ReadScreenPixel(IDirect3DDevice9* dev, float out[3]) {
    IDirect3DSurface9 *bb = nullptr, *rt = nullptr, *sys = nullptr;
    bool ok = false;
    if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
        D3DSURFACE_DESC d{};
        bb->GetDesc(&d);
        if ((d.Format == D3DFMT_X8R8G8B8 || d.Format == D3DFMT_A8R8G8B8) && SUCCEEDED(dev->CreateRenderTarget(1, 1, d.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, nullptr)) &&
            SUCCEEDED(dev->CreateOffscreenPlainSurface(1, 1, d.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr))) {
            RECT src{g_pixel.x, g_pixel.y, g_pixel.x + 1, g_pixel.y + 1};
            D3DLOCKED_RECT lr{};
            if (SUCCEEDED(dev->StretchRect(bb, &src, rt, nullptr, D3DTEXF_NONE)) && SUCCEEDED(dev->GetRenderTargetData(rt, sys)) &&
                SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
                const DWORD c = *static_cast<const DWORD*>(lr.pBits);
                out[0] = ((c >> 16) & 0xFF) / 255.0f;
                out[1] = ((c >> 8) & 0xFF) / 255.0f;
                out[2] = (c & 0xFF) / 255.0f;
                sys->UnlockRect();
                ok = true;
            }
        }
    }
    if (sys) sys->Release();
    if (rt) rt->Release();
    if (bb) bb->Release();
    return ok;
}

// Two captures in a row (piece A, then piece B of a modular object): which constants of the object draw differ.
void WriteComparison(std::ostream& out, const Pick& a, const Pick& b) {
    out << "== COMPARACAO COM A CAPTURA ANTERIOR (desenho do objeto em cada pixel) ==\n";
    auto line = [&](const char* name, const Pick& p) {
        out << std::format("{}: pixel ({}, {}) desenho #{} VS={:08X} PS={:08X} cor na tela {}\n   mod: {}\n", name, p.pixel.x, p.pixel.y, p.index, p.vs, p.ps,
            p.hasColor ? std::format("({:.3f} {:.3f} {:.3f})", p.color[0], p.color[1], p.color[2]) : std::string("(not read)"), p.bridge);
    };
    line("anterior", a);
    line("agora", b);
    out << (a.vs == b.vs && a.ps == b.ps ? "mesmo par de shaders\n" : "SHADERS DIFERENTES\n");
    auto diff = [&](const char* name, const ConstList& x, const ConstList& y) {
        std::map<int, std::pair<std::array<float, 4>, std::array<float, 4>>> m;
        for (const auto& [i, v] : x) m[i].first = v;
        for (const auto& [i, v] : y) m[i].second = v;
        int n = 0;
        out << name << " diferentes:";
        for (const auto& [i, p] : m) {
            bool differs = false;
            for (int k = 0; k < 4; k++) differs |= std::fabs(p.first[k] - p.second[k]) > 1e-4f * std::max(1.0f, std::fabs(p.first[k]));
            if (!differs) continue;
            n++;
            out << std::format("\n   [{}] antes ({:.4g} {:.4g} {:.4g} {:.4g}) agora ({:.4g} {:.4g} {:.4g} {:.4g})", i, p.first[0], p.first[1], p.first[2], p.first[3], p.second[0],
                p.second[1], p.second[2], p.second[3]);
        }
        out << std::format("\n   ({} registros)\n", n);
    };
    diff("PS", a.psc, b.psc);
    diff("VS", a.vsc, b.vsc);
    out << "\n";
}

void FinishCapture(IDirect3DDevice9* dev) {
    // Collect occlusion results
    const auto t0 = std::chrono::steady_clock::now();
    size_t pending = 0;
    do {
        pending = 0;
        for (auto& d : g_draws) {
            if (d.resolved || !d.query) continue;
            DWORD n = 0;
            const HRESULT hr = d.query->GetData(&n, sizeof(n), D3DGETDATA_FLUSH);
            if (hr == S_OK) {
                d.samples = n;
                d.resolved = true;
            } else if (hr == S_FALSE)
                pending++;
            else
                d.resolved = true;
        }
        if (pending) Sleep(1);
    } while (pending && std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(1500));

    ClearTextures();
    StartCaptureFolder();
    std::map<IDirect3DBaseTexture9*, int> texIndex;
    std::ostringstream out; // written to the capture folder and to ApexRadiance_LightProbe.txt (the latest) at the end
    out << std::format("S3SS Light Probe | {} | pixel ({}, {}) | tela {}x{} | desenhos na tela: {} | sem resposta: {}\n", g_capName, g_pixel.x, g_pixel.y, g_bbWidth, g_bbHeight,
        g_draws.size(), pending);
    out << "Capture: " << g_captureWhy << "\n";
    out << "Estados: z/zwrite/zfunc/blend/src/dst/blendop/atest/cw/stencil/cull/srgb/depthbias/slopebias (bits de float)\n\n";
    int covering = 0, pickRank = -1;
    const DrawRec* pick = nullptr;
    float screen[3] = {};
    const bool hasColor = ReadScreenPixel(dev, screen);
    for (auto& d : g_draws) {
        if (d.samples == 0) continue;
        covering++;
        out << std::format("== DESENHO #{} {} tipo={} prims={} amostras={} VS={:08X} PS={:08X}\n", d.index, d.kind, static_cast<int>(d.type), d.prims, d.samples,
            reinterpret_cast<uintptr_t>(d.vs), reinterpret_cast<uintptr_t>(d.ps));
        out << "   shaders: VS " << DumpShader(d.vs, std::format("VS_{:08X}", reinterpret_cast<uintptr_t>(d.vs))) << " | PS " << DumpShader(d.ps, std::format("PS_{:08X}", reinterpret_cast<uintptr_t>(d.ps))) << "\n";
        out << "   estados:";
        for (int i = 0; i < 14; i++) out << std::format(" {}={}", kRecordedStateNames[i], d.rs[i]);
        out << "\n";
        for (int s = 0; s < 16; s++) {
            if (!d.tex[s]) continue;
            auto it = texIndex.find(d.tex[s]);
            if (it == texIndex.end()) {
                TexInfo info;
                info.tex = d.tex[s];
                info.tex->AddRef();
                info.desc = TexDesc(d.tex[s]);
                g_textures.push_back(info);
                it = texIndex.emplace(d.tex[s], static_cast<int>(g_textures.size())).first;
            }
            out << std::format("   s{}: T{} {} filtro min/mag/mip={}/{}/{}\n", s, it->second, TexDesc(d.tex[s]), d.filt[s][0], d.filt[s][1], d.filt[s][2]);
        }
        out << "   mod: " << d.bridge << "\n";
        if (!d.instances.empty()) out << "   " << d.instances << "\n";
        out << "   PS c0..c223 (zeros omitidos):";
        for (const auto& [i, v] : d.psc) out << std::format(" [{}]({:.9g} {:.9g} {:.9g} {:.9g})", i, v[0], v[1], v[2], v[3]);
        out << "\n   VS c0..c255 (zeros omitidos):";
        for (const auto& [i, v] : d.vsc) out << std::format(" [{}]({:.9g} {:.9g} {:.9g} {:.9g})", i, v[0], v[1], v[2], v[3]);
        out << "\n\n";
        // the object draw of this pixel, for the comparison with the next capture: the last one the mod lit, else the
        // last one of a rig-lit object, else the last scene draw (a full-screen pass, 2 triangles, only if nothing else)
        const int rank = d.bridge.starts_with("OBJECT") ? 3 : d.bridge.find("object with rig") != std::string::npos ? 2 : d.prims > 2 ? 1 : 0;
        if (!pick || rank >= pickRank) {
            pick = &d;
            pickRank = rank;
        }
    }
    Pick now;
    if (pick) {
        now.valid = true;
        now.pixel = g_pixel;
        now.index = pick->index;
        now.vs = reinterpret_cast<uintptr_t>(pick->vs);
        now.ps = reinterpret_cast<uintptr_t>(pick->ps);
        now.bridge = pick->bridge;
        now.psc = pick->psc;
        now.vsc = pick->vsc;
        now.hasColor = hasColor;
        std::memcpy(now.color, screen, sizeof screen);
    }
    out << std::format("== COR NA TELA NO PIXEL: {}\n\n", hasColor ? std::format("({:.3f} {:.3f} {:.3f})", screen[0], screen[1], screen[2]) : std::string("(not read)"));
    if (g_prevPick.valid && now.valid) WriteComparison(out, g_prevPick, now);
    if (now.valid) g_prevPick = std::move(now);
    out << "== TEXTURAS DOS DESENHOS QUE PINTAM O PIXEL ==\n";
    for (size_t i = 0; i < g_textures.size(); i++) {
        std::string file;
        const std::string stats = DumpTexture(dev, g_textures[i].tex, std::format("T{}", i + 1), file);
        g_textures[i].file = file;
        out << std::format("T{} {}: {}\n", i + 1, g_textures[i].desc, stats);
    }
    const std::string report = out.str();
    Captures::WriteText(CapDir() / "Light capture.txt", report);
    ReleaseDraws();
    g_status = std::format("Measured: {} draws cover pixel ({}, {}), {} textures saved in Captures\\{}.", covering, g_pixel.x, g_pixel.y, g_textures.size(), g_capName);
    LOG_INFO("[LightProbe] " + g_status);
    Recorder::Note(std::format("[probe] {} ({})", g_capName, g_captureWhy));
    Captures::Finish(g_capDir, std::format("a light capture of the pixel ({}, {}) under the mouse: the {} draws that paint it and their textures ({})", g_pixel.x,
                                           g_pixel.y, covering, g_captureWhy), Captures::CaptureKind::LightCapture);
}

} // namespace

namespace LightProbe {

bool Capturing() { return g_state == State::Capturing; }
bool Busy() { return g_state != State::Idle || g_aiming.load() || g_aimClick.load() != 0; }
void Aim() { g_watch = Watch{}; g_aimClick.store(0); g_aiming.store(true); }
bool Aiming() { return g_aiming.load(); }
void CancelAim() { g_aiming.store(false); g_aimClick.store(0); }
bool ConfirmAim(POINT clientPixel) {
    if (!g_aiming.exchange(false)) return false;
    const uint32_t xy = static_cast<uint16_t>(clientPixel.x) | (static_cast<uint32_t>(static_cast<uint16_t>(clientPixel.y)) << 16);
    g_aimClick.store((uint64_t{1} << 32) | xy);
    return true;
}

void OnPresent(IDirect3DDevice9* dev) {
    if (!dev) return;
    if (g_state == State::Capturing) {
        g_state = State::Idle;
        FinishCapture(dev);
        if (!AnyBlank()) UnregisterHooks();
    } else if (g_state == State::Armed) {
        g_state = State::Capturing; // draws of the next frame are recorded
    }

    // Its shortcut (Hotkeys: Ctrl+Shift+V, 4 or F7 by preset), eaten before the game sees it
    const bool pressed = Hotkeys::Take(Hotkeys::Action::Probe);
    const uint64_t click = g_aimClick.exchange(0);
    if (pressed && g_state != State::Idle) LOG_INFO("[LightProbe] Its shortcut while a capture is still running: ignored");
    const auto start = [&](POINT pixel, const std::string& why) {
        IDirect3DSurface9* bb = nullptr;
        D3DSURFACE_DESC bd{};
        if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
            bb->GetDesc(&bd);
            bb->Release();
        }
        g_bbWidth = bd.Width;
        g_bbHeight = bd.Height;
        g_pixel = pixel;
        g_captureWhy = why;
        ReleaseDraws();
        RegisterHooks();
        g_state = State::Armed;
        g_status = std::format("Measuring pixel ({}, {})...", g_pixel.x, g_pixel.y);
        LOG_INFO("[LightProbe] Capture requested: " + g_status + " (" + why + ")");
    };
    const DWORD now = GetTickCount();
    if ((pressed || click) && g_state == State::Idle) {
        IDirect3DSurface9* bb = nullptr;
        D3DSURFACE_DESC bd{};
        if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
            bb->GetDesc(&bd);
            bb->Release();
        }
        D3DDEVICE_CREATION_PARAMETERS cp{};
        dev->GetCreationParameters(&cp);
        POINT p{};
        HWND wnd = cp.hFocusWindow ? cp.hFocusWindow : GetForegroundWindow();
        if (click) {
            p.x = static_cast<short>(click & 0xFFFF);
            p.y = static_cast<short>((click >> 16) & 0xFFFF);
        } else if (!GetCursorPos(&p) || !ScreenToClient(wnd, &p)) return;
        RECT cr{};
        if (!GetClientRect(wnd, &cr) || !bd.Width || !bd.Height) return;
        if ((click || g_aiming.load()) && (!PtInRect(&cr, p) || GetForegroundWindow() != wnd)) return;
        const LONG cw = std::max<LONG>(1, cr.right - cr.left), ch = std::max<LONG>(1, cr.bottom - cr.top);
        POINT pixel{};
        pixel.x = std::clamp<LONG>(static_cast<LONG>(static_cast<double>(p.x) * bd.Width / cw), 0, static_cast<LONG>(bd.Width) - 1);
        pixel.y = std::clamp<LONG>(static_cast<LONG>(static_cast<double>(p.y) * bd.Height / ch), 0, static_cast<LONG>(bd.Height) - 1);
        const bool guided = g_aiming.exchange(false) || click != 0;
        start(pixel, guided ? "guided one-shot capture under the mouse" : "requested by the shortcut; the same pixel is measured again 1 s and 3 s after any lot changes the story it shows, for 2 minutes");
        Captures::Notify(I18n::Tr("Capturing the light under the mouse\xE2\x80\xA6"), 3, Captures::NoteKind::Probe);
        g_watch = guided ? Watch{} : Watch{true, pixel, ShownStories(), now + 120000, 6, {}};
        return;
    }
    // The automatic captures after a floor change
    if (!g_watch.on) return;
    if (static_cast<int32_t>(now - g_watch.until) >= 0 || g_watch.left <= 0) {
        g_watch.on = false;
        return;
    }
    const std::map<uint32_t, int> shown = ShownStories();
    std::string change;
    for (const auto& [lot, story] : shown)
        if (const auto it = g_watch.stories.find(lot); it != g_watch.stories.end() && it->second != story)
            change += std::format("{}lot {:08X} story {} -> {}", change.empty() ? "" : ", ", lot, it->second, story);
    g_watch.stories = shown;
    if (!change.empty()) {
        g_watch.due.push_back({now + 1000, "automatic, 1 s after the floor change (" + change + ")"});
        g_watch.due.push_back({now + 3000, "automatic, 3 s after the floor change (" + change + ")"});
        Recorder::Note("[probe] floor change (" + change + "): automatic captures in 1 s and 3 s");
    }
    if (g_state == State::Idle && !g_watch.due.empty() && static_cast<int32_t>(now - g_watch.due.front().first) >= 0) {
        const std::string why = g_watch.due.front().second;
        g_watch.due.erase(g_watch.due.begin());
        g_watch.left--;
        start(g_watch.pixel, why);
    }
}

void RenderUI() {
    ImGui::TextWrapped("Measure: put the mouse over the spot and press Ctrl+Shift+F7. Two measurements in a row (piece A, then piece B) are compared at the end of the file.");
    ImGui::TextWrapped("%s", g_status.c_str());
    if (g_textures.empty()) return;
    ImGui::TextWrapped("Textures used at this pixel. Tick one to replace it with a solid colour and see the effect on screen:");
    bool changed = false;
    changed |= ImGui::Checkbox("Replace with white (unticked = black)", &g_blankWhite);
    for (size_t i = 0; i < g_textures.size(); i++) {
        const std::string label = std::format("T{}: {}##probe{}", i + 1, g_textures[i].desc, i);
        changed |= ImGui::Checkbox(label.c_str(), &g_textures[i].blank);
    }
    if (AnyBlank()) RegisterHooks();
    else if (g_state == State::Idle) UnregisterHooks();
    if (ImGui::Button("Untick all")) {
        for (auto& t : g_textures) t.blank = false;
        if (g_state == State::Idle) UnregisterHooks();
    }
    (void)changed;
}

void Shutdown() {
    CancelAim();
    UnregisterHooks();
    ReleaseDraws();
    ClearTextures();
    for (auto* q : g_queryPool)
        if (q) q->Release();
    g_queryPool.clear();
    if (g_blackTex) {
        g_blackTex->Release();
        g_blackTex = nullptr;
    }
    if (g_whiteTex) {
        g_whiteTex->Release();
        g_whiteTex = nullptr;
    }
    g_state = State::Idle;
}

} // namespace LightProbe
