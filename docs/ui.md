> Published 2.5.6: System > Performance begins with Optimize rendering, default on
> for missing settings, preserving explicit saved off choices. Its row reset and
> Reset all restore on. One unified ASI offers optional developer mode. The System
> display page is Edge Smoothing only; window and pacing controls are removed.

> Local development (not released): new configurations start with Ambient Occlusion enabled at 168% Strength, 351 m
> Distance and High quality. Advanced defaults are 130% Reach and 38% Keep lamp light. The separate Sim Occlusion
> card starts off; when enabled, its defaults are 47% body intensity, 38% hair intensity and 47% maximum darkening.
> See [Ambient Occlusion](features/ambient-occlusion.md) for shader coverage, fallback behavior, revision migration
> and validation limits.

> Local development: new configurations enable the filtered screenshot shortcut by default on C. Previously saved
> screenshot keys remain unchanged. It
> leaves bare F10 available for the game's UI toggle, which is used only internally while hiding the interface for a
> screenshot; the prior UI state is restored afterwards. Ctrl+Shift+F10 remains Compare.

> Current local development: the Optimize rendering card and its mode switch are
> removed; the existing performance patch controls remain. The published 2.5.6
> behavior described above is historical.

> Published since 2.5.5: Report uses the restored session/capture/list/help layout
> with optional title/description after saving. Earlier guided stages and required
> descriptions are superseded. See [bug-reports.md](features/bug-reports.md).

> Historical RC, superseded before 2.5.5: Display and fluency was tested with
> window/monitor selection and read-only VRR reports, then removed. See
> [display-fluency.md](features/display-fluency.md) for historical findings only.

# Menu UI (Violet design)

## Private RC Overview refinement (2026-10-02)

Overview keeps all eleven existing controls and page/tab links. Three Violet cards group them into Lighting
(Night Lights, Water Reflections, Faster Room Lighting, Lot Lighting While Moving), Image (Picture, Ambient
Occlusion, Banding Fix, Depth Blur, Edge Smoothing), and Performance and screen (Faster File Lookups, Borderless).
Profiles and favorites are not added to Overview. Switches alone convey enabled/disabled state; no repeated
"On" labels. Enabled Edge Smoothing shows its actual method/quality; Depth Blur shows its chosen focus mode.
The existing open-menu backbuffer MSAA check supplies "Waiting for game settings" on affected enabled rows.
Unknown/device-loading state does not invent a conflict. GPU chips retain measured-only timing and hide while
disabled or blocked. Long translated names wrap before the switch/chip.

The discreet reset icon beside the title confirms inline, restores only the displayed enable switches, water
reflection amount and window mode, and retains fine tuning, language, shortcuts, developer preferences and saved
files. Existing toast Undo is reused. Whole-mod reset is kept in Settings, not Overview. All new strings are EN,
PT-BR, ES and FR. No new hooks, timers, render passes, background work or saved keys. Compilation and translation
checks do not replace in-game verification of spacing, loading/Reset compatibility and restoration/Undo.

Private update, 2026-10-02: on-screen startup, recording, capture and comparison notices now share the same top-center position and Violet pill style. See [bug-reports.md](features/bug-reports.md#one-click-selection-and-centered-notices-private-2026-10-02) for priority, click confirmation and return-to-Report behavior. The simpler recording/saving layout is now implemented locally as `2.5.4-test-report-library`, with required details replacing the capture workspace rather than opening a modal. Panel opening and notices wait for the loaded world; this is not part of the published 2.5.4 binary.

The in-game menu of Apex Radiance (window id `###ApexWindow`, default key Ctrl+Shift+F11; layout saved in
`Documents\...\Apex Radiance\apex_radiance_imgui.ini`). Visible names come from `apex_version.h` (`APEX_PRODUCT_NAME` =
"Apex Radiance", `APEX_PRODUCT_TAGLINE` = "for The Sims 3", `APEX_LOGO_LETTER`); internal names keep "Apex". Write
"Apex Radiance" in visible text through `APEX_PRODUCT_NAME`, never the old "S3SS Apex" / "Apex Edition".

History: reorganised on 2026-09-28 for players (short plain words, one feature per card, developer items on one
Developer page); the same day it got a design polish (spacing scale, row descriptions, dividers, tinted notes, button
styles), a full copy rewrite, and the grouped sidebar with tabbed pages below. Later that day (approved from a mockup):
search, changed markers with per-setting Reset, the undo toast, Profiles, peek, hold to compare, GPU cost
chips, "Reload save" badges, inline "Turn on" dependencies, the welcome tour, the status bar, the collapsible sidebar,
colour tracks and keyboard use (sections below).

## Files
- `apex_gui.cpp`: window, header, sidebar, pages, search results, Profiles, welcome tour, undo toast, status
  bar, first-launch hint. Header: the logo (ui/logo.h: ui/apex_logo.png as a 128x128 texture with mips, ui/logo_data.h; the old violet tile with `APEX_LOGO_LETTER` only if the texture cannot be made), name and tagline (centred on the logo), the search field, Night/Day pill
  (moon / sun), frame-time pill, close (x); all vertically centred on the 32 px tile (narrow windows drop the Night/Day
  pill, then the frame-time pill, to keep the search field at least 110 px). Sidebar 170 px at scale 1 (or the 44 px
  icon rail) with group labels and, at the bottom, the collapse button and the version; the page in a scrolling child;
  the status bar under both. Default window 560 x 640 at scale 1 (min 400 x 300).
- `ui/widgets.{h,cpp}` (`ApexUi`): cards, rows and the other components, see "Components".
- `ui/icons.{h,cpp}` + `ui/lucide_data.h`: the icons, see "Icons".
- `ui/violet_theme.{h,cpp}` (`VioletTheme`): palette, the global style and the fonts. Apex owns its ImGui context, so
  `Overlay::Init` applies both once. Sizes are at 1080p; the overlay scales the style by resolution
  (`0.9 * pow(h / 1080, 0.8)`) and the user's text size and sets `style.FontScaleMain`. Widget geometry uses
  `ApexUi::Unit()` (= `FontScaleMain`): never write a pixel size without `* u`.
- Fonts: Segoe UI 15 px (+ Bold for titles). Missing files: ImGui's default font. No icon font.
- Feature code draws its own controls: `EdgeSmoothingPatch` / `DepthBlurPatch::RenderCustomUI` (card body) and
  `RenderDeveloperUI` (Developer page), `Picture::RenderUI(tab)` / `RenderDeveloperUI`, `Borderless::RenderUI`, and
  Night Lights through `patches/night_lighting.h` (one function per card).

## Pages
Sidebar: Overview, then three groups (small upper-case muted labels): WORLD, IMAGE, SYSTEM. Pages with tabs use the
underline `TabBar` under the page title; the selected page and each page's tab are statics (kept while the game runs,
not saved; `Go(page, &tab, n)` opens a page on a tab).

