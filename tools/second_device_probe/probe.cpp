// In-game test helper (never shipped): an ASI that makes a throwaway Direct3D 9 device the way Turbo Simulation Engine
// does (its own IDirect3D9, HAL, a 0x0 back buffer on a hidden 120x61 window, flags 0x120), then releases the device and
// the IDirect3D9. Apex's log tells it when: "before" = right after the game's device is created (before its first
// frame, as a player's 4.0.10 log showed), "after" = 600 ms after the first Present (as the 4.0.8 player's log showed).
// SecondDeviceProbe.ini next to it: [probe] when=before|after. What it did goes to SecondDeviceProbe.log beside it.
// Build (x86 Native Tools prompt):
//   cl /nologo /O2 /LD /EHsc tools\second_device_probe\probe.cpp d3d9.lib user32.lib shell32.lib ole32.lib /Fe:SecondDeviceProbe.asi
#include <windows.h>
#include <d3d9.h>
#include <shlobj.h>
#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>

namespace {
std::wstring g_dir;

void Note(const char* fmt, ...) {
    char line[512];
    va_list a;
    va_start(a, fmt);
    vsnprintf(line, sizeof line, fmt, a);
    va_end(a);
    SYSTEMTIME t;
    GetLocalTime(&t);
    std::ofstream(g_dir + L"SecondDeviceProbe.log", std::ios::app) << std::to_string(t.wHour) << ':' << std::to_string(t.wMinute) << ':'
                                                                    << std::to_string(t.wSecond) << '.' << std::to_string(t.wMilliseconds) << ' ' << line << '\n';
}

bool LogHas(const std::wstring& log, const char* text) {
    std::ifstream f(log, std::ios::binary); // (MSVC opens it shared: Apex keeps writing)
    std::stringstream s;
    s << f.rdbuf();
    return s.str().find(text) != std::string::npos;
}

DWORD WINAPI Run(void* module) {
    wchar_t when[16]{};
    GetPrivateProfileStringW(L"probe", L"when", L"before", when, 16, (g_dir + L"SecondDeviceProbe.ini").c_str());
    const bool after = std::wstring(when) == L"after";
    PWSTR docs = nullptr;
    if (FAILED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs))) return 1;
    const std::wstring log = std::wstring(docs) + L"\\Electronic Arts\\The Sims 3\\Apex Radiance\\ApexRadiance_LOG.txt";
    CoTaskMemFree(docs);
    Note("waiting (%s)", after ? "after the first Present" : "right after the game's device");
    const char* trigger = after ? "[D3D] First Present" : "[D3D] Game device created";
    for (int i = 0; !LogHas(log, trigger); i++) {
        if (i > 15000) { // 5 minutes
            Note("gave up: no \"%s\" in Apex's log", trigger);
            return 1;
        }
        Sleep(20);
    }
    if (after) Sleep(600);
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = static_cast<HINSTANCE>(module);
    wc.lpszClassName = L"SecondDeviceProbe";
    RegisterClassW(&wc);
    const HWND w = CreateWindowExW(0, wc.lpszClassName, L"", WS_POPUP, 0, 0, 120, 61, nullptr, nullptr, wc.hInstance, nullptr);
    IDirect3D9* d3d = Direct3DCreate9(D3D_SDK_VERSION);
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE;
    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
    pp.hDeviceWindow = w;
    IDirect3DDevice9* dev = nullptr;
    const HRESULT hr = d3d ? d3d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, w, D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_DISABLE_DRIVER_MANAGEMENT, &pp, &dev) : E_FAIL;
    Note("CreateDevice 0x%08X: device %p, back buffer %ux%u, IDirect3D9 %p", static_cast<unsigned>(hr), static_cast<void*>(dev), pp.BackBufferWidth,
         pp.BackBufferHeight, static_cast<void*>(d3d));
    const ULONG devLeft = dev ? dev->Release() : 0;
    const ULONG d3dLeft = d3d ? d3d->Release() : 0;
    // another module holding the IDirect3D9 shows here as references left (Apex's Display since 4.0.11)
    Note("released: device references left %lu, IDirect3D9 references left %lu", devLeft, d3dLeft);
    DestroyWindow(w);
    return 0;
}
} // namespace

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, void*) {
    if (reason == DLL_PROCESS_ATTACH) {
        DisableThreadLibraryCalls(module);
        wchar_t p[MAX_PATH];
        GetModuleFileNameW(module, p, MAX_PATH);
        g_dir = p;
        g_dir.resize(g_dir.find_last_of(L'\\') + 1);
        if (const HANDLE t = CreateThread(nullptr, 0, Run, module, 0, nullptr)) CloseHandle(t);
    }
    return TRUE;
}
