// Screenshots and videos with Apex's effects, and their [capture] settings in ApexRadiance.toml (see apex_capture.h).
#include "apex_capture.h"
#include "looks.h"
#include "captures.h"
#include "video_mp4.h"
#include "apex_log.h"
#include "apex_paths.h"
#include "apex_util.h"
#include "d3d9_hooks.h"
#include "render_callbacks.h"
#include "imgui.h"
#include "ui/widgets.h"
#include "ui/i18n.h"
#include "ui/violet_theme.h"
#include "hook_guard.h"
#include <toml++/toml.hpp>
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <mmsystem.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <format>
#include <map>
#include <mutex>
#include <deque>
#include <optional>

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "shell32.lib")


namespace ApexCapture {
using ApexUi::IconId;
namespace {

std::string Lower(std::string s) { for (auto& c : s) c = static_cast<char>(tolower(static_cast<unsigned char>(c))); return s; }
std::wstring LowerW(std::wstring s) { for (auto& c : s) c = static_cast<wchar_t>(towlower(c)); return s; }
std::string Trim(const std::string& s) {
    const size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

// ---------------------------------------------------------------------------------------------- [capture] in the TOML
const char* const kLookNames[] = {"with", "without", "both"};
const char* const kFolderNames[] = {"game", "apex", "custom"};
const char* const kQualityNames[] = {"smaller", "balanced", "best", "custom"};
const char* const kContainerNames[] = {"mp4", "mp4-safe", "mkv"};
const char* const kPosNames[] = {"top-left", "top-center", "top-right", "bottom-left", "bottom-right"};

template <size_t N> int NameIndex(const toml::node_view<const toml::node>& n, const char* const (&names)[N], int def) {
    const std::string v = Lower(Trim(n.value_or(std::string())));
    for (size_t i = 0; i < N; i++) if (v == names[i]) return static_cast<int>(i);
    return def;
}
int Int(const toml::node_view<const toml::node>& n, int def) { return static_cast<int>(n.value_or(static_cast<int64_t>(def))); }

Settings Clamped(Settings s) {
    s.sound = Trim(s.sound);
    if (Lower(s.sound) == "off") s.sound.clear();
    ScreenshotSettings& t = s.shot;
    if (Trim(t.fileName).empty()) t.fileName = kDefaultShotName;
    t.look = std::clamp(t.look, 0, 2);
    t.jpgQuality = std::clamp(t.jpgQuality, 1, 100);
    t.sizePercent = std::clamp(t.sizePercent, 10, 100);
    t.burstCount = std::clamp(t.burstCount, 1, kMaxBurst);
    t.burstIntervalMs = std::clamp(t.burstIntervalMs, 50, 10000);
    if (t.burstCount > 1) t.burstIntervalMs = std::min(t.burstIntervalMs, kMaxBurstMs / (t.burstCount - 1)); // the whole burst within 30 s
    VideoSettings& v = s.video;
    const VideoSettings def;
    if (Trim(v.fileName).empty()) v.fileName = def.fileName;
    v.folder = std::clamp(v.folder, 0, 2);
    v.look = std::clamp(v.look, 0, 1);
    v.size = v.size >= 1800 ? 2160 : v.size >= 1300 ? 1440 : v.size >= 900 ? 1080 : 720;
    v.fps = v.fps >= 50 ? 60 : v.fps >= 27 ? 30 : 24;
    v.quality = std::clamp(v.quality, 0, 3);
    v.customMbps = std::clamp(v.customMbps, 1, 200);
    v.container = std::clamp(v.container, 0, 2);
    v.newClipMb = std::clamp(v.newClipMb, 0, 100000);
    v.stopAfterS = std::clamp(v.stopAfterS, 0, 36000);
    v.stopAtMb = std::clamp(v.stopAtMb, 0, 100000);
    v.warnBeforeS = std::clamp(v.warnBeforeS, 0, 600);
    v.timerPosition = std::clamp(v.timerPosition, 0, 4);
    v.timerSize = std::clamp(v.timerSize, 70, 160);
    v.timerOpacity = std::clamp(v.timerOpacity, 30, 100);
    v.externalKey = Trim(v.externalKey);
    if (!v.recordKey.vk) v.recordKey = def.recordKey;
    return s;
}

std::mutex g_lock;
Settings g_s;
std::atomic<unsigned> g_sVersion{1}; // changes with g_s (under g_lock): VideoNow copies the settings again

// %Counter%: the next number, then one more (saved with the settings)
int NextCounter() {
    int n = 1;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        n = g_s.counter;
        g_s.counter = n >= 99999999 ? 1 : n + 1;
    }
    ApexConfig::RequestSave();
    return n;
}
bool g_loaded = false;

void RefreshSaveName();
bool WantsSaveName(const Settings& s) {
    auto has = [](const std::string& t) { return Lower(t).find("%savename%") != std::string::npos; };
    return has(s.shot.fileName) || has(s.video.fileName) || has(s.video.customFolder) || has(ApexConfig::GetUi().screenshotCustomFolder);
}

void EnsureLoadedLocked() {
    if (g_loaded) return;
    g_loaded = true;
    toml::table root;
    ApexConfig::ReadRoot(root);
    g_s = FromToml(root);
    ++g_sVersion;
}

// The video settings for the render thread's reads at every frame while recording (Present, the timer): copied again only
// when they changed, so a frame does not copy their strings
const VideoSettings& VideoNow() {
    static VideoSettings v;
    static unsigned seen = 0;
    if (g_sVersion.load() != seen) {
        std::lock_guard<std::mutex> lk(g_lock);
        EnsureLoadedLocked();
        v = g_s.video;
        seen = g_sVersion.load();
    }
    return v;
}

ApexConfig::KeyChord RecordKey() {
    std::lock_guard<std::mutex> lk(g_lock);
    EnsureLoadedLocked();
    return g_s.video.recordKey;
}

// ---------------------------------------------------------------------------------------------- paths and names
std::wstring KnownFolder(int csidl) {
    wchar_t buf[MAX_PATH]{};
    return SUCCEEDED(SHGetFolderPathW(nullptr, csidl, nullptr, SHGFP_TYPE_CURRENT, buf)) ? std::wstring(buf) : std::wstring();
}

bool HasToken(const std::wstring& s, const std::wstring& token) {
    std::wstring lower = s, lt = token;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::towlower);
    std::transform(lt.begin(), lt.end(), lt.begin(), ::towlower);
    return lower.find(lt) != std::wstring::npos;
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

std::wstring SafeName(std::wstring s) {
    for (auto& c : s) if (wcschr(L"\\/:*?\"<>|", c) || c < 32) c = L'_';
    return s;
}

// ---- %SaveName%: the most recently written Saves\*.sims3 folder, found on a worker; the render thread reads the cache
std::mutex g_saveNameLock;
std::wstring g_saveName;
unsigned long long g_saveNameAt = 0;
std::atomic<bool> g_saveNameBusy{false};

std::wstring NewestSaveName() {
    std::wstring best;
    std::error_code ec;
    std::filesystem::file_time_type bestTime{};
    for (const auto& e : std::filesystem::directory_iterator(std::filesystem::path(StripSlash(ApexPaths::GameDocumentsDirectory())) / L"Saves", ec)) {
        const auto& p = e.path();
        if (_wcsicmp(p.extension().c_str(), L".sims3") != 0 || !e.is_directory(ec)) continue;
        const auto t = e.last_write_time(ec);
        if (ec) continue;
        if (best.empty() || t > bestTime) { bestTime = t; best = p.stem().wstring(); }
    }
    return best;
}

void RefreshSaveName() {
    if (g_saveNameBusy.exchange(true)) return;
    { std::lock_guard<std::mutex> lk(g_saveNameLock); g_saveNameAt = GetTickCount64(); }
    if (!HookGuard::StartDetached("Capture: save name", [] {
            struct Done { ~Done() { g_saveNameBusy = false; } } done;
            std::wstring n = NewestSaveName();
            std::lock_guard<std::mutex> lk(g_saveNameLock);
            g_saveName = std::move(n);
        }))
        g_saveNameBusy = false;
}

// shortcut: the newest save folder, not the one loaded; a loaded save counts once the game writes it
std::wstring CurrentSaveName() {
    std::wstring n;
    bool stale = false;
    {
        std::lock_guard<std::mutex> lk(g_saveNameLock);
        n = g_saveName;
        stale = GetTickCount64() - g_saveNameAt > 10000;
    }
    if (stale) RefreshSaveName();
    return n.empty() ? std::wstring(L"The Sims 3") : n;
}

// Date, time and save tokens, shared by folder paths and file names
std::wstring TimeTokens(std::wstring n, const SYSTEMTIME& t) {
    if (n.find(L'%') == std::wstring::npos) return n;
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
    if (HasToken(n, L"%MonthName%")) n = Replace(n, L"%MonthName%", dateText(L"MMMM"));
    if (HasToken(n, L"%MonthShort%")) n = Replace(n, L"%MonthShort%", dateText(L"MMM"));
    if (HasToken(n, L"%WeekdayShort%")) n = Replace(n, L"%WeekdayShort%", dateText(L"ddd"));
    if (HasToken(n, L"%Weekday%")) n = Replace(n, L"%Weekday%", dateText(L"dddd"));
    if (HasToken(n, L"%SaveName%")) n = Replace(n, L"%SaveName%", SafeName(CurrentSaveName()));
    return n;
}

// A typed folder (tokens allowed) -> the full path; not made
std::filesystem::path ExpandFolder(const std::string& path) {
    std::wstring p = ApexUtil::ToWide(Trim(path));
    if (p.empty()) return {};
    { SYSTEMTIME t{}; GetLocalTime(&t); p = TimeTokens(p, t); } // %Day%-%Month%-%Year%, %SaveName% ...: one folder per day or save
    p = Replace(p, L"%GameScreenshots%", StripSlash(ApexPaths::GameDocumentsDirectory()) + L"\\Screenshots");
    p = Replace(p, L"%ApexFolder%", StripSlash(ApexPaths::ApexDirectory()));
    if (HasToken(p, L"%Pictures%")) p = Replace(p, L"%Pictures%", KnownFolder(CSIDL_MYPICTURES));
    if (HasToken(p, L"%Videos%")) p = Replace(p, L"%Videos%", KnownFolder(CSIDL_MYVIDEO));
    if (HasToken(p, L"%Documents%")) p = Replace(p, L"%Documents%", KnownFolder(CSIDL_PERSONAL));
    wchar_t env[MAX_PATH * 2]{};
    if (ExpandEnvironmentStringsW(p.c_str(), env, MAX_PATH * 2)) p = env;
    std::filesystem::path f(p);
    if (f.is_relative()) f = std::filesystem::path(StripSlash(ApexPaths::GameDocumentsDirectory())) / f;
    return f;
}

// The last folder made per kind (0 screenshots, 1 videos): the same path again is only checked, not made again
std::mutex g_folderLock;
std::wstring g_folderMade[2];

bool MakeFolder(const std::filesystem::path& f, int slot) {
    if (f.empty()) return false;
    {
        std::lock_guard<std::mutex> lk(g_folderLock);
        if (g_folderMade[slot] == f.wstring()) {
            const DWORD a = GetFileAttributesW(f.c_str());
            if (a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY)) return true;
        }
    }
    std::error_code ec;
    std::filesystem::create_directories(f, ec);
    if (!std::filesystem::is_directory(f, ec)) return false;
    std::lock_guard<std::mutex> lk(g_folderLock);
    g_folderMade[slot] = f.wstring();
    return true;
}

// ---------------------------------------------------------------------------------------------- "before" picture
IDirect3DSurface9* g_beforeRt = nullptr;
UINT g_beforeW = 0, g_beforeH = 0;
bool g_wantBefore = false, g_beforeFresh = false, g_beforeLogged = false;
void ReleaseBefore() { if (g_beforeRt) { g_beforeRt->Release(); g_beforeRt = nullptr; } g_beforeFresh = false; }

// ---------------------------------------------------------------------------------------------- video control
std::atomic<bool> g_toggle{false};
std::atomic<bool> g_hooks{false};
std::atomic<bool> g_externalOn{false};
bool g_videoHidUi = false;
int g_clipPart = 1;                       // %Part%: the clip of the running recording (new_clip_mb)
double g_clipBaseSec = 0.0;               // seconds of the clips already saved in this recording
unsigned long long g_externalStart = 0;   // GetTickCount64 when the external recorder's key was sent
double g_lastSec = 0.0;                   // the running video's length and size, kept for the "saved" note
unsigned long long g_lastBytes = 0;
Settings g_rec;                           // render thread: the settings the running recording started with
bool g_lookSeen = false;                  // look = without: this frame was taken before the look (BeforeLook)
bool g_withoutFailed = false;             // ... and that copy failed once: the rest of the recording keeps the look
int g_bbH = 0, g_bbW = 0;                 // the back buffer, for %Width% %Height%

// The video is read at Present (after Apex draws) when the menu or the timer has to be in it
bool GrabAtPresent(const VideoSettings& v) { return v.includeMenu || (v.timerInVideo && (v.timer || v.redFrame)); }
std::string Clock(int sec, bool hours) {
    sec = std::max(0, sec);
    if (hours || sec >= 3600) return std::format("{}:{:02}:{:02}", sec / 3600, sec / 60 % 60, sec % 60);
    return std::format("{:02}:{:02}", sec / 60, sec % 60);
}

bool GameForeground() {
    DWORD pid = 0;
    const HWND fg = GetForegroundWindow();
    return fg && GetWindowThreadProcessId(fg, &pid) && pid == GetCurrentProcessId();
}

// The other recorder's chord, only into the game's own foreground window and only an allowed chord
bool SendExternalKey(const std::string& text) {
    UINT vk = 0; bool ctrl = false, shift = false, alt = false, win = false;
    if (!ExternalKeyAllowed(text) || !ParseExternalKey(text, vk, ctrl, shift, alt, win)) {
        LOG_WARNING("[Video] The other app's key is not allowed: " + text);
        return false;
    }
    if (!GameForeground()) {
        LOG_WARNING("[Video] The other app's key was not sent: the game is not the foreground window");
        return false;
    }
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
    return SendInput(static_cast<UINT>(in.size()), in.data(), sizeof(INPUT)) == in.size();
}

// The quality presets are Quality 60 / 80 / 95 of the rate below; custom is a fixed rate
int KbpsFor(const VideoSettings& v) {
    if (v.quality == kQualityCustom) return v.customMbps * 1000;
    static const int kQuality[] = {60, 80, 95};
    const int q = kQuality[std::clamp(v.quality, 0, 2)];
    const double base = v.size <= 720 ? 8000.0 : v.size <= 1080 ? 16000.0 : v.size <= 1440 ? 28000.0 : 52000.0; // kbit/s at 30 fps, Quality 80
    const double fps = v.fps >= 50 ? 1.6 : v.fps / 30.0;
    return std::clamp(static_cast<int>(base * fps * q / 80.0), 1000, 200000);
}

void Note(const char* text, int seconds, Captures::NoteKind kind) {
    if (kind == Captures::NoteKind::Warning || Get().notes) Captures::Notify(text, seconds, kind);
}
void Note(const std::string& text, int seconds, Captures::NoteKind kind) { Note(text.c_str(), seconds, kind); }

void CopyPresetNextTo(const std::filesystem::path& file) {
    std::wstring preset = PresetFile();
    if (preset.empty()) return;
    std::filesystem::path ini = file;
    ini.replace_extension(L".ini");
    HookGuard::StartDetached("Capture: preset copy", [preset = std::move(preset), ini = std::move(ini)] {
        if (!CopyFileW(preset.c_str(), ini.c_str(), FALSE)) LOG_WARNING("[Video] The preset could not be copied: " + ApexUtil::ToUtf8(ini.wstring()));
    });
}

void StartRecording(IDirect3DDevice9* dev, bool nextClip = false) {
    if (!nextClip) {
        g_rec = Get();
        g_lookSeen = g_withoutFailed = false;
    }
    const VideoSettings& v = g_rec.video;
    if (v.external) {
        if (!SendExternalKey(v.externalKey)) {
            Note(I18n::Tr("The other app's key is not allowed or the game is not in front; choose a key with Ctrl, Alt, Shift or Win"), 6,
                 Captures::NoteKind::Warning);
            return;
        }
        g_externalOn = true;
        g_externalStart = GetTickCount64();
        PlaySoundFile();
        Note(I18n::Trf("The other app is recording ({})", v.externalKey), 3, Captures::NoteKind::Info);
        return;
    }
    FolderResult fr = FolderResult::Ok;
    const auto folder = CaptureFolder(true, fr);
    if (fr == FolderResult::FellBack && !nextClip)
        Note(I18n::Tr("The chosen videos folder is not available; saved in the game's videos folder"), 5, Captures::NoteKind::Warning);
    if (folder.empty()) { Note(I18n::Tr("Could not create the videos folder"), 5, Captures::NoteKind::Warning); return; }
    g_clipPart = nextClip ? g_clipPart + 1 : 1;
    const bool mkv = v.container == kContainerMkv;
    auto file = NewFile(folder, v.fileName, mkv ? "mkv" : "mp4", true);
    if (mkv) { // recorded as <name>.mp4 next to the free <name>.mkv
        file.replace_extension(L".mp4");
        std::error_code ec;
        for (unsigned k = 2; std::filesystem::exists(file, ec) && k < 1000; ++k) {
            file = NewFile(folder, v.fileName + std::format("_{}", k), "mkv", true);
            file.replace_extension(L".mp4");
        }
    }
    VideoMp4::Options o;
    o.fps = v.fps;
    o.maxHeight = v.size;
    o.hevc = v.hevc;
    o.bitrateKbps = KbpsFor(v);
    o.audio = v.gameSound;
    o.mouse = v.includePointer;
    o.fragmented = v.container != kContainerMp4;
    o.mkv = mkv;
    o.keepMp4 = v.keepMp4;
    o.nextClip = nextClip;
    std::string why;
    if (!VideoMp4::Start(dev, file, o, why)) {
        LOG_WARNING("[Video] Not started: " + why);
        Note(I18n::Tr("The video could not start; the reason is in the log"), 6, Captures::NoteKind::Warning);
        return;
    }
    if (v.savePreset) CopyPresetNextTo(file);
    if (nextClip) { LOG_INFO(std::format("[Video] New clip {} at the new_clip_mb size", g_clipPart)); return; } // UI, sound and timer carry on
    g_clipBaseSec = 0.0;
    g_videoHidUi = v.hideGameUi && Captures::HideGameUiForVideo(true);
    PlaySoundFile();
}

void StopRecording() {
    if (g_externalOn) {
        SendExternalKey(g_rec.video.externalKey);
        g_externalOn = false;
        PlaySoundFile();
        Note(I18n::Tr("The other app stopped recording; the video is in its own folder"), 4, Captures::NoteKind::Info);
        return;
    }
    if (!VideoMp4::Active()) return;
    VideoMp4::Stop();
    if (g_videoHidUi) Captures::HideGameUiForVideo(false); // only when the game's UI is still hidden (Captures tracks F10)
    g_videoHidUi = false;
    PlaySoundFile();
    Note(I18n::Tr("Saving the video..."), 4, Captures::NoteKind::Saving);
}

void OnPresent(IDirect3DDevice9* dev) {
    if (!g_bbH && dev) { IDirect3DSurface9* bb = nullptr; if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) { D3DSURFACE_DESC d{}; bb->GetDesc(&d); g_bbW = static_cast<int>(d.Width); g_bbH = static_cast<int>(d.Height); bb->Release(); } }
    if (g_toggle.exchange(false)) { if (Recording()) StopRecording(); else StartRecording(dev); }
    if (VideoMp4::Active()) {
        const VideoSettings& v = VideoNow(); // limits follow the menu while recording
        if (VideoMp4::Failed()) StopRecording();
        else if ((v.stopAfterS > 0 && g_clipBaseSec + VideoMp4::Seconds() >= v.stopAfterS) ||
                 (v.newClipMb <= 0 && v.stopAtMb > 0 && VideoMp4::BytesWritten() >= (static_cast<unsigned long long>(v.stopAtMb) << 20))) {
            LOG_INFO("[Video] Stopped at the stop_after_s / stop_at_mb limit");
            StopRecording();
        } else if (v.newClipMb > 0 && VideoMp4::BytesWritten() >= (static_cast<unsigned long long>(v.newClipMb) << 20)) {
            // the clip is saved and the recording goes on in a new file (a frame or two between them)
            g_clipBaseSec += VideoMp4::Seconds();
            VideoMp4::Stop();
            StartRecording(dev, true);
        } else {
            if (GrabAtPresent(g_rec.video) && !g_lookSeen) VideoMp4::Grab(dev); // the Apex menu / the timer is drawn by now
            g_lastSec = VideoMp4::Seconds();
            g_lastBytes = VideoMp4::BytesWritten();
        }
    }
    if (g_videoHidUi && !VideoMp4::Active()) { // ended without StopRecording (a new clip failed, the game closing was cancelled)
        Captures::HideGameUiForVideo(false);
        g_videoHidUi = false;
    }
    g_lookSeen = false;
    VideoMp4::Pump(dev);
    bool ok = false;
    std::filesystem::path saved;
    if (!VideoMp4::TakeResult(ok, &saved).empty()) {
        if (ok) SetLastSaved(true, saved);
        if (!ok) Note(I18n::Tr("The video could not be saved; the reason is in the log"), 8, Captures::NoteKind::Warning);
        else Note(I18n::Trf("Video saved: {} \xC2\xB7 {} MB", Clock(static_cast<int>(g_clipBaseSec + g_lastSec), false),
                            static_cast<int>(g_lastBytes / (1024ull * 1024ull) + 1)), 5, Captures::NoteKind::Success);
    }
}

