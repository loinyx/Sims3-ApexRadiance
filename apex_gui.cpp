#include "developer_settings.h"
// The Apex menu (see apex_gui.h).
#include "apex_gui.h"
#include "apex_config.h"
#include "apex_presets.h"
#include "apex_version.h"
#include "apex_log.h"
#include "apex_paths.h"
#include "apex_util.h"
#include "build_flavor.h"
#include "d3d9_bootstrap.h"
#include "frame_profiler.h"
#include "game_version.h"
#include "game_addresses.h"
#include "world_session.h"
#include "night_lighting.h"
#include "patch_base.h"
#include "performance.h"
#include "picture.h"
#include "recorder.h"
#include "captures.h"
#include "light_diag.h"
#include "light_probe.h"
#include "hotkeys.h"
#include "s3ss_detect.h"
#include "unlit_rooms.h"
#include "shader_cache.h"
#include "sim_occlusion.h"
#include "ui/i18n.h"
#include "ui/logo.h"
#include "ui/violet_theme.h"
#include "ui/widgets.h"
#include "imgui.h"
#include "imgui_internal.h"
#include <toml++/toml.hpp>
#include <objbase.h>
#include <shellapi.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cfloat>
#include <cstdio>
#include <cstring>
#include <format>
#include <mutex>
#include <string>
#include <thread>
#include <cctype>
#include <fstream>
#include <iterator>
#include <vector>

#pragma comment(lib, "ole32.lib") // CoInitializeEx for ShellExecuteW (shell32.lib is linked by apex_paths.cpp)

namespace ApexGui {
namespace {

using ApexUi::ButtonKind;
using ApexUi::IconId;
using VioletTheme::Col;

std::atomic<Startup> g_startup{Startup::Loading};
std::mutex g_detailLock;
std::string g_startupDetail;
std::atomic<bool> g_oldStandalone{false}; // an older S3SSApex.asi is loaded too (idle): banner
std::string g_oldStandaloneModule;        // under g_detailLock

// Sidebar pages and the tabs of each page. The selected page and tabs are kept while the game runs (not saved).
enum Page : int { PageOverview, PageLighting, PageWaterSnow, PageColor, PageAmbientOcclusion, PageDepthBlur, PageEdgeSmoothing, PagePerformance, PageDeveloper, PageSettings, PageReport, PageConflicts };
enum LightingTab : int { LightingLamps, LightingGround, LightingObjects, LightingBuildings, LightingStories };
enum SettingsTab : int { SettingsMenu, SettingsShortcuts, SettingsProfiles, SettingsCompatibility, SettingsAbout };
int g_page = PageOverview;
int g_lightingTab = LightingLamps;
int g_colorTab = Picture::TabBasic;
constexpr int kColorBandingTab = Picture::TabCount; // Color > Banding: the Banding Fix and Smooth gradients
int g_settingsTab = SettingsMenu;

// ---- menu state (render thread, inside the overlay's ImGui frame) ----
char g_search[96] = {};           // the header's search query; non-empty = the content shows the results
bool g_focusSearch = false;       // Ctrl+F: focus the search field this frame
bool g_menuHovered = false;       // the mouse is over the menu window (last frame)
bool g_menuFocused = false;       // the menu window has keyboard focus (last frame)
ImVec2 g_windowMin{}, g_windowMax{}; // the menu window's rectangle (last frame)
std::atomic<bool> g_keysOverMenu{false}; // Alt / B are the menu's (Client::CaptureKey, window thread)
std::atomic<bool> g_menuTextInput{false}; // bare screenshot letters must remain typeable in menu text fields
float g_alpha = 1.0f;             // the menu's opacity (peek, slider drag fade)
bool g_waitingForKey = false;     // Settings > Menu: waiting for a new menu key (Esc cancels it, not the menu)
bool g_holdCompare = false;       // the hold-to-compare button is held this frame
int g_recRow = -1;                // Shortcuts: the row whose key is being recorded (-1 = none)
void ShortcutsContent();
void ShortcutsTab();
void LanguageRow();
void ScreenshotCaptureCard();

// Undo: the feature state at the last click / key activation in the menu (before any widget saw it), and the toast
toml::table g_clickSnapshot;
bool g_haveClickSnapshot = false;
struct Toast {
    bool active = false;
    std::string text;    // as shown
    std::string logText; // English, for the log
    toml::table undo; // the state Undo restores
    bool restoreUi = false;
    ApexConfig::UiSettings undoUi;
    double start = 0.0;
};
Toast g_toast;
constexpr double kToastSeconds = 4.0;

void ShowToast(const std::string& text, toml::table undo, const std::string& logText = {}) {
    g_toast.active = true;
    g_toast.restoreUi = false;
    g_toast.text = text;
    g_toast.logText = logText.empty() ? text : logText;
    g_toast.undo = std::move(undo);
    g_toast.start = ImGui::GetTime();
}

// Opens a page (and one of its tabs)
void Go(int page, int* tabOfPage = nullptr, int tab = 0) {
    g_page = page;
    if (tabOfPage) *tabOfPage = tab;
}

// Descriptions (hover) of the cards that are not ApexPatch features (the patches carry theirs in their metadata)
constexpr const char* kPictureDescription = "Fine-tune how the world looks: brightness, contrast, color, sharpness and smoother skies, plus film-style "
                                            "tones and a vignette. Menus and text keep their normal look. Part of " APEX_PRODUCT_NAME ". Credits: @loinyx";
constexpr const char* kShoreDescription = "Ponds and lakes mirror the trees, houses and lamps along their shore, on top of the game's sky reflection. "
                                          "Needs Night Lights and Depth Blur, with the game's own Edge Smoothing off. Part of " APEX_PRODUCT_NAME ". Credits: @loinyx";
constexpr const char* kProfilerDescription = "Measures every frame and breaks down each hitch. Writes ApexRadiance_Hitches.txt. Development build only. Part of "
                                             APEX_PRODUCT_NAME ". Credits: @loinyx";

constexpr const char* kNightLighting = "NightTerrainRelight";
constexpr const char* kUpperFloors = "SplitLevelGroundLight";

// ---- feature state ----

ApexPatch* Find(const char* name) { return PatchManager::Get().Find(name); }

const char* Description(const ApexPatch* patch) {
    const FeatureInfo* meta = patch ? patch->GetMetadata() : nullptr;
    return meta ? meta->description.c_str() : nullptr;
}

bool Loading() { return g_startup.load() != Startup::Running; }

// Switches are inert while features start and on an unsupported game version
bool Switchable(const ApexPatch* patch) { return patch && patch->IsCompatibleWithCurrentVersion() && !Loading(); }

void SetPatch(ApexPatch* patch, bool on) {
    if (on ? patch->Install() : patch->Uninstall()) PatchManager::Get().SetUnsavedChanges(true);
}

void CardNote(const char* text) { ApexUi::IconNote(IconId::Info, text); }

void CardError(const std::string& error) {
    if (error.empty()) return;
    ApexUi::IconNote(IconId::TriangleAlert, I18n::Trf("Error: {}", error).c_str(), VioletTheme::kError);
}

void NotAvailableNote(const ApexPatch* patch) { CardNote(patch->UnavailableReason().c_str()); }

// Whether StateNotes draws anything
bool HasStateNotes(const ApexPatch* patch) { return !patch->IsCompatibleWithCurrentVersion() || Loading() || !patch->GetLastError().empty(); }

// "Not available on <version>", "StartingÃ¢â‚¬Â¦" and the red error of a feature
void StateNotes(const ApexPatch* patch) {
    if (!patch->IsCompatibleWithCurrentVersion()) NotAvailableNote(patch);
    else if (Loading()) CardNote("Starting\xE2\x80\xA6");
    CardError(patch->GetLastError());
}

// The GPU cost chip of a feature ("~0.4 ms"); nullptr while it is off or not measured
template <int N> const char* CostChip(float ms, char (&buf)[N]) { return ApexUi::CostChipText(ms, buf, N) ? buf : nullptr; }

// One feature card: header (icon, title, subtitle; the description, which ends with the credit, on hover) with its
// on/off switch and its GPU cost chip, then (below a divider) its state notes and body(patch) while it is on. In the
// search results the body is searched even while the feature is off.
template <typename Body> void FeatureCardWith(const char* patchName, IconId icon, const char* title, const char* subtitle, Body&& body, bool showDivider = true) {
    ApexPatch* patch = Find(patchName);
    if (!patch) return;
    ImGui::PushID(patchName);
    if (ApexUi::BeginCard("##Card")) {
        bool on = patch->IsEnabled();
        char chipBuf[24];
        const char* chip = on ? CostChip(patch->GpuCostMs(), chipBuf) : nullptr;
        if (ApexUi::CardHeader(icon, title, subtitle, Description(patch), &on, Switchable(patch), nullptr, chip)) SetPatch(patch, on);
        const bool enabled = patch->IsEnabled();
        if ((enabled && showDivider) || HasStateNotes(patch)) ApexUi::CardDivider();
        StateNotes(patch);
        if (enabled || ApexUi::FilterActive()) body(patch);
    }
    ApexUi::EndCard();
    ImGui::PopID();
}

// The same with the feature's own controls (RenderCustomUI)
void FeatureCard(const char* patchName, IconId icon, const char* title, const char* subtitle) {
    FeatureCardWith(patchName, icon, title, subtitle, [](ApexPatch* p) { p->RenderCustomUI(); });
}

// A primary "Turn on <feature>" button (inert while the feature cannot be switched); the change can be undone
void TurnOnButton(ApexPatch* patch, const char* label, IconId icon, const char* changeText) {
    ImGui::BeginDisabled(!Switchable(patch));
    if (ApexUi::IconTextButton(label, icon, nullptr, ButtonKind::Primary)) {
        SetPatch(patch, true);
        ApexUi::ReportChange(changeText); // the undo toast shows it translated, the log in English
    }
    ImGui::EndDisabled();
}

// Tabs whose options need Night Lights: while it is off, a note and the button to turn it on. True when it is on (and
// always in the search results, where the options are searched even while it is off).
bool NightLightsReady(const char* what) {
    ApexPatch* ntr = Find(kNightLighting);
    if (!ntr) return false;
    if (ntr->IsEnabled() || ApexUi::FilterActive()) return true;
    if (!ntr->IsCompatibleWithCurrentVersion()) {
        NotAvailableNote(ntr);
        return false;
    }
    CardNote(I18n::Trf("Turn on Night Lights to adjust {}", I18n::Tr(what)).c_str());
    TurnOnButton(ntr, "Turn on Night Lights", IconId::MoonStar, "Night Lights turned on");
    return false;
}

// ---- Water Reflections (the shore reflection of Night Lights' lake pass, reflexoNoLago) ----

float g_lastShore = 1.0f; // strength restored when the switch goes back on
constexpr float kShoreDefault = 1.0f; // reflexoNoLago's registered default

bool ShoreOn() { return NightLighting::ShoreReflection() > 0.0f; }

void SetShore(bool on) {
    if (on) {
        NightLighting::SetShoreReflection(g_lastShore > 0.0f ? g_lastShore : kShoreDefault);
    } else {
        const float v = NightLighting::ShoreReflection();
        if (v > 0.0f) g_lastShore = v;
        NightLighting::SetShoreReflection(0.0f);
    }
}

// ---- recommendations: DXVK and official Sims3SettingsSetter (user, 30/09: "DXVK is recommended for the mod to work well;
// suggest both at the start when missing; when installed, only Settings shows them") ----

constexpr const char* kRecommendText = APEX_PRODUCT_NAME " works on its own, but it pairs well with Sims3SettingsSetter by sims3fiend: a frame "
                                       "rate limiter, fewer stutters and many extra game settings.";
constexpr const char* kDxvkText = "Runs the game on Vulkan: fewer stutters, and " APEX_PRODUCT_NAME " is made and tested with it.";
constexpr const wchar_t* kS3SSReleasesUrl = L"https://github.com/sims3fiend/Sims3SettingsSetter/releases";
constexpr const wchar_t* kDxvkReleasesUrl = L"https://github.com/doitsujin/dxvk/releases";

bool S3SSMissing() {
    const S3SSDetect::Info info = S3SSDetect::Scan(); // cached after the first scan
    return info.scanned && !info.s3ssLoaded && !info.oldCombinedBuild;
}

// Whether the game draws through DXVK: the d3d9.dll it loaded is not Windows' own and names DXVK inside (checked once)
bool DxvkLoaded() {
    static int cached = -1;
    if (cached >= 0) return cached == 1;
    const HMODULE m = GetModuleHandleW(L"d3d9.dll");
    if (!m) return false; // not loaded yet: ask again later
    cached = 0;
    wchar_t path[MAX_PATH] = {}, windows[MAX_PATH] = {};
    if (!GetModuleFileNameW(m, path, MAX_PATH)) return false;
    const UINT wl = GetWindowsDirectoryW(windows, MAX_PATH);
    if (wl && _wcsnicmp(path, windows, wl) == 0) return false; // System32 / SysWOW64: Windows' own
    std::ifstream in(path, std::ios::binary);
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    for (char& c : bytes) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    cached = bytes.find("dxvk") != std::string::npos ? 1 : 0;
    LOG_INFO(std::format("[Menu] d3d9.dll: {} ({})", ApexUtil::ToUtf8(path), cached ? "DXVK" : "not DXVK"));
    return cached == 1;
}
bool DxvkMissing() { return g_startup.load() != Startup::Loading && !DxvkLoaded(); }
bool AnythingRecommended() { return S3SSMissing() || DxvkMissing(); }

// Opens a page in the default browser, on a short-lived thread (ShellExecute can take a moment and wants COM on its
// thread; the render thread must not wait for it).
void OpenPage(const wchar_t* url) {
    std::thread([url] {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        const HINSTANCE r = ShellExecuteW(nullptr, L"open", url, nullptr, nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(r) <= 32) LOG_WARNING(std::format("[Menu] Could not open {} ({})", ApexUtil::ToUtf8(url), reinterpret_cast<INT_PTR>(r)));
        if (SUCCEEDED(com)) CoUninitialize();
    }).detach();
}

void DownloadS3SSButton() {
    if (ApexUi::IconTextButton("Download##S3SS", IconId::Download, "Opens the Sims3SettingsSetter releases page on GitHub in your browser", ButtonKind::Primary))
        OpenPage(kS3SSReleasesUrl);
}
void DownloadDxvkButton() {
    if (ApexUi::IconTextButton("Download##DXVK", IconId::Download, "Opens the DXVK releases page on GitHub in your browser", ButtonKind::Primary))
        OpenPage(kDxvkReleasesUrl);
}

// The missing ones, each with its line and Download (Overview and Compatibility settings)
void RecommendedItems() {
    if (DxvkMissing()) {
        ImGui::TextUnformatted("DXVK");
        ApexUi::MutedText(kDxvkText);
        DownloadDxvkButton();
        ApexUi::Gap(ApexUi::kSpace2);
    }
    if (S3SSMissing()) {
        ImGui::TextUnformatted("Sims3SettingsSetter");
        ApexUi::MutedText(kRecommendText);
        DownloadS3SSButton();
        ApexUi::Gap(ApexUi::kSpace2);
    }
}
void DontShowRecommended() {
    ApexConfig::UiSettings ui = ApexConfig::GetUi();
    ui.recommendS3SS = false; // [ui] recommend_s3ss (both recommendations); Settings > Compatibility keeps the links
    ApexConfig::SetUi(ui);
}

void RecommendS3SSCard() {
    if (!AnythingRecommended() || !ApexConfig::GetUi().recommendS3SS) return;
    ImGui::PushID("RecommendS3SS");
    if (ApexUi::BeginCard("##Card")) {
        ApexUi::CardHeader(IconId::Puzzle, "Recommended for " APEX_PRODUCT_NAME, "For the mod to work at its best", nullptr, nullptr);
        ApexUi::CardDivider();
        RecommendedItems();
        const std::string dontShow = std::string(I18n::Tr("Don't show again")) + "###DontShowS3SS"; // the id stays English
        if (ImGui::TextLink(dontShow.c_str())) DontShowRecommended();
    }
    ApexUi::EndCard();
    ImGui::PopID();
}

// ---- Overview ----

bool g_menuGameAaOn = false;

void OverviewPatchRow(const char* patchName, IconId icon, const char* name, const char* phrase, int page, int* tabOfPage = nullptr, int tab = 0) {
    ApexPatch* patch = Find(patchName);
    if (!patch) return;
    bool on = patch->IsEnabled(), nameClicked = false;
    char chipBuf[24];
    const char* chip = CostChip(patch->GpuCostMs(), chipBuf);
    const bool blocked = on && g_menuGameAaOn &&
        (std::strcmp(patchName, "EdgeSmoothing") == 0 || std::strcmp(patchName, "DepthBlur") == 0 || std::strcmp(patchName, "AmbientOcclusion") == 0);
    const char* summary = blocked ? "Waiting for game settings" : on ? patch->OverviewSummary() : nullptr;
    if (ApexUi::OverviewRow(patchName, icon, name, phrase, Description(patch), &on, Switchable(patch), nullptr, &nameClicked, blocked ? nullptr : chip, summary, blocked)) SetPatch(patch, on);
    if (nameClicked) Go(page, tabOfPage, tab);
    CardError(patch->GetLastError());
}

constexpr const char* kOverviewPerformancePatches[] = {
    Performance::kResourceCacheName, Performance::kLookupMissesName, Performance::kFileListName,
    Performance::kRoomLightQueueName, Performance::kLotLightingName, Performance::kWallShadingName,
    Performance::kFastTextureName, Performance::kFastCacheName, Performance::kFastCasName,
    Performance::kFastMemoryName, Performance::kSceneBudgetName, Performance::kObjectIndexName,
};

constexpr const char* kOverviewMainPatches[] = {
    kNightLighting, "AmbientOcclusion", "SceneDither", "DepthBlur", "EdgeSmoothing",
};

bool PerformanceGroupEnabled() {
    for (const char* name : kOverviewPerformancePatches) {
        const ApexPatch* patch = Find(name);
        if (!patch || !patch->IsEnabled()) return false;
    }
    return true;
}

bool PerformanceGroupAnyEnabled() {
    for (const char* name : kOverviewPerformancePatches)
        if (const ApexPatch* patch = Find(name); patch && patch->IsEnabled()) return true;
    return false;
}

bool PerformanceGroupSwitchable() {
    if (Loading()) return false;
    for (const char* name : kOverviewPerformancePatches)
        if (!Switchable(Find(name))) return false;
    return true;
}

void SetPerformanceGroup(bool on) {
    if (on) {
        // Enable Faster Game File Lookups before its Remember Missing Files extension.
        for (const char* name : kOverviewPerformancePatches)
            if (ApexPatch* patch = Find(name); patch && Switchable(patch) && !patch->IsEnabled()) SetPatch(patch, true);
    } else {
        // Stop the dependent negative cache before turning off its parent.
        for (auto it = std::rbegin(kOverviewPerformancePatches); it != std::rend(kOverviewPerformancePatches); ++it)
            if (ApexPatch* patch = Find(*it); patch && Switchable(patch) && patch->IsEnabled()) SetPatch(patch, false);
    }
}

bool AllOverviewEffectsEnabled() {
    for (const char* name : kOverviewMainPatches) {
        const ApexPatch* patch = Find(name);
        if (!patch || !patch->IsEnabled()) return false;
    }
    return Picture::Get().GetParams().enabled && ShoreOn() && PerformanceGroupEnabled();
}

bool AnyOverviewEffectEnabled() {
    for (const char* name : kOverviewMainPatches)
        if (const ApexPatch* patch = Find(name); patch && patch->IsEnabled()) return true;
    return Picture::Get().GetParams().enabled || ShoreOn() || PerformanceGroupAnyEnabled();
}

bool AllOverviewEffectsSwitchable() {
    if (Loading()) return false;
    for (const char* name : kOverviewMainPatches)
        if (!Switchable(Find(name))) return false;
    return PerformanceGroupSwitchable();
}

void SetAllOverviewEffects(bool on) {
    if (!on) SetShore(false); // Disable the reflection before its Night Lights / Depth Blur requirements.
    for (const char* name : kOverviewMainPatches)
        if (ApexPatch* patch = Find(name); patch && Switchable(patch) && patch->IsEnabled() != on) SetPatch(patch, on);

    PictureParams picture = Picture::Get().GetParams();
    if (picture.enabled != on) {
        picture.enabled = on;
        Picture::Get().SetParams(picture, true);
    }
    SetPerformanceGroup(on);
    if (on) SetShore(true); // Night Lights and Depth Blur are enabled above.
}

void OverviewPage() {
    ApexUi::PageTitle("Overview", "See what is in use; click a resource to open its settings");
    RecommendS3SSCard();
    ImGui::PushID("Overview");
    if (ApexUi::BeginCard("##AllEffects")) {
        const bool anyEffectOn = AnyOverviewEffectEnabled();
        const char* state = AllOverviewEffectsEnabled() ? "All effects are on" : anyEffectOn ? "Some effects are on" : "All effects are off";
        bool allEffects = AllOverviewEffectsEnabled();
        if (ApexUi::CardHeader(IconId::ListChecks, "All effects", state, "Turn all effects on or off together", &allEffects,
                               AllOverviewEffectsSwitchable()))
            SetAllOverviewEffects(allEffects);
    }
    ApexUi::EndCard();
    ApexUi::GroupLabel("Lighting");
    if (ApexUi::BeginCard("##Lighting")) {
        bool nameClicked = false;
        OverviewPatchRow(kNightLighting, IconId::MoonStar, "Night Lights", "Lamps light up your neighborhood at night", PageLighting, &g_lightingTab, LightingLamps);
        if (ApexPatch* ntr = Find(kNightLighting)) {
            ApexPatch* blur = Find("DepthBlur");
            const char* phrase = !ntr->IsEnabled() ? "Needs Night Lights" : (blur && !blur->IsEnabled()) ? "Needs Depth Blur" : "Ponds mirror their shore";
            bool on = ShoreOn();
            if (ApexUi::OverviewRow("WaterReflections", IconId::MirrorRound, "Water Reflections", phrase, kShoreDescription, &on, !Loading(), nullptr, &nameClicked, nullptr, on && g_menuGameAaOn ? "Waiting for game settings" : nullptr, on && g_menuGameAaOn))
                SetShore(on);
            if (nameClicked) Go(PageWaterSnow);
        }
    }
    ApexUi::EndCard();
    ApexUi::GroupLabel("Image");
    if (ApexUi::BeginCard("##Image")) {
        bool nameClicked = false;
        {
            PictureParams p = Picture::Get().GetParams();
            bool on = p.enabled;
            char chipBuf[24];
            const char* chip = p.enabled ? CostChip(Picture::Get().GpuMs(), chipBuf) : nullptr;
            if (ApexUi::OverviewRow("Picture", IconId::Palette, "Picture", "Brightness, color and sharpness", kPictureDescription, &on, true, nullptr, &nameClicked, chip)) {
                p.enabled = on;
                Picture::Get().SetParams(p, true);
            }
            if (nameClicked) Go(PageColor);
        }
        OverviewPatchRow("AmbientOcclusion", IconId::Contrast, "Ambient Occlusion", "Soft shade where things meet", PageAmbientOcclusion);
        OverviewPatchRow("SceneDither", IconId::Blend, "Banding Fix", "No color steps in light and shadows", PageColor, &g_colorTab, kColorBandingTab);
        OverviewPatchRow("DepthBlur", IconId::Aperture, "Depth Blur", "Softly blurs the distant background", PageDepthBlur);
        OverviewPatchRow("EdgeSmoothing", IconId::Spline, "Edge Smoothing", "Clean, smooth edges on the world", PageEdgeSmoothing);
    }
    ApexUi::EndCard();
    ApexUi::GroupLabel("Performance");
    if (ApexUi::BeginCard("##PerformanceScreen")) {
        bool on = PerformanceGroupEnabled(), nameClicked = false;
        if (ApexUi::OverviewRow("PerformanceGroup", IconId::Gauge, "Performance", "One switch for all 12 performance options",
                                nullptr, &on, PerformanceGroupSwitchable(), nullptr, &nameClicked))
            SetPerformanceGroup(on);
        if (nameClicked) Go(PagePerformance);
    }
    ApexUi::EndCard();
    ImGui::PopID();
}


// ---- World > Lighting ----

// "Upper floors light the ground": the Every-Story Ground Light feature's own switch, first in the Stories card
void UpperFloorRow() {
    ApexPatch* patch = Find(kUpperFloors);
    if (!patch) return;
    constexpr const char* kLabel = "Upper floors light the ground";
    constexpr const char* kText = "Lamps upstairs also light the yard below";
    ImGui::PushID("UpperFloors");
    if (NightLighting::SplitLevelProvidedByS3SS()) {
        // Sims3SettingsSetter's own fix does it: shown on, not switchable here
        bool on = true;
        ImGui::BeginDisabled();
        ApexUi::SwitchRow(kLabel, &on, kText);
        ImGui::EndDisabled();
        CardNote("Already handled by Sims3SettingsSetter (its Split-Level Lighting Fix is on)");
    } else {
        bool on = patch->IsEnabled();
        ImGui::BeginDisabled(!Switchable(patch));
        if (ApexUi::SwitchRow(kLabel, &on, kText, patch->IsEnabledByDefault())) {
            SetPatch(patch, on);
            NightLighting::RefreshSoon(); // the ground light is baked again with it (any lighting setting refreshes the lighting)
        }
        ImGui::EndDisabled();
        if (!patch->IsCompatibleWithCurrentVersion()) NotAvailableNote(patch);
        CardError(patch->GetLastError());
    }
    ImGui::PopID();
}

void LampsTabContent() {
    FeatureCardWith(kNightLighting, IconId::MoonStar, "Night Lights", "Lamps light your neighborhood at night", [](ApexPatch*) {}, false);
    ApexPatch* ntr = Find(kNightLighting);
    if (!ntr) return;
    if (!ntr->IsEnabled()) {
        if (ntr->IsCompatibleWithCurrentVersion() && !Loading()) CardNote("Turn on Night Lights, then fine-tune each part in the tabs above");
        return;
    }
    if (ApexUi::BeginCard("##NightBalance")) NightLighting::DrawLightingBalance();
    ApexUi::EndCard();
    NightLighting::DrawRefreshCard();

}

void GroundTabContent() {
    if (NightLightsReady("the ground and lots")) NightLighting::DrawGroundCard();
}

void StoriesTabContent() {
    if (NightLightsReady("the stories")) NightLighting::DrawStoriesCard(&UpperFloorRow);
}

void ObjectsTabContent() {
    if (NightLightsReady("objects")) NightLighting::DrawObjectsCard();
}

void BuildingsTabContent() {
    if (!NightLightsReady("walls, roofs and rooms")) return;
    NightLighting::DrawBuildingsCard();
    NightLighting::DrawRoomsCard();
}

void LightingPage() {
    ApexUi::PageTitle("Lighting", "Warm lamp light around your lots at night");
    static const char* const kTabs[] = {"Overview", "Ground", "Objects", "Buildings", "Stories"};
    ApexUi::TabBar("##LightingTabs", &g_lightingTab, kTabs, IM_COUNTOF(kTabs));
    switch (g_lightingTab) {
    case LightingGround: GroundTabContent(); break;
    case LightingObjects: ObjectsTabContent(); break;
    case LightingBuildings: BuildingsTabContent(); break;
    case LightingStories: StoriesTabContent(); break;
    default: LampsTabContent(); break;
    }
}

// ---- World > Water & Snow ----

void GameEdgeSmoothingNote(const char* forWhat); // Image > Depth Blur, below

void WaterReflectionsCard() {
    ApexPatch* ntr = Find(kNightLighting);
    if (!ntr) return;
    ApexPatch* blur = Find("DepthBlur");
    ImGui::PushID("WaterReflections");
    if (ApexUi::BeginCard("##Card")) {
        bool on = ShoreOn();
        if (ApexUi::CardHeader(IconId::MirrorRound, "Water Reflections", "Ponds mirror trees, houses and lamps", kShoreDescription, &on, !Loading())) SetShore(on);
        // The reflection is drawn by Night Lights' water pass and reads Depth Blur's scene depth
        const bool needLights = !ntr->IsEnabled(), needBlur = blur && !blur->IsEnabled();
        if (needLights || needBlur || on) ApexUi::CardDivider();
        if (needLights) {
            CardNote("Needs Night Lights (Lighting page)");
            TurnOnButton(ntr, "Turn on Night Lights", IconId::MoonStar, "Night Lights turned on");
        }
        if (needBlur) {
            CardNote("Needs Depth Blur (Depth Blur page)");
            TurnOnButton(blur, "Turn on Depth Blur", IconId::Aperture, "Depth Blur turned on");
        }
        // The reflection reads the scene depth, which the game's own (multisampled) Edge Smoothing does not give
        if (on) GameEdgeSmoothingNote("Water Reflections");
        if (on) {
            float v = NightLighting::ShoreReflection();
            if (ApexUi::SliderPercent("Reflection brightness", &v, 0.05f, 3.0f, "How strong the reflection is; 100% is the default", kShoreDefault)) {
                NightLighting::SetShoreReflection(v);
                g_lastShore = v;
            }
        }
    }
    ApexUi::EndCard();
    ImGui::PopID();
}

void WaterTabContent() {
    // The lamp glow needs Night Lights; the Water Reflections card below says so with its own button
    ApexPatch* ntr = Find(kNightLighting);
    if (ntr && (ntr->IsEnabled() || ApexUi::FilterActive())) {
        NightLighting::DrawWaterCard();
    } else if (ntr) {
        CardNote("Lamp glow on ponds needs Night Lights");
        TurnOnButton(ntr, "Turn on Night Lights", IconId::MoonStar, "Night Lights turned on");
        ApexUi::Gap(ApexUi::kSpace2);
    }
    WaterReflectionsCard();
}

void SnowTabContent() {
    if (NightLightsReady("snow")) NightLighting::DrawSnowCard();
}

void WaterSnowContent() {
    WaterTabContent();
    if (!ApexUi::FilterActive()) ApexUi::Gap(ApexUi::kSpace3);
    SnowTabContent();
}

void WaterSnowPage() {
    ApexUi::PageTitle("Water & Snow", "Pond lighting, water reflections and sidewalk snow");
    WaterSnowContent();
}

// ---- Image > Color ----

// The Picture card above the tabs: its switch ([qol.picture] enabled, saved at once), the GPU cost chip, hold to compare
// (the original picture while held; never saved) and before / after (the left half without Picture; never saved)
void PictureHeaderCard() {
    ImGui::PushID("Picture");
    if (ApexUi::BeginCard("##Card")) {
        PictureParams p = Picture::Get().GetParams();
        bool on = p.enabled, compare = p.compare;
        ApexUi::HeaderExtra extra;
        extra.iconOff = IconId::Columns2;
        extra.iconOn = IconId::Columns2;
        extra.value = &compare;
        extra.enabled = p.enabled;
        extra.tooltip = "Before and after: the left half of the screen without Picture, the right half with it (not saved)";
        extra.holdIcon = IconId::Eye;
        extra.holdTooltip = "Hold to compare: the game without Picture while you hold it (or hold B over the menu)";
        char chipBuf[24];
        const char* chip = p.enabled ? CostChip(Picture::Get().GpuMs(), chipBuf) : nullptr;
        const bool switched = ApexUi::CardHeader(IconId::Palette, "Picture", "Brightness, contrast, color and sharpness", kPictureDescription, &on, true, &extra, chip);
        if (extra.held) g_holdCompare = true;
        if (switched || extra.clicked) {
            p.enabled = on;
            p.compare = compare;
            Picture::Get().SetParams(p, switched);
        }
        // On but not being applied (and why): also written to the log
        const std::string problem = Picture::Get().Problem(true);
        if (!problem.empty()) {
            ApexUi::Gap(ApexUi::kSpace2);
            ApexUi::IconNote(IconId::TriangleAlert, problem.c_str(), VioletTheme::kWarning);
        } else if (Picture::Get().MenusTinted()) {
            ApexUi::Gap(ApexUi::kSpace2);
            ApexUi::IconNote(IconId::Info, "Color also tints the game's menus here, because the game draws its picture in a way Apex can't split; "
                                           "turning off the game's own Edge Smoothing (Options \xE2\x80\xBA Graphics) usually fixes it");
        }
    }
    ApexUi::EndCard();
    ImGui::PopID();
}

void PictureRows(int tab) {
    ImGui::PushID("PictureRows");
    if (ApexUi::BeginCard("##Card")) Picture::Get().RenderUI(tab); // greyed out while Picture is off
    ApexUi::EndCard();
    ImGui::PopID();
}

// Color > Banding (user, 30/09: everything against color steps in one place): the Banding Fix card (scene_dither.cpp:
// switch, grain Strength) with Picture's Smooth gradients (the deband), which follows the Banding Fix's switch
void BandingTabContent() {
    ApexUi::IconNote(IconId::Info, "Still being tested: if anything looks wrong or the game crashes, turn it off");
    ApexUi::Gap(ApexUi::kSpace1);
    FeatureCardWith("SceneDither", IconId::Blend, "Banding Fix", "Smooth light, with no color steps", [](ApexPatch* p) {
        p->RenderCustomUI();
        static const PictureParams kDef{};
        PictureParams q = Picture::Get().GetParams();
        if (ApexUi::SliderPercent("Smooth gradients", &q.deband, 0.0f, 2.0f, "Softens the steps left in skies and shadows; works with Picture off too; 0% is off",
                                  kDef.deband))
            Picture::Get().SetParams(q, ApexUi::SliderCommitted());
        else if (ApexUi::SliderCommitted())
            Picture::Get().SetParams(q, true);
        ApexUi::IconNote(IconId::Info, "Smooth gradients also reaches the sky and other surfaces the grain can't");
    });
}

void ColorPage() {
    ApexUi::PageTitle("Color", "How the game's picture looks");
    static const char* const kTabs[] = {"Basic", "Tones", "Color", "Detail", "Banding"};
    static_assert(IM_COUNTOF(kTabs) == Picture::TabCount + 1, "one tab name per Picture tab, then Banding");
    ApexUi::TabBar("##ColorTabs", &g_colorTab, kTabs, IM_COUNTOF(kTabs));
    if (g_colorTab == kColorBandingTab) {
        BandingTabContent();
        return;
    }
    PictureHeaderCard();
    PictureRows(g_colorTab);
}

// Depth Blur and Edge Smoothing both need the game's own (multisampled) Edge Smoothing off
void GameEdgeSmoothingNote(const char* forWhat) {
    if (!g_menuGameAaOn) return; // Show the prerequisite only while the conflict actually exists.
    const std::string note = I18n::Trf("For {}, turn off the game's own Edge Smoothing (Options \xE2\x80\xBA Graphics)", I18n::Tr(forWhat));
    ApexUi::IconNote(IconId::Info, note.c_str());
    ApexUi::Gap(ApexUi::kSpace1);
}

// ---- Image > Ambient Occlusion ----

void AmbientOcclusionContent() {
    GameEdgeSmoothingNote("Ambient Occlusion");
    if (auto* ao = Find("AmbientOcclusion"); ao && ao->IsEnabled()) {
        ApexUi::IconNote(IconId::Gauge, "Heavier on the graphics card than other effects: lower the Quality if the game slows down");
        ApexUi::Gap(ApexUi::kSpace1);
    }
    FeatureCard("AmbientOcclusion", IconId::Contrast, "Ambient Occlusion", "Soft shade under furniture, in corners and around houses");
    ApexUi::Gap(ApexUi::kSpace2);
    SimOcclusion::RenderUI(Find("AmbientOcclusion"));
}

void AmbientOcclusionPage() {
    ApexUi::PageTitle("Ambient Occlusion", "Soft shade where things meet");
    AmbientOcclusionContent();
}

// ---- Image > Depth Blur ----

void DepthBlurContent() {
    GameEdgeSmoothingNote("Depth Blur");
    FeatureCard("DepthBlur", IconId::Aperture, "Depth Blur", "Like a camera focused on what you look at");
}

void DepthBlurPage() {
    ApexUi::PageTitle("Depth Blur", "Softly blurs the distant background");
    DepthBlurContent();
}

// ---- System > Display ----

bool GameAaBlocksEffects() {
    if (!g_menuGameAaOn) return false;
    const auto active = [](const char* name) { auto* p = Find(name); return p && p->IsEnabled(); };
    return active("EdgeSmoothing") || active("DepthBlur") || active("AmbientOcclusion") ||
        (active(kNightLighting) && NightLighting::ShoreReflection() > 0.0f);
}

// S3SS.toml's saved room colour (read-only; S3SSDetect re-reads the file at most every 3 s)
bool S3SSRoomColourSaved(bool recheck = false) { return S3SSDetect::SavedRoomAmbientOverride(recheck).has_value(); }

// The Attention page and its sidebar entry exist only while one of its items applies
bool HasAttentionItems() { return GameAaBlocksEffects() || S3SSRoomColourSaved() || g_oldStandalone.load(); }

void S3SSRoomColourItem() {
    static const char* result = nullptr; // the last correction's outcome, kept while the page is open
    if (!S3SSRoomColourSaved() && !result) return;
    ImGui::PushID("AttentionS3SS");
    if (ApexUi::BeginCard("##Card")) {
        ApexUi::CardHeader(IconId::Puzzle, "Saved S3SS room color",
                           "S3SS keeps a fixed color for the background light in rooms (BradyBunchBlue RGB)", nullptr, nullptr);
        ApexUi::CardDivider();
        if (S3SSRoomColourSaved()) {
            if (ApexUi::BeginControlRow("Remove the saved color", "Copies S3SS.toml to the Apex Radiance folder and removes only that color; the rest of S3SS stays as it is",
                                        ApexUi::ButtonWidth("Back up and correct##S3SSFix", true))) {
                if (ApexUi::IconTextButton("Back up and correct##S3SSFix", IconId::Save, nullptr, ButtonKind::Primary)) {
                    switch (UnlitRooms::CorrectS3SSConflict().status) {
                    case S3SSDetect::RoomAmbientCorrectionStatus::S3SSNotLoaded: result = "Sims3SettingsSetter is not loaded. No changes were made."; break;
                    case S3SSDetect::RoomAmbientCorrectionStatus::ConfigUnavailable: result = "Could not read S3SS.toml. No changes were made."; break;
                    case S3SSDetect::RoomAmbientCorrectionStatus::NoOverride: result = "No supported saved room-light color override was found. S3SS was not changed."; break;
                    case S3SSDetect::RoomAmbientCorrectionStatus::BackupFailed: result = "Apex could not verify the backup. S3SS was not changed."; break;
                    case S3SSDetect::RoomAmbientCorrectionStatus::ConfigChanged: result = "S3SS.toml changed during correction. No changes were written; try again."; break;
                    case S3SSDetect::RoomAmbientCorrectionStatus::WriteFailed: result = "Apex could not save the correction. Check the Apex log; the backup is preserved."; break;
                    case S3SSDetect::RoomAmbientCorrectionStatus::Saved: result = "Correction complete. The backup is in the Apex Radiance folder; restart the game for S3SS to keep the change."; break;
                    }
                    S3SSRoomColourSaved(true);
                }
                ApexUi::EndControlRow();
            }
            ApexUi::MutedText("Rooms at Night already uses the game's blue in its place; removing the color also restores it while Rooms at Night is off");
        }
        if (result) ApexUi::IconNote(S3SSRoomColourSaved() ? IconId::TriangleAlert : IconId::CircleCheck, result,
                                     S3SSRoomColourSaved() ? VioletTheme::kWarning : VioletTheme::kSuccess);
    }
    ApexUi::EndCard();
    ImGui::PopID();
}

void OldStandaloneItem() {
    if (!g_oldStandalone.load()) return;
    std::string oldModule;
    {
        std::lock_guard<std::mutex> lock(g_detailLock);
        oldModule = g_oldStandaloneModule;
    }
    ImGui::PushID("AttentionOldCopy");
    if (ApexUi::BeginCard("##Card")) {
        ApexUi::CardHeader(IconId::TriangleAlert, "Two copies of the mod",
                           "An old version, under the previous name, is next to the current one. Only one can run, so the old one stays idle", nullptr, nullptr);
        ApexUi::CardDivider();
        ApexUi::MutedText(I18n::Trf("With the game closed, delete {} from Game\\Bin and keep ApexRadiance.asi. This notice goes away on the next start.", oldModule).c_str());
    }
    ApexUi::EndCard();
    ImGui::PopID();
}

void ConflictsPage() {
    ApexUi::PageTitle("Attention", "Settings outside Apex that need a look");
    if (!HasAttentionItems()) ApexUi::IconNote(IconId::CircleCheck, "Nothing to fix");
    S3SSRoomColourItem();
    OldStandaloneItem();
}

void AntiAliasingContent() {
    GameEdgeSmoothingNote("Edge Smoothing");
    FeatureCard("EdgeSmoothing", IconId::Spline, "Edge Smoothing", "Clean edges on the world; menus stay sharp");
}

void EdgeSmoothingPage() {
    ApexUi::PageTitle("Edge Smoothing", "Clean edges on the world; menus stay sharp");
    AntiAliasingContent();
}

// ---- System > Performance ----

// A feature shown as one switch row inside a card (its description, which ends with the credit, on hover), with its
// "Not available" / error notes under it. True while it is on (or in the search results, where its rows are searched).
bool FeatureSwitchRow(const char* patchName, const char* label, const char* text, bool experimental = false) {
    ApexPatch* patch = Find(patchName);
    if (!patch) return false;
    ImGui::PushID(patchName);
    bool on = patch->IsEnabled();
    ImGui::BeginDisabled(!Switchable(patch));
    if (experimental) ApexUi::SetNextRowBadge("Experimental", "Still being tested: if anything looks wrong or the game crashes, turn it off");
    if (ApexUi::SwitchRow(label, &on, text, patch->IsEnabledByDefault())) SetPatch(patch, on);
    ImGui::EndDisabled();
    if (!ApexUi::FilterActive()) {
        ApexUi::Tooltip(Description(patch));
        if (!patch->IsCompatibleWithCurrentVersion()) NotAvailableNote(patch);
        else if (Loading()) CardNote("Starting\xE2\x80\xA6");
        CardError(patch->GetLastError());
    }
    ImGui::PopID();
    return patch->IsEnabled() || ApexUi::FilterActive();
}

void PerformanceCard() {
    ImGui::PushID("PerformanceLighting");
    if (ApexUi::BeginCard("##Card")) {
        ApexUi::CardHeader(IconId::Gauge, "Camera and lighting", "Smoother movement while rooms and lots update", nullptr, nullptr);
        ApexUi::CardDivider();
        FeatureSwitchRow(Performance::kRoomLightQueueName, "Faster room lighting", "Rooms light up sooner when you enter a lot or change floors");
        if (FeatureSwitchRow(Performance::kLotLightingName, "Spread lot lighting while moving", "Lots relight in small steps while the camera moves")) {
            float ms = static_cast<float>(Performance::LotLightingBudgetMs());
            char value[16];
            std::snprintf(value, sizeof value, "%d ms", Performance::LotLightingBudgetMs());
            ApexUi::SliderOptions o;
            o.tooltip = "The current lot's time per frame while moving; 3 ms is the default";
            o.valueText = value;
            o.leftLabel = "Smoother";
            o.rightLabel = "Lights sooner";
            o.defaultValue = static_cast<float>(Performance::kLotLightingBudgetDefault);
            if (ApexUi::Slider("Lot lighting time while moving", &ms, 1.0f, 15.0f, o)) Performance::SetLotLightingBudgetMs(static_cast<int>(std::lround(ms)));
        }
        FeatureSwitchRow(Performance::kWallShadingName, "Wall shading waits while moving", "Walls of new lots get their shading when you stop");
        FeatureSwitchRow(Performance::kSceneBudgetName, "Spread new objects over frames", "Fewer hitches when a lot streams in while the camera moves");
    }
    ApexUi::EndCard();
    ImGui::PopID();
    ImGui::PushID("PerformanceFiles");
    if (ApexUi::BeginCard("##Card")) {
        ApexUi::CardHeader(IconId::Search, "Files and objects", "Less repeated searching as content loads", nullptr, nullptr);
        ApexUi::CardDivider();
        if (FeatureSwitchRow(Performance::kResourceCacheName, "Faster game file lookups", "Fewer small stutters when objects and textures load"))
            FeatureSwitchRow(Performance::kLookupMissesName, "Remember missing files", "Skips repeated searches for files no package has");
        FeatureSwitchRow(Performance::kFileListName, "Faster file lists", "Fewer stutters when Sims load outfits and shapes");
        FeatureSwitchRow(Performance::kObjectIndexName, "Faster object lookups", "Fewer hitches when lot lights update; less script work");
    }
    ApexUi::EndCard();
    ImGui::PopID();
    ImGui::PushID("PerformanceTextures");
    if (ApexUi::BeginCard("##Card")) {
        ApexUi::CardHeader(IconId::Layers, "Textures and Sims", "Fewer pauses when textures and Sims are built", nullptr, nullptr);
        ApexUi::CardDivider();
        if (FeatureSwitchRow(Performance::kFastTextureName, "Faster texture compression", "Fewer hitches when the game builds terrain, Sim and lot textures")) {
            bool cores = Performance::FastTextureSeveralCores();
            if (ApexUi::SwitchRow("Use several cores", &cores, "Large textures are shared out over several processor cores, with the same result"))
                Performance::SetFastTextureSeveralCores(cores);
        }
        FeatureSwitchRow(Performance::kFastCacheName, "Faster cache compression", "Fewer hitches when the game stores Sims and objects in its caches");
        FeatureSwitchRow(Performance::kFastCasName, "Faster Sim building", "Fewer hitches when Sims are edited or change outfits");
    }
    ApexUi::EndCard();
    ImGui::PopID();
    ImGui::PushID("PerformanceMemory");
    if (ApexUi::BeginCard("##Card")) {
        ApexUi::CardHeader(IconId::Activity, "Memory handling", "Less overhead when the game creates temporary data", nullptr, nullptr);
        ApexUi::CardDivider();
        FeatureSwitchRow(Performance::kFastMemoryName, "Faster memory handling", "Less waiting when the game hands out and frees memory");
    }
    ApexUi::EndCard();
    ImGui::PopID();
}

void PerformancePage() {
    ApexUi::PageTitle("Performance", "Fewer stutters while you play");
    PerformanceCard();
}

// ---- System > Report a problem (30/09): the captures players send with a bug report (features/captures.h) ----
// Everything a player needs on one page, in plain words: how to report, the capture buttons with their keys, capture
// sessions that gather several captures in one folder, and the list of saved captures with Open / Delete.

struct ReportState {
    std::vector<Captures::Entry> list;
    unsigned long long scannedAt = 0;
    std::string confirmDelete; // the folder whose Delete was clicked once ("*" = Delete all)
    unsigned long long confirmAt = 0;
    bool receiptInitialized = false;
    uint64_t receiptSeen = 0;
    std::string notesFolder;
    Captures::Description fallbackNote;
    char optionalTitle[256]{};
    char optionalDescription[3072]{};
    bool notesError = false;
    bool notesDeleteError = false;
};
ReportState g_report;

// Shared action alignment is also used by the developer-mode confirmation.
bool ReportDialogActions(const char* first, bool firstIcon, const char* last, bool lastIcon) {
    const float available = ImGui::GetContentRegionAvail().x;
    const float firstWidth = ApexUi::ButtonWidth(first, firstIcon);
    const float total = firstWidth + ImGui::GetStyle().ItemSpacing.x + ApexUi::ButtonWidth(last, lastIcon);
    const bool oneRow = total <= available;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, available - (oneRow ? total : firstWidth)));
    return oneRow;
}
void ReportDialogLastAction(const char* label, bool withIcon, bool oneRow) {
    if (oneRow) ImGui::SameLine();
    else ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - ApexUi::ButtonWidth(label, withIcon)));
}

