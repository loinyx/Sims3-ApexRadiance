# Lot Streaming: validation

The feature is described in [features/performance/lot-streaming.md](../features/performance/lot-streaming.md). It must:

- Write `WorldManager+0xDC` (distance) and `+0xE4` (detailed lots) only after reading the expected native value, keep
  them only while Apex still owns them, and restore only the fields it still owns.
- Enable the native Lot LoD transition throttle and set the camera threshold (`+0xEC`) to 5.0, restoring only values
  still equal to what Apex wrote.
- Replace the detailed-lot bias (`subss xmm0,[edi+0E0h]` after the JZ) with `mulss` by 0.81 only when the JZ reads 0x74
  and the 8 bytes match, and restore it only while it still holds Apex's instruction.
- Raise "Max Active Lot Threshold" to Max Active Lots while Smooth lot streaming runs, never lower it, and restore it only
  while it holds Apex's value.
- Share one load-stage budget per frame among the streaming lots (6 ms still, 4 ms moving): never hold the lot being
  played, a waiting stage (17 on), the loading screen or the impostor builder's updates; hold any other lot at most 6
  frames in a row, one forced lot per frame.
- Hold the map-view "skip lot streaming" gate (`+0x258`) only while the map is open and 1 s after, then restore it.
- Make no write at all for a part that official Sims3SettingsSetter already handles.
- Keep building and apartment shells, large exterior geometry and flora in the first object window.

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
- With `-Exe`, the Steam 1.67.2 bytes at the six sites the patches read or write, and the threshold's initial value.

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

## In-game test plan

1. Turn on Extended lot detail at 300 and 16 and pan over a dense neighbourhood. **Expected:** lots keep full detail
   farther away and up to 16 at once; the log shows the values applied.
2. Turn it off. **Expected:** the game's 70 and 8 come back.
3. Smooth lot streaming on, pan quickly. **Expected:** fewer lots switching detail back and forth.
4. Keep lot visibility stable on, rotate the camera in place. **Expected:** no lots loading or unloading from the angle
   alone.
5. Pause lot streaming in map view on, open and close the map. **Expected:** no lot detail streaming while the map is
   open; streaming resumes about 1 s after it closes.
6. With official Sims3SettingsSetter's LotStreamingOptimizations on. **Expected:** each matching row shows "Handled by
   Sims3SettingsSetter" and Apex makes no write.
7. Spread lot objects while loading on, enter a lot. **Expected:** objects appear over a few frames; shells and flora at
   once.
8. Keep lot visibility stable on, pan slowly along the edge of the detail distance. **Expected:** the log shows
   `[LotVisibility] Detailed lots measured at 0.81`; lots at the edge stay detailed instead of switching back and forth;
   the Frame Profiler's promotions and demotions per minute fall.
9. Extended lot detail at 16 and Smooth lot streaming on. **Expected:** the log shows `[LotLod] Max Active Lot Threshold
   12 -> 16`; promotions start one at a time.
10. Frame Profiler on, travel to a dense neighbourhood (Lot Lighting While Moving on). **Expected:** no hitch dominated by
    "Lot load stages" above about 15 ms; lots still appear; the Lot Lighting While Moving developer line shows the shared
    build counters. Repeat with `[dev_build] lot_build_share = false` to compare.

## Confirmed in game

- The fork's controlled probes and the A/B above, on the EA app build 1.69.

## Open checks

- The shared lot build time, the 0.81 hysteresis and the threshold follow in game (Steam 1.67.2): only offline evidence
  so far.

- Steam 1.67.2: every part in game; its addresses were found offline (signatures), not confirmed in game.
- Memory use at 300 + 16 in large worlds on Steam.
- Spread lot objects while loading together with lamp edits (lamps arriving one by one relit the rooms).
