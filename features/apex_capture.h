#pragma once
// Screenshots and videos with Apex's effects. Settings live in ApexRadiance.toml: [capture] (sound, notes),
// [capture.screenshot] and [capture.video]; the screenshot key, folder and Hide game UI stay in [ui] (screenshot_*).
// Screenshots go through Captures::RequestPlayerScreenshot (features/captures.cpp), videos through VideoMp4
// (features/video_mp4.cpp). Nothing is hooked until a capture is asked for.
#include "apex_config.h"
#include <d3d9.h>
#include <filesystem>
#include <string>
#include <vector>
#include <windows.h>

namespace ApexCapture {

enum Look : int { kLookWith, kLookWithout, kLookBoth };            // with | without | both (screenshots only)
enum Folder : int { kFolderGame, kFolderApex, kFolderCustom };     // game | apex | custom
enum Quality : int { kQualitySmaller, kQualityBalanced, kQualityBest, kQualityCustom };
enum Container : int { kContainerMp4, kContainerMp4Safe, kContainerMkv };
enum TimerPos : int { kTopLeft, kTopCenter, kTopRight, kBottomLeft, kBottomRight };

inline constexpr const char* kDefaultShotName = "Screenshot_%Date%_%TimeHour%-%TimeMinute%-%TimeSecond%_%TimeMS%"; // 3.2.3's name
inline constexpr int kMaxBurst = 20;
inline constexpr int kMaxBurstMs = 30000;

struct ScreenshotSettings { // [capture.screenshot]; key, folder and hide_game_ui are [ui] screenshot_*
    std::string fileName = kDefaultShotName;
    bool includeMenu = false;
    bool includePointer = false;
    int look = kLookWith;
    bool savePreset = false;
    bool jpg = false;            // file_type png | jpg
    int jpgQuality = 95;         // 1-100
    int sizePercent = 100;       // 10-100
    int burstCount = 1;          // 1-20
    int burstIntervalMs = 500;   // 50-10000, and (burst_count - 1) * interval <= 30 s
};

struct VideoSettings { // [capture.video]
    int folder = kFolderGame;
    std::string customFolder;
    std::string fileName = "Video_%Date%_%TimeHour%-%TimeMinute%-%TimeSecond%";
    bool hideGameUi = false;
    bool includeMenu = false;
    bool includePointer = false;
    int look = kLookWith;        // with | without
    bool savePreset = false;
    int size = 1080;             // 720 | 1080 | 1440 | 2160
    int fps = 30;                // 24 | 30 | 60
    int quality = kQualityBalanced;
    int customMbps = 20;         // 1-200 (quality = custom)
    int container = kContainerMp4Safe;
    bool keepMp4 = false;
    bool hevc = false;           // codec h264 | hevc
    bool gameSound = true;
    int newClipMb = 0;           // 0-100000
    int stopAfterS = 600;        // 0-36000
    int stopAtMb = 4000;         // 0-100000
    int warnBeforeS = 60;        // 0-600
    bool timer = true;
    int timerPosition = kTopCenter;
    int timerSize = 100;         // 70-160 %
    int timerOpacity = 90;       // 30-100 %
    bool timerBlink = true;
    bool timerFileSize = true;
    bool timerStopKey = true;
    bool timerHours = false;
    bool redFrame = false;
    bool timerInVideo = false;
    bool external = false;       // recorder apex | external
    std::string externalKey = "Win+Alt+R";
    ApexConfig::KeyChord recordKey{VK_F8, false, true, false};
};

struct Settings {
    std::string sound;           // [capture] sound: "" (off), "camera" or a .wav path
    bool notes = true;           // [capture] notes: a short note when a capture is saved
    int counter = 1;             // [capture] counter: the next %Counter% value (1-99999999), kept here instead of a file
    ScreenshotSettings shot;
    VideoSettings video;
};

Settings Get();                  // any thread
void Set(const Settings& s);     // clamped; saved with ApexRadiance.toml (debounced)
// [capture] of a root table (missing keys: defaults; every value clamped), and back
Settings FromToml(const toml::table& root);
void ToToml(const Settings& s, toml::table& root);
void LoadFromToml(const toml::table& root); // ApexConfig::LoadSettings: the settings at start (no save)

// The folder a capture goes to, made when missing. FellBack: a custom folder could not be used, the game's is. Failed:
// none could be made. Resolved when a capture starts and kept while the path stays the same.
enum class FolderResult { Ok, FellBack, Failed };
std::filesystem::path CaptureFolder(bool video, FolderResult& r);
std::filesystem::path DefaultFolder(int mode, bool video); // the game or Apex folder (not made)
// A free file in folder from the naming pattern (+ " (2)" on a clash)
std::filesystem::path NewFile(const std::filesystem::path& folder, const std::string& pattern, const char* ext, bool video, int width = 0, int height = 0);
std::wstring PresetFile();       // the ReShade/GShade preset in use ("" when the look is off)
void PlaySoundFile();            // [capture] sound, asynchronous; nothing when off

// The frame copied just before the ReShade/GShade look draws it (screenshot look = without | both)
void WantBefore(bool on);                                         // render thread
void BeforeLook(IDirect3DDevice9* dev, IDirect3DSurface9* src);   // patches/looks_patch.cpp, right before the look runs
bool ReadBefore(IDirect3DDevice9* dev, std::vector<BYTE>& bgr, UINT& w, UINT& h); // render thread at Present: B G R, this frame only

// Video
bool KeyDown(WPARAM vk, bool repeat) noexcept; // the menu's key hook: true when it was the record key (eaten)
void ToggleRecording();                 // any thread: start or stop at the next frame
void Poll();                            // render thread, every frame: hooks the frame once a recording is asked for
void OnGameClosing();                   // window thread (WM_CLOSE, WM_DESTROY, WM_ENDSESSION): stop and wait for the file
bool Recording();
int RecordingSeconds();
bool DrawTimer();                       // the menu's frame (apex_gui Draw): the recording timer and red frame; true when drawn
bool NeedsGradeBeforeOverlay();         // the bootstrap runs Apex colour before the overlay this frame (video grabs there)
// The other recorder's key: "Win+Alt+R", "Ctrl+F9" ... A chord with a modifier; never a bare key or one Windows owns.
bool ParseExternalKey(const std::string& text, UINT& vk, bool& ctrl, bool& shift, bool& alt, bool& win);
bool ExternalKeyAllowed(const std::string& text);

// Image > Capture (apex_gui.cpp): one card per tab. lookOn: a ReShade/GShade look is on (the Look rows show).
// ScreenshotCard returns true when "Take one" was clicked (the menu asks for the picture at its next frame). gameAa:
// the game's multisampled anti-aliasing is on (Record is off and a note under the card says why).
bool ScreenshotCard(bool lookOn);
void VideoCard(bool lookOn, bool gameAa);
// This session's last saved screenshot / video ("" none); set by the writers (any thread)
std::filesystem::path LastSaved(bool video);
void SetLastSaved(bool video, const std::filesystem::path& file);
// Why a video cannot start now (English, shown through I18n::Tr), nullptr when it can (or the other app records);
// the memory check runs at most once a second
const char* CannotRecord(bool gameAa);
// The "Other" folder row: the folder as its description and Change (folder dialog); the typed path is under a
// "Type a folder" arrow. slot: 0 screenshots, 1 videos. True when the folder changed.
bool FolderRow(const char* label, const char* id, std::string& value, int slot, const char* pickTip);

} // namespace ApexCapture
