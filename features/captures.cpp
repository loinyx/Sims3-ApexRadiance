// Bug-report captures: folders, the files copied with them and the on-screen notes (see captures.h).
//
// Part of Apex Radiance. Credits: @loinyx
#include "captures.h"
#include "apex_log.h"
#include "hook_guard.h"
#include "apex_paths.h"
#include "apex_util.h"
#include "apex_version.h"
#include "apex_config.h"
#include "game_version.h"
#include "ui/i18n.h"
#include "render_callbacks.h"
#include "d3d9_hooks.h"
#include "overlay.h"
#include "f10_study.h"
#include <windows.h>
#include <shellapi.h>
#include <wincodec.h>
#include <d3d9.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <format>
#include <fstream>
#include <mutex>
#include <map>
#include <sstream>
#include <thread>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "windowscodecs.lib")

namespace Captures {
namespace {

std::mutex g_lock;
std::string g_note;
NoteKind g_noteKind = NoteKind::Info;
unsigned long long g_noteUntil = 0;
std::filesystem::path g_session; // the open session's folder (empty: none); under g_lock
int g_sessionCount = 0;
unsigned long long g_sessionStart = 0; // GetTickCount64 when it was opened
std::vector<std::string> g_sessionItems; // what each capture of the session was
SaveResult g_result;
std::string g_description, g_retryDescription, g_retryWhat;
std::string g_retryTitle;
std::filesystem::path g_retryFolder;
std::map<std::filesystem::path, std::string> g_failedText;
std::map<std::filesystem::path, bool> g_shotJobs; // pending PNGs; protected by g_lock
std::map<std::filesystem::path, int> g_dumpJobs; // pending texture-dump PNGs per capture folder; protected by g_lock
bool g_copyFailed = false, g_shotFailed = false, g_dumpFailed = false;
bool g_notifyCompletion = false;
CaptureKind g_retryKind = CaptureKind::Generic;
std::vector<std::pair<std::filesystem::path, std::filesystem::path>> g_removed;

std::string DescriptionText(const std::string& title, const std::string& text) {
    return (title.empty() ? std::string() : "Title: " + title + "\n\n") + text + "\n";
}

bool ChildName(const std::string& name) {
    return !name.empty() && name != "." && name != ".." && name != ".Removed" &&
           name.find_first_of("\\/:*?\"<>|") == std::string::npos && name.back() != '.' && name.back() != ' ';
}
bool PlainDirectory(const std::filesystem::path& path) {
    const DWORD attrs = GetFileAttributesW(path.c_str());
    return attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) && !(attrs & FILE_ATTRIBUTE_REPARSE_POINT);
}
void UpdateResultLocked() {
    g_result.saving = g_shotJobs.contains(g_retryFolder) || g_dumpJobs.contains(g_retryFolder);
    g_result.failed = g_copyFailed || g_shotFailed || g_dumpFailed;
    for (const auto& [file, text] : g_failedText) {
        (void)text;
        if (file.parent_path() == g_retryFolder) g_result.failed = true;
    }
}
void PruneLater();
void CompleteShot(const std::filesystem::path& folder, bool ok) {
    {
        std::lock_guard<std::mutex> lk(g_lock);
        g_shotJobs.erase(folder);
        if (folder == g_retryFolder) {
            g_shotFailed = !ok;
            UpdateResultLocked();
            g_notifyCompletion = true;
        }
    }
    PruneLater();
}
void CompleteDump(const std::filesystem::path& folder, bool ok) {
    {
        std::lock_guard<std::mutex> lk(g_lock);
        auto it = g_dumpJobs.find(folder);
        if (it != g_dumpJobs.end() && --it->second <= 0) g_dumpJobs.erase(it);
        if (folder == g_retryFolder) {
            if (!ok) g_dumpFailed = true;
            UpdateResultLocked();
            if (!g_result.saving) g_notifyCompletion = true;
        }
    }
    if (!ok) LOG_WARNING("[Captures] A texture image could not be written in " + ApexUtil::ToUtf8(folder.wstring()));
    PruneLater();
}

// One PNG through WIC (lossless). fmt: GUID_WICPixelFormat24bppBGR or GUID_WICPixelFormat8bppGray; the calling thread
// must have COM initialised.
bool EncodePng(const std::filesystem::path& file, const BYTE* pixels, size_t size, UINT w, UINT h, UINT bytesPerPixel, WICPixelFormatGUID want) {
    IWICImagingFactory* factory = nullptr;
    IWICStream* stream = nullptr;
    IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) &&
              SUCCEEDED(factory->CreateStream(&stream)) && SUCCEEDED(stream->InitializeFromFilename(file.c_str(), GENERIC_WRITE)) &&
              SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatPng, nullptr, &enc)) && SUCCEEDED(enc->Initialize(stream, WICBitmapEncoderNoCache)) &&
              SUCCEEDED(enc->CreateNewFrame(&frame, nullptr)) && SUCCEEDED(frame->Initialize(nullptr)) && SUCCEEDED(frame->SetSize(w, h));
    WICPixelFormatGUID fmt = want;
    ok = ok && SUCCEEDED(frame->SetPixelFormat(&fmt)) && IsEqualGUID(fmt, want) &&
         SUCCEEDED(frame->WritePixels(h, w * bytesPerPixel, static_cast<UINT>(size), const_cast<BYTE*>(pixels))) && SUCCEEDED(frame->Commit()) &&
         SUCCEEDED(enc->Commit());
    if (frame) frame->Release();
    if (enc) enc->Release();
    if (stream) stream->Release();
    if (factory) factory->Release();
    return ok;
}

// ---- Texture-dump PNGs (Light capture, developer mode): one worker thread, in order, bounded memory ----
struct DumpJob {
    std::filesystem::path folder, file;
    std::vector<BYTE> pixels;
    UINT w = 0, h = 0;
    int channels = 3;
};
std::mutex g_dumpLock;
std::condition_variable g_dumpCv;
std::deque<DumpJob> g_dumpQueue;
size_t g_dumpQueuedBytes = 0;
bool g_dumpWorker = false;
constexpr size_t kDumpQueueCap = 320u << 20; // queued pixels above this make the render thread wait (32-bit address space)

