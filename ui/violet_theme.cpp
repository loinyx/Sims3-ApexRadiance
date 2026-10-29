// The Violet theme (see violet_theme.h).
#include "violet_theme.h"
#include "apex_log.h"
#include "apex_util.h"
#include "i18n.h"
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <format>
#include <initializer_list>
#include <string>
#include <vector>

namespace VioletTheme {
namespace {

constexpr float kFontSize = 15.0f; // Segoe UI at 1080p (the overlay's FontScaleMain doubles it at 4K)

ImFont* g_regular = nullptr;
ImFont* g_bold = nullptr;
float g_baseSize = 13.0f; // ImGui's default font

std::wstring FontsFolder() {
    wchar_t dir[MAX_PATH] = {};
    const UINT n = GetWindowsDirectoryW(dir, MAX_PATH);
    if (!n || n >= MAX_PATH) return L"C:\\Windows\\Fonts\\";
    return std::wstring(dir) + L"\\Fonts\\";
}

// First of the files that exists, as UTF-8 (ImGui opens it with _wfopen after converting back); empty when none does.
std::string FindFont(const std::wstring& folder, std::initializer_list<const wchar_t*> names) {
    for (const wchar_t* name : names) {
        const std::wstring path = folder + name;
        if (ApexUtil::FileExists(path)) return ApexUtil::ToUtf8(path);
    }
    return {};
}

// ---- Fallback fonts for the scripts Segoe UI lacks ----
// Segoe UI covers Latin, Greek and Cyrillic, so 16 of the 21 menu languages need nothing else. Japanese, Korean, the two
// Chinese and Thai get a Windows font merged into the regular and the bold font. ImGui 1.92 rasterises glyphs on demand
// (the DX9 backend has RendererHasTextures), so no glyph ranges are needed and the atlas only grows by the characters
// actually drawn; but the whole font file stays in memory for the atlas' lifetime, and a CJK file is 10-21 MB in a 32-bit
// game that runs close to its address-space limit. So only the scripts in use are merged (the current language and
// Windows' language, shown in "Automatic (...)"), plus all of them while the language list is open, and the fonts are
// rebuilt when that set changes (UpdateFonts, between frames).
using I18n::Script;

// Candidates per script, best first (Windows 10/11 names; the later ones are older Windows' or optional fonts)
const std::initializer_list<const wchar_t*>& Candidates(Script script) {
    static const std::initializer_list<const wchar_t*> japanese = {L"YuGothM.ttc", L"YuGothR.ttc", L"meiryo.ttc", L"msgothic.ttc"};
    static const std::initializer_list<const wchar_t*> korean = {L"malgun.ttf", L"gulim.ttc"};
    static const std::initializer_list<const wchar_t*> simplified = {L"msyh.ttc", L"simsun.ttc"};
    static const std::initializer_list<const wchar_t*> traditional = {L"msjh.ttc", L"mingliu.ttc"};
    static const std::initializer_list<const wchar_t*> thai = {L"LeelawUI.ttf", L"tahoma.ttf"};
    static const std::initializer_list<const wchar_t*> none = {};
    switch (script) {
    case Script::Japanese: return japanese;
    case Script::Korean: return korean;
    case Script::ChineseSimplified: return simplified;
    case Script::ChineseTraditional: return traditional;
    case Script::Thai: return thai;
    default: return none;
    }
}

const char* ScriptName(Script script) {
    switch (script) {
    case Script::Japanese: return "Japanese";
    case Script::Korean: return "Korean";
    case Script::ChineseSimplified: return "Chinese Simplified";
    case Script::ChineseTraditional: return "Chinese Traditional";
    case Script::Thai: return "Thai";
    default: return "Latin";
    }
}

std::vector<Script> g_loadedScripts; // merged into g_regular / g_bold now, in merge order
bool g_fontsLoaded = false;          // LoadFonts ran (UpdateFonts only rebuilds what LoadFonts set up)
int g_allScriptsFrame = -1000;       // last frame the language list asked for every script

// Frames the extra scripts stay after the language list closes, so a list that is opened again (or a frame where the
// list is not drawn) does not reload 30 MB of fonts each time
constexpr int kAllScriptsLinger = 120;

// The scripts to merge, in merge order. Han characters exist in four fonts with different regional shapes; ImGui takes a
// glyph from the first merged font that has it, so the current language's script goes first (Japanese text gets the
// Japanese forms, Traditional Chinese the Traditional ones).
std::vector<Script> WantedScripts() {
    std::vector<Script> s;
    auto add = [&s](Script script) {
        if (script != Script::Latin && std::find(s.begin(), s.end(), script) == s.end()) s.push_back(script);
    };
    static const I18n::Lang system = I18n::SystemLanguage();
    add(I18n::ScriptOf(I18n::Current()));
    add(I18n::ScriptOf(system));
    add(Script::Thai); // under 1 MB: always there, so "ไทย" reads in the language list without a reload
    if (ImGui::GetCurrentContext() && ImGui::GetFrameCount() - g_allScriptsFrame <= kAllScriptsLinger) {
        // Only the native names are shown in the other scripts: Microsoft YaHei has every Han character of them
        // (日本語, 简体中文, 繁體中文) and Malgun Gothic the Hangul (한국어), so two files cover the whole list
        add(Script::ChineseSimplified);
        add(Script::Korean);
    }
    return s;
}

// The whole file in an ImGui allocation (the atlas frees it when it owns it); nullptr when it cannot be read
void* ReadFontFile(const std::wstring& path, int* size) {
    *size = 0;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") || !f) return nullptr;
    void* data = nullptr;
    if (fseek(f, 0, SEEK_END) == 0) {
        const long n = ftell(f);
        if (n > 0 && n < 64L * 1024 * 1024 && fseek(f, 0, SEEK_SET) == 0) {
            data = IM_ALLOC(static_cast<size_t>(n));
            if (data && fread(data, 1, static_cast<size_t>(n), f) == static_cast<size_t>(n)) *size = static_cast<int>(n);
            else if (data) {
                IM_FREE(data);
                data = nullptr;
            }
        }
    }
    fclose(f);
    return data;
}

unsigned Be16(const unsigned char* p) { return (static_cast<unsigned>(p[0]) << 8) | p[1]; }
unsigned Be32(const unsigned char* p) { return (Be16(p) << 16) | Be16(p + 2); }

// unitsPerEm / (hhea ascender - descender) of the first face: the em size of a font added at a 1 px "height". ImGui
// (stb_truetype) sizes every font so that ascender - descender = the font size, and those metrics differ a lot between
// families (MS Gothic's span is 1 em, Meiryo's 1.5), so merged fonts are rescaled to Segoe UI's em with this ratio.
// 0 when the tables cannot be read.
float EmRatio(const void* data, int size) {
    const auto* b = static_cast<const unsigned char*>(data);
    if (!b || size < 12) return 0.0f;
    unsigned offset = 0;
    if (Be32(b) == 0x74746366u) { // 'ttcf': a collection, first face
        if (size < 16) return 0.0f;
        offset = Be32(b + 12);
    }
    if (offset + 12 > static_cast<unsigned>(size)) return 0.0f;
    const unsigned tables = Be16(b + offset + 4);
    unsigned head = 0, hhea = 0;
    for (unsigned i = 0; i < tables; i++) {
        const unsigned rec = offset + 12 + i * 16;
        if (rec + 16 > static_cast<unsigned>(size)) return 0.0f;
        const unsigned tag = Be32(b + rec);
        if (tag == 0x68656164u) head = Be32(b + rec + 8);      // 'head'
        else if (tag == 0x68686561u) hhea = Be32(b + rec + 8); // 'hhea'
    }
    if (!head || !hhea || head + 20 > static_cast<unsigned>(size) || hhea + 8 > static_cast<unsigned>(size)) return 0.0f;
    const float upem = static_cast<float>(Be16(b + head + 18));
    const float ascent = static_cast<float>(static_cast<short>(Be16(b + hhea + 4)));
    const float descent = static_cast<float>(static_cast<short>(Be16(b + hhea + 6)));
    return ascent - descent > 0.0f && upem > 0.0f ? upem / (ascent - descent) : 0.0f;
}

} // namespace

ImVec4 Col(unsigned rgb, float alpha) {
    return ImVec4(static_cast<float>((rgb >> 16) & 0xFF) / 255.0f, static_cast<float>((rgb >> 8) & 0xFF) / 255.0f, static_cast<float>(rgb & 0xFF) / 255.0f, alpha);
}

void ApplyStyle(ImGuiStyle& style) {
    ImGui::StyleColorsDark(&style); // anything not set below keeps a sane dark value

    style.WindowPadding = ImVec2(10.0f, 10.0f);
    style.FramePadding = ImVec2(kControlPadding, (kControlCompact - kFontSize) * 0.5f);
    style.ItemSpacing = ImVec2(12.0f, 5.0f);
    style.ItemInnerSpacing = ImVec2(6.0f, 4.0f);
    style.IndentSpacing = 14.0f;
    style.ScrollbarSize = 8.0f; // thin
    style.GrabMinSize = 12.0f;  // the slider grab (ui/widgets.cpp draws a circle of this diameter)
    style.DisabledAlpha = 0.5f; // disabled rows dim as a whole
    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.FrameBorderSize = 0.0f;
    style.WindowRounding = 18.0f;
    style.ChildRounding = 12.0f;
    style.FrameRounding = 10.0f;
    style.PopupRounding = 12.0f;
    style.ScrollbarRounding = 8.0f;
    style.GrabRounding = 10.0f;
    style.TabRounding = 10.0f;
    style.SeparatorTextBorderSize = 1.0f;
    style.WindowMenuButtonPosition = ImGuiDir_None;

    ImVec4* c = style.Colors;
    c[ImGuiCol_Text] = Col(kText);
    c[ImGuiCol_TextDisabled] = Col(kTextMuted);
    c[ImGuiCol_WindowBg] = Col(kWindowBg);
    c[ImGuiCol_ChildBg] = Col(0, 0.0f); // the sidebar and content areas show the window; cards push their own colour
    c[ImGuiCol_PopupBg] = Col(kHoverBg, 0.98f);
    c[ImGuiCol_Border] = Col(kCardBorder);
    c[ImGuiCol_BorderShadow] = Col(0, 0.0f);
    c[ImGuiCol_FrameBg] = Col(0x1A1B20);
    c[ImGuiCol_FrameBgHovered] = Col(0x222329);
    c[ImGuiCol_FrameBgActive] = Col(0x2A2B31);
    c[ImGuiCol_TitleBg] = Col(kWindowBg);
    c[ImGuiCol_TitleBgActive] = Col(kCardBg);
    c[ImGuiCol_TitleBgCollapsed] = Col(kWindowBg);
    c[ImGuiCol_MenuBarBg] = Col(kCardBg);
    c[ImGuiCol_ScrollbarBg] = Col(0, 0.0f);
    c[ImGuiCol_ScrollbarGrab] = Col(kToggleOff);
    c[ImGuiCol_ScrollbarGrabHovered] = Col(0x44454D);
    c[ImGuiCol_ScrollbarGrabActive] = Col(kAccentDark);
    c[ImGuiCol_CheckMark] = Col(kAccent);
    c[ImGuiCol_CheckboxSelectedBg] = Col(0x1A1B20);
    c[ImGuiCol_SliderGrab] = Col(kAccent);
    c[ImGuiCol_SliderGrabActive] = Col(kAccentLight);
    c[ImGuiCol_Button] = Col(kCardBorder);
    c[ImGuiCol_ButtonHovered] = Col(kAccentDark);
    c[ImGuiCol_ButtonActive] = Col(kAccent);
    c[ImGuiCol_Header] = Col(kSelectedBg);
    c[ImGuiCol_HeaderHovered] = Col(kAccentDark, 0.45f);
    c[ImGuiCol_HeaderActive] = Col(kAccentDark, 0.70f);
    c[ImGuiCol_Separator] = Col(kCardBorder);
    c[ImGuiCol_SeparatorHovered] = Col(kAccentDark);
    c[ImGuiCol_SeparatorActive] = Col(kAccent);
    c[ImGuiCol_ResizeGrip] = Col(kAccent, 0.20f);
    c[ImGuiCol_ResizeGripHovered] = Col(kAccent, 0.60f);
    c[ImGuiCol_ResizeGripActive] = Col(kAccent, 0.90f);
    c[ImGuiCol_InputTextCursor] = Col(kAccentLight);
    c[ImGuiCol_TabHovered] = Col(kAccentDark, 0.80f);
    c[ImGuiCol_Tab] = Col(kSelectedBg);
    c[ImGuiCol_TabSelected] = Col(kAccentDark);
    c[ImGuiCol_TabSelectedOverline] = Col(kAccent);
    c[ImGuiCol_TabDimmed] = Col(kSelectedBg);
    c[ImGuiCol_TabDimmedSelected] = Col(kAccentDark, 0.70f);
    c[ImGuiCol_TabDimmedSelectedOverline] = Col(kAccent, 0.50f);
    c[ImGuiCol_PlotLines] = Col(kAccent);
    c[ImGuiCol_PlotLinesHovered] = Col(kAccentLight);
    c[ImGuiCol_PlotHistogram] = Col(kAccent);
    c[ImGuiCol_PlotHistogramHovered] = Col(kAccentLight);
    c[ImGuiCol_TableHeaderBg] = Col(kSelectedBg);
    c[ImGuiCol_TableBorderStrong] = Col(kCardBorder);
    c[ImGuiCol_TableBorderLight] = Col(kSelectedBg);
    c[ImGuiCol_TableRowBg] = Col(0, 0.0f);
    c[ImGuiCol_TableRowBgAlt] = Col(0xFFFFFF, 0.03f);
    c[ImGuiCol_TextLink] = Col(kAccent);
    c[ImGuiCol_TextSelectedBg] = Col(kAccent, 0.35f);
    c[ImGuiCol_TreeLines] = Col(kCardBorder);
    c[ImGuiCol_DragDropTarget] = Col(kAccent);
    c[ImGuiCol_NavCursor] = Col(kAccentLight); // the keyboard focus ring (2 px, rounded like the frame) on every nav-focusable widget
    c[ImGuiCol_NavWindowingHighlight] = Col(kAccentLight, 0.70f);
    c[ImGuiCol_NavWindowingDimBg] = Col(0, 0.35f);
    c[ImGuiCol_ModalWindowDimBg] = Col(0, 0.45f);
}

void LoadFonts(ImGuiIO& io) {
    ImFontAtlas* atlas = io.Fonts;
    const std::wstring folder = FontsFolder();
    const std::string regular = FindFont(folder, {L"segoeui.ttf"});
    const std::string bold = FindFont(folder, {L"segoeuib.ttf"});
    g_fontsLoaded = true;
    g_loadedScripts = WantedScripts();
    if (regular.empty()) {
        LOG_WARNING("[Overlay] Segoe UI not found: the menu uses ImGui's default font");
        return; // ImGui adds its default font at the first frame
    }
    int regularSize = 0;
    void* regularData = ReadFontFile(ApexUtil::ToWide(regular), &regularSize);
    const float segoeEm = EmRatio(regularData, regularSize);
    ImFontConfig base;
    std::snprintf(base.Name, sizeof(base.Name), "Segoe UI");
    g_regular = regularData ? atlas->AddFontFromMemoryTTF(regularData, regularSize, kFontSize, &base) : nullptr;
    if (!g_regular) {
        if (regularData) IM_FREE(regularData); // AddFontFromMemoryTTF failed: the atlas did not take it
        LOG_WARNING("[Overlay] Segoe UI could not be loaded: the menu uses ImGui's default font");
        return;
    }
    g_baseSize = kFontSize;
    // Each merged font is added to the regular font (which owns the file data) and to the bold one (which shares it:
    // CJK bold files would double 10-20 MB for titles only, so titles in those scripts use the regular weight)
    struct Merged {
        void* data;
        int size;
        float scale;
    };
    std::vector<Merged> merged;
    std::string found;
    for (Script script : g_loadedScripts) {
        bool loaded = false;
        for (const wchar_t* name : Candidates(script)) {
            const std::wstring path = folder + name;
            if (!ApexUtil::FileExists(path)) continue;
            int size = 0;
            void* data = ReadFontFile(path, &size);
            if (!data) continue;
            const float em = EmRatio(data, size);
            const float scale = segoeEm > 0.0f && em > 0.0f ? std::clamp(segoeEm / em, 0.75f, 1.35f) : 1.0f;
            ImFontConfig cfg;
            cfg.MergeMode = true;
            cfg.ExtraSizeScale = scale;
            std::snprintf(cfg.Name, sizeof(cfg.Name), "%s", ApexUtil::ToUtf8(name).c_str());
            if (!atlas->AddFontFromMemoryTTF(data, size, 0.0f, &cfg)) {
                IM_FREE(data);
                continue;
            }
            merged.push_back({data, size, scale});
            found += std::format("{}{} {} (x{:.2f})", found.empty() ? "" : ", ", ScriptName(script), ApexUtil::ToUtf8(name), scale);
            loaded = true;
            break;
        }
        if (!loaded) {
            std::string names;
            for (const wchar_t* name : Candidates(script)) names += (names.empty() ? "" : ", ") + ApexUtil::ToUtf8(name);
            LOG_WARNING(std::format("[Overlay] No {} font in the Windows fonts folder ({}): that text shows as boxes", ScriptName(script), names));
        }
    }
    if (!bold.empty()) {
        ImFontConfig boldCfg;
        std::snprintf(boldCfg.Name, sizeof(boldCfg.Name), "Segoe UI Bold");
        g_bold = atlas->AddFontFromFileTTF(bold.c_str(), kFontSize, &boldCfg);
    }
    if (g_bold) {
        for (const Merged& m : merged) {
            ImFontConfig cfg;
            cfg.MergeMode = true;
            cfg.FontDataOwnedByAtlas = false; // the regular font's source frees it
            cfg.ExtraSizeScale = m.scale;
            atlas->AddFontFromMemoryTTF(m.data, m.size, 0.0f, &cfg);
        }
    } else {
        g_bold = g_regular;
    }
    io.FontDefault = g_regular;
    LOG_INFO(std::string("[Overlay] Fonts: Segoe UI") + (g_bold != g_regular ? " + Bold" : "") + (found.empty() ? "" : "; merged: " + found) +
             " (icons: Lucide vector strokes)");
}

void UpdateFonts(ImGuiIO& io) {
    // Only between frames, and only when the backend uploads atlas changes itself (a locked atlas cannot change)
    if (!g_fontsLoaded || io.Fonts->Locked || (io.BackendFlags & ImGuiBackendFlags_RendererHasTextures) == 0) return;
    if (WantedScripts() == g_loadedScripts) return;
    // ClearFonts drops every font and source (the file data with them); the atlas texture is kept and repacked, and
    // ImGui re-binds io.FontDefault at the next NewFrame. Nothing keeps an ImFont* across frames (RegularFont() and
    // BoldFont() are read at each use).
    io.Fonts->ClearFonts();
    g_regular = g_bold = nullptr;
    g_baseSize = 13.0f;
    io.FontDefault = nullptr;
    LoadFonts(io);
}

void RequestAllScripts() {
    if (ImGui::GetCurrentContext()) g_allScriptsFrame = ImGui::GetFrameCount();
}

ImFont* RegularFont() { return g_regular; }
ImFont* BoldFont() { return g_bold ? g_bold : g_regular; }
float BaseFontSize() { return g_baseSize; }

} // namespace VioletTheme
