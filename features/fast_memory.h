#pragma once
// Faster memory handling (Apex Radiance, feature "FastMemory"; docs/features/performance.md, "How it works: Faster Memory
// Handling").
//
// The game has one general allocator for every thread (EA PPMalloc, dlmalloc style; global pointer 0x011CB864) behind a
// single critical section (allocator+0x4E8, pointer at +0x4E4). The loading RE of 30/09 (research notes loadre\memory.md)
// found two costs on the render thread:
//   - The critical section is created with InitializeCriticalSectionAndSpinCount(cs, 10): a waiting thread spins about
//     24 ns and then sleeps, and every hand-over is a sleep and a wake-up. The game's other locks get Windows' default of
//     2000 (about 5 us). This sets the allocator's to 2000 too (SetCriticalSectionSpinCount; only the spin changes).
//   - Blocks of 128 KB and more get their own VirtualAlloc, and freeing one calls VirtualFree(MEM_RELEASE) (0x004E5306)
//     while the lock is held: the unmap blocks the freeing thread and every thread waiting for the allocator. That call
//     is redirected to a queue that a helper thread releases at once, outside the lock. The allocator's state is the same
//     (it ignores the call's result); the address range is only returned to Windows microseconds later.
//     So that the delay can never make an allocation fail, the allocator's five VirtualAlloc calls go through Apex too:
//     when one fails while releases are queued, the queue is released on the spot and the call is made again. The
//     decommit of a core's tail (0x004E504C) is never deferred: the allocator may commit that range again right after.
// Every call site is checked byte for byte before it is rewritten and written back on Stop.
#include <cstdint>
#include <string>

namespace FastMemory {

bool Start(std::string* error);
void Stop();
bool Running();
// Drain blocks already released by the game before entering its native world save.
uint32_t PrepareForSave();

struct Stats {
    uint32_t spinBefore = 0, spinNow = 0; // the allocator lock's spin count (0 = not read)
    bool deferring = false;               // the big-block release goes through the queue
    uint64_t deferred = 0;                // releases done by the helper thread
    uint64_t direct = 0;                  // released on the calling thread (queue full, stopping, not a plain release)
    uint32_t maxQueued = 0;
    uint64_t allocRetries = 0, allocRetryFailed = 0; // a VirtualAlloc of the allocator failed with releases queued
    double releaseMs = 0.0;                          // time the helper thread spent in VirtualFree
};
Stats GetStats();
std::string StatusText();
void RenderDeveloperUI();

} // namespace FastMemory
