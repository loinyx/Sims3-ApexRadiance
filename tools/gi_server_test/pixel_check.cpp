// The per-pixel delta shadow kernel, standalone (docs/features/realtime-gi/research/16-salvage-per-pixel-rt.md 2, 4.1, 5;
// no bridge, no game): pixel_shadow.comp on a Vulkan device with ray queries, over a synthetic depth buffer of check.cpp's
// lot seen from inside its lamp room.
//   Drawn into the depth: walls, floors, two object boxes, a chair's mesh, a Sim (capsules at 0.8 of their radius).
//   Traced: static = walls, floors and the object boxes, the chair's among them; delta casters = the chair's mesh
//   (inside its box) and the Sim's capsules (around what is drawn).
// Checked: the guide (position, normal) against the drawn surfaces; at most 4 lamps a pixel, the strongest; the rays
// of a frame within the cap; the occlusion against a CPU reference from the same guide (point emitters, then the 16
// samples of the emitter balls); 16 accumulated frames equal to the 16-sample dispatch; no self-shadow: a pixel the
// drawn surfaces leave lit is lit, also on the chair inside its box and on the Sim inside its capsules.
// Printed: the time of a frame and its rays. Read-only: prints to stdout, writes no file.
//   --bench <frames>  no checks: the dispatch's time by device timestamps, the kernel and its variants (run.ps1 builds them)
//   --device <text>  the device whose name holds <text>      --size <w> <h>  the depth's size (1920 1080)
#define main SolveMain // check.cpp's scene, device and buffer code as they are (other packages edit that file)
#include "check.cpp"
#undef main
#include <atomic>
#include <thread>
#include "pixel_shadow.h" // SPIR-V of pixel_shadow.comp and of its variants for --bench, built by run.ps1
#include "px_group16.h"
#include "px_lean.h"
#include "px_stop1.h"
#include "px_stop2.h"
#include "px_stop3.h"
#include "px_trace0.h"

namespace {
using RtLampProxy::Tri;
struct V3 {
    float x, y, z;
};
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float Len(V3 a) { return std::sqrt(Dot(a, a)); }
V3 Unit(V3 a) { return a * (1.0f / Len(a)); }
V3 At(const float* p) { return {p[0], p[1], p[2]}; }

// The shader's blocks
struct Capsule {
    float a[3], r, b[3], pad;
};
struct Box {
    float lo[4], hi[4];
};
struct GpuLamp {
    float pos[4], col[4];
};
struct Pixel {
    float mul[4], pos[4], nrm[4];
    uint32_t sel, u, d, rays;
    float num[4], den[4];
};
struct PixPush {
    float inv[16], camA[4];
    uint32_t width, height, lamps, casters, frame, samples;
    float bias, radiusScale;
};
constexpr uint32_t kMaxLamps = 4;
constexpr float kA = 0.25f;     // the light no lamp ray accounts for: a look constant (research 16 4.1), not tuned here
constexpr float kBias = 2e-3f;  // m: the ray origin's distance from the surface at the camera
constexpr float kNear = 0.1f, kFar = 1000.0f; // not verified: the game's clip planes (the depth's precision follows them)
constexpr float kShrink = 0.8f; // the drawn Sim against its capsules

// ---------------------------------------------------------------- the scene
struct PScene {
    RtLampProxy::Mesh walls;        // static, both sides (instance 0)
    std::vector<Tri> boxes, detail; // static object boxes (1) and the delta detail mesh (2): AddShape's winding, culled
    size_t drawnBoxes = 0;          // the boxes drawn into the depth (the chair's box is not: its mesh is)
    std::vector<Capsule> capsules;  // delta (3)
    std::vector<Box> casters;       // the delta casters' bounds
    std::vector<Lamp> lamps;
    // Mesh casters (mesh_check.cpp): static meshes as one bottom level a model and an instance an object, both sides
    struct Placed {
        uint32_t model;
        float xf[12]; // rows of a 3 x 4 matrix, model to world
    };
    std::vector<std::vector<Tri>> models; // model space
    std::vector<Placed> placed;
    RtLampProxy::Mesh meshes;              // the same in world space with its grid: the CPU reference's copy
    std::vector<Tri> shown;                // drawn only: the objects' own meshes when their casters differ
    bool twoSidedDetail = false;           // the delta detail meshes are game meshes (open, any winding): no culling
    RtLampProxy::Mesh detailGrid;          // (twoSidedDetail) the detail with its grid: the CPU reference's copy
    V3 eye{}, right{}, up{}, fwd{};
    float tanX = 0.0f, tanY = 0.0f;
};

PScene BuildPixelScene(int w, int h) {
    using namespace RtLampProxy;
    PScene ps;
    const Scene sc = BuildScene(1);
    Mesh box, detail;
    for (size_t i = 0; i < sc.mesh.tris.size(); i++) {
        const Tri& t = sc.mesh.tris[i];
        if (sc.mesh.kind[i] == Mesh::kShape) box.tris.push_back(t);
        else AddTri(ps.walls, t.v[0], t.v[1], t.v[2]);
    }
    BuildGrid(ps.walls);
    ps.drawnBoxes = box.tris.size();
    const auto shape = [](float cx, float cy, float cz, float hx, float hy, float hz) { return ObjShape{{cx, cy, cz}, {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}, {hx, hy, hz}}; };
    // A chair beside the table: the texel tier's box, and inside it the mesh (seat, back, four legs)
    AddShape(box, shape(12.5f, 0.53f, 14.2f, 0.25f, 0.53f, 0.25f));
    AddShape(detail, shape(12.5f, 0.43f, 14.2f, 0.22f, 0.02f, 0.22f));
    AddShape(detail, shape(12.5f, 0.75f, 14.4f, 0.22f, 0.30f, 0.02f));
    for (int i = 0; i < 4; i++) AddShape(detail, shape(12.5f + ((i & 1) ? 0.19f : -0.19f), 0.205f, 14.2f + ((i & 2) ? 0.19f : -0.19f), 0.02f, 0.205f, 0.02f));
    ps.boxes = box.tris, ps.detail = detail.tris;
    ps.casters.push_back({{detail.lo[0], detail.lo[1], detail.lo[2], 0}, {detail.hi[0], detail.hi[1], detail.hi[2], 0}});
    // A standing Sim: legs, torso, head, arms
    ps.capsules = {{{13.90f, 0.10f, 12.5f}, 0.09f, {13.90f, 0.85f, 12.5f}, 0}, {{14.10f, 0.10f, 12.5f}, 0.09f, {14.10f, 0.85f, 12.5f}, 0},
                   {{14.00f, 0.95f, 12.5f}, 0.17f, {14.00f, 1.40f, 12.5f}, 0}, {{14.00f, 1.60f, 12.5f}, 0.11f, {14.00f, 1.66f, 12.5f}, 0},
                   {{13.78f, 1.35f, 12.5f}, 0.05f, {13.72f, 0.85f, 12.5f}, 0}, {{14.22f, 1.35f, 12.5f}, 0.05f, {14.28f, 0.85f, 12.5f}, 0}};
    Box sim{{1e30f, 1e30f, 1e30f, 0}, {-1e30f, -1e30f, -1e30f, 0}};
    for (const Capsule& c : ps.capsules)
        for (int k = 0; k < 3; k++) sim.lo[k] = std::min({sim.lo[k], c.a[k] - c.r, c.b[k] - c.r}), sim.hi[k] = std::max({sim.hi[k], c.a[k] + c.r, c.b[k] + c.r});
    sim.lo[3] = 1.0f; // the Sim counts as moved this frame, the chair does not
    ps.casters.push_back(sim);
    // check.cpp's three lamps and four more: more than the cap in view, one behind a door, two behind walls
    ps.lamps = sc.lamps;
    ps.lamps.push_back({{10.8f, 1.6f, 10.8f}, 0.10f, {1.2f, 1.0f, 0.7f}, kRoomA, "floor lamp in A"});
    ps.lamps.push_back({{11.0f, 2.0f, 15.5f}, 0.08f, {0.8f, 0.8f, 1.0f}, kRoomA, "wall lamp in A"});
    ps.lamps.push_back({{19.0f, 2.5f, 13.0f}, 0.15f, {3.0f, 3.0f, 3.0f}, kRoomC, "ceiling lamp in C"});
    ps.lamps.push_back({{13.0f, 5.5f, 13.0f}, 0.15f, {3.0f, 3.0f, 3.0f}, kRoomUp, "ceiling lamp upstairs"});
    // The camera: in a top corner of room A, looking down at the table, the chair and the Sim
    ps.eye = {10.5f, 2.6f, 15.6f};
    ps.fwd = Unit(V3{13.5f, 0.6f, 12.6f} - ps.eye);
    ps.right = Unit(Cross({0, 1, 0}, ps.fwd)), ps.up = Cross(ps.fwd, ps.right);
    ps.tanY = std::tan(0.5f * 1.0472f), ps.tanX = ps.tanY * static_cast<float>(w) / static_cast<float>(h);
    return ps;
}

// pixel_shadow.comp SphereEnter, CapsuleEnter
float SphereEnter(V3 oc, V3 rd, float r) {
    const float b = Dot(rd, oc), h = b * b - (Dot(oc, oc) - r * r);
    return h > 0.0f ? -b - std::sqrt(h) : -1.0f;
}
float CapsuleEnter(V3 ro, V3 rd, V3 pa, V3 pb, float r) {
    const V3 ba = pb - pa, oa = ro - pa;
    const float baba = Dot(ba, ba), bard = Dot(ba, rd), baoa = Dot(ba, oa);
    const float a = baba - bard * bard;
    if (a > 1e-6f * baba) {
        const float b = baba * Dot(rd, oa) - baoa * bard, c = baba * Dot(oa, oa) - baoa * baoa - r * r * baba, h = b * b - a * c;
        if (h < 0.0f) return -1.0f;
        const float t = (-b - std::sqrt(h)) / a, y = baoa + t * bard;
        if (y > 0.0f && y < baba) return t;
        return SphereEnter(y <= 0.0f ? oa : ro - pb, rd, r);
    }
    return SphereEnter(bard > 0.0f ? oa : ro - pb, rd, r);
}

template <class F> void Rows(int n, F f) { // (half the cores: the machine is shared)
    std::atomic<int> next{0};
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < std::max(1u, std::thread::hardware_concurrency() / 2); t++)
        pool.emplace_back([&] {
            for (int i; (i = next++) < n;) f(i);
        });
    for (std::thread& t : pool) t.join();
}

