// ReShade bridge (see reshade_bridge.h). The only file that includes the ReShade add-on headers.
//
// Registration: ReShade (d3d9.dll, dxgi.dll ...; any module exporting ReShadeRegisterAddon) accepts this ASI as an add-on.
// Apex calls the exports itself instead of reshade.hpp's helpers: it registers with API version 14 (ReShade 6.3), the
// oldest whose effect_runtime methods (up to reload_effect_next_frame) and event numbers match these headers; checked
// against the 6.0, 6.3 and 6.6 headers. ReShade refuses a newer version than its own, so an older ReShade reports TooOld.
// The normal ReShade build (limited add-on support) blocks external add-ons only from the events between
// destroy_effect_runtime and present; the ones used here (reshade_present, reshade_begin_effects,
// destroy_effect_runtime, reshade_reloaded_effects) are outside that range (source/addon_manager.cpp, 7bf9de8).
//
// Rendering point: effect_runtime::render_effects outside Present captures and restores the D3D9 state itself and marks
// the frame as rendered, so ReShade skips its own pass at Present ("to avoid effects being applied to user interface
// elements", reshade_api.hpp). With a zero render target it only updates uniforms and marks the frame: AtPresent uses that
// for frames where the boundary did not run, so the effects never land on the interface.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "reshade_bridge.h"
#include <windows.h>
#include <psapi.h>
#include <algorithm>
#include <cmath>
#include <cctype>
#include <cwctype>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <format>
#include <string_view>
#include "third_party/reshade/reshade_events.hpp"
extern "C" {
#include "third_party/puff/puff.h"
}
#include "apex_log.h"
#include "apex_paths.h"
#include "apex_util.h"
#include "depth_share.h"

#pragma comment(lib, "version.lib") // GetFileVersionInfoW (the ReShade DLL's version)

namespace ReShadeBridge {
namespace {

using namespace reshade;
using namespace reshade::api;

constexpr uint32_t kApiVersion = 14; // ReShade 6.3 (see the top of the file)
constexpr unsigned long long kSearchEveryMs = 2000;
constexpr size_t kMaxPackBytes = 64u * 1024u * 1024u;  // a shader pack .zip
constexpr size_t kMaxFileBytes = 64u * 1024u * 1024u;  // one extracted file
constexpr size_t kMaxPackFiles = 4000;

using RegisterAddonFn = bool (*)(void*, uint32_t);
using UnregisterAddonFn = void (*)(void*);
using EventFn = void (*)(addon_event, void*);
using GetConfigFn = bool (*)(void*, effect_runtime*, const char*, const char*, char*, size_t*);
using SetConfigArrayFn = void (*)(void*, effect_runtime*, const char*, const char*, const char*, size_t);
using BasePathFn = void (*)(char*, size_t*);

struct Exports {
    HMODULE module = nullptr;
    RegisterAddonFn registerAddon = nullptr;
    UnregisterAddonFn unregisterAddon = nullptr;
    EventFn registerEvent = nullptr, unregisterEvent = nullptr;
    GetConfigFn getConfig = nullptr;
    SetConfigArrayFn setConfigArray = nullptr;
    BasePathFn basePath = nullptr;
};

struct BridgeState {
    bool started = false;      // the feature is on
    bool registered = false;   // ReShade accepted the add-on
    bool refused = false;      // ReShade refused it (too old): not retried until Stop/Start
    Exports x;
    HMODULE self = nullptr;
    std::string version;
    effect_runtime* runtime = nullptr; // the runtime that presented last (render thread)
    unsigned long long lastSearch = 0;
    bool useDepth = false;
    bool depthBound = false;       // DEPTH points at Apex's texture
    bool renderedThisFrame = false; // render_effects ran at the boundary this frame
    bool multisampled = false;      // the back buffer is multisampled: ReShade keeps its default point
    bool loggedConnected = false, loggedOtherApi = false;
    int framesThisSecond = 0, framesLastSecond = 0;
    unsigned long long secondStart = 0;
    bool presetDirty = false;
};
BridgeState g;
std::atomic<int> g_state{static_cast<int>(State::Off)};

void SetState(State s) { g_state.store(static_cast<int>(s)); }

HMODULE SelfModule() {
    HMODULE m = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&SelfModule), &m);
    return m;
}

// The loaded module that exports ReShade's add-on functions (reshade.hpp's get_reshade_module_handle does the same)
HMODULE FindReShade() {
    HMODULE modules[1024];
    DWORD bytes = 0;
    if (!EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &bytes)) return nullptr;
    const DWORD n = std::min<DWORD>(bytes, sizeof(modules)) / sizeof(HMODULE);
    for (DWORD i = 0; i < n; i++)
        if (GetProcAddress(modules[i], "ReShadeRegisterAddon") && GetProcAddress(modules[i], "ReShadeUnregisterAddon")) return modules[i];
    return nullptr;
}

