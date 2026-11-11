// A recording of a few seconds of lighting activity, every line with its clock time (2026-09-30; for players too since the
// Report a problem page, 30/09 evening: features/captures.h).
//
// Its shortcut (F6 with the F-key set) starts, again asks to stop (an on-screen note shows it, apex_gui.cpp): it goes on for
// kSettleMs, so the screenshot and the last lines show the settled light, and saves; a further press saves at once. It
// stops by itself after kMaxMs. While it runs, the lighting modules write their detailed log lines in the public build too
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
//  - [camera] lines when the camera changes, [effect] lines when the features on change, a [ran] line a second (what ran),
//    the last [cube] line of each light probe and a [settle] line on the wait;
//  - at the end, ApexRadiance.toml as it was when the recording started (every setting).
// In developer mode the folder also gets Video.mjpeg (capture_video.h), Cubes\ (the last cube of each light probe, its 6 faces
// side by side) and what the frames after the recording show (capture_burst.h).
#include "recorder.h"
#include "captures.h"
#include "ui/i18n.h"
#include "hotkeys.h"
#include "apex_log.h"
#include "apex_util.h"
#include "apex_paths.h"
#include "level_light_share.h"
#include "room_map_padding.h"
#include "lot_light_bridge.h"
#include "night_lighting.h"
#include "room_light_queue.h"
#include "unlit_rooms.h"
#include "lamp_mark_filter.h"
#include "screen_watch.h"
#include "apex_memory.h"
#include "dev_switches.h"
#include "outdoor_delta.h"
#include "post_scene.h"
#include "capture_video.h"
#include "capture_burst.h"
#include "no_scene_trace.h"
#include "lot_lighting_motion.h"
#include "patch_base.h"
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr DWORD kMaxMs = 60000, kStatusEveryMs = 100, kRanEveryMs = 1000;
// Fixed: solves and cube writes go on with no lamp edited, so waiting for them to stop would never end
constexpr DWORD kSettleMs = 8000;
bool g_on = false, g_keyWasDown = false;
std::atomic<bool> g_stopRequest{false}, g_cancelRequest{false};
DWORD g_startTick = 0, g_lastStatus = 0, g_lastRan = 0, g_settleAt = 0; // g_settleAt: when the stop key was pressed
SYSTEMTIME g_startClock{};
std::uintmax_t g_logStart = 0;
struct Line {
    DWORD tick;
    std::string text;
};
std::vector<Line> g_lines;
std::string g_lastStatusText[10];
std::string g_lastCamera, g_lastEffects;
int g_cameraSeen = 0, g_cameraMirrored = 0; // status checks that had a camera, and of those the mirrored ones
bool g_sceneHooked = false;                 // the empty scene effect and the camera request are on
std::map<const char*, long> g_counts;       // Count, since the last [ran] line
std::string g_settingsAtStart; // ApexRadiance.toml as it was when the recording started
size_t g_notes = 0;            // lines added by other modules (Note: furniture, probe), capped
constexpr size_t kMaxNotes = 40000;
// [room] lines have their own cap (30/09, F6 capture: a Brightness drag filled the shared cap with [furniture] lines in 4 s
// and the room tracer stopped before the room that went wrong)
size_t g_roomNotes = 0;
constexpr size_t kMaxRoomNotes = 60000;
std::atomic<bool> g_toggleRequest{false}; // the Report a problem page asked to start / stop
std::string g_saved; // the file just written (the on-screen note)
DWORD g_savedAt = 0;

std::filesystem::path Dir() { return std::filesystem::path(ApexPaths::ApexDirectory()); }

// ---- Light update trace (measures what happens when the lights update) ----
// The lamps the player edited during the recording (lamp_mark_filter.cpp), the end of every room solve (the solve journal's
// 'E' notes: the room shows its new maps from the next frame) and the screen pixels of ScreenWatch, summed up per lamp edit
// at the top of Recording.txt ("Light updates"), the pixels also in "Screen pixels.csv". A room's first 'E' after an edit is
// when its new light appears (class 0 = the quick pass), the last one its refinement; an edit's window ends at the next
// edit or when its lot shows another story.
struct Edit {
    DWORD tick;
    uint32_t lot;
    int story; // the story whose tree level saw it first (the game marks the rooms of the stories its light reaches)
    bool on, moved;
};
std::mutex g_editMx;
std::vector<Edit> g_edits;
constexpr DWORD kBurstGapMs = 400; // edits closer than this are one ("all the lights" switches each lamp a few ms apart)
// The story each lot shows, at the start and at every change (the 100 ms status check). A lamp edit's window ends when its
// lot shows another story or leaves the view (a recording: the third edit counted the solves of a trip to the map view)
struct ViewSnap {
    DWORD tick;
    std::map<uint32_t, int> stories;
};
std::vector<ViewSnap> g_views;
constexpr int kNotShown = -1000;
// The terrain chunks whose smoothed light map changed (LightmapSmooth, render thread): the ground's new light
struct GroundShown {
    DWORD tick;
    int ix, iz;
    bool together;
};
std::vector<GroundShown> g_ground;
// The last write of each light probe's cube (under g_editMx). The last, not the first ones: a recording has hundreds of
// writes and the ones that count are those of the settled light
struct CubeNote {
    DWORD tick = 0;
    int writes = 0;
    std::string text;
    std::vector<unsigned char> strip; // (developer mode) its 6 faces side by side, B G R rows
    unsigned side = 0;
};
std::map<std::pair<uintptr_t, std::string>, CubeNote> g_cubes;