void DumpWorker() {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    for (;;) {
        DumpJob job;
        {
            std::lock_guard<std::mutex> lk(g_dumpLock);
            if (g_dumpQueue.empty()) { g_dumpWorker = false; break; }
            job = std::move(g_dumpQueue.front());
            g_dumpQueue.pop_front();
        }
        const bool ok = EncodePng(job.file, job.pixels.data(), job.pixels.size(), job.w, job.h, static_cast<UINT>(job.channels),
                                  job.channels == 1 ? GUID_WICPixelFormat8bppGray : GUID_WICPixelFormat24bppBGR);
        const size_t bytes = job.pixels.size();
        std::vector<BYTE>().swap(job.pixels);
        {
            std::lock_guard<std::mutex> lk(g_dumpLock);
            g_dumpQueuedBytes -= bytes;
        }
        g_dumpCv.notify_all();
        CompleteDump(job.folder, ok);
    }
    if (SUCCEEDED(com)) CoUninitialize();
    g_dumpCv.notify_all();
}

std::filesystem::path Dir() { return std::filesystem::path(ApexPaths::ApexDirectory()); }

// Explorer on a folder, on a short-lived thread with COM (30/09: called from the menu frame, ShellExecuteW pumped the
// game window's messages, the overlay's window procedure ran again inside the frame and its mutex threw: a crash)
void ShowInExplorer(const std::filesystem::path& folder) {
    // (07/10, players' Runtime Error: started and run under HookGuard::StartDetached, so neither can end the game)
    HookGuard::StartDetached("Captures: open a folder in Explorer", [folder] {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        const HINSTANCE r = ShellExecuteW(nullptr, L"open", folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(r) <= 32) LOG_WARNING(std::format("[Captures] Could not open the folder ({})", reinterpret_cast<INT_PTR>(r)));
        if (SUCCEEDED(com)) CoUninitialize();
    });
}

bool CopyIfThere(const std::filesystem::path& from, const std::filesystem::path& to) {
    std::error_code ec;
    if (std::filesystem::exists(from, ec)) std::filesystem::copy_file(from, to, std::filesystem::copy_options::overwrite_existing, ec);
    return !ec;
}

uint64_t FolderSize(const std::filesystem::path& path) {
    namespace fs = std::filesystem;
    uint64_t bytes = 0;
    std::error_code ec;
    auto it = fs::recursive_directory_iterator(path, fs::directory_options::skip_permission_denied, ec);
    for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_directory(ec) && !PlainDirectory(it->path())) it.disable_recursion_pending();
        if (it->is_regular_file(ec)) {
            const auto size = it->file_size(ec);
            if (!ec) bytes += size;
        }
    }
    return bytes;
}

// ---- Screenshots (user, 30/09: "the game should capture the screen too, to have a print") ----
// Finish queues the capture folder; the next frame's picture is copied from the back buffer and written as
// Screenshot.png on a short-lived thread (WIC). Menu closed: at Present, so the picture has everything the player sees
// (Color filters included); the capture notes are not drawn that frame (ScreenshotPending). Menu open: the bootstrap
// runs Picture once, then fires filteredSceneBeforeOverlay, before the Apex menu draws.
struct ShotJob {
    std::filesystem::path file;
    std::filesystem::path reportFolder;
    bool report = true;
    int skipPresents = 0;
};
std::vector<ShotJob> g_shots; // render-thread requests; report shots and standalone player photos
bool g_shotHooks = false;
std::atomic<bool> g_shotsOn{true};
std::atomic<bool> g_gameUiHidden{false};
struct PlayerPhotoState {
    bool active = false;
    bool restoreOverlay = false;
    bool toggledGameUi = false;
    unsigned long long requestedAt = 0;
};
PlayerPhotoState g_playerPhoto;

bool PostGameUiToggle() {
    return Overlay::PostGameKeyPress(VK_F10); // bypass Apex hotkey handling, but let the game toggle its UI
}

void RestorePlayerPhoto() {
    if (!g_playerPhoto.active) return;
    if (g_playerPhoto.toggledGameUi && !PostGameUiToggle())
        LOG_WARNING("[Captures] Could not restore the game's UI after a screenshot");
    Overlay::SetCaptureSuppressed(false);
    Overlay::SetVisible(g_playerPhoto.restoreOverlay);
    g_playerPhoto = {};
}

void WritePng(ShotJob job, std::vector<BYTE> bgr, UINT w, UINT h) {
    HookGuard::StartDetached("Captures: screenshot writer", [job = std::move(job), bgr = std::move(bgr), w, h] {
        const auto& file = job.file;
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool ok = EncodePng(file, bgr.data(), bgr.size(), w, h, 3, GUID_WICPixelFormat24bppBGR);
        if (SUCCEEDED(com)) CoUninitialize();
        if (!ok) LOG_WARNING("[Captures] The screenshot could not be written: " + ApexUtil::ToUtf8(file.wstring()));
        if (job.report) CompleteShot(job.reportFolder, ok);
        else Notify(I18n::Tr(ok ? "Screenshot saved" : "The screenshot could not be saved"), 4,
                    ok ? NoteKind::Screenshot : NoteKind::Warning);
    });
}

