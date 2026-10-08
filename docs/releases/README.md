# Release notes

One page per published version of Apex Radiance (`ApexRadiance.asi`, repository
[loinyx/Sims3-ApexRadiance](https://github.com/loinyx/Sims3-ApexRadiance/releases)). Each page reproduces the published
GitHub release text in a common layout. Unreleased work is described in the feature pages with the status
"In development"; it gets a page here only when it is published.

## Format

Every release page uses these sections, in this order. Omit a section that would be empty; never add other top-level
sections.

| Section | Contents |
|---|---|
| `# Apex Radiance <version>` | Title, followed by one line: publication date, Git tag (linked to the GitHub release) and asset |
| `## Highlights` | The release headline and any introduction, in the player's terms |
| `## New features` | New features, options or pages. One bullet each, with a bold lead naming the feature |
| `## Changes` | Changed behaviour, defaults, menu organisation, diagnostics |
| `## Fixes` | Bugs fixed |
| `## Known limitations` | What is not supported, still unverified or pending validation at publication |
| `## Upgrade notes` | Installation and update steps, settings carried over or reset, corrections to the published text |

The pages of earlier versions reproduce the published GitHub release text, sorted into these sections. Write the
GitHub release body from the same sections; the Nexus changelog is derived from it. The release process (review,
tests, publication) is in [workflow.md](../workflow.md#6-release).

## Versions

| Version | Published | Headline |
|---|---|---|
| [2.11.0](2.11.0.md) | 2026-10-07 | Color groups, optional filter shortcuts and hidden-interface effect timing |
| [2.10.1](2.10.1.md) | 2026-10-07 | Faster exterior lighting updates in Build/Buy |
| [2.8.2](2.8.2.md) | 2026-10-07 | What's new lists the last 8 versions with a scroll bar |
| [2.8.1](2.8.1.md) | 2026-10-07 | Doors keep their side's light; snow under rugs; Banding Fix defaults in every profile |
| [2.8.0](2.8.0.md) | 2026-10-07 | Light through doors and windows; lamp colours stay near lamps; sharp indoor light with High; wall lamps at the right height |
| [2.7.1](2.7.1.md) | 2026-10-06 | 2.7.0 without the false antivirus alert |
| [2.7.0](2.7.0.md) | 2026-10-06 | Filters, lamp switches in one frame and lot streaming |
| [2.6.0](2.6.0.md) | 2026-10-05 | Sim Occlusion, a welcome screen and a cleaner menu |
| [2.5.6](2.5.6.md) | 2026-10-03 | Responsive World Lights and Rendering Optimizations |
| [2.5.5](2.5.5.md) | 2026-10-03 | Clearer lighting controls and simpler captures |
| [2.5.4](2.5.4.md) | 2026-10-02 | Hotfix: responsive lighting and continuous lot edges |
| [2.5.3](2.5.3.md) | 2026-10-02 | Faster lighting updates and a clearer Performance menu |
| [2.5.2](2.5.2.md) | 2026-10-01 | Pond reflections in any weather |
| [2.5.1](2.5.1.md) | 2026-10-01 | Capture notes with the menu closed |
| [2.5.0](2.5.0.md) | 2026-10-01 | Report a problem: captures, sessions and screenshots |
| [2.4.0](2.4.0.md) | 2026-10-01 | Smoother edges, texture sharpening and quicker street-lamp light |
| [2.3.0](2.3.0.md) | 2026-09-30 | Light between floors fixed, 3x faster cache compression, faster Sim building |
| [2.2.1](2.2.1.md) | 2026-09-30 | Banding Fix is now Experimental, with a 0-100% Strength and an optional Moving grain |
| [2.2.0](2.2.0.md) | 2026-09-30 | Banding Fix, and a lighter Ambient Occlusion with more qualities and map view |
| [2.1.0](2.1.0.md) | 2026-09-30 | Ambient Occlusion (experimental) and a start note on the first loading screen |
| [2.0.0](2.0.0.md) | 2026-09-30 | Indoor light between floors, Rooms at Night, faster room lighting, custom shortcuts and Extreme anti-aliasing |
| [1.5.1](1.5.1.md) | 2026-09-29 | Steadier night lighting and smooth light indoors |
| [1.5.0](1.5.0.md) | 2026-09-29 | More control over Night Lights: light styles, brightness per part, lamp colors and moonlight |
| [1.4.8](1.4.8.md) | 2026-09-29 | Color works with shader-replacing mods |
| [1.4.6](1.4.6.md) | 2026-09-29 | Color fix |
| [1.4.4](1.4.4.md) | 2026-09-29 | Better diagnostics for Color |
| [1.4.3](1.4.3.md) | 2026-09-29 | Reset all settings, and a friendlier search |
| [1.4.2](1.4.2.md) | 2026-09-29 | Spread new objects over frames is back (experimental) |
| [1.4.1](1.4.1.md) | 2026-09-29 | Menu languages and better profiles |
| [1.4.0](1.4.0.md) | 2026-09-29 | Even fewer stutters |
| [1.3.0](1.3.0.md) | 2026-09-29 | Smoother gameplay: fewer micro-stutters |
| [1.2.0](1.2.0.md) | 2026-09-29 | Support for other versions of the game (experimental) |
| [1.1.1](1.1.1.md) | 2026-09-28 | Night Lights: faster, cleaner, seamless |
| [1.1.0](1.1.0.md) | 2026-09-28 | Depth Blur, rebuilt |
| [1.0.0](1.0.0.md) | 2026-09-28 | First standalone release of Apex Radiance |

Versions 1.4.5 and 1.4.7 were installed for local testing only and never published.

## Before Apex Radiance

The combined build (Apex inside a fork of Sims3SettingsSetter) was published from
`loinyx/Sims3SettingsSetter-Apex` as `nightremake-v0.1.0-alpha` and `apex-v0.2.0-alpha` (asset `S3SSApex.asi`).
Their contents are analysed in [history/changes-since-0.1.0.md](../history/changes-since-0.1.0.md).