// Runs at the scene's end while recording: its count is the frames that got there (the [ran] lines), and with it the camera
// vote has the post-scene draw hooks whatever is on in the menu
void SceneBoundary(IDirect3DDevice9*) {}
void SceneHook(bool on) {
    if (on == g_sceneHooked) return;
    g_sceneHooked = on;
    if (on) PostScene::Add(PostScene::kRecording, SceneBoundary, false);
    else PostScene::Remove(SceneBoundary);
    PostScene::WantCamera(on);
}

// (development builds, release_debug_buffers) Once a recording is saved or cancelled its lines (up to 60000 strings), views,
// ground notes, lamp edits and the settings copy are given back; before, they stayed until the next recording started.
// Nothing reads them between recordings (Note and the trackers only add while one runs).
void ReleaseBuffers() {
    OutdoorDelta::SceneEnd({}); // (a cancelled recording drops its scene; after a save it was already moved: nothing left)
    CaptureVideo::End({});      // (the same for its video)
    NoSceneTrace::End();        // (and its no-scene frame trace)
    SceneHook(false);
    if (!DevSwitches::ReleaseDebugBuffers() || g_on) return;
    using namespace ApexMemory::Est;
    uint64_t bytes = Vec(g_lines, [](const Line& l) { return Str(l.text); }) + Vec(g_views, [](const ViewSnap& v) { return Tree(v.stories); }) + Vec(g_ground) +
                     Str(g_settingsAtStart);
    std::vector<Line>().swap(g_lines);
    std::vector<ViewSnap>().swap(g_views);
    std::vector<GroundShown>().swap(g_ground);
    std::string().swap(g_settingsAtStart);
    {
        std::lock_guard<std::mutex> lk(g_editMx);
        bytes += Vec(g_edits);
        std::vector<Edit>().swap(g_edits);
        g_cubes.clear();
    }
    // the tracers' last states: a new recording empties them at its start (FurnitureTraceReset, TraceRooms(true)) and nothing
    // else reads them, so they need not wait for it
    bytes += LotLightBridge::FurnitureTraceRelease() + LevelLightShare::TraceRoomsRelease();
    if (bytes) ApexMemory::NoteReleased("Recorder (F6): the recording's lines after it was saved or cancelled", bytes);
}
void MeasureMemory() {
    static ApexMemory::Cadence cadence;
    if (!cadence.Due()) return;
    using namespace ApexMemory::Est;
    static auto* sLines = ApexMemory::Register("Recorder", "recording lines, views and notes (F6)");
    uint64_t bytes = Vec(g_lines, [](const Line& l) { return Str(l.text); }) + Vec(g_views, [](const ViewSnap& v) { return Tree(v.stories); }) + Vec(g_ground) +
                     Str(g_settingsAtStart);
    {
        std::lock_guard<std::mutex> lk(g_editMx);
        bytes += Vec(g_edits);
    }
    ApexMemory::Set(sLines, bytes, g_lines.size());
}
std::string Clock(DWORD tick);

std::string Seconds(DWORD from, DWORD at) { return std::format("{:+.2f} s", static_cast<int32_t>(at - from) / 1000.0); }
float Luma(const unsigned char* c) { return 0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2]; }
bool Before(DWORD a, DWORD b) { return static_cast<int32_t>(a - b) < 0; }
DWORD Earliest(DWORD a, DWORD b) { return !a || Before(b, a) ? b : a; }
DWORD Latest(DWORD a, DWORD b) { return !a || Before(a, b) ? b : a; }

int StoryShown(uint32_t lot, DWORD at) {
    int story = kNotShown;
    for (const ViewSnap& v : g_views) {
        if (Before(at, v.tick)) break;
        const auto it = v.stories.find(lot);
        story = it == v.stories.end() ? kNotShown : it->second;
    }
    return story;
}

// One screen point over a lamp edit's window: from what to what, when it began to change and settled, and whether it
// reached its new value and then left it again ("right, then wrong, then right")
struct PointStory {
    bool changed = false;
    float from = 0, to = 0;
    DWORD first = 0, settled = 0;
    float worstAway = 0; // the farthest it went from its final value after reaching it once
    DWORD worstAt = 0;
    int reversals = 0;
    bool wrong() const { return worstAway > 6.0f; }
};
PointStory TellPoint(const std::vector<ScreenWatch::Sample>& samples, int point, DWORD t0, DWORD t1) {
    PointStory p;
    std::vector<std::pair<DWORD, float>> v;
    float base = -1.0f;
    for (const auto& s : samples) {
        if (Before(s.tick, t0)) {
            base = Luma(s.rgb[point]);
            continue;
        }
        if (!Before(s.tick, t1)) break;
        v.emplace_back(s.tick, Luma(s.rgb[point]));
    }
    if (v.empty()) return p;
    if (base < 0.0f) base = v.front().second;
    p.from = base;
    p.to = v.back().second;
    constexpr float kNoise = 3.0f, kStep = 6.0f;
    for (const auto& [t, l] : v)
        if (std::fabs(l - base) > kNoise) {
            p.first = t;
            p.changed = true;
            break;
        }
    if (!p.changed) return p;
    p.settled = p.first;
    for (const auto& [t, l] : v)
        if (std::fabs(l - p.to) > kNoise) p.settled = t;
    // after the first time it is at its final value, how far it leaves it again
    bool reached = false;
    for (const auto& [t, l] : v) {
        if (!Before(t, p.first) && !reached) {
            reached = std::fabs(l - p.to) <= kNoise;
            continue;
        }
        if (reached && std::fabs(l - p.to) > p.worstAway) {
            p.worstAway = std::fabs(l - p.to);
            p.worstAt = t;
        }
    }
    // direction changes larger than kStep (hysteresis)
    int dir = 0;
    float pivot = v.front().second;
    for (const auto& [t, l] : v) {
        if (dir >= 0 && l < pivot - kStep) {
            if (dir > 0) p.reversals++;
            dir = -1;
            pivot = l;
        } else if (dir <= 0 && l > pivot + kStep) {
            if (dir < 0) p.reversals++;
            dir = 1;
            pivot = l;
        } else if ((dir > 0 && l > pivot) || (dir < 0 && l < pivot)) pivot = l;
    }
    return p;
}

