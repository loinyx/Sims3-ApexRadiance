// The player's MP4 recorder (see video_mp4.h).
#include "video_mp4.h"
#include "apex_log.h"
#include "apex_util.h"
#include "hook_guard.h"
#include "captures.h"
#include "mkv_remux.h"
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <mferror.h>
#include <codecapi.h>
#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <mmdeviceapi.h>
#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <format>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#pragma comment(lib, "mfplat.lib")
#pragma comment(lib, "mfreadwrite.lib")
#pragma comment(lib, "mfuuid.lib")
#pragma comment(lib, "mmdevapi.lib")
#pragma comment(lib, "ole32.lib")

namespace VideoMp4 {
namespace {

template <class T> void SafeRelease(T*& p) { if (p) { p->Release(); p = nullptr; } }

constexpr int kSlots = 3;      // GPU copies in flight
constexpr size_t kQueue = 2;   // frames waiting for the encoder (32-bit game: memory stays small)
constexpr UINT kRate = 48000;  // game sound: 48 kHz stereo 16-bit
constexpr LONGLONG kHns = 10000000;

LONGLONG Qpc() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t.QuadPart; }
LONGLONG QpcFreq() { static const LONGLONG f = [] { LARGE_INTEGER t; QueryPerformanceFrequency(&t); return t.QuadPart; }(); return f; }

// ---- the session: owned by the render thread while recording, then by the worker until the file is done ----
struct Frame { std::vector<BYTE> px; LONGLONG t = 0; };
struct Session {
    std::filesystem::path file;
    Options o;
    UINT w = 0, h = 0;
    LONGLONG qpc0 = 0;
    std::mutex m;
    std::condition_variable cv;
    std::deque<Frame> q;
    std::vector<std::vector<BYTE>> pool;
    bool stopping = false;
    std::atomic<bool> failed{false};
    std::atomic<unsigned> written{0}, dropped{0};
    std::atomic<unsigned long long> bytes{0};
    std::string error, notes;
    IMFSinkWriter* writer = nullptr;
    std::mutex writerLock;
    DWORD vs = 0, as = 0;
    bool hasAudio = false;
    IAudioClient* audio = nullptr;
    IAudioCaptureClient* capture = nullptr;
    HANDLE audioEvent = nullptr;
    std::atomic<bool> audioStop{false};
    LONGLONG lastVideoT = -1;
};

struct Slot { IDirect3DSurface9* rt = nullptr; IDirect3DSurface9* sys = nullptr; IDirect3DQuery9* q = nullptr; int state = 0; LONGLONG t = 0; };
Slot g_slots[kSlots];
std::shared_ptr<Session> g_s;       // the recording (render thread; set under g_sLock, read by StopAndWait)
std::mutex g_sLock;
std::atomic<bool> g_active{false};
std::atomic<int> g_finishing{0}; // clips still being saved (a new clip may start while the last one is)
LONGLONG g_nextDue = 0, g_period = 1;
bool g_grabbedThisFrame = false;
std::mutex g_resultLock;
std::string g_result;
bool g_resultOk = false;
std::filesystem::path g_resultFile;

void ReleaseSlots() {
    for (auto& s : g_slots) { SafeRelease(s.rt); SafeRelease(s.sys); SafeRelease(s.q); s.state = 0; }
}

// ---- game sound: WASAPI process loopback of this process (and its children) ----
class ActivateHandler final : public IActivateAudioInterfaceCompletionHandler, public IAgileObject {
    LONG ref_ = 1;
  public:
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    HRESULT hr = E_FAIL;
    IAudioClient* client = nullptr;
    ~ActivateHandler() { if (done) CloseHandle(done); }
    STDMETHODIMP QueryInterface(REFIID riid, void** out) override {
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IActivateAudioInterfaceCompletionHandler)) *out = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
        else if (riid == __uuidof(IAgileObject)) *out = static_cast<IAgileObject*>(this);
        else { *out = nullptr; return E_NOINTERFACE; }
        AddRef();
        return S_OK;
    }
    STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&ref_); }
    STDMETHODIMP_(ULONG) Release() override { const LONG r = InterlockedDecrement(&ref_); if (!r) delete this; return r; }
    STDMETHODIMP ActivateCompleted(IActivateAudioInterfaceAsyncOperation* op) override {
        HRESULT h = E_FAIL;
        IUnknown* u = nullptr;
        if (SUCCEEDED(op->GetActivateResult(&h, &u)) && SUCCEEDED(h) && u) u->QueryInterface(IID_PPV_ARGS(&client));
        hr = client ? S_OK : (FAILED(h) ? h : E_FAIL);
        if (u) u->Release();
        SetEvent(done);
        return S_OK;
    }
};

