#include "ui/widgets.h"
// Frame Capture (diagnostic)
// Records every render-target switch, depth-stencil switch, clear, StretchRect and draw call of a few
// consecutive frames into Documents\...\Apex Radiance\ApexRadiance_FrameCapture.txt. Used to find where the game finishes the
// 3D scene and starts drawing its UI, and whether the scene depth buffer can be read (needed for a depth blur).
// Zero cost while idle: hooks only format text while a capture is running.

#include "patch_base.h"
#include "hotkeys.h"
#include "apex_version.h"
#include "build_flavor.h"
#include "memory_patch.h"
#include "apex_log.h"
#include "d3d9_hooks.h"
#include "render_callbacks.h"
#include "d3d9_extra_hooks.h"
#include "apex_paths.h"
#include "imgui.h"
#include <d3d9.h>
#include <chrono>
#include <filesystem>
#include <format>
#include <fstream>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

constexpr const char* kHookName = "FrameCapture";
constexpr int kFramesPerCapture = 2;
constexpr size_t kMaxLines = 80000;

struct CaptureState {
    bool active = false;       // patch installed
    bool armed = false;        // waiting for next Present to start
    bool capturing = false;    // recording
    int framesLeft = 0;
    int frameNumber = 0;
    unsigned drawIndex = 0;    // draw counter inside current frame
    bool keyWasDown = false;
    int endScenesWhileArmed = 0;
    unsigned presentsSeen = 0;

    std::vector<std::string> lines;
    std::vector<std::string> pendingDefs;
    std::unordered_map<const void*, std::string> ids;
    std::unordered_map<std::string, int> counters;

    std::string runSig;
    unsigned runStart = 0;
    unsigned runCount = 0;
    unsigned runPrims = 0;

    IDirect3DSurface9* backBuffer = nullptr; // raw pointer, identity only
    std::string status = "Ready";
};

CaptureState g;

std::string FmtName(D3DFORMAT f) {
    switch (f) {
    case D3DFMT_UNKNOWN: return "UNKNOWN";
    case D3DFMT_A8R8G8B8: return "A8R8G8B8";
    case D3DFMT_X8R8G8B8: return "X8R8G8B8";
    case D3DFMT_R5G6B5: return "R5G6B5";
    case D3DFMT_A2R10G10B10: return "A2R10G10B10";
    case D3DFMT_A2B10G10R10: return "A2B10G10R10";
    case D3DFMT_A16B16G16R16F: return "A16B16G16R16F";
    case D3DFMT_A32B32G32R32F: return "A32B32G32R32F";
    case D3DFMT_R16F: return "R16F";
    case D3DFMT_R32F: return "R32F";
    case D3DFMT_G16R16F: return "G16R16F";
    case D3DFMT_G32R32F: return "G32R32F";
    case D3DFMT_G16R16: return "G16R16";
    case D3DFMT_A8: return "A8";
    case D3DFMT_L8: return "L8";
    case D3DFMT_A8L8: return "A8L8";
    case D3DFMT_D16: return "D16";
    case D3DFMT_D24S8: return "D24S8";
    case D3DFMT_D24X8: return "D24X8";
    case D3DFMT_D32: return "D32";
    case D3DFMT_D24FS8: return "D24FS8";
    case D3DFMT_D32F_LOCKABLE: return "D32F_LOCKABLE";
    case D3DFMT_DXT1: return "DXT1";
    case D3DFMT_DXT3: return "DXT3";
    case D3DFMT_DXT5: return "DXT5";
    default: break;
    }
    DWORD v = static_cast<DWORD>(f);
    if (v > 0xFF) {
        char c[5] = {static_cast<char>(v & 0xFF), static_cast<char>((v >> 8) & 0xFF), static_cast<char>((v >> 16) & 0xFF), static_cast<char>((v >> 24) & 0xFF), 0};
        return std::string("FOURCC:") + c;
    }
    return std::format("fmt{}", v);
}

std::string UsageStr(DWORD usage) {
    std::string s;
    if (usage & D3DUSAGE_RENDERTARGET) s += "RT|";
    if (usage & D3DUSAGE_DEPTHSTENCIL) s += "DS|";
    if (usage & D3DUSAGE_DYNAMIC) s += "DYN|";
    if (usage & D3DUSAGE_AUTOGENMIPMAP) s += "AUTOMIP|";
    if (s.empty()) return "-";
    s.pop_back();
    return s;
}

