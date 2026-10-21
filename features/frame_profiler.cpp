#ifndef APEX_NO_DEV_TOOLS // (a build without the developer tools: ApexFlavorDefines=APEX_NO_DEV_TOOLS)
#include "build_flavor.h"
// Included in the unified binary; runtime developer mode gates activation.
// Frame-hitch profiler (see frame_profiler.h).
//
// ---- Frames ----
// Frame boundary = the first D3D9 registry Present hook. It is registered at priority -1000 (Priority is an int enum and the
// registry sorts by its value), so it runs before every module's Priority::First Present hook, in particular before
// PostScene resets the camera matrix it captured this frame. Frame time = time between two boundaries.
// Clock: RDTSC when the CPU reports an invariant TSC (calibrated against QPC over 20 ms when first enabled and refined from
// a long QPC baseline while running), otherwise QPC. RDTSC keeps the per-draw cost at a few ns.
//
// ---- Timed game functions (Steam 1.67.2, TS3W.exe, no ASLR) ----
// Every pattern was checked against re\TS3W.exe: unique in .text and matching at the Steam address; for Detours targets
// the first instructions it relocates were decoded by hand and no branch in .text lands inside them. On Steam the pattern
// must match at the Steam address (call sites: and the CALL must reach the Steam callee); on other builds it must match
// exactly once in .text. A target whose bytes do not match (another module detoured or patched it, other build) is
// skipped and listed in Advanced > Hooks. Everything is attached at the first frame boundary after the profiler is
// turned on (on the render thread, after the startup patches) and removed when it is turned off.
//   0x00EC9F00 render frame        __stdcall(1), ret 4    83 EC 20 56 E8..    callers 0xECA059 / 0xECA2E6 / 0xECAAFC.
//              BeginFrame (0x611620: device vtable +0xA4 BeginScene), scene + UI, then 0x611760 -> 0x611680, then at
//              +0xBA the inactive-window limiter (the 9 bytes Smooth Patch replaces with its DelayAfterFramePresentation call).
//   0x00611680 end frame + present __thiscall(3), ret 0xC  83 EC 20 56 8B F1.. device vtable +0xA8 (EndScene), +0x44
//              (Present); only caller 0x611766 (0x611760, itself called from 0xEC63C4 / 0xEC9EBA / 0xEC9FB5).
//   0x00C6C290 lot LOD scoring     __thiscall(4), ret 0x10 55 8B EC 83 E4 F0 81 EC 84 08..  from WorldManager::Update
//              (0xC6D6E4). Arg 2 = the camera point it stores at WorldManager+0x3A0 (used here as the camera-motion signal).
//   0x00AC20E0 lot detail request  __thiscall(1), ret 4    53 8A 5C 24 08 56 8B F1 8A 86 C1 00 00 00 3A C3..
//              if lot+0xC1 != (char)arg and lot+0xC9 (bulldozing) == 0: stores the flag and posts AddLotObjectsToScene
//              (0xAC1130, arg 1) or the demotion (arg 0) through PostRemoteMethodCall (0xABE9C0). Counted as promotions /
//              demotions with exactly that condition. Callers 0xC6A0F9, 0xC6C7C3, 0xC6C7CE, 0xAE61A7.
//   0x00AEB2E0 lot renderer update __thiscall(1), ret 4    56 8B F1 E8..       callers 0xC7CEDF, 0xAEB3F9, 0xAEB41C.
//   0x00AEA680 lot load stages     __thiscall(0), ret     81 EC BC 00 00 00 53 56..  budget 20 ms (35 / 2000 in some
//              states); only caller 0xAEB306 (inside 0xAEB2E0). Timed at that CALL (E8 rel32 -> hook), not at the entry:
//              Smooth Streaming detours the entry and verifies its bytes before installing. Site pattern at 0xAEB2F8
//              "80 7E 1E 00 75 0D 80 7E 1F 00 75 07 8B CE E8 ?? ?? ?? ?? 8D 8E 70 03 00 00 E8" (unique), CALL at +14.
//   0x00AD9E30 lot LOD switch      __thiscall(1), ret 4    56 8B F1 8B 4E 28 80 79 1E 00..  "World/LotImpostor/
//              LODOverrideHook"; calls 0xAEB3F0 (0xAEB2E0 twice + 0xADBAD0). Caller 0xADAEBC.
//   0x00ADBAD0 lot lighting setup  __thiscall(0), ret     53 55 56 8B F1 8B 46 40.. (pattern extended to 47 bytes: three
//              sibling loops 0xADB851 / 0xADBBA0 / 0xADBC30 share the first 45). Calls 0x6A80E0 and 0x6A4180 per level.
//   0x006A80E0 room lighting       __thiscall(0), ret     83 EC 08 55 56 57 8B F9..  per room: 0x6C54E0 + 0x6A3EC0
//              (synchronous room lightmap solve). Only caller 0xADBB37.
//   0x00ADB8F0 lot lighting update __thiscall(0), ret     83 EC 1C 56 8B F1 80 7E 18 00..  the budgeted per-frame room
//              lighting of normal lot loading (budget FUN_00ADB120, solves through FUN_006A8BA0). Only caller 0xAE4D2A
//              (FUN_00AE4CB0, called at the end of 0xAEB2E0).
//   0x00C845C0 terrain update      __thiscall(2), ret 8    callers 0xC6AF5A, 0xC6D68F, 0xC8525F. Timed at the per-frame
//              CALL in WorldManager::Update, 0xC6D68F (same reason as above). Site pattern at 0xC6D682
//              "8B 44 24 0C 50 8D 4C 24 14 51 8B 4E 58 E8 ?? ?? ?? ?? 80 BE 58 02 00 00 00" (unique), CALL at +13. Calls from
//              the other two callers are not timed; Night Terrain Relight's own calls from its Present hook count as
//              "Present hooks (mod)".
//   0x00E4A050 GC_try_to_collect   __cdecl(1)             83 3D ?? ?? ?? ?? 00 74 06 FF 15..  Boehm: lock, GC_init if
//              needed, 0xE49FB0 GC_try_to_collect_inner. Only caller 0xD819AA (MonoScriptHost::Simulate, simulation thread);
//              the thread that calls it is taken as the simulation thread.
//   0x00ABFAC0 Lot::UpdateObjectSceneNode __thiscall(3), ret 0xC (same pattern as LotStreamingOptimizations). Optional and
//              session-only (see "Not hooked by default" below).
// Not hooked on purpose:
//   - 0x006A3EC0 (room lightmap solve): LotEdgeLighting checks its entry bytes before installing and would refuse to
//     install while it is detoured; it is timed through its only caller 0x006A80E0 instead.
//   - Lot::AddLotObjectsToScene (0xAC1130): the Lot Streaming throttle overwrites its entry with a JMP.
//   - 0x00ABFAC0 by default: LotStreamingOptimizations finds it by a pattern that starts at the entry each time it
//     installs; while detoured that fails and the object throttle stays off. Opt-in, not saved, for measurements.
//   - WorldManager::Update (0xC6D570): detoured by LotStreamingOptimizations' map view blocker.
//   - The entries of 0xAEA680 / 0xC845C0: see above. Side effect of the terrain call-site redirect: Smooth Streaming, if
//     it installs while the profiler is on, sees another target at 0xC6D68F and skips the flush of its terrain queue
//     when it is later turned off (its own check, not fatal). Turn the profiler on after Smooth Streaming to avoid it.
//   - On non-Steam builds, GC Scheduler finds 0xC6C290 by a pattern from the entry; while the profiler is on it falls back
//     to its second camera source.
//
// ---- Attribution ----
// Every timed call pushes a frame on a per-thread stack; on return, the call's inclusive time is added to its parent's
// child time. Per category the profiler records:
//   - exclusive (self) time: the call's time minus the time of every timed call inside it. Exclusive times never overlap,
//     so on the render thread they add up to at most the frame time ("Unattributed" = the rest);
//   - inclusive time: counted only for the outermost call of that category on the stack (no double counting under
//     recursion); it includes nested timed calls (e.g. "Lot load stages" inclusive contains the room lighting inside it).
// Time is accumulated per thread (lock-free, owner-written relaxed atomics) and read at each frame boundary as deltas,
// bucketed by thread: render thread (the one calling Present), simulation thread (the one calling GC_try_to_collect)
// and other threads. Calls still open on the render thread are split at the boundary, so the render thread's time goes to
// the frame in which it was spent. Other threads' calls are attributed to the frame in which they return.
// Special splits on the render thread: 0x611680 counts as "EndScene + overlays" until the Present boundary and as
// "Present (driver)" after it; the part of 0x00EC9F00 after 0x611680 returns is "Frame limiter". So a frame interval
// holds the previous Present's wait, the limiter, the game's update and render and this frame's EndScene.
// CPU time = frame time - Present (driver) - Frame limiter.
//
// ---- D3D9 (through the D3D9Hooks registry only) ----
// Present: the start hook at priority -1000 and the end hook at +1000 bracket every other module's Present hooks ("Present
// hooks (mod)"), and every Present hook is timed by name while the profiler is on ("<name> (Present)").
// Every other chain (2026-09-29): the registry itself books its outermost dispatch on a thread as "D3D hooks (mod)"
// (BeginModTime / EndModTime around the whole chain, framework/d3d9_hooks.cpp), also when a module returns Skip / Block.
// Until then only the draws were bracketed by -1000 / +1000 hooks, and a dispatch cut short by Skip (every draw Night
// Lighting replaces) was dropped, so "mod D3D" undercounted about 5x (research\perf2\apexcost\report.md). A replaced
// draw's re-issue, and its driver call, happen inside the outer dispatch and are counted with it. The measurement
// includes the bookkeeping (two clock reads and a push / pop per outermost dispatch).
// Counts: DrawIndexedPrimitive / DrawPrimitive (game draws vs draws inside EndScene: overlays, Picture pass), primitives
// (a -1000 counting hook), CreateTexture / CreateRenderTarget / CreateVertexShader / CreatePixelShader, and SetTexture /
// Set*Shader / Set*ShaderConstantF / SetRenderTarget (counted in the registry's detours, D3D9Hooks::ReadStateCallCounts;
// shown with "Count state calls").
// The registry only calls hooks before the device method, so the duration of resource creation cannot be measured
// through it. Not exposed by the registry at all: CreateVertexBuffer / CreateIndexBuffer, Lock / Unlock, SetRenderState;
// DrawPrimitiveUP / DrawIndexedPrimitiveUP only through ExtraHooks' single observer slot, which Frame Capture owns, so not
// used. Per-hook-name timing of the draw hooks: Advanced > "Per-hook registry timing" (frame_profiler.h).
//
// ---- Main loop, services, jobs, waits, I/O (engine study, Steam 1.67.2; same verification as above) ----
// The render thread is the main thread; its loop (0xECA960) runs app state 0xEC6C30, ServiceManager::Update 0x588E00 ->
// 0x59ED20 (every service's vtable +0x1C update), SceneCaptureManager 0x9DE140, Scene::BeginFrame 0x6EBB70, the render
// frame 0xEC9F00, Scene::EndFrame 0x6E8810 and the clock tick 0x5943F0.
//   0x0059ED20 / 0x0059ED70  ServiceManager main / simulation loops (thiscall(float,float), ret 8): replaced by an
//              identical C++ walk (the pattern is the whole 0x44-byte body) that times each service call, keyed by the
//              update function (names from the study for the known ones, else address + vtable).
//   0x00599720 ExecuteJob (thiscall(job), ret 4): every thread; on the render thread keyed by [job+0x10], or for remote
//              calls (job function 0x7D9840, bytes checked) by the method: vtable+0x10 of [job+0x14], or [obj+0x10] for
//              PostRemoteMethodCall objects (vtable 0x10650C4, checked at 0xABEA0A).
//   0x0059A220 WaitForJob (thiscall(job), ret 4): render thread, keyed by the job waited for.
//   0x004E16F0 Mutex::Lock, 0x004E2760 Semaphore::Wait (thiscall(timeout*), ret 4): every thread pays two clock reads;
//              render-thread calls that blocked > 0.1 ms are booked (as a leaf under the running timed call), keyed by the
//              caller's return address. Semaphore waits inside WaitForJob count as the job wait.
//   0x004DB850 FileStream::Read (ret 8), 0x004DB8E0 Flush (ret), 0x004EC010 RefPack read (stdcall(5), ret 0x14):
//              timed on the render thread; bytes read counted.
//   0x006EBB70 / 0x006E8810 / 0x009DE140 / 0x00EC6C30 / 0x005943F0 / 0x00AD97E0: timed (see kCats).
// The functions many threads call (the loops, ExecuteJob, WaitForJob, Mutex, Semaphore, file I/O) are not attached with
// Detours but by hand with all other threads suspended and checked (see AttachSafe).
// Mutex::Lock is timed only with the Advanced option "Time the Mutex::Lock hook" (off by default since 2026-09-28): the
// resource lookup below calls it ~580 times per scan, so its two clock reads distorted exactly the path being measured.
//
// ---- Counters (anti-stutter plan, research\perf2\plan.md section 8; addresses in framework/game_addresses.h) ----
// Six categories timed on every thread they run on, each with per-frame calls / inclusive ms per thread bucket, the
// longest single call (render thread / other threads) and an extra count. Verified in engine_map\full.asm (Steam 1.67.2):
//   0x004AFFC0 ResourceMgr::FindProvider  thiscall(key*, cookie*), ret 8; push ecx/ebx/ebp/esi/edi, key = [esp+18h],
//              cookie out = [esp+20h] after the pushes; returns the provider (0 = not found). No direct CALL: only virtual,
//              through slot +0x40 of the base vtable 0x00FB2DA0 and of the derived one 0x00FFE250 (0x00FB2DE0 / 0x00FFE290),
//              and the wrapper 0x004AFDA0 (slot +0x44). Hooked by swapping both slots (one aligned 4-byte store each, no code
//              byte changes). Extra: packages probed = index of the returned provider in [this+0x30, this+0x34) (8-byte
//              entries) + 1, or all of them on a miss; misses.
//   0x006E4130 scene pending-node drain    thiscall(), ret; splices the list [this+0x20] out, zeroes [this+0x18] and adds
//              1 per node processed (0x006E41EF), so [this+0x18] read right after the call = nodes processed. Timed at Scene::BeginFrame's CALL 0x006EBC49 only (the other 5
//              callers are not per-frame). Extra: nodes. Since 2026-09-29 the CALL is shared through framework/call_chain.h:
//              the profiler is the outer layer, the scene node budget (features/scene_budget.h) the inner one when on;
//              extra "deferred" = nodes the budget left queued for the next frames (SceneBudget::TakeDrainNote).
//   0x004EC200 RefPack stream write        thiscall(src, size, dst, capacity, flags), ret 0x14 (uses [ecx+4] = allocator;
//              the plan's "stdcall" was wrong). dst == 0 with flags & 1 = size bound only (no work, not timed). Else
//              FUN_004EC0A0 -> FUN_004EB750 (<= 16 KB) or FUN_004EBB90; returns the compressed size. Only referenced by the
//              stream vtable slot 0x00FB901C; FUN_004EC0A0 / 004EBB90 / 004EB750 have no other callers. Hooked through that
//              slot as the outer layer of framework/slot_chain.h (site RefPackCompress; the fast compressor of
//              features/fast_refpack.h is the inner layer when on). Extra: bytes in (size), bytes out (return value).
//   0x006152F0 / 0x006154B0 DXT1 / DXT5 encoders  cdecl(dst*, src*) (callers "add esp,8"); dst = {ptr, width +4, height
//              +8, pitch +0xC}. 8 + 7 direct callers on several threads: since 2026-09-29 hooked at the entry through
//              framework/entry_chain.h (the prologue 55 8B EC 83 E4 F0 moved to a trampoline, a JMP written with all other
//              threads suspended), the profiler being the outer layer and the fast encoder (features/fast_dxt.h) the inner
//              one. Extra: pixels (width x height).
//   0x00C62D40 object lookup by ID         thiscall(idLo, idHi, int* visited), ret 0xC; ecx passed on to FUN_00C60D30. 233
//              call sites (script natives on the simulation thread, lot lighting, camera). Since 2026-09-29 hooked at the
//              entry through framework/entry_chain.h (site ObjectById: the 8 prologue bytes 8B 44 24 0C 8B 54 24 08 moved to a
//              trampoline, a JMP written with all other threads suspended), the profiler being the outer layer and the object
//              lookup index (features/object_index.h) the inner one. Extra: "from index" (ObjectIndex::TakeLookupNote).
//   0x006A8BA0 lot room solve              thiscall(timer*, float budget), ret 8; x87 stack empty at the call and on return.
//              Its only caller is the lot lighting update FUN_00ADB8F0 (render thread): timed at that CALL, 0x00ADB9AD.
//              ecx = one level object of the lot (the deque at manager+0x24..0x40), so calls = lot levels updated (not rooms).
//   0x0068B810 wall AO step                thiscall(stopwatch*, float budget), ret 8; only reference: slot +0x1C of the solver
//              vtable 0x00FF0594 (0x00FF05B0), called by the solver driver 0x00688920 from the room solve above. One call =
//              one pass over every outdoor wall of a level (no time check). Hooked through that slot as a layer of
//              framework/slot_chain.h inside the wall shading gate (features/lot_lighting_motion.h, Layer::Gate, which must
//              be outermost), so with the gate on only the passes that run are timed. Extra: none (calls = passes).
//   0x004B1AE0 / 0x00736660 ResourceMgr / ResourceSystem::GetKeyList  thiscall(vector* out, filter*, bool unique), ret 0xC;
//              only references: slot +0x20 of the base (0x00FB2DC0) and derived (0x00FFE270) vtables; the derived one calls
//              the base directly (no double count). Outer layer of both slots; the file list cache (features/resource_cache.h)
//              is inside. Extras: keys (the out vector's growth in 16-byte keys, before the derived sort / unique), packages
//              answered from the file list cache (kXListCached).
//   The FindProvider slots are shared with the resource lookup cache through framework/slot_chain.h (the profiler is the
//   outer layer whichever installs first); Hook_FindProvider calls SlotChain::Next and adds "from cache" (kXCacheHits)
//   and "absent from cache" (kXCacheAbsent, "Remember missing files").
// No branch in .text lands inside any replaced prologue or CALL (checked in full.asm). S3SS and the other installed ASIs
// touch none of these sites (plan section 6). They are attached only once the game-address scan has run
// (GameAddr::Scanned, first Present + 1 s), on every build, so the scan's self-check never sees these hooks; the two CALLs
// are written with every other thread suspended (WriteCallSuspended), the lot lighting update being also reachable from
// the lot impostor builder path.
//
// ---- Sampling (Advanced, off by default) ----
// For the time no timed function covers ("Unattributed"): a sampler thread pauses the render and/or simulation thread
// g_sampleHz times a second (default 2000; high-resolution waitable timer), records EIP and the TS3W return addresses
// found on the first 4 KB of its stack, and the render thread assigns the samples to frame intervals. Per hitch:
// share of samples per code class (TS3W, DXVK/driver, system, Apex, other ASI, other), the top 8 code locations (TS3W
// function start guessed from the int3 padding, or the module), the top 8 TS3W call sites on the stack and the TS3W
// callers of samples in system code (waits, heap, I/O). Session tables compare hitch frames with other frames.
// Cost: each sample pauses the target for the SuspendThread / GetThreadContext / 4 KB copy / ResumeThread round trip
// (typically 5-30 us under WOW64, measured and shown in the UI), i.e. roughly 1-6% of the sampled thread at 2000 Hz, plus
// the same order of CPU on the sampler's core. The render thread's per-frame work is a table update per sample.
//
// ---- Output ----
// Render thread: rings (graph, averages, median window, the last 200 hitches), histogram for percentiles, totals.
// Hitches go to a single-producer / single-consumer queue; a writer thread appends them to ApexRadiance_Hitches.txt at most
// once per second (and on "Save report now"). Nothing is written or flushed on the render thread.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "frame_profiler.h"
#include "lot_load_analysis.h"
#include "d3d9_hooks.h"
#include "game_addresses.h"
#include "post_scene.h"
#include "memory_patch.h"
#include "shader_cache.h"
#include "slot_chain.h"
#include "entry_chain.h"
#include "call_chain.h"
#include "resource_cache.h"
#include "scene_budget.h"
#include "object_index.h"
#include "lot_lighting_motion.h"
#include "fast_crc.h"
#include "fast_memory.h"
#include "address_space.h"
#include "apex_config.h"
#include "apex_paths.h"
#include "apex_log.h"
#include "hook_guard.h"
#include "imgui.h"
#include "ui/widgets.h"
#include <toml++/toml.hpp>
#include <intrin.h>
#include <tlhelp32.h>
#include <psapi.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <format>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr const char* kHookName = "FrameProfiler";
constexpr int kPrioStart = -1000; // below Priority::First: before every other module's hooks
constexpr int kPrioEnd = 1000;    // above Priority::Last: after every other module's hooks

// ---- categories ----
enum Cat : int {
    kRenderFrame,
    kEndScene,
    kPresentHooks,
    kPresentDriver,
    kFrameLimiter,
    kModD3DHooks,
    kLotLodScoring,
    kLotDetailRequest,
    kLotRendererUpdate,
    kLotLoadStages,
    kLotViewSwitch,
    kLotLightingInit,
    kRoomLighting,
    kLotLightingUpdate,
    kLotObjectBuild,
    kTerrainUpdate,
    kScriptGC,
    kSceneBeginFrame,
    kSceneEndFrame,
    kSceneCapture,
    kAppState,
    kClockTick,
    kImpostorPump,
    kService,
    kJob,
    kJobWait,
    kMutexWait,
    kSemWait,
    kFileRead,
    kFileFlush,
    kRefPackRead,
    kLampRefresh, // mod time: Night Lighting's lamp list refresh (FrameProfiler::ModTime::LampRefresh)
    kReissuedDraw, // the driver's time for a draw Night Lighting re-issued, inside "D3D hooks (mod)" (FrameProfiler::ModTime::ReissuedDraw)
    // counters (research\perf2\plan.md section 8): timed on every thread, with per-bucket calls, longest call and an extra count
    kResLookup,
    kScenePending,
    kRefPackCompress,
    kDxtEncode,
    kObjectLookup,
    kLotRoomSolve,
    kWallAo,
    kKeyList,
    kTexCreate,
    kTexFill,
    kCatCount
};
constexpr int kFirstGameCat = kLotLodScoring;
constexpr int kFirstCounterCat = kResLookup;
constexpr int kNC = kCatCount - kFirstCounterCat; // number of counters
inline bool IsCounterCat(int c) { return c >= kFirstCounterCat && c < kCatCount; }

// Extra counts of the counters (per thread, like the times)
// kXCacheHits: resource lookups answered by the resource lookup cache (features/resource_cache.h), whose FindProvider
// layer sits inside the profiler's (framework/slot_chain.h); their kXPackages count is the packages the cache asked.
// kXCacheAbsent: of those, answered "no package holds it" ("Remember missing files"). kXKeys: keys a key list call
// returned (the out vector's growth); kXListCached: packages whose keys the file list cache gave from memory.
// kXDeferred: scene nodes the scene node budget (features/scene_budget.h) left queued; kXIndexHits: object lookups
// answered by the object lookup index (features/object_index.h). Both layers sit inside the profiler's.
enum Extra : int {
    kXPackages, kXMisses, kXNodes, kXBytesIn, kXBytesOut, kXPixels, kXCacheHits, kXCacheAbsent, kXKeys, kXListCached, kXDeferred, kXIndexHits,
    kExtraCount
};

struct CatInfo {
    const char* name;
    const char* hint;
};
const CatInfo kCats[kCatCount] = {
    {"Render frame (game)", "FUN_00EC9F00, the game's frame render: scene and UI submission. Self time only (end of frame, Present, limiter and the mod's D3D hooks are separate)."},
    {"EndScene + overlays", "FUN_00611680 up to Present: IDirect3DDevice9::EndScene, which also runs the Apex overlay (menu), the Picture pass and, when installed, official S3SS's overlay."},
    {"Present hooks (mod)", "The D3D9 hook registry's Present hooks of all Apex modules (includes this profiler's own per-frame bookkeeping)."},
    {"Present (driver)", "IDirect3DDevice9::Present itself (DXVK): submission, GPU back-pressure and vsync wait. High = GPU or vsync bound."},
    {"Frame limiter", "After Present in FUN_00EC9F00: the Smooth Patch frame limiter, or the game's own ~30 ms sleep while the window is inactive."},
    {"D3D hooks (mod)", "Time inside the D3D9 hook registry: every outermost dispatch of the draw, state and resource-creation chains (all Apex modules' "
                        "callbacks, also when one skips the game's call; a draw Night Lighting replaces is re-issued inside it, its driver call booked as \"Re-issued draw (driver)\"). "
                        "Sampled: one outermost dispatch in 16 is timed and counted 16 times. Present hooks are separate."},
    {"Lot LOD scoring", "FUN_00C6C290 (from WorldManager::Update): scores the lots around the camera and requests detailed view or demotion."},
    {"Lot detail request", "FUN_00AC20E0: sets a lot's detailed-view flag and posts Lot::AddLotObjectsToScene (run inline when posted from its own thread)."},
    {"Lot renderer update", "FUN_00AEB2E0: per-lot renderer update; drives the lot load state machine."},
    {"Lot load stages", "FUN_00AEA680: the lot load state machine, running load stages for up to a 20 ms budget per call."},
    {"Lot LOD switch", "FUN_00AD9E30 (World/LotImpostor/LODOverrideHook): finishes a lot's switch at once (FUN_00AEB3F0: two renderer updates + lighting)."},
    {"Lot lighting setup", "FUN_00ADBAD0: per lot level, the room lighting below and the lot light solvers (FUN_006A4180)."},
    {"Room lighting + solve", "FUN_006A80E0: for every room, the room light update (FUN_006C54E0) and the synchronous room lightmap solve (FUN_006A3EC0)."},
    {"Lot lighting update", "FUN_00ADB8F0 (lot renderer update -> FUN_00AE4CB0): the per-frame room lighting of a lot, time-boxed by FUN_00ADB120 (5-30 ms), solving rooms through FUN_006A8BA0."},
    {"Lot object scene nodes", "Lot::UpdateObjectSceneNode (FUN_00ABFAC0): one object's scene node (AddLotObjectsToScene, Lot Streaming throttle). Optional, see Advanced."},
    {"Terrain update", "FUN_00C845C0 (from WorldManager::Update): terrain update / rebuild (Terrain/WorldLeafTexture)."},
    {"Script GC", "GC_try_to_collect (FUN_00E4A050): the explicit collection in MonoScriptHost::Simulate, on the simulation thread."},
    {"Scene::BeginFrame", "FUN_006EBB70 (main loop): submits the scene jobs and hands the render queue over; can wait for the previous frame's job."},
    {"Scene::EndFrame", "FUN_006E8810 (main loop): waits for the scene jobs and pops the render queue."},
    {"Scene capture", "FUN_009DE140, SceneCaptureManager: off-screen thumbnail / photo captures and their read-back."},
    {"App state update", "FUN_00EC6C30 (main loop): application state machine."},
    {"Game clock tick", "FUN_005943F0 (main loop): game clock."},
    {"Lot impostor pump", "FUN_00AD97E0: builds a lot impostor by pumping the services until its job is done (no Present meanwhile)."},
    {"Services (self)", "Service updates (ServiceManager, FUN_0059ED20 / FUN_0059ED70): the services' own code, outside the timed functions inside them. Per service in Advanced."},
    {"Jobs (self)", "JobManager::ExecuteJob (FUN_00599720): jobs run on this thread, outside the timed functions inside them. Per job in Advanced."},
    {"Wait for job", "JobManager::WaitForJob (FUN_0059A220): blocked until another thread finishes a job (jobs it runs itself are counted as jobs)."},
    {"Mutex wait", "EA::Thread::Mutex::Lock (FUN_004E16F0) calls that blocked for more than 0.1 ms (render thread only)."},
    {"Semaphore wait", "EA::Thread::Semaphore::Wait (FUN_004E2760) calls that blocked for more than 0.1 ms (render thread only, outside WaitForJob)."},
    {"File read", "FileStream::Read (FUN_004DB850, ReadFile) on the render thread."},
    {"File flush", "FileStream::Flush (FUN_004DB8E0, FlushFileBuffers) on the render thread."},
    {"RefPack read", "RefPack stream read + decompression (FUN_004EC010) on the render thread."},
    {"Lamp refresh (mod)", "Night Lighting's lamp list refresh, every 20 frames inside its Present hook (lot_light_bridge.cpp OnPresent): light enumeration, the "
                           "lit outdoor lamps for roofs / water / objects and the lot lamp change tracking. Part of the mod time."},
    {"Re-issued draw (driver)", "The driver call of a draw Night Lighting replaced and re-issued (lot_light_bridge.cpp), taken out of \"D3D hooks (mod)\": the game "
                                "would have paid it for its own call. Sampled with the registry's dispatches (one in 16, scaled)."},
    {"Resource lookup", "ResourceMgr::FindProvider (FUN_004AFFC0, through its two vtable slots): which package holds a resource key. It asks every registered package "
                        "in turn (two critical sections and a hash probe each; about 290 packages with the user's mods). Materials, async-load finalize jobs, CAS "
                        "and lot loading go through it. All threads."},
    {"Scene pending nodes", "FUN_006E4130 called by Scene::BeginFrame (CALL 0x006EBC49): processes every scene node queued since the last frame, with no budget "
                            "(materials resolving their textures, models). Nodes counted."},
    {"RefPack compress", "RefPack stream write (FUN_004EC200, stream vtable slot 0x00FB901C): compresses a resource written into the in-memory caches (sim and object "
                         "compositors, world caches); above 16 KB FUN_004EBB90, which allocates and clears a 256 KB table per call. Bytes in / out counted. All threads."},
    {"DXT encode", "CPU DXT1 / DXT5 encoders (FUN_006152F0 / FUN_006154B0): terrain bakes, compositor output, captures. Pixels counted. All threads."},
    {"Object lookup by ID", "FUN_00C62D40: finds an object by its 64-bit ID with a recursive walk of the whole object tree (no index). Script natives (simulation "
                            "thread), lot lighting, camera. All threads."},
    {"Lot room solve", "FUN_006A8BA0 called by the lot lighting update (CALL 0x00ADB9AD): relights the dirty rooms of one level of a lot within the per-frame "
                       "budget of FUN_00ADB120 (scaled down while the camera moves when Lot Lighting While Moving is on). Calls = lot levels updated."},
    {"Wall AO pass", "FUN_0068B810 through its vtable slot 0x00FF05B0: the wall ambient-occlusion step of one lot level, run inside the room solve. It shades every "
                     "outdoor wall of the level in one go with no time check. With Wall Shading While Moving on, only the passes that run are timed (the gate "
                     "is outside this counter)."},
    {"Key list", "ResourceMgr::GetKeyList (FUN_004B1AE0 / FUN_00736660, vtable slots 0x00FB2DC0 / 0x00FFE270): lists every key matching a filter by walking the "
                 "index of every package (CAS asks it for all keys of a type). Keys returned counted; with Faster File Lists on, packages answered from memory. All threads."},
    {"Texture create", "FUN_0060CEA0 called by the DDS texture loader (CALL 0x0060E1DC; texture load finalize job 0x007297C0): IDirect3DDevice9::CreateTexture "
                       "of a loaded texture (MANAGED pool; in DXVK a zero-filled, mapped CPU copy). Pixels of level 0 counted; by size in the report. All threads."},
    {"Texture fill", "FUN_0060D290 called by the DDS texture loader (CALL 0x0060E1FF): locks every mip level of the new texture and copies the file's rows into it. "
                     "By size in the report. All threads."},
};

// ---- settings ----
std::atomic<bool> g_enabled{false};
std::atomic<float> g_mult{2.0f};    // hitch = frame > max(g_mult x median, g_floorMs)
std::atomic<float> g_floorMs{8.0f};
std::atomic<bool> g_countState{true};
std::atomic<bool> g_writeFile{true};
std::atomic<bool> g_regTiming{false};       // Advanced option
std::atomic<bool> g_regTimingActive{false}; // option && enabled, read by the registry instrumentation
std::atomic<bool> g_sampleRender{false};    // statistical sampler: render thread
std::atomic<bool> g_sampleSim{false};       // statistical sampler: simulation thread
std::atomic<int> g_sampleHz{2000};
std::atomic<bool> g_timeMutex{false};       // Advanced "Time the Mutex::Lock hook" (off: the lookup path is measured undistorted)
std::atomic<bool> g_objectBuildWanted{false};           // session only (see the header comment)
bool g_stateHooksActive = false;            // the state-call counters are registered

// ---- clock ----
bool g_useTsc = false; // decided once, before the first hook is attached, never changed afterwards
double g_qpcFreq = 1.0;
double g_msPerTick = 0.0;
uint64_t g_calQpc0 = 0, g_calTsc0 = 0;
uint64_t g_blockTicks = ~0ull; // set by InitClock
std::once_flag g_clockOnce;

uint64_t Qpc() {
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    return static_cast<uint64_t>(q.QuadPart);
}

inline uint64_t Now() {
    if (g_useTsc) return __rdtsc();
    return Qpc();
}

void InitClock() {
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    g_qpcFreq = static_cast<double>(f.QuadPart);
    double tickFreq = g_qpcFreq;
    int r[4] = {};
    __cpuid(r, static_cast<int>(0x80000000));
    bool invariant = false;
    if (static_cast<unsigned>(r[0]) >= 0x80000007u) {
        __cpuid(r, static_cast<int>(0x80000007));
        invariant = (r[3] & (1 << 8)) != 0;
    }
    if (invariant) {
        const uint64_t q0 = Qpc();
        const uint64_t t0 = __rdtsc();
        const uint64_t qEnd = q0 + static_cast<uint64_t>(g_qpcFreq / 50.0); // 20 ms
        uint64_t q1 = q0;
        while ((q1 = Qpc()) < qEnd) YieldProcessor();
        const uint64_t t1 = __rdtsc();
        const double f2 = static_cast<double>(t1 - t0) * g_qpcFreq / static_cast<double>(q1 - q0);
        if (f2 > 1e8) {
            tickFreq = f2;
            g_calQpc0 = q0;
            g_calTsc0 = t0;
            g_useTsc = true;
        }
    }
    g_msPerTick = 1000.0 / tickFreq;
    g_blockTicks = static_cast<uint64_t>(tickFreq / 10000.0); // 0.1 ms: a Mutex / Semaphore call slower than this blocked
}

// Render thread: refines the TSC rate from the long baseline taken at InitClock
void RefineClock() {
    if (!g_useTsc) return;
    const uint64_t q = Qpc();
    const uint64_t t = __rdtsc();
    if (q > g_calQpc0 + static_cast<uint64_t>(g_qpcFreq) && t > g_calTsc0)
        g_msPerTick = 1000.0 * static_cast<double>(q - g_calQpc0) / (g_qpcFreq * static_cast<double>(t - g_calTsc0));
}

// ---- per-thread accumulators and call stacks ----
constexpr int kMaxSlots = 128;
constexpr int kMaxDepth = 32;
constexpr uintptr_t kStaleMargin = 64; // bytes; see the D3D9 note in the header comment

enum FrameFlags : uint8_t { kOuter = 1, kSwitchAtSplit = 2, kDispatch = 4 };

struct Frame {
    uint64_t start;
    uint64_t child; // inclusive time of the timed calls made inside this one
    const void* sp; // an address in the pushing function's stack frame: identity and stack depth
    uint8_t cat;
    uint8_t flags;
};

enum : uint8_t { kScaleRoot = 8 }; // FrameFlags: a sampled frame whose time (and that of every frame inside it) is booked x scale

