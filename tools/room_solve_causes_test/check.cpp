// Offline checks of the room solve causes counters (features/room_solve_causes.cpp). Credits: @loinyx
#include "room_solve_causes.h"
#include <cstdio>
#include <string>

static int g_fail = 0;
static void Check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) g_fail++;
}
static bool Has(const std::string& s, const char* part) { return s.find(part) != std::string::npos; }

int main() {
    using namespace RoomSolveCauses;
    std::string line;
    Check(!Frame(false, 1000, &line), "the first frame starts the window");
    // camera still: two callers, one of them on a flag change; one room solved three times (classes 0, 1, 2), another once
    NoteInvalidate(0x006C7451, false);
    NoteInvalidate(0x006C7451, false);
    NoteInvalidate(0x0069F160, true);
    NoteSolve(0x1000, 3, 1, 77, 0);
    NoteSolve(0x1000, 3, 1, 77, 1);
    NoteSolve(0x1000, 3, 1, 77, 2);
    NoteSolve(0x2000, 0, 0, 77, 2);
    Check(!Frame(true, 30000, &line), "no line before the window ends");
    // camera moving from here
    NoteInvalidate(0x006C7451, false);
    NoteSolve(0x1000, 3, 1, 77, 2);
    NoteSolve(0x3000, 5, 2, 78, 7);
    Check(Frame(true, 61000, &line), "a line once the window ends");
    std::printf("  %s\n", line.c_str());
    Check(Has(line, "camera still 1 frames, moving 2"), "frames split by camera state");
    Check(Has(line, "finished solves still 4 (class 0/1/2 1/1/2), moving 2 (0/0/1 +1 other)"), "solves by camera state and class");
    Check(Has(line, "invalidates still 3, moving 1"), "invalidates by camera state");
    Check(Has(line, "invalidated by 0x006c7451 still 2 moving 1; 0x0069f160 (flag) still 1 moving 0"), "callers sorted by count, flag changes apart");
    Check(Has(line, "3 rooms solved; most lot 77 story 1 room 3 still 3 moving 1 (class 0/1/2 1/1/2)"), "the room solved most comes first");
    Check(!Frame(false, 62000, &line), "the window starts again");
    // full tables: every caller and room past the table size is counted, not listed
    for (uintptr_t c = 1; c <= 70; c++) NoteInvalidate(c * 16, false);
    for (uintptr_t r = 1; r <= 130; r++) NoteSolve(r * 64, 1, 0, 1, 0);
    Check(Frame(false, 123000, &line), "a second line");
    Check(Has(line, "invalidates still 70, moving 0") && Has(line, "6 invalidates by callers not listed"), "callers beyond 64 counted as more");
    Check(Has(line, "128 rooms solved") && Has(line, "2 solves of rooms not listed"), "rooms beyond 128 counted as more");
    Check(!Has(line, "ApexRadiance.asi") && CallerText(0x00401000) == "0x00401000", "addresses outside the module printed as they are");
    std::printf(g_fail ? "%d checks FAILED\n" : "Room solve causes: all checks passed\n", g_fail);
    return g_fail ? 1 : 0;
}
