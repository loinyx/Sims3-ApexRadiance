#pragma once
// One-time move of a saved [patches.NightTerrainRelight] to the lighting defaults of 3.0.0 ([meta] lighting_defaults).
// A save writes every key, so a value nobody touched looks like a choice: only a key still at its old default moves, and
// every other value stays. A player who picked the old default on purpose cannot be told apart.
#include <toml++/toml.hpp>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace ApexLightingDefaults {

inline constexpr int64_t kRevision = 1;

struct Move {
    const char* key;
    double before, after;
};
// Brightness of each surface: one key at a time
inline constexpr Move kMoves[] = {{"luzNoChao", 1.0, 0.8966},
                                  {"forcaNosObjetos", 0.75, 1.0},
                                  {"forcaLuzPorPixelNosObjetos", 0.75, 1.0},
                                  {"forcaNasCercas", 0.75, 1.0},
                                  {"forcaNasParedes", 0.30, 0.56},
                                  {"forcaNosTelhados", 0.45, 0.6},
                                  {"reflexoNoLago", 1.56, 1.0}};
// The lamp colours read together (lot lamps follow the street's unless they have their own): all three or none
inline constexpr Move kStreetTint = {"luzDasLampadasNatural", 1.0, 0.2496};
inline constexpr Move kLotTint = {"corDasLampadasDoLote", 1.0, 0.0};
inline constexpr const char* kLotTintOwn = "corPropriaNoLote"; // false -> true
inline constexpr Move kOldGround = {"brilhoNoChao", 0.75, 0.0}, kOldLotGround = {"forcaDasLampadasDoLote", 0.8, 0.0}; // (the ground before 2.18: read by MigrateGround, moved as luzNoChao)

inline bool AtOldDefault(const toml::table& t, const Move& m) {
    const auto v = t[m.key].value<double>();
    return v && std::fabs(*v - m.before) < 1e-4; // saved floats carry float rounding (0.44999998807907104)
}
inline bool AtOldOrNone(const toml::table& t, const Move& m) { return !t.contains(m.key) || AtOldDefault(t, m); }

// Moves what is still at its old default in root's Night Lights table; returns the keys it moved.
inline std::vector<std::string> Apply(toml::table& root) {
    std::vector<std::string> moved;
    toml::table* t = root["patches"]["NightTerrainRelight"].as_table();
    if (!t) return moved;
    const auto set = [&](const Move& m) {
        t->insert_or_assign(m.key, m.after);
        moved.emplace_back(m.key);
    };
    for (const Move& m : kMoves)
        if (AtOldDefault(*t, m)) set(m);
    // A config older than 2.18 holds the ground as two keys (MigrateGround reads them while luzNoChao is missing): at their old defaults it is luzNoChao's
    if (!t->contains(kMoves[0].key) && (t->contains(kOldGround.key) || t->contains(kOldLotGround.key)) && AtOldOrNone(*t, kOldGround) && AtOldOrNone(*t, kOldLotGround)) {
        t->insert_or_assign(kMoves[0].key, kMoves[0].after);
        moved.emplace_back(kMoves[0].key);
    }
    if (AtOldDefault(*t, kStreetTint) && AtOldDefault(*t, kLotTint) && (*t)[kLotTintOwn].value<bool>() == false) {
        set(kStreetTint);
        set(kLotTint);
        t->insert_or_assign(kLotTintOwn, true);
        moved.emplace_back(kLotTintOwn);
    }
    return moved;
}

} // namespace ApexLightingDefaults
