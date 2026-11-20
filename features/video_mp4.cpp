// The player's MP4 recorder (see video_mp4.h).
#include "video_mp4.h"
#include "apex_log.h"
#include "apex_util.h"
#include "hook_guard.h"
#include "captures.h"
#include "mkv_remux.h"
#include "shader_cache.h"
#include "d3d9_hooks.h"
#include "d3d9_extra_hooks.h"
#include <d3dcompiler.h>
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

constexpr int kSlots = 4;      // GPU copies in flight (3 left a 4K 60 fps video short of free ones)
constexpr size_t kQueue = 2;   // frames waiting for the encoder (32-bit game: memory stays small)
constexpr size_t kMfPool = 6;  // the encoder's input buffers, reused (a 4K frame each: no allocation per frame)

// The frame as NV12 made on the GPU (BT.709, 16-235), the encoder's own input: 1.5 bytes a pixel read back instead of 4,
// and no colour conversion on the CPU. One A8R8G8B8 target of w/4 x 3h/2 holds the bytes in NV12's order: rows 0..h-1
// four luma samples a texel, rows h..3h/2-1 two U V pairs a texel (memory order B G R A, so the shader writes them as
// R = 3rd, G = 2nd, B = 1st, A = 4th). Chroma: the 2 x 2 average (bilinear at the block's centre).
constexpr const char* kNv12Hlsl = R"(
sampler2D src : register(s0);
float4 c0 : register(c0); // 1/w, 1/h of the frame, h, 0
static const float3 kY = float3(0.2126, 0.7152, 0.0722) * (219.0 / 255.0);
static const float3 kU = float3(-0.1146, -0.3854, 0.5) * (224.0 / 255.0);
static const float3 kV = float3(0.5, -0.4542, -0.0458) * (224.0 / 255.0);
float Luma(float x, float y) { return dot(tex2Dlod(src, float4((x + 0.5) * c0.x, (y + 0.5) * c0.y, 0, 0)).rgb, kY) + 16.0 / 255.0; }
float2 Chroma(float j, float r) {
    float3 c = tex2Dlod(src, float4((2.0 * j + 1.0) * c0.x, (2.0 * r + 1.0) * c0.y, 0, 0)).rgb;
    return float2(dot(c, kU), dot(c, kV)) + 128.0 / 255.0;
}
float4 Nv12PS(float2 p : VPOS) : COLOR {
    p = floor(p); // (the pixel's integer position, whatever offset the runtime gives VPOS)
    if (p.y < c0.z) {
        float x = p.x * 4.0;
        return float4(Luma(x + 2.0, p.y), Luma(x + 1.0, p.y), Luma(x, p.y), Luma(x + 3.0, p.y));
    }
    float r = p.y - c0.z, j = p.x * 2.0;
    float2 a = Chroma(j, r), b = Chroma(j + 1.0, r);
    return float4(b.x, a.y, a.x, b.y);
}
)";
const ShaderCache::Id g_nv12Ps = [] {
    ShaderCache::Desc d;
    d.tag = "Video NV12PS";
    d.source = kNv12Hlsl;
    d.sourceName = "video_nv12_ps.hlsl";
    d.entry = "Nv12PS";
    d.target = "ps_3_0";
    d.flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
    return ShaderCache::Add(std::move(d));
}();
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
    std::atomic<bool> writing{false}; // the encoder takes frames (it starts in about 2 s; none are copied before)
    std::atomic<unsigned> written{0}, dropped{0}, droppedGpu{0}; // dropped: all; droppedGpu: of them, no free GPU slot
    std::atomic<unsigned long long> bytes{0};
    std::string error, notes;
    bool nv12 = false;    // NV12 made on the GPU (else B G R X, converted by Media Foundation)
    DWORD frameBytes = 0; // one frame as the encoder takes it
    IMFSinkWriter* writer = nullptr;
    std::mutex writerLock;
    DWORD vs = 0, as = 0;
    bool hasAudio = false;
    IAudioClient* audio = nullptr;
    IAudioCaptureClient* capture = nullptr;
    HANDLE audioEvent = nullptr;
    std::atomic<bool> audioStop{false};
    LONGLONG lastVideoT = -1, firstVideoT = -1; // the first and last frame's times (the frames start once the encoder runs)
    // for the log: samples the encoder refused (and the first error), game sound written and filled with silence
    std::atomic<unsigned> videoFails{0}, audioFails{0};
    std::atomic<long> firstVideoHr{S_OK}, firstAudioHr{S_OK};
    std::atomic<unsigned long long> audioFrames{0}, silentFrames{0};
};

