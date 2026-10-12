#pragma once
// ReShade FX effects inside Apex Radiance (Color > ReShade): runs ReShade's .fx effects itself, no ReShade install.
//
// The effects come from Apex Radiance\ReShade\Shaders (Textures, Presets beside it). A worker thread preprocesses and
// parses every .fx with the ReShade FX compiler (features/reshade_fx_compile.cpp) and compiles the shaders of the effects
// that are on (bytecode cached in ReShade\Cache). The render thread creates the Direct3D 9 objects and runs the enabled
// techniques at Apex's post-scene boundary (PostScene::kReShade: after Color, before the game's interface), with Apex's
// scene depth as DEPTH and a copy of the picture as COLOR. Which effects are on and their settings live in a ReShade
// preset .ini (ReShade's own format, so presets move both ways).
//
// Threads: everything here runs on the render thread (the menu is drawn there too), except AddFiles and CurrentPreset
// (any thread).
#include <d3d9.h>
#include <cstdint>
#include <string>
#include <vector>

namespace ReShadeFx {

enum class State : int {
    Off,     // the feature is off
    Empty,   // no effect files yet
    Loading, // reading or compiling effects
    Ready,   // every effect file was read (some may have failed)
};

// Feature switch (ReShade patch)
void Start();
void Stop();
// Present (render thread): starts and finishes loading, the effects' timers and toggle keys
void AtPresent(IDirect3DDevice9* dev);
// The PostScene effect: the enabled techniques on the finished scene (after Color), before the game's interface
void Render(IDirect3DDevice9* dev);
// Before a device Reset: releases the Direct3D objects that do not survive it (recreated at the next Render)
void BeforeReset();
// An enabled effect reads DEPTH (the patch keeps Apex's scene depth on for it)
bool WantsDepth();

State GetState();
int EffectsLoading(); // effect files still being read or compiled
int FramesRenderedLastSecond();


// ---- the menu (render thread) ----
struct EffectInfo {
    std::string file;  // "Clarity.fx"
    std::string error; // the first compiler error; empty when it loaded
    bool loading = false;
};
std::vector<EffectInfo> Effects(); // file name order

struct Technique {
    std::string key;    // "Name@File.fx" (the preset's name for it)
    std::string effect; // "Clarity.fx"
    std::string name;   // its ui_label, or its name
    std::string tooltip;
    bool enabled = false;
};
std::vector<Technique> Techniques(); // render order; hidden ones left out
void SetTechnique(const std::string& key, bool on); // and saves the preset

enum class UniformKind { Bool, Int, Float, Combo, Color };
struct Uniform {
    std::string effect, name; // the preset's names
    std::string label, tooltip, category;
    UniformKind kind = UniformKind::Float;
    int components = 1;      // 1..4 (float and int); 3 or 4 for Color
    float min = 0.0f, max = 1.0f, step = 0.0f;
    bool drag = false;       // ui_type "drag" or "input": no fixed range
    std::vector<std::string> items; // Combo: the choices (ui_items)
    float value[4] = {0, 0, 0, 0};
};
// The adjustable uniforms of an effect file (hidden ones, special "source" values, arrays and matrices left out)
std::vector<Uniform> Uniforms(const std::string& effect);
void SetUniform(const Uniform& u); // the preset is saved by SavePreset (when an edit is committed)
void ResetUniform(const Uniform& u);
void SavePreset();

// Presets: the .ini files in ReShade\Presets (file names); the current one (created when first saved)
std::vector<std::string> PresetFiles();
std::string CurrentPreset();
void SetPreset(const std::string& fileName); // loads it (saves the current one first)

// Apex Radiance\ReShade\ (Shaders, Textures, Presets below it), created on demand
std::wstring EffectsFolder();
// Copies the chosen files (.fx, .fxh, textures, .ini presets) or extracts a .zip shader pack into the folder above. File
// work only (any thread); the render thread then calls Reload(). Returns how many files were added; error (English, for
// the log) names the file that failed.
int AddFiles(const std::vector<std::wstring>& paths, std::string& error);
bool RemoveEffect(const std::string& fileName); // deletes the .fx from ReShade\Shaders; reloads
void Reload(); // reads every effect file again

} // namespace ReShadeFx
