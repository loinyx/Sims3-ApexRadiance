// The server tier's texel solve, standalone (docs/features/realtime-gi/design.md 3.4, 3.5, 3.9; no bridge, no game):
// the occluder proxy of a synthetic two-story lot (features/rt_lamp_proxy.h: a lamp room with a window and two object
// boxes, a room joined to it by a door, a closed room, a closed room upstairs) on a Vulkan device, solved by solve.comp
// on both back ends: hardware ray queries, and the same kernel walking the proxy's grid with no ray extension.
//
//  1. Links: per texel, the texels its cosine-weighted rays hit (lamp independent, traced once).
//  2. Per lamp, its own layer: direct light by shadow rays to the emitter ball, then bounces as gathers over the links
//     until the layer settles (feedback, no ray).
// Checked: no leak (a lamp adds exactly 0 to every texel of a room no opening joins, direct and indirect); the door room
// is lit; the direct light against the CPU reference (RtLampProxy::Clear, the same emitter points); the energy balance
// of the closed room (absorbed = emitted); both back ends against each other; a second solve byte-equal.
// Printed: rays a second, the bounces and the time to settle. Read-only: prints to stdout, writes no file.
//   --no-rays        the compute back end only          --device <text>  the device whose name holds <text>
//   --density <n>    texels a metre (4)                  --rays <n>       rays a texel (256)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define VK_NO_PROTOTYPES
#define VOLK_IMPLEMENTATION
#include "volk.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <string>
#include <vector>
#include "features/rt_lamp_proxy.h"
// SPIR-V of solve.comp, built by run.ps1
#include "links_rq.h"
#include "direct_rq.h"
#include "links_sw.h"
#include "direct_sw.h"
#include "bounce.h"

