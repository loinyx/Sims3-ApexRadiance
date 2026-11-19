// dxvk_external_pass: checks patch 0004 (ID3D9VkExternalPass) without the game. On the given x64 d3d9.dll it creates a
// 64x64 render target with storage usage, clears it red through Direct3D 9, then records a compute dispatch through
// ExecuteExternal that writes green into the left half, and reads the target back with GetRenderTargetData.
// Expected: left half green, right half red, after the first pass and after 1000 more. Stock DXVK answers
// E_NOINTERFACE; the test then prints "external pass: no" and exits 0.
// Build (x64 developer prompt, from this folder):
//   glslang -V --target-env vulkan1.3 --vn kFillSpv -o fill_spv.h fill.comp
//   cl /nologo /EHsc /O2 /I..\src\include\vulkan\include dxvk_external_pass.cpp user32.lib
// Run: dxvk_external_pass.exe <path to d3d9.dll>
#include <windows.h>
#include <d3d9.h>
#define VK_NO_PROTOTYPES
#include <vulkan/vulkan.h>
#include <cstdio>
#include <cstdint>
#include "fill_spv.h"

// The DXVK interop interfaces, as declared in src/d3d9/d3d9_interfaces.h (method order matters)
struct D3D9VkExtImageDesc {
  D3DRESOURCETYPE Type; UINT Width, Height, Depth, MipLevels; DWORD Usage; D3DFORMAT Format; D3DPOOL Pool;
  D3DMULTISAMPLE_TYPE MultiSample; DWORD MultiSampleQuality; bool Discard, IsAttachmentOnly, IsLockable;
  VkImageUsageFlags ImageUsage;
};
MIDL_INTERFACE("d56344f5-8d35-46fd-806d-94c351b472c1")
ID3D9VkInteropTexture : public IUnknown {
  virtual HRESULT STDMETHODCALLTYPE GetVulkanImageInfo(VkImage*, VkImageLayout*, VkImageCreateInfo*) = 0;
};
MIDL_INTERFACE("2eaa4b89-0107-4bdb-87f7-0f541c493ce0")
ID3D9VkInteropDevice : public IUnknown {
  virtual void STDMETHODCALLTYPE GetVulkanHandles(VkInstance*, VkPhysicalDevice*, VkDevice*) = 0;
  virtual void STDMETHODCALLTYPE GetSubmissionQueue(VkQueue*, uint32_t*, uint32_t*) = 0;
  virtual void STDMETHODCALLTYPE TransitionTextureLayout(ID3D9VkInteropTexture*, const VkImageSubresourceRange*, VkImageLayout, VkImageLayout) = 0;
  virtual void STDMETHODCALLTYPE FlushRenderingCommands() = 0;
  virtual void STDMETHODCALLTYPE LockSubmissionQueue() = 0;
  virtual void STDMETHODCALLTYPE ReleaseSubmissionQueue() = 0;
  virtual void STDMETHODCALLTYPE LockDevice() = 0;
  virtual void STDMETHODCALLTYPE UnlockDevice() = 0;
  virtual bool STDMETHODCALLTYPE WaitForResource(IDirect3DResource9*, DWORD) = 0;
  virtual HRESULT STDMETHODCALLTYPE CreateImage(const D3D9VkExtImageDesc*, IDirect3DResource9**) = 0;
};
typedef void (STDMETHODCALLTYPE *PFN_D3D9VkExternalPass)(VkCommandBuffer, const void*);
MIDL_INTERFACE("7daa16ca-164c-44ea-b567-7f73c71ddaf8")
ID3D9VkExternalPass : public IUnknown {
  virtual BOOL STDMETHODCALLTYPE SupportsRayQuery() = 0;
  virtual HRESULT STDMETHODCALLTYPE ExecuteExternal(PFN_D3D9VkExternalPass, const void*, UINT, UINT,
    IDirect3DResource9* const*, const VkImageLayout*) = 0;
};

static PFN_vkCmdBindPipeline g_bindPipeline;
static PFN_vkCmdBindDescriptorSets g_bindSets;
static PFN_vkCmdPushConstants g_push;
static PFN_vkCmdDispatch g_dispatch;