void BeforeOverlay(IDirect3DDevice9* dev) {
    // look + colour done, Apex menu and notes not yet drawn
    if (VideoMp4::Active() && !GrabAtPresent(g_rec.video) && !g_lookSeen) VideoMp4::Grab(dev);
}

void OnPreReset(IDirect3DDevice9*) {
    if (VideoMp4::Active()) { StopRecording(); LOG_INFO("[Video] Stopped: the screen was reset"); }
    VideoMp4::Release();
    g_bbH = g_bbW = 0;
    ReleaseBefore();
}

// Hooked once, at the first capture that needs it (a recording, a picture without the look); idle afterwards
void EnsureHooks() {
    if (g_hooks.exchange(true)) return;
    RenderCallbacks::Add(RenderCallbacks::filteredSceneBeforeOverlay, BeforeOverlay);
    RenderCallbacks::Add(RenderCallbacks::preReset, OnPreReset);
    D3D9Hooks::RegisterPresent("ApexCapture", [](D3D9Hooks::DeviceContext& ctx, const RECT*, const RECT*, HWND, const RGNDATA*) {
        OnPresent(ctx.device);
        return D3D9Hooks::HookAction::Continue;
    }, D3D9Hooks::Priority::Early);
    LOG_INFO("[Capture] Frame hooks installed");
}

