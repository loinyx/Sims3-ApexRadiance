// The per-pixel shadow pass of the bridge server (real-time GI work package XR-06, research 16 section 3.3): the kernel
// tools/gi_server_test/pixel_shadow.comp (BRIDGE_PASS) on DXVK's own Vulkan device, recorded into DXVK's command stream
// through ID3D9VkExternalPass (native/dxvk patch 0004). Nothing is submitted or waited for here: the pass runs in
// stream order, so a draw the client issues after it samples this frame's outputs.
//   init()    looks for the interface and ray queries; a reason comes back when the pass stays off
//   run()     one pass: block, depth, optional motion class, up to four outputs (gi_pixel_block.h)
//   forget()  a texture is about to be destroyed
//   drop()    before a device reset or destroy: waits for the device, frees every Vulkan object
//   view()    the server's view of a client texture (WP11): blits it into a client render target texture
//   addModel(), setMeshes()  mesh casters (MC-2): a model's triangles once, then the placed objects; each model becomes
//             one bottom level on the renderer's device when a pass first needs it, its objects instances of it
// Header only: the bridge server and native/dxvk/test/dxvk_gi_pixel_pass.cpp compile the same file. The including file
// provides kGiPixelSpv (the compiled kernel) before this header.
// All calls come from one thread (the server's play thread); the record callback runs on DXVK's command thread and
// reads only the copy it is given and objects that live until drop().
#pragma once
#include <windows.h>
#include <d3d9.h>
#ifndef VK_NO_PROTOTYPES
#define VK_NO_PROTOTYPES
#endif
#include <vulkan/vulkan.h>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <algorithm>
#include <initializer_list>
#include <vector>
#include "gi_pixel_block.h"

// DXVK's interop interfaces, as declared in src/d3d9/d3d9_interfaces.h (method order matters)
struct GiD3D9VkExtImageDesc;
MIDL_INTERFACE("d56344f5-8d35-46fd-806d-94c351b472c1")
IGiD3D9VkInteropTexture : public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE GetVulkanImageInfo(VkImage*, VkImageLayout*, VkImageCreateInfo*) = 0;
};
MIDL_INTERFACE("2eaa4b89-0107-4bdb-87f7-0f541c493ce0")
IGiD3D9VkInteropDevice : public IUnknown {
  virtual void STDMETHODCALLTYPE GetVulkanHandles(VkInstance*, VkPhysicalDevice*, VkDevice*) = 0;
  virtual void STDMETHODCALLTYPE GetSubmissionQueue(VkQueue*, uint32_t*, uint32_t*) = 0;
  virtual void STDMETHODCALLTYPE TransitionTextureLayout(IGiD3D9VkInteropTexture*, const VkImageSubresourceRange*, VkImageLayout, VkImageLayout) = 0;
  virtual void STDMETHODCALLTYPE FlushRenderingCommands() = 0;
  virtual void STDMETHODCALLTYPE LockSubmissionQueue() = 0;
  virtual void STDMETHODCALLTYPE ReleaseSubmissionQueue() = 0;
  virtual void STDMETHODCALLTYPE LockDevice() = 0;
  virtual void STDMETHODCALLTYPE UnlockDevice() = 0;
  virtual bool STDMETHODCALLTYPE WaitForResource(IDirect3DResource9*, DWORD) = 0;
  virtual HRESULT STDMETHODCALLTYPE CreateImage(const GiD3D9VkExtImageDesc*, IDirect3DResource9**) = 0;
};
typedef void (STDMETHODCALLTYPE *PFN_GiD3D9VkExternalPass)(VkCommandBuffer, const void*);
MIDL_INTERFACE("7daa16ca-164c-44ea-b567-7f73c71ddaf8")
IGiD3D9VkExternalPass : public IUnknown {
  virtual BOOL STDMETHODCALLTYPE SupportsRayQuery() = 0;
  virtual HRESULT STDMETHODCALLTYPE ExecuteExternal(PFN_GiD3D9VkExternalPass, const void*, UINT, UINT, IDirect3DResource9* const*, const VkImageLayout*) = 0;
};

#define GI_PIXEL_VK_FUNCS(X) \
  X(CreateBuffer) X(DestroyBuffer) X(GetBufferMemoryRequirements) X(AllocateMemory) X(FreeMemory) X(BindBufferMemory) \
  X(MapMemory) X(GetBufferDeviceAddress) X(CreateImage) X(DestroyImage) X(GetImageMemoryRequirements) X(BindImageMemory) \
  X(CreateImageView) X(DestroyImageView) X(CreateSampler) X(DestroySampler) X(CreateDescriptorSetLayout) \
  X(DestroyDescriptorSetLayout) X(CreateDescriptorPool) X(DestroyDescriptorPool) X(AllocateDescriptorSets) \
  X(UpdateDescriptorSets) X(CreatePipelineLayout) X(DestroyPipelineLayout) X(CreateShaderModule) X(DestroyShaderModule) \
  X(CreateComputePipelines) X(DestroyPipeline) X(CreateQueryPool) X(DestroyQueryPool) X(GetQueryPoolResults) \
  X(DeviceWaitIdle) X(CreateAccelerationStructureKHR) X(DestroyAccelerationStructureKHR) \
  X(GetAccelerationStructureBuildSizesKHR) X(GetAccelerationStructureDeviceAddressKHR) \
  X(CmdBuildAccelerationStructuresKHR) X(CmdUpdateBuffer) X(CmdPipelineBarrier) X(CmdBindPipeline) \
  X(CmdBindDescriptorSets) X(CmdDispatch) X(CmdCopyImage) X(CmdBlitImage) X(CmdResetQueryPool) X(CmdWriteTimestamp)

class GiPixelPass {
public:
  ~GiPixelPass() { close(); }
  void close() { drop(); release(); }

