// Screenshots and videos with Apex's effects, and the player's settings file ApexCapture.ini (see apex_capture.h).
#include "apex_capture.h"
#include "captures.h"
#include "video_mp4.h"
#include "patch_base.h"
#include "apex_config.h"
#include "apex_log.h"
#include "apex_paths.h"
#include "apex_util.h"
#include "memory_patch.h"
#include "d3d9_hooks.h"
#include "render_callbacks.h"
#include "overlay.h"
#include "imgui.h"
#include "ui/widgets.h"
#include "ui/i18n.h"
#include "ui/violet_theme.h"
#include "hook_guard.h"
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <mmsystem.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <optional>
#include <sstream>

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "shell32.lib")

// gshade_looks.cpp: the preset in use (false while the look is off)
bool GShadeLooksPresetInUse(std::wstring& file);

namespace ApexCapture {
using ApexUi::IconId;
namespace {

// ---------------------------------------------------------------------------------------------- the file's keys
enum class Kind { Bool, Int, Text, Pick };
struct Key {
    const char* section;
    const char* name;
    const char* note;
    Kind kind;
    bool Settings::* b = nullptr;
    int Settings::* i = nullptr;
    std::string Settings::* s = nullptr;
    int lo = 0, hi = 0;
    const char* off = nullptr; // Pick: the value when b is false / true
    const char* on = nullptr;
};
Key B(const char* sec, const char* n, bool Settings::* m, const char* note) { Key k{sec, n, note, Kind::Bool}; k.b = m; return k; }
Key I(const char* sec, const char* n, int Settings::* m, int lo, int hi, const char* note) { Key k{sec, n, note, Kind::Int}; k.i = m; k.lo = lo; k.hi = hi; return k; }
Key T(const char* sec, const char* n, std::string Settings::* m, const char* note) { Key k{sec, n, note, Kind::Text}; k.s = m; return k; }
Key P(const char* sec, const char* n, bool Settings::* m, const char* off, const char* on, const char* note) { Key k{sec, n, note, Kind::Pick}; k.b = m; k.off = off; k.on = on; return k; }

const std::vector<Key>& Keys() {
    static const std::vector<Key> k = {
        B("GENERAL", "ReloadOnChange", &Settings::reloadOnChange, "1 = Apex re-reads this file a moment after you save it (no restart needed). The menu tab writes here too."),
        B("GENERAL", "ShowNotice", &Settings::showNotice, "1 = a short note at the top of the screen when a picture or video is saved. Notes are never in the picture or the video."),
        T("GENERAL", "SoundPath", &Settings::soundPath, "Played for every screenshot and at the start and stop of a video: camera (a camera shutter, built in), gshade (GShade's notification.wav when GShade is installed), a .wav file, or empty = silent."),

        T("SCREENSHOT", "KeyScreenshot", &Settings::keyScreenshot, "Screenshot key: F8, Ctrl+F8, Shift+F12 ... GShade's numbers work too (107,0,1,0 = Shift+Num +). A bare letter or digit is refused (it would fire while typing names)."),
        T("SCREENSHOT", "SavePath", &Settings::shotPath, "Folder for screenshots. Tokens: %GameScreenshots% (Documents\\Electronic Arts\\The Sims 3\\Screenshots), %ApexFolder%, %Pictures%, %Videos%, %Documents%, and the date, time and save tokens of FileNaming (for example F:\\Pictures\\TS3W\\%Day%-%Month%-%Year%). Made when missing."),
        T("SCREENSHOT", "FileNaming", &Settings::shotNaming, "File name. Tokens as in GShade: %Date% %TimeHour% %TimeMinute% %TimeSecond% %TimeMS% %PresetName%, plus %Day% %Month% %Year% %YearShort% %MonthName% %MonthShort% %Weekday% %WeekdayShort% %Hour12% %AmPm%, %SaveName% (the save in play), %Type% (Screenshot/Video), %Part% (the clip number with NewClipEveryMB), %Width% %Height%, %Counter% (a running number) and %Look% (on/off). They work in SavePath too."),
        I("SCREENSHOT", "CounterDigits", &Settings::counterDigits, 1, 8, "How many digits %Counter% has (4 = 0001). The counter carries on across game sessions."),
        P("SCREENSHOT", "FileFormat", &Settings::jpg, "png", "jpg", "png = lossless (about 6-9 MB at 2560x1440), jpg = small. GShade wrote a number here; Apex takes the name. (png | jpg)"),
        I("SCREENSHOT", "JPEGQuality", &Settings::jpegQuality, 1, 100, "JPEG quality, 1-100. Only for FileFormat=jpg."),
        I("SCREENSHOT", "ResolutionScale", &Settings::shotScale, 10, 100, "Picture size in % of the screen. 100 = the game's own size; 50 = half."),
        B("SCREENSHOT", "IncludeGameUI", &Settings::shotGameUi, "1 = the game's UI is in the picture. 0 = Apex hides it for one frame (F10) and brings it back. Loading and menu screens always keep theirs."),
        B("SCREENSHOT", "IncludeApexMenu", &Settings::shotApexMenu, "1 = the Apex menu is in the picture when it is open. 0 = never."),
        B("SCREENSHOT", "SaveBeforeShot", &Settings::saveBefore, "1 = also save the same frame without the GShade look, as <name>_before (as GShade does)."),
        B("SCREENSHOT", "SavePresetFile", &Settings::savePreset, "1 = also save a copy of the preset .ini next to the picture, so the look can be found again (as GShade does)."),
        B("SCREENSHOT", "IncludeMouse", &Settings::shotMouse, "1 = the mouse pointer is in the picture, where it was on screen. 0 = never."),
        I("SCREENSHOT", "BatchQuantity", &Settings::batchQuantity, 1, 50, "Pictures per key press. 1 = one; more = a burst (GShade's Batch)."),
        I("SCREENSHOT", "BatchInterval", &Settings::batchInterval, 50, 10000, "Milliseconds between the pictures of a burst."),

        B("VIDEO", "Enabled", &Settings::videoEnabled, "1 = the record key works. 0 = it does nothing."),
        P("VIDEO", "Engine", &Settings::external, "builtin", "external", "builtin = Apex writes the MP4 itself. external = Apex presses ExternalKey for Xbox Game Bar, ShadowPlay or OBS; they save in their own folder. (builtin | external)"),
        T("VIDEO", "KeyRecord", &Settings::keyRecord, "Start / stop key: press once to start, again to stop."),
        T("VIDEO", "SavePath", &Settings::videoPath, "Folder for videos. Same tokens as screenshots."),
        T("VIDEO", "FileNaming", &Settings::videoNaming, "File name. Same tokens as screenshots."),
        P("VIDEO", "Container", &Settings::fragmented, "mp4", "mp4-safe", "mp4 = plays everywhere. mp4-safe = fragmented MP4: a crash mid-recording keeps what was recorded. mkv = recorded as mp4-safe, then copied into an MKV when it stops (no quality loss). (mp4 | mp4-safe | mkv)"),
        B("VIDEO", "KeepMp4", &Settings::keepMp4, "With Container=mkv: 1 = keep the MP4 next to the MKV. 0 = only the MKV."),
        I("VIDEO", "NewClipEveryMB", &Settings::newClipMB, 0, 100000, "While recording, save the clip and go on in a new file at this size in MB (1024 = 1 GB). 0 = one file. %Part% numbers the clips."),
        P("VIDEO", "Codec", &Settings::hevc, "h264", "hevc", "hevc = smaller files, only with a graphics card that encodes HEVC; else Apex falls back to h264. (h264 | hevc)"),
        I("VIDEO", "FPS", &Settings::fps, 10, 60, "Frames per second (24, 30 or 60). 60 costs twice the readback of 30."),
        P("VIDEO", "RateControl", &Settings::bitrateMode, "quality", "bitrate", "quality = Quality below sets the size. bitrate = a fixed BitrateMbps. (quality | bitrate)"),
        I("VIDEO", "Quality", &Settings::quality, 1, 100, "Quality 1-100 (RateControl=quality). 80 = about 16 Mbit/s at 1080p30."),
        I("VIDEO", "BitrateMbps", &Settings::bitrateMbps, 1, 200, "Mbit/s (RateControl=bitrate)."),
        I("VIDEO", "MaxHeight", &Settings::maxHeight, 360, 2160, "Video height in lines (720, 1080, 1440). Scaled on the graphics card before the readback; the game's own height is the most it takes. Higher costs more bridge memory."),
        P("VIDEO", "Audio", &Settings::audio, "off", "game", "game = The Sims 3's own sound only (Windows 10 2004 or later), not Discord or music players. off = silent. (game | off)"),
        B("VIDEO", "IncludeGameUI", &Settings::videoGameUi, "1 = the game's UI is in the video. 0 = hidden (F10) while recording."),
        B("VIDEO", "IncludeApexMenu", &Settings::videoApexMenu, "1 = the Apex menu (and its notes) is in the video when open. 0 = never."),
        B("VIDEO", "IncludeMouse", &Settings::videoMouse, "1 = the mouse pointer moves in the video as on screen. 0 = never."),
        I("VIDEO", "MaxLength", &Settings::maxLength, 0, 36000, "Stop after this many seconds (0 = never)."),
        I("VIDEO", "MaxFileMB", &Settings::maxFileMB, 0, 100000, "Stop at this file size in MB (0 = never)."),
        B("VIDEO", "ShowRecordingDot", &Settings::recordingDot, "1 = a recording timer on screen while a video records: a red dot, the time and the file size."),
        T("VIDEO", "TimerPosition", &Settings::timerPos, "Where the timer sits: top-left | top-center | top-right | bottom-left | bottom-right."),
        I("VIDEO", "TimerSize", &Settings::timerSize, 70, 160, "Timer size in % (100 = the size of Apex notes)."),
        I("VIDEO", "TimerOpacity", &Settings::timerOpacity, 30, 100, "Timer opacity in %."),
        B("VIDEO", "TimerBlink", &Settings::timerBlink, "1 = the red dot pulses."),
        B("VIDEO", "TimerHours", &Settings::timerHours, "1 = always show hours (0:02:15). 0 = hours only from one hour on."),
        B("VIDEO", "TimerStopKey", &Settings::timerKey, "1 = the timer shows the key that stops the video."),
        B("VIDEO", "TimerFileSize", &Settings::timerFileSize, "1 = the timer shows the file size so far (built-in recorder only)."),
        B("VIDEO", "TimerInVideo", &Settings::timerInVideo, "1 = the timer and the red frame are in the video too (the video is then taken after Apex draws, so an open Apex menu and its notes are in it as well). 0 = on screen only."),
        B("VIDEO", "RedFrame", &Settings::redFrame, "1 = a thin red frame around the screen while recording."),
        I("VIDEO", "WarnBefore", &Settings::warnBefore, 0, 600, "Seconds before MaxLength or MaxFileMB stops the video: the timer turns amber and shows the time left. 0 = no warning."),
        B("VIDEO", "SavedNote", &Settings::savedNote, "1 = a short note when the video is saved: its length and size."),
        T("VIDEO", "ExternalKey", &Settings::externalKey, "Engine=external only: the recorder's start/stop key. Win+Alt+R = Xbox Game Bar."),

        T("PROFILES", "Active", &Settings::profileActive, "The capture profile in use. Every value in this file is saved into it (Capture profiles\\<name>.ini next to this file), except the keys and this section, which every profile shares. Type another profile's name here to switch."),
        T("PROFILES", "KeyNext", &Settings::keyNextProfile, "Switches to the next capture profile (in name order). Empty = no key."),
        B("PROFILES", "ShowNote", &Settings::profileNote, "1 = a short note with the profile's name when it switches. Never in pictures or videos."),
        T("PROFILE", "Key", &Settings::profileKey, "This profile's own key: switches to it (Ctrl+1, Ctrl+2 ...). Empty = none."),
        T("PROFILE", "ShotKey", &Settings::profileShotKey, "Takes one picture (or burst) with this profile without switching to it, for example a clean shot while recording with the UI. Empty = none."),
    };
    return k;
}

std::string Lower(std::string s) { for (auto& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c))); return s; }
std::string Trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

std::string ValueOf(const Settings& st, const Key& k) {
    switch (k.kind) {
    case Kind::Bool: return st.*k.b ? "1" : "0";
    case Kind::Int: return std::to_string(st.*k.i);
    case Kind::Text: return st.*k.s;
    case Kind::Pick: return k.b == &Settings::fragmented && st.mkv ? "mkv" : st.*k.b ? k.on : k.off;
    }
    return {};
}