// A grid point's place on the screen in percent ("x 26% y 38%")
std::string GridPlace(int i) {
    const int w = std::max(1, ScreenWatch::Width()), h = std::max(1, ScreenWatch::Height());
    return std::format("x {}% y {}%", (ScreenWatch::PointX(i) * 100 + w / 2) / w, (ScreenWatch::PointY(i) * 100 + h / 2) / h);
}

std::string LightUpdates(DWORD start, DWORD end) {
    std::vector<Edit> edits;
    {
        std::lock_guard<std::mutex> lk(g_editMx);
        edits = g_edits;
    }
    std::sort(edits.begin(), edits.end(), [](const Edit& a, const Edit& b) { return Before(a.tick, b.tick); });
    const auto events = LevelLightShare::JournalEventsSince(start);
    const auto samples = ScreenWatch::Samples();
    constexpr int kMid = ScreenWatch::kColumn / 2;
    std::string s = "==== Light updates (per lamp edit: when each room showed its new light, and the screen pixels) ====\n";
    if (!samples.empty()) {
        std::string rows;
        for (int i = 0; i < ScreenWatch::kColumn; i++) rows += std::format("{}{}", i ? ", " : "", ScreenWatch::PointY(i));
        s += std::format("Screen points, read before the Apex menu and notices are drawn (every frame in \"Screen pixels.csv\"): a column at x {}, "
                         "rows {} around {}, and a grid of {} x {} over the screen\n",
                         ScreenWatch::PointX(kMid), rows,
                         ScreenWatch::ColumnAtCentre() ? "the screen's centre (the mouse was on the Apex menu or outside the game)" : "the mouse",
                         ScreenWatch::kGridX, ScreenWatch::kGridY);
    } else
        s += "Screen points: none (the back buffer could not be read)\n";
    s += "Class 0 = the quick pass (the light shown first), 2 = its refinement\n";
    if (edits.empty()) return s + "No lamp was edited during the recording.\n\n";
    // bursts of edits
    std::vector<std::pair<size_t, size_t>> bursts; // [first, last] indices
    for (size_t i = 0; i < edits.size(); i++)
        if (bursts.empty() || edits[i].tick - edits[bursts.back().second].tick > kBurstGapMs) bursts.emplace_back(i, i);
        else bursts.back().second = i;
    const int mouseRow = ScreenWatch::PointY(kMid);
    for (size_t b = 0; b < bursts.size(); b++) {
        const Edit& e0 = edits[bursts[b].first];
        const DWORD t0 = e0.tick;
        DWORD t1 = b + 1 < bursts.size() ? edits[bursts[b + 1].first].tick : end;
        // the window ends when the lot shows another story (or leaves the view)
        const int storyThen = StoryShown(e0.lot, t0);
        DWORD viewAt = 0;
        int storyAfter = storyThen;
        for (const ViewSnap& v : g_views) {
            if (!Before(t0, v.tick) || !Before(v.tick, t1)) continue;
            const auto it = v.stories.find(e0.lot);
            const int now = it == v.stories.end() ? kNotShown : it->second;
            if (now != storyThen) {
                viewAt = v.tick;
                storyAfter = now;
                break;
            }
        }
        if (viewAt) t1 = viewAt;
        int on = 0, off = 0, moved = 0;
        std::map<int, int> perStoryEdits;
        for (size_t i = bursts[b].first; i <= bursts[b].second; i++) {
            const Edit& e = edits[i];
            if (e.moved) moved++;
            else if (e.on) on++;
            else off++;
            perStoryEdits[e.story]++;
        }
        std::string where;
        for (const auto& [story, n] : perStoryEdits) where += std::format("{}story {}: {}", where.empty() ? "" : ", ", story, n);
        s += std::format("\n-- Lamp edit at {} (lot {:08X}, showing story {}): {} lamp light(s), {} switched on, {} off, {} moved or changed (seen on {})\n", Clock(t0),
                         e0.lot, storyThen == kNotShown ? std::string("none") : std::to_string(storyThen), bursts[b].second - bursts[b].first + 1, on, off,
                         moved, where);
        if (viewAt)
            s += std::format("   the view changed at {} (the lot shows {}): what came after is not counted for this edit\n", Seconds(t0, viewAt),
                             storyAfter == kNotShown ? std::string("no story, out of the view") : std::format("story {}", storyAfter));
        // the rooms of that lot that ended a solve in the window: (tick, class just shown)
        // (a solve ending before the room was invalidated or sent in this window was started for an earlier change)
        struct RoomTimes {
            std::vector<std::pair<DWORD, int>> ends;
            DWORD asked = 0;
            bool older = false, merged = false;
        };
        std::map<std::pair<int, int>, RoomTimes> rooms; // (story, room)
        for (const auto& ev : events) {
            if (ev.lot != e0.lot || Before(ev.tick, t0) || !Before(ev.tick, t1)) continue;
            RoomTimes& r = rooms[{ev.level, ev.id}];
            if ((ev.event == 'I' || ev.event == 'Q') && !r.asked) r.asked = ev.tick;
            if (ev.event == 'E') {
                if (r.asked) r.ends.emplace_back(ev.tick, ev.shown); // noted after the finalize: the class it now shows
                else r.older = true;
            }
            r.merged = r.merged || ev.merged;
        }
        int olderOnly = 0;
        for (const auto& [key, r] : rooms)
            if (r.older && r.ends.empty()) olderOnly++;
        struct StorySpan {
            int rooms = 0;
            DWORD firstLight = 0, allLit = 0, lastSolve = 0;
        };
        std::map<int, StorySpan> perStory;
        DWORD first = 0, allLit = 0, last = 0, atriumLitMin = 0, atriumLitMax = 0, atriumLastMin = 0, atriumLastMax = 0;
        int shown = 0, twice = 0, quickFirst = 0;
        std::string again, atrium;
        for (const auto& [key, r] : rooms) {
            if (r.ends.empty()) continue;
            shown++;
            const DWORD a = r.ends.front().first, z = r.ends.back().first;
            if (r.ends.front().second == 0) quickFirst++;
            first = Earliest(first, a);
            allLit = Latest(allLit, a);
            last = Latest(last, z);
            StorySpan& st = perStory[key.first];
            st.rooms++;
            st.firstLight = Earliest(st.firstLight, a);
            st.allLit = Latest(st.allLit, a);
            st.lastSolve = Latest(st.lastSolve, z);
            if (r.ends.size() > 1) {
                twice++;
                if (again.size() < 600) {
                    again += std::format("{}room {} story {} x{} (", again.empty() ? "" : "; ", key.second, key.first, r.ends.size());
                    for (size_t k = 0; k < r.ends.size(); k++) again += std::format("{}{} class {}", k ? ", " : "", Seconds(t0, r.ends[k].first), r.ends[k].second);
                    again += ")";
                }
            }
            if (r.merged) {
                atriumLitMin = Earliest(atriumLitMin, a);
                atriumLitMax = Latest(atriumLitMax, a);
                atriumLastMin = Earliest(atriumLastMin, z);
                atriumLastMax = Latest(atriumLastMax, z);
                atrium += std::format("{}room {} story {} {} (last {})", atrium.empty() ? "" : ", ", key.second, key.first, Seconds(t0, a), Seconds(t0, z));
            }
        }
        if (!shown) s += "   no room of that lot ended a solve in this window\n";
        else {
            s += std::format("   rooms that showed new light: {}, all of them by {} (the first {}), {} with the quick pass first; the last solve {}\n", shown,
                             Seconds(t0, allLit), Seconds(t0, first), quickFirst, Seconds(t0, last));
            for (const auto& [story, span] : perStory)
                s += std::format("   story {}: {} room(s), new light {} .. {}, last solve {}\n", story, span.rooms, Seconds(t0, span.firstLight), Seconds(t0, span.allLit),
                                 Seconds(t0, span.lastSolve));
            if (perStory.size() > 1) {
                DWORD litMin = 0, litMax = 0, lastMin = 0, lastMax = 0;
                for (const auto& [story, span] : perStory) {
                    litMin = Earliest(litMin, span.allLit);
                    litMax = Latest(litMax, span.allLit);
                    lastMin = Earliest(lastMin, span.lastSolve);
                    lastMax = Latest(lastMax, span.lastSolve);
                }
                s += std::format("   between stories: each story had all its new light by {} .. {}: {:.2f} s apart; their last solves {:.2f} s apart\n",
                                 Seconds(t0, litMin), Seconds(t0, litMax), static_cast<int32_t>(litMax - litMin) / 1000.0,
                                 static_cast<int32_t>(lastMax - lastMin) / 1000.0);
            }
            if (!atrium.empty())
                s += std::format("   atrium rooms, new light (last solve): {}; new light {:.2f} s apart, last solves {:.2f} s apart\n", atrium,
                                 static_cast<int32_t>(atriumLitMax - atriumLitMin) / 1000.0, static_cast<int32_t>(atriumLastMax - atriumLastMin) / 1000.0);
            if (twice) s += std::format("   solved more than once: {} room(s): {}\n", twice, again);
        }
        if (olderOnly) s += std::format("   {} room(s) only finished a solve started before this edit (not counted)\n", olderOnly);
        // the objects (furniture and other room-mode parts): their drawing changed (the [furniture] lines)
        {
            int n = 0;
            DWORD a = 0, z = 0;
            for (const Line& l : g_lines)
                if (!Before(l.tick, t0) && Before(l.tick, t1) && l.text.rfind("[furniture] changed", 0) == 0) {
                    n++;
                    a = Earliest(a, l.tick);
                    z = Latest(z, l.tick);
                }
            if (n) s += std::format("   objects: {} object part(s) changed their light, {} .. {}\n", n, Seconds(t0, a), Seconds(t0, z));
            else s += "   objects: no object part changed its light in this window\n";
        }
        // the ground (terrain chunks, their smoothed maps and the world atlas that lots, floors and fences read)
        {
            std::vector<const GroundShown*> g;
            for (const GroundShown& x : g_ground)
                if (!Before(x.tick, t0) && Before(x.tick, t1)) g.push_back(&x);
            if (g.empty()) s += "   ground: no terrain chunk showed new light in this window\n";
            else {
                const bool oneFrame = std::all_of(g.begin(), g.end(), [&](const GroundShown* x) { return x->tick == g.front()->tick; });
                const bool together = std::all_of(g.begin(), g.end(), [](const GroundShown* x) { return x->together; });
                std::string list;
                for (const GroundShown* x : g)
                    if (list.size() < 300) list += std::format("{}({},{}) {}", list.empty() ? "" : ", ", x->ix, x->iz, Seconds(t0, x->tick));
                s += std::format("   ground: {} terrain chunk(s) showed new light, {} .. {}{}: {}\n", g.size(), Seconds(t0, g.front()->tick), Seconds(t0, g.back()->tick),
                                 oneFrame ? (together ? ", in one frame (held for the switch)" : ", in one frame") : (together ? ", held for the switch" : ", one by one"), list);
            }
        }
        // the screen: the column around the mouse (or the centre)...
        if (samples.empty()) continue;
        DWORD aboveSettled = 0, belowSettled = 0;
        for (int i = 0; i < ScreenWatch::kColumn; i++) {
            const PointStory p = TellPoint(samples, i, t0, t1);
            const int dy = ScreenWatch::PointY(i) - mouseRow;
            const std::string label = dy == 0 ? std::string(ScreenWatch::ColumnAtCentre() ? "at the centre" : "at the mouse") : std::format("{:+} px", dy);
            if (!p.changed) {
                s += std::format("   pixel {}: unchanged ({:.0f})\n", label, p.from);
                continue;
            }
            s += std::format("   pixel {}: {:.0f} -> {:.0f}, changing from {}, settled {}{}{}\n", label, p.from, p.to, Seconds(t0, p.first), Seconds(t0, p.settled),
                             p.reversals ? std::format(", {} reversal(s)", p.reversals) : std::string(),
                             p.wrong() ? std::format(", RIGHT THEN WRONG: it left its final value by {:.0f} at {}", p.worstAway, Seconds(t0, p.worstAt)) : std::string());
            if (dy < 0) aboveSettled = Latest(aboveSettled, p.settled);
            if (dy > 0) belowSettled = Latest(belowSettled, p.settled);
        }
        if (aboveSettled && belowSettled)
            s += std::format("   on screen: above the {} settled {}, below it {}: {:.2f} s apart\n", ScreenWatch::ColumnAtCentre() ? "centre" : "mouse",
                             Seconds(t0, aboveSettled), Seconds(t0, belowSettled), std::fabs(static_cast<int32_t>(aboveSettled - belowSettled) / 1000.0));
        // ... and the grid over the screen
        int changed = 0, reversing = 0, wrong = 0;
        DWORD settledMin = 0, settledMax = 0;
        std::string list, wrongList;
        for (int i = ScreenWatch::kColumn; i < ScreenWatch::kGrid; i++) {
            const PointStory p = TellPoint(samples, i, t0, t1);
            if (!p.changed) continue;
            changed++;
            settledMin = Earliest(settledMin, p.settled);
            settledMax = Latest(settledMax, p.settled);
            if (p.reversals) reversing++;
            if (p.wrong()) {
                wrong++;
                wrongList += std::format("{}{} (settled {}, left it by {:.0f} at {})", wrongList.empty() ? "" : "; ", GridPlace(i), Seconds(t0, p.settled), p.worstAway,
                                         Seconds(t0, p.worstAt));
            }
            list += std::format("{}{} {:.0f} -> {:.0f} settled {}{}", list.empty() ? "" : "; ", GridPlace(i), p.from, p.to, Seconds(t0, p.settled),
                                p.reversals ? std::format(" ({} reversal(s))", p.reversals) : std::string());
        }
        if (!changed) s += std::format("   screen grid ({} points): none changed\n", ScreenWatch::kGrid - ScreenWatch::kColumn);
        else {
            s += std::format("   screen grid ({} points): {} changed, settled {} .. {}; {} with reversals; RIGHT THEN WRONG at {}{}\n",
                             ScreenWatch::kGrid - ScreenWatch::kColumn, changed, Seconds(t0, settledMin), Seconds(t0, settledMax), reversing, wrong,
                             wrong ? ": " + wrongList : std::string());
            s += "   grid points that changed: " + list + "\n";
        }
    }
    return s + "\n";
}

