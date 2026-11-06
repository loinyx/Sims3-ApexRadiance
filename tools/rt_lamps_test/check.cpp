// Real-time outdoor lamps (features/rt_lamp_proxy.h, rt_lamp_model.h, rt_lamp_gpu.cpp, shaders/rt_lamps_hlsl.h), offline:
//
//  1. Shadows: the occluder proxy built from story grids (a synthetic house with a balcony, a window, a door, a garden wall,
//     a porch roof and lamps under, beside and behind them; and the story grids, placed floors and lamps of Full captures'
//     World.txt), each lamp's cube shadow map rendered on a real Direct3D 9 device with the production pass, and the share
//     of the lamp ball every receiver texel sees on the GPU (AccPS's debug output) compared with the CPU reference (rays to
//     the ball's disc against the same triangles). Counted per texel and lamp: the 8-bit difference of the lamp's
//     contribution there (share difference x min(law, 1) x 255); the target is 99 % within 2/255. Leaks (the reference
//     sees none of the lamp, the GPU half or more, and the lamp would add 2/255 or more) are listed by what is near.
//  2. With the production grid walk (GridPassImpl and its helpers, extracted from features/level_light_share.cpp by run.ps1
//     when available): the CPU test of the room solve against the same reference, on the same receivers, for one story.
//  3. The bake split: base + GPU lamps == the game's texel. Synthetic room-0 texels (a sum S of baked lights, some of them GPU
//     lamps with their parts, the dither, the floor mask) relit on the GPU with no occluder (share 1) against the pack of S
//     (the game's texel) and against RtLampModel::Compose; class-2 wall blocks blurred on the GPU against the game's blur
//     (outdoor_delta_model.h BlurBlock) bit for bit.
//  4. Physical indoor lamps (PhysKeptCheck): the game's 3D test on the GPU (T3dPS) against RoomLightTests::occlude, the kept
//     shadow tiles against the reference, and RelightKeptPhys against the CPU's law x share x T3D, KeptTexel, pack and blur;
//     room 0's lot lamps from their laws (Room0PhysCheck: AccumulatePhys, the floor mask and exit alpha) against the CPU.
// Read-only: prints to stdout, writes no file. A missing D3D9 device or capture is a failure.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d9.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <format>
#include <fstream>
#include <map>
#include <random>
#include <sstream>
#include <string>
#include <vector>
#include "features/rt_lamp_proxy.h"
#include "features/stair_set.h"
#include "features/rt_lamp_model.h"
#include "features/rt_lamp_gpu.h"
#include "features/phys_lamp_pick.h"
#include "features/arch_ambient.h"
#include "features/room_lamp_move.h"
#include "features/rt_outdoor_lamps.h"
#include "features/lamp_reach.h"
// The dither offset of 2.12.4's Smooth light maps (RoomMapDither::Offset, interleaved gradient noise): 2.12.3 + RT has no
// map dither (its relight passes 0), but the compose shader keeps the input, so the harness still checks it
namespace RoomMapDither {
inline float Offset(unsigned x, unsigned y) {
    const double f = std::fmod(0.06711056 * static_cast<double>(x) + 0.00583715 * static_cast<double>(y), 1.0);
    const double g = std::fmod(52.9829189 * f, 1.0);
    return static_cast<float>(g - 0.5);
}
} // namespace RoomMapDither
#include "shaders/rt_lamps_hlsl.h"

#ifdef RT_OBJECT_SHAPES
#include "features/object_occlusion.h"
#endif
#ifdef RT_GRID_EXTRACT
#include <atomic>
#include <memory>
#include <unordered_set>
#include "features/penumbra.h"
#include "features/object_occlusion.h"
#include "features/lamp_visibility.h"
#include "features/wall_openings.h"
namespace Grid {
// what the extracted code needs around it (level_light_share.cpp)
struct Xform {
    float r[4][4];
};
struct CutRec {
    uint8_t kind = 0;
    uint32_t flags = 0;
};
inline int Quadrant(float fx, float fz) { return LampVis::Quadrant(fx, fz); }
#include "grid_extract.inc"
} // namespace Grid
#endif

namespace {
int checks = 0, failures = 0;
bool g_debug = false;
bool g_allLamps = false; // --probe: every lamp of the lot (indoor ones too), not only room 0's
void Check(bool ok, const std::string& text) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL: %s\n", text.c_str());
    }
}

