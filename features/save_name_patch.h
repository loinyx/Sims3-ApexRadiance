#pragma once
// The script bytes of the save name fix (save_name_fix.h), apart from the process: shared with tools/save_name_fix_test.
//
// Sims3.Gameplay.MainMenuState.Startup (Sims3GameplaySystems.dll), as IL. With "-worldFile:<save>.sims3" it takes the first
// branch; the second is dead code (a local set to false and never changed):
//   110: ldloc.0; ldloc.s 9; ldloc.s 8; ldc.i4.1; call TransitionToGameStateToInWorld(string, SaveGameMetadata, ..)
//   11B: br 1AE
//   120: ldloc.1; brfalse.s 151
//   123: newobj WorldFileMetadata; stloc.s 10; ... call TransitionToGameStateToInWorld(string, WorldFileMetadata, ..); br.s 1AE
// Patched: 11B: br 123, and 123 to 14E is
//   call Responder.get_Instance; callvirt get_OptionsModel; ldloc.0; callvirt IOptionsModel.set_SaveName; br 1AE; nop ..
// The three calls' tokens come from LiveModeState in the same assembly, which makes the same calls:
//   call get_Instance; callvirt get_OptionsModel; ldarg.0; ldfld mForcedSaveFilename; callvirt set_SaveName;
//   call get_Instance; callvirt get_OptionsModel; ldc.i4.1; callvirt SaveGame(bool); pop; ldarg.0; ldfld ..; brfalse.s
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace SaveNamePatch {

constexpr int kAny = -1;

// 110 .. 150; the tokens are free
constexpr int kStartup[] = {
    0x06, 0x11, 0x09, 0x11, 0x08, 0x17, 0x28, kAny, kAny, kAny, kAny, 0x38, 0x8E, 0x00, 0x00, 0x00, // 110
    0x07, 0x2C, 0x2E,                                                                               // 120
    0x73, kAny, kAny, kAny, kAny, 0x13, 0x0A, 0x11, 0x0A, 0x06, 0x7D, kAny, kAny, kAny, kAny,       // 123
    0x28, kAny, kAny, kAny, kAny, 0x6F, kAny, kAny, kAny, kAny, 0x12, 0x0A, 0x6F, kAny, kAny, kAny, kAny, 0x26, // 132
    0x06, 0x11, 0x0A, 0x11, 0x08, 0x17, 0x28, kAny, kAny, kAny, kAny, 0x2B, 0x5D};                  // 144
constexpr size_t kStartupSize = sizeof kStartup / sizeof kStartup[0];
constexpr size_t kAnchor = 7;     // its first bytes are fixed: what a search looks for
constexpr size_t kBranch = 0x0C;  // the displacement of the br at 11B
constexpr size_t kDead = 0x13;    // 123
constexpr size_t kDeadSize = 0x2C; // 123 .. 14E (the br.s at 14F stays)

// LiveModeState: the tokens' places
constexpr int kTokens[] = {
    0x28, kAny, kAny, kAny, 0x06, 0x6F, kAny, kAny, kAny, 0x06, 0x02, 0x7B, kAny, kAny, kAny, 0x04, 0x6F, kAny, kAny, kAny, 0x0A,
    0x28, kAny, kAny, kAny, 0x06, 0x6F, kAny, kAny, kAny, 0x06, 0x17, 0x6F, kAny, kAny, kAny, 0x0A, 0x26, 0x02, 0x7B, kAny, kAny, kAny, 0x04, 0x2C};
constexpr size_t kTokensSize = sizeof kTokens / sizeof kTokens[0];
constexpr size_t kInstance = 1, kOptions = 6, kField = 12, kSetName = 17, kInstance2 = 22, kOptions2 = 27, kField2 = 40;

inline bool Matches(const uint8_t* p, const int* pattern, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (pattern[i] != kAny && p[i] != pattern[i]) return false;
    return true;
}
inline bool IsStartup(const uint8_t* p) { return Matches(p, kStartup, kStartupSize); }
inline bool IsTokens(const uint8_t* p) {
    return Matches(p, kTokens, kTokensSize) && !std::memcmp(p + kInstance, p + kInstance2, 4) && !std::memcmp(p + kOptions, p + kOptions2, 4) && !std::memcmp(p + kField, p + kField2, 4);
}

// the place of the one match in [data, data + size), or size when there is none or more than one
template <typename Is> size_t FindOnly(const uint8_t* data, size_t size, size_t n, uint8_t first, Is is) {
    size_t found = size;
    for (size_t at = 0; at + n <= size;) {
        const auto* hit = static_cast<const uint8_t*>(std::memchr(data + at, first, size - n - at + 1));
        if (!hit) break;
        at = static_cast<size_t>(hit - data);
        if (is(hit)) {
            if (found != size) return size;
            found = at;
        }
        at++;
    }
    return found;
}

// what replaces the dead block (kDeadSize bytes), from the tokens' match
inline void Build(const uint8_t* tokens, uint8_t* out) {
    std::memset(out, 0x00, kDeadSize); // nop
    out[0] = 0x28; std::memcpy(out + 1, tokens + kInstance, 4);
    out[5] = 0x6F; std::memcpy(out + 6, tokens + kOptions, 4);
    out[10] = 0x06;
    out[11] = 0x6F; std::memcpy(out + 12, tokens + kSetName, 4);
    const int32_t toEnd = 0x1AE - (0x123 + 21);
    out[16] = 0x38; std::memcpy(out + 17, &toEnd, 4);
}
constexpr int32_t kNewBranch = 0x123 - 0x120;

} // namespace SaveNamePatch
