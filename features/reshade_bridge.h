#pragma once
// ReShade bridge (Apex Radiance): connects to an installed ReShade (6.3 or newer, the normal or the add-on build) as an
// add-on, through the functions its DLL exports. Apex never ships or loads ReShade itself.
//
// While connected, Apex renders ReShade's effects at its own post-scene boundary (after Color, before the game draws its
// interface) instead of ReShade's default point at Present, so menus, plumbobs and text keep their colours. Optionally it
// binds Apex's shared scene depth (the INTZ texture) as ReShade's DEPTH texture.
//
// Only the D3D9 runtime is driven. ReShade running on Vulkan (DXVK) or another API is reported and left alone.
// No ReShade types leak out of this header: reshade_bridge.cpp is the only file that includes the ReShade headers (their
// ImGui overlay header must not meet the menu's ImGui configuration).
//
// Threads: everything here runs on the render thread (the menu is drawn there too), except State(), which is atomic.
#include <d3d9.h>
#include <cstdint>
#include <string>
#include <vector>

namespace ReShadeBridge {

enum class State : int {
    Off,         // the feature is off: Apex does not touch ReShade
    NotFound,    // no ReShade DLL in the game
    TooOld,      // ReShade refused the registration (older than 6.3)
    Registering, // found, waiting for its effect runtime (the first frames)
    OtherApi,    // ReShade runs through Vulkan (DXVK) or another API: its default rendering is left as it is
    Connected,   // effects render before the game interface
};

// Feature switch (ReShadeEffects patch): start looking for ReShade, or let go of it (unregisters; ReShade renders at its
// default point again)
void Start();
void Stop();
// Present (render thread, before ReShade's own Present sees the frame): keeps looking for ReShade while not connected
// (throttled) and, when this frame's effects did not run before the interface, tells ReShade to skip them this frame
// rather than draw them over the menus.
void AtPresent(IDirect3DDevice9* dev);
// The PostScene effect: ReShade's effects on the finished scene (after Color), before the game's interface
void RenderBeforeInterface(IDirect3DDevice9* dev);
// The shared scene depth as ReShade's DEPTH texture (re-applied before each render, after ReShade's own depth add-on)
void UseApexDepth(bool on);
// Around a device Reset (Apex releases the shared depth): drop the binding until the next frame
void BeforeReset();

State GetState();
// The ReShade DLL's file version ("6.4.1"); empty when not found
std::string Version();
// Effects rendered before the interface in the last second (status line)
int FramesRenderedLastSecond();
// The back buffer is multisampled (the game's own Edge Smoothing): ReShade keeps its default point, over the interface
bool Multisampled();

// ---- the menu (render thread) ----
struct Technique {
    uint64_t handle = 0;
    std::string effect; // file name, "Clarity.fx"
    std::string name;   // technique name, or its ui_label
    std::string tooltip;
    bool enabled = false;
};
// Loaded techniques in ReShade's order (empty while effects are still compiling)
std::vector<Technique> Techniques();
void SetTechnique(uint64_t handle, bool on); // and saves the ReShade preset

enum class UniformKind { Bool, Int, Float, Combo, Color };
struct Uniform {
    uint64_t handle = 0;
    std::string label, tooltip, category;
    UniformKind kind = UniformKind::Float;
    int components = 1;      // 1..4 (float and int); 3 or 4 for Color
    float min = 0.0f, max = 1.0f, step = 0.0f;
    bool drag = false;       // ui_type "drag" or "input": no fixed range
    std::vector<std::string> items; // Combo: the choices (ui_items)
    float value[4] = {0, 0, 0, 0};  // current value (Bool/Int/Combo as numbers)
};
// The adjustable uniforms of an effect file (hidden ones and special "source" values left out)
std::vector<Uniform> Uniforms(const std::string& effect);
void SetUniform(const Uniform& u); // writes u.value; the preset is saved when the menu closes or a technique changes
void ResetUniform(const Uniform& u);
void SavePreset();

// Presets: the .ini files ReShade can use (its own folder and Apex Radiance\ReShade\Presets); the current one
std::vector<std::wstring> PresetFiles();
std::wstring CurrentPreset();
void SetPreset(const std::wstring& path);

// Apex Radiance\ReShade\ (Shaders, Textures, Presets below it), created on demand and added to ReShade's search paths
std::wstring EffectsFolder();
bool EnsureSearchPaths();
// Copies the chosen files (.fx, .fxh, textures, .ini presets) or extracts a .zip shader pack into the folder above. File
// work only (any thread): the render thread then calls EnsureSearchPaths() and Reload(). Returns how many files were
// added; error (English, for the log) names the file that failed.
int AddFiles(const std::vector<std::wstring>& paths, std::string& error);
// Effect files Apex added (Shaders folder), for Update and Remove
std::vector<std::wstring> AddedEffects();
bool RemoveEffect(const std::wstring& fileName); // the .fx in Apex's Shaders folder; reloads
void Reload();

} // namespace ReShadeBridge