// ---------------------------------------------------------------- the device
IDirect3DDevice9* g_dev = nullptr;
HWND g_wnd = nullptr;
bool MakeDevice() {
    WNDCLASSA wc{};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "ApexRtLampsTest";
    RegisterClassA(&wc);
    g_wnd = CreateWindowA(wc.lpszClassName, "rt lamps test", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    if (!d3d || !g_wnd) return false;
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.BackBufferFormat = D3DFMT_UNKNOWN;
    pp.BackBufferWidth = 64, pp.BackBufferHeight = 64;
    pp.hDeviceWindow = g_wnd;
    HRESULT hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, g_wnd, D3DCREATE_HARDWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &pp, &g_dev);
    if (FAILED(hr)) hr = d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, g_wnd, D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE, &pp, &g_dev);
    D3DADAPTER_IDENTIFIER9 id{};
    d3d->GetAdapterIdentifier(D3DADAPTER_DEFAULT, 0, &id);
    char path[MAX_PATH] = {};
    GetModuleFileNameA(GetModuleHandleA("d3d9.dll"), path, MAX_PATH);
    std::printf("device: %s | adapter %s | d3d9.dll %s\n", SUCCEEDED(hr) ? "HAL" : "none", id.Description, path);
    d3d->Release();
    return SUCCEEDED(hr) && g_dev;
}
void* g_shaders[RtLampGpu::kShaderCount] = {};
bool MakeShaders() {
    for (int i = 0; i < RtLampGpu::kShaderCount; i++) {
        const auto id = static_cast<RtLampGpu::ShaderId>(i);
        ID3DBlob *code = nullptr, *errs = nullptr;
        const HRESULT hr = D3DCompile(kRtLampsHlsl, std::strlen(kRtLampsHlsl), "rt_lamps", nullptr, nullptr, RtLampGpu::EntryOf(id), RtLampGpu::IsVertex(id) ? "vs_3_0" : "ps_3_0",
                                      D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errs);
        if (FAILED(hr) || !code) {
            std::printf("FAIL: %s does not compile: %s\n", RtLampGpu::EntryOf(id), errs ? static_cast<const char*>(errs->GetBufferPointer()) : "?");
            if (errs) errs->Release();
            return false;
        }
        if (errs) errs->Release();
        HRESULT c = RtLampGpu::IsVertex(id) ? g_dev->CreateVertexShader(static_cast<const DWORD*>(code->GetBufferPointer()), reinterpret_cast<IDirect3DVertexShader9**>(&g_shaders[i]))
                                            : g_dev->CreatePixelShader(static_cast<const DWORD*>(code->GetBufferPointer()), reinterpret_cast<IDirect3DPixelShader9**>(&g_shaders[i]));
        std::printf("shader %-10s %5u bytes\n", RtLampGpu::EntryOf(id), static_cast<unsigned>(code->GetBufferSize()));
        code->Release();
        if (FAILED(c)) {
            std::printf("FAIL: %s not created\n", RtLampGpu::EntryOf(id));
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------- scenes
struct LampDef {
    float pos[3];
    float colour[3] = {1, 0.95f, 0.85f};
    float range = 60.0f, intensity = 1.0f;
    int type = 3;
    float axis[3] = {0, -1, 0}, off = 0, scale = 0; // spot
    float a1[3] = {0, -1, 0}, o1 = 1, o2 = 1, S[3] = {0.3f, 0.3f, 0.3f}; // shaded lamp
    std::string name;
    int room = 0;
};
struct Receiver {
    float p[3], n[3];
    int kind; // 0 wall, 1 floor
    int story;
};
struct Scene {
    std::string name;
    bool ceilingsOpen = false; // GPU lamps for every room's proxy: no slab over a roofed ceiling of the story below
    RtLampProxy::Lot lot;
    std::vector<LampDef> lamps;
    std::vector<Receiver> recv;
#ifdef RT_OBJECT_SHAPES
    std::vector<ObjOcc::Box> shapes; // object shapes (lot space = world here), after ObjOcc::Classify
#endif
};
#ifdef RT_OBJECT_SHAPES
// The object shapes as GPU lamps for every room puts them in the occluders: every shape but the see-through ones and the
// ones within `nearSkip` of `head` (null: all of them)
void AddShapes(RtLampProxy::Mesh& m, const std::vector<ObjOcc::Box>& shapes, const float* head, float nearSkip) {
    for (const ObjOcc::Box& b : shapes) {
        if (b.opacity < 0.5f || (head && ObjOcc::Contains(b, head, nearSkip))) continue;
        RtLampProxy::ObjShape o;
        std::memcpy(o.c, b.c, sizeof o.c), std::memcpy(o.u, b.u, sizeof o.u), std::memcpy(o.h, b.h, sizeof o.h);
        o.cylinder = b.kind == ObjOcc::Kind::Cylinder, o.axis = b.axis, o.rad = b.rad;
        RtLampProxy::AddShape(m, o);
    }
}

#endif

RtLampProxy::Edge EdgeZ(int x, int Z, float foot, float top) {
    RtLampProxy::Edge e;
    e.x0 = static_cast<float>(x), e.z0 = static_cast<float>(Z), e.x1 = static_cast<float>(x + 1), e.z1 = static_cast<float>(Z);
    e.foot = e.base = foot, e.topL = e.topM = e.topR = top;
    return e;
}
RtLampProxy::Edge EdgeX(int X, int z, float foot, float top) {
    RtLampProxy::Edge e;
    e.x0 = static_cast<float>(X), e.z0 = static_cast<float>(z), e.x1 = static_cast<float>(X), e.z1 = static_cast<float>(z + 1);
    e.foot = e.base = foot, e.topL = e.topM = e.topR = top;
    return e;
}
// Wall receivers along an edge, both faces, every 0.25 m, 5 cm off the line (as the game lays wall samples out)
void WallReceivers(std::vector<Receiver>& out, const RtLampProxy::Edge& e, int story) {
    const float dx = e.x1 - e.x0, dz = e.z1 - e.z0, len = std::sqrt(dx * dx + dz * dz);
    const float nx = -dz / len, nz = dx / len;
    for (float u = 0.125f; u < 1.0f; u += 0.25f)
        for (float y = e.foot + 0.125f; y < RtLampProxy::EdgeTop(e, u); y += 0.25f) {
            if (e.open && u > e.o.u0 && u < e.o.u1 && y > e.base + e.o.sill && y < e.base + RtLampProxy::OpeningHead(e.o, u)) continue;
            for (int s = -1; s <= 1; s += 2) {
                Receiver r;
                r.p[0] = e.x0 + dx * u + nx * 0.05f * s, r.p[1] = y, r.p[2] = e.z0 + dz * u + nz * 0.05f * s;
                r.n[0] = nx * s, r.n[1] = 0, r.n[2] = nz * s;
                r.kind = 0, r.story = story;
                out.push_back(r);
            }
        }
}
void FloorReceivers(std::vector<Receiver>& out, float x0, float z0, float x1, float z1, float y, int story, float step = 0.25f) {
    for (float z = z0 + step * 0.5f; z < z1; z += step)
        for (float x = x0 + step * 0.5f; x < x1; x += step) {
            Receiver r{{x, y, z}, {0, 1, 0}, 1, story};
            out.push_back(r);
        }
}

Scene Synthetic() {
    using namespace RtLampProxy;
    Scene sc;
    sc.name = "synthetic house (balcony, window, door, garden wall, porch roof)";
    const int W = 40, H = 40;
    for (int s = 0; s <= 1; s++) {
        Story st;
        st.story = s, st.w = W, st.h = H;
        st.floorY.assign(W * H, kNoFloor);
        st.roofed.assign(W * H, 0);
        st.ceil.assign(W * H, kNoFloor);
        st.placed.assign(W * H, 0);
        const float foot = 3.0f * s, top = foot + 3.0f;
        st.plane = foot;
        for (int x = 10; x < 20; x++) {
            Edge a = EdgeZ(x, 10, foot, top), b = EdgeZ(x, 18, foot, top);
            if (x == 14) { // a window
                a.open = true, a.o.u0 = 0.1f, a.o.u1 = 0.9f, a.o.sill = 0.85f;
                for (float& h : a.o.head) h = 2.25f;
            }
            if (s == 1 && x == 16) { // a balcony door (open: an arch)
                b.open = true, b.o.u0 = 0.05f, b.o.u1 = 0.95f, b.o.sill = 0.0f;
                for (float& h : b.o.head) h = 2.4f;
            }
            st.edges.push_back(a), st.edges.push_back(b);
        }
        for (int z = 10; z < 18; z++) {
            Edge a = EdgeX(10, z, foot, top), b = EdgeX(20, z, foot, top);
            if (s == 0 && z == 13) { // an open door to the porch
                b.open = true, b.o.u0 = 0.0f, b.o.u1 = 1.0f, b.o.sill = 0.0f;
                for (float& h : b.o.head) h = 2.4f;
            }
            st.edges.push_back(a), st.edges.push_back(b);
        }
        for (int z = 10; z < 18; z++)
            for (int x = 10; x < 20; x++) {
                const size_t t = static_cast<size_t>(z) * W + x;
                st.floorY[t] = foot;
                st.roofed[t] = 0xF;
                st.ceil[t] = top;
                if (s == 1) st.placed[t] = 0xF;
            }
        if (s == 0) {
            for (int x = 12; x < 16; x++) st.edges.push_back(EdgeZ(x, 25, 0.0f, 2.0f)); // a garden wall
            for (int z = 12; z < 16; z++)                                              // a porch roof
                for (int x = 20; x < 23; x++) {
                    const size_t t = static_cast<size_t>(z) * W + x;
                    st.floorY[t] = 0.0f, st.roofed[t] = 0xF, st.ceil[t] = 3.0f;
                }
        } else {
            for (int z = 18; z < 20; z++) // a 2 m balcony
                for (int x = 10; x < 20; x++) {
                    const size_t t = static_cast<size_t>(z) * W + x;
                    st.floorY[t] = 3.0f, st.placed[t] = 0xF;
                }
        }
        sc.lot.stories.push_back(std::move(st));
    }
    const auto lamp = [&](float x, float y, float z, const char* name) {
        LampDef l;
        l.pos[0] = x, l.pos[1] = y, l.pos[2] = z;
        l.name = name;
        sc.lamps.push_back(l);
    };
    lamp(15.0f, 2.2f, 19.0f, "under the balcony");
    lamp(14.5f, 1.8f, 9.5f, "beside the window");
    lamp(14.0f, 1.0f, 26.0f, "behind the garden wall");
    lamp(12.5f, 2.0f, 9.9f, "hugging a wall (10 cm)");
    lamp(21.5f, 2.5f, 13.5f, "under the porch roof");
    lamp(5.0f, 1.5f, 5.0f, "garden post");
    lamp(15.0f, 4.6f, 19.3f, "on the balcony");
    for (const Story& st : sc.lot.stories)
        for (const Edge& e : st.edges) WallReceivers(sc.recv, e, st.story);
    // the ground outside the house, the balcony's top
    for (float z = 0.125f; z < 40.0f; z += 0.25f)
        for (float x = 0.125f; x < 40.0f; x += 0.25f) {
            if (x > 10.0f && x < 20.0f && z > 10.0f && z < 18.0f) continue;
            Receiver r{{x, 0.0f, z}, {0, 1, 0}, 1, 0};
            sc.recv.push_back(r);
        }
    FloorReceivers(sc.recv, 10.0f, 18.0f, 20.0f, 20.0f, 3.0f, 1);
    return sc;
}

// Two stories of rooms (GPU lamps for every room): a partition with a door, ceilings, an upper floor over the rooms (no slab
// over a ceiling), a lamp hung 5 cm under a ceiling, a table lamp, a wall sconce and a lamp upstairs; receivers on every wall
// face, the floors and the ceilings of story 0 and the floor of story 1
Scene IndoorRooms() {
    using namespace RtLampProxy;
    Scene sc;
    sc.name = "two stories of rooms (partition with a door, ceilings, upper floor)";
    sc.ceilingsOpen = true;
    const int W = 24, H = 24;
    for (int s = 0; s <= 1; s++) {
        Story st;
        st.story = s, st.w = W, st.h = H;
        st.floorY.assign(W * H, kNoFloor);
        st.roofed.assign(W * H, 0);
        st.ceil.assign(W * H, kNoFloor);
        st.placed.assign(W * H, 0);
        const float foot = 3.0f * s, top = foot + 3.0f;
        st.plane = foot;
        for (int x = 4; x < 14; x++) st.edges.push_back(EdgeZ(x, 4, foot, top)), st.edges.push_back(EdgeZ(x, 12, foot, top));
        for (int z = 4; z < 12; z++) {
            st.edges.push_back(EdgeX(4, z, foot, top)), st.edges.push_back(EdgeX(14, z, foot, top));
            Edge mid = EdgeX(9, z, foot, top); // the partition
            if (z == 7) { // a door
                mid.open = true, mid.o.u0 = 0.05f, mid.o.u1 = 0.95f, mid.o.sill = 0.0f;
                for (float& h : mid.o.head) h = 2.1f;
            }
            st.edges.push_back(mid);
        }
        for (int z = 4; z < 12; z++)
            for (int x = 4; x < 14; x++) {
                const size_t t = static_cast<size_t>(z) * W + x;
                st.floorY[t] = foot, st.roofed[t] = 0xF, st.ceil[t] = top;
                if (s == 1) st.placed[t] = 0xF;
            }
        sc.lot.stories.push_back(std::move(st));
    }
    const auto lamp = [&](float x, float y, float z, const char* name) {
        LampDef l;
        l.pos[0] = x, l.pos[1] = y, l.pos[2] = z;
        l.range = 25.0f;
        l.name = name;
        sc.lamps.push_back(l);
    };
    lamp(6.5f, 2.95f, 8.0f, "ceiling lamp (5 cm under the ceiling)");
    lamp(12.0f, 0.9f, 10.0f, "table lamp next door");
    lamp(13.85f, 1.9f, 6.0f, "wall sconce (15 cm off the wall)");
    lamp(8.0f, 5.0f, 8.0f, "lamp upstairs");
    for (const Story& st : sc.lot.stories)
        for (const Edge& e : st.edges) WallReceivers(sc.recv, e, st.story);
    FloorReceivers(sc.recv, 4.0f, 4.0f, 14.0f, 12.0f, 0.0f, 0);
    FloorReceivers(sc.recv, 4.0f, 4.0f, 14.0f, 12.0f, 3.0f, 1);
    for (float z = 4.125f; z < 12.0f; z += 0.25f) // the ceilings of story 0
        for (float x = 4.125f; x < 14.0f; x += 0.25f) {
            Receiver r{{x, 3.0f, z}, {0, -1, 0}, 1, 0};
            sc.recv.push_back(r);
        }
    return sc;
}

#ifdef RT_OBJECT_SHAPES // (2.12.3 + RT: GPU lamps for every room and its object shapes are not part of this build)
// The rooms above with furniture (GPU lamps for every room, phase 2b: object shadows): a table (its top only), a column
// (a cylinder), a chair (seat and back), a cupboard (solid), a leafy plant (see-through: left out) and a standing lamp
// whose own body holds its head
std::vector<ObjOcc::Box> Furniture() {
    std::vector<ObjOcc::Box> out;
    const auto add = [&](float x0, float y0, float z0, float x1, float y1, float z1, float opacity = 1.0f) {
        const float lo[3] = {x0, y0, z0}, hi[3] = {x1, y1, z1};
        const float rows[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
        ObjOcc::Box b, back;
        if (!ObjOcc::FromModel(lo, hi, rows, b)) return ObjOcc::Shape::Small;
        b.opacity = opacity;
        const ObjOcc::Shape sh = ObjOcc::Classify(b, &back);
        if (sh == ObjOcc::Shape::Small || sh == ObjOcc::Shape::Huge) return sh;
        out.push_back(b);
        if (sh == ObjOcc::Shape::Chair) out.push_back(back);
        return sh;
    };
    Check(add(5.9f, 0.0f, 7.4f, 7.1f, 0.75f, 8.6f) == ObjOcc::Shape::Slab, "furniture: the table is a slab");
    Check(add(7.4f, 0.0f, 5.4f, 7.7f, 2.6f, 5.7f) == ObjOcc::Shape::Column, "furniture: the column is a cylinder");
    Check(add(5.2f, 0.0f, 10.0f, 5.8f, 1.0f, 10.6f) == ObjOcc::Shape::Chair, "furniture: the chair is a seat and a back");
    Check(add(10.2f, 0.0f, 4.2f, 11.4f, 1.9f, 4.8f) == ObjOcc::Shape::Solid, "furniture: the cupboard is solid");
    add(8.0f, 0.0f, 10.5f, 8.7f, 1.4f, 11.2f, 0.3f); // the plant
    add(11.75f, 0.0f, 9.75f, 12.25f, 0.95f, 10.25f); // the table lamp's own body (its head at 0.9 m)
    return out;
}
Scene FurnishedRooms() {
    Scene sc = IndoorRooms();
    sc.name = "the same rooms furnished (table, column, chair, cupboard, plant)";
    sc.shapes = Furniture();
    std::vector<LampDef> keep;
    for (const LampDef& l : sc.lamps) {
        bool close = false;
        for (const ObjOcc::Box& b : sc.shapes) close = close || ObjOcc::Contains(b, l.pos, ObjOcc::kNearSkip);
        if (!close) keep.push_back(l);
    }
    sc.lamps = keep;
    return sc;
}

// GPU lamps for every room, phase 2b: the occluders' object shapes against the solves' own object test (ObjOcc::Pass, the
// one Object shadows on walls runs in every room solve). A texel x lamp pair is shaded by the solve when its pass is
// under 0.5 (the hard edge of the penumbra) and by the occluders when the segment meets a triangle of the shapes.
void ObjectParityCheck() {
    Scene sc = IndoorRooms();
    sc.shapes = Furniture();
    std::printf("\n== object shadows: the occluders' shapes against the solves' object test, %zu shapes, %zu receivers, %zu lamps\n", sc.shapes.size(),
                sc.recv.size(), sc.lamps.size());
    long n = 0, same = 0, cpuShaded = 0, gpuShaded = 0, leaks = 0, dark = 0;
    for (const LampDef& L : sc.lamps) {
        // the occluders as the shadow of this lamp sees them (the shapes within kNearSkip of its head left out)
        RtLampProxy::Mesh m;
        AddShapes(m, sc.shapes, L.pos, ObjOcc::kNearSkip);
        RtLampProxy::BuildGrid(m);
        long ln = 0, lsame = 0;
        for (const Receiver& r : sc.recv) {
            const float b[3] = {r.p[0] + 0.01f * r.n[0], r.p[1] + 0.01f * r.n[1], r.p[2] + 0.01f * r.n[2]};
            float pass = 1.0f;
            for (const ObjOcc::Box& box : sc.shapes) {
                if (ObjOcc::Contains(box, L.pos, ObjOcc::kOwnLampMargin)) continue; // (the solves: the lamp's own object)
                pass *= ObjOcc::Pass(L.pos, b, box, 0.0f, 0.0f);
            }
            const bool cpu = pass < 0.5f;
            const bool gpu = m.tris.empty() ? false : !RtLampProxy::Clear(m, L.pos, b, ObjOcc::kNearSkip, ObjOcc::kFarSkip);
            n++, ln++;
            if (cpu == gpu) same++, lsame++;
            cpuShaded += cpu, gpuShaded += gpu;
            leaks += cpu && !gpu, dark += gpu && !cpu;
        }
        std::printf("   %-40s %6.2f%% of %ld pairs agree\n", L.name.c_str(), ln ? 100.0 * lsame / ln : 100.0, ln);
    }
    std::printf("   all: %ld pairs, %.2f%% agree; shaded by the solve %ld, by the occluders %ld (lit only on the GPU %ld, dark only on the GPU %ld)\n", n,
                n ? 100.0 * same / n : 100.0, cpuShaded, gpuShaded, leaks, dark);
    Check(cpuShaded > 0, "object shadows: the furniture shades no pair (the scene is wrong)");
    Check(n == 0 || same >= static_cast<long>(0.98 * n), "object shadows: fewer than 98% of pairs agree with the solves' object test");
}

#endif

// ---------------------------------------------------------------- World.txt (a Full capture)
std::vector<float> Floats(const std::string& s) {
    std::vector<float> v;
    std::string t;
    for (char c : s) t += (c == '(' || c == ')' || c == ',' || c == '/') ? ' ' : c;
    std::istringstream in(t);
    std::string w;
    while (in >> w) {
        char* end = nullptr;
        const float f = std::strtof(w.c_str(), &end);
        if (end && *end == 0 && !w.empty()) v.push_back(f);
    }
    return v;
}
std::string After(const std::string& line, const std::string& key) {
    const size_t p = line.find(key);
    return p == std::string::npos ? std::string() : line.substr(p + key.size());
}
struct CapLot {
    uint64_t id = 0;
    RtLampProxy::Lot lot;
    std::map<int, RtLampProxy::Story> stories;
    bool haveXf = false;
};
bool LoadWorld(const std::string& path, std::vector<Scene>& out, int maxLamps) {
    std::ifstream f(path);
    if (!f) return false;
    std::vector<std::string> lines;
    for (std::string l; std::getline(f, l);) lines.push_back(l);
    std::map<uint64_t, CapLot> lots;
    std::multimap<uint64_t, LampDef> lamps;
    enum { kNone, kLights, kGrids, kFloors } sect = kNone;
    CapLot* cur = nullptr;
    RtLampProxy::Story* st = nullptr;
    int floorRows = 0;
    std::vector<float> rowFloors;
    for (size_t i = 0; i < lines.size(); i++) {
        const std::string& L = lines[i];
        if (L.rfind("==== LIGHTS", 0) == 0) sect = kLights;
        else if (L.rfind("==== STORY GRIDS", 0) == 0) sect = kGrids;
        else if (L.rfind("==== PLACED FLOORS", 0) == 0) sect = kFloors;
        else if (L.rfind("==== ", 0) == 0) sect = kNone;
        if (sect == kLights && L.rfind("#", 0) == 0 && (g_allLamps || L.find(" room 0 ") != std::string::npos)) {
            LampDef d;
            const size_t tp = L.find(" type ");
            if (tp == std::string::npos) continue;
            d.type = std::atoi(L.c_str() + tp + 6);
            if (d.type != 3 && d.type != 4 && d.type != 5 && d.type != 11) continue;
            const std::string lotHex = After(L, " lot ").substr(0, 16);
            const uint64_t lot = std::strtoull(lotHex.c_str(), nullptr, 16);
            if (!lot) continue;
            const auto pos = Floats(After(L, " pos ").substr(0, After(L, " pos ").find(')') + 1));
            const auto rng = Floats(After(L, " range ").substr(0, After(L, " range ").find(' ')));
            const auto inten = Floats(After(L, " intensity ").substr(0, After(L, " intensity ").find(')') + 1));
            const auto f0 = Floats(After(L, " F0 ").substr(0, After(L, " F0 ").find(')') + 1));
            if (pos.size() < 3 || rng.empty()) continue;
            for (int k = 0; k < 3; k++) d.pos[k] = pos[k];
            d.range = std::clamp(rng[0], 1.0f, 200.0f);
            d.intensity = !inten.empty() && inten[0] > 0.0f ? inten[0] : 1.0f;
            if (f0.size() >= 3)
                for (int k = 0; k < 3; k++) d.colour[k] = std::max(f0[k], 0.0f);
            if (d.type == 4) {
                const auto ax = Floats(After(L, "axis[+0x170] ").substr(0, After(L, "axis[+0x170] ").find(')') + 1));
                const auto off = Floats(After(L, "offset[+0x158] ").substr(0, After(L, "offset[+0x158] ").find(' ')));
                const auto scl = Floats(After(L, "scale[+0x154] "));
                if (ax.size() >= 3 && !off.empty() && !scl.empty()) {
                    for (int k = 0; k < 3; k++) d.axis[k] = ax[k];
                    d.off = off[0], d.scale = scl[0];
                }
            } else if (d.type == 5) {
                const auto a1 = Floats(After(L, "a1[+0x1A0] ").substr(0, After(L, "a1[+0x1A0] ").find(')') + 1));
                const auto o1 = Floats(After(L, "o1[+0x174] ").substr(0, After(L, "o1[+0x174] ").find(' ')));
                const auto o2 = Floats(After(L, "o2[+0x170] ").substr(0, After(L, "o2[+0x170] ").find(' ')));
                const auto S = Floats(After(L, "S[+0x150] "));
                if (a1.size() >= 3 && !o1.empty() && !o2.empty() && S.size() >= 3) {
                    for (int k = 0; k < 3; k++) d.a1[k] = a1[k], d.S[k] = S[k];
                    d.o1 = o1[0], d.o2 = o2[0];
                }
            }
            d.name = L.substr(0, L.find(" vt "));
            d.room = std::atoi(After(L, " room ").c_str());
            lamps.emplace(lot, d);
        }
        if (sect == kGrids && L.rfind("-- grid ", 0) == 0) {
            // "-- grid mgr .. | lot <16 hex> story <s> | <w>x<h> tiles | ..."
            const size_t bar = L.find(" | lot ");
            if (bar == std::string::npos) continue;
            const std::string rest = L.substr(bar + 7);
            const uint64_t lot = std::strtoull(rest.substr(0, 16).c_str(), nullptr, 16);
            const int story = std::atoi(After(rest, " story ").c_str());
            const std::string dims = After(rest, " | ");
            const int w = std::atoi(dims.c_str()), h = std::atoi(dims.c_str() + dims.find('x') + 1);
            if (!lot || story < 0 || story > 7 || w <= 0 || h <= 0 || w > 256 || h > 256) {
                cur = nullptr, st = nullptr;
                continue;
            }
            cur = &lots[lot];
            cur->id = lot;
            st = &cur->stories[story];
            *st = RtLampProxy::Story{};
            st->story = story, st->w = w, st->h = h;
            st->floorY.assign(static_cast<size_t>(w) * h, RtLampProxy::kNoFloor);
            st->roofed.assign(static_cast<size_t>(w) * h, 0);
            st->ceil.assign(static_cast<size_t>(w) * h, RtLampProxy::kNoFloor);
            st->placed.assign(static_cast<size_t>(w) * h, 0);
            floorRows = 0;
            continue;
        }
        if (sect == kGrids && cur && st) {
            if (L.find("world -> lot (mgr+0xE0) rows:") != std::string::npos && !cur->haveXf) {
                const auto v = Floats(After(L, "rows:"));
                if (v.size() >= 16) {
                    for (int r = 0; r < 4; r++)
                        for (int c = 0; c < 4; c++) cur->lot.worldToLot[r][c] = v[r * 4 + c];
                    cur->haveXf = true;
                }
                continue;
            }
            std::istringstream in(L);
            std::string kind;
            in >> kind;
            if (kind == "Z" || kind == "X" || kind == "D0" || kind == "D1") {
                int a = 0, b = 0;
                in >> a >> b;
                RtLampProxy::Edge e;
                if (kind == "Z") e.x0 = static_cast<float>(a), e.z0 = static_cast<float>(b), e.x1 = static_cast<float>(a + 1), e.z1 = static_cast<float>(b);
                else if (kind == "X") e.x0 = static_cast<float>(a), e.z0 = static_cast<float>(b), e.x1 = static_cast<float>(a), e.z1 = static_cast<float>(b + 1);
                else if (kind == "D0") e.x0 = static_cast<float>(a), e.z0 = static_cast<float>(b), e.x1 = static_cast<float>(a + 1), e.z1 = static_cast<float>(b + 1);
                else e.x0 = static_cast<float>(a), e.z0 = static_cast<float>(b + 1), e.x1 = static_cast<float>(a + 1), e.z1 = static_cast<float>(b);
                const auto foot = Floats(After(L, " foot ").substr(0, After(L, " foot ").find(' ')));
                const auto base = Floats(After(L, " base ").substr(0, After(L, " base ").find(' ')));
                const auto top = Floats(After(L, " top ").substr(0, After(L, " top ").find(' ')));
                if (foot.empty() || base.empty() || top.size() < 3) continue;
                e.foot = foot[0], e.base = base[0], e.topL = top[0], e.topM = top[1], e.topR = top[2];
                if (L.find(" opening#") != std::string::npos) {
                    const auto sill = Floats(After(L, " sill ").substr(0, After(L, " sill ").find(' ')));
                    std::string hs = After(L, " head ");
                    hs = hs.substr(0, hs.find(' '));
                    for (char& c : hs)
                        if (c == '.' && false) c = ' ';
                    // "h0..hm..h4"
                    std::vector<float> heads;
                    for (size_t p = 0; p <= hs.size();) {
                        const size_t q = hs.find("..", p);
                        heads.push_back(std::strtof(hs.substr(p, q == std::string::npos ? std::string::npos : q - p).c_str(), nullptr));
                        if (q == std::string::npos) break;
                        p = q + 2;
                    }
                    std::string us = After(L, " u ");
                    us = us.substr(0, us.find(' '));
                    const size_t dd = us.find("..");
                    if (!sill.empty() && heads.size() >= 3 && dd != std::string::npos) {
                        e.open = true;
                        e.o.sill = sill[0];
                        e.o.u0 = std::strtof(us.substr(0, dd).c_str(), nullptr), e.o.u1 = std::strtof(us.substr(dd + 2).c_str(), nullptr);
                        e.o.head[0] = heads[0], e.o.head[2] = heads[1], e.o.head[4] = heads[2];
                        e.o.head[1] = 0.5f * (heads[0] + heads[1]), e.o.head[3] = 0.5f * (heads[1] + heads[2]);
                    }
                }
                st->edges.push_back(e);
            } else if (kind == "z") {
                int z = 0;
                std::string word;
                in >> z >> word; // "floor:"
                rowFloors.clear();
                std::string v;
                while (in >> v) rowFloors.push_back(v == "-" ? RtLampProxy::kNoFloor : std::strtof(v.c_str(), nullptr));
                floorRows = z;
                if (z >= 0 && z < st->h)
                    for (int x = 0; x < st->w && x < static_cast<int>(rowFloors.size()); x++) {
                        const size_t t = static_cast<size_t>(z) * st->w + x;
                        st->floorY[t] = rowFloors[x];
                        if (rowFloors[x] < RtLampProxy::kNoFloor * 0.5f) st->ceil[t] = rowFloors[x] + 3.0f;
                    }
            } else if (kind == "flags:") {
                std::string v;
                for (int x = 0; x < st->w && (in >> v); x++) {
                    const int fl = static_cast<int>(std::strtol(v.c_str(), nullptr, 16));
                    if (floorRows >= 0 && floorRows < st->h) st->roofed[static_cast<size_t>(floorRows) * st->w + x] = static_cast<uint8_t>((fl >> 4) & 0xF);
                }
            }
        }
        if (sect == kFloors && L.rfind("-- mgr ", 0) == 0) {
            const std::string lotHex = After(L, " lot ").substr(0, 16);
            const uint64_t lot = std::strtoull(lotHex.c_str(), nullptr, 16);
            const int story = std::atoi(After(L, " story ").c_str());
            const auto low = Floats(After(L, "lowest floor ").substr(0, After(L, "lowest floor ").find(' ')));
            const std::string dims = After(L, "(lot y) | ");
            const int w = std::atoi(dims.c_str()), d = std::atoi(dims.c_str() + dims.find('x') + 1);
            cur = nullptr, st = nullptr;
            const auto it = lots.find(lot);
            if (it == lots.end() || !it->second.stories.count(story)) continue;
            RtLampProxy::Story& s = it->second.stories[story];
            if (s.w != w || s.h != d) continue;
            if (!low.empty()) s.plane = low[0];
            for (size_t k = i + 1; k < lines.size() && k <= i + 2 + static_cast<size_t>(d); k++) {
                std::istringstream in(lines[k]);
                std::string zw, hex;
                int z = -1;
                if (!(in >> zw) || zw != "z") continue;
                in >> z >> hex;
                if (z < 0 || z >= d) continue;
                for (int x = 0; x < w && x < static_cast<int>(hex.size()); x++) {
                    const char c = hex[x];
                    const int v = c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : 0;
                    s.placed[static_cast<size_t>(z) * w + x] = static_cast<uint8_t>(v);
                }
            }
        }
    }
    for (auto& [id, cl] : lots) {
        int walls = 0;
        for (auto& [s, story] : cl.stories) walls += static_cast<int>(story.edges.size());
        const auto range = lamps.equal_range(id);
        const int nl = static_cast<int>(std::distance(range.first, range.second));
        if (walls < 20 || nl == 0 || !cl.haveXf) continue;
        Scene sc;
        sc.name = std::format("lot {:016X} ({} stories, {} wall edges, {} outdoor lamps)", id, cl.stories.size(), walls, nl);
        for (auto& [s, story] : cl.stories) cl.lot.stories.push_back(story);
        sc.lot = cl.lot;
        int taken = 0;
        for (auto it = range.first; it != range.second && taken < maxLamps; ++it, ++taken) {
            LampDef d = it->second;
            float lp[3];
            RtLampProxy::ToLot(sc.lot, d.pos, lp);
            for (int k = 0; k < 3; k++) d.pos[k] = lp[k];
            // the cone's axes into lot space
            float ax[3], a1[3];
            RtLampProxy::DirToLot(sc.lot, d.axis, ax);
            RtLampProxy::DirToLot(sc.lot, d.a1, a1);
            for (int k = 0; k < 3; k++) d.axis[k] = ax[k], d.a1[k] = a1[k];
            sc.lamps.push_back(d);
        }
        for (const RtLampProxy::Story& story : sc.lot.stories) {
            for (const RtLampProxy::Edge& e : story.edges) WallReceivers(sc.recv, e, story.story);
            for (int z = 0; z < story.h; z++)
                for (int x = 0; x < story.w; x++) {
                    const size_t t = static_cast<size_t>(z) * story.w + x;
                    const bool outdoorGround = story.story == 0 && !story.roofed[t];
                    const bool slab = story.story >= 1 && story.placed[t] && !story.roofed[t];
                    if (!outdoorGround && !slab) continue;
                    const float y = story.floorY[t] < RtLampProxy::kNoFloor * 0.5f ? story.floorY[t] : story.plane;
                    if (!(y < RtLampProxy::kNoFloor * 0.5f)) continue;
                    FloorReceivers(sc.recv, static_cast<float>(x), static_cast<float>(z), static_cast<float>(x + 1), static_cast<float>(z + 1), y, story.story, 0.5f);
                }
        }
        out.push_back(std::move(sc));
    }
    return true;
}

// ---------------------------------------------------------------- the law of a lamp definition
RtLampModel::Lamp ToLamp(const LampDef& d, float mult = 1.0f) {
    RtLampModel::Lamp L;
    for (int k = 0; k < 3; k++) L.law.pos[k] = d.pos[k], L.law.colour[k] = d.colour[k];
    const float w = d.range * d.intensity;
    L.law.W = 0.075f * w * (d.type == 11 ? 3.3333f : 1.0f);
    L.law.cmax = 2.0f;
    GpuLampLaw::NoCone(L.law);
    if (d.type == 4 && d.scale > 0.0f) GpuLampLaw::SpotCone(L.law, d.axis, d.off, d.scale);
    else if (d.type == 5) GpuLampLaw::ShadedCone(L.law, d.a1, 5.0f, d.o1, 5.0f, d.o2, d.S);
    float r0, R;
    LampReach::Radii(w, r0, R);
    L.law.r0sq = r0 * r0, L.law.invR2 = 1.0f / (R * R);
    L.mult = mult;
    return L;
}

// ---------------------------------------------------------------- 1. shadows: GPU against the reference
struct ShadowTally {
    long n = 0, within2 = 0, within1 = 0, leaks = 0, darkened = 0;
    double sumAbs = 0.0;
    long visWithin05 = 0;
    int debugShown = 0;
    long lit = 0;                      // pairs lit after the threshold, by the reference or the GPU
    double accErr = 0.0, rlgErr = 0.0; // |light - law x the reference's share, thresholded| over them: Acc, AccRlg
    double accRlgMax = 0.0;            // the largest |Acc - AccRlg| (one Shadow)
    std::map<std::string, long> leakNear;
};
std::string NearWhat(const Scene& sc, const Receiver& r) {
    // the nearest geometry: a slab edge, a wall within 0.3 m, an opening
    float best = 1e9f;
    std::string what = "open ground";
    for (const RtLampProxy::Story& s : sc.lot.stories)
        for (const RtLampProxy::Edge& e : s.edges) {
            const float dx = e.x1 - e.x0, dz = e.z1 - e.z0, l2 = dx * dx + dz * dz;
            const float u = std::clamp(((r.p[0] - e.x0) * dx + (r.p[2] - e.z0) * dz) / l2, 0.0f, 1.0f);
            const float px = e.x0 + dx * u - r.p[0], pz = e.z0 + dz * u - r.p[2];
            const float d = std::sqrt(px * px + pz * pz);
            if (r.p[1] < e.foot - 0.2f || r.p[1] > RtLampProxy::EdgeTop(e, u) + 0.2f) continue;
            if (d < best) best = d, what = e.open ? "an opening's wall" : (u <= 0.0f || u >= 1.0f) ? "a wall's end" : "a wall";
        }
    if (best > 0.3f) {
        for (const RtLampProxy::Story& s : sc.lot.stories)
            if (s.story >= 1 && std::fabs(r.p[1] - s.plane) < 0.5f) return "a slab";
        return "open ground";
    }
    return what;
}
bool ShadowCompare(const Scene& sc, int face, float radius, ShadowTally& total, bool printLamps, bool fast = false) {
    RtLampProxy::Mesh mesh;
    RtLampProxy::Build(sc.lot, mesh, sc.ceilingsOpen);
#ifdef RT_OBJECT_SHAPES
    if (!sc.shapes.empty()) {
        AddShapes(mesh, sc.shapes, nullptr, 0.0f); // (the scene's lamps stand clear of its objects)
        RtLampProxy::BuildGrid(mesh);
    }
#endif
    std::printf("\n[%s] proxy: %zu triangles (%d walls, %d openings, %d roof, %d slab triangles), %zu receivers, %zu lamps, face %d, ball %.2f m%s\n", sc.name.c_str(),
                mesh.tris.size(), mesh.walls, mesh.openings, mesh.roofTris, mesh.slabTris, sc.recv.size(), sc.lamps.size(), face, radius,
                fast ? ", the dragged lamp's shader (AccFastPS)" : "");
    RtLampGpu::Engine eng(RtLampGpu::PlainCalls());
    std::string err;
    if (!eng.Init(g_dev, g_shaders, face, err) || !eng.SetProxy(g_dev, mesh, err) || !eng.EnsureTiles(g_dev, 1, err)) {
        Check(false, "engine: " + err);
        return false;
    }
    // receivers as texels of one map (w x h)
    const int w = 1024;
    std::vector<RtLampGpu::TexelVertex> verts;
    std::vector<uint32_t> stamp;
    uint32_t mark = 0;
    std::vector<size_t> used;
    for (size_t li = 0; li < sc.lamps.size(); li += RtLampGpu::kAtlasLamps) {
        const int n = static_cast<int>(std::min<size_t>(RtLampGpu::kAtlasLamps, sc.lamps.size() - li));
        float pos[RtLampGpu::kAtlasLamps][3];
        std::vector<RtLampModel::Lamp> lamps;
        for (int k = 0; k < n; k++) {
            for (int c = 0; c < 3; c++) pos[k][c] = sc.lamps[li + k].pos[c];
            lamps.push_back(ToLamp(sc.lamps[li + k]));
        }
        g_dev->BeginScene();
        eng.Begin(g_dev);
        if (!eng.RenderShadows(g_dev, pos, n, 64.0f, err)) {
            eng.End(g_dev);
        g_dev->EndScene();
            Check(false, "shadows: " + err);
            return false;
        }
        eng.End(g_dev);
        g_dev->EndScene();
        if (li == 0 && (printLamps || fast)) { // the GPU time of one relight: every receiver of the scene with this batch's lamps
            const auto flush = [] {
                IDirect3DQuery9* q = nullptr;
                if (FAILED(g_dev->CreateQuery(D3DQUERYTYPE_EVENT, &q)) || !q) return;
                q->Issue(D3DISSUE_END);
                while (q->GetData(nullptr, 0, D3DGETDATA_FLUSH) == S_FALSE) {
                }
                q->Release();
            };
            const int tw = 1024;
            const size_t count = std::min<size_t>(sc.recv.size(), static_cast<size_t>(tw) * 1024);
            std::vector<RtLampGpu::TexelVertex> tv(count);
            for (size_t j = 0; j < count; j++) {
                tv[j].x = static_cast<float>(j % tw), tv[j].y = static_cast<float>(j / tw), tv[j].w = 1.0f;
                for (int c = 0; c < 3; c++) tv[j].p[c] = sc.recv[j].p[c], tv[j].n[c] = sc.recv[j].n[c];
            }
            RtLampGpu::Map tm;
            tm.w = tw, tm.h = static_cast<int>((count + tw - 1) / tw), tm.count = static_cast<UINT>(count);
            RtLampGpu::Engine::MakeVb(g_dev, tv.data(), tm.count, &tm.vb);
            IDirect3DTexture9* out = nullptr;
            g_dev->CreateTexture(tm.w, tm.h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &out, nullptr);
            RtLampGpu::Params p;
            p.radius = radius;
            p.fast = fast;
            g_dev->BeginScene(); // (once before: the scratch targets are made at the first use)
            eng.Begin(g_dev);
            eng.Accumulate(g_dev, tm, lamps.data(), n, p, true, err);
            eng.Compose(g_dev, tm, nullptr, out, p, err);
            eng.End(g_dev);
            g_dev->EndScene();
            flush();
            double shMs = 1e9, relMs = 1e9; // (the fastest of 5 runs: the wall clock around a flush jitters by about a millisecond)
            bool ok = true;
            for (int rep = 0; rep < 5; rep++) {
                const auto t0 = std::chrono::steady_clock::now();
                g_dev->BeginScene();
                eng.Begin(g_dev);
                eng.RenderShadows(g_dev, pos, n, 64.0f, err);
                eng.End(g_dev);
                g_dev->EndScene();
                flush();
                const auto t1 = std::chrono::steady_clock::now();
                g_dev->BeginScene();
                eng.Begin(g_dev);
                ok = eng.Accumulate(g_dev, tm, lamps.data(), n, p, true, err) && eng.Compose(g_dev, tm, nullptr, out, p, err) && ok;
                eng.End(g_dev);
                g_dev->EndScene();
                flush();
                const auto t2 = std::chrono::steady_clock::now();
                shMs = std::min(shMs, std::chrono::duration<double, std::milli>(t1 - t0).count());
                relMs = std::min(relMs, std::chrono::duration<double, std::milli>(t2 - t1).count());
            }
            std::printf("  GPU time%s: %d lamps' shadows %.2f ms; %zu texels x %d lamps relit %.2f ms%s\n", fast ? " (dragged)" : "", n, shMs, count, n, relMs,
                        ok ? "" : (" (failed: " + err + ")").c_str());
            tm.vb->Release();
            if (out) out->Release();
        }
        for (int k = 0; k < n; k++) {
            const LampDef& ld = sc.lamps[li + k];
            // the receivers this lamp reaches (law >= 1/255 somewhere), at most 60000, evenly
            used.clear();
            for (size_t i = 0; i < sc.recv.size(); i++) {
                float law[3];
                GpuLampLaw::Eval(lamps[k].law, sc.recv[i].p, sc.recv[i].n, GpuLampLaw::WallReach(sc.recv[i].n, true), law);
                if (std::max({law[0], law[1], law[2]}) >= 1.0f / 255.0f) used.push_back(i);
            }
            if (used.size() > 60000) {
                std::vector<size_t> sub;
                const double step = static_cast<double>(used.size()) / 60000.0;
                for (double s = 0; s < used.size(); s += step) sub.push_back(used[static_cast<size_t>(s)]);
                used.swap(sub);
            }
            if (used.empty()) continue;
            const int h = static_cast<int>((used.size() + w - 1) / w);
            verts.assign(used.size(), RtLampGpu::TexelVertex{});
            for (size_t j = 0; j < used.size(); j++) {
                const Receiver& r = sc.recv[used[j]];
                RtLampGpu::TexelVertex& v = verts[j];
                v.x = static_cast<float>(j % w), v.y = static_cast<float>(j / w), v.z = 0.0f, v.w = 1.0f;
                for (int c = 0; c < 3; c++) v.p[c] = r.p[c], v.n[c] = r.n[c];
            }
            RtLampGpu::Map m;
            m.w = w, m.h = h, m.count = static_cast<UINT>(verts.size());
            if (!RtLampGpu::Engine::MakeVb(g_dev, verts.data(), m.count, &m.vb)) {
                Check(false, "vertex buffer");
                return false;
            }
            RtLampGpu::Params p;
            p.radius = radius;
            p.thr = 0.0f; // the share alone
            p.debugLamp = k;
            p.aa = 0.0f;
            p.fast = fast;
            g_dev->BeginScene();
        eng.Begin(g_dev);
            const bool ok = eng.Accumulate(g_dev, m, lamps.data(), n, p, true, err);
            eng.End(g_dev);
        g_dev->EndScene();
            std::vector<float> sum;
            bool read = ok && eng.ReadSum(g_dev, w, h, sum);
            // the light the maps get, the threshold and its fade on: lamp k alone through Acc (outdoor lamps) and AccRlg
            RtLampGpu::Params pl;
            pl.radius = radius, pl.fast = fast;
            std::vector<RtLampModel::Lamp> alone = lamps;
            for (int i = 0; i < n; i++) alone[i].mult = i == k ? 1.0f : 0.0f;
            std::vector<float> cell(RtLampGpu::Engine::kTableCols * 4, 0.0f), accL, rlgL;
            cell[0] = cell[1] = 1.0f;
            const int tile = 0;
            g_dev->BeginScene();
            eng.Begin(g_dev);
            read = read && eng.Accumulate(g_dev, m, alone.data(), n, pl, true, err) && eng.ReadSum(g_dev, w, h, accL);
            read = read && eng.RenderShadowTile(g_dev, lamps[k].law.pos, tile, 64.0f, err) && eng.SetTable(g_dev, cell.data(), 1, err) &&
                   eng.AccumulateRlg(g_dev, m, &lamps[k], &tile, 1, pl, true, err) && eng.ReadSum(g_dev, w, h, rlgL);
            eng.End(g_dev);
            g_dev->EndScene();
            m.vb->Release();
            if (!read) {
                Check(false, "accumulate / read: " + err);
                return false;
            }
            ShadowTally t;
            std::map<std::string, long> leakHere;
            for (size_t j = 0; j < used.size(); j++) {
                const Receiver& r = sc.recv[used[j]];
                const float gpu = sum[j * 4];
                if (gpu < 0.0f) continue; // (the lamp adds nothing here: N.l <= 0 or outside its cones)
                float ref = RtLampProxy::Visibility(mesh, ld.pos, radius, r.p, 8, &stamp, &mark);
                if (ref > 0.0f && ref < 1.0f) ref = RtLampProxy::Visibility(mesh, ld.pos, radius, r.p, RtLampProxy::kRefSamples, &stamp, &mark);
                float law[3];
                GpuLampLaw::Eval(lamps[k].law, r.p, r.n, GpuLampLaw::WallReach(r.n, true), law);
                const float lawMax = std::min(std::max({law[0], law[1], law[2]}), 1.0f);
                const float d8 = std::fabs(gpu - ref) * lawMax * 255.0f;
                float want[3] = {law[0] * ref, law[1] * ref, law[2] * ref};
                const float cs = (want[2] + want[1]) + want[0], x = (cs - pl.thr) / pl.thr;
                const float fade = !(cs >= pl.thr) ? 0.0f : cs < 2.0f * pl.thr ? x * x * (3.0f - 2.0f * x) : 1.0f;
                double accD = 0.0, rlgD = 0.0, litGpu = 0.0;
                for (int c = 0; c < 3; c++) {
                    want[c] *= fade, litGpu += accL[j * 4 + c] + rlgL[j * 4 + c];
                    accD += std::fabs(accL[j * 4 + c] - want[c]), rlgD += std::fabs(rlgL[j * 4 + c] - want[c]);
                    total.accRlgMax = std::max<double>(total.accRlgMax, std::fabs(accL[j * 4 + c] - rlgL[j * 4 + c]));
                }
                if (fade > 0.0f || litGpu > 0.0) total.lit++, total.accErr += accD, total.rlgErr += rlgD;
                t.n++;
                t.sumAbs += std::fabs(gpu - ref);
                if (std::fabs(gpu - ref) <= 0.05f) t.visWithin05++;
                if (d8 <= 2.0f) t.within2++;
                if (d8 <= 1.0f) t.within1++;
                if (g_debug && d8 > 8.0f && t.debugShown < 8) {
                    t.debugShown++;
                    std::printf("      off by %.0f/255 at (%.2f %.2f %.2f) n (%.0f %.0f %.0f), %.2f m from the lamp: reference %.3f, GPU %.3f\n", d8, r.p[0], r.p[1], r.p[2], r.n[0], r.n[1],
                                r.n[2], std::sqrt((r.p[0] - ld.pos[0]) * (r.p[0] - ld.pos[0]) + (r.p[1] - ld.pos[1]) * (r.p[1] - ld.pos[1]) + (r.p[2] - ld.pos[2]) * (r.p[2] - ld.pos[2])), ref, gpu);
                }
                if (ref <= 0.0f && gpu >= 0.5f && lawMax * 255.0f >= 2.0f) {
                    t.leaks++;
                    leakHere[NearWhat(sc, r)]++;
                }
                if (ref >= 1.0f && gpu <= 0.5f && lawMax * 255.0f >= 2.0f) t.darkened++;
            }
            if (printLamps) {
                std::printf("  lamp %-28s %6ld texels: within 2/255 %6.2f%%, within 1/255 %6.2f%%, share within 0.05 %6.2f%%, mean |share diff| %.4f, leaks %ld, wrongly dark %ld\n",
                            ld.name.c_str(), t.n, t.n ? 100.0 * t.within2 / t.n : 100.0, t.n ? 100.0 * t.within1 / t.n : 100.0, t.n ? 100.0 * t.visWithin05 / t.n : 100.0,
                            t.n ? t.sumAbs / t.n : 0.0, t.leaks, t.darkened);
                if (g_debug) {
                    // what is around the lamp: the nearest triangle (its distance from the lamp's centre)
                    float best = 1e9f;
                    for (const RtLampProxy::Tri& tr : mesh.tris) {
                        float c[3] = {(tr.v[0][0] + tr.v[1][0] + tr.v[2][0]) / 3, (tr.v[0][1] + tr.v[1][1] + tr.v[2][1]) / 3, (tr.v[0][2] + tr.v[1][2] + tr.v[2][2]) / 3};
                        const float dx = c[0] - ld.pos[0], dy = c[1] - ld.pos[1], dz = c[2] - ld.pos[2];
                        best = std::min(best, std::sqrt(dx * dx + dy * dy + dz * dz));
                    }
                    std::printf("      lamp at lot (%.2f %.2f %.2f) type %d, nearest triangle centre %.2f m\n", ld.pos[0], ld.pos[1], ld.pos[2], ld.type, best);
                }
            }
            for (const auto& [what, c] : leakHere) {
                if (printLamps) std::printf("      leaks near %s: %ld\n", what.c_str(), c);
                total.leakNear[what] += c;
            }
            total.n += t.n, total.within2 += t.within2, total.within1 += t.within1, total.leaks += t.leaks, total.darkened += t.darkened, total.sumAbs += t.sumAbs,
                total.visWithin05 += t.visWithin05;
        }
    }
    const auto& s = eng.GetStats();
    std::printf("  GPU: %ld shadow tiles (%ld draws), %ld sum draws\n", s.shadowTiles, s.shadowDraws, s.accDraws);
    return true;
}

// A wall with an optional door and a second wall behind it, one lamp: Shadow's walk against the reference. Behind a door the outer
// half of the penumbra has to exist (a segment passing the jamb is not blocked by being behind it as the lamp's centre sees it);
// a solid wall must not leak with the lamp 0.4 m off it; a wall the lamp's centre cannot see past the door is one depth layer
// past what the map holds, so a thin strip may pass there (bounded here)
Scene WallScene(const char* name, bool door, bool second, float lampZ) {
    using namespace RtLampProxy;
    Scene sc;
    sc.name = name;
    const int W = 24, H = 24;
    Story st;
    st.story = 0, st.w = W, st.h = H;
    st.floorY.assign(W * H, kNoFloor);
    st.roofed.assign(W * H, 0);
    st.ceil.assign(W * H, kNoFloor);
    st.placed.assign(W * H, 0);
    st.plane = 0.0f;
    for (int x = 4; x < 16; x++) {
        Edge e = EdgeZ(x, 10, 0.0f, 3.0f);
        if (door && (x == 9 || x == 10)) {
            e.open = true, e.o.u0 = x == 9 ? 0.05f : 0.0f, e.o.u1 = x == 9 ? 1.0f : 0.95f, e.o.sill = 0.0f;
            for (float& h : e.o.head) h = 2.4f;
        }
        st.edges.push_back(e);
        if (second) st.edges.push_back(EdgeZ(x, 12, 0.0f, 3.0f));
    }
    sc.lot.stories.push_back(std::move(st));
    LampDef l;
    l.pos[0] = 9.7f, l.pos[1] = 1.35f, l.pos[2] = lampZ;
    l.name = name;
    sc.lamps.push_back(l);
    for (const Edge& e : sc.lot.stories[0].edges) WallReceivers(sc.recv, e, 0);
    FloorReceivers(sc.recv, 4.0f, 5.0f, 16.0f, 16.0f, 0.0f, 0, 0.125f);
    return sc;
}
void PenumbraCheck() {
    std::printf("\n[shadows] the walk past a door's jamb, by a solid wall and with a wall behind the door\n");
    const auto run = [](const Scene& sc) {
        ShadowTally t;
        ShadowCompare(sc, 256, 0.15f, t, true);
        return t;
    };
    const auto mean = [](const ShadowTally& t) { return t.n ? t.sumAbs / t.n : 0.0; };
    const ShadowTally door = run(WallScene("door", true, false, 8.7f)), solid = run(WallScene("solid wall, lamp 0.4 m off it", false, false, 9.6f)),
                      stacked = run(WallScene("second wall behind the door", true, true, 8.7f));
    Check(door.n > 0 && mean(door) < 0.005, std::format("the door's penumbra: mean share error {:.4f}, the disc search's was 0.0065", mean(door)));
    Check(solid.n > 0 && solid.leaks == 0 && mean(solid) < 0.001, std::format("a solid wall leaks: {} texels, mean share error {:.4f}", solid.leaks, mean(solid)));
    Check(stacked.n > 0 && stacked.leaks <= stacked.n / 200, std::format("a wall behind the door: {} of {} texels leak", stacked.leaks, stacked.n));
    // (the walk that went round the jamb stopped testing: 0.0099, the penumbra's strip through the second wall to the floor behind it)
    Check(mean(stacked) < 0.0054, std::format("a wall behind the door: mean share error {:.4f}, the disc search's was 0.0057", mean(stacked)));
}

// ---------------------------------------------------------------- 3. the bake split and the blur, bit for bit
bool SplitCheck() {
    std::printf("\n[bake split] base + GPU lamps against the game's texel (no occluder: share 1)\n");
    std::mt19937 rng(1234);
    std::uniform_real_distribution<float> U(0.0f, 1.0f);
    const int w = 256, h = 128;
    // lamps around a floor and a wall (lot space)
    std::vector<RtLampModel::Lamp> lamps;
    for (int i = 0; i < 6; i++) {
        LampDef d;
        d.pos[0] = 10.0f + 8.0f * U(rng), d.pos[1] = 0.5f + 3.0f * U(rng), d.pos[2] = 10.0f + 8.0f * U(rng);
        d.type = i == 2 ? 5 : i == 4 ? 4 : 3;
        d.colour[0] = 0.6f + 0.4f * U(rng), d.colour[1] = 0.6f + 0.4f * U(rng), d.colour[2] = 0.5f + 0.5f * U(rng);
        d.range = 30.0f + 40.0f * U(rng);
        d.scale = 4.9f, d.off = -0.32f;
        d.o1 = 0.7f, d.o2 = 0.5f;
        lamps.push_back(ToLamp(d, i == 0 ? 2.0f : 1.0f));
    }
    RtLampGpu::Engine eng(RtLampGpu::PlainCalls());
    std::string err;
    RtLampProxy::Mesh empty;
    if (!eng.Init(g_dev, g_shaders, 64, err) || !eng.SetProxy(g_dev, empty, err)) {
        Check(false, "engine: " + err);
        return false;
    }
    const float thr = 3.0f / 255.0f;
    struct Tx {
        RtLampModel::TexelIn in;
        float p[3], n[3];
        uint32_t game = 0, model = 0;
        bool past1 = false; // the sum passes 1: packed with its hue kept (outdoor_delta_model.h KeepHue)
        float sumMax = 0;   // its largest channel (the outdoor scale gives it back)
    };
    std::vector<Tx> tx;
    std::vector<RtLampGpu::TexelVertex> verts;
    std::vector<RtLampGpu::TexelDyn> dyn;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            if ((x * 7 + y * 3) % 5 == 0) continue; // texels no sample writes: the game's map stays there
            Tx t;
            const bool wall = y < h / 2;
            t.p[0] = 10.0f + 8.0f * x / w, t.p[1] = wall ? 0.2f + 2.6f * (y / static_cast<float>(h / 2)) : 0.0f, t.p[2] = wall ? 9.95f : 10.0f + 8.0f * (y - h / 2) / (h / 2.0f);
            t.n[0] = 0, t.n[1] = wall ? 0.0f : 1.0f, t.n[2] = wall ? 1.0f : 0.0f;
            // the baked rest (window lights and the like) and its parts
            for (float& b : t.in.base) b = U(rng) < 0.2f ? 0.0f : 0.6f * U(rng) * U(rng);
            t.in.masked = !wall && (x % 3 != 0);
            t.in.gated = t.in.masked && (x % 2 == 0);
            t.in.baseVis = t.in.masked ? 0.3f * U(rng) : 0.0f;
            t.in.baseAll = t.in.masked ? t.in.baseVis + 0.2f * U(rng) : 0.0f;
            t.in.under = t.in.gated ? (U(rng) < 0.2f ? 1.0f : U(rng) < 0.3f ? -1.0f : U(rng)) : -1.0f;
            t.in.alpha = 0.0f;
            t.in.dither = (y % 4 == 3) ? 0.0f : RoomMapDither::Offset(x, y) / 255.0f;
            // the GPU lamps, as the room solve would have added them (each a part of S): share 1
            float G[3] = {}, gVis = 0, gAll = 0;
            for (const auto& L : lamps) {
                RtLampModel::Add(L, t.p, t.n, 1.0f, true, thr, true, G);
                if (t.in.masked) RtLampModel::AddMask(L, t.p, 1.0f, thr, gVis, gAll);
            }
            // the game's texel: the plain sum packed (the parts summed in the solve's order)
            float S[3] = {t.in.base[0] + G[0], t.in.base[1] + G[1], t.in.base[2] + G[2]};
            t.sumMax = std::max({S[0], S[1], S[2]});
            t.past1 = t.sumMax > 1.0f;
            float alpha = t.in.masked ? OutdoorDeltaModel::MaskAlpha(t.in.baseVis + gVis, t.in.baseAll + gAll, t.in.gated ? t.in.under : -1.0f) : 0.0f;
            float c[4];
            OutdoorDeltaModel::PackInput(S, alpha, c);
            if (t.in.dither != 0.0f) OutdoorDeltaModel::Dither(c, t.in.dither);
            t.game = OutdoorDeltaModel::PackModel(c);
            t.model = RtLampModel::Compose(t.in, G, gVis, gAll);
            RtLampGpu::TexelVertex v{};
            v.x = static_cast<float>(x), v.y = static_cast<float>(y), v.z = 0.0f, v.w = 1.0f;
            for (int k = 0; k < 3; k++) v.p[k] = t.p[k], v.n[k] = t.n[k], v.base[k] = t.in.base[k];
            v.p[3] = (t.in.masked ? 1.0f : 0.0f) + (t.in.gated ? 2.0f : 0.0f);
            v.n[3] = t.in.alpha;
            v.base[3] = t.in.dither;
            v.mask[0] = t.in.baseVis, v.mask[1] = t.in.baseAll;
            RtLampGpu::TexelDyn dv{};
            dv.v[0] = gAll, dv.v[1] = t.in.under;
            tx.push_back(t);
            verts.push_back(v);
            dyn.push_back(dv);
            (void)x;
        }
    // the game's map under the texels (a texel no sample writes keeps it): a pattern
    std::vector<uint32_t> gamePx(static_cast<size_t>(w) * h);
    for (size_t i = 0; i < gamePx.size(); i++) gamePx[i] = 0xFF000000u | static_cast<uint32_t>(i * 2654435761u >> 8);
    IDirect3DTexture9* game = nullptr;
    if (!RtLampGpu::Engine::MakePreblur(g_dev, w, h, gamePx.data(), &game)) {
        Check(false, "game map texture");
        return false;
    }
    IDirect3DTexture9* out = nullptr;
    g_dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &out, nullptr);
    RtLampGpu::Map m;
    m.w = w, m.h = h, m.count = static_cast<UINT>(verts.size());
    RtLampGpu::Engine::MakeVb(g_dev, verts.data(), m.count, &m.vb);
    RtLampGpu::Engine::MakeDyn(g_dev, dyn.data(), m.count, &m.dyn);
    RtLampGpu::Params p;
    p.thr = thr, p.fade = true, p.reachOn = true, p.mask = true;
    float pos[RtLampGpu::kAtlasLamps][3];
    for (size_t k = 0; k < lamps.size(); k++)
        for (int c = 0; c < 3; c++) pos[k][c] = lamps[k].law.pos[c];
    // two batches (3 + 3 lamps): the sum's ping-pong
    g_dev->BeginScene();
        eng.Begin(g_dev);
    bool ok = eng.RenderShadows(g_dev, pos, 3, 64.0f, err) && eng.Accumulate(g_dev, m, lamps.data(), 3, p, true, err);
    for (int k = 0; k < 3; k++)
        for (int c = 0; c < 3; c++) pos[k][c] = lamps[3 + k].law.pos[c];
    ok = ok && eng.RenderShadows(g_dev, pos, 3, 64.0f, err) && eng.Accumulate(g_dev, m, lamps.data() + 3, 3, p, false, err) && eng.Compose(g_dev, m, game, out, p, err);
    eng.End(g_dev);
        g_dev->EndScene();
    Check(ok, "relight passes: " + err);
    // read back
    std::vector<uint32_t> got(static_cast<size_t>(w) * h);
    {
        IDirect3DSurface9 *s = nullptr, *sys = nullptr;
        out->GetSurfaceLevel(0, &s);
        g_dev->CreateOffscreenPlainSurface(w, h, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, nullptr);
        if (SUCCEEDED(g_dev->GetRenderTargetData(s, sys))) {
            D3DLOCKED_RECT lr{};
            sys->LockRect(&lr, nullptr, D3DLOCK_READONLY);
            for (int y = 0; y < h; y++) std::memcpy(&got[static_cast<size_t>(y) * w], static_cast<BYTE*>(lr.pBits) + lr.Pitch * y, w * 4);
            sys->UnlockRect();
        }
        sys->Release();
        s->Release();
    }
    long exactGame = 0, off1Game = 0, worseGame = 0, exactModel = 0, untouched = 0, untouchedBad = 0;
    int maxGame = 0;
    for (size_t i = 0; i < tx.size(); i++) {
        const int x = static_cast<int>(verts[i].x), y = static_cast<int>(verts[i].y);
        const uint32_t g = got[static_cast<size_t>(y) * w + x];
        const int dg = OutdoorDeltaModel::MaxLevelDiff(g, tx[i].game);
        maxGame = std::max(maxGame, dg);
        (dg == 0 ? exactGame : dg == 1 ? off1Game : worseGame)++;
        if (dg > 1 && worseGame <= 8)
            std::printf("    texel %d,%d %s%s%s: GPU %08X, game %08X, model %08X, base %.3f %.3f %.3f dither %.4f under %.2f\n", x, y, tx[i].in.masked ? "masked " : "",
                        tx[i].in.gated ? "gated " : "", y < h / 2 ? "wall" : "floor", g, tx[i].game, tx[i].model, tx[i].in.base[0], tx[i].in.base[1], tx[i].in.base[2],
                        tx[i].in.dither, tx[i].in.under);
        if (g == tx[i].model) exactModel++;
    }
    if (worseGame && g_debug) { // each lamp of the first texel off: its share and law luminance on the GPU and the CPU's law
        size_t bad = 0;
        for (size_t i = 0; i < tx.size(); i++) {
            const int x = static_cast<int>(verts[i].x), y = static_cast<int>(verts[i].y);
            if (OutdoorDeltaModel::MaxLevelDiff(got[static_cast<size_t>(y) * w + x], tx[i].game) > 1) {
                bad = i;
                break;
            }
        }
        const int bx = static_cast<int>(verts[bad].x), by = static_cast<int>(verts[bad].y);
        for (int b = 0; b < 2; b++)
            for (int k = 0; k < 3; k++) {
                RtLampGpu::Params q = p;
                q.debugLamp = k;
                g_dev->BeginScene();
                eng.Begin(g_dev);
                eng.Accumulate(g_dev, m, lamps.data() + 3 * b, 3, q, true, err);
                eng.End(g_dev);
                g_dev->EndScene();
                std::vector<float> sum;
                eng.ReadSum(g_dev, w, h, sum);
                float law[3];
                GpuLampLaw::Eval(lamps[3 * b + k].law, tx[bad].p, tx[bad].n, GpuLampLaw::WallReach(tx[bad].n, true), law);
                const float* s = &sum[(static_cast<size_t>(by) * w + bx) * 4];
                std::printf("    texel %d,%d lamp %d: GPU share %.3f law lum %.5f | CPU law lum %.5f\n", bx, by, 3 * b + k, s[0], s[1], OutdoorDeltaModel::Lum3(law));
            }
    }
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            if ((x * 7 + y * 3) % 5 == 0) {
                untouched++;
                if (got[static_cast<size_t>(y) * w + x] != gamePx[static_cast<size_t>(y) * w + x]) untouchedBad++;
            }
    std::printf("  %zu texels: the game's texel exactly %ld, 1/255 off %ld, more %ld (largest %d/255); the CPU model exactly %ld; %ld texels no sample writes kept "
                "the game's map (%ld changed)\n",
                tx.size(), exactGame, off1Game, worseGame, maxGame, exactModel, untouched, untouchedBad);
    Check(worseGame == 0, "a relit texel more than 1/255 from the game's texel");
    Check(exactGame >= static_cast<long>(tx.size() * 0.99), "fewer than 99% of relit texels equal to the game's");
    Check(untouchedBad == 0, "a texel no sample writes changed");
    {
        long past = 0, pastExact = 0;
        for (size_t i = 0; i < tx.size(); i++)
            if (tx[i].past1) {
                past++;
                const uint32_t g = got[static_cast<size_t>(verts[i].y) * w + static_cast<size_t>(verts[i].x)];
                if (OutdoorDeltaModel::MaxLevelDiff(g, tx[i].game) == 0) pastExact++;
            }
        std::printf("  of them %ld with a sum past 1 (its hue kept), %ld equal to the game's texel\n", past, pastExact);
        Check(past > 0 && pastExact == past, "texels with a sum past 1: the compose shader keeps the hue as the solve does");
    }
    if (!m.blur) {
        // the outdoor scale (rt_outdoor_lamps.h ScaleOf): composed again with the second target, the colour is the same and
        // 2^(kScaleLog2 r) gives back the sum's largest channel past 1 (to 8), 1 within 1 and where no texel lands
        IDirect3DTexture9* scale = nullptr;
        g_dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &scale, nullptr);
        IDirect3DTexture9* out3 = nullptr;
        g_dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &out3, nullptr);
        for (int k = 0; k < 3; k++)
            for (int c = 0; c < 3; c++) pos[k][c] = lamps[k].law.pos[c];
        g_dev->BeginScene();
        eng.Begin(g_dev);
        bool ok3 = scale && out3 && eng.RenderShadows(g_dev, pos, 3, 64.0f, err) && eng.Accumulate(g_dev, m, lamps.data(), 3, p, true, err);
        for (int k = 0; k < 3; k++)
            for (int c = 0; c < 3; c++) pos[k][c] = lamps[3 + k].law.pos[c];
        ok3 = ok3 && eng.RenderShadows(g_dev, pos, 3, 64.0f, err) && eng.Accumulate(g_dev, m, lamps.data() + 3, 3, p, false, err) && eng.Compose(g_dev, m, game, out3, p, err, scale);
        eng.End(g_dev);
        g_dev->EndScene();
        Check(ok3, "relight passes with the outdoor scale: " + err);
        const auto read = [&](IDirect3DTexture9* t, std::vector<uint32_t>& v) {
            v.assign(static_cast<size_t>(w) * h, 0);
            IDirect3DSurface9 *s = nullptr, *sys = nullptr;
            if (!t || FAILED(t->GetSurfaceLevel(0, &s))) return;
            g_dev->CreateOffscreenPlainSurface(w, h, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, nullptr);
            if (sys && SUCCEEDED(g_dev->GetRenderTargetData(s, sys))) {
                D3DLOCKED_RECT lr{};
                sys->LockRect(&lr, nullptr, D3DLOCK_READONLY);
                for (int y = 0; y < h; y++) std::memcpy(&v[static_cast<size_t>(y) * w], static_cast<BYTE*>(lr.pBits) + lr.Pitch * y, w * 4);
                sys->UnlockRect();
            }
            if (sys) sys->Release();
            s->Release();
        };
        std::vector<uint32_t> col, sc;
        read(out3, col);
        read(scale, sc);
        long same = 0, past = 0, within1 = 0, bad1 = 0;
        float worst = 0;
        for (size_t i = 0; i < tx.size(); i++) {
            const size_t at = static_cast<size_t>(verts[i].y) * w + static_cast<size_t>(verts[i].x);
            if (col[at] == got[at]) same++;
            const float k = std::exp2(RtOutdoorLamps::kScaleLog2 * static_cast<float>((sc[at] >> 16) & 0xFF) / 255.0f);
            if (tx[i].past1) {
                past++;
                const float want = std::min(tx[i].sumMax, 8.0f);
                worst = std::max(worst, std::fabs(k / want - 1.0f));
            } else {
                within1++;
                if (k != 1.0f) bad1++;
            }
        }
        long emptyBad = 0;
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++)
                if ((x * 7 + y * 3) % 5 == 0 && sc[static_cast<size_t>(y) * w + x] != 0) emptyBad++;
        std::printf("  the outdoor scale: colour unchanged on %ld of %zu texels; %ld past 1, the largest channel back within %.2f %%; %ld within 1 at x 1 (%ld not); "
                    "%ld texels no sample writes not at 0\n",
                    same, tx.size(), past, 100.0f * worst, within1, bad1, emptyBad);
        Check(same == static_cast<long>(tx.size()), "the outdoor scale leaves the relit colour as it was");
        Check(past > 0 && worst <= 0.005f, "the outdoor scale gives back the sum's largest channel past 1 (within half a percent)");
        Check(bad1 == 0 && emptyBad == 0, "the outdoor scale is 1 within 1 and where no texel lands");
        if (scale) scale->Release();
        if (out3) out3->Release();
    }
    {
        // the floor mask's "all" of the GPU lamps evaluated by the compose shader (one batch of all six lamps, dyn.x = 0)
        std::vector<RtLampGpu::TexelDyn> dyn2 = dyn;
        for (auto& d : dyn2) d.v[0] = 0.0f;
        RtLampGpu::Map m2 = m;
        m2.dyn = nullptr;
        RtLampGpu::Engine::MakeDyn(g_dev, dyn2.data(), m2.count, &m2.dyn);
        float pos6[RtLampGpu::kAtlasLamps][3];
        for (size_t k = 0; k < lamps.size(); k++)
            for (int c = 0; c < 3; c++) pos6[k][c] = lamps[k].law.pos[c];
        RtLampGpu::Params p2 = p;
        p2.maskAllGpu = true;
        g_dev->BeginScene();
        eng.Begin(g_dev);
        bool ok2 = eng.RenderShadows(g_dev, pos6, static_cast<int>(lamps.size()), 64.0f, err) &&
                   eng.Accumulate(g_dev, m2, lamps.data(), static_cast<int>(lamps.size()), p2, true, err) && eng.Compose(g_dev, m2, game, out, p2, err);
        eng.End(g_dev);
        g_dev->EndScene();
        std::vector<uint32_t> got2(static_cast<size_t>(w) * h);
        IDirect3DSurface9 *s = nullptr, *sys = nullptr;
        out->GetSurfaceLevel(0, &s);
        g_dev->CreateOffscreenPlainSurface(w, h, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, nullptr);
        if (SUCCEEDED(g_dev->GetRenderTargetData(s, sys))) {
            D3DLOCKED_RECT lr{};
            sys->LockRect(&lr, nullptr, D3DLOCK_READONLY);
            for (int y = 0; y < h; y++) std::memcpy(&got2[static_cast<size_t>(y) * w], static_cast<BYTE*>(lr.pBits) + lr.Pitch * y, w * 4);
            sys->UnlockRect();
        }
        sys->Release();
        s->Release();
        m2.dyn->Release();
        long masked = 0, exact = 0, worse = 0;
        int largest = 0;
        for (size_t i = 0; i < tx.size(); i++) {
            if (!tx[i].in.masked) continue;
            masked++;
            const int x = static_cast<int>(verts[i].x), y = static_cast<int>(verts[i].y);
            const int dg = OutdoorDeltaModel::MaxLevelDiff(got2[static_cast<size_t>(y) * w + x], tx[i].game);
            largest = std::max(largest, dg);
            if (dg == 0) exact++;
            if (dg > 1) worse++;
        }
        std::printf("  floor mask with the GPU lamps' \"all\" from the compose shader: %ld masked texels, %ld equal to the game's texel, %ld more than 1/255 off "
                    "(largest %d/255)\n", masked, exact, worse, largest);
        Check(ok2, "the compose shader's mask: " + err);
        Check(worse == 0 && exact >= static_cast<long>(masked * 0.99), "the compose shader's floor mask differs from the game's texel");
    }

    // ---- class-2 wall blocks: the GPU blur against the game's (outdoor_delta_model.h), bit for bit
    std::printf("\n[wall blur] class-2 blocks blurred on the GPU against the game's blur\n");
    std::vector<RtLampGpu::Block> blocks;
    std::vector<uint32_t> pre(static_cast<size_t>(w) * h, 0);
    std::vector<uint8_t> inBlock(static_cast<size_t>(w) * h, 0);
    for (int by = 0; by + 8 <= h; by += 9)
        for (int bx = 0; bx + 3 <= w; bx += 3 + (by % 7)) {
            const int bw = 1 + (bx * 13 + by) % 17, rows = 2 + (bx + by) % 7; // (blocks never overlap, as in the atlas)
            if (bx + bw > w || by + rows > h) continue;
            blocks.push_back({bx, by, bx + bw - 1, by + rows - 1});
            for (int y = by; y < by + rows; y++)
                for (int x = bx; x < bx + bw; x++) {
                    pre[static_cast<size_t>(y) * w + x] = (static_cast<uint32_t>(rng()) & 0x00FFFFFF) | (U(rng) < 0.1f ? 0x11000000u : 0u);
                    inBlock[static_cast<size_t>(y) * w + x] = 1;
                }
            bx += bw;
        }
    IDirect3DTexture9* preTex = nullptr;
    RtLampGpu::Engine::MakePreblur(g_dev, w, h, pre.data(), &preTex);
    // texels written over the blocks before the blur: every other texel of a block
    std::vector<RtLampGpu::TexelVertex> bv;
    std::vector<uint32_t> composed = pre;
    for (size_t i = 0; i < tx.size(); i++) {
        const int x = static_cast<int>(verts[i].x), y = static_cast<int>(verts[i].y);
        if (!inBlock[static_cast<size_t>(y) * w + x] || ((x + y) & 1)) continue;
        bv.push_back(verts[i]);
        composed[static_cast<size_t>(y) * w + x] = tx[i].model;
    }
    // the expected: each block blurred the game's way from its texels before the blur
    std::vector<uint32_t> expect = gamePx;
    for (const RtLampGpu::Block& b : blocks) {
        const int n = b.x1 - b.x0 + 1, rows = b.y1 - b.y0 + 1;
        std::vector<uint32_t> before(static_cast<size_t>(n) * rows), ext;
        for (int r = 0; r < rows; r++) std::memcpy(&before[static_cast<size_t>(r) * n], &composed[static_cast<size_t>(b.y0 + r) * w + b.x0], n * 4);
        ext = before;
        OutdoorDeltaModel::BlurBlock(ext.data(), before.data(), n, rows, false, false, 2);
        for (int r = 0; r < rows; r++) std::memcpy(&expect[static_cast<size_t>(b.y0 + r) * w + b.x0], &ext[static_cast<size_t>(r) * n], n * 4);
    }
    // the GPU: the same texels, composed before the blur (the sum from above is reused: same lamps, same texels)
    RtLampGpu::Map mb;
    mb.w = w, mb.h = h, mb.count = static_cast<UINT>(bv.size());
    std::vector<RtLampGpu::TexelDyn> bdyn;
    for (size_t i = 0; i < tx.size(); i++) {
        const int x = static_cast<int>(verts[i].x), y = static_cast<int>(verts[i].y);
        if (inBlock[static_cast<size_t>(y) * w + x] && !((x + y) & 1)) bdyn.push_back(dyn[i]);
    }
    RtLampGpu::Engine::MakeVb(g_dev, bv.data(), mb.count, &mb.vb);
    RtLampGpu::Engine::MakeDyn(g_dev, bdyn.data(), mb.count, &mb.dyn);
    mb.blur = true, mb.passes = 2, mb.blocks = blocks, mb.preblur = preTex;
    // (with the outdoor scale: drawn through scratch 3 and copied; the blurred colour must stay the game's)
    IDirect3DTexture9* scaleB = nullptr;
    g_dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &scaleB, nullptr);
    g_dev->BeginScene();
        eng.Begin(g_dev);
    ok = eng.Compose(g_dev, mb, game, out, p, err, scaleB);
    eng.End(g_dev);
        g_dev->EndScene();
    Check(ok, "blur passes: " + err);
    {
        IDirect3DSurface9 *s = nullptr, *sys = nullptr;
        out->GetSurfaceLevel(0, &s);
        g_dev->CreateOffscreenPlainSurface(w, h, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, nullptr);
        if (SUCCEEDED(g_dev->GetRenderTargetData(s, sys))) {
            D3DLOCKED_RECT lr{};
            sys->LockRect(&lr, nullptr, D3DLOCK_READONLY);
            for (int y = 0; y < h; y++) std::memcpy(&got[static_cast<size_t>(y) * w], static_cast<BYTE*>(lr.pBits) + lr.Pitch * y, w * 4);
            sys->UnlockRect();
        }
        sys->Release();
        s->Release();
    }
    long blockTexels = 0, blockBad = 0, outsideBad = 0;
    for (size_t i = 0; i < got.size(); i++) {
        if (inBlock[i]) {
            blockTexels++;
            if (got[i] != expect[i]) {
                if (blockBad < 5) std::printf("    block texel %zu,%zu: GPU %08X, game %08X\n", i % w, i / w, got[i], expect[i]);
                blockBad++;
            }
        } else if (got[i] != gamePx[i])
            outsideBad++;
    }
    std::printf("  %zu blocks, %ld block texels (%zu written before the blur): %ld differ from the game's blur; %ld texels outside the blocks changed\n", blocks.size(),
                blockTexels, bv.size(), blockBad, outsideBad);
    Check(blockBad == 0, "a blurred block texel differs from the game's blur");
    Check(outsideBad == 0, "a texel outside the blocks changed");
    {
        std::vector<uint32_t> sc(static_cast<size_t>(w) * h, 0);
        IDirect3DSurface9 *s = nullptr, *sys = nullptr;
        if (scaleB && SUCCEEDED(scaleB->GetSurfaceLevel(0, &s))) {
            g_dev->CreateOffscreenPlainSurface(w, h, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, nullptr);
            if (sys && SUCCEEDED(g_dev->GetRenderTargetData(s, sys))) {
                D3DLOCKED_RECT lr{};
                sys->LockRect(&lr, nullptr, D3DLOCK_READONLY);
                for (int y = 0; y < h; y++) std::memcpy(&sc[static_cast<size_t>(y) * w], static_cast<BYTE*>(lr.pBits) + lr.Pitch * y, w * 4);
                sys->UnlockRect();
            }
            if (sys) sys->Release();
            s->Release();
        }
        long inScaled = 0, outScaled = 0;
        for (size_t i = 0; i < sc.size(); i++) (inBlock[i] ? inScaled : outScaled) += sc[i] != 0 ? 1 : 0;
        std::printf("  the outdoor scale of the blurred atlas: %ld block texels past 1, %ld texels outside the blocks not at 0\n", inScaled, outScaled);
        Check(inScaled > 0 && outScaled == 0, "the blurred atlas's outdoor scale is written on its texels past 1 only");
    }
    if (scaleB) scaleB->Release();
    m.vb->Release(), m.dyn->Release(), mb.vb->Release(), mb.dyn->Release();
    preTex->Release(), game->Release(), out->Release();
    return true;
}

