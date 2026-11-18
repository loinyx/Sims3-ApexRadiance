// What the Shaders and Capture pages reach outside the menu: the feature system, settings file, hooks and logs
#define NOMINMAX
#include <windows.h>
#include <filesystem>
#include <memory>
#include <vector>
#include "patch_base.h"
#include "apex_config.h"
#include "apex_paths.h"
#include "d3d9_hooks.h"
#include "d3d9_extra_hooks.h"
#include "post_scene.h"
#include "depth_share.h"
#include "shader_cache.h"
#include "crash_report.h"
#include "hook_guard.h"
#include "game_addresses.h"

std::vector<std::unique_ptr<ApexPatch> (*)()> g_factories;
PatchRegistration::PatchRegistration(PatchFactory factory, FeatureInfo) { g_factories.push_back(factory); }
ApexPatch::ApexPatch(std::string name, void*) : patchName(std::move(name)) {}
ApexPatch::~ApexPatch() = default;
void ApexPatch::Update() {}
void ApexPatch::RenderCustomUI() {}
void ApexPatch::SaveToToml(toml::table&) const {}
bool ApexPatch::LoadFromToml(const toml::table&) { return true; }
void ApexPatch::ApplyTableLive(const toml::table&) {}
void ApexPatch::NotifySettingChanged() {}
bool ApexPatch::IsCompatibleWithCurrentVersion() const { return true; }
std::string ApexPatch::UnavailableReason() const { return {}; }
void ApexPatch::RegisterFloatSetting(float*, const std::string&, SettingWidget, float, float, float, const std::string&, const std::vector<std::pair<std::string, float>>&) {}
void ApexPatch::RegisterBoolSetting(bool*, const std::string&, bool, const std::string&) {}
PatchManager& PatchManager::Get() { static PatchManager m; return m; }
void PatchManager::SetUnsavedChanges(bool) {}

namespace ApexLog { void Write(Level, const std::string&, const std::source_location&) noexcept {} bool DebugOn() noexcept { return false; } }
namespace D3D9Hooks {
bool RegisterDrawIndexedPrimitive(const std::string&, DrawIndexedPrimitiveHook, Priority) { return true; }
bool RegisterDrawPrimitive(const std::string&, DrawPrimitiveHook, Priority) { return true; }
bool RegisterSetRenderTarget(const std::string&, SetRenderTargetHook, Priority) { return true; }
bool RegisterSetViewport(const std::string&, SetViewportHook, Priority) { return true; }
bool RegisterPresent(const std::string&, PresentHook, Priority) { return true; }
void UnregisterAll(const std::string&) {}
HRESULT CallOriginalSetRenderTarget(IDirect3DDevice9*, DWORD, IDirect3DSurface9*) { return S_OK; }
}
namespace ExtraHooks {
HRESULT RawSetDepthStencilSurface(IDirect3DDevice9*, IDirect3DSurface9*) { return S_OK; }
HRESULT RawGetDepthStencilSurface(IDirect3DDevice9*, IDirect3DSurface9**) { return S_OK; }
void UseVertexInputSetLast(IDirect3DVertexDeclaration9*&, DWORD&) {}
}
namespace PostScene {
void Add(int, Effect, bool) {}
void Remove(Effect) {}
bool SceneDepthValid() { return false; }
void WantCamera(bool, bool) {}
int DepthWritesThisFrame() { return 0; }
int DepthWritesLastFrame() { return 0; }
bool Counting() { return false; }
bool ColourTilesStarted() { return false; }
float ColourCurveLastFrame() { return 0.0f; }
void SetBeforeColourPass(Effect) {}
bool BoundaryOnDraw() { return false; }
}
namespace DepthShare {
IDirect3DTexture9* Texture() { return nullptr; }
IDirect3DSurface9* Surface() { return nullptr; }
bool InternalPass() { return false; }
void Request(bool) {}
}
namespace ShaderCache {
Id Add(Desc) { return 0; }
bool PrecompileComplete() { return true; }
Result CreatePixelShader(IDirect3DDevice9*, Id, IDirect3DPixelShader9**, std::string*) { return Result::CreateFailed; }
}
namespace CrashReport { void ThreadStart() {} }
namespace Overlay { bool IsVisible() { return true; } float TakeWheel() { return 0.0f; } }
namespace HookGuard { void Note(const char*) noexcept {} }
namespace GameAddr { uintptr_t Get(Id) { return 0; } }
std::wstring g_root = std::filesystem::temp_directory_path().wstring() + L"apex-shaders-menu-test\\";
namespace ApexPaths {
const std::wstring& ApexDirectory() { return g_root; }
const std::wstring& GameDocumentsDirectory() { static const std::wstring d = LR"(C:\Example\Documents\Electronic Arts\The Sims 3\)"; return d; }
bool EnsureApexDirectory() { return true; }
}

ApexConfig::UiSettings g_ui;
namespace ApexConfig {
UiSettings GetUi() { return g_ui; }
void SetUi(const UiSettings& u) { g_ui = u; }
bool ReadRoot(toml::table&) { return true; }
void RequestSave() {}
bool ParseKeyChord(const std::string&, KeyChord&) { return false; }
std::string KeyChordText(const KeyChord& k) { return std::string(k.ctrl ? "Ctrl+" : "") + (k.shift ? "Shift+" : "") + (k.alt ? "Alt+" : "") + (k.vk == VK_MENU ? std::string("Alt") : "F" + std::to_string(k.vk - VK_F1 + 1)); }
}
