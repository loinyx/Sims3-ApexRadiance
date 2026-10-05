# Developer mode: history

Chronological record of investigations and design decisions. Newest entries at the bottom. The current behaviour is
described in [features/developer-mode.md](../features/developer-mode.md).

### 2026-10-02: one unified build

**Context:** Apex Radiance shipped two flavours: a public build and a developer build selected at compile time
(`ApexPublic`, `S3SS_PUBLIC`), with developer tools compiled out of the public binary. Every release produced two
assets.

**Finding:** the two binaries drifted, and testing a player problem with developer tools meant swapping binaries.

**Outcome:** one binary, `Release\ApexRadiance.asi`; `ApexPublic` became a no-op. Developer mode is read from
`[ui] developer_mode` before features are constructed, enabled in Settings > Menu after a confirmation, and needs a
restart in both directions. Profiles gained the optional Development part (bit 8). At the same time Edge Smoothing
listed FXAA (recommended) first, followed by SMAA, without changing stored enum values or the chosen method; the
Display page then had Anti-aliasing as its first tab and Window as the second. The Display page and the window controls
were later removed (2.5.5 release notes).

### 2026-10-03: Developer page hierarchy

**Context:** the Developer page had grown into long lists of switches and readouts. A static HTML preview with example
state was used to agree on the layout before the native implementation.

**Finding:** the five tabs (Lighting, Performance, Captures, Visual effects, Translations) were the right split, but
each needed a clear order: actions first, readouts in expandable groups.

**Outcome:** Lighting uses evidence, comparison, refresh and inspection cards, with provider and surface readouts,
terrain events, texture probes and the original individual tests in expandable groups. Performance keeps live profiler
results visible and groups measurement setup and collected timing details. Captures keeps real sessions, report and
light capture actions and the library, with frame operations and their file and shortcut details separated. Visual
effects keeps every diagnostic view and capture action, with camera, focus, shader and rendering readouts expandable.
Translations separates collection and actions, missing text and placeholder checks, using the real language selector
and collected errors. The native page uses the existing diagnostics only (no example numbers, no unsupported temporal
controls). Hooks, actions, setting keys and defaults, diagnostic persistence, activation and restart rules and profile
behaviour did not change. Compact tool controls and primary collection actions share the menu's control geometry
(commit 393399f).
