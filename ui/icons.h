#pragma once
// Menu icons: Lucide (ISC License, third_party/lucide/LICENSE), drawn as vector strokes with ImDrawList.
//  - ui/lucide_data.h holds each icon's SVG elements (path d strings, circles, rects, lines) copied from
//    third_party/lucide/icons/*.svg; no build step and no texture.
//  - On first use an icon's paths are parsed (M L H V C S Q T A Z, absolute and relative) and flattened to polylines in
//    the 24x24 viewBox (curves and arcs subdivided finely enough for large sizes), then cached.
//  - Drawing scales the cached polylines: AddPolyline with thickness 2/24 of the size (Lucide's stroke-width 2), closed
//    for Z, round caps and joins as small filled circles at the ends and at sharp corners; circles with AddCircle, rects
//    with AddRect and their corner radius. Everything follows the requested size, ImGui's anti-aliased lines do the rest.
// No device dependency. Render thread only (the menu is drawn there, under the overlay's ImGui lock).
#include "imgui.h"

namespace ApexUi {

// Order = ui/lucide_data.h kIcons (a static_assert in icons.cpp checks the count)
enum class IconId : int {
    Activity,          // activity
    Aperture,          // aperture
    AppWindow,         // app-window
    Armchair,          // armchair
    Blend,             // blend
    Bug,               // bug
    Camera,            // camera
    ChevronDown,       // chevron-down
    ChevronRight,      // chevron-right
    CircleCheck,       // circle-check
    CircleDashed,      // circle-dashed
    Columns2,          // columns-2
    Contrast,          // contrast
    Crosshair,         // crosshair
    Download,          // download
    Droplet,           // droplet
    ExternalLink,      // external-link
    Fence,             // fence
    Heart,             // heart
    House,             // house
    Image,             // image
    Info,              // info
    Keyboard,          // keyboard
    LandPlot,          // land-plot
    Layers,            // layers
    LayoutDashboard,   // layout-dashboard
    Lightbulb,         // lightbulb
    ListChecks,        // list-checks
    Maximize,          // maximize
    MirrorRound,       // mirror-round
    Monitor,           // monitor
    MoonStar,          // moon-star
    Moon,              // moon
    Palette,           // palette
    Puzzle,            // puzzle
    RotateCcw,         // rotate-ccw
    Save,              // save
    Scan,              // scan
    Settings,          // settings
    SlidersHorizontal, // sliders-horizontal
    Snowflake,         // snowflake
    Sparkles,          // sparkles
    Spline,            // spline
    Stethoscope,       // stethoscope
    SunMedium,         // sun-medium
    Sun,               // sun
    Thermometer,       // thermometer
    Trees,             // trees
    TriangleAlert,     // triangle-alert
    Type,              // type
    WavesHorizontal,   // waves-horizontal
    Wrench,            // wrench
    X,                 // x
    // added by hand (not in third_party/lucide/icons, see ui/lucide_data.h)
    Search,        // search
    Undo2,         // undo-2
    ChevronLeft,   // chevron-left
    ChevronsLeft,  // chevrons-left
    ChevronsRight, // chevrons-right
    Check,         // check
    Gauge,         // gauge
    Eye,           // eye
    Bookmark,      // bookmark
    Trash2,        // trash-2
    UserRound,     // user-round
    Count,
    None = Count, // "no icon" for widgets that take an optional one
};

// Draws the icon in dl with its top-left corner at pos, sizePx wide and high, in color. Nothing for IconId::None.
void DrawIcon(ImDrawList* dl, IconId id, ImVec2 pos, float sizePx, ImU32 color);
// Same, into the current window's draw list (no item is submitted).
void Icon(IconId id, ImVec2 pos, float sizePx, ImU32 color);
// Inline: submits a sizePx square item at the cursor (Dummy) and draws the icon in it, vertically centred on the text
// line when the icon is smaller than it. Follow with ImGui::SameLine() to put a label after it.
void InlineIcon(IconId id, float sizePx, ImU32 color);
// An icon (text-sized) then the label, on one line (the label is a normal text item, hover it for a tooltip).
void IconLabel(IconId id, const char* label, ImU32 iconColor);

} // namespace ApexUi
