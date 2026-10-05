# Game builds and game-code addresses

This page documents how Apex Radiance finds the game code it patches on every build of The Sims 3: the fixed addresses of Steam 1.67.2 (`TS3W.exe`) and a runtime signature scan for every other build, first of all the EA app 1.69.47 (`TS3.exe`). Every feature that patches or calls game code depends on it.

## Scope

| | |
|---|---|
| Game build | Steam 1.67.2 (`TS3W.exe`, image base 0x00400000, fixed addresses); EA app 1.69.47 (`TS3.exe`) and other builds through the signature scan |
| Used by | Every game-code feature: [Night Lighting](../features/night-lighting/README.md), [Every-Story Ground Light](../features/night-lighting/level-light-share.md), [Performance](../features/performance/README.md), [Light Diag](../features/dev-tools/light-diag.md), S3SS detection |
| Evidence | `framework/game_addresses.{h,cpp}`, `framework/game_version.{h,cpp}`, offline checker `S3SS-dev\research\port169\sigcheck.pl`, research notes `S3SS-dev\research\port169\method.md` |

## Overview

The page covers the EA app DRM finding, the decision not to dump the decrypted game, how resolution works, what is logged, the full
signature table, which features need which addresses, and what is still unverified on EA.
Code: `framework/game_addresses.{h,cpp}` (table and resolver), `framework/game_version.{h,cpp}` (build detection),
the features that use the addresses (`patches/night_terrain_relight_patch.cpp`,
`patches/split_level_ground_light_patch.cpp`, `features/object_light_bridge.cpp`, `features/level_light_share.cpp`,
`features/rig_tracker.cpp`, `features/lot_light_bridge.cpp`, `features/light_diag.cpp`, `framework/s3ss_detect.cpp`,
and the Performance and Frame Profiler sources in `features/`, such as `resource_cache.cpp`, `lot_lighting_motion.cpp`,
`scene_budget.cpp`, `object_index.cpp`, `fast_memory.cpp`, `fast_crc.cpp`, `unlit_rooms.cpp` and `frame_profiler.cpp`; the
loaded-world gate `features/world_session.h` uses `WorldManagerPtr` and `UiServiceGetter`).
Offline checker: `S3SS-dev\research\port169\sigcheck.pl`. Research notes: `S3SS-dev\research\port169\method.md`.

## Details

### Builds

| Build | Exe | PE timestamp | `GameVersion` | Game-code addresses |
|---|---|---|---|---|
| Retail disc 1.67.2.024002 | TS3W.exe | 0x52D872DA | `Retail` | signature scan |
| Steam 1.67.2.024037 | TS3W.exe | 0x52DEC247 | `Steam` | fixed (verified in `S3SS-dev\re\TS3W.exe`), signatures logged as a self-check |
| EA app 1.69.47.024017 | TS3.exe | 0x6707155C | `EA` | signature scan |
| Origin / EA 1.69.43.024017 | TS3.exe | 0x568D4BAC | `EA_1_69_43` | signature scan |
| anything else | | | `Unknown` | signature scan |

`DetectGameVersion` reads the timestamp of the main module. Display features (`VERSION_ALL`) run on every build. The
game-code features stay registered with `supportedVersions = VERSION_STEAM` plus a `gameCodeGroup`
(`FeatureInfo::gameCodeGroup`): on another build they can be switched on when their group of addresses was found
(`ApexPatch::IsCompatibleWithCurrentVersion`), otherwise the menu says "Not available on <version>: missing <names>"
(`ApexPatch::UnavailableReason`) and nothing is written.

### The EA app build: encrypted code, no dump

`TS3.exe` 1.69.47 carries an extra section `.ooa` (EA Origin/EA-app online activation; entry point 0xEB4000 inside it,
references `Core/Activation.dll`). Its `.text` is encrypted on disk: 8.000 bits/byte entropy with a flat histogram and a
single `55 8B EC` in 12 MB (Steam `TS3W.exe`: 6.62 bits/byte, 4437). The activation stub decrypts it in memory at process
start. `.rdata` and `.data` are plain on disk (strings, vtables and tables can be read statically; code cannot). Section
layout and "file offset = VA - 0x400000" hold for `.text/.rdata/.data` in both exes. Details and the byte statistics:
`research\port169\method.md`.

The deltas between Steam and EA addresses taken from official Sims3SettingsSetter (which supports EA by pattern) are not
constant (-0x10 near 0x404573, -0x130 at 0x4D58B0, -0x460 at 0x567460, -0x6F0 at 0x58DD5E, -0xC80 at 0xC6D570), and the EA
`.text` is 0x49DC9 bytes longer: every address needs its own match, a per-region offset cannot work. The small deltas and
S3SS's patterns matching both builds suggest the same compiler and mostly the same code generation (unverified for the
functions below).

**Decision (2026-09-28): no dump of the decrypted image.** Writing the decrypted game to disk would create an
unprotected copy of it. Instead the mod scans its own process memory: at the point where it installs the features (first
Present + 1 s, see `apex_main.cpp`) the stub has long decrypted `.text`. Nothing is written besides the normal log lines;
at most 16 bytes per match are logged, never a memory region. The price: the EA matches cannot be reviewed offline before
shipping, so every signature logs enough to be refined from a player's `ApexRadiance_LOG.txt`.

### How addresses are resolved

`GameAddr::Resolve()` runs once on the init thread after `WaitForSettle()` and before `ApexConfig::LoadFeatures()`:

1. `.text` of the main module from its in-memory PE headers, split into readable ranges (`VirtualQuery`).
2. Non-Steam builds: if `.text` has fewer than 1000 `55 8B EC` it is taken as still encrypted: wait 1 s and count again,
   up to 15 times (a warning each time).
3. Every entry of the table ([Address reference](#address-reference)), in order (dependencies first). A signature is an IDA-style masked pattern
   (`??` = any byte); rel32 call/jump targets, absolute addresses (globals, vtables, constants) and anything relocation
   dependent are always wildcards.
   - an address counts only when its signature matches **exactly once**; for signatures that read a value (a call
     target, or a dword such as a global) several matches are accepted when they all give the same value;
   - each entry has a primary and an alternate signature; the alternate is tried only when the primary is not unique;
   - the result must lie in `.text` (code) or in the image (globals, vtables);
   - kinds: `Sig` (match + offset, or the target of the CALL there, or the dword there), `Multi(n)` (exactly n matches,
     consecutive ids by address), `InRange(dep, n)` (the signature searched only in the n bytes from another entry, e.g.
     a call inside a function found before), `CallIn(dep, n)` (after the match, the CALL within n bytes that lands on
     another entry), `LowestOf2` (twin functions, the lower one), `Deref(dep, off)` (the dword at another entry + off:
     globals from an instruction's operand, vtable slots), `Target(dep)` (the target of the CALL at another entry, the own
     signature as fallback), `CallersOf(dep, n)` (every CALL in `.text` to another entry: exactly n), `LightType(t)` (the
     light factory's jump table -> the constructor of light type t -> the vtable it stores), `SlotsOf(dep, n)` (every
     4-aligned dword equal to another entry in the read-only data sections, i.e. its vtable slots: exactly n; added
     2026-09-28 for the Frame Profiler counters).
4. Cross-checks (non-Steam: a failed check drops the ids concerned): the nine light vtables are distinct and share one
   position function (`+0x24`), the batch solve call is one of the three point-solve calls and calls the point solve, the
   story refresh calls follow the cascade test in order.
5. Steam 1.67.2: the scan runs the same way but only as a self-check. `GameAddr::Get` always returns the fixed address;
   a signature that disagrees is logged as `DIFFERS: the fixed address is kept`. The features keep their exact byte
   checks on Steam, so Steam behaviour is unchanged.

`GameAddr::Get(id)` returns the address or 0. Every feature loads its addresses from `GameAddr` when it installs, checks
the ids it needs with `GameAddr::Have` and, when one is missing, reports `Not available on <version>: missing ...`
(menu error or log warning) without touching game code. On non-Steam builds the checks that embedded relocation-dependent
bytes compare against bytes built from the resolved addresses (on Steam these are the very bytes the checks always had),
and checks that only pinned Steam's exact register allocation or stack frame (function prologues of called functions,
the `call edx` before a return address, the `jnz` distance in the GetLotID gather and the story cascade) are relaxed to
what the patch needs.

#### Struct offsets

All object offsets (light manager `+0x1C0` of the root, night level `+0xF0`, light cells `+0x104`, countdowns
`+0x38/+0x3C`, light fields `+0xB0` type, `+0xC0` lot id, `+0x100` flags, `+0x120` position, rig `+0x1D4/+0x1E0/+0x224`,
light tree levels `+0x6A0 + level * 0x1A4`, ...) are assumed to be the same as on Steam. Some are confirmed by the
signatures themselves (they contain the displacement: `+0x1C0` in the root getter, `+0x224` and `+0x104` in the cell
gather call, `+0xD8`, `+0x54`, `+0xE0`, `+0x1A0`, `+0x6A0`, `+0x140`, `+0x38`). The rest is checked once per session on
non-Steam builds by `GameAddr::CheckWorldStructs` when the first world is live: root -> light manager -> night level
(0..1), light cells and countdowns (-1..), light tree bucket count, and every enumerated light's vtable type (from the
light factory) against its type field `+0xB0` plus the alive flag. It only logs (`[Addr] Struct check: ...`).

### Diagnostics in the log

Both builds (development and public), log only:

```
[Addr] Scanning the game's code for EA 1.69.47.024017: .text 0x00401000..0x00fde73a (1 readable range(s), NNNN "push ebp; mov ebp,esp")
[Addr] RootGetter: 1 match at 0x00XXXXXX -> 0x00XXXXXX (Steam 0x006e97b0)
[Addr]   0x00XXXXXX: CC CC CC CC | A1 xx xx xx xx 85 C0 75 01 C3 8B 80
[Addr] ArmSiteRemoval: 0 matches -> not found (Steam 0x006b60d3)
[Addr] ArmSiteRemoval (alternate): 3 matches at ... -> 0x...
[Addr] SetColourCall0..6: 7 call(s) of SetLightColour at ... (expects 7; Steam 0x006c047d..)
[Addr] RootPtr: 0x0XXXXXXX from RootGetter + 0x1 (Steam 0x011d1860)
[Addr] NN of 101 addresses found in NN ms | NightLights: available | SplitLevel: available
[Addr] Struct check (EA 1.69.47.024017): light manager 0x..., night level 0.00 (+0xF0), ...: plausible
```

(The values above only illustrate the format.) One line per signature attempt with the match count and the first 8
matches, then for each of the first 3 matches 16 bytes (4 before the match, `|`, 12 from the match start) on non-Steam
builds, or on Steam when the self-check differs. To refine a signature from a player's log: take the bytes after `|` of
the right match, compare with the table below, widen the wildcards where the build differs, and check the new pattern
for uniqueness on Steam with `sigcheck.pl` (it must still resolve every id to the fixed address).