| Page (sidebar icon) | Content |
|---|---|
| Overview (layout-dashboard) | Sims3SettingsSetter recommendation card (only while S3SS is not loaded and `[ui] recommend_s3ss` is true; "Download", "Don't show again"). One card listing every feature as a row (icon, name = link to its page and tab, phrase, GPU cost chip, switch; Borderless shows its mode in a pill): Night Lights, Water Reflections ("Needs Night Lights" / "Needs Depth Blur" when one is off), Picture, Depth Blur, Borderless, Edge Smoothing, Faster File Lookups (gauge; `ResourceLookupCache`), Faster Room Lighting (gauge; `RoomLightQueue`, since 2026-09-29), Lot Lighting While Moving (gauge; `LotLightingMotion`); the three open the Performance page. |
| WORLD > Lighting (moon-star) | Tabs **Lamps** (Night Lights card: master switch `NightTerrainRelight` + "Lamp color" Pink ... Warm white with a colour track, swatch and "Reload save" badge), **Ground** (Ground & Lots: street lamps light lots, lot lamps light the street, smooth ground light, BRIGHTNESS), **Objects** (every option shown, groups LAMP LIGHT and DOORS, COUNTERS AND FENCES; "Light stairs, railings, columns" has the "Reload save" badge; the DOORS group needs two Ground options: a note naming the missing one(s) and a primary "Turn it on" / "Turn both on"), **Buildings** (Buildings card: groups WALLS and ROOFS; Rooms at Night card (moon, since 2026-09-29): "Darker unlit rooms" + Light left, Blue tint, Soft light on furniture), **Stories** (since 2026-09-29, the user asked for everything about stories in its own tab; Stories card (layers): "Upper floors light the ground" = the separate `SplitLevelGroundLight` feature, shown on and disabled with "Already handled by Sims3SettingsSetter ..." when S3SS's own fix is on; "Outdoor light between floors"; "Seamless walls between floors" and "Indoor light between floors" (Experimental), both disabled with a "Needs ..." note while "Outdoor light between floors" is off). While Night Lights is off, the other tabs show a note and a primary "Turn on Night Lights" button. |
| WORLD > Water & Snow (waves-horizontal) | Tabs **Water** (Lamp Glow card while Night Lights is on, else "Lamp glow on ponds needs Night Lights" + "Turn on Night Lights"; Water Reflections card = `reflexoNoLago`, with "Needs Night Lights (Lighting page)" / "Needs Depth Blur (Depth Blur page)" and primary "Turn on ..." buttons) and **Snow** (walked-on sidewalks; needs Night Lights; needs "Street lamps light lots": note + "Turn it on"). |
| IMAGE > Color (palette) | Tabs Basic, Tones, Color, Detail (Picture) and Banding (30/09). Banding: the Banding Fix card (blend icon; on by default; Strength 0-100%, Moving grain; Smooth gradients = Picture's deband, which follows the Banding Fix switch and runs with Picture off too; a note; Developer > Debug views: coverage counters and "Show covered surfaces"; features/banding-fix.md). Picture tabs: the Picture card header above the rows (switch = `[qol.picture] enabled`; GPU cost chip; hold to compare (eye) and before / after (columns-2) buttons, disabled while Picture is off, never saved). Tabs **Basic** (brightness, contrast, saturation, temperature, sharpness, smooth gradients), **Tones** (midtones, shadows, highlights, blacks), **Color** (tint, vibrance; FILM TONES = split toning, hue sliders on a hue-circle track with a swatch; COLOR MIXER), **Detail** (clarity, vignette, vignette size). Rows stay visible, greyed out, while Picture is off. |
| IMAGE > Ambient Occlusion (contrast) | New configurations start with the scene AO enabled at Strength 168%, Distance 351 m, High quality, Reach 130% and Keep lamp light 38%; Also in map view is on. Note: turn off the game's own Edge Smoothing; performance note (gauge) "Heavier on the graphics card than other effects: lower the Quality if the game slows down". Advanced includes "Show the shade alone" (not saved); a separate **Sim Occlusion** card below uses the Lucide User Round icon and places its Experimental badge beside its switch. It is off by default. When enabled, Sim intensity and Maximum darkening start at 47%, Hair intensity at 38%, and Transparent hair is on; Advanced also contains Show Sim coverage (not saved). Turning the Sim switch off hides these controls. No page or card reset buttons. Developer > Debug views: status, GPU cost, camera read-out, "Show the shade alone". See features/ambient-occlusion.md. |
| IMAGE > Depth Blur (aperture) | Note: turn off the game's own Edge Smoothing. Depth Blur card (GPU cost chip): Focus Auto / Fixed (segmented), Blur amount (%); Auto: Sharp area Small / Medium / Large; Fixed: Distance Near / Medium / Far + "Fine-tune distance" (0-100% of 0..0.5) + Transition; Sharp in map view; Advanced (rare knobs): Strength, Quality, Focus speed (Auto only, "0.3 s"), Blur the sky, Glowing lights. Mode-specific rows are drawn (and searchable) only in their mode. |
| SYSTEM > Edge Smoothing | A single page with SMAA/FXAA controls and the existing game-MSAA compatibility notice. Window/monitor selection, Apex FPS/V-Sync and VRR status are removed. Legacy profile display sections are ignored without moving category bits. |
| SYSTEM > Performance (gauge; added 2026-09-29) | One card "Performance" ("Fewer stutters while you play"; `PerformanceCard`, no header switch; [features/performance.md](features/performance.md)): switch rows, each drawn by `FeatureSwitchRow` (the feature description on hover of the row, "Not available" / "Starting…" / error notes under it), in this order: "Faster game file lookups" ("Fewer small stutters when objects and textures load"; `ResourceLookupCache`, default off; while it is on, the row "Remember missing files" ("Skips repeated searches for files no package has"; `ResourceLookupMisses`, default off) under it), "Faster file lists" ("Fewer stutters when Sims load outfits and shapes"; `FileListCache`, default off), "Spread lot lighting while moving" ("Lots relight in small steps while the camera moves"; `LotLightingMotion`, default on; while it is on, the slider "Lot lighting time while moving" (1-15, value "3 ms", end labels "Smoother" / "Lights sooner", default 3; `Performance::SetLotLightingBudgetMs`, saved as `budgetWhileMovingMs`)), "Wall shading waits while moving" ("Walls of new lots get their shading when you stop"; `WallShadingWhileMoving`, default on), "Faster texture compression" ("Fewer hitches when the game builds terrain, Sim and lot textures"; `FastTextureCompression`, default off; while it is on, the row "Use several cores" ("Large textures are shared out over several processor cores, with the same result"; `useSeveralCores`, default on) under it), "Faster cache compression" ("Fewer hitches when the game stores Sims and objects in its caches"; `FastCacheCompression`, default off), "Spread new objects over frames" ("Fewer hitches when a lot streams in while the camera moves"; `SceneNodeBudget`, experimental, default off) and "Faster object lookups" ("Fewer hitches when lot lights update; less script work"; `ObjectLookupIndex`, experimental, default off). Only the lot lighting row has a slider; the other tuning is in the Developer card. |
| SYSTEM > Report a problem (bug; unified build) | Restored v2.5.3: Capture session hero, always-visible recording/report/point/snapshot tools, dated capture list with Open and two-click confirmed Delete, and sending instructions. No stages, capture/library tabs or compulsory notes. Busy guards and conditional failed-save retry remain; older redesign notes below are historical. |
| SYSTEM > Developer (wrench; development build only) | ImGui tabs: Lighting (Night Lights status, census (list-checks), diagnostics (stethoscope), light probe (crosshair), counters, the generic list of every option; Every-Story Ground Light state), Profiler (activity; Frame Profiler, the Apex shaders line, then the "Performance" dev card: resource lookup cache counters (with the "Remember missing files" and file list cache lines), "Check 1 answer in N against the game", "Check every answer for 10 s", lot lighting call / camera / budget lines, the wall shading gate lines with the slider "Longest wait of a pass while moving (ms)" (0-10000, default 2000), the texture / cache compression lines, the scene node budget lines with the sliders "Nodes per frame while moving" (8-4096, default 512), "ms per frame while moving" (0.1-10, default 2.0) and "Longest wait (ms)" (16-5000, default 500), and the object lookup index lines with "Check 1 answer in N against the game" (default 64) and "Check every answer for 10 s"; none of these is saved), Capture (camera; Frame Capture), Debug views (bug; Edge Smoothing status / GPU cost / red pixels, Depth Blur status / focus mode and depth / GPU cost / "Show blur amount" / far plane, Picture's 8-bit note and GPU cost). |
| SYSTEM > Settings (settings) | Tabs **Menu** (control rows: menu key + Change, text size - 100% + Reset steps, "Show the welcome tour again" + Show, Save now), **Shortcuts** (menu/action key rows, optional filtered screenshot shortcut and temporary game-UI hiding), **Profiles** (see "Profiles"), **Compatibility** (rows Game, Sims3SettingsSetter Installed (circle-check) / Not installed, Features; the recommendation + Download while S3SS is missing; "Details" = the raw S3SS summary and settings migration note), **About** (name, version and build in the header; CREDITS). |

Every card is `PushID(<name>)` + `BeginCard("##Card")` ... `EndCard()` + `PopID()`. Every setting is a row drawn by
`SwitchRow` / `Slider` / `SliderPercent` / `SegmentedRow` whose label is its stable id (unique within its card, `##`
suffix when two labels read the same, e.g. `Brightness##Walls`); keep it that way: search, changed markers and undo
reports all hang off these rows. Each page tab's content is its own function in `apex_gui.cpp` (`LampsTabContent`,
`GroundTabContent`, ..., `PictureHeaderCard`, `PictureRows(tab)`, `DepthBlurContent`, `BorderlessCard`,
`AntiAliasingContent`, `PerformanceCard`, `MenuTab`) so the page and the search results draw the same code.

## Design tokens
All sizes are 1080p pixels at text size 1, times `ApexUi::Unit()`.

**Palette** (`violet_theme.h`): accent #7F77DD, accent dark #534AB7, accent light #CECBF6, window #15161A, card #1C1D22,
border / dividers #2A2B31, selected #24252B, hover #1F2025, switch off #3A3B42, text #E8E8EC, muted #8B8C96, warning
(amber) #E0A84F, error (soft red) #E8716B, success (muted green, status bar only) #7DBE9A. Disabled = style alpha 0.5
(`DisabledAlpha`) on the whole row. Keyboard focus ring = ImGui's nav cursor in accent light (#CECBF6), 2 px, rounded
like the frame, on every nav-focusable widget (all custom widgets use `InvisibleButton` with
`ImGuiButtonFlags_EnableNav`, so ImGui draws it).

**Chips** (`Chip`, 0.8x text, 6 px side padding, 14% fill of their colour): muted for GPU cost ("~0.4 ms"), amber for
"Reload save". **Changed dot**: 3 px radius, accent, 6 px after the label (after the badge when there is one).

**Spacing scale** (`kSpace1..4` = 4 / 8 / 12 / 16):
- window padding 12; content area padding 8 x 4; sidebar hairline, then 12 to the page;
- card padding 16 x 12, card rounding 10, 12 between cards (`EndCard`), 12 under the page title / tab bar;
- `CardDivider()` between a card's header and its body: 12 above the hairline, 12 below; only when a body follows;
- rows: consecutive rows are `kRowGap` (16) apart, label to label, with a hairline in the middle; anything that is not a
  row (note, button, "Advanced", group label) keeps 8 from a row above it (12 for a group label);
- label to description 2; description / label to a slider track or segmented control 5 (item spacing).

**Typography**: page title bold 1.3x, page subtitle muted 1x; card title bold 1.05x, card subtitle muted 1x; row
label 1x `kText`; description 0.87x muted; slider value 1x muted, right-aligned on the label's line; end labels, pills
and the sidebar version 0.87x muted; group labels ("WALLS", sidebar "WORLD") bold 0.8x muted, upper case, letter-spaced.

**Icons**: on card headers (18, accent), the sidebar (18), overview rows (18), notes (14), buttons (14), pills (14).
Setting rows have no icons (all rows in a card look the same).

## Components (`ApexUi`, ui/widgets.h)
- `PageTitle(title, subtitle)`; `TabBar(id, &tab, labels, n, icons)`: underline tabs (selected = white text + 2 px
  accent underline, others muted, hairline under the bar, wraps when narrow).
- `BeginCard` / `EndCard` (always call EndCard), `CardHeader(icon, title, subtitle, tooltip, toggle, toggleEnabled,
  extra)` (`HeaderExtra`: an icon toggle left of the switch, used by before / after), `CardDivider()`.
- Rows (dividers between consecutive rows): `SwitchRow(label, v, description, default)` (switch right, centred on the
  text block); `Slider(label, v, min, max, SliderOptions)` (label + value on one line, description, track; options:
  format / scale / offset, fixed value text, end labels, colour swatch, `defaultValue`, a gradient track
  `trackFrom`/`trackTo` or the hue circle `hueTrack`) and `SliderPercent(label, v, min, max, description, default)`;
  `SegmentedRow(label, description, id, current, labels, count, tooltips, icons, defaultIndex)`;
  `BeginControlRow(label, description, controlsWidth)` (returns false when the search hides it: then draw nothing and
  skip `EndControlRow`) / `EndControlRow()` for custom controls on the right (menu key, text size, info rows, profiles);
  `OverviewRow(..., chip)`. The description is always visible (no hover tooltip on rows); `SliderCommitted()` =
  deactivated after edit of the last slider, or its Reset (Picture saves then). `SetNextRowBadge(text, tooltip)` puts a
  chip after the next row's label.
- Defaults are optional arguments (`BoolDefault` from a bool, `kNoDefault` / `kNoDefaultIndex` = none); pass them
  from the code's real defaults (registered setting defaults, `PictureParams{}`, the `Params{}` of Edge Smoothing and
  Depth Blur, `Borderless::Mode::Off`, `ApexPatch::IsEnabledByDefault()`), never guessed.
- `CardHeader(..., extra, chip)`: `HeaderExtra` also has a hold button (`holdIcon`, `holdTooltip`, `held` while
  pressed); `chip` = GPU cost. Header switches, overview switches and rows report their changes (`ReportChange`).
- `Segmented(id, current, labels, count, tooltips, compact, icons)`: segments share the width (a vertical list when they
  do not fit); selected = accent dark fill with accent light text; hover tooltips per segment; `current = -1` = none
  selected (a fine-tuned Depth Blur distance).
- `BeginAdvanced` / `EndAdvanced`: hairline, then an accent chevron + "Advanced" row (full-width click target),
  collapsed by default (state per id in the card's storage); contents are not indented. Use it only for rare knobs (a
  page or tab with room shows options directly). `AdvancedNode` (tree node) stays for the Frame Profiler.
- `GroupLabel("WALLS")`: groups inside a card.
- `IconNote(icon, text, rgb)`: icon + wrapped text in a tinted rounded box (10% of its colour): info = `Info`, muted;
  warning = `TriangleAlert`, `kWarning`; error = `TriangleAlert`, `kError`.
- Buttons, one frame high: `IconTextButton(label, icon, tooltip, ButtonKind)` and `TextButton(label, tooltip, kind,
  minWidth)`; `ButtonKind::Secondary` (neutral fill, border turns accent on hover: Reset, Save, Change, - / +) and
  `ButtonKind::Primary` (accent dark fill, accent on hover, white text: "Turn on ...", "Download"). `ButtonWidth` for
  control rows. "Don't show again" stays a text link.
- `IconButton`, `Pill`, `Chip`, `SidebarItem(icon, label, selected, collapsed)` (32 high; collapsed = icon only, label
  as tooltip), `SidebarGroup(text, collapsed)` (collapsed = a short hairline), `ToggleSwitch`, `Tooltip`, `MutedText`,
  `Gap`.
- Search filter: `BeginFilter(query)`, `SetFilterCrumb(crumb, id)`, `EndFilter(&clicked)`, `FilterActive()`; change
  reports: `ReportChange`, `SetChangeReporting`, `TakeChange`; drag fade: `SliderDragging`, `SetKeepActiveSliderOpaque`.

## Features of the menu (how they work, where their state lives)

State: everything below lives in `apex_gui.cpp` statics (render thread, inside the overlay's ImGui frame) unless a
`[ui]` key is named. Persisted `[ui]` keys go through `ApexConfig::GetUi` / `SetUi` (saved by the pump thread a second
later): `welcome_done` (default false; missing = false, so migrated configs see the tour once) and `sidebar_collapsed`
(default false). Feature state changes go through the features' own paths (`ApexPatch`, `NotifySettingChanged`,
`PatchManager` unsaved flag, `Picture::SetParams`, `Borderless::SetMode`), so autosave works as before.

### Search
Header field "Search settings" (search icon, "Ctrl+F" hint while empty; Esc or the x clears it). Ctrl+F focuses it,
only while the menu window has keyboard focus (then ImGui captures the keyboard, so the game never sees it). While the
query is not empty the content shows **Search**: one card with every matching row, live (the real controls). Matching:
every word of the query (case-insensitive, ASCII) must appear in the row's visible label or its description. How: the
page tabs are functions; `SearchResults` calls each one (`SearchParts`, in sidebar order: Lighting tabs, Water & Snow
tabs, Color header + Color tabs, Depth Blur, Display tabs, Performance, Settings > Menu and Shortcuts) between `ApexUi::BeginFilter` and
`EndFilter`. In filter mode the row widgets draw only when they match, each after a small muted breadcrumb link
("Lighting › Lamps"); card frames, dividers, notes, buttons, group labels, page titles, tab bars and "Advanced"
headers draw nothing (buttons inside a visible control row still draw); a card header with a switch becomes a
searchable switch row (so "night lights" or "depth blur" finds the master switches). Feature card bodies and Night
Lights tabs are searched even while the feature is off. Clicking a breadcrumb opens that page and tab and clears the
search; so does picking a sidebar page. Not searched: Overview (it repeats the master switches), Developer, Profiles,
Compatibility, About.

### Changed markers and per-setting Reset
A row with a default shows a violet dot after its label while its value differs from the default ("Changed from the
default" on hover); hovering the row shows a small Reset button (rotate-ccw) right after it, which puts that one setting
back through the row's normal change path (the widget sets the value and returns "changed", so Night Lights'
`ApplyLive`, Picture's save, etc. run as for a click). Sliders compare with a tolerance of 1/10000 of their range.
The small Reset buttons are not keyboard stops (they only exist on hover).

### Undo toast
At the start of each frame, when the left mouse button goes down inside the menu window or Space / Enter / keypad Enter
is pressed while it has focus (and no slider was active in the previous frame, so the key that ends a keyboard
adjustment does not count), the menu takes `ApexConfig::CaptureFeatureState` (every `[patches.*]` table, `[qol.picture]`,
`[display]`). Rows, card-header switches and overview switches report what changed (`"<Label> turned on/off"`,
`"<Label> changed"`, `"<Label> reset"`; sliders once, on release); Reset buttons and "Turn on" buttons report too. The
last report of a frame becomes the toast at the bottom right of the window, above the status bar: the text and an
"Undo" link (undo-2), about 4 s, fading in and out; the timer waits while the pointer is on it. Undo =
`ApexConfig::ApplyFeatureState(snapshot)`: only sections that differ are applied (feature settings and on / off through
`ApexPatch::ApplyTableLive`, Picture through `SetParams`, the window mode through `Borderless::SetMode`), then unsaved +
save. Only the last change is undoable. The Developer page does not report (`SetChangeReporting(false)`); menu
preferences (text size, menu key, sidebar) are not part of the undoable state.

### Looks (removed)
A "Looks" card (Classic / Balanced / Cinematic presets on Overview and in the tour) existed briefly and was removed on
2026-09-28 at the user's request: applying a look rewrote the player's tuned Night Lights, Picture and Depth Blur values
(only the last change is undoable), which destroyed a hand-tuned setup. Do not bring presets back without the user asking;
if ever, save the current setup as a profile first.

### Profiles (Settings > Profiles)
"SAVE CURRENT SETUP": "What to save" = a checkbox per part (Night Lights, Color, Depth Blur, Edge Smoothing,
Window mode, Performance, Shortcuts (off by default), Ambient Occlusion (bit 7, added 30/09 so older part masks keep their bits); the rest checked by default, kept while the game runs), then a name field (only letters, digits, space, - and _ can be typed; at most 32 characters; Enter
saves) and Save; an existing name asks "... already exists; replace it?" inline. "SAVED": one row per profile with Load
and Delete; the row's description lists the parts the file has. Load opens an inline pick of those parts (all
checked) with Load / Cancel, and applies only the checked ones (`KeepProfileParts`). Delete asks inline, "Delete this
profile?" with Delete / Cancel. "Open the Profiles folder" opens it in Explorer (to share profiles or copy them to
another PC). Profile names are user data: shown untranslated (`SetNextRowUntranslated`). Files:
`Documents\...\Apex Radiance\Profiles\<name>.toml`, written by `ApexConfig::SaveProfile` = `CaptureFeatureState(profile
features only)` + `[meta]`: the same tables the main config saves for Night Lights (`NightTerrainRelight`), Every-Story
Ground Light, Edge Smoothing, Depth Blur and the Performance page's features (`[patches.<Name>]` with `enabled`), Picture (`[qol.picture]`) and the
window mode (`[display]`); developer tools stay out. Load = `ReadProfile` + `ApplyFeatureState` (live, marks unsaved
changes, autosaves into ApexRadiance.toml) + the undo toast "Profile loaded". Names are sanitised
(`SanitizeProfileName`: allowed characters only, no leading / double / trailing spaces, Windows device names refused);
files whose names do not survive it are not listed. The list is read when the tab opens and after each action.

