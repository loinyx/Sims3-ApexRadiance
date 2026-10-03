// Smoothed terrain light maps (part of Night Lighting)
//
// The world's lamp light on the ground comes from one 256x256 DXT5 light map per 256 m terrain chunk (1 texel per metre,
// see lot_light_bridge.cpp). Two visible problems come from that texture itself:
//  - blocky, "low resolution" lamp circles: 1 texel per metre stretched with plain bilinear filtering, plus the 4x4 DXT
//    blocks;
//  - purple / green specks at night: DXT stores colour as RGB565 endpoints, so dim light gets rounded to tinted values
//    (green has 6 bits, red and blue 5). On white snow this is very visible.
// Fix, texture side (no shader changes): every chunk light map the world draws is decoded, the colour noise is removed
// by blurring only the colour ratio (brightness stays sharp), and it is enlarged 4x with a cubic B-spline (smooth, no
// ringing) that reads across the neighbouring chunks, so there are no seams. The result (1024x1024 A8R8G8B8 with
// dithering and a full mip chain, in video memory) replaces the game's texture in the world terrain, lot and road draws.
// The heavy work runs on a worker thread.
//
// Correct first (28/09): when the game re-renders a chunk map (dusk, a lamp edit, a rebuild) the smoothed map of the OLD
// map is never shown: Get/Find return nullptr (the draw keeps the game's current map, correct but blockier) and the
// atlas cell gets a plain 2x copy of the current map at once, until the smoothed map of the new map is uploaded.
// Changes are found by a hash of the map (round robin, faster after a rebuild) and, when the terrain relight installed
// its call-site hook, by a notice from the game's own per-chunk texture re-render (NoteChunkRendered).
//
// GPU path (default since 28/09, CPU above kept as the fallback): the same smoothing done with pixel shaders
// (shaders/lightmap_smooth_ps.hlsl) on the render thread, reading the game's chunk maps directly (no lock, no decode, no
// upload, no worker), rendered into the chunk's own render-target texture and its atlas cell. A changed chunk is rebuilt
// by the first draw that needs it in the same frame (Get / Find / Atlas), so the smoothed map of the game's current map
// is on screen in the frame the game's map changed; chunks out of view and neighbour-border updates are spread over the
// frames (about 1 ms of GPU per frame, measured with timestamp queries). The plain atlas copies (AtlasRawCopy) are not
// needed on this path: nothing reads a cell between the change and its rebuild.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "lightmap_smooth.h"
#include "apex_log.h"
#include "build_flavor.h"
#include "d3d9_extra_hooks.h"
#include "lightmap_smooth_hlsl.h"
#include "shader_cache.h"
#include <windows.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <climits>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <format>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

constexpr int kSrc = 256;             // game light map size
constexpr int kScale = 4;             // enlargement
constexpr int kOut = kSrc * kScale;   // 1024
constexpr UINT kLevels = 11;          // 1024 .. 1
constexpr int kBorder = 6;            // texels read from the neighbouring chunks
constexpr int kExt = kSrc + 2 * kBorder;
constexpr int kChunkSize = 256;       // world units per chunk (key step)
constexpr int kMaxInFlight = 2;       // jobs queued + processing + results waiting (5.6 MB each result: 32-bit process)
constexpr uint32_t kSweepFrames = 300; // a full rebuild re-renders one chunk per frame: 300 frames covers 16x16 chunks
constexpr uint32_t kSettleFrames = 30; // neighbour re-smooths wait until no chunk changed for this many frames
constexpr double kRawBudgetMs = 2.0;   // plain atlas copies per frame (render thread)

using Plane = std::vector<float>;     // RGBA float, kSrc * kSrc * 4 (worker only)
using Raw = std::vector<uint8_t>;     // the game's level 0, DXT5, 64 KB (kept per chunk)
using RawPtr = std::shared_ptr<const Raw>;
constexpr int kPitch = kSrc / 4 * 16;  // DXT5 bytes per row of blocks

struct Entry {
    IDirect3DTexture9* src = nullptr;  // game texture (AddRef'd)
    uint64_t hash = 0;                 // hash of the game's CURRENT map: 0 = not read yet (new texture), 1 = unreadable
    RawPtr raw;                        // last map read (64 KB DXT5)
    uint64_t rawHash = 0;              // hash of raw
    IDirect3DTexture9* smooth = nullptr; // D3DPOOL_DEFAULT
    uint64_t doneHash = 0;             // map the smoothed texture was built from (shown only while == hash)
    uint64_t doneSig = 0;              // inputs (this map and its 8 neighbours) of the smoothed texture
    uint64_t atlasHash = 0;            // map the atlas cell holds (0 = nothing)
    bool atlasSmooth = false;          // the atlas cell holds the smoothed map (else a plain 2x copy)
    bool dirty = false;                // own map changed: needs smoothing
    bool borderDirty = false;          // a neighbour changed: its 6 border texels are stale
    bool inFlight = false;
    bool awaiting = false;             // a rebuild is sweeping and this chunk was not re-rendered yet
    int gen = 0;
    uint32_t lastUse = 0; // frame of the last draw that asked for it (visible chunks are processed first)
    bool byUseMoved = false; // listed in g_byUseMoved: lastUse changed (or new) since g_byUse was ordered

    // ---- GPU path (see "GPU path" below); the CPU fields above stay unused while it is active ----
    IDirect3DTexture9* gtex = nullptr; // 1024x1024 A8R8G8B8, 11 levels, D3DUSAGE_RENDERTARGET, D3DPOOL_DEFAULT
    uint64_t gver = 1;                 // version of the game's map: +1 at every change seen (notice, new texture, hash)
    uint64_t gBuiltVer = 0;            // map version gtex was built from (shown only while == gver)
    uint64_t gBuiltSig = 0;            // inputs of gtex (this map's and the 8 neighbours' versions, GpuSig)
    uint64_t gAtlasSig = 0;            // build the atlas cell holds (0 = none)
    uint64_t ghash = 0;                // hash of the map at the last build / check (0 = unknown): fallback detection
    bool gNoHash = false;              // the map cannot be locked (not a managed DXT5): changes only by notice / pointer
    uint32_t gTryFrame = UINT32_MAX;   // frame of the last build attempt
};

struct SharedSrc { // what the worker reads when it starts a job (guarded by g_mx)
    RawPtr raw;
    uint64_t hash = 0;
};

struct Job {
    LightmapSmooth::Key key;
    int gen;
};

struct Result {
    LightmapSmooth::Key key;
    int gen = 0;
    uint64_t centreHash = 0;
    uint64_t sig = 0;
    float ms = 0.0f;
    std::vector<std::vector<uint32_t>> levels;
};

std::atomic<bool> g_enabled{true};
std::atomic<bool> g_failed{false}; // out of memory once: off for the rest of the session
std::map<LightmapSmooth::Key, Entry> g_entries; // render thread only
size_t g_checkCursor = 0;
size_t g_boostCursor = 0;
int g_inFlight = 0;
uint32_t g_frame = 0; // OnPresent counter, for Entry::lastUse
int g_genCounter = 0;

// ---- chunks in order of use (2026-09-29, render thread) ----
// The GPU service (first call of a frame, and again when a change was seen), the GPU Present step and the CPU Present
// step each walk the chunks most recently drawn first. They used to build a list of g_entries and stable_sort it by
// lastUse every time. That order is exactly the order by (lastUse descending, key ascending): the input was in key order
// and the keys are unique. It is kept here and brought up to date only for the chunks whose lastUse changed since (a few
// per frame: Get marks them), by taking those out, sorting them and merging them back; the result equals a full sort.
using EntryNode = std::pair<const LightmapSmooth::Key, Entry>;
std::vector<EntryNode*> g_byUse;      // g_entries by (lastUse desc, key asc) as of the last ByUse()
bool g_byUseValid = false;            // false: rebuild from g_entries (start, and after Clear)
std::vector<EntryNode*> g_byUseMoved; // entries marked byUseMoved (new, or lastUse changed) since g_byUse was ordered

bool ByUseLess(const EntryNode* a, const EntryNode* b) {
    return a->second.lastUse != b->second.lastUse ? a->second.lastUse > b->second.lastUse : a->first < b->first;
}

void NoteUseChanged(EntryNode* n) {
    if (n->second.byUseMoved) return;
    n->second.byUseMoved = true;
    g_byUseMoved.push_back(n);
}

const std::vector<EntryNode*>& ByUse() {
    if (!g_byUseValid) {
        for (EntryNode* n : g_byUseMoved) n->second.byUseMoved = false;
        g_byUseMoved.clear();
        g_byUse.clear();
        g_byUse.reserve(g_entries.size());
        for (auto& kv : g_entries) g_byUse.push_back(&kv);
        std::sort(g_byUse.begin(), g_byUse.end(), ByUseLess);
        g_byUseValid = true;
        return g_byUse;
    }
    if (g_byUseMoved.empty()) return g_byUse;
    static std::vector<EntryNode*> rest;
    rest.clear();
    for (EntryNode* n : g_byUse) // the unchanged ones, still in order (their lastUse and key are the same)
        if (!n->second.byUseMoved) rest.push_back(n);
    std::sort(g_byUseMoved.begin(), g_byUseMoved.end(), ByUseLess);
    g_byUse.clear();
    std::merge(rest.begin(), rest.end(), g_byUseMoved.begin(), g_byUseMoved.end(), std::back_inserter(g_byUse), ByUseLess);
    for (EntryNode* n : g_byUseMoved) n->second.byUseMoved = false;
    g_byUseMoved.clear();
    return g_byUse;
}

// After g_entries lost elements: the lists hold dangling pointers (their flags are not touched)
void ForgetByUse() {
    g_byUse.clear();
    g_byUseMoved.clear();
    g_byUseValid = false;
}

// timing / sweep state (render thread)
uint32_t g_expectUntil = 0;   // no new jobs before this frame (a rebuild is imminent)
uint32_t g_boostUntil = 0;    // 4 visible chunks checked per frame before this frame
uint32_t g_sweepUntil = 0;
bool g_sweepOn = false;
uint32_t g_lastChangeFrame = 0;
bool g_haveKick = false, g_haveConsume = false;
Clock::time_point g_kickTime{}, g_consumeTime{};
std::string g_kickReason;
int g_sweepChanges = 0;
double g_sweepFirstMs = -1.0, g_sweepLastMs = -1.0, g_kickToConsumeMs = -1.0;
double g_lastChangeSinceKickMs = -1.0;
std::string g_lastSweep = "none";

// counters (render thread; the job timings are written under g_mx)
int g_uploaded = 0;
int g_deferredUploads = 0;
int g_staleResults = 0;
int g_skippedSame = 0;
int g_rawCopies = 0;
int g_changes = 0;
int g_notices = 0;
int g_unreadable = 0; // game maps that could not be read (not managed 256x256 DXT5)
int g_jobsDone = 0;
double g_jobMsSum = 0.0;
float g_jobMsMax = 0.0f;

std::mutex g_mx;
std::condition_variable g_cv;
std::deque<Job> g_jobs;
std::vector<Result> g_results;
std::map<LightmapSmooth::Key, SharedSrc> g_shared;
bool g_workerStarted = false; // detached worker (a joinable std::thread would terminate the game at exit)
bool g_stop = false;

// chunk re-render notices from the game's terrain update (same render thread; locked anyway, it is cheap)
std::mutex g_noticeMx;
std::vector<LightmapSmooth::Key> g_noticeKeys;
std::atomic<bool> g_noticePending{false};           // GPU path: notices waiting (checked on every draw without the lock)
std::vector<LightmapSmooth::Key> g_drainedThisFrame; // GPU path: notices already taken this frame (OnTerrainRebuilt)

double MsSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

// ---- DXT5 decode ----
void DecodeDxt5(const uint8_t* data, int pitch, Plane& out) {
    out.assign(static_cast<size_t>(kSrc) * kSrc * 4, 0.0f);
    for (int by = 0; by < kSrc / 4; by++) {
        const uint8_t* row = data + by * pitch;
        for (int bx = 0; bx < kSrc / 4; bx++) {
            const uint8_t* b = row + bx * 16;
            float a[8];
            a[0] = b[0] / 255.0f;
            a[1] = b[1] / 255.0f;
            if (b[0] > b[1]) {
                for (int i = 1; i < 7; i++) a[i + 1] = ((7 - i) * a[0] + i * a[1]) / 7.0f;
            } else {
                for (int i = 1; i < 5; i++) a[i + 1] = ((5 - i) * a[0] + i * a[1]) / 5.0f;
                a[6] = 0.0f;
                a[7] = 1.0f;
            }
            uint64_t aBits = 0;
            for (int i = 0; i < 6; i++) aBits |= static_cast<uint64_t>(b[2 + i]) << (8 * i);
            const uint16_t c0 = static_cast<uint16_t>(b[8] | (b[9] << 8));
            const uint16_t c1 = static_cast<uint16_t>(b[10] | (b[11] << 8));
            float col[4][3];
            auto unpack = [](uint16_t c, float* o) {
                o[0] = ((c >> 11) & 31) / 31.0f;
                o[1] = ((c >> 5) & 63) / 63.0f;
                o[2] = (c & 31) / 31.0f;
            };
            unpack(c0, col[0]);
            unpack(c1, col[1]);
            for (int k = 0; k < 3; k++) {
                col[2][k] = (2 * col[0][k] + col[1][k]) / 3.0f;
                col[3][k] = (col[0][k] + 2 * col[1][k]) / 3.0f;
            }
            const uint32_t cBits = b[12] | (b[13] << 8) | (b[14] << 16) | (static_cast<uint32_t>(b[15]) << 24);
            for (int py = 0; py < 4; py++)
                for (int px = 0; px < 4; px++) {
                    const int i = py * 4 + px;
                    float* o = &out[((static_cast<size_t>(by) * 4 + py) * kSrc + bx * 4 + px) * 4];
                    const int ci = (cBits >> (2 * i)) & 3;
                    o[0] = col[ci][0];
                    o[1] = col[ci][1];
                    o[2] = col[ci][2];
                    o[3] = a[(aBits >> (3 * i)) & 7];
                }
        }
    }
}

