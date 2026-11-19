// A window with the Apex menu as the game draws it, to click through without the game: the real header, sidebar,
// footer, undo toast and Shaders and Capture pages (production code, extracted by extract_ui.py), on the fixture's
// data (looks_fixture.cpp). Downloads take about two seconds a pack. The looks are copies: the player's own .ini files
// copied into a temporary Looks folder, and three example GShade presets beside the viewer; nothing outside them is
// written. Build and start: tools\shaders_menu_test\live.bat <output folder>
#include "rec.h"
#include <d3d9.h>
#include <filesystem>
#include <string>
#include "imgui_impl_dx9.h"
#include "imgui_impl_win32.h"
#include "patch_base.h"
#include "apex_config.h"
#include "build_flavor.h"
#include "apex_version.h"
#include "features/looks_preset.h"
#include "patches/looks.h"
#include "patches/night_lighting.h"
#include "ui/widgets.h"
#include "ui/violet_theme.h"
#include "ui/icons.h"
#include "ui/i18n.h"
#include "ui/logo.h"
#include "fixture.h"
using ApexUi::IconId;
using VioletTheme::Col;

extern LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
extern int g_shadersTab, g_captureTab;
extern ApexPatch* g_looksPatch;
void ShadersPage();
void CapturePage();

// ---- what the extracted window parts reach in apex_gui.cpp ----
enum Page : int { PageOverview, PageLighting, PageWaterSnow, PageColor, PageAmbientOcclusion, PageDepthBlur, PageEdgeSmoothing, PagePerformance, PageDeveloper, PageSettings, PageReport, PageConflicts, PageBanding, PageLotStreaming, PageProfiles, PageDisplay, PageCapture, PageShaders };
int g_page = PageShaders;
char g_search[96] = {};
bool g_focusSearch = false;
bool WelcomeActive() { return false; }
void HideWelcome() {}
bool HasAttentionItems() { return false; }
namespace NightLighting { bool MenuNightLevel(float& level) { level = 0.9f; return true; } }
namespace ApexConfig { bool SavePending() { return false; } }
float VersionButtonWidth() { return ImGui::CalcTextSize("4.0.5").x + 20.0f * ApexUi::Unit(); }
void VersionButton(float x, float y, float h)
{
    ImGui::GetWindowDrawList()->AddText(ImVec2(x + 10.0f * ApexUi::Unit(), y + (h - ImGui::GetTextLineHeight()) * 0.5f), ImGui::GetColorU32(Col(VioletTheme::kTextMuted)), "4.0.5");
}
// Undo: the Looks feature's table before the click, put back as the game puts back every feature's
toml::table g_snapshot;
void ApplyFeatureStateForgetting(const toml::table& state)
{
    if (const toml::table* t = state["Looks"].as_table()) g_looksPatch->ApplyTableLive(*t);
}
#include "chrome.inc"