### Features and the addresses they need

| Feature / part | Needs | Without them |
|---|---|---|
| Night Lights (group `NightLights`, the switch itself) | RootGetter, RootPtr, QueueRoom, TerrainVisitorSite, the three ArmSites | "Not available on <version>: missing ..." |
| Night Lights: lot lamps light the street (`luzDoLoteNaGrama`) | TerrainVisitorSite, ArmSites | install fails while the option is on (as on Steam when the code differs) |
| Night Lights: chunk re-render notices (smoothed maps) | ChunkRenderCall, ChunkRenderFn | warning; maps found by hashing |
| Night Lights: street lamps lit in lot solves (developer option) | LampColourSite (inside LightEval11) | the option fails |
| Night Lights: all lots high quality (developer option) | QualitySite0/1 | the option fails |
| Night Lights: lot pass never binds the terrain map (developer option) | LotPassSite, LotPassConst, LotPassTexGlobal, LotPassNullBind | the option fails |
| Lot light bridge (street lamps in lots, soft lot edges, lamp scan) | RootPtr, EnumLights | lamp enumeration off; lot rectangles unavailable |
| Objects: lamp light on objects | LightVtable11, LightColour11, RigGatherReturn, CapOperandSite, CapGlobal, LumaWeights, DirtyAllRigs, RootPtr; each class boost needs its LightVtableN/LightColourN | warning, part off |
| Objects: stairs, railings, columns | RigCtor, RigCtorCall0..2 | warning, part off |
| Objects: fenced areas | RoomGatherCall, RoomGather, CellGather, RigUpdate, RigVtable | warning, part off |
| Lamp colour (warm white) | SetLightColour, SetColourCall0..6, ScriptSetColour, ScriptSetColourCall | warning, part off |
| Light between stories | AddWorldLights(+2 calls), LevelGather(+2 calls), CascadeTest, RoomById/InvalidateRoom/SetInsert (+calls), RootPtr, SolvePoint(+3 calls), LightEvalReturn, WallTestCall, WallTest | warning, part off |
| Light between stories: walls of other stories | WallCullCall, WallCull, BatchSolveCall, BatchSamples, LightPos; each class needs LightVtableN/LightEvalN | warning, sub-part off |
| Doors and windows stay lit (rig tracker) | ModelDraw, BinderCall, Binder, InstanceFlush, RigVtable | warning, part off |
| Every-Story Ground Light (group `SplitLevel`) | GetLotIdGatherCall, GetLotId | "Not available on <version>: missing ..." |
| S3SS Split-Level fix detection | GetLotId | treated as not active |
| Light Diag (Ctrl+Shift+F8, developer mode) | RootGetter, EnumLights (light vtables 4/5 for the cone lines) | "not available"; the raw Steam globals are printed on Steam only |
| Faster Game File Lookups (`ResourceLookupCache`, group `ResourceCache`) | ResFindProvider + 2 slots, ResRegisterDb + slot, ResRegisterDbDerived + slot, ResSetDbPriority + 2 slots, ResDbChanged + 2 slots, ShadowedDbVtable (and its three methods checked at run time, [../features/performance/README.md](../features/performance/README.md)) | "Not available on <version>: missing ..." / the read-only class check fails the install |
| Lot Lighting While Moving (`LotLightingMotion`, group `LotLightingMotion`) | LotLightBudgetCall, LotLightBudget, CameraRootCall, CameraGetterCall, CameraRootGetter, CameraGetter (the root global, camera offset and eye offset are parsed from those bytes) | "Not available on <version>: missing ..." / "The camera position was not found" |
| Faster Texture Compression (`FastTextureCompression`, group `FastTextureCompression`) | DxtEncode1, DxtEncode5 (the prologue `55 8B EC 83 E4 F0` is checked by `framework/entry_chain.cpp`); the first 16 textures of a session are compared with the game's own encoder on every build | "Not available on <version>: missing ..." / "the entry bytes ... changed"; a difference turns it off |
| Faster Cache Compression (`FastCacheCompression`, group `FastCacheCompression`) | RefPackCompress + RefPackCompressSlot; RefPackDecompress optional (the checks then use Apex's copy of the decoder) | "Not available on <version>: missing ..." |
| Wall Shading While Moving (`WallShadingWhileMoving`, group `WallShadingWhileMoving`) | WallAoStep + WallAoStepSlot, WallAoDriver, the four camera ids; the driver's whole body and the step's first 69 bytes (kStepBytes) are checked at run time | "Not available on <version>: missing ..." / "... differs from the one studied; nothing was changed" |
| Remember Missing Files (`ResourceLookupMisses`, group `ResourceCache`) | as Faster Game File Lookups; the write epochs also use DpfVtable, DpfDerivedVtable, DdfVtable, PackedStreamVtable and DpfWriteDirect, each class optional (its methods' first bytes checked; any difference = that class stays probed, logged by `[ResourceCache] Write epochs: ...`) | per class: "probed (...)" |
| Faster File Lists (`FileListCache`, group `FileListCache`) | ResKeyList + slot, ResKeyListDerived + slot, KeyTypeFilterVtable, the four package-list methods + slots, ShadowedDbVtable; the base loop, the vector insert, the sort, the filter's predicate and the read-only class's key list are checked at run time | "Not available on <version>: missing ..." / "... differs from the one studied; nothing was changed" |
| Spread New Objects Over Frames (`SceneNodeBudget`, group `SceneNodeBudget`) | SceneDrainCall, SceneDrain, SceneBoundsCall, SceneNodeBounds, SceneSpatialCall, SceneNodeSpatial, SceneNodeDtor, SceneAddNode, SceneHolderTeardown, the four camera ids; at run time the whole drain (0xD1 bytes) is compared with the Steam code (rel32s excepted) and its two CALLs must sit at +0xAE / +0xB6 and reach SceneNodeBounds / SceneNodeSpatial; the heads of AddNode, the node destructor and the teardown are compared too | "Not available on <version>: missing ..." / "... is not the code Apex was written for" |
| Faster Object Lookups (`ObjectLookupIndex`, group `ObjectIndex`) | ObjectById, ObjectTreeWalk, ObjectTreeSearch; at run time the three bodies are compared with the Steam code (rel32s and the lookup's first 8 bytes excepted), the lookup must CALL the walk at +0x10, the walk the search at +0x41, the search itself at +0x68; container / object classes are recognised by the bytes of their vtable functions | "Not available on <version>: missing ..." / "... is not the code Apex was written for" |
| Night Lights: local terrain relight and paced sweep (developer toggles `relightNearbyChunks`, `relightPacedSweep`; no group, optional) | WorldManagerPtr, TerrainUpdateCall (the terrain offset is the disp8 of its `mov ecx,[esi+disp8]`), ChunkRenderCall / ChunkRenderFn (the completion signal); at run time the terrain back pointer, the chunk grid (nx x nz, cell 256, dense), each chunk's corner / centre / size / rect and its bake record are checked before a chunk is flagged | lamp changes keep the full rebuild; the developer status says why |
| Night Lights brightness controls ("Street lamps" / "Lot lamps" on the ground, "Moonlight") | BakeColourSite, SunlightScale | those controls are not available |
| Rooms at Night (`features/unlit_rooms.cpp`) | UnlitColourA/B, DimColourA/B, FillGate, FillColour; the base under the lamps (optional) also needs DimAmbient, DimAmbientCall0..1, WorldManagerPtr; the `mov ecx, imm32`, `cmp byte` and `movaps` encodings around each site are checked | "Rooms at night: Not available on <version>: missing ..." / "the game code differs" |
| Light between stories: indoor light (`InstallIndoor`) | LightBright, AddRoomLight, RoomUpdatePush, RoomUpdate, ChangedClearCall, ChangedClear, FloorSet + FloorSetCall0..3, FloorRemove + FloorRemoveCall, LevelVtable; optional: LevelCtorCall/LevelCtor (floors at construction), LodChoice + 4 calls + LodMax (detail for rooms seen through an opening), RoomSolveStartCall/RoomSolveStart + WallPassCall/WallPass (one ambient for stacked rooms), WallSamplesCall/WallSamples, WallBlurCall/WallBlur, WallBlurPasses, WallBlurMode, WallSolveCall/WallSolve (wall alignment) | "Not available on <version>: missing ..." / "the game code differs"; each optional part logs a warning and is left as the game has it |
| | Faster Room Lighting (`features/room_light_queue.cpp`) | PriorityLotObject, PriorityLotTest (all parts); then per part: RoomPriorityCall + RoomPriority, LodStepSite, KeepClassA + KeepClassB, RoomPickJump + RoomPick + RoomSolveStep + StopwatchCtor/Start/Elapsed | "Faster room lighting: Not available ..."; a part whose bytes differ stays off |
| Faster Sim Building (`FastCasSort`, group `FastCasSort`) | CasTriSort | "Not available on <version>: missing ..." |
| Faster Memory (`FastMemory`, group `FastMemory`) | AllocGlobal, AllocMmapFreeCall | "Not available on <version>: missing ..." |
| Faster cache compression, record checksums (group `FastRecordCrc`) | RecordCrc, RecordCrcTable | "Not available on <version>: missing ..." |
| Frame Profiler texture load counters (developer mode) | TexCreateCall, TexCreate, TexFillCall, TexFill | those counters are not installed |
| Loaded-world gate (`features/world_session.h`: start note, Depth Blur) | WorldManagerPtr, UiServiceGetter (its first bytes must be `A1 imm32 ... C3`) | the world counts as not loaded: no start note and no Depth Blur |

Not part of this table: the map view probe (`features/map_view.cpp`) already finds its function at run time through the
script binding name in `.rdata` and its `{function, name}` table, on any build; the Frame Profiler (developer mode)
keeps its own Steam targets (address plus pattern), except the targets that name a table id (the performance hooks and
the texture load calls above), which take the resolved address and, for a CALL site, check its callee; shader patches match game shaders by bytecode, not game code.

### What is uncertain on EA 1.69.47

- **Nothing here has run on EA yet.** The signatures are unique on Steam; whether they match on the EA build is only
  known from a first log. The EA build is a later compile (2024 timestamp, 0x49DC9 more code). Register allocation,
  stack frame sizes, branch distances and inlining may differ.
- Code sites that Apex overwrites (TerrainVisitorSite, the ArmSites, LampColourSite, LotPassSite, QualitySites,
  CapOperandSite, CascadeTest) are patched with bytes that assume Steam's registers (`push edi` = the light, `esi` = the
  light, `[esp+0Ch]`, `[edi+0D8h]`). Their exact original bytes stay checked before writing, so a register-agnostic
  alternate would only turn "not found" into "differs": these entries have alternates with other context, not other
  registers. If EA allocates registers differently here, the patch code itself has to be adapted.
- Call sites that Apex redirects only change the rel32 of an existing CALL, so they work with any registers: their
  alternates (RigCtorCall, RoomGatherCall, ScriptSetColourCall, GetLotIdGatherCall, ...) keep less context. The function
  starts that Apex detours (ModelDraw, InstanceFlush) have alternates with the frame size open.
- Counts that must match exactly: 3 arm sites, 2 quality sites, 7 callers of SetLightColour, 2 of AddWorldLights, 2 of
  LevelGather, 3 of SolvePoint. A new caller on EA (or an inlined one fewer) makes that entry fail rather than guess.
- WallCullBatchFn is the lower of two twin functions (2D and 3D wall culling). On Steam the lower one is the 2D one; on EA
  the linker order is assumed to be the same (unverified). A wrong pick only affects the optional "walls of other stories".
- LevelGather's alternate (`53 56 8D 88 A0 06 00 00 E8`, call at +8) takes the second call of it in AddWorldLights; it
  resolves to the same function.
- GetLotId comes from the call in the outdoor-room gather, so it is found also when official S3SS already zeroed its body
  (its Split-Level fix, applied by pattern before Apex scans). S3SS's own pattern for it is Steam's exact 13-byte body,
  so where S3SS patched it the offsets `+0xC0/+0xC4` that `VanillaGetLotId` copies are the same.
- Official S3SS patches other game code before Apex scans (Apex waits for it). None of its fixed Steam patch sites (from its source) overlaps
  these signatures on Steam; on EA this is assumed.
- Struct offsets are assumed from Steam; `CheckWorldStructs` logs whether the ones it can read look right.

### Checking a table change

```
perl S3SS-dev\research\port169\sigcheck.pl [exe] [game_addresses.cpp]
```

Read-only. It parses `kInfo` and `kTable` (keep one entry per line in their current shape), runs the resolution rules
over the Steam exe and prints, per id, the match counts of both signatures and `ok` / `MISMATCH` against the fixed Steam
address. A change to the table is only done when it prints `101 ok, 0 mismatch`. The per-address context used to write
the signatures comes from `research\engine_map\full.asm` (dumpbin disassembly of `TS3W.exe`).

## Address reference

Checked on `S3SS-dev\re\TS3W.exe` (Steam 1.67.2) with `research\port169\sigcheck.pl`, which parses the table from
`game_addresses.cpp` and runs the same rules: **all 101 ids resolve to the fixed Steam address** (116 of 116 since
2026-09-28, with the 15 Frame Profiler ids at the end of the table below; 133 of 133 since 2026-09-29, with the 17
performance ids after them; 153 of 153 since the v1.5.0 merge (v1.4.0 round 3 ids, then the scene node budget and object index ids); 155 of 155
with WorldManagerPtr and TerrainUpdateCall (local terrain relight, 2026-09-29); their alternates were also checked one by one: each matches once, at the same place). "Matches" gives the
count of the primary / alternate signature on Steam (over `.text`, or over the range for `InRange`); `SetLightColour`
has 2 / 6 matches that all call the same function (accepted: the value agrees). Offsets are from the match start; "call
at +n" = the target of the CALL there, "dword at +n" = the value there.

| Id | Steam VA | Kind | Signature(s) | Matches on Steam |
|---|---|---|---|---|
| RootGetter | 0x006E97B0 | Sig | `A1 ?? ?? ?? ?? 85 C0 75 01 C3 8B 80 C0 01 00 00 C3` +0<br>alt: `C7 44 24 ?? ?? ?? ?? ?? E8 ?? ?? ?? ?? 3B C7 74 0C 8D 4C 24 ?? 51 8B C8 E8` call at +8 | matches 1 / 1 |
| RootPtr | 0x011D1860 | Deref(RootGetter, 1) | - | Deref(RootGetter+0x1) |
| QueueRoom | 0x006C7160 | Sig | `83 EC 2C 53 55 56 33 DB 8B F1 88 5C 24 0C 8B 44 24 0C` +0<br>alt: `89 3E E8 ?? ?? ?? ?? 53 8B CF E8 ?? ?? ?? ?? 50 8B CE E8` call at +2 | matches 1 / 1 |
| TerrainVisitorSite | 0x00C29626 | Sig | `56 57 8B 7C 24 0C 8B 07 8B 50 20 8B F1 8B CF FF D2 84 C0 74 ?? 8B 46 08 3B 46 0C` +6<br>alt: `8B 07 8B 50 20 8B F1 8B CF FF D2 84 C0 74 ?? 8B 46 08 3B 46 0C 8D 4E 04` +0 | matches 1 / 1 |
| ArmSiteRemoval | 0x006B60D3 | Multi(3) | `8B 17 8B 42 20 8B CF FF D0 84 C0 74 ?? C7 46 38 32 00 00 00` +0<br>alt: `80 7E 40 00 74 08 57 8B CE E8 ?? ?? ?? ?? 8B 17 8B 42 20 8B CF FF D0 84 C0 74` +14 | matches 3 / 3 |
| EnumLights | 0x006ACF70 | Sig | `E8 ?? ?? ?? ?? 3B C7 74 0C 8D 4C 24 ?? 51 8B C8 E8 ?? ?? ?? ?? 8B 46 1C` call at +16<br>alt: `E8 ?? ?? ?? ?? 8B 4C 24 04 51 68 ?? ?? ?? ?? 8B C8 E8 ?? ?? ?? ?? C2 04 00` +0 | matches 1 / 1 |
| ChunkRenderCall | 0x00C8504C | Sig | `80 7E 54 00 74 ?? 6A 00 56 8B CF E8 ?? ?? ?? ?? C6 44 24 0C 01` +11<br>alt: `6A 00 56 8B CF E8 ?? ?? ?? ?? C6 44 24 0C 01 EB` +5 | matches 1 / 1 |
| ChunkRenderFn | 0x00C7E7A0 | Target(ChunkRenderCall) | - | Target(ChunkRenderCall) |
| LightJumpTable | 0x006AC7A0 | Sig | `8B B1 04 01 00 00 33 C0 85 F6 0F 84 ?? ?? ?? ?? 8B 4C 24 08 83 C1 FD 83 F9 08 0F 87 ?? ?? ?? ?? FF 24 8D` dword at +35<br>alt: `85 F6 0F 84 ?? ?? ?? ?? 8B 4C 24 08 83 C1 FD 83 F9 08 0F 87 ?? ?? ?? ?? FF 24 8D` dword at +27 | matches 1 / 1 |
| LightPos | 0x009691E0 | Deref(LightVtable3, 0x24) | - | Deref(LightVtable3+0x24) |
| LampColourSite | 0x006BE18C | InRange(LightEval11, 0x300) | `0F 28 86 E0 00 00 00 8B 55 10` +0<br>alt: `0F 28 86 E0 00 00 00` +0 | matches 1 / 1 |
| LotPassSite | 0x00C7F87D | Sig | `8B 7D 08 8B 87 D8 00 00 00 85 C0 F3 0F 10 05 ?? ?? ?? ?? F3 0F 11 44 24 18 74 13` +3<br>alt: `8B 87 D8 00 00 00 85 C0 F3 0F 10 05 ?? ?? ?? ?? F3 0F 11 44 24 ?? 74` +0 | matches 1 / 1 |
| LotPassConst | 0x0107A538 | Deref(LotPassSite, 0x0C) | - | Deref(LotPassSite+0xC) |
| LotPassTexGlobal | 0x011ECE80 | Deref(LotPassSite, 0x1A) | - | Deref(LotPassSite+0x1A) |
| LotPassNullBind | 0x00C7F8B7 | InRange(LotPassSite, 0x80) | `80 78 1D 00 75 ?? A1 ?? ?? ?? ?? 6A 00 6A 00 50` +6<br>alt: `A1 ?? ?? ?? ?? 6A 00 6A 00 50 8B CE E8` +0 | matches 1 / 1 |
| QualitySite0 | 0x00ADB66B | Multi(2) | `75 0B 80 ?? 4D 00 C6 44 24 0C 00 74 05 C6 44 24 0C 01` +6<br>alt: `4D 00 C6 44 24 0C 00 74 05 C6 44 24 0C 01` +2 | matches 2 / 2 |
| CapOperandSite | 0x006B9418 | Sig | `0F 5F F0 0F 28 00 0F 5F F0 B9 ?? ?? ?? ?? 0F 29 74 24 30 E8` +9<br>alt: `B9 ?? ?? ?? ?? 0F 29 74 24 30 E8 ?? ?? ?? ?? D9 00` +0 | matches 1 / 1 |
| CapGlobal | 0x011D0BA8 | Deref(CapOperandSite, 1) | - | Deref(CapOperandSite+0x1) |
| RigGatherReturn | 0x006BB2B3 | Sig | `8B 17 8B 52 10 8D 44 24 10 50 8D 8E 40 01 00 00 51 8B CF FF D2 83 7E 08 00 74` +21<br>alt: `8D 8E 40 01 00 00 51 8B CF FF D2 83 7E 08 00` +11 | matches 1 / 1 |
| LumaWeights | 0x011D1140 | Sig | `0F 29 4E 10 0F 28 05 ?? ?? ?? ?? 0F 28 4E 10` dword at +7<br>alt: `0F 29 71 10 0F 28 05 ?? ?? ?? ??` dword at +7 | matches 1 / 1 |
| DirtyAllRigs | 0x006B58F0 | Sig | `83 EC 10 55 8B E9 33 C9 33 C0 39 4D 30 89 44 24 0C 76` +0<br>alt: `55 8B E9 33 C9 33 C0 39 4D 30` +-3 | matches 1 / 1 |
| RigCtor | 0x006BB8F0 | Sig | `56 8B F1 57 C7 06 ?? ?? ?? ?? 33 C0 8D 4E 04 87 01 8A 54 24 0C 8A 86 24 02 00 00` +0<br>alt: `C7 06 ?? ?? ?? ?? 33 C0 8D 4E 04 87 01 8A 54 24 0C 8A 86 24 02 00 00` +-4 | matches 1 / 1 |
| RigVtable | 0x00FF4218 | InRange(RigCtor, 0x80) | `C7 06 ?? ?? ?? ?? C7 46 08 00 00 00 00 C7 46 0C 00 00 00 00` dword at +2<br>alt: `24 E7 0A D0 8B CE C7 06 ?? ?? ?? ??` dword at +8 | matches 1 / 1 |
| RigCtorCall0 | 0x006F7905 | CallIn(RigCtor, 0x18) | `6A 01 56 8B C8 81 E2 01 FF FF FF 52 E8` +0<br>alt: `9C 02 00 00 C0 ?? 04 6A 01 56` +0 | matches 1 / 1 |
| RigCtorCall1 | 0x006F795C | CallIn(RigCtor, 0x18) | `6A 02 56 81 E1 01 FF FF FF 51 8B C8 E8` +0<br>alt: `9C 02 00 00 C0 ?? 04 6A 02 56` +0 | matches 1 / 1 |
| RigCtorCall2 | 0x006F799D | CallIn(RigCtor, 0x18) | `6A 00 56 81 E1 01 FF FF FF 51 8B C8 E8` +0<br>alt: `9C 02 00 00 C0 ?? 04 6A 00 56` +0 | matches 1 / 1 |
| RoomGatherCall | 0x006BBE70 | Sig | `8D 97 C8 00 00 00 52 8D 47 30 50 8B CE E8 ?? ?? ?? ?? 8B CE E8` +13<br>alt: `8D 47 30 50 8B CE E8 ?? ?? ?? ?? 8B CE E8` +6 | matches 1 / 1 |
| RoomGather | 0x006BB2F0 | Target(RoomGatherCall) | - | Target(RoomGatherCall) |
| CellGather | 0x006B5AF0 | Sig | `F6 86 24 02 00 00 10 74 ?? 8B 88 04 01 00 00 56 E8 ?? ?? ?? ?? 8B 0D` call at +16<br>alt: `83 EC 1C 8B 54 24 20 53 55 56 57 8B F1 8D 44 24 1C 50 8D 4C 24 28` +0 | matches 1 / 1 |
| RigUpdate | 0x006BBF90 | Sig | `80 A1 24 02 00 00 F7 83 B9 EC 01 00 00 00 74 06 83 79 08 00 74 ?? 83 B9 D4 01 00 00 02` +0<br>alt: `80 A1 24 02 00 00 F7 83 B9 EC 01 00 00 00` +0 | matches 1 / 1 |
| SetLightColour | 0x006BDA90 | Sig | `74 ?? 8D 4F 10 51 8B CE E8 ?? ?? ?? ?? D9 47 1C` call at +8<br>alt: `8D 4F 10 51 8B CE E8 ?? ?? ?? ?? D9 47 1C` call at +6 | matches 2 / 6 |
| SetColourCall0 | 0x006C047D | CallersOf(SetLightColour, 7) | - | 7 (expects 7) |
| ScriptSetColourCall | 0x006B0BDE | Sig | `8B 0E 8D 44 24 10 50 0F 29 44 24 14 E8 ?? ?? ?? ?? 83 C6 04` +12<br>alt: `50 0F 29 44 24 14 E8 ?? ?? ?? ?? 83 C6 04` +6 | matches 1 / 1 |
| ScriptSetColour | 0x006BC3E0 | Target(ScriptSetColourCall) | `55 8B EC 83 E4 F0 8B 45 08 0F 28 00 0F 29 81 F0 00 00 00 F6 81 00 01 00 00 20` +0 | Target(ScriptSetColourCall) (fallback sig: 1 match(es), 006BC3E0) |
| AddWorldLights | 0x006C6AB0 | Sig | `55 8B EC 83 E4 F0 83 EC 34 53 56 33 DB F6 05 ?? ?? ?? ?? 01 57 8B F9 75` +0<br>alt: `83 EC 34 53 56 33 DB F6 05 ?? ?? ?? ?? 01 57 8B F9` +-6 | matches 1 / 1 |
| AddWorldLightsCall0 | 0x006C5816 | CallersOf(AddWorldLights, 2) | - | 2 (expects 2) |
| LevelGather | 0x006C6990 | InRange(AddWorldLights, 0x100) | `6A 01 56 8B CF E8 ?? ?? ?? ?? 39 5E 0C` call at +5<br>alt: `53 56 8D 88 A0 06 00 00 E8` call at +8 | matches 1 / 1 |
| LevelGatherCall0 | 0x006C6B08 | CallersOf(LevelGather, 2) | - | 2 (expects 2) |
| CascadeTest | 0x006C73AA | Sig | `39 86 A0 01 00 00 0F 85 ?? ?? ?? ?? 33 ED 83 FD FC 8B C5 7D 07 B8 FC FF FF FF EB 0A 83 FD 08 7C 05 B8 07 00 00 00` +0<br>alt: `39 86 A0 01 00 00 0F 85 ?? ?? ?? ?? 33 ED 83 FD FC` +0 | matches 1 / 1 |
| RoomByIdCall | 0x006C73F0 | InRange(CascadeTest, 0x100) | `74 ?? 6A 00 E8 ?? ?? ?? ?? 85 C0 74 ?? 6A 00 6A 01 8B C8 E8` +4<br>alt: `6A 00 E8 ?? ?? ?? ?? 85 C0 74` +2 | matches 1 / 1 |
| RoomById | 0x006A6550 | Target(RoomByIdCall) | - | Target(RoomByIdCall) |
| InvalidateCall | 0x006C73FF | InRange(RoomByIdCall, 0x20) | `6A 00 6A 01 8B C8 E8` +6<br>alt: `8B C8 E8` +2 | matches 1 / 1 |
| InvalidateRoom | 0x0069EED0 | Target(InvalidateCall) | - | Target(InvalidateCall) |
| SetInsertCall | 0x006C741B | InRange(CascadeTest, 0x100) | `C6 44 24 10 00 8B 54 24 10 52 8D 44 24 18 50 8D 4C 24 34 51 8D 4F 28 E8` +23<br>alt: `51 8D 4F 28 E8` +4 | matches 1 / 1 |
| SetInsert | 0x00B7AAD0 | Target(SetInsertCall) | - | Target(SetInsertCall) |
| SolvePoint | 0x0069FD60 | Sig | `55 8B EC 83 E4 F0 83 EC 74 0F 57 C0 8B 45 08 53 8B D9 8B 8B CC 00 00 00 2B 8B C8 00 00 00` +0<br>alt: `8D 54 24 30 52 8B CB E8 ?? ?? ?? ?? 80 7B 18 00 0F 57 C9 0F 85` call at +7 | matches 1 / 1 |
| SolvePointCall0 | 0x006A1187 | CallersOf(SolvePoint, 3) | - | 3 (expects 3) |
| BatchSolveCall | 0x006A3336 | Sig | `8D 54 24 30 52 8B CB E8 ?? ?? ?? ?? 80 7B 18 00 0F 57 C9 0F 85` +7<br>alt: `E8 ?? ?? ?? ?? 80 7B 18 00 0F 57 C9 0F 85` +0 | matches 1 / 1 |
| LightEvalReturn | 0x0069FE19 | InRange(SolvePoint, 0x300) | `8B 3E 50 52 8B 57 4C 8B CE FF D2 0F 28 4C 24 40` +11<br>alt: `8B 57 4C 8B CE FF D2` +7 | matches 1 / 1 |
| WallTestCall | 0x0069FE93 | InRange(SolvePoint, 0x300) | `8B CB E8 ?? ?? ?? ?? 84 C0 74 ?? 8B 45 14 80 78 01 00` +2<br>alt: `E8 ?? ?? ?? ?? 84 C0 74 ?? 8B 45 14 80 78 01 00` +0 | matches 1 / 1 |
| WallTest | 0x0069FC40 | Target(WallTestCall) | - | Target(WallTestCall) |
| BatchSamples | 0x01158AC8 | Sig | `8D 46 44 50 68 ?? ?? ?? ?? E8` dword at +5<br>alt: `8D 46 30 50 83 C6 44 56 68 ?? ?? ?? ??` dword at +9 | matches 1 / 1 |
| WallCullBatchFn | 0x006A30B0 | LowestOf2 | `55 8B EC 83 E4 F0 83 EC 24 53 56 8B F1 8B 9E CC 00 00 00 2B 9E C8 00 00 00 57 8B 7D 0C` +0<br>alt: `8B 9E CC 00 00 00 2B 9E C8 00 00 00 57 8B 7D 0C 8B CF 89 74 24 1C C1 FB 02` +-13 | matches 2 / 2 |
| WallCullCall | 0x006A311F | InRange(WallCullBatchFn, 0x90) | `56 83 C1 30 E8` +4<br>alt: `83 C1 30 E8` +3 | matches 1 / 1 |
| WallCull | 0x0069DFF0 | Target(WallCullCall) | - | Target(WallCullCall) |
| ModelDraw | 0x006F6250 | Sig | `55 8B EC 83 E4 F0 81 EC 94 01 00 00 53 8B D9 F7 43 40 00 10 00 00 56 57 0F 85` +0<br>alt: `55 8B EC 83 E4 F0 81 EC ?? ?? 00 00 53 8B D9 F7 43 40 00 10 00 00 56 57 0F 85` +0 | matches 1 / 1 |
| BinderCall | 0x006F68C5 | InRange(ModelDraw, 0x1000) | `0F 95 44 24 16 8B CF E8 ?? ?? ?? ?? 6A 00 E8` +7<br>alt: `3A C1 0F 95 44 24 ?? 8B CF E8` +9 | matches 1 / 1 |
| Binder | 0x006B8B30 | Target(BinderCall) | - | Target(BinderCall) |
| InstanceFlush | 0x006CF920 | Sig | `55 8B EC 83 E4 F0 81 EC A4 0B 00 00 53 56 8B F1 80 7E 4C 01 57 89 74 24 10 0F 84` +0<br>alt: `55 8B EC 83 E4 F0 81 EC ?? ?? 00 00 53 56 8B F1 80 7E 4C 01 57 89 74 24 10 0F 84` +0 | matches 1 / 1 |
| GetLotIdGatherCall | 0x006B635D | Sig | `FF D0 84 C0 74 ?? 8B CE E8 ?? ?? ?? ?? 0B C2 75 ?? 8B 45 04` +8<br>alt: `8B CE E8 ?? ?? ?? ?? 0B C2 75 ?? 8B 45 04` +2 | matches 1 / 1 |
| GetLotId | 0x006BC020 | Target(GetLotIdGatherCall) | `8B 81 C0 00 00 00 8B 91 C4 00 00 00 C3` +0 | Target(GetLotIdGatherCall) (fallback sig: 1 match(es), 006BC020) |
| LightVtable3..11 | 0xFF42A0, FF4570, FF4350, FF4468, FF43A8, FF4408, FF44C0, FF4518, FF42F8 (types 3..11) | LightType(LightJumpTable, t) | - (case code "8B C8 E8 ctor" within 0x48 bytes, then the ctor's first `C7 06/07 <imm in image>` within 0x88 bytes) | all 9 ok |
| LightColour3..11 | 0x6C02A0, 6C1BC0, 6C0690, 6C1320, 6C0AF0, 6C0FE0, 6C16D0, 6C1980, 6C02A0 | Deref(LightVtableN, 0x10) | - | all 9 ok |
| LightEval3..11 | 0x6BDE90, 6BFFB0, 6BE1C0, 6BFA70, 6BEFD0, 6BF880, 6BFBA0, 6BFDC0, 6BE020 | Deref(LightVtableN, 0x4C) | - | all 9 ok |
| ResFindProvider | 0x004AFFC0 | Sig | `51 53 55 56 57 8B F9 8D 5F 48 68 ?? ?? ?? ?? 8B CB E8 ?? ?? ?? ?? 8B 77 30 8B 6F 34 3B F5` +0<br>alt: `8B F9 8D 5F 48 68 ?? ?? ?? ?? 8B CB E8 ?? ?? ?? ?? 8B 77 30 8B 6F 34 3B F5 C7 44 24 10 00 00 00 00` +-5 | matches 1 / 1 |
| ResFindProviderSlot0/1 | 0x00FB2DE0, 0x00FFE290 | SlotsOf(ResFindProvider, 2) | - | 2 (expects 2) |
| RefPackCompress | 0x004EC200 | Sig | `8B 54 24 14 33 C0 F6 C2 02 74 07 B8 01 00 00 00 EB 0D F7 C2 00 00 01 00 74 05 B8 02 00 00 00 56` +0<br>alt: `F6 C2 02 74 07 B8 01 00 00 00 EB 0D F7 C2 00 00 01 00 74 05 B8 02 00 00 00 56 8B 74 24 10 85 F6` +-6 | matches 1 / 1 |
| RefPackCompressSlot | 0x00FB901C | SlotsOf(RefPackCompress, 1) | - | 1 (expects 1) |
| SceneDrainCall | 0x006EBC49 | Sig | `8B 4E 08 E8 ?? ?? ?? ?? 80 BE A2 02 00 00 00 75 ?? 8B 4E 38 E8` +3<br>alt: `E8 ?? ?? ?? ?? 80 BE A2 02 00 00 00 75 ?? 8B 4E 38 E8 ?? ?? ?? ?? 8B 4E 38 E8` +0 | matches 1 / 1 |
| SceneDrain | 0x006E4130 | Target(SceneDrainCall) | `55 8B EC 83 E4 F0 83 EC 34 53 56 57 8B F9 8B 77 20 8B 5F 24 8D 47 20 3B F0` +0 | fallback sig: 1 match |
| DxtEncode1 | 0x006152F0 | Sig | `55 8B EC 83 E4 F0 81 EC 54 01 00 00 8B 45 08 8B 50 04 8B 48 08 53 56 8D 72 03` +0<br>alt: `81 EC 54 01 00 00 8B 45 08 8B 50 04 8B 48 08 53 56 8D 72 03 83 E6 FC 03 F6` +-6 | matches 1 / 1 |
| DxtEncode5 | 0x006154B0 | Sig | `55 8B EC 83 E4 F0 81 EC A4 01 00 00 8B 45 08 8B 48 04 8D 51 03 83 E2 FC` +0<br>alt: `81 EC A4 01 00 00 8B 45 08 8B 48 04 8D 51 03 83 E2 FC 03 D2 03 D2 53 8B 18` +-6 | matches 1 / 1 |
| ObjectById | 0x00C62D40 | Sig | `8B 44 24 0C 8B 54 24 08 56 50 8B 44 24 0C 52 50 E8 ?? ?? ?? ?? 8B F0 85 F6 74 14 8B 16 8B 42 40 8B CE FF D0 83 F8 01` +0<br>alt: `8B 44 24 0C 8B 54 24 08 56 50 8B 44 24 0C 52 50 E8` +0 | matches 1 / 1 |
| RoomSolveCall | 0x00ADB9AD | Sig | `85 C9 74 10 51 8D 54 24 18 D9 1C 24 52 E8` +13<br>alt: `8B 0C 88 85 C9 74 ?? 51 8D 54 24 ?? D9 1C 24 52 E8` +16 | matches 1 / 1 |
| RoomSolve | 0x006A8BA0 | Target(RoomSolveCall) | `56 8B F1 80 BE 80 02 00 00 00 57 75 05 E8 ?? ?? ?? ?? 83 BE 88 00 00 00` +0 | fallback sig: 1 match |
| RemoteCallJob | 0x007D9840 | Sig | `83 7C 24 0C 04 56 57 75 6C 8B 7C 24 0C 33 F6 F6 47 20 01 74 4A` +0<br>alt: `F6 47 20 01 74 ?? 8B 35 ?? ?? ?? ?? 85 F6 74 ?? 8D 44 24 14 50 57 8B CE C7 44 24 1C 00 00 00 00 E8` +-15 | matches 1 / 1 |
| RemoteMethodVtable | 0x010650C4 | Sig (image) | `89 50 10 8A 54 24 1C 88 48 19 C7 00 ?? ?? ?? ?? 88 50 18` dword at +12<br>alt: `8A 54 24 1C 88 48 19 C7 00 ?? ?? ?? ?? 88 50 18 8B 10` dword at +9 | matches 1 / 1 |
| RemoteMethodVtable2 | 0x010650D8 | Sig (image) | `89 50 10 8A 54 24 1C 89 48 14 C7 00 ?? ?? ?? ?? 88 50 18` dword at +12<br>alt: `8A 54 24 1C 89 48 14 C7 00 ?? ?? ?? ?? 88 50 18 8B 10` dword at +9 | matches 1 / 1 |
| ResRegisterDb | 0x004B2D00 | Sig | `83 EC 10 53 55 56 57 8B F9 8D 4F 48 68 ?? ?? ?? ?? 89 4C 24 18 E8 ?? ?? ?? ?? 80 7C 24 24 00 C6 44 24 13 00 0F 84` +0<br>alt: `8D 4F 48 68 ?? ?? ?? ?? 89 4C 24 18 E8 ?? ?? ?? ?? 80 7C 24 24 00 C6 44 24 13 00` +-9 | matches 1 / 1 |
| ResRegisterDbSlot | 0x00FB2DD4 | SlotsOf(ResRegisterDb, 1) | - | 1 (expects 1) |
| ResRegisterDbDerived | 0x00736A70 | Sig | `81 EC 14 02 00 00 80 BC 24 18 02 00 00 00 53 8B 9C 24 20 02 00 00 55 56 57 8B F9 75 ?? 85 DB 74` +0<br>alt: `80 BC 24 18 02 00 00 00 53 8B 9C 24 20 02 00 00 55 56 57 8B F9` +-6 | matches 1 / 1 |
| ResRegisterDbDerivedSlot | 0x00FFE284 | SlotsOf(ResRegisterDbDerived, 1) | - | 1 (expects 1) |
| ResSetDbPriority | 0x004B2EC0 | Sig | `83 EC 0C 55 56 8B E9 57 8D 4D 48 68 ?? ?? ?? ?? 89 4C 24 18 E8 ?? ?? ?? ?? 8B 75 30 8B 45 34 3B F0 8D 7D 30 0F 84` +0<br>alt: `8D 4D 48 68 ?? ?? ?? ?? 89 4C 24 18 E8 ?? ?? ?? ?? 8B 75 30 8B 45 34 3B F0 8D 7D 30` +-8 | matches 1 / 1 |
| ResSetDbPrioritySlot0/1 | 0x00FB2DDC, 0x00FFE28C | SlotsOf(ResSetDbPriority, 2) | - | 2 (expects 2) |
| ResDbChanged | 0x004B0960 | Sig | `51 53 8B D9 56 8D 73 48 68 ?? ?? ?? ?? 8B CE 89 74 24 0C E8 ?? ?? ?? ?? 8B 54 24 14 85 D2 0F 84 ?? ?? ?? ?? 83 7B 20 00` +0<br>alt: `8D 73 48 68 ?? ?? ?? ?? 8B CE 89 74 24 0C E8 ?? ?? ?? ?? 8B 54 24 14 85 D2 0F 84` +-5 | matches 1 / 1 |
| ResDbChangedSlot0/1 | 0x00FB2DEC, 0x00FFE29C | SlotsOf(ResDbChanged, 2) | - | 2 (expects 2) |
| ShadowedDbVtable | 0x00FFE078 | Sig (image) | `8D 86 C0 00 00 00 50 C7 06 ?? ?? ?? ?? C7 07 ?? ?? ?? ?? C7 46 0C ?? ?? ?? ?? 89 9E B8 00 00 00` dword at +15<br>alt: `C7 06 ?? ?? ?? ?? C7 07 ?? ?? ?? ?? C7 46 0C ?? ?? ?? ?? 89 9E B8 00 00 00 FF 15` dword at +8 | matches 1 / 1 |
| LotLightBudgetCall | 0x00ADB95D | Sig | `8D 4C 24 10 E8 ?? ?? ?? ?? 8B CE E8 ?? ?? ?? ?? D9 54 24 0C 33 DB 85 ED 7E` +11<br>alt: `8B CE E8 ?? ?? ?? ?? D9 54 24 0C 33 DB 85 ED 7E ?? 57 8B 4E 28` +2 | matches 1 / 1 |
| LotLightBudget | 0x00ADB120 | Target(LotLightBudgetCall) | `51 A1 ?? ?? ?? ?? 85 C0 56 8B F1 74 ?? 83 B8 ?? ?? 00 00 00 75 ?? D9 05 ?? ?? ?? ?? 5E 59 C3 8B 46 14` +0 | fallback sig: 1 match |
| CameraRootCall | 0x00C6D5BD | Sig | `E8 ?? ?? ?? ?? 8B C8 E8 ?? ?? ?? ?? 0F 28 40 ?? 8B C8 0F 29 44 24 ?? E8 ?? ?? ?? ?? 83 7E 58 00` +0<br>alt: `8B C8 E8 ?? ?? ?? ?? 0F 28 40 ?? 8B C8 0F 29 44 24 ?? E8 ?? ?? ?? ?? 83 7E 58 00 0F 28 00` +-5 | matches 1 / 1 |
| CameraGetterCall | 0x00C6D5C4 | Sig | the same two signatures, +7 / +2 | matches 1 / 1 |
| CameraRootGetter | 0x006E8330 | Target(CameraRootCall) | - | Target(CameraRootCall) |
| CameraGetter | 0x006E8400 | Target(CameraGetterCall) | - | Target(CameraGetterCall) |
| RefPackDecompress | 0x004EB3B0 | Sig (CALL target) | `3B C2 77 14 8B 44 24 0C 50 56 52 51 E8 ?? ?? ?? ?? 83 C4 10 5E C2 14 00` call at +12<br>alt: `8B 44 24 0C 50 56 52 51 E8 ?? ?? ?? ?? 83 C4 10 5E C2 14 00` call at +8 (both inside the stream read 0x004EC010, not on the decoder's entry, which the official S3SS detours) | matches 1 / 1 (sigcheck.pl 2026-09-29: 134 of 134 ok) |
| WallAoStep | 0x0068B810 | Sig | `83 EC 34 55 56 8B F1 83 7E 04 00 74 14 E8 ?? ?? ?? ?? 8B 4E 04 50 E8 ?? ?? ?? ?? 8B E8 85 ED 75 18 8B 46 04 8A 80 80 02 00 00` +0<br>alt: `8B 46 04 8A 80 80 02 00 00 F6 D8 5E 5D 1B C0 83 E0 02 83 C4 34 C2 08 00 8B 85 DC 00 00 00 2B 85 D8 00 00 00` +-33 | matches 1 / 1 |
| WallAoStepSlot | 0x00FF05B0 | SlotsOf(WallAoStep), 1 | - | 1 slot |
| WallAoDriver | 0x00688920 | Sig | the whole body `56 8B F1 83 7E 14 02 74 27 ... 89 46 14 5E C2 08 00` +0<br>alt: its last 25 bytes `8B 06 8B 50 1C 51 8B 4C 24 0C D9 1C 24 51 8B CE FF D2 89 46 14 5E C2 08 00` +-27 | matches 1 / 1 |
| ResKeyList | 0x004B1AE0 | Sig | `83 EC 10 53 33 C0 38 44 24 20 56 57 89 44 24 0C 0F 84 ?? ?? ?? ?? 8B B1 A0 00 00 00 8B B9 A4 00 00 00 3B F7` +0<br>alt: the derived function `8B 44 24 0C 8B 54 24 08 56 8B 74 24 08 50 52 56 E8 ?? ?? ?? ?? 85 C0 74 0D 85 F6 74 09 56 E8` call at +16 | matches 1 / 1 |
| ResKeyListSlot | 0x00FB2DC0 | SlotsOf(ResKeyList), 1 | - | 1 slot |
| ResKeyListDerived | 0x00736660 | Sig | the whole body `8B 44 24 0C 8B 54 24 08 56 8B 74 24 08 50 52 56 E8 ?? ?? ?? ?? 85 C0 74 0D 85 F6 74 09 56 E8 ?? ?? ?? ?? 83 C4 04 5E C2 0C 00` +0 | matches 1 |
| ResKeyListDerivedSlot | 0x00FFE270 | SlotsOf(ResKeyListDerived), 1 | - | 1 slot |
| KeyTypeFilterVtable | 0x00FD8248 | Sig (dword) | `57 8D 4C 24 64 51 C7 44 24 68 ?? ?? ?? ?? C7 44 24 6C DA 7D 03 0A 8B 10 8B 52 20` dword at +10 (CAS 0x005DA0C0)<br>alt: `C7 44 24 68 ?? ?? ?? ?? C7 44 24 6C DA 7D 03 0A 8B 10 8B 52 20 8D 4C 24 34 51 8B C8 FF D2` dword at +4 | matches 1 / 1 |
| DpfVtable | 0x00FB2600 | Sig (dword) | ctor 0x004A8F70: `33 DB 3B C3 C7 06 ?? ?? ?? ?? C7 46 04 ?? ?? ?? ?? 88 5E 0C 88 5E 0D 88 5E 0E C6 46 0F 01` dword at +6<br>alt: dtor 0x004A8440: `C7 06 ?? ?? ?? ?? C7 46 04 ?? ?? ?? ?? E8 ?? ?? ?? ?? 8D 8E 68 03 00 00 E8` dword at +2 | matches 1 / 1 |
| DpfDerivedVtable | 0x01048DA0 | Sig (dword) | `D9 EE 51 D9 1C 24 C7 06 ?? ?? ?? ?? C7 46 04 ?? ?? ?? ?? 8D BE 98 03 00 00 C7 47 0C` dword at +8 | matches 1 |
| DdfVtable | 0x00FB2420 | Sig (dword) | `33 DB 3B C3 C7 06 ?? ?? ?? ?? C7 46 04 ?? ?? ?? ?? 88 5E 0C 75 05 E8 ?? ?? ?? ?? 89 46 10 88 5E 14 89 5E 18` dword at +6 | matches 1 |
| PackedStreamVtable | 0x00FFD790 | Sig (dword) | `33 DB 3B C3 C7 06 ?? ?? ?? ?? C7 46 04 ?? ?? ?? ?? 88 5E 0C 75 05 E8 ?? ?? ?? ?? 89 46 10 89 5E 14 C7 46 28` dword at +6 | matches 1 |
| DpfWriteDirect | 0x004A7FC0 | Sig | `83 EC 28 53 56 8B F1 8D 8E 70 02 00 00 68 ?? ?? ?? ?? 89 4C 24 10 E8 ?? ?? ?? ?? B3 02 84 5E 14` +0 | matches 1 (sigcheck.pl 2026-09-29: 147 of 147 ok) |
| SceneBoundsCall | 0x006E41DE | InRange(SceneDrain, 0xD1) | `8D 44 24 20 50 8B CE E8 ?? ?? ?? ?? 50 8B CE E8` +7<br>alt: `50 8B CE E8 ?? ?? ?? ?? 50 8B CE E8` +3 | matches 1 / 1 |
| SceneNodeBounds | 0x006FB4B0 | Target(SceneBoundsCall) | `55 8B EC 83 E4 F0 81 EC 8C 00 00 00 56 8B F1 8A 46 40 F6 D0 A8 01 75 ?? 8B 46 30 85 C0 74 ?? 83 78 2C 00 74` +0 | fallback sig: 1 match |
| SceneSpatialCall | 0x006E41E6 | InRange(SceneDrain, 0xD1) | `50 8B CE E8 ?? ?? ?? ?? 8B 4C 24 1C 01 5F 18` +3<br>alt: `E8 ?? ?? ?? ?? 8B 4C 24 1C 01 5F 18` +0 | matches 1 / 1 |
| SceneNodeSpatial | 0x006FAD70 | Target(SceneSpatialCall) | `8B 41 30 85 C0 74 14 8B 40 2C 85 C0 74 0D 8B 54 24 04 52 51 8B C8 E8 ?? ?? ?? ?? C2 04 00` +0 | fallback sig: 1 match |
| SceneNodeDtor | 0x006FD930 | Sig | `55 8B EC 83 E4 F0 81 EC 94 00 00 00 53 56 57 8B F9 8D 9F 94 01 00 00 C7 07 ?? ?? ?? ?? 39 5B 04 74` +0<br>alt: `8D 9F 94 01 00 00 C7 07 ?? ?? ?? ?? 39 5B 04 74 ?? 8D 4C 24 10 E8` -17 | matches 1 / 1 |
| SceneAddNode | 0x006E6480 | Sig | `56 8B 74 24 08 83 7E 30 00 57 8B F9 0F 85 ?? ?? ?? ?? 53 55 E8` +0<br>alt: `83 7E 30 00 57 8B F9 0F 85 ?? ?? ?? ?? 53 55 E8` -5 | matches 1 / 1 |
| SceneHolderTeardown | 0x006E4DE0 | Sig | `53 55 56 57 8B F9 8B 4F 2C 85 C9 74 ?? E8 ?? ?? ?? ?? 8B 77 2C 85 F6 74` +0<br>alt: `8B 4F 2C 85 C9 74 ?? E8 ?? ?? ?? ?? 8B 77 2C 85 F6 74 ?? 8B CE E8` -6 | matches 1 / 1 (sigcheck.pl 2026-09-29: 158 of 158 ok) |
| ObjectTreeWalk | 0x00C60D30 | Sig | `53 8B 5C 24 08 55 8B 6C 24 10 56 8B F1 8B CB 33 C0 0B CD 74 ?? 8B 96 A0 00 00 00 2B 96 9C 00 00 00 57 33 FF C1 FA 02` +0<br>alt: `8B 44 24 0C 52 50 E8 ?? ?? ?? ?? 8B F0 85 F6 74 14 8B 16 8B 42 40 8B CE FF D0 83 F8 01` call at +6 (inside ObjectById) | matches 1 / 1 |
| ObjectTreeSearch | 0x00C5FA60 | Sig | `53 55 56 8B 74 24 10 85 F6 0F 84 ?? ?? ?? ?? 8B 46 48 8B 5C 24 14 3B C3 8B 6C 24 18 75 ?? 8B 4E 4C 3B CD 74` +0<br>alt: `8B 04 B8 51 55 53 50 E8 ?? ?? ?? ?? 83 C4 10 85 C0 75` call at +7 (inside the walk) | matches 1 / 1 (sigcheck.pl 2026-09-29, after merging v1.4.0 and C6 / C8: 153 of 153 ok) |
| WorldManagerPtr | 0x011ECBC4 | Sig (dword) | store in FUN_00c6cf80 at 0x00C6D0CC: `8D 8D 9C 00 00 00 89 2D ?? ?? ?? ?? E8` dword at +8<br>alt: clear in FUN_00c6b500 at 0x00C6B507: `51 53 56 33 DB 8B F1 89 1D ?? ?? ?? ?? 8B 8E 6C 01 00 00` dword at +9 | matches 1 / 1 |
| TerrainUpdateCall | 0x00C6D68C | Sig | `8B 44 24 0C 50 8D 4C 24 14 51 8B 4E 58 E8 ?? ?? ?? ?? 80 BE 58 02 00 00 00` +10 (the call rel32 is wildcarded: the Frame Profiler redirects it)<br>alt: `51 8B 4E ?? E8 ?? ?? ?? ?? 80 BE 58 02 00 00 00 75` +1 | matches 1 / 1 (sigcheck.pl 2026-09-29: 155 of 155 ok) |

The ids after the first one of a `Multi` / `CallersOf` entry take the following matches by address: ArmSiteRemoval
0x6B60D3, ArmSiteRegister 0x6B6516, ArmSiteMoved 0x6B6618; QualitySite0 0xADB66B, QualitySite1 0xADB884; SetColourCall0..6
0x6C047D, 6C051D, 6C05C1, 6C1251, 6C15D1, 6C1891, 6C1B11; AddWorldLightsCall0/1 0x6C5816, 0x6C7094; LevelGatherCall0/1
0x6C6B08, 0x6C6B2D; SolvePointCall0..2 0x6A1187, 0x6A126F, 0x6A3336. On other builds the arm sites are named by that
order (removal, registration, moved: the order of their functions on Steam), which only matters for log text.

Removed from the Steam-only checks on other builds: the four `push BatchSamples` sites before the calls of FUN_006a31d0
(`.text` has 9 pushes of that global on Steam, so "all pushes" cannot identify them); on other builds BatchSamples comes
from a signature that is itself such a push.

### Ids not covered by the recorded check

At HEAD, `framework/game_addresses.cpp` lists 230 ids (207 table rows; the other ids are filled by `Multi`,
`CallersOf` and `SlotsOf` rows, by address). The rows below are the ids added after the last check recorded above. Their
Steam address and signatures are copied from `kInfo` and `kTable`, and the notes from the comments in
`framework/game_addresses.h`. Their match counts on Steam are not recorded in this page *(unverified here)*; run
`sigcheck.pl` to record them. The self-check in `ApexRadiance_LOG.txt` reports any id whose signature does not give
the fixed Steam address.

| Id | Steam VA | Kind | Signature(s) | Notes |
|---|---|---|---|---|
| LightVtable4 | 0x00FF4570 | LightType(LightJumpTable, 4) | - |  |
| LightVtable5 | 0x00FF4350 | LightType(LightJumpTable, 5) | - |  |
| LightVtable6 | 0x00FF4468 | LightType(LightJumpTable, 6) | - |  |
| LightVtable7 | 0x00FF43A8 | LightType(LightJumpTable, 7) | - |  |
| LightVtable8 | 0x00FF4408 | LightType(LightJumpTable, 8) | - |  |
| LightVtable9 | 0x00FF44C0 | LightType(LightJumpTable, 9) | - |  |
| LightVtable10 | 0x00FF4518 | LightType(LightJumpTable, 10) | - |  |
| LightColour4 | 0x006C1BC0 | Deref(LightVtable4, 0x10) | - |  |
| LightColour5 | 0x006C0690 | Deref(LightVtable5, 0x10) | - |  |
| LightColour6 | 0x006C1320 | Deref(LightVtable6, 0x10) | - |  |
| LightColour7 | 0x006C0AF0 | Deref(LightVtable7, 0x10) | - |  |
| LightColour8 | 0x006C0FE0 | Deref(LightVtable8, 0x10) | - |  |
| LightColour9 | 0x006C16D0 | Deref(LightVtable9, 0x10) | - |  |
| LightColour10 | 0x006C1980 | Deref(LightVtable10, 0x10) | - |  |
| LightEval4 | 0x006BFFB0 | Deref(LightVtable4, 0x4C) | - |  |
| LightEval5 | 0x006BE1C0 | Deref(LightVtable5, 0x4C) | - |  |
| LightEval6 | 0x006BFA70 | Deref(LightVtable6, 0x4C) | - |  |
| LightEval7 | 0x006BEFD0 | Deref(LightVtable7, 0x4C) | - |  |
| LightEval8 | 0x006BF880 | Deref(LightVtable8, 0x4C) | - |  |
| LightEval9 | 0x006BFBA0 | Deref(LightVtable9, 0x4C) | - |  |
| LightEval10 | 0x006BFDC0 | Deref(LightVtable10, 0x4C) | - |  |
| LightFilter | 0x006C7820 | Sig | `53 8B 5C 24 08 56 8B F1 8B 46 1C 3B 43 0C 75 ?? 8B 4E 20 F6 81 90 00 00 00 02 74` +0<br>alt: `8B 46 1C 3B 43 0C 75 ?? 8B 4E 20 F6 81 90 00 00 00 02 74` +-8 | FUN_006c7820: the registry entry filter of a story's light gather |
| LightBright | 0x006BC520 | InRange(LightFilter, 0x40) | `F6 81 00 01 00 00 20 74 ?? E8` call at +9<br>alt: `20 74 ?? E8 ?? ?? ?? ?? 84 C0 74` call at +3 | FUN_006bc520 fastcall(light): the light is bright enough to count |
| AddRoomLight | 0x006A2060 | InRange(LightFilter, 0x70) | `57 8B CB E8` call at +3<br>alt: `83 F8 0B 75 ?? 57 8B CB E8` call at +8 | FUN_006a2060 thiscall(room, light) ret 4: adds a light to a room's list |
| RoomUpdatePush | 0x006C5E2A | Sig | `56 6A 00 8B F1 E8 ?? ?? ?? ?? 68 ?? ?? ?? ?? 8B CE E8 ?? ?? ?? ?? 8B CE 5E E9` +10<br>alt: `6A 00 8B F1 E8 ?? ?? ?? ?? 68 ?? ?? ?? ?? 8B CE E8` +9 | push FUN_006c7250 in FUN_006c5e20: the per-story room update, run for every story of every lot |
| RoomUpdate | 0x006C7250 | Deref(RoomUpdatePush, 1) | - | FUN_006c7250 fastcall(treeLevel) |
| LightEntryUpdate | 0x006C7BA0 | Sig | `55 8B EC 83 E4 F0 83 EC 24 53 56 8B F1 83 7E 24 00 57 0F 84 ?? ?? ?? ?? 8B 46 14 85 C0 0F 84` +0<br>alt: `83 7E 24 00 57 0F 84 ?? ?? ?? ?? 8B 46 14 85 C0 0F 84 ?? ?? ?? ?? 83 38 00 0F 84` +-13 | FUN_006c7ba0 thiscall(entry): recomputes window room/sky activation |
| ChangedClearCall | 0x006C7497 | InRange(CascadeTest, 0x100) | `8D 7E 08 52 50 8B CF E8` +7<br>alt: `52 50 8B CF E8` +4 | its call that empties the "changed rooms" set (ecx = treeLevel+8) after walking it |
| ChangedClear | 0x007F3790 | Target(ChangedClearCall) | - | FUN_007f3790 thiscall(set, buckets, count) ret 8 |
| FloorSet | 0x00A89DD0 | Sig | `53 55 56 8B F1 83 BE 64 02 00 00 00 57 75 ?? 6A 00 6A 00 6A 00 6A 00 68` +0<br>alt: `83 BE 64 02 00 00 00 57 75 ?? 6A 00 6A 00 6A 00 6A 00 68` +-5 | FUN_00a89dd0 thiscall(level floor object, ...): sets a floor quadrant |
| FloorSetCall0 | 0x00AA0ADB | CallersOf(FloorSet, 4) | - |  |
| FloorRemove | 0x00A893A0 | Sig | `53 55 56 57 8B F9 83 BF 64 02 00 00 00 75 ?? 6A 00 6A 00 6A 00 6A 00 68` +0<br>alt: `57 8B F9 83 BF 64 02 00 00 00 75 ?? 6A 00` +-3 | FUN_00a893a0 thiscall(level floor object, ...): removes one |
| FloorRemoveCall | 0x00AA05C7 | CallersOf(FloorRemove, 1) | - |  |
| LevelVtable | 0x01062680 | Sig | `E8 ?? ?? ?? ?? C7 06 ?? ?? ?? ?? 88 9E 10 02 00 00 89 9E 14 02 00 00` dword at +7<br>alt: `80 BE 10 02 00 00 00 C7 06 ?? ?? ?? ?? 74` dword at +9 | vtable of the level floor object (ctor FUN_00a88790) |
| LevelCtorCall | 0x00AA179E | Sig | `83 C4 30 85 C0 74 0B 8B C8 E8 ?? ?? ?? ?? 8B F8` +9<br>alt: `68 50 03 00 00 ?? ?? ?? E8 ?? ?? ?? ?? 83 C4 30 85 C0 74 0B 8B C8 E8` +22 | the only CALL of that ctor (after "new 0x350"): every level floor object is made there |
| LevelCtor | 0x00A88790 | Target(LevelCtorCall) | - | FUN_00a88790 thiscall(object), returns it, plain ret |
| LodChoice | 0x0069E710 | Sig | `8B 11 8B 82 88 00 00 00 56 8B B2 84 02 00 00 3B C6 7E` +0<br>alt: `8B 82 88 00 00 00 56 8B B2 84 02 00 00 3B C6` +-2 | FUN_0069e710 fastcall(room): the lighting LOD class a room should have (high only on the camera's story) |
| LodChoiceCall0 | 0x0069E82E | CallersOf(LodChoice, 4) | - |  |
| LodMax | 0x01158B00 | Deref(LodChoice, 0x4C) | - | the int it returns for the camera's story (0x01158B00) |
| RoomSolveStartCall | 0x006A3D0B | Sig | `8B CE DD D8 E8 ?? ?? ?? ?? 88 9E 38 06 00 00 C7 86 EC 00 00 00 01 00 00 00` +4<br>alt: `E8 ?? ?? ?? ?? 88 9E 38 06 00 00 C7 86 EC 00 00 00 01` +0 | the CALL of state 0 of the room solve in FUN_006a3c90 |
| RoomSolveStart | 0x006A18B0 | Target(RoomSolveStartCall) | - | FUN_006a18b0 thiscall(room): ambient colour, normalisation and ambient ramp of an indoor room |
| WallPassCall | 0x006A3D4C | Sig | `D9 1C 24 57 8B CE E8 ?? ?? ?? ?? 84 C0 0F 84 ?? ?? ?? ?? 88 9E 38 06 00 00 C7 86 EC 00 00 00 03` +6<br>alt: `57 8B CE E8 ?? ?? ?? ?? 84 C0 0F 84 ?? ?? ?? ?? 88 9E 38 06 00 00 C7 86 EC 00 00 00 03` +3 | the CALL of the wall texel pass (state 2) in FUN_006a3c90 |
| WallPass | 0x006A3A30 | Target(WallPassCall) | - | FUN_006a3a30 thiscall(room, int, float) ret 8 |
| WallSamplesCall | 0x006A3AF5 | InRange(WallPass, 0x150) | `68 C8 8A 15 01 52 50 8B CF E8` +9<br>alt: `52 50 8B CF E8` +4 | its CALL of FUN_006ac070 thiscall(wall, piece, class, batch) ret 0xC: the samples of one piece of a wall |
| WallSamples | 0x006AC070 | Target(WallSamplesCall) | - |  |
| WallBlurCall | 0x006A3B62 | InRange(WallPass, 0x150) | `8B CE E8 ?? ?? ?? ?? 5F 5E 5D B0 01` +2<br>alt: `E8 ?? ?? ?? ?? 5F 5E 5D B0 01 5B` +0 | its CALL of FUN_0069f650 fastcall(room): the wall atlas blur (LOD class 2) |
| WallBlur | 0x0069F650 | Target(WallBlurCall) | - |  |
| WallBlurPasses | 0x01158B1C | Deref(WallBlur, 0x20) | - | the dword of blur passes (0x01158B1C) |
| WallBlurMode | 0x011D02E4 | Deref(WallBlur, 0x88) | - | the byte of blur mode (0x011D02E4; 0 = [1 2 1] per axis) |
| WallSolveCall | 0x006A3B0A | InRange(WallPass, 0x150) | `53 68 C8 8A 15 01 8B CE E8` +8<br>alt: `68 C8 8A 15 01 8B CE E8` +7 | its CALL of FUN_006a31d0 thiscall(room, batch, {base, pitch}, flags, sampler, char) ret 0x14 for one piece |
| WallSolve | 0x006A31D0 | Target(WallSolveCall) | - |  |
| RoomAmbient | 0x006A0F50 | Sig | `55 8B EC 83 E4 F0 81 EC 04 01 00 00 53 56 8B F1 8B 86 C8 00 00 00 3B 86 CC 00 00 00 57 75` +0<br>alt: `8B 86 C8 00 00 00 3B 86 CC 00 00 00 57 75 ?? 8B 56 14` +-16 | FUN_006a0f50 fastcall(room): the room's ambient (state 0 of the room solve) |
| UnlitColourA | 0x006A0F95 | InRange(RoomAmbient, 0x80) | `84 C0 B9 ?? ?? ?? ?? 75 05 B9 ?? ?? ?? ?? E8` +3<br>alt: `B9 ?? ?? ?? ?? 75 05 B9` +1 | in it: imm32 of "mov ecx, 0x011D0B60" (the colour of a room with no lamp, on some lots) |
| UnlitColourB | 0x006A0F9C | InRange(RoomAmbient, 0x80) | `84 C0 B9 ?? ?? ?? ?? 75 05 B9 ?? ?? ?? ?? E8` +10<br>alt: `B9 ?? ?? ?? ?? 75 05 B9` +8 | in it: imm32 of "mov ecx, 0x011D0B40" (the same, other lots) |
| DimAmbient | 0x006A00A0 | Sig | `55 8B EC 83 E4 F0 83 EC 3C 8B C1 8B 50 14 8B 40 10 8B 0D` +0<br>alt: `83 EC 3C 8B C1 8B 50 14 8B 40 10 8B 0D` +-6 | FUN_006a00a0: a dim room's ambient topped up with that colour |
| DimColourA | 0x006A00C2 | InRange(DimAmbient, 0x40) | `84 C0 B9 ?? ?? ?? ?? 75 05 B9 ?? ?? ?? ?? E8` +3<br>alt: `B9 ?? ?? ?? ?? 75 05 B9` +1 | in it: the same two imm32 |
| DimColourB | 0x006A00C9 | InRange(DimAmbient, 0x40) | `84 C0 B9 ?? ?? ?? ?? 75 05 B9 ?? ?? ?? ?? E8` +10<br>alt: `B9 ?? ?? ?? ?? 75 05 B9` +8 |  |
| DimAmbientCall0 | 0x006A13F0 | CallersOf(DimAmbient, 2) | - |  |
| FillGate | 0x006BA57E | Sig | `80 3D ?? ?? ?? ?? 00 74 26 8B CF E8` +2<br>alt: `84 9F 24 02 00 00 74 2F 80 3D ?? ?? ?? ?? 00` +10 | imm32 of "cmp byte [0x01158D5C], 0" in FUN_006ba340: the fill light added to object rigs |
| FillColour | 0x006B816D | Sig | `0F 28 15 ?? ?? ?? ?? 0F 58 C1 0F 59 C5 0F 57 C9` +3<br>alt: `0F 28 15 ?? ?? ?? ?? 0F 58 C1 0F 59 C5` +3 | imm32 of "movaps xmm2, [0x011D0E10]" in FUN_006b7e70: the fill light's colour (0.8, 0.8, 1, 0.8) |
| RoomPriorityCall | 0x006A81DF | Sig | `8B CF E8 ?? ?? ?? ?? 51 D9 1C 24 57 8D 4C 24 20 E8` +2 | the only CALL of the room priority (in FUN_006a8190) |
| RoomPriority | 0x0069E770 | Target(RoomPriorityCall) | `83 EC 0C 56 8B F1 57 8B 3E 85 FF 75 08 D9 EE` +0 | FUN_0069e770 fastcall(room) -> float in ST0 |
| LodStepSite | 0x0069EAA2 | Sig | `85 FF 75 0A BF 01 00 00 00 8D 5F 01 EB 0C` +4 | "mov edi,1; lea ebx,[edi+1]" in FUN_0069ea70: class 0 -> 1 after a solve |
| KeepClassA | 0x0069EF58 | Sig | `83 F9 04 74 0F 3B C8 7C 0B 5F 89 86 F4 00 00 00` +7 | "jl" in FUN_0069eed0: an invalidated room restarts at class 0 when shown < LodChoice |
| KeepClassB | 0x0069F1C5 | Sig | `83 F9 04 74 06 3B C8 7C 02 8B F8 89 BE F4 00 00 00` +7 | the same "jl" in FUN_0069f160 |
| InvalidateFlag | 0x0069F160 | Sig | `8A 44 24 04 56 8B F1 3A 46 19 74 ?? 8B 0E 85 C9 88 46 19` +0 | FUN_0069f160 thiscall(room, char flag) ret 4: invalidates a room when its +0x19 flag changes (from 0x006A5E00) |
| RoomPickJump | 0x006C5E39 | Sig | `8B CE 5E E9 ?? ?? ?? ?? CC CC 8B 4C 24 04` +3 | "jmp FUN_006c5c20" at the end of FUN_006c5e20 (the per-frame light tree update) |
| RoomPick | 0x006C5C20 | Sig | `81 EC 14 04 00 00 55 8B E9 83 7D 74 00 0F 85` +0 | FUN_006c5c20 fastcall(tree): makes the best pending room current |
| RoomSolveStep | 0x006A3F80 | Sig | `83 B9 F0 00 00 00 03 75 12` +0 | FUN_006a3f80 thiscall(room, stopwatch*, float budget) ret 8: the budgeted solve of a room in state 3 |
| StopwatchCtor | 0x004F35B0 | Sig | `6A 04 8D 4C 24 18 8D 6C 10 C0 E8 ?? ?? ?? ?? 8D 4C 24 10 E8` call at +10 | FUN_004f35b0 thiscall(sw, kind, char start) ret 8 (kind 4 = ms) |
| StopwatchStart | 0x00408700 | Sig | `6A 04 8D 4C 24 18 8D 6C 10 C0 E8 ?? ?? ?? ?? 8D 4C 24 10 E8` call at +19 | FUN_00408700 thiscall(sw) |
| StopwatchElapsed | 0x004F33C0 | Sig | `8D 4C 24 14 E8 ?? ?? ?? ?? D9 44 24 10 D9 C9` call at +4 | FUN_004f33c0 thiscall(sw) -> ST0 |
| PriorityLotObject | 0x006FDE10 | Sig | `40 4C 50 51 E8 ?? ?? ?? ?? 8B C8 E8 ?? ?? ?? ?? 84 C0` call at +4 | FUN_006fde10: mov eax,[SceneObjectManager]; ret |
| PriorityLotTest | 0x006FDC80 | Sig | `40 4C 50 51 E8 ?? ?? ?? ?? 8B C8 E8 ?? ?? ?? ?? 84 C0` call at +11 | FUN_006fdc80 thiscall(som, lotLo, lotHi) ret 8 -> al: one of the two priority lots |
| BakeColourSite | 0x00C2950F | Sig | `E8 ?? ?? ?? ?? 0F 28 87 F0 00 00 00 0F 29 86 20 01 00 00 8B 17 0F 28 47 10` +5<br>alt: `0F 28 87 F0 00 00 00 0F 29 86 20 01 00 00 8B 17` +0 | movaps xmm0,[edi+0F0h] in the terrain bake FUN_00c292b0: the lamp colour copied to its shader parameter |
| SunlightScale | 0x011D0918 | Sig | `B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? F3 0F 10 00 0F 28 8E 00 08 00 00` dword at +1<br>alt: `B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? F3 0F 10 00 0F 28 8E ?? ?? 00 00 8D 8E` dword at +1 | the "Sunlight Scale" float FUN_00c11ad0 multiplies the sun / moon colour by (mov ecx,imm32 at 0x00C11B01) |
| CasTriSort | 0x005D1960 | Sig | `55 8B EC 83 E4 F0 81 EC A4 00 00 00 33 C0 89 44 24 08 89 44 24 0C 53 8D 44 24 0C 8B C8 89 44 24 0C 33 C0 56 57 89 44 24 20 89 44 24 24 89 44 24 28 8D 54 24 20 52 B8 AB AA AA AA F7 65 10` +0 | FUN_005d1960 cdecl(u16* indices, u8* vertices, u32 indexCount, u32 vertexCount, u16 stride, u8 offset): "CAS/ModelBuilder/TriangleSortDataList" (`features/cas_tri_sort.h`) |
| AllocGlobal | 0x011CB864 | Sig | `8B 44 24 0C 8B 4C 24 04 50 51 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? C3` dword at +12 | the global general allocator pointer 0x011CB864 (operator new FUN_004e3f90: mov ecx,[global]) |
| AllocMmapFreeCall | 0x004E5306 | Sig | `29 9F 8C 04 00 00 83 87 88 04 00 00 FF 51 FF 15 ?? ?? ?? ??` +14 | "call [VirtualFree]" in the allocator's FreeInternal FUN_004e51b0 (0x004E5306): releases a big block |
| RecordCrc | 0x004FA4C0 | Sig | `8B 4C 24 04 8B 44 24 08 8D 14 01 3B CA 8B 44 24 0C 73 1F 56 57 0F B6 39 8B F0 C1 EE 18 33 F7 C1 E0 08 33 04 B5 ?? ?? ?? ?? 83 C1 01 3B CA 72 E5 5F 5E 80 7C 24 10 00 74 02 F7 D0 C3` +0 | FUN_004fa4c0 cdecl(bytes, length, crc, bool invert): MSB-first table CRC-32 of the cache records |
| RecordCrcTable | 0x0114D330 | Deref(RecordCrc, 0x25) | - | its 256-entry table (0x0114D330) |
| TexCreateCall | 0x0060E1DC | Sig | `8B 45 DC 50 8B 4D F4 51 8B 55 08 52 E8 ?? ?? ?? ?? 83 C4 24` +12 | call FUN_0060cea0 (0x0060E1DC): creates the D3D texture (cdecl, 9 args) |
| TexCreate | 0x0060CEA0 | Target(TexCreateCall) | - |  |
| TexFillCall | 0x0060E1FF | Sig | `8A 4D FB 51 8B 55 10 52 8B 45 0C 50 8B 4D 08 51 E8 ?? ?? ?? ?? 83 C4 10` +16 | call FUN_0060d290 (0x0060E1FF): copies every mip level into it (cdecl, 4 args) |
| TexFill | 0x0060D290 | Target(TexFillCall) | - |  |
| UiServiceGetter | 0x0050AB70 | Sig | `56 57 E8 ?? ?? ?? ?? 8B F8 E8 ?? ?? ?? ?? 85 C0 74 ?? 8B 10 8B C8 8B 42 04 FF D0 8B F0 85 F6` call at +9 | UIManager_GetMainWindowImpl calls this read-only root-service getter |

Ids filled by a `Multi`, `CallersOf` or `SlotsOf` row (in address order after the first id of their row):
SetColourCall1..6 (0x006C051D, 0x006C05C1, 0x006C1251, 0x006C15D1, 0x006C1891, 0x006C1B11), AddWorldLightsCall1
(0x006C7094), LevelGatherCall1 (0x006C6B2D), SolvePointCall1..2 (0x006A126F, 0x006A3336), FloorSetCall1..3 (0x00AA0CCC,
0x00AA0E4A, 0x00AA0F72), LodChoiceCall1..3 (0x0069EA86, 0x0069EF46, 0x0069F1B3), DimAmbientCall1 (0x006A1410),
ResFindProviderSlot1 (0x00FFE290), ResSetDbPrioritySlot1 (0x00FFE28C), ResDbChangedSlot1 (0x00FFE29C).

## See also

- [Architecture: game-code patching helpers](../architecture.md#45-game-code-patching-helpers).
- [Light objects and rigs](light-objects-and-rigs.md), [Room light maps](room-light-maps.md), [Terrain and light bake](terrain-and-light-bake.md): what the Night Lighting ids point at.
- [Performance](../features/performance/README.md): the performance ids.
- [Workflow: diagnosing](../workflow.md#5-diagnosing-a-problem).
