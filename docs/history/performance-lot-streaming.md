# Lot Streaming: history

Chronological record of investigations and design decisions. Newest entries at the bottom. The current behaviour is
described in [features/performance/lot-streaming.md](../features/performance/lot-streaming.md).

### 2026-10-04: the fork's lot streaming research (idavidveiga)

**Context:** research, measurements and code by **idavidveiga** in the fork `idavidveiga/Sims3-ApexRadiance` (branch
`feature/lot-lod-streaming`, integrated on `integrate/upstream-v2.6.0-stable`), 2026-10-02 to 2026-10-04, with
controlled probes on the EA app build 1.69.47.024017.

**Finding:** the lot-detail pipeline has separate decisions (eligibility by distance, capacity, transition throttling,
visibility stability and per-object loading), each with its own native field. Distance 300 and 16 detailed lots were
validated; the native transition throttle with a camera threshold of 5.0 cut transitions by about a third and quick
reversals by most of them. The fork's research log, as written there:

This document records the Lot Streaming work separately from the general Apex Radiance changelog. It describes what was
measured in the game, which native fields were changed, how the current implementation works, which parts are independent
Apex research, and which parts share lineage with Sims3SettingsSetter (S3SS).

#### 1. Goal

The original problem was not simply "increase draw distance". The Sims 3 has several distinct decisions in the lot-detail
pipeline:

- whether a lot is eligible for Detailed View by distance/score;
- how many lots may remain detailed at once;
- how aggressively multiple LoD transitions are allowed to start;
- whether camera motion / viewing angle affects those transitions;
- how a lot's objects are built after the lot is promoted.

The work therefore separated **eligibility**, **capacity**, **transition throttling**, **visibility stability** and
**per-object loading** instead of treating them as one setting.

#### 2. Native fields and functions verified

The controlled EA 1.69 probes resolved and exercised the following native path:

| Item | EA 1.69 address / field | Meaning in the current research |
|---|---:|---|
| LotLodScoring | `0x00C6B610` | Native lot scoring path used before Detailed View decisions. |
| Lot metric function | `0x00C62110` | Produces the lot visibility/distance metric observed by the probe. |
| LotLodScoring metric CALL | `0x00C6B957` | Verified single CALL used to bind the metric safely. |
| Camera-bias branch | `0x00C623A5` | Short JZ used by the lot-visibility camera bias. |
| LotDetailRequest | `0x00AC1830` | Promotion/demotion request path logged by the probe. |
| WorldManager global | `0x01246C54` | Live WorldManager pointer. |
| WorldManager + `0xDC` | live field | Lot LOD distance. Native observed baseline: 70. |
| WorldManager + `0xE0` | live field | Active Lot Bias. Observed value: 8.0. |
| WorldManager + `0xE4` | live field | Max Active Lots. Observed/default test baseline: 8. |
| WorldManager + `0xE8` | live field | Terrain-height threshold. Observed value: 600. |
| WorldManager + `0xEC` | live field | Camera speed threshold. Native observed value: 32. |

The probe identifies the lot pointer from the metric's arguments at runtime and refuses to guess when the expected relation
cannot be proven.

#### 3. Lot LOD distance: 70 -> 100 -> 200 -> 300

##### What was changed

The test probe temporarily overrides only `WorldManager+0xDC`, after first proving the expected baseline. The write is
guarded, maintained only while Apex still owns the value, and restored on a clean unload.

##### What the tests proved

The native metric behaves as a squared-distance eligibility test for the controlled cases:

- distance 200 produced a cutoff around **40,000 = 200^2**;
- distance 300 produced a cutoff around **90,000 = 300^2**;
- at 300, a lot with metric **87,798.671875** remained Detailed View ON (~296.31 distance);
- values clearly above 90,000 remained outside the distance eligibility range in the controlled sparse-area test.

OFF states below the cutoff are not evidence of a different distance formula: the independent active-lot capacity and
priority rules can keep an otherwise eligible lot out of Detailed View.

##### Current decision

**300 is the current validated Lot LOD distance baseline.** No larger value is being adopted at this stage.

#### 4. Max Active Lots: 8 -> 16

##### What was changed

A second guarded test override was added for `WorldManager+0xE4`.

This is the native **Max Active Lots** capacity field. It is separate from the S3SS setting named
`Throttle Lot LoD Transitions Max Active Lot Threshold`.

##### What the tests proved

With distance 300 and the native capacity still at 8, dense areas repeatedly saturated at eight Detailed View lots.

With only the capacity changed from 8 to 16:

- WorldManager read back **Max Active Lots=16**;
- the same dense reference camera position that had previously saturated at exactly eight lots produced **16 consecutive
  Detailed View ON** lots;
