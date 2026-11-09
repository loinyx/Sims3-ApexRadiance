#ifndef APEX_NO_DEV_TOOLS // (a build without the developer tools: ApexFlavorDefines=APEX_NO_DEV_TOOLS)
// The frames after a recording (see capture_burst.h).
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "capture_burst.h"
#include "build_flavor.h"
#include "captures.h"
#include "frame_classes.h"
#include "light_diag.h"
#include "light_probe.h"
#include "lot_light_bridge.h"
#include "post_scene.h"
#include "probe_neutral.h"
#include "render_callbacks.h"
#include "screen_watch.h"
#include "world_session.h"
#include "apex_log.h"
#include <windows.h>
#include <algorithm>
#include <cstring>
#include <format>
#include <vector>

namespace {

constexpr DWORD kMaxMs = 30000;
constexpr int kTries = 3;           // a cube write costs a view a frame, at most this many times
constexpr UINT kMaxMapSide = 2048;  // (5632 px maps need 220 MB of memory and a pass on the render thread)
constexpr size_t kMaxMaps = 64;

enum class Step { Off, Shot, Shade, Classes, Mouse, Cube, Snapshot, Maps, White };
Step g_step = Step::Off;
std::filesystem::path g_folder;
std::string g_what, g_text, g_abort;
DWORD g_startTick = 0;
IDirect3DSurface9 *g_rt = nullptr, *g_sys = nullptr;
int g_w = 0, g_h = 0;
bool g_armed = false, g_wanted = false, g_grabbed = false; // a view's frame is being drawn; its copy is asked; made
bool g_shade = false, g_shadeDrawn = false;
bool g_asked = false; // a light capture runs
bool g_resetHooked = false, g_haveCamera = false;
float g_camera[4][4];
int g_tries = 0;
long g_writes = 0;
POINT g_cubePixel{};
bool g_haveCubePixel = false;
struct Map {
    IDirect3DBaseTexture9* tex;
    UINT w, h;
};
std::vector<Map> g_maps; // holds a reference to each
size_t g_mapAt = 0;

void Line(const std::string& text) { g_text += text + '\n'; }

void ReleaseMaps() {
    for (const Map& m : g_maps) m.tex->Release();
    g_maps.clear();
    g_mapAt = 0;
}
void Free() {
    if (g_rt) g_rt->Release();
    if (g_sys) g_sys->Release();
    g_rt = g_sys = nullptr;
    ReleaseMaps();
    for (IDirect3DBaseTexture9* t : LotLightBridge::TakeFrameView().maps) t->Release();
}
void PreReset(IDirect3DDevice9*) {
    if (g_step == Step::Off) return;
    Free();
    g_abort = "the device was reset";
}

void Grab(IDirect3DDevice9* dev) {
    IDirect3DSurface9* bb = nullptr;
    if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return;
    g_grabbed = SUCCEEDED(dev->StretchRect(bb, nullptr, g_rt, nullptr, D3DTEXF_NONE));
    bb->Release();
}
// (ScreenWatch's point of the frame: before the Apex menu and notices)
void AtOverlay(IDirect3DDevice9* dev) {
    if (!g_wanted || !dev || !g_rt) return;
    g_wanted = false;
    Grab(dev);
}

// Asks the next frame's picture: read at the next Present. false: this device gives no picture
bool Arm(IDirect3DDevice9* dev) {
    if (!g_rt) {
        IDirect3DSurface9* bb = nullptr;
        if (FAILED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) || !bb) return false;
        D3DSURFACE_DESC d{};
        bb->GetDesc(&d);
        bb->Release();
        if (d.Format != D3DFMT_X8R8G8B8 && d.Format != D3DFMT_A8R8G8B8) return false;
        g_w = static_cast<int>(d.Width);
        g_h = static_cast<int>(d.Height);
        if (FAILED(dev->CreateRenderTarget(d.Width, d.Height, d.Format, D3DMULTISAMPLE_NONE, 0, FALSE, &g_rt, nullptr)) ||
            FAILED(dev->CreateOffscreenPlainSurface(d.Width, d.Height, d.Format, D3DPOOL_SYSTEMMEM, &g_sys, nullptr))) {
            Free();
            return false;
        }
    }
    g_wanted = g_armed = true;
    g_grabbed = false;
    return true;
}

