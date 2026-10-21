#pragma once
// Daylight probe (numbered development builds only, switch daylight_log in [dev_build]): logs the game's sky state that
// decides how dark it is outdoors (sun height, weather blend, the smooth and the stepped night level, the weather-aware
// manmade light level), so that outdoor lamps can later follow the real outdoor light instead of the clock alone.
// Read-only: nothing it reads is written and nothing on screen changes. Render thread.
#include <cstdint>
#include <string>
#include <vector>

namespace DaylightProbe {
void OnPresent(uintptr_t lightMgr); // every frame with a world; logs one line every 5 s and when the night level steps
std::string SnapshotText(uintptr_t lightMgr, const std::vector<uintptr_t>& lights); // the lighting snapshot's section
}
