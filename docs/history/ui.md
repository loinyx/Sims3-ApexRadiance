# Menu UI: history

Chronological record of investigations and design decisions. Newest entries at the bottom. The current menu is
described in [ui.md](../ui.md).

### 2026-09-28: Violet menu for players

**Context:** the first standalone menu grew from developer tools and used technical wording.

**Finding:** players needed short plain words, one feature per card and developer items kept apart.

**Outcome:** the menu was reorganised for players with developer items on one Developer page. The same day it got a
design polish (spacing scale, row descriptions, dividers, tinted notes, button styles), a full copy rewrite and the
grouped sidebar with tabbed pages. Later that day, from a mockup: search, changed markers with per-setting
Reset, the undo toast, Profiles, peek, hold to compare, GPU cost chips, "Reload save" badges, inline "Turn on"
dependencies, the status bar, the collapsible sidebar, colour tracks and keyboard use.

### 2026-09-28: Looks presets removed

**Context:** a "Looks" card (Classic / Balanced / Cinematic presets on Overview and in the tour) existed briefly.

**Finding:** applying a look rewrote the player's tuned Night Lights, Picture and Depth Blur values; only the last
change is undoable, so a hand-tuned setup was lost.

**Outcome:** removed. Whole-setup presets are not reintroduced without an explicit request; if they ever return, the
current setup must first be saved as a profile.

### 2026-09-28: colour tracks and "Reload save" badges

**Context:** the Lamp color row (Lighting > Lamps, Pink ... Warm white) and "Light stairs, railings, columns" changed
only after a save loaded again.

**Finding:** players could not tell why nothing changed.

**Outcome:** both rows got an amber "Reload save" chip (tooltip "This change shows after you load a save again"), and
the footer "Some changes show after you reload your save" was removed. The Lamp color row had a gradient track from the
game's pink to warm white and a swatch computed from the real tint math (`LampColourAt` in
`night_terrain_relight_patch.cpp`, the same blend as `ObjectLightBridge::TintStockColour`: stock pink (1, 0.75, 0.79) to
warm white (1, 0.80, 0.62) scaled to the pink's luminance; an approximation of the on-screen colour). Picture's Shadow
and Highlight colors use a hue-circle track. The Lamp color row was later removed from the menu (`LampColourAt` is
unused); only the stairs badge remains.

### 2026-09-29: Stories tab, Rooms at Night card and Performance page

**Context:** story-related options were spread across tabs; the performance features had no page.

**Finding:** everything about stories belonged together.

**Outcome:** Lighting gained the **Stories** tab ("Upper floors light the ground" as the separate
`SplitLevelGroundLight` feature, shown on and disabled with "Already handled by Sims3SettingsSetter ..." when S3SS's
own fix is on; "Outdoor light between floors"; "Seamless walls between floors" and "Indoor light between floors"
(Experimental), disabled with a "Needs ..." note while "Outdoor light between floors" is off), and Buildings gained the
Rooms at Night card ("Darker unlit rooms" + Light left, Blue tint, Soft light on furniture). SYSTEM > Performance was
added with one card of switches.

### 2026-09-30: start note, recommendations and Banding tab

**Context:** a faint brand name in the bottom-right corner was drawn with the terrain, often still under the load
screen, and went unnoticed.

**Finding:** a clear note at every start was more useful.

