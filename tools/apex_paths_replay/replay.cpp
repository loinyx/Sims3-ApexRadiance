// Offline replay of Apex Radiance's own light tests in a room's solve (LevelLightShare's LightEvalHook) against a
// RoomLampProbe dump: the floor-under-this-story cut (OwnFloorBlocksAt), InnerShadow (InnerPass), the outdoor lamps through
// openings (YardShadow -> OpenLampPass, OpeningGain, EntryScale with the recorded outdoor total, OpeningLampFactor,
// MaxResponse), IndoorShadow (IndoorPassImpl, IndoorBoundaryPass; the game's wall test of each ray segment's room is the
// recorded answer) and the soft light cut (FadeAtThreshold). The production functions are extracted by run.ps1 from
// features/level_light_share.cpp and run over a copy of the game memory they read, rebuilt from the dump (story managers,
// tiles, floor keys) and the dumped story grids. Then the whole chain for every dumped texel: the game's evaluation x Apex's
// steps x the game's 2D wall and 3D tests (tools/room_lamp_3d_replay) against the measured part.
// Usage: replay.exe <RoomLampProbe-*.txt>   (read-only, prints to stdout)
#include <xmmintrin.h>
#include <excpt.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>
#include <format>

#define APEX_DEVELOPMENT_BUILD_FLAVOR 1 // (dev_switches.h: the switches read their atomics, set from the dump's APEX line)
#include "features/fast_math.h"
#include "features/penumbra.h"
#include "features/lamp_visibility.h"
#include "features/wall_openings.h"
#include "features/inner_retry.h"
#include "features/opening_lamp_balance.h"
#include "features/gpu_lamp_law.h"
#include "features/dev_switches.h"
#include "features/mesh_occlusion.h"
#include "features/outdoor_delta.h"
#include "features/rt_lamp_model.h"
#include "features/room_lamp_move.h"

// ---- what the extracted production code needs around it ----
using BYTE = unsigned char;
using DWORD = unsigned long;
constexpr bool kPublicBuild = true;
constexpr bool kMemoryOptimizationsEnabled = true;
namespace ApexUtil {
template <typename T> inline void CountOne(std::atomic<T>& c) { c.fetch_add(1, std::memory_order_relaxed); }
}
enum TestPart { kTpWall, kTpWallGame, kTpKeep, kTpEntry, kTpMid, kTpYard, kTpGate, kTpIndoor, kTpCount };
struct PartTimer {
    explicit PartTimer(int) {}
};
enum : uint8_t { kCutUnknown = 0, kCutDoor = 1, kCutWindow = 2, kCutArch = 3, kCutGate = 4 };
bool g_ghostSolve = false, g_enterReady = true;
BYTE* g_swapAt = nullptr;
BYTE g_swapSaved = 0;
std::atomic<long> g_faults{0}, g_indoorLampWallVetoes{0}, g_innerDown{0}, g_softCutFaded{0};
using WallTest_t = bool(__thiscall*)(void* room, void* indexVec, const float* lightPos, const void* sample, float* transmission);
uintptr_t kWallTest = 0;
thread_local OutdoorDelta::ProbeEval* t_pe = nullptr;
thread_local const unsigned char* t_solveRoom = nullptr;
struct RoomInfo;
struct RayEntry;
struct SlabRay;
struct StoryGrid;
struct GridWhy;
uintptr_t StoryManager(uintptr_t tracker, int level);
uintptr_t LevelForPoint(uintptr_t mgr);
uintptr_t LevelManager(uintptr_t level);
void* RoomByIdCached(void* mgr, int id);
void LampPos(void* light, float* out);
bool OutdoorEntry(const RoomInfo& info, int home, void* light, const float* sample, RayEntry& entry, bool* above, SlabRay* slab);
float YardPass(const StoryGrid* g0, const StoryGrid* g1, void* light, const float* pointW, float* lampW, GridWhy& why);
float InnerDownGate(const RoomInfo& info, const float* lampW, const float* pointW);

#include "extracted_apex_paths.h"

float YardPass(const StoryGrid* g0, const StoryGrid* g1, void* light, const float* pointW, float* lampW, GridWhy& why) {
    return g0 ? YardPassImpl(g0, g1, light, pointW, lampW, why) : 0.0f;
}
bool OutdoorEntry(const RoomInfo& info, int home, void* light, const float* sample, RayEntry& entry, bool* above, SlabRay* slab) {
    return OutdoorEntryImpl(info, home, light, sample, entry, above, slab);
}
float InnerDownGate(const RoomInfo& info, const float* lampW, const float* pointW) { return InnerDownGateImpl(info, lampW, pointW); }

// the game's 2D and 3D tests (tools/room_lamp_3d_replay), in their own namespace
namespace r3d {
#define main r3d_unused_main
#include "tools/room_lamp_3d_replay/replay.cpp"
#undef main
} // namespace r3d

// ---- the game memory the extracted code reads, rebuilt from the dump ----
// A story: its manager (+0x88 story, +0x98 lowest floor with +0xD4 = 0, +0xE0 lot matrix, +0x260 tiles, +0x264/+0x268 their
// size), its tiles (+0x78 floor height, +0x7C + q x 0x14 the quadrant's room) and its floor object (+0x264 -> grid: +0 data,
// +0x10 / +0x14 size; 40-byte cells, the quadrants' keys at +8 + q x 8)
struct FakeStory {
    int story = -1;
    uint32_t realMgr = 0, realLevel = 0;
    std::vector<uint8_t> mgr = std::vector<uint8_t>(0x300), level = std::vector<uint8_t>(0x300), grid = std::vector<uint8_t>(0x20), data;
    std::vector<std::array<uint8_t, 0xCC>> tiles;
    std::vector<uintptr_t> tilePtr;
    int tw = 0, th = 0, kw = 0, kh = 0;
    uintptr_t Mgr() const { return reinterpret_cast<uintptr_t>(mgr.data()); }
    uintptr_t Level() const { return reinterpret_cast<uintptr_t>(level.data()); }
    template <class T> void Put(std::vector<uint8_t>& b, size_t off, T v) { std::memcpy(b.data() + off, &v, sizeof v); }
};
struct FakeLot {
    FakeStory* s[8] = {};
};
std::unordered_map<uintptr_t, FakeStory*> g_byMgr, g_byLevel; // fake addresses
std::unordered_map<uint32_t, FakeStory*> g_realMgr;           // the current room's stories by their real manager
std::array<uint8_t, 0x700> g_dummyRoom{};
float g_lamp[4] = {};
uintptr_t StoryManager(uintptr_t tracker, int level) {
    const FakeLot* lot = reinterpret_cast<const FakeLot*>(tracker);
    return lot && level >= 0 && level < 8 && lot->s[level] ? lot->s[level]->Mgr() : 0;
}
uintptr_t LevelForPoint(uintptr_t mgr) {
    if (const auto it = g_byMgr.find(mgr); it != g_byMgr.end()) return it->second->Level();
    if (const auto it = g_realMgr.find(static_cast<uint32_t>(mgr)); it != g_realMgr.end()) return it->second->Level();
    return 0;
}
uintptr_t LevelManager(uintptr_t level) {
    const auto it = g_byLevel.find(level);
    return it != g_byLevel.end() ? it->second->Mgr() : 0;
}
void LampPos(void*, float* out) { std::memcpy(out, g_lamp, sizeof g_lamp); }

// ---- the dump ----
using Tok = std::vector<std::string_view>;
Tok Split(const std::string& s) {
    Tok t;
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && (s[i] == ' ' || s[i] == '\r')) i++;
        size_t j = i;
        while (j < s.size() && s[j] != ' ' && s[j] != '\r') j++;
        if (j > i) t.emplace_back(s.data() + i, j - i);
        i = j;
    }
    return t;
}
float F(std::string_view s) {
    float v = 0.0f;
    std::from_chars(s.data(), s.data() + s.size(), v);
    return v;
}
int I(std::string_view s) {
    int v = 0;
    std::from_chars(s.data(), s.data() + s.size(), v);
    return v;
}
uint32_t H(std::string_view s) {
    uint32_t v = 0;
    std::from_chars(s.data(), s.data() + s.size(), v, 16);
    return v;
}
uint64_t H64(std::string_view s) {
    uint64_t v = 0;
    std::from_chars(s.data(), s.data() + s.size(), v, 16);
    return v;
}
float HF(std::string_view s) { return std::bit_cast<float>(H(s)); }
std::vector<std::string_view> SplitC(std::string_view s, char c) {
    std::vector<std::string_view> v;
    size_t i = 0;
    while (true) {
        const size_t j = s.find(c, i);
        v.push_back(s.substr(i, j == std::string_view::npos ? std::string_view::npos : j - i));
        if (j == std::string_view::npos) break;
        i = j + 1;
    }
    return v;
}