// Integer DXT5 decode to A8R8G8B8 (render thread, for the plain atlas copy): 256x256 texels.
void DecodeDxt5Argb(const uint8_t* data, uint32_t* out) {
    for (int by = 0; by < kSrc / 4; by++) {
        const uint8_t* row = data + by * kPitch;
        for (int bx = 0; bx < kSrc / 4; bx++) {
            const uint8_t* b = row + bx * 16;
            uint32_t a[8];
            a[0] = b[0];
            a[1] = b[1];
            if (a[0] > a[1]) {
                for (uint32_t i = 1; i < 7; i++) a[i + 1] = ((7 - i) * a[0] + i * a[1] + 3) / 7;
            } else {
                for (uint32_t i = 1; i < 5; i++) a[i + 1] = ((5 - i) * a[0] + i * a[1] + 2) / 5;
                a[6] = 0;
                a[7] = 255;
            }
            uint64_t aBits = 0;
            for (int i = 0; i < 6; i++) aBits |= static_cast<uint64_t>(b[2 + i]) << (8 * i);
            const uint32_t c0 = b[8] | (b[9] << 8), c1 = b[10] | (b[11] << 8);
            uint32_t col[4][3];
            auto unpack = [](uint32_t c, uint32_t* o) {
                const uint32_t r = (c >> 11) & 31, g = (c >> 5) & 63, bl = c & 31;
                o[0] = (r << 3) | (r >> 2);
                o[1] = (g << 2) | (g >> 4);
                o[2] = (bl << 3) | (bl >> 2);
            };
            unpack(c0, col[0]);
            unpack(c1, col[1]);
            for (int k = 0; k < 3; k++) {
                col[2][k] = (2 * col[0][k] + col[1][k] + 1) / 3;
                col[3][k] = (col[0][k] + 2 * col[1][k] + 1) / 3;
            }
            const uint32_t cBits = b[12] | (b[13] << 8) | (b[14] << 16) | (static_cast<uint32_t>(b[15]) << 24);
            for (int py = 0; py < 4; py++)
                for (int px = 0; px < 4; px++) {
                    const int i = py * 4 + px;
                    const int ci = (cBits >> (2 * i)) & 3;
                    out[(by * 4 + py) * kSrc + bx * 4 + px] =
                        (a[(aBits >> (3 * i)) & 7] << 24) | (col[ci][0] << 16) | (col[ci][1] << 8) | col[ci][2];
                }
        }
    }
}

// Hash of level 0 in 64-bit words (FNV-style multiply, high half folded down). Never 0 (not read) nor 1 (unreadable).
uint64_t HashRows(const uint8_t* base, int pitch) {
    uint64_t h = 1469598103934665603ull;
    for (int r = 0; r < kSrc / 4; r++) {
        const uint8_t* row = base + static_cast<size_t>(r) * pitch;
        for (int i = 0; i < kPitch; i += 8) {
            uint64_t w;
            std::memcpy(&w, row + i, 8);
            h = (h ^ w) * 1099511628211ull;
            h ^= h >> 29;
        }
    }
    h |= 2; // bit 1 set: never 0 or 1
    return h;
}

// Inputs of one smoothing job: the 3x3 map hashes (0 = no map) combined.
uint64_t SigOf(const uint64_t h[9]) {
    uint64_t s = 0x9E3779B97F4A7C15ull;
    for (int i = 0; i < 9; i++) {
        s ^= h[i] + 0x9E3779B97F4A7C15ull + (s << 6) + (s >> 2);
        s *= 0xFF51AFD7ED558CCDull;
    }
    return s | 1;
}

// Reads level 0 of a game light map: -1 not a lockable 256x256 DXT5, 0 same as `known`, 1 changed (raw filled).
int ReadMap(IDirect3DTexture9* tex, uint64_t known, uint64_t& hash, RawPtr& raw) {
    D3DSURFACE_DESC d{};
    if (FAILED(tex->GetLevelDesc(0, &d)) || d.Width != kSrc || d.Height != kSrc || d.Format != D3DFMT_DXT5 || d.Pool == D3DPOOL_DEFAULT) return -1;
    D3DLOCKED_RECT lr{};
    if (FAILED(tex->LockRect(0, &lr, nullptr, D3DLOCK_READONLY))) return -1;
    const auto* bits = static_cast<const uint8_t*>(lr.pBits);
    hash = HashRows(bits, lr.Pitch);
    int result = 0;
    if (hash != known) {
        try {
            auto copy = std::make_shared<Raw>(static_cast<size_t>(kPitch) * (kSrc / 4));
            for (int r = 0; r < kSrc / 4; r++) std::memcpy(copy->data() + r * kPitch, bits + static_cast<size_t>(r) * lr.Pitch, kPitch);
            raw = std::move(copy);
            result = 1;
        } catch (...) {
            tex->UnlockRect(0);
            throw;
        }
    }
    tex->UnlockRect(0);
    return result;
}

// ---- smoothing (worker thread) ----
void BSplineWeights(float t, float w[4]) {
    const float t2 = t * t, t3 = t2 * t, s = 1 - t;
    w[0] = s * s * s / 6.0f;
    w[1] = (3 * t3 - 6 * t2 + 4) / 6.0f;
    w[2] = (-3 * t3 + 3 * t2 + 3 * t + 1) / 6.0f;
    w[3] = t3 / 6.0f;
}

// raw[9]: [dz+1][dx+1], centre = 4 (never null)
void Process(const RawPtr raw[9], Result& r) {
    // 1. Extended source (centre chunk + borders from the neighbours, clamped where a neighbour is missing).
    //    Chunks are decoded one at a time (only 64 KB of DXT5 is kept per chunk).
    std::vector<float> ext(static_cast<size_t>(kExt) * kExt * 4);
    Plane plane;
    for (int slot = 0; slot < 9; slot++) {
        const int sdx = slot % 3 - 1, sdz = slot / 3 - 1;
        if (!raw[slot]) continue;
        DecodeDxt5(raw[slot]->data(), kPitch, plane);
        for (int y = 0; y < kExt; y++)
            for (int x = 0; x < kExt; x++) {
                int sx = x - kBorder, sy = y - kBorder;
                const int dx = sx < 0 ? -1 : (sx >= kSrc ? 1 : 0);
                const int dz = sy < 0 ? -1 : (sy >= kSrc ? 1 : 0);
                const bool neighbour = raw[(dz + 1) * 3 + (dx + 1)] != nullptr;
                if (neighbour ? (dx != sdx || dz != sdz) : slot != 4) continue; // this texel comes from another slot
                if (neighbour) {
                    sx -= dx * kSrc;
                    sy -= dz * kSrc;
                } else { // missing neighbour: clamp to the centre chunk
                    sx = std::clamp(sx, 0, kSrc - 1);
                    sy = std::clamp(sy, 0, kSrc - 1);
                }
                std::memcpy(&ext[(static_cast<size_t>(y) * kExt + x) * 4], &plane[(static_cast<size_t>(sy) * kSrc + sx) * 4], 16);
            }
    }
    Plane().swap(plane);

    // 2. Brightness (luma) and alpha planes; colour ratio from a blurred copy (removes the RGB565 tint noise).
    const size_t n = static_cast<size_t>(kExt) * kExt;
    std::vector<float> Y(n), A(n), rgbB(n * 3), yB(n), tmp(n * 4);
    for (size_t i = 0; i < n; i++) {
        const float* c = &ext[i * 4];
        Y[i] = 0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2];
        A[i] = c[3];
    }
    const float g[7] = {0.0366f, 0.1112f, 0.2167f, 0.2710f, 0.2167f, 0.1112f, 0.0366f}; // sigma 1.5
    auto blur = [&](auto get, auto set) {
        for (int y = 0; y < kExt; y++)
            for (int x = 0; x < kExt; x++) {
                float s[4] = {};
                for (int k = -3; k <= 3; k++) {
                    const int xx = std::clamp(x + k, 0, kExt - 1);
                    float v[4];
                    get(static_cast<size_t>(y) * kExt + xx, v);
                    for (int c = 0; c < 4; c++) s[c] += g[k + 3] * v[c];
                }
                std::memcpy(&tmp[(static_cast<size_t>(y) * kExt + x) * 4], s, 16);
            }
        for (int y = 0; y < kExt; y++)
            for (int x = 0; x < kExt; x++) {
                float s[4] = {};
                for (int k = -3; k <= 3; k++) {
                    const int yy = std::clamp(y + k, 0, kExt - 1);
                    for (int c = 0; c < 4; c++) s[c] += g[k + 3] * tmp[(static_cast<size_t>(yy) * kExt + x) * 4 + c];
                }
                set(static_cast<size_t>(y) * kExt + x, s);
            }
    };
    blur([&](size_t i, float* v) { v[0] = ext[i * 4]; v[1] = ext[i * 4 + 1]; v[2] = ext[i * 4 + 2]; v[3] = Y[i]; },
         [&](size_t i, const float* s) { rgbB[i * 3] = s[0]; rgbB[i * 3 + 1] = s[1]; rgbB[i * 3 + 2] = s[2]; yB[i] = s[3]; });
    std::vector<float> chroma(n * 3);
    for (size_t i = 0; i < n; i++) {
        const float yb = yB[i];
        const float w = std::clamp(yb / 0.03f, 0.0f, 1.0f); // very dim: neutral (the noise is all there is)
        for (int c = 0; c < 3; c++) {
            const float ratio = yb > 1e-4f ? std::clamp(rgbB[i * 3 + c] / yb, 0.0f, 4.0f) : 1.0f;
            chroma[i * 3 + c] = 1.0f + (ratio - 1.0f) * w;
        }
    }

    // 3. 4x enlargement: cubic B-spline for brightness and alpha, bilinear for the (already smooth) colour ratio.
    //    Output texel j sits at source coordinate (j + 0.5) / 4 - 0.5.
    struct Tap { int i0; float w[4]; int l0; float lf; };
    std::vector<Tap> taps(kOut);
    for (int j = 0; j < kOut; j++) {
        const float s = (j + 0.5f) / kScale - 0.5f + kBorder;
        const float f = std::floor(s);
        taps[j].i0 = static_cast<int>(f) - 1;
        BSplineWeights(s - f, taps[j].w);
        taps[j].l0 = static_cast<int>(f);
        taps[j].lf = s - f;
    }
    // horizontal pass: kExt rows x kOut columns (Y, A, chroma rgb)
    std::vector<float> h(static_cast<size_t>(kExt) * kOut * 5);
    for (int y = 0; y < kExt; y++)
        for (int x = 0; x < kOut; x++) {
            const Tap& t = taps[x];
            float yy = 0, aa = 0;
            for (int k = 0; k < 4; k++) {
                const size_t i = static_cast<size_t>(y) * kExt + (t.i0 + k);
                yy += t.w[k] * Y[i];
                aa += t.w[k] * A[i];
            }
            float* o = &h[(static_cast<size_t>(y) * kOut + x) * 5];
            o[0] = yy;
            o[1] = aa;
            const size_t i0 = static_cast<size_t>(y) * kExt + t.l0;
            for (int c = 0; c < 3; c++) o[2 + c] = chroma[i0 * 3 + c] * (1 - t.lf) + chroma[(i0 + 1) * 3 + c] * t.lf;
        }
    static const float bayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};
    r.levels.clear();
    r.levels.emplace_back(static_cast<size_t>(kOut) * kOut);
    auto& l0 = r.levels[0];
    for (int y = 0; y < kOut; y++) {
        const Tap& t = taps[y];
        for (int x = 0; x < kOut; x++) {
            float yy = 0, aa = 0;
            for (int k = 0; k < 4; k++) {
                const float* p = &h[(static_cast<size_t>(t.i0 + k) * kOut + x) * 5];
                yy += t.w[k] * p[0];
                aa += t.w[k] * p[1];
            }
            const float* c0 = &h[(static_cast<size_t>(t.l0) * kOut + x) * 5];
            const float* c1 = &h[(static_cast<size_t>(t.l0 + 1) * kOut + x) * 5];
            const float d = (bayer[y & 3][x & 3] + 0.5f) / 16.0f - 0.5f;
            uint32_t px = 0;
            for (int c = 0; c < 3; c++) {
                const float ch = c0[2 + c] * (1 - t.lf) + c1[2 + c] * t.lf;
                const float v = std::clamp(yy * ch * 255.0f + d, 0.0f, 255.0f);
                px |= static_cast<uint32_t>(v + 0.5f) << (16 - 8 * c); // A8R8G8B8: R at bit 16, G 8, B 0
            }
            px |= static_cast<uint32_t>(std::clamp(aa * 255.0f + d, 0.0f, 255.0f) + 0.5f) << 24;
            l0[static_cast<size_t>(y) * kOut + x] = px;
        }
    }
    // 4. Mip chain (2x2 box).
    for (int size = kOut / 2; size >= 1; size /= 2) {
        const auto& prev = r.levels.back();
        std::vector<uint32_t> lv(static_cast<size_t>(size) * size);
        const int ps = size * 2;
        for (int y = 0; y < size; y++)
            for (int x = 0; x < size; x++) {
                uint32_t s[4] = {};
                const uint32_t q[4] = {prev[(2 * y) * ps + 2 * x], prev[(2 * y) * ps + 2 * x + 1], prev[(2 * y + 1) * ps + 2 * x], prev[(2 * y + 1) * ps + 2 * x + 1]};
                for (uint32_t v : q)
                    for (int c = 0; c < 4; c++) s[c] += (v >> (8 * c)) & 255;
                uint32_t px = 0;
                for (int c = 0; c < 4; c++) px |= ((s[c] + 2) / 4) << (8 * c);
                lv[static_cast<size_t>(y) * size + x] = px;
            }
        r.levels.push_back(std::move(lv));
    }
}

