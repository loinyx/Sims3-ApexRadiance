# Looks: the players' own .fx shaders: history

Chronological record of investigations and design decisions. Newest entries at the bottom. The feature page is
[features/looks.md](../features/looks.md).

### 2026-11-14: feasibility of compiling ReShade FX for Direct3D 9

**Context:** Looks ran only the 17 hand ports of `third_party/reshade_shaders`. Players have their own ReShade and
GShade packs; a look that names one of their techniques showed it as *can't run here*. The earlier converted-effect
package (`GShadeLooks.pak`, rejected in the feature page) had shown that ReShade's own D3D9 code generator produces
shader model 3.0 code the game's device runs; it was converted offline and could not load a player's files.

**Compiler vendored:** ReShade's effect compiler from [crosire/reshade](https://github.com/crosire/reshade) at tag
`v6.7.3` (commit `4a50d1eddace85734871d91792ff214f13f66c01`), BSD 3-Clause, in `third_party/reshadefx/` with its
`LICENSE.md`:

| Files | Role |
|---|---|
| `effect_lexer.{hpp,cpp}`, `effect_token.hpp` | Lexer |
| `effect_preprocessor.{hpp,cpp}` | C-style preprocessor |
| `effect_parser.hpp`, `effect_parser_exp.cpp`, `effect_parser_stmt.cpp` | Parser |
| `effect_symbol_table.{hpp,cpp}`, `effect_symbol_table_intrinsics.inl` | Symbol table and intrinsics |
| `effect_expression.{hpp,cpp}` | Expressions and constants |
| `effect_codegen.hpp`, `effect_codegen_hlsl.cpp` | Code generator interface and its HLSL back-end |
| `effect_module.hpp` | The effect module (textures, samplers, uniforms, techniques, passes) |

Not vendored: the GLSL, SPIR-V, DXBC and DXIL back-ends and `com_ptr.hpp` (Apex calls `D3DCompile` itself, see below).

**Changes to the vendored files** (every other line is as at the tag):

- Building with MSVC C++20 x86: no source change. The files are compiled with `/Zc:char8_t-` and
  `_SILENCE_CXX20_U8PATH_DEPRECATION_WARNING` (per file in `ApexRadiance.vcxproj`), so `path::u8string()` returns
  `std::string` as in the C++17 build ReShade uses. They build without warnings at `/W3`; no warning is silenced.
- `effect_preprocessor.hpp/.cpp`, for untrusted input: an optional `include_filter` callback checked before an
  `#include` is read (Apex: the path must be inside the Shaders folder, without `..`, at most 2 MB, at most 64 includes per
  effect); `max_output_size` and `max_tokens` caps checked in the main loop (Apex: 16 MB of output, 20 million tokens),
  so a macro that expands without end stops with "the file expands to too much code".