std::string NewId(const void* p, const char* prefix) {
    int n = ++g.counters[prefix];
    std::string id = std::format("{}{}", prefix, n);
    g.ids[p] = id;
    return id;
}

std::string TexId(IDirect3DBaseTexture9* t);

std::string SurfId(IDirect3DSurface9* s) {
    if (!s) return "null";
    if (auto it = g.ids.find(s); it != g.ids.end()) return it->second;

    std::string id = NewId(s, "S");
    D3DSURFACE_DESC d{};
    std::string desc = "?";
    if (SUCCEEDED(s->GetDesc(&d))) {
        desc = std::format("{}x{} {} usage={} pool={} ms={}", d.Width, d.Height, FmtName(d.Format), UsageStr(d.Usage), static_cast<int>(d.Pool), static_cast<int>(d.MultiSampleType));
    }
    IDirect3DTexture9* container = nullptr;
    if (SUCCEEDED(s->GetContainer(IID_IDirect3DTexture9, reinterpret_cast<void**>(&container))) && container) {
        desc += " (texture level of " + TexId(container) + ")";
        container->Release();
    }
    if (s == g.backBuffer) desc += " <-- BACKBUFFER";
    g.pendingDefs.push_back(std::format("    [def] {} = {}", id, desc));
    return id;
}

std::string TexId(IDirect3DBaseTexture9* t) {
    if (!t) return "null";
    if (auto it = g.ids.find(t); it != g.ids.end()) return it->second;

    std::string id = NewId(t, "T");
    std::string desc = "?";
    D3DRESOURCETYPE type = t->GetType();
    if (type == D3DRTYPE_TEXTURE) {
        D3DSURFACE_DESC d{};
        if (SUCCEEDED(static_cast<IDirect3DTexture9*>(t)->GetLevelDesc(0, &d))) {
            desc = std::format("tex2D {}x{} {} usage={} mips={}", d.Width, d.Height, FmtName(d.Format), UsageStr(d.Usage), t->GetLevelCount());
        }
    } else if (type == D3DRTYPE_CUBETEXTURE) {
        D3DSURFACE_DESC d{};
        if (SUCCEEDED(static_cast<IDirect3DCubeTexture9*>(t)->GetLevelDesc(0, &d))) {
            desc = std::format("cube {} {} usage={}", d.Width, FmtName(d.Format), UsageStr(d.Usage));
        }
    } else if (type == D3DRTYPE_VOLUMETEXTURE) {
        desc = "volume";
    }
    g.pendingDefs.push_back(std::format("    [def] {} = {}", id, desc));
    return id;
}

std::string ObjId(const void* p, const char* prefix) {
    if (!p) return "null";
    if (auto it = g.ids.find(p); it != g.ids.end()) return it->second;
    return NewId(p, prefix);
}

void AddLine(std::string line) {
    if (g.lines.size() < kMaxLines) g.lines.push_back(std::move(line));
}

void FlushDefs() {
    for (auto& d : g.pendingDefs) AddLine(std::move(d));
    g.pendingDefs.clear();
}

void FlushRun() {
    if (g.runCount == 0) return;
    if (g.runCount == 1) {
        AddLine(std::format("  #{} {} prims={}", g.runStart, g.runSig, g.runPrims));
    } else {
        AddLine(std::format("  #{}-#{} {} x{} prims={}", g.runStart, g.runStart + g.runCount - 1, g.runSig, g.runCount, g.runPrims));
    }
    g.runCount = 0;
    g.runSig.clear();
}

void Event(const std::string& text) {
    FlushRun();
    FlushDefs();
    AddLine(text);
}