struct Slot { IDirect3DSurface9* rt = nullptr; IDirect3DSurface9* sys = nullptr; IDirect3DQuery9* q = nullptr; int state = 0; LONGLONG t = 0; };
Slot g_slots[kSlots];
// NV12: the frame scaled to the video's size (the shader's source) and the shader; the state the pass changes
IDirect3DTexture9* g_rgbTex = nullptr;
IDirect3DSurface9* g_rgbSurf = nullptr;
IDirect3DPixelShader9* g_nv12 = nullptr;
IDirect3DStateBlock9* g_saved = nullptr;
IDirect3DVertexBuffer9* g_quad = nullptr; // (a buffer and DrawPrimitive: an UP draw would reach the draw hooks' observers)
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
    SafeRelease(g_rgbSurf);
    SafeRelease(g_rgbTex);
    SafeRelease(g_nv12);
    SafeRelease(g_saved);
    SafeRelease(g_quad);
}

// The NV12 slots and the shader (render thread); false: the B G R X slots are used instead
bool MakeNv12(IDirect3DDevice9* dev, UINT w, UINT h, std::string& why) {
    std::string err;
    if (ShaderCache::CreatePixelShader(dev, g_nv12Ps, &g_nv12, &err) != ShaderCache::Result::Ok || !g_nv12) { why = "its shader: " + err.substr(0, 200); return false; }
    if (FAILED(dev->CreateTexture(w, h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_X8R8G8B8, D3DPOOL_DEFAULT, &g_rgbTex, nullptr)) || !g_rgbTex ||
        FAILED(g_rgbTex->GetSurfaceLevel(0, &g_rgbSurf))) { why = "its scaled frame"; return false; }
    if (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &g_saved)) || !g_saved) { why = "a state block"; return false; }
    struct V { float x, y, z, rhw; };
    const float x1 = w / 4.0f - 0.5f, y1 = h * 1.5f - 0.5f;
    const V quad[4] = {{-0.5f, -0.5f, 0, 1}, {x1, -0.5f, 0, 1}, {-0.5f, y1, 0, 1}, {x1, y1, 0, 1}};
    void* p = nullptr;
    if (FAILED(dev->CreateVertexBuffer(sizeof quad, D3DUSAGE_WRITEONLY, D3DFVF_XYZRHW, D3DPOOL_DEFAULT, &g_quad, nullptr)) || !g_quad ||
        FAILED(g_quad->Lock(0, 0, &p, 0)) || !p) { why = "its quad"; return false; }
    memcpy(p, quad, sizeof quad);
    g_quad->Unlock();
    for (auto& sl : g_slots)
        if (FAILED(dev->CreateRenderTarget(w / 4, h * 3 / 2, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &sl.rt, nullptr)) ||
            FAILED(dev->CreateOffscreenPlainSurface(w / 4, h * 3 / 2, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sl.sys, nullptr)) ||
            FAILED(dev->CreateQuery(D3DQUERYTYPE_EVENT, &sl.q))) { why = "its surfaces"; return false; }
    return true;
}