**Outcome:** the brand note was removed and the "is ready" pill now shows at every start (chosen design: logo, "Apex
Radiance is ready", a dot, "press" and the key in light violet, in a dark rounded pill with a faint violet border).
DXVK (detected by the loaded `d3d9.dll`: not in the Windows folder and containing "dxvk") and official
Sims3SettingsSetter became recommendations: a note in the top-left corner at every start ("Not now" = this session,
"Don't show again" = `[ui] recommend_s3ss = false`, for both), the Overview card and Settings > Compatibility. Color
gained the Banding tab. Profiles gained the Ambient Occlusion part (bit 7) so older masks keep their bits.

### 2026-10-01: page layout before the 2.5.x refinements

**Context:** the page table of the menu as it stood before the later refinements, kept for reference.

**Finding:** Lighting had tabs **Lamps** (Night Lights card: master switch and "Lamp color" Pink ... Warm white with a
colour track, swatch and "Reload save" badge), **Ground** (Ground & Lots: street lamps light lots, lot lamps light the
street, smooth ground light, BRIGHTNESS), **Objects** (groups LAMP LIGHT and DOORS, COUNTERS AND FENCES; the DOORS group
needs two Ground options, with a note naming the missing ones and "Turn it on" / "Turn both on"), **Buildings** (WALLS
and ROOFS; Rooms at Night) and **Stories**. Depth Blur: Focus Auto / Fixed, Blur amount; Auto: Sharp area Small /
Medium / Large; Fixed: Distance Near / Medium / Far, "Fine-tune distance" (0-100% of 0..0.5) and Transition; Sharp in
map view; Advanced: Strength, Quality, Focus speed (Auto only, "0.3 s"), Blur the sky, Glowing lights. Settings > Menu
had the menu key row with Change. The Developer page (development build only) had ImGui tabs Lighting (Night Lights
status, census, diagnostics, light probe, counters, the generic list of every option, Every-Story Ground Light state),
Profiler (Frame Profiler, the Apex shaders line, then a Performance card: resource lookup cache counters with the
"Remember missing files" and file list cache lines, "Check 1 answer in N against the game", "Check every answer for 10
s", lot lighting call / camera / budget lines, wall shading gate lines with the slider "Longest wait of a pass while
moving (ms)" (0-10000, default 2000), texture and cache compression lines, scene node budget lines with "Nodes per frame
while moving" (8-4096, default 512), "ms per frame while moving" (0.1-10, default 2.0) and "Longest wait (ms)" (16-5000,
default 500), and object lookup index lines with "Check 1 answer in N against the game" (default 64) and "Check every
answer for 10 s"; none saved), Capture (Frame Capture) and Debug views (Edge Smoothing status, GPU cost and red pixels;
Depth Blur status, focus mode and depth, GPU cost, "Show blur amount", far plane; Picture's 8-bit note and GPU cost).
The Ambient Occlusion page carried a note to turn off the game's own Edge Smoothing and a performance note ("Heavier on
the graphics card than other effects: lower the Quality if the game slows down"), with "Show the shade alone" (not
saved) under Advanced. Undo snapshots included `[display]` and applied the window mode through `Borderless::SetMode`;
search covered the Display tabs; the status bar showed "Sims3SettingsSetter detected" / "not installed"; developer
items were guarded with `if constexpr (!kPublicBuild)`.

**Outcome:** superseded by the entries below; the current layout is in [ui.md](../ui.md).

### 2026-10-02: Performance grouping (2.5.3)

**Context:** the Performance page was one long card.

**Finding:** four groups read better: Camera and lighting (room queue, moving lot budget, wall shading, scene setup);
Files and objects (resource cache, missing resources, file lists, object index); Textures and Sims (DXT, several cores,
cache compression, Sim sorting); Memory handling.

**Outcome:** four Violet cards; feature descriptions remain on hover; no keys or defaults changed; Experimental badges
removed.

### 2026-10-02: Overview refinement

**Context:** private release candidate.

**Finding:** Overview kept eleven controls: Lighting (Night Lights, Water Reflections, Faster Room Lighting, Lot
Lighting While Moving), Image (Picture, Ambient Occlusion, Banding Fix, Depth Blur, Edge Smoothing), Performance and
screen (Faster File Lookups, Borderless). Switches alone convey state (no repeated "On"); Edge Smoothing showed its
method and quality and Depth Blur its focus mode; the open-menu back-buffer MSAA check supplied "Waiting for game
settings"; unknown or loading state never invented a conflict; long names wrap before the switch.