void ReportOptionalNotes() {
    const auto saved = Captures::LastSave();
    if (saved.serial != g_report.receiptSeen && !saved.saving && !Captures::Saving() && !Recorder::Active()) {
        g_report.receiptSeen = saved.serial;
        if (saved.serial && !saved.failed && !Captures::SessionActive()) {
            g_report.notesFolder = saved.folder;
            g_report.fallbackNote = Captures::ReadDescription(saved.folder);
            g_report.optionalTitle[0] = g_report.optionalDescription[0] = 0;
            g_report.notesError = false;
            g_report.notesDeleteError = false;
            ImGui::OpenPopup("OptionalCaptureNotes");
        }
    }
    const float modalWidth = std::max(1.0f, std::min(500.0f * ApexUi::Unit(), ImGui::GetIO().DisplaySize.x - 32.0f));
    // Auto-resize only the height: full-width fields must not feed back into the next frame's width.
    ImGui::SetNextWindowSizeConstraints(ImVec2(modalWidth, 0.0f), ImVec2(modalWidth, FLT_MAX));
    ImGui::SetNextWindowSize(ImVec2(modalWidth, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("OptionalCaptureNotes", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize)) {
        ApexUi::CardHeader(IconId::Check, "Capture saved", "Give it a name to find it more easily", nullptr, nullptr);
        ApexUi::CardDivider();
        {
            const ApexUi::ControlSizeScope size(ApexUi::ControlSize::Primary);
            ImGui::TextUnformatted(I18n::Tr("Title (required)"));
            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("##OptionalCaptureTitle", g_report.optionalTitle, sizeof(g_report.optionalTitle));
            ApexUi::Gap(ApexUi::kSpace2);
            ImGui::TextUnformatted(I18n::Tr("Description (optional)"));
            ImGui::SetNextItemWidth(-1);
            ImGui::InputTextMultiline("##OptionalCaptureDescription", g_report.optionalDescription, sizeof(g_report.optionalDescription), ImVec2(-1, 100 * ApexUi::Unit()));
            ApexUi::MutedText("Enter a title to finish. The description is optional; your capture files are already saved.");
            if (g_report.notesDeleteError) ApexUi::IconNote(IconId::TriangleAlert, "Could not delete the capture. Check folder access and try again.");
            if (g_report.notesError) ApexUi::IconNote(IconId::TriangleAlert, "Could not save the description. Check free space and folder access");
            ApexUi::Gap(ApexUi::kSpace2);
            const auto filled = [](const char* value) { return std::string_view(value).find_first_not_of(" \t\r\n") != std::string_view::npos; };
            const bool titleValid = filled(g_report.optionalTitle);
            const bool oneRow = ReportDialogActions("Cancel", false, "Save capture", true);
            ImGui::BeginDisabled(Captures::Saving() || Recorder::Active() || LightProbe::Busy());
            if (ApexUi::TextButton("Cancel", "Deletes this new capture and closes the form")) {
                if (Captures::Delete(g_report.notesFolder)) {
                    g_report.scannedAt = 0;
                    g_report.notesFolder.clear();
                    ImGui::CloseCurrentPopup();
                } else g_report.notesDeleteError = true;
            }
            ImGui::EndDisabled();
            ReportDialogLastAction("Save capture", true, oneRow);
            ImGui::BeginDisabled(!titleValid || Captures::Saving() || Recorder::Active() || LightProbe::Busy());
            if (ApexUi::IconTextButton("Save capture", IconId::Save, nullptr, ButtonKind::Primary)) {
                const std::string title = g_report.optionalTitle;
                const std::string text = filled(g_report.optionalDescription) ? g_report.optionalDescription : g_report.fallbackNote.text;
                if (Captures::SaveFolderDescription(g_report.notesFolder, title, text)) {
                    g_report.scannedAt = 0;
                    ImGui::CloseCurrentPopup();
                } else g_report.notesError = true;
            }
            ImGui::EndDisabled();
        } // restore the frame style before EndPopup checks its stack
        ImGui::EndPopup();
    }
}

std::string SizeText(uint64_t bytes) {
    if (bytes >= (1ull << 30)) return std::format("{:.1f} GB", static_cast<double>(bytes) / (1ull << 30));
    if (bytes >= (1ull << 20)) return std::format("{:.1f} MB", static_cast<double>(bytes) / (1ull << 20));
    return std::format("{} KB", (bytes + 1023) / 1024);
}

// The key of a capture action, as text ("F6")
std::string CaptureKey(Hotkeys::Action a) { return ApexConfig::KeyChordText(Hotkeys::Key(a)); }

// A Delete button that asks for a second click within 4 s (id = the folder, "*" = all); true on the confirming click
bool ConfirmDelete(const char* label, const std::string& id, const char* tooltip) {
    const unsigned long long now = GetTickCount64();
    const bool armed = g_report.confirmDelete == id && now - g_report.confirmAt < 4000;
    const std::string text = armed ? std::string(I18n::Tr("Click again to delete")) + "##" + id : std::string(I18n::Tr(label)) + "##" + id;
    if (!ApexUi::IconTextButton(text.c_str(), IconId::Trash2, tooltip)) return false;
    if (armed) {
        g_report.confirmDelete.clear();
        return true;
    }
    g_report.confirmDelete = id;
    g_report.confirmAt = now;
    return false;
}

void ReportHowCard() {
    ImGui::PushID("ReportHow");
    if (ApexUi::BeginCard("##Card")) {
        ApexUi::CardHeader(IconId::Info, "How to report a problem", "Three steps, about a minute", nullptr, nullptr);
        ApexUi::CardDivider();
        ApexUi::MutedText("1. Make the problem happen in the game (or keep it on screen).");
        ApexUi::MutedText("2. Save a capture below, or use its shortcut in the game. A note at the top center confirms the capture and saving.");
        ApexUi::MutedText("3. Open the captures folder, right-click the capture's folder, Send to \xE2\x80\xBA Compressed (zipped) folder, and send the .zip with a few "
                          "words on what you saw: the Bugs tab on Nexus Mods, or GitHub.");
        ApexUi::Gap(ApexUi::kSpace1);
        const std::string tip = I18n::Trf("Not sure it is Apex Radiance? Press {} (Compare with the game): if the problem goes away, it comes from the mod.",
                                          CaptureKey(Hotkeys::Action::Compare));
        ApexUi::IconNote(IconId::Lightbulb, tip.c_str());
        const std::string crash = Captures::RecentCrash();
        if (!crash.empty()) {
            const std::string note = I18n::Trf("The game crashed on {}: \"Save a report\" includes the crash details.", crash);
            ApexUi::IconNote(IconId::TriangleAlert, note.c_str(), VioletTheme::kWarning);
        }
    }
    ApexUi::EndCard();
    ImGui::PopID();
}

void ReportCaptureCard() {
    ImGui::PushID("ReportCapture");
    if (ApexUi::BeginCard("##Card")) {
        ApexUi::CardHeader(IconId::Camera, "Save a capture", "Each one goes into its own dated folder; nothing is ever overwritten", nullptr, nullptr);
        ApexUi::CardDivider();
        const float u = ApexUi::Unit();
        // a row: name, plain explanation (with its key), one button
        const auto row = [&](const char* label, const std::string& description, const char* button, IconId icon, ApexUi::ButtonKind kind) {
            if (!ApexUi::BeginControlRow(label, description.c_str(), ApexUi::ButtonWidth(I18n::Tr(button), true))) return false;
            const bool stopping = Recorder::Active() && std::string_view(button).starts_with("Stop");
            ImGui::BeginDisabled(!stopping && (Loading() || Captures::Saving() || LightProbe::Busy() || Recorder::Active()));
            const bool clicked = ApexUi::IconTextButton(button, icon, nullptr, kind);
            ImGui::EndDisabled();
            ApexUi::EndControlRow();
            return clicked;
        };
        if (row("Save a report", I18n::Tr("The log and your settings. Good for any problem, and after a crash."), "Save##Report", IconId::Save,
                ApexUi::ButtonKind::Secondary)) {
            Captures::SaveReport();
            g_report.scannedAt = 0;
        }
        const int secs = Recorder::SecondsRecorded();
        const std::string recDesc =
            I18n::Trf("Records lighting for up to 20 seconds while you reproduce the problem. Key: {}",
                      CaptureKey(Hotkeys::Action::Recorder));
        if (row("Record a few seconds", recDesc, secs >= 0 ? "Stop##Rec" : "Start##Rec", IconId::Activity, secs >= 0 ? ApexUi::ButtonKind::Primary : ApexUi::ButtonKind::Secondary)) {
            Recorder::RequestToggle();
            g_report.scannedAt = 0;
        }
        const std::string probeKey = CaptureKey(Hotkeys::Action::Probe);
        const std::string probeDesc = I18n::Trf("Close the menu, point at the problem and press {} to capture that spot and its textures.",
                                                probeKey);
        const ImVec2 probeChipSize = ApexUi::ChipSize(probeKey.c_str());
        if (ApexUi::BeginControlRow("Capture the light at a spot", probeDesc.c_str(), probeChipSize.x, IconId::None, probeChipSize.y)) {
            ApexUi::Chip(probeKey.c_str(), VioletTheme::kAccentLight);
            ApexUi::EndControlRow();
        }
        const std::string snapDesc = I18n::Trf("Saves the current lamps and rooms to investigate incorrect lighting. Key: {}",
                                               CaptureKey(Hotkeys::Action::Diagnostics));
        if (row("Lighting snapshot", snapDesc, "Save##Snapshot", IconId::Lightbulb, ApexUi::ButtonKind::Secondary)) {
            LightDiag::RequestDump();
            g_report.scannedAt = 0;
        }
        ApexPatch* nl = Find("NightTerrainRelight");
        if (!nl || !nl->IsEnabled()) CardNote("The recording and the two lighting captures need Night Lights on");

        // a picture of the screen with every capture ([ui] capture_screenshot)
        ApexConfig::UiSettings ui = ApexConfig::GetUi();
        if (ApexUi::SwitchRow("Include a screenshot", &ui.captureScreenshot,
                              "Each capture saves a screenshot without this menu to show the problem", true))
            { ApexConfig::SetUi(ui); Captures::SetScreenshots(ui.captureScreenshot); }
    }
    ApexUi::EndCard();
    ImGui::PopID();
}

void ReportListCard() {
    ImGui::PushID("ReportList");
    const unsigned long long now = GetTickCount64();
    if (!g_report.scannedAt || now - g_report.scannedAt > 2000) { // the folder is read again every 2 s while the page is open
        g_report.list = Captures::List();
        g_report.scannedAt = now;
    }
    if (ApexUi::BeginCard("##Card")) {
        uint64_t total = 0;
        for (const auto& e : g_report.list) total += e.bytes;
        const std::string sub = g_report.list.empty() ? std::string(I18n::Tr("None yet"))
                                                      : I18n::Trf("{} saved, {} in all; newest first", g_report.list.size(), SizeText(total));
        ApexUi::CardHeader(IconId::Bookmark, "Your captures", sub.c_str(), nullptr, nullptr);
        ApexUi::CardDivider();
        const std::string openSession = Captures::SessionFolder();
        for (const auto& e : g_report.list) {
            ImGui::PushID(e.folder.c_str());
            std::string kind = I18n::Tr(e.kind.c_str());
            if (e.kind.rfind("Session", 0) == 0) kind = I18n::Trf("Session ({} captures)", e.items) + (e.folder == openSession ? std::string(" \xC2\xB7 ") + I18n::Tr("open") : "");
            const std::string label = !e.title.empty() && e.title != e.folder ? e.title :
                e.date.empty() ? kind : std::format("{} \xC2\xB7 {} \xC2\xB7 {}", e.date, e.time, kind);
            const std::string size = SizeText(e.bytes);
            const bool deleteArmed = g_report.confirmDelete == e.folder && now - g_report.confirmAt < 4000;
            const char* deleteLabel = deleteArmed ? "Click again to delete" : "Delete";
            const bool canDelete = e.folder != openSession;
            const float gap = ImGui::GetStyle().ItemSpacing.x;
            const float w = ImGui::CalcTextSize(size.c_str()).x + gap + ApexUi::ButtonWidth("Open", true) +
                            (canDelete ? gap + ApexUi::ButtonWidth(deleteLabel, true) : 0.0f);
            ApexUi::SetNextRowUntranslated();
            if (ApexUi::BeginControlRow(label.c_str(), nullptr, w)) {
                ImGui::AlignTextToFramePadding();
                ImGui::TextColored(Col(VioletTheme::kTextMuted), "%s", size.c_str());
                ImGui::SameLine();
                if (ApexUi::IconTextButton("Open", IconId::ExternalLink, "Shows this capture's folder")) Captures::Open(e.folder);
                ImGui::SameLine();
                ImGui::BeginDisabled(Captures::Saving() || Recorder::Active() || LightProbe::Busy());
                if (e.folder != openSession && ConfirmDelete("Delete", e.folder, "Deletes this capture for good")) {
                    Captures::Delete(e.folder);
                    g_report.scannedAt = 0;
                }
                ImGui::EndDisabled();
                ApexUi::EndControlRow();
            }
            ImGui::PopID();
        }
        ApexUi::Gap(ApexUi::kSpace1);
        if (ApexUi::IconTextButton("Open the captures folder", IconId::ExternalLink, "Opens Apex Radiance \xE2\x80\xBA Captures in your Documents folder"))
            Captures::OpenFolder();
        if (!g_report.list.empty()) {
            ImGui::SameLine();
            ImGui::BeginDisabled(Captures::Saving() || Recorder::Active() || LightProbe::Busy());
            if (ConfirmDelete("Delete all", "*", "Deletes every saved capture for good")) {
                Captures::DeleteAll();
                g_report.scannedAt = 0;
            }
        }
        if (!g_report.list.empty()) ImGui::EndDisabled();
        ApexUi::MutedText("Captures are never overwritten or deleted by themselves; delete the ones you no longer need here.");
    }
    ApexUi::EndCard();
    ImGui::PopID();
}

// The capture session, first on the page and drawn apart (user, 30/09: "the sessions part should be higher up, clearer,
// a bit different from the others with a nicer UI"): a violet-edged card with a big icon, the three steps as numbered
// bubbles and one large button; while a session is open, a pulsing dot, its time and count, the captures so far and the
// End button.
void SessionHeroCard() {
    ImGui::PushID("ReportSession");
    const float u = ApexUi::Unit();
    const bool open = Captures::SessionActive();
    ImGui::PushStyleColor(ImGuiCol_ChildBg, Col(open ? VioletTheme::kAccentDark : VioletTheme::kCardBg, open ? 0.28f : 1.0f));
    ImGui::PushStyleColor(ImGuiCol_Border, Col(VioletTheme::kAccent, open ? 0.65f : 0.35f));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 14.0f * u);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ApexUi::kSpace4 * u, ApexUi::kSpace3 * u));
    const bool visible = ImGui::BeginChild("##Hero", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_AlwaysUseWindowPadding,
                                           ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(3);
    ImGui::PopStyleColor(2);
    if (visible) {
        ImDrawList* dl = ImGui::GetWindowDrawList();
        const float lineH = ImGui::GetTextLineHeight();
        const int seconds = Captures::SessionSeconds();
        const std::string subtitle = open
            ? I18n::Trf("Recording a session \xC2\xB7 {}:{:02} \xC2\xB7 {} captures", seconds / 60, seconds % 60, Captures::SessionCaptures())
            : std::string(I18n::Tr("The best way to report a problem: everything you capture goes into one folder"));
        ApexUi::CardHeader(IconId::Bug, "Capture session", subtitle.c_str(), nullptr, nullptr);
        ApexUi::Gap(ApexUi::kSpace3);
        if (!open) {
            // the three steps as numbered bubbles
            static const char* const kSteps[3] = {"Start a session here", "Make the problem happen and save captures, with the buttons below or their keys",
                                                  "End it: one folder with the captures, the log and your settings, ready to send"};
            ImVec2 previousCenter{};
            for (int i = 0; i < 3; i++) {
                const ImVec2 b = ImGui::GetCursorScreenPos();
                const float br = std::max(9.0f * u, lineH * 0.45f);
                const ImVec2 center(b.x + br, b.y + lineH * 0.5f);
                if (i) dl->AddLine(ImVec2(previousCenter.x, previousCenter.y + br), ImVec2(center.x, center.y - br), ImGui::GetColorU32(Col(VioletTheme::kAccent, 0.3f)), u);
                dl->AddCircleFilled(center, br, ImGui::GetColorU32(Col(VioletTheme::kAccentDark)), 24);
                previousCenter = center;
                const std::string n = std::to_string(i + 1);
                const float numberSize = ImGui::GetFontSize() * 0.85f;
                const ImVec2 ns = ImGui::GetFont()->CalcTextSizeA(numberSize, FLT_MAX, 0.0f, n.c_str());
                dl->AddText(ImGui::GetFont(), numberSize, ImVec2(center.x - ns.x * 0.5f, center.y - ns.y * 0.5f),
                            ImGui::GetColorU32(Col(VioletTheme::kText)), n.c_str());
                ImGui::Dummy(ImVec2(2.0f * br, lineH));
                ImGui::SameLine(0.0f, ApexUi::kSpace3 * u);
                ImGui::PushTextWrapPos(0.0f);
                ImGui::TextColored(Col(VioletTheme::kText, 0.9f), "%s", I18n::Tr(kSteps[i]));
                ImGui::PopTextWrapPos();
                if (i < 2) ApexUi::Gap(ApexUi::kSpace2);
            }
            ApexUi::Gap(ApexUi::kSpace3);
            ImGui::Separator();
            ApexUi::Gap(ApexUi::kSpace2);
            const ApexUi::ControlSizeScope size(ApexUi::ControlSize::Primary);
            ImGui::BeginDisabled(Loading() || Captures::Saving() || Recorder::Active() || LightProbe::Busy());
            if (ApexUi::IconTextButton("Start a session##Sess", IconId::Camera, "Every capture you save goes into this session's folder until you end it",
                                       ApexUi::ButtonKind::Primary)) {
                Captures::BeginSession();
                g_report.scannedAt = 0;
            }
            ImGui::EndDisabled();
        } else {
            const std::vector<std::string> items = Captures::SessionItems();
            if (items.empty()) {
                ApexUi::MutedText("No captures yet: make the problem happen, then save captures with the buttons below or their keys.");
            } else {
                const size_t from = items.size() > 6 ? items.size() - 6 : 0;
                if (from) ImGui::TextColored(Col(VioletTheme::kTextMuted), "%s", I18n::Trf("\xE2\x80\xA6 and {} more", from).c_str());
                for (size_t i = from; i < items.size(); i++) {
                    // "HH-MM-SS kind" -> "HH:MM:SS  kind"
                    std::string line = items[i];
                    if (line.size() > 9 && line[2] == '-' && line[5] == '-') line = line.substr(0, 2) + ":" + line.substr(3, 2) + ":" + line.substr(6, 2) + "  " + I18n::Tr(line.substr(9).c_str());
                    ApexUi::DrawIcon(dl, IconId::Check, ImVec2(ImGui::GetCursorScreenPos().x, ImGui::GetCursorScreenPos().y + (lineH - ApexUi::kIconSmall * u) * 0.5f), ApexUi::kIconSmall * u,
                                     ImGui::GetColorU32(Col(VioletTheme::kSuccess)));
                    ImGui::Dummy(ImVec2(ApexUi::kIconSmall * u, lineH));
                    ImGui::SameLine(0.0f, ApexUi::kSpace2 * u);
                    ImGui::TextColored(Col(VioletTheme::kText, 0.9f), "%s", line.c_str());
                }
            }
            ApexUi::Gap(ApexUi::kSpace3);
            const ApexUi::ControlSizeScope size(ApexUi::ControlSize::Primary);
            ImGui::BeginDisabled(Captures::Saving() || Recorder::Active() || LightProbe::Busy());
            if (ApexUi::IconTextButton("End and save the session##Sess", IconId::Check, "Adds the log, your settings and a list of the captures, ready to zip and send",
                                       ApexUi::ButtonKind::Primary)) {
                Captures::EndSession();
                g_report.scannedAt = 0;
            }
            ImGui::EndDisabled();
            ImGui::SameLine();
            if (ApexUi::IconTextButton("Open its folder##Sess", IconId::ExternalLink, "Shows this session's folder")) Captures::Open(Captures::SessionFolder());
        }
    }
    ImGui::EndChild();
    ImGui::Dummy(ImVec2(0.0f, ApexUi::kSpace2 * u));
    ImGui::PopID();
}