// false = the value was not understood (the default stays)
bool Assign(Settings& st, const Key& k, const std::string& raw) {
    const std::string v = Lower(Trim(raw));
    switch (k.kind) {
    case Kind::Bool:
        if (v == "1" || v == "true" || v == "on" || v == "yes") { st.*k.b = true; return true; }
        if (v == "0" || v == "false" || v == "off" || v == "no") { st.*k.b = false; return true; }
        return false;
    case Kind::Int: {
        char* end = nullptr;
        const long n = strtol(v.c_str(), &end, 10);
        if (v.empty() || (end && *end)) return false;
        st.*k.i = std::clamp(static_cast<int>(n), k.lo, k.hi);
        return true;
    }
    case Kind::Text: st.*k.s = Trim(raw); return true;
    case Kind::Pick:
        if (k.b == &Settings::fragmented) st.mkv = v == "mkv";
        if (v == k.on || (k.b == &Settings::fragmented && v == "mkv")) { st.*k.b = true; return true; }
        if (v == k.off) { st.*k.b = false; return true; }
        if (k.b == &Settings::jpg && (v == "2" || v == "jpeg")) { st.*k.b = true; return true; }   // GShade: 0 bmp, 1 png, 2 jpg
        if (k.b == &Settings::jpg && (v == "0" || v == "1" || v == "bmp")) { st.*k.b = false; return true; }
        return false;
    }
    return false;
}

// ---------------------------------------------------------------------------------------------- state
std::mutex g_lock;
Settings g_s;
bool g_loaded = false;
std::vector<std::tuple<std::string, std::string, std::string>> g_unknown; // section, key, value: kept when writing
FILETIME g_seen{};
unsigned long long g_dirtyAt = 0, g_lastCheck = 0;
bool g_dirty = false;
std::string g_lastError;
struct UiSeen { ApexConfig::KeyChord key; bool hide = true; int folder = 0; std::string custom; } g_uiSeen;
bool g_uiSeenSet = false;

FILETIME WriteTime(const std::filesystem::path& p) {
    WIN32_FILE_ATTRIBUTE_DATA d{};
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &d)) return {};
    return d.ftLastWriteTime;
}
bool SameTime(const FILETIME& a, const FILETIME& b) { return a.dwLowDateTime == b.dwLowDateTime && a.dwHighDateTime == b.dwHighDateTime; }

std::string Render(const Settings& st, bool profileFile = false) {
    std::string t = profileFile ?
        "; Apex Radiance capture profile \"" + st.profileActive + "\". Switch to it in Settings > Menu, with KeyNext, or with\n"
        "; [PROFILE] Key below. While it is in use, ApexCapture.ini holds these values and saves them back here; edit it there.\n"
        "; The keys (KeyScreenshot, KeyRecord, [PROFILES]) come from ApexCapture.ini and are shared by every profile.\n" :
        "; ApexCapture.ini - how Apex Radiance saves screenshots and videos.\n"
        "; Edit freely: lines starting with ; are notes. Apex re-reads this file when you save it, and the\n"
        "; Screenshot capture and Video recording cards (Settings > Menu) edit this same file.\n"
        "; A value Apex does not understand keeps its default (noted in ApexRadiance_LOG.txt). Unknown keys are kept.\n"
        "; Delete the file to get every default back.\n";
    std::string sec;
    for (const Key& k : Keys()) {
        if (sec != k.section) {
            if (!sec.empty())
                for (const auto& [s, n, v] : g_unknown) if (s == sec) t += n + "=" + v + "\n";
            sec = k.section;
            t += "\n[" + sec + "]\n";
        }
        t += std::string("; ") + k.note + (k.kind == Kind::Bool ? "  (1 | 0)" : "") + "\n" + k.name + "=" + ValueOf(st, k) + "\n";
    }
    for (const auto& [s, n, v] : g_unknown) if (s == sec) t += n + "=" + v + "\n";
    std::string other;
    for (const auto& [s, n, v] : g_unknown) {
        if (s == "GENERAL" || s == "SCREENSHOT" || s == "VIDEO" || s == "PROFILES" || s == "PROFILE") continue;
        if (other != s) { other = s; t += "\n[" + s + "]\n"; }
        t += n + "=" + v + "\n";
    }
    return t;
}

bool WriteText(const std::filesystem::path& file, const std::string& text);
void WriteProfileLocked();

bool WriteLocked() {
    const auto file = File();
    if (!WriteText(file, Render(g_s))) { LOG_WARNING("[Capture] ApexCapture.ini could not be written"); return false; }
    g_seen = WriteTime(file);
    WriteProfileLocked(); // auto-save into the profile in use
    return true;
}

// One settings file into st (defaults first). false = it cannot be opened.
bool ParseFile(const std::filesystem::path& file, Settings& st, std::vector<std::tuple<std::string, std::string, std::string>>* unknown, std::string& bad) {
    std::ifstream in(file, std::ios::binary);
    if (!in) return false;
    st = Settings{};
    if (unknown) unknown->clear();
    std::string line, sec;
    while (std::getline(in, line)) {
        if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF && static_cast<unsigned char>(line[1]) == 0xBB) line = line.substr(3);
        line = Trim(line);
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line.front() == '[' && line.back() == ']') { sec = Trim(line.substr(1, line.size() - 2)); for (auto& c : sec) c = static_cast<char>(toupper(static_cast<unsigned char>(c))); continue; }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        const std::string name = Trim(line.substr(0, eq)), value = line.substr(eq + 1);
        const Key* key = nullptr;
        for (const Key& k : Keys()) if (sec == k.section && _stricmp(name.c_str(), k.name) == 0) { key = &k; break; }
        if (!key) { if (unknown) unknown->emplace_back(sec, name, Trim(value)); continue; }
        if (!Assign(st, *key, value)) bad += std::string(bad.empty() ? "" : ", ") + key->name + "=" + Trim(value);
    }
    return true;
}

// ---------------------------------------------------------------------------------------------- capture profiles
struct Profile { std::string name; Settings s; };
std::vector<Profile> g_profiles;          // sorted by name; the one in use is g_s (live)
unsigned long long g_profilesScanned = 0;
bool g_hadLoad = false;                  // LoadLocked ran once (a later read may switch by Active=)
std::string g_wantProfile, g_wantShot;    // set by the key hook, done in Tick (render thread)
bool g_wantNext = false;
std::atomic<DWORD> g_overrideThread{0};   // Get() on this thread returns g_override (one picture with another profile)
Settings g_override;

std::filesystem::path ProfileDir() { return std::filesystem::path(ApexPaths::ApexDirectory()) / L"Capture profiles"; }
std::string CleanName(std::string n) {
    for (auto& c : n) if (strchr("<>:\"/\\|?*", c) || static_cast<unsigned char>(c) < 32) c = '-';
    n = Trim(n);
    while (!n.empty() && (n.back() == '.' || n.back() == ' ')) n.pop_back();
    return n.empty() ? std::string("Default") : n.substr(0, 60);
}
std::filesystem::path ProfilePath(const std::string& name) { return ProfileDir() / (ApexUtil::ToWide(CleanName(name)) + L".ini"); }

bool WriteText(const std::filesystem::path& file, const std::string& text) {
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    const auto tmp = std::filesystem::path(file).concat(L".tmp");
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        std::string crlf; // Windows line ends: it is opened in Notepad
        crlf.reserve(text.size() + 200);
        for (char c : text) { if (c == '\n') crlf += '\r'; crlf += c; }
        out << crlf;
    }
    if (!MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING)) { DeleteFileW(tmp.c_str()); return false; }
    return true;
}

// the profile in use, saved from g_s
void WriteProfileLocked() {
    g_s.profileActive = CleanName(g_s.profileActive);
    if (!WriteText(ProfilePath(g_s.profileActive), Render(g_s, true)))
        LOG_WARNING("[Capture] Capture profile could not be written: " + g_s.profileActive);
}

// The values every profile shares come from the one in use
void KeepShared(Settings& st, const Settings& from) {
    st.reloadOnChange = from.reloadOnChange;
    st.keyScreenshot = from.keyScreenshot;
    st.keyRecord = from.keyRecord;
    st.keyNextProfile = from.keyNextProfile;
    st.profileNote = from.profileNote;
}

void ScanProfilesLocked() {
    g_profilesScanned = GetTickCount64();
    std::vector<Profile> list;
    std::error_code ec;
    for (const auto& e : std::filesystem::directory_iterator(ProfileDir(), ec)) {
        if (!e.is_regular_file(ec) || _wcsicmp(e.path().extension().c_str(), L".ini") != 0) continue;
        Profile p;
        p.name = ApexUtil::ToUtf8(e.path().stem().wstring());
        std::string bad;
        if (!ParseFile(e.path(), p.s, nullptr, bad)) continue;
        p.s.profileActive = p.name;
        list.push_back(std::move(p));
    }
    std::sort(list.begin(), list.end(), [](const Profile& a, const Profile& b) { return _stricmp(a.name.c_str(), b.name.c_str()) < 0; });
    g_profiles = std::move(list);
}

// Reads the file into g_s (defaults first). Missing: written with the defaults.
void LoadLocked() {
    const auto file = File();
    g_loaded = true;
    Settings st;
    std::string bad;
    if (!ParseFile(file, st, &g_unknown, bad)) {
        g_s = Settings{};
        g_unknown.clear();
        // first time: the screenshot settings the player already had ([ui] screenshot_*)
        const ApexConfig::UiSettings ui = ApexConfig::GetUi();
        if (ui.screenshotKey.vk) g_s.keyScreenshot = ApexConfig::KeyChordText(ui.screenshotKey);
        g_s.shotGameUi = !ui.screenshotHideGameUi;
        if (ui.screenshotFolder == 1) g_s.shotPath = "%ApexFolder%\\Screenshots";
        else if (ui.screenshotFolder == 2 && !ui.screenshotCustomFolder.empty()) g_s.shotPath = ui.screenshotCustomFolder;
        WriteLocked();
        ScanProfilesLocked();
        LOG_INFO("[Capture] ApexCapture.ini written with the defaults: " + ApexUtil::ToUtf8(file.wstring()));
        return;
    }
    // Active typed to another existing profile in the file: switch to it (its values, the shared ones from here)
    const std::string active = CleanName(st.profileActive);
    if (g_hadLoad && !g_s.profileActive.empty() && _stricmp(active.c_str(), CleanName(g_s.profileActive).c_str()) != 0 &&
        std::filesystem::exists(ProfilePath(active))) {
        Settings other;
        std::string bad2;
        if (ParseFile(ProfilePath(active), other, nullptr, bad2)) {
            KeepShared(other, st);
            other.profileActive = active;
            st = other;
        }
    }
    g_s = st;
    g_seen = WriteTime(file);
    g_lastError = bad.empty() ? std::string() : "Not understood (defaults kept): " + bad;
    WriteProfileLocked(); // the profile in use follows the file
    ScanProfilesLocked();
    g_hadLoad = true;
    LOG_INFO("[Capture] ApexCapture.ini read (profile " + g_s.profileActive + ")" + (bad.empty() ? std::string() : "; " + g_lastError));
}

// Switch: the one in use is saved first; the new one's values with the shared ones kept
bool SwitchProfileLocked(const std::string& want) {
    const std::string name = CleanName(want);
    if (_stricmp(name.c_str(), g_s.profileActive.c_str()) == 0) return false;
    Settings st;
    std::string bad;
    if (!ParseFile(ProfilePath(name), st, nullptr, bad)) return false;
    WriteProfileLocked();
    KeepShared(st, g_s);
    st.profileActive = name;
    g_s = st;
    g_dirty = false;
    WriteLocked();
    ScanProfilesLocked();
    LOG_INFO("[Capture] Capture profile: " + name);
    return true;
}

void EnsureLoaded() { if (!g_loaded) LoadLocked(); }

std::wstring KnownFolder(int csidl) {
    wchar_t buf[MAX_PATH]{};
    return SUCCEEDED(SHGetFolderPathW(nullptr, csidl, nullptr, SHGFP_TYPE_CURRENT, buf)) ? std::wstring(buf) : std::wstring();
}

std::wstring Replace(std::wstring s, const std::wstring& token, const std::wstring& with) {
    std::wstring lower = s, lt = token;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
    std::transform(lt.begin(), lt.end(), lt.begin(), ::towlower);
    for (size_t at = lower.find(lt); at != std::wstring::npos; at = lower.find(lt, at + with.size())) {
        s.replace(at, token.size(), with);
        lower.replace(at, token.size(), with);
    }
    return s;
}

std::wstring StripSlash(std::wstring s) { while (!s.empty() && (s.back() == L'\\' || s.back() == L'/')) s.pop_back(); return s; }

std::filesystem::path CounterFile() { return std::filesystem::path(ApexPaths::ApexDirectory()) / L"ApexCapture_counter.txt"; }
unsigned NextCounter() {
    static std::mutex m;
    std::lock_guard<std::mutex> lk(m);
    unsigned n = 0;
    { std::ifstream in(CounterFile()); in >> n; }
    ++n;
    { std::ofstream out(CounterFile(), std::ios::trunc); out << n; }
    return n;
}

std::wstring SafeName(std::wstring s) {
    for (auto& c : s) if (wcschr(L"\\/:*?\"<>|", c) || c < 32) c = L'_';
    return s;
}