std::string FileVersion(HMODULE module) {
    wchar_t path[MAX_PATH] = {};
    if (!GetModuleFileNameW(module, path, MAX_PATH)) return {};
    DWORD dummy = 0;
    const DWORD size = GetFileVersionInfoSizeW(path, &dummy);
    if (!size) return {};
    std::vector<unsigned char> data(size);
    if (!GetFileVersionInfoW(path, 0, size, data.data())) return {};
    VS_FIXEDFILEINFO* info = nullptr;
    UINT len = 0;
    if (!VerQueryValueW(data.data(), L"\\", reinterpret_cast<void**>(&info), &len) || !info) return {};
    return std::format("{}.{}.{}", HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS), HIWORD(info->dwFileVersionLS));
}

template <typename T> T Proc(HMODULE m, const char* name) { return reinterpret_cast<T>(GetProcAddress(m, name)); }

// ---- events (called by ReShade on the render thread) ----
void OnReShadePresent(effect_runtime* runtime);
void OnDestroyRuntime(effect_runtime* runtime);
void OnBeginEffects(effect_runtime* runtime, command_list*, resource_view, resource_view);
void OnReloadedEffects(effect_runtime* runtime);

void Register(addon_event ev, void* fn) { g.x.registerEvent(ev, fn); }
void Unregister(addon_event ev, void* fn) { if (g.x.unregisterEvent) g.x.unregisterEvent(ev, fn); }

bool TryRegister() {
    const HMODULE module = FindReShade();
    if (!module) {
        SetState(State::NotFound);
        return false;
    }
    Exports x;
    x.module = module;
    x.registerAddon = Proc<RegisterAddonFn>(module, "ReShadeRegisterAddon");
    x.unregisterAddon = Proc<UnregisterAddonFn>(module, "ReShadeUnregisterAddon");
    x.registerEvent = Proc<EventFn>(module, "ReShadeRegisterEvent");
    x.unregisterEvent = Proc<EventFn>(module, "ReShadeUnregisterEvent");
    x.getConfig = Proc<GetConfigFn>(module, "ReShadeGetConfigValue");
    x.setConfigArray = Proc<SetConfigArrayFn>(module, "ReShadeSetConfigArray");
    x.basePath = Proc<BasePathFn>(module, "ReShadeGetBasePath");
    g.version = FileVersion(module);
    if (!x.registerAddon || !x.registerEvent) {
        LOG_WARNING("[ReShade] Found ReShade " + g.version + " without the add-on functions Apex needs");
        g.refused = true;
        SetState(State::TooOld);
        return false;
    }
    if (!g.self) g.self = SelfModule();
    if (!x.registerAddon(g.self, kApiVersion)) {
        LOG_WARNING(std::format("[ReShade] ReShade {} refused Apex Radiance as an add-on (API version {}): ReShade 6.3 or newer is needed", g.version, kApiVersion));
        g.refused = true;
        SetState(State::TooOld);
        return false;
    }
    g.x = x;
    g.registered = true;
    Register(addon_event::reshade_present, reinterpret_cast<void*>(&OnReShadePresent));
    Register(addon_event::destroy_effect_runtime, reinterpret_cast<void*>(&OnDestroyRuntime));
    Register(addon_event::reshade_begin_effects, reinterpret_cast<void*>(&OnBeginEffects));
    Register(addon_event::reshade_reloaded_effects, reinterpret_cast<void*>(&OnReloadedEffects));
    SetState(State::Registering);
    LOG_INFO("[ReShade] Registered with ReShade " + g.version + " as an add-on; waiting for its effect runtime");
    return true;
}

void UnbindDepth(effect_runtime* runtime) {
    if (!g.depthBound || !runtime) return;
    runtime->update_texture_bindings("DEPTH", resource_view{0}, resource_view{0});
    g.depthBound = false;
}

void UnregisterAddon() {
    if (!g.registered) return;
    if (g.runtime) {
        if (g.depthBound) {
            UnbindDepth(g.runtime);
            g.runtime->reload_effect_next_frame(nullptr); // ReShade's own depth add-on binds its choice again after a reload
        }
        if (g.presetDirty) g.runtime->save_current_preset();
    }
    Unregister(addon_event::reshade_present, reinterpret_cast<void*>(&OnReShadePresent));
    Unregister(addon_event::destroy_effect_runtime, reinterpret_cast<void*>(&OnDestroyRuntime));
    Unregister(addon_event::reshade_begin_effects, reinterpret_cast<void*>(&OnBeginEffects));
    Unregister(addon_event::reshade_reloaded_effects, reinterpret_cast<void*>(&OnReloadedEffects));
    if (g.x.unregisterAddon) g.x.unregisterAddon(g.self);
    g.registered = false;
    g.runtime = nullptr;
    g.presetDirty = false;
    LOG_INFO("[ReShade] Unregistered: ReShade renders at its default point again");
}