// Render thread: the finished back buffer (including post-scene and Picture passes) -> queued PNGs.
void TakeShots(IDirect3DDevice9* dev) {
    if (g_shots.empty() || !dev) return;
    // Wait for the posted hide key to pass through the game procedure before counting a clean frame.
    if (g_playerPhoto.active && g_playerPhoto.toggledGameUi && !g_gameUiHidden.load()) {
        if (GetTickCount64() - g_playerPhoto.requestedAt < 2000) return;
        LOG_WARNING("[Captures] Screenshot cancelled: the UI hide key was not processed in time");
        std::erase_if(g_shots, [](const ShotJob& job) { return !job.report; });
        RestorePlayerPhoto();
        return;
    }
    std::vector<ShotJob> jobs;
    for (auto it = g_shots.begin(); it != g_shots.end();) {
        if (it->skipPresents > 0) { --it->skipPresents; ++it; }
        else { jobs.push_back(std::move(*it)); it = g_shots.erase(it); }
    }
    if (jobs.empty()) return;
    const bool hasPlayerPhoto = std::any_of(jobs.begin(), jobs.end(), [](const ShotJob& j) { return !j.report; });
    IDirect3DSurface9 *bb = nullptr, *resolved = nullptr, *sys = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) {
        for (const auto& job : jobs) if (job.report) CompleteShot(job.reportFolder, false);
        if (hasPlayerPhoto) RestorePlayerPhoto();
        return;
    }
    D3DSURFACE_DESC d{};
    bb->GetDesc(&d);
    std::vector<BYTE> bgr;
    if (d.Format == D3DFMT_X8R8G8B8 || d.Format == D3DFMT_A8R8G8B8) {
        IDirect3DSurface9* src = bb;
        if (d.MultiSampleType != D3DMULTISAMPLE_NONE && SUCCEEDED(dev->CreateRenderTarget(d.Width, d.Height, d.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &resolved, nullptr)) &&
            SUCCEEDED(dev->StretchRect(bb, nullptr, resolved, nullptr, D3DTEXF_NONE)))
            src = resolved;
        if ((d.MultiSampleType == D3DMULTISAMPLE_NONE || src == resolved) &&
            SUCCEEDED(dev->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &sys, nullptr)) && SUCCEEDED(dev->GetRenderTargetData(src, sys))) {
            D3DLOCKED_RECT lr{};
            if (SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
                bgr.resize(static_cast<size_t>(d.Width) * d.Height * 3);
                for (UINT y = 0; y < d.Height; y++) {
                    const BYTE* s = static_cast<const BYTE*>(lr.pBits) + static_cast<size_t>(y) * lr.Pitch;
                    BYTE* o = bgr.data() + static_cast<size_t>(y) * d.Width * 3;
                    for (UINT x = 0; x < d.Width; x++) { // B G R (A dropped: it is the bloom mask, not transparency)
                        o[3 * x] = s[4 * x];
                        o[3 * x + 1] = s[4 * x + 1];
                        o[3 * x + 2] = s[4 * x + 2];
                    }
                }
                sys->UnlockRect();
            }
        }
    }
    if (sys) sys->Release();
    if (resolved) resolved->Release();
    bb->Release();
    if (bgr.empty()) {
        LOG_WARNING("[Captures] No screenshot: the screen format could not be read");
        for (const auto& job : jobs) if (job.report) CompleteShot(job.reportFolder, false);
        if (hasPlayerPhoto) RestorePlayerPhoto();
        return;
    }
#ifdef APEX_F10_STUDY
    if (hasPlayerPhoto) F10Study::PhotoReceipt(d.Width, d.Height, g_playerPhoto.toggledGameUi);
#endif
    for (size_t i = 0; i < jobs.size(); i++) WritePng(std::move(jobs[i]), i + 1 < jobs.size() ? bgr : std::move(bgr), d.Width, d.Height);
    if (hasPlayerPhoto) RestorePlayerPhoto();
}

void ShotAtSceneEnd(IDirect3DDevice9* dev) {
    if (Overlay::IsVisible()) TakeShots(dev); // the menu is about to be drawn: the picture before it
}

void QueueShot(const std::filesystem::path& folder) {
    if (!g_shotsOn.load()) return;
    g_shots.push_back({folder / L"Screenshot.png", folder, true, 0});
    { std::lock_guard<std::mutex> lk(g_lock); g_shotJobs[folder] = true; }
    if (g_shotHooks) return;
    g_shotHooks = true; // once; the callbacks do nothing while nothing is queued
    RenderCallbacks::filteredSceneBeforeOverlay.Add(ShotAtSceneEnd);
    D3D9Hooks::RegisterPresent("CapturesScreenshot", [](D3D9Hooks::DeviceContext& ctx, const RECT*, const RECT*, HWND, const RGNDATA*) {
        TakeShots(ctx.device); // menu closed: the frame as it is shown (notes held back for it)
        return D3D9Hooks::HookAction::Continue;
    }, D3D9Hooks::Priority::First);
}

bool QueuePlayerPhoto(const std::filesystem::path& file, bool hideGameUi) {
    g_playerPhoto.active = true;
    g_playerPhoto.requestedAt = GetTickCount64();
    g_playerPhoto.restoreOverlay = Overlay::IsVisible();
    g_playerPhoto.toggledGameUi = hideGameUi && !g_gameUiHidden.load();
    Overlay::SetVisible(false);
    Overlay::SetCaptureSuppressed(true);
    if (g_playerPhoto.toggledGameUi && !PostGameUiToggle()) {
        LOG_WARNING("[Captures] Could not hide the game's UI; screenshot cancelled");
        Overlay::SetCaptureSuppressed(false);
        Overlay::SetVisible(g_playerPhoto.restoreOverlay);
        g_playerPhoto = {};
        return false;
    }
    g_shots.push_back({file, {}, false, 1}); // allow the posted F10 toggle to reach the game before reading the back buffer
    if (g_shotHooks) return true;
    g_shotHooks = true; // once; the callbacks do nothing while nothing is queued
    RenderCallbacks::filteredSceneBeforeOverlay.Add(ShotAtSceneEnd);
    D3D9Hooks::RegisterPresent("CapturesScreenshot", [](D3D9Hooks::DeviceContext& ctx, const RECT*, const RECT*, HWND, const RGNDATA*) {
        TakeShots(ctx.device); // menu closed: the frame as it is shown (notes held back for it)
        return D3D9Hooks::HookAction::Continue;
    }, D3D9Hooks::Priority::First);
    return true;
}

// ---- Automatic cleanup (07/10: Captures\ reached 19 GB in 3 days) ----
// "YYYY-MM-DD HH-MM-SS <kind>": the names NewFolder / BeginSession make; nothing else in Captures\ is touched
bool CaptureFolderName(const std::wstring& n) {
    if (n.size() < 21 || n[20] == L' ') return false;
    static const wchar_t kShape[] = L"dddd-dd-dd dd-dd-dd ";
    for (size_t i = 0; i < 20; i++) {
        if (kShape[i] == L'd' ? !(n[i] >= L'0' && n[i] <= L'9') : n[i] != kShape[i]) return false;
    }
    return ChildName(ApexUtil::ToUtf8(n));
}
std::atomic<bool> g_pruneRunning{false}, g_pruneAgain{false};

