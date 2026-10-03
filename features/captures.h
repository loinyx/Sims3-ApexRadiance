#pragma once
// Bug-report captures (Apex Radiance; docs/features/bug-reports.md): the recording (F6), the light capture (F7) and the
// lighting snapshot (F8), for players to send with a bug report.
//
// Every capture gets its own folder Documents\...\Apex Radiance\Captures\<YYYY-MM-DD HH-MM-SS> <kind>\ (never reused: a
// second capture in the same second gets " (2)"), and when it is done the folder also gets a copy of ApexRadiance_LOG.txt,
// ApexRadiance.toml, ApexRadiance_Crash.txt when there is one, and "About this capture.txt" (what it is, the version, how
// to send it). Capture folders are never reused or deleted by themselves; menu removal is reversible. Each start and
// completion shows a centered note on screen (Notify, drawn by the menu module).
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
#include <windows.h>

namespace Captures {

std::filesystem::path Root(); // ...\Apex Radiance\Captures
// A new, empty capture folder: "<date> <time> <kind>" (kind = "Recording", "Light capture", "Lighting snapshot")
std::filesystem::path NewFolder(const char* kind);
// The folder is complete: copies the log, the settings and the crash report into it, writes "About this capture.txt"
// (what = one line on what was captured). The save receipt waits for asynchronous PNG completion and reports failures.
enum class CaptureKind { Generic, Recording, LightCapture, LightingSnapshot };
void Finish(const std::filesystem::path& folder, const std::string& what, CaptureKind kind = CaptureKind::Generic);
// Render-thread capture output. Failed text is retained for RetrySave, without repeating the measurement.
bool WriteText(const std::filesystem::path& file, const std::string& text);
void SetDescription(const std::string& text);
std::string SavedDescription();
std::string SavedDescriptionTitle();
bool SaveDescription(const std::string& text); // add/edit the latest capture's note after it is saved
bool SaveDescription(const std::string& title, const std::string& text);
struct Description { std::string title, text; bool Complete() const { return text.find_first_not_of(" \t\r\n") != std::string::npos; } };
Description ReadDescription(const std::string& folder);
bool SaveFolderDescription(const std::string& folder, const std::string& title, const std::string& text);
struct SaveResult {
    uint64_t serial = 0;
    std::string folder; // direct child of Root: the collection when grouped
    bool saving = false;
    bool failed = false;
};
SaveResult LastSave(); // thread-safe, no disk scan
bool Saving(); // includes PNG encoding, unlike ScreenshotPending
void RetrySave(); // text and metadata retained; a failed screenshot is taken again
// Every capture also gets Screenshot.png, the picture of the next frame (menu closed: as shown, Color filters included;
// menu open: the picture before the Apex menu draws). On by default ([ui] capture_screenshot, set by the menu).
void SetScreenshots(bool on);
bool Screenshots();
bool ScreenshotPending(); // render thread: a screenshot is taken at the next Present (the capture notes are not drawn)
// Captures the finished back buffer (all Apex passes included) into Screenshots, optionally hiding the game's F10 UI for one frame.
bool RequestPlayerScreenshot(bool hideGameUi);
void ObserveGameUiKey(WPARAM vk, bool repeat); // call for game-window key-down messages to track the F10 visibility toggle
// "<date time> Report": only the log, the settings and the crash report (for any problem, crashes included)
void SaveReport();
// ApexRadiance_Crash.txt was written in the last 7 days: its date and time ("2026-09-30 21:50"), else ""
std::string RecentCrash();

// On-screen note at the top center for a few seconds; recording = the live recording note
enum class NoteKind { Info, Success, Warning, Saving, Screenshot, Probe };
void Notify(const std::string& text, int seconds = 5, NoteKind kind = NoteKind::Info);
struct Note {
    std::string text;
    bool visible = false;
    NoteKind kind = NoteKind::Info;
};
Note CurrentNote(); // render thread: the note to draw now

struct Summary {
    int count = 0;         // capture folders
    uint64_t bytes = 0;    // their size
    std::string newest;    // the newest folder's name
};
Summary Scan();            // walks Captures\ (call at most every few seconds)
void OpenFolder();         // Explorer on Captures\ (created if missing)
int DeleteAll();           // removes every capture folder; the number removed

// Capture sessions (user, 30/09: "sessions that put everything in one place"): while one is open, every capture goes into
// Captures\<date time> Session\<time> <kind>\; EndSession adds the log, the settings and "About this session.txt" (the list
// of its captures) to the session folder. A session left open when the game closes stays as it is (its captures are complete).
void BeginSession();
void EndSession();
bool SessionActive();
int SessionCaptures();       // captures saved in the open session
std::string SessionFolder(); // its folder name ("" when none)
std::vector<std::string> SessionItems(); // the captures saved in it so far, oldest first ("22-10-06 Recording")
int SessionSeconds();        // how long it has been open

// One capture (or session) folder, for the list on the Report a problem page (newest first)
struct Entry {
    std::string folder; // the folder's name ("2026-09-30 21-50-12 Recording")
    std::string date;   // "2026-09-30"
    std::string time;   // "21:50:12"
    std::string kind;   // "Recording", "Light capture", "Session", ...
    int items = 0;      // a session: the captures in it
    uint64_t bytes = 0;
    bool described = false;
    std::string title, description; // bounded notes, populated with the existing library scan
};
std::vector<Entry> List();               // walks Captures\ (call at most every few seconds)
void Open(const std::string& folder);    // Explorer on that capture
bool Delete(const std::string& folder);  // removes that capture (only folders inside Captures\)
// Reversible menu removal: move into .Removed, never overwrite or delete. Undo restores the last batch.
bool Remove(const std::string& folder);
int RemoveAll();
bool CanUndoRemoval();
int UndoRemoval();
std::vector<std::string> Files(const std::string& folder); // relative names, read on request only

} // namespace Captures