struct alignas(64) ThreadSlot {
    std::atomic<DWORD> tid{0};
    // written by the owning thread only (load + store, no locked instruction), read at frame boundaries
    std::atomic<uint64_t> excl[kCatCount]{};
    std::atomic<uint64_t> incl[kCatCount]{};
    std::atomic<uint32_t> calls[kCatCount]{};
    std::atomic<uint64_t> extra[kExtraCount]{}; // counters' extra counts (packages probed, nodes, bytes, pixels...)
    // owner only
    Frame stack[kMaxDepth]{};
    int depth = 0;
    uint32_t scale = 1; // inside a sampled frame (kScaleRoot): the sample's weight, else 1
    int open[kCatCount]{};
    uint32_t presentFnExits = 0;
    uint64_t lastPresentFnExit = 0;
};

ThreadSlot g_slots[kMaxSlots];
std::atomic<int> g_slotsUsed{0};
thread_local ThreadSlot* t_slot = nullptr;
thread_local bool t_noSlot = false;
std::atomic<DWORD> g_renderTid{0}; // render thread (written in the Present hook, read by the sampler too)
std::atomic<DWORD> g_simTid{0}; // the thread calling GC_try_to_collect

ThreadSlot* GetSlot() {
    if (ThreadSlot* s = t_slot) return s;
    if (t_noSlot) return nullptr;
    const DWORD tid = GetCurrentThreadId();
    for (int i = 0; i < kMaxSlots; i++) {
        DWORD expected = 0;
        if (g_slots[i].tid.compare_exchange_strong(expected, tid)) {
            int used = g_slotsUsed.load();
            while (used < i + 1 && !g_slotsUsed.compare_exchange_weak(used, i + 1)) {}
            t_slot = &g_slots[i];
            return t_slot;
        }
    }
    t_noSlot = true; // more than kMaxSlots threads ran timed code: this one is not timed
    return nullptr;
}

inline void Add64(std::atomic<uint64_t>& a, uint64_t v) {
    a.store(a.load(std::memory_order_relaxed) + v, std::memory_order_relaxed);
}
inline void Add32(std::atomic<uint32_t>& a, uint32_t v) {
    a.store(a.load(std::memory_order_relaxed) + v, std::memory_order_relaxed);
}

inline void Discard(ThreadSlot* s) {
    Frame& f = s->stack[--s->depth];
    s->open[f.cat]--;
    if (f.flags & kScaleRoot) s->scale = 1;
}

// Drops registry dispatches left open by a hook that returned Skip / Block: their frame lies at the same or a
// shallower stack depth than sp (a live parent dispatch is always well above a nested push).
inline void CleanStale(ThreadSlot* s, const void* sp) {
    const uintptr_t p = reinterpret_cast<uintptr_t>(sp);
    while (s->depth > 0) {
        const Frame& t = s->stack[s->depth - 1];
        if (!(t.flags & kDispatch) || reinterpret_cast<uintptr_t>(t.sp) >= p + kStaleMargin) break;
        Discard(s);
    }
}

inline int Push(ThreadSlot* s, int cat, const void* sp, uint8_t flags) {
    CleanStale(s, sp);
    if (s->depth >= kMaxDepth) return -1;
    Frame& f = s->stack[s->depth];
    f.cat = static_cast<uint8_t>(cat);
    f.sp = sp;
    f.child = 0;
    f.flags = static_cast<uint8_t>(flags | (s->open[cat] == 0 ? kOuter : 0));
    s->open[cat]++;
    f.start = Now();
    return s->depth++;
}

// carve: time at the end of the call that belongs to carveCat instead (the frame limiter inside FUN_00EC9F00)
// inclOut / selfOut (optional): the call's inclusive and self time, 0 when the frame was dropped
inline void Pop(ThreadSlot* s, int idx, const void* sp, uint64_t end, uint64_t carve, int carveCat, uint64_t* inclOut = nullptr, uint64_t* selfOut = nullptr) {
    if (inclOut) *inclOut = 0;
    if (selfOut) *selfOut = 0;
    if (idx >= s->depth || s->stack[idx].sp != sp) return; // dropped (stale cleanup or split bookkeeping)
    while (s->depth - 1 > idx) Discard(s);
    Frame& f = s->stack[idx];
    const uint64_t inclT = end > f.start ? end - f.start : 0;
    const uint64_t rest = inclT > f.child ? inclT - f.child : 0;
    if (carve > rest) carve = rest;
    // a sampled frame and the frames inside it stand for `scale` calls (BeginModTimeSampled); its parent, outside the
    // sample, loses the scaled time, while frames inside it pass their real time to the sampled frame
    const uint32_t k = s->scale;
    Add64(s->excl[f.cat], (rest - carve) * k);
    if (carve) Add64(s->excl[carveCat], carve * k);
    if (f.flags & kOuter) Add64(s->incl[f.cat], inclT * k);
    Add32(s->calls[f.cat], k);
    s->open[f.cat]--;
    s->depth = idx;
    const bool root = (f.flags & kScaleRoot) != 0;
    if (root) s->scale = 1;
    if (idx > 0) s->stack[idx - 1].child += root ? inclT * k : inclT;
    if (inclOut) *inclOut = inclT;
    if (selfOut) *selfOut = rest - carve;
}

// Render thread, at the frame boundary: books the time spent so far by every open call and restarts them at `now`
void Split(ThreadSlot* s, uint64_t now) {
    for (int i = 0; i < s->depth; i++) {
        const Frame& f = s->stack[i];
        const uint64_t segEnd = (i + 1 < s->depth) ? s->stack[i + 1].start : now;
        const uint64_t span = segEnd > f.start ? segEnd - f.start : 0;
        Add64(s->excl[f.cat], span > f.child ? span - f.child : 0);
        if (f.flags & kOuter) Add64(s->incl[f.cat], now > f.start ? now - f.start : 0);
    }
    for (int i = 0; i < s->depth; i++) {
        Frame& f = s->stack[i];
        f.start = now;
        f.child = 0;
        if (f.flags & kSwitchAtSplit) { // FUN_00611680: EndScene until Present, Present afterwards
            s->open[f.cat]--;
            f.cat = kPresentDriver;
            f.flags = static_cast<uint8_t>(f.flags & ~(kSwitchAtSplit | kOuter));
            if (s->open[kPresentDriver] == 0) f.flags |= kOuter;
            s->open[kPresentDriver]++;
        }
    }
}

struct Scope {
    ThreadSlot* s;
    int idx;
    explicit Scope(int cat, uint8_t flags = 0) : s(GetSlot()), idx(s ? Push(s, cat, this, flags) : -1) {}
    ~Scope() {
        if (idx >= 0) Pop(s, idx, this, Now(), 0, 0);
    }
    Scope(const Scope&) = delete;
    Scope& operator=(const Scope&) = delete;
};

// ---- D3D9 counters: touched inside registry hooks. The draw counts come from the render thread's lock-free draw
// dispatch (a draw from another thread runs under the registry lock and may race with it: a statistic, and
// D3D9Hooks::OffThreadDispatches() shows whether it ever happens); the Create* counts under the registry lock. ----
struct D3DCounts {
    uint32_t dip = 0, dp = 0, gameDraws = 0, endFrameDraws = 0;
    uint64_t prims = 0;
    uint32_t createTex = 0, createVS = 0, createPS = 0, createRT = 0;
};
D3DCounts g_d3d;
D3D9Hooks::StateCallCounts g_stateBase; // render thread: the registry's state-call counters at the last frame boundary

// ---- game-side counters ----
std::atomic<uint32_t> g_lotsPromoted{0}, g_lotsDemoted{0};
std::atomic<int> g_camPointMoved{0}, g_camPointSeen{0};
float g_camPoint[3] = {}; // written only by the thread calling FUN_00C6C290 (WorldManager::Update)
bool g_camPointValid = false;
float g_camVp[4][4] = {}; // render thread
bool g_camVpValid = false;

// ---- keyed tables: services, jobs, waits ----
inline bool IsRenderThread() {
    return __readfsdword(0x24) == g_renderTid.load(std::memory_order_relaxed); // TEB ClientId.UniqueThread
}

inline uint32_t KeyHash(uint32_t k) {
    k ^= k >> 16;
    k *= 0x7feb352dU;
    k ^= k >> 15;
    k *= 0x846ca68bU;
    k ^= k >> 16;
    return k;
}

// Open-addressing table of times per key (key 0 = empty). No allocation; single writer.
template <uint32_t N> struct TimeTable {
    uint32_t keys[N];
    uint64_t incl[N];  // ticks
    uint64_t self[N];  // ticks
    uint32_t calls[N];
    uint32_t aux[N];   // first non-zero aux seen: services = vtable, waits = kind
    uint32_t used;
    uint32_t lost;
    void Clear() {
        if (used || lost) std::memset(this, 0, sizeof *this);
    }
    int Find(uint32_t key) const {
        if (!key) key = 1;
        uint32_t i = KeyHash(key) & (N - 1);
        for (uint32_t probe = 0; probe < N; probe++, i = (i + 1) & (N - 1)) {
            if (keys[i] == key) return static_cast<int>(i);
            if (!keys[i]) return -1;
        }
        return -1;
    }
    void Add(uint32_t key, uint64_t in, uint64_t se, uint32_t n, uint32_t auxv) {
        if (!key) key = 1;
        uint32_t i = KeyHash(key) & (N - 1);
        for (uint32_t probe = 0; probe < N; probe++, i = (i + 1) & (N - 1)) {
            if (keys[i] != key) {
                if (keys[i]) continue;
                if (used * 10 >= N * 9) break;
                keys[i] = key;
                used++;
            }
            incl[i] += in;
            self[i] += se;
            calls[i] += n;
            if (!aux[i]) aux[i] = auxv;
            return;
        }
        lost++;
    }
    template <uint32_t M> void Merge(const TimeTable<M>& o) {
        for (uint32_t i = 0; i < M; i++)
            if (o.keys[i]) Add(o.keys[i], o.incl[i], o.self[i], o.calls[i], o.aux[i]);
    }
    // Indices of the maxN largest inclusive times, descending
    int Top(int* out, int maxN) const {
        int n = 0;
        for (uint32_t i = 0; i < N; i++) {
            if (!keys[i]) continue;
            if (n == maxN && incl[i] <= incl[out[n - 1]]) continue;
            int pos = n < maxN ? n++ : maxN - 1;
            while (pos > 0 && incl[out[pos - 1]] < incl[i]) {
                out[pos] = out[pos - 1];
                pos--;
            }
            out[pos] = static_cast<int>(i);
        }
        return n;
    }
};

// Wait kinds (aux of the wait tables)
enum WaitKind : uint32_t { kWaitJob = 1, kWaitMutex = 2, kWaitSemaphore = 3 };
const char* WaitKindName(uint32_t k) {
    return k == kWaitJob ? "job" : (k == kWaitMutex ? "mutex" : (k == kWaitSemaphore ? "semaphore" : "?"));
}
constexpr uint32_t kRemoteCallBit = 0x80000000u; // job key: a remote call's method (TS3W addresses are below 0x02000000)

// Current frame (render thread only; merged into the session tables and cleared at the frame boundary)
TimeTable<64> g_fSvc;
TimeTable<256> g_fJob;
TimeTable<128> g_fWait;
uint64_t g_fReadBytes = 0;
// Session, [0] = other frames, [1] = hitch frames (render thread; reset by Clear)
TimeTable<256> g_aSvc[2];
TimeTable<1024> g_aJob[2];
TimeTable<512> g_aWait[2];
uint64_t g_aReadBytes[2] = {};

// Services updated on other threads (the simulation loop FUN_0059ED70): session totals, any thread may write
struct OtherSvcSlot {
    std::atomic<uint32_t> key{0};
    std::atomic<uint32_t> vtable{0};
    std::atomic<uint64_t> incl{0}, self{0};
    std::atomic<uint32_t> calls{0};
};
constexpr int kOtherSvcSlots = 64;
OtherSvcSlot g_otherSvc[kOtherSvcSlots];
uint64_t g_otherSvcBaseIncl[kOtherSvcSlots] = {}, g_otherSvcBaseSelf[kOtherSvcSlots] = {}; // at the last Clear (render thread)
uint32_t g_otherSvcBaseCalls[kOtherSvcSlots] = {};

void AddOtherService(uint32_t key, uint32_t vtable, uint64_t in, uint64_t se) {
    for (auto& slot : g_otherSvc) {
        uint32_t k = slot.key.load(std::memory_order_acquire);
        if (!k) {
            uint32_t expected = 0;
            if (slot.key.compare_exchange_strong(expected, key)) {
                slot.vtable.store(vtable, std::memory_order_relaxed);
                k = key;
            } else {
                k = expected;
            }
        }
        if (k != key) continue;
        slot.incl.fetch_add(in, std::memory_order_relaxed);
        slot.self.fetch_add(se, std::memory_order_relaxed);
        slot.calls.fetch_add(1, std::memory_order_relaxed);
        return;
    }
}

// Service update functions (vtable +0x1C main / +0x20 simulation) named in the engine study (Steam 1.67.2); others show
// as their address and vtable.
struct NamedAddress {
    uint32_t addr;
    const char* name;
};
const NamedAddress kServiceNames[] = {
    {0x00588890, "MessageServer"},
    {0x00598660, "Input (message pump)"},
    {0x00599A10, "JobManager (main-thread jobs)"},
    {0x005F0E50, "CAS SimService"},
    {0x00608630, "CAS TextureCompositor"},
    {0x006E3620, "Scene service (SceneObjectManager)"},
    {0x0071E640, "Swarm (VFX)"},
    {0x00733D20, "ResourceChangeMonitor"},
    {0x00733D30, "ResourceChangeMonitor (sim)"},
    {0x007377F0, "ResourceSystem"},
    {0x007A08C0, "ShaderSystem"},
    {0x00A37E60, "Crossroads (AccountManager)"},
    {0x00B3A960, "ObjectDesigner"},
    {0x00C7E3C0, "WorldManager"},
    {0x00C7E300, "WorldManager (sim)"},
};

std::string ServiceName(uint32_t fn, uint32_t vtable) {
    if (g_gameVersion == GameVersion::Steam)
        for (const auto& n : kServiceNames)
            if (n.addr == fn) return n.name;
    return std::format("service {:08X} (vtable {:08X})", fn, vtable);
}

std::string JobName(uint32_t key) {
    if (key & kRemoteCallBit) return std::format("remote call -> {:08X}", key & ~kRemoteCallBit);
    return std::format("job {:08X}", key);
}

// Remote calls run as jobs whose function is FUN_007D9840(handle, RemoteCall*, phase); it calls the object's vtable +0x10.
// PostRemoteMethodCall objects (vtable 0x010650C4, built at 0x00ABEA0A; and 0x010650D8, built at 0x00ABEA93, whose method
// takes one byte instead of two) keep the native method at +0x10; their vtable +0x10 (0x00ABD3C0 / 0x00ABD3E0) calls it.
// Set by ResolveRemoteCallKeysLocked from the game-address table (render thread, at attach); 0 = jobs keyed by function.
uint32_t g_remoteCallJobFn = 0;
uint32_t g_remoteMethodVtable = 0;
uint32_t g_remoteMethodVtable2 = 0;

