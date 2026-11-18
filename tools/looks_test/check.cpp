// Offline check of the preset loader (features/looks_preset.cpp) and the in-game licence texts (console output only,
// reads LICENSES.md).
#include "looks_preset.h"
#include "third_party/reshade_shaders/reshade_shaders_hlsl.h"
#include "third_party/reshade_shaders/reshade_licenses.h"
#include <cmath>
#include <cstdio>
#include <iterator>
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

static std::string Words(const std::string& s) // whitespace-normalized
{
    std::istringstream in(s);
    std::string w, out;
    while (in >> w) out += w + " ";
    return out;
}

int main()
{
    // built-in looks were removed: an old apex: reference is no look at all
    Check(Looks::RefSource("apex:Natural") == Looks::Source::None, "apex: is no longer a source");

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

    // every value kind written as ReShade writes it (FormatValue) reads back the same (PresetValues); bools also as
    // true / false
    {
        static const LooksParam params[] = {
            { "f", "Float", 1, 8, LooksParamKind::Float, { 0.25f }, 0.0f, 1.0f, nullptr },
            { "i", "Int", 1, 9, LooksParamKind::Int, { -3.0f }, -10.0f, 10.0f, nullptr },
            { "b", "Bool", 1, 10, LooksParamKind::Bool, { 1.0f }, 0.0f, 1.0f, nullptr },
            { "c", "Combo", 1, 11, LooksParamKind::Combo, { 2.0f }, 0.0f, 3.0f, "A\0B\0C\0D\0" },
            { "rgb", "Color", 3, 12, LooksParamKind::Color, { 0.1f, 0.2f, 0.3f }, 0.0f, 1.0f, nullptr },
            { "rgba", "Color", 4, 13, LooksParamKind::Color, { 0.1f, 0.2f, 0.3f, 0.4f }, 0.0f, 1.0f, nullptr },
            { "v2", "Range (Start, End)", 2, 14, LooksParamKind::Float, { 0.1f, 0.85f }, 0.0f, 1.0f, nullptr },
            { "i2", "Size", 2, 15, LooksParamKind::Int, { 4.0f, -8.0f }, 0.0f, 0.0f, nullptr },
            { "f4", "Weights", 4, 16, LooksParamKind::Float, { 1.5f, -2.25f, 1e-3f, 123.456f }, 0.0f, 0.0f, nullptr },
            { "hid", "Hidden", 1, 17, LooksParamKind::Float, { 7.0f }, 0.0f, 0.0f, nullptr, nullptr, nullptr, nullptr, 0.0f, kParamHidden },
        };
        const LooksShader sh{ "T", "T.fx", nullptr, nullptr, params, (int)std::size(params), false, false, "", LooksShaderKind::Color };
        const float want[][4] = { { 0.75f }, { -7 }, { 0 }, { 3 }, { 0.5f, 0.25f, 1 }, { 1, 0, 0.5f, 0.125f }, { 0.2f, 0.9f }, { 12, -1 },
                                  { -1.5f, 0.5f, 2.0f, 99.0f }, { 3 } };
        std::string ini = "Techniques=T@T.fx\nTechniqueSorting=T@T.fx\n\n[T.fx]\n";
        for (size_t p = 0; p < std::size(params); ++p) ini += std::string(params[p].key) + "=" + Looks::FormatValue(params[p], want[p]) + "\n";
        Check(ini.find("f=0.750000\n") != std::string::npos && ini.find("i=-7\n") != std::string::npos && ini.find("b=0\n") != std::string::npos &&
                  ini.find("rgb=0.500000,0.250000,1.000000\n") != std::string::npos && ini.find("i2=12,-1\n") != std::string::npos,
              "values written as ReShade writes them");
        Looks::Preset pr;
        Check(Looks::ParsePreset(ini, pr), "written values parse");
        const std::vector<float> got = Looks::PresetValues(pr, sh);
        for (size_t p = 0; p < std::size(params); ++p)
            for (int c = 0; c < params[p].comps; ++c)
                Check(std::fabs(got[p * 4 + c] - want[p][c]) < 1e-5f, std::string("round trip of ") + params[p].key + "[" + std::to_string(c) + "]");
        Looks::Preset tf;
        Looks::ParsePreset("[T.fx]\nb=true\n", tf);
        Check(Looks::PresetValues(tf, sh)[2 * 4] == 1.0f, "bool written as true");
        Looks::ParsePreset("[T.fx]\nb=false\n", tf);
        Check(Looks::PresetValues(tf, sh)[2 * 4] == 0.0f, "bool written as false");
        Looks::ParsePreset("[T.fx]\n", tf);
        Check(Looks::PresetValues(tf, sh)[9 * 4] == 7.0f && Looks::PresetValues(tf, sh)[1 * 4] == -3.0f, "missing keys keep the .fx defaults");
        // the menu's controls for each kind
        Check(LooksControlOf(params[0]) == LooksControl::Slider && LooksControlOf(params[2]) == LooksControl::Switch &&
                  LooksControlOf(params[3]) == LooksControl::Choice && LooksControlOf(params[4]) == LooksControl::Color &&
                  LooksControlOf(params[5]) == LooksControl::Color && LooksControlOf(params[6]) == LooksControl::Vector &&
                  LooksControlOf(params[9]) == LooksControl::None && LooksItems(params[3]).size() == 4,
              "controls per value kind");
    }

    // PreprocessorDefinitions: ReShade's list, bounded; the global line, then the effect's section, the later winning
    {
        const LooksDefines d = Looks::ParseDefines("MXAO_ENABLE_IL=1, QUALITY = 2 ,SCALE=1.0,FLAG,1BAD=3,BAD-NAME=1,TEXT=a b,QUALITY=3");
        const LooksDefines want = { { "MXAO_ENABLE_IL", "1" }, { "QUALITY", "3" }, { "SCALE", "1.0" }, { "FLAG", "1" }, { "TEXT", "a b" } };
        Check(d == want, "PreprocessorDefinitions parsed (names checked, a name alone is 1, the last value wins)");
        Check(Looks::ParseDefines(Looks::FormatDefines(d)) == d, "PreprocessorDefinitions round trip");
        Check(Looks::FormatDefines(d) == "MXAO_ENABLE_IL=1,QUALITY=3,SCALE=1.0,FLAG=1,TEXT=a b", "PreprocessorDefinitions written as ReShade writes them");
        std::string many;
        for (int i = 0; i < 300; ++i) many += "D" + std::to_string(i) + "=1,";
        Check(Looks::ParseDefines(many).size() == 128, "at most 128 definitions");
        Check(Looks::ParseDefines("LONG=" + std::string(300, 'x')).empty(), "a value over 256 characters is dropped");
        Looks::Preset pr;
        Looks::ParsePreset("PreprocessorDefinitions=A=1,B=2\nTechniques=T@T.fx\n[T.fx]\nPreprocessorDefinitions=B=5,C=0\n[U.fx]\nx=1\n", pr);
        Check(Looks::PresetDefines(pr, "t.fx") == LooksDefines{ { "A", "1" }, { "B", "5" }, { "C", "0" } }, "the section's definitions after the global ones");
        Check(Looks::PresetDefines(pr, "U.fx") == LooksDefines{ { "A", "1" }, { "B", "2" } }, "an effect without its own definitions gets the global ones");
        Looks::Preset none;
        Looks::ParsePreset("Techniques=T@T.fx\n", none);
        Check(Looks::PresetDefines(none, "T.fx").empty(), "no definitions");
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
