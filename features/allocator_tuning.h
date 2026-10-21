#pragma once
// Game allocator tuning (numbered development builds; switches in dev_switches.h).
//
// The general allocator at [0x011CB864] keeps its cores (16 MB reservations) for the whole session and only decommits
// the free top of a core from inside a free when that top is at least +0x4C8 (256 KB). Its own trim, 0x004E4F90
// thiscall(pad), ret 4, walks the core list under the allocator's lock and decommits every trimmable core's free top down
// to `pad`, returning the bytes decommitted; the allocator recommits from core[1] on demand (0x004E54D0).
//   allocator_trim      calls that trim every 60 s (keeping 1 MB at each core top) and when a world save is prepared (private memory only; the
//                       address space stays reserved)
//   allocator_top_down  sets the allocator's own +0x498 flag at start-up, so its large blocks (>= 128 KB, own
//                       VirtualAlloc, short-lived) and fallback cores are placed from the top of the address space and
//                       stop cutting the middle into pieces
// Steam 1.67.2 only (fixed addresses, bytes checked). Live data is never touched: rendering is unchanged.
namespace AllocatorTuning {

void Install();          // init thread, after the dev switches are loaded
void Pump();             // init thread's pump: periodic trim
unsigned TrimNow(const char* why); // bytes decommitted (0 when off)

} // namespace AllocatorTuning