WAVEFORMATEX Pcm() {
    WAVEFORMATEX f{};
    f.wFormatTag = WAVE_FORMAT_PCM;
    f.nChannels = 2;
    f.nSamplesPerSec = kRate;
    f.wBitsPerSample = 16;
    f.nBlockAlign = 4;
    f.nAvgBytesPerSec = kRate * 4;
    return f;
}

bool OpenAudio(Session& s, std::string& why) {
    AUDIOCLIENT_ACTIVATION_PARAMS p{};
    p.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    p.ProcessLoopbackParams.TargetProcessId = GetCurrentProcessId();
    p.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
    PROPVARIANT pv{};
    pv.vt = VT_BLOB;
    pv.blob.cbSize = sizeof(p);
    pv.blob.pBlobData = reinterpret_cast<BYTE*>(&p);
    auto* h = new ActivateHandler();
    IActivateAudioInterfaceAsyncOperation* op = nullptr;
    HRESULT hr = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient), &pv, h, &op);
    if (SUCCEEDED(hr) && WaitForSingleObject(h->done, 3000) != WAIT_OBJECT_0) hr = HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    if (SUCCEEDED(hr)) hr = h->hr;
    if (SUCCEEDED(hr)) { s.audio = h->client; h->client = nullptr; }
    SafeRelease(h->client);
    SafeRelease(op);
    h->Release();
    if (FAILED(hr)) { why = std::format("game sound not available (process loopback {:#x}; needs Windows 10 2004 or later)", static_cast<unsigned>(hr)); return false; }
    WAVEFORMATEX f = Pcm();
    hr = s.audio->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_LOOPBACK | AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM,
                             2000000, 0, &f, nullptr);
    s.audioEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (SUCCEEDED(hr)) hr = s.audio->SetEventHandle(s.audioEvent);
    if (SUCCEEDED(hr)) hr = s.audio->GetService(IID_PPV_ARGS(&s.capture));
    if (FAILED(hr)) {
        why = std::format("game sound could not be opened ({:#x})", static_cast<unsigned>(hr));
        SafeRelease(s.capture);
        SafeRelease(s.audio);
        return false;
    }
    return true;
}

bool WriteAudio(Session& s, const BYTE* data, UINT32 frames, LONGLONG t) {
    const DWORD bytes = frames * 4;
    IMFMediaBuffer* buf = nullptr;
    IMFSample* smp = nullptr;
    BYTE* p = nullptr;
    bool ok = SUCCEEDED(MFCreateMemoryBuffer(bytes, &buf)) && SUCCEEDED(buf->Lock(&p, nullptr, nullptr));
    if (ok) {
        if (data) memcpy(p, data, bytes); else memset(p, 0, bytes);
        buf->Unlock();
        buf->SetCurrentLength(bytes);
        ok = SUCCEEDED(MFCreateSample(&smp)) && SUCCEEDED(smp->AddBuffer(buf)) && SUCCEEDED(smp->SetSampleTime(t)) &&
             SUCCEEDED(smp->SetSampleDuration(frames * kHns / kRate));
        if (ok) { std::lock_guard<std::mutex> lk(s.writerLock); ok = SUCCEEDED(s.writer->WriteSample(s.as, smp)); }
    }
    SafeRelease(smp);
    SafeRelease(buf);
    return ok;
}