// No C++ objects (SEH): the job's function, or for remote calls the method they will run
uint32_t JobKey(const uint8_t* job) {
    __try {
        const uint32_t fn = *reinterpret_cast<const uint32_t*>(job + 0x10);
        if (fn && fn == g_remoteCallJobFn) {
            const uint8_t* rc = *reinterpret_cast<const uint8_t* const*>(job + 0x14);
            if (rc) {
                const uint32_t vt = *reinterpret_cast<const uint32_t*>(rc);
                const bool posted = vt && (vt == g_remoteMethodVtable || vt == g_remoteMethodVtable2);
                const uint32_t method = posted ? *reinterpret_cast<const uint32_t*>(rc + 0x10) : *reinterpret_cast<const uint32_t*>(static_cast<uintptr_t>(vt) + 0x10);
                if (method) return method | kRemoteCallBit;
            }
        }
        return fn;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// A Mutex / Semaphore call that blocked on the render thread: booked as a leaf child of the running timed call
void RecordBlocked(int cat, uint32_t kind, uint64_t dt, uint32_t site) {
    ThreadSlot* s = GetSlot();
    if (!s) return;
    if (s->depth > 0 && s->stack[s->depth - 1].cat == kJobWait) return; // already counted as the job wait around it
    Add64(s->excl[cat], dt);
    if (s->open[cat] == 0) Add64(s->incl[cat], dt);
    Add32(s->calls[cat], 1);
    if (s->depth > 0) s->stack[s->depth - 1].child += dt;
    g_fWait.Add(site, dt, dt, 1, kind);
}

// ---- counters: the longest call per frame (any thread may write: atomic max; the render thread takes it at the frame
// boundary), the extra counts (owner-written per thread slot, like the times) ----
std::atomic<uint64_t> g_cMax[2][kNC]; // ticks: [0] render thread, [1] other threads

void NoteMax(int cat, uint64_t ticks) {
    std::atomic<uint64_t>& m = g_cMax[IsRenderThread() ? 0 : 1][cat - kFirstCounterCat];
    uint64_t cur = m.load(std::memory_order_relaxed);
    while (ticks > cur && !m.compare_exchange_weak(cur, ticks, std::memory_order_relaxed)) {}
}

inline void AddX(ThreadSlot* s, int x, uint64_t v) {
    Add64(s->extra[x], v);
}

// One timed call of a counter category (any thread). Identity = this object's stack address, as Scope.
struct CounterScope {
    ThreadSlot* s;
    int idx;
    int cat;
    bool timed = false; // End() booked the call: s is valid and the extra counts may be added
    explicit CounterScope(int c) : s(GetSlot()), idx(s ? Push(s, c, this, 0) : -1), cat(c) {}
    void End() {
        if (idx < 0) return;
        uint64_t incl = 0;
        Pop(s, idx, this, Now(), 0, 0, &incl);
        idx = -1;
        timed = true;
        NoteMax(cat, incl);
    }
    ~CounterScope() { End(); }
    CounterScope(const CounterScope&) = delete;
    CounterScope& operator=(const CounterScope&) = delete;
};

// FindProvider walks the vector [mgr+0x30, mgr+0x34) of 8-byte {provider, cookie} entries until one says yes: packages
// probed = the index of the provider it returned + 1, or all of them on a miss. Read without the manager's lock (a
// statistic; the list changes only when packages are registered), SEH-guarded, no C++ objects.
uint32_t PackagesProbed(const uint8_t* mgr, uint32_t provider) {
    __try {
        const uint32_t* b = *reinterpret_cast<const uint32_t* const*>(mgr + 0x30);
        const uint32_t* e = *reinterpret_cast<const uint32_t* const*>(mgr + 0x34);
        if (!b || e < b || e - b > 2 * 65536) return 0;
        const uint32_t n = static_cast<uint32_t>((e - b) / 2);
        if (!provider) return n;
        for (uint32_t i = 0; i < n; i++)
            if (b[2 * i] == provider) return i + 1;
        return n;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

uint32_t ReadU32Safe(const void* p, uint32_t offset) {
    __try {
        return p ? *reinterpret_cast<const uint32_t*>(static_cast<const uint8_t*>(p) + offset) : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// ---- timed game functions ----
enum TargetId : int {
    T_RenderFrame,
    T_EndFramePresent,
    T_LotLodScoring,
    T_LotDetailRequest,
    T_LotRendererUpdate,
    T_LotLoadStages,
    T_LotViewSwitch,
    T_LotLightingInit,
    T_RoomLighting,
    T_LotLightingUpdate,
    T_TerrainUpdate,
    T_ScriptGC,
    T_LotObjectBuild,
    T_ServiceLoopMain,
    T_ServiceLoopSim,
    T_ExecuteJob,
    T_WaitForJob,
    T_MutexLock,
    T_SemaphoreWait,
    T_FileRead,
    T_FileFlush,
    T_RefPackRead,
    T_SceneBeginFrame,
    T_SceneEndFrame,
    T_SceneCapture,
    T_AppState,
    T_ClockTick,
    T_ImpostorPump,
    // counters (addresses from framework/game_addresses.h)
    T_ResLookup,
    T_SceneDrain,
    T_RefPackCompress,
    T_DxtEncode1,
    T_DxtEncode5,
    T_ObjectById,
    T_RoomSolve,
    T_WallAo,
    T_KeyList,
    T_KeyListDerived,
    T_TexCreate,
    T_TexFill,
    kTargetCount
};

void* g_orig[kTargetCount] = {}; // Detours trampolines (the target address while not attached)

using FnThis0 = uint64_t(__fastcall*)(void*, void*);
using FnThis1 = uint64_t(__fastcall*)(void*, void*, uint32_t);
using FnThis2 = uint64_t(__fastcall*)(void*, void*, uint32_t, uint32_t);
using FnThis3 = uint64_t(__fastcall*)(void*, void*, uint32_t, uint32_t, uint32_t);
using FnThis4 = uint64_t(__fastcall*)(void*, void*, uint32_t, uint32_t, uint32_t, uint32_t);
using FnThis5 = uint64_t(__fastcall*)(void*, void*, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
using FnCdecl1 = uint64_t(__cdecl*)(uint32_t);
using FnCdecl2 = uint64_t(__cdecl*)(uint32_t, uint32_t);

template <int T, typename Fn> inline Fn Orig() {
    return reinterpret_cast<Fn>(g_orig[T]);
}

// The hooks are __fastcall(ecx, edx, stack args...): ABI-identical to the originals' __thiscall(ecx, stack args) with
// callee cleanup, ecx and edx passed through untouched. Return type uint64_t keeps EDX:EAX. Arguments are passed as raw
// dwords (floats included), never converted.

uint64_t __fastcall Hook_RenderFrame(void* self, void* edx, uint32_t a) {
    ThreadSlot* s = GetSlot();
    volatile char marker = 0;
    int idx = -1;
    uint32_t exits = 0;
    if (s) {
        exits = s->presentFnExits;
        idx = Push(s, kRenderFrame, const_cast<char*>(&marker), 0);
    }
    const uint64_t r = Orig<T_RenderFrame, FnThis1>()(self, edx, a);
    if (idx >= 0) {
        const uint64_t end = Now();
        const uint64_t carve = (s->presentFnExits != exits && end > s->lastPresentFnExit) ? end - s->lastPresentFnExit : 0;
        Pop(s, idx, const_cast<char*>(&marker), end, carve, kFrameLimiter);
    }
    return r;
}

uint64_t __fastcall Hook_EndFramePresent(void* self, void* edx, uint32_t a, uint32_t b, uint32_t c) {
    ThreadSlot* s = GetSlot();
    volatile char marker = 0;
    const int idx = s ? Push(s, kEndScene, const_cast<char*>(&marker), kSwitchAtSplit) : -1;
    const uint64_t r = Orig<T_EndFramePresent, FnThis3>()(self, edx, a, b, c);
    if (s) {
        const uint64_t end = Now();
        if (idx >= 0) Pop(s, idx, const_cast<char*>(&marker), end, 0, 0);
        s->presentFnExits++;
        s->lastPresentFnExit = end;
    }
    return r;
}

// arg 2 points to the camera point (16 bytes) the function copies to WorldManager+0x3A0 right away
uint64_t __fastcall Hook_LotLodScoring(void* self, void* edx, uint32_t dt, uint32_t cameraPoint, uint32_t c, uint32_t d) {
    if (cameraPoint) {
        float p[3];
        std::memcpy(p, reinterpret_cast<const void*>(static_cast<uintptr_t>(cameraPoint)), sizeof p);
        if (g_camPointValid) {
            const float m = std::max({std::fabs(p[0] - g_camPoint[0]), std::fabs(p[1] - g_camPoint[1]), std::fabs(p[2] - g_camPoint[2])});
            if (m > 1e-3f) g_camPointMoved.store(1, std::memory_order_relaxed);
        }
        std::memcpy(g_camPoint, p, sizeof p);
        g_camPointValid = true;
        g_camPointSeen.store(1, std::memory_order_relaxed);
    }
    Scope sc(kLotLodScoring);
    return Orig<T_LotLodScoring, FnThis4>()(self, edx, dt, cameraPoint, c, d);
}

// Same condition as the function itself: the flag changes only when it differs and the lot is not being bulldozed
uint64_t __fastcall Hook_LotDetailRequest(void* self, void* edx, uint32_t want) {
    const uint8_t* lot = static_cast<const uint8_t*>(self);
    const uint8_t w = static_cast<uint8_t>(want & 0xFF);
    if (lot[0xC1] != w && lot[0xC9] == 0) {
        (w ? g_lotsPromoted : g_lotsDemoted).fetch_add(1, std::memory_order_relaxed);
        LotLoadAnalysis::Request(self, w != 0);
    }
    Scope sc(kLotDetailRequest);
    return Orig<T_LotDetailRequest, FnThis1>()(self, edx, want);
}

uint64_t __fastcall Hook_LotRendererUpdate(void* self, void* edx, uint32_t a) {
    Scope sc(kLotRendererUpdate);
    return Orig<T_LotRendererUpdate, FnThis1>()(self, edx, a);
}

uint64_t __fastcall Hook_LotLoadStages(void* self, void* edx) {
    Scope sc(kLotLoadStages);
    const auto before = LotLoadAnalysis::ReadRenderer(self);
    const uint64_t started = LotLoadAnalysis::Clock();
    const auto result = Orig<T_LotLoadStages, FnThis0>()(self, edx);
    const uint64_t ended = LotLoadAnalysis::Clock();
    LotLoadAnalysis::Sample(before, LotLoadAnalysis::ReadRenderer(self), started, ended);
    return result;
}

uint64_t __fastcall Hook_LotViewSwitch(void* self, void* edx, uint32_t a) {
    Scope sc(kLotViewSwitch);
    return Orig<T_LotViewSwitch, FnThis1>()(self, edx, a);
}

uint64_t __fastcall Hook_LotLightingInit(void* self, void* edx) {
    Scope sc(kLotLightingInit);
    return Orig<T_LotLightingInit, FnThis0>()(self, edx);
}

uint64_t __fastcall Hook_RoomLighting(void* self, void* edx) {
    Scope sc(kRoomLighting);
    return Orig<T_RoomLighting, FnThis0>()(self, edx);
}

uint64_t __fastcall Hook_LotLightingUpdate(void* self, void* edx) {
    Scope sc(kLotLightingUpdate);
    return Orig<T_LotLightingUpdate, FnThis0>()(self, edx);
}

uint64_t __fastcall Hook_TerrainUpdate(void* self, void* edx, uint32_t a, uint32_t b) {
    Scope sc(kTerrainUpdate);
    return Orig<T_TerrainUpdate, FnThis2>()(self, edx, a, b);
}

uint64_t __cdecl Hook_ScriptGC(uint32_t stopFunc) {
    g_simTid.store(GetCurrentThreadId(), std::memory_order_relaxed);
    Scope sc(kScriptGC);
    return Orig<T_ScriptGC, FnCdecl1>()(stopFunc);
}

uint64_t __fastcall Hook_LotObjectBuild(void* self, void* edx, uint32_t a, uint32_t b, uint32_t c) {
    Scope sc(kLotObjectBuild);
    return Orig<T_LotObjectBuild, FnThis3>()(self, edx, a, b, c);
}

// ServiceManager update loops, replaced (the original body is never run while attached). Same walk as the game's code
// (FUN_0059ED20 / FUN_0059ED70, 0x44 bytes each, pattern = the whole body): if list+0xC != 0, for each node from [list]
// until the list head: if byte node+0x1A != 0 and (byte node+0x14 & flag), service = [node+8], call vtable[vOff](dt,
// realDt) (thiscall, ret 8); the next node is read after the call, as the original does. Each call is timed.
void RunServices(uint8_t* list, uint32_t vOff, uint8_t flag, uint32_t dt, uint32_t realDt) {
    using FnSvc = uint64_t(__fastcall*)(void*, void*, uint32_t, uint32_t);
    if (*reinterpret_cast<const uint32_t*>(list + 0x0C) == 0) return;
    uint8_t* node = *reinterpret_cast<uint8_t**>(list);
    while (node != list) {
        if (node[0x1A] != 0 && (node[0x14] & flag)) {
            void* svc = *reinterpret_cast<void**>(node + 8);
            void** vt = *reinterpret_cast<void***>(svc);
            const FnSvc fn = reinterpret_cast<FnSvc>(vt[vOff / 4]);
            ThreadSlot* s = GetSlot();
            volatile char marker = 0;
            const int idx = s ? Push(s, kService, const_cast<char*>(&marker), 0) : -1;
            fn(svc, nullptr, dt, realDt);
            if (idx >= 0) {
                uint64_t in = 0, se = 0;
                Pop(s, idx, const_cast<char*>(&marker), Now(), 0, 0, &in, &se);
                const uint32_t key = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(fn));
                const uint32_t vtable = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(vt));
                if (IsRenderThread()) g_fSvc.Add(key, in, se, 1, vtable);
                else AddOtherService(key, vtable, in, se);
            }
        }
        node = *reinterpret_cast<uint8_t**>(node);
    }
}

uint64_t __fastcall Hook_ServiceLoopMain(void* self, void* /*edx*/, uint32_t dt, uint32_t realDt) {
    RunServices(static_cast<uint8_t*>(self), 0x1C, 1, dt, realDt);
    return 0;
}

uint64_t __fastcall Hook_ServiceLoopSim(void* self, void* /*edx*/, uint32_t dt, uint32_t realDt) {
    RunServices(static_cast<uint8_t*>(self), 0x20, 2, dt, realDt);
    return 0;
}

// JobManager::ExecuteJob(job): runs [job+0x10](job+8, [job+0x14], 4). Timed on every thread (category), keyed on the
// render thread.
uint64_t __fastcall Hook_ExecuteJob(void* self, void* edx, uint32_t job) {
    ThreadSlot* s = GetSlot();
    volatile char marker = 0;
    const int idx = s ? Push(s, kJob, const_cast<char*>(&marker), 0) : -1;
    const bool render = IsRenderThread();
    const uint32_t key = render ? JobKey(reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(job))) : 0; // read before: the job may be freed
    const uint64_t r = Orig<T_ExecuteJob, FnThis1>()(self, edx, job);
    if (idx >= 0) {
        uint64_t in = 0, se = 0;
        Pop(s, idx, const_cast<char*>(&marker), Now(), 0, 0, &in, &se);
        if (render) g_fJob.Add(key, in, se, 1, 0);
    }
    return r;
}

// JobManager::WaitForJob(job): render thread only, keyed by the job waited for (jobs it runs itself are timed as jobs)
uint64_t __fastcall Hook_WaitForJob(void* self, void* edx, uint32_t job) {
    if (!IsRenderThread()) return Orig<T_WaitForJob, FnThis1>()(self, edx, job);
    ThreadSlot* s = GetSlot();
    volatile char marker = 0;
    const int idx = s ? Push(s, kJobWait, const_cast<char*>(&marker), 0) : -1;
    const uint32_t key = JobKey(reinterpret_cast<const uint8_t*>(static_cast<uintptr_t>(job)));
    const uint64_t r = Orig<T_WaitForJob, FnThis1>()(self, edx, job);
    if (idx >= 0) {
        uint64_t in = 0, se = 0;
        Pop(s, idx, const_cast<char*>(&marker), Now(), 0, 0, &in, &se);
        g_fWait.Add(key, se, se, 1, kWaitJob);
    }
    return r;
}

// Mutex::Lock / Semaphore::Wait: every thread goes through (two clock reads); only render-thread calls that blocked
// longer than 0.1 ms are recorded, keyed by the caller's return address
uint64_t __fastcall Hook_MutexLock(void* self, void* edx, uint32_t timeout) {
    const uint64_t t0 = Now();
    const uint64_t r = Orig<T_MutexLock, FnThis1>()(self, edx, timeout);
    const uint64_t dt = Now() - t0;
    if (dt > g_blockTicks && IsRenderThread()) RecordBlocked(kMutexWait, kWaitMutex, dt, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(_ReturnAddress())));
    return r;
}

uint64_t __fastcall Hook_SemaphoreWait(void* self, void* edx, uint32_t timeout) {
    const uint64_t t0 = Now();
    const uint64_t r = Orig<T_SemaphoreWait, FnThis1>()(self, edx, timeout);
    const uint64_t dt = Now() - t0;
    if (dt > g_blockTicks && IsRenderThread()) RecordBlocked(kSemWait, kWaitSemaphore, dt, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(_ReturnAddress())));
    return r;
}

// File I/O: timed on the render thread only
uint64_t __fastcall Hook_FileRead(void* self, void* edx, uint32_t buffer, uint32_t size) {
    if (!IsRenderThread()) return Orig<T_FileRead, FnThis2>()(self, edx, buffer, size);
    g_fReadBytes += size;
    Scope sc(kFileRead);
    return Orig<T_FileRead, FnThis2>()(self, edx, buffer, size);
}

uint64_t __fastcall Hook_FileFlush(void* self, void* edx) {
    if (!IsRenderThread()) return Orig<T_FileFlush, FnThis0>()(self, edx);
    Scope sc(kFileFlush);
    return Orig<T_FileFlush, FnThis0>()(self, edx);
}

using FnStd5 = uint64_t(__stdcall*)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
uint64_t __stdcall Hook_RefPackRead(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e) {
    if (!IsRenderThread()) return Orig<T_RefPackRead, FnStd5>()(a, b, c, d, e);
    Scope sc(kRefPackRead);
    return Orig<T_RefPackRead, FnStd5>()(a, b, c, d, e);
}

uint64_t __fastcall Hook_SceneBeginFrame(void* self, void* edx) {
    Scope sc(kSceneBeginFrame);
    return Orig<T_SceneBeginFrame, FnThis0>()(self, edx);
}

uint64_t __fastcall Hook_SceneEndFrame(void* self, void* edx) {
    Scope sc(kSceneEndFrame);
    return Orig<T_SceneEndFrame, FnThis0>()(self, edx);
}

uint64_t __fastcall Hook_SceneCapture(void* self, void* edx, uint32_t a, uint32_t b) {
    Scope sc(kSceneCapture);
    return Orig<T_SceneCapture, FnThis2>()(self, edx, a, b);
}

uint64_t __fastcall Hook_AppState(void* self, void* edx) {
    Scope sc(kAppState);
    return Orig<T_AppState, FnThis0>()(self, edx);
}

uint64_t __fastcall Hook_ClockTick(void* self, void* edx) {
    Scope sc(kClockTick);
    return Orig<T_ClockTick, FnThis0>()(self, edx);
}

uint64_t __fastcall Hook_ImpostorPump(void* self, void* edx, uint32_t job) {
    Scope sc(kImpostorPump);
    return Orig<T_ImpostorPump, FnThis1>()(self, edx, job);
}

// ---- counters (see the header comment for the verified signatures) ----

// ResourceMgr::FindProvider(key*, cookie*): reached through the two vtable slots only. The profiler is the outer layer of
// the slots (framework/slot_chain.h): it calls the next layer (the resource lookup cache when it is on) or the game's
// function, so it times every call whichever of the two installed first.
uint64_t __fastcall Hook_FindProvider(void* self, void* edx, uint32_t key, uint32_t cookieOut) {
    const FnThis2 next = reinterpret_cast<FnThis2>(SlotChain::Next(SlotChain::Site::FindProvider, SlotChain::Layer::FrameProfiler));
    CounterScope sc(kResLookup);
    const uint64_t r = next(self, edx, key, cookieOut);
    sc.End();
    const ResourceCache::LookupNote note = ResourceCache::TakeLookupNote(); // read on every call: it is cleared per lookup
    if (sc.timed) {
        const uint32_t provider = static_cast<uint32_t>(r);
        if (note.seen && note.hit) {
            AddX(sc.s, kXCacheHits, 1);
            if (note.negative) AddX(sc.s, kXCacheAbsent, 1);
            AddX(sc.s, kXPackages, note.probes); // the packages the cache asked
        } else {
            AddX(sc.s, kXPackages, PackagesProbed(static_cast<const uint8_t*>(self), provider));
        }
        if (!provider) AddX(sc.s, kXMisses, 1);
    }
    return r;
}

// Scene pending-node drain, from Scene::BeginFrame's CALL (render thread): [this+0x18] = nodes processed afterwards.
// Outer layer of the call chain (framework/call_chain.h): the next layer is the scene node budget when it is on
// (features/scene_budget.h), else the game's drain.
uint64_t __fastcall Hook_SceneDrain(void* self, void* edx) {
    const FnThis0 next = reinterpret_cast<FnThis0>(CallChain::Next(CallChain::Site::SceneDrain, CallChain::Layer::FrameProfiler));
    CounterScope sc(kScenePending);
    const uint64_t r = next(self, edx);
    sc.End();
    const SceneBudget::DrainNote note = SceneBudget::TakeDrainNote(); // read on every call: it is cleared per drain
    if (sc.timed) {
        AddX(sc.s, kXNodes, ReadU32Safe(self, 0x18));
        if (note.seen && note.budgeted) AddX(sc.s, kXDeferred, note.left);
    }
    return r;
}

// RefPack stream write(src, size, dst, capacity, flags): a size-bound query (no destination, flags & 1) does no work.
// Outer layer of the slot chain (framework/slot_chain.h): the next layer is the fast compressor when it is on
// (features/fast_refpack.h), else the game's function, so the counter times whichever compresses.
uint64_t __fastcall Hook_RefPackCompress(void* self, void* edx, uint32_t src, uint32_t size, uint32_t dst, uint32_t capacity, uint32_t flags) {
    const FnThis5 next = reinterpret_cast<FnThis5>(SlotChain::Next(SlotChain::Site::RefPackCompress, SlotChain::Layer::FrameProfiler));
    if (!dst && (flags & 1)) return next(self, edx, src, size, dst, capacity, flags);
    CounterScope sc(kRefPackCompress);
    const uint64_t r = next(self, edx, src, size, dst, capacity, flags);
    sc.End();
    if (sc.timed) {
        AddX(sc.s, kXBytesIn, size);
        AddX(sc.s, kXBytesOut, static_cast<uint32_t>(r)); // the compressed size (header included)
    }
    return r;
}

// DXT1 / DXT5 encoders, cdecl(dst*, src*): dst = {pixels, width, height, pitch}. Outer layer of the entry chain
// (framework/entry_chain.h): the next layer is the fast encoder when it is on (features/fast_dxt.h), else the game's code.
uint64_t DxtEncode(int target, uint32_t dst, uint32_t src) {
    const uint64_t pixels = static_cast<uint64_t>(ReadU32Safe(reinterpret_cast<const void*>(static_cast<uintptr_t>(dst)), 4)) *
                            ReadU32Safe(reinterpret_cast<const void*>(static_cast<uintptr_t>(dst)), 8);
    const EntryChain::Site site = target == T_DxtEncode5 ? EntryChain::Site::DxtEncode5 : EntryChain::Site::DxtEncode1;
    CounterScope sc(kDxtEncode);
    const uint64_t r = reinterpret_cast<FnCdecl2>(EntryChain::Next(site, EntryChain::Layer::FrameProfiler))(dst, src);
    sc.End();
    if (sc.timed) AddX(sc.s, kXPixels, pixels);
    return r;
}
uint64_t __cdecl Hook_DxtEncode1(uint32_t dst, uint32_t src) {
    return DxtEncode(T_DxtEncode1, dst, src);
}
uint64_t __cdecl Hook_DxtEncode5(uint32_t dst, uint32_t src) {
    return DxtEncode(T_DxtEncode5, dst, src);
}

// Object lookup by ID (idLo, idHi, visited*): any thread, bucketed render / simulation / other. Outer layer of the entry
// chain (framework/entry_chain.h): the next layer is the object lookup index when it is on (features/object_index.h),
// else the game's code (the trampoline).
uint64_t __fastcall Hook_ObjectById(void* self, void* edx, uint32_t idLo, uint32_t idHi, uint32_t visited) {
    const FnThis3 next = reinterpret_cast<FnThis3>(EntryChain::Next(EntryChain::Site::ObjectById, EntryChain::Layer::FrameProfiler));
    CounterScope sc(kObjectLookup);
    const uint64_t r = next(self, edx, idLo, idHi, visited);
    sc.End();
    const ObjectIndex::LookupNote note = ObjectIndex::TakeLookupNote(); // read on every call: it is cleared per lookup
    if (sc.timed && note.seen && note.hit) AddX(sc.s, kXIndexHits, 1);
    return r;
}

// One room of the lot lighting update (timer*, float budget as raw bits), from its only CALL
uint64_t __fastcall Hook_RoomSolve(void* self, void* edx, uint32_t timer, uint32_t budget) {
    CounterScope sc(kLotRoomSolve);
    return Orig<T_RoomSolve, FnThis2>()(self, edx, timer, budget);
}

// The wall AO step (stopwatch*, float budget as raw bits), through its vtable slot. Layer inside the wall shading gate
// (features/lot_lighting_motion.h), which must stay outermost: only the passes the gate lets through reach this counter.
uint64_t __fastcall Hook_WallAoStep(void* self, void* edx, uint32_t stopwatch, uint32_t budget) {
    const FnThis2 next = reinterpret_cast<FnThis2>(SlotChain::Next(SlotChain::Site::WallAoStep, SlotChain::Layer::FrameProfiler));
    CounterScope sc(kWallAo);
    return next(self, edx, stopwatch, budget);
}

// The out vector's size in keys (16 bytes each), SEH-guarded
uint32_t KeyVectorSize(uint32_t out) {
    __try {
        if (!out) return 0;
        const uint32_t* v = reinterpret_cast<const uint32_t*>(static_cast<uintptr_t>(out));
        return v[1] >= v[0] ? (v[1] - v[0]) / 16 : 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return 0; }
}

// ResourceMgr::GetKeyList(out, filter, unique): outer layer of the slot chain; the file list cache (features/resource_cache.h)
// is the inner layer when on. Keys = growth of the out vector (the derived function sorts and uniques it afterwards).
uint64_t KeyList(SlotChain::Site site, void* self, void* edx, uint32_t out, uint32_t filter, uint32_t unique) {
    const FnThis3 next = reinterpret_cast<FnThis3>(SlotChain::Next(site, SlotChain::Layer::FrameProfiler));
    const uint32_t before = KeyVectorSize(out);
    CounterScope sc(kKeyList);
    const uint64_t r = next(self, edx, out, filter, unique);
    sc.End();
    const ResourceCache::KeyListNote note = ResourceCache::TakeKeyListNote(); // read on every call: it is cleared per call
    if (sc.timed) {
        const uint32_t after = KeyVectorSize(out);
        AddX(sc.s, kXKeys, after > before ? after - before : 0);
        if (note.seen) AddX(sc.s, kXListCached, note.cachedPackages);
    }
    return r;
}
uint64_t __fastcall Hook_KeyList(void* self, void* edx, uint32_t out, uint32_t filter, uint32_t unique) {
    return KeyList(SlotChain::Site::KeyListBase, self, edx, out, filter, unique);
}
uint64_t __fastcall Hook_KeyListDerived(void* self, void* edx, uint32_t out, uint32_t filter, uint32_t unique) {
    return KeyList(SlotChain::Site::KeyListDerived, self, edx, out, filter, unique);
}

// ---- texture loads by size (the DDS loader's create and fill calls). The profiler's CreateTexture callback notes the
// size of the texture this thread is creating; the create hook files the call under its size class and leaves the class
// for the fill call that follows it on the same thread. ----
constexpr int kTexBuckets = 6; // level 0 pixels: <= 128^2, 256^2, 512^2, 1024^2, 2048^2, more
const char* const kTexBucketNames[kTexBuckets] = {"up to 128x128", "up to 256x256", "up to 512x512", "up to 1024x1024", "up to 2048x2048", "larger"};
struct TexBucket {
    std::atomic<uint64_t> loads{0}, createTicks{0}, createMax{0}, fills{0}, fillTicks{0}, fillMax{0}, bytes{0};
};
TexBucket g_tex[kTexBuckets + 1]; // + unknown size
thread_local uint32_t t_texW = 0, t_texH = 0, t_texLevels = 0;
thread_local D3DFORMAT t_texFormat = D3DFMT_UNKNOWN;
thread_local int t_texBucket = kTexBuckets;

int TexBucketOf(uint64_t pixels) {
    if (!pixels) return kTexBuckets;
    int b = 0;
    for (uint64_t lim = 128ull * 128ull; b < kTexBuckets - 1 && pixels > lim; lim *= 4) b++;
    return b;
}
// Bytes of the whole mip chain for the formats the game loads (DXT1 8 per 4x4 block, DXT3/5 16, else 4 per pixel), for the report
uint64_t TexBytes(uint32_t w, uint32_t h, uint32_t levels, D3DFORMAT f) {
    uint64_t total = 0;
    if (levels == 0) levels = 32; // D3D: the full chain (the loop stops at 1x1)
    for (uint32_t l = 0; l < levels && (w || h); l++) {
        const uint64_t bw = (std::max<uint32_t>(w, 1) + 3) / 4, bh = (std::max<uint32_t>(h, 1) + 3) / 4;
        total += f == D3DFMT_DXT1 ? bw * bh * 8 : (f == D3DFMT_DXT3 || f == D3DFMT_DXT5) ? bw * bh * 16 : static_cast<uint64_t>(std::max<uint32_t>(w, 1)) * std::max<uint32_t>(h, 1) * 4;
        w >>= 1;
        h >>= 1;
    }
    return total;
}
inline void StoreMax(std::atomic<uint64_t>& m, uint64_t v) {
    uint64_t cur = m.load(std::memory_order_relaxed);
    while (v > cur && !m.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {}
}

using FnCdecl4 = uint64_t(__cdecl*)(uint32_t, uint32_t, uint32_t, uint32_t);
using FnCdecl9 = uint64_t(__cdecl*)(uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t);
uint64_t __cdecl Hook_TexCreate(uint32_t a, uint32_t b, uint32_t c, uint32_t d, uint32_t e, uint32_t f, uint32_t g, uint32_t h, uint32_t i) {
    t_texW = t_texH = t_texLevels = 0;
    t_texFormat = D3DFMT_UNKNOWN;
    CounterScope sc(kTexCreate);
    const uint64_t t0 = Now();
    const uint64_t r = Orig<T_TexCreate, FnCdecl9>()(a, b, c, d, e, f, g, h, i);
    const uint64_t dt = Now() - t0;
    sc.End();
    const uint64_t pixels = static_cast<uint64_t>(t_texW) * t_texH;
    if (sc.timed) AddX(sc.s, kXPixels, pixels);
    const int bucket = TexBucketOf(pixels);
    t_texBucket = bucket;
    TexBucket& tb = g_tex[bucket];
    tb.loads.fetch_add(1, std::memory_order_relaxed);
    tb.createTicks.fetch_add(dt, std::memory_order_relaxed);
    StoreMax(tb.createMax, dt);
    tb.bytes.fetch_add(TexBytes(t_texW, t_texH, t_texLevels, t_texFormat), std::memory_order_relaxed);
    return r;
}
uint64_t __cdecl Hook_TexFill(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    CounterScope sc(kTexFill);
    const uint64_t t0 = Now();
    const uint64_t r = Orig<T_TexFill, FnCdecl4>()(a, b, c, d);
    const uint64_t dt = Now() - t0;
    sc.End();
    TexBucket& tb = g_tex[t_texBucket];
    t_texBucket = kTexBuckets;
    tb.fills.fetch_add(1, std::memory_order_relaxed);
    tb.fillTicks.fetch_add(dt, std::memory_order_relaxed);
    StoreMax(tb.fillMax, dt);
    return r;
}

struct TargetInfo {
    const char* name;
    uintptr_t steam;     // Steam 1.67.2: where the pattern starts (the function entry, or the context of a CALL)
    const char* pattern; // on Steam it must match at `steam`; elsewhere exactly once in .text
    int callOffset;      // -1: Detours on the function entry; else offset of the CALL rel32 inside the pattern
    uintptr_t callee;    // call-site targets: the function the CALL reaches on Steam 1.67.2 (cross-check)
    void* hook;
    const char* thread;  // expected thread, for the report
    bool optional;
    int safeLen = 0;     // > 0: patched by hand with this many relocation-free prologue bytes while all other threads are
                         // suspended and checked (functions called from many threads); 0: Detours / call site
    // Targets whose address comes from the game-address table (framework/game_addresses.h: fixed on Steam 1.67.2,
    // signature elsewhere). The pattern is then checked at that address (the entry, or the CALL for call-site targets,
    // callOffset 0) and `steam` is only shown while the table is not resolved yet.
    GameAddr::Id addrId = GameAddr::Id::Count;   // Count = none (pattern at `steam` / unique scan, as above)
    GameAddr::Id calleeId = GameAddr::Id::Count; // call-site targets: the function the CALL must reach
    GameAddr::Id slotId = GameAddr::Id::Count;   // vtable-slot targets: the first of `slots` consecutive slot ids
    int slots = 0;                               // > 0: hooked by swapping these vtable slots (the function is only called through them)
};
constexpr GameAddr::Id kNoAddr = GameAddr::Id::Count;
constexpr int kMaxTargetSlots = 2;

// Call-site targets: FUN_00AEA680 and FUN_00C845C0 are detoured by Smooth Streaming, which checks their entry bytes before
// it installs. Their per-frame CALL is redirected instead (tracked 5-byte write, restored byte for byte): Smooth Streaming
// still installs, and when it is on, the time measured includes its hook.
const TargetInfo kTargets[kTargetCount] = {
    {"Render frame (FUN_00EC9F00)", 0x00EC9F00, "83 EC 20 56 E8 ?? ?? ?? ?? 8B F0 8B CE E8 ?? ?? ?? ?? 84 C0 0F 84 ?? ?? ?? ?? 8B CE E8 ?? ?? ?? ?? D9 E8", -1, 0,
        reinterpret_cast<void*>(&Hook_RenderFrame), "render", false},
    {"End frame + Present (FUN_00611680)", 0x00611680, "83 EC 20 56 8B F1 8B 86 EC 00 00 00 85 C0 74 15 80 BE 8D 00 00 00 00 75 0C", -1, 0,
        reinterpret_cast<void*>(&Hook_EndFramePresent), "render", false},
    {"Lot LOD scoring (FUN_00C6C290)", 0x00C6C290, "55 8B EC 83 E4 F0 81 EC 84 08 00 00 A1 ?? ?? ?? ?? 53 8B D9 8B 4D 0C 0F 28 8B A0 03 00 00", -1, 0,
        reinterpret_cast<void*>(&Hook_LotLodScoring), "render (WorldManager::Update)", false},
    {"Lot detail request (FUN_00AC20E0)", 0x00AC20E0, "53 8A 5C 24 08 56 8B F1 8A 86 C1 00 00 00 3A C3 0F 84 ?? ?? ?? ?? 80 BE C9 00 00 00 00", -1, 0,
        reinterpret_cast<void*>(&Hook_LotDetailRequest), "render (WorldManager::Update)", false},
    {"Lot renderer update (FUN_00AEB2E0)", 0x00AEB2E0, "56 8B F1 E8 ?? ?? ?? ?? 80 7E 0C 00 0F 84 ?? ?? ?? ?? 83 7E 2C 00 74 ?? 80 7E 1E 00", -1, 0,
        reinterpret_cast<void*>(&Hook_LotRendererUpdate), "render (lot pass FUN_00C7CEA0)", false},
    {"Lot load stages (CALL at 0x00AEB306)", 0x00AEB2F8, "80 7E 1E 00 75 0D 80 7E 1F 00 75 07 8B CE E8 ?? ?? ?? ?? 8D 8E 70 03 00 00 E8", 14, 0x00AEA680,
        reinterpret_cast<void*>(&Hook_LotLoadStages), "render (lot pass FUN_00C7CEA0)", false},
    {"Lot LOD switch (FUN_00AD9E30)", 0x00AD9E30, "56 8B F1 8B 4E 28 80 79 1E 00 75 0F 8B 44 24 08 C7 40 0C 07 00 00 00 5E C2 04 00", -1, 0,
        reinterpret_cast<void*>(&Hook_LotViewSwitch), "lot impostor builder", false},
    {"Lot lighting setup (FUN_00ADBAD0)", 0x00ADBAD0,
        "53 55 56 8B F1 8B 46 40 2B 46 30 8B 4E 34 2B 4E 38 8B 56 2C 2B 56 24 C1 F8 02 C1 E0 06 C1 F9 02 03 C1 C1 FA 02 8D 6C 10 C0 33 DB 85 ED 57 7E 43", -1, 0,
        reinterpret_cast<void*>(&Hook_LotLightingInit), "lot impostor builder", false},
    {"Room lighting + solve (FUN_006A80E0)", 0x006A80E0, "83 EC 08 55 56 57 8B F9 80 BF 80 02 00 00 00 75 05 E8 ?? ?? ?? ?? 8B 87 38 02 00 00 8B 8F 34 02 00 00", -1, 0,
        reinterpret_cast<void*>(&Hook_RoomLighting), "lot impostor builder", false},
    {"Lot lighting update (FUN_00ADB8F0)", 0x00ADB8F0,
        "83 EC 1C 56 8B F1 80 7E 18 00 0F 84 ?? ?? ?? ?? F3 0F 10 05 ?? ?? ?? ?? 0F 2F 05 ?? ?? ?? ?? 76 04 83 46 50 01 80 7E 4E 00", -1, 0,
        reinterpret_cast<void*>(&Hook_LotLightingUpdate), "render (lot pass FUN_00C7CEA0)", false},
    {"Terrain update (CALL at 0x00C6D68F)", 0x00C6D682, "8B 44 24 0C 50 8D 4C 24 14 51 8B 4E 58 E8 ?? ?? ?? ?? 80 BE 58 02 00 00 00", 13, 0x00C845C0,
        reinterpret_cast<void*>(&Hook_TerrainUpdate), "render (WorldManager::Update)", false},
    {"GC_try_to_collect (FUN_00E4A050)", 0x00E4A050,
        "83 3D ?? ?? ?? ?? 00 74 06 FF 15 ?? ?? ?? ?? E8 ?? ?? ?? ?? 83 3D ?? ?? ?? ?? 00 75 05 E8 ?? ?? ?? ?? 56 6A 00 6A 00 6A 00 6A 00 6A 00 6A 00 E8 ?? ?? ?? ?? 8B 44 24 20 50 E8",
        -1, 0, reinterpret_cast<void*>(&Hook_ScriptGC), "simulation", false},
    {"Lot::UpdateObjectSceneNode (FUN_00ABFAC0)", 0x00ABFAC0, "83 EC 0C 83 B9 64 03 00 00 00 89 4C 24 04 0F 84 ?? ?? ?? ?? 83 B9 08 04 00 00 01", -1, 0,
        reinterpret_cast<void*>(&Hook_LotObjectBuild), "AddLotObjectsToScene's thread", true},
    // Engine study targets (Steam 1.67.2). The two service loops are replaced: their patterns are the whole 0x44-byte body.
    {"ServiceManager main loop (FUN_0059ED20)", 0x0059ED20,
        "57 8B F9 8B 47 0C 85 C0 74 36 56 8B 37 3B F7 74 2E 80 7E 1A 00 74 22 F6 46 14 01 74 1C 8B 4E 08 D9 44 24 10 8B 11 8B 42 1C 83 EC 08 D9 5C 24 04 D9 44 24 14 D9 1C 24 FF D0 8B 36 3B F7 "
        "75 D2 5E 5F C2 08 00",
        -1, 0, reinterpret_cast<void*>(&Hook_ServiceLoopMain), "render (main loop)", false, 6},
    {"ServiceManager sim loop (FUN_0059ED70)", 0x0059ED70,
        "57 8B F9 8B 47 0C 85 C0 74 36 56 8B 37 3B F7 74 2E 80 7E 1A 00 74 22 F6 46 14 02 74 1C 8B 4E 08 D9 44 24 10 8B 11 8B 42 20 83 EC 08 D9 5C 24 04 D9 44 24 14 D9 1C 24 FF D0 8B 36 3B F7 "
        "75 D2 5E 5F C2 08 00",
        -1, 0, reinterpret_cast<void*>(&Hook_ServiceLoopSim), "simulation", false, 6},
    {"JobManager::ExecuteJob (FUN_00599720)", 0x00599720,
        "53 55 56 8B 74 24 10 33 ED 39 6E 10 57 8B D9 74 3E 8D 7B 08 8B CF C7 46 24 04 00 00 00 E8 ?? ?? ?? ?? 8B 46 14 8B 56 10 6A 04 50 8D 4E 08 51 FF D2", -1, 0,
        reinterpret_cast<void*>(&Hook_ExecuteJob), "any (keyed on render)", false, 7},
    {"JobManager::WaitForJob (FUN_0059A220)", 0x0059A220, "51 55 56 57 8B E9 E8 ?? ?? ?? ?? 8B 8D 88 00 00 00 8D 95 88 00 00 00 33 FF 3B CA 89 7C 24 0C 74 0B", -1, 0,
        reinterpret_cast<void*>(&Hook_WaitForJob), "render (others pass through)", false, 6},
    {"Mutex::Lock (FUN_004E16F0)", 0x004E16F0, "53 55 56 8B F1 80 7E 24 00 57 74 63 8B 6C 24 14 83 7D 00 FF 56 75 14 FF 15 ?? ?? ?? ?? 83 46 20 01", -1, 0,
        reinterpret_cast<void*>(&Hook_MutexLock), "any (blocked, render)", false, 5},
    {"Semaphore::Wait (FUN_004E2760)", 0x004E2760, "56 8B 74 24 08 8B 06 83 F8 FF 57 8B F9 74 19 85 C0 74 15 E8 ?? ?? ?? ?? 8B 0E 3B C8 76 06 2B C8", -1, 0,
        reinterpret_cast<void*>(&Hook_SemaphoreWait), "any (blocked, render)", false, 5},
    {"FileStream::Read (FUN_004DB850)", 0x004DB850, "56 8B F1 8B 46 04 83 F8 FF 74 30 8B 54 24 0C 6A 00 8D 4C 24 10 51 8B 4C 24 10 52 51 50 FF 15 ?? ?? ?? ??", -1, 0,
        reinterpret_cast<void*>(&Hook_FileRead), "any (timed on render)", false, 6},
    {"FileStream::Flush (FUN_004DB8E0)", 0x004DB8E0, "56 8B F1 8B 4E 04 32 C0 83 F9 FF 74 20 53 51 FF 15 ?? ?? ?? ?? 85 C0 0F 95 C3 84 DB 75 0C FF 15", -1, 0,
        reinterpret_cast<void*>(&Hook_FileFlush), "any (timed on render)", false, 6},
    {"RefPack stream read (FUN_004EC010)", 0x004EC010, "56 8B 74 24 08 66 8B 06 8A E8 8A CC 0F B7 C1 8B D0 81 E2 FF 1F 00 00 81 FA FB 10 00 00 75 42", -1, 0,
        reinterpret_cast<void*>(&Hook_RefPackRead), "any (timed on render)", false, 5},
    {"Scene::BeginFrame (FUN_006EBB70)", 0x006EBB70, "51 56 8B F1 80 BE 90 02 00 00 00 0F 85 ?? ?? ?? ?? A1 ?? ?? ?? ?? 80 B8 8D 00 00 00 00 0F 85", -1, 0,
        reinterpret_cast<void*>(&Hook_SceneBeginFrame), "render (main loop)", false},
    {"Scene::EndFrame (FUN_006E8810)", 0x006E8810, "53 56 8B F1 E8 ?? ?? ?? ?? 32 DB 88 98 D0 05 00 00 38 9E 9D 02 00 00 0F 84 ?? ?? ?? ?? 38 9E 90", -1, 0,
        reinterpret_cast<void*>(&Hook_SceneEndFrame), "render (main loop)", false},
    {"SceneCaptureManager (FUN_009DE140)", 0x009DE140, "55 8B EC 83 E4 F0 81 EC 54 07 00 00 53 56 57 8B F9 8D 47 1C 39 00 0F 84 ?? ?? ?? ?? 83 7D 08 00", -1, 0,
        reinterpret_cast<void*>(&Hook_SceneCapture), "render (main loop)", false},
    {"App state (FUN_00EC6C30)", 0x00EC6C30, "56 8B F1 8B 4E 04 85 C9 75 04 39 0E 74 2F 8B C1 F7 D8 23 C1 8B 0E 85 C9 74 05 39 41 04 74 10 50", -1, 0,
        reinterpret_cast<void*>(&Hook_AppState), "render (main loop)", false},
    {"Game clock tick (FUN_005943F0)", 0x005943F0, "83 EC 10 55 56 57 8D 44 24 14 50 8B F1 FF 15 ?? ?? ?? ?? 8B 7C 24 14 8B 6C 24 18 8B CF 2B 4E 38", -1, 0,
        reinterpret_cast<void*>(&Hook_ClockTick), "render (main loop)", false},
    {"Lot impostor pump (FUN_00AD97E0)", 0x00AD97E0, "83 EC 08 80 79 25 00 0F 84 ?? ?? ?? ?? 53 55 56 8B 35 ?? ?? ?? ?? 57 6A 00 68 ?? ?? ?? ?? 8B CE", -1, 0,
        reinterpret_cast<void*>(&Hook_ImpostorPump), "render", false},
    // Counters (research\perf2\plan.md section 8). Patterns = the bytes checked at the game-address table's address.
    {"Resource lookup (FUN_004AFFC0, 2 vtable slots)", 0x004AFFC0, "51 53 55 56 57 8B F9 8D 5F 48 68 ?? ?? ?? ?? 8B CB E8 ?? ?? ?? ?? 8B 77 30 8B 6F 34 3B F5", -1, 0,
        reinterpret_cast<void*>(&Hook_FindProvider), "any", false, 0, GameAddr::Id::ResFindProvider, kNoAddr, GameAddr::Id::ResFindProviderSlot0, 2},
    {"Scene pending nodes (CALL at 0x006EBC49, call chain)", 0x006EBC49, "E8 ?? ?? ?? ?? 80 BE A2 02 00 00 00 75 ?? 8B 4E 38 E8", 0, 0x006E4130,
        reinterpret_cast<void*>(&Hook_SceneDrain), "render (Scene::BeginFrame)", false, 0, GameAddr::Id::SceneDrainCall, GameAddr::Id::SceneDrain},
    {"RefPack compress (FUN_004EC200, vtable slot)", 0x004EC200, "8B 54 24 14 33 C0 F6 C2 02 74 07 B8 01 00 00 00 EB 0D F7 C2 00 00 01 00 74 05 B8 02 00 00 00 56", -1, 0,
        reinterpret_cast<void*>(&Hook_RefPackCompress), "any", false, 0, GameAddr::Id::RefPackCompress, kNoAddr, GameAddr::Id::RefPackCompressSlot, 1},
    // The two DXT entries and the object lookup go through framework/entry_chain.h, the scene drain CALL through
    // framework/call_chain.h (AttachTarget / DetachTarget); their safeLen is informational
    {"DXT1 encode (FUN_006152F0, entry chain)", 0x006152F0, "55 8B EC 83 E4 F0 81 EC 54 01 00 00 8B 45 08 8B 50 04 8B 48 08 53 56 8D 72 03", -1, 0,
        reinterpret_cast<void*>(&Hook_DxtEncode1), "any", false, 6, GameAddr::Id::DxtEncode1},
    {"DXT5 encode (FUN_006154B0, entry chain)", 0x006154B0, "55 8B EC 83 E4 F0 81 EC A4 01 00 00 8B 45 08 8B 48 04 8D 51 03 83 E2 FC", -1, 0,
        reinterpret_cast<void*>(&Hook_DxtEncode5), "any", false, 6, GameAddr::Id::DxtEncode5},
    {"Object lookup by ID (FUN_00C62D40, entry chain)", 0x00C62D40, "8B 44 24 0C 8B 54 24 08 56 50 8B 44 24 0C 52 50 E8 ?? ?? ?? ?? 8B F0 85 F6 74 14 8B 16 8B 42 40 8B CE FF D0 83 F8 01", -1,
        0, reinterpret_cast<void*>(&Hook_ObjectById), "any (render / simulation)", false, 8, GameAddr::Id::ObjectById},
    {"Lot room solve (CALL at 0x00ADB9AD)", 0x00ADB9AD, "E8 ?? ?? ?? ?? EB 02 DD D8 8D 4C 24 14 E8", 0, 0x006A8BA0,
        reinterpret_cast<void*>(&Hook_RoomSolve), "render (lot lighting update)", false, 0, GameAddr::Id::RoomSolveCall, GameAddr::Id::RoomSolve},
    {"Wall AO pass (FUN_0068B810, vtable slot)", 0x0068B810, "83 EC 34 55 56 8B F1 83 7E 04 00 74 14 E8 ?? ?? ?? ?? 8B 4E 04 50 E8", -1, 0,
        reinterpret_cast<void*>(&Hook_WallAoStep), "render (room solve)", false, 0, GameAddr::Id::WallAoStep, kNoAddr, GameAddr::Id::WallAoStepSlot, 1},
    {"Key list (FUN_004B1AE0, vtable slot)", 0x004B1AE0, "83 EC 10 53 33 C0 38 44 24 20 56 57 89 44 24 0C 0F 84", -1, 0,
        reinterpret_cast<void*>(&Hook_KeyList), "any", false, 0, GameAddr::Id::ResKeyList, kNoAddr, GameAddr::Id::ResKeyListSlot, 1},
    {"Key list, ResourceSystem (FUN_00736660, vtable slot)", 0x00736660, "8B 44 24 0C 8B 54 24 08 56 8B 74 24 08 50 52 56 E8", -1, 0,
        reinterpret_cast<void*>(&Hook_KeyListDerived), "any", false, 0, GameAddr::Id::ResKeyListDerived, kNoAddr, GameAddr::Id::ResKeyListDerivedSlot, 1},
    {"Texture create (CALL at 0x0060E1DC)", 0x0060E1DC, "E8 ?? ?? ?? ?? 83 C4 24 0F B6 C0 85 C0", 0, 0x0060CEA0,
        reinterpret_cast<void*>(&Hook_TexCreate), "render (texture load finalize)", false, 0, GameAddr::Id::TexCreateCall, GameAddr::Id::TexCreate},
    {"Texture fill (CALL at 0x0060E1FF)", 0x0060E1FF, "E8 ?? ?? ?? ?? 83 C4 10", 0, 0x0060D290,
        reinterpret_cast<void*>(&Hook_TexFill), "render (texture load finalize)", false, 0, GameAddr::Id::TexFillCall, GameAddr::Id::TexFill},
};
const int kTargetCat[kTargetCount] = {kRenderFrame, kEndScene, kLotLodScoring, kLotDetailRequest, kLotRendererUpdate, kLotLoadStages,
    kLotViewSwitch, kLotLightingInit, kRoomLighting, kLotLightingUpdate, kTerrainUpdate, kScriptGC, kLotObjectBuild,
    kService, kService, kJob, kJobWait, kMutexWait, kSemWait, kFileRead, kFileFlush, kRefPackRead, kSceneBeginFrame, kSceneEndFrame, kSceneCapture, kAppState,
    kClockTick, kImpostorPump, kResLookup, kScenePending, kRefPackCompress, kDxtEncode, kDxtEncode, kObjectLookup, kLotRoomSolve, kWallAo, kKeyList, kKeyList, kTexCreate, kTexFill};

struct TargetState {
    uintptr_t addr = 0; // the function entry, or the CALL instruction for call-site targets
    bool attached = false;
    std::vector<MemPatch::PatchLocation> patched; // call-site targets: the original 5 bytes
    uintptr_t scanned = 0; // non-Steam builds: where the unique scan found the pattern (scanned once)
    uint8_t orig8[8] = {};   // safeLen targets: the original first 8 bytes
    uint8_t* tramp = nullptr; // safeLen targets: copied prologue + JMP back (kept for the process lifetime)
    uintptr_t slotAddr[kMaxTargetSlots] = {}; // vtable-slot targets: the slots swapped to the hook
    bool waitingAddr = false; // the game-address table was not resolved yet: attached at a later frame boundary
    std::string how;
    std::string status = "Off";
};
TargetState g_targets[kTargetCount]; // guarded by g_ctrlMutex
std::mutex g_ctrlMutex;
std::string g_summary; // guarded by g_ctrlMutex
std::atomic<bool> g_attachPending{false}; // attach at the next frame boundary (render thread)
std::atomic<bool> g_waitAddr{false};      // some targets wait for GameAddr::Resolve (profiler on at start-up; see OnPresentStart)

struct TextSection {
    uintptr_t begin = 0;
    size_t size = 0;
};

TextSection GetText() {
    static const TextSection sec = [] {
        TextSection t;
        const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
        const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
        const IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++, s++) {
            if (std::strncmp(reinterpret_cast<const char*>(s->Name), ".text", IMAGE_SIZEOF_SHORT_NAME) == 0) {
                t.begin = base + s->VirtualAddress;
                t.size = s->Misc.VirtualSize;
                break;
            }
        }
        return t;
    }();
    return sec;
}

size_t PatternLength(const char* p) {
    size_t n = 0;
    while (*p) {
        while (*p == ' ') p++;
        if (!*p) break;
        n++;
        p += 2;
    }
    return n;
}

// The pattern matches exactly at addr (inside the game's .text)
bool MatchAt(uintptr_t addr, const char* pattern) {
    const TextSection t = GetText();
    const size_t n = PatternLength(pattern);
    if (!t.begin || n == 0 || addr < t.begin || addr + n > t.begin + t.size) return false;
    return MemPatch::ScanPattern(reinterpret_cast<BYTE*>(addr), n, pattern) == addr;
}

// Unique match of the pattern in .text, 0 when absent or ambiguous
uintptr_t ScanUnique(const char* pattern, uintptr_t* firstOut = nullptr) {
    const TextSection t = GetText();
    const size_t n = PatternLength(pattern);
    if (!t.begin || n == 0 || t.size < n) return 0;
    const uintptr_t a = MemPatch::ScanPattern(reinterpret_cast<BYTE*>(t.begin), t.size, pattern);
    if (firstOut) *firstOut = a;
    if (!a) return 0;
    const uintptr_t next = a + 1;
    const uintptr_t end = t.begin + t.size;
    if (end - next >= n && MemPatch::ScanPattern(reinterpret_cast<BYTE*>(next), end - next, pattern)) return 0;
    return a;
}

// The function entry, or the CALL instruction for call-site targets (resolved, else the Steam 1.67.2 one)
uintptr_t DisplayAddress(int i) {
    if (g_targets[i].addr) return g_targets[i].addr;
    return kTargets[i].steam + static_cast<uintptr_t>(std::max(0, kTargets[i].callOffset));
}

uintptr_t CallTarget(uintptr_t call) {
    int32_t rel;
    std::memcpy(&rel, reinterpret_cast<const void*>(call + 1), 4);
    return call + 5 + static_cast<uintptr_t>(static_cast<intptr_t>(rel));
}

// Sets st.addr to the function entry (entry targets) or to the CALL instruction (call-site targets), or explains why not
void ResolveTarget(int i) {
    TargetState& st = g_targets[i];
    const TargetInfo& ti = kTargets[i];
    st.addr = 0;
    st.waitingAddr = false;
    if (ti.addrId != kNoAddr) { // address from the game-address table, bytes checked here
        if (!GameAddr::Scanned()) {
            st.status = "Waiting for the game-address scan (attached at a frame boundary once it is done)";
            st.waitingAddr = true;
            g_waitAddr.store(true);
            return;
        }
        const uintptr_t a = GameAddr::Get(ti.addrId);
        if (!a) {
            st.status = std::format("Skipped: {} not found on {} (game_addresses.cpp)", GameAddr::Name(ti.addrId), GetGameVersionName());
            return;
        }
        // The DXT encoders' and the object lookup's entries are shared through framework/entry_chain.h (with the fast
        // encoder / the object lookup index), which checks the prologue itself (the entry may already hold its JMP)
        const bool entryChained = i == T_DxtEncode1 || i == T_DxtEncode5 || i == T_ObjectById;
        if (!entryChained && !MatchAt(a, ti.pattern)) {
            st.status = std::format("Skipped: bytes at {:#010x} do not match (detoured or patched by another module?)", a);
            return;
        }
        st.how = entryChained ? (GameAddr::IsFixed() ? "Steam 1.67.2 address, entry checked by the entry chain" : "game-address signature, entry checked by the entry chain")
                              : (GameAddr::IsFixed() ? "Steam 1.67.2 address, bytes checked" : "game-address signature, bytes checked");
        if (ti.callOffset < 0) {
            st.addr = a;
            return;
        }
        const uintptr_t call = a + static_cast<uintptr_t>(ti.callOffset);
        const uintptr_t callee = GameAddr::Get(ti.calleeId);
        // The scene drain's CALL is shared with the scene node budget through framework/call_chain.h, which checks where
        // the CALL goes itself (it may already reach the budget's hook)
        if (i == T_SceneDrain) {
            if (*reinterpret_cast<const uint8_t*>(call) != 0xE8 || !callee) {
                st.status = std::format("Skipped: no CALL at {:#010x}", call);
                return;
            }
            st.how = GameAddr::IsFixed() ? "Steam 1.67.2 address, CALL checked by the call chain" : "game-address signature, CALL checked by the call chain";
            st.addr = call;
            return;
        }
        if (*reinterpret_cast<const uint8_t*>(call) != 0xE8 || !callee || CallTarget(call) != callee) {
            st.status = std::format("Skipped: the CALL at {:#010x} does not reach {} ({:#010x}; redirected by another module?)", call, GameAddr::Name(ti.calleeId), callee);
            return;
        }
        st.addr = call;
        return;
    }
    uintptr_t start = 0;
    if (g_gameVersion == GameVersion::Steam) {
        if (!MatchAt(ti.steam, ti.pattern)) {
            st.status = std::format("Skipped: bytes at {:#010x} do not match (detoured or patched by another module?)", ti.steam);
            return;
        }
        start = ti.steam;
        st.how = "pattern matches at the Steam 1.67.2 address";
    } else {
        if (st.scanned && MatchAt(st.scanned, ti.pattern)) {
            start = st.scanned;
        } else {
            uintptr_t first = 0;
            start = ScanUnique(ti.pattern, &first);
            if (!start) {
                st.status = first ? std::format("Skipped: pattern matches more than once (first at {:#010x})", first) : "Skipped: pattern not found (other game build, or patched by another module)";
                return;
            }
            st.scanned = start;
        }
        st.how = "unique pattern match";
    }
    if (ti.callOffset < 0) {
        st.addr = start;
        return;
    }
    const uintptr_t call = start + static_cast<uintptr_t>(ti.callOffset);
    if (*reinterpret_cast<const uint8_t*>(call) != 0xE8) {
        st.status = std::format("Skipped: no CALL at {:#010x}", call);
        return;
    }
    if (g_gameVersion == GameVersion::Steam && CallTarget(call) != ti.callee) {
        st.status = std::format("Skipped: the CALL at {:#010x} reaches {:#010x}, expected {:#010x} (redirected by another module?)", call, CallTarget(call), ti.callee);
        return;
    }
    st.addr = call;
}

bool WriteCallSuspended(uintptr_t call, const uint8_t bytes[5]); // hand-made hooks section

// Call-site targets: CALL rel32 -> the hook, which calls the original callee through g_orig
bool AttachCallSite(int i) {
    TargetState& st = g_targets[i];
    const uintptr_t call = st.addr;
    const uintptr_t callee = CallTarget(call);
    std::vector<BYTE> orig(5), bytes(5);
    std::memcpy(orig.data(), reinterpret_cast<const void*>(call), 5);
    bytes[0] = 0xE8;
    const int32_t rel = MemPatch::CalculateRelativeOffset(call, reinterpret_cast<uintptr_t>(kTargets[i].hook));
    std::memcpy(&bytes[1], &rel, 4);
    g_orig[i] = reinterpret_cast<void*>(callee); // set before the CALL can reach the hook
    st.patched.clear();
    if (kTargets[i].addrId != kNoAddr) {
        // counters: their CALLs may also run on a lot impostor builder thread, so the 5 bytes are written with every other
        // thread suspended and none executing inside them (the original bytes are kept in orig8 for the detach)
        std::memcpy(st.orig8, orig.data(), 5);
        if (!WriteCallSuspended(call, bytes.data())) {
            st.status = std::format("Skipped: could not patch the CALL at {:#010x} (a thread kept executing it, or the write failed)", call);
            return false;
        }
        st.attached = true;
        st.status = std::format("Timed at the CALL {:#010x} -> {:#010x} ({}; written with all threads checked)", call, callee, st.how);
        return true;
    }
    if (!MemPatch::WriteBytes(call, bytes, &st.patched, &orig)) {
        MemPatch::RestoreAll(st.patched);
        st.status = std::format("Skipped: could not patch the CALL at {:#010x}", call);
        return false;
    }
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(call), 5);
    st.attached = true;
    st.status = std::format("Timed at the CALL {:#010x} -> {:#010x} ({})", call, callee, st.how);
    return true;
}

// ---- hand-made hooks for functions many threads call (Mutex::Lock, ExecuteJob, file reads...) ----
// Detours only protects the threads passed to DetourUpdateThread, and DetourUpdateThread / the commit allocate from the
// heap, which is unsafe once other threads are suspended. Here everything is allocated first: the trampoline (the
// verified, relocation-free prologue bytes + JMP back), the thread handles. Then every other thread is suspended and its
// EIP checked: if one is inside the prologue being replaced, all are resumed and it is retried. The patch is one locked
// 8-byte write (the entry is 16-byte aligned), so no thread can fetch half of it. While threads are suspended only
// GetThreadContext, VirtualProtect, the locked write and FlushInstructionCache run. Trampolines are never freed, so a
// thread still inside one after the hook is removed stays valid.
uint8_t* g_trampPool = nullptr; // guarded by g_ctrlMutex
size_t g_trampUsed = 0;
constexpr size_t kTrampPool = 4096, kTrampSlot = 32;

uint8_t* BuildTrampoline(uintptr_t target, int len) {
    if (!g_trampPool) g_trampPool = static_cast<uint8_t*>(VirtualAlloc(nullptr, kTrampPool, MEM_COMMIT | MEM_RESERVE, PAGE_EXECUTE_READWRITE));
    if (!g_trampPool || g_trampUsed + kTrampSlot > kTrampPool) return nullptr;
    uint8_t* t = g_trampPool + g_trampUsed;
    g_trampUsed += kTrampSlot;
    std::memcpy(t, reinterpret_cast<const void*>(target), static_cast<size_t>(len));
    t[len] = 0xE9;
    const int32_t rel = static_cast<int32_t>((target + len) - (reinterpret_cast<uintptr_t>(t) + len + 5));
    std::memcpy(t + len + 1, &rel, 4);
    FlushInstructionCache(GetCurrentProcess(), t, kTrampSlot);
    return t;
}

bool WriteQwordAtomic(uintptr_t addr, const uint8_t bytes[8]) {
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(addr), 8, PAGE_EXECUTE_READWRITE, &old)) return false;
    int64_t v;
    std::memcpy(&v, bytes, 8);
    volatile int64_t* p = reinterpret_cast<volatile int64_t*>(addr);
    int64_t cur = _InterlockedCompareExchange64(p, 0, 0); // atomic read (writes only if it was 0)
    for (;;) {
        const int64_t prev = _InterlockedCompareExchange64(p, v, cur);
        if (prev == cur) break;
        cur = prev;
    }
    VirtualProtect(reinterpret_cast<void*>(addr), 8, old, &old);
    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(addr), 8);
    return true;
}

