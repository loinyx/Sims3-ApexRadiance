// ReShade FX effects inside Apex Radiance: the runtime (see reshade_fx.h). Mirrors ReShade's own D3D9 path
// (source/runtime.cpp render_technique and the d3d9 back end, crosire/reshade 7bf9de8) without its API layer:
//  - before the first pass of a technique and after every pass that drew to the back buffer, the back buffer is copied
//    to the COLOR texture (StretchRect);
//  - a pass draws its vertex count with the vertex id in TEXCOORD0 (DrawPrimitiveUP, which no Apex hook sees);
//  - the uniforms go to float constants c0.. of both shaders, __TEXEL_SIZE__ to vertex c255, the COLOR / DEPTH pixel
//    sizes to their semantic registers;
//  - the whole device state is captured in a state block (plus the render targets and depth-stencil, which a state
//    block does not hold) and applied back after the last technique.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "reshade_fx.h"
#include "reshade_fx_compile.h"
#include "reshade_fx_internal.h"
#include <windows.h>
#include <d3dcompiler.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <ctime>
#include <deque>
#include <format>
#include <map>
#include <memory>
#include <mutex>
#include "apex_log.h"
#include "apex_util.h"
#include "d3d9_extra_hooks.h"
#include "d3d9_hooks.h"
#include "depth_share.h"
#include "hook_guard.h"
#include "overlay.h"
#include "post_scene.h"