void ReportPage() {
    if (!g_report.receiptInitialized) {
        g_report.receiptSeen = Captures::LastSave().serial;
        g_report.receiptInitialized = true;
    }
    ApexUi::PageTitle("Report a problem", "Save what helps fix a bug, then send it");
    SessionHeroCard();
    ReportCaptureCard();
    ReportListCard();
    ReportHowCard();
    ReportOptionalNotes();
    const auto saved = Captures::LastSave();
    if (saved.failed) {
        ApexUi::IconNote(IconId::TriangleAlert, "Some files could not be saved");
        ImGui::BeginDisabled(Captures::Saving() || Recorder::Active() || LightProbe::Busy());
        if (ApexUi::IconTextButton("Retry saving", IconId::RotateCcw)) Captures::RetrySave();
        ImGui::EndDisabled();
    }
}

// ---- System > Developer (development build) ----

// A developer card: header, then body() (or "Off" while the feature is off)
template <typename Body> void DevCard(const char* id, IconId icon, const char* title, const char* subtitle, bool on, Body&& body) {
    ImGui::PushID(id);
    if (ApexUi::BeginCard("##Card")) {
        ApexUi::CardHeader(icon, title, subtitle, nullptr, nullptr);
        ApexUi::CardDivider();
        if (on) {
            ApexUi::ControlSizeScope compact(ApexUi::ControlSize::Compact);
            body();
        }
        else CardNote("Off");
    }
    ApexUi::EndCard();
    ImGui::PopID();
}

void DevLightingTab() {
    ApexUi::MutedText("Keep the camera fixed. Save the correct state, cause the problem and save again. Refresh only after capturing the incorrect state.");
    ApexPatch* ntr = Find(kNightLighting);
    if (ntr && ntr->IsEnabled()) NightLighting::DrawDeveloper();
    else DevCard("DevNightLighting", IconId::Lightbulb, "Lighting diagnostics", "Turn on Night Lights to inspect lighting", false, [] {});
    if (ApexPatch* upper = Find(kUpperFloors)) {
        if (ApexUi::BeginAdvanced("GroundProvider", "Technical reference from the current code")) {
            const char* state = !upper->IsEnabled() ? "off" : NightLighting::SplitLevelProvidedByS3SS() ? "on, provided by Sims3SettingsSetter" : "on (GetLotID 0x6BC020 returns 0)";
            ImGui::TextDisabled("Every-Story Ground Light: %s", state);
            ApexUi::EndAdvanced();
        }
    }
}

void DevProfilerTab() {
    ApexUi::MutedText("Clear old data, reproduce the pause, then stop and save the measurement.");
    ImGui::PushID("FrameProfiler");
    if (ApexUi::BeginCard("##Card")) {
        bool on = FrameProfiler::IsEnabled();
        if (ApexUi::CardHeader(IconId::Activity, "Frame times and stutters", "Find what contributes to the longest pauses", nullptr, &on)) {
            FrameProfiler::SetEnabled(on);
            ApexConfig::RequestSave();
        }
        ApexUi::CardDivider();
        FrameProfiler::RenderUI(false);
    }
    ApexUi::EndCard();
    ImGui::PopID();
    DevCard("ShaderPreparation", IconId::Aperture, "Shader preparation", "Read the current preparation state", true, [] {
        if (ApexUi::BeginAdvanced("ShaderState", "Technical reference")) {
            ImGui::TextWrapped("Apex shaders: %s", ShaderCache::StatusText().c_str());
            ApexUi::EndAdvanced();
        }
    });
    const auto inspect = [](const char* key, const char* title, const char* purpose, const char* id) {
        ApexPatch* p = Find(key);
        DevCard(id, IconId::Gauge, title, purpose, p && p->IsEnabled(), [p] { p->RenderDeveloperUI(); });
    };
    ApexPatch* cache = Find(Performance::kResourceCacheName);
    ApexPatch* lists = Find(Performance::kFileListName);
    const bool cachesOn = (cache && cache->IsEnabled()) || (lists && lists->IsEnabled());
    DevCard("DevCaches", IconId::Search, "File searches and remembered answers",
            "Compare cached file and list answers with the game", cachesOn,
            [cache, lists] { (cache ? cache : lists)->RenderDeveloperUI(); });
    inspect(Performance::kObjectIndexName, "Find objects faster", "Check indexed results and expired entries", "DevObjectIndex");
    inspect(Performance::kLotLightingName, "Lighting while the camera moves", "Inspect deferred lighting work and the camera state", "DevLotLighting");
    inspect(Performance::kWallShadingName, "Wall shading", "Inspect deferred lighting work and the camera state", "DevWallShading");
    inspect(Performance::kSceneBudgetName, "Objects spread across frames", "Inspect pending work and object lifetime checks", "DevSceneObjects");
    inspect(Performance::kFastTextureName, "Texture compression and processor cores", "Compare texture output and processor worker use", "DevTextures");
    inspect(Performance::kFastCacheName, "Compressed game data", "Verify decompressed data matches the original", "DevCompression");
}

void DevDebugViewsTab() {
    ApexUi::MutedText("Inspect one effect at a time. Switch off its diagnostic view when finished.");
    const auto inspect = [](const char* key, const char* title, const char* purpose) {
        ApexPatch* p = Find(key);
        DevCard(key, IconId::Eye, title, purpose, p && p->IsEnabled(), [p] { p->RenderDeveloperUI(); });
    };
    inspect("EdgeSmoothing", "Edge smoothing", "Inspect the pixels changed by SMAA or FXAA.");
    inspect("DepthBlur", "Depth blur", "Inspect focus and blur strength. The far-plane value is used only by fixed focus.");
    inspect("AmbientOcclusion", "Ambient shadows", "Show the added shade by itself and save depth and colour for investigation.");
    inspect("SceneDither", "Gradient correction coverage", "Show which surfaces receive the correction and review why some shaders were refused.");
    DevCard("DebugPicture", IconId::Image, "Image adjustments", "Review image precision and the time the effect uses on the graphics card.", true, [] { Picture::Get().RenderDeveloperUI(); });
}

void DeveloperPage() {
    const toml::table preferencesBefore = DeveloperSettings::Capture();
    toml::table profilerBefore; FrameProfiler::SaveToToml(profilerBefore);
    ApexUi::PageTitle("Developer", "Inspect a problem, collect evidence and compare the result");
    static int tab = 0;
    constexpr const char* labels[] = {"Lighting", "Performance", "Captures", "Visual effects", "Translations"};
    constexpr IconId icons[] = {IconId::Lightbulb, IconId::Activity, IconId::Camera, IconId::Eye, IconId::Type};
    ApexUi::TabBar("##DeveloperTabs", &tab, labels, 5, icons);
    if (tab == 0) DevLightingTab();
    else if (tab == 1) DevProfilerTab();
    else if (tab == 2) {
        ApexUi::MutedText("A session groups related captures. Profiler measurements are saved separately.");
        SessionHeroCard();
        ReportCaptureCard();
        FeatureCard("FrameCapture", IconId::Camera, "Capture two drawn frames", "Save two frames of rendering operations for detailed investigation.");
        ReportListCard();
    } else if (tab == 3) DevDebugViewsTab();
    else {
        ApexUi::MutedText("Open the affected screens in the language you want to check. Save the missing-text list and placeholder errors to the log.");
        DevCard("TranslationReview", IconId::Type, "Review translations", "Collect missing text while visiting menu screens", true, [] {
            LanguageRow();
            const std::string missing = std::to_string(I18n::MissingCount());
            if (ApexUi::BeginControlRow("Missing translations", nullptr, ImGui::CalcTextSize(missing.c_str()).x)) {
                ImGui::AlignTextToFramePadding();
                ImGui::TextDisabled("%s", missing.c_str());
                ApexUi::EndControlRow();
            }
            const float actionsW = ApexUi::ButtonWidth("Clear collected data", true) + ImGui::GetStyle().ItemSpacing.x + ApexUi::ButtonWidth("Write the list to the log", true);
            if (ApexUi::BeginControlRow("Collected issues", "Save the current list before clearing it", actionsW)) {
                if (ApexUi::IconTextButton("Clear collected data", IconId::RotateCcw)) I18n::ClearMissing();
                ImGui::SameLine();
                if (ApexUi::IconTextButton("Write the list to the log", IconId::Save, nullptr, ButtonKind::Primary)) LOG_INFO("[I18n] Texts without a translation:\n" + I18n::MissingList(4000));
                ApexUi::EndControlRow();
            }
        });
        DevCard("MissingText", IconId::ListChecks, "Missing text", "Collected from the menu", true, [] {
            static ImGuiTextFilter filter;
            filter.Draw(I18n::Tr("Filter collected text"), ImGui::GetContentRegionAvail().x);
            if (I18n::MissingCount()) {
                const std::string list = I18n::MissingList(4000);
                bool found = false;
                for (const char* line = list.c_str(); *line;) {
                    const char* end = std::strchr(line, '\n');
                    if (!end) end = line + std::strlen(line);
                    if (filter.PassFilter(line, end)) { ImGui::TextWrapped("%.*s", static_cast<int>(end - line), line); found = true; }
                    line = *end ? end + 1 : end;
                }
                if (!found) ApexUi::MutedText("No collected text matches this filter");
            } else ApexUi::MutedText("No missing text collected. Visit menu screens to begin.");
        });
        DevCard("PlaceholderChecks", IconId::Scan, "Placeholder checks", "Formatting arguments must match across languages", true, [] {
            const std::string problems = I18n::PlaceholderProblems();
            if (problems.empty()) ApexUi::MutedText("No placeholder errors found");
            else {
                ApexUi::IconNote(IconId::TriangleAlert, "Translation argument errors", VioletTheme::kWarning);
                ImGui::TextWrapped("%s", problems.c_str());
            }
        });
    }
    toml::table profilerAfter; FrameProfiler::SaveToToml(profilerAfter);
    if (preferencesBefore != DeveloperSettings::Capture() || profilerBefore != profilerAfter) ApexConfig::RequestSave();
}


// ---- System > Settings ----

