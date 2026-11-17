// Offline check of the shader constant range histogram (framework/const_range_histogram.h, real-time GI S0-14).
// Read-only: prints to stdout, writes no file. Feeds synthetic calls shaped like the game's (Shader_SetFloatConstants,
// InstancingEmulation), Apex's and another module's, and checks the runs, the highest register, the reserved-range
// writes and the site table.
#include "const_range_histogram.h"
#include <cstdio>
#include <string>

namespace {
int g_fail = 0;
void Expect(bool ok, const std::string& what) {
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what.c_str());
    if (!ok) g_fail++;
}
bool Has(const std::string& s, const std::string& part) { return s.find(part) != std::string::npos; }
// Fake sites: 0x79E10A / 0x79E0F6 = game, 0x10001000 = Apex, 0x20002000 = another module
int OwnerOf(uintptr_t ret) { return ret < 0x1000000 ? ConstRange::kGame : ret < 0x20000000 ? ConstRange::kApex : ConstRange::kOther; }
std::string Describe(uintptr_t ret) {
    char b[32];
    std::snprintf(b, sizeof b, "site 0x%X", static_cast<unsigned>(ret));
    return b;
}
} // namespace

int main() {
    using namespace ConstRange;
    static Histogram h; // large: not on the stack
    Expect(h.Report(Describe).empty(), "no call: empty report");
    // the game: PS c0-c24 in two runs, VS c0-c227 (a palette up to c227), instancing c40-c112
    for (int i = 0; i < 3; i++) {
        h.Note(kPs, 0x79E10A, 0, 8, OwnerOf);
        h.Note(kPs, 0x79E167, 10, 15, OwnerOf);
        h.Note(kVs, 0x79E0F6, 2, 226, OwnerOf);
        h.Note(kVs, 0x696A7D, 40, 73, OwnerOf);
    }
    // Apex: its own registers, inside a reserved range too (not counted as a foreign write)
    h.Note(kPs, 0x10001000, 200, 5, OwnerOf);
    h.Note(kPs, 0x10001010, 208, 16, OwnerOf);
    h.Note(kVs, 0x10001020, 252, 1, OwnerOf);
    h.EndFrame();
    Expect(h.HighestEnd(kPs, kGame) == 25, "game PS highest c24");
    Expect(h.HighestEnd(kVs, kGame) == 228, "game VS highest c227");
    Expect(h.HighestEnd(kPs, kApex) == 224, "Apex PS highest c223");
    Expect(h.ReservedWrites(kPs) == 0 && h.ReservedWrites(kVs) == 0, "no foreign reserved write yet");
    Expect(h.TakeNewReserved(Describe).empty(), "nothing to log yet");
    const std::string r1 = h.Report(Describe);
    Expect(Has(r1, "PS game  registers c0-c7, c10-c24"), "PS game runs c0-c7, c10-c24");
    Expect(Has(r1, "VS game  registers c2-c227"), "VS game run c2-c227");
    Expect(Has(r1, "site 0x79E10A") && Has(r1, "7 calling sites"), "site table lists the game site");
    Expect(h.SiteCount() == 7, "7 calling sites");
    // another module writes PS c210-c211 and VS c240; a bad call past the last register
    h.Note(kPs, 0x20002000, 210, 2, OwnerOf);
    h.Note(kVs, 0x20002010, 240, 1, OwnerOf);
    h.Note(kVs, 0x79E0F6, 250, 10, OwnerOf);
    h.EndFrame();
    Expect(h.ReservedWrites(kPs) == 1, "PS foreign reserved write counted");
    Expect(h.ReservedWrites(kVs) == 2, "VS foreign reserved writes counted (other module and the game's c250-c259)");
    Expect(h.OutOfRange(kVs) == 1, "VS call past c255 counted");
    const auto lines = h.TakeNewReserved(Describe);
    Expect(lines.size() == 3, "three sites to log");
    bool sawPs = false;
    for (const std::string& l : lines) sawPs |= Has(l, "PS c210-c211 by site 0x20002000 (other), 1 of its 1 calls");
    Expect(sawPs, "PS reserved line names the range and the site");
    Expect(h.TakeNewReserved(Describe).empty(), "each site logged once");
    const std::string sum = h.Summary();
    Expect(Has(sum, "PS: highest written game c24 Apex c223 other c211"), "summary: PS highest by owner");
    Expect(Has(sum, "reserved c228-c251 written by others 2 times") && Has(sum, "1 calls past c255"), "summary: VS reserved and out of range");
    const std::string r2 = h.Report(Describe);
    Expect(Has(r2, "highest a frame: last c255, most c255"), "VS game frame maxima");
    Expect(Has(r2, "highest a frame: last none, most c24"), "PS game: none last frame, c24 at most");
    std::printf("%s\n%s", g_fail ? "const_range_check: FAILED" : "const_range_check: all passed", r2.c_str());
    return g_fail ? 1 : 0;
}