void OnDraw(IDirect3DDevice9* dev, const char* kind, D3DPRIMITIVETYPE type, UINT prims) {
    if (!g.capturing) return;
    unsigned index = g.drawIndex++;

    IDirect3DSurface9* rt0 = nullptr;
    IDirect3DSurface9* rt1 = nullptr;
    IDirect3DSurface9* ds = nullptr;
    IDirect3DPixelShader9* ps = nullptr;
    IDirect3DVertexShader9* vs = nullptr;
    IDirect3DBaseTexture9* t0 = nullptr;
    IDirect3DBaseTexture9* t1 = nullptr;
    IDirect3DBaseTexture9* t2 = nullptr;
    dev->GetRenderTarget(0, &rt0);
    dev->GetRenderTarget(1, &rt1);
    dev->GetDepthStencilSurface(&ds);
    dev->GetPixelShader(&ps);
    dev->GetVertexShader(&vs);
    dev->GetTexture(0, &t0);
    dev->GetTexture(1, &t1);
    dev->GetTexture(2, &t2);

    DWORD zEnable = 99, zWrite = 99, zFunc = 99, blend = 99, colorWrite = 99, stencil = 99, alphaTest = 99;
    dev->GetRenderState(D3DRS_ZENABLE, &zEnable);
    dev->GetRenderState(D3DRS_ZWRITEENABLE, &zWrite);
    dev->GetRenderState(D3DRS_ZFUNC, &zFunc);
    dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &blend);
    dev->GetRenderState(D3DRS_COLORWRITEENABLE, &colorWrite);
    dev->GetRenderState(D3DRS_STENCILENABLE, &stencil);
    dev->GetRenderState(D3DRS_ALPHATESTENABLE, &alphaTest);
    D3DVIEWPORT9 vp{};
    dev->GetViewport(&vp);

    std::string sig = std::format("{} type={} rt0={} rt1={} ds={} ps={} vs={} t0={} t1={} t2={} z={}/{}/f{} st={} blend={} atest={} cw={:X} vp={},{} {}x{}", kind, static_cast<int>(type), SurfId(rt0), SurfId(rt1), SurfId(ds),
        ObjId(ps, "PS"), ObjId(vs, "VS"), TexId(t0), TexId(t1), TexId(t2), zEnable, zWrite, zFunc, stencil, blend, alphaTest, colorWrite, vp.X, vp.Y, vp.Width, vp.Height);

    if (rt0) rt0->Release();
    if (rt1) rt1->Release();
    if (ds) ds->Release();
    if (ps) ps->Release();
    if (vs) vs->Release();
    if (t0) t0->Release();
    if (t1) t1->Release();
    if (t2) t2->Release();

    if (g.runCount > 0 && sig == g.runSig) {
        g.runCount++;
        g.runPrims += prims;
        return;
    }
    FlushRun();
    FlushDefs();
    g.runSig = std::move(sig);
    g.runStart = index;
    g.runCount = 1;
    g.runPrims = prims;
}

std::string HrStr(HRESULT hr) { return SUCCEEDED(hr) ? "YES" : std::format("NO (0x{:08X})", static_cast<unsigned>(hr)); }

