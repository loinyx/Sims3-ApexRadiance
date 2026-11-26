#pragma once
// Read-only loaded-world gate shared by the menu notice and Depth Blur.
// Fields: docs/engine/lot-loading-and-streaming.md. Never latch across loads.
#include "game_addresses.h"
#include <Windows.h>
#include <string>

namespace WorldSession {
struct Settled {
    unsigned long long activeAt = 0;
    bool tracking = false;
    bool ready = false;
    void Update(bool active, unsigned long long now) {
        if (!active) { tracking = false; ready = false; return; }
        if (!tracking) { tracking = true; activeAt = now; }
        ready = now - activeAt >= 3000;
    }
};
constexpr bool Active(unsigned char active, int mode) {
    return active != 0 && mode >= 1 && mode <= 3;
}
// Startup/loading window id set by 0x00EC7DB9 and removed by 0x00EC7A60.
// UI service/root/child lookups are the same read-only path used by that callback.
// A game build where the getter was not found or has another shape cannot show the window at all: it never holds the
// effects back there (failing closed left Color, Ambient Occlusion and Edge Smoothing off for good on such a build,
// switched on in the menu). The UI not up yet (start-up) still waits.
inline bool LoaderDismissed() {
    const uintptr_t getter = GameAddr::Get(GameAddr::Id::UiServiceGetter);
    if (!getter) return true;
    __try {
        const auto* code = reinterpret_cast<const unsigned char*>(getter);
        if (code[0] != 0xA1 || code[5] != 0xC3) return true;
        const uintptr_t global = *reinterpret_cast<const uint32_t*>(code + 1);
        const uintptr_t service = global ? *reinterpret_cast<const uintptr_t*>(global) : 0;
        if (!service) return false;
        const uintptr_t serviceVtable = *reinterpret_cast<const uintptr_t*>(service);
        using RootFn = void*(__thiscall*)(void*);
        const uintptr_t root = reinterpret_cast<uintptr_t>(reinterpret_cast<RootFn>(*reinterpret_cast<const uintptr_t*>(serviceVtable + 4))(reinterpret_cast<void*>(service)));
        if (!root) return false;
        const uintptr_t rootVtable = *reinterpret_cast<const uintptr_t*>(root);
        using ChildFn = void*(__thiscall*)(void*,uint32_t,int);
        return !reinterpret_cast<ChildFn>(*reinterpret_cast<const uintptr_t*>(rootVtable + 0xF4))(reinterpret_cast<void*>(root), 0x95947678u, 0);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
// The world's own state alone, without asking the UI: safe from another thread (the UI is the main thread's)
inline bool WorldActive() {
    const uintptr_t global = GameAddr::Get(GameAddr::Id::WorldManagerPtr);
    if (!global) return false;
    __try {
        const uintptr_t world = *reinterpret_cast<const uintptr_t*>(global);
        return world && Active(*reinterpret_cast<const unsigned char*>(world + 0x41), *reinterpret_cast<const int*>(world + 0x1B4));
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
inline bool IsActive() {
    const uintptr_t global = GameAddr::Get(GameAddr::Id::WorldManagerPtr);
    if (!global) return false;
    __try {
        const uintptr_t world = *reinterpret_cast<const uintptr_t*>(global);
        // The world's own state first: the loading window is asked of the game's UI, which is still being built at start-up.
        return world && Active(*reinterpret_cast<const unsigned char*>(world + 0x41),
                               *reinterpret_cast<const int*>(world + 0x1B4)) && LoaderDismissed();
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// The screen effects' gate (Picture, Ambient Occlusion, Edge Smoothing: nothing on the main menu or on a
// load screen). IsActive alone opens during a load from the main menu (the script-driven load screen is not the startup
// window; a log: Bridgeport's load screen counted as playable), so the world must also have been drawn: at least
// kMinDepthWrites depth-writing draws per frame for kDrawnMs in a row (a load screen draws 0 .. 50, the world hundreds).
// Then it stays open while the world is active, so the save screen and the in-game menus keep the same look; it closes
// with the world (main menu) and waits for the next world to be drawn. Render thread only.
// 06/10 (Twinbrook load screen): the interactive load screen draws about 100 depth-writing draws for tens of seconds and
// Night Lighting's load-settled signal can stay on across a travel, so both are required: settled (when Night Lighting
// runs) and at least kMinDepthWrites draws for kDrawnMs (the world drew 185 .. 700 in the captures).
// 150 was tried on 06/10 and never opened in close views (fewer draws than that): 48 again, with the settled signal now
// reset by every new world (the Twinbrook load screen passed only because that signal was stale after a travel).
constexpr int kMinDepthWrites = 48;
constexpr unsigned long long kDrawnMs = 500; // was 1000: the effects came too late
constexpr unsigned long long kSparseMs = 2000; // a sparse world (fewer than kMinDepthWrites a frame): drawn this long, all else ready
// Night Lighting's "world live" (terrain drawn; true when it is off), registered by Depth Blur; null = not required. It was
// the after-load refresh ("load settled") until 06/10: effects came 3 .. 10 s after the map showed
inline bool (*g_loadSettled)() = nullptr;
}
namespace PostScene { int DepthWritesThisFrame(); int DepthWritesLastFrame(); bool Counting(); }
namespace WorldSession {
inline bool InWorld() {
    static bool open = false;
    static unsigned long long drawnSince = 0;
    static unsigned long long sparseSince = 0; // a sparse world drawn (see below)
    // a game build where the world manager was not found: the drawn world alone decides, every frame (no latch, so the
    // main menu's few draws close it again); never off for good
    const bool worldKnown = GameAddr::Get(GameAddr::Id::WorldManagerPtr) != 0;
    if (worldKnown && !IsActive()) {
        open = false;
        drawnSince = 0;
        sparseSince = 0;
        return false;
    }
    if (open && worldKnown) return true;
    const unsigned long long now = GetTickCount64();
    // the last complete frame's count (06/10: Night Lighting asks at Present, after the count was reset for the next
    // frame, so the current count read 0 there and restarted the timer every frame); no counting at all (no post-scene
    // effect on): the settled signal alone
    const int now0 = PostScene::DepthWritesThisFrame(), last = PostScene::DepthWritesLastFrame();
    const int writes = now0 > last ? now0 : last;
    // the drawn time runs alongside the settled wait (effects came about 3 s after the world appeared,
    // because the 1 s count only started once Night Lighting had settled)
    if (!PostScene::Counting() || writes >= kMinDepthWrites) {
        if (!drawnSince) drawnSince = now;
    } else drawnSince = 0;
    // A sparse world (an empty world in Edit in Game: 7 depth-writing draws a frame, bare terrain) never reaches
    // kMinDepthWrites: with the world manager read, the world active, its loading window gone and Night Lighting's "world
    // live", any drawn world counts once that lasted kSparseMs
    const bool settled = !g_loadSettled || g_loadSettled();
    if (worldKnown && settled && writes > 0) {
        if (!sparseSince) sparseSince = now;
    } else sparseSince = 0;
    open = settled && ((drawnSince && now - drawnSince >= kDrawnMs) || (sparseSince && now - sparseSince >= kSparseMs));
    return open;
}
// Diagnostics: every input of the gate above, as one log fragment (render thread)
inline std::string GateText() {
    const bool known = GameAddr::Get(GameAddr::Id::WorldManagerPtr) != 0;
    std::string s = std::string("world manager ") + (known ? "found" : "not found");
    if (known) s += std::string(", active ") + (IsActive() ? "yes" : "no") + ", loading window " + (LoaderDismissed() ? "gone" : "up");
    s += ", depth writes " + std::to_string(PostScene::DepthWritesLastFrame()) + " (needs " + std::to_string(kMinDepthWrites) + ")";
    s += std::string(", lighting settled ") + (!g_loadSettled ? "n/a" : g_loadSettled() ? "yes" : "no");
    return s;
}
}