namespace {
// The page band: the Shaders and Capture pages; the others are outside this viewer
void DrawPage()
{
    if (g_page == PageShaders) ShadersPage();
    else if (g_page == PageCapture) CapturePage();
    else
    {
        ApexUi::PageTitle("Not in this viewer", "Only Shaders and Capture are here; the other pages need the game");
    }
}

// MainWindow's layout (apex_gui.cpp), without the parts that need the game
void Window()
{
    const float u = ApexUi::Unit();
    ImGui::SetNextWindowSize(ImVec2(560.0f * u, 640.0f * u), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(ImVec2(80.0f, 40.0f), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(400.0f * u, 300.0f * u), ImVec2(FLT_MAX, FLT_MAX));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    const bool drawn = ImGui::Begin("Apex Radiance###ApexWindow", nullptr, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar();
    const ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
    // the feature's state before a click in the menu: what Undo puts back
    const ImGuiIO& io = ImGui::GetIO();
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && io.MousePos.x >= wp.x && io.MousePos.y >= wp.y && io.MousePos.x < wp.x + ws.x && io.MousePos.y < wp.y + ws.y)
    {
        g_snapshot.clear();
        toml::table looks;
        g_looksPatch->SaveToToml(looks);
        g_snapshot.insert_or_assign("Looks", std::move(looks));
    }
    if (drawn)
    {
        const bool collapsed = ApexConfig::GetUi().sidebarCollapsed;
        const float headerH = std::floor(kHeaderH * u), footerH = std::floor(kFooterH * u);
        const float sidebarW = std::floor((collapsed ? 56.0f : 184.0f) * u);
        const float bodyTop = wp.y + headerH + 1.0f;
        const float footerTop = std::floor(wp.y + ws.y - footerH);
        const float bodyH = std::fmax(footerTop - bodyTop, 60.0f * u);
        Header(wp, ws.x, headerH);
        ChromeHLine(wp.y + headerH);
        ChromeHLine(footerTop);
        ChromeVLine(wp.x + sidebarW, wp.y + headerH, footerTop);
        ImGui::SetCursorScreenPos(ImVec2(wp.x, bodyTop));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ApexUi::kSpace2 * u, ApexUi::kSpace3 * u));
        ImGui::PushStyleColor(ImGuiCol_ChildBg, Col(VioletTheme::kSidebarBg));
        ImGui::PushStyleVar(ImGuiStyleVar_ChildRounding, 0.0f);
        ImGui::BeginChild("##Sidebar", ImVec2(sidebarW, bodyH), ImGuiChildFlags_AlwaysUseWindowPadding, 0);
        ImGui::PopStyleVar(2);
        ImGui::PopStyleColor();
        Sidebar(collapsed);
        ImGui::EndChild();
        ImGui::SetCursorScreenPos(ImVec2(wp.x + sidebarW + 1.0f, bodyTop));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(ApexUi::kSpace4 * u, ApexUi::kSpace3 * u));
        static bool s_contentScrolls = false;
        if (!s_contentScrolls)
            for (ImGuiCol c : { ImGuiCol_ScrollbarGrab, ImGuiCol_ScrollbarGrabHovered, ImGuiCol_ScrollbarGrabActive }) ImGui::PushStyleColor(c, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::BeginChild("##Content", ImVec2(ws.x - sidebarW - 1.0f, bodyH), ImGuiChildFlags_AlwaysUseWindowPadding, ImGuiWindowFlags_AlwaysVerticalScrollbar);
        if (!s_contentScrolls) ImGui::PopStyleColor(3);
        s_contentScrolls = ImGui::GetScrollMaxY() > 0.0f;
        ImGui::PopStyleVar();
        ImGui::PushID(g_page);
        DrawPage();
        ImGui::PopID();
        if (const float barH = Looks::PacksBarHeight(); barH > 0.0f) ImGui::Dummy(ImVec2(0.0f, barH));
        ImGui::EndChild();
        StatusBar(ImVec2(wp.x, footerTop + 1.0f), ws.x, footerH - 1.0f, collapsed);
        ImGui::SetCursorScreenPos(ImVec2(wp.x, wp.y + ws.y - 1.0f));
        ImGui::Dummy(ImVec2(0.0f, 0.0f));
        const float barLeft = wp.x + sidebarW + 1.0f + ApexUi::kSpace4 * u;
        const float barRight = wp.x + ws.x - ImGui::GetStyle().ScrollbarSize - ApexUi::kSpace4 * u;
        const float barTop = Looks::PacksBar(barLeft, barRight, footerTop);
        // a reported change becomes the undo toast, as the game's menu does it
        std::string text;
        if (ApexUi::TakeChange(text)) ShowToast(text, g_snapshot);
        DrawToast(barTop, barTop < footerTop ? barRight : 0.0f);
    }
    ImGui::End();
}

// The player's own looks copied into the temporary Looks folder, and three example GShade presets beside the viewer
void CopyLooks()
{
    namespace fs = std::filesystem;
    std::error_code ec;
    const fs::path looks = fs::path(Looks::LooksFolder());
    fs::create_directories(looks, ec);
    wchar_t docs[MAX_PATH] = {};
    if (GetEnvironmentVariableW(L"USERPROFILE", docs, MAX_PATH))
        for (const auto& e : fs::directory_iterator(fs::path(docs) / L"Documents\\Electronic Arts\\The Sims 3\\Apex Radiance\\Looks", ec))
            if (e.path().extension() == L".ini") fs::copy_file(e.path(), looks / e.path().filename(), fs::copy_options::overwrite_existing, ec);
    wchar_t exe[MAX_PATH] = {};
    GetModuleFileNameW(nullptr, exe, MAX_PATH);
    const fs::path gshade = fs::path(exe).parent_path() / L"1. Shader Presets";
    fs::create_directories(gshade, ec);
    const char* const kExamples[][2] = { { "[Example] Serene Morning.ini", "Levels@Levels.fx,Vibrance@Vibrance.fx,Curves@Curves.fx" },
                                         { "[Example] Noir Night.ini", "Levels@Levels.fx,Curves@Curves.fx,Vignette@Vignette.fx,MXAO@qUINT_mxao.fx" },
                                         { "[Example] Winter Film.ini", "Technicolor2@Technicolor2.fx,Vignette@Vignette.fx" } };
    for (const auto& ex : kExamples)
        if (FILE* f = _wfopen((gshade / ex[0]).c_str(), L"wb"))
        {
            fprintf(f, "Techniques=%s\r\nTechniqueSorting=%s\r\n", ex[1], ex[1]);
            fclose(f);
        }
}