struct XCross {
    uint32_t light;
    int floor, room;
    uint32_t level;
    bool outdoor;
    float headroom;
};
struct DumpRoom {
    int story = 0, id = 0;
    std::map<std::string, std::string, std::less<>> apex;
    bool hasInfo = false;
    int level = 0;
    bool indoor = false, yard = false, openings = false;
    std::vector<XCross> cross;
    std::vector<uintptr_t> same, lower, inner, innerUp;
    std::map<uint32_t, float> innerNorm; // (XN) an inner lamp's source room normalisation (InnerNormScale)
    std::map<std::string, uint64_t> gridRole;
    std::map<int, uint64_t> floorStory;
    r3d::Room r3;
    // (IndoorShadow) the other rooms its lamps' ray segments were tested in (OTHER blocks), by story and id
    std::map<std::pair<int, int>, r3d::Room> other;
    r3d::Room* curOther = nullptr;
    // the fills of the opening's outdoor total (TF lines), by index
    struct FillL {
        uint32_t light;
        float pos[3], c[3], pass, day;
        int slot;
    };
    struct Fill {
        float X[3], n[3];
        std::vector<FillL> l;
    };
    std::map<int, Fill> tf;
    float A(const char* k, float d = 0.0f) const {
        const auto it = apex.find(k);
        return it != apex.end() ? F(it->second) : d;
    }
};
struct Smp {
    int room = 0;
    size_t line = 0;
    char tag = 'T';
    uint32_t light = 0;
    float p[4] = {0, 0, 0, 1}, n[4] = {}, L[3] = {}, hook[3] = {}, wall[3] = {}, got[3] = {}, from[3] = {}, C[3] = {}, c0[3] = {};
    int raw = -1;
    float rawT = 1.0f;
    int paths = 0, batch = -1, count = 0, f0 = 1, f1 = 1, evals = 0;
    bool hasEv = false;
    OutdoorDelta::ProbeEval ev;
    int tf = -1; // (P7) the fill of its outdoor total
    // (room 0) M: a floor mask part (vis, all, the lamp's law dumped); E: an exit ground texel (the map's texel, the exit parts' luminance)
    float mVis = 0.0f, mAll = 0.0f, eLum = 0.0f;
    bool lawOk = false;
    uint32_t packed = 0;
};
std::unordered_map<uint32_t, GpuLampLaw::Lamp> g_laws; // (LIGHT lines) each light's GPU law

// ---- the game's wall test of a ray segment's room (IndoorPassImpl) ----
// The rooms the dump holds the walls of (the solving room's and its OTHER blocks), by story and id: their memory (the mode
// byte at +0x639 IndoorPassImpl sets) and walls. Any other room: the answer the game test recorded for that segment.
struct FakeRoom {
    std::array<uint8_t, 0x700> mem{};
    r3d::Room room;
};
std::map<std::pair<int, int>, FakeRoom> g_fakeRooms;
void* RoomByIdCached(void* mgr, int id) {
    if (const auto s = g_byMgr.find(reinterpret_cast<uintptr_t>(mgr)); s != g_byMgr.end())
        if (const auto it = g_fakeRooms.find({s->second->story, id}); it != g_fakeRooms.end()) return it->second.mem.data();
    return g_dummyRoom.data();
}
const OutdoorDelta::ProbeEval* g_recEv = nullptr;
int g_segCall = 0;
long g_segMissing = 0, g_segReplayed = 0, g_segRecorded = 0;
size_t g_line = 0;
struct Cmp;
Cmp* g_cSegKeep = nullptr;
Cmp* g_cSegPassed = nullptr;