// ---------------------------------------------------------------- 4. GPU lamps for every room: raw and toned texels of several rooms
// One story map holding texels of room 0 (raw, two of its lamps listed twice), a roofed room whose pass adds its ambient and
// a roofed room whose pass does not, each with its own normalisation, ambient and lamps (the room table), bounce light on the
// roofed texels, tile-edge copies (a texel written again at another place with its source's dither), 20 lamps (two batches,
// the table written again between them), kept shadow tiles with no occluder (share 1). The reference is the game's own
// tail on the solve's sum (FUN_006a31d0, gpu_lamp_law.h IndoorTexel; the ramp as FUN_006ab210): the base is that sum less
// what the GPU lamps added (the split the capture makes), so base + the GPU lamps must give the game's texel back.
float RampRef(float y, float base, float K) { // FUN_006ab210: (1 - K) + 2 K (y - base) / 3, the x87 sum in double
    const float oneMinusK = static_cast<float>(1.0 - static_cast<double>(K));
    const float t = (y - base) * (1.0f / 3.0f);
    return static_cast<float>(static_cast<double>(K) * static_cast<double>(t) * 2.0 + static_cast<double>(oneMinusK));
}
bool RoomLampsCheck() {
    std::printf("\n[every room] raw and toned texels of three rooms, base + GPU lamps against the game's texel (no occluder: share 1)\n");
    std::mt19937 rng(4321);
    std::uniform_real_distribution<float> U(0.0f, 1.0f);
    const int w = 256, h = 128, nLamps = 20;
    std::vector<RtLampModel::Lamp> lamps;
    for (int i = 0; i < nLamps; i++) {
        LampDef d;
        d.pos[0] = 6.0f + 14.0f * U(rng), d.pos[1] = 0.4f + 2.4f * U(rng), d.pos[2] = 6.0f + 14.0f * U(rng);
        d.type = i % 5 == 2 ? 5 : i % 5 == 4 ? 4 : 3;
        d.colour[0] = 0.5f + 0.5f * U(rng), d.colour[1] = 0.5f + 0.5f * U(rng), d.colour[2] = 0.4f + 0.6f * U(rng);
        d.range = 20.0f + 40.0f * U(rng);
        d.scale = 4.9f, d.off = -0.32f;
        d.o1 = 0.7f, d.o2 = 0.5f;
        lamps.push_back(ToLamp(d, 1.0f));
    }
    // the rooms: 0 raw, 1 toned with its pass's ambient, 2 toned without; how often each lists each lamp; its values
    const int nRooms = 3;
    int mult[nRooms][nLamps] = {};
    for (int l = 0; l < nLamps; l++) {
        mult[0][l] = l < 6 ? (l < 2 ? 2 : 1) : 0;
        mult[1][l] = (l >= 4 && l < 14) ? 1 : 0;
        mult[2][l] = (l >= 10) ? 1 : 0;
    }
    const float norm[nRooms] = {1.0f, 1.37f, 0.62f}, amb[nRooms][3] = {{0, 0, 0}, {0.21f, 0.18f, 0.15f}, {0.05f, 0.07f, 0.09f}};
    const float k = 0.62f, K = 0.4f, rampBase = 0.05f, thr = 3.0f / 255.0f;
    const bool reach[nRooms] = {true, false, false};
    struct Tx {
        int x, y, room;
        float p[3], n[3];
        uint32_t game = 0, game2 = 0; // game2: lamp kOff switched off (the kept sum less its part)
    };
    const int kOff = 5;
    std::vector<Tx> tx;
    std::vector<RtLampGpu::TexelVertex> verts;
    std::vector<RtLampGpu::TexelDyn> dyn;
    long toned = 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            if ((x * 5 + y * 11) % 7 == 0) continue; // texels of no captured room: the game's map stays
            Tx t;
            t.x = x, t.y = y;
            t.room = x < w / 3 ? 0 : x < 2 * w / 3 ? 1 : 2;
            const bool wall = y < h / 2;
            t.p[0] = 6.0f + 14.0f * x / w, t.p[1] = wall ? 0.1f + 2.8f * (y / static_cast<float>(h / 2)) : 0.0f, t.p[2] = wall ? 5.95f : 6.0f + 14.0f * (y - h / 2) / (h / 2.0f);
            t.n[0] = 0, t.n[1] = wall ? 0.0f : 1.0f, t.n[2] = wall ? 1.0f : 0.0f;
            const bool tone = t.room > 0;
            float base[3];
            for (float& b : base) b = U(rng) < 0.3f ? 0.0f : 0.25f * U(rng) * U(rng);
            float bounce[3] = {};
            if (tone && U(rng) < 0.7f)
                for (float& b : bounce) b = 0.05f * U(rng);
            // what the solve added for the GPU lamps (each lamp's part of the sum, as the room's list holds it)
            const float dither = (y % 5 == 4) ? 0.0f : RoomMapDither::Offset(x, y) / 255.0f;
            const float ramp = RampRef(t.p[1], rampBase, K);
            for (int pass = 0; pass < 2; pass++) {
                float G[3] = {};
                for (int l = 0; l < nLamps; l++) {
                    if (!mult[t.room][l] || (pass == 1 && l == kOff)) continue;
                    float part[3] = {};
                    RtLampModel::Lamp L = lamps[l];
                    L.mult = static_cast<float>(mult[t.room][l]);
                    RtLampModel::Add(L, t.p, t.n, 1.0f, reach[t.room], thr, true, part);
                    for (int c = 0; c < 3; c++) G[c] += part[c];
                }
                float in[4];
                if (tone) {
                    float xs[3], rampAmb[3];
                    for (int c = 0; c < 3; c++) xs[c] = norm[t.room] * (base[c] + G[c] + bounce[c]), rampAmb[c] = (t.room == 1 ? amb[t.room][c] : 0.0f) * ramp;
                    GpuLampLaw::IndoorTexel(xs, rampAmb, ramp, k, in);
                    // (the game: alpha x ambient, alpha = ramp x weight; IndoorTexel takes ramp x ambient x weight: the same product)
                } else {
                    const float S[3] = {base[0] + G[0], base[1] + G[1], base[2] + G[2]};
                    OutdoorDeltaModel::PackInput(S, 0.0f, in);
                }
                float c4[4] = {std::min(in[0], 1.0f), std::min(in[1], 1.0f), std::min(in[2], 1.0f), std::min(in[3], 1.0f)};
                if (dither != 0.0f) OutdoorDeltaModel::Dither(c4, dither);
                (pass ? t.game2 : t.game) = OutdoorDeltaModel::PackModel(c4);
            }
            if (tone) toned++;
            RtLampGpu::TexelVertex v{};
            v.x = static_cast<float>(x), v.y = static_cast<float>(y), v.w = 1.0f;
            for (int c = 0; c < 3; c++) v.p[c] = t.p[c], v.n[c] = t.n[c], v.base[c] = base[c];
            v.p[3] = static_cast<float>((tone ? 4 : 0) + 16 * t.room);
            v.n[3] = tone ? ramp : 0.0f;
            v.base[3] = dither;
            v.mask[0] = t.room == 1 ? 1.0f : 0.0f;
            RtLampGpu::TexelDyn dv{};
            for (int c = 0; c < 3; c++) dv.v[c] = tone ? bounce[c] : 0.0f;
            if (!tone) dv.v[1] = -1.0f;
            tx.push_back(t);
            verts.push_back(v);
            dyn.push_back(dv);
        }
    // tile-edge copies: some texels written again one place to the right (their source's data and dither)
    std::vector<std::pair<size_t, int>> dups; // (source, destination x)
    for (size_t i = 0; i < tx.size(); i++)
        if (tx[i].y >= h / 2 && tx[i].x + 1 < w && (tx[i].x * 3 + tx[i].y) % 23 == 0 && (tx[i].x + 1) % (w / 3) != 0) dups.emplace_back(i, tx[i].x + 1);
    for (const auto& [src, dx] : dups) {
        RtLampGpu::TexelVertex v = verts[src];
        v.x = static_cast<float>(dx);
        verts.push_back(v);
        dyn.push_back(dyn[src]);
    }
    std::vector<uint32_t> expect(static_cast<size_t>(w) * h);
    for (size_t i = 0; i < expect.size(); i++) expect[i] = 0xFF000000u | static_cast<uint32_t>(i * 2654435761u >> 8);
    const std::vector<uint32_t> gamePx = expect;
    for (const Tx& t : tx) expect[static_cast<size_t>(t.y) * w + t.x] = t.game;
    for (const auto& [src, dx] : dups) expect[static_cast<size_t>(tx[src].y) * w + dx] = tx[src].game;
    RtLampGpu::Engine eng(RtLampGpu::PlainCalls());
    std::string err;
    RtLampProxy::Mesh empty;
    if (!eng.Init(g_dev, g_shaders, 64, err) || !eng.SetProxy(g_dev, empty, err) || !eng.EnsureTiles(g_dev, 32, err)) {
        Check(false, "engine: " + err);
        return false;
    }
    IDirect3DTexture9* game = nullptr;
    RtLampGpu::Engine::MakePreblur(g_dev, w, h, gamePx.data(), &game);
    IDirect3DTexture9* out = nullptr;
    g_dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &out, nullptr);
    RtLampGpu::Map m;
    m.w = w, m.h = h, m.count = static_cast<UINT>(verts.size());
    RtLampGpu::Engine::MakeVb(g_dev, verts.data(), m.count, &m.vb);
    RtLampGpu::Engine::MakeDyn(g_dev, dyn.data(), m.count, &m.dyn);
    RtLampGpu::Params p;
    p.thr = thr, p.fade = true, p.reachOn = true, p.mask = false;
    std::vector<float> table(static_cast<size_t>(nRooms) * RtLampGpu::Engine::kTableCols * 4, 0.0f);
    for (int r = 0; r < nRooms; r++) {
        float* row = &table[(static_cast<size_t>(r) * RtLampGpu::Engine::kTableCols + 16) * 4];
        row[0] = norm[r], row[1] = k;
        for (int c = 0; c < 3; c++) row[4 + c] = amb[r][c];
    }
    g_dev->BeginScene();
    eng.Begin(g_dev);
    bool ok = true;
    for (int b = 0; ok && b < nLamps; b += RtLampGpu::kAtlasLamps) {
        const int n = std::min(RtLampGpu::kAtlasLamps, nLamps - b);
        int tiles[RtLampGpu::kAtlasLamps] = {};
        for (int i = 0; i < n && ok; i++) {
            tiles[i] = 31 - ((b + i) % 32); // (kept tiles in any order)
            ok = eng.RenderShadowTile(g_dev, lamps[b + i].law.pos, tiles[i], 64.0f, err);
        }
        for (int r = 0; r < nRooms; r++)
            for (int i = 0; i < RtLampGpu::kAtlasLamps; i++) {
                float* cell = &table[(static_cast<size_t>(r) * RtLampGpu::Engine::kTableCols + i) * 4];
                cell[0] = i < n ? static_cast<float>(mult[r][b + i]) : 0.0f;
                cell[1] = i < n && reach[r] ? 1.0f : 0.0f;
            }
        ok = ok && eng.SetTable(g_dev, table.data(), nRooms, err) && eng.AccumulateRlg(g_dev, m, lamps.data() + b, tiles, n, p, b == 0, err);
    }
    IDirect3DTexture9* kept = nullptr;
    g_dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A32B32G32R32F, D3DPOOL_DEFAULT, &kept, nullptr);
    ok = ok && eng.StoreAcc(g_dev, kept, w, h, err) && eng.ComposeRlg(g_dev, m, game, out, p, err);
    eng.End(g_dev);
    g_dev->EndScene();
    Check(ok, "every room: relight passes: " + err);
    std::vector<uint32_t> got;
    Check(RtLampGpu::Engine::ReadTexture(g_dev, out, w, h, got), "every room: read back");
    long exact[2] = {}, within2[2] = {}, n[2] = {}, untouchedBad = 0, dupBad = 0;
    int largest[2] = {};
    for (const Tx& t : tx) {
        const int kind = t.room > 0 ? 1 : 0;
        const size_t at = static_cast<size_t>(t.y) * w + t.x;
        if (std::any_of(dups.begin(), dups.end(), [&](const auto& d) { return tx[d.first].y == t.y && d.second == t.x; })) continue; // (a copy landed there)
        const int dg = OutdoorDeltaModel::MaxLevelDiff(got[at], t.game);
        n[kind]++;
        exact[kind] += dg == 0 ? 1 : 0;
        within2[kind] += dg <= 2 ? 1 : 0;
        if (dg > largest[kind] && dg > 1 && largest[kind] <= 1) std::printf("    texel %d,%d room %d: GPU %08X, game %08X\n", t.x, t.y, t.room, got[at], t.game);
        largest[kind] = std::max(largest[kind], dg);
    }
    for (const auto& [src, dx] : dups)
        if (OutdoorDeltaModel::MaxLevelDiff(got[static_cast<size_t>(tx[src].y) * w + dx], tx[src].game) > 2) dupBad++;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            if ((x * 5 + y * 11) % 7 == 0 && got[static_cast<size_t>(y) * w + x] != expect[static_cast<size_t>(y) * w + x]) untouchedBad++;
    for (int kind = 0; kind < 2; kind++)
        std::printf("  %s texels: %ld, equal to the game's texel %.2f%%, within 2/255 %.2f%% (largest %d/255)\n", kind ? "toned (roofed rooms)" : "raw (room 0)", n[kind],
                    n[kind] ? 100.0 * exact[kind] / n[kind] : 100.0, n[kind] ? 100.0 * within2[kind] / n[kind] : 100.0, largest[kind]);
    std::printf("  %zu tile-edge copies (%ld more than 2/255 off their source's texel); texels of no captured room changed: %ld\n", dups.size(), dupBad, untouchedBad);
    Check(toned > 0 && n[1] > 0, "every room: no toned texel compared");
    for (int kind = 0; kind < 2; kind++) Check(n[kind] == 0 || within2[kind] >= static_cast<long>(0.99 * n[kind]), std::format("every room: fewer than 99% of {} texels within 2/255", kind ? "toned" : "raw"));
    Check(dupBad == 0, "every room: a tile-edge copy differs from its source's texel");
    Check(untouchedBad == 0, "every room: a texel of no captured room changed");
    // a switch: lamp kOff taken out of the kept sum (its part with -mult), the map composed again
    {
        g_dev->BeginScene();
        eng.Begin(g_dev);
        int tile = 31 - (kOff % 32);
        for (int r = 0; r < nRooms; r++)
            for (int i = 0; i < RtLampGpu::kAtlasLamps; i++) {
                float* cell = &table[(static_cast<size_t>(r) * RtLampGpu::Engine::kTableCols + i) * 4];
                cell[0] = i == 0 ? -static_cast<float>(mult[r][kOff]) : 0.0f;
                cell[1] = i == 0 && reach[r] ? 1.0f : 0.0f;
            }
        bool ok2 = eng.SeedAcc(g_dev, kept, w, h, err) && eng.SetTable(g_dev, table.data(), nRooms, err) &&
                   eng.AccumulateRlg(g_dev, m, lamps.data() + kOff, &tile, 1, p, false, err) && eng.ComposeRlg(g_dev, m, game, out, p, err);
        eng.End(g_dev);
        g_dev->EndScene();
        Check(ok2, "every room: the switched lamp's pass: " + err);
        std::vector<uint32_t> got2;
        RtLampGpu::Engine::ReadTexture(g_dev, out, w, h, got2);
        long n2 = 0, within = 0, exact2 = 0;
        int large = 0;
        for (const Tx& t : tx) {
            if (std::any_of(dups.begin(), dups.end(), [&](const auto& d) { return tx[d.first].y == t.y && d.second == t.x; })) continue;
            const int dg = OutdoorDeltaModel::MaxLevelDiff(got2[static_cast<size_t>(t.y) * w + t.x], t.game2);
            n2++;
            within += dg <= 2 ? 1 : 0;
            exact2 += dg == 0 ? 1 : 0;
            large = std::max(large, dg);
        }
        std::printf("  lamp %d switched off by taking its part out of the kept sum: %ld texels, equal to the game's texel without it %.2f%%, within 2/255 %.2f%% (largest %d/255)\n",
                    kOff, n2, n2 ? 100.0 * exact2 / n2 : 100.0, n2 ? 100.0 * within / n2 : 100.0, large);
        Check(n2 > 0 && within >= static_cast<long>(0.99 * n2), "every room: the switch through the kept sum: fewer than 99% within 2/255");
    }
    kept->Release();
    m.vb->Release(), m.dyn->Release();
    game->Release(), out->Release();
    return true;
}

#ifdef RT_GRID_EXTRACT
// ---------------------------------------------------------------- 2. the room solve's grid walk against the reference
// (the story above: the ceilings are taken down to its floor where lower, as BuildStoryGrid and the occluders do)
Grid::StoryGrid ToGrid(const RtLampProxy::Story& s, const RtLampProxy::Story* above = nullptr) {
    Grid::StoryGrid g;
    g.story = s.story, g.w = s.w, g.h = s.h;
    g.zl.assign(static_cast<size_t>(s.h + 1) * s.w, -1);
    g.xl.assign(static_cast<size_t>(s.w + 1) * s.h, -1);
    g.dg.assign(2 * static_cast<size_t>(s.w) * s.h, -1);
    g.flags.assign(static_cast<size_t>(s.w) * s.h, 0);
    g.ceil.assign(static_cast<size_t>(s.w) * s.h, 1e9f);
    g.quad.assign(4 * static_cast<size_t>(s.w) * s.h, 0);
    for (const RtLampProxy::Edge& e : s.edges) {
        Grid::GridEdge ge;
        ge.foot = e.foot, ge.base = e.base, ge.topL = e.topL, ge.topM = e.topM, ge.topR = e.topR;
        if (e.open) {
            Grid::GridOpening o;
            o.sill = e.o.sill, o.u0 = e.o.u0, o.u1 = e.o.u1;
            for (int k = 0; k < RtLampProxy::kProfile; k++) o.head[k] = e.o.head[k];
            g.openings.push_back(o);
            ge.open = static_cast<uint16_t>(g.openings.size());
        }
        const int32_t idx = static_cast<int32_t>(g.edges.size());
        g.edges.push_back(ge);
        g.minFoot = std::min(g.minFoot, e.foot);
        const int x0 = static_cast<int>(std::floor(std::min(e.x0, e.x1))), z0 = static_cast<int>(std::floor(std::min(e.z0, e.z1)));
        if (e.z0 == e.z1) g.zl[static_cast<size_t>(static_cast<int>(e.z0)) * s.w + x0] = idx;
        else if (e.x0 == e.x1) g.xl[static_cast<size_t>(z0) * (s.w + 1) + static_cast<int>(e.x0)] = idx;
        else {
            const bool d0 = (e.x1 - e.x0) * (e.z1 - e.z0) > 0;
            g.dg[2 * (static_cast<size_t>(z0) * s.w + x0) + (d0 ? 0 : 1)] = idx;
            g.flags[static_cast<size_t>(z0) * s.w + x0] |= static_cast<uint8_t>(d0 ? 1 : 2);
        }
    }
    std::vector<float> ceil;
    RtLampProxy::CeilingsUnderAbove(s, above, ceil);
    for (size_t t = 0; t < g.flags.size(); t++) {
        g.flags[t] |= static_cast<uint8_t>(s.roofed[t] << 4);
        if (ceil[t] < RtLampProxy::kNoFloor * 0.5f) g.ceil[t] = ceil[t];
    }
    g.plane = s.plane < RtLampProxy::kNoFloor * 0.5f ? s.plane : -1e30f;
    return g;
}
void GridCompare(const Scene& sc, float radius) {
    // story 0's own lamps and receivers (the walk of its grid and the story above's, as the room solve's yard test does)
    if (sc.lot.stories.empty()) return;
    const RtLampProxy::Story* s0 = nullptr;
    const RtLampProxy::Story* s1 = nullptr;
    for (const auto& s : sc.lot.stories) {
        if (s.story == 0) s0 = &s;
        if (s.story == 1) s1 = &s;
    }
    if (!s0) return;
    Grid::StoryGrid g0 = ToGrid(*s0, s1), g1 = s1 ? ToGrid(*s1) : Grid::StoryGrid{}; // (as the reference below: story 0 under story 1, story 1 alone)
    RtLampProxy::Lot one;
    one.stories.push_back(*s0);
    if (s1) {
        RtLampProxy::Story walls = *s1; // the story above's walls and roofs, no slabs (the walk has none)
        walls.placed.assign(walls.placed.size(), 0);
        one.stories.push_back(walls);
    }
    RtLampProxy::Mesh mesh;
    RtLampProxy::Build(one, mesh);
    Penumbra::SetRadius(radius);
    Grid::t_mapAA = 0.0f;
    std::vector<uint32_t> stamp;
    uint32_t mark = 0;
    long n = 0, within2 = 0, within05 = 0;
    double sumAbs = 0;
    for (const LampDef& ld : sc.lamps) {
        if (ld.pos[1] > 3.0f + (s0->edges.empty() ? 0.0f : s0->edges[0].foot)) continue; // story 0's lamps
        const RtLampModel::Lamp L = ToLamp(ld);
        for (const Receiver& r : sc.recv) {
            if (r.story != 0) continue;
            float law[3];
            GpuLampLaw::Eval(L.law, r.p, r.n, GpuLampLaw::WallReach(r.n, true), law);
            const float lawMax = std::min(std::max({law[0], law[1], law[2]}), 1.0f);
            if (lawMax * 255.0f < 1.0f) continue;
            Grid::GridWhy why{};
            float pass = Grid::GridPassImpl(g0, ld.pos, r.p, why);
            if (pass > 0.0f && s1 && std::max(ld.pos[1], r.p[1]) > g1.minFoot - Grid::kGridSoft) pass *= Grid::GridPassImpl(g1, ld.pos, r.p, why, g1.plane);
            float ref = RtLampProxy::Visibility(mesh, ld.pos, radius, r.p, 8, &stamp, &mark);
            if (ref > 0.0f && ref < 1.0f) ref = RtLampProxy::Visibility(mesh, ld.pos, radius, r.p, RtLampProxy::kRefSamples, &stamp, &mark);
            const float d = std::fabs(pass - ref);
            n++;
            sumAbs += d;
            if (d * lawMax * 255.0f <= 2.0f) within2++;
            if (d <= 0.05f) within05++;
        }
    }
    std::printf("  the room solve's grid walk (GridPassImpl, story 0 + the walls above) against the reference on the same walls and roofs: %ld texel x lamp pairs, "
                "within 2/255 %.2f%%, share within 0.05 %.2f%%, mean |share diff| %.4f\n",
                n, n ? 100.0 * within2 / n : 100.0, n ? 100.0 * within05 / n : 100.0, n ? sumAbs / n : 0.0);
}
#endif

} // namespace