// Waiting for a new menu key: the first key pressed (with the modifiers held at that moment) becomes the chord.
void MenuKeyRow() {
    constexpr const char* kLabel = "Menu key";
    constexpr const char* kText = "Opens and closes this menu";
    ApexConfig::UiSettings ui = ApexConfig::GetUi();
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    if (!g_waitingForKey) {
        const std::string key = ApexConfig::KeyChordText(ui.toggle);
        if (!ApexUi::BeginControlRow(kLabel, kText, ImGui::CalcTextSize(key.c_str()).x + gap + ApexUi::ButtonWidth("Change##MenuKey", false))) return;
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(key.c_str());
        ImGui::SameLine();
        if (ApexUi::TextButton("Change##MenuKey", "Any key, with Ctrl, Shift or Alt if you like; Insert alone is taken by another mod's menu")) g_waitingForKey = true;
        ApexUi::EndControlRow();
        return;
    }
    const char* prompt = I18n::Tr("Press a key (Esc cancels)");
    if (ApexUi::BeginControlRow(kLabel, kText, ImGui::CalcTextSize(prompt).x)) {
        ImGui::AlignTextToFramePadding();
        ImGui::TextColored(Col(VioletTheme::kWarning), "%s", prompt);
        ApexUi::EndControlRow();
    }
    if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
        g_waitingForKey = false;
        return;
    }
    for (UINT vk = 0x08; vk <= 0xFE; vk++) {
        switch (vk) {
        case VK_SHIFT: case VK_CONTROL: case VK_MENU: case VK_LSHIFT: case VK_RSHIFT: case VK_LCONTROL: case VK_RCONTROL: case VK_LMENU: case VK_RMENU:
        case VK_LWIN: case VK_RWIN: case VK_ESCAPE: case VK_RETURN: case VK_SPACE: case VK_TAB: case VK_CAPITAL: case VK_NUMLOCK:
        case VK_LBUTTON: case VK_RBUTTON: case VK_MBUTTON: case VK_XBUTTON1: case VK_XBUTTON2:
            continue;
        default: break;
        }
        if (!(GetAsyncKeyState(static_cast<int>(vk)) & 0x8000)) continue;
        ui.toggle.vk = vk;
        ui.toggle.ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        ui.toggle.shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        ui.toggle.alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
        if (vk == VK_INSERT && !ui.toggle.ctrl && !ui.toggle.shift && !ui.toggle.alt) continue; // S3SS's own key
        ui.keyChosen = true;
        ApexConfig::SetUi(ui);
        g_waitingForKey = false;
        LOG_INFO("[Menu] Menu key set to " + ApexConfig::KeyChordText(ui.toggle));
        break;
    }
}

// Fixed steps instead of a slider: a slider would move under the mouse while the text it resizes grows
void TextSizeRow() {
    static constexpr float kSizes[] = {0.8f, 0.9f, 1.0f, 1.15f, 1.3f, 1.5f, 1.75f, 2.0f};
    static constexpr const char* kSizeNames[] = {"80%", "90%", "100%", "115%", "130%", "150%", "175%", "200%"};
    ApexConfig::UiSettings ui = ApexConfig::GetUi();
    int current = 0;
    for (int i = 0; i < IM_COUNTOF(kSizes); i++)
        if (std::fabs(kSizes[i] - ui.fontScale) < std::fabs(kSizes[current] - ui.fontScale)) current = i;
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    const float stepW = ImGui::GetFrameHeight() * 1.4f; // - and + are equally wide
    const float valueW = ImGui::CalcTextSize("200%").x + 2.0f * ApexUi::kSpace1 * ApexUi::Unit();
    const float resetW = ApexUi::ButtonWidth("Reset##TextSize", false);
    if (!ApexUi::BeginControlRow("Text size", "Makes the whole menu bigger or smaller", 2.0f * stepW + valueW + resetW + 3.0f * gap)) return;
    ImGui::BeginDisabled(current == 0);
    if (ApexUi::TextButton("-##TextSmaller", "Smaller", ButtonKind::Secondary, stepW)) {
        ui.fontScale = kSizes[current - 1];
        ApexConfig::SetUi(ui);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    {
        // The current size, centred between - and +
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float h = ImGui::GetFrameHeight();
        ImGui::Dummy(ImVec2(valueW, h));
        const ImVec2 ts = ImGui::CalcTextSize(kSizeNames[current]);
        ImGui::GetWindowDrawList()->AddText(ImVec2(p.x + (valueW - ts.x) * 0.5f, p.y + (h - ts.y) * 0.5f), ImGui::GetColorU32(ImGuiCol_Text), kSizeNames[current]);
    }
    ImGui::SameLine();
    ImGui::BeginDisabled(current == IM_COUNTOF(kSizes) - 1);
    if (ApexUi::TextButton("+##TextLarger", "Larger", ButtonKind::Secondary, stepW)) {
        ui.fontScale = kSizes[current + 1];
        ApexConfig::SetUi(ui);
    }
    ImGui::EndDisabled();
    ImGui::SameLine();
    ImGui::BeginDisabled(kSizes[current] == 1.0f);
    if (ApexUi::TextButton("Reset##TextSize", "Back to 100%")) {
        ui.fontScale = 1.0f;
        ApexConfig::SetUi(ui);
    }
    ImGui::EndDisabled();
    ApexUi::EndControlRow();
}

// Menu language: Automatic (Windows' display language), English, PortuguÃƒÂªs, EspaÃƒÂ±ol, FranÃƒÂ§ais (each in its own words)
void LanguageRow() {
    ApexConfig::UiSettings ui = ApexConfig::GetUi();
    const std::string automatic = I18n::Trf("Automatic ({})", I18n::NativeName(I18n::SystemLanguage()));
    const char* labels[] = {automatic.c_str(), I18n::NativeName(I18n::Lang::English), I18n::NativeName(I18n::Lang::Portuguese),
                            I18n::NativeName(I18n::Lang::Spanish), I18n::NativeName(I18n::Lang::French)};
    int current = std::clamp(ui.language + 1, 0, IM_ARRAYSIZE(labels) - 1); // -1 automatic -> 0
    ApexUi::SetChangeReporting(false); // a menu preference, not part of the undoable state
    if (ApexUi::SelectRow("Language", "The language of this menu", "##Language", &current, labels, IM_ARRAYSIZE(labels), 220.0f, 0)) {
        ui.language = current - 1;
        ApexConfig::SetUi(ui);
    }
    ApexUi::SetChangeReporting(true);
}

// "Saved to <file>" in the current language (the path is found once)
std::string AutosaveHint() {
    static const std::string path = ApexUtil::ToUtf8(ApexPaths::ConfigFile());
    return I18n::Trf("Saved to {}", path);
}

// Every feature back to its defaults (not the menu's preferences), after an inline confirmation;
// the undo toast brings the previous settings back
bool g_confirmResetAll = false;

void ResetAllRow() {
    const float resetW = ApexUi::ButtonWidth("Reset all", true), cancelW = ApexUi::ButtonWidth("Cancel##ResetAll", false);
    const float gap = ImGui::GetStyle().ItemSpacing.x;
    const char* description = g_confirmResetAll ? "Restore the whole mod? Captures, reports and saved profiles will stay"
                                                : "Restore features, colors, menu preferences and shortcuts. Saved files stay";
    if (!ApexUi::BeginControlRow("Reset all settings", description, g_confirmResetAll ? resetW + gap + cancelW : resetW)) return;
    if (!g_confirmResetAll) {
        ImGui::BeginDisabled(Loading());
        if (ApexUi::IconTextButton("Reset all", IconId::RotateCcw)) g_confirmResetAll = true;
        ImGui::EndDisabled();
    } else {
        if (ApexUi::IconTextButton("Reset all", IconId::RotateCcw, nullptr, ButtonKind::Primary)) {
            toml::table before, defaults;
            ApexConfig::CaptureFeatureState(before);
            ApexConfig::DefaultFeatureState(defaults);
            const auto previousUi = ApexConfig::GetUi();
            ApexConfig::ApplyFeatureState(defaults);
            ApexConfig::UiSettings uiDefaults;
            uiDefaults.startProfileDone = previousUi.startProfileDone; // the welcome is not repeated
            ApexConfig::SetUi(uiDefaults);
            LOG_INFO("[Menu] All settings reset to their defaults");
            ShowToast(I18n::Tr("All settings reset"), std::move(before), "All settings reset");
            g_toast.restoreUi = true;
            g_toast.undoUi = previousUi;
            g_confirmResetAll = false;
        }
        ImGui::SameLine();
        if (ApexUi::TextButton("Cancel##ResetAll")) g_confirmResetAll = false;
    }
    ApexUi::EndControlRow();
}

void SaveRow() {
    if (!ApexUi::BeginControlRow("Save settings", "Changes also save by themselves after a second", ApexUi::ButtonWidth("Save now", true))) return;
    const std::string hint = AutosaveHint();
    if (ApexUi::IconTextButton("Save now", IconId::Save, hint.c_str())) ApexConfig::Save();
    ApexUi::EndControlRow();
}

// A status row: label on the left, the value (muted, after an optional small icon; translated) on the right
void InfoRow(const char* label, const std::string& english, IconId icon = IconId::None, unsigned iconRgb = VioletTheme::kTextMuted) {
    const std::string value(I18n::Tr(std::string_view(english)));
    const float u = ApexUi::Unit();
    const float is = ApexUi::kIconSmall * u, ig = 6.0f * u;
    const bool withIcon = icon != IconId::None;
    const float valueW = ImGui::CalcTextSize(value.c_str()).x + (withIcon ? is + ig : 0.0f);
    if (!ApexUi::BeginControlRow(label, nullptr, std::min(valueW, ImGui::GetContentRegionAvail().x * 0.6f))) return;
    if (withIcon) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        const float h = ImGui::GetFrameHeight();
        ImGui::Dummy(ImVec2(is, h));
        ApexUi::DrawIcon(ImGui::GetWindowDrawList(), icon, ImVec2(p.x, p.y + (h - is) * 0.5f), is, ImGui::GetColorU32(Col(iconRgb)));
        ImGui::SameLine(0.0f, ig);
    }
    ImGui::AlignTextToFramePadding();
    ImGui::PushStyleColor(ImGuiCol_Text, Col(VioletTheme::kTextMuted));
    ImGui::TextUnformatted(value.c_str());
    ImGui::PopStyleColor();
    ApexUi::EndControlRow();
}

void CreditLine(const char* text) {
    ImGui::Bullet();
    ApexUi::MutedText(text);
}

bool g_developerConfirmRequested = false;
toml::table g_developerPendingProfile;
std::string g_developerPendingProfileName;
void DeveloperModeRow() {
    auto ui = ApexConfig::GetUi();
    bool enabled = ui.developerMode;
    if (ApexUi::SwitchRow("Enable developer mode", &enabled, "Advanced tools for testing and diagnostics. Requires restarting the game", false)) {
        if (enabled) { g_developerPendingProfile = {}; g_developerPendingProfileName.clear(); g_developerConfirmRequested = true; }
        else { ui.developerMode = false; ApexConfig::SetUi(ui); }
    }
    if (ui.developerMode != !kPublicBuild.load(std::memory_order_relaxed))
        ApexUi::IconNote(IconId::Info, "Restart the game to apply the developer mode change");
}
void DeveloperConfirmation() {
    if (g_developerConfirmRequested) {
        ImGui::OpenPopup("DeveloperModeConfirmation");
        g_developerConfirmRequested = false;
    }
    ImGui::SetNextWindowSize(ImVec2(500.0f * ApexUi::Unit(), 0), ImGuiCond_Appearing);
    if (ImGui::BeginPopupModal("DeveloperModeConfirmation", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_AlwaysAutoResize)) {
        ApexUi::CardHeader(IconId::TriangleAlert, "Enable developer mode?", "Use these tools only when you need to investigate a problem", nullptr, nullptr);
        ApexUi::CardDivider();
        ApexUi::MutedText("Diagnostic views can temporarily change the image. Measurements and extra checks can reduce performance while running.");
        ApexUi::MutedText("Captures and reports may contain your settings, local file paths and details about the current game session. Review them before sharing. Nothing is sent automatically.");
        ApexUi::MutedText("Restart the game after confirming. Measurements and recordings will not start automatically when you load a profile.");
        ApexUi::Gap(ApexUi::kSpace2);
        {
            const ApexUi::ControlSizeScope size(ApexUi::ControlSize::Primary);
            const bool oneRow = ReportDialogActions("Cancel", false, "Enable developer mode", true);
            if (ApexUi::TextButton("Cancel")) { g_developerPendingProfile = {}; ImGui::CloseCurrentPopup(); }
            ReportDialogLastAction("Enable developer mode", true, oneRow);
            if (ApexUi::IconTextButton("Enable developer mode", IconId::Wrench, nullptr, ButtonKind::Primary)) {
                toml::table before; ApexConfig::CaptureFeatureState(before);
                auto ui = ApexConfig::GetUi(); ui.developerMode = true; ApexConfig::SetUi(ui);
                if (!g_developerPendingProfile.empty()) {
                    ApexConfig::ApplyFeatureState(g_developerPendingProfile);
                    ShowToast(I18n::Tr("Profile loaded"), std::move(before), "Profile loaded: " + g_developerPendingProfileName);
                    g_developerPendingProfile = {};
                }
                ImGui::CloseCurrentPopup();
            }
        } // restore the frame style before EndPopup checks its stack
        ImGui::EndPopup();
    }
}

void MenuTab() {
    ImGui::PushID("Menu");
    if (ApexUi::BeginCard("##Card")) {
        ApexUi::CardHeader(IconId::Settings, "Menu", "Language, text size and startup notice", nullptr, nullptr);
        ApexUi::CardDivider();
        LanguageRow();
        TextSizeRow();
        {
            ApexConfig::UiSettings ui = ApexConfig::GetUi();
            if (ApexUi::SwitchRow("Startup menu hint", &ui.startNote, "Shows the menu shortcut when the game starts", true))
                ApexConfig::SetUi(ui); // [ui] start_note
        }
    }
    ApexUi::EndCard();
    ScreenshotCaptureCard();
    if (ApexUi::BeginCard("##Maintenance")) {
        ApexUi::CardHeader(IconId::Wrench, "Settings and maintenance", "Saving, reset and optional developer tools", nullptr, nullptr);
        ApexUi::CardDivider();
        SaveRow();
        DeveloperModeRow();
        ResetAllRow();
    }
    ApexUi::EndCard();
    ImGui::PopID();
}

// ---- Settings > Profiles ----

struct ProfileItem {
    std::string name;
    IconId icon = IconId::Bookmark;
    unsigned parts = 0; // what the file has (ApexConfig::ProfilePartsOf)
    int builtin = -1;   // ApexPresets index (shipped with the mod, no file, cannot be deleted); -1 = a file in the Profiles folder
    std::string Key() const { return builtin >= 0 ? std::format("#builtin{}", builtin) : name; } // file names cannot contain '#'
};
struct ProfilesState {
    char name[ApexConfig::kProfileNameMax + 1] = {};
    IconId icon = IconId::Bookmark;
    std::vector<ProfileItem> list;
    bool listDirty = true;
    unsigned saveParts = ApexConfig::kProfilePartsAll & ~ApexConfig::kPartShortcuts; // what "Save" writes (shortcuts: only when picked, they belong to the keyboard)
    std::string message; // the result of the last action
    bool messageError = false;
    std::string confirmDelete;  // a profile waiting for "Delete?"
    std::string confirmReplace; // a name that exists, waiting for "Replace?"
    std::string loading;        // a profile whose parts are being picked before "Load"
    unsigned loadParts = 0;
    double applyOpenedAt = 0.0;
};
ProfilesState g_profiles;

// ---- Welcome page: a new installation's first menu open offers a built-in profile to start with. Shown once: the first
// draw sets [ui] start_profile_done; it stays on screen this session until answered or another page is opened. ----
int g_welcomeState = -1;                     // -1 not decided yet, 0 not shown, 1 showing
int g_welcomeChoice = ApexPresets::kDefault; // the selected starting profile (Default, listed second)
bool WelcomeActive() {
    if (g_welcomeState < 0) g_welcomeState = ApexConfig::GetUi().startProfileDone ? 0 : 1;
    return g_welcomeState == 1;
}
void HideWelcome() { g_welcomeState = 0; }

// Only letters, digits, space, - and _ can be typed (the file name is the profile name)
int ProfileNameFilter(ImGuiInputTextCallbackData* data) {
    const ImWchar c = data->EventChar;
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '-' || c == '_';
    return ok ? 0 : 1;
}

void ProfileMessage(const std::string& text, bool error) {
    g_profiles.message = text;
    g_profiles.messageError = error;
}

// "Night Lights, Color, Window mode" in the menu's language
std::string ProfilePartsText(unsigned parts) {
    std::string s;
    for (int i = 0; i < ApexConfig::kProfilePartCount; i++)
        if (parts & (1u << i)) {
            if (!s.empty()) s += ", ";
            s += I18n::Tr(ApexConfig::ProfilePartName(i));
        }
    return s;
}

