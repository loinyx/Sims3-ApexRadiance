#pragma once
// Extended lot detail: the Lot LOD distance (WorldManager +0xDC) of each live WorldManager. The value is captured per
// WorldManager, guarded before writes, maintained only while Apex still owns it and restored on a clean disable/unload.
// The game's own Max Active Lots option (+0xE4) is left alone.
//
// EA 1.69 validation baseline: distance 300.

#include <string>

namespace LotDetailRange {

inline constexpr int kDefaultDistance = 300;
inline constexpr int kMinDistance = 70;
inline constexpr int kMaxDistance = 300;

bool Start(int distance, std::string* error = nullptr);
void Stop();
void Tick();
bool Running();
std::string StatusText();

} // namespace LotDetailRange