// ---------------------------------------------------------------- the depth, drawn on the CPU
enum : uint8_t { kSky, kWalls, kBoxes, kChair, kSim };
const char* const kClassName[] = {"", "walls and floors", "object boxes", "chair", "Sim"};
struct Drawn {
    int w = 0, h = 0;
    std::vector<float> depth; // 24 bits, as the game's depth
    std::vector<V3> P, N;     // the surface under each pixel: its point and its normal toward the camera
    std::vector<uint8_t> cls;
};

Drawn Draw(const PScene& ps, int w, int h) {
    Drawn dr;
    dr.w = w, dr.h = h;
    const size_t n = static_cast<size_t>(w) * h;
    dr.depth.assign(n, 1.0f), dr.P.assign(n, {}), dr.N.assign(n, {}), dr.cls.assign(n, kSky);
    Rows(h, [&](int y) {
        for (int x = 0; x < w; x++) {
            const float nx = (x + 0.5f) / w * 2.0f - 1.0f, ny = 1.0f - (y + 0.5f) / h * 2.0f;
            const V3 d = ps.right * (nx * ps.tanX) + ps.up * (ny * ps.tanY) + ps.fwd; // t along d = the view depth
            float best = 1e30f;
            V3 N{};
            uint8_t cls = kSky;
            const auto tris = [&](const Tri* t, size_t count, uint8_t c) {
                for (size_t i = 0; i < count; i++) { // Moller-Trumbore, both sides
                    const V3 a = At(t[i].v[0]), e1 = At(t[i].v[1]) - a, e2 = At(t[i].v[2]) - a, p = Cross(d, e2);
                    const float det = Dot(e1, p);
                    if (std::fabs(det) < 1e-12f) continue;
                    const V3 s = ps.eye - a, q = Cross(s, e1);
                    const float u = Dot(s, p) / det, v = Dot(d, q) / det, tt = Dot(e2, q) / det;
                    if (u < 0.0f || v < 0.0f || u + v > 1.0f || tt <= kNear || tt >= best) continue;
                    best = tt, N = Unit(Cross(e1, e2)), cls = c;
                }
            };
            tris(ps.walls.tris.data(), ps.walls.tris.size(), kWalls);
            tris(ps.boxes.data(), ps.drawnBoxes, kBoxes);
            tris(ps.detail.data(), ps.detail.size(), kChair);
            tris(ps.shown.data(), ps.shown.size(), kChair);
            const float dl = Len(d);
            for (const Capsule& c : ps.capsules) {
                const float tt = CapsuleEnter(ps.eye, d * (1.0f / dl), At(c.a), At(c.b), c.r * kShrink) / dl;
                if (tt <= kNear || tt >= best) continue;
                const V3 hit = ps.eye + d * tt, ba = At(c.b) - At(c.a);
                best = tt, cls = kSim, N = Unit(hit - (At(c.a) + ba * std::clamp(Dot(hit - At(c.a), ba) / Dot(ba, ba), 0.0f, 1.0f)));
            }
            if (cls == kSky) continue;
            const size_t i = static_cast<size_t>(y) * w + x;
            const double z = kFar / (kFar - kNear) * (1.0 - kNear / best);
            dr.depth[i] = static_cast<float>(std::floor(z * 16777215.0 + 0.5) / 16777215.0);
            dr.P[i] = ps.eye + d * best, dr.N[i] = Dot(N, d) > 0.0f ? N * -1.0f : N, dr.cls[i] = cls;
        }
    });
    return dr;
}