void WriteHeader(IDirect3DDevice9* dev) {
    AddLine(APEX_PRODUCT_NAME " Frame Capture");
    auto now = std::chrono::system_clock::now();
    AddLine(std::format("Date: {:%Y-%m-%d %H:%M:%S}", std::chrono::floor<std::chrono::seconds>(now)));
    AddLine(std::format("Game version: {}", GetGameVersionName()));

    D3DDEVICE_CREATION_PARAMETERS cp{};
    if (SUCCEEDED(dev->GetCreationParameters(&cp))) {
        AddLine(std::format("Device: adapter={} type={} behavior=0x{:X} (PUREDEVICE={})", cp.AdapterOrdinal, static_cast<int>(cp.DeviceType), cp.BehaviorFlags, (cp.BehaviorFlags & D3DCREATE_PUREDEVICE) ? "yes" : "no"));
    }

    IDirect3DSwapChain9* sc = nullptr;
    if (SUCCEEDED(dev->GetSwapChain(0, &sc)) && sc) {
        D3DPRESENT_PARAMETERS pp{};
        if (SUCCEEDED(sc->GetPresentParameters(&pp))) {
            AddLine(std::format("Present params: {}x{} fmt={} count={} ms={} msq={} swap={} windowed={} autoDS={} dsfmt={} flags=0x{:X} interval=0x{:X}", pp.BackBufferWidth, pp.BackBufferHeight, FmtName(pp.BackBufferFormat),
                pp.BackBufferCount, static_cast<int>(pp.MultiSampleType), pp.MultiSampleQuality, static_cast<int>(pp.SwapEffect), pp.Windowed, pp.EnableAutoDepthStencil, FmtName(pp.AutoDepthStencilFormat), pp.Flags,
                pp.PresentationInterval));
        }
        sc->Release();
    }

    IDirect3DSurface9* bb = nullptr;
    if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
        g.backBuffer = bb;
        g.ids[bb] = "BACKBUFFER";
        D3DSURFACE_DESC d{};
        bb->GetDesc(&d);
        AddLine(std::format("BACKBUFFER = {}x{} {} ms={}", d.Width, d.Height, FmtName(d.Format), static_cast<int>(d.MultiSampleType)));
        bb->Release();
    }

    IDirect3DSurface9* ds = nullptr;
    if (SUCCEEDED(dev->GetDepthStencilSurface(&ds)) && ds) {
        std::string id = SurfId(ds);
        AddLine("Depth-stencil bound at the start: " + id);
        ds->Release();
    }

    IDirect3D9* d3d = nullptr;
    if (SUCCEEDED(dev->GetDirect3D(&d3d)) && d3d) {
        D3DDISPLAYMODE dm{};
        dev->GetDisplayMode(0, &dm);
        auto fourcc = [](char a, char b, char c, char d) { return static_cast<D3DFORMAT>(MAKEFOURCC(a, b, c, d)); };
        AddLine("Readable depth support (depth-stencil texture):");
        AddLine("  INTZ: " + HrStr(d3d->CheckDeviceFormat(cp.AdapterOrdinal, cp.DeviceType, dm.Format, D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_TEXTURE, fourcc('I', 'N', 'T', 'Z'))));
        AddLine("  DF24: " + HrStr(d3d->CheckDeviceFormat(cp.AdapterOrdinal, cp.DeviceType, dm.Format, D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_TEXTURE, fourcc('D', 'F', '2', '4'))));
        AddLine("  DF16: " + HrStr(d3d->CheckDeviceFormat(cp.AdapterOrdinal, cp.DeviceType, dm.Format, D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_TEXTURE, fourcc('D', 'F', '1', '6'))));
        AddLine("  RESZ: " + HrStr(d3d->CheckDeviceFormat(cp.AdapterOrdinal, cp.DeviceType, dm.Format, D3DUSAGE_RENDERTARGET, D3DRTYPE_SURFACE, fourcc('R', 'E', 'S', 'Z'))));
        AddLine("  D24S8 as a texture: " + HrStr(d3d->CheckDeviceFormat(cp.AdapterOrdinal, cp.DeviceType, dm.Format, D3DUSAGE_DEPTHSTENCIL, D3DRTYPE_TEXTURE, D3DFMT_D24S8)));
        d3d->Release();
    }
    FlushDefs();
    AddLine("Legend: #n = draw index in the frame; z=ZENABLE/ZWRITE/ZFUNC; st=STENCIL; cw=COLORWRITE; t0..t2 = textures on samplers 0..2");
    AddLine("");
}

void WriteFile() {
    FlushRun();
    FlushDefs();
    std::filesystem::path path = std::filesystem::path(ApexPaths::ApexDirectory()) / L"ApexRadiance_FrameCapture.txt";
    std::ofstream out(path, std::ios::out | std::ios::trunc);
    if (!out) {
        g.status = "ERROR: could not write the file";
        LOG_ERROR("[FrameCapture] Could not open output file");
        return;
    }
    for (const auto& l : g.lines) out << l << "\n";
    if (g.lines.size() >= kMaxLines) out << "(capture truncated at " << kMaxLines << " lines)\n";
    out.close();
    g.status = std::format("Capture saved ({} lines) to ApexRadiance_FrameCapture.txt", g.lines.size());
    LOG_INFO("[FrameCapture] " + g.status);
}

void ResetCaptureData() {
    g.lines.clear();
    g.pendingDefs.clear();
    g.ids.clear();
    g.counters.clear();
    g.runSig.clear();
    g.runCount = 0;
    g.drawIndex = 0;
    g.frameNumber = 0;
    g.backBuffer = nullptr;
}

void StartCapture(IDirect3DDevice9* dev) {
    ResetCaptureData();
    g.armed = false;
    g.capturing = true;
    g.framesLeft = kFramesPerCapture;
    g.frameNumber = 1;
    WriteHeader(dev);
    AddLine("==== FRAME 1 ====");
    g.status = "Capturing...";
}