bool IsD3D9(effect_runtime* runtime) { return runtime && runtime->get_device() && runtime->get_device()->get_api() == device_api::d3d9; }

void OnReShadePresent(effect_runtime* runtime) {
    if (!g.started || !runtime) return;
    if (!IsD3D9(runtime)) {
        SetState(State::OtherApi);
        if (!g.loggedOtherApi) {
            g.loggedOtherApi = true;
            LOG_INFO("[ReShade] ReShade does not run on Direct3D 9 here (DXVK turns it into Vulkan): its effects stay at its default point, over the interface");
        }
        return;
    }
    g.runtime = runtime;
    SetState(State::Connected);
    if (!g.loggedConnected) {
        g.loggedConnected = true;
        LOG_INFO("[ReShade] Connected to the Direct3D 9 effect runtime: effects render before the game's interface");
        EnsureSearchPaths();
    }
}

void OnDestroyRuntime(effect_runtime* runtime) {
    if (runtime != g.runtime) return;
    g.runtime = nullptr;
    g.depthBound = false; // its bindings went with it
    if (g.started && g.registered) SetState(State::Registering);
}

// Runs inside render_effects (ours and ReShade's own at Present), after ReShade's depth add-on picked its depth buffer
// (registered before Apex): Apex's binding wins. A texture Apex released in the meantime is never left bound.
void OnBeginEffects(effect_runtime* runtime, command_list*, resource_view, resource_view) {
    if (runtime != g.runtime) return;
    IDirect3DTexture9* tex = g.useDepth ? DepthShare::Texture() : nullptr;
    if (tex) {
        const resource_view view{reinterpret_cast<uintptr_t>(tex)}; // D3D9 shader resource view = the texture (d3d9_impl_device.cpp)
        runtime->update_texture_bindings("DEPTH", view, view);
        g.depthBound = true;
    } else {
        UnbindDepth(runtime);
    }
}

void OnReloadedEffects(effect_runtime* runtime) {
    if (runtime == g.runtime) g.depthBound = false; // rebound at the next begin
}

std::string ConfigString(const char* section, const char* key) {
    if (!g.x.getConfig || !g.runtime) return {};
    size_t size = 0;
    if (!g.x.getConfig(g.self, g.runtime, section, key, nullptr, &size) || size == 0) return {};
    std::string value(size, '\0');
    g.x.getConfig(g.self, g.runtime, section, key, value.data(), &size);
    value.resize(size);
    return value; // elements separated by '\0'
}