// g_rgbTex (w x h) into the slot's NV12 target, the device's state put back after
bool ToNv12(IDirect3DDevice9* dev, IDirect3DSurface9* rt, UINT w, UINT h) {
    D3D9Hooks::SettleDeferredShaders(dev);
    g_saved->Capture();
    const ExtraHooks::VertexInputMark input = ExtraHooks::MarkVertexInput();
    IDirect3DSurface9* savedRt[4] = {};
    IDirect3DSurface9* savedDs = nullptr;
    for (DWORD i = 0; i < 4; i++) if (FAILED(dev->GetRenderTarget(i, &savedRt[i]))) savedRt[i] = nullptr;
    if (FAILED(dev->GetDepthStencilSurface(&savedDs))) savedDs = nullptr;
    for (DWORD i = 1; i < 4; i++) dev->SetRenderTarget(i, nullptr);
    D3D9Hooks::CallOriginalSetRenderTarget(dev, 0, rt);
    dev->SetDepthStencilSurface(nullptr);
    for (const auto [st, v] : {std::pair{D3DRS_ZENABLE, 0ul}, {D3DRS_ZWRITEENABLE, 0ul}, {D3DRS_ALPHABLENDENABLE, 0ul}, {D3DRS_ALPHATESTENABLE, 0ul},
                               {D3DRS_STENCILENABLE, 0ul}, {D3DRS_CULLMODE, static_cast<DWORD>(D3DCULL_NONE)}, {D3DRS_FILLMODE, static_cast<DWORD>(D3DFILL_SOLID)},
                               {D3DRS_COLORWRITEENABLE, 0xFul}, {D3DRS_SRGBWRITEENABLE, 0ul}, {D3DRS_FOGENABLE, 0ul}, {D3DRS_CLIPPLANEENABLE, 0ul},
                               {D3DRS_SCISSORTESTENABLE, 0ul}})
        dev->SetRenderState(st, v);
    D3D9Hooks::CallOriginalSetTexture(dev, 0, g_rgbTex);
    for (const auto [st, v] : {std::pair{D3DSAMP_MINFILTER, static_cast<DWORD>(D3DTEXF_LINEAR)}, {D3DSAMP_MAGFILTER, static_cast<DWORD>(D3DTEXF_LINEAR)},
                               {D3DSAMP_MIPFILTER, static_cast<DWORD>(D3DTEXF_NONE)}, {D3DSAMP_ADDRESSU, static_cast<DWORD>(D3DTADDRESS_CLAMP)},
                               {D3DSAMP_ADDRESSV, static_cast<DWORD>(D3DTADDRESS_CLAMP)}, {D3DSAMP_SRGBTEXTURE, 0ul}})
        dev->SetSamplerState(0, st, v);
    D3D9Hooks::CallOriginalSetVertexShader(dev, nullptr);
    D3D9Hooks::CallOriginalSetPixelShader(dev, g_nv12);
    const float c0[4] = { 1.0f / w, 1.0f / h, static_cast<float>(h), 0.0f };
    dev->SetPixelShaderConstantF(0, c0, 1);
    dev->SetFVF(D3DFVF_XYZRHW);
    dev->SetStreamSource(0, g_quad, 0, 16);
    dev->SetStreamSourceFreq(0, 1);
    const bool drawn = SUCCEEDED(D3D9Hooks::CallOriginalDrawPrimitive(dev, D3DPT_TRIANGLESTRIP, 0, 2));
    D3D9Hooks::CallOriginalSetTexture(dev, 0, nullptr);
    D3D9Hooks::CallOriginalSetRenderTarget(dev, 0, savedRt[0]);
    for (DWORD i = 1; i < 4; i++) dev->SetRenderTarget(i, savedRt[i]);
    dev->SetDepthStencilSurface(savedDs);
    g_saved->Apply(); // (after the targets: setting target 0 resets the viewport)
    ExtraHooks::RestoreVertexInput(input);
    for (auto*& s : savedRt) SafeRelease(s);
    SafeRelease(savedDs);
    return drawn;
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
        if (ok) {
            std::lock_guard<std::mutex> lk(s.writerLock); // null once finalized
            const HRESULT hr = s.writer ? s.writer->WriteSample(s.as, smp) : E_POINTER;
            ok = SUCCEEDED(hr);
            if (!ok && s.audioFails++ == 0) s.firstAudioHr = hr;
        }
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
    if (const HRESULT hr = s.audio->Start(); FAILED(hr)) {
        LOG_WARNING(std::format("[Video] Game sound did not start ({:#x}): the video has no sound", static_cast<unsigned>(hr)));
        if (SUCCEEDED(com)) CoUninitialize();
        return;
    }
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
                s.audioFrames += n;
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
            s.audioFrames += n;
            s.silentFrames += n;
        }
    }
    s.audio->Stop();
    if (SUCCEEDED(com)) CoUninitialize();
}

