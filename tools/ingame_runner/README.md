# In-game runner

Runs Apex Radiance in the real game without anyone at the keyboard: it installs a built `ApexRadiance.asi`, starts
The Sims 3 once per load with that load's `ApexRadiance.toml`, loads a test save, drives the job's steps (speed, camera
pan, saves, reloads), collects Apex Radiance's own log and crash report, optionally records frame times, closes the
game, puts every installed file back and writes `results.md`. A feature is compared on and off as an A/B pair of
separate game starts in ABBA order.

This is the one tool in `tools/` that starts the game and writes into the game and Documents folders. The offline
harness rules in [workflow.md](../../docs/workflow.md#4-testing) do not apply to it; its own safety rules are
[below](#safety).

## Requirements

| What | Why |
|---|---|
| The Sims 3 Steam 1.67.2 (`TS3W.exe`), Steam running | The only supported build ([game-versions.md](../../docs/engine/game-versions.md)) |
| An ASI loader in `Game\Bin` (for example Ultimate ASI Loader as `wininet.dll`) | Loads `ApexRadiance.asi`, as for players; `-AsiLoader <dll>` installs one as `wininet.dll` for the run |
| `Documents\Electronic Arts\The Sims 3\Mods\Resource.cfg` with a `Packages` folder | Loads the test agent, a script mod |
| A built `ApexRadiance.asi` | `tools/development/build.ps1`; the runner takes `Public\` first, then `Release\`, or `-Asi <path>` |
| Node | `build_agent.mjs` |
| Test saves (below) | Every job names the save it loads |
| Optional: Python with `frida` (`pip install frida`) | `-Profiler`: frame times and simulation hitches |

Windows PowerShell 5.1 and PowerShell 7 both run the scripts.

### Test saves

Saves are made by the tester and never committed: they hold EA content and the tester's own households, and a shared
save would not match every tester's installed packs. A job names a save without its `.sims3` folder extension. The
shipped jobs use two:

| Save | How to make it |
|---|---|
| `ApexTest-Town` | A populated town (Sunset Valley or any other) in live mode by day, a household with several Sims on a furnished lot, the camera at street height over the lot. Save as `ApexTest-Town` |
| `ApexTest-Night` | The same town at night (21:00 to 23:00) with lit lots in view. Save as `ApexTest-Night` |

Make each save with the same expansions and mods that will be installed for the runs; a save that shows a dialog on
load still works (the agent answers dialogs), but a dialog-free save gives steadier numbers.

## Install

```powershell
./tools/development/build.ps1 -Flavor Public
node tools/ingame_runner/build_agent.mjs -GameBin 'C:\path\to\The Sims 3\Game\Bin'
./tools/ingame_runner/selftest.ps1
```

`build_agent.mjs` reads the game's script assemblies from `gameplay.package`, `scripts.package`, `simcore.package` and
`UI\UI.package` (read-only) into the ignored `outputs/ingame/agent/refs`, compiles `agent/*.cs` against them with the
.NET Framework C# compiler that ships with Windows, and writes `outputs/ingame/agent/ApexTestAgent.package`. Nothing is
installed until a run starts.

## Running

One job, one load with the feature on (the last variant):

```powershell
./tools/ingame_runner/run.ps1 -GameBin 'C:\path\to\The Sims 3\Game\Bin' -Job tools/ingame_runner/jobs/smoke.json
```

An A/B pair (four game starts: off, on, on, off) with frame times:

```powershell
./tools/ingame_runner/run.ps1 -GameBin '...\Game\Bin' -Job tools/ingame_runner/jobs/resource-lookup-cache.json -AB -Profiler Frames
```

A queue (every performance job as an ABBA pair, about 15 minutes per load):

```powershell
./tools/ingame_runner/run.ps1 -GameBin '...\Game\Bin' -Queue tools/ingame_runner/queues/performance.json -Profiler Frames
```

`-DryRun` lists the loads and each load's TOML changes without touching anything. Other options: `-Variant <name>`,
`-Profiler Sim|Both` (simulation-thread hitches), `-GameDocuments <folder>` for a localized Documents folder,
`-LoadTimeoutMin`, `-KeepSaves`, `-OutDir`.

Each load:

1. Writes `ApexRadiance.toml`: the tester's own file with the job's and the variant's `[patches.<Name>]` keys changed.
2. Starts `TS3W.exe` from `Game\Bin` (no launcher) and finds the test agent's mailbox in the game's memory.
3. Answers the main menu's "Mod Scripts Found" warning (accepting it during a load leaves the game on the loading
   screen), then loads the save through the agent and waits for live mode, answering dialogs on the way.
4. Runs the steps. A measured `wait` records frame times with `-Profiler`.
5. Asks the game to quit, waits up to 2 minutes, then closes the window; the game is ended only when both fail.
6. Copies `ApexRadiance_LOG.txt`, `ApexRadiance_Crash.txt` and `ApexRadiance_Hitches.txt` from this session and checks
   the log, and moves the agent's marker file (below) into the load's folder.

Every wait checks progress every 30 seconds (simulator ticks, or CPU time while loading) and prints the minutes left.
Two checks in a row without progress fail the load at once instead of waiting for the time limit.

The wait for the test agent after the start fails with its reason at the first of these:

| Check | Fails when |
|---|---|
| Game process | it closed (the exit code is shown) |
| `ApexRadiance_LOG.txt` | no log of this start 60 s after the start (`ApexRadiance.asi` did not load) |
| Progress | CPU time, the log and the agent heartbeat did not move over two checks (60 s) |
| Agent | no mailbox 90 s after the log's `[Main] Features started`: without the agent's marker the agent did not load, with it the agent loaded but its mailbox was not found |

The marker is a `ScriptError_*.xml` in the user folder (the game's only file API for scripts) with one
`<ApexTestAgent version=".." stage="loaded" commands="ok" />` line, written when the agent's code first runs; `commands`
holds the error when the agent's commands failed to register (`game_state` reports it as `start_error`).
`-LoadTimeoutMin` is the last resort only. `run.ps1` exits 0 when every load passed, 1 when a load failed and 2 when
the run stopped before every load ran.

## Reading the results

`outputs/ingame/<run>/` holds one folder per load (`<job>-<n>-<variant>`) with the TOML used, Apex Radiance's files
and the profiler output, plus `results.md` and `results.json`.

A load passes when the game reaches live mode, runs every step and quits, no crash report is written, every `expect`
pattern matches a line of `ApexRadiance_LOG.txt` and no fail pattern does. The fail patterns of every load:
`[CRITICAL]`, `[Patches] ... did not start`, `Verification mismatch`, `turns itself off` / `turned itself off`,
`Result differs from the game`, `Safety check failed`; a job adds its own.

`results.md` lists per load: frames per second, frame time p50 / p95 / p99 / max and frames over 50 ms per minute
(from `frame-times.js`, focused frames only: the game caps itself at about 30 FPS in the background), and simulation
hitches per minute (`stutter-profile.js`). For an A/B pair it adds the mean per variant and the load order. Loads are
separate game starts, so a difference smaller than the spread between loads of the same variant is not an effect.

## Writing a job

A job is a JSON file in `jobs/`, its `id` equal to the file name:

```json
{
  "id": "file-list-cache",
  "description": "What the job shows and its pass rule.",
  "save": "ApexTest-Town",
  "toml": { "ui": { "developer_mode": false } },
  "variants": {
    "off": { "toml": { "patches.FileListCache": { "enabled": false } } },
    "on":  { "toml": { "patches.FileListCache": { "enabled": true } }, "expect": ["\\[FileListCache\\] On:"] }
  },
  "fail": ["\\[FileListCache\\] Verification mismatch"],
  "steps": [
    { "do": "speed", "speed": "fast" },
    { "do": "camera_pan", "lots": 8, "seconds": 40, "passes": 15 },
    { "do": "wait", "minutes": 10, "measure": true }
  ]
}
```

| Field | Meaning |
|---|---|
| `toml` | Keys set for every variant: `{ "<table>": { "<key>": value } }`, tables as written in `ApexRadiance.toml` |
| `variants` | Named variants with their own `toml`, `expect` and `fail`. Without `-AB` or `-Variant` the last one runs; `-AB` runs them in ABBA order, or in the job's `order` list |
| `expect`, `fail` | Regular expressions over the lines of `ApexRadiance_LOG.txt` |
| `steps` | Run in order after the save is in live mode. Without steps: `speed` (`speed`, default `normal`), then a measured `wait` of `minutes` (default 10) |
| `gameArgs` | Extra `TS3W.exe` arguments |

| Step | Arguments | Does |
|---|---|---|
| `speed` | `speed`: `pause`, `normal`, `fast`, `ultra` | Sets the game speed; a wait sets it again when a dialog paused the game |
| `wait` | `minutes`, `measure` | Lets the game run; answers dialogs every 30 s. `measure` records frame times with `-Profiler` (the first measured wait counts) |
| `camera` | `pos`, `target` (`{x, y, z}`) | Places the game camera |
| `camera_pan` | `lots` (8), `height` (25 m), `seconds` (40), `dwellS` (0), `passes` (4) | Pans the camera over the densest neighbourhood's lots, there and back, in the background; follow it with a `wait` |
| `save` | | Saves as a new `ApexRunner-<load>-<n>` save and waits until it is written. The runner deletes these saves after the load unless `-KeepSaves` |
| `reload` | `save` (`last` or a save name) | Loads a save from inside the world and waits for live mode |

For a new feature: copy the closest job, set the `[patches.<Name>]` table (the feature's settings table names it),
put the feature's start line in the `on` variant's `expect` (most features log `[<Tag>] On: ...` when they install)
and its own refusal or mismatch lines in `fail`, and pick steps that exercise it. Then `./tools/ingame_runner/run.ps1
... -DryRun` and `./tools/ingame_runner/selftest.ps1`.

A step the agent does not have yet goes into `agent/Agent.cs` (a `Dispatcher.Register` command; the game's own script
API, C# 5) and `run.ps1` (the step's `switch` branch). Rebuild with `build_agent.mjs`.

## Safety

- **Never while the game is in use.** The runner refuses to start when `TS3W.exe` or `Sims3LauncherW.exe` is running,
  and stops the run when a game is still running after a load.
- **Everything is put back.** Before installing, it copies `Bin\ApexRadiance.asi`, `ApexRadiance.toml`,
  `scriptCache.package` and, with `-AsiLoader`, `Bin\wininet.dll` into `<run>\backup` with a manifest, and restores
  them (or removes what did not exist) when the run ends, fails or is stopped with Ctrl+C. After a power loss:
  `run.ps1 -Restore outputs/ingame/<run>`.
- **Saves:** jobs only load the tester's saves; `save` steps write new `ApexRunner-*` saves, never over the loaded one,
  and the runner deletes them after the load.
- **The game window** is never moved or focused by the runner. Leave it in front and do not use the computer during
  measured runs: frame times only count while the window is in front, and other work on the machine shows up as
  stutter.
- The game is ended (`Stop-Process`) only when it neither quits nor closes its window, and only the process the runner
  started.

## Limits

- Apex Radiance reads its settings at start, so every A/B variant is a separate game start. The Tester tab's switches
  change at run time but only from the menu; the runner cannot flip them.
- Frame times come from a Frida hook on the game's frame function (`0x00EC9F00`). The developer-mode Frame Profiler
  hooks the same function and checks its bytes, so it would refuse to time that function if started from the menu
  during a measured load. It cannot be started by the runner: `[qol.frame_profiler] enabled` is forced off at start.
- `stutter-profile.js` hooks the simulation loop and the route functions; another mod that patches those prologues
  makes it refuse that hook (logged in `measure<n>-profiler.log`).
- The agent drives the game through its script API: a save that opens a dialog the agent does not know may need an
  answer rule in `kDialogRules` (`agent/Agent.cs`).
- Only the Steam 1.67.2 executable is supported; the profilers check their hook bytes and refuse other builds.

## Origin and licences

Adapted from the test harness of [sims3-modernization](https://github.com/rodneypantonial/sims3-modernization) (MIT,
[LICENSE-sims3-modernization](LICENSE-sims3-modernization)): the agent and its mailbox, the profilers (copied
unchanged apart from their headers; their `docs/` references point to that repository), the package writer and the
fail-fast waits. The agent was forked there from [Sims3-MCP](https://github.com/Critical-Reynolds/Sims3-MCP) (MIT,
[agent/LICENSE-Sims3-MCP](agent/LICENSE-Sims3-MCP)).
