# Bone Index Guard

A game fix that stops a crash while a Sim is being built: a Sim part (a hair, clothing or accessory mesh, usually
custom content) whose vertices name a bone its own bone list does not have made the game read past that list. With the
fix such a vertex is tied to the part's first bone instead. Parts without the defect are built exactly as before.

## Status

| | |
|---|---|
| Availability | Released in 3.1.3 |
| Default | Always on; no menu row |
| Menu | None |
| Configuration | None |
| Source | [`features/bone_index_guard.{h,cpp}`](../../features/bone_index_guard.cpp) |

## The problem

When the CAS model builder fills a part's vertex buffer (`0x005D1010`, called from FillDrawable `0x005D3760`, Steam
1.67.2), it maps every vertex's bone byte through the part's bone list with a virtual getter (vtable slot `+0x54`,
`0x005C6E00`): `return list[index]`, the list being the vector at `+0x58` / `+0x5C`. There is no bounds check. In the
crash this fix answers, a part with a list of 1 bone asked for bone 109; the list sat in the last 16 bytes of a 16 MB
allocator region, so the read landed in free address space: access violation at `TS3W.exe+0x1C6E07` reading
`0x7F9501A4`, on the main thread, with no Apex code on that path. Where the list does not end a region the read returns
whatever lies after it, so the crash comes and goes with the memory layout.

## How Apex Radiance solves it

1. At start-up the byte pattern of the getter and of the count getter in front of it
   (`8B 41 5C 2B 41 58 C1 F8 02 C3 CC CC CC CC CC CC 8B 41 58 8B 4C 24 04 8B 04 88 C2 04 00`) is searched in the game
   executable.
2. The getter's first instruction becomes a jump to Apex (`MemPatch::WriteCodeSuspended`). Apex reads the same vector:
   a valid index returns its entry; an index past the list returns the first entry (0 for an empty list) and the first
   eight cases are logged.

## Settings

None.

## Compatibility and interactions

- Every caller of the getter gets the same answer for a valid index.

## Limitations

- A broken part is drawn with those vertices following its first bone; the part itself is not repaired.
- A game build where neither pattern is found once keeps the fix off and logs it. EA app 1.69: the patterns are checked at
  start-up on the decrypted code (its executable is encrypted on disk); the log line says which one was found.

## Technical reference

- Log: `[BoneIndexGuard] On at 0x005C6E00: ...`; `[BoneIndexGuard] A Sim part asked for bone N of M (caller ...)` when
  a broken part is built.
- Installed in `InitBody` after `GameAddr::Resolve`, next to the delegate signature fix (`apex_main.cpp`).
- The jump covers the getter's first instruction (3 bytes) and part of the second; no thread is stopped inside those
  5 bytes when they are written, and the rest of the getter is unchanged.

## Rejected approaches

None.

## See also

- [Validation](../validation/bone-index-guard.md)