struct PassData { VkPipeline pipeline; VkPipelineLayout layout; VkDescriptorSet set; uint32_t groupsX, groupsY; float color[4]; };

static void STDMETHODCALLTYPE RecordFill(VkCommandBuffer cmd, const void* data) {
  const PassData& d = *static_cast<const PassData*>(data);
  g_bindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d.pipeline);
  g_bindSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, d.layout, 0, 1, &d.set, 0, nullptr);
  g_push(cmd, d.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(d.color), d.color);
  g_dispatch(cmd, d.groupsX, d.groupsY, 1);
}

#define CHECK(x) do { HRESULT hr_ = (x); if (FAILED(hr_)) { std::printf("FAIL %s: %08lx\n", #x, (unsigned long)hr_); return 1; } } while (0)
#define VKCHECK(x) do { VkResult r_ = (x); if (r_ != VK_SUCCESS) { std::printf("FAIL %s: %d\n", #x, (int)r_); return 1; } } while (0)

// Reads the target back; returns the number of pixels that differ from left-green / right-red
static int CountWrong(IDirect3DDevice9* dev, IDirect3DSurface9* rt, IDirect3DSurface9* sys) {
  if (FAILED(dev->GetRenderTargetData(rt, sys))) return -1;
  D3DLOCKED_RECT lr;
  if (FAILED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) return -1;
  int wrong = 0;
  for (int y = 0; y < 64; y++)
    for (int x = 0; x < 64; x++) {
      const uint8_t* p = static_cast<const uint8_t*>(lr.pBits) + y * lr.Pitch + x * 4;  // R, G, B, A
      bool green = p[0] == 0 && p[1] == 255 && p[2] == 0;
      bool red = p[0] == 255 && p[1] == 0 && p[2] == 0;
      wrong += (x < 32) ? !green : !red;
    }
  sys->UnlockRect();
  return wrong;
}

