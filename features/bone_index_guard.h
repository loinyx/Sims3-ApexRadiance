#pragma once
// Game fix: a Sim part whose vertices name a bone past its own bone list no longer crashes the Sim model builder
// (docs/features/bone-index-guard.md).
//
// The CAS model builder's vertex fill (Steam 1.67.2: 0x005D1010, from FillDrawable 0x005D3760) maps each vertex's bone
// byte through the part's bone list with a virtual getter (vtable slot +0x54, 0x005C6E00: return list[index], list =
// the vector at +0x58 / +0x5C), with no bounds check. A broken part (seen: index 109 into a list of 1) reads past the
// vector; when the vector ends a heap region the read faults (access violation at 0x005C6E07). The getter's entry jumps
// to Apex, which returns the same entry for a valid index and the part's first bone for an index past the list (0 when
// the list is empty), and logs the first cases.
// Always on, no menu row. Found by byte pattern (the getter and the count getter before it); not found: off, logged.
namespace BoneIndexGuard {

// After GameAddr::Resolve (game code readable). Once per process; logs the result.
void Install();

} // namespace BoneIndexGuard
