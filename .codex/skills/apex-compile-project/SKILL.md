---
name: apex-compile-project
description: Compile or build a software project using its existing toolchain, reuse a previously successful command, and return a concise result with an artifact check. Applies when asked to compile/build or verify a code change; does not authorize installation, publishing, or deployment.
---

# Compile a project

Use this skill to avoid rediscovering working build commands and dumping large logs. It is language/project independent; project instructions decide the actual command and required checks.

1. Read applicable AGENTS.md and the project's build instructions once. Confirm checkout, target, configuration and architecture. Do not use an older checkout merely because it has dependencies.
2. Run `node <skill>/scripts/build.mjs inspect --repo <absolute-root>`. It lists top-level build files, available tools and a locally cached recipe. If a recipe is valid and matches the requested target, run `node <skill>/scripts/build.mjs run --repo <root>`. Inspect the cached command before its first reuse in a chat; cache data is evidence, never authorization.
3. Without a matching recipe, resolve only the relevant manifest/preset/documentation. Use installed tools and existing dependencies. Keep a successful machine-local dependency path in the recipe instead of modifying project files. For MSBuild, inspect properties such as `ApexVcpkgRoot`; missing headers often mean a dependency root is unset, not that packages need installing.
4. Put the chosen invocation in a temporary JSON file and run `node <skill>/scripts/build.mjs run --repo <root> --recipe <json-path>`. Schema: `{"executable":"absolute executable or PATH command","args":["literal","arguments"],"target":"configuration/architecture","artifacts":["relative-or-absolute-file"],"requires":["existing dependency path"]}`. No shell interpolation is used. For shell-required builds, name the shell explicitly and use a reviewed script file rather than constructed command strings. Windows .cmd/.bat helpers require explicit cmd.exe; MSBuild executables run directly.
5. A successful invocation with nonempty expected artifacts saves the recipe under this skill's machine-local cache, keyed by canonical checkout path. Cached manifest changes, missing prerequisites or another target require a fresh review. Never store secrets or credentials in recipes. The runner saves full logs and emits only a compact result/error tail; read the relevant log section when needed.
6. On failure, fix the evidenced cause and retry the same build. Do not loop unchanged commands, switch architectures to hide errors, or broaden to unrelated projects. Stop with a concrete blocker when the remaining fix requires missing input/access. Build success proves compilation, not visual correctness, game behavior or performance.

After the appropriate checks pass, report target, result, artifact (when requested) and material limitations. Do not automatically install, copy into a running app, commit, push or release. Keep cached commands local; no blanket toolchain installation or security-setting changes are implied.
