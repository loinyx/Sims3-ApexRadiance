// Outdoor lamp edits by delta (runs with Lamp lighting > Instant; see outdoor_delta.h).
//
// ---- The game side (Steam 1.67.2, read with dumpbin 13/10) ----
//   The budgeted room solve FUN_006a3c90 resumes at room+0xEC: 0 state 0 (0x006A18B0), 1 the map lock step 0x0069FA40 (call
//   at 0x006A3D26; "test al,al; je 0x006A3E50", where +0x638 = 0 and +0xEC = 8: the finalize 0x006A0E00 is next), 2 the
//   wall pass 0x006A3A30 (one FUN_006a31d0 per wall piece into the wall atlas, its lock record room+0x680, then the class-2
//   blur 0x0069F650), 3..7 the passes 0x006A3B80 on +0x180, +0x270, +0x360, +0x450, +0x540 (their batches write through
//   pass+0x44: room+0x1C4, +0x2B4, +0x3A4, +0x494, +0x584; an outdoor room skips step 5), 8 the finalize (class-2 floor and
//   ceiling tile edges copied, 0x0069EFC0 -> 0x006A99B0, plain copies; the maps unlocked; +0xEC = 9).
//   FUN_006a31d0 thiscall(room, vector* samples, {base, pitch}* map, char flags[2], sampler, char) ret 0x14: when flags[0] or
//   flags[1] and 4 samples or more, the batch centre 0x0069F1E0 and the per-light wall and 3D lists 0x006A30B0 / 0x006A3140
//   over the room's light list; per sample LightPointWithAllLights 0x0069FD60 (call at 0x006A3336, LevelLightShare's
//   SolvePointBatch), which zeroes the sum, then for each light i of room+0xC8..+0xCC evaluates it (vfunc+0x4C), drops it
//   under the threshold +0x63C, tests it against list2D[i] / list3D[i] and adds t x colour to the sum before the next light;
//   an outdoor room (+0x18) packs min(sum, 1) with the floor mask's alpha (0x006A333B) at the sample's (+0x20, +0x22).
//   The lock records (10 dwords: +0 bits, +4 pitch, +0x18 width, +0x1C height) and the class's texture slot
//   0x011D0460 + class x 16 (wall 3, floor 0, ceiling 1, objects 2; [wrapper+0x30] the texture) as GpuLamps reads them.
//
// Part of Apex Radiance. Credits: @loinyx