// The worker takes only a key: it reads the LATEST maps of the chunk and its neighbours when it starts the job, so a job
// queued before a neighbour changed still uses the neighbour's new map.
void WorkerMain() {
    for (;;) {
        Job job;
        RawPtr raw[9];
        uint64_t hs[9] = {};
        {
            std::unique_lock<std::mutex> lk(g_mx);
            // at most 2 finished maps waiting (5.6 MB each): the render thread uploads one per frame
            g_cv.wait(lk, [] { return g_stop || (!g_jobs.empty() && g_results.size() < 2); });
            if (g_stop) return;
            job = g_jobs.front();
            g_jobs.pop_front();
            for (int dz = -1; dz <= 1; dz++)
                for (int dx = -1; dx <= 1; dx++) {
                    auto it = g_shared.find({job.key.first + dx * kChunkSize, job.key.second + dz * kChunkSize});
                    if (it == g_shared.end() || !it->second.raw) continue;
                    raw[(dz + 1) * 3 + (dx + 1)] = it->second.raw;
                    hs[(dz + 1) * 3 + (dx + 1)] = it->second.hash;
                }
        }
        Result r;
        r.key = job.key;
        r.gen = job.gen;
        r.centreHash = hs[4];
        r.sig = SigOf(hs);
        const auto t0 = Clock::now();
        if (raw[4]) {
            try {
                Process(raw, r);
            } catch (...) { // out of memory in a 32-bit game: give up on this map, never take the game down
                r.levels.clear();
            }
        }
        r.ms = static_cast<float>(MsSince(t0));
        for (auto& p : raw) p.reset();
        std::lock_guard<std::mutex> lk(g_mx);
        if (!r.levels.empty()) {
            g_jobsDone++;
            g_jobMsSum += r.ms;
            g_jobMsMax = std::max(g_jobMsMax, r.ms);
        }
        g_results.push_back(std::move(r));
    }
}

void EnsureWorker() {
    if (g_workerStarted) return;
    try {
        std::thread(WorkerMain).detach();
        g_workerStarted = true;
    } catch (...) {
    }
}

LightmapSmooth::Key Neighbour(const LightmapSmooth::Key& k, int dx, int dz) { return {k.first + dx * kChunkSize, k.second + dz * kChunkSize}; }

void MarkBordersAround(const LightmapSmooth::Key& k) {
    for (int dz = -1; dz <= 1; dz++)
        for (int dx = -1; dx <= 1; dx++) {
            if (!dx && !dz) continue;
            auto it = g_entries.find(Neighbour(k, dx, dz));
            if (it != g_entries.end()) it->second.borderDirty = true;
        }
}

uint64_t CurrentSig(const LightmapSmooth::Key& k) {
    uint64_t hs[9] = {};
    for (int dz = -1; dz <= 1; dz++)
        for (int dx = -1; dx <= 1; dx++) {
            auto it = g_entries.find(Neighbour(k, dx, dz));
            if (it != g_entries.end() && it->second.raw) hs[(dz + 1) * 3 + (dx + 1)] = it->second.rawHash;
        }
    return SigOf(hs);
}

// ---- smoothed-texture pool (D3DPOOL_DEFAULT): each entry updates its own texture in place; textures dropped by an
// unreadable map are kept (up to 4) for the next new chunk instead of creating one per upload.
std::vector<IDirect3DTexture9*> g_texPool;
IDirect3DTexture9* g_upStaging = nullptr; // SYSTEMMEM 1024x1024, all levels: persistent (was one per upload)

void PoolPut(IDirect3DTexture9* t) {
    if (!t) return;
    if (g_texPool.size() < 4) g_texPool.push_back(t);
    else t->Release();
}

void ReleasePool() {
    for (auto* t : g_texPool) t->Release();
    g_texPool.clear();
}

void NoteChange(const LightmapSmooth::Key& key, const Entry& e, const char* via) {
    g_changes++;
    g_lastChangeFrame = g_frame;
    if (g_haveKick) g_lastChangeSinceKickMs = MsSince(g_kickTime);
    if (g_sweepOn) {
        const double ms = g_haveConsume ? MsSince(g_consumeTime) : -1.0;
        if (g_sweepChanges++ == 0) g_sweepFirstMs = ms;
        g_sweepLastMs = ms;
    }
    if (!kPublicBuild) {
        if (e.lastUse + 2 >= g_frame) // chunks in view only (a full rebuild re-renders every chunk of the world)
            LOG_DEBUG(std::format("[LightmapSmooth] Chunk ({}, {}) changed ({}), {:.0f} ms after the last kick", key.first, key.second, via,
                                  g_lastChangeSinceKickMs));
    }
}

void MarkUnreadable(const LightmapSmooth::Key& k, Entry& e) {
    if (e.hash == 1) return;
    // Not readable (not a managed 256x256 DXT5): never smoothed. A smoothed map of an OLDER source must not stay
    // (review 25/09, winter seam m73/m74): Get/Find already refuse it (hash != doneHash); its texture goes to the pool.
    D3DSURFACE_DESC d{};
    if (e.src) e.src->GetLevelDesc(0, &d);
    LOG_INFO(std::format("[LightmapSmooth] Map ({}, {}) unreadable: {}x{} format {} pool {} levels {}{}", k.first, k.second, d.Width, d.Height,
                         static_cast<unsigned>(d.Format), static_cast<unsigned>(d.Pool), e.src ? e.src->GetLevelCount() : 0,
                         e.smooth ? " (old smoothed map dropped)" : ""));
    PoolPut(e.smooth);
    e.smooth = nullptr;
    e.doneHash = e.doneSig = 0;
    e.hash = 1;
    e.raw.reset();
    e.rawHash = 0;
    e.dirty = false;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        g_shared.erase(k);
    }
    MarkBordersAround(k);
    g_unreadable++;
}

// Reads one game map; on a change keeps the new map, marks it (and its neighbours' borders) for smoothing. Returns
// false when it could not be read.
bool CheckEntry(const LightmapSmooth::Key& key, Entry& e, const char* via) {
    if (!e.src) return false;
    uint64_t h = 0;
    RawPtr raw;
    const int r = ReadMap(e.src, e.hash, h, raw);
    if (r < 0) {
        MarkUnreadable(key, e);
        return false;
    }
    if (r == 0) return true;
    e.hash = h;
    e.awaiting = false; // (re-)rendered
    if (h == e.rawHash) { // a new texture with the map we already have: the smoothed one is current again if it was built from it
        if (e.doneHash != h) e.dirty = true;
        return true;
    }
    e.raw = std::move(raw);
    e.rawHash = h;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        g_shared[key] = SharedSrc{e.raw, h};
    }
    MarkBordersAround(key);
    e.dirty = true;     // cleared without a job when the inputs equal those of the smoothed map (see the queue)
    NoteChange(key, e, via);
    return true;
}

// ---- World light atlas: every chunk map at 2 texels per metre in one texture covering all chunks seen, so surfaces
// that only know their world position (snowy floor tiles) can read the terrain light anywhere, across chunk borders. A
// render-target texture in video memory; chunks are copied in with UpdateSurface from small system-memory staging
// textures (a ring of 3, so a copy never waits for the GPU to finish the previous one). A chunk holds a plain 2x copy of
// the game's current map until its smoothed map (mip 1) replaces it. When the world grows the atlas is recreated larger
// and the old contents are copied over on the GPU (StretchRect, no scaling): cells never go black.
constexpr int kAtlasPerChunk = kOut / 2; // 512 texels per 256 m chunk
constexpr int kAtlasMaxChunks = 16;      // 8192 texels
constexpr int kAtlasStages = 3;
IDirect3DTexture9* g_atlas = nullptr;
IDirect3DTexture9* g_atlasStage[kAtlasStages] = {};
int g_atlasStageNext = 0;
int g_atlasMinX = 0, g_atlasMinZ = 0, g_atlasW = 0, g_atlasH = 0;
int g_atlasChunks = 0;
int g_atlasGrowths = 0;
std::vector<uint32_t> g_decodeBuf; // 256x256 (render thread)

int ChunkIndex(int key) { return (key - kChunkSize / 2) / kChunkSize; } // key = chunk centre = 256 * i + 128

void ReleaseAtlas() {
    if (g_atlas) g_atlas->Release();
    g_atlas = nullptr;
    g_atlasW = g_atlasH = 0;
    g_atlasChunks = 0;
    for (auto& [k, e] : g_entries) {
        e.atlasHash = 0;
        e.atlasSmooth = false;
        e.gAtlasSig = 0;
    }
}

void ReleaseStaging() {
    for (auto*& s : g_atlasStage) {
        if (s) s->Release();
        s = nullptr;
    }
    if (g_upStaging) g_upStaging->Release();
    g_upStaging = nullptr;
}

// Locks a free atlas staging texture. wait = false: nullptr when every one is still being copied by the GPU.
IDirect3DTexture9* LockAtlasStage(IDirect3DDevice9* dev, D3DLOCKED_RECT& lr, bool wait) {
    for (int n = 0; n < kAtlasStages; n++) {
        const int i = (g_atlasStageNext + n) % kAtlasStages;
        IDirect3DTexture9*& s = g_atlasStage[i];
        if (!s && FAILED(dev->CreateTexture(kAtlasPerChunk, kAtlasPerChunk, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &s, nullptr))) {
            s = nullptr;
            continue;
        }
        HRESULT hr = s->LockRect(0, &lr, nullptr, D3DLOCK_DONOTWAIT);
        if (hr == D3DERR_WASSTILLDRAWING) continue;
        if (FAILED(hr)) hr = s->LockRect(0, &lr, nullptr, 0); // flag not supported on textures: plain lock
        if (FAILED(hr)) continue;
        g_atlasStageNext = (i + 1) % kAtlasStages;
        return s;
    }
    if (!wait) return nullptr;
    IDirect3DTexture9* s = g_atlasStage[g_atlasStageNext];
    if (!s || FAILED(s->LockRect(0, &lr, nullptr, 0))) return nullptr;
    g_atlasStageNext = (g_atlasStageNext + 1) % kAtlasStages;
    return s;
}

bool AtlasCell(const LightmapSmooth::Key& key, POINT& pt) {
    if (!g_atlas) return false;
    const int ix = ChunkIndex(key.first) - g_atlasMinX, iz = ChunkIndex(key.second) - g_atlasMinZ;
    if (ix < 0 || iz < 0 || ix >= g_atlasW || iz >= g_atlasH) return false;
    pt = POINT{ix * kAtlasPerChunk, iz * kAtlasPerChunk};
    return true;
}

bool AtlasCommit(IDirect3DDevice9* dev, IDirect3DTexture9* stage, const POINT& pt) {
    IDirect3DSurface9 *src = nullptr, *dst = nullptr;
    bool ok = false;
    if (SUCCEEDED(stage->GetSurfaceLevel(0, &src)) && SUCCEEDED(g_atlas->GetSurfaceLevel(0, &dst))) ok = SUCCEEDED(dev->UpdateSurface(src, nullptr, dst, &pt));
    if (src) src->Release();
    if (dst) dst->Release();
    if (ok) g_atlasChunks++;
    return ok;
}

// Smoothed map (its mip 1, 512x512) into the chunk's atlas cell.
bool AtlasUpload(IDirect3DDevice9* dev, const LightmapSmooth::Key& key, const std::vector<uint32_t>& level) {
    POINT pt{};
    if (!AtlasCell(key, pt) || level.size() != static_cast<size_t>(kAtlasPerChunk) * kAtlasPerChunk) return false;
    D3DLOCKED_RECT lr{};
    IDirect3DTexture9* stage = LockAtlasStage(dev, lr, true);
    if (!stage) return false;
    for (int y = 0; y < kAtlasPerChunk; y++)
        std::memcpy(static_cast<uint8_t*>(lr.pBits) + y * lr.Pitch, &level[static_cast<size_t>(y) * kAtlasPerChunk], kAtlasPerChunk * 4);
    stage->UnlockRect(0);
    return AtlasCommit(dev, stage, pt);
}

// Two channels per word (R and B, or A and G) weighted in 16-bit lanes; the 4 weights add up to 16.
inline uint32_t Blend4(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t wa, uint32_t wb, uint32_t wc, uint32_t wd) {
    constexpr uint32_t m = 0x00FF00FF;
    const uint32_t rb = (((a & m) * wa + (b & m) * wb + (c & m) * wc + (d & m) * wd + 0x00080008) >> 4) & m;
    const uint32_t ag = ((((a >> 8) & m) * wa + ((b >> 8) & m) * wb + ((c >> 8) & m) * wc + ((d >> 8) & m) * wd + 0x00080008) >> 4) & m;
    return rb | (ag << 8);
}

// Plain copy of the game's current map into the chunk's atlas cell: bilinear 2x at the same sample positions as the
// smoothed map's mip 1 (texel m at source coordinate m/2 - 0.25), so the light level matches the smoothed map.
// Returns -1 when the chunk has no cell, 0 when no staging texture is free (tried again next frame), 1 when copied.
int AtlasRawCopy(IDirect3DDevice9* dev, const LightmapSmooth::Key& key, const Entry& e) {
    POINT pt{};
    if (!e.raw || !AtlasCell(key, pt)) return -1;
    D3DLOCKED_RECT lr{};
    IDirect3DTexture9* stage = LockAtlasStage(dev, lr, false);
    if (!stage) return 0;
    g_decodeBuf.resize(static_cast<size_t>(kSrc) * kSrc);
    DecodeDxt5Argb(e.raw->data(), g_decodeBuf.data());
    const uint32_t* s = g_decodeBuf.data();
    for (int y = 0; y < kAtlasPerChunk; y++) {
        const int k = y >> 1;
        const int y0 = (y & 1) ? k : std::max(k - 1, 0), y1 = (y & 1) ? std::min(k + 1, kSrc - 1) : k;
        const uint32_t wy0 = (y & 1) ? 3 : 1, wy1 = (y & 1) ? 1 : 3;
        const uint32_t* r0 = s + y0 * kSrc;
        const uint32_t* r1 = s + y1 * kSrc;
        auto* o = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(lr.pBits) + y * lr.Pitch);
        for (int x = 0; x < kAtlasPerChunk; x++) {
            const int j = x >> 1;
            const int x0 = (x & 1) ? j : std::max(j - 1, 0), x1 = (x & 1) ? std::min(j + 1, kSrc - 1) : j;
            const uint32_t wx0 = (x & 1) ? 3 : 1, wx1 = (x & 1) ? 1 : 3;
            o[x] = Blend4(r0[x0], r0[x1], r1[x0], r1[x1], wy0 * wx0, wy0 * wx1, wy1 * wx0, wy1 * wx1);
        }
    }
    stage->UnlockRect(0);
    return AtlasCommit(dev, stage, pt) ? 1 : -1;
}

