// ReShade effects (Apex Radiance): ReShade's .fx effects run by Apex itself (no ReShade install) on the finished scene,
// after Color and before the game draws its interface, with Apex's scene depth. The menu (Color > ReShade) switches and
// adjusts the effects of the current preset, picks the preset and adds effect files or shader packs to
// Apex Radiance\FX\. The engine lives in features/reshade_fx.cpp; docs/features/reshade.md.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "reshade_patch.h"
#include "patch_base.h"
#include "apex_version.h"
#include "apex_log.h"
#include "apex_util.h"
#include "d3d9_hooks.h"
#include "hook_guard.h"
#include "post_scene.h"
#include "render_callbacks.h"
#include "reshade_fx.h"
#include "imgui.h"
#include "ui/i18n.h"
#include "ui/violet_theme.h"
#include "ui/widgets.h"
#include <windows.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <format>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace {

constexpr const char* kHookName = "ReShade";
constexpr const char* kPresetKey = "preset";

struct State {
    bool active = false;
    // GPU cost of render_effects, timestamp queries (render thread)
    static constexpr int kQ = 4;
    IDirect3DQuery9 *qDisjoint[kQ] = {}, *qBegin[kQ] = {}, *qEnd[kQ] = {}, *qFreq[kQ] = {};
    bool qIssued[kQ] = {};
    bool qCreated = false;
    int qNext = 0;
    float gpuMs = -1.0f;
};
State g;

template <typename T> void SafeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

void ReleaseQueries() {
    for (int i = 0; i < State::kQ; i++) {
        SafeRelease(g.qDisjoint[i]);
        SafeRelease(g.qBegin[i]);
        SafeRelease(g.qEnd[i]);
        SafeRelease(g.qFreq[i]);
        g.qIssued[i] = false;
    }
    g.qCreated = false;
    g.gpuMs = -1.0f;
}

void ReadTimings() {
    for (int i = 0; i < State::kQ; i++) {
        if (!g.qIssued[i]) continue;
        BOOL disjoint = TRUE;
        UINT64 t0 = 0, t1 = 0, freq = 0;
        if (g.qDisjoint[i]->GetData(&disjoint, sizeof disjoint, 0) != S_OK || g.qBegin[i]->GetData(&t0, sizeof t0, 0) != S_OK ||
            g.qEnd[i]->GetData(&t1, sizeof t1, 0) != S_OK || g.qFreq[i]->GetData(&freq, sizeof freq, 0) != S_OK)
            continue;
        g.qIssued[i] = false;
        if (!disjoint && freq && t1 > t0) {
            const float ms = static_cast<float>(double(t1 - t0) * 1000.0 / double(freq));
            g.gpuMs = g.gpuMs < 0 ? ms : g.gpuMs * 0.9f + ms * 0.1f;
        }
    }
}

// PostScene effect (order kReShade): after Color, before the game's interface
void ReShadeEffect(IDirect3DDevice9* dev) {
    if (!g.active) return;
    if (!g.qCreated) {
        g.qCreated = true;
        for (int i = 0; i < State::kQ; i++) {
            dev->CreateQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT, &g.qDisjoint[i]);
            dev->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &g.qBegin[i]);
            dev->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &g.qEnd[i]);
            dev->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ, &g.qFreq[i]);
        }
    }
    ReadTimings();
    const int qi = g.qNext;
    const bool timed = !g.qIssued[qi] && g.qDisjoint[qi] && g.qBegin[qi] && g.qEnd[qi] && g.qFreq[qi];
    if (timed) {
        g.qDisjoint[qi]->Issue(D3DISSUE_BEGIN);
        g.qBegin[qi]->Issue(D3DISSUE_END);
    }
    ReShadeFx::Render(dev);
    if (timed) {
        g.qEnd[qi]->Issue(D3DISSUE_END);
        g.qFreq[qi]->Issue(D3DISSUE_END);
        g.qDisjoint[qi]->Issue(D3DISSUE_END);
        g.qIssued[qi] = true;
        g.qNext = (qi + 1) % State::kQ;
    }
}

void OnPreReset(IDirect3DDevice9*) {
    if (!g.active) return;
    ReShadeFx::BeforeReset();
    ReleaseQueries();
}

