# World light atlas and smoothed terrain light maps: history

Chronological record of investigations and design decisions. Newest entries at the bottom. The current behaviour is
described in [features/night-lighting/world-atlas-and-smoothed-maps.md](../features/night-lighting/world-atlas-and-smoothed-maps.md).

### 2026-09-25: robustness review, items 7 and 8 (memory)

**Context:** review of the combined build's smoothing code in the 32-bit process.

**Finding:** the result queue had no limit (5.6 MB per result) and 1 MB of floats per chunk was kept forever.

**Outcome:** at most 2 results waiting, only the 64 KB DXT5 kept per chunk (decoded on the worker), everything cleared
on world change, generations discard results from before a `Clear`. The worker stays detached: a joinable
`std::thread` would terminate the game at exit. Kept.

### 2026-09-25: winter seam m73/m74 and m76

**Context:** a straight seam on winter ground near a lot.

**Finding:** hypothesis 1 (m73/m74): a stale smoothed map survived when the game swapped in an unreadable new map.
Hypothesis 2 (m76): lots read their home chunk map with CLAMP ([lot light pass history](night-lighting-lot-light-pass.md)).

**Outcome:** unreadable maps drop the old smoothed map; lots read the atlas. Kept.

### 2026-09-25: load order

**Context:** snow took more than 20 s to look right after loading.

**Finding:** chunks were smoothed in registration order, not by visibility.

**Outcome:** chunks in view are processed first (`lastUse`). Kept.

### 2026-09-28: correct first (CPU path rewrite)

**Context:** the v0.1.0 path kept showing the smoothed map of the old map after the game re-rendered a chunk (dusk,
lamp edit), until the round-robin hash (one chunk per frame over every registered chunk) found it, a single worker job
ran (25 to 40 ms, estimated), and one upload per frame recreated a 5.6 MB staging texture and a video-memory texture. A
3x3 invalidation re-smoothed each chunk up to 9 times during a full rebuild. Atlas growth recreated and cleared the atlas
black and re-smoothed every chunk.

**Finding:** black ground light on lots, floors and fences while panning, and stale light after every rebuild.

**Outcome:** stale smoothed maps are never shown, the atlas gets a plain copy of a changed chunk at once, the atlas keeps
its contents when it grows (GPU `StretchRect`), jobs are coalesced by key, uploads reuse one persistent 1024² staging
texture and a ring of 3 512² atlas stagings (performance item 5 of the 25/09 review), and the call-site notice at
0x00C8504C reports which chunk changed. Expected latency per chunk in view: detection 0 to 1 frames, plain copy the same
frame, smoothed map about one job later (25 to 40 ms estimated, plus up to 2 jobs ahead of it) plus 1 frame. Do not
bring back "keep the old smoothed map until the new one is ready" or a black clear on growth.

### 2026-09-28: GPU path

**Context:** the CPU path shows a blocky frame after every change and needs lock, decode, worker and upload.

**Finding:** the same math can run as pixel shaders on the render thread straight from the game's chunk maps, in the
frame the map changes.

**Outcome:** eight entry points in `shaders/lightmap_smooth_ps.hlsl`, compiled at start-up on a background thread
(`framework/shader_cache.h`). The GPU path became the default; the CPU path stays as the automatic fallback and the
developer A/B. The CPU path's `AtlasRawCopy` is not used on this path. Expected cost about 0.2 to 0.5 ms per chunk on a
mid-range desktop GPU (estimate).

### 2026-09-29: kept order by use (`ByUse`)

**Context:** CPU cost of the per-frame scheduling.

**Finding:** the GPU service, the GPU Present step and the CPU Present step each built a list of every entry and
`stable_sort`ed it by `lastUse`. That order is exactly (lastUse descending, key ascending).

**Outcome:** `ByUse()` keeps the order and only takes out, sorts and merges back the entries marked by a new entry or
changed `lastUse`: the same result as a full sort. The notice drain reads the pending flag with a plain load before the
locked exchange, as it runs from every draw that asks for a chunk. The lot passes bind the atlas through `SamplerBind`.

### 2026-09-29: re-smoothing on streaming churn

**Context:** the post-0.1.0 per-second relight reconciliation
([changes-since-0.1.0.md](changes-since-0.1.0.md) section 2.1).

**Finding:** re-smoothing on every streaming change caused 12 to 60 ms hitches.

**Outcome:** the module reacts only to maps the game really re-rendered, and a neighbour's border change waits until
chunks stop changing. `ExpectRebuild` no longer holds jobs for automatic lamp changes, which may wait and are often
skipped ([terrain relight](night-lighting-terrain-relight.md)). Kept.

### 2026-10-02: summer multi-pass light pair