**Finding: the vendored compiler targets shader model 3.0.** `create_codegen_hlsl(30, ...)` is the path ReShade's own
Direct3D 9 runtime uses (`runtime.cpp`: shader model 30 when the renderer is `0x9000`): uniforms in float constant
registers from `c0`, one per uniform, `__TEXEL_SIZE__` at VS `c255` for the half-pixel offset, `<SEMANTIC>_PIXEL_SIZE`
from `c223` down, combined samplers, `SV_VertexID` as a float `TEXCOORD0`, shifts emulated with `exp2`. Apex compiles each
entry point a technique uses with `D3DCompile` (`vs_3_0` / `ps_3_0`), with the macros ReShade 6.7.3 defines on D3D9
(`__RESHADE__ 60703`, `__RENDERER__ 0x9000`, `BUFFER_WIDTH` / `BUFFER_HEIGHT` of the picture, `BUFFER_COLOR_BIT_DEPTH 8`, ...)
and the compatibility macros ReShade prepends (`tex2Doffset`, `tex2Dgather`). Depth is ReShade's `DEPTH` semantic with
`ReShade.fxh`'s defaults (`RESHADE_DEPTH_*` not defined), fed by Apex's DepthShare (INTZ, raw device depth, as ReShade's).

**Direct3D 9 fallbacks** (tried before a technique is refused; each one used is logged and printed by the tool):

| Missing on D3D9 | Fallback | Rescued in the test packs |
|---|---|---|
| RG8 render target | RGBA8 | SweetFX SMAA (`edgesTex`) |
| RG11B10F | RGBA16F | none in the packs |
| R32I / R32U / RGBA32I / RGBA32U | R32F / RGBA32F (shader model 3.0 reads every texture as float; exact up to 2^24) | a test file (`tex2Dfetch` of an `R32U` target) |
| 1D texture | 2D texture of height 1 (`sampler1D` reads it) | none in the packs |
| L8 / L16 render target not offered by the GPU | RGBA8 / G16R16 (checked with `CheckDeviceFormat`) | not needed on the test GPU |
| `D3DCompile` refuses at optimisation level 3 | flow control kept, flow control avoided, level 1, no optimisation, in that order | none in the packs |

Final failures (no Direct3D 9 equivalent): compute shaders and storage writes, more than 4 render targets in a pass,
3D render targets, integer bitwise operators in `ps_3_0` (the generator emulates shifts only), a texture semantic other
than `COLOR` / `DEPTH`, and missing texture files.

**Test packs** (scratch folder laid out like `Apex Radiance\Shaders\`, 1920x1080, `tools/looks_fx_check --run`: compiled,
then every runnable technique created and run once on a real Direct3D 9 device on a test gradient with its defaults):

| Pack | .fx files | Techniques | Run on D3D9 | Picture changed with the defaults | Read depth |
|---|---|---|---|---|---|
| AlucardDH/dh-reshade-shaders (`master`, Shaders) | 13 | 15 | 14 | 8 | 8 |
| CeeJayDK/SweetFX (`master`) | 28 | 31 | 31 | 20 | 1 |
| crosire/reshade-shaders `slim` (incl. `ReShade.fxh`, `ReShadeUI.fxh`) | 5 | 6 | 5 | 4 | 2 |
| crosire/reshade-shaders `legacy` | 20 | 29 | 29 | 15 | 10 |
| prod80/prod80-ReShade-Repository (`master`) | 29 | 30 | 30 | 21 | 6 |
| luluco250/FXShaders (`master`) | 36 | 37 | 37 | 26 | 5 |
| **Total** | **131** | **148** | **146** | 94 | 32 |

"Picture changed" is a hint only: many effects leave a plain gradient unchanged with their defaults (sharpening,
anti-aliasing, depth effects without depth). None failed to create or run.

The two that do not run:

| File | Technique | Reason |
|---|---|---|
| `dh_ahoh.fx` | DH_AhOh | Bitwise operations (`&`, `|`) on integers; `ps_3_0` has none and the generator does not emulate them |
| `UIMask.fx` (slim) | UIMask_Bottom | Needs `UIMask.png`, which the pack leaves to the player |

Safety files (scratch, all refused without a crash): an absolute `#include` and a `../../` one ("#include not allowed"),
a recursive include, a 3 MB file, two macro expansion bombs (one that grows the output, one that expands to nothing
10^9 times), a compute shader and a five-target pass. Each is refused in under 7 s together.

**Cost:** compiling the 131 files took 75 s on one thread (`dh_uber_rt.fx` 17 s, `DOF.fx` 11 s, `dh_anime.fx` 9 s,
most under 0.5 s). The largest texture memory is `dh_uber_rt.fx` at 1920x1080: 211 MB. In the game the compile runs on a
background thread at below-normal priority, once per file per session and picture size.

**Outcome:** kept. The compiler targets shader model 3.0 and nearly every effect of the common packs compiles and runs
on a Direct3D 9 device; Phase 2 (runtime) and Phase 3 (menu) went ahead. Nothing was checked in the game yet.

### 2026-11-14: runtime and menu

**Context:** the compiled effects run in Looks' run list beside the built-in ports.

**Design:**

- `features/looks_fx.cpp` compiles one file (any thread) and runs one technique on a device the way ReShade 6.7.3's D3D9
  runtime does (adapted from the contributor's removed `gs_fx` runtime): `COLOR` copied from the picture at the start
  and after a pass that wrote it, render targets cleared when created, stencil for full-size passes, auto-generated mips,
  vertex ids as a float stream, uniforms in PS and VS constants, `__TEXEL_SIZE__` per viewport. Texture files are read
  and decoded with WIC when an effect is created on the device (once, and again after a device Reset), never per frame.
- `features/looks_fx_catalog.cpp` scans `Apex Radiance\Shaders\` (`.fx` in it and in `Shaders\` up to three folders
  deep, at most 500) on a background thread. Each file compiles on its own thread with a 16 MB stack, SEH around it and a
  two-minute limit: a file that crashes or hangs the compiler is refused, never the game. Unchanged files are kept for
  the session; results are published after each file, so shaders appear as they compile. The render thread adopts a
  result at Present and Looks looks its run list up again.
- User techniques are shader indices from `kUserShaderBase` in the same table as the ports (`Looks::Shader`), with
  `LooksParam` entries built from the uniforms (`ui_label`, `ui_min`, `ui_max`, `ui_type`, `ui_items`; `source`,
  `hidden`, matrices and arrays are not sliders). Presets, tweaks, run order and *Save as a new look…* work unchanged.
  A built-in port of the same technique and file wins.
- `BUFFER_WIDTH` / `BUFFER_HEIGHT` are compile-time constants in ReShade: an effect compiled for another size waits
  while the folder compiles again for the new one.
- Caps: 256 MB of textures per effect, 384 MB for all running user effects, 64 textures per effect, 64 passes per
  technique, a million vertices per pass, 256 user passes per frame, 224 constant registers.
- The state around a run with a user effect is saved in full (all render states it sets, 16 samplers, 224 PS and 256 VS
  constants, render targets 1 to 3), on top of the `D3DSBT_ALL` state block.

**Menu (the chosen design):** a *Shader library* card under the look's cards (Built into Apex, Your shaders
with *Add .fx files…* and the folder); *Add a shader* grouped by source with collapsible headers and counts, only
shaders that run here; the *can't run here* lines say what to do (add the file, or "doesn't run in The Sims 3" without
detail), with *Add .fx files…*, *Open the Shaders folder* and *Check again* when a file is missing. Reasons stay in the
log and in the developer build's *Live counters*.

**Outcome:** built (0 errors, 0 warnings); offline checks pass (`tools/looks_fx_check`); not checked in the game.

### 2026-11-14: performance and memory pass

**Context:** the per-frame cost of a look, the work done while Looks is off, and the compiles and memory of browsing
shader packs, with one rule: the picture must not change.

**Changes:**

- Device state: `SavedState` puts back by hand exactly what the passes change (the lists moved to
  `features/looks_runner.h`). The built-in passes' PS constants are saved up to the highest register their parameters
  use (`Looks::PsConstsUsed`): PD80 Selective Color writes up to c63, which the hand restore of c0..c31 left to the state
  block. The `D3DSBT_ALL` state block, captured and applied at every run point (up to three a frame), is now used only
  when a getter fails (RTX Remix). The VS constants of a full save live in a fixed array instead of a vector per run.
- *Strength* 0 (a blend factor that rounds to 0) skips the chain: the write-back was destination x 1 + source x 0.
- No allocation per frame in the run path: the step list is kept, the status line is appended in place and compared
  without copies, the render-state cache of `Fx::Instance::Run` is on the stack.
- Compile cache: `Fx::Compile` keeps D3DCompile's results in `Shaders\.cache\fx_<path hash>_<W>x<H>.bin`, keyed by the
  generated HLSL of every entry point, the target and D3DCompiler's version.
- No shader is skipped for neutral values: none of the ports is an exact identity at its defaults.
- With Looks off nothing was hooked or polled already (hooks are registered at Install; the folder is read on request
  only), so no folder watcher was added: a watcher would be new work.

**Measured** (`tools/looks_fx_check`, Direct3D 9 HAL, 1920x1080, the six packs of the first entry):

| What | Before | After |
|---|---|---|
| Device state not put back by hand after the 17 ports (`--state`) | 32 PS constants (c32..c63, left to the state block) | 0 of 118 changed states |
| ... after the ports and 24 techniques of players' effects | | 0 of 588 |
| `D3DSBT_ALL` Capture + Apply per run point (CPU) | 26 to 28 us, up to 3 a frame | 0 (only when a getter fails) |
| The chain at Strength 0: Natural / all 17 ports (CPU + GPU, flushed) | 1.4 to 1.6 / 1.4 to 3.7 ms a frame | 0 |
| Compiling the 131 .fx files | 76.9 s | 76.9 s the first time, 0.33 s after |
| `dh_uber_rt.fx` | 17.8 s | 16 ms from the cache |

`--cache`: the 131 effects compiled with and without the cache have the same bytecode, notes and reasons; a test effect
compiles again after its `.fxh`, its `.fx` or the picture size changed and is read from the cache otherwise.

**Picture:** `--compare --hashes` (17 ports and their originals, four value sets, gradient, noise and two game
screenshots: 272 pictures) and `--run --hashes` (146 techniques of the six packs) print the same hashes when built from
the sources before the change (`/DLOOKS_FX_CHECK_BASELINE`) and after it: the same pixels.

**Outcome:** kept; built (0 errors, 0 warnings); offline checks pass; not checked in the game (the state block's share
of the GPU driver's work and the frame time in the game are not measured).

### 2026-11-14: a definition the file overrides recompiled it at every scan

**Context:** `MacrosMatch` compared the values the compiler ended with. A look definition that the `.fx` overrides with
its own `#define` never matched, so the file compiled again at every scan, and each new effect was created again (at
4K while packs were checked in the background).

**Finding:** the definitions asked for, not the ones the preprocessor ended with, decide whether a compiled effect is
still current.

**Outcome:** changed: an effect keeps the definitions it was compiled with (`Effect::compiledWith`), and those count as
a match.

### 2026-11-14: World only ran under the game's bloom

**Context:** with *World only*, the world looked lighter and less saturated than with *World and menus*; session logs
had no "after the game's bloom" line.

**Finding:** a frame whose after-bloom part ran at the scene boundary (no bloom strip the frame before) also stopped
watching for the strip, so the next frame expected none either. From the first such frame on, every look ran at the
boundary, under the game's bloom.

**Outcome:** changed: `OnDraw` watches for the strip in every frame and runs the look there when it has not run yet.
Not yet checked in the game ([validation](../validation/looks.md#open-checks)).