// ---------------------------------------------------------------------------------------------- "before" picture
IDirect3DSurface9* g_beforeRt = nullptr;
UINT g_beforeW = 0, g_beforeH = 0;
bool g_wantBefore = false, g_beforeFresh = false, g_beforeLogged = false;
void ReleaseBefore() { if (g_beforeRt) { g_beforeRt->Release(); g_beforeRt = nullptr; } g_beforeFresh = false; }

// ---------------------------------------------------------------------------------------------- video control
std::atomic<bool> g_toggle{false};
bool g_externalOn = false;
bool g_videoHidUi = false;
unsigned long long g_lastDotNote = 0;
int g_clipPart = 1;                       // %Part%: the clip of the running recording (NewClipEveryMB)
double g_clipBaseSec = 0.0;               // seconds of the clips already saved in this recording
unsigned long long g_externalStart = 0;   // GetTickCount64 when the external recorder's key was sent
double g_lastSec = 0.0;                   // the running video's length and size, kept for the "saved" note
unsigned long long g_lastBytes = 0;
// The video is read at Present (after Apex draws) when the menu or the timer has to be in it
bool GrabAtPresent(const Settings& s) { return s.videoApexMenu || (s.timerInVideo && (s.recordingDot || s.redFrame)); }
std::string Clock(int sec, bool hours) {
    sec = std::max(0, sec);
    if (hours || sec >= 3600) return std::format("{}:{:02}:{:02}", sec / 3600, sec / 60 % 60, sec % 60);
    return std::format("{:02}:{:02}", sec / 60, sec % 60);
}
int g_bbH = 0, g_bbW = 0; // the back buffer, for the menu's estimate

void SendChord(UINT vk, bool ctrl, bool shift, bool alt, bool win) {
    std::vector<INPUT> in;
    auto key = [&](WORD k, bool up) { INPUT i{}; i.type = INPUT_KEYBOARD; i.ki.wVk = k; i.ki.dwFlags = up ? KEYEVENTF_KEYUP : 0; in.push_back(i); };
    if (win) key(VK_LWIN, false);
    if (ctrl) key(VK_CONTROL, false);
    if (shift) key(VK_SHIFT, false);
    if (alt) key(VK_MENU, false);
    key(static_cast<WORD>(vk), false);
    key(static_cast<WORD>(vk), true);
    if (alt) key(VK_MENU, true);
    if (shift) key(VK_SHIFT, true);
    if (ctrl) key(VK_CONTROL, true);
    if (win) key(VK_LWIN, true);
    SendInput(static_cast<UINT>(in.size()), in.data(), sizeof(INPUT));
}

int KbpsFor(const Settings& s) {
    if (s.bitrateMode) return s.bitrateMbps * 1000;
    const double base = s.maxHeight <= 720 ? 8000.0 : s.maxHeight <= 1080 ? 16000.0 : 28000.0; // kbit/s at 30 fps, Quality 80
    const double fps = s.fps >= 50 ? 1.6 : s.fps / 30.0;
    return std::clamp(static_cast<int>(base * fps * s.quality / 80.0), 1000, 200000);
}

void Note(const std::string& text, int seconds, Captures::NoteKind kind) {
    if (Get().showNotice || kind == Captures::NoteKind::Warning) Captures::Notify(text, seconds, kind);
}

void StartRecording(IDirect3DDevice9* dev, bool nextClip = false) {
    const Settings s = Get();
    if (!s.videoEnabled) return;
    if (s.external) {
        UINT vk = 0; bool c = false, sh = false, a = false, w = false;
        if (!ParseKey(s.externalKey, vk, c, sh, a, w)) { Note("ExternalKey in ApexCapture.ini is not a key: " + s.externalKey, 5, Captures::NoteKind::Warning); return; }
        SendChord(vk, c, sh, a, w);
        g_externalOn = true;
        g_externalStart = GetTickCount64();
        PlaySoundFile();
        Note("External recorder started (" + s.externalKey + ")", 3, Captures::NoteKind::Info);
        return;
    }
    std::string why;
    const auto folder = ResolveFolder(s.videoPath, why);
    if (folder.empty()) { Note("No video: " + why, 6, Captures::NoteKind::Warning); return; }
    g_clipPart = nextClip ? g_clipPart + 1 : 1;
    auto file = NewFile(folder, s.videoNaming, s.mkv ? "mkv" : "mp4");
    if (s.mkv) { // recorded as <name>.mp4 next to the free <name>.mkv
        file.replace_extension(L".mp4");
        std::error_code ec;
        for (unsigned k = 2; std::filesystem::exists(file, ec) && k < 1000; ++k) {
            file = NewFile(folder, s.videoNaming + std::format("_{}", k), "mkv");
            file.replace_extension(L".mp4");
        }
    }
    VideoMp4::Options o;
    o.fps = s.fps;
    o.maxHeight = s.maxHeight;
    o.hevc = s.hevc;
    o.fragmented = s.fragmented;
    o.bitrateKbps = KbpsFor(s);
    o.audio = s.audio;
    o.mouse = s.videoMouse;
    o.fragmented = s.fragmented || s.mkv;
    o.mkv = s.mkv;
    o.keepMp4 = s.keepMp4;
    o.nextClip = nextClip;
    if (!VideoMp4::Start(dev, file, o, why)) { Note("No video: " + why, 6, Captures::NoteKind::Warning); return; }
    if (nextClip) { LOG_INFO(std::format("[Video] New clip {} at the NewClipEveryMB size", g_clipPart)); return; } // UI, sound and timer carry on
    g_clipBaseSec = 0.0;
    g_videoHidUi = !s.videoGameUi && Captures::HideGameUiForVideo(true);
    PlaySoundFile();
    g_lastDotNote = 0;
}

void StopRecording() {
    const Settings s = Get();
    if (g_externalOn) {
        UINT vk = 0; bool c = false, sh = false, a = false, w = false;
        if (ParseKey(s.externalKey, vk, c, sh, a, w)) SendChord(vk, c, sh, a, w);
        g_externalOn = false;
        PlaySoundFile();
        Note("External recorder stopped; the video is in its own folder", 4, Captures::NoteKind::Info);
        return;
    }
    if (!VideoMp4::Active()) return;
    VideoMp4::Stop();
    if (g_videoHidUi) Captures::HideGameUiForVideo(false);
    g_videoHidUi = false;
    PlaySoundFile();
    Note("Saving the video...", 4, Captures::NoteKind::Saving);
}

void OnPresent(IDirect3DDevice9* dev) {
    Tick();
    if (!g_bbH && dev) { IDirect3DSurface9* bb = nullptr; if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) { D3DSURFACE_DESC d{}; bb->GetDesc(&d); g_bbW = static_cast<int>(d.Width); g_bbH = static_cast<int>(d.Height); bb->Release(); } }
    if (g_toggle.exchange(false)) { if (Recording()) StopRecording(); else StartRecording(dev); }
    if (VideoMp4::Active()) {
        const Settings s = Get();
        if (VideoMp4::Failed()) StopRecording();
        else if ((s.maxLength > 0 && g_clipBaseSec + VideoMp4::Seconds() >= s.maxLength) ||
                 (s.newClipMB <= 0 && s.maxFileMB > 0 && VideoMp4::BytesWritten() >= (static_cast<unsigned long long>(s.maxFileMB) << 20))) {
            LOG_INFO("[Video] Stopped at the MaxLength / MaxFileMB limit");
            StopRecording();
        } else if (s.newClipMB > 0 && VideoMp4::BytesWritten() >= (static_cast<unsigned long long>(s.newClipMB) << 20)) {
            // the clip is saved and the recording goes on in a new file (a frame or two between them)
            g_clipBaseSec += VideoMp4::Seconds();
            VideoMp4::Stop();
            StartRecording(dev, true);
            if (!VideoMp4::Active()) { // the new clip could not start: end the recording as the key would
                if (g_videoHidUi) Captures::HideGameUiForVideo(false);
                g_videoHidUi = false;
                PlaySoundFile();
            }
        } else {
            if (GrabAtPresent(s)) VideoMp4::Grab(dev); // the Apex menu / the timer is drawn by now
            g_lastSec = VideoMp4::Seconds();
            g_lastBytes = VideoMp4::BytesWritten();
        }
    }
    VideoMp4::Pump(dev);
    bool ok = false;
    const std::string result = VideoMp4::TakeResult(ok);
    if (!result.empty()) {
        LOG_INFO("[Video] " + result);
        if (!ok) Note(result, 8, Captures::NoteKind::Warning);
        else if (Get().savedNote) Note(I18n::Trf("Video saved: {} \xC2\xB7 {} MB", Clock(static_cast<int>(g_lastSec), false),
                                                  static_cast<int>(g_lastBytes / (1024ull * 1024ull) + 1)), 5, Captures::NoteKind::Success);
    }
}

void BeforeOverlay(IDirect3DDevice9* dev) {
    if (VideoMp4::Active() && !GrabAtPresent(Get())) VideoMp4::Grab(dev); // look + colour done, Apex menu and notes not yet drawn
}

void OnPreReset(IDirect3DDevice9*) {
    if (VideoMp4::Active()) { StopRecording(); LOG_INFO("[Video] Stopped: the screen was reset"); }
    VideoMp4::Release();
    g_bbH = g_bbW = 0;
    ReleaseBefore();
}

// ---------------------------------------------------------------------------------------------- menu helpers
// The rows themselves are drawn by the Settings page (apex_gui.cpp): Settings > Menu "Screenshot capture" and
// "Video recording", keys on Settings > Shortcuts. Labels and descriptions are translated by the widgets.
// The tokens of the folder and file-name boxes: Apex's tooltip (wrapped, 28 font sizes) while the box is hovered or typed in
void TokenTooltip() {
    if (!ImGui::IsItemActive() && !ImGui::IsItemHovered(ImGuiHoveredFlags_ForTooltip)) return;
    static const char* const kTokens[][2] = {
        {"%SaveName%", "The save in play"},       {"%Date%", "Date, as 2026-11-14"},         {"%Day%", "Day, 01 to 31"},
        {"%Month%", "Month, 01 to 12"},           {"%Year%", "Year, as 2026"},               {"%YearShort%", "Year, as 26"},
        {"%MonthName%", "Month name"},            {"%MonthShort%", "Short month name"},      {"%Weekday%", "Weekday name"},
        {"%WeekdayShort%", "Short weekday name"}, {"%TimeHour%", "Hour, 00 to 23"},          {"%Hour12%", "Hour, 01 to 12"},
        {"%AmPm%", "AM or PM"},                   {"%TimeMinute%", "Minute"},                {"%TimeSecond%", "Second"},
        {"%TimeMS%", "Milliseconds"},             {"%PresetName%", "The look's preset name"}, {"%Look%", "Look on or off"},
        {"%Type%", "Screenshot or Video"},        {"%Part%", "Clip number of a long video"}, {"%Width%", "Picture width"},
        {"%Height%", "Picture height"},           {"%Counter%", "A running number"}};
    if (!ImGui::BeginTooltip()) return;
    ImGui::TextUnformatted(I18n::Tr("Tokens for the folder and the file name"));
    ImGui::Spacing();
    float tokenW = 0.0f;
    for (const auto& t : kTokens) tokenW = std::fmax(tokenW, ImGui::CalcTextSize(t[0]).x);
    const float gap = ImGui::GetFontSize();
    for (const auto& t : kTokens) {
        ImGui::TextColored(VioletTheme::Col(VioletTheme::kAccent), "%s", t[0]);
        ImGui::SameLine(tokenW + gap + ImGui::GetStyle().WindowPadding.x);
        ImGui::TextColored(VioletTheme::Col(VioletTheme::kTextMuted), "%s", I18n::Tr(t[1]));
    }
    ImGui::EndTooltip();
}

bool TextRow(const char* label, const char* desc, std::string& value, float width = 300.0f, bool tokens = false) {
    static std::map<const void*, std::string> editing;
    bool changed = false;
    const float w = width * ApexUi::Unit();
    if (ApexUi::BeginControlRow(label, desc, w)) {
        const ApexUi::ControlSizeScope size(ApexUi::ControlSize::Compact);
        char buf[512];
        auto it = editing.find(&value);
        snprintf(buf, sizeof(buf), "%s", it != editing.end() ? it->second.c_str() : value.c_str());
        ImGui::SetNextItemWidth(w);
        ImGui::PushID(&value);
        ImGui::InputText("##t", buf, sizeof(buf));
        if (tokens) TokenTooltip();
        if (ImGui::IsItemActive()) editing[&value] = buf;
        if (ImGui::IsItemDeactivated()) {
            if (value != buf) { value = buf; changed = true; }
            editing.erase(&value);
        }
        ImGui::PopID();
        ApexUi::EndControlRow();
    }
    return changed;
}

