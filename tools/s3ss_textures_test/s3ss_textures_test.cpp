// Offline checks of S3SS.toml reading and the change Apex makes to it (framework/s3ss_textures.h, pure part only)
#include "s3ss_textures.h"
#include <cstdio>
#include <string>

using namespace S3SSTextures;

int checks = 0, failures = 0;
void Check(bool ok, const char* what) {
    checks++;
    if (!ok) failures++, std::printf("FAIL: %s\n", what);
}

int main() {
    // the user's file of 22/10 (CRLF), both on, one with a comment
    const std::string on =
        "[patches.FrameRateLimit]\r\nenabled = true\r\n\r\n[patches.UncompressedLotTexturesPatch]\r\nenabled = true\r\n\r\n"
        "[patches.UncompressedSimTexturesPatch]\r\nenabled = true # set in the menu\r\nonlyForLOD0 = true\r\n\r\n[patches.WorldCacheSizePatch]\r\nenabled = true\r\n";
    Check(Enabled(on, kLotSection), "lot on is read");
    Check(Enabled(on, kSimSection), "sim on is read, past a comment");
    Check(Enabled(on, "patches.FrameRateLimit"), "another section is read on its own");
    const std::string off = TurnOff(on);
    Check(!Enabled(off, kLotSection) && !Enabled(off, kSimSection), "both off after the change");
    Check(Enabled(off, "patches.FrameRateLimit") && Enabled(off, "patches.WorldCacheSizePatch"), "the other patches stay on");
    Check(off.find("onlyForLOD0 = true\r\n") != std::string::npos, "onlyForLOD0 stays as it was");
    Check(off.find("enabled = false # set in the menu\r\n") != std::string::npos, "the comment and the line ending stay");
    Check(TurnOff(off) == off, "a second change changes nothing");
    // already off, missing sections, a BOM, spaces in the header
    const std::string none = "\xEF\xBB\xBF[ patches.UncompressedLotTexturesPatch ]\nenabled=false\n";
    Check(!Enabled(none, kLotSection) && !Enabled(none, kSimSection), "off and missing are off");
    Check(TurnOff(none) == none, "nothing to change leaves the text as it is");
    const std::string bom = "\xEF\xBB\xBF[patches.UncompressedSimTexturesPatch]\nenabled=true\n";
    Check(Enabled(bom, kSimSection), "after a BOM, without spaces");
    Check(TurnOff(bom) == "\xEF\xBB\xBF[patches.UncompressedSimTexturesPatch]\nenabled=false\n", "the BOM and the rest stay byte for byte");
    // a key named like it in another section, and a commented line, are not touched
    const std::string other = "[patches.UncompressedLotTexturesPatch]\n# enabled = true\nenabledX = true\n[other]\nenabled = true\n";
    Check(!Enabled(other, kLotSection), "a comment and another key do not count");
    Check(TurnOff(other) == other, "and are not changed");
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