void PruneOnce() {
    namespace fs = std::filesystem;
    const auto ui = ApexConfig::GetUi();
    const uint64_t maxBytes = static_cast<uint64_t>(std::max(0, ui.captureMaxMb)) << 20;
    const size_t maxFolders = static_cast<size_t>(std::max(0, ui.captureMaxFolders));
    if (!maxBytes && !maxFolders) return;
    const fs::path root = Root();
    // protected: the open session and every capture still being written or the latest one (their direct child of Captures\)
    std::vector<fs::path> keep;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        auto top = [&](fs::path p) {
            while (!p.empty() && p.parent_path() != root && p.parent_path() != p) p = p.parent_path();
            if (!p.empty() && p.parent_path() == root) keep.push_back(p.filename());
        };
        if (!g_session.empty()) top(g_session);
        if (!g_retryFolder.empty()) top(g_retryFolder);
        for (const auto& [f, pending] : g_shotJobs) { (void)pending; top(f); }
        for (const auto& [f, n] : g_dumpJobs) { (void)n; top(f); }
    }
    struct Item { fs::path path; uint64_t bytes; bool kept; };
    std::vector<Item> items;
    uint64_t total = 0;
    std::error_code ec;
    for (auto it = fs::directory_iterator(root, fs::directory_options::skip_permission_denied, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        const fs::path p = it->path();
        if (!PlainDirectory(p) || !CaptureFolderName(p.filename().wstring())) continue;
        const uint64_t bytes = FolderSize(p);
        total += bytes;
        items.push_back({p, bytes, std::find(keep.begin(), keep.end(), p.filename()) != keep.end()});
    }
    std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) { return a.path.filename() < b.path.filename(); }); // date first: oldest first
    size_t count = items.size();
    int removed = 0;
    uint64_t freed = 0;
    for (const Item& i : items) {
        const bool over = (maxFolders && count > maxFolders) || (maxBytes && total > maxBytes);
        if (!over) break;
        if (i.kept) continue;
        std::error_code removeError;
        fs::remove_all(i.path, removeError);
        if (removeError) {
            LOG_WARNING(std::format("[Captures] Cleanup could not remove Captures\\{} ({})", ApexUtil::ToUtf8(i.path.filename().wstring()), removeError.message()));
            continue;
        }
        removed++;
        freed += i.bytes;
        total -= i.bytes;
        count--;
    }
    if (removed)
        LOG_INFO(std::format("[Captures] Cleanup: removed the {} oldest capture folders ({:.1f} MB); {} left, {:.1f} MB (limits: {} folders, {} MB)", removed,
                             freed / 1048576.0, count, total / 1048576.0, maxFolders, maxBytes >> 20));
}

void PruneLater() {
    g_pruneAgain.store(true);
    if (g_pruneRunning.exchange(true)) return; // the running pass repeats once more
    if (!HookGuard::StartDetached("Captures: cleanup", [] {
            do {
                while (g_pruneAgain.exchange(false)) PruneOnce();
                g_pruneRunning.store(false);
            } while (g_pruneAgain.load() && !g_pruneRunning.exchange(true)); // a request that arrived as this pass ended
        }))
        g_pruneRunning.store(false);
}

} // namespace

void Prune() { PruneLater(); }

void WritePngAsync(const std::filesystem::path& folder, const std::filesystem::path& file, std::vector<BYTE> pixels, UINT w, UINT h, int channels) {
    if (pixels.size() < static_cast<size_t>(w) * h * channels || (channels != 1 && channels != 3)) return;
    { std::lock_guard<std::mutex> lk(g_lock); g_dumpJobs[folder]++; }
    bool start = false;
    {
        std::unique_lock<std::mutex> lk(g_dumpLock);
        // bounded memory: wait while a running worker holds too much (never when nothing is queued)
        g_dumpCv.wait(lk, [] { return !g_dumpWorker || g_dumpQueuedBytes < kDumpQueueCap; });
        g_dumpQueuedBytes += pixels.size();
        g_dumpQueue.push_back({folder, file, std::move(pixels), w, h, channels});
        if (!g_dumpWorker) start = g_dumpWorker = true;
    }
    if (start && !HookGuard::StartDetached("Captures: texture PNG writer", [] { DumpWorker(); })) {
        std::deque<DumpJob> failed;
        {
            std::lock_guard<std::mutex> lk(g_dumpLock);
            failed.swap(g_dumpQueue);
            g_dumpQueuedBytes = 0;
            g_dumpWorker = false;
        }
        for (const auto& j : failed) CompleteDump(j.folder, false);
    }
}

std::filesystem::path Root() { return Dir() / L"Captures"; }

// A new folder "<date time> <kind>" in parent (" (2)", " (3)"... when the name is taken: never reused)
std::filesystem::path MakeFolder(const std::filesystem::path& parent, const char* kind, bool withDate) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::create_directories(parent, ec);
    SYSTEMTIME t;
    GetLocalTime(&t);
    const std::string base = withDate ? std::format("{:04}-{:02}-{:02} {:02}-{:02}-{:02} {}", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, kind)
                                      : std::format("{:02}-{:02}-{:02} {}", t.wHour, t.wMinute, t.wSecond, kind);
    fs::path p = parent / fs::path(base);
    for (int n = 2; fs::exists(p, ec) && !ec; n++) p = parent / fs::path(std::format("{} ({})", base, n));
    fs::create_directories(p, ec);
    return p;
}

std::filesystem::path NewFolder(const char* kind) {
    std::lock_guard<std::mutex> lk(g_lock);
    if (!g_session.empty()) return MakeFolder(g_session, kind, false); // inside the open session
    return MakeFolder(Root(), kind, true);
}

void BeginSession() {
    std::lock_guard<std::mutex> lk(g_lock);
    if (!g_session.empty()) return;
    g_session = MakeFolder(Root(), "Session", true);
    if (!PlainDirectory(g_session)) {
        g_session.clear();
        g_note = I18n::Tr("Could not create the collection. Check folder access and free space");
        g_noteKind = NoteKind::Warning;
        g_noteUntil = GetTickCount64() + 6000;
        return;
    }
    g_sessionCount = 0;
    g_sessionStart = GetTickCount64();
    g_sessionItems.clear();
    LOG_INFO("[Captures] Session started: Captures\\" + ApexUtil::ToUtf8(g_session.filename().wstring()));
}

