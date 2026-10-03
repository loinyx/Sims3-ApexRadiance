// A recording of a few seconds of lighting activity, every line with its clock time (2026-09-30; for players too since the
// Report a problem page, 30/09 evening: features/captures.h).
//
// Its shortcut (F6 with the F-key set) starts, again stops (an on-screen note shows it, apex_gui.cpp); it stops by itself
// after kMaxMs. While it runs, the lighting modules write their detailed log lines in the public build too
// (Recorder::Verbose). The file Captures\<date time> Recording\Recording.txt (with the log, settings and "About this
// capture.txt" beside it) then holds, sorted by time:
//  - the log lines written meanwhile (ApexRadiance_LOG.txt, read from where it was at the start);
//  - the solve journal's notes (rooms solved, sent, held, invalidated with their caller: level_light_share.cpp);
//  - the status lines of the indoor light between stories, Rooms at Night, Faster Room Lighting, the indoor object maps
//    (RoomMapPadding pairings), the furniture counters, the camera's lot and story, the Rooms at Night sliders with what
//    they give furniture, and the lamp marks kept or let through with the messages that flagged light entries
//    (lamp_mark_filter.cpp), each time they change (checked every 100 ms);
//  - [furniture] lines from lot_light_bridge.cpp (every room-mode object part at its first draw and at every change) and
//    [probe] lines from light_probe.cpp (captures, also the automatic ones after a floor change);
//  - at the end, ApexRadiance.toml as it was when the recording started (every setting).
#include "recorder.h"
#include "captures.h"
#include "ui/i18n.h"
#include "hotkeys.h"
#include "apex_log.h"
#include "apex_paths.h"
#include "level_light_share.h"
#include "room_map_padding.h"
#include "lot_light_bridge.h"
#include "night_lighting.h"
#include "room_light_queue.h"
#include "unlit_rooms.h"
#include "lamp_mark_filter.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <filesystem>
#include <format>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr DWORD kMaxMs = 20000, kStatusEveryMs = 100;
bool g_on = false, g_keyWasDown = false;
std::atomic<bool> g_stopRequest{false}, g_cancelRequest{false};
DWORD g_startTick = 0, g_lastStatus = 0;
SYSTEMTIME g_startClock{};
std::uintmax_t g_logStart = 0;
struct Line {
    DWORD tick;
    std::string text;
};
std::vector<Line> g_lines;
std::string g_lastStatusText[8];
std::string g_settingsAtStart; // ApexRadiance.toml as it was when the recording started
size_t g_notes = 0;            // lines added by other modules (Note: furniture, probe), capped
constexpr size_t kMaxNotes = 40000;
// [room] lines have their own cap (30/09, F6 105204: a Brightness drag filled the shared cap with [furniture] lines in 4 s
// and the room tracer stopped before the room that went wrong)
size_t g_roomNotes = 0;
constexpr size_t kMaxRoomNotes = 20000;
std::atomic<bool> g_toggleRequest{false}; // the Report a problem page asked to start / stop
std::string g_saved; // the file just written (the on-screen note)
DWORD g_savedAt = 0;

std::filesystem::path Dir() { return std::filesystem::path(ApexPaths::ApexDirectory()); }

// "hh:mm:ss.mmm" of a tick, from the clock at the start
std::string Clock(DWORD tick) {
    const long long ms = static_cast<long long>(g_startClock.wHour) * 3600000 + g_startClock.wMinute * 60000 + g_startClock.wSecond * 1000 +
                         g_startClock.wMilliseconds + static_cast<int32_t>(tick - g_startTick);
    const long long d = ((ms % 86400000) + 86400000) % 86400000;
    return std::format("{:02}:{:02}:{:02}.{:03}", d / 3600000, d / 60000 % 60, d / 1000 % 60, d % 1000);
}

// room-mode furniture in the last 100 ms (lot_light_bridge) and the night level the furniture part follows
std::string FurnitureLine() {
    float level = -1.0f;
    NightLighting::MenuNightLevel(level);
    return std::format("Furniture (last 100 ms): {} | night level {:.2f}", LotLightBridge::FurnitureDiag(), level);
}

