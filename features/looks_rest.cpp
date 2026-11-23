// ReShade Effect Shader Toggler configurations in Looks (looks_rest.h). Credits: @loinyx
#include "looks_rest.h"
#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <map>

namespace Looks::Rest {
namespace {

std::string Trim(std::string s)
{
    while (!s.empty() && (s.back() == '\r' || s.back() == ' ' || s.back() == '\t')) s.pop_back();
    size_t a = 0;
    while (a < s.size() && (s[a] == ' ' || s[a] == '\t')) ++a;
    return s.substr(a);
}
std::string Lower(std::string s)
{
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}
// REST's CDataFile: GetBool is true for "1...", "true", "yes"; GetUInt is atoll (so "True" reads 0)
bool AsBool(const std::string& v) { return v.rfind('1', 0) == 0 || Lower(v) == "true" || Lower(v) == "yes"; }
uint32_t AsUInt(const std::string& v) { return static_cast<uint32_t>(std::atoll(v.c_str())); }

} // namespace

bool Parse(const std::string& text, Config& out)
{
    out = {};
    std::map<std::string, std::map<std::string, std::string>> sections; // lower-case section -> key -> value
    std::string section;
    for (size_t pos = 0; pos < text.size();)
    {
        size_t e = text.find('\n', pos);
        if (e == std::string::npos) e = text.size();
        const std::string line = Trim(text.substr(pos, e - pos));
        pos = e + 1;
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line.front() == '[' && line.back() == ']') { section = Lower(line.substr(1, line.size() - 2)); continue; }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        sections[section][Lower(Trim(line.substr(0, eq)))] = Trim(line.substr(eq + 1));
    }
    auto value = [&](const std::string& sec, const char* key, std::string* v) {
        const auto s = sections.find(sec);
        if (s == sections.end()) return false;
        const auto k = s->second.find(key);
        if (k == s->second.end() || k->second.empty()) return false;
        *v = k->second;
        return true;
    };
    std::string v;
    const uint32_t count = value("general", "amountgroups", &v) ? AsUInt(v) : 0;
    for (uint32_t i = 0; i < count && i < 64; ++i)
    {
        const std::string root = "group" + std::to_string(i);
        if (!sections.count(root)) continue;
        Group g;
        g.name = value(root, "name", &v) ? v : "Default";
        g.active = value(root, "active", &v) && AsBool(v);
        if (value(root, "invocationlocation", &v)) g.location = AsUInt(v);
        if (g.location > kOnRenderTargetChange)
        {
            out.notes.push_back(g.name + ": unknown invocation location " + v + ", before the draw instead");
            g.location = kBeforeDraw;
        }
        if (value(root, "matchswapchainresolutiononly", &v)) g.match = AsUInt(v);
        if (g.match > kAnySize) g.match = kAnySize; // (REST: no check above its known modes)
        g.requeue = value(root, "requeueafterrtmatchingfailure", &v) && AsBool(v);
        g.allowAll = value(root, "allowalltechniques", &v) && AsBool(v);
        g.exceptions = value(root, "techniqueexceptions", &v) && AsBool(v);
        if (value(root, "techniques", &v))
            for (size_t a = 0; a <= v.size();)
            {
                size_t b = v.find(',', a);
                if (b == std::string::npos) b = v.size();
                const std::string t = Trim(v.substr(a, b - a));
                if (!t.empty()) g.techniques.insert(t);
                a = b + 1;
            }
        for (const auto& [suffix, set] : { std::pair<const char*, std::unordered_set<uint32_t>*>{ "_vertexshaders", &g.vs }, { "_pixelshaders", &g.ps } })
        {
            const std::string sec = root + suffix;
            const uint32_t n = value(sec, "amounthashes", &v) ? AsUInt(v) : 0;
            for (uint32_t k = 0; k < n && k < 100000; ++k)
                if (value(sec, ("shaderhash" + std::to_string(k)).c_str(), &v) && AsUInt(v) != 0xFFFFFFFFu) set->insert(AsUInt(v)); // (REST's "missing")
        }
        if (value(root + "_computeshaders", "amounthashes", &v) && AsUInt(v)) out.notes.push_back(g.name + ": compute shaders (none in Direct3D 9)");
        if ((value(root, "providetexturebinding", &v) && AsBool(v)) || (value(root, "extractconstants", &v) && AsBool(v)) ||
            (value(root, "extractsrvs", &v) && AsBool(v)))
            out.notes.push_back(g.name + ": texture bindings and constant copies are not followed");
        if (value(root, "togglekey", &v) && AsUInt(v)) out.notes.push_back(g.name + ": its toggle key is not followed (Active decides)");
        out.groups.push_back(std::move(g));
    }
    if (count > 64) out.notes.push_back("groups after the 64th are not read");
    return !out.groups.empty();
}

uint32_t Crc32(const void* data, size_t bytes)
{
    static const auto table = [] {
        std::vector<uint32_t> t(256);
        for (uint32_t i = 0; i < 256; ++i)
        {
            uint32_t c = i;
            for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            t[i] = c;
        }
        return t;
    }();
    uint32_t c = 0xFFFFFFFFu;
    const uint8_t* p = static_cast<const uint8_t*>(data);
    for (size_t i = 0; i < bytes; ++i) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

bool Takes(const Group& g, const std::string& technique, const std::string& file)
{
    if (!g.active) return false;
    auto listed = [&] {
        if (g.techniques.count(technique)) return true;
        for (const std::string& t : g.techniques) // "Name [File.fx]"
        {
            const size_t open = t.rfind(" [");
            if (open == std::string::npos || t.back() != ']' || t.compare(0, open, technique) != 0) continue;
            if (Lower(t.substr(open + 2, t.size() - open - 3)) == Lower(file)) return true;
        }
        return false;
    };
    if (g.allowAll) return !(g.exceptions && listed());
    return listed();
}

bool SizeMatches(const Group& g, uint32_t w, uint32_t h, uint32_t bw, uint32_t bh)
{
    if (g.match == kAnySize) return true;
    if (g.match == kResolution) return w == bw && h == bh;
    // REST's check_aspect_ratio (from ReShade's generic_depth add-on): similar aspect, and a similar size or, extended,
    // almost an exact multiple of it
    if (!w || !h) return true;
    float wr = (float)bw / (float)w, hr = (float)bh / (float)h;
    const float aspect = (float)bw / (float)bh - (float)w / (float)h;
    if (std::fabs(aspect) > 0.1f) return false;
    if (wr <= 1.85f && wr >= 0.5f && hr <= 1.85f && hr >= 0.5f) return true;
    float wi, hi;
    return g.match == kExtendedAspectRatio && std::modf(wr, &wi) <= 0.02f && std::modf(hr, &hi) <= 0.02f;
}

} // namespace Looks::Rest
