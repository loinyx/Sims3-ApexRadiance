#pragma once
// The player's video recorder: the finished frame (ReShade/GShade look and Apex colour included, the Apex
// menu and notes left out unless asked) is scaled on the GPU to at most MaxHeight lines, read back through a ring of 3
// slots without waiting, and encoded by Windows Media Foundation (H.264 or HEVC, hardware encoder when present) into an
// MP4, with the game's own sound (WASAPI process loopback, Windows 10 2004+) as AAC. The render thread never waits: a
// frame with no free slot is dropped and counted.
#include <d3d9.h>
#include <filesystem>
#include <string>

namespace VideoMp4 {
struct Options {
    int fps = 30;
    int maxHeight = 1080;
    bool hevc = false;
    bool fragmented = false;
    int bitrateKbps = 16000;
    bool audio = true;
    bool mouse = false; // the mouse pointer drawn into each frame
    bool mkv = false;   // after the MP4 is finished: the same video as <name>.mkv (copied, not re-encoded)
    bool keepMp4 = false; // with mkv: keep the MP4 as well
    bool nextClip = false; // a new clip of a running recording: may start while the previous clip is still saved
};
// Render thread. false + why when it cannot start (nothing left open)
bool MediaFoundationAvailable(); // Windows' Media Foundation is installed (missing on Windows N without the Media Feature Pack)
bool Start(IDirect3DDevice9* dev, const std::filesystem::path& file, const Options& o, std::string& why);
// Render thread, with the frame finished (or src: that surface, e.g. the frame before the look); takes it when it is
// due. False only when src could not be copied.
bool Grab(IDirect3DDevice9* dev, IDirect3DSurface9* src = nullptr);
void Pump(IDirect3DDevice9* dev);   // render thread, every Present: moves the ring on, never waits
void Stop();                        // render thread: the file is finished on the worker; Finishing() tells when
// Any thread (the game closing): tells the writer to finish without device calls and waits up to ms; true when done
bool StopAndWait(unsigned ms);
void Release();                     // device reset: stops and frees the GPU objects
bool Active();                      // recording (Start .. Stop)
bool Finishing();                   // stopped, the worker still writes
double Seconds();
unsigned long long BytesWritten();  // the file's size so far (checked every second)
bool Failed();                      // the encoder could not start: Stop and read TakeResult
// The last finished clip: "" while none, else its log line (English); file: the saved video (the .mkv when made)
std::string TakeResult(bool& ok, std::filesystem::path* file = nullptr);
size_t LargestFreeBlock();          // the largest free block of the game's addresses (Start needs 192 MB); walks them
} // namespace VideoMp4