// A pixel whose 3x3 neighbours lie on its own surface (elsewhere a normal from the depth is not defined)
bool Interior(const PScene& ps, const Drawn& dr, size_t i) {
    const int x = static_cast<int>(i % dr.w), y = static_cast<int>(i / dr.w);
    if (x < 1 || y < 1 || x >= dr.w - 1 || y >= dr.h - 1) return false;
    const float z = Dot(dr.P[i] - ps.eye, ps.fwd);
    for (int dy = -1; dy <= 1; dy++)
        for (int dx = -1; dx <= 1; dx++) {
            const size_t j = static_cast<size_t>(y + dy) * dr.w + x + dx;
            if (dr.cls[j] != dr.cls[i] || Dot(dr.N[j], dr.N[i]) < 0.95f || std::fabs(Dot(dr.P[j] - ps.eye, ps.fwd) - z) > 0.02f * z) return false;
        }
    return true;
}

// ---------------------------------------------------------------- the device
VkAccelerationStructureKHR Accel(const Gpu& g, Trash& trash, VkAccelerationStructureTypeKHR type, const VkAccelerationStructureGeometryKHR& geo, uint32_t count,
                                 VkDeviceAddress* address = nullptr, VkDeviceSize* bytes = nullptr) { // (check.cpp BuildAccel's step)
    VkAccelerationStructureBuildGeometryInfoKHR info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    info.type = type, info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR, info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
    info.geometryCount = 1, info.pGeometries = &geo;
    VkAccelerationStructureBuildSizesInfoKHR sz{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    vkGetAccelerationStructureBuildSizesKHR(g.dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, &count, &sz);
    if (bytes) *bytes = sz.accelerationStructureSize;
    const Buf store = MakeBuf(g, trash, sz.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, false);
    const Buf scratch = MakeBuf(g, trash, sz.buildScratchSize + 256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false);
    VkAccelerationStructureCreateInfoKHR ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    ci.buffer = store.b, ci.size = sz.accelerationStructureSize, ci.type = type;
    VkAccelerationStructureKHR as;
    Vk(vkCreateAccelerationStructureKHR(g.dev, &ci, nullptr, &as), "vkCreateAccelerationStructureKHR");
    trash.push_back([dev = g.dev, as] { vkDestroyAccelerationStructureKHR(dev, as, nullptr); });
    info.dstAccelerationStructure = as;
    info.scratchData.deviceAddress = (Addr(g, scratch) + 255) & ~VkDeviceAddress(255);
    VkAccelerationStructureBuildRangeInfoKHR range{count, 0, 0, 0};
    const VkAccelerationStructureBuildRangeInfoKHR* ranges = &range;
    Run(g, [&](VkCommandBuffer cb) { vkCmdBuildAccelerationStructuresKHR(cb, 1, &info, &ranges); });
    if (address) {
        VkAccelerationStructureDeviceAddressInfoKHR ai{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
        ai.accelerationStructure = as;
        *address = vkGetAccelerationStructureDeviceAddressKHR(g.dev, &ai);
    }
    return as;
}

// AddShape's triangles face inward. Verified by the checks only (without the flag 8 % of the static counts differ from
// the CPU reference), not against the specification's wording
constexpr VkGeometryInstanceFlagsKHR kShapeFacing = VK_GEOMETRY_INSTANCE_TRIANGLE_FRONT_COUNTERCLOCKWISE_BIT_KHR;

struct Kernel {
    const Gpu& g;
    Trash trash;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    Buf out, stats, back;
    PixPush pc{};
    uint32_t pixels = 0;

    Kernel(const Gpu& gpu, const PScene& ps, const Drawn& dr) : g(gpu) {
        const VkBufferUsageFlags sb = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, in = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR;
        // Bottom levels under one top level; the instance's custom index names the set to the shader, its mask to the ray
        std::vector<VkAccelerationStructureInstanceKHR> inst;
        const auto add = [&](uint32_t i, VkGeometryInstanceFlagsKHR flags, VkDeviceAddress blas, const float* xf = nullptr) {
            VkAccelerationStructureInstanceKHR in{};
            if (xf) std::memcpy(in.transform.matrix, xf, sizeof in.transform.matrix);
            else in.transform.matrix[0][0] = in.transform.matrix[1][1] = in.transform.matrix[2][2] = 1.0f;
            in.instanceCustomIndex = i, in.mask = i < 2 ? 1 : 2, in.flags = flags, in.accelerationStructureReference = blas;
            inst.push_back(in);
        };
        const auto blas = [&](const std::vector<Tri>& tris) {
            VkDeviceAddress at = 0;
            const Buf verts = DeviceBuf(g, trash, tris.size() * sizeof(Tri), in, tris.data(), tris.size() * sizeof(Tri));
            VkAccelerationStructureGeometryKHR geo{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
            // Opaque: the traversal confirms a triangle itself. Without the flag every triangle on a ray came back to the
            // shader as a candidate (the slow path of the first version, which the bench still runs through its ray flag)
            geo.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR, geo.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
            geo.geometry.triangles = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR};
            geo.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT, geo.geometry.triangles.vertexStride = 12;
            geo.geometry.triangles.vertexData.deviceAddress = Addr(g, verts), geo.geometry.triangles.maxVertex = static_cast<uint32_t>(tris.size()) * 3 - 1;
            geo.geometry.triangles.indexType = VK_INDEX_TYPE_NONE_KHR;
            Accel(g, trash, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, geo, static_cast<uint32_t>(tris.size()), &at);
            return at;
        };
        // Walls block from both sides. AddShape's triangles face inward: with kShapeFacing the ray's back-face culling drops
        // the faces a ray leaves through, so a ray from inside a box or a mesh part leaves it without a hit
        add(0, VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR, blas(ps.walls.tris));
        if (!ps.boxes.empty()) add(1, kShapeFacing, blas(ps.boxes));
        if (!ps.detail.empty()) add(2, ps.twoSidedDetail ? VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR : kShapeFacing, blas(ps.detail));
        for (const auto& m : ps.models) {
            const VkDeviceAddress at = blas(m);
            for (const PScene::Placed& o : ps.placed)
                if (&ps.models[o.model] == &m) add(1, VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR, at, o.xf); // static mesh casters, both sides
        }
        {
            std::vector<VkAabbPositionsKHR> boxes;
            for (const Capsule& c : ps.capsules)
                boxes.push_back({std::min(c.a[0], c.b[0]) - c.r, std::min(c.a[1], c.b[1]) - c.r, std::min(c.a[2], c.b[2]) - c.r,
                                 std::max(c.a[0], c.b[0]) + c.r, std::max(c.a[1], c.b[1]) + c.r, std::max(c.a[2], c.b[2]) + c.r});
            const Buf b = DeviceBuf(g, trash, boxes.size() * sizeof boxes[0], in, boxes.data(), boxes.size() * sizeof boxes[0]);
            VkAccelerationStructureGeometryKHR geo{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
            geo.geometryType = VK_GEOMETRY_TYPE_AABBS_KHR;
            geo.geometry.aabbs = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_AABBS_DATA_KHR};
            geo.geometry.aabbs.data.deviceAddress = Addr(g, b), geo.geometry.aabbs.stride = sizeof boxes[0];
            VkDeviceAddress at = 0;
            Accel(g, trash, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, geo, static_cast<uint32_t>(boxes.size()), &at);
            add(3, 0, at);
        }
        const size_t ib = inst.size() * sizeof inst[0];
        const Buf instances = DeviceBuf(g, trash, ib, in, inst.data(), ib);
        VkAccelerationStructureGeometryKHR top{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
        top.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
        top.geometry.instances = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR};
        top.geometry.instances.data.deviceAddress = Addr(g, instances);
        const VkAccelerationStructureKHR tlas = Accel(g, trash, VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, top, static_cast<uint32_t>(inst.size()));

        std::vector<GpuLamp> lamps;
        for (const Lamp& l : ps.lamps) lamps.push_back({{l.pos[0], l.pos[1], l.pos[2], l.radius}, {l.col[0], l.col[1], l.col[2], 0}});
        pixels = static_cast<uint32_t>(dr.w / 4) * (dr.h / 4);
        out = DeviceBuf(g, trash, static_cast<VkDeviceSize>(pixels) * sizeof(Pixel), sb);
        stats = DeviceBuf(g, trash, 16, sb);
        back = MakeBuf(g, trash, out.size + 16, 0);
        std::vector<uint32_t> motion(dr.cls.size()); // the Sim is class 3 (skinned), the rest 0
        for (size_t i = 0; i < motion.size(); i++) motion[i] = dr.cls[i] == kSim ? 3 : 0;
        const Buf bufs[7] = {DeviceBuf(g, trash, dr.depth.size() * 4, sb, dr.depth.data(), dr.depth.size() * 4),
                             DeviceBuf(g, trash, lamps.size() * sizeof(GpuLamp), sb, lamps.data(), lamps.size() * sizeof(GpuLamp)),
                             DeviceBuf(g, trash, ps.capsules.size() * sizeof(Capsule), sb, ps.capsules.data(), ps.capsules.size() * sizeof(Capsule)),
                             DeviceBuf(g, trash, ps.casters.size() * sizeof(Box), sb, ps.casters.data(), ps.casters.size() * sizeof(Box)), out, stats,
                             DeviceBuf(g, trash, motion.size() * 4, sb, motion.data(), motion.size() * 4)};

        std::vector<VkDescriptorSetLayoutBinding> binds = {{0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}};
        for (uint32_t i = 1; i <= 7; i++) binds.push_back({i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
        VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        lci.bindingCount = static_cast<uint32_t>(binds.size()), lci.pBindings = binds.data();
        VkDescriptorSetLayout dsl;
        Vk(vkCreateDescriptorSetLayout(g.dev, &lci, nullptr, &dsl), "vkCreateDescriptorSetLayout");
        const VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(PixPush)};
        VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        plci.setLayoutCount = 1, plci.pSetLayouts = &dsl, plci.pushConstantRangeCount = 1, plci.pPushConstantRanges = &pcr;
        Vk(vkCreatePipelineLayout(g.dev, &plci, nullptr, &layout), "vkCreatePipelineLayout");
        const VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 7}, {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1}};
        VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dpci.maxSets = 1, dpci.poolSizeCount = 2, dpci.pPoolSizes = sizes;
        VkDescriptorPool dpool;
        Vk(vkCreateDescriptorPool(g.dev, &dpci, nullptr, &dpool), "vkCreateDescriptorPool");
        VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dsai.descriptorPool = dpool, dsai.descriptorSetCount = 1, dsai.pSetLayouts = &dsl;
        Vk(vkAllocateDescriptorSets(g.dev, &dsai, &set), "vkAllocateDescriptorSets");
        VkDescriptorBufferInfo infos[7];
        VkWriteDescriptorSetAccelerationStructureKHR asw{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
        asw.accelerationStructureCount = 1, asw.pAccelerationStructures = &tlas;
        VkWriteDescriptorSet writes[8]{};
        for (uint32_t i = 0; i < 8; i++) {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, writes[i].dstSet = set, writes[i].dstBinding = i, writes[i].descriptorCount = 1;
            if (i == 0) writes[i].pNext = &asw, writes[i].descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
            else infos[i - 1] = {bufs[i - 1].b, 0, VK_WHOLE_SIZE}, writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, writes[i].pBufferInfo = &infos[i - 1];
        }
        vkUpdateDescriptorSets(g.dev, 8, writes, 0, nullptr);
        trash.push_back([this, dsl, dpool] {
            vkDestroyDescriptorPool(g.dev, dpool, nullptr), vkDestroyPipelineLayout(g.dev, layout, nullptr), vkDestroyDescriptorSetLayout(g.dev, dsl, nullptr);
        });
        pipeline = Variant(pixel_shadow, sizeof pixel_shadow);

        // The inverse view-projection in closed form (columns): world / view depth = right x + up y + forward + eye / view depth
        const float k = -(kFar - kNear) / (kNear * kFar);
        const V3 c0 = ps.right * ps.tanX, c1 = ps.up * ps.tanY, c2 = ps.eye * k, c3 = ps.fwd + ps.eye * (1.0f / kNear);
        const float inv[16] = {c0.x, c0.y, c0.z, 0, c1.x, c1.y, c1.z, 0, c2.x, c2.y, c2.z, k, c3.x, c3.y, c3.z, 1.0f / kNear};
        std::memcpy(pc.inv, inv, sizeof inv);
        pc.camA[0] = ps.eye.x, pc.camA[1] = ps.eye.y, pc.camA[2] = ps.eye.z, pc.camA[3] = kA;
        pc.width = static_cast<uint32_t>(dr.w), pc.height = static_cast<uint32_t>(dr.h), pc.bias = kBias;
        pc.lamps = static_cast<uint32_t>(ps.lamps.size()), pc.casters = static_cast<uint32_t>(ps.casters.size());
    }
    ~Kernel() {
        vkDeviceWaitIdle(g.dev);
        for (auto it = trash.rbegin(); it != trash.rend(); ++it) (*it)();
    }

    // A pipeline of one build of pixel_shadow.comp
    VkPipeline Variant(const uint32_t* code, size_t bytes) {
        VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        smci.codeSize = bytes, smci.pCode = code;
        VkShaderModule mod;
        Vk(vkCreateShaderModule(g.dev, &smci, nullptr, &mod), "vkCreateShaderModule");
        VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        cpci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, mod, "main", nullptr};
        cpci.layout = layout;
        VkPipeline p;
        Vk(vkCreateComputePipelines(g.dev, VK_NULL_HANDLE, 1, &cpci, nullptr, &p), "vkCreateComputePipelines");
        trash.push_back([dev = g.dev, p, mod] { vkDestroyPipeline(dev, p, nullptr), vkDestroyShaderModule(dev, mod, nullptr); });
        return p;
    }

    // The device's own time of the dispatch alone: a timestamp before and after each of n one-sample frames, so the
    // submit, the wait and the copies stay outside. gapMs < 0: all frames in one submit (the device stays busy, as
    // inside a game's frame). Else a submit a frame with that pause before it (an idle device lowers its clock).
    // Returns the milliseconds of each frame; wall: submit to idle of each submit.
    std::vector<double> Bench(VkPipeline p, uint32_t group, uint32_t n, int gapMs, std::vector<double>* wall = nullptr) {
        VkQueryPoolCreateInfo qci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qci.queryType = VK_QUERY_TYPE_TIMESTAMP, qci.queryCount = 2 * n;
        VkQueryPool qp;
        Vk(vkCreateQueryPool(g.dev, &qci, nullptr, &qp), "vkCreateQueryPool");
        pc.samples = 1, pc.radiusScale = 1.0f;
        const auto frames = [&](VkCommandBuffer cb, uint32_t from, uint32_t to) {
            vkCmdResetQueryPool(cb, qp, 2 * from, 2 * (to - from));
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
            for (uint32_t f = from; f < to; f++) {
                pc.frame = f;
                vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
                vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qp, 2 * f);
                vkCmdDispatch(cb, (pc.width / 4 + group - 1) / group, (pc.height / 4 + group - 1) / group, 1);
                vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qp, 2 * f + 1);
            }
        };
        if (gapMs < 0) {
            const double s = Run(g, [&](VkCommandBuffer cb) { frames(cb, 0, n); });
            if (wall) wall->push_back(s * 1e3);
        } else
            for (uint32_t f = 0; f < n; f++) {
                std::this_thread::sleep_for(std::chrono::milliseconds(gapMs));
                const double s = Run(g, [&](VkCommandBuffer cb) { frames(cb, f, f + 1); });
                if (wall) wall->push_back(s * 1e3);
            }
        std::vector<uint64_t> t(2 * n);
        Vk(vkGetQueryPoolResults(g.dev, qp, 0, 2 * n, t.size() * 8, t.data(), 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT), "vkGetQueryPoolResults");
        vkDestroyQueryPool(g.dev, qp, nullptr);
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(g.pd, &props);
        std::vector<double> ms;
        for (uint32_t f = 0; f < n; f++) ms.push_back(static_cast<double>(t[2 * f + 1] - t[2 * f]) * props.limits.timestampPeriod * 1e-6);
        return ms;
    }

    // One dispatch; the copy back is outside the time. stat: rays traced, pixels that traced. sec: submit to idle;
    // gpuMs: the dispatch alone by the device's timestamps
    std::vector<Pixel> Dispatch(uint32_t frame, uint32_t samples, float radiusScale, double* sec = nullptr, uint32_t* stat = nullptr, double* gpuMs = nullptr) {
        pc.frame = frame, pc.samples = samples, pc.radiusScale = radiusScale;
        VkQueryPoolCreateInfo qci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
        qci.queryType = VK_QUERY_TYPE_TIMESTAMP, qci.queryCount = 2;
        VkQueryPool qp;
        Vk(vkCreateQueryPool(g.dev, &qci, nullptr, &qp), "vkCreateQueryPool");
        const double s = Run(g, [&](VkCommandBuffer cb) {
            vkCmdFillBuffer(cb, stats.b, 0, VK_WHOLE_SIZE, 0);
            VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT, mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, pipeline);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
            vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
            vkCmdResetQueryPool(cb, qp, 0, 2);
            vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qp, 0);
            vkCmdDispatch(cb, (pc.width / 4 + 7) / 8, (pc.height / 4 + 7) / 8, 1);
            vkCmdWriteTimestamp(cb, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, qp, 1);
        });
        uint64_t t[2];
        Vk(vkGetQueryPoolResults(g.dev, qp, 0, 2, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT), "vkGetQueryPoolResults");
        vkDestroyQueryPool(g.dev, qp, nullptr);
        VkPhysicalDeviceProperties props;
        vkGetPhysicalDeviceProperties(g.pd, &props);
        if (gpuMs) *gpuMs = static_cast<double>(t[1] - t[0]) * props.limits.timestampPeriod * 1e-6;
        Run(g, [&](VkCommandBuffer cb) {
            const VkBufferCopy all{0, 0, out.size}, tail{0, out.size, 16};
            vkCmdCopyBuffer(cb, out.b, back.b, 1, &all), vkCmdCopyBuffer(cb, stats.b, back.b, 1, &tail);
        });
        if (sec) *sec = s;
        if (stat) std::memcpy(stat, static_cast<const char*>(back.p) + out.size, 8);
        const Pixel* p = static_cast<const Pixel*>(back.p);
        return std::vector<Pixel>(p, p + pixels);
    }
};