// ---------------------------------------------------------------- the floor mask's under-floor share while a lamp is dragged
// rt_outdoor_lamps.cpp keeps the share's two sums in double and moves one lamp's stamp out and in (UnderDelta); the exact
// share (UnderShare's float sums) comes back from a worker once the lamp stays put. Here: 20 moves of one of 40 lamps
// against UnderShare, in the share and in the 8-bit level it gives
void UnderDeltaCheck() {
    std::printf("\n[under-floor share] the dragged lamp's stamp out of and into the sums against UnderShare\n");
    std::mt19937 rng(77);
    std::uniform_real_distribution<float> U(0.0f, 1.0f);
    std::vector<RtLampModel::BakeLamp> lamps(40);
    for (auto& L : lamps) {
        L.head[0] = 40 * U(rng), L.head[1] = 0.3f + 9.0f * U(rng), L.head[2] = 40 * U(rng);
        L.range = 6 + 10 * U(rng), L.intensity = 0.5f + U(rng), L.colour = 0.5f + 2.5f * U(rng);
    }
    const float base = 0.0f, eps = 0.02f;
    std::vector<std::array<float, 3>> pts(20000);
    for (auto& p : pts) p = {40 * U(rng), 3.0f * (1 + static_cast<int>(3 * U(rng))), 40 * U(rng)};
    std::vector<double> A(pts.size(), 0.0), Us(pts.size(), 0.0);
    for (size_t i = 0; i < pts.size(); i++)
        for (const auto& L : lamps) {
            float v;
            bool u;
            if (RtLampModel::BakeStamp(L, pts[i].data(), base, eps, v, u)) A[i] += v, Us[i] += u ? v : 0.0;
        }
    double worst = 0;
    long levels = 0, n = 0;
    for (int move = 0; move < 20; move++) {
        RtLampModel::BakeLamp& L = lamps[move % 3];
        const RtLampModel::BakeLamp old = L;
        L.head[0] += 0.5f, L.head[1] += (move & 1) ? 0.4f : -0.4f;
        for (size_t i = 0; i < pts.size(); i++) {
            float v;
            bool u;
            if (RtLampModel::BakeStamp(old, pts[i].data(), base, eps, v, u)) A[i] -= v, Us[i] -= u ? v : 0.0;
            if (RtLampModel::BakeStamp(L, pts[i].data(), base, eps, v, u)) A[i] += v, Us[i] += u ? v : 0.0;
            const float inc = A[i] > 1e-4 ? static_cast<float>(std::clamp(Us[i] / A[i], 0.0, 1.0)) : -1.0f;
            const float ex = RtLampModel::UnderShare(lamps.data(), static_cast<int>(lamps.size()), pts[i].data(), base, eps);
            n++;
            worst = std::max(worst, static_cast<double>(std::fabs(inc - ex)));
            if (std::lround(std::max(inc, 0.0f) * 255.0f) != std::lround(std::max(ex, 0.0f) * 255.0f)) levels++;
        }
    }
    std::printf("  %ld shares after a move: largest difference %.2e, %ld of them a different 8-bit level\n", n, worst, levels);
    Check(worst < 1e-4, "the dragged lamp's under-floor share drifts from UnderShare");
}

// UnderDelta stamps 4 texels at a time (BakeStamp4): every lane must be BakeStamp's, bit for bit (lamps on the lawn, below the
// base, right over a texel where the stamp is clamped at 1, and far away)
void BakeStamp4Check() {
    std::printf("\n[under-floor share] BakeStamp4 against BakeStamp, bit for bit\n");
    std::mt19937 rng(91);
    std::uniform_real_distribution<float> U(0.0f, 1.0f);
    long lanes = 0, differ = 0, stamped = 0;
    for (int t = 0; t < 4000; t++) {
        RtLampModel::BakeLamp L;
        L.head[0] = 60 * U(rng) - 10, L.head[1] = -1.0f + 10.0f * U(rng), L.head[2] = 60 * U(rng) - 10;
        L.range = 1 + 40 * U(rng), L.intensity = 0.1f + 3 * U(rng), L.colour = 3 * U(rng);
        L.liftable = (t & 1) ? 1u : 0u;
        const float base = (t % 7 == 0) ? 2.0f : 0.0f, eps = 0.02f;
        float sx[4], sy[4], sz[4];
        for (int k = 0; k < 4; k++) {
            const bool atLamp = (t + k) % 5 == 0; // (right under the lamp: the stamp past 1)
            sx[k] = atLamp ? L.head[0] + 0.01f * U(rng) : 50 * U(rng);
            sy[k] = 3.0f * static_cast<float>(static_cast<int>(3 * U(rng)));
            sz[k] = atLamp ? L.head[2] + 0.01f * U(rng) : 50 * U(rng);
        }
        float v4[4];
        bool u4[4], ok4[4];
        RtLampModel::BakeStamp4(L, sx, sy, sz, base, eps, v4, u4, ok4);
        for (int k = 0; k < 4; k++) {
            const float s[3] = {sx[k], sy[k], sz[k]};
            float v = 0;
            bool u = false;
            const bool ok = RtLampModel::BakeStamp(L, s, base, eps, v, u);
            lanes++;
            stamped += ok ? 1 : 0;
            if (ok != ok4[k] || (ok && (std::memcmp(&v, &v4[k], sizeof v) != 0 || u != u4[k]))) differ++;
        }
    }
    std::printf("  %ld lanes (%ld stamped): %ld differ\n", lanes, stamped, differ);
    Check(differ == 0 && stamped > 0, "BakeStamp4 differs from BakeStamp");
}


// ---------------------------------------------------------------- --probe: where every lamp head sits against the occluders
// (GPU lamps for every room: a head on the far side of a nearby plane, a ceiling lamp above the proxy's ceiling, puts the
// whole room in its shadow on the GPU while the solves see it)
bool InTri2(const RtLampProxy::Tri& t, float x, float z) {
    const auto side = [](float ax, float az, float bx, float bz, float px, float pz) { return (bx - ax) * (pz - az) - (bz - az) * (px - ax); };
    const float d0 = side(t.v[0][0], t.v[0][2], t.v[1][0], t.v[1][2], x, z), d1 = side(t.v[1][0], t.v[1][2], t.v[2][0], t.v[2][2], x, z),
                d2 = side(t.v[2][0], t.v[2][2], t.v[0][0], t.v[0][2], x, z);
    return (d0 >= 0 && d1 >= 0 && d2 >= 0) || (d0 <= 0 && d1 <= 0 && d2 <= 0);
}
void Probe(const Scene& sc) {
    RtLampProxy::Mesh mesh;
    RtLampProxy::Build(sc.lot, mesh, true);
    std::printf("\n[probe %s] %zu triangles, %zu lamps\n", sc.name.c_str(), mesh.tris.size(), sc.lamps.size());
    for (const RtLampProxy::Story& st : sc.lot.stories) { // each story's wall bases, wall tops and tile floors (medians)
        std::vector<float> base, top, fl;
        for (const RtLampProxy::Edge& e : st.edges)
            if (e.topM > e.base + 1.0f) base.push_back(e.base), top.push_back(e.topM);
        for (size_t t = 0; t < st.floorY.size(); t++)
            if (st.floorY[t] < RtLampProxy::kNoFloor * 0.5f) fl.push_back(st.floorY[t]);
        const auto med = [](std::vector<float>& v) {
            if (v.empty()) return -999.0f;
            std::nth_element(v.begin(), v.begin() + static_cast<std::ptrdiff_t>(v.size() / 2), v.end());
            return v[v.size() / 2];
        };
        const size_t nb = base.size(), nf = fl.size();
        const float mb = med(base), mt = med(top), mf = med(fl);
        std::printf("  story %d: %zu walls, median base %.2f, median top %.2f; %zu floor tiles, median floor %.2f; plane %.2f\n", st.story, nb, mb, mt, nf, mf,
                    st.plane < RtLampProxy::kNoFloor * 0.5f ? st.plane : -999.0f);
        // this story's floors against the ceilings of the story below where that one is roofed, and the roofed tiles' floors
        // against this story's own wall bases
        const RtLampProxy::Story* down = nullptr;
        for (const RtLampProxy::Story& d : sc.lot.stories)
            if (d.story == st.story - 1) down = &d;
        std::map<int, int> hist, own;
        for (size_t t = 0; t < st.floorY.size(); t++) {
            if (!(st.floorY[t] < RtLampProxy::kNoFloor * 0.5f)) continue;
            if (st.roofed.size() > t && st.roofed[t]) own[static_cast<int>(std::lround((st.floorY[t] - mb) * 4.0f))]++;
            if (!down || down->w != st.w || down->h != st.h || !(down->roofed.size() > t && down->roofed[t]) || !(down->ceil[t] < RtLampProxy::kNoFloor * 0.5f)) continue;
            hist[static_cast<int>(std::lround((st.floorY[t] - down->ceil[t]) * 4.0f))]++;
        }
        std::string h1, h2;
        for (const auto& [k, n] : hist) h1 += std::format(" {:+.2f}:{}", k / 4.0f, n);
        for (const auto& [k, n] : own) h2 += std::format(" {:+.2f}:{}", k / 4.0f, n);
        if (!h1.empty()) std::printf("    floors - the ceilings below (m: tiles):%s\n", h1.c_str());
        if (!h2.empty()) std::printf("    roofed tiles' floors - this story's wall base (m: tiles):%s\n", h2.c_str());
    }
    std::map<std::string, int> tally;
    for (const LampDef& d : sc.lamps) {
        // the horizontal planes over and under the head (within 0.5 m), and the nearest wall
        float above = 1e9f, below = 1e9f;
        int aboveKind = -1, aboveStory = 0, belowKind = -1, belowStory = 0;
        float wallD = 1e9f;
        for (size_t i = 0; i < mesh.tris.size(); i++) {
            const RtLampProxy::Tri& t = mesh.tris[i];
            const float y0 = std::min({t.v[0][1], t.v[1][1], t.v[2][1]}), y1 = std::max({t.v[0][1], t.v[1][1], t.v[2][1]});
            if (mesh.kind[i] == RtLampProxy::Mesh::kWall) {
                if (d.pos[1] < y0 - 0.3f || d.pos[1] > y1 + 0.3f) continue;
                // distance in xz to the wall's segment
                const float ax = t.v[0][0], az = t.v[0][2];
                float bx = t.v[1][0], bz = t.v[1][2];
                if (std::fabs(bx - ax) + std::fabs(bz - az) < 1e-4f) bx = t.v[2][0], bz = t.v[2][2];
                const float dx = bx - ax, dz = bz - az, l2 = dx * dx + dz * dz;
                if (l2 < 1e-8f) continue;
                const float u = std::clamp(((d.pos[0] - ax) * dx + (d.pos[2] - az) * dz) / l2, 0.0f, 1.0f);
                const float px = ax + dx * u - d.pos[0], pz = az + dz * u - d.pos[2];
                wallD = std::min(wallD, std::sqrt(px * px + pz * pz));
                continue;
            }
            if (y1 - y0 > 1e-3f || !InTri2(t, d.pos[0], d.pos[2])) continue;
            const float dy = y0 - d.pos[1];
            if (dy >= 0.0f && dy < above) above = dy, aboveKind = mesh.kind[i], aboveStory = mesh.story[i];
            if (dy < 0.0f && -dy < below) below = -dy, belowKind = mesh.kind[i], belowStory = mesh.story[i];
        }
        static constexpr const char* kK[4] = {"wall", "roof", "slab", "shape"};
        std::string what;
        if (above < 0.16f) what = std::format("a {} of story {} {:.3f} m over the head", kK[aboveKind & 3], aboveStory, above);
        if (below < 0.16f) what += std::format("{}a {} of story {} {:.3f} m under the head", what.empty() ? "" : "; ", kK[belowKind & 3], belowStory, below);
        if (wallD < 0.16f) what += std::format("{}a wall {:.3f} m away", what.empty() ? "" : "; ", wallD);
        const std::string cls = above < 0.16f ? "ceiling within the ball" : below < 0.16f ? "plane under the head within the ball" : wallD < 0.16f ? "wall within the ball" : "clear";
        tally[cls]++;
        if (!what.empty() || g_debug)
            std::printf("  %s type %d room %d at lot (%.2f %.2f %.2f): %s; nearest plane over %.2f m (%s %d), under %.2f m (%s %d)\n", d.name.c_str(), d.type, d.room, d.pos[0],
                        d.pos[1], d.pos[2], what.empty() ? "nothing within the ball" : what.c_str(), above, aboveKind >= 0 ? kK[aboveKind & 3] : "-", aboveStory, below,
                        belowKind >= 0 ? kK[belowKind & 3] : "-", belowStory);
    }
    for (const auto& [k, n] : tally) std::printf("  %s: %d lamps\n", k.c_str(), n);
}
// ---------------------------------------------------------------- 5. GPU room lamps: kept room solves (RelightKept)
// A synthetic kept room on two 64 x 64 maps, drawn by the production passes (AccKeptPS, ComposeKeptPS, CopyAtPS, the wall
// blur, BlockKeptPS) over a game map, against the CPU: base + RtLampModel::KeptAdd of every part, RtLampModel::KeptTexel
// (roofed and roofless texels, the ramp and ambient read back from the pack input with IndoorSplit, the alpha floor, the
// dither), the game's pack, BlurBlock with the rows beyond the blocks' edges, the tile-edge copies and the texels a copy of an
// unkept texel overwrote (the game's). The floor's own map is smaller than the working map the solve wrote (its top-left
// corner; copies from past the corner land inside). Every texel of the own map is compared; the others must stay the game's.
bool KeptGpuCheck() {
    std::printf("\n[GPU room lamps] kept texels relit on the GPU against KeptAdd + KeptTexel + the pack, the blur and the copies on the CPU\n");
    std::mt19937 rng(9001);
    std::uniform_real_distribution<float> U(0.0f, 1.0f);
    const int w = 64, h = 64, nLamps = 5;
    const float thr = 3.0f / 255.0f, k = 0.62f, floorA = 5.0f / 255.0f;
    GpuLampLaw::Lamp laws[nLamps];
    for (GpuLampLaw::Lamp& L : laws) {
        L.pos[0] = 32.0f * U(rng), L.pos[1] = 1.0f + 2.0f * U(rng), L.pos[2] = 32.0f * U(rng);
        L.W = 2.0f + 6.0f * U(rng);
        for (float& c : L.colour) c = 0.4f + 0.6f * U(rng);
        GpuLampLaw::NoCone(L);
    }
    { // (lamp 3: a TubeLight, its segment 1.3 m along x and up: KeptLaw's TubeLaw)
        const float s[3] = {1.2f, 0.5f, 0.0f};
        GpuLampLaw::TubeShape(laws[3], s, 1.3f);
    }
    { // (lamp 1: a RectangleAreaLight 1.2 x 0.8 m facing down: KeptLaw's AreaLaw)
        const float u[3] = {1.0f, 0.0f, 0.0f}, v[3] = {0.0f, 0.0f, 1.0f}, nrm[3] = {0.0f, 1.0f, 0.0f};
        GpuLampLaw::AreaShape(laws[1], u, v, nrm, 0.6f, 0.4f, false);
    }
    const auto packCpu = [](float c[4]) { return OutdoorDeltaModel::PackModel(c); };
    long n = 0, within1 = 0, maxDiff = 0, untouchedBad = 0;
    for (const int m : {1, 0}) { // a floor (copies, restores) and a class-2 wall atlas (blocks before the blur)
        RtLampGpu::KeptMapData d;
        d.w = w, d.h = h, d.thr = thr, d.blur = m == 0;
        const int ow = m == 1 ? 48 : w, oh = m == 1 ? 40 : h; // a floor's own map smaller than the working map
        std::vector<uint32_t> gamePx(static_cast<size_t>(w) * h);
        for (uint32_t& v : gamePx) v = static_cast<uint32_t>(rng());
        // kept texels at distinct places (every other one on the floor; the wall texels inside blocks)
        struct Tx {
            int x, y;
            float pos[3], nrm[3], base[3], S[3], p[4], norm, dither;
            bool roofed, floorOn;
            uint32_t cpu;
        };
        std::vector<Tx> tx;
        struct Blk {
            int x0, y0, x1, y1;
            bool above, below;
        };
        const std::vector<Blk> blocks = m == 0 ? std::vector<Blk>{{2, 4, 21, 10, true, true}, {30, 20, 45, 32, false, false}, {5, 40, 15, 50, true, false}} : std::vector<Blk>{};
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) {
                bool in = m == 1 ? (x + y) % 2 == 0 : false;
                for (const Blk& b : blocks) in = in || (x >= b.x0 && x <= b.x1 && y >= b.y0 && y <= b.y1 && (x * 7 + y * 3) % 5 != 0);
                if (!in) continue;
                Tx t{};
                t.x = x, t.y = y;
                t.pos[0] = x * 0.5f, t.pos[1] = m == 0 ? 0.2f + 0.04f * y : 0.0f, t.pos[2] = y * 0.5f;
                t.nrm[0] = 0, t.nrm[1] = m == 0 ? 0.0f : 1.0f, t.nrm[2] = m == 0 ? 1.0f : 0.0f;
                for (float& b : t.base) b = 0.15f * U(rng) * U(rng);
                t.roofed = (x / 8 + y / 8) % 3 != 0;
                t.floorOn = U(rng) < 0.8f;
                t.norm = 0.6f + U(rng);
                t.dither = (y % 4 == 3) ? 0.0f : RoomMapDither::Offset(x, y) / 255.0f;
                tx.push_back(t);
            }
        const uint32_t count = static_cast<uint32_t>(tx.size());
        d.accW = static_cast<int>(std::min<uint32_t>(count, 1024)), d.accH = static_cast<int>((count + d.accW - 1) / d.accW);
        // (stage 3) lamp 4 moved to (6, 2.6, 18): a wall across x = 10 (its hard and cull segments) and a disc of radius 1 at y = 2.3
        // under it, for the game's 2D and 3D tests of the move worker (room_lamp_move.h)
        laws[4].pos[0] = 6.0f, laws[4].pos[1] = 2.6f, laws[4].pos[2] = 18.0f;
        RoomLightTests::Room mvRoom;
        {
            RoomLightTests::Wall wall{};
            const float seg[6] = {10.0f, 0.0f, 0.0f, 0.0f, 0.0f, 32.0f}; // P, then E 0x20 bytes on
            for (const int base : {0x40, 0xA0}) wall.f[base / 4] = seg[0], wall.f[base / 4 + 1] = seg[1], wall.f[base / 4 + 2] = seg[2], wall.f[base / 4 + 8] = seg[3], wall.f[base / 4 + 9] = seg[4], wall.f[base / 4 + 10] = seg[5];
            mvRoom.walls.push_back(wall);
            mvRoom.idx2d = {0};
            RoomLightTests::Record rec{{6.0f, 2.3f, 18.0f}, 4.0f, {}};
            RoomLightTests::Blocker disc{};
            disc.vt = RoomLightTests::kDiscVt;
            disc.f[0x20 / 4] = 6.0f, disc.f[0x20 / 4 + 1] = 2.3f, disc.f[0x20 / 4 + 2] = 18.0f, disc.f[0x40 / 4 + 1] = 1.0f, disc.f[0x70 / 4] = 1.0f;
            rec.b.push_back(disc);
            mvRoom.rec.push_back(rec);
        }
        long mvParts = 0, mvShaded = 0, mvDark = 0;
        int mvUnknown = 0;
        // the parts, by lamp: the solve's sum S = base + the parts with the lamps as they were (the CPU's KeptAdd)
        std::vector<float> R(static_cast<size_t>(count) * 3);
        for (uint32_t i = 0; i < count; i++)
            for (int q = 0; q < 3; q++) R[i * 3 + q] = tx[i].base[q];
        for (int l = 0; l < nLamps; l++) {
            const UINT first = static_cast<UINT>(d.parts.size());
            for (uint32_t i = 0; i < count; i++) {
                if ((i + static_cast<uint32_t>(l)) % 3 == 0) continue;
                float A = 0.3f + 1.5f * U(rng), B = U(rng) < 0.2f ? 0.0f : U(rng), ts = U(rng) < 0.3f ? 0.5f + 0.5f * U(rng) : 1.0f;
                int mult = U(rng) < 0.2f ? 2 : 1, bits = (U(rng) < 0.3f ? 1 : 0) | (U(rng) < 0.8f ? 4 : 0);
                if (l == 4) { // (stage 3) a lamp moved: its part from the move worker's code at its new place, as OutdoorDelta keeps it
                    const std::vector<int> list = RoomLightTests::wallCull(mvRoom, {tx[i].pos[0], tx[i].pos[1], tx[i].pos[2]}, {laws[4].pos[0], laws[4].pos[1], laws[4].pos[2]});
                    RoomLampMove::PartAt(mvRoom, true, list, true, true, laws[4].pos, tx[i].pos, tx[i].nrm, A, B, &mvUnknown);
                    A = std::min(A, 60000.0f), ts = 1.0f, mult = 1, bits = 1 | 4;
                    mvParts++, mvShaded += B < 1.0f ? 1 : 0, mvDark += B == 0.0f ? 1 : 0;
                }
                float law[3];
                // (lamp 2: a light with no GPU law, its colour alone: KeptAdd's law is its colour, lawOk 2)
                if (l == 2) std::memcpy(law, laws[l].colour, sizeof law);
                else GpuLampLaw::Eval(laws[l], tx[i].pos, tx[i].nrm, 0.0f, law);
                // (the kept halves, as BuildKeptMap gives them)
                const float a = OutdoorDeltaModel::FromHalf(OutdoorDeltaModel::ToHalf(A)), b = OutdoorDeltaModel::FromHalf(OutdoorDeltaModel::ToHalf(B));
                const float s = OutdoorDeltaModel::FromHalf(OutdoorDeltaModel::ToHalf(ts));
                RtLampModel::KeptAdd(law, a, b, s, static_cast<float>(mult), bits & 1, (bits & 4) != 0, thr, &R[i * 3]);
                RtLampGpu::TexelVertex v{};
                v.x = static_cast<float>(i % d.accW), v.y = static_cast<float>(i / d.accW), v.w = 1.0f;
                for (int q = 0; q < 3; q++) v.p[q] = tx[i].pos[q], v.n[q] = tx[i].nrm[q];
                v.p[3] = static_cast<float>(bits), v.n[3] = static_cast<float>(mult);
                v.base[0] = a, v.base[1] = b, v.base[2] = s;
                d.parts.push_back(v);
            }
            d.lamps.push_back(static_cast<uintptr_t>(l + 1));
            d.ranges.emplace_back(first, static_cast<UINT>(d.parts.size()) - first);
        }
        // the solve's tail at its own sum (R here: the lamps as kept); the GPU's texel takes what IndoorSplit reads back
        std::vector<uint32_t> cpuPacked(count);
        const auto vertexOf = [&](uint32_t i, int x, int y) {
            Tx& t = tx[i];
            RtLampGpu::TexelVertex v{};
            v.x = static_cast<float>(x), v.y = static_cast<float>(y), v.w = 1.0f;
            for (int q = 0; q < 3; q++) v.p[q] = t.base[q];
            v.p[3] = t.dither;
            if (t.roofed) {
                float xs[3], rampAmb[3] = {}, ramp = 0.0f;
                for (int q = 0; q < 3; q++) xs[q] = t.norm * t.S[q];
                if (!GpuLampLaw::IndoorSplit(xs, t.p, k, rampAmb, ramp)) rampAmb[0] = rampAmb[1] = rampAmb[2] = ramp = 0.0f;
                for (int q = 0; q < 3; q++) v.n[q] = rampAmb[q];
                v.n[3] = ramp;
            } else
                v.n[3] = t.p[3];
            v.base[0] = t.norm, v.base[1] = k, v.base[2] = t.floorOn ? floorA : 0.0f, v.base[3] = t.roofed ? 1.0f : 0.0f;
            v.mask[0] = static_cast<float>(i % d.accW), v.mask[1] = static_cast<float>(i / d.accW);
            return v;
        };
        for (uint32_t i = 0; i < count; i++) {
            Tx& t = tx[i];
            // the solve as it was: its sum a little off the recomposed one (lamps edited since), its pack input from the game's tail
            for (int q = 0; q < 3; q++) t.S[q] = std::max(R[i * 3 + q] * (0.7f + 0.6f * U(rng)), 0.0f);
            if (t.roofed) {
                float x[3], c[3];
                for (int q = 0; q < 3; q++) x[q] = t.norm * t.S[q];
                GpuLampLaw::Tone(x, c);
                const float a = (0.6f + 0.4f * U(rng)) * GpuLampLaw::Weight(c, k);
                const float amb[3] = {0.3f * U(rng), 0.3f * U(rng), 0.3f * U(rng)};
                for (int q = 0; q < 3; q++) t.p[q] = c[q] + (i % 2 ? a * amb[q] : 0.0f);
                t.p[3] = a;
            } else {
                for (int q = 0; q < 3; q++) t.p[q] = std::min(t.S[q], 1.0f);
                t.p[3] = U(rng);
            }
            float c[4];
            RtLampModel::KeptTexel(&R[i * 3], t.S, t.p, t.norm, k, t.roofed, t.floorOn ? floorA : 0.0f, t.dither, c);
            cpuPacked[i] = packCpu(c);
        }
        std::vector<uint32_t> expect = gamePx;
        if (m == 1) {
            for (uint32_t i = 0; i < count; i++) {
                d.texels.push_back(vertexOf(i, tx[i].x, tx[i].y));
                expect[static_cast<size_t>(tx[i].y) * w + tx[i].x] = cpuPacked[i];
            }
            // tile-edge copies (the source composed at the copy's place): kept texels to the places to their right, and kept texels
            // past the own map's corner to places inside it; every 7th kept place back to the game's
            for (uint32_t i = 0; i < count; i += 5) {
                const Tx& t = tx[i];
                if (t.x + 1 >= w) continue;
                d.copies.push_back(vertexOf(i, t.x + 1, t.y));
                expect[static_cast<size_t>(t.y) * w + t.x + 1] = cpuPacked[i];
            }
            for (uint32_t i = 1; i < count; i += 3) {
                const Tx& t = tx[i];
                if (t.x < ow || t.y >= oh) continue;
                d.copies.push_back(vertexOf(i, t.x - 30, t.y));
                expect[static_cast<size_t>(t.y) * w + t.x - 30] = cpuPacked[i];
            }
            for (uint32_t i = 3; i < count; i += 7) {
                const Tx& t = tx[i];
                RtLampGpu::TexelVertex v{};
                v.x = v.mask[0] = static_cast<float>(t.x), v.y = v.mask[1] = static_cast<float>(t.y), v.w = 1.0f;
                d.restores.push_back(v);
                expect[static_cast<size_t>(t.y) * w + t.x] = gamePx[static_cast<size_t>(t.y) * w + t.x];
            }
        } else {
            // the blocks before the blur (the solve's texels, the rest of the block as it was) with the rows beyond, in a layout
            d.passes = 2, d.layoutW = w;
            std::vector<uint32_t> at(static_cast<size_t>(w) * h, UINT32_MAX);
            for (uint32_t i = 0; i < count; i++) at[static_cast<size_t>(tx[i].y) * w + tx[i].x] = i;
            int ly = 0;
            for (const Blk& b : blocks) ly += (b.above ? 2 : 0) + (b.y1 - b.y0 + 1) + (b.below ? 2 : 0);
            d.layoutH = ly;
            d.layout.assign(static_cast<size_t>(d.layoutW) * d.layoutH, 0);
            ly = 0;
            for (const Blk& b : blocks) {
                const int bw = b.x1 - b.x0 + 1, rows = b.y1 - b.y0 + 1, top = b.above ? 2 : 0, total = top + rows + (b.below ? 2 : 0);
                std::vector<uint32_t> ext(static_cast<size_t>(total) * bw), before(static_cast<size_t>(rows) * bw);
                for (uint32_t& v : ext) v = static_cast<uint32_t>(rng()); // (rows beyond, and the block's unkept texels)
                for (int y = 0; y < total; y++)
                    for (int x = 0; x < bw; x++) d.layout[static_cast<size_t>(ly + y) * d.layoutW + x] = ext[static_cast<size_t>(y) * bw + x];
                for (int y = 0; y < rows; y++)
                    for (int x = 0; x < bw; x++) {
                        const uint32_t i = at[static_cast<size_t>(b.y0 + y) * w + b.x0 + x];
                        if (i != UINT32_MAX) {
                            ext[static_cast<size_t>(top + y) * bw + x] = cpuPacked[i];
                            d.wallTexels.push_back(vertexOf(i, x, ly + top + y));
                        }
                        before[static_cast<size_t>(y) * bw + x] = ext[static_cast<size_t>(top + y) * bw + x];
                    }
                OutdoorDeltaModel::BlurBlock(ext.data(), before.data(), bw, rows, b.above, b.below, d.passes);
                for (int y = 0; y < rows; y++)
                    for (int x = 0; x < bw; x++) expect[static_cast<size_t>(b.y0 + y) * w + b.x0 + x] = ext[static_cast<size_t>(top + y) * bw + x];
                d.extRects.push_back(RtLampGpu::Block{0, ly, bw - 1, ly + total - 1});
                d.outs.push_back(RtLampGpu::KeptBlockOut{b.x0, b.y0, b.x1, b.y1, 0, ly + top, ly + top + rows - 1, b.above, b.below});
                ly += total;
            }
        }
        // the GPU
        RtLampGpu::Engine eng(RtLampGpu::PlainCalls());
        std::string err;
        RtLampProxy::Mesh empty;
        if (!eng.Init(g_dev, g_shaders, 64, err) || !eng.SetProxy(g_dev, empty, err)) {
            Check(false, "kept engine: " + err);
            return false;
        }
        // the room's own map: the working map's top-left ow x oh (the game's commit 0x006191D0)
        std::vector<uint32_t> own(static_cast<size_t>(ow) * oh);
        for (int y = 0; y < oh; y++)
            for (int x = 0; x < ow; x++) own[static_cast<size_t>(y) * ow + x] = gamePx[static_cast<size_t>(y) * w + x];
        IDirect3DTexture9 *game = nullptr, *out = nullptr;
        RtLampGpu::Engine::MakePreblur(g_dev, ow, oh, own.data(), &game);
        g_dev->CreateTexture(ow, oh, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &out, nullptr);
        RtLampGpu::KeptMapGpu g;
        bool ok = game && out && RtLampGpu::Engine::UploadKept(g_dev, d, g);
        const char lawOk[nLamps] = {1, 1, 2, 1, 1};
        std::vector<uint32_t> got;
        g_dev->BeginScene();
        if (ok && eng.Begin(g_dev)) {
            std::string refusedWhy;
            // (a blurred wall atlas is drawn only into a map of its exact size; any map larger than the working map is refused)
            const bool refused = !eng.RelightKept(g_dev, g, laws, lawOk, thr, game, out, m == 0 ? w - 1 : w + 1, h, refusedWhy);
            Check(refused, "kept relight: a map of another size than the rule allows was drawn");
            ok = eng.CopyTexture(g_dev, game, out, ow, oh, err) && eng.RelightKept(g_dev, g, laws, lawOk, thr, game, out, ow, oh, err);
            eng.End(g_dev);
        } else
            ok = false;
        g_dev->EndScene();
        ok = ok && RtLampGpu::Engine::ReadTexture(g_dev, out, ow, oh, got);
        g.Release();
        if (game) game->Release();
        if (out) out->Release();
        Check(ok, "kept relight on the GPU: " + err);
        if (!ok) return false;
        std::vector<uint8_t> touched(static_cast<size_t>(w) * h, 0);
        for (const Tx& t : tx) touched[static_cast<size_t>(t.y) * w + t.x] = 1;
        for (size_t i = 0; i < expect.size(); i++) {
            const size_t x = i % w, y = i / w;
            if (x >= static_cast<size_t>(ow) || y >= static_cast<size_t>(oh)) continue; // (past the own map: not in it)
            const uint32_t gv = got[y * ow + x];
            if (expect[i] == gamePx[i] && !touched[i]) {
                untouchedBad += gv != gamePx[i];
                continue;
            }
            const int dl = OutdoorDeltaModel::MaxLevelDiff(expect[i], gv);
            n++;
            within1 += dl <= 1;
            maxDiff = std::max<long>(maxDiff, dl);
        }
        std::printf("  %s (kept %dx%d, own map %dx%d): %u kept texels, %zu parts, %zu copies, %zu restores, %zu blocks\n", m == 0 ? "class-2 wall atlas" : "floor", w, h,
                    ow, oh, count, d.parts.size(), d.copies.size(), d.restores.size(), d.outs.size());
        std::printf("    the moved lamp (the move worker's parts): %ld parts, %ld shaded by the wall or the disc, %ld dark\n", mvParts, mvShaded, mvDark);
        Check(mvParts > 0 && mvShaded > 0 && mvDark > 0 && mvUnknown == 0, "kept relight: the moved lamp's tests shade none of its parts (the scene is wrong)");
    }
    std::printf("  %ld texels compared: %.2f%% within 1/255, largest difference %ld/255; texels left as the game's changed: %ld\n", n, n ? 100.0 * within1 / n : 100.0, maxDiff, untouchedBad);
    Check(n > 0 && within1 >= static_cast<long>(0.999 * n) && maxDiff <= 2, "kept relight: the GPU's texels within 1/255 of the CPU's (99.9%), none past 2/255");
    Check(untouchedBad == 0, "kept relight: a texel no kept room holds changed");
    return true;
}

