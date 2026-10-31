// Ground light profile (see ground_profile.h). Part of Apex Radiance. Credits: @loinyx

#include "ground_profile.h"
#include "build_flavor.h"
#include "ground_profile_math.h"
#include "captures.h"
#include "game_addresses.h"
#include "gpu_lamp_law.h"
#include "level_light_share.h"
#include "lightmap_smooth.h"
#include "lot_light_bridge.h"
#include "outdoor_delta.h"
#include "post_scene.h"
#include "terrain_bake_cone.h"
#include "terrain_stamps.h"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <format>
#include <map>
#include <ostream>
#include <set>
#include <sstream>
#include <string>
#include <vector>

namespace GroundProfile {
using GroundProfileMath::GroundUnder;
using GroundProfileMath::Invert4;
using GroundProfileMath::Project;
namespace {
constexpr float kBehind = 4.0f, kPast = 12.0f, kStep = 0.25f; // metres along the line from the lamp
constexpr float kNearLamps = 25.0f;                             // lamps within this of the target lamp are evaluated

struct LampInfo { // POD (read under SEH)
    uintptr_t light;
    int type;
    uint64_t lot;
    float pos[3], range, intensity, e0[3];
    TerrainBakeCone::Fields cone;
};
bool ReadLamp(uintptr_t L, LampInfo& i) {
    __try {
        const BYTE* p = reinterpret_cast<const BYTE*>(L);
        i.light = L;
        i.type = *reinterpret_cast<const int*>(p + 0xB0);
        i.lot = *reinterpret_cast<const uint32_t*>(p + 0xC0) | static_cast<uint64_t>(*reinterpret_cast<const uint32_t*>(p + 0xC4)) << 32;
        for (int k = 0; k < 3; k++) {
            i.pos[k] = reinterpret_cast<const float*>(p + 0x120)[k];
            i.e0[k] = reinterpret_cast<const float*>(p + 0xE0)[k];
        }
        i.range = *reinterpret_cast<const float*>(p + 0x130);
        i.intensity = *reinterpret_cast<const float*>(p + 0x10);
        i.cone.type = i.type;
        if (i.type == 4) {
            for (int k = 0; k < 3; k++) i.cone.axis4[k] = reinterpret_cast<const float*>(p + 0x170)[k];
            i.cone.off4 = *reinterpret_cast<const float*>(p + 0x158);
            i.cone.scale4 = *reinterpret_cast<const float*>(p + 0x154);
        } else if (i.type == 5) {
            for (int k = 0; k < 3; k++) {
                i.cone.a1[k] = reinterpret_cast<const float*>(p + 0x1A0)[k];
                i.cone.a2[k] = reinterpret_cast<const float*>(p + 0x190)[k];
                i.cone.S[k] = reinterpret_cast<const float*>(p + 0x150)[k];
            }
            i.cone.o1 = *reinterpret_cast<const float*>(p + 0x174);
            i.cone.o2 = *reinterpret_cast<const float*>(p + 0x170);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
float ReadConst(GameAddr::Id id, float def) {
    const uintptr_t at = GameAddr::Get(id);
    __try {
        const float v = at ? *reinterpret_cast<const float*>(at) : def;
        return std::isfinite(v) && v > 0.0f && v < 1e4f ? v : def;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return def;
    }
}

// The room solve's law of one lamp at a floor point (normal up), counted once (GpuLampLaw::Eval, world space, no reach)
void RoomLaw(const LampInfo& i, const float p[3], float k1, float k2, float cmax, float street, float s5, float out[3]) {
    GpuLampLaw::Lamp L;
    for (int k = 0; k < 3; k++) L.pos[k] = i.pos[k], L.colour[k] = std::isfinite(i.e0[k]) ? std::max(i.e0[k], 0.0f) : 0.0f;
    const float w = i.range * i.intensity;
    L.W = std::isfinite(w) && w > 0.0f ? k2 * w / k1 * (i.type == 11 ? street : 1.0f) : 0.0f;
    L.cmax = cmax;
    float r[3][4];
    TerrainBakeCone::Rows(i.cone, s5, r);
    for (int k = 0; k < 3; k++) L.axis[k] = r[0][k], L.spill[k] = r[2][k];
    L.sc1 = r[0][3], L.off1 = r[1][0], L.sc2 = r[1][1], L.off2 = r[1][2];
    const float n[3] = {0, 1, 0};
    GpuLampLaw::Eval(L, p, n, 0.0f, out);
}
// The terrain bake's law of one lamp at a flat ground point, as the bake (and StampPS) gives it
void BakeLaw(const float col[4], const float pos[3], const float cone[3][4], const float p[3], float out[3]) {
    const float L[3] = {p[0] - pos[0], p[1] - pos[1], p[2] - pos[2]};
    const float len = std::sqrt(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
    const float l[3] = {len > 0 ? -L[0] / len : 0, len > 0 ? -L[1] / len : 1, len > 0 ? -L[2] / len : 0};
    const float d2 = L[0] * L[0] + L[2] * L[2] + 1e-4f;
    const float c = l[1];
    const float a = std::clamp(col[3] / d2, 0.0f, 1.0f) * (c >= 0 ? std::clamp(std::sqrt(c), 0.0f, 1.0f) : 0.0f);
    float f[3] = {1, 1, 1};
    if (std::fabs(cone[2][3] - TerrainBakeCone::kMagic) < 0.25f) TerrainBakeCone::Factor(cone, l, f);
    for (int k = 0; k < 3; k++) out[k] = col[k] * a * f[k];
}

// One pixel of the back buffer through a reused 1x1 target (8 bits a channel)
struct ScreenReader {
    IDirect3DDevice9* dev;
    IDirect3DSurface9 *bb = nullptr, *rt = nullptr, *sys = nullptr;
    explicit ScreenReader(IDirect3DDevice9* d) : dev(d) {
        D3DSURFACE_DESC desc{};
        if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb || FAILED(bb->GetDesc(&desc)) ||
            (desc.Format != D3DFMT_X8R8G8B8 && desc.Format != D3DFMT_A8R8G8B8) ||
            FAILED(dev->CreateRenderTarget(1, 1, desc.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, nullptr)) ||
            FAILED(dev->CreateOffscreenPlainSurface(1, 1, desc.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr)))
            Release();
    }
    ~ScreenReader() { Release(); }
    void Release() {
        for (IDirect3DSurface9** s : {&sys, &rt, &bb})
            if (*s) (*s)->Release(), *s = nullptr;
    }
    bool Read(LONG x, LONG y, float out[3]) {
        if (!sys) return false;
        RECT src{x, y, x + 1, y + 1};
        D3DLOCKED_RECT lr{};
        if (FAILED(dev->StretchRect(bb, &src, rt, nullptr, D3DTEXF_NONE)) || FAILED(dev->GetRenderTargetData(rt, sys)) || FAILED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY)))
            return false;
        const DWORD c = *static_cast<const DWORD*>(lr.pBits);
        out[0] = ((c >> 16) & 0xFF) / 255.0f, out[1] = ((c >> 8) & 0xFF) / 255.0f, out[2] = (c & 0xFF) / 255.0f;
        sys->UnlockRect();
        return true;
    }
};

// The camera of this frame (world -> clip rows), its pixel for a world point, and the ground point under a pixel
bool Camera(double vp[4][4]) {
    float f[4][4];
    if (!PostScene::RecentCameraViewProj(f)) return false;
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) vp[r][c] = f[r][c];
    return true;
}
// The listed lamps and the one nearest a point (horizontally); its index or -1
std::vector<LampInfo> ListedLamps() {
    std::vector<LampInfo> lamps;
    for (uintptr_t L : TerrainStamps::LastLights()) {
        LampInfo i{};
        if (L && ReadLamp(L, i)) lamps.push_back(i);
    }
    return lamps;
}
int Nearest(const std::vector<LampInfo>& lamps, const float p[3]) {
    int best = -1;
    float bestD = 1e30f;
    for (size_t k = 0; k < lamps.size(); k++) {
        const float dx = lamps[k].pos[0] - p[0], dz = lamps[k].pos[2] - p[2], d = dx * dx + dz * dz;
        if (d < bestD) bestD = d, best = static_cast<int>(k);
    }
    return best;
}

std::string Rgb(const float* c) { return std::format("({:.3f} {:.3f} {:.3f})", c[0], c[1], c[2]); }
std::string Csv(const float* c) { return std::format("{:.4f},{:.4f},{:.4f}", c[0], c[1], c[2]); }
} // namespace

void Write(IDirect3DDevice9* dev, POINT pixel, unsigned bbWidth, unsigned bbHeight, const std::filesystem::path& folder, std::ostream& out) {
    out << "== GROUND LIGHT PROFILE (the lot lamp nearest the ground under the mouse; terms at points along a line through it) ==\n";
    // the camera: the mouse's ground point and every point's pixel
    double vp[4][4];
    const bool camera = Camera(vp) && bbWidth && bbHeight;
    const auto project = [&](const float p[3], LONG& x, LONG& y) { return camera && Project(vp, p, bbWidth, bbHeight, x, y); };
    // the lamps of the active lot (the last frame's list) read once
    const std::vector<LampInfo> lamps = ListedLamps();
    if (lamps.empty()) {
        out << "no lot lamps listed (no active lot, or Real-time outdoor lamps / Night Lighting off)\n\n";
        return;
    }
    // the mouse's ground point: the ray through the pixel at the height of the lamps' ground
    float ground = 0.0f;
    LotLightBridge::LotGroundY(lamps[0].lot, ground);
    float mouse[3] = {lamps[0].pos[0], ground, lamps[0].pos[2]};
    const bool mouseOk = camera && GroundUnder(vp, pixel, bbWidth, bbHeight, ground, mouse);
    // the target: the lamp nearest the mouse point (horizontally)
    const LampInfo* target = &lamps[std::max(0, Nearest(lamps, mouse))];
    LotLightBridge::LotGroundY(target->lot, ground);
    float dir[2] = {mouse[0] - target->pos[0], mouse[2] - target->pos[2]};
    const float dl = std::sqrt(dir[0] * dir[0] + dir[1] * dir[1]);
    if (dl > 0.25f) dir[0] /= dl, dir[1] /= dl;
    else dir[0] = 1, dir[1] = 0;
    const float k1 = ReadConst(GameAddr::Id::LampK1, 1.0f), k2 = ReadConst(GameAddr::Id::LampK2, 0.075f), cmax = ReadConst(GameAddr::Id::LampCmax, 2.0f),
                street = ReadConst(GameAddr::Id::StreetLampScale, 3.3333f), s5 = ReadConst(GameAddr::Id::ConeScale5, 5.0f);
    out << std::format("camera {} | mouse ground point {} ({:.2f} {:.2f} {:.2f}) | target L{:08X} type {} lot {:016X} at ({:.2f} {:.2f} {:.2f}), {:.2f} m over its lot's ground {:.2f} | "
                       "line direction ({:.3f} {:.3f}), {:.1f} m to the mouse point\n",
                       camera ? "read" : "not seen (no screen colours, the line runs along +x)", mouseOk ? "on the ground" : "not found", mouse[0], mouse[1], mouse[2],
                       target->light, target->type, target->lot, target->pos[0], target->pos[1], target->pos[2], target->pos[1] - ground, ground, dir[0], dir[1], dl);
    out << "law constants: k1 " << k1 << ", k2 " << k2 << ", Cmax " << cmax << ", street x" << street << ", cone scale " << s5 << " | terrain bake with cones: "
        << (NightTerrainBake::ConeShader() ? "yes" : "no") << "\n";
    out << "gains: " << LotLightBridge::GainText() << "\n";
    for (const LampInfo& i : lamps) // (developer builds: every lamp post of the lot, the mouse can miss the one in question)
        if (&i == target || (i.type == 11 && !kPublicBuild)) {
            out << std::format("-- lamp L{:08X} type {} at ({:.2f} {:.2f} {:.2f}){}\n", i.light, i.type, i.pos[0], i.pos[1], i.pos[2], &i == target ? " (the target)" : "");
            out << LotLightBridge::ShadeProbeText(i.pos);
        }

    // the nearby lamps: the bake's stamp of each and its states
    struct Near {
        const LampInfo* i;
        bool stampOk;
        float col[4], spos[3], rect[4], ground, cone[3][4];
    };
    std::vector<Near> nearby;
    for (const LampInfo& i : lamps) {
        const float dx = i.pos[0] - target->pos[0], dz = i.pos[2] - target->pos[2];
        if (dx * dx + dz * dz > kNearLamps * kNearLamps) continue;
        Near n{&i};
        n.stampOk = NightTerrainBake::StampOf(i.light, n.col, n.spos, n.rect, n.ground, n.cone);
        nearby.push_back(n);
    }
    out << std::format("lamps within {:.0f} m of it: {} (of {} listed)\n", kNearLamps, nearby.size(), lamps.size());
    for (const Near& n : nearby) {
        const LampInfo& i = *n.i;
        out << std::format("   L{:08X} type {} at ({:.2f} {:.2f} {:.2f}) range {:.1f} intensity {:.2f} colour {} | bake: {}", i.light, i.type, i.pos[0], i.pos[1], i.pos[2], i.range,
                           i.intensity, Rgb(i.e0),
                           n.stampOk ? std::format("taken, colour {} w {:.2f} from ({:.2f} {:.2f} {:.2f}), rect ({:.1f} {:.1f} {:.1f} {:.1f}){}", Rgb(n.col), n.col[3], n.spos[0], n.spos[1],
                                                   n.spos[2], n.rect[0], n.rect[1], n.rect[2], n.rect[3],
                                                   std::fabs(n.cone[2][3] - TerrainBakeCone::kMagic) < 0.25f && (n.cone[0][3] != 1.0f || n.cone[1][0] != -1.0f)
                                                       ? std::format(", cone axis ({:.2f} {:.2f} {:.2f}) sc {:.2f}/{:.2f} off {:.3f}/{:.3f} spill {}", n.cone[0][0], n.cone[0][1],
                                                                     n.cone[0][2], n.cone[0][3], n.cone[1][1], n.cone[1][0], n.cone[1][2], Rgb(n.cone[2]))
                                                       : std::string())
                                     : std::string("not taken (off, indoors, or not a lot lamp of the bake)"));
        const std::string st = TerrainStamps::LampState(i.light);
        out << std::format(" | ever baked {} | excluded {}{}\n", NightTerrainBake::EverBaked(i.light) ? "yes" : "no", NightTerrainBake::Excluded(i.light) ? "yes" : "no",
                           st.empty() ? "" : " | stamp: " + st);
    }

    // the target lot's story-0 floor map (the kept solve's texels with their world places)
    OutdoorDelta::Room0Export floor;
    bool haveFloor = false;
    {
        uintptr_t trackers[16];
        const int n = LevelLightShare::ActiveLotTrackers(trackers, 16);
        for (int t = 0; t < n && !haveFloor; t++) {
            void* room = LevelLightShare::Room0Of(trackers[t], 0);
            if (!room) continue;
            for (int cls : {2, 1, 0})
                if (OutdoorDelta::RecordVersion(room, cls) && OutdoorDelta::ExportRoom0(room, cls, floor) && floor.lot == target->lot) {
                    haveFloor = true;
                    break;
                }
        }
    }
    std::map<std::pair<int, int>, std::vector<size_t>> floorGrid; // floor texels by half-metre cell
    if (haveFloor)
        for (size_t k = 0; k < floor.texels.size(); k++)
            if (floor.texels[k].map == 1)
                floorGrid[{static_cast<int>(std::floor(floor.texels[k].pos[0] * 2)), static_cast<int>(std::floor(floor.texels[k].pos[2] * 2))}].push_back(k);
    out << std::format("lot floor map (story 0 room 0, class {}): {}\n", haveFloor ? floor.cls : -1,
                       haveFloor ? std::format("floor texels in {} half-metre cells, {} lamps drawn by the GPU", floorGrid.size(), floor.drawn.size()) : std::string("not kept (Instant lamps off, or no solve kept yet)"));

    // the points
    ScreenReader screen(dev);
    std::ostringstream csv;
    csv << "t,x,z,lot,pixel_x,pixel_y,screen_r,screen_g,screen_b,terrain_r,terrain_g,terrain_b,smoothed_r,smoothed_g,smoothed_b,stamps,floormap_r,floormap_g,floormap_b,"
           "floorbase_r,floorbase_g,floorbase_b,bakelaw_r,bakelaw_g,bakelaw_b,roomlaw_r,roomlaw_g,roomlaw_b\n";
    std::set<std::pair<int, int>> chunksSeen;
    std::ostringstream chunkLines;
    for (float t = -kBehind; t <= kPast + 1e-3f; t += kStep) {
        const float p[3] = {target->pos[0] + dir[0] * t, ground, target->pos[2] + dir[1] * t};
        const uint64_t lot = LotLightBridge::LotAt(p[0], p[2]);
        LONG px = -1, py = -1;
        float scr[3] = {-1, -1, -1};
        const bool onScreen = project(p, px, py) && screen.Read(px, py, scr);
        LightmapSmooth::ChunkSample cs;
        const bool chunk = LightmapSmooth::SampleChunk(dev, p[0], p[2], cs);
        const std::pair<int, int> ck{static_cast<int>(std::floor(p[0] / 256)) * 256 + 128, static_cast<int>(std::floor(p[2] / 256)) * 256 + 128};
        if (chunk && chunksSeen.insert(ck).second)
            chunkLines << std::format("   chunk ({}, {}): game map version {} (smoothed built from {}), re-renders {}, stamp version {} (built {}), stamps now {}\n", ck.first, ck.second,
                                      cs.gver, cs.built, cs.rver, cs.sver, cs.builtS, cs.stamps);
        // the floor texel nearest (within 0.5 m)
        float fm[3] = {-1, -1, -1}, fb[3] = {-1, -1, -1};
        if (haveFloor) {
            float bestD = 0.25f;
            const auto& tx = floor.texels;
            for (int gx = -1; gx <= 1; gx++)
                for (int gz = -1; gz <= 1; gz++) {
                    const auto it = floorGrid.find({static_cast<int>(std::floor(p[0] * 2)) + gx, static_cast<int>(std::floor(p[2] * 2)) + gz});
                    if (it == floorGrid.end()) continue;
                    for (size_t k : it->second) {
                        const float dx = tx[k].pos[0] - p[0], dz = tx[k].pos[2] - p[2], d = dx * dx + dz * dz;
                        if (d >= bestD) continue;
                        bestD = d;
                        const uint32_t c = tx[k].packed;
                        fm[0] = ((c >> 16) & 0xFF) / 255.0f, fm[1] = ((c >> 8) & 0xFF) / 255.0f, fm[2] = (c & 0xFF) / 255.0f;
                        std::memcpy(fb, tx[k].base, sizeof fb);
                    }
                }
        }
        // what the lamps should give here
        float bake[3] = {}, room[3] = {};
        std::vector<std::pair<float, uintptr_t>> top;
        for (const Near& n : nearby) {
            float b[3] = {}, r[3] = {};
            if (n.stampOk) BakeLaw(n.col, n.spos, n.cone, p, b);
            RoomLaw(*n.i, p, k1, k2, cmax, street, s5, r);
            for (int k = 0; k < 3; k++) bake[k] += b[k], room[k] += r[k];
            top.emplace_back(b[0] + b[1] + b[2] + r[0] + r[1] + r[2], n.i->light);
        }
        std::sort(top.begin(), top.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        std::string tops;
        for (size_t k = 0; k < top.size() && k < 2 && top[k].first > 0.003f; k++) tops += std::format(" L{:08X}", top[k].second);
        const float none[3] = {-1, -1, -1};
        out << std::format("t {:+6.2f} ({:.2f} {:.2f}) {} | screen {} | terrain map {} smoothed {}{} | lot floor map {} | expected: bake law {} room law x1 {} |{}\n", t, p[0], p[2],
                           lot ? std::format("lot {:08X}", static_cast<uint32_t>(lot)) : std::string("outside lots"), onScreen ? Rgb(scr) : std::string("(off screen)"),
                           chunk && cs.src ? Rgb(cs.srcRgba) : std::string("(none)"), chunk && cs.smooth ? Rgb(cs.smoothRgba) : std::string("(none)"),
                           chunk && cs.stamps ? std::format(" stamps {}", cs.stamps) : std::string(), fm[0] >= 0 ? Rgb(fm) + " base " + Rgb(fb) : std::string("(none)"), Rgb(bake),
                           Rgb(room), tops);
        csv << std::format("{:.2f},{:.3f},{:.3f},{:016X},{},{},{},{},{},{},{},{},{}\n", t, p[0], p[2], lot, px, py, Csv(onScreen ? scr : none),
                           Csv(chunk && cs.src ? cs.srcRgba : none), Csv(chunk && cs.smooth ? cs.smoothRgba : none), chunk ? cs.stamps : -1, Csv(fm[0] >= 0 ? fm : none),
                           Csv(fm[0] >= 0 ? fb : none), Csv(bake) + "," + Csv(room));
    }
    out << "chunks on the line:\n" << chunkLines.str() << "\n";
    Captures::WriteText(folder / "Ground profile.csv", csv.str());
}

namespace {
struct WatchRow {
    DWORD tick;
    float lamp[3];
    bool chunk, excluded;
    LightmapSmooth::ChunkSample cs;
    std::string stamp;
    int x[kWatchPoints], y[kWatchPoints];
};
struct WatchState {
    bool on = false, chosen = false;
    POINT mouse{};
    int w = 0, h = 0;
    uintptr_t light = 0;
    int type = 0;
    float dir[2] = {1, 0};
    std::vector<WatchRow> rows;
} g_watch;
constexpr size_t kWatchRows = 4000; // 20 s at 200 fps (ScreenWatch's samples)
} // namespace

void WatchBegin(POINT mouse, int bbWidth, int bbHeight) {
    WatchEnd();
    g_watch = WatchState{};
    g_watch.on = true, g_watch.mouse = mouse, g_watch.w = bbWidth, g_watch.h = bbHeight;
    g_watch.rows.reserve(kWatchRows);
    PostScene::WantCamera(true);
}

bool WatchPoints(int x[kWatchPoints], int y[kWatchPoints]) {
    if (!g_watch.on || g_watch.w <= 0 || g_watch.h <= 0) return false;
    double vp[4][4];
    if (!Camera(vp)) return false;
    const unsigned w = static_cast<unsigned>(g_watch.w), h = static_cast<unsigned>(g_watch.h);
    if (!g_watch.chosen) { // the first frame with a camera and lamps: the lamp nearest the mouse's ground point, and the line
        const std::vector<LampInfo> lamps = ListedLamps();
        if (lamps.empty()) return false;
        float ground = 0.0f, mouse[3] = {};
        LotLightBridge::LotGroundY(lamps[0].lot, ground);
        if (!GroundUnder(vp, g_watch.mouse, w, h, ground, mouse)) return false;
        int pick = Nearest(lamps, mouse);
        for (size_t k = 0; k < lamps.size(); k++) // a lamp being dragged (drawn on the GPU, moved within 2 s) before the nearest
            if (TerrainStamps::MovedWithin(lamps[k].light, 2000)) pick = static_cast<int>(k);
        const LampInfo& t = lamps[pick];
        const float dx = mouse[0] - t.pos[0], dz = mouse[2] - t.pos[2], d = std::sqrt(dx * dx + dz * dz);
        if (d > 0.25f) g_watch.dir[0] = dx / d, g_watch.dir[1] = dz / d;
        g_watch.light = t.light, g_watch.type = t.type, g_watch.chosen = true;
    }
    LampInfo i{};
    if (!ReadLamp(g_watch.light, i)) return false;
    float ground = i.pos[1];
    LotLightBridge::LotGroundY(i.lot, ground);
    WatchRow row{GetTickCount(), {i.pos[0], i.pos[1], i.pos[2]}};
    for (int k = 0; k < kWatchPoints; k++) {
        const float p[3] = {i.pos[0] + g_watch.dir[0] * WatchT(k), ground, i.pos[2] + g_watch.dir[1] * WatchT(k)};
        LONG px = 0, py = 0;
        if (!Project(vp, p, w, h, px, py)) px = py = -1;
        row.x[k] = px, row.y[k] = py;
        x[k] = std::max<LONG>(px, 0), y[k] = std::max<LONG>(py, 0); // (off screen: pixel 0, 0; the row says -1)
    }
    row.chunk = LightmapSmooth::SampleChunk(nullptr, i.pos[0], i.pos[2], row.cs, false);
    row.excluded = NightTerrainBake::Excluded(g_watch.light);
    row.stamp = TerrainStamps::LampState(g_watch.light);
    if (g_watch.rows.size() < kWatchRows) g_watch.rows.push_back(std::move(row));
    return true;
}

void WatchEnd() {
    if (!g_watch.on) return;
    g_watch.on = false;
    PostScene::WantCamera(false);
}

std::string WatchCsv(unsigned long startTick) {
    if (g_watch.rows.empty()) return {};
    std::string s = std::format("# lamp L{:08X} type {}; ground points at t = {:.0f} .. {:.0f} m along ({:.3f} {:.3f}) from it (their colours: Screen pixels.csv, "
                                "columns luma_ground...); x -1: off screen\n",
                                g_watch.light, g_watch.type, WatchT(0), WatchT(kWatchPoints - 1), g_watch.dir[0], g_watch.dir[1]);
    s += "elapsed_ms,lamp_x,lamp_y,lamp_z,map_version,smoothed_built,re_renders,stamp_version,stamp_built,stamps,excluded,stamp_state";
    for (int k = 0; k < kWatchPoints; k++) s += std::format(",x{0:+.0f},y{0:+.0f}", WatchT(k));
    s += "\n";
    for (const WatchRow& r : g_watch.rows) {
        s += std::format("{},{:.3f},{:.3f},{:.3f},{},{},{},{},{},{},{},\"{}\"", static_cast<int32_t>(r.tick - startTick), r.lamp[0], r.lamp[1], r.lamp[2],
                         r.chunk ? static_cast<int64_t>(r.cs.gver) : -1, r.chunk ? static_cast<int64_t>(r.cs.built) : -1, r.chunk ? static_cast<int64_t>(r.cs.rver) : -1,
                         r.chunk ? static_cast<int64_t>(r.cs.sver) : -1, r.chunk ? static_cast<int64_t>(r.cs.builtS) : -1, r.chunk ? r.cs.stamps : -1, r.excluded ? 1 : 0, r.stamp);
        for (int k = 0; k < kWatchPoints; k++) s += std::format(",{},{}", r.x[k], r.y[k]);
        s += "\n";
    }
    return s;
}
} // namespace GroundProfile
