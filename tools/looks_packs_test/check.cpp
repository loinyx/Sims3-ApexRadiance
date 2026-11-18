// Offline checks of the shader packs of Looks (features/looks_packs.cpp): allowed hosts, HTTP failures, unpacking (layout,
// caps, unsafe paths), ReShade's list joined with the index and the downloaded folders, the download queue with simulated
// HTTP answers (a pack gone, GitHub busy, no connection, a stop), and which pack has a missing .fx. Writes only inside
// the folder given (a test Apex Radiance folder). With --real it also downloads one small pack (Ptho-FX) from GitHub.
#include "looks_packs.h"
#include "looks_fx.h"
#include "apex_log.h"
#include "third_party/miniz/miniz.h"
#include "third_party/miniz/miniz_license.h"
#include <windows.h>
#include <cstdio>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

using namespace Looks::Packs;

static std::wstring g_apexDir;
namespace ApexPaths {
const std::wstring& ApexDirectory() { return g_apexDir; }
bool EnsureApexDirectory() { return !g_apexDir.empty(); }
}
namespace ApexLog { void Write(Level, const std::string& text, const std::source_location&) noexcept { fprintf(stderr, "log: %s\n", text.c_str()); } }
namespace CrashReport { void ThreadStart() {} }
namespace HookGuard { void Note(const char* where) noexcept { fprintf(stderr, "caught: %s\n", where); } }
// the catalog's part that looks_packs.cpp uses
static int g_scans = 0;
namespace Looks::Fx {
std::wstring ShadersFolder() { return g_apexDir.empty() ? std::wstring() : g_apexDir + L"Shaders\\"; }
void RequestScan(bool) { ++g_scans; }
bool InsideRoot(const std::wstring& root, const std::wstring& path)
{
    return path.size() > root.size() && _wcsnicmp(root.c_str(), path.c_str(), root.size()) == 0 && path.find(L"..") == std::wstring::npos;
}
}

static int checks = 0, failures = 0;
static void Check(bool ok, const std::string& what)
{
    ++checks;
    if (!ok) { ++failures; printf("FAIL %s\n", what.c_str()); }
}