// The sound keeps its own clock (samples written) from the moment it starts; silence fills the gaps when the game
// plays nothing, so sound and picture stay together.
void AudioLoop(std::shared_ptr<Session> sp) {
    Session& s = *sp;
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(s.audio->Start())) { if (SUCCEEDED(com)) CoUninitialize(); return; }
    const LONGLONG freq = QpcFreq();
    const LONGLONG start = (Qpc() - s.qpc0) * kHns / freq;
    LONGLONG frames = 0;
    auto timeAt = [&](LONGLONG n) { return start + n * kHns / kRate; };
    while (!s.audioStop.load()) {
        WaitForSingleObject(s.audioEvent, 50);
        UINT32 next = 0;
        while (SUCCEEDED(s.capture->GetNextPacketSize(&next)) && next) {
            BYTE* data = nullptr;
            UINT32 n = 0;
            DWORD flags = 0;
            if (FAILED(s.capture->GetBuffer(&data, &n, &flags, nullptr, nullptr))) break;
            if (n) {
                WriteAudio(s, (flags & AUDCLNT_BUFFERFLAGS_SILENT) ? nullptr : data, n, timeAt(frames));
                frames += n;
            }
            s.capture->ReleaseBuffer(n);
        }
        // nothing played for a while: silence up to 100 ms ago
        const LONGLONG wall = (Qpc() - s.qpc0) * kHns / freq;
        const LONGLONG behind = wall - 1000000 - timeAt(frames);
        if (behind > 1000000) {
            const UINT32 n = static_cast<UINT32>(std::min<LONGLONG>(behind * kRate / kHns, kRate));
            WriteAudio(s, nullptr, n, timeAt(frames));
            frames += n;
        }
    }
    s.audio->Stop();
    if (SUCCEEDED(com)) CoUninitialize();
}

// ---- the encoder (worker thread) ----
bool OpenWriter(Session& s, bool hevc, std::string& why) {
    IMFAttributes* a = nullptr;
    IMFMediaType *out = nullptr, *in = nullptr;
    HRESULT hr = MFCreateAttributes(&a, 4);
    if (SUCCEEDED(hr)) {
        a->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
        a->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
        a->SetGUID(MF_TRANSCODE_CONTAINERTYPE, s.o.fragmented ? MFTranscodeContainerType_FMPEG4 : MFTranscodeContainerType_MPEG4);
        hr = MFCreateSinkWriterFromURL(s.file.c_str(), nullptr, a, &s.writer);
    }
    if (SUCCEEDED(hr)) hr = MFCreateMediaType(&out);
    if (SUCCEEDED(hr)) {
        out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        out->SetGUID(MF_MT_SUBTYPE, hevc ? MFVideoFormat_HEVC : MFVideoFormat_H264);
        out->SetUINT32(MF_MT_AVG_BITRATE, static_cast<UINT32>(s.o.bitrateKbps) * 1000);
        out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(out, MF_MT_FRAME_SIZE, s.w, s.h);
        MFSetAttributeRatio(out, MF_MT_FRAME_RATE, s.o.fps, 1);
        MFSetAttributeRatio(out, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        if (!hevc) out->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
        hr = s.writer->AddStream(out, &s.vs);
    }
    if (SUCCEEDED(hr)) hr = MFCreateMediaType(&in);
    if (SUCCEEDED(hr)) {
        in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        in->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
        in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(in, MF_MT_FRAME_SIZE, s.w, s.h);
        MFSetAttributeRatio(in, MF_MT_FRAME_RATE, s.o.fps, 1);
        MFSetAttributeRatio(in, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        in->SetUINT32(MF_MT_DEFAULT_STRIDE, s.w * 4); // top-down rows
        hr = s.writer->SetInputMediaType(s.vs, in, nullptr);
    }
    SafeRelease(in);
    SafeRelease(out);
    SafeRelease(a);
    if (FAILED(hr)) { why = std::format("the {} encoder could not be set up ({:#x})", hevc ? "HEVC" : "H.264", static_cast<unsigned>(hr)); SafeRelease(s.writer); }
    return SUCCEEDED(hr);
}

bool AddAudioStream(Session& s) {
    IMFMediaType *out = nullptr, *in = nullptr;
    HRESULT hr = MFCreateMediaType(&out);
    if (SUCCEEDED(hr)) {
        out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        out->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_AAC);
        out->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        out->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kRate);
        out->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
        out->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, 24000); // 192 kbit/s
        hr = s.writer->AddStream(out, &s.as);
    }
    if (SUCCEEDED(hr)) hr = MFCreateMediaType(&in);
    if (SUCCEEDED(hr)) {
        in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Audio);
        in->SetGUID(MF_MT_SUBTYPE, MFAudioFormat_PCM);
        in->SetUINT32(MF_MT_AUDIO_BITS_PER_SAMPLE, 16);
        in->SetUINT32(MF_MT_AUDIO_SAMPLES_PER_SECOND, kRate);
        in->SetUINT32(MF_MT_AUDIO_NUM_CHANNELS, 2);
        in->SetUINT32(MF_MT_AUDIO_BLOCK_ALIGNMENT, 4);
        in->SetUINT32(MF_MT_AUDIO_AVG_BYTES_PER_SECOND, kRate * 4);
        hr = s.writer->SetInputMediaType(s.as, in, nullptr);
    }
    SafeRelease(in);
    SafeRelease(out);
    return SUCCEEDED(hr);
}