bool IntSlider(const char* label, const char* desc, int& v, int lo, int hi, int def, const char* fmt = "%.0f") {
    float f = static_cast<float>(v);
    ApexUi::SliderOptions o;
    o.format = fmt;
    o.tooltip = desc;
    o.defaultValue = static_cast<float>(def);
    if (!ApexUi::Slider(label, &f, static_cast<float>(lo), static_cast<float>(hi), o)) return false;
    const int n = std::clamp(static_cast<int>(std::lround(f)), lo, hi);
    if (n == v) return false;
    v = n;
    return true;
}

bool Pick(const char* label, const char* desc, const char* id, int& v, const char* const* labels, int count, int def) {
    int cur = v;
    if (!ApexUi::SegmentedRow(label, desc, id, &cur, labels, count, nullptr, nullptr, def)) return false;
    v = cur;
    return true;
}

void OpenPath(const std::wstring& path, bool select) {
    if (select) ShellExecuteW(nullptr, L"open", L"explorer.exe", (L"/select,\"" + path + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
    else ShellExecuteW(nullptr, L"open", path.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void OpenFolderOf(const std::string& path) {
    std::string why;
    const auto f = ResolveFolder(path, why);
    if (!f.empty()) OpenPath(f.wstring(), false);
}

// Keeps [ui] screenshot_* (the existing screenshot key and its Shortcuts rows) and the file the same, both ways
void SyncUi() {
    ApexConfig::UiSettings ui = ApexConfig::GetUi();
    Settings s;
    { std::lock_guard<std::mutex> lk(g_lock); s = g_s; }
    auto folderOf = [](const std::string& path, int& mode, std::string& custom) {
        const std::string p = Lower(Trim(path));
        if (p == "%gamescreenshots%") { mode = 0; custom.clear(); }
        else if (p == "%apexfolder%\\screenshots" || p == "%apexfolder%/screenshots") { mode = 1; custom.clear(); }
        else if (p.find('%') != std::string::npos) { mode = 2; custom = Trim(path); } // tokens stay as typed (not today's folder)
        else { mode = 2; std::string why; custom = ApexUtil::ToUtf8(ResolveFolder(path, why).wstring()); if (custom.empty()) custom = path; }
    };
    const bool uiChanged = g_uiSeenSet && (ui.screenshotKey.vk != g_uiSeen.key.vk || ui.screenshotKey.ctrl != g_uiSeen.key.ctrl || ui.screenshotKey.shift != g_uiSeen.key.shift ||
                           ui.screenshotKey.alt != g_uiSeen.key.alt || ui.screenshotHideGameUi != g_uiSeen.hide || ui.screenshotFolder != g_uiSeen.folder ||
                           ui.screenshotCustomFolder != g_uiSeen.custom);
    if (uiChanged) { // changed on the Shortcuts tab: into the file
        s.keyScreenshot = ApexConfig::KeyChordText(ui.screenshotKey);
        s.shotGameUi = !ui.screenshotHideGameUi;
        s.shotPath = ui.screenshotFolder == 0 ? "%GameScreenshots%" : ui.screenshotFolder == 1 ? "%ApexFolder%\\Screenshots" : ui.screenshotCustomFolder;
        Set(s);
    } else { // the file into [ui]
        UINT vk = 0; bool c = false, sh = false, a = false, w = false;
        ApexConfig::UiSettings want = ui;
        if (ParseKey(s.keyScreenshot, vk, c, sh, a, w) && !w) {
            const bool bareTyping = !c && !sh && !a && ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9') || vk == VK_SPACE);
            if (!bareTyping) want.screenshotKey = {vk, c, sh, a};
        }
        want.screenshotHideGameUi = !s.shotGameUi;
        folderOf(s.shotPath, want.screenshotFolder, want.screenshotCustomFolder);
        if (!want.screenshotShortcutEnabled) want.screenshotShortcutEnabled = true;
        const bool differs = want.screenshotKey.vk != ui.screenshotKey.vk || want.screenshotKey.ctrl != ui.screenshotKey.ctrl || want.screenshotKey.shift != ui.screenshotKey.shift ||
                             want.screenshotKey.alt != ui.screenshotKey.alt || want.screenshotHideGameUi != ui.screenshotHideGameUi || want.screenshotFolder != ui.screenshotFolder ||
                             want.screenshotCustomFolder != ui.screenshotCustomFolder || want.screenshotShortcutEnabled != ui.screenshotShortcutEnabled;
        if (differs) ApexConfig::SetUi(want);
        ui = want;
    }
    g_uiSeen = {ui.screenshotKey, ui.screenshotHideGameUi, ui.screenshotFolder, ui.screenshotCustomFolder};
    g_uiSeenSet = true;
}

// ---------------------------------------------------------------------------------------------- current save (%SaveName%)
// The game keeps each save as a folder Saves\<name>.sims3. A CreateFileW hook remembers the last such folder the game
// opens: loading reads it, saving (also "Save As" under a new name) writes it. Unverified in game: whether the main
// menu's save list opens every save (then the name is the last one listed until a save is loaded). Before any save is
// seen, the most recently written save folder is used.
SRWLOCK g_saveLock = SRWLOCK_INIT;
wchar_t g_saveName[MAX_PATH]{};
using CreateFileWFn = HANDLE(WINAPI*)(LPCWSTR, DWORD, DWORD, LPSECURITY_ATTRIBUTES, DWORD, DWORD, HANDLE);
CreateFileWFn g_realCreateFileW = nullptr;
bool g_saveWatchTried = false;

// "...\Saves\Medieval Kingdom.sims3\..." -> "Medieval Kingdom". No allocation, no file calls (runs inside CreateFileW).
void NoteSavePath(const wchar_t* p) noexcept {
    if (!p) return;
    const wchar_t* name = nullptr;
    for (const wchar_t* s = p; *s; ++s) {
        if ((s[0] == L'\\' || s[0] == L'/') && (s[1] | 0x20) == L's' && (s[2] | 0x20) == L'a' && (s[3] | 0x20) == L'v' &&
            (s[4] | 0x20) == L'e' && (s[5] | 0x20) == L's' && (s[6] == L'\\' || s[6] == L'/'))
            name = s + 7;
    }
    if (!name) return;
    const wchar_t* end = name;
    while (*end && *end != L'\\' && *end != L'/') ++end;
    const size_t len = static_cast<size_t>(end - name);
    if (len <= 6 || len >= MAX_PATH || _wcsnicmp(end - 6, L".sims3", 6) != 0) return; // not *.sims3.backup or loose files
    const size_t n = len - 6;
    AcquireSRWLockExclusive(&g_saveLock);
    if (wcsncmp(g_saveName, name, n) != 0 || g_saveName[n] != 0) {
        wmemcpy(g_saveName, name, n);
        g_saveName[n] = 0;
    }
    ReleaseSRWLockExclusive(&g_saveLock);
}

HANDLE WINAPI HookCreateFileW(LPCWSTR file, DWORD access, DWORD share, LPSECURITY_ATTRIBUTES sa, DWORD disposition, DWORD flags, HANDLE tmpl) {
    NoteSavePath(file);
    return g_realCreateFileW(file, access, share, sa, disposition, flags, tmpl);
}

void InstallSaveWatch() {
    g_saveWatchTried = true;
    HMODULE mod = GetModuleHandleW(L"kernelbase.dll");
    FARPROC target = mod ? GetProcAddress(mod, "CreateFileW") : nullptr;
    if (!target && (mod = GetModuleHandleW(L"kernel32.dll")) != nullptr) target = GetProcAddress(mod, "CreateFileW");
    if (!target) { LOG_WARNING("[Capture] %SaveName%: CreateFileW not found; the newest save folder is used"); return; }
    g_realCreateFileW = reinterpret_cast<CreateFileWFn>(target);
    if (DetourBatch::InstallHooks({{reinterpret_cast<void**>(&g_realCreateFileW), reinterpret_cast<void*>(&HookCreateFileW)}}))
        LOG_INFO("[Capture] %SaveName%: watching which save the game opens");
    else {
        g_realCreateFileW = nullptr;
        LOG_WARNING("[Capture] %SaveName%: hook failed; the newest save folder is used");
    }
}

// The newest Saves\*.sims3 folder (by last write), checked at most every 2 s
std::wstring NewestSaveName() {
    static std::mutex m;
    static std::wstring cached;
    static unsigned long long at = 0;
    std::lock_guard<std::mutex> lk(m);
    const unsigned long long now = GetTickCount64();
    if (at && now - at < 2000) return cached;
    at = now;
    cached.clear();
    std::error_code ec;
    std::filesystem::file_time_type best{};
    for (const auto& e : std::filesystem::directory_iterator(std::filesystem::path(StripSlash(ApexPaths::GameDocumentsDirectory())) / L"Saves", ec)) {
        const auto& p = e.path();
        if (_wcsicmp(p.extension().c_str(), L".sims3") != 0 || !e.is_directory(ec)) continue;
        const auto t = e.last_write_time(ec);
        if (ec) continue;
        if (cached.empty() || t > best) { best = t; cached = p.stem().wstring(); }
    }
    return cached;
}

std::wstring CurrentSaveName() {
    std::wstring n;
    AcquireSRWLockShared(&g_saveLock);
    n = g_saveName;
    ReleaseSRWLockShared(&g_saveLock);
    if (n.empty()) n = NewestSaveName();
    return n.empty() ? std::wstring(L"The Sims 3") : n;
}

// Date, time and save tokens, shared by folder paths and file names
std::wstring TimeTokens(std::wstring n, const SYSTEMTIME& t) {
    n = Replace(n, L"%Date%", std::format(L"{:04}-{:02}-{:02}", t.wYear, t.wMonth, t.wDay));
    n = Replace(n, L"%Year%", std::format(L"{:04}", t.wYear));
    n = Replace(n, L"%Month%", std::format(L"{:02}", t.wMonth));
    n = Replace(n, L"%Day%", std::format(L"{:02}", t.wDay));
    n = Replace(n, L"%TimeHour%", std::format(L"{:02}", t.wHour));
    n = Replace(n, L"%TimeMinute%", std::format(L"{:02}", t.wMinute));
    n = Replace(n, L"%TimeSecond%", std::format(L"{:02}", t.wSecond));
    n = Replace(n, L"%TimeMS%", std::format(L"{:03}", t.wMilliseconds));
    n = Replace(n, L"%YearShort%", std::format(L"{:02}", t.wYear % 100));
    n = Replace(n, L"%Hour12%", std::format(L"{:02}", t.wHour % 12 ? t.wHour % 12 : 12));
    n = Replace(n, L"%AmPm%", t.wHour < 12 ? L"AM" : L"PM");
    auto dateText = [&t](const wchar_t* fmt) { // month and day names in Windows' language
        wchar_t b[64]{};
        return GetDateFormatW(LOCALE_USER_DEFAULT, 0, &t, fmt, b, 64) ? std::wstring(b) : std::wstring();
    };
    if (n.find(L"%MonthName%") != std::wstring::npos || n.find(L"%monthname%") != std::wstring::npos) n = Replace(n, L"%MonthName%", dateText(L"MMMM"));
    n = Replace(n, L"%MonthShort%", dateText(L"MMM"));
    n = Replace(n, L"%WeekdayShort%", dateText(L"ddd"));
    n = Replace(n, L"%Weekday%", dateText(L"dddd"));
    if (n.find(L'%') != std::wstring::npos) n = Replace(n, L"%SaveName%", SafeName(CurrentSaveName()));
    return n;
}

} // namespace

// ---------------------------------------------------------------------------------------------- folder rows
// Windows' folder dialog on its own thread (as the profile file dialog); the chosen path is taken by FolderRow on a
// later frame, nothing when cancelled. One dialog at a time; slot says which row asked.
namespace {
std::mutex g_pickLock;
std::optional<std::wstring> g_pickResult;
int g_pickSlot = -1;
std::atomic<bool> g_pickBusy{false};

void PickFolder(int slot, std::string current, const char* titleKey) {
    if (g_pickBusy.exchange(true)) return;
    std::wstring title = ApexUtil::ToWide(I18n::Tr(titleKey));
    if (!HookGuard::StartDetached("Captures: folder dialog", [slot, current = std::move(current), title = std::move(title)] {
        std::optional<std::wstring> picked;
        struct Finish {
            std::optional<std::wstring>& picked;
            int slot;
            ~Finish() {
                std::lock_guard<std::mutex> lock(g_pickLock);
                g_pickResult = std::move(picked);
                g_pickSlot = slot;
                g_pickBusy = false;
            }
        } finish{picked, slot};
        const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(com)) return;
        IFileDialog* dialog = nullptr;
        if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
            DWORD options = 0;
            dialog->GetOptions(&options);
            dialog->SetOptions(options | FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM | FOS_NOCHANGEDIR | FOS_PATHMUSTEXIST);
            dialog->SetTitle(title.c_str());
            if (!current.empty()) {
                IShellItem* start = nullptr;
                if (SUCCEEDED(SHCreateItemFromParsingName(ApexUtil::ToWide(current).c_str(), nullptr, IID_PPV_ARGS(&start)))) {
                    dialog->SetFolder(start);
                    start->Release();
                }
            }
            if (SUCCEEDED(dialog->Show(nullptr))) {
                IShellItem* item = nullptr;
                if (SUCCEEDED(dialog->GetResult(&item))) {
                    PWSTR path = nullptr;
                    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                        picked = path;
                        CoTaskMemFree(path);
                    }
                    item->Release();
                }
            }
            dialog->Release();
        }
        CoUninitialize();
    })) g_pickBusy = false;
}

bool TakePicked(int slot, std::string& out) {
    std::lock_guard<std::mutex> lock(g_pickLock);
    if (!g_pickResult || g_pickSlot != slot) return false;
    out = ApexUtil::ToUtf8(*g_pickResult);
    g_pickResult.reset();
    return true;
}
} // namespace