### Peek and the drag fade
Holding Alt while the pointer is over the menu makes it nearly transparent (window 0.2, contents 0.1 through the
disabled alpha) and inert (`BeginDisabled`), so the game shows through; releasing restores it (eased over about 0.1 s).
Not while typing or dragging. While a slider is dragged (or adjusted with the keyboard) the window fades to 0.35 and the
active slider's row stays fully opaque (`SetKeepActiveSliderOpaque`). The status bar says "Hold Alt to peek". Keys:
while the menu has keyboard focus ImGui captures the keyboard (the existing policy), so Alt never reaches the game;
while the pointer is only hovering the menu, `GuiClient::CaptureKey` claims Alt and B (not while typing): the overlay
eats their key-down, key-up and character after ImGui saw them (`framework/overlay.cpp`; cleared on focus loss). Alt+Tab
is handled by Windows before any window sees it, so it keeps working.

### Hold to compare (Color page)
The eye button in the Picture header (left of before / after), or holding B while the pointer is over the menu (not
while typing), calls `Picture::HoldBypass()` every frame: the Picture pass is skipped for 0.15 s after the last call
(`m_holdUntil`, never saved), so the game shows its original picture and it comes back by itself on release.

### GPU cost chips
The Overview rows and card headers of Picture, Edge Smoothing and Depth Blur show "~0.4 ms" (tooltip "Measured cost on
your GPU per frame") from the features' own timestamp queries: `Picture::GpuMs()` and `ApexPatch::GpuCostMs()`
(Edge Smoothing and Depth Blur override it with their measured pass time). Hidden while the feature is off or not
measured yet.

### "Reload save" badges
An amber chip after the label of rows whose change shows only after a save or world loads again, tooltip "This change
shows after you load a save again": Lamp color (lamps are tinted when their colour is written, i.e. when a save loads)
and "Light stairs, railings, columns" (those pieces get their light when a world loads). Their descriptions no longer say
it, and the old footer "Some changes show after you reload your save" is gone. Every other Night Lights option applies
live (docs/features/night-lighting/README.md, "Applied" column).

### Inline dependencies
A row or card whose effect needs another feature or option shows an info note naming it (and where it is) plus a primary
button that turns it on directly (and reports the change, so it can be undone): Night Lights tabs ("Turn on Night
Lights"), Lamp Glow ("Turn on Night Lights"), Water Reflections ("Turn on Night Lights" / "Turn on Depth Blur"), Objects >
DOORS, COUNTERS AND FENCES ("Turn it on" / "Turn both on" for "Street lamps light lots" and "Smooth ground light"), Snow
("Turn it on" for "Street lamps light lots"). The game's own Edge Smoothing (Depth Blur, Edge Smoothing) is a game
option, so it stays a plain note.

### Welcome tour and the first-launch hint
The first time the menu opens in a session while `[ui] welcome_done` is false, the content area shows the tour (a card
with step dots; the sidebar and the search field are disabled meanwhile): 1) "Sims3SettingsSetter" ("Installed; you're all set", or the recommendation + Download), 2) "Your menu key" (the key +
Change). Buttons: Skip (link, step 1), Back, Next, Done; Skip and Done set `welcome_done = true`. Settings > Menu >
"Show the welcome tour again". The menu is never opened automatically: instead, at every launch (30/09: it used to be only
while the tour was never done), 3 s after Night Lighting reports the world live (changed 01/10 to avoid the first load), a
small non-blocking note (no input, no focus) shows in the top-left corner: the user's pick "A · compact pill" (logo,
**Apex Radiance is ready** in bold, a dot, "press" and the menu key in light violet; dark pill #15161a at 92%, border
#CECBF6 at 18%). It stays 8 s of time on screen (each frame counts at most 100 ms, so a loading stall does not use it up)
or until the menu is opened, and fades out over the last 0.8 s; `Client::AlwaysDraw` keeps ImGui frames going meanwhile.
Not shown while the first-start key choice is pending.
The ready delay resets if the world stops being live before it expires. With Night Lighting disabled, its world-live
signal is unavailable and the previous 2 s startup-time fallback remains. This changes display timing, not feature
installation or shader/font initialisation, and does not establish that the note caused the reported startup hitch.

