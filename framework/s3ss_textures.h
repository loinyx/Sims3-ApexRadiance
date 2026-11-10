#pragma once
// Sims3SettingsSetter's uncompressed textures under the RTX Remix bridge, as seen from Apex Radiance.
// [patches.UncompressedLotTexturesPatch] and [patches.UncompressedSimTexturesPatch] in S3SS.toml make the game build lot
// and Sim textures as A8R8G8B8 instead of DXT1 / DXT5 (4 to 8 times the bytes; S3SS's patches/uncompressed_*.cpp). Under
// the bridge every texture upload is copied to its 64-bit server, and players saw the camera stutter on turning
// (2026-11-09). While the bridge is in use and either is on, Apex asks at every start (a question card, and an item in
// Settings > Compatibility) and, on the player's click, sets enabled = false in both sections after a copy of the old
// file. S3SS reads S3SS.toml at start and writes it only when its own menu saves, so the change applies at the next start.
// A switch in S3SS's menu during play is seen too (Refresh): S3SS writes the game code at once, so the two sites are read
// (never written) once a second, and S3SS.toml is read again when its time stamp changes.
#include <algorithm>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace S3SSTextures {

struct State {
    bool s3ss = false;      // Sims3SettingsSetter.asi is loaded
    bool lot = false;       // [patches.UncompressedLotTexturesPatch] enabled = true, as read at the first look
    bool sim = false;       // [patches.UncompressedSimTexturesPatch] enabled = true
    bool liveLot = false;   // the game code holds S3SS's uncompressed lot format now (switched in its menu, saved or not)
    bool liveSim = false;   // the same for the Sim textures
    bool turnedOff = false; // Apply() changed the file this session (it takes effect at the next start)
    std::string error;      // why the last Apply() failed (English, for the log)
    std::wstring toml;      // S3SS.toml
};

// Looked up when the menu first asks, then kept up to date by Refresh; render thread only
const State& Get();
// Once a second at most (render thread): S3SS.toml read again if it changed, and the two code sites read. A file turned on
// again after Apply() clears turnedOff, so the question comes back
void Refresh();
// The RTX Remix bridge is in use, S3SS is loaded with either option on (in its file or in the game code now), and Apply()
// has not changed the file since
bool Recommended();
// Copies S3SS.toml to S3SS.toml.apex-backup, then writes enabled = false in both sections
bool Apply();

// ---- pure, inline for tools/s3ss_textures_test ----

inline constexpr std::string_view kLotSection = "patches.UncompressedLotTexturesPatch";
inline constexpr std::string_view kSimSection = "patches.UncompressedSimTexturesPatch";

namespace Detail {
inline std::string_view Trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}
// Calls f(valueStart, valueEnd) for each "enabled = value" line of [section] (offsets into toml)
template <class F> void ForEnabled(std::string_view toml, std::string_view section, F&& f) {
    bool in = false;
    for (size_t at = toml.substr(0, 3) == "\xEF\xBB\xBF" ? 3 : 0; at < toml.size();) { // (after a UTF-8 BOM)
        size_t end = toml.find('\n', at);
        if (end == std::string_view::npos) end = toml.size();
        const std::string_view line = Trim(toml.substr(at, end - at));
        if (!line.empty() && line.front() == '[') {
            const size_t close = line.find(']');
            in = close != std::string_view::npos && Trim(line.substr(1, close - 1)) == section;
        } else if (in && !line.empty() && line.front() != '#') {
            const size_t eq = toml.find('=', at);
            if (eq < end && Trim(toml.substr(at, eq - at)) == "enabled") {
                size_t v = eq + 1;
                while (v < end && (toml[v] == ' ' || toml[v] == '\t')) v++;
                size_t ve = v;
                while (ve < end && toml[ve] != ' ' && toml[ve] != '\t' && toml[ve] != '\r' && toml[ve] != '#') ve++;
                f(v, ve);
            }
        }
        at = end + 1;
    }
}
} // namespace Detail

// True when [section] has enabled = true (a key written twice: true when either says so)
inline bool Enabled(std::string_view toml, std::string_view section) {
    bool on = false;
    Detail::ForEnabled(toml, section, [&](size_t v, size_t ve) { on = on || toml.substr(v, ve - v) == "true"; });
    return on;
}
// The text with enabled = false in both sections; every other byte (comments, line endings, onlyForLOD0, the other
// patches) stays as it was
inline std::string TurnOff(std::string_view toml) {
    std::vector<std::pair<size_t, size_t>> values;
    for (const std::string_view section : {kLotSection, kSimSection})
        Detail::ForEnabled(toml, section, [&](size_t v, size_t ve) {
            if (toml.substr(v, ve - v) == "true") values.emplace_back(v, ve);
        });
    std::sort(values.rbegin(), values.rend()); // from the end, so the earlier offsets stay valid
    std::string out(toml);
    for (const auto& [v, ve] : values) out.replace(v, ve - v, "false");
    return out;
}

} // namespace S3SSTextures
