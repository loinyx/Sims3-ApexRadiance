// What the Overview's group switches turned off (see overview_restore.h). Credits: @loinyx
#include "overview_restore.h"
#include "apex_config.h"
#include <toml++/toml.hpp>
#include <iterator>
#include <mutex>
#include <optional>

namespace OverviewRestore {
namespace {

std::mutex g_lock;
std::once_flag g_loadOnce;
std::optional<std::vector<std::string>> g_sets[5];
std::optional<std::vector<int64_t>> g_screen;
const char* const kKeys[] = {"performance", "effects", "apex", "lot_streaming", "display"};

void EnsureLoaded() {
    std::call_once(g_loadOnce, [] {
        toml::table root;
        if (!ApexConfig::ReadRoot(root)) return;
        const toml::table* t = root["overview_restore"].as_table();
        if (!t) return;
        std::lock_guard<std::mutex> lock(g_lock);
        for (int i = 0; i < 5; i++)
            if (const toml::array* a = (*t)[kKeys[i]].as_array()) {
                std::vector<std::string> names;
                for (const auto& n : *a)
                    if (const auto s = n.value<std::string>()) names.push_back(*s);
                g_sets[i] = std::move(names);
            }
        if (const toml::array* a = (*t)["apex_screen"].as_array()) {
            std::vector<int64_t> v;
            for (const auto& n : *a) v.push_back(n.value_or(int64_t{0}));
            if (v.size() == 6) g_screen = std::move(v);
        }
    });
}

} // namespace

bool Has(Scope scope) {
    EnsureLoaded();
    std::lock_guard<std::mutex> lock(g_lock);
    return g_sets[static_cast<int>(scope)].has_value();
}

std::vector<std::string> Names(Scope scope) {
    EnsureLoaded();
    std::lock_guard<std::mutex> lock(g_lock);
    return g_sets[static_cast<int>(scope)].value_or(std::vector<std::string>{});
}

void Remember(Scope scope, std::vector<std::string> names) {
    EnsureLoaded();
    {
        std::lock_guard<std::mutex> lock(g_lock);
        g_sets[static_cast<int>(scope)] = std::move(names);
    }
    ApexConfig::RequestSave();
}

void Forget(Scope scope) {
    EnsureLoaded();
    {
        std::lock_guard<std::mutex> lock(g_lock);
        g_sets[static_cast<int>(scope)].reset();
        if (scope == Scope::Apex) g_screen.reset();
    }
    ApexConfig::RequestSave();
}

void ForgetAll() {
    EnsureLoaded();
    {
        std::lock_guard<std::mutex> lock(g_lock);
        for (size_t i = 0; i < std::size(g_sets); i++) // (the Display switch's settings: no profile sets them)
            if (i != static_cast<size_t>(Scope::Display)) g_sets[i].reset();
        g_screen.reset();
    }
    ApexConfig::RequestSave();
}

void RememberScreen(const int (&values)[6]) {
    EnsureLoaded();
    std::lock_guard<std::mutex> lock(g_lock);
    g_screen = std::vector<int64_t>(values, values + 6);
}

bool Screen(int (&values)[6]) {
    EnsureLoaded();
    std::lock_guard<std::mutex> lock(g_lock);
    if (!g_screen) return false;
    for (int i = 0; i < 6; i++) values[i] = static_cast<int>((*g_screen)[i]);
    return true;
}

void SaveToToml(toml::table& root) {
    EnsureLoaded();
    std::lock_guard<std::mutex> lock(g_lock);
    toml::table t;
    for (int i = 0; i < 5; i++)
        if (g_sets[i]) {
            toml::array a;
            for (const std::string& n : *g_sets[i]) a.push_back(n);
            t.insert(kKeys[i], std::move(a));
        }
    if (g_screen) {
        toml::array a;
        for (int64_t v : *g_screen) a.push_back(v);
        t.insert("apex_screen", std::move(a));
    }
    if (t.empty()) root.erase("overview_restore");
    else root.insert_or_assign("overview_restore", std::move(t));
}

} // namespace OverviewRestore
