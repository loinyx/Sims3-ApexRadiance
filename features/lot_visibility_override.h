#pragma once
// Keep lot visibility stable: a real hysteresis for lots already in Detailed View.
//
// The game's lot metric 0x00C62D80 (Steam 1.67.2) is a squared distance from the camera's target point to the lot, with
// no view angle. For a lot already detailed it then subtracts WorldManager+0xE0 ("Active Lot Bias", 8.0), meant to keep
// that lot detailed a little longer; but 8 square metres is under 3 m of distance, so the native hysteresis does almost
// nothing and lots at the edge of the detail distance or of the capacity switch back and forth. The 8 bytes of that
// subtraction (`subss xmm0,[edi+0E0h]`, 8 bytes after the JZ that skips it for lots not detailed) become
// `mulss xmm0,[kKeepFactor]`: a detailed lot is measured at kKeepFactor of its squared distance (0.81 = 90 % of its
// distance), so it keeps its detail until it is about 11 % farther than where a new lot is promoted, and it wins capacity
// ties against a lot that is only slightly nearer. Lots not detailed are measured exactly as before.
//
// Sims3SettingsSetter's visibilityOverride turns the JZ into a JMP (0xEB), which skips the subtraction for every lot
// (removing the small native hysteresis). When it owns that setting, or the JZ already reads 0xEB, Apex writes nothing.
// Apex restores the subtraction only when it still reads Apex's instruction.

#include <string>

namespace LotVisibilityOverride {

// Squared-distance factor for lots already detailed (0.81 = keep detail 1/0.9 times as far)
inline constexpr float kKeepFactor = 0.81f;

bool Start(std::string* error = nullptr);
void Stop();
void Tick();

bool Running();
bool HandledByS3SS();
bool AlreadyPatchedExternally();
std::string StatusText();

} // namespace LotVisibilityOverride
