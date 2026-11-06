// The production ScreenWatch and CaptureVideo rings on a native D3D9 device: real copies, read-backs and JPEGs, no game.
// A scratch folder is the only argument; Video.mjpeg stays there.
#include <windows.h>
#include "build_flavor.h"
#include "render_callbacks.h"
#include "screen_watch.h"
#include "capture_video.h"
#include "frame_classes.h"
#include <cassert>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

std::wstring g_folder;
bool g_recording = false, g_lamp = false;
namespace Recorder { bool Active() { return g_recording; } }
namespace ApexLog { void Write(Level, const std::string& text, const std::source_location&) noexcept { std::puts(text.c_str()); } bool DebugOn() noexcept { return false; } }
namespace HookGuard { void Note(const char* where) noexcept { std::puts(where); } void NoteAt(const char* where, const void*) noexcept { std::puts(where); } }
namespace CrashReport { void ThreadStart() {} }
namespace Overlay { bool MouseOverMenu() { return true; } }
namespace PostScene { bool RecentCameraViewProj(float vp[4][4]) { vp[1][1] = -1.0f; return g_lamp; } }
namespace ApexPaths { const std::wstring& ApexDirectory() { return g_folder; } }
namespace GroundProfile {
void WatchBegin(POINT, int, int) {}
void WatchEnd() {}
bool WatchPoints(int x[kWatchPoints], int y[kWatchPoints]) {
    for (int i = 0; i < kWatchPoints; i++) x[i] = 8, y[i] = 8;
    return g_lamp;
}
}

static D3DCOLOR Colour(int frame) { return D3DCOLOR_XRGB(frame & 0xFF, 255 - (frame & 0xFF), 40); }
static void Frame(IDirect3DDevice9* dev, int frame) {
    dev->BeginScene();
    dev->Clear(0, nullptr, D3DCLEAR_TARGET, Colour(frame), 1.0f, 0);
    RenderCallbacks::Fire(RenderCallbacks::filteredSceneBeforeOverlay, dev);
    dev->EndScene();
    dev->Present(nullptr, nullptr, nullptr, nullptr);
    ScreenWatch::OnPresent(dev);
}
// The classes frame read back: a darkened class keeps its hue, a dark or unpainted pixel is none, an edge is not picked
static void Classes() {
    using namespace FrameClasses;
    for (int i = 0; i < kCount; i++) {
        const Class& c = kClasses[i];
        assert(Nearest(c.r, c.g, c.b) == i && Nearest(c.r * 2 / 5, c.g * 2 / 5, c.b * 2 / 5) == i);
    }
    assert(Nearest(5, 5, 5) == -1 && Nearest(128, 178, 255) == -1);
    constexpr int w = 64, h = 32;
    std::vector<BYTE> bgr(w * h * 3);
    const auto paint = [&](int x0, int x1, int y0, int y1) {
        for (int y = y0; y < y1; y++)
            for (int x = x0; x < x1; x++) bgr[(y * w + x) * 3] = bgr[(y * w + x) * 3 + 1] = 200;
    };
    POINT p{};
    assert(!Pick(bgr.data(), w, h, kReadsCube, p));
    paint(30, 33, 0, h); // a line 3 px wide, in the middle: only edges
    assert(!Pick(bgr.data(), w, h, kReadsCube, p));
    paint(48, 60, 8, 24);
    paint(4, 10, 8, 24);
    assert(Pick(bgr.data(), w, h, kReadsCube, p) && p.x >= 50 && p.x < 58 && p.y >= 10 && p.y < 22); // the block nearer the centre
    long counts[kCount];
    Count(bgr.data(), w, h, counts);
    assert(counts[kReadsCube] > 0 && counts[kOther] == 0);
}
static std::string Read(const std::filesystem::path& p) {
    std::ifstream in(p, std::ios::binary);
    std::ostringstream s;
    s << in.rdbuf();
    return s.str();
}
static int Jpegs(const std::string& bytes) {
    int n = 0;
    for (size_t at = bytes.find("\xFF\xD8\xFF"); at != std::string::npos; at = bytes.find("\xFF\xD8\xFF", at + 3)) n++;
    return n;
}

