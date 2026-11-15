// features/apex_capture.cpp compiled as is, with the video encoder (VideoMp4) and the screenshot writer stubbed
#include "rec.h"
#include "features/apex_capture.cpp"
#include "fixture.h"

namespace { int g_video = 0; }

namespace VideoMp4 {
bool MediaFoundationAvailable() { return true; }
bool Start(IDirect3DDevice9*, const std::filesystem::path&, const Options&, std::string&) { return false; }
bool Grab(IDirect3DDevice9*, IDirect3DSurface9*) { return false; }
void Pump(IDirect3DDevice9*) {}
void Stop() {}
bool StopAndWait(unsigned) { return true; }
void Release() {}
bool Active() { return g_video == 1; }
bool Finishing() { return g_video == 2; }
double Seconds() { return 3725.0; }
unsigned long long BytesWritten() { return 2345ull << 20; }
bool Failed() { return false; }
std::string TakeResult(bool& ok, std::filesystem::path*) { ok = true; return {}; }
size_t LargestFreeBlock() { return 512u << 20; }
} // namespace VideoMp4

namespace Captures {
Note CurrentNote() { return {}; }
bool HideGameUiForVideo(bool) { return true; }
void Notify(const std::string&, int, NoteKind) {}
bool ScreenshotPending() { return false; }
} // namespace Captures

namespace Fixture {
void CaptureState(int video, int options)
{
    using namespace ApexCapture;
    g_video = video;
    Settings s; // the defaults
    if (options)
    {
        s.sound = "C:\\Example\\Music\\A rather long sound file name for the camera.wav";
        s.shot.jpg = true;
        s.shot.burstCount = 5;
        s.video.folder = kFolderCustom;
        s.video.customFolder = "D:\\Videos\\The Sims 3\\Apex Radiance recordings";
        s.video.quality = kQualityCustom;
        s.video.container = kContainerMkv;
        s.video.newClipMb = 300; // not one of the choices: the note under the row
    }
    Set(s);
    SetLastSaved(false, options ? "C:\\Example\\Documents\\Electronic Arts\\The Sims 3\\Screenshots\\Screenshot_2026-11-14_12-03-56_123.png" : "");
    SetLastSaved(true, options ? "C:\\Example\\Videos\\Video_2026-11-14_12-03-56.mp4" : "");
}
} // namespace Fixture