**Outcome:** a discreet reset icon beside the title restored only the displayed switches, the water reflection amount
and the window mode, with the toast Undo. Later superseded: the window controls were removed, the reset icon was removed
with the other page resets, and the Performance group became one switch.

### 2026-10-02: default activation policy

**Context:** release candidate for new configurations.

**Finding:** normal player features should start on, except Color and Ambient Occlusion.

**Outcome:** Night Lights, Edge Smoothing, Depth Blur, Every-Story Ground Light, Banding Fix and all twelve
performance features default on for missing feature states and new configurations; Color and Ambient Occlusion stay
off; developer mode stays opt-in with confirmation; profiler and capture actions never start automatically; stored
choices are never overwritten; internal diagnostic switches are unchanged. Window and presentation control was removed
for 2.5.5. Reference values copied from the maintainer's saved configuration the same day:

- Ambient Occlusion: strength 1.6693748235702515, reach 1.3028700351715088, light protection 0.5, quality index 2
  (High), map view true; disabled by default; registered defaults and reset parameters agree; revision 5 unchanged.
  (Ambient Occlusion defaults later changed; see [history/ambient-occlusion.md](ambient-occlusion.md).)
- Depth Blur: fixed focus, amount 100%, sharp area Large (auto-focus preference), focus speed 0.1 s, fixed start
  0.318108052, transition 0.287828237, strength 100%, quality Medium, lamp highlights on, sky blur on, sharp map view on;
  far plane 1000; legacy spread 0.800000012 kept but unused; debug view off. Registered defaults, initial state and
  reset state share the same `Params`.

### 2026-10-02: page defaults for every user

**Context:** the shared menu got a reset entry point on every page.

**Finding:** feature pages reset their whole page, including internal tabs, after confirmation; Water & Snow reset only
its four settings; Display reset edge smoothing and the Apex window mode; Settings reset menu preferences and shortcuts
(keeping the Report screenshot preference); Report reset its screenshot preference without deleting files; Overview
and Developer linked to the whole-mod reset. The whole-mod reset restored feature defaults, Picture, the window mode
and UI preferences and shortcuts, with Undo; captures, reports and profiles were never deleted.

**Outcome:** shipped in 2.5.4; page and card resets were removed on 2026-10-03.

### 2026-10-02: Night Lighting balance layout

**Context:** release candidate for the Lighting Overview tab.

**Finding:** a separate Night Lights master card, three intensity choices (Subtle / Soft / Natural), a custom/current
status, an expandable scope explanation, an explicit Undo choice and a pointer to the surface tabs worked best. Soft
kept the ten-value reference: ground .75, roads 1, street lamps .8, lot lamps .8, objects .75, pieces .75, fences .75,
walls 1.5, roofs .45, water .3. Subtle kept the previous -10% variation; Natural the original defaults. Soft Plus and
Balanced were removed. Unmatched values display Custom; no choice is applied on load. The color and moonlight legacy
parameters still serve existing profiles and runtime lighting. A selection changed the ten surface intensities only;
Undo choice restored all ten. The duplicate whole-Night-Lights reset button and its API were removed.

**Outcome:** released in 2.5.5. On 2026-10-04 the presets were scoped to the Lighting page: water was excluded, so each
preset now changes nine values ([night lighting](../features/night-lighting/README.md)).

### 2026-10-02: retired display runtime

**Context:** the Display / fluency page (window mode, V-Sync policy, Apex FPS limit, VRR reports) and its modules
`borderless`, `presentation` and `display_monitor`.

**Finding:** removing an Apex controller does not enable VRR in the driver or certify game scanout; it removes Apex's
policy changes and extra FPS waits.

**Outcome:** the page, the three modules, their project entries, unused vendor SDK headers and their fixture were
deleted, with the S3SS FPS and window detection helpers (general compatibility and split-floor detection remain). The
bootstrap forwards the original presentation parameters and Present has no Apex FPS wait. Old `[display]` values are
kept inert on save but neither applied nor exported; profile bit 4 stays unused. Edge Smoothing became its own page.
See [presentation.md](../features/presentation.md) and [display-fluency.md](../features/display-fluency.md). Release
x86 build, translation checks and legacy profile filtering passed the same day (candidate SHA-256
`75EFF7A869E709D9D1C9698FF00CEC5987D693F69FF2E43DA36BF2D20F8B203B`); gameplay validation was pending.