void RestartEffects(IDirect3DDevice9* dev) { OnPreReset(dev); }

// ---- Add effects: a multi-select file dialog on its own thread (COM; the menu frame must not pump the game window's
// messages), the files copied there too; the render thread then reloads the effects (TakeAddResult) ----
struct AddResult {
    int added = 0;
    std::string error;
    bool cancelled = false;
};
std::mutex g_addLock;
std::optional<AddResult> g_addResult;
std::atomic<bool> g_addBusy{false};

void PickAndAddFiles() {
    if (g_addBusy.exchange(true)) return;
    const std::wstring label = ApexUtil::ToWide(I18n::Tr("FX effects"));
    if (!HookGuard::StartDetached("ReShade: add effects", [label] {
            AddResult result;
            result.cancelled = true;
            std::vector<std::wstring> paths;
            const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
            if (SUCCEEDED(com)) {
                IFileOpenDialog* dialog = nullptr;
                if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
                    const COMDLG_FILTERSPEC types[] = {{label.c_str(), L"*.zip;*.fx;*.fxh;*.ini;*.png;*.jpg;*.jpeg;*.bmp;*.dds;*.tga"}};
                    dialog->SetFileTypes(1, types);
                    DWORD options = 0;
                    dialog->GetOptions(&options);
                    dialog->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR | FOS_FILEMUSTEXIST | FOS_ALLOWMULTISELECT);
                    if (SUCCEEDED(dialog->Show(nullptr))) {
                        IShellItemArray* items = nullptr;
                        if (SUCCEEDED(dialog->GetResults(&items))) {
                            DWORD count = 0;
                            items->GetCount(&count);
                            for (DWORD i = 0; i < count; i++) {
                                IShellItem* item = nullptr;
                                if (FAILED(items->GetItemAt(i, &item))) continue;
                                PWSTR path = nullptr;
                                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                                    paths.emplace_back(path);
                                    CoTaskMemFree(path);
                                }
                                item->Release();
                            }
                            items->Release();
                        }
                    }
                    dialog->Release();
                }
                CoUninitialize();
            }
            if (!paths.empty()) {
                result.cancelled = false;
                result.added = ReShadeFx::AddFiles(paths, result.error);
            }
            std::lock_guard<std::mutex> lock(g_addLock);
            g_addResult = std::move(result);
            g_addBusy = false;
        }))
        g_addBusy = false;
}

void OpenInShell(const std::wstring& target, bool createFolder) {
    if (createFolder) CreateDirectoryW(target.c_str(), nullptr);
    HookGuard::StartDetached("ReShade: open", [target] {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        ShellExecuteW(nullptr, L"open", target.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        if (SUCCEEDED(com)) CoUninitialize();
    });
}

// The last Add effects result, shown in the card for a few seconds (render thread)
std::string g_addMessage;
bool g_addMessageError = false;
double g_addMessageUntil = 0.0;

void TakeAddResult() {
    std::optional<AddResult> r;
    {
        std::lock_guard<std::mutex> lock(g_addLock);
        r = std::move(g_addResult);
        g_addResult.reset();
    }
    if (!r || r->cancelled) return;
    if (r->added > 0) ReShadeFx::Reload();
    g_addMessageError = !r->error.empty();
    g_addMessage = g_addMessageError ? I18n::Trf("Could not add {}", r->error.substr(0, r->error.find(':')))
                                     : I18n::Trf("Added {} files; loading them", r->added);
    g_addMessageUntil = ImGui::GetTime() + 8.0;
}

} // namespace

class ReShadePatch : public ApexPatch {
  public:
    ReShadePatch() : ApexPatch("ReShade", nullptr) {}

    bool Install() override {
        if (isEnabled) return true;
        lastError.clear();
        using namespace D3D9Hooks;
        RegisterPresent(kHookName, [](DeviceContext& ctx, const RECT*, const RECT*, HWND, const RGNDATA*) {
            ReShadeFx::AtPresent(ctx.device);
            return HookAction::Continue;
        }, Priority::Last);
        RenderCallbacks::Add(RenderCallbacks::preReset, OnPreReset);
        RenderCallbacks::Add(RenderCallbacks::restartEffects, RestartEffects);
        PostScene::Add(PostScene::kReShade, ReShadeEffect, false);
        ReShadeFx::Start();
        g.active = true;
        isEnabled = true;
        LOG_INFO("[ReShade] Installed");
        return true;
    }

