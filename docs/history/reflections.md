# Water Reflections: history

Chronological record of investigations and design decisions. Newest entries at the bottom. The current behaviour is
described in [features/reflections.md](../features/reflections.md). The history of the water pass itself is in
[features/night-lighting/water.md](../features/night-lighting/water.md).

### 2026-09-28: Reflections section in the combined build

**Context:** in the combined build (Sims3SettingsSetter fork) the pond options of Night Lighting were split into a
"Reflections" collapsing header of the Apex tab (`ApexRenderReflectionsUI()`, declared in `apex_ui.h`, implemented in
`night_terrain_relight_patch.cpp`, called from `gui.cpp` inside `ImGui::PushID("Reflections")`, placed between Night
Lighting and Ambient Occlusion). It showed "Needs Night Lighting enabled." when Night Lighting was off. Settings were
saved in `S3SS.toml`, table `[patches.NightTerrainRelight]`. `NightTerrainRelightPatch::ResetReflectionDefaults` reset
them separately from `ResetDefaults`.

**Finding:** players looked for the pond options as their own feature, not inside Night Lighting.

**Outcome:** kept as a separate menu entry. The keys stayed in `[patches.NightTerrainRelight]`. The standalone menu later
placed them on World > Water & Snow as the *Lamp Glow* and *Water Reflections* cards.

### 2026-09-28: shore reflection independent of the lamp glow

**Context:** with `lagosRefletemLampadas = false` the whole lake pass, reflection included, was switched off.

**Finding:** the pass can run with a lamp strength of 0 and add only the reflection, provided the scene depth exists.

**Outcome:** `shoreOnly = !g_water && g_waterReflSetting > 0 && DepthShare::Texture()`; the pass runs for either effect.

### 2026-09-28: Mirror Reflection Settings disambiguation

**Context:** the combined build also contained Sims3SettingsSetter's own "Mirror Reflection Settings" patch, which could
be confused with Reflections.

**Finding:** `patches/mirror_settings_patch.cpp` (patch name `MirrorSettings`, Patches tab, category Graphics,
`VERSION_ALL`) patches two floats in `.data`, `sfMirrorBaseDistance` and `sfMirrorDistanceScale` (base = scale address minus 4).
It finds them through the pattern `F3 0F 10 05 ?? ?? ?? ?? 0F C6 C0 00 0F 59 D8 0F 5C D9` (pattern offset 4, the
MOVSS operand), checks that both values are within 0.001 to 1000, and writes the configured values. Fade formula from its
technical details: `clamp01((mirrorSize x distanceScale - distance + baseDistance) / baseDistance)`. Settings in
`[patches.MirrorSettings]`: `baseDistance` (10.0, range 1 to 200) and `distanceScale` (15.0, range 0 to 200). Each mirror
is a separate camera render, so larger values cost performance. It affects mirror objects only.

**Outcome:** no shared code or addresses. Apex Radiance does not include this patch; it remains a Sims3SettingsSetter
feature.

### 2026-09-28: HDR lamp gain removed

**Context:** the combined build scaled the lamp glints by the HDR lamp gain.

**Finding:** HDR output is not part of the standalone.

**Outcome:** removed; see [removed-features.md](../removed-features.md).

### 2026-09-28: no depth guess with MSAA

**Context:** a fallback guessed a 40 m depth when no scene depth was readable (m25).

**Finding:** with the game's MSAA on it painted black patches on the water.

**Outcome:** rejected. Without readable depth the pass draws lamp glow only.

### 2026-10-02: water highlight options and glow range

**Context:** the lamp highlight filtering work of 2.5.5 added *Stabilize lamp sparkles on water* and *Preserve bright lamp
colors on water* (developer options, both on) and narrowed *Glow brightness* (`brilhoNaAgua`) to 0.1 to 0.4 with a
default of 0.4. The earlier range was 0.1 to 3.0 with a default of 1.0.

**Finding:** details in [features/night-lighting/water.md](../features/night-lighting/water.md).

**Outcome:** current settings as listed on the feature page.
