// The mouse pick read back without waiting for the GPU (see pick_readback.h).
//
// Part of Apex Radiance. Credits: @loinyx
#include "pick_readback.h"
#include "apex_log.h"
#include "d3d9_hooks.h"
#include "game_version.h"
#include "memory_patch.h"
#include "render_callbacks.h"
#include <windows.h>
#include <d3d9.h>
#include <atomic>
#include <cstring>
#include <format>

namespace PickReadback {
namespace {

// Steam 1.67.2: the pick manager's per-frame update (Scene::BeginFrame calls it at 0x006EBC62). With
// RenderPickDelayReadback on, one update renders the pick target and sets [this+0x14]; the next reads it back
// (0x006DFF45 -> 0x00610660, GetRenderTargetData) and clears it. That copy waited for the GPU: about 2.5 ms every other
// frame under the RTX Remix bridge (1.35 s in 10 s, a far camera pan in Sunset Valley).
constexpr uintptr_t kUpdate = 0x006DFEC0;
constexpr uint8_t kUpdateBytes[] = {0x55, 0x8B, 0xEC, 0x83, 0xE4, 0xF0, 0x81, 0xEC, 0x84, 0x07, 0x00, 0x00};
constexpr uintptr_t kReadbackDue = 0x14; // byte: the pick was rendered, its read-back is due this update
constexpr uint32_t kMaxWaitFrames = 4;   // then the game's read-back, waiting as before

using FnUpdate = void(__fastcall*)(void* self, void* edx);
FnUpdate g_original = nullptr;
std::atomic<IDirect3DDevice9*> g_dev{nullptr}; // the game's device (identity; Present)
IDirect3DQuery9* g_query = nullptr;            // render thread
IDirect3DDevice9* g_queryDev = nullptr;        // the device g_query was made on (identity)
void* g_pending = nullptr;                     // the manager whose read-back waits on g_query
uint32_t g_waits = 0;
std::atomic<uint64_t> c_deferred{0}, c_frames{0}, c_timedOut{0};

void ReleaseQuery() {
    if (g_query) g_query->Release();
    g_query = nullptr;
    g_pending = nullptr;
}

// (Steam 1.67.2, read from the code) With the flag set the update only reads back and returns (0x006DFF64 onwards, the
// next pick's request and render, runs only with it clear), so a deferred update skips nothing else.
// The read-back is due: the first time, an event query goes in behind the pick's draws and the update waits for the next
// frame; it reads back once the GPU has passed the query (no wait in the copy), or after kMaxWaitFrames
void __fastcall Hook_Update(void* self, void* edx) {
    IDirect3DDevice9* const dev = g_dev.load(std::memory_order_relaxed);
    if (g_query && g_queryDev != dev) ReleaseQuery(); // (another device: a query belongs to the one that made it)
    if (g_pending == self && !*reinterpret_cast<const uint8_t*>(reinterpret_cast<uintptr_t>(self) + kReadbackDue)) g_pending = nullptr; // the game dropped it
    if (dev && *reinterpret_cast<const uint8_t*>(reinterpret_cast<uintptr_t>(self) + kReadbackDue)) {
        if (g_pending != self) {
            if (!g_query && FAILED(dev->CreateQuery(D3DQUERYTYPE_EVENT, &g_query))) g_query = nullptr;
            g_queryDev = dev;
            if (g_query && SUCCEEDED(g_query->Issue(D3DISSUE_END))) {
                g_query->GetData(nullptr, 0, D3DGETDATA_FLUSH); // submit the pick's draws now
                g_pending = self, g_waits = 0;
                c_deferred.fetch_add(1, std::memory_order_relaxed);
                return; // (the flag stays set: the next update reads back)
            }
        } else if (g_query->GetData(nullptr, 0, D3DGETDATA_FLUSH) == S_FALSE && ++g_waits < kMaxWaitFrames) {
            c_frames.fetch_add(1, std::memory_order_relaxed);
            return;
        } else if (g_waits >= kMaxWaitFrames) {
            c_timedOut.fetch_add(1, std::memory_order_relaxed);
        }
        g_pending = nullptr;
    }
    g_original(self, edx);
}

void OnPreReset(IDirect3DDevice9*) { ReleaseQuery(); } // (render thread) the game reads its pick back as before until a new query

} // namespace

void Install() {
    if (g_gameVersion != GameVersion::Steam) return; // (the address is Steam 1.67.2's; other builds keep the game's wait)
    if (std::memcmp(reinterpret_cast<const void*>(kUpdate), kUpdateBytes, sizeof kUpdateBytes) != 0) {
        LOG_WARNING("[PickReadback] The pick update's code differs from Steam 1.67.2's: left to the game");
        return;
    }
    g_original = reinterpret_cast<FnUpdate>(kUpdate);
    if (!DetourBatch::InstallHooks({{reinterpret_cast<void**>(&g_original), reinterpret_cast<void*>(&Hook_Update)}})) {
        g_original = nullptr;
        LOG_ERROR("[PickReadback] Could not hook the pick update");
        return;
    }
    D3D9Hooks::RegisterPresent("PickReadback", [](D3D9Hooks::DeviceContext& ctx, const RECT*, const RECT*, HWND, const RGNDATA*) {
        g_dev.store(ctx.device, std::memory_order_relaxed);
        return D3D9Hooks::HookAction::Continue;
    });
    RenderCallbacks::Add(RenderCallbacks::preReset, OnPreReset);
    LOG_INFO("[PickReadback] On: the mouse pick is read back once the GPU has drawn it, without waiting");
}

std::string StatsText() {
    return std::format("{} pick read-backs deferred, {} frames waited, {} waited out", c_deferred.load(), c_frames.load(), c_timedOut.load());
}

} // namespace PickReadback
