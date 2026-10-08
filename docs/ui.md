# Menu reference (Violet UI)

Apex Radiance has one in-game menu: a dark Violet window with a grouped sidebar on the left, one page at a time on the
right, a search field in the header and a save-status footer. Every feature is a card; main controls are visible and
rare knobs sit in a collapsed **Advanced** section. The menu opens with Ctrl+Shift+F11 by default (other presets
below), only once a world is loaded. It is translated into 21 languages, chosen automatically from the Windows language (see Languages). This page
is the reference for its pages, widgets, shortcuts, profiles and notices; dated design decisions are in
[history/ui.md](history/ui.md) and test evidence in [validation/ui.md](validation/ui.md).

| | |
|---|---|
| Window | `###ApexWindow`, default 560 x 640 units (minimum 400 x 300); layout saved in `Documents\Electronic Arts\The Sims 3\Apex Radiance\apex_radiance_imgui.ini` |
| Settings | `[ui]` table of `ApexRadiance.toml` (keys below) |
| Source | [`apex_gui.cpp`](../apex_gui.cpp) (window, pages, search, profiles, notices), [`ui/widgets.h`](../ui/widgets.h) (`ApexUi`), [`ui/violet_theme.h`](../ui/violet_theme.h), [`ui/icons.h`](../ui/icons.h), [`ui/i18n.h`](../ui/i18n.h), [`hotkeys.cpp`](../hotkeys.cpp), [`framework/overlay.cpp`](../framework/overlay.cpp) |