std::vector<std::string> SplitNul(const std::string& s) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start < s.size()) {
        size_t end = s.find('\0', start);
        if (end == std::string::npos) end = s.size();
        if (end > start) out.emplace_back(s.substr(start, end - start));
        start = end + 1;
    }
    return out;
}

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::wstring LowerW(std::wstring s) {
    for (wchar_t& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

// Adds "<folder>\**" to a ReShade search path list unless it is already there (ReShade reloads its config)
bool AddSearchPath(const char* key, const std::wstring& folder) {
    if (!g.x.setConfigArray || !g.runtime) return false;
    const std::string wanted = ApexUtil::ToUtf8(folder) + "\\**";
    auto paths = SplitNul(ConfigString("GENERAL", key));
    for (const auto& p : paths)
        if (Lower(p) == Lower(wanted)) return true;
    paths.push_back(wanted);
    std::string joined;
    for (const auto& p : paths) {
        joined += p;
        joined.push_back('\0');
    }
    g.x.setConfigArray(g.self, g.runtime, "GENERAL", key, joined.data(), joined.size());
    LOG_INFO(std::format("[ReShade] Added {} to ReShade's {}", ApexUtil::ToUtf8(folder), key));
    return true;
}

std::wstring Sub(const wchar_t* name) { return EffectsFolder() + name + L"\\"; }

bool HasExtension(const std::wstring& name, std::initializer_list<const wchar_t*> exts) {
    const std::wstring ext = LowerW(std::filesystem::path(name).extension().wstring());
    for (const wchar_t* e : exts)
        if (ext == e) return true;
    return false;
}

// ---- ZIP shader packs (store and deflate; no ZIP64, no encryption). Only named payloads are written, below Apex's folder.
uint32_t Get(std::string_view in, size_t p, int count) {
    if (p > in.size() || static_cast<size_t>(count) > in.size() - p) throw std::runtime_error("truncated ZIP");
    uint32_t n = 0;
    for (int i = 0; i < count; ++i) n |= uint32_t(uint8_t(in[p + i])) << (8 * i);
    return n;
}

bool SafeRelative(const std::string& name) {
    if (name.empty() || name.size() > 260 || name.front() == '/' || name.find(':') != std::string::npos || name.find('\0') != std::string::npos)
        return false;
    size_t start = 0;
    while (start < name.size()) {
        size_t end = name.find('/', start);
        if (end == std::string::npos) end = name.size();
        const std::string_view part(name.data() + start, end - start);
        if (part.empty() || part == "." || part == "..") return false;
        start = end + 1;
    }
    return true;
}

struct ZipEntry { std::string name, bytes; };

std::vector<ZipEntry> ReadZip(const std::string& in) {
    if (in.size() < 22) throw std::runtime_error("not a ZIP file");
    // the end of central directory record (a comment may follow it)
    size_t end = std::string::npos;
    const size_t lowest = in.size() > 22 + 65535 ? in.size() - 22 - 65535 : 0;
    for (size_t p = in.size() - 22 + 1; p-- > lowest;)
        if (Get(in, p, 4) == 0x06054b50) { end = p; break; }
    if (end == std::string::npos) throw std::runtime_error("not a ZIP file");
    const uint32_t count = Get(in, end + 10, 2), dirOffset = Get(in, end + 16, 4);
    if (count == 0xFFFF || dirOffset == 0xFFFFFFFF) throw std::runtime_error("ZIP64 packs are not supported");
    if (count > kMaxPackFiles) throw std::runtime_error("too many files in the pack");
    std::vector<ZipEntry> out;
    size_t p = dirOffset, total = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (Get(in, p, 4) != 0x02014b50) throw std::runtime_error("damaged ZIP directory");
        const uint32_t flags = Get(in, p + 8, 2), method = Get(in, p + 10, 2), packed = Get(in, p + 20, 4), size = Get(in, p + 24, 4);
        const uint32_t nameSize = Get(in, p + 28, 2), extra = Get(in, p + 30, 2), comment = Get(in, p + 32, 2), local = Get(in, p + 42, 4);
        std::string name = in.substr(p + 46, nameSize);
        p += 46 + nameSize + extra + comment;
        std::replace(name.begin(), name.end(), '\\', '/');
        if (name.empty() || name.back() == '/') continue; // a folder
        if (flags & 1) throw std::runtime_error("encrypted ZIP packs are not supported");
        if (method != 0 && method != 8) throw std::runtime_error("unsupported ZIP compression");
        if (size > kMaxFileBytes || (total += size) > kMaxPackBytes) throw std::runtime_error("the pack is too large");
        if (Get(in, local, 4) != 0x04034b50) throw std::runtime_error("damaged ZIP entry");
        const size_t data = size_t(local) + 30 + Get(in, local + 26, 2) + Get(in, local + 28, 2);
        if (data > in.size() || packed > in.size() - data) throw std::runtime_error("damaged ZIP entry");
        ZipEntry e;
        e.name = std::move(name);
        if (method == 0) {
            if (packed != size) throw std::runtime_error("damaged ZIP entry");
            e.bytes = in.substr(data, size);
        } else {
            e.bytes.resize(size);
            unsigned long outLen = size, inLen = packed;
            if (puff(reinterpret_cast<unsigned char*>(e.bytes.data()), &outLen, reinterpret_cast<const unsigned char*>(in.data() + data), &inLen) != 0 ||
                outLen != size)
                throw std::runtime_error("damaged compressed data in the ZIP");
        }
        out.push_back(std::move(e));
    }
    return out;
}

// Where a file of a pack goes: below Shaders\ or Textures\ (keeping the folders after "Shaders/" / "Textures/"), a preset
// to Presets\, anything else (readme, add-ons, DLLs) is skipped. Empty = skip.
std::wstring Destination(const std::string& zipName) {
    if (!SafeRelative(zipName)) return {};
    const std::string lower = Lower(zipName);
    const std::wstring file = ApexUtil::ToWide(zipName.substr(zipName.find_last_of('/') + 1));
    auto below = [&](const char* folder) -> std::string {
        const std::string key = std::string(folder) + "/";
        size_t at = lower.rfind("/" + key);
        if (at != std::string::npos) return zipName.substr(at + key.size() + 1);
        if (lower.compare(0, key.size(), key) == 0) return zipName.substr(key.size());
        return {};
    };
    if (HasExtension(file, {L".addon", L".addon32", L".addon64", L".dll", L".exe", L".asi"})) return {};
    if (auto rel = below("shaders"); !rel.empty()) {
        std::wstring w = ApexUtil::ToWide(rel);
        std::replace(w.begin(), w.end(), L'/', L'\\');
        return Sub(L"Shaders") + w;
    }
    if (auto rel = below("textures"); !rel.empty()) {
        std::wstring w = ApexUtil::ToWide(rel);
        std::replace(w.begin(), w.end(), L'/', L'\\');
        return Sub(L"Textures") + w;
    }
    if (HasExtension(file, {L".fx", L".fxh"})) return Sub(L"Shaders") + file;
    if (HasExtension(file, {L".png", L".jpg", L".jpeg", L".bmp", L".dds", L".tga"})) return Sub(L"Textures") + file;
    if (HasExtension(file, {L".ini"}) && LowerW(file) != L"reshade.ini" && LowerW(file) != L"desktop.ini") return Sub(L"Presets") + file;
    return {};
}

bool WriteOut(const std::wstring& path, const std::string& bytes, std::string& error) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    if (ec) {
        error = "could not create " + ApexUtil::ToUtf8(std::filesystem::path(path).parent_path().wstring()) + ": " + ec.message();
        return false;
    }
    std::string why;
    if (!ApexUtil::WriteFileAtomic(path, bytes, &why)) {
        error = "could not write " + ApexUtil::ToUtf8(path) + ": " + why;
        return false;
    }
    return true;
}