// Opens every other thread of the process (before anything is suspended)
std::vector<HANDLE> OpenOtherThreads() {
    std::vector<HANDLE> out;
    out.reserve(256);
    const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    THREADENTRY32 te;
    te.dwSize = sizeof te;
    const DWORD pid = GetCurrentProcessId(), self = GetCurrentThreadId();
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
        if (HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, te.th32ThreadID)) out.push_back(h);
    }
    CloseHandle(snap);
    return out;
}

// Writes the 5 bytes of a CALL with every other thread suspended and none executing inside them; while they are suspended
// only GetThreadContext, VirtualProtect, the copy and FlushInstructionCache run (no heap, no lock). As AttachSafe.
bool WriteCallSuspended(uintptr_t call, const uint8_t bytes[5]) {
    std::vector<HANDLE> threads = OpenOtherThreads();
    bool written = false;
    for (int attempt = 0; attempt < 100 && !written; attempt++) {
        // ---- other threads suspended ----
        for (HANDLE h : threads) SuspendThread(h);
        bool busy = false;
        for (HANDLE h : threads) {
            CONTEXT ctx;
            std::memset(&ctx, 0, sizeof ctx);
            ctx.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(h, &ctx) && ctx.Eip > call && ctx.Eip < call + 5) {
                busy = true;
                break;
            }
        }
        if (!busy) {
            DWORD old = 0;
            if (VirtualProtect(reinterpret_cast<void*>(call), 5, PAGE_EXECUTE_READWRITE, &old)) {
                std::memcpy(reinterpret_cast<void*>(call), bytes, 5);
                VirtualProtect(reinterpret_cast<void*>(call), 5, old, &old);
                FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(call), 5);
                written = true;
            }
        }
        for (HANDLE h : threads) ResumeThread(h);
        // ---- resumed ----
        if (!busy) break;
        Sleep(1);
    }
    for (HANDLE h : threads) CloseHandle(h);
    return written;
}

bool AttachSafe(int i) {
    TargetState& st = g_targets[i];
    const TargetInfo& ti = kTargets[i];
    const uintptr_t addr = st.addr;
    const int len = ti.safeLen;
    if ((addr & 7) || len < 5 || len > 8) {
        st.status = std::format("Skipped: {:#010x} is not 8-byte aligned", addr);
        return false;
    }
    if (!st.tramp) st.tramp = BuildTrampoline(addr, len);
    if (!st.tramp) {
        st.status = "Skipped: no trampoline memory";
        return false;
    }
    std::memcpy(st.orig8, reinterpret_cast<const void*>(addr), 8);
    uint8_t patch[8];
    std::memcpy(patch, st.orig8, 8);
    patch[0] = 0xE9;
    const int32_t rel = MemPatch::CalculateRelativeOffset(addr, reinterpret_cast<uintptr_t>(ti.hook));
    std::memcpy(patch + 1, &rel, 4);
    g_orig[i] = st.tramp; // before the entry can reach the hook
    std::vector<HANDLE> threads = OpenOtherThreads();
    bool written = false;
    for (int attempt = 0; attempt < 100 && !written; attempt++) {
        // ---- other threads suspended: no allocation, no lock ----
        for (HANDLE h : threads) SuspendThread(h);
        bool busy = false;
        for (HANDLE h : threads) {
            CONTEXT ctx;
            std::memset(&ctx, 0, sizeof ctx);
            ctx.ContextFlags = CONTEXT_CONTROL;
            if (GetThreadContext(h, &ctx) && ctx.Eip > addr && ctx.Eip < addr + static_cast<uintptr_t>(len)) {
                busy = true;
                break;
            }
        }
        if (!busy) written = WriteQwordAtomic(addr, patch);
        for (HANDLE h : threads) ResumeThread(h);
        // ---- resumed ----
        if (!busy) break;
        Sleep(1);
    }
    for (HANDLE h : threads) CloseHandle(h);
    if (!written) {
        g_orig[i] = reinterpret_cast<void*>(addr);
        st.status = "Skipped: a thread kept running its first instructions, or the write failed";
        return false;
    }
    st.attached = true;
    st.status = std::format("Timed ({}; hand-made hook, all threads checked)", st.how);
    return true;
}

// The JMP is a single instruction, so no thread can be inside it: the original bytes go back with one locked write
void DetachSafe(int i) {
    TargetState& st = g_targets[i];
    uint8_t now[8];
    std::memcpy(now, reinterpret_cast<const void*>(st.addr), 8);
    int32_t rel;
    std::memcpy(&rel, now + 1, 4);
    if (now[0] != 0xE9 || st.addr + 5 + static_cast<uintptr_t>(static_cast<intptr_t>(rel)) != reinterpret_cast<uintptr_t>(kTargets[i].hook)) {
        st.attached = false; // changed by someone else: leave it (the hook keeps forwarding through the trampoline)
        st.status = "Off (the entry was changed by another module after the profiler; left as it is)";
        return;
    }
    if (!WriteQwordAtomic(st.addr, st.orig8)) {
        st.status = "Restore failed: still timed";
        return;
    }
    st.attached = false;
    st.status = "Off";
}

// ---- vtable-slot targets (functions reached only through vtables: FindProvider, the RefPack stream write) ----
// No code byte changes: each slot (4-byte aligned, read-only data) is swapped with one interlocked compare-exchange while
// its page is writable, so a thread reading the slot sees either the old or the new pointer. Nothing else to protect: the
// hook calls the original function through g_orig, which stays valid after the slot is put back.
bool SwapSlot(uintptr_t slot, uintptr_t expect, uintptr_t value) {
    if (slot & 3) return false;
    DWORD old = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(slot), 4, PAGE_READWRITE, &old)) return false;
    const long prev = _InterlockedCompareExchange(reinterpret_cast<volatile long*>(slot), static_cast<long>(value), static_cast<long>(expect));
    VirtualProtect(reinterpret_cast<void*>(slot), 4, old, &old);
    return static_cast<uintptr_t>(static_cast<unsigned long>(prev)) == expect;
}

// Targets whose slots are shared with other Apex modules through framework/slot_chain.h: the site, and the module layer
// that may sit inside (or, for the wall AO gate, outside) the profiler's
struct SharedSlot {
    int target;
    SlotChain::Site site;
    SlotChain::Layer other;
    const char* otherText;
};
const SharedSlot kSharedSlots[] = {
    {T_ResLookup, SlotChain::Site::FindProvider, SlotChain::Layer::ResourceCache, "; the resource lookup cache is inside"},
    {T_RefPackCompress, SlotChain::Site::RefPackCompress, SlotChain::Layer::FastCompress, "; the fast compressor is inside"},
    {T_WallAo, SlotChain::Site::WallAoStep, SlotChain::Layer::Gate, "; the wall shading gate is outside"},
    {T_KeyList, SlotChain::Site::KeyListBase, SlotChain::Layer::ResourceCache, "; the file list cache is inside"},
    {T_KeyListDerived, SlotChain::Site::KeyListDerived, SlotChain::Layer::ResourceCache, "; the file list cache is inside"},
};
const SharedSlot* SharedSlotOf(int i) {
    for (const SharedSlot& s : kSharedSlots)
        if (s.target == i) return &s;
    return nullptr;
}

bool AttachSlots(int i) {
    TargetState& st = g_targets[i];
    const TargetInfo& ti = kTargets[i];
    if (const SharedSlot* shared = SharedSlotOf(i)) {
        // The profiler is its layer of the slot chain, whichever module installs first
        std::string error;
        g_orig[i] = reinterpret_cast<void*>(st.addr); // display only: the hook calls SlotChain::Next
        if (!SlotChain::Install(shared->site, SlotChain::Layer::FrameProfiler, ti.hook, &error)) {
            st.status = "Skipped: " + error;
            return false;
        }
        st.attached = true;
        st.status = std::format("Timed through {} vtable slot{}, a layer of the slot chain ({}{})", ti.slots, ti.slots == 1 ? "" : "s", st.how,
                                SlotChain::Installed(shared->site, shared->other) ? shared->otherText : "");
        return true;
    }
    const uintptr_t hook = reinterpret_cast<uintptr_t>(ti.hook);
    uintptr_t slot[kMaxTargetSlots] = {};
    const int n = std::min(ti.slots, kMaxTargetSlots);
    for (int k = 0; k < n; k++) {
        slot[k] = GameAddr::Get(static_cast<GameAddr::Id>(static_cast<int>(ti.slotId) + k));
        uint32_t v = 0;
        if (!slot[k] || !MemPatch::ReadBytes(slot[k], &v, 4)) {
            st.status = std::format("Skipped: vtable slot {} not found ({})", k, GameAddr::Name(static_cast<GameAddr::Id>(static_cast<int>(ti.slotId) + k)));
            return false;
        }
        if (v != st.addr) {
            st.status = std::format("Skipped: the vtable slot {:#010x} holds {:#010x}, expected {:#010x} (replaced by another module?)", slot[k], v, st.addr);
            return false;
        }
    }
    g_orig[i] = reinterpret_cast<void*>(st.addr); // before any slot can reach the hook
    int done = 0;
    while (done < n && SwapSlot(slot[done], st.addr, hook)) done++;
    if (done < n) {
        for (int k = 0; k < done; k++) SwapSlot(slot[k], hook, st.addr);
        st.status = std::format("Skipped: could not write the vtable slot {:#010x}", slot[done]);
        return false;
    }
    for (int k = 0; k < n; k++) st.slotAddr[k] = slot[k];
    st.attached = true;
    st.status = std::format("Timed through {} vtable slot{} ({})", n, n == 1 ? "" : "s", st.how);
    return true;
}

void DetachSlots(int i) {
    TargetState& st = g_targets[i];
    if (const SharedSlot* shared = SharedSlotOf(i)) {
        SlotChain::Remove(shared->site, SlotChain::Layer::FrameProfiler); // the other module's layer, if any, stays
        st.attached = false;
        st.status = "Off";
        return;
    }
    const uintptr_t hook = reinterpret_cast<uintptr_t>(kTargets[i].hook);
    bool foreign = false;
    for (uintptr_t& slot : st.slotAddr) {
        if (!slot) continue;
        if (!SwapSlot(slot, hook, st.addr)) foreign = true; // changed by someone else after us: left as it is (it may still forward to the hook)
        slot = 0;
    }
    st.attached = false;
    st.status = foreign ? "Off (a vtable slot was changed by another module after the profiler; left as it is)" : "Off";
}

bool AttachTarget(int i) {
    TargetState& st = g_targets[i];
    const TargetInfo& ti = kTargets[i];
    if (st.attached) return true;
    ResolveTarget(i);
    if (!st.addr) return false;
    if (ti.slots > 0) return AttachSlots(i);
    if (i == T_SceneDrain) {
        // Shared with the scene node budget (features/scene_budget.h): the profiler is the outer layer of the call chain
        std::string error;
        g_orig[i] = reinterpret_cast<void*>(GameAddr::Get(ti.calleeId)); // display only: the hook calls CallChain::Next
        if (!CallChain::Install(CallChain::Site::SceneDrain, CallChain::Layer::FrameProfiler, ti.hook, &error)) {
            st.status = "Skipped: " + error;
            return false;
        }
        st.attached = true;
        st.status = std::format("Timed at the CALL {:#010x}, outer layer of the call chain ({}; written with all threads checked{})", st.addr, st.how,
                                CallChain::Installed(CallChain::Site::SceneDrain, CallChain::Layer::SceneBudget) ? "; the scene node budget is inside" : "");
        return true;
    }
    if (ti.callOffset >= 0) return AttachCallSite(i);
    if (i == T_ObjectById) {
        // Shared with the object lookup index (features/object_index.h): the profiler is the outer layer of the entry chain
        std::string error;
        g_orig[i] = reinterpret_cast<void*>(st.addr); // display only: the hook calls EntryChain::Next
        if (!EntryChain::Install(EntryChain::Site::ObjectById, EntryChain::Layer::FrameProfiler, ti.hook, &error)) {
            st.status = "Skipped: " + error;
            return false;
        }
        st.attached = true;
        st.status = std::format("Timed at the entry, outer layer of the entry chain ({}; JMP written with all threads checked{})", st.how,
                                EntryChain::Installed(EntryChain::Site::ObjectById, EntryChain::Layer::ObjectIndex) ? "; the object lookup index is inside" : "");
        return true;
    }
    if (i == T_DxtEncode1 || i == T_DxtEncode5) {
        // Entries shared with the fast DXT encoder (features/fast_dxt.h): the profiler is the outer layer of the entry chain
        const EntryChain::Site site = i == T_DxtEncode5 ? EntryChain::Site::DxtEncode5 : EntryChain::Site::DxtEncode1;
        std::string error;
        g_orig[i] = reinterpret_cast<void*>(st.addr); // display only: the hook calls EntryChain::Next
        if (!EntryChain::Install(site, EntryChain::Layer::FrameProfiler, ti.hook, &error)) {
            st.status = "Skipped: " + error;
            return false;
        }
        st.attached = true;
        st.status = std::format("Timed at the entry, outer layer of the entry chain ({}; JMP written with all threads checked{})", st.how,
                                EntryChain::Installed(site, EntryChain::Layer::FastDxt) ? "; the fast encoder is inside" : "");
        return true;
    }
    if (ti.safeLen > 0) {
        if (!MatchAt(st.addr, ti.pattern)) {
            st.status = std::format("Skipped: entry bytes at {:#010x} changed", st.addr);
            return false;
        }
        return AttachSafe(i);
    }
    if (!MatchAt(st.addr, ti.pattern)) {
        st.status = std::format("Skipped: entry bytes at {:#010x} changed", st.addr);
        return false;
    }
    g_orig[i] = reinterpret_cast<void*>(st.addr);
    std::lock_guard<std::recursive_mutex> detoursLock(DetourBatch::Lock());
    if (DetourTransactionBegin() != NO_ERROR) {
        st.status = "Skipped: DetourTransactionBegin failed";
        return false;
    }
    DetourUpdateThread(GetCurrentThread());
    LONG err = DetourAttach(&g_orig[i], ti.hook);
    if (err != NO_ERROR) {
        DetourTransactionAbort();
        g_orig[i] = reinterpret_cast<void*>(st.addr);
        st.status = std::format("Skipped: DetourAttach failed ({})", err);
        return false;
    }
    err = DetourTransactionCommit();
    if (err != NO_ERROR) {
        g_orig[i] = reinterpret_cast<void*>(st.addr);
        st.status = std::format("Skipped: DetourTransactionCommit failed ({})", err);
        return false;
    }
    st.attached = true;
    st.status = std::format("Timed ({})", st.how);
    return true;
}

void DetachTarget(int i) {
    TargetState& st = g_targets[i];
    if (!st.attached) return;
    if (kTargets[i].slots > 0) {
        DetachSlots(i);
        return;
    }
    if (i == T_SceneDrain) {
        if (!CallChain::Remove(CallChain::Site::SceneDrain, CallChain::Layer::FrameProfiler)) { // the budget's layer, if any, stays
            st.status = "Restore failed: still timed";
            return;
        }
        st.attached = false;
        st.status = "Off";
        return;
    }
    if (i == T_ObjectById) {
        if (!EntryChain::Remove(EntryChain::Site::ObjectById, EntryChain::Layer::FrameProfiler)) { // the index's layer, if any, stays
            st.status = "Restore failed: still timed";
            return;
        }
        st.attached = false;
        st.status = "Off";
        return;
    }
    if (i == T_DxtEncode1 || i == T_DxtEncode5) {
        const EntryChain::Site site = i == T_DxtEncode5 ? EntryChain::Site::DxtEncode5 : EntryChain::Site::DxtEncode1;
        if (!EntryChain::Remove(site, EntryChain::Layer::FrameProfiler)) { // the fast encoder's layer, if any, stays
            st.status = "Restore failed: still timed";
            return;
        }
        st.attached = false;
        st.status = "Off";
        return;
    }
    if (kTargets[i].safeLen > 0) {
        DetachSafe(i);
        return;
    }
    if (kTargets[i].callOffset >= 0) { // g_orig keeps the callee, so a call already inside the hook still completes
        if (*reinterpret_cast<const uint8_t*>(st.addr) != 0xE8 || CallTarget(st.addr) != reinterpret_cast<uintptr_t>(kTargets[i].hook)) {
            // another module rewrote the CALL after us: leave its bytes alone (the hook keeps forwarding to the callee)
            st.patched.clear();
            st.attached = false;
            st.status = "Off (the CALL was changed by another module after the profiler; left as it is)";
            return;
        }
        if (kTargets[i].addrId != kNoAddr) { // counters: restored as they were written (all threads checked)
            if (!WriteCallSuspended(st.addr, st.orig8)) {
                st.status = "Restore failed: still timed";
                LOG_ERROR(std::format("[FrameProfiler] Could not restore the CALL of {}", kTargets[i].name));
                return;
            }
            st.attached = false;
            st.status = "Off";
            return;
        }
        if (!MemPatch::RestoreAll(st.patched)) {
            st.status = "Restore failed: still timed";
            LOG_ERROR(std::format("[FrameProfiler] Could not restore the CALL of {}", kTargets[i].name));
            return;
        }
        FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(st.addr), 5);
        st.attached = false;
        st.status = "Off";
        return;
    }
    std::lock_guard<std::recursive_mutex> detoursLock(DetourBatch::Lock());
    if (DetourTransactionBegin() != NO_ERROR) {
        st.status = "Detach failed: still timed";
        return;
    }
    DetourUpdateThread(GetCurrentThread());
    if (DetourDetach(&g_orig[i], kTargets[i].hook) != NO_ERROR) {
        DetourTransactionAbort();
        st.status = "Detach failed: still timed";
        LOG_ERROR(std::format("[FrameProfiler] Could not detach {}", kTargets[i].name));
        return;
    }
    if (DetourTransactionCommit() != NO_ERROR) {
        st.status = "Detach failed: still timed";
        LOG_ERROR(std::format("[FrameProfiler] Could not detach {}", kTargets[i].name));
        return;
    }
    st.attached = false;
    st.status = "Off";
}

// ---- sampling: code classes and the per-hitch summary ----
enum CodeClass : uint8_t { kClsGame, kClsGraphics, kClsSystem, kClsOurAsi, kClsOtherAsi, kClsOther, kClsCount };
const char* const kClassNames[kClsCount] = {"TS3W", "DXVK/driver", "system", "Apex", "other ASI", "other"};
constexpr int kTopN = 8;
constexpr int kTopWait = 4;

struct HitchSamples {
    uint16_t n[2] = {};                // samples of the render / simulation thread inside the frame interval
    uint16_t cls[2][kClsCount] = {};   // ... per code class
    uint32_t fnKey[2][kTopN] = {};     // hottest code: TS3W function start (heuristic) or module base
    uint16_t fnCount[2][kTopN] = {};
    uint8_t fnCls[2][kTopN] = {};
    uint32_t site[kTopN] = {};         // render thread: TS3W call sites (return addresses) found on the stack
    uint16_t siteCount[kTopN] = {};
    uint32_t wait[kTopWait] = {};      // render thread: first TS3W return address of samples in system code
    uint16_t waitCount[kTopWait] = {};
};

// Render thread, per hitch: the heaviest services, jobs and waits of the frame
constexpr int kTopSvc = 8, kTopJobs = 6, kTopWaits = 6;
struct HitchDetail {
    uint32_t svcKey[kTopSvc] = {}; // service update function
    uint32_t svcVt[kTopSvc] = {};  // its vtable
    float svcMs[kTopSvc] = {}, svcSelfMs[kTopSvc] = {};
    uint16_t svcCalls[kTopSvc] = {};
    uint32_t jobKey[kTopJobs] = {}; // job function, or kRemoteCallBit | remote call method
    float jobMs[kTopJobs] = {}, jobSelfMs[kTopJobs] = {};
    uint16_t jobCalls[kTopJobs] = {};
    uint32_t waitKey[kTopWaits] = {}; // job waited for, or the caller's return address for mutex / semaphore
    uint8_t waitKind[kTopWaits] = {};
    float waitMs[kTopWaits] = {};
    uint16_t waitCount[kTopWaits] = {};
    uint32_t readBytes = 0; // FileStream::Read on the render thread
};

// ---- counters of one frame interval (thread buckets: 0 render, 1 simulation, 2 other) ----
struct CounterFrame {
    float incl[3][kNC] = {};              // ms, inclusive (outermost calls), per bucket
    uint32_t calls[3][kNC] = {};
    float maxMs[2][kNC] = {};             // longest single call: [0] render thread, [1] other threads
    uint64_t extra[3][kExtraCount] = {};  // Extra, per bucket
};

bool CounterActive(const CounterFrame& f, int k) {
    return f.calls[0][k] || f.calls[1][k] || f.calls[2][k];
}

// Dominant cause of a hitch: the largest single item on the render thread, exactly as research\perf2\tools\dom.pl picks it
// (render self categories except "Services (self)" / "Jobs (self)", each service's self time, each job's self time,
// Unattributed).
enum DomKind : uint8_t { kDomNone, kDomCategory, kDomService, kDomJob, kDomUnattributed };

// ---- per-frame record (also the hitch record) ----
struct HitchRecord {
    uint64_t frame = 0;
    double tSec = 0;
    float frameMs = 0, medianMs = 0, thresholdMs = 0;
    float cpuMs = 0, presentMs = 0, limiterMs = 0, modMs = 0, unattributedMs = 0;
    float render[kCatCount] = {};     // render thread, exclusive
    float renderIncl[kCatCount] = {}; // render thread, inclusive (outermost calls)
    float sim[kCatCount] = {};        // simulation thread, exclusive
    float other[kCatCount] = {};      // other threads, exclusive
    uint32_t calls[kCatCount] = {};   // all threads
    uint32_t gameDraws = 0, endFrameDraws = 0, dip = 0, dp = 0;
    uint64_t prims = 0;
    uint32_t setTexture = 0, setShader = 0, shaderConst = 0, setRT = 0;
    uint32_t createTex = 0, createVS = 0, createPS = 0, createRT = 0;
    uint32_t lotsPromoted = 0, lotsDemoted = 0;
    uint32_t pageFaults = 0; // the process's page faults during the frame
    int8_t camera = -1; // 1 moving, 0 still, -1 unknown
    bool foreground = true;
    bool stateCounted = false;
    HitchSamples samples; // filled for hitches while the sampler runs
    HitchDetail detail;   // filled for hitches
    CounterFrame counters;
    // hitches: the dominant cause and the counter with the largest render-thread self time
    uint8_t domKind = kDomNone;
    uint32_t domKey = 0; // category, service update function or job key
    uint32_t domAux = 0; // service vtable
    float domMs = 0;
    int8_t topCounter = -1; // counter index (0..kNC-1), -1 none
};

// page faults per frame, [0] other frames, [1] hitch frames (render thread; reset by Clear)
uint64_t g_pfSum[2] = {}, g_pfFrames[2] = {};
uint32_t g_lastPageFaults = 0;

// ---- render-thread statistics ----
constexpr int kMedianWindow = 120;
constexpr int kWarmupFrames = 30;
constexpr int kGraphFrames = 300;
constexpr int kLiveFrames = 60;
constexpr int kHitchRing = 200;
constexpr int kFineBins = 2000;  // 0.05 ms steps up to 100 ms
constexpr int kCoarseBins = 900; // 1 ms steps up to 1000 ms
constexpr int kBins = kFineBins + kCoarseBins + 1;

struct LiveSample {
    float frameMs, cpuMs, presentMs, limiterMs, modMs;
    float gameDraws, endFrameDraws, setTexture, setShader, shaderConst, setRT;
};

struct Stats {
    uint64_t frames = 0, hitches = 0;
    uint64_t framesMoving = 0, framesStill = 0, hitchesMoving = 0, hitchesStill = 0;
    double maxFrameMs = 0;
    std::array<uint32_t, kBins> hist{};
    double render[kCatCount] = {}, sim[kCatCount] = {}, other[kCatCount] = {};
    uint64_t bucketCalls[3][kCatCount] = {};
    double unattributed = 0;
    uint64_t lotsPromoted = 0, lotsDemoted = 0;
    // counters since Clear
    double cIncl[3][kNC] = {};
    uint64_t cCalls[3][kNC] = {};
    uint64_t cExtra[3][kExtraCount] = {};
    double cMax[2][kNC] = {};
};

Stats g_stats;
float g_graph[kGraphFrames] = {};
int g_graphPos = 0, g_graphCount = 0;
CounterFrame g_cLive[kLiveFrames] = {}; // the counters of the last frames (same positions as g_live)
LiveSample g_live[kLiveFrames] = {};
int g_livePos = 0, g_liveCount = 0;
float g_medianRing[kMedianWindow] = {};
int g_medianPos = 0, g_medianCount = 0;
HitchRecord g_hitches[kHitchRing];
int g_hitchPos = 0, g_hitchCount = 0;
HitchRecord g_cur;
uint32_t g_frameBucketCalls[3][kCatCount] = {};
float g_lastThreshold = 0, g_lastMedian = 0;
uint64_t g_frameIndex = 0, g_enableTicks = 0, g_lastBoundary = 0;
std::atomic<bool> g_needBaseline{true};

struct Seen {
    uint64_t excl[kCatCount];
    uint64_t incl[kCatCount];
    uint32_t calls[kCatCount];
    uint64_t extra[kExtraCount];
};
Seen g_seen[kMaxSlots] = {};

// ---- hitch queue to the writer thread (single producer: render thread; single consumer: writer) ----
constexpr uint32_t kQueueSize = 256;
HitchRecord g_queue[kQueueSize];
std::atomic<uint32_t> g_qHead{0}, g_qTail{0};
std::atomic<uint32_t> g_qDropped{0}, g_qWritten{0};
std::atomic<bool> g_writerRunning{false};

// ---- optional per-hook registry timing (all access under the registry mutex, see frame_profiler.h) ----
struct RegName {
    std::string name;
    uint64_t ticks = 0;
    uint32_t calls = 0;
};
struct RegPtr {
    const char* ptr;
    int idx;
};
std::vector<RegName> g_regNames;
std::vector<RegPtr> g_regPtrs;
uint32_t g_regFrames = 0;
uint64_t g_regWindowStart = 0;
struct RegDisplay {
    std::string name;
    float msPerFrame;
    float callsPerFrame;
};
std::vector<RegDisplay> g_regDisplay; // render thread

// ---- statistical sampler ----
// A dedicated thread wakes about g_sampleHz times a second (high-resolution waitable timer), suspends the render and/or
// simulation thread, reads its context (EIP, ESP), copies up to 4 KB of its stack and resumes it. Between
// SuspendThread and ResumeThread it only calls GetThreadContext and memcpy into a preallocated buffer: no heap, no
// logging, nothing that takes a lock the suspended thread could hold. The copy is bounded by the thread's stack region
// (VirtualQuery on the first ESP, done while the thread runs), so it cannot fault; it is SEH-guarded anyway.
// After ResumeThread the copy is scanned for dwords that point into TS3W's .text right after a CALL instruction
// (E8 rel32 reaching .text, or the FF /2 forms): candidate return addresses, a stack walk that needs no frame pointers
// (it can also pick up stale return addresses left in dead stack slots, so treat deep entries as hints). The samples go to
// a single-producer / single-consumer ring; at each frame boundary the render thread assigns them to the frame interval
// they were taken in (same clock as the frames).
#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif
constexpr int kMaxRets = 12;
constexpr size_t kStackCopy = 4096; // 512 until 30/09: DXVK / driver / kernel frames hid the game's in ~24% of the hitch samples
constexpr uint32_t kSampleRing = 8192;
constexpr uint32_t kUnknownKey = 0xFFFFFFFFu;

