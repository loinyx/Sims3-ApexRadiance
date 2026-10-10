#pragma once
// How Apex gets onto the game's Direct3D 9 device, next to official S3SS without racing it.
//
// TS3W.exe imports d3d9.dll statically, so the DLL is loaded before any ASI. In DllMain Apex detours only the export
// Direct3DCreate9. The first call that passes through it (the game's, or S3SS's hook thread's) installs, once and on
// that same thread, Apex's detour of IDirect3D9::CreateDevice. Every later hook on CreateDevice (S3SS's, its Resolution
// Spoofer's) is then installed after Apex's, in sequence: never two Detours transactions on the same function at once.
// CreateDevice (HAL): unchanged game present parameters, then EndScene and Reset of the created device are detoured.
// First EndScene: the D3D9Hooks registry and the ImGui overlay; first Present: the window procedure subclass.
// Every attach logs who already hooked the target (clean / E9 into which module).
//
// Per frame (inside the game's EndScene): RenderCallbacks::endSceneBeforeOverlay, Picture's end-of-scene copy, the Apex
// overlay (once per frame), the Picture pass, then the game's EndScene. Reset: overlay and features release their
// D3DPOOL_DEFAULT resources, the game's unchanged parameters and Reset, then recreate.
#include <windows.h>
#include <d3d9.h>
#include <cstdint>

namespace ApexD3D {

bool InstallFromDllMain(); // false when d3d9.dll is not loaded yet (EnsureInstalled then retries from the init thread)
void EnsureInstalled();    // init thread
void Shutdown();           // FreeLibrary only (never at process exit)

IDirect3DDevice9* Device(); // the game's HAL device (null before its first EndScene)
HWND Window();
void RequestEffectsRestart();
bool EffectsRestartPending();
bool PresentSeen();                // the first Present went through Apex's hooks
unsigned long long FirstPresentTick(); // GetTickCount64 at that first Present (0 before)

} // namespace ApexD3D
