#pragma once
// Color > ReShade (reshade_patch.cpp): the effect panels drawn below the ReShade card
#include <string>

namespace ReShadeUi {
// One panel per loaded effect file: its techniques with switches and, behind a disclosure, the effect's settings
void EffectPanels();
// The settings of one effect file (its uniforms), with Reset and, for files added through Apex, Remove
void EffectSettings(const std::string& effect, bool removable);
} // namespace ReShadeUi