void EnsureAtlas(IDirect3DDevice9* dev) {
    if (g_entries.empty()) return;
    int minX = INT_MAX, minZ = INT_MAX, maxX = INT_MIN, maxZ = INT_MIN;
    for (auto& [k, e] : g_entries) {
        minX = std::min(minX, ChunkIndex(k.first));
        maxX = std::max(maxX, ChunkIndex(k.first));
        minZ = std::min(minZ, ChunkIndex(k.second));
        maxZ = std::max(maxZ, ChunkIndex(k.second));
    }
    if (g_atlas && minX >= g_atlasMinX && minZ >= g_atlasMinZ && maxX < g_atlasMinX + g_atlasW && maxZ < g_atlasMinZ + g_atlasH) return;
    // grow to cover everything, with a chunk of margin so camera moves do not rebuild it every time
    if (g_atlas) {
        minX = std::min(minX, g_atlasMinX);
        minZ = std::min(minZ, g_atlasMinZ);
        maxX = std::max(maxX, g_atlasMinX + g_atlasW - 1);
        maxZ = std::max(maxZ, g_atlasMinZ + g_atlasH - 1);
    }
    minX--;
    minZ--;
    maxX++;
    maxZ++;
    const int w = maxX - minX + 1, h = maxZ - minZ + 1;
    if (w > kAtlasMaxChunks || h > kAtlasMaxChunks) { // unusually large world: no atlas
        ReleaseAtlas();
        return;
    }
    IDirect3DTexture9* atlas = nullptr;
    if (FAILED(dev->CreateTexture(w * kAtlasPerChunk, h * kAtlasPerChunk, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &atlas, nullptr))) {
        ReleaseAtlas();
        return;
    }
    IDirect3DSurface9* dst = nullptr;
    bool copied = false;
    if (SUCCEEDED(atlas->GetSurfaceLevel(0, &dst))) {
        // black first (cells of chunks not seen yet), then the old atlas on top at its offset
        dev->ColorFill(dst, nullptr, D3DCOLOR_ARGB(255, 0, 0, 0));
        IDirect3DSurface9* src = nullptr;
        if (g_atlas && SUCCEEDED(g_atlas->GetSurfaceLevel(0, &src))) {
            const int ox = (g_atlasMinX - minX) * kAtlasPerChunk, oz = (g_atlasMinZ - minZ) * kAtlasPerChunk;
            const RECT dr{ox, oz, ox + g_atlasW * kAtlasPerChunk, oz + g_atlasH * kAtlasPerChunk};
            copied = SUCCEEDED(dev->StretchRect(src, nullptr, dst, &dr, D3DTEXF_NONE));
            src->Release();
        }
        dst->Release();
    }
    const bool hadAtlas = g_atlas != nullptr;
    const int oldMinX = g_atlasMinX, oldMinZ = g_atlasMinZ, oldW = g_atlasW, oldH = g_atlasH, oldCopies = g_atlasChunks;
    if (g_atlas) g_atlas->Release();
    g_atlas = atlas;
    g_atlasMinX = minX;
    g_atlasMinZ = minZ;
    g_atlasW = w;
    g_atlasH = h;
    g_atlasChunks = copied ? oldCopies : 0;
    if (hadAtlas) g_atlasGrowths++;
    for (auto& [k, e] : g_entries) {
        const int ix = ChunkIndex(k.first), iz = ChunkIndex(k.second);
        const bool wasInside = hadAtlas && ix >= oldMinX && iz >= oldMinZ && ix < oldMinX + oldW && iz < oldMinZ + oldH;
        if (copied && wasInside) continue; // its cell came over with the copy
        const bool hadSmooth = e.atlasSmooth;
        e.atlasHash = 0; // a plain copy goes in at once (raw copies below), the smoothed one when the chunk is smoothed
        e.atlasSmooth = false;
        e.gAtlasSig = 0; // GPU path: the next GpuService writes the cell from the chunk's smoothed map
        if (hadSmooth && !copied) e.doneSig = 0; // copy failed: smooth it again so the atlas gets the smoothed map back
    }
    if (hadAtlas)
        LOG_INFO(std::format("[LightmapSmooth] World map grown to {}x{} chunks ({})", w, h, copied ? "old contents copied" : "copy failed: cells refilled"));
}

enum class UploadResult { Done, Later };

UploadResult Upload(IDirect3DDevice9* dev, Result& r) {
    auto it = g_entries.find(r.key);
    if (it == g_entries.end()) return UploadResult::Done;
    Entry& e = it->second;
    if (r.gen != e.gen || !e.inFlight) return UploadResult::Done; // from before a Clear or an older job
    auto finish = [&] {
        e.inFlight = false;
        g_inFlight = std::max(0, g_inFlight - 1);
    };
    if (r.levels.size() != kLevels) { // processing failed
        finish();
        return UploadResult::Done;
    }
    // the game changed the map again while it was smoothed: the game's map stays (CheckEntry marked it for a new job). While
    // the map is unknown (hash 0: a new texture not read yet) the result is kept; Get shows it only if the read confirms it.
    if (e.hash != 0 && r.centreHash != e.hash) {
        g_staleResults++;
        finish();
        return UploadResult::Done;
    }
    if (!g_upStaging && FAILED(dev->CreateTexture(kOut, kOut, kLevels, 0, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &g_upStaging, nullptr))) {
        g_upStaging = nullptr;
        finish();
        return UploadResult::Done;
    }
    D3DLOCKED_RECT lr{};
    HRESULT hr = g_upStaging->LockRect(0, &lr, nullptr, D3DLOCK_DONOTWAIT);
    if (hr == D3DERR_WASSTILLDRAWING) { // the previous upload is still being copied: next frame
        g_deferredUploads++;
        return UploadResult::Later;
    }
    if (FAILED(hr)) hr = g_upStaging->LockRect(0, &lr, nullptr, 0);
    if (FAILED(hr)) {
        finish();
        return UploadResult::Done;
    }
    bool ok = true;
    for (UINT l = 0; l < kLevels; l++) {
        if (l > 0 && FAILED(g_upStaging->LockRect(l, &lr, nullptr, 0))) {
            ok = false;
            break;
        }
        const int size = kOut >> l;
        for (int y = 0; y < size; y++) std::memcpy(static_cast<uint8_t*>(lr.pBits) + y * lr.Pitch, &r.levels[l][static_cast<size_t>(y) * size], size * 4);
        g_upStaging->UnlockRect(l);
    }
    IDirect3DTexture9* tex = e.smooth; // updated in place: draws already recorded keep the old contents (D3D9 ordering)
    bool fresh = false;
    if (ok && !tex) {
        fresh = true;
        if (!g_texPool.empty()) {
            tex = g_texPool.back();
            g_texPool.pop_back();
        } else if (FAILED(dev->CreateTexture(kOut, kOut, kLevels, 0, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &tex, nullptr)))
            tex = nullptr;
    }
    if (ok && tex && SUCCEEDED(dev->UpdateTexture(g_upStaging, tex))) {
        e.smooth = tex;
        e.doneHash = r.centreHash;
        e.doneSig = r.sig;
        g_uploaded++;
        if (AtlasUpload(dev, r.key, r.levels[1])) {
            e.atlasHash = r.centreHash;
            e.atlasSmooth = true;
        }
    } else if (fresh && tex)
        PoolPut(tex);
    finish();
    return UploadResult::Done;
}

void EndSweepIfDone() {
    if (!g_sweepOn) return;
    int awaiting = 0;
    for (auto& [k, e] : g_entries) awaiting += e.awaiting ? 1 : 0;
    if (awaiting > 0 && g_frame < g_sweepUntil) return;
    g_sweepOn = false;
    for (auto& [k, e] : g_entries) e.awaiting = false;
    g_lastSweep = std::format("kick -> rebuild {:.0f} ms, rebuild -> first chunk {:.0f} ms, -> last chunk {:.0f} ms ({} chunks changed, {} not re-rendered)",
                              g_kickToConsumeMs, g_sweepFirstMs, g_sweepLastMs, g_sweepChanges, awaiting);
    if (!kPublicBuild) LOG_INFO("[LightmapSmooth] Rebuild sweep done: " + g_lastSweep);
}

// =====================================================================================================================
// GPU path. The CPU steps of Process, one pixel-shader pass each (shaders/lightmap_smooth_ps.hlsl has the math):
//   1 Gather   game maps (chunk + 8 neighbours, bound one at a time) -> RAW   264x264 float RGBA (P grid = source -4..259)
//   2 HBlur    RAW -> HB   (horizontal Gaussian of R, G, B, Y)
//   3 VBlur    HB  -> CH   (vertical Gaussian, colour ratio with the dim-texel fade)
//   4 HUp      RAW -> HUYA (1024 x 264: B-spline of Y, A), CH -> HUC (1024 x 264: linear colour ratio)
//   5 VUp      HUYA + HUC -> the chunk's level 0 (1024x1024 A8R8G8B8, dither + 8-bit rounding in the shader)
//   6 Down     level 0 -> atlas cell (512x512, = mip 1) and -> MIP[1]; then MIP[l-1] -> MIP[l] (Down) and MIP[l] ->
//              chunk level l (Copy), l = 1..10. Separate scratch textures, so no pass samples the texture it renders to.
// All of it on the render thread, inside the game's scene, with every device state it touches saved and restored.
// =====================================================================================================================

constexpr int kP = kSrc + 8;             // P grid: source texels -4..259 of the chunk (P = source + 4)
constexpr int kUrgentCap = 32;           // own changes of chunks in view rebuilt in one service (all of them in practice)
constexpr int kCellCap = 64;             // atlas cells refilled from existing smoothed maps per service
constexpr double kBackgroundMs = 1.0;    // GPU budget per frame: chunks out of view and neighbour-border updates
constexpr float kDefaultChunkMs = 0.5f;  // cost estimate until the timestamp queries measured one

enum class GpuState { Unknown, Ready, Failed };
enum PsId { PsGather, PsHBlur, PsVBlur, PsHUpYA, PsHUpChroma, PsVUp, PsDown, PsCopy, PsCount };
constexpr const char* kPsEntries[PsCount] = {"GatherPS", "HBlurPS", "VBlurPS", "HUpYAPS", "HUpChromaPS", "VUpPS", "DownPS", "CopyPS"};
constexpr const char* kPsTags[PsCount] = {"LightmapSmooth GatherPS", "LightmapSmooth HBlurPS", "LightmapSmooth VBlurPS", "LightmapSmooth HUpYAPS",
                                          "LightmapSmooth HUpChromaPS", "LightmapSmooth VUpPS", "LightmapSmooth DownPS", "LightmapSmooth CopyPS"};

// Compiled at start-up on a background thread (framework/shader_cache.h); InitGpu only creates the shader objects.
ShaderCache::Id AddSmoothShader(int i) {
    ShaderCache::Desc d;
    d.tag = kPsTags[i];
    d.source = kLightmapSmoothHlsl;
    d.sourceName = "lightmap_smooth_ps.hlsl";
    d.entry = kPsEntries[i];
    d.target = "ps_3_0";
    d.flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
    d.priority = 0;
    return ShaderCache::Add(std::move(d));
}
const ShaderCache::Id kPsIds[PsCount] = {AddSmoothShader(0), AddSmoothShader(1), AddSmoothShader(2), AddSmoothShader(3),
                                         AddSmoothShader(4), AddSmoothShader(5), AddSmoothShader(6), AddSmoothShader(7)};
static_assert(PsCount == 8, "kPsIds lists every PsId");

bool g_gpuPreferred = true;               // developer setting (A/B); render thread
bool g_gpuActive = false;                 // the path in use (switching clears everything, see ResolveMode)
GpuState g_gpuState = GpuState::Unknown;  // shaders and formats checked (once per session)
std::string g_gpuReason;                  // why the GPU path is not available
IDirect3DDevice9* g_dev = nullptr;        // the game's device (identity only, from OnPresent)
IDirect3DPixelShader9* g_ps[PsCount] = {}; // survive a device reset, kept for the session
D3DFORMAT g_floatFmt = D3DFMT_UNKNOWN;    // A32B32G32R32F, else A16B16G16R16F

struct Scratch {
    IDirect3DTexture9* tex = nullptr;
    IDirect3DSurface9* surf = nullptr;
    void Release() {
        if (surf) surf->Release();
        if (tex) tex->Release();
        surf = nullptr;
        tex = nullptr;
    }
};
Scratch g_raw, g_hb, g_ch, g_huYA, g_huC; // float intermediates (D3DPOOL_DEFAULT render targets)
Scratch g_mip[kLevels];                   // A8R8G8B8, one level each: g_mip[l] is (1024 >> l)^2, l = 1..10

uint32_t g_servicedFrame = UINT32_MAX;
uint64_t g_changeEpoch = 1, g_serviceEpoch = 0; // a change seen since the last service runs it again in the same frame

