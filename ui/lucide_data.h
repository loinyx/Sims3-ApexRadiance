#pragma once
// Lucide icons (ISC License, Copyright (c) Lucide Icons and Contributors; full text in third_party/lucide/LICENSE).
// The elements of third_party/lucide/icons/<name>.svg copied as data (no build step): every icon is a 24x24 viewBox
// drawn as strokes (fill none, stroke-width 2, round caps and joins) made of path (d as in the file), circle, rect and
// line elements; `filled` marks the few elements with fill="currentColor" (the dots of palette). Only the icons the
// menu uses are here, in the order of ApexUi::IconId (ui/icons.h). To add one: copy its elements from the .svg by
// hand, append it to kIcons and to IconId.

namespace ApexUi::LucideData {

enum class Kind : unsigned char { Path, Circle, Rect, Line };

// Path: d. Circle: v = cx, cy, r. Rect: v = x, y, width, height, rx. Line: v = x1, y1, x2, y2.
struct Element {
    Kind kind;
    const char* d;
    float v[5];
    bool filled;
};
struct IconData {
    const char* name; // the Lucide file name
    const Element* elements;
    int count;
};

// activity.svg
inline constexpr Element kActivity[] = {
    {Kind::Path, "M22 12h-2.48a2 2 0 0 0-1.93 1.46l-2.35 8.36a.25.25 0 0 1-.48 0L9.24 2.18a.25.25 0 0 0-.48 0l-2.35 8.36A2 2 0 0 1 4.49 12H2", {}, false},
};
// aperture.svg
inline constexpr Element kAperture[] = {
    {Kind::Circle, nullptr, {12.0f, 12.0f, 10.0f}, false},
    {Kind::Path, "m14.31 8 5.74 9.94", {}, false},
    {Kind::Path, "M9.69 8h11.48", {}, false},
    {Kind::Path, "m7.38 12 5.74-9.94", {}, false},
    {Kind::Path, "M9.69 16 3.95 6.06", {}, false},
    {Kind::Path, "M14.31 16H2.83", {}, false},
    {Kind::Path, "m16.62 12-5.74 9.94", {}, false},
};
// app-window.svg
inline constexpr Element kAppWindow[] = {
    {Kind::Rect, nullptr, {2.0f, 4.0f, 20.0f, 16.0f, 2.0f}, false},
    {Kind::Path, "M10 4v4", {}, false},
    {Kind::Path, "M2 8h20", {}, false},
    {Kind::Path, "M6 4v4", {}, false},
};
// armchair.svg
inline constexpr Element kArmchair[] = {
    {Kind::Path, "M19 9V6a2 2 0 0 0-2-2H7a2 2 0 0 0-2 2v3", {}, false},
    {Kind::Path, "M3 16a2 2 0 0 0 2 2h14a2 2 0 0 0 2-2v-5a2 2 0 0 0-4 0v1.5a.5.5 0 0 1-.5.5h-9a.5.5 0 0 1-.5-.5V11a2 2 0 0 0-4 0z", {}, false},
    {Kind::Path, "M5 18v2", {}, false},
    {Kind::Path, "M19 18v2", {}, false},
};
// blend.svg
inline constexpr Element kBlend[] = {
    {Kind::Circle, nullptr, {15.0f, 9.0f, 7.0f}, false},
    {Kind::Circle, nullptr, {9.0f, 15.0f, 7.0f}, false},
};
// bug.svg
inline constexpr Element kBug[] = {
    {Kind::Path, "M12 20v-9", {}, false},
    {Kind::Path, "M14 7a4 4 0 0 1 4 4v3a6 6 0 0 1-12 0v-3a4 4 0 0 1 4-4z", {}, false},
    {Kind::Path, "M14.12 3.88 16 2", {}, false},
    {Kind::Path, "M21 21a4 4 0 0 0-3.81-4", {}, false},
    {Kind::Path, "M21 5a4 4 0 0 1-3.55 3.97", {}, false},
    {Kind::Path, "M22 13h-4", {}, false},
    {Kind::Path, "M3 21a4 4 0 0 1 3.81-4", {}, false},
    {Kind::Path, "M3 5a4 4 0 0 0 3.55 3.97", {}, false},
    {Kind::Path, "M6 13H2", {}, false},
    {Kind::Path, "m8 2 1.88 1.88", {}, false},
    {Kind::Path, "M9 7.13V6a3 3 0 1 1 6 0v1.13", {}, false},
};
// camera.svg
inline constexpr Element kCamera[] = {
    {Kind::Path, "M13.997 4a2 2 0 0 1 1.76 1.05l.486.9A2 2 0 0 0 18.003 7H20a2 2 0 0 1 2 2v9a2 2 0 0 1-2 2H4a2 2 0 0 1-2-2V9a2 2 0 0 1 2-2h1.997a2 2 0 0 0 1.759-1.048l.489-.904A2 2 0 0 1 10.004 4z", {}, false},
    {Kind::Circle, nullptr, {12.0f, 13.0f, 3.0f}, false},
};
// chevron-down.svg
inline constexpr Element kChevronDown[] = {
    {Kind::Path, "m6 9 6 6 6-6", {}, false},
};
// chevron-right.svg
inline constexpr Element kChevronRight[] = {
    {Kind::Path, "m9 18 6-6-6-6", {}, false},
};
// circle-check.svg
inline constexpr Element kCircleCheck[] = {
    {Kind::Circle, nullptr, {12.0f, 12.0f, 10.0f}, false},
    {Kind::Path, "m16 9-5.5 5.5L8 12", {}, false},
};
// circle-dashed.svg
inline constexpr Element kCircleDashed[] = {
    {Kind::Path, "M10.1 2.182a10 10 0 0 1 3.8 0", {}, false},
    {Kind::Path, "M13.9 21.818a10 10 0 0 1-3.8 0", {}, false},
    {Kind::Path, "M17.609 3.721a10 10 0 0 1 2.69 2.7", {}, false},
    {Kind::Path, "M2.182 13.9a10 10 0 0 1 0-3.8", {}, false},
    {Kind::Path, "M20.279 17.609a10 10 0 0 1-2.7 2.69", {}, false},
    {Kind::Path, "M21.818 10.1a10 10 0 0 1 0 3.8", {}, false},
    {Kind::Path, "M3.721 6.391a10 10 0 0 1 2.7-2.69", {}, false},
    {Kind::Path, "M6.391 20.279a10 10 0 0 1-2.69-2.7", {}, false},
};
// columns-2.svg
inline constexpr Element kColumns2[] = {
    {Kind::Rect, nullptr, {3.0f, 3.0f, 18.0f, 18.0f, 2.0f}, false},
    {Kind::Path, "M12 3v18", {}, false},
};
// contrast.svg
inline constexpr Element kContrast[] = {
    {Kind::Circle, nullptr, {12.0f, 12.0f, 10.0f}, false},
    {Kind::Path, "M12 18a6 6 0 0 0 0-12v12z", {}, false},
};
// crosshair.svg
inline constexpr Element kCrosshair[] = {
    {Kind::Circle, nullptr, {12.0f, 12.0f, 10.0f}, false},
    {Kind::Line, nullptr, {22.0f, 12.0f, 18.0f, 12.0f}, false},
    {Kind::Line, nullptr, {6.0f, 12.0f, 2.0f, 12.0f}, false},
    {Kind::Line, nullptr, {12.0f, 6.0f, 12.0f, 2.0f}, false},
    {Kind::Line, nullptr, {12.0f, 22.0f, 12.0f, 18.0f}, false},
};
// download.svg
inline constexpr Element kDownload[] = {
    {Kind::Path, "M12 15V3", {}, false},
    {Kind::Path, "M21 15v4a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2v-4", {}, false},
    {Kind::Path, "m7 10 5 5 5-5", {}, false},
};
// droplet.svg
inline constexpr Element kDroplet[] = {
    {Kind::Path, "M12 22a7 7 0 0 0 7-7c0-2-1-3.9-3-5.5s-3.5-4-4-6.5c-.5 2.5-2 4.9-4 6.5C6 11.1 5 13 5 15a7 7 0 0 0 7 7z", {}, false},
};
// external-link.svg
inline constexpr Element kExternalLink[] = {
    {Kind::Path, "M15 3h6v6", {}, false},
    {Kind::Path, "M10 14 21 3", {}, false},
    {Kind::Path, "M18 13v6a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V8a2 2 0 0 1 2-2h6", {}, false},
};
// fence.svg
inline constexpr Element kFence[] = {
    {Kind::Path, "M4 3 2 5v15c0 .6.4 1 1 1h2c.6 0 1-.4 1-1V5Z", {}, false},
    {Kind::Path, "M6 8h4", {}, false},
    {Kind::Path, "M6 18h4", {}, false},
    {Kind::Path, "m12 3-2 2v15c0 .6.4 1 1 1h2c.6 0 1-.4 1-1V5Z", {}, false},
    {Kind::Path, "M14 8h4", {}, false},
    {Kind::Path, "M14 18h4", {}, false},
    {Kind::Path, "m20 3-2 2v15c0 .6.4 1 1 1h2c.6 0 1-.4 1-1V5Z", {}, false},
};
// heart.svg
inline constexpr Element kHeart[] = {
    {Kind::Path, "M2 9.5a5.5 5.5 0 0 1 9.591-3.676.56.56 0 0 0 .818 0A5.49 5.49 0 0 1 22 9.5c0 2.29-1.5 4-3 5.5l-5.492 5.313a2 2 0 0 1-3 .019L5 15c-1.5-1.5-3-3.2-3-5.5", {}, false},
};
// house.svg
inline constexpr Element kHouse[] = {
    {Kind::Path, "M15 21v-8a1 1 0 0 0-1-1h-4a1 1 0 0 0-1 1v8", {}, false},
    {Kind::Path, "M3 10a2 2 0 0 1 .709-1.528l7-6a2 2 0 0 1 2.582 0l7 6A2 2 0 0 1 21 10v9a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2z", {}, false},
};
// image.svg
inline constexpr Element kImage[] = {
    {Kind::Rect, nullptr, {3.0f, 3.0f, 18.0f, 18.0f, 2.0f}, false},
    {Kind::Circle, nullptr, {9.0f, 9.0f, 2.0f}, false},
    {Kind::Path, "m21 15-3.086-3.086a2 2 0 0 0-2.828 0L6 21", {}, false},
};
// info.svg
inline constexpr Element kInfo[] = {
    {Kind::Circle, nullptr, {12.0f, 12.0f, 10.0f}, false},
    {Kind::Path, "M12 16v-4", {}, false},
    {Kind::Path, "M12 8h.01", {}, false},
};
// keyboard.svg
inline constexpr Element kKeyboard[] = {
    {Kind::Path, "M10 8h.01", {}, false},
    {Kind::Path, "M12 12h.01", {}, false},
    {Kind::Path, "M14 8h.01", {}, false},
    {Kind::Path, "M16 12h.01", {}, false},
    {Kind::Path, "M18 8h.01", {}, false},
    {Kind::Path, "M6 8h.01", {}, false},
    {Kind::Path, "M7 16h10", {}, false},
    {Kind::Path, "M8 12h.01", {}, false},
    {Kind::Rect, nullptr, {2.0f, 4.0f, 20.0f, 16.0f, 2.0f}, false},
};
// land-plot.svg
inline constexpr Element kLandPlot[] = {
    {Kind::Path, "m12 8 6-3-6-3v10", {}, false},
    {Kind::Path, "m8 11.99-5.5 3.14a1 1 0 0 0 0 1.74l8.5 4.86a2 2 0 0 0 2 0l8.5-4.86a1 1 0 0 0 0-1.74L16 12", {}, false},
    {Kind::Path, "m6.49 12.85 11.02 6.3", {}, false},
    {Kind::Path, "M17.51 12.85 6.5 19.15", {}, false},
};
// layers.svg
inline constexpr Element kLayers[] = {
    {Kind::Path, "M12.83 2.18a2 2 0 0 0-1.66 0L2.6 6.08a1 1 0 0 0 0 1.83l8.58 3.91a2 2 0 0 0 1.66 0l8.58-3.9a1 1 0 0 0 0-1.83z", {}, false},
    {Kind::Path, "M2 12a1 1 0 0 0 .58.91l8.6 3.91a2 2 0 0 0 1.65 0l8.58-3.9A1 1 0 0 0 22 12", {}, false},
    {Kind::Path, "M2 17a1 1 0 0 0 .58.91l8.6 3.91a2 2 0 0 0 1.65 0l8.58-3.9A1 1 0 0 0 22 17", {}, false},
};
// layout-dashboard.svg
inline constexpr Element kLayoutDashboard[] = {
    {Kind::Rect, nullptr, {3.0f, 3.0f, 7.0f, 9.0f, 1.0f}, false},
    {Kind::Rect, nullptr, {14.0f, 3.0f, 7.0f, 5.0f, 1.0f}, false},
    {Kind::Rect, nullptr, {14.0f, 12.0f, 7.0f, 9.0f, 1.0f}, false},
    {Kind::Rect, nullptr, {3.0f, 16.0f, 7.0f, 5.0f, 1.0f}, false},
};
// lightbulb.svg
inline constexpr Element kLightbulb[] = {
    {Kind::Path, "M15 14c.2-1 .7-1.7 1.5-2.5 1-.9 1.5-2.2 1.5-3.5A6 6 0 0 0 6 8c0 1 .2 2.2 1.5 3.5.7.7 1.3 1.5 1.5 2.5", {}, false},
    {Kind::Path, "M9 18h6", {}, false},
    {Kind::Path, "M10 22h4", {}, false},
};
// list-checks.svg
inline constexpr Element kListChecks[] = {
    {Kind::Path, "M13 5h8", {}, false},
    {Kind::Path, "M13 12h8", {}, false},
    {Kind::Path, "M13 19h8", {}, false},
    {Kind::Path, "m3 17 2 2 4-4", {}, false},
    {Kind::Path, "m3 7 2 2 4-4", {}, false},
};
// maximize.svg
inline constexpr Element kMaximize[] = {
    {Kind::Path, "M8 3H5a2 2 0 0 0-2 2v3", {}, false},
    {Kind::Path, "M21 8V5a2 2 0 0 0-2-2h-3", {}, false},
    {Kind::Path, "M3 16v3a2 2 0 0 0 2 2h3", {}, false},
    {Kind::Path, "M16 21h3a2 2 0 0 0 2-2v-3", {}, false},
};
// mirror-round.svg
inline constexpr Element kMirrorRound[] = {
    {Kind::Path, "M10 6.6 8.6 8", {}, false},
    {Kind::Path, "M12 18v4", {}, false},
    {Kind::Path, "M15 7.5 9.5 13", {}, false},
    {Kind::Path, "M7 22h10", {}, false},
    {Kind::Circle, nullptr, {12.0f, 10.0f, 8.0f}, false},
};
// monitor.svg
inline constexpr Element kMonitor[] = {
    {Kind::Rect, nullptr, {2.0f, 3.0f, 20.0f, 14.0f, 2.0f}, false},
    {Kind::Line, nullptr, {8.0f, 21.0f, 16.0f, 21.0f}, false},
    {Kind::Line, nullptr, {12.0f, 17.0f, 12.0f, 21.0f}, false},
};
// moon-star.svg
inline constexpr Element kMoonStar[] = {
    {Kind::Path, "M18 5h4", {}, false},
    {Kind::Path, "M20 3v4", {}, false},
    {Kind::Path, "M20.985 12.486a9 9 0 1 1-9.473-9.472c.405-.022.617.46.402.803a6 6 0 0 0 8.268 8.268c.344-.215.825-.004.803.401", {}, false},
};
// moon.svg
inline constexpr Element kMoon[] = {
    {Kind::Path, "M20.985 12.486a9 9 0 1 1-9.473-9.472c.405-.022.617.46.402.803a6 6 0 0 0 8.268 8.268c.344-.215.825-.004.803.401", {}, false},
};
// palette.svg
inline constexpr Element kPalette[] = {
    {Kind::Path, "M12 22a1 1 0 0 1 0-20 10 9 0 0 1 10 9 5 5 0 0 1-5 5h-2.25a1.75 1.75 0 0 0-1.4 2.8l.3.4a1.75 1.75 0 0 1-1.4 2.8z", {}, false},
    {Kind::Circle, nullptr, {13.5f, 6.5f, 0.5f}, true},
    {Kind::Circle, nullptr, {17.5f, 10.5f, 0.5f}, true},
    {Kind::Circle, nullptr, {6.5f, 12.5f, 0.5f}, true},
    {Kind::Circle, nullptr, {8.5f, 7.5f, 0.5f}, true},
};
// puzzle.svg
inline constexpr Element kPuzzle[] = {
    {Kind::Path, "M15.39 4.39a1 1 0 0 0 1.68-.474 2.5 2.5 0 1 1 3.014 3.015 1 1 0 0 0-.474 1.68l1.683 1.682a2.414 2.414 0 0 1 0 3.414L19.61 15.39a1 1 0 0 1-1.68-.474 2.5 2.5 0 1 0-3.014 3.015 1 1 0 0 1 .474 1.68l-1.683 1.682a2.414 2.414 0 0 1-3.414 0L8.61 19.61a1 1 0 0 0-1.68.474 2.5 2.5 0 1 1-3.014-3.015 1 1 0 0 0 .474-1.68l-1.683-1.682a2.414 2.414 0 0 1 0-3.414L4.39 8.61a1 1 0 0 1 1.68.474 2.5 2.5 0 1 0 3.014-3.015 1 1 0 0 1-.474-1.68l1.683-1.682a2.414 2.414 0 0 1 3.414 0z", {}, false},
};
// rotate-ccw.svg
inline constexpr Element kRotateCcw[] = {
    {Kind::Path, "M3 12a9 9 0 1 0 9-9 9.75 9.75 0 0 0-6.74 2.74L3 8", {}, false},
    {Kind::Path, "M3 3v5h5", {}, false},
};
// save.svg
inline constexpr Element kSave[] = {
    {Kind::Path, "M15.2 3a2 2 0 0 1 1.4.6l3.8 3.8a2 2 0 0 1 .6 1.4V19a2 2 0 0 1-2 2H5a2 2 0 0 1-2-2V5a2 2 0 0 1 2-2z", {}, false},
    {Kind::Path, "M17 21v-7a1 1 0 0 0-1-1H8a1 1 0 0 0-1 1v7", {}, false},
    {Kind::Path, "M7 3v4a1 1 0 0 0 1 1h7", {}, false},
};
// scan.svg
inline constexpr Element kScan[] = {
    {Kind::Path, "M3 7V5a2 2 0 0 1 2-2h2", {}, false},
    {Kind::Path, "M17 3h2a2 2 0 0 1 2 2v2", {}, false},
    {Kind::Path, "M21 17v2a2 2 0 0 1-2 2h-2", {}, false},
    {Kind::Path, "M7 21H5a2 2 0 0 1-2-2v-2", {}, false},
};
// settings.svg
inline constexpr Element kSettings[] = {
    {Kind::Path, "M9.671 4.136a2.34 2.34 0 0 1 4.659 0 2.34 2.34 0 0 0 3.319 1.915 2.34 2.34 0 0 1 2.33 4.033 2.34 2.34 0 0 0 0 3.831 2.34 2.34 0 0 1-2.33 4.033 2.34 2.34 0 0 0-3.319 1.915 2.34 2.34 0 0 1-4.659 0 2.34 2.34 0 0 0-3.32-1.915 2.34 2.34 0 0 1-2.33-4.033 2.34 2.34 0 0 0 0-3.831A2.34 2.34 0 0 1 6.35 6.051a2.34 2.34 0 0 0 3.319-1.915", {}, false},
    {Kind::Circle, nullptr, {12.0f, 12.0f, 3.0f}, false},
};
// sliders-horizontal.svg
inline constexpr Element kSlidersHorizontal[] = {
    {Kind::Path, "M10 5H3", {}, false},
    {Kind::Path, "M12 19H3", {}, false},
    {Kind::Path, "M14 3v4", {}, false},
    {Kind::Path, "M16 17v4", {}, false},
    {Kind::Path, "M21 12h-9", {}, false},
    {Kind::Path, "M21 19h-5", {}, false},
    {Kind::Path, "M21 5h-7", {}, false},
    {Kind::Path, "M8 10v4", {}, false},
    {Kind::Path, "M8 12H3", {}, false},
};
// snowflake.svg
inline constexpr Element kSnowflake[] = {
    {Kind::Path, "m10 20-1.25-2.5L6 18", {}, false},
    {Kind::Path, "M10 4 8.75 6.5 6 6", {}, false},
    {Kind::Path, "m14 20 1.25-2.5L18 18", {}, false},
    {Kind::Path, "m14 4 1.25 2.5L18 6", {}, false},
    {Kind::Path, "m17 21-3-6h-4", {}, false},
    {Kind::Path, "m17 3-3 6 1.5 3", {}, false},
    {Kind::Path, "M2 12h6.5L10 9", {}, false},
    {Kind::Path, "m20 10-1.5 2 1.5 2", {}, false},
    {Kind::Path, "M22 12h-6.5L14 15", {}, false},
    {Kind::Path, "m4 10 1.5 2L4 14", {}, false},
    {Kind::Path, "m7 21 3-6-1.5-3", {}, false},
    {Kind::Path, "m7 3 3 6h4", {}, false},
};
// sparkles.svg
inline constexpr Element kSparkles[] = {
    {Kind::Path, "M11.017 2.814a1 1 0 0 1 1.966 0l1.051 5.558a2 2 0 0 0 1.594 1.594l5.558 1.051a1 1 0 0 1 0 1.966l-5.558 1.051a2 2 0 0 0-1.594 1.594l-1.051 5.558a1 1 0 0 1-1.966 0l-1.051-5.558a2 2 0 0 0-1.594-1.594l-5.558-1.051a1 1 0 0 1 0-1.966l5.558-1.051a2 2 0 0 0 1.594-1.594z", {}, false},
    {Kind::Path, "M20 2v4", {}, false},
    {Kind::Path, "M22 4h-4", {}, false},
    {Kind::Circle, nullptr, {4.0f, 20.0f, 2.0f}, false},
};
// spline.svg
inline constexpr Element kSpline[] = {
    {Kind::Circle, nullptr, {19.0f, 5.0f, 2.0f}, false},
    {Kind::Circle, nullptr, {5.0f, 19.0f, 2.0f}, false},
    {Kind::Path, "M5 17A12 12 0 0 1 17 5", {}, false},
};
// stethoscope.svg
inline constexpr Element kStethoscope[] = {
    {Kind::Path, "M11 2v2", {}, false},
    {Kind::Path, "M5 2v2", {}, false},
    {Kind::Path, "M5 3H4a2 2 0 0 0-2 2v4a6 6 0 0 0 12 0V5a2 2 0 0 0-2-2h-1", {}, false},
    {Kind::Path, "M8 15a6 6 0 0 0 12 0v-3", {}, false},
    {Kind::Circle, nullptr, {20.0f, 10.0f, 2.0f}, false},
};
// sun-medium.svg
inline constexpr Element kSunMedium[] = {
    {Kind::Circle, nullptr, {12.0f, 12.0f, 4.0f}, false},
    {Kind::Path, "M12 3v1", {}, false},
    {Kind::Path, "M12 20v1", {}, false},
    {Kind::Path, "M3 12h1", {}, false},
    {Kind::Path, "M20 12h1", {}, false},
    {Kind::Path, "m18.364 5.636-.707.707", {}, false},
    {Kind::Path, "m6.343 17.657-.707.707", {}, false},
    {Kind::Path, "m5.636 5.636.707.707", {}, false},
    {Kind::Path, "m17.657 17.657.707.707", {}, false},
};
// sun.svg
inline constexpr Element kSun[] = {
    {Kind::Circle, nullptr, {12.0f, 12.0f, 4.0f}, false},
    {Kind::Path, "M12 2v2", {}, false},
    {Kind::Path, "M12 20v2", {}, false},
    {Kind::Path, "m4.93 4.93 1.41 1.41", {}, false},
    {Kind::Path, "m17.66 17.66 1.41 1.41", {}, false},
    {Kind::Path, "M2 12h2", {}, false},
    {Kind::Path, "M20 12h2", {}, false},
    {Kind::Path, "m6.34 17.66-1.41 1.41", {}, false},
    {Kind::Path, "m19.07 4.93-1.41 1.41", {}, false},
};
// thermometer.svg
inline constexpr Element kThermometer[] = {
    {Kind::Path, "M14 4v10.54a4 4 0 1 1-4 0V4a2 2 0 0 1 4 0Z", {}, false},
};
// trees.svg
inline constexpr Element kTrees[] = {
    {Kind::Path, "M10 10v.2A3 3 0 0 1 8.9 16H5a3 3 0 0 1-1-5.8V10a3 3 0 0 1 6 0Z", {}, false},
    {Kind::Path, "M7 16v6", {}, false},
    {Kind::Path, "M13 19v3", {}, false},
    {Kind::Path, "M12 19h8.3a1 1 0 0 0 .7-1.7L18 14h.3a1 1 0 0 0 .7-1.7L16 9h.2a1 1 0 0 0 .8-1.7L13 3l-1.4 1.5", {}, false},
};
// triangle-alert.svg
inline constexpr Element kTriangleAlert[] = {
    {Kind::Path, "m21.73 18-8-14a2 2 0 0 0-3.48 0l-8 14A2 2 0 0 0 4 21h16a2 2 0 0 0 1.73-3", {}, false},
    {Kind::Path, "M12 9v4", {}, false},
    {Kind::Path, "M12 17h.01", {}, false},
};
// type.svg
inline constexpr Element kType[] = {
    {Kind::Path, "M12 4v16", {}, false},
    {Kind::Path, "M4 7V5a1 1 0 0 1 1-1h14a1 1 0 0 1 1 1v2", {}, false},
    {Kind::Path, "M9 20h6", {}, false},
};
// waves-horizontal.svg
inline constexpr Element kWavesHorizontal[] = {
    {Kind::Path, "M2 12q2.5 2 5 0t5 0 5 0 5 0", {}, false},
    {Kind::Path, "M2 19q2.5 2 5 0t5 0 5 0 5 0", {}, false},
    {Kind::Path, "M2 5q2.5 2 5 0t5 0 5 0 5 0", {}, false},
};
// wrench.svg
inline constexpr Element kWrench[] = {
    {Kind::Path, "M14.7 6.3a1 1 0 0 0 0 1.4l1.6 1.6a1 1 0 0 0 1.4 0l3.106-3.105c.32-.322.863-.22.983.218a6 6 0 0 1-8.259 7.057l-7.91 7.91a1 1 0 0 1-2.999-3l7.91-7.91a6 6 0 0 1 7.057-8.259c.438.12.54.662.219.984z", {}, false},
};
// x.svg
inline constexpr Element kX[] = {
    {Kind::Path, "M18 6 6 18", {}, false},
    {Kind::Path, "m6 6 12 12", {}, false},
};
// ---- Added by hand from Lucide SVGs for search / undo / looks / profiles / Sim Occlusion:
// their elements were entered by hand from Lucide's published icons (same 24x24 stroke format), not copied from a file
// in third_party/lucide/icons/.
// search
inline constexpr Element kSearch[] = {
    {Kind::Circle, nullptr, {11.0f, 11.0f, 8.0f}, false},
    {Kind::Path, "m21 21-4.3-4.3", {}, false},
};
// undo-2
inline constexpr Element kUndo2[] = {
    {Kind::Path, "M9 14 4 9l5-5", {}, false},
    {Kind::Path, "M4 9h10.5a5.5 5.5 0 0 1 5.5 5.5a5.5 5.5 0 0 1-5.5 5.5H11", {}, false},
};
// chevron-left
inline constexpr Element kChevronLeft[] = {
    {Kind::Path, "m15 18-6-6 6-6", {}, false},
};
// chevrons-left
inline constexpr Element kChevronsLeft[] = {
    {Kind::Path, "m11 17-5-5 5-5", {}, false},
    {Kind::Path, "m18 17-5-5 5-5", {}, false},
};
// chevrons-right
inline constexpr Element kChevronsRight[] = {
    {Kind::Path, "m6 17 5-5-5-5", {}, false},
    {Kind::Path, "m13 17 5-5-5-5", {}, false},
};
// check
inline constexpr Element kCheck[] = {
    {Kind::Path, "M20 6 9 17l-5-5", {}, false},
};
// gauge
inline constexpr Element kGauge[] = {
    {Kind::Path, "m12 14 4-4", {}, false},
    {Kind::Path, "M3.34 19a10 10 0 1 1 17.32 0", {}, false},
};
// eye
inline constexpr Element kEye[] = {
    {Kind::Path, "M2.062 12.348a1 1 0 0 1 0-.696 10.75 10.75 0 0 1 19.876 0 1 1 0 0 1 0 .696 10.75 10.75 0 0 1-19.876 0", {}, false},
    {Kind::Circle, nullptr, {12.0f, 12.0f, 3.0f}, false},
};
// bookmark
inline constexpr Element kBookmark[] = {
    {Kind::Path, "m19 21-7-4-7 4V5a2 2 0 0 1 2-2h10a2 2 0 0 1 2 2v16z", {}, false},
};
// trash-2
inline constexpr Element kTrash2[] = {
    {Kind::Path, "M3 6h18", {}, false},
    {Kind::Path, "M19 6v14c0 1-1 2-2 2H7c-1 0-2-1-2-2V6", {}, false},
    {Kind::Path, "M8 6V4c0-1 1-2 2-2h4c1 0 2 1 2 2v2", {}, false},
    {Kind::Line, nullptr, {10.0f, 11.0f, 10.0f, 17.0f}, false},
    {Kind::Line, nullptr, {14.0f, 11.0f, 14.0f, 17.0f}, false},
};
// user-round.svg
inline constexpr Element kUserRound[] = {
    {Kind::Circle, nullptr, {12.0f, 8.0f, 5.0f}, false},
    {Kind::Path, "M20 21a8 8 0 0 0-16 0", {}, false},
};

inline constexpr IconData kIcons[] = {
    {"activity", kActivity, static_cast<int>(sizeof(kActivity) / sizeof(kActivity[0]))},
    {"aperture", kAperture, static_cast<int>(sizeof(kAperture) / sizeof(kAperture[0]))},
    {"app-window", kAppWindow, static_cast<int>(sizeof(kAppWindow) / sizeof(kAppWindow[0]))},
    {"armchair", kArmchair, static_cast<int>(sizeof(kArmchair) / sizeof(kArmchair[0]))},
    {"blend", kBlend, static_cast<int>(sizeof(kBlend) / sizeof(kBlend[0]))},
    {"bug", kBug, static_cast<int>(sizeof(kBug) / sizeof(kBug[0]))},
    {"camera", kCamera, static_cast<int>(sizeof(kCamera) / sizeof(kCamera[0]))},
    {"chevron-down", kChevronDown, static_cast<int>(sizeof(kChevronDown) / sizeof(kChevronDown[0]))},
    {"chevron-right", kChevronRight, static_cast<int>(sizeof(kChevronRight) / sizeof(kChevronRight[0]))},
    {"circle-check", kCircleCheck, static_cast<int>(sizeof(kCircleCheck) / sizeof(kCircleCheck[0]))},
    {"circle-dashed", kCircleDashed, static_cast<int>(sizeof(kCircleDashed) / sizeof(kCircleDashed[0]))},
    {"columns-2", kColumns2, static_cast<int>(sizeof(kColumns2) / sizeof(kColumns2[0]))},
    {"contrast", kContrast, static_cast<int>(sizeof(kContrast) / sizeof(kContrast[0]))},
    {"crosshair", kCrosshair, static_cast<int>(sizeof(kCrosshair) / sizeof(kCrosshair[0]))},
    {"download", kDownload, static_cast<int>(sizeof(kDownload) / sizeof(kDownload[0]))},
    {"droplet", kDroplet, static_cast<int>(sizeof(kDroplet) / sizeof(kDroplet[0]))},
    {"external-link", kExternalLink, static_cast<int>(sizeof(kExternalLink) / sizeof(kExternalLink[0]))},
    {"fence", kFence, static_cast<int>(sizeof(kFence) / sizeof(kFence[0]))},
    {"heart", kHeart, static_cast<int>(sizeof(kHeart) / sizeof(kHeart[0]))},
    {"house", kHouse, static_cast<int>(sizeof(kHouse) / sizeof(kHouse[0]))},
    {"image", kImage, static_cast<int>(sizeof(kImage) / sizeof(kImage[0]))},
    {"info", kInfo, static_cast<int>(sizeof(kInfo) / sizeof(kInfo[0]))},
    {"keyboard", kKeyboard, static_cast<int>(sizeof(kKeyboard) / sizeof(kKeyboard[0]))},
    {"land-plot", kLandPlot, static_cast<int>(sizeof(kLandPlot) / sizeof(kLandPlot[0]))},
    {"layers", kLayers, static_cast<int>(sizeof(kLayers) / sizeof(kLayers[0]))},
    {"layout-dashboard", kLayoutDashboard, static_cast<int>(sizeof(kLayoutDashboard) / sizeof(kLayoutDashboard[0]))},
    {"lightbulb", kLightbulb, static_cast<int>(sizeof(kLightbulb) / sizeof(kLightbulb[0]))},
    {"list-checks", kListChecks, static_cast<int>(sizeof(kListChecks) / sizeof(kListChecks[0]))},
    {"maximize", kMaximize, static_cast<int>(sizeof(kMaximize) / sizeof(kMaximize[0]))},
    {"mirror-round", kMirrorRound, static_cast<int>(sizeof(kMirrorRound) / sizeof(kMirrorRound[0]))},
    {"monitor", kMonitor, static_cast<int>(sizeof(kMonitor) / sizeof(kMonitor[0]))},
    {"moon-star", kMoonStar, static_cast<int>(sizeof(kMoonStar) / sizeof(kMoonStar[0]))},
    {"moon", kMoon, static_cast<int>(sizeof(kMoon) / sizeof(kMoon[0]))},
    {"palette", kPalette, static_cast<int>(sizeof(kPalette) / sizeof(kPalette[0]))},
    {"puzzle", kPuzzle, static_cast<int>(sizeof(kPuzzle) / sizeof(kPuzzle[0]))},
    {"rotate-ccw", kRotateCcw, static_cast<int>(sizeof(kRotateCcw) / sizeof(kRotateCcw[0]))},
    {"save", kSave, static_cast<int>(sizeof(kSave) / sizeof(kSave[0]))},
    {"scan", kScan, static_cast<int>(sizeof(kScan) / sizeof(kScan[0]))},
    {"settings", kSettings, static_cast<int>(sizeof(kSettings) / sizeof(kSettings[0]))},
    {"sliders-horizontal", kSlidersHorizontal, static_cast<int>(sizeof(kSlidersHorizontal) / sizeof(kSlidersHorizontal[0]))},
    {"snowflake", kSnowflake, static_cast<int>(sizeof(kSnowflake) / sizeof(kSnowflake[0]))},
    {"sparkles", kSparkles, static_cast<int>(sizeof(kSparkles) / sizeof(kSparkles[0]))},
    {"spline", kSpline, static_cast<int>(sizeof(kSpline) / sizeof(kSpline[0]))},
    {"stethoscope", kStethoscope, static_cast<int>(sizeof(kStethoscope) / sizeof(kStethoscope[0]))},
    {"sun-medium", kSunMedium, static_cast<int>(sizeof(kSunMedium) / sizeof(kSunMedium[0]))},
    {"sun", kSun, static_cast<int>(sizeof(kSun) / sizeof(kSun[0]))},
    {"thermometer", kThermometer, static_cast<int>(sizeof(kThermometer) / sizeof(kThermometer[0]))},
    {"trees", kTrees, static_cast<int>(sizeof(kTrees) / sizeof(kTrees[0]))},
    {"triangle-alert", kTriangleAlert, static_cast<int>(sizeof(kTriangleAlert) / sizeof(kTriangleAlert[0]))},
    {"type", kType, static_cast<int>(sizeof(kType) / sizeof(kType[0]))},
    {"waves-horizontal", kWavesHorizontal, static_cast<int>(sizeof(kWavesHorizontal) / sizeof(kWavesHorizontal[0]))},
    {"wrench", kWrench, static_cast<int>(sizeof(kWrench) / sizeof(kWrench[0]))},
    {"x", kX, static_cast<int>(sizeof(kX) / sizeof(kX[0]))},
    {"search", kSearch, static_cast<int>(sizeof(kSearch) / sizeof(kSearch[0]))},
    {"undo-2", kUndo2, static_cast<int>(sizeof(kUndo2) / sizeof(kUndo2[0]))},
    {"chevron-left", kChevronLeft, static_cast<int>(sizeof(kChevronLeft) / sizeof(kChevronLeft[0]))},
    {"chevrons-left", kChevronsLeft, static_cast<int>(sizeof(kChevronsLeft) / sizeof(kChevronsLeft[0]))},
    {"chevrons-right", kChevronsRight, static_cast<int>(sizeof(kChevronsRight) / sizeof(kChevronsRight[0]))},
    {"check", kCheck, static_cast<int>(sizeof(kCheck) / sizeof(kCheck[0]))},
    {"gauge", kGauge, static_cast<int>(sizeof(kGauge) / sizeof(kGauge[0]))},
    {"eye", kEye, static_cast<int>(sizeof(kEye) / sizeof(kEye[0]))},
    {"bookmark", kBookmark, static_cast<int>(sizeof(kBookmark) / sizeof(kBookmark[0]))},
    {"trash-2", kTrash2, static_cast<int>(sizeof(kTrash2) / sizeof(kTrash2[0]))},
    {"user-round", kUserRound, static_cast<int>(sizeof(kUserRound) / sizeof(kUserRound[0]))},
};

} // namespace ApexUi::LucideData
