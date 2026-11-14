#pragma once
// Looks: shader and parameter descriptions shared by the generated table (third_party/reshade_shaders), the runner and
// the menu.

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
};

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