std::string UniformString(effect_runtime* rt, effect_uniform_variable v, const char* name) {
    size_t size = 0;
    if (!rt->get_annotation_string_from_uniform_variable(v, name, nullptr, &size) || size <= 1) return {};
    std::string s(size, '\0');
    rt->get_annotation_string_from_uniform_variable(v, name, s.data(), &size);
    s.resize(size);
    return s;
}

std::string TechniqueString(effect_runtime* rt, effect_technique t, const char* name) {
    size_t size = 0;
    if (!rt->get_annotation_string_from_technique(t, name, nullptr, &size) || size <= 1) return {};
    std::string s(size, '\0');
    rt->get_annotation_string_from_technique(t, name, s.data(), &size);
    s.resize(size);
    return s;
}

} // namespace

// ---- lifecycle ----
void Start() {
    if (g.started) return;
    g.started = true;
    g.refused = false;
    g.lastSearch = 0;
    g.loggedConnected = g.loggedOtherApi = g.multisampled = false;
    SetState(State::NotFound);
}

void Stop() {
    if (!g.started) return;
    if (g.useDepth) DepthShare::Request(false);
    UnregisterAddon();
    g.started = false;
    g.useDepth = false;
    g.renderedThisFrame = false;
    SetState(State::Off);
}

void UseApexDepth(bool on) {
    on = on && g.started;
    if (on == g.useDepth) return;
    DepthShare::Request(on);
    g.useDepth = on;
    if (!on && g.runtime && g.depthBound) {
        UnbindDepth(g.runtime);
        g.runtime->reload_effect_next_frame(nullptr);
    }
}

void BeforeReset() {
    if (g.runtime) UnbindDepth(g.runtime);
}

void AtPresent(IDirect3DDevice9*) {
    if (!g.started) return;
    const unsigned long long now = GetTickCount64();
    if (now - g.secondStart >= 1000) {
        g.framesLastSecond = g.framesThisSecond;
        g.framesThisSecond = 0;
        g.secondStart = now;
    }
    if (!g.registered) {
        if (!g.refused && now - g.lastSearch >= kSearchEveryMs) {
            g.lastSearch = now;
            TryRegister();
        }
        return;
    }
    // This frame's effects did not run before the interface (a loading screen, a frame without the boundary, a multisampled
    // back buffer): mark the frame rendered with no target, so ReShade does not draw them over the menus at Present.
    if (g.runtime && !g.renderedThisFrame && !g.multisampled && IsD3D9(g.runtime)) {
        command_queue* queue = g.runtime->get_command_queue();
        if (command_list* cmd = queue ? queue->get_immediate_command_list() : nullptr) g.runtime->render_effects(cmd, resource_view{0}, resource_view{0});
    }
    g.renderedThisFrame = false;
}