std::string PixelCsv(DWORD start) {
    const auto samples = ScreenWatch::Samples();
    if (samples.empty()) return {};
    std::string s = "elapsed_ms,frame_ms,mirrored,video_frame";
    const int mouseRow = ScreenWatch::PointY(ScreenWatch::kColumn / 2);
    const int w = std::max(1, ScreenWatch::Width()), h = std::max(1, ScreenWatch::Height());
    // (no lamp was chosen: no ground columns)
    const bool ground = std::any_of(samples.begin(), samples.end(), [](const ScreenWatch::Sample& smp) { return smp.ground; });
    const int points = ground ? ScreenWatch::kPoints : ScreenWatch::kGrid;
    for (int i = 0; i < points; i++) {
        // the column by its row offset (luma+0 = the mouse or the centre), the grid by its place in percent (luma_x26y38)
        // (the ground points by their distance from the lamp along its line: luma_ground+3 = 3 m past it)
        const std::string name = i < ScreenWatch::kColumn ? std::format("{:+}", ScreenWatch::PointY(i) - mouseRow)
                                 : i < ScreenWatch::kGrid ? std::format("_x{}y{}", (ScreenWatch::PointX(i) * 100 + w / 2) / w, (ScreenWatch::PointY(i) * 100 + h / 2) / h)
                                                          : std::format("_ground{:+.0f}", GroundProfile::WatchT(i - ScreenWatch::kGrid));
        s += std::format(",luma{0},r{0},g{0},b{0}", name);
    }
    s += "\n";
    for (const auto& smp : samples) {
        s += std::format("{},{:.1f},{},{}", static_cast<int32_t>(smp.tick - start), smp.frameMs, smp.mirrored ? 1 : 0, smp.videoFrame);
        for (int i = 0; i < points; i++) {
            if (i >= ScreenWatch::kGrid && !smp.ground) s += ",,,,";
            else s += std::format(",{:.1f},{},{},{}", Luma(smp.rgb[i]), smp.rgb[i][0], smp.rgb[i][1], smp.rgb[i][2]);
        }
        s += "\n";
    }
    return s;
}