static bool Exists(const std::wstring& p) { return GetFileAttributesW(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

// A zip as GitHub's codeload makes one: every entry under "<repo>-<branch>/"
static std::string MakeZip(const std::vector<std::pair<std::string, std::string>>& files)
{
    mz_zip_archive z{};
    mz_zip_writer_init_heap(&z, 0, 0);
    for (auto& [name, body] : files) mz_zip_writer_add_mem(&z, name.c_str(), body.data(), body.size(), MZ_DEFAULT_COMPRESSION);
    void* buf = nullptr;
    size_t n = 0;
    mz_zip_writer_finalize_heap_archive(&z, &buf, &n);
    std::string out((const char*)buf, n);
    mz_zip_writer_end(&z);
    return out;
}

int main(int argc, char** argv)
{
    if (argc < 2) { printf("usage: looks_packs_test <empty test folder> [--real]\n"); return 2; }
    wchar_t full[MAX_PATH];
    GetFullPathNameW(std::wstring(argv[1], argv[1] + strlen(argv[1])).c_str(), MAX_PATH, full, nullptr);
    g_apexDir = full;
    if (g_apexDir.back() != L'\\') g_apexDir += L'\\';
    const bool real = argc > 2 && !strcmp(argv[2], "--real");
    std::error_code ec;
    std::filesystem::remove_all(PacksFolder(), ec);
    std::filesystem::create_directories(PacksFolder(), ec);

    // ---- hosts
    Check(AllowedUrl(L"https://codeload.github.com/a/b/zip/refs/heads/main"), "codeload allowed");
    Check(AllowedUrl(L"https://github.com/a/b/archive/main.zip"), "github.com allowed");
    Check(AllowedUrl(L"https://objects.githubusercontent.com/x"), "objects allowed");
    Check(AllowedUrl(L"https://raw.githubusercontent.com/crosire/reshade-shaders/list/EffectPackages.ini"), "raw allowed");
    Check(!AllowedUrl(L"http://codeload.github.com/a/b"), "plain HTTP refused");
    Check(!AllowedUrl(L"https://codeload.github.com.evil.example/a"), "a look-alike host refused");
    Check(!AllowedUrl(L"https://evil.example/codeload.github.com"), "another host refused");
    Check(!AllowedUrl(L"https://user@github.com/a"), "a user name refused");
    Check(!AllowedUrl(L"https://github.com:8443/a"), "a port refused");

    // ---- HTTP failures
    Check(FailureFor(404, false) == Failure::Gone && FailureFor(451, false) == Failure::Gone && FailureFor(410, false) == Failure::Gone, "404, 410, 451: gone");
    Check(FailureFor(403, true) == Failure::Busy && FailureFor(429, false) == Failure::Busy, "rate limits: busy");
    Check(FailureFor(403, false) == Failure::Failed && FailureFor(500, false) == Failure::Failed, "other answers: failed");
    Check(std::string(FailureText(Failure::Gone)) == "This pack is no longer on GitHub", "gone text");

    // ---- unpacking
    const std::string fx = "technique Test { }\n";
    const std::string zip = MakeZip({
        { "Pack-main/Shaders/A.fx", fx },
        { "Pack-main/Shaders/Sub/B.fxh", "// header\n" },
        { "Pack-main/Textures/T.png", "png" },
        { "Pack-main/Shaders/Near.png", "png" },
        { "Pack-main/Root.fx", fx },
        { "Pack-main/README.md", "readme" },
        { "Pack-main/docs/shot.png", "png" },
        { "Pack-main/Shaders/../../Evil.fx", fx },
        { "Pack-main/Shaders/a:b.fx", fx },
        { "Pack-main/Shaders/x/y/z/w/Deep.fx", fx },
        { "Pack-main/tool.exe", "MZ" },
    });
    const std::wstring dest = PacksFolder() + L"Unpack";
    Unpacked u;
    Check(Unpack(zip.data(), zip.size(), dest, &u) == Failure::None, "the zip unpacks");
    Check(Exists(dest + L"\\Shaders\\A.fx") && Exists(dest + L"\\Shaders\\Sub\\B.fxh") && Exists(dest + L"\\Shaders\\Root.fx"), "shader files under Shaders");
    Check(Exists(dest + L"\\Textures\\T.png") && Exists(dest + L"\\Textures\\Near.png"), "images under Textures");
    Check(!Exists(dest + L"\\Textures\\shot.png") && !Exists(dest + L"\\README.md") && !Exists(dest + L"\\tool.exe"), "other files left out");
    Check(!Exists(PacksFolder() + L"Evil.fx") && !Exists(g_apexDir + L"Evil.fx") && !Exists(dest + L"\\Shaders\\Evil.fx"), "a .. path is never written");
    Check(!Exists(dest + L"\\Shaders\\x\\y\\z\\w\\Deep.fx"), "more than four folders deep left out");
    Check(u.shaders == 3 && u.textures == 2 && u.skipped >= 4, "counts: " + std::to_string(u.shaders) + " shaders, " + std::to_string(u.textures) + " textures, " + std::to_string(u.skipped) + " left out");
    Check(Unpack("not a zip", 9, PacksFolder() + L"Bad", nullptr) == Failure::BadZip && !Exists(PacksFolder() + L"Bad"), "not a zip: refused");
    {
        std::vector<std::pair<std::string, std::string>> many;
        for (size_t i = 0; i <= kMaxFiles; ++i) many.push_back({ "P-main/Shaders/F" + std::to_string(i) + ".fxh", "" });
        const std::string big = MakeZip(many);
        Check(Unpack(big.data(), big.size(), PacksFolder() + L"Many", nullptr) == Failure::TooLarge, "over 5000 files: refused");
    }
    std::filesystem::remove_all(dest, ec);

    // ---- the list
    const std::string ini =
        "[00]\nPackageName=Standard effects\nDownloadUrl=https://github.com/crosire/reshade-shaders/archive/slim.zip\n\n"
        "[01]\nPackageName=SweetFX by CeeJay.dk\nDownloadUrl=https://github.com/CeeJayDK/SweetFX/archive/master.zip\n\n"
        "[02]\nPackageName=Legacy effects\nDownloadUrl=https://github.com/crosire/reshade-shaders/archive/legacy.zip\n\n"
        "[03]\nPackageName=New pack by Someone\nDownloadUrl=https://github.com/someone/newpack/archive/refs/heads/main.zip\n\n"
        "[04]\nPackageName=Not GitHub\nDownloadUrl=https://example.com/x.zip\n";
    {
        const auto all = MergeList("", {});
        Check(all.size() == (size_t)kIndexCount && all.size() == 45, "the built-in index has the 45 packs");
        const auto l = MergeList(ini, { "qUINT", "Hand made" });
        auto find = [&](const char* f) -> const Pack* { for (auto& p : l) if (p.folder == f) return &p; return nullptr; };
        Check(find("SweetFX") && find("SweetFX")->inList && find("SweetFX")->index, "SweetFX from the list, with its index");
        Check(find("ReShade standard effects") && find("ReShade legacy effects"), "both crosire branches");
        Check(find("New pack") && !find("New pack")->licenceKnown && !find("New pack")->index, "a pack the index does not know");
        Check(find("qUINT") && !find("qUINT")->inList, "downloaded, no longer in the list: still there");
        Check(find("Hand made") && !find("Hand made")->inList, "an unknown downloaded folder: still there");
        Check(!find("FXShaders"), "not downloaded and not in the list: left out");
        Check(l.size() == 6, "six packs");
        for (size_t i = 1; i < l.size(); ++i) Check(_stricmp(l[i - 1].name.c_str(), l[i].name.c_str()) <= 0, "by name");
    }

    // ---- the download queue, with simulated answers
    std::vector<std::string> asked;
    SetFetch([&](const std::string& url, uint64_t cap, std::string& body, const Progress& progress) -> Failure {
        asked.push_back(url);
        if (url.find("raw.githubusercontent.com") != std::string::npos) return Failure::Offline;
        if (url.find("CeeJayDK/SweetFX") != std::string::npos) { body = MakeZip({ { "SweetFX-master/Shaders/Vibrance.fx", fx } }); if (progress) progress(body.size(), body.size()); return Failure::None; }
        if (url.find("crosire/reshade-shaders/zip/refs/heads/slim") != std::string::npos) { body = MakeZip({ { "r-slim/Shaders/ReShade.fxh", "//\n" } }); return Failure::None; }
        if (url.find("martymcmodding/qUINT") != std::string::npos) return FailureFor(404, false);
        if (url.find("luluco250/FXShaders") != std::string::npos) return FailureFor(403, true);
        if (url.find("prod80") != std::string::npos) return Failure::Offline;
        return cap ? Failure::Failed : Failure::Failed;
    });
    RefreshList();
    WaitIdle();
    BeginFrame(); // the menu's frame start takes the new list in
    Check(CurrentListState() == ListState::Offline && List().size() == 45, "no connection: the built-in list");
    Download({ "SweetFX", "qUINT", "FXShaders", "prod80 color effects" });
    WaitIdle();
    {
        const Batch b = Status();
        Check(!b.running && b.rows.size() == 5 && b.rows[0].folder == "ReShade standard effects", "the standard effects go first");
        auto row = [&](const char* f) { for (auto& r : b.rows) if (r.folder == f) return r; return Row{}; };
        Check(row("ReShade standard effects").state == RowState::Done && row("SweetFX").state == RowState::Done, "downloaded");
        Check(row("qUINT").state == RowState::Failed && row("qUINT").failure == Failure::Gone, "404: gone");
        Check(row("FXShaders").failure == Failure::Busy, "rate limit: busy");
        Check(row("prod80 color effects").failure == Failure::Offline, "no connection");
        Check(Exists(PacksFolder() + L"SweetFX\\Shaders\\Vibrance.fx") && !Exists(PacksFolder() + L"SweetFX.part"), "SweetFX is in Packs");
        Check(!Exists(PacksFolder() + L"qUINT"), "nothing for a failed pack");
        Check(Unavailable("qUINT") && !Unavailable("FXShaders"), "a pack gone is unavailable for the session");
        Check(g_scans == 2, "a scan after each pack downloaded");
    }
    {
        const std::vector<Pack>& before = List();
        const Pack* first = before.empty() ? nullptr : &before[0];
        Check(&List() == &before && (!first || &List()[0] == first), "between frames the list and its packs stay where they are");
    }
    BeginFrame(); // the downloaded folders, seen at the next frame
    Check(Downloaded("SweetFX") && Downloaded("ReShade standard effects") && !Downloaded("qUINT"), "downloaded folders");
    // a missing .fx: the pack that has it, never one that is gone
    {
        const Pack* p = PackForFile("LumaSharpen.fx", "LumaSharpen");
        Check(p && p->folder == "SweetFX", "LumaSharpen.fx: SweetFX first (earliest on ReShade's list)");
        const Pack* q = PackForFile("qUINT_mxao.fx", "MXAO");
        Check(!q, "a file only in a pack that is gone: no pack (the add-this-file line)");
        Check(!PackForFile("NoSuchFile.fx", "X"), "an unknown file: no pack");
        // a GShade preset's renamed file: another file of a pack with the same technique stands in
        Check(FileForTechnique("LumaSharpen v2.fx", "LumaSharpen") == "LumaSharpen.fx", "a renamed file: the pack file with that technique");
        Check(FileForTechnique("LumaSharpen.fx", "LumaSharpen").empty(), "a file a pack has: no stand-in");
        Check(FileForTechnique("NoSuchFile.fx", "NoSuchTechnique").empty(), "an unknown technique: no stand-in");
    }
    // a pack already downloaded is never touched when its update fails
    {
        SetFetch([&](const std::string&, uint64_t, std::string&, const Progress&) { return FailureFor(404, false); });
        Download({ "SweetFX" });
        WaitIdle();
        Check(Exists(PacksFolder() + L"SweetFX\\Shaders\\Vibrance.fx"), "a failed update keeps the pack");
    }
    // Stop the rest
    {
        SetFetch([&](const std::string&, uint64_t, std::string& body, const Progress& progress) {
            for (int i = 0; i < 200; ++i) { if (progress && !progress(i, 200)) return Failure::Stopped; Sleep(5); }
            body = MakeZip({ { "x-main/Shaders/A.fx", "technique A {}\n" } });
            return Failure::None;
        });
        Download({ "CShade", "CorgiFX", "FGFX" });
        Sleep(50);
        StopRest();
        WaitIdle();
        const Batch b = Status();
        int stopped = 0;
        for (auto& r : b.rows) stopped += r.failure == Failure::Stopped;
        Check(stopped == 3 && !Exists(PacksFolder() + L"CShade"), "Stop the rest: the current and the waiting packs stop");
    }
    Check(std::string(Miniz::kLicense).find("Rich Geldreich") != std::string::npos, "miniz licence text");
    {
        std::ifstream f("third_party/miniz/LICENSE", std::ios::binary);
        std::stringstream s; s << f.rdbuf();
        std::string lic = s.str();
        lic.erase(std::remove(lic.begin(), lic.end(), '\r'), lic.end());
        while (!lic.empty() && (lic.back() == '\n' || lic.back() == '\r' || lic.back() == ' ')) lic.pop_back();
        Check(lic == Miniz::kLicense, "miniz_license.h matches third_party/miniz/LICENSE (run from the repository root)");
    }

    if (real)
    {
        SetFetch(nullptr);
        Download({ "Ptho-FX" });
        WaitIdle();
        const Batch b = Status();
        for (auto& r : b.rows) printf("real: %s %s\n", r.folder.c_str(), r.state == RowState::Done ? "downloaded" : FailureText(r.failure));
        Check(Exists(PacksFolder() + L"Ptho-FX\\Shaders"), "real download of Ptho-FX");
    }
    printf("%d checks, %d failures\n%s\n", checks, failures, failures ? "PROBLEMS" : "OK");
    return failures ? 1 : 0;
}