namespace {
int g_failed = 0;
void Check(bool ok, const std::string& what) {
    std::printf("  %s  %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) g_failed++;
}
void Vk(VkResult r, const char* what) {
    if (r == VK_SUCCESS) return;
    std::printf("FAIL  %s: VkResult %d\n", what, static_cast<int>(r));
    std::exit(1);
}

// ---------------------------------------------------------------- the scene
// shortcut: stories are 3 m and walls lie on tile lines (the world-cell map of solve.comp TexelAt); a captured lot with
// other story heights or diagonal walls needs a texel index a proxy triangle instead, upgrade with the server solve.
constexpr float kStory = 3.0f;
constexpr int kLot = 24; // tiles a side
struct Texel {
    float pos[4]; // w: room
    float nrm[4];
    float alb[4];
};
struct Lamp {
    float pos[3], radius, col[3];
    int room;
    const char* name;
};
struct Scene {
    RtLampProxy::Mesh mesh;
    std::vector<Texel> texels;
    std::vector<int32_t> map;
    std::vector<Lamp> lamps;
    int stories = 2, density = 4, lot = kLot + 8; // lot: tiles a side of the map (solve.comp lotSize)
};
enum { kOut = 0, kRoomA = 1, kRoomC = 2, kRoomB = 3, kRoomUp = 4 };
const char* const kRoomName[] = {"outside", "A (lamp, window, boxes)", "C (door to A)", "B (closed)", "upstairs (closed)"};

int RoomOf(int story, int x, int z) {
    if (z < 10 || z >= 16 || x < 10) return kOut;
    if (story == 1) return x < 16 ? kRoomUp : kOut;
    return x < 16 ? kRoomA : x < 22 ? kRoomC : x < 28 ? kRoomB : kOut;
}

Scene BuildScene(int density) {
    using namespace RtLampProxy;
    Scene sc;
    sc.density = density;
    Lot lot;
    for (int s = 0; s < sc.stories; s++) {
        Story st;
        st.story = s, st.w = st.h = kLot + 8;
        const size_t n = static_cast<size_t>(st.w) * st.h;
        st.floorY.assign(n, kNoFloor), st.roofed.assign(n, 0), st.ceil.assign(n, kNoFloor);
        const float foot = kStory * s;
        st.plane = foot;
        for (int z = 0; z < st.h; z++)
            for (int x = 0; x < st.w; x++) {
                const int r = RoomOf(s, x, z);
                if (r) st.floorY[static_cast<size_t>(z) * st.w + x] = foot, st.roofed[static_cast<size_t>(z) * st.w + x] = 0xF, st.ceil[static_cast<size_t>(z) * st.w + x] = foot + kStory;
                // a wall on every line between two different rooms (or a room and outside), toward -x and -z
                for (int a = 0; a < 2; a++) {
                    const int o = RoomOf(s, x - (a == 0), z - (a == 1));
                    if (o == r) continue;
                    Edge e;
                    e.x0 = static_cast<float>(x), e.z0 = static_cast<float>(z), e.x1 = static_cast<float>(x + (a == 1)), e.z1 = static_cast<float>(z + (a == 0));
                    e.foot = e.base = foot, e.topL = e.topM = e.topR = foot + kStory;
                    if (s == 0 && a == 0 && x == 16 && z == 12) { // the door between A and C
                        e.open = true, e.o.u0 = 0.1f, e.o.u1 = 0.9f, e.o.sill = 0.0f;
                        for (float& h : e.o.head) h = 2.2f;
                    }
                    if (s == 0 && a == 1 && x == 12 && z == 10) { // A's window to the outside
                        e.open = true, e.o.u0 = 0.1f, e.o.u1 = 0.9f, e.o.sill = 0.85f;
                        for (float& h : e.o.head) h = 2.25f;
                    }
                    st.edges.push_back(e);
                }
            }
        lot.stories.push_back(std::move(st));
    }
    Mesh& m = sc.mesh;
    Build(lot, m);
    // Floors (the proxy holds none on the ground and a 0.2 m slab upstairs that would cover the ceiling texels below)
    m.curKind = Mesh::kSlab;
    for (const Story& st : lot.stories)
        for (int z = 0; z < st.h; z++)
            for (int x = 0; x < st.w; x++) {
                const float y = st.floorY[static_cast<size_t>(z) * st.w + x];
                if (!(y < kNoFloor * 0.5f)) continue;
                const float a[3] = {float(x), y, float(z)}, b[3] = {float(x + 1), y, float(z)}, c[3] = {float(x + 1), y, float(z + 1)}, d[3] = {float(x), y, float(z + 1)};
                AddTri(m, a, b, c), AddTri(m, a, c, d);
            }
    const ObjShape boxes[] = {{{12.5f, 0.4f, 13.0f}, {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}, {0.8f, 0.4f, 0.5f}},   // a table
                              {{15.5f, 1.0f, 11.0f}, {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}, {0.3f, 1.0f, 0.6f}}};  // a cabinet
    for (const ObjShape& b : boxes) AddShape(m, b);
    BuildGrid(m);

    // Texels and the world-cell map (solve.comp TexelAt mirrors the index)
    const int S = sc.stories, G = kLot + 8, D = density, GD = G * D;
    sc.map.assign(static_cast<size_t>(2) * S * GD * GD + static_cast<size_t>(4) * S * (G + 1) * GD * 3 * D, -1);
    const float step = 1.0f / D;
    const auto add = [&](size_t cell, float x, float y, float z, float nx, float ny, float nz, int room, const float alb[3]) {
        sc.map[cell] = static_cast<int32_t>(sc.texels.size());
        sc.texels.push_back({{x, y, z, static_cast<float>(room)}, {nx, ny, nz, 0}, {alb[0], alb[1], alb[2], 0}});
    };
    const float kFloor[3] = {0.50f, 0.40f, 0.30f}, kCeil[3] = {0.80f, 0.80f, 0.80f}, kWall[3] = {0.70f, 0.70f, 0.70f}, kRed[3] = {0.70f, 0.15f, 0.10f};
    for (int s = 0; s < S; s++) {
        for (int iz = 0; iz < GD; iz++)
            for (int ix = 0; ix < GD; ix++) {
                const int room = RoomOf(s, ix / D, iz / D);
                if (!room) continue;
                const float x = (ix + 0.5f) * step, z = (iz + 0.5f) * step;
                add((static_cast<size_t>(0 * S + s) * GD + iz) * GD + ix, x, kStory * s + 0.02f, z, 0, 1, 0, room, kFloor);
                add((static_cast<size_t>(1 * S + s) * GD + iz) * GD + ix, x, kStory * (s + 1) - 0.02f, z, 0, -1, 0, room, kCeil);
            }
        for (const Edge& e : lot.stories[s].edges) {
            const int axis = e.x1 == e.x0 ? 0 : 1; // the wall's normal: x, z
            const int line = static_cast<int>(axis == 0 ? e.x0 : e.z0), along0 = static_cast<int>(axis == 0 ? e.z0 : e.x0) * D;
            for (int side = 0; side < 2; side++) {
                const float sg = side ? 1.0f : -1.0f;
                const int tx = axis == 0 ? line - (side ? 0 : 1) : static_cast<int>(e.x0), tz = axis == 1 ? line - (side ? 0 : 1) : static_cast<int>(e.z0);
                const int room = RoomOf(s, tx, tz);
                if (!room) continue; // an outdoor face: no texel
                for (int k = 0; k < D; k++)
                    for (int h = 0; h < 3 * D; h++) {
                        const float u = (k + 0.5f) * step, y = (h + 0.5f) * step;
                        if (e.open && u > e.o.u0 && u < e.o.u1 && y > e.o.sill && y < OpeningHead(e.o, u)) continue;
                        const size_t cell = static_cast<size_t>(2) * S * GD * GD + ((((static_cast<size_t>(axis) * 2 + side) * S + s) * (G + 1) + line) * GD + along0 + k) * (3 * D) + h;
                        const float px = axis == 0 ? line + 0.05f * sg : e.x0 + u, pz = axis == 1 ? line + 0.05f * sg : e.z0 + u;
                        add(cell, px, kStory * s + y, pz, axis == 0 ? sg : 0, 0, axis == 1 ? sg : 0, room, room == kRoomA && axis == 0 && line == 10 ? kRed : kWall);
                    }
            }
        }
    }
    sc.lamps = {{{13.0f, 2.5f, 13.0f}, 0.15f, {3.0f, 3.0f, 3.0f}, kRoomA, "ceiling lamp in A"},
                {{25.0f, 2.5f, 13.0f}, 0.15f, {3.0f, 3.0f, 3.0f}, kRoomB, "ceiling lamp in closed B"},
                {{14.5f, 1.2f, 15.0f}, 0.10f, {1.5f, 1.2f, 0.8f}, kRoomA, "table lamp in A"}};
    return sc;
}

// ---------------------------------------------------------------- Vulkan
struct Gpu {
    VkInstance inst = VK_NULL_HANDLE;
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties mem{};
    uint32_t family = 0;
    bool rays = false;
    std::string name;
};
struct Buf {
    VkBuffer b = VK_NULL_HANDLE;
    VkDeviceMemory m = VK_NULL_HANDLE;
    void* p = nullptr;
    VkDeviceSize size = 0;
};
using Trash = std::vector<std::function<void()>>;

bool HasExt(VkPhysicalDevice pd, const char* name) {
    uint32_t n = 0;
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &n, nullptr);
    std::vector<VkExtensionProperties> v(n);
    vkEnumerateDeviceExtensionProperties(pd, nullptr, &n, v.data());
    return std::any_of(v.begin(), v.end(), [&](const VkExtensionProperties& e) { return !std::strcmp(e.extensionName, name); });
}
bool HasRays(VkPhysicalDevice pd) {
    return HasExt(pd, VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME) && HasExt(pd, VK_KHR_RAY_QUERY_EXTENSION_NAME) && HasExt(pd, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME);
}

Gpu OpenGpu(const std::string& want, bool allowRays) {
    Gpu g;
    Vk(volkInitialize(), "the Vulkan loader");
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_2;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    Vk(vkCreateInstance(&ici, nullptr, &g.inst), "vkCreateInstance");
    volkLoadInstance(g.inst);
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(g.inst, &n, nullptr);
    std::vector<VkPhysicalDevice> pds(n);
    vkEnumeratePhysicalDevices(g.inst, &n, pds.data());
    for (int pass = 0; pass < 2 && !g.pd; pass++) // first a device with ray queries, then any
        for (VkPhysicalDevice pd : pds) {
            VkPhysicalDeviceProperties p;
            vkGetPhysicalDeviceProperties(pd, &p);
            if (!want.empty() && !std::strstr(p.deviceName, want.c_str())) continue;
            if (pass == 0 && !(HasRays(pd) && p.apiVersion >= VK_API_VERSION_1_2)) continue;
            g.pd = pd, g.name = p.deviceName, g.rays = allowRays && pass == 0;
            break;
        }
    if (!g.pd) {
        std::printf("FAIL  no Vulkan device%s%s\n", want.empty() ? "" : " named ", want.c_str());
        std::exit(1);
    }
    vkGetPhysicalDeviceMemoryProperties(g.pd, &g.mem);
    vkGetPhysicalDeviceQueueFamilyProperties(g.pd, &n, nullptr);
    std::vector<VkQueueFamilyProperties> qf(n);
    vkGetPhysicalDeviceQueueFamilyProperties(g.pd, &n, qf.data());
    for (uint32_t i = 0; i < n; i++)
        if (qf[i].queueFlags & VK_QUEUE_COMPUTE_BIT) {
            g.family = i;
            break;
        }
    const float prio = 1.0f;
    VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    qci.queueFamilyIndex = g.family, qci.queueCount = 1, qci.pQueuePriorities = &prio;
    VkPhysicalDeviceRayQueryFeaturesKHR rq{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_RAY_QUERY_FEATURES_KHR};
    rq.rayQuery = VK_TRUE;
    VkPhysicalDeviceAccelerationStructureFeaturesKHR as{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ACCELERATION_STRUCTURE_FEATURES_KHR, &rq};
    as.accelerationStructure = VK_TRUE;
    VkPhysicalDeviceBufferDeviceAddressFeatures bda{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_BUFFER_DEVICE_ADDRESS_FEATURES, &as};
    bda.bufferDeviceAddress = VK_TRUE;
    const char* exts[] = {VK_KHR_ACCELERATION_STRUCTURE_EXTENSION_NAME, VK_KHR_RAY_QUERY_EXTENSION_NAME, VK_KHR_DEFERRED_HOST_OPERATIONS_EXTENSION_NAME};
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1, dci.pQueueCreateInfos = &qci;
    if (g.rays) dci.pNext = &bda, dci.enabledExtensionCount = 3, dci.ppEnabledExtensionNames = exts; // the compute back end enables nothing
    Vk(vkCreateDevice(g.pd, &dci, nullptr, &g.dev), "vkCreateDevice");
    volkLoadDevice(g.dev);
    vkGetDeviceQueue(g.dev, g.family, 0, &g.queue);
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.queueFamilyIndex = g.family;
    Vk(vkCreateCommandPool(g.dev, &pci, nullptr, &g.pool), "vkCreateCommandPool");
    return g;
}

// A buffer in device memory, or (host) a mapped one in host memory: the solve's buffers live on the device, as the
// server's will (a shader reading host memory ran the bounce passes about 30 times slower); the harness copies in and out
Buf MakeBuf(const Gpu& g, Trash& trash, VkDeviceSize size, VkBufferUsageFlags usage, bool host = true, const void* data = nullptr, size_t bytes = 0) {
    Buf b;
    b.size = std::max<VkDeviceSize>(size, 16);
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = b.size, bci.usage = usage | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | (g.rays ? VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT : 0);
    Vk(vkCreateBuffer(g.dev, &bci, nullptr, &b.b), "vkCreateBuffer");
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(g.dev, b.b, &mr);
    const VkMemoryPropertyFlags need = host ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < g.mem.memoryTypeCount && type == UINT32_MAX; i++)
        if ((mr.memoryTypeBits & (1u << i)) && (g.mem.memoryTypes[i].propertyFlags & need) == need) type = i;
    VkMemoryAllocateFlagsInfo fl{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    fl.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, g.rays ? &fl : nullptr};
    mai.allocationSize = mr.size, mai.memoryTypeIndex = type;
    Vk(type == UINT32_MAX ? VK_ERROR_FEATURE_NOT_PRESENT : vkAllocateMemory(g.dev, &mai, nullptr, &b.m), "buffer memory");
    Vk(vkBindBufferMemory(g.dev, b.b, b.m, 0), "vkBindBufferMemory");
    trash.push_back([dev = g.dev, b] { vkDestroyBuffer(dev, b.b, nullptr), vkFreeMemory(dev, b.m, nullptr); });
    if (!host) return b;
    Vk(vkMapMemory(g.dev, b.m, 0, VK_WHOLE_SIZE, 0, &b.p), "vkMapMemory");
    std::memset(b.p, 0, static_cast<size_t>(b.size));
    if (data) std::memcpy(b.p, data, bytes);
    return b;
}
VkDeviceAddress Addr(const Gpu& g, const Buf& b) {
    VkBufferDeviceAddressInfo i{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    i.buffer = b.b;
    return vkGetBufferDeviceAddress(g.dev, &i);
}
// Records, submits and waits; returns the seconds from submit to idle
double Run(const Gpu& g, const std::function<void(VkCommandBuffer)>& record) {
    VkCommandBufferAllocateInfo ai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    ai.commandPool = g.pool, ai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, ai.commandBufferCount = 1;
    VkCommandBuffer cb;
    Vk(vkAllocateCommandBuffers(g.dev, &ai, &cb), "vkAllocateCommandBuffers");
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cb, &bi);
    record(cb);
    Vk(vkEndCommandBuffer(cb), "vkEndCommandBuffer");
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1, si.pCommandBuffers = &cb;
    const auto t0 = std::chrono::steady_clock::now();
    Vk(vkQueueSubmit(g.queue, 1, &si, VK_NULL_HANDLE), "vkQueueSubmit");
    Vk(vkQueueWaitIdle(g.queue), "vkQueueWaitIdle");
    const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    vkFreeCommandBuffers(g.dev, g.pool, 1, &cb);
    return s;
}