// (S8) The kept compose with the room's ambient now (ComposeKeptPS gAmb, RtLampModel::AmbScale): roofed texels whose pass adds
// the ambient and texels whose pass does not, composed with no scale, with a scale of exactly 1 (an unchanged ambient: the same
// texels byte for byte) and with a moved ambient, against the CPU's KeptTexel with the same scale
bool KeptAmbientCheck() {
    std::printf("\n[GPU room lamps] the kept compose with the room's ambient now (Rooms at Night without a solve)\n");
    std::mt19937 rng(4242);
    std::uniform_real_distribution<float> U(0.0f, 1.0f);
    const int w = 64, h = 64;
    const float k = 0.62f, floorA = 5.0f / 255.0f;
    RtLampGpu::KeptMapData d;
    d.w = w, d.h = h, d.accW = w, d.accH = h;
    const float solve[3] = {0.06f, 0.07f, 0.11f}, moved[3] = {0.021f, 0.03f, 0.19f};
    float unit[3], scale[3];
    RtLampModel::AmbScale(solve, solve, unit);
    RtLampModel::AmbScale(solve, moved, scale);
    std::vector<uint32_t> cpu(static_cast<size_t>(w) * h), cpuMoved(cpu.size());
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            float base[3], S[3], p[4], xs[3], c[3], o[4];
            for (int q = 0; q < 3; q++) base[q] = 0.6f * U(rng) * U(rng), S[q] = base[q];
            const float norm = 0.6f + U(rng), dither = (y % 4 == 3) ? 0.0f : RoomMapDither::Offset(x, y) / 255.0f;
            for (int q = 0; q < 3; q++) xs[q] = norm * S[q];
            GpuLampLaw::Tone(xs, c);
            const float a = (0.6f + 0.4f * U(rng)) * GpuLampLaw::Weight(c, k);
            const bool pass = (x + y) % 2 == 0; // (a ceiling pass adds the ambient; the wall and floor passes do not)
            for (int q = 0; q < 3; q++) p[q] = c[q] + (pass ? a * solve[q] : 0.0f);
            p[3] = a;
            RtLampModel::KeptTexel(base, S, p, norm, k, true, floorA, dither, o, unit);
            cpu[static_cast<size_t>(y) * w + x] = OutdoorDeltaModel::PackModel(o);
            RtLampModel::KeptTexel(base, S, p, norm, k, true, floorA, dither, o, scale);
            cpuMoved[static_cast<size_t>(y) * w + x] = OutdoorDeltaModel::PackModel(o);
            RtLampGpu::TexelVertex v{};
            v.x = static_cast<float>(x), v.y = static_cast<float>(y), v.w = 1.0f;
            for (int q = 0; q < 3; q++) v.p[q] = base[q];
            v.p[3] = dither;
            float rampAmb[3] = {}, ramp = 0.0f;
            if (!GpuLampLaw::IndoorSplit(xs, p, k, rampAmb, ramp)) rampAmb[0] = rampAmb[1] = rampAmb[2] = ramp = 0.0f;
            for (int q = 0; q < 3; q++) v.n[q] = rampAmb[q];
            v.n[3] = ramp;
            v.base[0] = norm, v.base[1] = k, v.base[2] = floorA, v.base[3] = 1.0f;
            v.mask[0] = static_cast<float>(x), v.mask[1] = static_cast<float>(y);
            d.texels.push_back(v);
        }
    RtLampGpu::Engine eng(RtLampGpu::PlainCalls());
    std::string err;
    RtLampProxy::Mesh empty;
    if (!eng.Init(g_dev, g_shaders, 64, err) || !eng.SetProxy(g_dev, empty, err)) {
        Check(false, "kept ambient engine: " + err);
        return false;
    }
    std::vector<uint32_t> gamePx(cpu.size(), 0);
    IDirect3DTexture9 *game = nullptr, *out = nullptr;
    RtLampGpu::Engine::MakePreblur(g_dev, w, h, gamePx.data(), &game);
    g_dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &out, nullptr);
    RtLampGpu::KeptMapGpu g;
    bool ok = game && out && RtLampGpu::Engine::UploadKept(g_dev, d, g);
    std::vector<uint32_t> got[3];
    const float* scales[3] = {nullptr, unit, scale};
    for (int run = 0; run < 3 && ok; run++) {
        g_dev->BeginScene();
        if (eng.Begin(g_dev)) {
            ok = eng.RelightKept(g_dev, g, nullptr, nullptr, 0.0f, game, out, w, h, err, scales[run]);
            eng.End(g_dev);
        } else
            ok = false;
        g_dev->EndScene();
        ok = ok && RtLampGpu::Engine::ReadTexture(g_dev, out, w, h, got[run]);
    }
    g.Release();
    if (game) game->Release();
    if (out) out->Release();
    Check(ok, "kept ambient relight on the GPU: " + err);
    if (!ok) return false;
    long same = 0, within = 0, maxD = 0, movedWithin = 0, movedMax = 0, changed = 0;
    for (size_t i = 0; i < cpu.size(); i++) {
        same += got[0][i] == got[1][i];
        const int d0 = OutdoorDeltaModel::MaxLevelDiff(cpu[i], got[1][i]), d1 = OutdoorDeltaModel::MaxLevelDiff(cpuMoved[i], got[2][i]);
        within += d0 <= 1, maxD = std::max<long>(maxD, d0);
        movedWithin += d1 <= 1, movedMax = std::max<long>(movedMax, d1);
        changed += got[2][i] != got[1][i];
    }
    const long n = static_cast<long>(cpu.size());
    std::printf("  %ld texels: scale 1 = no scale byte for byte %ld; vs the CPU %ld within 1/255 (largest %ld); a moved ambient: %ld changed, %ld within 1/255 of the "
                "CPU (largest %ld)\n",
                n, same, within, maxD, changed, movedWithin, movedMax);
    Check(same == n, "kept ambient: an unchanged ambient (scale exactly 1) composes byte for byte as without one");
    Check(within >= static_cast<long>(0.999 * n) && maxD <= 2, "kept ambient: the GPU's texels within 1/255 of the CPU's (99.9%), none past 2/255");
    Check(changed > 0 && movedWithin >= static_cast<long>(0.999 * n) && movedMax <= 2, "kept ambient: a moved ambient follows the CPU within 1/255 (99.9%)");
    return true;
}

// The sharper floors' targets (RtLampGpu::SharperFormat): ComposeKeptPS, ComposePS and ComposeK0PS into a 16-bit float target take
// the texel with no dither and no pack, and an 8-bit target drawn after it keeps both (the constant does not stay behind)
bool ReadHalves(IDirect3DTexture9* t, int w, int h, std::vector<float>& out) {
    IDirect3DSurface9 *src = nullptr, *sys = nullptr;
    bool ok = SUCCEEDED(t->GetSurfaceLevel(0, &src)) && SUCCEEDED(g_dev->CreateOffscreenPlainSurface(w, h, D3DFMT_A16B16G16R16F, D3DPOOL_SYSTEMMEM, &sys, nullptr)) &&
              SUCCEEDED(g_dev->GetRenderTargetData(src, sys));
    D3DLOCKED_RECT lr{};
    if (ok && (ok = SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY)))) {
        out.resize(static_cast<size_t>(w) * h * 4);
        for (int y = 0; y < h; y++) {
            const uint16_t* row = reinterpret_cast<const uint16_t*>(static_cast<const char*>(lr.pBits) + static_cast<size_t>(y) * lr.Pitch);
            for (int i = 0; i < w * 4; i++) out[static_cast<size_t>(y) * w * 4 + i] = OutdoorDeltaModel::FromHalf(row[i]);
        }
        sys->UnlockRect();
    }
    if (sys) sys->Release();
    if (src) src->Release();
    return ok;
}
bool SharperTargetCheck() {
    std::printf("\n[sharper floors] the 16-bit float target against the texel unpacked, the 8-bit one with its dither and pack\n");
    const D3DFORMAT fmt = RtLampGpu::SharperFormat(g_dev);
    if (fmt != D3DFMT_A16B16G16R16F) {
        std::printf("  this device falls back to the 8-bit target\n");
        return true;
    }
    std::mt19937 rng(777);
    std::uniform_real_distribution<float> U(0.0f, 1.0f);
    const int w = 64, h = 64;
    const float k = 0.62f, floorA = 5.0f / 255.0f;
    RtLampGpu::KeptMapData d;
    d.w = w, d.h = h, d.accW = w, d.accH = h;
    std::vector<float> plain(static_cast<size_t>(w) * h * 4);
    std::vector<uint32_t> packed(static_cast<size_t>(w) * h);
    std::vector<RtLampGpu::TexelVertex> raw;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const size_t i = static_cast<size_t>(y) * w + x;
            float base[3], p[4], xs[3], c[3], o[4];
            for (int q = 0; q < 3; q++) base[q] = 0.6f * U(rng) * U(rng);
            const float norm = 0.6f + U(rng), dither = RoomMapDither::Offset(x, y) / 255.0f;
            for (int q = 0; q < 3; q++) xs[q] = norm * base[q];
            GpuLampLaw::Tone(xs, c);
            const float a = (0.6f + 0.4f * U(rng)) * GpuLampLaw::Weight(c, k);
            for (int q = 0; q < 3; q++) p[q] = c[q] + a * 0.05f;
            p[3] = a;
            RtLampModel::KeptTexel(base, base, p, norm, k, true, floorA, 0.0f, &plain[i * 4]);
            RtLampModel::KeptTexel(base, base, p, norm, k, true, floorA, dither, o);
            packed[i] = OutdoorDeltaModel::PackModel(o);
            RtLampGpu::TexelVertex v{};
            v.x = static_cast<float>(x), v.y = static_cast<float>(y), v.w = 1.0f;
            for (int q = 0; q < 3; q++) v.p[q] = base[q];
            v.p[3] = dither;
            float rampAmb[3] = {}, ramp = 0.0f;
            if (!GpuLampLaw::IndoorSplit(xs, p, k, rampAmb, ramp)) rampAmb[0] = rampAmb[1] = rampAmb[2] = ramp = 0.0f;
            for (int q = 0; q < 3; q++) v.n[q] = rampAmb[q];
            v.n[3] = ramp;
            v.base[0] = norm, v.base[1] = k, v.base[2] = floorA, v.base[3] = 1.0f;
            v.mask[0] = static_cast<float>(x), v.mask[1] = static_cast<float>(y);
            d.texels.push_back(v);
            RtLampGpu::TexelVertex r{}; // (room 0's compose: a slow ramp with the dither at every texel)
            r.x = v.x, r.y = v.y, r.w = 1.0f;
            for (int q = 0; q < 3; q++) r.base[q] = (0.02f + 0.002f * q) + 0.0001f * x + 0.00003f * y;
            r.base[3] = dither;
            r.n[1] = 1.0f, r.n[3] = 0.25f;
            raw.push_back(r);
        }
    RtLampGpu::Engine eng(RtLampGpu::PlainCalls());
    std::string err;
    RtLampProxy::Mesh empty;
    if (!eng.Init(g_dev, g_shaders, 64, err) || !eng.SetProxy(g_dev, empty, err)) {
        Check(false, "sharper targets' engine: " + err);
        return false;
    }
    std::vector<uint32_t> gamePx(packed.size(), 0);
    IDirect3DTexture9 *game = nullptr, *wide = nullptr, *narrow = nullptr;
    RtLampGpu::Engine::MakePreblur(g_dev, w, h, gamePx.data(), &game);
    g_dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, fmt, D3DPOOL_DEFAULT, &wide, nullptr);
    g_dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &narrow, nullptr);
    RtLampGpu::KeptMapGpu g;
    RtLampGpu::Map m;
    m.w = w, m.h = h, m.count = static_cast<UINT>(raw.size());
    bool ok = game && wide && narrow && RtLampGpu::Engine::UploadKept(g_dev, d, g) && RtLampGpu::Engine::MakeVb(g_dev, raw.data(), m.count, &m.vb);
    std::vector<float> kept, room0[2];
    std::vector<uint32_t> kept8;
    const auto drawn = [&](auto draw) {
        g_dev->BeginScene();
        const bool began = eng.Begin(g_dev);
        const bool r = began && draw();
        if (began) eng.End(g_dev);
        g_dev->EndScene();
        return r;
    };
    ok = ok && drawn([&] { return eng.RelightKept(g_dev, g, nullptr, nullptr, 0.0f, game, wide, w, h, err); }) && ReadHalves(wide, w, h, kept);
    ok = ok && drawn([&] { return eng.RelightKept(g_dev, g, nullptr, nullptr, 0.0f, game, narrow, w, h, err); }) && RtLampGpu::Engine::ReadTexture(g_dev, narrow, w, h, kept8);
    for (int i = 0; i < 2 && ok; i++) {
        RtLampGpu::Params p;
        p.keptX = i == 1;
        ok = drawn([&] { return eng.Accumulate(g_dev, m, nullptr, 0, p, true, err) && eng.Compose(g_dev, m, nullptr, wide, p, err); }) && ReadHalves(wide, w, h, room0[i]);
    }
    g.Release();
    if (m.vb) m.vb->Release();
    if (game) game->Release();
    if (wide) wide->Release();
    if (narrow) narrow->Release();
    Check(ok, "sharper targets on the GPU: " + err);
    if (!ok) return false;
    float keptMax = 0.0f, room0Max[2] = {};
    long within = 0;
    for (size_t i = 0; i < packed.size(); i++) {
        for (int q = 0; q < 4; q++) keptMax = std::max(keptMax, std::fabs(kept[i * 4 + q] - plain[i * 4 + q]));
        within += OutdoorDeltaModel::MaxLevelDiff(packed[i], kept8[i]) <= 1;
        for (int s = 0; s < 2; s++) {
            for (int q = 0; q < 3; q++) room0Max[s] = std::max(room0Max[s], std::fabs(room0[s][i * 4 + q] - raw[i].base[q]));
            room0Max[s] = std::max(room0Max[s], std::fabs(room0[s][i * 4 + 3] - 0.25f));
        }
    }
    const long n = static_cast<long>(packed.size());
    std::printf("  %ld texels: kept compose off the plain texel by at most %.5f; room 0's %.6f, with kept lamps %.6f; the 8-bit target %ld within 1/255 of the dithered pack\n", n, keptMax,
                room0Max[0], room0Max[1], within);
    Check(keptMax <= 0.0006f, "ComposeKeptPS into a float target: the texel as it is (half precision)");
    Check(room0Max[0] <= 0.0002f && room0Max[1] <= 0.0002f, "ComposePS and ComposeK0PS into a float target: no dither, no pack (a dim ramp keeps half precision)");
    Check(within >= static_cast<long>(0.999 * n), "an 8-bit target drawn after a float one keeps the dither and the pack");
    return true;
}

// ---------------------------------------------------------------- 6. GPU room lamps: room 0's kept lamps (AddKeptParts)
// Room-0 texels relit by Real-time outdoor lamps' passes with no lamp of its own (Accumulate with none), the kept parts of
// lamps it does not draw added to the sum (AddKeptParts: one with a GPU law, one with its colour alone), then ComposePS, against
// the CPU: PackInput(base + the KeptAdd parts, alpha), the dither, the game's pack. Texels no part reaches keep their base.
bool Room0KeptCheck() {
    std::printf("\n[GPU room lamps] room 0's kept lamps added to Real-time outdoor lamps' sum against KeptAdd + PackInput on the CPU\n");
    std::mt19937 rng(77);
    std::uniform_real_distribution<float> U(0.0f, 1.0f);
    const int w = 64, h = 32, nk = 2;
    const float thr = 3.0f / 255.0f;
    GpuLampLaw::Lamp laws[nk];
    for (GpuLampLaw::Lamp& L : laws) {
        L.pos[0] = 16.0f * U(rng), L.pos[1] = 1.0f + U(rng), L.pos[2] = 8.0f * U(rng), L.W = 3.0f + 4.0f * U(rng);
        for (float& c : L.colour) c = 0.3f + 0.7f * U(rng);
        GpuLampLaw::NoCone(L);
    }
    const char lawOk[nk] = {1, 2}; // (the second: its colour alone)
    std::vector<RtLampGpu::TexelVertex> verts, parts;
    std::vector<std::pair<UINT, UINT>> ranges(nk);
    std::vector<uint32_t> want(static_cast<size_t>(w) * h, 0);
    std::vector<float> S(static_cast<size_t>(w) * h * 3);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            RtLampGpu::TexelVertex v{};
            v.x = static_cast<float>(x), v.y = static_cast<float>(y), v.w = 1.0f;
            v.p[0] = x * 0.25f, v.p[1] = 0.0f, v.p[2] = y * 0.25f, v.n[1] = 1.0f;
            v.n[3] = U(rng) * 0.5f; // the record's alpha
            for (int c = 0; c < 3; c++) v.base[c] = U(rng) < 0.3f ? 1.2f * U(rng) : 0.2f * U(rng), S[(static_cast<size_t>(y) * w + x) * 3 + c] = v.base[c];
            v.base[3] = (x + y) % 3 ? RoomMapDither::Offset(x, y) / 255.0f : 0.0f;
            verts.push_back(v);
        }
    for (int l = 0; l < nk; l++) {
        ranges[l].first = static_cast<UINT>(parts.size());
        for (int i = 0; i < w * h; i++) {
            if ((i + l) % 4 == 0) continue;
            const RtLampGpu::TexelVertex& t = verts[i];
            RtLampGpu::TexelVertex v{};
            v.x = t.x, v.y = t.y, v.w = 1.0f;
            for (int c = 0; c < 3; c++) v.p[c] = t.p[c], v.n[c] = t.n[c];
            const float A = OutdoorDeltaModel::FromHalf(OutdoorDeltaModel::ToHalf(0.2f + U(rng))), B = OutdoorDeltaModel::FromHalf(OutdoorDeltaModel::ToHalf(U(rng)));
            const int bits = (U(rng) < 0.2f ? 1 : 0) | (U(rng) < 0.8f ? 4 : 0), mult = U(rng) < 0.2f ? 2 : 1;
            v.p[3] = static_cast<float>(bits), v.n[3] = static_cast<float>(mult), v.base[0] = A, v.base[1] = B, v.base[2] = 1.0f;
            parts.push_back(v);
            float law[3];
            if (lawOk[l] == 2) std::memcpy(law, laws[l].colour, sizeof law);
            else GpuLampLaw::Eval(laws[l], t.p, t.n, 0.0f, law);
            RtLampModel::KeptAdd(law, A, B, 1.0f, static_cast<float>(mult), bits & 1, (bits & 4) != 0, thr, &S[static_cast<size_t>(i) * 3]);
        }
        ranges[l].second = static_cast<UINT>(parts.size()) - ranges[l].first;
    }
    for (int i = 0; i < w * h; i++) {
        float c[4];
        OutdoorDeltaModel::PackInput(&S[static_cast<size_t>(i) * 3], verts[i].n[3], c);
        if (verts[i].base[3] != 0.0f) OutdoorDeltaModel::Dither(c, verts[i].base[3]);
        want[i] = OutdoorDeltaModel::PackModel(c);
    }
    RtLampGpu::Engine eng(RtLampGpu::PlainCalls());
    std::string err;
    RtLampProxy::Mesh empty;
    if (!eng.Init(g_dev, g_shaders, 64, err) || !eng.SetProxy(g_dev, empty, err)) {
        Check(false, "room-0 kept engine: " + err);
        return false;
    }
    RtLampGpu::Map m;
    m.w = w, m.h = h, m.count = static_cast<UINT>(verts.size());
    IDirect3DVertexBuffer9* pvb = nullptr;
    IDirect3DTexture9* out = nullptr;
    bool ok = RtLampGpu::Engine::MakeVb(g_dev, verts.data(), m.count, &m.vb) && RtLampGpu::Engine::MakeVb(g_dev, parts.data(), static_cast<UINT>(parts.size()), &pvb) &&
              SUCCEEDED(g_dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &out, nullptr));
    RtLampGpu::Params p;
    p.thr = thr;
    std::vector<uint32_t> got;
    g_dev->BeginScene();
    if (ok && eng.Begin(g_dev)) {
        ok = eng.Accumulate(g_dev, m, nullptr, 0, p, true, err) && eng.AddKeptParts(g_dev, pvb, ranges, laws, lawOk, thr, w, h, err) &&
             eng.Compose(g_dev, m, nullptr, out, p, err);
        eng.End(g_dev);
    } else
        ok = false;
    g_dev->EndScene();
    ok = ok && RtLampGpu::Engine::ReadTexture(g_dev, out, w, h, got);
    if (m.vb) m.vb->Release();
    if (pvb) pvb->Release();
    if (out) out->Release();
    Check(ok, "room-0 kept parts on the GPU: " + err);
    if (!ok) return false;
    long within1 = 0, maxDiff = 0;
    for (int i = 0; i < w * h; i++) {
        const int d = OutdoorDeltaModel::MaxLevelDiff(want[i], got[i]);
        within1 += d <= 1;
        maxDiff = std::max<long>(maxDiff, d);
    }
    std::printf("  %d texels, %zu kept parts of 2 lamps (one its colour alone): %.2f%% within 1/255, largest difference %ld/255\n", w * h, parts.size(),
                100.0 * within1 / (w * h), maxDiff);
    Check(within1 >= (w * h) * 999 / 1000 && maxDiff <= 2, "room-0 kept parts: the GPU's texels within 1/255 of the CPU's (99.9%), none past 2/255");
    return true;
}

// ---------------------------------------------------------------- 7. GPU room lamps: room 0's exit lamps and floor mask lamps
// Room 0's kept lamps with floor mask parts and exit parts (AddKeptParts extra, ComposeK0PS): masked texels (their alpha from
// the floor mask's two luminances, the kept lamps' shares x KeptWrapLum now), exit ground texels (ExitAlpha of the exit lamps'
// light left in the base + the kept exit lamps' KeptAdd light), plain texels; one lamp with a GPU law, one its colour alone,
// both exits. Against the CPU: KeptAdd, KeptWrapLum, RtLampModel::Compose (with gExit), the laws as kept, changed (dimmed,
// recoloured) and one switched off. The bar is 0/255: every texel the CPU's
bool Room0KeptXCheck() {
    std::printf("\n[GPU room lamps] room 0's exit and floor mask lamps against KeptAdd + KeptWrapLum + Compose on the CPU\n");
    std::mt19937 rng(91);
    std::uniform_real_distribution<float> U(0.0f, 1.0f);
    const int w = 64, h = 32, nk = 2;
    const float thr = 3.0f / 255.0f;
    GpuLampLaw::Lamp kept[nk];
    for (GpuLampLaw::Lamp& L : kept) {
        L.pos[0] = 16.0f * U(rng), L.pos[1] = 1.0f + U(rng), L.pos[2] = 8.0f * U(rng), L.W = 3.0f + 4.0f * U(rng);
        for (float& c : L.colour) c = 0.3f + 0.7f * U(rng);
        GpuLampLaw::NoCone(L);
    }
    const char lawOk[nk] = {1, 2}; // (the second: its colour alone)
    std::vector<RtLampGpu::TexelVertex> verts, parts;
    std::vector<std::pair<UINT, UINT>> ranges(nk);
    std::vector<RtLampModel::TexelIn> in(static_cast<size_t>(w) * h);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const int i = y * w + x;
            RtLampGpu::TexelVertex v{};
            v.x = static_cast<float>(x), v.y = static_cast<float>(y), v.w = 1.0f;
            v.p[0] = x * 0.25f, v.p[1] = 0.0f, v.p[2] = y * 0.25f, v.n[1] = 1.0f;
            RtLampModel::TexelIn& t = in[i];
            const int kind = i % 3; // 0 masked, 1 exit ground, 2 plain
            v.n[3] = kind == 2 ? U(rng) * 0.5f : 0.0f;
            for (int c = 0; c < 3; c++) v.base[c] = t.base[c] = U(rng) < 0.3f ? 1.2f * U(rng) : 0.2f * U(rng);
            v.base[3] = t.dither = (x + y) % 3 ? RoomMapDither::Offset(x, y) / 255.0f : 0.0f;
            t.alpha = v.n[3];
            if (kind == 0) {
                v.p[3] = 1.0f, t.masked = true;
                t.baseAll = v.mask[1] = U(rng) < 0.3f ? 0.0f : 0.5f * U(rng);
                t.baseVis = v.mask[0] = t.baseAll * U(rng);
            } else if (kind == 1) {
                t.exitGround = true, v.mask[3] = 1.0f;
                t.exitBase = v.mask[2] = U(rng) < 0.5f ? 0.0f : 0.3f * U(rng);
            }
            verts.push_back(v);
        }
    // each lamp: light parts at 3 texels of 4 (exit parts), floor mask parts at most masked texels (shares as BuildKept0 keeps them)
    const size_t nPairs = static_cast<size_t>(w) * h * nk;
    std::vector<float> visShare(nPairs, 0.0f), allShare(nPairs, 0.0f), A(nPairs, -1.0f), B(nPairs, 0.0f), mult(nPairs, 1.0f);
    std::vector<int> bits(nPairs, 0);
    for (int l = 0; l < nk; l++) {
        ranges[l].first = static_cast<UINT>(parts.size());
        for (int i = 0; i < w * h; i++) {
            const RtLampGpu::TexelVertex& t = verts[i];
            const size_t k = static_cast<size_t>(i) * nk + l;
            if ((i + l) % 4 != 0) {
                RtLampGpu::TexelVertex v{};
                v.x = t.x, v.y = t.y, v.w = 1.0f;
                for (int c = 0; c < 3; c++) v.p[c] = t.p[c], v.n[c] = t.n[c];
                A[k] = OutdoorDeltaModel::FromHalf(OutdoorDeltaModel::ToHalf(0.2f + U(rng))), B[k] = OutdoorDeltaModel::FromHalf(OutdoorDeltaModel::ToHalf(U(rng)));
                bits[k] = (U(rng) < 0.2f ? 1 : 0) | (U(rng) < 0.8f ? 4 : 0), mult[k] = U(rng) < 0.2f ? 2.0f : 1.0f;
                v.p[3] = static_cast<float>(bits[k]), v.n[3] = mult[k], v.base[0] = A[k], v.base[1] = B[k], v.base[2] = 1.0f, v.mask[2] = 1.0f;
                parts.push_back(v);
            }
            if (in[i].masked && U(rng) < 0.8f) {
                RtLampGpu::TexelVertex v{};
                v.x = t.x, v.y = t.y, v.w = 1.0f;
                for (int c = 0; c < 3; c++) v.p[c] = t.p[c], v.n[c] = t.n[c];
                v.n[3] = 1.0f;
                allShare[k] = 0.5f + U(rng), visShare[k] = allShare[k] * (U(rng) < 0.3f ? 0.0f : U(rng));
                v.mask[0] = visShare[k], v.mask[1] = allShare[k];
                parts.push_back(v);
            }
        }
        ranges[l].second = static_cast<UINT>(parts.size()) - ranges[l].first;
    }
    RtLampGpu::Engine eng(RtLampGpu::PlainCalls());
    std::string err;
    RtLampProxy::Mesh empty;
    if (!eng.Init(g_dev, g_shaders, 64, err) || !eng.SetProxy(g_dev, empty, err)) {
        Check(false, "room-0 kept exit/mask engine: " + err);
        return false;
    }
    RtLampGpu::Map m;
    m.w = w, m.h = h, m.count = static_cast<UINT>(verts.size());
    IDirect3DVertexBuffer9* pvb = nullptr;
    IDirect3DTexture9* out = nullptr;
    const bool made = RtLampGpu::Engine::MakeVb(g_dev, verts.data(), m.count, &m.vb) &&
                      RtLampGpu::Engine::MakeVb(g_dev, parts.data(), static_cast<UINT>(parts.size()), &pvb) &&
                      SUCCEEDED(g_dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &out, nullptr));
    long n = 0, exact = 0, maxDiff = 0, alphaLit = 0, alphaMax = 0;
    for (int round = 0; made && round < 6; round++) {
        GpuLampLaw::Lamp laws[nk] = {kept[0], kept[1]};
        if (round >= 4) { // (the first a RectangleAreaLight 1 x 0.6 m, then a DiscAreaLight of radius 0.6, facing down: KeptLaw's AreaLaw, KeptWrapLum)
            const float u[3] = {1.0f, 0.0f, 0.0f}, v[3] = {0.0f, 0.0f, 1.0f}, nrm[3] = {0.0f, 1.0f, 0.0f};
            GpuLampLaw::AreaShape(laws[0], u, v, nrm, round == 4 ? 0.5f : 0.36f, round == 4 ? 0.3f : 0.6f, round == 5);
        }
        if (round == 3) { // (the first a TubeLight standing 1 m tall: KeptLaw's TubeLaw, KeptWrapLum toward its position)
            const float s[3] = {0.0f, 1.0f, 0.0f};
            GpuLampLaw::TubeShape(laws[0], s, 1.0f);
        }
        if (round == 1)
            for (GpuLampLaw::Lamp& L : laws) L.colour[0] *= 0.4f, L.colour[2] *= 1.7f, L.W *= 0.6f;
        if (round == 2) laws[0].colour[0] = laws[0].colour[1] = laws[0].colour[2] = 0.0f; // (switched off)
        std::vector<uint32_t> want(static_cast<size_t>(w) * h);
        for (int i = 0; i < w * h; i++) {
            float G[3] = {}, gVis = 0.0f, gAll = 0.0f, gExit = 0.0f;
            for (int l = 0; l < nk; l++) {
                const size_t k = static_cast<size_t>(i) * nk + l;
                if (A[k] >= 0.0f) {
                    float law[3], add[3] = {};
                    if (lawOk[l] == 2) std::memcpy(law, laws[l].colour, sizeof law);
                    else GpuLampLaw::Eval(laws[l], verts[i].p, verts[i].n, 0.0f, law);
                    RtLampModel::KeptAdd(law, A[k], B[k], 1.0f, mult[k], bits[k] & 1, (bits[k] & 4) != 0, thr, add);
                    for (int c = 0; c < 3; c++) G[c] += add[c];
                    gExit += OutdoorDeltaModel::Lum3(add);
                }
                if (allShare[k] != 0.0f) {
                    const float wl = RtLampModel::KeptWrapLum(laws[l], lawOk[l] == 2, verts[i].p);
                    gVis += wl * visShare[k], gAll += wl * allShare[k];
                }
            }
            want[i] = RtLampModel::Compose(in[i], G, gVis, gAll, gExit);
        }
        RtLampGpu::Params p;
        p.thr = thr, p.mask = true, p.keptX = true;
        std::vector<uint32_t> got;
        bool ok = true;
        g_dev->BeginScene();
        if (eng.Begin(g_dev)) {
            ok = eng.Accumulate(g_dev, m, nullptr, 0, p, true, err) && eng.AddKeptParts(g_dev, pvb, ranges, laws, lawOk, thr, w, h, err, true) &&
                 eng.Compose(g_dev, m, nullptr, out, p, err);
            eng.End(g_dev);
        } else
            ok = false;
        g_dev->EndScene();
        ok = ok && RtLampGpu::Engine::ReadTexture(g_dev, out, w, h, got);
        Check(ok, "room-0 kept exit/mask parts on the GPU: " + err);
        if (!ok) break;
        for (int i = 0; i < w * h; i++) {
            const int d = OutdoorDeltaModel::MaxLevelDiff(want[i], got[i]);
            n++, exact += d == 0, maxDiff = std::max<long>(maxDiff, d);
            if (!in[i].masked && !in[i].exitGround) continue;
            alphaLit += (want[i] >> 24) != 0;
            alphaMax = std::max<long>(alphaMax, static_cast<long>(want[i] >> 24));
        }
    }
    if (m.vb) m.vb->Release();
    if (pvb) pvb->Release();
    if (out) out->Release();
    Check(made, "room-0 kept exit/mask buffers could not be made");
    std::printf("  %ld texels over 6 rounds of laws (as kept, changed, one lamp off, one a TubeLight, a RectangleAreaLight, a DiscAreaLight), %zu kept points of 2 exit lamps (one its colour alone): %.2f%% exact, "
                "largest difference %ld/255; %ld masked or exit ground texels with an alpha (up to %ld/255)\n",
                n, parts.size(), n ? 100.0 * exact / n : 100.0, maxDiff, alphaLit, alphaMax);
    Check(n > 0 && maxDiff == 0, "room-0 kept exit/mask parts: the GPU's texels are the CPU's (0/255)");
    Check(alphaLit > 0, "room-0 kept exit/mask parts: no texel took an alpha from them");
    return true;
}