// Two aligned columns, collapsing to one when translated labels need more room.
// Presentation order is independent of the stable legacy category bits.
void ProfilePartChecks(const char* id, unsigned* parts, unsigned available) {
    ImGui::PushID(id);
    const float u = ApexUi::Unit();
    static constexpr int order[] = {0, 1, 7, 2, 3, 5, 6, 8};
    static constexpr IconId icons[] = {IconId::MoonStar, IconId::Palette, IconId::Contrast, IconId::Aperture,
                                      IconId::Spline, IconId::Gauge, IconId::Keyboard, IconId::Wrench};
    float longest = 0.0f;
    for (int i : order) if (available & (1u << i))
        longest = std::max(longest, ImGui::CalcTextSize(I18n::Tr(ApexConfig::ProfilePartName(i))).x);
    const float gutter = ApexUi::kSpace4 * u;
    const int columns = ImGui::GetContentRegionAvail().x >= 2.0f * (longest + 76.0f * u) + gutter ? 2 : 1;
    ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(gutter * 0.5f, 0.0f));
    if (ImGui::BeginTable("##Parts", columns, ImGuiTableFlags_SizingStretchSame)) {
        int visibleParts = 0;
        for (int part : order) if (available & (1u << part)) ++visibleParts;
        int shown = 0;
        for (int index = 0; index < IM_COUNTOF(order); ++index) {
            const int i = order[index];
            const unsigned bit = 1u << i;
            if (!(available & bit)) continue;
            ImGui::TableNextColumn();
            ImGui::PushID(i);
            const float width = ImGui::GetContentRegionAvail().x;
            const float height = std::max(40.0f * u, ImGui::GetTextLineHeight() + 16.0f * u);
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const bool on = (*parts & bit) != 0;
            if (ImGui::InvisibleButton("##Part", ImVec2(width, height), ImGuiButtonFlags_EnableNav))
                *parts = on ? (*parts & ~bit) : (*parts | bit);
            const bool checked = (*parts & bit) != 0;
            auto* draw = ImGui::GetWindowDrawList();
            if (ImGui::IsItemHovered()) draw->AddRectFilled(p, ImVec2(p.x + width, p.y + height), ImGui::GetColorU32(Col(VioletTheme::kHoverBg)), 4.0f * u);
            const float iconSize = ApexUi::kIconMedium * u;
            ApexUi::DrawIcon(draw, icons[index], ImVec2(p.x, p.y + (height - iconSize) * 0.5f), iconSize,
                             ImGui::GetColorU32(Col(VioletTheme::kAccent)));
            const char* label = I18n::Tr(ApexConfig::ProfilePartName(i));
            draw->AddText(ImVec2(p.x + iconSize + ApexUi::kSpace3 * u, p.y + (height - ImGui::GetTextLineHeight()) * 0.5f),
                          ImGui::GetColorU32(Col(VioletTheme::kText)), label);
            const float box = std::max(VioletTheme::kCheckboxSize * u, ImGui::GetFontSize());
            const ImVec2 check(std::round(p.x + width - box), std::round(p.y + (height - box) * 0.5f));
            const ImU32 frameColor = ImGui::GetColorU32(ImGui::IsItemActive() && ImGui::IsItemHovered() ? ImGuiCol_FrameBgActive :
                                                       ImGui::IsItemHovered() ? ImGuiCol_FrameBgHovered : ImGuiCol_FrameBg);
            ImGui::RenderFrame(check, ImVec2(check.x + box, check.y + box), frameColor, true, ImGui::GetStyle().FrameRounding);
            if (checked) {
                const float padding = std::max(1.0f, std::floor(box / 6.0f));
                ImGui::RenderCheckMark(draw, ImVec2(check.x + padding, check.y + padding), ImGui::GetColorU32(ImGuiCol_CheckMark), box - 2.0f * padding);
            }
            ImGui::RenderNavCursor(ImGui::GetCurrentContext()->LastItemData.Rect, ImGui::GetItemID());
            if (shown / columns < (visibleParts - 1) / columns)
                draw->AddLine(ImVec2(p.x, std::floor(p.y + height)), ImVec2(p.x + width, std::floor(p.y + height)), ImGui::GetColorU32(Col(VioletTheme::kCardBorder)));
            ++shown;
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::PopStyleVar();
    ImGui::PopID();
}

// Opens the Profiles folder in Explorer (to copy profiles to another PC or share them), on a short-lived thread
void OpenProfilesFolder() {
    ApexConfig::EnsureProfilesDirectory();
    std::thread([] {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        const std::wstring dir = ApexConfig::ProfilesFolder();
        const HINSTANCE r = ShellExecuteW(nullptr, L"open", dir.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(r) <= 32) LOG_WARNING(std::format("[Menu] Could not open the Profiles folder ({})", reinterpret_cast<INT_PTR>(r)));
        if (SUCCEEDED(com)) CoUninitialize();
    }).detach();
}

void SaveProfileNow(const std::string& name) {
    std::string err;
    if (ApexConfig::SaveProfile(name, g_profiles.saveParts, &err, ApexUi::IconName(g_profiles.icon))) {
        ProfileMessage(I18n::Trf("Saved \"{}\"", name), false);
        g_profiles.name[0] = '\0';
    } else {
        ProfileMessage(I18n::Trf("Could not save \"{}\": {}", name, err), true);
    }
    g_profiles.confirmReplace.clear();
    g_profiles.listDirty = true;
}

// name: the file's name or a built-in profile's English name (log); shown: as the menu shows it
void ApplyProfileState(const std::string& name, const std::string& shown, toml::table state, unsigned parts) {
    ApexConfig::KeepProfileParts(state, parts);
    if (const auto* d = state["developer"].as_table(); d && (*d)["enabled"].value_or(true) && !ApexConfig::GetUi().developerMode) {
        g_developerPendingProfile = std::move(state);
        g_developerPendingProfileName = name;
        g_developerConfirmRequested = true;
        return;
    }
    toml::table before;
    ApexConfig::CaptureFeatureState(before);
    ApexConfig::ApplyFeatureState(state);
    LOG_INFO(std::format("[Menu] Profile loaded: {} (parts {:#x})", name, parts));
    ProfileMessage(I18n::Trf("Loaded \"{}\"", shown), false);
    ShowToast(I18n::Tr("Profile loaded"), std::move(before), "Profile loaded: " + name);
}

void LoadProfileNow(const std::string& name, unsigned parts) {
    toml::table state;
    std::string err;
    if (!ApexConfig::ReadProfile(name, state, &err)) {
        ProfileMessage(I18n::Trf("Could not load \"{}\": {}", name, err), true);
        return;
    }
    ApplyProfileState(name, name, std::move(state), parts);
}

void LoadBuiltinProfileNow(int index, unsigned parts) {
    const ApexPresets::Preset& preset = ApexPresets::Get(index);
    toml::table state;
    std::string err;
    if (!ApexPresets::Read(index, state, &err)) {
        ProfileMessage(I18n::Trf("Could not load \"{}\": {}", I18n::Tr(preset.name), err), true);
        return;
    }
    ApplyProfileState(preset.name, I18n::Tr(preset.name), std::move(state), parts);
}

void LoadProfileItem(const ProfileItem& item, unsigned parts) {
    if (item.builtin >= 0) LoadBuiltinProfileNow(item.builtin, parts);
    else LoadProfileNow(item.name, parts);
}

// Uses stable Lucide names in metadata; enum positions never enter saved files.
void ProfileIconPicker(IconId& selected) {
    const float u = ApexUi::Unit();
    const float h = ImGui::GetFrameHeight();
    const float size = VioletTheme::kControlIcon * u;
    const float arrow = 10.0f * u;
    const float gap = VioletTheme::kControlIconGap * u, padding = ImGui::GetStyle().FramePadding.x;
    const float width = padding * 2.0f + size + gap + arrow;
    if (ImGui::Button("##ProfileIcon", ImVec2(width, h))) ImGui::OpenPopup("##ProfileIcons");
    // Button placement may include baseline alignment; anchor to its submitted rectangle.
    const ImVec2 p = ImGui::GetItemRectMin();
    const ImVec2 end = ImGui::GetItemRectMax();
    const float centerY = (p.y + end.y) * 0.5f;
    ApexUi::DrawIcon(ImGui::GetWindowDrawList(), selected, ImVec2(p.x + padding, centerY - size * 0.5f), size,
                     ImGui::GetColorU32(Col(VioletTheme::kAccent)));
    ApexUi::DrawIcon(ImGui::GetWindowDrawList(), IconId::ChevronDown,
                     ImVec2(p.x + padding + size + gap, centerY - arrow * 0.5f), arrow,
                     ImGui::GetColorU32(Col(VioletTheme::kTextMuted)));
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", I18n::Tr("Choose a profile icon"));
    if (ImGui::BeginPopup("##ProfileIcons")) {
        {
            const ApexUi::ControlSizeScope sizeScope(ApexUi::ControlSize::Compact);
            static constexpr IconId choices[] = {
                IconId::House, IconId::Armchair, IconId::Fence, IconId::LandPlot, IconId::Trees, IconId::Flower2,
                IconId::Leaf, IconId::Cat, IconId::Dog, IconId::UserRound, IconId::Heart, IconId::Coffee,
                IconId::Music, IconId::Sun, IconId::Moon, IconId::Cloud, IconId::Snowflake, IconId::Droplet,
                IconId::Camera, IconId::Image, IconId::Palette, IconId::Lightbulb, IconId::Diamond, IconId::Bookmark,
                IconId::Skull, IconId::CloudMoon, IconId::MoonStar, IconId::SunMedium, IconId::Sparkles, IconId::Flame,
                // home, Sims life and light (2026-10-05)
                IconId::Bed, IconId::Sofa, IconId::Bath, IconId::Lamp, IconId::LampDesk, IconId::LampFloor,
                IconId::LampCeiling, IconId::ChefHat, IconId::Utensils, IconId::Cake, IconId::Wine, IconId::Gift,
                IconId::Baby, IconId::Users, IconId::Shirt, IconId::PawPrint, IconId::Fish, IconId::Sprout,
                IconId::Guitar, IconId::BookOpen, IconId::Briefcase, IconId::GraduationCap, IconId::Car, IconId::PartyPopper,
                IconId::TreePalm, IconId::Tent, IconId::Sunset, IconId::CloudRain, IconId::Crown, IconId::WandSparkles
            };
            for (int i = 0; i < IM_COUNTOF(choices); ++i) {
                if (i % 6) ImGui::SameLine();
                const IconId id = choices[i];
                if (ApexUi::IconButton(ApexUi::IconName(id), id, ApexUi::IconName(id), selected == id, VioletTheme::kControlCompact)) {
                    selected = id;
                    ImGui::CloseCurrentPopup();
                }
            }
        } // restore the frame style before EndPopup checks its stack
        ImGui::EndPopup();
    }
}

void ProfilesTab() {
    ProfilesState& s = g_profiles;
    if (s.listDirty) {
        s.list.clear();
        for (const std::string& name : ApexConfig::ListProfiles()) {
            ProfileItem item{name, IconId::Bookmark, 0};
            toml::table state;
            if (ApexConfig::ReadProfile(name, state)) {
                item.parts = ApexConfig::ProfilePartsOf(state);
                item.icon = ApexUi::IconFromName(state["meta"]["icon"].value_or(std::string("bookmark")));
            }
            s.list.push_back(std::move(item));
        }
        for (int i = 0; i < ApexPresets::kCount; ++i) { // after the player's own, the built-in profiles in the welcome page's order
            const ApexPresets::Preset& preset = ApexPresets::Get(i);
            ProfileItem item{preset.name, ApexUi::IconFromName(preset.icon), 0, i};
            toml::table state;
            if (ApexPresets::Read(i, state)) item.parts = ApexConfig::ProfilePartsOf(state);
            s.list.push_back(std::move(item));
        }
        s.listDirty = false;
    }
    const float u = ApexUi::Unit();
    ImGui::PushID("Profiles");
    if (ApexUi::BeginCard("##Card")) {
        ApexUi::CardHeader(IconId::Bookmark, "Profiles", "Save your setup and switch between them", nullptr, nullptr);
        ApexUi::CardDivider();
        ApexUi::MutedText("Choose the settings to include in this profile.");
        const unsigned saveAvailable = ApexConfig::kProfilePartsAll & (ApexConfig::GetUi().developerMode ? ~0u : ~ApexConfig::kPartDeveloper);
        s.saveParts &= saveAvailable;
        if (!ApexUi::FilterActive()) ProfilePartChecks("SaveParts", &s.saveParts, saveAvailable);
        ApexUi::Gap(ApexUi::kSpace3);
        {
            const ApexUi::ControlSizeScope size(ApexUi::ControlSize::Primary);
            const float saveW = ApexUi::ButtonWidth("Save##Profile", true);
            const float gap = ImGui::GetStyle().ItemSpacing.x;
            const bool inlineSave = ImGui::GetContentRegionAvail().x >= 72.0f * u + 80.0f * u + saveW + 2.0f * gap;
            ProfileIconPicker(s.icon);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(std::fmax(ImGui::GetContentRegionAvail().x - (inlineSave ? saveW + gap : 0.0f), 80.0f * u));
            const bool enter = ImGui::InputTextWithHint("##ProfileName", I18n::Tr("Profile name"), s.name, sizeof s.name,
                                                        ImGuiInputTextFlags_CallbackCharFilter | ImGuiInputTextFlags_EnterReturnsTrue, ProfileNameFilter);
            if (inlineSave) ImGui::SameLine();
            const std::string clean = ApexConfig::SanitizeProfileName(s.name);
            const bool canSave = !clean.empty() && s.saveParts != 0 && !Loading();
            ImGui::BeginDisabled(!canSave);
            const bool save = ApexUi::IconTextButton("Save##Profile", IconId::Save, s.saveParts == 0 ? "Pick at least one part to save" : nullptr, ButtonKind::Primary);
            ImGui::EndDisabled();
            if ((save || (enter && !clean.empty())) && canSave) {
                if (ApexConfig::ProfileExists(clean) && s.confirmReplace != clean) s.confirmReplace = clean;
                else SaveProfileNow(clean);
            }
        } // primary profile form
        ApexUi::Gap(ApexUi::kSpace1);
        ApexUi::MutedText("Shortcuts are optional and start unchecked.");
        if (!s.confirmReplace.empty()) {
            const std::string q = I18n::Trf("\"{}\" already exists; replace it?", s.confirmReplace);
            ApexUi::IconNote(IconId::TriangleAlert, q.c_str(), VioletTheme::kWarning);
            ApexUi::Gap(ApexUi::kSpace1);
            if (ApexUi::TextButton("Replace##Profile", nullptr, ButtonKind::Primary)) SaveProfileNow(s.confirmReplace);
            ImGui::SameLine();
            if (ApexUi::TextButton("Cancel##Replace")) s.confirmReplace.clear();
        }
        if (!s.message.empty()) {
            ApexUi::Gap(ApexUi::kSpace1);
            ApexUi::IconNote(s.messageError ? IconId::TriangleAlert : IconId::CircleCheck, s.message.c_str(), s.messageError ? VioletTheme::kError : VioletTheme::kTextMuted);
        }

    }
    ApexUi::EndCard();
    if (ApexUi::BeginCard("##SavedProfiles")) {
        const float gap = ImGui::GetStyle().ItemSpacing.x;
        ApexUi::CardHeader(IconId::Layers, "Saved profiles", "Choose which saved settings to apply", nullptr, nullptr);
        ApexUi::CardDivider();
        const bool ownProfiles = s.list.size() > static_cast<size_t>(ApexPresets::kCount); // the player's files first, then the built-in ones
        for (const ProfileItem& item : s.list) {
            const std::string& name = item.name;
            const std::string key = item.Key();
            const bool builtin = item.builtin >= 0;
            if (ownProfiles && &item == &s.list.front()) ApexUi::GroupLabel("YOUR PROFILES");
            if (ownProfiles && item.builtin == 0) ApexUi::GroupLabel("BUILT-IN");
            ImGui::PushID(key.c_str());
            const bool confirming = !builtin && s.confirmDelete == key;
            const bool picking = s.loading == key;
            const float loadW = ApexUi::ButtonWidth("Apply", true), delW = ApexUi::ButtonWidth("Delete", true);
            const float controlsW = picking ? 0.0f : builtin ? loadW : delW + gap + loadW;
            const std::string partsText = item.parts ? ProfilePartsText(item.parts) : std::string(I18n::Tr("Nothing this version can load"));
            const char* description = confirming ? "Delete this profile?" : picking ? "Choose what to apply"
                                    : builtin ? ApexPresets::Get(item.builtin).description : partsText.c_str();
            if (!builtin) ApexUi::SetNextRowUntranslated(); // the name is the user's; built-in names and descriptions are translated
            if (ApexUi::BeginControlRow(name.c_str(), description, controlsW, item.icon)) {
                if (confirming) {
                    if (ApexUi::IconTextButton("Delete##Confirm", IconId::Trash2, "Deletes the profile file", ButtonKind::Primary)) {
                        std::string err;
                        if (ApexConfig::DeleteProfile(name, &err)) ProfileMessage(I18n::Trf("Deleted \"{}\"", name), false);
                        else ProfileMessage(I18n::Trf("Could not delete \"{}\": {}", name, err), true);
                        s.confirmDelete.clear();
                        s.listDirty = true;
                    }
                    ImGui::SameLine();
                    if (ApexUi::TextButton("Cancel", nullptr, ButtonKind::Secondary, loadW)) s.confirmDelete.clear();
                } else if (!picking) {
                    if (!builtin) {
                        if (ApexUi::IconTextButton("Delete", IconId::Trash2)) {
                            s.confirmDelete = key;
                            s.loading.clear();
                        }
                        ImGui::SameLine();
                    }
                    ImGui::BeginDisabled(Loading() || item.parts == 0);
                    if (ApexUi::IconTextButton("Apply", IconId::Download, "Pick which parts of this profile to apply")) {
                        s.loading = key;
                        s.applyOpenedAt = ImGui::GetTime();
                        s.loadParts = item.parts & ~(ApexConfig::kPartShortcuts | ApexConfig::kPartDeveloper); // shortcuts only when picked (they belong to the keyboard)
                        s.confirmDelete.clear();
                    }
                    ImGui::EndDisabled();
                }
                ApexUi::EndControlRow();
                if (picking) {
                    ApexUi::Gap(ApexUi::kSpace2);
                    const float reveal = std::clamp(static_cast<float>((ImGui::GetTime() - s.applyOpenedAt) / 0.14), 0.0f, 1.0f);
                    const float easedReveal = reveal * reveal * (3.0f - 2.0f * reveal);
                    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * (0.25f + 0.75f * easedReveal));
                    if (ApexUi::BeginCard("##ApplySelection")) {
                        unsigned selected = 0, total = 0;
                        for (int part : {0, 1, 7, 2, 3, 5, 6, 8}) {
                            const unsigned bit = 1u << part;
                            if (item.parts & bit) { ++total; if (s.loadParts & bit) ++selected; }
                        }
                        const std::string count = I18n::Trf("{} of {} selected", selected, total);
                        ImGui::TextUnformatted(I18n::Tr("Settings to apply"));
                        ImGui::SameLine();
                        ImGui::SetCursorPosX(ImGui::GetWindowContentRegionMax().x - ImGui::CalcTextSize(count.c_str()).x);
                        ImGui::TextDisabled("%s", count.c_str());
                        ApexUi::CardDivider();
                        {
                            ApexUi::ControlSizeScope gridSize(ApexUi::ControlSize::Compact);
                            ProfilePartChecks("LoadParts", &s.loadParts, item.parts);
                        }
                        ApexUi::CardDivider();
                        {
                            ApexUi::ControlSizeScope footerSize(ApexUi::ControlSize::Compact);
                            const float actionsW = ApexUi::ButtonWidth("Cancel", false) + gap + ApexUi::ButtonWidth("Apply", true);
                            const float footerHeight = ImGui::GetFrameHeight();
                            ImGui::PushFont(VioletTheme::RegularFont(), VioletTheme::BaseFontSize() * ApexUi::kSmallScale); // same regular face and scale as secondary descriptions
                            ImGui::PushStyleColor(ImGuiCol_Text, Col(VioletTheme::kTextMuted));
                            const bool footerVisible = ApexUi::BeginControlRow("Unchecked settings stay as they are", nullptr, actionsW, IconId::None, footerHeight);
                            ImGui::PopStyleColor();
                            ImGui::PopFont();
                            if (footerVisible) {
                                if (ApexUi::TextButton("Cancel##Load")) s.loading.clear();
                                ImGui::SameLine();
                                ImGui::BeginDisabled(Loading() || s.loadParts == 0);
                                if (ApexUi::IconTextButton("Apply##Picked", IconId::Download, "Apply the checked parts; Undo puts your settings back", ButtonKind::Primary)) {
                                    LoadProfileItem(item, s.loadParts);
                                    s.loading.clear();
                                }
                                ImGui::EndDisabled();
                                ApexUi::EndControlRow();
                            }
                        }
                    }
                    ApexUi::EndCard();
                    ImGui::PopStyleVar();
                }
            }
            ImGui::PopID();
        }
        ApexUi::Gap(ApexUi::kSpace2);
        if (ApexUi::IconTextButton("Open the Profiles folder", IconId::ExternalLink, "Copy profile files from there to share them or to use them on another PC"))
            OpenProfilesFolder();
    }
    ApexUi::EndCard();
    ImGui::PopID();
}

void CompatibilityTab() {
    ImGui::PushID("Compatibility");
    if (ApexUi::BeginCard("##Card")) {
        ApexUi::CardHeader(IconId::Puzzle, "Compatibility", "Game version, other mods and settings", nullptr, nullptr);
        ApexUi::CardDivider();
        const Startup startup = g_startup.load();
        InfoRow("Game", GetGameVersionName());
        // Detected: a violet check before the value; missing: a muted info mark
        const bool s3ssLoaded = S3SSDetect::Scan().s3ssLoaded;
        InfoRow("Sims3SettingsSetter", s3ssLoaded ? "Installed" : "Not installed", s3ssLoaded ? IconId::CircleCheck : IconId::Info,
                s3ssLoaded ? VioletTheme::kAccent : VioletTheme::kTextMuted);
        const bool dxvk = DxvkLoaded();
        InfoRow("DXVK", dxvk ? "Installed" : "Not installed", dxvk ? IconId::CircleCheck : IconId::Info, dxvk ? VioletTheme::kAccent : VioletTheme::kTextMuted);
        InfoRow("Features", startup == Startup::Running ? "Running" : startup == Startup::Loading ? "Starting\xE2\x80\xA6" : "Off (old combined build found)");
        if (AnythingRecommended()) { // shown even after the Overview card and the start note were dismissed
            ApexUi::GroupLabel("RECOMMENDED");
            RecommendedItems();
        }
        if (ApexUi::BeginAdvanced("Details##Compatibility", "Details")) {
            ApexUi::MutedText(("Sims3SettingsSetter: " + S3SSDetect::Summary()).c_str());
            ApexUi::MutedText(I18n::Trf("Settings: {}", ApexConfig::MigrationNote()).c_str());
            ApexUi::EndAdvanced();
        }
    }
    ApexUi::EndCard();
    ImGui::PopID();
}

void AboutTab() {
    ImGui::PushID("About");
    if (ApexUi::BeginCard("##Card")) {
        const std::string versionLine = I18n::Trf(kPublicBuild ? "Version {}" : "Version {} - Developer mode", APEX_VERSION_STRING);
        ApexUi::CardHeader(IconId::Info, APEX_PRODUCT_NAME " " APEX_PRODUCT_TAGLINE, versionLine.c_str(), nullptr, nullptr);
        ApexUi::CardDivider();
        ApexUi::GroupLabel("CREDITS");
        CreditLine(APEX_PRODUCT_NAME " by @loinyx.");
        CreditLine("Sims3SettingsSetter by sims3fiend: project origin and framework reference.");
        CreditLine("FXAA 3.11: Timothy Lottes (NVIDIA).");
        CreditLine("FidelityFX CAS: AMD (MIT).");
        CreditLine("Upper-floor ground light: Arro's technique, adapted in Apex Radiance.");
        CreditLine("Libraries and licenses: Dear ImGui (MIT), Microsoft Detours (MIT), toml++ (MIT), SMAA - Jorge Jimenez et al. (MIT), Lucide icons (ISC).");
    }
    ApexUi::EndCard();
    ImGui::PopID();
}

void SettingsPage() {
    ApexUi::PageTitle("Settings", "Menu, profiles, compatibility and credits");
    static const char* const kTabs[] = {"Menu", "Shortcuts", "Profiles", "Compatibility", "About"};
    if (ApexUi::TabBar("##SettingsTabs", &g_settingsTab, kTabs, IM_COUNTOF(kTabs)) && g_settingsTab == SettingsProfiles) g_profiles.listDirty = true;
    switch (g_settingsTab) {
    case SettingsShortcuts: ShortcutsTab(); break;
    case SettingsProfiles: ProfilesTab(); break;
    case SettingsCompatibility: CompatibilityTab(); break;
    case SettingsAbout: AboutTab(); break;
    default: MenuTab(); break;
    }
}

// ---- search ----
// Every searchable part of the menu, one per page tab, in sidebar order. In the results each part is drawn in filter
// mode (ApexUi::BeginFilter): only its matching rows, each under the part's breadcrumb, which opens that page and tab.

struct SearchPart {
    const char* crumbPage; // the breadcrumb, "Page Ã¢â‚¬Âº Tab" (English keys, shown translated)
    const char* crumbTab;  // nullptr = the page alone
    int page;
    int* tab; // nullptr = a page without tabs
    int tabIndex;
    void (*draw)();
};

const SearchPart* SearchParts(int& count) {
    static const SearchPart kParts[] = {
        {"Lighting", "Overview", PageLighting, &g_lightingTab, LightingLamps, LampsTabContent},
        {"Lighting", "Ground", PageLighting, &g_lightingTab, LightingGround, GroundTabContent},
        {"Lighting", "Objects", PageLighting, &g_lightingTab, LightingObjects, ObjectsTabContent},
        {"Lighting", "Buildings", PageLighting, &g_lightingTab, LightingBuildings, BuildingsTabContent},
        {"Lighting", "Stories", PageLighting, &g_lightingTab, LightingStories, StoriesTabContent},
        {"Water & Snow", nullptr, PageWaterSnow, nullptr, 0, WaterSnowContent},
        {"Color", "Banding", PageColor, &g_colorTab, kColorBandingTab, BandingTabContent},
        {"Color", nullptr, PageColor, nullptr, 0, PictureHeaderCard},
        {"Color", "Basic", PageColor, &g_colorTab, Picture::TabBasic, [] { PictureRows(Picture::TabBasic); }},
        {"Color", "Tones", PageColor, &g_colorTab, Picture::TabTones, [] { PictureRows(Picture::TabTones); }},
        {"Color", "Color", PageColor, &g_colorTab, Picture::TabColor, [] { PictureRows(Picture::TabColor); }},
        {"Color", "Detail", PageColor, &g_colorTab, Picture::TabDetail, [] { PictureRows(Picture::TabDetail); }},
        {"Ambient Occlusion", nullptr, PageAmbientOcclusion, nullptr, 0, AmbientOcclusionContent},
        {"Depth Blur", nullptr, PageDepthBlur, nullptr, 0, DepthBlurContent},
        {"Edge Smoothing", nullptr, PageEdgeSmoothing, nullptr, 0, AntiAliasingContent},
        {"Performance", nullptr, PagePerformance, nullptr, 0, PerformanceCard},
        {"Settings", "Menu", PageSettings, &g_settingsTab, SettingsMenu, MenuTab},
        {"Settings", "Shortcuts", PageSettings, &g_settingsTab, SettingsShortcuts, ShortcutsTab},
    };
    count = IM_COUNTOF(kParts);
    return kParts;
}

void SearchResults() {
    ApexUi::PageTitle("Search", "Settings that match; click a page name to open it");
    ImGui::PushID("SearchResults");
    int clicked = -1, drawn = 0;
    if (ApexUi::BeginCard("##Card")) {
        int count = 0;
        const SearchPart* parts = SearchParts(count);
        ApexUi::BeginFilter(g_search);
        for (int i = 0; i < count; i++) {
            ImGui::PushID(i);
            std::string crumb = I18n::Tr(parts[i].crumbPage);
            if (parts[i].crumbTab) crumb += std::string(" \xE2\x80\xBA ") + I18n::Tr(parts[i].crumbTab);
            ApexUi::SetFilterCrumb(crumb.c_str(), i);
            parts[i].draw();
            ImGui::PopID();
        }
        drawn = ApexUi::EndFilter(&clicked);
        if (drawn == 0) {
            const std::string none = I18n::Trf("No settings match \"{}\"", static_cast<const char*>(g_search));
            ApexUi::MutedText(none.c_str());
        }
        if (clicked >= 0 && clicked < count) {
            Go(parts[clicked].page, parts[clicked].tab, parts[clicked].tabIndex);
            g_search[0] = '\0';
        }
    }
    ApexUi::EndCard();
    ImGui::PopID();
}

// ---- window parts ----

// The search field in the header: a search icon inside, the "Ctrl+F" hint while empty, a clear button while not
void SearchBox(float x, float y, float width) {
    const float u = ApexUi::Unit();
    const float h = ImGui::GetFrameHeight();
    const float is = VioletTheme::kControlIcon * u;
    const bool hasText = g_search[0] != '\0';
    const float clearW = hasText ? h : 0.0f;
    ImGui::SetCursorPos(ImVec2(x, y));
    const ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(ApexUi::kSpace3 * u + is + ApexUi::kSpace2 * u, ImGui::GetStyle().FramePadding.y));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, h * 0.5f);
    ImGui::SetNextItemWidth(std::fmax(width - clearW, 40.0f * u));
    if (g_focusSearch) {
        ImGui::SetKeyboardFocusHere();
        g_focusSearch = false;
    }
    const char* placeholder = I18n::Tr("Search settings");
    ImGui::InputTextWithHint("##Search", placeholder, g_search, sizeof g_search, ImGuiInputTextFlags_EscapeClearsAll | ImGuiInputTextFlags_AutoSelectAll);
    const bool active = ImGui::IsItemActive();
    ImGui::PopStyleVar(2);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 muted = ImGui::GetColorU32(Col(VioletTheme::kTextMuted));
    ApexUi::DrawIcon(dl, IconId::Search, ImVec2(p.x + ApexUi::kSpace3 * u, p.y + (h - is) * 0.5f), is, active ? ImGui::GetColorU32(Col(VioletTheme::kAccentLight)) : muted);
    if (!hasText && !active) {
        ImGui::PushFont(nullptr, VioletTheme::BaseFontSize() * ApexUi::kSmallScale);
        const char* hint = "Ctrl+F";
        const ImVec2 ts = ImGui::CalcTextSize(hint);
        const float hx = p.x + width - ts.x - 10.0f * u;
        if (hx > p.x + ApexUi::kSpace3 * u + is + ApexUi::kSpace2 * u + ImGui::CalcTextSize(placeholder).x + 8.0f * u) // only when it fits after the placeholder
            dl->AddText(ImVec2(hx, p.y + (h - ts.y) * 0.5f), muted, hint);
        ImGui::PopFont();
    }
    if (hasText) {
        ImGui::SameLine(0.0f, 0.0f);
        if (ApexUi::IconButton("##ClearSearch", IconId::X, "Clear the search (Esc)", false, h / u)) g_search[0] = '\0';
    }
}

// Logo tile, name and tagline (left); the search field, night/day and frame-time pills and close (right), all centred
// on the logo tile. Returns false when the close button was pressed.
bool Header() {
    const float u = ApexUi::Unit();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float startX = ImGui::GetCursorPosX(), startY = ImGui::GetCursorPosY();
    const float width = ImGui::GetContentRegionAvail().x;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float tile = 32.0f * u;

    // Logo (ui/logo.h); the plain tile with the letter when its texture could not be made
    if (!ApexUi::DrawLogo(dl, p, ImVec2(p.x + tile, p.y + tile))) {
        dl->AddRectFilled(p, ImVec2(p.x + tile, p.y + tile), ImGui::GetColorU32(Col(VioletTheme::kAccent)), ApexUi::kSpace2 * u);
        ImGui::PushFont(VioletTheme::BoldFont(), VioletTheme::BaseFontSize() * 1.3f);
        const ImVec2 letter = ImGui::CalcTextSize(APEX_LOGO_LETTER);
        dl->AddText(ImVec2(p.x + (tile - letter.x) * 0.5f, p.y + (tile - letter.y) * 0.5f), IM_COL32_WHITE, APEX_LOGO_LETTER);
        ImGui::PopFont();
    }
    ImGui::Dummy(ImVec2(tile, tile));
    ImGui::SameLine(0.0f, ApexUi::kSpace3 * u);

    // Name and tagline, centred on the tile (saving is shown in the status bar)
    const float nameH = ImGui::GetFontSize() * 1.15f, tagH = ImGui::GetFontSize();
    ImGui::SetCursorPosY(startY + std::fmax(0.0f, (tile - nameH - tagH) * 0.5f));
    ImGui::BeginGroup();
    ImGui::PushStyleVarY(ImGuiStyleVar_ItemSpacing, 0.0f);
    ImGui::PushFont(VioletTheme::BoldFont(), VioletTheme::BaseFontSize() * 1.15f);
    ImGui::TextUnformatted(APEX_PRODUCT_NAME);
    ImGui::PopFont();
    ImGui::PushStyleColor(ImGuiCol_Text, Col(VioletTheme::kTextMuted));
    ImGui::TextUnformatted(I18n::Tr(APEX_PRODUCT_TAGLINE));
    ImGui::PopStyleColor();
    ImGui::PopStyleVar();
    ImGui::EndGroup();
    const float nameRight = ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x + ImGui::GetScrollX() + ApexUi::kSpace3 * u;

    // Right side, laid out from the right edge: [search] [night/day] [frame time] [close]
    const float gap = 6.0f * u;
    const float button = 26.0f * u;
    const float closeX = startX + width - button;

    const ImGuiIO& io = ImGui::GetIO();
    char perf[48];
    const float fps = io.Framerate;
    std::snprintf(perf, sizeof perf, "%.1f ms \xC2\xB7 %.0f fps", fps > 0.0f ? 1000.0f / fps : 0.0f, fps);
    const ImVec2 perfSize = ApexUi::PillSize(perf, false);
    const float perfX = closeX - gap - perfSize.x;

    float level = 0.0f;
    const bool haveLevel = NightLighting::MenuNightLevel(level);
    const bool night = level > 0.5f;
    const char* dayText = night ? "Night" : "Day";
    const ImVec2 daySize = ApexUi::PillSize(dayText, true);
    const float dayX = perfX - gap - daySize.x;

    // The search field gets the room left of the pills; narrow windows drop the pills first
    const float minSearch = 110.0f * u, maxSearch = 220.0f * u;
    bool showDay = haveLevel, showPerf = true;
    auto leftEdge = [&] { return showDay ? dayX : showPerf ? perfX : closeX; };
    if (leftEdge() - gap - nameRight < minSearch) showDay = false;
    if (leftEdge() - gap - nameRight < minSearch) showPerf = false;
    const float searchW = std::fmin(leftEdge() - gap - nameRight, maxSearch);

    if (showDay) {
        ImGui::SetCursorPos(ImVec2(dayX, startY + (tile - daySize.y) * 0.5f));
        ApexUi::Pill(dayText, night, night ? IconId::Moon : IconId::Sun);
        ApexUi::Tooltip(I18n::Trf("How dark the game thinks it is: {:.2f} (0 is day, 1 is night)", level).c_str());
    }
    if (showPerf) {
        ImGui::SetCursorPos(ImVec2(perfX, startY + (tile - perfSize.y) * 0.5f));
        ApexUi::Pill(perf, false);
        ApexUi::Tooltip("Frame time and frame rate, averaged over recent frames");
    }
    if (searchW >= 60.0f * u) {
        SearchBox(leftEdge() - gap - searchW, startY + (tile - ImGui::GetFrameHeight()) * 0.5f, searchW);
    }
    bool keepOpen = true;
    ImGui::SetCursorPos(ImVec2(closeX, startY + (tile - button) * 0.5f));
    const std::string closeTip = I18n::Trf("Close (Esc); {} opens it again", ApexConfig::KeyChordText(ApexConfig::GetUi().toggle));
    if (ApexUi::IconButton("##Close", IconId::X, closeTip.c_str(), false, 26.0f)) keepOpen = false;
    // The next item starts below the tile
    ImGui::SetCursorPos(ImVec2(startX, startY + tile));
    ImGui::Dummy(ImVec2(0.0f, 0.0f));
    return keepOpen;
}