// ---- comparisons ----
struct Cmp {
    std::string name;
    long n = 0, e4 = 0, e2 = 0;
    struct W {
        float err, rec, rep;
        size_t line;
    };
    std::vector<W> worst;
    void Add(float rec, float rep, size_t line) {
        n++;
        float e = std::fabs(rec - rep);
        if (!std::isfinite(e)) e = rec == rep ? 0.0f : 1e9f;
        e4 += e <= 1e-4f, e2 += e <= 0.01f;
        if (e > 1e-4f) {
            worst.push_back({e, rec, rep, line});
            std::sort(worst.begin(), worst.end(), [](const W& a, const W& b) { return a.err > b.err; });
            if (worst.size() > 4) worst.pop_back();
        }
    }
    void Print() const {
        if (!n) return;
        std::printf("  %-44s %7ld  within 1e-4 %6.2f%%  within 0.01 %6.2f%%\n", name.c_str(), n, 100.0 * e4 / n, 100.0 * e2 / n);
        for (const W& w : worst) std::printf("      line %zu: recorded %.6g replay %.6g\n", w.line, w.rec, w.rep);
    }
};
struct Fin { // the final colour of an evaluation, within 2/255
    std::string name;
    long n = 0, ok = 0;
    struct W {
        float err;
        size_t line;
        float rec, rep;
    };
    std::vector<W> worst;
    void Add(const float* rec, const float* rep, size_t line) {
        n++;
        const float e = std::max({std::fabs(rec[0] - rep[0]), std::fabs(rec[1] - rep[1]), std::fabs(rec[2] - rep[2])});
        ok += e <= 2.0f / 255.0f;
        if (e > 2.0f / 255.0f) {
            worst.push_back({e, line, rec[0] + rec[1] + rec[2], rep[0] + rep[1] + rep[2]});
            std::sort(worst.begin(), worst.end(), [](const W& a, const W& b) { return a.err > b.err; });
            if (worst.size() > 4) worst.pop_back();
        }
    }
    void Print() const {
        if (!n) return;
        std::printf("  %-44s %7ld  within 2/255 %6.2f%%\n", name.c_str(), n, 100.0 * ok / n);
        for (const W& w : worst) std::printf("      line %zu: off %.4f (recorded r+g+b %.5f, replay %.5f)\n", w.line, w.err, w.rec, w.rep);
    }
};
bool __fastcall RecordedWallTest(void* room, void*, void*, const float* from, const void* to, float* t) { // (= thiscall, ret 0x10)
    const int i = g_segCall++;
    const bool rec = g_recEv && i < g_recEv->nseg && i < 8 && g_recEv->seg[i].passed != 255;
    for (auto& [key, f] : g_fakeRooms)
        if (f.mem.data() == room) { // the game's 2D test of that room with its dumped walls, in the mode IndoorPassImpl set
            f.room.mode = f.mem[0x639];
            const float* b = static_cast<const float*>(to);
            float k = 1.0f;
            const bool ok = r3d::wallTest(f.room, f.room.idx2d, r3d::V{from[0], from[1], from[2]}, r3d::V{b[0], b[1], b[2]}, &k);
            g_segReplayed++;
            if (rec) g_cSegKeep->Add(g_recEv->seg[i].keep, k, g_line), g_cSegPassed->Add(g_recEv->seg[i].passed, ok ? 1.0f : 0.0f, g_line);
            *t = k;
            return ok;
        }
    if (!rec) {
        g_segMissing++;
        *t = 1.0f;
        return true;
    }
    g_segRecorded++;
    *t = g_recEv->seg[i].keep;
    return g_recEv->seg[i].passed != 0;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: replay <RoomLampProbe.txt>\n");
        return 2;
    }
    std::setvbuf(stdout, nullptr, _IONBF, 0); // (whatever happens, the lines printed so far)
    std::ifstream f(argv[1], std::ios::binary);
    if (!f) {
        std::perror(argv[1]);
        return 2;
    }
    std::vector<DumpRoom> rooms;
    std::vector<Smp> S;
    std::unordered_map<uint64_t, std::shared_ptr<StoryGrid>> grids;
    std::unordered_map<uint64_t, std::unique_ptr<FakeStory>> floors;
    StoryGrid* cg = nullptr;
    FakeStory* cf = nullptr;
    std::string line;
    size_t ln = 0;
    while (std::getline(f, line)) {
        ln++;
        const Tok t = Split(line);
        if (t.empty()) continue;
        const std::string_view k = t[0];
        const size_t n = t.size();
        if (k == "CONST" && n >= 3) {
            const uint32_t a = H(t[1]);
            const float v = HF(t[2]);
            using namespace r3d;
            switch (a) {
            case 0x00F98964: K_ZERO = v; break;
            case 0x00F98A58: K_TWO = v; break;
            case 0x00FAA210: K_NEG1 = v; break;
            case 0x00FB2F6C: K_FOUR = v; break;
            case 0x00FECE18: K_00625 = v; break;
            case 0x00FF31AC: K_STOP = v; break;
            case 0x0106B4B4: K_015 = v; break;
            case 0x0106D6CC: K_DROP = v; break;
            case 0x0107A538: K_ONE = v; break;
            case 0x011D0240: K_EPS = v; break;
            case 0x00F9A5AC: K_HALF = v; break;
            case 0x00FF307C: K_PEN_C = v; break;
            case 0x01158A50: K_WALL_Y = v; break;
            }
            continue;
        }
        if (k == "ROOM" && n >= 5) {
            rooms.emplace_back();
            rooms.back().story = I(t[2]), rooms.back().id = I(t[4]);
            continue;
        }
        if (rooms.empty()) continue;
        DumpRoom& R = rooms.back();
        // (OTHER blocks) another room's walls and records: the same lines with an O
        std::string_view kk = k;
        r3d::Room* tg = &R.r3;
        if (k.size() > 1 && k[0] == 'O' && (k == "OR" || k == "OB" || k == "OWALLS" || k == "OW" || k == "OIDX2D")) {
            if (!R.curOther) continue;
            kk = k.substr(1), tg = R.curOther;
        }
        if (k == "APEX") {
            for (size_t i = 1; i + 1 < n;) {
                if (t[i] == "dev") {
                    i++;
                    continue;
                }
                R.apex[std::string(t[i])] = std::string(t[i + 1]);
                i += 2;
            }
        } else if (k == "INFO" && n >= 11) {
            R.hasInfo = true;
            R.level = I(t[2]), R.indoor = I(t[6]) != 0, R.yard = I(t[8]) != 0, R.openings = I(t[10]) != 0;
        } else if (k == "XC" && n >= 12) {
            R.cross.push_back({H(t[1]), I(t[3]), I(t[5]), H(t[7]), I(t[9]) != 0, F(t[11])});
        } else if (k == "XN" && n >= 6) {
            R.innerNorm[H(t[1])] = F(t[5]);
        } else if ((k == "XS" || k == "XL" || k == "XI" || k == "XU") && n >= 2) {
            (k == "XS" ? R.same : k == "XL" ? R.lower : k == "XI" ? R.inner : R.innerUp).push_back(H(t[1]));
        } else if ((k == "GRIDHASH" || k == "GRIDREF") && n >= 5) {
            const uint64_t h = H64(k == "GRIDHASH" ? t[1] : t[2]);
            const size_t r = k == "GRIDHASH" ? 3 : 4;
            if (r < n) R.gridRole[std::string(t[r])] = h;
            if (k == "GRIDHASH") {
                auto& g = grids[h];
                g = std::make_shared<StoryGrid>();
                cg = g.get();
            }
        } else if (k == "GRID" && cg && n >= 38) {
            StoryGrid& g = *cg;
            g.serial = static_cast<uint32_t>(I(t[2])), g.story = I(t[4]), g.mgr = H(t[6]), g.w = I(t[8]), g.h = I(t[10]);
            g.minFoot = HF(t[12]), g.plane = HF(t[14]), g.cutGen = static_cast<uint32_t>(std::stoul(std::string(t[16])));
            for (int q = 0; q < 16; q++) g.xf.r[q / 4][q % 4] = HF(t[22 + q]);
            const size_t nt = static_cast<size_t>(g.w) * g.h;
            g.zl.assign(static_cast<size_t>(g.h + 1) * g.w, -1), g.xl.assign(static_cast<size_t>(g.w + 1) * g.h, -1);
            g.dg.assign(2 * nt, -1), g.flags.assign(nt, 0), g.ceil.assign(nt, 1e30f);
        } else if (k == "GZ" && cg && n >= 2) {
            const int Z = I(t[1]);
            if (Z < 0 || Z > cg->h || cg->zl.empty()) continue;
            for (int x = 0; x < cg->w && 2 + x < static_cast<int>(n); x++) cg->zl[static_cast<size_t>(Z) * cg->w + x] = I(t[2 + x]);
        } else if (k == "GX" && cg && n >= 2) {
            const int z = I(t[1]);
            if (z < 0 || z >= cg->h || cg->xl.empty()) continue;
            for (int X = 0; X <= cg->w && 2 + X < static_cast<int>(n); X++) cg->xl[static_cast<size_t>(z) * (cg->w + 1) + X] = I(t[2 + X]);
        } else if (k == "GT" && cg && n >= 2) {
            const int z = I(t[1]);
            if (z < 0 || z >= cg->h || cg->flags.empty()) continue;
            for (int x = 0; x < cg->w && 2 + x < static_cast<int>(n); x++) {
                const auto v = SplitC(t[2 + x], ',');
                if (v.size() < 4) continue;
                const size_t i = static_cast<size_t>(z) * cg->w + x;
                cg->dg[2 * i] = I(v[0]), cg->dg[2 * i + 1] = I(v[1]), cg->flags[i] = static_cast<uint8_t>(I(v[2]));
                if (v[3] != "-") cg->ceil[i] = HF(v[3]);
            }
        } else if (k == "GE" && cg && n >= 11) {
            GridEdge e;
            e.foot = HF(t[2]), e.base = HF(t[3]), e.topL = HF(t[4]), e.topM = HF(t[5]), e.topR = HF(t[6]);
            e.open = static_cast<uint16_t>(I(t[7])), e.faces = static_cast<uint8_t>(I(t[8])), e.half = I(t[9]) != 0, e.shortTop = I(t[10]) != 0;
            cg->edges.push_back(e);
        } else if (k == "GO" && cg && n >= 8 + WallOpenings::kProfile) {
            GridOpening o;
            size_t i = 2;
            o.sill = HF(t[i++]);
            for (float& h : o.head) h = HF(t[i++]);
            o.u0 = HF(t[i++]), o.u1 = HF(t[i++]), o.kind = static_cast<uint8_t>(I(t[i++])), o.shaped = static_cast<uint8_t>(I(t[i++])), o.glass = HF(t[i++]);
            cg->openings.push_back(o);
        } else if ((k == "FLOORHASH" || k == "FLOORREF") && n >= 5) {
            const uint64_t h = H64(k == "FLOORHASH" ? t[1] : t[2]);
            R.floorStory[I(t[k == "FLOORHASH" ? 3 : 4])] = h;
            if (k == "FLOORHASH") {
                auto& fs = floors[h];
                fs = std::make_unique<FakeStory>();
                cf = fs.get();
            }
        } else if (k == "FLOOR" && cf && n >= 32) {
            FakeStory& s = *cf;
            s.story = I(t[2]), s.realMgr = H(t[4]), s.realLevel = H(t[6]);
            s.tw = I(t[10]), s.th = I(t[11]), s.kw = I(t[13]), s.kh = I(t[14]);
            s.Put(s.mgr, 0x88, s.story), s.Put(s.mgr, 0x98, HF(t[8])), s.Put(s.mgr, 0xD4, 0.0f);
            for (int q = 0; q < 16; q++) s.Put(s.mgr, 0xE0 + 4 * q, HF(t[16 + q]));
            s.tiles.assign(static_cast<size_t>(s.tw) * s.th, {});
            s.tilePtr.assign(static_cast<size_t>(s.tw) * s.th, 0);
            s.Put(s.mgr, 0x260, reinterpret_cast<uintptr_t>(s.tilePtr.data())), s.Put(s.mgr, 0x264, s.tw), s.Put(s.mgr, 0x268, s.th);
            s.data.assign(static_cast<size_t>(s.kw) * s.kh * 40, 0);
            for (size_t c = 0; c < static_cast<size_t>(s.kw) * s.kh; c++)
                for (int q = 0; q < 4; q++) s.Put(s.data, c * 40 + 8 + q * 8, 0xFFFFFFF8u), s.Put(s.data, c * 40 + 12 + q * 8, 0xFFFFFFFFu);
            s.Put(s.grid, 0, reinterpret_cast<uintptr_t>(s.data.data())), s.Put(s.grid, 0x10, s.kw), s.Put(s.grid, 0x14, s.kh);
            s.Put(s.level, 0x264, reinterpret_cast<uintptr_t>(s.grid.data()));
            g_byMgr[s.Mgr()] = cf, g_byLevel[s.Level()] = cf;
        } else if (k == "FT" && cf && n >= 2) {
            const int z = I(t[1]);
            for (int x = 0; x < cf->tw && 2 + x < static_cast<int>(n) && z < cf->th; x++) {
                if (t[2 + x] == "-") continue;
                const auto v = SplitC(t[2 + x], ',');
                if (v.size() < 5) continue;
                auto& tile = cf->tiles[static_cast<size_t>(z) * cf->tw + x];
                const float h = HF(v[0]);
                std::memcpy(tile.data() + 0x78, &h, 4);
                for (int q = 0; q < 4; q++) {
                    const int r = I(v[1 + q]);
                    std::memcpy(tile.data() + 0x7C + q * 0x14, &r, 4);
                }
                cf->tilePtr[static_cast<size_t>(z) * cf->tw + x] = reinterpret_cast<uintptr_t>(tile.data());
            }
        } else if (k == "FK" && cf && n >= 2) {
            const int z = I(t[1]);
            for (int x = 0; x < cf->kw && 2 + x < static_cast<int>(n) && z < cf->kh; x++) {
                if (t[2 + x] == "-") continue;
                const auto v = SplitC(t[2 + x], ',');
                for (int q = 0; q < 4 && q < static_cast<int>(v.size()); q++) {
                    const auto kv = SplitC(v[q], '.');
                    if (kv.size() < 2) continue;
                    const size_t c = static_cast<size_t>(z) * cf->kw + x;
                    cf->Put(cf->data, c * 40 + 8 + q * 8, H(kv[0])), cf->Put(cf->data, c * 40 + 12 + q * 8, H(kv[1]));
                }
            }
        } else if (kk == "R" && n >= 10) { // (the 3D blockers and the walls, as tools/room_lamp_3d_replay reads them)
            r3d::Record r;
            r.c = {F(t[3]), F(t[4]), F(t[5])};
            r.r2 = F(t[8]);
            tg->rec.push_back(r);
        } else if (kk == "B" && n >= 71 && !tg->rec.empty()) {
            r3d::Blocker b;
            b.vt = H(t[3]);
            for (int i = 0; i < 64; ++i) b.f[i] = HF(t[7 + i]);
            tg->rec.back().b.push_back(b);
        } else if (kk == "WALLS" && n >= 4) {
            tg->mode = I(t[3]);
        } else if (kk == "W" && n >= 68) {
            r3d::Wall w;
            for (int i = 0; i < 64; ++i) w.f[i] = HF(t[4 + i]);
            tg->walls.push_back(w);
        } else if (kk == "IDX2D" && n >= 2) {
            for (size_t i = 2; i < n; ++i) tg->idx2d.push_back(I(t[i]));
        } else if (k == "OTHER" && n >= 5) {
            R.curOther = &R.other[{I(t[2]), I(t[4])}];
        } else if (k == "TF" && n >= 19) {
            DumpRoom::Fill& fl = R.tf[I(t[1])];
            for (int q = 0; q < 3; q++) fl.X[q] = F(t[5 + q]), fl.n[q] = F(t[9 + q]);
            for (size_t i = 19; i + 15 < n && t[i] == "l"; i += 16) {
                DumpRoom::FillL e{H(t[i + 1])};
                for (int q = 0; q < 3; q++) e.pos[q] = F(t[i + 3 + q]), e.c[q] = F(t[i + 7 + q]);
                e.pass = F(t[i + 11]), e.day = F(t[i + 13]), e.slot = I(t[i + 15]);
                fl.l.push_back(e);
            }
        } else if (k == "LIGHT" && n >= 38 && t[17] == "law") {
            uint32_t w[20];
            for (int q = 0; q < 20; q++) w[q] = H(t[18 + q]);
            static_assert(sizeof(GpuLampLaw::Lamp) == sizeof w);
            std::memcpy(&g_laws[H(t[1])], w, sizeof w);
            GpuLampLaw::Lamp& law = g_laws[H(t[1])];
            if (t[3] == "11" && law.cmax == 2.0f) law.cmax *= 3.33333325f; // (dumps before the street cap fix: the game scales the capped light)
        } else if (k == "M" && n >= 26) { // (room 0) a floor mask part
            Smp s;
            s.room = static_cast<int>(rooms.size()) - 1, s.line = ln, s.tag = 'M', s.light = H(t[1]);
            for (int i = 0; i < 3; i++) s.p[i] = F(t[3 + i]), s.n[i] = F(t[7 + i]), s.L[i] = F(t[11 + i]);
            s.lawOk = I(t[15]) != 0, s.mVis = F(t[17]), s.mAll = F(t[19]);
            S.push_back(s);
        } else if (k == "E" && n >= 19) { // (room 0) an exit ground texel
            Smp s;
            s.room = static_cast<int>(rooms.size()) - 1, s.line = ln, s.tag = 'E';
            for (int i = 0; i < 3; i++) s.p[i] = F(t[3 + i]), s.n[i] = F(t[7 + i]);
            s.packed = H(t[13]);
            for (size_t i = 19; i + 3 < n; i += 4) {
                const float c[3] = {F(t[i + 1]), F(t[i + 2]), F(t[i + 3])};
                s.eLum += OutdoorDeltaModel::Lum3(c);
            }
            S.push_back(s);
        } else if ((k == "T" || k == "Z") && n >= 26) {
            Smp s;
            s.room = static_cast<int>(rooms.size()) - 1, s.line = ln, s.tag = k[0], s.light = H(t[1]);
            for (int i = 0; i < 3; i++) {
                s.p[i] = F(t[3 + i]), s.n[i] = F(t[7 + i]), s.L[i] = F(t[11 + i]);
                s.hook[i] = F(t[15 + i]), s.wall[i] = F(t[19 + i]), s.got[i] = F(t[23 + i]);
                s.from[i] = s.L[i], s.C[i] = s.p[i];
            }
            OutdoorDelta::ProbeEval& e = s.ev;
            for (size_t i = 26; i < n; i++) {
                const std::string_view w = t[i];
                const auto has = [&](size_t m) { return i + m < n; };
                if (w == "from" && has(3)) s.from[0] = F(t[i + 1]), s.from[1] = F(t[i + 2]), s.from[2] = F(t[i + 3]);
                else if (w == "raw" && has(2) && t[i + 1] != "none") s.raw = I(t[i + 1]), s.rawT = F(t[i + 2]);
                else if (w == "path" && has(1)) s.paths = I(t[i + 1]);
                else if (w == "batch" && has(10)) {
                    s.batch = I(t[i + 1]), s.count = I(t[i + 3]), s.f0 = I(t[i + 5]), s.f1 = I(t[i + 6]);
                    s.C[0] = F(t[i + 8]), s.C[1] = F(t[i + 9]), s.C[2] = F(t[i + 10]);
                } else if (w == "c0" && has(5)) s.c0[0] = F(t[i + 1]), s.c0[1] = F(t[i + 2]), s.c0[2] = F(t[i + 3]), s.evals = I(t[i + 5]);
                else if (w == "ev" && has(16)) {
                    s.hasEv = true;
                    e.set = static_cast<uint8_t>(I(t[i + 2]));
                    e.c0[0] = F(t[i + 4]), e.c0[1] = F(t[i + 5]), e.c0[2] = F(t[i + 6]);
                    e.mount = F(t[i + 8]), e.day = F(t[i + 10]), e.reach = F(t[i + 12]);
                    e.cut = static_cast<uint8_t>(I(t[i + 14])), e.early = static_cast<uint8_t>(I(t[i + 16]));
                } else if (w == "p6" && has(4)) {
                    e.has |= 1;
                    for (int q = 0; q < 4; q++) e.p6[q] = F(t[i + 1 + q]);
                } else if (w == "p7" && has(10)) {
                    e.has |= 2;
                    for (int q = 0; q < 10; q++) e.p7[q] = F(t[i + 1 + q]);
                } else if (w == "p8" && has(9)) {
                    e.has |= 4;
                    for (int q = 0; q < 3; q++) e.p8[q] = F(t[i + 1 + q]);
                    e.why = static_cast<int8_t>(I(t[i + 5])), e.stage = static_cast<int8_t>(I(t[i + 7])), e.nseg = static_cast<uint8_t>(I(t[i + 9]));
                    int sg = 0;
                    for (size_t j = i + 10; j + 6 < n && t[j] == "seg" && sg < 8; j += 7, sg++) {
                        auto& g = e.seg[sg];
                        g.story = static_cast<int8_t>(I(t[j + 1])), g.room = static_cast<int16_t>(I(t[j + 2]));
                        g.t0 = F(t[j + 3]), g.t1 = F(t[j + 4]), g.keep = F(t[j + 5]), g.passed = static_cast<uint8_t>(I(t[j + 6]));
                    }
                } else if (w == "ent" && has(3)) e.entry = 1, e.ent[0] = F(t[i + 1]), e.ent[1] = F(t[i + 2]), e.ent[2] = F(t[i + 3]);
                else if (w == "fade" && has(2)) e.has |= 8, e.fade = F(t[i + 1]), e.fadeThr = F(t[i + 2]);
                else if (w == "tf" && has(1)) s.tf = I(t[i + 1]);
            }
            S.push_back(s);
        }
    }
    for (auto& [h, g] : grids) NoteNearEdges(*g);
    long nT = 0, nZ = 0, nEv = 0;
    long nM = 0, nE = 0;
    for (const Smp& s : S) (s.tag == 'T' ? nT : s.tag == 'Z' ? nZ : s.tag == 'M' ? nM : nE)++, nEv += s.hasEv;
    std::printf("rooms %zu, grids %zu, floors %zu, texel lines %zu (T %ld, Z %ld), with Apex's record %ld\n", rooms.size(), grids.size(), floors.size(), S.size(), nT, nZ, nEv);
    if (nM || nE) std::printf("(room 0: floor mask lines %ld, exit ground lines %ld, among the texel lines above)\n", nM, nE);

    kWallTest = reinterpret_cast<uintptr_t>(&RecordedWallTest);
    Cmp cSegKeep{"P8 other room's wall test keep (dumped walls)"}, cSegPassed{"P8 other room's wall test passed (0/1)"};
    g_cSegKeep = &cSegKeep, g_cSegPassed = &cSegPassed;
    Cmp c7total{"P7 total recomputed from its TF fill"}, c7cutRe{"P7 entry cut (recomputed total)"}, cTfPass{"TF a room-0 light's pass at X (YardPass)"};
    Fin fTfLaw{"TF a room-0 light at X: GPU law vs the game"};
    long tfFills = 0, tfNoLaw = 0;
    Fin m0Inner{"room 0: model got, InnerShadow (set 1)"}, m0Own{"room 0: model got, no Apex record"}, m0Other{"room 0: model got, CrossFloorShadow (not replayed)"};
    Cmp cMall{"room 0 mask: no-walls copy vs the law (KeptWrapLum)"}, cMvis{"room 0 mask: walls-tested copy vs law x wall test"};
    Cmp cEtile{"room 0 exit ground: under a floor tile (0 expected)"}, cEalpha{"room 0 exit ground: ExitAlpha vs the map's alpha"};
    long mNoLaw = 0;
    Cmp cMount{"P1 MountedNdl factor"}, cCut{"P2 floor cut (0/1)"}, cEarly{"P5 threshold early-out (0/1)"};
    Cmp c6pass{"P6 InnerShadow pass"}, c6k{"P6 norm scale"};
    Cmp c7pass{"P7 opening pass"}, c7t{"P7 tOpen"}, c7times{"P7 times"}, c7law{"P7 law"}, c7gain{"P7 gain"}, c7cut{"P7 entry cut (recorded total)"},
        c7k{"P7 factor k (norm at solve start)"}, c7kEnd{"P7 factor k (norm at solve end)"}, c7q{"P7 MaxResponse q"}, c7thr{"P7 thrScale"};
    Cmp c8mine{"P8 IndoorShadow mine"}, c8slab{"P8 slab share"}, c8trans{"P8 walls' transmission"}, c8stage{"P8 stage (equal = 0 off)"},
        c8why{"P8 why (equal = 0 off)"}, c8nseg{"P8 segments"}, c8seg{"P8 segment t1"}, c8room{"P8 segment room"}, c8ent{"P8 entry point (max axis)"};
    Cmp c9{"P9 soft cut factor"}, c9thr{"P9 threshold"};
    Fin fRec{"chain with the recorded factors vs hook"}, fCut{"final: floor-cut evaluations"}, fInner{"final: InnerShadow (set 1)"},
        fOpen{"final: opening (set 2)"}, fCross{"final: IndoorShadow (set 3)"};
    Fin mAll{"model got, all texel lines"}, mForeign{"model got, lamps of other rooms/stories"}, mForeignHook{"recorded hook x replayed 2D/3D, foreign"}, mForeignT{"model got, foreign, T lines (lit)"}, mForeignWall{"model got, foreign with the game's wall test run"},
        mOwn{"model got, own lamps (hook recorded)"};
    long noCrossLevel = 0, crossLevels = 0, noInfo = 0;
    Fin mMoveOwn{"move worker, own lamps"}, mMoveForeign{"move worker, plain lamps of other rooms"}, mMoveT{"move worker, T lines (lit)"};
    long mvOut = 0, mvDayReach = 0, mvCut = 0;
    Fin mMove0{"move worker, room 0 (inner lamps)"}, mMove0T{"move worker, room 0, T lines (lit)"}, mMove0Ev{"move worker, room 0, with a recorded evaluation"},
        mMove0NoEv{"move worker, room 0, its record dropped"};
    std::map<std::string, long> m0Out;
    Fin fLaw{"T/Z: dumped law x GameOverLaw vs the game's evaluation"}, fLawTube{"T/Z: the same, TubeLights (GpuLampLaw::Tube)"};
    long lawElsewhere = 0;
    int mvUnknown = 0, mvKey = -1, mvRoom = -1;
    float mvL[3] = {};
    std::vector<int> mvList;

    int cur = -1;
    FakeLot lot;
    RoomInfo info;
    std::array<uint8_t, 0x700> room{};
    std::vector<uint32_t> walls;
    std::vector<int> list;
    int listKey = -1;
    float listL[3] = {};
    for (const Smp& s : S) {
        const DumpRoom& R = rooms[s.room];
        if (s.room != cur) { // the room's state, its record, its stories and grids
            cur = s.room;
            lot = FakeLot{};
            g_realMgr.clear();
            for (const auto& [k, h] : R.floorStory)
                if (const auto it = floors.find(h); it != floors.end() && k >= 0 && k < 8) lot.s[k] = it->second.get(), g_realMgr[it->second->realMgr] = it->second.get();
            const auto grid = [&](const char* role) -> std::shared_ptr<const StoryGrid> {
                const auto it = R.gridRole.find(role);
                if (it == R.gridRole.end()) return nullptr;
                const auto g = grids.find(it->second);
                if (g == grids.end()) return nullptr;
                if (g->second->story >= 0 && g->second->story < 8 && lot.s[g->second->story]) g->second->mgr = lot.s[g->second->story]->Mgr(); // (InnerDownGateImpl's LevelManager check)
                return g->second;
            };
            info = RoomInfo{};
            info.level = R.hasInfo ? R.level : R.story, info.id = R.id, info.indoor = R.indoor, info.yard = R.yard, info.openings = R.openings;
            info.tracker = reinterpret_cast<uintptr_t>(&lot);
            info.mgr = lot.s[R.story] ? lot.s[R.story]->Mgr() : 0;
            for (const XCross& x : R.cross) {
                Cross c{x.light, x.floor};
                c.room = x.room, c.outdoor = x.outdoor, c.upwardHeadroom = x.headroom;
                for (const FakeStory* st : lot.s)
                    if (st && st->realLevel == x.level) c.level = st->Level();
                crossLevels++, noCrossLevel += c.level == 0;
                info.cross.push_back(c);
            }
            std::sort(info.cross.begin(), info.cross.end());
            info.same = R.same, info.lower = R.lower, info.inner = R.inner, info.innerUp = R.innerUp;
            for (auto* v : {&info.same, &info.lower, &info.inner, &info.innerUp}) std::sort(v->begin(), v->end());
            info.grid[0] = grid("grid0"), info.grid[1] = grid("grid1"), info.below = grid("below"), info.openGrid[0] = grid("open0"), info.openGrid[1] = grid("open1");
            info.innerGrid[0] = grid("inner0"), info.innerGrid[1] = grid("inner1"), info.upGrid[0] = grid("up0"), info.upGrid[1] = grid("up1");
            for (int k = 0; k < 8; k++) info.mid[k] = grid(std::format("mid{}", k).c_str()), info.lampGrid[k] = grid(std::format("lamp{}", k).c_str());
            room.fill(0);
            const uintptr_t m = info.mgr;
            const int id = R.id;
            const float norm0 = R.A("norm0", 1.0f), thr = R.A("thr");
            std::memcpy(room.data(), &m, 4), std::memcpy(room.data() + 0xC, &id, 4), std::memcpy(room.data() + 0x160, &norm0, 4), std::memcpy(room.data() + 0x63C, &thr, 4);
            room[0x18] = static_cast<uint8_t>(R.A("outdoor")), room[0x639] = static_cast<uint8_t>(R.A("soft"));
            Penumbra::g_radius = R.A("pen", Penumbra::kDefaultRadius), Penumbra::g_on = R.A("penOn", 1) != 0.0f;
            t_mapAA = R.A("aa"), g_hardWallTops = R.A("hardTops") != 0.0f;
            DevSwitches::indoorLampWalls = R.A("indoorLampWalls", 1) != 0.0f, DevSwitches::softSlabEdges = R.A("softSlabEdges", 1) != 0.0f;
            DevSwitches::softOpeningEdges = R.A("softOpeningEdges", 1) != 0.0f, DevSwitches::steadyIndoorLight = R.A("steadyIndoorLight", 1) != 0.0f;
            DevSwitches::innerNormSettle = R.A("innerNormSettle", 1) != 0.0f;
            g_openWall = R.A("openWall", 1), g_openGround = R.A("openGround", 1);
            g_ctx = SolveCtx{};
            g_ctx.info = R.hasInfo ? &info : nullptr, g_ctx.thr = thr, g_ctx.soft = room[0x639], g_ctx.fade = R.A("softCut") != 0.0f;
            t_solveRoom = room.data();
            listKey = -1;
            if (!R.hasInfo) noInfo++;
            g_fakeRooms.clear();
            g_fakeRooms[{R.story, R.id}].room = R.r3;
            for (const auto& [key, o] : R.other) g_fakeRooms[key].room = o;
            for (const auto& [idx, fl] : R.tf) { // each fill once: its lights' pass at X, and their GPU law there
                tfFills++;
                for (const DumpRoom::FillL& e : fl.l) {
                    std::memcpy(g_lamp, e.pos, sizeof e.pos), g_lamp[3] = 1.0f;
                    alignas(16) float lw[4] = {};
                    GridWhy why{};
                    cTfPass.Add(e.pass, YardPass(info.grid[0].get(), info.grid[1].get(), reinterpret_cast<void*>(static_cast<uintptr_t>(e.light)), fl.X, lw, why), 0);
                    const auto lawIt = g_laws.find(e.light);
                    if (lawIt == g_laws.end()) {
                        tfNoLaw++;
                        continue;
                    }
                    float out[3];
                    GpuLampLaw::Eval(lawIt->second, fl.X, fl.n, 0.0f, out);
                    fTfLaw.Add(e.c, out, 0);
                }
            }
        }
        const char flags[2] = {static_cast<char>(s.f0), static_cast<char>(s.f1)};
        g_ctx.flags = flags;
        g_line = s.line;
        if (s.tag == 'T' || s.tag == 'Z') { // the GPU law (the LIGHT line's, the lamp at that place) against the game's evaluation per entry
            const auto lawIt = g_laws.find(s.light);
            if (lawIt != g_laws.end()) {
                const GpuLampLaw::Lamp& lw = lawIt->second;
                if (std::fabs(lw.pos[0] - s.L[0]) + std::fabs(lw.pos[1] - s.L[1]) + std::fabs(lw.pos[2] - s.L[2]) > 1e-3f) lawElsewhere++;
                else {
                    float game[3], law[3];
                    for (int i = 0; i < 3; i++) game[i] = s.hasEv ? s.ev.c0[i] : s.c0[i] / static_cast<float>(std::max(s.evals, 1));
                    GpuLampLaw::Eval(lw, s.p, s.n, 0.0f, law);
                    const float gol = RoomLampMove::GameOverLaw(lw, s.p, s.n);
                    for (float& v : law) v *= gol;
                    (GpuLampLaw::IsTube(lw) ? fLawTube : fLaw).Add(game, law, s.line);
                }
            }
        }
        if (s.tag == 'M') { // (room 0) the floor mask's copies: the no-walls one is the law's luminance (N.l ignored), the walls-tested one x the game's wall test
            const auto lawIt = g_laws.find(s.light);
            if (!s.lawOk || lawIt == g_laws.end()) {
                mNoLaw++;
                continue;
            }
            const float all = RtLampModel::KeptWrapLum(lawIt->second, false, s.p);
            float t = 1.0f;
            const bool ok = r3d::wallTest(R.r3, R.r3.idx2d, r3d::V{s.L[0], s.L[1], s.L[2]}, r3d::V{s.p[0], s.p[1], s.p[2]}, &t);
            cMall.Add(s.mAll, all, s.line), cMvis.Add(s.mVis, ok ? all * t : 0.0f, s.line);
            continue;
        }
        if (s.tag == 'E') { // (room 0) an exit ground texel: not under a floor tile, its alpha the exit lamps' light
            cEtile.Add(0.0f, GroundFloorTileAt(room.data(), s.p) ? 1.0f : 0.0f, s.line);
            cEalpha.Add(static_cast<float>(s.packed >> 24) / 255.0f, RtLampModel::ExitAlpha(s.eLum), s.line);
            continue;
        }
        float hookModel[3] = {s.hook[0], s.hook[1], s.hook[2]};
        const bool foreign = s.hasEv;
        if (s.hasEv) {
            const OutdoorDelta::ProbeEval& e = s.ev;
            const size_t L = s.line;
            void* light = reinterpret_cast<void*>(static_cast<uintptr_t>(s.light));
            std::memcpy(g_lamp, s.L, sizeof s.L), g_lamp[3] = 1.0f;
            float c[4] = {e.c0[0], e.c0[1], e.c0[2], 0.0f}, cr[4] = {e.c0[0], e.c0[1], e.c0[2], 0.0f}; // replayed, recorded factors
            const auto lit = [](const float* v) { return v[0] > 0.0f || v[1] > 0.0f || v[2] > 0.0f; };
            const auto mul = [](float* v, float k, int ch = 4) { for (int i = 0; i < ch; i++) v[i] *= k; };
            bool done = false;
            float thrScale = 1.0f, thrScaleRec = 1.0f;
            // P1 MountedNdl (as LightEvalHook)
            float mount = 1.0f;
            if (lit(c) && std::fabs(s.n[1]) < 0.5f) {
                const float l[3] = {s.L[0] - s.p[0], s.L[1] - s.p[1], s.L[2] - s.p[2]};
                const float plane = s.n[0] * l[0] + s.n[1] * l[1] + s.n[2] * l[2];
                if (plane > 0.0f && plane < GpuLampLaw::kMountReach) {
                    const float d = ApexMath::Sqrt(l[0] * l[0] + l[1] * l[1] + l[2] * l[2]);
                    const float ndl = d > 1e-4f ? plane / d : 1.0f;
                    const float raised = GpuLampLaw::MountedNdl(ndl, plane, s.n[1]);
                    if (raised > ndl && ndl > 1e-3f) mount = std::min(raised / ndl, 8.0f);
                }
            }
            cMount.Add(e.mount, mount, L);
            mul(c, mount, 3), mul(cr, e.mount, 3);
            // P2 the floor under this story
            if (lit(c) || lit(cr)) {
                const bool cut = OwnFloorBlocksAt(room.data(), s.L, s.p);
                cCut.Add(e.cut, cut ? 1.0f : 0.0f, L);
                if (cut) std::fill(c, c + 4, 0.0f);
                if (e.cut) std::fill(cr, cr + 4, 0.0f);
                if (cut || e.cut) done = true; // (the hook returns; the fade still runs)
            }
            if (!done) {
                if (lit(c) && e.day != 1.0f) mul(c, e.day);
                if (lit(cr) && e.day != 1.0f) mul(cr, e.day);
                if (e.reach != 1.0f) mul(c, e.reach), mul(cr, e.reach);
            }
            if (!done && g_ctx.info) {
                const bool early = g_ctx.thr > (c[2] + c[1]) + c[0];
                cEarly.Add(e.early, early ? 1.0f : 0.0f, L);
                if (early || e.early) done = true;
            }
            if (!done && g_ctx.info) {
                OutdoorDelta::ProbeEval rep{};
                t_pe = &rep;
                g_recEv = &e, g_segCall = 0;
                const uintptr_t l = s.light;
                const bool inner = std::binary_search(info.inner.begin(), info.inner.end(), l);
                const bool same = std::binary_search(info.same.begin(), info.same.end(), l);
                const bool openLamp = info.openings && same;
                if (inner) { // P6 InnerShadow
                    alignas(16) float lw[4] = {};
                    GridWhy why{};
                    const float pass = InnerPass(info, light, s.p, lw, why);
                    const float k = pass > 0.0f ? (e.p6[2] < 0.0f ? 1.0f : InnerLight::NormScale(e.p6[2], e.p6[3], false)) : 1.0f;
                    c6pass.Add(e.p6[0], pass, L), c6k.Add(e.p6[1], k, L);
                    if (pass < 1.0f || k != 1.0f) mul(c, std::max(0.0f, pass) * k);
                    if (e.p6[0] < 1.0f || e.p6[1] != 1.0f) mul(cr, std::max(0.0f, e.p6[0]) * e.p6[1]);
                } else if (openLamp || (info.yard && same)) { // P7 the opening
                    const int times = info.level == 0 || (info.level == 1 && std::binary_search(info.lower.begin(), info.lower.end(), l)) ? 2 : 1;
                    const float law = openLamp ? std::max({c[0], c[1], c[2]}) : 0.0f;
                    const float hue[3] = {c[0], c[1], c[2]};
                    alignas(16) float lw[4] = {};
                    GridWhy why{};
                    const float pass = info.openings ? OpenLampPass(info, light, s.p, lw, why) : YardPass(info.grid[0].get(), info.grid[1].get(), light, s.p, lw, why);
                    c7pass.Add(e.p7[0], pass, L), c7t.Add(e.p7[1], why.tOpen, L), c7times.Add(e.p7[2], static_cast<float>(times), L), c7law.Add(e.p7[3], law, L);
                    if (pass < 1.0f) mul(c, std::max(0.0f, pass));
                    if (e.p7[0] < 1.0f) mul(cr, std::max(0.0f, e.p7[0]));
                    if (openLamp && lit(c)) {
                        const float gain = OpeningLampBalance::OpeningGain(lw, s.p, s.n, why.tOpen);
                        const float cut = OpeningLampBalance::EntryScale(law, times, gain, e.p7[5]);
                        if (cut != 1.0f) mul(c, cut);
                        const float k = OpeningLampFactor(s.n, room.data());
                        const float norm1 = R.A("norm1", 1.0f);
                        std::array<uint8_t, 0x700> roomEnd = room;
                        std::memcpy(roomEnd.data() + 0x160, &norm1, 4);
                        const float kEnd = OpeningLampFactor(s.n, roomEnd.data());
                        mul(c, k);
                        float scaled = cut * k, q = -1.0f;
                        if (!RoomOutdoorFlag(room.data())) {
                            q = OpeningLampBalance::MaxResponse(c, static_cast<float>(times) * InnerLight::Norm(RoomNormOf(room.data())));
                            if (q != 1.0f) mul(c, q);
                            scaled *= q;
                        }
                        if (scaled > 0.0f && scaled < 1.0f && ApexMath::IsFinite(scaled)) thrScale = scaled;
                        c7gain.Add(e.p7[4], gain, L), c7cut.Add(e.p7[6], cut, L), c7k.Add(e.p7[7], k, L), c7kEnd.Add(e.p7[7], kEnd, L), c7q.Add(e.p7[8], q, L);
                        if (const auto fi = R.tf.find(s.tf); fi != R.tf.end()) { // the total from the fill (OpeningOutdoorTotal)
                            float tot[3] = {}, own[3] = {};
                            bool hasOwn = false;
                            for (const DumpRoom::FillL& f : fi->second.l) {
                                const float p = f.pass * f.day;
                                if (!(p > 0.0f)) continue;
                                for (int q = 0; q < 3; q++) {
                                    const float v = std::isfinite(f.c[q]) ? std::max(0.0f, f.c[q] * p) : 0.0f;
                                    tot[q] += v;
                                    if (f.slot >= 0 && f.light == s.light) own[q] += v, hasOwn = true;
                                }
                            }
                            const float R7 = law * static_cast<float>(times) * std::max(gain, 1.0f), top = std::max({hue[0], hue[1], hue[2]});
                            float T = -1.0f;
                            if (hasOwn && top > 0.0f && R7 > 0.0f) {
                                T = 0.0f;
                                for (int q = 0; q < 3; q++) T = std::max(T, hue[q] / top * R7 + std::max(0.0f, tot[q] - own[q]));
                            }
                            c7total.Add(e.p7[5], T, L), c7cutRe.Add(e.p7[6], OpeningLampBalance::EntryScale(law, times, gain, T), L);
                        }
                        c7thr.Add(e.p7[9], thrScale, L);
                    }
                    if (lit(cr) && e.p7[4] >= 0.0f) {
                        if (e.p7[6] != 1.0f) mul(cr, e.p7[6]);
                        mul(cr, e.p7[7]);
                        if (e.p7[8] >= 0.0f && e.p7[8] != 1.0f) mul(cr, e.p7[8]);
                        thrScaleRec = e.p7[9];
                    }
                } else if (info.indoor) { // P8 IndoorShadow (the cross lamps only)
                    if (const Cross* cx = FindCross(info, l); cx && lit(c)) {
                        PassWhy why = PassWhy::Passed;
                        RayEntry entry;
                        const float mine = IndoorPassImpl(info, *cx, light, s.p, why, &entry);
                        c8mine.Add(e.p8[0], mine, L), c8slab.Add(e.p8[1], rep.p8[1], L), c8trans.Add(e.p8[2], rep.p8[2], L);
                        c8stage.Add(0.0f, e.stage == rep.stage ? 0.0f : 1.0f, L), c8why.Add(0.0f, e.why == static_cast<int8_t>(why) ? 0.0f : 1.0f, L);
                        c8nseg.Add(e.nseg, rep.nseg, L);
                        for (int i = 0; i < std::min<int>(e.nseg, rep.nseg) && i < 8; i++) c8seg.Add(e.seg[i].t1, rep.seg[i].t1, L), c8room.Add(e.seg[i].room, rep.seg[i].room, L);
                        if (e.entry && mine > 0.0f && entry.valid)
                            c8ent.Add(0.0f, std::max({std::fabs(e.ent[0] - entry.at[0]), std::fabs(e.ent[1] - entry.at[1]), std::fabs(e.ent[2] - entry.at[2])}), L);
                        if (mine < 1.0f) mul(c, std::max(0.0f, mine));
                    }
                    if ((e.has & 4) && e.p8[0] < 1.0f) mul(cr, std::max(0.0f, e.p8[0]));
                }
                t_pe = nullptr, g_recEv = nullptr;
            }
            // P9 the soft light cut, on every way out
            if (e.has & 8) {
                const float f9 = FadeAtThreshold(c, thrScale);
                c9.Add(e.fade, f9, s.line), c9thr.Add(e.fadeThr, g_ctx.thr * thrScale, s.line);
                FadeAtThreshold(cr, thrScaleRec);
            }
            const float ev = static_cast<float>(std::max(s.evals, 1));
            float rep[3], rec[3];
            for (int i = 0; i < 3; i++) rep[i] = c[i] * ev, rec[i] = cr[i] * ev;
            fRec.Add(s.hook, rec, s.line);
            (e.cut ? fCut : e.set == 1 ? fInner : e.set == 2 ? fOpen : e.set == 3 ? fCross : fCut).Add(s.hook, rep, s.line);
            std::memcpy(hookModel, rep, sizeof rep);
        }
        // the game's 2D wall test (raw none: Apex skipped it, or the game did not run it) and its 3D test, as the probe saw them
        const r3d::Room& rr = R.r3;
        const r3d::V Lv{s.L[0], s.L[1], s.L[2]}, pv{s.p[0], s.p[1], s.p[2]}, fv{s.from[0], s.from[1], s.from[2]}, Cv{s.C[0], s.C[1], s.C[2]};
        float wallShare = 1.0f;
        if (s.raw >= 0) {
            const bool built = s.count >= 4 && (s.f0 || s.f1);
            if (built && (listKey != s.batch || listL[0] != s.L[0] || listL[1] != s.L[1] || listL[2] != s.L[2])) {
                list = r3d::wallCull(rr, Cv, Lv), listKey = s.batch;
                std::memcpy(listL, s.L, sizeof listL);
            }
            float t = 1.0f;
            const bool ok = r3d::wallTest(rr, built ? list : rr.idx2d, fv, pv, &t);
            wallShare = ok ? t : 0.0f;
        }
        float F3 = wallShare;
        int unk = 0;
        if (F3 > 0.0f && s.f1) {
            const float prod = F3 * r3d::occlude(rr, Lv, pv, &unk);
            F3 = r3d::K_DROP > prod ? 0.0f : prod;
        }
        float model[3], modelRecHook[3];
        for (int i = 0; i < 3; i++) model[i] = hookModel[i] * F3, modelRecHook[i] = s.hook[i] * F3;
        if (R.id == 0) { // (room 0) its lamps carried outdoors apart; out of the move worker's scope
            ((s.paths & 8) ? m0Other : s.hasEv && s.ev.set == 1 ? m0Inner : m0Own).Add(s.got, model, s.line);
            mvOut++;
            // (GPU room lamps stage 3b) the move worker's room-0 part (room_lamp_move.h Part0At with the production InnerPassAt, at
            // the recorded position) drawn as the GPU draws it (KeptAdd), the game's evaluation standing for the law; out of its
            // scope (what LevelLightShare::Room0MovePath refuses, or what the dump lacks) counted by reason
            const uintptr_t l = s.light;
            const char* why0 = nullptr;
            float reach = 1.0f;
            const auto nk = R.innerNorm.find(s.light);
            if (!R.hasInfo) why0 = "room 0 without Apex's record";
            else if (!std::binary_search(info.inner.begin(), info.inner.end(), l)) why0 = "not a lamp of the record's inner list";
            else if (std::binary_search(info.innerUp.begin(), info.innerUp.end(), l)) why0 = "a lamp of the story above (innerUp)";
            else if (nk == R.innerNorm.end() || nk->second < 0.0f) why0 = "no source normalisation dumped (XN)";
            else if (std::fabs(s.n[1]) < 0.5f && R.A("reach") != 0.0f) {
                if (s.hasEv) reach = s.ev.reach; // (the lamp's range x intensity is not dumped: ReachShare's own answer)
                else why0 = "a wall texel with realistic reach and no recorded evaluation";
            }
            if (why0) {
                m0Out[why0]++;
                continue;
            }
            const float ev = static_cast<float>(std::max(s.evals, 1));
            float law[3];
            for (int i = 0; i < 3; i++) law[i] = s.hasEv ? s.ev.c0[i] : s.c0[i] / ev;
            float A = 0.0f, B = 0.0f, out[3] = {};
            const bool spread = R.A("softOpeningEdges", 1) != 0.0f;
            const auto pass = [&] {
                GridWhy why{};
                return InnerPassAt(info.innerGrid[0].get(), info.innerGrid[1].get(), s.L, s.p, spread, why);
            };
            if (RoomLampMove::Part0At(R.r3, s.f1 != 0, s.L, s.p, s.n, (law[2] + law[1]) + law[0], g_ctx.thr, reach, InnerLight::NormScale(nk->second, 1.0f, true), pass, A, B,
                                      &mvUnknown))
                RtLampModel::KeptAdd(law, A, B, 1.0f, ev, true, g_ctx.fade, g_ctx.thr, out);
            mMove0.Add(s.got, out, s.line);
            if (s.tag == 'T') mMove0T.Add(s.got, out, s.line);
            (s.hasEv ? mMove0Ev : mMove0NoEv).Add(s.got, out, s.line);
            continue;
        }
        mAll.Add(s.got, model, s.line);
        (foreign ? mForeign : mOwn).Add(s.got, model, s.line);
        if (foreign) mForeignHook.Add(s.got, modelRecHook, s.line);
        if (foreign && s.tag == 'T') mForeignT.Add(s.got, model, s.line);
        if (foreign && s.raw >= 0) mForeignWall.Add(s.got, model, s.line);
        // (GPU room lamps stage 3) the move worker's part (room_lamp_move.h PartAt, at the recorded position) drawn as the GPU draws it
        // (KeptAdd), the game's evaluation c0 standing for the law; out of its scope (what LevelLightShare::MovePath refuses):
        // another path than the plain one, a roofless room with a record, a lamp under the room's story floor
        if (!(s.paths & ~1) && (!s.hasEv || s.ev.set == 0) && !(R.hasInfo && !R.indoor) && LampOverOwnFloor(room.data(), s.L)) {
            const bool built = RoomLampMove::ListsBuilt(static_cast<uint32_t>(s.count), s.f0 != 0, s.f1 != 0);
            if (built && (mvKey != s.batch || mvL[0] != s.L[0] || mvL[1] != s.L[1] || mvL[2] != s.L[2] || mvRoom != s.room)) {
                mvList = r3d::wallCull(rr, Cv, Lv), mvKey = s.batch, mvRoom = s.room;
                std::memcpy(mvL, s.L, sizeof mvL);
            }
            float A = 1.0f, B = 0.0f, out[3] = {};
            RoomLampMove::PartAt(rr, built, mvList, s.f0 != 0, s.f1 != 0, s.L, s.p, s.n, A, B, &mvUnknown);
            const float* law = s.hasEv ? s.ev.c0 : s.c0;
            RtLampModel::KeptAdd(law, A, B, 1.0f, static_cast<float>(std::max(s.evals, 1)), g_ctx.info != nullptr, g_ctx.fade, g_ctx.thr, out);
            (s.hasEv ? mMoveForeign : mMoveOwn).Add(s.got, out, s.line);
            if (s.tag == 'T') mMoveT.Add(s.got, out, s.line);
            if (s.hasEv && (s.ev.day != 1.0f || s.ev.reach != 1.0f)) mvDayReach++;
            if (s.hasEv && s.ev.cut) mvCut++;
        } else
            mvOut++;
    }
    std::printf("cross lamps whose floor object is not among the dumped floors: %ld of %ld; rooms with no record: %ld; segment answers missing: %ld\n\n",
                noCrossLevel, crossLevels, noInfo, g_segMissing);
    std::printf("factors, replay vs recorded (per evaluation):\n");
    for (const Cmp* c : {&cMount, &cCut, &cEarly, &c6pass, &c6k, &c7pass, &c7t, &c7times, &c7law, &c7gain, &c7cut, &c7k, &c7kEnd, &c7q, &c7thr, &c8mine, &c8slab, &c8trans,
                         &c8stage, &c8why, &c8nseg, &c8seg, &c8room, &c8ent, &c9, &c9thr})
        c->Print();
    std::printf("\nthe colour leaving the hook (x the lamp's entries) vs the recorded hook:\n");
    for (const Fin* x : {&fRec, &fCut, &fInner, &fOpen, &fCross}) x->Print();
    std::printf("\nthe whole chain (Apex's steps x the game's 2D wall and 3D tests) vs the measured part:\n");
    for (const Fin* x : {&mAll, &mForeign, &mForeignT, &mForeignWall, &mForeignHook, &mOwn}) x->Print();
    std::printf("\nthe move worker (GPU room lamps stage 3) at the recorded positions vs the measured part (out of its scope: %ld lines; in scope with a\n"
                "daytime or reach factor: %ld, with the floor cut: %ld; unknown blocker vtables: %d):\n",
                mvOut, mvDayReach, mvCut, mvUnknown);
    for (const Fin* x : {&mMoveOwn, &mMoveForeign, &mMoveT}) x->Print();
    std::printf("\nthe move worker in room 0 (GPU room lamps stage 3b) at the recorded positions vs the measured part; out of its scope:\n");
    for (const auto& [why, n] : m0Out) std::printf("  %-60s %7ld\n", why.c_str(), n);
    for (const Fin* x : {&mMove0, &mMove0T, &mMove0Ev, &mMove0NoEv}) x->Print();
    std::printf("\nthe GPU law at the recorded texels vs the game's evaluation (the lamp dumped at another place: %ld lines):\n", lawElsewhere);
    for (const Fin* x : {&fLaw, &fLawTube}) x->Print();
    std::printf("\nIndoorShadow's other-room wall tests with the dumped walls (segments replayed %ld, left to the recorded answer %ld):\n", g_segReplayed, g_segRecorded);
    for (const Cmp* c : {&cSegKeep, &cSegPassed}) c->Print();
    std::printf("\nthe opening's outdoor total from the dumped fills (TF fills %ld, lights without a dumped law %ld):\n", tfFills, tfNoLaw);
    for (const Cmp* c : {&c7total, &c7cutRe, &cTfPass}) c->Print();
    fTfLaw.Print();
    std::printf("\nroom 0's lamps carried outdoors (mask lines without a law: %ld):\n", mNoLaw);
    for (const Fin* x : {&m0Inner, &m0Own, &m0Other}) x->Print();
    for (const Cmp* c : {&cMall, &cMvis, &cEtile, &cEalpha}) c->Print();
    return 0;
}