  // nullptr: the pass can run. Otherwise why not (the feature stays off; logged once by the caller)
  const char* init(IDirect3DDevice9* dev) {
    release();
    if (FAILED(dev->QueryInterface(__uuidof(IGiD3D9VkExternalPass), reinterpret_cast<void**>(&m_pass)))) return "stock renderer (no external pass interface)";
    if (!m_pass->SupportsRayQuery()) return fail("no ray queries on this device");
    if (FAILED(dev->QueryInterface(__uuidof(IGiD3D9VkInteropDevice), reinterpret_cast<void**>(&m_interop)))) return fail("no interop interface");
    VkInstance inst;
    m_interop->GetVulkanHandles(&inst, &m_phys, &m_dev);
    HMODULE vk = LoadLibraryA("vulkan-1.dll");
    auto gipa = vk ? reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(vk, "vkGetInstanceProcAddr")) : nullptr;
    auto gdpa = gipa ? reinterpret_cast<PFN_vkGetDeviceProcAddr>(gipa(inst, "vkGetDeviceProcAddr")) : nullptr;
    if (!gdpa) return fail("no Vulkan loader");
#define X(n) if (!(m_vk.n = reinterpret_cast<PFN_vk##n>(gdpa(m_dev, "vk" #n)))) return fail("missing vk" #n);
    GI_PIXEL_VK_FUNCS(X)
#undef X
    auto memProps = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties>(gipa(inst, "vkGetPhysicalDeviceMemoryProperties"));
    auto props = reinterpret_cast<PFN_vkGetPhysicalDeviceProperties>(gipa(inst, "vkGetPhysicalDeviceProperties"));
    if (!memProps || !props) return fail("no Vulkan loader");
    memProps(m_phys, &m_mem);
    VkPhysicalDeviceProperties p;
    props(m_phys, &p);
    m_nsPerTick = p.limits.timestampPeriod;
    return nullptr;
  }
  bool ready() const { return m_interop != nullptr; }

  // A model's triangles in model space (xyz a vertex, abc a triangle), kept here; built when an instance names it, and
  // again after drop(). Its index for GiPixelMesh::model, or kNoModel for an empty, too large or broken mesh
  static constexpr uint32_t kNoModel = ~0u;
  uint32_t addModel(const float* pos, uint32_t vertices, const uint32_t* tri, uint32_t triangles) {
    if (!pos || !tri || !vertices || !triangles || triangles > kMaxModelTriangles || vertices > 3 * kMaxModelTriangles || m_models.size() >= kMaxModels) return kNoModel;
    for (uint32_t i = 0; i < triangles * 3; i++)
      if (tri[i] >= vertices) return kNoModel;
    Model m;
    m.vertices = vertices, m.triangles = triangles;
    for (uint32_t v = 0; v < vertices; v++)
      for (int a = 0; a < 3; a++) m.lo[a] = std::min(m.lo[a], pos[v * 3 + a]), m.hi[a] = std::max(m.hi[a], pos[v * 3 + a]);
    m.bytes.resize(size_t(vertices) * 12 + size_t(triangles) * 12);
    std::memcpy(m.bytes.data(), pos, size_t(vertices) * 12);
    std::memcpy(m.bytes.data() + size_t(vertices) * 12, tri, size_t(triangles) * 12);
    m_models.push_back(std::move(m));
    return static_cast<uint32_t>(m_models.size() - 1);
  }
  // The placed objects of the next passes (copied). A pass takes the kGiPixelMeshes nearest its camera; an object whose
  // model is not built yet waits for it (kBuildsPerPass models a pass)
  void setMeshes(const GiPixelMesh* list, uint32_t count) { m_meshes.assign(list, list + count); }

  // out[i] may be null. E_FAIL counts as a fault; the text of the last one is lastFault()
  HRESULT run(const GiPixelBlock& block, IDirect3DBaseTexture9* depth, IDirect3DBaseTexture9* cls, IDirect3DBaseTexture9* const* out) {
    if (!ready() || !depth) return fault("not ready");
    if (block.lamps > kGiPixelLamps || block.casters > kGiPixelCasters || block.capsules > kGiPixelCapsules || block.boxes > kGiPixelBoxes
        || block.samples < 1 || block.samples > 16) return fault("block counts out of range");
    VkImageCreateInfo depthInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}, clsInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    const VkImage depthImage = imageOf(depth, &depthInfo), clsImage = cls ? imageOf(cls, &clsInfo) : VK_NULL_HANDLE;
    if (!depthImage || (cls && !clsImage)) return fault("input is not a renderer texture");
    if (!(depthInfo.usage & VK_IMAGE_USAGE_SAMPLED_BIT) || !isDepth(depthInfo.format)) return fault("depth is not a sampled depth texture");
    const uint32_t w = depthInfo.extent.width / 4, h = depthInfo.extent.height / 4;
    if (!w || !h) return fault("depth too small");

    if (depthImage != m_depthImage || clsImage != m_clsImage || w != m_w || h != m_h) {
      drop();  // (waits for the device: only when the inputs change, never in a steady frame)
      if (const char* why = create(depthImage, depthInfo.format, clsImage, clsInfo.format, w, h)) {
        drop();
        return fault(why);
      }
      m_depthTex = depth, m_clsTex = cls;
    }

    Record* r = &m_record;
    r->self = this, r->first = m_fresh, r->slot = m_passes % kSlots;
    IDirect3DResource9* res[2 + kGiPixelOutputs] = {depth};
    VkImageLayout layouts[2 + kGiPixelOutputs] = {VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL};
    UINT count = 1;
    if (cls) res[count] = cls, layouts[count++] = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    for (uint32_t i = 0; i < kGiPixelOutputs; i++) {
      r->out[i] = VK_NULL_HANDLE;
      if (!out || !out[i]) continue;
      VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
      const VkImage image = imageOf(out[i], &info);
      if (!image || info.format != VK_FORMAT_R16G16B16A16_SFLOAT || info.extent.width != w || info.extent.height != h)
        return fault("output is not an A16B16G16R16F texture of a quarter of the depth's size");
      r->out[i] = image, res[count] = out[i], layouts[count++] = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    }
    std::memcpy(r->block, &block, kBlockHead);
    GiPixelBlock* b = reinterpret_cast<GiPixelBlock*>(r->block);  // (only the fields before `box` exist in the copy)
    b->width = depthInfo.extent.width, b->height = depthInfo.extent.height;
    b->flags = cls ? kGiPixelFlagClass : 0;
    // Instances: capsule i is instance i (the kernel reads the capsule by the instance's index), then the boxes, then the meshes
    r->instances = block.capsules + block.boxes;
    std::memset(r->inst, 0, sizeof(VkAccelerationStructureInstanceKHR) * r->instances);
    for (uint32_t i = 0; i < block.capsules; i++) {
      const GiPixelCapsule& c = block.capsule[i];
      VkAccelerationStructureInstanceKHR& n = r->inst[i];
      for (int a = 0; a < 3; a++) {
        const float lo = (c.a[a] < c.b[a] ? c.a[a] : c.b[a]) - c.r, hi = (c.a[a] > c.b[a] ? c.a[a] : c.b[a]) + c.r;
        n.transform.matrix[a][a] = hi - lo, n.transform.matrix[a][3] = lo;
      }
      n.instanceCustomIndex = 3, n.mask = 2, n.accelerationStructureReference = m_blasAddr[1];
    }
    for (uint32_t i = 0; i < block.boxes; i++) {
      VkAccelerationStructureInstanceKHR& n = r->inst[block.capsules + i];
      std::memcpy(n.transform.matrix, block.box[i].m, sizeof n.transform.matrix);
      // shortcut: a mirrored transform (negative determinant) turns the cube inside out for the rays' back-face culling,
      // upgrade when the client sends mirrored boxes
      n.instanceCustomIndex = 1, n.mask = 1, n.flags = kBoxFacing, n.accelerationStructureReference = m_blasAddr[0];
    }
    if (const char* why = meshInstances(block, *r)) return fault(why);
    b->meshBounds = m_boundsLast;  // (meshInstances wrote the bounds after the block's casters)

    collect();
    const UINT size = static_cast<UINT>(offsetof(Record, inst) + sizeof(VkAccelerationStructureInstanceKHR) * r->instances);
    if (FAILED(m_pass->ExecuteExternal(&GiPixelPass::record, r, size, count, res, layouts))) return fault("the renderer refused the pass");
    m_fresh = false;
    m_passes++;
    return S_OK;
  }

  // WP11: the server's view of a client texture (a G-buffer target such as albedo + id), blitted on the renderer's device
  // in stream order into a client render target texture of any size and colour format the blit supports (nearest at the
  // same size, so an id byte stays exact; linear otherwise). Proves the renderer's image of a D3D9 render target reaches
  // the server; a consumer binds the same image as XR-06 binds the class texture. Needs no pass objects.
  HRESULT view(IDirect3DBaseTexture9* src, IDirect3DBaseTexture9* dst) {
    if (!ready() || !src || !dst) return viewFault("not ready or no texture");
    VkImageCreateInfo si{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO}, di{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ViewRecord r{m_vk.CmdBlitImage, imageOf(src, &si), imageOf(dst, &di)};
    if (!r.src || !r.dst) return viewFault("input is not a renderer texture");
    if (isDepth(si.format) || isDepth(di.format) || !(si.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) || !(di.usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT))
      return viewFault("view needs two colour textures the renderer can copy");
    r.sw = si.extent.width, r.sh = si.extent.height, r.dw = di.extent.width, r.dh = di.extent.height;
    IDirect3DResource9* res[2] = {src, dst};
    const VkImageLayout layouts[2] = {VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL};
    if (FAILED(m_pass->ExecuteExternal(&GiPixelPass::recordView, &r, sizeof r, 2, res, layouts))) return viewFault("the renderer refused the view");
    m_views++;
    return S_OK;
  }
  uint32_t views() const { return m_views; }
  uint32_t viewFaults() const { return m_viewFaults; }

  void forget(const IUnknown* texture) {
    if (texture && (texture == m_depthTex || texture == m_clsTex)) drop();
  }

  // Frees every Vulkan object after the device has finished what was recorded. The interfaces stay: run() builds again
  void drop() {
    if (!m_dev || !m_created) return;
    m_interop->FlushRenderingCommands();
    m_interop->LockSubmissionQueue();
    m_vk.DeviceWaitIdle(m_dev);
    m_interop->ReleaseSubmissionQueue();
    collect();
    for (VkImageView v : {m_depthView, m_clsView, m_outView[0], m_outView[1], m_outView[2], m_outView[3]})
      if (v) m_vk.DestroyImageView(m_dev, v, nullptr);
    for (VkImage i : m_outImage) if (i) m_vk.DestroyImage(m_dev, i, nullptr);
    for (VkAccelerationStructureKHR a : m_as) if (a) m_vk.DestroyAccelerationStructureKHR(m_dev, a, nullptr);
    for (VkBuffer b : {m_blockBuf, m_instBuf, m_geoBuf, m_statsBuf, m_asBuf, m_scratchBuf}) if (b) m_vk.DestroyBuffer(m_dev, b, nullptr);
    for (Model& m : m_models) {
      if (m.as) m_vk.DestroyAccelerationStructureKHR(m_dev, m.as, nullptr);
      m.as = VK_NULL_HANDLE, m.blas = 0;
    }
    for (const Chunk& c : m_chunks) m_vk.DestroyBuffer(m_dev, c.buf, nullptr);
    for (VkBuffer b : m_retired) m_vk.DestroyBuffer(m_dev, b, nullptr);
    if (m_scratch) m_vk.DestroyBuffer(m_dev, m_scratch, nullptr);
    m_chunks.clear(), m_retired.clear(), m_scratch = VK_NULL_HANDLE, m_scratchSize = 0, m_scratchAddrMesh = 0, m_built = 0;
    for (VkDeviceMemory mem : m_memory) m_vk.FreeMemory(m_dev, mem, nullptr);  // (unmaps the counters and the mesh data)
    if (m_pipeline) m_vk.DestroyPipeline(m_dev, m_pipeline, nullptr);
    if (m_layout) m_vk.DestroyPipelineLayout(m_dev, m_layout, nullptr);
    if (m_pool) m_vk.DestroyDescriptorPool(m_dev, m_pool, nullptr);
    if (m_setLayout) m_vk.DestroyDescriptorSetLayout(m_dev, m_setLayout, nullptr);
    if (m_sampler) m_vk.DestroySampler(m_dev, m_sampler, nullptr);
    if (m_queries) m_vk.DestroyQueryPool(m_dev, m_queries, nullptr);
    m_depthView = m_clsView = VK_NULL_HANDLE, m_depthImage = m_clsImage = VK_NULL_HANDLE;
    m_depthTex = m_clsTex = nullptr;
    for (uint32_t i = 0; i < kGiPixelOutputs; i++) m_outView[i] = VK_NULL_HANDLE, m_outImage[i] = VK_NULL_HANDLE;
    for (VkAccelerationStructureKHR& a : m_as) a = VK_NULL_HANDLE;
    m_blockBuf = m_instBuf = m_geoBuf = m_statsBuf = m_asBuf = m_scratchBuf = VK_NULL_HANDLE;
    m_memory.clear(), m_stats = nullptr, m_statsSeen = 0;
    m_pipeline = VK_NULL_HANDLE, m_layout = VK_NULL_HANDLE, m_pool = VK_NULL_HANDLE, m_setLayout = VK_NULL_HANDLE;
    m_sampler = VK_NULL_HANDLE, m_queries = VK_NULL_HANDLE, m_set = VK_NULL_HANDLE;
    m_w = m_h = 0, m_created = false;
  }

  // Reads what the device has answered so far and returns the counters
  GiPixelCounters counters() {
    collect();
    return {m_passes, m_faults, m_rays, m_raysLast, m_gpuUs, m_w, m_h, m_meshesLast, m_built};
  }
  const char* lastFault() const { return m_lastFault; }
  // The counter line of the log, without a line end
  int line(char* text, size_t size) {
    const GiPixelCounters c = counters();
    return std::snprintf(text, size, "[GiPixel] passes=%u rays=%llu raysLast=%u gpuUs=%u size=%ux%u meshes=%u models=%u bounds=%u faults=%u%s%s", c.passes,
                         static_cast<unsigned long long>(c.rays), c.raysLast, c.gpuUs, c.width, c.height, c.meshes, c.models, m_boundsLast, c.faults,
                         m_faults ? " last=" : "", m_faults ? m_lastFault : "");
  }

private:
  enum : uint32_t { kSlots = 8, kMaxInstances = kGiPixelCapsules + kGiPixelBoxes + kGiPixelMeshes, kBlockHead = offsetof(GiPixelBlock, box),
                     kShaderBlock = kBlockHead + kGiPixelMeshBounds * sizeof(GiPixelCaster), kCubeBytes = 36 * 12 };
  // shortcut: a model over 64 k triangles is left out (the survey's 99 % is 5 k) and at most 4096 models are kept; raise when the counters show refusals
  enum : uint32_t { kMaxModelTriangles = 65536, kMaxModels = 4096, kBuildsPerPass = 16, kChunkBytes = 8u << 20 };
  // Game meshes are open and wound either way: no culling, whatever the rays ask
  static constexpr VkGeometryInstanceFlagsKHR kMeshFacing = VK_GEOMETRY_INSTANCE_TRIANGLE_FACING_CULL_DISABLE_BIT_KHR;
  struct Model {
    std::vector<uint8_t> bytes;  // vertices, then indices
    uint32_t vertices = 0, triangles = 0;
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};  // model space
    VkAccelerationStructureKHR as = VK_NULL_HANDLE;  // created by the pass that builds it; freed by drop()
    VkDeviceAddress blas = 0;
  };
  struct Build {  // what the record callback needs of a model: the play thread may grow the model list meanwhile
    VkAccelerationStructureKHR as;
    VkDeviceAddress data;
    uint32_t vertices, triangles;
  };
  struct Chunk {  // a buffer the mesh data (host-visible) or the bottom levels (device-local) are cut from
    VkBuffer buf;
    VkDeviceAddress addr;
    uint8_t* map;
    VkDeviceSize used, size;
    bool host;
  };
  // The unit cube's faces wind counter-clockwise seen from outside; with this flag the rays' back-face culling drops the
  // faces a ray leaves through, so a ray that starts just inside a box leaves it (verified by dxvk_gi_pixel_pass)
  static constexpr VkGeometryInstanceFlagsKHR kBoxFacing = VK_GEOMETRY_INSTANCE_TRIANGLE_FRONT_COUNTERCLOCKWISE_BIT_KHR;
  struct Record {  // copied by ExecuteExternal; `inst` is cut after the last instance
    GiPixelPass* self;
    VkImage out[kGiPixelOutputs];
    uint32_t first, slot, instances, builds;
    VkDeviceAddress scratch;  // of the builds
    Build build[kBuildsPerPass];
    uint8_t block[kShaderBlock];
    VkAccelerationStructureInstanceKHR inst[kMaxInstances];
  };
  struct Funcs {
#define X(n) PFN_vk##n n;
    GI_PIXEL_VK_FUNCS(X)
#undef X
  };

  const char* fail(const char* why) { release(); return why; }
  HRESULT fault(const char* why) { m_faults++, m_lastFault = why; return E_FAIL; }
  void release() {
    if (m_interop) m_interop->Release();
    if (m_pass) m_pass->Release();
    m_interop = nullptr, m_pass = nullptr, m_dev = VK_NULL_HANDLE;
  }
  static bool isDepth(VkFormat f) {
    return f == VK_FORMAT_D24_UNORM_S8_UINT || f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_D32_SFLOAT || f == VK_FORMAT_D16_UNORM || f == VK_FORMAT_X8_D24_UNORM_PACK32;
  }
  static VkImage imageOf(IUnknown* texture, VkImageCreateInfo* info) {
    IGiD3D9VkInteropTexture* t = nullptr;
    if (FAILED(texture->QueryInterface(__uuidof(IGiD3D9VkInteropTexture), reinterpret_cast<void**>(&t)))) return VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    const HRESULT hr = t->GetVulkanImageInfo(&image, nullptr, info);
    t->Release();
    return SUCCEEDED(hr) ? image : VK_NULL_HANDLE;
  }

  bool buffer(VkBuffer* b, VkDeviceSize size, VkBufferUsageFlags usage, void** mapped = nullptr) {
    VkBufferCreateInfo ci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    ci.size = size, ci.usage = usage;
    if (m_vk.CreateBuffer(m_dev, &ci, nullptr, b) != VK_SUCCESS) return false;
    VkMemoryRequirements req;
    m_vk.GetBufferMemoryRequirements(m_dev, *b, &req);
    const VkMemoryPropertyFlags want = mapped ? VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT : VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT;
    VkDeviceMemory mem;
    if (!allocate(req, want, (usage & VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT) != 0, &mem)) return false;
    if (m_vk.BindBufferMemory(m_dev, *b, mem, 0) != VK_SUCCESS) return false;
    return !mapped || m_vk.MapMemory(m_dev, mem, 0, VK_WHOLE_SIZE, 0, mapped) == VK_SUCCESS;
  }
  bool allocate(const VkMemoryRequirements& req, VkMemoryPropertyFlags want, bool address, VkDeviceMemory* mem) {
    VkMemoryAllocateFlagsInfo flags{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_FLAGS_INFO};
    flags.flags = VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT;
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.pNext = address ? &flags : nullptr, ai.allocationSize = req.size;
    for (uint32_t i = 0; i < m_mem.memoryTypeCount; i++)
      if ((req.memoryTypeBits >> i & 1) && (m_mem.memoryTypes[i].propertyFlags & want) == want) {
        ai.memoryTypeIndex = i;
        if (m_vk.AllocateMemory(m_dev, &ai, nullptr, mem) != VK_SUCCESS) return false;
        m_memory.push_back(*mem);
        return true;
      }
    return false;
  }
  VkDeviceAddress address(VkBuffer b) {
    VkBufferDeviceAddressInfo ai{VK_STRUCTURE_TYPE_BUFFER_DEVICE_ADDRESS_INFO};
    ai.buffer = b;
    return m_vk.GetBufferDeviceAddress(m_dev, &ai);
  }
  bool view(VkImageView* v, VkImage image, VkFormat format, VkImageAspectFlags aspect) {
    VkImageViewCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    ci.image = image, ci.viewType = VK_IMAGE_VIEW_TYPE_2D, ci.format = format, ci.subresourceRange = {aspect, 0, 1, 0, 1};
    return m_vk.CreateImageView(m_dev, &ci, nullptr, v) == VK_SUCCESS;
  }

  // A range of a chunk: 256-byte aligned (bottom levels need it); a new chunk when none has room
  bool cut(bool host, VkDeviceSize size, Chunk** chunk, VkDeviceSize* offset) {
    size = (size + 255) & ~VkDeviceSize(255);
    for (Chunk& c : m_chunks)
      if (c.host == host && c.size - c.used >= size) return *chunk = &c, *offset = c.used, c.used += size, true;
    Chunk c{VK_NULL_HANDLE, 0, nullptr, 0, std::max<VkDeviceSize>(size, kChunkBytes), host};
    const VkBufferUsageFlags addr = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    void* map = nullptr;
    if (!buffer(&c.buf, c.size, host ? VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | addr : VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | addr,
                host ? &map : nullptr)) {
      if (c.buf) m_retired.push_back(c.buf);
      return false;
    }
    c.addr = address(c.buf), c.map = static_cast<uint8_t*>(map), c.used = size;
    m_chunks.push_back(c);
    return *chunk = &m_chunks.back(), *offset = 0, true;
  }

  // Makes a model's bottom level and its input data ready for a build in this pass; the build itself is recorded
  const char* prepare(Model& m, Build& b) {
    VkAccelerationStructureGeometryKHR geo{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    geo.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR, geo.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
    geo.geometry.triangles = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR};
    geo.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT, geo.geometry.triangles.vertexStride = 12;
    geo.geometry.triangles.maxVertex = m.vertices - 1, geo.geometry.triangles.indexType = VK_INDEX_TYPE_UINT32;
    VkAccelerationStructureBuildGeometryInfoKHR info{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    info.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, info.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    info.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR, info.geometryCount = 1, info.pGeometries = &geo;
    VkAccelerationStructureBuildSizesInfoKHR sizes{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
    m_vk.GetAccelerationStructureBuildSizesKHR(m_dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, &m.triangles, &sizes);
    if (sizes.buildScratchSize > m_scratchSize) {  // the old scratch may still be in use by the device: kept until drop()
      if (m_scratch) m_retired.push_back(m_scratch);
      m_scratch = VK_NULL_HANDLE, m_scratchSize = 0;
      if (!buffer(&m_scratch, sizes.buildScratchSize + 256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT)) return "mesh scratch buffer failed";
      m_scratchSize = sizes.buildScratchSize, m_scratchAddrMesh = (address(m_scratch) + 255) & ~VkDeviceAddress(255);
    }
    Chunk *data, *store;
    VkDeviceSize dataAt, storeAt;
    if (!cut(true, m.bytes.size(), &data, &dataAt)) return "mesh data buffer failed";
    std::memcpy(data->map + dataAt, m.bytes.data(), m.bytes.size());
    const VkDeviceAddress dataAddr = data->addr + dataAt;  // (before the next cut: it may grow the chunk list)
    if (!cut(false, sizes.accelerationStructureSize, &store, &storeAt)) return "bottom level buffer failed";
    VkAccelerationStructureCreateInfoKHR ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
    ci.buffer = store->buf, ci.offset = storeAt, ci.size = sizes.accelerationStructureSize, ci.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    if (m_vk.CreateAccelerationStructureKHR(m_dev, &ci, nullptr, &m.as) != VK_SUCCESS) return m.as = VK_NULL_HANDLE, "bottom level creation failed";
    VkAccelerationStructureDeviceAddressInfoKHR ai{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
    ai.accelerationStructure = m.as;
    m.blas = m_vk.GetAccelerationStructureDeviceAddressKHR(m_dev, &ai);
    b = {m.as, dataAddr, m.vertices, m.triangles};
    m_built++;
    return nullptr;
  }

  // The mesh instances of a pass, after the capsules and boxes: the nearest objects to the camera whose model is built,
  // building up to kBuildsPerPass models it lacks
  const char* meshInstances(const GiPixelBlock& block, Record& r) {
    r.builds = 0, r.scratch = m_scratchAddrMesh, m_meshesLast = 0, m_boundsLast = 0;
    m_boxes.clear();
    if (m_meshes.empty()) return nullptr;
    m_order.resize(m_meshes.size());
    for (uint32_t i = 0; i < m_order.size(); i++) m_order[i] = i;
    const auto dist = [&](uint32_t i) {
      const float* m = m_meshes[i].m;
      const float x = m[3] - block.cam[0], y = m[7] - block.cam[1], z = m[11] - block.cam[2];
      return x * x + y * y + z * z;
    };
    if (m_order.size() > kGiPixelMeshes)
      std::nth_element(m_order.begin(), m_order.begin() + kGiPixelMeshes, m_order.end(), [&](uint32_t a, uint32_t b) { return dist(a) < dist(b); });
    const uint32_t take = std::min<uint32_t>(static_cast<uint32_t>(m_order.size()), kGiPixelMeshes);
    for (uint32_t k = 0; k < take; k++) {
      const GiPixelMesh& o = m_meshes[m_order[k]];
      if (o.model >= m_models.size()) continue;
      Model& m = m_models[o.model];
      if (!m.as) {
        if (r.builds == kBuildsPerPass) continue;
        if (const char* why = prepare(m, r.build[r.builds])) return why;
        r.builds++, r.scratch = m_scratchAddrMesh;
      }
      VkAccelerationStructureInstanceKHR& n = r.inst[r.instances++];
      n = {};
      std::memcpy(n.transform.matrix, o.m, sizeof n.transform.matrix);
      // A delta caster (the texel tier holds at most the object's box): its shadow shows where no static occluder already hides the lamp
      n.instanceCustomIndex = 2, n.mask = 2, n.flags = kMeshFacing, n.accelerationStructureReference = m.blas;
      m_meshesLast++;
      m_boxes.push_back(worldBox(m, o.m));
    }
    groupBounds(reinterpret_cast<GiPixelCaster*>(r.block + kBlockHead));
    return nullptr;
  }

  // An instance's world bounds: the model's box through the 3 x 4 matrix
  static GiPixelCaster worldBox(const Model& m, const float* x) {
    GiPixelCaster c{};
    for (int row = 0; row < 3; row++) {
      float lo = x[row * 4 + 3], hi = lo;
      for (int a = 0; a < 3; a++) {
        const float u = x[row * 4 + a] * m.lo[a], v = x[row * 4 + a] * m.hi[a];
        lo += std::min(u, v), hi += std::max(u, v);
      }
      c.lo[row] = lo, c.hi[row] = hi;
    }
    return c;
  }
  // The mesh instances' reach bounds for the kernel: the instances' boxes grouped by a grid cell of their centre, the cell
  // doubled until at most kGiPixelMeshBounds groups remain (a lot's furniture: a few cells of 2-4 m)
  void groupBounds(GiPixelCaster* out) {
    if (m_boxes.empty()) return;
    for (float cell = 2.0f;; cell *= 2.0f) {
      uint32_t n = 0;
      bool over = false;
      m_cellOf.clear();
      for (const GiPixelCaster& b : m_boxes) {
        const int64_t cx = static_cast<int64_t>(std::floor((b.lo[0] + b.hi[0]) * 0.5f / cell)), cz = static_cast<int64_t>(std::floor((b.lo[2] + b.hi[2]) * 0.5f / cell));
        const uint64_t key = static_cast<uint64_t>(cx) * 0x9E3779B97F4A7C15ull ^ static_cast<uint64_t>(cz);
        uint32_t at = n;
        for (uint32_t i = 0; i < n; i++)
          if (m_cellOf[i] == key) { at = i; break; }
        if (at == n) {
          if (n == kGiPixelMeshBounds) { over = true; break; }
          m_cellOf.push_back(key), out[n++] = b;
          continue;
        }
        for (int a = 0; a < 3; a++) out[at].lo[a] = std::min(out[at].lo[a], b.lo[a]), out[at].hi[a] = std::max(out[at].hi[a], b.hi[a]);
      }
      if (!over) { m_boundsLast = n; return; }
    }
  }

  // The build inputs of the three acceleration structures: 0 the unit cube, 1 the unit box of a capsule, 2 the top level
  void geometry(uint32_t i, VkAccelerationStructureGeometryKHR* geo, VkAccelerationStructureBuildGeometryInfoKHR* info) const {
    *geo = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    if (i == 0) {
      // Opaque: the traversal confirms a triangle itself (XR-09)
      geo->geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR, geo->flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
      geo->geometry.triangles = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR};
      geo->geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT, geo->geometry.triangles.vertexStride = 12;
      geo->geometry.triangles.vertexData.deviceAddress = m_geoAddr, geo->geometry.triangles.maxVertex = 35;
      geo->geometry.triangles.indexType = VK_INDEX_TYPE_NONE_KHR;
    } else if (i == 1) {
      geo->geometryType = VK_GEOMETRY_TYPE_AABBS_KHR;
      geo->geometry.aabbs = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_AABBS_DATA_KHR};
      geo->geometry.aabbs.data.deviceAddress = m_geoAddr + kCubeBytes, geo->geometry.aabbs.stride = 24;
    } else {
      geo->geometryType = VK_GEOMETRY_TYPE_INSTANCES_KHR;
      geo->geometry.instances = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_INSTANCES_DATA_KHR};
      geo->geometry.instances.data.deviceAddress = m_instAddr;
    }
    *info = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
    info->type = i == 2 ? VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR : VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
    // The top level is built again every pass: fast to build. The two bottom levels are built once
    info->flags = i == 2 ? VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_BUILD_BIT_KHR : VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
    info->mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR, info->geometryCount = 1, info->pGeometries = geo;
    info->dstAccelerationStructure = m_as[i], info->scratchData.deviceAddress = m_scratchAddr[i];
  }

  const char* create(VkImage depthImage, VkFormat depthFormat, VkImage clsImage, VkFormat clsFormat, uint32_t w, uint32_t h) {
    m_created = true;
    const VkBufferUsageFlags dst = VK_BUFFER_USAGE_TRANSFER_DST_BIT, addr = VK_BUFFER_USAGE_SHADER_DEVICE_ADDRESS_BIT;
    const VkBufferUsageFlags in = VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR | addr | dst;
    void* stats = nullptr;
    if (!buffer(&m_blockBuf, kShaderBlock, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | dst)
        || !buffer(&m_instBuf, sizeof(VkAccelerationStructureInstanceKHR) * kMaxInstances, in) || !buffer(&m_geoBuf, kCubeBytes + 24, in)
        || !buffer(&m_statsBuf, 16, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, &stats)) return "buffer creation failed";
    m_stats = static_cast<volatile uint32_t*>(stats);
    m_stats[0] = m_stats[1] = 0, m_statsSeen = 0;
    m_geoAddr = address(m_geoBuf), m_instAddr = address(m_instBuf);

    // Three acceleration structures in one buffer, each with its own scratch range
    VkAccelerationStructureBuildSizesInfoKHR sizes[3];
    VkDeviceSize asAt[3], scratchAt[3], asSize = 0, scratchSize = 0;
    for (uint32_t i = 0; i < 3; i++) {
      VkAccelerationStructureGeometryKHR geo;
      VkAccelerationStructureBuildGeometryInfoKHR info;
      geometry(i, &geo, &info);
      const uint32_t count = i == 0 ? 12 : i == 1 ? 1 : kMaxInstances;
      sizes[i] = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_SIZES_INFO_KHR};
      m_vk.GetAccelerationStructureBuildSizesKHR(m_dev, VK_ACCELERATION_STRUCTURE_BUILD_TYPE_DEVICE_KHR, &info, &count, &sizes[i]);
      asAt[i] = asSize, asSize += (sizes[i].accelerationStructureSize + 255) & ~VkDeviceSize(255);
      scratchAt[i] = scratchSize, scratchSize += (sizes[i].buildScratchSize + 255) & ~VkDeviceSize(255);
    }
    if (!buffer(&m_asBuf, asSize, VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_STORAGE_BIT_KHR | addr)
        || !buffer(&m_scratchBuf, scratchSize + 256, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | addr)) return "acceleration structure buffers failed";
    const VkDeviceAddress scratch = (address(m_scratchBuf) + 255) & ~VkDeviceAddress(255);
    for (uint32_t i = 0; i < 3; i++) {
      VkAccelerationStructureCreateInfoKHR ci{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_CREATE_INFO_KHR};
      ci.buffer = m_asBuf, ci.offset = asAt[i], ci.size = sizes[i].accelerationStructureSize;
      ci.type = i == 2 ? VK_ACCELERATION_STRUCTURE_TYPE_TOP_LEVEL_KHR : VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR;
      if (m_vk.CreateAccelerationStructureKHR(m_dev, &ci, nullptr, &m_as[i]) != VK_SUCCESS) return "acceleration structure creation failed";
      m_scratchAddr[i] = scratch + scratchAt[i];
      if (i < 2) {
        VkAccelerationStructureDeviceAddressInfoKHR ai{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_DEVICE_ADDRESS_INFO_KHR};
        ai.accelerationStructure = m_as[i];
        m_blasAddr[i] = m_vk.GetAccelerationStructureDeviceAddressKHR(m_dev, &ai);
      }
    }

    // Own output images: a Direct3D 9 render target has no storage usage, so the kernel writes these and they are copied
    for (uint32_t i = 0; i < kGiPixelOutputs; i++) {
      VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
      ci.imageType = VK_IMAGE_TYPE_2D, ci.format = VK_FORMAT_R16G16B16A16_SFLOAT, ci.extent = {w, h, 1}, ci.mipLevels = ci.arrayLayers = 1;
      ci.samples = VK_SAMPLE_COUNT_1_BIT, ci.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
      if (m_vk.CreateImage(m_dev, &ci, nullptr, &m_outImage[i]) != VK_SUCCESS) return "image creation failed";
      VkMemoryRequirements req;
      m_vk.GetImageMemoryRequirements(m_dev, m_outImage[i], &req);
      VkDeviceMemory mem;
      if (!allocate(req, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, false, &mem) || m_vk.BindImageMemory(m_dev, m_outImage[i], mem, 0) != VK_SUCCESS
          || !view(&m_outView[i], m_outImage[i], ci.format, VK_IMAGE_ASPECT_COLOR_BIT)) return "image creation failed";
    }
    if (!view(&m_depthView, depthImage, depthFormat, VK_IMAGE_ASPECT_DEPTH_BIT)
        || (clsImage && !view(&m_clsView, clsImage, clsFormat, VK_IMAGE_ASPECT_COLOR_BIT))) return "input view creation failed";
    VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};  // (the kernel fetches texels; the filter is unused)
    sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (m_vk.CreateSampler(m_dev, &sci, nullptr, &m_sampler) != VK_SUCCESS) return "sampler creation failed";

    const VkShaderStageFlags cs = VK_SHADER_STAGE_COMPUTE_BIT;
    const VkDescriptorType sampled = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, storage = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, sbuf = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    const VkDescriptorSetLayoutBinding binds[9] = {{0, VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1, cs}, {1, sampled, 1, cs}, {2, sbuf, 1, cs}, {3, sampled, 1, cs},
                                                   {4, storage, 1, cs}, {5, storage, 1, cs}, {6, storage, 1, cs}, {7, storage, 1, cs}, {8, sbuf, 1, cs}};
    VkDescriptorSetLayoutCreateInfo lci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    lci.bindingCount = 9, lci.pBindings = binds;
    const VkDescriptorPoolSize poolSizes[4] = {{VK_DESCRIPTOR_TYPE_ACCELERATION_STRUCTURE_KHR, 1}, {sampled, 2}, {sbuf, 2}, {storage, 4}};
    VkDescriptorPoolCreateInfo pci{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pci.maxSets = 1, pci.poolSizeCount = 4, pci.pPoolSizes = poolSizes;
    VkDescriptorSetAllocateInfo sai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    VkPipelineLayoutCreateInfo plci{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    VkShaderModuleCreateInfo mci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mci.codeSize = sizeof(kGiPixelSpv), mci.pCode = kGiPixelSpv;
    VkShaderModule module = VK_NULL_HANDLE;
    if (m_vk.CreateDescriptorSetLayout(m_dev, &lci, nullptr, &m_setLayout) != VK_SUCCESS || m_vk.CreateDescriptorPool(m_dev, &pci, nullptr, &m_pool) != VK_SUCCESS)
      return "descriptor creation failed";
    sai.descriptorPool = m_pool, sai.descriptorSetCount = 1, sai.pSetLayouts = &m_setLayout;
    plci.setLayoutCount = 1, plci.pSetLayouts = &m_setLayout;
    if (m_vk.AllocateDescriptorSets(m_dev, &sai, &m_set) != VK_SUCCESS || m_vk.CreatePipelineLayout(m_dev, &plci, nullptr, &m_layout) != VK_SUCCESS
        || m_vk.CreateShaderModule(m_dev, &mci, nullptr, &module) != VK_SUCCESS) return "pipeline layout creation failed";
    VkComputePipelineCreateInfo cpci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    cpci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, module, "main"};
    cpci.layout = m_layout;
    const VkResult made = m_vk.CreateComputePipelines(m_dev, VK_NULL_HANDLE, 1, &cpci, nullptr, &m_pipeline);
    m_vk.DestroyShaderModule(m_dev, module, nullptr);
    if (made != VK_SUCCESS) return "kernel pipeline creation failed";

    VkWriteDescriptorSetAccelerationStructureKHR was{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET_ACCELERATION_STRUCTURE_KHR};
    was.accelerationStructureCount = 1, was.pAccelerationStructures = &m_as[2];
    const VkImageLayout depthLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
    // Without a class texture binding 3 holds the depth again; the block's flag keeps the kernel from reading it
    const VkDescriptorImageInfo images[6] = {{m_sampler, m_depthView, depthLayout},
                                             clsImage ? VkDescriptorImageInfo{m_sampler, m_clsView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL} : VkDescriptorImageInfo{m_sampler, m_depthView, depthLayout},
                                             {VK_NULL_HANDLE, m_outView[0], VK_IMAGE_LAYOUT_GENERAL}, {VK_NULL_HANDLE, m_outView[1], VK_IMAGE_LAYOUT_GENERAL},
                                             {VK_NULL_HANDLE, m_outView[2], VK_IMAGE_LAYOUT_GENERAL}, {VK_NULL_HANDLE, m_outView[3], VK_IMAGE_LAYOUT_GENERAL}};
    const VkDescriptorBufferInfo buffers[2] = {{m_blockBuf, 0, VK_WHOLE_SIZE}, {m_statsBuf, 0, VK_WHOLE_SIZE}};
    VkWriteDescriptorSet writes[9];
    for (uint32_t i = 0; i < 9; i++) {
      writes[i] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
      writes[i].dstSet = m_set, writes[i].dstBinding = i, writes[i].descriptorCount = 1, writes[i].descriptorType = binds[i].descriptorType;
    }
    writes[0].pNext = &was;
    writes[1].pImageInfo = &images[0], writes[3].pImageInfo = &images[1];
    writes[2].pBufferInfo = &buffers[0], writes[8].pBufferInfo = &buffers[1];
    for (uint32_t i = 0; i < 4; i++) writes[4 + i].pImageInfo = &images[2 + i];
    m_vk.UpdateDescriptorSets(m_dev, 9, writes, 0, nullptr);

    VkQueryPoolCreateInfo qci{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    qci.queryType = VK_QUERY_TYPE_TIMESTAMP, qci.queryCount = kSlots * 2;
    if (m_vk.CreateQueryPool(m_dev, &qci, nullptr, &m_queries) != VK_SUCCESS) return "query pool creation failed";
    m_depthImage = depthImage, m_clsImage = clsImage, m_w = w, m_h = h, m_fresh = true, m_made = m_passes;
    return nullptr;
  }

  // Counters the device has written: rays (a running sum in a mapped buffer) and the timestamps of an older pass
  void collect() {
    if (!m_created || !m_stats) return;
    const uint32_t now = m_stats[0];
    if (now != m_statsSeen) m_raysLast = now - m_statsSeen, m_rays += m_raysLast, m_statsSeen = now;
    if (m_passes - m_made < 4) return;
    uint64_t t[2];
    if (m_vk.GetQueryPoolResults(m_dev, m_queries, ((m_passes - 4) % kSlots) * 2, 2, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS)
      m_gpuUs = static_cast<uint32_t>(static_cast<double>(t[1] - t[0]) * m_nsPerTick / 1000.0);
  }

  void barrier(VkCommandBuffer cmd, VkPipelineStageFlags from, VkAccessFlags wrote, VkPipelineStageFlags to, VkAccessFlags reads) const {
    VkMemoryBarrier b{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    b.srcAccessMask = wrote, b.dstAccessMask = reads;
    m_vk.CmdPipelineBarrier(cmd, from, to, 0, 1, &b, 0, nullptr, 0, nullptr);
  }
  struct ViewRecord {  // copied by ExecuteExternal
    PFN_vkCmdBlitImage blit;
    VkImage src, dst;
    uint32_t sw, sh, dw, dh;
  };
  HRESULT viewFault(const char* why) { m_viewFaults++, m_lastFault = why; return E_FAIL; }
  // On DXVK's command thread: the renderer has moved src to TRANSFER_SRC and dst to TRANSFER_DST
  static void STDMETHODCALLTYPE recordView(VkCommandBuffer cmd, const void* data) {
    const ViewRecord& r = *static_cast<const ViewRecord*>(data);
    VkImageBlit b{};
    b.srcSubresource = b.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    b.srcOffsets[1] = {int32_t(r.sw), int32_t(r.sh), 1};
    b.dstOffsets[1] = {int32_t(r.dw), int32_t(r.dh), 1};
    const bool same = r.sw == r.dw && r.sh == r.dh;
    r.blit(cmd, r.src, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, r.dst, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &b, same ? VK_FILTER_NEAREST : VK_FILTER_LINEAR);
  }
  // On DXVK's command thread, in stream order
  static void STDMETHODCALLTYPE record(VkCommandBuffer cmd, const void* data) {
    const Record& r = *static_cast<const Record*>(data);
    const GiPixelPass& p = *r.self;
    const Funcs& vk = p.m_vk;
    const VkPipelineStageFlags transfer = VK_PIPELINE_STAGE_TRANSFER_BIT, build = VK_PIPELINE_STAGE_ACCELERATION_STRUCTURE_BUILD_BIT_KHR, compute = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
    const VkAccessFlags asRead = VK_ACCESS_ACCELERATION_STRUCTURE_READ_BIT_KHR, asWrite = VK_ACCESS_ACCELERATION_STRUCTURE_WRITE_BIT_KHR;
    const VkImageSubresourceRange whole = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkAccelerationStructureGeometryKHR geo[3];
    VkAccelerationStructureBuildGeometryInfoKHR info[3];
    for (uint32_t i = 0; i < 3; i++) p.geometry(i, &geo[i], &info[i]);

    if (r.first) {  // the first pass after create(): the output images' layout and the two bottom levels
      VkImageMemoryBarrier ib[kGiPixelOutputs];
      for (uint32_t i = 0; i < kGiPixelOutputs; i++) {
        ib[i] = {VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
        ib[i].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT, ib[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, ib[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
        ib[i].srcQueueFamilyIndex = ib[i].dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, ib[i].image = p.m_outImage[i], ib[i].subresourceRange = whole;
      }
      vk.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, compute, 0, 0, nullptr, 0, nullptr, kGiPixelOutputs, ib);
      float g[(kCubeBytes + 24) / 4];
      uint32_t n = 0;
      for (int axis = 0; axis < 3; axis++)
        for (int side = 0; side < 2; side++) {
          static const int kQuad[6][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 0}, {1, 1}, {0, 1}};
          for (int k = 0; k < 6; k++) {
            const int* q = kQuad[side ? k : 5 - k];  // (reversed on the low side: outward faces wind one way)
            float v[3];
            v[axis] = static_cast<float>(side), v[(axis + 1) % 3] = static_cast<float>(q[0]), v[(axis + 2) % 3] = static_cast<float>(q[1]);
            g[n++] = v[0], g[n++] = v[1], g[n++] = v[2];
          }
        }
      g[n++] = 0, g[n++] = 0, g[n++] = 0, g[n++] = 1, g[n++] = 1, g[n++] = 1;  // the capsule's unit box
      vk.CmdUpdateBuffer(cmd, p.m_geoBuf, 0, sizeof g, g);
      p.barrier(cmd, transfer, VK_ACCESS_TRANSFER_WRITE_BIT, build, VK_ACCESS_SHADER_READ_BIT);
      const VkAccelerationStructureBuildRangeInfoKHR cube{12, 0, 0, 0}, box{1, 0, 0, 0};
      const VkAccelerationStructureBuildRangeInfoKHR* ranges[2] = {&cube, &box};
      vk.CmdBuildAccelerationStructuresKHR(cmd, 2, info, ranges);
      p.barrier(cmd, build, asWrite, build, asRead);
    }

    // Bottom levels of the models this pass needs first (their data is in mapped memory written before the pass was sent).
    // They share one scratch range, so each build waits for the one before
    for (uint32_t i = 0; i < r.builds; i++) {
      const Build& b = r.build[i];
      VkAccelerationStructureGeometryKHR mg{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
      mg.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR, mg.flags = VK_GEOMETRY_OPAQUE_BIT_KHR;
      mg.geometry.triangles = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR};
      mg.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT, mg.geometry.triangles.vertexStride = 12;
      mg.geometry.triangles.vertexData.deviceAddress = b.data, mg.geometry.triangles.maxVertex = b.vertices - 1;
      mg.geometry.triangles.indexType = VK_INDEX_TYPE_UINT32, mg.geometry.triangles.indexData.deviceAddress = b.data + VkDeviceSize(b.vertices) * 12;
      VkAccelerationStructureBuildGeometryInfoKHR mi{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_BUILD_GEOMETRY_INFO_KHR};
      mi.type = VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, mi.flags = VK_BUILD_ACCELERATION_STRUCTURE_PREFER_FAST_TRACE_BIT_KHR;
      mi.mode = VK_BUILD_ACCELERATION_STRUCTURE_MODE_BUILD_KHR, mi.geometryCount = 1, mi.pGeometries = &mg;
      mi.dstAccelerationStructure = b.as, mi.scratchData.deviceAddress = r.scratch;
      const VkAccelerationStructureBuildRangeInfoKHR range{b.triangles, 0, 0, 0};
      const VkAccelerationStructureBuildRangeInfoKHR* ranges = &range;
      vk.CmdBuildAccelerationStructuresKHR(cmd, 1, &mi, &ranges);
      p.barrier(cmd, build, asWrite, build, asRead | asWrite);
    }

    // The frame's block and instances travel inside the command buffer: no staging buffer a later pass could overwrite
    vk.CmdUpdateBuffer(cmd, p.m_blockBuf, 0, kShaderBlock, r.block);
    // (vkCmdUpdateBuffer takes 64 KB a call at most)
    for (VkDeviceSize at = 0, all = sizeof(VkAccelerationStructureInstanceKHR) * r.instances; at < all; at += 65536)
      vk.CmdUpdateBuffer(cmd, p.m_instBuf, at, std::min<VkDeviceSize>(65536, all - at), reinterpret_cast<const uint8_t*>(r.inst) + at);
    p.barrier(cmd, transfer, VK_ACCESS_TRANSFER_WRITE_BIT, build | compute, VK_ACCESS_SHADER_READ_BIT);
    const VkAccelerationStructureBuildRangeInfoKHR top{r.instances, 0, 0, 0};
    const VkAccelerationStructureBuildRangeInfoKHR* topRange = &top;
    vk.CmdBuildAccelerationStructuresKHR(cmd, 1, &info[2], &topRange);
    p.barrier(cmd, build, asWrite, compute, asRead);

    vk.CmdResetQueryPool(cmd, p.m_queries, r.slot * 2, 2);
    vk.CmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, p.m_queries, r.slot * 2);
    vk.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.m_pipeline);
    vk.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.m_layout, 0, 1, &p.m_set, 0, nullptr);
    vk.CmdDispatch(cmd, (p.m_w + 7) / 8, (p.m_h + 7) / 8, 1);
    vk.CmdWriteTimestamp(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, p.m_queries, r.slot * 2 + 1);

    p.barrier(cmd, compute, VK_ACCESS_SHADER_WRITE_BIT, transfer, VK_ACCESS_TRANSFER_READ_BIT);
    VkImageCopy region{};
    region.srcSubresource = region.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.extent = {p.m_w, p.m_h, 1};
    for (uint32_t i = 0; i < kGiPixelOutputs; i++)
      if (r.out[i]) vk.CmdCopyImage(cmd, p.m_outImage[i], VK_IMAGE_LAYOUT_GENERAL, r.out[i], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    // The next pass writes the buffers and the images again
    p.barrier(cmd, transfer | compute, VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT, transfer | compute,
              VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_WRITE_BIT);
  }

  IGiD3D9VkExternalPass* m_pass = nullptr;
  IGiD3D9VkInteropDevice* m_interop = nullptr;
  VkPhysicalDevice m_phys = VK_NULL_HANDLE;
  VkDevice m_dev = VK_NULL_HANDLE;
  Funcs m_vk{};
  VkPhysicalDeviceMemoryProperties m_mem{};
  float m_nsPerTick = 1.0f;

  bool m_created = false, m_fresh = false;
  const IUnknown* m_depthTex = nullptr;  // compared only, never used
  const IUnknown* m_clsTex = nullptr;
  VkImage m_depthImage = VK_NULL_HANDLE, m_clsImage = VK_NULL_HANDLE;
  VkImageView m_depthView = VK_NULL_HANDLE, m_clsView = VK_NULL_HANDLE;
  uint32_t m_w = 0, m_h = 0;
  VkImage m_outImage[kGiPixelOutputs] = {};
  VkImageView m_outView[kGiPixelOutputs] = {};
  VkBuffer m_blockBuf = VK_NULL_HANDLE, m_instBuf = VK_NULL_HANDLE, m_geoBuf = VK_NULL_HANDLE, m_statsBuf = VK_NULL_HANDLE, m_asBuf = VK_NULL_HANDLE, m_scratchBuf = VK_NULL_HANDLE;
  std::vector<VkDeviceMemory> m_memory;
  VkAccelerationStructureKHR m_as[3] = {};
  VkDeviceAddress m_geoAddr = 0, m_instAddr = 0, m_blasAddr[2] = {}, m_scratchAddr[3] = {};
  VkSampler m_sampler = VK_NULL_HANDLE;
  VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
  VkDescriptorPool m_pool = VK_NULL_HANDLE;
  VkDescriptorSet m_set = VK_NULL_HANDLE;
  VkPipelineLayout m_layout = VK_NULL_HANDLE;
  VkPipeline m_pipeline = VK_NULL_HANDLE;
  VkQueryPool m_queries = VK_NULL_HANDLE;
  volatile uint32_t* m_stats = nullptr;
  uint32_t m_statsSeen = 0;

  std::vector<Model> m_models;      // kept through drop(); their Vulkan objects are not
  std::vector<GiPixelMesh> m_meshes;
  std::vector<uint32_t> m_order;
  std::vector<GiPixelCaster> m_boxes;  // the last pass's mesh instances' world bounds
  std::vector<uint64_t> m_cellOf;
  uint32_t m_boundsLast = 0;
  std::vector<Chunk> m_chunks;
  std::vector<VkBuffer> m_retired;  // replaced scratch buffers, freed by drop()
  VkBuffer m_scratch = VK_NULL_HANDLE;
  VkDeviceSize m_scratchSize = 0;
  VkDeviceAddress m_scratchAddrMesh = 0;
  uint32_t m_built = 0, m_meshesLast = 0;

  Record m_record{};  // filled by run(), copied by the renderer
  uint32_t m_passes = 0, m_made = 0, m_faults = 0, m_raysLast = 0, m_gpuUs = 0, m_views = 0, m_viewFaults = 0;
  uint64_t m_rays = 0;
  const char* m_lastFault = "";
};