void Sidebar(bool collapsed) {
    struct Item {
        int page;
        IconId icon;
        const char* label;
        const char* group; // a group label starts before this item
    };
    static const Item items[] = {
        {PageOverview, IconId::LayoutDashboard, "Overview", nullptr},
        {PageLighting, IconId::MoonStar, "Lighting", "WORLD"},
        {PageWaterSnow, IconId::WavesHorizontal, "Water & Snow", nullptr},
        {PageColor, IconId::Palette, "Color", "IMAGE"},
        {PageAmbientOcclusion, IconId::Contrast, "Ambient Occlusion", nullptr},
        {PageDepthBlur, IconId::Aperture, "Depth Blur", nullptr},
        {PageEdgeSmoothing, IconId::Spline, "Edge Smoothing", "SYSTEM"},
        {PagePerformance, IconId::Gauge, "Performance", nullptr},
        {PageConflicts, IconId::TriangleAlert, "Attention", nullptr},
        {PageReport, IconId::Bug, "Report a problem", nullptr},
        {PageDeveloper, IconId::Wrench, "Developer", nullptr},
        {PageSettings, IconId::Settings, "Settings", nullptr},
    };
    const float u = ApexUi::Unit();
    const bool searching = g_search[0] != '\0';
    ImGui::PushStyleVarY(ImGuiStyleVar_ItemSpacing, 2.0f * u);
    for (const Item& item : items) {
        if (kPublicBuild && item.page == PageDeveloper) continue;
        if (item.page == PageConflicts && !HasAttentionItems() && g_page != PageConflicts) continue;
        if (item.group) ApexUi::SidebarGroup(item.group, collapsed);
        if (ApexUi::SidebarItem(item.icon, item.label, g_page == item.page && !searching && !WelcomeActive(), collapsed)) {
            g_page = item.page;
            HideWelcome(); // another page: the welcome is not shown again
            g_search[0] = '\0'; // leaving the search results
        }
    }
    ImGui::PopStyleVar();

    // Footer: the collapse button and (expanded) the version, at the bottom when there is room
    const float bs = 26.0f;
    const float footerY = ImGui::GetWindowHeight() - bs * u - ApexUi::kSpace1 * u;
    if (footerY > ImGui::GetCursorPosY() + ApexUi::kSpace1 * u) {
        ImGui::SetCursorPos(ImVec2(collapsed ? (ImGui::GetWindowWidth() - bs * u) * 0.5f : ApexUi::kSpace1 * u, footerY));
        if (ApexUi::IconButton("##CollapseSidebar", collapsed ? IconId::ChevronsRight : IconId::ChevronsLeft, collapsed ? "Expand the sidebar" : "Collapse the sidebar",
                               false, bs)) {
            ApexConfig::UiSettings ui = ApexConfig::GetUi();
            ui.sidebarCollapsed = !collapsed; // [ui] sidebar_collapsed
            ApexConfig::SetUi(ui);
        }
        if (!collapsed) {
            ImGui::PushFont(nullptr, VioletTheme::BaseFontSize() * ApexUi::kSmallScale);
            const float textY = footerY + (bs * u - ImGui::GetTextLineHeight()) * 0.5f;
            ImGui::SetCursorPos(ImVec2(ApexUi::kSpace1 * u + bs * u + ApexUi::kSpace1 * u, textY));
            ImGui::PushStyleColor(ImGuiCol_Text, Col(VioletTheme::kTextMuted));
            ImGui::TextUnformatted(I18n::Trf("Version {}", APEX_VERSION_STRING).c_str());
            ImGui::PopStyleColor();
            ImGui::PopFont();
        }
    }
}


// Queried only while drawing the open menu; the backbuffer reflects actual MSAA after reset.
void GameAaCompatibilityNotice() {
    g_menuGameAaOn = false;
    if (Loading()) return;
    auto* device = ApexD3D::Device();
    if (!device) return;
    IDirect3DSurface9* backbuffer = nullptr;
    if (FAILED(device->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &backbuffer)) || !backbuffer) return;
    D3DSURFACE_DESC desc{};
    const HRESULT result = backbuffer->GetDesc(&desc);
    backbuffer->Release();
    if (FAILED(result) || desc.MultiSampleType == D3DMULTISAMPLE_NONE) return;
    g_menuGameAaOn = true;
    const auto active = [](const char* name) { auto* p = Find(name); return p && p->IsEnabled(); };
    const bool aa = active("EdgeSmoothing"), blur = active("DepthBlur"), ao = active("AmbientOcclusion");
    const bool water = active(kNightLighting) && NightLighting::ShoreReflection() > 0.0f;
    const bool overview = g_page == PageOverview || g_page == PageConflicts;
    const bool relevant = overview ? (aa || blur || ao || water) :
        (g_page == PageDepthBlur && blur) || (g_page == PageAmbientOcclusion && ao) ||
        (g_page == PageEdgeSmoothing && aa) || (g_page == PageWaterSnow && water);
    if (!relevant) return;
    ImGui::PushID("GameAaCompatibility");
    if (ApexUi::BeginCard("##Warning", true)) {
        ApexUi::IconLabel(IconId::TriangleAlert, I18n::Tr("Turn off the game's Edge Smoothing"), ImGui::GetColorU32(Col(VioletTheme::kWarning)));
        ImGui::Spacing();
        ImGui::TextWrapped("%s", I18n::Tr("The Sims 3's Edge Smoothing prevents these enabled Apex effects from working:"));
        if (overview ? aa : g_page == PageEdgeSmoothing) ImGui::BulletText("%s", I18n::Tr("Edge Smoothing"));
        if (overview ? blur : g_page == PageDepthBlur) ImGui::BulletText("%s", I18n::Tr("Depth Blur"));
        if (overview ? ao : g_page == PageAmbientOcclusion) ImGui::BulletText("%s", I18n::Tr("Ambient Occlusion"));
        if (overview ? water : g_page == PageWaterSnow) ImGui::BulletText("%s", I18n::Tr("Water Reflections"));
        static bool instructions = false;
        if (ApexUi::IconTextButton("How to turn it off", IconId::Info, nullptr, ButtonKind::Primary)) instructions = !instructions;
        if (instructions) {
            ApexUi::CardDivider();
            ImGui::TextWrapped("%s", I18n::Tr("1. Open The Sims 3 menu and choose Options > Graphics."));
            ImGui::TextWrapped("%s", I18n::Tr("2. Set Edge Smoothing to Off and apply the change."));
            ImGui::TextWrapped("%s", I18n::Tr("3. Return to the game. Apex will check compatibility again."));
            ImGui::TextWrapped("%s", I18n::Tr("Your Apex settings are kept. This notice disappears when the conflict is resolved."));
        }
    }
    ApexUi::EndCard();
    ImGui::PopID();
}

void MarkWelcomeShown() {
    ApexConfig::UiSettings ui = ApexConfig::GetUi();
    if (ui.startProfileDone) return;
    ui.startProfileDone = true; // [ui] start_profile_done: never shown again
    ApexConfig::SetUi(ui);
}

// The content area while the welcome is pending: one card with the built-in profiles (the same choice rows as the
// lighting balance) and two actions. Apply goes through the Saved profiles path, so Undo is offered.
void WelcomePage() {
    MarkWelcomeShown();
    ApexUi::PageTitle("Welcome to " APEX_PRODUCT_NAME, "Choose how you want to start");
    ImGui::PushID("Welcome");
    if (ApexUi::BeginCard("##Card")) {
        ApexUi::CardHeader(IconId::None, "Starting profile", "You can switch later in Settings \xE2\x80\xBA Profiles", nullptr, nullptr);
        ApexUi::CardDivider();
        for (int i = 0; i < ApexPresets::kCount; ++i) {
            const ApexPresets::Preset& preset = ApexPresets::Get(i);
            ImGui::PushID(i);
            if (ApexUi::ProfileChoiceRow("Preset", ApexUi::IconFromName(preset.icon), preset.name, preset.description, g_welcomeChoice == i))
                g_welcomeChoice = i;
            ImGui::PopID();
        }
        ApexUi::CardDivider();
        ApexUi::Gap(ApexUi::kSpace2);
        {
            const ApexUi::ControlSizeScope size(ApexUi::ControlSize::Primary);
            const float gap = ImGui::GetStyle().ItemSpacing.x;
            const float actionsW = ApexUi::ButtonWidth("Keep as it is", false) + gap + ApexUi::ButtonWidth("Apply##Welcome", true);
            ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::fmax(0.0f, ImGui::GetContentRegionAvail().x - actionsW));
            if (ApexUi::TextButton("Keep as it is", "Your current settings stay as they are")) {
                LOG_INFO("[Menu] Welcome: settings kept");
                HideWelcome();
            }
            ImGui::SameLine();
            ImGui::BeginDisabled(Loading());
            if (ApexUi::IconTextButton("Apply##Welcome", IconId::Check, "Undo puts your settings back", ButtonKind::Primary)) {
                toml::table state;
                const int choice = g_welcomeChoice;
                unsigned parts = 0;
                if (ApexPresets::Read(choice, state)) parts = ApexConfig::ProfilePartsOf(state);
                LOG_INFO(std::format("[Menu] Welcome: {} chosen", ApexPresets::Get(choice).name));
                LoadBuiltinProfileNow(choice, parts & ~(ApexConfig::kPartShortcuts | ApexConfig::kPartDeveloper));
                HideWelcome();
            }
            ImGui::EndDisabled();
        }
    }
    ApexUi::EndCard();
    ImGui::PopID();
}

void DrawPage() {
    if (g_page == PageConflicts) { ConflictsPage(); GameAaCompatibilityNotice(); return; }
    GameAaCompatibilityNotice();
    switch (g_page) {
    case PageLighting: LightingPage(); break;
    case PageWaterSnow: WaterSnowPage(); break;
    case PageColor: ColorPage(); break;
    case PageAmbientOcclusion: AmbientOcclusionPage(); break;
    case PageDepthBlur: DepthBlurPage(); break;
    case PageEdgeSmoothing: EdgeSmoothingPage(); break;
    case PageConflicts: ConflictsPage(); break;
    case PagePerformance: PerformancePage(); break;
    case PageDeveloper:
        if (!kPublicBuild) {
            ApexUi::SetChangeReporting(false); // developer switches are not part of the undoable state
            DeveloperPage();
            ApexUi::SetChangeReporting(true);
        }
        break;
    case PageSettings: SettingsPage(); break;
    case PageReport: ReportPage(); break;
    default: OverviewPage(); break;
    }
}

// Height of the status bar (the hairline, a gap and one line of small text)
float StatusBarHeight() {
    ImGui::PushFont(nullptr, VioletTheme::BaseFontSize() * ApexUi::kSmallScale);
    const float lineH = ImGui::GetTextLineHeight();
    ImGui::PopFont();
    return lineH + ApexUi::kSpace2 * ApexUi::Unit();
}

// The thin footer: saving state (left), the peek hint (right)
void StatusBar(float height) {
    const float u = ApexUi::Unit();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    ImGui::Dummy(ImVec2(w, height));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(ImVec2(p.x, std::floor(p.y)), ImVec2(p.x + w, std::floor(p.y) + 1.0f), ImGui::GetColorU32(Col(VioletTheme::kCardBorder)));
    ImGui::PushFont(nullptr, VioletTheme::BaseFontSize() * ApexUi::kSmallScale);
    const float lineH = ImGui::GetTextLineHeight();
    const float y = p.y + height - lineH;
    const float is = 12.0f * u, ig = 5.0f * u;
    const ImU32 muted = ImGui::GetColorU32(Col(VioletTheme::kTextMuted));

    const bool saving = ApexConfig::SavePending();
    const char* left = I18n::Tr(saving ? "Saving\xE2\x80\xA6" : "All changes saved");
    const float leftW = is + ig + ImGui::CalcTextSize(left).x;
    const char* right = I18n::Tr("Hold Alt to peek");
    const float rightW = ImGui::CalcTextSize(right).x;
    const float spacing = ApexUi::kSpace4 * u;
    const bool showRight = leftW + spacing + rightW <= w;

    ApexUi::DrawIcon(dl, saving ? IconId::Save : IconId::CircleCheck, ImVec2(p.x, y + (lineH - is) * 0.5f), is,
                     saving ? muted : ImGui::GetColorU32(Col(VioletTheme::kSuccess)));
    dl->AddText(ImVec2(p.x + is + ig, y), saving ? muted : ImGui::GetColorU32(Col(VioletTheme::kSuccess, 0.85f)), left);
    if (showRight) dl->AddText(ImVec2(p.x + w - rightW, y), muted, right);
    ImGui::PopFont();
}

// The undo toast at the bottom right of the window, above the status bar: "<what changed>  Undo", about 4 s (fading;
// the timer waits while the mouse is on it)
void DrawToast(float bottomY) {
    if (!g_toast.active) return;
    const float u = ApexUi::Unit();
    const double now = ImGui::GetTime();
    double elapsed = now - g_toast.start;
    if (elapsed > kToastSeconds) {
        g_toast.active = false;
        return;
    }
    const float fade = elapsed < 0.15 ? static_cast<float>(elapsed / 0.15) : elapsed > kToastSeconds - 0.6 ? static_cast<float>((kToastSeconds - elapsed) / 0.6) : 1.0f;
    const float padX = ApexUi::kSpace3 * u, padY = ApexUi::kSpace2 * u, gap = ApexUi::kSpace4 * u;
    const float is = ApexUi::kIconSmall * u;
    const char* undoText = I18n::Tr("Undo");
    const ImVec2 textSize = ImGui::CalcTextSize(g_toast.text.c_str());
    const float undoW = is + 5.0f * u + ImGui::CalcTextSize(undoText).x;
    const float lineH = ImGui::GetTextLineHeight();
    const ImVec2 size(padX + textSize.x + gap + undoW + padX, padY + lineH + padY);
    const ImVec2 winPos = ImGui::GetWindowPos();
    const ImVec2 winSize = ImGui::GetWindowSize();
    const ImVec2 pos(std::fmax(winPos.x + ImGui::GetStyle().WindowPadding.x, winPos.x + winSize.x - ImGui::GetStyle().WindowPadding.x - size.x - ApexUi::kSpace2 * u),
                     bottomY - size.y - ApexUi::kSpace2 * u);

    ImGui::SetCursorScreenPos(pos);
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, ImGui::GetStyle().Alpha * std::fmin(std::fmax(fade, 0.0f), 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ChildBg, Col(VioletTheme::kSelectedBg));
    ImGui::PushStyleColor(ImGuiCol_Border, Col(VioletTheme::kAccentDark));
    ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 8.0f * u);
    ImGui::PushStyleVar(ImGuiStyleVar_ChildBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    if (ImGui::BeginChild("##UndoToast", size, ImGuiChildFlags_Borders, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)) {
        const ImVec2 cp = ImGui::GetCursorScreenPos();
        ImDrawList* dl = ImGui::GetWindowDrawList();
        dl->AddText(ImVec2(cp.x + padX, cp.y + padY), ImGui::GetColorU32(Col(VioletTheme::kText)), g_toast.text.c_str());
        const ImVec2 up(cp.x + padX + textSize.x + gap, cp.y + padY);
        ImGui::SetCursorScreenPos(ImVec2(up.x - 4.0f * u, cp.y + padY - 2.0f * u));
        const bool undo = ImGui::InvisibleButton("##Undo", ImVec2(undoW + 8.0f * u, lineH + 4.0f * u), ImGuiButtonFlags_EnableNav);
        const bool hovered = ImGui::IsItemHovered();
        const ImU32 col = ImGui::GetColorU32(Col(hovered ? VioletTheme::kAccentLight : VioletTheme::kAccent));
        ApexUi::DrawIcon(dl, IconId::Undo2, ImVec2(up.x, up.y + (lineH - is) * 0.5f), is, col);
        dl->AddText(ImVec2(up.x + is + 5.0f * u, up.y), col, undoText);
        if (ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows)) g_toast.start = now - std::fmin(elapsed, 1.0); // wait while pointed at
        if (undo) {
            ApexConfig::ApplyFeatureState(g_toast.undo);
            if (g_toast.restoreUi) ApexConfig::SetUi(g_toast.undoUi);
            LOG_INFO("[Menu] Undo: " + g_toast.logText);
            g_toast.active = false;
        }
    }
    ImGui::EndChild();
    ImGui::PopStyleVar(4);
    ImGui::PopStyleColor(2);
}

bool HoldShortcutDown(const ApexConfig::KeyChord& key) {
    if (!key.vk || key.ctrl || key.shift || key.alt) return false;
    if ((GetAsyncKeyState(VK_CONTROL) & 0x8000) || (GetAsyncKeyState(VK_SHIFT) & 0x8000)) return false;
    const bool altDown = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
    if (key.vk != VK_MENU && altDown) return false;
    return (GetAsyncKeyState(static_cast<int>(key.vk)) & 0x8000) != 0;
}

void MainWindow() {
    const float u = ApexUi::Unit();
    if (kPublicBuild) {
        if (g_page == PageDeveloper) g_page = PageOverview;
    }
    ImGuiIO& io = ImGui::GetIO();
    bool closeRequested = false;

    // ---- keys and pointer, from the previous frame's hover / focus (before any widget sees this frame's input) ----
    const bool dragging = ApexUi::SliderDragging();
    // The configurable peek key makes the menu translucent and inert (never while typing or dragging).
    const auto uiKeys = ApexConfig::GetUi();
    const bool peek = g_menuHovered && HoldShortcutDown(uiKeys.peekKey) &&
                      !io.WantTextInput && !dragging && !ImGui::IsAnyItemActive();
    if (g_menuFocused && !peek) {
        // Esc: clears the search, then closes the menu (never while a field is being
        // edited or the menu key is being chosen)
        if (ImGui::IsKeyPressed(ImGuiKey_Escape, false) && !g_waitingForKey && g_recRow < 0 && !ImGui::IsAnyItemActive() && !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) {
            if (g_search[0]) g_search[0] = '\0';
            else closeRequested = true;
        }
    }
    // Undo: the feature state before a click or a keyboard activation inside the menu (not while a slider is active: the
    // key that ends a keyboard adjustment must not replace the state from before it)
    const bool pointerInMenu = io.MousePos.x >= g_windowMin.x && io.MousePos.y >= g_windowMin.y && io.MousePos.x < g_windowMax.x && io.MousePos.y < g_windowMax.y;
    const bool activation = g_menuFocused && (ImGui::IsKeyPressed(ImGuiKey_Space, false) || ImGui::IsKeyPressed(ImGuiKey_Enter, false) ||
                                              ImGui::IsKeyPressed(ImGuiKey_KeypadEnter, false));
    if (!peek && !dragging && ((ImGui::IsMouseClicked(ImGuiMouseButton_Left) && pointerInMenu) || activation)) {
        ApexConfig::CaptureFeatureState(g_clickSnapshot);
        g_haveClickSnapshot = true;
    }
    // Opacity: peek 0.2 (x the disabled alpha of the inert contents = about 0.1), dragging a slider 0.35 (the dragged
    // row stays opaque), else 1; eased over about 0.1 s
    const float target = peek ? 0.2f : dragging ? 0.35f : 1.0f;
    g_alpha += (target - g_alpha) * std::fmin(1.0f, io.DeltaTime * 14.0f);
    if (std::fabs(target - g_alpha) < 0.01f) g_alpha = target;
    ApexUi::SetKeepActiveSliderOpaque(dragging && !peek);

    ImGui::SetNextWindowSize(ImVec2(560.0f * u, 640.0f * u), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(400.0f * u, 300.0f * u), ImVec2(FLT_MAX, FLT_MAX));
    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, g_alpha);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ApexUi::kSpace3 * u, ApexUi::kSpace3 * u));
    bool open = true;
    // "###ApexWindow": its own id (S3SS's window is ###S3SSWindow), stable whatever the visible name
    const bool drawn = ImGui::Begin(APEX_PRODUCT_NAME "###ApexWindow", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar |
                                                                                 ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    g_menuHovered = ImGui::IsWindowHovered(ImGuiHoveredFlags_RootAndChildWindows | ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    g_menuFocused = ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows);
    g_windowMin = ImGui::GetWindowPos();
    g_windowMax = ImVec2(g_windowMin.x + ImGui::GetWindowWidth(), g_windowMin.y + ImGui::GetWindowHeight());
    if (drawn) {
        ImGui::BeginDisabled(peek);
        open = Header();
        ImGui::Dummy(ImVec2(0.0f, 1.0f * u));
        ImGui::Separator();

        // Sidebar (fixed width, or the icon rail) and the page (scrolls in its own child), then the status bar
        const bool collapsed = ApexConfig::GetUi().sidebarCollapsed;
        const float statusH = StatusBarHeight();
        const float bodyH = std::fmax(ImGui::GetContentRegionAvail().y - statusH - ImGui::GetStyle().ItemSpacing.y, 60.0f * u);
        const float sidebarW = (collapsed ? 44.0f : 170.0f) * u;
        ImGui::BeginChild("##Sidebar", ImVec2(sidebarW, bodyH), ImGuiChildFlags_None, 0);
        Sidebar(collapsed);
        ImGui::EndChild();
        ImGui::SameLine(0.0f, 0.0f);
        {
            // Hairline between the sidebar and the page
            const ImVec2 a = ImGui::GetCursorScreenPos();
            ImGui::GetWindowDrawList()->AddLine(ImVec2(a.x + 6.0f * u, a.y), ImVec2(a.x + 6.0f * u, a.y + bodyH), ImGui::GetColorU32(Col(VioletTheme::kCardBorder)), 1.0f);
        }
        ImGui::SameLine(0.0f, ApexUi::kSpace3 * u);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ApexUi::kSpace2 * u, ApexUi::kSpace1 * u));
        ImGui::BeginChild("##Content", ImVec2(0.0f, bodyH), ImGuiChildFlags_AlwaysUseWindowPadding, 0);
        ImGui::PopStyleVar();
        if (g_search[0]) {
            SearchResults();
        } else if (WelcomeActive()) {
            WelcomePage();
        } else {
            ImGui::PushID(g_page); // each page's widgets get their own ids
            DrawPage();
            ImGui::PopID();
        }
        ImGui::EndChild();
        const float statusTop = ImGui::GetCursorScreenPos().y;
        StatusBar(statusH);
        DrawToast(statusTop);
        DeveloperConfirmation();
        ImGui::EndDisabled();

        // Hold to compare: the eye button, or the configured key while the pointer is over the menu.
        const bool holdKey = g_menuHovered && !peek && !io.WantTextInput && HoldShortcutDown(uiKeys.pictureCompareKey);
        if ((g_holdCompare || holdKey) && Picture::Get().GetParams().enabled) Picture::Get().HoldBypass();
    }
    g_holdCompare = false;
    ImGui::End();
    ImGui::PopStyleVar(); // Alpha

    // Peek and hold-to-compare belong to the menu while the pointer is over it; not while typing.
    g_keysOverMenu.store(g_menuHovered && !io.WantTextInput);
    g_menuTextInput.store(io.WantTextInput);

    // The last change of the frame becomes the undo toast (with the state from before the click)
    std::string change, changeEnglish;
    const bool changed = ApexUi::TakeChange(change, &changeEnglish);
    if (changed) LOG_INFO("[Menu] Changed: " + changeEnglish); // with the Undo lines, the log shows what the player did
    if (changed && g_haveClickSnapshot) {
        ShowToast(change, g_clickSnapshot, changeEnglish);
        g_haveClickSnapshot = false; // the next change takes a new snapshot at its own click
    }

    if (!open || closeRequested) {
        g_keysOverMenu.store(false);
        Overlay::SetVisible(false);
    }
}

// Read-only session gate, using the resolved WorldManager global and documented active/mode fields.
// The render thread publishes a cached bool; the window thread never reads game memory.
std::atomic<bool> g_menuAvailable{false};
std::atomic<unsigned> g_worldEpoch{0}; // bumped when the world session starts or ends (load, travel, quit)
bool g_lastWorldSession = false;
unsigned long long g_menuLiveAt = 0, g_menuGateCheckedAt = 0;
bool WorldSessionActive() {
    return WorldSession::IsActive();
}
void UpdateMenuAvailability() {
    const auto now = GetTickCount64();
    if (g_menuGateCheckedAt && now - g_menuGateCheckedAt < 200) return;
    g_menuGateCheckedAt = now;
    const auto startup = g_startup.load();
    const auto* night = Find(kNightLighting);
    const bool session = WorldSessionActive();
    if (session != g_lastWorldSession) {
        g_lastWorldSession = session;
        g_worldEpoch.fetch_add(1);
        Captures::OnWorldSessionChanged(); // the game shows its UI again after a load
    }
    const bool worldLive = session && (startup == Startup::RefusedOldBuild || !night || !night->IsEnabled() || NightLighting::WorldLive());
    if ((startup != Startup::Running && startup != Startup::RefusedOldBuild) || !worldLive) {
        g_menuLiveAt = 0;
        g_menuAvailable.store(false);
        return;
    }
    if (!g_menuLiveAt) g_menuLiveAt = now;
    g_menuAvailable.store(now - g_menuLiveAt >= 3000);
}