namespace ReShadeFx {
namespace {

namespace fs = std::filesystem;
using namespace ReShadeFxCompile;
using namespace Detail;

constexpr const char* kDefaultPreset = "Default.ini";
constexpr DWORD kMaxSamplers = 16;
constexpr DWORD kMaxTargets = 4;

template <typename T> void SafeRelease(T*& p) {
    if (p) {
        p->Release();
        p = nullptr;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Worker: parses every effect file, compiles the ones that are on, decodes their texture images
// ---------------------------------------------------------------------------------------------------------------------
struct TexImage {
    std::string uniqueName;
    Image image;
};

struct Job {
    uint64_t generation = 0;
    Input input;
    bool compile = false;
    std::shared_ptr<Effect> parsed; // set: compile this parsed effect (no new parse)
};

struct Result {
    uint64_t generation = 0;
    fs::path file;
    std::shared_ptr<Effect> effect;
    bool compileRequested = false;
    std::vector<TexImage> images;
    std::string imageError;
};

struct Worker {
    std::mutex lock;
    std::deque<Job> jobs;
    std::vector<Result> results;
    bool running = false;
};
std::shared_ptr<Worker> g_worker = std::make_shared<Worker>();

bool CompileCached(const std::string& hlsl, const std::string& entry, const char* target, std::string& bytecode, std::string& errors) {
    const uint64_t hash = Hash64(target, entry, hlsl);
    if (ReadCache(hash, bytecode)) return true;
    ID3DBlob *code = nullptr, *msgs = nullptr;
    const HRESULT hr = D3DCompile(hlsl.data(), hlsl.size(), nullptr, nullptr, nullptr, entry.c_str(), target, D3DCOMPILE_OPTIMIZATION_LEVEL1, 0, &code, &msgs);
    if (msgs) {
        errors.assign(static_cast<const char*>(msgs->GetBufferPointer()), msgs->GetBufferSize());
        while (!errors.empty() && errors.back() == '\0') errors.pop_back();
        msgs->Release();
    }
    if (code) {
        bytecode.assign(static_cast<const char*>(code->GetBufferPointer()), code->GetBufferSize());
        code->Release();
    }
    if (FAILED(hr) || bytecode.empty()) return false;
    WriteCache(hash, bytecode);
    return true;
}

void LoadImages(const Effect& e, Result& r) {
    for (const auto& tex : e.module.textures) {
        const std::string source = AnnotationString(tex.annotations, "source");
        if (source.empty() || !tex.semantic.empty()) continue;
        const fs::path path = FindTexture(source);
        TexImage ti;
        ti.uniqueName = tex.unique_name;
        std::string error;
        if (path.empty()) {
            if (r.imageError.empty()) r.imageError = "texture file " + source + " is missing";
        } else if (!LoadImageFile(path, tex.width, tex.height, ti.image, error)) {
            if (r.imageError.empty()) r.imageError = error;
        }
        r.images.push_back(std::move(ti)); // an empty image stays black, as in ReShade
    }
}

void WorkerLoop(std::shared_ptr<Worker> w) {
    for (;;) {
        Job job;
        {
            std::lock_guard<std::mutex> lock(w->lock);
            if (w->jobs.empty()) {
                w->running = false;
                return;
            }
            job = std::move(w->jobs.front());
            w->jobs.pop_front();
        }
        Result r;
        r.generation = job.generation;
        r.file = job.input.file;
        r.compileRequested = job.compile;
        const auto t0 = std::chrono::steady_clock::now();
        auto e = job.parsed ? std::make_shared<Effect>(*job.parsed) : std::make_shared<Effect>(Parse(job.input));
        if (e->parsed && job.compile && Compile(*e, CompileCached)) LoadImages(*e, r);
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        if (!e->parsed || (job.compile && !e->compiled))
            LOG_WARNING(std::format("[ReShade] {} did not compile: {}", e->fileName, FirstError(e->errors)));
        else
            LOG_INFO(std::format("[ReShade] {} {} in {:.0f} ms", e->fileName, job.compile ? "compiled" : "read", ms));
        r.effect = std::move(e);
        std::lock_guard<std::mutex> lock(w->lock);
        w->results.push_back(std::move(r));
    }
}

void Queue(Job job) {
    bool start = false;
    {
        std::lock_guard<std::mutex> lock(g_worker->lock);
        g_worker->jobs.push_back(std::move(job));
        if (!g_worker->running) g_worker->running = start = true;
    }
    if (start && !HookGuard::StartDetached("ReShade: effect compiler", [w = g_worker] { WorkerLoop(w); })) {
        std::lock_guard<std::mutex> lock(g_worker->lock);
        g_worker->running = false;
    }
}

// ---------------------------------------------------------------------------------------------------------------------
// Runtime state (render thread)
// ---------------------------------------------------------------------------------------------------------------------
struct Tex {
    reshadefx::texture info;
    IDirect3DTexture9* tex = nullptr;
    IDirect3DSurface9* surface = nullptr; // level 0 of a render target
    D3DFORMAT format = D3DFMT_UNKNOWN;
    bool fromImage = false; // MANAGED: survives a Reset
    bool autoMips = false;
    void Release() {
        SafeRelease(surface);
        SafeRelease(tex);
    }
    ~Tex() { Release(); }
};

struct Pass {
    const reshadefx::pass* desc = nullptr;
    IDirect3DVertexShader9* vs = nullptr; // owned by the effect's shader maps
    IDirect3DPixelShader9* ps = nullptr;
    std::vector<uint32_t> vsSamplers;
    Tex* targets[kMaxTargets] = {};
    int targetCount = 0;
};

struct Tech {
    std::string name, key, label, tooltip;
    bool hidden = false, enabled = false;
    int timeoutMs = 0, timeLeftMs = 0;
    int toggleKey = 0;
    bool toggleCtrl = false, toggleShift = false, toggleAlt = false, toggleWasDown = false;
    size_t index = 0; // in module.techniques
    std::vector<Pass> passes;
};

enum class Special { None, FrameTime, FrameCount, Random, PingPong, Date, Timer, Key, MousePoint, MouseDelta, MouseButton, MouseWheel, OverlayOpen, Zero };

struct FxEffect {
    fs::path path;
    std::string file;
    std::shared_ptr<Effect> fx;
    std::string error;
    bool parsing = true, compiling = false, created = false;
    std::vector<float> data; // uniform constants (shader model 3: every value a float)
    std::vector<Special> specials; // per module uniform
    std::vector<Tech> techs;
    std::vector<std::shared_ptr<Tex>> textures;
    std::map<std::string, IDirect3DVertexShader9*> vs;
    std::map<std::string, IDirect3DPixelShader9*> ps;
    std::vector<TexImage> images; // decoded by the worker, uploaded at creation
    bool usesDepth = false;

    bool AnyEnabled() const {
        return std::any_of(techs.begin(), techs.end(), [](const Tech& t) { return t.enabled; });
    }
    void ReleaseObjects() {
        for (auto& [_, s] : vs) SafeRelease(s);
        for (auto& [_, s] : ps) SafeRelease(s);
        vs.clear();
        ps.clear();
        for (auto& t : techs) t.passes.clear();
        textures.clear();
        created = false;
    }
    ~FxEffect() { ReleaseObjects(); }
};

struct Runtime {
    bool started = false;
    uint64_t generation = 0;
    uint32_t width = 0, height = 0; // the effects are compiled for this back buffer size
    bool scanned = false;           // the folder was read for this generation
    std::vector<std::unique_ptr<FxEffect>> effects;
    std::vector<std::string> order; // technique keys, render order
    Preset preset;
    std::string presetName = kDefaultPreset;
    HWND window = nullptr; // the device's focus window (mouse position)
    bool presetLoaded = false, presetDirty = false;
    std::map<std::string, std::weak_ptr<Tex>> shared; // textures by unique name (effects share them)
    // device objects
    IDirect3DTexture9* color = nullptr;
    IDirect3DSurface9* colorSurface = nullptr;
    D3DFORMAT colorFormat = D3DFMT_UNKNOWN;
    IDirect3DSurface9* stencil = nullptr;
    IDirect3DStateBlock9* stateBlock = nullptr;
    IDirect3DVertexDeclaration9* decl = nullptr;
    std::vector<float> vertexIds;
    // timing and input
    LARGE_INTEGER start{}, lastFrame{}, freq{};
    float frameMs = 0.0f;
    uint32_t frameCount = 0;
    int framesThisSecond = 0, framesLastSecond = 0;
    unsigned long long secondStart = 0;
    bool wantsDepth = false;
    POINT lastMouse{};
    std::string headerError;
};
Runtime g;
std::atomic<int> g_state{static_cast<int>(State::Off)};

// CurrentPreset's copy of g.presetName: the config is saved from any thread
std::mutex g_presetNameLock;
std::string g_presetName = kDefaultPreset;

void SetState(State s) { g_state.store(static_cast<int>(s)); }

std::wstring PresetPath(const std::string& name) { return Sub(L"Presets") + ApexUtil::ToWide(name); }

// ---- uniform values (shader model 3 layout: float i of a uniform at offset / 4 + i; array elements and matrix rows
// take one register each, as ReShade's get/set_uniform_value_data) ----
size_t ValueIndex(const reshadefx::uniform& u, size_t element, size_t row, size_t col) {
    const size_t base = u.offset / 4;
    if (u.type.is_matrix()) return base + (element * u.type.rows * 4 + row * 4 + col);
    if (u.type.is_array()) return base + element * 4 + row;
    return base + row;
}

size_t Components(const reshadefx::uniform& u) { return std::min<size_t>(u.type.components(), 16); }

float ConstantAsFloat(const reshadefx::type& t, const reshadefx::constant& c, size_t i) {
    if (t.is_floating_point()) return c.as_float[i];
    if (t.is_signed()) return static_cast<float>(c.as_int[i]);
    return static_cast<float>(c.as_uint[i]);
}

void WriteValues(FxEffect& fx, const reshadefx::uniform& u, const float* values, size_t count, size_t element = 0) {
    const size_t rows = u.type.rows, cols = std::max<size_t>(u.type.cols, 1);
    for (size_t i = 0; i < count && i < rows * cols; i++) {
        const size_t idx = u.type.is_matrix() ? ValueIndex(u, element, i / cols, i % cols) : ValueIndex(u, element, i, 0);
        if (idx < fx.data.size()) fx.data[idx] = values[i];
    }
}

void ReadValues(const FxEffect& fx, const reshadefx::uniform& u, float* values, size_t count, size_t element = 0) {
    const size_t rows = u.type.rows, cols = std::max<size_t>(u.type.cols, 1);
    for (size_t i = 0; i < count && i < rows * cols; i++) {
        const size_t idx = u.type.is_matrix() ? ValueIndex(u, element, i / cols, i % cols) : ValueIndex(u, element, i, 0);
        values[i] = idx < fx.data.size() ? fx.data[idx] : 0.0f;
    }
}

void ResetValue(FxEffect& fx, const reshadefx::uniform& u) {
    const size_t n = Components(u);
    const size_t elements = u.type.is_array() ? std::min<size_t>(u.type.array_length, 1024) : 1;
    for (size_t a = 0; a < elements; a++) {
        float values[16] = {};
        if (u.has_initializer_value) {
            const reshadefx::constant& c = u.type.is_array() ? (a < u.initializer_value.array_data.size() ? u.initializer_value.array_data[a] : reshadefx::constant{})
                                                             : u.initializer_value;
            for (size_t i = 0; i < n; i++) values[i] = ConstantAsFloat(u.type, c, i);
        }
        WriteValues(fx, u, values, n, a);
    }
}

Special SpecialOf(const reshadefx::uniform& u) {
    const std::string s = AnnotationString(u.annotations, "source");
    if (s.empty()) return Special::None;
    if (s == "frametime") return Special::FrameTime;
    if (s == "framecount") return Special::FrameCount;
    if (s == "random") return Special::Random;
    if (s == "pingpong") return Special::PingPong;
    if (s == "date") return Special::Date;
    if (s == "timer") return Special::Timer;
    if (s == "key") return Special::Key;
    if (s == "mousepoint") return Special::MousePoint;
    if (s == "mousedelta") return Special::MouseDelta;
    if (s == "mousebutton") return Special::MouseButton;
    if (s == "mousewheel") return Special::MouseWheel;
    if (s == "overlay_open" || s == "ui_open") return Special::OverlayOpen;
    return Special::Zero;
}

std::string FormatFloat(float v) {
    std::string s = std::format("{:.6f}", v);
    return s;
}

// The preset's values of one effect (keys = uniform names), over the defaults
void ApplyPresetValues(FxEffect& fx) {
    if (!fx.fx || !fx.fx->parsed) return;
    for (size_t i = 0; i < fx.fx->module.uniforms.size(); i++) {
        const auto& u = fx.fx->module.uniforms[i];
        if (fx.specials[i] != Special::None) continue;
        ResetValue(fx, u);
        if (u.type.is_array() || AnnotationInt(u.annotations, "nosave", 0)) continue;
        const std::string text = g.preset.Get(fx.file, u.name);
        if (text.empty()) continue;
        const auto items = SplitList(text);
        float values[16] = {};
        ReadValues(fx, u, values, Components(u));
        for (size_t c = 0; c < items.size() && c < Components(u); c++) {
            char* end = nullptr;
            const float v = std::strtof(items[c].c_str(), &end);
            if (end != items[c].c_str()) values[c] = u.type.is_integral() && !u.type.is_floating_point() ? std::round(v) : v;
        }
        WriteValues(fx, u, values, Components(u));
    }
}

void WritePresetValues(const FxEffect& fx) {
    if (!fx.fx || !fx.fx->parsed) return;
    for (size_t i = 0; i < fx.fx->module.uniforms.size(); i++) {
        const auto& u = fx.fx->module.uniforms[i];
        if (fx.specials[i] != Special::None || u.type.is_array() || AnnotationInt(u.annotations, "nosave", 0)) continue;
        float values[16] = {};
        const size_t n = Components(u);
        ReadValues(fx, u, values, n);
        std::vector<std::string> items;
        for (size_t c = 0; c < n; c++)
            items.push_back(u.type.is_floating_point() ? FormatFloat(values[c]) : std::to_string(static_cast<long long>(std::lround(values[c]))));
        g.preset.Set(fx.file, u.name, JoinList(items));
    }
}

// ---- which techniques are on (the preset's Techniques list, or an "enabled" annotation) ----
bool ListedIn(const std::vector<std::string>& list, const Tech& t) {
    return std::find(list.begin(), list.end(), t.key) != list.end() || std::find(list.begin(), list.end(), t.name) != list.end();
}

void ApplyPresetTechniques(FxEffect& fx) {
    const auto list = SplitList(g.preset.Get("", "Techniques"));
    for (auto& t : fx.techs) {
        const auto& desc = fx.fx->module.techniques[t.index];
        t.enabled = AnnotationInt(desc.annotations, "enabled", 0) != 0 || ListedIn(list, t);
        t.timeLeftMs = t.timeoutMs;
        const auto key = SplitList(g.preset.Get("", "Key" + t.key));
        const auto keyByName = key.empty() ? SplitList(g.preset.Get("", "Key" + t.name)) : key;
        if (!keyByName.empty()) {
            t.toggleKey = std::atoi(keyByName[0].c_str());
            t.toggleCtrl = keyByName.size() > 1 && keyByName[1] != "0";
            t.toggleShift = keyByName.size() > 2 && keyByName[2] != "0";
            t.toggleAlt = keyByName.size() > 3 && keyByName[3] != "0";
        } else {
            t.toggleKey = AnnotationInt(desc.annotations, "toggle", 0);
            t.toggleCtrl = AnnotationInt(desc.annotations, "togglectrl", 0) || AnnotationInt(desc.annotations, "toggle_ctrl", 0);
            t.toggleShift = AnnotationInt(desc.annotations, "toggleshift", 0) || AnnotationInt(desc.annotations, "toggle_shift", 0);
            t.toggleAlt = AnnotationInt(desc.annotations, "togglealt", 0) || AnnotationInt(desc.annotations, "toggle_alt", 0);
        }
    }
}

void SortOrder() {
    std::vector<std::string> sorting = SplitList(g.preset.Get("", "TechniqueSorting"));
    if (sorting.empty()) sorting = SplitList(g.preset.Get("", "Techniques"));
    std::vector<std::string> keys;
    for (const auto& fx : g.effects)
        for (const auto& t : fx->techs) keys.push_back(t.key);
    auto rank = [&](const std::string& key) -> size_t {
        auto it = std::find(sorting.begin(), sorting.end(), key);
        if (it == sorting.end()) it = std::find(sorting.begin(), sorting.end(), key.substr(0, key.find('@')));
        return static_cast<size_t>(it - sorting.begin());
    };
    std::stable_sort(keys.begin(), keys.end(), [&](const std::string& a, const std::string& b) { return rank(a) < rank(b); });
    g.order = std::move(keys);
}

FxEffect* FindEffect(const std::string& file) {
    for (auto& fx : g.effects)
        if (fx->file == file) return fx.get();
    return nullptr;
}

std::pair<FxEffect*, Tech*> FindTech(const std::string& key) {
    for (auto& fx : g.effects)
        for (auto& t : fx->techs)
            if (t.key == key) return {fx.get(), &t};
    return {nullptr, nullptr};
}

// ---- loading ----
Input MakeInput(const fs::path& file) {
    Input in;
    in.file = file;
    in.includes = IncludeFolders();
    in.width = g.width;
    in.height = g.height;
    const std::string name = Utf8(file.filename());
    for (auto& d : ParseDefinitions(g.preset.Get(name, "PreprocessorDefinitions"))) in.definitions.push_back(std::move(d));
    for (auto& d : ParseDefinitions(g.preset.Get("", "PreprocessorDefinitions"))) in.definitions.push_back(std::move(d));
    // Sims 3 depth is not reversed (d = A - near * A / z); ReShade.fxh assumes reversed depth unless told otherwise
    in.definitions.emplace_back("RESHADE_DEPTH_INPUT_IS_REVERSED", "0");
    return in;
}

bool EffectWanted(const fs::path& file) {
    // compile now when the preset turns one of its techniques on (known only from the names before the parse)
    const std::string name = Utf8(file.filename());
    for (const auto& item : SplitList(g.preset.Get("", "Techniques"))) {
        const size_t at = item.find('@');
        if (at == std::string::npos || Lower(item.substr(at + 1)) == Lower(name)) return true;
    }
    return false;
}

void LoadPreset() {
    g.preset = Preset{};
    std::string text;
    if (ApexUtil::ReadFileBytes(PresetPath(g.presetName), text)) g.preset = ParsePreset(text);
    g.presetLoaded = true;
    g.presetDirty = false;
}

void StartLoading() {
    g.generation++;
    g.scanned = true;
    for (auto& fx : g.effects) fx->ReleaseObjects();
    g.effects.clear();
    g.order.clear();
    if (!g.presetLoaded) LoadPreset();
    std::error_code ec;
    for (const wchar_t* sub : {L"Shaders", L"Textures", L"Presets", L"Cache", L"Internal"}) fs::create_directories(Sub(sub), ec);
    g.headerError.clear();
    if (!EnsureInternalHeaders(g.headerError)) LOG_WARNING("[ReShade] Could not write the built-in headers: " + g.headerError);
    for (const fs::path& file : ScanEffects()) {
        auto fx = std::make_unique<FxEffect>();
        fx->path = file;
        fx->file = Utf8(file.filename());
        Job job;
        job.generation = g.generation;
        job.input = MakeInput(file);
        job.compile = EffectWanted(file);
        fx->compiling = job.compile;
        g.effects.push_back(std::move(fx));
        Queue(std::move(job));
    }
    SetState(g.effects.empty() ? State::Empty : State::Loading);
    LOG_INFO(std::format("[ReShade] Reading {} effect files for {}x{}", g.effects.size(), g.width, g.height));
}

void RequestCompile(FxEffect& fx) {
    if (fx.compiling || !fx.fx || !fx.fx->parsed || fx.fx->compiled) return;
    fx.compiling = true;
    Job job;
    job.generation = g.generation;
    job.input = MakeInput(fx.path);
    job.compile = true;
    job.parsed = fx.fx;
    Queue(std::move(job));
    SetState(State::Loading);
}

void TakeResult(Result& r) {
    if (r.generation != g.generation) return;
    FxEffect* fx = nullptr;
    for (auto& e : g.effects)
        if (e->path == r.file) fx = e.get();
    if (!fx) return;
    const bool newParse = !fx->fx || !fx->fx->parsed;
    fx->ReleaseObjects();
    fx->fx = std::move(r.effect);
    fx->parsing = false;
    fx->compiling = false;
    fx->images = std::move(r.images);
    fx->error.clear();
    if (!fx->fx->parsed || (r.compileRequested && !fx->fx->compiled)) fx->error = FirstError(fx->fx->errors);
    if (fx->error.empty() && !r.imageError.empty()) LOG_WARNING("[ReShade] " + fx->file + ": " + r.imageError);
    if (!fx->fx->parsed) {
        fx->techs.clear();
        return;
    }
    if (newParse) {
        const auto& m = fx->fx->module;
        fx->data.assign((m.total_uniform_size / 4 + 3) & ~size_t(3), 0.0f);
        fx->specials.clear();
        for (const auto& u : m.uniforms) fx->specials.push_back(SpecialOf(u));
        fx->techs.clear();
        for (size_t i = 0; i < m.techniques.size(); i++) {
            const auto& desc = m.techniques[i];
            Tech t;
            t.index = i;
            t.name = desc.name;
            t.key = desc.name + "@" + fx->file;
            t.label = AnnotationString(desc.annotations, "ui_label");
            if (t.label.empty()) t.label = desc.name;
            t.tooltip = AnnotationString(desc.annotations, "ui_tooltip");
            t.hidden = AnnotationInt(desc.annotations, "hidden", 0) != 0;
            t.timeoutMs = AnnotationInt(desc.annotations, "timeout", 0);
            fx->techs.push_back(std::move(t));
        }
        fx->usesDepth = false;
        for (const auto& s : m.samplers)
            for (const auto& tex : m.textures)
                if (tex.unique_name == s.texture_name && tex.semantic == "DEPTH") fx->usesDepth = true;
        ApplyPresetValues(*fx);
        ApplyPresetTechniques(*fx);
        SortOrder();
    }
    if (fx->AnyEnabled() && !fx->fx->compiled && fx->error.empty()) RequestCompile(*fx);
}

// ---------------------------------------------------------------------------------------------------------------------
// Device objects
// ---------------------------------------------------------------------------------------------------------------------
bool FormatSupported(IDirect3DDevice9* dev, D3DFORMAT fmt, DWORD usage) {
    IDirect3D9* d3d = nullptr;
    if (FAILED(dev->GetDirect3D(&d3d)) || !d3d) return false;
    D3DDEVICE_CREATION_PARAMETERS cp{};
    D3DDISPLAYMODE mode{};
    dev->GetCreationParameters(&cp);
    const bool ok = SUCCEEDED(d3d->GetAdapterDisplayMode(cp.AdapterOrdinal, &mode)) &&
                    SUCCEEDED(d3d->CheckDeviceFormat(cp.AdapterOrdinal, cp.DeviceType, mode.Format, usage, D3DRTYPE_TEXTURE, fmt));
    d3d->Release();
    return ok;
}

D3DFORMAT TargetFormat(IDirect3DDevice9* dev, reshadefx::texture_format f) {
    using F = reshadefx::texture_format;
    std::vector<D3DFORMAT> choices;
    switch (f) {
    case F::r8: choices = {D3DFMT_L8, D3DFMT_A8R8G8B8}; break;
    case F::r16: choices = {D3DFMT_L16, D3DFMT_G16R16, D3DFMT_A16B16G16R16F}; break;
    case F::r16f: choices = {D3DFMT_R16F, D3DFMT_G16R16F, D3DFMT_A16B16G16R16F}; break;
    case F::r32f:
    case F::r32i:
    case F::r32u: choices = {D3DFMT_R32F, D3DFMT_G32R32F, D3DFMT_A32B32G32R32F}; break;
    case F::rg8: choices = {D3DFMT_G16R16, D3DFMT_A8R8G8B8}; break; // D3D9 has no two-channel 8-bit target
    case F::rg16: choices = {D3DFMT_G16R16, D3DFMT_A16B16G16R16}; break;
    case F::rg16f: choices = {D3DFMT_G16R16F, D3DFMT_A16B16G16R16F}; break;
    case F::rg32f: choices = {D3DFMT_G32R32F, D3DFMT_A32B32G32R32F}; break;
    case F::rgba16: choices = {D3DFMT_A16B16G16R16, D3DFMT_A16B16G16R16F}; break;
    case F::rgba16f: choices = {D3DFMT_A16B16G16R16F}; break;
    case F::rgba32f:
    case F::rgba32i:
    case F::rgba32u: choices = {D3DFMT_A32B32G32R32F, D3DFMT_A16B16G16R16F}; break;
    case F::rgb10a2: choices = {D3DFMT_A2R10G10B10, D3DFMT_A2B10G10R10, D3DFMT_A16B16G16R16F}; break;
    case F::rg11b10f: choices = {D3DFMT_A16B16G16R16F}; break;
    default: choices = {D3DFMT_A8R8G8B8}; break;
    }
    choices.push_back(D3DFMT_A8R8G8B8);
    for (D3DFORMAT fmt : choices)
        if (FormatSupported(dev, fmt, D3DUSAGE_RENDERTARGET)) return fmt;
    return D3DFMT_A8R8G8B8;
}

// (Re)creates a texture's Direct3D object: a render target, cleared to zero, or a MANAGED texture holding its image
bool EnsureTexture(IDirect3DDevice9* dev, Tex& t, const Image* image, std::string& error) {
    if (t.tex) return true;
    if (t.info.type == reshadefx::texture_type::texture_3d) {
        error = t.info.unique_name + ": 3D textures are not supported in Direct3D 9 effects here";
        return false;
    }
    const UINT w = std::max<UINT>(t.info.width, 1), h = t.info.type == reshadefx::texture_type::texture_1d ? 1 : std::max<UINT>(t.info.height, 1);
    t.autoMips = t.info.levels > 1;
    if (t.fromImage) {
        const DWORD usage = t.autoMips ? D3DUSAGE_AUTOGENMIPMAP : 0;
        if (FAILED(dev->CreateTexture(w, h, t.autoMips ? 0 : 1, usage, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t.tex, nullptr)) || !t.tex) {
            t.tex = nullptr;
            error = t.info.unique_name + ": could not create the texture";
            return false;
        }
        t.format = D3DFMT_A8R8G8B8;
        D3DLOCKED_RECT lr{};
        if (SUCCEEDED(t.tex->LockRect(0, &lr, nullptr, 0))) {
            for (UINT y = 0; y < h; y++) {
                uint8_t* row = static_cast<uint8_t*>(lr.pBits) + static_cast<size_t>(y) * lr.Pitch;
                if (image && image->width == w && image->height >= h && !image->bgra.empty())
                    std::memcpy(row, image->bgra.data() + static_cast<size_t>(y) * w * 4, static_cast<size_t>(w) * 4);
                else
                    std::memset(row, 0, static_cast<size_t>(w) * 4);
            }
            t.tex->UnlockRect(0);
        }
        if (t.autoMips) t.tex->GenerateMipSubLevels();
        return true;
    }
    t.format = TargetFormat(dev, t.info.format);
    DWORD usage = D3DUSAGE_RENDERTARGET;
    if (t.autoMips && FormatSupported(dev, t.format, D3DUSAGE_RENDERTARGET | D3DUSAGE_AUTOGENMIPMAP)) usage |= D3DUSAGE_AUTOGENMIPMAP;
    else t.autoMips = false;
    if (FAILED(dev->CreateTexture(w, h, t.autoMips ? 0 : 1, usage, t.format, D3DPOOL_DEFAULT, &t.tex, nullptr)) || !t.tex) {
        t.tex = nullptr;
        error = std::format("{}: could not create a {}x{} render target", t.info.unique_name, w, h);
        return false;
    }
    t.tex->GetSurfaceLevel(0, &t.surface);
    if (t.surface) dev->ColorFill(t.surface, nullptr, 0);
    return true;
}

template <typename Shader, typename Create> Shader* CreateShader(const EntryPoint& ep, Create create) {
    Shader* s = nullptr;
    if (ep.bytecode.empty() || FAILED(create(reinterpret_cast<const DWORD*>(ep.bytecode.data()), &s))) return nullptr;
    return s;
}

bool CreateEffectObjects(IDirect3DDevice9* dev, FxEffect& fx) {
    if (fx.created) return true;
    if (!fx.fx || !fx.fx->compiled) return false;
    const auto& m = fx.fx->module;
    std::string error;
    // textures (shared with other effects by unique name)
    std::map<std::string, Tex*> byName;
    for (const auto& info : m.textures) {
        if (!info.semantic.empty()) continue;
        std::shared_ptr<Tex> t;
        if (auto it = g.shared.find(info.unique_name); it != g.shared.end()) t = it->second.lock();
        if (t && (t->info.width != info.width || t->info.height != info.height || t->info.format != info.format || t->info.levels != info.levels)) {
            fx.error = info.unique_name + ": another effect already uses this texture with a different size or format";
            fx.textures.clear();
            return false;
        }
        if (!t) {
            t = std::make_shared<Tex>();
            t->info = info;
            const auto image = std::find_if(fx.images.begin(), fx.images.end(), [&](const TexImage& i) { return i.uniqueName == info.unique_name; });
            t->fromImage = image != fx.images.end();
            if (!EnsureTexture(dev, *t, t->fromImage ? &image->image : nullptr, error)) {
                fx.error = error;
                fx.textures.clear();
                return false;
            }
            g.shared[info.unique_name] = t;
        }
        byName[info.unique_name] = t.get();
        fx.textures.push_back(std::move(t));
    }
    fx.images.clear(); // uploaded (MANAGED textures keep their copy)
    // shaders
    for (const auto& ep : fx.fx->entryPoints) {
        if (ep.type == reshadefx::shader_type::vertex) {
            auto* s = CreateShader<IDirect3DVertexShader9>(ep, [&](const DWORD* code, IDirect3DVertexShader9** out) { return D3D9Hooks::CallOriginalCreateVertexShader(dev, code, out); });
            if (!s) error = ep.name + ": could not create the vertex shader";
            fx.vs[ep.name] = s;
        } else {
            auto* s = CreateShader<IDirect3DPixelShader9>(ep, [&](const DWORD* code, IDirect3DPixelShader9** out) { return D3D9Hooks::CallOriginalCreatePixelShader(dev, code, out); });
            if (!s) error = ep.name + ": could not create the pixel shader";
            fx.ps[ep.name] = s;
        }
        if (!error.empty()) break;
    }
    // passes
    for (auto& t : fx.techs) {
        if (!error.empty()) break;
        t.passes.clear();
        for (const auto& desc : m.techniques[t.index].passes) {
            Pass p;
            p.desc = &desc;
            p.vs = fx.vs[desc.vs_entry_point];
            p.ps = desc.ps_entry_point.empty() ? nullptr : fx.ps[desc.ps_entry_point];
            if (const EntryPoint* ep = fx.fx->Find(desc.vs_entry_point)) p.vsSamplers = ep->samplers;
            for (int i = 0; i < 8 && !desc.render_target_names[i].empty(); i++) {
                if (i >= static_cast<int>(kMaxTargets)) {
                    error = "a pass writes more than 4 render targets";
                    break;
                }
                auto it = byName.find(desc.render_target_names[i]);
                if (it == byName.end()) {
                    error = desc.render_target_names[i] + ": unknown render target";
                    break;
                }
                p.targets[p.targetCount++] = it->second;
            }
            if (!p.vs) error = "a pass has no vertex shader";
            t.passes.push_back(p);
        }
    }
    if (!error.empty()) {
        fx.error = error;
        fx.ReleaseObjects();
        return false;
    }
    fx.created = true;
    return true;
}

void ReleaseDefaultPool() {
    for (auto& fx : g.effects)
        for (auto& t : fx->textures)
            if (!t->fromImage) t->Release();
    SafeRelease(g.colorSurface);
    SafeRelease(g.color);
    SafeRelease(g.stencil);
    SafeRelease(g.stateBlock);
}

bool EnsureShared(IDirect3DDevice9* dev, const D3DSURFACE_DESC& bb) {
    if (!g.color || g.colorFormat != bb.Format) {
        SafeRelease(g.colorSurface);
        SafeRelease(g.color);
        if (FAILED(dev->CreateTexture(bb.Width, bb.Height, 1, D3DUSAGE_RENDERTARGET, bb.Format, D3DPOOL_DEFAULT, &g.color, nullptr)) || !g.color) {
            g.color = nullptr;
            return false;
        }
        g.color->GetSurfaceLevel(0, &g.colorSurface);
        g.colorFormat = bb.Format;
    }
    if (!g.stateBlock && FAILED(dev->CreateStateBlock(D3DSBT_ALL, &g.stateBlock))) g.stateBlock = nullptr;
    if (!g.decl) {
        const D3DVERTEXELEMENT9 elements[] = {{0, 0, D3DDECLTYPE_FLOAT1, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0}, D3DDECL_END()};
        if (FAILED(dev->CreateVertexDeclaration(elements, &g.decl))) g.decl = nullptr;
    }
    return g.colorSurface && g.stateBlock && g.decl;
}

IDirect3DSurface9* Stencil(IDirect3DDevice9* dev, const D3DSURFACE_DESC& bb) {
    if (!g.stencil && FAILED(dev->CreateDepthStencilSurface(bb.Width, bb.Height, D3DFMT_D24S8, bb.MultiSampleType, bb.MultiSampleQuality, FALSE, &g.stencil, nullptr)))
        g.stencil = nullptr;
    return g.stencil;
}

// ---------------------------------------------------------------------------------------------------------------------
// Rendering
// ---------------------------------------------------------------------------------------------------------------------
D3DBLEND Blend(reshadefx::blend_factor f) {
    using B = reshadefx::blend_factor;
    switch (f) {
    case B::zero: return D3DBLEND_ZERO;
    case B::one: return D3DBLEND_ONE;
    case B::source_color: return D3DBLEND_SRCCOLOR;
    case B::one_minus_source_color: return D3DBLEND_INVSRCCOLOR;
    case B::dest_color: return D3DBLEND_DESTCOLOR;
    case B::one_minus_dest_color: return D3DBLEND_INVDESTCOLOR;
    case B::source_alpha: return D3DBLEND_SRCALPHA;
    case B::one_minus_source_alpha: return D3DBLEND_INVSRCALPHA;
    case B::dest_alpha: return D3DBLEND_DESTALPHA;
    case B::one_minus_dest_alpha: return D3DBLEND_INVDESTALPHA;
    }
    return D3DBLEND_ONE;
}

D3DBLENDOP BlendOp(reshadefx::blend_op op) {
    using O = reshadefx::blend_op;
    switch (op) {
    case O::subtract: return D3DBLENDOP_SUBTRACT;
    case O::reverse_subtract: return D3DBLENDOP_REVSUBTRACT;
    case O::min: return D3DBLENDOP_MIN;
    case O::max: return D3DBLENDOP_MAX;
    default: return D3DBLENDOP_ADD;
    }
}

D3DSTENCILOP StencilOp(reshadefx::stencil_op op) {
    using S = reshadefx::stencil_op;
    switch (op) {
    case S::zero: return D3DSTENCILOP_ZERO;
    case S::replace: return D3DSTENCILOP_REPLACE;
    case S::increment_saturate: return D3DSTENCILOP_INCRSAT;
    case S::decrement_saturate: return D3DSTENCILOP_DECRSAT;
    case S::invert: return D3DSTENCILOP_INVERT;
    case S::increment: return D3DSTENCILOP_INCR;
    case S::decrement: return D3DSTENCILOP_DECR;
    default: return D3DSTENCILOP_KEEP;
    }
}

D3DCMPFUNC StencilFunc(reshadefx::stencil_func f) { return static_cast<D3DCMPFUNC>(static_cast<int>(f) + 1); } // never = 1 ... always = 8

void SamplerStates(IDirect3DDevice9* dev, DWORD stage, const reshadefx::sampler& s, bool srgb) {
    const int f = static_cast<int>(s.filter);
    const bool aniso = s.filter == reshadefx::filter_mode::anisotropic;
    dev->SetSamplerState(stage, D3DSAMP_MINFILTER, aniso ? D3DTEXF_ANISOTROPIC : ((f >> 4) & 1) ? D3DTEXF_LINEAR : D3DTEXF_POINT);
    dev->SetSamplerState(stage, D3DSAMP_MAGFILTER, aniso ? D3DTEXF_ANISOTROPIC : ((f >> 2) & 1) ? D3DTEXF_LINEAR : D3DTEXF_POINT);
    dev->SetSamplerState(stage, D3DSAMP_MIPFILTER, (f & 1) ? D3DTEXF_LINEAR : D3DTEXF_POINT);
    dev->SetSamplerState(stage, D3DSAMP_ADDRESSU, static_cast<DWORD>(s.address_u)); // wrap 1, mirror 2, clamp 3, border 4: D3D9's values
    dev->SetSamplerState(stage, D3DSAMP_ADDRESSV, static_cast<DWORD>(s.address_v));
    dev->SetSamplerState(stage, D3DSAMP_ADDRESSW, static_cast<DWORD>(s.address_w));
    dev->SetSamplerState(stage, D3DSAMP_BORDERCOLOR, 0);
    dev->SetSamplerState(stage, D3DSAMP_MAXANISOTROPY, aniso ? 16 : 1);
    dev->SetSamplerState(stage, D3DSAMP_MAXMIPLEVEL, static_cast<DWORD>(std::clamp(s.min_lod, 0.0f, 15.0f)));
    float bias = s.lod_bias;
    DWORD biasBits = 0;
    std::memcpy(&biasBits, &bias, sizeof biasBits);
    dev->SetSamplerState(stage, D3DSAMP_MIPMAPLODBIAS, biasBits);
    dev->SetSamplerState(stage, D3DSAMP_SRGBTEXTURE, srgb ? TRUE : FALSE);
}

struct Frame {
    IDirect3DSurface9* bb = nullptr;
    D3DSURFACE_DESC desc{};
    IDirect3DTexture9* depth = nullptr;
    UINT depthW = 0, depthH = 0;
};

IDirect3DBaseTexture9* SamplerTexture(const FxEffect& fx, const reshadefx::sampler& s, const Frame& f, const std::map<std::string, Tex*>& byName) {
    for (const auto& tex : fx.fx->module.textures) {
        if (tex.unique_name != s.texture_name) continue;
        if (tex.semantic == "COLOR") return g.color;
        if (tex.semantic == "DEPTH") return f.depth;
        if (!tex.semantic.empty()) return nullptr;
        auto it = byName.find(tex.unique_name);
        return it != byName.end() ? it->second->tex : nullptr;
    }
    return nullptr;
}

void RenderTechnique(IDirect3DDevice9* dev, FxEffect& fx, Tech& t, const Frame& f) {
    const auto& m = fx.fx->module;
    std::map<std::string, Tex*> byName;
    for (auto& tex : fx.textures) {
        if (!tex->tex) {
            std::string error;
            if (!EnsureTexture(dev, *tex, nullptr, error)) {
                fx.error = error;
                return;
            }
        }
        byName[tex->info.unique_name] = tex.get();
    }
    // uniforms: c0.. of both shaders
    const UINT regs = static_cast<UINT>(fx.data.size() / 4);
    if (regs) {
        D3D9Hooks::CallOriginalSetVertexShaderConstantF(dev, 0, fx.data.data(), std::min<UINT>(regs, 224));
        D3D9Hooks::CallOriginalSetPixelShaderConstantF(dev, 0, fx.data.data(), std::min<UINT>(regs, 224));
    }
    bool copyBackBuffer = true; // the first pass sees the current picture
    bool stencilCleared = false;
    DWORD boundStages = 0;
    for (const Pass& p : t.passes) {
        const reshadefx::pass& d = *p.desc;
        if (copyBackBuffer) dev->StretchRect(f.bb, nullptr, g.colorSurface, nullptr, D3DTEXF_NONE);
        // render targets
        UINT vw = d.viewport_width, vh = d.viewport_height;
        if (p.targetCount == 0) {
            D3D9Hooks::CallOriginalSetRenderTarget(dev, 0, f.bb);
            for (DWORD i = 1; i < kMaxTargets; i++) D3D9Hooks::CallOriginalSetRenderTarget(dev, i, nullptr);
            vw = f.desc.Width;
            vh = f.desc.Height;
            copyBackBuffer = true;
        } else {
            for (DWORD i = 0; i < kMaxTargets; i++) D3D9Hooks::CallOriginalSetRenderTarget(dev, i, i < static_cast<DWORD>(p.targetCount) ? p.targets[i]->surface : nullptr);
            if (!vw || !vh) {
                vw = p.targets[0]->info.width;
                vh = p.targets[0]->info.height;
            }
            copyBackBuffer = false;
        }
        const bool stencil = d.stencil_enable && vw == f.desc.Width && vh == f.desc.Height;
        ExtraHooks::RawSetDepthStencilSurface(dev, stencil ? Stencil(dev, f.desc) : nullptr);
        if (stencil && !stencilCleared && g.stencil) {
            dev->Clear(0, nullptr, D3DCLEAR_STENCIL, 0, 1.0f, 0);
            stencilCleared = true;
        }
        if (d.clear_render_targets) dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1.0f, 0);
        const D3DVIEWPORT9 vp{0, 0, vw, vh, 0.0f, 1.0f};
        D3D9Hooks::CallOriginalSetViewport(dev, &vp);
        // states
        dev->SetRenderState(D3DRS_ZENABLE, D3DZB_FALSE);
        dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
        dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
        dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
        dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
        dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
        dev->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
        dev->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
        dev->SetRenderState(D3DRS_LIGHTING, FALSE);
        dev->SetRenderState(D3DRS_LASTPIXEL, TRUE);
        dev->SetRenderState(D3DRS_POINTSPRITEENABLE, FALSE);
        dev->SetRenderState(D3DRS_MULTISAMPLEANTIALIAS, FALSE);
        dev->SetRenderState(D3DRS_ALPHABLENDENABLE, d.blend_enable[0] ? TRUE : FALSE);
        dev->SetRenderState(D3DRS_SRCBLEND, Blend(d.source_color_blend_factor[0]));
        dev->SetRenderState(D3DRS_DESTBLEND, Blend(d.dest_color_blend_factor[0]));
        dev->SetRenderState(D3DRS_BLENDOP, BlendOp(d.color_blend_op[0]));
        dev->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
        dev->SetRenderState(D3DRS_SRCBLENDALPHA, Blend(d.source_alpha_blend_factor[0]));
        dev->SetRenderState(D3DRS_DESTBLENDALPHA, Blend(d.dest_alpha_blend_factor[0]));
        dev->SetRenderState(D3DRS_BLENDOPALPHA, BlendOp(d.alpha_blend_op[0]));
        dev->SetRenderState(D3DRS_COLORWRITEENABLE, d.render_target_write_mask[0]);
        dev->SetRenderState(D3DRS_COLORWRITEENABLE1, d.render_target_write_mask[1]);
        dev->SetRenderState(D3DRS_COLORWRITEENABLE2, d.render_target_write_mask[2]);
        dev->SetRenderState(D3DRS_COLORWRITEENABLE3, d.render_target_write_mask[3]);
        dev->SetRenderState(D3DRS_SRGBWRITEENABLE, d.srgb_write_enable ? TRUE : FALSE);
        dev->SetRenderState(D3DRS_STENCILENABLE, stencil ? TRUE : FALSE);
        if (stencil) {
            dev->SetRenderState(D3DRS_STENCILFUNC, StencilFunc(d.stencil_comparison_func));
            dev->SetRenderState(D3DRS_STENCILREF, d.stencil_reference_value);
            dev->SetRenderState(D3DRS_STENCILMASK, d.stencil_read_mask);
            dev->SetRenderState(D3DRS_STENCILWRITEMASK, d.stencil_write_mask);
            dev->SetRenderState(D3DRS_STENCILPASS, StencilOp(d.stencil_pass_op));
            dev->SetRenderState(D3DRS_STENCILFAIL, StencilOp(d.stencil_fail_op));
            dev->SetRenderState(D3DRS_STENCILZFAIL, StencilOp(d.stencil_depth_fail_op));
            dev->SetRenderState(D3DRS_TWOSIDEDSTENCILMODE, FALSE);
        }
        // shaders and their textures
        D3D9Hooks::CallOriginalSetVertexShader(dev, p.vs);
        D3D9Hooks::CallOriginalSetPixelShader(dev, p.ps);
        dev->SetVertexDeclaration(g.decl);
        DWORD stages = 0;
        for (const auto& b : d.sampler_bindings) {
            if (b.entry_point_binding >= kMaxSamplers || b.index >= m.samplers.size()) continue;
            const auto& s = m.samplers[b.index];
            const bool srgb = std::any_of(d.texture_bindings.begin(), d.texture_bindings.end(),
                                          [&](const reshadefx::texture_binding& tb) { return tb.entry_point_binding == b.entry_point_binding && tb.srgb; });
            IDirect3DBaseTexture9* tex = SamplerTexture(fx, s, f, byName);
            const DWORD stage = b.entry_point_binding;
            D3D9Hooks::CallOriginalSetTexture(dev, stage, tex);
            SamplerStates(dev, stage, s, srgb);
            stages |= 1u << stage;
            if (stage < 4 && std::find(p.vsSamplers.begin(), p.vsSamplers.end(), stage) != p.vsSamplers.end()) {
                D3D9Hooks::CallOriginalSetTexture(dev, D3DVERTEXTEXTURESAMPLER0 + stage, tex);
                SamplerStates(dev, D3DVERTEXTEXTURESAMPLER0 + stage, s, srgb);
            }
        }
        for (DWORD s = 0; s < kMaxSamplers; s++) // stages the previous pass used and this one does not
            if ((boundStages & (1u << s)) && !(stages & (1u << s))) D3D9Hooks::CallOriginalSetTexture(dev, s, nullptr);
        boundStages = stages;
        // __TEXEL_SIZE__ (half-pixel offset) and the COLOR / DEPTH pixel sizes
        const float texel[4] = {-1.0f / static_cast<float>(vw), 1.0f / static_cast<float>(vh), 0.0f, 0.0f};
        D3D9Hooks::CallOriginalSetVertexShaderConstantF(dev, 255, texel, 1);
        for (const auto& tex : m.textures) {
            if (tex.semantic.empty() || tex.semantic_binding >= 224) continue;
            float size[4] = {0, 0, 0, 0};
            if (tex.semantic == "COLOR") {
                size[0] = 1.0f / static_cast<float>(f.desc.Width);
                size[1] = 1.0f / static_cast<float>(f.desc.Height);
            } else if (tex.semantic == "DEPTH" && f.depthW && f.depthH) {
                size[0] = 1.0f / static_cast<float>(f.depthW);
                size[1] = 1.0f / static_cast<float>(f.depthH);
            }
            D3D9Hooks::CallOriginalSetVertexShaderConstantF(dev, tex.semantic_binding, size, 1);
            D3D9Hooks::CallOriginalSetPixelShaderConstantF(dev, tex.semantic_binding, size, 1);
        }
        // draw: the vertex ids as floats in TEXCOORD0
        UINT prims = 0;
        D3DPRIMITIVETYPE type = D3DPT_TRIANGLELIST;
        const UINT n = d.num_vertices;
        switch (d.topology) {
        case reshadefx::primitive_topology::point_list: type = D3DPT_POINTLIST, prims = n; break;
        case reshadefx::primitive_topology::line_list: type = D3DPT_LINELIST, prims = n / 2; break;
        case reshadefx::primitive_topology::line_strip: type = D3DPT_LINESTRIP, prims = n > 1 ? n - 1 : 0; break;
        case reshadefx::primitive_topology::triangle_strip: type = D3DPT_TRIANGLESTRIP, prims = n > 2 ? n - 2 : 0; break;
        default: type = D3DPT_TRIANGLELIST, prims = n / 3; break;
        }
        if (g.vertexIds.size() < n) {
            g.vertexIds.resize(n);
            for (UINT i = 0; i < n; i++) g.vertexIds[i] = static_cast<float>(i);
        }
        if (prims) dev->DrawPrimitiveUP(type, prims, g.vertexIds.data(), sizeof(float));
        // mipmaps of the targets
        if (d.generate_mipmaps)
            for (int i = 0; i < p.targetCount; i++)
                if (p.targets[i]->autoMips) p.targets[i]->tex->GenerateMipSubLevels();
    }
}

// Special uniforms of the effects being rendered (ReShade's render_effects)
void UpdateSpecials(FxEffect& fx) {
    const auto& m = fx.fx->module;
    for (size_t i = 0; i < m.uniforms.size(); i++) {
        const auto& u = m.uniforms[i];
        float v[4] = {0, 0, 0, 0};
        switch (fx.specials[i]) {
        case Special::None: continue;
        case Special::FrameTime: v[0] = g.frameMs; break;
        case Special::FrameCount: v[0] = u.type.is_boolean() ? static_cast<float>(g.frameCount % 2 == 0) : static_cast<float>(g.frameCount); break;
        case Special::Random: {
            const int lo = AnnotationInt(u.annotations, "min", 0), hi = AnnotationInt(u.annotations, "max", RAND_MAX);
            v[0] = static_cast<float>(lo + std::rand() % (std::abs(hi - lo) + 1));
            break;
        }
        case Special::PingPong: {
            const float lo = AnnotationFloat(u.annotations, "min", 0.0f), hi = AnnotationFloat(u.annotations, "max", 1.0f);
            const float stepMin = AnnotationFloat(u.annotations, "step", 0.0f, 0), stepMax = AnnotationFloat(u.annotations, "step", 0.0f, 1);
            const float smoothing = AnnotationFloat(u.annotations, "smoothing", 0.0f);
            float inc = stepMax == 0 ? stepMin : stepMin + std::fmod(static_cast<float>(std::rand()), stepMax - stepMin + 1);
            ReadValues(fx, u, v, 2);
            if (v[1] >= 0) {
                inc = std::max(inc - std::max(0.0f, smoothing - (hi - v[0])), 0.05f) * g.frameMs * 0.001f;
                if ((v[0] += inc) >= hi) v[0] = hi, v[1] = -1;
            } else {
                inc = std::max(inc - std::max(0.0f, smoothing - (v[0] - lo)), 0.05f) * g.frameMs * 0.001f;
                if ((v[0] -= inc) <= lo) v[0] = lo, v[1] = +1;
            }
            break;
        }
        case Special::Date: {
            const std::time_t now = std::time(nullptr);
            std::tm tm{};
            localtime_s(&tm, &now);
            v[0] = static_cast<float>(tm.tm_year + 1900);
            v[1] = static_cast<float>(tm.tm_mon + 1);
            v[2] = static_cast<float>(tm.tm_mday);
            v[3] = static_cast<float>(tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec);
            break;
        }
        case Special::Timer: {
            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            v[0] = static_cast<float>(static_cast<double>(now.QuadPart - g.start.QuadPart) * 1000.0 / static_cast<double>(g.freq.QuadPart));
            break;
        }
        case Special::Key: {
            const int key = AnnotationInt(u.annotations, "keycode", 0);
            if (key > 7 && key < 256) v[0] = (GetAsyncKeyState(key) & 0x8000) ? 1.0f : 0.0f;
            break;
        }
        case Special::MousePoint: {
            POINT pt{};
            GetCursorPos(&pt);
            if (g.window) ScreenToClient(g.window, &pt); // in the game window's pixels, as ReShade
            v[0] = static_cast<float>(pt.x);
            v[1] = static_cast<float>(pt.y);
            break;
        }
        case Special::OverlayOpen: v[0] = Overlay::IsVisible() ? 1.0f : 0.0f; break;
        default: break; // mouse delta / buttons / wheel and unknown sources stay zero
        }
        WriteValues(fx, u, v, std::min<size_t>(Components(u), 4));
    }
}

void Toggles() {
    const HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    const bool focused = pid == GetCurrentProcessId();
    auto down = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
    for (auto& fx : g.effects) {
        for (auto& t : fx->techs) {
            if (t.toggleKey <= 7 || t.toggleKey >= 256) continue;
            const bool isDown = focused && down(t.toggleKey) && down(VK_CONTROL) == t.toggleCtrl && down(VK_SHIFT) == t.toggleShift && down(VK_MENU) == t.toggleAlt;
            if (isDown && !t.toggleWasDown) {
                t.enabled = !t.enabled;
                t.timeLeftMs = t.timeoutMs;
                if (t.enabled) RequestCompile(*fx);
            }
            t.toggleWasDown = isDown;
        }
    }
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------------------------------------------------
void Start() {
    if (g.started) return;
    g.started = true;
    g.scanned = false;
    g.width = g.height = 0;
    QueryPerformanceFrequency(&g.freq);
    QueryPerformanceCounter(&g.start);
    g.lastFrame = g.start;
    SetState(State::Loading);
}

void Stop() {
    if (!g.started) return;
    SavePreset();
    g.generation++; // results still on their way are dropped
    for (auto& fx : g.effects) fx->ReleaseObjects();
    g.effects.clear();
    g.order.clear();
    ReleaseDefaultPool();
    SafeRelease(g.decl);
    g.shared.clear();
    if (g.wantsDepth) DepthShare::Request(false);
    g.wantsDepth = false;
    g.started = false;
    g.presetLoaded = false;
    SetState(State::Off);
}

void BeforeReset() { ReleaseDefaultPool(); }

bool WantsDepth() { return g.wantsDepth; }

void AtPresent(IDirect3DDevice9* dev) {
    if (!g.started) return;
    if (!g.window) {
        D3DDEVICE_CREATION_PARAMETERS cp{};
        if (SUCCEEDED(dev->GetCreationParameters(&cp))) g.window = cp.hFocusWindow;
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    g.frameMs = static_cast<float>(static_cast<double>(now.QuadPart - g.lastFrame.QuadPart) * 1000.0 / static_cast<double>(g.freq.QuadPart));
    g.lastFrame = now;
    g.frameCount++;
    const unsigned long long tick = GetTickCount64();
    if (tick - g.secondStart >= 1000) {
        g.framesLastSecond = g.framesThisSecond;
        g.framesThisSecond = 0;
        g.secondStart = tick;
    }
    // the back buffer size the effects are compiled for
    IDirect3DSurface9* bb = nullptr;
    if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
        D3DSURFACE_DESC desc{};
        bb->GetDesc(&desc);
        bb->Release();
        if (desc.Width != g.width || desc.Height != g.height) {
            g.width = desc.Width;
            g.height = desc.Height;
            g.scanned = false;
        }
    }
    if (!g.scanned && g.width && g.height) StartLoading();
    // finished work from the compiler thread
    std::vector<Result> results;
    {
        std::lock_guard<std::mutex> lock(g_worker->lock);
        results.swap(g_worker->results);
    }
    for (auto& r : results) TakeResult(r);
    int loading = 0;
    for (const auto& fx : g.effects) loading += (fx->parsing || fx->compiling) ? 1 : 0;
    if (g.scanned) SetState(g.effects.empty() ? State::Empty : loading ? State::Loading : State::Ready);
    Toggles();
    // technique timeouts
    for (auto& fx : g.effects)
        for (auto& t : fx->techs)
            if (t.enabled && t.timeoutMs > 0 && (t.timeLeftMs -= static_cast<int>(g.frameMs)) <= 0) t.enabled = false;
    // Apex's scene depth while an effect that reads it is on
    bool depth = false;
    for (const auto& fx : g.effects) depth |= fx->usesDepth && fx->AnyEnabled();
    if (depth != g.wantsDepth) {
        DepthShare::Request(depth);
        g.wantsDepth = depth;
    }
}

void Render(IDirect3DDevice9* dev) {
    if (!g.started || g.order.empty()) return;
    Frame f;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &f.bb)) || !f.bb) return;
    struct Guard {
        IDirect3DSurface9* s;
        ~Guard() { s->Release(); }
    } bbGuard{f.bb};
    f.bb->GetDesc(&f.desc);
    if (f.desc.Width != g.width || f.desc.Height != g.height) return; // compiled for another size: reloading
    // the techniques to run this frame
    std::vector<std::pair<FxEffect*, Tech*>> run;
    for (const auto& key : g.order) {
        auto [fx, t] = FindTech(key);
        if (!fx || !t || !t->enabled || !fx->error.empty() || !fx->fx || !fx->fx->compiled) continue;
        if (!CreateEffectObjects(dev, *fx)) {
            LOG_WARNING("[ReShade] " + fx->file + ": " + fx->error);
            continue;
        }
        run.emplace_back(fx, t);
    }
    if (run.empty() || !EnsureShared(dev, f.desc)) return;
    if (PostScene::SceneDepthValid()) f.depth = DepthShare::Texture();
    if (f.depth) {
        D3DSURFACE_DESC dd{};
        if (SUCCEEDED(f.depth->GetLevelDesc(0, &dd))) f.depthW = dd.Width, f.depthH = dd.Height;
    }
    // save: the state block holds everything but the render targets and the depth-stencil
    IDirect3DSurface9* rts[kMaxTargets] = {};
    IDirect3DSurface9* ds = nullptr;
    for (DWORD i = 0; i < kMaxTargets; i++) dev->GetRenderTarget(i, &rts[i]);
    ExtraHooks::RawGetDepthStencilSurface(dev, &ds);
    g.stateBlock->Capture();
    FxEffect* updated = nullptr;
    for (auto& [fx, t] : run) {
        if (fx != updated) {
            UpdateSpecials(*fx);
            updated = fx;
        }
        RenderTechnique(dev, *fx, *t, f);
    }
    for (DWORD i = 0; i < kMaxTargets; i++) {
        D3D9Hooks::CallOriginalSetRenderTarget(dev, i, rts[i]);
        SafeRelease(rts[i]);
    }
    ExtraHooks::RawSetDepthStencilSurface(dev, ds);
    SafeRelease(ds);
    g.stateBlock->Apply();
    g.framesThisSecond++;
}

State GetState() { return static_cast<State>(g_state.load()); }

int EffectsLoading() {
    int n = 0;
    for (const auto& fx : g.effects) n += (fx->parsing || fx->compiling) ? 1 : 0;
    return n;
}

int FramesRenderedLastSecond() { return g.framesLastSecond; }

// ---------------------------------------------------------------------------------------------------------------------
// Menu
// ---------------------------------------------------------------------------------------------------------------------
std::vector<EffectInfo> Effects() {
    std::vector<EffectInfo> out;
    for (const auto& fx : g.effects) out.push_back({fx->file, fx->error, fx->parsing || fx->compiling});
    return out;
}

std::vector<Technique> Techniques() {
    std::vector<Technique> out;
    for (const auto& key : g.order) {
        auto [fx, t] = FindTech(key);
        if (!fx || !t || t->hidden) continue;
        out.push_back({t->key, fx->file, t->label, t->tooltip, t->enabled});
    }
    return out;
}

void SetTechnique(const std::string& key, bool on) {
    auto [fx, t] = FindTech(key);
    if (!fx || !t) return;
    t->enabled = on;
    t->timeLeftMs = t->timeoutMs;
    if (on) RequestCompile(*fx);
    g.presetDirty = true;
    SavePreset();
}

std::vector<Uniform> Uniforms(const std::string& effect) {
    std::vector<Uniform> out;
    FxEffect* fx = FindEffect(effect);
    if (!fx || !fx->fx || !fx->fx->parsed) return out;
    for (size_t i = 0; i < fx->fx->module.uniforms.size(); i++) {
        const auto& v = fx->fx->module.uniforms[i];
        const auto& a = v.annotations;
        if (fx->specials[i] != Special::None || AnnotationInt(a, "hidden", 0)) continue;
        if (v.type.is_array() || v.type.is_matrix() || v.type.rows < 1 || v.type.rows > 4) continue;
        Uniform u;
        u.effect = fx->file;
        u.name = v.name;
        u.components = static_cast<int>(v.type.rows);
        u.label = AnnotationString(a, "ui_label");
        if (u.label.empty()) u.label = v.name;
        u.tooltip = AnnotationString(a, "ui_tooltip");
        u.category = AnnotationString(a, "ui_category");
        const std::string type = AnnotationString(a, "ui_type");
        const bool hasMin = FindAnnotation(a, "ui_min") != nullptr, hasMax = FindAnnotation(a, "ui_max") != nullptr;
        u.min = AnnotationFloat(a, "ui_min", 0.0f);
        u.max = AnnotationFloat(a, "ui_max", 0.0f);
        u.step = AnnotationFloat(a, "ui_step", 0.0f);
        ReadValues(*fx, v, u.value, static_cast<size_t>(u.components));
        if (v.type.is_boolean()) {
            u.kind = UniformKind::Bool;
            u.components = 1;
        } else if (v.type.is_integral()) {
            u.kind = UniformKind::Int;
            if ((type == "combo" || type == "radio" || type == "list") && u.components == 1) {
                const std::string items = AnnotationString(a, "ui_items");
                size_t start = 0;
                while (start < items.size()) {
                    size_t end = items.find('\0', start);
                    if (end == std::string::npos) end = items.size();
                    if (end > start) u.items.push_back(items.substr(start, end - start));
                    start = end + 1;
                }
                if (!u.items.empty()) u.kind = UniformKind::Combo;
            }
            if (!hasMin) u.min = 0.0f;
            if (!hasMax) u.max = u.kind == UniformKind::Combo ? static_cast<float>(u.items.size()) - 1.0f : 100.0f;
        } else {
            u.kind = (type == "color" && u.components >= 3) ? UniformKind::Color : UniformKind::Float;
            if (!hasMin) u.min = 0.0f;
            if (!hasMax) u.max = 1.0f;
        }
        u.drag = (type == "drag" || type == "input") && !(hasMin && hasMax);
        if (u.max < u.min) std::swap(u.min, u.max);
        out.push_back(std::move(u));
    }
    return out;
}

void SetUniform(const Uniform& u) {
    FxEffect* fx = FindEffect(u.effect);
    if (!fx || !fx->fx) return;
    for (const auto& v : fx->fx->module.uniforms) {
        if (v.name != u.name) continue;
        float values[4];
        for (int i = 0; i < 4; i++) values[i] = (u.kind == UniformKind::Float || u.kind == UniformKind::Color) ? u.value[i] : std::round(u.value[i]);
        WriteValues(*fx, v, values, static_cast<size_t>(std::clamp(u.components, 1, 4)));
        g.presetDirty = true;
        return;
    }
}

void ResetUniform(const Uniform& u) {
    FxEffect* fx = FindEffect(u.effect);
    if (!fx || !fx->fx) return;
    for (const auto& v : fx->fx->module.uniforms)
        if (v.name == u.name) {
            ResetValue(*fx, v);
            g.presetDirty = true;
        }
}

void SavePreset() {
    if (!g.started || !g.presetDirty || !g.presetLoaded) return;
    std::vector<std::string> enabled, sorting;
    for (const auto& key : g.order) {
        auto [fx, t] = FindTech(key);
        if (!fx || !t) continue;
        sorting.push_back(key);
        if (t->enabled) enabled.push_back(key);
    }
    // techniques the preset lists but whose effect files are gone stay listed (ReShade keeps them too)
    for (const auto& item : SplitList(g.preset.Get("", "Techniques")))
        if (!FindTech(item).second && std::find(enabled.begin(), enabled.end(), item) == enabled.end() && item.find('@') != std::string::npos &&
            !FindEffect(item.substr(item.find('@') + 1)))
            enabled.push_back(item);
    g.preset.Set("", "Techniques", JoinList(enabled));
    if (g.preset.Has("", "TechniqueSorting") || enabled != sorting) g.preset.Set("", "TechniqueSorting", JoinList(sorting));
    for (const auto& fx : g.effects)
        if (fx->AnyEnabled() || g.preset.sections.count(fx->file)) WritePresetValues(*fx);
    std::error_code ec;
    fs::create_directories(Sub(L"Presets"), ec);
    std::string why;
    if (ApexUtil::WriteFileAtomic(PresetPath(g.presetName), WritePreset(g.preset), &why)) g.presetDirty = false;
    else LOG_WARNING("[ReShade] Could not save the preset " + g.presetName + ": " + why);
}

std::vector<std::string> PresetFiles() {
    std::vector<std::string> out;
    std::error_code ec;
    const fs::path folder = Sub(L"Presets");
    if (fs::is_directory(folder, ec))
        for (const auto& entry : fs::directory_iterator(folder, ec))
            if (entry.is_regular_file(ec) && LowerW(entry.path().extension().wstring()) == L".ini") out.push_back(Utf8(entry.path().filename()));
    if (std::none_of(out.begin(), out.end(), [](const std::string& n) { return Lower(n) == Lower(g.presetName); })) out.push_back(g.presetName);
    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) { return Lower(a) < Lower(b); });
    return out;
}