// ---------------------------------------------------------------------------------------------- menu helpers
// The tokens of the folder and file-name boxes: Apex's tooltip while the box is hovered or typed in
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
        {"%Height%", "Picture height"}, {"%Counter%", "A number that goes up by one with each file"}};
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
    static std::map<ImGuiID, std::string> editing; // by the row's ImGui id: the cards' settings are copies on the stack
    bool changed = false;
    const float w = std::fmin(width * ApexUi::Unit(), ImGui::GetContentRegionAvail().x); // never past the card when narrow
    if (ApexUi::BeginControlRow(label, desc, w)) {
        const ApexUi::ControlSizeScope size(ApexUi::ControlSize::Compact);
        char buf[512];
        const ImGuiID key = ImGui::GetID(label);
        auto it = editing.find(key);
        snprintf(buf, sizeof(buf), "%s", it != editing.end() ? it->second.c_str() : value.c_str());
        ImGui::SetNextItemWidth(w);
        ImGui::PushID(&value);
        ImGui::InputText("##t", buf, sizeof(buf));
        if (tokens) TokenTooltip();
        if (ImGui::IsItemActive()) editing[key] = buf;
        if (ImGui::IsItemDeactivated()) {
            if (value != buf) { value = buf; changed = true; }
            editing.erase(key);
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

// ShellExecute runs off the render thread: it pumps the game window's messages while Explorer starts, which left the
// overlay's input stuck (the menu no longer opened after "Open the folder")
void ShellOpen(std::wstring file, std::wstring args) {
    HookGuard::StartDetached("Capture: open a folder", [file = std::move(file), args = std::move(args)] {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        ShellExecuteW(nullptr, L"open", file.c_str(), args.empty() ? nullptr : args.c_str(), nullptr, SW_SHOWNORMAL);
        if (SUCCEEDED(com)) CoUninitialize();
    });
}

void OpenFolderOf(const std::string& path) {
    const auto f = ExpandFolder(path);
    std::error_code ec;
    if (f.empty() || !(std::filesystem::create_directories(f, ec), std::filesystem::is_directory(f, ec))) return;
    ShellOpen(f.wstring(), L"");
}

} // namespace

// ---------------------------------------------------------------------------------------------- folder rows
// Windows' folder dialog on its own thread; the chosen path is taken by FolderRow on a later frame, nothing when
// cancelled. One dialog at a time; slot says which row asked.
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
    const float w = ApexUi::ButtonWidth(change, true);
    // the row's description is the folder itself (as set: tokens such as %Day% stay visible)
    if (ApexUi::BeginControlRow(label, value.empty() ? "No folder set" : value.c_str(), w)) {
        ImGui::BeginDisabled(g_pickBusy.load());
        if (ApexUi::IconTextButton(change, IconId::FolderOpen, pickTip))
            PickFolder(slot, ApexUtil::ToUtf8(ExpandFolder(value).wstring()), slot == 0 ? "Choose a folder for the screenshots" : "Choose a folder for the videos");
        ImGui::EndDisabled();
        ApexUi::EndControlRow();
    }
    // the plain text box, for typing a path or tokens, behind its own arrow
    if (ApexUi::BeginAdvanced("##TypeFolder", "Type a folder")) {
        static std::map<ImGuiID, std::string> editing;
        const float bw = std::fmin(300.0f * u, ImGui::GetContentRegionAvail().x);
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

// ---------------------------------------------------------------------------------------------- settings (public)
Settings FromToml(const toml::table& root) {
    Settings s;
    const auto c = root["capture"];
    s.sound = c["sound"].value_or(std::string());
    s.notes = c["notes"].value_or(s.notes);
    s.counter = std::clamp(static_cast<int>(c["counter"].value_or(int64_t{s.counter})), 1, 99999999);
    const auto t = c["screenshot"];
    ScreenshotSettings& sh = s.shot;
    sh.fileName = t["file_name"].value_or(sh.fileName);
    sh.includeMenu = t["include_menu"].value_or(sh.includeMenu);
    sh.includePointer = t["include_pointer"].value_or(sh.includePointer);
    sh.look = NameIndex(t["look"], kLookNames, sh.look);
    sh.savePreset = t["save_preset"].value_or(sh.savePreset);
    sh.jpg = Lower(t["file_type"].value_or(std::string("png"))) == "jpg";
    sh.jpgQuality = Int(t["jpg_quality"], sh.jpgQuality);
    sh.sizePercent = Int(t["size_percent"], sh.sizePercent);
    sh.burstCount = Int(t["burst_count"], sh.burstCount);
    sh.burstIntervalMs = Int(t["burst_interval_ms"], sh.burstIntervalMs);
    const auto vt = c["video"];
    VideoSettings& v = s.video;
    v.folder = NameIndex(vt["folder"], kFolderNames, v.folder);
    v.customFolder = vt["custom_folder"].value_or(v.customFolder);
    v.fileName = vt["file_name"].value_or(v.fileName);
    v.hideGameUi = vt["hide_game_ui"].value_or(v.hideGameUi);
    v.includeMenu = vt["include_menu"].value_or(v.includeMenu);
    v.includePointer = vt["include_pointer"].value_or(v.includePointer);
    v.look = NameIndex(vt["look"], kLookNames, v.look);
    v.savePreset = vt["save_preset"].value_or(v.savePreset);
    v.size = Int(vt["size"], v.size);
    v.fps = Int(vt["fps"], v.fps);
    v.quality = NameIndex(vt["quality"], kQualityNames, v.quality);
    v.customMbps = Int(vt["custom_mbps"], v.customMbps);
    v.container = NameIndex(vt["container"], kContainerNames, v.container);
    v.keepMp4 = vt["keep_mp4"].value_or(v.keepMp4);
    v.hevc = Lower(vt["codec"].value_or(std::string("h264"))) == "hevc";
    v.gameSound = vt["game_sound"].value_or(v.gameSound);
    v.newClipMb = Int(vt["new_clip_mb"], v.newClipMb);
    v.stopAfterS = Int(vt["stop_after_s"], v.stopAfterS);
    v.stopAtMb = Int(vt["stop_at_mb"], v.stopAtMb);
    v.warnBeforeS = Int(vt["warn_before_s"], v.warnBeforeS);
    v.timer = vt["timer"].value_or(v.timer);
    v.timerPosition = NameIndex(vt["timer_position"], kPosNames, v.timerPosition);
    v.timerSize = Int(vt["timer_size"], v.timerSize);
    v.timerOpacity = Int(vt["timer_opacity"], v.timerOpacity);
    v.timerBlink = vt["timer_blink"].value_or(v.timerBlink);
    v.timerFileSize = vt["timer_file_size"].value_or(v.timerFileSize);
    v.timerStopKey = vt["timer_stop_key"].value_or(v.timerStopKey);
    v.timerHours = vt["timer_hours"].value_or(v.timerHours);
    v.redFrame = vt["red_frame"].value_or(v.redFrame);
    v.timerInVideo = vt["timer_in_video"].value_or(v.timerInVideo);
    v.external = Lower(vt["recorder"].value_or(std::string("apex"))) == "external";
    v.externalKey = vt["external_key"].value_or(v.externalKey);
    ApexConfig::KeyChord k;
    if (ApexConfig::ParseKeyChord(vt["record_key"].value_or(std::string()), k) && k.vk) v.recordKey = k;
    return Clamped(s);
}

void ToToml(const Settings& in, toml::table& root) {
    const Settings s = Clamped(in);
    toml::table shot;
    shot.insert("file_name", s.shot.fileName);
    shot.insert("include_menu", s.shot.includeMenu);
    shot.insert("include_pointer", s.shot.includePointer);
    shot.insert("look", kLookNames[s.shot.look]);
    shot.insert("save_preset", s.shot.savePreset);
    shot.insert("file_type", s.shot.jpg ? "jpg" : "png");
    shot.insert("jpg_quality", static_cast<int64_t>(s.shot.jpgQuality));
    shot.insert("size_percent", static_cast<int64_t>(s.shot.sizePercent));
    shot.insert("burst_count", static_cast<int64_t>(s.shot.burstCount));
    shot.insert("burst_interval_ms", static_cast<int64_t>(s.shot.burstIntervalMs));
    const VideoSettings& v = s.video;
    toml::table video;
    video.insert("folder", kFolderNames[v.folder]);
    video.insert("custom_folder", v.customFolder);
    video.insert("file_name", v.fileName);
    video.insert("hide_game_ui", v.hideGameUi);
    video.insert("include_menu", v.includeMenu);
    video.insert("include_pointer", v.includePointer);
    video.insert("look", kLookNames[v.look]);
    video.insert("save_preset", v.savePreset);
    video.insert("size", static_cast<int64_t>(v.size));
    video.insert("fps", static_cast<int64_t>(v.fps));
    video.insert("quality", kQualityNames[v.quality]);
    video.insert("custom_mbps", static_cast<int64_t>(v.customMbps));
    video.insert("container", kContainerNames[v.container]);
    video.insert("keep_mp4", v.keepMp4);
    video.insert("codec", v.hevc ? "hevc" : "h264");
    video.insert("game_sound", v.gameSound);
    video.insert("new_clip_mb", static_cast<int64_t>(v.newClipMb));
    video.insert("stop_after_s", static_cast<int64_t>(v.stopAfterS));
    video.insert("stop_at_mb", static_cast<int64_t>(v.stopAtMb));
    video.insert("warn_before_s", static_cast<int64_t>(v.warnBeforeS));
    video.insert("timer", v.timer);
    video.insert("timer_position", kPosNames[v.timerPosition]);
    video.insert("timer_size", static_cast<int64_t>(v.timerSize));
    video.insert("timer_opacity", static_cast<int64_t>(v.timerOpacity));
    video.insert("timer_blink", v.timerBlink);
    video.insert("timer_file_size", v.timerFileSize);
    video.insert("timer_stop_key", v.timerStopKey);
    video.insert("timer_hours", v.timerHours);
    video.insert("red_frame", v.redFrame);
    video.insert("timer_in_video", v.timerInVideo);
    video.insert("recorder", v.external ? "external" : "apex");
    video.insert("external_key", v.externalKey);
    video.insert("record_key", ApexConfig::KeyChordText(v.recordKey));
    toml::table t;
    t.insert("sound", s.sound.empty() ? std::string("off") : s.sound);
    t.insert("notes", s.notes);
    t.insert("counter", static_cast<int64_t>(s.counter));
    t.insert("screenshot", std::move(shot));
    t.insert("video", std::move(video));
    root.insert_or_assign("capture", std::move(t));
}

void LoadFromToml(const toml::table& root) {
    Settings s = FromToml(root);
    const bool saveName = WantsSaveName(s);
    {
        std::lock_guard<std::mutex> lk(g_lock);
        g_s = std::move(s);
        g_loaded = true;
        ++g_sVersion;
    }
    if (saveName) RefreshSaveName(); // ready before the first capture
}

Settings Get() {
    std::lock_guard<std::mutex> lk(g_lock);
    EnsureLoadedLocked();
    return g_s;
}

void Set(const Settings& s) {
    const Settings c = Clamped(s);
    const bool saveName = WantsSaveName(c);
    {
        std::lock_guard<std::mutex> lk(g_lock);
        g_loaded = true;
        g_s = c;
        ++g_sVersion;
    }
    if (saveName) RefreshSaveName();
    ApexConfig::RequestSave();
}

std::filesystem::path DefaultFolder(int mode, bool video) {
    const std::wstring base = mode == kFolderApex ? ApexPaths::ApexDirectory() : ApexPaths::GameDocumentsDirectory();
    if (base.empty()) return {};
    // the game's own folders: Screenshots, and Recorded Videos (where the game's video capture saves)
    const wchar_t* sub = !video ? L"Screenshots" : mode == kFolderApex ? L"Videos" : L"Recorded Videos";
    return std::filesystem::path(base) / sub;
}

std::filesystem::path CaptureFolder(bool video, FolderResult& r) {
    int mode = kFolderGame;
    std::string custom;
    if (video) {
        const Settings s = Get();
        mode = s.video.folder;
        custom = s.video.customFolder;
    } else {
        const ApexConfig::UiSettings ui = ApexConfig::GetUi();
        mode = ui.screenshotFolder;
        custom = ui.screenshotCustomFolder;
    }
    const int slot = video ? 1 : 0;
    r = FolderResult::Ok;
    if (mode == kFolderCustom && !Trim(custom).empty()) {
        const auto f = ExpandFolder(custom);
        if (MakeFolder(f, slot)) return f;
        LOG_WARNING(std::string("[Captures] The chosen ") + (video ? "videos" : "screenshots") + " folder is not available: " + custom);
        r = FolderResult::FellBack;
    }
    const auto f = DefaultFolder(mode == kFolderApex ? kFolderApex : kFolderGame, video);
    if (MakeFolder(f, slot)) return f;
    r = FolderResult::Failed;
    return {};
}

std::filesystem::path NewFile(const std::filesystem::path& folder, const std::string& pattern, const char* ext, bool video, int width, int height) {
    SYSTEMTIME t{};
    GetLocalTime(&t);
    std::wstring n = ApexUtil::ToWide(pattern.empty() ? std::string(kDefaultShotName) : pattern);
    n = TimeTokens(n, t);
    if (n.find(L'%') != std::wstring::npos) {
        std::wstring preset;
        const bool look = Looks::PresetInUse(preset);
        std::wstring presetName = std::filesystem::path(preset).stem().wstring();
        if (presetName.empty()) presetName = L"Apex";
        int w = width ? width : g_bbW, h = height ? height : g_bbH; // screenshots pass the back buffer's size
        if (n.find(L"%Counter%") != std::wstring::npos) n = Replace(n, L"%Counter%", std::format(L"{:04}", NextCounter()));
        if (!video) { const int sc = Get().shot.sizePercent; w = w * sc / 100; h = h * sc / 100; }
        n = Replace(n, L"%Type%", video ? L"Video" : L"Screenshot");
        n = Replace(n, L"%Part%", std::format(L"{:02}", video ? std::max(1, g_clipPart) : 1));
        n = Replace(n, L"%Width%", std::to_wstring(w));
        n = Replace(n, L"%Height%", std::to_wstring(h));
        n = Replace(n, L"%PresetName%", presetName);
        n = Replace(n, L"%Look%", look ? L"on" : L"off");
    }
    n = SafeName(n);
    if (n.empty()) n = L"Apex";
    const std::wstring e = L"." + ApexUtil::ToWide(ext);
    // names already handed out count as taken: a picture's file is written later, on another thread, so a burst or two
    // quick presses in one second would otherwise get the same name
    static std::mutex lock;
    static std::deque<std::wstring> given; // the latest 256, lower case
    std::lock_guard<std::mutex> guard(lock);
    auto taken = [&](const std::filesystem::path& f) {
        std::error_code ec;
        const std::wstring key = LowerW(f.wstring());
        return std::filesystem::exists(f, ec) || std::find(given.begin(), given.end(), key) != given.end();
    };
    std::filesystem::path file = folder / (n + e);
    for (unsigned k = 2; !folder.empty() && taken(file) && k < 10000; ++k) file = folder / (n + std::format(L" ({})", k) + e);
    given.push_back(LowerW(file.wstring()));
    if (given.size() > 256) given.pop_front();
    return file;
}

std::wstring PresetFile() {
    std::wstring f;
    return Looks::PresetInUse(f) ? f : std::wstring();
}

// sound = camera: a mechanical shutter made here (two curtain clicks and the mirror's thump), 16-bit mono WAV
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

void PlaySoundFile() {
    const std::string p = Get().sound;
    if (p.empty()) return;
    if (Lower(p) != "camera") {
        wchar_t env[MAX_PATH * 2]{};
        std::wstring file = ApexUtil::ToWide(p);
        if (ExpandEnvironmentStringsW(file.c_str(), env, MAX_PATH * 2)) file = env;
        // only a file on this PC: a network path would stall the frame and hand Windows' sign-in to that host
        const bool local = file.size() > 3 && iswalpha(file[0]) && file[1] == L':' && (file[2] == L'\\' || file[2] == L'/') &&
                           GetDriveTypeW(file.substr(0, 3).c_str()) != DRIVE_REMOTE;
        std::error_code ec;
        if (local && std::filesystem::exists(file, ec)) { PlaySoundW(file.c_str(), nullptr, SND_FILENAME | SND_ASYNC | SND_NODEFAULT); return; }
    }
    PlaySoundW(reinterpret_cast<LPCWSTR>(CameraShutterWav().data()), nullptr, SND_MEMORY | SND_ASYNC | SND_NODEFAULT); // camera, or a missing file
}

bool ParseExternalKey(const std::string& text, UINT& vk, bool& ctrl, bool& shift, bool& alt, bool& win) {
    ctrl = shift = alt = win = false;
    vk = 0;
    std::string rest = Trim(text);
    // "Win+" is not an Apex key name: taken off first
    for (;;) {
        const size_t plus = rest.find('+');
        if (plus == std::string::npos || plus + 1 >= rest.size()) break;
        const std::string mod = Lower(Trim(rest.substr(0, plus)));
        if (mod == "win" || mod == "windows") { win = true; rest = rest.substr(plus + 1); continue; }
        break;
    }
    ApexConfig::KeyChord c;
    if (rest.empty() || !ApexConfig::ParseKeyChord(rest, c) || !c.vk) return false;
    vk = c.vk; ctrl = c.ctrl; shift = c.shift; alt = c.alt;
    return true;
}

bool ExternalKeyAllowed(const std::string& text) {
    UINT vk = 0; bool c = false, s = false, a = false, w = false;
    if (!ParseExternalKey(text, vk, c, s, a, w)) return false;
    if (!c && !s && !a && !w) return false; // a bare key
    switch (vk) {
    case VK_SHIFT: case VK_CONTROL: case VK_MENU: case VK_LWIN: case VK_RWIN: case VK_APPS:
    case VK_LSHIFT: case VK_RSHIFT: case VK_LCONTROL: case VK_RCONTROL: case VK_LMENU: case VK_RMENU:
        return false; // a modifier alone
    default: break;
    }
    if (a && (vk == VK_F4 || vk == VK_TAB || vk == VK_ESCAPE || vk == VK_SPACE)) return false; // close, switch, window menu
    if (c && vk == VK_ESCAPE) return false;                                                    // Start menu, Task Manager
    if (c && a && (vk == VK_DELETE || vk == VK_END)) return false;                             // the secure attention keys
    if (w && (vk == 'L' || vk == 'D' || vk == 'M' || vk == VK_TAB)) return false;               // lock, desktop, minimise, task view
    return true;
}

void WantBefore(bool on) {
    g_wantBefore = on;
    if (on) EnsureHooks(); // its render target is released at a device reset
    else ReleaseBefore();
}

void BeforeLook(IDirect3DDevice9* dev, IDirect3DSurface9* src) {
    if (!dev || !src) return;
    // a video without the look: this frame is taken here, before the look draws it
    if (VideoMp4::Active() && g_rec.video.look == kLookWithout && !g_withoutFailed) {
        g_lookSeen = true;
        if (!VideoMp4::Grab(dev, src)) {
            g_lookSeen = false;
            g_withoutFailed = true;
            LOG_WARNING("[Video] The frame before the look could not be copied; the video keeps the look");
        }
    }
    if (!g_wantBefore) return;
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
        const ApexConfig::KeyChord k = RecordKey();
        if (!k.vk || k.vk != vk) return false;
        if (!k.ctrl && !k.shift && !k.alt && ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9') || vk == VK_SPACE)) return false; // typing
        const bool ctrl = GetKeyState(VK_CONTROL) < 0, shift = GetKeyState(VK_SHIFT) < 0, alt = GetKeyState(VK_MENU) < 0;
        if (ctrl != k.ctrl || shift != k.shift || alt != k.alt) return false;
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
void Poll() { if (g_toggle.load()) EnsureHooks(); }

void OnGameClosing() {
    if (g_externalOn.exchange(false)) SendExternalKey(g_rec.video.externalKey);
    if (!VideoMp4::Active() && !VideoMp4::Finishing()) return;
    LOG_INFO("[Video] The game is closing: finishing the video");
    if (!VideoMp4::StopAndWait(5000)) LOG_WARNING("[Video] The video was still being written when the game closed");
}

bool Recording() { return VideoMp4::Active() || g_externalOn; }
int RecordingSeconds() { // all clips of the recording; the other app's since its key was sent
    if (g_externalOn) return static_cast<int>((GetTickCount64() - g_externalStart) / 1000ull);
    return static_cast<int>(g_clipBaseSec + VideoMp4::Seconds());
}

// The recording timer: a pill in the Apex note style (red dot, time, file size, stop key) at timer_position, and the
// optional red frame. Drawn on the foreground list, no input. Kept out of the video unless timer_in_video (GrabAtPresent),
// out of screenshots, and hidden when the menu is recorded but the timer is not (it would be in the video).
bool DrawTimer() {
    try {
        if (!Recording() || Captures::ScreenshotPending()) return false;
        const VideoSettings& s = VideoNow();
        if (!s.timer && !s.redFrame) return false;
        if (s.includeMenu && !s.timerInVideo && VideoMp4::Active()) return false;
        const bool builtin = VideoMp4::Active();
        const int sec = RecordingSeconds();
        const unsigned long long bytes = builtin ? VideoMp4::BytesWritten() : 0ull;
        // time left before stop_after_s / stop_at_mb (built-in only; the size estimate uses the rate so far)
        int left = -1;
        if (builtin && s.warnBeforeS > 0) {
            if (s.stopAfterS > 0) left = s.stopAfterS - sec;
            const int clipSec = static_cast<int>(VideoMp4::Seconds());
            if (s.newClipMb <= 0 && s.stopAtMb > 0 && clipSec >= 3 && bytes > 0) {
                const double rate = static_cast<double>(bytes) / std::max(1, clipSec);
                const double room = static_cast<double>(static_cast<unsigned long long>(s.stopAtMb) << 20) - static_cast<double>(bytes);
                const int bySize = static_cast<int>(std::max(0.0, room) / rate);
                left = left < 0 ? bySize : std::min(left, bySize);
            }
            if (left > s.warnBeforeS) left = -1;
        }
        const bool warn = left >= 0;
        const float u = ApexUi::Unit();
        const float scale = static_cast<float>(s.timerSize) / 100.0f;
        const float alpha = static_cast<float>(s.timerOpacity) / 100.0f;
        const auto* vp = ImGui::GetMainViewport();
        ImDrawList* dl = ImGui::GetForegroundDrawList();
        const unsigned tint = warn ? VioletTheme::kWarning : VioletTheme::kError;
        if (s.redFrame) {
            const float t = 3.0f * u;
            dl->AddRect(ImVec2(vp->Pos.x + t * 0.5f, vp->Pos.y + t * 0.5f),
                        ImVec2(vp->Pos.x + vp->Size.x - t * 0.5f, vp->Pos.y + vp->Size.y - t * 0.5f),
                        ImGui::GetColorU32(VioletTheme::Col(tint, 0.85f * alpha)), 0.0f, 0, t);
        }
        if (!s.timer) return true;
        // the pill's text changes about once a second: built again only then, not at every frame
        struct TextKey {
            int sec, left; unsigned long long mb; bool builtin; unsigned version; I18n::Lang lang;
            bool operator==(const TextKey&) const = default;
        };
        static TextKey shownKey{-1, -1, 0, false, 0, I18n::Lang{}};
        static std::string text, extra;
        const TextKey key{sec, left, bytes >> 20, builtin, g_sVersion.load(), I18n::Current()};
        if (!(key == shownKey)) {
            shownKey = key;
            text = Clock(sec, s.timerHours);
            extra.clear();
            if (s.timerFileSize && builtin) extra += std::format("  \xC2\xB7  {} MB", bytes >> 20);
            if (warn) extra += std::string("  \xC2\xB7  ") + I18n::Trf("{} left", Clock(left, false));
            else if (s.timerStopKey) extra += std::string("  \xC2\xB7  ") + I18n::Trf("{} stops", ApexConfig::KeyChordText(s.recordKey));
        }
        ImFont* font = ImGui::GetFont();
        const float fs = ImGui::GetFontSize() * scale;
        const ImVec2 t1 = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, text.c_str());
        const ImVec2 t2 = extra.empty() ? ImVec2(0, 0) : font->CalcTextSizeA(fs, FLT_MAX, 0.0f, extra.c_str());
        const float pad = 14.0f * u * scale, dotR = 5.0f * u * scale, gap = 10.0f * u * scale;
        const float h = std::max(40.0f * u * scale, fs + 16.0f * u * scale);
        const float w = pad + 2.0f * dotR + gap + t1.x + t2.x + pad;
        const float margin = (ApexUi::kSpace4 + ApexUi::kSpace1) * u;
        const bool bottom = s.timerPosition >= kBottomLeft;
        const bool atLeft = s.timerPosition == kTopLeft || s.timerPosition == kBottomLeft;
        const bool atRight = s.timerPosition == kTopRight || s.timerPosition == kBottomRight;
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

bool NeedsGradeBeforeOverlay() { return VideoMp4::Active() && !GrabAtPresent(g_rec.video); }

// ---------------------------------------------------------------------------------------------- Image > Capture
namespace {
std::mutex g_lastLock;
std::filesystem::path g_lastSaved[2]; // this session's last screenshot / video

// Explorer with the file selected
void ShowInExplorer(const std::filesystem::path& file) {
    ShellOpen(L"explorer.exe", L"/select,\"" + file.wstring() + L"\"");
}

std::string MinSec(int sec) {
    sec = std::max(0, sec);
    return sec >= 3600 ? std::format("{}:{:02}:{:02}", sec / 3600, sec / 60 % 60, sec % 60) : std::format("{}:{:02}", sec / 60, sec % 60);
}
int MbPerMinute(const VideoSettings& v) { return static_cast<int>(KbpsFor(v) * 60.0 / 8.0 / 1024.0 + 0.5); }

// "<key> · Last: <name>" (the name only when one was saved this session)
std::string WithLast(std::string status, const std::filesystem::path& last) {
    if (!last.empty()) status += " \xC2\xB7 " + I18n::Trf("Last: {}", ApexUtil::ToUtf8(last.filename().wstring()));
    return status;
}

// "Where": Save to (the folder in use as its description, Open the folder beside it) and, with Other, the Folder row
bool WhereRows(bool video, int& mode, std::string& custom) {
    static const char* const kWhere[] = {"Game", "Apex", "Other"};
    static const char* const kShotTips[] = {"Next to the photos the game takes, in Documents > Electronic Arts > The Sims 3 > Screenshots",
                                            "Kept apart from the game's photos, in Documents > Electronic Arts > The Sims 3 > Apex Radiance > Screenshots",
                                            "Any folder you choose, for example on another drive"};
    bool ch = false;
    int m = std::clamp(mode, 0, 2);
    const std::string where = m == kFolderCustom && !Trim(custom).empty()
        ? custom : ApexUtil::ToUtf8(DefaultFolder(m == kFolderApex ? kFolderApex : kFolderGame, video).wstring());
    ApexUi::SetNextRowAction(IconId::FolderOpen, "Open the folder");
    if (ApexUi::SegmentedRow("Save to", where.c_str(), video ? "##VideoWhere" : "##ShotWhere", &m, kWhere, 3, video ? nullptr : kShotTips, nullptr, kFolderGame)) {
        // Other starts from the folder in use; type over it or press Change
        if (m == kFolderCustom && Trim(custom).empty()) custom = ApexUtil::ToUtf8(DefaultFolder(mode == kFolderApex ? kFolderApex : kFolderGame, video).wstring());
        if (m != kFolderCustom || !Trim(custom).empty()) { mode = m; ch = true; }
    }
    if (ApexUi::RowActionClicked()) OpenFolderOf(where);
    if (mode == kFolderCustom)
        ch |= FolderRow("Folder", video ? "VideoFolder" : "ShotFolder", custom, video ? 1 : 0,
                        video ? "Choose another folder for the videos" : "Choose another folder for the screenshots");
    return ch;
}

// [capture] sound and notes (both tabs). play: the sound choice changed (heard once it is saved)
bool WhenSavedRows(Settings& s, bool& play) {
    const Settings def;
    bool ch = false;
    ApexUi::CardGroup("When saved");
    static const char* const kSound[] = {"Off", "Camera", "Own file"};
    const std::string low = Lower(s.sound);
    int mode = low.empty() ? 0 : low == "camera" ? 1 : 2;
    static int ownMode = -1; // "Own file" chosen with no file typed yet
    if (ownMode == 2 && mode == 0) mode = 2;
    if (Pick("Sound", "Plays with each picture and when a video starts and stops", "##CapSound", mode, kSound, 3, 0)) {
        ownMode = mode;
        if (mode != 2) { s.sound = mode == 0 ? "" : "camera"; ch = play = true; }
    }
    if (mode == 2) {
        std::string file = low == "camera" ? std::string() : s.sound;
        if (TextRow("Sound file", "Full path of a .wav file", file)) { s.sound = file; ch = play = true; }
    }
    ch |= ApexUi::SwitchRow("Note when saved", &s.notes, "A short note when a picture or video is saved", def.notes);
    return ch;
}

constexpr const char* kFileNameDesc = "Date, time and look name; hover the box for the tokens";
} // namespace

std::filesystem::path LastSaved(bool video) {
    std::lock_guard<std::mutex> lk(g_lastLock);
    return g_lastSaved[video ? 1 : 0];
}

void SetLastSaved(bool video, const std::filesystem::path& file) {
    std::lock_guard<std::mutex> lk(g_lastLock);
    g_lastSaved[video ? 1 : 0] = file;
}

const char* CannotRecord(bool gameAa) {
    if (Get().video.external) return nullptr; // the other app records
    if (!VideoMp4::MediaFoundationAvailable()) return "Videos need Windows' Media Feature Pack, which this Windows does not have";
    if (gameAa) return "The game's own Edge Smoothing is on; turn it off to record (Options \xE2\x80\xBA Graphics)";
    // the recorder needs one 192 MB block of the game's addresses (VideoMp4::Start); walked at most once a second
    static unsigned long long checkedAt = 0;
    static bool low = false;
    const unsigned long long now = GetTickCount64();
    if (!checkedAt || now - checkedAt >= 1000) { checkedAt = now; low = VideoMp4::LargestFreeBlock() < (192u << 20); }
    if (low) return "The game is low on free memory for a video; save and restart the game";
    return nullptr;
}

bool ScreenshotCard(bool lookOn) {
    bool take = false;
    if (ApexUi::BeginCard("##CaptureShots")) {
        const Settings def;
        Settings s = Get();
        ApexConfig::UiSettings ui = ApexConfig::GetUi(); // [ui] screenshot_folder, _custom_folder, _hide_game_ui
        bool ch = false, uiCh = false, play = false;
        const std::filesystem::path last = LastSaved(false);
        const std::string status = WithLast(ApexConfig::KeyChordText(ui.screenshotKey), last);
        const int hit = ApexUi::CardStatusHeader(IconId::Camera, "Screenshots", status.c_str(), VioletTheme::kTextMuted, false,
                                                 last.empty() ? nullptr : "Show", "Take one", IconId::Camera, ApexUi::ButtonKind::Primary, true);
        if (hit == 1) take = true;
        if (hit == 2) ShowInExplorer(last);
        ApexUi::CardDivider();

        uiCh |= WhereRows(false, ui.screenshotFolder, ui.screenshotCustomFolder);
        uiCh |= ApexUi::SwitchRow("Hide game UI", &ui.screenshotHideGameUi, "Hides the game interface for the photo, then restores it", true);
        if (lookOn) {
            static const char* const kLook[] = {"With", "Without", "Both"};
            ch |= Pick("Look", "Both also saves the picture without it", "##ShotLook", s.shot.look, kLook, 3, def.shot.look);
        }
        static const char* const kFormat[] = {"PNG", "JPG"};
        int fmt = s.shot.jpg ? 1 : 0;
        if (Pick("Format", "PNG keeps every detail; JPG files are smaller", "##CapFormat", fmt, kFormat, 2, 0)) { s.shot.jpg = fmt == 1; ch = true; }

        std::string summary = I18n::Trf("{}% size", s.shot.sizePercent);
        if (s.shot.burstCount > 1) summary += ", " + I18n::Trf("{} per press", s.shot.burstCount);
        if (ApexUi::BeginAdvanced("##ShotAdvanced", "Advanced", summary.c_str())) {
            ApexUi::CardGroup("File");
            if (s.shot.jpg) ch |= IntSlider("JPG quality", "Higher keeps more detail and makes larger files", s.shot.jpgQuality, 1, 100, def.shot.jpgQuality, "%.0f%%");
            ch |= IntSlider("Size", "Of the game's screen size", s.shot.sizePercent, 10, 100, def.shot.sizePercent, "%.0f%%");
            ch |= TextRow("File name", kFileNameDesc, s.shot.fileName, 300.0f, true);
            ApexUi::CardGroup("Also in the picture");
            ch |= ApexUi::SwitchRow("Include the Apex menu", &s.shot.includeMenu, "When the menu is open, it is in the picture", def.shot.includeMenu);
            ch |= ApexUi::SwitchRow("Include the mouse pointer", &s.shot.includePointer, "Where it was on screen, at the picture's size", def.shot.includePointer);
            ch |= ApexUi::SwitchRow("Save the look's preset too", &s.shot.savePreset, "A copy of the look's .ini next to the picture", def.shot.savePreset);
            ApexUi::CardGroup("Series");
            ch |= IntSlider("Pictures per press", "More than one takes a quick series", s.shot.burstCount, 1, kMaxBurst, def.shot.burstCount);
            if (s.shot.burstCount > 1) ch |= IntSlider("Time between pictures", "In milliseconds", s.shot.burstIntervalMs, 50, 5000, def.shot.burstIntervalMs, "%.0f ms");
            ch |= WhenSavedRows(s, play);
            ApexUi::EndAdvanced();
        }
        if (uiCh) ApexConfig::SetUi(ui);
        if (ch) Set(s);
        if (play) PlaySoundFile(); // hear the choice
    }
    ApexUi::EndCard();
    return take;
}

void VideoCard(bool lookOn, bool gameAa) {
    const char* why = nullptr;
    if (ApexUi::BeginCard("##CaptureVideo")) {
        const Settings def;
        Settings s = Get();
        VideoSettings& v = s.video;
        const VideoSettings& dv = def.video;
        bool ch = false, play = false;
        const bool rec = Recording();
        const bool finishing = !rec && VideoMp4::Finishing();
        why = rec || finishing ? nullptr : CannotRecord(gameAa);
        const std::filesystem::path last = LastSaved(true);
        std::string status;
        if (rec) {
            status = VideoMp4::Active() ? I18n::Trf("Recording {} \xC2\xB7 {} MB", MinSec(RecordingSeconds()), static_cast<int>(VideoMp4::BytesWritten() >> 20))
                                        : I18n::Trf("Recording {}", MinSec(RecordingSeconds()));
        } else if (finishing) {
            status = I18n::Tr("Saving the video\xE2\x80\xA6");
        } else {
            status = ApexConfig::KeyChordText(v.recordKey);
            if (!v.external) status += " \xC2\xB7 " + I18n::Trf("about {} MB a minute", MbPerMinute(v));
            status = WithLast(status, last);
        }
        if (why) status = I18n::Tr(why); // in the header, in amber, with Record off
        const bool showLink = !rec && !finishing && !last.empty() && !why;
        const int hit = ApexUi::CardStatusHeader(IconId::Video, "Video", status.c_str(), rec ? VioletTheme::kError : why ? VioletTheme::kWarning : VioletTheme::kTextMuted, rec,
                                                 showLink ? "Show" : nullptr, rec ? "Stop" : "Record", rec ? IconId::X : IconId::Video,
                                                 rec ? ApexUi::ButtonKind::Secondary : ApexUi::ButtonKind::Primary, rec || (!finishing && !why));
        if (hit == 1) ToggleRecording();
        if (hit == 2) ShowInExplorer(last);
        ApexUi::CardDivider();

        const bool own = !v.external; // Apex records; with another app, Apex only presses its key and the rest is that app's
        if (!own) ApexUi::IconNote(IconId::Info, "Another app records these videos; set their folder and quality in that app");
        if (own) ch |= WhereRows(true, v.folder, v.customFolder);
        ch |= ApexUi::SwitchRow("Hide game UI", &v.hideGameUi, "Hides the game interface until the video stops", dv.hideGameUi);
        if (own) ch |= ApexUi::SwitchRow("Game sound", &v.gameSound, "Only The Sims 3, not music or chat apps", dv.gameSound);
        if (own && lookOn) {
            static const char* const kLook[] = {"With", "Without"};
            int look = v.look == kLookWithout ? 1 : 0;
            if (Pick("Look", "Without records the game without your look", "##VideoLook", look, kLook, 2, 0)) {
                v.look = look ? kLookWithout : kLookWith;
                ch = true;
            }
        }
        if (own) {
            static const char* const kQuality[] = {"Smaller", "Balanced", "Best", "Custom"};
            const std::string qualityDesc = I18n::Trf("{}p \xC2\xB7 {} fps \xC2\xB7 about {} MB a minute", v.size, v.fps, MbPerMinute(v));
            ch |= Pick("Quality", qualityDesc.c_str(), "##CapQuality", v.quality, kQuality, 4, dv.quality);
        }

        static const char* const kContName[] = {"MP4", "Crash-safe MP4", "MKV"};
        std::string summary = own ? std::string(I18n::Tr(kContName[std::clamp(v.container, 0, 2)])) + ", " + (v.hevc ? "HEVC" : "H.264") : std::string();
        if (v.timer) summary += std::string(summary.empty() ? "" : ", ") + I18n::Tr("timer on screen");
        if (ApexUi::BeginAdvanced("##VideoAdvanced", "Advanced", summary.c_str())) {
          if (own) { // what only Apex's own recorder uses
            ApexUi::CardGroup("Picture");
            static const char* const kSize[] = {"720p", "1080p", "1440p", "4K"};
            static const int kSizeV[] = {720, 1080, 1440, 2160};
            int si = v.size >= 2160 ? 3 : v.size >= 1440 ? 2 : v.size >= 1080 ? 1 : 0;
            if (Pick("Size", "Larger looks sharper and costs more speed", "##CapSize", si, kSize, 4, 1)) { v.size = kSizeV[si]; ch = true; }
            static const char* const kFps[] = {"24", "30", "60"};
            static const int kFpsV[] = {24, 30, 60};
            int fi = v.fps >= 60 ? 2 : v.fps >= 30 ? 1 : 0;
            if (Pick("Frames per second", "60 is smoothest and costs the most speed", "##CapFps", fi, kFps, 3, 1)) { v.fps = kFpsV[fi]; ch = true; }
            if (v.quality == kQualityCustom)
                ch |= IntSlider("Custom bitrate", "Megabits per second; higher keeps more detail and makes larger files", v.customMbps, 1, 100, dv.customMbps);
            ApexUi::CardGroup("File");
            static const char* const kCont[] = {"MP4", "Crash-safe MP4", "MKV"};
            ch |= Pick("Format", "Crash-safe keeps the video if the game crashes; MKV is made from it when the video stops", "##CapCont", v.container, kCont, 3, dv.container);
            if (v.container == kContainerMkv) ch |= ApexUi::SwitchRow("Keep the MP4 too", &v.keepMp4, "Saves the same video as MP4 next to the MKV", dv.keepMp4);
            static const char* const kCodec[] = {"H.264", "HEVC"};
            int codec = v.hevc ? 1 : 0;
            if (Pick("Compression", "HEVC files are smaller; it needs a newer graphics card", "##CapCodec", codec, kCodec, 2, 0)) { v.hevc = codec == 1; ch = true; }
            ch |= TextRow("File name", kFileNameDesc, v.fileName, 300.0f, true);
            ApexUi::CardGroup("Also in the video");
            ch |= ApexUi::SwitchRow("Include the Apex menu", &v.includeMenu, "When the menu is open, it is in the video", dv.includeMenu);
            ch |= ApexUi::SwitchRow("Include the mouse pointer", &v.includePointer, "The pointer moves in the video as on screen", dv.includePointer);
            ch |= ApexUi::SwitchRow("Save the look's preset too", &v.savePreset, "A copy of the look's .ini next to the video", dv.savePreset);
            ApexUi::CardGroup("Length");
            {
                static const int kClipMB[] = {0, 500, 1024, 2048, 4096, 8192};
                static const char* const kClip[] = {"Off", "500 MB", "1 GB", "2 GB", "4 GB", "8 GB"};
                int clip = -1;
                for (int k = 0; k < 6; k++) if (v.newClipMb == kClipMB[k]) clip = k;
                int pick = clip < 0 ? 0 : clip;
                if (Pick("New clip every", "Saves the clip at this size and records on in a new file", "##CapClip", pick, kClip, 6, 0) || (clip < 0 && pick != 0)) {
                    v.newClipMb = kClipMB[pick];
                    ch = true;
                }
                if (clip < 0) ApexUi::IconNote(IconId::Info, I18n::Trf("Set in the settings file: {} MB", v.newClipMb).c_str());
            }
            ch |= IntSlider("Stop after", "0 records until you stop it", v.stopAfterS, 0, 3600, dv.stopAfterS, "%.0f s");
            ch |= IntSlider("Stop at file size", "0 has no limit", v.stopAtMb, 0, 16000, dv.stopAtMb, "%.0f MB");
            ch |= IntSlider("Warn before the limit", "The timer turns amber and shows the time left; 0 is off", v.warnBeforeS, 0, 600, dv.warnBeforeS, "%.0f s");
          }
            ApexUi::CardGroup("Timer on screen");
            ch |= ApexUi::SwitchRow("Show a recording timer", &v.timer, "Time and file size on screen while recording", dv.timer);
            if (v.timer) {
                static const char* const kPos[] = {"Top left", "Top center", "Top right", "Bottom left", "Bottom right"};
                ch |= Pick("Timer position", "Where the timer sits on the screen", "##CapTimerPos", v.timerPosition, kPos, 5, dv.timerPosition);
                ch |= IntSlider("Timer size", "100 is the size of Apex notes", v.timerSize, 70, 160, dv.timerSize, "%.0f%%");
                ch |= IntSlider("Timer opacity", "Lower lets more of the game show through", v.timerOpacity, 30, 100, dv.timerOpacity, "%.0f%%");
                ch |= ApexUi::SwitchRow("Pulse the red dot", &v.timerBlink, "The dot fades in and out while recording", dv.timerBlink);
                ch |= ApexUi::SwitchRow("Show the file size", &v.timerFileSize, "How large the video is so far", dv.timerFileSize);
                ch |= ApexUi::SwitchRow("Show the stop key", &v.timerStopKey, "The key that stops the video", dv.timerStopKey);
                ch |= ApexUi::SwitchRow("Always show hours", &v.timerHours, "0:02:15 instead of 02:15", dv.timerHours);
            }
            ch |= ApexUi::SwitchRow("Red frame while recording", &v.redFrame, "A thin red border around the screen", dv.redFrame);
            if (v.timer || v.redFrame)
                ch |= ApexUi::SwitchRow("Timer in the video too", &v.timerInVideo, "An open Apex menu is then in the video too", dv.timerInVideo);
            ApexUi::CardGroup("Recorder");
            static const char* const kEngine[] = {"Apex", "Another app"};
            int eng = v.external ? 1 : 0;
            if (Pick("Recorder", "Another app: Apex presses its key (Xbox Game Bar, OBS)", "##CapEngine", eng, kEngine, 2, 0)) { v.external = eng == 1; ch = true; }
            if (v.external) ApexUi::IconNote(IconId::Info, "The other app's key is set on the Shortcuts tab"); // spaced from the row above, as notes are
            ch |= WhenSavedRows(s, play);
            ApexUi::EndAdvanced();
        }
        if (ch) Set(s);
        if (play) PlaySoundFile(); // hear the choice
    }
    ApexUi::EndCard();
}

} // namespace ApexCapture
