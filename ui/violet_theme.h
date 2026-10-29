#pragma once
// The Violet theme of the Apex menu: colours, the global ImGui style of Apex's own ImGui context, and the fonts.
// Apex owns its ImGui context (separate from official S3SS's), so the style is set once, globally, when the overlay
// creates the context (framework/overlay.cpp). Sizes are in 1080p pixels; the overlay scales the style and the fonts
// with the resolution (1080p = 1, 4K = 2) and the user's text size.
//
// Fonts: Segoe UI (segoeui.ttf) and Segoe UI Bold (segoeuib.ttf) for titles; missing files fall back to ImGui's default
// font. Segoe UI covers Latin, Greek and Cyrillic; the Japanese, Korean, Chinese and Thai menu languages merge a Windows
// font into both (LoadFonts). Icons are not a font: they are Lucide vector strokes drawn by ui/icons.h.
#include "imgui.h"

namespace VioletTheme {

// Control heights at the reference scale; colour and size are independent.
inline constexpr float kControlCompact = 30.0f;
inline constexpr float kControlPrimary = 36.0f;
inline constexpr float kControlIcon = 16.0f;

inline constexpr float kControlPadding = 10.0f;
inline constexpr float kPrimaryPadding = 14.0f;
inline constexpr float kControlIconGap = 6.0f;
inline constexpr float kCheckboxSize = 20.0f;

// Palette (0xRRGGBB)
inline constexpr unsigned kAccent = 0x7F77DD;      // violet: toggles on, icons, slider fill, "Advanced"
inline constexpr unsigned kAccentDark = 0x534AB7;  // highlighted pill, hovered buttons
inline constexpr unsigned kAccentLight = 0xCECBF6; // text on the dark violet
inline constexpr unsigned kWindowBg = 0x0B0C0E;
inline constexpr unsigned kCardBg = 0x131417;
inline constexpr unsigned kCardBorder = 0x1B1C20;
inline constexpr unsigned kSelectedBg = 0x1C1D22;  // selected sidebar item, neutral pills
inline constexpr unsigned kHoverBg = 0x111215;
inline constexpr unsigned kSidebarBg = 0x08090A; // the sidebar, a step below the window
inline constexpr unsigned kToggleOff = 0x303137;
inline constexpr unsigned kText = 0xE8E8EC;
inline constexpr unsigned kTextMuted = 0x8B8C96;
inline constexpr unsigned kError = 0xE8716B;   // soft red: error notes
inline constexpr unsigned kWarning = 0xE0A84F; // amber: warning notes, "Reload save" badges
inline constexpr unsigned kSuccess = 0x7DBE9A; // muted green: "All changes saved" in the status bar

// 0xRRGGBB (+ alpha) as an ImVec4 (for PushStyleColor / GetColorU32, which apply the disabled alpha)
ImVec4 Col(unsigned rgb, float alpha = 1.0f);

// Once, right after ImGui::CreateContext (overlay): the Violet style and colours at 1080p sizes.
void ApplyStyle(ImGuiStyle& style);
// Once, right after ImGui::CreateContext (overlay), before the first frame: adds the fonts to io.Fonts.
void LoadFonts(ImGuiIO& io);
// Before each NewFrame (overlay, outside a frame): rebuilds the fonts when the scripts in use changed (another menu
// language, or the language list was opened or closed a while ago). Cheap when nothing changed.
void UpdateFonts(ImGuiIO& io);
// During a frame: the language list is open, so every language's native name must render (merges the CJK fonts for a
// short while; violet_theme.cpp explains the memory trade-off)
void RequestAllScripts();

ImFont* RegularFont(); // nullptr = ImGui's default font
ImFont* BoldFont();    // nullptr = same as the regular font
// Size the fonts were added at (1080p pixels, before FontScaleMain); use it for PushFont(font, BaseFontSize() * k).
float BaseFontSize();

} // namespace VioletTheme