// ---------------------------------------------------------------- the reference
// pixel_shadow.comp Trace on the CPU: bit 0 a static occluder on the segment, else bit 1 a delta caster (when asked for)
uint32_t CpuTrace(const PScene& ps, V3 O, V3 e, bool delta) {
    const V3 d = e - O;
    const float len = Len(d);
    if (!(len > 2e-3f)) return 0;
    thread_local std::vector<uint32_t> stamp;
    thread_local uint32_t mark = 0;
    const float o[3] = {O.x, O.y, O.z}, b[3] = {e.x, e.y, e.z}, dd[3] = {d.x, d.y, d.z}, tMax = 1.0f - 1e-3f / len;
    if (!RtLampProxy::Clear(ps.walls, o, b, 0.0f, 1e-3f, &stamp, ++mark)) return 1;
    const auto tris = [&](const std::vector<Tri>& v) {
        for (const Tri& t : v) // AddShape's triangles face inward: only a ray that enters meets one
            if (Dot(Cross(At(t.v[1]) - At(t.v[0]), At(t.v[2]) - At(t.v[0])), d) > 0.0f && RtLampProxy::SegTri(o, dd, t, 0.0f, tMax)) return true;
        return false;
    };
    if (tris(ps.boxes) || !RtLampProxy::Clear(ps.meshes, o, b, 0.0f, 1e-3f, &stamp, ++mark)) return 1;
    if (!delta) return 0;
    if (ps.twoSidedDetail ? !RtLampProxy::Clear(ps.detailGrid, o, b, 0.0f, 1e-3f, &stamp, ++mark) : tris(ps.detail)) return 2;
    for (const Capsule& c : ps.capsules) {
        const float t = CapsuleEnter(O, d * (1.0f / len), At(c.a), At(c.b), c.r);
        if (t > 0.0f && t < len - 1e-3f) return 2;
    }
    return 0;
}