int g_gpuBuilds = 0, g_gpuInView = 0, g_gpuOutOfView = 0, g_gpuBorders = 0, g_gpuCells = 0, g_gpuFailures = 0;
int g_gpuHashChecks = 0, g_sameMapNotices = 0;
float g_msPerChunk = -1.0f, g_msBatchMax = 0.0f, g_msLastBatch = -1.0f;
int g_timedBatches = 0;

std::atomic<bool> g_compareRequested{false};
std::string g_compareResult = "not run";

bool Visible(const Entry& e) { return e.lastUse + 2 >= g_frame; }

// Inputs of a GPU build: the map versions of the chunk and its 8 neighbours (0 = neighbour not registered).
uint64_t GpuSig(const LightmapSmooth::Key& k) {
    uint64_t v[9] = {};
    for (int dz = -1; dz <= 1; dz++)
        for (int dx = -1; dx <= 1; dx++) {
            auto it = g_entries.find(Neighbour(k, dx, dz));
            if (it != g_entries.end() && it->second.src) v[(dz + 1) * 3 + (dx + 1)] = it->second.gver;
        }
    return SigOf(v);
}

void GpuRuntimeFailure(const std::string& why) {
    if (g_gpuState == GpuState::Failed) return;
    g_gpuState = GpuState::Failed;
    g_gpuReason = why;
    LOG_WARNING("[LightmapSmooth] GPU smoothing failed (" + why + "): switching to the CPU path");
}

// Hash of level 0 of a lockable (managed 256x256 DXT5) game map; false when it cannot be locked.
bool HashMap(IDirect3DTexture9* tex, uint64_t& h) {
    D3DSURFACE_DESC d{};
    if (FAILED(tex->GetLevelDesc(0, &d)) || d.Width != kSrc || d.Height != kSrc || d.Format != D3DFMT_DXT5 || d.Pool == D3DPOOL_DEFAULT) return false;
    D3DLOCKED_RECT lr{};
    if (FAILED(tex->LockRect(0, &lr, nullptr, D3DLOCK_READONLY))) return false;
    h = HashRows(static_cast<const uint8_t*>(lr.pBits), lr.Pitch);
    tex->UnlockRect(0);
    return true;
}

void BumpVersion(const LightmapSmooth::Key& k, Entry& e, const char* via) {
    e.gver++;
    e.awaiting = false; // (re-)rendered
    g_changeEpoch++;
    NoteChange(k, e, via);
}

// Chunk re-render notices (render thread; cheap when there are none): the map changed now, rebuild it before its next
// use. A lockable map whose hash did not change (a rebuild by day re-renders every chunk the same) is not rebuilt.
void DrainNotices() {
    // A plain load first (2026-09-29): this runs from every draw that asks for a chunk; the locked exchange only when a
    // notice is waiting. A notice posted just after the load is taken by the next call, as one posted just after the
    // exchange always was.
    if (!g_noticePending.load(std::memory_order_relaxed) || !g_noticePending.exchange(false)) return;
    std::vector<LightmapSmooth::Key> keys;
    {
        std::lock_guard<std::mutex> lk(g_noticeMx);
        keys.swap(g_noticeKeys);
    }
    for (const auto& k : keys) {
        if (g_drainedThisFrame.size() < 256) g_drainedThisFrame.push_back(k);
        auto it = g_entries.find(k);
        if (it == g_entries.end()) continue;
        Entry& e = it->second;
        g_notices++;
        if (e.src && !e.gNoHash) {
            uint64_t h = 0;
            if (HashMap(e.src, h)) {
                if (e.ghash != 0 && h == e.ghash) {
                    e.awaiting = false;
                    g_sameMapNotices++;
                    continue;
                }
                e.ghash = h;
            } else
                e.gNoHash = true;
        }
        BumpVersion(k, e, "render notice");
    }
}

// Fallback detection (Present): maps changed without a notice (other callers of the re-render, hook not installed).
void GpuHashCheck(const LightmapSmooth::Key& k, Entry& e, const char* via) {
    if (!e.src || e.gNoHash) return;
    uint64_t h = 0;
    if (!HashMap(e.src, h)) {
        e.gNoHash = true;
        return;
    }
    g_gpuHashChecks++;
    if (e.ghash == 0) { // not known yet: the next build records it
        e.ghash = h;
        return;
    }
    if (h != e.ghash) {
        e.ghash = h;
        BumpVersion(k, e, via);
    }
}

// ---- setup ----
bool RtFormatOk(IDirect3DDevice9* dev, D3DFORMAT fmt) {
    IDirect3D9* d3d = nullptr;
    if (FAILED(dev->GetDirect3D(&d3d)) || !d3d) return false;
    D3DDEVICE_CREATION_PARAMETERS cp{};
    bool ok = SUCCEEDED(dev->GetCreationParameters(&cp));
    D3DFORMAT adapterFmt = D3DFMT_X8R8G8B8;
    D3DDISPLAYMODE mode{};
    if (ok && SUCCEEDED(d3d->GetAdapterDisplayMode(cp.AdapterOrdinal, &mode)) && mode.Format != D3DFMT_UNKNOWN) adapterFmt = mode.Format;
    ok = ok && SUCCEEDED(d3d->CheckDeviceFormat(cp.AdapterOrdinal, cp.DeviceType, adapterFmt, D3DUSAGE_RENDERTARGET, D3DRTYPE_TEXTURE, fmt));
    d3d->Release();
    return ok;
}

const char* FloatFmtName() {
    return g_floatFmt == D3DFMT_A32B32G32R32F ? "A32B32G32R32F" : (g_floatFmt == D3DFMT_A16B16G16R16F ? "A16B16G16R16F" : "none");
}

// Once per session: pixel shader 3.0, the render-target formats, the shaders. Failure = CPU path (logged).
bool InitGpu(IDirect3DDevice9* dev) {
    auto fail = [](const std::string& why) {
        g_gpuState = GpuState::Failed;
        g_gpuReason = why;
        LOG_WARNING("[LightmapSmooth] GPU smoothing unavailable (" + why + "): using the CPU path");
        return false;
    };
    D3DCAPS9 caps{};
    if (FAILED(dev->GetDeviceCaps(&caps)) || caps.PixelShaderVersion < D3DPS_VERSION(3, 0)) return fail("no pixel shader 3.0");
    if (!RtFormatOk(dev, D3DFMT_A8R8G8B8)) return fail("no A8R8G8B8 render-target textures");
    if (RtFormatOk(dev, D3DFMT_A32B32G32R32F)) g_floatFmt = D3DFMT_A32B32G32R32F;
    else if (RtFormatOk(dev, D3DFMT_A16B16G16R16F)) g_floatFmt = D3DFMT_A16B16G16R16F;
    else return fail("no float render-target texture format");
    for (int i = 0; i < PsCount; i++) {
        if (g_ps[i]) continue;
        // precompiled at start-up (shader_cache.h): only the shader object is created here
        std::string msg;
        const ShaderCache::Result res = ShaderCache::CreatePixelShader(dev, kPsIds[i], &g_ps[i], &msg);
        if (res == ShaderCache::Result::CreateFailed) msg = "CreatePixelShader failed";
        if (res != ShaderCache::Result::Ok && msg.empty()) msg = "unknown error";
        if (!msg.empty()) {
            LOG_ERROR(std::format("[LightmapSmooth] Shader {} failed: {}", kPsEntries[i], msg));
            return fail(std::string("shader ") + kPsEntries[i] + " did not compile");
        }
    }
    g_gpuState = GpuState::Ready;
    LOG_INFO(std::format("[LightmapSmooth] GPU smoothing ready (intermediates {}, PS 3.0 {} instruction slots)", FloatFmtName(), caps.MaxPixelShader30InstructionSlots));
    return true;
}

bool CreateScratch(IDirect3DDevice9* dev, UINT w, UINT h, D3DFORMAT fmt, Scratch& s) {
    if (s.tex) return true;
    if (FAILED(dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, fmt, D3DPOOL_DEFAULT, &s.tex, nullptr)) || !s.tex) {
        s.tex = nullptr;
        return false;
    }
    if (FAILED(s.tex->GetSurfaceLevel(0, &s.surf)) || !s.surf) {
        s.Release();
        return false;
    }
    return true;
}

// Scratch render targets, created on first use and after a device reset (about 13 MB of video memory with 32-bit floats).
bool EnsureGpuRes(IDirect3DDevice9* dev) {
    bool ok = CreateScratch(dev, kP, kP, g_floatFmt, g_raw) && CreateScratch(dev, kP, kP, g_floatFmt, g_hb) && CreateScratch(dev, kP, kP, g_floatFmt, g_ch) &&
              CreateScratch(dev, kOut, kP, g_floatFmt, g_huYA) && CreateScratch(dev, kOut, kP, g_floatFmt, g_huC);
    for (UINT l = 1; ok && l < kLevels; l++) ok = CreateScratch(dev, kOut >> l, kOut >> l, D3DFMT_A8R8G8B8, g_mip[l]);
    return ok;
}

void ReleaseGpuRes() {
    for (Scratch* s : {&g_raw, &g_hb, &g_ch, &g_huYA, &g_huC}) s->Release();
    for (auto& s : g_mip) s.Release();
}

// ---- GPU timing (timestamp queries, read back without waiting at Present) ----
struct TimeSlot {
    IDirect3DQuery9 *dis = nullptr, *t0 = nullptr, *t1 = nullptr, *freq = nullptr;
    bool issued = false;
    int chunks = 0;
    void Release() {
        for (IDirect3DQuery9** q : {&dis, &t0, &t1, &freq}) {
            if (*q) (*q)->Release();
            *q = nullptr;
        }
        issued = false;
    }
};
constexpr int kTimeSlots = 8;
TimeSlot g_ts[kTimeSlots];
int g_tsNext = 0;

TimeSlot* BeginTiming(IDirect3DDevice9* dev) {
    TimeSlot& s = g_ts[g_tsNext];
    if (s.issued) return nullptr;
    if (!s.dis) {
        const bool ok = SUCCEEDED(dev->CreateQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT, &s.dis)) && SUCCEEDED(dev->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &s.t0)) &&
                        SUCCEEDED(dev->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &s.t1)) && SUCCEEDED(dev->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ, &s.freq));
        if (!ok) {
            s.Release();
            return nullptr;
        }
    }
    s.dis->Issue(D3DISSUE_BEGIN);
    s.t0->Issue(D3DISSUE_END);
    return &s;
}

void EndTiming(TimeSlot* s, int chunks) {
    if (!s) return;
    s->t1->Issue(D3DISSUE_END);
    s->freq->Issue(D3DISSUE_END);
    s->dis->Issue(D3DISSUE_END);
    s->issued = true;
    s->chunks = chunks;
    g_tsNext = (g_tsNext + 1) % kTimeSlots;
}

void ReadTimings() {
    for (auto& s : g_ts) {
        if (!s.issued) continue;
        BOOL disjoint = TRUE;
        UINT64 t0 = 0, t1 = 0, freq = 0;
        if (s.dis->GetData(&disjoint, sizeof disjoint, 0) != S_OK || s.t0->GetData(&t0, sizeof t0, 0) != S_OK || s.t1->GetData(&t1, sizeof t1, 0) != S_OK ||
            s.freq->GetData(&freq, sizeof freq, 0) != S_OK)
            continue;
        s.issued = false;
        if (disjoint || !freq || t1 < t0) continue;
        const float ms = static_cast<float>(double(t1 - t0) * 1000.0 / double(freq));
        g_msLastBatch = ms;
        g_msBatchMax = std::max(g_msBatchMax, ms);
        g_timedBatches++;
        if (s.chunks > 0) {
            const float per = ms / static_cast<float>(s.chunks);
            g_msPerChunk = g_msPerChunk < 0 ? per : g_msPerChunk * 0.8f + per * 0.2f;
        }
    }
}

void ReleaseTimings() {
    for (auto& s : g_ts) s.Release();
    g_tsNext = 0;
}

// ---- device state of the passes: everything they change is saved first and put back after (through the device, so
// the hook trackers of the other modules see the game's state again) ----
constexpr D3DRENDERSTATETYPE kPassRs[] = {D3DRS_ZENABLE, D3DRS_ZWRITEENABLE, D3DRS_ALPHABLENDENABLE, D3DRS_SEPARATEALPHABLENDENABLE, D3DRS_ALPHATESTENABLE,
    D3DRS_STENCILENABLE, D3DRS_CULLMODE, D3DRS_SCISSORTESTENABLE, D3DRS_FOGENABLE, D3DRS_SRGBWRITEENABLE, D3DRS_CLIPPLANEENABLE, D3DRS_COLORWRITEENABLE,
    D3DRS_FILLMODE, D3DRS_WRAP0};
constexpr DWORD kPassRsValue[] = {D3DZB_FALSE, FALSE, FALSE, FALSE, FALSE, FALSE, D3DCULL_NONE, FALSE, FALSE, FALSE, 0, 0xF, D3DFILL_SOLID, 0};
constexpr D3DSAMPLERSTATETYPE kPassSs[] = {D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER, D3DSAMP_MIPFILTER, D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_SRGBTEXTURE,
    D3DSAMP_MAXMIPLEVEL, D3DSAMP_MIPMAPLODBIAS};
constexpr DWORD kPassSsValue[] = {D3DTEXF_POINT, D3DTEXF_POINT, D3DTEXF_NONE, D3DTADDRESS_CLAMP, D3DTADDRESS_CLAMP, FALSE, 0, 0};
constexpr int kPassRsN = static_cast<int>(sizeof(kPassRs) / sizeof(kPassRs[0]));
constexpr int kPassSsN = static_cast<int>(sizeof(kPassSs) / sizeof(kPassSs[0]));
static_assert(kPassRsN == sizeof(kPassRsValue) / sizeof(kPassRsValue[0]) && kPassSsN == sizeof(kPassSsValue) / sizeof(kPassSsValue[0]));
constexpr DWORD kPassSamplers = 2; // s0, s1
constexpr UINT kPassConsts = 2;    // c0, c1
constexpr DWORD kMaxRts = 4;

