#include "ui/widgets.h"
#include "build_flavor.h"
#include "patch_base.h"
#include "game_addresses.h"
#include "ui/i18n.h"
#include <algorithm>
#include <cfloat>
#include <cmath>
#include <format>
#include <mutex>

// ---------------------------------------------------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------------------------------------------------
namespace {

void Tooltip(const std::string& text) {
    if (!text.empty() && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
        ImGui::BeginTooltip();
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 30.0f);
        ImGui::TextUnformatted(text.c_str());
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
    }
}

template <typename T> T Clamp(T v, T lo, T hi) { return v < lo ? lo : (v > hi ? hi : v); }

class FloatOption final : public PatchSetting {
  public:
    FloatOption(float* v, std::string key, SettingWidget ui, float def, float lo, float hi, std::string desc, std::vector<std::pair<std::string, float>> presets)
        : PatchSetting(std::move(key), std::move(desc)), v_(v), ui_(ui), def_(def), lo_(lo), hi_(hi), presets_(std::move(presets)) {
        *v_ = def_;
    }
    void Reset() override { *v_ = def_; }
    void Save(toml::table& t) const override { t.insert_or_assign(key_, static_cast<double>(*v_)); }
    void SaveDefault(toml::table& t) const override { t.insert_or_assign(key_, static_cast<double>(def_)); }
    void Load(const toml::table& t) override {
        if (auto d = t[key_].value<double>()) {
            if (std::isfinite(*d)) *v_ = static_cast<float>(Clamp(*d, static_cast<double>(lo_), static_cast<double>(hi_)));
        }
    }
    bool Draw() override {
        bool changed = false;
        const std::string id = "##" + key_;
        ImGui::TextUnformatted(description_.empty() ? key_.c_str() : description_.c_str());
        for (const auto& [label, value] : presets_) {
            if (ApexUi::TextButton((label + id).c_str())) {
                *v_ = value;
                changed = true;
            }
            ImGui::SameLine();
        }
        if (!presets_.empty()) ImGui::NewLine();
        ImGui::SetNextItemWidth(-FLT_MIN);
        switch (ui_) {
        case SettingWidget::Slider: changed |= ImGui::SliderFloat(id.c_str(), v_, lo_, hi_, "%.3f"); break;
        case SettingWidget::Drag: changed |= ImGui::DragFloat(id.c_str(), v_, (hi_ - lo_) / 200.0f, lo_, hi_, "%.3f"); break;
        case SettingWidget::InputBox:
            if (ImGui::InputFloat(id.c_str(), v_, 0.0f, 0.0f, "%.3f")) {
                *v_ = Clamp(*v_, lo_, hi_);
                changed = true;
            }
            break;
        }
        return changed;
    }

  private:
    float* v_;
    SettingWidget ui_;
    float def_, lo_, hi_;
    std::vector<std::pair<std::string, float>> presets_;
};

class IntOption final : public PatchSetting {
  public:
    IntOption(int* v, std::string key, SettingWidget ui, int def, int lo, int hi, std::string desc, std::vector<std::pair<std::string, int>> presets)
        : PatchSetting(std::move(key), std::move(desc)), v_(v), ui_(ui), def_(def), lo_(lo), hi_(hi), presets_(std::move(presets)) {
        *v_ = def_;
    }
    void Reset() override { *v_ = def_; }
    void Save(toml::table& t) const override { t.insert_or_assign(key_, static_cast<int64_t>(*v_)); }
    void SaveDefault(toml::table& t) const override { t.insert_or_assign(key_, static_cast<int64_t>(def_)); }
    void Load(const toml::table& t) override {
        if (auto i = t[key_].value<int64_t>()) *v_ = static_cast<int>(Clamp<int64_t>(*i, lo_, hi_));
    }
    bool Draw() override {
        bool changed = false;
        const std::string id = "##" + key_;
        ImGui::TextUnformatted(description_.empty() ? key_.c_str() : description_.c_str());
        for (const auto& [label, value] : presets_) {
            if (ApexUi::TextButton((label + id).c_str())) {
                *v_ = value;
                changed = true;
            }
            ImGui::SameLine();
        }
        if (!presets_.empty()) ImGui::NewLine();
        ImGui::SetNextItemWidth(-FLT_MIN);
        switch (ui_) {
        case SettingWidget::Slider: changed |= ImGui::SliderInt(id.c_str(), v_, lo_, hi_); break;
        case SettingWidget::Drag: changed |= ImGui::DragInt(id.c_str(), v_, 1.0f, lo_, hi_); break;
        case SettingWidget::InputBox:
            if (ImGui::InputInt(id.c_str(), v_)) {
                *v_ = Clamp(*v_, lo_, hi_);
                changed = true;
            }
            break;
        }
        return changed;
    }

