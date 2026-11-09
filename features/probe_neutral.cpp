// Lamp colours stay near lamps (see probe_neutral.h)
#include "probe_neutral.h"
#include "entry_chain.h"
#include "game_addresses.h"
#include "apex_log.h"
#include "recorder.h"
#include "level_light_share.h"
#include <windows.h>
#include <d3d9.h>
#include <atomic>
#include <cstdint>
#include <format>
#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace ProbeNeutral {
namespace {
using EntryChain::Layer;
using EntryChain::Site;

// LightProbe fields (ctor 0x006B4240; writers 0x006B4720 / 0x006B4B10; blend 0x006B3160 lerps the old and new cubes into the
// bound ones): bound diffuse / specular, the new capture when double-buffered (byte +0x2C97)
constexpr uintptr_t kDiffuse = 0x2C50, kSpecular = 0x2C5C, kDiffNew = 0x2C60, kSpecNew = 0x2C64, kDoubleBuffered = 0x2C97;
// Game texture object: +0x2C kind (1 = cube, 0x00619910), +0x30 the D3D texture (0x00618FB0 / 0x006192E0 lock it)
constexpr uintptr_t kTexKind = 0x2C, kTexD3D = 0x30;

enum Result : int { kDone, kNoTexture, kNotCube, kFormat, kLockFailed, kFault };
struct CubeStats { // POD (filled inside __try)
    uint32_t texels;
    uint32_t size;
    uint32_t levels;
    uintptr_t field;
};

std::mutex g_ctrl;
bool g_installed = false;
std::atomic<bool> g_on{false};
std::atomic<float> g_strength{1.0f}, g_night{0.0f};
std::atomic<long> c_diff{0}, c_spec{0}, c_done{0}, c_day{0}, c_fail{0};
std::atomic<int> g_lastBucket{-1};
std::atomic<bool> g_failLogged{false};

// None until the night level passes 0.5 (sunset keeps its colours), smooth to full at 0.9; x strength
int Amount256() {
    if (!g_on.load(std::memory_order_relaxed)) return 0;
    float w = (g_night.load(std::memory_order_relaxed) - 0.5f) / 0.4f;
    w = w < 0.0f ? 0.0f : (w > 1.0f ? 1.0f : w);
    w = w * w * (3.0f - 2.0f * w);
    return static_cast<int>(w * g_strength.load(std::memory_order_relaxed) * 256.0f + 0.5f);
}

// rgb -> lerp(rgb, luma x tint, a/256), alpha kept; Rec.709 weights on the stored 8-bit values (54 + 183 + 19 = 256).
// tint (x256, luma 256) is the colour of the cube's top face: the night sky for the exterior probe (its blue stays, the
// lamps' pools on the ground and walls lose their hue), the ceiling for a room probe (a room keeps its own lamps' colour;
// 07/10 review: a plain grey also took the moonlight blue away)
void NeutralRows(uint8_t* bits, int pitch, uint32_t w, uint32_t h, int a, const int* tint) {
    for (uint32_t y = 0; y < h; y++) {
        uint32_t* p = reinterpret_cast<uint32_t*>(bits + static_cast<intptr_t>(y) * pitch);
        for (uint32_t x = 0; x < w; x++) {
            const uint32_t c = p[x];
            const int r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
            const int l = (54 * r + 183 * g + 19 * b + 128) >> 8;
            const int tr = std::min(255, (l * tint[0]) >> 8), tg = std::min(255, (l * tint[1]) >> 8), tb = std::min(255, (l * tint[2]) >> 8);
            const int r2 = r + (((tr - r) * a) >> 8), g2 = g + (((tg - g) * a) >> 8), b2 = b + (((tb - b) * a) >> 8);
            p[x] = (c & 0xFF000000u) | (static_cast<uint32_t>(r2) << 16) | (static_cast<uint32_t>(g2) << 8) | static_cast<uint32_t>(b2);
        }
    }
}

// The cube the writer just filled (single or double-buffered field), every level and face (POD only, SEH)
int NeutralProbeCube(uint8_t* probe, uintptr_t single, uintptr_t buffered, int a, CubeStats* st) {
    __try {
        const uintptr_t field = *reinterpret_cast<const uint8_t*>(probe + kDoubleBuffered) ? buffered : single;
        st->field = field;
        const uint8_t* tex = *reinterpret_cast<uint8_t* const*>(probe + field);
        if (!tex) return kNoTexture;
        if (*reinterpret_cast<const uint32_t*>(tex + kTexKind) != 1) return kNotCube;
        IDirect3DCubeTexture9* cube = *reinterpret_cast<IDirect3DCubeTexture9* const*>(tex + kTexD3D);
        if (!cube || cube->GetType() != D3DRTYPE_CUBETEXTURE) return kNotCube;
        const DWORD levels = cube->GetLevelCount();
        st->levels = levels;
        // the top face's mean colour (level 0) as the tint, luma 256; too dark to tell: grey
        int tint[3] = {256, 256, 256};
        {
            D3DSURFACE_DESC d0;
            D3DLOCKED_RECT lr;
            if (SUCCEEDED(cube->GetLevelDesc(0, &d0)) && (d0.Format == D3DFMT_A8R8G8B8 || d0.Format == D3DFMT_X8R8G8B8) &&
                SUCCEEDED(cube->LockRect(D3DCUBEMAP_FACE_POSITIVE_Y, 0, &lr, nullptr, D3DLOCK_READONLY))) {
                uint64_t sr = 0, sg = 0, sb = 0;
                for (uint32_t y = 0; y < d0.Height; y++) {
                    const uint32_t* p = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(lr.pBits) + static_cast<intptr_t>(y) * lr.Pitch);
                    for (uint32_t x = 0; x < d0.Width; x++) sr += (p[x] >> 16) & 0xFF, sg += (p[x] >> 8) & 0xFF, sb += p[x] & 0xFF;
                }
                cube->UnlockRect(D3DCUBEMAP_FACE_POSITIVE_Y, 0);
                const uint64_t lum = (54 * sr + 183 * sg + 19 * sb) >> 8;
                const uint64_t n = static_cast<uint64_t>(d0.Width) * d0.Height;
                if (n && lum >= 2 * n) { // mean luma >= 2 of 255
                    tint[0] = static_cast<int>(std::min<uint64_t>(512, sr * 256 / lum));
                    tint[1] = static_cast<int>(std::min<uint64_t>(512, sg * 256 / lum));
                    tint[2] = static_cast<int>(std::min<uint64_t>(512, sb * 256 / lum));
                }
            }
        }
        for (DWORD lv = 0; lv < levels; lv++) {
            D3DSURFACE_DESC d;
            if (FAILED(cube->GetLevelDesc(lv, &d))) return kLockFailed;
            if (d.Format != D3DFMT_A8R8G8B8 && d.Format != D3DFMT_X8R8G8B8) return kFormat;
            if (lv == 0) st->size = d.Width;
            for (int f = 0; f < 6; f++) {
                D3DLOCKED_RECT lr;
                const D3DCUBEMAP_FACES face = static_cast<D3DCUBEMAP_FACES>(f);
                if (FAILED(cube->LockRect(face, lv, &lr, nullptr, 0))) return kLockFailed; // as the game locks them
                NeutralRows(static_cast<uint8_t*>(lr.pBits), lr.Pitch, d.Width, d.Height, a, tint);
                cube->UnlockRect(face, lv);
                st->texels += d.Width * d.Height;
            }
        }
        return kDone;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return kFault;
    }
}