// Room 0's floor mask "all" on the GPU (Params::maskAllGpu: ComposeK0PS's MaskAll reads the outdoor batch's lamp constants)
// with room 0's kept lamps added between the batch and the compose (AddKeptParts): the same texels as the CPU's "all" handed in
// through dyn.x. AddKeptParts draws each kept lamp with its own constants c0..c96; the compose must still see the batch's
bool MaskAllAfterKeptCheck() {
    std::printf("\n[GPU room lamps] room 0's floor mask of the outdoor lamps on the GPU with kept lamps added before the compose\n");
    std::mt19937 rng(17);
    std::uniform_real_distribution<float> U(0.0f, 1.0f);
    const int w = 32, h = 16, no = 3;
    const float thr = 3.0f / 255.0f;
    RtLampModel::Lamp outdoor[no];
    for (RtLampModel::Lamp& L : outdoor) {
        L.law.pos[0] = 8.0f * U(rng), L.law.pos[1] = 2.0f + U(rng), L.law.pos[2] = 4.0f * U(rng), L.law.W = 20.0f + 20.0f * U(rng);
        for (float& c : L.law.colour) c = 0.3f + 0.7f * U(rng);
        GpuLampLaw::NoCone(L.law);
        L.mult = U(rng) < 0.5f ? 2.0f : 1.0f;
    }
    GpuLampLaw::Lamp kept = outdoor[0].law; // (a kept lamp far from them, of another colour)
    kept.pos[0] = 40.0f, kept.colour[0] = 0.1f, kept.colour[2] = 1.0f;
    const char lawOk[1] = {1};
    std::vector<RtLampGpu::TexelVertex> verts, parts;
    std::vector<RtLampGpu::TexelDyn> dyn;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            RtLampGpu::TexelVertex v{};
            v.x = static_cast<float>(x), v.y = static_cast<float>(y), v.w = 1.0f;
            v.p[0] = x * 0.25f, v.p[1] = 0.0f, v.p[2] = y * 0.25f, v.p[3] = 1.0f, v.n[1] = 1.0f; // (every texel masked)
            for (int c = 0; c < 3; c++) v.base[c] = 0.1f * U(rng);
            v.mask[1] = 0.2f * U(rng), v.mask[0] = v.mask[1] * U(rng);
            verts.push_back(v);
            float all = 0.0f; // the CPU's: AccPS's law without the shadow, toward the lamp, dropped under the threshold
            for (const RtLampModel::Lamp& L : outdoor) {
                float d[3] = {L.law.pos[0] - v.p[0], L.law.pos[1] - v.p[1], L.law.pos[2] - v.p[2]}, c3[3];
                const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                for (float& q : d) q /= len;
                GpuLampLaw::Eval(L.law, v.p, d, 0.0f, c3);
                if ((c3[2] + c3[1]) + c3[0] >= thr) all += L.mult * OutdoorDeltaModel::Lum3(c3);
            }
            dyn.push_back(RtLampGpu::TexelDyn{{all, -1.0f, 0.0f, 0.0f}});
            RtLampGpu::TexelVertex k = v; // the kept lamp's part here and its floor mask part
            k.p[3] = 4.0f, k.n[3] = 1.0f, k.base[0] = 0.5f, k.base[1] = 1.0f, k.base[2] = 1.0f, k.base[3] = 0.0f;
            k.mask[0] = 0.3f, k.mask[1] = 0.6f, k.mask[2] = 0.0f, k.mask[3] = 0.0f;
            parts.push_back(k);
        }
    const std::vector<std::pair<UINT, UINT>> ranges = {{0u, static_cast<UINT>(parts.size())}};
    RtLampGpu::Engine eng(RtLampGpu::PlainCalls());
    std::string err;
    RtLampProxy::Mesh empty;
    if (!eng.Init(g_dev, g_shaders, 64, err) || !eng.SetProxy(g_dev, empty, err)) {
        Check(false, "mask after kept engine: " + err);
        return false;
    }
    RtLampGpu::Map mGpu, mCpu;
    mGpu.w = mCpu.w = w, mGpu.h = mCpu.h = h, mGpu.count = mCpu.count = static_cast<UINT>(verts.size());
    IDirect3DVertexBuffer9* pvb = nullptr;
    IDirect3DTexture9* out = nullptr;
    const bool made = RtLampGpu::Engine::MakeVb(g_dev, verts.data(), mGpu.count, &mGpu.vb) && RtLampGpu::Engine::MakeVb(g_dev, verts.data(), mCpu.count, &mCpu.vb) &&
                      RtLampGpu::Engine::MakeDyn(g_dev, dyn.data(), mCpu.count, &mCpu.dyn) &&
                      RtLampGpu::Engine::MakeVb(g_dev, parts.data(), static_cast<UINT>(parts.size()), &pvb) &&
                      SUCCEEDED(g_dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &out, nullptr));
    std::vector<uint32_t> got[2];
    bool ok = made;
    for (int run = 0; ok && run < 2; run++) { // 0: "all" on the GPU (MaskAll), 1: the CPU's through dyn.x
        RtLampGpu::Params p;
        p.thr = thr, p.mask = true, p.keptX = true, p.maskAllGpu = run == 0;
        const RtLampGpu::Map& m = run == 0 ? mGpu : mCpu;
        g_dev->BeginScene();
        if (eng.Begin(g_dev)) {
            ok = eng.Accumulate(g_dev, m, outdoor, no, p, true, err) && eng.AddKeptParts(g_dev, pvb, ranges, &kept, lawOk, thr, w, h, err, true) &&
                 eng.Compose(g_dev, m, nullptr, out, p, err);
            eng.End(g_dev);
        } else
            ok = false;
        g_dev->EndScene();
        ok = ok && RtLampGpu::Engine::ReadTexture(g_dev, out, w, h, got[run]);
    }
    long n = 0, alphaSame = 0, worst = 0, lit = 0;
    for (size_t i = 0; ok && i < got[0].size(); i++) {
        n++;
        const long da = std::labs(static_cast<long>(got[0][i] >> 24) - static_cast<long>(got[1][i] >> 24));
        alphaSame += da <= 1, worst = std::max(worst, da), lit += (got[1][i] >> 24) != 0;
    }
    if (mGpu.vb) mGpu.vb->Release();
    if (mCpu.vb) mCpu.vb->Release();
    if (mCpu.dyn) mCpu.dyn->Release();
    if (pvb) pvb->Release();
    if (out) out->Release();
    Check(ok, "mask after kept lamps: " + err);
    std::printf("  %ld masked texels, 3 outdoor lamps and a kept lamp: the GPU's \"all\" against the CPU's: %ld within 1/255 of alpha (largest %ld), %ld with an alpha\n", n,
                alphaSame, worst, lit);
    Check(n > 0 && alphaSame == n && lit > n / 2, "room 0's floor mask with kept lamps: MaskAll evaluates the outdoor batch's lamps (within 1/255)");
    return true;
}