// The video encoder Media Foundation picked (its friendly name; hardware encoders name the GPU), for the log
std::string EncoderName(Session& s) {
    std::string name = "unknown";
    IMFSinkWriterEx* ex = nullptr;
    if (SUCCEEDED(s.writer->QueryInterface(IID_PPV_ARGS(&ex)))) {
        for (DWORD i = 0;; ++i) {
            GUID category{};
            IMFTransform* t = nullptr;
            if (FAILED(ex->GetTransformForStream(s.vs, i, &category, &t))) break;
            if (category == MFT_CATEGORY_VIDEO_ENCODER) {
                IMFAttributes* a = nullptr;
                wchar_t* w = nullptr;
                UINT32 n = 0;
                if (SUCCEEDED(t->GetAttributes(&a)) && a && SUCCEEDED(a->GetAllocatedString(MFT_FRIENDLY_NAME_Attribute, &w, &n))) {
                    name = ApexUtil::ToUtf8(w);
                    CoTaskMemFree(w);
                }
                if (a && SUCCEEDED(a->GetItem(MFT_ENUM_HARDWARE_URL_Attribute, nullptr))) name += " (hardware)"; // a string: present on hardware encoders
                SafeRelease(a);
            }
            SafeRelease(t);
        }
        SafeRelease(ex);
    }
    return name;
}