**Shortcuts (03/10).** Every Apex action shortcut is intercepted by the overlay before the game sees it. Preset ids and
serialized values remain stable (`letters`, `numbers`, `fkeys`, `mine`); the visible names are **Letter row**,
**Number row**, **Function row**, and **Custom**. A missing `[ui] hotkey_preset` still means the earlier Function keys layout. Selecting a
preset is an explicit user action; opening Settings never replaces a saved key or applies a preset.

| | Letter row | Number row | Function row |
|---|---|---|---|
| Menu (`toggle_key`) | Ctrl+Shift+R | Ctrl+Shift+1 | Ctrl+Shift+F11 |
| Compare with the game | Ctrl+Shift+T | Ctrl+Shift+2 | Ctrl+Shift+F10 |
| Refresh the lighting | Ctrl+Shift+G | Ctrl+Shift+3 | Ctrl+Shift+F9 |
| Dev: Light Probe / Light Diag / recorder / Frame Capture | V / B / X / F | 4 / 5 / 6 / 7 | F7 / F8 / F6 / F5 |

Settings > Shortcuts presents the selected layout as a responsive QWERTY keyboard map and three selectable preset
tiles. Hovered assigned keys show their action and full modifier combination; clicking one records a replacement. The **Core actions**, **Report a
problem**, **Developer tools**, **While Apex is open**, and **Screenshot capture** groups also show clickable key chips;
menu navigation, hold-to-peek, hold-to-compare, diagnostics, and screenshot actions can all be changed there. Recording
waits until held keys are released, Esc cancels, and a note explains collisions and reserved game/Windows shortcuts.
The **Custom** state keeps the selected preset as the fallback for any action the player has not changed. Choosing a
preset explicitly clears per-action overrides; merely loading Settings never does. Narrow windows stack the map, actions
and supporting cards. Search keeps the standard shortcut rows and preserves the screenshot controls' enabled-state
behavior.