IDirect3D9* g_d3d = nullptr;
IDirect3DDevice9* g_dev = nullptr;
D3DPRESENT_PARAMETERS g_pp = {};

LRESULT WINAPI WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam)
{
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam)) return true;
    switch (msg)
    {
    case WM_SIZE:
        if (g_dev && wParam != SIZE_MINIMIZED)
        {
            g_pp.BackBufferWidth = LOWORD(lParam);
            g_pp.BackBufferHeight = HIWORD(lParam);
            ImGui_ImplDX9_InvalidateDeviceObjects();
            g_dev->Reset(&g_pp);
            ImGui_ImplDX9_CreateDeviceObjects();
        }
        return 0;
    case WM_DESTROY: PostQuitMessage(0); return 0;
    }
    return DefWindowProcW(hWnd, msg, wParam, lParam);
}
} // namespace

int WINAPI wWinMain(HINSTANCE inst, HINSTANCE, PWSTR, int)
{
    ImGui_ImplWin32_EnableDpiAwareness();
    WNDCLASSEXW wc = { sizeof(wc), CS_CLASSDC, WndProc, 0, 0, inst, nullptr, LoadCursor(nullptr, IDC_ARROW), nullptr, nullptr, L"ApexMenuViewer", nullptr };
    RegisterClassExW(&wc);
    HWND hwnd = CreateWindowW(wc.lpszClassName, L"Apex Radiance · Shaders menu viewer", WS_OVERLAPPEDWINDOW, 60, 40, 1100, 820, nullptr, nullptr, inst, nullptr);
    g_d3d = Direct3DCreate9(D3D_SDK_VERSION);
    g_pp.Windowed = TRUE;
    g_pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    g_pp.BackBufferFormat = D3DFMT_UNKNOWN;
    g_pp.EnableAutoDepthStencil = TRUE;
    g_pp.AutoDepthStencilFormat = D3DFMT_D16;
    g_pp.PresentationInterval = D3DPRESENT_INTERVAL_ONE;
    if (!g_d3d || g_d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hwnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &g_pp, &g_dev) < 0) return 1;
    ShowWindow(hwnd, SW_SHOWDEFAULT);
    UpdateWindow(hwnd);

    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    VioletTheme::ApplyStyle(ImGui::GetStyle());
    VioletTheme::LoadFonts(io);
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX9_Init(g_dev);
    CopyLooks();
    g_looksPatch = Fixture::StartLooks();
    Fixture::LooksState(false, false, false, 0);
    Fixture::PacksState(0);
    Fixture::Live();
    I18n::SetChoice(1); // Portuguese (Brazil); Settings > Menu > Language changes it in the game
    g_shadersTab = Looks::kTabLook;

    bool done = false;
    while (!done)
    {
        MSG msg;
        while (PeekMessageW(&msg, nullptr, 0U, 0U, PM_REMOVE))
        {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) done = true;
        }
        if (done) break;
        VioletTheme::UpdateFonts(io);
        ImGui_ImplDX9_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();
        RecItems().clear();
        Window();
        ImGui::EndFrame();
        g_dev->SetRenderState(D3DRS_ZENABLE, FALSE);
        g_dev->Clear(0, nullptr, D3DCLEAR_TARGET | D3DCLEAR_ZBUFFER, D3DCOLOR_RGBA(36, 44, 38, 255), 1.0f, 0);
        if (g_dev->BeginScene() >= 0)
        {
            ImGui::Render();
            ImGui_ImplDX9_RenderDrawData(ImGui::GetDrawData());
            g_dev->EndScene();
        }
        g_dev->Present(nullptr, nullptr, nullptr, nullptr);
    }
    ImGui_ImplDX9_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    g_dev->Release();
    g_d3d->Release();
    DestroyWindow(hwnd);
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
    return 0;
}
