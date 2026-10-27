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
    Check(!Frame(false, 1000, &line), "the first frame starts the window (settled: the camera never moved)");
    // settled: two callers, one of them on a flag change, one QueueRoom caller; one room solved three times, another once
    NoteInvalidate(0x006C7451, false);
    NoteInvalidate(0x006C7451, false);
    NoteInvalidate(0x0069F160, true);
    NoteInvalidate(0x00401234, false, true);
    NoteSolve(0x1000, 3, 1, 77, 0);
    NoteSolve(0x1000, 3, 1, 77, 1);
    NoteSolve(0x1000, 3, 1, 77, 2);
    NoteSolve(0x2000, 0, 0, 77, 2);
    Check(!Frame(true, 30000, &line), "no line before the window ends");
    // moving
    NoteInvalidate(0x006C7451, false);
    NoteSolve(0x3000, 5, 2, 78, 7);
    Check(!Frame(false, 35000, &line), "stopped 5 s after the move");
    // stopped (still for less than 10 s)
    NoteInvalidate(0x006C7451, false);
    NoteSolve(0x1000, 3, 1, 77, 2);
    Check(Frame(false, 61000, &line), "a line once the window ends");
    std::printf("  %s\n", line.c_str());
    Check(Has(line, "camera frames settled 2 stopped 1 moving 1"), "frames split by camera state (61 s is 31 s after the move: settled)");
    Check(Has(line, "finished solves settled 4 (class 0/1/2 1/1/2), stopped 1 (class 0/1/2 0/0/1), moving 1 (class 0/1/2 0/0/0 +1 other)"),
          "solves by camera state and class");
    Check(Has(line, "invalidates settled 4 stopped 1 moving 1"), "invalidates by camera state");
    Check(Has(line, "invalidated by 0x006c7451 settled 2 stopped 1 moving 1"), "the caller with the most counts first");
    Check(Has(line, "0x0069f160 (flag) settled 1 stopped 0 moving 0") && Has(line, "QueueRoom from 0x00401234 settled 1 stopped 0 moving 0"),
          "flag changes and QueueRoom's callers apart");
    Check(Has(line, "3 rooms solved; most lot 77 story 1 room 3 settled 3 stopped 1 moving 0 (class 0/1/2 1/1/2)"), "the room solved most comes first");
    Check(!Frame(false, 75000, &line), "the window starts again; still 45 s after the move: settled");
    NoteSolve(0x1000, 3, 1, 77, 0);
    // full tables: every caller and room past the table size is counted, not listed
    for (uintptr_t c = 1; c <= 70; c++) NoteInvalidate(c * 16, false);
    for (uintptr_t r = 1; r <= 130; r++) NoteSolve(r * 64, 1, 0, 1, 0);
    Check(Frame(false, 123000, &line), "a second line");
    Check(Has(line, "invalidates settled 70 stopped 0 moving 0") && Has(line, "6 invalidates by callers not listed"), "callers beyond 64 counted as more");
    Check(Has(line, "128 rooms solved") && Has(line, "3 solves of rooms not listed"), "rooms beyond 128 counted as more");
    Check(!Has(line, "ApexRadiance.asi") && CallerText(0x00401000) == "0x00401000", "addresses outside the module printed as they are");
    std::printf(g_fail ? "%d checks FAILED\n" : "Room solve causes: all checks passed\n", g_fail);
    return g_fail ? 1 : 0;
}
