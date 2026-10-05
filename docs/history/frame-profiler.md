# Frame Profiler: history

Chronological record of investigations and design decisions. Newest entries at the bottom. The current behaviour is
described in [features/frame-profiler.md](../features/frame-profiler.md).

### 2026-09-28: engine study in the combined build

**Context:** in the combined build (Apex inside a fork of Sims3SettingsSetter) the profiler was present in both build
flavours, configured in `S3SS.toml` under `[qol.frame_profiler]` and shown in the Apex tab, section "Performance",
collapsing header "Frame Profiler" (`gui.cpp`). It timed 28 targets and logged `Timing 27 of 27 game functions` with the
optional one excluded. Output: `S3SS_Hitches.txt` in the S3SS folder.

**Finding:** 11 sessions, 51 MB of output. The 13:19 report (51 600 frames, p50 4.38 / p95 14.28 / p99 32.38 ms, 6.86%
hitches):

- Unattributed render-thread time was about 5 ms per hitch (5.08).
- "Services (self)" dominated hitches (13.6 ms per hitch, worst 134 ms): CAS SimService, JobManager main-thread jobs (job
  0x007297C0, async resource finalize, 2.2 ms per hitch), CAS TextureCompositor and WorldManager. This motivated Service
  Frame Budget.
- Terrain update and Scene::BeginFrame produced the largest single spikes (worst 52 ms and 46 ms).
- Spikes over 50 ms created about 16 textures each. Smooth Streaming (limiting lot and light work) did not reduce them.
- Remote-call jobs printed as `job 007D9840`: `g_remoteCallJobFn` and `g_remoteMethodVtable` were declared 0 and never
  assigned.

**Outcome:** the profiler became the measuring tool for Service Frame Budget, Smooth Streaming and Script GC Scheduler;
all three were removed from the standalone for no perceptible gain ([removed features](../removed-features.md)).

### 2026-09-28: combined-build interactions (historical)

These applied to modules that no longer exist in Apex Radiance:

- **Smooth Streaming** detoured 0x00AEA680 and 0x00C845C0 and verified their entry bytes, which is why the profiler
  times those functions at their call sites; the measured time included Smooth Streaming's hook. The code comment warns
  that Smooth Streaming, installed while the profiler was on, would see a foreign target at 0x00C6D68F and skip its
  terrain-queue flush; its later code accepted a call target outside TS3W's image, so the warning was considered
  outdated (inferred from code, not re-tested).
- **Service Frame Budget** detoured 0x00599A10, 0x007377F0, 0x00608630 and 0x005F0E50; the loop replacement landed in
  those detours and kept the names because services are keyed by the vtable entry.
- **Script GC Scheduler** redirected the CALL at 0x00D819AA; GC stayed timed at the 0x00E4A050 entry. On non-Steam
  builds it found 0x00C6C290 by an entry pattern and fell back to its second camera source while the profiler was on.
- **LotEdgeLighting** (`patches/lot_edge_lighting_patch.cpp`, a development leftover) checked the entry bytes of
  0x006A3EC0, so the room lightmap solve is timed through its caller.
- **HDR output, Native HDR and AO** ran inside EndScene and the registry, so they appeared in "EndScene + overlays",
  "end-of-frame draws" and the mod rows. They were removed from the standalone (AO later returned as standalone GTAO).
- The separation plan reserved registry priority -500 for an Apex Present hook between the profiler's boundary and the
  PostScene / Depth Blur / HDR frame-boundary hooks at `Priority::First`.

### 2026-09-28: standalone, developer build only

**Context:** separating Apex Radiance from the combined build.