// The frame times of the samples, for the top of Recording.txt
std::string FrameTimes() {
    std::vector<float> ms;
    for (const auto& smp : ScreenWatch::Samples())
        if (smp.frameMs > 0.0f) ms.push_back(smp.frameMs);
    if (ms.empty()) return "Frame time: not measured\n";
    std::sort(ms.begin(), ms.end());
    return std::format("Frame time over {} samples: median {:.1f} ms, p95 {:.1f} ms, longest {:.1f} ms\n", ms.size(), ms[ms.size() / 2], ms[ms.size() * 95 / 100], ms.back());
}

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

// the story each loaded lot shows (a floor switch shows as a new line; the lot played is the one whose number moves),
// also kept for the light update summary (g_views)
std::string CameraLine(DWORD now) {
    uint32_t lots[256];
    int stories[256];
    const int n = LevelLightShare::DisplayLevels(lots, stories, 256);
    std::map<uint32_t, int> v;
    for (int i = 0; i < n; i++) v[lots[i]] = stories[i];
    std::string s = "Stories shown (lot:story):";
    for (const auto& [lot, story] : v) s += std::format(" {:08X}:{}", lot, story);
    if (g_views.empty() || g_views.back().stories != v) g_views.push_back({now, std::move(v)});
    return s;
}