Letter row groups nearby left-hand keys; Number row is easy to recall; Function row preserves the earlier layout. Compare turns Night
Lighting, Depth Blur, Edge Smoothing and Picture off and back (not saved; a note shows at the top while off). Refresh
does what the Developer buttons "Rebuild terrain light now" and "Relight lots now" do plus every room and the object
rigs (`NightLighting::RefreshAll`). Per-action choices are stored in `[ui]`: existing `compare_key` and `refresh_key`,
plus `probe_key`, `diagnostics_key`, `recorder_key`, and `frame_capture_key`. Missing new fields mean “use the selected
preset”, preserving older config files. The same fields are included in the optional Shortcuts section of saved
profiles. Settings > Shortcuts has **Use Apex screenshot shortcut**, enabled by default on C only for new or missing key
settings. Existing saved screenshot keys remain unchanged. Apex consumes C and saves one filtered PNG to the game's
standard Documents `Electronic Arts\The Sims 3\Screenshots` folder; it does not also invoke the native screenshot.
Bare F10 remains the game's interface toggle; Apex uses it internally only while hiding the interface for a shot. It
reads the finished back buffer after Apex's scene and Picture passes. Ctrl+Shift+F10 remains Compare. **Hide game UI**
is on by default and temporarily toggles F10, captures one frame and restores the prior tracked state. Apex's own
overlay is suppressed for that frame. This is separate from Report's diagnostic screenshots.
Search, peek, and Picture compare bindings are also saved under `[ui]` and included in the optional Shortcuts profile
section; loading an older config or profile that lacks them keeps their defaults.

