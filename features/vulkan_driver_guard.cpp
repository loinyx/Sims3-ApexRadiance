// One graphics driver in the game: keeps an AMD integrated GPU's Vulkan driver out of TS3W (see vulkan_driver_guard.h).
//
// Part of Apex Radiance. Credits: @loinyx
#include "vulkan_driver_guard.h"
#include "apex_log.h"
#include "platform.h"
#include <windows.h>
#include <dxgi.h>
#pragma comment(lib, "advapi32.lib")
#include <algorithm>
#include <cwctype>
#include <format>
#include <mutex>
#include <string>
#include <vector>

namespace VulkanDriverGuard {
namespace {

constexpr UINT kVendorAmd = 0x1002;
// AMD's 32-bit driver manifests (amd-vulkan32.json; older drivers amdvlk32.json): loader globs, matched on the file name
constexpr const wchar_t* kDisable = L"amd-vulkan32.json,amdvlk32.json";

std::once_flag g_once;
std::string g_decision;

bool EnvSet(const wchar_t* name) { return GetEnvironmentVariableW(name, nullptr, 0) > 0; }

struct Adapter {
    std::wstring name;
    UINT vendor = 0;
    unsigned long long dedicated = 0;
};

bool ListAdapters(std::vector<Adapter>& out) {
    HMODULE dxgi = LoadLibraryW(L"dxgi.dll");
    if (!dxgi) return false;
    using CreateFactory1 = HRESULT(WINAPI*)(REFIID, void**);
    auto create = reinterpret_cast<CreateFactory1>(GetProcAddress(dxgi, "CreateDXGIFactory1"));
    IDXGIFactory1* factory = nullptr;
    if (!create || FAILED(create(__uuidof(IDXGIFactory1), reinterpret_cast<void**>(&factory))) || !factory) return false;
    IDXGIAdapter1* a = nullptr;
    for (UINT i = 0; factory->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; i++) {
        DXGI_ADAPTER_DESC1 d{};
        if (a && SUCCEEDED(a->GetDesc1(&d)) && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE)) out.push_back({d.Description, d.VendorId, d.DedicatedVideoMemory});
        if (a) a->Release();
        a = nullptr;
    }
    factory->Release();
    return true;
}

std::string Narrow(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s += c < 128 ? static_cast<char>(c) : '?';
    return s;
}

// AMD registers the same manifest as an implicit LAYER too (VK_LAYER_AMD_switchable_graphics, library amdvlk32.dll; the
// display adapter's VulkanImplicitLayersWow, seen on a test PC), and VK_LOADER_DRIVERS_DISABLE does not stop layers:
// amdvlk32.dll (~85 MB) was still loaded. The layer's own "disable_environment" variable keeps it out: its known name.
// 2.7.0 read it from every AMD manifest registered for a display adapter (enumerating the display class keys of the
// registry); a generic antivirus heuristic flagged that build as a false positive, so the name the manifest gives is set directly.
std::vector<std::pair<std::wstring, std::wstring>> AmdLayerDisables() {
    return {{L"DISABLE_LAYER_AMD_SWITCHABLE_GRAPHICS_1", L"1"}};
}

std::string Decide() {
    if (Platform::RemixBridge()) return "left as it is: RTX Remix bridge (DXVK and the Vulkan loader run in another process)";
    if (GetModuleHandleW(L"vulkan-1.dll")) return "left as it is: the Vulkan loader was already loaded";
    for (const wchar_t* v : {L"VK_LOADER_DRIVERS_DISABLE", L"VK_LOADER_DRIVERS_SELECT", L"VK_DRIVER_FILES", L"VK_ICD_FILENAMES", L"VK_ADD_DRIVER_FILES"})
        if (EnvSet(v)) return "left as it is: a Vulkan driver variable is already set";
    std::vector<Adapter> adapters;
    if (!ListAdapters(adapters)) return "left as it is: the graphics adapters could not be listed";
    const Adapter* main = nullptr;
    bool haveAmd = false;
    for (const Adapter& a : adapters) {
        if (!main || a.dedicated > main->dedicated) main = &a;
        haveAmd |= a.vendor == kVendorAmd;
    }
    std::string list;
    for (const Adapter& a : adapters) list += std::format("{}{} ({:04X}, {} MB)", list.empty() ? "" : ", ", Narrow(a.name), a.vendor, a.dedicated >> 20);
    if (!haveAmd) return "no AMD graphics driver to leave out (" + list + ")";
    if (!main || main->vendor == kVendorAmd) return "left as it is: the main graphics card is AMD (" + list + ")";
    for (const Adapter& a : adapters)
        if (a.vendor == kVendorAmd && a.dedicated * 2 >= main->dedicated) return "left as it is: the AMD adapter is not a small integrated GPU (" + list + ")";
    if (!SetEnvironmentVariableW(L"VK_LOADER_DRIVERS_DISABLE", kDisable)) return "the variable could not be set";
    std::string layers;
    for (const auto& [name, value] : AmdLayerDisables())
        if (!EnvSet(name.c_str()) && SetEnvironmentVariableW(name.c_str(), value.c_str())) layers += (layers.empty() ? "" : ", ") + Narrow(name);
    return "AMD's 32-bit Vulkan driver left out of the game (VK_LOADER_DRIVERS_DISABLE" + (layers.empty() ? std::string() : ", and its implicit layer: " + layers) +
           "; this process only); the game renders on " + Narrow(main->name) + " (" + list + ")";
}

} // namespace

void BeforeDirect3DCreate() {
    std::call_once(g_once, [] {
        g_decision = Decide();
        LOG_INFO("[VulkanDriverGuard] " + g_decision);
    });
}

const char* Decision() { return g_decision.c_str(); }

} // namespace VulkanDriverGuard
