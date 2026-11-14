#pragma once
// GShade looks: shader/parameter descriptions shared by the generated table, the runner and the menu.

enum class GsKind { Float, Int, Bool, Combo, Color, Source }; // Source: filled by the runner (items = source name)

struct GsParamDesc
{
    const char* key;   // uniform name = key in the preset .ini
    const char* label; // ui_label from the .fx
    int comps;         // 1..4
    int reg;           // pixel shader constant register (c<reg>)
    GsKind kind;
    float def[4];      // default from the .fx
    float min, max;    // ui_min / ui_max
    const char* items; // combo items, '\0' separated (ui_items), or nullptr
};

struct GsShaderDesc
{
    const char* technique;     // technique name in the .ini ("Techniques=Name@File.fx")
    const char* file;          // .fx file name = section name in the .ini
    const char* const* src;    // HLSL body pieces (nullptr-terminated); the runner prepends the common prelude
    const char* const* entries;// ps_3_0 entry points (nullptr-terminated), in pass order
    const GsParamDesc* params;
    int paramCount;
    bool needsDepth;
    bool srgb;                 // one-pass shader reads and writes sRGB (SRGBTexture/SRGBWriteEnable = true)
};
