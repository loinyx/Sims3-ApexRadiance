#include "tree_light_rig.h"
#include "apex_log.h"
#include "entry_chain.h"
#include "game_addresses.h"
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <format>
#include <mutex>

namespace TreeLightRig {
namespace {
using EntryChain::Layer;
using EntryChain::Site;

constexpr uint32_t kUndergroundKey = 0x08D11E8Cu; // FNV-1 of "undergroundtree"
constexpr uintptr_t kKeyOff = 0x1F0, kScriptKeyOff = 0x53C, kDirtyOff = 0x536, kLevelOff = 0x1B8;

std::mutex g_ctrl;
bool g_installed = false;
std::atomic<long> g_restored{0}, g_seen{0};
std::atomic<int> g_logged{0};

using FnSetLevel = void(__fastcall*)(uint8_t* cluster, void* edx, int level);
using FnSetCustomRig = void(__fastcall*)(uint8_t* cluster, void* edx, uint32_t key, uint32_t arg);

// POD only (SEH): the cluster's key back to the normal rig when the setter left it on the underground rig with no script
// rig; its flag (set by the setter itself) makes the next cluster update apply it
bool TryRestore(uint8_t* cluster, int& level) {
    __try {
        if (!cluster || *reinterpret_cast<const uint32_t*>(cluster + kKeyOff) != kUndergroundKey || *reinterpret_cast<const uint32_t*>(cluster + kScriptKeyOff) != 0)
            return false;
        level = *reinterpret_cast<const int*>(cluster + kLevelOff);
        *reinterpret_cast<uint32_t*>(cluster + kKeyOff) = 0;
        *reinterpret_cast<uint8_t*>(cluster + kDirtyOff) = 1;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
void Restore(uint8_t* cluster) {
    int level = 0;
    if (!TryRestore(cluster, level)) return;
    g_restored.fetch_add(1, std::memory_order_relaxed);
    if (g_logged.fetch_add(1, std::memory_order_relaxed) < 20)
        LOG_INFO(std::format("[TreeLightRig] Tree cluster {:#010x} (level {}) given the normal light rig instead of the underground one",
                             reinterpret_cast<uintptr_t>(cluster), level));
}

void __fastcall Hook_SetLevel(uint8_t* cluster, void* edx, int level) {
    reinterpret_cast<FnSetLevel>(EntryChain::Next(Site::TreeSetLevel, Layer::TreeLightRig))(cluster, edx, level);
    g_seen.fetch_add(1, std::memory_order_relaxed);
    Restore(cluster);
}
void __fastcall Hook_SetCustomRig(uint8_t* cluster, void* edx, uint32_t key, uint32_t arg) {
    reinterpret_cast<FnSetCustomRig>(EntryChain::Next(Site::TreeSetCustomRig, Layer::TreeLightRig))(cluster, edx, key, arg);
    Restore(cluster);
}
} // namespace

bool Install(std::string* error) {
    std::lock_guard<std::mutex> lock(g_ctrl);
    if (g_installed) return true;
    std::string missing;
    if (!GameAddr::GroupAvailable("TreeLightRig", &missing)) {
        if (error) *error = GameAddr::NotAvailable(missing);
        return false;
    }
    std::string err;
    if (!EntryChain::Install(Site::TreeSetLevel, Layer::TreeLightRig, reinterpret_cast<void*>(&Hook_SetLevel), &err)) {
        if (error) *error = "could not hook the tree level setter: " + err;
        return false;
    }
    if (!EntryChain::Install(Site::TreeSetCustomRig, Layer::TreeLightRig, reinterpret_cast<void*>(&Hook_SetCustomRig), &err)) {
        EntryChain::Remove(Site::TreeSetLevel, Layer::TreeLightRig);
        if (error) *error = "could not hook the tree custom rig setter: " + err;
        return false;
    }
    g_installed = true;
    LOG_INFO(std::format("[TreeLightRig] On: trees the game sets on the underground light rig (level setter {:#010x}, custom rig setter {:#010x}) keep the normal rig",
                         GameAddr::Get(GameAddr::Id::TreeSetLevel), GameAddr::Get(GameAddr::Id::TreeSetCustomRig)));
    return true;
}

void Uninstall() {
    std::lock_guard<std::mutex> lock(g_ctrl);
    if (!g_installed) return;
    EntryChain::Remove(Site::TreeSetCustomRig, Layer::TreeLightRig);
    EntryChain::Remove(Site::TreeSetLevel, Layer::TreeLightRig);
    g_installed = false;
    LOG_INFO(std::format("[TreeLightRig] Off ({} tree clusters given the normal rig)", g_restored.load()));
}

bool Installed() {
    std::lock_guard<std::mutex> lock(g_ctrl);
    return g_installed;
}

std::string StatusText() {
    return std::format("{} | tree levels set {}, clusters given the normal rig instead of the underground one {}", Installed() ? "on" : "off", g_seen.load(),
                       g_restored.load());
}

} // namespace TreeLightRig
