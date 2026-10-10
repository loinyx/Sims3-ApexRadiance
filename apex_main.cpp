// Apex Radiance for The Sims 3: ApexRadiance.asi, a standalone ASI for The Sims 3 (TS3W.exe) that runs next to an
// unmodified official Sims3SettingsSetter.
//
// Start-up:
//  DllMain      process check, instance mutexes (a second copy, or an older S3SSApex.asi that loaded first, keeps this
//               copy idle), Direct3DCreate9 export detour (d3d9_bootstrap.h), init thread.
//  init thread  ApexRadiance_LOG.txt, game version, background precompile of Apex's HLSL shaders (shader_cache.h),
//               S3SS detection, features created, one-time migration (previous
//               S3SS\Apex\Apex.toml, else S3SS.toml), settings (menu, display, Picture, profiler); then waits until
//               the game settled (first Present + 1 s, so official S3SS has loaded its own patches), checks for the
//               old combined build and an older S3SSApex.asi, resolves the game-code addresses (game_addresses.h:
//               fixed on Steam 1.67.2, signature scan elsewhere), installs the enabled features, and becomes the pump:
//               every 10 ms each feature's Update() and the config autosave.
//  Lifetime     pinned before any hook or worker starts; released by Windows at process exit.
#include <windows.h>
#include "apex_config.h"
#include "apex_gui.h"
#include "apex_log.h"
#include "apex_paths.h"
#include "apex_util.h"
#include "apex_version.h"
#include "address_space.h"
#include "build_flavor.h"
#include "conflict_guard.h"
#include "crash_report.h"
#include "d3d9_bootstrap.h"
#include "frame_profiler.h"
#include "game_addresses.h"
#include "game_version.h"
#include "hook_guard.h"
#include "module_lifetime.h"
#include "load_timing.h"
#include "overlay.h"
#include "patch_base.h"
#include "s3ss_detect.h"
#include "shader_cache.h"
#include <format>
#include <string>

#pragma comment(lib, "version.lib") // GetFileVersionInfoW (LogEnvironment)