While `[ui] key_chosen` is false (missing = false: every existing config sees it once), a centered note shows the menu
key and offers **Customize**. The compact editor uses the same preset names, defaulting to Function row for configs
without a saved selection; its core action chips can be changed individually. Finishing closes the note and sets
`key_chosen`; pressing the current menu key also closes it while keeping that key. Settings > Menu: "Shortcuts" (the preset) and the keys of the quick
actions, then "Menu key" (Change: any key).

### Status bar
A thin footer under the sidebar and page (hairline, then one line of small text): left "All changes saved" (circle-check,
muted green) or "Saving…" (save icon) while `ApexConfig::SavePending()` (a requested save or unsaved feature changes);
middle "Sims3SettingsSetter detected" (violet check) / "Sims3SettingsSetter not installed" (dropped when narrow); right
"Hold Alt to peek". It replaces the header's unsaved dot.

### Collapsible sidebar
The chevrons button at the bottom of the sidebar collapses it to a 44 px icon rail (items show their name as a tooltip,
group labels become short hairlines, the version hides) and expands it again; saved in `[ui] sidebar_collapsed`.

### Colour tracks
Lamp color: a gradient track from the game's pink to warm white and a swatch of the current colour, both computed from
the real tint math (`LampColourAt` in `night_terrain_relight_patch.cpp`, the same blend as
`ObjectLightBridge::TintStockColour`: stock pink (1, 0.75, 0.79) to warm white (1, 0.80, 0.62) scaled to the pink's
luminance; shown as they are, an approximation of the on-screen colour). Picture's Shadow color / Highlight color: a
hue-circle track and a swatch of the hue.

### Keyboard
`ImGuiConfigFlags_NavEnableKeyboard` is on (overlay). Arrows / Tab move between widgets; Space / Enter toggles switches
and presses buttons; a slider is adjusted with the arrows after Space / Enter. Esc (when the menu has focus, no field is
being edited and the menu key is not being chosen): clears the search, else closes the
menu. Ctrl+F: the search field.

## Languages

English, Portuguese (Brazil), Spanish and French (Settings > Menu > Language; "Automatic" follows Windows' display
language; saved as `[ui] language = "auto" / "en" / "pt" / "es" / "fr"`). `ui/i18n.h`: the English text is the key,
translations are in `i18n/tr_widgets.cpp` (texts the widgets write themselves), `tr_menu.cpp` (apex_gui.cpp),
`tr_image.cpp` (Color, Edge Smoothing, Depth Blur, Borderless), `tr_lighting.cpp` (Night Lights, Water & Snow) and
`tr_features.cpp` (performance features, framework notices). Every widget translates what it draws; ImGui IDs stay the
English label, so switching languages keeps the menu's state. The search matches both the English and the shown text.
A text without a translation shows in English; the development build lists them in Developer > Language (also
translations whose `{}` placeholders differ from the English). How to add texts: `i18n/TRANSLATING.md`. Logs and the
Developer page stay English.

