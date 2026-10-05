# Light Diag (lighting snapshot)

The lighting snapshot writes the game's CPU-side lighting state to a text file: every light object the game knows,
every loaded lot lighting manager per story with its rooms and the lights in each room's list, the Level Light Share
and indoor-story diagnostics, and the game's own lamp-formula constants. Players save it as **Lighting snapshot** on
the Report a problem page or with its shortcut; each snapshot goes to its own capture folder. Nothing on screen
changes.

## Status

| | |
|---|---|
| Availability | Released in 2.5.0 as a player capture (Report a problem); always read-only |
| Default | Available whenever Night Lights is installed and the game addresses validate; no switch |
| Menu | System > Report a problem > Save a capture > Lighting snapshot (Save). Developer > Lighting > Collect lighting evidence > Save light diagnostics |
| Configuration | No feature table. Shortcut `[ui] diagnostics_key` in `ApexRadiance.toml` (see [ui.md](../../ui.md#shortcuts)) |
| Source | [`features/light_diag.cpp`](../../../features/light_diag.cpp), [`features/light_diag.h`](../../../features/light_diag.h) |

## The problem

The Light Probe ([light-probe.md](light-probe.md)) shows what the GPU draws at one pixel, but many lighting bugs are
decided earlier, on the CPU: which lamps exist, whether they are on, which rooms and stories list them, which colour the
script gave them, and which constants the game's lamp formula uses. None of that is visible on screen.

## How Apex Radiance solves it

The snapshot walks the game's own structures on the render thread and prints them. It enumerates every light through
the game's light enumerator with a fake visitor, walks the lot lighting tree and every room hash, appends the Level
Light Share diagnostics, and reads the lamp-law globals. Every memory read is guarded, so a stale pointer prints `?` or
0 instead of crashing. The file answers questions such as which lamps reach which story, whether street lamps are lit,
why an enclosed plaza is dark (a room with 0 lights), and the cone of a spotlight.

## Settings

| Control | Where | Effect |
|---|---|---|
| Lighting snapshot, Save | Report a problem > Save a capture | Requests a snapshot on the next frame (`LightDiag::RequestDump`) |
| Shortcut (`[ui] diagnostics_key`; preset `B`, `5` or `F8`) | In game | Same request, edge-triggered through `Hotkeys::Take(Action::Diagnostics)` |
| Include a screenshot (`[ui] capture_screenshot`, default true) | Report a problem | Adds `Screenshot.png` to the folder |
| Save light diagnostics | Developer > Lighting > Collect lighting evidence | Same request; shows the shortcut beside it |
| Record story light samples for the diagnostics | Same card | Arms Level Light Share sample recording (`LevelLightShare::SetDiagArmed`) |
| Diagnostics status | Developer > Lighting | `Ready`, `No world loaded`, or `Saved: N lights, M lot stories, R rooms` |

## Compatibility and interactions

- Hosted by Night Lights: `LightDiag::Init` runs from `NightTerrainRelightPatch::Install` and `LightDiag::OnPresent` is
  called from its `OnPresent` (first thing, before the world-state check). Without Night Lights installed there is no
  snapshot; the Report page notes that the captures need Night Lights on.
- The light enumerator is the same function the lot light bridge calls every 20 frames; calling it from Present is safe
  (same thread as the game's light update).
- The stories section depends on Level Light Share ([level-light-share.md](../night-lighting/level-light-share.md)).
- Typical pairing: a snapshot first, then light captures on the surfaces in question.

## Limitations

- Synchronous on the render thread and large (about 1.9 MB with about 5,700 lights): a visible hitch.
- Light indices (`#i`) change between snapshots and pointers change between sessions. Identify a light by `L<ptr>`, and
  only within one session.
- The stories section covers only the active lot and points within 3.5 m of a light, with at most 4,000 outdoor and
  40,000 indoor records, and swaps its records out: a second snapshot without a new lot solve has none.
- Section labels stay in Portuguese; unknown fields (`flag280`, `x100`, `no_hash24`, flag bits 0x08/0x10/0x80) are
  printed raw.
- The lamp-law globals are read only on Steam 1.67.2 (`GameAddr::IsFixed()`); other builds print
  `(constantes globais: so na versao Steam 1.67.2)`.

## Technical reference

### Flow

1. **Init** (`LightDiag::Init`): resolves `GameAddr::Id::RootGetter` and `EnumLights` (fixed Steam addresses or
   signatures in `framework/game_addresses.cpp`), checks byte 0xA1 at the root getter and its 11 tail bytes at +5, and
   on the fixed build compares the 15 enumerator bytes. On failure Night Lights logs
   `[NightTerrainRelight] Light diagnostics not available on this game version` and the shortcut does nothing. On
   success `g_rootPtrAddr` = the imm32 at root getter +1 (the global holding the lighting root).
2. **Per frame** (`LightDiag::OnPresent`): when the shortcut was pressed or a request is pending, calls `WriteDiag`.
3. **WriteDiag**: root = `*g_rootPtrAddr`, lightMgr = `*(root + 0x1C0)`. Without a light manager it sets
   `No world loaded` and shows the warning notice "Lighting snapshot: load a world first". Otherwise it creates
   `Captures\<date time> Lighting snapshot\` (`Captures::NewFolder`), enumerates lights (up to 200,000) inside `__try`
   with a visitor whose vtable[0] is `VisitLight` (`__fastcall` standing in for `thiscall(visitor, Light*) ret 4`),
   walks the lot tree, appends `LevelLightShare::DiagText()` and `PixelLampDiag`, writes `Lighting snapshot.txt`, logs
   `[LightDiag] Saved: N lights, M lot stories, R rooms`, and calls `Captures::Finish` (log, settings, crash file,
   `About this capture.txt`, notice "Lighting snapshot saved. Open Report a problem to find your files").

### File sections

Labels are the literal strings the code writes; the English meaning follows.

**1. Header:** `Apex Radiance lighting snapshot` / `night level=<f> lightMgr=<ptr> cells=<ptr> counter=<a> / <b>`

| Field | Source | Meaning |
|---|---|---|
| night level | lightMgr+0xF0 (float) | Night level 0..1 (also the mod's night factor) |
| lightMgr | root+0x1C0 | World light manager |
| cells | lightMgr+0x104 | Light cells (32 m grid used by the gathers) |
| counter | cells+0x38 / cells+0x3C (int) | Terrain light rebuild countdowns that Night Lights arms (-1 = idle); see [terrain-relight.md](../night-lighting/terrain-relight.md) |

**2. `==== TODAS AS LUZES (N) | postes da rua (tipo 11, lote 0): X acesos de Y ====`** (all lights). `, ENUMERACAO
FALHOU` is appended if the enumerator faulted. Street lamps = type 0xB with lot id 0; lit = flag 0x20. One line per
light, `#i` = enumeration index:

```
#0 L7F022780 vt=00FF43A8 tipo=7 lote=6C11001BCDB06290 comodo=0 d0=1 flags=90[apagada desabilitada] pos=(964.5 92.1 722.5) raio=0.00 intens=(1.00 1.00 1.00 1.00) cor=(1.00 1.00 1.00 1.00) efetiva=(0.00 0.00 0.00 0.00)
```

| Label | Offset in Light | Meaning |
|---|---|---|
| `L<ptr>` | - | Light object address |
| vt | +0x00 | Vtable = light class: 0xFF42A0 type 3, 0xFF42F8 type 11 (street lamp), 0xFF4570 type 4 (spot), 0xFF4350 type 5, 0xFF43A8 type 7 (window light), 0xFF4408 CircleWindowLight, 0xFF4468 TubeLight; see [light-objects-and-rigs.md](../../engine/light-objects-and-rigs.md) |
| tipo | +0xB0 (int) | Light type |
| lote | +0xC4 : +0xC0 | 64-bit lot id (0 = world) |
| comodo | +0x08 (int) | Room id (0 = outdoors) |
| d0 | +0xD0 (int) | Story of the light (the terrain bake refuses lot lights with +0xD0 != 0) |
| flags | +0x100 (byte) | 0x01 `viva` (alive), 0x02 `celulas` (in the cells), 0x04 `comodo-conhecido` (room known), 0x20 `ACESA`/`apagada` (on/off), 0x40 `habilitada`/`desabilitada` (enabled). 0x08/0x10/0x80 printed in hex only |
| pos | +0x120 (3 floats) | Position (lamp head) |
| raio | +0x130 | Range (40/70/97/100 typical) |
| intens | +0x10 (4 floats) | Intensity |
| cor | +0xF0 | Colour set at creation or by script |
| efetiva | +0xE0 | Effective colour = intensity x colour when on, 0 when off |

**3. `==== LOTES (arvore <ptr>, baldes <n>) ====`** (lots and rooms). Tree = lightMgr+0xD4; buckets tree+0x58, count
tree+0x5C; the node at `buckets[count]` is the end sentinel; each node has its tracker at +0x08 and next at +0x10. For
each tracker, stories -4..7: manager = `*(tracker + 0x6A0 + level*0x1A4)`.

`-- LOTE <id> andar <a> (nivel da arvore <L>) gerenciador=<mgr> sistema=ok|DIFERENTE flag280=<b> qualidade288=<q>`

| Label | Source | Meaning |
|---|---|---|
| LOTE | mgr+0x94 : mgr+0x90 | Lot id |
| andar | mgr+0x88 | The manager's real story (lot load recollects room 0 of all stories with the level-0 tree level; see [level-light-share.md](../night-lighting/level-light-share.md)) |
| nivel da arvore | loop index -4..7 | Slot in the tracker |
| gerenciador | - | Lot lighting manager of that story |
| sistema | `*(mgr) == lightMgr` | Sanity check (`DIFERENTE` = inconsistent) |
| flag280 | mgr+0x280 (byte) | Unknown (1 in all samples) |
| qualidade288 | mgr+0x288 (byte) | Lighting quality, 1 = high (the active lot; Level Light Share uses it as "active lot") |

Rooms: hash at mgr+0x230 (buckets +0x234, count +0x238); node key (room id) +0x00, value (Room*) +0x10, next +0x80.

`   comodo <id> (<ptr>) estado=<s> classe=<c> x100=<x> externo=<e> no_hash24=<h> luzes=<n> (postes da rua a / acesos b, outras c): #i #j L<ptr> ...`

| Label | Source | Meaning |
|---|---|---|
| estado | room+0xF0 | Solve state |
| classe | room+0xF4 | LOD class (class 2: the wall atlas gets `[0x1158B1C]` blur passes) |
| x100 | room+0x100 | Unknown |
| externo | room+0x18 (byte) | 1 for room 0 (outdoors) and for roofless fenced areas |
| no_hash24 | hash node+0x24 | Unknown |
| luzes | room+0xC8 .. +0xCC | The room's light list (vector of Light*), with counts of street lamps (lit) and others; each entry as enumeration index or raw pointer |

**4. `==== ANDARES (luz externa entre andares) ====`** (`LevelLightShare::DiagText`). First line: the Level Light Share
status. Then the sample records collected by the light-eval hook during solves since the previous snapshot: only the
active lot (mgr+0x288 != 0), only points within 3.5 m of the light. Header line `Pontos a ate 3,5 m ... {} registrados
de {} vistos`; a per light/story summary (`-- resumo por luz e andar --`), then `-- pontos --` with: story of the
point `A<n>`, light, type, story of the light (`casa`, -1 = own story or world), light position, point, normal, colour
sum `cor`, the game's wall test `jogo` (1 passed, 0 blocked, -1 not run) and its factor `t`, our factor `nosso` (-1 =
not tested), `lote` (batch flag), `filtro` (culled list), `comodo`, and for crossing points `cruza=(x,z,q) h t chave
baixo=comodo motivo`.

Records are collected only while armed, because they cost time in every lot light solve. The Developer checkbox arms
them; a snapshot also arms them (`g_diagArmed.exchange(true)`). A snapshot taken while unarmed with no records writes
the status line and "No samples: recording them was off (it costs time in every light solve). It is on now: let the lot
relight (or use "Relight lots now") and save the diagnostics again." Records are also cleared when the option is
toggled (`RefreshAllLots`).

The section continues with `==== STORIES INDOORS (indoor lamps through stair openings) ====` (`IndoorDiagText`: floor
objects known and linked to a story through their lot), `==== SEAM (atrium walls at the floor line, N samples) ====`
(`SeamDiagText`: pairs of wall samples just under and just over the floor line of an atrium group, with LOD class, y,
summed light, normalisation and value after the curve, latest solves first) and the solve journal (`JournalText`).

**5. `==== LUZ POR PIXEL (passo 3, incremento 0) ====`** (per-pixel lamp light; `PixelLampDiag`). The values a
per-pixel lamp term must copy from the game's own wall and floor light solve:

```
k1 [0x11D0A60]=1 k2 [0x11D0A68]=0.075 Cmax [0x11D1160]=(2 2 2 2) poste [0x1158DA8]=3.3333 tipo5 s [0x11D11A0]=5
desfoque das paredes: passadas [0x1158B1C]=2 modo [0x11D02E4]=0
-- lote 4522001BE6BBCA40 andar 0 comodo0=3DDFE700 classe=0 limiar[+0x63C]=0.0235294 matriz[+0xF8]=(0.9397 0 0.342 0 0 1 0 0 -0.342 0 0.9397 0 896.7 60.53 1232 1)
-- luzes nas listas do comodo 0 (64): alcance +0x130, forca +0x10.x, cores E0/F0, cone
L7D515AB0 #5666 vt=00FF4570 tipo=4 listas=6 pos=(882.4 63.55 1188) alcance=30 forca=(1 1 1 1) E0=(0.85 0.8625 1 0.85) F0=(0.85 0.8625 1 0.85) cone: eixo[+0x170]=(0.005923 -0.9999 -0.01627) desloc[+0x158]=-0.3211 escala[+0x154]=4.902
```

| Item | Address / offset | Meaning | Typical value |
|---|---|---|---|
| k1 | `[0x011D0A60]` float | Divisor of the lamp weight | 1 |
| k2 | `[0x011D0A68]` float | Lamp weight factor: W = +0x130 x (+0x10).x x k2 / k1 | 0.075 |
| Cmax | `[0x011D1160]` 4 floats | Per-lamp clamp, `min(Cmax, W*NdotL/d^2)` | (2 2 2 2) |
| poste | `[0x01158DA8]` float | Street-lamp factor (the x3.333 of the ground) | 3.3333 |
| tipo5 s | `[0x011D11A0]` float | Cone scale of type 5 lights | 5 |
| passadas | `[0x01158B1C]` uint32 | Wall atlas blur passes (class-2 rooms) | 2 |
| modo | `[0x011D02E4]` byte | Blur branch: 0 = separable [1 2 1]/4 rounded up; else 2x2 box with half-texel shift | 0 |
| classe | room0+0xF4 | LOD class of room 0 | 0 or 2 |
| limiar | room0+0x63C float | Per-light threshold (applied at 0x69FE19..0x69FE4D) | 0.0235294 (class 0), 0.0117647 (class 2) |
| matriz | `*(float**)(room0+0xF8)`, 16 floats | Lot-to-world matrix, row-vector: world = x*m[0..3] + y*m[4..7] + z*m[8..11] + m[12..15] (`nula` when absent) | Rotation + lot origin |

The light list after it holds every light found in any room 0 of any story of any loaded lot, deduplicated; `listas` =
in how many room-0 lists it appears. Fields: `#index` (or `-`), vt, type, pos +0x120, `alcance` +0x130, `forca` +0x10
(4 floats), E0 +0xE0, F0 +0xF0, and the cone (classes resolved through `GameAddr::Id::LightVtable4` / `LightVtable5`):

| Class | vt (Steam) | Cone fields |
|---|---|---|
| Type 4 spot | 0x00FF4570 | Axis `eixo[+0x170]` (3 floats), offset `desloc[+0x158]`, scale `escala[+0x154]` |
| Type 5 | 0x00FF4350 | `a1[+0x1A0]` (3), `o1[+0x174]`, `a2[+0x190]` (3), `o2[+0x170]`, `S[+0x150]` (3) |

Spotlights (type 4) typically have axis about (0, -1, 0), offset -0.32 and scale 4.9. Without a lot tree the section
prints `(sem arvore de lotes)`.

**6. Footer:** `End: <N> lights, <M> lot stories, <R> rooms`.

### Game addresses and patterns

| Address (Steam) | What | Verification |
|---|---|---|
| 0x006E97B0 | Root getter: `A1 <imm32> 85 C0 75 01 C3 8B 80 C0 01 00 00` (`mov eax,[g]; test eax,eax; jnz; ret; mov eax,[eax+0x1C0]`) | Byte 0xA1 at +0 and the 11 tail bytes at +5; root global = imm32 at +1. Night Lights' own `Install` makes the same check ("Light manager code differs at 0x6E97B0") |
| 0x006ACF70 | Light enumerator, `stdcall(visitor*)`; visitor vtable[0] = `thiscall(visitor, Light*) ret 4`; bytes `E8 2B 36 00 00 8B 4C 24 04 51 68 40 CF 6A 00` | memcmp on the fixed build; decompile `FUN_006b05a0(); FUN_006b0710(&LAB_006acf40, param_1);` |
| 0x011D0A60, 0x011D0A68, 0x011D1160, 0x01158DA8, 0x011D11A0 | Lamp-law globals (section 5) | Read only |
| 0x01158B1C, 0x011D02E4 | Wall blur pass count and branch flag | From the decompile of 0x0069F650 |

Structure offsets for Light, lot manager, room and tree are in the tables above; the engine side is in
[room-light-maps.md](../../engine/room-light-maps.md) and [light-objects-and-rigs.md](../../engine/light-objects-and-rigs.md).

### Files and functions

| File | Symbol | Role |
|---|---|---|
| `features/light_diag.h` | `LightDiag::Init`, `RequestDump`, `OnPresent`, `Status` | API |
| `features/light_diag.cpp` | `WriteDiag` | Sections 1-3 and 6; calls the others |
| | `PixelLampDiag` | Section 5 |
| | `EnumerateLights`, `VisitLight`, `g_visitor` | Game enumerator with a fake visitor |
| | `LightLine`, `Vec4`, `Floats`, `SafeCopy`, `Rd<T>` | Formatting and guarded reads |
| `features/level_light_share.cpp` | `DiagText`, `Diag`, `IndoorDiagText`, `SeamDiagText`, `JournalText`, `SetDiagArmed` | Section 4 |
| `patches/night_terrain_relight_patch.cpp` | `Install` (`LightDiag::Init`), `OnPresent`, `DrawDeveloper` (button, checkbox, status) | Host |
| `apex_gui.cpp` | Report page, Lighting snapshot row | Player request |

`patches/light_diag_patch.cpp` is a different, untracked leftover from the combined build; it is not part of this
repository or project and must never be committed (see [history](../../history/dev-tools-light-diag.md)).

## Rejected approaches

- A second diagnostics writer in `patches/light_diag_patch.cpp`
  ([history](../../history/dev-tools-light-diag.md#2026-09-25-two-writers-for-the-same-shortcut)).
- Identifying lights by enumeration index
  ([history](../../history/dev-tools-light-diag.md#2026-09-25-passo3-snapshots)).
- Forcing high lighting quality on every lot to fix the lot-edge cut
  ([history](../../history/dev-tools-light-diag.md#2026-09-24-first-snapshots)).
- Recording story samples in every solve
  ([history](../../history/dev-tools-light-diag.md#2026-09-29-story-samples-only-while-armed)).

## See also

- [Validation](../../validation/dev-tools-light-diag.md)
- [History](../../history/dev-tools-light-diag.md)
- [Light Probe](light-probe.md), [Report a problem](../bug-reports.md)
- [Level Light Share](../night-lighting/level-light-share.md), [lamp colour](../night-lighting/lamp-colour.md)