  private:
    int* v_;
    SettingWidget ui_;
    int def_, lo_, hi_;
    std::vector<std::pair<std::string, int>> presets_;
};

class BoolOption final : public PatchSetting {
  public:
    BoolOption(bool* v, std::string key, bool def, std::string desc) : PatchSetting(std::move(key), std::move(desc)), v_(v), def_(def) { *v_ = def_; }
    void Reset() override { *v_ = def_; }
    void Save(toml::table& t) const override { t.insert_or_assign(key_, *v_); }
    void SaveDefault(toml::table& t) const override { t.insert_or_assign(key_, def_); }
    void Load(const toml::table& t) override {
        if (auto b = t[key_].value<bool>()) *v_ = *b;
    }
    bool Draw() override {
        const std::string label = (description_.empty() ? key_ : description_) + "##" + key_;
        const bool changed = ApexUi::Checkbox(label.c_str(), v_);
        Tooltip("Config key: " + key_);
        return changed;
    }

  private:
    bool* v_;
    bool def_;
};

class EnumOption final : public PatchSetting {
  public:
    EnumOption(int* v, std::string key, int def, std::string desc, std::vector<std::string> choices)
        : PatchSetting(std::move(key), std::move(desc)), v_(v), def_(def), choices_(std::move(choices)) {
        *v_ = def_;
    }
    void Reset() override { *v_ = def_; }
    void Save(toml::table& t) const override { t.insert_or_assign(key_, static_cast<int64_t>(*v_)); }
    void SaveDefault(toml::table& t) const override { t.insert_or_assign(key_, static_cast<int64_t>(def_)); }
    void Load(const toml::table& t) override {
        if (auto i = t[key_].value<int64_t>(); i && *i >= 0 && *i < static_cast<int64_t>(choices_.size())) *v_ = static_cast<int>(*i);
    }
    bool Draw() override {
        bool changed = false;
        ImGui::TextUnformatted(description_.empty() ? key_.c_str() : description_.c_str());
        const char* current = (*v_ >= 0 && *v_ < static_cast<int>(choices_.size())) ? choices_[*v_].c_str() : "?";
        ImGui::SetNextItemWidth(-FLT_MIN);
        if (ImGui::BeginCombo(("##" + key_).c_str(), current)) {
            for (int i = 0; i < static_cast<int>(choices_.size()); i++) {
                const bool selected = *v_ == i;
                if (ImGui::Selectable(choices_[i].c_str(), selected)) {
                    *v_ = i;
                    changed = true;
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        return changed;
    }

  private:
    int* v_;
    int def_;
    std::vector<std::string> choices_;
};

} // namespace

// ---------------------------------------------------------------------------------------------------------------------
// ApexPatch
// ---------------------------------------------------------------------------------------------------------------------
ApexPatch::ApexPatch(std::string name, void*) : patchName(std::move(name)) {}
ApexPatch::~ApexPatch() = default;

void ApexPatch::SetMetadata(const FeatureInfo& meta) { metadata_ = std::make_unique<FeatureInfo>(meta); }

bool ApexPatch::IsCompatibleWithCurrentVersion() const {
    if (!metadata_ || IsVersionSupported(metadata_->supportedVersions)) return true;
    return metadata_->gameCodeGroup && GameAddr::GroupAvailable(metadata_->gameCodeGroup);
}

namespace {
// The game-code names a feature's group lacks ("" without a group or before the game code was scanned)
std::string MissingGameCode(const FeatureInfo* meta) {
    std::string missing;
    if (meta && meta->gameCodeGroup && GameAddr::Resolved()) GameAddr::GroupAvailable(meta->gameCodeGroup, &missing);
    return missing;
}

// UnavailableReason in English, for Fail (the log and lastError stay English)
std::string UnavailableReasonEnglish(const ApexPatch& patch) { return GameAddr::NotAvailable(MissingGameCode(patch.GetMetadata())); }
} // namespace

// The menu's note: GameAddr::NotAvailable's text in the menu language
std::string ApexPatch::UnavailableReason() const {
    const std::string missing = MissingGameCode(metadata_.get());
    const char* version = g_gameVersion == GameVersion::Unknown ? I18n::Tr(GetGameVersionName()) : GetGameVersionName(); // "Unknown" translates
    if (!GameAddr::Resolved()) return I18n::Trf("Not available on {} (game code not scanned yet)", version);
    return missing.empty() ? I18n::Trf("Not available on {}", version) : I18n::Trf("Not available on {}: missing {}", version, missing);
}

bool ApexPatch::Fail(const std::string& message) {
    lastError = message;
    LOG_ERROR("[" + patchName + "] " + message);
    return false;
}

void ApexPatch::RegisterFloatSetting(float* value, const std::string& key, SettingWidget ui, float def, float min, float max, const std::string& description,
                                     const std::vector<std::pair<std::string, float>>& presets) {
    settings.push_back(std::make_unique<FloatOption>(value, key, ui, def, min, max, description, presets));
}

void ApexPatch::RegisterIntSetting(int* value, const std::string& key, int def, int min, int max, const std::string& description,
                                   const std::vector<std::pair<std::string, int>>& presets, SettingWidget ui) {
    settings.push_back(std::make_unique<IntOption>(value, key, ui, def, min, max, description, presets));
}

void ApexPatch::RegisterBoolSetting(bool* value, const std::string& key, bool def, const std::string& description) {
    settings.push_back(std::make_unique<BoolOption>(value, key, def, description));
}

void ApexPatch::RegisterEnumSetting(int* value, const std::string& key, int def, const std::string& description, const std::vector<std::string>& choices) {
    settings.push_back(std::make_unique<EnumOption>(value, key, def, description, choices));
}

void ApexPatch::NotifySettingChanged() {
    lastSettingChange.store(std::chrono::steady_clock::now());
    pendingReinstall = true;
    PatchManager::Get().SetUnsavedChanges(true);
}

void ApexPatch::Update() {
    if (!pendingReinstall || !isEnabled.load()) return;
    if (std::chrono::steady_clock::now() - lastSettingChange.load() < SETTING_CHANGE_DEBOUNCE) return;
    pendingReinstall = false;
    LOG_INFO("[" + patchName + "] Reinstalling after a setting change");
    if (Uninstall()) Install();
}

void ApexPatch::RenderCustomUI() {
    SAFE_IMGUI_BEGIN();
    bool changed = false;
    for (auto& s : settings) {
        ImGui::PushID(s->Key().c_str());
        changed |= s->Draw();
        ImGui::PopID();
    }
    if (changed) NotifySettingChanged();
}

void ApexPatch::SaveToToml(toml::table& table) const {
    table.insert_or_assign("enabled", isEnabled.load());
    for (const auto& s : settings) s->Save(table);
}

void ApexPatch::DefaultsToToml(toml::table& table) const {
    for (const auto& s : settings) s->SaveDefault(table);
}

void ApexPatch::ApplyTableLive(const toml::table& table) {
    toml::table before, after;
    for (const auto& s : settings) s->Save(before);
    for (auto& s : settings) s->Load(table);
    for (const auto& s : settings) s->Save(after);
    if (before != after) NotifySettingChanged();
    const auto enabled = table["enabled"].value<bool>();
    if (!enabled || *enabled == isEnabled.load()) return;
    if (*enabled && !IsCompatibleWithCurrentVersion()) {
        Fail(UnavailableReasonEnglish(*this));
        return;
    }
    if (*enabled ? Install() : Uninstall()) PatchManager::Get().SetUnsavedChanges(true);
}

bool ApexPatch::LoadFromToml(const toml::table& table) {
    for (auto& s : settings) s->Load(table);
    const auto enabled = table["enabled"].value<bool>();
    if (!enabled) return true;
    enabledFromConfig_ = true;
    if (*enabled == isEnabled.load()) return true;
    if (*enabled && !IsCompatibleWithCurrentVersion()) return Fail(UnavailableReasonEnglish(*this));
    return *enabled ? Install() : Uninstall();
}

// ---------------------------------------------------------------------------------------------------------------------
// Registration and PatchManager
// ---------------------------------------------------------------------------------------------------------------------
namespace {
struct Registration {
    PatchFactory factory;
    FeatureInfo metadata;
};
std::vector<Registration>& Registrations() {
    static std::vector<Registration> list; // filled by static initialisers, before DllMain
    return list;
}
} // namespace

PatchRegistration::PatchRegistration(PatchFactory factory, FeatureInfo metadata) { Registrations().push_back({factory, std::move(metadata)}); }

PatchManager& PatchManager::Get() {
    static PatchManager instance;
    return instance;
}

void PatchManager::CreateAll() {
    if (created_) return;
    created_ = true;
    for (const Registration& r : Registrations()) {
        std::unique_ptr<ApexPatch> p;
        try {
            p = r.factory();
        } catch (const std::exception& e) {
            LOG_ERROR(std::format("[Patches] Creating {} failed: {}", r.metadata.displayName, e.what()));
            continue;
        }
        if (!p) continue;
        if (Find(p->GetName())) {
            LOG_WARNING("[Patches] Duplicate feature name ignored: " + p->GetName());
            continue;
        }
        p->SetMetadata(r.metadata);
        patches_.push_back(std::move(p));
    }
    std::string names;
    for (const auto& p : patches_) names += (names.empty() ? "" : ", ") + p->GetName();
    LOG_INFO(std::format("[Patches] {} features: {}", patches_.size(), names));
}

ApexPatch* PatchManager::Find(std::string_view name) const {
    for (const auto& p : patches_)
        if (p->GetName() == name) return p.get();
    return nullptr;
}

void PatchManager::SaveToToml(toml::table& root) const {
    toml::table all;
    if (auto existing = root["patches"].as_table()) all = *existing; // keep tables of features this build does not have
    for (const auto& p : patches_) {
        toml::table t;
        if (auto old = all[p->GetName()].as_table()) t = *old;
        p->SaveToToml(t);
        all.insert_or_assign(p->GetName(), std::move(t));
    }
    root.insert_or_assign("patches", std::move(all));
}

void PatchManager::LoadFromToml(const toml::table& root) {
    const toml::table* all = root["patches"].as_table();
    if (!all) return;
    for (const auto& p : patches_) {
        if (kPublicBuild && p->GetName() == "FrameCapture") continue;
        const toml::table* t = (*all)[p->GetName()].as_table();
        if (!t) continue;
        try {
            if (!p->LoadFromToml(*t)) LOG_WARNING(std::format("[Patches] {} did not start: {}", p->GetName(), p->GetLastError()));
        } catch (const std::exception& e) {
            LOG_ERROR(std::format("[Patches] Loading {} failed: {}", p->GetName(), e.what()));
        }
    }
}

void PatchManager::EnableDefaults() {
    for (const auto& p : patches_) {
        if (p->EnabledStateFromConfig() || p->IsEnabled() || !p->IsEnabledByDefault() || !p->IsCompatibleWithCurrentVersion()) continue;
        if (!p->Install()) LOG_WARNING(std::format("[Patches] {} (on by default) did not start: {}", p->GetName(), p->GetLastError()));
    }
}

void PatchManager::UpdateAll() {
    for (const auto& p : patches_) {
        try {
            p->Update();
        } catch (const std::exception& e) {
            LOG_ERROR(std::format("[Patches] {} Update failed: {}", p->GetName(), e.what()));
        } catch (...) {
            LOG_ERROR(std::format("[Patches] {} Update failed", p->GetName()));
        }
    }
}

void PatchManager::UninstallAll() {
    for (auto it = patches_.rbegin(); it != patches_.rend(); ++it)
        if ((*it)->IsEnabled()) (*it)->Uninstall();
}

void PatchManager::SetUnsavedChanges(bool unsaved) {
    unsaved_.store(unsaved);
    if (unsaved) lastChange_.store(std::chrono::steady_clock::now());
}