struct PassState {
    IDirect3DDevice9* dev = nullptr;
    IDirect3DSurface9* rt[kMaxRts] = {};
    IDirect3DSurface9* ds = nullptr;
    IDirect3DPixelShader9* ps = nullptr;
    IDirect3DVertexShader9* vs = nullptr;
    IDirect3DVertexDeclaration9* decl = nullptr;
    DWORD fvf = 0;
    IDirect3DVertexBuffer9* vb0 = nullptr;
    UINT vb0Offset = 0, vb0Stride = 0, freq0 = 1, freq1 = 1;
    IDirect3DBaseTexture9* tex[kPassSamplers] = {};
    DWORD ss[kPassSamplers][kPassSsN] = {};
    DWORD rs[kPassRsN] = {};
    float psc[kPassConsts * 4] = {};
    D3DVIEWPORT9 vp{};
    RECT scissor{};

    void Capture(IDirect3DDevice9* d) {
        dev = d;
        for (DWORD i = 0; i < kMaxRts; i++)
            if (FAILED(dev->GetRenderTarget(i, &rt[i]))) rt[i] = nullptr;
        if (FAILED(ExtraHooks::RawGetDepthStencilSurface(dev, &ds))) ds = nullptr;
        dev->GetPixelShader(&ps);
        dev->GetVertexShader(&vs);
        dev->GetVertexDeclaration(&decl);
        dev->GetFVF(&fvf);
        dev->GetStreamSource(0, &vb0, &vb0Offset, &vb0Stride);
        dev->GetStreamSourceFreq(0, &freq0);
        dev->GetStreamSourceFreq(1, &freq1);
        for (DWORD s = 0; s < kPassSamplers; s++) {
            dev->GetTexture(s, &tex[s]);
            for (int i = 0; i < kPassSsN; i++) dev->GetSamplerState(s, kPassSs[i], &ss[s][i]);
        }
        for (int i = 0; i < kPassRsN; i++) dev->GetRenderState(kPassRs[i], &rs[i]);
        dev->GetPixelShaderConstantF(0, psc, kPassConsts);
        dev->GetViewport(&vp);
        dev->GetScissorRect(&scissor);
    }

    // The pass states (render target 0 is set per pass; the others and the depth-stencil are unbound: sizes differ).
    void Apply() {
        for (DWORD i = 1; i < kMaxRts; i++)
            if (rt[i]) dev->SetRenderTarget(i, nullptr);
        ExtraHooks::RawSetDepthStencilSurface(dev, nullptr);
        dev->SetVertexShader(nullptr);
        dev->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
        dev->SetStreamSourceFreq(0, 1);
        dev->SetStreamSourceFreq(1, 1);
        for (int i = 0; i < kPassRsN; i++) dev->SetRenderState(kPassRs[i], kPassRsValue[i]);
        for (DWORD s = 0; s < kPassSamplers; s++) {
            dev->SetTexture(s, nullptr); // nothing of ours stays bound while it becomes a render target
            for (int i = 0; i < kPassSsN; i++) dev->SetSamplerState(s, kPassSs[i], kPassSsValue[i]);
        }
    }

    void Restore() {
        if (!dev) return;
        dev->SetRenderTarget(0, rt[0]); // resets the viewport and the scissor rect: they come after
        for (DWORD i = 1; i < kMaxRts; i++)
            if (rt[i]) dev->SetRenderTarget(i, rt[i]);
        ExtraHooks::RawSetDepthStencilSurface(dev, ds);
        for (DWORD s = 0; s < kPassSamplers; s++) {
            dev->SetTexture(s, tex[s]);
            for (int i = 0; i < kPassSsN; i++) dev->SetSamplerState(s, kPassSs[i], ss[s][i]);
        }
        for (int i = 0; i < kPassRsN; i++) dev->SetRenderState(kPassRs[i], rs[i]);
        dev->SetPixelShaderConstantF(0, psc, kPassConsts);
        dev->SetPixelShader(ps);
        dev->SetVertexShader(vs);
        if (decl) dev->SetVertexDeclaration(decl);
        else dev->SetFVF(fvf);
        dev->SetStreamSource(0, vb0, vb0Offset, vb0Stride); // DrawPrimitiveUP cleared stream 0
        dev->SetStreamSourceFreq(0, freq0);
        dev->SetStreamSourceFreq(1, freq1);
        dev->SetViewport(&vp);
        dev->SetScissorRect(&scissor);
        for (auto*& s : rt)
            if (s) s->Release(), s = nullptr;
        for (auto*& t : tex)
            if (t) t->Release(), t = nullptr;
        if (ds) ds->Release();
        if (ps) ps->Release();
        if (vs) vs->Release();
        if (decl) decl->Release();
        if (vb0) vb0->Release();
        ds = nullptr;
        ps = nullptr;
        vs = nullptr;
        decl = nullptr;
        vb0 = nullptr;
        dev = nullptr;
    }

    ~PassState() { Restore(); } // also on an exception (out of memory) in the middle of the passes
};

struct QuadVertex {
    float x, y, z, rhw, u, v;
};

// Rectangle [x0, x1) x [y0, y1) of the current render target (pixels; pre-transformed, the viewport is the whole target);
// the texture coordinate is (u0, v0) at the top-left pixel corner and (u1, v1) at the bottom-right one.
void DrawRect(IDirect3DDevice9* dev, float x0, float y0, float x1, float y1, float u0, float v0, float u1, float v1) {
    const QuadVertex q[4] = {{x0 - 0.5f, y0 - 0.5f, 0, 1, u0, v0}, {x1 - 0.5f, y0 - 0.5f, 0, 1, u1, v0}, {x0 - 0.5f, y1 - 0.5f, 0, 1, u0, v1},
                             {x1 - 0.5f, y1 - 0.5f, 0, 1, u1, v1}};
    dev->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, q, sizeof(QuadVertex));
}

// Full target of w x h pixels, texture coordinates in pixel units (u = x + 0.5 at pixel x).
void DrawPixels(IDirect3DDevice9* dev, int w, int h) {
    DrawRect(dev, 0, 0, static_cast<float>(w), static_cast<float>(h), 0, 0, static_cast<float>(w), static_cast<float>(h));
}

void InSize(IDirect3DDevice9* dev, UINT reg, int w, int h) {
    const float c[4] = {1.0f / static_cast<float>(w), 1.0f / static_cast<float>(h), static_cast<float>(w), static_cast<float>(h)};
    dev->SetPixelShaderConstantF(reg, c, 1);
}

bool Target(IDirect3DDevice9* dev, IDirect3DSurface9* s) { return s && SUCCEEDED(dev->SetRenderTarget(0, s)); }

// The chunk's level 0 -> its atlas cell (DownPS = exactly the chunk's mip 1).
bool WriteCell(IDirect3DDevice9* dev, const LightmapSmooth::Key& key, Entry& e) {
    POINT pt{};
    if (!e.gtex || !AtlasCell(key, pt)) return false;
    IDirect3DSurface9* dst = nullptr;
    if (FAILED(g_atlas->GetSurfaceLevel(0, &dst)) || !dst) return false;
    const bool ok = Target(dev, dst);
    dst->Release();
    if (!ok) return false;
    dev->SetPixelShader(g_ps[PsDown]);
    dev->SetTexture(0, e.gtex);
    InSize(dev, 0, kOut, kOut);
    const float x = static_cast<float>(pt.x), y = static_cast<float>(pt.y), n = static_cast<float>(kAtlasPerChunk);
    DrawRect(dev, x, y, x + n, y + n, 0, 0, n, n);
    e.gAtlasSig = e.gBuiltSig;
    g_atlasChunks++;
    g_gpuCells++;
    return true;
}

// One chunk, every pass (state already applied by RunBatch).
bool BuildOne(IDirect3DDevice9* dev, const LightmapSmooth::Key& key, Entry& e) {
    if (!e.src) return false;
    if (!e.gtex && (FAILED(dev->CreateTexture(kOut, kOut, kLevels, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &e.gtex, nullptr)) || !e.gtex)) {
        e.gtex = nullptr;
        return false;
    }
    // inputs: the chunk's map and the registered neighbours' (a missing one = the centre map, clamped: see GatherPS)
    IDirect3DTexture9* nb[9] = {};
    uint64_t vers[9] = {};
    for (int dz = -1; dz <= 1; dz++)
        for (int dx = -1; dx <= 1; dx++) {
            const int slot = (dz + 1) * 3 + (dx + 1);
            if (!dx && !dz) {
                nb[slot] = e.src;
                vers[slot] = e.gver;
                continue;
            }
            auto it = g_entries.find(Neighbour(key, dx, dz));
            if (it != g_entries.end() && it->second.src) {
                nb[slot] = it->second.src;
                vers[slot] = it->second.gver;
            }
        }
    const uint64_t sig = SigOf(vers);
    if (!e.gNoHash) { // the map this build reads (fallback detection compares with it)
        uint64_t h = 0;
        if (HashMap(e.src, h)) e.ghash = h;
        else e.gNoHash = true;
    }

    // 1. Gather: 9 regions of the P grid (x: [0,4) left neighbour, [4,260) chunk, [260,264) right neighbour; same in z)
    if (!Target(dev, g_raw.surf)) return false;
    dev->SetPixelShader(g_ps[PsGather]);
    for (int dz = -1; dz <= 1; dz++)
        for (int dx = -1; dx <= 1; dx++) {
            const int slot = (dz + 1) * 3 + (dx + 1);
            const bool present = nb[slot] != nullptr;
            const int x0 = dx < 0 ? 0 : (dx == 0 ? 4 : kSrc + 4), x1 = dx < 0 ? 4 : (dx == 0 ? kSrc + 4 : kP);
            const int y0 = dz < 0 ? 0 : (dz == 0 ? 4 : kSrc + 4), y1 = dz < 0 ? 4 : (dz == 0 ? kSrc + 4 : kP);
            // texel of this map at the region's first column / row (a missing neighbour reads the centre map outside
            // [0, 256): CLAMP gives the CPU's "clamp to the centre chunk")
            const int tx0 = (x0 - 4) - (present ? dx * kSrc : 0), tz0 = (y0 - 4) - (present ? dz * kSrc : 0);
            dev->SetTexture(0, present ? nb[slot] : e.src);
            const float inv = 1.0f / kSrc;
            DrawRect(dev, static_cast<float>(x0), static_cast<float>(y0), static_cast<float>(x1), static_cast<float>(y1), tx0 * inv, tz0 * inv, (tx0 + x1 - x0) * inv,
                     (tz0 + y1 - y0) * inv);
        }
    // 2-3. Chroma cleanup
    if (!Target(dev, g_hb.surf)) return false;
    dev->SetPixelShader(g_ps[PsHBlur]);
    dev->SetTexture(0, g_raw.tex);
    InSize(dev, 0, kP, kP);
    DrawPixels(dev, kP, kP);
    if (!Target(dev, g_ch.surf)) return false;
    dev->SetPixelShader(g_ps[PsVBlur]);
    dev->SetTexture(0, g_hb.tex);
    DrawPixels(dev, kP, kP);
    // 4. Enlarge, horizontal
    if (!Target(dev, g_huYA.surf)) return false;
    dev->SetPixelShader(g_ps[PsHUpYA]);
    dev->SetTexture(0, g_raw.tex);
    DrawPixels(dev, kOut, kP);
    if (!Target(dev, g_huC.surf)) return false;
    dev->SetPixelShader(g_ps[PsHUpChroma]);
    dev->SetTexture(0, g_ch.tex);
    DrawPixels(dev, kOut, kP);
    // 5. Enlarge, vertical -> level 0
    IDirect3DSurface9* lvl = nullptr;
    if (FAILED(e.gtex->GetSurfaceLevel(0, &lvl)) || !lvl) return false;
    bool ok = Target(dev, lvl);
    lvl->Release();
    if (!ok) return false;
    dev->SetPixelShader(g_ps[PsVUp]);
    dev->SetTexture(0, g_huYA.tex);
    dev->SetTexture(1, g_huC.tex);
    InSize(dev, 0, kOut, kP);
    InSize(dev, 1, kOut, kP);
    DrawPixels(dev, kOut, kOut);
    dev->SetTexture(1, nullptr);
    e.gBuiltSig = sig; // for WriteCell
    // 6. Atlas cell and mips
    WriteCell(dev, key, e);
    for (UINT l = 1; l < kLevels; l++) {
        const int size = kOut >> l;
        if (!Target(dev, g_mip[l].surf)) return false;
        dev->SetPixelShader(g_ps[PsDown]);
        dev->SetTexture(0, l == 1 ? static_cast<IDirect3DBaseTexture9*>(e.gtex) : g_mip[l - 1].tex); // level 0 of the chunk
        InSize(dev, 0, size * 2, size * 2);
        DrawPixels(dev, size, size);
        if (FAILED(e.gtex->GetSurfaceLevel(l, &lvl)) || !lvl) return false;
        dev->SetTexture(0, g_mip[l].tex); // before the chunk becomes the target: it is no longer bound
        ok = Target(dev, lvl);
        lvl->Release();
        if (!ok) return false;
        dev->SetPixelShader(g_ps[PsCopy]);
        InSize(dev, 0, size, size);
        DrawPixels(dev, size, size);
    }
    dev->SetTexture(0, nullptr);
    e.gBuiltVer = e.gver;
    return true;
}

struct BuildItem {
    const LightmapSmooth::Key* key;
    Entry* e;
    int kind; // 0 own change in view, 1 own change out of view, 2 neighbour border, 3 atlas cell only
};

