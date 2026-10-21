# Faster Scripts

The game's scripts (Sim decisions, routing, timers) run on the Mono interpreter built into `TS3W.exe`. Faster Scripts
removes two pieces of extra work it does constantly: every comparison of two decimal numbers called a separate library
to check each number first, and every look-up of a type's information went through a lock and a search. The results
are the same; scripts simply do less work. Nothing on screen changes.

## Status

| | |
|---|---|
| Availability | Released in 2.7.0 (Experimental until 2.15.0) |
| Default | On |
| Menu | System > Performance > Game and scripts > *Faster scripts* |
| Configuration | `[patches.ScriptMath]` in `ApexRadiance.toml` |
| Source | [`features/script_math.{h,cpp}`](../../../features/script_math.cpp), [`patches/performance_patches.cpp`](../../../patches/performance_patches.cpp) (`ScriptMathPatch`) |

## The problem

- **NaN tests.** The interpreter's 25 floating-point compare and branch handlers test both operands for NaN by calling
  `msvcr80!_isnan` through a register before comparing them: two library calls per comparison.
- **Type objects.** `mono_type_get_object` (0x00EA8A00) takes a lock and searches a hash table for the reflection
  object of a type, on every call, though the answer for a (domain, type) pair never changes while the domain lives.

## How Apex Radiance solves it

The two parts start independently; the switch is on when at least one of them could start.

1. **NaN tests inline.** In each of the 25 handlers, both test blocks become the same test without the call: `fucomip
   st0, st0` (which pops and sets the parity flag only for a NaN) and `jnp`, leaving `eax` 0 or 1 exactly as `_isnan`
   returned it. The registers, the x87 stack and the flags the following jumps read are those of the original.
2. **Type object cache.** `mono_type_get_object` answers from a lock-free (domain, type) cache filled only with the
   game's own answers. `mono_domain_free` (0x00E75340) bumps a generation first, so nothing stored for a freed domain is
   answered again. Answers equal to the class's `reflection_info` (TypeBuilder types) are never stored. The first 256
   answers from the cache are also compared with the game's; a difference turns the cache off for the session.

## Settings

| Menu label | TOML key | Type | Default | Range | Effect |
|---|---|---|---|---|---|
| Faster scripts | `[patches.ScriptMath] enabled` | bool | on | | Turns both parts on. A missing key reads as on |

The row is part of the Overview's Performance group switch. It is not in the Performance part of profiles: applying a
profile leaves it as it is.

## Compatibility and interactions

- **Mods that patch the Mono interpreter** (Sims3Performance's isinst cache at 0x00EA3820, its Mono ehash at
  0x00EBBDE0 / 0x00EA8720, MonoPatcher) touch other functions. A handler whose bytes differ from the expected pattern is
  left alone.
- **Game build:** Steam 1.67.2 and the EA app build. The NaN handlers are found by pattern in `.text` on any build; the
  type cache uses the address group `ScriptTypeCache` (signatures of `mono_type_get_object` and `mono_domain_free`).

## Limitations

The gameplay candidate adds a per-thread last-result cache using the same domain/type/generation key as the shared
table. A domain free or restart invalidates both levels; cacheable results still come from the game and retain the
startup checks. Shared fields are atomic under the existing sequence protocol. No managed allocation, GC budget or
collection frequency changes. This adds 16 bytes per participating thread on the 32-bit build.

- The gain depends on how much script work a household runs; it has not been measured in frames.
- If the type cache finds a difference it turns itself off for the session and the game's answer is used (logged).

## Technical reference

**NaN handlers** (0x00E54D1B..0x00E58304 on Steam 1.67.2), each:

```
fld qword [esi-10h]                         DD 46 F0
mov ebx, [_isnan import slot]               8B 1D <slot>       \
sub esi, 10h | 8                            83 EE xx            |
sub esp, 8 ; fstp qword [esp]               83 EC 08 DD 1C 24   | block A (22 bytes)
call ebx ; add esp, 8 ; test eax, eax       FF D3 83 C4 08 85 C0 /
jnz <nan path>                              0F 85 rel32 | 75 rel8
fld qword [esi+8] | [esi]                   DD 46 08 | DD 06
sub esp, 8 ; fstp qword [esp] ; call ebx ; add esp, 8 ; test eax, eax     block B (13 bytes)
```

Both blocks become two pushes for the old `sub esp, 8`, `fucomip st0, st0`, `jnp +1`, `inc eax` (`eax` zeroed first
in A, already 0 in B). The bytes from the old return address on (A +17, B +8) are kept, so a thread inside `_isnan`
during the switch returns into the same code; `ebx` still receives `_isnan`'s address (`mov ebx, imm32`). Start checks
that the slot holds `msvcr80!_isnan` and that it returns exactly 0 or 1, and no branch in `TS3W.exe` targets the inside
of a block. The writes are made with every other thread suspended outside the changed bytes.

**Type cache:** `EntryChain` sites `MonoTypeGetObject` (cdecl(domain, type)) and `MonoDomainFree` (cdecl(domain,
force)), layer `ScriptMath`. Entries are written with a sequence lock and read without a lock.

Log lines: `[ScriptMath] N interpreter compare handlers test NaN inline (...)`, `[ScriptMath] Type objects
(mono_type_get_object ...) answered from a cache; the first 256 answers are checked against the game`, a warning for a
part that could not start, and at Stop a summary of calls, cache answers, checks, differences and domains freed.

## Rejected approaches

None recorded.

## See also

- [Performance overview](README.md)
- [Validation](../../validation/performance-faster-scripts.md)
- [History](../../history/performance-faster-scripts.md)
- [Engine: Mono and the GC](../../engine/mono-gc.md)
