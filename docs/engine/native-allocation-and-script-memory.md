# Native allocation and script memory

## Scope

Steam 1.67.2.024037, PE timestamp `0x52DEC247`, base `0x00400000`. Static executable validation establishes the
counter paths below; runtime ownership, aliases and reclamation effects require separate captures.

## Allocation domains

The primary native allocator global is `0x011CB864`. Calls at `0x004E3F9A` load it before entering the native
allocator. Runtime support wrappers also load `0x011D4BC8` before calling the same allocator entry `0x004E69D0`;
its free wrapper calls `0x004E5920`. These two globals may point to the same object. Reads must not be summed until
pointer identities and coverage are checked.

Native allocator fields `+0x488/+0x48C` count the dedicated large-allocation path: `0x004E50F0` adds the size and one
block after VirtualAlloc succeeds; its free path subtracts them. They do not count all allocations served by native
cores/pools. `+0x494` is the existing large-block threshold; field meaning beyond this path needs runtime validation.

The byte at `+0x498` selects `MEM_TOP_DOWN` in this native allocation path: flags are `0x3000` or `0x103000`. This is
an existing engine option, not a change applied by the snapshot tool. Global activation is not justified until pointer
consumers and workload effects are verified. A low-region free-block minimum alone does not prove allocation failure.

## Script heap accounting

`0x00E4DD30` reads heap bytes from `0x012225A0`; `0x00E4DD40` reads free heap bytes from `0x012225B4`. The wrapper at
`0x00E69670` subtracts those values to report Mono used size. This subtraction is not exact reachable-object memory:
collector accounting, retained objects and internal fragmentation remain separate questions. Heap growth checks the
value at `0x012225A4` against the expanded total when nonzero (`0x00E49CA3` onward). Zero bypasses that particular check;
it does not remove address-space limits.

Runtime wrappers at `0x00D70900` and `0x00D9AAF0` call VirtualAlloc with `MEM_RESERVE|MEM_COMMIT|MEM_WRITE_WATCH` and
`PAGE_EXECUTE_READWRITE`. This demonstrates another allocation path outside the five existing FastMemory sites.
Executable private pages still cannot all be attributed to the managed heap from page protection alone. The scan of
the on-disk executable finds seven direct calls through this VirtualAlloc import slot; indirect/dynamic imports and
other modules remain outside that count.

## Read-only capture

`tools/memory_snapshot/snapshot.cpp` queries a running 32-bit process from outside the game. VirtualQueryEx groups
regions by allocation base; GetMappedFileName identifies mapped files/images. Optional QueryWorkingSetEx reports
resident private and shareable pages without deliberately touching the target pages. Shareable is the Windows flag,
not proportional physical usage. Image/mapped copy-on-write pages can be private while retaining their region type.

Fixed game counters require the Steam PE header and exact getter bytes. Native counter reads check the embedded
critical-section layout and reject changing counts. The companion PowerShell script accepts TS3W only. Cache-pointer
reads additionally require a matching Apex file SHA-256 and the native ResourceSystem vtable; the known pointer RVA
is supported only for explicitly hashed artifacts in the capture script. Cache fields are raw accounting, not guaranteed reclaimable RAM.

## Limitations

Captures are sequential while the process continues; neither stable reads nor signatures make them atomic. No game
function is invoked, no thread is suspended, and no allocation is freed or trimmed. The tool does not walk native or
managed object graphs. Unknown/private blocks retain unknown ownership. Live cache reads and script counter values
remain unverified until the game is running.