- the 17th and later candidates could remain OFF even while inside distance 300, proving the capacity/priority stage is
  independent of distance eligibility.

##### Current decision

**16 is the current validated Max Active Lots baseline.** We are deliberately not testing 24/32 now. The goal is
refinement and stability, not maximising the number indefinitely.

#### 5. Memory and stability observation

The 300 + 16 run showed no crash, fatal error or runaway allocation pattern in the recorded test.

During a heavier part of the run:

- private committed memory reached about **1566 MB**;
- roughly **1994-2007 MB** of address space remained free;
- the largest free block remained about **1790 MB**;
- later the private value returned to roughly **1285-1286 MB**.

This is not a universal memory guarantee for every world/save, but it is enough to keep 300 + 16 as the current practical
baseline for further refinement.

#### 6. How the current Lot Streaming pieces work

##### 6.1 Smooth Lot Streaming

Apex can enable the game's native **Throttle Lot LoD Transitions** mechanism and maintain a camera-speed threshold. It does
not replace the game's lot loader.

The production feature uses camera threshold **5.0**, while the native value observed by the probe is 32.0. The controlled
300 + 16 A/B validated 5.0 together with the native transition throttle: transition rate fell from 149.3 to 99.2 per minute,
same-lot reversals within 5 seconds fell from 87 to 18, and reversals within 2 seconds fell from 48 to 3. The 5.0 value is
therefore the current validated production baseline.

##### 6.2 Transition Max Active Lot Threshold = 12

The feature `LotActiveThreshold` resolves the live setting
`Throttle Lot LoD Transitions Max Active Lot Threshold` and currently applies 12.

This value controls the native **transition throttle policy**. It is **not** the same field as
`WorldManager+0xE4 Max Active Lots`.

The value 12 comes from the S3SS LotStreamingOptimizations implementation. It remains implemented for development/reference,
but it is **not exposed in the main Performance menu** and it is not part of the validated 300 + 16 production baseline.
The user-facing capacity control is WorldManager+0xE4 (**Maximum detailed lots**, 8-16).

##### 6.3 Keep Lot Visibility Stable

The game's metric contains a camera-view bias controlled by a short conditional branch. The S3SS behavior changes the
branch from JZ (`0x74`) to JMP (`0xEB`), so lots are not promoted/demoted purely because the viewing angle changes.

Apex implements the same behavior with its own address resolution, ownership detection and safe restoration rules.

##### 6.4 Pause Lot Streaming in Map View

S3SS implements a map-view blocker around the same native `WorldManager+0x258` "skip lot streaming" gate.

Apex shares the feature goal but the standalone implementation is different: it does not detour
`WorldManager::Update`; it maintains the live gate from Apex's existing pump while map view is open and for a short
exit grace period, then restores the previous value safely.

##### 6.5 Spread Lot Objects While Loading

This is **not** a Lot LOD distance/capacity setting. It runs later, when a promoted lot builds scene objects.

The feature has direct S3SS lineage and is explicitly a port/adaptation of
`LotStreamingOptimizations.objectThrottle` into Apex's framework:

- intercept `Lot::AddLotObjectsToScene`;
- build regular objects in small continuation windows;
- use the game's remote-method marshal for continuations;
- keep large objects / outdoor flora synchronous in the first window because one-shot lot activation fixups require
  them to exist immediately.

#### 7. S3SS provenance: what is shared and what is Apex work

The public S3SS source was compared against the current standalone Apex implementation so the provenance is recorded
explicitly.

| Area | Relationship to S3SS | Current authorship/provenance statement |
|---|---|---|
| Native TS3 Lot LOD metric / fields | Same game code is available to both mods | **EA game code**, not owned by either project. |
| Distance 300 research and cutoff proof | Not part of the current public S3SS LotStreamingOptimizations settings | **Apex research/probe work.** |
| Max Active Lots 8 -> 16 at WorldManager+0xE4 | Distinct from S3SS's threshold 12 | **Apex research/probe work.** |
| Metric argument identification, 200^2/300^2 validation, diagnostic logging | No equivalent used for these controlled tests in the compared S3SS patch | **Apex diagnostic/research work.** |
| Transition throttle + threshold 12 | S3SS implements the same native live settings and uses 12 | **Behavior/value lineage from S3SS; Apex standalone wrapper/resolver/ownership code is its own implementation. The threshold-12 control is retained only for development/reference and is not in the main menu.** |
| Camera speed threshold 5.0 | S3SS exposes the same setting with default 5.0 | **Behavior/default lineage from S3SS; validated by Apex's final 300 + 16 A/B and used by the production Smooth Lot Streaming feature.** |
| Visibility JZ -> JMP | Same patch behavior is present in S3SS | **Behavior/patch lineage from S3SS; Apex adds its own validation/ownership/restoration layer.** |
| Map-view blocker | Same feature goal and native skip gate | **S3SS lineage for the feature; Apex standalone implementation differs and avoids the S3SS WorldManager::Update detour.** |
| Object throttle | S3SS public implementation is the source lineage | **Port/adaptation from S3SS into Apex EntryChain and safety framework.** |

