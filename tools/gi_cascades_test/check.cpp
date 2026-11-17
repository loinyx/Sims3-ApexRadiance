// Real-time GI, X-05: the radiance cascades as the server tier would run them (cascades.comp, a Vulkan 1.1 compute
// shader, no ray extension) against their CPU form (features/plan_cascades.h): every level's image and the gather, value
// by value, on the doorway case of tools/rt_lamps_test/plan_cascades_case.h, with and without the fix; then the time of the
// dispatches on a 64 x 64 tile story by level count. Read-only: prints to stdout, writes no file.
//   --device <text>  the device whose name holds <text>
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
static int checks = 0, failures = 0;
static void Check(bool ok, const std::string& text) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL: %s\n", text.c_str());
    }
}
#include "tools/rt_lamps_test/plan_cascades_case.h"
// SPIR-V of cascades.comp, built by run.ps1
#include "cascade.h"
#include "gather.h"

namespace {
using namespace PlanCascadesTest;
using namespace PlanCascades;

void Vk(VkResult r, const char* what) {
    if (r == VK_SUCCESS) return;
    std::printf("FAIL  %s: VkResult %d\n", what, static_cast<int>(r));
    std::exit(1);
}
struct Gpu {
    VkInstance inst = VK_NULL_HANDLE;
    VkPhysicalDevice pd = VK_NULL_HANDLE;
    VkDevice dev = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties mem{};
    uint32_t family = 0;
    std::string name;
};
struct Buf {
    VkBuffer b = VK_NULL_HANDLE;
    VkDeviceMemory m = VK_NULL_HANDLE;
    void* p = nullptr;
    VkDeviceSize size = 0;
};
using Trash = std::vector<std::function<void()>>;

Gpu OpenGpu(const std::string& want) {
    Gpu g;
    Vk(volkInitialize(), "the Vulkan loader");
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_1;
    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    Vk(vkCreateInstance(&ici, nullptr, &g.inst), "vkCreateInstance");
    volkLoadInstance(g.inst);
    uint32_t n = 0;
    vkEnumeratePhysicalDevices(g.inst, &n, nullptr);
    std::vector<VkPhysicalDevice> pds(n);
    vkEnumeratePhysicalDevices(g.inst, &n, pds.data());
    for (int pass = 0; pass < 2 && !g.pd; pass++) // first a discrete device, then any
        for (VkPhysicalDevice pd : pds) {
            VkPhysicalDeviceProperties p;
            vkGetPhysicalDeviceProperties(pd, &p);
            if (!want.empty() && !std::strstr(p.deviceName, want.c_str())) continue;
            if (pass == 0 && p.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU) continue;
            g.pd = pd, g.name = p.deviceName;
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
    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1, dci.pQueueCreateInfos = &qci;
    Vk(vkCreateDevice(g.pd, &dci, nullptr, &g.dev), "vkCreateDevice");
    volkLoadDevice(g.dev);
    vkGetDeviceQueue(g.dev, g.family, 0, &g.queue);
    VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pci.queueFamilyIndex = g.family;
    Vk(vkCreateCommandPool(g.dev, &pci, nullptr, &g.pool), "vkCreateCommandPool");
    return g;
}
// A storage buffer in device memory, or (host) a mapped one (the images live on the device, as in tools/gi_server_test)
Buf MakeBuf(const Gpu& g, Trash& trash, VkDeviceSize size, bool host) {
    Buf b;
    b.size = std::max<VkDeviceSize>(size, 16);
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = b.size, bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    Vk(vkCreateBuffer(g.dev, &bci, nullptr, &b.b), "vkCreateBuffer");
    VkMemoryRequirements mr;
    vkGetBufferMemoryRequirements(g.dev, b.b, &mr);
    const VkMemoryPropertyFlags need = host ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    uint32_t type = UINT32_MAX;
    for (uint32_t i = 0; i < g.mem.memoryTypeCount && type == UINT32_MAX; i++)
        if ((mr.memoryTypeBits & (1u << i)) && (g.mem.memoryTypes[i].propertyFlags & need) == need) type = i;
    VkMemoryAllocateInfo mai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    mai.allocationSize = mr.size, mai.memoryTypeIndex = type;
    Vk(type == UINT32_MAX ? VK_ERROR_FEATURE_NOT_PRESENT : vkAllocateMemory(g.dev, &mai, nullptr, &b.m), "buffer memory");
    Vk(vkBindBufferMemory(g.dev, b.b, b.m, 0), "vkBindBufferMemory");
    trash.push_back([dev = g.dev, b] { vkDestroyBuffer(dev, b.b, nullptr), vkFreeMemory(dev, b.m, nullptr); });
    if (host) Vk(vkMapMemory(g.dev, b.m, 0, VK_WHOLE_SIZE, 0, &b.p), "vkMapMemory");
    return b;
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
Buf DeviceBuf(const Gpu& g, Trash& trash, const void* data, size_t bytes) {
    const Buf b = MakeBuf(g, trash, bytes, false);
    Trash once;
    const Buf stage = MakeBuf(g, once, b.size, true);
    std::memcpy(stage.p, data, bytes);
    Run(g, [&](VkCommandBuffer cb) {
        const VkBufferCopy all{0, 0, bytes};
        vkCmdCopyBuffer(cb, stage.b, b.b, 1, &all);
    });
    once[0]();
    return b;
}
std::vector<float> ReadBuf(const Gpu& g, const Buf& b, size_t floats) {
    Trash once;
    const Buf stage = MakeBuf(g, once, floats * 4, true);
    Run(g, [&](VkCommandBuffer cb) {
        const VkBufferCopy all{0, 0, floats * 4};
        vkCmdCopyBuffer(cb, b.b, stage.b, 1, &all);
    });
    std::vector<float> out(floats);
    std::memcpy(out.data(), stage.p, floats * 4);
    once[0]();
    return out;
}

struct Push { // cascades.comp PC
    float fld[4], lvl[4], upl[4], cfg[4];
    int32_t flg[4];
};
struct Solver {
    const Gpu& g;
    Trash trash;
    VkDescriptorSetLayout dsl = VK_NULL_HANDLE;
    VkPipelineLayout layout = VK_NULL_HANDLE;
    VkDescriptorPool dpool = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkPipeline pCascade = VK_NULL_HANDLE, pGather = VK_NULL_HANDLE;

    explicit Solver(const Gpu& gpu) : g(gpu) {
        VkDescriptorSetLayoutBinding binds[4];
        for (uint32_t i = 0; i < 4; i++) binds[i] = {i, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        lci.bindingCount = 4, lci.pBindings = binds;
        Vk(vkCreateDescriptorSetLayout(g.dev, &lci, nullptr, &dsl), "vkCreateDescriptorSetLayout");
        const VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(Push)};
        VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        plci.setLayoutCount = 1, plci.pSetLayouts = &dsl, plci.pushConstantRangeCount = 1, plci.pPushConstantRanges = &pcr;
        Vk(vkCreatePipelineLayout(g.dev, &plci, nullptr, &layout), "vkCreatePipelineLayout");
        const VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4};
        VkDescriptorPoolCreateInfo dpci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        dpci.maxSets = 1, dpci.poolSizeCount = 1, dpci.pPoolSizes = &size;
        Vk(vkCreateDescriptorPool(g.dev, &dpci, nullptr, &dpool), "vkCreateDescriptorPool");
        VkDescriptorSetAllocateInfo dsai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        dsai.descriptorPool = dpool, dsai.descriptorSetCount = 1, dsai.pSetLayouts = &dsl;
        Vk(vkAllocateDescriptorSets(g.dev, &dsai, &set), "vkAllocateDescriptorSets");
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
        pCascade = pipeline(cascade, sizeof cascade), pGather = pipeline(gather, sizeof gather);
    }
    ~Solver() {
        for (auto it = trash.rbegin(); it != trash.rend(); ++it) (*it)();
        vkDestroyDescriptorPool(g.dev, dpool, nullptr), vkDestroyPipelineLayout(g.dev, layout, nullptr), vkDestroyDescriptorSetLayout(g.dev, dsl, nullptr);
    }
    double Dispatch(VkPipeline p, const Buf bufs[4], const Push& pc, int w, int h) {
        VkDescriptorBufferInfo infos[4];
        VkWriteDescriptorSet writes[4];
        for (uint32_t i = 0; i < 4; i++) {
            infos[i] = {bufs[i].b, 0, VK_WHOLE_SIZE};
            writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            writes[i].dstSet = set, writes[i].dstBinding = i, writes[i].descriptorCount = 1, writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, writes[i].pBufferInfo = &infos[i];
        }
        vkUpdateDescriptorSets(g.dev, 4, writes, 0, nullptr);
        return Run(g, [&](VkCommandBuffer cb) {
            vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, p);
            vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, layout, 0, 1, &set, 0, nullptr);
            vkCmdPushConstants(cb, layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
            vkCmdDispatch(cb, (w + 7) / 8, (h + 7) / 8, 1);
        });
    }
};
struct GpuOut {
    std::vector<std::vector<float>> lv;
    std::vector<float> out; // nx x nz x kOut
    std::vector<double> ms; // per level
};
// Every level, last to first, and the gather. read: the images are copied back
GpuOut Solve(Solver& sv, const Field& f, const Emit& e, const Params& p, const Layout& l, bool read) {
    Trash trash;
    const size_t n = static_cast<size_t>(f.w) * f.h, texels = static_cast<size_t>(l.tw) * l.th;
    const Buf field = DeviceBuf(sv.g, trash, &f.d[n * p.band], n * 4), emit = DeviceBuf(sv.g, trash, e.v.data(), e.v.size() * 4);
    std::vector<Buf> lv;
    for (int i = 0; i < p.levels; i++) lv.push_back(MakeBuf(sv.g, trash, texels * 16, false));
    const Buf gat = MakeBuf(sv.g, trash, static_cast<size_t>(l.nx) * l.nz * 32, false);
    GpuOut r;
    r.lv.resize(p.levels), r.ms.assign(p.levels, 0.0);
    Push pc{};
    const float fld[4] = {static_cast<float>(f.px), static_cast<float>(PlanField::kPad), static_cast<float>(f.w), static_cast<float>(f.h)};
    std::memcpy(pc.fld, fld, sizeof fld);
    for (int i = p.levels - 1; i >= 0; i--) {
        const Level& g = l.lv[i];
        const bool top = i + 1 == p.levels;
        const Level up = top ? Level{1, 1, 1, 1, 1.0f, 0, 0} : l.lv[i + 1];
        const float lvl[4] = {g.spacing, static_cast<float>(g.side), g.t0, g.t1}, upl[4] = {up.spacing, static_cast<float>(up.side), static_cast<float>(up.nx), static_cast<float>(up.nz)};
        const float cfg[4] = {static_cast<float>(l.tw), static_cast<float>(l.th), static_cast<float>(g.dirs * 4), p.capFar};
        const int32_t flg[4] = {!top, p.fix, p.steps, p.d0};
        std::memcpy(pc.lvl, lvl, sizeof lvl), std::memcpy(pc.upl, upl, sizeof upl), std::memcpy(pc.cfg, cfg, sizeof cfg), std::memcpy(pc.flg, flg, sizeof flg);
        const Buf bufs[4] = {field, emit, top ? emit : lv[i + 1], lv[i]};
        r.ms[i] = 1000.0 * sv.Dispatch(sv.pCascade, bufs, pc, l.tw, l.th);
        if (read) r.lv[i] = ReadBuf(sv.g, lv[i], texels * 4);
    }
    const Buf bufs[4] = {field, emit, lv[0], gat};
    sv.Dispatch(sv.pGather, bufs, pc, l.nx, l.nz);
    if (read) r.out = ReadBuf(sv.g, gat, static_cast<size_t>(l.nx) * l.nz * kOut);
    for (auto it = trash.rbegin(); it != trash.rend(); ++it) (*it)();
    return r;
}
} // namespace