void EndSession() {
    std::filesystem::path s;
    std::vector<std::string> items;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        s = g_session;
        items = g_sessionItems;
    }
    if (s.empty() || Saving()) return;
    const bool logOk = CopyIfThere(Dir() / L"ApexRadiance_LOG.txt", s / L"ApexRadiance_LOG.txt");
    const bool configOk = CopyIfThere(Dir() / L"ApexRadiance.toml", s / L"ApexRadiance.toml");
    const bool crashOk = CopyIfThere(Dir() / L"ApexRadiance_Crash.txt", s / L"ApexRadiance_Crash.txt");
    std::ostringstream about;
    {
        about << std::format("{} {} - a capture session with {} captures, on {}.\n\n", APEX_PRODUCT_NAME, APEX_VERSION_STRING, items.size(), GetGameVersionName());
        for (const std::string& i : items) about << "- " << i << "\n";
        about << "\nEach capture is in its own folder here, with the log as it was at that moment; the log and settings at the end of the session are "
                 "beside them.\n\nTo report a problem: right-click this folder > Send to > Compressed (zipped) folder, then attach the .zip to your post "
                 "in the Bugs tab of Apex Radiance on Nexus Mods, or to an issue on GitHub (github.com/loinyx/Sims3-ApexRadiance/issues). Say in a few "
                 "words what you saw and what you did just before.\n";
    }
    WriteText(s / L"About this session.txt", about.str());
    const auto existingNote = ReadDescription(ApexUtil::ToUtf8(s.filename().wstring()));
    if (!existingNote.Complete()) {
        WriteText(s / L"User notes.txt", DescriptionText(ApexUtil::ToUtf8(s.filename().wstring()), about.str()));
    }
    {
        std::lock_guard<std::mutex> lk(g_lock);
        if (g_retryFolder != s) { g_retryDescription.clear(); g_retryTitle.clear(); }
        g_retryFolder = s;
        g_retryWhat.clear(); // retrying this receipt finishes the still-open collection
        g_retryKind = CaptureKind::Generic;
        g_copyFailed = !logOk || !configOk || !crashOk;
        g_shotFailed = false;
        g_result = {g_result.serial + 1, ApexUtil::ToUtf8(s.filename().wstring()), false, false};
        UpdateResultLocked();
        if (!g_result.failed) g_session.clear();
    }
    const std::string name = ApexUtil::ToUtf8(s.filename().wstring());
    LOG_INFO(std::format("[Captures] Session ended: Captures\\{} ({} captures)", name, items.size()));
    const bool failed = LastSave().failed;
    Notify(failed ? I18n::Tr("Some files could not be saved. Open Report a problem to retry") : I18n::Trf("Session saved in Captures \xE2\x80\xBA {}", name), 6,
           failed ? NoteKind::Warning : NoteKind::Success);
}

bool SessionActive() {
    std::lock_guard<std::mutex> lk(g_lock);
    return !g_session.empty();
}
int SessionCaptures() {
    std::lock_guard<std::mutex> lk(g_lock);
    return g_sessionCount;
}
std::string SessionFolder() {
    std::lock_guard<std::mutex> lk(g_lock);
    return g_session.empty() ? std::string() : ApexUtil::ToUtf8(g_session.filename().wstring());
}
std::vector<std::string> SessionItems() {
    std::lock_guard<std::mutex> lk(g_lock);
    std::vector<std::string> names;
    for (const std::string& i : g_sessionItems) names.push_back(i.substr(0, i.find(": "))); // the folder name only
    return names;
}
int SessionSeconds() {
    std::lock_guard<std::mutex> lk(g_lock);
    return g_session.empty() ? 0 : static_cast<int>((GetTickCount64() - g_sessionStart) / 1000);
}

bool WriteText(const std::filesystem::path& file, const std::string& text) {
    std::ofstream out(file, std::ios::binary | std::ios::trunc);
    out.write(text.data(), static_cast<std::streamsize>(text.size()));
    out.close();
    const bool ok = !out.fail();
    std::lock_guard<std::mutex> lk(g_lock);
    if (ok) g_failedText.erase(file);
    else g_failedText[file] = text;
    return ok;
}

void SetDescription(const std::string& text) {
    std::lock_guard<std::mutex> lk(g_lock);
    g_description = text;
}
SaveResult LastSave() { std::lock_guard<std::mutex> lk(g_lock); return g_result; }
bool Saving() { std::lock_guard<std::mutex> lk(g_lock); return !g_shotJobs.empty() || !g_dumpJobs.empty(); }

std::string SavedDescription() { std::lock_guard<std::mutex> lk(g_lock); return g_retryDescription; }
std::string SavedDescriptionTitle() { std::lock_guard<std::mutex> lk(g_lock); return g_retryTitle; }
bool SaveDescription(const std::string& text) { return SaveDescription("", text); }
bool SaveDescription(const std::string& title, const std::string& text) {
    std::filesystem::path folder;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        if (!g_result.serial || g_result.failed || g_result.saving || !g_shotJobs.empty() ||
            text.find_first_not_of(" \t\r\n") == std::string::npos) return false;
        folder = g_retryFolder;
        g_retryDescription = text;
        g_retryTitle = title;
    }
    const bool ok = WriteText(folder / L"User notes.txt", DescriptionText(title, text));
    { std::lock_guard<std::mutex> lk(g_lock); UpdateResultLocked(); }
    Notify(I18n::Tr(ok ? "Description saved with the capture" : "Some files could not be saved. Open Report a problem to retry"), 5,
           ok ? NoteKind::Success : NoteKind::Warning);
    return ok;
}

Description ReadDescription(const std::string& folder) {
    Description note;
    if (!ChildName(folder)) return note;
    const auto path = Root() / std::filesystem::path(ApexUtil::ToWide(folder));
    if (!PlainDirectory(path)) return note;
    const auto file = path / L"User notes.txt";
    const DWORD attrs = GetFileAttributesW(file.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES || (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) return note;
    std::ifstream in(file, std::ios::binary);
    char bytes[4096]{};
    in.read(bytes, sizeof(bytes));
    note.text.assign(bytes, static_cast<size_t>(in.gcount()));
    if (note.text.rfind("Title: ", 0) == 0) {
        const auto end = note.text.find("\n\n");
        if (end != std::string::npos) {
            note.title = note.text.substr(7, end - 7);
            note.text.erase(0, end + 2);
        }
    }
    return note;
}
bool SaveFolderDescription(const std::string& folder, const std::string& title, const std::string& text) {
    if (!ChildName(folder) || text.find_first_not_of(" \t\r\n") == std::string::npos || Saving()) return false;
    const auto path = Root() / std::filesystem::path(ApexUtil::ToWide(folder));
    if (!PlainDirectory(path)) return false;
    const auto file = path / L"User notes.txt";
    for (const auto& candidate : {file, std::filesystem::path(file.wstring() + L".tmp")}) {
        const DWORD attrs = GetFileAttributesW(candidate.c_str());
        if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT)) return false;
    }
    return ApexUtil::WriteFileAtomic(file.wstring(), DescriptionText(title, text));
}

