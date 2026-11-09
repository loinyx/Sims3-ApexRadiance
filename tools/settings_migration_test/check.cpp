// The one-time move to the new lighting defaults: what is at its old default moves, a choice stays, and the new values are
// the ones the feature registers. Usage: settings_migration_checks <repo>
#include "apex_lighting_defaults.h"
#include <cstdio>
#include <fstream>
#include <iterator>
#include <regex>
#include <sstream>

using namespace ApexLightingDefaults;

static int g_failed = 0, g_passed = 0;
static void Check(bool ok, const char* what) {
    (ok ? g_passed : g_failed)++;
    if (!ok) std::printf("FAILED: %s\n", what);
}
static double Value(const toml::table& root, const char* key) { return root["patches"]["NightTerrainRelight"][key].value_or(-1.0); }

// Every key as 2.18 wrote it for a player who changed nothing (floats as saved, with their rounding)
static const char* const kUntouched = R"toml(
[patches.NightTerrainRelight]
enabled = true
luzNoChao = 1.0
forcaNosObjetos = 0.75
forcaLuzPorPixelNosObjetos = 0.75
forcaNasCercas = 0.75
forcaNasParedes = 0.30000001192092896
forcaNosTelhados = 0.44999998807907104
reflexoNoLago = 1.559999942779541
luzDasLampadasNatural = 1.0
corDasLampadasDoLote = 1.0
corPropriaNoLote = false
detalheDaLuz = 1
)toml";

// The maintainer's values: nothing to move
static const char* const kTuned = R"toml(
[patches.NightTerrainRelight]
luzNoChao = 0.896644651889801
forcaNosObjetos = 1.0
forcaLuzPorPixelNosObjetos = 1.0
forcaNasCercas = 1.0
forcaNasParedes = 0.5600000023841858
forcaNosTelhados = 0.6000000238418579
reflexoNoLago = 1.0
luzDasLampadasNatural = 0.2495652139186859
corDasLampadasDoLote = 0.0
corPropriaNoLote = true
)toml";

static toml::table* Night(toml::table& root) { return root["patches"]["NightTerrainRelight"].as_table(); }

int main(int argc, char** argv) {
    {
        toml::table root = toml::parse(kUntouched);
        const auto moved = Apply(root);
        Check(moved.size() == std::size(kMoves) + 3, "an untouched config moves every key");
        for (const Move& m : kMoves) Check(Value(root, m.key) == m.after, m.key);
        Check(Value(root, kStreetTint.key) == kStreetTint.after && Value(root, kLotTint.key) == kLotTint.after, "the lamp colours move");
        Check((*Night(root))[kLotTintOwn].value_or(false), "lot lamps get their own colour");
        Check((*Night(root))["detalheDaLuz"].value_or(0) == 1 && (*Night(root))["enabled"].value_or(false), "other keys stay");
        Check(Apply(root).empty(), "a second run moves nothing");
    }
    {
        // revision 2: Classic goes to Instant for everyone, a config already on revision 1 too; nothing else moves then
        toml::table root = toml::parse("[patches.NightTerrainRelight]\nlampLightingMode = \"classic\"\nforcaNasParedes = 0.30\n");
        const auto moved = Apply(root, 1);
        Check(moved.size() == 1 && (*Night(root))[kLampMode].value_or(std::string()) == "instant", "revision 2 forces Instant");
        Check(Value(root, "forcaNasParedes") == 0.30, "revision 2 leaves the revision 1 keys alone");
        toml::table none = toml::parse("[patches.NightTerrainRelight]\nforcaNasParedes = 0.30\n");
        Check(Apply(none, 1).empty(), "a config with no saved mode is already on the default");
    }
    {
        toml::table root = toml::parse(kTuned);
        const toml::table before = root;
        Check(Apply(root).empty() && root == before, "tuned values stay");
    }
    {
        toml::table root = toml::parse(kUntouched);
        Night(root)->insert_or_assign("forcaNasParedes", 0.4);
        const auto moved = Apply(root);
        Check(Value(root, "forcaNasParedes") == 0.4 && Value(root, "forcaNosObjetos") == 1.0 && moved.size() == std::size(kMoves) + 2, "a chosen value stays, the others move");
    }
    for (const char* key : {kStreetTint.key, kLotTint.key}) {
        toml::table root = toml::parse(kUntouched);
        Night(root)->insert_or_assign(key, 0.5);
        Apply(root);
        Check(Value(root, key) == 0.5 && !(*Night(root))[kLotTintOwn].value_or(true), "a chosen lamp colour keeps all three");
    }
    {
        toml::table root = toml::parse(kUntouched);
        Night(root)->insert_or_assign(kLotTintOwn, true);
        Apply(root);
        Check(Value(root, kLotTint.key) == 1.0 && Value(root, kStreetTint.key) == 1.0, "warm white lot lamps of their own stay");
    }
    {
        toml::table root = toml::parse("[ui]\nlanguage = \"en\"\n");
        Check(Apply(root).empty(), "no Night Lights table: nothing to do");
        toml::table old = toml::parse("[patches.NightTerrainRelight]\nbrilhoNoChao = 0.5\n");
        Check(Apply(old).empty() && !(*Night(old))["luzNoChao"], "a key the file lacks is not added (the old ground keys stay in charge)");
        toml::table before218 = toml::parse("[patches.NightTerrainRelight]\nbrilhoNoChao = 0.75\nforcaDasLampadasDoLote = 0.800000011920929\n");
        Check(Apply(before218).size() == 1 && Value(before218, "luzNoChao") == kMoves[0].after, "a config before 2.18 at the old ground keys' defaults takes the new ground light");
        toml::table chosenLot = toml::parse("[patches.NightTerrainRelight]\nbrilhoNoChao = 0.75\nforcaDasLampadasDoLote = 0.72\n");
        Check(Apply(chosenLot).empty(), "a chosen old ground key keeps the old keys in charge");
    }
    if (argc > 1) { // the values moved to are the feature's registered defaults
        std::ifstream f(std::string(argv[1]) + "/patches/night_terrain_relight_patch.cpp", std::ios::binary);
        const std::string src((std::istreambuf_iterator<char>(f)), {});
        const auto registered = [&](const Move& m) {
            std::smatch s;
            return std::regex_search(src, s, std::regex(std::string("\"") + m.key + "\", SettingWidget::Slider, ([0-9.]+)f")) && std::fabs(std::stod(s[1]) - m.after) < 1e-6;
        };
        for (const Move& m : kMoves) Check(registered(m), m.key);
        Check(registered(kStreetTint) && registered(kLotTint), "lamp colours registered");
        Check(src.find(std::string("\"") + kLotTintOwn + "\", true") != std::string::npos, "own lot colour registered");
    }
    std::printf("settings migration: %d passed, %d failed\n", g_passed, g_failed);
    return g_failed ? 1 : 0;
}
