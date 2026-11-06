#ifndef APEX_NO_DEV_TOOLS // (a build without the developer tools: ApexFlavorDefines=APEX_NO_DEV_TOOLS)
// The video of a recording (see capture_video.h).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "capture_video.h"
#include "apex_log.h"
#include "apex_util.h"
#include "hook_guard.h"
#include <windows.h>
#include <wincodec.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <format>
#include <mutex>
#include <new>
#include <vector>

namespace {

constexpr int kSlots = 3, kEveryFrames = 2, kMaxHeight = 1080;
constexpr float kQuality = 0.75f; // WIC's ImageQuality (0..1)

enum Stage : int { kFree, kCopied, kReading, kEncoding };
struct Slot {
    IDirect3DSurface9* rt = nullptr;
    IDirect3DSurface9* sys = nullptr;
    IDirect3DQuery9* done = nullptr;
    std::atomic<int> stage{kFree};
    std::vector<BYTE> pixels; // B G R X rows of g_w pixels (the worker's while kEncoding)
};
Slot g_slots[kSlots];
int g_w = 0, g_h = 0, g_next = 0, g_oldest = 0;
bool g_on = false;           // frames are being taken (render thread)
FILE* g_file = nullptr;      // the worker's until End
std::filesystem::path g_temp;
long g_turn = 0, g_grabbed = 0, g_dropped = 0;
DWORD g_firstTick = 0, g_lastTick = 0;
std::mutex g_mx;
std::condition_variable g_cv;
bool g_closing = false, g_workerDone = true; // under g_mx
std::atomic<long> g_written{0};
std::atomic<bool> g_writeFailed{false};

// One frame as a JPEG at the start of `mem`: its size, 0 when it failed. rows = B G R, top-down
size_t Encode(IWICImagingFactory* factory, IStream* mem, BYTE* rows) {
    const LARGE_INTEGER zero{};
    ULARGE_INTEGER size{};
    IWICBitmapEncoder* enc = nullptr;
    IWICBitmapFrameEncode* frame = nullptr;
    IPropertyBag2* bag = nullptr;
    WICPixelFormatGUID fmt = GUID_WICPixelFormat24bppBGR;
    bool ok = SUCCEEDED(mem->Seek(zero, STREAM_SEEK_SET, nullptr)) && SUCCEEDED(mem->SetSize(ULARGE_INTEGER{})) &&
              SUCCEEDED(factory->CreateEncoder(GUID_ContainerFormatJpeg, nullptr, &enc)) && SUCCEEDED(enc->Initialize(mem, WICBitmapEncoderNoCache)) &&
              SUCCEEDED(enc->CreateNewFrame(&frame, &bag));
    if (ok) {
        PROPBAG2 name{};
        name.pstrName = const_cast<LPOLESTR>(L"ImageQuality");
        VARIANT value{};
        value.vt = VT_R4;
        value.fltVal = kQuality;
        ok = SUCCEEDED(bag->Write(1, &name, &value)) && SUCCEEDED(frame->Initialize(bag)) && SUCCEEDED(frame->SetSize(g_w, g_h)) && SUCCEEDED(frame->SetPixelFormat(&fmt)) &&
             IsEqualGUID(fmt, GUID_WICPixelFormat24bppBGR) && SUCCEEDED(frame->WritePixels(g_h, g_w * 3, g_w * 3 * g_h, rows)) && SUCCEEDED(frame->Commit()) &&
             SUCCEEDED(enc->Commit()) && SUCCEEDED(mem->Seek(zero, STREAM_SEEK_CUR, &size));
    }
    if (bag) bag->Release();
    if (frame) frame->Release();
    if (enc) enc->Release();
    return ok ? static_cast<size_t>(size.QuadPart) : 0;
}

void WriteFrame(IWICImagingFactory* factory, IStream* mem, Slot& s) {
    const size_t count = static_cast<size_t>(g_w) * g_h;
    for (size_t i = 0; i < count; i++) std::memmove(&s.pixels[3 * i], &s.pixels[4 * i], 3); // (B G R X -> B G R, in place)
    const size_t size = Encode(factory, mem, s.pixels.data());
    HGLOBAL global = nullptr;
    const void* bytes = size && SUCCEEDED(GetHGlobalFromStream(mem, &global)) ? GlobalLock(global) : nullptr;
    const bool ok = bytes && std::fwrite(bytes, 1, size, g_file) == size;
    if (bytes) GlobalUnlock(global);
    if (ok) g_written.fetch_add(1);
    else if (!g_writeFailed.exchange(true)) LOG_WARNING("[CaptureVideo] A frame could not be encoded or written: the video ends there");
}

// The slots in the ring's order, until End asks to close and nothing is left to encode
void Worker() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL); // the game first: a late frame is only dropped and counted
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    IWICImagingFactory* factory = nullptr;
    IStream* mem = nullptr;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory))) || FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &mem)))
        g_writeFailed = true;
    try { // (End waits for this thread)
        for (int i = 0;; i = (i + 1) % kSlots) {
            Slot& s = g_slots[i];
            {
                std::unique_lock<std::mutex> lk(g_mx);
                g_cv.wait(lk, [&s] { return s.stage.load() == kEncoding || g_closing; });
                if (s.stage.load() != kEncoding) break;
            }
            if (!g_writeFailed) WriteFrame(factory, mem, s);
            s.stage = kFree;
        }
    } catch (...) {
        g_writeFailed = true;
    }
    if (mem) mem->Release();
    if (factory) factory->Release();
    if (SUCCEEDED(com)) CoUninitialize();
    std::lock_guard<std::mutex> lk(g_mx);
    g_workerDone = true;
    g_cv.notify_all();
}