// ---------------------------------------------------------------- 8. physical indoor lamps (RelightKeptPhys, AccPhysPS)
// A roofed room (IndoorRooms' room A, story 0: a window in its outer wall, a door to room B, a stairwell hole and a
// double-height part with no ceiling and no floor above) and the room-0 ground outside it, its furniture as the game's 3D
// test records (rectangles and discs, one plane and two, behind 300 empty records and 300 blockers the segments never
// reach: more than a ps_3_0 loop's 255 steps), lit by a bare bulb, a spot, a shaded lamp, a street lamp outside, a TubeLight,
// a rectangle area light, a lamp of room B (its room row: mult x the norm scale) and a lamp upstairs. On the GPU: the kept
// shadow tiles, AccPhysPS's debug output per lamp (the shadow share, Occlude3D) and RelightKeptPhys (the sum, KeptTexel's
// compose, the class-2 blur). On the CPU: RoomLightTests::occlude, RtLampProxy::Visibility, the law, the drop and the soft
// light cut, KeptTexel, the pack and BlurBlock.
bool PhysKeptCheck() {
    std::printf("\n[physical indoor lamps] kept texels relit from the lamps on the GPU (cube shadows x the game's 3D test) against the CPU\n");
    using namespace RtLampProxy;
    Scene sc = IndoorRooms();
    for (Story& st : sc.lot.stories) {
        if (st.story == 0)
            for (Edge& e : st.edges)
                if (e.x0 == 4.0f && e.x1 == 4.0f && e.z0 == 7.0f) { // a window in room A's outer wall
                    e.open = true, e.o.u0 = 0.1f, e.o.u1 = 0.9f, e.o.sill = 0.9f;
                    for (float& h : e.o.head) h = 2.1f;
                }
        const auto hole = [&](int x0, int z0, int x1, int z1) { // no ceiling over story 0, no floor placed on story 1
            for (int z = z0; z <= z1; z++)
                for (int x = x0; x <= x1; x++) {
                    const size_t t = static_cast<size_t>(z) * st.w + x;
                    if (st.story == 0) st.roofed[t] = 0, st.ceil[t] = kNoFloor;
                    else st.placed[t] = 0;
                }
        };
        hole(5, 9, 6, 10); // the stairwell
        hole(7, 5, 8, 7);  // the double-height part
    }
    Mesh mesh;
    Build(sc.lot, mesh, true);
    const auto inA = [](const float* p) { return p[0] > 4.0f && p[0] < 9.0f && p[2] > 4.0f && p[2] < 12.0f && p[1] < 3.0f; };
    const auto roofedAt = [&](const float* p) {
        const Story& s0 = sc.lot.stories[0];
        const int x = static_cast<int>(std::floor(p[0])), z = static_cast<int>(std::floor(p[2]));
        return s0.roofed[static_cast<size_t>(z) * s0.w + x] != 0;
    };
    // the texels: room A's floor, walls and ceiling (slot 0; every tenth not tested), the ground outside and the outer face of
    // its west wall (slot 1, room 0)
    std::vector<Receiver> recv;
    for (const Edge& e : sc.lot.stories[0].edges)
        if (std::max(e.x0, e.x1) <= 9.0f) WallReceivers(recv, e, 0);
    FloorReceivers(recv, 4.0f, 4.0f, 9.0f, 12.0f, 0.0f, 0);
    FloorReceivers(recv, 0.0f, 4.0f, 4.0f, 12.0f, 0.0f, 0);
    for (float z = 4.125f; z < 12.0f; z += 0.25f)
        for (float x = 4.125f; x < 9.0f; x += 0.25f) {
            Receiver r{{x, 3.0f, z}, {0, -1, 0}, 1, 0};
            if (roofedAt(r.p)) recv.push_back(r);
        }
    struct Tx {
        float pos[3], nrm[3], base[3], S[3], p[4], norm, dither;
        int slot;
        bool tested;
    };
    std::mt19937 rng(4242);
    std::uniform_real_distribution<float> U(0.0f, 1.0f);
    std::vector<Tx> tx;
    for (const Receiver& r : recv) {
        const bool a = inA(r.p), out = r.p[0] < 4.0f;
        if (!a && !out) continue;
        Tx t{};
        std::memcpy(t.pos, r.p, sizeof t.pos), std::memcpy(t.nrm, r.n, sizeof t.nrm);
        t.slot = a ? 0 : 1;
        t.tested = a && tx.size() % 10 != 0;
        for (float& b : t.base) b = 0.1f * U(rng) * U(rng);
        t.norm = 0.6f + U(rng);
        tx.push_back(t);
    }
    const uint32_t count = static_cast<uint32_t>(tx.size());
    const int w = 64, h = static_cast<int>((count + w - 1) / w);
    for (uint32_t i = 0; i < count; i++) tx[i].dither = (i % 4 == 3) ? 0.0f : RoomMapDither::Offset(i % w, i / w) / 255.0f;
    // the lamps (lot space)
    const auto def = [](float x, float y, float z, int type) {
        LampDef d;
        d.pos[0] = x, d.pos[1] = y, d.pos[2] = z, d.type = type, d.range = 12.0f;
        if (type == 4) d.scale = 4.9f, d.off = -0.32f;
        return d;
    };
    std::vector<RtLampModel::Lamp> lamps = {ToLamp(def(6.5f, 2.95f, 8.0f, 3)), ToLamp(def(8.0f, 2.5f, 6.0f, 4)), ToLamp(def(5.0f, 1.4f, 5.0f, 5)),
                                            ToLamp(def(2.0f, 3.0f, 8.0f, 11)), ToLamp(def(4.3f, 1.8f, 10.0f, 3)), ToLamp(def(6.0f, 2.98f, 6.0f, 3)),
                                            ToLamp(def(12.0f, 0.9f, 10.0f, 3)), ToLamp(def(8.0f, 5.0f, 6.0f, 3))};
    const int nL = static_cast<int>(lamps.size());
    const char* names[] = {"bare bulb (3)", "spot (4)", "shaded lamp (5)", "street lamp outside (11)", "TubeLight", "rectangle area light", "lamp of room B", "lamp upstairs"};
    {
        const float s[3] = {0.0f, 0.0f, 1.0f};
        GpuLampLaw::TubeShape(lamps[4].law, s, 1.0f);
        const float u[3] = {1.0f, 0.0f, 0.0f}, v[3] = {0.0f, 0.0f, 1.0f}, nrm[3] = {0.0f, 1.0f, 0.0f};
        GpuLampLaw::AreaShape(lamps[5].law, u, v, nrm, 0.3f, 0.2f, false);
    }
    // the room table: room A (lamp of room B x its norm scale 0.8, no reach), room 0 (reach on its outer walls; none for the tube)
    // (the shaded lamp stands on furniture in room A: its 3D test takes the furniture fill there)
    std::vector<RtLampGpu::Engine::PhysRoom> rooms(2);
    for (int l = 0; l < nL; l++) {
        rooms[0].lamp.push_back({l == 6 ? 0.8f : 1.0f, 0.0f});
        rooms[1].lamp.push_back({1.0f, l == 4 ? 0.0f : 1.0f});
        rooms[0].fill.push_back(l == 2 ? RtLampGpu::kFurnitureFill : 0.0f), rooms[1].fill.push_back(0.0f);
    }
    // room A's 3D test records
    RoomLightTests::Room roomA;
    for (int i = 0; i < 300; i++) roomA.rec.push_back({{100.0f + i, 0.0f, 100.0f}, 0.01f, {}}); // (empty, far away)
    const auto rect = [](const float A1[3], const float A2[3], bool two, const float N[3], const float Uv[3], const float Vv[3], float hu, float hv) {
        RoomLightTests::Blocker k{};
        k.vt = RoomLightTests::kQuadVt;
        for (int c = 0; c < 3; c++)
            k.f[0x70 / 4 + c] = A1[c], k.f[0x80 / 4 + c] = A2[c], k.f[0x90 / 4 + c] = N[c], k.f[0xA0 / 4 + c] = Uv[c], k.f[0xB0 / 4 + c] = Vv[c], k.f[0xE0 / 4 + c] = 7.0f,
                                                                                                                                     k.f[0xF0 / 4 + c] = -3.0f;
        k.f[4] = two ? 1.0f : 0.0f;
        k.f[0xD0 / 4] = k.f[0xD8 / 4] = hu, k.f[0xD4 / 4] = k.f[0xDC / 4] = hv;
        return k;
    };
    const auto disc = [](const float c1[3], const float c2[3], bool two, float r2) {
        RoomLightTests::Blocker k{};
        k.vt = RoomLightTests::kDiscVt;
        for (int c = 0; c < 3; c++) k.f[0x20 / 4 + c] = c1[c], k.f[0x30 / 4 + c] = c2[c], k.f[0x80 / 4 + c] = 7.0f, k.f[0x90 / 4 + c] = -3.0f;
        k.f[0x40 / 4 + 1] = 1.0f;
        k.f[4] = two ? 1.0f : 0.0f;
        k.f[0x70 / 4] = r2;
        return k;
    };
    const float up[3] = {0, 1, 0}, ex[3] = {1, 0, 0}, ez[3] = {0, 0, 1};
    {
        RoomLightTests::Record unreached{{6.5f, 1.5f, 8.0f}, 100.0f, {}};
        const float A[3] = {6.5f, 50.0f, 8.0f};
        for (int i = 0; i < 300; i++) unreached.b.push_back(rect(A, A, false, up, ex, ez, 0.5f, 0.5f)); // (a plane at 50 m: never reached)
        roomA.rec.push_back(unreached);
        const float t1[3] = {6.5f, 0.75f, 8.0f};
        roomA.rec.push_back({{6.5f, 0.75f, 8.0f}, 1.0f, {rect(t1, t1, false, up, ex, ez, 0.6f, 0.4f)}}); // a table top
        const float c1[3] = {5.0f, 1.0f, 6.0f}, c2[3] = {5.6f, 1.0f, 6.0f};
        roomA.rec.push_back({{5.3f, 1.0f, 6.0f}, 2.0f, {rect(c1, c2, true, ex, up, ez, 1.0f, 0.5f)}}); // a cupboard's two sides
        const float d1[3] = {7.5f, 0.7f, 10.5f};
        roomA.rec.push_back({{7.5f, 0.7f, 10.5f}, 0.5f, {disc(d1, d1, false, 0.25f)}}); // a round table
        const float s1[3] = {8.0f, 1.0f, 5.0f}, s2[3] = {8.0f, 1.2f, 5.0f};
        roomA.rec.push_back({{8.0f, 1.1f, 5.0f}, 0.3f, {disc(s1, s2, true, 0.09f)}}); // a stool's two discs
    }
    // (world space, where the game's 3D test runs: the lot moved by kWorld; the records, the points and the lamps' +0x120 there)
    const float kWorld[3] = {100.5f, 0.0f, 200.25f};
    for (RoomLightTests::Record& r : roomA.rec) {
        r.c = {r.c.x + kWorld[0], r.c.y + kWorld[1], r.c.z + kWorld[2]};
        for (RoomLightTests::Blocker& k : r.b)
            for (const int off : {0x20, 0x30, 0x70, 0x80}) // (the planes' points: rectangles +0x70, +0x80; discs +0x20, +0x30)
                if ((off < 0x70) == (k.vt == RoomLightTests::kDiscVt))
                    for (int c = 0; c < 3; c++) k.f[off / 4 + c] += kWorld[c];
    }
    std::vector<std::array<float, 3>> world(nL);
    for (int l = 0; l < nL; l++)
        for (int c = 0; c < 3; c++) world[l][c] = lamps[l].law.pos[c] + kWorld[c];
    const auto worldOf = [&](const Tx& t) { return RoomLightTests::V{t.pos[0] + kWorld[0], t.pos[1] + kWorld[1], t.pos[2] + kWorld[2]}; };
    RtLampGpu::BlockerTable table;
    // (BlockerTable::Offset, a table dragged with its lamp) the table top appended where a solve read it, 0.4 m off, and carried
    // back by its record's offset: Occlude3D tests the segment moved back as RoomLightTests::occlude tests roomA
    RoomLightTests::Room read = roomA;
    read.rec[1].c.x -= 0.4f, read.rec[1].c.z += 0.3f;
    for (RoomLightTests::Blocker& k : read.rec[1].b)
        for (const int off : {0x70, 0x80}) k.f[off / 4] -= 0.4f, k.f[off / 4 + 2] += 0.3f;
    rooms[0].header = static_cast<float>(table.Append(read));
    table.Offset(static_cast<int>(rooms[0].header), 1, 0.4f, -0.3f);
    // the phys points (the sum's layout = the map's: texel i at (i % w, i / w))
    RtLampGpu::KeptMapData d;
    d.w = w, d.h = h, d.accW = w, d.accH = h;
    for (uint32_t i = 0; i < count; i++) {
        RtLampGpu::TexelVertex v{};
        v.x = static_cast<float>(i % w), v.y = static_cast<float>(i / w), v.w = 1.0f;
        for (int q = 0; q < 3; q++) v.p[q] = v.mask[q] = tx[i].pos[q], v.n[q] = tx[i].nrm[q], v.base[q] = tx[i].pos[q] + kWorld[q];
        v.p[3] = static_cast<float>((tx[i].tested ? 8 : 0) + 16 * tx[i].slot);
        d.phys.push_back(v);
    }
    // the GPU: shadow tiles, then each lamp alone at every point: its 3D test (T3dPS) and, at untested copies of the points with
    // mult 1 and no light cut, law x its shadow share (AccPhysPS)
    RtLampGpu::Engine eng(RtLampGpu::PlainCalls());
    std::string err;
    std::vector<int> tiles(nL);
    for (int l = 0; l < nL; l++) tiles[l] = l;
    RtLampGpu::Params prm;
    prm.thr = 3.0f / 255.0f;
    bool ok = eng.Init(g_dev, g_shaders, 256, err) && eng.SetProxy(g_dev, RtLampProxy::Mesh{}, err) && eng.SetProxy(g_dev, mesh, err, true); // (as in game: the tiles' own occluders)
    std::vector<std::vector<float>> t3gpu(nL), lawSh(nL);
    std::vector<RtLampGpu::TexelVertex> untested = d.phys;
    for (RtLampGpu::TexelVertex& v : untested) v.p[3] = static_cast<float>(16 * (static_cast<int>(v.p[3]) / 16));
    IDirect3DVertexBuffer9 *pvb = nullptr, *uvb = nullptr;
    ok = ok && RtLampGpu::Engine::MakeVb(g_dev, d.phys.data(), count, &pvb) && RtLampGpu::Engine::MakeVb(g_dev, untested.data(), count, &uvb);
    g_dev->BeginScene();
    if (ok && eng.Begin(g_dev)) {
        ok = eng.EnsureTiles(g_dev, nL, err) && eng.SetBlockers(g_dev, table, err);
        for (int l = 0; ok && l < nL; l++) ok = eng.RenderShadowTile(g_dev, lamps[l].law.pos, l, 64.0f, err);
        eng.End(g_dev);
    } else
        ok = false;
    g_dev->EndScene();
    for (int l = 0; ok && l < nL; l++) {
        std::vector<float> rows(rooms.size() * RtLampGpu::Engine::kTableCols * 4, 0.0f);
        for (size_t r = 0; r < rooms.size(); r++) {
            rows[r * RtLampGpu::Engine::kTableCols * 4] = 1.0f, rows[r * RtLampGpu::Engine::kTableCols * 4 + 1] = rooms[r].lamp[l][1];
            rows[(r * RtLampGpu::Engine::kTableCols + 16) * 4] = rooms[r].header;
        }
        RtLampGpu::Params pd = prm;
        pd.thr = 0.0f, pd.fade = false;
        for (const bool tested : {true, false}) {
            RtLampGpu::Map dm;
            dm.w = w, dm.h = h, dm.vb = tested ? pvb : uvb, dm.count = count;
            g_dev->BeginScene();
            eng.Begin(g_dev);
            ok = eng.SetTable(g_dev, rows.data(), static_cast<int>(rooms.size()), err) && eng.AccumulateRlg(g_dev, dm, &lamps[l], &tiles[l], 1, pd, true, err, true, reinterpret_cast<const float (*)[3]>(world[l].data()));
            eng.End(g_dev);
            g_dev->EndScene();
            ok = ok && (tested ? eng.ReadT3(g_dev, w, h, t3gpu[l]) : eng.ReadSum(g_dev, w, h, lawSh[l]));
            if (!ok) break;
        }
    }
    // A vertical tube: T3dPS is the mean of the 3D tests from its 4 points
    if (ok && nL > 0) {
        auto tube = lamps[0];
        const float up[3] = {0.0f, 1.0f, 0.0f};
        GpuLampLaw::TubeShape(tube.law, up, 1.0f);
        float low = world[0][1];
        const float step = GpuLampLaw::TubeStep(tube.law, low);
        std::vector<float> rows(rooms.size() * RtLampGpu::Engine::kTableCols * 4, 0.0f), t3tube;
        for (size_t r = 0; r < rooms.size(); r++) {
            rows[r * RtLampGpu::Engine::kTableCols * 4] = 1.0f, rows[r * RtLampGpu::Engine::kTableCols * 4 + 1] = rooms[r].lamp[0][1];
            rows[(r * RtLampGpu::Engine::kTableCols + 16) * 4] = rooms[r].header;
        }
        RtLampGpu::Params pd = prm;
        pd.thr = 0.0f, pd.fade = false;
        RtLampGpu::Map dm;
        dm.w = w, dm.h = h, dm.vb = pvb, dm.count = count;
        g_dev->BeginScene();
        eng.Begin(g_dev);
        ok = eng.SetTable(g_dev, rows.data(), static_cast<int>(rooms.size()), err) && eng.AccumulateRlg(g_dev, dm, &tube, &tiles[0], 1, pd, true, err, true, reinterpret_cast<const float (*)[3]>(world[0].data()));
        eng.End(g_dev);
        g_dev->EndScene();
        ok = ok && eng.ReadT3(g_dev, w, h, t3tube);
        float worst = 0.0f;
        int moved = 0, unk = 0;
        for (uint32_t i = 0; ok && i < count; i++)
            if (tx[i].tested) {
                float mean = 0.0f;
                for (int q = 0; q < GpuLampLaw::kTubeTests; q++) mean += RoomLightTests::occlude(roomA, {world[0][0], low + step * q, world[0][2]}, worldOf(tx[i]), &unk);
                mean /= GpuLampLaw::kTubeTests;
                worst = std::max(worst, std::fabs(t3tube[i * 4] - mean));
                moved += t3tube[i * 4] != t3gpu[0][i * 4];
            }
        Check(ok && step > 0.0f && moved > 0 && worst <= 0.02f, "physical: a vertical tube's 3D test is more than 0.02 from the mean of its 4 points");
    }
    // (legs, PostT) the table top's record with four legs: T3dPS against BlockerTable::Occlude; a lamp 1.2 m up shades some floor more than
    // the top alone, a ceiling lamp at 2.6 m a little more, a lamp 4 m up exactly as the top alone
    if (ok) {
        RoomLightTests::Room legged = roomA;
        RoomLightTests::Record& top = legged.rec[301];
        for (const float sx : {-0.5f, 0.5f})
            for (const float sz : {-0.3f, 0.3f}) top.posts.push_back({{top.c.x + sx, 0.03f, top.c.z + sz}, {top.c.x + sx, 0.72f, top.c.z + sz}, 0.03f});
        RtLampGpu::BlockerTable withLegs, without;
        const int hl = withLegs.Append(legged), hn = without.Append(roomA);
        std::vector<float> rows(rooms.size() * RtLampGpu::Engine::kTableCols * 4, 0.0f), t3;
        for (size_t r = 0; r < rooms.size(); r++) {
            rows[r * RtLampGpu::Engine::kTableCols * 4] = 1.0f, rows[r * RtLampGpu::Engine::kTableCols * 4 + 1] = rooms[r].lamp[0][1];
            rows[(r * RtLampGpu::Engine::kTableCols + 16) * 4] = static_cast<float>(hl);
        }
        RtLampGpu::Params pd = prm;
        pd.thr = 0.0f, pd.fade = false;
        RtLampGpu::Map dm;
        dm.w = w, dm.h = h, dm.vb = pvb, dm.count = count;
        for (const float y : {1.2f, 2.6f, 4.0f}) {
            const float at[3] = {5.6f + kWorld[0], y, 8.0f + kWorld[2]};
            g_dev->BeginScene();
            eng.Begin(g_dev);
            ok = ok && eng.SetBlockers(g_dev, withLegs, err) && eng.SetTable(g_dev, rows.data(), static_cast<int>(rooms.size()), err) && eng.AccumulateRlg(g_dev, dm, &lamps[0], &tiles[0], 1, pd, true, err, true, reinterpret_cast<const float (*)[3]>(at));
            eng.End(g_dev);
            g_dev->EndScene();
            ok = ok && eng.ReadT3(g_dev, w, h, t3);
            float worst = 0.0f;
            int moved = 0;
            for (uint32_t i = 0; ok && i < count; i++)
                if (tx[i].tested) {
                    const RoomLightTests::V lamp{at[0], at[1], at[2]};
                    const float cpu = withLegs.Occlude(hl, lamp, worldOf(tx[i]), prm.edgeR, prm.radius);
                    worst = std::max(worst, std::fabs(t3[i * 4] - cpu));
                    moved += cpu != without.Occlude(hn, lamp, worldOf(tx[i]), prm.edgeR, prm.radius);
                }
            Check(ok && worst <= 0.02f && (y < 3.0f ? moved > 0 : moved == 0), "physical: a table's legs on the GPU are more than 0.02 from BlockerTable::Occlude, or a lamp past the gate sees them");
        }
        // (sphere_cut_fade) the same table with its bands' fade: T3dPS against BlockerTable::Occlude, the floor moved from the game's step
        {
            RtLampGpu::BlockerTable faded = withLegs;
            faded.FadeCut(hl);
            const float at[3] = {5.6f + kWorld[0], 2.6f, 8.0f + kWorld[2]};
            g_dev->BeginScene();
            eng.Begin(g_dev);
            ok = ok && eng.SetBlockers(g_dev, faded, err) && eng.SetTable(g_dev, rows.data(), static_cast<int>(rooms.size()), err) && eng.AccumulateRlg(g_dev, dm, &lamps[0], &tiles[0], 1, pd, true, err, true, reinterpret_cast<const float (*)[3]>(at));
            eng.End(g_dev);
            g_dev->EndScene();
            ok = ok && eng.ReadT3(g_dev, w, h, t3);
            float worst = 0.0f;
            int moved = 0;
            for (uint32_t i = 0; ok && i < count; i++)
                if (tx[i].tested) {
                    const RoomLightTests::V lamp{at[0], at[1], at[2]};
                    const float cpu = faded.Occlude(hl, lamp, worldOf(tx[i]), prm.edgeR, prm.radius);
                    worst = std::max(worst, std::fabs(t3[i * 4] - cpu));
                    moved += cpu != withLegs.Occlude(hl, lamp, worldOf(tx[i]), prm.edgeR, prm.radius);
                }
            std::printf("  sphere fade: %d tested points moved from the step, GPU within %.6f of BlockerTable::Occlude\n", moved, worst);
            Check(ok && worst <= 0.02f && moved > 0, "physical: the sphere's fade on the GPU is more than 0.02 from BlockerTable::Occlude, or moves nothing");
        }
        // (legs under a high lamp) on the floor around the table, the legs' share (with / without) on a 6.25 cm grid: how deep it goes, how hard its
        // edge is, and that nothing but the legs' own streaks moves
        {
            const RoomLightTests::Record& rc = legged.rec[301];
            const auto legsAt = [&](float lampY, float off, float& deep, float& edge, float& topEdge, float& moved) {
                const RoomLightTests::V lamp{rc.c.x + off, lampY, rc.c.z};
                constexpr int kN = 64;
                constexpr float kStep = 0.0625f;
                std::vector<float> share(kN * kN), own(kN * kN);
                deep = edge = topEdge = moved = 0.0f;
                for (int z = 0; z < kN; z++)
                    for (int x = 0; x < kN; x++) {
                        const RoomLightTests::V p{rc.c.x + (x - kN / 2) * kStep, 0.0f, rc.c.z + (z - kN / 2) * kStep};
                        const float t = without.Occlude(hn, lamp, p, prm.edgeR, prm.radius), l = withLegs.Occlude(hl, lamp, p, prm.edgeR, prm.radius);
                        share[static_cast<size_t>(z) * kN + x] = t - l, own[static_cast<size_t>(z) * kN + x] = t;
                        deep = std::max(deep, t - l);
                        moved += t != l ? kStep * kStep : 0.0f;
                    }
                for (int z = 0; z + 1 < kN; z++)
                    for (int x = 0; x + 1 < kN; x++) {
                        const size_t i = static_cast<size_t>(z) * kN + x;
                        edge = std::max({edge, std::fabs(share[i + 1] - share[i]), std::fabs(share[i + kN] - share[i])});
                        topEdge = std::max({topEdge, std::fabs(own[i + 1] - own[i]), std::fabs(own[i + kN] - own[i])});
                    }
            };
            float baseEdge = 0.0f, deep, edge, topEdge, moved;
            legsAt(rc.c.y * 0.0f + 0.72f + RtLampGpu::kPostHighM, 1.0f, deep, baseEdge, topEdge, moved); // (the highest lamp that shaded legs before)
            for (const float off : {0.0f, 1.0f})
                for (const float y : {1.2f, 2.2f, 2.4f, 2.6f, 2.8f, 3.2f, 3.6f}) {
                    legsAt(y, off, deep, edge, topEdge, moved);
                    std::printf("  legs of a 0.72 m table, lamp at %.1f m, %.0f m aside: deepest %.3f, hardest step a 6.25 cm texel %.3f (the top's own %.3f), area %.2f m2\n", y, off, deep, edge, topEdge, moved);
                    if (y > 0.72f + RtLampGpu::kPostHighM) Check(edge <= baseEdge * 1.01f + 1e-4f, "physical: legs under a high lamp are harder than those of the highest lamp that shaded them before");
                    if (y > 0.72f + RtLampGpu::kPostHighM) Check(edge <= topEdge, "physical: legs under a high lamp are harder than the top's own soft edge");
                }
            float a1, b1, a2, b2, c1, d1;
            legsAt(0.72f + RtLampGpu::kPostHighM - 0.001f, 1.0f, a1, b1, c1, d1), legsAt(0.72f + RtLampGpu::kPostHighM + 0.001f, 1.0f, a2, b2, c1, d1);
            Check(std::fabs(a1 - a2) < 0.01f, "physical: legs jump where a lamp passes kPostHighM");
            legsAt(0.72f + RtLampGpu::kPostGateM - 0.001f, 1.0f, a1, b1, c1, d1);
            Check(a1 < 0.001f, "physical: legs jump where a lamp passes kPostGateM");
            Check(1.75f * (RtLampGpu::kPostFloorW + RtLampGpu::kPostHighW) <= RtLampGpu::kPostReach + 1e-6f, "physical: a leg's widest edge passes the record's sphere");
        }
        // (the added cost) the same room as a furnished one: 16 tables' legs on every record near the texels, a lamp under the old gate's reach
        // (legs walked, as before), at 2.6 m (walked now) and at 4 m (skipped)
        {
            RoomLightTests::Room bench = roomA;
            for (int k = 0; k < 16; k++)
                for (const float sx : {-0.4f, 0.4f})
                    for (const float sz : {-0.3f, 0.3f}) {
                        const float cx = 3.0f + (k % 4) * 2.0f + sx, cz = 4.0f + (k / 4) * 2.0f + sz;
                        bench.rec[301].posts.push_back({{cx, 0.03f, cz}, {cx, 0.72f, cz}, 0.03f});
                    }
            RtLampGpu::BlockerTable benchLegs;
            const int hb = benchLegs.Append(bench);
            std::vector<float> rowsB(rooms.size() * RtLampGpu::Engine::kTableCols * 4, 0.0f), t3b;
            for (size_t r = 0; r < rooms.size(); r++) {
                rowsB[r * RtLampGpu::Engine::kTableCols * 4] = 1.0f, rowsB[r * RtLampGpu::Engine::kTableCols * 4 + 1] = rooms[r].lamp[0][1];
                rowsB[(r * RtLampGpu::Engine::kTableCols + 16) * 4] = static_cast<float>(hb);
            }
            RtLampGpu::Params pb = prm;
            pb.thr = 0.0f, pb.fade = false;
            RtLampGpu::Map bm;
            bm.w = w, bm.h = h, bm.vb = pvb, bm.count = count;
            const auto relight = [&](float y, const RtLampGpu::BlockerTable& t) {
                const float at[3] = {5.6f + kWorld[0], y, 8.0f + kWorld[2]};
                double best = 1e9;
                for (int rep = 0; rep < 15 && ok; rep++) {
                    g_dev->BeginScene();
                    eng.Begin(g_dev);
                    ok = eng.SetBlockers(g_dev, t, err) && eng.SetTable(g_dev, rowsB.data(), static_cast<int>(rooms.size()), err);
                    eng.End(g_dev);
                    g_dev->EndScene();
                    ok = ok && eng.ReadT3(g_dev, w, h, t3b);
                    const auto t0 = std::chrono::steady_clock::now();
                    g_dev->BeginScene();
                    eng.Begin(g_dev);
                    ok = ok && eng.AccumulateRlg(g_dev, bm, &lamps[0], &tiles[0], 1, pb, true, err, true, reinterpret_cast<const float (*)[3]>(at));
                    eng.End(g_dev);
                    g_dev->EndScene();
                    ok = ok && eng.ReadT3(g_dev, w, h, t3b);
                    best = std::min(best, std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count());
                }
                return best;
            };
            const double none = relight(2.6f, without), low = relight(1.2f, benchLegs), high = relight(2.6f, benchLegs), past = relight(4.0f, benchLegs);
            std::printf("  drag cost, %u texels, one lamp, 64 legs: no legs %.3f ms; legs: lamp at 1.2 m %.3f ms, 2.6 m (new) %.3f ms, 4 m (past the gate) %.3f ms\n", count, none, low, high, past);
        }
        g_dev->BeginScene();
        eng.Begin(g_dev);
        ok = ok && eng.SetBlockers(g_dev, table, err);
        eng.End(g_dev);
        g_dev->EndScene();
    }
    // (review fixes) a shared blocker table is uploaded once while bound and again after another one; an empty open-top proxy
    // leaves the tiles without occluders (never the outdoor proxy). Lamp 0, then the scene as it was
    if (ok) {
        const auto pass = [&](bool tested, std::vector<float>& out) {
            std::vector<float> rows(rooms.size() * RtLampGpu::Engine::kTableCols * 4, 0.0f);
            for (size_t r = 0; r < rooms.size(); r++) {
                rows[r * RtLampGpu::Engine::kTableCols * 4] = 1.0f, rows[r * RtLampGpu::Engine::kTableCols * 4 + 1] = rooms[r].lamp[0][1];
                rows[(r * RtLampGpu::Engine::kTableCols + 16) * 4] = rooms[r].header;
            }
            RtLampGpu::Params pd = prm;
            pd.thr = 0.0f, pd.fade = false;
            RtLampGpu::Map dm;
            dm.w = w, dm.h = h, dm.vb = tested ? pvb : uvb, dm.count = count;
            g_dev->BeginScene();
            eng.Begin(g_dev);
            bool k = eng.SetTable(g_dev, rows.data(), static_cast<int>(rooms.size()), err) &&
                     eng.AccumulateRlg(g_dev, dm, &lamps[0], &tiles[0], 1, pd, true, err, true, reinterpret_cast<const float (*)[3]>(world[0].data()));
            eng.End(g_dev);
            g_dev->EndScene();
            return k && (tested ? eng.ReadT3(g_dev, w, h, out) : eng.ReadSum(g_dev, w, h, out));
        };
        const auto shared = std::make_shared<const RtLampGpu::BlockerTable>(table), none = std::make_shared<const RtLampGpu::BlockerTable>();
        std::vector<float> a1, a2, b, a3;
        bool k = eng.SetBlockers(g_dev, shared, err) && pass(true, a1) && eng.SetBlockers(g_dev, shared, err) && pass(true, a2) && eng.SetBlockers(g_dev, none, err) &&
                 pass(true, b) && eng.SetBlockers(g_dev, shared, err) && pass(true, a3);
        bool same = k, open = k, shadedBefore = false;
        for (uint32_t i = 0; k && i < count; i++)
            if (tx[i].tested) {
                same = same && a1[i * 4] == t3gpu[0][i * 4] && a2[i * 4] == t3gpu[0][i * 4] && a3[i * 4] == t3gpu[0][i * 4];
                open = open && b[i * 4] == 1.0f;
            }
        Check(same && open, "physical: a shared blocker table is bound once and uploaded again after another: " + err);
        std::vector<float> clear;
        g_dev->BeginScene();
        k = eng.SetProxy(g_dev, mesh, err) && eng.SetProxy(g_dev, RtLampProxy::Mesh{}, err, true) && eng.Begin(g_dev);
        if (k) k = eng.RenderShadowTile(g_dev, lamps[0].law.pos, 0, 64.0f, err), eng.End(g_dev);
        g_dev->EndScene();
        k = k && pass(false, clear);
        bool whole = k;
        for (uint32_t i = 0; k && i < count; i++) {
            float law[3];
            GpuLampLaw::Eval(lamps[0].law, tx[i].pos, tx[i].nrm, GpuLampLaw::WallReach(tx[i].nrm, prm.reachOn) * rooms[tx[i].slot].lamp[0][1], law);
            const float ls = (law[2] + law[1]) + law[0];
            if (!(ls > 1e-4f)) continue;
            const float was = ((lawSh[0][i * 4 + 2] + lawSh[0][i * 4 + 1]) + lawSh[0][i * 4]) / ls, now = ((clear[i * 4 + 2] + clear[i * 4 + 1]) + clear[i * 4]) / ls;
            shadedBefore = shadedBefore || was < 0.5f;
            whole = whole && now > 0.99f;
        }
        Check(whole && shadedBefore, "physical: an empty open-top proxy leaves the tiles without occluders, not the outdoor proxy's: " + err);
        g_dev->BeginScene();
        ok = eng.SetProxy(g_dev, RtLampProxy::Mesh{}, err) && eng.SetProxy(g_dev, mesh, err, true) && eng.Begin(g_dev);
        if (ok) ok = eng.RenderShadowTile(g_dev, lamps[0].law.pos, 0, 64.0f, err) && eng.SetBlockers(g_dev, table, err), eng.End(g_dev);
        g_dev->EndScene();
    }
    if (pvb) pvb->Release();
    if (uvb) uvb->Release();
    Check(ok, "physical: the per-lamp passes: " + err);
    if (!ok) return false;
    const auto lawOf = [&](int l, const Tx& t, float out[3]) {
        const float reach = rooms[t.slot].lamp[l][1];
        GpuLampLaw::Eval(lamps[l].law, t.pos, t.nrm, GpuLampLaw::WallReach(t.nrm, prm.reachOn) * reach, out);
    };
    // the GPU's shadow share of lamp l at point i (-1: its law adds nothing there)
    const auto shareOf = [&](int l, uint32_t i) {
        float law[3];
        lawOf(l, tx[i], law);
        const float ls = (law[2] + law[1]) + law[0];
        const float* g = &lawSh[l][i * 4];
        return ls > 0.0f ? ((g[2] + g[1]) + g[0]) / ls : -1.0f;
    };
    // (a) Occlude3D against RoomLightTests::occlude, (b) the shadow share against the reference, and the reference's shares
    long t3n = 0, t3exact = 0, t3shaded = 0, shN = 0, shWithin2 = 0;
    float t3worst = 0.0f;
    int unknown = 0;
    std::vector<uint32_t> stamp;
    uint32_t mark = 0;
    std::vector<std::vector<uint8_t>> refClear(nL, std::vector<uint8_t>(count, 0)); // the reference sees lamp l whole at point i
    for (int l = 0; l < nL; l++) {
        long n = 0, w2 = 0;
        for (uint32_t i = 0; i < count; i++) {
            const Tx& t = tx[i];
            const float sh = shareOf(l, i);
            if (t.tested) {
                const float cpu = RoomLightTests::occlude(roomA, {world[l][0], world[l][1], world[l][2]}, worldOf(t), &unknown);
                const float g3 = t3gpu[l][i * 4];
                t3n++, t3exact += g3 == cpu, t3shaded += cpu < 1.0f;
                t3worst = std::max(t3worst, std::fabs(g3 - cpu));
            }
            if (sh < 0.0f || rooms[t.slot].lamp[l][0] == 0.0f) continue; // (the lamp adds nothing here)
            float law[3];
            lawOf(l, t, law);
            float ref = Visibility(mesh, lamps[l].law.pos, prm.radius, t.pos, 8, &stamp, &mark);
            if (ref > 0.0f && ref < 1.0f) ref = Visibility(mesh, lamps[l].law.pos, prm.radius, t.pos, kRefSamples, &stamp, &mark);
            refClear[l][i] = ref >= 1.0f;
            const float lawMax = std::min(std::max({law[0], law[1], law[2]}), 1.0f);
            n++, w2 += std::fabs(sh - ref) * lawMax * 255.0f <= 2.0f;
        }
        std::printf("  %-26s %5ld lit points: shadow share within 2/255 of the reference %6.2f%%\n", names[l], n, n ? 100.0 * w2 / n : 100.0);
        shN += n, shWithin2 += w2;
    }
    std::printf("  Occlude3D: %ld tested points x lamps, %ld shaded by the furniture, %.2f%% bit for bit, largest difference %.6f\n", t3n, t3shaded, t3n ? 100.0 * t3exact / t3n : 100.0,
                t3worst);
    Check(t3n > 0 && t3shaded > 0 && unknown == 0, "physical: the furniture shades no tested point (the scene is wrong)");
    Check(t3worst <= 0.02f, "physical: Occlude3D more than 0.02 from RoomLightTests::occlude");
    Check(shN > 0 && shWithin2 >= static_cast<long>(0.99 * shN), "physical: fewer than 99% of shadow shares within 2/255 of the reference");
    // the CPU's texels: the sum from each lamp's share (the GPU's shadow share; vis1: 1 where the reference sees the lamp whole) x
    // the 3D test, KeptTexel and the pack
    const float k = 0.62f, floorA = 5.0f / 255.0f;
    const auto sumAt = [&](uint32_t i, bool vis1, float R[3]) {
        const Tx& t = tx[i];
        for (int q = 0; q < 3; q++) R[q] = t.base[q];
        for (int l = 0; l < nL; l++) {
            const float mult = rooms[t.slot].lamp[l][0];
            float law[3];
            lawOf(l, t, law);
            if (mult == 0.0f || !((law[0] + law[1] + law[2]) > 0.0f)) continue;
            float sh = vis1 && refClear[l][i] ? 1.0f : shareOf(l, i);
            if (t.tested) {
                const RoomLightTests::V head{world[l][0], world[l][1], world[l][2]};
                const float fill = rooms[t.slot].fill[l];
                const float prod = sh * RtLampGpu::FurnitureFill(fill > 0.0f ? table.OccludeFill(static_cast<int>(rooms[t.slot].header), head, worldOf(t), prm.radius) : RoomLightTests::occlude(roomA, head, worldOf(t), &unknown), fill);
                sh = RoomLightTests::K_DROP > prod ? 0.0f : prod;
            }
            float c[3] = {law[0] * sh, law[1] * sh, law[2] * sh};
            RtLampModel::Fade(c, prm.thr, prm.fade);
            for (int q = 0; q < 3; q++) R[q] += mult * c[q];
        }
    };
    std::vector<uint32_t> cpuPacked(count), cpuClear(count), cpuSoft(count);
    for (uint32_t i = 0; i < count; i++) {
        Tx& t = tx[i];
        float R[3], R1[3];
        sumAt(i, false, R);
        sumAt(i, true, R1);
        const bool roofed = t.slot == 0;
        for (int q = 0; q < 3; q++) t.S[q] = std::max(R[q] * (0.7f + 0.6f * U(rng)), 0.0f); // (the solve as it was)
        if (roofed) {
            float x[3], c[3];
            for (int q = 0; q < 3; q++) x[q] = t.norm * t.S[q];
            GpuLampLaw::Tone(x, c);
            const float a = (0.6f + 0.4f * U(rng)) * GpuLampLaw::Weight(c, k);
            const float amb[3] = {0.3f * U(rng), 0.3f * U(rng), 0.3f * U(rng)};
            for (int q = 0; q < 3; q++) t.p[q] = c[q] + (i % 2 ? a * amb[q] : 0.0f);
            t.p[3] = a;
        } else {
            for (int q = 0; q < 3; q++) t.p[q] = std::min(t.S[q], 1.0f);
            t.p[3] = U(rng);
        }
        float c[4];
        RtLampModel::KeptTexel(R, t.S, t.p, t.norm, k, roofed, roofed ? floorA : 0.0f, t.dither, c);
        cpuPacked[i] = OutdoorDeltaModel::PackModel(c);
        RtLampModel::KeptTexel(R, t.S, t.p, t.norm, k, roofed, roofed ? floorA : 0.0f, t.dither, c, nullptr, true);
        cpuSoft[i] = OutdoorDeltaModel::PackModel(c);
        RtLampModel::KeptTexel(R1, t.S, t.p, t.norm, k, roofed, roofed ? floorA : 0.0f, t.dither, c);
        cpuClear[i] = OutdoorDeltaModel::PackModel(c);
    }
    const auto vertexOf = [&](uint32_t i, int x, int y) {
        const Tx& t = tx[i];
        RtLampGpu::TexelVertex v{};
        v.x = static_cast<float>(x), v.y = static_cast<float>(y), v.w = 1.0f;
        for (int q = 0; q < 3; q++) v.p[q] = t.base[q];
        v.p[3] = t.dither;
        if (t.slot == 0) {
            float xs[3], rampAmb[3] = {}, ramp = 0.0f;
            for (int q = 0; q < 3; q++) xs[q] = t.norm * t.S[q];
            if (!GpuLampLaw::IndoorSplit(xs, t.p, k, rampAmb, ramp)) rampAmb[0] = rampAmb[1] = rampAmb[2] = ramp = 0.0f;
            for (int q = 0; q < 3; q++) v.n[q] = rampAmb[q];
            v.n[3] = ramp;
        } else
            v.n[3] = t.p[3];
        v.base[0] = t.norm, v.base[1] = k, v.base[2] = t.slot == 0 ? floorA : 0.0f, v.base[3] = t.slot == 0 ? 1.0f : 0.0f;
        v.mask[0] = static_cast<float>(i % w), v.mask[1] = static_cast<float>(i / w);
        return v;
    };
    // (c) RelightKeptPhys: a floor map and a class-2 wall atlas (one block of 8 rows after another, blurred twice)
    long n = 0, within1 = 0, maxDiff = 0, clearN = 0, clearWithin1 = 0, clearMax = 0, untouchedBad = 0;
    double gpuMs = 0.0;
    for (const int m : {1, 0}) {
        RtLampGpu::KeptMapData dk = d;
        dk.blur = m == 0;
        std::vector<uint32_t> gamePx(static_cast<size_t>(w) * h);
        for (uint32_t& v : gamePx) v = static_cast<uint32_t>(rng());
        std::vector<uint32_t> expect = gamePx;
        if (m == 1)
            for (uint32_t i = 0; i < count; i++) {
                dk.texels.push_back(vertexOf(i, i % w, i / w));
                expect[i] = cpuPacked[i];
            }
        else {
            dk.passes = 2, dk.layoutW = w, dk.layoutH = h;
            dk.layout = gamePx;
            for (int y0 = 0; y0 < h; y0 += 8) {
                const int y1 = std::min(y0 + 7, h - 1), rows = y1 - y0 + 1;
                std::vector<uint32_t> ext(static_cast<size_t>(rows) * w), before;
                for (int y = y0; y <= y1; y++)
                    for (int x = 0; x < w; x++) {
                        const uint32_t i = static_cast<uint32_t>(y * w + x);
                        uint32_t& e = ext[static_cast<size_t>(y - y0) * w + x];
                        e = gamePx[i];
                        if (i < count) e = cpuPacked[i], dk.wallTexels.push_back(vertexOf(i, x, y));
                    }
                before = ext;
                OutdoorDeltaModel::BlurBlock(ext.data(), before.data(), w, rows, false, false, dk.passes);
                for (int y = y0; y <= y1; y++)
                    for (int x = 0; x < w; x++) expect[static_cast<size_t>(y) * w + x] = ext[static_cast<size_t>(y - y0) * w + x];
                dk.extRects.push_back(RtLampGpu::Block{0, y0, w - 1, y1});
                dk.outs.push_back(RtLampGpu::KeptBlockOut{0, y0, w - 1, y1, 0, y0, y1, false, false});
            }
        }
        IDirect3DTexture9 *game = nullptr, *out = nullptr;
        RtLampGpu::Engine::MakePreblur(g_dev, w, h, gamePx.data(), &game);
        g_dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &out, nullptr);
        RtLampGpu::KeptMapGpu g;
        ok = game && out && RtLampGpu::Engine::UploadKept(g_dev, dk, g);
        std::vector<uint32_t> got;
        const auto t0 = std::chrono::steady_clock::now();
        g_dev->BeginScene();
        if (ok && eng.Begin(g_dev)) {
            ok = eng.CopyTexture(g_dev, game, out, w, h, err) && eng.RelightKeptPhys(g_dev, g, lamps.data(), tiles.data(), reinterpret_cast<const float (*)[3]>(world.data()), nL, rooms, prm, game, out, w, h, err);
            eng.End(g_dev);
        } else
            ok = false;
        g_dev->EndScene();
        ok = ok && RtLampGpu::Engine::ReadTexture(g_dev, out, w, h, got);
        gpuMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (ok && m == 1) { // (tone_soft) the same map with ToneSoft: the compose against KeptTexel's
            RtLampGpu::Params soft = prm;
            soft.toneSoft = true;
            std::vector<uint32_t> gs;
            g_dev->BeginScene();
            if (eng.Begin(g_dev)) {
                ok = eng.CopyTexture(g_dev, game, out, w, h, err) && eng.RelightKeptPhys(g_dev, g, lamps.data(), tiles.data(), reinterpret_cast<const float (*)[3]>(world.data()), nL, rooms, soft, game, out, w, h, err);
                eng.End(g_dev);
            } else
                ok = false;
            g_dev->EndScene();
            ok = ok && RtLampGpu::Engine::ReadTexture(g_dev, out, w, h, gs);
            long softBad = 0, softMoved = 0;
            for (uint32_t i = 0; ok && i < count; i++) softBad += OutdoorDeltaModel::MaxLevelDiff(cpuSoft[i], gs[i]) > 2, softMoved += cpuSoft[i] != cpuPacked[i];
            // (this scene's lamps are far past the knee: faint texels of no lamp, a sweep over it)
            RtLampGpu::KeptMapData ds = dk;
            std::vector<uint32_t> want(count), game0(count);
            for (uint32_t i = 0; i < count; i++) {
                const float s = 0.06f * static_cast<float>(i) / static_cast<float>(count), Rs[3] = {s, 0.7f * s, 0.4f * s};
                const Tx& t = tx[i];
                float c[4];
                for (int q = 0; q < 3; q++) ds.texels[i].p[q] = Rs[q];
                RtLampModel::KeptTexel(Rs, t.S, t.p, t.norm, k, t.slot == 0, t.slot == 0 ? floorA : 0.0f, t.dither, c, nullptr, true);
                want[i] = OutdoorDeltaModel::PackModel(c);
                RtLampModel::KeptTexel(Rs, t.S, t.p, t.norm, k, t.slot == 0, t.slot == 0 ? floorA : 0.0f, t.dither, c);
                game0[i] = OutdoorDeltaModel::PackModel(c);
            }
            std::vector<RtLampGpu::Engine::PhysRoom> dark = rooms;
            for (auto& r : dark)
                for (auto& l : r.lamp) l[0] = 0.0f;
            RtLampGpu::KeptMapGpu gd;
            ok = ok && RtLampGpu::Engine::UploadKept(g_dev, ds, gd);
            g_dev->BeginScene();
            if (ok && eng.Begin(g_dev)) {
                ok = eng.CopyTexture(g_dev, game, out, w, h, err) && eng.RelightKeptPhys(g_dev, gd, lamps.data(), tiles.data(), reinterpret_cast<const float (*)[3]>(world.data()), nL, dark, soft, game, out, w, h, err);
                eng.End(g_dev);
            } else
                ok = false;
            g_dev->EndScene();
            ok = ok && RtLampGpu::Engine::ReadTexture(g_dev, out, w, h, gs);
            gd.Release();
            for (uint32_t i = 0; ok && i < count; i++) softBad += OutdoorDeltaModel::MaxLevelDiff(want[i], gs[i]) > 2, softMoved += want[i] != game0[i];
            std::printf("  ToneSoft: %ld texels change, %ld more than 2/255 from the CPU's\n", softMoved, softBad);
            Check(ok && softMoved > 0 && softBad <= static_cast<long>(count / 1000), "physical: ToneSoft's compose differs from the CPU's");
        }
        if (ok && m == 1) { // more lamps than the atlas holds (gpu_room_lamps.cpp RelightPhys): passes of 3 over a 3-row atlas, tiles rendered in turn
            std::vector<uint32_t> split;
            g_dev->BeginScene();
            if (eng.Begin(g_dev)) {
                ok = eng.EnsureTiles(g_dev, 3, err) && eng.CopyTexture(g_dev, game, out, w, h, err);
                for (int g0 = 0; ok && g0 < nL; g0 += 3) {
                    const int gn = std::min(3, nL - g0);
                    std::vector<int> rowOf(gn);
                    std::vector<RtLampGpu::Engine::PhysRoom> part(rooms.size());
                    for (size_t r = 0; r < rooms.size(); r++)
                        part[r].header = rooms[r].header, part[r].lamp.assign(rooms[r].lamp.begin() + g0, rooms[r].lamp.begin() + g0 + gn),
                        part[r].fill.assign(rooms[r].fill.begin() + g0, rooms[r].fill.begin() + g0 + gn);
                    for (int i = 0; ok && i < gn; i++) rowOf[i] = i, ok = eng.RenderShadowTile(g_dev, lamps[g0 + i].law.pos, i, 64.0f, err);
                    ok = ok && eng.RelightKeptPhys(g_dev, g, lamps.data() + g0, rowOf.data(), reinterpret_cast<const float (*)[3]>(world.data() + g0), gn, part, prm, game, out, w,
                                                   h, err, g0 == 0, g0 + 3 >= nL);
                }
                ok = ok && eng.EnsureTiles(g_dev, nL, err); // (back to a tile per lamp for the wall atlas)
                for (int l = 0; ok && l < nL; l++) ok = eng.RenderShadowTile(g_dev, lamps[l].law.pos, l, 64.0f, err);
                eng.End(g_dev);
            } else
                ok = false;
            g_dev->EndScene();
            ok = ok && RtLampGpu::Engine::ReadTexture(g_dev, out, w, h, split);
            size_t same = 0;
            for (size_t i = 0; ok && i < got.size(); i++) same += split[i] == got[i];
            std::printf("  RelightKeptPhys in passes of 3 lamps over a 3-row atlas: %zu of %zu texels as in one pass\n", same, got.size());
            Check(ok && same == got.size(), "physical: the passes over one sum give the one pass's map: " + err);
        }
        g.Release();
        if (game) game->Release();
        if (out) out->Release();
        Check(ok, "physical: RelightKeptPhys: " + err);
        if (!ok) return false;
        for (size_t i = 0; i < expect.size(); i++) {
            if (i >= count && m == 1) {
                untouchedBad += got[i] != gamePx[i];
                continue;
            }
            const int dl = OutdoorDeltaModel::MaxLevelDiff(expect[i], got[i]);
            n++, within1 += dl <= 1, maxDiff = std::max<long>(maxDiff, dl);
            bool clear = false;
            for (int l = 0; l < nL && i < count; l++) clear = clear || refClear[l][i];
            if (m == 1 && clear) { // (a lamp the reference sees whole there: the CPU with its share 1)
                const int dc = OutdoorDeltaModel::MaxLevelDiff(cpuClear[i], got[i]);
                clearN++, clearWithin1 += dc <= 1, clearMax = std::max<long>(clearMax, dc);
            }
        }
    }
    std::printf("  RelightKeptPhys (%u points, %d lamps, map %dx%d, a floor and a blurred wall atlas, %.1f ms with the read-backs): %ld texels, %.2f%% within 1/255 of the CPU, "
                "largest %ld/255; with the reference's unshadowed lamps at share 1: %ld texels, %.2f%% within 1/255, largest %ld/255; texels left as the game's changed: %ld\n",
                count, nL, w, h, gpuMs, n, n ? 100.0 * within1 / n : 100.0, maxDiff, clearN, clearN ? 100.0 * clearWithin1 / clearN : 100.0, clearMax, untouchedBad);
    Check(n > 0 && within1 >= static_cast<long>(0.999 * n) && maxDiff <= 2, "physical: the GPU's texels within 1/255 of the CPU's (99.9%), none past 2/255");
    Check(clearN > 0 && clearWithin1 >= static_cast<long>(0.995 * clearN), "physical: fewer than 99.5% of unshadowed texels within 1/255 of the CPU's");
    Check(untouchedBad == 0, "physical: a texel no kept texel holds changed");
    // (stage 4) the lamp of room B moved into room A with no solve, and a lamp of the lot far away: the lamps drawn are picked by
    // reach over the map's box (phys_lamp_pick.h), the moved one with a fresh row (registered in room A now: norm scale 1), the far
    // one culled. On the GPU: RelightKeptPhys of the picked lamps; on the CPU: every lamp (the far one too) with the reference's
    // shadow share x the 3D test, KeptTexel and the pack
    {
        std::vector<RtLampModel::Lamp> all = lamps;
        std::vector<std::array<float, 3>> rowA(nL), row0(nL);
        for (int l = 0; l < nL; l++) rowA[l] = rooms[0].lamp[l], row0[l] = rooms[1].lamp[l];
        all[6].law.pos[0] = 7.0f, all[6].law.pos[1] = 1.2f, all[6].law.pos[2] = 9.0f;
        rowA[6][0] = PhysLampPick::NormKFresh(0, 1, 0.9f, 0, 1, 0.9f, false); // (its home is room A now)
        all.push_back(ToLamp(def(60.0f, 1.0f, 60.0f, 3)));
        rowA.push_back({1.0f, 0.0f, 0.0f}), row0.push_back({1.0f, 1.0f, 0.0f});
        PhysLampPick::Box box;
        for (const Tx& t : tx) box.Grow(t.pos);
        std::vector<int> pick;
        for (int l = 0; l < static_cast<int>(all.size()); l++)
            if (PhysLampPick::Meets(box, all[l].law.pos, PhysLampPick::Reach2(all[l].law, std::max(rowA[l][0], row0[l][0]), prm.thr))) pick.push_back(l);
        const bool farCulled = std::find(pick.begin(), pick.end(), nL) == pick.end(), movedIn = std::find(pick.begin(), pick.end(), 6) != pick.end();
        const int np = static_cast<int>(pick.size());
        std::vector<RtLampModel::Lamp> pl;
        std::vector<std::array<float, 3>> pw;
        std::vector<RtLampGpu::Engine::PhysRoom> pr(2);
        pr[0].header = rooms[0].header, pr[1].header = rooms[1].header;
        std::vector<int> pt(np);
        for (int i = 0; i < np; i++) {
            const int l = pick[i];
            pl.push_back(all[l]), pt[i] = i;
            pw.push_back({all[l].law.pos[0] + kWorld[0], all[l].law.pos[1] + kWorld[1], all[l].law.pos[2] + kWorld[2]});
            pr[0].lamp.push_back(rowA[l]), pr[1].lamp.push_back(row0[l]);
            pr[0].fill.push_back(l < nL ? rooms[0].fill[l] : 0.0f), pr[1].fill.push_back(0.0f);
        }
        RtLampGpu::KeptMapData dk = d;
        std::vector<uint32_t> gamePx(static_cast<size_t>(w) * h, 0xFF000000u), got;
        for (uint32_t i = 0; i < count; i++) dk.texels.push_back(vertexOf(i, i % w, i / w));
        IDirect3DTexture9 *game = nullptr, *out = nullptr;
        RtLampGpu::Engine::MakePreblur(g_dev, w, h, gamePx.data(), &game);
        g_dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &out, nullptr);
        RtLampGpu::KeptMapGpu g;
        bool okM = game && out && RtLampGpu::Engine::UploadKept(g_dev, dk, g);
        g_dev->BeginScene();
        if (okM && eng.Begin(g_dev)) {
            okM = eng.EnsureTiles(g_dev, std::max(np, 1), err) && eng.CopyTexture(g_dev, game, out, w, h, err);
            for (int i = 0; okM && i < np; i++) okM = eng.RenderShadowTile(g_dev, pl[i].law.pos, i, 64.0f, err);
            okM = okM && eng.RelightKeptPhys(g_dev, g, pl.data(), pt.data(), reinterpret_cast<const float (*)[3]>(pw.data()), np, pr, prm, game, out, w, h, err);
            eng.End(g_dev);
        } else
            okM = false;
        g_dev->EndScene();
        okM = okM && RtLampGpu::Engine::ReadTexture(g_dev, out, w, h, got);
        g.Release();
        if (game) game->Release();
        if (out) out->Release();
        // each picked lamp's GPU shadow share alone (untested points, mult 1, no light cut), as (b) above: the CPU then differs from
        // the GPU only by what the pick and the rows do
        std::vector<std::vector<float>> shM(all.size());
        IDirect3DVertexBuffer9* uvb2 = nullptr;
        okM = okM && RtLampGpu::Engine::MakeVb(g_dev, untested.data(), count, &uvb2);
        for (int i = 0; okM && i < np; i++) {
            const int l = pick[i];
            std::vector<float> rows(2 * RtLampGpu::Engine::kTableCols * 4, 0.0f);
            for (int r = 0; r < 2; r++) {
                rows[r * RtLampGpu::Engine::kTableCols * 4] = 1.0f, rows[r * RtLampGpu::Engine::kTableCols * 4 + 1] = (r == 0 ? rowA : row0)[l][1];
                rows[(r * RtLampGpu::Engine::kTableCols + 16) * 4] = pr[r].header;
            }
            RtLampGpu::Params pd = prm;
            pd.thr = 0.0f, pd.fade = false;
            RtLampGpu::Map dm;
            dm.w = w, dm.h = h, dm.vb = uvb2, dm.count = count;
            g_dev->BeginScene();
            eng.Begin(g_dev);
            okM = eng.SetTable(g_dev, rows.data(), 2, err) && eng.AccumulateRlg(g_dev, dm, &pl[i], &pt[i], 1, pd, true, err, true, reinterpret_cast<const float (*)[3]>(pw[i].data()));
            eng.End(g_dev);
            g_dev->EndScene();
            okM = okM && eng.ReadSum(g_dev, w, h, shM[l]);
        }
        if (uvb2) uvb2->Release();
        long mn = 0, mWithin2 = 0, mMax = 0, mLit = 0;
        for (uint32_t i = 0; okM && i < count; i++) {
            const Tx& t = tx[i];
            float R[3] = {t.base[0], t.base[1], t.base[2]}, c[4];
            for (size_t l = 0; l < all.size(); l++) {
                const float mult = (t.slot == 0 ? rowA : row0)[l][0], reach = (t.slot == 0 ? rowA : row0)[l][1];
                float law[3];
                GpuLampLaw::Eval(all[l].law, t.pos, t.nrm, GpuLampLaw::WallReach(t.nrm, prm.reachOn) * reach, law);
                if (mult == 0.0f || !((law[0] + law[1] + law[2]) > 0.0f)) continue;
                float sh = 0.0f;
                if (shM[l].empty()) sh = Visibility(mesh, all[l].law.pos, prm.radius, t.pos, kRefSamples, &stamp, &mark); // (the culled one)
                else {
                    const float* gs = &shM[l][i * 4];
                    sh = ((gs[2] + gs[1]) + gs[0]) / ((law[2] + law[1]) + law[0]);
                }
                if (t.tested) {
                    const RoomLightTests::V from{all[l].law.pos[0] + kWorld[0], all[l].law.pos[1] + kWorld[1], all[l].law.pos[2] + kWorld[2]};
                    const float fill = t.slot == 0 && static_cast<int>(l) < nL ? rooms[0].fill[l] : 0.0f;
                    const float prod = sh * RtLampGpu::FurnitureFill(fill > 0.0f ? table.OccludeFill(static_cast<int>(rooms[0].header), from, worldOf(t), prm.radius) : RoomLightTests::occlude(roomA, from, worldOf(t), &unknown), fill);
                    sh = RoomLightTests::K_DROP > prod ? 0.0f : prod;
                }
                float cl[3] = {law[0] * sh, law[1] * sh, law[2] * sh};
                RtLampModel::Fade(cl, prm.thr, prm.fade);
                if (l == 6 && cl[0] + cl[1] + cl[2] > 1.0f / 255.0f) mLit++;
                for (int q = 0; q < 3; q++) R[q] += mult * cl[q];
            }
            RtLampModel::KeptTexel(R, t.S, t.p, t.norm, k, t.slot == 0, t.slot == 0 ? floorA : 0.0f, t.dither, c);
            const int dl = OutdoorDeltaModel::MaxLevelDiff(OutdoorDeltaModel::PackModel(c), got[i]);
            mn++, mWithin2 += dl <= 1, mMax = std::max<long>(mMax, dl);
        }
        std::printf("  a lamp moved into room A and one far away (%d of %zu lamps picked by reach, the far one %s): %ld texels, %.2f%% within 1/255 of the CPU with every lamp (their GPU shadow shares), "
                    "largest %ld/255; %ld texels lit by the moved lamp\n",
                    np, all.size(), farCulled ? "culled" : "drawn", mn, mn ? 100.0 * mWithin2 / mn : 100.0, mMax, mLit);
        Check(okM, "physical: the moved lamp's relight: " + err);
        Check(farCulled && movedIn && mLit > 0, "physical: the pick by reach draws the moved lamp and culls the far one");
        Check(mn > 0 && mWithin2 >= static_cast<long>(0.999 * mn) && mMax <= 2, "physical: a lamp moved into another room: the GPU's texels within 1/255 of the CPU's with every lamp (99.9%), none past 2/255");
    }
    // The dense quads (kept maps: the points as textures, AccPhysInPS and T3dDensePS) against one point per texel: the same sum, bit for
    // bit, at a shadow looked up off the point and with a footprint; a place left empty is refused
    {
        const std::string src = std::string(kRtLampsHlsl) + "float4 AccPhysAtPS(TexelIn i, float2 vpos : VPOS) : COLOR0 { return AccPhys(i, vpos, 24, 16, 16, true); }\n";
        ID3DBlob *code = nullptr, *errs = nullptr;
        IDirect3DPixelShader9* atPs = nullptr;
        bool okD = SUCCEEDED(D3DCompile(src.data(), src.size(), "rt_lamps", nullptr, nullptr, "AccPhysAtPS", "ps_3_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &code, &errs)) && code &&
                   SUCCEEDED(g_dev->CreatePixelShader(static_cast<const DWORD*>(code->GetBufferPointer()), &atPs));
        void* withAt[RtLampGpu::kShaderCount];
        std::memcpy(withAt, g_shaders, sizeof withAt);
        withAt[RtLampGpu::kAccPhysPs] = atPs;
        RtLampGpu::KeptMapData dd = d;
        for (uint32_t i = 0; i < count; i++) {
            if (i % 3 == 0) dd.phys[i].mask[0] += 0.2f;
            if (i % 5 == 0) dd.phys[i].mask[3] = 0.03f;
        }
        for (uint32_t i = 0; i < count; i++) // (a sharper floor's shape: 2 x 2 sub-texels a texel, the last row short)
            for (int q = 0; q < 4; q++) {
                RtLampGpu::TexelVertex v = dd.phys[i];
                v.x = 2.0f * v.x + static_cast<float>(q & 1), v.y = 2.0f * v.y + static_cast<float>(q >> 1);
                dd.physHi.push_back(v);
            }
        RtLampGpu::KeptMapGpu g;
        IDirect3DVertexBuffer9* vb = nullptr;
        RtLampGpu::Engine ref(RtLampGpu::PlainCalls());
        okD = okD && RtLampGpu::Engine::UploadKept(g_dev, dd, g) && RtLampGpu::Engine::MakeVb(g_dev, dd.phys.data(), count, &vb) && ref.Init(g_dev, withAt, 256, err) &&
              ref.SetProxy(g_dev, RtLampProxy::Mesh{}, err) && ref.SetProxy(g_dev, mesh, err, true);
        const size_t wantRects = count % w ? 2 : 1;
        Check(okD && g.phys.rects.size() == wantRects && g.physHi.rects.size() == wantRects, "physical: the kept points' rects");
        g_dev->BeginScene();
        if (okD && eng.Begin(g_dev)) { // (the checks above left other lamps in its tiles)
            okD = eng.EnsureTiles(g_dev, nL, err) && eng.SetBlockers(g_dev, table, err);
            for (int l = 0; okD && l < nL; l++) okD = eng.RenderShadowTile(g_dev, lamps[l].law.pos, l, 64.0f, err);
            eng.End(g_dev);
        }
        if (okD && ref.Begin(g_dev)) {
            okD = ref.EnsureTiles(g_dev, nL, err) && ref.SetBlockers(g_dev, table, err);
            for (int l = 0; okD && l < nL; l++) okD = ref.RenderShadowTile(g_dev, lamps[l].law.pos, l, 64.0f, err);
            ref.End(g_dev);
        }
        g_dev->EndScene();
        RtLampGpu::Map dense, pts;
        dense.w = pts.w = w, dense.h = pts.h = h, dense.pts = &g.phys, dense.blocks = g.phys.rects, pts.vb = vb, pts.count = count;
        std::vector<float> rows(rooms.size() * RtLampGpu::Engine::kTableCols * 4), a, b;
        constexpr int kB = RtLampGpu::Engine::kPhysBatch;
        for (int b0 = 0; okD && b0 < nL; b0 += kB) {
            const int nb = std::min(kB, nL - b0);
            std::fill(rows.begin(), rows.end(), 0.0f);
            for (size_t r = 0; r < rooms.size(); r++) {
                float* row = &rows[r * RtLampGpu::Engine::kTableCols * 4];
                for (int i = 0; i < nb; i++) {
                    for (int c = 0; c < 3; c++) row[i * 4 + c] = rooms[r].lamp[b0 + i][c];
                    row[i * 4 + 3] = rooms[r].fill[b0 + i];
                }
                row[16 * 4] = rooms[r].header;
            }
            const auto* from = reinterpret_cast<const float (*)[3]>(world.data() + b0);
            g_dev->BeginScene();
            eng.Begin(g_dev);
            okD = eng.SetTable(g_dev, rows.data(), static_cast<int>(rooms.size()), err) && eng.AccumulateRlg(g_dev, dense, lamps.data() + b0, tiles.data() + b0, nb, prm, b0 == 0, err, true, from);
            eng.End(g_dev);
            ref.Begin(g_dev);
            okD = okD && ref.SetTable(g_dev, rows.data(), static_cast<int>(rooms.size()), err) && ref.AccumulateRlg(g_dev, pts, lamps.data() + b0, tiles.data() + b0, nb, prm, b0 == 0, err, true, from);
            ref.End(g_dev);
            g_dev->EndScene();
        }
        okD = okD && eng.ReadSum(g_dev, w, h, a) && ref.ReadSum(g_dev, w, h, b);
        long differ = 0, lit = 0;
        for (uint32_t i = 0; okD && i < count; i++) {
            differ += std::memcmp(&a[size_t(i) * 4], &b[size_t(i) * 4], 16) != 0;
            lit += b[size_t(i) * 4] > 0.0f;
        }
        std::printf("  dense quads against one point per texel: %ld of %u sums differ bit for bit (%ld lit)\n", differ, count, lit);
        Check(okD && differ == 0 && lit > 0, "physical: the dense quads' sum is not the points' bit for bit: " + err);
        // The dense pass with another room's records, glass and the bounce (cross_room_shadows, glass_share, room_bounce): lamp 0 alone, a joined
        // table behind row 1, two glazed spans across the room; the T3 pack against the CPU, the sum with the glass against the sum
        // without it (the soft cut before the glass: no texel goes out), and the bounce of a batch of no lamp against BounceAt
        {
            RoomLightTests::Room nb;
            {
                RoomLightTests::Blocker k{};
                k.vt = RoomLightTests::kQuadVt;
                const float A[3] = {6.0f + kWorld[0], 1.0f + kWorld[1], 5.0f + kWorld[2]}, Nn[3] = {0, 1, 0}, Uv[3] = {1, 0, 0}, Vv[3] = {0, 0, 1};
                for (int c = 0; c < 3; c++)
                    k.f[0x70 / 4 + c] = A[c], k.f[0x80 / 4 + c] = A[c], k.f[0x90 / 4 + c] = Nn[c], k.f[0xA0 / 4 + c] = Uv[c], k.f[0xB0 / 4 + c] = Vv[c], k.f[0xE0 / 4 + c] = 7.0f, k.f[0xF0 / 4 + c] = -3.0f;
                k.f[0xD0 / 4] = k.f[0xD4 / 4] = k.f[0xD8 / 4] = k.f[0xDC / 4] = 1.0f;
                nb.rec.push_back({{A[0], A[1], A[2]}, 4.0f, {k}});
            }
            RtLampGpu::BlockerTable other;
            const int ho = other.Append(nb);
            RtLampGpu::BlockerTable j = RtLampGpu::BlockerTable::Join({&table, static_cast<int>(rooms[0].header)}, {{&other, ho}});
            // (behind 300 panes out of the way: a ps_3_0 loop runs at most 255 times, and a lot's windows can be more)
            std::vector<RtLampProxy::Pane> panes(300, RtLampProxy::Pane{-50, -100, 50, -100, -10, 10});
            panes.push_back({-50, 6.5f, 50, 6.5f, -10, 10}), panes.push_back({-50, 3.5f, 50, 3.5f, -10, 10});
            RtLampGpu::Params pg = prm;
            pg.paneCount = static_cast<float>(panes.size()), pg.paneFirst = static_cast<float>(j.AppendPanes(panes));
            std::fill(rows.begin(), rows.end(), 0.0f);
            for (size_t r = 0; r < rooms.size(); r++) {
                float* row = &rows[r * RtLampGpu::Engine::kTableCols * 4];
                row[0] = rooms[r].lamp[0][0], row[1] = rooms[r].lamp[0][1], row[2] = 2.0f;
                row[16 * 4] = r == 0 ? 0.0f : -1.0f;
            }
            const auto* from = reinterpret_cast<const float (*)[3]>(world.data());
            std::vector<float> t3, s0, s1;
            const auto run = [&](const RtLampGpu::Params& q, std::vector<float>* outT3, std::vector<float>& sum) {
                g_dev->BeginScene();
                eng.Begin(g_dev);
                bool k = eng.SetBlockers(g_dev, j, err) && eng.SetTable(g_dev, rows.data(), static_cast<int>(rooms.size()), err) && eng.AccumulateRlg(g_dev, dense, lamps.data(), tiles.data(), 1, q, true, err, true, from);
                eng.End(g_dev);
                g_dev->EndScene();
                return k && (!outT3 || eng.ReadT3(g_dev, w, h, *outT3)) && eng.ReadSum(g_dev, w, h, sum);
            };
            bool okG = run(prm, nullptr, s0) && run(pg, &t3, s1);
            long panesBad = 0, t3Bad = 0, sumBad = 0, crossed = 0, cast = 0, lost = 0;
            for (uint32_t i = 0; okG && i < count; i++) {
                const float v = t3[size_t(i) * 4];
                if (v == 1.0f) continue; // (a texel lamp 0 does not reach)
                const int n = static_cast<int>(std::floor(v * 0.5f));
                const float t = v - 2.0f * static_cast<float>(n);
                panesBad += n != RtLampProxy::PaneCount(panes, lamps[0].law.pos, dd.phys[i].p);
                crossed += n > 0;
                if (dd.phys[i].p[3] >= 8.0f && dd.phys[i].p[3] < 16.0f) { // (room A, tested)
                    const float cpu = j.Occlude(1, {world[0][0], world[0][1], world[0][2]}, {dd.phys[i].base[0], dd.phys[i].base[1], dd.phys[i].base[2]}, prm.edgeR, prm.radius);
                    t3Bad += std::fabs(t - cpu) > 1e-3f;
                    cast += table.Occlude(static_cast<int>(rooms[0].header), {world[0][0], world[0][1], world[0][2]}, {dd.phys[i].base[0], dd.phys[i].base[1], dd.phys[i].base[2]}, prm.edgeR, prm.radius) > cpu + 0.05f;
                }
                const float pass = std::pow(WallOpenings::kWindowPass, static_cast<float>(n));
                for (int c = 0; c < 3; c++) sumBad += std::fabs(s1[size_t(i) * 4 + c] - s0[size_t(i) * 4 + c] * pass) > 1e-4f * std::max(s0[size_t(i) * 4 + c], 1e-3f);
                lost += s0[size_t(i) * 4 + 1] > 0.0f && s1[size_t(i) * 4 + 1] <= 0.0f;
            }
            std::printf("  dense pass with a joined table and 2 panes: %ld texels behind glass, %ld shaded by the joined room only; panes off the CPU %ld, 3D tests %ld, sums %ld, gone dark %ld\n", crossed, cast, panesBad, t3Bad, sumBad, lost);
            Check(okG && crossed > 0 && cast > 0, "across rooms: the scene crosses no pane or the joined room shades nothing: " + err);
            Check(panesBad == 0 && t3Bad == 0, "across rooms: T3dDensePS's panes or joined records differ from the CPU's");
            Check(sumBad == 0 && lost == 0, "across rooms: the glass is not the pass per pane after the soft cut");
            ArchAmbient::BounceRow br{6.0f, 12.0f, 9.0f, 12.0f, 0.0f, 2.6f, {0.01f, 0.03f, 0.02f}};
            RtLampGpu::Params pb = prm;
            pb.bounceN = ArchAmbient::BounceConsts(&br, 1, pb.bounceRows, pb.bounceHue);
            std::vector<float> s2;
            g_dev->BeginScene();
            eng.Begin(g_dev);
            bool okB = eng.SetBlockers(g_dev, j, err) && eng.SetTable(g_dev, rows.data(), static_cast<int>(rooms.size()), err) && eng.AccumulateRlg(g_dev, dense, nullptr, nullptr, 0, pb, true, err, true, nullptr);
            eng.End(g_dev);
            g_dev->EndScene();
            okB = okB && eng.ReadSum(g_dev, w, h, s2);
            float worstB = 0.0f, most = 0.0f;
            for (uint32_t i = 0; okB && i < count; i++) {
                float rgb[3] = {};
                const float* Nn = dd.phys[i].n;
                const float l = std::sqrt(Nn[0] * Nn[0] + Nn[1] * Nn[1] + Nn[2] * Nn[2]), nn[3] = {Nn[0] / l, Nn[1] / l, Nn[2] / l};
                ArchAmbient::BounceAt(pb.bounceRows, pb.bounceN, pb.bounceHue, dd.phys[i].p, nn, rgb);
                const float cpu = rgb[1] / pb.bounceHue[1];
                worstB = std::max(worstB, std::fabs(s2[size_t(i) * 4 + 3] - cpu)), most = std::max(most, cpu);
                worstB = std::max(worstB, std::fabs(s2[size_t(i) * 4 + 1])); // (no lamp: nothing but the bounce)
            }
            std::printf("  bounce of a batch of no lamp: largest %.5f, off the CPU by at most %.6f\n", most, worstB);
            Check(okB && most > 0.0f && worstB < 1e-4f, "across rooms: the bounce differs from the CPU's: " + err);
            g_dev->BeginScene();
            if (eng.Begin(g_dev)) eng.SetBlockers(g_dev, table, err), eng.End(g_dev);
            g_dev->EndScene();
        }
        RtLampGpu::KeptMapData hole = d;
        hole.phys.erase(hole.phys.begin() + 1);
        RtLampGpu::KeptMapGpu holeGpu;
        Check(!RtLampGpu::Engine::UploadKept(g_dev, hole, holeGpu), "physical: a place left empty in the kept points was not refused");
        g.Release();
        if (vb) vb->Release();
        if (atPs) atPs->Release();
        if (code) code->Release();
        if (errs) errs->Release();
    }
    // without the physical shaders (not in the shader cache, no compiler): the engine still starts (the outdoor lamps) and
    // RelightKeptPhys refuses, so the game's map stays
    {
        void* some[RtLampGpu::kShaderCount];
        for (int i = 0; i < RtLampGpu::kShaderCount; i++) some[i] = RtLampGpu::IsPhysical(static_cast<RtLampGpu::ShaderId>(i)) ? nullptr : g_shaders[i];
        RtLampGpu::Engine bare(RtLampGpu::PlainCalls());
        std::string why;
        RtLampGpu::KeptMapGpu none;
        none.w = none.h = none.accW = none.accH = 1;
        const bool init = bare.Init(g_dev, some, 64, why);
        const bool refused = init && !bare.RelightKeptPhys(g_dev, none, nullptr, nullptr, nullptr, 0, rooms, prm, nullptr, reinterpret_cast<IDirect3DTexture9*>(1), 1, 1, why) &&
                             why.find("physical shaders") != std::string::npos;
        std::printf("  without the physical shaders: Init %s, RelightKeptPhys %s (%s)\n", init ? "ok" : "failed", refused ? "refused" : "ran", why.c_str());
        Check(init && refused, "physical: the engine without the physical shaders");
    }
    return true;
}

// ---------------------------------------------------------------- 9. physical indoor lamps over room 0 (AccumulatePhys, AccPhysXPS)
// Room 0's map (a floor of masked, exit ground and plain texels and an outer wall) whose lot lamps left its base, drawn as
// Real-time outdoor lamps draws it: the lamps from their laws (a bare bulb, a spot listed twice, a shaded lamp at a norm scale, a
// TubeLight, a rectangle area light, a street lamp; reach on two, exit on three) over the phys points, some plain texels behind a
// table of the game's 3D test, then the outdoor batch with first = false and ComposeK0PS over the second sum. No cube occluder
// (share 1). On the CPU: the law x the 3D test, the soft light cut, AddMask's luminances and the exit light, RtLampModel::Compose.
// The lamps in two calls over one sum give the one call's map.
bool Room0PhysCheck() {
    std::printf("\n[physical indoor lamps] room 0's lot lamps from their laws (floor mask and exit alpha in the second sum) against the CPU\n");
    std::mt19937 rng(77);
    std::uniform_real_distribution<float> U(0.0f, 1.0f);
    const int w = 64, h = 32;
    const float thr = 3.0f / 255.0f;
    const auto def = [](float x, float y, float z, int type) {
        LampDef d;
        d.pos[0] = x, d.pos[1] = y, d.pos[2] = z, d.type = type, d.range = 12.0f;
        if (type == 4) d.scale = 4.9f, d.off = -0.32f;
        return d;
    };
    std::vector<RtLampModel::Lamp> lamps = {ToLamp(def(4.0f, 2.5f, 3.0f, 3)), ToLamp(def(10.0f, 2.8f, 5.0f, 4)), ToLamp(def(6.0f, 1.2f, 6.0f, 5)),
                                            ToLamp(def(3.0f, 1.5f, 5.0f, 3)), ToLamp(def(12.0f, 2.9f, 2.0f, 3)), ToLamp(def(1.0f, 3.0f, 7.0f, 11))};
    {
        const float s[3] = {0.0f, 0.0f, 1.0f};
        GpuLampLaw::TubeShape(lamps[3].law, s, 1.0f);
        const float u[3] = {1.0f, 0.0f, 0.0f}, v[3] = {0.0f, 0.0f, 1.0f}, nrm[3] = {0.0f, 1.0f, 0.0f};
        GpuLampLaw::AreaShape(lamps[4].law, u, v, nrm, 0.3f, 0.2f, false);
    }
    const int nL = static_cast<int>(lamps.size());
    // room rows: mult x norm scale, reach, exit
    const std::vector<std::array<float, 3>> rows = {{1.0f, 0.0f, 1.0f}, {2.0f, 1.0f, 0.0f}, {0.8f, 0.0f, 1.0f}, {1.0f, 0.0f, 0.0f}, {1.0f, 0.0f, 1.0f}, {1.0f, 1.0f, 0.0f}};
    const float kWorld[3] = {100.5f, 0.0f, 200.25f};
    std::vector<std::array<float, 3>> world(nL);
    for (int l = 0; l < nL; l++) {
        lamps[l].mult = rows[l][0];
        for (int c = 0; c < 3; c++) world[l][c] = lamps[l].law.pos[c] + kWorld[c];
    }
    // the 3D test: a table top over part of the floor (world space)
    RoomLightTests::Room room0;
    {
        RoomLightTests::Blocker k{};
        k.vt = RoomLightTests::kQuadVt;
        const float A[3] = {8.0f + kWorld[0], 0.75f + kWorld[1], 4.0f + kWorld[2]}, N[3] = {0, 1, 0}, Uv[3] = {1, 0, 0}, Vv[3] = {0, 0, 1};
        for (int c = 0; c < 3; c++)
            k.f[0x70 / 4 + c] = A[c], k.f[0x80 / 4 + c] = A[c], k.f[0x90 / 4 + c] = N[c], k.f[0xA0 / 4 + c] = Uv[c], k.f[0xB0 / 4 + c] = Vv[c], k.f[0xE0 / 4 + c] = 7.0f,
                                                                                                                    k.f[0xF0 / 4 + c] = -3.0f;
        k.f[0xD0 / 4] = k.f[0xD8 / 4] = 1.2f, k.f[0xD4 / 4] = k.f[0xDC / 4] = 0.8f;
        room0.rec.push_back({{A[0], A[1], A[2]}, 4.0f, {k}});
    }
    RtLampGpu::BlockerTable table;
    RtLampGpu::Engine::PhysRoom room;
    room.header = static_cast<float>(table.Append(room0));
    room.lamp = rows;
    room.fill = {RtLampGpu::kFurnitureFill, 0.0f, RtLampGpu::kFurnitureFill, 0.0f, 0.0f, 0.0f}; // (two exit lamps standing on furniture)
    // the texels (the map's points as rt_outdoor_lamps.cpp PrepareJob makes them) and the phys points at the same places
    std::vector<RtLampGpu::TexelVertex> verts, phys;
    std::vector<RtLampModel::TexelIn> in(static_cast<size_t>(w) * h);
    std::vector<char> tested(static_cast<size_t>(w) * h, 0);
    long nTested = 0, nShaded = 0;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            const int i = y * w + x;
            const bool wall = y >= 28;
            RtLampGpu::TexelVertex v{};
            v.x = static_cast<float>(x), v.y = static_cast<float>(y), v.w = 1.0f;
            if (wall) v.p[0] = 0.0f, v.p[1] = 0.25f + (y - 28) * 0.5f, v.p[2] = x * 0.25f, v.n[0] = 1.0f;
            else v.p[0] = 0.5f + x * 0.25f, v.p[1] = 0.0f, v.p[2] = y * 0.25f, v.n[1] = 1.0f;
            RtLampModel::TexelIn& t = in[i];
            const int kind = wall ? 2 : i % 3; // 0 masked, 1 exit ground, 2 plain
            v.n[3] = kind == 2 ? U(rng) * 0.5f : 0.0f;
            for (int c = 0; c < 3; c++) v.base[c] = t.base[c] = U(rng) < 0.3f ? 1.2f * U(rng) : 0.2f * U(rng);
            v.base[3] = t.dither = (x + y) % 3 ? RoomMapDither::Offset(x, y) / 255.0f : 0.0f;
            t.alpha = v.n[3];
            int bits = 0;
            if (kind == 0) {
                v.p[3] = 1.0f, t.masked = true, bits = 1;
                t.baseAll = v.mask[1] = U(rng) < 0.3f ? 0.0f : 0.5f * U(rng);
                t.baseVis = v.mask[0] = t.baseAll * U(rng);
            } else if (kind == 1) {
                t.exitGround = true, v.mask[3] = 1.0f, bits = 4;
                t.exitBase = v.mask[2] = U(rng) < 0.5f ? 0.0f : 0.3f * U(rng);
            }
            if (!wall && x % 2 == 0) tested[i] = 1, bits |= 8, nTested++; // (masked and exit ground texels too: AccPhysXPS's 3D test)
            verts.push_back(v);
            RtLampGpu::TexelVertex q{};
            q.x = v.x, q.y = v.y, q.w = 1.0f;
            for (int c = 0; c < 3; c++) q.p[c] = v.p[c], q.n[c] = v.n[c], q.base[c] = v.p[c] + kWorld[c];
            q.p[3] = static_cast<float>(bits);
            phys.push_back(q);
        }
    // the CPU
    const bool reachOn = true;
    std::vector<uint32_t> want(static_cast<size_t>(w) * h);
    for (int i = 0; i < w * h; i++) {
        const RtLampGpu::TexelVertex& v = verts[i];
        float G[3] = {}, gVis = 0.0f, gAll = 0.0f, gExit = 0.0f;
        for (int l = 0; l < nL; l++) {
            const float mult = rows[l][0];
            float law[3];
            GpuLampLaw::Eval(lamps[l].law, v.p, v.n, GpuLampLaw::WallReach(v.n, reachOn) * rows[l][1], law);
            float sh = 1.0f;
            if (tested[i]) {
                int unknown = 0;
                const RoomLightTests::V head{world[l][0], world[l][1], world[l][2]}, at{v.p[0] + kWorld[0], v.p[1] + kWorld[1], v.p[2] + kWorld[2]};
                const float prod = RtLampGpu::FurnitureFill(
                    room.fill[l] > 0.0f ? table.OccludeFill(static_cast<int>(room.header), head, at, RtLampGpu::Params{}.radius) : RoomLightTests::occlude(room0, head, at, &unknown), room.fill[l]);
                sh = RoomLightTests::K_DROP > prod ? 0.0f : prod;
                nShaded += prod < 1.0f;
            }
            if (in[i].masked) {
                float d[3] = {lamps[l].law.pos[0] - v.p[0], lamps[l].law.pos[1] - v.p[1], lamps[l].law.pos[2] - v.p[2]}, wl[3];
                const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
                for (float& c : d) c /= len;
                GpuLampLaw::Eval(lamps[l].law, v.p, d, 0.0f, wl);
                if (thr <= (wl[2] + wl[1]) + wl[0]) {
                    const float lum = mult * OutdoorDeltaModel::Lum3(wl);
                    gAll += lum, gVis += lum * sh;
                }
            }
            float c[3] = {law[0] * sh, law[1] * sh, law[2] * sh};
            RtLampModel::Fade(c, thr, true);
            for (int q = 0; q < 3; q++) G[q] += mult * c[q];
            if (in[i].exitGround && rows[l][2] > 0.5f && (law[0] + law[1] + law[2]) > 0.0f) {
                const float mc[3] = {mult * c[0], mult * c[1], mult * c[2]};
                gExit += OutdoorDeltaModel::Lum3(mc);
            }
        }
        want[i] = RtLampModel::Compose(in[i], G, gVis, gAll, gExit);
    }
    // the GPU: one call with every lamp, then two calls over one sum
    RtLampGpu::Engine eng(RtLampGpu::PlainCalls());
    std::string err;
    RtLampProxy::Mesh empty;
    bool ok = eng.Init(g_dev, g_shaders, 64, err) && eng.SetProxy(g_dev, empty, err) && eng.SetProxy(g_dev, empty, err, true);
    RtLampGpu::Map m, pm;
    m.w = pm.w = w, m.h = pm.h = h, m.count = pm.count = static_cast<UINT>(verts.size());
    IDirect3DTexture9* out = nullptr;
    ok = ok && RtLampGpu::Engine::MakeVb(g_dev, verts.data(), m.count, &m.vb) && RtLampGpu::Engine::MakeVb(g_dev, phys.data(), pm.count, &pm.vb, true) &&
         SUCCEEDED(g_dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &out, nullptr));
    std::vector<int> tiles(nL);
    for (int l = 0; l < nL; l++) tiles[l] = l;
    RtLampGpu::Params p;
    p.thr = thr, p.mask = true, p.keptX = true, p.physX = true, p.reachOn = reachOn;
    std::vector<uint32_t> got[2];
    for (int split = 0; ok && split < 2; split++) {
        g_dev->BeginScene();
        if (eng.Begin(g_dev)) {
            ok = eng.EnsureTiles(g_dev, nL, err) && eng.SetBlockers(g_dev, table, err);
            for (int l = 0; ok && l < nL; l++) ok = eng.RenderShadowTile(g_dev, lamps[l].law.pos, l, 64.0f, err);
            const auto wp = reinterpret_cast<const float (*)[3]>(world.data());
            if (!split) ok = ok && eng.AccumulatePhys(g_dev, pm, lamps.data(), tiles.data(), wp, nL, room, p, true, err);
            else {
                RtLampGpu::Engine::PhysRoom a = room, b = room;
                a.lamp.assign(rows.begin(), rows.begin() + 3), b.lamp.assign(rows.begin() + 3, rows.end());
                a.fill.assign(room.fill.begin(), room.fill.begin() + 3), b.fill.assign(room.fill.begin() + 3, room.fill.end());
                ok = ok && eng.AccumulatePhys(g_dev, pm, lamps.data(), tiles.data(), wp, 3, a, p, true, err) &&
                     eng.AccumulatePhys(g_dev, pm, lamps.data() + 3, tiles.data() + 3, wp + 3, nL - 3, b, p, false, err);
            }
            ok = ok && eng.Accumulate(g_dev, m, nullptr, 0, p, false, err) && eng.Compose(g_dev, m, nullptr, out, p, err);
            eng.End(g_dev);
        } else
            ok = false;
        g_dev->EndScene();
        ok = ok && RtLampGpu::Engine::ReadTexture(g_dev, out, w, h, got[split]);
    }
    if (m.vb) m.vb->Release();
    if (pm.vb) pm.vb->Release();
    if (out) out->Release();
    Check(ok, "physical room 0 on the GPU: " + err);
    if (!ok) return false;
    long n = 0, within1 = 0, maxDiff = 0, alphaN = 0, alphaBad = 0, alphaLit = 0, same = 0;
    for (int i = 0; i < w * h; i++) {
        const int d = OutdoorDeltaModel::MaxLevelDiff(want[i], got[0][i]);
        n++, within1 += d <= 1, maxDiff = std::max<long>(maxDiff, d);
        same += got[1][i] == got[0][i];
        if (!in[i].masked && !in[i].exitGround) continue;
        alphaN++, alphaBad += (want[i] >> 24) != (got[0][i] >> 24), alphaLit += (want[i] >> 24) != 0;
    }
    std::printf("  %ld texels, %d lamps (%ld floor texels behind the 3D test, masked and exit ground among them, %ld lamp pairs shaded): %.2f%% within 1/255 of the CPU, largest %ld/255; "
                "%ld masked or exit ground texels, %ld with an alpha, %ld alphas differ; two calls over one sum: %ld of %ld texels as one\n",
                n, nL, nTested, nShaded, n ? 100.0 * within1 / n : 100.0, maxDiff, alphaN, alphaLit, alphaBad, same, n);
    Check(nShaded > 0, "physical room 0: the 3D test shades no texel (the scene is wrong)");
    Check(n > 0 && within1 == n, "physical room 0: a texel more than 1/255 from the CPU's (unshadowed lamps)");
    Check(alphaLit > 0 && alphaBad == 0, "physical room 0: the floor mask or exit alpha differs from the CPU's");
    Check(same == n, "physical room 0: two calls over one sum differ from one");
    return true;
}

