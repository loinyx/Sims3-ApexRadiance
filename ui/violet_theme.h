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

// Palette (0xRRGGBB). The accent is Apex Launcher's colour theme: the launcher writes it to [ui] theme_accent,
// theme_accent_dark, theme_accent_light and theme_segment_on (apex_config.cpp reads them before the menu is made), and
// these are Amethyst's when it has not. The surfaces and texts are the launcher's neutral greys in every theme.
inline unsigned kAccent = 0x7F77DD;      // toggles on, icons, slider fill, "Advanced" (white text reads on it)
inline unsigned kAccentDark = 0x534AB7;  // highlighted pill, hovered buttons
inline unsigned kAccentLight = 0xCECBF6; // text on the dark accent
inline unsigned kSegmentOn = 0x2A2650;   // the chosen option of a segmented control (white text)
inline unsigned kGradientFrom = 0x534AB7; // the main button: the theme's gradient, left to right, as the launcher's
inline unsigned kGradientTo = 0x534AB7;
inline unsigned kOnGradient = 0xFFFFFF;   // its words and icon: the ink that reads on all of the gradient
inline constexpr unsigned kWindowBg = 0x050505;
inline constexpr unsigned kCardBg = 0x111111;
inline constexpr unsigned kCardBorder = 0x1F1F1F;
inline constexpr unsigned kSelectedBg = 0x1D1D1D;  // selected sidebar item, neutral pills
inline constexpr unsigned kHoverBg = 0x0F0F0F;
inline constexpr unsigned kPopupBg = 0x161616; // dialogs, dropdowns, menus and tooltips: one step above a card, opaque
inline constexpr unsigned kSidebarBg = 0x040404; // the sidebar, a step below the window
inline constexpr unsigned kToggleOff = 0x2A2A2A;
inline constexpr unsigned kText = 0xFFFFFF;
inline constexpr unsigned kTextMuted = 0x959595;
inline constexpr unsigned kTextFaint = 0x5F5F5F; // handles and hints that should not compete with the text (drag grips)
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
