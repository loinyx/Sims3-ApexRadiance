# Delegate Signature Fix

A game fix that stops a crash on the simulator thread: when a script callback that takes no arguments runs (for example
the one registered to run when the loading screen goes away), the game could read past the end of the callback's
description and crash. With the fix the callback is checked the same way, without the out-of-bounds read. Nothing
drawn or simulated changes.

## Status

| | |
|---|---|
| Availability | Released in 3.1.1 |
| Default | Always on; no menu row |
| Menu | None |
| Configuration | None |
| Source | [`features/delegate_sig_fix.{h,cpp}`](../../features/delegate_sig_fix.cpp) |

## The problem

Before it invokes a script delegate, the game asks whether the delegate's `Invoke` method takes a single `Object[]`
parameter (the function at `0x00D70F90`, Steam 1.67.2). It reads the first parameter type, `sig->params[0]` at
`[esi+0x10]` of the Mono `MonoMethodSignature`, and passes it to `mono_type_get_type` (`0x00F4AC60`) before it tests
`param_count` (`[esi+4]`) against 1. A signature with no parameters is 16 bytes long, so that read returns whatever Mono
allocated next in the image's memory pool. When the next object is another signature, the value read is that
signature's first dword (1), and `mono_type_get_type` faults reading address `0x00000007`
(`TS3W.exe+0xB4AC64`). Whether it crashes depends on the pool layout, so the crash comes and goes between sessions
and mod sets. See [Mono and the simulation thread](../engine/mono-gc.md).

## How Apex Radiance solves it

Only the call at the out-of-bounds read is retargeted; the game's code around it is unchanged:

1. At start-up the byte pattern `8B 7E 10 57 E8 ?? ?? ?? ?? 83 C4 04 66 83 7E 04 01 75 ?? 83 F8 1D 75` is searched in
   the game executable, and the call's target is checked to be `mono_type_get_type`.
2. The `call` at the pattern's offset 4 (`0x00D70FB6`) is rewritten to call a small stub, with every other thread
   suspended and none inside the instruction (`MemPatch::WriteCodeSuspended`).
3. The stub returns 0 (not an array type) when `param_count` is not 1, and otherwise jumps to the game's function. The
   game's own `param_count` test right after the call already returned false for those signatures, so every result is
   the same as before; only the read of the missing parameter is gone.

## Settings

None.

## Compatibility and interactions

- A mod that already changed this call (its target is no longer `mono_type_get_type`) is left alone and the fix stays
  off.
- Works beside sims3-modernization, which reaches the same check through the game's delegate invoke.

## Limitations

- A game build where the pattern is not found keeps the fix off and logs it ([validation](../validation/delegate-signature-fix.md)).

## Technical reference

- Log: `[DelegateSigFix] On at 0x00D70FB6: ...`, or a warning naming why it stays off (pattern not found, unexpected
  call target, write failed).
- Installed in `InitBody` after `GameAddr::Resolve`, next to the other start-up installs (`apex_main.cpp`).
- The stub keeps the call's return address, so a thread already inside `mono_type_get_type` while the call is rewritten
  returns normally.

## Rejected approaches

- Reordering the 19 to 24 bytes at `0x00D70FB2` so the count is tested first: a thread stopped inside
  `mono_type_get_type` during the write would return into the middle of the rewritten code.

## See also

- [Validation](../validation/delegate-signature-fix.md)