void OnPresentBoundary(IDirect3DDevice9* dev) {
    g.presentsSeen++;
    if (g.capturing) {
        Event(std::format("==== Present (end of frame {}, {} draws) ====", g.frameNumber, g.drawIndex));
        if (--g.framesLeft <= 0) {
            g.capturing = false;
            WriteFile();
            return;
        }
        g.frameNumber++;
        g.drawIndex = 0;
        AddLine(std::format("==== FRAME {} ====", g.frameNumber));
        return;
    }
    if (g.armed) StartCapture(dev);
}

// ---- observers on the shared extra hooks (methods the D3D9Hooks registry does not cover) ----

void ObserveClear(IDirect3DDevice9* dev, DWORD count, DWORD flags, D3DCOLOR color, float z) {
    if (!g.capturing) return;
    IDirect3DSurface9* rt0 = nullptr;
    IDirect3DSurface9* ds = nullptr;
    dev->GetRenderTarget(0, &rt0);
    dev->GetDepthStencilSurface(&ds);
    std::string rtId = SurfId(rt0);
    std::string dsId = SurfId(ds);
    if (rt0) rt0->Release();
    if (ds) ds->Release();
    std::string f;
    if (flags & D3DCLEAR_TARGET) f += "COLOR ";
    if (flags & D3DCLEAR_ZBUFFER) f += "Z ";
    if (flags & D3DCLEAR_STENCIL) f += "STENCIL ";
    Event(std::format("  Clear [{}] rt0={} ds={} color=0x{:08X} z={} rects={}", f, rtId, dsId, static_cast<unsigned>(color), z, count));
}

void ObserveSetDepthStencil(IDirect3DDevice9*, IDirect3DSurface9* surface) {
    if (!g.capturing) return;
    std::string id = SurfId(surface);
    Event("  SetDepthStencilSurface = " + id);
}

void ObserveStretchRect(IDirect3DDevice9*, IDirect3DSurface9* src, IDirect3DSurface9* dst, D3DTEXTUREFILTERTYPE filter) {
    if (!g.capturing) return;
    std::string s = SurfId(src);
    std::string d = SurfId(dst);
    Event(std::format("  StretchRect {} -> {} filter={}", s, d, static_cast<int>(filter)));
}

void ObserveDrawUP(IDirect3DDevice9* dev, const char* kind, D3DPRIMITIVETYPE type, UINT prims) { OnDraw(dev, kind, type, prims); }

void EnsureDetours(IDirect3DDevice9* dev) { ExtraHooks::EnsureInstalled(dev); }

void Arm() {
    if (g.capturing || g.armed) return;
    g.armed = true;
    g.endScenesWhileArmed = 0;
    g.status = "Waiting for the next frame...";
}

void OnEndScene(IDirect3DDevice9* dev) {
    if (!g.active) return;
    EnsureDetours(dev);

    // Its shortcut (Hotkeys: Ctrl+Shift+F, 7 or F5 by preset)
    if (Hotkeys::Take(Hotkeys::Action::FrameCapture)) Arm();

    if (g.capturing) Event("  ---- Game's EndScene (everything below: the Apex overlay, the Picture pass, other overlays) ----");

    if (g.armed && ++g.endScenesWhileArmed > 300 && g.presentsSeen == 0) {
        g.armed = false;
        g.status = "ERROR: the game does not call IDirect3DDevice9::Present, capture cancelled";
        LOG_ERROR("[FrameCapture] No Present seen while armed");
    }
}

} // namespace

class FrameCapturePatch : public ApexPatch {
  public:
    FrameCapturePatch() : ApexPatch("FrameCapture", nullptr) {}

