#include "developer_settings.h"
#include "apex_config.h"
#include "apex_version.h"
#include "apex_log.h"
#include "apex_paths.h"
#include "apex_util.h"
#include "build_flavor.h"
#include "patch_base.h"
#include "picture.h"
#include "frame_profiler.h"
#include "performance.h"
#include "ui/i18n.h"
#include <toml++/toml.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <format>
#include <mutex>
#include <sstream>

namespace ApexConfig {

// Features the Compare shortcut has off right now: saved as on (SetCompareOverride)
std::vector<std::string> g_compareOff;
bool g_comparePictureOff = false;
namespace {

std::mutex g_fileLock; // one reader/writer of ApexRadiance.toml at a time
std::mutex g_uiLock;
UiSettings g_ui;
toml::table g_importedDeveloper; // pending preferences imported while restart is required
std::atomic<bool> g_saveRequested{false};
std::atomic<std::chrono::steady_clock::time_point> g_saveRequestedAt{};
std::once_flag g_migrateOnce;
std::string g_migrationNote = "not run";
std::mutex g_noteLock;

constexpr int kConfigVersion = 1;
constexpr auto kSaveDelay = std::chrono::milliseconds(1000);

// Features whose tables come over from S3SS.toml (the others were removed from the standalone: their tables are dropped)
const char* const kMigratedPatches[] = {"NightTerrainRelight", "EdgeSmoothing", "DepthBlur", "FrameCapture"};

void SetNote(std::string note) {
    std::lock_guard<std::mutex> lock(g_noteLock);
    g_migrationNote = std::move(note);
}

std::string Serialize(const toml::table& root) {
    std::ostringstream ss;
    ss << root;
    return ss.str();
}

bool ParseFile(const std::wstring& path, toml::table& out, std::string* error) {
    std::string text;
    if (!ApexUtil::ReadFileBytes(path, text)) return false;
    try {
        out = toml::parse(text);
        return true;
    } catch (const toml::parse_error& e) {
        if (error) *error = std::string(e.description());
        return false;
    }
}

// Copies the keys `keys` of src into dst (only those present).
template <typename Keys> void CopyKeys(const toml::table& src, toml::table& dst, const Keys& keys) {
    for (const auto& k : keys)
        if (const toml::node* n = src.get(k)) dst.insert_or_assign(k, *n);
}

// The previous standalone build (S3SSApex.asi) kept the same schema in Documents\...\S3SS\Apex\Apex.toml: copied as it
// is (every key); its menu layout (apex_imgui.ini) is not. The old folder is left in place. Returns false when there is
// no such file (the caller then migrates from S3SS.toml); true when it was handled, including a failed copy (nothing
// written: the next start tries again, and this session runs on defaults).
bool CopyFromPreviousBuild(const std::wstring& apexFile) {
    const std::wstring legacyFile = ApexPaths::LegacyConfigFile();
    if (legacyFile.empty() || !ApexUtil::FileExists(legacyFile)) return false;
    const std::string legacyUtf8 = ApexUtil::ToUtf8(legacyFile);
    std::string raw;
    if (!ApexUtil::ReadFileBytes(legacyFile, raw)) {
        SetNote("the previous build's " + legacyUtf8 + " could not be read: defaults for now (it is tried again at the next start)");
        LOG_ERROR("[Config] Migration: " + legacyUtf8 + " exists but could not be read; nothing written, retried at the next start");
        return true;
    }
    std::string err;
    bool written;
    {
        std::lock_guard<std::mutex> lock(g_fileLock);
        written = ApexUtil::WriteFileAtomic(apexFile, raw, &err);
    }
    if (!written) {
        SetNote("copying the previous build's Apex.toml failed (" + err + "): defaults for now (it is tried again at the next start)");
        LOG_ERROR("[Config] Migration: could not write " + ApexUtil::ToUtf8(apexFile) + ": " + err);
        return true;
    }
    // The menu layout (apex_imgui.ini) is not copied: the menu's scale changed, so the new default window size applies.
    const std::string note = "copied from the previous build's S3SS\\Apex\\Apex.toml (all settings kept; the old folder was left in place)";
    SetNote(note);
    LOG_INFO("[Config] Migration path: previous standalone build. " + legacyUtf8 + " -> " + ApexUtil::ToUtf8(apexFile));
    LOG_INFO("[Config] " + note);
    return true;
}

void DoMigrate() {
    if (!ApexPaths::EnsureApexDirectory()) {
        SetNote("the " APEX_PRODUCT_NAME " folder could not be created");
        LOG_ERROR("[Config] Cannot create " + ApexUtil::ToUtf8(ApexPaths::ApexDirectory()));
        return;
    }
    const std::wstring apexFile = ApexPaths::ConfigFile();
    if (ApexUtil::FileExists(apexFile)) {
        SetNote("ApexRadiance.toml already exists (no migration needed)");
        LOG_INFO("[Config] Migration path: none (ApexRadiance.toml already exists)");
        return;
    }
    if (CopyFromPreviousBuild(apexFile)) return;

    LOG_INFO("[Config] Migration path: no previous S3SS\\Apex\\Apex.toml; reading S3SS.toml (combined build) if present");
    toml::table out;
    toml::table meta;
    meta.insert("version", kConfigVersion);
    meta.insert("created_by", APEX_PRODUCT_NAME " " APEX_VERSION_STRING);
    std::string note;

    std::string raw;
    if (!ApexUtil::ReadFileBytes(ApexPaths::S3SSConfigFile(), raw)) {
        meta.insert("migrated_from_s3ss", false);
        note = "fresh install (no previous Apex.toml or S3SS.toml): defaults";
    } else {
        // 1. keep the old file exactly as it was (once)
        const std::wstring backup = ApexPaths::MigrationBackup();
        if (!ApexUtil::FileExists(backup)) {
            std::string err;
            if (!ApexUtil::WriteFileAtomic(backup, raw, &err)) LOG_WARNING("[Config] Could not back up S3SS.toml: " + err);
        }
        // 2. carry over only what this build reads
        toml::table src;
        try {
            src = toml::parse(raw);
        } catch (const toml::parse_error& e) {
            meta.insert("migrated_from_s3ss", false);
            note = std::string("S3SS.toml could not be read (") + std::string(e.description()) + "): defaults; the original is in S3SS.toml.pre-split.bak";
        }
        if (note.empty()) {
            std::vector<std::string> carried, missing;
            toml::table qol;
            if (const toml::table* sq = src["qol"].as_table()) {
                size_t n = 0;
                const char* const* keys = Picture::Keys(n);
                if (const toml::table* p = (*sq)["picture"].as_table()) {
                    toml::table pic;
                    for (size_t i = 0; i < n; i++)
                        if (const toml::node* v = p->get(keys[i])) pic.insert_or_assign(keys[i], *v);
                    qol.insert_or_assign("picture", std::move(pic));
                    carried.push_back("qol.picture");
                } else if (const toml::table* h = (*sq)["hdr"].as_table()) {
                    // settings from before the Picture section existed: the grading lived in [qol.hdr]
                    toml::table pic;
                    pic.insert("enabled", (*h)["enabled"].value_or(false));
                    for (const char* k : {"exposure", "contrast", "midtones", "shadows", "highlights", "blacks", "temperature", "tint", "saturation", "vibrance", "deband", "sharpen"})
                        if (const toml::node* v = h->get(k)) pic.insert_or_assign(k, *v);
                    qol.insert_or_assign("picture", std::move(pic));
                    carried.push_back("qol.picture (from the old grading in qol.hdr)");
                } else {
                    missing.push_back("qol.picture");
                }
                if (const toml::table* fp = (*sq)["frame_profiler"].as_table()) {
                    qol.insert_or_assign("frame_profiler", *fp);
                    carried.push_back("qol.frame_profiler");
                }
            }
            if (!qol.empty()) out.insert("qol", std::move(qol));

            toml::table patches;
            const toml::table* sp = src["patches"].as_table();
            for (const char* name : kMigratedPatches) {
                const toml::table* t = sp ? (*sp)[name].as_table() : nullptr;
                ApexPatch* patch = PatchManager::Get().Find(name);
                if (!t || !patch) {
                    if (!t) missing.push_back(std::string("patches.") + name);
                    continue;
                }
                toml::table dst;
                if (const toml::node* en = t->get("enabled")) dst.insert_or_assign("enabled", *en);
                CopyKeys(*t, dst, patch->SettingKeys());
                if (std::string_view(name) == "NightTerrainRelight") {
                    // Strengths retuned after v0.1.0 for lighting changes this build does not have: back to v0.1.0's values.
                    dst.insert_or_assign("forcaNosObjetos", 0.57);
                    dst.insert_or_assign("forcaLuzPorPixelNosObjetos", 1.0);
                    dst.insert_or_assign("forcaNasCercas", 1.0);
                }
                patches.insert_or_assign(name, std::move(dst));
                carried.push_back(std::string("patches.") + name);
            }
            if (!patches.empty()) out.insert("patches", std::move(patches));
            meta.insert("migrated_from_s3ss", true);
            auto join = [](const std::vector<std::string>& v) {
                std::string s;
                for (const auto& x : v) s += (s.empty() ? "" : ", ") + x;
                return s.empty() ? std::string("none") : s;
            };
            note = "migrated from S3SS.toml (copy in S3SS.toml.pre-split.bak): " + join(carried) + "; not found (defaults): " + join(missing);
        }
    }
    meta.insert("migration", note);
    out.insert("meta", std::move(meta));

    std::string err;
    {
        std::lock_guard<std::mutex> lock(g_fileLock);
        if (!ApexUtil::WriteFileAtomic(apexFile, Serialize(out), &err)) note += " (ApexRadiance.toml could not be written: " + err + ")";
    }
    SetNote(note);
    LOG_INFO("[Config] " + note);
}

// ---- key chords ----
struct NamedKey {
    UINT vk;
    const char* name;
};
constexpr NamedKey kNamedKeys[] = {
    {VK_MENU, "Alt"},
    {VK_INSERT, "Insert"},   {VK_DELETE, "Delete"},    {VK_HOME, "Home"},          {VK_END, "End"},           {VK_PRIOR, "PageUp"},       {VK_NEXT, "PageDown"},
    {VK_PAUSE, "Pause"},     {VK_SCROLL, "ScrollLock"}, {VK_OEM_3, "Backtick"},     {VK_OEM_MINUS, "Minus"},   {VK_OEM_PLUS, "Equals"},    {VK_OEM_4, "LeftBracket"},
    {VK_OEM_6, "RightBracket"}, {VK_OEM_5, "Backslash"}, {VK_OEM_1, "Semicolon"},   {VK_OEM_7, "Quote"},       {VK_OEM_COMMA, "Comma"},    {VK_OEM_PERIOD, "Period"},
    {VK_RETURN, "Enter"},    {VK_SPACE, "Space"},
    {VK_OEM_2, "Slash"},     {VK_MULTIPLY, "NumpadMultiply"}, {VK_ADD, "NumpadPlus"}, {VK_SUBTRACT, "NumpadMinus"}, {VK_DIVIDE, "NumpadDivide"},
};

std::string Upper(std::string s) {
    for (char& c : s) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return s;
}

} // namespace

std::string KeyName(UINT vk) {
    if (vk >= VK_F1 && vk <= VK_F24) return "F" + std::to_string(vk - VK_F1 + 1);
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return std::string(1, static_cast<char>(vk));
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return "Numpad" + std::to_string(vk - VK_NUMPAD0);
    for (const NamedKey& k : kNamedKeys)
        if (k.vk == vk) return k.name;
    return std::format("Key{}", vk);
}

std::string KeyChordText(const KeyChord& c) {
    std::string s;
    if (c.ctrl) s += "Ctrl+";
    if (c.shift) s += "Shift+";
    if (c.alt) s += "Alt+";
    return s + KeyName(c.vk);
}

bool ParseKeyChord(const std::string& text, KeyChord& out) {
    KeyChord c{0, false, false, false};
    std::string rest = text;
    for (;;) {
        const size_t plus = rest.find('+');
        if (plus == std::string::npos) break;
        const std::string mod = Upper(rest.substr(0, plus));
        if (mod == "CTRL" || mod == "CONTROL") c.ctrl = true;
        else if (mod == "SHIFT") c.shift = true;
        else if (mod == "ALT") c.alt = true;
        else return false;
        rest = rest.substr(plus + 1);
    }
    const std::string key = Upper(rest);
    if (key.empty()) return false;
    for (UINT vk = 1; vk < 256 && !c.vk; vk++)
        if (Upper(KeyName(vk)) == key) c.vk = vk;
    if (!c.vk) return false;
    out = c;
    return true;
}

// A bare letter, digit or Space would fire while typing Sim, lot or save names: older saves stored a bare C.
static KeyChord AcceptScreenshotKey(const KeyChord& c) {
    const bool bare = !c.ctrl && !c.shift && !c.alt &&
                      ((c.vk >= 'A' && c.vk <= 'Z') || (c.vk >= '0' && c.vk <= '9') || c.vk == VK_SPACE);
    if (!bare) return c;
    const KeyChord fallback = UiSettings{}.screenshotKey;
    LOG_INFO("[Config] screenshot_key " + KeyChordText(c) + " is a bare typing key; using " + KeyChordText(fallback));
    return fallback;
}

UiSettings GetUi() {
    std::lock_guard<std::mutex> lock(g_uiLock);
    return g_ui;
}

void SetUi(const UiSettings& ui) {
    {
        std::lock_guard<std::mutex> lock(g_uiLock);
        g_ui = ui;
    }
    I18n::SetChoice(ui.language);
    RequestSave();
}

void EnsureMigrated() {
    std::call_once(g_migrateOnce, [] {
        try {
            DoMigrate();
        } catch (const std::exception& e) {
            SetNote(std::string("migration failed: ") + e.what());
            LOG_ERROR("[Config] Migration failed: " + std::string(e.what()));
        }
    });
}

std::string MigrationNote() {
    std::lock_guard<std::mutex> lock(g_noteLock);
    return g_migrationNote;
}

bool ReadRoot(toml::table& out) {
    std::lock_guard<std::mutex> lock(g_fileLock);
    std::string error;
    if (ParseFile(ApexPaths::ConfigFile(), out, &error)) return true;
    if (!error.empty()) LOG_ERROR("[Config] ApexRadiance.toml: " + error);
    out = toml::table{};
    return false;
}

void LoadDeveloperMode() {
    toml::table root;
    ReadRoot(root);
    const auto* ui = root["ui"].as_table();
    kPublicBuild.store(!(ui && (*ui)["developer_mode"].value_or(false)), std::memory_order_relaxed);
}

void LoadSettings() {
    I18n::SetChoice(-1); // Windows' language until [ui] language says otherwise
    toml::table root;
    if (!ReadRoot(root)) {
        LOG_INFO("[Config] No ApexRadiance.toml yet: defaults");
        return;
    }
    if (const toml::table* ui = root["ui"].as_table()) {
        UiSettings u;
        KeyChord chord;
        if (ParseKeyChord((*ui)["toggle_key"].value_or(std::string()), chord)) u.toggle = chord;
        u.fontScale = std::clamp(static_cast<float>((*ui)["font_scale"].value_or(1.0)), 0.5f, 3.0f);
        u.recommendS3SS = (*ui)["recommend_s3ss"].value_or(true);
        u.startNote = (*ui)["start_note"].value_or(true);
        u.captureScreenshot = (*ui)["capture_screenshot"].value_or(true);
        u.developerMode = (*ui)["developer_mode"].value_or(false);
        kPublicBuild.store(!u.developerMode, std::memory_order_relaxed);
        u.welcomeDone = (*ui)["welcome_done"].value_or(false); // retained for compatibility; no longer controls startup UI
        u.keyChosen = (*ui)["key_chosen"].value_or(false);
        u.startProfileDone = (*ui)["start_profile_done"].value_or(true); // only new installations (no [ui] yet) see the welcome page
        const std::string preset = (*ui)["hotkey_preset"].value_or(std::string());
        u.hotkeyPreset = preset == "letters" ? 0 : preset == "numbers" ? 1 : preset == "fkeys" ? 2 : preset == "mine" ? 3 : -1;
        u.minePresetBase = static_cast<int>((*ui)["mine_base"].value_or(int64_t{0}));
        KeyChord own;
        if (ParseKeyChord((*ui)["compare_key"].value_or(std::string()), own)) u.compareKey = own;
        if (ParseKeyChord((*ui)["refresh_key"].value_or(std::string()), own)) u.refreshKey = own;
        if (ParseKeyChord((*ui)["probe_key"].value_or(std::string()), own)) u.probeKey = own;
        if (ParseKeyChord((*ui)["diagnostics_key"].value_or(std::string()), own)) u.diagnosticsKey = own;
        if (ParseKeyChord((*ui)["recorder_key"].value_or(std::string()), own)) u.recorderKey = own;
        if (ParseKeyChord((*ui)["frame_capture_key"].value_or(std::string()), own)) u.frameCaptureKey = own;
        if (ParseKeyChord((*ui)["search_key"].value_or(std::string()), own)) u.searchKey = own;
        if (ParseKeyChord((*ui)["peek_key"].value_or(std::string()), own)) u.peekKey = own;
        if (ParseKeyChord((*ui)["picture_compare_key"].value_or(std::string()), own)) u.pictureCompareKey = own;
        // screenshot_shortcut_enabled is no longer shown in the menu; a saved false would hide the shortcut for good
        if (ParseKeyChord((*ui)["screenshot_key"].value_or(std::string()), own)) u.screenshotKey = AcceptScreenshotKey(own);
        u.screenshotHideGameUi = (*ui)["screenshot_hide_game_ui"].value_or(true);
        u.screenshotToApexFolder = (*ui)["screenshot_folder"].value_or(std::string("game")) == "apex";
        u.sidebarCollapsed = (*ui)["sidebar_collapsed"].value_or(false);
        const std::string lang = (*ui)["language"].value_or(std::string("auto"));
        u.language = lang == "en" ? 0 : lang == "pt" ? 1 : lang == "es" ? 2 : lang == "fr" ? 3 : -1;
        I18n::SetChoice(u.language);
        std::lock_guard<std::mutex> lock(g_uiLock);
        g_ui = u;
    }
    { std::lock_guard<std::mutex> lock(g_uiLock);
        if (const auto* d = root["developer"].as_table()) g_importedDeveloper = *d;
    }
    if (const toml::table* qol = root["qol"].as_table()) {
        Picture::Get().LoadFromToml(*qol);
        toml::table profilerPreferences = *qol;
        if (auto* p = profilerPreferences["frame_profiler"].as_table()) p->insert_or_assign("enabled", false);
        FrameProfiler::LoadFromToml(profilerPreferences); // always start measurements manually
    }
    LOG_INFO("[Config] Settings loaded from ApexRadiance.toml");
}

void ApplyDeveloperPreferences(const toml::table& d) {
    if (kPublicBuild) return;
    if (const auto* controls = d["controls"].as_table()) DeveloperSettings::Apply(*controls);
    if (const auto* patches = d["patches"].as_table()) {
        for (const auto& p : PatchManager::Get().GetPatches()) {
            if (p->GetName() == "FrameCapture") continue;
            if (const auto* t = (*patches)[p->GetName()].as_table()) {
                toml::table preferences = *t; preferences.erase("enabled");
                p->ApplyTableLive(preferences);
            }
        }
    }
}

void LoadFeatures() {
    toml::table root;
    ReadRoot(root);
    PatchManager::Get().LoadFromToml(root);
    PatchManager::Get().EnableDefaults();
    if (const auto* d = root["developer"].as_table()) ApplyDeveloperPreferences(*d);
    PatchManager::Get().SetUnsavedChanges(false);
}

bool Save(std::string* error) {
    try {
        toml::table root;
        ReadRoot(root); // keeps what this build does not write (tables of features it does not have)
        toml::table meta;
        if (const toml::table* old = root["meta"].as_table()) meta = *old;
        meta.insert_or_assign("version", kConfigVersion);
        meta.insert_or_assign("written_by", APEX_PRODUCT_NAME " " APEX_VERSION_STRING);
        root.insert_or_assign("meta", std::move(meta));

        const UiSettings u = GetUi();
        toml::table ui;
        ui.insert("toggle_key", KeyChordText(u.toggle));
        ui.insert("font_scale", static_cast<double>(u.fontScale));
        ui.insert("recommend_s3ss", u.recommendS3SS);
        ui.insert("start_note", u.startNote);
        ui.insert("capture_screenshot", u.captureScreenshot);
        ui.insert("developer_mode", u.developerMode);
        ui.insert("welcome_done", u.welcomeDone);
        ui.insert("key_chosen", u.keyChosen);
        ui.insert("start_profile_done", u.startProfileDone);
        static constexpr const char* kPresetKeys[] = {"letters", "numbers", "fkeys", "mine"};
        if (u.hotkeyPreset >= 0 && u.hotkeyPreset < 4) ui.insert("hotkey_preset", kPresetKeys[u.hotkeyPreset]);
        if (u.hotkeyPreset == 3) ui.insert("mine_base", static_cast<int64_t>(u.minePresetBase));
        if (u.compareKey.vk) ui.insert("compare_key", KeyChordText(u.compareKey));
        if (u.refreshKey.vk) ui.insert("refresh_key", KeyChordText(u.refreshKey));
        if (u.probeKey.vk) ui.insert("probe_key", KeyChordText(u.probeKey));
        if (u.diagnosticsKey.vk) ui.insert("diagnostics_key", KeyChordText(u.diagnosticsKey));
        if (u.recorderKey.vk) ui.insert("recorder_key", KeyChordText(u.recorderKey));
        if (u.frameCaptureKey.vk) ui.insert("frame_capture_key", KeyChordText(u.frameCaptureKey));
        ui.insert("search_key", KeyChordText(u.searchKey));
        ui.insert("peek_key", KeyChordText(u.peekKey));
        ui.insert("picture_compare_key", KeyChordText(u.pictureCompareKey));
        ui.insert("screenshot_shortcut_enabled", u.screenshotShortcutEnabled);
        ui.insert("screenshot_key", KeyChordText(u.screenshotKey));
        ui.insert("screenshot_hide_game_ui", u.screenshotHideGameUi);
        ui.insert("screenshot_folder", std::string(u.screenshotToApexFolder ? "apex" : "game"));
        ui.insert("sidebar_collapsed", u.sidebarCollapsed);
        static constexpr const char* kLanguageKeys[] = {"en", "pt", "es", "fr"};
        ui.insert("language", u.language >= 0 && u.language < 4 ? kLanguageKeys[u.language] : "auto");
        root.insert_or_assign("ui", std::move(ui));


        toml::table qol;
        if (const toml::table* old = root["qol"].as_table()) qol = *old; // e.g. [qol.frame_profiler] in a public build
        Picture::Get().SaveToToml(qol);
        if (g_comparePictureOff)
            if (toml::table* pic = qol["picture"].as_table()) pic->insert_or_assign("enabled", true);
        if (!kPublicBuild) {
            qol.erase("frame_profiler"); // FrameProfiler::SaveToToml inserts (it does not replace)
            FrameProfiler::SaveToToml(qol);
        }
        root.insert_or_assign("qol", std::move(qol));

        {
            toml::table imported;
            { std::lock_guard<std::mutex> lock(g_uiLock); imported = g_importedDeveloper; }
            if (!imported.empty()) {
                root.insert_or_assign("developer", imported);
                if (auto* p = imported["frame_profiler"].as_table()) {
                    auto* q = root["qol"].as_table();
                    if (q) { q->insert_or_assign("frame_profiler", *p); (*q)["frame_profiler"].as_table()->insert_or_assign("enabled", false); }
                }
            }
            if (!kPublicBuild && u.developerMode) {
                toml::table d; d.insert("controls", DeveloperSettings::Capture());
                root.insert_or_assign("developer", std::move(d));
            }
        }
        PatchManager::Get().SaveToToml(root);
        if (toml::table* saved = root["patches"].as_table()) // compared features: saved as on (review H2)
            for (const std::string& n : g_compareOff)
                if (toml::table* pt = (*saved)[n].as_table()) pt->insert_or_assign("enabled", true);

        std::string err;
        bool ok;
        {
            std::lock_guard<std::mutex> lock(g_fileLock);
            ok = ApexPaths::EnsureApexDirectory() && ApexUtil::WriteFileAtomic(ApexPaths::ConfigFile(), Serialize(root), &err);
        }
        if (!ok) {
            LOG_ERROR("[Config] Save failed: " + err);
            if (error) *error = err;
            return false;
        }
        PatchManager::Get().SetUnsavedChanges(false);
        g_saveRequested = false;
        LOG_DEBUG("[Config] ApexRadiance.toml saved");
        return true;
    } catch (const std::exception& e) {
        LOG_ERROR(std::string("[Config] Save failed: ") + e.what());
        if (error) *error = e.what();
        return false;
    }
}

void RequestSave() {
    g_saveRequestedAt.store(std::chrono::steady_clock::now());
    g_saveRequested = true;
}

void PumpAutosave() {
    const auto now = std::chrono::steady_clock::now();
    const bool requested = g_saveRequested.load() && now - g_saveRequestedAt.load() >= kSaveDelay;
    const bool featureChange = PatchManager::Get().HasUnsavedChanges() && now - PatchManager::Get().LastChange() >= kSaveDelay;
    if (requested || featureChange) Save();
}

bool SavePending() { return g_saveRequested.load() || PatchManager::Get().HasUnsavedChanges(); }

// ---- feature state (looks, profiles, undo) ----

namespace {

// The features a profile carries (their looks and options); developer tools stay out of profiles
// The profile part a feature belongs to (0 = not part of profiles)
unsigned FeaturePart(const std::string& name) {
    if (name == "NightTerrainRelight" || name == "SplitLevelGroundLight") return kPartNightLights;
    if (name == "AmbientOcclusion") return kPartAmbientOcclusion;
    if (name == "SceneDither") return kPartColor; // the Color page's Banding Fix
    if (name == "DepthBlur") return kPartDepthBlur;
    if (name == "EdgeSmoothing") return kPartEdgeSmoothing;
    for (const char* p : {Performance::kResourceCacheName, Performance::kLookupMissesName, Performance::kFileListName, Performance::kLotLightingName,
                          Performance::kWallShadingName, Performance::kFastTextureName, Performance::kFastCacheName, Performance::kSceneBudgetName,
                          Performance::kObjectIndexName})
        if (name == p) return kPartPerformance;
    return 0;
}

bool IsProfileFeature(const std::string& name) { return FeaturePart(name) != 0; }

} // namespace

void SetCompareOverride(const std::vector<std::string>& patches, bool picture) {
    g_compareOff = patches;
    g_comparePictureOff = picture;
}

void CaptureFeatureState(toml::table& out, bool profileFeaturesOnly) {
    out = toml::table{};
    toml::table patches;
    for (const auto& p : PatchManager::Get().GetPatches()) {
        if (profileFeaturesOnly && !IsProfileFeature(p->GetName())) continue;
        toml::table t;
        p->SaveToToml(t);
        if (std::find(g_compareOff.begin(), g_compareOff.end(), p->GetName()) != g_compareOff.end()) t.insert_or_assign("enabled", true);
        patches.insert_or_assign(p->GetName(), std::move(t));
    }
    out.insert_or_assign("patches", std::move(patches));
    toml::table qol;
    Picture::Get().SaveToToml(qol);
    out.insert_or_assign("qol", std::move(qol));
    if (!profileFeaturesOnly || !kPublicBuild || GetUi().developerMode) {
        toml::table d;
        if (kPublicBuild) {
            std::lock_guard<std::mutex> lock(g_uiLock);
            d = g_importedDeveloper;
        }
        d.insert_or_assign("enabled", GetUi().developerMode);
        if (!kPublicBuild) {
        d.insert_or_assign("controls", DeveloperSettings::Capture());
        toml::table diagnosticPatches;
        for (const auto& p : PatchManager::Get().GetPatches()) {
            if (p->GetName() == "FrameCapture") continue;
            toml::table preferences; p->SaveToToml(preferences); preferences.erase("enabled");
            diagnosticPatches.insert(p->GetName(), std::move(preferences));
        }
        d.insert("patches", std::move(diagnosticPatches));
        toml::table profiler; FrameProfiler::SaveToToml(profiler);
        if (auto* p = profiler["frame_profiler"].as_table()) {
            p->insert_or_assign("enabled", false); // importing never starts a measurement
            d.insert("frame_profiler", *p);
        }
        }
        out.insert_or_assign("developer", std::move(d));
    }
}

void DefaultFeatureState(toml::table& out) {
    out = toml::table{};
    toml::table patches;
    for (const auto& p : PatchManager::Get().GetPatches()) {
        toml::table t;
        p->DefaultsToToml(t);
        t.insert_or_assign("enabled", p->IsEnabledByDefault());
        patches.insert_or_assign(p->GetName(), std::move(t));
    }
    out.insert_or_assign("patches", std::move(patches));
    toml::table qol;
    Picture::ParamsToToml(PictureParams{}, qol);
    out.insert_or_assign("qol", std::move(qol));
}

void ApplyFeatureState(const toml::table& state) {
    if (const auto* d = state["developer"].as_table()) {
        UiSettings u = GetUi();
        const bool wanted = (*d)["enabled"].value_or(true);
        // Activation must already have been confirmed by the UI. Undo/deactivation remains allowed.
        if (!wanted || u.developerMode) {
            u.developerMode = wanted;
            SetUi(u);
            if (!kPublicBuild && wanted) {
                ApplyDeveloperPreferences(*d);
                if (const auto* p = (*d)["frame_profiler"].as_table()) {
                    toml::table q; q.insert("frame_profiler", *p);
                    q["frame_profiler"].as_table()->insert_or_assign("enabled", false);
                    FrameProfiler::LoadFromToml(q);
                }
            }
            { std::lock_guard<std::mutex> lock(g_uiLock);
                g_importedDeveloper = *d;
                if (!kPublicBuild) g_importedDeveloper.erase("patches");
            }
            RequestSave();
        }
    }
    if (const toml::table* sc = state["shortcuts"].as_table()) { // a profile's shortcuts (only when that part was picked)
        UiSettings u = GetUi();
        KeyChord k;
        if (ParseKeyChord((*sc)["menu_key"].value_or(std::string()), k)) u.toggle = k;
        const std::string preset = (*sc)["preset"].value_or(std::string());
        u.hotkeyPreset = preset == "letters" ? 0 : preset == "numbers" ? 1 : preset == "fkeys" ? 2 : preset == "mine" ? 3 : -1;
        u.minePresetBase = static_cast<int>((*sc)["mine_base"].value_or(static_cast<int64_t>(u.minePresetBase)));
        u.compareKey = ParseKeyChord((*sc)["compare_key"].value_or(std::string()), k) ? k : KeyChord{0, true, true, false};
        u.refreshKey = ParseKeyChord((*sc)["refresh_key"].value_or(std::string()), k) ? k : KeyChord{0, true, true, false};
        u.probeKey = ParseKeyChord((*sc)["probe_key"].value_or(std::string()), k) ? k : KeyChord{0, true, true, false};
        u.diagnosticsKey = ParseKeyChord((*sc)["diagnostics_key"].value_or(std::string()), k) ? k : KeyChord{0, true, true, false};
        u.recorderKey = ParseKeyChord((*sc)["recorder_key"].value_or(std::string()), k) ? k : KeyChord{0, true, true, false};
        u.frameCaptureKey = ParseKeyChord((*sc)["frame_capture_key"].value_or(std::string()), k) ? k : KeyChord{0, true, true, false};
        if (ParseKeyChord((*sc)["search_key"].value_or(std::string()), k)) u.searchKey = k;
        if (ParseKeyChord((*sc)["peek_key"].value_or(std::string()), k)) u.peekKey = k;
        if (ParseKeyChord((*sc)["picture_compare_key"].value_or(std::string()), k)) u.pictureCompareKey = k;
        if (ParseKeyChord((*sc)["screenshot_key"].value_or(std::string()), k)) u.screenshotKey = AcceptScreenshotKey(k);
        u.screenshotHideGameUi = (*sc)["screenshot_hide_game_ui"].value_or(u.screenshotHideGameUi);
        if (auto f = (*sc)["screenshot_folder"].value<std::string>()) u.screenshotToApexFolder = *f == "apex";
        u.keyChosen = true;
        SetUi(u);
        LOG_INFO("[Config] Shortcuts taken from the profile: menu key " + KeyChordText(u.toggle));
    }
    bool any = false;
    if (const toml::table* patches = state["patches"].as_table()) {
        for (const auto& p : PatchManager::Get().GetPatches()) {
            const toml::table* t = (*patches)[p->GetName()].as_table();
            if (!t) continue;
            // Only the keys the table has are compared (and applied)
            toml::table current, currentKeys;
            p->SaveToToml(current);
            for (auto&& [key, value] : *t)
                if (const toml::node* c = current.get(key.str())) currentKeys.insert_or_assign(std::string(key.str()), *c);
            if (currentKeys == *t) continue;
            try {
                p->ApplyTableLive(*t);
            } catch (const std::exception& e) {
                LOG_ERROR(std::format("[Config] Applying {} failed: {}", p->GetName(), e.what()));
            }
            any = true;
        }
    }
    if (const toml::table* qol = state["qol"].as_table()) {
        PictureParams target;
        if (Picture::ParamsFromToml(*qol, target)) {
            const PictureParams current = Picture::Get().GetParams();
            toml::table a, b;
            Picture::ParamsToToml(current, a);
            Picture::ParamsToToml(target, b);
            if (a != b) {
                target.compare = current.compare;
                Picture::Get().SetParams(target, true);
                any = true;
            }
        }
    }
    if (any) {
        PatchManager::Get().SetUnsavedChanges(true);
        RequestSave();
    }
}

// ---- profiles ----

const char* ProfilePartName(int index) {
    static const char* const kNames[kProfilePartCount] = {"Lighting", "Color", "Depth Blur", "Edge Smoothing", "", "Performance", "Shortcuts", "Ambient Occlusion", "Developer"};
    return index >= 0 && index < kProfilePartCount ? kNames[index] : "";
}

unsigned ProfilePartsOf(const toml::table& state) {
    unsigned parts = 0;
    if (const toml::table* patches = state["patches"].as_table())
        for (auto&& [key, value] : *patches) parts |= FeaturePart(std::string(key.str()));
    if (const toml::table* qol = state["qol"].as_table(); qol && !qol->empty()) parts |= kPartColor;
    if (state["shortcuts"].as_table()) parts |= kPartShortcuts;
    if (state["developer"].as_table()) parts |= kPartDeveloper;
    return parts;
}

void KeepProfileParts(toml::table& state, unsigned parts) {
    if (toml::table* patches = state["patches"].as_table()) {
        std::vector<std::string> drop;
        for (auto&& [key, value] : *patches)
            if (!(FeaturePart(std::string(key.str())) & parts)) drop.emplace_back(key.str());
        for (const std::string& k : drop) patches->erase(k);
    }
    if (!(parts & kPartColor)) state.erase("qol");
    state.erase("display"); // obsolete controller table is never applied/exported
    if (!(parts & kPartShortcuts)) state.erase("shortcuts");
    if (!(parts & kPartDeveloper)) state.erase("developer");
}

namespace {

std::wstring ProfilesDirectory() { return ApexPaths::ApexDirectory() + L"Profiles\\"; }

std::wstring ProfileFile(const std::string& name) { return ProfilesDirectory() + ApexUtil::ToWide(name) + L".toml"; }

bool IsReservedName(const std::string& name) {
    static const char* const kReserved[] = {"CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
                                            "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};
    const std::string upper = Upper(name);
    for (const char* r : kReserved)
        if (upper == r) return true;
    return false;
}

} // namespace

std::string SanitizeProfileName(const std::string& raw) {
    std::string out;
    for (const char c : raw) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == ' ' || c == '-' || c == '_';
        if (!ok) continue;
        if (c == ' ' && (out.empty() || out.back() == ' ')) continue; // no leading or double spaces
        out.push_back(c);
    }
    while (!out.empty() && out.back() == ' ') out.pop_back();
    if (out.size() > static_cast<size_t>(kProfileNameMax)) out.resize(static_cast<size_t>(kProfileNameMax));
    while (!out.empty() && out.back() == ' ') out.pop_back();
    if (IsReservedName(out)) return {};
    return out;
}

std::vector<std::string> ListProfiles() {
    std::vector<std::string> names;
    WIN32_FIND_DATAW fd{};
    const HANDLE h = FindFirstFileW((ProfilesDirectory() + L"*.toml").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return names;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring file = fd.cFileName;
        if (file.size() <= 5) continue;
        const std::string name = ApexUtil::ToUtf8(file.substr(0, file.size() - 5));
        if (SanitizeProfileName(name) == name) names.push_back(name); // only names the menu can load back
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(names.begin(), names.end(), [](const std::string& a, const std::string& b) { return Upper(a) < Upper(b); });
    return names;
}

bool ProfileExists(const std::string& name) {
    const std::string clean = SanitizeProfileName(name);
    return !clean.empty() && ApexUtil::FileExists(ProfileFile(clean));
}

bool SaveProfile(const std::string& name, unsigned parts, std::string* error, const std::string& icon) {
    const std::string clean = SanitizeProfileName(name);
    if (clean.empty() || clean != name) {
        if (error) *error = "invalid name";
        return false;
    }
    try {
        toml::table root;
        CaptureFeatureState(root, true);
        { // the shortcuts (the menu's [ui] keys), removed below unless picked
            const UiSettings u = GetUi();
            toml::table sc;
            sc.insert("menu_key", KeyChordText(u.toggle));
            static constexpr const char* kPresetKeys[] = {"letters", "numbers", "fkeys", "mine"};
            if (u.hotkeyPreset >= 0 && u.hotkeyPreset < 4) sc.insert("preset", kPresetKeys[u.hotkeyPreset]);
            if (u.hotkeyPreset == 3) sc.insert("mine_base", static_cast<int64_t>(u.minePresetBase));
            if (u.compareKey.vk) sc.insert("compare_key", KeyChordText(u.compareKey));
            if (u.refreshKey.vk) sc.insert("refresh_key", KeyChordText(u.refreshKey));
            if (u.probeKey.vk) sc.insert("probe_key", KeyChordText(u.probeKey));
            if (u.diagnosticsKey.vk) sc.insert("diagnostics_key", KeyChordText(u.diagnosticsKey));
            if (u.recorderKey.vk) sc.insert("recorder_key", KeyChordText(u.recorderKey));
            if (u.frameCaptureKey.vk) sc.insert("frame_capture_key", KeyChordText(u.frameCaptureKey));
            sc.insert("search_key", KeyChordText(u.searchKey));
            sc.insert("peek_key", KeyChordText(u.peekKey));
            sc.insert("picture_compare_key", KeyChordText(u.pictureCompareKey));
            sc.insert("screenshot_shortcut_enabled", u.screenshotShortcutEnabled);
            sc.insert("screenshot_key", KeyChordText(u.screenshotKey));
            sc.insert("screenshot_hide_game_ui", u.screenshotHideGameUi);
            sc.insert("screenshot_folder", std::string(u.screenshotToApexFolder ? "apex" : "game"));
            root.insert_or_assign("shortcuts", std::move(sc));
        }
        KeepProfileParts(root, parts);
        toml::table meta;
        meta.insert("written_by", APEX_PRODUCT_NAME " " APEX_VERSION_STRING);
        meta.insert("profile", clean);
        meta.insert("icon", icon);
        root.insert_or_assign("meta", std::move(meta));
        if (!ApexPaths::EnsureApexDirectory()) {
            if (error) *error = "the settings folder could not be created";
            return false;
        }
        const std::wstring dir = ProfilesDirectory();
        if (!CreateDirectoryW(dir.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) {
            if (error) *error = "the Profiles folder could not be created";
            return false;
        }
        std::string err;
        if (!ApexUtil::WriteFileAtomic(ProfileFile(clean), Serialize(root), &err)) {
            LOG_ERROR("[Config] Profile " + clean + " could not be saved: " + err);
            if (error) *error = err.empty() ? "the file could not be written" : err;
            return false;
        }
        LOG_INFO("[Config] Profile saved: " + clean);
        return true;
    } catch (const std::exception& e) {
        if (error) *error = e.what();
        return false;
    }
}

bool ReadProfile(const std::string& name, toml::table& out, std::string* error) {
    const std::string clean = SanitizeProfileName(name);
    if (clean.empty()) {
        if (error) *error = "invalid name";
        return false;
    }
    std::string parseError;
    if (!ParseFile(ProfileFile(clean), out, &parseError)) {
        if (error) *error = parseError.empty() ? "the file could not be read" : parseError;
        LOG_ERROR("[Config] Profile " + clean + " could not be read" + (parseError.empty() ? std::string() : ": " + parseError));
        return false;
    }
    return true;
}

std::wstring ProfilesFolder() { return ProfilesDirectory(); }

bool EnsureProfilesDirectory() {
    if (!ApexPaths::EnsureApexDirectory()) return false;
    return CreateDirectoryW(ProfilesDirectory().c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

bool DeleteProfile(const std::string& name, std::string* error) {
    const std::string clean = SanitizeProfileName(name);
    if (clean.empty() || !DeleteFileW(ProfileFile(clean).c_str())) {
        if (error) *error = "the file could not be deleted";
        return false;
    }
    LOG_INFO("[Config] Profile deleted: " + clean);
    return true;
}

} // namespace ApexConfig
