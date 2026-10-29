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
#include "features/rt_lamp_model.h"
#include "features/rt_lamp_gpu.h"
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
    if (!eng.Init(g_dev, g_shaders, face, err) || !eng.SetProxy(g_dev, mesh, err)) {
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
            const bool read = ok && eng.ReadSum(g_dev, w, h, sum);
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
int main(int argc, char** argv) {
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
        Check(total.n == 0 || total.within2 >= static_cast<long>(0.95 * total.n), "the dragged shader: fewer than 95% of texels within 2/255 of the reference");
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
