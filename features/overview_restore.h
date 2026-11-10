#pragma once
// What the Overview's group switches turned off, so turning them back on restores exactly that (Credits: @loinyx).
// Kept in ApexRadiance.toml ([overview_restore]) so it survives a restart:
//   performance = [...]  the Performance options that were on when the Performance switch turned them off
//   effects     = [...]  the effects that were on when All effects turned them off ("#picture" = Color, "#shore" = Water Reflections)
//   apex        = [...]  everything that was on when "Turn off Apex" ran; its presence means Apex is off
//   lot_streaming = [...] the Lot Streaming options that were on; display = [...] the Display settings (six numbers)
//   apex_screen = {...}  the Display settings at that moment ([screen] keys)
#include <string>
#include <vector>

namespace toml {
inline namespace v3 {
class table;
}
} // namespace toml

namespace OverviewRestore {

enum class Scope { Performance, Effects, Apex, LotStreaming, Display };

bool Has(Scope scope);
std::vector<std::string> Names(Scope scope);
void Remember(Scope scope, std::vector<std::string> names); // saves
void Forget(Scope scope);                                   // saves
// Everything remembered dropped but the Display switch's: a preset, Reset all or Undo set the features directly, so
// nothing is to be restored (the Display settings are not in profiles)
void ForgetAll();
// The Display settings remembered with Scope::Apex: mode, width, height, fps, background fps, show fps
void RememberScreen(const int (&values)[6]);
bool Screen(int (&values)[6]);
void SaveToToml(toml::table& root); // [overview_restore]

} // namespace OverviewRestore
