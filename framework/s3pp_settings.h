#pragma once
// Sims3Performance (S3PP) next to sims3-modernization (ts3native.asi), as seen from Apex Radiance. With ts3native's
// route planning on its worker thread, S3PP's ShortWaitSpin keeps the waiting threads spinning on the routing lock;
// ts3native's author recommends ShortWaitSpin = 0 and ThreadStackDiet = 0 (Sims3SettingsSetter already makes the stack
// fix). Apex shows both mods in Settings > Compatibility and, when either setting is on, offers to turn them off:
// the two lines of [Patches] in Sims3Performance.ini change on the player's click, after a copy of the old file.
#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace S3PPSettings {

struct State {
    bool ts3native = false;  // ts3native.asi is loaded
    bool s3pp = false;       // Sims3Performance.asi is loaded
    bool spin = false;       // [Patches] ShortWaitSpin = 1 in its ini, as read at the first look (what the game runs with)
    bool diet = false;       // [Patches] ThreadStackDiet = 1
    bool turnedOff = false;  // Apply() changed the file this session (it takes effect at the next start)
    std::string error;       // why the last Apply() failed (English, for the log)
    std::wstring ini;        // Sims3Performance.ini next to the module
};

// Looked up once, when the menu first asks (the ASIs have loaded by then); render thread only
const State& Get();
// Both mods are loaded and either setting is on, and Apply() has not changed the file yet
bool Recommended();
// Copies Sims3Performance.ini to Sims3Performance.ini.apex-backup, then writes ShortWaitSpin = 0 and ThreadStackDiet = 0
bool Apply();

// ---- pure, inline for tools/s3pp_settings_test ----

namespace Detail {
inline std::string_view Trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}
inline bool SameText(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++)
        if ((a[i] | 0x20) != (b[i] | 0x20)) return false;
    return true;
}
// Calls f(line, valueStart, valueEnd) for each "key = value" line of [Patches] whose key matches (offsets into ini)
template <class F> void ForPatchKey(std::string_view ini, std::string_view key, F&& f) {
    bool inPatches = false;
    for (size_t at = ini.substr(0, 3) == "\xEF\xBB\xBF" ? 3 : 0; at < ini.size();) { // (after a UTF-8 BOM)
        size_t end = ini.find('\n', at);
        if (end == std::string_view::npos) end = ini.size();
        const std::string_view line = Trim(ini.substr(at, end - at));
        if (!line.empty() && line.front() == '[') {
            const size_t close = line.find(']');
            inPatches = close != std::string_view::npos && SameText(Trim(line.substr(1, close - 1)), "Patches"); // "[ Patches ] ; note" too
        }
        else if (inPatches && !line.empty() && line.front() != ';' && line.front() != '#') {
            const size_t eq = ini.find('=', at);
            if (eq < end && SameText(Trim(ini.substr(at, eq - at)), key)) {
                size_t v = eq + 1;
                while (v < end && (ini[v] == ' ' || ini[v] == '\t')) v++;
                size_t ve = v;
                while (ve < end && ini[ve] != ' ' && ini[ve] != '\t' && ini[ve] != '\r' && ini[ve] != ';' && ini[ve] != '#') ve++;
                f(v, ve);
            }
        }
        at = end + 1;
    }
}
} // namespace Detail

// The value of key in [Patches], -1 when absent or not a number (a key written twice: the higher value, so a duplicate
// line cannot hide an "on")
inline int ReadPatch(std::string_view ini, std::string_view key) {
    int value = -1;
    Detail::ForPatchKey(ini, key, [&](size_t v, size_t ve) {
        const std::string_view s = ini.substr(v, ve - v);
        if (!s.empty() && s.size() < 9 && s.find_first_not_of("0123456789") == std::string_view::npos) value = (std::max)(value, std::stoi(std::string(s)));
    });
    return value;
}
// The text with every ShortWaitSpin and ThreadStackDiet line of [Patches] set to 0; every other byte (comments, line
// endings, [Tuning]'s ShortWaitSpinUs and the rest) stays as it was
inline std::string TurnOff(std::string_view ini) {
    std::vector<std::pair<size_t, size_t>> values;
    for (const std::string_view key : {std::string_view("ShortWaitSpin"), std::string_view("ThreadStackDiet")})
        Detail::ForPatchKey(ini, key, [&](size_t v, size_t ve) { values.emplace_back(v, ve); });
    std::sort(values.rbegin(), values.rend()); // from the end, so the earlier offsets stay valid
    std::string out(ini);
    for (const auto& [v, ve] : values) out.replace(v, ve - v, "0");
    return out;
}

} // namespace S3PPSettings
