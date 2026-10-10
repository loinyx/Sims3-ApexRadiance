#pragma once
// 3D .cube tables: red changes fastest, then green, then blue.
// Kept independent of D3D so malformed files and axis ordering can be tested offline.
#include <array>
#include <cmath>
#include <istream>
#include <locale>
#include <sstream>
#include <string>
#include <vector>

namespace CubeLut {
constexpr size_t kMaxBytes = 32 * 1024 * 1024;
struct Table {
    int size = 0;
    std::array<float, 3> minimum{0, 0, 0}, inverseRange{1, 1, 1};
    std::vector<std::array<float, 4>> values;
};

struct Layout { unsigned width = 0, height = 0; };
inline Layout Pack(int size, unsigned maxWidth, unsigned maxHeight) {
    if (size < 2 || size > 65 || !maxWidth || !maxHeight) return {};
    const size_t count = size_t(size) * size * size;
    unsigned width = 1, height = 1;
    while (width < 1024 && width * 2 <= maxWidth && width < count) width *= 2;
    while (size_t(width) * height < count) height *= 2;
    return height <= maxHeight ? Layout{width, height} : Layout{};
}

inline bool Parse(std::istream& input, Table& result, std::string& error) {
    result = {};
    Table table;
    std::array<float, 3> maximum{1, 1, 1};
    bool haveMin = false, haveMax = false, haveRange = false, data = false;
    size_t bytes = 0, number = 0;
    std::string line;
    auto fail = [&](const char* why) { error = "Line " + std::to_string(number) + ": " + why; return false; };
    while (std::getline(input, line)) {
        ++number;
        bytes += line.size() + 1;
        if (bytes > kMaxBytes) return fail("the file exceeds 32 MiB");
        if (number == 1 && line.compare(0, 3, "\xEF\xBB\xBF") == 0) line.erase(0, 3);
        // Comments inside a quoted TITLE are harmless: titles are not used by the loader.
        line.erase(line.find('#') == std::string::npos ? line.size() : line.find('#'));
        std::istringstream row(line);
        row.imbue(std::locale::classic());
        std::string tag, extra;
        if (!(row >> tag)) continue;
        auto done = [&] { return !(row >> extra); };
        if (tag == "TITLE") {
            if (data) return fail("headers must precede the table");
            continue;
        }
        if (tag == "LUT_1D_SIZE" || tag == "LUT_1D_INPUT_RANGE") return fail("1D and combined LUTs are not supported");
        if (tag == "LUT_3D_SIZE") {
            if (data || table.size || !(row >> table.size) || !done() || table.size < 2 || table.size > 65)
                return fail("LUT_3D_SIZE must appear once and be between 2 and 65");
            table.values.reserve(size_t(table.size) * table.size * table.size);
            continue;
        }
        if (tag == "DOMAIN_MIN" || tag == "DOMAIN_MAX") {
            bool& have = tag == "DOMAIN_MIN" ? haveMin : haveMax;
            auto& v = tag == "DOMAIN_MIN" ? table.minimum : maximum;
            if (data || have || haveRange || !(row >> v[0] >> v[1] >> v[2]) || !done()) return fail("invalid or repeated domain header");
            for (float f : v) if (!std::isfinite(f)) return fail("domain values must be finite");
            have = true;
            continue;
        }
        if (tag == "LUT_3D_INPUT_RANGE") {
            float lo = 0, hi = 0;
            if (data || haveRange || haveMin || haveMax || !(row >> lo >> hi) || !done() || !std::isfinite(lo) || !std::isfinite(hi))
                return fail("invalid or conflicting input range");
            table.minimum.fill(lo); maximum.fill(hi); haveRange = true;
            continue;
        }
        if (!table.size) return fail("LUT_3D_SIZE is missing");
        std::istringstream values(line);
        values.imbue(std::locale::classic());
        std::array<float, 4> v{0, 0, 0, 1};
        if (!(values >> v[0] >> v[1] >> v[2]) || (values >> extra)) return fail("expected three RGB numbers");
        for (float f : v) if (!std::isfinite(f)) return fail("RGB values must be finite");
        if (table.values.size() == size_t(table.size) * table.size * table.size) return fail("too many RGB entries");
        table.values.push_back(v);
        data = true;
    }
    if (input.bad()) return fail("the file could not be read");
    if (!table.size || table.values.size() != size_t(table.size) * table.size * table.size) return fail("the RGB table is incomplete");
    for (int i = 0; i < 3; ++i) {
        const float range = maximum[i] - table.minimum[i];
        if (!(range > 0) || !std::isfinite(range) || !std::isfinite(1.0f / range)) return fail("each domain maximum must exceed its minimum");
        table.inverseRange[i] = 1.0f / range;
    }
    result = std::move(table);
    error.clear();
    return true;
}
} // namespace CubeLut