bool FolderRow(const char* label, const char* id, std::string& value, int slot, const char* pickTip) {
    bool changed = false;
    std::string picked;
    if (TakePicked(slot, picked) && !picked.empty() && picked != value) { value = picked; changed = true; }
    ImGui::PushID(id);
    const float u = ApexUi::Unit();
    const char* change = "Change##FolderPick";
    const char* open = "Open##FolderOpen";
    const float w = ApexUi::ButtonWidth(change, true) + ApexUi::kSpace3 * u + ApexUi::ButtonWidth(open, true);
    // the row's description is the folder itself (as set: tokens such as %Day% stay visible)
    if (ApexUi::BeginControlRow(label, value.empty() ? "No folder set" : value.c_str(), w)) {
        ImGui::BeginDisabled(g_pickBusy.load());
        if (ApexUi::IconTextButton(change, IconId::FolderOpen, pickTip)) {
            std::string why, start = value;
            if (start.find('%') != std::string::npos) start = ApexUtil::ToUtf8(ResolveFolder(value, why).wstring());
            PickFolder(slot, start, slot == 0 ? "Choose a folder for the screenshots" : "Choose a folder for the videos");
        }
        ImGui::EndDisabled();
        ImGui::SameLine(0.0f, ApexUi::kSpace3 * u);
        if (ApexUi::IconTextButton(open, IconId::ExternalLink, "Opens this folder in Explorer")) OpenFolderOf(value);
        ApexUi::EndControlRow();
    }
    // the plain text box, for typing a path or tokens, behind its own arrow
    if (ApexUi::BeginAdvanced("##TypeFolder", "Type a folder")) {
        static std::map<ImGuiID, std::string> editing;
        const float bw = 300.0f * u;
        if (ApexUi::BeginControlRow("Folder path", "Hover the box for the tokens; applies when you leave the box", bw)) {
            const ApexUi::ControlSizeScope size(ApexUi::ControlSize::Compact);
            const ImGuiID key = ImGui::GetID("##Path");
            char buf[512];
            auto it = editing.find(key);
            snprintf(buf, sizeof(buf), "%s", it != editing.end() ? it->second.c_str() : value.c_str());
            ImGui::SetNextItemWidth(bw);
            ImGui::InputText("##Path", buf, sizeof(buf));
            TokenTooltip();
            if (ImGui::IsItemActive()) editing[key] = buf;
            if (ImGui::IsItemDeactivated()) {
                editing.erase(key);
                std::string v = Trim(buf);
                while (!v.empty() && v.front() == '"') v.erase(v.begin()); // a pasted "copy as path"
                while (!v.empty() && v.back() == '"') v.pop_back();
                if (!v.empty() && v != value) { value = v; changed = true; }
            }
            ApexUi::EndControlRow();
        }
        ApexUi::EndAdvanced();
    }
    ImGui::PopID();
    return changed;
}

// ---------------------------------------------------------------------------------------------- public
std::filesystem::path File() { return std::filesystem::path(ApexPaths::ApexDirectory()) / L"ApexCapture.ini"; }

Settings Get() {
    if (g_overrideThread.load() == GetCurrentThreadId()) return g_override; // one picture with another profile
    std::lock_guard<std::mutex> lk(g_lock);
    EnsureLoaded();
    return g_s;
}

void Set(const Settings& s) {
    std::lock_guard<std::mutex> lk(g_lock);
    EnsureLoaded();
    g_s = s;
    g_dirty = true;
    g_dirtyAt = GetTickCount64();
}

void Tick() {
    if (!g_saveWatchTried) InstallSaveWatch(); // first frame (main menu), before any save loads
    const unsigned long long now = GetTickCount64();
    bool reloaded = false;
    // capture profiles: keys pressed (switch, next, one picture), and the list kept fresh (files edited by hand)
    std::string switched;
    bool note = false, shot = false;
    Settings shotWith;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        EnsureLoaded();
        if (now - g_profilesScanned > 3000) ScanProfilesLocked();
        if (g_wantNext && !g_profiles.empty()) {
            size_t i = 0;
            for (; i < g_profiles.size(); i++) if (_stricmp(g_profiles[i].name.c_str(), g_s.profileActive.c_str()) == 0) break;
            g_wantProfile = g_profiles[i >= g_profiles.size() ? 0 : (i + 1) % g_profiles.size()].name;
        }
        g_wantNext = false;
        if (!g_wantProfile.empty() && SwitchProfileLocked(g_wantProfile)) { switched = g_s.profileActive; note = g_s.profileNote; }
        g_wantProfile.clear();
        if (!g_wantShot.empty()) {
            for (const Profile& p : g_profiles)
                if (_stricmp(p.name.c_str(), g_wantShot.c_str()) == 0) { shotWith = p.s; shot = true; }
            if (_stricmp(g_wantShot.c_str(), g_s.profileActive.c_str()) == 0) { shotWith = g_s; shot = true; }
            KeepShared(shotWith, g_s);
            g_wantShot.clear();
        }
    }
    if (!switched.empty()) {
        SyncUi();
        if (note) Note(I18n::Trf("Capture profile: {}", switched), 2, Captures::NoteKind::Info);
    }
    if (shot) { // the queued picture copies its settings now; the sound and the "before" frame too
        g_override = shotWith;
        g_overrideThread = GetCurrentThreadId();
        Captures::RequestPlayerScreenshot(!shotWith.shotGameUi);
        g_overrideThread = 0;
        LOG_INFO("[Capture] One picture with profile " + shotWith.profileActive);
    }
    {
        std::lock_guard<std::mutex> lk(g_lock);
        EnsureLoaded();
        if (g_dirty && now - g_dirtyAt >= 500) { g_dirty = false; WriteLocked(); }
        if (now - g_lastCheck < 1000) return;
        g_lastCheck = now;
        const FILETIME t = WriteTime(File());
        if (!t.dwLowDateTime && !t.dwHighDateTime) { WriteLocked(); } // deleted: the current values again
        else if (!g_dirty && g_s.reloadOnChange && !SameTime(t, g_seen)) { LoadLocked(); reloaded = true; }
    }
    SyncUi();
    if (reloaded) {
        std::string err;
        { std::lock_guard<std::mutex> lk(g_lock); err = g_lastError; }
        Note(err.empty() ? "Capture settings read from ApexCapture.ini" : "ApexCapture.ini: " + err, 4,
             err.empty() ? Captures::NoteKind::Info : Captures::NoteKind::Warning);
    }
}

std::filesystem::path ResolveFolder(const std::string& path, std::string& why) {
    std::wstring p = ApexUtil::ToWide(Trim(path));
    if (p.empty()) p = L"%GameScreenshots%";
    { SYSTEMTIME t{}; GetLocalTime(&t); p = TimeTokens(p, t); } // %Day%-%Month%-%Year%, %SaveName% ...: one folder per day or save
    p = Replace(p, L"%GameScreenshots%", StripSlash(ApexPaths::GameDocumentsDirectory()) + L"\\Screenshots");
    p = Replace(p, L"%ApexFolder%", StripSlash(ApexPaths::ApexDirectory()));
    p = Replace(p, L"%Pictures%", KnownFolder(CSIDL_MYPICTURES));
    p = Replace(p, L"%Videos%", KnownFolder(CSIDL_MYVIDEO));
    p = Replace(p, L"%Documents%", KnownFolder(CSIDL_PERSONAL));
    wchar_t env[MAX_PATH * 2]{};
    if (ExpandEnvironmentStringsW(p.c_str(), env, MAX_PATH * 2)) p = env;
    std::filesystem::path f(p);
    if (f.is_relative()) f = std::filesystem::path(StripSlash(ApexPaths::GameDocumentsDirectory())) / f;
    std::error_code ec;
    std::filesystem::create_directories(f, ec);
    if (!std::filesystem::is_directory(f, ec)) { why = "the folder cannot be made: " + ApexUtil::ToUtf8(f.wstring()); return {}; }
    return f;
}

std::filesystem::path NewFile(const std::filesystem::path& folder, const std::string& pattern, const char* ext) {
    SYSTEMTIME t{};
    GetLocalTime(&t);
    std::wstring preset;
    const bool look = GShadeLooksPresetInUse(preset);
    std::wstring presetName = std::filesystem::path(preset).stem().wstring();
    if (presetName.empty()) presetName = L"Apex";
    std::wstring n = ApexUtil::ToWide(pattern.empty() ? std::string("%Date%_%TimeHour%-%TimeMinute%-%TimeSecond%-%TimeMS%_%PresetName%") : pattern);
    n = TimeTokens(n, t);
    const bool video = _stricmp(ext, "mp4") == 0;
    int w = g_bbW, h = g_bbH;
    if (!video) { const int sc = std::clamp(Get().shotScale, 10, 100); w = w * sc / 100; h = h * sc / 100; }
    n = Replace(n, L"%Type%", video ? L"Video" : L"Screenshot");
    n = Replace(n, L"%Part%", std::format(L"{:02}", video ? std::max(1, g_clipPart) : 1));
    n = Replace(n, L"%Width%", std::to_wstring(w));
    n = Replace(n, L"%Height%", std::to_wstring(h));
    n = Replace(n, L"%PresetName%", presetName);
    n = Replace(n, L"%Look%", look ? L"on" : L"off");
    if (n.find(L"%Counter%") != std::wstring::npos || n.find(L"%counter%") != std::wstring::npos) {
        const int digits = std::clamp(Get().counterDigits, 1, 8);
        n = Replace(n, L"%Counter%", folder.empty() ? std::wstring(digits, L'#') : std::format(L"{:0{}}", NextCounter(), digits));
    }
    n = SafeName(n);
    if (n.empty()) n = L"Apex";
    const std::wstring e = L"." + ApexUtil::ToWide(ext);
    std::filesystem::path file = folder / (n + e);
    std::error_code ec;
    for (unsigned k = 2; !folder.empty() && std::filesystem::exists(file, ec) && k < 10000; ++k) file = folder / (n + std::format(L" ({})", k) + e);
    return file;
}

std::wstring PresetFile() {
    std::wstring f;
    return GShadeLooksPresetInUse(f) ? f : std::wstring();
}

// SoundPath "camera": a mechanical shutter made here (two curtain clicks and the mirror's thump), 16-bit mono WAV
const std::vector<BYTE>& CameraShutterWav() {
    static const std::vector<BYTE> wav = [] {
        constexpr int rate = 44100, count = rate * 22 / 100;
        std::vector<float> v(count, 0.0f);
        unsigned seed = 0x2545F491u;
        auto noise = [&seed] { seed = seed * 1664525u + 1013904223u; return static_cast<float>((seed >> 9) & 0x7FFF) / 16383.5f - 1.0f; };
        constexpr double kPi = 3.14159265358979;
        auto click = [&](double at, float amp, double body, double thump) {
            float last = 0.0f;
            for (int i = static_cast<int>(at * rate); i < count; i++) {
                const double t = static_cast<double>(i) / rate - at;
                if (t > 0.08) break;
                const float n = noise();
                const float hp = n - last; // brighter, metallic
                last = n;
                v[i] += amp * static_cast<float>(hp * 0.6 * std::exp(-t / 0.0035) + std::sin(2 * kPi * body * t) * 0.35 * std::exp(-t / 0.007) +
                                                 std::sin(2 * kPi * 140.0 * t) * thump * std::exp(-t / 0.02));
            }
        };
        click(0.000, 0.55f, 2300.0, 0.15);
        click(0.012, 0.25f, 1700.0, 0.0);
        click(0.085, 0.85f, 1900.0, 0.45);
        float peak = 0.0f;
        for (float x : v) peak = std::max(peak, std::fabs(x));
        const float g = peak > 0.0f ? 0.7f / peak : 1.0f;
        std::vector<BYTE> w(44 + count * 2);
        auto put32 = [&w](size_t at, unsigned x) { for (int k = 0; k < 4; k++) w[at + k] = static_cast<BYTE>(x >> (8 * k)); };
        auto put16 = [&w](size_t at, unsigned x) { w[at] = static_cast<BYTE>(x); w[at + 1] = static_cast<BYTE>(x >> 8); };
        memcpy(w.data(), "RIFF", 4); put32(4, static_cast<unsigned>(36 + count * 2)); memcpy(w.data() + 8, "WAVEfmt ", 8);
        put32(16, 16); put16(20, 1); put16(22, 1); put32(24, rate); put32(28, rate * 2); put16(32, 2); put16(34, 16);
        memcpy(w.data() + 36, "data", 4); put32(40, static_cast<unsigned>(count * 2));
        for (int i = 0; i < count; i++) put16(44 + 2 * i, static_cast<unsigned>(static_cast<short>(std::clamp(v[i] * g, -1.0f, 1.0f) * 32767.0f)) & 0xFFFF);
        return w;
    }();
    return wav;
}