// The camera of the frame just drawn, when it changes. The eye is the game's own: the view-projection may be the mirrored copy
void CameraNote(DWORD now) {
    float vp[4][4], eye[3] = {};
    if (!PostScene::RecentCameraViewProj(vp)) return;
    const bool mirrored = vp[1][1] < 0.0f;
    g_cameraSeen++;
    if (mirrored) g_cameraMirrored++;
    std::string s = std::format("[camera] {} near {:.4f}", mirrored ? "mirrored" : "plain", PostScene::CameraNear());
    if (LotLightingMotion::CameraEye(eye)) s += std::format(" eye {:.3f} {:.3f} {:.3f}", eye[0], eye[1], eye[2]);
    s += " vp";
    for (const auto& row : vp) s += std::format(" {:.5f} {:.5f} {:.5f} {:.5f}", row[0], row[1], row[2], row[3]);
    if (s == g_lastCamera) return;
    g_lastCamera = s;
    g_lines.push_back({now, std::move(s)});
}

// The features that are on, when that changes
void EffectNote(DWORD now) {
    std::string s;
    for (const auto& p : PatchManager::Get().GetPatches())
        if (p->IsEnabled()) s += (s.empty() ? "" : ", ") + p->GetName();
    if (s == g_lastEffects) return;
    g_lastEffects = s;
    g_lines.push_back({now, "[effect] on: " + s});
}

const char* EffectName(int order) {
    switch (order) {
    case PostScene::kAmbientOcclusion: return "ambient occlusion";
    case PostScene::kEdgeSmoothing: return "edge smoothing";
    case PostScene::kDepthBlur: return "depth blur";
    case PostScene::kPicture: return "colour";
    case PostScene::kRecording: return "frames at the scene's end";
    case 0: return "bloom alpha probe";
    default: return "high precision scene";
    }
}

// What ran since the last line
void RanNote(DWORD now) {
    std::string s;
    for (const auto& [order, n] : PostScene::TakeRan()) s += std::format("{}{} {}", s.empty() ? "" : ", ", EffectName(order), n);
    for (const auto& [what, n] : g_counts) s += std::format("{}{} {}", s.empty() ? "" : ", ", what, n);
    g_counts.clear();
    g_lines.push_back({now, std::format("[ran] in the last {:.1f} s: {}", (now - g_lastRan) / 1000.0, s.empty() ? std::string("nothing") : s)});
    g_lastRan = now;
}