void Finish(const std::filesystem::path& folder, const std::string& what, CaptureKind kind) {
    std::string description, title;
    bool retry = false, inSession = false, texturesPending = false;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        retry = folder == g_retryFolder && g_result.failed;
        if (retry) kind = g_retryKind;
        // Only the latest receipt offers Retry. Do not accumulate large failed recordings in the 32-bit game.
        if (!retry) {
            for (auto it = g_failedText.begin(); it != g_failedText.end();) {
                if (it->first.parent_path() != folder) it = g_failedText.erase(it); else ++it;
            }
        }
        description = retry ? g_retryDescription : g_description;
        title = retry ? g_retryTitle : std::string();
        // Optional player notes: fallback describes recorded evidence, never an inferred problem.
        if (title.empty()) title = ApexUtil::ToUtf8(folder.filename().wstring());
        if (description.find_first_not_of(" \t\r\n") == std::string::npos) {
            description = std::format("Diagnostic capture: {}.\nGame: {}.\nApex Radiance: {}.\nFolder: {}.\nAvailable logs, settings and capture details are stored alongside this note.",
                what, GetGameVersionName(), APEX_VERSION_STRING, ApexUtil::ToUtf8(folder.filename().wstring()));
        }
        g_retryFolder = folder;
        g_retryWhat = what;
        g_retryKind = kind;
        g_retryDescription = description;
        g_retryTitle = title;
        g_shotFailed = false;
        if (!retry) g_dumpFailed = false;
        texturesPending = g_dumpJobs.contains(folder);
        g_notifyCompletion = false;
        g_result = {g_result.serial + 1, ApexUtil::ToUtf8(folder.filename().wstring()), false, false};
        if (folder.parent_path().parent_path() == Root()) g_result.folder = ApexUtil::ToUtf8(folder.parent_path().filename().wstring());
    }
    const bool logOk = CopyIfThere(Dir() / L"ApexRadiance_LOG.txt", folder / L"ApexRadiance_LOG.txt");
    const bool configOk = CopyIfThere(Dir() / L"ApexRadiance.toml", folder / L"ApexRadiance.toml");
    const bool crashOk = CopyIfThere(Dir() / L"ApexRadiance_Crash.txt", folder / L"ApexRadiance_Crash.txt");
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::ostringstream about;
    about << std::format("{} {} - {}\n", APEX_PRODUCT_NAME, APEX_VERSION_STRING, what);
    about << std::format("Taken {:04}-{:02}-{:02} {:02}:{:02}:{:02} on {}.\n\n", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, GetGameVersionName());
    about << "This folder contains the capture, available mod log, settings and crash report.\n"
             "To report a problem: compress this folder as ZIP and attach it to a Nexus Mods bug report or a GitHub issue "
             "(github.com/loinyx/Sims3-ApexRadiance/issues). Describe what happened and how to reproduce it. "
             "Nothing is uploaded automatically.\n";
    if (texturesPending)
        about << "\nThe texture images (T<n>_*.png, lossless) are still being written in the background when this note is saved; wait for the "
                 "\"saved\" note in the game before zipping the folder.\n";
    WriteText(folder / L"About this capture.txt", about.str());
    if (!description.empty() || !title.empty()) WriteText(folder / L"User notes.txt", DescriptionText(title, description));
    // A failed text write is recoverable without measuring or recording the problem again.
    if (!retry) QueueShot(folder);
    {
        std::lock_guard<std::mutex> lk(g_lock);
        g_copyFailed = !logOk || !configOk || !crashOk;
        if (!g_session.empty() && folder.parent_path() == g_session) {
            inSession = true;
            const std::string item = ApexUtil::ToUtf8(folder.filename().wstring()) + ": " + what;
            if (std::find(g_sessionItems.begin(), g_sessionItems.end(), item) == g_sessionItems.end()) {
                g_sessionCount++;
                g_sessionItems.push_back(item);
            }
        }
        UpdateResultLocked();
    }
    LOG_INFO(std::format("[Captures] {}: {} ({})", LastSave().failed ? "Save incomplete" : "Capture written", ApexUtil::ToUtf8(folder.wstring()), what));
    const SaveResult result = LastSave();
    const bool saving = Saving();
    const char* message = "Capture saved. Open Report a problem to find your files";
    if (result.failed) message = "Some files could not be saved. Open Report a problem to retry";
    else if (saving) message = "Saving the capture and screenshot...";
    else if (kind == CaptureKind::Recording) message = "Recording saved. Open Report a problem to find your files";
    else if (kind == CaptureKind::LightCapture) message = "Light capture saved. Open Report a problem to find your files";
    else if (kind == CaptureKind::LightingSnapshot) message = "Lighting snapshot saved. Open Report a problem to find your files";
    Notify(I18n::Tr(message), 6, result.failed ? NoteKind::Warning : saving ? NoteKind::Saving : NoteKind::Success);
    (void)inSession;
    PruneLater(); // the new capture itself is protected
}

void RetrySave() {
    std::map<std::filesystem::path, std::string> pending;
    std::filesystem::path folder;
    std::string what;
    bool shot = false;
    {
        std::lock_guard<std::mutex> lk(g_lock);
        if (!g_result.failed || !g_shotJobs.empty()) return;
        folder = g_retryFolder;
        what = g_retryWhat;
        shot = g_shotFailed;
        for (const auto& [file, text] : g_failedText) if (file.parent_path() == folder) pending.emplace(file, text);
    }
    for (const auto& [file, text] : pending) WriteText(file, text);
    if (what.empty()) {
        if (SessionActive()) EndSession();
        else {
            { std::lock_guard<std::mutex> lk(g_lock); UpdateResultLocked(); ++g_result.serial; }
            const bool failed = LastSave().failed;
            Notify(I18n::Tr(failed ? "Some files could not be saved. Open Report a problem to retry" : "Description saved with the capture"), 5,
                   failed ? NoteKind::Warning : NoteKind::Success);
        }
        return;
    }
    Finish(folder, what);
    if (shot) {
        QueueShot(folder); // current frame, not a promise to reproduce the earlier image
        Notify(I18n::Tr("Saving the capture and screenshot..."), 6, NoteKind::Saving);
    }
    { std::lock_guard<std::mutex> lk(g_lock); UpdateResultLocked(); }
}

