// ReShade FX effects, the CPU half (see reshade_fx_compile.h). Follows ReShade's own load_effect (source/runtime.cpp,
// crosire/reshade 7bf9de8) for the predefined macros and the compatibility macros, with the D3D9 code generator.
#include "reshade_fx_compile.h"
#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include "third_party/reshadefx/effect_codegen.hpp"
#include "third_party/reshadefx/effect_parser.hpp"
#include "third_party/reshadefx/effect_preprocessor.hpp"

namespace ReShadeFxCompile {
namespace {

std::string Trim(std::string s) {
    const auto notSpace = [](unsigned char c) { return !std::isspace(c); };
    s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
    s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
    return s;
}

// The sN registers an entry point's code declares (assemble_code_for_entry_point renumbers them from s0)
std::vector<uint32_t> SamplerRegisters(const std::string& hlsl) {
    std::vector<uint32_t> out;
    static constexpr std::string_view kTag = ": register(s";
    for (size_t p = hlsl.find(kTag); p != std::string::npos; p = hlsl.find(kTag, p + 1)) {
        size_t q = p + kTag.size();
        uint32_t n = 0;
        bool digit = false;
        while (q < hlsl.size() && std::isdigit(static_cast<unsigned char>(hlsl[q]))) {
            n = n * 10 + static_cast<uint32_t>(hlsl[q++] - '0');
            digit = true;
        }
        if (digit && std::find(out.begin(), out.end(), n) == out.end()) out.push_back(n);
    }
    return out;
}

} // namespace

std::string Utf8(const std::filesystem::path& p) {
    const auto s = p.u8string(); // std::u8string in C++20, std::string before
    return std::string(reinterpret_cast<const char*>(s.data()), s.size());
}

const EntryPoint* Effect::Find(const std::string& name) const {
    for (const auto& e : entryPoints)
        if (e.name == name) return &e;
    return nullptr;
}

Effect Parse(const Input& in) {
    Effect e;
    e.fileName = Utf8(in.file.filename());

    reshadefx::preprocessor pp;
    // Preset and Apex definitions first: add_macro_definition keeps the first one of a name
    for (const auto& [name, value] : in.definitions)
        if (!name.empty()) pp.add_macro_definition(name, value.empty() ? "1" : value);
    pp.add_macro_definition("__RESHADE__", std::to_string(kReShadeVersion));
    pp.add_macro_definition("__RESHADE_PERMUTATION__", "0");
    pp.add_macro_definition("__RESHADE_PERFORMANCE_MODE__", "0");
    pp.add_macro_definition("__VENDOR__", "0");
    pp.add_macro_definition("__DEVICE__", "0");
    pp.add_macro_definition("__RENDERER__", "36864"); // 0x9000: Direct3D 9
    pp.add_macro_definition("__APPLICATION__", "0");
    pp.add_macro_definition("BUFFER_WIDTH", std::to_string(in.width));
    pp.add_macro_definition("BUFFER_HEIGHT", std::to_string(in.height));
    pp.add_macro_definition("BUFFER_RCP_WIDTH", "(1.0 / BUFFER_WIDTH)");
    pp.add_macro_definition("BUFFER_RCP_HEIGHT", "(1.0 / BUFFER_HEIGHT)");
    pp.add_macro_definition("BUFFER_COLOR_SPACE", "1");      // sRGB
    pp.add_macro_definition("BUFFER_COLOR_FORMAT", "87");    // b8g8r8a8_unorm (api::format)
    pp.add_macro_definition("BUFFER_COLOR_BIT_DEPTH", "8");

    std::error_code ec;
    pp.add_include_path(in.file.parent_path());
    for (const auto& path : in.includes) pp.add_include_path(path);

    // ReShade's compatibility macros for older effects
    pp.append_string("#define tex2Doffset(s, coords, offset) tex2D(s, coords, offset)\n"
                     "#define tex2Dlodoffset(s, coords, offset) tex2Dlod(s, coords, offset)\n"
                     "#define tex2Dgather(s, t, c) tex2Dgather##c(s, t)\n"
                     "#define tex2Dgatheroffset(s, t, o, c) tex2Dgather##c(s, t, o)\n"
                     "#define tex2Dgather0 tex2DgatherR\n"
                     "#define tex2Dgather1 tex2DgatherG\n"
                     "#define tex2Dgather2 tex2DgatherB\n"
                     "#define tex2Dgather3 tex2DgatherA\n");
    const bool preprocessed = pp.append_file(in.file);
    e.errors += pp.errors();
    e.includedFiles = pp.included_files();
    if (!preprocessed) return e;

    std::unique_ptr<reshadefx::codegen> codegen(reshadefx::create_codegen_hlsl(30, false, false));
    reshadefx::parser parser;
    const bool parsed = parser.parse(pp.output(), codegen.get());
    e.errors += parser.errors();
    if (!parsed) return e;
    e.module = codegen->module();

    for (const auto& [name, type] : e.module.entry_points) {
        EntryPoint ep;
        ep.name = name;
        ep.type = type;
        if (type == reshadefx::shader_type::compute) {
            e.errors += "error: " + name + ": compute shaders are not supported in Direct3D 9\n";
            return e;
        }
        std::string unused;
        if (!codegen->assemble_code_for_entry_point(name, ep.hlsl, unused, e.errors)) {
            e.errors += "error: " + name + ": could not generate its code\n";
            return e;
        }
        ep.samplers = SamplerRegisters(ep.hlsl);
        e.entryPoints.push_back(std::move(ep));
    }
    e.parsed = true;
    return e;
}

bool Compile(Effect& e, const HlslCompiler& compiler) {
    if (!e.parsed) return false;
    if (!compiler) return false;
    for (auto& ep : e.entryPoints) {
        if (!ep.bytecode.empty()) continue;
        const char* target = ep.type == reshadefx::shader_type::vertex ? "vs_3_0" : "ps_3_0";
        std::string errors;
        const bool ok = compiler(ep.hlsl, ep.name, target, ep.bytecode, errors);
        if (!errors.empty()) e.errors += errors;
        if (!ok || ep.bytecode.empty()) {
            ep.bytecode.clear();
            if (errors.empty()) e.errors += "error: " + ep.name + ": the shader compiler failed\n";
            return false;
        }
    }
    e.compiled = true;
    return true;
}

std::string FirstError(const std::string& errors) {
    size_t start = 0;
    std::string first;
    while (start < errors.size()) {
        size_t end = errors.find('\n', start);
        if (end == std::string::npos) end = errors.size();
        const std::string line = errors.substr(start, end - start);
        start = end + 1;
        const size_t at = line.find("error");
        if (at == std::string::npos) continue;
        first = Trim(line.substr(at));
        break;
    }
    if (first.size() > 160) first = first.substr(0, 157) + "...";
    return first;
}

const reshadefx::annotation* FindAnnotation(const std::vector<reshadefx::annotation>& list, const char* name) {
    for (const auto& a : list)
        if (a.name == name) return &a;
    return nullptr;
}

std::string AnnotationString(const std::vector<reshadefx::annotation>& list, const char* name) {
    const auto* a = FindAnnotation(list, name);
    return a && a->type.base == reshadefx::type::t_string ? a->value.string_data : std::string();
}

float AnnotationFloat(const std::vector<reshadefx::annotation>& list, const char* name, float fallback, int component) {
    const auto* a = FindAnnotation(list, name);
    if (!a || !a->type.is_numeric()) return fallback;
    const int c = std::clamp(component, 0, 15);
    if (a->type.is_floating_point()) return a->value.as_float[c];
    if (a->type.is_signed()) return static_cast<float>(a->value.as_int[c]);
    return static_cast<float>(a->value.as_uint[c]);
}

int AnnotationInt(const std::vector<reshadefx::annotation>& list, const char* name, int fallback, int component) {
    const auto* a = FindAnnotation(list, name);
    if (!a || !a->type.is_numeric()) return fallback;
    const int c = std::clamp(component, 0, 15);
    if (a->type.is_floating_point()) return static_cast<int>(a->value.as_float[c]);
    return a->value.as_int[c];
}

// ---- presets ----
std::string Preset::Get(const std::string& section, const std::string& key) const {
    const auto s = sections.find(section);
    if (s == sections.end()) return {};
    const auto k = s->second.find(key);
    return k == s->second.end() ? std::string() : k->second;
}

bool Preset::Has(const std::string& section, const std::string& key) const {
    const auto s = sections.find(section);
    return s != sections.end() && s->second.count(key) != 0;
}

void Preset::Set(const std::string& section, const std::string& key, const std::string& value) {
    if (!sections.count(section) && !section.empty()) sectionOrder.push_back(section);
    sections[section][key] = value;
}

void Preset::Erase(const std::string& section, const std::string& key) {
    const auto s = sections.find(section);
    if (s != sections.end()) s->second.erase(key);
}

Preset ParsePreset(const std::string& text) {
    Preset p;
    std::string section;
    size_t start = 0;
    while (start < text.size()) {
        size_t end = text.find('\n', start);
        if (end == std::string::npos) end = text.size();
        std::string line = Trim(text.substr(start, end - start));
        start = end + 1;
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line.front() == '[' && line.back() == ']') {
            section = Trim(line.substr(1, line.size() - 2));
            if (!p.sections.count(section)) {
                p.sections[section];
                p.sectionOrder.push_back(section);
            }
            continue;
        }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        p.Set(section, Trim(line.substr(0, eq)), Trim(line.substr(eq + 1)));
    }
    return p;
}

