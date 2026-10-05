#pragma once
// ApexRadiance.toml: Apex Radiance's own configuration, in Documents\...\Apex Radiance\ (never S3SS.toml).
//   [meta]               version, the build that wrote it, the one-time migration from S3SS.toml
//   [ui]                 toggle_key, font_scale, shortcuts and screenshot capture settings, legacy welcome_done/key_chosen, start_profile_done, sidebar_collapsed
//   [qol.picture]        Picture filters (same keys as the combined build)
//   [qol.frame_profiler] Frame Profiler (development build)
//   [patches.<Name>]     one table per feature: enabled + its settings (same keys as before the split)
// Writes are atomic (temporary file + replace) and debounced: RequestSave marks the file dirty, the pump thread writes it
// a second later.
#include <windows.h>
#include <string>
#include <vector>

namespace toml {
inline namespace v3 {
class table;
}
} // namespace toml

namespace ApexConfig {

struct KeyChord {
    UINT vk = VK_F11;
    bool ctrl = true;
    bool shift = true;
    bool alt = false;
};

struct UiSettings {
    KeyChord toggle;       // opens / closes the Apex menu (default Ctrl+Shift+F11; S3SS uses a bare Insert)
    bool developerMode = false; // applies next game start; enabling requires UI confirmation
    float fontScale = 1.0f;
    bool recommendS3SS = true; // the "Recommended: Sims3SettingsSetter" card while S3SS is not loaded ([ui] recommend_s3ss)
    bool startNote = true;     // the "Apex Radiance is ready, press <key>" note at every start ([ui] start_note)
    bool captureScreenshot = true; // the Report a problem captures also save Screenshot.png ([ui] capture_screenshot)
    bool welcomeDone = false;  // legacy welcome-tour flag, kept when reading and writing older configs
    bool keyChosen = false;    // legacy first-start key prompt flag; no longer gates the menu or startup hint
    bool startProfileDone = false; // the welcome page was shown ([ui] start_profile_done; false only for a new installation: a config without it counts as true)
    int hotkeyPreset = -1;     // Hotkeys::Preset of the other shortcuts ([ui] hotkey_preset = "letters" / "numbers" / "fkeys"; missing = -1: the F keys)
    KeyChord compareKey{0, true, true, false}; // the player's own key for Compare ([ui] compare_key; vk 0 = the preset's)
    KeyChord refreshKey{0, true, true, false}; // the player's own key for Refresh ([ui] refresh_key; vk 0 = the preset's)
    KeyChord probeKey{0, true, true, false}; // optional override for Light Probe ([ui] probe_key; vk 0 = the preset's)
    KeyChord diagnosticsKey{0, true, true, false}; // optional override for Light Diag ([ui] diagnostics_key; vk 0 = the preset's)
    KeyChord recorderKey{0, true, true, false}; // optional override for Recording ([ui] recorder_key; vk 0 = the preset's)
    KeyChord frameCaptureKey{0, true, true, false}; // optional override for Frame Capture ([ui] frame_capture_key; vk 0 = the preset's)
    KeyChord searchKey{'F', true, false, false}; // focuses settings search while the menu is open ([ui] search_key)
    KeyChord peekKey{VK_MENU, false, false, false}; // hold to peek through the menu ([ui] peek_key)
    KeyChord pictureCompareKey{'B', false, false, false}; // hold to bypass Picture while over the menu ([ui] picture_compare_key)
    bool screenshotShortcutEnabled = true; // intercept a configurable key for filtered screenshots ([ui] screenshot_shortcut_enabled)
    KeyChord screenshotKey{VK_F8, false, false, false}; // F8 alone: an extra filtered screenshot (Ctrl+Shift+F8 is the lighting snapshot); the game's own C stays untouched
    bool screenshotToApexFolder = false; // [ui] screenshot_folder: "game" (Documents\...\The Sims 3\Screenshots) or "apex" (Apex Radiance\Screenshots)
    bool screenshotHideGameUi = true; // temporarily toggle the game's F10 UI visibility only while taking the screenshot
    int minePresetBase = 0; // the preset the "mine" keys started from (its keys for the tools) ([ui] mine_base)
    bool sidebarCollapsed = false; // the sidebar is the icon-only rail ([ui] sidebar_collapsed)
    int language = -1;             // menu language: -1 = Windows' display language, else I18n::Lang ([ui] language = "auto" / "en" / "pt" / "es" / "fr")
};

std::string KeyChordText(const KeyChord& chord); // "Ctrl+Shift+F11"
bool ParseKeyChord(const std::string& text, KeyChord& out);
std::string KeyName(UINT vk);

UiSettings GetUi();
void SetUi(const UiSettings& ui); // and saves (debounced)

// One-time migration (call_once), only while ApexRadiance.toml does not exist:
//  1. the previous standalone build's S3SS\Apex\Apex.toml (same schema) is copied as it is; its apex_imgui.ini is
//     not (the menu's scale changed: the new default window size applies); the old folder stays;
//  2. else the combined build's S3SS.toml: backed up to S3SS.toml.pre-split.bak (in the Apex Radiance folder), then
//     only the settings the features of this build read are carried over (needs PatchManager::CreateAll() first);
//  3. else defaults.
void EnsureMigrated();
std::string MigrationNote(); // what the migration did, for the log and the Settings page (Status > Settings)

// [ui], [qol.*] (no feature is installed here)
void LoadDeveloperMode(); // before feature construction; chooses startup-only runtime gates
void LoadSettings();
// [patches.*]: installs the enabled features, then the ones on by default that the file does not mention
void LoadFeatures();

// Parses ApexRadiance.toml; false when it is missing or unreadable (out is left empty).
bool ReadRoot(toml::table& out);
bool Save(std::string* error = nullptr);
void RequestSave();
void PumpAutosave(); // pump thread
// A save is waiting (a requested save or unsaved feature changes): the menu's status bar says "Saving…"
bool SavePending();

// ---- feature state: looks, profiles and undo (menu, render thread) ----
// The tables ApexRadiance.toml keeps for the features: [patches.<Name>] (every feature, or only the ones a profile
// carries: Night Lights, Every-Story Ground Light, Edge Smoothing, Depth Blur), [qol.picture]. Legacy [display] values are ignored.
// While the Compare shortcut has these features off (apex_gui ToggleCompare), saves and profiles store them as on: the
// comparison is never saved (review 30/09, H2). Empty = none.
void SetCompareOverride(const std::vector<std::string>& patches, bool picture);
void CaptureFeatureState(toml::table& out, bool profileFeaturesOnly = false);
// Applies such a table live, like changes in the menu: only the sections that differ from the current state (feature
// settings and on / off through ApexPatch::ApplyTableLive, Picture through SetParams); sections the table does not have stay as they are. Marks unsaved changes and requests a save.
void ApplyFeatureState(const toml::table& state);
// Every feature at its defaults, as a CaptureFeatureState table: each setting's default and its default on / off, and
// Color at its defaults. The menu's own preferences (language, key, text size) are not included.
void DefaultFeatureState(toml::table& out);

// ---- profiles: Documents\...\Apex Radiance\Profiles\<name>.toml ----
// Each file is CaptureFeatureState(profile features), limited to the parts chosen when it was saved, plus [meta] (the
// build that wrote it). Loading applies the parts the user picks among those the file has (KeepProfileParts). Names: letters, digits,
// space, - and _ only, at most 32 characters (SanitizeProfileName; empty = not usable).
inline constexpr int kProfileNameMax = 32;
std::string SanitizeProfileName(const std::string& raw);
std::vector<std::string> ListProfiles(); // sorted by name (case-insensitive)
bool ProfileExists(const std::string& name);
// The parts of a profile, as bit flags (index i = bit 1 << i)
enum ProfilePart : unsigned {
    kPartNightLights = 1u << 0,   // Night Lights and Every-Story Ground Light (with Water & Snow)
    kPartColor = 1u << 1,         // Color ([qol] picture)
    kPartDepthBlur = 1u << 2,     // Depth Blur
    kPartEdgeSmoothing = 1u << 3, // Edge Smoothing
    // Bit 4 is reserved for retired window profiles; other category bits do not shift.
    kPartPerformance = 1u << 5,   // the Performance page's features
    kPartShortcuts = 1u << 6,     // keyboard and screenshot shortcut settings; not saved by default
    kPartDeveloper = 1u << 8, // advanced settings, opt-in and hidden in normal mode
    kPartAmbientOcclusion = 1u << 7, // Ambient Occlusion (after Shortcuts: older saved part masks keep their bits)
};
inline constexpr int kProfilePartCount = 9;
inline constexpr unsigned kProfilePartsAll = ((1u << kProfilePartCount) - 1) & ~(1u << 4);
const char* ProfilePartName(int index); // English, for the menu ("Night Lights")
unsigned ProfilePartsOf(const toml::table& state);         // the parts a profile table has
void KeepProfileParts(toml::table& state, unsigned parts); // removes the other parts from a profile table
bool SaveProfile(const std::string& name, unsigned parts = kProfilePartsAll, std::string* error = nullptr, const std::string& icon = "bookmark");
// Parses the profile (does not apply it: see ApplyFeatureState)
bool ReadProfile(const std::string& name, toml::table& out, std::string* error = nullptr);
bool DeleteProfile(const std::string& name, std::string* error = nullptr);
std::wstring ProfilesFolder();  // the Profiles folder inside the Apex Radiance folder (trailing backslash)
bool EnsureProfilesDirectory(); // creates it if needed

} // namespace ApexConfig