// The stair set: kept only by the floor holes, it only grows, its uploads are bounded, and the floor under the treads loses the lamp
// A whole tile kept in the split tiles' atlas gets copies at its place in the tile columns; a half tile's block, a tile with texels there
// and a tile of another map do not (room 8's blocks of the 23-40-21 scene: 7 whole tiles, each copy landing on the map's 4 x 4 hole and border)
static void FloorGridTwinChecks() {
    struct T {
        float p[3];
        uint16_t x, y;
        uint8_t map;
    };
    std::vector<T> v;
    const auto block = [&](int tx, int tz, int bx, int by, int map, bool push) {
        for (int j = 0; j < 8; j++)
            for (int i = 0; i < 8; i++)
                v.push_back({{tx + (i + 0.5f) / 8.0f + (push && i == j ? 0.03f : 0.0f), 1.0f, tz + (j + 0.5f) / 8.0f + (push && i == j ? 0.1f : 0.0f)},
                              static_cast<uint16_t>(bx + i), static_cast<uint16_t>(by + j), static_cast<uint8_t>(map)});
    };
    block(22, 10, 357, 1, 1, false);       // a whole tile in the atlas
    block(21, 10, 367, 1, 1, true);        // a half tile's block
    block(5, 5, 40, 40, 1, false);         // a tile in its place
    block(5, 5, 377, 1, 1, false);         // (its place is taken)
    block(24, 8, 387, 1, 2, false);        // another map
    const size_t n = PhysLampPick::AddFloorGridTwins(v, 1, 8, 512, 512, [](const T& t, float o[3]) { std::memcpy(o, t.p, sizeof t.p); });
    Check(n == 64, "floor twins: only the whole atlas tile is copied");
    bool placed = true;
    for (size_t i = 0; i < n; i++) {
        const T& c = v[v.size() - n + i];
        placed = placed && c.x / 8 == 22 && c.y / 8 == 10 && std::floor(c.p[0] * 8.0f) == c.x && std::floor(c.p[2] * 8.0f) == c.y;
    }
    Check(placed, "floor twins: each copy stands where the outdoor floor's uv reads it");
}
static void StairSetChecks() {
    using MeshOcclusion::Triangle;
    RtLampProxy::Lot lot;
    lot.stories.resize(2);
    for (int s = 0; s < 2; s++) lot.stories[s].story = s, lot.stories[s].w = lot.stories[s].h = 32, lot.stories[s].plane = 3.0f * s;
    lot.stories[1].removed.assign(32 * 32, 0);
    lot.worldToLot[3][0] = -100.0f; // (lot x = world x - 100)
    StairSet::Set set;
    set.Begin(lot, 1);
    Check(!StairSet::wanted.load(), "stair set: a lot with no floor hole reads no draw");
    for (int z = 10; z < 14; z++) lot.stories[1].removed[z * 32 + 10] = 15;
    set.Begin(lot, 2);
    Check(StairSet::wanted.load(), "stair set: a floor hole asks for the draws");
    const auto tread = [](float x, float y, float z) { return std::vector<Triangle>{{{x, y, z}, {x + 1, y, z}, {x + 1, y, z + 1}}, {{x, y, z}, {x + 1, y, z + 1}, {x, y, z + 1}}}; };
    set.Add(tread(120, 1.5f, 11), 1000);
    Check(set.Tris() == 0, "stair set: far from the hole stays out");
    set.Add(tread(110, 5.0f, 11), 1000);
    Check(set.Tris() == 0, "stair set: above the hole's story stays out");
    set.Add({{{105, 1, 5}, {116, 1, 5}, {116, 1, 16}}}, 1000);
    Check(set.Tris() == 0, "stair set: a triangle as wide as a floor stays out");
    Check(!set.Due(1000), "stair set: nothing to upload yet");
    for (int z = 10; z < 14; z++) set.Add(tread(110, 0.75f * (z - 9), static_cast<float>(z)), 1000);
    set.Add(tread(110, 0.75f, 10), 1000);
    Check(set.Tris() == 8, "stair set: the treads over the hole stay, once each");
    Check(!set.Due(1000 + StairSet::kQuietMs - 1) && set.Due(1000 + StairSet::kQuietMs), "stair set: an upload waits for the stair to be read");
    RtLampProxy::Mesh m;
    RtLampProxy::Build(lot, m, true);
    const size_t built = m.tris.size();
    const float lamp[3] = {10.5f, 2.9f, 11.5f}, under[3] = {10.5f, 0.01f, 11.5f};
    Check(RtLampProxy::Clear(m, lamp, under), "stair set: the lamp reaches the floor with no stair");
    set.AppendTo(m);
    Check(m.tris.size() == built + 8 && m.kind.back() == RtLampProxy::Mesh::kShape, "stair set: its triangles join the mesh as shapes");
    Check(!RtLampProxy::Clear(m, lamp, under), "stair set: the floor under the treads loses the lamp");
    set.Uploaded(5000);
    Check(!set.Due(6000), "stair set: nothing new, no upload");
    set.Add(tread(111, 2.0f, 12), 6000);
    Check(!set.Due(5000 + StairSet::kUploadGapMs - 1) && set.Due(5000 + StairSet::kUploadGapMs), "stair set: uploads keep their gap");
    for (uint32_t i = 0; i < StairSet::kMaxUploads; i++) set.Uploaded(5000), set.Add(tread(111, 2.0f + 0.01f * i, 12), 6000);
    Check(!set.Due(1000000), "stair set: uploads are bounded per lot");
    std::vector<Triangle> many;
    for (int i = 0; i < 5000; i++) many.push_back({{110 + static_cast<float>(i / 1000) * 0.01f, 1 + static_cast<float>(i % 1000) * 0.002f, 11}, {111, 1, 11}, {111, 1, 12}});
    set.Add(many, 7000);
    Check(set.Tris() == StairSet::kMaxTris && set.Full(), "stair set: bounded");
    set.Clear();
    Check(!StairSet::wanted.load() && set.Tris() == 0 && set.Gen() == 0, "stair set: cleared with the lot");
}

int main(int argc, char** argv) {
    StairSetChecks();
    FloorGridTwinChecks();
    std::vector<std::string> worlds;
    int maxLamps = 12;
    char* dbg = nullptr;
    size_t dbgLen = 0;
    g_debug = _dupenv_s(&dbg, &dbgLen, "RT_DEBUG") == 0 && dbg;
    free(dbg);
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--lamps" && i + 1 < argc) maxLamps = std::atoi(argv[++i]);
        else if (a == "--probe") g_allLamps = true;
        else worlds.push_back(a);
    }
    if (g_allLamps) { // (the probe only: where the lamp heads sit against the occluders, no device)
        std::vector<Scene> scenes;
        for (const std::string& w : worlds) LoadWorld(w, scenes, 100000);
        for (const Scene& sc : scenes) Probe(sc);
        return 0;
    }
    if (!MakeDevice()) {
        std::printf("FAIL: no Direct3D 9 HAL device\n");
        return 1;
    }
    if (!MakeShaders()) return 1;
    SplitCheck();
    RoomLampsCheck();
    KeptGpuCheck();
    KeptAmbientCheck();
    Room0KeptCheck();
    Room0KeptXCheck();
    MaskAllAfterKeptCheck();
    PhysKeptCheck();
    Room0PhysCheck();
    SharperTargetCheck();
    PenumbraCheck();
#ifdef RT_OBJECT_SHAPES
    ObjectParityCheck();
#endif
    UnderDeltaCheck();
    BakeStamp4Check();
    std::vector<Scene> scenes;
    scenes.push_back(Synthetic());
    scenes.push_back(IndoorRooms());
#ifdef RT_OBJECT_SHAPES
    scenes.push_back(FurnishedRooms());
#endif
    for (const std::string& w : worlds) {
        const size_t before = scenes.size();
        if (!LoadWorld(w, scenes, maxLamps)) {
            Check(false, "capture not readable: " + w);
            continue;
        }
        std::printf("\n%s: %zu lots with walls and outdoor lamps\n", w.c_str(), scenes.size() - before);
    }
    for (const int face : {256, 128}) {
        ShadowTally total;
        const auto t0 = std::chrono::steady_clock::now();
        for (const Scene& sc : scenes) ShadowCompare(sc, face, 0.15f, total, face == 256);
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        std::printf("\n== shadows, face %d, ball 0.15 m: %ld texel x lamp pairs: within 2/255 %.2f%%, within 1/255 %.2f%%, share within 0.05 %.2f%%, mean |share diff| %.4f, "
                    "leaks %ld, wrongly dark %ld (%.1f s)\n",
                    face, total.n, total.n ? 100.0 * total.within2 / total.n : 100.0, total.n ? 100.0 * total.within1 / total.n : 100.0,
                    total.n ? 100.0 * total.visWithin05 / total.n : 100.0, total.n ? total.sumAbs / total.n : 0.0, total.leaks, total.darkened, s);
        for (const auto& [what, c] : total.leakNear) std::printf("   leaks near %s: %ld\n", what.c_str(), c);
        std::printf("   the light against law x the reference's share, %ld lit pairs: mean |diff| Acc %.5f, AccRlg %.5f; Acc against AccRlg at most %.6f\n", total.lit,
                    total.lit ? total.accErr / total.lit : 0.0, total.lit ? total.rlgErr / total.lit : 0.0, total.accRlgMax);
        Check(total.lit > 0 && total.accRlgMax < 2.0 / 255.0, "Acc and AccRlg differ: they take one Shadow");
        if (face == 256) {
            Check(total.n > 0, "no texel compared");
            Check(total.n == 0 || total.within2 >= static_cast<long>(0.99 * total.n), "fewer than 99% of texels within 2/255 of the reference (face 256)");
        }
    }
    {
        // the shader used while a lamp is dragged (8 search taps, 4 ball points x 8 steps; the exact one runs on release)
        ShadowTally total;
        for (const Scene& sc : scenes) ShadowCompare(sc, 256, 0.15f, total, false, true);
        std::printf("\n== shadows while dragged (AccFastPS), face 256: %ld pairs: within 2/255 %.2f%%, within 1/255 %.2f%%, share within 0.05 %.2f%%, leaks %ld, wrongly dark %ld\n",
                    total.n, total.n ? 100.0 * total.within2 / total.n : 100.0, total.n ? 100.0 * total.within1 / total.n : 100.0,
                    total.n ? 100.0 * total.visWithin05 / total.n : 100.0, total.leaks, total.darkened);
        std::printf("   the light against law x the reference's share, %ld lit pairs: mean |diff| AccFast %.5f, AccRlgFast %.5f; AccFast against AccRlgFast at most %.6f\n", total.lit,
                    total.lit ? total.accErr / total.lit : 0.0, total.lit ? total.rlgErr / total.lit : 0.0, total.accRlgMax);
        Check(total.n == 0 || total.within2 >= static_cast<long>(0.95 * total.n), "the dragged shader: fewer than 95% of texels within 2/255 of the reference");
        Check(total.lit > 0 && total.accRlgMax < 2.0 / 255.0, "AccFast and AccRlgFast differ: they take one Shadow");
    }
#ifdef RT_GRID_EXTRACT
    std::printf("\n");
    for (const Scene& sc : scenes) {
        std::printf("[%s]\n", sc.name.c_str());
        GridCompare(sc, 0.15f);
    }
#else
    std::printf("\n(the grid walk was not extracted: run through run.ps1)\n");
#endif
    std::printf("\n%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