void RenderBeforeInterface(IDirect3DDevice9* dev) {
    if (!g.started || !g.runtime || !IsD3D9(g.runtime) || g.renderedThisFrame) return;
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return;
    D3DSURFACE_DESC desc{};
    bb->GetDesc(&desc);
    if (desc.MultiSampleType != D3DMULTISAMPLE_NONE) {
        // ReShade cannot render into a multisampled target outside Present: leave this to ReShade's default point
        if (!g.multisampled) {
            g.multisampled = true;
            LOG_INFO("[ReShade] The back buffer is multisampled (the game's own Edge Smoothing): effects stay at ReShade's default point");
        }
        bb->Release();
        return;
    }
    g.multisampled = false;
    command_queue* queue = g.runtime->get_command_queue();
    command_list* cmd = queue ? queue->get_immediate_command_list() : nullptr;
    if (cmd) {
        const resource_view rtv{reinterpret_cast<uintptr_t>(bb)}; // D3D9 render target view = the surface
        g.runtime->render_effects(cmd, rtv, rtv);
        g.renderedThisFrame = true;
        g.framesThisSecond++;
    }
    bb->Release();
}

State GetState() { return static_cast<State>(g_state.load()); }
std::string Version() { return g.version; }
int FramesRenderedLastSecond() { return g.framesLastSecond; }
bool Multisampled() { return g.multisampled; }

// ---- menu ----
std::vector<Technique> Techniques() {
    std::vector<Technique> out;
    if (!g.runtime) return out;
    g.runtime->enumerate_techniques(nullptr, [](effect_runtime* rt, effect_technique t, void* user) {
        auto& list = *static_cast<std::vector<Technique>*>(user);
        if (bool hidden = false; rt->get_annotation_bool_from_technique(t, "hidden", &hidden, 1) && hidden) return;
        Technique tech;
        tech.handle = t.handle;
        char buf[256];
        size_t size = sizeof(buf);
        rt->get_technique_effect_name(t, buf, &size);
        tech.effect.assign(buf, size);
        size = sizeof(buf);
        rt->get_technique_name(t, buf, &size);
        tech.name.assign(buf, size);
        if (std::string label = TechniqueString(rt, t, "ui_label"); !label.empty()) tech.name = label;
        tech.tooltip = TechniqueString(rt, t, "ui_tooltip");
        tech.enabled = rt->get_technique_state(t);
        list.push_back(std::move(tech));
    }, &out);
    return out;
}

void SetTechnique(uint64_t handle, bool on) {
    if (!g.runtime || !handle) return;
    g.runtime->set_technique_state(effect_technique{handle}, on);
    g.runtime->save_current_preset();
    g.presetDirty = false;
}

std::vector<Uniform> Uniforms(const std::string& effect) {
    std::vector<Uniform> out;
    if (!g.runtime) return out;
    g.runtime->enumerate_uniform_variables(effect.c_str(), [](effect_runtime* rt, effect_uniform_variable v, void* user) {
        auto& list = *static_cast<std::vector<Uniform>*>(user);
        if (bool hidden = false; rt->get_annotation_bool_from_uniform_variable(v, "hidden", &hidden, 1) && hidden) return;
        if (!UniformString(rt, v, "source").empty()) return; // timer, frame count, mouse ...: set by ReShade
        format base = format::unknown;
        uint32_t rows = 0, columns = 0, length = 0;
        rt->get_uniform_variable_type(v, &base, &rows, &columns, &length);
        if (length > 1 || columns > 1 || rows < 1 || rows > 4) return; // arrays and matrices: ReShade's own menu
        Uniform u;
        u.handle = v.handle;
        u.components = static_cast<int>(rows);
        u.label = UniformString(rt, v, "ui_label");
        if (u.label.empty()) {
            char buf[256];
            size_t size = sizeof(buf);
            rt->get_uniform_variable_name(v, buf, &size);
            u.label.assign(buf, size);
        }
        u.tooltip = UniformString(rt, v, "ui_tooltip");
        u.category = UniformString(rt, v, "ui_category");
        const std::string type = UniformString(rt, v, "ui_type");
        float range[1] = {0};
        bool hasMin = rt->get_annotation_float_from_uniform_variable(v, "ui_min", range, 1);
        u.min = range[0];
        bool hasMax = rt->get_annotation_float_from_uniform_variable(v, "ui_max", range, 1);
        u.max = range[0];
        if (rt->get_annotation_float_from_uniform_variable(v, "ui_step", range, 1)) u.step = range[0];
        if (base == format::r32_typeless) {
            u.kind = UniformKind::Bool;
            u.components = 1;
            bool b = false;
            rt->get_uniform_value_bool(v, &b, 1);
            u.value[0] = b ? 1.0f : 0.0f;
        } else if (base == format::r32_sint || base == format::r32_uint || base == format::r16_sint || base == format::r16_uint) {
            u.kind = UniformKind::Int;
            if ((type == "combo" || type == "radio" || type == "list") && rows == 1) {
                u.kind = UniformKind::Combo;
                for (auto& item : SplitNul(UniformString(rt, v, "ui_items"))) u.items.push_back(std::move(item));
                if (u.items.empty()) u.kind = UniformKind::Int;
            }
            int32_t values[4] = {};
            rt->get_uniform_value_int(v, values, rows);
            for (uint32_t i = 0; i < rows; i++) u.value[i] = static_cast<float>(values[i]);
            if (!hasMin) u.min = 0.0f;
            if (!hasMax) u.max = u.kind == UniformKind::Combo ? static_cast<float>(u.items.size()) - 1.0f : 100.0f;
        } else if (base == format::r32_float || base == format::r16_float) {
            u.kind = (type == "color" && rows >= 3) ? UniformKind::Color : UniformKind::Float;
            rt->get_uniform_value_float(v, u.value, rows);
            if (!hasMin) u.min = 0.0f;
            if (!hasMax) u.max = 1.0f;
        } else {
            return;
        }
        u.drag = (type == "drag" || type == "input") && !(hasMin && hasMax);
        if (u.max < u.min) std::swap(u.min, u.max);
        list.push_back(std::move(u));
    }, &out);
    return out;
}

