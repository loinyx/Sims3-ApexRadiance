#pragma once
// Keep lot visibility stable: a real hysteresis for lots already in Detailed View.
//
// The game's lot metric 0x00C62D80 (Steam 1.67.2) is the largest axis distance from the camera's target point to the
// lot's rectangle, squared, with no view angle. For a lot already detailed it then subtracts WorldManager+0xE0 ("Active
// Lot Bias", 8.0) from the value it returns; 8 square metres is under 3 m of distance, so lots fighting for the Max
// Active Lots slots swap back and forth as the camera moves. The 8 bytes of that subtraction (`subss xmm0,[edi+0E0h]`,
// 8 bytes after the JZ that skips it for lots not detailed) become `mulss xmm0,[kKeepFactor]`: a detailed lot ranks at
// kKeepFactor of its squared distance, so it keeps its slot against a lot up to 1/sqrt(kKeepFactor) times nearer (0.5:
// about 1.41 times). Lots not detailed are measured exactly as before.
// Only the ranking (0x00C64050) reads that value. LotLodScoring's distance test (`comiss xmm0,[edi+4]` at 0x00C6C706)
// reads the metric's raw out value and stays native: on the ranked value, a detailed lot past the distance would keep a
// slot that a waiting lot inside it needs, leaving that nearer lot an impostor without its lighting.
//
// Sims3SettingsSetter's visibilityOverride turns the JZ into a JMP (0xEB), which skips the subtraction for every lot
// (removing the small native hysteresis). When it owns that setting, or the JZ already reads 0xEB, Apex writes nothing.
// Apex restores the subtraction only when it still reads Apex's instruction.

#include <string>

namespace LotVisibilityOverride {

// Ranking factor for lots already detailed (0.5 = keep the slot against a lot up to sqrt(2) times nearer)
inline constexpr float kKeepFactor = 0.5f;

bool Start(std::string* error = nullptr);
void Stop();
void Tick();

bool Running();
bool HandledByS3SS();
bool AlreadyPatchedExternally();
std::string StatusText();

} // namespace LotVisibilityOverride