##### Does standalone Apex contain S3SS code?

The accurate answer is **not "none at all" and not "the Lot Streaming code is just S3SS"**.

- The standalone Apex framework was rewritten and does not carry wholesale S3SS framework files or S3SS file headers.
- Several Lot Streaming features deliberately reproduce S3SS behavior because S3SS already identified useful native
  controls.
- **Object throttle is explicitly a port/adaptation from the S3SS feature and therefore has direct source lineage.**
- The visibility override and the 12 / 5.0 streaming-setting choices also have direct S3SS behavior lineage, while Apex
  supplies its own integration, validation, ownership and restoration machinery.
- The new **Distance 300**, **Max Active Lots 16**, metric probe, squared-distance validation and the controlled EA 1.69
  tests are Apex-side research and are not presented as S3SS work.

This distinction should be preserved in future public documentation and credits.

#### 8. Current production baseline

The validated research values are now in the normal production path, without the metric probe:

- **Extended Lot Detail**: enabled by default on EA 1.69.
  - **Lot detail distance**: range **70-300**, default **300**, snapped in 10-unit steps.
  - **Maximum detailed lots**: range **8-16**, default **16**, one-lot steps.
  - Apex captures the native values per live WorldManager, writes only guarded expected values, stops maintaining a field
    if another owner changes it, and restores only fields it still owns.
- **Smooth Lot Streaming**: enabled by default where supported; native transition throttle ON + camera threshold **5.0**.
- **Keep Lot Visibility Stable**: optional/experimental; camera-angle bias JZ -> JMP with ownership-safe restoration.
- **Pause Lot Streaming in Map View**: optional/experimental; uses WorldManager+0x258 while the map is open plus a 1 s exit grace.
- **Object streaming** remains separate from lot eligibility/capacity:
  - **Spread Lot Objects While Loading**: optional, default **2 regular objects per window** with **16 ms** between windows;
  - **Spread New Objects Over Frames**: optional Scene Node Budget applied later in the scene pipeline.

Active Lot Bias remains **8.0** and the terrain-height threshold remains **600**; neither is changed by Extended Lot Detail.

#### 9. Deferred / separate research

The validated Lot Streaming pass is complete. Items intentionally outside this pass are:

1. **Objects/flora visual distance:** scene/object LOD research, separate from lot Detailed View eligibility.
2. **Threshold 12 experiments:** the S3SS-derived internal transition threshold remains available only for development/reference;
   it is not a normal user-facing control.
3. **Higher-than-validated ranges:** values above distance 300 or more than 16 detailed lots are deliberately not exposed until
   separately tested for stability and memory behavior.

#### 10. Production integration after validation

After the final controlled OFF vs Smooth comparison, the research baseline was promoted into normal Apex code:

- `WorldManager+0xDC` is now managed by the production **Extended Lot Detail** feature, default **300**.
- `WorldManager+0xE4` is managed by the same feature, default **16**.
- Both settings are persisted in `[patches.LotDetailRange]` and can be changed from the Performance menu within the validated
  ranges: distance **70-300** (10-unit steps) and capacity **8-16**.
- The production implementation does **not** install the metric/scoring/detail-request diagnostic hooks.
- Smooth Lot Streaming is validated as the normal companion behavior: native transition throttle ON + camera threshold 5.
- The misleading threshold-12 switch is no longer shown in the main menu. The underlying diagnostic feature was kept for development/reference.
- Lot-object throttling and Scene Node Budget are presented separately under Object streaming.

Final A/B in one game session (300 + 16 held constant):
- everything OFF: 268 transitions over ~107.7 s = **149.3 transitions/min**; 87 same-lot reversals <=5 s; 48 <=2 s.
- Smooth ON: 190 transitions over ~114.9 s = **99.2 transitions/min**; 18 same-lot reversals <=5 s; 3 <=2 s.
- approximate reduction: **34%** transitions/min, **79%** <=5 s reversals, **94%** <=2 s reversals.

**Outcome:** the validated baseline (distance 300, 16 detailed lots, the transition throttle with camera threshold 5.0,
the visibility override and the map-view pause) became production features in the fork; the metric probe stayed a
diagnostic build.

