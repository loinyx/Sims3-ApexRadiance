#pragma once
// GPU light probe used by Night Lighting ("light capture" on the Report a problem page, both builds).
// Its shortcut (F7 with the F-key set) with the mouse over a pixel: finds every draw call that covers that pixel in the
// next frame (occlusion query on a 1x1 scissored copy of each draw), writes Captures\<date time> Light capture\Light
// capture.txt with the textures those draws use as BMP files beside it (features/captures.h), and (Developer page) lets the
// user replace any of them with black or white to see what it does on screen.
#include <d3d9.h>
#include <filesystem>
#include <string>

namespace LightProbe {
void OnPresent(IDirect3DDevice9* device); // call every frame from the Present hook (render thread)
void RenderUI();                          // ImGui section
void Shutdown();                          // unregister hooks and release references
bool Capturing();                        // the draws of this frame are being recorded (other modules add detail only then)
bool Busy();                             // armed or recording: do not finalize the containing session yet
void Aim();                              // one-shot capture; click a point or use the existing Probe shortcut
bool Aiming();                           // thread-safe, for the overlay and Esc handling
void CancelAim();                        // cancels only selection, not a capture already running
bool ConfirmAim(POINT clientPixel);       // window thread: remember the clicked client pixel, finish selection at once
// Render thread (capture_burst.h): a capture of that back-buffer pixel into folder\sub, with no Finish of its own; false while
// another one runs. Done when Busy() is false again
bool CaptureInto(IDirect3DDevice9* dev, const std::filesystem::path& folder, const char* sub, POINT pixel, const std::string& why);
// Render thread: a 2D texture as a PNG in folder\Maps, as a capture saves its textures. Its line for a report
std::string SaveMap(IDirect3DDevice9* dev, IDirect3DBaseTexture9* tex, const std::filesystem::path& folder, const std::string& name);
}