// the story each loaded lot shows (a floor switch shows as a new line; the lot played is the one whose number moves)
std::string CameraLine() {
    uint32_t lots[256];
    int stories[256];
    const int n = LevelLightShare::DisplayLevels(lots, stories, 256);
    std::vector<std::pair<uint32_t, int>> v;
    for (int i = 0; i < n; i++) v.emplace_back(lots[i], stories[i]);
    std::sort(v.begin(), v.end());
    std::string s = "Stories shown (lot:story):";
    for (const auto& [lot, story] : v) s += std::format(" {:08X}:{}", lot, story);
    return s;
}

void Status(DWORD now) {
    const std::string texts[8] = {"Indoor light between stories: " + LevelLightShare::Status(), "Rooms at Night: " + UnlitRooms::Status(),
                                  "Faster Room Lighting: " + RoomLightQueue::StatusText(), "Indoor object maps: " + RoomMapPadding::Status(), FurnitureLine(),
                                  CameraLine(), "Rooms at Night sliders: " + UnlitRooms::SettingsText(),
                                  "Rooms keep their light: " + LampMarkFilter::Status()};
    for (int i = 0; i < 8; i++)
        if (texts[i] != g_lastStatusText[i]) {
            g_lastStatusText[i] = texts[i];
            g_lines.push_back({now, "[status] " + texts[i]});
        }
    for (auto& line : LevelLightShare::TraceRooms(false)) // the rooms whose ambient, state, class or lights changed
        if (g_roomNotes < kMaxRoomNotes) {
            g_roomNotes++;
            g_lines.push_back({now, std::move(line)});
        }
}