// A device buffer holding data (or zeros)
Buf DeviceBuf(const Gpu& g, Trash& trash, VkDeviceSize size, VkBufferUsageFlags usage, const void* data = nullptr, size_t bytes = 0) {
    const Buf b = MakeBuf(g, trash, size, usage, false);
    Trash once;
    const Buf stage = MakeBuf(g, once, b.size, 0, true, data, bytes);
    Run(g, [&](VkCommandBuffer cb) {
        const VkBufferCopy all{0, 0, b.size};
        vkCmdCopyBuffer(cb, stage.b, b.b, 1, &all);
    });
    once[0]();
    return b;
}

// One bottom-level structure over the proxy's triangles and a top level with its one instance
VkAccelerationStructureKHR BuildAccel(const Gpu& g, Trash& trash, const Buf& verts, uint32_t tris) {
    const auto build = [&](VkAccelerationStructureTypeKHR type, const VkAccelerationStructureGeometryKHR& geo, uint32_t count) {
        VkAccelerationStructureBuildGeometryInfoKHR info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
        info.type = type, info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR, info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR;
        info.geometryCount = 1, info.pGeometries = &geo;
        VkAccelerationStructureBuildSizesInfoKHR sz{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
        vkGetAccelerationStructureBuildSizesKHR(g.dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, &count, &sz);
        const Buf store = MakeBuf(g, trash, sz.accelerationStructureSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR, false);
        const Buf scratch = MakeBuf(g, trash, sz.buildScratchSize + 256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, false);
        VkAccelerationStructureCreateInfoKHR ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
        ci.buffer = store.b, ci.size = sz.accelerationStructureSize, ci.type = type;
        VkAccelerationStructureKHR as;
        Vk(vkCreateAccelerationStructureKHR(g.dev, &ci, nullptr, &as), "vkCreateAccelerationStructureKHR");
        trash.push_back([dev = g.dev, as] { vkDestroyAccelerationStructureKHR(dev, as, nullptr); });
        info.dstAccelerationStructure = as;
        info.scratchData.deviceAddress = (Addr(g, scratch) + 255) & ~VkDeviceAddress(255); // (the scratch alignment is at most 256)
        VkAccelerationStructureBuildRangeInfoKHR range{count, 0, 0, 0};
        const VkAccelerationStructureBuildRangeInfoKHR* ranges = &range;
        Run(g, [&](VkCommandBuffer cb) { vkCmdBuildAccelerationStructuresKHR(cb, 1, &info, &ranges); });
        return as;
    };
    VkAccelerationStructureGeometryKHR tri{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    tri.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR, tri.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    tri.geometry.triangles = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR};
    tri.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT, tri.geometry.triangles.vertexStride = 16;
    tri.geometry.triangles.vertexData.deviceAddress = Addr(g, verts), tri.geometry.triangles.maxVertex = tris * 3 - 1;
    tri.geometry.triangles.indexType = VK_INDEX_TYPE_NONE_KHR;
    const VkAccelerationStructureKHR blas = build(VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, tri, tris);
    VkAccelerationStructureDeviceAddressInfoKHR ai{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
    ai.accelerationStructure = blas;
    VkAccelerationStructureInstanceKHR one{};
    one.transform.matrix[0][0] = one.transform.matrix[1][1] = one.transform.matrix[2][2] = 1.0f;
    one.mask = 0xFF, one.flags = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
    one.accelerationStructureReference = vkGetAccelerationStructureDeviceAddressKHR(g.dev, &ai);
    const Buf inst = DeviceBuf(g, trash, sizeof one, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, &one, sizeof one);
    VkAccelerationStructureGeometryKHR top{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    top.geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR, top.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    top.geometry.instances = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR};
    top.geometry.instances.data.deviceAddress = Addr(g, inst);
    return build(VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR, top, 1);
}

// ---------------------------------------------------------------- the solve
struct Push { // solve.comp PC
    float lampPos[4], lampCol[4];
    uint32_t texels, rays, lamp, src, shadowRays, stories, lotSize, density, lamps;
    int32_t gx0, gz0, gw, gh;
};
struct Result {
    std::vector<uint32_t> links;
    std::vector<float> direct, ind; // lamps x texels x 4
    std::vector<int> bounces;
    double accelSec = 0, linkSec = 0, directSec = 0, bounceSec = 0, batchSec = 0; // wall time of the passes (submit + wait a dispatch)
};
constexpr uint32_t kShadowRays = 16, kMaxBounces = 64;
constexpr float kSettled = 0.1f / 255.0f; // a layer is settled when no texel moved more than this in a bounce

// batchBounces > 0: afterwards, every lamp's direct pass and that many bounces again in one submission (batchSec: the
// layers' GPU time without a submit and a wait a dispatch)
Result Solve(const Gpu& g, bool rays, const Scene& sc, uint32_t nRays, bool print, int batchBounces = 0) {
    Trash trash;
    const RtLampProxy::Mesh& m = sc.mesh;
    const uint32_t T = static_cast<uint32_t>(sc.texels.size()), L = static_cast<uint32_t>(sc.lamps.size()), nTris = static_cast<uint32_t>(m.tris.size());
    std::vector<float> verts(static_cast<size_t>(nTris) * 12);
    for (uint32_t i = 0; i < nTris; i++)
        for (int v = 0; v < 3; v++) {
            std::memcpy(&verts[(static_cast<size_t>(i) * 3 + v) * 4], m.tris[i].v[v], 12);
            verts[(static_cast<size_t>(i) * 3 + v) * 4 + 3] = static_cast<float>(m.kind[i]);
        }
    const VkBufferUsageFlags sb = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
    const Buf bufs[8] = {
        DeviceBuf(g, trash, verts.size() * 4, sb | (rays ? VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR : 0), verts.data(), verts.size() * 4),
        DeviceBuf(g, trash, m.cellStart.size() * 4, sb, m.cellStart.data(), m.cellStart.size() * 4),
        DeviceBuf(g, trash, m.cellTris.size() * 4, sb, m.cellTris.data(), m.cellTris.size() * 4),
        DeviceBuf(g, trash, sc.texels.size() * sizeof(Texel), sb, sc.texels.data(), sc.texels.size() * sizeof(Texel)),
        DeviceBuf(g, trash, sc.map.size() * 4, sb, sc.map.data(), sc.map.size() * 4),
        DeviceBuf(g, trash, static_cast<VkDeviceSize>(T) * nRays * 4, sb),
        DeviceBuf(g, trash, static_cast<VkDeviceSize>(L) * T * 16, sb),
        DeviceBuf(g, trash, static_cast<VkDeviceSize>(2) * L * T * 16, sb)};
    double accelSec = 0.0;
    VkAccelerationStructureKHR tlas = VK_NULL_HANDLE;
    if (rays) {
        const auto t0 = std::chrono::steady_clock::now();
        tlas = BuildAccel(g, trash, bufs[0], nTris);
        accelSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    }

    std::vector<VkDescriptorSetLayoutBinding> binds;
    if (rays) binds.push_back({0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
    for (uint32_t i = 1; i <= 8; i++) binds.push_back({i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
    VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    lci.bindingCount = static_cast<uint32_t>(binds.size()), lci.pBindings = binds.data();
    VkDescriptorSetLayout dsl;
    Vk(vkCreateDescriptorSetLayout(g.dev, &lci, nullptr, &dsl), "vkCreateDescriptorSetLayout");
    const VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push)};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    plci.setLayoutCount = 1, plci.pSetLayouts = &dsl, plci.pushConstantRangeCount = 1, plci.pPushConstantRanges = &pcr;
    VkPipelineLayout layout;
    Vk(vkCreatePipelineLayout(g.dev, &plci, nullptr, &layout), "vkCreatePipelineLayout");
    const VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 8}, {VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1}};
    VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    dpci.maxSets = 1, dpci.poolSizeCount = rays ? 2u : 1u, dpci.pPoolSizes = sizes;
    VkDescriptorPool dpool;
    Vk(vkCreateDescriptorPool(g.dev, &dpci, nullptr, &dpool), "vkCreateDescriptorPool");
    VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dsai.descriptorPool = dpool, dsai.descriptorSetCount = 1, dsai.pSetLayouts = &dsl;
    VkDescriptorSet set;
    Vk(vkAllocateDescriptorSets(g.dev, &dsai, &set), "vkAllocateDescriptorSets");
    VkDescriptorBufferInfo infos[8];
    std::vector<VkWriteDescriptorSet> writes;
    for (uint32_t i = 0; i < 8; i++) {
        infos[i] = {bufs[i].b, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = set, w.dstBinding = i + 1, w.descriptorCount = 1, w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, w.pBufferInfo = &infos[i];
        writes.push_back(w);
    }
    VkWriteDescriptorSetAccelerationStructureKHR asw{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
    asw.accelerationStructureCount = 1, asw.pAccelerationStructures = &tlas;
    if (rays) {
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, &asw};
        w.dstSet = set, w.dstBinding = 0, w.descriptorCount = 1, w.descriptorType = VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR;
        writes.push_back(w);
    }
    vkUpdateDescriptorSets(g.dev, static_cast<uint32_t>(writes.size()), writes.data(), 0, nullptr);
    const auto pipeline = [&](const uint32_t* code, size_t bytes) {
        VkShaderModuleCreateInfo smci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        smci.codeSize = bytes, smci.pCode = code;
        VkShaderModule mod;
        Vk(vkCreateShaderModule(g.dev, &smci, nullptr, &mod), "vkCreateShaderModule");
        VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        cpci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, mod, "main", nullptr};
        cpci.layout = layout;
        VkPipeline p;
        Vk(vkCreateComputePipelines(g.dev, VK_NULL_HANDLE, 1, &cpci, nullptr, &p), "vkCreateComputePipelines");
        trash.push_back([dev = g.dev, mod, p] { vkDestroyPipeline(dev, p, nullptr), vkDestroyShaderModule(dev, mod, nullptr); });
        return p;
    };
    const VkPipeline pLinks = rays ? pipeline(links_rq, sizeof links_rq) : pipeline(links_sw, sizeof links_sw);
    const VkPipeline pDirect = rays ? pipeline(direct_rq, sizeof direct_rq) : pipeline(direct_sw, sizeof direct_sw);
    const VkPipeline pBounce = pipeline(bounce, sizeof bounce);
    trash.push_back([dev = g.dev, dsl, layout, dpool] {
        vkDestroyDescriptorPool(dev, dpool, nullptr), vkDestroyPipelineLayout(dev, layout, nullptr), vkDestroyDescriptorSetLayout(dev, dsl, nullptr);
    });

    Push pc{};
    pc.texels = T, pc.rays = nRays, pc.shadowRays = kShadowRays, pc.stories = static_cast<uint32_t>(sc.stories), pc.lotSize = static_cast<uint32_t>(sc.lot);
    pc.density = static_cast<uint32_t>(sc.density), pc.lamps = L, pc.gx0 = m.gx0, pc.gz0 = m.gz0, pc.gw = m.gw, pc.gh = m.gh;
    const auto pass = [&](VkPipeline p, uint32_t groupsY) {
        return Run(g, [&](VkCommandBuffer cb) {
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
            vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
            vkCmdDispatch(cb, (T + 63) / 64, groupsY, 1);
        });
    };
    Result res;
    const double linkSec = pass(pLinks, nRays);
    double directSec = 0.0, bounceSec = 0.0;
    // Read-back (outside the timed passes): a range of a device buffer, through one mapped buffer
    const Buf back = MakeBuf(g, trash, std::max({bufs[5].size, bufs[6].size, bufs[7].size}), 0);
    const auto read = [&](const Buf& from, VkDeviceSize offset, VkDeviceSize bytes, VkDeviceSize to) {
        Run(g, [&](VkCommandBuffer cb) {
            const VkBufferCopy part{offset, to, bytes};
            vkCmdCopyBuffer(cb, from.b, back.b, 1, &part);
        });
    };
    const float* ind = static_cast<const float*>(back.p);
    const VkDeviceSize layer = static_cast<VkDeviceSize>(T) * 16;
    for (uint32_t l = 0; l < L; l++) {
        const Lamp& lamp = sc.lamps[l];
        std::memcpy(pc.lampPos, lamp.pos, 12), pc.lampPos[3] = lamp.radius;
        std::memcpy(pc.lampCol, lamp.col, 12);
        pc.lamp = l, pc.src = 0;
        directSec += pass(pDirect, 1);
        int n = 0;
        for (float moved = 1.0f; n < static_cast<int>(kMaxBounces) && moved > kSettled; n++) {
            bounceSec += pass(pBounce, 1);
            pc.src ^= 1u;
            moved = 0.0f;
            read(bufs[7], l * layer, layer, 0), read(bufs[7], (L + l) * layer, layer, layer); // the lamp's two feedback halves
            const float *a = ind, *b = ind + static_cast<size_t>(T) * 4;
            for (size_t k = 0; k < static_cast<size_t>(T) * 4; k++) moved = std::max(moved, std::fabs(a[k] - b[k]));
        }
        res.bounces.push_back(n);
        const float* last = ind + static_cast<size_t>(pc.src) * T * 4; // (src now names the half written last)
        res.ind.insert(res.ind.end(), last, last + static_cast<size_t>(T) * 4);
    }
    read(bufs[5], 0, static_cast<VkDeviceSize>(T) * nRays * 4, 0);
    res.links.assign(static_cast<const uint32_t*>(back.p), static_cast<const uint32_t*>(back.p) + static_cast<size_t>(T) * nRays);
    read(bufs[6], 0, L * layer, 0);
    res.direct.assign(ind, ind + static_cast<size_t>(L) * T * 4);
    res.accelSec = accelSec, res.linkSec = linkSec, res.directSec = directSec, res.bounceSec = bounceSec;
    for (uint32_t l0 = 0; batchBounces > 0 && l0 < L; l0 += 8) // 8 lamps a submission: far from the driver's watchdog
        res.batchSec += Run(g, [&](VkCommandBuffer cb) {
            VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
            mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT, mb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
            for (uint32_t l = l0; l < std::min(L, l0 + 8); l++) {
                const Lamp& lamp = sc.lamps[l];
                std::memcpy(pc.lampPos, lamp.pos, 12), pc.lampPos[3] = lamp.radius;
                std::memcpy(pc.lampCol, lamp.col, 12);
                pc.lamp = l, pc.src = 0;
                for (int b = -1; b < batchBounces; b++) {
                    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, b < 0 ? pDirect : pBounce);
                    vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
                    vkCmdDispatch(cb, (T + 63) / 64, 1, 1);
                    vkCmdPipelineBarrier(cb, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
                    if (b >= 0) pc.src ^= 1u;
                }
            }
        });
    if (print) {
        const double linkRays = static_cast<double>(T) * nRays;
        std::printf("  links: %.2f M rays in %.1f ms = %.0f M rays a second", linkRays / 1e6, linkSec * 1e3, linkRays / linkSec / 1e6);
        if (rays) std::printf(" (acceleration structures, %u triangles: %.1f ms)", nTris, accelSec * 1e3);
        std::printf("\n  %u lamps: direct (up to %u shadow rays a texel) %.1f ms; bounces", L, kShadowRays, directSec * 1e3);
        for (int b : res.bounces) std::printf(" %d", b);
        std::printf(" in %.1f ms (no ray); time to settle from nothing %.1f ms, a lamp change %.1f ms\n", bounceSec * 1e3,
                    (accelSec + linkSec + directSec + bounceSec) * 1e3, (directSec + bounceSec) * 1e3 / L);
    }
    vkDeviceWaitIdle(g.dev);
    for (auto it = trash.rbegin(); it != trash.rend(); ++it) (*it)();
    return res;
}

// ---------------------------------------------------------------- the checks
void EmitterPoint(const Lamp& l, uint32_t k, float out[3]) { // solve.comp PASS 1
    const float y = 1.0f - (2.0f * k + 1.0f) / kShadowRays, rr = std::sqrt(1.0f - y * y), a = 2.39996323f * k;
    out[0] = l.pos[0] + l.radius * rr * std::cos(a), out[1] = l.pos[1] + l.radius * y, out[2] = l.pos[2] + l.radius * rr * std::sin(a);
}
float Byte(float e) { return std::min(e, 1.0f) * 255.0f; }

void CheckResult(const Scene& sc, const Result& r, const char* tag) {
    const size_t T = sc.texels.size();
    for (size_t l = 0; l < sc.lamps.size(); l++) {
        const Lamp& lamp = sc.lamps[l];
        const float *dir = &r.direct[l * T * 4], *ind = &r.ind[l * T * 4];
        long count[5] = {}, lit[5] = {}, litInd[5] = {};
        double sumInd[5] = {};
        for (size_t i = 0; i < T; i++) {
            const int room = static_cast<int>(sc.texels[i].pos[3]);
            count[room]++;
            if (dir[i * 4] != 0.0f || dir[i * 4 + 1] != 0.0f || dir[i * 4 + 2] != 0.0f) lit[room]++;
            if (ind[i * 4] != 0.0f || ind[i * 4 + 1] != 0.0f || ind[i * 4 + 2] != 0.0f) litInd[room]++;
            sumInd[room] += ind[i * 4 + 1];
        }
        std::printf("  [%s] %s:\n", tag, lamp.name);
        for (int room = 1; room <= 4; room++) {
            // joined to the lamp's room by an opening: A and C by the door
            const bool joined = room == lamp.room || (lamp.room == kRoomA && room == kRoomC) || (lamp.room == kRoomC && room == kRoomA);
            std::printf("    room %-24s %5ld texels, direct on %5ld, indirect on %5ld, mean indirect %.4f\n", kRoomName[room], count[room], lit[room], litInd[room],
                        count[room] ? sumInd[room] / count[room] : 0.0);
            if (!joined) Check(lit[room] == 0 && litInd[room] == 0, std::string("no leak: exactly 0 in room ") + kRoomName[room]);
            else Check(litInd[room] > count[room] / 2 && (room != lamp.room || lit[room] > count[room] / 2), std::string("lit: room ") + kRoomName[room]);
        }
        // The direct light against the CPU reference: the same emitter points against the same triangles
        long within = 0;
        float worst = 0.0f;
        for (size_t i = 0; i < T; i++) {
            const Texel& t = sc.texels[i];
            const float L[3] = {lamp.pos[0] - t.pos[0], lamp.pos[1] - t.pos[1], lamp.pos[2] - t.pos[2]};
            const float d2 = L[0] * L[0] + L[1] * L[1] + L[2] * L[2], c = (t.nrm[0] * L[0] + t.nrm[1] * L[1] + t.nrm[2] * L[2]) / std::sqrt(d2);
            float share = 0.0f;
            if (c > 0.0f)
                for (uint32_t k = 0; k < kShadowRays; k++) {
                    float e[3];
                    EmitterPoint(lamp, k, e);
                    if (RtLampProxy::Clear(sc.mesh, t.pos, e, 1e-3f, 1e-3f)) share += 1.0f / kShadowRays;
                }
            const float diff = std::fabs(Byte(lamp.col[1] * std::max(c, 0.0f) / d2 * share) - Byte(dir[i * 4 + 1]));
            worst = std::max(worst, diff);
            if (diff <= 2.0f) within++;
        }
        std::printf("    direct against the CPU reference: %.2f %% of texels within 2/255, worst %.1f/255\n", 100.0 * within / T, worst);
        Check(within >= static_cast<long>(T * 0.99), "direct light matches the CPU reference on 99 % of texels");
        if (lamp.room == kRoomB) {
            // A closed room with no absorber but its surfaces: what they absorb is what the lamp sent (every texel is the same area)
            double sent = 0.0, absorbed = 0.0;
            for (size_t i = 0; i < T; i++)
                for (int k = 0; k < 3; k++) sent += dir[i * 4 + k], absorbed += (1.0 - sc.texels[i].alb[k]) * (dir[i * 4 + k] + ind[i * 4 + k]);
            std::printf("    energy balance of the closed room: absorbed / sent = %.4f\n", absorbed / sent);
            Check(std::fabs(absorbed / sent - 1.0) < 0.03, "multi-bounce energy balance within 3 %");
        }
    }
}

void CheckParity(const Scene& sc, const Result& a, const Result& b) {
    long linkDiff = 0, within = 0;
    for (size_t i = 0; i < a.links.size(); i++) linkDiff += a.links[i] != b.links[i];
    double sa = 0.0, sb = 0.0;
    float worst = 0.0f;
    const size_t n = a.direct.size() / 4;
    for (size_t i = 0; i < n; i++) {
        float d = 0.0f;
        for (int k = 0; k < 3; k++) {
            const float x = a.direct[i * 4 + k] + a.ind[i * 4 + k], y = b.direct[i * 4 + k] + b.ind[i * 4 + k];
            sa += x, sb += y, d = std::max(d, std::fabs(Byte(x) - Byte(y)));
        }
        worst = std::max(worst, d);
        if (d <= 2.0f) within++;
    }
    std::printf("  ray queries against the compute back end: %ld of %zu links differ (%.4f %%); %.2f %% of lamp texels within 2/255, worst %.1f/255; mean ratio %.4f\n",
                linkDiff, a.links.size(), 100.0 * linkDiff / a.links.size(), 100.0 * within / n, worst, sb / sa);
    Check(linkDiff <= static_cast<long>(a.links.size() / 1000), "back ends: at most 0.1 % of links differ");
    Check(within >= static_cast<long>(n * 0.99) && std::fabs(sb / sa - 1.0) < 0.01, "back ends: 99 % of texels within 2/255, mean within 1 %");
    (void)sc;
}
} // namespace

int main(int argc, char** argv) {
    bool allowRays = true;
    std::string want;
    int density = 4, nRays = 256;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--no-rays") allowRays = false;
        else if (a == "--device" && i + 1 < argc) want = argv[++i];
        else if (a == "--density" && i + 1 < argc) density = std::clamp(std::atoi(argv[++i]), 1, 16);
        else if (a == "--rays" && i + 1 < argc) nRays = std::clamp(std::atoi(argv[++i]), 1, 4096);
        else {
            std::printf("FAIL  unknown argument %s\n", a.c_str());
            return 1;
        }
    }
    const Scene sc = BuildScene(density);
    Gpu g = OpenGpu(want, allowRays);
    std::printf("Texel solve on %s (ray queries: %s)\n", g.name.c_str(), g.rays ? "yes" : allowRays ? "not available" : "off");
    std::printf("  proxy: %zu triangles (%d walls, %d openings, 2 boxes); %zu texels at %d a metre, %d rays a texel, %zu lamps\n", sc.mesh.tris.size(), sc.mesh.walls,
                sc.mesh.openings, sc.texels.size(), density, nRays, sc.lamps.size());
    Result rq;
    if (g.rays) {
        std::printf("Ray-query back end\n");
        const Result again = Solve(g, true, sc, static_cast<uint32_t>(nRays), false); // (unprinted: the device's first passes run slow)
        rq = Solve(g, true, sc, static_cast<uint32_t>(nRays), true);
        CheckResult(sc, rq, "rays");
        Check(again.links == rq.links && !std::memcmp(again.direct.data(), rq.direct.data(), rq.direct.size() * 4) && !std::memcmp(again.ind.data(), rq.ind.data(), rq.ind.size() * 4),
              "a second solve is byte-equal");
    }
    std::printf("Compute back end (no ray extension in the shaders%s)\n", g.rays ? "" : " or on the device");
    const Result sw = Solve(g, false, sc, static_cast<uint32_t>(nRays), true);
    CheckResult(sc, sw, "compute");
    if (g.rays) CheckParity(sc, rq, sw);
    vkDestroyCommandPool(g.dev, g.pool, nullptr);
    vkDestroyDevice(g.dev, nullptr);
    vkDestroyInstance(g.inst, nullptr);
    std::printf(g_failed ? "FAILED: %d check(s)\n" : "All checks passed\n", g_failed);
    return g_failed ? 1 : 0;
}