int main(int argc, char** argv) {
    (void)&PlanFieldChecks, (void)&PlanCascadesChecks; // (the CPU cases run in tools/rt_lamps_test)
    std::string want;
    for (int i = 1; i < argc; i++)
        if (std::string(argv[i]) == "--device" && i + 1 < argc) want = argv[++i];
    const Gpu g = OpenGpu(want);
    std::printf("device: %s (compute, no ray extension)\n", g.name.c_str());
    {
        Solver sv(g);
        { // the doorway: the device's images against the CPU's
            const RtLampProxy::Lot lot = DoorRooms();
            const Field f = PlanField::BuildField(lot.stories[0]);
            const Emit e = LampEmit(f, 1, 3.5f, 4.0f, 1.0f, 6.0f, 1.0f, 7.0f);
            for (const bool fix : {true, false}) {
                Params p;
                p.fix = fix;
                Layout l;
                std::vector<std::vector<float>> cpu;
                const std::vector<float> cpuOut = PlanCascades::Solve(f, e, p, &l, &cpu);
                const GpuOut r = Solve(sv, f, e, p, l, true);
                const std::string name = fix ? "fix" : "plain";
                std::printf("\n[doorway, %s] %d x %d a level\n", name.c_str(), l.tw, l.th);
                for (int i = p.levels - 1; i >= 0; i--) {
                    const float peak = *std::max_element(cpu[i].begin(), cpu[i].end());
                    const Diff d = Compare(r.lv[i], cpu[i], 0.001f * peak);
                    std::printf("  level %d: mean |device - CPU| %.5f %% of the peak %.4f, largest %.3f %%, %.3f %% of %ld values within 0.1 %%\n", i, 100 * d.sum / d.n / peak, peak, 100 * d.worst / peak,
                                100.0 * d.within / d.n, d.n);
                    Check(d.within >= 0.999 * d.n && d.sum / d.n < 0.0002 * peak, name + ": level " + std::to_string(i) + " is off the CPU's");
                }
                const float peak = *std::max_element(cpuOut.begin(), cpuOut.end());
                const Diff d = Compare(r.out, cpuOut, 0.001f * peak);
                std::printf("  gather: mean |device - CPU| %.5f %% of the peak %.4f, largest %.3f %%, %.3f %% within 0.1 %%\n", 100 * d.sum / d.n / peak, peak, 100 * d.worst / peak, 100.0 * d.within / d.n);
                Check(d.n == static_cast<long>(cpuOut.size()) && d.within >= 0.999 * d.n, name + ": the gather is off the CPU's");
            }
        }
        { // the cost on a large story: 64 x 64 tiles of 8 x 8 tile rooms with doors, by level count
            RtLampProxy::Lot lot;
            lot.stories.push_back(PlanFieldTest::MakeStory(0, 64, 64, 0.0f));
            RtLampProxy::Story& s = lot.stories[0];
            for (int k = 0; k <= 64; k += 8) PlanFieldTest::Line(s, k, 0, k, 64), PlanFieldTest::Line(s, 0, k, 64, k);
            for (RtLampProxy::Edge& e : s.edges)
                if ((static_cast<int>(e.x0 + e.z0)) % 8 == 3) e.open = true, e.o.u0 = 0.1f, e.o.u1 = 0.9f, e.o.sill = 0.0f;
            const Field f = PlanField::BuildField(s);
            const Emit e = LampEmit(f, 1, 30.0f, 30.0f, 0.0f, 64.0f, 0.0f, 64.0f, 50.0f);
            std::printf("\n[64 x 64 tiles, 8 x 8 tile rooms with doors] field %d x %d; ms a level, last level first (this machine, other work running: order of magnitude)\n", f.w, f.h);
            for (const float spacing : {0.25f, 0.5f})
                for (const bool fix : {true, false})
                    for (const int levels : {1, 2, 3, 4}) {
                        Params p;
                        p.fix = fix, p.levels = levels, p.spacing = spacing, p.l0 = 4.0f * spacing;
                        const Layout l = LayoutOf(f, p);
                        double best = 1e9;
                        std::string per;
                        for (int rep = 0; rep < 3; rep++) {
                            const GpuOut r = Solve(sv, f, e, p, l, false);
                            double sum = 0;
                            std::string text;
                            for (int i = levels - 1; i >= 0; i--) sum += r.ms[i], text += (text.empty() ? "" : " + ") + std::to_string(r.ms[i]).substr(0, 5);
                            if (sum < best) best = sum, per = text;
                        }
                        std::printf("  %.2f m probes, l0 %.1f m, %s, %d level%s (to %3.0f m): %4d x %4d a level, %2d traces a texel: %7.2f ms (%s)\n", spacing, p.l0, fix ? "fix  " : "plain", levels,
                                    levels > 1 ? "s" : " ", l.lv.back().t1, l.tw, l.th, fix ? 16 : 4, best, per.c_str());
                    }
        }
    }
    std::printf("\n%d checks, %d failed\n", checks, failures);
    return failures != 0;
}
