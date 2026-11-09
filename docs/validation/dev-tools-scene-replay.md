# Lighting scene replay: validation

Checks the physical indoor lamp model ([gpu-room-lamps.md](../features/night-lighting/gpu-room-lamps.md)) against the
game's own room solves, offline, from scenes recorded once in game. Developer tooling; nothing changes in what the player
sees.

## Recording a scene

A Recording capture (the Recorder's shortcut or its Report page row) also records a lighting scene while it runs, when
the physical indoor lamp model and the room lamp capture are on:

- At the start, every kept physical room of the active lot is sent to a solve once.
- Each lamp edit taken on the GPU sends its rooms to a solve once edits rest for 400 ms.
- Every physical room solve kept meanwhile is written (`OutdoorDelta::SceneWriteRoom`): the lot's occluders
  (`RtLampProxy::Lot`, once per generation), the room's texels (world position, normal, map place, 3D test switch), its
  furniture blocker table, and per listed lamp with a law its law, mult x norm scale, reach flag and the game's part at
  each texel.

The file is saved as `Lighting scene.apexscene` in the capture's folder (format: `features/scene_record.h`). Each recorded
solve also logs a `[GpuOracle]` line: the share of lamp texels within 2/255 of the game's part by surface, the worst
texel and the most frequent likely cause. That line leaves out the cube shadow, which only the offline replay traces.

The recording runs for the Recorder's 60 s. A room solve the game has not finished by then is not in the file.

## Replaying

```powershell
tools/scene_replay/run.ps1 -OutDir <scratch> [-Scenes <files or folders>] [-Baseline <file>] [-UpdateBaseline] [-SelfTest]
```

`-Scenes` defaults to the game's Captures folder. Every room solve goes through the current code of the GPU term
(`SceneRecord::GpuTerm`: law, the cube shadow's CPU reference `RtLampProxy::Visibility`, the game's 3D test over the
blocker table, the 0.01 drop and the soft light cut, x mult and norm scale), against the game's part. The report gives
the share within 2/255 per scene and per surface, the misses by likely cause and the five worst rooms. Likely causes:
`shadow3D` (right without the 3D test), `opening` (right without the cube shadow), `reach`, `norm` (one factor off),
`position` (lit where the law has nothing) and `unknown`.

The score is against the game's own term: the furniture fill the GPU adds under a lamp's own table
(`RtLampGpu::FurnitureFill`) is not in it, and the report and the `[GpuOracle]` line give the share of lamp texels it lifts.

With `-Baseline`, a scene whose share falls more than 0.5 points under its baseline value fails the run (exit 1);
`-UpdateBaseline` writes the current values. A scene is named by its capture folder and the folder above it (every
capture's file has the same name); an older baseline keyed by file name is written again once.

## Health report

`python tools/apex_health.py [log]` prints at most 25 lines from `ApexRadiance_LOG.txt`: edits taken and refused with the
top reasons, how fast an edit is shown, the GPU against the game's solves (the Developer self-check lines), the oracle
lines, the frame cost and the maps the game wrote over the GPU's (flicker). It keeps its summary in
`apex_health_last.json` beside the log and prints the change since the last run. The game also logs a `[Health]` warning
when more than 10% of the physical edits in 10 s went to the game's solves, with the top three reasons.

## Automated tests

| Harness | Covers | Run |
|---|---|---|
| `tools/scene_replay/run.ps1 -SelfTest` | The file round trip, the GPU term against equal and halved parts, the cause tags | x86 MSVC |
| `python tools/apex_health.py --selftest` | The log parsing and the report over a sample log | Python 3 |

## Open checks

- UNVERIFIED until the first scene is recorded: that the recorded parts are each lamp's own share of the solve as the
  GPU term models it, the file size of a full recording, and how many rooms the game solves within the 60 s.
- The cube shadow's CPU reference uses 16 samples by default (`--samples`); its agreement with the GPU's shadow map is
  checked by `tools/rt_lamps_test`, not here.
- The physical model differs from the game by design where light passes openings, so `opening` misses there are expected.