void Status(DWORD now) {
    CameraNote(now);
    EffectNote(now);
    if (now - g_lastRan >= kRanEveryMs || !g_on) RanNote(now);
    const std::string texts[10] = {"Indoor light between stories: " + LevelLightShare::Status(), LevelLightShare::CrossLotText(), "Rooms at Night: " + UnlitRooms::Status(),
                                  "Faster Room Lighting: " + RoomLightQueue::StatusText(), "Indoor object maps: " + RoomMapPadding::Status(), FurnitureLine(),
                                  CameraLine(now), "Rooms at Night sliders: " + UnlitRooms::SettingsText(),
                                  "Rooms keep their light: " + LampMarkFilter::Status(),
                                  // (development builds, arch_ambient) the arches' floor draws: matched or not, and why
                                  DevSwitches::ArchAmbient() ? LevelLightShare::ArchAmbientStatus() : std::string()};
    for (int i = 0; i < 10; i++)
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

// The stop key: the recording goes on until the light has settled
void BeginSettle() {
    g_settleAt = GetTickCount();
    LOG_INFO("[Recorder] Stop asked: saving in 8 s (its shortcut again saves at once)");
    Captures::Notify(I18n::Tr("Saving the capture and screenshot..."), kSettleMs / 1000 + 1, Captures::NoteKind::Saving);
}

void Start() {
    g_on = true;
    LevelLightShare::BeginSeamRecording();
    g_lines.clear();
    for (auto& s : g_lastStatusText) s.clear();
    g_lastCamera.clear();
    g_lastEffects.clear();
    g_cameraSeen = g_cameraMirrored = 0;
    g_counts.clear();
    g_settleAt = 0;
    SceneHook(true);
    NoSceneTrace::Begin();
    PostScene::TakeRan();
    g_startTick = GetTickCount();
    g_lastRan = g_startTick;
    GetLocalTime(&g_startClock);
    std::error_code ec;
    g_logStart = std::filesystem::file_size(Dir() / L"ApexRadiance_LOG.txt", ec);
    if (ec) g_logStart = 0;
    g_lastStatus = g_startTick;
    g_notes = 0;
    g_roomNotes = 0;
    {
        std::lock_guard<std::mutex> lk(g_editMx);
        g_edits.clear();
        g_cubes.clear();
    }
    g_views.clear();
    g_ground.clear();
    g_settingsAtStart.clear();
    {
        std::ifstream in(Dir() / L"ApexRadiance.toml", std::ios::binary);
        if (in) g_settingsAtStart.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    }
    LotLightBridge::FurnitureTraceReset(); // every object is written once again at its first draw
    if (!kPublicBuild) OutdoorDelta::SceneBegin(Dir() / L"Lighting scene.tmp"); // (developer: the physical rooms' solves, for tools/scene_replay; it sends each to its solve, up to 1 GB)
    // every room once, as it is now (30/09: these lines used to be dropped, so a recording showed rooms only once they changed)
    for (auto& line : LevelLightShare::TraceRooms(true))
        if (g_roomNotes < kMaxRoomNotes) {
            g_roomNotes++;
            g_lines.push_back({g_startTick, std::move(line)});
        }
    Status(g_startTick);
    LOG_INFO("[Recorder] Recording started (its shortcut again to stop; stops by itself after 60 s)");
}

void Stop() {
    g_on = false;
    const std::string noScene = NoSceneTrace::End();
    const std::string wallSeams = LevelLightShare::EndSeamRecording(true);
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
    {
        std::lock_guard<std::mutex> lk(g_editMx);
        for (const Edit& e : g_edits)
            all.emplace_back(Clock(e.tick), std::format("[edit] a lamp light seen on story {} (lot {:08X}) {}", e.story, e.lot,
                                                        e.moved ? "moved or changed" : e.on ? "switched on" : "switched off"));
        for (const auto& [key, c] : g_cubes) all.emplace_back(Clock(c.tick), std::format("[cube] {} cube of probe {:#x}, {} write(s), the last: {}", key.second, key.first, c.writes, c.text));
    }
    for (const GroundShown& x : g_ground)
        all.emplace_back(Clock(x.tick), std::format("[ground] terrain chunk ({},{}) shows new light{}", x.ix, x.iz, x.together ? " (held for a lamp switch, with its batch)" : ""));
    int settleNotes = 0;
    DWORD settleLast = 0;
    for (auto& [tick, text] : LevelLightShare::JournalSince(g_startTick)) {
        if (g_settleAt && !Before(static_cast<DWORD>(tick), g_settleAt)) settleNotes++, settleLast = static_cast<DWORD>(tick);
        all.emplace_back(Clock(static_cast<DWORD>(tick)), "[solve] " + text);
    }
    if (g_settleAt)
        all.emplace_back(Clock(end), std::format("[settle] {:.1f} s from the stop key to the save{}: {} solve note(s) in that time{}", (end - g_settleAt) / 1000.0,
                                                 end - g_settleAt < kSettleMs ? " (ended by the key or the menu)" : "", settleNotes,
                                                 settleNotes ? std::format(", the last {:.2f} s before the save", (end - settleLast) / 1000.0) : std::string()));
    g_settleAt = 0;
    for (auto& l : logLines) all.emplace_back(l.first, "[log] " + l.second);
    std::stable_sort(all.begin(), all.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    const std::filesystem::path folder = Captures::NewFolder("Recording");
    {
        std::lock_guard<std::mutex> lk(g_editMx);
        for (auto& [key, c] : g_cubes) {
            if (c.strip.empty()) continue;
            std::error_code ec;
            std::filesystem::create_directories(folder / L"Cubes", ec);
            Captures::WritePngAsync(folder, folder / L"Cubes" / std::format("{} {:08X}.png", key.second, key.first), std::move(c.strip), c.side * 6, c.side, 3);
            c.strip.clear();
        }
    }
    OutdoorDelta::SceneEnd(folder / L"Lighting scene.apexscene");
    const std::string video = CaptureVideo::End(folder / L"Video.mjpeg");
    const std::string name = ApexUtil::ToUtf8(folder.filename().wstring());
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
        if (!kPublicBuild)
            out << "    developer mode: box = the size of the rig's box; lamp part = a lamp stands in that box (the part may be the lamp, or an object around it); "
                   "rig own = the rig's own lights where c4..c7 do not hold them\n";
        out << "[room] = an indoor room of a loaded lot, once at the start and again when its ambient (+0x110, the colour its walls take; "
               "+0x120), normalisation, solve state, LOD class (solving / shown), light count or shown story changes (checked every 100 ms)\n";
        out << "[probe] = a Light capture\n";
        out << "[edit] = a lamp the player switched, moved or changed; [solve] ... E = a room's solve ended: it shows its new light from the next "
               "frame (during a recording every room is noted)\n";
        out << "[ground] = a terrain chunk's smoothed light map (also what lots, floors and fences read) shows new light from this frame\n";
        out << "[camera] = the camera when it changes: plain or mirrored, near plane, the game's eye, the view-projection's 4 rows; [effect] = the features on, "
               "when that changes; [ran] = what ran in that time (what is not named did not run); [cube] = a light probe's last cube, the mean colours of its "
               "faces (+x -x +y -y +z -z, 0..255 as stored); [settle] = the wait between the stop key and the save\n";
        if (g_cameraMirrored)
            out << std::format("WARNING: the camera was the water reflection's mirrored copy in {} of {} checks: what follows the camera can be wrong there\n", g_cameraMirrored, g_cameraSeen);
        else if (!g_cameraSeen) out << "WARNING: no camera was seen\n";
        out << FrameTimes();
        if (!video.empty()) out << "[video] " << video << '\n';
        out << '\n';
        out << LightUpdates(g_startTick, end);
        if (g_notes >= kMaxNotes) out << std::format("(the furniture and probe lines stopped after {} lines)\n\n", kMaxNotes);
        if (g_roomNotes >= kMaxRoomNotes) out << std::format("(the [room] lines stopped after {} lines)\n\n", kMaxRoomNotes);
        for (const auto& [clock, text] : all) out << clock << ' ' << text << '\n';
        if (!noScene.empty()) out << '\n' << noScene;
        out << "\n==== ApexRadiance.toml at the start of the recording ====\n" << g_settingsAtStart << '\n';
    }
    Captures::WriteText(folder / L"Recording.txt", out.str());
    Captures::WriteText(folder / L"Wall seams.csv", wallSeams);
    if (const std::string pixels = PixelCsv(g_startTick); !pixels.empty()) Captures::WriteText(folder / L"Screen pixels.csv", pixels);
    if (const std::string ground = GroundProfile::WatchCsv(g_startTick); !ground.empty()) Captures::WriteText(folder / L"Ground timeline.csv", ground);
    LOG_INFO(std::format("[Recorder] Saved {} lines to Captures\\{}", all.size(), name));
    const std::string what = std::format("a recording of {:.0f} s of the lighting", (end - g_startTick) / 1000.0);
    if (!CaptureBurst::Begin(folder, what)) Captures::Finish(folder, what, Captures::CaptureKind::Recording);
    g_saved = name;
    g_savedAt = GetTickCount();
    ReleaseBuffers();
}

} // namespace