// ---- the encoder (worker thread) ----
bool OpenWriter(Session& s, bool hevc, std::string& why) {
    IMFAttributes* a = nullptr;
    IMFMediaType *out = nullptr, *in = nullptr;
    HRESULT hr = MFCreateAttributes(&a, 4);
    if (SUCCEEDED(hr)) {
        a->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
        a->SetUINT32(MF_SINK_WRITER_DISABLE_THROTTLING, TRUE);
        // (Media Foundation's fragmented MP4 sink refuses HEVC at BeginWriting, MF_E_INVALIDMEDIATYPE, at any size: HEVC is
        // written as a plain MP4)
        a->SetGUID(MF_TRANSCODE_CONTAINERTYPE, s.o.fragmented && !hevc ? MFTranscodeContainerType_FMPEG4 : MFTranscodeContainerType_MPEG4);
        hr = MFCreateSinkWriterFromURL(s.file.c_str(), nullptr, a, &s.writer);
    }
    if (SUCCEEDED(hr)) hr = MFCreateMediaType(&out);
    if (SUCCEEDED(hr)) {
        out->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        out->SetGUID(MF_MT_SUBTYPE, hevc ? MFVideoFormat_HEVC : MFVideoFormat_H264);
        out->SetUINT32(MF_MT_AVG_BITRATE, static_cast<UINT32>(s.o.bitrateKbps) * 1000);
        out->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        if (s.nv12) {
            out->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
            out->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
            out->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
            out->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
        }
        MFSetAttributeSize(out, MF_MT_FRAME_SIZE, s.w, s.h);
        MFSetAttributeRatio(out, MF_MT_FRAME_RATE, s.o.fps, 1);
        MFSetAttributeRatio(out, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        if (!hevc) out->SetUINT32(MF_MT_MPEG2_PROFILE, eAVEncH264VProfile_High);
        hr = s.writer->AddStream(out, &s.vs);
    }
    if (SUCCEEDED(hr)) hr = MFCreateMediaType(&in);
    if (SUCCEEDED(hr)) {
        in->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
        in->SetGUID(MF_MT_SUBTYPE, s.nv12 ? MFVideoFormat_NV12 : MFVideoFormat_RGB32);
        in->SetUINT32(MF_MT_INTERLACE_MODE, MFVideoInterlace_Progressive);
        MFSetAttributeSize(in, MF_MT_FRAME_SIZE, s.w, s.h);
        MFSetAttributeRatio(in, MF_MT_FRAME_RATE, s.o.fps, 1);
        MFSetAttributeRatio(in, MF_MT_PIXEL_ASPECT_RATIO, 1, 1);
        in->SetUINT32(MF_MT_DEFAULT_STRIDE, s.nv12 ? s.w : s.w * 4); // top-down rows
        if (s.nv12) { // what the shader made, so players read it back the same way
            in->SetUINT32(MF_MT_YUV_MATRIX, MFVideoTransferMatrix_BT709);
            in->SetUINT32(MF_MT_VIDEO_NOMINAL_RANGE, MFNominalRange_16_235);
            in->SetUINT32(MF_MT_VIDEO_PRIMARIES, MFVideoPrimaries_BT709);
            in->SetUINT32(MF_MT_TRANSFER_FUNCTION, MFVideoTransFunc_709);
        }
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
    if (ok && s.o.hevc && s.o.fragmented) s.notes += "HEVC is written as a plain MP4 (Windows cannot write it crash-safe); ";
    if (!ok && mf && s.o.hevc) {
        DeleteFileW(s.file.c_str());
        s.notes += "HEVC not available here, H.264 used; ";
        ok = OpenWriter(s, false, why);
    }
    if (ok && s.o.audio) {
        std::string aw;
        if (OpenAudio(s, aw) && AddAudioStream(s)) s.hasAudio = true;
        else {
            s.notes += (aw.empty() ? std::string("game sound could not be added") : aw) + "; ";
            SafeRelease(s.capture);
            SafeRelease(s.audio);
            if (s.audioEvent) { CloseHandle(s.audioEvent); s.audioEvent = nullptr; }
        }
    }
    if (ok) {
        if (const HRESULT hr = s.writer->BeginWriting(); FAILED(hr)) { ok = false; why = std::format("the MP4 file could not be started ({:#x})", static_cast<unsigned>(hr)); }
        else LOG_INFO(std::format("[Video] Writer started: {} encoder \"{}\", {}{}", s.o.hevc && s.notes.find("HEVC not available") == std::string::npos ? "HEVC" : "H.264",
                                  EncoderName(s), s.hasAudio ? "game sound 48 kHz stereo" : "no sound",
                                  s.notes.empty() ? "" : "; " + s.notes.substr(0, s.notes.size() - 2))); // notes end in "; "
    }
    if (!ok) {
        s.error = mf ? why : "Media Foundation is not available";
        s.failed = true;
        SafeRelease(s.writer);
        SafeRelease(s.capture);
        SafeRelease(s.audio);
        if (s.audioEvent) { CloseHandle(s.audioEvent); s.audioEvent = nullptr; }
        DeleteFileW(s.file.c_str());
        if (mf) MFShutdown();
        if (SUCCEEDED(com)) CoUninitialize();
        Finish(s, false, "The video could not be recorded: " + s.error);
        return;
    }
    s.writing = true; // the render thread starts copying frames now
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
    std::vector<IMFMediaBuffer*> mfPool; // each held once here; one the encoder no longer holds is free
    const auto takeBuffer = [&]() -> IMFMediaBuffer* { // a reference for one frame
        for (IMFMediaBuffer* b : mfPool) { b->AddRef(); if (b->Release() == 1) { b->AddRef(); return b; } }
        IMFMediaBuffer* b = nullptr;
        if (FAILED(MFCreateMemoryBuffer(s.frameBytes, &b)) || !b) return nullptr;
        if (mfPool.size() < kMfPool) { mfPool.push_back(b); b->AddRef(); } // (past the pool, the encoder holding them all: used once)
        return b;
    };
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
            IMFMediaBuffer* buf = takeBuffer();
            IMFSample* smp = nullptr;
            BYTE* p = nullptr;
            const DWORD bytes = s.frameBytes;
            LONGLONG t = std::max(f.t, s.lastVideoT + 1);
            s.lastVideoT = t;
            if (s.firstVideoT < 0) s.firstVideoT = t;
            if (buf && SUCCEEDED(buf->Lock(&p, nullptr, nullptr))) {
                memcpy(p, f.px.data(), bytes);
                buf->Unlock();
                buf->SetCurrentLength(bytes);
                if (SUCCEEDED(MFCreateSample(&smp)) && SUCCEEDED(smp->AddBuffer(buf)) && SUCCEEDED(smp->SetSampleTime(t)) && SUCCEEDED(smp->SetSampleDuration(dur))) {
                    std::lock_guard<std::mutex> lk(s.writerLock);
                    const HRESULT hr = s.writer->WriteSample(s.vs, smp);
                    if (SUCCEEDED(hr)) s.written++;
                    else if (s.videoFails++ == 0) { s.firstVideoHr = hr; LOG_WARNING(std::format("[Video] The encoder refused a frame ({:#x}); later refusals are counted", static_cast<unsigned>(hr))); }
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
    for (IMFMediaBuffer* b : mfPool) b->Release(); // (the encoder releases what it still holds at Finalize)
    mfPool.clear();
    s.audioStop = true;
    const bool audioEnded = !audioDone || WaitForSingleObject(audioDone, 5000) == WAIT_OBJECT_0;
    if (audioEnded && audioDone) CloseHandle(audioDone);
    HRESULT fin;
    const ULONGLONG finStart = GetTickCount64();
    { // under the lock: a sound thread that missed the wait finds no writer instead of a released one
        std::lock_guard<std::mutex> lk(s.writerLock);
        fin = s.writer->Finalize();
        SafeRelease(s.writer);
    }
    {
        const double secs = s.lastVideoT > s.firstVideoT ? (s.lastVideoT - s.firstVideoT + kHns / std::max(1, s.o.fps)) / static_cast<double>(kHns) : 0.0;
        LOG_INFO(std::format("[Video] Encoder done: {} frames written over {:.1f} s ({:.1f} fps of {} asked), {} dropped ({} with the GPU copies still busy, the rest with the encoder behind), {} refused{}; "
                             "sound {}: {:.1f} s, {:.1f} s of it silence, {} refused{}; finalize {} ms ({:#x}){}",
                             s.written.load(), secs, secs > 0 ? s.written.load() / secs : 0.0, s.o.fps, s.dropped.load(), s.droppedGpu.load(), s.videoFails.load(),
                             s.videoFails ? std::format(" (first {:#x})", static_cast<unsigned>(s.firstVideoHr.load())) : "",
                             s.hasAudio ? "on" : "off", s.audioFrames.load() / static_cast<double>(kRate), s.silentFrames.load() / static_cast<double>(kRate),
                             s.audioFails.load(), s.audioFails ? std::format(" (first {:#x})", static_cast<unsigned>(s.firstAudioHr.load())) : "",
                             GetTickCount64() - finStart, static_cast<unsigned>(fin), audioEnded ? "" : "; the sound thread did not stop within 5 s"));
    }
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
        const ULONGLONG remuxStart = GetTickCount64();
        const bool remuxed = MkvRemux::Mp4ToMkv(s.file, mkv, mkvWhy);
        LOG_INFO(remuxed ? std::format("[Video] MKV made from the MP4 in {} ms", GetTickCount64() - remuxStart)
                         : std::format("[Video] MKV not made after {} ms: {}", GetTickCount64() - remuxStart, mkvWhy));
        if (remuxed) {
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
    if (d.MultiSampleType != D3DMULTISAMPLE_NONE) { bb->Release(); why = "the game uses multisampled anti-aliasing; turn it off for videos"; return false; }
    auto s = std::make_shared<Session>();
    s->file = file;
    s->o = o;
    s->o.fps = std::clamp(o.fps, 10, 60);
    s->h = std::min<UINT>(d.Height, static_cast<UINT>(std::max(240, o.maxHeight))) & ~1u;
    s->w = static_cast<UINT>(static_cast<unsigned long long>(d.Width) * s->h / d.Height) & ~3u; // (NV12 packs 4 a texel)
    std::string nv12Why;
    s->nv12 = MakeNv12(dev, s->w, s->h, nv12Why);
    if (!s->nv12) {
        LOG_WARNING("[Video] The frame is converted on the CPU: the GPU conversion could not be set up (" + nv12Why + ")");
        ReleaseSlots();
    }
    s->frameBytes = s->nv12 ? s->w * s->h * 3 / 2 : s->w * s->h * 4;
    for (auto& sl : g_slots) {
        if (s->nv12) break;
        if (FAILED(dev->CreateRenderTarget(s->w, s->h, D3DFMT_X8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &sl.rt, nullptr)) ||
            FAILED(dev->CreateOffscreenPlainSurface(s->w, s->h, D3DFMT_X8R8G8B8, D3DPOOL_SYSTEMMEM, &sl.sys, nullptr)) ||
            FAILED(dev->CreateQuery(D3DQUERYTYPE_EVENT, &sl.q))) {
            ReleaseSlots();
            bb->Release();
            why = "the video surfaces could not be created";
            return false;
        }
    }
    // The 8-bit slots take the frame through StretchRect, which also converts another screen format (a 10-bit back
    // buffer set by another mod); one trial copy says whether the GPU can
    const bool convertible = (d.Format == D3DFMT_X8R8G8B8 || d.Format == D3DFMT_A8R8G8B8) ||
                             SUCCEEDED(dev->StretchRect(bb, nullptr, s->nv12 ? g_rgbSurf : g_slots[0].rt, nullptr, D3DTEXF_LINEAR));
    bb->Release();
    if (!convertible) { ReleaseSlots(); why = std::format("the screen format {} cannot be recorded", static_cast<int>(d.Format)); return false; }
    s->qpc0 = Qpc();
    g_period = std::max<LONGLONG>(1, QpcFreq() / s->o.fps);
    g_nextDue = s->qpc0;
    { std::lock_guard<std::mutex> lk(g_sLock); g_s = s; }
    g_active = true;
    g_finishing++;
    if (!o.nextClip) { std::lock_guard<std::mutex> lk(g_resultLock); g_result.clear(); }
    if (!HookGuard::StartDetached("Video: MP4 writer", [s] { Worker(s); })) {
        g_active = false;
        g_finishing--;
        { std::lock_guard<std::mutex> lk(g_sLock); g_s.reset(); }
        ReleaseSlots();
        why = "the video writer could not be started";
        return false;
    }
    LOG_INFO(std::format("[Video] Recording {} at {}x{}, {} fps, {} kbit/s{}{}, {}", ApexUtil::ToUtf8(file.wstring()), s->w, s->h, s->o.fps, o.bitrateKbps,
                         o.hevc ? ", HEVC" : ", H.264", o.audio ? ", game sound" : "", s->nv12 ? "NV12 made on the GPU" : "B G R X (converted on the CPU)"));
    return true;
}

bool Grab(IDirect3DDevice9* dev, IDirect3DSurface9* src) {
    if (!g_active.load() || !dev || g_grabbedThisFrame || !g_s) return true;
    const LONGLONG now = Qpc();
    if (now < g_nextDue - g_period / 8) return true;
    if (g_s->failed.load() || !g_s->writing.load()) return true;
    IDirect3DSurface9* bb = nullptr;
    if (src) { bb = src; bb->AddRef(); }
    else if (!OnBackBuffer(dev, bb)) return true; // an off-screen pass: wait for the frame's own end
    g_grabbedThisFrame = true;
    // The period this frame stands for (one taken up to 1/8 period early counts for the period ahead, so the next one is a
    // whole period later), and its time on the video's even grid: one frame a period, evenly spaced
    const LONGLONG k = (now - g_s->qpc0 + g_period / 8) / g_period;
    g_nextDue = g_s->qpc0 + (k + 1) * g_period;
    Slot* slot = nullptr;
    for (auto& sl : g_slots) if (sl.state == 0) { slot = &sl; break; }
    if (!slot) { g_s->dropped++; g_s->droppedGpu++; bb->Release(); return true; }
    const bool copied = g_s->nv12 ? SUCCEEDED(dev->StretchRect(bb, nullptr, g_rgbSurf, nullptr, D3DTEXF_LINEAR)) && ToNv12(dev, slot->rt, g_s->w, g_s->h)
                                  : SUCCEEDED(dev->StretchRect(bb, nullptr, slot->rt, nullptr, D3DTEXF_LINEAR));
    if (copied) {
        slot->q->Issue(D3DISSUE_END);
        slot->state = 1;
        slot->t = k * g_period * kHns / QpcFreq();
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
                    const bool nv12 = g_s->nv12;
                    const size_t row = static_cast<size_t>(g_s->w) * (nv12 ? 1 : 4); // NV12: w bytes in both planes
                    const UINT rows = nv12 ? g_s->h * 3 / 2 : g_s->h;
                    px.resize(row * rows);
                    for (UINT y = 0; y < rows; y++) memcpy(px.data() + y * row, static_cast<const BYTE*>(lr.pBits) + static_cast<size_t>(y) * lr.Pitch, row);
                    if (g_s->o.mouse) { // a frame or two after the grab
                        if (nv12) Captures::DrawMousePointerNv12(dev, px.data(), g_s->w, g_s->h);
                        else Captures::DrawMousePointer(dev, px.data(), g_s->w, g_s->h, 4);
                    }
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