void Finish(Session& s, bool ok, const std::string& line) {
    {
        std::lock_guard<std::mutex> lk(g_resultLock);
        g_result = line;
        g_resultOk = ok;
        g_resultFile = s.file;
    }
    (ok ? LOG_INFO("[Video] " + line) : LOG_WARNING("[Video] " + line));
    g_finishing--;
    (void)s;
}

void Worker(std::shared_ptr<Session> sp) {
    Session& s = *sp;
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool mf = SUCCEEDED(MFStartup(MF_VERSION, MFSTARTUP_LITE));
    std::string why;
    bool ok = mf && OpenWriter(s, s.o.hevc, why);
    if (!ok && mf && s.o.hevc) {
        DeleteFileW(s.file.c_str());
        s.notes += "HEVC not available here, H.264 used; ";
        ok = OpenWriter(s, false, why);
    }
    if (ok && s.o.audio) {
        std::string aw;
        if (OpenAudio(s, aw) && AddAudioStream(s)) s.hasAudio = true;
        else { s.notes += (aw.empty() ? std::string("game sound could not be added") : aw) + "; "; SafeRelease(s.capture); SafeRelease(s.audio); }
    }
    if (ok && FAILED(s.writer->BeginWriting())) { ok = false; why = "the MP4 file could not be started"; }
    if (!ok) {
        s.error = mf ? why : "Media Foundation is not available";
        s.failed = true;
        SafeRelease(s.writer);
        SafeRelease(s.capture);
        SafeRelease(s.audio);
        if (s.audioEvent) CloseHandle(s.audioEvent);
        DeleteFileW(s.file.c_str());
        if (mf) MFShutdown();
        if (SUCCEEDED(com)) CoUninitialize();
        Finish(s, false, "The video could not be recorded: " + s.error);
        return;
    }
    HANDLE audioDone = s.hasAudio ? CreateEventW(nullptr, TRUE, FALSE, nullptr) : nullptr;
    if (audioDone && !HookGuard::StartDetached("Video: game sound", [sp, audioDone] {
            struct Done { HANDLE h; ~Done() { SetEvent(h); } } done{audioDone};
            AudioLoop(sp);
        })) {
        CloseHandle(audioDone);
        audioDone = nullptr;
    }
    const LONGLONG dur = kHns / std::max(1, s.o.fps);
    unsigned long long lastSize = 0;
    for (;;) {
        Frame f;
        {
            std::unique_lock<std::mutex> lk(s.m);
            s.cv.wait_for(lk, std::chrono::milliseconds(500), [&] { return !s.q.empty() || s.stopping; });
            if (s.q.empty()) {
                if (s.stopping) break;
            } else {
                f = std::move(s.q.front());
                s.q.pop_front();
            }
        }
        if (!f.px.empty()) {
            IMFMediaBuffer* buf = nullptr;
            IMFSample* smp = nullptr;
            BYTE* p = nullptr;
            const DWORD bytes = s.w * s.h * 4;
            LONGLONG t = std::max(f.t, s.lastVideoT + 1);
            s.lastVideoT = t;
            if (SUCCEEDED(MFCreateMemoryBuffer(bytes, &buf)) && SUCCEEDED(buf->Lock(&p, nullptr, nullptr))) {
                memcpy(p, f.px.data(), bytes);
                buf->Unlock();
                buf->SetCurrentLength(bytes);
                if (SUCCEEDED(MFCreateSample(&smp)) && SUCCEEDED(smp->AddBuffer(buf)) && SUCCEEDED(smp->SetSampleTime(t)) && SUCCEEDED(smp->SetSampleDuration(dur))) {
                    std::lock_guard<std::mutex> lk(s.writerLock);
                    if (SUCCEEDED(s.writer->WriteSample(s.vs, smp))) s.written++;
                }
            }
            SafeRelease(smp);
            SafeRelease(buf);
            std::lock_guard<std::mutex> lk(s.m);
            s.pool.push_back(std::move(f.px));
        }
        const unsigned long long now = GetTickCount64();
        if (now - lastSize >= 1000) {
            lastSize = now;
            WIN32_FILE_ATTRIBUTE_DATA d{};
            if (GetFileAttributesExW(s.file.c_str(), GetFileExInfoStandard, &d))
                s.bytes = (static_cast<unsigned long long>(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
        }
    }
    s.audioStop = true;
    const bool audioEnded = !audioDone || WaitForSingleObject(audioDone, 5000) == WAIT_OBJECT_0;
    if (audioEnded && audioDone) CloseHandle(audioDone);
    const HRESULT fin = s.writer->Finalize();
    SafeRelease(s.writer);
    if (audioEnded) { // else the sound thread may still use them: left to the process
        SafeRelease(s.capture);
        SafeRelease(s.audio);
        if (s.audioEvent) CloseHandle(s.audioEvent);
    }
    MFShutdown();
    if (SUCCEEDED(com)) CoUninitialize();
    WIN32_FILE_ATTRIBUTE_DATA d{};
    unsigned long long size = 0;
    if (GetFileAttributesExW(s.file.c_str(), GetFileExInfoStandard, &d)) size = (static_cast<unsigned long long>(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
    std::string name = ApexUtil::ToUtf8(s.file.filename().wstring());
    if (FAILED(fin) || !s.written) {
        if (!s.written) DeleteFileW(s.file.c_str());
        Finish(s, false, std::format("The video could not be finished ({:#x}, {} frames): {}", static_cast<unsigned>(fin), s.written.load(), name));
        return;
    }
    if (s.o.mkv) { // the MP4 copied into Matroska; the MP4 stays when that fails
        std::filesystem::path mkv = s.file;
        mkv.replace_extension(L".mkv");
        std::string mkvWhy;
        if (MkvRemux::Mp4ToMkv(s.file, mkv, mkvWhy)) {
            if (!s.o.keepMp4) DeleteFileW(s.file.c_str());
            s.file = mkv; // the saved video (Finish)
            if (GetFileAttributesExW(mkv.c_str(), GetFileExInfoStandard, &d)) size = (static_cast<unsigned long long>(d.nFileSizeHigh) << 32) | d.nFileSizeLow;
            name = ApexUtil::ToUtf8(mkv.filename().wstring()) + (s.o.keepMp4 ? " + .mp4" : "");
        } else {
            s.notes += "MKV not made (" + mkvWhy + "), the MP4 is kept; ";
        }
    }
    Finish(s, true, std::format("Video saved: {} ({}x{}, {} frames, {} dropped, {:.0f} MB{}{}{})", name, s.w, s.h, s.written.load(), s.dropped.load(),
                                size / 1048576.0, s.hasAudio ? ", game sound" : ", no sound", s.notes.empty() ? "" : "; ", s.notes));
}

// The largest free block of the game's address space (32-bit): the recorder needs room
size_t LargestFree() {
    size_t best = 0;
    MEMORY_BASIC_INFORMATION mi{};
    for (const char* p = nullptr; VirtualQuery(p, &mi, sizeof(mi)) == sizeof(mi);) {
        if (mi.State == MEM_FREE) best = std::max(best, static_cast<size_t>(mi.RegionSize));
        const char* next = static_cast<const char*>(mi.BaseAddress) + mi.RegionSize;
        if (next <= p) break;
        p = next;
    }
    return best;
}

bool OnBackBuffer(IDirect3DDevice9* dev, IDirect3DSurface9*& bb) {
    IDirect3DSurface9* rt = nullptr;
    const bool okBb = SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb;
    const bool okRt = SUCCEEDED(dev->GetRenderTarget(0, &rt)) && rt;
    const bool same = okBb && okRt && bb == rt;
    if (rt) rt->Release();
    if (!same && bb) { bb->Release(); bb = nullptr; }
    return same;
}

} // namespace

bool MediaFoundationAvailable() {
    // mfplat and mfreadwrite are delay-loaded (ApexRadiance.vcxproj): no Media Foundation call may run without them
    static const bool ok = [] {
        for (const wchar_t* dll : {L"mfplat.dll", L"mfreadwrite.dll"})
            if (!LoadLibraryExW(dll, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) return false;
        return true;
    }();
    return ok;
}

bool Start(IDirect3DDevice9* dev, const std::filesystem::path& file, const Options& o, std::string& why) {
    if (!MediaFoundationAvailable()) { why = "Media Foundation is not available"; return false; }
    if (g_active.load()) { why = "a video is already recording"; return false; }
    if (g_finishing.load() > 0 && !o.nextClip) { why = "the last video is still being saved"; return false; }
    if (!dev) { why = "no device"; return false; }
    if (g_s) { std::lock_guard<std::mutex> lk(g_sLock); g_s.reset(); } // a session ended by StopAndWait
    ReleaseSlots();
    const size_t freeBlock = LargestFree();
    if (freeBlock < (192u << 20)) { why = std::format("the game has too little free memory for a video ({} MB in one block, 192 MB needed); save and restart the game", freeBlock >> 20); return false; }
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) { why = "the screen could not be read"; return false; }
    D3DSURFACE_DESC d{};
    bb->GetDesc(&d);
    bb->Release();
    if (d.MultiSampleType != D3DMULTISAMPLE_NONE) { why = "the game uses multisampled anti-aliasing; turn it off for videos"; return false; }
    if (d.Format != D3DFMT_X8R8G8B8 && d.Format != D3DFMT_A8R8G8B8) { why = std::format("the screen format {} cannot be recorded", static_cast<int>(d.Format)); return false; }
    auto s = std::make_shared<Session>();
    s->file = file;
    s->o = o;
    s->o.fps = std::clamp(o.fps, 10, 60);
    s->h = std::min<UINT>(d.Height, static_cast<UINT>(std::max(240, o.maxHeight))) & ~1u;
    s->w = static_cast<UINT>(static_cast<unsigned long long>(d.Width) * s->h / d.Height) & ~1u;
    for (auto& sl : g_slots) {
        if (FAILED(dev->CreateRenderTarget(s->w, s->h, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &sl.rt, nullptr)) ||
            FAILED(dev->CreateOffscreenPlainSurface(s->w, s->h, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sl.sys, nullptr)) ||
            FAILED(dev->CreateQuery(D3DQUERYTYPE_EVENT, &sl.q))) {
            ReleaseSlots();
            why = "the video surfaces could not be created";
            return false;
        }
    }
    s->qpc0 = Qpc();
    g_period = std::max<LONGLONG>(1, QpcFreq() / s->o.fps);
    g_nextDue = s->qpc0;
    { std::lock_guard<std::mutex> lk(g_sLock); g_s = s; }
    g_active = true;
    g_finishing++;
    if (!o.nextClip) { std::lock_guard<std::mutex> lk(g_resultLock); g_result.clear(); }
    HookGuard::StartDetached("Video: MP4 writer", [s] { Worker(s); });
    LOG_INFO(std::format("[Video] Recording {} at {}x{}, {} fps, {} kbit/s{}{}", ApexUtil::ToUtf8(file.wstring()), s->w, s->h, s->o.fps, o.bitrateKbps,
                         o.hevc ? ", HEVC" : ", H.264", o.audio ? ", game sound" : ""));
    return true;
}

bool Grab(IDirect3DDevice9* dev, IDirect3DSurface9* src) {
    if (!g_active.load() || !dev || g_grabbedThisFrame || !g_s) return true;
    const LONGLONG now = Qpc();
    if (now < g_nextDue - g_period / 8) return true;
    if (g_s->failed.load()) return true;
    IDirect3DSurface9* bb = nullptr;
    if (src) { bb = src; bb->AddRef(); }
    else if (!OnBackBuffer(dev, bb)) return true; // an off-screen pass: wait for the frame's own end
    g_grabbedThisFrame = true;
    g_nextDue = g_s->qpc0 + ((now - g_s->qpc0) / g_period + 1) * g_period;
    Slot* slot = nullptr;
    for (auto& sl : g_slots) if (sl.state == 0) { slot = &sl; break; }
    if (!slot) { g_s->dropped++; bb->Release(); return true; }
    const bool copied = SUCCEEDED(dev->StretchRect(bb, nullptr, slot->rt, nullptr, D3DTEXF_LINEAR));
    if (copied) {
        slot->q->Issue(D3DISSUE_END);
        slot->state = 1;
        slot->t = (now - g_s->qpc0) * kHns / QpcFreq();
    }
    bb->Release();
    return copied || !src;
}

void Pump(IDirect3DDevice9* dev) {
    g_grabbedThisFrame = false;
    if (!g_s || !dev) return;
    if (!g_active.load()) { // stopped by StopAndWait (the game was closing, then did not close)
        ReleaseSlots();
        std::lock_guard<std::mutex> lk(g_sLock);
        g_s.reset();
        return;
    }
    // oldest first
    Slot* order[kSlots];
    for (int i = 0; i < kSlots; i++) order[i] = &g_slots[i];
    std::sort(order, order + kSlots, [](const Slot* a, const Slot* b) { return a->t < b->t; });
    for (Slot* sl : order) {
        if (sl->state == 2) {
            D3DLOCKED_RECT lr{};
            if (SUCCEEDED(sl->sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
                std::vector<BYTE> px;
                bool room = false;
                {
                    std::lock_guard<std::mutex> lk(g_s->m);
                    room = g_s->q.size() < kQueue;
                    if (room && !g_s->pool.empty()) { px = std::move(g_s->pool.back()); g_s->pool.pop_back(); }
                }
                if (room) {
                    const size_t row = static_cast<size_t>(g_s->w) * 4;
                    px.resize(row * g_s->h);
                    for (UINT y = 0; y < g_s->h; y++) memcpy(px.data() + y * row, static_cast<const BYTE*>(lr.pBits) + static_cast<size_t>(y) * lr.Pitch, row);
                    if (g_s->o.mouse) Captures::DrawMousePointer(dev, px.data(), g_s->w, g_s->h, 4); // a frame or two after the grab
                    std::lock_guard<std::mutex> lk(g_s->m);
                    g_s->q.push_back({std::move(px), sl->t});
                    g_s->cv.notify_one();
                } else {
                    g_s->dropped++;
                }
                sl->sys->UnlockRect();
            }
            sl->state = 0;
        } else if (sl->state == 1 && sl->q->GetData(nullptr, 0, 0) == S_OK) {
            sl->state = SUCCEEDED(dev->GetRenderTargetData(sl->rt, sl->sys)) ? 2 : 0; // locked next frame
        }
    }
}

void Stop() {
    if (!g_active.exchange(false)) return;
    ReleaseSlots();
    if (g_s) {
        std::lock_guard<std::mutex> lk(g_s->m);
        g_s->stopping = true;
        g_s->cv.notify_one();
    }
    std::lock_guard<std::mutex> lk(g_sLock);
    g_s.reset();
}

bool StopAndWait(unsigned ms) {
    std::shared_ptr<Session> s;
    { std::lock_guard<std::mutex> lk(g_sLock); s = g_s; }
    if (g_active.exchange(false) && s) {
        std::lock_guard<std::mutex> lk(s->m);
        s->stopping = true;
        s->cv.notify_one();
    }
    const unsigned long long until = GetTickCount64() + ms;
    while (g_finishing.load() > 0 && GetTickCount64() < until) Sleep(20);
    return g_finishing.load() <= 0;
}

void Release() { Stop(); ReleaseSlots(); }
bool Active() { return g_active.load(); }
bool Finishing() { return !g_active.load() && g_finishing.load() > 0; }
double Seconds() { return g_active.load() && g_s ? static_cast<double>(Qpc() - g_s->qpc0) / QpcFreq() : 0.0; }
unsigned long long BytesWritten() { return g_s ? g_s->bytes.load() : 0; }
bool Failed() { return g_s && g_s->failed.load(); }

std::string TakeResult(bool& ok, std::filesystem::path* file) {
    std::lock_guard<std::mutex> lk(g_resultLock);
    std::string r = std::move(g_result);
    g_result.clear();
    ok = g_resultOk;
    if (file) *file = g_resultFile;
    return r;
}

size_t LargestFreeBlock() { return LargestFree(); }

} // namespace VideoMp4
