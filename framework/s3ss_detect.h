#pragma once
#include <optional>
// Official Sims3SettingsSetter (S3SS), the old combined build (S3SS with Apex inside, named S3SSApex.asi) and the
// previous standalone build (also S3SSApex.asi), as seen from Apex Radiance. None exports anything, so they are
// recognised by strings in their read-only data:
//  - S3SS: its log header and its ImGui window id;
//  - the old combined build: the same two plus the old "Apex Edition" product name;
//  - the previous standalone build: the old product name alone (or its file name S3SSApex.asi).
// The needles are kept encoded in this binary so that Apex itself (or a second copy of it) never matches them.
// Also: the per-process instance mutexes, and settings read from S3SS.toml (read-only).
#include <windows.h>
#include <cstdint>
#include <string>
#include <array>

namespace S3SSDetect {

struct Info {
    bool scanned = false;
    bool s3ssLoaded = false;        // official S3SS module present
    bool oldCombinedBuild = false;  // the pre-split S3SS + Apex build is loaded: Apex must not run its features
    bool oldStandalone = false;     // the previous standalone S3SSApex.asi is loaded too (idle when this build loaded first)
    std::wstring s3ssModule;        // file name of the S3SS module
    std::wstring combinedModule;    // file name of the old combined module
    std::wstring oldStandaloneModule; // file name of the previous standalone module
    uintptr_t s3ssBase = 0;         // image range of the S3SS module (to tell whether a jump lands in it)
    size_t s3ssSize = 0;
};

// Enumerates the loaded modules (except Apex's own) and caches the result. Rescan() looks again (ASIs load one by one
// at startup; the D3D bootstrap rescans at device creation and at the first Present).
Info Scan();
Info Rescan(); // copies: another thread may rescan meanwhile
bool IsInS3SS(uintptr_t address);

enum class Instance {
    Unset,
    Owner,              // this copy runs; it holds Local\ApexRadiance.<pid> and Local\S3SSApex.<pid>
    DuplicateSelf,      // another ApexRadiance.asi already holds Local\ApexRadiance.<pid>: this copy stays idle
    OldStandaloneFirst, // an old S3SSApex.asi loaded first and holds Local\S3SSApex.<pid>: this copy stays idle
};
// Called once from DllMain (later calls return the first result). The owner also takes the previous standalone build's
// mutex name, so an old S3SSApex.asi that loads after it finds it taken and stays idle by its own duplicate check.
Instance AcquireInstanceMutex();

// S3SS.toml (read-only): [patches.<name>].enabled and overlay configuration.
bool S3SSPatchEnabled(const char* patchName);
// True when [patches.<patchName>] is enabled and its boolean <settingName> is true. If the setting is absent,
// defaultValue is used (matching S3SS settings that default to on).
bool S3SSPatchBoolSettingEnabled(const char* patchName, const char* settingName, bool defaultValue = true);
bool S3SSOverlayDisabled();
// Read-only compatibility data; no S3SS configuration is written.
enum class RoomAmbientCorrectionStatus { S3SSNotLoaded, ConfigUnavailable, NoOverride, BackupFailed, ConfigChanged, WriteFailed, Saved };
struct RoomAmbientCorrection {
    RoomAmbientCorrectionStatus status = RoomAmbientCorrectionStatus::S3SSNotLoaded;
    bool found = false;
    bool saved = false;
    std::array<float, 3> rgb{};
};
// Read-only: official S3SS is loaded and S3SS.toml saves a supported room-ambient override (the exact entry
// supported room-ambient setting). Nothing is written. The file is read again at most every 3 s (the menu asks
// every frame), or now with fresh = true.
std::optional<std::array<float, 3>> SavedRoomAmbientOverride(bool fresh = false);
// S3SS's "Split-Level Lighting Fix" is in place: enabled in S3SS.toml, or GetLotID (0x6BC020 on Steam, found by signature
// elsewhere: game_addresses.h) no longer holds its original bytes. Apex's own equivalent (patches/split_level_ground_light_patch.cpp) then stays out of the way.
// Call it before Apex writes its own patch there (the byte test cannot tell the two apart).
bool SplitLevelFixActive();

std::string Summary(); // one line for the log and the About section

} // namespace S3SSDetect
