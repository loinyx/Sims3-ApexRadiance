#pragma once
// Apex's D3D9 device hooks. At the first EndScene of the game's device the framework detours 15 IDirect3DDevice9
// methods; features register callbacks by name, run in priority order (lower first; equal priorities in registration
// order). A callback returning Skip or Block stops the chain and the device call (S_OK / E_FAIL is returned to the game).
// A callback may call the device (a nested dispatch on the same thread) or register and unregister callbacks; a chain
// being run keeps the list it started with until it returns.
//
// Dispatch and locking (2026-09-29, docs/architecture.md section 3.6):
//  - The draw and state chains (DrawIndexedPrimitive, DrawPrimitive, SetRenderTarget, SetViewport, SetPixelShader,
//    SetVertexShader, SetTexture, Set*ShaderConstantF) are dispatched WITHOUT a lock on the render thread (the thread of
//    the first EndScene): they read an immutable list through an atomic pointer. Registering publishes a new list under
//    the registration lock; the old lists are kept until a safe point (Present on the render thread outside every dispatch, or
//    Uninstall), so a dispatch still reading one stays valid.
//  - Present, BeginScene and the Create* chains, and every chain called from another thread, run under the one
//    recursive registration lock, as before (one such dispatch at a time).
//  - UnregisterAll called from a thread other than the render thread returns only once the render thread has left any
//    lock-free dispatch that may still run the removed callbacks (bounded wait, 1 s, logged if it expires). Do not hold a
//    lock that a draw / state callback takes while unregistering from another thread.
#include <d3d9.h>
#include <cstdint>
#include <memory>
#include <string>
#include <type_traits>