**Outcome:** the profiler, its UI and the registry per-hook timing were compiled only into the development build. The
output file became `ApexRadiance_Hitches.txt` in `Documents\Electronic Arts\The Sims 3\Apex Radiance\`. The remote-call
job keys were fixed (`ResolveRemoteCallKeysLocked`, both PostRemoteMethodCall vtables; the second, 0x010650D8, was not
known before).

### 2026-09-28: counters, dominant cause and presets (anti-stutter plan section 8)

**Context:** the anti-stutter plan (`research\perf2\plan.md`) needed per-function evidence for its candidates.

**Outcome:** six counters (resource lookups, scene pending nodes, RefPack compression, DXT encoding, object lookups by ID,
lot room solves), a dominant cause per hitch (the `dominant:` line, same rule as `dom.pl`), the option "Time the
Mutex::Lock hook" (off by default: the resource lookup takes that lock about 580 times per full scan, so the hook's two
clock reads per call inflated exactly the lookup path, plan caveat 1b) and two measurement presets (plan section 9.2).
The counters were attached by slot swaps, hand-made hooks and suspended CALL writes, never Detours, because the functions
run on many threads. The log then read `Timing 33 of 33`. The plan had described the RefPack write as stdcall; it is
thiscall (it uses `[ecx+4]`, the allocator). The analysis tools were updated the same day; checked on the combined
build's 51 MB file with the same numbers as plan sections 2.1 and 2.2.

### 2026-09-29: layered hook sites

**Context:** the performance features needed the same sites as the counters.

**Outcome:** FindProvider and the RefPack write moved to `framework/slot_chain.h`, the DXT encoders and the object lookup
(previously hand-made hooks, safeLen 6 and 8) to `framework/entry_chain.h`, the scene drain CALL to
`framework/call_chain.h`. The profiler is the outer layer. "Lot room solve" calls were relabelled "lot levels updated"
(they had been described as rooms relit).

### 2026-09-29: round 3 counters

**Outcome:** Wall AO pass (inside the wall shading gate) and the two Key list targets; the log read `Timing 36 of 36`
with the defaults.

### 2026-09-29: measuring Apex's own cost (M1 to M3, P1)

**Context:** `research\perf2\apexcost\report.md` measured Apex code at about 0.8 ms of a 5.1 ms frame while "mod D3D" read
0.16 ms. A dispatch cut short by Skip (every draw Night Lighting replaces) never reached the +1000 hook and was dropped by
`CleanStale`, and the state chains were not timed at all.

**Outcome:**

- M1: the registry itself books its outermost dispatch as "D3D hooks (mod)", also on Skip or Block; the draw +1000 hooks
  were removed.
- M2: samples in Apex code keyed per function by RVA, with `ApexRadiance.map` written by the Release build.
- M3: every Present callback timed by name while the profiler is on.
- P1: state calls counted in the registry's detours. Before, *Count state calls* registered six counting callbacks, which
  made every game state call run a full dispatch (0.25 to 0.35 ms per frame).
- "Lamp refresh (mod)" added as its own category inside the Present hooks.

### 2026-09-30: loading research follow-ups

**Context:** in the sampler, deep DXVK, driver and kernel frames hid the game's frames in about 24% of the hitch samples
with the 512-byte stack copy.

**Outcome:** the stack copy grew to 4 KB; system-code samples record the first caller outside system code; exact EIPs in
system code are named by the nearest export; page faults per hitch; the Texture create and Texture fill counters with a
report table by texture size class (40 targets in all). The address-space monitor was added the same day
([Vulkan driver guard history](performance-vulkan-driver-guard.md)).

### 2026-09-30: sampler key pitfall

**Finding:** the sampling run that led to [Faster Sim Building](performance-fast-cas-sort.md) booked FUN_005d1960's
samples to FUN_005d1010, because FUN_005d1960 follows FUN_005d1010's jump table without int3 padding. Return addresses
on the stack identified the real function.

### 2026-10-02: unified ASI (2.5.5)

**Outcome:** one binary for all players; the profiler's code is included and gated by developer mode at run time. Loading
the settings always forces `enabled` off, and profile import never starts a measurement
([developer mode](../features/developer-mode.md)). Commit `8a145e9`.

### 2026-10-03: per-name timing of state chains (2.5.6)

**Outcome:** commit `98a4149`: with *Per-hook registry timing*, the SetRenderTarget, Set*Shader, SetTexture, SetViewport
and Set*ShaderConstantF chains are timed per name, each entry carrying a method suffix. The option's tooltip was not
updated.
