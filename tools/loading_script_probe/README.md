# Loading script task observer

Temporary diagnostic companion for the native transition watch. It reads the progress dialog, MiniLoad callbacks,
the previous and next game modes, and the script task registry. While the dialog remains open, the game's
`Simulator.GetObjectStackTrace` API reads selected suspended task stacks. Native function return alone cannot show
which managed continuation is waiting.

`Probe.cs` uses the installed game's script references. `build.ps1` takes their folder with `-References`, a Python
executable with `-Python`, and an output folder with `-OutDir`. It requires the existing s3pe ScriptResource library
to encode the assembly. The build runs `Check.cs` against isolated game API stubs, checks the script resource
roundtrip and verifies the package index and exact payloads.

The resulting `ApexRadiance_LoadingScriptStudy_test1.package` is a diagnostic tool, separate from the public ASI.
Install it in the game's `Mods/Packages` with the game and launcher closed. Remove it after collecting the report.
It replaces no game resources, resumes no task and changes no loading flags. Live task enumeration, observation
cost and startup through the tuning loader still require in-game validation.

After a world loads, a task checks for the dialog once per second, including while modal windows or the simulator
are paused. Snapshots begin when the dialog appears; suspended stacks are collected every ten seconds while it
remains. The observer selects at most 64 relevant tasks after inspecting at most 16384 registry entries. Truncation
is reported. A world session ends collection after six snapshots, sixteen report files or one MiB of text.

Reports use the game's script report writer: `ScriptError_*.xml` in the game documents folder, with root
`ApexLoadingScriptStudy`. These are diagnostic snapshots, not evidence that an exception occurred. `Check.cs`
verifies modal-safe scheduling, passive field reads, task selection and stack limits, closure observation, valid XML
and output caps. It does not emulate the game's scheduler or establish a cause for a live loading hang.