    bool Uninstall() override {
        if (!isEnabled) return true;
        lastError.clear();
        PostScene::Remove(ReShadeEffect);
        D3D9Hooks::UnregisterAll(kHookName);
        RenderCallbacks::Remove(RenderCallbacks::preReset, OnPreReset);
        RenderCallbacks::Remove(RenderCallbacks::restartEffects, RestartEffects);
        ReShadeFx::Stop();
        ReleaseQueries();
        g.active = false;
        isEnabled = false;
        LOG_INFO("[ReShade] Uninstalled");
        return true;
    }

    // Settings are read every frame
    void Update() override { pendingReinstall = false; }

    // The current preset's file name (the effects and their values live in that .ini, ReShade's format)
    void SaveToToml(toml::table& table) const override {
        ApexPatch::SaveToToml(table);
        table.insert_or_assign(kPresetKey, ReShadeFx::CurrentPreset());
    }
    bool LoadFromToml(const toml::table& table) override {
        const bool ok = ApexPatch::LoadFromToml(table);
        if (const auto name = table[kPresetKey].value<std::string>()) ReShadeFx::SetPreset(*name);
        return ok;
    }
    void ApplyTableLive(const toml::table& table) override {
        ApexPatch::ApplyTableLive(table);
        if (const auto name = table[kPresetKey].value<std::string>(); name && *name != ReShadeFx::CurrentPreset()) ReShadeFx::SetPreset(*name);
    }

    float GpuCostMs() const override { return (isEnabled.load() && g.gpuMs >= 0.0f) ? g.gpuMs : -1.0f; }
    const char* OverviewSummary() const override {
        switch (ReShadeFx::GetState()) {
        case ReShadeFx::State::Empty: return "No effects yet";
        case ReShadeFx::State::Loading: return "Loading effects";
        default: return nullptr;
        }
    }

    // The FX effects card's body (Color > FX): state, preset and Add effects
    void RenderCustomUI() override {
        SAFE_IMGUI_BEGIN();
        using ApexUi::IconId;
        TakeAddResult();
        switch (ReShadeFx::GetState()) {
        case ReShadeFx::State::Off: break;
        case ReShadeFx::State::Empty:
            ApexUi::IconNote(IconId::Info, "Add effects to start: .fx files or a .zip shader pack");
            break;
        case ReShadeFx::State::Loading: {
            const int n = ReShadeFx::EffectsLoading();
            ApexUi::IconNote(IconId::Info, n > 0 ? I18n::Trf("Loading {} effects…", n).c_str() : I18n::Tr("Loading effects…"));
            break;
        }
        case ReShadeFx::State::Ready: {
            const auto techniques = ReShadeFx::Techniques();
            int on = 0;
            for (const auto& t : techniques) on += t.enabled ? 1 : 0;
            ApexUi::IconNote(IconId::CircleCheck, I18n::Trf("{} of {} effects on", on, techniques.size()).c_str(), VioletTheme::kSuccess);
            break;
        }
        }
        ApexUi::Gap(ApexUi::kSpace1);

        // Preset: the .ini files in FX\Presets (ReShade's own format)
        {
            const auto presets = ReShadeFx::PresetFiles();
            const std::string current = ReShadeFx::CurrentPreset();
            int selected = -1;
            std::vector<const char*> labels;
            for (size_t i = 0; i < presets.size(); i++) {
                labels.push_back(presets[i].c_str());
                if (_stricmp(presets[i].c_str(), current.c_str()) == 0) selected = static_cast<int>(i);
            }
            if (!labels.empty()) {
                const ApexUi::ControlSizeScope size(ApexUi::ControlSize::Compact);
                int choice = std::max(selected, 0);
                if (ApexUi::SelectRow("Effects preset", "Which effects are on and how they're set", "##ReShadePreset", &choice,
                                      labels.data(), static_cast<int>(labels.size())) &&
                    choice != selected) {
                    ReShadeFx::SetPreset(presets[static_cast<size_t>(choice)]);
                    NotifySettingChanged();
                }
            }
        }

        // Add effects: files or a shader pack
        {
            const char* addLabel = "Add effects";
            const char* folderLabel = "Open the folder";
            const float gap = ApexUi::kSpace3 * ApexUi::Unit();
            const float w = ApexUi::ButtonWidth(addLabel, true) + gap + ApexUi::ButtonWidth(folderLabel, true);
            const ApexUi::ControlSizeScope size(ApexUi::ControlSize::Compact);
            if (ApexUi::BeginControlRow("Your effects", "Effect files (.fx), textures, presets (.ini) or a .zip pack", w)) {
                ImGui::BeginDisabled(g_addBusy.load());
                if (ApexUi::IconTextButton(addLabel, IconId::Plus, "Copies them to Apex Radiance\\FX and loads them", ApexUi::ButtonKind::Primary))
                    PickAndAddFiles();
                ImGui::EndDisabled();
                ImGui::SameLine(0.0f, gap);
                if (ApexUi::IconTextButton(folderLabel, IconId::ExternalLink, "Opens Apex Radiance\\FX in Explorer (created if needed)"))
                    OpenInShell(ReShadeFx::EffectsFolder(), true);
                ApexUi::EndControlRow();
            }
            if (!g_addMessage.empty() && ImGui::GetTime() < g_addMessageUntil)
                ApexUi::IconNote(g_addMessageError ? IconId::TriangleAlert : IconId::CircleCheck, g_addMessage.c_str(),
                                 g_addMessageError ? VioletTheme::kWarning : VioletTheme::kSuccess);
        }
    }

