// PlantScan's bake (features/plant_scan.cpp, run in the game on custom content) against generate.py's table
// (features/plant_puffs_data.inc) on the game's own packages: the same models, the same balls to 1 mm and 0.001 of depth.
// With a second folder (Documents\Electronic Arts\The Sims 3), it also bakes Mods and DCCache there and lists the models.
// Read-only. Build and run (x86 Native Tools prompt, from the repository root):
//   cl /nologo /std:c++20 /utf-8 /EHsc /O2 /DNOMINMAX /D_CRT_SECURE_NO_WARNINGS /DPLANT_SCAN_BAKE_ONLY /I. /Iframework /Ifeatures
//      tools\plant_puffs_table\check.cpp features\plant_scan.cpp features\refpack_codec.cpp /Fe:%TEMP%\plant_check.exe
//   %TEMP%\plant_check.exe "<game folder>" ["<Sims 3 documents folder>"]
#include <windows.h>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <vector>
#include "features/plant_scan.h"

namespace {
void Collect(const std::wstring& dir, const wchar_t* ext, std::vector<std::wstring>& out, bool deep, bool builds) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const std::wstring n = fd.cFileName;
        if (n == L"." || n == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (deep) Collect(dir + n + L"\\", ext, out, deep, builds);
        } else if (n.size() > wcslen(ext) && _wcsicmp(n.c_str() + n.size() - wcslen(ext), ext) == 0 &&
                   (!builds || n.rfind(L"FullBuild", 0) == 0 || n.rfind(L"DeltaBuild", 0) == 0))
            out.push_back(dir + n);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}
std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = towlower(c);
    return s;
}
} // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        printf("usage: plant_check <game folder> [<Sims 3 documents folder>]\n");
        return 2;
    }
    std::wstring game = argv[1];
    if (game.back() != L'\\') game += L'\\';
    std::vector<std::wstring> files;
    Collect(game, L".package", files, true, true);
    // as generate.py's Packages: Full builds first, a Delta build overriding them, each group by its lower-case path
    std::sort(files.begin(), files.end(), [](const std::wstring& a, const std::wstring& b) {
        const bool da = a.find(L"\\DeltaBuild") != std::wstring::npos, db = b.find(L"\\DeltaBuild") != std::wstring::npos;
        return da != db ? db : Lower(a) < Lower(b);
    });
    auto t0 = std::chrono::steady_clock::now();
    std::unordered_map<uint64_t, std::vector<ObjectLegs::Puff>> baked;
    PlantScan::Bake(files, baked);
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    int failures = 0, balls = 0;
    for (const ObjectLegs::PlantModel& m : ObjectLegs::kPlantModels) {
        const auto it = baked.find(m.lite);
        if (it == baked.end() || it->second.size() != m.count) {
            printf("FAIL %016llX: the table has %u balls, the C++ bake %zu\n", static_cast<unsigned long long>(m.lite), m.count, it == baked.end() ? 0 : it->second.size());
            failures++;
            continue;
        }
        for (int i = 0; i < m.count; i++) {
            const ObjectLegs::Puff &a = ObjectLegs::kPuffs[m.first + i], &b = it->second[i];
            if (std::abs(a.x - b.x) > 1 || std::abs(a.y - b.y) > 1 || std::abs(a.z - b.z) > 1 || std::abs(a.r - b.r) > 1 || std::abs(int(a.tau) - int(b.tau)) > 1) {
                printf("FAIL %016llX ball %d: table (%d %d %d r %d tau %u), C++ (%d %d %d r %d tau %u)\n", static_cast<unsigned long long>(m.lite), i, a.x, a.y, a.z, a.r, a.tau, b.x, b.y, b.z, b.r, b.tau);
                failures++;
            }
            balls++;
        }
    }
    const size_t extra = baked.size() - std::count_if(baked.begin(), baked.end(), [](const auto& kv) { return ObjectLegs::BakedPlant(kv.first) != nullptr; });
    printf("%zu packages in %.0f ms: %zu models baked, %zu table models (%d balls) checked, %zu baked models not in the table, %d failures\n",
           files.size(), ms, baked.size(), std::size(ObjectLegs::kPlantModels), balls, extra, failures);
    if (argc > 2) {
        std::wstring docs = argv[2];
        if (docs.back() != L'\\') docs += L'\\';
        std::vector<std::wstring> cc;
        Collect(docs + L"Mods\\", L".package", cc, true, false);
        Collect(docs + L"DCCache\\", L".dbc", cc, false, false);
        t0 = std::chrono::steady_clock::now();
        std::unordered_map<uint64_t, std::vector<ObjectLegs::Puff>> found;
        std::unordered_map<uint64_t, std::string> names;
        PlantScan::Bake(cc, found, &names);
        const double ccMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        int inTable = 0;
        for (const auto& [lite, puffs] : found) {
            const bool known = ObjectLegs::BakedPlant(lite) != nullptr;
            inTable += known;
            printf("  %016llX %2zu balls%s %s\n", static_cast<unsigned long long>(lite), puffs.size(), known ? " (in the table)" : "", names[lite].c_str());
        }
        printf("custom content: %zu files in %.0f ms, %zu plant models (%d already in the table)\n", cc.size(), ccMs, found.size(), inTable);
    }
    return failures ? 1 : 0;
}
