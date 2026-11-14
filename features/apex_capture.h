#pragma once
// Screenshots and videos with Apex's effects. The player's own settings file, ApexCapture.ini, in
// Documents\Electronic Arts\The Sims 3\Apex Radiance\: plain text, a note above every line, keys named as in GShade.ini's
// [SCREENSHOT] section. Read at first use, re-read when it is saved (ReloadOnChange), written by the Screenshot capture
// and Video recording cards (Settings > Menu). Screenshots go through Captures::RequestPlayerScreenshot (features/captures.cpp),
// videos through VideoMp4 (features/video_mp4.cpp).
#include <d3d9.h>
#include <filesystem>
#include <string>
#include <vector>
#include <windows.h>

namespace ApexCapture {

struct Settings {
    // [GENERAL]
    bool reloadOnChange = true;
    bool showNotice = true;
    std::string soundPath;
    // [SCREENSHOT]
    std::string keyScreenshot = "F8";
    std::string shotPath = "%GameScreenshots%";
    std::string shotNaming = "%Date%_%TimeHour%-%TimeMinute%-%TimeSecond%-%TimeMS%_%PresetName%";
    int counterDigits = 4;
    bool jpg = false;
    int jpegQuality = 95;
    int shotScale = 100;
    bool shotGameUi = false;
    bool shotApexMenu = false;
    bool saveBefore = false;
    bool savePreset = false;
    bool shotMouse = false;        // IncludeMouse: the mouse pointer in the picture
    int batchQuantity = 1;
    int batchInterval = 500;
    // [VIDEO]
    bool videoEnabled = true;
    bool external = false;
    std::string keyRecord = "Shift+F8";
    std::string videoPath = "%GameScreenshots%\\Videos";
    std::string videoNaming = "%Date%_%TimeHour%-%TimeMinute%-%TimeSecond%_%PresetName%";
    bool fragmented = false; // Container=mp4-safe (and mkv: recorded crash-safe, then copied into the MKV)
    bool mkv = false;        // Container=mkv
    bool keepMp4 = false;    // KeepMp4: with mkv, the MP4 stays too
    int newClipMB = 0;       // NewClipEveryMB: a new file every this many MB while recording (0 = one file)
    bool hevc = false;
    int fps = 30;
    bool bitrateMode = false;
    int quality = 80;
    int bitrateMbps = 20;
    int maxHeight = 1080;
    bool audio = true;
    bool videoGameUi = true;
    bool videoApexMenu = false;
    bool videoMouse = false;       // IncludeMouse: the mouse pointer in the video
    int maxLength = 600;
    int maxFileMB = 4000;
    bool recordingDot = true;      // ShowRecordingDot: the on-screen recording timer
    std::string timerPos = "top-center"; // top-left | top-center | top-right | bottom-left | bottom-right
    int timerSize = 100;           // %
    int timerOpacity = 90;         // %
    bool timerBlink = true;
    bool timerHours = false;
    bool timerKey = true;
    bool timerFileSize = true;
    bool timerInVideo = false;
    bool redFrame = false;
    int warnBefore = 60;           // seconds before MaxLength / MaxFileMB; 0 = no warning
    bool savedNote = true;
    std::string externalKey = "Win+Alt+R";
    // [PROFILES] (shared: the same whatever profile is in use)
    std::string profileActive = "Default";
    std::string keyNextProfile = "Ctrl+F8";
    bool profileNote = true;
    // [PROFILE] (this profile's own keys)
    std::string profileKey;     // switches to this profile
    std::string profileShotKey; // one picture with this profile, without switching to it
};

Settings Get();                    // any thread; loads the file at first use (writes it with the defaults when missing)
void Set(const Settings& s);       // the menu: applies at once, written to the file half a second later
std::filesystem::path File();      // ...\Apex Radiance\ApexCapture.ini
void Tick();                       // render thread, every Present: file watch, pending write, sync with [ui] screenshot_*

// "%GameScreenshots%\Videos" -> a folder that exists (created). Empty + why when it cannot be made.
std::filesystem::path ResolveFolder(const std::string& path, std::string& why);
// A free file in folder from the naming pattern (+ " (2)" on a clash). counter: the %Counter% value
std::filesystem::path NewFile(const std::filesystem::path& folder, const std::string& pattern, const char* ext);
std::wstring PresetFile();         // the GShade preset in use ("" when the look is off)
void PlaySoundFile();              // SoundPath, asynchronous; nothing when empty
// GShade numbers ("107,0,1,0" = Shift+NumPlus) or Apex's text ("Shift+F8"); false = not a key
bool ParseKey(const std::string& text, UINT& vk, bool& ctrl, bool& shift, bool& alt, bool& win);

// The "before" picture (SaveBeforeShot): the frame copied just before the GShade look draws it
void WantBefore(bool on);                                         // render thread
void BeforeLook(IDirect3DDevice9* dev, IDirect3DSurface9* src);   // gshade_looks.cpp, right before the look runs
bool ReadBefore(IDirect3DDevice9* dev, std::vector<BYTE>& bgr, UINT& w, UINT& h); // render thread at Present: B G R, this frame only

// Video
bool KeyDown(WPARAM vk, bool repeat) noexcept; // the menu's key hook: true when it was the record key (eaten)
void ToggleRecording();                 // any thread: start or stop at the next frame
bool Recording();
int RecordingSeconds();
bool DrawTimer();                  // the menu's frame (apex_gui Draw): the recording timer and red frame; true when drawn
bool NeedsGradeBeforeOverlay();         // the bootstrap runs Apex colour before the overlay this frame (video grabs there)

// Settings > Menu (apex_gui.cpp): rows inside "Screenshot capture", and the "Video recording" card's rows and file group
void ScreenshotRows();
void VideoRows();
void FileRows();
// A folder row in Apex style: the folder as its description, Change (folder dialog) and Open; the typed path is under a
// "Type a folder" arrow. slot: 0 screenshots, 1 videos. True when the folder changed.
bool FolderRow(const char* label, const char* id, std::string& value, int slot, const char* pickTip);

// Capture profiles (OBS style): every value of ApexCapture.ini except the keys is kept per profile, one file each in
// ...\Apex Radiance\Capture profiles\<name>.ini. ApexCapture.ini always holds the profile in use; edits there and in
// the menu are saved into that profile at once. Switch with the menu, KeyNext, or a profile's own Key; ShotKey takes
// one picture with a profile without switching.
std::vector<std::string> Profiles();          // names, sorted
bool SwitchProfile(const std::string& name);  // render thread (the menu)
void ProfileCard();                           // Settings > Menu, above "Screenshot capture"

} // namespace ApexCapture
