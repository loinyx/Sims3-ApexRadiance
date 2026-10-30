// Split-Level / Every-Story Ground Light (part of Night Lighting)
//
// GetLotID (0x006BC020, thiscall(light) -> 64-bit lot id in EDX:EAX) has two callers: the outdoor-room light gather
// (0x006B635D) and the terrain light bake (0x00C294D0, right before the bake's story test at 0x00C294D9). With the lot id
// reported as 0 to the bake, lot lamps on any story light the ground outside the lot like world lamps, so the terrain has
// no straight light cut at lot edges or between stories (v0.1.0 was played with official Sims3SettingsSetter's
// "Split-Level Lighting Fix", which zeroes GetLotID for both callers).
//
// Patches:
// - the first 5 bytes of GetLotID become "xor eax,eax; xor edx,edx; ret" (skipped when official S3SS's fix already did it);
// - the outdoor-room gather's call at 0x006B635D goes to LevelLightShare::GatherLotId (the real lot id), also when S3SS
//   zeroes GetLotID: with lot id 0 there, every lot street lamp was listed a third time in its lot's room maps, and the lot
//   ground was ~1.5x brighter than the terrain next to it (a straight step at the lot edge; research\borda2). It answers 0
//   only for the lamp posts of other lots, which then light this lot's outside walls as world lamps do.
// Original bytes are checked before writing and put back on uninstall, only where Apex wrote them.
//
// Part of Apex Radiance. Credits: @loinyx

#include "patch_base.h"
#include "apex_version.h"
#include "night_lighting.h"
#include "s3ss_detect.h"
#include "game_addresses.h"
#include "level_light_share.h"
#include "ui/i18n.h"
#include <atomic>
#include <cstring>
#include <format>

namespace {

std::atomic<bool> g_providedByS3SS{false}; // for the menu (NightLighting::SplitLevelProvidedByS3SS)

// Addresses: the fixed Steam 1.67.2 ones, or found by signature on other builds (game_addresses.h); set by Install.
uintptr_t kGetLotId = 0; // 0x006BC020 on Steam
// mov eax,[ecx+0C0h]; mov edx,[ecx+0C4h]; ret  (TS3W.exe 1.67.2 Steam)
const std::vector<BYTE> kVanilla = {0x8B, 0x81, 0xC0, 0x00, 0x00, 0x00, 0x8B, 0x91, 0xC4, 0x00, 0x00, 0x00, 0xC3};
// xor eax,eax; xor edx,edx; ret
const std::vector<BYTE> kReturnZero = {0x33, 0xC0, 0x33, 0xD2, 0xC3};

// The outdoor-room light gather's call to GetLotID ("mov ecx,esi; call GetLotID; or eax,edx; jnz"). That gather keeps
// world lights only (lot id 0); with the zeroed GetLotID every lot street lamp passed it too and was listed a third time
// in its lot's room light maps (the game already lists lot lamps twice), making the lot map ~1.5x the terrain stamp: a
// straight light step at the lot edge (research\borda2). The call is sent to LevelLightShare::GatherLotId (the original
// body, but lot id 0 for another lot's lamp post), so only the terrain bake (0x00C294D0) sees lot id 0 for every lamp.
uintptr_t kGatherCall = 0; // 0x006B635D on Steam
// From kGatherCall - 2: mov ecx,esi; call GetLotID; or eax,edx; jnz +23h (Steam: 8B CE E8 BE 5C 00 00 0B C2 75 23)
std::vector<BYTE> GatherContext() {
    const int32_t rel = MemPatch::CalculateRelativeOffset(kGatherCall, kGetLotId);
    std::vector<BYTE> b = {0x8B, 0xCE, 0xE8, 0, 0, 0, 0, 0x0B, 0xC2, 0x75, 0x23};
    std::memcpy(b.data() + 3, &rel, sizeof rel);
    BYTE jnz = 0; // other builds: the jnz distance is not checked (the signature matched the rest)
    if (!GameAddr::IsFixed() && MemPatch::ReadBytes(kGatherCall + 8, &jnz, 1)) b[10] = jnz;
    return b;
}

} // namespace

class SplitLevelGroundLightPatch : public ApexPatch {
  public:
    SplitLevelGroundLightPatch() : ApexPatch("SplitLevelGroundLight", nullptr) {}

