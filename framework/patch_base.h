#pragma once
// Apex feature system. Every feature ("patch") derives from ApexPatch, registers itself with APEX_REGISTER_FEATURE and keeps
// its options as registered settings, saved under [patches.<Name>] in ApexRadiance.toml:
//   - Install / Uninstall do the work; Fail() records the reason shown in the menu.
//   - Update() runs on the pump thread every ~10 ms; by default it reinstalls the feature 2 s after a setting changed.
//   - RenderCustomUI() draws the feature's controls while it is on (default: its registered settings);
//     RenderDeveloperUI() its developer-only lines (Developer page, development build).
#include <windows.h>
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <string_view>
#include <utility>
#include <vector>
#include <toml++/toml.hpp>
#include "imgui.h"
#include "apex_log.h"
#include "game_version.h"
#include "memory_patch.h"

// Skips a RenderCustomUI body when no ImGui context exists.
#define SAFE_IMGUI_BEGIN()                                                                                                                                                                                                   \
    if (!ImGui::GetCurrentContext()) return

enum class SettingWidget { InputBox, Slider, Drag };

struct FeatureInfo {
    std::string displayName;
    std::string description; // shown on hover of the feature (ends with the credit)
    std::string category = "General";
    bool experimental = false;
    bool enabledByDefault = false;
    GameVersionMask supportedVersions = VERSION_ALL;
    std::vector<std::string> technicalDetails;
    // Game-code features: on a build outside supportedVersions the feature is still available when this group of
    // addresses was found by signature (game_addresses.h GroupAvailable), e.g. "NightLights"
    const char* gameCodeGroup = nullptr;
};

// One option of a feature: a value owned by the feature, its default, how it is drawn and its TOML key.
class PatchSetting {
  public:
    PatchSetting(std::string key, std::string description) : key_(std::move(key)), description_(std::move(description)) {}
    virtual ~PatchSetting() = default;
    const std::string& Key() const { return key_; }
    const std::string& Description() const { return description_; }
    virtual void Reset() = 0;
    virtual void Save(toml::table& table) const = 0;
    // Writes the default value under the key (looks, "is this the default" checks)
    virtual void SaveDefault(toml::table& table) const = 0;
    virtual void Load(const toml::table& table) = 0;
    virtual bool Draw() = 0; // true when the user changed the value

  protected:
    std::string key_;
    std::string description_;
};

class ApexPatch {
  public:
    static constexpr auto SETTING_CHANGE_DEBOUNCE = std::chrono::seconds(2);

    explicit ApexPatch(std::string name, void* reserved = nullptr);
    virtual ~ApexPatch();
    ApexPatch(const ApexPatch&) = delete;
    ApexPatch& operator=(const ApexPatch&) = delete;

    virtual bool Install() = 0;
    virtual bool Uninstall() = 0;
    virtual void Update();
    virtual void RenderCustomUI();
    // Status lines, GPU cost and debug views for the menu's Developer page (development build only; default: nothing)
    virtual void RenderDeveloperUI() {}
    virtual void SaveToToml(toml::table& table) const;
    virtual bool LoadFromToml(const toml::table& table);
    // Applies a [patches.<Name>] table while the game runs (profiles, looks, undo; render thread, from the menu): the
    // settings it has (NotifySettingChanged when one differs), then "enabled" (Install / Uninstall when it differs). Keys
    // it does not have stay as they are. Features whose options install parts live override it (Night Lights).
    virtual void ApplyTableLive(const toml::table& table);
    // Every registered setting at its default (no "enabled" key)
    void DefaultsToToml(toml::table& table) const;
    // GPU time of the feature per frame in ms, measured with timestamp queries; < 0 = not measured (or off)
    virtual float GpuCostMs() const { return -1.0f; }
    // Cheap read-only menu summary; never allocates resources or serializes settings.
    virtual const char* OverviewSummary() const { return nullptr; }

    // A setting changed in the menu: reinstall after SETTING_CHANGE_DEBOUNCE (see Update) and save the config.
    void NotifySettingChanged();