namespace D3D9Hooks {

enum class Priority : int { First = 0, Early = 25, Normal = 50, Late = 75, Last = 100 }; // any int is allowed
enum class HookAction { Continue, Skip, Block };

struct DeviceContext {
    IDirect3DDevice9* device = nullptr;
    // Render states several draw callbacks read for the same draw (PostScene, Picture, the Banding Fix), read once per
    // dispatch (05/10: each read its own every draw, ~2,000 draws a frame). Valid for this dispatch only; a callback that
    // changes them through CallOriginal* restores them before it returns, as every callback does.
    uint8_t cached = 0;
    DWORD zEnable = 0, zWrite = 0, zFunc = 0;
    DWORD ZEnable() {
        if (!(cached & 1)) {
            zEnable = D3DZB_TRUE;
            device->GetRenderState(D3DRS_ZENABLE, &zEnable);
            cached |= 1;
        }
        return zEnable;
    }
    DWORD ZWriteEnable() {
        if (!(cached & 2)) {
            zWrite = TRUE;
            device->GetRenderState(D3DRS_ZWRITEENABLE, &zWrite);
            cached |= 2;
        }
        return zWrite;
    }
    DWORD ZFunc() { // (2.15.3) PostScene, Picture and the Banding Fix's jitter each read it for the same draw
        if (!(cached & 4)) {
            zFunc = D3DCMP_LESSEQUAL;
            device->GetRenderState(D3DRS_ZFUNC, &zFunc);
            cached |= 4;
        }
        return zFunc;
    }
};

// A registered callback: a plain function pointer and its object, called with one indirect call per dispatch (14/10:
// std::function added a virtual call and a copy of the arguments per callback, ~6,000 dispatches a frame). Built
// implicitly from any callable taking (DeviceContext&, args...) and returning HookAction: a lambda without captures is
// called through a function made for its type (no object); any other callable is copied to the heap, kept alive by
// the callback and every list that holds it.
template <typename Sig> class Callback;
template <typename... A> class Callback<HookAction(DeviceContext&, A...)> {
  public:
    using Raw = HookAction (*)(void* self, DeviceContext&, A...);
    Callback() = default;
    template <typename F, typename D = std::decay_t<F>,
              typename = std::enable_if_t<!std::is_same_v<D, Callback> && std::is_invocable_r_v<HookAction, D&, DeviceContext&, A...>>>
    Callback(F&& f) { // NOLINT: implicit on purpose (registration sites pass lambdas)
        if constexpr (std::is_empty_v<D> && std::is_default_constructible_v<D>) {
            fn_ = &Stateless<D>;
        } else {
            if constexpr (std::is_constructible_v<bool, const D&>)
                if (!static_cast<bool>(f)) return; // an empty std::function: nothing registered
            auto owned = std::make_shared<D>(std::forward<F>(f));
            self_ = owned.get();
            owner_ = std::move(owned);
            fn_ = &Stateful<D>;
        }
    }
    explicit operator bool() const { return fn_ != nullptr; }
    HookAction operator()(DeviceContext& ctx, A... a) const { return fn_(self_, ctx, a...); }
    Raw fn() const { return fn_; }
    void* self() const { return self_; }

  private:
    template <typename D> static HookAction Stateless(void*, DeviceContext& ctx, A... a) { return D{}(ctx, a...); }
    template <typename D> static HookAction Stateful(void* self, DeviceContext& ctx, A... a) { return (*static_cast<D*>(self))(ctx, a...); }
    Raw fn_ = nullptr;
    void* self_ = nullptr;
    std::shared_ptr<void> owner_;
};

using DrawIndexedPrimitiveHook = Callback<HookAction(DeviceContext&, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT)>;
using DrawPrimitiveHook = Callback<HookAction(DeviceContext&, D3DPRIMITIVETYPE, UINT, UINT)>;
using SetRenderTargetHook = Callback<HookAction(DeviceContext&, DWORD, IDirect3DSurface9*)>;
using SetPixelShaderHook = Callback<HookAction(DeviceContext&, IDirect3DPixelShader9*)>;
using SetVertexShaderHook = Callback<HookAction(DeviceContext&, IDirect3DVertexShader9*)>;
using SetTextureHook = Callback<HookAction(DeviceContext&, DWORD, IDirect3DBaseTexture9*)>;
using PresentHook = Callback<HookAction(DeviceContext&, const RECT*, const RECT*, HWND, const RGNDATA*)>;
using BeginSceneHook = Callback<HookAction(DeviceContext&)>;
using CreateTextureHook = Callback<HookAction(DeviceContext&, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DTexture9**, HANDLE*)>;
using CreateRenderTargetHook = Callback<HookAction(DeviceContext&, UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD, BOOL, IDirect3DSurface9**, HANDLE*)>;
using SetViewportHook = Callback<HookAction(DeviceContext&, const D3DVIEWPORT9*)>;
using CreatePixelShaderHook = Callback<HookAction(DeviceContext&, const DWORD*, IDirect3DPixelShader9**)>;
using CreateVertexShaderHook = Callback<HookAction(DeviceContext&, const DWORD*, IDirect3DVertexShader9**)>;
using SetPixelShaderConstantFHook = Callback<HookAction(DeviceContext&, UINT, const float*, UINT)>;
using SetVertexShaderConstantFHook = Callback<HookAction(DeviceContext&, UINT, const float*, UINT)>;

bool RegisterDrawIndexedPrimitive(const std::string& name, DrawIndexedPrimitiveHook hook, Priority priority = Priority::Normal);
bool RegisterDrawPrimitive(const std::string& name, DrawPrimitiveHook hook, Priority priority = Priority::Normal);
bool RegisterSetRenderTarget(const std::string& name, SetRenderTargetHook hook, Priority priority = Priority::Normal);
bool RegisterSetPixelShader(const std::string& name, SetPixelShaderHook hook, Priority priority = Priority::Normal);
bool RegisterSetVertexShader(const std::string& name, SetVertexShaderHook hook, Priority priority = Priority::Normal);
bool RegisterSetTexture(const std::string& name, SetTextureHook hook, Priority priority = Priority::Normal);
bool RegisterPresent(const std::string& name, PresentHook hook, Priority priority = Priority::Normal);
bool RegisterBeginScene(const std::string& name, BeginSceneHook hook, Priority priority = Priority::Normal);
bool RegisterCreateTexture(const std::string& name, CreateTextureHook hook, Priority priority = Priority::Normal);
bool RegisterCreateRenderTarget(const std::string& name, CreateRenderTargetHook hook, Priority priority = Priority::Normal);
bool RegisterSetViewport(const std::string& name, SetViewportHook hook, Priority priority = Priority::Normal);
bool RegisterCreatePixelShader(const std::string& name, CreatePixelShaderHook hook, Priority priority = Priority::Normal);
bool RegisterCreateVertexShader(const std::string& name, CreateVertexShaderHook hook, Priority priority = Priority::Normal);
bool RegisterSetPixelShaderConstantF(const std::string& name, SetPixelShaderConstantFHook hook, Priority priority = Priority::Normal);
bool RegisterSetVertexShaderConstantF(const std::string& name, SetVertexShaderConstantFHook hook, Priority priority = Priority::Normal);

// Removes every callback registered under this name (all methods).
void UnregisterAll(const std::string& name);

// Draws re-issued by a callback (Night Lighting replaces a draw: it changes the shader or constants, calls the device to
// draw, and skips the game's call; lot_light_bridge.cpp). The re-issued draw goes through the draw chains again, inside a
// ReissueScope on this thread: there it runs only the callbacks registered under a name passed to ObserveReissuedDraws
// (the Banding Fix, which dithers the replacement shader; Wall Heights, which runs after Night Lighting and sees a
// replaced wall only there; PostScene and Picture, whose scene draw counts include it; the developer observers: Frame
// Profiler, Light Probe, Frame Capture). The others (Ambient Occlusion's receiver replay, Night Lighting)
// already ran for the game's call of the same draw with the same state, and their nested run changed nothing (14/10). In the development build the driver's time for a re-issued draw is booked
// as "Re-issued draw (driver)", not as mod time.
void ObserveReissuedDraws(const std::string& name); // DrawIndexedPrimitive and DrawPrimitive callbacks of this name, now and later
// Render-target substitution (one owner: the High-precision scene, scene_target.cpp). Every SetRenderTarget callback sees
// the surface the caller asked for; the device then binds fn(index, requested). CallOriginalSetRenderTarget binds through
// it too, so a callback that saves render target 0 and puts it back keeps the substitution. nullptr = none.
using RenderTargetSubstitute = IDirect3DSurface9* (*)(DWORD index, IDirect3DSurface9* requested);
void SetRenderTargetSubstitute(RenderTargetSubstitute fn);
void BeginReissue();
void EndReissue();
struct ReissueScope {
    ReissueScope() { BeginReissue(); }
    ~ReissueScope() { EndReissue(); }
    ReissueScope(const ReissueScope&) = delete;
    ReissueScope& operator=(const ReissueScope&) = delete;
};

// The device methods without any Apex callback: the next hook in the Detours chain (a module that detoured the same
// function BEFORE Apex, e.g. official S3SS when it attached first), or the driver. A module that detoured it AFTER Apex
// (outer) does not see these calls either. Before Install they call through the device's vtable.
// Apex's own internal state changes around a replaced draw use these (Night Lighting, lot_light_bridge.cpp), so they
// never re-enter Apex's own chains; the replaced draw itself is still re-issued through the device (its observers:
// Post-scene / Picture trigger counts, Light Probe, Frame Capture, Frame Profiler).
HRESULT CallOriginalCreateRenderTarget(IDirect3DDevice9* device, UINT width, UINT height, D3DFORMAT format, D3DMULTISAMPLE_TYPE multiSample, DWORD quality, BOOL lockable,
                                       IDirect3DSurface9** surface, HANDLE* shared);
HRESULT CallOriginalSetRenderTarget(IDirect3DDevice9* device, DWORD index, IDirect3DSurface9* surface);
HRESULT CallOriginalSetViewport(IDirect3DDevice9* device, const D3DVIEWPORT9* viewport);
HRESULT CallOriginalDrawIndexedPrimitive(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, INT baseVertex, UINT minVertex, UINT numVertices, UINT startIndex, UINT primCount);
HRESULT CallOriginalDrawPrimitive(IDirect3DDevice9* device, D3DPRIMITIVETYPE type, UINT startVertex, UINT primCount);
HRESULT CallOriginalSetVertexShaderConstantF(IDirect3DDevice9* device, UINT start, const float* data, UINT count);
HRESULT CallOriginalSetPixelShaderConstantF(IDirect3DDevice9* device, UINT start, const float* data, UINT count);
HRESULT CallOriginalSetPixelShader(IDirect3DDevice9* device, IDirect3DPixelShader9* shader);
HRESULT CallOriginalCreatePixelShader(IDirect3DDevice9* device, const DWORD* function, IDirect3DPixelShader9** shader);
HRESULT CallOriginalCreateVertexShader(IDirect3DDevice9* device, const DWORD* function, IDirect3DVertexShader9** shader);
HRESULT CallOriginalSetVertexShader(IDirect3DDevice9* device, IDirect3DVertexShader9* shader);
HRESULT CallOriginalSetTexture(IDirect3DDevice9* device, DWORD stage, IDirect3DBaseTexture9* texture);

// ---- the shaders bound now (render thread) ----
// Followed through the SetPixelShader / SetVertexShader dispatches and CallOriginalSet*Shader, so a draw callback knows
// them without a device call (14/10: the Banding Fix read both back on every scene draw). Read back from the device, and
// followed from then on, whenever they are not known: on another thread, after a device call made from another
// thread, at every BeginScene and Present (a state block may have restored them: the menu overlay; a Reset), and after
// ShaderStateChanged. The pointers are identities: the device holds the references.
IDirect3DPixelShader9* BoundPixelShader(IDirect3DDevice9* device);
IDirect3DVertexShader9* BoundVertexShader(IDirect3DDevice9* device);
// (2.15.3) The texture bound at a pixel sampler (0..15), followed the same way through SetTexture and CallOriginalSetTexture
// (other stages: read back every time). No reference is added: an identity the device holds; a caller that keeps it past
// a texture change adds its own. One answer in 256 is checked against the device; after 8 differences in a session the
// textures are no longer followed (logged).
IDirect3DBaseTexture9* BoundTexture(IDirect3DDevice9* device, DWORD stage);
// As IDirect3DDevice9::GetTexture (a reference added to *texture, null for an empty stage), from BoundTexture
HRESULT GetBoundTexture(IDirect3DDevice9* device, DWORD stage, IDirect3DBaseTexture9** texture);
// Shaders were set without the device's Set*Shader (a state block applied, RtLampGpu::Engine::End): read them back
void ShaderStateChanged();

// ---- development build: counters for the Frame Profiler (zeros in the public build) ----
// Calls that reached Apex's detours (the game's calls, and other modules' calls made through the device), counted
// per method in the detours themselves with plain per-thread-unsafe increments (a statistic). Monotonic; the profiler
// takes differences.
struct StateCallCounts {
    uint32_t setTexture = 0, setVertexShader = 0, setPixelShader = 0, setVertexConstants = 0, setPixelConstants = 0, setRenderTarget = 0;
};
StateCallCounts ReadStateCallCounts();
// Dispatches of the lock-free chains that came from a thread other than the render thread (they took the lock).
uint32_t OffThreadDispatches();

// Framework (d3d9_bootstrap.cpp): attach the detours to the game's device (render thread, once) / detach (FreeLibrary).
bool Install(IDirect3DDevice9* device);
bool IsInstalled();
void Uninstall();

} // namespace D3D9Hooks