std::string WritePreset(const Preset& p) {
    std::string out;
    if (const auto g = p.sections.find(""); g != p.sections.end())
        for (const auto& [k, v] : g->second) out += k + "=" + v + "\n";
    for (const auto& name : p.sectionOrder) {
        const auto s = p.sections.find(name);
        if (s == p.sections.end() || name.empty() || s->second.empty()) continue;
        out += "\n[" + name + "]\n";
        for (const auto& [k, v] : s->second) out += k + "=" + v + "\n";
    }
    return out;
}

std::vector<std::string> SplitList(const std::string& value) {
    std::vector<std::string> out;
    size_t start = 0;
    while (start <= value.size()) {
        size_t end = value.find(',', start);
        if (end == std::string::npos) end = value.size();
        std::string item = Trim(value.substr(start, end - start));
        if (!item.empty()) out.push_back(std::move(item));
        start = end + 1;
    }
    return out;
}

std::string JoinList(const std::vector<std::string>& items) {
    std::string out;
    for (const auto& item : items) {
        if (!out.empty()) out += ',';
        out += item;
    }
    return out;
}

std::vector<std::pair<std::string, std::string>> ParseDefinitions(const std::string& value) {
    std::vector<std::pair<std::string, std::string>> out;
    for (const auto& item : SplitList(value)) {
        const size_t eq = item.find('=');
        if (eq == std::string::npos) out.emplace_back(item, "1");
        else out.emplace_back(Trim(item.substr(0, eq)), Trim(item.substr(eq + 1)));
    }
    return out;
}

} // namespace ReShadeFxCompile
