# Release process

How a version of Apex Radiance is reviewed, documented and published. Building and installing are in
[workflow.md](workflow.md); documentation rules in [DOCUMENTATION-STANDARD.md](DOCUMENTATION-STANDARD.md); public
wording in [PUBLIC-TEXT-POLICY.md](PUBLIC-TEXT-POLICY.md).

## 1. Review

Every release is reviewed on its exact commit before publication.

- **Independent review** of substantive code: a reviewer who did not write the change traces concrete failure cases,
  regressions and missing validation in the combined release diff. Findings are settled with code, reproductions and
  tests; uncertainty without evidence stays recorded as such. Documentation-only changes get a focused direct review.
- **Correctness:** real callers, invalidation and stale caches, resource lifetime, error paths, concurrency, state
  restoration, supported game versions and configuration or profile migrations, traced end to end.
- **Regressions:** behaviour outside the changed path, enable and disable transitions, defaults, loading, device reset,
  resolution changes and interactions with other features.
- **Visual preservation:** intended visual changes are documented. Unchanged features are compared against a verified
  baseline in the same scene, camera and settings. Intensity, quality, resolution, light counts or update latency are
  never reduced as an optimization without an intended, documented visual change.
- **Performance:** repeated work, allocations and synchronization, including work outside the profiler's measured
  region. Frame-time gains are claimed only from representative measurements.
- Each actionable finding has a severity, file and line, trigger, impact and fix or required evidence. Demonstrated
  issues are fixed in small commits and the affected checks rerun. A change added after the review is reviewed before
  publishing.

## 2. Documentation

- Feature pages describe the code at the release commit (defaults, ranges, keys, labels, dependencies, limitations).
- Validation pages record the checks that ran on the release commit in *Latest results*; unverified gameplay stays in
  *Open checks*.
- `docs/releases/<version>.md` follows [releases/README.md](releases/README.md) and is added to its index; released
  features are marked "Released in <version>" in their Status tables and in [README.md](README.md).

### Promoting a development switch

Before tagging, list every use of `DevSwitches::` in the release diff and record a decision for each switch in the
release PR: shipped on (`kShipped`), kept developer-only, or removed. A shipped switch needs its feature page to
describe it (with "No setting" in the Settings table when it has no menu option), its validation page to list its
open checks, and its known blockers either settled or stated in the release notes' known limitations. The table of
switches in [workflow.md](workflow.md#development-switches-dev_build) is updated in the same change.

## 3. In-game What's new

The changelog opened from the version in the menu footer (`apex_changelog.cpp`):

- The new version goes at the top of `kReleases` with its release date; older versions live on the GitHub releases page.
- Lines come from `docs/releases/<version>.md`, sorted into `added`, `improved` and `fixed`; empty sections are left out.
- Each line is one short sentence in the player's words (about 5 to 15 words), present tense, naming the feature by its
  menu name, with no ending period, internal names, addresses, test counts, marketing adjectives or emoji. A fix line
  states what now works ("Depth Blur waits until the world has loaded"), never "Fixed an issue where…".
- Every line is translated into all 21 menu languages (`i18n/tr_menu.cpp` and the `lang_<code>.cpp` tables).
- `APEX_VERSION_NUMBER` in `apex_version.h` matches the newest entry; the footer shows it once.

## 4. Build and publish

0. Refresh the built-in shaders: start the release build once on Windows with no `ApexRadiance_ShaderCache.bin` in the
   Apex Radiance folder, then copy the file it writes to `resources/ApexRadiance_ShaderCache.bin` and build again; the
   next start must log `N Apex shaders taken from the ones built into the ASI` and `0 to compile`. When the start writes
   no file, every shader was already built in and the resource is current ([steam-deck.md](features/steam-deck.md)).
1. Build the standard flavor (with the developer tools, no `APEX_NO_DEV_TOOLS`) once from the reviewed commit
   ([workflow.md](workflow.md#1-build)); run `tools/i18n_check` and `perl tools/menu_audit.pl .`. Since 2.13.1 the release
   asset is this build: developer mode ships off (`developer_mode = false`, enabling it asks for confirmation), and while it
   is off the build runs and logs as the players' build. (2.13.1 testing: a game session with the `APEX_NO_DEV_TOOLS` build
   behaved differently from the standard one with developer mode off; the cause was not found.)
2. Test the build in game and record the results in the validation pages.
3. The release PR is titled `release: X.Y.Z`; merging and publishing are maintainer actions.
4. Publish `Release\ApexRadiance.asi` to `loinyx/Sims3-ApexRadiance` as `Apex Radiance X.Y.Z`, with
   player-facing notes checked with `python tools/public_text_check.py --text-file`. The Nexus workflow derives its
   changelog and file description from that body. The notes open with a bold headline (the release's theme in a few
   words) and one short paragraph on what the player will notice, then one `## ` section per menu area the release
   changes (for example `## Banding Fix`, `## Night Lights`, `## Performance`), then `## Fixes`; no install or upgrade
   section (the release title carries the version):

   ```
   **<headline>**

   <one paragraph, two sentences at most, in the player's words>

   ## <menu area>
   - <what the player gets or what looks better>
   ## Fixes
   - <the problem that is gone, in the player's words>
   ```

   One short line per item, about 5 to 15 words, no ending period, no area prefix inside the lines (the section names
   it), related changes merged into one line. The lines follow the in-game What's new. The tone is natural and warm,
   with a light The Sims flavour (Sims, the lawn, the living room, the neighbourhood at night) and never overdone: no
   puns, Simlish, emoji, exclamation marks or marketing words; say what the player sees, not how it works.
5. After publication, verify that the published asset matches the reviewed build (version and SHA-256) and that the
   Nexus workflow succeeded. Publication does not validate gameplay.
