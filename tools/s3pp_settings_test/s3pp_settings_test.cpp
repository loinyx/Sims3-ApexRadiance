// Sims3Performance.ini reading and the two-line change (framework/s3pp_settings.h): reads the [Patches] values, leaves
// every other byte alone, and never touches [Tuning]'s ShortWaitSpinUs or a key outside [Patches]
#include "s3pp_settings.h"
#include <cstdio>

static int g_fail = 0;
static void Check(bool ok, const char* what) {
    std::printf("%s %s\n", ok ? "PASS" : "FAIL", what);
    if (!ok) g_fail++;
}

int main() {
    using namespace S3PPSettings;
    const std::string ini = "; Sims3Performance\r\n[General]\r\nShortWaitSpin=1\r\n[Patches]\r\nThreadStackDiet=1\r\n; ShortWaitSpin=1 (comment)\r\n"
                            "ShortWaitSpin = 1 ; spins\r\nOther=1\r\n[Tuning]\r\nShortWaitSpinUs=100\r\nShortWaitSpin=1\r\n";
    Check(ReadPatch(ini, "ShortWaitSpin") == 1, "reads ShortWaitSpin from [Patches] only");
    Check(ReadPatch(ini, "threadstackdiet") == 1, "keys are case-insensitive");
    Check(ReadPatch(ini, "Missing") == -1, "an absent key reads -1");
    const std::string out = TurnOff(ini);
    const std::string want = "; Sims3Performance\r\n[General]\r\nShortWaitSpin=1\r\n[Patches]\r\nThreadStackDiet=0\r\n; ShortWaitSpin=1 (comment)\r\n"
                             "ShortWaitSpin = 0 ; spins\r\nOther=1\r\n[Tuning]\r\nShortWaitSpinUs=100\r\nShortWaitSpin=1\r\n";
    Check(out == want, "TurnOff changes only the two values in [Patches]");
    Check(ReadPatch(out, "ShortWaitSpin") == 0 && ReadPatch(out, "ThreadStackDiet") == 0, "both read 0 afterwards");
    Check(TurnOff(out) == out, "TurnOff on an already-off file changes nothing");
    const std::string twice = "[Patches]\nShortWaitSpin=0\nShortWaitSpin=1\n";
    Check(ReadPatch(twice, "ShortWaitSpin") == 1 && TurnOff(twice) == "[Patches]\nShortWaitSpin=0\nShortWaitSpin=0\n",
          "a key written twice counts as on and both lines change");
    Check(ReadPatch("[Patches] ; note\nShortWaitSpin=1", "ShortWaitSpin") == 1, "a section header with a comment, no final newline");
    const std::string bom = "\xEF\xBB\xBF[ patches ]\n  ShortWaitSpin=1#spins\n";
    Check(ReadPatch(bom, "ShortWaitSpin") == 1, "a UTF-8 BOM, spaces and lower case in the header, an indented key, a # comment");
    Check(TurnOff(bom) == "\xEF\xBB\xBF[ patches ]\n  ShortWaitSpin=0#spins\n", "TurnOff keeps the BOM and the # comment");
    std::printf("%s\n", g_fail ? "s3pp_settings: FAILED" : "s3pp_settings: all checks passed");
    return g_fail ? 1 : 0;
}