### 2026-10-06: port into Apex Radiance

**Context:** commit `ba456a5` ported the fork's lot detail and object streaming into Apex Radiance.

**Finding:** the fork claimed fixed Steam addresses only for the scoring, request, throttle and visibility branch. The
six object-throttle addresses were found offline in Steam TS3W.exe with the fork's EA signatures, each unique:
LotAddObjectsToScene 0x00AC1130, LotUpdateObjectSceneNode 0x00ABFAC0, ScriptMessageScopeCtor 0x007D2DB0,
ScriptMessageScopeDtor 0x007D2DF0, PostRemoteMethodCall 0x00ABE9C0 (the copy called from LotDetailRequest at
0x00AC2117; its twin 0x00B35DB0 has 4 unrelated callers and differs in the last argument load), IsObjectLargeOrFlora
0x00B088C0.

**Outcome:** Extended lot detail, Spread lot objects while loading and Keep lot visibility stable, EA-only in the fork,
run on Steam with fixed addresses and on other builds through their address groups. Each part yields when official
Sims3SettingsSetter has the matching LotStreamingOptimizations setting on. "Use LoD Active-Lot Threshold 12" stays
registered, off and out of the menu, as in the fork; the metric probe (`APEX_LOT_LOD_DISTANCE_PROBE`) was not ported.
Commit `c425026` gave Lot Streaming its own page under System.

### 2026-10-06: Spread lot objects while loading off by default

**Context:** commit `37ffa08`. Lamps are lot objects, so with the object throttle they arrived one by one and every
arrival relit the rooms: a lamp edit took 5.8 to 7.2 s to settle.

**Outcome:** off by default (2 objects per window and 16 ms between windows when turned on).

### 2026-10-06: off by default for 2.7.0, out of the built-in profiles

**Context:** the Steam port had seen little play before the 2.7.0 release.

**Outcome:** the four lot detail switches (Extended lot detail, Smooth lot streaming, Keep lot visibility stable, Pause
lot streaming in map view) are off by default, and the built-in profiles no longer carry them, so applying a profile
leaves the Lot Streaming page as the player set it.

### 2026-10-27: native metric, throttle threshold and shared build time

**Context:** a study of the lot streaming path on Steam 1.67.2 (disassembly of 0x00C6D570, 0x00C6C290, 0x00C62D80,
0x00C69FF0, 0x00C7CEA0, 0x00AEB2E0, 0x00AEA680) with the Frame Profiler hitch reports (6251 hitches) and 11 LotLoadAnalysis
captures (244 completed lots).

**Finding:**

- The lot metric 0x00C62D80 is a squared distance from the camera's target point (camera +0x230 / +0x240 / +0x200 from
  0x00961440) to the lot, with no view angle. The JZ at 0x00C63015 only decides whether a detailed lot gets
  `metric -= Active Lot Bias` (8.0): under 3 m of distance. Sims3SettingsSetter's `JZ` -> `JMP` removed that small
  hysteresis rather than a view bias.
- LotLodScoring returns early while the camera target's mean speed over 10 samples exceeds `+0xEC`. The throttle flag
  applies only while Max Active Lots <= the global 0x01184328 (12): with Extended lot detail at 16 lots the throttle was
  off. With it on, one lot is promoted per call and none while 0x00C69FF0 reports a promoted lot still building.
- The renderer update ignores the load stages' result (0x00AEB306), and the stages already return 0 without a state
  change when their lot's lock is busy.
- Recorded loads: a lot's build CPU is p50 12.5 ms, p90 30.8 ms, max 117 ms; its time from request to done is p50 2.3 s,
  p90 8.3 s, of which the lighting wait of stage 20 is p50 1.9 s, p90 7.4 s. Up to 7 lots built in one frame; 96 hitches
  were dominated by the load stages (55 with the camera still, average 23.5 ms, mostly 7 lots in the frame).
- "Lot object scene nodes" (the object throttle's per-object call) cost 3.3 ms over 6075 calls in a profiler session: the
  object throttle saves almost nothing where it acts; the expensive part is the scene drain that the scene node budget
  already spreads.

**Outcome:** Keep lot visibility stable now multiplies a detailed lot's metric by 0.81 (the same 8 bytes); Smooth lot
streaming keeps the threshold at Max Active Lots; the lot load stages share one budget per frame (6 ms still, 4 ms
moving), started with Lot Lighting While Moving. The object throttle is kept as it was; removing it is left to the
maintainer. Offline evidence in the [validation page](../validation/performance-lot-streaming.md).
