#include "lot_detail_range.h"
#include "apex_log.h"
#include "game_addresses.h"
#include "game_version.h"
#include "memory_patch.h"
#include <Windows.h>
#include <cmath>
#include <cstdint>
#include <format>
#include <mutex>
#include <string>
#include <vector>

namespace LotDetailRange {
namespace {

std::mutex g_lock;
bool g_running = false;

uintptr_t g_worldManagerGlobal = 0;
uintptr_t g_worldManager = 0;

float g_targetDistance = static_cast<float>(kDefaultDistance);

float g_originalDistance = 0.0f;
bool g_originalDistanceValid = false;
bool g_distanceWritten = false;
bool g_distanceAbandoned = false;
bool g_distanceMaintainLogged = false;

template <typename T> bool Read(uintptr_t address, T& out) {
    return address && MemPatch::ReadBytes(address, &out, sizeof(T));
}

template <typename T> std::vector<BYTE> Bytes(const T& value) {
    const BYTE* p = reinterpret_cast<const BYTE*>(&value);
    return std::vector<BYTE>(p, p + sizeof(T));
}

template <typename T> bool WriteExpected(uintptr_t address, const T& value, const T& expected) {
    const std::vector<BYTE> bytes = Bytes(value);
    const std::vector<BYTE> old = Bytes(expected);
    return MemPatch::WriteBytes(address, bytes, nullptr, &old);
}

bool PlausibleDistance(float value) {
    return std::isfinite(value) && value >= 1.0f && value <= 2000.0f;
}

void ClearWorldState() {
    g_worldManager = 0;
    g_originalDistance = 0.0f;
    g_originalDistanceValid = false;
    g_distanceWritten = false;
    g_distanceAbandoned = false;
    g_distanceMaintainLogged = false;
}

bool ApplyDistance(float current, std::string* error) {
    if (g_distanceAbandoned) return true;

    if (current == g_targetDistance) return true;

    if (!g_distanceWritten) {
        if (!WriteExpected(g_worldManager + 0xDC, g_targetDistance, current)) {
            if (error) *error = "Could not apply WorldManager+0xDC Lot LOD distance";
            return false;
        }
        g_distanceWritten = true;
        LOG_INFO(std::format("[LotDetailRange] Lot LOD distance: {:.3f} -> {:.3f}", current, g_targetDistance));
        return true;
    }

    // The game can restore its own world baseline while a world is settling. Reassert only that captured baseline.
    // Any third value is treated as another owner and Apex stops touching this field.
    if (g_originalDistanceValid && current == g_originalDistance) {
        if (!WriteExpected(g_worldManager + 0xDC, g_targetDistance, current)) {
            if (error) *error = "Could not maintain WorldManager+0xDC Lot LOD distance";
            return false;
        }
        if (!g_distanceMaintainLogged) {
            LOG_INFO(std::format("[LotDetailRange] Lot LOD distance drifted {:.3f} -> {:.3f}; maintained",
                                 current, g_targetDistance));
            g_distanceMaintainLogged = true;
        }
        return true;
    }

    g_distanceAbandoned = true;
    LOG_WARNING(std::format("[LotDetailRange] Another owner changed Lot LOD distance to {:.3f}; Apex stops maintaining +0xDC",
                            current));
    return true;
}

bool ApplyCurrentWorld(std::string* error) {
    uintptr_t world = 0;
    if (!Read(g_worldManagerGlobal, world) || !world) return true;

    const bool newWorld = world != g_worldManager;
    if (newWorld) {
        // Never restore/write through a stale WorldManager pointer. The new world gets a fresh captured baseline.
        ClearWorldState();
        g_worldManager = world;
    }

    float distance = 0.0f;
    if (!Read(world + 0xDC, distance) || !PlausibleDistance(distance)) {
        if (error) *error = "WorldManager+0xDC Lot LOD distance is unreadable or implausible";
        if (newWorld) ClearWorldState();
        return false;
    }

    if (newWorld) {
        g_originalDistance = distance;
        g_originalDistanceValid = true;
        LOG_INFO(std::format("[LotDetailRange] WorldManager {:#010x}: native Lot LOD distance {:.3f}; target {:.0f}",
                             world, distance, g_targetDistance));
    }

    return ApplyDistance(distance, error);
}

void RestoreCurrentWorldIfOwned() {
    if (!g_worldManager) {
        ClearWorldState();
        return;
    }

    uintptr_t liveWorld = 0;
    if (!Read(g_worldManagerGlobal, liveWorld) || liveWorld != g_worldManager) {
        if (g_distanceWritten)
            LOG_INFO("[LotDetailRange] Restore skipped: WorldManager changed");
        ClearWorldState();
        return;
    }

    if (g_distanceWritten && g_originalDistanceValid && !g_distanceAbandoned) {
        float current = 0.0f;
        if (!Read(g_worldManager + 0xDC, current)) {
            LOG_WARNING("[LotDetailRange] Lot LOD distance restore skipped: +0xDC unreadable");
        } else if (current != g_targetDistance) {
            LOG_INFO(std::format("[LotDetailRange] Lot LOD distance restore skipped: another owner changed {:.3f} to {:.3f}",
                                 g_targetDistance, current));
        } else if (WriteExpected(g_worldManager + 0xDC, g_originalDistance, current)) {
            LOG_INFO(std::format("[LotDetailRange] Lot LOD distance restored {:.3f} -> {:.3f}",
                                 current, g_originalDistance));
        } else {
            LOG_WARNING("[LotDetailRange] Lot LOD distance restore failed");
        }
    }

    ClearWorldState();
}

} // namespace

bool Start(int distance, std::string* error) {
    std::lock_guard<std::mutex> guard(g_lock);
    if (g_running) return true;

    if (distance < kMinDistance || distance > kMaxDistance) {
        if (error) *error = std::format("Lot detail distance {} is outside the validated range {}..{}",
                                        distance, kMinDistance, kMaxDistance);
        return false;
    }

    g_worldManagerGlobal = GameAddr::Get(GameAddr::Id::WorldManagerPtr);
    if (!g_worldManagerGlobal) {
        if (error) *error = "WorldManager singleton address is unavailable";
        return false;
    }

    g_targetDistance = static_cast<float>(distance);
    ClearWorldState();

    LOG_INFO(std::format("[LotDetailRange] Starting on {}: target distance {}", GetGameVersionName(), distance));

    std::string worldError;
    if (!ApplyCurrentWorld(&worldError)) {
        RestoreCurrentWorldIfOwned();
        if (error) *error = worldError;
        return false;
    }

    g_running = true;
    LOG_INFO("[LotDetailRange] Active");
    return true;
}

void Stop() {
    std::lock_guard<std::mutex> guard(g_lock);
    if (!g_running) return;

    RestoreCurrentWorldIfOwned();
    g_running = false;
    g_worldManagerGlobal = 0;
    LOG_INFO("[LotDetailRange] Stopped");
}

void Tick() {
    std::lock_guard<std::mutex> guard(g_lock);
    if (!g_running) return;

    std::string error;
    if (!ApplyCurrentWorld(&error) && !error.empty())
        LOG_WARNING("[LotDetailRange] " + error);
}

bool Running() {
    std::lock_guard<std::mutex> guard(g_lock);
    return g_running;
}

std::string StatusText() {
    std::lock_guard<std::mutex> guard(g_lock);
    if (!g_running) return "Off";
    if (!g_worldManager)
        return std::format("On; waiting for a world (distance {})", static_cast<int>(g_targetDistance));
    return std::format("On; lot detail distance {}", static_cast<int>(g_targetDistance));
}

} // namespace LotDetailRange