    bool Install() override {
        if (isEnabled) return true;
        lastError.clear();
        providedByS3SS_ = false;
        std::string missing;
        if (!GameAddr::Have({GameAddr::Id::GetLotIdGatherCall, GameAddr::Id::GetLotId}, &missing)) return Fail(GameAddr::NotAvailable(missing));
        kGetLotId = GameAddr::Get(GameAddr::Id::GetLotId);
        kGatherCall = GameAddr::Get(GameAddr::Id::GetLotIdGatherCall);
        const std::vector<BYTE> kGatherContext = GatherContext();
        // The room gather keeps the real lot id, also when S3SS zeroes GetLotID (its fix has the same triple count)
        if (!MemPatch::ValidateBytes(reinterpret_cast<LPCVOID>(kGatherCall - 2), kGatherContext.data(), kGatherContext.size()))
            return Fail(std::format("The outdoor-room light gather differs at 0x{:X} (different game version or another mod)", kGatherCall));
        std::vector<BYTE> call = {0xE8, 0, 0, 0, 0};
        const int32_t rel = MemPatch::CalculateRelativeOffset(kGatherCall, reinterpret_cast<uintptr_t>(&LevelLightShare::GatherLotId));
        std::memcpy(call.data() + 1, &rel, sizeof rel);
        const std::vector<BYTE> callExpected(kGatherContext.begin() + 2, kGatherContext.begin() + 7);
        if (!MemPatch::WriteBytes(kGatherCall, call, &written_, &callExpected)) return Fail("Could not patch the outdoor-room light gather");
        if (S3SSDetect::SplitLevelFixActive()) {
            // Official S3SS's Split-Level Lighting Fix does the same thing: nothing to write.
            providedByS3SS_ = true;
            g_providedByS3SS = true;
            isEnabled = true;
            LOG_INFO(std::format("[SplitLevelGroundLight] Provided by Sims3SettingsSetter (its Split-Level Lighting Fix is on); Apex only keeps the real lot id "
                                 "in the outdoor-room gather (0x{:X})",
                                 kGatherCall));
            return true;
        }
        const std::vector<BYTE> expected(kVanilla.begin(), kVanilla.begin() + static_cast<std::ptrdiff_t>(kReturnZero.size()));
        if (!MemPatch::ValidateBytes(reinterpret_cast<LPCVOID>(kGetLotId), kVanilla.data(), kVanilla.size()) ||
            !MemPatch::WriteBytes(kGetLotId, kReturnZero, &written_, &expected)) {
            MemPatch::RestoreAll(written_); // the gather call written above
            return Fail(std::format("GetLotID differs at 0x{:X} (different game version or another mod)", kGetLotId));
        }
        isEnabled = true;
        LOG_INFO(std::format("[SplitLevelGroundLight] Installed at 0x{:X} (lot lamps light the ground of every story; the outdoor-room gather at 0x{:X} keeps the real lot id)",
                             kGetLotId, kGatherCall));
        return true;
    }

    bool Uninstall() override {
        if (!isEnabled) return true;
        lastError.clear();
        if (!written_.empty() && !MemPatch::RestoreAll(written_)) return Fail("Could not restore GetLotID or the gather call");
        providedByS3SS_ = false;
        g_providedByS3SS = false;
        isEnabled = false;
        LOG_INFO("[SplitLevelGroundLight] Uninstalled");
        return true;
    }

    void Update() override { pendingReinstall = false; }

    void RenderCustomUI() override {
        SAFE_IMGUI_BEGIN();
        if (providedByS3SS_) ImGui::TextDisabled("%s", I18n::Tr("Already handled by Sims3SettingsSetter (its Split-Level Lighting Fix is on)"));
        else ImGui::TextDisabled("%s", I18n::Tr("Lamps on every floor light the ground outside the lot, with no hard edge at the lot border"));
    }

  private:
    std::vector<MemPatch::PatchLocation> written_;
    bool providedByS3SS_ = false;
};

bool NightLighting::SplitLevelProvidedByS3SS() { return g_providedByS3SS.load(); }

APEX_REGISTER_FEATURE(SplitLevelGroundLightPatch, {.displayName = "Every-Story Ground Light",
                                            .description = "Lamps on any floor of a lot light the ground and the yard around the house, with no hard edge at "
                                                           "lot borders or between floors. Part of Night Lights in " APEX_PRODUCT_NAME ". Credits: @loinyx",
                                            .category = "Graphics",
                                            .experimental = false,
                                            .enabledByDefault = true,
                                            .supportedVersions = VERSION_STEAM,
                                            .technicalDetails = {"GetLotID (0x6BC020) returns 0 for the terrain bake (0xC294D0); the outdoor-room light gather's call (0x6B635D) keeps the real lot id, so lot lamps are not listed a third time in their lot's room maps; it reports 0 only for the lamp posts of other lots.",
                                                                 "Original bytes checked before writing; restored on uninstall only when Apex wrote them.",
                                                                 "Skipped when official S3SS's Split-Level Lighting Fix is already on."},
                                            .gameCodeGroup = "SplitLevel"});