struct Sample {
    uint64_t t;
    uint32_t eip;
    uint32_t ret[kMaxRets];
    uint32_t firstOut; // EIP in system code: the first return address on the stack in any other module (TS3W, DXVK, an ASI...), 0 none
    uint8_t nRet;
    uint8_t thread; // 0 render, 1 simulation
};
// Sampler thread: for an EIP in system code, the first dword of the stack copy that is a return address after a CALL in a
// module that is not system code (defined with the module table below)
uint32_t FirstOutsideSystem(uint32_t eip, const uint32_t* stack, size_t dwords);

Sample g_sampleRing[kSampleRing];
std::atomic<uint32_t> g_sHead{0}, g_sTail{0};
std::atomic<uint32_t> g_sDropped{0};
std::atomic<uint32_t> g_sTaken{0};       // written by the sampler thread only
std::atomic<uint64_t> g_sPausedTicks{0}; // time the targets spent suspended, sampler thread only
HANDLE g_samplerThread = nullptr;        // guarded by g_ctrlMutex
std::atomic<bool> g_samplerRunning{false};
std::atomic<bool> g_samplerStop{false};
uint32_t g_stackBuf[kStackCopy / 4];     // sampler thread only

// No C++ objects in here (SEH)
bool CopyStack(void* dst, const void* src, size_t n) {
    __try {
        std::memcpy(dst, src, n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// r points into .text (r >= begin + 7): is the instruction ending at r a CALL?
bool IsCallSite(uintptr_t r, uintptr_t begin, uintptr_t end) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(r);
    if (p[-5] == 0xE8) { // call rel32, target inside .text
        int32_t rel;
        std::memcpy(&rel, p - 4, 4);
        const uintptr_t target = r + static_cast<uintptr_t>(static_cast<intptr_t>(rel));
        if (target >= begin && target < end) return true;
    }
    if (p[-6] == 0xFF && (p[-5] == 0x15 || ((p[-5] & 0xF8) == 0x90 && p[-5] != 0x94))) return true; // call [disp32] / [reg+disp32]
    if (p[-7] == 0xFF && p[-6] == 0x94) return true;                                                  // call [sib+disp32]
    if (p[-4] == 0xFF && p[-3] == 0x54) return true;                                                  // call [sib+disp8]
    if (p[-3] == 0xFF && (((p[-2] & 0xF8) == 0x50 && p[-2] != 0x54) || p[-2] == 0x14)) return true;   // call [reg+disp8] / [sib]
    if (p[-2] == 0xFF && ((p[-1] & 0xF8) == 0xD0 || ((p[-1] & 0xF8) == 0x10 && p[-1] != 0x14 && p[-1] != 0x15))) return true; // call reg / [reg]
    return false;
}

struct SampleTarget {
    DWORD tid = 0;
    HANDLE h = nullptr;
    uintptr_t stackLow = 0, stackEnd = 0; // the thread's stack reservation start and committed end
};

void Retarget(SampleTarget& tg, DWORD tid) {
    if (tg.h) CloseHandle(tg.h);
    tg = SampleTarget{};
    tg.tid = tid;
    if (tid && tid != GetCurrentThreadId()) tg.h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION, FALSE, tid);
}

void TakeSample(SampleTarget& tg, uint8_t which, uintptr_t textBegin, uintptr_t textEnd) {
    CONTEXT ctx;
    std::memset(&ctx, 0, sizeof ctx);
    ctx.ContextFlags = CONTEXT_CONTROL;
    size_t copied = 0;
    const uint64_t t0 = Now();
    if (SuspendThread(tg.h) == static_cast<DWORD>(-1)) return;
    // ---- target suspended: GetThreadContext and memcpy only ----
    const bool ok = GetThreadContext(tg.h, &ctx) != FALSE;
    const uintptr_t esp = ctx.Esp;
    if (ok && tg.stackEnd && esp >= tg.stackLow && esp < tg.stackEnd) {
        copied = std::min<size_t>(kStackCopy, tg.stackEnd - esp) & ~static_cast<size_t>(3);
        if (!CopyStack(g_stackBuf, reinterpret_cast<const void*>(esp), copied)) copied = 0;
    }
    ResumeThread(tg.h);
    // ---- target running again ----
    const uint64_t t1 = Now();
    Add64(g_sPausedTicks, t1 - t0);
    if (!ok) return;
    if (!tg.stackEnd) { // first sample of this thread: its stack region bounds every later copy
        MEMORY_BASIC_INFORMATION mbi;
        if (VirtualQuery(reinterpret_cast<const void*>(esp), &mbi, sizeof mbi) && mbi.State == MEM_COMMIT) {
            tg.stackLow = reinterpret_cast<uintptr_t>(mbi.AllocationBase);
            tg.stackEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
        }
    }
    const uint32_t h = g_sHead.load(std::memory_order_relaxed);
    if (h - g_sTail.load(std::memory_order_acquire) >= kSampleRing) {
        g_sDropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    Sample& s = g_sampleRing[h % kSampleRing];
    s.t = t0;
    s.eip = ctx.Eip;
    s.thread = which;
    s.nRet = 0;
    for (size_t k = 0; k < copied / 4 && s.nRet < kMaxRets; k++) {
        const uintptr_t v = g_stackBuf[k];
        if (v >= textBegin + 7 && v < textEnd && IsCallSite(v, textBegin, textEnd)) s.ret[s.nRet++] = static_cast<uint32_t>(v);
    }
    s.firstOut = FirstOutsideSystem(ctx.Eip, g_stackBuf, copied / 4);
    g_sHead.store(h + 1, std::memory_order_release);
    Add32(g_sTaken, 1);
}

DWORD WINAPI SamplerProc(LPVOID) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    if (!timer) timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_ALL_ACCESS); // older Windows: timer resolution
    const TextSection text = GetText();
    SampleTarget targets[2];
    while (!g_samplerStop.load(std::memory_order_relaxed)) {
        const int hz = std::clamp(g_sampleHz.load(std::memory_order_relaxed), 250, 4000);
        LARGE_INTEGER due;
        due.QuadPart = -static_cast<LONGLONG>(10'000'000 / hz); // relative, 100 ns units
        if (timer && SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) WaitForSingleObject(timer, 50);
        else Sleep(1);
        for (uint8_t w = 0; w < 2; w++) {
            const bool want = w == 0 ? g_sampleRender.load(std::memory_order_relaxed) : g_sampleSim.load(std::memory_order_relaxed);
            const DWORD tid = want ? (w == 0 ? g_renderTid.load(std::memory_order_relaxed) : g_simTid.load(std::memory_order_relaxed)) : 0;
            if (tid != targets[w].tid) Retarget(targets[w], tid);
            if (targets[w].h) TakeSample(targets[w], w, text.begin, text.begin + text.size);
        }
    }
    for (auto& t : targets)
        if (t.h) CloseHandle(t.h);
    if (timer) CloseHandle(timer);
    return 0;
}

std::string OwnModuleText(); // below, with the module table

// Caller holds g_ctrlMutex
void UpdateSamplerLocked() {
    const bool want = g_enabled.load() && (g_sampleRender.load() || g_sampleSim.load());
    if (want && !g_samplerThread) {
        GetText(); // initialised here, not in the sampler
        g_samplerStop.store(false);
        g_samplerThread = CreateThread(nullptr, 0, SamplerProc, nullptr, 0, nullptr);
        if (!g_samplerThread) LOG_ERROR("[FrameProfiler] Could not start the sampler thread");
        else LOG_INFO("[FrameProfiler] Sampler on");
        static bool ownLogged = false; // once per session: what the "apexradiance.asi fn~+RVA" keys refer to
        if (!ownLogged) {
            ownLogged = true;
            LOG_INFO("[FrameProfiler] " + OwnModuleText());
        }
        g_samplerRunning.store(g_samplerThread != nullptr);
    } else if (!want && g_samplerThread) {
        g_samplerStop.store(true);
        WaitForSingleObject(g_samplerThread, 2000);
        CloseHandle(g_samplerThread);
        g_samplerThread = nullptr;
        LOG_INFO("[FrameProfiler] Sampler off");
        g_samplerRunning.store(false);
    }
}

// ---- module table (append-only: written by the render thread, read by the writer thread too) ----
struct ModuleEntry {
    uintptr_t base;
    uintptr_t end;
    uint8_t cls;
    char name[40]; // lower case
};
constexpr int kMaxModules = 384;
ModuleEntry g_modules[kMaxModules];
std::atomic<int> g_moduleCount{0};
uint64_t g_modulesRefreshTick = 0; // render thread

bool EndsWith(const char* s, const char* suffix) {
    const size_t a = std::strlen(s), b = std::strlen(suffix);
    return a >= b && std::strcmp(s + a - b, suffix) == 0;
}
bool StartsWith(const char* s, const char* prefix) {
    return std::strncmp(s, prefix, std::strlen(prefix)) == 0;
}

uint8_t ClassifyModule(HMODULE h, const char* n) {
    if (h == GetModuleHandleW(nullptr)) return kClsGame;
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&ClassifyModule), &self);
    if (h == self) return kClsOurAsi;
    if (EndsWith(n, ".asi")) return kClsOtherAsi;
    if (!std::strcmp(n, "d3d9.dll") || !std::strcmp(n, "dxgi.dll") || !std::strcmp(n, "d3d11.dll") || !std::strcmp(n, "vulkan-1.dll") || StartsWith(n, "nv") ||
        StartsWith(n, "amd") || StartsWith(n, "ati") || StartsWith(n, "igvk") || StartsWith(n, "igd"))
        return kClsGraphics;
    if (!std::strcmp(n, "ntdll.dll") || !std::strcmp(n, "kernelbase.dll") || !std::strcmp(n, "kernel32.dll") || !std::strcmp(n, "win32u.dll") ||
        !std::strcmp(n, "user32.dll") || !std::strcmp(n, "gdi32.dll") || !std::strcmp(n, "gdi32full.dll") || StartsWith(n, "wow64"))
        return kClsSystem;
    return kClsOther;
}

// Render thread only (the single writer)
void RefreshModules() {
    HMODULE mods[512];
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &needed)) return;
    const int count = std::min<int>(static_cast<int>(needed / sizeof(HMODULE)), 512);
    int n = g_moduleCount.load(std::memory_order_relaxed);
    for (int i = 0; i < count && n < kMaxModules; i++) {
        const uintptr_t base = reinterpret_cast<uintptr_t>(mods[i]);
        bool known = false;
        for (int k = 0; k < n && !known; k++) known = g_modules[k].base == base;
        if (known) continue;
        MODULEINFO mi;
        wchar_t wname[MAX_PATH];
        if (!GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof mi) || !GetModuleBaseNameW(GetCurrentProcess(), mods[i], wname, MAX_PATH)) continue;
        ModuleEntry& e = g_modules[n];
        e.base = base;
        e.end = base + mi.SizeOfImage;
        size_t j = 0;
        for (; wname[j] && j < sizeof(e.name) - 1; j++) {
            wchar_t c = wname[j];
            if (c >= L'A' && c <= L'Z') c = static_cast<wchar_t>(c + 32);
            e.name[j] = c < 128 ? static_cast<char>(c) : '?';
        }
        e.name[j] = 0;
        e.cls = ClassifyModule(mods[i], e.name);
        g_moduleCount.store(++n, std::memory_order_release);
    }
}

int FindModule(uintptr_t a) {
    const int n = g_moduleCount.load(std::memory_order_acquire);
    for (int i = 0; i < n; i++)
        if (a >= g_modules[i].base && a < g_modules[i].end) return i;
    return -1;
}

// ---- sampler thread: the module table sorted by base (rebuilt when the render thread appended modules) ----
struct SortedModule {
    uintptr_t base, end;
    uint8_t cls;
};
SortedModule g_sortedModules[kMaxModules]; // sampler thread only
int g_sortedCount = 0, g_sortedSeen = -1;

int SortedFind(uintptr_t a) {
    int lo = 0, hi = g_sortedCount - 1;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        if (a < g_sortedModules[mid].base) hi = mid - 1;
        else if (a >= g_sortedModules[mid].end) lo = mid + 1;
        else return mid;
    }
    return -1;
}

// No C++ objects (SEH): the module's bytes before r may be unreadable
bool IsCallSiteGuarded(uintptr_t r, uintptr_t begin, uintptr_t end) {
    __try {
        return IsCallSite(r, begin, end);
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

uint32_t FirstOutsideSystem(uint32_t eip, const uint32_t* stack, size_t dwords) {
    const int n = g_moduleCount.load(std::memory_order_acquire);
    if (n != g_sortedSeen) {
        for (int i = 0; i < n; i++) g_sortedModules[i] = {g_modules[i].base, g_modules[i].end, g_modules[i].cls};
        std::sort(g_sortedModules, g_sortedModules + n, [](const SortedModule& a, const SortedModule& b) { return a.base < b.base; });
        g_sortedCount = n;
        g_sortedSeen = n;
    }
    const int em = SortedFind(eip);
    if (em < 0 || g_sortedModules[em].cls != kClsSystem) return 0;
    for (size_t k = 0; k < dwords; k++) {
        const uintptr_t v = stack[k];
        const int m = SortedFind(v);
        if (m < 0 || g_sortedModules[m].cls == kClsSystem || v < g_sortedModules[m].base + 0x1000) continue;
        if (IsCallSiteGuarded(v, g_sortedModules[m].base, g_sortedModules[m].end)) return static_cast<uint32_t>(v);
    }
    return 0;
}

// The exported function of module mi at or before `a` ("NtWaitForAlertByThreadId+0xC"), or "" (report threads)
bool ExportNearRaw(uintptr_t base, uintptr_t a, char* out, size_t outSize) {
    __try {
        const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS32*>(base + dos->e_lfanew);
        const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        if (!dir.VirtualAddress) return false;
        const auto ex = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + dir.VirtualAddress);
        const auto funcs = reinterpret_cast<const DWORD*>(base + ex->AddressOfFunctions);
        const auto names = reinterpret_cast<const DWORD*>(base + ex->AddressOfNames);
        const auto ords = reinterpret_cast<const WORD*>(base + ex->AddressOfNameOrdinals);
        const DWORD rva = static_cast<DWORD>(a - base);
        DWORD best = 0;
        const char* bestName = nullptr;
        for (DWORD i = 0; i < ex->NumberOfNames; i++) {
            const DWORD f = funcs[ords[i]];
            if (f >= dir.VirtualAddress && f < dir.VirtualAddress + dir.Size) continue; // forwarder
            if (f <= rva && f >= best) {
                best = f;
                bestName = reinterpret_cast<const char*>(base + names[i]);
            }
        }
        if (!bestName) return false;
        _snprintf_s(out, outSize, _TRUNCATE, "%s+0x%X", bestName, rva - best);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

std::string ExportNear(int mi, uintptr_t a) {
    char buf[128];
    if (mi < 0 || !ExportNearRaw(g_modules[mi].base, a, buf, sizeof buf)) return "";
    return buf;
}

// "TS3W XXXXXXXX" or "module+RVA"
std::string CallerText(uint32_t a) {
    const int mi = FindModule(a);
    if (mi < 0) return std::format("{:08X}", a);
    if (g_modules[mi].cls == kClsGame) return std::format("TS3W {:08X}", a);
    return std::format("{}+{:X}", g_modules[mi].name, a - static_cast<uint32_t>(g_modules[mi].base));
}

// Render thread: FindModule, refreshing the table (at most once a second) for addresses in modules not seen yet
int ModuleOf(uintptr_t a) {
    int i = FindModule(a);
    if (i < 0) {
        const uint64_t now = GetTickCount64();
        if (g_moduleCount.load() == 0 || now - g_modulesRefreshTick >= 1000) {
            g_modulesRefreshTick = now;
            RefreshModules();
            i = FindModule(a);
        }
    }
    return i;
}

uint8_t KeyClass(uint32_t key) {
    if (key == kUnknownKey) return kClsOther;
    const int mi = FindModule(key);
    return mi >= 0 ? g_modules[mi].cls : kClsOther;
}

// Code key as text: "TS3W fn~XXXXXXXX" (function start, heuristic) or the module name
std::string KeyText(uint32_t key) {
    if (key == kUnknownKey) return "(no module)";
    const int mi = FindModule(key);
    if (mi >= 0 && g_modules[mi].cls == kClsGame) return std::format("TS3W fn~{:08X}", key);
    if (mi >= 0 && g_modules[mi].cls == kClsOurAsi) return std::format("{} fn~+{:X}", g_modules[mi].name, key - static_cast<uint32_t>(g_modules[mi].base));
    if (mi >= 0) return g_modules[mi].name;
    return std::format("{:08X}", key);
}

inline uint32_t Hash32(uint32_t k) {
    k ^= k >> 16;
    k *= 0x7feb352dU;
    k ^= k >> 15;
    k *= 0x846ca68bU;
    k ^= k >> 16;
    return k;
}

// Start of the TS3W function containing eip, by MSVC's layout: functions start 16-byte aligned after int3 (CC) padding.
// Checked on the 4126 functions of the RE dump: 94% start that way, 5.5% follow a RET without padding (those are merged
// with the function before them), and 0.1% of the aligned positions inside bodies follow a CC byte (split). A grouping
// key for the tables, not an exact symbol: resolve it with the decompile. Cached (direct-mapped).
constexpr uint32_t kFnCache = 16384;
uint32_t g_fnCacheKey[kFnCache] = {}, g_fnCacheVal[kFnCache] = {}; // render thread

// This ASI's own .text and PE TimeDateStamp (the sampler keys Apex code by RVA; resolve them with ApexRadiance.map of the
// same build, which the Release configuration writes next to the .asi)
struct OwnModule {
    uintptr_t base = 0;
    uint32_t size = 0;
    uint32_t timeDateStamp = 0;
    TextSection text;
};
const OwnModule& Own() {
    static const OwnModule own = [] {
        OwnModule m;
        HMODULE self = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&Own), &self) || !self)
            return m;
        m.base = reinterpret_cast<uintptr_t>(self);
        const auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(m.base);
        const auto nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(m.base + dos->e_lfanew);
        m.size = nt->OptionalHeader.SizeOfImage;
        m.timeDateStamp = nt->FileHeader.TimeDateStamp;
        const IMAGE_SECTION_HEADER* s = IMAGE_FIRST_SECTION(nt);
        for (WORD i = 0; i < nt->FileHeader.NumberOfSections; i++, s++) {
            if (std::strncmp(reinterpret_cast<const char*>(s->Name), ".text", IMAGE_SIZEOF_SHORT_NAME) == 0) {
                m.text.begin = m.base + s->VirtualAddress;
                m.text.size = s->Misc.VirtualSize;
                break;
            }
        }
        return m;
    }();
    return own;
}

std::string OwnModuleText() {
    const OwnModule& m = Own();
    return std::format("Apex code in the samples: apexradiance.asi base {:08X}, size {:X}, PE TimeDateStamp {:08X}; keys 'fn~+RVA' = function start guessed from "
                       "the int3 padding, as an RVA: resolve with ApexRadiance.map of this build (map address - preferred base)",
                       m.base, m.size, m.timeDateStamp);
}

uint32_t FnStartIn(uint32_t eip, const TextSection& t);

uint32_t FnStartGuess(uint32_t eip) {
    return FnStartIn(eip, GetText());
}

// The same guess inside this ASI's own code (the key stays an absolute address; KeyText prints it as an RVA)
uint32_t OwnFnStartGuess(uint32_t eip) {
    return FnStartIn(eip, Own().text);
}

uint32_t FnStartIn(uint32_t eip, const TextSection& t) {
    const uint32_t slot = Hash32(eip) & (kFnCache - 1);
    if (g_fnCacheKey[slot] == eip && eip) return g_fnCacheVal[slot];
    uint32_t result = eip & ~0xFu;
    if (eip >= t.begin + 16 && eip < t.begin + t.size) {
        uintptr_t p = eip & ~static_cast<uintptr_t>(0xF);
        for (int k = 0; k < 4096 && p > t.begin; k++, p -= 16) {
            if (*reinterpret_cast<const uint8_t*>(p - 1) == 0xCC) {
                result = static_cast<uint32_t>(p);
                break;
            }
        }
    }
    g_fnCacheKey[slot] = eip;
    g_fnCacheVal[slot] = result;
    return result;
}

// Open-addressing counter (key 0 = empty slot; key 0 is counted as 1). No allocation.
template <uint32_t N> struct CountTable {
    uint32_t keys[N];
    uint32_t counts[N];
    uint32_t used;
    uint32_t lost; // samples not counted because the table was full
    void Clear() { std::memset(this, 0, sizeof *this); }
    void Add(uint32_t key) {
        if (!key) key = 1;
        uint32_t i = Hash32(key) & (N - 1);
        for (uint32_t probe = 0; probe < N; probe++, i = (i + 1) & (N - 1)) {
            if (keys[i] == key) {
                counts[i]++;
                return;
            }
            if (!keys[i]) {
                if (used * 10 >= N * 9) break;
                keys[i] = key;
                counts[i] = 1;
                used++;
                return;
            }
        }
        lost++;
    }
    uint32_t Get(uint32_t key) const {
        if (!key) key = 1;
        uint32_t i = Hash32(key) & (N - 1);
        for (uint32_t probe = 0; probe < N; probe++, i = (i + 1) & (N - 1)) {
            if (keys[i] == key) return counts[i];
            if (!keys[i]) return 0;
        }
        return 0;
    }
    // The maxN largest counts, descending
    int Top(uint32_t* outKeys, uint32_t* outCounts, int maxN) const {
        int n = 0;
        for (uint32_t i = 0; i < N; i++) {
            if (!keys[i]) continue;
            const uint32_t c = counts[i];
            if (n == maxN && c <= outCounts[n - 1]) continue;
            int pos = n < maxN ? n++ : maxN - 1;
            while (pos > 0 && outCounts[pos - 1] < c) {
                outKeys[pos] = outKeys[pos - 1];
                outCounts[pos] = outCounts[pos - 1];
                pos--;
            }
            outKeys[pos] = keys[i];
            outCounts[pos] = c;
        }
        return n;
    }
};

// Session aggregates, [0] = other frames, [1] = hitch frames (render thread; reset by Clear)
struct SampleAgg {
    uint32_t total[2];           // samples: [0] render thread, [1] simulation thread
    uint32_t cls[2][kClsCount];
    CountTable<4096> fn[2];      // hottest code per thread
    CountTable<4096> site;       // render thread: call sites on the stack
    CountTable<1024> wait;       // render thread: first TS3W return address of samples in system code
    CountTable<1024> deep;       // render thread: first return address outside system code (any module) of samples in system code
};
SampleAgg g_agg[2];
CountTable<4096> g_hotEip; // render thread, hitch frames: exact EIP

struct FrameSampleAgg {
    uint32_t n[2];
    uint32_t cls[2][kClsCount];
    CountTable<1024> fn[2];
    CountTable<1024> site;
    CountTable<256> wait;
};
FrameSampleAgg g_frameAgg; // scratch for the current hitch frame

void ClearSampleAggregates() {
    for (auto& a : g_agg) {
        std::memset(a.total, 0, sizeof a.total);
        std::memset(a.cls, 0, sizeof a.cls);
        a.fn[0].Clear();
        a.fn[1].Clear();
        a.site.Clear();
        a.wait.Clear();
        a.deep.Clear();
    }
    g_hotEip.Clear();
}

// Render thread, at the frame boundary once the frame is classified: moves the samples taken in [intervalStart, now)
// into the session aggregates and, for a hitch, into out.
void ConsumeSamples(uint64_t intervalStart, uint64_t now, bool hitch, HitchSamples* out) {
    uint32_t t = g_sTail.load(std::memory_order_relaxed);
    const uint32_t h = g_sHead.load(std::memory_order_acquire);
    if (t == h) return;
    FrameSampleAgg& fa = g_frameAgg;
    if (hitch) {
        std::memset(fa.n, 0, sizeof fa.n);
        std::memset(fa.cls, 0, sizeof fa.cls);
        fa.fn[0].Clear();
        fa.fn[1].Clear();
        fa.site.Clear();
        fa.wait.Clear();
    }
    SampleAgg& agg = g_agg[hitch ? 1 : 0];
    for (; t != h; t++) {
        const Sample& s = g_sampleRing[t % kSampleRing];
        if (s.t >= now) break;             // belongs to the next frame
        if (s.t < intervalStart) continue; // left over from before this interval (sampler restarted, profiler re-enabled)
        const int thr = s.thread ? 1 : 0;
        const int mi = ModuleOf(s.eip);
        const uint8_t cls = mi >= 0 ? g_modules[mi].cls : kClsOther;
        // TS3W and this ASI: a function key (guessed start); other modules: the module
        const uint32_t key = cls == kClsGame ? FnStartGuess(s.eip)
                             : cls == kClsOurAsi ? OwnFnStartGuess(s.eip)
                                                 : (mi >= 0 ? static_cast<uint32_t>(g_modules[mi].base) : kUnknownKey);
        agg.total[thr]++;
        agg.cls[thr][cls]++;
        agg.fn[thr].Add(key);
        if (hitch) {
            fa.n[thr]++;
            fa.cls[thr][cls]++;
            fa.fn[thr].Add(key);
            if (thr == 0) g_hotEip.Add(s.eip);
        }
        if (thr == 0) {
            for (int k = 0; k < s.nRet; k++) {
                bool dup = false;
                for (int j = 0; j < k && !dup; j++) dup = s.ret[j] == s.ret[k];
                if (dup) continue;
                agg.site.Add(s.ret[k]);
                if (hitch) fa.site.Add(s.ret[k]);
            }
            if (cls == kClsSystem && s.nRet) {
                agg.wait.Add(s.ret[0]);
                if (hitch) fa.wait.Add(s.ret[0]);
            }
            if (cls == kClsSystem && s.firstOut) agg.deep.Add(s.firstOut);
        }
    }
    g_sTail.store(t, std::memory_order_release);
    if (!hitch || !out) return;
    uint32_t keys[kTopN], counts[kTopN];
    for (int thr = 0; thr < 2; thr++) {
        out->n[thr] = static_cast<uint16_t>(std::min<uint32_t>(fa.n[thr], 65535));
        for (int c = 0; c < kClsCount; c++) out->cls[thr][c] = static_cast<uint16_t>(std::min<uint32_t>(fa.cls[thr][c], 65535));
        const int n = fa.fn[thr].Top(keys, counts, kTopN);
        for (int i = 0; i < n; i++) {
            out->fnKey[thr][i] = keys[i];
            out->fnCount[thr][i] = static_cast<uint16_t>(std::min<uint32_t>(counts[i], 65535));
            out->fnCls[thr][i] = KeyClass(keys[i]);
        }
    }
    const int ns = fa.site.Top(keys, counts, kTopN);
    for (int i = 0; i < ns; i++) {
        out->site[i] = keys[i];
        out->siteCount[i] = static_cast<uint16_t>(std::min<uint32_t>(counts[i], 65535));
    }
    const int nw = fa.wait.Top(keys, counts, kTopWait);
    for (int i = 0; i < nw; i++) {
        out->wait[i] = keys[i];
        out->waitCount[i] = static_cast<uint16_t>(std::min<uint32_t>(counts[i], 65535));
    }
}

void ClearFrameTables() {
    g_fSvc.Clear();
    g_fJob.Clear();
    g_fWait.Clear();
    g_fReadBytes = 0;
}

// Render thread, at the frame boundary once the frame is classified: the frame's services / jobs / waits go into the
// session tables and, for a hitch, the heaviest ones into out
void ConsumeFrameTables(bool hitch, HitchDetail* out) {
    const int b = hitch ? 1 : 0;
    g_aSvc[b].Merge(g_fSvc);
    g_aJob[b].Merge(g_fJob);
    g_aWait[b].Merge(g_fWait);
    g_aReadBytes[b] += g_fReadBytes;
    if (hitch && out) {
        const double k = g_msPerTick;
        int idx[8];
        int n = g_fSvc.Top(idx, kTopSvc);
        for (int i = 0; i < n; i++) {
            const int j = idx[i];
            out->svcKey[i] = g_fSvc.keys[j];
            out->svcVt[i] = g_fSvc.aux[j];
            out->svcMs[i] = static_cast<float>(static_cast<double>(g_fSvc.incl[j]) * k);
            out->svcSelfMs[i] = static_cast<float>(static_cast<double>(g_fSvc.self[j]) * k);
            out->svcCalls[i] = static_cast<uint16_t>(std::min<uint32_t>(g_fSvc.calls[j], 65535));
        }
        n = g_fJob.Top(idx, kTopJobs);
        for (int i = 0; i < n; i++) {
            const int j = idx[i];
            out->jobKey[i] = g_fJob.keys[j];
            out->jobMs[i] = static_cast<float>(static_cast<double>(g_fJob.incl[j]) * k);
            out->jobSelfMs[i] = static_cast<float>(static_cast<double>(g_fJob.self[j]) * k);
            out->jobCalls[i] = static_cast<uint16_t>(std::min<uint32_t>(g_fJob.calls[j], 65535));
        }
        n = g_fWait.Top(idx, kTopWaits);
        for (int i = 0; i < n; i++) {
            const int j = idx[i];
            out->waitKey[i] = g_fWait.keys[j];
            out->waitKind[i] = static_cast<uint8_t>(g_fWait.aux[j]);
            out->waitMs[i] = static_cast<float>(static_cast<double>(g_fWait.incl[j]) * k);
            out->waitCount[i] = static_cast<uint16_t>(std::min<uint32_t>(g_fWait.calls[j], 65535));
        }
        out->readBytes = static_cast<uint32_t>(std::min<uint64_t>(g_fReadBytes, 0xFFFFFFFFull));
    }
    ClearFrameTables();
}

// ---- frame boundary ----
int BinOf(float ms) {
    if (!(ms > 0.0f)) return 0;
    if (ms < 100.0f) return std::min(kFineBins - 1, static_cast<int>(ms / 0.05f));
    if (ms < 1000.0f) return kFineBins + std::min(kCoarseBins - 1, static_cast<int>(ms - 100.0f));
    return kBins - 1;
}

float BinValue(int b) {
    if (b < kFineBins) return (static_cast<float>(b) + 0.5f) * 0.05f;
    if (b < kFineBins + kCoarseBins) return 100.0f + static_cast<float>(b - kFineBins) + 0.5f;
    return 1000.0f;
}

float Percentile(double p) {
    if (!g_stats.frames) return 0.0f;
    const uint64_t target = std::max<uint64_t>(1, static_cast<uint64_t>(std::ceil(p * static_cast<double>(g_stats.frames))));
    uint64_t cum = 0;
    for (int b = 0; b < kBins; b++) {
        cum += g_stats.hist[b];
        if (cum >= target) return BinValue(b);
    }
    return BinValue(kBins - 1);
}

float MedianOfWindow() {
    if (!g_medianCount) return 0.0f;
    float tmp[kMedianWindow];
    std::memcpy(tmp, g_medianRing, sizeof(float) * g_medianCount);
    const int mid = g_medianCount / 2;
    std::nth_element(tmp, tmp + mid, tmp + g_medianCount);
    return tmp[mid];
}

// r == nullptr: baseline only (forget what accumulated while the profiler was off)
void SnapshotThreads(HitchRecord* r) {
    const int used = g_slotsUsed.load(std::memory_order_acquire);
    const DWORD renderTid = g_renderTid.load(std::memory_order_relaxed);
    const DWORD simTid = g_simTid.load(std::memory_order_relaxed);
    if (r) std::memset(g_frameBucketCalls, 0, sizeof g_frameBucketCalls);
    for (int i = 0; i < used; i++) {
        ThreadSlot& s = g_slots[i];
        const DWORD tid = s.tid.load(std::memory_order_relaxed);
        Seen& seen = g_seen[i];
        const int bucket = tid == renderTid ? 0 : (simTid != 0 && tid == simTid ? 1 : 2);
        for (int c = 0; c < kCatCount; c++) {
            const uint64_t e = s.excl[c].load(std::memory_order_relaxed);
            const uint64_t n = s.incl[c].load(std::memory_order_relaxed);
            const uint32_t k = s.calls[c].load(std::memory_order_relaxed);
            if (r) {
                const float em = static_cast<float>(static_cast<double>(e - seen.excl[c]) * g_msPerTick);
                const float im = static_cast<float>(static_cast<double>(n - seen.incl[c]) * g_msPerTick);
                if (bucket == 0) {
                    r->render[c] += em;
                    r->renderIncl[c] += im;
                } else if (bucket == 1) {
                    r->sim[c] += em;
                } else {
                    r->other[c] += em;
                }
                r->calls[c] += k - seen.calls[c];
                g_frameBucketCalls[bucket][c] += k - seen.calls[c];
                if (IsCounterCat(c)) {
                    r->counters.incl[bucket][c - kFirstCounterCat] += im;
                    r->counters.calls[bucket][c - kFirstCounterCat] += k - seen.calls[c];
                }
            }
            seen.excl[c] = e;
            seen.incl[c] = n;
            seen.calls[c] = k;
        }
        for (int x = 0; x < kExtraCount; x++) {
            const uint64_t v = s.extra[x].load(std::memory_order_relaxed);
            if (r) r->counters.extra[bucket][x] += v - seen.extra[x];
            seen.extra[x] = v;
        }
    }
}

int8_t SampleCamera() {
    bool seen = false, moved = false;
    const int pointMoved = g_camPointMoved.exchange(0, std::memory_order_relaxed);
    if (g_camPointSeen.exchange(0, std::memory_order_relaxed)) {
        seen = true;
        moved = pointMoved != 0;
    }
    // (The combined build also used PostScene's camera view-projection as a fallback here; the standalone keeps
    // v0.1.0's PostScene, which does not track the camera, so frames without the lot LOD call count as "unknown".)
    return moved ? 1 : (seen ? 0 : -1);
}

bool IsForeground() {
    const HWND w = GetForegroundWindow();
    DWORD pid = 0;
    if (w) GetWindowThreadProcessId(w, &pid);
    return pid == GetCurrentProcessId();
}