    void RenderDeveloperUI() override {
        SAFE_IMGUI_BEGIN();
        static const char* const kStates[] = {"Off", "Empty", "Loading", "Ready"};
        ImGui::TextDisabled("State: %s; %d effect files, %d loading", kStates[static_cast<int>(ReShadeFx::GetState())],
                            static_cast<int>(ReShadeFx::Effects().size()), ReShadeFx::EffectsLoading());
        ImGui::TextDisabled("Rendered before the interface: %d frames in the last second", ReShadeFx::FramesRenderedLastSecond());
        ImGui::TextDisabled("Scene depth requested: %s", ReShadeFx::WantsDepth() ? "yes" : "no");
        if (g.gpuMs >= 0) ImGui::TextDisabled("GPU cost: %.2f ms per frame", g.gpuMs);
    }
};

APEX_REGISTER_FEATURE(ReShadePatch, {.displayName = "FX effects",
                                     .description = "Runs shader effects from .fx files on the game's picture, with nothing else to install, while menus, plumbobs "
                                                    "and text keep their colors; effects that blur by distance see exactly how far things are. Part of " APEX_PRODUCT_NAME
                                                    ". Credits: @loinyx",
                                     .category = "Graphics",
                                     .experimental = true,
                                     .enabledByDefault = true,
                                     .supportedVersions = VERSION_ALL,
                                     .technicalDetails = {"Embeds the ReShade FX compiler (BSD-3); .fx files become shader model 3 code, compiled with D3DCompile on a worker thread and cached.",
                                                          "Runs the enabled techniques at the PostScene boundary (after Color), so effects never land on the interface.",
                                                          "COLOR is a copy of the back buffer; DEPTH is Apex's INTZ scene depth, requested only while an enabled effect reads it.",
                                                          "Effects, textures and presets live in Apex Radiance\\FX\\{Shaders,Textures,Presets}; presets use the ReShade .ini format."}})