std::wstring GShadeSound() {
    // 64-bit Program Files first (the game is 32-bit, so its %ProgramFiles% is Program Files (x86))
    for (const wchar_t* at : {L"%ProgramW6432%\\GShade\\notification.wav", L"%ProgramFiles%\\GShade\\notification.wav"}) {
        wchar_t pf[MAX_PATH]{};
        std::error_code ec;
        if (ExpandEnvironmentStringsW(at, pf, MAX_PATH) && !wcschr(pf, L'%') && std::filesystem::exists(pf, ec)) return pf;
    }
    return {};
}

void PlaySoundFile() {
    const std::string p = Trim(Get().soundPath);
    if (p.empty()) return;
    std::wstring file = Lower(p) == "gshade" ? GShadeSound() : Lower(p) == "camera" ? std::wstring() : ApexUtil::ToWide(p);
    if (!file.empty()) {
        wchar_t env[MAX_PATH * 2]{};
        if (ExpandEnvironmentStringsW(file.c_str(), env, MAX_PATH * 2)) file = env;
        std::error_code ec;
        if (std::filesystem::exists(file, ec)) { PlaySoundW(file.c_str(), nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT); return; }
    }
    PlaySoundW(reinterpret_cast<LPCWSTR>(CameraShutterWav().data()), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT); // camera, or a missing file
}

bool ParseKey(const std::string& text, UINT& vk, bool& ctrl, bool& shift, bool& alt, bool& win) {
    const std::string t = Trim(text);
    ctrl = shift = alt = win = false;
    vk = 0;
    if (t.empty()) return false;
    if (isdigit(static_cast<unsigned char>(t[0]))) { // GShade: vk,ctrl,shift,alt
        int v[4]{};
        if (sscanf_s(t.c_str(), "%d,%d,%d,%d", &v[0], &v[1], &v[2], &v[3]) < 1 || v[0] <= 0 || v[0] > 255) return false;
        vk = static_cast<UINT>(v[0]);
        ctrl = v[1] != 0; shift = v[2] != 0; alt = v[3] != 0;
        return true;
    }
    std::string rest = t;
    // "Win+" is not an Apex key name: taken off first
    for (;;) {
        const size_t plus = rest.find('+');
        if (plus == std::string::npos || plus + 1 >= rest.size()) break;
        const std::string mod = Lower(Trim(rest.substr(0, plus)));
        if (mod == "win" || mod == "windows") { win = true; rest = rest.substr(plus + 1); continue; }
        break;
    }
    ApexConfig::KeyChord c;
    if (!ApexConfig::ParseKeyChord(rest, c) || !c.vk) return false;
    vk = c.vk; ctrl = c.ctrl; shift = c.shift; alt = c.alt;
    return true;
}

void WantBefore(bool on) {
    g_wantBefore = on;
    if (!on) ReleaseBefore();
}

void BeforeLook(IDirect3DDevice9* dev, IDirect3DSurface9* src) {
    if (!g_wantBefore || !dev || !src) return;
    D3DSURFACE_DESC d{};
    src->GetDesc(&d);
    if (g_beforeRt && (g_beforeW != d.Width || g_beforeH != d.Height)) ReleaseBefore();
    if (!g_beforeRt && FAILED(dev->CreateRenderTarget(d.Width, d.Height, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &g_beforeRt, nullptr))) {
        g_beforeRt = nullptr;
        return;
    }
    g_beforeW = d.Width;
    g_beforeH = d.Height;
    g_beforeFresh = SUCCEEDED(dev->StretchRect(src, nullptr, g_beforeRt, nullptr, D3DTEXF_NONE));
    if (!g_beforeFresh && !g_beforeLogged) {
        g_beforeLogged = true;
        LOG_WARNING(std::format("[Capture] The \"before\" picture could not be copied (format {})", static_cast<int>(d.Format)));
    }
}

bool ReadBefore(IDirect3DDevice9* dev, std::vector<BYTE>& bgr, UINT& w, UINT& h) {
    const bool fresh = g_beforeFresh;
    g_beforeFresh = false;
    if (!fresh || !g_beforeRt || !dev) return false;
    IDirect3DSurface9* sys = nullptr;
    bool ok = false;
    if (SUCCEEDED(dev->CreateOffscreenPlainSurface(g_beforeW, g_beforeH, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, nullptr)) &&
        SUCCEEDED(dev->GetRenderTargetData(g_beforeRt, sys))) {
        D3DLOCKED_RECT lr{};
        if (SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
            w = g_beforeW;
            h = g_beforeH;
            bgr.resize(static_cast<size_t>(w) * h * 3);
            for (UINT y = 0; y < h; y++) {
                const BYTE* s = static_cast<const BYTE*>(lr.pBits) + static_cast<size_t>(y) * lr.Pitch;
                BYTE* o = bgr.data() + static_cast<size_t>(y) * w * 3;
                for (UINT x = 0; x < w; x++) { o[3 * x] = s[4 * x]; o[3 * x + 1] = s[4 * x + 1]; o[3 * x + 2] = s[4 * x + 2]; }
            }
            sys->UnlockRect();
            ok = true;
        }
    }
    if (sys) sys->Release();
    return ok;
}

// Runs inside the game's window procedure: it must never throw (an exception there switches Apex's hotkeys off).
bool KeyDown(WPARAM vk, bool repeat) noexcept {
    if (repeat) return false;
    try {
        const Settings s = Get();
        const bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0, alt = GetKeyState(VK_MENU) < 0;
        auto is = [&](const std::string& text) {
            UINT k = 0; bool c = false, sh = false, a = false, w = false;
            if (Trim(text).empty() || !ParseKey(text, k, c, sh, a, w) || w || k != vk) return false;
            if (!c && !sh && !a && ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9') || vk == VK_SPACE)) return false; // typing
            return ctrl == c && shift == sh && alt == a;
        };
        { // capture profiles
            std::lock_guard<std::mutex> lk(g_lock);
            if (is(s.keyNextProfile)) { g_wantNext = true; return true; }
            for (const Profile& p : g_profiles) {
                const bool live = _stricmp(p.name.c_str(), g_s.profileActive.c_str()) == 0;
                const Settings& ps = live ? g_s : p.s;
                if (is(ps.profileShotKey)) { g_wantShot = p.name; return true; }
                if (is(ps.profileKey)) { if (!live) g_wantProfile = p.name; return true; }
            }
        }
        if (!s.videoEnabled || !is(s.keyRecord)) return false;
        ToggleRecording();
        return true;
    } catch (const std::exception& e) {
        try { LOG_ERROR(std::string("[Capture] Record key check failed: ") + e.what()); } catch (...) {}
    } catch (...) {
        try { LOG_ERROR("[Capture] Record key check failed: unknown exception"); } catch (...) {}
    }
    return false;
}

void ToggleRecording() { g_toggle = true; }
bool Recording() { return VideoMp4::Active() || g_externalOn; }
int RecordingSeconds() { return static_cast<int>(g_clipBaseSec + VideoMp4::Seconds()); } // all clips of the recording

// The recording timer: a pill in the Apex note style (red dot, time, file size, stop key) at TimerPosition, and the
// optional red frame. Drawn on the foreground list, no input. Kept out of the video unless TimerInVideo (GrabAtPresent),
// out of screenshots, and hidden when the menu is recorded but the timer is not (it would be in the video).
bool DrawTimer() {
    try {
        if (!Recording() || Captures::ScreenshotPending()) return false;
        const Settings s = Get();
        if (!s.recordingDot && !s.redFrame) return false;
        if (s.videoApexMenu && !s.timerInVideo && VideoMp4::Active()) return false;
        const bool builtin = VideoMp4::Active();
        const int sec = builtin ? static_cast<int>(VideoMp4::Seconds())
                                : static_cast<int>((GetTickCount64() - g_externalStart) / 1000ull);
        const unsigned long long bytes = builtin ? VideoMp4::BytesWritten() : 0ull;
        // time left before MaxLength / MaxFileMB (built-in only; the size estimate uses the rate so far)
        int left = -1;
        if (builtin && s.warnBefore > 0) {
            if (s.maxLength > 0) left = s.maxLength - sec;
            if (s.maxFileMB > 0 && sec >= 3 && bytes > 0) {
                const double rate = static_cast<double>(bytes) / std::max(1, sec);
                const double room = static_cast<double>(static_cast<unsigned long long>(s.maxFileMB) << 20) - static_cast<double>(bytes);
                const int bySize = static_cast<int>(std::max(0.0, room) / rate);
                left = left < 0 ? bySize : std::min(left, bySize);
            }
            if (left > s.warnBefore) left = -1;
        }
        const bool warn = left >= 0;
        const float u = ApexUi::Unit();
        const float scale = static_cast<float>(std::clamp(s.timerSize, 70, 160)) / 100.0f;
        const float alpha = static_cast<float>(std::clamp(s.timerOpacity, 30, 100)) / 100.0f;
        const auto* vp = ImGui::GetMainViewport();
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        const unsigned tint = warn ? VioletTheme::kWarning : VioletTheme::kError;
        if (s.redFrame) {
            const float t = 3.0f * u;
            dl->AddRect(ImVec2(vp->Pos.x + t * 0.5f, vp->Pos.y + t * 0.5f),
                        ImVec2(vp->Pos.x + vp->Size.x - t * 0.5f, vp->Pos.y + vp->Size.y - t * 0.5f),
                        ImGui::GetColorU32(VioletTheme::Col(tint, 0.85f * alpha)), 0.0f, 0, t);
        }
        if (!s.recordingDot) return true;
        const std::string text = Clock(sec, s.timerHours);
        std::string extra;
        if (s.timerFileSize && builtin) extra += std::format("  \xC2\xB7  {} MB", bytes >> 20);
        if (warn) extra += std::string("  \xC2\xB7  ") + I18n::Trf("{} left", Clock(left, false));
        else if (s.timerKey) extra += std::string("  \xC2\xB7  ") + I18n::Trf("{} stops", s.keyRecord);
        ImFont* font = ImGui::GetFont();
        const float fs = ImGui::GetFontSize() * scale;
        const ImVec2 t1 = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, text.c_str());
        const ImVec2 t2 = extra.empty() ? ImVec2(0, 0) : font->CalcTextSizeA(fs, FLT_MAX, 0.0f, extra.c_str());
        const float pad = 14.0f * u * scale, dotR = 5.0f * u * scale, gap = 10.0f * u * scale;
        const float h = std::max(40.0f * u * scale, fs + 16.0f * u * scale);
        const float w = pad + 2.0f * dotR + gap + t1.x + t2.x + pad;
        const float margin = (ApexUi::kSpace4 + ApexUi::kSpace1) * u;
        const std::string pos = Lower(Trim(s.timerPos));
        const bool bottom = pos.rfind("bottom", 0) == 0;
        const bool atLeft = pos.find("left") != std::string::npos, atRight = pos.find("right") != std::string::npos;
        float x = vp->Pos.x + (vp->Size.x - w) * 0.5f;
        if (atLeft) x = vp->Pos.x + margin;
        if (atRight) x = vp->Pos.x + vp->Size.x - margin - w;
        float y = bottom ? vp->Pos.y + vp->Size.y - margin - h : vp->Pos.y + margin;
        if (!bottom && !atLeft && !atRight && Captures::CurrentNote().visible) y += 44.0f * u + ApexUi::kSpace2 * u; // under an Apex note
        const ImVec2 p0(x, y), p1(x + w, y + h);
        dl->AddRectFilled(p0, p1, ImGui::GetColorU32(VioletTheme::Col(VioletTheme::kWindowBg, 0.92f * alpha)), h * 0.5f);
        dl->AddRect(p0, p1, ImGui::GetColorU32(VioletTheme::Col(tint, 0.35f * alpha)), h * 0.5f, 0, 1.0f * u);
        const float pulse = s.timerBlink ? 0.45f + 0.55f * std::abs(std::cos(static_cast<float>(GetTickCount64() % 2000) * 3.14159265f / 2000.0f)) : 1.0f;
        const float cy = y + h * 0.5f;
        dl->AddCircleFilled(ImVec2(x + pad + dotR, cy), dotR, ImGui::GetColorU32(VioletTheme::Col(tint, pulse * alpha)), 16);
        const float tx = x + pad + 2.0f * dotR + gap, ty = cy - t1.y * 0.5f;
        dl->AddText(font, fs, ImVec2(tx, ty), ImGui::GetColorU32(VioletTheme::Col(VioletTheme::kText, alpha)), text.c_str());
        if (!extra.empty())
            dl->AddText(font, fs, ImVec2(tx + t1.x, ty),
                        ImGui::GetColorU32(VioletTheme::Col(warn ? VioletTheme::kWarning : VioletTheme::kTextMuted, alpha)), extra.c_str());
        return true;
    } catch (...) {
        return false;
    }
}

