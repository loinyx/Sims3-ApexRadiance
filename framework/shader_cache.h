#pragma once
// Apex's own HLSL shaders, compiled off the render thread (docs/architecture.md, "Shader precompile").
//
// Every feature registers each HLSL variant it can use (all qualities and modes) with Add(), from a namespace-scope
// initialiser in its own .cpp, so the whole list exists before the mod's init thread runs. Start() (init thread, right
// after the log opens, long before the device exists) compiles all of them with D3DCompile on one background thread and
// keeps the bytecode for the whole session. The render thread only creates the D3D9 shader objects from that bytecode
// (cheap) where the feature used to compile: at its first use, and again whenever the feature released them (Uninstall,
// Shutdown) - never compiling again.
//
// A request for a variant the worker has not finished yet makes the worker take that variant next and waits for it
// (logged and counted: it should not happen in practice, the precompile ends seconds after the game starts).
// D3D9 shader objects survive a device Reset; the features that release them anyway recreate them from the kept bytecode.
#include <d3d9.h>
#include <string>
#include <utility>
#include <vector>

namespace ShaderCache {

using Id = int; // index in the registry; -1 = none

struct Desc {
    const char* tag = "";         // log name, e.g. "DepthBlur BlurPS (TAPS 8)"
    std::string source;           // HLSL (kept only until it is compiled)
    const char* sourceName = "";  // pSourceName given to D3DCompile (as the feature passed it before)
    const char* entry = "main";
    const char* target = "ps_3_0";
    unsigned flags = 0;           // D3DCOMPILE_* flags, exactly as the feature compiled before
    std::vector<std::pair<std::string, std::string>> macros;
    int priority = 1;             // 0 = compiled first (the default quality / mode of each feature)
};

// Registers one variant (any thread; normally namespace-scope initialisers). A variant added after the worker finished is
// compiled by a new worker run.
Id Add(Desc desc);

// Starts the background compile of every registered variant (init thread). Safe to call more than once.
void Start();

// Nonblocking readiness probe: pending or failed worker creation never stalls a frame.
bool PrecompileComplete();

enum class Result { Ok, CompileFailed, CreateFailed };

// Creates the pixel shader of `id` from the precompiled bytecode (render thread). CompileFailed: *compileError gets the
// compiler's message. Waits for the worker when it has not compiled the variant yet.
Result CreatePixelShader(IDirect3DDevice9* dev, Id id, IDirect3DPixelShader9** out, std::string* compileError = nullptr);

// The precompiled bytecode of `id` (any thread; waits for the worker like CreatePixelShader). False with *compileError
// when it did not compile. For shaders a feature creates itself (vertex shaders, its own verification).
bool Bytecode(Id id, std::vector<DWORD>& out, std::string* compileError = nullptr);

// "Precompiled 34 of 34 Apex shaders in 1234 ms on a background thread (0 failed); render-thread waits: 0" (dev status)
std::string StatusText();

// Render-thread waits for the precompile so far: count and total ms (any thread; takes the registry lock briefly)
void RenderThreadWaits(int* waits, double* ms);

// FreeLibrary only: the worker stops between two compiles.
void Shutdown();

} // namespace ShaderCache