// The picture asked, as B G R rows; empty when there is none
std::vector<BYTE> ReadView(IDirect3DDevice9* dev) {
    g_armed = g_wanted = false;
    std::vector<BYTE> bgr;
    if (!g_rt) return bgr;
    if (!g_grabbed) Grab(dev); // no EndScene reached the overlay this frame: the frame as presented
    if (!g_grabbed || FAILED(dev->GetRenderTargetData(g_rt, g_sys))) return bgr;
    bgr.resize(static_cast<size_t>(g_w) * g_h * 3);
    D3DLOCKED_RECT lr{};
    if (FAILED(g_sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) return {};
    for (int y = 0; y < g_h; y++) {
        const BYTE* s = static_cast<const BYTE*>(lr.pBits) + static_cast<size_t>(y) * lr.Pitch;
        BYTE* o = bgr.data() + static_cast<size_t>(y) * g_w * 3;
        for (int x = 0; x < g_w; x++) o[3 * x] = s[4 * x], o[3 * x + 1] = s[4 * x + 1], o[3 * x + 2] = s[4 * x + 2];
    }
    g_sys->UnlockRect();
    return bgr;
}

std::string CameraText() {
    float vp[4][4];
    if (!PostScene::RecentCameraViewProj(vp)) return "camera not seen";
    const bool moved = g_haveCamera && std::memcmp(vp, g_camera, sizeof vp) != 0;
    if (!g_haveCamera) std::memcpy(g_camera, vp, sizeof vp), g_haveCamera = true;
    return std::format("camera {}{}", vp[1][1] < 0.0f ? "mirrored" : "plain", moved ? ", MOVED since the first view" : "");
}

void Save(const char* file, std::vector<BYTE> bgr) {
    Line(std::format("[view] {}: {}", file, CameraText()));
    Captures::WritePngAsync(g_folder, g_folder / file, std::move(bgr), static_cast<UINT>(g_w), static_cast<UINT>(g_h), 3);
}

void ShadeStep(IDirect3DDevice9* dev) {
    if (!g_armed) {
        if (Arm(dev)) {
            g_shade = true;
            g_shadeDrawn = false;
        } else {
            Line("[skip] no views: the back buffer cannot be read");
            g_step = Step::Mouse;
        }
        return;
    }
    g_shade = false;
    std::vector<BYTE> bgr = ReadView(dev);
    if (g_shadeDrawn && !bgr.empty()) Save("AO alone.png", std::move(bgr));
    else Line("[skip] AO alone.png: Ambient Occlusion did not draw its shade (off, or no scene depth in that frame)");
    g_step = Step::Classes;
}

// One frame of LotLightBridge::SetFrameView: false until it is over. bgr: its picture, empty when there is none
bool LotViewStep(IDirect3DDevice9* dev, LotLightBridge::FrameView view, const char* file, std::vector<BYTE>& bgr, LotLightBridge::FrameViewResult& result) {
    if (!g_armed) {
        if (!Arm(dev)) {
            Line(std::format("[skip] {}: the back buffer cannot be read", file));
            return true;
        }
        g_writes = ProbeNeutral::Writes();
        LotLightBridge::SetFrameView(view, static_cast<UINT>(g_w), static_cast<UINT>(g_h));
        return false;
    }
    bgr = ReadView(dev); // (first: when it runs out of memory, Close gives back the maps the view still holds)
    result = LotLightBridge::TakeFrameView();
    if (ProbeNeutral::Writes() != g_writes) {
        Line(std::format("[skip] {}: a light probe wrote its cube in that frame: frame dropped ({} draws on other targets were left alone)", file, result.left));
        for (IDirect3DBaseTexture9* t : result.maps) t->Release();
        result.maps.clear();
        bgr.clear();
        if (++g_tries < kTries) return false;
    } else if (!result.drawn) {
        Line(std::format("[skip] {}: no draw of that frame reached the lighting hooks", file));
        bgr.clear();
    }
    g_tries = 0;
    return true;
}

void ClassesStep(IDirect3DDevice9* dev) {
    std::vector<BYTE> bgr;
    LotLightBridge::FrameViewResult r;
    if (!LotViewStep(dev, LotLightBridge::FrameView::Classes, "Classes.png", bgr, r)) return;
    g_step = Step::Mouse;
    for (IDirect3DBaseTexture9* t : r.maps) {
        D3DSURFACE_DESC d{};
        static_cast<IDirect3DTexture9*>(t)->GetLevelDesc(0, &d);
        g_maps.push_back({t, d.Width, d.Height});
    }
    std::sort(g_maps.begin(), g_maps.end(), [](const Map& a, const Map& b) { return a.w * a.h > b.w * b.h; });
    if (bgr.empty()) return;
    long counts[FrameClasses::kCount];
    FrameClasses::Count(bgr.data(), g_w, g_h, counts);
    Line(std::format("[classes] {} draws painted, {} left as they are (no depth test, no colour written, or not the screen's target)", r.drawn, r.left));
    for (int i = 0; i < FrameClasses::kCount; i++) {
        const FrameClasses::Class& c = FrameClasses::kClasses[i];
        Line(std::format("[class] {} | {} {} {} | {}", c.name, c.r, c.g, c.b, counts[i] * 16));
    }
    g_haveCubePixel = FrameClasses::Pick(bgr.data(), g_w, g_h, FrameClasses::kReadsCube, g_cubePixel);
    Save("Classes.png", std::move(bgr));
}

// Light capture of a pixel: asked once, done when the probe is idle again. true: the step is over
bool CaptureStep(IDirect3DDevice9* dev, const char* sub, POINT pixel, const char* why) {
    if (!g_asked) {
        g_asked = LightProbe::CaptureInto(dev, g_folder, sub, pixel, why);
        if (g_asked) Line(std::format("[probe] {}: pixel {} {}, {}", sub, pixel.x, pixel.y, why));
        else Line(std::format("[skip] {}: another light capture was running", sub));
        return !g_asked;
    }
    if (LightProbe::Busy()) return false;
    g_asked = false;
    return true;
}

void MapStep(IDirect3DDevice9* dev) { // one a frame: a large one takes a while on this thread
    if (g_mapAt >= g_maps.size()) {
        ReleaseMaps();
        g_step = Step::White;
        return;
    }
    const Map& m = g_maps[g_mapAt++];
    if (g_mapAt > kMaxMaps) Line(std::format("[map skipped] {}x{}: past the first {} maps", m.w, m.h, kMaxMaps));
    else if (m.w > kMaxMapSide || m.h > kMaxMapSide) Line(std::format("[map skipped] {}x{}: larger than {} per side", m.w, m.h, kMaxMapSide));
    else Line("[map] " + LightProbe::SaveMap(dev, m.tex, g_folder, std::format("M{}", g_mapAt)));
}

void Close(const std::string& why) {
    if (!why.empty()) Line("[end] " + why);
    Line(std::format("[end] {:.1f} s", (GetTickCount() - g_startTick) / 1000.0));
    g_shade = g_armed = g_wanted = g_asked = false;
    Free();
    RenderCallbacks::Remove(RenderCallbacks::filteredSceneBeforeOverlay, AtOverlay);
    PostScene::WantCamera(false);
    g_step = Step::Off;
    LOG_INFO(std::format("[CaptureBurst] Done{}{}", why.empty() ? "" : ": ", why));
    Captures::WriteText(g_folder / L"Burst.txt", g_text);
    Captures::Finish(g_folder, g_what, Captures::CaptureKind::Recording);
    g_text.clear();
}

void Advance(IDirect3DDevice9* dev) {
    switch (g_step) {
    case Step::Shot:
        if (Captures::ScreenshotPending()) return;
        g_step = Step::Shade;
        return ShadeStep(dev);
    case Step::Shade: return ShadeStep(dev);
    case Step::Classes: return ClassesStep(dev);
    case Step::Mouse: {
        POINT pixel{};
        const bool centre = ScreenWatch::MouseOrCentre(dev, pixel);
        if (CaptureStep(dev, "Light capture mouse", pixel, centre ? "the screen's centre (the mouse was on the Apex menu or outside the game)" : "under the mouse")) g_step = Step::Cube;
        return;
    }
    case Step::Cube:
        if (!g_haveCubePixel) Line("[skip] Light capture cube: no pixel that reads the cube was found");
        if (!g_haveCubePixel || CaptureStep(dev, "Light capture cube", g_cubePixel, "a pixel that reads the cube")) g_step = Step::Snapshot;
        return;
    case Step::Snapshot:
        LightDiag::WriteInto(g_folder);
        Line("[snapshot] Lighting snapshot.txt: " + LightDiag::Status());
        g_step = Step::Maps;
        return;
    case Step::Maps: return MapStep(dev);
    case Step::White: {
        std::vector<BYTE> bgr;
        LotLightBridge::FrameViewResult r;
        if (!LotViewStep(dev, LotLightBridge::FrameView::WhiteAlbedo, "White albedo.png", bgr, r)) return;
        if (!bgr.empty()) {
            Line(std::format("[white] {} draws drawn with white textures, {} left as they are", r.drawn, r.left));
            Save("White albedo.png", std::move(bgr));
        }
        return Close({});
    }
    case Step::Off: return;
    }
}

} // namespace