Visible names come from `apex_version.h` (`APEX_PRODUCT_NAME` = "Apex Radiance", `APEX_PRODUCT_TAGLINE` = "for The
Sims 3"); internal names keep "Apex". Visible text uses `APEX_PRODUCT_NAME`, never "S3SS Apex" or "Apex Edition".

## Layout

- **Header:** the logo (`ui/logo.h`: `ui/apex_logo.png` as a 128x128 texture with mips from `ui/logo_data.h`; a violet
  tile with `APEX_LOGO_LETTER` if the texture cannot be made), name and tagline, the search field (110 to 220 units
  wide), a Night/Day pill (moon or sun), a frame-time pill (`ms · fps`) and Close (tooltip "Close (Esc); <key> opens it
  again"). Narrow windows drop the Night/Day pill, then the frame-time pill, to keep the search field at least 110
  units wide.
- **Sidebar:** 170 units wide, or a 44-unit icon rail when collapsed (`[ui] sidebar_collapsed`); group labels; the
  collapse button and the version at the bottom. Collapsed items show their name as a tooltip and group labels become
  short hairlines.
- **Page:** a scrolling child with the page title, an optional underline tab bar and cards. The selected page and each
  page's tab live while the game runs and are not saved (`Go(page, &tab, n)` opens a page on a tab).
- **Footer:** "All changes saved" (CircleCheck, muted green) or "Saving…" (Save icon) while `ApexConfig::SavePending()`,
  and "Hold Alt to peek" on the right when there is room. Sims3SettingsSetter detection lives in Settings >
  Compatibility.

## Pages

| Page (sidebar icon) | Content |
|---|---|
| Overview (layout-dashboard) | Recommendation card "Recommended for Apex Radiance" (DXVK and Sims3SettingsSetter, only the missing ones, while `[ui] recommend_s3ss` is true; Download, "Don't show again"). **All effects** card: one switch with "All effects are on / Some effects are on / All effects are off"; it controls Night Lights, Ambient Occlusion, Banding Fix, Depth Blur, Edge Smoothing, Picture, Water Reflections and the Performance group. **Lighting** card: Night Lights, Water Reflections. **Image** card: Picture, Ambient Occlusion, Banding Fix, Depth Blur, Edge Smoothing. **Performance** card: one switch for all 15 performance options. Clicking a name opens its page. Rows blocked by the game's anti-aliasing show "Waiting for game settings"; GPU cost chips show measured time only |
| WORLD > Lighting (moon-star) | Tabs **Overview** (Night Lights card; lighting balance Subtle / Soft / Natural / Custom with Undo choice; Refresh lighting card), **Ground**, **Objects**, **Buildings** (Buildings card, Rooms at Night card), **Stories** (Upper floors light the ground and the floor-sharing options). While Night Lights is off, the other tabs show a note and "Turn on Night Lights". See [Night Lighting](features/night-lighting/README.md) |
| WORLD > Water & Snow (waves-horizontal) | Lamp Glow, Water Reflections (with "Turn on Night Lights" / "Turn on Depth Blur"), Snow. See [reflections](features/reflections.md) |
| IMAGE > Color (palette) | Overview owns the Color master switch, measured GPU cost and comparison tools; its card title matches the tab. Clickable overview rows lead to Basic, Tones, Color, Detail and Filters, each with an independent group switch. Master off disables and visually turns off dependent switches without clearing saved group preferences. Filters has six compact family panels, down/up adjustment disclosures, right-click/ellipsis shortcut actions and an optional tag left of the disclosure arrow. The shortcut modal uses the native card header and right-aligned actions. No profiles on this overview. See [picture-filters.md](features/picture-filters.md) |
| IMAGE > Banding Fix (blend) | "Still being tested" note and the Banding Fix card with Strength, Moving grain and Smooth gradients. See [banding-fix.md](features/banding-fix.md) |
| IMAGE > Ambient Occlusion (contrast) | Scene AO card and Sim Occlusion card. Scene AO runs at full resolution; the reduced-resolution and reconstruction controls were removed at the user's request. See [ambient-occlusion.md](features/ambient-occlusion.md), [sim-occlusion.md](features/sim-occlusion.md) |
| IMAGE > Depth Blur (aperture) | Depth Blur card; mode-specific rows are drawn and searchable only in their mode. See [depth-blur.md](features/depth-blur.md) |
| IMAGE > Edge Smoothing (spline) | FXAA / SMAA controls and the game-MSAA notice. See [edge-smoothing.md](features/edge-smoothing.md) |
| SYSTEM > Performance (gauge) | Five cards with the 15 performance switches: Camera and lighting, Files and objects, Textures and Sims, Memory handling, Game and scripts. See [Performance](features/performance/README.md) |
| SYSTEM > Lot Streaming (layers) | Cards Lot detail streaming (Extended lot detail with its distance and lot count, Smooth lot streaming, Keep lot visibility stable, Pause lot streaming in map view) and Object streaming (Spread lot objects while loading). See [lot-streaming.md](features/performance/lot-streaming.md) |
| SYSTEM > Attention (triangle-alert) | Only while something outside Apex blocks an effect (the game's own anti-aliasing with an affected feature enabled, a room colour saved in Sims3SettingsSetter) |
| SYSTEM > Report a problem (bug) | Capture session, Save a capture, Your captures, How to report a problem. See [bug-reports.md](features/bug-reports.md) |
| SYSTEM > Developer (wrench) | Developer mode only: tabs Lighting, Performance, Captures, Visual effects, Translations. See [developer-mode.md](features/developer-mode.md) |
| SYSTEM > Settings (settings) | Tabs **Menu**, **Shortcuts**, **Profiles**, **Compatibility**, **About** (below) |

### Settings tabs

| Tab | Cards |
|---|---|
| Menu | **Menu**: Language (select, 220 units), Text size (- / value / + / Reset; 80, 90, 100, 115, 130, 150, 175, 200%), Startup menu hint (`[ui] start_note`). **Screenshot capture**: Use Apex screenshot shortcut, its key, Hide game UI in screenshots, destination note. **Settings and maintenance**: Save now ("Changes also save by themselves after a second"), Enable developer mode, Reset all settings (inline confirmation and Undo; restores features, Picture and every `[ui]` preference; captures, reports and profiles stay) |
| Shortcuts | **Shortcuts**: preset (Letters / Numbers / F keys / Custom, 180 units), Open the menu, Compare with the game, Refresh the lighting, Take a filtered screenshot (disabled while the screenshot shortcut is off), note "F10 always hides the game interface". **Report a problem**: Recording, Light capture, Lighting snapshot, and Frame Capture in developer mode. **While Apex is open**: Search the settings, Peek at the game behind the menu, Compare the picture without its filters. **Personalize**: "Show the startup hint next time" |
| Profiles | Profiles card (save) and Saved profiles card (below) |
| Compatibility | Game version; Sims3SettingsSetter and DXVK Installed / Not installed; Features Running / Starting… / Off (old combined build found); RECOMMENDED group while one is missing; Details (the raw S3SS summary and the settings migration note) |
| About | "Apex Radiance for The Sims 3", "Version X" (or "Version X - Developer mode"), CREDITS |

There is no per-page or per-card reset. Each row with a default shows a changed dot and, on hover, its own Reset
button; Settings > Menu has the whole-mod reset.

## Widgets and conventions

Every card is `PushID(<name>)` + `BeginCard("##Card")` ... `EndCard()` + `PopID()`. Every setting is a row drawn by
`SwitchRow`, `Slider`, `SliderPercent`, `SegmentedRow` or `SelectRow` whose label is its stable id (unique within its
card, with a `##` suffix when two labels read the same, such as `Brightness##Walls`). Search, changed markers and undo
reports hang off these rows. Each page tab's content is its own function in `apex_gui.cpp` (`LampsTabContent`,
`GroundTabContent`, `PictureHeaderCard`, `PictureRows(tab)`, `DepthBlurContent`, `AntiAliasingContent`,
`PerformanceCard`, `MenuTab`, `ShortcutsTab`...), so pages and search results draw the same code. Feature code draws
its own card bodies (`RenderCustomUI`, `Picture::RenderUI(tab)`, the Night Lighting functions in
`patches/night_lighting.h`) and Developer sections.

Rules:

- Main controls visible; tuning in a collapsed **Advanced**; developer tools only on the Developer page.
- Feature switches are inert while features start ("Starting…") or on an unsupported game ("Not available on ...");
  `GetLastError()` shows as an error note; a feature's controls appear only while it is on (Picture's stay visible,
  greyed; search shows them either way).
- The feature description, ending with "Credits: @loinyx", is the hover tooltip of the card title, the card switch and
  the overview name; there are no other visible credit lines.
- Defaults passed to rows come from the code's real defaults (registered setting defaults, `PictureParams{}`, the
  features' `Params{}`, `ApexPatch::IsEnabledByDefault()`), never guessed.
- After adding any option, review the whole menu's organization and run the `apex-menu` audit
  (`perl .claude/skills/apex-menu/audit.pl .`).

### Design tokens

All sizes are reference units at 1080p and text size 100%, multiplied by `ApexUi::Unit()` (= `style.FontScaleMain`;
the overlay scales the style by `0.9 * pow(h / 1080, 0.8)` and the text size). Never write a pixel size without `* u`.

| Token | Value |
|---|---|
| Palette (`violet_theme.h`) | Accent #7F77DD, accent dark #534AB7, accent light #CECBF6, window #15161A, card #1C1D22, border #2A2B31, selected #24252B, hover #1F2025, switch off #3A3B42, text #E8E8EC, muted #8B8C96, warning #E0A84F, error #E8716B, success #7DBE9A |
| Disabled | Style alpha 0.5 on the whole row |
| Focus ring | ImGui's nav cursor in accent light, 2 px, rounded like the frame; custom widgets use `InvisibleButton` with `ImGuiButtonFlags_EnableNav` and repaint the cursor above their fills |
| Control heights | Compact 30 (`kControlCompact`), primary 36 (`kControlPrimary`) for profile saving, capture completion, session start/end and the developer confirmation |
| Button geometry | Icon 16 (`kControlIcon`), icon-text gap 6, padding 10 (primary 14); checkboxes 20 |
| Spacing (`kSpace1..4`) | 4 / 8 / 12 / 16; rows `kRowGap` 16 apart with a hairline in the middle; non-row elements keep 8 below a row (12 for a group label) |
| Window and cards | Window padding 12; content 8 x 4; card padding 16 x 12, rounding 10, 12 between cards; `CardDivider()` 12 above and below, only when a body follows; 12 under the page title and tab bar |
| Typography | Segoe UI 15 (Bold for titles; ImGui default if missing). Page title 1.3x bold, card title 1.05x bold; descriptions, values, end labels and pills 0.87x muted (`kSmallScale`); group labels 0.8x (`kGroupScale`); sidebar group labels in the regular font, muted, without letter spacing |
| Icons | `kIconSmall` 14 (notes, pills), `kIconMedium` 18 (card headers, sidebar, overview rows); setting rows have no icons |
| Chips | 0.8x text, 6 side padding, 14% fill: muted for GPU cost ("~0.4 ms", tooltip "Measured cost on your GPU per frame"), amber for "Reload save" |
| Changed dot | 3 radius, accent, 6 after the label (after the badge when there is one) |
| Control rows | Measure the label/description block before vertical centring; if fewer than 120 units remain for text, the controls move below with an 8-unit gap. Optional measured control height for small chips |

### Components (`ApexUi`, `ui/widgets.h`)

- `PageTitle(title, subtitle)`; `TabBar(id, &tab, labels, n, icons)`: underline tabs (selected = white text and a 2-unit
  accent underline; wraps when narrow).
- `BeginCard` / `EndCard` (always call EndCard); `CardHeader(icon, title, subtitle, tooltip, toggle, toggleEnabled,
  extra, chip)` with `HeaderExtra` for an icon toggle or a hold button (`holdIcon`, `holdTooltip`, `held`); `CardDivider()`.
- Rows: `SwitchRow(label, v, description, default)`; `Slider(label, v, min, max, SliderOptions)` (format, scale, offset,
  fixed value text, end labels, colour swatch, `defaultValue`, gradient track `trackFrom`/`trackTo` or hue circle
  `hueTrack`); `SliderPercent`; `SegmentedRow(...)`; `SelectRow(label, description, id, current, labels, count, width,
  defaultIndex)` (compact Lucide chevron inside the field); `BeginControlRow(label, description, controlsWidth, icon,
  controlHeight)` / `EndControlRow()` (returns false when search hides it: draw nothing and skip `EndControlRow`);
  `OverviewRow(...)`. Descriptions are always visible. `SliderCommitted()` = released after an edit or its Reset.
  `SetNextRowBadge(text, tooltip)` puts a chip after the next label; `SetNextRowUntranslated()` for user data.
- `Segmented(id, current, labels, count, tooltips, compact, icons)`: shared width (a vertical list when they do not
  fit); `current = -1` = none selected.
- `BeginAdvanced` / `EndAdvanced`: hairline (reused when right after `CardDivider`), accent chevron and "Advanced",
  collapsed by default, contents not indented. `AdvancedNode` stays for the frame profiler.
- `GroupLabel("WALLS")`; `IconNote(icon, text, rgb)`: info (Info, muted), warning (TriangleAlert, amber), error
  (TriangleAlert, red), in a 10% tinted box.
- Buttons: `IconTextButton(label, icon, tooltip, ButtonKind)`, `TextButton(label, tooltip, kind, minWidth)`;
  `ButtonKind::Secondary` (neutral, accent border on hover) and `Primary` (accent dark fill). `ControlSizeScope`
  sets compact or primary frame padding for a group and restores it before `EndPopup`. `ButtonWidth` for control
  rows. "Don't show again" stays a text link.
- `IconButton`, `Pill`, `Chip`, `SidebarItem`, `SidebarGroup`, `ToggleSwitch`, `Tooltip`, `MutedText`, `Gap`.
- Search filter `BeginFilter` / `SetFilterCrumb` / `EndFilter` / `FilterActive`; change reports `ReportChange`,
  `SetChangeReporting`, `TakeChange`; drag fade `SliderDragging`, `SetKeepActiveSliderOpaque`.

## Menu behaviour

State lives in `apex_gui.cpp` statics on the render thread unless a `[ui]` key is named; `[ui]` keys go through
`ApexConfig::GetUi` / `SetUi` and are saved a second later. Feature changes go through the features' own paths, so
autosave works as usual.

- **Search:** header field "Search settings" (Ctrl+F hint; Esc or x clears). The search key focuses it while the menu
  has keyboard focus. Every word of the query (case-insensitive) must appear in a row's label or description, in
  English or the shown language. Results are the live controls, each after a breadcrumb link ("Lighting › Ground");
  card headers with a switch become searchable switch rows; collapsed Advanced content is searched; feature bodies are
  searched while the feature is off. Searched, in order: the Lighting tabs, Water & Snow, Color (Banding, header, Basic,
  Tones, Color, Detail), Ambient Occlusion, Depth Blur, Edge Smoothing, Performance, Settings > Menu and Shortcuts.
  Not searched: Overview, Developer, Profiles, Compatibility, About, Report.
- **Changed markers and per-setting Reset:** a dot after the label while the value differs from its default ("Changed
  from the default"); hovering shows a Reset button that restores that one setting through the row's normal change
  path. Sliders compare with a tolerance of 1/10000 of their range. Reset buttons are not keyboard stops.
- **Undo toast:** at the first click or Space / Enter of an interaction, the menu takes
  `ApexConfig::CaptureFeatureState`. Rows, header and overview switches, Reset and "Turn on" buttons report
  "<Label> turned on/off", "<Label> changed" or "<Label> reset" (sliders once, on release). The last report becomes a
  toast at the bottom right with Undo, for 4 s (paused while hovered). Undo applies only sections that differ through
  `ApexConfig::ApplyFeatureState`. Only the last change is undoable. Menu preferences (language, text size, keys,
  sidebar) and the Developer page do not report.
- **Peek:** holding the peek key (default Alt, `[ui] peek_key`) while the pointer is over the menu makes it nearly
  transparent (window 0.2, contents about 0.1) and inert; not while typing or dragging. While a slider is dragged the
  window fades to 0.35 and the active row stays opaque.
- **Hold to compare (Color):** the eye button in the Picture header, or holding the picture-compare key (default `B`,
  `[ui] picture_compare_key`) over the menu, calls `Picture::HoldBypass()`; Picture is skipped for 0.15 s after the
  last call and returns by itself.
- **Hover key capture:** while the pointer is over the menu but the keyboard is not captured, `GuiClient::CaptureKey`
  claims the peek and picture-compare keys (not while typing); the overlay eats their key-down, key-up and character.
  Alt+Tab is handled by Windows first.
- **GPU cost chips:** from `Picture::GpuMs()` and `ApexPatch::GpuCostMs()`; hidden while off, blocked or not yet
  measured.
- **"Reload save" badge:** "Light stairs, railings, columns" (Lighting > Objects) shows an amber chip, tooltip "This
  change shows after you load a save again"; every other Night Lights option applies live.
- **Inline dependencies:** a row or card that needs another option shows an info note and a primary button that turns
  it on (and can be undone): "Turn on Night Lights" (Lighting tabs, Lamp Glow, Water Reflections), "Turn on Depth Blur"
  (Water Reflections), "Turn it on" / "Turn both on" (Lighting > Objects ground options, Snow). The game's own
  anti-aliasing is a game option and stays a plain note.
- **Keyboard:** `ImGuiConfigFlags_NavEnableKeyboard`; arrows and Tab move, Space / Enter toggle and press, sliders
  adjust with the arrows after Space / Enter. Esc (menu focused, no field active, no key being recorded) clears the
  search, else closes the menu.

## Shortcuts

All Apex shortcuts are intercepted by the overlay before the game sees them. Presets (`[ui] hotkey_preset`: `letters`,
`numbers`, `fkeys`, `mine`; shown as Letters, Numbers, F keys, Custom). A missing preset means F keys. Selecting a
preset is explicit and clears per-action overrides; opening Settings never changes a saved key. Recording a key on a row
switches the preset to Custom, which keeps the previous preset (`[ui] mine_base`) as the fallback for unchanged actions.

| Action | TOML (`[ui]`) | Letters | Numbers | F keys |
|---|---|---|---|---|
| Open the menu | `toggle_key` | Ctrl+Shift+R | Ctrl+Shift+1 | Ctrl+Shift+F11 |
| Compare with the game | `compare_key` | Ctrl+Shift+T | Ctrl+Shift+2 | Ctrl+Shift+F10 |
| Refresh the lighting | `refresh_key` | Ctrl+Shift+G | Ctrl+Shift+3 | Ctrl+Shift+F9 |
| Light capture | `probe_key` | Ctrl+Shift+V | Ctrl+Shift+4 | Ctrl+Shift+F7 |
| Lighting snapshot | `diagnostics_key` | Ctrl+Shift+B | Ctrl+Shift+5 | Ctrl+Shift+F8 |
| Recording | `recorder_key` | Ctrl+Shift+X | Ctrl+Shift+6 | Ctrl+Shift+F6 |
| Frame Capture (developer mode) | `frame_capture_key` | Ctrl+Shift+F | Ctrl+Shift+7 | Ctrl+Shift+F5 |

Not part of the presets:

| Action | TOML (`[ui]`) | Default |
|---|---|---|
| Take a filtered screenshot | `screenshot_key`, `screenshot_folder`, `screenshot_hide_game_ui` | `F8`, `game` (or `apex`), hide UI on (a saved bare letter/digit falls back to the default) |
| Search the settings | `search_key` | Ctrl+F |
| Peek at the game behind the menu | `peek_key` | Alt |
| Compare the picture without its filters | `picture_compare_key` | B |

A per-action key of vk 0 means "use the preset", so older files keep working. Compare turns Night Lights, Ambient
Occlusion, Depth Blur, Edge Smoothing and Picture off and back without saving, with a notice while off. Refresh runs
`NightLighting::RefreshAll` (terrain, rooms, lots and object rigs).

Recording a key (`ChordProblem`) waits until held keys are released; Esc cancels; recording stops when the editor is not
drawn. Refused: Esc; bare F10 (the game's interface toggle); Windows and Apps keys; bare Insert (Sims3SettingsSetter's
menu); Ctrl+Shift+C (the cheat console); Alt+F4 and Alt+Tab; modifiers on the hold rows (peek, picture compare); bare
letters, digits, Space, Enter, Backspace, Delete, Tab and arrows except on the hold rows; duplicates
("Already used by: <row>"). Key chips are at least 150 units wide (`KeyChipWidth`).

Passthrough: bare F10 down and up always reach the game, even with the menu open. After Ctrl+Shift+C opens the game's
cheat console, no Apex shortcut fires until Enter, Esc or Ctrl+Shift+C closes it; the guess also ends on focus loss,
when the Apex menu opens, on a world change or after 30 s without typing. The game's own C screenshot is never intercepted.

## Profiles

Settings > Profiles saves and applies parts of the setup as `Documents\...\Apex Radiance\Profiles\<name>.toml`.

| Bit | Part | Contents | Default when saving |
|---|---|---|---|
| 0 | Lighting | `NightTerrainRelight`, `SplitLevelGroundLight` (Night Lights, Every-Story Ground Light, Water & Snow) | Checked |
| 1 | Color | `[qol]` Picture and `SceneDither` (Banding Fix) | Checked |
| 2 | Depth Blur | `DepthBlur` | Checked |
| 3 | Edge Smoothing | `EdgeSmoothing` | Checked |
| 4 | Reserved | Former window mode; unused so later bits do not shift | - |
| 5 | Performance | The performance features | Checked |
| 6 | Shortcuts | `[shortcuts]` (menu and action keys, search, peek, picture compare) | Unchecked |
| 7 | Ambient Occlusion | `AmbientOcclusion` | Checked |
| 8 | Developer | `[developer]` ([developer-mode.md](features/developer-mode.md#profiles)) | Unchecked; shown only when developer mode is requested or active |

Parts are shown in the order Lighting, Color, Ambient Occlusion, Depth Blur, Edge Smoothing, Performance, Shortcuts,
Developer, in a two-column grid (icon and label left, 20-unit checkbox right) that falls back to one column when
translated labels do not fit.

- **Profiles card:** "Choose the settings to include in this profile.", the grid, a 30-icon Lucide picker, the name
  field (letters, digits, space, `-` and `_`; at most 32 characters; Enter saves) and Save. An existing name asks
  "\"<name>\" already exists; replace it?" with Replace / Cancel. The icon is stored by name in `[meta].icon`; unknown
  or missing names show Bookmark; applying ignores it.
- **Saved profiles card:** one row per profile (icon, untranslated name, the parts it contains) with Delete then Apply.
  Delete asks "Delete this profile?" inline. Apply opens a nested selection card (140 ms smoothstep fade) with "Settings
  to apply", "N of M selected", the grid and a footer "Unchecked settings stay as they are" with Cancel and Apply.
  Shortcuts and Developer start unchecked. A Developer part that requests activation opens the developer confirmation
  first. "Open the Profiles folder" opens Explorer.
- Applying = `ReadProfile` + `KeepProfileParts` + `ApplyFeatureState` (live, autosaved), log
  `[Menu] Profile loaded: <name> (parts 0x..)`, and the undo toast "Profile loaded". Names are sanitised
  (`SanitizeProfileName`: allowed characters only, no leading, double or trailing spaces, Windows device names refused);
  files whose names do not survive it are not listed. Retired `[display]` sections are ignored.

## Startup and notices

**Availability.** The menu, its shortcuts and the notices become available only after a world is loaded and settled
(`UpdateMenuAvailability`, checked every 200 ms on the render thread; the window procedure reads the cached result):

1. `WorldSession::IsActive()`: the WorldManager global, active flag +0x41, mode +0x1B4 in 1..3, and `LoaderDismissed()`
   (the UI service's root window, vtable +0xF4, no longer has the loading window id 0x95947678, which 0x00EC7DB9 adds
   and 0x00EC7A60 removes) ([world_session.h](../features/world_session.h));
2. with Night Lights on, `NightLighting::WorldLive()` as well;
3. then 3 s of continuous readiness.

Returning to a loading screen closes the gate again. Depth Blur also defers its pass while the loading UI is attached.

**Startup hint.** Once the menu is available, a top-center pill shows the logo and "Apex Radiance is ready · press <menu
key>" for 8 s on screen (each frame counts at most 100 ms, so a stall does not use it up); opening the menu ends it.
`[ui] start_note` (Settings > Menu > Startup menu hint, or "Show the startup hint next time") turns it on or off. No
key choice, tour or install prompt is shown at first start; legacy `[ui] welcome_done` and `key_chosen` are kept but
unused.

**Notices.** Capture, recording, comparison and startup pills share one top-center anchor 20 scaled units below the top,
one at a time: capture or recording first, then compare, then the startup hint. Startup banners suppress routine
notices, and captures hide them in their screenshot. Each pill sets its width from the measured text before `Begin`, so
the full text stays on one line; icon pills are 44 units high with a 48-unit icon compartment, a 20-unit icon, a subtle
separator, a 14-unit gap and an 18-unit right inset. They enter over 140 ms with a small downward fade and leave over
120 ms (alpha and position only).

| Icon | Used for |
|---|---|
| Activity (red, pulsing) | Recording in progress ("Recording N s · press <key> to stop") |
| Crosshair | Light capture and pointer aiming |
| CircleCheck | Capture saved |
| TriangleAlert | Warnings |
| Save | Writing files |
| Camera | Screenshot saved |
| Columns2 | Compare ("The game without Apex · <key> to turn it back on") |
| Logo (Sparkles fallback) | Startup hint |

**Startup banners** (drawn even while the menu is closed):

- An old combined build is loaded (`Startup::RefusedOldBuild`): "Apex Radiance is off" (error colour), "An old combined
  build (Sims3SettingsSetter with Apex inside) is also installed:", the module, and "Delete that file from Game\Bin,
  keep the official Sims3SettingsSetter.asi, then restart the game." Features stay off.
- An older standalone `S3SSApex.asi` is loaded too (`ApexGui::SetOldStandaloneNotice`): "An older <module> is also
  installed. Delete it from Game\Bin." (warning colour); features keep running. When the old copy loaded first, Apex
  Radiance idles (no menu, no banner) and only writes an error line to `ApexRadiance_LOG.txt`.

## What's new

Click the version in the menu footer to open the latest eight releases, newest first, in a scrollable list.
The release heading comes from `apex_changelog.cpp`; the footer comes from `apex_version.h`. Keep the newest
release entry aligned with the build version, and include intervening releases rather than skipping their history.
Each player-facing bullet must be translated in all 21 languages, including the seventeen single-language tables.
Regenerate `i18n/keys.tsv` and run the translation checker after adding or updating a release entry.

## Languages

Every language The Sims 3 ships in: English, Portuguese (Brazil), Spanish, French, German, Italian, Dutch, Polish,
Russian, Czech, Hungarian, Greek, Danish, Swedish, Norwegian, Finnish, Japanese, Korean, Chinese (Simplified),
Chinese (Traditional) and Thai (`I18n::Lang`; the first four keep their saved numbers). Settings > Menu > Language lists
"Automatic (<language>)" and then each language by its own name in its own script (a scrolling dropdown).
Automatic follows Windows' display language (`GetUserDefaultUILanguage`; Chinese is Traditional for Taiwan, Hong Kong
and Macao, else Simplified; any other language is English). `[ui] language = "auto"` or the language's code: `en pt es
fr de it nl pl ru cs hu el da sv no fi ja ko zh_hans zh_hant th` (an unknown code reads as automatic).

`ui/i18n.h`: the English text is the key. Two kinds of tables:
- `i18n/tr_*.cpp`: {English, Portuguese, Spanish, French} per entry, one table per part of the menu:
  `tr_widgets.cpp` (texts the widgets write), `tr_menu.cpp` (`apex_gui.cpp`), `tr_image.cpp` (Color, Edge Smoothing,
  Depth Blur), `tr_lighting.cpp` (Night Lights, Water & Snow), `tr_features.cpp` (performance features, framework
  notices) and `tr_developer.inc`. They are the list of every key: each new text gets an entry here.
- `i18n/lang_<code>.cpp`: {English, translation} pairs for one of the other seventeen languages, so a language is
  added or updated without touching the four-language tables. `i18n/keys.tsv` (written by `perl tools/i18n_keys.pl`)
  lists every key with its table, the pt/es/fr texts as hints and its placeholders, for the translators.

Widgets translate what they draw; raw ImGui text and run-time text use `I18n::Tr` / `Trf`. ImGui ids stay the English
label, so switching languages keeps the menu state. A missing translation shows English; developer mode lists missing
texts and placeholder mismatches on Developer > Translations. `tools/i18n_check` (run by the release script) fails on
conflicting duplicates, four-language entries missing a language, and translations whose `{}` placeholders or `##id`
differ from the key; it also prints each other language's coverage, its stale keys (translations of English texts that
changed) and whether `keys.tsv` is current. How to add texts: [`i18n/TRANSLATING.md`](../i18n/TRANSLATING.md). Logs and
the Developer page stay English.

Fonts (`ui/violet_theme.cpp`): Segoe UI covers Latin, Greek and Cyrillic. Japanese (Yu Gothic, else Meiryo or MS
Gothic), Korean (Malgun Gothic), Chinese Simplified (Microsoft YaHei, else SimSun), Chinese Traditional (Microsoft
JhengHei, else MingLiU) and Thai (Leelawadee UI, else Tahoma) come from the Windows fonts folder, merged into the regular
and bold fonts (titles in those scripts use the regular weight). ImGui 1.92 rasterises glyphs on demand, but each merged
file stays in memory (CJK: 10-21 MB), so only the scripts in use are merged: the current language's and Windows'
language's, Thai (under 1 MB) always, and Microsoft YaHei plus Malgun Gothic while the language list is open (for the
native names; dropped about two seconds after it closes). The fonts are rebuilt between frames when that set changes.
The current language's font is merged first, so shared Han characters take its regional forms. Merged fonts are scaled
to Segoe UI's em, which keeps CJK and Thai glyphs inside Segoe UI's line height. The log names the files used and any
script without a font.

Long translations (German or Finnish compounds, CJK without spaces): labels, descriptions, notes, page, section, card and
profile titles wrap (ImGui breaks a run without spaces at the line's end). Texts in a box of fixed width (tabs,
segments, pills, buttons, sidebar items, group labels, the Advanced row, chips, slider end labels, the dropdown's
chosen item) are drawn whole when they fit, which is always the case in English, and otherwise end with "…" and show
the full text on hover. A tab or pill is never wider than its line, a button never wider than its window, chips and
pills at most 16 em.

## Copy guidelines

- American English, friendly and plain: say what the player will see, not how it works. No jargon outside the
  Developer page (no "per-pixel", "lightmap", "shader", "depth", "bridge", "rig", "story").
- Vocabulary: **lamps**, **lots**, **street**, **ground**, **brightness** (strengths, shown in %), **upper floors**.
  "Night Lights" is the feature name everywhere; "Lighting" is only its page.
- Sentence case for labels, tabs and buttons ("Street lamps light lots"); Title Case only for page and card titles
  ("Ground & Lots", "Lamp Glow").
- Labels: at most about 32 characters, no final period, positive (a switch says what turning it on does).
- Descriptions, notes and tooltips: one short sentence, ideally at most 60 characters (never more than about 90), no
  final period; join two ideas with a semicolon.
- Values: % or named steps (Near / Medium / Far, Low ... Ultra) instead of raw numbers; signed amounts as "+25".
- Notes that point elsewhere name the place: "(Ground tab)", "(Lighting page)", "(Options › Graphics)".
- Prose blocks keep full sentences with periods: feature hover descriptions (ending "Part of Apex Radiance. Credits:
  @loinyx"), the recommendation texts, the startup banners and the credits.

## Icons

Lucide icons (ISC License; license `third_party/lucide/LICENSE`, source SVGs `third_party/lucide/icons/*.svg`),
credited in Settings > About.

- `ui/lucide_data.h`: each icon's SVG elements as data (path `d` strings, circles, rects, lines, `filled` dots). No
  build step: to add one, copy its elements from the `.svg`, append it to `kIcons` and to `ApexUi::IconId` in the same
  order (a `static_assert` checks the count). Ten icons (search, undo-2, chevron-left, chevrons-left, chevrons-right,
  check, gauge, eye, bookmark, trash-2) were entered by hand from Lucide's published icons; if one looks off, replace
  its data from the downloaded `.svg`.
- `ui/icons.cpp`: paths are parsed on first use (M L H V C S Q T A Z, absolute and relative; arcs by the SVG
  endpoint-to-centre conversion), flattened to polylines in the 24x24 viewBox and cached. Drawing uses
  `ImDrawList::AddPolyline` with thickness 2/24 of the size (at least 1 px), round caps and joins as small filled
  circles, `AddCircle`, `AddRect`, `AddCircleFilled`. No texture, render thread only.
- API: `DrawIcon(dl, id, pos, size, color)`, `Icon(id, pos, size, color)`, `InlineIcon(id, size, color)`,
  `IconLabel(id, label, color)`.

## Credits (Settings > About)

"Apex Radiance by @loinyx."; "Sims3SettingsSetter by sims3fiend: project origin and framework reference."; "FXAA 3.11:
Timothy Lottes (NVIDIA)."; "FidelityFX CAS: AMD (MIT)."; "Upper-floor ground light: Arro's technique, adapted in Apex
Radiance."; "Libraries and licenses: Dear ImGui (MIT), Microsoft Detours (MIT), toml++ (MIT), SMAA - Jorge Jimenez et
al. (MIT), Lucide icons (ISC)." Arro and the Split-Level fix are not mentioned anywhere else (feature descriptions,
tooltips, release notes, promotional text); functional notices about official S3SS's own fix ("Already handled by
Sims3SettingsSetter (its Split-Level Lighting Fix is on)") stay. Sims3SettingsSetter appears only in compatibility
notices, detection, the recommendation card, Credits and the settings migration.

## See also

- [Validation](validation/ui.md)
- [History](history/ui.md)
- [Report a problem](features/bug-reports.md), [Developer mode](features/developer-mode.md)

Ambient Occlusion no longer exposes Temporal smoothing, Thin object detail or Object thickness. Strength, Distance, Quality, map-view shading, Reach, lamp protection and Sim controls remain available.