bool BannerNeeded() { return g_startup.load() == Startup::RefusedOldBuild || g_oldStandalone.load(); }

struct NoticeMotionState {
    bool active = false;
    unsigned long long enteredAt = 0;
    unsigned long long exitedAt = 0;
};
struct NoticeMotion {
    bool draw = false;
    float alpha = 1.0f;
    float yOffset = 0.0f;
};
constexpr unsigned long long kNoticeEnterMs = 140;
constexpr unsigned long long kNoticeExitMs = 120;
NoticeMotionState g_captureNoticeMotion, g_compareNoticeMotion, g_hintNoticeMotion;

NoticeMotion AnimateNotice(NoticeMotionState& state, bool active) {
    const unsigned long long now = GetTickCount64();
    if (active) {
        if (!state.active) {
            state.enteredAt = now;
            state.exitedAt = 0;
        }
        state.active = true;
        const float t = std::clamp(static_cast<float>(now - state.enteredAt) / static_cast<float>(kNoticeEnterMs), 0.0f, 1.0f);
        const float eased = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
        return {true, eased, -8.0f * (1.0f - eased)};
    }
    if (state.active) {
        state.active = false;
        state.exitedAt = now;
    }
    if (!state.exitedAt || now - state.exitedAt >= kNoticeExitMs) {
        state.exitedAt = 0;
        return {};
    }
    const float t = std::clamp(static_cast<float>(now - state.exitedAt) / static_cast<float>(kNoticeExitMs), 0.0f, 1.0f);
    const float eased = 1.0f - (1.0f - t) * (1.0f - t) * (1.0f - t);
    return {true, 1.0f - eased, -4.0f * eased};
}

bool NoticeAnimationsPending() {
    const auto pending = [](const NoticeMotionState& state) {
        return state.active || (state.exitedAt && GetTickCount64() - state.exitedAt < kNoticeExitMs);
    };
    return pending(g_captureNoticeMotion) || pending(g_compareNoticeMotion) || pending(g_hintNoticeMotion);
}
bool NoticeAnimationPending(const NoticeMotionState& state) {
    return state.active || (state.exitedAt && GetTickCount64() - state.exitedAt < kNoticeExitMs);
}

// Old builds found at startup: the combined build (features off) and/or an older standalone S3SSApex.asi (idle).
// One shared anchor for on-screen notices, irrespective of resolution, scale or notice type.
void PlaceScreenNotice(float yOffset = 0.0f) {
    const auto* vp = ImGui::GetMainViewport();
    const float u = ApexUi::Unit();
    const float margin = (ApexUi::kSpace4 + ApexUi::kSpace1) * u;
    ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + vp->Size.x * 0.5f, vp->Pos.y + margin + yOffset * u), ImGuiCond_Always, ImVec2(0.5f, 0.0f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(1.0f, 1.0f), ImVec2(std::max(1.0f, vp->Size.x - 2.0f * margin), vp->Size.y));
}

bool BeginNoticePill(const char* id, float contentWidth, float alpha = 1.0f, bool recording = false, unsigned borderTint = VioletTheme::kAccentLight, float yOffset = 0.0f, bool iconLayout = true) {
    PlaceScreenNotice(yOffset);
    const float u = ApexUi::Unit();
    const float h = ImGui::GetTextLineHeight();
    const float horizontalPadding = iconLayout ? 0.0f : ApexUi::kSpace3 * u;
    const float verticalPadding = iconLayout ? 0.0f : ApexUi::kSpace2 * u;
    // Wrapped text cannot determine an auto-sized width: it otherwise settles at one glyph.
    // Fix width from actual content before Begin, and allow auto-resize only for height.
    const float margin = (ApexUi::kSpace4 + ApexUi::kSpace1) * u;
    const float width = std::ceil(contentWidth + (iconLayout ? 18.0f * u : 2.0f * horizontalPadding));
    const float height = iconLayout ? std::max(44.0f * u, h + 16.0f * u) : std::round(h * 1.6f) + 2.0f * verticalPadding;
    ImGui::SetNextWindowSizeConstraints(ImVec2(width, height), ImVec2(width, height));
    ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_Always);
    {
        const auto* vp = ImGui::GetMainViewport();
        // Explicit size/position avoids first-frame pivot deferral on a new ImGui window.
        ImGui::SetNextWindowPos(ImVec2(vp->Pos.x + (vp->Size.x - width) * 0.5f, vp->Pos.y + margin + yOffset * u), ImGuiCond_Always);
    }

    ImGui::PushStyleVar(ImGuiStyleVar_Alpha, alpha);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(horizontalPadding, verticalPadding));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, height * 0.5f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(1.0f, h + 2.0f * verticalPadding));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, Col(VioletTheme::kWindowBg, 0.92f));
    ImGui::PushStyleColor(ImGuiCol_Border, Col(recording ? VioletTheme::kError : borderTint, 0.18f));
    return ImGui::Begin(id, nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoSavedSettings |
                                      ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoInputs);
}

void EndNoticePill() {
    ImGui::End();
    ImGui::PopStyleColor(2);
    ImGui::PopStyleVar(5);
}

float NoticeContentWidth(const std::string& text) {
    const float u = ApexUi::Unit();
    return ImGui::CalcTextSize(text.c_str()).x + (48.0f + 14.0f) * u;
}

void NoticeText(const std::string& text, IconId icon, bool recording = false, unsigned tint = VioletTheme::kAccentLight) {
    const float pulse = recording ? 0.6f + 0.4f * std::abs(std::sin(static_cast<float>(GetTickCount64() % 2000) * 3.14159265f / 2000.0f)) : 1.0f;
    const float u = ApexUi::Unit();
    const ImVec2 start = ImGui::GetWindowPos();
    const float height = ImGui::GetWindowHeight();
    const float iconSize = 20.0f * u;
    const float compartment = 48.0f * u;
    const float textX = compartment + 14.0f * u;
    const std::string& displayText = text;
    const ImVec2 textSize = ImGui::CalcTextSize(displayText.c_str());
    const float iconY = start.y + (height - iconSize) * 0.5f;
    ApexUi::Icon(icon, ImVec2(start.x + (compartment - iconSize) * 0.5f, iconY), iconSize,
                 ImGui::GetColorU32(Col(recording ? VioletTheme::kError : tint, pulse)));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float separatorHalf = 11.0f * u;
    dl->AddLine(ImVec2(start.x + compartment, start.y + height * 0.5f - separatorHalf),
                ImVec2(start.x + compartment, start.y + height * 0.5f + separatorHalf),
                ImGui::GetColorU32(Col(recording ? VioletTheme::kError : tint, 0.25f)), 1.0f * u);
    dl->AddText(ImVec2(start.x + textX, start.y + (height - textSize.y) * 0.5f), ImGui::GetColorU32(ImGuiCol_Text), displayText.c_str());
    ImGui::Dummy(ImVec2(0.0f, ImGui::GetTextLineHeight()));
}

void Banner() {
    std::string detail, oldModule;
    {
        std::lock_guard<std::mutex> lock(g_detailLock);
        detail = g_startupDetail;
        oldModule = g_oldStandaloneModule;
    }
    const bool refused = g_startup.load() == Startup::RefusedOldBuild;
    PlaceScreenNotice();
    ImGui::SetNextWindowBgAlpha(0.9f);
    if (ImGui::Begin("##ApexBanner", nullptr, ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings |
                                              ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav)) {
        ApexUi::InlineIcon(IconId::TriangleAlert, ApexUi::kIconMedium * ApexUi::Unit(), ImGui::GetColorU32(Col(VioletTheme::kWarning)));
        ImGui::SameLine();
        if (refused) {
            ImGui::TextColored(Col(VioletTheme::kError), "%s", I18n::Tr(APEX_PRODUCT_NAME " is off"));
            ImGui::TextUnformatted(I18n::Tr("An old combined build (Sims3SettingsSetter with Apex inside) is also installed:"));
            ImGui::TextUnformatted(detail.c_str());
            ImGui::TextUnformatted(I18n::Tr("Delete that file from Game\\Bin, keep the official Sims3SettingsSetter.asi, then restart the game."));
        }
        if (g_oldStandalone.load()) {
            if (refused) ImGui::Separator();
            ImGui::TextColored(Col(VioletTheme::kWarning), "%s", I18n::Trf("An older {} is also installed. Delete it from Game\\Bin.", oldModule).c_str());
            ImGui::TextUnformatted(I18n::Tr("It's the previous version of " APEX_PRODUCT_NAME " and stays idle for now. Keep ApexRadiance.asi and the official "
                                            "Sims3SettingsSetter.asi."));
        }
    }
    ImGui::End();
}

// ---- The start note (user's pick 30/09, "A Ã‚Â· compact pill"): the logo, "Apex Radiance is ready", a dot, "press" and the
// menu key in light violet, in a dark rounded pill with a faint violet border, top-center, at every start (never
// takes input; fades out). Starts only after the loaded-world/menu gate settles. Each frame counts at most 100 ms
// so a loading stall does not use up the note; opening the menu ends it. ----
constexpr int kHintMs = 8000;
int g_hintLeftMs = 0;                  // time on screen left (render thread)
bool g_hintStarted = false;            // started once this start
unsigned long long g_hintLastDraw = 0; // the previous Hint() frame

// Render thread, every frame (Client::AlwaysDraw)
void UpdateHint() {
    if (g_startup.load() != Startup::Running) return;
    if (!g_menuAvailable.load()) {
        g_hintLastDraw = 0; // hide and pause a live hint during another load
        return;
    }
    if (!g_hintStarted) {
        g_hintStarted = true;
        if (ApexConfig::GetUi().startNote && !Overlay::IsVisible()) {
            g_hintLeftMs = kHintMs;
            g_hintLastDraw = 0;
        }
    }
}

bool HintVisible() { return g_menuAvailable.load() && (g_hintLeftMs > 0 || NoticeAnimationPending(g_hintNoticeMotion)); }

void Hint() {
    const unsigned long long now = GetTickCount64();
    if (g_hintLastDraw) g_hintLeftMs -= static_cast<int>(std::min<unsigned long long>(now - g_hintLastDraw, 100));
    g_hintLastDraw = now;
    const NoticeMotion motion = AnimateNotice(g_hintNoticeMotion, g_hintLeftMs > 0);
    if (!motion.draw) return;
    const std::string ready = I18n::Tr(APEX_PRODUCT_NAME " is ready");
    const std::string press = I18n::Tr("press");
    const std::string key = ApexConfig::KeyChordText(ApexConfig::GetUi().toggle);
    const float u = ApexUi::Unit();
    const float lineH = ImGui::GetTextLineHeight();
    const float logo = std::round(lineH * 1.6f); // 26 px beside 16 px text, as the mock-up
    const std::string text = ready + " \xC2\xB7 " + press + " " + key;
    if (BeginNoticePill("##ApexHint", ImGui::CalcTextSize(text.c_str()).x + logo + 10.0f * u, motion.alpha, false,
                        VioletTheme::kAccentLight, motion.yOffset, false)) {
        const ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::Dummy(ImVec2(logo, logo));
        if (!ApexUi::DrawLogo(ImGui::GetWindowDrawList(), p, ImVec2(p.x + logo, p.y + logo), ImGui::GetStyle().Alpha))
            ApexUi::DrawIcon(ImGui::GetWindowDrawList(), IconId::Sparkles, p, logo, ImGui::GetColorU32(Col(VioletTheme::kAccent)));
        ImGui::SameLine(0.0f, 10.0f * u);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + (logo - lineH) * 0.5f);
        ImGui::TextUnformatted(text.c_str());
    }
    EndNoticePill();
}

// Key capture for the optional editor in Settings > Shortcuts.
// The first key pressed now (with the modifiers held at that moment); false while none
bool CaptureChord(ApexConfig::KeyChord& out) {
    for (UINT vk = 0x08; vk <= 0xFE; vk++) {
        switch (vk) {
        case VK_SHIFT: case VK_CONTROL: case VK_MENU: case VK_LSHIFT: case VK_RSHIFT: case VK_LCONTROL: case VK_RCONTROL: case VK_LMENU: case VK_RMENU:
        case VK_LWIN: case VK_RWIN: case VK_ESCAPE: case VK_RETURN: case VK_SPACE: case VK_TAB: case VK_CAPITAL: case VK_NUMLOCK:
        case VK_LBUTTON: case VK_RBUTTON: case VK_MBUTTON: case VK_XBUTTON1: case VK_XBUTTON2:
            continue;
        default: break;
        }
        if (!(GetAsyncKeyState(static_cast<int>(vk)) & 0x8000)) continue;
        ApexConfig::KeyChord c;
        c.vk = vk;
        c.ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
        c.shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
        c.alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
        if (vk == VK_INSERT && !c.ctrl && !c.shift && !c.alt) continue; // S3SS's own key
        out = c;
        return true;
    }
    return false;
}

// The same, with Space and Enter allowed (the shortcut editor; its checks refuse them alone)
bool CaptureChordAny(ApexConfig::KeyChord& out) {
    for (UINT vk : {static_cast<UINT>(VK_SPACE), static_cast<UINT>(VK_RETURN)})
        if (GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) {
            out.vk = vk;
            out.ctrl = (GetAsyncKeyState(VK_CONTROL) & 0x8000) != 0;
            out.shift = (GetAsyncKeyState(VK_SHIFT) & 0x8000) != 0;
            out.alt = (GetAsyncKeyState(VK_MENU) & 0x8000) != 0;
            return true;
        }
    return CaptureChord(out);
}

bool CaptureHoldKey(ApexConfig::KeyChord& out) {
    for (UINT vk = 0x08; vk <= 0xFE; vk++) {
        if (vk == VK_ESCAPE || vk == VK_LBUTTON || vk == VK_RBUTTON || vk == VK_MBUTTON || vk == VK_XBUTTON1 || vk == VK_XBUTTON2 ||
            vk == VK_SHIFT || vk == VK_CONTROL || vk == VK_MENU || vk == VK_LSHIFT || vk == VK_RSHIFT ||
            vk == VK_LCONTROL || vk == VK_RCONTROL || vk == VK_LMENU || vk == VK_RMENU ||
            vk == VK_LWIN || vk == VK_RWIN || vk == VK_CAPITAL || vk == VK_NUMLOCK || vk == VK_SCROLL) continue;
        if (GetAsyncKeyState(static_cast<int>(vk)) & 0x8000) {
            out = ApexConfig::KeyChord{vk, false, false, false};
            return true;
        }
    }
    // Alt is a useful standalone hold key; Shift and Ctrl are deliberately not accepted as base keys.
    if (GetAsyncKeyState(VK_MENU) & 0x8000) {
        out = ApexConfig::KeyChord{VK_MENU, false, false, false};
        return true;
    }
    return false;
}

// ---- Shortcuts: Settings > Shortcuts ----
// A key is recorded by clicking its chip and pressing the combination: while recording, every key press is eaten
// (Client::HotkeyDown, CaptureKey) so neither the game nor another shortcut sees it; Esc cancels. A combination is refused
// with a note when it is another action's, the game's cheat console (Ctrl+Shift+C), Windows' own (Alt+F4, Alt+Tab),
// Sims3SettingsSetter's menu (Insert alone), or a bare letter, digit or Space (it would stop that key from typing in the game).
enum ShortcutRow : int {
    RowMenu, RowCompare, RowRefresh, RowScreenshot, RowRecorder, RowProbe, RowDiagnostics, RowFrameCapture,
    RowSearch, RowPeek, RowPictureCompare, RowCount
};
UINT g_recHeldVk = 0;       // a refused key still held: ignored until released
bool g_recWaitRelease = false; // recording starts only once every key is up (the click's Enter, a held chord)
int g_recSeenFrame = -1;       // the last frame the editor was drawn: recording stops when it is not (menu closed, page left)
std::string g_recNote;      // why the last combination was refused

ApexConfig::KeyChord RowKey(int row) {
    if (row == RowMenu) return ApexConfig::GetUi().toggle;
    switch (row) {
    case RowCompare: return Hotkeys::Key(Hotkeys::Action::Compare);
    case RowRefresh: return Hotkeys::Key(Hotkeys::Action::Refresh);
    case RowScreenshot: return Hotkeys::Key(Hotkeys::Action::Screenshot);
    case RowRecorder: return Hotkeys::Key(Hotkeys::Action::Recorder);
    case RowProbe: return Hotkeys::Key(Hotkeys::Action::Probe);
    case RowDiagnostics: return Hotkeys::Key(Hotkeys::Action::Diagnostics);
    case RowFrameCapture: return Hotkeys::Key(Hotkeys::Action::FrameCapture);
    case RowSearch: return ApexConfig::GetUi().searchKey;
    case RowPeek: return ApexConfig::GetUi().peekKey;
    default: return ApexConfig::GetUi().pictureCompareKey;
    }
}
const char* RowName(int row) {
    switch (row) {
    case RowMenu: return "Open the menu";
    case RowCompare: return "Compare with the game";
    case RowRefresh: return "Refresh the lighting";
    case RowScreenshot: return "Take a filtered screenshot";
    case RowRecorder: return "Recording";
    case RowProbe: return "Light capture";
    case RowDiagnostics: return "Lighting snapshot";
    case RowFrameCapture: return "Frame Capture";
    case RowSearch: return "Search the settings";
    case RowPeek: return "Peek at the game behind the menu";
    default: return "Compare the picture without its filters";
    }
}
const char* RowText(int row) {
    switch (row) {
    case RowMenu: return "Opens and closes the " APEX_PRODUCT_NAME " menu";
    case RowCompare: return "Turns Apex's effects off and on, to see the difference";
    case RowRefresh: return "Relights the ground, lots and rooms when something loaded wrong";
    case RowScreenshot: return "Saves the finished game image after Apex's visual effects";
    case RowRecorder: return "Collects lighting activity and settings for troubleshooting";
    case RowProbe: return "Captures lighting at a point you choose";
    case RowDiagnostics: return "Saves a snapshot of the lighting state";
    case RowFrameCapture: return "Records draw calls for troubleshooting";
    case RowSearch: return "Focuses the settings search field";
    case RowPeek: return "Hold the key over the menu to see through it";
    default: return "Hold the key over the menu to bypass Picture";
    }
}
bool SameChord(const ApexConfig::KeyChord& a, const ApexConfig::KeyChord& b) {
    return a.vk == b.vk && a.ctrl == b.ctrl && a.shift == b.shift && a.alt == b.alt;
}
// Empty when the combination can be used for `row`, else why not (English, translated where drawn)
std::string ChordProblem(const ApexConfig::KeyChord& c, int row) {
    const bool mods = c.ctrl || c.shift || c.alt;
    if (c.vk == VK_ESCAPE) return "Escape cancels the current action";
    if (c.vk == VK_F10 && !mods) return "F10 is reserved for hiding the game's interface";
    if (c.vk == VK_LWIN || c.vk == VK_RWIN || c.vk == VK_APPS) return "Windows keys are reserved by Windows";
    if (c.vk == VK_INSERT && !mods) return "Insert alone opens Sims3SettingsSetter's menu";
    if (c.vk == 'C' && c.ctrl && c.shift && !c.alt) return "Ctrl+Shift+C is the game's cheat console";
    if ((c.vk == VK_F4 || c.vk == VK_TAB) && c.alt) return "That combination belongs to Windows";
    if ((row == RowPeek || row == RowPictureCompare) && mods) return "Choose one key without modifiers for this hold action";
    if (row != RowPeek && row != RowPictureCompare && !mods && ((c.vk >= 'A' && c.vk <= 'Z') || (c.vk >= '0' && c.vk <= '9') || c.vk == VK_SPACE ||
                  c.vk == VK_RETURN || c.vk == VK_BACK || c.vk == VK_DELETE || c.vk == VK_TAB || (c.vk >= VK_LEFT && c.vk <= VK_DOWN)))
        return "Use it with Ctrl, Shift or Alt: alone it would stop that key from typing in the game";
    for (int r = 0; r < RowCount; r++) {
        if (r == RowScreenshot && !ApexConfig::GetUi().screenshotShortcutEnabled) continue;
        if (r == RowFrameCapture && kPublicBuild) continue;
        if (r != row && SameChord(RowKey(r), c)) return I18n::Trf("Already used by: {}", I18n::Tr(RowName(r)));
    }
    return {};
}
void StoreRowKey(int row, const ApexConfig::KeyChord& c) {
    ApexConfig::UiSettings ui = ApexConfig::GetUi();
    const int base = ui.hotkeyPreset >= 0 && ui.hotkeyPreset < static_cast<int>(Hotkeys::Preset::Count) ? ui.hotkeyPreset : static_cast<int>(Hotkeys::Preset::FKeys);
    if (ui.hotkeyPreset != Hotkeys::kMine) ui.minePresetBase = base;
    if (row == RowMenu) ui.toggle = c;
    else switch (row) {
    case RowCompare: ui.compareKey = c; break;
    case RowRefresh: ui.refreshKey = c; break;
    case RowScreenshot: ui.screenshotKey = c; break;
    case RowRecorder: ui.recorderKey = c; break;
    case RowProbe: ui.probeKey = c; break;
    case RowDiagnostics: ui.diagnosticsKey = c; break;
    case RowFrameCapture: ui.frameCaptureKey = c; break;
    case RowSearch: ui.searchKey = c; break;
    case RowPeek: ui.peekKey = c; break;
    case RowPictureCompare: ui.pictureCompareKey = c; break;
    default: break;
    }
    ui.hotkeyPreset = Hotkeys::kMine;
    ui.keyChosen = true;
    ApexConfig::SetUi(ui);
    LOG_INFO(std::format("[Menu] {}: {}", RowName(row), ApexConfig::KeyChordText(c)));
}
// Render thread, every frame while a row records
void RecordStep() {
    if (g_recRow < 0) return;
    // not while the editor is not on screen, nor while another window has the keyboard (review 30/09, M1)
    if (!Overlay::IsVisible() || ImGui::GetFrameCount() - g_recSeenFrame > 2) {
        g_recRow = -1;
        g_recNote.clear();
        return;
    }
    DWORD pid = 0;
    const HWND fg = GetForegroundWindow();
    if (!fg || !GetWindowThreadProcessId(fg, &pid) || pid != GetCurrentProcessId()) return;
    if (g_recWaitRelease) {
        for (int vk = 0x08; vk <= 0xFE; vk++)
            if (vk != VK_LBUTTON && vk != VK_RBUTTON && vk != VK_MBUTTON && (GetAsyncKeyState(vk) & 0x8000)) return;
        g_recWaitRelease = false;
    }
    if (GetAsyncKeyState(VK_ESCAPE) & 0x8000) {
        g_recRow = -1;
        g_recNote.clear();
        return;
    }
    if (g_recHeldVk) {
        if (GetAsyncKeyState(static_cast<int>(g_recHeldVk)) & 0x8000) return;
        g_recHeldVk = 0;
    }
    ApexConfig::KeyChord c;
    const bool holdKeyRow = g_recRow == RowPeek || g_recRow == RowPictureCompare;
    if (!(holdKeyRow ? CaptureHoldKey(c) : CaptureChordAny(c))) return;
    const std::string problem = ChordProblem(c, g_recRow);
    if (!problem.empty()) {
        g_recNote = ApexConfig::KeyChordText(c) + ": " + I18n::Tr(problem.c_str()); // a text already translated comes back as it is
        g_recHeldVk = c.vk;
        return;
    }
    StoreRowKey(g_recRow, c);
    g_recRow = -1;
    g_recNote.clear();
}
void ApplyPreset(int preset) {
    ApexConfig::UiSettings ui = ApexConfig::GetUi();
    ui.hotkeyPreset = preset;
    ui.toggle = Hotkeys::PresetMenu(static_cast<Hotkeys::Preset>(preset));
    ui.compareKey.vk = ui.refreshKey.vk = 0;
    ui.probeKey.vk = ui.diagnosticsKey.vk = ui.recorderKey.vk = ui.frameCaptureKey.vk = 0;
    ui.keyChosen = true;
    ApexConfig::SetUi(ui);
    LOG_INFO(std::format("[Menu] Shortcut preset: {}", Hotkeys::PresetName(static_cast<Hotkeys::Preset>(preset))));
}

std::string KeyChipText(int row) {
    return g_recRow == row ? std::string(I18n::Tr("Press the keys\xE2\x80\xA6")) : ApexConfig::KeyChordText(RowKey(row));
}
float KeyChipWidth(int row) {
    const std::string text = KeyChipText(row);
    return std::fmax(ImGui::CalcTextSize(text.c_str()).x + 2.0f * ImGui::GetStyle().FramePadding.x, 150.0f * ApexUi::Unit());
}

// The key chip of a row: its keys, or the recording prompt; a click starts recording
void KeyChip(int row) {
    ImGui::PushID(row);
    const bool rec = g_recRow == row;
    const std::string text = KeyChipText(row);
    const float w = KeyChipWidth(row);
    if (ApexUi::TextButton((text + "##Chip").c_str(), nullptr, rec ? ButtonKind::Primary : ButtonKind::Secondary, w)) {
        g_recRow = rec ? -1 : row;
        g_recWaitRelease = true;
        g_recNote.clear();
        g_recHeldVk = 0;
    }
    if (ImGui::IsItemHovered() && !rec) ImGui::SetTooltip("%s", I18n::Tr("Click, then press the new combination (Esc cancels)"));
    ImGui::PopID();
}

