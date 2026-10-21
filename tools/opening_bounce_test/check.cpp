// Bounce light through floor openings (features/opening_bounce.h): the share of the other story room's ambient a room
// joined to it by a stairwell takes. Read-only; prints the checks and exits 1 on a failure.
#include "features/opening_bounce.h"
#include <cstdio>

namespace {
int checks = 0, failures = 0;
void Check(bool ok, const char* text) {
    ++checks;
    if (!ok) {
        ++failures;
        std::printf("FAIL: %s\n", text);
    }
}
bool Near(float a, float b, float e = 1e-6f) { return a - b < e && b - a < e; }
} // namespace

int main() {
    using namespace OpeningBounce;
    // a capture: the landing over the stairwell, no lamp of its own, the unlit colour; the room below lit
    const float landing[3] = {0.0068f, 0.0068f, 0.0068f};
    Neighbour below;
    below.c4[0] = 0.060f, below.c4[1] = 0.048f, below.c4[2] = 0.027f; // a warm lamp's room
    below.norm = 1.5f;
    below.shared = 32; // a 2 x 4 tile stair opening
    float out[3];
    const bool lit = Add(landing, 1.0f, 120, &below, 1, out);
    const float f = 32.0f / 120.0f, k = kShare * f / 1.5f;
    std::printf("Opening bounce: landing (%.4f %.4f %.4f) -> (%.4f %.4f %.4f)\n", landing[0], landing[1], landing[2], out[0], out[1], out[2]);
    Check(lit, "the landing over a lit room takes a share of its ambient");
    Check(Near(out[0], landing[0] + k * below.c4[0]) && Near(out[1], landing[1] + k * below.c4[1]) && Near(out[2], landing[2] + k * below.c4[2]),
          "share = kShare x opening / area x the other colour at this room's normalisation");
    Check(out[0] > out[2], "it takes the other room's hue (warm)");
    // no opening, no neighbour: nothing
    float same[3];
    Check(!Add(landing, 1.0f, 120, nullptr, 0, same) && same[0] == landing[0], "no neighbour: unchanged");
    Neighbour none = below;
    none.shared = 0;
    Check(!Add(landing, 1.0f, 120, &none, 1, same), "no shared quadrant: unchanged");
    // a darker room on the other side: never darker
    const float bright[3] = {0.08f, 0.08f, 0.08f};
    Neighbour dark;
    dark.c4[0] = dark.c4[1] = dark.c4[2] = 0.001f;
    dark.norm = 1.0f;
    dark.shared = 32;
    float kept[3];
    Add(bright, 1.0f, 120, &dark, 1, kept);
    Check(kept[0] >= bright[0] && kept[1] >= bright[1] && kept[2] >= bright[2], "never darker");
    Check(kept[0] <= bright[0] + kShare * (32.0f / 120.0f) * 0.001f + 1e-7f, "a dark neighbour adds almost nothing");
    // the whole floor open over a bright room: capped at that room's colour (here as a share of 1, bounded anyway)
    Neighbour wide = below;
    wide.norm = 1.0f;
    wide.shared = 400;
    float capped[3];
    Add(landing, 1.0f, 100, &wide, 1, capped);
    Check(capped[0] <= wide.c4[0] + 1e-7f && capped[1] <= wide.c4[1] + 1e-7f && capped[2] <= wide.c4[2] + 1e-7f, "capped at the brighter colour");
    // the other way: the room below takes the lit landing's share the same way
    Neighbour above;
    above.c4[0] = above.c4[1] = above.c4[2] = 0.05f;
    above.norm = 1.0f;
    above.shared = 32;
    float down[3];
    Check(Add(landing, 1.0f, 200, &above, 1, down) && down[1] > landing[1], "the room below takes a lit room above's share too");
    // a normalisation that is not finite or not positive: ignored
    Neighbour bad = below;
    bad.norm = 0.0f;
    Check(!Add(landing, 1.0f, 120, &bad, 1, same), "a neighbour with no normalisation is ignored");
    Check(!Add(landing, 0.0f, 120, &below, 1, same), "a room with no normalisation is left alone");
    std::printf("Opening bounce: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