bool NeedsGradeBeforeOverlay() { return VideoMp4::Active() && !GrabAtPresent(Get()); }

// ---------------------------------------------------------------------------------------------- Settings > Menu rows
// Inside the "Screenshot capture" card, after its folder and "Hide game UI" rows (which stay [ui]'s and are synced)
void ScreenshotRows() {
    const Settings def;
    Settings s = Get();
    bool ch = false;
    ch |= TextRow("File name", "Date, time and look name; hover the box for the tokens", s.shotNaming, 300.0f, true);
    static const char* const kFormat[] = {"PNG", "JPG"};
    int fmt = s.jpg ? 1 : 0;
    if (Pick("File type", "PNG keeps every detail; JPG files are smaller", "##CapFormat", fmt, kFormat, 2, def.jpg ? 1 : 0)) { s.jpg = fmt == 1; ch = true; }
    if (s.jpg) ch |= IntSlider("JPG quality", "Higher keeps more detail and makes larger files", s.jpegQuality, 1, 100, def.jpegQuality, "%.0f%%");
    ch |= IntSlider("Picture size", "Of the game's screen size", s.shotScale, 10, 100, def.shotScale, "%.0f%%");
    { // [ui] screenshot_hide_game_ui (synced with IncludeGameUI); shown here, under Picture size
        ApexConfig::UiSettings ui = ApexConfig::GetUi();
        if (ApexUi::SwitchRow("Hide game UI in screenshots", &ui.screenshotHideGameUi,
                              "Hides the game interface for the photo, then restores it; turn off to include it", true)) ApexConfig::SetUi(ui);
    }
    ch |= ApexUi::SwitchRow("Include the Apex menu", &s.shotApexMenu, "When the menu is open, it is in the picture", def.shotApexMenu);
    ch |= ApexUi::SwitchRow("Save a copy without ReShade/GShade", &s.saveBefore, "The same moment without ReShade/GShade, ending in _before", def.saveBefore);
    ch |= ApexUi::SwitchRow("Save the ReShade/GShade preset with it", &s.savePreset, "A copy of the ReShade/GShade preset file next to the picture", def.savePreset);
    ch |= IntSlider("Pictures per key press", "More than one takes a quick series", s.batchQuantity, 1, 50, def.batchQuantity);
    if (s.batchQuantity > 1) ch |= IntSlider("Time between pictures", "In milliseconds", s.batchInterval, 50, 5000, def.batchInterval, "%.0f ms");
    ch |= ApexUi::SwitchRow("Include the mouse pointer", &s.shotMouse, "Where it was on screen, at the picture's size", def.shotMouse);
    {
        static const char* const kSound[] = {"Off", "Camera", "GShade", "Own file"};
        const std::string low = Lower(Trim(s.soundPath));
        int mode = low.empty() ? 0 : low == "camera" ? 1 : low == "gshade" ? 2 : 3;
        static int ownMode = -1; // "Own file" chosen with no file typed yet
        if (ownMode == 3 && mode == 0) mode = 3;
        if (Pick("Shutter sound", "Plays with each picture and when a video starts and stops", "##CapSound", mode, kSound, 4, 0)) {
            ownMode = mode;
            if (mode != 3) {
                s.soundPath = mode == 0 ? "" : mode == 1 ? "camera" : "gshade";
                ch = true;
                Set(s);
                PlaySoundFile(); // hear the choice
            }
        }
        if (mode == 3) {
            std::string file = low == "camera" || low == "gshade" ? std::string() : s.soundPath;
            if (TextRow("Sound file", "Full path of a .wav file", file)) { s.soundPath = file; ch = true; Set(s); PlaySoundFile(); }
        }
        if (mode == 2 && GShadeSound().empty()) ApexUi::IconNote(IconId::Info, I18n::Tr("GShade is not installed here; the camera sound plays instead"));
    }
    if (ch) Set(s);
}

// The "Video recording" card's rows
void VideoRows() {
    const Settings def;
    Settings s = Get();
    bool ch = false;
    {
        const bool rec = Recording();
        const char* button = rec ? "Stop recording" : "Start recording";
        const float w = ApexUi::ButtonWidth(button, true);
        const int sec = RecordingSeconds();
        const std::string state = rec ? I18n::Trf("Recording: {}:{:02}", sec / 60, sec % 60)
                                : VideoMp4::Finishing() ? std::string(I18n::Tr("Saving the last video"))
                                                        : std::string(I18n::Tr("Or press the key set on the Shortcuts tab"));
        if (ApexUi::BeginControlRow("Record now", state.c_str(), w)) {
            ImGui::BeginDisabled(!rec && (!s.videoEnabled || VideoMp4::Finishing()));
            if (ApexUi::IconTextButton(button, rec ? IconId::X : IconId::Video)) ToggleRecording();
            ImGui::EndDisabled();
            ApexUi::EndControlRow();
        }
    }
    ch |= ApexUi::SwitchRow("Allow video recording", &s.videoEnabled, "Turn off to make the record key do nothing", def.videoEnabled);
    static const char* const kEngine[] = {"Apex", "Another app"};
    int eng = s.external ? 1 : 0;
    if (Pick("Recorder", "Another app: Apex presses its key (Xbox Game Bar, OBS)", "##CapEngine", eng, kEngine, 2, def.external ? 1 : 0)) { s.external = eng == 1; ch = true; }
    if (s.external) {
        // its key ("The other app's key", Win+ allowed) is on Settings > Shortcuts > Capture profiles
        ApexUi::MutedText("The other app's key is set on the Shortcuts tab");
        if (ch) Set(s);
        return;
    }
    ch |= FolderRow("Videos folder", "VideoFolder", s.videoPath, 1, "Choose another folder for the videos");
    ch |= TextRow("File name##Video", "Date, time and look name; hover the box for the tokens", s.videoNaming, 300.0f, true);
    static const char* const kSize[] = {"720p", "1080p", "1440p"};
    static const int kSizeV[] = {720, 1080, 1440};
    int si = s.maxHeight >= 1300 ? 2 : s.maxHeight >= 900 ? 1 : 0;
    if (Pick("Video size", "Larger looks sharper and costs more speed", "##CapSize", si, kSize, 3, 1)) { s.maxHeight = kSizeV[si]; ch = true; }
    static const char* const kFps[] = {"24", "30", "60"};
    static const int kFpsV[] = {24, 30, 60};
    int fi = s.fps >= 50 ? 2 : s.fps >= 27 ? 1 : 0;
    if (Pick("Frames per second", "60 is smoothest and costs the most speed", "##CapFps", fi, kFps, 3, 1)) { s.fps = kFpsV[fi]; ch = true; }
    static const char* const kRate[] = {"By quality", "Fixed size"};
    int rc = s.bitrateMode ? 1 : 0;
    if (Pick("Quality mode", "Fixed size keeps every minute about the same size", "##CapRate", rc, kRate, 2, def.bitrateMode ? 1 : 0)) { s.bitrateMode = rc == 1; ch = true; }
    if (s.bitrateMode) ch |= IntSlider("Megabits per second", "Higher keeps more detail and makes larger files", s.bitrateMbps, 1, 100, def.bitrateMbps);
    else ch |= IntSlider("Video quality", "Higher keeps more detail and makes larger files", s.quality, 1, 100, def.quality, "%.0f%%");
    static const char* const kCont[] = {"MP4", "Crash-safe MP4", "MKV"};
    int cont = s.mkv ? 2 : s.fragmented ? 1 : 0;
    if (Pick("File type##Video", "Crash-safe keeps the video if the game crashes; MKV is made from it when the video stops", "##CapCont", cont, kCont, 3, def.fragmented ? 1 : 0)) {
        s.fragmented = cont >= 1;
        s.mkv = cont == 2;
        ch = true;
    }
    if (s.mkv) ch |= ApexUi::SwitchRow("Keep the MP4 too", &s.keepMp4, "Saves the same video as MP4 next to the MKV", def.keepMp4);
    {
        static const int kClipMB[] = {0, 500, 1024, 2048, 4096, 8192};
        static const char* const kClip[] = {"Off", "500 MB", "1 GB", "2 GB", "4 GB", "8 GB"};
        int clip = -1;
        for (int k = 0; k < 6; k++) if (s.newClipMB == kClipMB[k]) clip = k;
        int pick = clip < 0 ? 0 : clip;
        if (Pick("New clip every", "Saves the clip at this size and records on in a new file", "##CapClip", pick, kClip, 6, 0) || (clip < 0 && pick != 0)) {
            s.newClipMB = kClipMB[pick];
            ch = true;
        }
        if (clip < 0) ApexUi::IconNote(IconId::Info, I18n::Trf("Set in the settings file: {} MB", s.newClipMB).c_str());
    }
    static const char* const kCodec[] = {"H.264", "HEVC"};
    int codec = s.hevc ? 1 : 0;
    if (Pick("Compression", "HEVC files are smaller; it needs a newer graphics card", "##CapCodec", codec, kCodec, 2, def.hevc ? 1 : 0)) { s.hevc = codec == 1; ch = true; }
    ch |= ApexUi::SwitchRow("Record the game's sound", &s.audio, "Only The Sims 3, not music or chat apps", def.audio);
    bool hideUi = !s.videoGameUi;
    if (ApexUi::SwitchRow("Hide game UI while recording", &hideUi, "Hides the game interface until the video stops", !def.videoGameUi)) { s.videoGameUi = !hideUi; ch = true; }
    ch |= ApexUi::SwitchRow("Include the Apex menu##Video", &s.videoApexMenu, "When the menu is open, it is in the video", def.videoApexMenu);
    ch |= ApexUi::SwitchRow("Include the mouse pointer##Video", &s.videoMouse, "The pointer moves in the video as on screen", def.videoMouse);
    ch |= ApexUi::SwitchRow("Show a recording timer", &s.recordingDot, "Time and file size on screen while recording", def.recordingDot);
    if (s.recordingDot) {
        static const char* const kPos[] = {"Top left", "Top centre", "Top right", "Bottom left", "Bottom right"};
        static const char* const kPosKey[] = {"top-left", "top-center", "top-right", "bottom-left", "bottom-right"};
        int pos = 1;
        for (int i = 0; i < 5; ++i) if (Lower(Trim(s.timerPos)) == kPosKey[i]) pos = i;
        if (Pick("Timer position", "Where the timer sits on the screen", "##CapTimerPos", pos, kPos, 5, 1)) { s.timerPos = kPosKey[pos]; ch = true; }
        ch |= IntSlider("Timer size", "100 is the size of Apex notes", s.timerSize, 70, 160, def.timerSize, "%.0f%%");
        ch |= IntSlider("Timer opacity", "Lower lets more of the game show through", s.timerOpacity, 30, 100, def.timerOpacity, "%.0f%%");
        ch |= ApexUi::SwitchRow("Pulse the red dot", &s.timerBlink, "The dot fades in and out while recording", def.timerBlink);
        ch |= ApexUi::SwitchRow("Show the file size", &s.timerFileSize, "How large the video is so far", def.timerFileSize);
        ch |= ApexUi::SwitchRow("Show the stop key", &s.timerKey, "The key that stops the video", def.timerKey);
        ch |= ApexUi::SwitchRow("Always show hours", &s.timerHours, "0:02:15 instead of 02:15", def.timerHours);
    }
    ch |= ApexUi::SwitchRow("Red frame while recording", &s.redFrame, "A thin red border around the screen", def.redFrame);
    if (s.recordingDot || s.redFrame)
        ch |= ApexUi::SwitchRow("Timer in the video too", &s.timerInVideo, "An open Apex menu is then in the video too", def.timerInVideo);
    ch |= IntSlider("Stop after", "0 records until you stop it", s.maxLength, 0, 3600, def.maxLength, "%.0f s");
    ch |= IntSlider("Stop at file size", "0 has no limit", s.maxFileMB, 0, 16000, def.maxFileMB, "%.0f MB");
    ch |= IntSlider("Warn before the limit", "The timer turns amber and shows the time left; 0 is off", s.warnBefore, 0, 600, def.warnBefore, "%.0f s");
    ch |= ApexUi::SwitchRow("Short note when saved", &s.savedNote, "Length and size of the saved video", def.savedNote);
    ApexUi::IconNote(IconId::Info, I18n::Trf("About {} MB per minute; if the game stutters, lower the video size or frames per second",
                                             static_cast<int>(KbpsFor(s) * 60.0 / 8.0 / 1024.0 + 0.5)).c_str());
    if (ch) Set(s);
}