const char* ResultText(int r) {
    switch (r) {
    case kNoTexture: return "no texture";
    case kNotCube: return "not a cube";
    case kFormat: return "format not 8-bit ARGB";
    case kLockFailed: return "LockRect failed";
    case kFault: return "access fault";
    default: return "done";
    }
}

// (developer mode) Cubes up to this size keep their pixels for the capture
constexpr uint32_t kStripSide = 128;

// The mean colour of each face of the cube the writer just filled, level 0, as the scene reads it from now on, and with strip
// (room for kStripSide) the faces side by side, B G R rows (POD only, SEH)
bool FaceMeans(uint8_t* probe, uintptr_t single, uintptr_t buffered, float mean[6][3], uint32_t* size, uint8_t* strip) {
    __try {
        const uintptr_t field = *reinterpret_cast<const uint8_t*>(probe + kDoubleBuffered) ? buffered : single;
        const uint8_t* tex = *reinterpret_cast<uint8_t* const*>(probe + field);
        if (!tex || *reinterpret_cast<const uint32_t*>(tex + kTexKind) != 1) return false;
        IDirect3DCubeTexture9* cube = *reinterpret_cast<IDirect3DCubeTexture9* const*>(tex + kTexD3D);
        D3DSURFACE_DESC d;
        if (!cube || cube->GetType() != D3DRTYPE_CUBETEXTURE || FAILED(cube->GetLevelDesc(0, &d)) || (d.Format != D3DFMT_A8R8G8B8 && d.Format != D3DFMT_X8R8G8B8)) return false;
        *size = d.Width;
        for (int f = 0; f < 6; f++) {
            D3DLOCKED_RECT lr;
            const D3DCUBEMAP_FACES face = static_cast<D3DCUBEMAP_FACES>(f);
            if (FAILED(cube->LockRect(face, 0, &lr, nullptr, D3DLOCK_READONLY))) return false;
            uint64_t sr = 0, sg = 0, sb = 0;
            for (uint32_t y = 0; y < d.Height; y++) {
                const uint32_t* p = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(lr.pBits) + static_cast<intptr_t>(y) * lr.Pitch);
                for (uint32_t x = 0; x < d.Width; x++) sr += (p[x] >> 16) & 0xFF, sg += (p[x] >> 8) & 0xFF, sb += p[x] & 0xFF;
                if (strip && d.Width <= kStripSide)
                    for (uint32_t x = 0; x < d.Width; x++) {
                        uint8_t* o = strip + (static_cast<size_t>(y) * d.Width * 6 + static_cast<size_t>(f) * d.Width + x) * 3;
                        o[0] = p[x] & 0xFF, o[1] = (p[x] >> 8) & 0xFF, o[2] = (p[x] >> 16) & 0xFF;
                    }
            }
            cube->UnlockRect(face, 0);
            const float n = static_cast<float>(d.Width) * static_cast<float>(d.Height);
            mean[f][0] = static_cast<float>(sr) / n, mean[f][1] = static_cast<float>(sg) / n, mean[f][2] = static_cast<float>(sb) / n;
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// During a recording, by day too: the [cube] line of this probe
void NoteCube(uint8_t* probe, uintptr_t single, uintptr_t buffered, const char* what) {
    if (!Recorder::Active()) return;
    float mean[6][3];
    uint32_t size = 0;
    std::vector<unsigned char> strip(kPublicBuild ? 0 : static_cast<size_t>(kStripSide) * kStripSide * 18);
    if (!FaceMeans(probe, single, buffered, mean, &size, strip.empty() ? nullptr : strip.data())) return;
    strip.resize(size <= kStripSide ? static_cast<size_t>(size) * size * 18 : 0);
    Recorder::NoteCube(reinterpret_cast<uintptr_t>(probe), what, mean, size, std::move(strip));
}

void AfterWrite(uint8_t* probe, uintptr_t single, uintptr_t buffered, const char* what) {
    const int a = Amount256();
    if (a <= 0) {
        c_day.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    LARGE_INTEGER t0, t1, fq;
    QueryPerformanceCounter(&t0);
    CubeStats st{};
    const int r = NeutralProbeCube(probe, single, buffered, a, &st);
    QueryPerformanceCounter(&t1);
    if (r != kDone) {
        c_fail.fetch_add(1, std::memory_order_relaxed);
        if (!g_failLogged.exchange(true))
            LOG_WARNING(std::format("[ProbeNeutral] {} cube of probe {:#x} (+{:#x}) left as the game wrote it: {}", what, reinterpret_cast<uintptr_t>(probe), st.field, ResultText(r)));
        return;
    }
    c_done.fetch_add(1, std::memory_order_relaxed);
    const int bucket = a / 64; // a line per quarter step of the weight (dusk, dawn, setting changes), not per capture
    if (g_lastBucket.exchange(bucket) != bucket) {
        QueryPerformanceFrequency(&fq);
        LOG_INFO(std::format("[ProbeNeutral] Lamp colours out of the sky light at {}% (night {:.2f}): {} cube of probe {:#x} (+{:#x}), {} px x {} levels, {} texels, {:.3f} ms",
                             a * 100 / 256, g_night.load(), what, reinterpret_cast<uintptr_t>(probe), st.field, st.size, st.levels, st.texels,
                             (t1.QuadPart - t0.QuadPart) * 1000.0 / fq.QuadPart));
    }
}

using FnDiffuse = void(__fastcall*)(uint8_t* probe, void* edx);
using FnSpecular = char(__fastcall*)(uint8_t* probe, void* edx, void* rt);

// 0x006B4720 thiscall(probe), ret (one caller, 0x006B55A8)
void __fastcall DiffuseWriteHook(uint8_t* probe, void* edx) {
    reinterpret_cast<FnDiffuse>(EntryChain::Next(Site::ProbeDiffuseWrite, Layer::ProbeNeutral))(probe, edx);
    c_diff.fetch_add(1, std::memory_order_relaxed);
    AfterWrite(probe, kDiffuse, kDiffNew, "diffuse");
    NoteCube(probe, kDiffuse, kDiffNew, "diffuse");
}
// Who took a capture. The game re-queues a probe when |clock - last| reaches its period (0x006B3100: +0x2B44 the clock, +0x2C6C
// last, +0x2C70 the period), and 0x006B1CA0 moves last so that happens in 2 s, for a solve (0x69F5F0) and a probe the pool gives
// to a room (0x006B04D5). Apex's ReflectionDue writes last too (NoteArmed). None of that leaves a mark on the probe, so this tells
// them apart by what it sees: armed by Apex, in another room than at its last capture, a full period since it, or sooner.
std::mutex g_armedMx;
std::unordered_map<const void*, DWORD> g_armed; // under g_armedMx: the tick Apex made it due
struct LastCapture {
    float clock = 0.0f;
    int story = -1, id = -1;
};
std::unordered_map<const uint8_t*, LastCapture> g_captures; // (the thread that writes the cubes)
constexpr DWORD kArmedMs = 5000;
void ReadClock(const uint8_t* probe, float* clock, float* every) {
    __try {
        *clock = *reinterpret_cast<const float*>(probe + 0x2B44), *every = *reinterpret_cast<const float*>(probe + 0x2C70);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}
std::string CapturePath(const uint8_t* probe) {
    const DWORD now = GetTickCount();
    long armedAgo = -1;
    {
        std::lock_guard<std::mutex> lk(g_armedMx);
        if (const auto it = g_armed.find(probe); it != g_armed.end()) {
            if (now - it->second < kArmedMs) armedAgo = static_cast<long>(now - it->second);
            g_armed.erase(it);
        }
    }
    float clock = 0.0f, every = 0.0f;
    ReadClock(probe, &clock, &every);
    LastCapture cur{clock};
    const bool known = LevelLightShare::RoomOfProbe(reinterpret_cast<uintptr_t>(probe), &cur.story, &cur.id);
    const auto [last, first] = g_captures.try_emplace(probe, cur);
    const LastCapture was = last->second;
    last->second = cur;
    const std::string room = known ? std::format("story {} room {}", cur.story, cur.id) : "a room not known";
    const std::string gap = first ? "its first" : std::format("{:.1f} s since its last, period {:.0f} s", clock - was.clock, every);
    if (armedAgo >= 0) return std::format("{}: Apex's lamp edit, made due {} ms ago ({})", room, armedAgo, gap);
    if (first) return std::format("{}: the game ({})", room, gap);
    if (was.id != cur.id || was.story != cur.story) return std::format("{}: the game's probe pool, from story {} room {} ({})", room, was.story, was.id, gap);
    return std::format("{}: the game's {} ({})", room, clock - was.clock >= every - 1.0f ? "period" : "solve", gap);
}

// 0x006B4B10 thiscall(probe, rt), ret 4, al (one caller, 0x006B5667): false = nothing was written
char __fastcall SpecularWriteHook(uint8_t* probe, void* edx, void* rt) {
    const char ok = reinterpret_cast<FnSpecular>(EntryChain::Next(Site::ProbeSpecularWrite, Layer::ProbeNeutral))(probe, edx, rt);
    c_spec.fetch_add(1, std::memory_order_relaxed);
    if (ok) AfterWrite(probe, kSpecular, kSpecNew, "specular"), NoteCube(probe, kSpecular, kSpecNew, "specular");
    if (ok && Recorder::Verbose()) { // (a recording holds the log's lines too)
        const std::string line = std::format("[ProbeNeutral] Reflection cube of probe {:#x} taken for {}", reinterpret_cast<uintptr_t>(probe), CapturePath(probe));
        if (Recorder::Active()) Recorder::Note(line);
        else LOG_INFO(line);
    }
    return ok;
}
} // namespace

bool Install(std::string& error) {
    std::lock_guard<std::mutex> lk(g_ctrl);
    if (g_installed) return true;
    using GameAddr::Id;
    std::string why;
    if (!GameAddr::Have({Id::ProbeDiffuseWrite, Id::ProbeSpecularWrite}, &why)) {
        error = "Lamp colours stay near lamps: " + GameAddr::NotAvailable(why);
        return false;
    }
    std::string err;
    if (!EntryChain::Install(Site::ProbeDiffuseWrite, Layer::ProbeNeutral, reinterpret_cast<void*>(&DiffuseWriteHook), &err)) {
        error = "Lamp colours stay near lamps: " + err;
        return false;
    }
    if (!EntryChain::Install(Site::ProbeSpecularWrite, Layer::ProbeNeutral, reinterpret_cast<void*>(&SpecularWriteHook), &err)) {
        EntryChain::Remove(Site::ProbeDiffuseWrite, Layer::ProbeNeutral);
        error = "Lamp colours stay near lamps: " + err;
        return false;
    }
    g_installed = true;
    LOG_INFO(std::format("[ProbeNeutral] Installed on the light probe writers {:#x} (diffuse) and {:#x} (specular)", GameAddr::Get(Id::ProbeDiffuseWrite),
                         GameAddr::Get(Id::ProbeSpecularWrite)));
    return true;
}

void Uninstall() {
    std::lock_guard<std::mutex> lk(g_ctrl);
    if (!g_installed) return;
    EntryChain::Remove(Site::ProbeSpecularWrite, Layer::ProbeNeutral);
    EntryChain::Remove(Site::ProbeDiffuseWrite, Layer::ProbeNeutral);
    g_installed = false;
    LOG_INFO(std::format("[ProbeNeutral] Uninstalled (writes: diffuse {}, specular {}; made neutral {}; by day {}; failed {})", c_diff.load(), c_spec.load(), c_done.load(),
                         c_day.load(), c_fail.load()));
}

void Set(bool on, float strength) {
    g_on.store(on, std::memory_order_relaxed);
    g_strength.store(strength < 0.0f ? 0.0f : (strength > 1.0f ? 1.0f : strength), std::memory_order_relaxed);
}
void SetNightLevel(float level) { g_night.store(level, std::memory_order_relaxed); }
long Writes() { return c_diff.load(std::memory_order_relaxed) + c_spec.load(std::memory_order_relaxed); }
void NoteArmed(const void* probe) {
    if (!Recorder::Verbose()) return;
    std::lock_guard<std::mutex> lk(g_armedMx);
    if (g_armed.size() > 64) g_armed.clear(); // (entries wait for a capture that may never come)
    g_armed[probe] = GetTickCount();
}

std::string Status() {
    return std::format("probe writes diffuse {} / specular {}, made neutral {}, by day {}, failed {}, weight now {}%", c_diff.load(), c_spec.load(), c_done.load(), c_day.load(),
                       c_fail.load(), Amount256() * 100 / 256);
}
} // namespace ProbeNeutral
