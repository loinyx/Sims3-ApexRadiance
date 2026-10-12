#pragma once
// ReShade FX effects, the CPU half (no device): turns an .fx file into Direct3D 9 shaders with the ReShade FX compiler
// (third_party/reshadefx, BSD-3-Clause), reads and writes ReShade preset .ini files. Used by the runtime
// (features/reshade_fx.cpp) on its worker thread and by the offline check (tools/reshade_fx_test).
//
// Shader model 3 layout (what the generated code expects, effect_codegen_hlsl.cpp):
//  - every uniform is a float constant register c<offset / 16> in both shaders (ints and bools as floats);
//  - a texture with a semantic (COLOR, DEPTH) has its pixel size in c<semantic_binding>;
//  - the vertex shader adds __TEXEL_SIZE__ (c255 = -1 / width, 1 / height) to its position (half-pixel offset);
//  - the vertex id arrives as a float in TEXCOORD0;
//  - sampler s<N> of an entry point is pass.sampler_bindings[i] with entry_point_binding N.
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>
#include "third_party/reshadefx/effect_module.hpp"

namespace reshadefx {
class codegen;
}

namespace ReShadeFxCompile {

// The ReShade version the effects see in __RESHADE__ (the compiler is ReShade 6.6's, crosire/reshade 7bf9de8)
constexpr int kReShadeVersion = 60600;

struct Input {
    std::filesystem::path file;                 // the .fx
    std::vector<std::filesystem::path> includes; // searched in order after the effect's own folder
    uint32_t width = 0, height = 0;              // BUFFER_WIDTH / BUFFER_HEIGHT
    // Preset and Apex definitions; the first one of a name wins (preset entries go first)
    std::vector<std::pair<std::string, std::string>> definitions;
};

// HLSL -> bytecode ("vs_3_0" / "ps_3_0"); false with the compiler's message. Null: no bytecode (parse-only checks).
using HlslCompiler = std::function<bool(const std::string& hlsl, const std::string& entry, const char* target, std::string& bytecode, std::string& errors)>;

struct EntryPoint {
    std::string name;
    reshadefx::shader_type type = reshadefx::shader_type::unknown;
    std::string hlsl;              // the generated code of this entry point alone
    std::string bytecode;          // empty until compiled
    std::vector<uint32_t> samplers; // the sN registers it reads (vertex shaders: which vertex samplers to bind)
};

struct Effect {
    std::string fileName;          // "Clarity.fx"
    bool parsed = false;           // techniques and uniforms are known
    bool compiled = false;         // every entry point has bytecode
    std::string errors;            // compiler messages (warnings included when it worked)
    reshadefx::effect_module module;
    std::vector<EntryPoint> entryPoints;
    std::vector<std::filesystem::path> includedFiles;
    const EntryPoint* Find(const std::string& name) const;
};

// Preprocesses, parses and generates the shader model 3 code of every entry point (no bytecode yet)
Effect Parse(const Input& in);
// Compiles the entry points that have no bytecode yet. False on the first failure (errors appended).
bool Compile(Effect& e, const HlslCompiler& compiler);
// The first line of the first error, without the file path ("error X3004: undeclared identifier 'x'")
std::string FirstError(const std::string& errors);

// UTF-8 text of a path (C++17 and C++20)
std::string Utf8(const std::filesystem::path& p);

// Annotation helpers
const reshadefx::annotation* FindAnnotation(const std::vector<reshadefx::annotation>& list, const char* name);
std::string AnnotationString(const std::vector<reshadefx::annotation>& list, const char* name);
float AnnotationFloat(const std::vector<reshadefx::annotation>& list, const char* name, float fallback, int component = 0);
int AnnotationInt(const std::vector<reshadefx::annotation>& list, const char* name, int fallback, int component = 0);

// ---- ReShade preset .ini (global keys before the first section, one [Effect.fx] section per effect) ----
struct Preset {
    std::map<std::string, std::map<std::string, std::string>> sections; // "" = global keys
    std::vector<std::string> sectionOrder;

    std::string Get(const std::string& section, const std::string& key) const;
    void Set(const std::string& section, const std::string& key, const std::string& value);
    void Erase(const std::string& section, const std::string& key);
    bool Has(const std::string& section, const std::string& key) const;
};
Preset ParsePreset(const std::string& text);
std::string WritePreset(const Preset& p);
std::vector<std::string> SplitList(const std::string& value); // "a,b,c" (ReShade's list values)
std::string JoinList(const std::vector<std::string>& items);
// "KEY=VALUE,KEY2" -> pairs (a bare key means "1")
std::vector<std::pair<std::string, std::string>> ParseDefinitions(const std::string& value);

} // namespace ReShadeFxCompile