void SetUniform(const Uniform& u) {
    if (!g.runtime || !u.handle) return;
    const effect_uniform_variable v{u.handle};
    switch (u.kind) {
    case UniformKind::Bool: {
        const bool b = u.value[0] != 0.0f;
        g.runtime->set_uniform_value_bool(v, &b, 1);
        break;
    }
    case UniformKind::Int:
    case UniformKind::Combo: {
        int32_t values[4];
        for (int i = 0; i < 4; i++) values[i] = static_cast<int32_t>(std::lround(u.value[i]));
        g.runtime->set_uniform_value_int(v, values, static_cast<size_t>(u.components));
        break;
    }
    default:
        g.runtime->set_uniform_value_float(v, u.value, static_cast<size_t>(u.components));
        break;
    }
    g.presetDirty = true;
}

void ResetUniform(const Uniform& u) {
    if (!g.runtime || !u.handle) return;
    g.runtime->reset_uniform_value(effect_uniform_variable{u.handle});
    g.presetDirty = true;
}

void SavePreset() {
    if (!g.runtime || !g.presetDirty) return;
    g.runtime->save_current_preset();
    g.presetDirty = false;
}

std::wstring ReShadeFolder() {
    if (!g.x.basePath) return {};
    char buf[MAX_PATH * 3] = {};
    size_t size = sizeof(buf);
    g.x.basePath(buf, &size);
    std::wstring folder = ApexUtil::ToWide(std::string(buf, strnlen(buf, sizeof(buf))));
    if (!folder.empty() && folder.back() != L'\\' && folder.back() != L'/') folder.push_back(L'\\');
    return folder;
}

// A ReShade preset: an .ini with a Techniques= or PreprocessorDefinitions= line near its top (the game's folder holds
// other .ini files too)
bool LooksLikePreset(const std::filesystem::path& path) {
    const std::wstring name = LowerW(path.filename().wstring());
    if (LowerW(path.extension().wstring()) != L".ini" || name == L"reshade.ini" || name == L"desktop.ini") return false;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    char buf[8192];
    DWORD read = 0;
    const BOOL ok = ReadFile(h, buf, sizeof(buf), &read, nullptr);
    CloseHandle(h);
    if (!ok) return false;
    const std::string_view text(buf, read);
    return text.find("Techniques=") != std::string_view::npos || text.find("PreprocessorDefinitions=") != std::string_view::npos;
}

std::vector<std::wstring> PresetFiles() {
    std::vector<std::wstring> out;
    auto scan = [&](const std::wstring& folder) {
        std::error_code ec;
        if (folder.empty() || !std::filesystem::is_directory(folder, ec)) return;
        for (const auto& entry : std::filesystem::directory_iterator(folder, ec))
            if (entry.is_regular_file(ec) && LooksLikePreset(entry.path())) out.push_back(entry.path().wstring());
    };
    scan(ReShadeFolder());
    scan(Sub(L"Presets"));
    const std::wstring current = CurrentPreset();
    if (!current.empty() && std::none_of(out.begin(), out.end(), [&](const std::wstring& p) { return LowerW(p) == LowerW(current); })) out.push_back(current);
    std::sort(out.begin(), out.end(), [](const std::wstring& a, const std::wstring& b) {
        return LowerW(std::filesystem::path(a).filename().wstring()) < LowerW(std::filesystem::path(b).filename().wstring());
    });
    return out;
}