void SetScreenshots(bool on) { g_shotsOn = on; }
bool Screenshots() { return g_shotsOn.load(); }
bool ScreenshotPending() {
    return std::any_of(g_shots.begin(), g_shots.end(), [](const ShotJob& job) { return job.report; });
}

void ObserveGameUiKey(WPARAM vk, bool repeat) {
    if (vk == VK_F10 && !repeat) {
        g_gameUiHidden.store(!g_gameUiHidden.load());
#ifdef APEX_F10_STUDY
        F10Study::Request(g_gameUiHidden.load());
#endif
    }
}

void OnWorldSessionChanged() {
    if (!g_playerPhoto.active) g_gameUiHidden.store(false); // a loaded world starts with the game's UI shown
}

bool RequestPlayerScreenshot(bool hideGameUi) {
    if (g_playerPhoto.active) return false;
    std::error_code ec;
    const bool toApex = ApexConfig::GetUi().screenshotToApexFolder;
    const std::wstring& gameDir = toApex ? ApexPaths::ApexDirectory() : ApexPaths::GameDocumentsDirectory();
    if (gameDir.empty()) {
        Notify(I18n::Tr("Could not create the screenshots folder"), 5, NoteKind::Warning);
        return false;
    }
    const std::filesystem::path folder = std::filesystem::path(gameDir) / L"Screenshots";
    std::filesystem::create_directories(folder, ec);
    if (ec) {
        Notify(I18n::Tr("Could not create the screenshots folder"), 5, NoteKind::Warning);
        return false;
    }
    SYSTEMTIME t{};
    GetLocalTime(&t);
    const std::string base = std::format("Screenshot_{:04}-{:02}-{:02}_{:02}-{:02}-{:02}_{:03}",
                                         t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    std::filesystem::path file = folder / (base + ".png");
    for (unsigned suffix = 2; std::filesystem::exists(file, ec) && !ec; ++suffix)
        file = folder / (base + std::format(" ({})", suffix) + ".png");
    if (ec) {
        Notify(I18n::Tr("Could not create the screenshots folder"), 5, NoteKind::Warning);
        return false;
    }
    if (!QueuePlayerPhoto(file, hideGameUi)) {
        Notify(I18n::Tr("Could not hide the game interface; screenshot was not taken"), 5, NoteKind::Warning);
        return false;
    }
    LOG_INFO("[Captures] Filtered screenshot requested: " + ApexUtil::ToUtf8(file.wstring()));
    return true;
}

void SaveReport() { Finish(NewFolder("Report"), "a report: the log and the settings"); }

std::string RecentCrash() {
    WIN32_FILE_ATTRIBUTE_DATA a{};
    const std::filesystem::path p = Dir() / L"ApexRadiance_Crash.txt";
    if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &a)) return "";
    ULARGE_INTEGER w{}, now{};
    w.LowPart = a.ftLastWriteTime.dwLowDateTime;
    w.HighPart = a.ftLastWriteTime.dwHighDateTime;
    FILETIME nf;
    GetSystemTimeAsFileTime(&nf);
    now.LowPart = nf.dwLowDateTime;
    now.HighPart = nf.dwHighDateTime;
    if (now.QuadPart < w.QuadPart || now.QuadPart - w.QuadPart > 7ull * 24 * 3600 * 10000000ull) return "";
    FILETIME local;
    SYSTEMTIME t;
    if (!FileTimeToLocalFileTime(&a.ftLastWriteTime, &local) || !FileTimeToSystemTime(&local, &t)) return "";
    return std::format("{:04}-{:02}-{:02} {:02}:{:02}", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute);
}

void Notify(const std::string& text, int seconds, NoteKind kind) {
    std::lock_guard<std::mutex> lk(g_lock);
    g_note = text;
    g_noteKind = kind;
    g_noteUntil = GetTickCount64() + static_cast<unsigned long long>(seconds) * 1000ull;
}

Note CurrentNote() {
    std::lock_guard<std::mutex> lk(g_lock);
    if (g_notifyCompletion && !g_result.saving) {
        g_notifyCompletion = false;
        const bool failed = g_result.failed;
        const char* message = "Capture saved. Open Report a problem to find your files";
        if (failed) message = "Some files could not be saved. Open Report a problem to retry";
        else if (g_retryKind == CaptureKind::Recording) message = "Recording saved. Open Report a problem to find your files";
        else if (g_retryKind == CaptureKind::LightCapture) message = "Light capture saved. Open Report a problem to find your files";
        else if (g_retryKind == CaptureKind::LightingSnapshot) message = "Lighting snapshot saved. Open Report a problem to find your files";
        g_note = I18n::Tr(message);
        g_noteKind = failed ? NoteKind::Warning : NoteKind::Success;
        g_noteUntil = GetTickCount64() + 6000;
    }
    Note n;
    if (!g_note.empty() && GetTickCount64() < g_noteUntil) {
        n.text = g_note;
        n.visible = true;
        n.kind = g_noteKind;
    }
    return n;
}

