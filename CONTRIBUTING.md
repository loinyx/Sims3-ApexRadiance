# Working on Apex Radiance

Read `AGENTS.md` and `CLAUDE.md`, then the relevant feature, engine, validation and history pages. Existing rejected approaches require new evidence before another attempt. Back up existing source/game/config files before modifying them according to the project workflow. Do not commit proprietary executables, shader bytecode, captures, credentials or build output.

## Reproducible Windows build

Install Visual Studio 2022 C++ Build Tools with MSVC v143 and a Windows SDK, Git and Node. From the repository root in PowerShell:

```powershell
./tools/development/setup.ps1
./tools/development/build.ps1 -Flavor Public
./tools/development/build.ps1 -Flavor Developer
./tools/development/check.ps1 -Suite Smoke
```

Setup uses the exact vcpkg revision in `vcpkg.json` and `x86-windows-static`. It downloads dependencies but does not install the mod into the game. An existing vcpkg checkout at another revision is refused rather than reset. Builds retain static CRT, C++20 and x86. Public artifacts omit developer tools; developer artifacts remain local. The build helper records the validated machine-local recipe.

`-Suite Full` runs the same three CPU harnesses at their full default coverage. These cover DXT, RefPack and CAS triangle sorting; they do not represent the full project's tests. Read each feature's validation page and run its relevant harnesses before changing it. D3D9 HAL tests and actual gameplay remain separate from the hosted CI job.

The GitHub build workflow checks PRs and main/codex branch pushes. Downloaded CI artifacts are test builds, not automatically published releases. Existing Nexus workflows still run when a release is published.

## Continuity across agents, chats and accounts

The shared source of truth is the branch and its PR, including the Agent handoff section. Record the exact tested SHA, results, decisions and remaining checks. A shared chat snapshot does not synchronize future messages or transfer access to this machine or GitHub credentials. Each account must obtain its own authorized access.

Before takeover: inspect the current PR head, review/check status and local dirty files. Do not overwrite another agent's work. For simultaneous work, use one worktree and contribution branch per agent, agree on file ownership and assign one integrator for the shared PR. Never force-push away other work.

Use coherent commits, maintain one draft PR for the task and update its handoff as work progresses. Publishing a release requires the repository's review skill, recorded provenance and explicit authorization; successful CI alone does not establish visual correctness or gameplay performance.

## Reverse engineering

Keep the local corpus under an ignored output directory or another private research folder. Identify executables and shader archives by SHA256 and build/version. Prefer the existing engine documentation and recover old research before recreating it. Work from the local Steam executable and read-only shader archive; preserve the project's decision not to dump the decrypted EA executable.

Use a reproducible scene and diagnostic captures before proposing a patch. Confirm calling conventions, register/state ownership and expected original bytes. Resolve non-Steam locations by validated signatures. Keep unknown facts explicitly unverified. Run the affected offline harnesses, then compare native D3D9 and DXVK in game when the change touches rendering.

Install the official Ghidra release and its required JDK 21. Set `JAVA_HOME` to that JDK if Ghidra cannot discover it. The following recipes keep generated game data under the ignored `outputs/research` directory:

```powershell
./tools/development/research.ps1 -GameBin 'C:\path\to\The Sims 3\Game\Bin'
./tools/development/research.ps1 -GameBin 'C:\path\to\The Sims 3\Game\Bin' -GhidraRoot 'C:\path\to\ghidra' -Analyze
./tools/development/research.ps1 -GameBin 'C:\path\to\The Sims 3\Game\Bin' -GhidraRoot 'C:\path\to\ghidra' -Export
```

Analyze imports the verified Steam 1.67.2 executable without modifying it; repeating an import refuses to overwrite the existing project. Headless analysis uses a configurable Java heap (`-MaxMemoryGB`, 24 GB by default). The 2 GB and 8 GB imports exhausted the heap in Ghidra analyzers. The initial profile disables the x86 constant reference analyzer, and the recipe now rejects any import log reporting an out-of-memory error or failed import before export. The recovered 8 GB project indexed 38,878 functions and decompiled 15 selected engine functions, but the failed import means those results are provisional; inferred cross-references and analyzer coverage need review. Export reopens the completed project read-only, writes a function index and decompiles 15 documented engine entry points. Generated names, types and control flow require manual verification; this is a replacement research baseline, not recovery of previous analyst annotations.