void Start() {
    g_on = true;
    g_lines.clear();
    for (auto& s : g_lastStatusText) s.clear();
    g_startTick = GetTickCount();
    GetLocalTime(&g_startClock);
    std::error_code ec;
    g_logStart = std::filesystem::file_size(Dir() / L"ApexRadiance_LOG.txt", ec);
    if (ec) g_logStart = 0;
    g_lastStatus = g_startTick;
    g_notes = 0;
    g_roomNotes = 0;
    g_settingsAtStart.clear();
    {
        std::ifstream in(Dir() / L"ApexRadiance.toml", std::ios::binary);
        if (in) g_settingsAtStart.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    LotLightBridge::FurnitureTraceReset(); // every object is written once again at its first draw
    // every room once, as it is now (30/09: these lines used to be dropped, so a recording showed rooms only once they changed)
    for (auto& line : LevelLightShare::TraceRooms(true))
        if (g_roomNotes < kMaxRoomNotes) {
            g_roomNotes++;
            g_lines.push_back({g_startTick, std::move(line)});
        }
    Status(g_startTick);
    LOG_INFO("[Recorder] Recording started (its shortcut again to stop; stops by itself after 20 s)");
}

void Stop() {
    g_on = false;
    const DWORD end = GetTickCount();
    Status(end);
    LOG_INFO("[Recorder] Recording stopped");
    // the log lines written meanwhile (they carry their own clock: sorted in by it)
    std::vector<std::pair<std::string, std::string>> logLines; // (clock, text)
    {
        std::ifstream in(Dir() / L"ApexRadiance_LOG.txt", std::ios::binary);
        if (in) {
            in.seekg(static_cast<std::streamoff>(g_logStart));
            std::string line;
            while (std::getline(in, line)) {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (line.size() > 13 && line[2] == ':' && line[5] == ':' && line[8] == '.') logLines.emplace_back(line.substr(0, 12), line.substr(13));
            }
        }
    }
    std::vector<std::pair<std::string, std::string>> all; // (clock, text)
    for (const Line& l : g_lines) all.emplace_back(Clock(l.tick), l.text);
    for (auto& [tick, text] : LevelLightShare::JournalSince(g_startTick)) all.emplace_back(Clock(static_cast<DWORD>(tick)), "[solve] " + text);
    for (auto& l : logLines) all.emplace_back(l.first, "[log] " + l.second);
    std::stable_sort(all.begin(), all.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    const std::filesystem::path folder = Captures::NewFolder("Recording");
    const std::string name = folder.filename().string();
    std::ostringstream out;
    {
        out << std::format("Apex Radiance recording: {} to {} ({:.1f} s), {} lines\n", Clock(g_startTick), Clock(end), (end - g_startTick) / 1000.0, all.size());
        out << "[solve] = the solve journal (S ambient step done, W wall pass done, Q sent by Apex, H held by Apex, I / F invalidated, with the caller); "
               "[status] = a status line that changed; [log] = the log\n";
        out << "[furniture] = a room-mode object (furniture) whose drawing changed, one line at its first draw and one at every change: its world position, "
               "the path (A = Apex's indoor-object shader, B = the game's shader turned by Rooms at Night, game = untouched), dark = its rig holds "
               "[NoLight] lights, the 4 rig lights as the game set them (N = [NoLight], F = fill, L = lamp, - = empty) with their colours, the vertex "
               "lights' sum, the ambient cube weight (game -> drawn), the blue kept, and for path A the room light map, its first directional map and the "
               "read scale\n";
        out << "[room] = an indoor room of a loaded lot, once at the start and again when its ambient (+0x110, the colour its walls take; "
               "+0x120), normalisation, solve state, LOD class (solving / shown), light count or shown story changes (checked every 100 ms)\n";
        out << "[probe] = a Light Probe (F7) capture, also the automatic ones taken 1 s and 3 s after any lot changes the story it shows\n\n";
        if (g_notes >= kMaxNotes) out << std::format("(the furniture and probe lines stopped after {} lines)\n\n", kMaxNotes);
        if (g_roomNotes >= kMaxRoomNotes) out << std::format("(the [room] lines stopped after {} lines)\n\n", kMaxRoomNotes);
        for (const auto& [clock, text] : all) out << clock << ' ' << text << '\n';
        out << "\n==== ApexRadiance.toml at the start of the recording ====\n" << g_settingsAtStart << '\n';
    }
    Captures::WriteText(folder / L"Recording.txt", out.str());
    LOG_INFO(std::format("[Recorder] Saved {} lines to Captures\\{}", all.size(), name));
    Captures::Finish(folder, std::format("a recording of {:.0f} s of the lighting", (end - g_startTick) / 1000.0), Captures::CaptureKind::Recording);
    g_saved = name;
    g_savedAt = GetTickCount();
}

} // namespace

namespace Recorder {
bool Active() { return g_on; }
void Note(const std::string& text) {
    if (!g_on || g_notes >= kMaxNotes) return;
    g_notes++;
    g_lines.push_back({GetTickCount(), text});
}
int SecondsRecorded() { return g_on ? static_cast<int>((GetTickCount() - g_startTick) / 1000) : -1; }
void RequestToggle() { g_toggleRequest = true; }
void RequestStop() { g_stopRequest = true; }
void RequestCancel() { g_cancelRequest = true; }
const char* JustSaved() { return !g_saved.empty() && GetTickCount() - g_savedAt < 4000 ? g_saved.c_str() : ""; }
void OnPresent() {
    const bool requested = g_toggleRequest.exchange(false);
    const bool pressed = Hotkeys::Take(Hotkeys::Action::Recorder) || requested;
    const bool stop = g_stopRequest.exchange(false);
    if (g_cancelRequest.exchange(false)) {
        if (g_on) {
            g_on = false;
            g_lines.clear();
            g_settingsAtStart.clear();
            g_saved.clear();
            g_notes = g_roomNotes = 0;
            LOG_INFO("[Recorder] Cancelled: no capture folder created");
            Captures::Notify(I18n::Tr("Recording cancelled. No capture was saved"));
        }
        return; // cancellation wins over a shortcut, Stop or the 20-second deadline
    }
    if (stop) {
        if (g_on) Stop();
        return;
    }
    if (pressed) {
        if (g_on) Stop();
        else Start();
        return;
    }
    if (!g_on) return;
    const DWORD now = GetTickCount();
    if (now - g_lastStatus >= kStatusEveryMs) {
        g_lastStatus = now;
        Status(now);
    }
    if (now - g_startTick >= kMaxMs) Stop();
}
} // namespace Recorder