int main(int argc, char** argv) {
    assert(argc == 2);
    Classes();
    const std::filesystem::path folder = argv[1];
    g_folder = folder.wstring();
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    kPublicBuild = false;
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW, wc.lpszClassName = L"ApexRingCheck";
    RegisterClassW(&wc);
    HWND wnd = CreateWindowW(wc.lpszClassName, L"", WS_POPUP, 0, 0, 2560, 1440, nullptr, nullptr, nullptr, nullptr);
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE, pp.SwapEffect = D3DSWAPEFFECT_DISCARD, pp.BackBufferWidth = 2560, pp.BackBufferHeight = 1440, pp.BackBufferFormat = D3DFMT_X8R8G8B8;
    pp.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE, pp.hDeviceWindow = wnd;
    IDirect3DDevice9* dev = nullptr;
    if (!d3d || FAILED(d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev))) {
        std::puts("no D3D9 device here: nothing checked");
        return 2;
    }
    const std::filesystem::path temp = folder / L"Video.tmp", video = folder / L"Video.mjpeg";
    std::filesystem::remove(video);
    std::ofstream(temp) << "a leftover of a crash"; // goes when the recording starts

    // a recording: no lamp in its first half
    g_recording = true;
    constexpr int kFrames = 200;
    for (int i = 0; i < kFrames; i++) {
        g_lamp = i >= kFrames / 2;
        Frame(dev, i);
        Sleep(2);
    }
    const std::string line = CaptureVideo::End(video);
    g_recording = false;
    ScreenWatch::OnPresent(dev);
    const auto samples = ScreenWatch::Samples();
    assert(samples.size() > kFrames / 2);
    int plain = 0, ground = 0, framed = 0, lastVideo = -1;
    for (const auto& s : samples) {
        (s.ground ? ground : plain)++;
        assert(s.mirrored == s.ground); // (the stub's camera: mirrored, seen only with the lamp)
        if (&s > &samples[1]) assert(s.frameMs > 0.0f); // (the first two come before a second Present)
        // the mouse's point has its frame's colour, the ground points theirs only when they were read
        const int f = s.rgb[ScreenWatch::kColumn / 2][0];
        assert(s.rgb[ScreenWatch::kColumn / 2][1] == 255 - f && s.rgb[ScreenWatch::kColumn / 2][2] == 40);
        assert(s.rgb[ScreenWatch::kGrid][2] == (s.ground ? 40 : 0));
        if (s.videoFrame >= 0) framed++, assert(s.videoFrame > lastVideo), lastVideo = s.videoFrame;
    }
    assert(plain > 0 && ground > 0 && framed > 0);
    const std::string bytes = Read(video);
    assert(!std::filesystem::exists(temp) && Jpegs(bytes) > kFrames / 8);
    assert(line.find("1920x1080") != std::string::npos);
    std::printf("%zu samples (%d without ground points), %d of them with a video frame; %s\n", samples.size(), plain, framed, line.c_str());

    // nothing reads the slots: never a wait, the frames with no slot are dropped and counted
    D3DSURFACE_DESC bd{};
    IDirect3DSurface9* bb = nullptr;
    dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb);
    bb->GetDesc(&bd);
    bb->Release();
    const std::filesystem::path other = folder / L"Dropped.mjpeg";
    CaptureVideo::Begin(dev, bd, temp);
    int taken = 0;
    for (int i = 0; i < 20; i++) taken += CaptureVideo::Grab(dev) >= 0;
    assert(taken == 3);
    for (int i = 0; i < 40; i++) dev->Present(nullptr, nullptr, nullptr, nullptr), CaptureVideo::Pump(dev), Sleep(5); // the 3 go through
    const std::string dropped = CaptureVideo::End(other);
    std::puts(dropped.c_str());
    assert(dropped.find("3 frames") != std::string::npos && dropped.find("7 dropped") != std::string::npos);
    assert(Jpegs(Read(other)) == 3);
    std::filesystem::remove(other);

    // a device reset in the middle stops the frames; a cancelled recording leaves no file
    CaptureVideo::Begin(dev, bd, temp);
    for (int i = 0; i < 2; i++) CaptureVideo::Grab(dev);
    CaptureVideo::Release();
    assert(CaptureVideo::Grab(dev) == -1);
    assert(CaptureVideo::End({}).empty() && !std::filesystem::exists(temp));
    D3DSURFACE_DESC msaa = bd;
    msaa.MultiSampleType = D3DMULTISAMPLE_4_SAMPLES;
    CaptureVideo::Begin(dev, msaa, temp);
    assert(CaptureVideo::Grab(dev) == -1 && !std::filesystem::exists(temp));
    std::puts("screen watch and video rings passed");
    dev->Release();
    d3d->Release();
    return 0;
}