// Runs the items with one state save / restore and one timing.
void RunBatch(IDirect3DDevice9* dev, const std::vector<BuildItem>& items) {
    if (items.empty() || g_gpuState != GpuState::Ready) return;
    if (!EnsureGpuRes(dev)) {
        GpuRuntimeFailure("could not create its render targets");
        return;
    }
    PassState st;
    st.Capture(dev);
    st.Apply();
    TimeSlot* ts = BeginTiming(dev);
    int built = 0;
    for (const BuildItem& it : items) {
        if (it.kind == 3) {
            WriteCell(dev, *it.key, *it.e);
            continue;
        }
        it.e->gTryFrame = g_frame;
        it.e->gBuiltVer = 0; // a build that stops half way is never shown (the game's map is, until the next try)
        if (!BuildOne(dev, *it.key, *it.e)) {
            g_gpuFailures++;
            continue;
        }
        built++;
        g_gpuBuilds++;
        (it.kind == 0 ? g_gpuInView : (it.kind == 1 ? g_gpuOutOfView : g_gpuBorders))++;
    }
    EndTiming(ts, built);
    st.Restore();
}

std::string RunCompare(IDirect3DDevice9* dev); // developer: GPU vs CPU on one chunk (below)

// GPU work called from the draw hooks: an exception (out of memory in the 32-bit game) turns the feature off (released at
// the next Present) instead of unwinding into the lot light bridge, which would switch every fix off.
template <typename F> void GpuSafe(F&& f) {
    try {
        f();
    } catch (...) {
        if (!g_failed.exchange(true)) LOG_WARNING("[LightmapSmooth] Out of memory: smoothed light map off");
    }
}

// Rebuilds what the frame needs, from the draw hooks (inside the game's scene): every chunk in view whose own map
// changed (the same frame), atlas cells to refill, and once per frame chunks out of view / neighbour-border updates
// within kBackgroundMs of GPU time. Runs again in the same frame only when a new change was seen.
bool g_gpuBusy = false; // a pass is being drawn (our device calls run other modules' hooks: never re-enter)

void GpuService(IDirect3DDevice9* dev) {
    if (!dev || !g_gpuActive || g_gpuState != GpuState::Ready || g_gpuBusy) return;
    struct Busy {
        Busy() { g_gpuBusy = true; }
        ~Busy() { g_gpuBusy = false; }
    } busy;
    DrainNotices();
    const bool newFrame = g_servicedFrame != g_frame;
    const bool compare = !kPublicBuild && g_compareRequested.load(std::memory_order_relaxed);
    if (!newFrame && g_serviceEpoch == g_changeEpoch && !compare) return;
    g_servicedFrame = g_frame;
    g_serviceEpoch = g_changeEpoch;

    static std::vector<std::pair<const LightmapSmooth::Key, Entry>*> order;
    static std::vector<BuildItem> items;
    const std::vector<EntryNode*>& byUse = ByUse(); // most recently drawn first (a copy: the batch below may draw)
    order.assign(byUse.begin(), byUse.end());
    items.clear();
    int urgent = 0, cells = 0;
    for (auto* kv : order) {
        Entry& e = kv->second;
        if (!e.src) continue;
        const bool own = e.gBuiltVer != e.gver;
        if (own && e.gTryFrame == g_frame) continue; // failed this frame: the game's map is shown, next frame again
        POINT pt{};
        if (own && Visible(e) && urgent < kUrgentCap) {
            items.push_back({&kv->first, &e, 0});
            urgent++;
        } else if (!own && e.gtex && g_atlas && e.gAtlasSig != e.gBuiltSig && cells < kCellCap && AtlasCell(kv->first, pt)) {
            items.push_back({&kv->first, &e, 3});
            cells++;
        }
    }
    // Held while a rebuild is imminent (it re-renders every map anyway); neighbour borders wait for the end of a rebuild
    // sweep and for chunks to stop changing (as on the CPU path).
    const bool holding = g_frame < g_expectUntil;
    if (newFrame && !holding) {
        const bool settling = g_frame - g_lastChangeFrame < kSettleFrames;
        const double est = g_msPerChunk > 0.0f ? g_msPerChunk : kDefaultChunkMs;
        double spent = 0.0;
        for (int pass = 0; pass < 2; pass++) // own changes first (their cells hold an older map), then borders
            for (auto* kv : order) {
                Entry& e = kv->second;
                if (!e.src || e.gTryFrame == g_frame) continue;
                if (spent > 0.0 && spent + est > kBackgroundMs) break;
                if (std::any_of(items.begin(), items.end(), [&](const BuildItem& b) { return b.e == &e && b.kind != 3; })) continue;
                const bool own = e.gBuiltVer != e.gver;
                if (pass == 0 ? !own : (own || !e.gtex || g_sweepOn || settling || e.gBuiltSig == GpuSig(kv->first))) continue;
                items.push_back({&kv->first, &e, pass == 0 ? 1 : 2});
                spent += est;
            }
    }
    RunBatch(dev, items);
    if (compare && g_compareRequested.exchange(false)) g_compareResult = RunCompare(dev);
}

// Present, GPU path: timings, the fallback hash checks, sweep end, atlas growth. No drawing here.
void OnPresentGpu(IDirect3DDevice9* dev) {
    ReadTimings();
    {
        std::lock_guard<std::mutex> lk(g_mx); // a job the worker finished after a switch from the CPU path
        g_results.clear();
    }
    const std::vector<EntryNode*>& order = ByUse(); // most recently drawn first (nothing below draws or adds chunks)
    if (g_frame < g_boostUntil) { // after a kick / rebuild: 4 chunks in view per frame
        size_t nVisible = 0;
        while (nVisible < order.size() && Visible(order[nVisible]->second)) nVisible++;
        for (int i = 0; i < 4 && nVisible > 0; i++) {
            g_boostCursor = (g_boostCursor + 1) % nVisible;
            GpuHashCheck(order[g_boostCursor]->first, order[g_boostCursor]->second, "fast check");
        }
    }
    g_checkCursor = (g_checkCursor + 1) % g_entries.size();
    auto it = g_entries.begin();
    std::advance(it, g_checkCursor);
    GpuHashCheck(it->first, it->second, "round robin");
    EndSweepIfDone();
    EnsureAtlas(dev);
    g_drainedThisFrame.clear();
}

void ReleaseGpuChunks() {
    for (auto& [k, e] : g_entries) {
        if (e.gtex) e.gtex->Release();
        e.gtex = nullptr;
        e.gBuiltVer = 0;
        e.gBuiltSig = 0;
        e.gAtlasSig = 0;
    }
}

// The developer setting / GPU availability decide the path; a switch drops everything (the draws register the chunks
// again next frame and the new path rebuilds them).
void ResolveMode(IDirect3DDevice9* dev) {
    if (g_gpuPreferred && g_gpuState == GpuState::Unknown) InitGpu(dev);
    const bool gpu = g_gpuPreferred && g_gpuState == GpuState::Ready;
    if (gpu == g_gpuActive) return;
    LightmapSmooth::Clear();
    g_gpuActive = gpu;
    LOG_INFO(std::string("[LightmapSmooth] Smoothing on the ") + (gpu ? "GPU" : (g_gpuPreferred ? "CPU (GPU unavailable: " + g_gpuReason + ")" : "CPU (developer setting)")));
}

// Developer: builds the most recently drawn chunk whose maps can be read on both paths, reads it back (one-off, it stalls)
// and compares every level with the CPU path's result for the same maps.
std::string RunCompare(IDirect3DDevice9* dev) {
    try {
        std::vector<std::pair<const LightmapSmooth::Key, Entry>*> order;
        for (auto& kv : g_entries)
            if (kv.second.src) order.push_back(&kv);
        std::stable_sort(order.begin(), order.end(), [](auto* a, auto* b) { return a->second.lastUse > b->second.lastUse; });
        for (auto* kv : order) {
            Entry& e = kv->second;
            RawPtr raw[9];
            bool readable = true;
            for (int dz = -1; dz <= 1 && readable; dz++)
                for (int dx = -1; dx <= 1 && readable; dx++) {
                    auto it = g_entries.find(Neighbour(kv->first, dx, dz));
                    if (it == g_entries.end() || !it->second.src) continue; // missing: both paths clamp to the centre
                    uint64_t h = 0;
                    readable = ReadMap(it->second.src, 0, h, raw[(dz + 1) * 3 + (dx + 1)]) == 1;
                }
            if (!readable || !raw[4]) continue;
            const std::vector<BuildItem> one{{&kv->first, &e, 0}};
            RunBatch(dev, one);
            if (e.gBuiltVer != e.gver || !e.gtex) return "GPU build failed";
            Result r;
            Process(raw, r);
            if (r.levels.size() != kLevels) return "CPU smoothing failed";
            int maxDiff0[4] = {}, maxDiffMips = 0;
            size_t off1 = 0, offMore = 0;
            for (UINT l = 0; l < kLevels; l++) {
                const int size = kOut >> l;
                IDirect3DSurface9 *src = nullptr, *sys = nullptr;
                bool ok = SUCCEEDED(e.gtex->GetSurfaceLevel(l, &src)) && src &&
                          SUCCEEDED(dev->CreateOffscreenPlainSurface(size, size, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, nullptr)) && sys &&
                          SUCCEEDED(dev->GetRenderTargetData(src, sys));
                D3DLOCKED_RECT lr{};
                if (ok) ok = SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY));
                if (ok) {
                    for (int y = 0; y < size; y++) {
                        const auto* row = reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(lr.pBits) + static_cast<size_t>(y) * lr.Pitch);
                        for (int x = 0; x < size; x++) {
                            const uint32_t g = row[x], c = r.levels[l][static_cast<size_t>(y) * size + x];
                            int worst = 0;
                            for (int ch = 0; ch < 4; ch++) {
                                const int d = std::abs(static_cast<int>((g >> (8 * ch)) & 255) - static_cast<int>((c >> (8 * ch)) & 255));
                                worst = std::max(worst, d);
                                if (l == 0) maxDiff0[ch] = std::max(maxDiff0[ch], d); // ch: 0 B, 1 G, 2 R, 3 A
                            }
                            if (l == 0) {
                                off1 += worst == 1 ? 1 : 0;
                                offMore += worst > 1 ? 1 : 0;
                            } else
                                maxDiffMips = std::max(maxDiffMips, worst);
                        }
                    }
                    sys->UnlockRect();
                }
                if (sys) sys->Release();
                if (src) src->Release();
                if (!ok) return std::format("read back of level {} failed", l);
            }
            const std::string res = std::format("chunk ({}, {}): level 0 max diff R {} G {} B {} A {} (texels off by 1: {}, by more: {} of {}), levels 1-10 max diff {}",
                                                kv->first.first, kv->first.second, maxDiff0[2], maxDiff0[1], maxDiff0[0], maxDiff0[3], off1, offMore, kOut * kOut, maxDiffMips);
            LOG_INFO("[LightmapSmooth] GPU vs CPU, " + res);
            return res;
        }
        return "no chunk whose maps can be locked (managed DXT5) in view";
    } catch (...) {
        return "out of memory";
    }
}

} // namespace