void Off(const char* why) {
    LOG_WARNING(std::string("[CaptureVideo] No video for this recording: ") + why);
    CaptureVideo::Release();
}

} // namespace

namespace CaptureVideo {

void Begin(IDirect3DDevice9* dev, const D3DSURFACE_DESC& bb, const std::filesystem::path& temp) {
    End({});
    std::error_code ec;
    std::filesystem::remove(temp, ec); // the leftover of a recording the game did not outlive
    // shortcut: no video from a multisampled back buffer (StretchRect cannot scale it), add a full-size resolve if game AA is used
    if (bb.MultiSampleType != D3DMULTISAMPLE_NONE) return Off("the back buffer is multisampled");
    g_h = std::min<int>(static_cast<int>(bb.Height), kMaxHeight) & ~1;
    g_w = static_cast<int>(static_cast<long long>(bb.Width) * g_h / std::max<UINT>(bb.Height, 1)) & ~1;
    if (g_w <= 0 || g_h <= 0) return Off("the back buffer has no size");
    try {
        for (Slot& s : g_slots) {
            s.pixels.resize(static_cast<size_t>(g_w) * g_h * 4);
            if (FAILED(dev->CreateRenderTarget(g_w, g_h, bb.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &s.rt, nullptr)) ||
                FAILED(dev->CreateOffscreenPlainSurface(g_w, g_h, bb.Format, D3DPOOL_SYSTEMMEM, &s.sys, nullptr)) || FAILED(dev->CreateQuery(D3DQUERYTYPE_EVENT, &s.done)))
                return Off("its surfaces could not be created");
        }
    } catch (const std::bad_alloc&) {
        return Off("no memory for its frames");
    }
    if (_wfopen_s(&g_file, temp.c_str(), L"wb") || !g_file) {
        g_file = nullptr;
        return Off("its file could not be created");
    }
    g_temp = temp;
    g_next = g_oldest = 0;
    g_turn = g_grabbed = g_dropped = 0;
    g_firstTick = g_lastTick = 0;
    g_written = 0;
    g_writeFailed = false;
    g_closing = false;
    g_workerDone = false;
    if (!HookGuard::StartDetached("CaptureVideo: frame encoder", [] { Worker(); })) {
        g_workerDone = true;
        End({});
        return Off("its worker could not start");
    }
    g_on = true;
}

int Grab(IDirect3DDevice9* dev) {
    if (!g_on || ++g_turn % kEveryFrames) return -1;
    Slot& s = g_slots[g_next];
    if (s.stage.load() != kFree) {
        g_dropped++;
        return -1;
    }
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return -1;
    const bool ok = SUCCEEDED(dev->StretchRect(bb, nullptr, s.rt, nullptr, D3DTEXF_LINEAR));
    bb->Release();
    if (!ok) {
        Off("the frame could not be scaled");
        return -1;
    }
    s.done->Issue(D3DISSUE_END);
    s.stage = kCopied;
    g_next = (g_next + 1) % kSlots;
    g_lastTick = GetTickCount();
    if (!g_firstTick) g_firstTick = g_lastTick;
    return g_grabbed++;
}

void Pump(IDirect3DDevice9* dev) {
    if (!g_on) return;
    while (g_slots[g_oldest].stage.load() == kReading) {
        Slot& s = g_slots[g_oldest];
        if (s.done->GetData(nullptr, 0, 0) != S_OK) break; // not done yet (no flush asked)
        D3DLOCKED_RECT lr{};
        if (FAILED(s.sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) return Off("a frame could not be read");
        for (int y = 0; y < g_h; y++) std::memcpy(&s.pixels[static_cast<size_t>(y) * g_w * 4], static_cast<const BYTE*>(lr.pBits) + static_cast<size_t>(y) * lr.Pitch, static_cast<size_t>(g_w) * 4);
        s.sys->UnlockRect();
        {
            std::lock_guard<std::mutex> lk(g_mx);
            s.stage = kEncoding;
        }
        g_cv.notify_all();
        g_oldest = (g_oldest + 1) % kSlots;
    }
    for (int k = 0; k < kSlots; k++) {
        Slot& s = g_slots[(g_oldest + k) % kSlots];
        const int stage = s.stage.load();
        if (stage == kReading) continue;
        if (stage != kCopied || s.done->GetData(nullptr, 0, 0) != S_OK) break;
        if (FAILED(dev->GetRenderTargetData(s.rt, s.sys))) return Off("a frame could not be copied");
        s.done->Issue(D3DISSUE_END);
        s.stage = kReading;
    }
}

std::string End(const std::filesystem::path& to) {
    Release();
    const bool wrote = g_file;
    if (wrote) {
        {
            std::unique_lock<std::mutex> lk(g_mx);
            g_closing = true;
            g_cv.notify_all();
            g_cv.wait(lk, [] { return g_workerDone; });
        }
        std::fclose(g_file);
        g_file = nullptr;
    }
    for (Slot& s : g_slots) std::vector<BYTE>().swap(s.pixels), s.stage = kFree;
    if (!wrote) return {};
    const long frames = g_written.load();
    std::error_code ec;
    if (to.empty() || !frames) {
        std::filesystem::remove(g_temp, ec);
        return {};
    }
    std::filesystem::rename(g_temp, to, ec);
    if (ec) std::filesystem::copy_file(g_temp, to, std::filesystem::copy_options::overwrite_existing, ec), std::filesystem::remove(g_temp, ec);
    const double seconds = (g_lastTick - g_firstTick) / 1000.0;
    return std::format("{}: {}x{}, {} frames in {:.1f} s ({:.2f} fps), every {} frames, {} dropped{}", ApexUtil::ToUtf8(to.filename().wstring()), g_w, g_h, frames, seconds,
                       seconds > 0.0 ? frames / seconds : 0.0, kEveryFrames, g_dropped, g_writeFailed ? ", ended early by a failed frame" : "");
}

void Release() {
    g_on = false;
    for (Slot& s : g_slots) {
        if (s.done) s.done->Release();
        if (s.rt) s.rt->Release();
        if (s.sys) s.sys->Release();
        s.done = nullptr, s.rt = nullptr, s.sys = nullptr;
        int copied = kCopied, reading = kReading; // (the frames not read yet are lost; the worker keeps the one it has)
        if (!s.stage.compare_exchange_strong(copied, kFree)) s.stage.compare_exchange_strong(reading, kFree);
    }
}

} // namespace CaptureVideo
#endif // APEX_NO_DEV_TOOLS