Summary Scan() {
    namespace fs = std::filesystem;
    Summary s;
    std::error_code ec;
    fs::file_time_type newest{};
    for (auto it = fs::directory_iterator(Root(), fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::directory_iterator(); it.increment(ec)) {
        const auto& e = *it;
        if (!PlainDirectory(e.path()) || e.path().filename() == L".Removed") continue;
        s.count++;
        const auto wt = e.last_write_time(ec);
        if (s.newest.empty() || wt > newest) {
            newest = wt;
            s.newest = ApexUtil::ToUtf8(e.path().filename().wstring());
        }
        s.bytes += FolderSize(e.path());
    }
    return s;
}

void OpenFolder() {
    std::error_code ec;
    std::filesystem::create_directories(Root(), ec);
    ShowInExplorer(Root());
}

int DeleteAll() {
    namespace fs = std::filesystem;
    std::error_code ec;
    int n = 0;
    const std::string open = SessionFolder();
    // error_code overloads only (07/10, players' Runtime Error): the range-for's ++ throws on a folder that cannot be read
    for (auto it = fs::directory_iterator(Root(), fs::directory_options::skip_permission_denied, ec); !ec && it != fs::directory_iterator(); it.increment(ec)) {
        const auto& e = *it;
        std::error_code removeError;
        if (PlainDirectory(e.path()) && e.path().filename() != L".Removed" && ApexUtil::ToUtf8(e.path().filename().wstring()) != open &&
            fs::remove_all(e.path(), removeError) != static_cast<std::uintmax_t>(-1))
            n++;
    }
    LOG_INFO(std::format("[Captures] {} capture folders deleted from the menu", n));
    return n;
}

std::vector<Entry> List() {
    namespace fs = std::filesystem;
    std::vector<Entry> out;
    std::error_code ec;
    for (auto it = fs::directory_iterator(Root(), fs::directory_options::skip_permission_denied, ec);
         !ec && it != fs::directory_iterator(); it.increment(ec)) {
        const auto& e = *it;
        if (!PlainDirectory(e.path()) || e.path().filename() == L".Removed") continue;
        Entry x;
        x.folder = ApexUtil::ToUtf8(e.path().filename().wstring());
        // "YYYY-MM-DD HH-MM-SS kind": names made by NewFolder; other folders are listed by name only
        if (x.folder.size() > 20 && x.folder[4] == '-' && x.folder[10] == ' ' && x.folder[13] == '-') {
            x.date = x.folder.substr(0, 10);
            x.time = x.folder.substr(11, 2) + ":" + x.folder.substr(14, 2) + ":" + x.folder.substr(17, 2);
            x.kind = x.folder.substr(20);
        } else {
            x.kind = x.folder;
        }
        x.bytes = FolderSize(e.path());
        const auto note = ReadDescription(x.folder);
        x.described = note.Complete();
        x.title = note.title;
        x.description = note.text;
        if (x.kind.rfind("Session", 0) == 0)
            for (auto child = fs::directory_iterator(e.path(), fs::directory_options::skip_permission_denied, ec);
                 !ec && child != fs::directory_iterator(); child.increment(ec))
                if (PlainDirectory(child->path())) x.items++;
        out.push_back(std::move(x));
    }
    std::sort(out.begin(), out.end(), [](const Entry& a, const Entry& b) { return a.folder > b.folder; }); // the date first in the name: newest first
    return out;
}

void Open(const std::string& folder) {
    if (!ChildName(folder)) return;
    const std::filesystem::path p = Root() / std::filesystem::path(ApexUtil::ToWide(folder));
    if (PlainDirectory(p)) ShowInExplorer(p);
}

bool Delete(const std::string& folder) {
    namespace fs = std::filesystem;
    if (!ChildName(folder) || Saving()) return false; // only a direct child of Captures\.
    if (folder == SessionFolder()) return false; // the open session: end it first
    std::error_code ec;
    const fs::path p = Root() / fs::path(ApexUtil::ToWide(folder));
    if (!PlainDirectory(p)) return false;
    const bool ok = fs::remove_all(p, ec) != static_cast<std::uintmax_t>(-1) && !ec;
    LOG_INFO(std::format("[Captures] Deleted from the menu: Captures\\{}{}", folder, ok ? "" : " (failed: " + ec.message() + ")"));
    return ok;
}

namespace {
bool MoveToRemoved(const std::string& folder) {
    namespace fs = std::filesystem;
    if (!ChildName(folder) || folder == SessionFolder() || Saving()) return false;
    const fs::path from = Root() / fs::path(ApexUtil::ToWide(folder)), area = Root() / L".Removed";
    if (!PlainDirectory(from)) return false;
    std::error_code ec;
    fs::create_directories(area, ec);
    if (ec || !PlainDirectory(area)) return false;
    fs::path to = area / fs::path(ApexUtil::ToWide(folder));
    for (int n = 2; fs::exists(to, ec) && n < 1000; ++n) to = area / fs::path(std::format("{} ({})", folder, n));
    if (ec || fs::exists(to, ec)) return false;
    fs::rename(from, to, ec); // same root, no recursive move and no overwrite
    if (ec) return false;
    g_removed.emplace_back(to, from);
    {
        std::lock_guard<std::mutex> lk(g_lock);
        if (g_result.folder == folder) { g_result = {}; g_notifyCompletion = false; }
    }
    return true;
}
}
bool Remove(const std::string& folder) {
    if (!ChildName(folder) || folder == SessionFolder() || Saving()) return false;
    const auto previous = g_removed;
    g_removed.clear();
    if (MoveToRemoved(folder)) return true;
    g_removed = previous;
    return false;
}
int RemoveAll() {
    if (Saving()) return 0;
    const auto list = List();
    const auto previous = g_removed;
    g_removed.clear();
    for (const auto& entry : list) MoveToRemoved(entry.folder);
    const int count = static_cast<int>(g_removed.size());
    if (!count) g_removed = previous;
    return count;
}
bool CanUndoRemoval() { return !g_removed.empty(); }
int UndoRemoval() {
    if (Saving()) return 0;
    int restored = 0;
    std::vector<std::pair<std::filesystem::path, std::filesystem::path>> pending;
    for (const auto& item : g_removed) {
        std::error_code ec;
        if (!PlainDirectory(item.first) || !PlainDirectory(item.first.parent_path()) || std::filesystem::exists(item.second, ec) || ec) {
            pending.push_back(item);
            continue;
        }
        std::filesystem::rename(item.first, item.second, ec);
        if (ec) pending.push_back(item); else ++restored;
    }
    g_removed = std::move(pending);
    return restored;
}
std::vector<std::string> Files(const std::string& folder) {
    std::vector<std::string> files;
    if (!ChildName(folder)) return files;
    const auto path = Root() / std::filesystem::path(ApexUtil::ToWide(folder));
    if (!PlainDirectory(path)) return files;
    std::error_code ec;
    auto it = std::filesystem::recursive_directory_iterator(path, std::filesystem::directory_options::skip_permission_denied, ec);
    for (; !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (it->is_directory(ec) && !PlainDirectory(it->path())) it.disable_recursion_pending();
        if (it->is_regular_file(ec)) files.push_back(ApexUtil::ToUtf8(it->path().lexically_relative(path).wstring()));
    }
    std::sort(files.begin(), files.end());
    return files;
}

} // namespace Captures