## Copy guidelines
- American English, friendly and plain, for players: say what the player will **see**, not how it works. No jargon
  outside the Developer page (no "per-pixel", "lightmap", "shader", "depth", "bridge", "rig", "story").
- Vocabulary: **lamps** (not lights / lamp light mixed), **lots**, **street**, **ground**, **brightness** (for
  strengths, shown in %), **upper floors** (not stories). "Night Lights" is the feature name everywhere; "Lighting" is
  only its page.
- Sentence case for labels, tabs and buttons ("Street lamps light lots"); Title Case only for page and card titles
  ("Ground & Lots", "Lamp Glow").
- Labels: at most ~32 characters, no final period, positive (a switch says what turning it on does: "Sharp in map
  view", "Outdoor light between floors", not "Off in ..." / "... aren't cut").
- Descriptions, notes, button and segment tooltips: one short sentence, ideally at most 60 characters (never more than
  ~90), **no final period**; join two ideas with a semicolon ("How bright lit roofs get; 60% is the default").
- Defaults are written "100% is the default" / "0% is off" / "100% is unchanged".
- Values: % or named steps (Near / Medium / Far, Low ... Ultra) instead of raw numbers; signed amounts as "+25".
- Notes that point elsewhere name the place: "(Ground tab)", "(Lighting page)", "(Depth Blur page)", "(Options ›
  Graphics)".
- Prose blocks keep full sentences with periods: feature hover descriptions (they end with "Part of Apex Radiance.
  Credits: @loinyx"), the Sims3SettingsSetter recommendation, the startup banners and the credits.

## Icons (ui/icons.h)
Lucide icons (ISC License, Copyright (c) Lucide Icons and Contributors; the license text is
`third_party/lucide/LICENSE`, the source SVGs are `third_party/lucide/icons/*.svg`). Credited in Settings > About >
Credits ("Lucide icons (ISC)").
- `ui/lucide_data.h`: each used icon's SVG elements copied as data (path `d` strings, circle cx/cy/r, rect
  x/y/width/height/rx, line x1/y1/x2/y2, and `filled` for `fill="currentColor"` dots). No build step: to add an icon,
  copy its elements from the .svg by hand, append it to `kIcons` and to `ApexUi::IconId` in the same order (a
  static_assert checks the count). Ten icons at the end of the list (search, undo-2, chevron-left, chevrons-left,
  chevrons-right, check, gauge, eye, bookmark, trash-2) are not in `third_party/lucide/icons/`: their elements were
  entered by hand from Lucide's published icons; if one looks off, download its .svg and replace the data.
- `ui/icons.cpp`: on first use an icon's paths are parsed (M L H V C S Q T A Z, absolute and relative; arcs by the SVG
  endpoint-to-centre conversion) and flattened to polylines in the 24x24 viewBox, then cached. Drawing scales them:
  `ImDrawList::AddPolyline` with thickness 2/24 of the size (Lucide's stroke-width 2, at least 1 px), closed for Z; round
  caps and joins as small filled circles at open ends and at corners sharper than ~25 degrees; circles with `AddCircle`,
  rects with `AddRect` and their corner radius; filled dots with `AddCircleFilled`. No texture, no device dependency,
  render thread only.
- API: `DrawIcon(dl, id, pos, size, color)`, `Icon(id, pos, size, color)` (current window), `InlineIcon(id, size, color)`
  (reserves the square with a Dummy), `IconLabel(id, label, color)`. Sizes: `kIconSmall` 14 and `kIconMedium` 18, times
  `Unit()`.

## Rules kept from the previous menu
- Feature switches are inert while features start ("Starting…") or on an unsupported game ("Not available on ...");
  `GetLastError()` shows as an error note; a feature's own controls appear only while it is on (Picture's stay visible,
  greyed; the search results show them either way).
- The description (ending with "Credits: @loinyx") is the hover tooltip of the card title, the card switch and the
  overview name; no visible credit lines besides the Credits section.
- Developer items only under `if constexpr (!kPublicBuild)` (the Developer page is not in the public sidebar).

## Startup banners (drawn even while the menu is closed)
- Old combined build loaded (`Startup::RefusedOldBuild`): "Apex Radiance is off" (error colour), the module name, and
  "Delete that file from Game\Bin, keep the official Sims3SettingsSetter.asi, then restart the game." Features stay off.
- Older standalone `S3SSApex.asi` loaded too (`ApexGui::SetOldStandaloneNotice`): "An older S3SSApex.asi is also
  installed. Delete it from Game\Bin." (warning colour). Features keep running (the old copy idles). When the old copy
  loaded first, Apex Radiance itself idles (no menu, no banner) and only writes an error line to `ApexRadiance_LOG.txt`.

## Credits (Settings > About)
sims3fiend (Sims3SettingsSetter, the model for the rewritten framework), FXAA 3.11 (Timothy Lottes), third-party code
(Dear ImGui, Microsoft Detours, toml++, SMAA, Lucide icons), the single line "Every-Story Ground Light uses a technique
first shared by Arro.", and "Apex Radiance by @loinyx". Do not mention Arro or the Split-Level fix anywhere else
(feature descriptions, tooltips, release notes, promo text); functional notices about official S3SS's own fix being on
("Already handled by Sims3SettingsSetter (its Split-Level Lighting Fix is on)") stay.
- Brand note: removed 30/09 (the faint name in the bottom-right corner was drawn with the terrain, often still under the load screen, and the user did not see it); the "is ready" note above now shows at every start instead.
- Recommendations (30/09): DXVK (detected by the loaded d3d9.dll: not in the Windows folder and "dxvk" inside the file)
  and official Sims3SettingsSetter. Only the missing ones are listed, each with Download (GitHub releases): at every start
  a note in the top-left corner (before the shortcuts note; "Not now" = this session, "Don't show again" = `[ui]
  recommend_s3ss = false`, now for both), the Overview card "Recommended for Apex Radiance", and Settings > Compatibility
  (DXVK and Sims3SettingsSetter rows with Installed / Not installed, the RECOMMENDED group while one is missing).

