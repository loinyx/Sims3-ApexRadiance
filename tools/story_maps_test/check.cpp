// StoryMapsInclude over a synthetic story manager laid out as the game's: three map classes, then the room hash.
// Part of Apex Radiance. Credits: @loinyx
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <vector>
static long g_faults = 0;
static LONG CALLBACK CountFault(EXCEPTION_POINTERS* e) {
    if (e->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) g_faults++;
    return EXCEPTION_CONTINUE_SEARCH;
}
#include "production.h"
struct Wrapper {
    uint8_t pad[0x2C];
    uint32_t kind;
    uintptr_t tex;
};
int main() {
    AddVectoredExceptionHandler(1, CountFault);
    std::vector<uint8_t> mgr(0x300);
    const auto at = [&](size_t off) -> uintptr_t& { return *reinterpret_cast<uintptr_t*>(mgr.data() + off); };
    const uintptr_t m = reinterpret_cast<uintptr_t>(mgr.data());
    Wrapper own{{}, 0, 0x1234}, cube{{}, 1, 0x5678};
    at(0x16C + 2 * 64 + 12) = reinterpret_cast<uintptr_t>(&own);
    at(0x16C + 4) = reinterpret_cast<uintptr_t>(&cube);
    at(0x230) = 0xD73916FE; // what the reports read in the slots past the third class
    at(0x238) = 0xB;
    StoryMapSlot slot;
    int failed = 0;
    if (!StoryMapsInclude(m, 0x1234, slot) || slot.wrapper != reinterpret_cast<uintptr_t>(&own) || !StoryMapSlotMatches(slot, 0x1234)) failed++;
    if (StoryMapsInclude(m, 0x5678, slot) || slot.wrapper) failed++; // a cube is not a 2D map
    for (int i = 0; i < 1000; i++)
        if (StoryMapsInclude(m, 0x9999, slot)) failed++; // the manager that does not hold the map
    if (g_faults) failed++;
    std::printf("%d failed, %ld access violations\n", failed, g_faults);
    return failed != 0;
}
