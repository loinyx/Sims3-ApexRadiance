# Lot Streaming: validation

The feature is described in [features/performance/lot-streaming.md](../features/performance/lot-streaming.md). It must:

- Write `WorldManager+0xDC` (distance) only after reading the expected native value, keep it only while Apex still owns
  it, and restore it only while it still holds Apex's value.
- Never write `+0xE4` (Max Active Lots) or `+0xEC` (camera-speed threshold): the game's own option applies, also after a
  switch is turned off in a loaded world.
- Replace the detailed-lot bias (`subss xmm0,[edi+0E0h]` after the JZ) with `mulss` by 0.81 only when the JZ reads 0x74
  and the 8 bytes match, and restore it only while it still holds Apex's instruction.
- Share one load-stage budget per frame among the streaming lots (6 ms still, 4 ms moving): never hold the lot being
  played (it keeps the game's slice), a waiting stage (17 on), the loading screen or the impostor builder's updates;
  force the longest waiter after 6 frames, one lot per frame, so no lot waits more than 5 + N frames with N waiting;
  stay effective with the Frame Profiler on.
- Hold the map-view "skip lot streaming" gate (`+0x258`) only while the map is open and 1 s after, then restore it.
- Make no write at all for a part that official Sims3SettingsSetter already handles.

## Automated tests

`tools/lot_streaming_test` (in the Smoke suite of `tools/development/check.ps1`):

```
powershell -File tools/lot_streaming_test/run.ps1 [-CsvDir <folder with LotLoadAnalysis-*.csv>] [-Exe <Steam TS3W.exe>]
```

- The shared build time's decision (`features/lot_build_share_policy.h`): waiting stages pass through, budgets, floors,
  the 6-frame limit and one forced lot per frame, the lot being played.
- A synthetic burst of 7 lots promoted together, and with `-CsvDir` a replay of recorded lot loads with and without the
  shared budget (frames over 8 and 16 ms, time over 8 ms, frames to finish a lot's build).
- A model of LotLodScoring's ranking (squared distance, capacity, detail distance) with the native bias against the 0.81
  factor, on a swaying camera path.
- With `-Exe`, the Steam 1.67.2 bytes at the five sites the patches read or write.

## Latest results

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| 2026-10-04 | fork `feature/lot-lod-streaming` | Controlled probes, EA 1.69.47.024017 | Distance 200 and 300 cut off at metrics of about 40,000 and 90,000 (squared distance); 16 detailed lots at a dense point that saturated at 8; no crash or runaway allocation at 300 + 16 (private memory up to about 1566 MB, largest free block about 1790 MB) | In game (fork) |
| 2026-10-04 | fork `feature/lot-lod-streaming` | A/B in one session at 300 + 16 | Off: 149.3 transitions a minute, 87 same-lot reversals within 5 s, 48 within 2 s. Smooth lot streaming on: 99.2 a minute, 18 and 3 | In game (fork) |
| 2026-10-06 | `ba456a5` | Offline signature search in Steam TS3W.exe | The six object-throttle addresses found, each unique | None |
| 2026-10-27 | branch `perf/lot-streaming` | `tools/lot_streaming_test`, 11 recorded LotLoadAnalysis CSVs (158 lots, 743 build calls) | Native 20 ms slices: 138 frames over 16 ms, 3034 ms over 8 ms. Shared budget: 10 frames over 16 ms, 376 ms over 8 ms; lot builds end p50 4 / p90 21 frames later. Synthetic 7-lot burst: worst frame 163.8 -> 16.4 ms | None |
| 2026-10-27 | branch `perf/lot-streaming` | `tools/lot_streaming_test` hysteresis model (5 seeds, 3000 frames) | Detail switches at 70 / 8, 300 / 16 and 150 / 12: 6652 -> 1344, 5522 -> 1732, 6062 -> 1540 | None |
| 2026-10-27 | branch `perf/lot-streaming` | `tools/lot_streaming_test -Exe` on Steam TS3W.exe (SHA-256 10b2423e...0609) | All six sites match; threshold 12 | None |
| 2026-10-27 | branch `perf/lot-streaming` | Smoke and Lighting suites, three builds (development, public, standard) | Passed; builds without warnings | None |
| 2026-10-27 | branch `perf/lot-streaming` | Independent review of the release diff | Fixed: the gate passed every call through while the Frame Profiler detoured the renderer update (now a marker on the lot pass's CALL); a lot could wait far longer than 6 frames with 8 or more waiting (the longest waiter is forced); the played lot lost its own slice; the share started without the slice stub. Rejected with evidence: a negative metric (its only caller is 0x00C6C5D7 and the value is a square) | None |
| 2026-10-27 | branch `perf/lot-streaming` | `tools/lot_streaming_test` after the review fixes, 12 CSVs (195 lots) | Frames over 16 ms 162 -> 10, time over 8 ms 3412 -> 383 ms; 10 lots waiting behind the played lot: longest wait 15 frames, each ran at least 199 times in 2000 frames | None |
| 2026-10-27 | 2.16.2-dev.1 | In game (log) | `[LotVisibility] Detailed lots measured at 0.81`, `[LotLod] Max Active Lot Threshold 12 -> 16`, `[LotBuildShare] On` | In game |
| 2026-11-11 | before the removal | In game (log), Extended lot detail (300, 8 lots) and Smooth lot streaming on, then off in the loaded world | Captured at once: `camera speed threshold is 0.000`, `Max Active Lots 4`; after the game's option: `drifted 32.000 -> 5.000; maintained`; turned off: `Max Active Lots restored 8 -> 4`, `Camera speed threshold restored to 0.000` | In game |
| 2026-11-11 | branch `release/3.1.2` | Disassembly of Steam TS3W.exe and the game's `GraphicsRules.sgr` | The constructor stores `+0xDC` 70.0 (0x00C672A4), `+0xE4` 4 (0x00C672F6) and `+0xEC` 0.0 (0x00C67308); 0x00C60C00, called at load end, writes `+0xE4` = min(MaxActiveLots, PlatformMaxActiveLots) and `+0xEC` = CameraSpeedThreshold; the option's settings give 1 to 8 lots with thresholds 1.0 to 32.0 | None |
| 2026-11-11 | branch `release/3.1.2` | Removal of Smooth lot streaming, Spread lot objects while loading, the threshold-12 switch and Maximum detailed lots | Not run in game; `tools/lot_streaming_test` not run | None |

## In-game test plan

1. Turn on Extended lot detail at 300 and pan over a dense neighbourhood. **Expected:** lots keep full detail farther
   away; the log shows `[LotDetailRange] Lot LOD distance: 70.000 -> 300.000`; the number of detailed lots stays the
   game's option.
2. Turn it off in the loaded world, then pan. **Expected:** the log shows `Lot LOD distance restored 300.000 -> 70.000`;
   lots keep loading while the camera moves and as many lots as before stay detailed.
3. Keep lot visibility stable on, rotate the camera in place. **Expected:** no lots loading or unloading from the angle
   alone.
4. Pause lot streaming in map view on, open and close the map. **Expected:** no lot detail streaming while the map is
   open; streaming resumes about 1 s after it closes.
5. With official Sims3SettingsSetter's LotStreamingOptimizations on. **Expected:** the Keep lot visibility stable and
   Pause lot streaming in map view rows show "Handled by Sims3SettingsSetter" and Apex makes no write.
6. Keep lot visibility stable on, pan slowly along the edge of the detail distance. **Expected:** the log shows
   `[LotVisibility] Detailed lots measured at 0.81`; lots at the edge stay detailed instead of switching back and forth;
   the Frame Profiler's promotions and demotions per minute fall.
7. Frame Profiler on, travel to a dense neighbourhood (Lot Lighting While Moving on). **Expected:** no hitch dominated by
   "Lot load stages" above about 15 ms; lots still appear; the Lot Lighting While Moving developer line shows the shared
   build counters. Repeat with `[dev_build] lot_build_share = false` to compare.
8. Load a configuration saved by 3.1.1 with Smooth lot streaming, Spread lot objects while loading and 16 detailed lots
   on. **Expected:** it loads without an error, the page shows three switches, and the game's own number of detailed
   lots applies.

## Confirmed in game

- The fork's controlled probes and the A/B above, on the EA app build 1.69.

## Open checks

- The shared lot build time and the 0.81 hysteresis in game (Steam 1.67.2): only offline evidence so far.
- Steam 1.67.2: every part in game; its addresses were found offline (signatures), not confirmed in game.
- Memory use at distance 300 in large worlds on Steam.
- The 3.1.2 removal in game: test plan steps 2 and 8.
