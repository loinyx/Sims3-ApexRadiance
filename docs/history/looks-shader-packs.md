# Looks: shader packs: history

Chronological record of investigations and design decisions. Newest entries at the bottom. The feature page is
[features/looks.md](../features/looks.md#shader-packs).

### 2026-11-14: the 17 built-in ports against their authors' originals

**Context:** with ReShade's compiler in Apex ([looks-fx-loading.md](looks-fx-loading.md)), each of the 17 hand ports
of `third_party/reshade_shaders` could be checked against its author's `.fx` compiled by the same path that runs the
players' files.

**Method:** `tools/looks_fx_check --compare` compiles each original (SweetFX `93ddf39`, prod80 `1c2ed5b`,
reshade-shaders `legacy` `bcb5ba5`, Daodan `f01ddb6`, the commits the ports name; `ReShade.fxh` and `ReShadeUI.fxh` from
`slim` `fd00221`) and runs it and the port on the same Direct3D 9 device at 1920x1080, on four pictures (a gradient,
per-pixel noise, two game screenshots) with four value sets (the defaults, every parameter at a quarter of its range,
at three quarters, and the two in turn): 16 runs per shader. The table gives the largest and the mean difference of any
channel, in 8-bit levels.

| Port | Original | Max | Mean |
|---|---|---|---|
| Levels | Levels.fx | 0 | 0.0001 |
| Technicolor2 | Technicolor2.fx | 0 | 0 |
| RetroTint | RetroTint.fx | 0 | 0 |
| DPX | DPX.fx | 1 | 0.0001 |
| AdaptiveSharpen | AdaptiveSharpen.fx | 1 | 0.0000 |
| Vibrance | Vibrance.fx | 0 | 0 |
| Tint | Sepia.fx | 0 | 0 |
| Tonemap | Tonemap.fx | 0 | 0 |
| Colourfulness | Colourfulness.fx | 1 | 0.0000 |
| LumaSharpen | LumaSharpen.fx | 1 | 0.0029 |
| ContrastAdaptiveSharpen | CAS.fx | 0 | 0 |
| Curves | Curves.fx | 0 | 0 |
| Vignette | Vignette.fx | 1 | 0.0000 |
| Technicolor | Technicolor.fx | 0 | 0 |
| HDR | FakeHDR.fx | 1 | 0.0000 |
| prod80_04_SelectiveColor | PD80_04_Selective_Color.fx | 0 | 0 |
| LiftGammaGain | LiftGammaGain.fx | 0 | 0 |

One case is reported and not counted: Levels with the black point on the white point (both at 191, the "three
quarters" set) scales by 255 / 0.00025, as the `.fx` does to avoid a division by zero; a last-bit difference between
the two compilers then reaches 4 levels on a few pixels of the gradient. With the points apart, Levels is exact.

**Outcome:** every port matches its original within 1 level; no port changed. The ports stay in the ASI (the five
built-in looks use them, and they need no compile at run time).

### 2026-11-14: which packs Apex may carry

**Context:** players asked for every pack of ReShade's official list (`EffectPackages.ini`, branch `list` of
crosire/reshade-shaders: 45 packs). A licence audit found 19 whose licence allows redistribution under Apex's MIT
licence (MIT, BSD 3-Clause, CC0); the others are GPL, carry the author's own terms or state no licence.

**Decision (maintainer):** Apex carries no pack. Every pack is downloaded by the player, on a click, from its author's
GitHub, under its author's licence; the ASI keeps only an index of names. Settings > About credits the packs as
downloaded from their authors, without licence texts (Apex does not redistribute them). The 17 ports and their
licences stay as they are.

### 2026-11-14: the pack index and the downloads

**Index:** `tools/looks_packs_index.py` reads ReShade's list and, through the GitHub API, each pack's branch head, file
tree and licence, and scans every `.fx` for `technique <Name>` (outside comments). It writes
`features/looks_packs_index.h`: per pack its name, author, folder, repository, branch, the commit read, the licence
label, an approximate download size (text files at a third of their size, the rest whole, as a zip of the branch
compresses them), the technique count, its place on ReShade's list, and the `.fx` names with their techniques. No
shader code. 45 packs, 517 `.fx` files, 571 techniques (34 KB of header). One list entry names a branch the repository
renamed (Ann-ReShade, `master` to `main`): the index keeps the default branch.

**Downloads** (`features/looks_packs.cpp`): WinHTTP with the generic user agent `ApexRadiance`, HTTPS only, each
redirect followed by hand and only while every hop stays on github.com, codeload.github.com,
objects.githubusercontent.com or raw.githubusercontent.com. The zip of the pack's branch (at most 64 MB) is read with
miniz 3.1.2 (MIT, vendored in `third_party/miniz`) on a worker thread: `.fx` and `.fxh` go to `Shaders\`, images to
`Textures\`, each path taken from the first `Shaders` / `Textures` folder as ReShade's installer does; at most 5000
files and 128 MB, at most four folders deep, no `..`, absolute or reserved names. The files go into
`<pack>.part`, which replaces the pack's folder in one rename only when everything was written, so a failed update
keeps the old download. ReShade's standard effects (`ReShade.fxh`, which most packs include) are queued first when
they are missing. ReShade's list is read again, once a session, when the player first clicks Download.

**Failures:** 404, 410 and 451 are "This pack is no longer on GitHub" (the pack is unavailable for the session, the
queue goes on); 403 or 429 with a rate limit, and 503, "GitHub is busy; try again in a few minutes"; no connection,
"No connection to GitHub"; a redirect off GitHub, "GitHub sent the download somewhere else". A downloaded pack that
left ReShade's list stays listed as downloaded, with Remove; a pack neither downloaded nor on the list is not shown.
A look that needs a file only a gone pack has falls back to "add this file to the Shaders folder".

**Compile cost:** downloaded packs compile like the player's own files, but a pack's `.fx` compiles only when a look
names it or the player opens *Add a shader* or the Get shader packs card (once a session), so a player with every pack
does not compile 517 files at each start.

**Checks:** `tools/looks_packs_test`: 54 checks, 0 failures (hosts, HTTP failures, unpacking with unsafe paths and the
file cap, the list joined with the index and the downloaded folders, the queue with simulated answers: a pack gone,
GitHub busy, no connection, a failed update that keeps the pack, Stop the rest); one real download, Ptho-FX
(3 KB), through WinHTTP into a scratch folder. `tools/looks_fx_check --packs`: a pack's files are not compiled before
they are wanted, then counted per pack.

**ASI size:** 15 368 192 bytes before, 15 552 000 after (+180 KB: miniz, the downloads, the index, the menu and its 48 texts in 21 languages).

**Outcome:** built; offline checks pass; nothing was checked in the game.

### 2026-11-14: browsing packs compiles nothing

**Context:** opening *Add a shader* or the Get shader packs card compiled every downloaded pack's `.fx` once a session
and kept them all in memory; with Looks off, the card's visit was remembered and the compile ran at the next turn-on.
`Looks::NeededPacks` read the run list, which is empty while Looks is off, so a preset exported then named no pack.

**Design:** a pack's `.fx` compiles only when a look names it or the player adds one of its shaders. The check cache
(`Shaders\.cache\checked.txt`: per file its size, time and techniques, with run here / hidden) fills the list and
"<x> of <y> run here"; while the player browses, the files it does not know are checked once in the background (one at a
time, below normal priority, 20 ms apart, stopping for any new scan request), and each effect is let go. A shader added
from the list joins the run list at once and compiles (its file is asked for). *Check again* checks the packs again.
`NeededPacks` reads the look's .ini and the folders (`Fx::FindFiles`). *Add a shader* recomputes its filtered list only
when its inputs change and lays out only the rows in view (`ApexUi::SkipControlRow` keeps the others' height).

**Measured** (`tools/looks_fx_check --packs` on six packs, 131 `.fx`, 146 listed techniques):

| | Before | After, first browse | After, next sessions |
|---|---|---|---|
| Browse time | 76.0 s (compiles) | 79.1 s (background check) | 62 ms |
| Pack effects kept compiled | 146 techniques | 0 | 0 |
| Private bytes after browsing | 7.1 MB | 4.2 MB | 3.2 MB |
| Largest free block after browsing (from 1205 MB) | 806 MB | 799 MB | 1203 MB |
| A wanted pack file | compiled | | 62 ms (compile cache) |

The largest free block falls by about 400 MB in both first browses (the compiler's own allocations); a later session
does not compile, so it keeps its address space. Not measured: the menu's frame time with hundreds of rows (no ImGui
harness times it).

**Outcome:** kept; offline checks pass; not checked in the game.

### 2026-11-14: the missing shaders' block and renamed GShade files

**Context:** the shaders a look could not run sat behind a "<n> can't run here" disclosure as text lines, with one row
of buttons for all of them; every download button switched to the Library tab. A GShade preset can name a file that no
pack publishes under that name (`MXAO@MXAO 4.0.2 EX.fx`), so `PackForFile` found nothing and the shader stayed missing
although qUINT has the same technique in `qUINT_mxao.fx`.

**Design:** a rounded block under the list, drawn like an open shader's values, with one row per shader and its own
action (*Download*, *Add .fx files…* or a spinner) and a footer with the folder, *Check again* and *Download all*.
Downloads stay on the Look tab; the rows show their progress. `Looks::Packs::FileForTechnique` picks a pack file with
the same technique when no file of the original name exists anywhere: a downloaded pack first, then a file whose name
shares the original's first word, then ReShade's order. The entry keeps its own name (`PresetEntry::alt` holds the
stand-in), so its values come from the look's own section and an exported preset names the pack.

**Outcome:** kept; offline checks (`looks_packs_test`, `shaders_menu_test`) updated; not checked in the game.