int main(int argc, char** argv) {
  if (argc < 2) { std::fprintf(stderr, "usage: dxvk_external_pass <d3d9.dll>\n"); return 2; }
  HMODULE dll = LoadLibraryA(argv[1]);
  auto create = dll ? reinterpret_cast<IDirect3D9* (WINAPI*)(UINT)>(GetProcAddress(dll, "Direct3DCreate9")) : nullptr;
  IDirect3D9* d3d = create ? create(D3D_SDK_VERSION) : nullptr;
  if (!d3d) { std::printf("FAIL: no Direct3D 9 from %s\n", argv[1]); return 1; }

  WNDCLASSA wc = {};
  wc.lpfnWndProc = DefWindowProcA;
  wc.hInstance = GetModuleHandleA(nullptr);
  wc.lpszClassName = "dxvk_external_pass";
  RegisterClassA(&wc);
  HWND wnd = CreateWindowA(wc.lpszClassName, wc.lpszClassName, WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
  D3DPRESENT_PARAMETERS pp = {};
  pp.Windowed = TRUE;
  pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
  pp.BackBufferFormat = D3DFMT_X8R8G8B8;
  pp.hDeviceWindow = wnd;
  IDirect3DDevice9* dev = nullptr;
  CHECK(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev));

  ID3D9VkExternalPass* pass = nullptr;
  if (FAILED(dev->QueryInterface(__uuidof(ID3D9VkExternalPass), reinterpret_cast<void**>(&pass)))) {
    std::printf("external pass: no\n");
    return 0;
  }
  std::printf("external pass: yes, ray query: %s\n", pass->SupportsRayQuery() ? "yes" : "no");
  ID3D9VkInteropDevice* interop = nullptr;
  CHECK(dev->QueryInterface(__uuidof(ID3D9VkInteropDevice), reinterpret_cast<void**>(&interop)));

  D3D9VkExtImageDesc desc = {};
  desc.Type = D3DRTYPE_TEXTURE;
  desc.Width = desc.Height = 64;
  desc.Depth = desc.MipLevels = 1;
  desc.Usage = D3DUSAGE_RENDERTARGET;
  desc.Format = D3DFMT_A8B8G8R8;
  desc.Pool = D3DPOOL_DEFAULT;
  desc.ImageUsage = VK_IMAGE_USAGE_STORAGE_BIT;
  IDirect3DResource9* res = nullptr;
  CHECK(interop->CreateImage(&desc, &res));
  IDirect3DTexture9* tex = static_cast<IDirect3DTexture9*>(res);
  IDirect3DSurface9* rt = nullptr;
  CHECK(tex->GetSurfaceLevel(0, &rt));
  IDirect3DSurface9* sys = nullptr;
  CHECK(dev->CreateOffscreenPlainSurface(64, 64, D3DFMT_A8B8G8R8, D3DPOOL_SYSTEMMEM, &sys, nullptr));

  ID3D9VkInteropTexture* itex = nullptr;
  CHECK(tex->QueryInterface(__uuidof(ID3D9VkInteropTexture), reinterpret_cast<void**>(&itex)));
  VkImage image = VK_NULL_HANDLE;
  VkImageCreateInfo info = { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
  CHECK(itex->GetVulkanImageInfo(&image, nullptr, &info));

  VkInstance vkInst; VkPhysicalDevice vkPhys; VkDevice vkDev;
  interop->GetVulkanHandles(&vkInst, &vkPhys, &vkDev);
  HMODULE vk = LoadLibraryA("vulkan-1.dll");
  auto gipa = reinterpret_cast<PFN_vkGetInstanceProcAddr>(GetProcAddress(vk, "vkGetInstanceProcAddr"));
  auto gdpa = reinterpret_cast<PFN_vkGetDeviceProcAddr>(gipa(vkInst, "vkGetDeviceProcAddr"));
#define LOAD(name) auto name = reinterpret_cast<PFN_##name>(gdpa(vkDev, #name))
  LOAD(vkCreateImageView); LOAD(vkDestroyImageView); LOAD(vkCreateDescriptorSetLayout); LOAD(vkDestroyDescriptorSetLayout);
  LOAD(vkCreateDescriptorPool); LOAD(vkDestroyDescriptorPool); LOAD(vkAllocateDescriptorSets); LOAD(vkUpdateDescriptorSets);
  LOAD(vkCreatePipelineLayout); LOAD(vkDestroyPipelineLayout); LOAD(vkCreateShaderModule); LOAD(vkDestroyShaderModule);
  LOAD(vkCreateComputePipelines); LOAD(vkDestroyPipeline); LOAD(vkDeviceWaitIdle);
  g_bindPipeline = reinterpret_cast<PFN_vkCmdBindPipeline>(gdpa(vkDev, "vkCmdBindPipeline"));
  g_bindSets = reinterpret_cast<PFN_vkCmdBindDescriptorSets>(gdpa(vkDev, "vkCmdBindDescriptorSets"));
  g_push = reinterpret_cast<PFN_vkCmdPushConstants>(gdpa(vkDev, "vkCmdPushConstants"));
  g_dispatch = reinterpret_cast<PFN_vkCmdDispatch>(gdpa(vkDev, "vkCmdDispatch"));

  VkImageViewCreateInfo viewInfo = { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
  viewInfo.image = image;
  viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D;
  viewInfo.format = info.format;
  viewInfo.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
  VkImageView view;
  VKCHECK(vkCreateImageView(vkDev, &viewInfo, nullptr, &view));
  VkDescriptorSetLayoutBinding binding = { 0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT };
  VkDescriptorSetLayoutCreateInfo setInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO };
  setInfo.bindingCount = 1;
  setInfo.pBindings = &binding;
  VkDescriptorSetLayout setLayout;
  VKCHECK(vkCreateDescriptorSetLayout(vkDev, &setInfo, nullptr, &setLayout));
  VkDescriptorPoolSize poolSize = { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1 };
  VkDescriptorPoolCreateInfo poolInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO };
  poolInfo.maxSets = 1;
  poolInfo.poolSizeCount = 1;
  poolInfo.pPoolSizes = &poolSize;
  VkDescriptorPool pool;
  VKCHECK(vkCreateDescriptorPool(vkDev, &poolInfo, nullptr, &pool));
  VkDescriptorSetAllocateInfo allocInfo = { VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO };
  allocInfo.descriptorPool = pool;
  allocInfo.descriptorSetCount = 1;
  allocInfo.pSetLayouts = &setLayout;
  PassData data = {};
  VKCHECK(vkAllocateDescriptorSets(vkDev, &allocInfo, &data.set));
  VkDescriptorImageInfo imageInfo = { VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL };
  VkWriteDescriptorSet write = { VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET };
  write.dstSet = data.set;
  write.descriptorCount = 1;
  write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
  write.pImageInfo = &imageInfo;
  vkUpdateDescriptorSets(vkDev, 1, &write, 0, nullptr);
  VkPushConstantRange range = { VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(data.color) };
  VkPipelineLayoutCreateInfo layoutInfo = { VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO };
  layoutInfo.setLayoutCount = 1;
  layoutInfo.pSetLayouts = &setLayout;
  layoutInfo.pushConstantRangeCount = 1;
  layoutInfo.pPushConstantRanges = &range;
  VKCHECK(vkCreatePipelineLayout(vkDev, &layoutInfo, nullptr, &data.layout));
  VkShaderModuleCreateInfo moduleInfo = { VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO };
  moduleInfo.codeSize = sizeof(kFillSpv);
  moduleInfo.pCode = kFillSpv;
  VkShaderModule module;
  VKCHECK(vkCreateShaderModule(vkDev, &moduleInfo, nullptr, &module));
  VkComputePipelineCreateInfo pipeInfo = { VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO };
  pipeInfo.stage = { VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, module, "main" };
  pipeInfo.layout = data.layout;
  VKCHECK(vkCreateComputePipelines(vkDev, VK_NULL_HANDLE, 1, &pipeInfo, nullptr, &data.pipeline));
  data.groupsX = 32 / 8;
  data.groupsY = 64 / 8;
  data.color[1] = data.color[3] = 1.0f;

  const VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL;
  int fails = 0;
  for (int round = 0; round < 2; round++) {
    int passes = round == 0 ? 1 : 1000;
    for (int i = 0; i < passes; i++) {
      // Direct3D 9 clears the whole target red, then the external pass paints the left half green
      CHECK(dev->SetRenderTarget(0, rt));
      CHECK(dev->Clear(0, nullptr, D3DCLEAR_TARGET, D3DCOLOR_XRGB(255, 0, 0), 1.0f, 0));
      CHECK(pass->ExecuteExternal(RecordFill, &data, sizeof(data), 1, &res, &layout));
      if (i % 100 == 99) dev->Present(nullptr, nullptr, nullptr, nullptr);
    }
    int wrong = CountWrong(dev, rt, sys);
    std::printf("after %d pass(es): %d wrong pixels\n", round == 0 ? 1 : 1001, wrong);
    fails += wrong != 0;
  }
  // a null callback is refused
  HRESULT hrNull = pass->ExecuteExternal(nullptr, nullptr, 0, 0, nullptr, nullptr);
  std::printf("null callback: %08lx\n", static_cast<unsigned long>(hrNull));
  fails += hrNull != D3DERR_INVALIDCALL;

  interop->FlushRenderingCommands();
  vkDeviceWaitIdle(vkDev);
  vkDestroyPipeline(vkDev, data.pipeline, nullptr);
  vkDestroyShaderModule(vkDev, module, nullptr);
  vkDestroyPipelineLayout(vkDev, data.layout, nullptr);
  vkDestroyDescriptorPool(vkDev, pool, nullptr);
  vkDestroyDescriptorSetLayout(vkDev, setLayout, nullptr);
  vkDestroyImageView(vkDev, view, nullptr);
  itex->Release(); sys->Release(); rt->Release(); res->Release(); interop->Release(); pass->Release();
  dev->Release(); d3d->Release();
  DestroyWindow(wnd);
  std::printf("%s\n", fails ? "FAIL" : "PASS");
  return fails ? 1 : 0;
}