void QueuePush(const HitchRecord& r) {
    const uint32_t h = g_qHead.load(std::memory_order_relaxed);
    if (h - g_qTail.load(std::memory_order_acquire) >= kQueueSize) {
        g_qDropped.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    g_queue[h % kQueueSize] = r;
    g_qHead.store(h + 1, std::memory_order_release);
}

void RollRegistryWindow(uint64_t now) {
    g_regFrames++;
    if (static_cast<double>(now - g_regWindowStart) * g_msPerTick < 1000.0) return;
    g_regDisplay.clear();
    for (auto& n : g_regNames) {
        if (n.calls && g_regFrames) {
            g_regDisplay.push_back({n.name, static_cast<float>(static_cast<double>(n.ticks) * g_msPerTick / g_regFrames), static_cast<float>(n.calls) / g_regFrames});
        }
        n.ticks = 0;
        n.calls = 0;
    }
    std::sort(g_regDisplay.begin(), g_regDisplay.end(), [](const RegDisplay& a, const RegDisplay& b) { return a.msPerFrame > b.msPerFrame; });
    g_regFrames = 0;
    g_regWindowStart = now;
}

// Hitch frames: the dominant cause (as dom.pl: largest render self category except the service / job loops' own
// "Services (self)" / "Jobs (self)", each service and job of the frame by self time, Unattributed) and the counter with the
// largest render-thread self time
void ComputeDominant(HitchRecord& r) {
    float best = 0.0f;
    auto consider = [&](float ms, uint8_t kind, uint32_t key, uint32_t aux) {
        if (ms > best) {
            best = ms;
            r.domKind = kind;
            r.domKey = key;
            r.domAux = aux;
        }
    };
    for (int c = 0; c < kCatCount; c++)
        if (c != kService && c != kJob) consider(r.render[c], kDomCategory, static_cast<uint32_t>(c), 0);
    for (int i = 0; i < kTopSvc && r.detail.svcCalls[i]; i++) consider(r.detail.svcSelfMs[i], kDomService, r.detail.svcKey[i], r.detail.svcVt[i]);
    for (int i = 0; i < kTopJobs && r.detail.jobCalls[i]; i++) consider(r.detail.jobSelfMs[i], kDomJob, r.detail.jobKey[i], 0);
    consider(r.unattributedMs, kDomUnattributed, 0, 0);
    r.domMs = best;
    float top = 0.05f;
    r.topCounter = -1;
    for (int k = 0; k < kNC; k++)
        if (r.render[kFirstCounterCat + k] >= top) {
            top = r.render[kFirstCounterCat + k];
            r.topCounter = static_cast<int8_t>(k);
        }
}

void UpdateStats(const HitchRecord& r, bool hitch) {
    Stats& s = g_stats;
    s.frames++;
    s.maxFrameMs = std::max(s.maxFrameMs, static_cast<double>(r.frameMs));
    s.hist[BinOf(r.frameMs)]++;
    for (int c = 0; c < kCatCount; c++) {
        s.render[c] += r.render[c];
        s.sim[c] += r.sim[c];
        s.other[c] += r.other[c];
        for (int b = 0; b < 3; b++) s.bucketCalls[b][c] += g_frameBucketCalls[b][c];
    }
    s.unattributed += r.unattributedMs;
    s.lotsPromoted += r.lotsPromoted;
    s.lotsDemoted += r.lotsDemoted;
    for (int b = 0; b < 3; b++) {
        for (int k = 0; k < kNC; k++) {
            s.cIncl[b][k] += r.counters.incl[b][k];
            s.cCalls[b][k] += r.counters.calls[b][k];
        }
        for (int x = 0; x < kExtraCount; x++) s.cExtra[b][x] += r.counters.extra[b][x];
    }
    for (int b = 0; b < 2; b++)
        for (int k = 0; k < kNC; k++) s.cMax[b][k] = std::max(s.cMax[b][k], static_cast<double>(r.counters.maxMs[b][k]));
    g_cLive[g_livePos] = r.counters; // g_livePos advances below, with g_live
    if (r.camera == 1) s.framesMoving++;
    if (r.camera == 0) s.framesStill++;
    if (hitch) {
        s.hitches++;
        if (r.camera == 1) s.hitchesMoving++;
        if (r.camera == 0) s.hitchesStill++;
    }
    g_graph[g_graphPos] = r.frameMs;
    g_graphPos = (g_graphPos + 1) % kGraphFrames;
    g_graphCount = std::min(g_graphCount + 1, kGraphFrames);
    LiveSample& l = g_live[g_livePos];
    l = {r.frameMs, r.cpuMs, r.presentMs, r.limiterMs, r.modMs, static_cast<float>(r.gameDraws), static_cast<float>(r.endFrameDraws), static_cast<float>(r.setTexture),
        static_cast<float>(r.setShader), static_cast<float>(r.shaderConst), static_cast<float>(r.setRT)};
    g_livePos = (g_livePos + 1) % kLiveFrames;
    g_liveCount = std::min(g_liveCount + 1, kLiveFrames);
}

void FrameBoundary(uint64_t now) {
    if (g_needBaseline.exchange(false)) {
        SnapshotThreads(nullptr);
        g_d3d = D3DCounts{};
        g_stateBase = D3D9Hooks::ReadStateCallCounts();
        g_lotsPromoted.store(0);
        g_lotsDemoted.store(0);
        g_camPointMoved.store(0);
        g_camPointSeen.store(0);
        g_camVpValid = false;
        g_lastBoundary = now;
        g_enableTicks = now;
        g_regWindowStart = now;
        g_regFrames = 0;
        g_medianCount = g_medianPos = 0; // warm-up again: the first frames include attaching the hooks
        ClearFrameTables();
        for (auto& row : g_cMax)
            for (auto& m : row) m.store(0, std::memory_order_relaxed);
        return;
    }
    if ((g_frameIndex & 127) == 0) RefineClock();

    HitchRecord& r = g_cur;
    r = HitchRecord{};
    const uint64_t intervalStart = g_lastBoundary;
    r.frameMs = static_cast<float>(static_cast<double>(now - g_lastBoundary) * g_msPerTick);
    g_lastBoundary = now;
    r.frame = ++g_frameIndex;
    r.tSec = static_cast<double>(now - g_enableTicks) * g_msPerTick / 1000.0;
    SnapshotThreads(&r);
    for (int b = 0; b < 2; b++)
        for (int k = 0; k < kNC; k++) r.counters.maxMs[b][k] = static_cast<float>(static_cast<double>(g_cMax[b][k].exchange(0, std::memory_order_relaxed)) * g_msPerTick);

    r.gameDraws = g_d3d.gameDraws;
    r.endFrameDraws = g_d3d.endFrameDraws;
    r.dip = g_d3d.dip;
    r.dp = g_d3d.dp;
    r.prims = g_d3d.prims;
    {
        // counted by the registry's detours themselves (plain counters, no registered callback)
        const D3D9Hooks::StateCallCounts c = D3D9Hooks::ReadStateCallCounts();
        r.setTexture = c.setTexture - g_stateBase.setTexture;
        r.setShader = (c.setVertexShader - g_stateBase.setVertexShader) + (c.setPixelShader - g_stateBase.setPixelShader);
        r.shaderConst = (c.setVertexConstants - g_stateBase.setVertexConstants) + (c.setPixelConstants - g_stateBase.setPixelConstants);
        r.setRT = c.setRenderTarget - g_stateBase.setRenderTarget;
        g_stateBase = c;
    }
    r.createTex = g_d3d.createTex;
    r.createVS = g_d3d.createVS;
    r.createPS = g_d3d.createPS;
    r.createRT = g_d3d.createRT;
    r.stateCounted = g_stateHooksActive;
    g_d3d = D3DCounts{};
    r.lotsPromoted = g_lotsPromoted.exchange(0, std::memory_order_relaxed);
    r.lotsDemoted = g_lotsDemoted.exchange(0, std::memory_order_relaxed);
    r.camera = SampleCamera();

    r.presentMs = r.render[kPresentDriver];
    r.limiterMs = r.render[kFrameLimiter];
    r.modMs = r.render[kModD3DHooks] + r.render[kPresentHooks] + r.render[kLampRefresh];
    r.cpuMs = std::max(0.0f, r.frameMs - r.presentMs - r.limiterMs);
    float sum = 0.0f;
    for (int c = 0; c < kCatCount; c++) sum += r.render[c];
    r.unattributedMs = std::max(0.0f, r.frameMs - sum);

    r.medianMs = MedianOfWindow();
    r.thresholdMs = std::max(g_mult.load(std::memory_order_relaxed) * r.medianMs, g_floorMs.load(std::memory_order_relaxed));
    g_lastMedian = r.medianMs;
    g_lastThreshold = r.thresholdMs;
    const bool hitch = g_medianCount >= kWarmupFrames && r.frameMs > r.thresholdMs;
    g_medianRing[g_medianPos] = r.frameMs;
    g_medianPos = (g_medianPos + 1) % kMedianWindow;
    g_medianCount = std::min(g_medianCount + 1, kMedianWindow);
    {
        // page faults of the whole process during this frame (soft and hard: DXVK's mapped texture copies, fresh allocations)
        PROCESS_MEMORY_COUNTERS pmc;
        if (GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof pmc)) {
            if (g_lastPageFaults) {
                r.pageFaults = pmc.PageFaultCount - g_lastPageFaults;
                g_pfSum[hitch ? 1 : 0] += r.pageFaults;
                g_pfFrames[hitch ? 1 : 0]++;
            }
            g_lastPageFaults = pmc.PageFaultCount;
        }
    }
    ConsumeSamples(intervalStart, now, hitch, &r.samples);
    ConsumeFrameTables(hitch, &r.detail);
    if (hitch) ComputeDominant(r);

    UpdateStats(r, hitch);
    if (hitch) {
        r.foreground = IsForeground();
        g_hitches[g_hitchPos] = r;
        g_hitchPos = (g_hitchPos + 1) % kHitchRing;
        g_hitchCount = std::min(g_hitchCount + 1, kHitchRing);
        if (g_writeFile.load(std::memory_order_relaxed) && g_writerRunning.load(std::memory_order_relaxed)) QueuePush(r);
    }
    RollRegistryWindow(now);
}

// ---- registry hooks ----
void AttachAllLocked();
void AttachWaitingLocked();

void OnPresentStart(D3D9Hooks::DeviceContext& ctx) {
    const uint64_t now = Now();
    g_renderTid.store(GetCurrentThreadId(), std::memory_order_relaxed);
    if (ThreadSlot* s = GetSlot()) {
        CleanStale(s, &ctx);
        Split(s, now);
        Push(s, kPresentHooks, &ctx, kDispatch); // the bookkeeping below counts as mod Present-hook time
    }
    FrameBoundary(now);
    if (g_attachPending.load(std::memory_order_relaxed)) {
        // First frame after enabling: the timed functions are attached here, on the render thread, so the call-site
        // writes never race the render thread executing them, and after the startup patches have installed.
        // try_lock: SetEnabled holds g_ctrlMutex while it waits for the registry mutex this hook runs under.
        std::unique_lock<std::mutex> lk(g_ctrlMutex, std::try_to_lock);
        if (lk.owns_lock() && g_attachPending.load()) {
            AttachAllLocked();
            g_attachPending.store(false);
        }
    } else if (g_waitAddr.load(std::memory_order_relaxed) && GameAddr::Scanned()) {
        // profiler on from the start: the counters wait for the game-address scan (init thread, first Present + 1 s) on
        // every build, so the scan never sees their hooks (and on non-Steam builds their addresses come from it)
        std::unique_lock<std::mutex> lk(g_ctrlMutex, std::try_to_lock);
        if (lk.owns_lock() && g_enabled.load()) AttachWaitingLocked();
    }
}

// Draw counts only: the dispatch's time is booked by the registry itself (BeginModTime / EndModTime around its outermost
// dispatch, framework/d3d9_hooks.cpp), which also covers dispatches a module cuts short with Skip.
void OnDrawStart(bool indexed, UINT prims) {
    ThreadSlot* s = GetSlot();
    if (indexed) g_d3d.dip++;
    else g_d3d.dp++;
    if (s && s->open[kEndScene] > 0) {
        g_d3d.endFrameDraws++;
    } else {
        g_d3d.gameDraws++;
        g_d3d.prims += prims;
    }
}

void EndDispatch(const void* sp) {
    ThreadSlot* s = t_slot;
    if (!s) return;
    for (int i = s->depth - 1; i >= 0; i--) {
        if (s->stack[i].sp == sp && (s->stack[i].flags & kDispatch)) {
            Pop(s, i, sp, Now(), 0, 0);
            return;
        }
    }
}

// Caller holds g_ctrlMutex
void RegisterD3DHooks() {
    using namespace D3D9Hooks;
    const Priority start = static_cast<Priority>(kPrioStart);
    const Priority end = static_cast<Priority>(kPrioEnd);
    RegisterPresent(kHookName, [](DeviceContext& ctx, const RECT*, const RECT*, HWND, const RGNDATA*) {
        OnPresentStart(ctx);
        return HookAction::Continue;
    }, start);
    RegisterPresent(kHookName, [](DeviceContext& ctx, const RECT*, const RECT*, HWND, const RGNDATA*) {
        EndDispatch(&ctx);
        return HookAction::Continue;
    }, end);
    ObserveReissuedDraws(kHookName); // counts every draw, the re-issued ones too (as before)
    RegisterDrawIndexedPrimitive(kHookName, [](DeviceContext&, D3DPRIMITIVETYPE, INT, UINT, UINT, UINT, UINT primCount) {
        OnDrawStart(true, primCount);
        return HookAction::Continue;
    }, start);
    RegisterDrawPrimitive(kHookName, [](DeviceContext&, D3DPRIMITIVETYPE, UINT, UINT primCount) {
        OnDrawStart(false, primCount);
        return HookAction::Continue;
    }, start);
    RegisterCreateTexture(kHookName, [](DeviceContext&, UINT w, UINT h, UINT levels, DWORD, D3DFORMAT format, D3DPOOL, IDirect3DTexture9**, HANDLE*) {
        g_d3d.createTex++;
        t_texW = w; // for the texture-load table (Hook_TexCreate, same thread)
        t_texH = h;
        t_texLevels = levels;
        t_texFormat = format;
        return HookAction::Continue;
    }, start);
    RegisterCreateRenderTarget(kHookName, [](DeviceContext&, UINT, UINT, D3DFORMAT, D3DMULTISAMPLE_TYPE, DWORD, BOOL, IDirect3DSurface9**, HANDLE*) {
        g_d3d.createRT++;
        return HookAction::Continue;
    }, start);
    RegisterCreateVertexShader(kHookName, [](DeviceContext&, const DWORD*, IDirect3DVertexShader9**) {
        g_d3d.createVS++;
        return HookAction::Continue;
    }, start);
    RegisterCreatePixelShader(kHookName, [](DeviceContext&, const DWORD*, IDirect3DPixelShader9**) {
        g_d3d.createPS++;
        return HookAction::Continue;
    }, start);
    // SetTexture / Set*Shader / Set*ShaderConstantF / SetRenderTarget: counted by the registry's detours with plain
    // per-method counters (D3D9Hooks::ReadStateCallCounts, read at each frame boundary). Until 2026-09-29 six callbacks were
    // registered here, which made every state call of the game run a full chain dispatch (0.25-0.35 ms per frame in the
    // development build, research\perf2\apexcost\report.md); the option now only decides whether the counts are shown.
    g_stateHooksActive = g_countState.load();
}

// ---- text output ----
std::string NowString() {
    const std::time_t t = std::time(nullptr);
    std::tm tm{};
    localtime_s(&tm, &t);
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y-%m-%d %H:%M:%S", &tm);
    return buf;
}

const char* CameraText(int8_t c) {
    return c > 0 ? "moving" : (c == 0 ? "still" : "n/a");
}

std::string CatList(const float* v, float minMs) {
    int idx[kCatCount];
    int n = 0;
    for (int c = 0; c < kCatCount; c++)
        if (v[c] >= minMs) idx[n++] = c;
    std::sort(idx, idx + n, [&](int a, int b) { return v[a] > v[b]; });
    std::string s;
    for (int k = 0; k < n; k++) s += std::format("{}{} {:.2f}", k ? ", " : "", kCats[idx[k]].name, v[idx[k]]);
    return s;
}

double Pct(uint32_t part, uint32_t whole) {
    return whole ? 100.0 * static_cast<double>(part) / static_cast<double>(whole) : 0.0;
}

// Sampling lines of one hitch (writer thread and report: reads only the append-only module table)
std::string FormatHitchSamples(const HitchSamples& m) {
    std::string s;
    static const char* const kThreadNames[2] = {"render", "simulation"};
    for (int thr = 0; thr < 2; thr++) {
        const uint32_t n = m.n[thr];
        if (!n) continue;
        s += std::format("   samples {} thread ({}):", kThreadNames[thr], n);
        for (int c = 0; c < kClsCount; c++)
            if (m.cls[thr][c]) s += std::format(" {} {:.0f}%", kClassNames[c], Pct(m.cls[thr][c], n));
        s += " | hot:";
        for (int i = 0; i < kTopN && m.fnCount[thr][i]; i++) s += std::format("{} {} {:.0f}%", i ? "," : "", KeyText(m.fnKey[thr][i]), Pct(m.fnCount[thr][i], n));
        if (thr == 0 && m.siteCount[0]) {
            s += " | on the stack:";
            for (int i = 0; i < kTopN && m.siteCount[i]; i++) s += std::format("{} {:08X} {:.0f}%", i ? "," : "", m.site[i], Pct(m.siteCount[i], n));
        }
        if (thr == 0 && m.waitCount[0]) {
            s += " | system code called from:";
            for (int i = 0; i < kTopWait && m.waitCount[i]; i++) s += std::format("{} {:08X} {:.0f}%", i ? "," : "", m.wait[i], Pct(m.waitCount[i], n));
        }
        s += "\n";
    }
    return s;
}

std::string WaitName(uint32_t key, uint32_t kind) {
    if (kind == kWaitJob) return "job wait for " + JobName(key);
    return std::format("{} at {:08X}", WaitKindName(kind), key);
}

// Services, jobs, waits and file reads of one hitch (render thread)
std::string FormatHitchDetail(const HitchDetail& d) {
    std::string s;
    if (d.svcCalls[0]) {
        s += "   services (render thread, ms incl / self):";
        for (int i = 0; i < kTopSvc && d.svcCalls[i]; i++)
            s += std::format("{} {} {:.2f} / {:.2f}", i ? "," : "", ServiceName(d.svcKey[i], d.svcVt[i]), d.svcMs[i], d.svcSelfMs[i]);
        s += "\n";
    }
    if (d.jobCalls[0]) {
        s += "   jobs run on the render thread (ms incl / self, count):";
        for (int i = 0; i < kTopJobs && d.jobCalls[i]; i++) s += std::format("{} {} {:.2f} / {:.2f} x{}", i ? "," : "", JobName(d.jobKey[i]), d.jobMs[i], d.jobSelfMs[i], d.jobCalls[i]);
        s += "\n";
    }
    if (d.waitCount[0]) {
        s += "   render-thread waits (ms, count):";
        for (int i = 0; i < kTopWaits && d.waitCount[i]; i++) s += std::format("{} {} {:.2f} x{}", i ? "," : "", WaitName(d.waitKey[i], d.waitKind[i]), d.waitMs[i], d.waitCount[i]);
        s += "\n";
    }
    if (d.readBytes) s += std::format("   file reads on the render thread: {:.1f} KB\n", d.readBytes / 1024.0);
    return s;
}

// Session tables of services / jobs / waits (render thread): ms per hitch vs ms per other frame
std::string KeyedReport() {
    const double hitches = static_cast<double>(g_stats.hitches);
    const double others = static_cast<double>(g_stats.frames - g_stats.hitches);
    const double k = g_msPerTick;
    auto perHitch = [&](uint64_t t) { return hitches > 0 ? static_cast<double>(t) * k / hitches : 0.0; };
    auto perOther = [&](uint64_t t) { return others > 0 ? static_cast<double>(t) * k / others : 0.0; };
    std::string s;
    constexpr int kRows = 40;
    int idx[kRows];
    int n = g_aSvc[1].Top(idx, kRows);
    if (n) {
        s += "Services on the render thread: ms per hitch (incl / self) | ms per other frame (incl / self) | calls in hitches\n";
        for (int i = 0; i < n; i++) {
            const auto& t = g_aSvc[1];
            const int j = idx[i];
            const int o = g_aSvc[0].Find(t.keys[j]);
            s += std::format("   {:<48} {:>8.2f} / {:>7.2f} | {:>7.2f} / {:>7.2f} | {:>8}\n", ServiceName(t.keys[j], t.aux[j]), perHitch(t.incl[j]), perHitch(t.self[j]),
                o >= 0 ? perOther(g_aSvc[0].incl[o]) : 0.0, o >= 0 ? perOther(g_aSvc[0].self[o]) : 0.0, t.calls[j]);
        }
    }
    n = g_aJob[1].Top(idx, kRows);
    if (n) {
        s += "Jobs run on the render thread: ms per hitch (incl / self) | ms per other frame (incl) | runs in hitches\n";
        for (int i = 0; i < n; i++) {
            const auto& t = g_aJob[1];
            const int j = idx[i];
            const int o = g_aJob[0].Find(t.keys[j]);
            s += std::format("   {:<48} {:>8.2f} / {:>7.2f} | {:>7.2f} | {:>8}\n", JobName(t.keys[j]), perHitch(t.incl[j]), perHitch(t.self[j]), o >= 0 ? perOther(g_aJob[0].incl[o]) : 0.0,
                t.calls[j]);
        }
    }
    n = g_aWait[1].Top(idx, kRows);
    if (n) {
        s += "Render-thread waits (job waits by job, blocked mutex / semaphore calls by return address): ms per hitch | ms per other frame | count in hitches\n";
        for (int i = 0; i < n; i++) {
            const auto& t = g_aWait[1];
            const int j = idx[i];
            const int o = g_aWait[0].Find(t.keys[j]);
            s += std::format("   {:<48} {:>8.2f} | {:>7.2f} | {:>8}\n", WaitName(t.keys[j], t.aux[j]), perHitch(t.incl[j]), o >= 0 ? perOther(g_aWait[0].incl[o]) : 0.0, t.calls[j]);
        }
    }
    if (g_aReadBytes[0] || g_aReadBytes[1])
        s += std::format("File reads on the render thread: {:.1f} KB per hitch, {:.2f} KB per other frame\n", hitches > 0 ? g_aReadBytes[1] / 1024.0 / hitches : 0.0,
            others > 0 ? g_aReadBytes[0] / 1024.0 / others : 0.0);
    bool header = false;
    for (int i = 0; i < kOtherSvcSlots; i++) {
        const uint32_t key = g_otherSvc[i].key.load();
        if (!key) continue;
        if (!header) {
            s += "Services on other threads (simulation loop), since Clear: total ms incl / self | calls\n";
            header = true;
        }
        const uint64_t in = g_otherSvc[i].incl.load() - g_otherSvcBaseIncl[i], se = g_otherSvc[i].self.load() - g_otherSvcBaseSelf[i];
        s += std::format("   {:<48} {:>10.1f} / {:>10.1f} | {:>8}\n", ServiceName(key, g_otherSvc[i].vtable.load()), static_cast<double>(in) * k, static_cast<double>(se) * k,
            g_otherSvc[i].calls.load() - g_otherSvcBaseCalls[i]);
    }
    return s;
}

std::string SamplerStatusText() {
    const uint32_t taken = g_sTaken.load();
    const double costUs = taken ? static_cast<double>(g_sPausedTicks.load()) * g_msPerTick * 1000.0 / taken : 0.0;
    return std::format("Sampler {} ({} Hz target): {} samples, {:.1f} us paused per sample, {} dropped (ring full)", g_samplerRunning.load() ? "on" : "off",
        g_sampleHz.load(), taken, costUs, g_sDropped.load());
}

// Render thread: session tables (hitch frames vs other frames), sorted by samples in hitch frames
std::string SamplingReport() {
    const SampleAgg& hi = g_agg[1];
    const SampleAgg& lo = g_agg[0];
    if (!hi.total[0] && !hi.total[1] && !lo.total[0] && !lo.total[1]) return "";
    std::string s = "Sampling (return addresses from a heuristic stack walk; 'fn~' = TS3W function start guessed from the int3 padding,\n"
                    "resolve it with the decompile; % = share of that thread's samples in hitch frames / in other frames):\n";
    s += "   " + SamplerStatusText() + "\n";
    s += "   " + OwnModuleText() + "\n";
    static const char* const kThreadNames[2] = {"Render", "Simulation"};
    constexpr int kRows = 40;
    uint32_t keys[kRows], counts[kRows];
    for (int thr = 0; thr < 2; thr++) {
        if (!hi.total[thr] && !lo.total[thr]) continue;
        s += std::format("{} thread: {} samples in hitch frames, {} in other frames\n", kThreadNames[thr], hi.total[thr], lo.total[thr]);
        s += "   code class          hitch %   other %\n";
        for (int c = 0; c < kClsCount; c++)
            s += std::format("   {:<18} {:>8.1f} {:>9.1f}\n", kClassNames[c], Pct(hi.cls[thr][c], hi.total[thr]), Pct(lo.cls[thr][c], lo.total[thr]));
        s += "   hottest code (hitch frames)      samples  hitch %  other %  class\n";
        int n = hi.fn[thr].Top(keys, counts, kRows);
        for (int i = 0; i < n; i++)
            s += std::format("   {:<32} {:>8} {:>8.1f} {:>8.1f}  {}\n", KeyText(keys[i]), counts[i], Pct(counts[i], hi.total[thr]), Pct(lo.fn[thr].Get(keys[i]), lo.total[thr]),
                kClassNames[KeyClass(keys[i])]);
        if (thr != 0) continue;
        s += "   TS3W call sites on the stack (return address)  samples  hitch %  other %\n";
        n = hi.site.Top(keys, counts, kRows);
        for (int i = 0; i < n; i++)
            s += std::format("   {:08X}                                  {:>8} {:>8.1f} {:>8.1f}\n", keys[i], counts[i], Pct(counts[i], hi.total[0]), Pct(lo.site.Get(keys[i]), lo.total[0]));
        s += "   system code (ntdll/kernel/user32...) called from  samples  hitch %  other %\n";
        n = hi.wait.Top(keys, counts, 20);
        for (int i = 0; i < n; i++)
            s += std::format("   {:08X}                                  {:>8} {:>8.1f} {:>8.1f}\n", keys[i], counts[i], Pct(counts[i], hi.total[0]), Pct(lo.wait.Get(keys[i]), lo.total[0]));
        s += "   system code: first caller outside system code (4 KB of stack; TS3W, DXVK, driver, ASI...)  samples  hitch %  other %\n";
        n = hi.deep.Top(keys, counts, 20);
        for (int i = 0; i < n; i++)
            s += std::format("   {:<40} {:>8} {:>8.1f} {:>8.1f}\n", CallerText(keys[i]), counts[i], Pct(counts[i], hi.total[0]), Pct(lo.deep.Get(keys[i]), lo.total[0]));
        s += "   exact EIPs in hitch frames                  samples  hitch %  module\n";
        n = g_hotEip.Top(keys, counts, kRows);
        for (int i = 0; i < n; i++) {
            const int mi = FindModule(keys[i]);
            const std::string where = mi < 0 ? std::string("?")
                                      : g_modules[mi].cls == kClsOurAsi ? std::format("{}+{:X}", g_modules[mi].name, keys[i] - static_cast<uint32_t>(g_modules[mi].base))
                                      : g_modules[mi].cls == kClsSystem ? std::string(g_modules[mi].name) + " " + ExportNear(mi, keys[i])
                                                                        : std::string(g_modules[mi].name);
            s += std::format("   {:08X}                                  {:>8} {:>8.1f}  {}\n", keys[i], counts[i], Pct(counts[i], hi.total[0]), where);
        }
    }
    return s;
}

// ---- counters and dominant cause as text (hitch blocks, report, UI). Formats read by research\perf2\tools\*.pl:
//   "   counters (calls x ms incl. on the render / simulation / other threads): <Name> <n> x <ms>[ (max <ms>)] / <n> x <ms> /
//    <n> x <ms>[ (max <ms>)][, extras]; <Name> ..."   (max: longest single call, render thread / simulation + other threads)
//   "   dominant: <key> <ms> ms (<p>% of the frame) | top counter: <Name> <ms> ms self (<ms> ms incl.)" or "... | top counter: none"
//   with <key> as dom.pl names it: a category name, "svc:<service>", "job:<job name>" or "Unattributed".
std::string DomName(const HitchRecord& h) {
    switch (h.domKind) {
    case kDomCategory:
        return kCats[h.domKey].name;
    case kDomService:
        return "svc:" + ServiceName(h.domKey, h.domAux);
    case kDomJob:
        return "job:" + JobName(h.domKey);
    case kDomUnattributed:
        return "Unattributed";
    default:
        return "none";
    }
}

std::string DominantText(const HitchRecord& h) {
    std::string s = std::format("dominant: {} {:.2f} ms ({:.0f}% of the frame) | top counter: ", DomName(h), h.domMs, h.frameMs > 0.0f ? 100.0 * h.domMs / h.frameMs : 0.0);
    if (h.topCounter < 0) return s + "none";
    const int c = kFirstCounterCat + h.topCounter;
    return s + std::format("{} {:.2f} ms self ({:.2f} ms incl.)", kCats[c].name, h.render[c], h.renderIncl[c]);
}

// The extra counts of counter k over all threads, as text (", packages per lookup 245.3, misses 40", ...)
std::string CounterExtraText(const uint64_t extra[3][kExtraCount], uint64_t calls, int k) {
    auto sum = [&](int x) { return extra[0][x] + extra[1][x] + extra[2][x]; };
    switch (kFirstCounterCat + k) {
    case kResLookup: {
        std::string t = std::format(", packages per lookup {:.1f}, misses {}", calls ? static_cast<double>(sum(kXPackages)) / static_cast<double>(calls) : 0.0, sum(kXMisses));
        if (sum(kXCacheHits)) t += std::format(", from cache {}", sum(kXCacheHits)); // resource lookup cache on (",": agg.pl splits counters on "; ")
        if (sum(kXCacheAbsent)) t += std::format(", absent from cache {}", sum(kXCacheAbsent)); // "Remember missing files" on
        return t;
    }
    case kScenePending: {
        std::string t = std::format(", nodes {}", sum(kXNodes));
        if (sum(kXDeferred)) t += std::format(", deferred {}", sum(kXDeferred)); // scene node budget on (",": agg.pl splits counters on "; ")
        return t;
    }
    case kRefPackCompress:
        return std::format(", in {:.1f} KB, out {:.1f} KB", static_cast<double>(sum(kXBytesIn)) / 1024.0, static_cast<double>(sum(kXBytesOut)) / 1024.0);
    case kDxtEncode:
    case kTexCreate:
        return std::format(", pixels {:.2f} M", static_cast<double>(sum(kXPixels)) / 1e6);
    case kKeyList: {
        std::string t = std::format(", keys {}", sum(kXKeys));
        if (sum(kXListCached)) t += std::format(", packages from cache {}", sum(kXListCached)); // file list cache on
        return t;
    }
    case kObjectLookup:
        return sum(kXIndexHits) ? std::format(", from index {}", sum(kXIndexHits)) : std::string(); // object lookup index on
    default:
        return "";
    }
}

std::string FormatCounters(const CounterFrame& f, const float* renderIncl) {
    std::string s;
    for (int k = 0; k < kNC; k++) {
        if (!CounterActive(f, k)) continue;
        s += std::format("{}{} {} x {:.2f}", s.empty() ? "" : "; ", kCats[kFirstCounterCat + k].name, f.calls[0][k], f.incl[0][k]);
        if (f.calls[0][k]) s += std::format(" (max {:.2f})", f.maxMs[0][k]);
        s += std::format(" / {} x {:.2f} / {} x {:.2f}", f.calls[1][k], f.incl[1][k], f.calls[2][k], f.incl[2][k]);
        if (f.calls[1][k] || f.calls[2][k]) s += std::format(" (max {:.2f})", f.maxMs[1][k]);
        s += CounterExtraText(f.extra, static_cast<uint64_t>(f.calls[0][k]) + f.calls[1][k] + f.calls[2][k], k);
        if (kFirstCounterCat + k == kLotRoomSolve && renderIncl) s += std::format(", lot lighting update {:.2f} ms incl.", renderIncl[kLotLightingUpdate]);
    }
    return s;
}

std::string FormatHitch(const HitchRecord& h) {
    std::string s = std::format("#{} t={:.1f}s  frame {:.2f} ms (median {:.2f}, threshold {:.2f})  cpu {:.2f}  present {:.2f}  limiter {:.2f}  mod D3D {:.2f}  camera {}{}\n", h.frame, h.tSec,
        h.frameMs, h.medianMs, h.thresholdMs, h.cpuMs, h.presentMs, h.limiterMs, h.modMs, CameraText(h.camera), h.foreground ? "" : "  (window in background)");
    const std::string render = CatList(h.render, 0.05f);
    s += std::format("   render thread (self ms): {}{}Unattributed {:.2f}\n", render, render.empty() ? "" : ", ", h.unattributedMs);
    float nested[kCatCount];
    for (int c = 0; c < kCatCount; c++) nested[c] = h.renderIncl[c] > h.render[c] + 0.05f ? h.renderIncl[c] : 0.0f;
    if (const std::string incl = CatList(nested, 0.05f); !incl.empty()) s += "   render thread (incl. nested ms): " + incl + "\n";
    if (const std::string sim = CatList(h.sim, 0.05f); !sim.empty()) s += "   simulation thread (self ms): " + sim + "\n";
    if (const std::string other = CatList(h.other, 0.05f); !other.empty()) s += "   other threads (self ms): " + other + "\n";
    std::string calls;
    for (int c = kFirstGameCat; c < kCatCount; c++)
        if (h.calls[c]) calls += std::format("{}{} x{}", calls.empty() ? "" : ", ", kCats[c].name, h.calls[c]);
    if (!calls.empty()) s += "   calls: " + calls + "\n";
    s += std::format("   d3d: draws {} (+{} end-of-frame), prims {}, created: textures {}, render targets {}, vertex shaders {}, pixel shaders {}", h.gameDraws, h.endFrameDraws, h.prims,
        h.createTex, h.createRT, h.createVS, h.createPS);
    if (h.stateCounted) s += std::format("; SetTexture {}, Set*Shader {}, shader constants {}, SetRenderTarget {}", h.setTexture, h.setShader, h.shaderConst, h.setRT);
    s += std::format("\n   lots promoted {}, demoted {}, page faults {}\n", h.lotsPromoted, h.lotsDemoted, h.pageFaults);
    if (const std::string c = FormatCounters(h.counters, h.renderIncl); !c.empty()) s += "   counters (calls x ms incl. on the render / simulation / other threads): " + c + "\n";
    if (h.domKind != kDomNone) s += "   " + DominantText(h) + "\n";
    s += FormatHitchDetail(h.detail);
    s += FormatHitchSamples(h.samples);
    return s;
}

std::string ClockText() {
    if (g_msPerTick <= 0.0) return "not started";
    return g_useTsc ? std::format("RDTSC (invariant, {:.3f} GHz)", 1.0 / (g_msPerTick * 1e6)) : std::string("QueryPerformanceCounter");
}

// Caller holds g_ctrlMutex
std::string HookStatusText() {
    std::string s;
    for (int i = 0; i < kTargetCount; i++)
        s += std::format("   {:<44} {:#010x}  expected thread: {:<30} {}\n", kTargets[i].name, DisplayAddress(i), kTargets[i].thread, g_targets[i].status);
    return s;
}

std::string SessionHeader() {
    return std::format("\n==== Frame profiler session {} | game {} | clock {} | hitch = frame > max({:.1f} x median of the last {} frames, {:.1f} ms) ====\n"
                       "Times are ms per frame interval (Present to Present); 'self' = exclusive of nested timed calls; see frame_profiler.cpp.\n",
        NowString(), GetGameVersionName(), ClockText(), g_mult.load(), kMedianWindow, g_floorMs.load());
}

void AppendFile(const std::filesystem::path& path, const std::string& text) {
    std::ofstream f(path, std::ios::out | std::ios::app | std::ios::binary);
    if (f) f.write(text.data(), static_cast<std::streamsize>(text.size()));
}

// ---- writer thread ----
// A raw Win32 thread, not a global std::thread: a joinable std::thread destroyed at process exit calls std::terminate.
HANDLE g_writerThread = nullptr;
std::atomic<bool> g_writerStop{false};
HANDLE g_wake = nullptr;
std::mutex g_pendingMutex;
std::string g_pendingText;
std::filesystem::path g_filePath;

struct WriterArgs {
    std::filesystem::path path;
    std::string header;
};

void WriterMain(const std::filesystem::path& path, const std::string& header) {
    AppendFile(path, header);
    std::vector<HitchRecord> batch;
    batch.reserve(kQueueSize);
    for (;;) {
        const bool stop = g_writerStop.load();
        batch.clear();
        uint32_t t = g_qTail.load(std::memory_order_relaxed);
        const uint32_t h = g_qHead.load(std::memory_order_acquire);
        while (t != h) {
            batch.push_back(g_queue[t % kQueueSize]);
            t++;
        }
        g_qTail.store(t, std::memory_order_release);
        std::string out;
        for (const auto& r : batch) out += FormatHitch(r);
        {
            std::lock_guard<std::mutex> lk(g_pendingMutex);
            out += g_pendingText;
            g_pendingText.clear();
        }
        if (!out.empty()) AppendFile(path, out);
        g_qWritten.fetch_add(static_cast<uint32_t>(batch.size()), std::memory_order_relaxed);
        if (stop) break;
        WaitForSingleObject(g_wake, 1000); // batches at most once per second (sooner only for "Save report now")
    }
}

