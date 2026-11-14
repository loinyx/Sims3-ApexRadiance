#pragma once
// Looks: shader and parameter descriptions shared by the generated table (third_party/reshade_shaders), the runner and
// the menu.
#include <cstdint>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

enum class LooksParamKind { Float, Int, Bool, Combo, Color, Source }; // Source: filled by the runner (items = source name)

// The menu's Kind filter of the "Add a shader" list (0 = all kinds)
enum class LooksShaderKind { Color = 1, Sharpen, Light, Film, Stylized };

struct LooksParam
{
    const char* key;   // uniform name = key in the preset .ini
    const char* label; // ui_label from the .fx
    int comps;         // 1..4
    int reg;           // pixel shader constant register (c<reg>)
    LooksParamKind kind;
    float def[4];      // default from the .fx
    float min, max;    // ui_min / ui_max
    const char* items; // combo items, '\0' separated (ui_items), or nullptr
    // The rest comes from a player's .fx only (the built-in table leaves it empty)
    const char* tooltip = nullptr;  // ui_tooltip: the row's description
    const char* category = nullptr; // ui_category: rows under a collapsible group label
    const char* text = nullptr;     // ui_text: a block of text before the row
    float step = 0.0f;              // ui_step (0 = none)
    uint8_t flags = 0;              // kParamHidden | kParamClosed | kParamField
};
inline constexpr uint8_t kParamHidden = 1; // hidden = true: set from the .ini, never shown
inline constexpr uint8_t kParamClosed = 2; // ui_category_closed: its group starts collapsed
inline constexpr uint8_t kParamField = 4;  // ui_type "input" or none: a number field, not a slider

// The control the menu gives a parameter (for Choice the menu picks segmented or a dropdown by the labels' width)
enum class LooksControl { None, Switch, Slider, Choice, Color, Vector, Field, Text };
// The ui_items of a parameter ('\0' separated; an empty item ends them)
inline std::vector<const char*> LooksItems(const LooksParam& p)
{
    std::vector<const char*> out;
    for (const char* s = p.items; s && *s && out.size() < 256; s += strlen(s) + 1) out.push_back(s);
    return out;
}
inline LooksControl LooksControlOf(const LooksParam& p)
{
    if (p.kind == LooksParamKind::Source || (p.flags & kParamHidden)) return LooksControl::None;
    if (p.kind == LooksParamKind::Bool) return LooksControl::Switch;
    if (p.kind == LooksParamKind::Combo) return !LooksItems(p).empty() ? LooksControl::Choice : p.text ? LooksControl::Text : LooksControl::Field;
    if (p.kind == LooksParamKind::Color && p.comps >= 3) return LooksControl::Color;
    if (p.comps > 1) return LooksControl::Vector;
    return (p.flags & kParamField) || !(p.max > p.min) ? LooksControl::Field : LooksControl::Slider;
}

// Preprocessor definitions (#define) of a look, in order: name, value
using LooksDefines = std::vector<std::pair<std::string, std::string>>;

struct LooksShader
{
    const char* technique;      // technique name in the .ini ("Techniques=Name@File.fx")
    const char* file;           // .fx file name = section name in the .ini
    const char* const* src;     // HLSL body pieces (nullptr-terminated); the runner prepends shaders/looks_common.hlsl
    const char* const* entries; // ps_3_0 entry points (nullptr-terminated), in pass order
    const LooksParam* params;
    int paramCount;
    bool needsDepth;
    bool srgb;                  // one-pass shader reads and writes sRGB (SRGBTexture/SRGBWriteEnable = true)
    const char* description;    // one short menu line (English, translated by the menu)
    LooksShaderKind kind;
};