// Shared key editor for Settings > Shortcuts.
void ShortcutsContent() {
    g_recSeenFrame = ImGui::GetFrameCount();
    ApexConfig::UiSettings ui = ApexConfig::GetUi();
    static const char* const kPresets[] = {"Letters", "Numbers", "F keys", "Custom"};
    int sel = ui.hotkeyPreset < 0 ? static_cast<int>(Hotkeys::Preset::FKeys)
                                  : std::clamp(ui.hotkeyPreset, 0, Hotkeys::kMine);
    const float u = ApexUi::Unit();
    if (ApexUi::BeginControlRow("Key set", "Pick a ready set, or click a key below to record your own", 220.0f * u)) {
        ImGui::SetNextItemWidth(220.0f * u);
        if (ImGui::BeginCombo("##KeySet", I18n::Tr(kPresets[sel]))) {
            for (int i = 0; i < 3; i++)
                if (ImGui::Selectable(I18n::Tr(kPresets[i]), sel == i)) ApplyPreset(i);
            if (sel == Hotkeys::kMine) ImGui::Selectable(I18n::Tr(kPresets[3]), true);
            ImGui::EndCombo();
        }
        ApexUi::EndControlRow();
    }
    if (sel < 3) ApexUi::MutedText(I18n::Tr(Hotkeys::PresetDescription(static_cast<Hotkeys::Preset>(sel))));
    for (int row : {RowMenu, RowCompare, RowRefresh}) {
        if (!ApexUi::BeginControlRow(RowName(row), RowText(row), KeyChipWidth(row))) continue;
        KeyChip(row);
        ApexUi::EndControlRow();
    }
    if (g_recRow >= 0) ApexUi::IconNote(IconId::Keyboard, g_recNote.empty() ? I18n::Tr("Press the new combination now; Esc cancels") : g_recNote.c_str(),
                                        g_recNote.empty() ? VioletTheme::kAccent : VioletTheme::kWarning);
    ApexUi::GroupLabel("IN THE MENU");
    for (int row : {RowSearch, RowPeek, RowPictureCompare}) {
        if (!ApexUi::BeginControlRow(RowName(row), RowText(row), KeyChipWidth(row))) continue;
        KeyChip(row);
        ApexUi::EndControlRow();
    }
    ApexUi::GroupLabel("REPORT A PROBLEM");
    for (int row : {RowRecorder, RowProbe, RowDiagnostics}) {
        if (!ApexUi::BeginControlRow(RowName(row), RowText(row), KeyChipWidth(row))) continue;
        KeyChip(row);
        ApexUi::EndControlRow();
    }
    if (!kPublicBuild) {
        ApexUi::GroupLabel("DEVELOPER TOOLS");
        if (ApexUi::BeginControlRow(RowName(RowFrameCapture), RowText(RowFrameCapture), KeyChipWidth(RowFrameCapture))) {
            KeyChip(RowFrameCapture);
            ApexUi::EndControlRow();
        }
    }
}

// Settings > Menu: screenshot settings and the existing validated shortcut recorder.
void ScreenshotCaptureCard() {
    g_recSeenFrame = ImGui::GetFrameCount();
    if (ApexUi::BeginCard("##ScreenshotCapture")) {
        ApexConfig::UiSettings ui = ApexConfig::GetUi();
        ApexUi::CardHeader(IconId::Camera, "Screenshot capture", "Save the finished game image with Apex's active effects", nullptr, nullptr);
        ApexUi::CardDivider();
        static const char* const kFolders[] = {"Game's Screenshots folder", "Apex Radiance folder"};
        static const char* const kFolderTips[] = {"Next to the photos the game takes, in Documents > Electronic Arts > The Sims 3 > Screenshots",
                                                  "Kept apart from the game's photos, in Documents > Electronic Arts > The Sims 3 > Apex Radiance > Screenshots"};
        int folder = ui.screenshotToApexFolder ? 1 : 0;
        if (ApexUi::SegmentedRow("Save screenshots to", "Where the filtered screenshots are saved", "##ShotFolder", &folder, kFolders, 2, kFolderTips, nullptr, 0)) {
            ui.screenshotToApexFolder = folder == 1;
            ApexConfig::SetUi(ui);
        }
        if (ApexUi::BeginControlRow(RowName(RowScreenshot),
                                    I18n::Trf("Press {} to save one screenshot with Apex's effects", ApexConfig::KeyChordText(Hotkeys::Key(Hotkeys::Action::Screenshot))).c_str(),
                                    KeyChipWidth(RowScreenshot))) {
            KeyChip(RowScreenshot);
            ApexUi::EndControlRow();
        }
        if (g_recRow == RowScreenshot)
            ApexUi::IconNote(IconId::Keyboard, g_recNote.empty() ? I18n::Tr("Press the new combination now; Esc cancels") : g_recNote.c_str(),
                             g_recNote.empty() ? VioletTheme::kAccent : VioletTheme::kWarning);
        if (ApexUi::SwitchRow("Hide game UI in screenshots", &ui.screenshotHideGameUi,
                              "Hides the game interface for the photo, then restores it; turn off to include it")) ApexConfig::SetUi(ui);
    }
    ApexUi::EndCard();

}

// Settings > Shortcuts
void ShortcutsTab() {
    ImGui::PushID("Shortcuts");
    g_recSeenFrame = ImGui::GetFrameCount();
    if (ApexUi::FilterActive()) {
        if (ApexUi::BeginCard("##SearchShortcuts")) {
            ApexUi::CardHeader(IconId::Keyboard, "Shortcuts", "Choose keys for the menu and Apex actions", nullptr, nullptr);
            ApexUi::CardDivider();
            ShortcutsContent();

        }
        ApexUi::EndCard();
        ImGui::PopID();
        return;
    }

    if (ApexUi::BeginCard("##MainShortcuts")) {
        ApexUi::CardHeader(IconId::Keyboard, "Shortcuts", "Choose a preset or click a shortcut to edit it", nullptr, nullptr);
        ApexUi::CardDivider();
        const auto current = ApexConfig::GetUi();
        const int selected = current.hotkeyPreset < 0 ? static_cast<int>(Hotkeys::Preset::FKeys)
            : std::clamp(current.hotkeyPreset, 0, Hotkeys::kMine);
        static const char* const presets[] = {"Letters", "Numbers", "F keys", "Custom"};
        if (ApexUi::BeginControlRow("Shortcut preset", "Switch the main shortcuts together", 180.0f * ApexUi::Unit())) {
            ImGui::SetNextItemWidth(180.0f * ApexUi::Unit());
            if (ImGui::BeginCombo("##MainPreset", I18n::Tr(presets[selected]))) {
                for (int i = 0; i < 3; ++i) {
                    if (ImGui::Selectable(I18n::Tr(presets[i]), selected == i)) ApplyPreset(i);
                    if (selected == i) ImGui::SetItemDefaultFocus();
                }
                if (selected == Hotkeys::kMine) {
                    ImGui::BeginDisabled();
                    ImGui::Selectable(I18n::Tr("Custom"), true);
                    ImGui::EndDisabled();
                }
                ImGui::EndCombo();
            }
            ApexUi::EndControlRow();
        }
        for (int row : {RowMenu, RowCompare, RowRefresh, RowScreenshot}) {
            ImGui::BeginDisabled(row == RowScreenshot && !ApexConfig::GetUi().screenshotShortcutEnabled);
            if (ApexUi::BeginControlRow(RowName(row), nullptr, KeyChipWidth(row))) {
                KeyChip(row);
                ApexUi::EndControlRow();
            }
            ImGui::EndDisabled();
        }
        if (g_recRow >= 0)
            ApexUi::IconNote(IconId::Keyboard, g_recNote.empty() ? I18n::Tr("Press the new combination now; Esc cancels") : g_recNote.c_str(),
                             g_recNote.empty() ? VioletTheme::kAccent : VioletTheme::kWarning);
        ApexUi::Gap(ApexUi::kSpace1);
        ApexUi::MutedText("F10 always hides the game interface");
    }
    ApexUi::EndCard();

    if (ApexUi::BeginCard("##ReportShortcuts")) {
        ApexUi::CardHeader(IconId::ListChecks, "Report a problem", "Shortcuts for recording and lighting captures", nullptr, nullptr);
        ApexUi::CardDivider();
        for (int row : {RowRecorder, RowProbe, RowDiagnostics}) {
            if (!ApexUi::BeginControlRow(RowName(row), RowText(row), KeyChipWidth(row))) continue;
            KeyChip(row);
            ApexUi::EndControlRow();
        }
        if (!kPublicBuild && ApexUi::BeginControlRow(RowName(RowFrameCapture), RowText(RowFrameCapture), KeyChipWidth(RowFrameCapture))) {
            KeyChip(RowFrameCapture);
            ApexUi::EndControlRow();
        }
    }
    ApexUi::EndCard();

    const bool narrowExtras = ImGui::GetContentRegionAvail().x < 700.0f * ApexUi::Unit();
    if (ImGui::BeginTable("##ShortcutExtras", narrowExtras ? 1 : 2, ImGuiTableFlags_SizingStretchProp, ImVec2(0.0f, 0.0f))) {
        if (!narrowExtras) {
            ImGui::TableSetupColumn("menu", ImGuiTableColumnFlags_WidthStretch, 1.0f);
            ImGui::TableSetupColumn("help", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        }
        ImGui::TableNextColumn();
        if (ApexUi::BeginCard("##InMenu")) {
            ApexUi::CardHeader(IconId::AppWindow, "While Apex is open", "Quick ways to navigate and compare", nullptr, nullptr);
            ApexUi::CardDivider();
            for (int row : {RowSearch, RowPeek, RowPictureCompare}) {
                if (!ApexUi::BeginControlRow(RowName(row), RowText(row), KeyChipWidth(row))) continue;
                KeyChip(row);
                ApexUi::EndControlRow();
            }
        }
        ApexUi::EndCard();
        ImGui::TableNextColumn();
        if (ApexUi::BeginCard("##PersonalizeShortcuts")) {
            ApexUi::CardHeader(IconId::SlidersHorizontal, "Personalize", "Your settings stay yours", nullptr, nullptr);
            ApexUi::CardDivider();
            ApexUi::MutedText("Presets change only after you select one. Custom key choices are saved and stay in place.");
            ApexUi::Gap(ApexUi::kSpace2);
            if (ApexUi::TextButton("Show the startup hint next time##Shortcuts", "Shows the menu shortcut at the top center of the screen at the next start")) {
                ApexConfig::UiSettings ui = ApexConfig::GetUi();
                ui.startNote = true;
                ApexConfig::SetUi(ui);
            }
        }
        ApexUi::EndCard();
        ImGui::EndTable();
    }
    ImGui::PopID();
}

// ---- Compare with the game (its shortcut): Night Lighting, Ambient Occlusion, Depth Blur, Edge Smoothing and the picture filters off, then
// back as they were. Not saved: the features are installed / removed directly, and the picture filters are set without a
// save (a setting saved meanwhile from the menu could store them off: the menu is closed while comparing, normally).
bool g_comparing = false;
std::vector<ApexPatch*> g_comparePaused;
bool g_comparePicture = false;
void ToggleCompare() {
    if (!g_comparing) {
        g_comparePaused.clear();
        for (const char* name : {"NightTerrainRelight", "AmbientOcclusion", "SceneDither", "DepthBlur", "EdgeSmoothing"})
            if (ApexPatch* p = Find(name); p && p->IsEnabled() && p->Uninstall()) g_comparePaused.push_back(p);
        PictureParams pp = Picture::Get().GetParams();
        g_comparePicture = pp.enabled;
        if (pp.enabled) {
            pp.enabled = false;
            Picture::Get().SetParams(pp, false);
        }
        {
            std::vector<std::string> names;
            for (ApexPatch* p : g_comparePaused) names.push_back(p->GetName());
            ApexConfig::SetCompareOverride(names, g_comparePicture); // never saved as off (review H2)
        }
        g_comparing = true;
        LOG_INFO(std::format("[Menu] Compare: {} features off until the shortcut is pressed again", g_comparePaused.size() + (g_comparePicture ? 1 : 0)));
    } else {
        for (ApexPatch* p : g_comparePaused) p->Install();
        g_comparePaused.clear();
        if (g_comparePicture) {
            PictureParams pp = Picture::Get().GetParams();
            pp.enabled = true;
            Picture::Get().SetParams(pp, false);
        }
        ApexConfig::SetCompareOverride({}, false);
        g_comparing = false;
        LOG_INFO("[Menu] Compare: features back on");
    }
}
// While comparing: a small note top centre (the game as it looks without Apex)
bool CompareNote(bool drawAllowed = true) {
    static std::string cachedText;
    if (g_comparing) {
        const std::string key = ApexConfig::KeyChordText(Hotkeys::Key(Hotkeys::Action::Compare));
        cachedText = I18n::Trf("The game without Apex \xC2\xB7 {} to turn it back on", key);
    }
    const NoticeMotion motion = AnimateNotice(g_compareNoticeMotion, g_comparing);
    if (!motion.draw || !drawAllowed) return false;
    if (BeginNoticePill("##ApexCompareNote", NoticeContentWidth(cachedText), motion.alpha, false,
                        VioletTheme::kAccentLight, motion.yOffset))
        NoticeText(cachedText, IconId::Columns2);
    EndNoticePill();
    return true;
}
// Render thread, every frame: the shortcuts that are not the menu key
void RunShortcuts() {
    RecordStep();
    if (Hotkeys::Take(Hotkeys::Action::Compare)) ToggleCompare();
    if (Hotkeys::Take(Hotkeys::Action::Refresh)) NightLighting::RefreshAll();
    if (Hotkeys::Take(Hotkeys::Action::Screenshot))
        Captures::RequestPlayerScreenshot(ApexConfig::GetUi().screenshotHideGameUi);
}

// The capture notes (Report a problem, both builds): a pill at the top center in the start note's style; shown instead of the start note when
// both show, never taking input. While a recording runs: a red dot, its seconds and the key to stop; then "Saved ..." (or
// the other captures' start / saved notes, Captures::Notify) for a few seconds; while a capture session is open and
// nothing else shows: the session and its count, so it is not forgotten.
bool CaptureNote(bool drawAllowed = true) {
    static std::string cachedText;
    static IconId cachedIcon = IconId::Info;
    static bool cachedDot = false;
    static unsigned cachedTint = VioletTheme::kAccentLight;
    const int secs = Recorder::SecondsRecorded();
    const Captures::Note note = Captures::CurrentNote();
    std::string text;
    bool dot = false;
    if (LightProbe::Aiming()) {
        text = I18n::Trf("Click the problem to capture it, or press {}. Esc cancels", CaptureKey(Hotkeys::Action::Probe));
    } else if (secs >= 0) {
        text = I18n::Trf("Recording {} s \xC2\xB7 press {} to stop", secs, ApexConfig::KeyChordText(Hotkeys::Key(Hotkeys::Action::Recorder)));
        dot = true;
    } else if (note.visible) {
        text = note.text; // translated when it was made (Captures::Notify callers)
    }
    IconId icon = IconId::Info;
    unsigned tint = VioletTheme::kAccentLight;
    if (dot) { icon = IconId::Activity; tint = VioletTheme::kError; }
    else if (LightProbe::Aiming() || note.kind == Captures::NoteKind::Probe) { icon = IconId::Crosshair; }
    else switch (note.kind) {
        case Captures::NoteKind::Success: icon = IconId::CircleCheck; tint = VioletTheme::kSuccess; break;
        case Captures::NoteKind::Warning: icon = IconId::TriangleAlert; tint = VioletTheme::kWarning; break;
        case Captures::NoteKind::Saving: icon = IconId::Save; break;
        case Captures::NoteKind::Screenshot: icon = IconId::Camera; tint = VioletTheme::kSuccess; break;
        case Captures::NoteKind::Info:
        case Captures::NoteKind::Probe: break;
    }
    const bool active = LightProbe::Aiming() || secs >= 0 || note.visible;
    if (active) {
        cachedText = text;
        cachedIcon = icon;
        cachedDot = dot;
        cachedTint = tint;
    }
    const NoticeMotion motion = AnimateNotice(g_captureNoticeMotion, active);
    if (!motion.draw || !drawAllowed) return false;
    if (BeginNoticePill("##ApexCaptureNote", NoticeContentWidth(cachedText), motion.alpha, cachedDot, cachedTint, motion.yOffset))
        NoticeText(cachedText, cachedIcon, cachedDot, cachedTint);
    EndNoticePill();
    return true;
}

// A pixel target only: confirming click is consumed; no object lookup or per-frame scene scans.
void CaptureTarget() {
    if (!LightProbe::Aiming()) return;
    if (Overlay::IsVisible()) { LightProbe::CancelAim(); return; }
    POINT p{};
    const HWND window = GetForegroundWindow();
    DWORD process = 0;
    GetWindowThreadProcessId(window, &process);
    if (process != GetCurrentProcessId() || !GetCursorPos(&p) || !ScreenToClient(window, &p)) return;
    RECT client{};
    if (!GetClientRect(window, &client) || !PtInRect(&client, p)) return;
    const auto* vp = ImGui::GetMainViewport();
    const float x = vp->Pos.x + p.x * vp->Size.x / std::max<LONG>(1, client.right);
    const float y = vp->Pos.y + p.y * vp->Size.y / std::max<LONG>(1, client.bottom);
    const float r = 9.0f * ApexUi::Unit();
    auto* draw = ImGui::GetForegroundDrawList();
    const ImU32 color = ImGui::GetColorU32(Col(VioletTheme::kAccentLight));
    draw->AddCircle(ImVec2(x, y), r, ImGui::GetColorU32(Col(VioletTheme::kWindowBg)), 24, 4.0f);
    draw->AddCircle(ImVec2(x, y), r, color, 24, 1.5f);
    draw->AddLine(ImVec2(x - r - 4, y), ImVec2(x - r + 3, y), color, 1.5f);
    draw->AddLine(ImVec2(x + r - 3, y), ImVec2(x + r + 4, y), color, 1.5f);
    draw->AddLine(ImVec2(x, y - r - 4), ImVec2(x, y - r + 3), color, 1.5f);
    draw->AddLine(ImVec2(x, y + r - 3), ImVec2(x, y + r + 4), color, 1.5f);
}

std::atomic<bool> g_returnFromProbe{false};
class GuiClient final : public Overlay::Client {
    bool eatProbeMouseUp = false; // window thread only: the confirming click must not also select/place a game object
    // Window thread only: the game's cheat console is inferred from Ctrl+Shift+C, so the guess must not stick. It ends on
    // Enter/Esc, focus loss, the Apex menu opening, a world change, or 30 s without typing.
    bool cheatConsoleGuess = false;
    unsigned long long cheatConsoleInputAt = 0;
    unsigned cheatConsoleEpoch = 0;
    bool CheatConsoleOpen() {
        if (cheatConsoleGuess && (Overlay::IsVisible() || cheatConsoleEpoch != g_worldEpoch.load() ||
                                  GetTickCount64() - cheatConsoleInputAt > 30000))
            cheatConsoleGuess = false;
        return cheatConsoleGuess;
    }
  public:
    void Draw() override {
        if (!g_menuAvailable.load()) {
            if (HintVisible() && !Captures::ScreenshotPending()) Hint();
            return;
        }
        if (g_returnFromProbe.load() && !LightProbe::Busy() && !Captures::ScreenshotPending() && !Captures::Saving()) {
            g_returnFromProbe.store(false);
            g_page = PageReport;
            Overlay::SetVisible(true);
        }
        Captures::SetScreenshots(ApexConfig::GetUi().captureScreenshot); // [ui] capture_screenshot
        bool noticeShown = false;
        const bool noticesAllowed = !BannerNeeded() && !Captures::ScreenshotPending();
        if (!Captures::ScreenshotPending()) {
            CaptureTarget();
        }
        noticeShown = CaptureNote(noticesAllowed); // never in the capture's own screenshot
        const bool compareShown = CompareNote(noticesAllowed && !noticeShown);
        if (!noticeShown) noticeShown = compareShown;
        if (BannerNeeded()) { Banner(); noticeShown = true; }
        if (!Overlay::IsVisible()) {
            if (!noticeShown && HintVisible() && !Captures::ScreenshotPending()) Hint();
            return;
        }
        g_hintLeftMs = 0; // opening the menu ends the start note
        AnimateNotice(g_hintNoticeMotion, false);
        MainWindow();
    }

    bool AlwaysDraw() override {
        UpdateMenuAvailability();
        UpdateHint();
        if (!g_menuAvailable.load()) { Overlay::SetVisible(false); return HintVisible(); }
        RunShortcuts();
        // the capture notes (recording, saved, an open session) show with the menu closed too
        return BannerNeeded() || HintVisible() || g_comparing || Recorder::SecondsRecorded() >= 0 || Captures::CurrentNote().visible ||
               NoticeAnimationsPending() || (!g_profiles.loading.empty() && ImGui::GetTime() - g_profiles.applyOpenedAt < 0.14) || LightProbe::Aiming() || g_returnFromProbe.load();
    }

    bool IsToggleKey(WPARAM vk) override {
        if (CheatConsoleOpen()) return false; // the game's cheat console owns all keys until Enter, Esc or Ctrl+Shift+C
        if (g_recRow >= 0) return false; // being recorded as a shortcut
        const ApexConfig::KeyChord c = ApexConfig::GetUi().toggle;
        if (vk != c.vk) return false;
        const bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0, alt = GetKeyState(VK_MENU) < 0;
        return ctrl == c.ctrl && shift == c.shift && alt == c.alt;
    }

    float FontScale() override { return ApexConfig::GetUi().fontScale; }
    bool CanOpen() override { return g_menuAvailable.load(); }

    bool OnWindowMessage(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, LRESULT* result) override {
        if (msg == WM_KILLFOCUS || (msg == WM_ACTIVATEAPP && !wp)) {
            eatProbeMouseUp = false;
            cheatConsoleGuess = false;
            LightProbe::CancelAim();
        }
        if ((msg == WM_CHAR || msg == WM_KEYDOWN) && cheatConsoleGuess) cheatConsoleInputAt = GetTickCount64();
        if (msg == WM_LBUTTONUP && eatProbeMouseUp) {
            eatProbeMouseUp = false;
            *result = 0;
            return true;
        }
        if (!Overlay::IsVisible() && (msg == WM_LBUTTONDOWN || msg == WM_LBUTTONDBLCLK) && LightProbe::Aiming()) {
            const POINT pixel{static_cast<short>(LOWORD(lp)), static_cast<short>(HIWORD(lp))};
            RECT client{};
            if (GetForegroundWindow() == hwnd && GetClientRect(hwnd, &client) && PtInRect(&client, pixel) && LightProbe::ConfirmAim(pixel)) {
                eatProbeMouseUp = true;
                g_returnFromProbe.store(true);
                *result = 0;
                return true;
            }
        }
        return false;
    }

    void GameKeyDown(WPARAM vk, bool repeat) override {
        Captures::ObserveGameUiKey(vk, repeat);
        if (repeat) return;
        const bool cheatConsoleChord = vk == 'C' && GetKeyState(VK_CONTROL) < 0 && GetKeyState(VK_SHIFT) < 0 &&
                                       GetKeyState(VK_MENU) >= 0;
        if (cheatConsoleChord) {
            cheatConsoleGuess = !CheatConsoleOpen();
            cheatConsoleInputAt = GetTickCount64();
            cheatConsoleEpoch = g_worldEpoch.load();
        } else if (cheatConsoleGuess && (vk == VK_RETURN || vk == VK_ESCAPE)) {
            cheatConsoleGuess = false;
        }
    }

    // The configurable peek and hold-to-compare keys belong to the menu while the pointer is over it.
    bool CaptureKey(WPARAM vk) override {
        const auto ui = ApexConfig::GetUi();
        const ApexConfig::KeyChord screenshot = Hotkeys::Key(Hotkeys::Action::Screenshot);
        const bool bareScreenshotKey = ui.screenshotShortcutEnabled && screenshot.vk == vk &&
                                       !screenshot.ctrl && !screenshot.shift && !screenshot.alt;
        const bool holdKey = vk == ui.peekKey.vk && HoldShortcutDown(ui.peekKey);
        const bool pictureKey = vk == ui.pictureCompareKey.vk && HoldShortcutDown(ui.pictureCompareKey);
        return (g_keysOverMenu.load() && (holdKey || pictureKey)) ||
               (Overlay::IsVisible() && !CheatConsoleOpen() && !g_menuTextInput.load() && bareScreenshotKey);
    }

    // While a shortcut records, every key press is eaten (no shortcut fires, the game sees nothing)
    bool HotkeyDown(WPARAM vk, bool repeat) override {
        if (!g_menuAvailable.load()) return false;
        if (CheatConsoleOpen()) return false; // don't steal letters from a cheat being typed
        if (vk == VK_ESCAPE && LightProbe::Aiming()) { LightProbe::CancelAim(); return true; }
        const auto ui = ApexConfig::GetUi();
        const bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0, alt = GetKeyState(VK_MENU) < 0;
        if (g_recRow < 0 && Overlay::IsVisible() && g_menuFocused && !g_menuTextInput.load() && vk == ui.searchKey.vk &&
            ui.searchKey.ctrl == ctrl && ui.searchKey.shift == shift && ui.searchKey.alt == alt) {
            g_focusSearch = true;
            return true;
        }
        const ApexConfig::KeyChord screenshot = Hotkeys::Key(Hotkeys::Action::Screenshot);
        if (g_recRow < 0 && Overlay::IsVisible() && g_menuTextInput.load() && screenshot.vk == vk &&
            !screenshot.ctrl && !screenshot.shift && !screenshot.alt)
            return false; // let ImGui handle typing; its keyboard capture still keeps the key from reaching the game
        const bool aiming = LightProbe::Aiming();
        const bool handled = g_recRow >= 0 || Hotkeys::OnKeyDown(vk, repeat);
        if (aiming && handled && vk == Hotkeys::Key(Hotkeys::Action::Probe).vk) g_returnFromProbe.store(true);
        return handled;
    }
};

GuiClient g_client;

} // namespace

Overlay::Client& Client() { return g_client; }

void SetStartup(Startup state, const std::string& detail) {
    {
        std::lock_guard<std::mutex> lock(g_detailLock);
        g_startupDetail = detail;
    }
    g_startup.store(state);
}

Startup GetStartup() { return g_startup.load(); }

void SetOldStandaloneNotice(const std::string& module) {
    {
        std::lock_guard<std::mutex> lock(g_detailLock);
        g_oldStandaloneModule = module.empty() ? std::string("S3SSApex.asi") : module;
    }
    g_oldStandalone.store(true);
}

} // namespace ApexGui
