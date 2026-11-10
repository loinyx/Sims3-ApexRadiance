#pragma once
// Game fix: a script delegate with no parameters can crash the simulator thread (docs/features/delegate-signature-fix.md).
//
// The game's delegate check (Steam 1.67.2: 0x00D70F90) asks whether the delegate's Invoke takes one Object[] parameter.
// It reads sig->params[0] ([esi+0x10]) and passes it to mono_type_get_type before it tests param_count ([esi+4]) == 1.
// A zero-parameter MonoMethodSignature is 16 bytes, so that read lands on whatever Mono allocated next; when that is
// another signature's first dword (1), mono_type_get_type reads address 7 and the game crashes. Install retargets only
// the CALL at the read to a stub that returns 0 (not SZARRAY) when param_count != 1 and otherwise jumps to the original;
// the game's own param_count test after the call then returns false, as it always did for those signatures.
// Always on, no menu row. Found by byte pattern; not found or already changed: off, logged.
namespace DelegateSigFix {

// After GameAddr::Resolve (game code readable). Once per process; logs the result.
void Install();

} // namespace DelegateSigFix