DWORD WINAPI WriterThreadProc(LPVOID param) {
    const std::unique_ptr<WriterArgs> args(static_cast<WriterArgs*>(param));
    WriterMain(args->path, args->header);
    return 0;
}

// Caller holds g_ctrlMutex
void StartWriter() {
    if (g_writerThread) return;
    ApexPaths::EnsureApexDirectory();
    g_filePath = std::filesystem::path(ApexPaths::ApexDirectory()) / L"ApexRadiance_Hitches.txt";
    {
        // appended across sessions: past 16 MB it becomes ApexRadiance_Hitches.old.txt (replacing the previous one), 07/10
        std::error_code ec;
        const auto size = std::filesystem::file_size(g_filePath, ec);
        if (!ec && size > (16u << 20)) {
            const auto old = std::filesystem::path(ApexPaths::ApexDirectory()) / L"ApexRadiance_Hitches.old.txt";
            std::filesystem::remove(old, ec);
            std::filesystem::rename(g_filePath, old, ec);
            if (!ec) LOG_INFO(std::format("[FrameProfiler] ApexRadiance_Hitches.txt was {:.1f} MB: moved to ApexRadiance_Hitches.old.txt", size / 1048576.0));
        }
    }
    if (!g_wake) g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_wake) {
        LOG_ERROR("[FrameProfiler] CreateEvent failed, ApexRadiance_Hitches.txt will not be written");
        return;
    }
    g_writerStop.store(false);
    g_qTail.store(g_qHead.load()); // nothing queued is carried over into a new session
    auto* args = new WriterArgs{g_filePath, SessionHeader()};
    g_writerThread = CreateThread(nullptr, 0, WriterThreadProc, args, 0, nullptr);
    if (!g_writerThread) {
        delete args;
        LOG_ERROR("[FrameProfiler] CreateThread failed, ApexRadiance_Hitches.txt will not be written");
        return;
    }
    g_writerRunning.store(true);
}

// The target is left off by an option (not counted in the summary)
bool OffByOption(int i) {
    return (kTargets[i].optional && !g_objectBuildWanted) || (i == T_MutexLock && !g_timeMutex.load());
}
const char* OffByOptionText(int i) {
    return i == T_MutexLock ? "Off (option: Advanced > Time the Mutex::Lock hook)" : "Off (optional: Advanced > Time lot object building)";
}

// Caller holds g_ctrlMutex. "Timing N of M game functions" (+ waiting ones), logged when it changes.
void UpdateSummaryLocked() {
    int timed = 0, total = 0, waiting = 0;
    for (int i = 0; i < kTargetCount; i++) {
        if (OffByOption(i) && !g_targets[i].attached) continue;
        total++;
        if (g_targets[i].attached) timed++;
        if (g_targets[i].waitingAddr) waiting++;
    }
    std::string s = std::format("Timing {} of {} game functions", timed, total);
    if (waiting) s += std::format(" ({} waiting for the game-address scan)", waiting);
    if (s != g_summary) {
        g_summary = s;
        LOG_INFO("[FrameProfiler] " + g_summary);
    }
}

// Remote-call jobs keyed by the method they run (the known bug of the combined build: these were never set). Caller holds
// g_ctrlMutex, render thread (JobKey reads them on the render thread only).
void ResolveRemoteCallKeysLocked() {
    if (g_remoteCallJobFn || !GameAddr::Scanned()) return;
    const uintptr_t fn = GameAddr::Get(GameAddr::Id::RemoteCallJob);
    // entry: cmp dword [esp+0Ch],4 (phase 4 = run); push esi; push edi; jne
    if (!fn || !MatchAt(fn, "83 7C 24 0C 04 56 57 75")) {
        LOG_WARNING("[FrameProfiler] Remote-call job function not found: remote calls are listed as their job function");
        return;
    }
    g_remoteMethodVtable = static_cast<uint32_t>(GameAddr::Get(GameAddr::Id::RemoteMethodVtable));
    g_remoteMethodVtable2 = static_cast<uint32_t>(GameAddr::Get(GameAddr::Id::RemoteMethodVtable2));
    g_remoteCallJobFn = static_cast<uint32_t>(fn);
}

// Caller holds g_ctrlMutex. Runs on the render thread at the first frame boundary after enabling.
void AttachAllLocked() {
    ResolveRemoteCallKeysLocked();
    for (int i = 0; i < kTargetCount; i++) {
        if (OffByOption(i)) {
            g_targets[i].status = OffByOptionText(i);
            continue;
        }
        if (!AttachTarget(i) && !g_targets[i].waitingAddr) LOG_WARNING(std::format("[FrameProfiler] {}: {}", kTargets[i].name, g_targets[i].status));
    }
    UpdateSummaryLocked();
    if (g_writerRunning.load()) {
        std::lock_guard<std::mutex> lk(g_pendingMutex);
        g_pendingText += "Timed functions:\n" + HookStatusText();
    }
}

// Caller holds g_ctrlMutex; render thread, frame boundary: the targets that waited for GameAddr::Resolve
void AttachWaitingLocked() {
    g_waitAddr.store(false);
    ResolveRemoteCallKeysLocked();
    bool any = false;
    for (int i = 0; i < kTargetCount; i++) {
        if (!g_targets[i].waitingAddr || OffByOption(i)) continue;
        any = true;
        if (!AttachTarget(i) && !g_targets[i].waitingAddr) LOG_WARNING(std::format("[FrameProfiler] {}: {}", kTargets[i].name, g_targets[i].status));
    }
    if (!any) return;
    UpdateSummaryLocked();
    if (g_writerRunning.load()) {
        std::lock_guard<std::mutex> lk(g_pendingMutex);
        g_pendingText += "Timed functions (after the game-address scan):\n" + HookStatusText();
    }
}

// Caller holds g_ctrlMutex: the Mutex::Lock option changed while the profiler may be on
void ApplyMutexOptionLocked() {
    if (!g_enabled.load() || g_attachPending.load()) return; // attached (or not) with the others at the next frame boundary
    if (g_timeMutex.load()) {
        if (!AttachTarget(T_MutexLock)) LOG_WARNING(std::format("[FrameProfiler] {}: {}", kTargets[T_MutexLock].name, g_targets[T_MutexLock].status));
    } else {
        DetachTarget(T_MutexLock);
        if (!g_targets[T_MutexLock].attached && g_targets[T_MutexLock].status == "Off") g_targets[T_MutexLock].status = OffByOptionText(T_MutexLock);
    }
    UpdateSummaryLocked();
}

// Caller holds g_ctrlMutex
void StopWriter() {
    g_writerRunning.store(false);
    if (!g_writerThread) return;
    g_writerStop.store(true);
    SetEvent(g_wake);
    WaitForSingleObject(g_writerThread, 5000);
    CloseHandle(g_writerThread);
    g_writerThread = nullptr;
}

// ---- report ----
LiveSample AverageLive() {
    LiveSample a{};
    if (!g_liveCount) return a;
    for (int i = 0; i < g_liveCount; i++) {
        const LiveSample& l = g_live[i];
        a.frameMs += l.frameMs;
        a.cpuMs += l.cpuMs;
        a.presentMs += l.presentMs;
        a.limiterMs += l.limiterMs;
        a.modMs += l.modMs;
        a.gameDraws += l.gameDraws;
        a.endFrameDraws += l.endFrameDraws;
        a.setTexture += l.setTexture;
        a.setShader += l.setShader;
        a.shaderConst += l.shaderConst;
        a.setRT += l.setRT;
    }
    const float n = static_cast<float>(g_liveCount);
    a.frameMs /= n;
    a.cpuMs /= n;
    a.presentMs /= n;
    a.limiterMs /= n;
    a.modMs /= n;
    a.gameDraws /= n;
    a.endFrameDraws /= n;
    a.setTexture /= n;
    a.setShader /= n;
    a.shaderConst /= n;
    a.setRT /= n;
    return a;
}

struct HitchAggregate {
    int count = 0, moving = 0, still = 0;
    double frameMs = 0, medianMs = 0, unattributed = 0;
    double render[kCatCount] = {}, others[kCatCount] = {}, worst[kCatCount] = {};
    uint64_t calls[kCatCount] = {};
    uint64_t createTex = 0, createShaders = 0, promoted = 0;
};

HitchAggregate AggregateHitches() {
    HitchAggregate a;
    for (int k = 0; k < g_hitchCount; k++) {
        const HitchRecord& h = g_hitches[k];
        a.count++;
        a.frameMs += h.frameMs;
        a.medianMs += h.medianMs;
        a.unattributed += h.unattributedMs;
        if (h.camera == 1) a.moving++;
        if (h.camera == 0) a.still++;
        for (int c = 0; c < kCatCount; c++) {
            a.render[c] += h.render[c];
            const double o = static_cast<double>(h.sim[c]) + h.other[c];
            a.others[c] += o;
            a.worst[c] = std::max(a.worst[c], static_cast<double>(h.render[c]) + o);
            a.calls[c] += h.calls[c];
        }
        a.createTex += h.createTex;
        a.createShaders += h.createVS + h.createPS;
        a.promoted += h.lotsPromoted;
    }
    return a;
}

std::string GcCallSiteText() {
    static uintptr_t site = 0;
    static bool tried = false;
    if (!tried) {
        tried = true;
        if (g_gameVersion == GameVersion::Steam) {
            if (MatchAt(0x00D819A0, "68 ?? ?? ?? ?? A3")) site = 0x00D819AA;
        } else if (const uintptr_t a = ScanUnique("68 ?? ?? ?? ?? A3 ?? ?? ?? ?? ?? ?? ?? ?? ?? A1 ?? ?? ?? ?? 83 C4 04 3B 05")) {
            site = a + 10;
        }
    }
    if (!site) return "call site in MonoScriptHost::Simulate not found";
    const uint8_t op = *reinterpret_cast<const uint8_t*>(site);
    if (op == 0x90) return "call removed (Chunky Patch - Disable GC_try_to_collect): no explicit collections to measure";
    if (op == 0xE8) {
        int32_t rel;
        std::memcpy(&rel, reinterpret_cast<const void*>(site + 1), 4);
        const uintptr_t target = site + 5 + static_cast<uintptr_t>(static_cast<intptr_t>(rel));
        if (target == g_targets[T_ScriptGC].addr || target == kTargets[T_ScriptGC].steam) return "calls GC_try_to_collect directly";
        return std::format("redirected to {:#010x} by another patch (e.g. Script GC Scheduler); timed whenever it calls GC_try_to_collect", target);
    }
    return "modified by another patch";
}

std::string LimiterText() {
    const uintptr_t fn = g_targets[T_RenderFrame].addr;
    if (!fn) return "unknown (render frame function not resolved)";
    const uintptr_t site = fn + 0xBA; // Steam 1.67.2: 0x00EC9FBA, the site Smooth Patch replaces
    if (MatchAt(site, "80 BE 8D 00 00 00 00 5E 74 15")) return "game default (sleeps ~30 ms per frame only while the window is inactive)";
    if (MatchAt(site, "3E 56 E8 ?? ?? ?? ?? 5E EB")) return "Smooth Patch frame limiter";
    return "modified by another patch";
}

// Dominant causes over the hitch ring (window in foreground), as dom.pl aggregates them: per camera state and frame-time
// bucket, the share of hitches each dominant item accounts for and its average ms; then the top counters. One line each.
std::vector<std::string> DominantSummaryLines() {
    struct Agg {
        int n = 0;
        double ms = 0;
    };
    static const char* const kBins[4] = {"< 16 ms", "16-25 ms", "25-50 ms", ">= 50 ms"};
    std::map<std::string, Agg> dom[3][4]; // camera: 0 moving, 1 still, 2 unknown
    std::map<std::string, Agg> top[3];
    int n[3][4] = {}, nCam[3] = {};
    const int first = (g_hitchPos - g_hitchCount + kHitchRing) % kHitchRing;
    for (int k = 0; k < g_hitchCount; k++) {
        const HitchRecord& h = g_hitches[(first + k) % kHitchRing];
        if (!h.foreground || h.domKind == kDomNone) continue;
        const int cam = h.camera == 1 ? 0 : (h.camera == 0 ? 1 : 2);
        const int bin = h.frameMs < 16.0f ? 0 : (h.frameMs < 25.0f ? 1 : (h.frameMs < 50.0f ? 2 : 3));
        Agg& d = dom[cam][bin][DomName(h)];
        d.n++;
        d.ms += h.domMs;
        n[cam][bin]++;
        nCam[cam]++;
        Agg& t = top[cam][h.topCounter >= 0 ? kCats[kFirstCounterCat + h.topCounter].name : "none"];
        t.n++;
        t.ms += h.topCounter >= 0 ? h.render[kFirstCounterCat + h.topCounter] : 0.0f;
    }
    static const char* const kCam[3] = {"camera moving", "camera still", "camera n/a"};
    auto sorted = [](const std::map<std::string, Agg>& m) {
        std::vector<std::pair<std::string, Agg>> v(m.begin(), m.end());
        std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second.n > b.second.n; });
        return v;
    };
    std::vector<std::string> lines;
    for (int cam = 0; cam < 3; cam++) {
        if (!nCam[cam]) continue;
        for (int bin = 0; bin < 4; bin++) {
            if (!n[cam][bin]) continue;
            std::string line = std::format("{}, {} (N={}):", kCam[cam], kBins[bin], n[cam][bin]);
            int shown = 0;
            for (const auto& [name, a] : sorted(dom[cam][bin])) {
                if (shown++ == 6) break;
                line += std::format("{} {} {:.0f}% (avg {:.1f} ms)", shown > 1 ? ";" : "", name, 100.0 * a.n / n[cam][bin], a.ms / a.n);
            }
            lines.push_back(line);
        }
        std::string line = std::format("{}, top counter (largest render self time of the six counters, N={}):", kCam[cam], nCam[cam]);
        int shown = 0;
        for (const auto& [name, a] : sorted(top[cam])) {
            line += std::format("{} {} {:.0f}%", shown++ ? ";" : "", name, 100.0 * a.n / nCam[cam]);
            if (name != "none") line += std::format(" (avg {:.2f} ms)", a.ms / a.n);
        }
        lines.push_back(line);
    }
    return lines;
}

// Counters since Clear (all frames) and per hitch (the hitch ring), for the report
std::string CounterReport() {
    const Stats& st = g_stats;
    bool any = false;
    for (int k = 0; k < kNC && !any; k++) any = st.cCalls[0][k] || st.cCalls[1][k] || st.cCalls[2][k];
    if (!any) return "";
    const double frames = static_cast<double>(std::max<uint64_t>(1, st.frames));
    std::string s = "Counters since Clear (calls x ms inclusive; longest single call): render thread | simulation thread | other threads | render thread per frame | extras\n";
    for (int k = 0; k < kNC; k++) {
        const uint64_t calls = st.cCalls[0][k] + st.cCalls[1][k] + st.cCalls[2][k];
        if (!calls) continue;
        s += std::format("   {:<22} {:>9} x {:>9.1f} (max {:>6.2f}) | {:>9} x {:>8.1f} | {:>9} x {:>8.1f} (max {:>6.2f}) | {:>8.2f} x {:>6.3f} ms{}\n", kCats[kFirstCounterCat + k].name,
                         st.cCalls[0][k], st.cIncl[0][k], st.cMax[0][k], st.cCalls[1][k], st.cIncl[1][k], st.cCalls[2][k], st.cIncl[2][k], st.cMax[1][k],
                         static_cast<double>(st.cCalls[0][k]) / frames, st.cIncl[0][k] / frames, CounterExtraText(st.cExtra, calls, k));
    }
    // per hitch (the ring)
    int count = 0;
    double incl[3][kNC] = {}, calls[3][kNC] = {}, self[kNC] = {};
    uint64_t extra[3][kExtraCount] = {};
    for (int j = 0; j < g_hitchCount; j++) {
        const HitchRecord& h = g_hitches[j];
        count++;
        for (int b = 0; b < 3; b++) {
            for (int k = 0; k < kNC; k++) {
                incl[b][k] += h.counters.incl[b][k];
                calls[b][k] += h.counters.calls[b][k];
            }
            for (int x = 0; x < kExtraCount; x++) extra[b][x] += h.counters.extra[b][x];
        }
        for (int k = 0; k < kNC; k++) self[k] += h.render[kFirstCounterCat + k];
    }
    if (count) {
        s += std::format("Counters per hitch (last {} hitches): render calls x ms incl. (self) | simulation | other | extras per hitch\n", count);
        for (int k = 0; k < kNC; k++) {
            const double c = calls[0][k] + calls[1][k] + calls[2][k];
            if (c <= 0.0) continue;
            uint64_t perHitch[3][kExtraCount];
            for (int b = 0; b < 3; b++)
                for (int x = 0; x < kExtraCount; x++) perHitch[b][x] = extra[b][x] / static_cast<uint64_t>(count);
            s += std::format("   {:<22} {:>8.1f} x {:>7.2f} ({:>6.2f}) | {:>8.1f} x {:>7.2f} | {:>8.1f} x {:>7.2f}{}\n", kCats[kFirstCounterCat + k].name, calls[0][k] / count,
                             incl[0][k] / count, self[k] / count, calls[1][k] / count, incl[1][k] / count, calls[2][k] / count, incl[2][k] / count,
                             CounterExtraText(perHitch, static_cast<uint64_t>(c / count + 0.5), k));
        }
    }
    return s;
}

// Texture loads by size (the DDS loader's create and fill calls, since Clear)
std::string TextureLoadReport() {
    std::string s;
    for (int b = 0; b <= kTexBuckets; b++) {
        const TexBucket& t = g_tex[b];
        const uint64_t n = t.loads.load(), f = t.fills.load();
        if (!n && !f) continue;
        const double c = static_cast<double>(t.createTicks.load()) * g_msPerTick, fl = static_cast<double>(t.fillTicks.load()) * g_msPerTick;
        s += std::format("   {:<16} {:>7} {:>9.1f} {:>7.3f} {:>7.2f} {:>9.1f} {:>7.3f} {:>7.2f} {:>9.1f}\n", b < kTexBuckets ? kTexBucketNames[b] : "size unknown", n, c,
            n ? c / static_cast<double>(n) : 0.0, static_cast<double>(t.createMax.load()) * g_msPerTick, fl, f ? fl / static_cast<double>(f) : 0.0,
            static_cast<double>(t.fillMax.load()) * g_msPerTick, static_cast<double>(t.bytes.load()) / (1024.0 * 1024.0));
    }
    if (s.empty()) return "";
    return "Texture loads by size (DDS loader: create = CALL 0x0060E1DC, fill = CALL 0x0060E1FF; all threads, since Clear):\n"
           "   level 0            loads  create ms     avg     max   fill ms     avg     max        MB\n" +
           s;
}

std::string PageFaultText() {
    const auto per = [](uint64_t faults, uint64_t frames) { return frames ? static_cast<double>(faults) / static_cast<double>(frames) : 0.0; };
    return std::format("Page faults per frame (process, soft and hard): hitch frames {:.0f}, other frames {:.0f}\n", per(g_pfSum[1], g_pfFrames[1]),
        per(g_pfSum[0], g_pfFrames[0]));
}

std::string BuildReport() {
    const LiveSample a = AverageLive();
    std::string s = std::format("\n==== Frame profiler report {} ====\n", NowString());
    s += std::format("Game {} | clock {} | hitch = frame > max({:.1f} x median of the last {} frames, {:.1f} ms)\n", GetGameVersionName(), ClockText(), g_mult.load(), kMedianWindow,
        g_floorMs.load());
    s += std::format("Frames {} | hitches {} ({:.2f}%) | p50 {:.2f} ms | p95 {:.2f} | p99 {:.2f} | max {:.1f}\n", g_stats.frames, g_stats.hitches,
        g_stats.frames ? 100.0 * static_cast<double>(g_stats.hitches) / static_cast<double>(g_stats.frames) : 0.0, Percentile(0.5), Percentile(0.95), Percentile(0.99), g_stats.maxFrameMs);
    s += std::format("Last {} frames: frame {:.2f} ms, cpu {:.2f}, present {:.2f}, limiter {:.2f}, mod D3D {:.2f}, draws {:.0f} (+{:.0f} end-of-frame)\n", g_liveCount, a.frameMs, a.cpuMs,
        a.presentMs, a.limiterMs, a.modMs, a.gameDraws, a.endFrameDraws);
    const auto rate = [](uint64_t h, uint64_t f) { return f ? 100.0 * static_cast<double>(h) / static_cast<double>(f) : 0.0; };
    s += std::format("Camera: moving in {} frames, still in {}; hitch rate while moving {:.2f}%, while still {:.2f}%\n", g_stats.framesMoving, g_stats.framesStill,
        rate(g_stats.hitchesMoving, g_stats.framesMoving), rate(g_stats.hitchesStill, g_stats.framesStill));
    {
        std::lock_guard<std::mutex> lk(g_ctrlMutex);
        s += "Hooks:\n" + HookStatusText();
    }
    s += "GC call site: " + GcCallSiteText() + "\nFrame limiter: " + LimiterText() + "\n";
    s += "Totals since Clear (self ms): category | render thread | simulation thread | other threads | calls\n";
    for (int c = 0; c < kCatCount; c++) {
        const uint64_t calls = g_stats.bucketCalls[0][c] + g_stats.bucketCalls[1][c] + g_stats.bucketCalls[2][c];
        if (calls || g_stats.render[c] > 0.0 || g_stats.sim[c] > 0.0 || g_stats.other[c] > 0.0)
            s += std::format("   {:<24} {:>12.1f} {:>12.1f} {:>12.1f} {:>10}\n", kCats[c].name, g_stats.render[c], g_stats.sim[c], g_stats.other[c], calls);
    }
    s += std::format("   {:<24} {:>12.1f}\n", "Unattributed (render)", g_stats.unattributed);
    s += CounterReport();
    s += TextureLoadReport();
    s += PageFaultText();
    s += AddressSpace::ReportText();
    s += "Apex shaders: " + ShaderCache::StatusText() + "\n";
    if (!g_regDisplay.empty()) {
        s += "Registry hooks by name (last second; Present hooks always, draw hooks with per-hook registry timing):\n";
        for (const auto& d : g_regDisplay) s += std::format("   {:<36} {:>7.3f} ms / frame {:>9.1f} calls / frame\n", d.name, d.msPerFrame, d.callsPerFrame);
    }
    s += std::format("Draw / state hooks called off the render thread (dispatched under the lock): {}\n", D3D9Hooks::OffThreadDispatches());
    s += "Resource lookup cache: " + ResourceCache::StatusText() + "\n";
    s += "Resource lookup cache counters: " + ResourceCache::ReportLine() + "\n";
    s += "Remember missing files: " + ResourceCache::MissesStatusText() + "\n";
    s += "File list cache: " + ResourceCache::KeyListStatusText() + "\n";
    s += "Lot lighting while moving: " + LotLightingMotion::StatusText() + "\n";
    s += "Wall shading while moving: " + LotLightingMotion::WallAoStatusText() + "\n";
    s += "Spread new objects over frames: " + SceneBudget::StatusText() + "\n";
    s += "Faster object lookups: " + ObjectIndex::StatusText() + "\n";
    s += "Faster cache compression, record checksums: " + FastCrc::StatusText() + "\n";
    s += "Faster memory handling: " + FastMemory::StatusText() + "\n";
    const HitchAggregate h = AggregateHitches();
    if (h.count) {
        s += std::format("Last {} hitches: average frame {:.2f} ms (median before them {:.2f}), camera moving in {}, still in {}; per hitch (self ms): category | render | other threads | worst\n",
            h.count, h.frameMs / h.count, h.medianMs / h.count, h.moving, h.still);
        for (int c = 0; c < kCatCount; c++)
            if (h.render[c] + h.others[c] > 0.0)
                s += std::format("   {:<24} {:>8.2f} {:>8.2f} {:>8.2f}\n", kCats[c].name, h.render[c] / h.count, h.others[c] / h.count, h.worst[c]);
        s += std::format("   {:<24} {:>8.2f}\n", "Unattributed (render)", h.unattributed / h.count);
        const std::vector<std::string> dom = DominantSummaryLines();
        if (!dom.empty()) {
            s += "Dominant cause of the last hitches (window in foreground; as research\\perf2\\tools\\dom.pl): % of the hitches of each bucket (average ms of that item)\n";
            for (const std::string& line : dom) s += "   " + line + "\n";
        }
        s += "Hitches (oldest first):\n";
        const int first = (g_hitchPos - g_hitchCount + kHitchRing) % kHitchRing;
        for (int k = 0; k < g_hitchCount; k++) s += FormatHitch(g_hitches[(first + k) % kHitchRing]);
    }
    s += KeyedReport();
    s += SamplingReport();
    s += "==== end of report ====\n";
    return s;
}

void SaveReport() {
    std::string text = BuildReport();
    if (g_writerRunning.load()) {
        {
            std::lock_guard<std::mutex> lk(g_pendingMutex);
            g_pendingText += text;
        }
        if (g_wake) SetEvent(g_wake);
        return;
    }
    // profiler off: a one-shot thread, so the file I/O stays off the render thread
    ApexPaths::EnsureApexDirectory();
    std::filesystem::path path = std::filesystem::path(ApexPaths::ApexDirectory()) / L"ApexRadiance_Hitches.txt";
    HookGuard::StartDetached("FrameProfiler: hitch file writer", [path, t = std::move(text)] { AppendFile(path, t); }); // (07/10: cannot end the game)
}

void Clear() {
    ClearSampleAggregates();
    for (auto& t : g_tex) {
        t.loads = 0;
        t.createTicks = 0;
        t.createMax = 0;
        t.fills = 0;
        t.fillTicks = 0;
        t.fillMax = 0;
        t.bytes = 0;
    }
    for (int b = 0; b < 2; b++) g_pfSum[b] = g_pfFrames[b] = 0;
    for (int b = 0; b < 2; b++) {
        g_aSvc[b].Clear();
        g_aJob[b].Clear();
        g_aWait[b].Clear();
        g_aReadBytes[b] = 0;
    }
    for (int i = 0; i < kOtherSvcSlots; i++) {
        g_otherSvcBaseIncl[i] = g_otherSvc[i].incl.load();
        g_otherSvcBaseSelf[i] = g_otherSvc[i].self.load();
        g_otherSvcBaseCalls[i] = g_otherSvc[i].calls.load();
    }
    g_stats = Stats{};
    g_graphPos = g_graphCount = 0;
    g_livePos = g_liveCount = 0;
    g_medianPos = g_medianCount = 0;
    g_hitchPos = g_hitchCount = 0;
    g_lastThreshold = g_lastMedian = 0.0f;
    g_regDisplay.clear();
}

// ---- UI ----
void Hint(const char* text) {
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", text);
}

// Measurement presets for the 60-second protocol (research\perf2\plan.md section 9.2). Both: hitch = 2.0 x median, floor 8 ms,
// state calls counted, the file written, per-hook registry timing and lot object building off, Mutex::Lock not timed.
// Timing run (A): no sampling. Sampling run (B): the render and simulation threads sampled at 2000 Hz. Turns the profiler on.
void ApplyPreset(bool sampling) {
    g_mult.store(2.0f);
    g_floorMs.store(8.0f);
    g_writeFile.store(true);
    g_sampleRender.store(sampling);
    g_sampleSim.store(sampling);
    g_sampleHz.store(2000);
    g_regTiming.store(false);
    g_regTimingActive.store(false);
    const bool stateChanged = !g_countState.exchange(true);
    {
        std::lock_guard<std::mutex> lk(g_ctrlMutex);
        g_timeMutex.store(false);
        ApplyMutexOptionLocked();
        if (g_objectBuildWanted) {
            g_objectBuildWanted = false;
            if (g_enabled.load() && !g_attachPending.load()) {
                DetachTarget(T_LotObjectBuild);
                UpdateSummaryLocked();
            }
        }
        if (stateChanged && g_enabled.load()) {
            D3D9Hooks::UnregisterAll(kHookName);
            RegisterD3DHooks();
        }
    }
    if (!g_enabled.load()) FrameProfiler::SetEnabled(true);
    {
        std::lock_guard<std::mutex> lk(g_ctrlMutex);
        UpdateSamplerLocked();
    }
    ApexConfig::RequestSave();
    LOG_INFO(std::format("[FrameProfiler] Measurement preset: {} run (hitch 2.0 x median, floor 8 ms, state calls counted, file on, Mutex::Lock not timed, sampling {})",
                         sampling ? "sampling" : "timing", sampling ? "render + simulation threads at 2000 Hz" : "off"));
}

// Averages of the counters over the last frames (g_cLive)
struct CounterAvg {
    double incl[3][kNC] = {}, calls[3][kNC] = {};
    double extra[3][kExtraCount] = {};
    int frames = 0;
};

CounterAvg AverageCounters() {
    CounterAvg a;
    a.frames = g_liveCount;
    if (!g_liveCount) return a;
    for (int i = 0; i < g_liveCount; i++) {
        const CounterFrame& f = g_cLive[i];
        for (int b = 0; b < 3; b++) {
            for (int k = 0; k < kNC; k++) {
                a.incl[b][k] += f.incl[b][k];
                a.calls[b][k] += f.calls[b][k];
            }
            for (int x = 0; x < kExtraCount; x++) a.extra[b][x] += static_cast<double>(f.extra[b][x]);
        }
    }
    const double n = static_cast<double>(g_liveCount);
    for (int b = 0; b < 3; b++) {
        for (int k = 0; k < kNC; k++) {
            a.incl[b][k] /= n;
            a.calls[b][k] /= n;
        }
        for (int x = 0; x < kExtraCount; x++) a.extra[b][x] /= n;
    }
    return a;
}

// Extra counts per frame (averages), for the UI
std::string CounterExtraPerFrame(const CounterAvg& a, int k) {
    auto sum = [&](int x) { return a.extra[0][x] + a.extra[1][x] + a.extra[2][x]; };
    const double calls = a.calls[0][k] + a.calls[1][k] + a.calls[2][k];
    switch (kFirstCounterCat + k) {
    case kResLookup:
        return std::format("{:.0f} packages per lookup, {:.1f} misses, {:.0f}% from cache", calls > 0 ? sum(kXPackages) / calls : 0.0, sum(kXMisses),
                           calls > 0 ? 100.0 * sum(kXCacheHits) / calls : 0.0);
    case kScenePending:
        return sum(kXDeferred) > 0 ? std::format("{:.1f} nodes, {:.1f} deferred", sum(kXNodes), sum(kXDeferred)) : std::format("{:.1f} nodes", sum(kXNodes));
    case kObjectLookup:
        return std::format("{:.0f}% from index", calls > 0 ? 100.0 * sum(kXIndexHits) / calls : 0.0);
    case kRefPackCompress:
        return std::format("{:.1f} -> {:.1f} KB", sum(kXBytesIn) / 1024.0, sum(kXBytesOut) / 1024.0);
    case kDxtEncode:
    case kTexCreate:
        return std::format("{:.3f} Mpx", sum(kXPixels) / 1e6);
    case kLotRoomSolve:
        return std::format("{:.2f} lot levels", calls);
    case kWallAo:
        return std::format("{:.2f} passes", calls);
    case kKeyList:
        return std::format("{:.0f} keys, {:.0f} packages from cache", sum(kXKeys), sum(kXListCached));
    default:
        return "";
    }
}