// The settings file group at the end of the "Video recording" card
void FileRows() {
    const Settings def;
    Settings s = Get();
    bool ch = false;
    const std::wstring file = File().wstring();
    const std::string fileUtf8 = ApexUtil::ToUtf8(file);
    {
        const float w = ApexUi::ButtonWidth("Open in Notepad", true) + ApexUi::kSpace3 * ApexUi::Unit() + ApexUi::ButtonWidth("Read again", true);
        if (ApexUi::BeginControlRow("Settings file", "Screenshots and videos follow ApexCapture.ini; you can edit it yourself", w)) {
            if (ApexUi::IconTextButton("Open in Notepad", IconId::ExternalLink, fileUtf8.c_str()))
                ShellExecuteW(nullptr, L"open", L"notepad.exe", (L"\"" + file + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
            ImGui::SameLine(0.0f, ApexUi::kSpace3 * ApexUi::Unit());
            if (ApexUi::IconTextButton("Read again", IconId::RotateCcw, "Reads the file now")) {
                std::lock_guard<std::mutex> lk(g_lock);
                LoadLocked();
                s = g_s;
            }
            ApexUi::EndControlRow();
        }
    }
    ch |= ApexUi::SwitchRow("Read changes automatically", &s.reloadOnChange, "Edit the file while the game runs", def.reloadOnChange);
    ch |= ApexUi::SwitchRow("Show a note when saved", &s.showNotice, "A short note when a picture or video is saved", def.showNotice);
    {
        std::lock_guard<std::mutex> lk(g_lock);
        if (!g_lastError.empty()) ApexUi::IconNote(IconId::TriangleAlert, g_lastError.c_str(), VioletTheme::kWarning);
    }
    if (ch) Set(s);
}

// ---------------------------------------------------------------------------------------------- the feature
namespace {
class ApexCapturePatch final : public ApexPatch {
  public:
    ApexCapturePatch() : ApexPatch("ApexCapture", nullptr) {}
    bool Install() override {
        if (isEnabled) return true;
        Get(); // reads (or writes) the file now
        RenderCallbacks::Add(RenderCallbacks::filteredSceneBeforeOverlay, BeforeOverlay);
        RenderCallbacks::Add(RenderCallbacks::preReset, OnPreReset);
        D3D9Hooks::RegisterPresent("ApexCapture", [](D3D9Hooks::DeviceContext& ctx, const RECT*, const RECT*, HWND, const RGNDATA*) {
            OnPresent(ctx.device);
            return D3D9Hooks::HookAction::Continue;
        }, D3D9Hooks::Priority::Early);
        isEnabled = true;
        LOG_INFO("[Capture] Installed; settings: " + ApexUtil::ToUtf8(File().wstring()));
        return true;
    }
    bool Uninstall() override {
        if (!isEnabled) return true;
        if (Recording()) StopRecording();
        RenderCallbacks::Remove(RenderCallbacks::filteredSceneBeforeOverlay, BeforeOverlay);
        RenderCallbacks::Remove(RenderCallbacks::preReset, OnPreReset);
        D3D9Hooks::UnregisterAll("ApexCapture");
        isEnabled = false;
        return true;
    }
    // its rows are on Settings > Menu (ScreenshotRows, VideoRows), not on a feature card
};
} // namespace

// ---------------------------------------------------------------------------------------------- capture profiles (public)
std::vector<std::string> Profiles() {
    std::lock_guard<std::mutex> lk(g_lock);
    EnsureLoaded();
    std::vector<std::string> names;
    for (const Profile& p : g_profiles) names.push_back(p.name);
    return names;
}

bool SwitchProfile(const std::string& name) {
    bool ok = false, note = false;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        EnsureLoaded();
        if (g_dirty) { g_dirty = false; WriteLocked(); }
        ok = SwitchProfileLocked(name);
        note = g_s.profileNote;
    }
    if (ok) {
        SyncUi();
        if (note) Note(I18n::Trf("Capture profile: {}", CleanName(name)), 2, Captures::NoteKind::Info);
    }
    return ok;
}

namespace {
// What a profile changes, as short chips: UI, pointer, sound, picture type, video type
std::vector<std::pair<const char*, bool>> ProfileChips(const Settings& p) {
    const std::string snd = Lower(Trim(p.soundPath));
    return {{p.shotGameUi ? "UI" : "No UI", p.shotGameUi},
            {p.shotMouse ? "Pointer" : "No pointer", p.shotMouse},
            {snd.empty() ? "Silent" : snd == "camera" ? "Camera" : snd == "gshade" ? "GShade" : "Own sound", !snd.empty()},
            {p.jpg ? "JPG" : "PNG", false},
            {p.mkv ? "MKV" : p.fragmented ? "Crash-safe MP4" : "MP4", false}};
}

// One profile row: a dot, the name, the chips and its key. true = clicked.
bool ProfileRow(const std::string& name, const Settings& p, bool active) {
    const float u = ApexUi::Unit();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x, h = 34.0f * u;
    ImGui::PushID(name.c_str());
    const bool clicked = ImGui::InvisibleButton("##profile", ImVec2(w, h));
    const bool hover = ImGui::IsItemHovered();
    ImGui::PopID();
    const ImVec2 p1(p0.x + w, p0.y + h);
    dl->AddRectFilled(p0, p1, ImGui::GetColorU32(VioletTheme::Col(active ? VioletTheme::kAccentDark : VioletTheme::kSelectedBg, active ? 0.28f : hover ? 1.0f : 0.6f)), 8.0f * u);
    if (active) dl->AddRect(p0, p1, ImGui::GetColorU32(VioletTheme::Col(VioletTheme::kAccent, 0.9f)), 8.0f * u, 0, 1.0f * u);
    const float cy = p0.y + h * 0.5f;
    const ImVec2 dot(p0.x + 14.0f * u, cy);
    if (active) dl->AddCircleFilled(dot, 4.5f * u, ImGui::GetColorU32(VioletTheme::Col(VioletTheme::kAccent)), 16);
    else dl->AddCircle(dot, 4.5f * u, ImGui::GetColorU32(VioletTheme::Col(VioletTheme::kTextMuted)), 16, 1.5f * u);
    const float fs = ImGui::GetFontSize();
    ImFont* font = ImGui::GetFont();
    float x = p0.x + 28.0f * u;
    const float nameW = 150.0f * u;
    ImGui::PushClipRect(ImVec2(x, p0.y), ImVec2(x + nameW - 6.0f * u, p1.y), true);
    dl->AddText(ImVec2(x, cy - fs * 0.5f), ImGui::GetColorU32(VioletTheme::Col(VioletTheme::kText)), name.c_str());
    ImGui::PopClipRect();
    x += nameW;
    // key on the right
    const std::string key = Trim(p.profileKey);
    const char* keyText = key.empty() ? I18n::Tr("No key") : key.c_str();
    const float kw = font->CalcTextSizeA(fs * 0.9f, FLT_MAX, 0.0f, keyText).x + 14.0f * u;
    const float kx = p1.x - 10.0f * u - kw;
    dl->AddRect(ImVec2(kx, cy - 10.0f * u), ImVec2(kx + kw, cy + 10.0f * u), ImGui::GetColorU32(VioletTheme::Col(VioletTheme::kTextMuted, 0.4f)), 5.0f * u);
    dl->AddText(font, fs * 0.9f, ImVec2(kx + 7.0f * u, cy - fs * 0.45f), ImGui::GetColorU32(VioletTheme::Col(VioletTheme::kTextMuted)), keyText);
    // chips, as many as fit
    for (const auto& [label, on] : ProfileChips(p)) {
        const char* t = I18n::Tr(label);
        const float cw = font->CalcTextSizeA(fs * 0.85f, FLT_MAX, 0.0f, t).x + 12.0f * u;
        if (x + cw > kx - 8.0f * u) break;
        const unsigned rgb = on ? VioletTheme::kAccentLight : VioletTheme::kTextMuted;
        dl->AddRectFilled(ImVec2(x, cy - 9.0f * u), ImVec2(x + cw, cy + 9.0f * u), ImGui::GetColorU32(VioletTheme::Col(on ? VioletTheme::kAccent : VioletTheme::kTextMuted, on ? 0.22f : 0.12f)), 9.0f * u);
        dl->AddText(font, fs * 0.85f, ImVec2(x + 6.0f * u, cy - fs * 0.425f), ImGui::GetColorU32(VioletTheme::Col(rgb)), t);
        x += cw + 4.0f * u;
    }
    if (hover && !active) ImGui::SetTooltip("%s", I18n::Tr("Click to use this profile"));
    ImGui::Dummy(ImVec2(0.0f, 4.0f * u));
    return clicked;
}
} // namespace

void ProfileCard() {
    if (ApexUi::BeginCard("##CaptureProfiles")) {
        ApexUi::CardHeader(IconId::Settings2, "Capture profiles", "Every screenshot and video setting below belongs to the profile in use", nullptr, nullptr);
        ApexUi::CardDivider();
        ApexUi::IconNote(IconId::Info, I18n::Tr("Keys for capture are on the Shortcuts tab"), VioletTheme::kAccent);
        Settings s;
        std::vector<Profile> list;
        {
            std::lock_guard<std::mutex> lk(g_lock);
            EnsureLoaded();
            s = g_s;
            list = g_profiles;
        }
        std::string pick;
        for (const Profile& p : list) {
            const bool active = _stricmp(p.name.c_str(), s.profileActive.c_str()) == 0;
            if (ProfileRow(p.name, active ? s : p.s, active) && !active) pick = p.name;
        }
        if (!pick.empty()) SwitchProfile(pick);

        const float u = ApexUi::Unit();
        const float bw = ApexUi::ButtonWidth("New profile", true) + ApexUi::ButtonWidth("Delete##Profile", true) + ApexUi::kSpace3 * u;
        if (ApexUi::BeginControlRow("Profiles", "New copies the profile in use; Delete removes it", bw)) {
            if (ApexUi::IconTextButton("New profile", IconId::Bookmark, "A copy of the profile in use, under a new name")) {
                std::string name;
                for (int n = static_cast<int>(list.size()) + 1; n < 1000; n++) {
                    name = std::format("Profile {}", n);
                    if (!std::filesystem::exists(ProfilePath(name))) break;
                }
                {
                    std::lock_guard<std::mutex> lk(g_lock);
                    Settings copy = g_s;
                    copy.profileActive = name;
                    copy.profileKey.clear();
                    copy.profileShotKey.clear();
                    WriteText(ProfilePath(name), Render(copy, true));
                    ScanProfilesLocked();
                }
                SwitchProfile(name);
            }
            ImGui::SameLine(0.0f, ApexUi::kSpace3 * u);
            ImGui::BeginDisabled(list.size() < 2);
            if (ApexUi::IconTextButton("Delete##Profile", IconId::Trash2, "Deletes the profile in use and switches to the next one")) {
                const std::string gone = s.profileActive;
                std::string next;
                for (const Profile& p : list) if (_stricmp(p.name.c_str(), gone.c_str()) != 0) { next = p.name; break; }
                if (!next.empty() && SwitchProfile(next)) {
                    std::error_code ec;
                    std::filesystem::remove(ProfilePath(gone), ec);
                    std::lock_guard<std::mutex> lk(g_lock);
                    ScanProfilesLocked();
                    LOG_INFO("[Capture] Capture profile deleted: " + gone);
                }
            }
            ImGui::EndDisabled();
            ApexUi::EndControlRow();
        }
        // the profile in use: its name and keys
        std::string name = s.profileActive;
        if (TextRow("Profile name", "The name of the profile in use", name, 200.0f)) {
            const std::string clean = CleanName(name);
            if (_stricmp(clean.c_str(), s.profileActive.c_str()) != 0 && !std::filesystem::exists(ProfilePath(clean))) {
                std::lock_guard<std::mutex> lk(g_lock);
                std::error_code ec;
                std::filesystem::remove(ProfilePath(g_s.profileActive), ec);
                g_s.profileActive = clean;
                WriteLocked();
                ScanProfilesLocked();
            }
        }
        // the keys (this profile's, one picture, next profile, the other app's) are key chips on Settings > Shortcuts
        if (ApexUi::SwitchRow("Show the name when switching", &s.profileNote, "A short note on screen, never in pictures or videos", true)) {
            Settings cur = Get();
            cur.profileNote = s.profileNote;
            Set(cur);
        }
    }
    ApexUi::EndCard();
}

} // namespace ApexCapture

using ApexCapture::ApexCapturePatch;
APEX_REGISTER_FEATURE(ApexCapturePatch, { .displayName = "Screenshots & video",
                                                       .description = "Screenshots and MP4 videos with Apex's effects, saved the way ApexCapture.ini says.",
                                                       .category = "Graphics",
                                                       .experimental = true,
                                                       .enabledByDefault = true,
                                                       .supportedVersions = VERSION_ALL,
                                                       .technicalDetails = { "Settings: Documents\\...\\Apex Radiance\\ApexCapture.ini (re-read when saved).",
                                                                             "Video: Media Foundation H.264/HEVC + game sound (process loopback)." } })