### 2026-10-02: guided developer diagnosis and consolidated workspace

**Context:** development build.

**Finding:** a Violet guide card at the top of the Developer page (select rooms and floors, camera stutters, dark
objects, visual effects or missing translations, then Prepare / Reproduce / Save evidence) opened the relevant tool
tab without enabling diagnostics; capture-session controls (Begin, Save report, End, Open captures folder) stayed in
the guide; profiler measurements kept their separate report. A later version used horizontal Start here, Lighting,
Performance, Captures, Visual effects and Translations tabs, each task with its explanatory card, without a second
sidebar. Profiler counters were grouped as Files and objects, Camera and lighting, and Textures and compression; the
Language tab became a Translation checks card.

**Outcome:** Start here was later removed and Developer opens in Lighting; see
[history/developer-mode.md](developer-mode.md).

### 2026-10-02: guided Report page, availability gate and compact footer (2.5.4)

**Context:** the 2.5.4 hotfix and the local `2.5.4-test-report-library` tree.

**Finding:** 2.5.4 published a guided Report page (choice / capture / finish card with the original tools in advanced
sections, a temporary Violet target, the configured key and Esc). The local tree made panel opening, shortcuts and
notices wait for a loaded active session plus three seconds of readiness (Night Lights' world-live signal, with a
read-only WorldManager fallback), sampled the overlay delta time every game frame and logged a throttled slow-panel
trace. The footer was reduced to save status and "Hold Alt to peek"; Sims3SettingsSetter detection moved to Settings >
Compatibility; About credits began with Apex Radiance by @loinyx and a short sims3fiend framework attribution replaced
the promotional paragraph.

**Outcome:** the gate and footer are current; the Report page was restored to the 2.5.3 layout
([history/bug-reports.md](bug-reports.md)).

### 2026-10-02: centered notice width, icons and spacing

**Context:** notices were drawn as narrow vertical strips in some languages and used mixed icons.

**Finding:** auto-sizing and wrapping fed back into each other.

**Outcome:** pills set their width from the measured text before `Begin`; icon notices use a separated capsule (44
units high, 48-unit icon compartment, 20-unit icon, 22-unit subtle separator, 14-unit gap, 18-unit right inset);
enter 140 ms, exit 120 ms. Report screenshots with the menu open use the explicit `filteredSceneBeforeOverlay` stage
(PostScene and Picture's scene copy finish, Picture runs once, the screenshot is copied, then the menu draws; the
normal late Picture call does no second pass). The colour-difference UI mask is a heuristic and a reported F10 photo
mismatch was not considered resolved without a matched in-game comparison. Recommendation and key setup headers used
Info and Keyboard.

### 2026-10-02: unified build

**Context:** one ASI for players and developers.

**Finding:** see [history/developer-mode.md](developer-mode.md).

**Outcome:** released in 2.5.5. FXAA is listed first and recommended; SMAA is spatial only; there is no Window page.

### 2026-10-02: synchronization controls (removed before 2.5.5)

**Context:** a release candidate added experimental V-Sync policy, optional Apex FPS limiting and a target slider to
Display > Window beside Borderless, sharing the Window profile part and page reset; "Restore synchronization" reset
those three values; an S3SS FPS conflict blocked pacing with an inline explanation; driver VRR activation and DXVK
overrides were explained inline.

**Finding:** no automatic VRR claim could be made.

**Outcome:** removed with the display runtime ([presentation.md](../features/presentation.md)).

### 2026-10-03: Sim occlusion card and removal of page resets

**Context:** Ambient Occlusion gained a second card.

