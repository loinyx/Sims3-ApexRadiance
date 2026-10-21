# Native world-save result

## Scope

Steam 1.67.2.024037, executable timestamp `0x52DEC247`, image base `0x00400000`. Static control-flow inspection;
runtime failure attribution and other executable versions remain unverified.

## Evidence

The caller at `0x00AAC320` invokes `0x00C6D460`, tests AL and returns 12 on false. Earlier operations in the caller
have separate result paths, including 9, 10 and 11; a later operation can return 13.

Inside `0x00C6D460`, `0x00C6BCC0` returns the initial boolean at `0x00C6D529`. `0x00C61DC0` runs next and its result
is ANDed with the first. `0x00C62860` runs only when the combined result is true. The final AL is returned after
cleanup. A single outer failure therefore does not identify which stage failed or demonstrate allocation failure.

The second stage walks the pointer vector at world-manager offsets `+0x9C/+0xA0`, reads object fields `+0x48/+0x4C`
and calls virtual slot `+0x40`. It also has early false paths for missing input, stream setup and `+0xB0`. Object
ownership and the meaning of those fields require separate validation.

Helper `0x004DDD20`, called by several stages, writes eight bytes through stream slot `+0x38`, after optional
conversion through `0x009DAD00`. It is not evidence of a large byte-buffer copy and should not be replaced on that
assumption. Helper `0x007D7840` calls stream slots `+0x20` and `+0x08`, then clears its wrapper pointers; this alone
does not establish which backing allocation is released.

## Application

The existing MemoryGuard hook brackets the outer native world-save call. Its private-memory endpoints do not measure
the peak. The public experiment additionally wraps the direct dispatch call at `0x00AB12AF` to `0x00AAC110`, after
the caller pushes two arguments and loads ECX. This dispatch returns an integer result with `ret 8`; the wrapper
preserves that result, keeps a nested save scope and releases only the reserve, already-deferred frees and inactive
Apex compression storage. It does not invoke native cache trimming from an unverified thread.
When sims3-modernization's `ts3native.asi` is loaded, the entry at `0x00AAC110` can start with a `jmp rel32` into that
module (its detour). Apex accepts that form only when the target lies inside the loaded module; the call bytes at
`0x00AB12AF` are checked as before ([room-to-save.md](../features/performance/room-to-save.md)).
Calls at `0x00C6D529`, `0x00C6D537` and `0x00C6D544` report the three stage outcomes with unchanged thiscall arguments
and boolean returns. Stage memory endpoints remain distinct from a peak measurement.
Replacing serialization with streaming requires establishing the stream format and seek/backpatch requirements.

## Limitations

Static paths do not establish which path ran in a reported Error 12, how much memory each stage needs or whether
failure originates in a script callback, stream operation or allocation. No serialization behaviour is changed.