std::string CurrentPreset() {
    std::lock_guard<std::mutex> lock(g_presetNameLock);
    return g_presetName;
}

void SetPreset(const std::string& fileName) {
    if (fileName.empty() || fileName.find_first_of("\\/:") != std::string::npos) return;
    if (Lower(fileName) == Lower(g.presetName) && g.presetLoaded) return;
    SavePreset();
    const std::string oldDefinitions = g.preset.Get("", "PreprocessorDefinitions");
    std::map<std::string, std::string> oldEffectDefinitions;
    for (const auto& fx : g.effects) oldEffectDefinitions[fx->file] = g.preset.Get(fx->file, "PreprocessorDefinitions");
    g.presetName = fileName;
    {
        std::lock_guard<std::mutex> lock(g_presetNameLock);
        g_presetName = fileName;
    }
    if (!g.started) return; // loaded at Start
    LoadPreset();
    LOG_INFO("[ReShade] Preset: " + fileName);
    // different preprocessor definitions mean different code: read every effect again
    bool definitionsChanged = g.preset.Get("", "PreprocessorDefinitions") != oldDefinitions;
    for (const auto& fx : g.effects) definitionsChanged |= g.preset.Get(fx->file, "PreprocessorDefinitions") != oldEffectDefinitions[fx->file];
    if (definitionsChanged) {
        g.scanned = false;
        return;
    }
    for (auto& fx : g.effects) {
        if (!fx->fx || !fx->fx->parsed) continue;
        ApplyPresetValues(*fx);
        ApplyPresetTechniques(*fx);
        if (fx->AnyEnabled()) RequestCompile(*fx);
    }
    SortOrder();
}

bool RemoveEffect(const std::string& fileName) {
    FxEffect* fx = FindEffect(fileName);
    if (!fx) return false;
    std::error_code ec;
    if (!fs::remove(fx->path, ec)) return false;
    LOG_INFO("[ReShade] Removed " + ApexUtil::ToUtf8(fx->path.wstring()));
    Reload();
    return true;
}

void Reload() {
    SavePreset();
    g.scanned = false;
}

} // namespace ReShadeFx
