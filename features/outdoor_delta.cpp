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
#include "rt_lamp_proxy.h"
#include "rt_lamp_gpu.h"
#include "drag_lite.h"
#include "gpu_room_lamps.h"
#include "level_light_share.h"
#include "room_light_queue.h"
#include "room_map_dither.h"
#include "room_lamp_move.h"
#include "inner_retry.h"
#include "phys_lamp_pick.h"
#include "recorder.h"
#include "probe_neutral.h"
#include "penumbra.h"
#include "apex_util.h"
#include "scene_record.h"
#include "lamp_mark_filter.h"
#include "object_legs.h"
#include "unlit_rooms.h"
#include "apex_log.h"
#include "apex_paths.h"
#include "game_addresses.h"
#include "game_version.h"
#include "hook_guard.h"
#include "object_light_bridge.h"
#include "dev_switches.h"
#include "build_flavor.h"
#include "apex_memory.h"
#include <bit>
#include <span>
#include <windows.h>
#include <d3d9.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cmath>
#include <cstring>
#include <format>
#include <filesystem>
#include <map>
#include <set>
#include <memory>
#include <tuple>
#include <thread>
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
// A game blocker's vtable as the copies name it (room_light_tests.h: Steam's values, also in the probe dumps and replays); a
// Steam value met on another build is some other kind there (1: unknown)
uint32_t TestVt(uint32_t vt) {
    static const uint32_t quad = static_cast<uint32_t>(GameAddr::Get(GameAddr::Id::BlockerQuadVt)), disc = static_cast<uint32_t>(GameAddr::Get(GameAddr::Id::BlockerDiscVt));
    if (vt == quad) return RoomLightTests::kQuadVt;
    if (vt == disc) return RoomLightTests::kDiscVt;
    return vt == RoomLightTests::kQuadVt || vt == RoomLightTests::kDiscVt ? 1u : vt;
}
constexpr int kMaxChanged = 8;                    // more changed lights: solved in full
// (14/10: 160 MB, 256 MB pinned; with the game's saves failing on a fragmented address space below 2 GB, the parts are
// kept above 2 GB (HighAllocator) and within a lower budget)
constexpr size_t kMaxBytes = size_t{96} << 20;    // every room's kept parts together; past it the oldest go
constexpr size_t kMaxRecordBytes = size_t{96} << 20;
// (the room lamp probe) per probed solve: the evaluation records kept for lamps of another room or story and for lamps the
// floor cut stopped (about 300 B each), and the evaluations kept although Apex left them dark (the dump's Z lines)
constexpr size_t kProbeEvalMax = 65536;
constexpr long kProbeZeroMax = 200000;
// (GPU room lamps stage 1) the kept room solves of the active lot together; past it a new solve is not kept (never one kept
// room dropped for another: the rooms would evict one another in a loop, as the pinned room 0s did)
constexpr size_t kMaxKeptBytes = size_t{192} << 20;
// shortcut: an evaluation darker than this share of the threshold before the soft light cut is not kept (a lamp brightened
// more than 4 times misses those texels); lower it if instant edits brighten lamps further
constexpr float kKeepFloor = 0.25f;
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
std::atomic<bool> g_probe{false}; // the developer's room lamp probe (SetRoomProbe)
std::atomic<bool> g_capture{false}; // GPU room lamps stage 1: the active lot's room solves kept (SetRoomLampCapture)
std::atomic<bool> g_keptClear{false}; // the kept room solves go (the capture switched off; done on the light tree thread)
std::atomic<bool> g_physical{false}; // (physical indoor lamps) the kept rooms keep no parts (SetRoomLampPhysical)
// (physical, stage 4) lamp edits: taken, left to the room solves, the lot's room solves begun within kEditSolveMs of one taken
// outside the settle and the off-thread resends (target 0), the settle's and resends' solves; OnPresent logs them every 10 s
constexpr DWORD kEditSolveMs = 3000;
std::atomic<long> c_physTaken{0}, c_physRefused{0}, c_physEditSolves{0}, c_physSettleSolves{0};
std::atomic<DWORD> g_lastPhysEdit{0};
// (health) the physical edits refused in the summary's 10 s, by reason (the room's story and id left out)
std::mutex g_refuseMx;
std::map<std::string, long> g_refuseWhy;
// Lighting scene recordings (scene_record.h): while a Recording capture runs, each physical room solve kept is written with
// the game's part of each of its lamps (KeepEnd); a lamp edit taken on the GPU sends its rooms to a solve once edits rest
// 400 ms, and the recording's start sends every kept physical room of the lot, so each step has the game's answer
std::mutex g_sceneMx;
std::FILE* g_sceneFile = nullptr; // (under g_sceneMx)
std::filesystem::path g_scenePath;
uint32_t g_sceneGen = 0;
long g_sceneRooms = 0;
std::atomic<bool> g_sceneOn{false}, g_sceneAll{false};
DWORD g_sceneEditSeen = 0; // (render thread) the last taken edit whose rooms went to a solve
constexpr long kSceneMaxBytes = 1L << 30; // shortcut: a recording stops growing at 1 GB, raise it if long sessions need more
std::unordered_map<const void*, DWORD> g_physSent; // (light tree thread) room -> when the settle or a resend sent it
// (physical, stage 4; light tree thread) the kept copies RoomFinalized held for the finalized solve's KeepEnd: (key, version)
std::vector<std::pair<uint64_t, uint32_t>> g_heldKept;
// (physical) the instant day/night switch: the night level's ends as the render thread saw them (NoteNightLevel)
std::mutex g_clockMx;
PhaseCache::Clock g_clock;
PhaseCache::Clock ClockNow() {
    std::lock_guard<std::mutex> lk(g_clockMx);
    return g_clock;
}
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
    // (2.15.3) a roofed room's lamp that story 0's room 0 took through an opening: its part of a ground texel is also in the
    // texel's alpha (LevelLightShare::SetIndoorLightOnGround), which a delta does not compose again
    bool exit = false;
    // (physical indoor lamps, Record::phys) its scale in room 0 (NormKOf) and whether the solve gives it the realistic reach share
    // there (LevelLightShare::ReachAppliesIn)
    float normK = 1.0f;
    bool reach = false;
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
// kDup: another sample of the solve writes the same texel; kExitGround (GPU room lamps, a room 0 keeping lamps): a story-0
// ground texel whose alpha is the exit lamps' light (LevelLightShare SolveGroundTexel), not under a floor tile
enum : uint8_t { kMasked = 1, kGated = 2, kDup = 4, kExitGround = 8 };
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
    // (GPU room lamps stage 2a, kept captures) the block's texels are in preblur (ok, or one with rows beyond its edges), and
    // those rows: 2 above, then 2 below, from ghostOff (above / below: which BlurWalls took)
    bool have = false, above = false, below = false;
    uint32_t ghostOff = 0;
};
struct MapGeo {
    bool used = false;
    uintptr_t tex = 0;
    uint32_t pitch = 0, w = 0, h = 0;
};
// (GPU room lamps, kept captures: KeepEnd for a room > 0, BuildKept0 for room 0's lamps Real-time outdoor lamps does not draw)
// One part of a texel: A, B and the threshold scale as halves, the lamp's entries, the bits (kKeptEarly 1, kKeptNoB 2,
// kKeptFade 4; enum below)
struct KeptPart {
    uint32_t texel = 0;
    uint16_t a = 0, b = 0, thrScale = 0; // halves
    uint8_t mult = 1, bits = 0;
};
// A light's kept parts: its law when kept (a law of the GPU's, or, colourOnly, its colour alone: a light whose evaluation is
// its colour x a share that its colour does not change, TubeLight; geo then the signature of what the share reads)
struct KeptLamp {
    uintptr_t light = 0;
    int type = 0;
    float pos[3] = {}; // (stage 2b) where it was (its law's position): moved since, its parts no longer hold
    bool colourOnly = false;
    uint64_t geo = 0;
    GpuLampLaw::Lamp law;
    std::vector<KeptPart, ApexMem::HighAllocator<KeptPart>> parts; // by texel, ascending
    // (room 0's kept lamps) an exit lamp: its parts are also the ground texels' alpha (kExitGround); its floor mask parts: per
    // masked texel the walls-tested and no-walls luminance over its law's luminance there with N.l ignored (RtLampModel::KeptWrapLum)
    bool exit = false;
    struct Mask {
        uint32_t texel;
        float vis, all;
    };
    std::vector<Mask> mask;
    // (a kept room) its parts were carried from the room's earlier kept solve while it was switched off (CarryOff)
    bool carried = false;
    // (physical indoor lamps) its evaluations per texel (the list's count, LampRec::mult) and its scale in this room: a lamp
    // registered in another room shines at its own room's normalisation (InnerLight::NormScale, InnerNormScaleAt); reach: the
    // solve gives it the realistic reach share on this room's wall texels (LevelLightShare::ReachAppliesIn)
    uint8_t mult = 1;
    float normK = 1.0f;
    bool reach = false;
    int home = -1; // (physical) the room it was registered in then: one registered elsewhere since takes a fresh row (phys_lamp_pick.h)
};
struct Record {
    uint32_t version = 0; // (Real-time outdoor lamps) changes whenever the kept parts or texels change
    // (Real-time outdoor lamps) the texels without the drawn lamps' parts: its hash, and a version that changes only with it
    // (a solve or a delta that changed only drawn lamps keeps it: the GPU's relit maps stand as they are)
    uint64_t baseHash = 0;
    uint32_t baseVersion = 0;
    // a lamp edit sent the room to its full solve: kept only as Real-time outdoor lamps' base until that solve ends (no delta)
    bool stale = false;
    // a room 0 of a lot next to the active one (SetNeighbourLots): Real-time outdoor lamps' base only, every light of its list
    // kept with no part (collapsed, not drawn); no delta takes it
    bool baseOnly = false;
    uintptr_t room = 0, mgr = 0;
    int story = 0, cls = 0;
    uint64_t lot = 0;
    uint32_t gen = 0;
    uint64_t physics = 0, geo = 0;
    uint64_t geoParts[3] = {}; // (GPU room lamps) RoomGeoKey's parts at the probe's start
    uint64_t roomGeo = 0;      // (physical) its 3D blockers' key at the solve's start (PhysLampPick::BlockersKey; 0: not read)
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
    // (GPU room lamps) the lamps of its list Real-time outdoor lamps does not draw, kept as a kept room's (BuildKept0): their
    // parts leave the base it relights from and the GPU adds them back (ExportRoom0); a delta that changes one drops it
    std::vector<KeptLamp> kept0;
    // (physical indoor lamps) kept by the physical model: its lamps' normK and reach, and room 0's 3D test records as its solve
    // started (blkHeader < 0: none read)
    bool phys = false;
    std::shared_ptr<const RtLampGpu::BlockerTable> blk;
    float blkHeader = -1.0f;
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
               r.blocks.capacity() * sizeof(BlockRec) + r.preblur.capacity() * 4 + r.drawnSum.capacity() * sizeof(DrawnSum) +
               (r.blk ? r.blk->rows.capacity() * sizeof(RtLampGpu::BlockerTable::Row) : 0);
    for (const LampRec& l : r.lamps) b += sizeof(LampRec) + l.parts.capacity() * sizeof(Contrib);
    for (const KeptLamp& l : r.kept0) b += sizeof(KeptLamp) + l.parts.capacity() * sizeof(KeptPart) + l.mask.capacity() * sizeof(KeptLamp::Mask);
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
        const bool k0 = std::any_of(r.kept0.begin(), r.kept0.end(), [&](const KeptLamp& k) { return k.light == l.light; });
        if (!l.drawn && !k0) {
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
    // (GPU room lamps) room 0's kept lamps are exported with the base (Room0Export::kept0): a solve that keeps another set of
    // them, or their parts elsewhere, over the same base is a new export. Without them a lamp that came into room 0's list over
    // an unchanged base was never uploaded, so every later edit of it was refused (2026-11-02, a drag)
    for (const KeptLamp& k : r.kept0) {
        mix(k.light), mix(k.colourOnly ? 1 : 0), mix(k.exit ? 1 : 0), mix(k.geo);
        const auto* lw = reinterpret_cast<const uint32_t*>(&k.law);
        for (size_t q = 0; q < sizeof k.law / 4; q++) mix(lw[q]);
        for (const KeptPart& p : k.parts)
            mix(static_cast<uint64_t>(p.texel) | static_cast<uint64_t>(p.a) << 32 | static_cast<uint64_t>(p.b) << 48), mix(p.thrScale | p.mult << 16 | p.bits << 24);
        for (const KeptLamp::Mask& m : k.mask) mix(m.texel), mix(std::bit_cast<uint32_t>(m.vis)), mix(std::bit_cast<uint32_t>(m.all));
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
void EraseStale(const void* room, int cls) {
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    const auto it = g_records.find(KeyOf(room, cls));
    if (it != g_records.end() && it->second->stale) EraseLocked(it->first);
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
        const int n = PhysLampPick::ListLen(b, e, max);
        for (int i = 0; i < n; i++) out[i] = b[i];
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
uint64_t LightLotOf(uintptr_t L) { // POD (SEH): the light's lot, 0 none (a street lamp); ~0 unreadable
    __try {
        const BYTE* p = reinterpret_cast<const BYTE*>(L);
        return *reinterpret_cast<const uint32_t*>(p + 0xC0) | static_cast<uint64_t>(*reinterpret_cast<const uint32_t*>(p + 0xC4)) << 32;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return ~0ull;
    }
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
// A room's reflection cube (what metal shows) is taken only 2 s after its solve starts (0x006B1CA0 with 2.0), and a GPU edit runs no
// solve: the fridge kept the lamp's old place until 4.5 s after the drop). Due at the probes' next pass
bool ReflectionDue(const BYTE* room) {
    BYTE *proxy = nullptr, *probe = nullptr, on = 0;
    float now = 0.0f, every = 0.0f;
    if (!CopyBytes(&proxy, room + 0x154, 4) || !proxy || !CopyBytes(&probe, proxy + 8, 4) || !probe || !CopyBytes(&on, proxy + 0x30, 1) || !on) return false;
    if (!CopyBytes(&now, probe + 0x2B44, 4) || !CopyBytes(&every, probe + 0x2C70, 4)) return false;
    const float last = now - every - 1.0f; // (the 1: its own clock may stand still, paused)
    if (!CopyBytes(probe + 0x2C6C, &last, 4)) return false;
    ProbeNeutral::NoteArmed(probe);
    return true;
}
// During a drag a capture runs whole in one frame: one stage a frame took 0.29 s a cube at the drag's frame rate, and the rooms queued
// behind each other (a reflection half a second late)
void ReflectionPace() {
    static BYTE* const all = reinterpret_cast<BYTE*>(GameAddr::Get(GameAddr::Id::ProbeAllStages));
    static bool set = false;
    static BYTE was = 0;
    const bool drag = GpuRoomLamps::PhysMoveRecent(DragLite::kStillMs);
    if (!all || drag == set) return;
    if (drag && !CopyBytes(&was, all, 1)) return;
    const BYTE v = drag ? 1 : was;
    if (CopyBytes(all, &v, 1)) set = drag;
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
// (kept captures) an evaluation's bits: Apex's threshold early-out returned; the soft light cut ran (its threshold x thrScale)
enum : uint8_t { kKeptEarly = 1, kKeptNoB = 2, kKeptFade = 4 }; // kKeptNoB (a kept part): the game never tested it (B = 0)
struct Acc {
    uintptr_t light;
    float rgb[3];
    float vis, all;
    float hook[3]; // (the room lamp probe) the evaluations' colour before the game's tests
    float wall[3]; // (the room lamp probe) that colour after the game's 2D wall test
    float from[3]; // (the room lamp probe) the game's own wall test: where its ray starts, its answer (raw: 0 not run, 1 ok, 2 dropped)
    float rawT;
    uint8_t raw;
    float pre[3];   // (the room lamp probe) the colour before Apex's path for it (path bits: 1 << path)
    uint8_t paths;
    float c0[3];    // (the room lamp probe) the game's evaluations, before any of Apex's steps (SampleState::accEv: the last one's record)
    uint8_t evals;
    float preSum;   // (kept captures) r + g + b of the evaluations' colour before the soft light cut, the last one's threshold scale and bits
    float thrScale;
    uint8_t kbits;
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
    bool exitGround = false; // (GPU room lamps) ExitGround: its alpha is the exit lamps' light
    float visLum = 0.0f, allLum = 0.0f;
    float last[3] = {}; // (the room lamp probe) the last evaluation's colour, for its wall test
    uintptr_t lastLight = 0;
    uintptr_t pathLight = 0; // (the room lamp probe) the evaluation whose path was noted, its path and colour before it
    int path = 0;
    float pre[3] = {};
    ProbeEval cur;         // (the room lamp probe) the evaluation in progress (ProbeEvalBegin)
    ProbeEval accEv[128];  // the last evaluation's record of each acc[]
};
SampleState g_s; // light tree thread only

// (GPU room lamps stage 2a) per kept texel: what the solve's tail took (RoomMapDither's pack thunk, NotePack) and what its place
// holds when the solve ends (BeforeFinalize: the class-2 wall blur and the tile-edge copies done)
enum : uint8_t { kTailDither = 1, kTailFloor = 2 };
constexpr uint32_t kNoSrc = UINT32_MAX;
struct KeptTail {
    float p[4] = {};    // the pack input before the clamp (FUN_006a31d0 [esp+60h])
    float norm = 1.0f;  // +0x160 as the texel was toned
    uint32_t final = 0; // what its place holds when the solve ends
    uint32_t src = 0;   // the texel whose packed value (blurred, for a class-2 wall) ends there; kNoSrc: a copy of a texel no capture holds
    uint8_t bits = 0, seen = 0;
};
// A tile-edge copy of a kept texel (src) to a place no kept texel holds, and what that place holds when the solve ends
struct KeptCopy {
    uint8_t map = 0;
    uint16_t x = 0, y = 0;
    uint32_t src = 0, final = 0;
};
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
    bool probe = false; // a room other than room 0, for the room lamp probe: compared at its end and dropped
    bool keep = false;  // (GPU room lamps stage 1) that capture is kept at its end (KeepEnd)
    bool phys = false;  // (physical indoor lamps) kept with no parts, with the room's 3D test records read at its start (tests)
    RoomLightTests::Room tests;
    std::vector<std::pair<uintptr_t, std::array<float, 3>>> startPos; // (phys) each listed lamp's place as `tests` were read
    bool keep0 = false; // (GPU room lamps) a room 0's: the lights of keep0Lights are captured as a kept room's (BuildKept0)
    bool probe0 = false; // (the room lamp probe) a room 0 of the active lot: the lights of keep0Lights are recorded and dumped (ProbeEnd)
    std::unordered_set<uintptr_t> keep0Lights;
    struct ProbeHook {
        uint32_t texel;
        uintptr_t light;
        float rgb[3];
        float wall[3];
        float from[3], rawT;
        uint8_t raw;
        float pre[3];
        uint8_t paths;
        float c0[3];
        uint32_t ev;    // into probeEval (UINT32_MAX: none)
        uint8_t evals;
        float preSum, thrScale; // (kept captures) Acc's
        uint8_t kbits;
    };
    std::vector<ProbeHook> probeHook; // (the probe) each light's colour at each texel before the game's tests
    std::vector<ProbeEval> probeEval; // (the probe) the last evaluation's steps, for lamps of another room or story (kProbeEvalMax)
    long probeZero = 0, probeEvalDropped = 0;
    std::vector<std::string> probeTotals; // (the probe) the fills of the opening's outdoor total (ProbeTotalLine), TF lines
    float normStart = -1.0f; // the room's normalisation (+0x160) when its solve started
    // a self-check solve: the texels the delta wrote, by (map, x, y)
    bool check = false, checkLightsSame = false;
    std::unordered_map<uint64_t, uint32_t> checkOld;
    long cmpN = 0, cmpOver1 = 0, cmpAt1 = 0;
    int cmpMax = 0;
    // (GPU room lamps stage 2a, kept captures) the texels' tails, the tile-edge copies to other places, why the tail is not kept
    std::vector<KeptTail, ApexMem::HighAllocator<KeptTail>> tail;
    std::vector<KeptCopy> copies;
    const char* tailWhy = nullptr;
    bool finals = false, roofed = false;
    long killed = 0;    // kept texels a copy of a texel no capture holds overwrote
    float toneK = 0.0f; // the roofed tail's k ([0x011D0BA0])
    float amb[3] = {}, ambPass[3] = {}; // +0x110 and the pass's +0x650 at the solve's end (the log; the GPU's compose)
    float ambPair[2][4] = {};           // (the day/night cache) +0x110 and +0x120 whole at the solve's end
    PhaseCache::Stamp phaseStart;       // (the day/night cache) the night level's end as the solve started
    // (GPU room lamps stage 4, kept captures) the directional maps (FUN_006a09f0): per sample its texel and position; per light
    // of the list at each sample its part (the accumulator after it less before), the game's step called again there (summed
    // over its listings) and how often it was listed; at the solve's end the set [room+8] and the working set's texels
    struct BasisSample {
        uint16_t x = 0, y = 0;
        float pos[4] = {};
    };
    struct BasisPart {
        uint32_t sample = 0;
        uintptr_t light = 0;
        float c[16] = {}, step[16] = {};
        uint8_t mult = 0;
    };
    std::vector<BasisSample> basis;
    std::vector<BasisPart> basisParts;
    std::vector<uint32_t> basisGame; // 4 per sample (map m at 4i + m)
    uintptr_t basisSet = 0;
    uint32_t basisW = 0, basisH = 0, basisHint = 0;
    const char* basisWhy = nullptr;
};
Capture g_cap; // light tree thread only
// (GPU room lamps stage 2a) the kept capture's texel whose pack comes next (SampleEnd -> NotePack, light tree thread)
thread_local const void* t_packRoom = nullptr;
thread_local uint32_t t_packTexel = 0;

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
    t_packRoom = nullptr;
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
constexpr const char* kWhyEditBatches = "too many batches to solve again in one step during an edit (the full solve is sliced)";
constexpr const char* kWhyBaseOnly = "a lot next to the active one keeps only the GPU's base";
std::atomic<long> c_batchRefused{0};
bool LampEditRecent() {
    const unsigned long edit = LampMarkFilter::EditLastTick();
    return LevelLightShare::LampDragging() || (edit && GetTickCount() - edit < kEditWindowMs);
}
bool LawOfLight(uintptr_t light, uint64_t lot, KeptLamp& kl, int* regRoom); // below
Delta ComputeDelta(BYTE* room, const RoomRead& rr, Record& r, const char*& why, Plan& plan) {
    if (r.baseOnly) { // (no light's part to take out or to skip as drawn)
        why = kWhyBaseOnly;
        return Delta::Fail;
    }
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
    // (physical indoor lamps) a room 0 drawn physically takes every lamp with a GPU law out of its base and draws it from its law,
    // exit ground included (ExportRoom0): such lamps changing leave nothing to solve. The full solve below wrote the game's room-0
    // maps over the GPU's at every indoor lamp edit (story 0 room 0 "solved in full (a lamp shining out of
    // a room onto the ground changed)" at each drag; the stairs and objects reading those maps flickered)
    if (r.phys && RtOutdoorLamps::PhysCoversRoom0(room)) {
        bool allLaw = true;
        for (const Changed& c : changed) {
            KeptLamp kl;
            allLaw = allLaw && (c.rec < 0 || !r.lamps[static_cast<size_t>(c.rec)].drawn) && LawOfLight(c.light, r.lot, kl, nullptr) && !kl.colourOnly;
        }
        if (allLaw) {
            for (const Changed& c : changed) plan.lights.push_back(c.light);
            c_gpuSkips.fetch_add(1, std::memory_order_relaxed);
            return Delta::Ok;
        }
    }
    // (2.15.3) indoor light on the ground: a changed exit lamp also changes the ground texels' alpha (E), which the kept parts
    // do not hold; the full solve writes both
    if (LevelLightShare::IndoorLightOnGroundActive())
        for (const Changed& c : changed)
            if ((c.rec >= 0 && r.lamps[static_cast<size_t>(c.rec)].exit) || LevelLightShare::ExitLampOf(room, c.light)) {
                why = "a lamp shining out of a room onto the ground changed";
                return Delta::Fail;
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
            why = kWhyEditBatches;
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
            l->exit = LevelLightShare::ExitLampOf(room, c.light);
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
        std::erase_if(r.kept0, [&](const KeptLamp& k) { return k.light == l->light; }); // (its parts changed: back in the base)
    }
    std::erase_if(r.lamps, [](const LampRec& l) { return l.mult <= 0; });
    std::erase_if(r.kept0, [&](const KeptLamp& k) { return std::none_of(r.lamps.begin(), r.lamps.end(), [&](const LampRec& l) { return l.light == k.light; }); });
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
        if (it == g_records.end() || it->second->stale) return "a story's room 0 has no kept parts";
        if (it->second->baseOnly) return kWhyBaseOnly;
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
        else if ((it = g_records.find(KeyOf(room, cls))) == g_records.end() || it->second->stale) why = "no kept parts";
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
            // (a lamp-only refusal keeps the base the GPU's until its solve ends, as at the lock step)
            if (it != g_records.end()) {
                if (it->second->stale || (why && (std::strcmp(why, kWhyEditBatches) == 0 || std::strcmp(why, "too many lights changed") == 0))) it->second->stale = true;
                else EraseLocked(it->first);
            }
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
    const int sent = LevelLightShare::SendRoom0DependentsTail(p.tracker, p.stories, &lights);
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
void SetRoomProbe(bool on) {
    if (g_probe.exchange(on) == on) return;
    LOG_INFO(on ? "[RoomLampProbe] On: each solve of the active lot's rooms other than room 0 is compared with the GPU's lamp law"
                : "[RoomLampProbe] Off");
}
bool RoomProbe() { return g_probe.load(std::memory_order_relaxed); }
void SetRoomLampCapture(bool on) {
    if (g_capture.exchange(on) == on) return;
    if (!on) g_keptClear.store(true);
    LOG_INFO(on ? "[GpuRoomLamps] Capture on: each solve of the active lot's rooms other than room 0 is kept for the GPU" : "[GpuRoomLamps] Capture off: the kept rooms go");
}
bool RoomLampCapture() { return g_capture.load(std::memory_order_relaxed); }
bool KeptClearDue() { return g_keptClear.load(std::memory_order_relaxed); }
void SetRoomLampPhysical(bool on) {
    if (g_physical.exchange(on) == on) return;
    g_keptClear.store(true); // (the other model's kept rooms go; each room is kept again at its next solve)
    LOG_INFO(on ? "[GpuRoomLamps] Model physical: kept rooms keep no parts and are drawn from their lamps' laws (rooms > 0; room 0 and lamp edits still the game's)"
                : "[GpuRoomLamps] Model replica: kept rooms keep each lamp's parts");
}
bool RoomLampPhysical() { return g_physical.load(std::memory_order_relaxed); }
bool RoomTestsFound() {
    static int s_found = -1; // (render thread)
    if (s_found < 0 && GameAddr::Resolved()) {
        using GameAddr::Id;
        std::string missing;
        s_found = GameAddr::Have({Id::BlockerQuadCtor, Id::BlockerQuadVt, Id::BlockerDiscVt, Id::TestZero, Id::TestTwo, Id::TestNeg1, Id::TestFour, Id::TestDiscEdge,
                                  Id::TestStop, Id::TestQuadEdge, Id::TestDrop, Id::TestOne, Id::TestEps, Id::TestHalf, Id::TestPenumbra, Id::TestWallY},
                                 &missing);
        if (s_found) LOG_INFO(std::format("[GpuRoomLamps] The game's room light tests found on {}: furniture blockers {:#010x} / {:#010x}, 13 constants", GetGameVersionName(),
                                          GameAddr::Get(Id::BlockerQuadVt), GameAddr::Get(Id::BlockerDiscVt)));
        else LOG_WARNING("[GpuRoomLamps] Rooms keep the game's solves: " + GameAddr::NotAvailable(missing));
    }
    return s_found > 0;
}
std::atomic<bool> g_hiFloors{false};
void SetHiFloors(bool on) {
    if (g_hiFloors.exchange(on) == on) return;
    g_keptClear.store(true); // (each room is kept again at its next solve, with or without its floor's sub-texels)
    LOG_INFO(on ? "[GpuRoomLamps] Sharper floors: the physical rooms' floor maps drawn at twice their size" : "[GpuRoomLamps] Sharper floors off");
}
bool HiFloors() { return g_hiFloors.load(std::memory_order_relaxed); }
std::atomic<bool> g_trueEdges{false};
void SetTrueFurnitureEdges(bool on) { g_trueEdges.store(on, std::memory_order_relaxed); } // (the relight rules take it: the maps relit)
bool TrueFurnitureEdges() { return g_trueEdges.load(std::memory_order_relaxed); }
bool RoomHooks() { return g_probe.load(std::memory_order_relaxed) || g_capture.load(std::memory_order_relaxed); }
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
void SettleDue(); // below (GPU room lamps: the rooms of the lamp edits taken, 2 s after a lamp's last one)
void BasisDue();  // below (GPU room lamps stage 4: the directional maps of the lamp edits taken)
void RefreshLotLamps(); // below (physical indoor lamps: the active lot's registered lights for the render thread)
void QueueBasisGone(uintptr_t light); // below (physical, stage 5: a lamp that left the lot's registry)
void NoteUnkept(const void* room);     // below (SolveUnkept)
void PhaseSwitchDue(); // below (the instant day/night switch: the cached solves put back after a quick switch)
void AfterRoomUpdate(const void* tl) {
    SettleDue();
    RefreshLotLamps(); // (first: the physical directional maps pick the lamps from its snapshot, a new or moved lamp included)
    HookGuard::Run("OutdoorDelta day/night switch", [] { PhaseSwitchDue(); }); // (before BasisDue: its directional carry is written there)
    BasisDue();
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
    // (a base-only record does not hold the lights that left the list during its solve)
    if (it == g_records.end() || it->second->stale || it->second->baseOnly) return false;
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
    static uintptr_t s_tracker = 0, s_held = 0;
    static uint64_t s_lot = 0;
    static DWORD s_at = 0;
    const DWORD now = GetTickCount();
    // (active_lot_hold) the lot Real-time outdoor lamps holds while the camera moves, only while it runs (its tracker is
    // current then); a change of it is read at once, not after the 500 ms: the lot it switches to is sent to its solve at once
    const uintptr_t held = DevSwitches::ActiveLotHold() && RtOutdoorLamps::Watching() ? RtOutdoorLamps::ActiveTracker() : 0;
    if (s_at && now - s_at < 500 && held == s_held) return s_lot;
    s_at = now | 1;
    s_held = held;
    uintptr_t trackers[8];
    const int n = LevelLightShare::ActiveLotTrackers(trackers, 8);
    uintptr_t t = n > 0 ? trackers[0] : 0;
    for (int i = 0; i < n; i++)
        if (trackers[i] == s_tracker) t = s_tracker;
    // (active_lot_hold) while Real-time outdoor lamps holds its active lot as the camera moves, the same lot: two pickers apart
    // kept the waiting lot's room-0 solve in the active lot's form, which its neighbour copies cannot use
    if (n > 0 && held && LevelLightShare::TrackerLive(held)) t = held;
    s_tracker = t;
    s_lot = 0;
    RoomRead r0;
    if (const void* room0 = t ? LevelLightShare::Room0Of(t, 0) : nullptr; room0 && ReadRoom(static_cast<const unsigned char*>(room0), r0)) s_lot = r0.lot;
    return s_lot;
}
namespace {
std::mutex g_neighbourMx; // the lots next to the active one (the render thread sets them, SolveStart reads them)
uint64_t g_neighbours[4] = {};
int g_neighbourN = 0;
bool NeighbourLot(uint64_t lot) {
    std::lock_guard<std::mutex> lk(g_neighbourMx);
    return lot && std::find(g_neighbours, g_neighbours + g_neighbourN, lot) != g_neighbours + g_neighbourN;
}
} // namespace
void SetNeighbourLots(const uint64_t* lots, int n) {
    std::lock_guard<std::mutex> lk(g_neighbourMx);
    g_neighbourN = lots ? std::clamp(n, 0, 4) : 0;
    std::copy_n(lots, g_neighbourN, g_neighbours);
}
// ---- the developer's room lamp probe (SetRoomProbe; light tree thread) ----
// A room other than room 0 is captured with the same hooks (each light's part of its sum, before the roofed rooms' tone), then
// each lot lamp's part is compared, texel by texel, with the law the GPU draws it with and no shadow: what the GPU would have
// to reproduce. Nothing of it is kept.
bool ReadRoomTests(const BYTE* room, RoomLightTests::Room& r, std::string& why, bool anyBlocker = false);
namespace {
std::atomic<int> g_probeLines{0};
constexpr int kProbeLines = 2000; // (a session's log: every room of a few lots, with their lamps)
bool ProbeLog() {
    const int n = g_probeLines.fetch_add(1, std::memory_order_relaxed);
    if (n == kProbeLines) LOG_INFO("[RoomLampProbe] Line limit reached: later probes are not logged");
    return n < kProbeLines;
}
// (GPU room lamps: CarryOff, the move path) the lot's geometry key mixed with what the game's tests read of the room
// (room_light_tests.h): the walls' fields the 2D test reads, its default wall list and wall mode, each 3D record's centre and
// squared radii, and each blocker's vtable and the fields its transmission reads. Never the records' raw bytes: their blocker
// vectors are made again by every gather (a carry refused at every solve, 2026-11-02). parts (optional): the lot's key, the
// walls', the 3D blockers'. 0: not read
uint64_t RoomGeoKey(const BYTE* room, uint64_t parts[3] = nullptr, RoomLightTests::Room* tests = nullptr) {
    uint64_t p[3] = {LevelLightShare::LotGeometryKey(room), 1469598103934665603ull, 1469598103934665603ull};
    if (parts) std::fill_n(parts, 3, 0ull);
    RoomLightTests::Room t;
    std::string why;
    if (!p[0] || !ReadRoomTests(room, t, why, true)) return 0;
    const auto mix = [&](uint64_t& h, const void* src, size_t n) {
        for (size_t i = 0; i < n; i++) h = (h ^ static_cast<const uint8_t*>(src)[i]) * 1099511628211ull;
    };
    const auto mixAt = [&](uint64_t& h, const float* f, int off, int n) { mix(h, f + off / 4, static_cast<size_t>(n) * 4); };
    const uint32_t nw = static_cast<uint32_t>(t.walls.size()), ni = static_cast<uint32_t>(t.idx2d.size()), nr = static_cast<uint32_t>(t.rec.size());
    mix(p[1], &t.mode, sizeof t.mode), mix(p[1], &nw, 4), mix(p[1], &ni, 4);
    mix(p[1], t.idx2d.data(), t.idx2d.size() * 4);
    for (const RoomLightTests::Wall& w : t.walls) {
        for (const int off : {0x40, 0x60, 0x70, 0x90, 0xA0, 0xC0, 0xD0}) mixAt(p[1], w.f, off, 3); // the three segments, the normal
        mixAt(p[1], w.f, 0xE0, 3);                                                                  // its length, the junction counts
        const uint32_t bits = w.u(0xEC) & 3u;
        mix(p[1], &bits, 4);
    }
    mix(p[2], &nr, 4);
    for (const RoomLightTests::Record& rc : t.rec) {
        const uint64_t rk = RoomLightTests::RecordKey(rc); // (a blocker of a kind the copy does not know: all of it; the move path refuses it)
        mix(p[2], &rk, 8);
    }
    if (parts) std::copy_n(p, 3, parts);
    if (tests) *tests = std::move(t);
    uint64_t geo = p[0];
    mix(geo, &p[1], 8), mix(geo, &p[2], 8);
    return geo | 1;
}
// Why a room's key differs from the one kept with its solve (parts as RoomGeoKey gives them)
const char* GeoChanged(const uint64_t was[3], const uint64_t now[3]) {
    return !now[0] || !was[0] ? "the room's walls or 3D blockers could not be read"
           : was[0] != now[0] ? "the lot's geometry changed"
           : was[1] != now[1] ? "the room's walls changed"
                              : "the room's 3D blockers changed";
}
void ProbeStart(unsigned char* room, const RoomRead& rr) {
    const char* refuse = !LevelLightShare::DeltaReady() ? "the room solve hooks are not in"
                         : RoomLightQueue::QuickPassTarget(room) > 0  ? "a quick pass"
                         : !LevelLightShare::ListAllWrapped(room)     ? "a light whose evaluation is not wrapped"
                                                                      : nullptr;
    if (refuse) {
        if (ProbeLog()) LOG_INFO(std::format("[RoomLampProbe] Story {} room {} class {}: not probed ({}{})", rr.story, rr.id, rr.cls, refuse,
                                 std::strcmp(refuse, "a light whose evaluation is not wrapped") == 0 ? ": " + LevelLightShare::FirstUnwrappedLight(room) : std::string()));
        return;
    }
    Capture& c = g_cap;
    c.active = c.ok = c.probe = true;
    c.keep = g_capture.load(std::memory_order_relaxed);
    c.phys = c.keep && g_physical.load(std::memory_order_relaxed);
    c.room = room;
    c.rec = std::make_unique<Record>();
    Record& r = *c.rec;
    r.room = reinterpret_cast<uintptr_t>(room);
    r.mgr = rr.mgr, r.story = rr.story, r.lot = rr.lot, r.cls = rr.cls;
    r.thr = rr.thr, r.wallMode = rr.wallMode;
    CopyBytes(&c.normStart, room + 0x160, sizeof c.normStart);
    // (GPU room lamps, CarryOff) the lot's geometry key and what the game's tests read of the room as the solve starts: the
    // parts of a lamp kept by an earlier solve still hold while it is the same (UNVERIFIED that a moved object always changes
    // its record's blockers)
    if (c.keep) r.geo = RoomGeoKey(room, r.geoParts, c.phys ? &c.tests : nullptr);
    c.phaseStart = PhaseCache::StampOf(ClockNow(), GetTickCount());
    // (physical) its lamps' places read with those records: the kept place anchors the carries of a table dragged with its lamp,
    // and read at the end it was 0.2-0.5 m ahead of the records (one 78 ms solve): the table's shadow trailed it
    c.startPos.clear();
    if (c.phys) {
        uintptr_t list[64];
        const int n = ReadList(room, list, static_cast<int>(std::size(list)));
        for (int i = 0; i < n; i++) {
            GpuLampLaw::Lamp law;
            int type = 0, reg = -1;
            if (RtOutdoorLamps::WorldLaw(list[i], rr.lot, false, law, type, reg)) c.startPos.push_back({list[i], {law.pos[0], law.pos[1], law.pos[2]}});
        }
    }
}
inline float Max3(const float* c) { return std::max({c[0], c[1], c[2]}); }
// What the game's 3D and 2D tests read of a room (the dump's RECORDS, R, B, WALLS, W, IDX2D and IDX3D lines; tag: their prefix)
std::string ProbeRoomTests(const BYTE* room, const char* tag) {
    std::string out;
    uintptr_t rb[2] = {};
    float tail[4] = {};
    if (CopyBytes(rb, room + 0x40, sizeof rb) && rb[0] && rb[1] >= rb[0] && (rb[1] - rb[0]) % 64 == 0) {
        const size_t nr = std::min<size_t>((rb[1] - rb[0]) / 64, 512);
        CopyBytes(tail, room + 0x9C, sizeof tail); // (+0xA0: the scale the 3D test's answer takes)
        out += std::format("{}RECORDS {} scaleA0 {:.6f}\n", tag, nr, tail[1]);
        for (size_t i = 0; i < nr; i++) {
            float rec[16] = {};
            if (!CopyBytes(rec, reinterpret_cast<const void*>(rb[0] + i * 64), 64)) break;
            uintptr_t bl[2] = {};
            std::memcpy(bl, &rec[12], sizeof bl);
            out += std::format("{}R {} c {:.4f} {:.4f} {:.4f} {:.4f} r {:.4f} {:.4f} {:.4f} {:.4f} w8 {:.4f} {:.4f} {:.4f} {:.4f}", tag, i, rec[0], rec[1], rec[2], rec[3], rec[4], rec[5],
                               rec[6], rec[7], rec[8], rec[9], rec[10], rec[11]);
            const size_t nb = bl[0] && bl[1] >= bl[0] ? std::min<size_t>((bl[1] - bl[0]) / 4, 64) : 0;
            out += std::format(" blockers {}\n", nb);
            for (size_t j = 0; j < nb; j++) {
                uintptr_t b = 0, vt = 0, f14 = 0;
                if (!CopyBytes(&b, reinterpret_cast<const void*>(bl[0] + j * 4), 4) || !b || !CopyBytes(&vt, reinterpret_cast<const void*>(b), 4) || !vt ||
                    !CopyBytes(&f14, reinterpret_cast<const void*>(vt + 0x14), 4))
                    continue;
                uint32_t raw[64] = {}; // (0x0069BEA0 reads the blocker up to +0xFF)
                CopyBytes(raw, reinterpret_cast<const void*>(b), sizeof raw);
                out += std::format("{}B {:08X} vt {:08X} f14 {:08X} raw", tag, b, vt, f14);
                for (uint32_t w : raw) out += std::format(" {:08X}", w);
                out += '\n';
            }
        }
    } else out += std::format("{}RECORDS none\n", tag);
    // the 2D wall test 0x0069FC40 -> 0x0069D4C0: the mode byte +0x639, the walls (room+0x30..+0x34, pointers; 0x100 bytes each),
    // the default index vectors (2D room+0x78..+0x7C, 3D room+0x88..+0x8C)
    uint8_t mode = 0;
    CopyBytes(&mode, room + 0x639, 1);
    uintptr_t wv[2] = {}, i2[2] = {}, i3[2] = {};
    CopyBytes(wv, room + 0x30, sizeof wv), CopyBytes(i2, room + 0x78, sizeof i2), CopyBytes(i3, room + 0x88, sizeof i3);
    const size_t nw = wv[0] && wv[1] >= wv[0] ? std::min<size_t>((wv[1] - wv[0]) / 4, 1024) : 0;
    out += std::format("{}WALLS {} mode {}\n", tag, nw, static_cast<int>(mode));
    for (size_t i = 0; i < nw; i++) {
        uintptr_t w = 0;
        if (!CopyBytes(&w, reinterpret_cast<const void*>(wv[0] + i * 4), 4) || !w) continue;
        uint32_t raw[64] = {};
        CopyBytes(raw, reinterpret_cast<const void*>(w), sizeof raw);
        out += std::format("{}W {} {:08X} raw", tag, i, w);
        for (uint32_t x : raw) out += std::format(" {:08X}", x);
        out += '\n';
    }
    for (int v = 0; v < 2; v++) {
        const uintptr_t* iv = v ? i3 : i2;
        const size_t ni = iv[0] && iv[1] >= iv[0] ? std::min<size_t>((iv[1] - iv[0]) / 4, 4096) : 0;
        out += std::format("{}{} {}", tag, v ? "IDX3D" : "IDX2D", ni);
        for (size_t i = 0; i < ni; i++) {
            int32_t x = 0;
            CopyBytes(&x, reinterpret_cast<const void*>(iv[0] + i * 4), 4);
            out += std::format(" {}", x);
        }
        out += '\n';
    }
    return out;
}
// The probe's dump (one file a session in the Apex Radiance folder): each probed room's 3D blockers as the game's 3D test
// 0x0069D910 reads them (room+0x40..+0x44: 64-byte records, the blocker pointers at record+0x30..+0x34; each blocker's
// vtable, its vfunc+0x14 = the transmission, and its first 0x100 bytes; once, the globals the 3D test reads) and the sample texels ProbeEnd picked
void ProbeDump(const BYTE* room, const RoomRead& rr, const std::string& texels) {
    static std::FILE* s_file = nullptr;
    static bool s_tried = false;
    static size_t s_bytes = 0;
    constexpr size_t kMaxBytes = 64u << 20; // (one file, at most 64 MB a session)
    if (!s_tried) {
        s_tried = true;
        SYSTEMTIME t{};
        GetLocalTime(&t);
        if (ApexPaths::EnsureApexDirectory())
            s_file = _wfopen((ApexPaths::ApexDirectory() + std::format(L"RoomLampProbe-{:04}{:02}{:02}-{:02}{:02}{:02}.txt", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
                                                                       t.wSecond)).c_str(),
                             L"wb");
        LOG_INFO(s_file ? "[RoomLampProbe] Dump: RoomLampProbe-<time>.txt in the Apex Radiance folder" : "[RoomLampProbe] Dump: the file could not be made");
        if (s_file) { // the 3D test's constants (0x0069D910, 0x0069FCE0, the blockers' vfunc+0x14 and their helpers 0x0069B4C0..0x0069B72A)
            static constexpr uint32_t kConsts[] = {0x00F98964, 0x00F98A58, 0x00F9A5AC, 0x00FAA210, 0x00FB2F6C, 0x00FBD498, 0x00FECE18, 0x00FF31AC, 0x0106B4B4, 0x0106D6CC,
                                                   0x0107A538, 0x011CB864, 0x011D01B0, 0x011D01C0, 0x011D01D0, 0x011D01E0, 0x011D01F0, 0x011D0200, 0x011D0240,
                                                   0x00FF30A0, 0x00FF307C, 0x0106E33C, 0x00F9896C, 0x01158A50, 0x011D0190}; // (and the 2D wall test's)
            std::string c;
            for (uint32_t at : kConsts) {
                uint32_t v[4] = {};
                CopyBytes(v, reinterpret_cast<const void*>(static_cast<uintptr_t>(at)), sizeof v);
                c += std::format("CONST {:08X} {:08X} {:08X} {:08X} {:08X}\n", at, v[0], v[1], v[2], v[3]);
            }
            s_bytes += std::fwrite(c.data(), 1, c.size(), s_file);
        }
    }
    if (!s_file || s_bytes > kMaxBytes) return;
    static std::unordered_set<uint64_t> s_room0; // (room 0: once a session per room and class, its walls and records are the lot's largest)
    if (rr.id == 0 && !s_room0.insert(static_cast<uint64_t>(reinterpret_cast<uintptr_t>(room)) << 2 | static_cast<uint64_t>(rr.cls & 3)).second) return;
    std::string out = std::format("ROOM story {} id {} class {} roofed {} room {:08X}\n", rr.story, rr.id, rr.cls, rr.outdoor ? 0 : 1, reinterpret_cast<uintptr_t>(room));
    // Apex's state for the room's solve, its record's lamp sets and the grids and floors their tests read
    out += LevelLightShare::ProbeRoomText(room, RtOutdoorLamps::TrackerOf(room), g_cap.normStart);
    out += std::format("PROBE evals {} dropped {} zero {}\n", g_cap.probeEval.size(), g_cap.probeEvalDropped, g_cap.probeZero);
    out += ProbeRoomTests(room, "");
    // (IndoorShadow) the other rooms a lamp's ray segments were tested in by the game's 2D test: their walls and records
    std::vector<std::pair<int, int>> others;
    for (const ProbeEval& e : g_cap.probeEval)
        for (int k = 0; k < e.nseg && k < 8; k++)
            if (e.seg[k].passed != 255 && e.seg[k].story >= 0 && e.seg[k].room >= 0) others.emplace_back(e.seg[k].story, e.seg[k].room);
    std::sort(others.begin(), others.end());
    others.erase(std::unique(others.begin(), others.end()), others.end());
    const uintptr_t tracker = RtOutdoorLamps::TrackerOf(room);
    for (const auto& [story, id] : others) {
        const BYTE* o = static_cast<const BYTE*>(LevelLightShare::RoomOf(tracker, story, id));
        if (o == room) continue;
        out += std::format("OTHER story {} id {} room {:08X}\n", story, id, reinterpret_cast<uintptr_t>(o));
        if (o) out += ProbeRoomTests(o, "O");
    }
    for (const std::string& l : g_cap.probeTotals) out += l; // (the opening's outdoor totals)
    out += texels;
    s_bytes += std::fwrite(out.data(), 1, out.size(), s_file);
    std::fflush(s_file);
}
// The occluders the GPU path would shadow a room's lamps with (LevelLightShare::ExportLotProxy, ceilings left open over the
// rooms as the GPU room lamps build had them), read again when the lot or its geometry changes
struct ProbeMesh {
    uintptr_t tracker = 0;
    uint64_t geo = 0;
    RtLampProxy::Lot lot;
    RtLampProxy::Mesh mesh;
    bool ok = false;
    std::string why;
};
ProbeMesh g_probeMesh; // light tree thread
const ProbeMesh& ProbeOccluders(const void* room) {
    const uintptr_t tracker = RtOutdoorLamps::TrackerOf(room);
    const uint64_t geo = LevelLightShare::LotGeometryKey(room);
    ProbeMesh& p = g_probeMesh;
    if (tracker && geo && p.tracker == tracker && p.geo == geo) return p;
    p = ProbeMesh{};
    p.tracker = tracker, p.geo = geo;
    if (!tracker) p.why = "the lot's story tracker is not known yet";
    else if (LevelLightShare::ExportLotProxy(tracker, p.lot, p.why)) {
        RtLampProxy::Build(p.lot, p.mesh, true);
        p.ok = true;
    }
    return p;
}
// (the dump) the evaluation's steps on a T or Z line: the game's colour summed over the lamp's entries, then the last
// evaluation's record when kept (docs/features/night-lighting/gpu-room-lamps-probe.md)
std::string EvTokens(const Capture::ProbeHook& ph, const std::vector<ProbeEval>& evs) {
    std::string t = std::format(" c0 {:.9g} {:.9g} {:.9g} evals {}", ph.c0[0], ph.c0[1], ph.c0[2], static_cast<int>(ph.evals));
    if (ph.ev >= evs.size()) return t;
    const ProbeEval& e = evs[ph.ev];
    t += std::format(" ev set {} e0 {:.9g} {:.9g} {:.9g} mount {:.9g} day {:.9g} reach {:.9g} cut {} early {}", static_cast<int>(e.set), e.c0[0], e.c0[1], e.c0[2], e.mount,
                     e.day, e.reach, static_cast<int>(e.cut), static_cast<int>(e.early));
    if (e.has & 1) t += std::format(" p6 {:.9g} {:.9g} {:.9g} {:.9g}", e.p6[0], e.p6[1], e.p6[2], e.p6[3]);
    if (e.has & 2) {
        t += " p7";
        for (float v : e.p7) t += std::format(" {:.9g}", v);
        if (e.tf >= 0) t += std::format(" tf {}", e.tf); // (the TF line of the outdoor total's fill)
    }
    if (e.has & 4) {
        t += std::format(" p8 {:.9g} {:.9g} {:.9g} why {} stage {} nseg {}", e.p8[0], e.p8[1], e.p8[2], static_cast<int>(e.why), static_cast<int>(e.stage), static_cast<int>(e.nseg));
        for (int k = 0; k < e.nseg && k < 8; k++) {
            const ProbeEval::Seg& g = e.seg[k];
            t += std::format(" seg {} {} {:.9g} {:.9g} {:.9g} {}", static_cast<int>(g.story), static_cast<int>(g.room), g.t0, g.t1, g.keep, static_cast<int>(g.passed));
        }
        if (e.entry) t += std::format(" ent {:.9g} {:.9g} {:.9g}", e.ent[0], e.ent[1], e.ent[2]);
    }
    if (e.has & 8) t += std::format(" fade {:.9g} {:.9g}", e.fade, e.fadeThr);
    return t;
}
void ProbeEnd(const BYTE* room) {
    const Capture& c = g_cap;
    const Record& r = *c.rec;
    RoomRead rr;
    if (!ReadRoom(room, rr)) return;
    const char* kind = rr.outdoor ? "roofless" : "roofed";
    const bool r0 = rr.id == 0; // (room 0: only the lights its capture recorded, keep0Lights, are compared and dumped)
    std::vector<ListLight> list;
    if (c.ok && !UniqueList(room, list)) Fail("the light list could not be read");
    if (c.ok && r.samples.size() != r.texels.size() * 0x30) Fail("the samples do not match the texels");
    if (!c.ok) {
        if (ProbeLog()) LOG_INFO(std::format("[RoomLampProbe] Story {} room {} class {} ({}): not compared ({})", rr.story, rr.id, rr.cls, kind, c.why ? c.why : "?"));
        return;
    }
    LARGE_INTEGER t0{};
    QueryPerformanceCounter(&t0);
    const bool reachOn = LevelLightShare::RealisticReachActive(), fade = LevelLightShare::SoftLightCutOn();
    const size_t n = r.texels.size();
    constexpr float kLevel = 1.0f / 255.0f;
    std::unordered_map<uintptr_t, const LampRec*> parts;
    for (const LampRec& l : r.lamps) parts[l.light] = &l;
    std::unordered_map<uintptr_t, std::vector<const Capture::ProbeHook*>> hooks;
    for (const Capture::ProbeHook& h : c.probeHook)
        if (h.texel < n) hooks[h.light].push_back(&h);
    // (the dump) each texel's batch: its index, size, the pass's switches (flags[0] walls, flags[1] 3D) and centroid
    std::vector<uint32_t> batchOf(n, 0xFFFFFFFFu);
    std::vector<float> centroid(r.batches.size() * 3, 0.0f);
    for (size_t b = 0; b < r.batches.size(); b++) {
        const BatchRec& br = r.batches[b];
        for (uint32_t k = 0; k < br.count && br.first + k < n; k++) {
            batchOf[br.first + k] = static_cast<uint32_t>(b);
            const float* p = reinterpret_cast<const float*>(r.samples.data() + static_cast<size_t>(br.first + k) * 0x30);
            for (int q = 0; q < 3; q++) centroid[b * 3 + q] += p[q] / static_cast<float>(br.count);
        }
    }
    // light no part holds (the GPU's base would keep it whatever it draws)
    std::vector<float> got(n * 3, 0.0f), measAll(n * 3, 0.0f), lawAll(n * 3, 0.0f);
    for (const LampRec& l : r.lamps)
        for (const Contrib& p : l.parts)
            if (p.texel < n)
                for (int k = 0; k < 3; k++) got[p.texel * 3 + k] += p.rgb[k];
    long unheld = 0;
    for (size_t i = 0; i < n; i++)
        if (std::max({r.texels[i].t.S[0] - got[i * 3], r.texels[i].t.S[1] - got[i * 3 + 1], r.texels[i].t.S[2] - got[i * 3 + 2]}) > 2.0f * kLevel) unheld++;
    std::vector<std::string> rows;
    int drawn = 0;
    const ProbeMesh& occ = ProbeOccluders(room);
    // shortcut: only every stride-th texel takes the occluder test (about 200,000 rays a solve at most); test all if the shares look noisy
    const size_t stride = std::max<size_t>(1, n * std::max<size_t>(list.size(), 1) / 200000);
    std::vector<float> shadowAll(occ.ok ? n * 3 : 0, 0.0f);
    std::string dumpTexels; // (the dump) sample texels: position, normal, lamp, colour before the tests, after the walls, final
    int dumpShaded = 0, dumpOpen = 0, dumpZero = 0;
    for (const ListLight& li : list) {
        if (r0 && !hooks.count(li.light)) continue;
        GpuLampLaw::Lamp law;
        int type = 0, reg = -1;
        if (!RtOutdoorLamps::WorldLaw(li.light, r.lot, false, law, type, reg)) continue;
        // (realistic reach as the GPU path of the experiment applied it: rooms without a roof and lamps registered outdoors)
        const bool reach = reachOn && (rr.outdoor || reg == 0);
        if (reach) RtOutdoorLamps::WorldLaw(li.light, r.lot, true, law, type, reg);
        drawn++;
        std::fill(got.begin(), got.end(), 0.0f);
        if (const auto it = parts.find(li.light); it != parts.end())
            for (const Contrib& p : it->second->parts)
                if (p.texel < n)
                    for (int k = 0; k < 3; k++) got[p.texel * 3 + k] = p.rgb[k];
        std::vector<float> mv, ma; // (room 0) its floor mask parts: the walls-tested and the no-walls copy's luminance
        bool exitLamp = false;
        if (const auto it = parts.find(li.light); r0 && it != parts.end()) {
            mv.assign(n, 0.0f), ma.assign(n, 0.0f), exitLamp = it->second->exit;
            for (const Contrib& p : it->second->parts)
                if (p.texel < n) mv[p.texel] = FromHalf(p.vis), ma[p.texel] = FromHalf(p.all);
        }
        const RtLampModel::Lamp L{law, static_cast<float>(li.mult)};
        // (game tests: the solve gave 0.4 to 0.7 of the law, and neither realistic reach indoors nor the cap explained it)
        // the colour each evaluation left before the game's own wall and object tests: the law apart from those tests
        std::vector<float> hk, wl; // the colour before the game's tests, and after its 2D wall test
        std::vector<const Capture::ProbeHook*> hp; // (the dump) the texel's last evaluation of this lamp
        std::vector<float> pr; // the colour before Apex's path (InnerShadow, the opening, IndoorShadow...)
        uint8_t pathsSeen = 0;
        if (const auto it = hooks.find(li.light); it != hooks.end()) {
            hk.assign(n * 3, 0.0f), wl.assign(n * 3, 0.0f), hp.assign(n, nullptr), pr.assign(n * 3, 0.0f);
            for (const Capture::ProbeHook* h : it->second) {
                for (int k = 0; k < 3; k++) hk[h->texel * 3 + k] += h->rgb[k], wl[h->texel * 3 + k] += h->wall[k], pr[h->texel * 3 + k] += h->pre[k];
                hp[h->texel] = h;
                pathsSeen |= h->paths;
            }
        }
        long lit = 0, match = 0, dark = 0, extra = 0;
        long rays = 0, shadowMatch = 0, shadedOk = 0, leak = 0, over = 0; // (the tested texels against the occluders)
        std::vector<float> ratios;
        float darkWorst = 0.0f, offWorst = 0.0f;
        size_t darkAt = 0, offAt = 0;
        float offMeas[3] = {}, offLaw[3] = {};
        for (size_t i = 0; i < n; i++) {
            const float* s = reinterpret_cast<const float*>(r.samples.data() + i * 0x30);
            float m[3] = {};
            RtLampModel::Add(L, s, s + 4, 1.0f, reach, r.thr, fade, m);
            const float* g = &got[i * 3];
            for (int k = 0; k < 3; k++) measAll[i * 3 + k] += g[k], lawAll[i * 3 + k] += m[k];
            const OutdoorDeltaModel::ProbeClass pc = OutdoorDeltaModel::ProbeClassify(g, m);
            const bool tested = occ.ok && i % stride == 0;
            if (pc == OutdoorDeltaModel::ProbeClass::Unlit) {
                if (tested)
                    for (int k = 0; k < 3; k++) shadowAll[i * 3 + k] += m[k];
                continue;
            }
            lit++;
            if (tested) { // the occluders the GPU would shadow it with (hard: one ray to the lamp's centre)
                float a[3], b[3], nl[3];
                RtLampProxy::ToLot(occ.lot, law.pos, a);
                RtLampProxy::ToLot(occ.lot, s, b);
                RtLampProxy::DirToLot(occ.lot, s + 4, nl);
                for (int k = 0; k < 3; k++) b[k] += 0.02f * nl[k];
                const bool clear = RtLampProxy::Clear(occ.mesh, a, b, 0.05f, 0.01f);
                const bool solveDark = pc == OutdoorDeltaModel::ProbeClass::Dark;
                rays++;
                shadedOk += solveDark && !clear, leak += solveDark && clear, over += !solveDark && !clear;
                const float ms[3] = {clear ? m[0] : 0.0f, clear ? m[1] : 0.0f, clear ? m[2] : 0.0f};
                for (int k = 0; k < 3; k++) shadowAll[i * 3 + k] += ms[k];
                const OutdoorDeltaModel::ProbeClass ps = OutdoorDeltaModel::ProbeClassify(g, ms);
                shadowMatch += ps == OutdoorDeltaModel::ProbeClass::Match || ps == OutdoorDeltaModel::ProbeClass::Unlit;
            }
            const float d = std::max({std::fabs(g[0] - m[0]), std::fabs(g[1] - m[1]), std::fabs(g[2] - m[2])});
            if (pc == OutdoorDeltaModel::ProbeClass::Match) match++;
            else if (pc == OutdoorDeltaModel::ProbeClass::Dark) {
                dark++;
                if (Max3(m) > darkWorst) darkWorst = Max3(m), darkAt = i;
            } else if (pc == OutdoorDeltaModel::ProbeClass::Extra) extra++;
            else {
                const float lm = OutdoorDeltaModel::Lum3(m);
                if (lm > 0.0f) ratios.push_back(OutdoorDeltaModel::Lum3(g) / lm);
                if (d > offWorst) {
                    offWorst = d, offAt = i;
                    std::memcpy(offMeas, g, sizeof offMeas), std::memcpy(offLaw, m, sizeof offLaw);
                }
            }
        }
        float median = 0.0f;
        if (!ratios.empty()) {
            std::nth_element(ratios.begin(), ratios.begin() + ratios.size() / 2, ratios.end());
            median = ratios[ratios.size() / 2];
        }
        const auto pct = [lit](long v) { return lit ? 100.0 * static_cast<double>(v) / static_cast<double>(lit) : 0.0; };
        const auto at = [&](size_t i) {
            const float* s = reinterpret_cast<const float*>(r.samples.data() + i * 0x30);
            return std::format("({:.2f} {:.2f} {:.2f}) n ({:.2f} {:.2f} {:.2f}) {:.2f} m from it", s[0], s[1], s[2], s[4], s[5], s[6],
                               std::sqrt((s[0] - law.pos[0]) * (s[0] - law.pos[0]) + (s[1] - law.pos[1]) * (s[1] - law.pos[1]) + (s[2] - law.pos[2]) * (s[2] - law.pos[2])));
        };
        std::string row = std::format("[RoomLampProbe]   lamp {:08X} type {} x{} (registered in room {}{}): {} texels lit, {:.1f}% within 2/255, {:.1f}% dark in the solve, "
                                      "{:.1f}% lit only by the solve, {:.1f}% other strength (median solve/law {:.3f})",
                                      li.light, type, li.mult, reg, reg == rr.id ? ", this one" : "", lit, pct(match), pct(dark), pct(extra), pct(static_cast<long>(ratios.size())), median);
        if (dark) row += std::format("; darkest miss: law {:.3f} at {}", darkWorst, at(darkAt));
        if (offWorst > 0.0f)
            row += std::format("; largest other: solve ({:.3f} {:.3f} {:.3f}) law ({:.3f} {:.3f} {:.3f}) at {}", offMeas[0], offMeas[1], offMeas[2], offLaw[0], offLaw[1], offLaw[2], at(offAt));
        if (!hk.empty()) {
            long lawLit = 0, lawMatch = 0, seen = 0, seen3 = 0;
            long preLit = 0, preMatch = 0, fSeen = 0, fSame = 0, fZero = 0; // the law vs the colour before Apex's path; the path's factor
            std::vector<float> factors;
            long wOpen = 0, wBlocked = 0, wPartly = 0, oOpen = 0, oBlocked = 0, oPartly = 0; // the wall test, the 3D (object) test
            std::vector<float> lawRatios;
            int lampZero = 0;
            // (the dump) one texel's line: T a lit one, Z one the game lit and Apex left dark (the same fields)
            const auto dumpLine = [&](char tag, size_t i) {
                const float* s = reinterpret_cast<const float*>(r.samples.data() + i * 0x30);
                const float* h = &hk[i * 3];
                dumpTexels += std::format("{} {:08X} p {:.4f} {:.4f} {:.4f} n {:.3f} {:.3f} {:.3f} L {:.4f} {:.4f} {:.4f} hook {:.5f} {:.5f} {:.5f} wall {:.5f} {:.5f} {:.5f} "
                                          "got {:.5f} {:.5f} {:.5f}",
                                          tag, li.light, s[0], s[1], s[2], s[4], s[5], s[6], law.pos[0], law.pos[1], law.pos[2], h[0], h[1], h[2], wl[i * 3], wl[i * 3 + 1],
                                          wl[i * 3 + 2], got[i * 3], got[i * 3 + 1], got[i * 3 + 2]);
                if (const Capture::ProbeHook* ph = hp[i]; ph && ph->raw)
                    dumpTexels += std::format(" from {:.4f} {:.4f} {:.4f} raw {} {:.5f}", ph->from[0], ph->from[1], ph->from[2], ph->raw == 1 ? 1 : 0, ph->rawT);
                else dumpTexels += " raw none";
                if (const Capture::ProbeHook* ph = hp[i])
                    dumpTexels += std::format(" path {} pre {:.5f} {:.5f} {:.5f}", static_cast<int>(ph->paths), pr[i * 3], pr[i * 3 + 1], pr[i * 3 + 2]);
                if (const uint32_t b = batchOf[i]; b < r.batches.size())
                    dumpTexels += std::format(" batch {} n {} flags {} {} c {:.4f} {:.4f} {:.4f}", b, r.batches[b].count, static_cast<int>(r.batches[b].flags[0]),
                                              static_cast<int>(r.batches[b].flags[1]), centroid[b * 3], centroid[b * 3 + 1], centroid[b * 3 + 2]);
                if (const Capture::ProbeHook* ph = hp[i]) dumpTexels += EvTokens(*ph, c.probeEval);
                if (r0)
                    dumpTexels += std::format(" r0 bits {} vis {:.6g} all {:.6g} tvis {:.6g} tall {:.6g} exit {}", static_cast<int>(r.texels[i].bits), mv.empty() ? 0.0f : mv[i],
                                              ma.empty() ? 0.0f : ma[i], r.texels[i].t.vis, r.texels[i].t.all, exitLamp ? 1 : 0);
                dumpTexels += '\n';
            };
            for (size_t i = 0; i < n; i++) {
                const float* s = reinterpret_cast<const float*>(r.samples.data() + i * 0x30);
                float m[3] = {};
                RtLampModel::Add(L, s, s + 4, 1.0f, reach, r.thr, fade, m);
                const float* h = &hk[i * 3];
                const OutdoorDeltaModel::ProbeClass ph = OutdoorDeltaModel::ProbeClassify(h, m);
                if (ph != OutdoorDeltaModel::ProbeClass::Unlit) {
                    lawLit++;
                    lawMatch += ph == OutdoorDeltaModel::ProbeClass::Match;
                    if (ph == OutdoorDeltaModel::ProbeClass::Strength && OutdoorDeltaModel::Lum3(m) > 0.0f)
                        lawRatios.push_back(OutdoorDeltaModel::Lum3(h) / OutdoorDeltaModel::Lum3(m));
                }
                {
                    const float* pv = &pr[i * 3];
                    const OutdoorDeltaModel::ProbeClass pp = OutdoorDeltaModel::ProbeClassify(pv, m);
                    if (pp != OutdoorDeltaModel::ProbeClass::Unlit) preLit++, preMatch += pp == OutdoorDeltaModel::ProbeClass::Match;
                    const float lp = OutdoorDeltaModel::Lum3(pv);
                    if (Max3(pv) >= kLevel && lp > 0.0f) {
                        const float f = OutdoorDeltaModel::Lum3(h) / lp;
                        fSeen++, fSame += std::fabs(f - 1.0f) < 0.01f, fZero += f < 0.01f;
                        if (std::fabs(f - 1.0f) >= 0.01f && f >= 0.01f) factors.push_back(f);
                    }
                }
                // (the dump) an evaluation the game lit and Apex left dark (the floor cut, a slab, a wall of another story)
                if (const Capture::ProbeHook* ph = hp[i]; Max3(h) < kLevel && ph && Max3(ph->c0) >= kLevel && dumpZero < 800 && lampZero < 200) {
                    dumpZero++, lampZero++;
                    dumpLine('Z', i);
                }
                const float lh = OutdoorDeltaModel::Lum3(h);
                if (Max3(h) < kLevel || !(lh > 0.0f)) continue;
                const float lw = OutdoorDeltaModel::Lum3(&wl[i * 3]);
                const float tw = std::clamp(lw / lh, 0.0f, 1.0f); // the 2D wall test's share
                seen++;
                wOpen += tw >= 0.98f, wBlocked += tw <= 0.02f, wPartly += tw > 0.02f && tw < 0.98f;
                if (!(lw > 0.0f) || Max3(&wl[i * 3]) < kLevel) continue;
                const float to = OutdoorDeltaModel::Lum3(&got[i * 3]) / lw; // the 3D test's share (and the threshold's drop)
                seen3++;
                oOpen += to >= 0.98f, oBlocked += to <= 0.02f, oPartly += to > 0.02f && to < 0.98f;
                // (the dump: the shaded texels first, a few open ones, for an offline replay of the 3D test)
                const bool shaded = to < 0.98f;
                if (shaded ? dumpShaded < 1500 : (dumpOpen < 300 && i % 7 == 0)) {
                    (shaded ? dumpShaded : dumpOpen)++;
                    dumpLine('T', i);
                }
            }
            float lm = 0.0f;
            if (!lawRatios.empty()) {
                std::nth_element(lawRatios.begin(), lawRatios.begin() + lawRatios.size() / 2, lawRatios.end());
                lm = lawRatios[lawRatios.size() / 2];
            }
            const auto of = [](long v, long of) { return of ? 100.0 * static_cast<double>(v) / static_cast<double>(of) : 0.0; };
            row += std::format("; the law alone (the colour before the game's wall and object tests): {:.1f}% of {} texels within 2/255 ({:.1f}% other strength, "
                               "median {:.3f}); the game's wall test at {} texels: {:.1f}% open, {:.1f}% blocked, {:.1f}% partly; its 3D (object) test at {} texels "
                               "past the walls: {:.1f}% open, {:.1f}% blocked, {:.1f}% partly",
                               of(lawMatch, lawLit), lawLit, of(static_cast<long>(lawRatios.size()), lawLit), lm, seen, of(wOpen, seen), of(wBlocked, seen), of(wPartly, seen),
                               seen3, of(oOpen, seen3), of(oBlocked, seen3), of(oPartly, seen3));
            float fm = 0.0f;
            if (!factors.empty()) {
                std::nth_element(factors.begin(), factors.begin() + factors.size() / 2, factors.end());
                fm = factors[factors.size() / 2];
            }
            static constexpr const char* kPath[5] = {"own", "InnerShadow", "opening", "CrossFloorShadow", "IndoorShadow"};
            std::string names;
            for (int p = 0; p < 5; p++)
                if (pathsSeen >> p & 1) names += std::string(names.empty() ? "" : "+") + kPath[p];
            row += std::format("; Apex's path {}: the law vs the colour before it {:.1f}% of {} within 2/255; its factor at {} texels: {:.1f}% unchanged, "
                               "{:.1f}% zero, {:.1f}% other (median {:.3f})",
                               names.empty() ? "none" : names, of(preMatch, preLit), preLit, fSeen, of(fSame, fSeen), of(fZero, fSeen),
                               of(static_cast<long>(factors.size()), fSeen), fm);
        }
        if (rays) {
            const auto of = [rays](long v) { return 100.0 * static_cast<double>(v) / static_cast<double>(rays); };
            row += std::format("; with the occluders ({} texels tested): {:.1f}% within 2/255, {:.1f}% dark in both, {:.1f}% dark in the solve only (the GPU "
                               "lights them), {:.1f}% shaded by the occluders only (the GPU darkens them)",
                               rays, of(shadowMatch), of(shadedOk), of(leak), of(over));
        }
        rows.push_back(std::move(row));
    }
    long lit = 0, within = 0;
    for (size_t i = 0; i < n; i++) {
        if (Max3(&measAll[i * 3]) < 0.5f * kLevel && Max3(&lawAll[i * 3]) < 0.5f * kLevel) continue;
        lit++;
        within += std::max({std::fabs(measAll[i * 3] - lawAll[i * 3]), std::fabs(measAll[i * 3 + 1] - lawAll[i * 3 + 1]), std::fabs(measAll[i * 3 + 2] - lawAll[i * 3 + 2])}) <= 2.0f * kLevel;
    }
    long sLit = 0, sWithin = 0;
    if (occ.ok)
        for (size_t i = 0; i < n; i += stride) {
            if (Max3(&measAll[i * 3]) < 0.5f * kLevel && Max3(&shadowAll[i * 3]) < 0.5f * kLevel) continue;
            sLit++;
            sWithin += std::max({std::fabs(measAll[i * 3] - shadowAll[i * 3]), std::fabs(measAll[i * 3 + 1] - shadowAll[i * 3 + 1]),
                                 std::fabs(measAll[i * 3 + 2] - shadowAll[i * 3 + 2])}) <= 2.0f * kLevel;
        }
    if (!ProbeLog()) return;
    const std::string shadowed = occ.ok ? std::format("{:.1f}% with the occluders ({} texels tested)", sLit ? 100.0 * static_cast<double>(sWithin) / static_cast<double>(sLit) : 100.0, sLit)
                                        : std::format("occluders not compared ({})", occ.why);
    LOG_INFO(std::format("[RoomLampProbe] Story {} room {} class {} ({}): {} texels, {} lot lamps of {} lights listed; their light within 2/255 of the GPU law: "
                         "{:.1f}% unshadowed ({} texels lit by either), {}; light no part holds at {} texels; {:.1f} ms",
                         rr.story, rr.id, rr.cls, kind, n, drawn, list.size(), lit ? 100.0 * static_cast<double>(within) / static_cast<double>(lit) : 100.0, lit, shadowed,
                         unheld, MsSince(t0)));
    for (const std::string& row : rows) LOG_INFO(row);
    // (the dump) every light of the room's list: type, colour, position, whether the GPU draws it; for a light it does not draw,
    // a sample of its lit texels (the colour before the game's tests, after its wall test, and its part)
    for (const ListLight& li : list) {
        int32_t type = -1;
        uintptr_t vt = 0;
        float col[3] = {}, pos[3] = {};
        CopyBytes(&type, reinterpret_cast<const void*>(li.light + 0xB0), sizeof type);
        CopyBytes(&vt, reinterpret_cast<const void*>(li.light), sizeof vt);
        CopyBytes(col, reinterpret_cast<const void*>(li.light + 0xE0), sizeof col);
        CopyBytes(pos, reinterpret_cast<const void*>(li.light + 0x120), sizeof pos);
        GpuLampLaw::Lamp law;
        int lawType = 0, reg = -1;
        const bool drawnLight = RtOutdoorLamps::WorldLaw(li.light, r.lot, false, law, lawType, reg);
        dumpTexels += std::format("LIGHT {:08X} type {} vt {:08X} x{} colour {:.3f} {:.3f} {:.3f} at {:.3f} {:.3f} {:.3f} drawn {}", li.light, type, vt, li.mult, col[0],
                                  col[1], col[2], pos[0], pos[1], pos[2], drawnLight ? 1 : 0);
        if (drawnLight) { // (its GPU law: GpuLampLaw::Lamp's 20 floats as bits, for an offline replay)
            static_assert(sizeof(GpuLampLaw::Lamp) == 20 * 4);
            uint32_t lw[20];
            std::memcpy(lw, &law, sizeof lw);
            dumpTexels += " law";
            for (uint32_t v : lw) dumpTexels += std::format(" {:08X}", v);
        }
        dumpTexels += '\n';
        const auto hk = hooks.find(li.light);
        if (drawnLight || hk == hooks.end()) continue;
        const auto pt = parts.find(li.light);
        const size_t step = hk->second.size() / 600 + 1;
        for (size_t j = 0; j < hk->second.size(); j += step) {
            const Capture::ProbeHook& h = *hk->second[j];
            if (Max3(h.rgb) < kLevel) continue;
            float g[3] = {};
            if (pt != parts.end()) {
                const auto& ps = pt->second->parts;
                const auto at = std::lower_bound(ps.begin(), ps.end(), h.texel, [](const Contrib& c, uint32_t t) { return c.texel < t; });
                if (at != ps.end() && at->texel == h.texel) std::memcpy(g, at->rgb, sizeof g);
            }
            const float* s = reinterpret_cast<const float*>(r.samples.data() + static_cast<size_t>(h.texel) * 0x30);
            dumpTexels += std::format("N {:08X} p {:.4f} {:.4f} {:.4f} n {:.3f} {:.3f} {:.3f} hook {:.5f} {:.5f} {:.5f} wall {:.5f} {:.5f} {:.5f} got {:.5f} {:.5f} {:.5f}\n",
                                      li.light, s[0], s[1], s[2], s[4], s[5], s[6], h.rgb[0], h.rgb[1], h.rgb[2], h.wall[0], h.wall[1], h.wall[2], g[0], g[1], g[2]);
        }
    }
    // (room 0) the floor mask's parts of the lamps it recorded (M: per masked texel the walls-tested and no-walls copy's
    // luminance, the texel's totals) and the exit ground texels (E: tapped, not under a floor tile; the exit lamps' parts and
    // the texel the map holds), at most 4000 lines of each a solve
    if (r0) {
        int mLines = 0;
        for (const LampRec& l : r.lamps) {
            if (!c.keep0Lights.count(l.light) || mLines >= 4000) continue;
            GpuLampLaw::Lamp law;
            int type = 0, reg = -1;
            const bool hasLaw = RtOutdoorLamps::WorldLaw(l.light, r.lot, false, law, type, reg);
            size_t masked = 0;
            for (const Contrib& p : l.parts) masked += p.texel < n && (p.vis || p.all);
            const size_t step = masked / 1000 + 1;
            size_t k = 0;
            for (const Contrib& p : l.parts) {
                if (p.texel >= n || !(p.vis || p.all) || k++ % step || mLines >= 4000) continue;
                mLines++;
                const float* s = reinterpret_cast<const float*>(r.samples.data() + static_cast<size_t>(p.texel) * 0x30);
                const TexelRec& t = r.texels[p.texel];
                dumpTexels += std::format("M {:08X} p {:.4f} {:.4f} {:.4f} n {:.3f} {:.3f} {:.3f} L {:.4f} {:.4f} {:.4f} law {} vis {:.6g} all {:.6g} tvis {:.6g} tall {:.6g} bits {}",
                                          l.light, s[0], s[1], s[2], s[4], s[5], s[6], law.pos[0], law.pos[1], law.pos[2], hasLaw ? 1 : 0, FromHalf(p.vis), FromHalf(p.all),
                                          t.t.vis, t.t.all, static_cast<int>(t.bits));
                if (const uint32_t b = batchOf[p.texel]; b < r.batches.size())
                    dumpTexels += std::format(" batch {} n {} flags {} {} c {:.4f} {:.4f} {:.4f}", b, r.batches[b].count, static_cast<int>(r.batches[b].flags[0]),
                                              static_cast<int>(r.batches[b].flags[1]), centroid[b * 3], centroid[b * 3 + 1], centroid[b * 3 + 2]);
                dumpTexels += '\n';
            }
        }
        size_t exits = 0;
        for (size_t i = 0; i < n; i++) exits += (r.texels[i].bits & kExitGround) != 0;
        const size_t step = exits / 4000 + 1;
        std::unordered_map<uint32_t, std::string> eLine; // texel -> its exit lamps' parts
        for (size_t i = 0, k = 0; i < n; i++)
            if ((r.texels[i].bits & kExitGround) && k++ % step == 0) eLine[static_cast<uint32_t>(i)];
        for (const LampRec& l : r.lamps) {
            if (!l.exit) continue;
            for (const Contrib& p : l.parts)
                if (const auto it = eLine.find(p.texel); it != eLine.end())
                    it->second += std::format(" {:08X} {:.6g} {:.6g} {:.6g}", l.light, p.rgb[0], p.rgb[1], p.rgb[2]);
        }
        std::vector<uint32_t> order;
        for (const auto& [i, s] : eLine) order.push_back(i);
        std::sort(order.begin(), order.end());
        for (uint32_t i : order) {
            const float* s = reinterpret_cast<const float*>(r.samples.data() + static_cast<size_t>(i) * 0x30);
            dumpTexels += std::format("E {} p {:.4f} {:.4f} {:.4f} n {:.3f} {:.3f} {:.3f} bits {} packed {:08X} sum {:.6g} {:.6g} {:.6g} exitParts{}\n", i, s[0], s[1], s[2], s[4],
                                      s[5], s[6], static_cast<int>(r.texels[i].bits), r.texels[i].packed, r.texels[i].t.S[0], r.texels[i].t.S[1], r.texels[i].t.S[2], eLine[i]);
        }
    }
    ProbeDump(room, rr, dumpTexels);
}

// ---- GPU room lamps, stage 1 (docs/features/night-lighting/gpu-room-lamps.md): a room's solve kept for the GPU ----
// Per texel and lamp the GPU draws: part = mult x fade(law x A) x B (RtLampModel::KeptAdd), A the colour Apex's tests left
// before the soft light cut over the lamp's law, B the game's 2D wall x 3D share. The texel's base is its sum less those
// parts. Light tree thread only (kept, recomposed and cleared there).
struct KeptRoom {
    uintptr_t room = 0, mgr = 0;
    uint64_t lot = 0;
    int story = 0, id = 0, cls = 0;
    float thr = 0.0f;
    uint32_t version = 0;                  // (stage 2b) g_keptVersion when it was kept
    DWORD keptAt = 0;                      // GetTickCount when its solve was kept (PhysCoverWhy's grace before its maps are drawn)
    unsigned maps = 0;                     // the maps it wrote texels into (bit m)
    bool editable = false;                 // the GPU draws every texel it wrote (no wall left to the game's blur)
    std::vector<uintptr_t> baked;          // lights it lit that the GPU does not draw (their light stays in the base)
    uint32_t mapW[4] = {}, mapH[4] = {};   // the maps the solve wrote (0: none)
    std::vector<uint8_t, ApexMem::HighAllocator<uint8_t>> samples; // the capture's: 0x30 per texel
    std::vector<TexelRec, ApexMem::HighAllocator<TexelRec>> texels; // the capture's: map, place, the solve's sum, the packed texel
    std::vector<float, ApexMem::HighAllocator<float>> base;         // 3 per texel
    std::vector<KeptLamp> lamps;
    // (stage 2a) the tail: per texel (tail, empty when tailWhy), the tile-edge copies to other places, the class-2 wall blocks
    // before the blur (with the rows beyond their edges), the roofed tail's k and the room's ambients at the solve's end
    std::vector<KeptTail, ApexMem::HighAllocator<KeptTail>> tail;
    std::vector<KeptCopy> copies;
    std::vector<BlockRec> blocks;
    std::vector<uint32_t, ApexMem::HighAllocator<uint32_t>> preblur;
    uint32_t passes = 0;
    bool wallsOk = true, roofed = false;
    const char* tailWhy = nullptr;
    long killed = 0;
    float toneK = 0.0f, amb[3] = {}, ambPass[3] = {};
    uint64_t geo = 0; // RoomGeoKey at its solve's start (CarryOff, the move path; 0: not read)
    uint64_t roomGeo = 0; // (physical) its 3D blockers' key at its solve's start (PhysCoverWhy; 0: not read)
    float norm = 1.0f;    // (physical) its normalisation (+0x160) at its solve's end: a fresh lamp's norm scale there
    bool outdoor = false; // (physical) a roofless room
    uint64_t geoParts[3] = {};
    // (stage 3) the solve's batches (their samples' range and test switches), and for a copy a lamp move made (MoveWorker) the
    // version whose drawing it continues until the GPU drew it
    std::vector<BatchRec> batches;
    uint32_t movedFrom = 0;
    bool moveCopy = false; // (a copy a lamp move made: its texels are not the game's, so it has no GPU self-check)
    // (physical indoor lamps, stage 1) kept with no parts: the base is the sum less every lamp of `lamps` (each listed with its law
    // and no part), and the room's 3D test records as its solve started (RoomGeoKey's read); not drawn yet (KeptRooms)
    bool physical = false;
    RoomLightTests::Room tests;
    std::shared_ptr<const RtLampGpu::BlockerTable> blk; // (tests as the GPU's blocker table, and its records' header row)
    float blkHeader = -1.0f;
    // (stage 4) the directional maps: per sample its texel and position, the lights the GPU does not draw summed (basisBase, 16
    // per sample), every other light of the list with its part and its share of the game's step (NaN: not a multiple), the
    // texels at the solve's end, the set they went to; basisWhy: not usable (kNoBasis: the room writes none); basisId: the
    // version whose capture they are (a move's copy keeps it)
    struct BasisLamp {
        uintptr_t light = 0;
        uint8_t mult = 1;
        std::vector<float> c, pass; // 16 / 1 per sample
    };
    std::vector<Capture::BasisSample> basis;
    std::vector<float> basisBase;
    std::vector<BasisLamp> basisLamps;
    std::vector<uint32_t> basisGame;
    uintptr_t basisSet = 0;
    uint32_t basisW = 0, basisH = 0, basisId = 0;
    const char* basisWhy = nullptr;
    size_t bytes = 0;
    // (the day/night cache) the night level's end as its solve started, and its ambient pair as its solve gave it with Rooms at
    // Night's base (ambCe: that base's colour; ambOk: the room held it at the solve's end)
    PhaseCache::Stamp start, keptStamp;
    std::vector<KeptLamp> bakedLaws; // (the day/night cache) the baked lights with a law as kept: their state at the swap
    // ambOwn: the record's own colour (ambPair[0], or what a stairwell's share was added to); ambLls: LevelLightShare held it as
    // the room's own colour, with the normalisation ambNorm
    float ambPair[2][4] = {}, ambCe[4] = {}, ambOwn[4] = {}, ambNorm = 0.0f;
    bool ambOk = false, ambLls = false;
};
} // namespace
// below (physical, stage 5: a room whose directional maps were written physically, kept again; prev: its last kept solve)
void NotePhysBasisKept(uint64_t key, const KeptRoom* prev, const KeptRoom& k);
void CarrySiblings(uint64_t key, uintptr_t set);
uintptr_t RoomSet(uintptr_t room);
void WritePhysCarry();
namespace {
// (stage 2b) g_kept and g_keptBytes: the light tree thread changes them, the render thread reads them (gpu_room_lamps.cpp) and
// any thread's finalize drops a room (RoomFinalized); a kept room never changes once kept
std::mutex g_keptMx;
std::unordered_map<uint64_t, std::shared_ptr<const KeptRoom>> g_kept;
std::atomic<uint32_t> g_keptVersion{0};
size_t g_keptBytes = 0;
// (under g_keptMx; the instant day/night switch) per kept room key the last physical solve kept wholly at each end of the night
// level (PhaseCache): shared with g_kept while it is the room's kept solve; only the other end's are extra bytes
std::unordered_map<uint64_t, PhaseCache::Slots<std::shared_ptr<const KeptRoom>>> g_phaseKept;
size_t PhaseExtraBytes() {
    size_t b = 0;
    for (const auto& [key, s] : g_phaseKept) {
        const auto it = g_kept.find(key);
        for (const auto& p : s.at)
            if (p && (it == g_kept.end() || it->second != p)) b += p->bytes;
    }
    return b;
}
// (light tree thread) the last quick switch: its rooms swapped from the cache, solved since or not, for its summary 10 s later
struct PhaseSwitch {
    uint32_t serial = 0;                         // the clock's quick switch it is
    PhaseCache::End to = PhaseCache::kTwilight;  // its end
    DWORD from = 0;                              // the level's departure from the other end
    bool open = false;                           // its summary not written yet
    std::vector<uint64_t> due;                   // rooms still to try (kSwapsPerUpdate per room update)
    std::vector<std::pair<uint64_t, DWORD>> retry; // rooms tried again from then (a lamp's state not yet as kept)
    std::set<uint64_t> rooms;                    // the active lot's kept physical rooms at the switch
    std::map<uint64_t, std::pair<DWORD, bool>> shown; // room -> when it first showed the new end, swapped (else its solve)
    std::set<uint64_t> solvedAfter;              // swapped rooms whose solve landed since
    std::map<uint64_t, const char*> why;         // room -> why it was not swapped (its last try)
    std::set<uint64_t> sent;                     // sent to their solve by Apex as the level left the other end
};
PhaseSwitch g_phaseSwitch;
// (light tree thread) the rooms sent at the last departure (no cached solve of the other end), and those sent once more
std::map<uint64_t, DWORD> g_phaseSent;
std::set<uint64_t> g_phaseResent;
// (KeepEnd) a solve of a room of the switch begun after its departure landed. A room Apex sent whose solve was not made wholly at
// the end the level is at now (begun in the fade: the sky between the ends) is sent once more, so that end's cache can take it at
// the next departure
void NotePhaseLanded(uint64_t key, const KeptRoom& k) {
    if (const auto ps = g_phaseSent.find(key); ps != g_phaseSent.end() && static_cast<int32_t>(k.start.tick - ps->second) >= 0 && k.keptStamp.at != PhaseCache::kTwilight &&
                                                !PhaseCache::Whole(k.start, k.keptStamp) && g_phaseResent.insert(key).second)
        LevelLightShare::QueueRoomId(RtOutdoorLamps::ActiveTracker(), k.story, k.id, -1);
    PhaseSwitch& s = g_phaseSwitch;
    if (!s.open || !s.rooms.count(key) || static_cast<int32_t>(k.start.tick - s.from) < 0) return;
    const auto [it, fresh] = s.shown.try_emplace(key, k.keptAt, false);
    if (!fresh && it->second.second) s.solvedAfter.insert(key);
}
} // namespace
bool IndoorTopOf(uint64_t lot, int story, int id, std::vector<TopTexel>& out, uint32_t& version, int& cls) {
    out.clear();
    std::shared_ptr<const KeptRoom> best;
    {
        std::lock_guard<std::mutex> lk(g_keptMx);
        // (the class asked for: the one the room shows, whose map its draw and the attic read; -1 or none kept of it: the newest
        // class, IndoorFitOf's)
        for (const auto& [key, k] : g_kept)
            if (k->lot == lot && k->story == story && k->id == id && k->roofed && !k->tail.empty() &&
                (!best || (k->cls == cls) > (best->cls == cls) || ((k->cls == cls) == (best->cls == cls) && k->cls > best->cls)))
                best = k;
    }
    if (!best) return false;
    const KeptRoom& k = *best;
    version = k.version, cls = k.cls;
    const size_t n = std::min({k.texels.size(), k.base.size() / 3, k.samples.size() / 0x30});
    float topY = -1e30f;
    for (size_t i = 0; i < n; i++)
        if (k.texels[i].map == 0) topY = std::max(topY, reinterpret_cast<const float*>(k.samples.data() + i * 0x30)[1]);
    for (size_t i = 0; i < n; i++) {
        const float* smp = reinterpret_cast<const float*>(k.samples.data() + i * 0x30);
        if (k.texels[i].map != 0 || smp[1] < topY - 0.2f) continue; // (its walls' top row: the wall rows end at base + 3)
        const float norm = i < k.tail.size() ? k.tail[i].norm : k.norm;
        TopTexel t{};
        for (int q = 0; q < 3; q++) t.pos[q] = smp[q], t.n[q] = smp[4 + q], t.rgb[q] = std::max(0.0f, norm * k.base[i * 3 + q]);
        t.x = k.texels[i].x, t.y = k.texels[i].y;
        out.push_back(t);
    }
    return !out.empty();
}
uint32_t IndoorVersionOf(uint64_t lot, int story, int id, int cls) {
    std::lock_guard<std::mutex> lk(g_keptMx);
    for (const auto& [key, k] : g_kept)
        if (k->lot == lot && k->story == story && k->id == id && k->cls == cls && k->roofed && !k->tail.empty()) return k->version;
    return 0;
}
bool IndoorFitOf(uint64_t lot, int story, int id, IndoorFit& out) {
    std::shared_ptr<const KeptRoom> best;
    {
        std::lock_guard<std::mutex> lk(g_keptMx);
        for (const auto& [key, k] : g_kept)
            if (k->lot == lot && k->story == story && k->id == id && k->roofed && !k->tail.empty() && (!best || k->cls > best->cls)) best = k;
    }
    if (!best) return false;
    // (each kept solve once: a kept room never changes)
    static std::mutex s_mx;
    static std::unordered_map<const KeptRoom*, IndoorFit> s_fits;
    {
        std::lock_guard<std::mutex> lk(s_mx);
        if (const auto it = s_fits.find(best.get()); it != s_fits.end() && it->second.version == best->version) return out = it->second, true;
    }
    const KeptRoom& k = *best;
    std::vector<std::array<float, 5>> good;
    const size_t n = std::min({k.texels.size(), k.tail.size(), k.samples.size() / 0x30});
    float topY = -1e30f;
    for (size_t i = 0; i < n; i++)
        if (k.texels[i].map == 0) topY = std::max(topY, reinterpret_cast<const float*>(k.samples.data() + i * 0x30)[1]);
    double top[3] = {};
    int topN = 0;
    for (size_t i = 0; i < n; i++) {
        if (k.texels[i].map != 0) continue; // (the wall pass's ambient)
        const KeptTail& tl = k.tail[i];
        float xs[3], c[3], ra[3], r = 0.0f;
        for (int q = 0; q < 3; q++) xs[q] = tl.norm * k.texels[i].t.S[q];
        GpuLampLaw::Tone(xs, c);
        const float* smp = reinterpret_cast<const float*>(k.samples.data() + i * 0x30);
        if (smp[1] >= topY - 0.5f) {
            for (int q = 0; q < 3; q++) top[q] += xs[q];
            topN++;
        }
        if (GpuLampLaw::Weight(c, k.toneK) >= 0.2f && GpuLampLaw::IndoorSplit(xs, tl.p, k.toneK, ra, r)) good.push_back({smp[1], r, ra[0], ra[1], ra[2]});
    }
    const GpuLampLaw::SplitFill f = GpuLampLaw::FitSplit(std::move(good));
    IndoorFit o;
    o.k = k.toneK, o.version = k.version, o.topN = topN;
    for (int q = 0; q < 3; q++) o.topMean[q] = topN ? static_cast<float>(top[q] / topN) : 0.0f;
    if (f.ok) o.fit[0] = f.a, o.fit[1] = f.b, o.fit[2] = f.lo, o.fit[3] = f.hi;
    else o.fit[0] = o.fit[2] = o.fit[3] = 1.0f;
    {
        std::lock_guard<std::mutex> lk(s_mx);
        if (s_fits.size() > 64) s_fits.clear();
        s_fits[best.get()] = o;
    }
    out = o;
    return true;
}
namespace {
// (under g_keptMx) the kept solves the last finalize that dropped any dropped (RoomFinalized): the room's next kept solve on the
// light tree thread carries their switched-off lamps (CarryOff), then they go
std::vector<std::shared_ptr<const KeptRoom>> g_finalized;
// The lamp edits taken (TakesLampEdit, light tree thread): a lamp switched off -> the kept solves (room, version) of the rooms
// listing it then; its switch on is taken only while they are the same (CarryOff moves a room's entry to its next kept solve)
std::unordered_map<uintptr_t, std::vector<std::pair<const void*, uint32_t>>> g_takenOff;
bool g_keptFull = false; // (the log: once per crossing of the budget)
std::unordered_map<uint64_t, size_t> g_keptRefused; // (under g_keptMx) key -> bytes of its solve the budget refused (PhysCoverWhy sends it again once it fits)
std::atomic<int> g_keptLines{0};
bool KeptLog() { return g_keptLines.fetch_add(1, std::memory_order_relaxed) < 2000; }
// The table the GPU draws records from, a copy with DrawnBlockers' seats: the records as read stay what the rest compares and tests
std::shared_ptr<const RtLampGpu::BlockerTable> DrawnTableOf(const std::vector<RoomLightTests::Record>& records, float& header) {
    RoomLightTests::Room d;
    d.rec = records;
    if (DevSwitches::ChairArmCaps()) RtLampGpu::DrawnBlockers(d.rec);
    auto blk = std::make_shared<RtLampGpu::BlockerTable>();
    header = static_cast<float>(blk->Append(d));
    if (DevSwitches::SphereCutFade()) blk->FadeCut(static_cast<int>(header));
    return blk;
}
// (live_object_tables) A kept room's table with its objects as they stand now. Only the room's solve read the records again, and it waits
// for the drag to end: the shadow stayed behind for the whole drag plus 0.48 s. The game's move writes a
// blocker's fields in place (0x0069AD80, the disc's 0x0069BB47 and on), so the addresses a solve read keep following the object: no
// room memory is touched here, and an object gone leaves an old copy (CopyBytes) or, its memory reused by a shape of the same kind,
// wrong geometry until its solve. shortcut: room 0's table and the directional maps wait for the solve, an object new in the room or
// dragged into another casts there at its solve; a live room 0 needs RecordRoom's r.blk republished
constexpr DWORD kLiveMs = 100; // every kept room's blockers are read this often at most, a room with a moved record each frame (RoomLightTests::LiveDue)
uint32_t g_liveStep = 0; // (render thread) ObjectLegs::Gen as this frame's round began: its maps share one read, beside the lamps it reads
struct LiveTable {
    uint32_t version = 0, gen = 0, step = 0;
    uint64_t key = 0; // 0: its objects stand where its solve read them
    std::shared_ptr<const RtLampGpu::BlockerTable> blk;
    int header = -1;
};
std::unordered_map<uint64_t, LiveTable> g_live; // (render thread) by KeyOf
// (live_object_tables) when a kept room's live table last changed, and when that run of changes began (gaps under kLiveRunGapMs join
// a run): an object dragged in a room the GPU draws holds its solve as a dragged lamp does (DragLite::HoldGpuDrawn)
struct LiveRun {
    DWORD first = 0, last = 0;
};
constexpr DWORD kLiveRunGapMs = 750;
std::mutex g_liveRunMx;
std::unordered_map<const void*, LiveRun> g_liveRuns; // (under g_liveRunMx) by room
void NoteLiveMove(const void* room) {
    const DWORD now = GetTickCount();
    std::lock_guard<std::mutex> lk(g_liveRunMx);
    if (g_liveRuns.size() > 512) g_liveRuns.clear();
    LiveRun& r = g_liveRuns[room];
    if (!r.last || now - r.last >= kLiveRunGapMs) r.first = now;
    r.last = now;
}
// ObjectLegs::Gen at most every kLiveMs (0: nothing to read); a line a second of what moved it, for the capture that decides on the carry
uint32_t LiveGen() {
    static uint32_t s_seen = 0, s_logHook = 0;
    static DWORD s_at = 0, s_logAt = 0;
    if (!DevSwitches::LiveObjectTables() || !g_physical.load(std::memory_order_relaxed)) return 0;
    const uint32_t gen = ObjectLegs::Gen();
    const DWORD now = GetTickCount();
    if (gen == s_seen || now - s_at < kLiveMs) return s_seen;
    s_seen = gen, s_at = now;
    uint32_t hook = 0;
    unsigned long thread = 0;
    ObjectLegs::MoveStats(hook, thread);
    if (hook != s_logHook && now - s_logAt >= 1000) {
        if (KeptLog()) LOG_INFO(std::format("[LiveTable] {} object moves in {} ms, the last on thread {}", hook - s_logHook, now - s_logAt, thread));
        s_logHook = hook, s_logAt = now;
    }
    return s_seen;
}
// True: `out` holds k's records as they stand now, `key` says so. False: `key` as it is (0: nothing moved, `was`: still that). A moved record
// has no shape in `out`: the carry leaves it alone, and it follows its object here
bool LiveTests(const KeptRoom& k, uint64_t was, RoomLightTests::Room& out, uint64_t& key) {
    std::vector<RoomLightTests::LiveMove> moved;
    key = RoomLightTests::LiveMoves(k.tests.rec, [](uintptr_t at, RoomLightTests::Blocker& b) {
        const bool ok = CopyBytes(&b.vt, reinterpret_cast<const void*>(at), 4) && CopyBytes(b.f, reinterpret_cast<const void*>(at), sizeof b.f);
        b.vt = TestVt(b.vt);
        return ok;
    }, moved);
    if (!key || key == was) return false;
    out.rec = k.tests.rec;
    for (RoomLightTests::LiveMove& m : moved) out.rec[m.rec].b = std::move(m.b), RoomLightTests::Bound(out.rec[m.rec]);
    for (RoomLightTests::Record& r : out.rec) r.posts.clear();
    ObjectLegs::Fill(out);
    for (const RoomLightTests::LiveMove& m : moved) out.rec[m.rec].shape = 0;
    return true;
}
size_t KeptBytes(const KeptRoom& k) {
    size_t b = sizeof(KeptRoom) + k.samples.capacity() + k.texels.capacity() * sizeof(TexelRec) + k.base.capacity() * sizeof(float) +
               k.tail.capacity() * sizeof(KeptTail) + k.copies.capacity() * sizeof(KeptCopy) + k.blocks.capacity() * sizeof(BlockRec) + k.preblur.capacity() * 4 +
               k.batches.capacity() * sizeof(BatchRec) + k.basis.capacity() * sizeof(Capture::BasisSample) + k.basisBase.capacity() * 4 + k.basisGame.capacity() * 4;
    for (const KeptLamp& l : k.lamps) b += sizeof(KeptLamp) + l.parts.capacity() * sizeof(KeptPart);
    for (const KeptRoom::BasisLamp& l : k.basisLamps) b += sizeof l + (l.c.capacity() + l.pass.capacity()) * 4;
    b += k.tests.walls.capacity() * sizeof(RoomLightTests::Wall) + k.tests.idx2d.capacity() * 4 + k.tests.rec.capacity() * sizeof(RoomLightTests::Record);
    for (const RoomLightTests::Record& r : k.tests.rec) b += r.b.capacity() * sizeof(RoomLightTests::Blocker) + r.at.capacity() * sizeof(uintptr_t);
    if (k.blk) b += sizeof(RtLampGpu::BlockerTable) + k.blk->rows.capacity() * sizeof(RtLampGpu::BlockerTable::Row);
    return b;
}
void ClearKept() {
    std::lock_guard<std::mutex> lk(g_keptMx);
    g_kept.clear();
    g_phaseKept.clear();
    g_finalized.clear();
    g_keptBytes = 0;
    g_keptFull = false;
    g_keptRefused.clear();
}
// The law a light is kept with (any thread): the GPU's law (RtOutdoorLamps::WorldLaw; a TubeLight's: GpuLampLaw::Tube), else its
// colour alone (RtOutdoorLamps::ColourLaw: a TubeLight without a segment, whose evaluation 0x006BFA70 is its colour +0xE0 x a share its colour does not
// change); false: it has neither (its light stays in the base)
bool LawOfLight(uintptr_t light, uint64_t lot, KeptLamp& kl, int* regRoom = nullptr) {
    int type = 0, reg = -1;
    kl.light = light;
    if (RtOutdoorLamps::WorldLaw(light, lot, false, kl.law, type, reg)) {
        if (regRoom) *regRoom = reg;
        kl.type = type, kl.colourOnly = false, kl.geo = 0;
        std::memcpy(kl.pos, kl.law.pos, sizeof kl.pos);
        return true;
    }
    float colour[3] = {};
    uint64_t geo = 0;
    if (!RtOutdoorLamps::ColourLaw(light, lot, colour, geo)) return false;
    kl.law = GpuLampLaw::Lamp{};
    std::memcpy(kl.law.colour, colour, sizeof colour);
    kl.type = 6, kl.colourOnly = true, kl.geo = geo;
    return true;
}
// A kept lamp's law now (false: none now, it adds nothing); moved: its place, or (its colour alone) what its share reads (the
// intensity, the tube's ends and radius, the range), is not the one its parts were kept with
bool LawNow(const KeptLamp& kl, uint64_t lot, GpuLampLaw::Lamp& law, bool& moved) {
    KeptLamp now;
    moved = false;
    if (!LawOfLight(kl.light, lot, now) || now.colourOnly != kl.colourOnly) return false;
    law = now.law;
    moved = kl.colourOnly ? now.geo != kl.geo && (now.law.colour[0] + now.law.colour[1] + now.law.colour[2]) > 0.0f
                          : GpuLampLaw::Moved(now.law, kl.law, 1e-4f);
    return true;
}
// The law at a sample (position, then normal at +4): the GPU's, or the colour alone
void LawAt(const GpuLampLaw::Lamp& law, bool colourOnly, const float* s, float out[3]) {
    if (colourOnly) std::memcpy(out, law.colour, sizeof law.colour);
    else GpuLampLaw::Eval(law, s, s + 4, 0.0f, out);
}
// A light's parts from a capture's hooks: A = the colour before the soft light cut over its law there (r + g + b), B = its
// part of the solve (lr: its parts, null when none) over its colour after the soft light cut (0 when the game never tested it).
// gotSum: n zeros (left so). Returns the parts kept
long KeptPartsOf(KeptLamp& kl, const std::vector<const Capture::ProbeHook*>& hs, const LampRec* lr, const uint8_t* samples, size_t n, std::vector<float>& gotSum,
                 long& noB) {
    if (lr)
        for (const Contrib& p : lr->parts)
            if (p.texel < n) gotSum[p.texel] = (p.rgb[2] + p.rgb[1]) + p.rgb[0];
    kl.parts.reserve(hs.size());
    for (const Capture::ProbeHook* h : hs) {
        const int mult = std::max<int>(1, h->evals);
        const float pre = h->preSum / static_cast<float>(mult);
        if (!(pre > 0.0f) || h->texel >= n) continue;
        const float* s = reinterpret_cast<const float*>(samples + static_cast<size_t>(h->texel) * 0x30);
        float lw[3];
        LawAt(kl.law, kl.colourOnly, s, lw);
        const float ls = (lw[2] + lw[1]) + lw[0];
        const float hs3 = (h->rgb[2] + h->rgb[1]) + h->rgb[0];
        KeptPart p;
        p.texel = h->texel;
        p.a = ToHalf(ls > 1e-9f ? std::min(pre / ls, 60000.0f) : 0.0f);
        p.b = ToHalf(hs3 > 0.0f ? std::clamp(gotSum[h->texel] / hs3, 0.0f, 1.0f) : 0.0f);
        p.thrScale = ToHalf(h->thrScale);
        p.mult = static_cast<uint8_t>(std::min(mult, 255));
        p.bits = static_cast<uint8_t>(h->kbits | (hs3 > 0.0f ? 0 : kKeptNoB));
        noB += hs3 > 0.0f ? 0 : 1;
        kl.parts.push_back(p);
    }
    if (lr)
        for (const Contrib& p : lr->parts)
            if (p.texel < n) gotSum[p.texel] = 0.0f;
    ShrinkExact(kl.parts);
    return static_cast<long>(kl.parts.size());
}
// Room 0's lamps Real-time outdoor lamps does not draw (indoor lamps its list takes through openings), kept as a kept room's
// (GPU room lamps): their parts leave Real-time outdoor lamps' base, and the GPU adds them back (ExportRoom0). An exit lamp's
// parts are also the alpha of the ground texels (kExitGround: the luminance of the exit lamps' light); a part in the floor mask's
// two copies is kept as its luminance over the lamp's law there with N.l ignored (KeptWrapLum). A lamp with a mask part where
// that law is dark stays in the base (its edits take the solves)
void BuildKept0(Capture& c, Record& r) {
    const size_t n = r.texels.size();
    std::unordered_map<uintptr_t, std::vector<const Capture::ProbeHook*>> hooks;
    for (const Capture::ProbeHook& h : c.probeHook)
        if (h.texel < n && c.keep0Lights.count(h.light)) hooks[h.light].push_back(&h);
    std::vector<float> gotSum(n, 0.0f);
    long noB = 0, exits = 0, masks = 0, maskParts = 0;
    std::string left;
    static const std::vector<const Capture::ProbeHook*> s_none;
    for (const LampRec& l : r.lamps) {
        if (!c.keep0Lights.count(l.light) || l.drawn || l.collapsed || l.mult <= 0) continue;
        KeptLamp kl;
        if (!LawOfLight(l.light, r.lot, kl)) continue;
        bool dark = false;
        for (const Contrib& p : l.parts) {
            if ((p.vis == 0 && p.all == 0) || p.texel >= n) continue;
            const float wl = RtLampModel::KeptWrapLum(kl.law, kl.colourOnly, reinterpret_cast<const float*>(r.samples.data() + static_cast<size_t>(p.texel) * 0x30));
            if (!(wl > 1e-9f)) {
                dark = true;
                break;
            }
            kl.mask.push_back(KeptLamp::Mask{p.texel, FromHalf(p.vis) / wl, FromHalf(p.all) / wl});
        }
        if (dark) {
            left += std::format("{}{:08X}", left.empty() ? "" : ", ", l.light);
            continue;
        }
        kl.exit = l.exit;
        exits += l.exit ? 1 : 0, masks += kl.mask.empty() ? 0 : 1, maskParts += static_cast<long>(kl.mask.size());
        ShrinkExact(kl.mask);
        const auto it = hooks.find(l.light);
        KeptPartsOf(kl, it != hooks.end() ? it->second : s_none, &l, r.samples.data(), n, gotSum, noB);
        r.kept0.push_back(std::move(kl));
    }
    if ((!r.kept0.empty() || !left.empty()) && KeptLog())
        LOG_INFO(std::format("[GpuRoomLamps] Story {} room 0 class {}: {} lamps kept for the GPU ({} exit lamps, {} with floor mask parts: {} parts){}", r.story, r.cls,
                             r.kept0.size(), exits, masks, maskParts,
                             left.empty() ? std::string() : std::format("; left in the base (a floor mask part where its law is dark): {}", left)));
}
// The kept solve recomposed from what was kept, with the lamps' laws as they are now, against the solve's own sum (before the
// tone): the share within 2/255 and the worst texels (the log)
void KeptFinalCheck(const KeptRoom& k, const std::vector<float>& sum, const LARGE_INTEGER& t0);
void KeptSelfCheck(const KeptRoom& k) {
    LARGE_INTEGER t0{};
    QueryPerformanceCounter(&t0);
    const size_t n = k.texels.size();
    std::vector<float> sum(k.base.begin(), k.base.end());
    std::vector<GpuLampLaw::Lamp> laws(k.lamps.size());
    std::vector<char> lawOk(k.lamps.size(), 0);
    int lost = 0;
    for (size_t li = 0; li < k.lamps.size(); li++) {
        bool moved = false;
        lawOk[li] = LawNow(k.lamps[li], k.lot, laws[li], moved) ? 1 : 0;
        if (!lawOk[li]) {
            lost++;
            continue;
        }
        for (const KeptPart& p : k.lamps[li].parts) {
            if (p.texel >= n) continue;
            const float* s = reinterpret_cast<const float*>(k.samples.data() + static_cast<size_t>(p.texel) * 0x30);
            float law[3];
            LawAt(laws[li], k.lamps[li].colourOnly, s, law);
            RtLampModel::KeptAdd(law, FromHalf(p.a), FromHalf(p.b), FromHalf(p.thrScale), static_cast<float>(p.mult), (p.bits & kKeptEarly) != 0,
                                 (p.bits & kKeptFade) != 0, k.thr, &sum[static_cast<size_t>(p.texel) * 3]);
        }
    }
    constexpr float kTol = 2.0f / 255.0f;
    long lit = 0, within = 0;
    std::vector<std::pair<float, uint32_t>> worst; // the worst three
    for (size_t i = 0; i < n; i++) {
        const float* S = k.texels[i].t.S;
        const float* R = &sum[i * 3];
        if (std::max({S[0], S[1], S[2], R[0], R[1], R[2]}) < 0.5f / 255.0f) continue;
        lit++;
        const float d = std::max({std::fabs(S[0] - R[0]), std::fabs(S[1] - R[1]), std::fabs(S[2] - R[2])});
        within += d <= kTol;
        if (d > kTol && (worst.size() < 3 || d > worst.back().first)) {
            worst.emplace_back(d, static_cast<uint32_t>(i));
            std::sort(worst.begin(), worst.end(), std::greater<>());
            if (worst.size() > 3) worst.pop_back();
        }
    }
    if (!KeptLog()) return;
    std::string w;
    for (const auto& [d, i] : worst) {
        const float* s = reinterpret_cast<const float*>(k.samples.data() + static_cast<size_t>(i) * 0x30);
        const TexelRec& t = k.texels[i];
        w += std::format("; texel ({:.2f} {:.2f} {:.2f}) {} map: solve ({:.4f} {:.4f} {:.4f}) recomposed ({:.4f} {:.4f} {:.4f})", s[0], s[1], s[2], kMapName[t.map & 3],
                         t.t.S[0], t.t.S[1], t.t.S[2], sum[i * 3], sum[i * 3 + 1], sum[i * 3 + 2]);
        // its largest kept part: the lamp, A, B and the law there
        float best = -1.0f;
        std::string lamp;
        for (size_t li = 0; li < k.lamps.size(); li++) {
            if (!lawOk[li]) continue;
            const auto& parts = k.lamps[li].parts;
            const auto it = std::lower_bound(parts.begin(), parts.end(), i, [](const KeptPart& p, uint32_t x) { return p.texel < x; });
            if (it == parts.end() || it->texel != i) continue;
            float law[3];
            LawAt(laws[li], k.lamps[li].colourOnly, s, law);
            const float m = (law[2] + law[1] + law[0]) * FromHalf(it->a) * FromHalf(it->b) * static_cast<float>(it->mult);
            if (m > best)
                best = m, lamp = std::format(", lamp {:08X} x{} A {:.4f} B {:.4f} law ({:.4f} {:.4f} {:.4f}) bits {}", k.lamps[li].light, static_cast<int>(it->mult),
                                             FromHalf(it->a), FromHalf(it->b), law[0], law[1], law[2], static_cast<int>(it->bits));
        }
        w += lamp;
    }
    LOG_INFO(std::format("[GpuRoomLamps] Story {} room {} class {} self-check: {} texels lit, {:.2f}% within 2/255 of the solve's sum recomposed from the kept "
                         "parts ({} lamps without a law now){}",
                         k.story, k.id, k.cls, lit, lit ? 100.0 * static_cast<double>(within) / static_cast<double>(lit) : 100.0, lost, w));
    KeptFinalCheck(k, sum, t0);
}
// ---- GPU room lamps, stage 2a: the kept texels' tail, to the texel the game leaves in the map ----
// The roofed tail's k (FUN_006a31d0 +0x1D0: "mov ecx,0x011D0BA0" (Steam 1.67.2), +0x1DD: "call 0x00F626F0" (mov eax,ecx; ret),
// +0x1F1: "movss xmm0,[eax]"; dumpbin): the float at that immediate. POD (SEH)
bool ReadToneK(float& k) {
    const uintptr_t f = GameAddr::Get(GameAddr::Id::WallSolve);
    if (!f) return false;
    __try {
        const BYTE* p = reinterpret_cast<const BYTE*>(f + 0x1D0);
        if (p[0] != 0xB9 || p[0xD] != 0xE8 || p[0x21] != 0xF3 || p[0x22] != 0x0F || p[0x23] != 0x10 || p[0x24] != 0x00) return false;
        const BYTE* q = reinterpret_cast<const BYTE*>(f + 0x1D0 + 0xD + 5 + *reinterpret_cast<const int32_t*>(p + 0xE));
        if (q[0] != 0x8B || q[1] != 0xC1 || q[2] != 0xC3) return false;
        k = *reinterpret_cast<const float*>(*reinterpret_cast<const uintptr_t*>(p + 1));
        return k == k && k > -1e30f && k < 1e30f;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// The finalize's class-2 step (FUN_006a0e00: FUN_006a8ca0(classEntry), then FUN_0069efc0(room, bits, classEntry) for the
// floor and ceiling maps): 1 = it copies at this class, 0 = not, -1 = unreadable. POD (SEH)
using MapEdges_t = char(__thiscall*)(void* classEntry);
using EdgeCopy_t = void(__thiscall*)(void* room, uintptr_t bits, void* classEntry);
int MapEdgesTry(const BYTE* room, void*& entry) {
    const uintptr_t edges = GameAddr::Get(GameAddr::Id::MapEdges);
    if (!edges || !GameAddr::Get(GameAddr::Id::EdgeCopy)) return -1;
    __try {
        entry = reinterpret_cast<void*>(*reinterpret_cast<const uintptr_t*>(room + 4) + static_cast<uintptr_t>(*reinterpret_cast<const int*>(room + 0xF4)) * 64);
        return reinterpret_cast<MapEdges_t>(edges)(entry) ? 1 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
}
bool EdgeCopyTry(BYTE* room, uintptr_t bits, void* entry) {
    __try {
        reinterpret_cast<EdgeCopy_t>(GameAddr::Get(GameAddr::Id::EdgeCopy))(room, bits, entry);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool ReadMap(const Desc& d, uint32_t* out) {
    for (uint32_t y = 0; y < d.h; y++)
        if (!CopyBytes(out + static_cast<size_t>(y) * d.w, reinterpret_cast<const void*>(d.base + static_cast<uintptr_t>(d.pitch) * y), static_cast<size_t>(d.w) * 4)) return false;
    return true;
}
// The tile-edge copies of map m (1 floor, 2 ceiling) followed by a dry run: every place holds its own index while the
// game's own copy runs, so each place the copy wrote names the place it came from; then the whole map is put back as it was
// and the finalize copies it for real. The copies are plain texel copies within the map (docs/engine/room-light-maps.md;
// UNVERIFIED beyond 0x0069efc0's loop: 0x006a9de0 / 0x006a9a50 were not read in full).
bool TagCopies(BYTE* room, int m, const Desc& d, void* entry, const std::unordered_map<uint64_t, uint32_t>& last) {
    Capture& c = g_cap;
    Record& r = *c.rec;
    uintptr_t bits = 0;
    if (!CopyBytes(&bits, room + kMapDesc[m], sizeof bits) || bits != d.base) return c.tailWhy = "a tile-edge copy into a map of no known lock", false;
    const size_t w = d.w, cells = w * d.h;
    // the kept places of this map: their texel (the last write)
    std::vector<uint32_t> srcAt(cells, kNoSrc);
    bool any = false;
    for (const auto& [key, i] : last)
        if (static_cast<int>(key >> 32) == m) srcAt[static_cast<size_t>(r.texels[i].y) * w + r.texels[i].x] = i, any = true;
    if (!any) return true;
    // every place holds its own index while the copy runs (a map holds at most 8192 x 8192 places: no two tags alike, and no
    // value the map held can pass for one; 23 rooms had no free range when only the kept places were tagged: black texels of
    // every alpha); each place the copy wrote then names the place it came from
    std::vector<uint32_t> before(cells), after(cells), tags(cells);
    if (!ReadMap(d, before.data())) return c.tailWhy = "a map could not be read before the tile-edge copies", false;
    for (size_t i = 0; i < cells; i++) tags[i] = static_cast<uint32_t>(i);
    // (nothing allocates from the tags to the map put back)
    const bool tagged = WriteRows(d, 0, 0, static_cast<int>(w), static_cast<int>(d.h), tags.data());
    const bool ran = tagged && EdgeCopyTry(room, bits, entry);
    const bool read = ran && ReadMap(d, after.data());
    if (!WriteRows(d, 0, 0, static_cast<int>(w), static_cast<int>(d.h), before.data())) return c.tailWhy = "a map could not be put back after the tile-edge copies", false;
    if (!read) return c.tailWhy = "the tile-edge copies could not be followed", false;
    std::unordered_map<uint32_t, std::pair<uint32_t, uint32_t>> moved; // kept place -> (its new source texel, the value it ends with)
    for (size_t i = 0; i < cells; i++) {
        const uint32_t from = after[i];
        if (from == i) continue;
        if (from >= cells) return c.tailWhy = "a tile-edge copy wrote a value that is no place of its map", false;
        if (srcAt[i] != kNoSrc) moved[static_cast<uint32_t>(i)] = std::make_pair(srcAt[from], before[from]); // (kNoSrc: from a place no capture holds)
        else if (srcAt[from] != kNoSrc) c.copies.push_back(KeptCopy{static_cast<uint8_t>(m), static_cast<uint16_t>(i % w), static_cast<uint16_t>(i / w), srcAt[from], before[from]});
    }
    for (size_t i = 0; i < r.texels.size(); i++) {
        const TexelRec& t = r.texels[i];
        if (t.map != m) continue;
        const auto it = moved.find(static_cast<uint32_t>(static_cast<size_t>(t.y) * w + t.x));
        if (it == moved.end()) continue;
        c.tail[i].src = it->second.first, c.tail[i].final = it->second.second;
        c.killed += it->second.first == kNoSrc ? 1 : 0;
    }
    return true;
}
// Before the finalize: what each kept texel's place holds (the class-2 walls blurred; the floor and ceiling as packed) and,
// at class 2, the tile-edge copies the finalize is about to make (light tree thread)
void KeepFinals(BYTE* room) {
    Capture& c = g_cap;
    Record& r = *c.rec;
    const size_t n = r.texels.size();
    if (c.tailWhy) return;
    if (c.tail.size() != n || std::any_of(c.tail.begin(), c.tail.end(), [](const KeptTail& t) { return !t.seen; })) {
        c.tailWhy = "a texel packed without its pack input (Smooth light maps' pack hook)";
        return;
    }
    RoomRead rr;
    if (!ReadRoom(room, rr)) return void(c.tailWhy = "the room could not be read");
    c.roofed = !rr.outdoor;
    if (c.roofed && !ReadToneK(c.toneK)) return void(c.tailWhy = "the roofed tail's k could not be read");
    CopyBytes(c.amb, room + 0x110, sizeof c.amb), CopyBytes(c.ambPass, room + 0x650, sizeof c.ambPass);
    CopyBytes(c.ambPair, room + 0x110, sizeof c.ambPair); // (+0x110 and +0x120 are adjacent)
    try {
        std::unordered_map<uint64_t, uint32_t> last; // place -> its last write
        last.reserve(n);
        for (uint32_t i = 0; i < n; i++) last[TexelKey(r.texels[i].map, r.texels[i].x, r.texels[i].y)] = i;
        Desc d[4];
        bool have[4] = {};
        for (size_t i = 0; i < n; i++) {
            const TexelRec& t = r.texels[i];
            if (!have[t.map] && !(have[t.map] = ReadDesc(room, t.map, d[t.map]))) return void(c.tailWhy = "a map is not locked at the solve's end");
            if (t.x >= d[t.map].w || t.y >= d[t.map].h ||
                !ReadTexel(d[t.map].base + static_cast<uintptr_t>(d[t.map].pitch) * t.y + static_cast<uintptr_t>(t.x) * 4, c.tail[i].final))
                return void(c.tailWhy = "a texel could not be read at the solve's end");
            c.tail[i].src = last[TexelKey(t.map, t.x, t.y)];
        }
        void* entry = nullptr;
        const int edges = rr.cls == 2 && (have[1] || have[2]) ? MapEdgesTry(room, entry) : 0;
        if (edges < 0) return void(c.tailWhy = "the finalize's tile-edge step could not be read");
        for (const int m : {1, 2})
            if (edges > 0 && have[m] && !TagCopies(room, m, d[m], entry, last)) return;
        c.finals = true;
    } catch (...) {
        c.tailWhy = "no memory";
    }
}
// Every kept texel's final texel recomposed from its recomposed sum (KeptSelfCheck's): the solve's tail (RtLampModel::
// KeptTexel), the game's pack, the class-2 wall blur (BlurBlock over the blocks before the blur, with the rows beyond their
// edges, in BlurWalls' order) and the tile-edge copies, against what its place held when the solve ended (the log)
void KeptFinalCheck(const KeptRoom& k, const std::vector<float>& sum, const LARGE_INTEGER& t0) {
    const size_t n = k.texels.size();
    if (k.tailWhy || k.tail.size() != n) {
        LOG_INFO(std::format("[GpuRoomLamps] Story {} room {} class {} final self-check: not run ({})", k.story, k.id, k.cls, k.tailWhy ? k.tailWhy : "no tail kept"));
        return;
    }
    // each texel packed again (before the blur)
    std::vector<uint32_t> fin(n);
    long noSplit = 0;
    for (size_t i = 0; i < n; i++) {
        const KeptTail& t = k.tail[i];
        const TexelRec& x = k.texels[i];
        float c[4];
        noSplit += RtLampModel::KeptTexel(&sum[i * 3], x.t.S, t.p, t.norm, k.toneK, k.roofed, (t.bits & kTailFloor) ? RoomMapDither::kRoofedAlphaMin : 0.0f,
                                          (t.bits & kTailDither) ? RoomMapDither::Offset(x.x, x.y) / 255.0f : 0.0f, c)
                       ? 0
                       : 1;
        fin[i] = PackGame(c, 0, 0, false);
    }
    // the class-2 walls blurred again; a wall texel of no block BlurWalls blurred (or walls the game's own blur did) is not
    // recomposed
    std::vector<uint32_t> out(fin);
    std::vector<uint8_t> lost(n, 0);
    long wallLost = 0;
    if (k.cls == 2) {
        std::unordered_map<uint32_t, uint32_t> at; // wall place -> its last write
        for (uint32_t i = 0; i < n; i++)
            if (k.texels[i].map == 0 && k.tail[i].src == i) at[static_cast<uint32_t>(k.texels[i].y) << 16 | k.texels[i].x] = i, lost[i] = 1;
        if (k.passes && k.wallsOk) {
            std::vector<uint32_t> blk, ext;
            for (const BlockRec& b : k.blocks) {
                if (!b.have) continue;
                const int w = b.x1 - b.x0 + 1, rows = b.y1 - b.y0 + 1, top = b.above ? 2 : 0, total = top + rows + (b.below ? 2 : 0);
                blk.assign(k.preblur.begin() + b.off, k.preblur.begin() + b.off + static_cast<size_t>(w) * rows);
                for (int y = 0; y < rows; y++)
                    for (int x = 0; x < w; x++)
                        if (const auto it = at.find(static_cast<uint32_t>(b.y0 + y) << 16 | static_cast<uint32_t>(b.x0 + x)); it != at.end()) blk[static_cast<size_t>(y) * w + x] = fin[it->second];
                ext.assign(static_cast<size_t>(total) * w, 0);
                if (b.above) std::copy_n(k.preblur.begin() + b.ghostOff, static_cast<size_t>(w) * 2, ext.begin());
                std::copy(blk.begin(), blk.end(), ext.begin() + static_cast<size_t>(top) * w);
                if (b.below) std::copy_n(k.preblur.begin() + b.ghostOff + static_cast<size_t>(w) * 2, static_cast<size_t>(w) * 2, ext.begin() + static_cast<size_t>(top + rows) * w);
                FollowGhosts(ext.data(), &k.preblur[b.off], w, rows, b.above, b.below);
                BlurBlock(ext.data(), blk.data(), w, rows, b.above, b.below, k.passes);
                for (int y = 0; y < rows; y++)
                    for (int x = 0; x < w; x++)
                        if (const auto it = at.find(static_cast<uint32_t>(b.y0 + y) << 16 | static_cast<uint32_t>(b.x0 + x)); it != at.end())
                            out[it->second] = ext[static_cast<size_t>(top + y) * w + x], lost[it->second] = 0;
            }
        }
        for (const auto& [place, i] : at) wallLost += lost[i];
    }
    // against the game's
    long N = 0, within = 0, killed = 0;
    int alphaMax = 0;
    std::vector<std::pair<int, int64_t>> worst; // (levels, texel; -1 - copy), the worst three
    const auto compare = [&](uint32_t game, uint32_t src, int64_t who) {
        N++;
        int d = 0;
        for (int s = 0; s < 24; s += 8) d = std::max(d, std::abs(static_cast<int>((game >> s) & 0xFFu) - static_cast<int>((out[src] >> s) & 0xFFu)));
        alphaMax = std::max(alphaMax, std::abs(static_cast<int>(game >> 24) - static_cast<int>(out[src] >> 24)));
        within += d <= 2;
        if (d > 2 && (worst.size() < 3 || d > worst.back().first)) {
            worst.emplace_back(d, who);
            std::sort(worst.begin(), worst.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
            if (worst.size() > 3) worst.pop_back();
        }
    };
    for (size_t i = 0; i < n; i++) {
        const uint32_t s = k.tail[i].src;
        if (s == kNoSrc) killed++;
        else if (!lost[s]) compare(k.tail[i].final, s, static_cast<int64_t>(i));
    }
    for (size_t j = 0; j < k.copies.size(); j++)
        if (!lost[k.copies[j].src]) compare(k.copies[j].final, k.copies[j].src, -1 - static_cast<int64_t>(j));
    std::string w;
    for (const auto& [d, who] : worst) {
        const uint32_t s = who >= 0 ? k.tail[static_cast<size_t>(who)].src : k.copies[static_cast<size_t>(-1 - who)].src;
        const TexelRec& t = k.texels[s];
        const KeptTail& tl = k.tail[s];
        const float* R = &sum[static_cast<size_t>(s) * 3];
        float xs[3], x[3], tone[3], rampAmb[3] = {}, ramp = 0.0f;
        for (int q = 0; q < 3; q++) xs[q] = tl.norm * t.t.S[q], x[q] = tl.norm * std::max(R[q], 0.0f);
        GpuLampLaw::Tone(x, tone);
        const bool split = k.roofed && GpuLampLaw::IndoorSplit(xs, tl.p, k.toneK, rampAmb, ramp);
        const int gx = who >= 0 ? k.texels[static_cast<size_t>(who)].x : k.copies[static_cast<size_t>(-1 - who)].x;
        const int gy = who >= 0 ? k.texels[static_cast<size_t>(who)].y : k.copies[static_cast<size_t>(-1 - who)].y;
        const uint32_t game = who >= 0 ? k.tail[static_cast<size_t>(who)].final : k.copies[static_cast<size_t>(-1 - who)].final;
        w += std::format("; {} levels at {} ({}, {}){}{}: sum ({:.4f} {:.4f} {:.4f}) solve's ({:.4f} {:.4f} {:.4f}) toned ({:.4f} {:.4f} {:.4f}) ramp {:.4f} "
                         "ramp x ambient ({:.4f} {:.4f} {:.4f}){} dither {:+.2f} pack input ({:.4f} {:.4f} {:.4f} {:.4f}) packed game {:08X} recomposed {:08X}, "
                         "final game {:08X} recomposed {:08X}",
                         d, kMapName[t.map & 3], gx, gy, s != static_cast<uint32_t>(who) ? std::format(" (copy of ({}, {}))", t.x, t.y) : "",
                         t.map == 0 && k.cls == 2 ? " (blurred)" : "", R[0], R[1], R[2], t.t.S[0], t.t.S[1], t.t.S[2], tone[0], tone[1], tone[2], ramp, rampAmb[0],
                         rampAmb[1], rampAmb[2], k.roofed && !split ? " (not read back)" : "", (tl.bits & kTailDither) ? RoomMapDither::Offset(t.x, t.y) : 0.0f, tl.p[0],
                         tl.p[1], tl.p[2], tl.p[3], t.packed, fin[s], game, out[s]);
    }
    LOG_INFO(std::format("[GpuRoomLamps] Story {} room {} class {} final self-check: {} texels, {:.2f}% within 2/255 (alpha within {}/255){}; {} room (norm {:.4f}, "
                         "k {:.4f}, ambient ({:.3f} {:.3f} {:.3f}), pass ambient ({:.3f} {:.3f} {:.3f})), {} tile-edge copies to other places, {} texels a copy of a "
                         "texel no capture holds overwrote, {} wall texels not blurred again, {} roofed texels whose ramp could not be read back; {:.1f} ms with the "
                         "sum's check",
                         k.story, k.id, k.cls, N, N ? 100.0 * static_cast<double>(within) / static_cast<double>(N) : 100.0, alphaMax, w, k.roofed ? "roofed" : "roofless",
                         n ? k.tail[0].norm : 0.0f, k.toneK, k.amb[0], k.amb[1], k.amb[2], k.ambPass[0], k.ambPass[1], k.ambPass[2], k.copies.size(), killed,
                         wallLost, noSplit, MsSince(t0)));
}
// A room kept again while lamps of it are switched off by a taken edit (its settle's solve, or any solve): its new kept solve
// holds none of their parts, so each such lamp's parts are carried over from the room's earlier kept solve (the one its switch
// off was taken with) when the texels are the same places with the same samples, the lot's geometry key and the room's 3D
// blockers are the same, and the lamp has not moved (a tube: what its share reads is the same). Its switch on is then drawn
// from them (TakesLampEdit). Light tree thread, before k is kept (k.version set)
bool SameBasisSamples(const KeptRoom& a, const KeptRoom& b) { // (stage 4) both solves' directional samples: the same texels, the same places
    return !a.basisWhy && !b.basisWhy && a.basis.size() == b.basis.size() &&
           std::equal(a.basis.begin(), a.basis.end(), b.basis.begin(),
                      [](const Capture::BasisSample& p, const Capture::BasisSample& q) { return p.x == q.x && p.y == q.y && std::memcmp(p.pos, q.pos, sizeof p.pos) == 0; });
}
void CarryOff(KeptRoom& k, uint64_t key) {
    std::shared_ptr<const KeptRoom> old;
    {
        std::lock_guard<std::mutex> lk(g_keptMx);
        for (const auto& f : g_finalized)
            if (KeyOf(reinterpret_cast<const void*>(f->room), f->cls) == key) old = f;
        g_finalized.clear();
        if (const auto it = g_kept.find(key); !old && it != g_kept.end()) old = it->second;
    }
    if (!old || g_takenOff.empty()) return;
    const void* room = reinterpret_cast<const void*>(k.room);
    std::vector<const KeptLamp*> off;
    for (const KeptLamp& kl : old->lamps) {
        const auto it = g_takenOff.find(kl.light);
        if (it == g_takenOff.end() || std::find(it->second.begin(), it->second.end(), std::make_pair(room, old->version)) == it->second.end()) continue;
        if (std::any_of(k.lamps.begin(), k.lamps.end(), [&](const KeptLamp& n) { return n.light == kl.light; }) ||
            std::find(k.baked.begin(), k.baked.end(), kl.light) != k.baked.end())
            continue; // (the new solve has it: its own parts stand)
        off.push_back(&kl);
    }
    if (off.empty()) return;
    const char* why = nullptr;
    if (!k.geo || old->geo != k.geo) why = GeoChanged(old->geoParts, k.geoParts);
    else if (old->texels.size() != k.texels.size() || old->samples.size() != k.samples.size() ||
             std::memcmp(old->samples.data(), k.samples.data(), k.samples.size()) != 0 ||
             !std::equal(old->texels.begin(), old->texels.end(), k.texels.begin(), [](const TexelRec& a, const TexelRec& b) { return a.x == b.x && a.y == b.y && a.map == b.map; }))
        why = "its texels or samples changed";
    std::string carried, dropped;
    for (const KeptLamp* kl : off) {
        const char* lampWhy = why;
        if (!lampWhy) {
            KeptLamp now;
            if (!LawOfLight(kl->light, k.lot, now) || now.colourOnly != kl->colourOnly) lampWhy = "it has no law now";
            else if (kl->colourOnly ? now.geo != kl->geo : GpuLampLaw::Moved(now.law, kl->law, 1e-4f))
                lampWhy = "it moved or its shape changed";
        }
        auto& rooms = g_takenOff[kl->light];
        if (lampWhy) {
            dropped += std::format("{}{:08X} ({})", dropped.empty() ? "" : ", ", kl->light, lampWhy);
            continue; // (its entry keeps the earlier version: its switch on takes the solves)
        }
        k.lamps.push_back(*kl);
        k.lamps.back().carried = true;
        // (stage 4) its directional part too, from the same samples (the switch on is then written from it)
        const auto ob = std::find_if(old->basisLamps.begin(), old->basisLamps.end(), [&](const KeptRoom::BasisLamp& b) { return b.light == kl->light; });
        if (ob != old->basisLamps.end() && SameBasisSamples(*old, k) &&
            std::none_of(k.basisLamps.begin(), k.basisLamps.end(), [&](const KeptRoom::BasisLamp& b) { return b.light == kl->light; }))
            k.basisLamps.push_back(*ob);
        std::replace(rooms.begin(), rooms.end(), std::make_pair(room, old->version), std::make_pair(room, k.version));
        std::sort(rooms.begin(), rooms.end());
        carried += std::format("{}{:08X}", carried.empty() ? "" : ", ", kl->light);
    }
    if (KeptLog())
        LOG_INFO(std::format("[GpuRoomLamps] Story {} room {} class {}: switched-off lamps' parts{}{}", k.story, k.id, k.cls,
                             carried.empty() ? std::string() : std::format(" carried from its last kept solve: {}", carried),
                             dropped.empty() ? std::string() : std::format("{} not carried: {}", carried.empty() ? "" : ";", dropped)));
}
// ---- (stage 4) the directional maps (docs/features/night-lighting/gpu-room-lamps.md "Directional maps") ----
constexpr const char* kNoBasis = "it writes no directional map";
// A directional texel as FUN_006a09f0 packs it: map m of the accumulator in [0, 1] with alpha 1, the game's pack 0x0060CD10
uint32_t PackBasis(const float* acc16, int m) {
    alignas(16) float in[4];
    BasisPackInput(acc16, m, in);
    return PackGame(in, 0, 0, false);
}
// The kept solve's directional parts: the lights it keeps in its base summed, every other light's part with its share of the
// game's step, and the check that every texel packed again from them is the game's (only a room that passes takes edits)
void BuildBasis(Capture& c, KeptRoom& k) {
    const size_t n = c.basis.size();
    k.basisWhy = c.basisWhy                                ? c.basisWhy
                 : !LevelLightShare::BasisSamplesVec()     ? "the directional maps' hook is not installed"
                 : n == 0                                  ? kNoBasis
                 : c.basisGame.size() != n * 4             ? "its directional maps were not read at the solve's end"
                                                           : nullptr;
    if (k.basisWhy) return;
    k.basisBase.assign(n * 16, 0.0f);
    std::unordered_map<uintptr_t, size_t> idx;
    // (physical, stage 5) the base holds the lights without a law exactly as the maps' base does (KeepEnd's `baked`), and a light
    // of the list no light map texel evaluated by the same rule: every lamp with a law is drawn from it (BasisDuePhys)
    std::unordered_map<uintptr_t, bool> inBase;
    const auto based = [&](uintptr_t light) {
        const auto [it, fresh] = inBase.try_emplace(light, std::find(k.baked.begin(), k.baked.end(), light) != k.baked.end());
        if (fresh && !it->second && c.phys && std::none_of(k.lamps.begin(), k.lamps.end(), [&](const KeptLamp& l) { return l.light == light; })) {
            KeptLamp kl;
            it->second = !LawOfLight(light, k.lot, kl) || kl.colourOnly;
        }
        return it->second;
    };
    long parts = 0, known = 0;
    for (const Capture::BasisPart& p : c.basisParts) {
        if (p.sample >= n) continue;
        if (based(p.light)) {
            for (int i = 0; i < 16; i++) k.basisBase[static_cast<size_t>(p.sample) * 16 + i] += p.c[i];
            continue;
        }
        const auto [it, fresh] = idx.try_emplace(p.light, k.basisLamps.size());
        if (fresh) {
            KeptRoom::BasisLamp& bl = k.basisLamps.emplace_back();
            bl.light = p.light, bl.mult = std::max<uint8_t>(p.mult, 1);
            bl.c.assign(n * 16, 0.0f), bl.pass.assign(n, std::nanf(""));
        }
        KeptRoom::BasisLamp& bl = k.basisLamps[it->second];
        std::memcpy(&bl.c[static_cast<size_t>(p.sample) * 16], p.c, sizeof p.c);
        float pass = 0.0f;
        known += BasisPass(p.c, p.step, pass) ? 1 : 0;
        bl.pass[p.sample] = pass;
        parts++;
    }
    std::vector<float> acc(k.basisBase);
    for (const KeptRoom::BasisLamp& bl : k.basisLamps)
        for (size_t q = 0; q < acc.size(); q++) acc[q] += bl.c[q];
    long within = 0;
    int worst = 0;
    for (size_t i = 0; i < n; i++)
        for (int m = 0; m < 4; m++) {
            const int d = MaxLevelDiff(PackBasis(&acc[i * 16], m), c.basisGame[i * 4 + m]);
            within += d <= 1 ? 1 : 0, worst = std::max(worst, d);
        }
    if (within != static_cast<long>(n * 4)) k.basisWhy = "its directional texels packed again from their parts differ from the game's";
    if (KeptLog())
        LOG_INFO(std::format("[GpuRoomLamps] Story {} room {} class {}: {} directional samples, {} lamps ({} parts, {} of them the game's step called again times a "
                             "share) packed again from their parts: {} of {} texels within 1/255 (largest {}/255){}",
                             k.story, k.id, k.cls, n, k.basisLamps.size(), parts, known, within, n * 4, worst, k.basisWhy ? "; its directional maps take no edit" : ""));
    k.basis = std::move(c.basis), k.basisGame = std::move(c.basisGame), k.basisSet = c.basisSet, k.basisW = c.basisW, k.basisH = c.basisH;
    ShrinkExact(k.basis), ShrinkExact(k.basisBase), ShrinkExact(k.basisGame);
}

// (physical indoor lamps) a lamp's scale in a room: the room it shines out of as InnerShadow scales it (the record's innerSrc, any
// story); else a lamp registered in another room of this story (reg), at that room's normalisation against this one's
float NormKOf(const BYTE* room, int story, int id, bool outdoor, uintptr_t light, int reg) {
    if (const float k = LevelLightShare::InnerNormScaleOf(room, light); k > 0.0f) return k;
    const uintptr_t tracker = reg > 0 && reg != id ? RtOutdoorLamps::TrackerOf(room) : 0;
    const void* src = tracker ? LevelLightShare::RoomOf(tracker, story, reg) : nullptr;
    float normSrc = 1.0f, normHere = 1.0f;
    if (!src || !CopyBytes(&normSrc, static_cast<const BYTE*>(src) + 0x160, sizeof normSrc)) return 1.0f;
    if (!outdoor) CopyBytes(&normHere, room + 0x160, sizeof normHere);
    return std::isfinite(normSrc) && std::isfinite(normHere) ? InnerLight::NormScale(normSrc, normHere, outdoor) : 1.0f;
}
// (scene recordings) a physical room solve just kept, with the game's parts of its listed lamps (light tree thread)
void SceneWriteRoom(const KeptRoom& k, const Record& r) {
    if (!g_sceneOn.load(std::memory_order_relaxed)) return;
    SceneRecord::Room s;
    s.h.tick = GetTickCount(), s.h.lot = k.lot, s.h.story = k.story, s.h.id = k.id, s.h.cls = k.cls, s.h.thr = k.thr, s.h.norm = k.norm;
    s.h.radius = Penumbra::Radius(), s.h.fade = LevelLightShare::SoftLightCutOn(), s.h.reachOn = LevelLightShare::RealisticReachActive(), s.h.outdoor = k.outdoor;
    if (k.blk) s.blk = k.blk->rows, s.h.blkHeader = k.blkHeader;
    const size_t n = k.texels.size();
    if (k.samples.size() < n * 0x30) return;
    std::vector<uint8_t> tested(n, 0);
    for (const BatchRec& b : k.batches)
        for (uint32_t j = 0; j < b.count && b.first + j < n; j++) tested[b.first + j] = b.flags[1] != 0;
    s.texels.resize(n);
    for (size_t i = 0; i < n; i++) {
        const float* p = reinterpret_cast<const float*>(k.samples.data() + i * 0x30);
        SceneRecord::Texel& t = s.texels[i];
        std::memcpy(t.p, p, sizeof t.p), std::memcpy(t.n, p + 4, sizeof t.n);
        t.x = k.texels[i].x, t.y = k.texels[i].y, t.map = k.texels[i].map, t.tested = tested[i], t.pad[0] = t.pad[1] = 0;
    }
    std::unordered_map<uintptr_t, const LampRec*> got;
    for (const LampRec& l : r.lamps) got[l.light] = &l;
    for (const KeptLamp& kl : k.lamps) {
        const auto it = got.find(kl.light);
        if (kl.colourOnly || it == got.end() || it->second->collapsed) continue; // (no part of its own: nothing to compare)
        SceneRecord::Lamp L;
        L.light = kl.light, L.law = kl.law, L.type = kl.type, L.row = static_cast<float>(std::max<int>(kl.mult, 1)) * kl.normK, L.reach = kl.reach ? 1 : 0;
        std::vector<SceneRecord::Part> parts;
        for (const Contrib& p : it->second->parts)
            if (p.texel < n) parts.push_back({p.texel, {p.rgb[0], p.rgb[1], p.rgb[2]}});
        s.lamps.push_back(L), s.parts.push_back(std::move(parts));
    }
    // (the oracle's line: without the cube shadow, which only tools/scene_replay traces; bounded, the light tree thread waits)
    size_t work = 0;
    for (const auto& p : s.parts) work += n + p.size();
    if (work <= 2000000) {
        SceneRecord::Stats st;
        SceneRecord::Compare(s, nullptr, nullptr, st);
        static const char* const kSurf[4] = {"wall", "floor", "ceiling", "objects"};
        std::string surf;
        for (int m = 0; m < 4; m++)
            if (st.mapN[m]) surf += std::format(", {} {:.1f}%", kSurf[m], 100.0 * static_cast<double>(st.mapWithin[m]) / static_cast<double>(st.mapN[m]));
        int top = SceneRecord::kUnknown;
        for (int t = SceneRecord::kShadow3D; t < SceneRecord::kTags; t++)
            if (st.tags[t] > st.tags[top]) top = t;
        if (st.n)
            LOG_INFO(std::format("[GpuOracle] story {} room {} class {}: within2 {:.1f}% of {} lamp texels{}; max err {:.3f} (lamp {:08X} texel {}), most misses {}; "
                                 "furniture fill lifts {}",
                                 k.story, k.id, k.cls, 100.0 * static_cast<double>(st.within) / static_cast<double>(st.n), st.n, surf, st.worst,
                                 static_cast<uint32_t>(st.worstLight), st.worstTexel, st.within == st.n ? "none" : SceneRecord::TagName(top), st.filled));
    }
    uint32_t gen = 0;
    const std::shared_ptr<const RtLampProxy::Lot> lot = RtOutdoorLamps::KeptLot(gen);
    std::lock_guard<std::mutex> lk(g_sceneMx);
    if (!g_sceneFile || std::ftell(g_sceneFile) > kSceneMaxBytes) return;
    if (lot && gen != g_sceneGen) SceneRecord::WriteLot(g_sceneFile, gen, *lot), g_sceneGen = gen;
    s.h.lotGen = g_sceneGen;
    SceneRecord::WriteRoom(g_sceneFile, s);
    g_sceneRooms++;
}
void KeepEnd(const BYTE* room) {
    Capture& c = g_cap;
    Record& r = *c.rec;
    RoomRead rr;
    if (!ReadRoom(room, rr)) return;
    if (g_keptClear.exchange(false)) ClearKept();
    // (stage 2b) the class the solve wrote is the one at its start: a room whose class changed meanwhile (a floor switch sends
    // it to the shown story's class) wrote maps of the other class's size; it is not kept (a 64 x 64 class-0 solve was kept as
    // class 2)
    if (c.ok && rr.cls != r.cls) {
        if (KeptLog()) LOG_INFO(std::format("[GpuRoomLamps] Story {} room {} class {}: not kept (its class was {} when its solve started)", rr.story, rr.id, rr.cls, r.cls));
        // (a lot made active solves its rooms at class 0 and shows them at class 2 meanwhile: a room no lamp lists was solved
        // again only by an edit's fallback, 11 s after it in a log, refusing every physical edit until then;
        // a room gone to class 0, its lot no longer lit at the high quality, keeps nothing worth a solve)
        if (rr.cls != 0 && c.phys) NoteUnkept(room); // (Replica refuses no edit for a room it does not list: no load spike)
        return;
    }
    if (c.phys != g_physical.load(std::memory_order_relaxed)) { // (the model changed during its solve: the next solve keeps it)
        if (KeptLog()) LOG_INFO(std::format("[GpuRoomLamps] Story {} room {} class {}: not kept (the model changed during its solve)", rr.story, rr.id, rr.cls));
        return NoteUnkept(room);
    }
    if (!c.ok || r.samples.size() != r.texels.size() * 0x30) {
        if (KeptLog()) LOG_INFO(std::format("[GpuRoomLamps] Story {} room {} class {}: not kept ({})", rr.story, rr.id, rr.cls, c.ok ? "the samples do not match the texels" : c.why ? c.why : "?"));
        return;
    }
    // the rooms of another lot (the active lot changed) go first
    {
        std::lock_guard<std::mutex> lk(g_keptMx);
        for (auto it = g_kept.begin(); it != g_kept.end();)
            if (it->second->lot != r.lot) g_keptBytes -= std::min(g_keptBytes, it->second->bytes), it = g_kept.erase(it);
            else ++it;
        for (auto it = g_phaseKept.begin(); it != g_phaseKept.end();) // (the cache too: a lot change)
            if ((it->second.at[0] && it->second.at[0]->lot != r.lot) || (it->second.at[1] && it->second.at[1]->lot != r.lot)) it = g_phaseKept.erase(it);
            else ++it;
    }
    const size_t n = r.texels.size();
    auto k = std::make_shared<KeptRoom>();
    k->room = reinterpret_cast<uintptr_t>(room), k->mgr = rr.mgr, k->lot = r.lot, k->story = rr.story, k->id = rr.id, k->cls = rr.cls, k->thr = r.thr;
    for (int m = 0; m < 4; m++) k->mapW[m] = r.maps[m].used ? r.maps[m].w : 0, k->mapH[m] = r.maps[m].used ? r.maps[m].h : 0;
    long parts = 0, noB = 0;
    try {
        k->base.resize(n * 3);
        for (size_t i = 0; i < n; i++)
            for (int q = 0; q < 3; q++) k->base[i * 3 + q] = r.texels[i].t.S[q];
        std::unordered_map<uintptr_t, const LampRec*> got;
        for (const LampRec& l : r.lamps) got[l.light] = &l;
        std::unordered_map<uintptr_t, std::vector<const Capture::ProbeHook*>> hooks;
        for (const Capture::ProbeHook& h : c.probeHook)
            if (h.texel < n) hooks[h.light].push_back(&h);
        std::vector<float> gotSum(n, 0.0f);
        for (const auto& [light, hs] : hooks) {
            KeptLamp kl;
            int reg = -1;
            // (not drawn: its light stays in the base; physical: so does a light whose law is its colour alone, it has no place)
            if (!LawOfLight(light, r.lot, kl, &reg) || (c.phys && kl.colourOnly)) {
                k->baked.push_back(light);
                continue;
            }
            if (const auto sp = std::find_if(c.startPos.begin(), c.startPos.end(), [&](const auto& p) { return p.first == light; }); sp != c.startPos.end())
                std::copy_n(sp->second.data(), 3, kl.pos); // (the law stays the live one: only the carries' anchor)
            const LampRec* lr = got.count(light) ? got[light] : nullptr;
            if (c.phys) {
                int evals = 1;
                for (const Capture::ProbeHook* h : hs) evals = std::max(evals, static_cast<int>(h->evals));
                kl.mult = static_cast<uint8_t>(std::clamp(lr ? lr->mult : evals, 1, 255));
                kl.normK = NormKOf(room, rr.story, rr.id, rr.outdoor, light, reg);
                kl.reach = LevelLightShare::ReachAppliesIn(room, light);
                kl.home = reg;
            }
            if (lr)
                for (const Contrib& p : lr->parts)
                    if (p.texel < n)
                        for (int q = 0; q < 3; q++) k->base[static_cast<size_t>(p.texel) * 3 + q] -= p.rgb[q];
            if (!c.phys) parts += KeptPartsOf(kl, hs, lr, r.samples.data(), n, gotSum, noB); // (physical: listed with its law, no part)
            k->lamps.push_back(std::move(kl));
        }
        // (stage 2a) the tail, when the solve's end saw every texel's pack input and final texel
        k->tailWhy = c.tailWhy ? c.tailWhy : !c.finals ? "the solve's end was not seen" : c.tail.size() != n ? "a texel without its tail" : nullptr;
        if (!k->tailWhy) {
            k->tail = std::move(c.tail), k->copies = std::move(c.copies), k->blocks = std::move(r.blocks), k->preblur = std::move(r.preblur);
            k->passes = r.passes, k->wallsOk = r.wallsOk, k->roofed = c.roofed, k->killed = c.killed, k->toneK = c.toneK;
            std::memcpy(k->amb, c.amb, sizeof k->amb), std::memcpy(k->ambPass, c.ambPass, sizeof k->ambPass);
            ShrinkExact(k->tail), ShrinkExact(k->copies), ShrinkExact(k->blocks), ShrinkExact(k->preblur);
        }
        // the capture's texels and samples move into the kept room (the capture is dropped next)
        BuildBasis(c, *k); // (stage 4; after `baked`)
        k->batches = r.batches;
        k->physical = c.phys;
        k->outdoor = rr.outdoor;
        if (!CopyBytes(&k->norm, room + 0x160, sizeof k->norm) || !std::isfinite(k->norm)) k->norm = 1.0f;
        if (c.phys) { // (its blocker table once, shared by its maps' entries: in its bytes)
            k->tests = std::move(c.tests);
            ObjectLegs::Fill(k->tests);
            k->blk = DrawnTableOf(k->tests.rec, k->blkHeader);
        }
        k->samples = std::move(r.samples);
        k->texels = std::move(r.texels);
        ShrinkExact(k->samples), ShrinkExact(k->texels);
        // (stage 2b) the maps it wrote, and whether the GPU draws every texel of them (a lamp edit can then skip its solve): no
        // class-2 wall texel outside the blocks Apex blurs, no wall the game's own blur did
        for (const TexelRec& t : k->texels) k->maps |= 1u << (t.map & 3);
        k->editable = !k->tailWhy;
        if (k->editable && k->cls == 2 && (k->maps & 1)) {
            std::unordered_set<uint32_t> blurred;
            for (const BlockRec& b : k->blocks)
                if (b.have)
                    for (int y = b.y0; y <= b.y1; y++)
                        for (int x = b.x0; x <= b.x1; x++) blurred.insert(static_cast<uint32_t>(y) << 16 | static_cast<uint32_t>(x));
            k->editable = k->passes && k->wallsOk &&
                          std::all_of(k->texels.begin(), k->texels.end(), [&](const TexelRec& t) { return t.map != 0 || blurred.count(static_cast<uint32_t>(t.y) << 16 | t.x); });
        }
    } catch (...) {
        if (KeptLog()) LOG_INFO(std::format("[GpuRoomLamps] Story {} room {} class {}: not kept (no memory)", rr.story, rr.id, rr.cls));
        return;
    }
    k->geo = r.geo, k->roomGeo = r.roomGeo;
    std::copy_n(r.geoParts, 3, k->geoParts);
    const uint64_t key = KeyOf(room, rr.cls);
    k->version = g_keptVersion.fetch_add(1, std::memory_order_relaxed) + 1;
    k->basisId = k->version;
    if (!k->physical) HookGuard::Run("OutdoorDelta carry switched-off lamps", [&] { CarryOff(*k, key); }); // (physical: no parts to carry)
    k->bytes = KeptBytes(*k);
    k->keptAt = GetTickCount();
    k->start = c.phaseStart;
    if (k->physical && !k->tailWhy) {
        std::memcpy(k->ambPair, c.ambPair, sizeof k->ambPair);
        bool shared = false;
        k->ambOk = UnlitRooms::SolveBase(room, c.ambPair, k->ambCe, k->ambOwn, shared);
        k->ambLls = k->ambOk && LevelLightShare::AmbientOwnIs(const_cast<unsigned char*>(reinterpret_cast<const unsigned char*>(room)), k->ambOwn, k->ambNorm);
        // (another own colour than its solve's: cached only as a stairwell's share over it, which the swap puts back as shown)
        if (shared) k->ambOk = k->ambLls;
    }
    // a candidate of the day/night cache: a physical solve drawn whole (every texel on the GPU), made wholly at one end
    const PhaseCache::Stamp endStamp = PhaseCache::StampOf(ClockNow(), k->keptAt);
    k->keptStamp = endStamp;
    if (k->physical)
        for (const uintptr_t light : k->baked)
            if (KeptLamp kl; LawOfLight(light, k->lot, kl)) k->bakedLaws.push_back(kl); // (window lights have none)
    const bool phaseCand = k->physical && k->editable && !k->tailWhy && k->ambOk && PhaseCache::Whole(k->start, endStamp);
    const auto mb = [](size_t b) { return static_cast<double>(b) / (1024.0 * 1024.0); };
    std::shared_ptr<const KeptRoom> kept, prev;
    size_t rooms = 0, total = 0;
    {
        std::lock_guard<std::mutex> lk(g_keptMx);
        const auto old = g_kept.find(key);
        if (old != g_kept.end()) prev = old->second;
        const size_t oldBytes = old != g_kept.end() ? old->second->bytes : 0;
        const size_t after = g_keptBytes - std::min(g_keptBytes, oldBytes) + k->bytes;
        // (the day/night cache's other end gives way to any kept solve: it only saves a wait)
        if (!g_phaseKept.empty() && after + PhaseExtraBytes() > kMaxKeptBytes) {
            if (KeptLog()) LOG_INFO(std::format("[GpuRoomLamps] Day/night cache dropped ({} rooms): the {:.0f} MB budget", g_phaseKept.size(), mb(kMaxKeptBytes)));
            g_phaseKept.clear();
        }
        if (after > kMaxKeptBytes) { // (the room's earlier kept solve, if any, goes: it no longer holds what its maps hold)
            if (old != g_kept.end()) g_keptBytes -= std::min(g_keptBytes, oldBytes), g_kept.erase(old);
            if (!g_keptFull && KeptLog())
                LOG_INFO(std::format("[GpuRoomLamps] Story {} room {} class {}: not kept (the {:.0f} MB budget: {:.1f} MB kept in {} rooms, this one {:.1f} MB)", rr.story, rr.id,
                                     rr.cls, mb(kMaxKeptBytes), mb(g_keptBytes), g_kept.size(), mb(k->bytes)));
            if (g_keptRefused.size() < 1024) g_keptRefused[key] = k->bytes;
            g_keptFull = true;
            return;
        }
        g_keptFull = false;
        g_keptBytes = after;
        g_keptRefused.erase(key);
        kept = g_kept[key] = std::move(k);
        rooms = g_kept.size(), total = g_keptBytes;
        if (phaseCand) {
            auto& slot = g_phaseKept[key];
            slot.at[endStamp.heading] = kept, slot.keptAt[endStamp.heading] = kept->keptAt, slot.start[endStamp.heading] = kept->start.tick;
        }
    }
    NotePhaseLanded(key, *kept);
    if (kept->physical) { // (no parts: nothing to recompose, so no self-check)
        NotePhysBasisKept(key, prev.get(), *kept);
        if (kept->basisSet) CarrySiblings(key, kept->basisSet);
        // (its light list is the solve's now: its rigs take it at once after a lamp edit, not at the next 5 s sweep)
        if (const DWORD edit = g_lastPhysEdit.load(std::memory_order_relaxed); edit && GetTickCount() - edit < 10000) {
            ObjectLightBridge::RegatherRigsOfRoom(RtOutdoorLamps::ActiveTracker(), kept->story, kept->id);
            ReflectionDue(room); // (and metal: not 2 s from now)
        }
        HookGuard::Run("OutdoorDelta scene recording", [&] { SceneWriteRoom(*kept, r); });
        if (KeptLog())
            LOG_INFO(std::format("[GpuRoomLamps] Story {} room {} class {}: kept {} texels (physical: no parts), {:.1f} MB; {} rooms, {:.1f} MB of {:.0f}", kept->story,
                                 kept->id, kept->cls, kept->texels.size(), mb(kept->bytes), rooms, mb(total), mb(kMaxKeptBytes)));
        return;
    }
    if (KeptLog())
        LOG_INFO(std::format("[GpuRoomLamps] Story {} room {} class {}: kept {} texels, {} drawn lamps, {} parts ({} never tested by the game), {:.1f} MB; {} rooms kept, "
                             "{:.1f} MB of {:.0f} MB",
                             kept->story, kept->id, kept->cls, kept->texels.size(), kept->lamps.size(), parts, noB, mb(kept->bytes), rooms, mb(total), mb(kMaxKeptBytes)));
    if (g_probe.load(std::memory_order_relaxed)) KeptSelfCheck(*kept);
}
} // namespace

void SolveStart(unsigned char* room) {
    if (g_clear.exchange(false)) DropCapture();
    if (g_keptClear.exchange(false)) ClearKept(); // (the capture switched off)
    if (g_cap.active) DropCapture(); // the solve it kept was abandoned (restarted, or another room's)
    if (!g_on.load(std::memory_order_relaxed) || !room) return;
    RoomRead rr;
    if (!ReadRoom(room, rr)) return;
    if (g_physical.load(std::memory_order_relaxed) && g_capture.load(std::memory_order_relaxed)) { // (stage 4: the summary's solves)
        const DWORD now = GetTickCount(), last = g_lastPhysEdit.load(std::memory_order_relaxed);
        const auto sent = g_physSent.find(room);
        if (sent != g_physSent.end() && now - sent->second < 10000) c_physSettleSolves.fetch_add(1, std::memory_order_relaxed), g_physSent.erase(sent);
        else if (last && now - last < kEditSolveMs && RtOutdoorLamps::TrackerOf(room)) c_physEditSolves.fetch_add(1, std::memory_order_relaxed);
        if (g_physSent.size() > 256) g_physSent.clear();
    }
    if (rr.id != 0) { // another room of a lot with a lamp edit going on: its solve time (the log)
        const DWORD now = GetTickCount();
        {
            std::lock_guard<std::mutex> lk(g_sideMx);
            if (g_lotEdits.count(rr.lot) && g_otherStart.size() < 512) g_otherStart[room] = {rr.lot, now};
        }
        if ((g_probe.load(std::memory_order_relaxed) || g_capture.load(std::memory_order_relaxed)) && rr.cls >= 0 && rr.cls <= 2 && rr.lot == ActiveLot()) ProbeStart(room, rr);
        return;
    }
    if (!rr.outdoor || rr.cls < 0 || rr.cls > 2) return;
    // only the active lot's room 0s keep their parts (every lot's took 206 MB; edits elsewhere solve in full); a lot next to
    // it keeps Real-time outdoor lamps' base: the texels and their sums, no light's part
    const bool baseOnly = rr.lot != ActiveLot();
    if (baseOnly && !NeighbourLot(rr.lot)) return;
    // the conditions a delta needs, checked again at its lock step
    const char* refuse = !LevelLightShare::DeltaReady() ? "the room solve hooks are not in"
                         : RoomLightQueue::QuickPassTarget(room) > 0  ? "a quick pass"
                         : !LevelLightShare::ListAllWrapped(room)     ? "a light whose evaluation is not wrapped"
                                                                      : nullptr;
    if (refuse && baseOnly) return EraseStale(room, rr.cls); // (quietly: the drop readout and its log lines are the active lot's)
    if (refuse) {
        if (refuse[0] == 'a' && std::strcmp(refuse, "a light whose evaluation is not wrapped") == 0) {
            static std::atomic<int> s_lines{0};
            if (s_lines.fetch_add(1) < 20)
                LOG_INFO(std::format("[OutdoorDelta] Story {} room 0 class {}: the light whose evaluation is not wrapped: {}", rr.story, rr.cls,
                                     LevelLightShare::FirstUnwrappedLight(room)));
        }
        LogDrop(rr.story, rr.cls, "its parts are not kept from this solve", refuse);
        EraseStale(room, rr.cls);
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
    r.baseOnly = baseOnly;
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
    // GPU room lamps (developer): the lot lamps of its list Real-time outdoor lamps does not draw (indoor lamps through openings)
    // are captured as a kept room's, so that the GPU can draw them from their parts (BuildKept0)
    // (the room lamp probe: the same lights, recorded and dumped at the solve's end; with it alone nothing is kept)
    if ((g_capture.load(std::memory_order_relaxed) || g_probe.load(std::memory_order_relaxed)) && !baseOnly)
        for (const ListLight& l : c.startList) {
            KeptLamp kl;
            if (!DrawnLight(l.light, rr.lot) && LawOfLight(l.light, rr.lot, kl)) c.keep0Lights.insert(l.light);
        }
    // (physical indoor lamps: no kept0; ExportRoom0(physical) takes every law lamp out of the base instead, drawn over room 0's 3D
    // test records as the solve starts: UNVERIFIED that room 0's records are the outdoor objects its batches test)
    c.phys = g_capture.load(std::memory_order_relaxed) && g_physical.load(std::memory_order_relaxed) && !baseOnly;
    r.phys = c.phys;
    uint64_t geoParts[3] = {};
    r.roomGeo = c.phys && RoomGeoKey(room, geoParts) ? PhysLampPick::BlockersKey(geoParts) : 0;
    r.blk.reset(), r.blkHeader = -1.0f;
    if (c.phys) {
        RoomLightTests::Room t;
        std::string why;
        if (ReadRoomTests(room, t, why, true)) {
            ObjectLegs::Fill(t);
            r.blk = DrawnTableOf(t.rec, r.blkHeader);
        }
        else if (KeptLog()) LOG_INFO(std::format("[GpuRoomLamps] Story {} room 0 class {}: its 3D test records could not be read ({}): its lamps drawn without them", rr.story, rr.cls, why));
    }
    c.keep0 = g_capture.load(std::memory_order_relaxed) && !c.phys && !c.keep0Lights.empty();
    c.probe0 = g_probe.load(std::memory_order_relaxed) && !baseOnly;
    // a self-check: the full solve's texels are compared with what the delta wrote
    bool check = false;
    {
        std::lock_guard<std::mutex> lk(g_sideMx);
        check = g_checkPending.erase(room) > 0;
    }
    if (check && !baseOnly) {
        std::lock_guard<std::recursive_mutex> lk(g_mx);
        const auto it = g_records.find(KeyOf(room, rr.cls));
        if (it != g_records.end() && !it->second->stale) {
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
    else if (it == g_records.end() || it->second->stale) why = "no kept parts for this room and class yet";
    // (a lot that was active and is now next to the active one: its full record is not the base this solve keeps; a delta on it
    // would end the capture and leave the GPU with no base for that story)
    else if (g_cap.active && g_cap.room == room && g_cap.rec && g_cap.rec->baseOnly && !it->second->baseOnly) why = kWhyBaseOnly;
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
            if (d == Delta::NoChange) {
                why = "no light of its list changed";
                // its kept solve written back instead of the game's full solve: a camera pan to the next lot and back makes
                // the game free and recreate this story's maps filled with 0 and solve room 0 again with nothing changed (8 such
                // solves of about 3.5 s in a recording). Every kept texel is written in the solve's order (the last write of a texel
                // two samples share stays), the class-2 walls through their blocks' blur as a delta does; a wall block that cannot
                // be blurred again leaves the full solve. The record and its version are left as they are
                Plan rp;
                rp.dirtyBlock.assign(r.blocks.size(), 0);
                bool ok = true;
                for (size_t i = 0; i < r.texels.size() && ok; i++) {
                    const TexelRec& t = r.texels[i];
                    if (t.map == 0 && r.cls == 2) {
                        if (r.passes <= 0 || t.block >= r.blocks.size() || !r.blocks[t.block].ok) ok = false;
                        else rp.dirtyBlock[t.block] = 1;
                    } else
                        rp.writes.emplace_back(static_cast<uint32_t>(i), t.packed);
                    rp.texels++;
                }
                if (!ok) why = "no light of its list changed; a wall block cannot be blurred again";
                else if ((done = ApplyDelta(room, r, rp, why))) plan = std::move(rp), why = nullptr;
            } else if (d == Delta::Ok) done = ApplyDelta(room, r, plan, why);
        }
        if (done) UpdateBytes(r);
    }
    const double ms = MsSince(t0);
    if (!done) {
        // a full solve rewrites the maps: what was kept of this room and class is no longer what they hold
        const bool had = it != g_records.end(), base = had && it->second->baseOnly;
        // (a lamp-only refusal: its base stays the GPU's until the solve's own parts replace it, so the stories do not wait; a
        // base-only record, while this solve is kept in its place)
        if (had && (it->second->stale || (why && (std::strcmp(why, kWhyEditBatches) == 0 || std::strcmp(why, "too many lights changed") == 0 ||
                                                  (std::strcmp(why, kWhyBaseOnly) == 0 && g_cap.active && g_cap.room == room)))))
            it->second->stale = true;
        else EraseLocked(key);
        lk.unlock();
        c_full.fetch_add(1, std::memory_order_relaxed);
        NoteReason(why ? why : "?");
        {
            std::lock_guard<std::mutex> sk(g_sideMx);
            if (g_fullWhy.size() > 256) g_fullWhy.clear();
            g_fullWhy[room] = why ? why : "?";
        }
        const unsigned long edit = LampMarkFilter::EditLastTick();
        if ((had && !base) || (edit && GetTickCount() - edit < kEditWindowMs))
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
    // (physical) Apex's last directional writes go back right after the finalize's unlock copied the game's levels over them (the
    // room's own carry, its siblings'), after DropHeld below: from BasisDuePhys alone they waited for the next room update with
    // the maps free, and furniture showed the game's level for frames, A-B-A-B at the settle
    struct CarryNow {
        ~CarryNow() {
            if (!g_capture.load(std::memory_order_relaxed) || !g_physical.load(std::memory_order_relaxed) || !LevelLightShare::RoomMapsFree()) return;
            try { // (a destructor: an exception here would end the game; BasisDuePhys writes the carry instead)
                WritePhysCarry();
            } catch (...) {
                HookGuard::Note("OutdoorDelta carry at solve end");
            }
        }
    } carryNow;
    // (physical, stage 4) the kept copies RoomFinalized held for this solve: gone once it ends unless its KeepEnd replaced them
    struct DropHeld {
        ~DropHeld() {
            if (g_heldKept.empty()) return;
            std::lock_guard<std::mutex> lk(g_keptMx);
            for (const auto& [key, version] : g_heldKept)
                if (const auto it = g_kept.find(key); it != g_kept.end() && it->second->version == version)
                    g_keptBytes -= std::min(g_keptBytes, it->second->bytes), g_kept.erase(it);
            g_heldKept.clear();
        }
    } dropHeld;
    // (physical) any solve of the story, room 0 and unkept ones too, copied the class's working set over its directional maps
    if (g_capture.load(std::memory_order_relaxed) && g_physical.load(std::memory_order_relaxed))
        if (const uintptr_t set = RoomSet(reinterpret_cast<uintptr_t>(room))) {
            RoomRead rr;
            CarrySiblings(ReadRoom(room, rr) && rr.cls >= 0 ? KeyOf(room, rr.cls) : 0, set);
        }
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
    if (c.probe) {
        if (g_probe.load(std::memory_order_relaxed)) ProbeEnd(room);
        if (c.keep) KeepEnd(room); // (after the probe: it takes the capture's texels)
        return DropCapture();
    }
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
                r.lamps.back().drawn = !r.baseOnly && DrawnLight(list[i].light, r.lot); // (the GPU draws none of a base-only room's lights)
                r.lamps.back().collapsed = r.baseOnly;
                r.lamps.back().exit = LevelLightShare::ExitLampOf(room, list[i].light);
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
        // (one reason each: a lamp drag lost its kept parts here and the joint reason could not say which key moved)
        if (c.ok && LevelLightShare::LotGeometryKey(room) != r.geo) Fail("walls, floors or openings changed during the solve");
        if (c.ok && LevelLightShare::PhysicsKey() != r.physics) Fail("the light rules (PhysicsKey) changed during the solve");
        if (c.ok && r.gen != g_gen.load()) Fail("the lighting options changed during the solve");
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
        // texels two samples write (rewritten in the solve's order by a delta). A base-only record (a neighbour lot) never takes a
        // delta nor a self-check: neither this nor the batches' spheres below, O(texels) on the light thread at each solve's end
        if (!r.baseOnly) {
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
        for (BatchRec& b : r.baseOnly ? std::span<BatchRec>{} : std::span<BatchRec>{r.batches}) {
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
        // Every light of a texel's sum must be in a light's part: the base Real-time outdoor lamps relights from is the sum less
        // the drawn lamps' parts, so light the parts miss stays in it whatever the GPU draws (a courtyard kept a recoloured,
        // then switched-off sconce's old green until Restart lighting, F7 with every lamp of the lot off). A solve whose sum
        // passes its parts by more than 2 levels at a texel is not kept (the room's edits take the game's solve). Once per
        // kept solve: one pass over the parts. (A base-only record has none: its base is the whole sum.)
        if (c.ok && !r.baseOnly) {
            std::vector<float> got(r.texels.size() * 3, 0.0f);
            for (const LampRec& l : r.lamps)
                for (const Contrib& p : l.parts)
                    if (p.texel < r.texels.size())
                        for (int k = 0; k < 3; k++) got[static_cast<size_t>(p.texel) * 3 + k] += p.rgb[k];
            size_t over = 0, worstAt = 0;
            float worst = 0.0f;
            for (size_t i = 0; i < r.texels.size(); i++) {
                const float* S = r.texels[i].t.S;
                const float d = std::max({S[0] - got[i * 3], S[1] - got[i * 3 + 1], S[2] - got[i * 3 + 2]});
                if (d > worst) worst = d, worstAt = i;
                if (d > 2.0f / 255.0f) over++;
            }
            if (!kPublicBuild) {
                std::string drawn;
                for (const LampRec& l : r.lamps) {
                    if (!l.drawn) continue;
                    float lum = 0.0f;
                    for (const Contrib& p : l.parts) lum += Lum3(p.rgb);
                    drawn += std::format(" {:08X} x{} {} parts lum {:.2f};", l.light, l.mult, l.parts.size(), lum);
                }
                LOG_INFO(std::format("[OutdoorDelta] Kept solve of story {} room 0 class {}: {} texels, sum past its parts at {} (largest {:.3f} at texel "
                                     "{},{} map {}); drawn lamps:{}",
                                     r.story, r.cls, r.texels.size(), over, worst, r.texels.empty() ? 0 : r.texels[worstAt].x,
                                     r.texels.empty() ? 0 : r.texels[worstAt].y, r.texels.empty() ? 0 : r.texels[worstAt].map, drawn.empty() ? " none" : drawn));
            }
            if (over) Fail("light in a texel's sum that no light's part holds");
        }
    }
    // (the room lamp probe) room 0's lamps carried outdoors, its floor mask and exit ground parts and its whole light list, dumped
    if (c.probe0 && g_probe.load(std::memory_order_relaxed)) HookGuard::Run("RoomLampProbe room 0", [&] { ProbeEnd(room); });
    if (c.ok) {
        Record& r = *c.rec;
        r.used = GetTickCount();
        r.version = g_version.fetch_add(1, std::memory_order_relaxed) + 1;
        r.baseVersion = r.version;
        if (c.keep0) HookGuard::Run("OutdoorDelta room 0 kept lamps", [&] { BuildKept0(c, r); });
        if (c.phys) // (the solve's own rules, read while its room info stands)
            HookGuard::Run("OutdoorDelta room 0 physical lamps", [&] {
                for (LampRec& l : r.lamps) {
                    KeptLamp kl;
                    int reg = -1;
                    if (l.drawn || !LawOfLight(l.light, r.lot, kl, &reg)) continue;
                    l.normK = NormKOf(room, r.story, 0, true, l.light, reg);
                    l.reach = LevelLightShare::ReachAppliesIn(room, l.light);
                }
            });
        if (RtOutdoorLamps::Watching()) r.baseHash = BaseHash(r);
        ShrinkRecord(r); // (outside g_mx: the copies take a while on a large room)
        const uint64_t key = KeyOf(room, r.cls);
        const int story = r.story, cls = r.cls;
        const bool baseOnly = r.baseOnly;
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
            // stay over the ceiling (with no pinned room the check never refuses: a record is at most kMaxRecordBytes). A base-only
            // record is neither counted nor refused here: the loop below drops it first, the new one too
            size_t pinnedBytes = 0;
            for (const auto& [k2, rec] : g_records)
                if (PinnedLocked(rec->room) && !rec->baseOnly) pinnedBytes += rec->bytes;
            const bool refused = !baseOnly && pinnedBytes + r.bytes > kMaxPinnedBytes;
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
            // the oldest rooms go past the budget: never a pinned one (the active lot's room 0s, bounded above). The base-only
            // records go first, and only they make room for a new one (it goes last of them): the other records are as without them
            while (g_bytes.load() > kMaxBytes && g_records.size() > 1) {
                auto oldest = g_records.end();
                for (auto it = g_records.begin(); it != g_records.end(); ++it)
                    if ((it->second->baseOnly || (!baseOnly && it->first != key && !PinnedLocked(it->second->room))) &&
                        (oldest == g_records.end() || (it->second->baseOnly != oldest->second->baseOnly ? it->second->baseOnly
                                                                                                        : static_cast<int32_t>(it->second->used - oldest->second->used) < 0)))
                        oldest = it;
                if (oldest == g_records.end()) break;
                LOG_INFO(std::format("[OutdoorDelta] Story {} room 0 class {}: kept parts dropped (memory budget: {:.1f} MB of {:.1f} MB kept in {} rooms, {} MB allowed)",
                                     oldest->second->story, oldest->second->cls, static_cast<double>(oldest->second->bytes) / (1 << 20),
                                     static_cast<double>(g_bytes.load()) / (1 << 20), g_records.size(), kMaxBytes >> 20));
                NoteReason("memory budget");
                c_budgetDrops.fetch_add(1, std::memory_order_relaxed);
                EraseLocked(oldest->first);
            }
            if (baseOnly && g_records.count(key) && LogAllowed())
                LOG_INFO(std::format("[OutdoorDelta] Story {} room 0 class {} of a lot next to the active one: kept as Real-time outdoor lamps' base only ({} texels, {:.1f} MB)",
                                     story, cls, r.texels.size(), static_cast<double>(r.bytes) / (1 << 20)));
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
        if (unsure && !baseOnly && LogAllowed())
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
        if (ReadRoom(room, rr)) {
            if (!c.rec->baseOnly) LogDrop(rr.story, rr.cls, "its parts are not kept from this solve", c.why ? c.why : "?"); // (the active lot's readout)
            EraseStale(room, rr.cls);
        }
        if (c.check) LOG_INFO(std::format("[OutdoorDelta] Self-check of a room 0: its full solve was not kept ({}), not compared", c.why ? c.why : "?"));
    }
    DropCapture();
}

bool DeltaSolving(const void* room) { return room && g_deltaRoom.load(std::memory_order_relaxed) == room; }
void NoteDirectional(const void* room) {
    if (g_cap.active && g_cap.room == room) g_cap.directional = true;
}
namespace {
constexpr size_t kMaxBasisSamples = 8192, kMaxBasisParts = 65536; // per kept solve
// The sample FUN_006a09f0 passes `pos` for (its sample 0.5 m up, 0x006A0C2F): its texel (+0x20, +0x22). POD (SEH)
bool FindBasisTexel(uintptr_t vec, const float* pos, uint16_t& x, uint16_t& y, uint32_t& hint) {
    __try {
        const uintptr_t b = *reinterpret_cast<const uintptr_t*>(vec), e = *reinterpret_cast<const uintptr_t*>(vec + 4);
        if (!b || e <= b || (e - b) % 0x30 || (e - b) / 0x30 > 65536) return false;
        const uint32_t n = static_cast<uint32_t>((e - b) / 0x30);
        for (uint32_t k = 0; k < n; k++) {
            const uint32_t i = (hint + k) % n;
            const uintptr_t s = b + static_cast<uintptr_t>(i) * 0x30;
            const float* f = reinterpret_cast<const float*>(s);
            if (f[0] == pos[0] && f[2] == pos[2] && std::fabs(pos[1] - f[1] - 0.5f) < 1e-3f) {
                x = *reinterpret_cast<const uint16_t*>(s + 0x20), y = *reinterpret_cast<const uint16_t*>(s + 0x22);
                hint = i + 1;
                return true;
            }
        }
        return false;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
} // namespace
bool KeepsBasis(const void* room) { return g_cap.active && g_cap.ok && g_cap.keep && g_cap.room == room && !g_cap.basisWhy; }
void NoteBasis(const void* room, const float* pos, uintptr_t light, const float* before, const float* after, const float* step) {
    if (!KeepsBasis(room) || !pos || !light || !before || !after || !step) return;
    Capture& c = g_cap;
    const auto fail = [&c](const char* why) {
        c.basisWhy = why;
        std::vector<Capture::BasisSample>().swap(c.basis);
        std::vector<Capture::BasisPart>().swap(c.basisParts);
    };
    try {
        if (c.basis.empty() || std::memcmp(c.basis.back().pos, pos, sizeof c.basis.back().pos) != 0) { // (the next sample: every light of the list in turn)
            if (c.basis.size() >= kMaxBasisSamples) return fail("too many directional samples");
            const uintptr_t vec = LevelLightShare::BasisSamplesVec();
            Capture::BasisSample s;
            std::memcpy(s.pos, pos, sizeof s.pos);
            if (!vec || !FindBasisTexel(vec, pos, s.x, s.y, c.basisHint)) return fail("a directional sample's texel was not found");
            c.basis.push_back(s);
        }
        const uint32_t si = static_cast<uint32_t>(c.basis.size() - 1);
        Capture::BasisPart* p = nullptr;
        for (size_t j = c.basisParts.size(); j-- > 0 && c.basisParts[j].sample == si;)
            if (c.basisParts[j].light == light) p = &c.basisParts[j];
        if (!p) {
            if (c.basisParts.size() >= kMaxBasisParts) return fail("too many directional parts");
            p = &c.basisParts.emplace_back();
            p->sample = si, p->light = light;
        }
        for (int i = 0; i < 16; i++) {
            const float d = after[i] - before[i];
            p->c[i] += std::isfinite(d) ? d : 0.0f;
            p->step[i] += std::isfinite(step[i]) ? step[i] : 0.0f;
        }
        p->mult = static_cast<uint8_t>(std::min(p->mult + 1, 255));
    } catch (...) {
        fail("no memory");
    }
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
    s.exitGround = false;
    s.visLum = s.allLum = 0.0f;
    return true;
}
// (physical: room 0's exit ground is always drawn from its lamps, ExportRoom0; without the marks, outside a probe, no indoor
// lamp lit the lot ground through an opening)
bool KeepsGround() { return g_cap.active && g_cap.ok && (g_cap.keep0 || g_cap.probe0 || g_cap.phys); }
void ExitGround(bool on) {
    if (g_s.on) g_s.exitGround = on;
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
    a = Acc{light, {0.0f, 0.0f, 0.0f}, 0.0f, 0.0f, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 0.0f}, 1.0f, 0, {0.0f, 0.0f, 0.0f}, 0, {0.0f, 0.0f, 0.0f}, 0, 0.0f, 1.0f, 0};
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
    if (!s.on || g_cap.rec->baseOnly) return; // (a base-only capture keeps no light's part: OnEval finds no tap)
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
void OnEvalOut(uintptr_t light, const float* colour) {
    SampleState& s = g_s;
    if (!s.on || s.tap != 0 || !(g_cap.probe || g_cap.keep0 || g_cap.probe0) || !colour) return;
    if (Acc* a = AccFor(light)) {
        for (int i = 0; i < 3; i++) a->hook[i] += colour[i], a->wall[i] += colour[i]; // (as passed until its wall test says otherwise)
        const bool noted = s.pathLight == light; // (no path noted: the threshold dropped it before, or it is not a roofed room's)
        for (int i = 0; i < 3; i++) a->pre[i] += noted ? s.pre[i] : colour[i];
        a->paths |= static_cast<uint8_t>(1u << (noted ? s.path : 0));
        if (s.cur.light == light) { // the evaluation's record (ProbeEvalBegin): its game colour, and the record as the last one
            for (int i = 0; i < 3; i++) a->c0[i] += s.cur.c0[i];
            if (a->evals < 255) a->evals++;
            // (kept captures) the colour before the soft light cut (the colour passed here when the cut did not run)
            a->preSum += (s.cur.has & 8) ? s.cur.preFade : (colour[2] + colour[1]) + colour[0];
            a->thrScale = s.cur.thrScale;
            a->kbits = static_cast<uint8_t>((s.cur.early ? kKeptEarly : 0) | ((s.cur.has & 8) ? kKeptFade : 0));
            s.accEv[a - s.acc] = s.cur;
        }
    }
    s.pathLight = 0;
    s.cur.light = 0;
    for (int i = 0; i < 3; i++) s.last[i] = colour[i];
    s.lastLight = light;
}
void OnWallTest(bool passed, float t) {
    SampleState& s = g_s;
    if (!s.on || s.tap != 0 || !(g_cap.probe || g_cap.keep0 || g_cap.probe0) || !s.lastLight) return;
    const float share = passed ? std::clamp(t, 0.0f, 1.0f) : 0.0f;
    if (Acc* a = AccFor(s.lastLight))
        for (int i = 0; i < 3; i++) a->wall[i] -= s.last[i] * (1.0f - share);
    s.lastLight = 0;
}
ProbeEval* ProbeEvalBegin(uintptr_t light, const float* c0, int set) {
    SampleState& s = g_s;
    if (!s.on || s.tap != 0 || !(g_cap.probe || g_cap.keep0 || g_cap.probe0) || !c0) return nullptr;
    s.cur = ProbeEval{};
    s.cur.light = light;
    s.cur.set = static_cast<uint8_t>(std::clamp(set, 0, 255));
    for (int i = 0; i < 3; i++) s.cur.c0[i] = c0[i];
    return &s.cur;
}
int ProbeTotalLine(const std::string& body) {
    Capture& c = g_cap;
    if (!g_s.on || !c.probe || !c.ok || c.probeTotals.size() >= 20000) return -1; // (a solve's fills: one a batch and place)
    try {
        c.probeTotals.push_back(std::format("TF {} {}\n", c.probeTotals.size(), body));
    } catch (...) {
        return -1;
    }
    return static_cast<int>(c.probeTotals.size()) - 1;
}
void OnEvalPath(uintptr_t light, int path, const float* colour) {
    SampleState& s = g_s;
    if (!s.on || s.tap != 0 || !(g_cap.probe || g_cap.keep0 || g_cap.probe0) || !colour) return;
    s.pathLight = light, s.path = std::clamp(path, 0, 7);
    for (int i = 0; i < 3; i++) s.pre[i] = colour[i];
}
void OnWallTestRaw(const float* from, bool ok, float t) {
    SampleState& s = g_s;
    if (!s.on || s.tap != 0 || !(g_cap.probe || g_cap.keep0 || g_cap.probe0) || !s.lastLight || !from) return;
    if (Acc* a = AccFor(s.lastLight)) {
        for (int i = 0; i < 3; i++) a->from[i] = from[i];
        a->rawT = t, a->raw = ok ? 1 : 2;
    }
}
void OnEval(uintptr_t light) {
    SampleState& s = g_s;
    if (!s.on || s.tap < 0 || !s.sum) return;
    FlushTap();
    s.pending = light;
    for (int i = 0; i < 3; i++) s.snap[i] = s.sum[i];
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
    // the batch: the vector the solve's callers pass (rows lit beyond a wall's edges are another vector: not kept, but in a
    // physical capture of a room > 0, which keeps no parts and redoes no batch, a batch of its own between two batches (a log:
    // 4 rooms of a lot never kept, so every physical edit of that lot was refused). One inside a batch would break
    // that batch's span (its sphere, the dump's batchOf): not kept. Room 0's batches feed the outdoor delta's in-solve redo: strict
    uintptr_t vb = 0, ve = 0;
    const uintptr_t sp = reinterpret_cast<uintptr_t>(sample);
    const bool loose = !ReadBatchVec(vb, ve) || sp < vb || sp >= ve || (sp - vb) % 0x30;
    if (loose && (!c.phys || rr.id == 0 || (!r.batches.empty() && r.texels.size() - r.batches.back().first < r.batches.back().count)))
        return Fail("a sample outside the solve's batch");
    const uint32_t index = loose ? 0 : static_cast<uint32_t>((sp - vb) / 0x30);
    if (index == 0) {
        BatchRec b;
        b.first = static_cast<uint32_t>(r.texels.size());
        b.count = loose ? 1u : static_cast<uint32_t>((ve - vb) / 0x30);
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
    t.bits = static_cast<uint8_t>((s.masked ? kMasked : 0) | (s.gated ? kGated : 0) | (s.exitGround && !s.masked ? kExitGround : 0));
    for (int i = 0; i < 3; i++) t.t.S[i] = out[i];
    t.t.vis = s.masked ? s.visLum : 0.0f;
    t.t.all = s.masked ? s.allLum : 0.0f;
    const uint32_t texel = static_cast<uint32_t>(r.texels.size());
    try {
        r.texels.push_back(t);
        r.samples.insert(r.samples.end(), raw, raw + 0x30);
        if (c.keep) c.tail.emplace_back(); // (NotePack fills it)
        for (int i = 0; i < s.n; i++) {
            const Acc& a = s.acc[i];
            Contrib p;
            p.texel = texel;
            for (int q = 0; q < 3; q++) p.rgb[q] = a.rgb[q];
            p.vis = s.masked ? ToHalf(a.vis) : 0;
            p.all = s.masked ? ToHalf(a.all) : 0;
            if (c.probe || ((c.keep0 || c.probe0) && c.keep0Lights.count(a.light))) {
                const bool lit = a.hook[0] != 0.0f || a.hook[1] != 0.0f || a.hook[2] != 0.0f;
                const ProbeEval* ev = a.evals ? &s.accEv[i] : nullptr;
                const bool apex = ev && (ev->set || ev->cut); // a lamp of another room or story, or one the floor cut stopped
                // (an evaluation the game lit and Apex left dark is kept too, at most kProbeZeroMax a solve)
                const bool zero = !lit && apex && std::max({a.c0[0], a.c0[1], a.c0[2]}) >= 1.0f / 255.0f && c.probeZero < kProbeZeroMax;
                // (kept captures) every evaluation lit before the soft light cut down to kKeepFloor of its threshold, dark or not
                const bool keep = (c.keep || c.keep0) && a.evals && a.preSum / static_cast<float>(a.evals) >= kKeepFloor * r.thr * a.thrScale;
                if (lit || zero || keep) {
                    c.probeZero += zero ? 1 : 0;
                    uint32_t evIdx = UINT32_MAX;
                    if (apex && c.probeEval.size() < kProbeEvalMax) {
                        evIdx = static_cast<uint32_t>(c.probeEval.size());
                        c.probeEval.push_back(*ev);
                    } else if (apex) c.probeEvalDropped++;
                    c.probeHook.push_back(Capture::ProbeHook{texel, a.light, {a.hook[0], a.hook[1], a.hook[2]}, {a.wall[0], a.wall[1], a.wall[2]}, {a.from[0], a.from[1], a.from[2]},
                                                             a.rawT, a.raw, {a.pre[0], a.pre[1], a.pre[2]}, a.paths, {a.c0[0], a.c0[1], a.c0[2]}, evIdx, a.evals,
                                                             a.preSum, a.thrScale, a.kbits});
                }
            }
            if (!Nonzero(p)) continue;
            auto [it, fresh] = c.lampIdx.try_emplace(a.light, static_cast<int>(r.lamps.size()));
            if (fresh) {
                r.lamps.push_back(LampRec{});
                r.lamps.back().light = a.light;
                r.lamps.back().drawn = DrawnLight(a.light, r.lot);
                r.lamps.back().exit = LevelLightShare::ExitLampOf(room, a.light);
            }
            r.lamps[static_cast<size_t>(it->second)].parts.push_back(p);
        }
    } catch (...) {
        return Fail("no memory");
    }
    c.pending = static_cast<int>(texel);
    if (c.keep) t_packRoom = room, t_packTexel = texel;
    if ((texel & 4095) == 0 && BytesOf(r) > kMaxRecordBytes) Fail("a room too large to keep");
}

namespace {
// (stage 4) the directional maps' working set as the finalize's unlock 0x0069E950 is about to copy it into the set [room+8]
// (when room+0x629 is set, 0x0069EA3A): the 4 lock records at room+0x584 + i x 0x28 (FUN_006a09f0's pass +0x540, + 0x44). POD (SEH)
bool ReadBasisDesc(const BYTE* room, uintptr_t& set, Desc d[4]) {
    __try {
        if (!room[0x629]) return false;
        set = *reinterpret_cast<const uintptr_t*>(room + 8);
        for (int i = 0; i < 4; i++) {
            const BYTE* rec = room + 0x584 + i * 0x28;
            d[i].base = *reinterpret_cast<const uintptr_t*>(rec);
            d[i].pitch = *reinterpret_cast<const uint32_t*>(rec + 4);
            d[i].w = *reinterpret_cast<const uint32_t*>(rec + 0x18);
            d[i].h = *reinterpret_cast<const uint32_t*>(rec + 0x1C);
        }
        return set != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
void KeepBasisFinal(const BYTE* room) {
    Capture& c = g_cap;
    if (c.basisWhy || c.basis.empty()) return;
    uintptr_t set = 0;
    Desc d[4];
    if (!ReadBasisDesc(room, set, d)) return void(c.basisWhy = "its directional maps are not locked at the solve's end");
    for (int i = 0; i < 4; i++)
        if (!d[i].base || d[i].w != d[0].w || d[i].h != d[0].h || d[i].pitch < d[i].w * 4 || d[i].w > 512 || d[i].h > 512)
            return void(c.basisWhy = "a directional map is not locked as the others");
    try {
        c.basisGame.assign(c.basis.size() * 4, 0u);
    } catch (...) {
        return void(c.basisWhy = "no memory");
    }
    for (size_t i = 0; i < c.basis.size(); i++)
        for (int m = 0; m < 4; m++)
            if (c.basis[i].x >= d[m].w || c.basis[i].y >= d[m].h ||
                !ReadTexel(d[m].base + static_cast<uintptr_t>(d[m].pitch) * c.basis[i].y + static_cast<uintptr_t>(c.basis[i].x) * 4, c.basisGame[i * 4 + m]))
                return void(c.basisWhy = "a directional texel could not be read at the solve's end");
    c.basisSet = set, c.basisW = d[0].w, c.basisH = d[0].h;
}
} // namespace
void BeforeFinalize(unsigned char* room) {
    if (!g_cap.active || g_cap.room != room) return;
    FlushPending();
    if (g_cap.keep && g_cap.ok) KeepFinals(room);
    if (g_cap.keep && g_cap.ok) KeepBasisFinal(room);
}
namespace {
// (physical indoor lamps, stage 4) the rooms solved off the light tree thread whose kept physical copy stays drawn, sent to one
// light tree thread solve by SettleDue (any thread pushes)
std::mutex g_requeueMx;
std::vector<const void*> g_requeue;
// (stage 4) the active lot's registered lights (LotLamps): the light tree thread writes, the render thread reads
constexpr DWORD kLotLampsMs = 250;
constexpr DWORD kLotGoneMs = 1000; // a lamp missing from the registry this long has left it
std::mutex g_lotLampsMx;
std::map<uintptr_t, uint32_t> g_lotMissing; // (under g_lotLampsMx) PhysLampPick::CarryMissing
std::vector<LotLamp> g_lotLamps;
uint32_t g_lotLampsVer = 0;
std::vector<uintptr_t> g_lotGone;    // (under g_lotLampsMx) PhysLampPick::NoteGone
uintptr_t g_lotLampsTracker = 0;     // (light tree thread) the lot of the last snapshot
DWORD g_lotLampsAt = 0; // (light tree thread)
std::atomic<bool> g_lotLampsDirty{false};
} // namespace
void RefreshLotLamps() {
    if (!g_capture.load(std::memory_order_relaxed) || !g_physical.load(std::memory_order_relaxed)) return;
    const DWORD now = GetTickCount();
    if (!g_lotLampsDirty.exchange(false) && now - g_lotLampsAt < kLotLampsMs) return;
    g_lotLampsAt = now;
    const uintptr_t tracker = RtOutdoorLamps::ActiveTracker();
    // (a lot no longer lit at the high quality unregisters its lamps: 175 "left" at once in a log as the camera
    // moved off it, each rewriting the directional maps; the last snapshot stays until it is active again)
    uintptr_t active[4];
    const int na = std::max(LevelLightShare::ActiveLotTrackers(active, 4), 0);
    std::vector<LevelLightShare::RegisteredLight> reg;
    if (!tracker || std::find(active, active + na, tracker) == active + na || !LevelLightShare::RegisteredLights(tracker, reg)) return; // (the last snapshot stays)
    std::vector<LotLamp> out;
    out.reserve(reg.size());
    for (const LevelLightShare::RegisteredLight& r : reg) {
        LotLamp l;
        l.light = r.light, l.story = r.story, l.home = r.home, l.homeOutdoor = r.home <= 0;
        if (r.home > 0)
            if (const BYTE* home = static_cast<const BYTE*>(LevelLightShare::RoomOf(tracker, r.story, r.home))) {
                BYTE flag = 0;
                if (!CopyBytes(&l.homeNorm, home + 0x160, sizeof l.homeNorm) || !std::isfinite(l.homeNorm)) l.homeNorm = 1.0f;
                if (CopyBytes(&flag, home + 0x18, 1)) l.homeOutdoor = flag != 0;
            }
        out.push_back(l);
    }
    std::lock_guard<std::mutex> lk(g_lotLampsMx);
    // (a lamp of the last snapshot the registry lacks now stays in it for kLotGoneMs: the registry drops a lamp for a few
    // frames while it is dragged and every lamp of the lot while build mode edits it (a log: 709 lamps "left" in 3 min,
    // 175 at once within 50 ms, none deleted), which took them off every map and rewrote the directional maps without them)
    PhysLampPick::CarryMissing(g_lotLamps, out, tracker == g_lotLampsTracker, now, kLotGoneMs, g_lotMissing);
    if (out == g_lotLamps) return;
    // (a lamp deleted: the object removal marks its rooms, 0x006C75A1 for its light and occluder entries, which stay the game's:
    // its room's light list holds the removed light until that room gathers again, and its object's 3D blockers are gone. A kept
    // list holding it no longer draws it from here on)
    std::vector<uintptr_t> before, seen;
    if (tracker == g_lotLampsTracker)
        for (const LotLamp& l : g_lotLamps) before.push_back(l.light);
    else
        g_lotGone.clear();
    for (const LotLamp& l : out) seen.push_back(l.light);
    if (KeptLog())
        for (const uintptr_t l : before)
            if (std::find(seen.begin(), seen.end(), l) == seen.end())
                LOG_INFO(std::format("[GpuRoomLamps] physical: lamp {:08X} left the lot's registry (deleted, or moved to a basement or off the lot): no map draws it now; its rooms "
                                     "take the game's object removal marks",
                                     l));
    for (const uintptr_t l : before)
        if (std::find(seen.begin(), seen.end(), l) == seen.end()) QueueBasisGone(l);
    PhysLampPick::NoteGone(before, seen, g_lotGone);
    g_lotLampsTracker = tracker;
    g_lotLamps.swap(out), g_lotLampsVer++;
}
uint32_t LotLamps(std::vector<LotLamp>& out, std::vector<uintptr_t>* gone) {
    std::lock_guard<std::mutex> lk(g_lotLampsMx);
    out = g_lotLamps;
    if (gone) *gone = g_lotGone;
    return g_lotLampsVer;
}
bool InLotLamps(uintptr_t tracker, uintptr_t light, bool& known) {
    std::lock_guard<std::mutex> lk(g_lotLampsMx);
    known = tracker && tracker == g_lotLampsTracker && g_lotLampsVer != 0;
    return known && std::any_of(g_lotLamps.begin(), g_lotLamps.end(), [&](const LotLamp& l) { return l.light == light; });
}
bool StandsOnFurniture(uintptr_t tracker, int story, int room, uintptr_t light) {
    void* r = tracker && room > 0 ? LevelLightShare::RoomOf(tracker, story, room) : nullptr;
    RoomRead rr;
    if (!r || !ReadRoom(static_cast<const BYTE*>(r), rr)) return false;
    std::shared_ptr<const KeptRoom> k;
    {
        std::lock_guard<std::mutex> lk(g_keptMx);
        if (const auto it = g_kept.find(KeyOf(r, rr.cls)); it != g_kept.end()) k = it->second;
    }
    if (!k || !k->physical) return false;
    const auto kl = std::find_if(k->lamps.begin(), k->lamps.end(), [&](const KeptLamp& l) { return l.light == light; }); // (where it stood at the solve)
    return kl != k->lamps.end() && std::any_of(k->tests.rec.begin(), k->tests.rec.end(), [&](const RoomLightTests::Record& rc) {
               return RoomLightTests::StandsOn(rc, kl->pos);
           });
}
void NoteLightObjectRemoved() {
    if (g_capture.load(std::memory_order_relaxed) && g_physical.load(std::memory_order_relaxed)) g_lotLampsDirty.store(true, std::memory_order_relaxed);
}
namespace {
// (physical) the rooms whose game's maps lack a lamp edit taken without their solve (phys_lamp_pick.h TouchedRooms): noted by
// TakesLampEditPhys and by the maps that drew an edited lamp (NoteTouched), forgotten at the room's next solve's end
// (RoomFinalized); sent to their solve when their maps fall back to the game's (SolveTouched)
std::mutex g_touchedMx;
PhysLampPick::TouchedRooms g_touched;
bool TouchedKey(const void* room, PhysLampPick::TouchedRooms::Key& k) {
    RoomRead rr;
    const uintptr_t tracker = room ? RtOutdoorLamps::TrackerOf(room) : 0;
    if (!tracker || !ReadRoom(static_cast<const BYTE*>(room), rr)) return false;
    k = {tracker, rr.story, rr.id};
    return true;
}
} // namespace
void NoteTouched(const void* room) {
    PhysLampPick::TouchedRooms::Key k;
    if (!TouchedKey(room, k)) return;
    std::lock_guard<std::mutex> lk(g_touchedMx);
    g_touched.Note(k);
}
std::set<PhysLampPick::TouchedRooms::Key> g_unkept; // (under g_touchedMx) SolveUnkept
std::atomic<bool> g_unkeptAny{false};
void NoteUnkept(const void* room) { // (light tree thread, KeepEnd)
    PhysLampPick::TouchedRooms::Key k;
    if (!TouchedKey(room, k)) return;
    std::lock_guard<std::mutex> lk(g_touchedMx);
    if (g_unkept.size() < 1024) g_unkept.insert(k), g_unkeptAny.store(true, std::memory_order_relaxed);
}
bool TraceLine() {
    static std::atomic<DWORD> s_window{0};
    static std::atomic<int> s_n{0};
    if (!Recorder::Verbose() || !g_physical.load(std::memory_order_relaxed)) return false;
    const DWORD now = GetTickCount();
    DWORD w = s_window.load(std::memory_order_relaxed);
    if (now - w > 10000 && s_window.compare_exchange_strong(w, now)) s_n.store(0, std::memory_order_relaxed);
    return s_n.fetch_add(1, std::memory_order_relaxed) < 60;
}
void SolveUnkept() {
    if (!g_unkeptAny.exchange(false, std::memory_order_relaxed)) return;
    std::set<PhysLampPick::TouchedRooms::Key> due;
    {
        std::lock_guard<std::mutex> lk(g_touchedMx);
        due.swap(g_unkept);
    }
    // (a room whose class changes during every solve would be sent again forever: 3 sends a minute at most)
    static std::map<PhysLampPick::TouchedRooms::Key, std::pair<DWORD, int>> s_sent; // (render thread) key -> (window start, sends)
    const DWORD now = GetTickCount();
    if (s_sent.size() > 4096) s_sent.clear();
    for (const auto& [tracker, story, id] : due) {
        auto& [at, n] = s_sent[{tracker, story, id}];
        if (now - at >= 60000) at = now, n = 0;
        if (++n > 3) continue;
        if (LevelLightShare::TrackerLive(tracker))
            if (void* r = id ? LevelLightShare::RoomOf(tracker, story, id) : LevelLightShare::Room0Of(tracker, story)) LevelLightShare::QueueRoomAgain(r);
    }
}
void SolveTouched(const void* room) {
    std::vector<PhysLampPick::TouchedRooms::Key> due;
    PhysLampPick::TouchedRooms::Key k;
    {
        std::lock_guard<std::mutex> lk(g_touchedMx);
        if (g_touched.rooms.empty()) return;
    }
    const bool one = room && TouchedKey(room, k);
    if (room && !one) return;
    {
        std::lock_guard<std::mutex> lk(g_touchedMx);
        if (!one) due = g_touched.TakeAll();
        else if (g_touched.Take(k)) due.push_back(k);
    }
    int sent = 0;
    for (const auto& [tracker, story, id] : due)
        if (LevelLightShare::TrackerLive(tracker))
            if (void* r = id ? LevelLightShare::RoomOf(tracker, story, id) : LevelLightShare::Room0Of(tracker, story)) sent += LevelLightShare::QueueRoomAgain(r) ? 1 : 0;
    if (!due.empty() && KeptLog())
        LOG_INFO(std::format("[GpuRoomLamps] physical: {} rooms shown from the game's maps again with a lamp edit taken since their solve: {} sent to their solve",
                             due.size(), sent));
}
void RoomFinalized(const void* room, bool gather) {
    bool touched = false; // (its maps hold its lamps as they are now)
    {
        std::lock_guard<std::mutex> lk(g_touchedMx);
        touched = !g_touched.rooms.empty();
    }
    if (PhysLampPick::TouchedRooms::Key key; touched && TouchedKey(room, key)) {
        std::lock_guard<std::mutex> lk(g_touchedMx);
        g_touched.Solved(key);
    }
    // (physical, stage 4) the room's kept physical copy stays drawn while its geometry is the same: off the light tree thread
    // until a solve here keeps it again (sent by SettleDue); on it until this solve's KeepEnd replaces it (SolveEnd drops it if
    // none does), so a settle's solve shows no frame of the game's texels between the two
    const bool keeping = !gather || (g_cap.active && g_cap.room == room && g_cap.keep);
    if (keeping && g_physical.load(std::memory_order_relaxed)) {
        uint64_t geo = 0;
        std::vector<std::pair<uint64_t, uint32_t>> held;
        {
            std::lock_guard<std::mutex> lk(g_keptMx);
            for (const auto& [key, k] : g_kept)
                if (k->room == reinterpret_cast<uintptr_t>(room) && k->physical) geo = k->geo, held.emplace_back(key, k->version);
        }
        // (a solve about to run: held whatever its key reads now, as its KeepEnd replaces them or SolveEnd drops them; the key also
        // takes the lot's doors, so a door a Sim opened dropped the room's copy and showed the game's texels until it was kept again)
        if (geo && (gather || RoomGeoKey(static_cast<const BYTE*>(room)) == geo)) {
            if (gather) {
                g_heldKept = std::move(held);
                return;
            }
            std::lock_guard<std::mutex> lk(g_requeueMx);
            if (g_requeue.size() < 256 && std::find(g_requeue.begin(), g_requeue.end(), room) == g_requeue.end()) g_requeue.push_back(room);
            return;
        }
    }
    std::lock_guard<std::mutex> lk(g_keptMx);
    bool first = true;
    for (auto it = g_kept.begin(); it != g_kept.end();)
        if (it->second->room == reinterpret_cast<uintptr_t>(room)) {
            g_keptBytes -= std::min(g_keptBytes, it->second->bytes);
            if (first) g_finalized.clear(), first = false;
            g_finalized.push_back(std::move(it->second)); // (the next kept solve of the room may carry its switched-off lamps: CarryOff)
            it = g_kept.erase(it);
        } else
            ++it;
}
// The settles due (SettleDue, light tree thread): per lamp whose edit was taken, its lot, its rooms (story, id) and the tick of
// its last edit taken (one per lamp: a drag's or a slider's edits coalesce)
constexpr DWORD kSettleMs = 2000;
struct Settle {
    uintptr_t tracker = 0;
    std::vector<std::pair<int, int>> rooms;
    DWORD at = 0;
};
std::unordered_map<uintptr_t, Settle> g_settle;
// (S8, light tree thread) the rooms of the lamp edits taken whose ambient (Rooms at Night's colour and the lamps' share,
// +0x110 / +0x120) is taken again after this room update (AmbientDue): (lot, story, id) -> the edit's kind
std::map<std::tuple<uintptr_t, int, int>, const char*> g_ambDue;
int g_reflectStory = -1; // the story of the last edit's lamp: rooms of other stories take their reflection at their solve
int g_reflectOwn = 0;    // the room that lamp stands in
std::set<std::tuple<uintptr_t, int, int>> g_reflectOwed; // rooms of its story with a step since their last reflection
uintptr_t TlTracker(uintptr_t tl) { // POD (SEH)
    __try {
        return *reinterpret_cast<const uintptr_t*>(tl + 4);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
// ---- GPU room lamps stage 3: a lamp moved within its room (docs/features/night-lighting/gpu-room-lamps.md "Lamp moves") ----
// TakesLampEdit (light tree thread) copies what the game's tests read of each kept room listing the lamp (its walls, default
// wall list and wall mode, its 3D records and their blockers; room_light_tests.h) and queues one job per room; the move
// worker (one thread) recomputes the lamp's parts at every kept texel with room_lamp_move.h (validated offline by
// tools/apex_paths_replay at the probe's recorded positions) and puts in a copy of the kept room with those parts and the
// lamp's law at its new place under a new version, which the GPU draws as any kept room. Until then the GPU draws the room's
// last parts with the law they were kept with (gpu_room_lamps.cpp), never the new law over old parts.
struct MoveJob {
    std::shared_ptr<const KeptRoom> k;
    KeptLamp lamp; // its law at the new place (no parts)
    RoomLightTests::Room tests;
    uint8_t mult = 1, bits = 0;
    uintptr_t tracker = 0;
    LARGE_INTEGER at{};
    // (stage 3b) a room 0 whose kept record holds the lamp (Record::kept0; k is null then): the record's key and version, its
    // samples and batches (copied: the worker reads no record), its threshold and room 0's path of the lamp
    struct Room0 {
        uint64_t key = 0;
        uint32_t version = 0;
        int story = 0, cls = 0;
        float thr = 0.0f;
        std::vector<uint8_t> samples;
        std::vector<BatchRec> batches;
        LevelLightShare::Room0Path path;
    };
    std::shared_ptr<const Room0> r0;
};
std::mutex g_moveMx;
std::condition_variable g_moveCv;
std::map<std::pair<uint64_t, uintptr_t>, MoveJob> g_moveJobs; // (kept room key, lamp) -> its latest move (a drag's later move replaces it)
bool g_moveThread = false;
std::atomic<long> c_moveJobs{0}, c_moveDone{0}, c_moveStale{0}, c_moveFailed{0};
// The constants the game's tests read (once; the light tree thread, before the first job)
void ReadTestConsts() {
    static bool s_done = false;
    if (s_done) return;
    s_done = true;
    using GameAddr::Id;
    constexpr Id kIds[] = {Id::TestZero, Id::TestTwo, Id::TestNeg1, Id::TestFour, Id::TestDiscEdge, Id::TestStop, Id::TestQuadEdge,
                           Id::TestDrop, Id::TestOne, Id::TestEps, Id::TestHalf, Id::TestPenumbra, Id::TestWallY}; // (kConstAt's order)
    static_assert(std::size(kIds) == std::size(RoomLightTests::kConstAt));
    for (size_t i = 0; i < std::size(kIds); i++) {
        float v = 0.0f;
        if (const uintptr_t at = GameAddr::Get(kIds[i]); at && CopyBytes(&v, reinterpret_cast<const void*>(at), sizeof v)) RoomLightTests::SetConst(RoomLightTests::kConstAt[i], v);
    }
}
// The room's walls (+0x30..+0x34, 0x100 bytes each), its default 2D list (+0x78..+0x7C), its wall mode (+0x639) and its 3D
// records (+0x40..+0x44, 64 bytes: the centre, then r2 at +0x10, the blockers' vector at +0x30) with their blockers (0x100
// bytes), as the room lamp probe dumps them. False (why): unreadable, or a blocker the copy does not know (unless anyBlocker)
bool ReadRoomTests(const BYTE* room, RoomLightTests::Room& r, std::string& why, bool anyBlocker) {
    uintptr_t wv[2] = {}, i2[2] = {}, rb[2] = {};
    uint8_t mode = 0;
    if (!CopyBytes(&mode, room + 0x639, 1) || !CopyBytes(wv, room + 0x30, sizeof wv) || !CopyBytes(i2, room + 0x78, sizeof i2) || !CopyBytes(rb, room + 0x40, sizeof rb))
        return why = "its walls could not be read", false;
    r.mode = mode;
    const auto count = [](const uintptr_t* v, size_t size, size_t max) { return v[0] && v[1] >= v[0] && (v[1] - v[0]) % size == 0 ? (v[1] - v[0]) / size : v[0] == v[1] ? 0 : max + 1; };
    const size_t nw = count(wv, 4, 1024), ni = count(i2, 4, 4096), nr = count(rb, 64, 512);
    if (nw > 1024 || ni > 4096 || nr > 512) return why = "its walls or 3D records could not be read", false;
    r.walls.resize(nw);
    for (size_t i = 0; i < nw; i++) {
        uintptr_t w = 0;
        if (!CopyBytes(&w, reinterpret_cast<const void*>(wv[0] + i * 4), 4) || !w || !CopyBytes(r.walls[i].f, reinterpret_cast<const void*>(w), sizeof r.walls[i].f))
            return why = "a wall could not be read", false;
    }
    r.idx2d.resize(ni);
    if (ni && !CopyBytes(r.idx2d.data(), reinterpret_cast<const void*>(i2[0]), ni * 4)) return why = "its wall list could not be read", false;
    r.rec.resize(nr);
    for (size_t i = 0; i < nr; i++) {
        float rec[16] = {};
        if (!CopyBytes(rec, reinterpret_cast<const void*>(rb[0] + i * 64), 64)) return why = "a 3D record could not be read", false;
        r.rec[i].c = {rec[0], rec[1], rec[2]}, r.rec[i].r2 = rec[4], r.rec[i].cull2 = rec[8];
        uintptr_t bl[2] = {};
        std::memcpy(bl, &rec[12], sizeof bl);
        const size_t nb = count(bl, 4, 64);
        if (nb > 64) return why = "a 3D record's blockers could not be read", false;
        r.rec[i].b.resize(nb), r.rec[i].at.resize(nb);
        for (size_t j = 0; j < nb; j++) {
            uintptr_t b = 0;
            RoomLightTests::Blocker& k = r.rec[i].b[j];
            if (!CopyBytes(&b, reinterpret_cast<const void*>(bl[0] + j * 4), 4) || !b || !CopyBytes(&k.vt, reinterpret_cast<const void*>(b), 4) ||
                !CopyBytes(k.f, reinterpret_cast<const void*>(b), sizeof k.f))
                return why = "a 3D blocker could not be read", false;
            k.vt = TestVt(k.vt);
            if (!j) r.rec[i].shape = b;
            r.rec[i].at[j] = b;
            if (!anyBlocker && k.vt != RoomLightTests::kQuadVt && k.vt != RoomLightTests::kDiscVt) return why = std::format("a 3D blocker of a kind the copy does not know (vtable {:08X})", k.vt), false;
        }
    }
    return true;
}
// (stage 3b, the move worker) a room-0 job: the lamp's parts at every texel of the record's copy where its law at the new place
// is lit (room_lamp_move.h Part0At: InnerShadow's path; kept as a capture keeps them, lit before the soft light cut down to
// kKeepFloor of the threshold), put in the record's kept lamp with that law under a new version (Real-time outdoor lamps
// exports it again, ExportRoom0) while the record is the one copied (else dropped: solved or changed meanwhile)
void MoveRoom0(const MoveJob& job, const LARGE_INTEGER& t0) {
    const MoveJob::Room0& z = *job.r0;
    const size_t n = z.samples.size() / 0x30;
    KeptLamp kl = job.lamp;
    const float* L = kl.law.pos;
    int unknown = 0;
    long lit = 0;
    for (const BatchRec& b : z.batches) {
        const bool f1 = b.flags[1] != 0;
        for (uint32_t i = 0; i < b.count && b.first + i < n; i++) {
            const uint32_t t = b.first + i;
            const float* s = reinterpret_cast<const float*>(z.samples.data() + static_cast<size_t>(t) * 0x30);
            float lw[3];
            LawAt(kl.law, false, s, lw);
            const float sum = (lw[2] + lw[1]) + lw[0];
            if (!(sum > 0.0f)) continue;
            lit++;
            const float gol = RoomLampMove::GameOverLaw(kl.law, s, s + 4);
            float A = 0.0f, B = 0.0f;
            if (!RoomLampMove::Part0At(job.tests, f1, L, s, s + 4, sum * gol, z.thr, RoomLampMove::ReachAt(z.path.reach, z.path.reachW, L, s, s + 4), z.path.normK,
                                       [&] { return LevelLightShare::Room0PassAt(z.path, L, s); }, A, B, &unknown))
                continue;
            A *= gol;
            if (!(sum * A >= kKeepFloor * z.thr)) continue;
            KeptPart p;
            p.texel = t, p.a = ToHalf(std::min(A, 60000.0f)), p.b = ToHalf(std::clamp(B, 0.0f, 1.0f)), p.thrScale = ToHalf(1.0f), p.mult = job.mult, p.bits = job.bits;
            kl.parts.push_back(p);
        }
    }
    ShrinkExact(kl.parts);
    const size_t parts = kl.parts.size();
    LARGE_INTEGER t1{}, f{};
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&f);
    const double ms = 1000.0 * static_cast<double>(t1.QuadPart - t0.QuadPart) / static_cast<double>(f.QuadPart);
    const double since = 1000.0 * static_cast<double>(t1.QuadPart - job.at.QuadPart) / static_cast<double>(f.QuadPart);
    if (unknown) { // (ReadRoomTests refuses them: not reached)
        c_moveFailed.fetch_add(1, std::memory_order_relaxed);
        const bool queued = LevelLightShare::QueueRoom0(job.tracker, z.story);
        if (KeptLog())
            LOG_INFO(std::format("[GpuRoomLamps] move: lamp {:08X} story {} room 0: left to the room solve ({} rays met a 3D blocker the copy does not know){}", kl.light, z.story,
                                 unknown, queued ? "" : " (the room queue refused it)"));
        return;
    }
    uint32_t version = 0;
    {
        std::lock_guard<std::recursive_mutex> lk(g_mx);
        const auto it = g_records.find(z.key);
        if (it != g_records.end() && it->second->version == z.version)
            for (KeptLamp& k0 : it->second->kept0)
                if (k0.light == kl.light) {
                    kl.exit = k0.exit, kl.carried = k0.carried;
                    k0 = std::move(kl);
                    Record& r = *it->second;
                    version = r.version = g_version.fetch_add(1, std::memory_order_relaxed) + 1;
                    r.baseVersion = r.version; // (its export changed: Real-time outdoor lamps builds its slots again)
                    break;
                }
    }
    (version ? c_moveDone : c_moveStale).fetch_add(1, std::memory_order_relaxed);
    if (KeptLog())
        LOG_INFO(std::format("[GpuRoomLamps] move: lamp {:08X} story {} room 0 class {}: {} texels x 1 lamp recomputed ({} lit, {} parts) in {:.1f} ms on the move worker, "
                             "{:.1f} ms after the move; {}",
                             job.lamp.light, z.story, z.cls, n, lit, parts, ms, since,
                             version ? std::format("kept as version {}", version) : std::string("dropped (room 0 was solved or changed meanwhile)")));
}
void MoveWorker() {
    for (;;) {
        MoveJob job;
        {
            std::unique_lock<std::mutex> lk(g_moveMx);
            g_moveCv.wait(lk, [] { return !g_moveJobs.empty(); });
            job = std::move(g_moveJobs.begin()->second);
            g_moveJobs.erase(g_moveJobs.begin());
        }
        LARGE_INTEGER t0{}, t1{}, f{};
        QueryPerformanceCounter(&t0);
        if (job.r0) {
            MoveRoom0(job, t0);
            continue;
        }
        const KeptRoom& k = *job.k;
        const size_t n = k.texels.size();
        KeptLamp kl = job.lamp;
        int unknown = 0;
        long lit = 0;
        std::vector<int> list;
        for (const BatchRec& b : k.batches) {
            const bool f0 = b.flags[0] != 0, f1 = b.flags[1] != 0, built = RoomLampMove::ListsBuilt(b.count, f0, f1);
            if (built) { // (the cull ray from the batch's centroid, as the probe's dump gives it)
                float c[3] = {};
                for (uint32_t i = 0; i < b.count && b.first + i < n; i++)
                    for (int q = 0; q < 3; q++) c[q] += reinterpret_cast<const float*>(k.samples.data() + static_cast<size_t>(b.first + i) * 0x30)[q] / static_cast<float>(b.count);
                list = RoomLightTests::wallCull(job.tests, {c[0], c[1], c[2]}, {kl.law.pos[0], kl.law.pos[1], kl.law.pos[2]});
            }
            for (uint32_t i = 0; i < b.count && b.first + i < n; i++) {
                const uint32_t t = b.first + i;
                const float* s = reinterpret_cast<const float*>(k.samples.data() + static_cast<size_t>(t) * 0x30);
                float lw[3];
                LawAt(kl.law, false, s, lw);
                if (!((lw[2] + lw[1]) + lw[0] > 0.0f)) continue;
                float A = 1.0f, B = 0.0f;
                RoomLampMove::PartAt(job.tests, built, list, f0, f1, kl.law.pos, s, s + 4, A, B, &unknown);
                A *= RoomLampMove::GameOverLaw(kl.law, s, s + 4); // (A is over the game's evaluation; KeptAdd's law raises N.l itself)
                lit++;
                KeptPart p;
                p.texel = t, p.a = ToHalf(std::min(A, 60000.0f)), p.b = ToHalf(std::clamp(B, 0.0f, 1.0f)), p.thrScale = ToHalf(1.0f), p.mult = job.mult, p.bits = job.bits;
                kl.parts.push_back(p);
            }
        }
        std::sort(kl.parts.begin(), kl.parts.end(), [](const KeptPart& a, const KeptPart& b) { return a.texel < b.texel; });
        ShrinkExact(kl.parts);
        QueryPerformanceCounter(&t1);
        QueryPerformanceFrequency(&f);
        const double ms = 1000.0 * static_cast<double>(t1.QuadPart - t0.QuadPart) / static_cast<double>(f.QuadPart);
        const double since = 1000.0 * static_cast<double>(t1.QuadPart - job.at.QuadPart) / static_cast<double>(f.QuadPart);
        if (unknown) { // (ReadRoomTests refuses them: not reached)
            c_moveFailed.fetch_add(1, std::memory_order_relaxed);
            const bool queued = LevelLightShare::QueueRoomId(job.tracker, k.story, k.id, -1);
            if (KeptLog())
                LOG_INFO(std::format("[GpuRoomLamps] move: lamp {:08X} story {} room {}: left to the room solve ({} rays met a 3D blocker the copy does not know){}", kl.light, k.story,
                                     k.id, unknown, queued ? "" : " (the room queue refused it)"));
            continue;
        }
        auto nk = std::make_shared<KeptRoom>(k);
        nk->moveCopy = true;
        bool found = false;
        for (KeptLamp& l : nk->lamps)
            if (l.light == kl.light) l = kl, found = true;
        if (!found) nk->lamps.push_back(kl);
        nk->bytes = KeptBytes(*nk);
        bool put = false, full = false;
        uint32_t version = 0;
        size_t keptBytes = 0;
        {
            std::lock_guard<std::mutex> lk(g_keptMx);
            const auto it = g_kept.find(KeyOf(reinterpret_cast<const void*>(k.room), k.cls));
            keptBytes = g_keptBytes;
            // (the copy replaces the room's kept solve: within the budget as KeepEnd keeps a solve; past it the room keeps the
            // copy it has, drawn with the lamp's law as kept, and is sent to its solve below)
            full = it != g_kept.end() && it->second == job.k && g_keptBytes - std::min(g_keptBytes, k.bytes) + nk->bytes > kMaxKeptBytes;
            if (!full && it != g_kept.end() && it->second == job.k) {
                version = nk->version = g_keptVersion.fetch_add(1, std::memory_order_relaxed) + 1;
                // (the drawing this one continues: TakesLampEdit takes the next move while the GPU has not drawn it yet)
                nk->movedFrom = GpuRoomLamps::Drawn(reinterpret_cast<const void*>(k.room), k.version, k.maps) ? k.version : k.movedFrom;
                g_keptBytes = g_keptBytes - std::min(g_keptBytes, k.bytes) + nk->bytes;
                it->second = nk;
                put = true;
            }
        }
        if (full) {
            c_moveFailed.fetch_add(1, std::memory_order_relaxed);
            const bool queued = LevelLightShare::QueueRoomId(job.tracker, k.story, k.id, -1);
            if (KeptLog())
                LOG_INFO(std::format("[GpuRoomLamps] move: lamp {:08X} story {} room {}: left to the room solve (the {:.0f} MB budget: {:.1f} MB kept, this copy {:.1f} MB){}", kl.light,
                                     k.story, k.id, static_cast<double>(kMaxKeptBytes) / 1048576.0, static_cast<double>(keptBytes) / 1048576.0,
                                     static_cast<double>(nk->bytes) / 1048576.0, queued ? "" : " (the room queue refused it)"));
            continue;
        }
        (put ? c_moveDone : c_moveStale).fetch_add(1, std::memory_order_relaxed);
        if (put) GpuRoomLamps::NoteEdit(kl.light, "move", {reinterpret_cast<const void*>(k.room)}, std::to_string(k.story));
        if (KeptLog())
            LOG_INFO(std::format("[GpuRoomLamps] move: lamp {:08X} story {} room {} class {}: {} texels x 1 lamp recomputed ({} lit, {} parts) in {:.1f} ms on the move worker, "
                                 "{:.1f} ms after the move; {}",
                                 kl.light, k.story, k.id, k.cls, n, lit, kl.parts.size(), ms, since,
                                 put ? std::format("kept as version {}", version) : std::string("dropped (the room was solved or kept again meanwhile)")));
    }
}
void QueueMove(uint64_t key, MoveJob&& job) {
    std::lock_guard<std::mutex> lk(g_moveMx);
    if (!g_moveThread) {
        g_moveThread = true;
        std::thread(MoveWorker).detach();
    }
    g_moveJobs[{key, job.lamp.light}] = std::move(job);
    c_moveJobs.fetch_add(1, std::memory_order_relaxed);
    g_moveCv.notify_one();
}
// A move of `light` in kept room k (light tree thread): its job, or why it is left to the game's path
const char* MoveJobFor(const void* room, const RoomRead& rr, const std::shared_ptr<const KeptRoom>& k, uintptr_t light, uintptr_t tracker, MoveJob& job,
                       std::string& why) {
    if (!LawOfLight(light, rr.lot, job.lamp)) return "a lamp without a law now";
    if (job.lamp.colourOnly) return "a light without a GPU law (its share follows its place)";
    const char* path = nullptr;
    const int kind = LevelLightShare::MovePath(room, light, job.lamp.law.pos, path);
    if (kind < 0) return path;
    uint64_t geoNow[3] = {};
    if (!k->geo || RoomGeoKey(static_cast<const BYTE*>(room), geoNow) != k->geo) return GeoChanged(k->geoParts, geoNow);
    if (k->batches.empty()) return "its kept solve has no batches";
    if (!ReadRoomTests(static_cast<const BYTE*>(room), job.tests, why)) return why.c_str();
    uintptr_t raw[1024];
    const int n = ReadList(static_cast<const BYTE*>(room), raw, static_cast<int>(std::size(raw)));
    job.mult = static_cast<uint8_t>(std::clamp<long long>(std::count(raw, raw + std::max(n, 0), light), 1, 255));
    // the soft light cut as the room's parts were kept (none kept: as it is now)
    bool any = false, fade = false;
    for (const KeptLamp& l : k->lamps)
        for (const KeptPart& p : l.parts) any = true, fade = fade || (p.bits & kKeptFade);
    if (!any) fade = LevelLightShare::SoftLightCutOn();
    job.bits = static_cast<uint8_t>((kind == 1 ? kKeptEarly : 0) | (fade ? kKeptFade : 0));
    job.k = k, job.tracker = tracker;
    QueryPerformanceCounter(&job.at);
    return nullptr;
}
bool RecordOfRoom(const Record& r, const RoomRead& now); // below
// (stage 3b) A move of `light` in a room 0 that lists it (light tree thread): its job (a copy of the kept record's samples and
// batches, room 0's path, its tests), or why it is left to the game's path. Only a lamp the record keeps (kept0) with no floor
// mask part, taken by room 0 through an opening of its own story (LevelLightShare::Room0MovePath), while the lot's geometry is
// that of the record's solve
const char* MoveJobFor0(const void* room, const RoomRead& rr, uintptr_t light, uintptr_t tracker, MoveJob& job, std::string& why) {
    if (!LawOfLight(light, rr.lot, job.lamp)) return "a lamp without a law now";
    if (job.lamp.colourOnly) return "a light without a GPU law (its share follows its place)";
    auto z = std::make_shared<MoveJob::Room0>();
    const char* path = nullptr;
    if (!LevelLightShare::Room0MovePath(room, light, z->path, path)) return path;
    bool fade = false;
    {
        std::lock_guard<std::recursive_mutex> lk(g_mx);
        const auto it = g_records.find(KeyOf(room, rr.cls));
        if (it == g_records.end() || !RecordOfRoom(*it->second, rr)) return "no kept record of room 0's class";
        const Record& r = *it->second;
        if (r.stale || r.baseOnly) return "room 0's kept record waits for its solve";
        const auto k0 = std::find_if(r.kept0.begin(), r.kept0.end(), [&](const KeptLamp& k) { return k.light == light; });
        if (k0 == r.kept0.end()) return "room 0's kept record does not hold its parts";
        if (!k0->mask.empty()) return "it has floor mask parts in room 0 (not redone for a move)";
        if (!r.geo || LevelLightShare::LotGeometryKey(room) != r.geo) return "the lot's geometry changed since room 0's solve";
        try {
            z->samples.assign(r.samples.begin(), r.samples.end());
            z->batches = r.batches;
        } catch (...) {
            return "no memory for the copy";
        }
        z->key = KeyOf(room, rr.cls), z->version = r.version, z->story = r.story, z->cls = r.cls, z->thr = r.thr;
        // the soft light cut as room 0's kept parts were kept (none kept: as it is now)
        bool any = false;
        for (const KeptLamp& l : r.kept0)
            for (const KeptPart& p : l.parts) any = true, fade = fade || (p.bits & kKeptFade);
        if (!any) fade = LevelLightShare::SoftLightCutOn();
    }
    if (!ReadRoomTests(static_cast<const BYTE*>(room), job.tests, why)) return why.c_str();
    uintptr_t raw[1024];
    const int n = ReadList(static_cast<const BYTE*>(room), raw, static_cast<int>(std::size(raw)));
    job.mult = static_cast<uint8_t>(std::clamp<long long>(std::count(raw, raw + std::max(n, 0), light), 1, 255));
    job.bits = static_cast<uint8_t>(kKeptEarly | (fade ? kKeptFade : 0));
    job.r0 = std::move(z), job.tracker = tracker;
    QueryPerformanceCounter(&job.at);
    return nullptr;
}
// ---- GPU room lamps stage 4: the directional maps of an edit taken (docs/features/night-lighting/gpu-room-lamps.md) ----
// The objects and Sims read the story's four directional maps (the set [room+8]), which only the room solves write. An edit
// taken (TakesLampEdit) queues the lamp; at the end of the room update (BasisDue, light tree thread) each of its kept rooms
// whose capture packed again exactly gets the lamp's part again (0 switched off, else its share of the game's step FUN_0069f280
// called now: the rig colour and place the step reads now; a move's share is 1, the plain path's) and its samples are packed
// again and written into the game's own textures of the set: the game itself locks and writes them on this thread (the unlock
// 0x0069E950 copies the working set in with 0x00619A00: LockRect / UnlockRect), and only while no solve holds the working maps
// (its unlock would copy the set it read before back over the write). A texel is written only while it holds what the room's
// capture or Apex's last write left there (another room's solve may have written it since).
struct BasisRoomRef {
    uint64_t key = 0;
    int story = 0, id = 0;
};
struct BasisEdit {
    const char* kind = "";
    bool off = false, move = false;
    std::vector<BasisRoomRef> rooms;
    LARGE_INTEGER at{};
    DWORD tick = 0;
};
std::map<uintptr_t, BasisEdit> g_basisEdits; // lamp -> its last edit taken, not yet written (light tree thread)
struct BasisNow {
    uint32_t id = 0;                                         // KeptRoom::basisId these hold for
    std::unordered_map<uintptr_t, std::vector<float>> parts; // the lamps edited since: their part now (16 per sample)
    std::vector<uint32_t> expect;                            // what each texel holds: the capture's, then Apex's last write
    bool phys = false;                                       // (physical, stage 5) written physically: `drawn` its lamps then
    std::vector<uintptr_t> drawn;
    std::vector<std::array<float, 3>> drawnAt; // (physical) where its last write drew each lamp of `drawn` (world)
    std::shared_ptr<const RoomLightTests::Room> drawnTests; // (physical) the 3D records it traced over when carried (a table drag); null: its solve's
    std::vector<uint32_t> carry; // (physical) Apex's last write, put back over the kept solve's texels (WritePhysCarry); empty: none
    size_t carryStale = 0;       // (its log) samples of `carry` left to the solve, and the ms CarryStale took
    double carryMs = 0.0;
};
std::unordered_map<uint64_t, BasisNow> g_basisNow; // kept room key -> (light tree thread)
struct BasisTexelWrite {
    uint16_t x, y;
    uint32_t want, expect, game;
    uint8_t done;
};
uintptr_t RoomSet(uintptr_t room) { // POD (SEH): the room's directional set [room+8]
    __try {
        return *reinterpret_cast<const uintptr_t*>(room + 8);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 0;
    }
}
// Map m of a set (the wrapper [set + 4m]; a 2D texture [wrapper+0x30] while [wrapper+0x2C] is 0, as 0x00618DF0 locks it): each
// texel holding `expect` takes `want`. POD (SEH); false: the map is not the captured one or could not be locked
bool WriteBasisMapTry(uintptr_t set, int m, uint32_t w, uint32_t h, BasisTexelWrite* t, size_t n) {
    __try {
        const uintptr_t wrapper = *reinterpret_cast<const uintptr_t*>(set + static_cast<uintptr_t>(m) * 4);
        if (!wrapper || *reinterpret_cast<const uint32_t*>(wrapper + 0x2C) != 0) return false;
        IDirect3DTexture9* tex = *reinterpret_cast<IDirect3DTexture9* const*>(wrapper + 0x30);
        D3DSURFACE_DESC dd{};
        if (!tex || FAILED(tex->GetLevelDesc(0, &dd)) || dd.Format != D3DFMT_A8R8G8B8 || dd.Width != w || dd.Height != h) return false;
        D3DLOCKED_RECT lr{};
        if (FAILED(tex->LockRect(0, &lr, nullptr, 0)) || !lr.pBits) return false;
        for (size_t i = 0; i < n; i++) {
            uint32_t* p = reinterpret_cast<uint32_t*>(static_cast<BYTE*>(lr.pBits) + static_cast<size_t>(lr.Pitch) * t[i].y) + t[i].x;
            t[i].done = BasisWrite(*p, t[i].want, t[i].expect, t[i].game) ? 1 : 0;
        }
        tex->UnlockRect(0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
void QueueBasis(uintptr_t light, const char* kind, bool off, bool move, const std::vector<BasisRoomRef>& rooms) {
    BasisEdit& e = g_basisEdits[light];
    e.kind = kind, e.off = off, e.move = move;
    for (const BasisRoomRef& r : rooms)
        if (std::none_of(e.rooms.begin(), e.rooms.end(), [&](const BasisRoomRef& x) { return x.key == r.key; })) e.rooms.push_back(r);
    QueryPerformanceCounter(&e.at);
    e.tick = GetTickCount();
}
// The lamp's part of each sample of the kept room now (into g_basisNow); null, or why not
BasisNow& NowOf(uint64_t key, const KeptRoom& k) {
    BasisNow& bn = g_basisNow[key];
    if (bn.id != k.basisId) bn = BasisNow{}, bn.id = k.basisId, bn.expect = k.basisGame;
    return bn;
}
const char* LampBasisNow(uint64_t key, const KeptRoom& k, uintptr_t light, const BasisEdit& e) {
    BasisNow& bn = NowOf(key, k);
    const size_t n = k.basis.size();
    const auto bl = std::find_if(k.basisLamps.begin(), k.basisLamps.end(), [&](const KeptRoom::BasisLamp& b) { return b.light == light; });
    if (e.move && DevSwitches::BasisWallFilter()) return "the walls in the directional maps (basis_wall_filter) are not redone for a move";
    std::vector<float> v(n * 16, 0.0f);
    if (!e.off)
        for (size_t i = 0; i < n; i++) {
            float step[16];
            if (!LevelLightShare::BasisStep(k.basis[i].pos, light, step)) return "the game's step could not be called";
            float pass = bl != k.basisLamps.end() ? bl->pass[i] : std::nanf("");
            if (e.move) { // (the plain path: Apex's basis tests leave such a lamp's step as it is)
                if (std::isfinite(pass) && std::fabs(pass - 1.0f) > 1e-3f) return "Apex's tests scale its directional part (not redone for a move)";
                pass = 1.0f;
            }
            if (!BasisPartNow(pass, bl != k.basisLamps.end() ? bl->mult : 1, step, &v[i * 16])) return "its directional part there is not the game's step times a share";
        }
    bn.parts[light] = std::move(v);
    return nullptr;
}
// The kept room's samples' texels (want(i, m): sample i, map m) written into its set
template <class Want>
const char* WriteBasisTexels(uint64_t key, const KeptRoom& k, const Want& want, long& written, long& left) {
    const uintptr_t set = RoomSet(k.room);
    if (!set || set != k.basisSet) return "its directional set changed";
    BasisNow& bn = NowOf(key, k);
    const size_t n = k.basis.size();
    std::vector<BasisTexelWrite> t(n);
    const long left0 = left;
    double sumNew = 0.0, sumGame = 0.0; // (evidence: furniture brighter for seconds after an edit) RGB levels written vs the solve's
    const bool trace = Recorder::Verbose(); // (the sums feed only the [BasisRatio] trace line)
    const auto rgb = [](uint32_t c) { return static_cast<double>(((c >> 16) & 0xFF) + ((c >> 8) & 0xFF) + (c & 0xFF)); };
    for (int m = 0; m < 4; m++) {
        for (size_t i = 0; i < n; i++) {
            const uint32_t expect = bn.expect[i * 4 + m];
            t[i] = BasisTexelWrite{k.basis[i].x, k.basis[i].y, want(i, m), expect, i * 4 + m < k.basisGame.size() ? k.basisGame[i * 4 + m] : expect, 0};
        }
        if (!WriteBasisMapTry(set, m, k.basisW, k.basisH, t.data(), n)) return "a directional map of its set is not the captured one";
        for (size_t i = 0; i < n; i++) {
            if (t[i].done) bn.expect[i * 4 + m] = t[i].want;
            (t[i].done ? written : left)++;
            if (trace && i * 4 + m < k.basisGame.size()) sumNew += rgb(t[i].want), sumGame += rgb(k.basisGame[i * 4 + m]);
        }
    }
    if (trace && TraceLine())
        LOG_INFO(std::format("[BasisRatio] story {} room {}: directional maps written at {:.2f}x the levels of its game solve ({} texels, {} left as another solve wrote them)",
                             k.story, k.id, sumGame > 0.0 ? sumNew / sumGame : 0.0, n * 4, left - left0));
    return nullptr;
}
// The kept room's samples' accumulators (16 each) packed and written into its set
const char* WriteBasisAcc(uint64_t key, const KeptRoom& k, const std::vector<float>& acc, long& written, long& left) {
    return WriteBasisTexels(key, k, [&](size_t i, int m) { return PackBasis(&acc[i * 16], m); }, written, left);
}
// The kept room's samples packed again (its base, each lamp's part now or as kept) and written into its set
const char* WriteBasis(uint64_t key, const KeptRoom& k, long& written, long& left) {
    const BasisNow& bn = NowOf(key, k);
    std::vector<float> acc(k.basisBase);
    const auto add = [&](const std::vector<float>& p) {
        if (p.size() == acc.size())
            for (size_t q = 0; q < acc.size(); q++) acc[q] += p[q];
    };
    for (const KeptRoom::BasisLamp& bl : k.basisLamps) {
        const auto it = bn.parts.find(bl.light);
        add(it != bn.parts.end() ? it->second : bl.c);
    }
    for (const auto& [L, p] : bn.parts) // (a lamp with no part at the capture)
        if (std::none_of(k.basisLamps.begin(), k.basisLamps.end(), [&](const KeptRoom::BasisLamp& b) { return b.light == L; })) add(p);
    return WriteBasisAcc(key, k, acc, written, left);
}
void BasisDuePhys(); // below (physical indoor lamps, stage 5)
void BasisDue() {
    if (!g_capture.load(std::memory_order_relaxed)) return void(g_basisEdits.clear());
    if (g_physical.load(std::memory_order_relaxed)) return BasisDuePhys();
    if (g_basisEdits.empty()) return;
    const DWORD now = GetTickCount();
    if (!LevelLightShare::RoomMapsFree()) { // (a solve between its lock and unlock: the next update, at most 1 s)
        for (auto it = g_basisEdits.begin(); it != g_basisEdits.end();) {
            if (now - it->second.tick <= 1000) {
                ++it;
                continue;
            }
            if (KeptLog())
                LOG_INFO(std::format("[GpuRoomLamps] {}: lamp {:08X} directional maps not written (another room's maps stayed locked for 1 s; they follow its rooms' solves)",
                                     it->second.kind, it->first));
            it = g_basisEdits.erase(it);
        }
        return;
    }
    const std::map<uintptr_t, BasisEdit> edits = std::move(g_basisEdits);
    g_basisEdits.clear();
    std::unordered_map<uint64_t, std::shared_ptr<const KeptRoom>> kept;
    {
        std::lock_guard<std::mutex> lk(g_keptMx);
        for (const auto& [light, e] : edits)
            for (const BasisRoomRef& r : e.rooms)
                if (const auto it = g_kept.find(r.key); it != g_kept.end()) kept[r.key] = it->second;
    }
    std::map<uintptr_t, std::string> why; // lamp -> its rooms not written
    std::set<uint64_t> dirty;
    std::map<uintptr_t, std::vector<uint64_t>> ok; // lamp -> its rooms whose part was computed again
    const auto note = [&](uintptr_t light, const BasisRoomRef& r, const char* w) { why[light] += std::format("{}story {} room {}: {}", why[light].empty() ? "" : ", ", r.story, r.id, w); };
    for (const auto& [light, e] : edits)
        for (const BasisRoomRef& r : e.rooms) {
            const auto it = kept.find(r.key);
            const char* w = it == kept.end() ? "its kept solve was dropped" : it->second->basisWhy;
            if (w == kNoBasis) continue; // (nothing of it to show)
            if (!w) w = LampBasisNow(r.key, *it->second, light, e);
            if (w) note(light, r, w);
            else dirty.insert(r.key), ok[light].push_back(r.key);
        }
    std::map<uint64_t, std::pair<long, long>> wrote; // key -> texels written, left as another solve wrote them
    std::map<uint64_t, const char*> failed;
    for (const uint64_t key : dirty) {
        long w = 0, l = 0;
        if (const char* f = WriteBasis(key, *kept[key], w, l)) failed[key] = f;
        else wrote[key] = {w, l};
    }
    if (!KeptLog()) return;
    LARGE_INTEGER t1{}, f{};
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&f);
    for (const auto& [light, e] : edits) {
        int rooms = 0, done = 0;
        long w = 0, l = 0;
        for (const BasisRoomRef& r : e.rooms) {
            const auto it = kept.find(r.key);
            if (it != kept.end() && it->second->basisWhy == kNoBasis) continue;
            rooms++;
            if (std::find(ok[light].begin(), ok[light].end(), r.key) == ok[light].end()) continue;
            if (const auto fw = failed.find(r.key); fw != failed.end()) note(light, r, fw->second);
            else done++, w += wrote[r.key].first, l += wrote[r.key].second;
        }
        if (!rooms) continue;
        LOG_INFO(std::format("[GpuRoomLamps] {}: lamp {:08X} directional maps written in {} of {} rooms {:.1f} ms after the edit ({} texels{}){}", e.kind, light, done, rooms,
                             1000.0 * static_cast<double>(t1.QuadPart - e.at.QuadPart) / static_cast<double>(f.QuadPart), w,
                             l ? std::format(", {} left as another solve wrote them", l) : std::string(),
                             why[light].empty() ? std::string() : std::format("; not written: {} (they follow its rooms' solves)", why[light])));
    }
}

// ---- physical indoor lamps, stage 5: the directional maps (docs/features/night-lighting/gpu-room-lamps.md "Physical model") ----
// After a physical edit taken (value, switch, move, new; a lamp that left the lot's registry) every kept room > 0 whose
// directional samples the lamp's reach meets now, or whose last write drew it (before any: its capture listed it), gets the
// parts of every lamp of the active lot with a law whose reach meets its samples, picked as its maps pick them
// (gpu_room_lamps.cpp: the solve's lamps out of its base with their rows, a fresh row once registered elsewhere or never
// listed): row x share x the game's step called now, share = the open-top proxy's visibility
// (PhysLampPick::BasisShare), dropped under K_DROP; plus its base (the lights without a law: BuildBasis). The
// steps run on the light tree thread when the job starts, the shares on a worker, the write at a later room update with
// stage 4's rules (only texels holding the capture's or the last write's value, only while no solve holds the working maps).
// A room written so is written again after its next kept solve (the settle's too): its texels do not go back to the game's.
// Room 0 has no directional samples to write: state 0 (0x006A18B0) sets up the directional pass (room+0x540, 0x006AA120 with
// 1 and 0x40 at 0x006A1BAB) only past 0x006A1AE8's `cmp byte [room+0x18], 0; jne` (an outdoor room returns first), and a
// room 0 capture that saw a directional step is never kept (SolveEnd: "a directional map") while room 0's are kept.
struct PhysBasisLamp {
    uintptr_t light = 0;
    float row = 1.0f, world[3] = {}, lot[3] = {};
    std::vector<float> step;     // 16 per sample
    std::vector<float> vis;      // per sample (the worker; 0 where the step is dark)
    std::vector<float> pane;     // per sample (the worker): the glass its light crosses (PhysLampPick::ShareCache)
    std::vector<float> part;     // (moved) its whole part, 16 per sample, as the room's solve would add it here; empty: step x shares
    bool model = false;          // its part needs the worker's shares
};
struct PhysBasisRoom {
    BasisRoomRef ref;
    std::shared_ptr<const KeptRoom> k;
    std::vector<float> lotPos; // 3 per sample
    std::vector<PhysBasisLamp> lamps;
    std::shared_ptr<const RoomLightTests::Room> tests; // (a table dragged with its lamp) its solve's records with the carried ones moved; null: k->tests
    uint64_t offKey = 0;                               // the carried offsets' hash (0: none)
    size_t traced = 0; // (the worker) samples traced, the others' shares kept from the last job
    bool redo = false;
};
struct PhysBasisJob {
    std::map<uintptr_t, BasisEdit> edits;
    std::map<uintptr_t, std::string> why; // lamp -> its rooms not written
    std::vector<PhysBasisRoom> rooms;
    std::shared_ptr<const RtLampProxy::Lot> lot;
    uint32_t gen = 0;
    float radius = 0.0f;
    double workerMs = 0.0, partMs = 0.0; // (partMs: the moved lamps' parts on the light tree thread)
    size_t basisRooms = 0;               // the physical kept rooms with directional maps the edits were tested against
    std::map<uint64_t, std::pair<long, long>> wroteNow; // the rooms written as the job started (no lamp of theirs needs the worker)
    std::map<uint64_t, const char*> failedNow;
    DWORD doneTick = 0;
    const char* failed = nullptr;
};
std::mutex g_pbMx;
std::condition_variable g_pbCv;
std::unique_ptr<PhysBasisJob> g_pbIn, g_pbOut; // (under g_pbMx) the job handed to the worker, the one it finished
bool g_pbThread = false;                        // (under g_pbMx)
bool g_pbBusy = false;                          // (light tree thread) a job handed over, not written yet
std::unique_ptr<PhysBasisJob> g_pbDone;         // (light tree thread) a finished job waiting for the working maps
std::set<uint64_t> g_pbRedo;                    // (light tree thread) rooms written physically, kept again since
// The game marks a room's rigs only for a lamp of that room: furniture of the rooms taking a lamp through an arch kept its old rig
// until the 5 s sweep (the fridge lit 4.5 s after the lamp had left). After a taken edit, at most every 100 ms and
// so also after the last step of a drag: the rooms listing it and every room of its story (one it comes into lists it only at
// its next gather: ObjectLightBridge CrossCandidates). Light tree thread
struct RigTail {
    uintptr_t tracker = 0;
    int story = 0;
    std::vector<std::pair<int, int>> ids;
    bool due = false;
    DWORD at = 0;
} g_rigTail;
void RegatherRigTail() {
    const DWORD now = GetTickCount();
    if (!g_rigTail.due || now - g_rigTail.at < 100 || !LevelLightShare::TrackerLive(g_rigTail.tracker)) return;
    g_rigTail.due = false, g_rigTail.at = now;
    for (const auto& [st, id] : g_rigTail.ids) ObjectLightBridge::RegatherRigsOfRoom(g_rigTail.tracker, st, id);
    std::vector<void*> rooms;
    LevelLightShare::LotRooms(g_rigTail.tracker, rooms);
    for (void* room : rooms)
        if (RoomRead rr; ReadRoom(static_cast<const BYTE*>(room), rr) && rr.story == g_rigTail.story && rr.id > 0)
            ObjectLightBridge::RegatherRigsOfRoom(g_rigTail.tracker, rr.story, rr.id);
}
// (light tree thread) a lamp's modelled part last written in a kept room: what it was written from and where the lamp stood
struct ModelPart {
    uint32_t basisId = 0; // (its solve)
    std::array<float, 3> world{};
    std::vector<float> part;
};
std::map<std::pair<uint64_t, uintptr_t>, ModelPart> g_modelParts;
// (light tree thread) per kept room: its solve (basisId) and the records carried since, (record, dx, dz) (StartPhysBasis)
std::unordered_map<uint64_t, std::pair<uint32_t, std::vector<std::array<float, 3>>>> g_basisCarried;
// The solve wrote its own levels (about 2x the physical ones in a stairwell), drawn until the redo's write lands a worker
// trace later: the stairs flashed for 1 to 3 frames on each solve during a drag). Apex's last write goes back at
// the end of this room update instead (WritePhysCarry) when the solve sampled the same texels; the redo still refines it
void NotePhysBasisKept(uint64_t key, const KeptRoom* prev, const KeptRoom& k) {
    const auto it = g_basisNow.find(key);
    if (it == g_basisNow.end() || !it->second.phys) return;
    g_pbRedo.insert(key);
    BasisNow& bn = it->second;
    // (furniture moved between the solves too: the carried write's old furniture shadow lasts only until the same redo, the solve's
    // own levels would flash instead; a table dragged with its lamp moves its room's records on every solve)
    if (!prev || bn.id != prev->basisId || prev->basisSet != k.basisSet || !SameBasisSamples(*prev, k)) return;
    std::vector<uint32_t> last = bn.carry.empty() ? std::move(bn.expect) : std::move(bn.carry); // (one not written yet: carried on)
    if (last.size() != k.basisGame.size()) return;
    CarryBasis(last.data(), prev->basisGame.data(), k.basisGame.data(), last.size());
    // ... except where a record that moved, came or went between the solves may shade a sample from a lamp: there the last write
    // held the old table's shadow, left on furniture and stairs at its old place after the drop); the new solve's
    // value until the redo. Each lamp is tested along its way between the solves and from where the last write drew it (a lamp
    // the solves did not list, or one that left the room during the drag, included)
    size_t staleN = 0;
    double staleMs = 0.0;
    if (prev->roomGeo != k.roomGeo || !prev->roomGeo || bn.drawnTests) {
        LARGE_INTEGER t0{}, t1{}, f{};
        QueryPerformanceCounter(&t0);
        std::vector<std::array<float, 6>> lamps;
        const auto seg = [&](const float* a, const float* b) { lamps.push_back({a[0], a[1], a[2], b[0], b[1], b[2]}); };
        const auto in = [](const std::vector<KeptLamp>& v, uintptr_t light) {
            const auto it = std::find_if(v.begin(), v.end(), [&](const KeptLamp& x) { return x.light == light; });
            return it != v.end() ? &*it : nullptr;
        };
        for (const KeptLamp& kl : prev->lamps) {
            const KeptLamp* b = in(k.lamps, kl.light);
            seg(kl.pos, b ? b->pos : kl.pos);
        }
        for (const KeptLamp& kl : k.lamps) {
            if (!in(prev->lamps, kl.light)) seg(kl.pos, kl.pos);
            if (!kl.colourOnly) seg(kl.pos, kl.law.pos); // (its way during the solve: kl.pos is where the records were read)
        }
        for (size_t d = 0; d < bn.drawn.size() && d < bn.drawnAt.size(); d++) {
            const KeptLamp* b = in(k.lamps, bn.drawn[d]);
            seg(bn.drawnAt[d].data(), b ? b->pos : bn.drawnAt[d].data());
        }
        const std::vector<char> stale =
            PhysLampPick::CarryStale(bn.drawnTests ? *bn.drawnTests : prev->tests, k.tests, lamps, k.basis.size(), [&](size_t i) { return k.basis[i].pos; });
        for (size_t i = 0; i < stale.size(); i++)
            if (stale[i]) std::copy_n(&k.basisGame[i * 4], 4, &last[i * 4]), staleN++;
        QueryPerformanceCounter(&t1), QueryPerformanceFrequency(&f);
        staleMs = 1000.0 * static_cast<double>(t1.QuadPart - t0.QuadPart) / static_cast<double>(f.QuadPart);
    }
    std::vector<uintptr_t> drawn = std::move(bn.drawn);
    std::vector<std::array<float, 3>> drawnAt = std::move(bn.drawnAt);
    std::shared_ptr<const RoomLightTests::Room> drawnTests = std::move(bn.drawnTests);
    BasisNow& now = NowOf(key, k); // (the new solve's capture expected)
    now.phys = true, now.drawn = std::move(drawn), now.drawnAt = std::move(drawnAt), now.drawnTests = std::move(drawnTests), now.carry = std::move(last);
    now.carryStale = staleN, now.carryMs = staleMs;
}
// The solve's unlock copied the class's whole working set over the story's directional maps: the other rooms of the set written
// physically show their game solve's values again. Their last write goes back with this room's (WritePhysCarry; BasisWrite takes
// a texel holding the room's own solve value)
void CarrySiblings(uint64_t key, uintptr_t set) {
    std::lock_guard<std::mutex> lk(g_keptMx);
    for (auto& [other, bn] : g_basisNow) {
        if (other == key || !bn.phys || !bn.carry.empty() || bn.expect.empty()) continue;
        const auto it = g_kept.find(other);
        if (it != g_kept.end() && it->second->basisId == bn.id && it->second->basisSet == set) bn.carry = bn.expect;
    }
}
// (light tree thread, the working maps free) the carried writes into their rooms' sets, before any redo's
void WritePhysCarry() {
    for (auto& [key, bn] : g_basisNow) {
        if (bn.carry.empty()) continue;
        const std::vector<uint32_t> carry = std::move(bn.carry);
        bn.carry.clear();
        std::shared_ptr<const KeptRoom> k;
        {
            std::lock_guard<std::mutex> lk(g_keptMx);
            if (const auto it = g_kept.find(key); it != g_kept.end() && it->second->basisId == bn.id) k = it->second;
        }
        if (!k) continue; // (dropped since)
        long written = 0, left = 0;
        const char* why = WriteBasisTexels(key, *k, [&](size_t i, int m) { return carry[i * 4 + m]; }, written, left);
        if (why ? KeptLog() : TraceLine()) // (one per solve during a drag: the verbose trace's budget)
            LOG_INFO(std::format("[GpuRoomLamps] physical: story {} room {}: its last directional write put back over its kept solve{}; {} of {} samples left to the solve "
                                 "(furniture moved, {:.2f} ms)",
                                 k->story, k->id, why ? std::format(" failed ({})", why) : std::format(" ({} texels, {} left as another solve wrote them)", written, left),
                                 bn.carryStale, k->basis.size(), bn.carryMs));
    }
}
void QueueBasisGone(uintptr_t light) {
    if (g_physical.load(std::memory_order_relaxed)) QueueBasis(light, "gone", true, false, {});
}
bool StepLit(const float* s) {
    for (int i = 0; i < 16; i++)
        if ((i & 3) != 3 && s[i] != 0.0f) return true;
    return false;
}
void PhysBasisWorker() {
    RtLampProxy::Mesh mesh;
    std::vector<RtLampProxy::Pane> panes;
    uint32_t meshGen = 0;
    std::vector<uint32_t> stamp;
    uint32_t mark = 0;
    // each room's lamps' shares from the last job (PhysLampPick::BasisSharesCached), valid while its kept solve, the proxy and the
    // radius stay; dropped past kShareBytes (32-bit address space)
    struct RoomShares {
        std::weak_ptr<const KeptRoom> k;
        uint32_t gen = 0;
        float radius = 0.0f;
        std::vector<float> lotPos; // (the samples in the lot's space: the same lot transform)
        uint64_t offKey = 0;       // (the same carried table)
        std::unordered_map<uintptr_t, PhysLampPick::ShareCache> lamps;
    };
    constexpr size_t kShareBytes = 24u << 20;
    std::unordered_map<uint64_t, RoomShares> shares;
    for (;;) {
        std::unique_ptr<PhysBasisJob> job;
        {
            std::unique_lock<std::mutex> lk(g_pbMx);
            g_pbCv.wait(lk, [] { return g_pbIn != nullptr; });
            job = std::move(g_pbIn);
        }
        LARGE_INTEGER t0{}, t1{}, f{};
        QueryPerformanceCounter(&t0);
        try {
            if (meshGen != job->gen) {
                RtLampProxy::Build(*job->lot, mesh, true), meshGen = job->gen, stamp.clear(), mark = 0;
                RtLampProxy::PanesOf(*job->lot, panes);
            }
            size_t bytes = 0;
            for (PhysBasisRoom& r : job->rooms) {
                const size_t n = r.k->basis.size();
                RoomShares& rs = shares[r.ref.key];
                const bool same = rs.k.lock() == r.k && rs.gen == job->gen && rs.radius == job->radius && rs.lotPos == r.lotPos && rs.offKey == r.offKey;
                if (!same) rs.lamps.clear(), rs.k = r.k, rs.gen = job->gen, rs.radius = job->radius, rs.lotPos = r.lotPos, rs.offKey = r.offKey;
                std::unordered_map<uintptr_t, PhysLampPick::ShareCache> keep; // (only the room's lamps now: one gone frees its shares)
                for (PhysBasisLamp& l : r.lamps) {
                    PhysLampPick::ShareCache& c = keep[l.light] = std::move(rs.lamps[l.light]);
                    if (!l.part.empty()) continue; // (its part is the solve's: no share to trace)
                    r.traced += PhysLampPick::BasisSharesCached(c, same, n, [&](size_t i) { return StepLit(&l.step[i * 16]); }, mesh, l.lot, job->radius, r.lotPos.data(), stamp, mark, l.vis,
                                                                  DevSwitches::GlassShare() ? &panes : nullptr);
                    l.pane = c.pane;
                }
                rs.lamps = std::move(keep);
            }
            for (const auto& [key, rs] : shares) {
                bytes += rs.lotPos.size() * sizeof(float);
                for (const auto& [light, c] : rs.lamps) bytes += c.done.size() * (2 * sizeof(float) + 1);
            }
            if (bytes > kShareBytes) shares.clear();
        } catch (...) {
            meshGen = 0;
            shares.clear();
            job->failed = "no memory";
        }
        QueryPerformanceCounter(&t1);
        QueryPerformanceFrequency(&f);
        job->workerMs = 1000.0 * static_cast<double>(t1.QuadPart - t0.QuadPart) / static_cast<double>(f.QuadPart);
        std::lock_guard<std::mutex> lk(g_pbMx);
        g_pbOut = std::move(job);
    }
}
// The edits' rooms and their lamps' steps now (light tree thread), handed to the worker; or logged when no room takes them
void LogPhysBasis(const PhysBasisJob& job, const std::map<uint64_t, std::pair<long, long>>& wrote, std::map<uint64_t, const char*>& failed);
void WritePhysRooms(const std::vector<PhysBasisRoom>& rooms, const char* jobFailed, std::map<uint64_t, std::pair<long, long>>& wrote, std::map<uint64_t, const char*>& failed);
void StartPhysBasis(std::map<uintptr_t, BasisEdit>&& edits) {
    auto job = std::make_unique<PhysBasisJob>();
    job->edits = std::move(edits);
    for (auto& [light, e] : job->edits) e.rooms.clear(); // (the rooms its reach meets, below; not the rooms listing it)
    job->lot = RtOutdoorLamps::KeptLot(job->gen);
    job->radius = Penumbra::Radius();
    // (the registry read again now: the game's step below is called with each lamp's light, never one deleted since the last
    // snapshot, whose light may be freed)
    g_lotLampsDirty.store(true, std::memory_order_relaxed);
    RefreshLotLamps();
    std::vector<LotLamp> lot;
    std::vector<uintptr_t> gone;
    LotLamps(lot, &gone);
    std::unordered_map<uintptr_t, const LotLamp*> at;
    for (const LotLamp& L : lot) at[L.light] = &L;
    std::vector<std::pair<uint64_t, std::shared_ptr<const KeptRoom>>> kept;
    {
        std::lock_guard<std::mutex> lk(g_keptMx);
        for (const auto& [key, k] : g_kept)
            if (k->physical) kept.emplace_back(key, k);
    }
    const std::set<uint64_t> redo = std::move(g_pbRedo);
    g_pbRedo.clear();
    for (const auto& [key, kp] : kept) {
        const KeptRoom& k = *kp;
        if (k.basisWhy == kNoBasis) continue;
        job->basisRooms++;
        PhysBasisRoom r;
        r.ref = BasisRoomRef{key, k.story, k.id}, r.k = kp, r.redo = redo.count(key) > 0;
        const size_t n = k.basis.size();
        PhysLampPick::Box box;
        for (const Capture::BasisSample& s : k.basis) box.Grow(s.pos);
        // (gpu_room_lamps.cpp's pick: never a light of the base, never one counted twice)
        std::vector<uintptr_t> listed;
        for (const KeptLamp& kl : k.lamps) listed.push_back(kl.light);
        const auto pick = [&](uintptr_t light, float row) {
            GpuLampLaw::Lamp w;
            int type = 0, reg = -1;
            // (a lamp switched off has a dark law: nothing, as in its maps, whatever the step reads then)
            if (!RtOutdoorLamps::WorldLaw(light, k.lot, false, w, type, reg) || !(w.W > 0.0f) || !((w.colour[0] + w.colour[1]) + w.colour[2] > 0.0f) ||
                !PhysLampPick::Meets(box, w.pos, PhysLampPick::Reach2(w, row, k.thr)))
                return;
            PhysBasisLamp& l = r.lamps.emplace_back();
            l.light = light, l.row = row;
            std::memcpy(l.world, w.pos, sizeof l.world);
        };
        const auto freshRow = [&](const LotLamp& L) { return PhysLampPick::NormKFresh(L.story, L.home, L.homeNorm, k.story, k.id, k.norm, k.outdoor); };
        for (const KeptLamp& kl : k.lamps) {
            if (std::find(gone.begin(), gone.end(), kl.light) != gone.end()) continue; // (deleted: its light may be freed)
            const auto it = at.find(kl.light);
            pick(kl.light, it != at.end() && it->second->home != kl.home ? freshRow(*it->second) : static_cast<float>(kl.mult) * kl.normK);
        }
        for (const LotLamp& L : lot)
            if (PhysLampPick::UseOf(L.light, listed, k.baked) == PhysLampPick::Use::Fresh) pick(L.light, freshRow(L));
        // A table dragged with its lamp moves its record here too, by gpu_room_lamps.cpp's rule (the record a listed lamp stood on
        // at the solve, its own object moved lately, the lamp kept its height), kept until the next solve: without it the furniture
        // and stairs took the table's shadow from where the solve read it for the whole drag
        try {
            if (g_basisCarried.size() > 4096) g_basisCarried.clear();
            auto& carried = g_basisCarried[key];
            if (carried.first != k.basisId) carried = {k.basisId, {}};
            for (const KeptLamp& kl : k.lamps) {
                const auto l = std::find_if(r.lamps.begin(), r.lamps.end(), [&](const PhysBasisLamp& x) { return x.light == kl.light; });
                if (l == r.lamps.end()) continue;
                const float dx = l->world[0] - kl.pos[0], dy = l->world[1] - kl.pos[1], dz = l->world[2] - kl.pos[2];
                if (dx * dx + dz * dz < 1e-8f || std::fabs(dy) > 0.02f) continue;
                for (size_t i = 0; i < k.tests.rec.size(); i++) {
                    if (!RoomLightTests::StandsOn(k.tests.rec[i], kl.pos)) continue;
                    const auto o = std::find_if(carried.second.begin(), carried.second.end(), [&](const std::array<float, 3>& c) { return c[0] == static_cast<float>(i); });
                    const uintptr_t shape = k.tests.rec[i].shape; // (carried already: it follows between its object's marks, as the GPU maps' carry)
                    if (!LampMarkFilter::ShapeMoved(shape) && !(o != carried.second.end() && LampMarkFilter::ShapeMovedWithin(shape, std::min(LampMarkFilter::kCarryStickMs, GpuRoomLamps::PhysDragMs())))) continue;
                    if (o == carried.second.end()) carried.second.push_back({static_cast<float>(i), dx, dz});
                    else (*o)[1] = dx, (*o)[2] = dz;
                }
            }
            if (!carried.second.empty()) {
                auto t = std::make_shared<RoomLightTests::Room>(k.tests);
                uint64_t h = 1469598103934665603ull;
                for (const auto& c : carried.second) {
                    RoomLightTests::Record& rc = t->rec[static_cast<size_t>(c[0])];
                    rc.c.x += c[1], rc.c.z += c[2], rc.off[0] = c[1], rc.off[1] = c[2];
                    for (size_t b = 0; b < sizeof c; b++) h = (h ^ reinterpret_cast<const uint8_t*>(c.data())[b]) * 1099511628211ull;
                }
                r.tests = std::move(t), r.offKey = h | 1;
            }
        } catch (...) {
            r.tests.reset(), r.offKey = 0;
        }
        // the room is written when an edited lamp lights it now or lit it in its last write
        const BasisNow& bn = NowOf(key, k);
        std::vector<uintptr_t> touched;
        for (auto& [light, e] : job->edits) {
            const auto has = [&](uintptr_t l) { return l == light; };
            const bool now = std::any_of(r.lamps.begin(), r.lamps.end(), [&](const PhysBasisLamp& l) { return l.light == light; });
            const bool before = bn.phys ? std::any_of(bn.drawn.begin(), bn.drawn.end(), has)
                                        : std::any_of(k.basisLamps.begin(), k.basisLamps.end(), [&](const KeptRoom::BasisLamp& b) { return b.light == light; });
            if (now || before) e.rooms.push_back(r.ref), touched.push_back(light);
        }
        if (touched.empty() && !r.redo) continue;
        const auto refuse = [&](const char* w) {
            for (const uintptr_t light : touched) job->why[light] += std::format("{}story {} room {}: {}", job->why[light].empty() ? "" : ", ", k.story, k.id, w);
        };
        if (k.basisWhy) {
            refuse(k.basisWhy);
            continue;
        }
        if (!job->lot) {
            refuse("its occluders with open tops are not uploaded yet");
            continue;
        }
        float xf[16];
        if (!RtOutdoorLamps::LotXform(k.mgr, xf)) {
            refuse("the lot's space could not be read");
            continue;
        }
        const auto toLot = [&](const float* p, float* o) {
            for (int q = 0; q < 3; q++) o[q] = p[0] * xf[q] + p[1] * xf[4 + q] + p[2] * xf[8 + q] + xf[12 + q];
        };
        bool ok = true;
        try {
            r.lotPos.resize(n * 3);
            for (size_t i = 0; i < n; i++) toLot(k.basis[i].pos, &r.lotPos[i * 3]);
            for (PhysBasisLamp& l : r.lamps) {
                toLot(l.world, l.lot);
                // A moved lamp's part as the room's solve will add it at this place, by the list of the room's last gather: the
                // furniture's tone during a drag is then the one after it. Modelled, it stepped -23% and +27% at the keep in story 1
                // room 2 (a lamp through its arch: the share depends on the place). Only a lamp the list holds,
                // indoors on the room's story: for any other the solve's own classes (made at the gather) do not describe it. A still
                // lamp takes it too where its kept pass is unknown at a lit sample
                const auto kl = std::find_if(k.lamps.begin(), k.lamps.end(), [&](const KeptLamp& x) { return x.light == l.light; });
                const auto bl = std::find_if(k.basisLamps.begin(), k.basisLamps.end(), [&](const KeptRoom::BasisLamp& b) { return b.light == l.light; });
                const float dx = kl != k.lamps.end() ? l.world[0] - kl->pos[0] : 1.0f, dy = kl != k.lamps.end() ? l.world[1] - kl->pos[1] : 0.0f,
                            dz = kl != k.lamps.end() ? l.world[2] - kl->pos[2] : 0.0f;
                const bool still = bl != k.basisLamps.end() && bl->pass.size() == n && dx * dx + dy * dy + dz * dz < 0.005f * 0.005f;
                const auto stepAll = [&] {
                    l.step.assign(n * 16, 0.0f);
                    for (size_t i = 0; i < n && ok; i++) ok = LevelLightShare::BasisStep(k.basis[i].pos, l.light, &l.step[i * 16]);
                };
                l.model = !still;
                if (still) {
                    stepAll();
                    for (size_t i = 0; i < n && ok && !l.model; i++) l.model = !std::isfinite(bl->pass[i]) && StepLit(&l.step[i * 16]);
                }
                const auto L = at.find(l.light);
                if (ok && l.model && (still || (L != at.end() && L->second->story == k.story && L->second->home > 0)) && RoomSet(k.room) == k.basisSet) {
                    uintptr_t list[64];
                    const int listed = ReadList(reinterpret_cast<const BYTE*>(k.room), list, static_cast<int>(std::size(list)));
                    const long mult = listed > 0 ? static_cast<long>(std::count(list, list + listed, l.light)) : 0;
                    if (mult > 0) {
                        LARGE_INTEGER p0{}, p1{}, pf{};
                        QueryPerformanceCounter(&p0);
                        l.part.assign(n * 16, 0.0f);
                        float one[16];
                        for (size_t i = 0; i < n; i++) {
                            if (!LevelLightShare::BasisPartNow(reinterpret_cast<void*>(k.room), k.basis[i].pos, l.light, one)) {
                                l.part.clear();
                                break;
                            }
                            for (int q = 0; q < 16; q++) l.part[i * 16 + q] = static_cast<float>(mult) * one[q];
                        }
                        QueryPerformanceCounter(&p1), QueryPerformanceFrequency(&pf);
                        job->partMs += 1000.0 * static_cast<double>(p1.QuadPart - p0.QuadPart) / static_cast<double>(pf.QuadPart);
                        l.model = l.part.empty();
                    }
                }
                // (a lamp the room does not list, standing where its modelled part was last written: that part, no worker)
                if (!still && l.model)
                    if (const auto m = g_modelParts.find(std::make_pair(key, l.light));
                        m != g_modelParts.end() && m->second.basisId == k.basisId && std::equal(l.world, l.world + 3, m->second.world.begin()) && m->second.part.size() == n * 16)
                        l.part = m->second.part, l.model = false;
                if (!still && l.model) stepAll(); // (the step and the worker's shares are for the model)
            }
        } catch (...) {
            refuse("no memory");
            continue;
        }
        if (!ok) {
            refuse("the game's step could not be called");
            continue;
        }
        job->rooms.push_back(std::move(r));
    }
    // A room none of whose lamps needs the worker is written now, in the update of the edit: through the worker the furniture followed
    // the lamp 40-100 ms behind the floors); a room just kept goes through it (its scale is learnt there)
    if (LevelLightShare::RoomMapsFree()) {
        std::vector<PhysBasisRoom> now;
        const auto direct = [](const PhysBasisRoom& r) {
            return !r.redo && NowOf(r.ref.key, *r.k).phys && std::none_of(r.lamps.begin(), r.lamps.end(), [](const PhysBasisLamp& l) { return l.model; });
        };
        for (PhysBasisRoom& r : job->rooms)
            if (direct(r)) {
                for (PhysBasisLamp& l : r.lamps)
                    if (l.part.empty()) l.vis.assign(r.k->basis.size(), 0.0f); // (the kept passes alone)
                now.push_back(std::move(r));
            }
        std::erase_if(job->rooms, [](const PhysBasisRoom& r) { return !r.k; });
        WritePhysRooms(now, nullptr, job->wroteNow, job->failedNow);
    }
    if (job->rooms.empty()) { // (nothing for the worker: the edits' log now)
        LogPhysBasis(*job, job->wroteNow, job->failedNow);
        return;
    }
    std::lock_guard<std::mutex> lk(g_pbMx);
    if (!g_pbThread) {
        g_pbThread = true;
        std::thread(PhysBasisWorker).detach();
    }
    g_pbIn = std::move(job);
    g_pbBusy = true;
    g_pbCv.notify_one();
}
// The finished job's rooms packed and written (light tree thread, the working maps free)
void WritePhysRooms(const std::vector<PhysBasisRoom>& rooms, const char* jobFailed, std::map<uint64_t, std::pair<long, long>>& wrote, std::map<uint64_t, const char*>& failed) {
    for (const PhysBasisRoom& r : rooms) {
        const uint64_t key = r.ref.key;
        if (jobFailed) {
            failed[key] = jobFailed;
            continue;
        }
        {
            std::lock_guard<std::mutex> lk(g_keptMx);
            const auto it = g_kept.find(key);
            if (it == g_kept.end() || it->second != r.k) {
                failed[key] = it == g_kept.end() ? "its kept solve was dropped" : "its solve was kept again meanwhile (written again from it)";
                if (it != g_kept.end()) g_pbRedo.insert(key);
                continue;
            }
        }
        const KeptRoom& k = *r.k;
        const size_t n = k.basis.size();
        std::vector<float> acc(k.basisBase);
        // (light tree thread) per kept room and lamp, the game's over the physical levels when it last stood still
        static std::unordered_map<uint64_t, std::unordered_map<uintptr_t, float>> s_scale;
        if (s_scale.size() > 4096) s_scale.clear();
        auto& scales = s_scale[key];
        for (const PhysBasisLamp& l : r.lamps) {
            const auto bl = std::find_if(k.basisLamps.begin(), k.basisLamps.end(), [&](const KeptRoom::BasisLamp& b) { return b.light == l.light; });
            const auto kl = std::find_if(k.lamps.begin(), k.lamps.end(), [&](const KeptLamp& x) { return x.light == l.light; });
            const float dx = kl != k.lamps.end() ? l.world[0] - kl->pos[0] : 1.0f, dy = kl != k.lamps.end() ? l.world[1] - kl->pos[1] : 0.0f,
                        dz = kl != k.lamps.end() ? l.world[2] - kl->pos[2] : 0.0f;
            const bool still = bl != k.basisLamps.end() && bl->pass.size() == n && dx * dx + dy * dy + dz * dz < 0.005f * 0.005f;
            const auto sc = scales.find(l.light);
            double game = 0.0, phys = 0.0;
            if (l.part.size() == acc.size()) {
                for (size_t q = 0; q < acc.size(); q++) acc[q] += l.part[q];
                continue;
            }
            if (!still) {
                std::vector<float> part(acc.size(), 0.0f);
                BasisLampAdd(false, nullptr, 1, l.row, l.vis.data(), RoomLightTests::K_DROP, l.step.data(), n, sc != scales.end() ? sc->second : 1.0f, part.data(), game, phys, l.pane.size() == n ? l.pane.data() : nullptr);
                for (size_t q = 0; q < acc.size(); q++) acc[q] += part[q];
                if (g_modelParts.size() > 256) g_modelParts.clear(); // (32-bit address space; a few rooms' lamps in use)
                g_modelParts[std::make_pair(key, l.light)] = ModelPart{k.basisId, {l.world[0], l.world[1], l.world[2]}, std::move(part)};
                continue;
            }
            BasisLampAdd(still, bl->pass.data(), bl->mult, l.row, l.vis.data(), RoomLightTests::K_DROP, l.step.data(), n,
                         sc != scales.end() ? sc->second : 1.0f, acc.data(), game, phys, l.pane.size() == n ? l.pane.data() : nullptr);
            if (still && phys > 1e-6) scales[l.light] = static_cast<float>(std::clamp(game / phys, 0.25, 4.0));
        }
        long w = 0, left = 0;
        if (const char* f = WriteBasisAcc(key, k, acc, w, left)) {
            failed[key] = f;
            continue;
        }
        wrote[key] = {w, left};
        BasisNow& bn = NowOf(key, k);
        bn.phys = true, bn.drawn.clear(), bn.drawnAt.clear(), bn.carry.clear(), bn.drawnTests = r.tests;
        for (const PhysBasisLamp& l : r.lamps) bn.drawn.push_back(l.light), bn.drawnAt.push_back({l.world[0], l.world[1], l.world[2]});
    }
}
void WritePhysBasis(PhysBasisJob& job) {
    WritePhysRooms(job.rooms, job.failed, job.wroteNow, job.failedNow);
    LogPhysBasis(job, job.wroteNow, job.failedNow);
}
void LogPhysBasis(const PhysBasisJob& job, const std::map<uint64_t, std::pair<long, long>>& wrote, std::map<uint64_t, const char*>& failed) {
    if (!KeptLog()) return;
    LARGE_INTEGER t1{}, f{};
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&f);
    long lamps = 0, redo = 0, redone = 0;
    size_t traced = 0, lit = 0; // (lit: every lamp's samples)
    for (const PhysBasisRoom& r : job.rooms) {
        lamps += static_cast<long>(r.lamps.size()), traced += r.traced;
        for (const PhysBasisLamp& l : r.lamps) lit += l.vis.size();
        redo += r.redo ? 1 : 0, redone += r.redo && wrote.count(r.ref.key) ? 1 : 0;
    }
    const std::string worker = std::format("; {:.2f} ms of lamps' parts, {} rooms written at once", job.partMs, job.wroteNow.size()) +
                               (job.rooms.empty() ? std::string()
                                                  : std::format("; {} lamps' shares in {} rooms in {:.1f} ms on a worker ({} of {} samples traced, the others kept)", lamps, job.rooms.size(), job.workerMs, traced, lit));
    int unmet = 0; // (edits whose lamp lit no kept room's directional samples: no write, and no line of their own)
    const char* unmetKind = "";
    for (const auto& [light, e] : job.edits) {
        int done = 0;
        long w = 0, l = 0;
        std::string why = job.why.count(light) ? job.why.at(light) : std::string();
        for (const BasisRoomRef& r : e.rooms) {
            if (const auto it = wrote.find(r.key); it != wrote.end()) done++, w += it->second.first, l += it->second.second;
            else if (const auto fw = failed.find(r.key); fw != failed.end())
                why += std::format("{}story {} room {}: {}", why.empty() ? "" : ", ", r.story, r.id, fw->second);
        }
        if (e.rooms.empty()) {
            unmet++, unmetKind = e.kind;
            continue;
        }
        LOG_INFO(std::format("[GpuRoomLamps] physical {}: lamp {:08X} directional maps written in {} of {} rooms {:.1f} ms after the edit ({} texels{}{}){}", e.kind, light,
                             done, e.rooms.size(), 1000.0 * static_cast<double>(t1.QuadPart - e.at.QuadPart) / static_cast<double>(f.QuadPart), w,
                             l ? std::format(", {} left as another solve wrote them", l) : std::string(), worker,
                             why.empty() ? std::string() : std::format("; not written: {} (they follow its rooms' solves)", why)));
    }
    if (unmet)
        LOG_INFO(std::format("[GpuRoomLamps] physical {}: {} lamp edits reach no directional sample of the {} kept rooms with directional maps: none written", unmetKind, unmet, job.basisRooms));
    if (redo)
        LOG_INFO(std::format("[GpuRoomLamps] physical: directional maps written again in {} of {} rooms after their kept solve{}", redone, redo, worker));
}
void BasisDuePhys() {
    RegatherRigTail();
    if (LevelLightShare::RoomMapsFree()) WritePhysCarry();
    const DWORD now = GetTickCount();
    if (g_pbBusy && !g_pbDone) {
        std::lock_guard<std::mutex> lk(g_pbMx);
        g_pbDone = std::move(g_pbOut);
        if (g_pbDone) g_pbDone->doneTick = now;
    }
    if (g_pbDone) {
        if (!LevelLightShare::RoomMapsFree()) { // (a solve between its lock and unlock: the next update, at most 1 s)
            if (now - g_pbDone->doneTick <= 1000) return;
            for (PhysBasisRoom& r : g_pbDone->rooms) {
                if (r.redo) g_pbRedo.insert(r.ref.key); // (its kept solve's texels are the game's: tried again with the next job)
                for (const auto& [light, e] : g_pbDone->edits)
                    if (std::any_of(e.rooms.begin(), e.rooms.end(), [&](const BasisRoomRef& x) { return x.key == r.ref.key; }))
                        g_pbDone->why[light] += std::format("{}story {} room {}: another room's maps stayed locked for 1 s", g_pbDone->why[light].empty() ? "" : ", ",
                                                            r.ref.story, r.ref.id);
            }
            g_pbDone->rooms.clear();
            LogPhysBasis(*g_pbDone, g_pbDone->wroteNow, g_pbDone->failedNow);
            for (const auto& [light, e] : g_pbDone->edits) g_basisEdits.try_emplace(light, e); // (a plain move has no settle: not lost)
        } else
            WritePhysBasis(*g_pbDone);
        g_pbDone.reset();
        g_pbBusy = false;
    }
    if (g_pbBusy || (g_basisEdits.empty() && g_pbRedo.empty())) return;
    // (the edits stay coalesced for the first free update: the newest place, written at once where it can be)
    if (!LevelLightShare::RoomMapsFree()) return;
    ReadTestConsts();
    std::map<uintptr_t, BasisEdit> edits = std::move(g_basisEdits);
    g_basisEdits.clear();
    StartPhysBasis(std::move(edits));
}

// ---- (physical) the instant day/night switch (gpu-room-lamps.md "Day and night switch"; light tree thread) ----
// As the level leaves an end, each room's kept solve made wholly there before the mode change joins that end's cache, and the
// rooms with no cached solve of the other end are sent to their solve at once. At the quick switch's arrival each room of the
// active lot takes back its cached solve of the new end when it still holds: the same object put back in g_kept (the GPU draws
// it as kept, its maps first as a changed signature), its ambient pair written as RetakeAmbient writes, its directional maps
// rewritten from it (the carry, then the physical redo). Any doubt: the room waits for its solve. The game's solves still run
// and take over at their KeepEnd.
namespace {
constexpr int kSwapsPerUpdate = 4; // (13 rooms in 4 room updates; each swap's maps are made again on the render thread, as a solve's keep)
constexpr DWORD kSwitchLogMs = 10000, kRetryMs = 250;
uint32_t g_leftSeen[2] = {}, g_departuresSeen = 0; // the clock's departures handled
bool SameTexels(const KeptRoom& a, const KeptRoom& b) {
    return a.texels.size() == b.texels.size() && a.samples.size() == b.samples.size() && std::equal(std::begin(a.mapW), std::end(a.mapW), std::begin(b.mapW)) &&
           std::equal(std::begin(a.mapH), std::end(a.mapH), std::begin(b.mapH)) && std::memcmp(a.samples.data(), b.samples.data(), a.samples.size()) == 0 &&
           std::equal(a.texels.begin(), a.texels.end(), b.texels.begin(), [](const TexelRec& p, const TexelRec& q) { return p.x == q.x && p.y == q.y && p.map == q.map; });
}
// Every lamp the room's solve shown now lists and the cached one does not is off now: the lists differ by end where lamps
// switch with the hour (a switched-off lamp leaves the list at the gather); one lit now that the cached solve lacks: its solve
bool ExtraLampsOff(const KeptRoom& cand, const KeptRoom& cur) {
    for (const KeptLamp& l : cur.lamps) {
        if (std::any_of(cand.lamps.begin(), cand.lamps.end(), [&](const KeptLamp& c) { return c.light == l.light; })) continue;
        KeptLamp now;
        if (LawOfLight(l.light, cand.lot, now) && now.law.W > 0.0f && (now.law.colour[0] > 0.0f || now.law.colour[1] > 0.0f || now.law.colour[2] > 0.0f)) return false;
    }
    return true;
}
// Every light of the cached solve with a law is now as it was kept: there, the same shape, colour (0 while off) and strength. A
// dusk switch back as it was holds; any other lamp change sends the room to its solve
bool LampsAsKept(const KeptRoom& k) {
    const auto same = [&](const KeptLamp& kl) {
        KeptLamp now;
        if (!LawOfLight(kl.light, k.lot, now) || now.colourOnly != kl.colourOnly) return false;
        if (kl.colourOnly ? now.geo != kl.geo : GpuLampLaw::Moved(now.law, kl.law, 1e-4f)) return false;
        const auto eq = [](float a, float b) { return std::fabs(a - b) <= 1e-4f * std::max(1.0f, std::fabs(b)); };
        return eq(now.law.W, kl.law.W) && eq(now.law.colour[0], kl.law.colour[0]) && eq(now.law.colour[1], kl.law.colour[1]) && eq(now.law.colour[2], kl.law.colour[2]);
    };
    return std::all_of(k.lamps.begin(), k.lamps.end(), same) && std::all_of(k.bakedLaws.begin(), k.bakedLaws.end(), same);
}
// One room of the switch: swapped (null), or why it waits for its solve
const char* SwapRoom(uint64_t key, DWORD now) {
    PhaseSwitch& sw = g_phaseSwitch;
    // (a kept solve of a class the room does not show now draws nothing: neither swapped nor counted)
    if (RoomRead now0; ReadRoom(reinterpret_cast<const BYTE*>(static_cast<uintptr_t>(key >> 2)), now0) && now0.cls != static_cast<int>(key & 3))
        return sw.rooms.erase(key), "not the class it shows";
    std::shared_ptr<const KeptRoom> cand, cur;
    PhaseCache::Facts f;
    f.switchFrom = sw.from;
    {
        std::lock_guard<std::mutex> lk(g_keptMx);
        if (const auto s = g_phaseKept.find(key); s != g_phaseKept.end() && s->second.at[sw.to]) cand = s->second.at[sw.to], f.candStart = s->second.start[sw.to];
        if (const auto it = g_kept.find(key); it != g_kept.end()) cur = it->second;
        f.solving = std::any_of(g_heldKept.begin(), g_heldKept.end(), [&](const auto& h) { return h.first == key; });
    }
    if (cand && cand == cur) { // (still kept: nothing to put back; the game's own solve when it began after the departure)
        const bool game = static_cast<int32_t>(cur->start.tick - sw.from) >= 0;
        sw.shown.try_emplace(key, game ? cur->keptAt : now, !game);
        return game ? "its solve since the switch landed" : nullptr;
    }
    f.cand = cand != nullptr, f.cur = cur != nullptr;
    if (cur) f.curStart = cur->start.tick;
    if (cand && cur) {
        BYTE* room = reinterpret_cast<BYTE*>(cand->room);
        RoomRead rr;
        f.solving = f.solving || (g_cap.active && g_cap.room == room);
        f.sameLot = cand->lot == cur->lot && cand->lot == ActiveLot() && ReadRoom(room, rr) && rr.story == cand->story && rr.id == cand->id && rr.cls == cand->cls;
        f.lastEdit = static_cast<uint32_t>(LampMarkFilter::EditLastTick());
        // (cheap first: the solves' keys, then the room read now)
        f.sameGeo = cand->geo && cand->geo == cur->geo && cand->roomGeo == cur->roomGeo && f.sameLot && RoomGeoKey(room) == cand->geo;
        f.sameTexels = SameTexels(*cand, *cur);
        f.sameLamps = ExtraLampsOff(*cand, *cur);
        f.lampsAsKept = LampsAsKept(*cand);
        const auto bn = g_basisNow.find(key);
        f.basisOk = (cand->basisWhy == kNoBasis && cur->basisWhy == kNoBasis) ||
                    (!cand->basisWhy && !cur->basisWhy && cand->basisSet && cand->basisSet == cur->basisSet && RoomSet(cand->room) == cand->basisSet && SameBasisSamples(*cur, *cand) &&
                     cand->basisGame.size() == cand->basis.size() * 4 && cur->basisGame.size() == cand->basisGame.size() &&
                     (bn == g_basisNow.end() || (bn->second.id == cur->basisId && bn->second.expect.size() == cand->basisGame.size())));
    }
    if (const char* why = PhaseCache::SwapWhy(f)) {
        if (why == PhaseCache::kLanded) sw.shown.try_emplace(key, cur->keptAt, false); // (solved before its turn here: shown, not "not solved")
        return why;
    }
    BYTE* room = reinterpret_cast<BYTE*>(cand->room);
    const char* why = nullptr;
    float was[2][4], wasOwn[4];
    if (!UnlitRooms::RestoreAmbient(room, cand->ambPair, cand->ambOwn, cand->ambCe, why, cur->ambOk ? cur->ambPair[0] : nullptr, cur->ambOk ? cur->ambOwn : nullptr, was,
                                    wasOwn))
        return why;
    bool same = false;
    {
        std::lock_guard<std::mutex> lk(g_keptMx);
        const auto it = g_kept.find(key);
        if ((same = it != g_kept.end() && it->second == cur)) PhaseCache::SwapIn(g_kept, g_keptBytes, key, cand);
    }
    if (!same) { // (a finalize on another thread meanwhile: its ambient goes back)
        UnlitRooms::RestoreAmbient(room, was, wasOwn, cand->ambCe, why, cand->ambPair[0], cand->ambOwn);
        return PhaseCache::kLanded;
    }
    // (its neighbours' stairwell shares read this room's own colour at their solves: the cached solve's)
    if (cand->ambLls) LevelLightShare::SetAmbientOwn(room, cand->ambOwn, cand->ambNorm);
    // its directional maps: the texels holding what the room showed take the cached solve's own values (WritePhysCarry), and a
    // room written physically is written again from it (the redo)
    if (cand->basisWhy != kNoBasis) {
        const auto bn = g_basisNow.find(key);
        const bool phys = bn != g_basisNow.end() && bn->second.phys;
        std::vector<uint32_t> expect = bn != g_basisNow.end() ? std::move(bn->second.expect) : cur->basisGame;
        BasisNow& b = g_basisNow[key];
        b = BasisNow{};
        b.id = cand->basisId, b.phys = phys, b.expect = std::move(expect), b.carry = cand->basisGame;
        if (phys) g_pbRedo.insert(key);
    }
    sw.shown.try_emplace(key, now, true);
    return nullptr;
}
void LogPhaseSwitch(DWORD now) {
    PhaseSwitch& s = g_phaseSwitch;
    s.open = false;
    int swapped = 0, solved = 0, solvedSent = 0, never = 0;
    DWORD lastSwap = 0, lastSolve = 0;
    std::map<std::string, int> whySolved, whyNever;
    for (const uint64_t key : s.rooms) {
        const auto w = s.why.find(key);
        const char* why = w != s.why.end() ? w->second : "not tried within the summary's time";
        const auto it = s.shown.find(key);
        if (it == s.shown.end()) {
            never++, whyNever[why]++;
            continue;
        }
        const DWORD t = it->second.first - s.from;
        if (it->second.second) swapped++, lastSwap = std::max(lastSwap, t);
        else solved++, solvedSent += s.sent.count(key) ? 1 : 0, lastSolve = std::max(lastSolve, t), whySolved[why]++;
    }
    const auto list = [](const std::map<std::string, int>& m) {
        std::string o;
        for (const auto& [w, n] : m) o += std::format("{}{} x{}", o.empty() ? "" : ", ", w, n);
        return o.empty() ? std::string("-") : o;
    };
    LOG_INFO(std::format("[GpuRoomLamps] Day/night switch to {}: {} rooms; swapped from the cache {} (last +{:.2f} s); waited, solved by the game {} (last +{:.2f} s; {} sent by "
                         "Apex at the switch's start); not solved within {:.1f} s {}; last room +{:.2f} s; {} swapped rooms solved again since; why solved: {}; why not "
                         "solved: {}",
                         s.to == PhaseCache::kNight ? "night" : "day", s.rooms.size(), swapped, lastSwap / 1000.0, solved, lastSolve / 1000.0, solvedSent,
                         (now - s.from) / 1000.0, never, std::max(lastSwap, lastSolve) / 1000.0, s.solvedAfter.size(), list(whySolved), list(whyNever)));
}
// The level left end e (mode: its mode change): e's late candidates go, each room's kept solve made wholly at e joins e's cache,
// and the rooms with no cached solve of the other end are sent to their solve (the shown class first)
void PhaseDeparture(PhaseCache::End e, uint32_t mode, bool missed, uint32_t stay, DWORD now) {
    const PhaseCache::End o = e == PhaseCache::kDay ? PhaseCache::kNight : PhaseCache::kDay;
    const uint64_t lot = ActiveLot();
    int dropped = 0, filled = 0, cached = 0, rooms = 0;
    std::vector<std::shared_ptr<const KeptRoom>> send;
    std::string notCached; // (story S room R: why) for each shown room of the lot not in e's cache now
    const auto note = [&](int story, int id, const char* why) { notCached += std::format("{}story {} room {} ({})", notCached.empty() ? "" : ", ", story, id, why); };
    std::set<const void*> keptRooms;
    {
        std::lock_guard<std::mutex> lk(g_keptMx);
        dropped = PhaseCache::PurgeDeparture(g_phaseKept, e, mode, missed);
        for (const auto& [key, k] : g_kept) {
            if (k->lot != lot) continue;
            RoomRead rr;
            if (!ReadRoom(reinterpret_cast<const BYTE*>(k->room), rr) || rr.cls != k->cls) continue; // (a class it does not show)
            keptRooms.insert(reinterpret_cast<const void*>(k->room)), rooms++;
            const char* why = missed                ? "a departure missed"
                              : !k->physical        ? "not physical"
                              : k->tailWhy          ? "no tail kept"
                              : !k->editable        ? "not drawn whole (a class-2 wall left to the game's blur)"
                              : !k->ambOk           ? "its ambient is not its solve's (an atrium's colour)"
                                                    : PhaseCache::FillWhy(k->start, k->keptStamp, e, stay, mode);
            auto& slot = g_phaseKept[key];
            if (!why && slot.at[e] != k) slot.at[e] = k, slot.keptAt[e] = k->keptAt, slot.start[e] = k->start.tick, filled++;
            if (slot.at[e]) cached++;
            else note(k->story, k->id, why ? why : "?");
            // (a cached solve of the other end older than a player's lamp edit is refused at the swap: its solve now)
            if (k->physical && (!slot.at[o] || PhaseCache::EditSince(static_cast<uint32_t>(LampMarkFilter::EditLastTick()), slot.start[o]))) send.push_back(k);
            if (!slot.at[0] && !slot.at[1]) g_phaseKept.erase(key);
        }
    }
    // the lot's rooms with no kept solve at all (never kept, or dropped since)
    std::vector<void*> all;
    LevelLightShare::LotRooms(RtOutdoorLamps::ActiveTracker(), all);
    for (void* room : all)
        if (RoomRead rr; !keptRooms.count(room) && ReadRoom(static_cast<const BYTE*>(room), rr) && rr.id > 0 && rr.lot == lot) rooms++, note(rr.story, rr.id, "no kept solve");
    g_phaseSent.clear(), g_phaseResent.clear();
    // shortcut: the class the room shows stands for visible (class 2 first); the room queue spreads the solves at its budget
    std::stable_sort(send.begin(), send.end(), [](const auto& a, const auto& b) { return a->cls > b->cls; });
    int sent = 0;
    const uintptr_t tracker = RtOutdoorLamps::ActiveTracker();
    for (const auto& k : send)
        if (LevelLightShare::QueueRoomId(tracker, k->story, k->id, -1)) sent++, g_phaseSent[KeyOf(reinterpret_cast<const void*>(k->room), k->cls)] = now;
    if (rooms && KeptLog())
        LOG_INFO(std::format("[GpuRoomLamps] Day/night cache: the level left {}: {} of {} rooms cached for it ({} new), {} dropped (begun at or after the mode change, {} ms "
                             "before the departure), {} rooms with no cached {} solve sent to their solve; not cached: {}",
                             e ? "night" : "day", cached, rooms, filled, dropped, static_cast<int32_t>(now - mode), sent, o ? "night" : "day",
                             notCached.empty() ? std::string("-") : notCached));
}
} // namespace
void PhaseSwitchDue() {
    if (!g_capture.load(std::memory_order_relaxed) || !g_physical.load(std::memory_order_relaxed)) {
        std::lock_guard<std::mutex> lk(g_keptMx);
        g_phaseKept.clear();
        return;
    }
    if (g_keptClear.load(std::memory_order_relaxed)) return; // (the old world's rooms: cleared at the next solve)
    const PhaseCache::Clock clk = ClockNow();
    const DWORD now = GetTickCount();
    // the level left an end
    for (int e = 0; e < 2; e++) {
        if (clk.leftN[e] == g_leftSeen[e]) continue;
        const bool missed = clk.leftN[e] - g_leftSeen[e] > 1 || clk.departures - g_departuresSeen > 1;
        g_leftSeen[e] = clk.leftN[e];
        PhaseDeparture(static_cast<PhaseCache::End>(e), PhaseCache::ModeChange(clk.leftAt[e], static_cast<uint32_t>(LampMarkFilter::WindowBurstStart())), missed,
                       clk.departures - 1, now);
    }
    g_departuresSeen = clk.departures;
    PhaseSwitch& s = g_phaseSwitch;
    if (clk.switches != s.serial) { // a new quick switch: the active lot's kept physical rooms, in turn
        if (s.open) LogPhaseSwitch(now);
        s = PhaseSwitch{};
        s.serial = clk.switches, s.to = clk.target, s.from = clk.switchFrom, s.open = true;
        for (const auto& [key, at] : g_phaseSent) s.sent.insert(key);
        const uint64_t lot = ActiveLot();
        std::vector<std::shared_ptr<const KeptRoom>> ask; // (no cached solve of the end and not sent at the departure: a fade turned back)
        {
            std::lock_guard<std::mutex> lk(g_keptMx);
            for (const auto& [key, k] : g_kept)
                if (k->physical && k->lot == lot) {
                    s.rooms.insert(key), s.due.push_back(key);
                    const auto c = g_phaseKept.find(key);
                    if ((c == g_phaseKept.end() || !c->second.at[s.to]) && !g_phaseSent.count(key)) ask.push_back(k);
                }
        }
        for (const auto& k : ask)
            if (LevelLightShare::QueueRoomId(RtOutdoorLamps::ActiveTracker(), k->story, k->id, -1)) s.sent.insert(KeyOf(reinterpret_cast<const void*>(k->room), k->cls));
    }
    for (auto it = s.retry.begin(); it != s.retry.end();) // (a lamp's state not yet as kept: again every kRetryMs while the switch is open)
        if (now - it->second >= kRetryMs && !s.shown.count(it->first)) s.due.push_back(it->first), it = s.retry.erase(it);
        else if (s.shown.count(it->first)) it = s.retry.erase(it);
        else ++it;
    // (only while no solve holds the working maps, as the directional writes; the rooms wait for the next update)
    for (int n = 0; n < kSwapsPerUpdate && !s.due.empty() && LevelLightShare::RoomMapsFree();) {
        const uint64_t key = s.due.back();
        s.due.pop_back();
        if (const char* why = SwapRoom(key, now)) {
            if (!s.rooms.count(key)) continue;
            s.why[key] = why;
            if (why == PhaseCache::kLampState && s.open) s.retry.emplace_back(key, now);
            // refused for good: it shows the other end until a solve lands, so its solve is asked for now (once)
            if (RoomRead rr; PhaseCache::SendOnRefusal(why, now - s.from) && !s.sent.count(key) &&
                             ReadRoom(reinterpret_cast<const BYTE*>(static_cast<uintptr_t>(key >> 2)), rr) &&
                             LevelLightShare::QueueRoomId(RtOutdoorLamps::ActiveTracker(), rr.story, rr.id, -1))
                s.sent.insert(key);
        } else
            n++;
    }
    if (s.open && now - s.from >= kSwitchLogMs) LogPhaseSwitch(now);
}

namespace {
// (physical, stage 4) whether every story 0 .. 7 of the lot draws `light` from its law now (phys_lamp_pick.h EditRefusal): empty,
// or why not. Also the rooms > 0 listing it (their (story, id) and directional maps), whether any room lists it and the room it
// is registered in
struct PhysCover {
    std::vector<std::pair<int, int>> ids;
    std::vector<BasisRoomRef> basisRooms;
    bool listed = false;
    int reg = -1;
};
std::string PhysCoverWhy(uintptr_t tracker, uintptr_t light, PhysCover& o) {
    std::vector<void*> rooms;
    LevelLightShare::LotRooms(tracker, rooms);
    std::vector<PhysLampPick::RoomFact> facts;
    uint64_t lot = 0;
    uintptr_t raw[1024];
    for (void* room : rooms) {
        RoomRead rr;
        if (!ReadRoom(static_cast<const BYTE*>(room), rr)) return "a room of the lot could not be read";
        PhysLampPick::RoomFact f;
        f.story = rr.story, f.id = rr.id;
        const int n = ReadList(static_cast<const BYTE*>(room), raw, static_cast<int>(std::size(raw)));
        f.lists = n > 0 && std::find(raw, raw + n, light) != raw + n;
        o.listed = o.listed || f.lists;
        if (rr.story >= 0 && rr.story <= 7) {
            lot = rr.lot;
            if (rr.id == 0) f.drawn = RtOutdoorLamps::PhysCoversRoom0(room);
            else {
                std::shared_ptr<const KeptRoom> k;
                bool fits = true;
                {
                    std::lock_guard<std::mutex> lk(g_keptMx);
                    const uint64_t key = KeyOf(room, rr.cls);
                    if (const auto it = g_kept.find(key); it != g_kept.end()) k = it->second;
                    // (a solve the budget refused is refused again while the budget holds: 3 full solves a minute for nothing)
                    if (const auto it = g_keptRefused.find(key); it != g_keptRefused.end()) fits = g_keptBytes + it->second <= kMaxKeptBytes;
                }
                f.kept = k && k->physical;
                // (a room solved before its lot became active was never kept, and nothing else sends it again: every edit of a
                // lamp-edit log was refused for it; SolveUnkept's 3 sends a minute bound this)
                if (!f.kept && fits && rr.cls > 0 && rr.cls <= 2 && rr.lot == ActiveLot()) NoteUnkept(room);
                // (kept again, relit next frame: 42 refusals in one log; a fresh keep's maps are drawn by the next relight, so its first
                // second counts as drawn: refused steps 70-150 ms after a keep held a drag)
                f.drawn = f.kept && (!k->maps || GpuRoomLamps::DrawnAny(room, k->maps) || GetTickCount() - k->keptAt < 1000);
                f.baked = f.kept && std::find(k->baked.begin(), k->baked.end(), light) != k->baked.end();
                // (an object moved in the room, its occluder mark dropped as the lamp's own: the side table a lamp stands on, dragged
                // with it, kept its old blockers in the GPU's 3D test; the room's records follow the object before its solve). Its
                // solve takes them again, 3 a minute at most; the edit is taken meanwhile (refused, every edit of a session went to the solves)
                if (f.kept && k->roomGeo) {
                    uint64_t now[3] = {};
                    if (RoomGeoKey(static_cast<const BYTE*>(room), now) && PhysLampPick::BlockersKey(now) != k->roomGeo) NoteUnkept(room);
                }
                if (f.lists) o.ids.emplace_back(rr.story, rr.id), o.basisRooms.push_back(BasisRoomRef{KeyOf(room, rr.cls), rr.story, rr.id});
            }
        }
        facts.push_back(f);
    }
    GpuLampLaw::Lamp law;
    int type = 0;
    if (!RtOutdoorLamps::WorldLaw(light, lot, false, law, type, o.reg)) return "no GPU law (not a lamp the GPU draws)";
    int at = -1;
    if (const char* why = PhysLampPick::EditRefusal(facts, at)) return at >= 0 ? std::format("story {} room {}: {}", facts[at].story, facts[at].id, why) : std::string(why);
    return {};
}
} // namespace
bool TakesLampEditPhys(uintptr_t tl, uintptr_t light, const char* kind, bool settle, bool dry) {
    if (!g_capture.load(std::memory_order_relaxed) || !g_physical.load(std::memory_order_relaxed) || !tl || !light) return false;
    const auto refuse = [&](const std::string& why) {
        if (dry) return false;
        c_physRefused.fetch_add(1, std::memory_order_relaxed);
        // (a lamp of another lot: counted only; at dawn a neighbour's 100 street lamps used up the log's line budget)
        if (KeptLog() && why != "not a lamp of the active lot") LOG_INFO(std::format("[GpuRoomLamps] physical {}: lamp {:08X} left to the room solves ({})", kind, light, why));
        {
            std::string cls = why; // (the reason without the room it names)
            if (cls.rfind("story ", 0) == 0) cls = cls.substr(std::min(cls.size(), cls.find(": ") == std::string::npos ? 0 : cls.find(": ") + 2));
            std::lock_guard<std::mutex> lk(g_refuseMx);
            if (g_refuseWhy.size() < 64 || g_refuseWhy.count(cls)) g_refuseWhy[cls]++;
        }
        return false;
    };
    const uintptr_t tracker = TlTracker(tl);
    if (!tracker || tracker != RtOutdoorLamps::ActiveTracker()) return refuse("not a lamp of the active lot");
    // (a lamp of a basement: its rooms keep the game's solves and gather it only when marked)
    int story = -99;
    if (!CopyBytes(&story, reinterpret_cast<const void*>(tl + 0x1A0), sizeof story) || story < 0 || story > 7) return refuse("a lamp of a basement (its rooms keep the game's solves)");
    PhysCover o;
    if (const std::string why = PhysCoverWhy(tracker, light, o); !why.empty()) return refuse(why);
    if (dry) return true;
    c_physTaken.fetch_add(1, std::memory_order_relaxed);
    g_lastPhysEdit.store(std::max<DWORD>(GetTickCount(), 1), std::memory_order_relaxed);
    g_lotLampsDirty.store(true, std::memory_order_relaxed); // (its room or story as registered now, for the maps' rows)
    GpuRoomLamps::NotePhysEdit(light, kind);
    {
        std::lock_guard<std::mutex> lk(g_touchedMx);
        g_touched.NoteEdit(tracker, story, o.ids, o.reg);
    }
    const bool move = std::strncmp(kind, "move", 4) == 0;
    QueueBasis(light, kind, std::strcmp(kind, "switch off") == 0, move, o.basisRooms);
    // The game marks a room's rigs only for a lamp of that room: furniture of the rooms taking this lamp through an arch kept its old
    // rig until the 5 s sweep (the fridge lit 4.5 s after the lamp had left). Every 100 ms of a drag, those listing
    // it and every room of its story (one it comes into lists it only at its next gather: ObjectLightBridge CrossCandidates)
    g_rigTail.ids = o.ids, g_rigTail.tracker = tracker, g_rigTail.story = story, g_rigTail.due = true;
    if (!move) RegatherRigTail();
    // (a new lamp no room lists yet settles its own room; one a room lists already, first seen since the filter started, adds
    // nothing new to their bases)
    if (std::strcmp(kind, "new") == 0) settle = settle && !o.listed;
    if (settle) { // (its own room too: its normalisation and ambient take the lamp's light)
        if (o.reg > 0 && std::find(o.ids.begin(), o.ids.end(), std::make_pair(story, o.reg)) == o.ids.end()) o.ids.emplace_back(story, o.reg);
        Settle& s = g_settle[light];
        s.tracker = tracker, s.at = GetTickCount();
        for (const auto& id : o.ids)
            if (std::find(s.rooms.begin(), s.rooms.end(), id) == s.rooms.end()) s.rooms.push_back(id);
    } else if (const auto it = g_settle.find(light); it != g_settle.end()) {
        it->second.at = GetTickCount(); // (a drag's later steps: a lamp dragged into another room settles once the drag is over)
    }
    // (S8) every edit: the rooms listing it and its own room take their ambient again now, a settle or not
    for (const auto& [st, id] : o.ids) g_ambDue.emplace(std::make_tuple(tracker, st, id), kind);
    if (o.reg > 0) g_ambDue.emplace(std::make_tuple(tracker, story, o.reg), kind);
    g_reflectStory = story, g_reflectOwn = o.reg;
    return true;
}
bool PhysDrawnRoom(const void* room) {
    RoomRead rr;
    if (!room || !ReadRoom(static_cast<const BYTE*>(room), rr) || rr.cls < 0) return false;
    std::shared_ptr<const KeptRoom> k;
    {
        std::lock_guard<std::mutex> lk(g_keptMx);
        if (const auto it = g_kept.find(KeyOf(room, rr.cls)); it != g_kept.end()) k = it->second;
    }
    return k && k->physical && (!k->maps || GpuRoomLamps::DrawnAny(room, k->maps) || GetTickCount() - k->keptAt < 1000);
}
bool PhysCoversLamp(uintptr_t tracker, uintptr_t light) {
    if (!g_capture.load(std::memory_order_relaxed) || !g_physical.load(std::memory_order_relaxed) || !tracker || !light || tracker != RtOutdoorLamps::ActiveTracker()) return false;
    PhysCover o;
    return PhysCoverWhy(tracker, light, o).empty();
}
bool TakesLampEdit(uintptr_t tl, uintptr_t light, bool wasOn, bool on, bool move, bool dry) {
    if (!g_capture.load(std::memory_order_relaxed) || !tl || !light || (dry && move)) return false;
    const char* kind = move ? "move" : wasOn == on ? "value" : on ? "switch on" : "switch off";
    if (g_physical.load(std::memory_order_relaxed)) return TakesLampEditPhys(tl, light, kind, false, dry); // (LampMarkFilter takes a physical edit itself)
    std::vector<std::pair<uint64_t, MoveJob>> jobs; // (a move) one per kept room listing it
    std::string moveWhy;
    const uintptr_t tracker = TlTracker(tl);
    if (!tracker) return false;
    std::vector<void*> rooms;
    LevelLightShare::LotRooms(tracker, rooms);
    std::vector<std::pair<const void*, uint32_t>> kept; // the rooms (id > 0) listing it, with their kept solve's version
    std::vector<std::pair<int, int>> ids;               // the same rooms: (story, id)
    std::vector<BasisRoomRef> basisRooms;                // (stage 4) the same rooms, for their directional maps
    // a light without a GPU law (a tube): its share now (an intensity or shape edit changes it: such an edit takes the solves)
    float colourNow[3] = {};
    uint64_t geoNow = 0;
    bool haveGeo = false, geoRead = false;
    std::vector<int> stories;
    int carriedRooms = 0; // (a switch on) rooms whose kept solve holds the lamp's parts carried from an earlier one (CarryOff)
    uintptr_t raw[1024];
    const auto refuse = [&](const RoomRead& rr, const char* why) {
        if (KeptLog() && !dry)
            LOG_INFO(std::format("[GpuRoomLamps] {}: lamp {:08X} left to the room solves (story {} room {}: {})", kind, light, rr.story, rr.id, why));
        return false;
    };
    for (void* room : rooms) {
        const int n = ReadList(static_cast<const BYTE*>(room), raw, static_cast<int>(std::size(raw)));
        if (n <= 0 || std::find(raw, raw + n, light) == raw + n) continue;
        RoomRead rr;
        if (!ReadRoom(static_cast<const BYTE*>(room), rr)) return false;
        if (!geoRead) haveGeo = RtOutdoorLamps::ColourLaw(light, rr.lot, colourNow, geoNow), geoRead = true;
        if (rr.id == 0) {
            if (move) { // (stage 3b) its parts there redone on the move worker
                // (room 0 covered, and its kept record holding the lamp: MoveJobFor0, not the maps drawn. No slot is uploaded
                // during a drag, so a lamp that came into room 0's list mid-drag was in no drawn map: every step was refused and
                // sent room 0 to the game's solve, whose maps were not shown either (2026-11-02). Room 0's maps show the record's
                // parts once the drag ends, as for a lamp they already drew)
                if (!RtOutdoorLamps::CoversRoom(room)) return refuse(rr, "Real-time outdoor lamps does not cover room 0 now (a story's maps not relit)");
                MoveJob job;
                if (const char* why = MoveJobFor0(room, rr, light, tracker, job, moveWhy)) return refuse(rr, why);
                jobs.emplace_back(KeyOf(room, rr.cls), std::move(job));
                continue;
            }
            if (!RtOutdoorLamps::CoversLamp(room, light)) return refuse(rr, "a room 0 Real-time outdoor lamps does not draw it in (no law, or a floor mask part where its law is dark)");
            // (a tube kept there with another share than now: drawn as its solve gave it, so its edit is not shown)
            std::lock_guard<std::recursive_mutex> lk(g_mx);
            if (const auto it = g_records.find(KeyOf(room, rr.cls)); it != g_records.end())
                for (const KeptLamp& k0 : it->second->kept0)
                    if (k0.light == light && k0.colourOnly && on && (!haveGeo || k0.geo != geoNow)) return refuse(rr, "a light without a GPU law whose intensity or shape changed");
            continue;
        }
        std::shared_ptr<const KeptRoom> k;
        {
            std::lock_guard<std::mutex> lk(g_keptMx);
            if (const auto it = g_kept.find(KeyOf(room, rr.cls)); it != g_kept.end()) k = it->second;
        }
        if (!k) return refuse(rr, "no kept solve of its class");
        if (k->physical) return refuse(rr, "physical: lamp edits are not taken yet (the room's solve redraws it)");
        if (!k->editable) return refuse(rr, k->tailWhy ? k->tailWhy : "a wall texel the GPU does not blur");
        if (std::find(k->baked.begin(), k->baked.end(), light) != k->baked.end()) return refuse(rr, "its kept solve has the lamp in its base");
        for (const KeptLamp& kl : k->lamps) {
            if (kl.light == light && kl.colourOnly && on && (!haveGeo || kl.geo != geoNow)) return refuse(rr, "a light without a GPU law whose intensity or shape changed");
            carriedRooms += kl.light == light && kl.carried ? 1 : 0;
        }
        // (a copy a lamp move made counts as drawn while the GPU still draws the one it replaced)
        if (!GpuRoomLamps::Drawn(room, k->version, k->maps) && !(k->movedFrom && GpuRoomLamps::Drawn(room, k->movedFrom, k->maps)))
            return refuse(rr, "a map of it not drawn on the GPU");
        if (move) {
            MoveJob job;
            if (const char* why = MoveJobFor(room, rr, k, light, tracker, job, moveWhy)) return refuse(rr, why);
            jobs.emplace_back(KeyOf(room, rr.cls), std::move(job));
        }
        kept.emplace_back(room, k->version);
        ids.emplace_back(rr.story, rr.id);
        basisRooms.push_back(BasisRoomRef{KeyOf(room, rr.cls), rr.story, rr.id});
        if (std::find(stories.begin(), stories.end(), rr.story) == stories.end()) stories.push_back(rr.story);
    }
    if (kept.empty()) return false; // (no indoor room lists it: not this path's)
    std::sort(kept.begin(), kept.end());
    if (dry) { // (a switch on as below, without its log or erase)
        if (wasOn || !on) return true;
        const auto it = g_takenOff.find(light);
        return it != g_takenOff.end() && it->second == kept;
    }
    if (!wasOn && on) { // (while it was off, a room that gathered or was solved again does not hold it as its kept solve does)
        const auto it = g_takenOff.find(light);
        const bool same = it != g_takenOff.end() && it->second == kept;
        if (it != g_takenOff.end()) g_takenOff.erase(it);
        if (!same) {
            if (KeptLog()) LOG_INFO(std::format("[GpuRoomLamps] switch on: lamp {:08X} left to the room solves (its rooms changed since it was switched off)", light));
            return false;
        }
        if (carriedRooms && KeptLog())
            LOG_INFO(std::format("[GpuRoomLamps] switch on: lamp {:08X} drawn from its parts carried into {} of its {} rooms' later kept solves", light, carriedRooms, kept.size()));
    } else if (wasOn && !on) {
        if (g_takenOff.size() > 1024) g_takenOff.clear();
        g_takenOff[light] = kept;
    }
    std::sort(stories.begin(), stories.end());
    std::string st;
    for (const int s : stories) st += (st.empty() ? "" : ", ") + std::to_string(s);
    std::vector<const void*> list;
    for (const auto& [room, v] : kept) list.push_back(room);
    if (move) { // (the worker notes each room's edit when its parts are in)
        ReadTestConsts();
        for (auto& [key, job] : jobs) QueueMove(key, std::move(job));
    } else
        GpuRoomLamps::NoteEdit(light, kind, list, st);
    QueueBasis(light, kind, wasOn && !on, move, basisRooms); // (written at the end of this room update: BasisDue)
    // the settle: those rooms are solved once kSettleMs after the lamp's last edit taken (a drag's edits make one settle)
    Settle& s = g_settle[light];
    s.tracker = tracker, s.at = GetTickCount();
    for (const auto& id : ids)
        if (std::find(s.rooms.begin(), s.rooms.end(), id) == s.rooms.end()) s.rooms.push_back(id);
    return true;
}
// (S8, light tree thread, after each room update) The ambient of the rooms of the lamp edits taken: with the GPU drawing the lamps,
// a room's ambient (+0x110: what its walls and floors add by the light map's alpha, the furniture fill's hue, and the ceiling
// pass's copy that the kept compose scales by it, gpu_room_lamps.cpp) is the only part of Rooms at Night left as the last solve
// set it. The game's ambient step is run on each room now (UnlitRooms::RetakeAmbient: the lamps' share now plus the colour), so
// switching the last lamp off or on and any lamp edit change the room's background on the next frame without a solve. The
// settle still solves the switch and intensity edits' rooms for their normalisation (+0x160) and what stays in the kept base (a
// stairwell's bounce and fill); a room whose list holds a light gone from the lot's registry (a deleted lamp: its solve comes)
// is not run
namespace {
DWORD g_ambLogged = 0;
void AmbientDue() {
    if (g_ambDue.empty()) return;
    LARGE_INTEGER q0{}, q1{};
    QueryPerformanceCounter(&q0);
    std::map<uintptr_t, std::vector<uintptr_t>> reg; // (lot) its registered lights, sorted
    int ok = 0, normed = 0;
    std::string why;
    const char* kind = g_ambDue.begin()->second;
    uintptr_t raw[1024];
    for (const auto& [key, k] : g_ambDue)
        if (std::get<1>(key) == g_reflectStory) g_reflectOwed.insert(key);
    for (const auto& [key, k] : g_ambDue) {
        const auto [tracker, story, id] = key;
        const auto left = [&](const char* w) {
            if (why.size() < 400) why += std::format("; story {} room {}: {}", story, id, w);
        };
        auto [r, fresh] = reg.try_emplace(tracker);
        if (fresh) {
            std::vector<LevelLightShare::RegisteredLight> lights;
            if (LevelLightShare::RegisteredLights(tracker, lights))
                for (const LevelLightShare::RegisteredLight& l : lights) r->second.push_back(l.light);
            std::sort(r->second.begin(), r->second.end());
        }
        BYTE* room = static_cast<BYTE*>(const_cast<void*>(LevelLightShare::RoomOf(tracker, story, id)));
        const int n = room ? ReadList(room, raw, static_cast<int>(std::size(raw))) : -1;
        if (n <= 0) {
            left(room ? "an empty light list (no lamp on at its solve: the colour alone until the settle's solve gathers it)" : "gone");
            continue;
        }
        if (!std::all_of(raw, raw + n, [&](uintptr_t l) { return std::binary_search(r->second.begin(), r->second.end(), l); })) {
            left("its list holds a light gone from the lot's registry");
            continue;
        }
        float norm = 0.0f, normNow = 0.0f;
        const char* w = nullptr;
        if (!UnlitRooms::RetakeAmbient(room, norm, w)) {
            left(w);
            continue;
        }
        ok++;
        normed += CopyBytes(&normNow, room + 0x160, sizeof normNow) && std::fabs(norm - normNow) > 1e-5f * std::fabs(normNow) ? 1 : 0;
    }
    QueryPerformanceCounter(&q1);
    LARGE_INTEGER f{};
    QueryPerformanceFrequency(&f);
    if ((!why.empty() || GetTickCount() - g_ambLogged >= 1000) && KeptLog()) { // (a drag's steps: once a second)
        g_ambLogged = GetTickCount();
        LOG_INFO(std::format("[GpuRoomLamps] physical {}: ambient taken again in {} of {} rooms in {:.2f} ms, no room solved{}{}", kind, ok, g_ambDue.size(),
                             1000.0 * static_cast<double>(q1.QuadPart - q0.QuadPart) / static_cast<double>(f.QuadPart),
                             normed ? std::format(" ({} with another normalisation from their solve)", normed) : std::string(), why));
    }
    g_ambDue.clear();
}
// One room's reflection every 100 ms, in turn, at a step: a capture is about 7 stages of a frame's work. 120 ms after the last edit
// the rest, or a drop would leave a room's cube a step behind (SettleDue runs for every level of every lot: an empty g_ambDue says
// nothing about the drag, so the clock is the edits': sliders and moves to another room too)
void ReflectionTurn(bool step) {
    static DWORD s_at = 0;
    static size_t s_next = 0;
    static bool s_ownLast = false;
    const DWORD now = GetTickCount();
    const auto it = OutdoorDeltaModel::ReflectionPick(g_reflectOwed, s_next, s_ownLast, step, now - g_lastPhysEdit.load(std::memory_order_relaxed), now - s_at, g_reflectStory, g_reflectOwn);
    if (it == g_reflectOwed.end()) return;
    const auto [tracker, story, id] = *it;
    g_reflectOwed.erase(it);
    if (!LevelLightShare::TrackerLive(tracker)) return;
    if (const void* room = LevelLightShare::RoomOf(tracker, story, id); room && ReflectionDue(static_cast<const BYTE*>(room))) s_at = now;
}
} // namespace
// The settle (light tree thread, after each room update): a lamp edit taken leaves its rooms' normalisation (+0x160), light list
// and what their kept base holds besides the lamps (a stairwell's bounce and fill) as their last solve set them (the ambient is
// taken again at once: AmbientDue, unless refused there); kSettleMs after the lamp's last edit taken its rooms are sent
// to their solve once with no urgency (LevelLightShare::QueueRoomId, as a GPU edit's tail sends: no edit hold, no player
// edit), kept again as usual meanwhile drawn from their kept solve. Room 0s are not sent (Real-time outdoor lamps draws the
// lamp there and solves no room 0 for its edits either)
void SettleDue() {
    ReflectionPace();
    const DWORD now = GetTickCount();
    const bool step = !g_ambDue.empty();
    AmbientDue();
    ReflectionTurn(step);
    std::vector<const void*> requeue;
    {
        std::lock_guard<std::mutex> lk(g_requeueMx);
        requeue.swap(g_requeue);
    }
    for (const void* room : requeue) { // (physical) solved off the light tree thread: one solve here (its kept copy drawn meanwhile)
        RoomRead rr;
        const uintptr_t tracker = RtOutdoorLamps::TrackerOf(room);
        if (!tracker || !ReadRoom(static_cast<const BYTE*>(room), rr)) continue;
        const bool sent = LevelLightShare::QueueRoomId(tracker, rr.story, rr.id, -1);
        if (sent) g_physSent[room] = now | 1;
        if (KeptLog())
            LOG_INFO(std::format("[GpuRoomLamps] Story {} room {}: solved off the light tree thread, its kept physical copy stays drawn (the same geometry); {}", rr.story,
                                 rr.id, sent ? "sent to one solve here" : "could not be sent to a solve here"));
    }
    if (g_settle.empty()) return;
    // each room once for the lamps due together (a room sent for one is already waiting for its gather for the next: the
    // game's queue refuses it, which is not a failure)
    std::map<std::tuple<uintptr_t, int, int>, bool> sent; // (tracker, story, id) -> the send went through
    for (auto it = g_settle.begin(); it != g_settle.end();) {
        if (now - it->second.at < kSettleMs) {
            ++it;
            continue;
        }
        int ok = 0, before = 0, refused = 0, solved = 0;
        for (const auto& [story, id] : it->second.rooms) {
            const auto key = std::make_tuple(it->second.tracker, story, id);
            if (const auto s = sent.find(key); s != sent.end()) {
                before++;
                continue;
            }
            // (a solve begun after the edit gathered it: nothing left to settle; a day/night switch's solves, mostly)
            if (const void* r = LevelLightShare::RoomOf(it->second.tracker, story, id)) {
                std::lock_guard<std::mutex> lk(g_keptMx);
                bool done = false;
                for (int cls = 0; cls < 4 && !done; cls++)
                    if (const auto k = g_kept.find(KeyOf(r, cls)); k != g_kept.end()) done = PhaseCache::SettleSolved(it->second.at, k->second->start.tick);
                if (done) {
                    solved++;
                    continue;
                }
            }
            const bool queued = LevelLightShare::QueueRoomId(it->second.tracker, story, id, -1);
            sent[key] = queued;
            if (queued)
                if (const void* r = LevelLightShare::RoomOf(it->second.tracker, story, id)) g_physSent[r] = now | 1;
            (queued ? ok : refused)++;
        }
        if (KeptLog())
            LOG_INFO(std::format("[GpuRoomLamps] settle: lamp {:08X}, {} rooms sent to their solve {:.0f} s after its last edit{}{}{}", it->first, ok, kSettleMs / 1000.0,
                                 solved ? std::format(", {} left alone (solved since the edit)", solved) : std::string(),
                                 before ? std::format(", {} sent for another lamp just before", before) : std::string(),
                                 refused ? std::format(", {} refused by the room queue (already waiting for their gather, or gone)", refused) : std::string()));
        it = g_settle.erase(it);
    }
}
void LiveFrame() { g_liveStep = ObjectLegs::Gen(); }
bool LiveMovedWithin(const void* room, unsigned long ms, unsigned long maxRun) {
    const DWORD now = GetTickCount();
    std::lock_guard<std::mutex> lk(g_liveRunMx);
    const auto it = g_liveRuns.find(room);
    return it != g_liveRuns.end() && now - it->second.last < ms && now - it->second.first < maxRun;
}
void KeptRooms(uintptr_t mgr, int cls, std::vector<KeptRef>& out) {
    out.clear();
    const uint32_t gen = LiveGen(), step = g_liveStep;
    std::vector<std::pair<uint64_t, std::shared_ptr<const KeptRoom>>> due;
    {
        std::lock_guard<std::mutex> lk(g_keptMx);
        for (const auto& [key, k] : g_kept)
            if (k->mgr == mgr && k->cls == cls && !k->tailWhy && k->tail.size() == k->texels.size()) {
                out.push_back(KeptRef{reinterpret_cast<const void*>(k->room), k->id, k->version});
                if (!gen || !k->physical) continue;
                const auto l = g_live.find(key);
                if (l == g_live.end() || l->second.version != k->version || RoomLightTests::LiveDue(gen, l->second.gen, step, l->second.step, l->second.key)) due.emplace_back(key, k);
                else out.back().live = l->second.key;
            }
    }
    if (g_live.size() > 512) g_live.clear(); // (rooms of lots left behind: read again)
    for (const auto& [key, k] : due) { // (the game's memory read outside the lock)
        LiveTable& l = g_live[key];
        const uint64_t was = l.version == k->version ? l.key : 0;
        uint64_t now = 0;
        try {
            RoomLightTests::Room t;
            const bool fresh = LiveTests(*k, was, t, now);
            if (fresh) {
                float header = -1.0f;
                l.blk = DrawnTableOf(t.rec, header), l.header = static_cast<int>(header);
                if (KeptLog() && !was) LOG_INFO(std::format("[LiveTable] Story {} room {}: its 3D test follows an object ahead of the solve", k->story, k->id));
            }
        } catch (...) {
            now = 0; // (no memory: its solve's table)
        }
        if (!now) l.blk.reset(), l.header = -1;
        // (room 0's table waits for its solve: holding its solve would keep the old shadow for the whole drag)
        if (now != was && k->id != 0) NoteLiveMove(reinterpret_cast<const void*>(k->room));
        l.version = k->version, l.gen = gen, l.step = step, l.key = now;
        for (KeptRef& r : out)
            if (r.room == reinterpret_cast<const void*>(k->room)) r.live = now;
    }
}
// What the GPU draws of map m of a kept room (render thread; the kept room is never changed once kept, so it is read without
// the lock): rt_lamp_gpu.h KeptMapData
bool KeptAt(uint64_t lot, int story, int id, int cls, KeptRef& out) {
    std::lock_guard<std::mutex> lk(g_keptMx);
    for (const auto& [key, k] : g_kept)
        if (k->lot == lot && k->story == story && k->id == id && k->cls == cls && !k->tailWhy && k->tail.size() == k->texels.size()) {
            out = KeptRef{reinterpret_cast<const void*>(k->room), k->id, k->version};
            if (const auto l = g_live.find(key); l != g_live.end() && l->second.version == k->version) out.live = l->second.key;
            return true;
        }
    return false;
}
bool KeptBlockers(const void* room, int cls, uint32_t version, std::shared_ptr<const RtLampGpu::BlockerTable>& t, int& header) {
    std::lock_guard<std::mutex> lk(g_keptMx);
    const auto it = g_kept.find(KeyOf(room, cls));
    if (it == g_kept.end() || it->second->version != version || !it->second->blk) return false;
    t = it->second->blk, header = static_cast<int>(it->second->blkHeader);
    if (const auto l = g_live.find(it->first); l != g_live.end() && l->second.version == version && l->second.blk) t = l->second.blk, header = l->second.header;
    return true;
}
bool BuildKeptMap(const void* room, int cls, uint32_t version, int m, RtLampGpu::KeptMapData& d, std::string& why) {
    std::shared_ptr<const KeptRoom> kp;
    {
        std::lock_guard<std::mutex> lk(g_keptMx);
        const auto it = g_kept.find(KeyOf(room, cls));
        if (it == g_kept.end() || it->second->version != version) return why = "no longer kept", false;
        kp = it->second;
    }
    const KeptRoom& k = *kp;
    const size_t n = k.texels.size();
    if (k.tailWhy || k.tail.size() != n || m < 0 || m > 3) return why = "no tail kept", false;
    d = RtLampGpu::KeptMapData{};
    d.story = k.story, d.id = k.id, d.cls = k.cls, d.m = m, d.lot = k.lot, d.thr = k.thr, d.w = static_cast<int>(k.mapW[m]), d.h = static_cast<int>(k.mapH[m]);
    // the map's texels: their place in the sum's layout, and the last write of each place
    std::vector<uint32_t> local(n, kNoSrc);
    std::unordered_map<uint32_t, uint32_t> lastAt; // y << 16 | x -> texel
    uint32_t count = 0;
    for (uint32_t i = 0; i < n; i++)
        if (k.texels[i].map == m) local[i] = count++, lastAt[static_cast<uint32_t>(k.texels[i].y) << 16 | k.texels[i].x] = i;
    if (!count || d.w <= 0 || d.h <= 0) return why = "no texel in that map", false;
    d.accW = static_cast<int>(std::min<uint32_t>(count, 1024)), d.accH = static_cast<int>((count + static_cast<uint32_t>(d.accW) - 1) / static_cast<uint32_t>(d.accW));
    if (d.accH > 8192) return why = "too many texels", false;
    const auto accX = [&](uint32_t i) { return static_cast<float>(local[i] % static_cast<uint32_t>(d.accW)); };
    const auto accY = [&](uint32_t i) { return static_cast<float>(local[i] / static_cast<uint32_t>(d.accW)); };
    const auto sample = [&](uint32_t i) { return reinterpret_cast<const float*>(k.samples.data() + static_cast<size_t>(i) * 0x30); };
    // the parts, by lamp
    for (const KeptLamp& l : k.lamps) {
        const UINT first = static_cast<UINT>(d.parts.size());
        for (const KeptPart& p : l.parts) {
            if (p.texel >= n || local[p.texel] == kNoSrc) continue;
            const float* s = sample(p.texel);
            RtLampGpu::TexelVertex v{};
            v.x = accX(p.texel), v.y = accY(p.texel), v.w = 1.0f;
            for (int q = 0; q < 3; q++) v.p[q] = s[q], v.n[q] = s[4 + q];
            v.p[3] = static_cast<float>(p.bits & (kKeptEarly | kKeptFade));
            v.n[3] = static_cast<float>(p.mult);
            v.base[0] = FromHalf(p.a), v.base[1] = FromHalf(p.b), v.base[2] = FromHalf(p.thrScale);
            d.parts.push_back(v);
        }
        if (d.parts.size() > first)
            d.lamps.push_back(l.light), d.colourOnly.push_back(l.colourOnly ? 1 : 0), d.ranges.emplace_back(first, static_cast<UINT>(d.parts.size()) - first);
    }
    // (physical) every texel of the map at its place in the sum's layout, in world space (gpu_room_lamps.cpp takes p and n to the
    // lot's space): its batch's 3D test switch (bit 8), one room slot; every lamp of the solve with a law, its mult x norm scale
    if (k.physical) {
        d.physical = true;
        std::vector<uint8_t> tested(n, 0);
        for (const BatchRec& b : k.batches)
            for (uint32_t j = 0; j < b.count && b.first + j < n; j++) tested[b.first + j] = b.flags[1] != 0;
        for (uint32_t i = 0; i < n; i++) {
            if (local[i] == kNoSrc) continue;
            const float* s = sample(i);
            RtLampGpu::TexelVertex v{};
            v.x = accX(i), v.y = accY(i), v.w = 1.0f;
            for (int q = 0; q < 3; q++) v.p[q] = v.base[q] = s[q], v.n[q] = s[4 + q];
            v.p[3] = tested[i] ? 8.0f : 0.0f;
            d.phys.push_back(v);
            for (int q = 0; q < 3; q++) d.physBox[q] = std::min(d.physBox[q], s[q]), d.physBox[3 + q] = std::max(d.physBox[3 + q], s[q]);
        }
        d.blockers = k.blk, d.blkHeader = k.blkHeader;
        for (const KeptLamp& l : k.lamps)
            d.lamps.push_back(l.light), d.physMult.push_back(static_cast<float>(l.mult) * l.normK), d.physReach.push_back(l.reach), d.physHome.push_back(l.home);
        d.physBaked = k.baked, d.physNorm = k.norm, d.physOutdoor = k.outdoor;
        if (k.roofed) std::memcpy(d.physAmb, k.amb, sizeof d.physAmb);
    }
    // (roofed) the ramp terms for the texels the solve's lamps saturated (GpuLampLaw::SplitFill), from this map's texels that split well
    GpuLampLaw::SplitFill fill;
    if (k.roofed) {
        std::vector<std::array<float, 5>> good;
        for (uint32_t i = 0; i < n; i++) {
            if (local[i] == kNoSrc) continue;
            const KeptTail& tl = k.tail[i];
            float xs[3], c[3], ra[3], r = 0.0f;
            for (int q = 0; q < 3; q++) xs[q] = tl.norm * k.texels[i].t.S[q];
            GpuLampLaw::Tone(xs, c);
            if (GpuLampLaw::Weight(c, k.toneK) >= 0.2f && GpuLampLaw::IndoorSplit(xs, tl.p, k.toneK, ra, r)) good.push_back({sample(i)[1], r, ra[0], ra[1], ra[2]});
        }
        fill = GpuLampLaw::FitSplit(std::move(good));
    }
    // a texel's compose point at (x, y) of its target (RtLampModel::KeptTexel's inputs)
    const auto texel = [&](uint32_t i, int x, int y) {
        const TexelRec& t = k.texels[i];
        const KeptTail& tl = k.tail[i];
        RtLampGpu::TexelVertex v{};
        v.x = static_cast<float>(x), v.y = static_cast<float>(y), v.w = 1.0f;
        for (int q = 0; q < 3; q++) v.p[q] = k.base[static_cast<size_t>(i) * 3 + q];
        v.p[3] = (tl.bits & kTailDither) ? RoomMapDither::Offset(t.x, t.y) / 255.0f : 0.0f;
        if (k.roofed) {
            float xs[3], rampAmb[3] = {}, ramp = 0.0f;
            for (int q = 0; q < 3; q++) xs[q] = tl.norm * t.t.S[q];
            GpuLampLaw::IndoorSplitOrFill(xs, tl.p, k.toneK, sample(i)[1], fill, rampAmb, ramp);
            for (int q = 0; q < 3; q++) v.n[q] = rampAmb[q];
            v.n[3] = ramp;
        } else
            v.n[3] = tl.p[3];
        v.base[0] = tl.norm, v.base[1] = k.toneK, v.base[2] = (tl.bits & kTailFloor) ? RoomMapDither::kRoofedAlphaMin : 0.0f, v.base[3] = k.roofed ? 1.0f : 0.0f;
        v.mask[0] = accX(i), v.mask[1] = accY(i);
        return v;
    };
    const auto copyPoint = [](int x, int y, int sx, int sy) {
        RtLampGpu::TexelVertex v{};
        v.x = static_cast<float>(x), v.y = static_cast<float>(y), v.w = 1.0f;
        v.mask[0] = static_cast<float>(sx), v.mask[1] = static_cast<float>(sy);
        return v;
    };
    const auto placeOf = [](int x, int y) { return static_cast<uint32_t>(y) << 16 | static_cast<uint32_t>(x); };
    const bool blurred = m == 0 && k.cls == 2;
    d.blur = blurred;
    for (const KeptLamp& l : k.lamps) d.roomLamps.push_back(RtLampGpu::KeptMapData::RoomLamp{l.light, {l.pos[0], l.pos[1], l.pos[2]}, l.geo, l.colourOnly, l.law});
    // every last write at its own place (a copy's source is taken as composed; a place a copy overwrites is drawn over after)
    if (!blurred)
        for (const auto& [key, i] : lastAt) {
            const TexelRec& t = k.texels[i];
            d.texels.push_back(texel(i, t.x, t.y));
            const uint32_t src = k.tail[i].src;
            if (src == kNoSrc) d.restores.push_back(copyPoint(t.x, t.y, t.x, t.y)); // (a copy of a texel no capture holds: the game's)
            else {
                if (src != i && local[src] != kNoSrc) d.copies.push_back(texel(src, t.x, t.y)); // (its source composed at its place)
                d.expect.emplace_back(placeOf(t.x, t.y), k.tail[i].final);
            }
        }
    for (const KeptCopy& c : k.copies)
        if (c.map == m && c.src < n && local[c.src] != kNoSrc) {
            d.copies.push_back(texel(c.src, c.x, c.y));
            d.expect.emplace_back(placeOf(c.x, c.y), c.final);
        }
    // class-2 walls: each block BlurWalls blurred, with its rows beyond, side by side in the layout (the game's own blur: none drawn)
    if (blurred && k.passes && k.wallsOk) {
        d.layoutW = d.w;
        int lx = 0, ly = 0, shelf = 0;
        std::vector<std::pair<int, int>> at(k.blocks.size(), {-1, -1});
        for (size_t j = 0; j < k.blocks.size(); j++) {
            const BlockRec& b = k.blocks[j];
            const int bw = b.x1 - b.x0 + 1, eh = (b.above ? 2 : 0) + (b.y1 - b.y0 + 1) + (b.below ? 2 : 0);
            if (!b.have || bw > d.layoutW) continue;
            if (lx + bw > d.layoutW) lx = 0, ly += shelf, shelf = 0;
            at[j] = {lx, ly};
            lx += bw, shelf = std::max(shelf, eh);
        }
        d.layoutH = ly + shelf;
        if (d.layoutH > 8192) return why = "too many wall blocks for the layout", false;
        d.layout.assign(static_cast<size_t>(d.layoutW) * std::max(d.layoutH, 1), 0);
        for (size_t j = 0; j < k.blocks.size(); j++) {
            if (at[j].first < 0) continue;
            const BlockRec& b = k.blocks[j];
            const int bw = b.x1 - b.x0 + 1, rows = b.y1 - b.y0 + 1, top = b.above ? 2 : 0, eh = top + rows + (b.below ? 2 : 0);
            const auto [bx, by] = at[j];
            const auto row = [&](int ly2, const uint32_t* src) { std::copy_n(src, bw, d.layout.begin() + static_cast<size_t>(ly2) * d.layoutW + bx); };
            if (b.above) row(by, &k.preblur[b.ghostOff]), row(by + 1, &k.preblur[b.ghostOff + static_cast<size_t>(bw)]);
            for (int y = 0; y < rows; y++) row(by + top + y, &k.preblur[b.off + static_cast<size_t>(y) * bw]);
            if (b.below) row(by + top + rows, &k.preblur[b.ghostOff + static_cast<size_t>(bw) * 2]), row(by + top + rows + 1, &k.preblur[b.ghostOff + static_cast<size_t>(bw) * 3]);
            d.extRects.push_back(RtLampGpu::Block{bx, by, bx + bw - 1, by + eh - 1});
            d.outs.push_back(RtLampGpu::KeptBlockOut{b.x0, b.y0, b.x1, b.y1, bx, by + top, by + top + rows - 1, b.above, b.below});
            for (int y = b.y0; y <= b.y1; y++)
                for (int x = b.x0; x <= b.x1; x++)
                    if (const auto it = lastAt.find(static_cast<uint32_t>(y) << 16 | static_cast<uint32_t>(x)); it != lastAt.end()) {
                        d.wallTexels.push_back(texel(it->second, bx + x - b.x0, by + top + y - b.y0));
                        d.expect.emplace_back(placeOf(x, y), k.tail[it->second].final);
                    }
        }
        d.passes = k.passes;
    }
    // (sharper floors) the floor map at twice its size: each kept texel's 4 sub-texels, their phys points at their own places on
    // the floor (PhysLampPick::SubTexelPos over the neighbours the solve wrote), composed at their own places of the 2w x 2h map
    // with the texel's base; a place no capture holds (a restore) keeps the game's texel through the upsample
    if (k.physical && m == 1 && !blurred && HiFloors() && 2 * d.accH <= 4096) {
        std::vector<uint8_t> tested(n, 0);
        for (const BatchRec& b : k.batches)
            for (uint32_t j = 0; j < b.count && b.first + j < n; j++) tested[b.first + j] = b.flags[1] != 0;
        const auto at = [&](int x, int y) -> const float* {
            if (x < 0 || y < 0) return nullptr;
            const auto it = lastAt.find(static_cast<uint32_t>(y) << 16 | static_cast<uint32_t>(x));
            return it == lastAt.end() ? nullptr : sample(it->second);
        };
        for (uint32_t i = 0; i < n; i++) {
            if (local[i] == kNoSrc) continue;
            const float* s = sample(i);
            // (the sub-texel's own footprint, a quarter texel each side: its shadow is the footprint's mean, so an edge sharper than
            // a sub-texel, a door jamb's near the door, does not step; mask.w, AccPhys)
            float sub[4][3];
            const float foot = PhysLampPick::SubTexels(s, k.texels[i].x, k.texels[i].y, at, sub);
            for (int q = 0; q < 4; q++) {
                RtLampGpu::TexelVertex v{};
                v.x = 2.0f * accX(i) + static_cast<float>(q & 1), v.y = 2.0f * accY(i) + static_cast<float>(q >> 1), v.w = 1.0f;
                std::memcpy(v.p, sub[q], sizeof sub[q]);
                std::memcpy(v.base, v.p, 3 * sizeof(float));
                for (int c = 0; c < 3; c++) v.n[c] = s[4 + c];
                v.p[3] = tested[i] ? 8.0f : 0.0f;
                v.mask[3] = foot;
                d.physHi.push_back(v);
            }
        }
        const auto sub = [&](uint32_t src, int x, int y) {
            for (int q = 0; q < 4; q++) {
                RtLampGpu::TexelVertex v = texel(src, 2 * x + (q & 1), 2 * y + (q >> 1));
                // (each sub-texel its own: the parent's in all four survived the read at twice the size as 12.5 cm blocks in dim light through an arch; a 16-bit target takes none, Packed)
                if (v.p[3] != 0.0f) v.p[3] = RoomMapDither::Offset(2 * k.texels[src].x + (q & 1), 2 * k.texels[src].y + (q >> 1)) / 255.0f;
                v.mask[0] = 2.0f * accX(src) + static_cast<float>(q & 1), v.mask[1] = 2.0f * accY(src) + static_cast<float>(q >> 1);
                d.texelsHi.push_back(v);
                d.placesHi.push_back(copyPoint(2 * x + (q & 1), 2 * y + (q >> 1), 2 * x + (q & 1), 2 * y + (q >> 1)));
            }
        };
        for (const auto& [key, i] : lastAt) // (as above: every last write, then the copies over them)
            if (k.tail[i].src != kNoSrc) sub(i, k.texels[i].x, k.texels[i].y);
        for (const auto& [key, i] : lastAt)
            if (const uint32_t src = k.tail[i].src; src != kNoSrc && src != i && local[src] != kNoSrc) sub(src, k.texels[i].x, k.texels[i].y);
        for (const KeptCopy& c : k.copies)
            if (c.map == m && c.src < n && local[c.src] != kNoSrc) sub(c.src, c.x, c.y);
    }
    if (k.moveCopy) d.expect.clear();
    if (d.texels.empty() && d.wallTexels.empty()) return why = "no texel drawn in that map", false;
    return true;
}
void NotePack(const void* room, bool xy, unsigned x, unsigned y, const float* input, bool dithered, bool alphaFloor) {
    if (!room || t_packRoom != room) return; // (another thread's pack, or no kept capture's texel pending)
    t_packRoom = nullptr;
    Capture& c = g_cap;
    if (!c.active || !c.keep || c.room != room || !c.rec || c.tailWhy || t_packTexel >= c.tail.size() || t_packTexel >= c.rec->texels.size()) return;
    const TexelRec& t = c.rec->texels[t_packTexel];
    KeptTail& k = c.tail[t_packTexel];
    if (!input) c.tailWhy = "the pack input is not known (the solve's code differs)";
    else if (!xy || t.x != x || t.y != y) c.tailWhy = "a texel packed at another place";
    else if (!CopyBytes(k.p, input, sizeof k.p) || !CopyBytes(&k.norm, static_cast<const BYTE*>(room) + 0x160, sizeof k.norm)) c.tailWhy = "a pack input could not be read";
    else k.bits = static_cast<uint8_t>((dithered ? kTailDither : 0) | (alphaFloor ? kTailFloor : 0)), k.seen = 1;
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
        // (a kept capture: also a block with rows lit beyond its edges, and those rows)
        b.have = b.ok || (g_cap.keep && v.before && v.x1 >= v.x0 && v.y1 >= v.y0);
        if (b.have) {
            b.off = static_cast<uint32_t>(r.preblur.size());
            r.preblur.insert(r.preblur.end(), v.before, v.before + static_cast<size_t>(v.x1 - v.x0 + 1) * (v.y1 - v.y0 + 1));
            if (g_cap.keep && v.ghost && (v.above || v.below)) {
                b.above = v.above, b.below = v.below;
                b.ghostOff = static_cast<uint32_t>(r.preblur.size());
                r.preblur.insert(r.preblur.end(), v.ghost, v.ghost + static_cast<size_t>(v.x1 - v.x0 + 1) * 4);
            }
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

// ---- Lighting scene recordings (scene_record.h) ----
bool SceneBegin(const std::filesystem::path& temp) {
    std::lock_guard<std::mutex> lk(g_sceneMx);
    if (g_sceneFile) std::fclose(g_sceneFile), g_sceneFile = nullptr;
    if (_wfopen_s(&g_sceneFile, temp.c_str(), L"wb") || !g_sceneFile) return g_sceneFile = nullptr, false;
    SceneRecord::WriteHead(g_sceneFile);
    g_scenePath = temp, g_sceneGen = 0, g_sceneRooms = 0;
    g_sceneOn.store(true), g_sceneAll.store(true); // (every kept physical room of the lot solved once: the first step)
    g_sceneEditSeen = g_lastPhysEdit.load(std::memory_order_relaxed);
    LOG_INFO(RoomLampPhysical() ? "[SceneRecord] Recording the physical rooms' solves with the game's parts of their lamps"
                                : "[SceneRecord] Recording, but the physical indoor lamp model is off: no room will be written");
    return true;
}
long SceneEnd(const std::filesystem::path& to) {
    std::lock_guard<std::mutex> lk(g_sceneMx);
    g_sceneOn.store(false), g_sceneAll.store(false);
    if (!g_sceneFile) return 0;
    std::fclose(g_sceneFile), g_sceneFile = nullptr;
    std::error_code ec;
    const long rooms = g_sceneRooms;
    if (to.empty() || !rooms) std::filesystem::remove(g_scenePath, ec);
    else {
        std::filesystem::rename(g_scenePath, to, ec);
        if (ec) std::filesystem::copy_file(g_scenePath, to, std::filesystem::copy_options::overwrite_existing, ec), std::filesystem::remove(g_scenePath, ec);
        LOG_INFO(std::format("[SceneRecord] Saved {} room solves to {}", rooms, ApexUtil::ToUtf8(to.filename().wstring())));
    }
    return to.empty() ? 0 : rooms;
}
// Render thread: the rooms a recording needs solved (the lot's kept physical rooms at its start; an edit's rooms once edits rest)
void SceneTick() {
    if (!g_sceneOn.load(std::memory_order_relaxed)) return;
    if (g_sceneAll.exchange(false)) {
        std::vector<std::tuple<uint64_t, int, int>> rooms;
        {
            std::lock_guard<std::mutex> lk(g_keptMx);
            for (const auto& [key, k] : g_kept)
                if (k->physical) rooms.emplace_back(k->lot, k->story, k->id);
        }
        const uintptr_t tracker = RtOutdoorLamps::ActiveTracker();
        int sent = 0;
        if (tracker && LevelLightShare::TrackerLive(tracker))
            for (const auto& [lot, story, id] : rooms)
                if (void* r = id ? LevelLightShare::RoomOf(tracker, story, id) : LevelLightShare::Room0Of(tracker, story)) sent += LevelLightShare::QueueRoomAgain(r) ? 1 : 0;
        LOG_INFO(std::format("[SceneRecord] {} kept physical rooms sent to a solve for the recording's first step", sent));
    }
    const DWORD last = g_lastPhysEdit.load(std::memory_order_relaxed);
    if (last && last != g_sceneEditSeen && GetTickCount() - last >= 400) {
        g_sceneEditSeen = last;
        SolveTouched(nullptr); // (the rooms of the edits taken since their solve: the game's answer at the lamps' new state)
    }
}

void OnPresent() {
    const DWORD now = GetTickCount();
    HookGuard::Run("OutdoorDelta left lot sweep", [now] { SweepLeftLots(now); });
    // (physical indoor lamps, stage 4) every 10 s with lamp edits: what they cost (solves outside the settle: target 0)
    static DWORD s_summaryAt = 0;
    if (now - s_summaryAt >= 10000) {
        s_summaryAt = now;
        const long taken = c_physTaken.exchange(0), refused = c_physRefused.exchange(0), solves = c_physEditSolves.exchange(0), settles = c_physSettleSolves.exchange(0);
        int maps = 0;
        double ms = 0.0, worst = 0.0;
        GpuRoomLamps::TakeRelightCost(maps, ms, worst);
        if (taken || refused)
            LOG_INFO(std::format("[GpuRoomLamps] physical lamp edits in the last 10 s: {} taken, {} left to the room solves; {} room solves of the lot within {} s of an edit "
                                 "taken outside the settle, {} settle or resend solves; physical relights {} maps, {:.1f} ms CPU (largest frame {:.2f} ms){}",
                                 taken, refused, solves, kEditSolveMs / 1000, settles, maps, ms, worst, GpuRoomLamps::TakeTileCost()));
        else GpuRoomLamps::TakeTileCost();
        std::map<std::string, long> why;
        {
            std::lock_guard<std::mutex> lk(g_refuseMx);
            why.swap(g_refuseWhy);
        }
        if (refused * 10 > taken + refused) { // (health: more than 10% of the edits went to the game's solves)
            std::vector<std::pair<long, std::string>> top;
            for (auto& [w, k] : why) top.emplace_back(k, w);
            std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
            std::string list;
            for (size_t i = 0; i < top.size() && i < 3; i++) list += std::format("{}{} x {}", i ? "; " : "", top[i].first, top[i].second);
            LOG_WARNING(std::format("[Health] physical edits refused: {} of {} in 10 s ({:.0f}%); top reasons: {}", refused, taken + refused,
                                    100.0 * static_cast<double>(refused) / static_cast<double>(taken + refused), list));
        }
    }
    HookGuard::Run("OutdoorDelta scene tick", SceneTick);
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
    g_keptClear.store(true); // (the old world's rooms: their addresses can come back as other rooms)
    g_dropPending.store(true);
    std::lock_guard<std::mutex> lk(g_sideMx);
    g_checkDue.clear();
    g_checkPending.clear();
    g_lotEdits.clear();
    g_otherStart.clear();
}
void NoteNightLevel(float level) {
    std::lock_guard<std::mutex> lk(g_clockMx);
    g_clock.Observe(PhaseCache::EndOf(level), GetTickCount(), level);
}
void InvalidateAll(const char* why) {
    (void)why;
    g_gen.fetch_add(1, std::memory_order_relaxed);
}
int ForgetLot(const uintptr_t* mgrs, int n) {
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    int gone = 0;
    for (auto it = g_records.begin(); it != g_records.end();)
        if (std::find(mgrs, mgrs + n, it->second->mgr) != mgrs + n) {
            g_bytes.fetch_sub(std::min(g_bytes.load(), it->second->bytes));
            it = g_records.erase(it);
            ++gone;
        } else
            ++it;
    c_records.store(static_cast<long>(g_records.size()));
    return gone;
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
    size_t samples = 0, texels = 0, baked = 0, drawn = 0, sums = 0, other = 0, n = 0, baseN = 0, baseBytes = 0;
    {
        std::unique_lock<std::recursive_mutex> lk(g_mx, std::try_to_lock); // (the render thread never waits for a worker's export copy)
        if (!lk.owns_lock()) return std::format("{:.1f} MB (the records were busy)", static_cast<double>(g_bytes.load()) / (1 << 20));
        for (const auto& [k, rec] : g_records) {
            samples += rec->samples.capacity(), texels += rec->texels.capacity() * sizeof(TexelRec);
            baked += PartsBytes(*rec, false), drawn += PartsBytes(*rec, true), sums += rec->drawnSum.capacity() * sizeof(DrawnSum);
            other += rec->batches.capacity() * sizeof(BatchRec) + rec->blocks.capacity() * sizeof(BlockRec) + rec->preblur.capacity() * 4;
            if (rec->baseOnly) baseN++, baseBytes += rec->bytes;
        }
        n = g_records.size();
    }
    const auto mb = [](size_t b) { return static_cast<double>(b) / (1 << 20); };
    return std::format("{:.1f} MB in {} rooms (samples {:.1f}, texels {:.1f}, baked lights' parts {:.1f}, drawn lamps' parts {:.1f}, drawn sums {:.1f}, batches and "
                       "wall blocks {:.1f}); {} rooms of lots next to the active one, {:.1f} MB (base only); dropped by the budget {}, given back after their lot "
                       "left {} ({:.1f} MB)",
                       mb(g_bytes.load()), n, mb(samples), mb(texels), mb(baked), mb(drawn), mb(sums), mb(other), baseN, mb(baseBytes), c_budgetDrops.load(),
                       c_leftLotDrops.load(), mb(c_leftLotBytes.load()));
}
size_t PinnedBudgetBytes() { return kMaxPinnedBytes; }
int KeptRooms() { return static_cast<int>(c_records.load()); }

// (14/10) a record is the room's only while the room still has its manager and story (a room 0 made again at a freed
// room's address inherited another room's texels until its first solve)
bool RecordOfRoom(const Record& r, const RoomRead& now) { return r.mgr == now.mgr && r.story == now.story; }
std::mutex g_versionMx;
std::unordered_map<uint64_t, uint64_t> g_versionSeen; // under g_versionMx: the last answer per key (when g_mx is busy; bit 32: base only)
uint32_t RecordVersion(const void* room, int cls, bool* baseOnly) {
    if (baseOnly) *baseOnly = false;
    if (!room || cls < 0 || cls > 2) return 0;
    RoomRead now;
    if (!ReadRoom(static_cast<const unsigned char*>(room), now)) return 0;
    const uint64_t key = KeyOf(room, cls);
    uint64_t v = 0;
    {
        // (14/10) the render thread asks every frame: a worker's export copy holding g_mx (tens of ms) gets the last answer
        std::unique_lock<std::recursive_mutex> lk(g_mx, std::try_to_lock);
        if (!lk.owns_lock()) {
            std::lock_guard<std::mutex> vk(g_versionMx);
            const auto s = g_versionSeen.find(key);
            v = s == g_versionSeen.end() ? 0 : s->second;
            if (baseOnly) *baseOnly = (v >> 32) != 0;
            return static_cast<uint32_t>(v);
        }
        const auto it = g_records.find(key);
        if (it != g_records.end() && RecordOfRoom(*it->second, now)) v = it->second->baseVersion | static_cast<uint64_t>(it->second->baseOnly) << 32;
    }
    std::lock_guard<std::mutex> vk(g_versionMx);
    if (g_versionSeen.size() > 256) g_versionSeen.clear();
    g_versionSeen[key] = v;
    if (baseOnly) *baseOnly = (v >> 32) != 0;
    return static_cast<uint32_t>(v);
}
bool ExportRoom0(const void* room, int cls, Room0Export& out, bool physical) {
    out = Room0Export{};
    if (!room || cls < 0 || cls > 2) return false;
    RoomRead now;
    if (!ReadRoom(static_cast<const unsigned char*>(room), now)) return false;
    std::lock_guard<std::recursive_mutex> lk(g_mx);
    const auto it = g_records.find(KeyOf(room, cls));
    if (it == g_records.end() || !RecordOfRoom(*it->second, now)) return false;
    const Record& r = *it->second;
    if (r.samples.size() != r.texels.size() * 0x30) return false;
    if (physical && !r.phys) out.physWhy = "its solve was kept by the replica model", physical = false; // (no normK, reach or 3D records)
    out.physical = physical;
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
            e.exitBase = 0.0f;
            e.worldBase[0] = e.worldBase[1] = e.worldBase[2] = 0.0f;
        }
        // (GPU room lamps) a kept exit lamp: the exit ground texels' alpha is drawn from the exit lamps' light (bit 4), those left
        // in the base giving exitBase
        // (physical) every lot lamp with a GPU law that Real-time outdoor lamps does not draw and no kept0 holds leaves the base too
        std::vector<uint8_t> phys(r.lamps.size(), 0);
        if (physical)
            for (size_t j = 0; j < r.lamps.size(); j++) {
                const LampRec& l = r.lamps[j];
                KeptLamp kl;
                phys[j] = !l.drawn && std::none_of(r.kept0.begin(), r.kept0.end(), [&](const KeptLamp& k) { return k.light == l.light; }) && LawOfLight(l.light, r.lot, kl) &&
                          !kl.colourOnly;
            }
        // (physical: always, so a fresh exit lamp, or one its solve did not take as one, lights the ground through the openings
        // its shadow tile sees; lot C49C's solves kept no exit part, so no indoor lamp ever lit its ground)
        bool exits = physical || std::any_of(r.kept0.begin(), r.kept0.end(), [](const KeptLamp& k) { return k.exit; });
        for (size_t j = 0; j < r.lamps.size(); j++) exits = exits || (phys[j] && r.lamps[j].exit);
        if (exits)
            for (size_t i = 0; i < r.texels.size(); i++)
                if (r.texels[i].bits & kExitGround) out.texels[i].bits |= 4;
        for (size_t j = 0; j < r.lamps.size(); j++) {
            const LampRec& l = r.lamps[j];
            out.lamps++;
            out.kept.push_back(l.light);
            const bool k0 = phys[j] || std::any_of(r.kept0.begin(), r.kept0.end(), [&](const KeptLamp& k) { return k.light == l.light; });
            if (exits && l.exit && !k0)
                for (const Contrib& p : l.parts)
                    if (p.texel < out.texels.size()) out.texels[p.texel].exitBase += Lum3(p.rgb);
            if (!l.drawn && !k0) {
                if (const uint64_t lot = physical ? LightLotOf(l.light) : r.lot; lot != r.lot && lot != ~0ull) // (a street lamp: left in the base, noted for the upper walls of open rooms; unreadable: left)
                    for (const Contrib& p : l.parts)
                        if (p.texel < out.texels.size())
                            for (int k = 0; k < 3; k++) out.texels[p.texel].worldBase[k] += p.rgb[k];
                continue;
            }
            if (l.drawn) out.drawn.push_back(l.light);
            if (phys[j])
                out.phys.push_back(l.light), out.physRow.push_back({static_cast<float>(std::max(l.mult, 1)) * l.normK, l.reach ? 1.0f : 0.0f, l.exit ? 1.0f : 0.0f});
            for (const Contrib& p : l.parts) {
                if (p.texel >= out.texels.size()) continue;
                Room0Export::Texel& e = out.texels[p.texel];
                for (int k = 0; k < 3; k++) e.base[k] -= p.rgb[k];
                e.vis -= FromHalf(p.vis);
                e.all -= FromHalf(p.all);
            }
        }
        for (const KeptLamp& k : r.kept0) {
            Room0Export::KeptLamp0 e;
            e.light = k.light, e.colourOnly = k.colourOnly, e.geo = k.geo, e.law = k.law;
            std::memcpy(e.pos, k.pos, sizeof e.pos);
            e.parts.reserve(k.parts.size());
            for (const KeptPart& p : k.parts)
                e.parts.push_back(Room0Export::KeptPart0{p.texel, FromHalf(p.a), FromHalf(p.b), FromHalf(p.thrScale), p.mult, static_cast<uint8_t>(p.bits & (kKeptEarly | kKeptFade))});
            e.exit = k.exit;
            for (const KeptLamp::Mask& q : k.mask) e.mask.push_back(Room0Export::KeptLamp0::Mask0{q.texel, q.vis, q.all});
            out.kept0.push_back(std::move(e));
        }
        for (const BlockRec& b : r.blocks) out.blocks.push_back(Room0Export::Block{b.x0, b.y0, b.x1, b.y1, b.ok, b.off});
        out.preblur.assign(r.preblur.begin(), r.preblur.end());
        if (physical) { // the texels whose batch ran the game's 3D test (bit 8), and room 0's records
            for (const BatchRec& b : r.batches)
                for (uint32_t j = 0; b.flags[1] && j < b.count && b.first + j < out.texels.size(); j++) out.texels[b.first + j].bits |= 8;
            out.blockers = r.blk, out.blkHeader = r.blkHeader;
        }
    } catch (...) {
        out = Room0Export{};
        return false;
    }
    if (physical && KeptLog())
        LOG_INFO(std::format("[GpuRoomLamps] Story {} room 0: base without {} lot lamps (physical; 3D test {})", out.story, out.phys.size(),
                             out.blkHeader < 0.0f || !out.blockers ? std::string("records not read") : std::format("{} rows", out.blockers->rows.size())));
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