**Finding:** scene AO first (Strength, Distance, Quality), then Sim Occlusion (body and hair intensity, maximum
darkening, advanced coverage preview), off by default, with the User Round icon and no Experimental badge, controls
hidden while off. Page and card restore-default buttons (Overview, Picture and mixer, Depth Blur, Edge Smoothing and
the others) duplicated the per-row Reset and the global reset.

**Outcome:** page and card resets were removed (commit 1bb2291); row defaults and Settings > Reset all remain.

### 2026-10-03: shortcuts, screenshots and cheat console

**Context:** shortcuts were fixed per preset; the game's screenshot lacked Apex effects.

**Finding:** a QWERTY keyboard map with preset tiles was tried first (commit 53dd565) and then replaced by editable
rows: the map's key geometry, tooltips and per-key action scans were heavy.

**Outcome:** Settings > Shortcuts uses a preset dropdown and single-line editable rows (commit 7eefaf0). The Screenshot
capture card is the second card of Settings > Menu, with the same recorder and validation; choosing another key frees
`C` for the game's native screenshot. Unmodified F10 down and up bypass Apex shortcuts and ImGui keyboard capture,
including with the menu open; synthetic screenshot keys are observed after forwarding and screenshots wait for that
observation (2-second bound); the visibility tracker remains an estimate. With the default `C`, Apex recognises the
forwarded Ctrl+Shift+C cheat-console toggle and passes keys through until Enter, Esc or the toggle (commit 762a002).
Picture and the shared AO / AA / Depth Blur chain defer their passes until registered shader precompilation completes,
using a try-lock that never waits for the compiler; filters can start later during loading.

### 2026-10-03: control sizing

**Context:** controls of different heights sat side by side.

**Finding:** an initial proposal used 36-unit compact and 44-unit primary frames; after the first game test the
sizes went to 34/40, then to 30/36 (compact controls with 18-unit icons, a 6-unit gap and 10-unit padding; primary
groups with 14-unit padding). Popup-local size scopes did not restore frame padding before `EndPopup`, so ImGui
recovered a missing pop and later over-popped the parent style, changing neighbouring heights.

**Outcome:** `ControlSizeScope` restores padding before `EndPopup` (profile icon picker, capture and developer modals);
segmented controls inherit the compact height; search uses the shared 20-unit icon and 12-unit inset; diagnostic
checkboxes keep 20-unit boxes; former SmallButton actions use the compact widget; the profile save group wraps its
action; control rows measure the text block and move controls below when fewer than 120 units remain; custom buttons
and switches repaint the focus cursor; capture-list action widths use the current Delete label and item spacing, and
hairlines end at the card's work rectangle; shortcut chips share `KeyChipWidth` (the former 180-unit reservation around
a 150-unit button was removed). Button icons were later set to 16 units. The external frontend-design guidance was
applied within the existing Segoe UI and Violet direction.

### 2026-10-03: cross-page polish and information hierarchy

**Context:** review of every page after the sizing work.

**Finding:** Settings > Menu reads best as appearance, screenshot capture, then maintenance (saving, developer mode,
reset). Game anti-aliasing prerequisite notes on AO, Depth Blur and Edge Smoothing should appear only while the
conflict is active. Page headings identify the task, card headings the affected area, and row descriptions explain
the result instead of repeating defaults.

**Outcome:** Lighting > Overview keeps the presets and Custom (lamp intensities only; Undo only after an undoable
choice). Ground separates three behaviour switches (and dusk Updates) from four intensity sliders. Objects separates
outdoor objects, connected pieces and indoor objects (Armchair, Fence and Lightbulb icons). Stories keeps the sharing
controls visible, with seam handling and all-floor detail in Floor detail. Color keeps tint and vibrance visible, with
Film tones and the six-channel Color mixer expandable. The AO GPU-cost note shows while world AO is on. Brightness and
startup hint copy were shortened in all four languages. Section labels use the regular font without letter spacing;
Overview headings are Lighting, Image and Performance. Lighting choices share the Overview row typography; the Night
Lights overview header lost its empty divider. The Report capture session uses the Bug icon without a decorative disk,
a quieter border, connected outlined step markers and a divider above Start a session; session start uses Camera and
an active session Activity.