#include "high_alloc.h"
#include "fast_math.h"
#include "outdoor_delta.h"
#include "outdoor_delta_model.h"
#include "rt_lamp_model.h"
#include "rt_outdoor_lamps.h"
#include "level_light_share.h"
#include "room_light_queue.h"
#include "room_map_dither.h"
#include "lamp_mark_filter.h"
#include "apex_log.h"
#include "game_addresses.h"
#include "hook_guard.h"
#include "dev_switches.h"
#include "apex_memory.h"
#include <bit>
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace OutdoorDelta {
namespace {

using namespace OutdoorDeltaModel;

constexpr uint32_t kMapDesc[4] = {0x680, 0x1C4, 0x2B4, 0x3A4}; // walls, floor, ceiling, objects (room+0xEC - 2)
constexpr int kSlotIdx[4] = {3, 0, 1, 2};
constexpr const char* kMapName[4] = {"wall", "floor", "ceiling", "object"};
// Game addresses (Steam 1.67.2 values; GameAddr on every build, required by LevelLightShare::DeltaReady)
uintptr_t BatchVec() { return GameAddr::Get(GameAddr::Id::BatchSamples); } // 0x01158AC8: the batch vector the solve's callers pass
uintptr_t Slots() { return GameAddr::Get(GameAddr::Id::MapSlots); }        // 0x011D0460: the per-class room map slots
uintptr_t Pack() { return GameAddr::Get(GameAddr::Id::TexelPack); }        // 0x0060CD10: the texel pack
constexpr int kMaxChanged = 8;                    // more changed lights: solved in full
// (14/10: 160 MB, 256 MB pinned; with the game's saves failing on a fragmented address space below 2 GB, the parts are
// kept above 2 GB (HighAllocator) and within a lower budget)
constexpr size_t kMaxBytes = size_t{96} << 20;    // every room's kept parts together; past it the oldest go
constexpr size_t kMaxRecordBytes = size_t{96} << 20;
// past kMaxBytes only the pinned rooms (Real-time outdoor lamps' active lot) are left. 14/10: 160 MB, with the drawn lamps'
// parts of those rooms kept as one sum per texel (CollapseDrawn), which is what Real-time outdoor lamps reads; a pinned room
// never drops another pinned room (that made the active lot's stories evict one another in a loop: dropped, sent to their
// solve, kept again, dropping the next): past this ceiling the new pinned solve is not kept instead
constexpr size_t kMaxPinnedBytes = size_t{160} << 20;
constexpr size_t kTightBytes = kMaxPinnedBytes / 100 * 85; // the budget is logged as tight past this (once per crossing)
constexpr DWORD kCheckDelayMs = 1500;
constexpr DWORD kEditWindowMs = 5000;   // a room-0 solve this long after a lamp edit belongs to it (the log)
constexpr DWORD kSummaryQuietMs = 400;  // a lot's edit summary is written once its room 0s were quiet this long
constexpr uint16_t kNoBlock = 0xFFFF, kBadBlock = 0xFFFE;

std::atomic<bool> g_on{false}, g_check{false}, g_clear{false};
std::atomic<uint32_t> g_gen{1};
std::atomic<uint32_t> g_version{1}; // Record::version's source
std::atomic<const void*> g_deltaRoom{nullptr};
std::atomic<long> c_deltas{0}, c_texels{0}, c_batches{0}, c_captures{0}, c_captureFails{0}, c_full{0}, c_checks{0}, c_checkBad{0};
std::atomic<long> c_checkMax{-1}, c_records{0};
std::atomic<long> c_gpuSkips{0}; // deltas left out because Real-time outdoor lamps draws every changed light
std::atomic<long> c_leftLotDrops{0}; // (left_lot_release) records of rooms whose lot left, given back
std::atomic<size_t> c_leftLotBytes{0};
std::atomic<long> c_budgetDrops{0}; // records the memory budget dropped (the oldest unpinned first)
std::atomic<size_t> g_bytes{0};
std::atomic<float> g_lastMs{0.0f}, g_maxMs{0.0f};

// ---- what is kept of a room-0 solve ----
struct LampRec {
    uintptr_t light = 0;
    uint64_t sig = 0;
    int mult = 0, type = 0;
    // Real-time outdoor lamps (rt_outdoor_lamps.h): the light was a lamp the GPU draws when its parts were first kept
    // (RtLampModel::Drawn: a lot lamp of the room's lot, outdoors); its parts leave the texel's base
    bool drawn = false;
    // (14/10) its parts are only in the record's drawn sum (CollapseDrawn): a delta that needs them solves in full
    bool collapsed = false;
    std::vector<Contrib, ApexMem::HighAllocator<Contrib>> parts; // by texel, ascending
};
// The collapsed drawn lamps' parts of a texel together (CollapseDrawn): the base Real-time outdoor lamps relights from is the
// texel's sum less this (and less the parts of drawn lamps kept one by one)
struct DrawnSum {
    float rgb[3] = {};
    float vis = 0.0f, all = 0.0f;
};
struct BatchRec {
    uint32_t first = 0, count = 0;
    char flags[2] = {};
    float c[3] = {}, r = 0.0f;
};
enum : uint8_t { kMasked = 1, kGated = 2, kDup = 4 }; // kDup: another sample of the solve writes the same texel
struct TexelRec {
    Totals t;
    uint32_t packed = 0; // what the map holds (a wall at class 2: before the blur)
    uint16_t x = 0, y = 0;
    uint8_t map = 0, bits = 0;
    uint16_t block = kNoBlock;
};
struct BlockRec {
    int x0 = 0, y0 = 0, x1 = -1, y1 = -1;
    uint32_t off = 0; // into preblur
    bool ok = false;
};
struct MapGeo {
    bool used = false;
    uintptr_t tex = 0;
    uint32_t pitch = 0, w = 0, h = 0;
};
struct Record {
    uint32_t version = 0; // (Real-time outdoor lamps) changes whenever the kept parts or texels change
    // (Real-time outdoor lamps) the texels without the drawn lamps' parts: its hash, and a version that changes only with it
    // (a solve or a delta that changed only drawn lamps keeps it: the GPU's relit maps stand as they are)
    uint64_t baseHash = 0;
    uint32_t baseVersion = 0;
    uintptr_t room = 0, mgr = 0;
    int story = 0, cls = 0;
    uint64_t lot = 0;
    uint32_t gen = 0;
    uint64_t physics = 0, geo = 0;
    bool dither = false;
    float thr = 0.0f;
    uint8_t wallMode = 0;
    MapGeo maps[4];
    uint32_t passes = 0;  // the class-2 wall blur (0: none)
    bool wallsOk = true;  // the walls were blurred by Apex's replay of the game's blur (or not at all)
    std::vector<uint8_t, ApexMem::HighAllocator<uint8_t>> samples; // 0x30 per texel, in solve order (above 2 GB when large)
    std::vector<TexelRec, ApexMem::HighAllocator<TexelRec>> texels;
    std::vector<BatchRec> batches;
    std::vector<LampRec> lamps;
    std::vector<BlockRec> blocks;
    std::vector<uint32_t, ApexMem::HighAllocator<uint32_t>> preblur;
    std::vector<DrawnSum, ApexMem::HighAllocator<DrawnSum>> drawnSum; // by texel, while lamps are collapsed (else empty)
    DWORD used = 0;
    size_t bytes = 0;
    uint8_t gone = 0; // (left_lot_release) liveness sweeps in a row that found no loaded lot holding its story manager
};
std::recursive_mutex g_mx; // g_records (the light tree thread uses them, the render thread clears them)
std::unordered_map<uint64_t, std::unique_ptr<Record>> g_records;
std::vector<const void*> g_pinned; // (under g_mx) rooms dropped last by the budget
std::atomic<long> c_pinnedDrops{0};
bool PinnedLocked(uintptr_t room) { return std::find(g_pinned.begin(), g_pinned.end(), reinterpret_cast<const void*>(room)) != g_pinned.end(); }
uint64_t KeyOf(const void* room, int cls) { return (static_cast<uint64_t>(reinterpret_cast<uintptr_t>(room)) << 2) | static_cast<uint64_t>(cls & 3); }
size_t BytesOf(const Record& r) {
    size_t b = sizeof(Record) + r.samples.capacity() + r.texels.capacity() * sizeof(TexelRec) + r.batches.capacity() * sizeof(BatchRec) +
               r.blocks.capacity() * sizeof(BlockRec) + r.preblur.capacity() * 4 + r.drawnSum.capacity() * sizeof(DrawnSum);
    for (const LampRec& l : r.lamps) b += sizeof(LampRec) + l.parts.capacity() * sizeof(Contrib);
    return b;
}
// A record's buffers at their exact size (BytesOf counts capacity, and growth leaves up to half a buffer unused). A buffer
// HighAllocator keeps above 2 GB stays where it is when its exact size would fall under kHighFrom: it would move to the heap
// below 2 GB, which the game's saves need.
template <class T> bool WouldLeaveHigh(const std::vector<T, ApexMem::HighAllocator<T>>& v, size_t n) {
    return v.capacity() * sizeof(T) >= ApexMem::kHighFrom && n * sizeof(T) < ApexMem::kHighFrom;
}
template <class T> bool WouldLeaveHigh(const std::vector<T>&, size_t) { return false; }
template <class V> void ShrinkExact(V& v) noexcept {
    if (v.capacity() == v.size() || WouldLeaveHigh(v, v.size())) return;
    try {
        v.shrink_to_fit();
    } catch (...) {
        // (no memory for the exact copy: the buffer stays as it is)
    }
}
// A new solve's record before it is kept: every buffer but the drawn lamps' parts, which CollapseDrawn weighs by their
// capacity first (ShrinkDrawnParts after it)
void ShrinkRecord(Record& r) {
    ShrinkExact(r.samples), ShrinkExact(r.texels), ShrinkExact(r.batches), ShrinkExact(r.blocks), ShrinkExact(r.preblur);
    for (LampRec& l : r.lamps)
        if (!l.drawn) ShrinkExact(l.parts);
}
void ShrinkDrawnParts(Record& r) {
    for (LampRec& l : r.lamps)
        if (l.drawn) ShrinkExact(l.parts);
}
// (14/10) The drawn lamps' parts of a room Real-time outdoor lamps relights, as one sum per texel: the GPU draws those lamps
// and reads only the base (the sum less them), and a delta of only such lamps changes nothing kept (RtOutdoorLamps covers the
// room). Exact: the same parts subtracted from the same sums. A delta that would need one lamp's parts back (the room no
// longer covered, or a drawn and a baked light changed together) solves in full and keeps the parts again.
void CollapseDrawn(Record& r) {
    size_t drawnBytes = 0;
    for (const LampRec& l : r.lamps)
        if (l.drawn && !l.collapsed) drawnBytes += l.parts.capacity() * sizeof(Contrib);
    // (only when it saves memory: the sum costs sizeof(DrawnSum) a texel once)
    if (!drawnBytes || (r.drawnSum.size() != r.texels.size() && drawnBytes <= r.texels.size() * sizeof(DrawnSum))) return;
    try {
        if (r.drawnSum.size() != r.texels.size()) r.drawnSum.assign(r.texels.size(), DrawnSum{});
    } catch (...) {
        return; // (no memory for the sum: the parts stay as they are)
    }
    for (LampRec& l : r.lamps) {
        if (!l.drawn || l.collapsed) continue;
        for (const Contrib& p : l.parts)
            if (p.texel < r.drawnSum.size()) {
                DrawnSum& d = r.drawnSum[p.texel];
                for (int k = 0; k < 3; k++) d.rgb[k] += p.rgb[k];
                d.vis += FromHalf(p.vis);
                d.all += FromHalf(p.all);
            }
        decltype(l.parts)().swap(l.parts);
        l.collapsed = true;
    }
}
size_t PartsBytes(const Record& r, bool drawn) {
    size_t b = 0;
    for (const LampRec& l : r.lamps)
        if (l.drawn == drawn) b += l.parts.capacity() * sizeof(Contrib);
    return b;
}

// The base the GPU relights from (Real-time outdoor lamps): the texels' layout, their sums without the drawn lamps' parts
// (to 1 / 8192: the same solve evaluated again matches), the lights left baked, and the wall blocks' texels the solve did not
// write (light thread, at the end of a solve or a delta)
uint64_t BaseHash(const Record& r) {
    const size_t n = r.texels.size();
    std::vector<float> b(n * 5);
    for (size_t i = 0; i < n; i++) {
        const Totals& t = r.texels[i].t;
        b[i * 5 + 0] = t.S[0], b[i * 5 + 1] = t.S[1], b[i * 5 + 2] = t.S[2], b[i * 5 + 3] = t.vis, b[i * 5 + 4] = t.all;
    }
    std::vector<uintptr_t> kept;
    for (size_t i = 0; i < n && i < r.drawnSum.size(); i++) {
        const DrawnSum& d = r.drawnSum[i];
        for (int k = 0; k < 3; k++) b[i * 5 + k] -= d.rgb[k];
        b[i * 5 + 3] -= d.vis;
        b[i * 5 + 4] -= d.all;
    }
    for (const LampRec& l : r.lamps) {
        if (!l.drawn) {
            kept.push_back(l.light);
            continue;
        }
        for (const Contrib& p : l.parts)
            if (p.texel < n) {
                for (int k = 0; k < 3; k++) b[p.texel * 5 + k] -= p.rgb[k];
                b[p.texel * 5 + 3] -= FromHalf(p.vis);
                b[p.texel * 5 + 4] -= FromHalf(p.all);
            }
    }
    std::sort(kept.begin(), kept.end());
    uint64_t h = 1469598103934665603ull;
    const auto mix = [&h](uint64_t v) { h = (h ^ v) * 1099511628211ull; };
    mix(n), mix(static_cast<uint64_t>(r.cls)), mix(r.dither ? 1 : 0), mix(static_cast<uint64_t>(r.passes)), mix(r.wallsOk ? 1 : 0);
    mix(std::bit_cast<uint32_t>(r.thr)), mix(r.wallMode); // (the threshold and the wall mode the texels were solved with)
    for (size_t i = 0; i < n; i++) {
        const TexelRec& t = r.texels[i];
        mix(static_cast<uint64_t>(t.x) | static_cast<uint64_t>(t.y) << 16 | static_cast<uint64_t>(t.map) << 32 | static_cast<uint64_t>(t.bits) << 40);
        mix(static_cast<uint64_t>(t.packed >> 24)); // (the alpha of a texel that is not masked)
        for (int k = 0; k < 5; k++) mix(static_cast<uint64_t>(static_cast<int64_t>(std::lround(static_cast<double>(b[i * 5 + k]) * 8192.0))));
    }
    for (uintptr_t L : kept) mix(L);
    if (!r.preblur.empty() && r.maps[0].w && r.maps[0].h) {
        std::vector<uint8_t> written(static_cast<size_t>(r.maps[0].w) * r.maps[0].h, 0);
        for (const TexelRec& t : r.texels)
            if (t.map == 0 && t.x < r.maps[0].w && t.y < r.maps[0].h) written[static_cast<size_t>(t.y) * r.maps[0].w + t.x] = 1;
        for (const BlockRec& bl : r.blocks) {
            mix(static_cast<uint64_t>(bl.x0) | static_cast<uint64_t>(bl.y0) << 16 | static_cast<uint64_t>(bl.x1) << 32 | static_cast<uint64_t>(bl.y1) << 48);
            const int bw = bl.x1 - bl.x0 + 1;
            for (int y = bl.y0; y <= bl.y1; y++)
                for (int x = bl.x0; x <= bl.x1; x++) {
                    const size_t src = bl.off + static_cast<size_t>(y - bl.y0) * bw + (x - bl.x0);
                    const bool in = x >= 0 && y >= 0 && x < static_cast<int>(r.maps[0].w) && y < static_cast<int>(r.maps[0].h);
                    if (in && !written[static_cast<size_t>(y) * r.maps[0].w + x] && src < r.preblur.size()) mix(r.preblur[src]);
                }
        }
    }
    return h;
}
void EraseLocked(uint64_t key) {
    const auto it = g_records.find(key);
    if (it == g_records.end()) return;
    g_bytes.fetch_sub(std::min(g_bytes.load(), it->second->bytes));
    g_records.erase(it);
    c_records.store(static_cast<long>(g_records.size()));
}
void ClearLocked() {
    g_records.clear();
    g_bytes.store(0);
    c_records.store(0);
}

// ---- game reads (POD, SEH) ----
struct RoomRead {
    uintptr_t mgr = 0;
    int id = -1, cls = -1, story = -99, ec = -1;
    bool outdoor = false;
    uint64_t lot = 0;
    float thr = 0.0f;
    uint8_t wallMode = 0;
};
bool ReadRoom(const BYTE* room, RoomRead& r) {
    __try {
        r.mgr = *reinterpret_cast<const uintptr_t*>(room);
        r.id = *reinterpret_cast<const int*>(room + 0xC);
        r.cls = *reinterpret_cast<const int*>(room + 0xF4);
        r.ec = *reinterpret_cast<const int*>(room + 0xEC);
        r.outdoor = room[0x18] != 0;
        r.thr = *reinterpret_cast<const float*>(room + 0x63C);
        r.wallMode = room[0x639];
        if (!r.mgr) return false;
        r.story = *reinterpret_cast<const int*>(r.mgr + 0x88);
        r.lot = *reinterpret_cast<const uint32_t*>(r.mgr + 0x90) | static_cast<uint64_t>(*reinterpret_cast<const uint32_t*>(r.mgr + 0x94)) << 32;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
int ReadList(const BYTE* room, uintptr_t* out, int max) {
    __try {
        const uintptr_t* b = *reinterpret_cast<const uintptr_t* const*>(room + 0xC8);
        const uintptr_t* e = *reinterpret_cast<const uintptr_t* const*>(room + 0xCC);
        if (!b || e < b || e - b > max) return -1;
        int n = 0;
        for (const uintptr_t* p = b; p < e; p++) out[n++] = *p;
        return n;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
// A light as its evaluation reads it: a signature of every value the room solve takes (LampMarkFilter's Signature, the cone
// and spill of spots and shaded lamps, and its position as the wall test asks it, vfunc+0x24), and what the reach bound needs
struct LightNow {
    uint64_t sig = 0;
    int type = 0;
    float head[3] = {}, range = 0.0f, intensity = 0.0f, esum = 0.0f, spill = 1.0f;
};
using LightPos_t = void*(__thiscall*)(void* light, float* out);
inline void Mix(uint64_t& h, uint32_t v) { h = (h ^ v) * 1099511628211ull; }
bool ReadLight(uintptr_t L, LightNow& r) {
    if (!L) return false;
    __try {
        const BYTE* p = reinterpret_cast<const BYTE*>(L);
        uint64_t h = 1469598103934665603ull;
        r.type = *reinterpret_cast<const int*>(p + 0xB0);
        Mix(h, static_cast<uint32_t>(r.type));
        for (int k = 0; k < 4; k++) Mix(h, *reinterpret_cast<const uint32_t*>(p + 0x10 + k * 4));
        for (int k = 0; k < 8; k++) Mix(h, *reinterpret_cast<const uint32_t*>(p + 0xC0 + k * 4));
        for (int k = 0; k < 8; k++) Mix(h, *reinterpret_cast<const uint32_t*>(p + 0xE0 + k * 4));
        Mix(h, p[0x100]);
        for (int k = 0; k < 4; k++) Mix(h, *reinterpret_cast<const uint32_t*>(p + 0x120 + k * 4));
        Mix(h, *reinterpret_cast<const uint32_t*>(p + 0x130));
        if (r.type == 4 || r.type == 5)
            for (int k = 0; k < 24; k++) Mix(h, *reinterpret_cast<const uint32_t*>(p + 0x150 + k * 4));
        alignas(16) float pos[4] = {};
        const void* vt = *reinterpret_cast<void* const*>(p);
        reinterpret_cast<LightPos_t>(reinterpret_cast<void* const*>(vt)[0x24 / 4])(const_cast<BYTE*>(p), pos);
        for (int k = 0; k < 4; k++) {
            uint32_t v;
            std::memcpy(&v, &pos[k], 4);
            Mix(h, v);
        }
        r.sig = h;
        for (int k = 0; k < 3; k++) r.head[k] = reinterpret_cast<const float*>(p + 0x120)[k];
        r.range = *reinterpret_cast<const float*>(p + 0x130);
        r.intensity = *reinterpret_cast<const float*>(p + 0x10);
        const float* e0 = reinterpret_cast<const float*>(p + 0xE0);
        const float* f0 = reinterpret_cast<const float*>(p + 0xF0);
        float a = 0.0f, b = 0.0f;
        for (int k = 0; k < 3; k++) {
            a += std::max(e0[k], 0.0f);
            b += std::max(f0[k], 0.0f);
        }
        // the lit colour; a street lamp that is off may be evaluated with +0xF0 x +0x10 ("street lamps count as lit")
        r.esum = a > 0.0f ? a : (r.type == 11 ? b * ApexMath::Fabs(r.intensity) : 0.0f);
        if (r.type == 5) {
            const float* S = reinterpret_cast<const float*>(p + 0x150);
            r.spill = std::max({1.0f, S[0], S[1], S[2]});
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// Real-time outdoor lamps: the light is one the GPU draws for a room of lot `roomLot` (RtLampModel::Drawn). POD (SEH)
bool DrawnLight(uintptr_t L, uint64_t roomLot) {
    if (!L) return false;
    RtLampModel::LightFields f;
    __try {
        const BYTE* p = reinterpret_cast<const BYTE*>(L);
        f.type = *reinterpret_cast<const int*>(p + 0xB0);
        f.lot = *reinterpret_cast<const uint32_t*>(p + 0xC0) | static_cast<uint64_t>(*reinterpret_cast<const uint32_t*>(p + 0xC4)) << 32;
        f.room = *reinterpret_cast<const int*>(p + 0x08);
        f.flags = p[0x100];
        if (f.type == 5)
            f.unmodelled = !RtLampModel::ShadedModelled(reinterpret_cast<const float*>(p + 0x1A0), reinterpret_cast<const float*>(p + 0x190),
                                                        *reinterpret_cast<const float*>(p + 0x174), *reinterpret_cast<const float*>(p + 0x170));
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return RtLampModel::Drawn(f, roomLot);
}
struct Desc {
    uintptr_t base = 0;
    uint32_t pitch = 0, w = 0, h = 0;
};
bool ReadDesc(const BYTE* room, int m, Desc& d) {
    __try {
        const BYTE* rec = room + kMapDesc[m];
        d.base = *reinterpret_cast<const uintptr_t*>(rec);
        d.pitch = *reinterpret_cast<const uint32_t*>(rec + 4);
        d.w = *reinterpret_cast<const uint32_t*>(rec + 0x18);
        d.h = *reinterpret_cast<const uint32_t*>(rec + 0x1C);
        return d.base && d.pitch >= 4 && d.w && d.h && d.w <= 8192 && d.h <= 8192 && d.pitch >= d.w * 4;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
uintptr_t SlotTexture(int cls, int m) {
    __try {
        if (cls < 0 || cls > 2) return 0;
        const uintptr_t wrapper = *reinterpret_cast<const uintptr_t*>(Slots() + static_cast<uintptr_t>(cls) * 16 + static_cast<uintptr_t>(kSlotIdx[m]) * 4);
        if (!wrapper || *reinterpret_cast<const uint32_t*>(wrapper + 0x2C) != 0) return 0;
        return *reinterpret_cast<const uintptr_t*>(wrapper + 0x30);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
bool ReadTexel(uintptr_t addr, uint32_t& v) {
    __try {
        v = *reinterpret_cast<const uint32_t*>(addr);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool WriteTexel(uintptr_t addr, uint32_t v) {
    __try {
        *reinterpret_cast<uint32_t*>(addr) = v;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool WriteRows(const Desc& d, int x0, int y0, int n, int rows, const uint32_t* src) {
    if (x0 < 0 || y0 < 0 || static_cast<uint32_t>(x0 + n) > d.w || static_cast<uint32_t>(y0 + rows) > d.h) return false;
    __try {
        for (int k = 0; k < rows; k++)
            std::memcpy(reinterpret_cast<void*>(d.base + static_cast<uintptr_t>(d.pitch) * (y0 + k) + static_cast<uintptr_t>(x0) * 4), src + static_cast<size_t>(k) * n, static_cast<size_t>(n) * 4);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool ReadBatchVec(uintptr_t& b, uintptr_t& e) {
    __try {
        const uintptr_t vec = BatchVec();
        b = *reinterpret_cast<const uintptr_t*>(vec);
        e = *reinterpret_cast<const uintptr_t*>(vec + 4);
        return b && e > b && (e - b) % 0x30 == 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool CopyBytes(void* dst, const void* src, size_t n) {
    __try {
        std::memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
struct Consts {
    float k1 = 1.0f, k2 = 0.075f, street = 3.3333f;
};
Consts ReadConsts() {
    Consts c;
    __try {
        // the light law constants (Steam 1.67.2: 0x011D0A60, 0x011D0A68, 0x01158DA8); the defaults when one was not found
        const uintptr_t k1At = GameAddr::Get(GameAddr::Id::LampK1), k2At = GameAddr::Get(GameAddr::Id::LampK2), stAt = GameAddr::Get(GameAddr::Id::StreetLampScale);
        const float k1 = k1At ? *reinterpret_cast<const float*>(k1At) : c.k1, k2 = k2At ? *reinterpret_cast<const float*>(k2At) : c.k2,
                    st = stAt ? *reinterpret_cast<const float*>(stAt) : c.street;
        if (k1 > 1e-6f && k1 < 1e6f) c.k1 = k1;
        if (k2 > 0.0f && k2 < 1e3f) c.k2 = k2;
        if (st > 0.0f && st < 100.0f) c.street = st;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return c;
}
// The game's pack after Smooth light maps' dither, as RoomMapDither's thunk does it
using Pack_t = void(__fastcall*)(void* out, void* edx, const float* c); // = thiscall ret 4
uint32_t PackGame(const float* colour, unsigned x, unsigned y, bool dither) {
    alignas(16) float c[4];
    std::memcpy(c, colour, sizeof c);
    if (dither) Dither(c, RoomMapDither::Offset(x, y) / 255.0f);
    DWORD out = 0;
    reinterpret_cast<Pack_t>(Pack())(&out, nullptr, c);
    return out;
}
bool DitherNow() { return RoomMapDither::Running() && RoomMapDither::Enabled(); }

// The room's light list, each light once with how often it is listed
struct ListLight {
    uintptr_t light;
    int mult;
};
bool UniqueList(const BYTE* room, std::vector<ListLight>& out) {
    uintptr_t raw[1024];
    const int n = ReadList(room, raw, static_cast<int>(std::size(raw)));
    if (n < 0) return false;
    out.clear();
    for (int i = 0; i < n; i++) {
        if (!raw[i]) continue;
        auto it = std::find_if(out.begin(), out.end(), [&](const ListLight& l) { return l.light == raw[i]; });
        if (it != out.end()) it->mult++;
        else out.push_back({raw[i], 1});
    }
    return true;
}

// ---- the solve being kept (light tree thread) ----
struct Acc {
    uintptr_t light;
    float rgb[3];
    float vis, all;
};
struct SampleState {
    bool on = false;
    int tap = -1;
    const float* sum = nullptr;
    uintptr_t pending = 0;
    float snap[3] = {};
    Acc acc[128];
    int n = 0;
    bool overflow = false;
    bool masked = false, gated = false;
    float visLum = 0.0f, allLum = 0.0f;
};
SampleState g_s; // light tree thread only

struct Capture {
    bool active = false, ok = false;
    const BYTE* room = nullptr;
    std::unique_ptr<Record> rec;
    std::unordered_map<uintptr_t, int> lampIdx;
    std::vector<ListLight> startList;
    std::vector<uint64_t> startSigs;
    int pending = -1; // the texel to read back
    const char* why = nullptr;
    bool directional = false;
    // a self-check solve: the texels the delta wrote, by (map, x, y)
    bool check = false, checkLightsSame = false;
    std::unordered_map<uint64_t, uint32_t> checkOld;
    long cmpN = 0, cmpOver1 = 0, cmpAt1 = 0;
    int cmpMax = 0;
};
Capture g_cap; // light tree thread only

void Fail(const char* why) {
    if (!g_cap.ok) return;
    g_cap.ok = false;
    g_cap.why = why;
    c_captureFails.fetch_add(1, std::memory_order_relaxed);
}
uint64_t TexelKey(int map, unsigned x, unsigned y) { return (static_cast<uint64_t>(map) << 32) | (static_cast<uint64_t>(y) << 16) | x; }
void FlushPending() {
    if (!g_cap.active || g_cap.pending < 0 || !g_cap.rec) return;
    TexelRec& t = g_cap.rec->texels[static_cast<size_t>(g_cap.pending)];
    g_cap.pending = -1;
    Desc d;
    uint32_t v = 0;
    if (!ReadDesc(g_cap.room, t.map, d) || t.x >= d.w || t.y >= d.h || !ReadTexel(d.base + static_cast<uintptr_t>(d.pitch) * t.y + static_cast<uintptr_t>(t.x) * 4, v)) {
        Fail("a texel could not be read back");
        return;
    }
    t.packed = v;
    if (g_cap.check && g_cap.checkLightsSame) {
        const auto it = g_cap.checkOld.find(TexelKey(t.map, t.x, t.y)); // (texels two samples write are left out of it)
        if (it != g_cap.checkOld.end()) {
            const int diff = MaxLevelDiff(it->second, v);
            g_cap.cmpN++;
            g_cap.cmpMax = std::max(g_cap.cmpMax, diff);
            if (diff == 1) g_cap.cmpAt1++;
            if (diff > 1) g_cap.cmpOver1++;
        }
    }
}
void DropCapture() {
    g_cap = Capture{};
    g_s = SampleState{};
}


// ---- the log, the self-check and the measures (render thread and light tree thread) ----
std::mutex g_sideMx;
std::unordered_map<const void*, DWORD> g_checkDue; // room -> when its full solve is asked
std::unordered_set<const void*> g_checkPending;    // asked, not solved yet
// One lamp edit of a lot (its first room-0 change after LampMarkFilter::EditLastTick), until its rooms are quiet
struct LotEdit {
    DWORD edit = 0, last = 0;
    int deltas = 0, full = 0, direct = 0;
    long texels = 0;
    double ms = 0.0;
    std::vector<uintptr_t> lights;
    std::string why; // the first full solve's reason
    int story0 = 99, story1 = -99;
    DWORD room0Final = 0; // the last room 0 brought up to date or solved
    int others = 0;       // other rooms of the lot solved meanwhile (roofed rooms through openings, yards)
    long othersMs = 0;    // their solve time (start to end, frames included)
    DWORD othersFinal = 0;
};
std::map<uint64_t, LotEdit> g_lotEdits; // under g_sideMx
std::map<std::string, long> g_reasons;  // under g_sideMx
std::unordered_map<const void*, const char*> g_fullWhy;          // room -> why its current solve is a full one (under g_sideMx)
std::unordered_map<const void*, std::pair<uint64_t, DWORD>> g_otherStart; // room -> (lot, its solve's start) (under g_sideMx)

LotEdit* EditOf(uint64_t lot, DWORD now) { // under g_sideMx
    const unsigned long edit = LampMarkFilter::EditLastTick();
    if (!edit || now - edit > kEditWindowMs) return nullptr;
    LotEdit& e = g_lotEdits[lot];
    if (e.edit != edit) e = LotEdit{static_cast<DWORD>(edit), now};
    e.last = now;
    return &e;
}
void NoteRoom0(uint64_t lot, int story, bool delta, long texels, double ms, const std::vector<uintptr_t>& lights, const char* why, bool direct) {
    const DWORD now = GetTickCount();
    std::lock_guard<std::mutex> lk(g_sideMx);
    LotEdit* e = EditOf(lot, now);
    if (!e) return;
    (delta ? e->deltas : e->full)++;
    if (direct) e->direct++;
    e->texels += texels;
    e->ms += ms;
    e->story0 = std::min(e->story0, story);
    e->story1 = std::max(e->story1, story);
    e->room0Final = now;
    for (uintptr_t L : lights)
        if (e->lights.size() < 4 && std::find(e->lights.begin(), e->lights.end(), L) == e->lights.end()) e->lights.push_back(L);
    if (!delta && e->why.empty() && why) e->why = why;
}
void NoteReason(const char* why) {
    std::lock_guard<std::mutex> lk(g_sideMx);
    if (g_reasons.size() < 64) g_reasons[why]++;
}
std::atomic<long> c_dropLogged{0};
// What was kept of a room is dropped: why (the log, at most 200 lines a session)
std::string g_lastDrop[8][3]; // (under g_sideMx) the last drop or refusal of each story's room 0 and class (any lot)
void LogDrop(int story, int cls, const char* what, const char* why) {
    NoteReason(why);
    if (story >= 0 && story < 8 && cls >= 0 && cls < 3) {
        std::lock_guard<std::mutex> lk(g_sideMx);
        g_lastDrop[story][cls] = std::format("{} ({})", what, why);
    }
    const unsigned long edit = LampMarkFilter::EditLastTick();
    if (!(edit && GetTickCount() - edit < kEditWindowMs) && c_dropLogged.load() > 40) return;
    if (c_dropLogged.fetch_add(1) < 200) LOG_INFO(std::format("[OutdoorDelta] Story {} room 0 class {}: {} ({})", story, cls, what, why));
}

DWORD g_logSecond = 0;
int g_logInSecond = 0;
bool LogAllowed() { // at most 20 delta lines a second (a drag)
    const DWORD now = GetTickCount();
    if (now - g_logSecond >= 1000) {
        g_logSecond = now;
        g_logInSecond = 0;
    }
    return ++g_logInSecond <= 20;
}

// ---- the delta (light tree thread) ----
struct ReplayOut {
    float rgb[3];
    float vis, all;
    bool set;
};
std::vector<ReplayOut> g_replay; // light tree thread
const uint8_t* g_replayBase = nullptr;

// The basic validity of a record for the room as it is now (no geometry: the caller compares it once per lot)
const char* ValidateBasic(const Record& r, const BYTE* room, const RoomRead& rr) {
    if (r.mgr != rr.mgr || r.cls != rr.cls) return "the room changed";
    if (r.gen != g_gen.load(std::memory_order_relaxed)) return "the lighting options changed";
    if (r.physics != LevelLightShare::PhysicsKey()) return "the lighting options changed";
    if (r.dither != DitherNow()) return "the map dither changed";
    if (r.thr != rr.thr || r.wallMode != rr.wallMode) return "the room's class settings changed";
    if (RoomLightQueue::QuickPassTarget(room) > 0) return "a quick pass";
    if (!LevelLightShare::ListAllWrapped(room)) return "a light whose evaluation is not wrapped";
    return nullptr;
}
// The maps a locked solve or the delta's own lock hold are the ones the record was kept from
const char* ValidateMaps(const Record& r, const BYTE* room, int cls) {
    for (int m = 0; m < 4; m++) {
        if (!r.maps[m].used) continue;
        Desc d;
        if (!ReadDesc(room, m, d) || d.pitch != r.maps[m].pitch || d.w != r.maps[m].w || d.h != r.maps[m].h) return "a map moved";
        if (SlotTexture(cls, m) != r.maps[m].tex) return "a map moved";
    }
    return nullptr;
}

struct Changed {
    uintptr_t light;
    int mult;     // now (0: left the list)
    int rec = -1; // its LampRec, -1 = new to the list
    LightNow now;
};
// What a delta writes: map texels (in the solve's order) and class-2 wall blocks to blur again
struct Plan {
    std::vector<std::pair<uint32_t, uint32_t>> writes; // (texel, packed)
    std::vector<uint8_t> dirtyBlock;
    std::vector<uintptr_t> lights; // the changed lights
    long texels = 0;
    int batches = 0, batchesSkipped = 0;
    float reach = -1.0f; // the largest reach bound used (m; -1 = none)
    bool Empty() const { return writes.empty() && std::none_of(dirtyBlock.begin(), dirtyBlock.end(), [](uint8_t b) { return b != 0; }); }
};
enum class Delta { Fail, NoChange, Ok };

// The changed lights' new parts and the texels they change (no map is touched); the record takes them
constexpr size_t kMaxEditBatches = 200;
std::atomic<long> c_batchRefused{0};
bool LampEditRecent() {
    const unsigned long edit = LampMarkFilter::EditLastTick();
    return LevelLightShare::LampDragging() || (edit && GetTickCount() - edit < kEditWindowMs);
}
Delta ComputeDelta(BYTE* room, const RoomRead& rr, Record& r, const char*& why, Plan& plan) {
    std::vector<ListLight> list;
    if (!UniqueList(room, list)) {
        why = "the light list could not be read";
        return Delta::Fail;
    }
    std::vector<Changed> changed;
    for (const ListLight& l : list) {
        LightNow now;
        if (!ReadLight(l.light, now)) {
            why = "a light could not be read";
            return Delta::Fail;
        }
        int idx = -1;
        for (size_t i = 0; i < r.lamps.size(); i++)
            if (r.lamps[i].light == l.light) idx = static_cast<int>(i);
        if (idx >= 0 && r.lamps[idx].mult == l.mult && r.lamps[idx].sig == now.sig) continue;
        changed.push_back({l.light, l.mult, idx, now});
    }
    for (size_t i = 0; i < r.lamps.size(); i++)
        if (std::none_of(list.begin(), list.end(), [&](const ListLight& l) { return l.light == r.lamps[i].light; })) changed.push_back({r.lamps[i].light, 0, static_cast<int>(i), LightNow{}});
    if (changed.empty()) return Delta::NoChange;
    // Real-time outdoor lamps draws every changed light on every story of this lot (a lamp it draws moved, switched or was
    // recoloured, or one left or joined the list): the maps it relights take their room-0 texels from the GPU, and the base
    // (the sum less the drawn lamps' parts) is the same with the old sum and the old parts. Nothing is solved again or
    // written; the parts stay as they were (a later delta with the option off takes them as changed), and the option
    // sends the lot's room 0s to their solve when it is switched off.
    if (RtOutdoorLamps::CoversRoom(room)) {
        bool allDrawn = true;
        for (const Changed& c : changed) {
            const bool now = DrawnLight(c.light, r.lot);
            // (14/10) a kept lamp whose "drawn" changed since its parts were kept (moved into or out of an indoor room still
            // listed): the record's base no longer matches what the GPU draws; the full solve keeps it again
            if (c.rec >= 0 && r.lamps[static_cast<size_t>(c.rec)].drawn != now) {
                why = "a lamp the GPU draws changed whether it is drawn";
                return Delta::Fail;
            }
            allDrawn = allDrawn && now;
        }
        if (allDrawn) {
            for (const Changed& c : changed) plan.lights.push_back(c.light);
            c_gpuSkips.fetch_add(1, std::memory_order_relaxed);
            return Delta::Ok;
        }
    }
    for (const Changed& c : changed)
        if (c.rec >= 0 && r.lamps[static_cast<size_t>(c.rec)].collapsed) {
            why = "a lamp the GPU draws keeps only its sum (Instant)";
            return Delta::Fail;
        }
    if (static_cast<int>(changed.size()) > kMaxChanged) {
        why = "too many lights changed";
        return Delta::Fail;
    }
    const Consts k = ReadConsts();
    // where each changed light can add anything (a lamp class's law; any other light: everywhere)
    std::vector<float> bounds(changed.size(), -1.0f);
    for (size_t ci = 0; ci < changed.size(); ci++) {
        const Changed& c = changed[ci];
        if (c.now.type == 3 || c.now.type == 4 || c.now.type == 5 || c.now.type == 11) {
            const float W = c.now.range * c.now.intensity * k.k2 / k.k1 * (c.now.type == 11 ? k.street : 1.0f);
            bounds[ci] = ApexMath::IsFinite(W) ? ReachBound2(ApexMath::Fabs(W), c.now.esum, c.now.spill, rr.thr) : -1.0f;
        }
    }
    // 14/10: the replay runs in the lock step, in one piece (13 deltas of 44-73 ms and 193-252 ms ones in the logs): while a
    // lamp is edited, more than kMaxEditBatches batches to solve again send the room to its full solve instead, which the
    // game slices over frames (the same texels: the delta reproduces the solve)
    if (LampEditRecent()) {
        size_t inReach = 0;
        for (size_t ci = 0; ci < changed.size(); ci++) {
            const float b2 = bounds[ci];
            if (changed[ci].mult <= 0 || changed[ci].mult > 16 || b2 == 0.0f) continue;
            for (const BatchRec& b : r.batches) {
                if (b2 > 0.0f) {
                    const float dx = changed[ci].now.head[0] - b.c[0], dy = changed[ci].now.head[1] - b.c[1], dz = changed[ci].now.head[2] - b.c[2];
                    const float d = ApexMath::Sqrt(dx * dx + dy * dy + dz * dz) - b.r;
                    if (d > 0.0f && d * d > b2) continue;
                }
                inReach++;
            }
        }
        if (inReach > kMaxEditBatches) {
            c_batchRefused.fetch_add(1, std::memory_order_relaxed);
            why = "too many batches to solve again in one step during an edit (the full solve is sliced)";
            return Delta::Fail;
        }
    }
    std::vector<uint8_t> touched(r.texels.size(), 0);
    std::vector<std::vector<Contrib>> fresh(changed.size());
    for (size_t ci = 0; ci < changed.size(); ci++) {
        const Changed& c = changed[ci];
        plan.lights.push_back(c.light);
        if (c.mult <= 0) continue;
        if (c.mult > 16) {
            why = "a light listed too often";
            return Delta::Fail;
        }
        const float bound2 = bounds[ci];
        if (bound2 > 0.0f) plan.reach = std::max(plan.reach, ApexMath::Sqrt(bound2));
        if (bound2 == 0.0f) continue; // it adds nothing anywhere (switched off)
        for (const BatchRec& b : r.batches) {
            if (bound2 > 0.0f) {
                const float dx = c.now.head[0] - b.c[0], dy = c.now.head[1] - b.c[1], dz = c.now.head[2] - b.c[2];
                const float d = ApexMath::Sqrt(dx * dx + dy * dy + dz * dz) - b.r;
                if (d > 0.0f && d * d > bound2) {
                    plan.batchesSkipped++;
                    continue;
                }
            }
            g_replay.assign(b.count, ReplayOut{});
            g_replayBase = r.samples.data() + static_cast<size_t>(b.first) * 0x30;
            const bool ok = LevelLightShare::ReplayBatch(room, g_replayBase, static_cast<int>(b.count), b.flags, c.light, c.mult);
            g_replayBase = nullptr;
            if (!ok) {
                why = "a batch could not be solved again";
                return Delta::Fail;
            }
            plan.batches++;
            for (uint32_t i = 0; i < b.count; i++) {
                const ReplayOut& o = g_replay[i];
                if (!o.set) {
                    why = "a sample was not solved again";
                    return Delta::Fail;
                }
                Contrib p;
                p.texel = b.first + i;
                for (int q = 0; q < 3; q++) p.rgb[q] = ApexMath::IsFinite(o.rgb[q]) ? o.rgb[q] : 0.0f;
                const bool masked = (r.texels[p.texel].bits & kMasked) != 0;
                p.vis = masked ? ToHalf(o.vis) : 0;
                p.all = masked ? ToHalf(o.all) : 0;
                if (Nonzero(p)) fresh[ci].push_back(p);
            }
        }
    }
    // each changed light's old part out, its new one in
    for (size_t ci = 0; ci < changed.size(); ci++) {
        const Changed& c = changed[ci];
        if (c.rec >= 0)
            for (const Contrib& p : r.lamps[c.rec].parts) {
                Remove(r.texels[p.texel].t, p);
                touched[p.texel] = 1;
            }
        for (const Contrib& p : fresh[ci]) {
            Add(r.texels[p.texel].t, p);
            touched[p.texel] = 1;
        }
    }
    for (size_t ci = 0; ci < changed.size(); ci++) {
        const Changed& c = changed[ci];
        LampRec* l = nullptr;
        if (c.rec >= 0) l = &r.lamps[c.rec];
        else {
            r.lamps.push_back(LampRec{});
            l = &r.lamps.back();
            l->light = c.light;
            l->drawn = DrawnLight(c.light, r.lot);
        }
        // at its exact size (assign keeps a larger buffer, or grows by half), unless that moves it under 2 GB (ShrinkExact)
        if (WouldLeaveHigh(l->parts, fresh[ci].size())) l->parts.assign(fresh[ci].begin(), fresh[ci].end());
        else {
            decltype(l->parts) exact(fresh[ci].begin(), fresh[ci].end());
            l->parts.swap(exact);
        }
        std::vector<OutdoorDeltaModel::Contrib>().swap(fresh[ci]);
        l->mult = c.mult;
        l->sig = c.now.sig;
        l->type = c.now.type;
    }
    std::erase_if(r.lamps, [](const LampRec& l) { return l.mult <= 0; });
    // the texels: composed and packed as the solve packs them; the lamps under an upper floor's plane (the floor mask's
    // gate) take the whole list, so every gated texel is composed again
    plan.dirtyBlock.assign(r.blocks.size(), 0);
    const bool blurred = r.passes > 0;
    for (size_t i = 0; i < r.texels.size(); i++) {
        TexelRec& t = r.texels[i];
        if (!touched[i] && !(t.bits & (kGated | kDup))) continue;
        float alpha = static_cast<float>(t.packed >> 24) / 255.0f;
        if (t.bits & kMasked) {
            const float under = (t.bits & kGated) ? LevelLightShare::UnderFloorShareAt(room, reinterpret_cast<const float*>(r.samples.data() + i * 0x30)) : -1.0f;
            alpha = MaskAlpha(t.t.vis, t.t.all, under);
        }
        alignas(16) float c[4];
        PackInput(t.t.S, alpha, c);
        const uint32_t v = PackGame(c, t.x, t.y, r.dither);
        if (v == t.packed && !(t.bits & kDup)) continue; // (a texel two samples write: each in the solve's order, the last stays)
        if (t.map == 0 && r.cls == 2) { // class 2: the wall atlas is blurred after the solve (Apex's replay of the game's blur)
            if (!blurred) {
                why = "walls blurred by the game's own blur";
                return Delta::Fail;
            }
            if (t.block >= r.blocks.size() || !r.blocks[t.block].ok) {
                why = "a wall block that cannot be blurred again";
                return Delta::Fail;
            }
            const BlockRec& b = r.blocks[t.block];
            r.preblur[b.off + static_cast<size_t>(t.y - b.y0) * (b.x1 - b.x0 + 1) + (t.x - b.x0)] = v;
            plan.dirtyBlock[t.block] = 1;
        } else
            plan.writes.emplace_back(static_cast<uint32_t>(i), v);
        t.packed = v;
        plan.texels++;
    }
    r.used = GetTickCount();
    r.version = g_version.fetch_add(1, std::memory_order_relaxed) + 1;
    if (RtOutdoorLamps::Watching()) { // (a delta that changed only drawn lamps returned above: its base is the same)
        const uint64_t bh = BaseHash(r);
        if (bh != r.baseHash || !r.baseVersion) r.baseHash = bh, r.baseVersion = r.version;
    }
    return Delta::Ok;
}
// The plan into the room's locked maps
bool ApplyDelta(const BYTE* room, Record& r, const Plan& plan, const char*& why) {
    Desc desc[4];
    for (int m = 0; m < 4; m++)
        if (r.maps[m].used && !ReadDesc(room, m, desc[m])) {
            why = "a map is not locked";
            return false;
        }
    for (const auto& [i, v] : plan.writes) {
        const TexelRec& t = r.texels[i];
        if (!r.maps[t.map].used || !WriteTexel(desc[t.map].base + static_cast<uintptr_t>(desc[t.map].pitch) * t.y + static_cast<uintptr_t>(t.x) * 4, v)) {
            why = "a texel could not be written";
            return false;
        }
    }
    std::vector<uint32_t> ext;
    for (size_t j = 0; j < plan.dirtyBlock.size(); j++) {
        if (!plan.dirtyBlock[j]) continue;
        const BlockRec& b = r.blocks[j];
        const int n = b.x1 - b.x0 + 1, rows = b.y1 - b.y0 + 1;
        ext.assign(r.preblur.begin() + b.off, r.preblur.begin() + b.off + static_cast<size_t>(n) * rows);
        BlurBlock(ext.data(), r.preblur.data() + b.off, n, rows, false, false, r.passes);
        if (!WriteRows(desc[0], b.x0, b.y0, n, rows, ext.data())) {
            why = "a wall block could not be written";
            return false;
        }
    }
    return true;
}
void UpdateBytes(Record& r) { // under g_mx
    const size_t was = r.bytes;
    r.bytes = BytesOf(r);
    g_bytes.fetch_add(r.bytes);
    g_bytes.fetch_sub(std::min(g_bytes.load(), was));
}
double MsSince(const LARGE_INTEGER& t0) {
    LARGE_INTEGER t1{}, fq{};
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&fq);
    return fq.QuadPart ? 1000.0 * static_cast<double>(t1.QuadPart - t0.QuadPart) / static_cast<double>(fq.QuadPart) : 0.0;
}
std::string LightsText(const std::vector<uintptr_t>& lights) {
    std::string ls;
    for (size_t i = 0; i < lights.size() && i < 4; i++) ls += std::format("{}L{:08X}", i ? " " : "", lights[i]);
    if (lights.size() > 4) ls += std::format(" (+{})", lights.size() - 4);
    return ls.empty() ? std::string("no light changed") : ls;
}

// ---- the direct path: a lamp edit taken from the lamp entry update (light tree thread) ----
std::atomic<bool> g_directWriting{false};
struct PendingLot {
    uintptr_t tracker = 0;
    unsigned stories = 0; // the lamp stories whose room 0 mark was dropped (their dependents are sent after the delta)
    LARGE_INTEGER at{};
    DWORD tick = 0;
};
std::vector<PendingLot> g_pending; // light tree thread only
std::atomic<long> c_direct{0}, c_directRooms{0}, c_directFallbacks{0}, c_taken{0}, c_refused{0};
std::atomic<float> g_lastDirectMs{0.0f};

// A room 0 the direct path can bring up to date: idle (solved, its maps unlocked, shown at the class it was solved at) and
// with a record of that class; why = what is missing
bool Idle(const BYTE* room, int& cls) {
    __try {
        const int state = *reinterpret_cast<const int*>(room + 0xF0);
        cls = *reinterpret_cast<const int*>(room + 0xF4);
        return (state == 4 || state == 5) && room[0xE8] == 0 && cls == *reinterpret_cast<const int*>(room + 0x100) && cls >= 0 && cls <= 2;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
uintptr_t ReadTracker(uintptr_t tl, int& story) {
    __try {
        story = *reinterpret_cast<const int*>(tl + 0x1A0);
        return *reinterpret_cast<const uintptr_t*>(tl + 4);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
const char* DirectReady(uintptr_t tracker) { // under g_mx
    if (!LevelLightShare::DeltaReady()) return "the room solve hooks are not in";
    int rooms = 0;
    for (int S = 0; S <= 7; S++) {
        BYTE* room = static_cast<BYTE*>(LevelLightShare::Room0Of(tracker, S));
        if (!room) continue;
        rooms++;
        int cls = -1;
        RoomRead rr;
        if (!Idle(room, cls) || !ReadRoom(room, rr)) return "a story's room 0 is being solved";
        const auto it = g_records.find(KeyOf(room, cls));
        if (it == g_records.end()) return "a story's room 0 has no kept parts";
        if (const char* why = ValidateBasic(*it->second, room, rr)) return why;
    }
    return rooms ? nullptr : "no room 0";
}

// Every story's room 0 of the lot brought up to date: computed, then (only where texels change) locked, written, finished
void DirectRun(PendingLot& p) {
    LARGE_INTEGER t0{};
    QueryPerformanceCounter(&t0);
    int rooms = 0, written = 0;
    long texels = 0;
    int batches = 0, skipped = 0;
    float reach = -1.0f;
    std::vector<uintptr_t> lights;
    std::string fallbacks;
    uint64_t lot = 0, geo = 0;
    int story0 = 99, story1 = -99;
    for (int S = 0; S <= 7; S++) {
        BYTE* room = static_cast<BYTE*>(LevelLightShare::Room0Of(p.tracker, S));
        if (!room) continue;
        rooms++;
        RoomRead rr;
        int cls = -1;
        const char* why = nullptr;
        LARGE_INTEGER r0{};
        QueryPerformanceCounter(&r0);
        std::unique_lock<std::recursive_mutex> lk(g_mx);
        auto it = g_records.end();
        if (!ReadRoom(room, rr) || !Idle(room, cls)) why = "being solved";
        else if ((it = g_records.find(KeyOf(room, cls))) == g_records.end()) why = "no kept parts";
        else if (!(why = ValidateBasic(*it->second, room, rr))) {
            if (!geo) geo = LevelLightShare::LotGeometryKey(room);
            if (!geo || geo != it->second->geo) why = "walls, floors or openings changed";
        }
        lot = rr.lot;
        Plan plan;
        Delta d = Delta::Fail;
        if (!why) d = ComputeDelta(room, rr, *it->second, why, plan);
        bool ok = d != Delta::Fail;
        if (d == Delta::Ok && !plan.Empty()) {
            // the room's own lock step (it copies the room's maps into the class's working maps and locks them), the
            // texels, then the finalize's own steps (edge copies, unlock and copy back, the room's lighting message)
            if (!LevelLightShare::RoomMapsFree()) {
                why = "another room's maps are locked";
                ok = false;
            } else {
                g_directWriting.store(true);
                g_deltaRoom.store(room);
                if (!LevelLightShare::LockRoomMaps(room)) {
                    why = "its maps could not be locked";
                    ok = false;
                } else {
                    if (!(why = ValidateMaps(*it->second, room, cls)) && !ApplyDelta(room, *it->second, plan, why)) {
                    }
                    ok = why == nullptr;
                    LevelLightShare::FinishRoomMaps(room);
                    RtOutdoorLamps::NoteMapsWritten(room); // (its relit copies start from the maps again)
                }
                g_deltaRoom.store(nullptr);
                g_directWriting.store(false);
                if (ok) written++;
            }
        }
        if (ok && it != g_records.end()) UpdateBytes(*it->second);
        if (!ok) {
            if (it != g_records.end()) EraseLocked(it->first);
            lk.unlock();
            c_directFallbacks.fetch_add(1, std::memory_order_relaxed);
            LogDrop(rr.story, cls, "solved in full after a lamp edit", why ? why : "?");
            LevelLightShare::QueueRoom0(p.tracker, S); // the room solve brings it up to date (and keeps its parts again)
            fallbacks += std::format("{}{} ({})", fallbacks.empty() ? "" : ", ", S, why ? why : "?");
            continue;
        }
        lk.unlock();
        texels += plan.texels;
        batches += plan.batches;
        skipped += plan.batchesSkipped;
        reach = std::max(reach, plan.reach);
        for (uintptr_t L : plan.lights)
            if (std::find(lights.begin(), lights.end(), L) == lights.end()) lights.push_back(L);
        story0 = std::min(story0, rr.story);
        story1 = std::max(story1, rr.story);
        if (d == Delta::Ok) {
            NoteRoom0(rr.lot, rr.story, true, plan.texels, MsSince(r0), plan.lights, nullptr, true);
            if (g_check.load(std::memory_order_relaxed) && !plan.Empty()) {
                std::lock_guard<std::mutex> sk(g_sideMx);
                if (g_checkDue.size() < 256) g_checkDue[room] = (GetTickCount() + kCheckDelayMs) | 1;
            }
        }
    }
    // the rooms that take room 0's lamps (yards, roofed rooms through their openings): sent as the game's walk would
    // (14/10 review: the edit's tail, no urgency, each room once, under the spread budget)
    const int sent = LevelLightShare::SendRoom0DependentsTail(p.tracker, p.stories);
    const double ms = MsSince(t0);
    const double sinceEdit = MsSince(p.at);
    c_direct.fetch_add(1, std::memory_order_relaxed);
    c_directRooms.fetch_add(written, std::memory_order_relaxed);
    c_texels.fetch_add(texels, std::memory_order_relaxed);
    c_batches.fetch_add(batches, std::memory_order_relaxed);
    g_lastDirectMs.store(static_cast<float>(ms));
    if (static_cast<float>(ms) > g_maxMs.load()) g_maxMs.store(static_cast<float>(ms));
    if (LogAllowed())
        LOG_INFO(std::format("[OutdoorDelta] Outdoor lamp delta: {}, {} rooms, {} texels, {:.1f} ms (no re-solve); lot {:08X}: every room 0 of stories {}..{} final "
                             "{:.1f} ms after the edit ({} of {} stories' maps rewritten, {} batches solved again, {} beyond reach {:.1f} m left out){}; {} other rooms sent",
                             LightsText(lights), written, texels, ms, static_cast<uint32_t>(lot), story0 == 99 ? 0 : story0, story1 < 0 ? 0 : story1, sinceEdit, written,
                             rooms, batches, skipped, reach,
                             fallbacks.empty() ? std::string() : std::format("; solved in full: stories {}", fallbacks), sent));
}

} // namespace

void SetEnabled(bool on) {
    if (g_on.exchange(on) == on) return;
    if (!on) {
        std::lock_guard<std::recursive_mutex> lk(g_mx);
        ClearLocked();
        g_clear.store(true);
        g_deltaRoom.store(nullptr); // (2.12.3 + RT: the solve hooks stop calling SolveEnd while off)
    }
    LOG_INFO(on ? "[OutdoorDelta] On: an outdoor lamp edit brings every story's room 0 up to date from each light's kept part"
                : "[OutdoorDelta] Off: every room 0 is solved in full");
}
bool Enabled() { return g_on.load(std::memory_order_relaxed); }
void SetSelfCheck(bool on) {
    if (g_check.exchange(on) == on) return;
    LOG_INFO(on ? "[OutdoorDelta] Self-check on: each room brought up to date is solved in full once more and compared" : "[OutdoorDelta] Self-check off");
}

namespace {
// A taken edit that cannot run: every story's room 0 goes through its solve, and the rooms that take its lamps are sent
std::atomic<bool> g_dropPending{false}; // the world changed: the taken edits' lots are gone
void Abandon(const PendingLot& p, const char* why, bool lotLive) {
    for (int S = 0; S <= 7; S++) LevelLightShare::QueueRoom0(p.tracker, S); // (SEH inside: a lot that went away sends nothing)
    if (lotLive)
        for (int L = 0; L <= 7; L++)
            if (p.stories >> L & 1u) LevelLightShare::SendRoom0Dependents(p.tracker, L);
    NoteReason(why);
    c_directFallbacks.fetch_add(1, std::memory_order_relaxed);
    if (LogAllowed()) LOG_INFO(std::format("[OutdoorDelta] Lamp edit taken but not run: every room 0 of the lot through its solve ({})", why));
}
} // namespace

bool TakeEdit(uintptr_t tl) {
    if (!g_on.load(std::memory_order_relaxed) || !tl) return false;
    int L = -99;
    const uintptr_t tracker = ReadTracker(tl, L);
    if (!tracker || L < 0 || L > 7) return false;
    for (PendingLot& p : g_pending)
        if (p.tracker == tracker) { // a second lamp of the lot in this update: the same run
            p.stories |= 1u << L;
            c_taken.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
    if (g_dropPending.exchange(false)) g_pending.clear();
    const char* why = nullptr;
    {
        std::lock_guard<std::recursive_mutex> lk(g_mx);
        if (g_clear.exchange(false)) {
            ClearLocked();
            DropCapture();
        }
        why = DirectReady(tracker);
    }
    if (why) {
        c_refused.fetch_add(1, std::memory_order_relaxed);
        NoteReason(why);
        if (LogAllowed()) LOG_INFO(std::format("[OutdoorDelta] Lamp edit on story {}: through the room solve ({})", L, why));
        return false;
    }
    LevelLightShare::NoteGpuEdit(); // (the edit marks no room: its tail runs under the spread budget)
    PendingLot p;
    p.tracker = tracker;
    p.stories = 1u << L;
    QueryPerformanceCounter(&p.at);
    p.tick = GetTickCount();
    g_pending.push_back(p);
    c_taken.fetch_add(1, std::memory_order_relaxed);
    return true;
}
void AfterRoomUpdate(const void* tl) {
    if (g_dropPending.exchange(false)) g_pending.clear();
    if (g_pending.empty() || !tl) return;
    if (!g_on.load(std::memory_order_relaxed)) { // switched off with edits taken: through the room solve
        const std::vector<PendingLot> all = std::move(g_pending);
        g_pending.clear();
        for (const PendingLot& p : all) Abandon(p, "switched off", true);
        return;
    }
    int L = -99;
    const uintptr_t tracker = ReadTracker(reinterpret_cast<uintptr_t>(tl), L);
    // a lot whose update never came back (unloaded): its room 0s go through their solve
    const DWORD now = GetTickCount();
    for (size_t i = 0; i < g_pending.size();)
        if (g_pending[i].tracker != tracker && now - g_pending[i].tick > 1000) {
            const PendingLot p = g_pending[i];
            g_pending.erase(g_pending.begin() + static_cast<std::ptrdiff_t>(i));
            Abandon(p, "its lot was not updated again", false);
        } else
            i++;
    for (size_t i = 0; i < g_pending.size(); i++) {
        if (g_pending[i].tracker != tracker) continue;
        // another room's solve holds the class's working maps locked (a solve spread over frames): the next update, at
        // most 1 s, then through the room solve
        if (!LevelLightShare::RoomMapsFree()) {
            if (now - g_pending[i].tick <= 1000) return;
            const PendingLot p = g_pending[i];
            g_pending.erase(g_pending.begin() + static_cast<std::ptrdiff_t>(i));
            Abandon(p, "another room's maps stayed locked for 1 s", true);
            return;
        }
        PendingLot p = g_pending[i];
        g_pending.erase(g_pending.begin() + static_cast<std::ptrdiff_t>(i));
        DirectRun(p);
        return;
    }
}
bool DirectWriting() { return g_directWriting.load(std::memory_order_relaxed); }
bool HasPending() { return !g_pending.empty(); }

bool UpToDate(const void* roomPtr) {
    if (!g_on.load(std::memory_order_relaxed) || !roomPtr) return false;
    const BYTE* room = static_cast<const BYTE*>(roomPtr);
    RoomRead rr;
    int cls = -1;
    if (!ReadRoom(room, rr) || rr.id != 0 || !rr.outdoor || !Idle(room, cls)) return false;
    std::vector<ListLight> list;
    if (!UniqueList(room, list)) return false;
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    const auto it = g_records.find(KeyOf(room, cls));
    if (it == g_records.end()) return false;
    const Record& r = *it->second;
    if (ValidateBasic(r, room, rr) || r.lamps.size() != list.size()) return false;
    for (const ListLight& l : list) {
        const auto rec = std::find_if(r.lamps.begin(), r.lamps.end(), [&](const LampRec& x) { return x.light == l.light; });
        LightNow now;
        if (rec == r.lamps.end() || rec->mult != l.mult || !ReadLight(l.light, now) || now.sig != rec->sig) return false;
    }
    return LevelLightShare::LotGeometryKey(room) == r.geo;
}

// The lot played now (light tree thread, read again every 500 ms): the first lot lit at the high quality, kept while it
// stays among them (the list's order changes as the camera moves), as Real-time outdoor lamps and GPU room lamps pick it
uint64_t ActiveLot() {
    static uintptr_t s_tracker = 0;
    static uint64_t s_lot = 0;
    static DWORD s_at = 0;
    const DWORD now = GetTickCount();
    if (s_at && now - s_at < 500) return s_lot;
    s_at = now | 1;
    uintptr_t trackers[8];
    const int n = LevelLightShare::ActiveLotTrackers(trackers, 8);
    uintptr_t t = n > 0 ? trackers[0] : 0;
    for (int i = 0; i < n; i++)
        if (trackers[i] == s_tracker) t = s_tracker;
    s_tracker = t;
    s_lot = 0;
    RoomRead r0;
    if (const void* room0 = t ? LevelLightShare::Room0Of(t, 0) : nullptr; room0 && ReadRoom(static_cast<const unsigned char*>(room0), r0)) s_lot = r0.lot;
    return s_lot;
}
void SolveStart(unsigned char* room) {
    if (g_clear.exchange(false)) DropCapture();
    if (g_cap.active) DropCapture(); // the solve it kept was abandoned (restarted, or another room's)
    if (!g_on.load(std::memory_order_relaxed) || !room) return;
    RoomRead rr;
    if (!ReadRoom(room, rr)) return;
    if (rr.id != 0) { // another room of a lot with a lamp edit going on: its solve time (the log)
        const DWORD now = GetTickCount();
        std::lock_guard<std::mutex> lk(g_sideMx);
        if (g_lotEdits.count(rr.lot) && g_otherStart.size() < 512) g_otherStart[room] = {rr.lot, now};
        return;
    }
    if (!rr.outdoor || rr.cls < 0 || rr.cls > 2) return;
    if (rr.lot != ActiveLot()) return; // (14/10: parts of every lot's room 0 took 206 MB; edits elsewhere solve in full)
    // the conditions a delta needs, checked again at its lock step
    const char* refuse = !LevelLightShare::DeltaReady() ? "the room solve hooks are not in"
                         : RoomLightQueue::QuickPassTarget(room) > 0  ? "a quick pass"
                         : !LevelLightShare::ListAllWrapped(room)     ? "a light whose evaluation is not wrapped"
                                                                      : nullptr;
    if (refuse) {
        if (refuse[0] == 'a' && std::strcmp(refuse, "a light whose evaluation is not wrapped") == 0) {
            static std::atomic<int> s_lines{0};
            if (s_lines.fetch_add(1) < 20)
                LOG_INFO(std::format("[OutdoorDelta] Story {} room 0 class {}: the light whose evaluation is not wrapped: {}", rr.story, rr.cls,
                                     LevelLightShare::FirstUnwrappedLight(room)));
        }
        LogDrop(rr.story, rr.cls, "its parts are not kept from this solve", refuse);
        return;
    }
    Capture& c = g_cap;
    c.active = c.ok = true;
    c.room = room;
    c.rec = std::make_unique<Record>();
    Record& r = *c.rec;
    r.room = reinterpret_cast<uintptr_t>(room);
    r.mgr = rr.mgr;
    r.story = rr.story;
    r.lot = rr.lot;
    r.cls = rr.cls;
    r.gen = g_gen.load(std::memory_order_relaxed);
    r.physics = LevelLightShare::PhysicsKey();
    r.geo = LevelLightShare::LotGeometryKey(room);
    r.dither = DitherNow();
    r.thr = rr.thr;
    r.wallMode = rr.wallMode;
    // (14/10: the vectors at the size this room and class kept last time, so they grow once)
    // (14/10 review: only when the records are free; a sizing hint is not worth a wait)
    if (std::unique_lock<std::recursive_mutex> lk(g_mx, std::try_to_lock); lk.owns_lock()) {
        if (const auto it = g_records.find(KeyOf(room, rr.cls)); it != g_records.end()) try {
                r.texels.reserve(it->second->texels.size());
                r.samples.reserve(it->second->samples.size());
                r.batches.reserve(it->second->batches.size());
            } catch (...) {
            }
    }
    if (!r.geo) Fail("the lot's geometry could not be read");
    if (!UniqueList(room, c.startList)) Fail("the light list could not be read");
    for (const ListLight& l : c.startList) {
        LightNow now;
        if (!ReadLight(l.light, now)) {
            Fail("a light could not be read");
            break;
        }
        c.startSigs.push_back(now.sig);
    }
    // a self-check: the full solve's texels are compared with what the delta wrote
    bool check = false;
    {
        std::lock_guard<std::mutex> lk(g_sideMx);
        check = g_checkPending.erase(room) > 0;
    }
    if (check) {
        std::lock_guard<std::recursive_mutex> lk(g_mx);
        const auto it = g_records.find(KeyOf(room, rr.cls));
        if (it != g_records.end()) {
            const Record& o = *it->second;
            c.check = true;
            c.checkLightsSame = o.lamps.size() == c.startList.size();
            for (size_t i = 0; i < c.startList.size() && c.checkLightsSame; i++) {
                const auto l = std::find_if(o.lamps.begin(), o.lamps.end(), [&](const LampRec& x) { return x.light == c.startList[i].light; });
                c.checkLightsSame = l != o.lamps.end() && l->mult == c.startList[i].mult && i < c.startSigs.size() && l->sig == c.startSigs[i];
            }
            if (c.checkLightsSame) {
                c.checkOld.reserve(o.texels.size());
                for (const TexelRec& t : o.texels)
                    if (!(t.bits & kDup)) c.checkOld[TexelKey(t.map, t.x, t.y)] = t.packed;
            }
        }
    }
    c_captures.fetch_add(1, std::memory_order_relaxed);
}

bool AtLockStep(unsigned char* room, bool gather) {
    if (!g_on.load(std::memory_order_relaxed) || !room) return false;
    RoomRead rr;
    if (!ReadRoom(room, rr) || rr.id != 0 || !rr.outdoor || rr.cls < 0 || rr.cls > 2) return false;
    const uint64_t key = KeyOf(room, rr.cls);
    std::unique_lock<std::recursive_mutex> lk(g_mx);
    if (g_clear.exchange(false)) {
        ClearLocked();
        DropCapture();
    }
    const auto it = g_records.find(key);
    const char* why = nullptr;
    if (!gather) why = "a solve on another thread";
    else if (g_cap.active && g_cap.room == room && g_cap.check) why = "the self-check's full solve";
    else if (it == g_records.end()) why = "no kept parts for this room and class yet";
    LARGE_INTEGER t0{};
    QueryPerformanceCounter(&t0);
    Plan plan;
    bool done = false;
    if (!why) {
        Record& r = *it->second;
        why = ValidateBasic(r, room, rr);
        if (!why) why = ValidateMaps(r, room, rr.cls);
        if (!why && LevelLightShare::LotGeometryKey(room) != r.geo) why = "walls, floors or openings changed";
        if (!why) {
            const Delta d = ComputeDelta(room, rr, r, why, plan);
            if (d == Delta::NoChange) why = "no light of its list changed";
            else if (d == Delta::Ok) done = ApplyDelta(room, r, plan, why);
        }
        if (done) UpdateBytes(r);
    }
    const double ms = MsSince(t0);
    if (!done) {
        // a full solve rewrites the maps: what was kept of this room and class is no longer what they hold
        const bool had = it != g_records.end();
        EraseLocked(key);
        lk.unlock();
        c_full.fetch_add(1, std::memory_order_relaxed);
        NoteReason(why ? why : "?");
        {
            std::lock_guard<std::mutex> sk(g_sideMx);
            if (g_fullWhy.size() > 256) g_fullWhy.clear();
            g_fullWhy[room] = why ? why : "?";
        }
        const unsigned long edit = LampMarkFilter::EditLastTick();
        if (had || (edit && GetTickCount() - edit < kEditWindowMs))
            if (LogAllowed()) LOG_INFO(std::format("[OutdoorDelta] Outdoor lamp delta: story {} room 0 class {} solved in full ({})", rr.story, rr.cls, why ? why : "?"));
        return false;
    }
    lk.unlock();
    // the solve goes to its finalize: nothing of the passes is kept from it
    DropCapture();
    g_deltaRoom.store(room, std::memory_order_relaxed);
    c_deltas.fetch_add(1, std::memory_order_relaxed);
    c_texels.fetch_add(plan.texels, std::memory_order_relaxed);
    c_batches.fetch_add(plan.batches, std::memory_order_relaxed);
    g_lastMs.store(static_cast<float>(ms));
    if (static_cast<float>(ms) > g_maxMs.load()) g_maxMs.store(static_cast<float>(ms));
    NoteRoom0(rr.lot, rr.story, true, plan.texels, ms, plan.lights, nullptr, false);
    if (LogAllowed())
        LOG_INFO(std::format("[OutdoorDelta] Outdoor lamp delta: {}, story {} room 0 class {} (in its solve): {} texels rewritten, {} batches solved again, {} beyond reach "
                             "{:.1f} m left out, {:.1f} ms (no re-solve)",
                             LightsText(plan.lights), rr.story, rr.cls, plan.texels, plan.batches, plan.batchesSkipped, plan.reach, ms));
    if (g_check.load(std::memory_order_relaxed)) {
        std::lock_guard<std::mutex> sk(g_sideMx);
        if (g_checkDue.size() < 256) g_checkDue[room] = (GetTickCount() + kCheckDelayMs) | 1;
    }
    return true;
}

void SolveEnd(unsigned char* room) {
    if (!room) return;
    if (g_deltaRoom.load(std::memory_order_relaxed) == room) {
        g_deltaRoom.store(nullptr, std::memory_order_relaxed);
        return;
    }
    {
        const char* why = nullptr;
        std::pair<uint64_t, DWORD> other{0, 0};
        {
            std::lock_guard<std::mutex> sk(g_sideMx);
            if (const auto it = g_fullWhy.find(room); it != g_fullWhy.end()) {
                why = it->second;
                g_fullWhy.erase(it);
            }
            if (const auto it = g_otherStart.find(room); it != g_otherStart.end()) {
                other = it->second;
                g_otherStart.erase(it);
            }
        }
        RoomRead rr;
        if (why && ReadRoom(room, rr)) NoteRoom0(rr.lot, rr.story, false, 0, 0.0, {}, why, false);
        if (other.second) { // another room of a lot with a lamp edit going on
            const DWORD now = GetTickCount();
            std::lock_guard<std::mutex> sk(g_sideMx);
            if (const auto e = g_lotEdits.find(other.first); e != g_lotEdits.end()) {
                e->second.others++;
                e->second.othersMs += static_cast<long>(now - other.second);
                e->second.othersFinal = now;
                e->second.last = now;
            }
        }
    }
    if (!g_cap.active || g_cap.room != room) return;
    FlushPending();
    Capture& c = g_cap;
    std::vector<ListLight> list;
    if (c.ok && !UniqueList(room, list)) Fail("the light list could not be read");
    if (c.ok && c.directional) Fail("a directional map");
    if (c.ok && g_cap.pending >= 0) Fail("a texel was not read back");
    long unsure = 0;
    if (c.ok) {
        Record& r = *c.rec;
        // A light that changed during the budgeted solve (a drag) or entered or left the list meanwhile lit the batches before
        // the change one way and the others another: its parts are what the maps hold, but its state is not one state. It is
        // kept with no signature, so the next delta takes it as changed: its parts out, its whole light in again.
        for (size_t i = 0; i < list.size(); i++) {
            LightNow now;
            const auto start = std::find_if(c.startList.begin(), c.startList.end(), [&](const ListLight& s) { return s.light == list[i].light; });
            const size_t si = static_cast<size_t>(start - c.startList.begin());
            const bool same = start != c.startList.end() && start->mult == list[i].mult && si < c.startSigs.size() && ReadLight(list[i].light, now) && now.sig == c.startSigs[si];
            if (!same) unsure++;
            const auto li = c.lampIdx.find(list[i].light);
            if (li == c.lampIdx.end()) {
                c.lampIdx[list[i].light] = static_cast<int>(r.lamps.size());
                r.lamps.push_back(LampRec{});
                r.lamps.back().drawn = DrawnLight(list[i].light, r.lot);
            }
            LampRec& l = r.lamps[static_cast<size_t>(c.lampIdx[list[i].light])];
            l.light = list[i].light;
            l.mult = list[i].mult;
            l.sig = same ? now.sig : 0;
            l.type = same ? now.type : 0;
        }
        // lights that added light and left the list: kept with no signature and no place in the list (the next delta takes
        // their parts out)
        for (LampRec& l : r.lamps)
            if (l.mult <= 0) {
                l.mult = 0;
                l.sig = 0;
                unsure++;
            }
        if (c.ok && (LevelLightShare::LotGeometryKey(room) != r.geo || LevelLightShare::PhysicsKey() != r.physics || r.gen != g_gen.load()))
            Fail("walls, floors, openings or options changed during the solve");
        if (c.ok && r.texels.empty()) Fail("no texel");
    }
    if (c.ok) {
        Record& r = *c.rec;
        // the class-2 wall texels' blocks (BlurWalls' jobs)
        if (r.passes > 0 && r.maps[0].used && r.maps[0].w && r.maps[0].h) {
            std::vector<uint16_t> grid(static_cast<size_t>(r.maps[0].w) * r.maps[0].h, kNoBlock);
            for (size_t j = 0; j < r.blocks.size() && j < kBadBlock; j++) {
                const BlockRec& b = r.blocks[j];
                for (int y = std::max(b.y0, 0); y <= b.y1 && y < static_cast<int>(r.maps[0].h); y++)
                    for (int x = std::max(b.x0, 0); x <= b.x1 && x < static_cast<int>(r.maps[0].w); x++)
                        grid[static_cast<size_t>(y) * r.maps[0].w + x] = b.ok ? static_cast<uint16_t>(j) : kBadBlock;
            }
            for (TexelRec& t : r.texels)
                if (t.map == 0) t.block = t.x < r.maps[0].w && t.y < r.maps[0].h ? grid[static_cast<size_t>(t.y) * r.maps[0].w + t.x] : kBadBlock;
            for (const TexelRec& t : r.texels)
                if (t.map == 0 && (t.block == kNoBlock || t.block == kBadBlock)) {
                    r.wallsOk = false;
                    break;
                }
        }
        if (!r.wallsOk)
            for (TexelRec& t : r.texels)
                if (t.map == 0) t.block = kBadBlock; // a delta that touches a wall texel solves in full
        // texels two samples write (rewritten in the solve's order by a delta)
        {
            std::unordered_map<uint64_t, uint32_t> seen;
            seen.reserve(r.texels.size());
            for (uint32_t i = 0; i < r.texels.size(); i++) {
                const auto [it, fresh] = seen.try_emplace(TexelKey(r.texels[i].map, r.texels[i].x, r.texels[i].y), i);
                if (!fresh) {
                    r.texels[i].bits |= kDup;
                    r.texels[it->second].bits |= kDup;
                }
            }
        }
        // each batch's bounding sphere (the reach test)
        for (BatchRec& b : r.batches) {
            double cx = 0, cy = 0, cz = 0;
            for (uint32_t i = 0; i < b.count; i++) {
                const float* p = reinterpret_cast<const float*>(r.samples.data() + static_cast<size_t>(b.first + i) * 0x30);
                cx += p[0], cy += p[1], cz += p[2];
            }
            b.c[0] = static_cast<float>(cx / b.count), b.c[1] = static_cast<float>(cy / b.count), b.c[2] = static_cast<float>(cz / b.count);
            float rad = 0.0f;
            for (uint32_t i = 0; i < b.count; i++) {
                const float* p = reinterpret_cast<const float*>(r.samples.data() + static_cast<size_t>(b.first + i) * 0x30);
                const float dx = p[0] - b.c[0], dy = p[1] - b.c[1], dz = p[2] - b.c[2];
                rad = std::max(rad, ApexMath::Sqrt(dx * dx + dy * dy + dz * dz));
            }
            b.r = rad + 0.01f;
        }
        r.used = GetTickCount();
        r.version = g_version.fetch_add(1, std::memory_order_relaxed) + 1;
        r.baseVersion = r.version;
        if (RtOutdoorLamps::Watching()) r.baseHash = BaseHash(r);
        ShrinkRecord(r); // (outside g_mx: the copies take a while on a large room)
        const uint64_t key = KeyOf(room, r.cls);
        const int story = r.story, cls = r.cls;
        {
            std::lock_guard<std::recursive_mutex> lk(g_mx);
            // (14/10) a room of the active lot Real-time outdoor lamps relights keeps its drawn lamps as one sum (the base is
            // unchanged: computed above from the same parts)
            const bool pinnedNew = PinnedLocked(reinterpret_cast<uintptr_t>(room));
            const size_t drawnBefore = PartsBytes(r, true);
            if (pinnedNew && RtOutdoorLamps::Watching()) CollapseDrawn(r);
            ShrinkDrawnParts(r); // (the collapsed ones are empty)
            r.bytes = BytesOf(r);
            {
                // the same base as the room's last kept solve: its version stays (the relit maps stand)
                const auto old = g_records.find(key);
                if (r.baseHash && old != g_records.end() && old->second->baseHash == r.baseHash && old->second->baseVersion) r.baseVersion = old->second->baseVersion;
            }
            EraseLocked(key);
            // A pinned room never drops another pinned room: if the pinned rooms with it would pass kMaxPinnedBytes, this solve
            // is not kept (the others stay, so the active lot's stories do not evict one another in a loop). Nor is another
            // room's solve that would pass it beside the pinned rooms: the loop below never drops the new record, so it would
            // stay over the ceiling (with no pinned room the check never refuses: a record is at most kMaxRecordBytes)
            size_t pinnedBytes = 0;
            for (const auto& [k2, rec] : g_records)
                if (PinnedLocked(rec->room)) pinnedBytes += rec->bytes;
            const bool refused = pinnedBytes + r.bytes > kMaxPinnedBytes;
            if (refused) {
                LOG_INFO(std::format("[OutdoorDelta] Story {} room 0 class {}: not kept ({:.1f} MB; the active lot's rooms already keep {:.1f} MB of {} MB allowed; "
                                     "{} texels: samples {:.1f} MB, texels {:.1f} MB, baked lights' parts {:.1f} MB, drawn lamps {:.1f} MB before their sum)",
                                     story, cls, static_cast<double>(r.bytes) / (1 << 20), static_cast<double>(pinnedBytes) / (1 << 20), kMaxPinnedBytes >> 20,
                                     r.texels.size(), static_cast<double>(r.samples.capacity()) / (1 << 20),
                                     static_cast<double>(r.texels.capacity() * sizeof(TexelRec)) / (1 << 20), static_cast<double>(PartsBytes(r, false)) / (1 << 20),
                                     static_cast<double>(drawnBefore) / (1 << 20)));
                NoteReason("memory budget (the active lot is full)");
                LogDrop(story, cls, "its parts are not kept from this solve", "the memory budget of the active lot is full");
                c_pinnedDrops.fetch_add(1);
                c.rec.reset();
            } else {
                g_bytes.fetch_add(r.bytes);
                g_records[key] = std::move(c.rec);
            }
            // the oldest rooms go past the budget: never a pinned one (the active lot's room 0s, bounded above)
            while (g_bytes.load() > kMaxBytes && g_records.size() > 1) {
                auto oldest = g_records.end();
                for (auto it = g_records.begin(); it != g_records.end(); ++it)
                    if (it->first != key && !PinnedLocked(it->second->room) &&
                        (oldest == g_records.end() || static_cast<int32_t>(it->second->used - oldest->second->used) < 0))
                        oldest = it;
                if (oldest == g_records.end()) break;
                LOG_INFO(std::format("[OutdoorDelta] Story {} room 0 class {}: kept parts dropped (memory budget: {:.1f} MB of {:.1f} MB kept in {} rooms, {} MB allowed)",
                                     oldest->second->story, oldest->second->cls, static_cast<double>(oldest->second->bytes) / (1 << 20),
                                     static_cast<double>(g_bytes.load()) / (1 << 20), g_records.size(), kMaxBytes >> 20));
                NoteReason("memory budget");
                c_budgetDrops.fetch_add(1, std::memory_order_relaxed);
                EraseLocked(oldest->first);
            }
            // tight: logged once each time the kept parts pass kTightBytes, with what fills them
            static bool s_tight = false;
            const size_t total = g_bytes.load();
            if (!s_tight && total > kTightBytes) {
                size_t samples = 0, texels = 0, baked = 0, drawn = 0, sums = 0;
                for (const auto& [k2, rec] : g_records) {
                    samples += rec->samples.capacity(), texels += rec->texels.capacity() * sizeof(TexelRec);
                    baked += PartsBytes(*rec, false), drawn += PartsBytes(*rec, true), sums += rec->drawnSum.capacity() * sizeof(DrawnSum);
                }
                LOG_INFO(std::format("[OutdoorDelta] Memory budget tight: {:.1f} MB kept in {} rooms ({} MB allowed for the active lot): samples {:.1f} MB, texels "
                                     "{:.1f} MB, baked lights' parts {:.1f} MB, drawn lamps' parts {:.1f} MB, drawn sums {:.1f} MB",
                                     static_cast<double>(total) / (1 << 20), g_records.size(), kMaxPinnedBytes >> 20, static_cast<double>(samples) / (1 << 20),
                                     static_cast<double>(texels) / (1 << 20), static_cast<double>(baked) / (1 << 20), static_cast<double>(drawn) / (1 << 20),
                                     static_cast<double>(sums) / (1 << 20)));
            }
            s_tight = total > kTightBytes * 9 / 10 && (s_tight || total > kTightBytes); // (re-armed once it falls 10% under)
            c_records.store(static_cast<long>(g_records.size()));
        }
        if (unsure && LogAllowed())
            LOG_INFO(std::format("[OutdoorDelta] Story {} room 0 class {}: parts kept, {} lights changed during the solve (the next delta takes them as changed)", story, cls, unsure));
        if (c.check) {
            c_checks.fetch_add(1, std::memory_order_relaxed);
            if (!c.checkLightsSame)
                LOG_INFO(std::format("[OutdoorDelta] Self-check: story {} room 0 class {}: not compared (its lights changed since the delta)", story, cls));
            else {
                if (c.cmpOver1) c_checkBad.fetch_add(1, std::memory_order_relaxed);
                c_checkMax.store(std::max<long>(c_checkMax.load(), c.cmpMax));
                LOG_INFO(std::format("[OutdoorDelta] Self-check: story {} room 0 class {}: {} texels compared with the full solve, largest difference {}/255 ({} texels "
                                     "differ by 1, {} by more){}",
                                     story, cls, c.cmpN, c.cmpMax, c.cmpAt1, c.cmpOver1, c.cmpOver1 ? " - MISMATCH" : ""));
            }
        }
    } else {
        RoomRead rr;
        if (ReadRoom(room, rr)) LogDrop(rr.story, rr.cls, "its parts are not kept from this solve", c.why ? c.why : "?");
        if (c.check) LOG_INFO(std::format("[OutdoorDelta] Self-check of a room 0: its full solve was not kept ({}), not compared", c.why ? c.why : "?"));
    }
    DropCapture();
}

bool DeltaSolving(const void* room) { return room && g_deltaRoom.load(std::memory_order_relaxed) == room; }
void NoteDirectional(const void* room) {
    if (g_cap.active && g_cap.room == room) g_cap.directional = true;
}

bool Sample(const unsigned char* room) {
    if (!g_cap.active || !g_cap.ok || g_cap.room != room) return false;
    SampleState& s = g_s;
    s.on = true;
    s.tap = -1;
    s.pending = 0;
    s.n = 0;
    s.overflow = false;
    s.masked = s.gated = false;
    s.visLum = s.allLum = 0.0f;
    return true;
}
namespace {
Acc* AccFor(uintptr_t light) {
    SampleState& s = g_s;
    for (int i = 0; i < s.n; i++)
        if (s.acc[i].light == light) return &s.acc[i];
    if (s.n >= static_cast<int>(std::size(s.acc))) {
        s.overflow = true;
        return nullptr;
    }
    Acc& a = s.acc[s.n++];
    a = Acc{light, {0.0f, 0.0f, 0.0f}, 0.0f, 0.0f};
    return &a;
}
void FlushTap() {
    SampleState& s = g_s;
    if (!s.pending || s.tap < 0 || !s.sum) return;
    const uintptr_t L = s.pending;
    s.pending = 0;
    const float d[3] = {s.sum[0] - s.snap[0], s.sum[1] - s.snap[1], s.sum[2] - s.snap[2]};
    if (d[0] == 0.0f && d[1] == 0.0f && d[2] == 0.0f) return;
    Acc* a = AccFor(L);
    if (!a) return;
    if (s.tap == 0)
        for (int i = 0; i < 3; i++) a->rgb[i] += d[i];
    else if (s.tap == 1) a->vis += Lum3(d);
    else a->all += Lum3(d);
}
} // namespace
void Tap(int which, const float* sum) {
    SampleState& s = g_s;
    if (!s.on) return;
    s.tap = which;
    s.sum = sum;
    s.pending = 0;
}
void TapEnd() {
    SampleState& s = g_s;
    if (!s.on) return;
    FlushTap();
    s.tap = -1;
    s.sum = nullptr;
}
void OnEval(uintptr_t light) {
    SampleState& s = g_s;
    if (!s.on || s.tap < 0 || !s.sum) return;
    FlushTap();
    s.pending = light;
    for (int i = 0; i < 3; i++) s.snap[i] = s.sum[i];
}
void OnFuse(uintptr_t light, const float* before, const float* after) {
    SampleState& s = g_s;
    if (!s.on) return;
    const float d[3] = {after[0] - before[0], after[1] - before[1], after[2] - before[2]};
    if (d[0] == 0.0f && d[1] == 0.0f && d[2] == 0.0f) return;
    if (Acc* a = AccFor(light)) a->all += Lum3(d);
}
void DropFused() {
    SampleState& s = g_s;
    if (!s.on) return;
    for (int i = 0; i < s.n; i++) s.acc[i].all = 0.0f;
}
void Mask(bool masked, bool gated, float visLum, float allLum) {
    SampleState& s = g_s;
    if (!s.on) return;
    s.masked = masked;
    s.gated = gated;
    s.visLum = visLum;
    s.allLum = allLum;
}

void SampleEnd(unsigned char* room, const void* sample, const char* flags, const float* out) {
    SampleState& s = g_s;
    if (!s.on) return;
    s.on = false;
    if (!g_cap.active || g_cap.room != room) return;
    FlushPending(); // the previous sample's texel is in its map by now
    if (!g_cap.ok) return;
    Capture& c = g_cap;
    Record& r = *c.rec;
    if (s.overflow) return Fail("too many lights at one texel");
    RoomRead rr;
    if (!ReadRoom(room, rr)) return Fail("the room could not be read");
    const int m = rr.ec - 2;
    if (m < 0 || m > 3) return Fail("a map the delta does not keep");
    MapGeo& g = r.maps[m];
    if (!g.used) {
        Desc d;
        if (!ReadDesc(room, m, d)) return Fail("a map is not locked");
        g.used = true;
        g.pitch = d.pitch, g.w = d.w, g.h = d.h;
        g.tex = SlotTexture(rr.cls, m);
        if (!g.tex) return Fail("a map's texture is not known");
    }
    // the batch: the vector the solve's callers pass (rows lit beyond a wall's edges are another vector: not kept)
    uintptr_t vb = 0, ve = 0;
    const uintptr_t sp = reinterpret_cast<uintptr_t>(sample);
    if (!ReadBatchVec(vb, ve) || sp < vb || sp >= ve || (sp - vb) % 0x30) return Fail("a sample outside the solve's batch");
    const uint32_t index = static_cast<uint32_t>((sp - vb) / 0x30);
    if (index == 0) {
        BatchRec b;
        b.first = static_cast<uint32_t>(r.texels.size());
        b.count = static_cast<uint32_t>((ve - vb) / 0x30);
        if (!flags || !CopyBytes(b.flags, flags, 2)) return Fail("the batch's switches could not be read");
        try {
            r.batches.push_back(b);
        } catch (...) {
            return Fail("no memory");
        }
    } else if (r.batches.empty() || r.texels.size() - r.batches.back().first != index || index >= r.batches.back().count)
        return Fail("a batch's samples out of order");
    TexelRec t;
    uint8_t raw[0x30];
    if (!CopyBytes(raw, sample, 0x30)) return Fail("a sample could not be read");
    std::memcpy(&t.x, raw + 0x20, 2);
    std::memcpy(&t.y, raw + 0x22, 2);
    if (t.x >= g.w || t.y >= g.h) return Fail("a texel outside its map");
    t.map = static_cast<uint8_t>(m);
    t.bits = static_cast<uint8_t>((s.masked ? kMasked : 0) | (s.gated ? kGated : 0));
    for (int i = 0; i < 3; i++) t.t.S[i] = out[i];
    t.t.vis = s.masked ? s.visLum : 0.0f;
    t.t.all = s.masked ? s.allLum : 0.0f;
    const uint32_t texel = static_cast<uint32_t>(r.texels.size());
    try {
        r.texels.push_back(t);
        r.samples.insert(r.samples.end(), raw, raw + 0x30);
        for (int i = 0; i < s.n; i++) {
            const Acc& a = s.acc[i];
            Contrib p;
            p.texel = texel;
            for (int q = 0; q < 3; q++) p.rgb[q] = a.rgb[q];
            p.vis = s.masked ? ToHalf(a.vis) : 0;
            p.all = s.masked ? ToHalf(a.all) : 0;
            if (!Nonzero(p)) continue;
            auto [it, fresh] = c.lampIdx.try_emplace(a.light, static_cast<int>(r.lamps.size()));
            if (fresh) {
                r.lamps.push_back(LampRec{});
                r.lamps.back().light = a.light;
                r.lamps.back().drawn = DrawnLight(a.light, r.lot);
            }
            r.lamps[static_cast<size_t>(it->second)].parts.push_back(p);
        }
    } catch (...) {
        return Fail("no memory");
    }
    c.pending = static_cast<int>(texel);
    if ((texel & 4095) == 0 && BytesOf(r) > kMaxRecordBytes) Fail("a room too large to keep");
}

void BeforeFinalize(unsigned char* room) {
    if (g_cap.active && g_cap.room == room) FlushPending();
}

void NoteWallBlur(const unsigned char* room, const WallBlockView* blocks, int count, uint32_t passes) {
    if (!g_cap.active || g_cap.room != room) return;
    FlushPending(); // the wall pass's last texel, before the blur
    if (!g_cap.ok) return;
    Record& r = *g_cap.rec;
    r.passes = passes;
    r.blocks.clear();
    r.preblur.clear();
    for (int j = 0; j < count; j++) {
        const WallBlockView& v = blocks[j];
        BlockRec b;
        b.x0 = v.x0, b.y0 = v.y0, b.x1 = v.x1, b.y1 = v.y1;
        b.ok = v.before && !v.ghosts && v.x1 >= v.x0 && v.y1 >= v.y0;
        if (b.ok) {
            b.off = static_cast<uint32_t>(r.preblur.size());
            r.preblur.insert(r.preblur.end(), v.before, v.before + static_cast<size_t>(v.x1 - v.x0 + 1) * (v.y1 - v.y0 + 1));
        }
        r.blocks.push_back(b);
    }
}
void NoteGameBlur(const unsigned char* room) {
    if (!g_cap.active || g_cap.room != room) return;
    FlushPending();
    RoomRead rr;
    if (g_cap.ok && ReadRoom(room, rr) && rr.cls == 2) g_cap.rec->wallsOk = false;
}

bool Replaying() { return g_replayBase != nullptr; }
void ReplaySample(const void* sample, const float* out, bool masked, float visLum, float allLum) {
    if (!g_replayBase || !sample || !out) return;
    const uintptr_t sp = reinterpret_cast<uintptr_t>(sample), b = reinterpret_cast<uintptr_t>(g_replayBase);
    if (sp < b || (sp - b) % 0x30) return;
    const size_t i = (sp - b) / 0x30;
    if (i >= g_replay.size()) return;
    ReplayOut& o = g_replay[i];
    for (int q = 0; q < 3; q++) o.rgb[q] = out[q];
    o.vis = masked ? visLum : 0.0f;
    o.all = masked ? allLum : 0.0f;
    o.set = true;
}

// (left_lot_release) The kept parts of a room whose story manager no loaded lot holds any more (its lot left the game's lot
// list, or the lot's lighting was built again with new managers). ValidateBasic accepts a record only for a room whose
// manager is the record's (the room changed otherwise), so such a record can never serve a delta again: it only held its
// share of kMaxBytes, and the budget then dropped the parts of live rooms first by age. Every kSweepMs in a live world (never
// while one loads, nor when the lot list was not read in full); a record goes at the second sweep in a row that misses its
// manager (LivenessPrune), so a lot streaming in keeps its parts.
static void SweepLeftLots(DWORD now) {
    constexpr DWORD kSweepMs = 10000;
    static DWORD s_at = 0;
    if (!DevSwitches::LeftLotRelease() || (s_at && now - s_at < kSweepMs)) return;
    s_at = now | 1;
    LivenessPrune::LiveSet mgrs;
    if (!LevelLightShare::LiveStoryManagers(mgrs) || !mgrs.Size()) return;
    size_t bytes = 0, rooms = 0, left = 0;
    {
        // (a worker copying an export holds g_mx for tens of ms: the render thread never waits for it; tried again next frame)
        std::unique_lock<std::recursive_mutex> lk(g_mx, std::try_to_lock);
        if (!lk.owns_lock()) {
            s_at = 0;
            return;
        }
        rooms = LivenessPrune::PruneInPlace(
            g_records, [&mgrs](uint64_t, const std::unique_ptr<Record>& r) { return mgrs.Contains(r->mgr); },
            [](std::unique_ptr<Record>& r) -> uint8_t& { return r->gone; },
            [&bytes](uint64_t, std::unique_ptr<Record>& r) {
                bytes += r->bytes;
                g_bytes.fetch_sub(std::min(g_bytes.load(), r->bytes));
            });
        c_records.store(static_cast<long>(g_records.size()));
        left = g_records.size();
    }
    if (!rooms) return;
    c_leftLotDrops.fetch_add(static_cast<long>(rooms), std::memory_order_relaxed);
    c_leftLotBytes.fetch_add(bytes, std::memory_order_relaxed);
    ApexMemory::NoteReleased("Quick outdoor lamp edits: kept parts of rooms whose lot left", bytes);
    LOG_INFO(std::format("[OutdoorDelta] Kept parts of {} rooms whose lot is no longer loaded given back ({:.1f} MB; {:.1f} MB kept in {} rooms)", rooms,
                         static_cast<double>(bytes) / (1 << 20), static_cast<double>(g_bytes.load()) / (1 << 20), left));
}

void OnPresent() {
    const DWORD now = GetTickCount();
    HookGuard::Run("OutdoorDelta left lot sweep", [now] { SweepLeftLots(now); });
    std::vector<const void*> due;
    std::vector<std::pair<uint64_t, LotEdit>> done;
    {
        std::lock_guard<std::mutex> lk(g_sideMx);
        for (auto it = g_checkDue.begin(); it != g_checkDue.end();)
            if (static_cast<int32_t>(now - it->second) >= 0) {
                due.push_back(it->first);
                it = g_checkDue.erase(it);
            } else
                ++it;
        for (auto it = g_lotEdits.begin(); it != g_lotEdits.end();)
            if (now - it->second.last >= kSummaryQuietMs) {
                done.emplace_back(it->first, std::move(it->second));
                it = g_lotEdits.erase(it);
            } else
                ++it;
        if (g_otherStart.size() > 256) g_otherStart.clear();
    }
    for (const void* room : due) {
        if (!g_check.load(std::memory_order_relaxed) || !g_on.load(std::memory_order_relaxed)) break;
        if (LevelLightShare::QueueRoomAgain(room)) {
            std::lock_guard<std::mutex> lk(g_sideMx);
            if (g_checkPending.size() < 256) g_checkPending.insert(room);
        }
    }
    // one line per edit (a drag: once it is let go): when every room 0 was final, and what the other rooms took
    for (const auto& [lot, e] : done) {
        if (!e.deltas && !e.full) continue;
        LOG_INFO(std::format("[OutdoorDelta] Lamp edit summary: {}, lot {:08X} stories {}..{}: {} room 0 deltas ({} without a solve), {} room 0 solves in full{}; every room "
                             "0 final {} ms after the edit; other rooms of the lot solved meanwhile {} ({} ms of solves, the last {} ms after the edit)",
                             LightsText(e.lights), static_cast<uint32_t>(lot), e.story0, e.story1, e.deltas, e.direct, e.full,
                             e.why.empty() ? std::string() : std::format(" ({})", e.why), e.room0Final ? static_cast<long>(e.room0Final - e.edit) : -1L, e.others,
                             e.othersMs, e.othersFinal ? static_cast<long>(e.othersFinal - e.edit) : -1L));
    }
}

void OnWorldChanged() {
    {
        std::lock_guard<std::recursive_mutex> lk(g_mx);
        ClearLocked();
    }
    g_clear.store(true);
    g_dropPending.store(true);
    std::lock_guard<std::mutex> lk(g_sideMx);
    g_checkDue.clear();
    g_checkPending.clear();
    g_lotEdits.clear();
    g_otherStart.clear();
}
void InvalidateAll(const char* why) {
    (void)why;
    g_gen.fetch_add(1, std::memory_order_relaxed);
}

std::string LastDrop(int story, int cls) {
    if (story < 0 || story >= 8 || cls < 0 || cls >= 3) return {};
    std::lock_guard<std::mutex> lk(g_sideMx);
    return g_lastDrop[story][cls];
}
// (14/10) render thread, every frame: never waits for a worker's export copy holding g_mx (tens of ms); the pins of the
// last frame stay a frame longer
long PinRooms(const void* const* rooms, int n) {
    std::unique_lock<std::recursive_mutex> lk(g_mx, std::try_to_lock);
    if (lk.owns_lock()) {
        g_pinned.assign(rooms, rooms + std::max(n, 0));
        // (14/10) a room kept before it was pinned (a solve at the load, before the lot was the active one) takes the drawn
        // sum now; the deltas read the parts under this lock too
        if (RtOutdoorLamps::Watching())
            for (auto& [key, rec] : g_records)
                if (PinnedLocked(rec->room)) {
                    const size_t before = rec->bytes;
                    CollapseDrawn(*rec);
                    rec->bytes = BytesOf(*rec);
                    g_bytes.fetch_add(rec->bytes - before); // (modulo 2^32: a smaller record subtracts)
                }
    }
    return c_pinnedDrops.load();
}
size_t KeptBytes() { return g_bytes.load(); }
std::string MemoryText() {
    size_t samples = 0, texels = 0, baked = 0, drawn = 0, sums = 0, other = 0, n = 0;
    {
        std::unique_lock<std::recursive_mutex> lk(g_mx, std::try_to_lock); // (the render thread never waits for a worker's export copy)
        if (!lk.owns_lock()) return std::format("{:.1f} MB (the records were busy)", static_cast<double>(g_bytes.load()) / (1 << 20));
        for (const auto& [k, rec] : g_records) {
            samples += rec->samples.capacity(), texels += rec->texels.capacity() * sizeof(TexelRec);
            baked += PartsBytes(*rec, false), drawn += PartsBytes(*rec, true), sums += rec->drawnSum.capacity() * sizeof(DrawnSum);
            other += rec->batches.capacity() * sizeof(BatchRec) + rec->blocks.capacity() * sizeof(BlockRec) + rec->preblur.capacity() * 4;
        }
        n = g_records.size();
    }
    const auto mb = [](size_t b) { return static_cast<double>(b) / (1 << 20); };
    return std::format("{:.1f} MB in {} rooms (samples {:.1f}, texels {:.1f}, baked lights' parts {:.1f}, drawn lamps' parts {:.1f}, drawn sums {:.1f}, batches and "
                       "wall blocks {:.1f}); dropped by the budget {}, given back after their lot left {} ({:.1f} MB)",
                       mb(g_bytes.load()), n, mb(samples), mb(texels), mb(baked), mb(drawn), mb(sums), mb(other), c_budgetDrops.load(), c_leftLotDrops.load(),
                       mb(c_leftLotBytes.load()));
}
size_t PinnedBudgetBytes() { return kMaxPinnedBytes; }
int KeptRooms() { return static_cast<int>(c_records.load()); }

// (14/10) a record is the room's only while the room still has its manager and story (a room 0 made again at a freed
// room's address inherited another room's texels until its first solve)
bool RecordOfRoom(const Record& r, const RoomRead& now) { return r.mgr == now.mgr && r.story == now.story; }
std::mutex g_versionMx;
std::unordered_map<uint64_t, uint32_t> g_versionSeen; // under g_versionMx: the last answer per key (when g_mx is busy)
uint32_t RecordVersion(const void* room, int cls) {
    if (!room || cls < 0 || cls > 2) return 0;
    RoomRead now;
    if (!ReadRoom(static_cast<const unsigned char*>(room), now)) return 0;
    const uint64_t key = KeyOf(room, cls);
    uint32_t v = 0;
    {
        // (14/10) the render thread asks every frame: a worker's export copy holding g_mx (tens of ms) gets the last answer
        std::unique_lock<std::recursive_mutex> lk(g_mx, std::try_to_lock);
        if (!lk.owns_lock()) {
            std::lock_guard<std::mutex> vk(g_versionMx);
            const auto s = g_versionSeen.find(key);
            return s == g_versionSeen.end() ? 0 : s->second;
        }
        const auto it = g_records.find(key);
        v = it == g_records.end() || !RecordOfRoom(*it->second, now) ? 0 : it->second->baseVersion;
    }
    std::lock_guard<std::mutex> vk(g_versionMx);
    if (g_versionSeen.size() > 256) g_versionSeen.clear();
    g_versionSeen[key] = v;
    return v;
}
bool ExportRoom0(const void* room, int cls, Room0Export& out) {
    out = Room0Export{};
    if (!room || cls < 0 || cls > 2) return false;
    RoomRead now;
    if (!ReadRoom(static_cast<const unsigned char*>(room), now)) return false;
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    const auto it = g_records.find(KeyOf(room, cls));
    if (it == g_records.end() || !RecordOfRoom(*it->second, now)) return false;
    const Record& r = *it->second;
    if (r.samples.size() != r.texels.size() * 0x30) return false;
    out.version = r.baseVersion;
    out.story = r.story, out.cls = r.cls, out.lot = r.lot, out.thr = r.thr, out.dither = r.dither, out.wallsOk = r.wallsOk, out.passes = r.passes;
    for (int m = 0; m < 4; m++) out.mapUsed[m] = r.maps[m].used, out.mapW[m] = r.maps[m].w, out.mapH[m] = r.maps[m].h;
    try {
        out.texels.resize(r.texels.size());
        for (size_t i = 0; i < r.texels.size(); i++) {
            const TexelRec& t = r.texels[i];
            Room0Export::Texel& e = out.texels[i];
            const float* s = reinterpret_cast<const float*>(r.samples.data() + i * 0x30);
            for (int k = 0; k < 3; k++) e.pos[k] = s[k], e.nrm[k] = s[4 + k], e.base[k] = t.t.S[k];
            e.vis = t.t.vis, e.all = t.t.all;
            if (i < r.drawnSum.size()) {
                const DrawnSum& d = r.drawnSum[i];
                for (int k = 0; k < 3; k++) e.base[k] -= d.rgb[k];
                e.vis -= d.vis, e.all -= d.all;
            }
            e.packed = t.packed;
            e.x = t.x, e.y = t.y, e.map = t.map;
            e.bits = static_cast<uint8_t>(((t.bits & kMasked) ? 1 : 0) | ((t.bits & kGated) ? 2 : 0));
        }
        for (const LampRec& l : r.lamps) {
            out.lamps++;
            out.kept.push_back(l.light);
            if (!l.drawn) continue;
            out.drawn.push_back(l.light);
            for (const Contrib& p : l.parts) {
                if (p.texel >= out.texels.size()) continue;
                Room0Export::Texel& e = out.texels[p.texel];
                for (int k = 0; k < 3; k++) e.base[k] -= p.rgb[k];
                e.vis -= FromHalf(p.vis);
                e.all -= FromHalf(p.all);
            }
        }
        for (const BlockRec& b : r.blocks) out.blocks.push_back(Room0Export::Block{b.x0, b.y0, b.x1, b.y1, b.ok, b.off});
        out.preblur.assign(r.preblur.begin(), r.preblur.end());
    } catch (...) {
        out = Room0Export{};
        return false;
    }
    return true;
}

std::string Status() {
    std::string reasons;
    {
        std::lock_guard<std::mutex> lk(g_sideMx);
        std::vector<std::pair<long, std::string>> v;
        for (const auto& [w, n] : g_reasons) v.emplace_back(n, w);
        std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        for (size_t i = 0; i < v.size() && i < 8; i++) reasons += std::format("{}{} {}", i ? ", " : "", v[i].second, v[i].first);
    }
    if (!g_on.load()) return "off";
    const long checkMax = c_checkMax.load();
    return std::format("on: lamp edits taken {} (refused {}), direct runs {} ({} rooms rewritten, {} sent to their solve; last {:.1f} ms), deltas in a solve {} ({} "
                       "texels, {} batches solved again; longest {:.1f} ms), room 0 solves in full {} | reasons: {} | solves kept {} ({} not kept), rooms kept {} "
                       "({:.1f} MB) | self-check {}: {} compared, largest difference {}, {} with a texel off by more than 1",
                       c_taken.load(), c_refused.load(), c_direct.load(), c_directRooms.load(), c_directFallbacks.load(), g_lastDirectMs.load(), c_deltas.load(),
                       c_texels.load(), c_batches.load(), g_maxMs.load(), c_full.load(), reasons.empty() ? "none" : reasons, c_captures.load(), c_captureFails.load(),
                       c_records.load(), static_cast<double>(g_bytes.load()) / (1 << 20), g_check.load() ? "on" : "off", c_checks.load(),
                       checkMax < 0 ? std::string("-") : std::format("{}/255", checkMax), c_checkBad.load());
}

} // namespace OutdoorDelta