void RenderCounters() {
    if (!ImGui::TreeNodeEx("Counters", ImGuiTreeNodeFlags_DefaultOpen)) return;
    const CounterAvg a = AverageCounters();
    int hitches = 0;
    double perHitch[kNC] = {}, perHitchSelf[kNC] = {};
    for (int j = 0; j < g_hitchCount; j++) {
        hitches++;
        for (int k = 0; k < kNC; k++) {
            perHitch[k] += g_hitches[j].counters.incl[0][k];
            perHitchSelf[k] += g_hitches[j].render[kFirstCounterCat + k];
        }
    }
    if (ImGui::BeginTable("##FpCounters", 7, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
        ImGui::TableSetupColumn("Counter");
        ImGui::TableSetupColumn("Render / frame");
        ImGui::TableSetupColumn("Simulation / frame");
        ImGui::TableSetupColumn("Other / frame");
        ImGui::TableSetupColumn("Per hitch (render)");
        ImGui::TableSetupColumn("Longest call");
        ImGui::TableSetupColumn("Per frame");
        ImGui::TableHeadersRow();
        for (int k = 0; k < kNC; k++) {
            const int c = kFirstCounterCat + k;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(kCats[c].name);
            Hint(kCats[c].hint);
            for (int b = 0; b < 3; b++) {
                ImGui::TableNextColumn();
                ImGui::Text("%.1f x %.3f ms", a.calls[b][k], a.incl[b][k]);
            }
            ImGui::TableNextColumn();
            if (hitches) ImGui::Text("%.2f ms (self %.2f)", perHitch[k] / hitches, perHitchSelf[k] / hitches);
            else ImGui::TextDisabled("-");
            ImGui::TableNextColumn();
            ImGui::Text("%.2f / %.2f ms", g_stats.cMax[0][k], g_stats.cMax[1][k]);
            Hint("Longest single call since Clear: render thread / other threads.");
            ImGui::TableNextColumn();
            ImGui::TextUnformatted(CounterExtraPerFrame(a, k).c_str());
        }
        ImGui::EndTable();
    }
    ImGui::TextDisabled("Calls x ms (inclusive) per frame, averages of the last %d frames; per hitch = the last %d hitches.", a.frames, hitches);
    if (g_timeMutex.load())
        ImGui::TextColored(ImVec4(1.0f, 0.75f, 0.3f, 1.0f), "Mutex::Lock is timed: the Resource lookup times include its overhead (Advanced).");
    ImGui::TreePop();
}

void RenderLive() {
    const LiveSample a = AverageLive();
    ImGui::Text("Frame %.2f ms (%.0f FPS)   CPU %.2f   Present %.2f   Limiter %.2f", a.frameMs, a.frameMs > 0.0f ? 1000.0f / a.frameMs : 0.0f, a.cpuMs, a.presentMs, a.limiterMs);
    Hint("Averages of the last 60 frames.\nCPU = frame time minus the time blocked in Present and in the frame limiter.\n"
         "High CPU = game or draw submission bound. High Present = GPU or vsync bound.");
    ImGui::Text("Draws %.0f / frame (+%.0f end-of-frame)   Mod D3D hooks %.2f ms", a.gameDraws, a.endFrameDraws, a.modMs);
    Hint("Game draw calls (DrawIndexedPrimitive + DrawPrimitive) per frame; end-of-frame = draws inside EndScene (menus, Picture pass).\n"
         "DrawPrimitiveUP / DrawIndexedPrimitiveUP are not counted.\n"
         "Mod D3D hooks = time all Apex modules spend in their D3D9 hooks per frame: every outermost registry dispatch (draws, state\n"
         "changes, resource creation; a replaced draw's re-issue included), the Present hooks and Night Lighting's lamp refresh.");
    if (g_stateHooksActive) {
        ImGui::TextDisabled("Per frame: SetTexture %.0f   Set*Shader %.0f   Shader constants %.0f   SetRenderTarget %.0f", a.setTexture, a.setShader, a.shaderConst, a.setRT);
    }

    float values[kGraphFrames];
    const int n = g_graphCount;
    for (int k = 0; k < n; k++) values[k] = g_graph[(g_graphPos - n + k + kGraphFrames) % kGraphFrames];
    char overlay[96];
    std::snprintf(overlay, sizeof overlay, "last %.1f ms   hitch above %.1f ms", n ? values[n - 1] : 0.0f, g_lastThreshold);
    const float top = std::min(250.0f, std::max(33.4f, g_lastThreshold * 1.5f));
    ImGui::PlotLines("##FrameTimes", values, n, 0, overlay, 0.0f, top, ImVec2(ImGui::GetContentRegionAvail().x, 80.0f));
    Hint("Frame time of the last 300 frames (ms). The scale tops out at 1.5 x the hitch threshold.");

    ImGui::Text("p50 %.2f   p95 %.2f   p99 %.2f   max %.1f ms", Percentile(0.5), Percentile(0.95), Percentile(0.99), g_stats.maxFrameMs);
    Hint("Frame-time percentiles since Clear (0.05 ms resolution below 100 ms).");
    ImGui::Text("%llu frames, %llu hitches (%.2f%% over the threshold)", static_cast<unsigned long long>(g_stats.frames), static_cast<unsigned long long>(g_stats.hitches),
        g_stats.frames ? 100.0 * static_cast<double>(g_stats.hitches) / static_cast<double>(g_stats.frames) : 0.0);
    Hint("A hitch is a frame longer than max(multiplier x median of the last 120 frames, floor). Both are in Advanced.");
    const auto rate = [](uint64_t h, uint64_t f) { return f ? 100.0 * static_cast<double>(h) / static_cast<double>(f) : 0.0; };
    if (g_stats.framesMoving + g_stats.framesStill) {
        ImGui::TextDisabled("Camera moving in %.0f%% of frames. Hitch rate: %.2f%% moving, %.2f%% still", rate(g_stats.framesMoving, g_stats.framesMoving + g_stats.framesStill),
            rate(g_stats.hitchesMoving, g_stats.framesMoving), rate(g_stats.hitchesStill, g_stats.framesStill));
    } else {
        ImGui::TextDisabled("Camera motion: not seen yet (needs a loaded world)");
    }
}

void RenderHitches() {
    const HitchAggregate h = AggregateHitches();
    if (ImGui::TreeNodeEx("Last hitches", ImGuiTreeNodeFlags_DefaultOpen)) {
        if (!h.count) {
            ImGui::TextDisabled("No hitch yet.");
        } else {
            ImGui::TextDisabled("%d hitches: average %.1f ms (median before them %.1f ms); camera moving in %d, still in %d", h.count, h.frameMs / h.count, h.medianMs / h.count, h.moving,
                h.still);
            int order[kCatCount];
            int n = 0;
            for (int c = 0; c < kCatCount; c++)
                if (h.render[c] + h.others[c] > 0.005 * h.count) order[n++] = c;
            std::sort(order, order + n, [&](int a, int b) { return h.render[a] + h.others[a] > h.render[b] + h.others[b]; });
            if (ImGui::BeginTable("##HitchCats", 5, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
                ImGui::TableSetupColumn("Category");
                ImGui::TableSetupColumn("Render ms");
                ImGui::TableSetupColumn("Other threads ms");
                ImGui::TableSetupColumn("Worst ms");
                ImGui::TableSetupColumn("Calls");
                ImGui::TableHeadersRow();
                for (int k = 0; k < n; k++) {
                    const int c = order[k];
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(kCats[c].name);
                    Hint(kCats[c].hint);
                    ImGui::TableNextColumn();
                    ImGui::Text("%.2f", h.render[c] / h.count);
                    ImGui::TableNextColumn();
                    ImGui::Text("%.2f", h.others[c] / h.count);
                    ImGui::TableNextColumn();
                    ImGui::Text("%.1f", h.worst[c]);
                    ImGui::TableNextColumn();
                    if (c >= kFirstGameCat) ImGui::Text("%.1f", static_cast<double>(h.calls[c]) / h.count);
                }
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted("Unattributed (render)");
                Hint("Render-thread time in code that is not timed: game update, UI, scripts on the main thread, everything else.");
                ImGui::TableNextColumn();
                ImGui::Text("%.2f", h.unattributed / h.count);
                ImGui::EndTable();
            }
            ImGui::TextDisabled("Per hitch, self time. Created per hitch: %.1f textures, %.1f shaders; %.2f lots promoted", static_cast<double>(h.createTex) / h.count,
                static_cast<double>(h.createShaders) / h.count, static_cast<double>(h.promoted) / h.count);
            if (ImGui::TreeNode("Dominant causes")) {
                ImGui::TextDisabled("Largest single item of each hitch (render self category, service, job or Unattributed), by camera state and frame time;\n"
                                    "window in foreground only. Written into every hitch of ApexRadiance_Hitches.txt as \"dominant:\".");
                const std::vector<std::string> lines = DominantSummaryLines();
                if (lines.empty()) ImGui::TextDisabled("No hitch with the window in foreground yet.");
                for (const std::string& line : lines) ImGui::TextWrapped("%s", line.c_str());
                ImGui::TreePop();
            }
        }
        ImGui::TreePop();
    }
    if (h.count && ImGui::TreeNode("Recent hitches")) {
        const int shown = std::min(g_hitchCount, 25);
        for (int k = 0; k < shown; k++) {
            const HitchRecord& r = g_hitches[(g_hitchPos - 1 - k + kHitchRing) % kHitchRing];
            float all[kCatCount];
            for (int c = 0; c < kCatCount; c++) all[c] = r.render[c] + r.sim[c] + r.other[c];
            int top[3] = {-1, -1, -1};
            for (int c = 0; c < kCatCount; c++) {
                if (all[c] < 0.05f) continue;
                for (int j = 0; j < 3; j++) {
                    if (top[j] < 0 || all[c] > all[top[j]]) {
                        for (int m = 2; m > j; m--) top[m] = top[m - 1];
                        top[j] = c;
                        break;
                    }
                }
            }
            std::string line = std::format("{:.1f} ms  camera {}  draws {}  |", r.frameMs, CameraText(r.camera), r.gameDraws);
            for (int j = 0; j < 3; j++)
                if (top[j] >= 0) line += std::format("  {} {:.1f}", kCats[top[j]].name, all[top[j]]);
            line += std::format("  Unattributed {:.1f}", r.unattributedMs);
            if (r.domKind != kDomNone) line += "  | " + DomName(r);
            ImGui::TextUnformatted(line.c_str());
        }
        ImGui::TreePop();
    }
}

// Services / jobs / waits of the render thread, hitch frames vs other frames
template <uint32_t N, typename NameFn>
void KeyedTable(const char* id, const TimeTable<N>& hi, const TimeTable<N>& lo, NameFn name, bool showSelf) {
    const double hitches = static_cast<double>(g_stats.hitches), others = static_cast<double>(g_stats.frames - g_stats.hitches);
    constexpr int kRows = 15;
    int idx[kRows];
    const int n = hi.Top(idx, kRows);
    if (!n) {
        ImGui::TextDisabled("Nothing in hitch frames yet.");
        return;
    }
    if (!ImGui::BeginTable(id, showSelf ? 5 : 4, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) return;
    ImGui::TableSetupColumn("Name");
    ImGui::TableSetupColumn("ms / hitch");
    if (showSelf) ImGui::TableSetupColumn("self ms / hitch");
    ImGui::TableSetupColumn("ms / other frame");
    ImGui::TableSetupColumn("Count in hitches");
    ImGui::TableHeadersRow();
    for (int i = 0; i < n; i++) {
        const int j = idx[i];
        const int o = lo.Find(hi.keys[j]);
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(name(hi.keys[j], hi.aux[j]).c_str());
        ImGui::TableNextColumn();
        ImGui::Text("%.2f", hitches > 0 ? static_cast<double>(hi.incl[j]) * g_msPerTick / hitches : 0.0);
        if (showSelf) {
            ImGui::TableNextColumn();
            ImGui::Text("%.2f", hitches > 0 ? static_cast<double>(hi.self[j]) * g_msPerTick / hitches : 0.0);
        }
        ImGui::TableNextColumn();
        ImGui::Text("%.2f", o >= 0 && others > 0 ? static_cast<double>(lo.incl[o]) * g_msPerTick / others : 0.0);
        ImGui::TableNextColumn();
        ImGui::Text("%u", hi.calls[j]);
    }
    ImGui::EndTable();
}

void RenderKeyed() {
    ImGui::SeparatorText("Services, jobs and waits");
    if (ImGui::TreeNode("Services (render thread)")) {
        ImGui::TextDisabled("Each service update of the main loop, incl. = with the timed functions inside it, self = without.");
        KeyedTable("##svc", g_aSvc[1], g_aSvc[0], [](uint32_t k, uint32_t aux) { return ServiceName(k, aux); }, true);
        ImGui::TreePop();
    }
    if (ImGui::TreeNode("Jobs run on the render thread")) {
        ImGui::TextDisabled("By job function; remote calls by the method they run.");
        KeyedTable("##job", g_aJob[1], g_aJob[0], [](uint32_t k, uint32_t) { return JobName(k); }, true);
        ImGui::TreePop();
    }
    if (ImGui::TreeNode("Render-thread waits")) {
        ImGui::TextDisabled("Job waits by job; mutex / semaphore calls that blocked > 0.1 ms by return address.");
        KeyedTable("##wait", g_aWait[1], g_aWait[0], [](uint32_t k, uint32_t aux) { return WaitName(k, aux); }, false);
        ImGui::TreePop();
    }
    if (ImGui::TreeNode("Services on other threads")) {
        ImGui::TextDisabled("The simulation loop (FUN_0059ED70) and any service update outside the render thread, total since Clear.");
        for (int i = 0; i < kOtherSvcSlots; i++) {
            const uint32_t key = g_otherSvc[i].key.load();
            if (!key) continue;
            const uint64_t in = g_otherSvc[i].incl.load() - g_otherSvcBaseIncl[i], se = g_otherSvc[i].self.load() - g_otherSvcBaseSelf[i];
            ImGui::Text("%-44s %9.1f ms (self %.1f), %u calls", ServiceName(key, g_otherSvc[i].vtable.load()).c_str(), static_cast<double>(in) * g_msPerTick,
                static_cast<double>(se) * g_msPerTick, g_otherSvc[i].calls.load() - g_otherSvcBaseCalls[i]);
        }
        ImGui::TreePop();
    }
}

// One sampling table: code (or call site) | hitch % | other frames % | module
void SamplingTable(const char* id, const char* first, uint32_t* keys, uint32_t* counts, int n, bool sites, uint32_t hiTotal, uint32_t loTotal,
    uint32_t (*loGet)(uint32_t)) {
    if (!n || !ImGui::BeginTable(id, 4, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) return;
    ImGui::TableSetupColumn(first);
    ImGui::TableSetupColumn("Hitch %");
    ImGui::TableSetupColumn("Other frames %");
    ImGui::TableSetupColumn("Class");
    ImGui::TableHeadersRow();
    for (int i = 0; i < n; i++) {
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        if (sites) ImGui::Text("%08X", keys[i]);
        else ImGui::TextUnformatted(KeyText(keys[i]).c_str());
        ImGui::TableNextColumn();
        ImGui::Text("%.1f", Pct(counts[i], hiTotal));
        ImGui::TableNextColumn();
        ImGui::Text("%.1f", Pct(loGet(keys[i]), loTotal));
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(sites ? "TS3W" : kClassNames[KeyClass(keys[i])]);
    }
    ImGui::EndTable();
}

void RenderSampling(bool& save) {
    ImGui::SeparatorText("Sampling");
    bool render = g_sampleRender.load(), sim = g_sampleSim.load();
    bool changed = false;
    if (ApexUi::Checkbox("Sample the render thread", &render)) changed = true;
    Hint("A sampler thread pauses the render thread ~2000 times a second for a few microseconds and records where it is\n"
         "(EIP and the TS3W call sites on its stack). Hitches then show which code the Unattributed time was spent in.\n"
         "Costs the paused time shown below (a few % of the render thread) plus a little CPU on another core.");
    if (ApexUi::Checkbox("Sample the simulation thread", &sim)) changed = true;
    Hint("Same for the simulation thread (the thread that calls GC_try_to_collect; unknown until the first script GC).");
    int hz = g_sampleHz.load();
    if (ImGui::SliderInt("Sampling rate", &hz, 250, 4000, "%d Hz")) g_sampleHz.store(hz);
    save |= ImGui::IsItemDeactivatedAfterEdit();
    Hint("Samples per second and per thread. Higher = finer detail, more pause time.");
    if (changed) {
        g_sampleRender.store(render);
        g_sampleSim.store(sim);
        std::lock_guard<std::mutex> lk(g_ctrlMutex);
        UpdateSamplerLocked();
        save = true;
    }
    // measured rate, refreshed once a second
    static uint64_t lastTick = 0;
    static uint32_t lastTaken = 0;
    static float rate = 0.0f;
    const uint64_t tick = GetTickCount64();
    if (tick - lastTick >= 1000) {
        const uint32_t taken = g_sTaken.load();
        rate = lastTick ? static_cast<float>(taken - lastTaken) * 1000.0f / static_cast<float>(tick - lastTick) : 0.0f;
        lastTick = tick;
        lastTaken = taken;
    }
    const uint32_t taken = g_sTaken.load();
    const double costUs = taken ? static_cast<double>(g_sPausedTicks.load()) * g_msPerTick * 1000.0 / taken : 0.0;
    ImGui::TextDisabled("Sampler %s: %.0f samples/s, %.1f us paused per sample (~%.1f%% of a sampled thread), %u dropped", g_samplerRunning.load() ? "on" : "off", rate, costUs,
        rate > 0.0f && (render || sim) ? rate / ((render && sim) ? 2.0 : 1.0) * costUs / 10000.0 : 0.0, g_sDropped.load());
    const DWORD simTid = g_simTid.load();
    ImGui::TextDisabled("Render thread %lu, simulation thread %s", static_cast<unsigned long>(g_renderTid.load()), simTid ? std::to_string(simTid).c_str() : "not identified yet");

    const SampleAgg& hi = g_agg[1];
    const SampleAgg& lo = g_agg[0];
    for (int thr = 0; thr < 2; thr++) {
        if (!hi.total[thr] && !lo.total[thr]) continue;
        ImGui::PushID(thr);
        if (ImGui::TreeNode(thr == 0 ? "Render thread samples" : "Simulation thread samples")) {
            ImGui::TextDisabled("%u samples in hitch frames, %u in other frames", hi.total[thr], lo.total[thr]);
            std::string line = "Hitch frames:";
            for (int c = 0; c < kClsCount; c++) line += std::format("  {} {:.0f}%", kClassNames[c], Pct(hi.cls[thr][c], hi.total[thr]));
            ImGui::TextUnformatted(line.c_str());
            line = "Other frames:";
            for (int c = 0; c < kClsCount; c++) line += std::format("  {} {:.0f}%", kClassNames[c], Pct(lo.cls[thr][c], lo.total[thr]));
            ImGui::TextUnformatted(line.c_str());
            Hint("Where the thread's time went: TS3W = the game's own code, DXVK/driver = d3d9.dll / dxgi / Vulkan driver,\n"
                 "system = ntdll / kernel32 / kernelbase / user32 (waits, heap, I/O), Apex = this mod (official S3SS counts as other ASI).");
            constexpr int kRows = 20;
            uint32_t keys[kRows], counts[kRows];
            static const SampleAgg* s_lo = nullptr;
            static int s_thr = 0;
            s_lo = &lo;
            s_thr = thr;
            ImGui::TextDisabled("Hottest code in hitch frames ('fn~' = TS3W function start, guessed; resolve it with the decompile)");
            int n = hi.fn[thr].Top(keys, counts, kRows);
            SamplingTable("##fn", "Code", keys, counts, n, false, hi.total[thr], lo.total[thr], [](uint32_t k) { return s_lo->fn[s_thr].Get(k); });
            if (thr == 0) {
                ImGui::TextDisabled("TS3W call sites on the stack (return addresses: who called into the hot code)");
                n = hi.site.Top(keys, counts, kRows);
                SamplingTable("##site", "Return address", keys, counts, n, true, hi.total[0], lo.total[0], [](uint32_t k) { return s_lo->site.Get(k); });
                ImGui::TextDisabled("System code (waits, heap, I/O) called from");
                n = hi.wait.Top(keys, counts, 10);
                SamplingTable("##wait", "Return address", keys, counts, n, true, hi.total[0], lo.total[0], [](uint32_t k) { return s_lo->wait.Get(k); });
            }
            ImGui::TreePop();
        }
        ImGui::PopID();
    }
}

void RenderAdvanced() {
    if (!ApexUi::AdvancedNode("Advanced##FrameProfiler")) return;
    bool save = false;
    float mult = g_mult.load();
    if (ImGui::SliderFloat("Hitch multiplier", &mult, 1.2f, 5.0f, "%.1fx")) g_mult.store(mult);
    save |= ImGui::IsItemDeactivatedAfterEdit();
    Hint("A frame is a hitch when it is longer than this many times the median of the last 120 frames (and than the floor).");
    float floorMs = g_floorMs.load();
    if (ImGui::SliderFloat("Hitch floor", &floorMs, 1.0f, 100.0f, "%.0f ms")) g_floorMs.store(floorMs);
    save |= ImGui::IsItemDeactivatedAfterEdit();
    Hint("Frames shorter than this are never hitches.");
    bool state = g_countState.load();
    if (ApexUi::Checkbox("Count state calls", &state)) {
        g_countState.store(state);
        std::lock_guard<std::mutex> lk(g_ctrlMutex);
        if (g_enabled.load()) {
            D3D9Hooks::UnregisterAll(kHookName);
            RegisterD3DHooks();
        }
        save = true;
    }
    Hint("Also show SetTexture, Set*Shader, Set*ShaderConstantF and SetRenderTarget per frame. The registry's detours count them with plain\n"
         "counters (no callback, a nanosecond per call, always on in the development build); Apex's own state changes around a replaced draw\n"
         "bypass the detours and are not counted.");
    bool file = g_writeFile.load();
    if (ApexUi::Checkbox("Write ApexRadiance_Hitches.txt", &file)) {
        g_writeFile.store(file);
        save = true;
    }
    Hint("Append every hitch to ApexRadiance_Hitches.txt in the Apex Radiance folder (Documents), in batches at most once per second.");
    bool objects = g_objectBuildWanted;
    if (ApexUi::Checkbox("Time lot object building (this session)", &objects)) {
        std::lock_guard<std::mutex> lk(g_ctrlMutex);
        g_objectBuildWanted = objects;
        if (g_enabled.load() && !g_attachPending.load()) {
            if (objects) AttachTarget(T_LotObjectBuild);
            else DetachTarget(T_LotObjectBuild);
            UpdateSummaryLocked();
        }
    }
    Hint("Also time Lot::UpdateObjectSceneNode (every object's scene node when a lot gets detailed).\n"
         "While on, Lot Streaming Optimizations cannot re-install (changing its settings turns its object throttle off),\n"
         "so leave its settings alone during the measurement. Not saved.");
    bool mutexTimed = g_timeMutex.load();
    if (ApexUi::Checkbox("Time the Mutex::Lock hook", &mutexTimed)) {
        std::lock_guard<std::mutex> lk(g_ctrlMutex);
        g_timeMutex.store(mutexTimed);
        ApplyMutexOptionLocked();
        save = true;
    }
    Hint("Hook EA::Thread::Mutex::Lock (FUN_004E16F0) on every thread and book the render-thread calls that blocked more than\n"
         "0.1 ms (\"Mutex wait\"). Off by default: every call pays two clock reads, and the resource lookup takes the lock about\n"
         "580 times per scan, so this hook inflates exactly what the Resource lookup counter measures (and its samples).\n"
         "Turn it on only to look for lock contention.");
    bool reg = g_regTiming.load();
    if (ApexUi::Checkbox("Per-hook registry timing", &reg)) {
        g_regTiming.store(reg);
        g_regTimingActive.store(reg && g_enabled.load());
    }
    Hint("Time of each module's D3D9 registry draw hooks by name (measured by framework/d3d9_hooks.cpp; costs two clock reads per\n"
         "callback per draw). The Present hooks are always timed by name while the profiler is on.");
    RenderKeyed();
    RenderSampling(save);
    if (save) ApexConfig::RequestSave();

    ImGui::SeparatorText("Hooks");
    {
        std::lock_guard<std::mutex> lk(g_ctrlMutex);
        if (!g_summary.empty()) ImGui::TextDisabled("%s", g_summary.c_str());
        if (ImGui::BeginTable("##FpHooks", 4, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
            ImGui::TableSetupColumn("Function");
            ImGui::TableSetupColumn("Address");
            ImGui::TableSetupColumn("Calls R / S / O");
            ImGui::TableSetupColumn("Status");
            ImGui::TableHeadersRow();
            for (int i = 0; i < kTargetCount; i++) {
                const int c = kTargetCat[i];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(kTargets[i].name);
                Hint(kCats[c].hint);
                ImGui::TableNextColumn();
                ImGui::Text("0x%08X", static_cast<unsigned>(DisplayAddress(i)));
                ImGui::TableNextColumn();
                uint64_t calls[3];
                for (int b = 0; b < 3; b++) // FUN_00611680 returns after the Present boundary, so its calls are booked as Present (driver)
                    calls[b] = g_stats.bucketCalls[b][c] + (i == T_EndFramePresent ? g_stats.bucketCalls[b][kPresentDriver] : 0);
                ImGui::Text("%llu / %llu / %llu", static_cast<unsigned long long>(calls[0]), static_cast<unsigned long long>(calls[1]), static_cast<unsigned long long>(calls[2]));
                Hint("Calls since Clear on the render thread / simulation thread / other threads.");
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(g_targets[i].status.c_str());
            }
            ImGui::EndTable();
        }
    }
    ImGui::TextDisabled("GC call site: %s", GcCallSiteText().c_str());
    ImGui::TextDisabled("Frame limiter: %s", LimiterText().c_str());
    ImGui::TextDisabled("Clock: %s", ClockText().c_str());
    ImGui::TextDisabled("Not measurable through the D3D9 hook registry: the duration of resource creation (hooks run before the call; counts only),\n"
                        "CreateVertexBuffer / CreateIndexBuffer, Lock / Unlock, SetRenderState, DrawPrimitiveUP / DrawIndexedPrimitiveUP.");
    if (g_writerRunning.load() || g_qWritten.load() || g_qDropped.load())
        ImGui::TextDisabled("ApexRadiance_Hitches.txt: %u hitches written, %u dropped (queue full)", g_qWritten.load(), g_qDropped.load());

    if (ImGui::TreeNode("Totals since Clear")) {
        if (ImGui::BeginTable("##FpTotals", 5, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_RowBg | ImGuiTableFlags_BordersInnerV)) {
            ImGui::TableSetupColumn("Category");
            ImGui::TableSetupColumn("Render s");
            ImGui::TableSetupColumn("Simulation s");
            ImGui::TableSetupColumn("Other s");
            ImGui::TableSetupColumn("Calls");
            ImGui::TableHeadersRow();
            for (int c = 0; c < kCatCount; c++) {
                const uint64_t calls = g_stats.bucketCalls[0][c] + g_stats.bucketCalls[1][c] + g_stats.bucketCalls[2][c];
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted(kCats[c].name);
                Hint(kCats[c].hint);
                ImGui::TableNextColumn();
                ImGui::Text("%.2f", g_stats.render[c] / 1000.0);
                ImGui::TableNextColumn();
                ImGui::Text("%.2f", g_stats.sim[c] / 1000.0);
                ImGui::TableNextColumn();
                ImGui::Text("%.2f", g_stats.other[c] / 1000.0);
                ImGui::TableNextColumn();
                ImGui::Text("%llu", static_cast<unsigned long long>(calls));
            }
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::TextUnformatted("Unattributed (render)");
            ImGui::TableNextColumn();
            ImGui::Text("%.2f", g_stats.unattributed / 1000.0);
            ImGui::EndTable();
        }
        ImGui::TextDisabled("Lots promoted %llu, demoted %llu", static_cast<unsigned long long>(g_stats.lotsPromoted), static_cast<unsigned long long>(g_stats.lotsDemoted));
        ImGui::TreePop();
    }

    if (g_regTiming.load() || !g_regDisplay.empty()) {
        ImGui::SeparatorText("Registry hooks by name");
        Hint("Present hooks are always timed by name while the profiler is on (\"<name> (Present)\"); the draw hooks only with \"Per-hook registry timing\".");
        if (g_regDisplay.empty()) {
            ImGui::TextDisabled("No data yet (updated once a second).");
        } else {
            for (const auto& d : g_regDisplay) ImGui::Text("%-36s %.3f ms / frame   %.0f calls / frame", d.name.c_str(), d.msPerFrame, d.callsPerFrame);
        }
    }
    ImGui::TextDisabled("Draw / state hooks called from another thread than the render thread: %u (dispatched under the lock)", D3D9Hooks::OffThreadDispatches());
    Hint("The draw and state hooks run without a lock on the render thread. A call from another thread takes the registry lock instead; if this "
         "number grows, a module's draw hooks may race with the render thread (see ApexRadiance_LOG.txt, [D3D9Hooks]).");
    ImGui::TreePop();
}

} // namespace

namespace FrameProfiler {

void SetEnabled(bool on) {
    if (kPublicBuild && on) return;
    std::lock_guard<std::mutex> lk(g_ctrlMutex);
    if (on == g_enabled.load()) return;
    LotLoadAnalysis::Reset();
    if (on) {
        std::call_once(g_clockOnce, InitClock);
        g_needBaseline.store(true);
        g_summary = "Waiting for the first frame to attach the timed functions";
        StartWriter(); // before the Present hook exists: the queue has no producer yet
        g_attachPending.store(true);
        RegisterD3DHooks(); // the first Present attaches the game functions (OnPresentStart)
        g_enabled.store(true);
        g_regTimingActive.store(g_regTiming.load());
        UpdateSamplerLocked();
        LOG_INFO("[FrameProfiler] On");
    } else {
        g_enabled.store(false);
        g_attachPending.store(false);
        g_waitAddr.store(false);
        g_regTimingActive.store(false);
        UpdateSamplerLocked(); // stops it: g_enabled is false
        D3D9Hooks::UnregisterAll(kHookName);
        for (int i = 0; i < kTargetCount; i++) {
            DetachTarget(i);
            g_targets[i].waitingAddr = false;
        }
        StopWriter();
        g_summary = "Off";
        LOG_INFO("[FrameProfiler] Off");
    }
}

bool IsEnabled() {
    return g_enabled.load();
}

void Shutdown() {
    SetEnabled(false);
}

void RenderUI(bool showEnable) {
    bool on = g_enabled.load();
    if (showEnable) { // the Violet menu's card has its own switch
        if (ApexUi::Checkbox("Enable frame profiler", &on)) {
            SetEnabled(on);
            ApexConfig::RequestSave();
        }
        Hint("Measures every frame and records what each hitch is made of: lot streaming and LOD, lot lighting, terrain,\n"
             "script GC, the game's render and Present, the frame limiter and the mod's own D3D9 hooks.\n"
             "Off = nothing is hooked, no cost.");
    }
    if (on || g_stats.frames) {
        const float actionsW = ApexUi::ButtonWidth("Clear##FrameProfiler", true) + ImGui::GetStyle().ItemSpacing.x + ApexUi::ButtonWidth("Save report now", true);
        if (ApexUi::BeginControlRow("Collected measurement", "Clear old data before the next run", actionsW)) {
            if (ApexUi::IconTextButton("Clear##FrameProfiler", ApexUi::IconId::RotateCcw)) Clear();
            ImGui::SameLine();
            if (ApexUi::IconTextButton("Save report now", ApexUi::IconId::Save)) SaveReport();
            ApexUi::EndControlRow();
        }
    }
    if (ApexUi::BeginAdvanced("MeasurementSetup", "Measurement setup")) {
        const float presetWidth = ApexUi::ButtonWidth("Timing run##FpPreset", false) + ImGui::GetStyle().ItemSpacing.x + ApexUi::ButtonWidth("Sampling run##FpPreset", false);
        if (ApexUi::BeginControlRow("Measurement preset", "Choose a timing or sampling run", presetWidth)) {
        if (ApexUi::TextButton("Timing run##FpPreset")) ApplyPreset(false);
        Hint("Recommended settings for the 60-second measurement, run type A (research\\perf2\\plan.md section 9):\n"
             "hitch multiplier 2.0, floor 8 ms, count state calls on, write the file on, sampling off, Mutex::Lock not timed,\n"
             "per-hook registry timing and lot object building off. Turns the profiler on.\n"
             "Then press Clear right before the run and Save report now right after it.");
        ImGui::SameLine();
        if (ApexUi::TextButton("Sampling run##FpPreset")) ApplyPreset(true);
        Hint("Recommended settings for the attribution run, run type B: the same as the timing run, plus sampling of the\n"
             "render and simulation threads at 2000 Hz. Turns the profiler on.\n"
             "Then press Clear right before the run and Save report now right after it.");
        ApexUi::EndControlRow();
        }
        RenderAdvanced();
        ApexUi::EndAdvanced();
    }
    if (!g_stats.frames) {
        ImGui::TextDisabled("%s", on ? "Waiting for frames..." : "Off: nothing is hooked.");
    } else {
        if (!on) ImGui::TextDisabled("Off: showing the data collected so far.");
        RenderLive();
        if (ApexUi::BeginAdvanced("CollectedDetails", "Collected timing details")) {
            RenderHitches();
            RenderCounters();
            ApexUi::EndAdvanced();
        }
    }
}

void SaveToToml(toml::table& qolTable) {
    toml::table t;
    t.insert("enabled", g_enabled.load());
    t.insert("hitch_multiplier", static_cast<double>(g_mult.load()));
    t.insert("hitch_floor_ms", static_cast<double>(g_floorMs.load()));
    t.insert("count_state_calls", g_countState.load());
    t.insert("write_file", g_writeFile.load());
    t.insert("sample_render", g_sampleRender.load());
    t.insert("sample_simulation", g_sampleSim.load());
    t.insert("sample_hz", static_cast<int64_t>(g_sampleHz.load()));
    t.insert("time_mutex_lock", g_timeMutex.load());
    t.insert("time_objects", g_objectBuildWanted.load());
    t.insert("registry_timing", g_regTiming.load());
    qolTable.insert("frame_profiler", std::move(t));
}

void LoadFromToml(const toml::table& qolTable) {
    if (kPublicBuild) return;
    bool enabled = false;
    bool mutexChanged = false;
    if (auto node = qolTable["frame_profiler"].as_table()) {
        const auto& t = *node;
        g_mult.store(std::clamp(static_cast<float>(t["hitch_multiplier"].value_or(2.0)), 1.2f, 5.0f));
        g_floorMs.store(std::clamp(static_cast<float>(t["hitch_floor_ms"].value_or(8.0)), 1.0f, 100.0f));
        const bool state = t["count_state_calls"].value_or(true);
        g_writeFile.store(t["write_file"].value_or(true));
        g_sampleRender.store(t["sample_render"].value_or(false));
        g_sampleSim.store(t["sample_simulation"].value_or(false));
        g_sampleHz.store(std::clamp(static_cast<int>(t["sample_hz"].value_or(int64_t{2000})), 250, 4000));
        g_objectBuildWanted.store(t["time_objects"].value_or(false));
        g_regTiming.store(t["registry_timing"].value_or(false));
        const bool mutexTimed = t["time_mutex_lock"].value_or(false);
        mutexChanged = mutexTimed != g_timeMutex.exchange(mutexTimed);
        enabled = t["enabled"].value_or(false);
        if (state != g_countState.load()) {
            g_countState.store(state);
            std::lock_guard<std::mutex> lk(g_ctrlMutex);
            if (g_enabled.load()) {
                D3D9Hooks::UnregisterAll(kHookName);
                RegisterD3DHooks();
            }
        }
    }
    SetEnabled(enabled);
    std::lock_guard<std::mutex> lk(g_ctrlMutex); // sampling / Mutex::Lock options may have changed while the profiler stays on
    UpdateSamplerLocked();
    if (mutexChanged) ApplyMutexOptionLocked();
}

bool RegistryHookTimingActive() {
    return g_regTimingActive.load(std::memory_order_relaxed);
}

uint64_t Ticks() {
    return Now();
}

bool PresentHookTimingActive() {
    return g_enabled.load(std::memory_order_relaxed);
}

bool ModTimeActive() {
    return g_enabled.load(std::memory_order_relaxed);
}

// The registry's outermost dispatches (D3DDispatch) and Night Lighting's lamp refresh: a timed frame on the calling
// thread's slot, closed by EndModTime with the same key (Pop checks it; a frame dropped meanwhile is ignored).
int BeginModTime(ModTime what, const void* key) {
    if (!g_enabled.load(std::memory_order_relaxed)) return -1;
    ThreadSlot* s = GetSlot();
    if (!s) return -1;
    const int cat = what == ModTime::LampRefresh ? kLampRefresh : what == ModTime::ReissuedDraw ? kReissuedDraw : kModD3DHooks;
    return Push(s, cat, key, 0);
}

int BeginModTimeSampled(ModTime what, const void* key, uint32_t scale) {
    if (!g_enabled.load(std::memory_order_relaxed)) return -1;
    ThreadSlot* s = GetSlot();
    if (!s || s->scale != 1 || scale < 1) return -1; // no sample inside a sample
    const int cat = what == ModTime::LampRefresh ? kLampRefresh : what == ModTime::ReissuedDraw ? kReissuedDraw : kModD3DHooks;
    const int idx = Push(s, cat, key, kScaleRoot);
    if (idx >= 0) s->scale = scale;
    return idx;
}

void EndModTime(int token, const void* key) {
    ThreadSlot* s = t_slot;
    if (!s || token < 0) return;
    Pop(s, token, key, Now(), 0, 0);
}

// Keyed by the name string's address (fast path), verified by content: the registry's short names live inside its
// vectors' elements, so an address can be reused by another module's hook after a register / unregister.
void AddRegistryHookTime(const std::string& hookName, uint64_t ticks) {
    const char* p = hookName.c_str();
    RegPtr* cached = nullptr;
    for (auto& e : g_regPtrs) {
        if (e.ptr == p) {
            if (g_regNames[e.idx].name == hookName) {
                g_regNames[e.idx].ticks += ticks;
                g_regNames[e.idx].calls++;
                return;
            }
            cached = &e;
            break;
        }
    }
    int idx = -1;
    for (size_t i = 0; i < g_regNames.size(); i++)
        if (g_regNames[i].name == hookName) idx = static_cast<int>(i);
    if (idx < 0) {
        if (g_regNames.size() >= 64) return;
        g_regNames.push_back({hookName, 0, 0});
        idx = static_cast<int>(g_regNames.size()) - 1;
    }
    if (cached) cached->idx = idx;
    else if (g_regPtrs.size() < 256) g_regPtrs.push_back({p, idx});
    g_regNames[idx].ticks += ticks;
    g_regNames[idx].calls++;
}

} // namespace FrameProfiler
#else
// Developer tools left out of this build: the profiler does nothing
#include "frame_profiler.h"
#include <imgui.h>
#include <intrin.h>
namespace FrameProfiler {
void SetEnabled(bool) {}
bool IsEnabled() { return false; }
void RenderUI(bool) { ImGui::TextDisabled("Not in this build"); }
void SaveToToml(toml::table&) {}
void LoadFromToml(const toml::table&) {}
void Shutdown() {}
bool RegistryHookTimingActive() { return false; }
uint64_t Ticks() { return __rdtsc(); }
void AddRegistryHookTime(const std::string&, uint64_t) {}
bool PresentHookTimingActive() { return false; }
bool ModTimeActive() { return false; }
int BeginModTime(ModTime, const void*) { return -1; }
int BeginModTimeSampled(ModTime, const void*, uint32_t) { return -1; }
void EndModTime(int, const void*) {}
} // namespace FrameProfiler
#endif