## Performance grouping (2.5.3)
Four Violet cards keep main controls visible: Camera and lighting (room queue, moving lot budget, wall shading, scene setup); Files and objects (resource cache, missing resources, file lists, object index); Textures and Sims (DXT, several cores, cache compression, Sim sorting); Memory handling. Existing feature descriptions remain on hover; no setting keys or defaults changed. Experimental badges removed at the user's request.

## Guided development diagnosis (development build, pending release)
The Developer page starts with a Violet guide card: select rooms/floors, camera stutters,
dark objects, visual effects or missing translations, then follow Prepare / Reproduce /
Save evidence. Selection opens the relevant existing tool tab without enabling diagnostics
or modifying settings. Open relevant tools returns to that tab at any time.
Capture-session controls stay visible in the guide: Begin, Save report and End, plus Open
captures folder. Reports reuse the existing capture APIs and current session folder.
Profiler measurements retain their separate report control; a capture report does not
export profiler results. Lighting, Profiler, Capture, Debug views and Language retain all
existing tools. Debug-view guidance asks for one view at a time; it does not enforce mutual
exclusion. Guide selection is session-only presentation state and is not saved to TOML.
The public build still hides Developer. No game hooks or lighting policies change.
Profiler performance counters are grouped as Files and objects, Camera and lighting,
and Textures and compression; all original controls are retained. The Language tab
uses a Translation checks card. Lighting and Debug views include visible test guidance.

### Defaults available to every user
The shared public/development menu has a reset entry point on every page. Feature pages reset their entire page, including its internal tabs, after confirmation. Water & Snow resets only its four registered water/snow settings and preserves lighting and upper-floor settings. Display resets edge smoothing and the Apex-managed window mode; externally managed window modes remain under their owner's control. Settings resets menu preferences and shortcuts while preserving the Report diagnostic screenshot preference. Report resets its screenshot preference without deleting any files. Overview and Developer link to the whole-mod reset in Settings > Menu because their contents span features and runtime diagnostics.
The whole-mod reset restores registered feature defaults, Picture, Apex window mode and UI preferences/shortcuts. Welcome/key setup completion flags are retained to avoid repeating onboarding. Undo restores feature/window state and UI preferences. Captures, reports and saved profiles are never deleted. These controls are translated into English, Portuguese, Spanish and French. Runtime-only developer diagnostics are outside the persisted feature reset scope.

### Guided Report page (published 2.5.4)
Report a problem uses a primary choice/capture/finish card, with all original tools and saved captures in advanced sections. Object capture uses a temporary Violet target at the mouse, the configured key and Esc cancellation. It measures a pixel rather than identifying an entire object. The shared public/private UI preserves existing capture APIs and filenames; see features/bug-reports.md.

### Consolidated private development workspace
The private Developer page now uses horizontal Start here, Lighting, Performance, Captures, Visual effects and Translations tabs. Each task has its own explanatory card and retains the existing diagnostic renderers. There is no second sidebar. This replaces the earlier guided tab arrangement described above. Developer stays excluded from the public build. Technical diagnostic output retains engine terminology.

### 2.5.4 public interface scope

The public hotfix includes the shared page/whole-mod default controls, Report capture controls and one-click object-point selection with centered notices. Developer tabs, profiler tools and private diagnostic controls remain excluded by `kPublicBuild`. The simpler Capture / Saved files layout is implemented only in the local `2.5.4-test-report-library` working tree; it is not in the published 2.5.4 release. The four-language translation checks passed; broader gameplay capture/cancel, loading-gate, performance and display-scale testing remains necessary.

### Local overlay availability and clock (`2.5.4-test-report-library`)

Panel opening, shortcuts and notices wait for a loaded active session plus three seconds of readiness; Night Lights uses its world-live signal, with a documented read-only WorldManager fallback when lighting is off. No timer based only on application startup unlocks the menu. The window procedure reads cached availability. Overlay delta time is sampled on every game frame, so a closed-menu gap does not contaminate the FPS average. Genuine low FPS remains visible. A throttled slow-panel phase trace is logged for gameplay diagnosis. The user later reported smooth operation again; the earlier system-wide slowdown's cause remains unconfirmed. See the Report feature doc for tests and fallback limits.

### Compact footer and credits (local test)
The footer shows only configuration save status and Hold Alt to peek. Sims3SettingsSetter detection stays in Settings > Compatibility rather than every page. About credits start with Apex Radiance by @loinyx; the former promotional paragraph is replaced by a short sims3fiend framework-design attribution in all four languages. Historical attribution and third-party licenses remain in the project documentation. Compatibility detection, conflict protection, and integration behavior are unchanged.

### Centered notice width and icons (local test)
Capture, recording, comparison and entry pills have a content-measured width set before ImGui Begin; only height auto-resizes. Wrapped text plus AlwaysAutoResize previously converged to one glyph of width, creating a vertical strip. Width is clamped to the viewport's shared notice margins. Capture uses Info, aiming Crosshair, recording a pulsing Activity icon and comparison Columns2; entry retains the logo/Sparkles fallback. Recommendation/key setup headers use Info/Keyboard. Existing top-center placement, lifetimes and input pass-through remain.

## Unified build and optional developer mode (2026-10-02)

Published since 2.5.5: one ASI contains the player features and optional developer tools. Enable developer mode in Settings > Menu after confirmation, then restart the game. Default off. Profiles optionally include Development; the save option is hidden in normal mode and appears when importing a profile containing it. Profiles never start measurements or recordings automatically. See [developer-mode.md](features/developer-mode.md). FXAA is first and recommended; SMAA is spatial only. There is no Window page in the current release.

## Historical RC synchronization controls (removed before 2.5.5)

Display > Window retains Borderless and adds experimental V-Sync policy, optional Apex FPS limiting and a dependent target slider. They share the Window profile category and page reset. Restore synchronization resets only these three values. S3SS FPS conflict blocks pacing and supplies an inline explanation. Driver VRR activation and DXVK overrides are explained inline; no automatic claim of VRR support or flicker elimination. See [features/presentation.md](features/presentation.md).

## Local Sim occlusion UI revision

The Ambient Occlusion page has two cards: scene AO first (Strength, Distance, Quality), then Sim Occlusion (body/hair intensity, maximum darkening and advanced coverage preview). The second depends on scene AO, is experimental and off by default; its switch has a dedicated User Round icon and an Experimental badge, and its controls are hidden while the switch is off. Page and card restore-default buttons, including Overview, Picture/mixer, Depth Blur and Edge Smoothing, are removed. Individual row defaults and the explicit global Settings reset remain. See [features/ambient-occlusion.md](features/ambient-occlusion.md).
