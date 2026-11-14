// Offline check of the built-in Apex looks and the in-game licence texts (console output only, reads LICENSES.md).
// Each built-in look is parsed with the real loader (features/looks_preset.cpp); every enabled technique must be a
// ported shader, every value a parameter of it, with the right number of components, inside its ui_min / ui_max.
#include "looks_preset.h"
#include "third_party/reshade_shaders/reshade_shaders_hlsl.h"
#include "third_party/reshade_shaders/reshade_licenses.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <sstream>
#include <string>

namespace ApexPaths {
const std::wstring& ApexDirectory() { static std::wstring d; return d; }
bool EnsureApexDirectory() { return false; }
}

static int checks = 0, failures = 0;
static void Check(bool ok, const std::string& what)
{
    ++checks;
    if (!ok) { ++failures; printf("FAIL %s\n", what.c_str()); }
}

static const LooksShader* Find(const Looks::PresetEntry& e)
{
    for (const auto& s : ReShadeShaders::kShaders)
        if (e.technique == s.technique && _stricmp(e.file.c_str(), s.file) == 0) return &s;
    return nullptr;
}

static std::string Words(const std::string& s) // whitespace-normalized
{
    std::istringstream in(s);
    std::string w, out;
    while (in >> w) out += w + " ";
    return out;
}

int main()
{
    const auto names = Looks::BuiltInLooks();
    Check(names.size() == 5, "five built-in looks");
    for (const std::string& name : names)
    {
        std::string text;
        Check(Looks::LookText("apex:" + name, text) && !text.empty(), name + ": text");
        Looks::Preset p;
        Check(Looks::ParsePreset(text, p) && p.error.empty(), name + ": parses");
        Check(!p.order.empty(), name + ": has techniques");
        Check(Looks::PresetSorting(p).size() == p.order.size(), name + ": TechniqueSorting lists every technique");
        for (const auto& e : p.order)
        {
            const LooksShader* s = Find(e);
            Check(s != nullptr, name + ": " + e.technique + "@" + e.file + " is a ported shader");
            if (!s) continue;
            const auto sec = p.sections.find(e.file);
            Check(sec != p.sections.end(), name + ": [" + e.file + "] section");
            if (sec == p.sections.end()) continue;
            for (const auto& [key, value] : sec->second)
            {
                const LooksParam* prm = nullptr;
                for (int i = 0; i < s->paramCount; ++i)
                    if (key == s->params[i].key) prm = &s->params[i];
                Check(prm != nullptr, name + ": " + e.file + " " + key + " is a uniform");
                if (!prm) continue;
                const auto parts = Looks::SplitList(value);
                Check((int)parts.size() == prm->comps, name + ": " + key + " has " + std::to_string(prm->comps) + " components");
                for (const auto& part : parts)
                {
                    char* end = nullptr;
                    const double x = strtod(part.c_str(), &end);
                    Check(end && !*end, name + ": " + key + " is a number");
                    Check(x >= prm->min && x <= prm->max, name + ": " + key + "=" + part + " inside its range");
                }
            }
        }
        for (const auto& [file, keys] : p.sections)
        {
            if (file.empty()) continue;
            bool used = false;
            for (const auto& e : p.order) used |= _stricmp(e.file.c_str(), file.c_str()) == 0;
            Check(used, name + ": [" + file + "] belongs to an enabled technique");
        }
    }
    Check(Looks::BuiltInLookText("Unknown") == nullptr, "unknown built-in look");

    // a GShade preset names SweetFX's HDR technique FakeHDR and its uniforms fHDRPower / fradius1 / fradius2
    {
        Looks::Preset g;
        Check(Looks::ParsePreset("Techniques=FakeHDR@FakeHDR.fx\nTechniqueSorting=FakeHDR@FakeHDR.fx\n[FakeHDR.fx]\nfHDRPower=1.5\nfradius1=0.7\nfradius2=0.9\n", g),
              "GShade FakeHDR preset parses");
        const LooksShader* hdr = nullptr;
        for (const auto& s : ReShadeShaders::kShaders)
            if (!_stricmp(s.file, "FakeHDR.fx")) hdr = &s;
        Check(hdr != nullptr, "FakeHDR is ported");
        if (hdr)
        {
            const std::vector<float> v = Looks::PresetValues(g, *hdr);
            for (int i = 0; i < hdr->paramCount; ++i)
            {
                const std::string key = hdr->params[i].key;
                const float want = key == "HDRPower" ? 1.5f : key == "radius1" ? 0.7f : key == "radius2" ? 0.9f : -1.0f;
                if (want >= 0) Check(v[i * 4] == want, "GShade FakeHDR value " + key + " read from its GShade name");
            }
        }
    }

    std::ifstream f("third_party/reshade_shaders/LICENSES.md", std::ios::binary);
    std::stringstream md;
    md << f.rdbuf();
    Check(f.good() || !md.str().empty(), "LICENSES.md readable (run from the repository root)");
    for (const auto& l : ReShadeShaders::kLicenses)
        Check(Words(md.str()).find(Words(l.text)) != std::string::npos, std::string("licence text in LICENSES.md: ") + l.title);

    printf("%d checks, %d failures\n%s\n", checks, failures, failures ? "PROBLEMS" : "OK");
    return failures ? 1 : 0;
}
