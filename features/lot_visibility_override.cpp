#include "lot_visibility_override.h"
#include "apex_log.h"
#include "game_addresses.h"
#include "game_version.h"
#include "memory_patch.h"
#include "s3ss_detect.h"
#include <cstdint>
#include <cstring>
#include <format>
#include <mutex>

namespace {

constexpr size_t kBiasOffset = 8; // the subtraction starts 8 bytes after the JZ (74 xx F3 0F 10 44 24 08)
constexpr size_t kInsnLen = 8;
constexpr uint8_t kNativeBias[kInsnLen] = {0xF3, 0x0F, 0x5C, 0x87, 0xE0, 0x00, 0x00, 0x00}; // subss xmm0,[edi+0E0h]

// Read by the game's code while the patch is in place: static storage, never freed
alignas(4) const float g_keepFactor = LotVisibilityOverride::kKeepFactor;

std::mutex g_lock;
bool g_running = false;
bool g_externalS3SS = false;
bool g_externalPatched = false;
bool g_owned = false;
uintptr_t g_jz = 0;
uint8_t g_patched[kInsnLen] = {};

bool S3SSOwnsVisibilityOverride() {
    const S3SSDetect::Info info = S3SSDetect::Scan();
    if (!info.s3ssLoaded) return false;
    return S3SSDetect::S3SSPatchBoolSettingEnabled("LotStreamingOptimizations", "visibilityOverride", true);
}

// mulss xmm0,[g_keepFactor]
void MakeKeepInsn(uint8_t out[kInsnLen]) {
    out[0] = 0xF3; out[1] = 0x0F; out[2] = 0x59; out[3] = 0x05;
    const uint32_t addr = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&g_keepFactor));
    std::memcpy(out + 4, &addr, 4);
}

void ResetState() {
    g_jz = 0;
    g_owned = false;
    g_externalPatched = false;
}

} // namespace

namespace LotVisibilityOverride {

bool Start(std::string* error) {
    std::lock_guard<std::mutex> guard(g_lock);
    if (g_running) return true;

    LOG_INFO(std::format("[LotVisibility] Starting the detailed-lot hysteresis on {}", GetGameVersionName()));

    if (S3SSOwnsVisibilityOverride()) {
        g_externalS3SS = true;
        g_running = true;
        LOG_INFO("[LotVisibility] Official Sims3SettingsSetter owns LotStreamingOptimizations.visibilityOverride; Apex makes no writes");
        return true;
    }

    std::string missing;
    if (!GameAddr::GroupAvailable("LotVisibilityOverride", &missing)) {
        if (error) *error = GameAddr::NotAvailable(missing);
        return false;
    }

    ResetState();
    g_jz = GameAddr::Get(GameAddr::Id::LotVisibilityCameraBiasJZ);
    uint8_t jz = 0;
    uint8_t insn[kInsnLen] = {};
    if (!g_jz || !MemPatch::ReadBytes(g_jz, &jz, 1) || !MemPatch::ReadBytes(g_jz + kBiasOffset, insn, kInsnLen)) {
        if (error) *error = "Could not read the lot metric's detailed-lot bias";
        ResetState();
        return false;
    }

    if (jz == 0xEB) {
        g_externalPatched = true;
        LOG_INFO("[LotVisibility] The detailed-lot branch is already a JMP (another mod's visibility override); Apex leaves it as it is");
    } else if (jz == 0x74 && std::memcmp(insn, kNativeBias, kInsnLen) == 0) {
        MakeKeepInsn(g_patched);
        if (!MemPatch::WriteCodeSuspended(g_jz + kBiasOffset, g_patched, kInsnLen)) {
            if (error) *error = "Could not write the detailed-lot hysteresis";
            ResetState();
            return false;
        }
        g_owned = true;
        LOG_INFO(std::format("[LotVisibility] Detailed lots measured at {:.2f} of their squared distance (bias at {:#010x}; the native 8.0 subtraction is replaced)",
                             kKeepFactor, g_jz + kBiasOffset));
    } else {
        if (error) *error = std::format("Unexpected code at the lot metric's detailed-lot bias ({:#010x}: {:02X})", g_jz, jz);
        ResetState();
        return false;
    }

    g_externalS3SS = false;
    g_running = true;
    return true;
}

void Stop() {
    std::lock_guard<std::mutex> guard(g_lock);
    if (!g_running) return;

    if (g_externalS3SS) {
        LOG_INFO("[LotVisibility] Off in Apex; Sims3SettingsSetter remains the owner");
    } else if (g_owned && g_jz) {
        uint8_t cur[kInsnLen] = {};
        if (!MemPatch::ReadBytes(g_jz + kBiasOffset, cur, kInsnLen)) {
            LOG_WARNING("[LotVisibility] Restore skipped: the bias instruction is unreadable");
        } else if (std::memcmp(cur, g_patched, kInsnLen) != 0) {
            LOG_INFO("[LotVisibility] Restore skipped: another module changed the bias instruction after Apex");
        } else if (MemPatch::WriteCodeSuspended(g_jz + kBiasOffset, kNativeBias, kInsnLen)) {
            LOG_INFO("[LotVisibility] The native detailed-lot bias is back");
        } else {
            // the game keeps reading Apex's factor, which stays valid (static storage)
            LOG_WARNING("[LotVisibility] Restore failed; the hysteresis stays");
            return;
        }
    }

    LOG_INFO("[LotVisibility] Stopped");
    g_running = false;
    g_externalS3SS = false;
    ResetState();
}

void Tick() {}

bool Running() {
    std::lock_guard<std::mutex> guard(g_lock);
    return g_running;
}

bool HandledByS3SS() {
    std::lock_guard<std::mutex> guard(g_lock);
    return g_running && g_externalS3SS;
}

bool AlreadyPatchedExternally() {
    std::lock_guard<std::mutex> guard(g_lock);
    return g_running && g_externalPatched;
}

std::string StatusText() {
    std::lock_guard<std::mutex> guard(g_lock);
    if (!g_running) return "Off";
    if (g_externalS3SS) return "Handled by Sims3SettingsSetter";
    if (g_externalPatched) return "On; another mod's visibility override is in place";
    if (g_owned) return "On; detailed lots keep their detail about 11% farther";
    return "On";
}

} // namespace LotVisibilityOverride