    bool Install() override {
        if (kPublicBuild) { lastError = "Enable developer mode and restart the game first"; return false; }
        if (isEnabled) return true;
        lastError.clear();
        LOG_INFO("[FrameCapture] Installing...");

        using namespace D3D9Hooks;
        RegisterBeginScene(kHookName, [](DeviceContext& ctx) {
            EnsureDetours(ctx.device);
            if (g.capturing) Event("  BeginScene");
            return HookAction::Continue;
        }, Priority::Last);

        RegisterSetRenderTarget(kHookName, [](DeviceContext&, DWORD index, IDirect3DSurface9* rt) {
            if (g.capturing) {
                std::string id = SurfId(rt);
                Event(std::format("  SetRenderTarget[{}] = {}", index, id));
            }
            return HookAction::Continue;
        }, Priority::Last);

        RegisterDrawIndexedPrimitive(kHookName, [](DeviceContext& ctx, D3DPRIMITIVETYPE type, INT, UINT, UINT, UINT, UINT prims) {
            OnDraw(ctx.device, "DIP", type, prims);
            return HookAction::Continue;
        }, Priority::Last);

        RegisterDrawPrimitive(kHookName, [](DeviceContext& ctx, D3DPRIMITIVETYPE type, UINT, UINT prims) {
            OnDraw(ctx.device, "DP", type, prims);
            return HookAction::Continue;
        }, Priority::Last);

        RegisterPresent(kHookName, [](DeviceContext& ctx, const RECT*, const RECT*, HWND, const RGNDATA*) {
            OnPresentBoundary(ctx.device);
            return HookAction::Continue;
        }, Priority::Last);

        RenderCallbacks::Add(RenderCallbacks::endSceneBeforeOverlay, OnEndScene);
        ExtraHooks::SetClearObserver(ObserveClear);
        ExtraHooks::SetSetDepthStencilObserver(ObserveSetDepthStencil);
        ExtraHooks::SetStretchRectObserver(ObserveStretchRect);
        ExtraHooks::SetDrawUPObserver(ObserveDrawUP);

        g.active = true;
        isEnabled = true;
        LOG_INFO("[FrameCapture] Installed (press the button or " + ApexConfig::KeyChordText(Hotkeys::Key(Hotkeys::Action::FrameCapture)) + " to capture)");
        return true;
    }

    bool Uninstall() override {
        if (!isEnabled) return true;
        lastError.clear();
        g.active = false;
        g.armed = false;
        if (g.capturing) {
            g.capturing = false;
            WriteFile();
        }
        RenderCallbacks::Remove(RenderCallbacks::endSceneBeforeOverlay, OnEndScene);
        D3D9Hooks::UnregisterAll(kHookName);
        ExtraHooks::SetClearObserver(nullptr);
        ExtraHooks::SetSetDepthStencilObserver(nullptr);
        ExtraHooks::SetStretchRectObserver(nullptr);
        ExtraHooks::SetDrawUPObserver(nullptr);
        isEnabled = false;
        LOG_INFO("[FrameCapture] Uninstalled");
        return true;
    }

    void RenderCustomUI() override {
        SAFE_IMGUI_BEGIN();
        ImGui::Text("Status: %s", g.status.c_str());
        if (ApexUi::BeginControlRow("Render operations", "Render targets, depth buffers, clears and draw calls", ApexUi::ButtonWidth("Capture two frames", true))) {
            if (ApexUi::IconTextButton("Capture two frames", ApexUi::IconId::Camera)) Arm();
            ApexUi::EndControlRow();
        }
        if (ApexUi::BeginAdvanced("FrameCaptureDetails", "File and shortcut")) {
            ImGui::TextDisabled("Shortcut: %s", ApexConfig::KeyChordText(Hotkeys::Key(Hotkeys::Action::FrameCapture)).c_str());
            ImGui::TextDisabled("Frames per capture: %d", kFramesPerCapture);
            ImGui::TextDisabled("File: Documents\\Electronic Arts\\The Sims 3\\Apex Radiance\\ApexRadiance_FrameCapture.txt");
            ApexUi::EndAdvanced();
        }
    }
};

#include "build_flavor.h"
APEX_REGISTER_FEATURE(FrameCapturePatch, {.displayName = "Frame Capture (developer)",
                                      .description = "Diagnostic: dumps the draw calls of 2 frames to ApexRadiance_FrameCapture.txt (shortcut in Settings > Shortcuts)."
                                                     " Part of " APEX_PRODUCT_NAME ". Credits: @loinyx",
                                      .category = "Experimental",
                                      .experimental = true,
                                      .supportedVersions = VERSION_ALL,
                                      .technicalDetails = {"Logs SetRenderTarget, SetDepthStencilSurface, Clear, StretchRect and every draw with its bound shaders, textures and depth state.",
                                          "Detours Clear, StretchRect, SetDepthStencilSurface, DrawPrimitiveUP and DrawIndexedPrimitiveUP; the rest comes from the D3D9 hook registry.",
                                          "No work is done while no capture is running."}})