namespace {

HMODULE g_module = nullptr;
HANDLE g_stop = nullptr; // process-lifetime pump; retained for stop-aware startup waits
HANDLE g_thread = nullptr;

constexpr DWORD kPumpIntervalMs = 10;
constexpr ULONGLONG kSettleAfterPresentMs = 1000; // official S3SS loads its patches from its hook thread at startup
constexpr ULONGLONG kSettleTimeoutMs = 20000;     // no Present by then (no device, headless): install anyway

bool Stopping(DWORD waitMs = 0) { return g_stop && WaitForSingleObject(g_stop, waitMs) == WAIT_OBJECT_0; }

std::wstring ModuleName(HMODULE m) {
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(m, path, MAX_PATH);
    const wchar_t* base = path;
    for (const wchar_t* p = path; *p; ++p)
        if (*p == L'\\' || *p == L'/') base = p + 1;
    return base;
}

void OpenLog() {
    if (ApexPaths::EnsureApexDirectory() && ApexLog::Open(ApexPaths::LogFile())) return;
    // Documents not reachable: next to the game (never S3SS_LOG.txt)
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir = exe;
    dir.resize(dir.find_last_of(L"\\/") + 1);
    ApexLog::Open(dir + L"ApexRadiance_LOG.txt");
}

// An older S3SSApex.asi loaded first and runs: this copy stays idle (no hooks, no menu) and only says why in its log.
DWORD WINAPI IdleNoticeThread(LPVOID) {
    OpenLog();
    LOG_ERROR("[Main] An older S3SSApex.asi is also installed; delete it from Game\\Bin. It loaded first, so this copy of " APEX_PRODUCT_NAME
              " " APEX_VERSION_STRING " stays idle (no features, no menu) until it is removed.");
    ApexLog::Close();
    return 0;
}

// "1.2.3.4" from a file's version resource (empty when it has none)
std::string FileVersion(const std::wstring& path) {
    DWORD handle = 0;
    const DWORD size = GetFileVersionInfoSizeW(path.c_str(), &handle);
    if (!size) return {};
    std::string buf(size, '\0');
    VS_FIXEDFILEINFO* info = nullptr;
    UINT len = 0;
    if (!GetFileVersionInfoW(path.c_str(), 0, size, buf.data()) || !VerQueryValueW(buf.data(), L"\\", reinterpret_cast<void**>(&info), &len) || !info) return {};
    return std::format("{}.{}.{}.{}", HIWORD(info->dwFileVersionMS), LOWORD(info->dwFileVersionMS), HIWORD(info->dwFileVersionLS), LOWORD(info->dwFileVersionLS));
}

// What a bug report needs about the player's setup: the ASI mods and the DLLs other mods put in Game\Bin (name, size,
// date, version) and the game's own graphics options (Options.ini)
void LogEnvironment() {
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring dir = exe;
    dir.resize(dir.find_last_of(L"\\/") + 1);
    std::string mods;
    for (const wchar_t* pattern : {L"*.asi", L"d3d9.dll", L"dxgi.dll", L"dinput8.dll", L"wininet.dll", L"version.dll", L"dsound.dll", L"winmm.dll", L"d3d11.dll", L"dxvk.conf", L"*.ini"}) {
        WIN32_FIND_DATAW fd{};
        const HANDLE h = FindFirstFileW((dir + pattern).c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) continue;
        do {
            if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
            SYSTEMTIME st{};
            FileTimeToSystemTime(&fd.ftLastWriteTime, &st);
            const unsigned long long bytes = (static_cast<unsigned long long>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
            const std::string version = FileVersion(dir + fd.cFileName);
            mods += std::format("\n    {} ({} bytes, {:04}-{:02}-{:02}{})", ApexUtil::ToUtf8(fd.cFileName), bytes, st.wYear, st.wMonth, st.wDay,
                                version.empty() ? std::string() : ", version " + version);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    LOG_INFO("[Main] Game\\Bin mods and wrappers:" + (mods.empty() ? std::string(" none found") : mods));
    // The game's graphics options (resolution, edge smoothing, quality levels ...)
    const std::wstring options = ApexPaths::GameDocumentsDirectory() + L"Options.ini";
    FILE* f = nullptr;
    if (!ApexPaths::GameDocumentsDirectory().empty() && _wfopen_s(&f, options.c_str(), L"rb") == 0 && f) {
        std::string text, line;
        char buf[512];
        while (fgets(buf, sizeof buf, f)) {
            line = buf;
            while (!line.empty() && (line.back() == '\n' || line.back() == '\r')) line.pop_back();
            if (!line.empty() && text.size() < 6000) text += "\n    " + line;
        }
        fclose(f);
        LOG_INFO("[Main] Options.ini:" + text);
    } else {
        LOG_INFO("[Main] Options.ini not found in " + ApexUtil::ToUtf8(ApexPaths::GameDocumentsDirectory()));
    }
}

// Waits until the game has presented a frame and a moment has passed (or the timeout). False when stopping.
bool WaitForSettle() {
    const ULONGLONG start = GetTickCount64();
    for (;;) {
        if (Stopping(50)) return false;
        const ULONGLONG now = GetTickCount64();
        if (ApexD3D::PresentSeen() && now - ApexD3D::FirstPresentTick() >= kSettleAfterPresentMs) return true;
        if (now - start >= kSettleTimeoutMs) {
            LOG_WARNING("[Main] No Present after 20 s: installing the features anyway");
            return true;
        }
    }
}

// Start-up, then the pump (InitThread)
DWORD InitBody() {
    OpenLog();
    LOG_INFO(std::format("[Main] {} {} ({}) in {}", APEX_PRODUCT_NAME, APEX_VERSION_STRING, "unified build",
                         ApexUtil::ToUtf8(ModuleName(nullptr))));
    LOG_INFO("[Main] Files: " + ApexUtil::ToUtf8(ApexPaths::ApexDirectory()));
    if (DetectGameVersion()) LOG_INFO(std::format("[Main] Game: {} [0x{:08X}]", GetGameVersionName(), g_exeTimestamp));
    else LOG_WARNING(std::format("[Main] Unknown game build [0x{:08X}]: game-code features start only where their code is found by signature", g_exeTimestamp));
    LogEnvironment();
    // Apex's HLSL shaders (every quality and mode) compile now on a background thread, so the render thread never runs
    // D3DCompile: the features only create the shader objects from the bytecode (shader_cache.h).
    ShaderCache::Start();


    S3SSDetect::Scan();
    ApexD3D::EnsureInstalled(); // only does something when the DllMain install could not happen

    try {
        ApexConfig::LoadDeveloperMode();
        PatchManager::Get().CreateAll();
        ApexConfig::EnsureMigrated();
        ApexConfig::LoadSettings();
    } catch (const std::exception& e) {
        LOG_ERROR(std::string("[Main] Settings could not be loaded: ") + e.what());
    }

    AddressSpace::Start(); // starts only in developer mode, after settings have been loaded
    LOG_INFO(kPublicBuild ? "[Main] Unified build: normal mode" : "[Main] Unified build: developer mode");
    if (!WaitForSettle()) return 0;
    const S3SSDetect::Info& s3ss = S3SSDetect::Rescan();
    if (s3ss.oldStandalone) {
        // It loaded after this build, found Local\S3SSApex.<pid> taken and idles; it still has to go.
        LOG_WARNING("[Main] An older " + ApexUtil::ToUtf8(s3ss.oldStandaloneModule) + " is also installed; delete it from Game\\Bin (it stays idle meanwhile)");
        ApexGui::SetOldStandaloneNotice(ApexUtil::ToUtf8(s3ss.oldStandaloneModule));
    }
    if (s3ss.oldCombinedBuild) {
        LOG_ERROR("[Main] The old combined build (" + ApexUtil::ToUtf8(s3ss.combinedModule) + ") is loaded too: " APEX_PRODUCT_NAME "'s features stay off. Delete it from Game\\Bin.");
        ApexGui::SetStartup(ApexGui::Startup::RefusedOldBuild, ApexUtil::ToUtf8(s3ss.combinedModule));
    } else {
        // Game-code addresses: fixed on Steam 1.67.2, found by signature on other builds (game code decrypted by now)
        GameAddr::Resolve();
        try {
            ApexConfig::LoadFeatures();
        } catch (const std::exception& e) {
            LOG_ERROR(std::string("[Main] Features could not be started: ") + e.what());
        }
        ApexGui::SetStartup(ApexGui::Startup::Running);
        LOG_INFO("[Main] Features started");
        LoadTiming::NoteFeaturesStarted();
    }
    CrashReport::Install(); // after the game's own start-up (it may set a filter too): ApexRadiance_Crash.txt on a crash

    // Pump: features' periodic work (deferred reinstalls, lamp scans scheduled off the render thread) and autosave.
    ULONGLONG lastGuardTick = 0;
    while (!Stopping(kPumpIntervalMs)) {
        // each step caught on its own (07/10, players' Runtime Error): one that throws is noted and the pump goes on
        if (ApexGui::GetStartup() == ApexGui::Startup::Running) HookGuard::Try("Pump: features' periodic work", [] { PatchManager::Get().UpdateAll(); });
        HookGuard::Try("Pump: settings autosave", [] { ApexConfig::PumpAutosave(); });
        HookGuard::Try("Pump: load timing", [] { LoadTiming::Pump(); }); // log lines only ([LoadTiming])
        const ULONGLONG now = GetTickCount64();
        if (now - lastGuardTick >= 1000) {
            lastGuardTick = now;
            HookGuard::Try("Pump: conflict guard", [] { ConflictGuard::Tick(); }); // TODO(step 8): watchdog
            CrashReport::Refresh();
            HookGuard::Try("Pump: crash report feature line", [] {
                std::string on;
                for (const auto& p : PatchManager::Get().GetPatches())
                    if (p->IsEnabled()) on += (on.empty() ? "" : ", ") + p->GetName();
                CrashReport::SetFeatureLine(on);
            });
        }
    }
    return 0;
}

// 07/10, players' Runtime Error: a C++ exception left in start-up or in the pump ends this thread (noted), never the game
DWORD WINAPI InitThread(LPVOID) {
    CrashReport::ThreadStart();
    try {
        return InitBody();
    } catch (...) {
        HookGuard::Note("Start-up and pump thread");
    }
    return 1;
}

bool IsGameProcess() {
    const std::wstring exe = ModuleName(nullptr);
    return _wcsicmp(exe.c_str(), L"TS3W.exe") == 0 || _wcsicmp(exe.c_str(), L"TS3.exe") == 0;
}

} // namespace

BOOL APIENTRY DllMain(HMODULE module, DWORD reason, LPVOID reserved) {
    switch (reason) {
    case DLL_PROCESS_ATTACH: {
        DisableThreadLibraryCalls(module);
        g_module = module;
        if (!IsGameProcess()) return FALSE; // the launcher and other tools: not loaded at all
        if (!ModuleLifetime::Pin(module)) {
            OutputDebugStringA("[" APEX_PRODUCT_NAME "] Could not retain the module for process lifetime; no hooks or workers started\n");
            return FALSE;
        }
        switch (S3SSDetect::AcquireInstanceMutex()) {
        case S3SSDetect::Instance::DuplicateSelf:
            OutputDebugStringA("[" APEX_PRODUCT_NAME "] Another copy of ApexRadiance.asi is already running in this process: this one stays idle\n");
            return TRUE;
        case S3SSDetect::Instance::OldStandaloneFirst:
            OutputDebugStringA("[" APEX_PRODUCT_NAME "] An older S3SSApex.asi is also installed and loaded first: this one stays idle. Delete it from Game\\Bin\n");
            if (HANDLE t = CreateThread(nullptr, 0, IdleNoticeThread, nullptr, 0, nullptr)) CloseHandle(t);
            return TRUE;
        default:
            break;
        }
        LOG_INFO(std::format("[Main] Loaded as {}", ApexUtil::ToUtf8(ModuleName(module))));
        Overlay::SetClient(&ApexGui::Client());
        ApexD3D::InstallFromDllMain(); // d3d9.dll is a static import of TS3W.exe: normally already loaded here
        g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
        g_thread = CreateThread(nullptr, 0, InitThread, nullptr, 0, nullptr);
        if (!g_thread) OutputDebugStringA("[" APEX_PRODUCT_NAME "] Could not start the init thread\n");
        break;
    }
    case DLL_PROCESS_DETACH:
        // Successful attachment is pinned: no live unload. At process exit do not
        // acquire other threads' locks or recreate/release graphics resources.
        break;
    default:
        break;
    }
    return TRUE;
}
