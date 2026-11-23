// Offline checks of features/looks_rest.cpp (REST configurations in Looks). Console output; exit 1 on a failure.
// Build and run (x86 or x64 Native Tools prompt, from the repository root):
//   cl /nologo /std:c++20 /EHsc /utf-8 /I. /Ifeatures tools\looks_rest_test\check.cpp features\looks_rest.cpp /Fe:%TEMP%\looks_rest_test.exe
//   %TEMP%\looks_rest_test.exe [ReshadeEffectShaderToggler.ini]
// With a file, its groups are listed too (read with the same parser).
#include "looks_rest.h"
#include <cstdio>
#include <fstream>
#include <sstream>

static int g_fail = 0;
static void Check(bool ok, const char* what)
{
    std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++g_fail;
}

int main(int argc, char** argv)
{
    using namespace Looks::Rest;
    Check(Crc32("123456789", 9) == 0xCBF43926u, "CRC32 check value of \"123456789\" (zlib's)");
    Check(Crc32("", 0) == 0, "CRC32 of nothing");

    const char* ini =
        "\n[General]\nResourceShim=none\nAmountGroups=3\n\n"
        "[Group0_VertexShaders]\nShaderHash0=892244598\nShaderHash1=3042119652\nAmountHashes=2\n\n"
        "[Group0_PixelShaders]\nShaderHash0=921676528\nAmountHashes=1\n\n"
        "[Group0]\r\nName=TS3 Pixel Shaders\r\nToggleKey=0\r\nActive=True\r\nInvocationLocation=2\r\n"
        "MatchSwapchainResolutionOnly=True\r\nTechniques=FastSharpen,FilmicSharpen,FakeHDR\r\nAllowAllTechniques=True\r\n"
        "TechniqueExceptions=True\r\n\r\n"
        "[Group1]\nName=Only Comic\nActive=1\nInvocationLocation=1\nMatchSwapchainResolutionOnly=2\nTechniques=Comic\nAllowAllTechniques=False\n"
        "[Group1_PixelShaders]\nShaderHash0=4294967295\nAmountHashes=1\n"
        "[Group2]\nName=Off\nActive=False\nTechniques=Comic\nToggleKey=118\n";
    Config c;
    Check(Parse(ini, c), "a REST .ini parses");
    Check(c.groups.size() == 3, "three groups");
    if (c.groups.size() == 3)
    {
        const Group& g0 = c.groups[0];
        Check(g0.name == "TS3 Pixel Shaders" && g0.active, "group 0: name and Active=True");
        Check(g0.location == kOnRenderTargetChange, "group 0: InvocationLocation=2 is on render target change");
        Check(g0.match == kResolution, "group 0: MatchSwapchainResolutionOnly=True reads 0 (REST's atoll): the resolution");
        Check(g0.vs.count(892244598u) && g0.vs.count(3042119652u) && g0.vs.size() == 2, "group 0: vertex shader hashes");
        Check(g0.ps.count(921676528u) && g0.ps.size() == 1, "group 0: pixel shader hash");
        Check(Takes(g0, "Comic", "Comic.fx") && Takes(g0, "MXAO", "qUINT_mxao.fx"), "group 0: every technique but the exceptions");
        Check(!Takes(g0, "FastSharpen", "FastSharpen.fx") && !Takes(g0, "FakeHDR", "Bloom.fx"), "group 0: the exceptions stay out");
        const Group& g1 = c.groups[1];
        Check(g1.active && g1.location == kAfterDraw && g1.match == kExtendedAspectRatio, "group 1: Active=1, after the draw, extended aspect");
        Check(g1.ps.empty(), "group 1: a hash at UINT32_MAX is REST's \"missing\"");
        Check(Takes(g1, "Comic", "Comic.fx") && !Takes(g1, "MXAO", "qUINT_mxao.fx"), "group 1: only its listed technique");
        Check(!Takes(c.groups[2], "Comic", "Comic.fx"), "group 2: an inactive group takes nothing");
        Check(!c.notes.empty(), "a toggle key is noted as not followed");
    }

    Config named;
    Parse("[General]\nAmountGroups=1\n[Group0]\nActive=1\nTechniques=Comic [Comic.fx],MXAO [qUINT_mxao.fx]\nAllowAllTechniques=0\n", named);
    Check(named.groups.size() == 1 && named.groups[0].name == "Default", "a group with no name is \"Default\"");
    if (named.groups.size() == 1)
    {
        Check(Takes(named.groups[0], "Comic", "comic.fx") && Takes(named.groups[0], "MXAO", "qUINT_mxao.fx"), "\"Name [File.fx]\" (REST's current form; file case aside)");
        Check(!Takes(named.groups[0], "Comic", "Other.fx"), "\"Name [File.fx]\": another file's technique of that name is not it");
    }

    Group r; r.match = kResolution;
    Check(SizeMatches(r, 1920, 1080, 1920, 1080) && !SizeMatches(r, 960, 540, 1920, 1080), "resolution: exact size only");
    Group a; a.match = kAspectRatio;
    Check(SizeMatches(a, 1280, 720, 1920, 1080) && !SizeMatches(a, 960, 540, 1920, 1080), "aspect: a similar size (1.5x), not half");
    Check(!SizeMatches(a, 1024, 1024, 1920, 1080), "aspect: a square is not 16:9");
    Group x; x.match = kExtendedAspectRatio;
    Check(SizeMatches(x, 960, 540, 1920, 1080) && SizeMatches(x, 480, 270, 1920, 1080), "extended aspect: exact divisions too");
    Group n; n.match = kAnySize;
    Check(SizeMatches(n, 17, 3, 1920, 1080), "any size");

    Config empty;
    Check(!Parse("[General]\nAmountGroups=0\n", empty) && empty.groups.empty(), "no group: not a configuration");

    if (argc > 1)
    {
        std::ifstream f(argv[1], std::ios::binary);
        std::stringstream s; s << f.rdbuf();
        Config real;
        Check(Parse(s.str(), real), "the given file parses");
        for (const Group& g : real.groups)
            std::printf("  group \"%s\": %s, location %u, size match %u, %zu VS + %zu PS hashes, %s%zu techniques listed\n", g.name.c_str(),
                        g.active ? "active" : "inactive", g.location, g.match, g.vs.size(), g.ps.size(),
                        g.allowAll ? (g.exceptions ? "all but " : "all (list ignored), ") : "only ", g.techniques.size());
        for (const std::string& note : real.notes) std::printf("  note: %s\n", note.c_str());
    }
    std::printf(g_fail ? "PROBLEMS\n" : "OK\n");
    return g_fail ? 1 : 0;
}