std::wstring CurrentPreset() {
    if (!g.runtime) return {};
    char buf[MAX_PATH * 3] = {};
    size_t size = sizeof(buf);
    g.runtime->get_current_preset_path(buf, &size);
    return ApexUtil::ToWide(std::string(buf, strnlen(buf, sizeof(buf))));
}

void SetPreset(const std::wstring& path) {
    if (!g.runtime || path.empty()) return;
    SavePreset();
    g.runtime->set_current_preset_path(ApexUtil::ToUtf8(path).c_str());
    LOG_INFO("[ReShade] Preset: " + ApexUtil::ToUtf8(path));
}

std::wstring EffectsFolder() { return ApexPaths::ApexDirectory() + L"ReShade\\"; }

bool EnsureSearchPaths() {
    std::error_code ec;
    if (!std::filesystem::is_directory(EffectsFolder(), ec)) return false; // nothing added yet: ReShade's config stays untouched
    const bool shaders = AddSearchPath("EffectSearchPaths", EffectsFolder() + L"Shaders");
    const bool textures = AddSearchPath("TextureSearchPaths", EffectsFolder() + L"Textures");
    return shaders && textures;
}

void Reload() {
    if (g.runtime) g.runtime->reload_effect_next_frame(nullptr);
}

int AddFiles(const std::vector<std::wstring>& paths, std::string& error) {
    int added = 0;
    std::error_code ec;
    for (const wchar_t* sub : {L"Shaders", L"Textures", L"Presets"}) std::filesystem::create_directories(Sub(sub), ec);
    for (const std::wstring& path : paths) {
        const std::wstring file = std::filesystem::path(path).filename().wstring();
        try {
            if (HasExtension(file, {L".zip"})) {
                std::string bytes;
                const auto size = std::filesystem::file_size(path, ec);
                if (ec || size > kMaxPackBytes) throw std::runtime_error(ec ? "could not read it" : "the pack is too large");
                if (!ApexUtil::ReadFileBytes(path, bytes)) throw std::runtime_error("could not read it");
                int fromPack = 0;
                for (const ZipEntry& e : ReadZip(bytes)) {
                    const std::wstring dest = Destination(e.name);
                    if (dest.empty()) continue;
                    if (!WriteOut(dest, e.bytes, error)) return added;
                    fromPack++;
                }
                if (fromPack == 0) throw std::runtime_error("no shaders, textures or presets inside");
                LOG_INFO(std::format("[ReShade] Added {} files from {}", fromPack, ApexUtil::ToUtf8(file)));
                added += fromPack;
                continue;
            }
            const std::wstring dest = Destination(ApexUtil::ToUtf8(file));
            if (dest.empty()) throw std::runtime_error("not an effect, texture or preset file");
            std::string bytes;
            const auto size = std::filesystem::file_size(path, ec);
            if (ec || size > kMaxFileBytes || !ApexUtil::ReadFileBytes(path, bytes)) throw std::runtime_error("could not read it");
            if (!WriteOut(dest, bytes, error)) return added;
            LOG_INFO("[ReShade] Added " + ApexUtil::ToUtf8(dest));
            added++;
        } catch (const std::exception& e) {
            error = ApexUtil::ToUtf8(file) + ": " + e.what();
            LOG_WARNING("[ReShade] Could not add " + error);
            return added;
        }
    }
    return added;
}

std::vector<std::wstring> AddedEffects() {
    std::vector<std::wstring> out;
    std::error_code ec;
    const std::wstring folder = Sub(L"Shaders");
    if (!std::filesystem::is_directory(folder, ec)) return out;
    for (auto it = std::filesystem::recursive_directory_iterator(folder, ec); !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
        if (it->is_regular_file(ec) && LowerW(it->path().extension().wstring()) == L".fx") out.push_back(it->path().filename().wstring());
    return out;
}

bool RemoveEffect(const std::wstring& fileName) {
    if (fileName.empty() || fileName.find_first_of(L"\\/:") != std::wstring::npos) return false;
    std::error_code ec;
    const std::wstring folder = Sub(L"Shaders");
    bool removed = false;
    for (auto it = std::filesystem::recursive_directory_iterator(folder, ec); !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec) || LowerW(it->path().filename().wstring()) != LowerW(fileName)) continue;
        const auto target = it->path();
        if (std::filesystem::remove(target, ec)) {
            LOG_INFO("[ReShade] Removed " + ApexUtil::ToUtf8(target.wstring()));
            removed = true;
        }
        break;
    }
    if (removed) Reload();
    return removed;
}

} // namespace ReShadeBridge