**Context:** captures 01:25:58 and 01:26:01, both on world terrain just outside a lot; 01:25:58 was the correct
appearance and 01:26:01 the wrong, brighter one.

**Finding:** 01:26:01 used the summer multi-pass world light shader (PS 756 bytes, FNV-1a `EC3141AB`; VS 656 bytes,
`5882F972`; earlier census id 475E594D/756). It reads the rebuilt map from s2 with the mapping in c13 and multiplies the
lamp RGB by c3.x squared. It never registered chunks, so its maps were not smoothed and *Ground brightness* (captured
effective scale 0.75 on the single-pass terrain) did not apply: the multi-pass draw stayed at scale 1.0
(ground_report.md section 4).

**Outcome:** the exact PS/VS pair is recognised, the 1/256 mapping validated, only s2 registered with the existing chunk
ownership checks, and the gain applied as c3.x x sqrt(gain). Constants and textures restored after the draw. Other
multi-pass variants unchanged. The reported cutoff resolved with this build
([validation](../validation/night-lighting-world-atlas-and-smoothed-maps.md)).

### 2026-10-02: compact single-layer world terrain

**Context:** F7 19:55:51 and 19:55:55; the same PS/VS pair in 19:44:08 and 19:47:36.

**Finding:** a world terrain variant with one diffuse layer reads its lamp map from s3, so the `WorldCandidate` rule
(s6+) left it unmodified next to a lot that already used the atlas and the 0.75 ground gain. Its rig-mode field had led
to 19:44:08 being read as an object; the bytecode computes chunk terrain geometry and world lamp-map uvs. 19:44:11 and
19:44:14 also contain different lot paint layers; their same-atlas observation stands.

**Outcome:** the exact pair (PS 1296 bytes `73376C6A`, VS 744 bytes `34E1F1B7`) is recognised with s3 only, the c15
mapping and the existing texture checks, reusing the smoothing path and the c7.x scale. No new per-frame scan or
replacement shader. This fixes one omitted path, not every terrain seam.

### 2026-10-04: daytime composition (11-52 F7 pair, PR #2)

**Context:** probes 11:52:00 and 11:52:03 at a daytime boundary use identical smoothed lamp and native-alpha textures.

**Finding:** the effective lamp scales agree (single pass c7.x = 1.21467388, multi-pass c3.x = 1.10212243 squared). The
inner pixel also runs terrain material passes and the multi-pass lighting shader with DESTCOLOR/SRCCOLOR blending: it
writes half the light to an 8-bit target and doubles it in material blending, so light above 2 is clipped. The single
pass applies albedo before clipping and keeps the extra light. GPU fixtures with both captured shaders reproduce the
mismatch (131 vs 102; 69 for both without lamps). This shows a composition discrepancy, not a full reconstruction of the
pixel inputs or proof that every boundary has this cause.

**Outcome:** `PatchTerrainDaylightRange` on the exact alpha-patched shader (FNV-1a `3A0A3E52`): by day the light
accumulator is capped at 2.0 before albedo, twilight blends, night keeps the alpha-only shader. It matches a hardware
composition range; it is not a new attenuation model. Native solar alpha, lamp maps, terrain strength, wall and object
balance and material passes unchanged. Gameplay and DXVK pending.

### 2026-10-04: daylight lamp RGB and native solar alpha (PR #2)

**Context:** the 02-05-41 session captured daytime terrain with the lamp map bound but its RGB multiplier zero (`c7.x`
single pass; `c3.x` squared multi-pass). A bound texture is not evidence of visible lamp light.

**Finding:** the world shaders drop lamp light by day while the lot grass shows it. The native map's alpha is solar
visibility, read by both captured variants; smoothing it with the lamp RGB could change solar shading.

**Outcome:** a daylight lamp term in `TerrainLightingPolicy::LampScale`, applied to world terrain and the terrain part
of the lot pass. Supported shaders sample native alpha on a spare sampler (`PatchTerrainNativeAlpha`,
`NativeTerrainSampler`) and keep the smoothed RGB; one texture read and one move per smoothed draw. No raw game map is
changed.

### 2026-10-04: squared ground scale (`622a93c`, PR #2)

**Context:** the 15-33-55 session contains a world terrain lamp-map path using `c4.x * c4.x` through a scalar temporary
beside a linear `c7.x` path.

**Finding:** only the linear path received *Ground brightness*, exposing a boundary whenever the gain differed from 100%.

**Outcome:** `LightMapScaleConst` recognises the exclusive squared multiplier from bytecode (not from snow or lot names)
and the runtime uses the square-root compensation. Shared constants and unknown layouts stay unchanged. Visual
validation on snow and grass pending ([snow](../features/night-lighting/snow.md)).