namespace Recorder {
bool Active() { return g_on; }
void Note(const std::string& text) {
    if (!g_on || g_notes >= kMaxNotes) return;
    g_notes++;
    g_lines.push_back({GetTickCount(), text});
}
void NoteLampEdit(uintptr_t treeLevel, bool on, bool moved) {
    if (!g_on) return;
    uint32_t lot = 0;
    int story = 0;
    if (!LevelLightShare::TreeLevelLot(treeLevel, lot, story)) return;
    std::lock_guard<std::mutex> lk(g_editMx);
    if (g_edits.size() < 4096) g_edits.push_back(Edit{GetTickCount(), lot, story, on, moved});
}
void NoteGround(int ix, int iz, bool together) {
    if (g_on && g_ground.size() < 4096) g_ground.push_back({GetTickCount(), ix, iz, together});
}
void NoteCube(uintptr_t probe, const char* what, const float mean[6][3], unsigned size, std::vector<unsigned char> strip) {
    if (!g_on) return;
    std::string text = std::format("{} px", size);
    for (int f = 0; f < 6; f++) text += std::format(" | {:.1f} {:.1f} {:.1f}", mean[f][0], mean[f][1], mean[f][2]);
    std::lock_guard<std::mutex> lk(g_editMx);
    CubeNote& c = g_cubes[{probe, what}];
    c.tick = GetTickCount();
    c.writes++;
    c.text = std::move(text);
    if (g_cubes.size() <= 64) c.strip = std::move(strip), c.side = size;
}
void Count(const char* what) {
    if (g_on) g_counts[what]++;
}
int SecondsRecorded() { return g_on ? static_cast<int>((GetTickCount() - g_startTick) / 1000) : -1; }
void RequestToggle() { g_toggleRequest = true; }
void RequestStop() { g_stopRequest = true; }
void RequestCancel() { g_cancelRequest = true; }
bool Settling() { return g_on && g_settleAt; }
void OnPresent() {
    if (DevSwitches::kCompiled) MeasureMemory(); // development builds (memory_owners)
    const bool requested = g_toggleRequest.exchange(false);
    // (a recording started during the frames after the last one would film them)
    const bool pressed = (Hotkeys::Take(Hotkeys::Action::Recorder) || requested) && !CaptureBurst::Running();
    const bool stop = g_stopRequest.exchange(false);
    if (g_cancelRequest.exchange(false)) {
        if (g_on) {
            g_on = false;
            g_settleAt = 0;
            LevelLightShare::EndSeamRecording(false);
            g_lines.clear();
            g_settingsAtStart.clear();
            g_saved.clear();
            g_notes = g_roomNotes = 0;
            ReleaseBuffers();
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
        if (!g_on) Start();
        else if (g_settleAt) Stop();
        else BeginSettle();
        return;
    }
    if (!g_on) return;
    const DWORD now = GetTickCount();
    if (now - g_lastStatus >= kStatusEveryMs) {
        g_lastStatus = now;
        Status(now);
    }
    if (g_settleAt ? now - g_settleAt >= kSettleMs : now - g_startTick >= kMaxMs) Stop();
}
} // namespace Recorder
