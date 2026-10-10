# Portable presets and LUT menu — 2026-10-08

Development changes on `codex/cube-luts`: Presets is a sidebar page, user presets precede built-ins, native modals
handle save/apply/import/export, and selected LUTs travel automatically in one stored ZIP. Legacy TOML, profile part
bits, Profiles and LUTs paths remain supported. LUTs has its own Color tab. Development dumps use English names
inside Diagnostics; old data is retained.

Design polish reuses the shared SelectRow, card headers, control-size scopes and IconNote. A stacked SelectRow uses
the normal Lucide chevron at full width. IconNote isolates its text baseline from the preceding input. Primary action
icons match their actions, the name field has a visible label, and row actions share a vertical center.

Validation: public Win32 Release test build completed with zero warnings. Production preset/package fixture:
3,869 checks, zero failures. Native layout fixture: 588 cases over 21 languages, 430/900-pixel widths and 1.0/1.4
scales, four frames per case, no ImGui errors or clipped modal actions. Translation table check: 1,964 keys, OK.
CPU-rendered previews were inspected. Native OS picker interaction, actual ZIP sharing between players and in-game
visual validation remain pending. The artifact is a test build, not a release.

## Approved compact action layout

The explanatory action-row variant was approved. Save current is violet; every preset action, file picker button,
name/icon field group and modal footer uses the compact scope, matching Apply. The LUT filename is shown once in the
selector, with the shared full-name tooltip. Single-line IconNote text now uses the same glyph-centering helper as
buttons; multiline notices retain their wrapped layout. Validation repeated: public Win32 test build, zero warnings;
588 native layout cases passed; translation check OK at 1,967 keys. In-game appearance remains to be checked.

## Quick export refinement

The selected quick-export flow keeps source, name and output summary visible. Customize content contains the
category choices; More options contains the optional shortcut and development parts. The page toolbar is now the
approved simple button bar, and saved user presets omit the category description. Built-in descriptions remain.
Export uses shared compact widgets and spacing. Its body scrolls when expanded content exceeds the available screen
height, keeping the footer outside the scroll region; centering follows changes in modal height.

Validation: public Win32 build with zero warnings; 756 native layout cases over 21 languages, two widths and two
scales, including expanded content and optional choices, four frames per case. No ImGui recovery errors or clipped
modal actions. Translation check passes at 1,968 keys. Actual in-game appearance and file-picker interaction remain
unverified. This is a test artifact, not an installed build or release.

## Return to the original export form

The quick-export proposal was rejected after testing. The original visible category checklist and form structure are restored. Polish is limited to shared label, field and section spacing, keeping compact controls, automatic LUT packaging and the existing native components. The quick-export validation above describes the superseded layout.


## Identified action menus

The approved identified-menu variant replaces individually bordered popup buttons with shared selectable action rows. Preset and filter shortcut popups show the selected item name above a separator and use aligned 16-unit Lucide icons. Existing export, delete and shortcut callbacks and disabled states are preserved. Public Win32 build passes with zero warnings; native fixture includes an open action popup across four frames in every supported language, width and scale. In-game appearance remains unverified.


## Card section heading standard

SectionLabel now delegates to the existing GroupLabel treatment used by Overview Lighting. Preset library and filter family headings share the same regular Segoe UI, muted color and spacing. Card titles and page titles keep their distinct hierarchy. Public Win32 build passes with zero warnings.