struct Counts { // packed as Pixel's
    uint32_t u = 0, d = 0;
};
// Every chosen lamp's rays with no reach test: the kernel's reach test must not lose a shadow
Counts Reference(const PScene& ps, V3 O, uint32_t sel, float radiusScale, uint32_t samples) {
    Counts c;
    for (uint32_t k = 0; k < kMaxLamps; k++) {
        const uint32_t id = sel >> (8 * k) & 0xFF;
        if (id == 0xFF) continue;
        Lamp l = ps.lamps[id];
        l.radius *= radiusScale;
        for (uint32_t s = 0; s < samples; s++) {
            float e[3];
            EmitterPoint(l, s, e);
            const uint32_t hit = CpuTrace(ps, O, At(e), true);
            if (!(hit & 1)) c.u += 1u << (8 * k);
            if (hit == 2) c.d += 1u << (8 * k);
        }
    }
    return c;
}
V3 Origin(const PScene& ps, const Pixel& p) { return At(p.pos) + At(p.nrm) * (kBias * (1.0f + 0.25f * Len(ps.eye - At(p.pos)))); }
float Geo(const Lamp& l, V3 P, V3 N) {
    const V3 L = At(l.pos) - P;
    const float d2 = Dot(L, L);
    return std::max(Dot(N, L), 0.0f) / std::sqrt(d2) / d2;
}
void Multiplier(const PScene& ps, const Pixel& p, Counts c, uint32_t samples, float out[3]) { // pixel_shadow.comp step 4
    float num[3] = {}, den[3] = {kA, kA, kA};
    for (uint32_t k = 0; k < kMaxLamps; k++) {
        const uint32_t id = p.sel >> (8 * k) & 0xFF;
        if (id == 0xFF) continue;
        const float ge = Geo(ps.lamps[id], At(p.pos), At(p.nrm)) / samples;
        for (int ch = 0; ch < 3; ch++) num[ch] += ps.lamps[id].col[ch] * ge * (c.d >> (8 * k) & 0xFF), den[ch] += ps.lamps[id].col[ch] * ge * (c.u >> (8 * k) & 0xFF);
    }
    for (int ch = 0; ch < 3; ch++) out[ch] = 1.0f - num[ch] / den[ch];
}
bool Drew(const Pixel& p) { return p.mul[3] < 1.0f; }

