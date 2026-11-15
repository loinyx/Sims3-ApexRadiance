// MP4 -> MKV (Matroska) remux for the video recorder: the H.264 / HEVC video and AAC sound are copied as they are
// (no re-encode). Reads plain and fragmented ("crash-safe") MP4 files as Media Foundation writes them.
#pragma once
#include <filesystem>
#include <string>

namespace MkvRemux {
// Writes `to` from `from`. false + why when the MP4 cannot be read or the MKV written (a partial `to` is deleted).
bool Mp4ToMkv(const std::filesystem::path& from, const std::filesystem::path& to, std::string& why);
} // namespace MkvRemux