namespace CaptureBurst {

bool Begin(const std::filesystem::path& folder, const std::string& what) {
    if (kPublicBuild || g_step != Step::Off || !WorldSession::InWorld()) return false;
    g_folder = folder;
    g_what = what;
    g_text = "Apex Radiance capture burst: the frames after the recording, one per view, in this order\n";
    g_abort.clear();
    g_startTick = GetTickCount();
    g_haveCamera = g_haveCubePixel = false;
    g_tries = 0;
    Captures::ScreenshotNow(folder);
    RenderCallbacks::Add(RenderCallbacks::filteredSceneBeforeOverlay, AtOverlay);
    if (!g_resetHooked) {
        RenderCallbacks::Add(RenderCallbacks::preReset, PreReset);
        g_resetHooked = true;
    }
    PostScene::WantCamera(true);
    g_step = Step::Shot;
    LOG_INFO("[CaptureBurst] The frames after the recording start");
    return true;
}
bool Running() { return g_step != Step::Off; }
bool ViewFrame() { return g_armed; }
void OnPresent(IDirect3DDevice9* dev) {
    if (g_step == Step::Off) return;
    if (!g_abort.empty()) return Close(g_abort);
    if (!dev || !WorldSession::InWorld()) return Close("the world was left");
    if (GetTickCount() - g_startTick >= kMaxMs) return Close("out of time");
    try {
        Advance(dev);
    } catch (...) { // (no memory for a view: the text is saved and every view's switch goes back in Close)
        Close("a step failed");
    }
}
bool ShadeAlone() { return g_shade; }
void ShadeDrawn() { g_shadeDrawn = true; }

} // namespace CaptureBurst
#endif // APEX_NO_DEV_TOOLS