// The kernel's counts and multiplier against the reference from the kernel's own guide
void CheckOcclusion(const PScene& ps, const Drawn& dr, const std::vector<Pixel>& px, float radiusScale, uint32_t samples, const char* tag, double minShare) {
    const int gw = dr.w / 4, gh = dr.h / 4;
    std::vector<Counts> ref(px.size());
    Rows(gh, [&](int y) {
        for (int x = 0; x < gw; x++) {
            const Pixel& p = px[static_cast<size_t>(y) * gw + x];
            if (Drew(p)) ref[static_cast<size_t>(y) * gw + x] = Reference(ps, Origin(ps, p), p.sel, radiusScale, samples);
        }
    });
    long drew = 0, sameD = 0, sameU = 0, traced = 0, within = 0, shadowed[5] = {}, hidden = 0;
    float worst = 0.0f;
    for (size_t i = 0; i < px.size(); i++) {
        const Pixel& p = px[i];
        if (!Drew(p)) continue;
        drew++;
        if (p.d == ref[i].d) sameD++;
        if (p.rays) traced++, sameU += p.u == ref[i].u;
        if (ref[i].d) shadowed[dr.cls[static_cast<size_t>(p.pos[3])]]++;
        // (a lamp whose every sample a static occluder hides, on a pixel that traced: its delta casters cast nothing)
        for (uint32_t k = 0; k < kMaxLamps; k++) hidden += p.rays && (p.sel >> (8 * k) & 0xFF) != 0xFF && !(ref[i].u >> (8 * k) & 0xFF);
        float m[3], diff = 0.0f;
        Multiplier(ps, p, p.rays ? ref[i] : Counts{}, samples, m);
        for (int ch = 0; ch < 3; ch++) diff = std::max(diff, std::fabs(m[ch] - p.mul[ch]) * 255.0f);
        worst = std::max(worst, diff);
        if (diff <= 2.0f) within++;
    }
    std::printf("  [%s] %ld pixels drawn: delta counts equal on %.3f %%, static counts on %.3f %% of the %ld that traced; multiplier within 2/255 on %.3f %%, worst %.1f/255\n", tag,
                drew, 100.0 * sameD / drew, traced ? 100.0 * sameU / traced : 100.0, traced, 100.0 * within / drew, worst);
    std::printf("    pixels in a delta shadow: %ld on walls and floors, %ld on object boxes, %ld on the chair, %ld on the Sim; lamp slots a static occluder hides: %ld\n", shadowed[kWalls],
                shadowed[kBoxes], shadowed[kChair], shadowed[kSim], hidden);
    Check(within >= static_cast<long>(drew * minShare) && sameD >= static_cast<long>(drew * minShare), std::string(tag) + ": occlusion and multiplier match the CPU reference");
    Check(shadowed[kWalls] > 0 && shadowed[kSim] + shadowed[kChair] + shadowed[kBoxes] > 0, std::string(tag) + ": delta shadows fall on floors and on objects (the check is not empty)");
}
} // namespace