// ---- Color > FX: one panel per effect file, the filter rows' look (see Picture's Filters tab) ----
namespace ReShadeUi {

void EffectPanels() {
    using ApexUi::IconId;
    ApexPatch* patch = PatchManager::Get().Find("ReShade");
    if (!patch || !patch->IsEnabled()) return;
    const auto state = ReShadeFx::GetState();
    if (state == ReShadeFx::State::Off || state == ReShadeFx::State::Empty) return;
    if (ApexUi::FilterActive()) return; // effect names and settings come from the shaders: not part of the menu's search
    const auto techniques = ReShadeFx::Techniques();
    const auto files = ReShadeFx::Effects();
    // effect files in render order (the first technique of each), then the ones without a technique to show (failed, loading)
    std::vector<std::string> effects;
    for (const auto& t : techniques)
        if (std::find(effects.begin(), effects.end(), t.effect) == effects.end()) effects.push_back(t.effect);
    for (const auto& f : files)
        if ((!f.error.empty() || f.loading) && std::find(effects.begin(), effects.end(), f.file) == effects.end()) effects.push_back(f.file);
    if (effects.empty()) {
        ApexUi::IconNote(IconId::Info, state == ReShadeFx::State::Loading ? "Loading effects…" : "These files have no effects to show");
        return;
    }

    const float u = ApexUi::Unit(), gap = ApexUi::kSpace2 * u;
    const ImVec2 switchSize = ApexUi::ToggleSwitchSize();
    const float controlsW = 24.0f * u + switchSize.x + gap; // disclosure + switch, as the filter rows
    for (const std::string& effect : effects) {
        const auto info = std::find_if(files.begin(), files.end(), [&](const ReShadeFx::EffectInfo& f) { return f.file == effect; });
        ImGui::PushID(effect.c_str());
        ApexUi::SectionLabel(effect.c_str()); // a file name: shown as it is (no translation entry)
        const bool visible = ApexUi::BeginCard("##Effect");
        if (visible) {
            if (info != files.end() && !info->error.empty()) {
                ApexUi::IconNote(IconId::TriangleAlert, I18n::Trf("This effect could not load: {}", info->error).c_str(), VioletTheme::kWarning);
                if (ApexUi::IconTextButton("Remove this effect", IconId::Trash2, "Deletes this effect's file from Apex Radiance\\FX"))
                    ReShadeFx::RemoveEffect(effect);
            } else if (info != files.end() && info->loading && std::none_of(techniques.begin(), techniques.end(),
                                                                             [&](const ReShadeFx::Technique& t) { return t.effect == effect; })) {
                ApexUi::MutedText("Loading…");
            }
            bool first = true;
            for (const auto& t : techniques) {
                if (t.effect != effect) continue;
                ImGui::PushID(t.name.c_str()); // by name: the disclosure stays open across reloads
                ApexUi::SetNextRowUntranslated();
                if (ApexUi::BeginControlRow(t.name.c_str(), t.tooltip.empty() ? nullptr : t.tooltip.c_str(), controlsW, IconId::None, 30.0f * u)) {
                    const ImGuiID openId = ImGui::GetID("Expanded");
                    bool expanded = first && ImGui::GetStateStorage()->GetBool(openId, false);
                    const float centerY = ImGui::GetCursorScreenPos().y + 15.0f * u;
                    auto center = [&](float h) { ImGui::SetCursorPosY(centerY - ImGui::GetWindowPos().y + ImGui::GetScrollY() - h * 0.5f); };
                    center(24 * u);
                    if (first) {
                        if (ApexUi::IconButton("##Adjust", expanded ? IconId::ChevronUp : IconId::ChevronDown, "Adjust this effect", expanded)) {
                            expanded = !expanded;
                            ImGui::GetStateStorage()->SetBool(openId, expanded);
                        }
                    } else {
                        ImGui::Dummy(ImVec2(24.0f * u, 24.0f * u));
                    }
                    ImGui::SameLine(0, gap);
                    center(switchSize.y);
                    bool on = t.enabled;
                    if (ApexUi::ToggleSwitch("##On", &on)) {
                        ReShadeFx::SetTechnique(t.key, on);
                        ApexUi::ReportChange(on ? "Effect turned on" : "Effect turned off");
                    }
                    ApexUi::EndControlRow();
                    if (expanded) {
                        ApexUi::Gap(ApexUi::kSpace3);
                        EffectSettings(effect, true);
                    }
                }
                first = false;
                ImGui::PopID();
            }
        }
        ApexUi::EndCard();
        ImGui::PopID();
    }
}

void EffectSettings(const std::string& effect, bool removable) {
    using ApexUi::IconId;
    auto uniforms = ReShadeFx::Uniforms(effect);
    if (uniforms.empty()) ApexUi::MutedText("This effect has no settings");
    std::string category;
    bool committed = false;
    for (auto& v : uniforms) {
        if (v.category != category) {
            category = v.category;
            if (!category.empty()) ApexUi::GroupLabel(category.c_str());
        }
        ImGui::PushID(static_cast<int>(&v - uniforms.data()));
        const char* desc = v.tooltip.empty() ? nullptr : v.tooltip.c_str();
        bool edited = false;
        switch (v.kind) {
        case ReShadeFx::UniformKind::Bool: {
            bool b = v.value[0] != 0.0f;
            ApexUi::SetNextRowUntranslated();
            if (ApexUi::SwitchRow(v.label.c_str(), &b, desc)) {
                v.value[0] = b ? 1.0f : 0.0f;
                edited = committed = true;
            }
            break;
        }
        case ReShadeFx::UniformKind::Combo: {
            std::vector<const char*> items;
            for (const auto& item : v.items) items.push_back(item.c_str());
            int current = std::clamp(static_cast<int>(std::lround(v.value[0])), 0, static_cast<int>(items.size()) - 1);
            const ApexUi::ControlSizeScope size(ApexUi::ControlSize::Compact);
            ApexUi::SetNextRowUntranslated();
            if (ApexUi::SelectRow(v.label.c_str(), desc, "##Choice", &current, items.data(), static_cast<int>(items.size()))) {
                v.value[0] = static_cast<float>(current);
                edited = committed = true;
            }
            break;
        }
        case ReShadeFx::UniformKind::Color: {
            const float w = 120.0f * ApexUi::Unit();
            ApexUi::SetNextRowUntranslated();
            if (ApexUi::BeginControlRow(v.label.c_str(), desc, w)) {
                ImGui::SetNextItemWidth(w);
                const ImGuiColorEditFlags flags = ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_Float;
                if (v.components >= 4 ? ImGui::ColorEdit4("##Color", v.value, flags) : ImGui::ColorEdit3("##Color", v.value, flags)) edited = true;
                committed |= ImGui::IsItemDeactivatedAfterEdit();
                ApexUi::EndControlRow();
            }
            break;
        }
        default: { // Int and Float, one slider per component
            static const char* const kAxis[] = {" X", " Y", " Z", " W"};
            float lo = v.min, hi = v.max;
            for (int c = 0; c < v.components; c++) {
                if (v.drag) { // no fixed range: keep the value inside the track
                    lo = std::min(lo, v.value[c]);
                    hi = std::max(hi, v.value[c] == 0.0f ? 1.0f : v.value[c] * 2.0f);
                }
            }
            if (hi <= lo) hi = lo + 1.0f;
            for (int c = 0; c < v.components; c++) {
                ImGui::PushID(c);
                const std::string label = v.components > 1 ? v.label + kAxis[c] : v.label;
                ApexUi::SliderOptions o;
                o.tooltip = desc;
                const bool isInt = v.kind == ReShadeFx::UniformKind::Int;
                o.format = isInt ? "%.0f" : (v.step >= 0.1f || hi - lo >= 10.0f ? "%.1f" : (v.step >= 0.01f || hi - lo >= 1.0f ? "%.2f" : "%.3f"));
                ApexUi::SetNextRowUntranslated();
                if (ApexUi::Slider(label.c_str(), &v.value[c], lo, hi, o)) {
                    if (isInt) v.value[c] = std::round(v.value[c]);
                    else if (v.step > 0.0f) v.value[c] = lo + std::round((v.value[c] - lo) / v.step) * v.step;
                    edited = true;
                }
                committed |= ApexUi::SliderCommitted();
                ImGui::PopID();
            }
            break;
        }
        }
        if (edited) ReShadeFx::SetUniform(v);
        ImGui::PopID();
    }
    ApexUi::Gap(ApexUi::kSpace2);
    if (!uniforms.empty() && ApexUi::IconTextButton("Reset this effect", IconId::RotateCcw, "Puts this effect's settings back to their defaults")) {
        for (const auto& v : uniforms) ReShadeFx::ResetUniform(v);
        committed = true;
        ApexUi::ReportChange("Effect reset");
    }
    if (removable) {
        if (!uniforms.empty()) ImGui::SameLine(0.0f, ApexUi::kSpace3 * ApexUi::Unit());
        if (ApexUi::IconTextButton("Remove this effect", IconId::Trash2, "Deletes this effect's file from Apex Radiance\\FX"))
            ReShadeFx::RemoveEffect(effect);
    }
    if (committed) ReShadeFx::SavePreset();
}

} // namespace ReShadeUi