### 2026-10-03: profiles redesign

**Context:** Profiles was one long card.

**Finding:** saving and the saved library are separate tasks.

**Outcome:** two cards; a 30-icon Lucide picker stored as `[meta].icon` by name (Bookmark when missing); an aligned
two-column selection grid (icon and label left, checkbox right, one-column fallback) for saving and applying;
selection labels match the tabs (Lighting, Color, Ambient Occlusion, Depth Blur, Edge Smoothing, Performance, Shortcuts,
Developer), with Lighting keeping its combined part; Apply replaces Load; rows read Delete then Apply, with Apply at the
right edge; category checkboxes use the shared 20-unit geometry with the whole row clickable; applying opens a nested
selection card (8 units below the profile identity, compact header and count, grid, footer with Cancel then Apply,
140 ms smoothstep fade, footer in the small description face); an Advanced section right after `CardDivider` reuses
that divider. Category bits, TOML files, replacement and deletion confirmations and Undo did not change (commits
6ad811b, fd3d0c9, 07ab9a0, 6080a6e).

### 2026-10-03: Developer tool bodies and Refresh lighting

**Context:** follow-up to the Developer hierarchy ([history/developer-mode.md](developer-mode.md)).

**Finding:** cached file answers, object lookups, texture and cache compression and scene budgets read better with
validation controls first and the verbose counters in Live counters; long descriptions must wrap.

**Outcome:** responsive row wrappers keep native integer semantics, bounds and setters; texture-worker readouts got
their own disclosure; shader preparation got a card. Refresh lighting became available without developer mode in
Lighting > Overview below the presets, reusing the terrain and lot request flags, disabled until the world and lighting
are active, never automatic; the same card is reused by Developer. Its Refresh lights button uses the full refresh path
of the Refresh shortcut (terrain, rooms, lots and object rigs). The Sim Occlusion coverage preview uses blue for pixels
adjusted by the Sim controls, green for hair and black for original scene AO (black is outside the separate controls,
not necessarily without AO).

### 2026-10-03: Optimize rendering switch removed

**Context:** published 2.5.6 started System > Performance with an "Optimize rendering" card (`[ui] performance_mode`),
on for missing settings while explicit saved off choices stayed off; its row reset and Reset all restored it to on.

**Finding:** the switch duplicated the individual performance switches and added mode-dependent paths.

**Outcome:** PR #2 removes the card and its mode switch; the existing performance switches remain
([performance.md](../features/performance.md)).

### 2026-10-04: simpler first start

**Context:** first-run welcome tour and key prompt.

**Finding:** a new player should not have to choose a key, install another mod or complete a tour before playing.

**Outcome:** a new installation starts with the feature defaults and Ctrl+Shift+F11; the startup hint is the only
first-run element and the legacy `welcome_done` / `key_chosen` flags no longer gate anything. Settings > Compatibility
keeps the optional DXVK and Sims3SettingsSetter recommendations, which no longer interrupt startup. Lighting > Buildings
> Rooms at Night shows the Sims3SettingsSetter compatibility card only when official S3SS is loaded; "Back up and
correct" backs up `S3SS.toml` and disables only the saved room-light RGB override, and enabling Rooms at Night never
writes `S3SS.toml` (commits b99786c, f0b8d54). The language row became a standard select (`SelectRow`, 220 units).

### 2026-10-05: startup notice after the world loads

**Context:** the startup hint appeared two seconds after the first Present, often over the loading screen.

**Finding:** the menu availability gate already knows when a world is loaded and settled.

**Outcome:** the hint starts only once the menu is available, pauses during another load, and counts at most 100 ms per
frame; `WorldSession::LoaderDismissed()` adds the loading-window check, and Depth Blur defers while the loading UI is
attached (commit f575f0a).