namespace LightmapSmooth {

void SetEnabled(bool on) {
    if (g_enabled.exchange(on) != on && !on) Clear();
}

bool Enabled() { return g_enabled.load(std::memory_order_relaxed) && !g_failed.load(std::memory_order_relaxed); }

// The smoothed map only while it was built from the game's current map (correct first).
static IDirect3DTexture9* Current(const Entry& e) {
    if (g_gpuActive) return e.gtex && e.gBuiltVer == e.gver ? e.gtex : nullptr;
    return e.smooth && e.hash == e.doneHash ? e.smooth : nullptr;
}

IDirect3DTexture9* Get(const Key& key, IDirect3DTexture9* original) {
    if (!Enabled() || !original) return nullptr;
    const auto placed = g_entries.try_emplace(key);
    const auto it = placed.first;
    Entry& e = it->second;
    if (placed.second || e.lastUse != g_frame) NoteUseChanged(&*it); // ByUse() puts it back in order
    e.lastUse = g_frame;
    if (e.src != original) {
        const bool known = e.src != nullptr;
        if (e.src) e.src->Release();
        e.src = original;
        e.src->AddRef();
        e.hash = 0; // new texture: its map is unknown until read (the smoothed one is not shown meanwhile)
        if (g_gpuActive) {
            e.ghash = 0;
            e.gNoHash = false;
            GpuSafe([&] { BumpVersion(key, e, known ? "new texture" : "new chunk"); });
        }
    }
    if (g_gpuActive && g_dev) {
        // the smoothed map of the game's current map, rebuilt now if needed (this chunk is in view: same frame); one try
        // per frame (a failed build shows the game's map)
        GpuSafe([&] {
            GpuService(g_dev);
            if (e.gBuiltVer != e.gver && e.src && e.gTryFrame != g_frame && !g_gpuBusy) {
                const std::vector<BuildItem> one{{&it->first, &e, 0}};
                RunBatch(g_dev, one);
            }
        });
        if (!Enabled()) return nullptr;
    }
    return Current(e);
}

IDirect3DTexture9* Atlas(float c[4], bool forDraw) {
    if (!Enabled()) return nullptr;
    if (forDraw && g_gpuActive && g_dev) { // cells of chunks that just changed, before this draw reads them
        GpuSafe([] { GpuService(g_dev); });
        if (!Enabled()) return nullptr;
    }
    if (!g_atlas || g_atlasChunks == 0) return nullptr;
    // uv = world xz * c.xy + c.zw; the atlas starts at chunk (minX, minZ), i.e. world 256 * min
    const float sx = 1.0f / static_cast<float>(g_atlasW * kChunkSize), sz = 1.0f / static_cast<float>(g_atlasH * kChunkSize);
    c[0] = sx;
    c[1] = sz;
    c[2] = -static_cast<float>(g_atlasMinX * kChunkSize) * sx;
    c[3] = -static_cast<float>(g_atlasMinZ * kChunkSize) * sz;
    return g_atlas;
}

IDirect3DTexture9* Find(const Key& key) {
    if (!Enabled()) return nullptr;
    if (g_gpuActive && g_dev) {
        GpuSafe([] { GpuService(g_dev); });
        if (!Enabled()) return nullptr;
    }
    auto it = g_entries.find(key);
    return it == g_entries.end() ? nullptr : Current(it->second);
}

void NoteKick(const char* reason) {
    g_haveKick = true;
    g_kickTime = Clock::now();
    g_kickReason = reason ? reason : "";
    g_boostUntil = std::max(g_boostUntil, g_frame + 60);
}

void OnTerrainRebuilt() {
    g_haveConsume = true;
    g_consumeTime = Clock::now();
    g_kickToConsumeMs = g_haveKick ? MsSince(g_kickTime) : -1.0;
    std::vector<Key> rendered;
    {
        std::lock_guard<std::mutex> lk(g_noticeMx);
        rendered = g_noticeKeys; // re-rendered in this very frame (before this call): not awaiting
    }
    rendered.insert(rendered.end(), g_drainedThisFrame.begin(), g_drainedThisFrame.end()); // GPU path: taken by a draw already
    for (auto& [k, e] : g_entries) e.awaiting = std::find(rendered.begin(), rendered.end(), k) == rendered.end();
    g_sweepOn = true;
    g_sweepUntil = g_frame + kSweepFrames;
    g_boostUntil = std::max(g_boostUntil, g_frame + kSweepFrames);
    g_sweepChanges = 0;
    g_sweepFirstMs = g_sweepLastMs = -1.0;
    g_expectUntil = 0; // the rebuild is here: chunks re-rendered from now on are smoothed at once
}

void ExpectRebuild(int frames) { g_expectUntil = std::max(g_expectUntil, g_frame + static_cast<uint32_t>(std::max(frames, 0))); }

void NoteChunkRendered(int ix, int iz) {
    try {
        std::lock_guard<std::mutex> lk(g_noticeMx);
        if (g_noticeKeys.size() < 256) g_noticeKeys.push_back({ix * kChunkSize + kChunkSize / 2, iz * kChunkSize + kChunkSize / 2});
        g_noticePending.store(true);
    } catch (...) {
    }
}

static void OnPresentBody(IDirect3DDevice9* dev) {
    g_dev = dev;
    if (Enabled()) ResolveMode(dev);
    if (g_gpuActive) {
        DrainNotices();
        if (!Enabled() || g_entries.empty()) {
            g_drainedThisFrame.clear();
            return;
        }
        g_frame++;
        OnPresentGpu(dev);
        return;
    }
    std::vector<Key> notices;
    {
        std::lock_guard<std::mutex> lk(g_noticeMx);
        notices.swap(g_noticeKeys);
    }
    if (!Enabled() || g_entries.empty()) return;
    EnsureWorker();
    g_frame++;
    // Chunks in view first: order by the last frame a draw asked for them (then by key, for a stable order).
    static std::vector<std::pair<const Key, Entry>*> order;
    const std::vector<EntryNode*>& byUse = ByUse(); // a copy: the steps below read and upload chunks
    order.assign(byUse.begin(), byUse.end());
    auto visible = [](const Entry& e) { return e.lastUse + 2 >= g_frame; };

    // 1. Chunks the game just re-rendered (notice from its per-chunk texture render): read now.
    for (const Key& k : notices) {
        auto it = g_entries.find(k);
        if (it == g_entries.end()) continue;
        g_notices++;
        it->second.awaiting = false;
        CheckEntry(k, it->second, "render notice");
    }
    // 2. New textures first (up to 4 per frame).
    int reads = 0;
    for (auto* kv : order) {
        if (kv->second.hash == 0 && reads < 4 && CheckEntry(kv->first, kv->second, "new texture")) reads++;
    }
    // 3. After a kick / rebuild: 4 chunks in view per frame, round robin.
    if (g_frame < g_boostUntil) {
        size_t nVisible = 0;
        while (nVisible < order.size() && visible(order[nVisible]->second)) nVisible++;
        for (int i = 0; i < 4 && nVisible > 0; i++) {
            g_boostCursor = (g_boostCursor + 1) % nVisible;
            auto* kv = order[g_boostCursor];
            if (kv->second.hash > 1) CheckEntry(kv->first, kv->second, "fast check");
        }
    }
    // 4. One round-robin check of any chunk per frame (changes outside a rebuild, chunks out of view).
    if (reads == 0) {
        g_checkCursor = (g_checkCursor + 1) % g_entries.size();
        auto it = g_entries.begin();
        std::advance(it, g_checkCursor);
        if (it->second.hash > 1) CheckEntry(it->first, it->second, "round robin");
    }
    EndSweepIfDone();
    EnsureAtlas(dev);

    // 5. Plain copies of changed maps into the atlas (correct at once), chunks in view first, within a time budget.
    if (g_atlas) {
        const auto t0 = Clock::now();
        int tried = 0;
        for (auto* kv : order) {
            Entry& e = kv->second;
            if (e.hash <= 1 || e.rawHash != e.hash || e.atlasHash == e.hash) continue;
            if (tried++ > 0 && MsSince(t0) > kRawBudgetMs) break;
            const int copied = AtlasRawCopy(dev, kv->first, e);
            if (copied == 0) break; // no staging free: next frame
            if (copied < 0) continue;
            e.atlasHash = e.hash;
            e.atlasSmooth = false;
            g_rawCopies++;
        }
    }

    // 6. Queue smoothing: at most kMaxInFlight jobs, chunks in view first, own changes before neighbour borders.
    //    Held while a rebuild is imminent; during a rebuild sweep only chunks already re-rendered; neighbour re-smooths
    //    only once chunks stopped changing (each job reads the latest neighbours when it starts anyway).
    const bool holding = g_frame < g_expectUntil;
    const bool settling = g_frame - g_lastChangeFrame < kSettleFrames;
    while (!holding && g_inFlight < kMaxInFlight) {
        std::pair<const Key, Entry>* pick = nullptr;
        bool pickOwn = false;
        for (auto* kv : order) {
            Entry& e = kv->second;
            if (e.inFlight || (!e.dirty && !e.borderDirty)) continue;
            if (e.hash <= 1 || !e.raw || e.rawHash != e.hash) continue; // map not read yet (or unreadable)
            const bool own = e.dirty && !(g_sweepOn && e.awaiting);
            const bool border = e.borderDirty && !g_sweepOn && !settling;
            if (!own && !border) continue;
            // inputs identical to those of the smoothed map (and the atlas already holds it): nothing to do
            if (e.smooth && CurrentSig(kv->first) == e.doneSig && !(e.atlasHash == e.hash && !e.atlasSmooth && g_atlas)) {
                e.dirty = e.borderDirty = false;
                g_skippedSame++;
                continue;
            }
            if (own && !pickOwn) {
                pick = kv;
                pickOwn = true;
                break; // order is by visibility: the first own change wins
            }
            if (!pick) pick = kv;
        }
        if (!pick) break;
        Entry& e = pick->second;
        e.dirty = e.borderDirty = false;
        e.inFlight = true;
        e.gen = ++g_genCounter;
        g_inFlight++;
        {
            std::lock_guard<std::mutex> lk(g_mx);
            g_jobs.push_back(Job{pick->first, e.gen});
        }
        g_cv.notify_one();
    }

    // 7. Upload one finished map per frame.
    Result r;
    bool have = false;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        if (!g_results.empty()) {
            r = std::move(g_results.front());
            g_results.erase(g_results.begin());
            have = true;
        }
    }
    if (have && Upload(dev, r) == UploadResult::Later) {
        std::lock_guard<std::mutex> lk(g_mx);
        g_results.insert(g_results.begin(), std::move(r)); // staging still busy: same result next frame
        have = false;
    }
    if (have) g_cv.notify_one(); // room for the next finished map
}

void OnPresent(IDirect3DDevice9* dev) {
    try {
        if (g_failed && (!g_entries.empty() || g_atlas)) Clear(); // out of memory inside a draw (GPU path): released here
        OnPresentBody(dev);
    } catch (...) { // out of memory: turn the feature off instead of taking the game down
        LOG_WARNING("[LightmapSmooth] Out of memory: smoothed light map off");
        g_failed = true;
        Clear();
    }
}

void OnPreReset(IDirect3DDevice9*) {
    ReleaseAtlas();
    ReleasePool();
    for (auto& [k, e] : g_entries) {
        if (e.smooth) {
            e.smooth->Release();
            e.smooth = nullptr;
        }
        e.doneHash = e.doneSig = 0;
        e.dirty = e.raw != nullptr && e.rawHash == e.hash; // rebuilt after the reset (the atlas gets plain copies first)
    }
    // GPU path: every render target goes (chunk maps, scratch, atlas) and the queries; the chunks are rebuilt by the
    // first draws after the reset (in view at once, the rest within the per-frame budget). The shaders stay.
    ReleaseGpuChunks();
    ReleaseGpuRes();
    ReleaseTimings();
    g_servicedFrame = UINT32_MAX;
    g_changeEpoch++;
}

void Clear() {
    {
        std::lock_guard<std::mutex> lk(g_mx);
        g_jobs.clear();
        g_results.clear();
        g_shared.clear();
    }
    g_cv.notify_all();
    {
        std::lock_guard<std::mutex> lk(g_noticeMx);
        g_noticeKeys.clear();
    }
    g_noticePending = false;
    g_drainedThisFrame.clear();
    ReleaseGpuChunks();
    ReleaseGpuRes();
    ReleaseTimings();
    g_servicedFrame = UINT32_MAX;
    for (auto& [k, e] : g_entries) {
        if (e.src) e.src->Release();
        if (e.smooth) e.smooth->Release();
    }
    g_entries.clear();
    ForgetByUse();
    g_checkCursor = g_boostCursor = 0;
    g_inFlight = 0;
    g_sweepOn = false;
    ReleaseAtlas();
    ReleasePool();
    ReleaseStaging();
    std::vector<uint32_t>().swap(g_decodeBuf);
}

static void AppendTimingStatus(std::string& s, int awaiting) {
    if (g_frame < g_expectUntil) s += " | holding: rebuild coming";
    if (g_sweepOn) s += std::format(" | rebuild sweep: {} chunks not re-rendered yet", awaiting);
    if (g_haveKick) s += std::format(" | last kick: {} ({:.1f} s ago), last change {:.0f} ms after it", g_kickReason, MsSince(g_kickTime) / 1000.0, g_lastChangeSinceKickMs);
    s += " | last sweep: " + g_lastSweep;
}

std::string Status() {
    if (!Enabled()) return "off";
    if (g_gpuActive) {
        int ready = 0, own = 0, border = 0, awaiting = 0;
        for (auto& [k, e] : g_entries) {
            if (Current(e)) ready++;
            if (e.src && e.gBuiltVer != e.gver) own++;
            else if (e.gtex && e.gBuiltSig != GpuSig(k)) border++;
            if (e.awaiting) awaiting++;
        }
        std::string s = std::format("GPU ({} intermediates) | chunks smoothed: {} of {} | waiting: {} (neighbour borders {}) | built: {} (in view {}, out of view {}, "
                                    "borders {}), failed {} | atlas cells written: {} | GPU time: {} per chunk, last batch {}, max batch {:.2f} ms ({} timed) | "
                                    "changes seen: {} (render notices {}, same map {}), hash checks {} | world map: {}x{} chunks ({} copies, grown {}x)",
                                    FloatFmtName(), ready, g_entries.size(), own, border, g_gpuBuilds, g_gpuInView, g_gpuOutOfView, g_gpuBorders, g_gpuFailures,
                                    g_gpuCells, g_msPerChunk >= 0 ? std::format("{:.3f} ms", g_msPerChunk) : std::string("not measured"),
                                    g_msLastBatch >= 0 ? std::format("{:.3f} ms", g_msLastBatch) : std::string("-"), g_msBatchMax, g_timedBatches, g_changes, g_notices,
                                    g_sameMapNotices, g_gpuHashChecks, g_atlasW, g_atlasH, g_atlasChunks, g_atlasGrowths);
        AppendTimingStatus(s, awaiting);
        if (!kPublicBuild) s += " | GPU vs CPU: " + g_compareResult;
        return s;
    }
    int ready = 0, pending = 0, awaiting = 0;
    for (auto& [k, e] : g_entries) {
        if (Current(e)) ready++;
        if (e.dirty || e.borderDirty || e.inFlight) pending++;
        if (e.awaiting) awaiting++;
    }
    int jobs = 0;
    double sum = 0.0;
    float mx = 0.0f;
    size_t queued = 0;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        jobs = g_jobsDone;
        sum = g_jobMsSum;
        mx = g_jobMsMax;
        queued = g_jobs.size() + g_results.size();
    }
    std::string s = g_gpuPreferred && g_gpuState == GpuState::Failed ? "CPU (GPU unavailable: " + g_gpuReason + ") | " : std::string("CPU | ");
    s += std::format("chunks smoothed: {} of {} | waiting: {} (in flight {}, queue {}) | jobs: {}, avg {:.1f} ms, max {:.1f} ms | skipped (inputs unchanged): {} | "
                                "uploaded: {} (later {}, outdated {}) | plain ground copies: {} | changes seen: {} (render notices {}) | unreadable: {} | "
                                "world map: {}x{} chunks ({} copies, grown {}x)",
                                ready, g_entries.size(), pending, g_inFlight, queued, jobs, jobs ? sum / jobs : 0.0, mx, g_skippedSame, g_uploaded, g_deferredUploads,
                                g_staleResults, g_rawCopies, g_changes, g_notices, g_unreadable, g_atlasW, g_atlasH, g_atlasChunks, g_atlasGrowths);
    AppendTimingStatus(s, awaiting);
    return s;
}

void SetGpuPreferred(bool on) { g_gpuPreferred = on; } // applied at the next OnPresent (ResolveMode)

bool GpuActive() { return g_gpuActive; }

void RequestCompare() {
    if (!kPublicBuild) {
        if (!g_gpuActive) {
            g_compareResult = "only on the GPU path";
            return;
        }
        g_compareResult = "waiting for the next ground draw";
        g_compareRequested = true;
    }
}

std::string CompareStatus() { return g_compareResult; }

} // namespace LightmapSmooth