#ifndef PIXEL_CHECK_NO_MAIN // (mesh_check.cpp takes the scene, the kernel and the reference)
int main(int argc, char** argv) {
    std::string want;
    int w = 1920, h = 1080, bench = 0;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--device" && i + 1 < argc) want = argv[++i];
        else if (a == "--bench" && i + 1 < argc) bench = std::clamp(std::atoi(argv[++i]), 8, 4096);
        else if (a == "--size" && i + 2 < argc) w = std::clamp(std::atoi(argv[i + 1]), 64, 3840) & ~3, h = std::clamp(std::atoi(argv[i + 2]), 64, 2160) & ~3, i += 2;
        else {
            std::printf("FAIL  unknown argument %s\n", a.c_str());
            return 1;
        }
    }
    const PScene ps = BuildPixelScene(w, h);
    const Drawn dr = Draw(ps, w, h);
    const Gpu g = OpenGpu(want, true);
    std::printf("Per-pixel delta shadows on %s\n", g.name.c_str());
    if (!g.rays) {
        std::printf("FAIL  the device has no ray queries: the kernel did not run\n");
        return 1;
    }
    const int gw = w / 4, gh = h / 4;
    const uint32_t cap = static_cast<uint32_t>(gw) * gh * kMaxLamps;
    std::printf("  depth %d x %d, guide %d x %d; static %zu + %zu triangles, delta %zu triangles + %zu capsules; %zu lamps, at most %u a pixel; cap %u rays a frame\n", w, h, gw, gh,
                ps.walls.tris.size(), ps.boxes.size(), ps.detail.size(), ps.capsules.size(), ps.lamps.size(), kMaxLamps, cap);
    if (bench) {
        // The dispatch's time by the device's timestamps, the first eighth of the frames of each run left out (warm-up)
        Kernel kernel(g, ps, dr);
        uint32_t stat[2];
        kernel.Dispatch(0, 1, 1.0f, nullptr, stat);
        std::printf("  bench: %d frames a run, %u rays a frame; ms of the dispatch alone by device timestamps (least / median / 90 %%), wall = submit to idle of a submit\n", bench, stat[0]);
        struct Variant {
            const char* name;
            const uint32_t* code;
            size_t bytes;
            uint32_t group;
        };
        const Variant variants[] = {{"first version (every candidate to the shader)", px_trace0, sizeof px_trace0, 8},
                                    {"kernel: all triangles in the traversal", pixel_shadow, sizeof pixel_shadow, 8},
                                    {"kernel, groups of 16 x 16", px_group16, sizeof px_group16, 16},
                                    {"kernel, no counters, 16 bytes out", px_lean, sizeof px_lean, 8},
                                    {"no rays: guide + lamps + reach", px_stop3, sizeof px_stop3, 8},
                                    {"no rays: guide + lamps", px_stop2, sizeof px_stop2, 8},
                                    {"no rays: guide", px_stop1, sizeof px_stop1, 8}};
        const auto row = [&](std::vector<double> ms) {
            ms.erase(ms.begin(), ms.begin() + ms.size() / 8);
            std::sort(ms.begin(), ms.end());
            std::printf(" %7.3f /%7.3f /%7.3f", ms[0], ms[ms.size() / 2], ms[ms.size() * 9 / 10]);
        };
        std::vector<VkPipeline> pipes;
        for (const Variant& v : variants) pipes.push_back(kernel.Variant(v.code, v.bytes));
        for (int round = 0; round < 3; round++) // again: a run another process disturbed shows as a row that differs
            for (size_t i = 0; i < pipes.size(); i++) {
                std::vector<double> all, wall, wallIdle;
                std::printf("  %-48s one submit", variants[i].name);
                row(kernel.Bench(pipes[i], variants[i].group, static_cast<uint32_t>(bench), -1, &all));
                std::printf(" wall / frames %6.3f", all[0] / bench); // (a check of the timestamps: it holds the submit too)
                std::printf("  | a submit a frame");
                row(kernel.Bench(pipes[i], variants[i].group, static_cast<uint32_t>(bench), 0, &wall));
                std::printf(" wall");
                row(wall);
                std::printf("  | 16 ms idle before each");
                row(kernel.Bench(pipes[i], variants[i].group, static_cast<uint32_t>(std::min(bench, 64)), 16, &wallIdle));
                std::printf(" wall");
                row(wallIdle);
                std::printf("\n");
            }
    } else {
        Kernel kernel(g, ps, dr);
        kernel.Dispatch(0, 1, 1.0f); // (unprinted: the device's first pass runs slow)
        const std::vector<Pixel> point = kernel.Dispatch(0, 1, 0.0f), ball = kernel.Dispatch(0, 16, 1.0f);

        // 1. The guide
        {
            long drew = 0, picked = 0, close = 0, interior = 0, normal = 0;
            float worstP = 0.0f;
            for (int y = 0; y < gh; y++)
                for (int x = 0; x < gw; x++) {
                    const Pixel& p = ball[static_cast<size_t>(y) * gw + x];
                    size_t at = 0;
                    float d = 4.0f;
                    for (int by = 0; by < 4; by++)
                        for (int bx = 0; bx < 4; bx++) {
                            const size_t i = static_cast<size_t>(y * 4 + by) * w + x * 4 + bx;
                            if (dr.depth[i] < d) d = dr.depth[i], at = i;
                        }
                    if (d >= 1.0f) {
                        picked += !Drew(p);
                        continue;
                    }
                    drew++;
                    picked += Drew(p) && static_cast<size_t>(p.pos[3]) == at;
                    const float err = Len(At(p.pos) - dr.P[at]);
                    worstP = std::max(worstP, err);
                    close += err <= 5e-3f;
                    if (Interior(ps, dr, at)) interior++, normal += Dot(At(p.nrm), dr.N[at]) >= 0.99f;
                }
            std::printf("  guide: %ld of %d pixels drawn; position off by at most %.2f mm; normal within 8 degrees on %.2f %% of the %ld pixels inside a surface\n", drew, gw * gh,
                        worstP * 1e3f, interior ? 100.0 * normal / interior : 0.0, interior);
            Check(picked == static_cast<long>(gw) * gh, "guide: the nearest pixel of every 4x4 block");
            Check(close == drew, "guide: every position within 5 mm of the drawn surface");
            Check(normal >= static_cast<long>(interior * 0.99), "guide: the normal from the depth on 99 % of the pixels inside a surface");
        }

        // 2. The lamps of a pixel: at most 4, none stronger left out
        {
            long bad = 0, full = 0;
            for (const Pixel& p : ball) {
                if (!Drew(p)) continue;
                double least = 1e30;
                int n = 0;
                bool chosen[256] = {};
                for (uint32_t k = 0; k < kMaxLamps; k++) {
                    const uint32_t id = p.sel >> (8 * k) & 0xFF;
                    if (id == 0xFF) continue;
                    if (id >= ps.lamps.size() || chosen[id]) {
                        bad++;
                        continue;
                    }
                    chosen[id] = true, n++;
                    const Lamp& l = ps.lamps[id];
                    const double s = Geo(l, At(p.pos), At(p.nrm)) * std::max({l.col[0], l.col[1], l.col[2]});
                    least = std::min(least, s);
                    if (s < kA / 128.0 * 0.999) bad++;
                }
                full += n == static_cast<int>(kMaxLamps);
                for (size_t id = 0; id < ps.lamps.size(); id++) {
                    const Lamp& l = ps.lamps[id];
                    const double s = Geo(l, At(p.pos), At(p.nrm)) * std::max({l.col[0], l.col[1], l.col[2]});
                    if (!chosen[id] && s > kA / 128.0 * 1.001 && (n < static_cast<int>(kMaxLamps) || s > least * 1.001)) bad++;
                }
            }
            std::printf("  lamps: %ld pixels hold the full %u\n", full, kMaxLamps);
            Check(bad == 0 && full > 0, "lamps: every pixel holds its strongest, at most 4, and the cap is reached");
        }

        // 3. The occlusion
        CheckOcclusion(ps, dr, point, 0.0f, 1, "point emitters", 0.999);
        CheckOcclusion(ps, dr, ball, 1.0f, 16, "emitter balls, 16 samples", 0.99);

        // 4. Frames: the cap, and 16 of them against the 16-sample dispatch
        {
            std::vector<Counts> sum(ball.size());
            std::vector<double> gpu;
            std::vector<double> ms, sums(ball.size() * 9); // a pixel: num, den, the multiplier, 3 channels each
            uint32_t most = 0, mostPixels = 0;
            bool perPixel = true;
            std::vector<Pixel> first;
            for (uint32_t f = 0; f < 16; f++) {
                double sec, gpuMs;
                uint32_t stat[2];
                const std::vector<Pixel> px = kernel.Dispatch(f, 1, 1.0f, &sec, stat, &gpuMs);
                gpu.push_back(gpuMs);
                uint32_t rays = 0;
                for (size_t i = 0; i < px.size(); i++)
                    for (int c = 0; c < 3; c++) sums[i * 9 + c] += px[i].num[c], sums[i * 9 + 3 + c] += px[i].den[c], sums[i * 9 + 6 + c] += px[i].mul[c] / 16.0;
                for (size_t i = 0; i < px.size(); i++) sum[i].u += px[i].u, sum[i].d += px[i].d, rays += px[i].rays, perPixel = perPixel && px[i].rays <= kMaxLamps;
                perPixel = perPixel && rays == stat[0];
                most = std::max(most, stat[0]), mostPixels = std::max(mostPixels, stat[1]);
                ms.push_back(sec * 1e3);
                if (f == 0) first = px;
            }
            // Class 3 pixels (the Sim's) hold one point: their 16 frames are 16 times the first (8 bits a lamp hold it)
            long same = 0, fixed = 0, fixedSame = 0, fixedTraced = 0;
            double worstSums = 0.0, worstMean = 0.0; // against the 16-sample multiplier: from the two sums, from the mean of 16 multipliers
            for (size_t i = 0; i < ball.size(); i++) {
                const bool walks = !(Drew(first[i]) && dr.cls[static_cast<size_t>(first[i].pos[3])] == kSim);
                for (int c = 0; c < 3 && walks; c++) {
                    worstSums = std::max(worstSums, std::fabs(1.0 - sums[i * 9 + c] / (kA * 16.0 + sums[i * 9 + 3 + c]) - ball[i].mul[c]));
                    worstMean = std::max(worstMean, std::fabs(sums[i * 9 + 6 + c] - ball[i].mul[c]));
                }
                if (!walks)
                    fixed++, fixedTraced += first[i].rays != 0, fixedSame += sum[i].u == first[i].u * 16 && sum[i].d == first[i].d * 16;
                else same += sum[i].u == ball[i].u && sum[i].d == ball[i].d;
            }
            std::sort(ms.begin(), ms.end()), std::sort(gpu.begin(), gpu.end());
            // (the submit's time is the harness's: a submit and a wait a frame on a device that idles between them;
            // --bench measures the dispatch properly)
            std::printf("  a frame: %u rays at most = %.1f %% of the cap, from %.1f %% of the pixels; the dispatch by device timestamps %.3f ms at best, %.3f ms median; its submit to idle %.2f ms "
                        "at best, %.2f ms median\n",
                        most, 100.0 * most / cap, 100.0 * mostPixels / (gw * gh), gpu[0], gpu[8], ms[0], ms[8]);
            // The moved bit against the reach test on the CPU from the kernel's own guide (its ray origin). Float steps
            // differ at a box's edge, so 99.9 %
            long moved = 0, movedSame = 0, movedTraced = 0, traced = 0, drawn = 0;
            const Box& sim = ps.casters.back();
            for (const Pixel& p : first) {
                if (!Drew(p)) continue;
                const V3 P = At(p.pos), O = P + At(p.nrm) * (kBias * (1.0f + 0.25f * Len(ps.eye - P)));
                bool want = false;
                for (uint32_t k = 0; k < kMaxLamps; k++) {
                    const uint32_t id = p.sel >> (8 * k) & 0xFF;
                    if (id == 0xFF) continue;
                    const Lamp& l = ps.lamps[id];
                    float n = 0.0f, f = 1.0f; // the segment O -> lamp against the Sim's bounds grown by the emitter's radius
                    for (int a = 0; a < 3; a++) {
                        const float o = a == 0 ? O.x : a == 1 ? O.y : O.z;
                        float d = l.pos[a] - o;
                        if (std::fabs(d) < 1e-9f) d = 1e-9f;
                        const float t0 = (sim.lo[a] - l.radius - o) / d, t1 = (sim.hi[a] + l.radius - o) / d;
                        n = std::max(n, std::min(t0, t1)), f = std::min(f, std::max(t0, t1));
                    }
                    want = want || n <= f;
                }
                const bool got = p.num[3] != 0.0f;
                drawn++, moved += got, movedSame += got == want, movedTraced += got && p.rays, traced += p.rays != 0;
            }
            std::printf("  a moved caster in reach: %ld pixels of the %ld that traced\n", moved, traced);
            Check(moved > 0 && moved < traced && movedTraced == moved && movedSame >= static_cast<long>(drawn * 0.999),
                  "the moved bit: on pixels that traced only, not on the chair's alone, as the reach test on the CPU");
            Check(perPixel && most <= cap && most > 0, "rays: at most 4 a pixel and frame, the counter equals the pixels' sum, the frame within the cap");
            std::printf("  16 frames against the 16-sample multiplier: off by at most %.3f/255 from the two sums, %.1f/255 from the mean of the frames' multipliers\n", worstSums * 255.0,
                        worstMean * 255.0);
            Check(worstSums * 255.0 < 0.01, "the two sums of 16 frames give the 16-sample multiplier");
            Check(same == static_cast<long>(ball.size()) - fixed, "16 accumulated frames equal the 16-sample dispatch on every pixel outside class 3");
            std::printf("  class 3: %ld pixels, %ld of them traced\n", fixed, fixedTraced);
            Check(fixedSame == fixed && fixedTraced > 0, "a class 3 pixel traces the same point in each of 16 frames");
            const std::vector<Pixel> again = kernel.Dispatch(0, 1, 1.0f);
            Check(!std::memcmp(again.data(), first.data(), first.size() * sizeof(Pixel)), "a frame traced again is byte-equal");
        }

        // 5. No self-shadow: where the drawn surface itself (its exact point and normal) is lit, the kernel's pixel is lit
        {
            std::vector<Counts> ideal(point.size());
            Rows(gh, [&](int y) {
                for (int x = 0; x < gw; x++) {
                    const Pixel& p = point[static_cast<size_t>(y) * gw + x];
                    if (!Drew(p)) continue;
                    const size_t at = static_cast<size_t>(p.pos[3]);
                    ideal[static_cast<size_t>(y) * gw + x] = Reference(ps, dr.P[at] + dr.N[at] * 1e-4f, p.sel, 0.0f, 1);
                }
            });
            long lit[5] = {}, acne[5] = {}, leak = 0;
            for (size_t i = 0; i < point.size(); i++) {
                const Pixel& p = point[i];
                if (!Drew(p) || !Interior(ps, dr, static_cast<size_t>(p.pos[3]))) continue;
                const uint8_t cls = dr.cls[static_cast<size_t>(p.pos[3])];
                if (ideal[i].d) leak += !p.d;
                else lit[cls]++, acne[cls] += p.d != 0;
            }
            std::printf("  self-shadow (point emitters, pixels inside a surface): lit pixels the kernel shadows:");
            bool ok = true;
            for (int c = kWalls; c <= kSim; c++) {
                std::printf(" %s %ld of %ld%s", kClassName[c], acne[c], lit[c], c < kSim ? "," : "");
                ok = ok && lit[c] > 0 && acne[c] <= lit[c] / 200;
            }
            std::printf("; shadowed pixels it leaves lit: %ld\n", leak);
            Check(ok, "no self-shadow: at most 0.5 % of the lit pixels of each kind (shadow edges moved by the origin's offset)");
        }
    }
    vkDestroyCommandPool(g.dev, g.pool, nullptr);
    vkDestroyDevice(g.dev, nullptr);
    vkDestroyInstance(g.inst, nullptr);
    std::printf(g_failed ? "FAILED: %d check(s)\n" : "All checks passed\n", g_failed);
    return g_failed ? 1 : 0;
}
#endif