    const std::string& GetName() const { return patchName; }
    bool IsEnabled() const { return isEnabled.load(); }
    const std::string& GetLastError() const { return lastError; }
    const FeatureInfo* GetMetadata() const { return metadata_.get(); }
    void SetMetadata(const FeatureInfo& meta);
    bool IsCompatibleWithCurrentVersion() const;
    // Why the feature cannot run on this build ("Not available on <version>[: missing <addresses>]")
    std::string UnavailableReason() const;
    bool IsEnabledByDefault() const { return metadata_ && metadata_->enabledByDefault; }
    bool EnabledStateFromConfig() const { return enabledFromConfig_; }
    // TOML keys of the registered settings (config migration keeps only these)
    std::vector<std::string> SettingKeys() const {
        std::vector<std::string> keys;
        for (const auto& s : settings) keys.push_back(s->Key());
        return keys;
    }

  protected:
    bool Fail(const std::string& message);

    void RegisterFloatSetting(float* value, const std::string& key, SettingWidget ui, float def, float min, float max, const std::string& description = "",
                              const std::vector<std::pair<std::string, float>>& presets = {});
    void RegisterIntSetting(int* value, const std::string& key, int def, int min, int max, const std::string& description = "",
                            const std::vector<std::pair<std::string, int>>& presets = {}, SettingWidget ui = SettingWidget::Slider);
    void RegisterBoolSetting(bool* value, const std::string& key, bool def, const std::string& description = "");
    void RegisterEnumSetting(int* value, const std::string& key, int def, const std::string& description, const std::vector<std::string>& choices);

    std::string patchName;
    std::atomic<bool> isEnabled{false};
    std::string lastError;
    std::atomic<bool> pendingReinstall{false};
    std::atomic<std::chrono::steady_clock::time_point> lastSettingChange{};
    std::vector<std::unique_ptr<PatchSetting>> settings;

  private:
    std::unique_ptr<FeatureInfo> metadata_;
    bool enabledFromConfig_ = false;
};

// All features, created once at startup from the registrations.
class PatchManager {
  public:
    static PatchManager& Get();

    void CreateAll(); // instantiates every APEX_REGISTER_FEATURE (once)
    const std::vector<std::unique_ptr<ApexPatch>>& GetPatches() const { return patches_; }
    ApexPatch* Find(std::string_view name) const;

    // [patches.<Name>] tables of the config root
    void SaveToToml(toml::table& root) const;
    void LoadFromToml(const toml::table& root);
    // Features marked enabledByDefault that the config did not mention
    void EnableDefaults();
    void UpdateAll();    // pump thread
    void UninstallAll(); // FreeLibrary only

    bool HasUnsavedChanges() const { return unsaved_.load(); }
    void SetUnsavedChanges(bool unsaved);
    // Time of the last SetUnsavedChanges(true) (autosave waits a moment after it)
    std::chrono::steady_clock::time_point LastChange() const { return lastChange_.load(); }

  private:
    PatchManager() = default;
    std::vector<std::unique_ptr<ApexPatch>> patches_;
    bool created_ = false;
    std::atomic<bool> unsaved_{false};
    std::atomic<std::chrono::steady_clock::time_point> lastChange_{};
};

// Registration (static objects in each feature's .cpp)
using PatchFactory = std::unique_ptr<ApexPatch> (*)();
struct PatchRegistration {
    PatchRegistration(PatchFactory factory, FeatureInfo metadata);
};

#define APEX_PATCH_CONCAT_INNER(a, b) a##b
#define APEX_PATCH_CONCAT(a, b) APEX_PATCH_CONCAT_INNER(a, b)
// APEX_REGISTER_FEATURE(ClassName, {.displayName = ..., .description = ..., ...});
#define APEX_REGISTER_FEATURE(ClassName, ...)                                                                                                                                                                                       \
    static const PatchRegistration APEX_PATCH_CONCAT(s_patchRegistration_, ClassName)([]() -> std::unique_ptr<ApexPatch> { return std::make_unique<ClassName>(); }, FeatureInfo __VA_ARGS__);
