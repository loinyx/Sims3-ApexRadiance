// Night Terrain Relight
//
// Problem 1: street-lamp light stops in a straight line at lot borders when the save was loaded during the day.
// Problem 2: lamps placed on a lot (outdoors) never light the world grass just outside the lot.
//
// Reverse-engineering notes (Steam 1.67.2.024037):
//  - The world terrain chunks are relit by FUN_00c845c0 (per frame, render thread). It sets byte +0x55 on EVERY chunk
//    (which rebuilds the chunk and its light textures through FUN_00c834f0 / FUN_00c83060 / FUN_00c7fa70 /
//    FUN_00c7e7a0 / FUN_00c25a90) when the world-light counter of the light cells reaches 0 AND it is night
//    (lightMgr+0xF0 > 0.99, FUN_006ac560) or the game is in edit mode.
//      cells = *(lightMgr + 0x104), lightMgr = *(*(0x011D1860) + 0x1C0)
//      +0x38 / +0x3C: countdowns set to 50 by FUN_006b64b0 (register), FUN_006b6090 (unregister), FUN_006b6590
//      (move / toggle), decremented to 0 each frame by FUN_006b5da0, reset to -1 by FUN_006b5770 when consumed.
//  - Picking up a street lamp in Build mode arms that counter (verified with a call trace: 128 chunk rebuilds and 243
//    light texture rebuilds follow). That is why it fixes the lighting. The night level setter FUN_006add60 switches
//    the lamps on at dusk but never arms the counter, so a save loaded by day keeps the "lamps off" terrain light.
//  - Only lights whose vfunc+0x20 returns 1 (class 0xFF42F8, type 0xB "world light") can arm the counter or enter the
//    terrain light bake: the visitor at 0xC29620 (vtable 0x010768A0) filters with the same vfunc. Ordinary lamps on a
//    lot (types 3..6) never reach the world terrain, hence the hard edge around lots.
//
// Fix:
//  - At dusk (level crosses 0.99 upwards) arm both countdowns, exactly like a lamp pick-up does. Also a button.
//  - Optional: outdoor lot lamps (lot id != 0, type 3..6, alive, enabled, room known and room 0) arm the counter like
//    world lights (3 call sites) and, when switched on, are accepted by the terrain light bake (visitor).

#include "patch_base.h"
#include "apex_version.h"
#include "memory_patch.h"
#include "game_addresses.h"
#include "apex_log.h"
#include "d3d9_hooks.h"
#include "depth_share.h"
#include "light_diag.h"
#include "recorder.h"
#include "hotkeys.h"
#include "light_probe.h"
#include "lot_light_bridge.h"
#include "lightmap_smooth.h"
#include "terrain_chunk_relight.h"
#include "terrain_lighting_policy.h"
#include "render_callbacks.h"
#include "object_light_bridge.h"
#include "level_light_share.h"
#include "room_ambient_policy.h"
#include "unlit_rooms.h"
#include "lamp_mark_filter.h"
#include "rig_tracker.h"
#include "room_map_padding.h"
#include "build_flavor.h"
#include "night_lighting.h"
#include "lot_lighting_motion.h"
#include "imgui.h"
#include "ui/widgets.h"
#include "ui/i18n.h"
#include "overlay.h"
#include <windows.h>
#include <algorithm>
#include <map>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstring>
#include <format>
#include <string>
#include <vector>

void RequestRefreshAfterLoad(); // below the patch's helpers: every room and lot once more, a moment after a load

namespace {

using Clock = std::chrono::steady_clock;

// ---- addresses: Steam 1.67.2.024037 (in the comments) or found by signature on other builds (game_addresses.h); set by
// LoadAddresses (Install), all validated byte for byte before use ----
uintptr_t kRootGetter = 0; // 0x006E97B0: A1 <imm32 = &root> 85 C0 75 01 C3 8B 80 C0 01 00 00
constexpr BYTE kRootGetterBytes[] = {0xA1, 0, 0, 0, 0, 0x85, 0xC0, 0x75, 0x01, 0xC3, 0x8B, 0x80, 0xC0, 0x01, 0x00, 0x00};

uintptr_t kVisitorSite = 0; // 0x00C29626: inside the "Terrain/Lights" collector visitor (0xC29620)
const std::vector<BYTE> kVisitorOrig = {0x8B, 0x07, 0x8B, 0x50, 0x20, 0x8B, 0xF1, 0x8B, 0xCF, 0xFF, 0xD2};
// mov eax,[edi]; mov edx,[eax+20h]; mov esi,ecx; mov ecx,edi; call edx      (followed by test al,al; jz)

struct ArmSite {
    uintptr_t addr;
    const char* name;
};
ArmSite kArmSites[] = {
    {0, "light registration (0x6B64B0)"},   // 0x006B6516
    {0, "light removal (0x6B6090)"},        // 0x006B60D3
    {0, "light moved/toggled (0x6B6590)"}, // 0x006B6618
};
const std::vector<BYTE> kArmOrig = {0x8B, 0x17, 0x8B, 0x42, 0x20, 0x8B, 0xCF, 0xFF, 0xD0};
// mov edx,[edi]; mov eax,[edx+20h]; mov ecx,edi; call eax      (followed by test al,al; jz; mov [esi+38h],32h)

// Apex's own kicks only (the game keeps writing 50 at its three arm sites): every Apex kick is already debounced (lamp
// edits 250 ms, dusk atrasoSegundos, load: world drawn + steady night level), so the game's extra 50-frame wait (~0.8 s)
// only delayed the result. 3 frames still lets the game's own per-frame decrement (0x006B5DA0) run before the consume
// in the terrain update (0x00C84C1B), exactly as with 50.
constexpr int kArmFrames = 3;

// The terrain update's per-chunk loop re-renders ONE chunk's composited textures per call (chunk+0x54 set):
// 0x00C85041 cmp byte [esi+54h],0; jz; 0x00C85047 push 0; push esi; mov ecx,edi; call 0x00C7E7A0 (at 0x00C8504C).
// FUN_00C7E7A0 = void __thiscall(terrain, chunk, char force), RET 8 (re/out/dump/asm/00c7e7a0.asm line 163); its full
// render path ends with "mov byte [esi+54h],0" (0x00C7E978), the only write of +0x54 in it. Redirecting this one CALL
// (the other callers 0xC8088E / 0xC8307E are left alone) tells the smoothed maps exactly which chunk map the game just
// re-rendered: chunk+0x0C / +0x10 = chunk x / z in world units, >> 8 = grid index (docs/engine/terrain-and-light-bake.md
// 3.3), the same index as the smoothed maps' key (centre = 256 i + 128).
uintptr_t kChunkRenderCall = 0;      // 0x00C8504C
uintptr_t kChunkRenderContextAt = 0; // 0x00C85047 (kChunkRenderCall - 5)
std::vector<BYTE> kChunkRenderContext; // 6A 00 56 8B CF E8 <rel32 to kChunkRenderFn> C6 44 24 0C 01 (Steam: rel32 4F 97 FF FF)
std::vector<BYTE> kChunkRenderOrig;    // E8 <rel32>
uintptr_t kChunkRenderFn = 0;        // 0x00C7E7A0
using ChunkRender_t = void(__thiscall*)(void* terrain, void* chunk, char force);
std::atomic<int> g_chunkRenders{0};
bool g_chunkHookInstalled = false;

bool ReadRenderedChunk(const BYTE* chunk, int& ix, int& iz) {
    __try {
        if (chunk[0x54] != 0) return false; // this call did not re-render (early return)
        ix = *reinterpret_cast<const int*>(chunk + 0x0C) >> 8;
        iz = *reinterpret_cast<const int*>(chunk + 0x10) >> 8;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// __fastcall with two stack arguments = __thiscall(terrain, chunk, force) for the caller (ECX = terrain, callee pops 8).
// Also the completion signal (and QPC timing) of the chunks the local terrain relight releases (ChunkRelight): this is
// the game's one-chunk-per-update sweep branch they go through (chunk+0x54).
void __fastcall ChunkRenderThunk(void* terrain, void* /*edx*/, BYTE* chunk, int force) {
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    reinterpret_cast<ChunkRender_t>(kChunkRenderFn)(terrain, chunk, static_cast<char>(force));
    QueryPerformanceCounter(&t1);
    int ix = 0, iz = 0;
    const bool rendered = ReadRenderedChunk(chunk, ix, iz);
    if (rendered) {
        g_chunkRenders.fetch_add(1, std::memory_order_relaxed);
        LightmapSmooth::NoteChunkRendered(ix, iz);
    }
    ChunkRelight::OnChunkRendered(terrain, chunk, rendered, t1.QuadPart - t0.QuadPart);
}

// Lot room light solve: FUN_006be020 (street-lamp class contribution, vfunc+0x4C) reads the lamp's effective colour
// +0xE0, which is zero while the lamp is off. Lot grass samples ONLY the lot's room-0 LightMap, so a lot solved by day
// never gets the street lamps. We replace "movaps xmm0,[esi+0E0h]" with a call that uses colour(+0xF0) x
// intensity(+0x10) for street lamps (type 0xB, raw lot id 0) that are off.
uintptr_t kLampColourSite = 0; // 0x006BE18C
const std::vector<BYTE> kLampColourOrig = {0x0F, 0x28, 0x86, 0xE0, 0x00, 0x00, 0x00};

// Room queue used when a lamp changes (Build-mode pick-up path): FUN_006c7160 thiscall(treeLevel, roomId), ret 4.
// The light update tree then invalidates the room on all levels, re-gathers world lights and re-solves it.
uintptr_t kQueueRoom = 0; // 0x006C7160
constexpr BYTE kQueueRoomBytes[] = {0x83, 0xEC, 0x2C, 0x53, 0x55, 0x56, 0x33, 0xDB, 0x8B, 0xF1}; // Steam's prologue (checked on Steam)
using QueueRoom_t = void(__thiscall*)(void* treeLevel, int roomId);

// ---- settings ----
bool g_autoDusk = true;
bool g_lotLamps = true;
bool g_streetLampsLit = false;
bool g_relightLots = false;
bool g_bridge = true;
bool g_objLamps = true;
float g_objStrength = 1.0f;
bool g_objAll = true;
bool g_smoothMaps = true;
bool g_smoothMapsGpu = true; // developer A/B: smoothing on the GPU (default) or on the CPU worker (dev-only setting)
bool g_softLotEdges = true; // lot grass feathers to the terrain light within 3 m of the lot edge (developer A/B, dev-only setting)
float g_sidewalkClear = 0.5f;
float g_lampTint = 1.0f;
bool g_fenceGround = true;
bool g_levelShare = true;
bool g_indoorShare = true;       // indoor lamps light the story above or below through stair openings (LevelLightShare::SetIndoor)
bool g_wallAlign = true;         // walls lit where their light is drawn: no step at the floor line (LevelLightShare::SetWallAlign)
bool g_allFloors = true;        // every floor of the active lot in full lighting detail (LevelLightShare::SetAllFloors)
bool g_unlitOn = true;           // rooms with every lamp off: Apex's light instead of the game's blue glow (UnlitRooms)
float g_unlitLight = 0.35f;      // how much of the game's unlit-room light stays
float g_unlitBlue = 0.0f;        // how much of its blue tint (0 = grey)
bool g_objPixel = true;
bool g_objPixelLights = true;          // outdoor rig objects: world lamps per pixel (seamless modular pieces)
float g_objPixelLightStrength = 1.0f;
float g_fenceGroundStrength = 1.0f;
bool g_walls = true;         // outdoor walls receive baked lamp light by day and night; off keeps the native draw
float g_wallStrength = 2.0f; // multiplier of baked wall lamp RGB, independent of the enabled state
bool g_roofs = true;
float g_roofStrengthSetting = 0.6f;
bool g_water = true;
bool g_waterFilter = true, g_waterColorCompression = true;
float g_waterStrengthSetting = 0.4f;
float g_waterReflSetting = 1.0f;
// Brightness controls (Lamps and Ground tabs). Ground / roads / lot lamps on lot grass apply live in the ground shaders
// (LotLightBridge::SetGroundBrightness); street / lot lamps in the terrain light bake (BakeColourStub) need a terrain
// rebuild, done once when the slider is let go; the moonlight scales the game's sun / moon colour at night.
float g_groundBrightness = 1.0f;
float g_roadBrightness = 1.0f;  // x the ground brightness
float g_streetLampGain = 1.0f;  // street lamps on the ground (terrain bake)
float g_lotLampGain = 1.0f;     // lot lamps on the ground (terrain bake + the lot's own light map)
float g_moonlight = 1.0f;       // the moon's light at night
bool g_edgePad = true;          // pad the room light maps' edges (RoomMapPadding): no dark side on things along walls
bool g_lotTintOwn = false;      // lot lamps have their own colour
float g_lotLampTint = 1.0f;     // their colour (0 = pink, 1 = warm white)
bool g_allLotsHQ = false;
bool g_lotPassNoTerrainMap = false;
// Developer toggles (29/09, not yet tested in game; the public build never registers them: always off there):
//  - relightNearbyChunks: a lamp change re-renders only the terrain chunks under the changed lamps, one at a time through
//    the game's sweep branch (features/terrain_chunk_relight.cpp), instead of Apex's full rebuild;
//  - relightPacedSweep: Apex's own dusk and lamp-change full rebuilds become a paced sweep of every chunk, nearest to the
//    camera first (the world-load rebuild and the button stay full rebuilds).
bool g_localRelight = false;
bool g_pacedSweep = true; // test007: user-approved gradual external terrain updates

// FUN_00c7f750(chunk, lot, out) builds the per-chunk light/fog pass. For the LOT pass (lot=1, [ebp+0Ch]) it binds the
// rebuilt terrain lightmap chunk+0xD8 when it exists; that texture has no street-lamp light inside lot footprints, so
// after any full terrain rebuild lot grass loses street lamps. Before a rebuild the lot pass binds nothing and lot grass
// uses the lot LightMap (which has them). We keep the lot pass on "nothing" (0xC7F8B7) and leave the world pass alone.
uintptr_t kLotPassSite = 0; // 0x00C7F87D
const std::vector<BYTE> kLotPassOrig = {0x8B, 0x87, 0xD8, 0x00, 0x00, 0x00, 0x85, 0xC0};
std::vector<BYTE> kLotPassContext; // F3 0F 10 05 <kLotPassConst> F3 0F 11 44 24 18 74 13 (Steam: 38 A5 07 01)
uintptr_t kLotPassConst = 0;       // 0x0107A538: the float the pass stores at [esp+18h]
uintptr_t kLotPassNullBind = 0;    // 0x00C7F8B7
std::vector<BYTE> kLotPassNullBindBytes; // A1 <texture global> 6A 00 6A 00 (Steam: 80 CE 1E 01 = 0x011ECE80)
std::atomic<int> g_lotPassRedirects{0};

__declspec(naked) void LotPassStub() {
    __asm {
        mov eax, dword ptr [edi+0xD8]
        cmp byte ptr [ebp+0x0C], 0
        jne lot
        test eax, eax
        ret
    lot:
        mov eax, kLotPassConst // address of the float (0x0107A538 on Steam)
        movss xmm0, dword ptr [eax]
        movss dword ptr [esp+0x1C], xmm0
        add esp, 4
        mov eax, kLotPassNullBind // 0x00C7F8B7 on Steam
        jmp eax
    }
}
int g_stuckFrames = 0;
Clock::time_point g_lastStuckKick{};
int g_armsAtLastStuckKick = 0;
int g_lastLampEdits = 0;
int g_lastUserEdits = 0;
// Lamp changes (and the live "lot lamps on the ground" switch) are coalesced: a decision once nothing changed for 250 ms.
// 29/09 (terrain-relight.md "Lamp change decisions"; research\perf2\round3.md 5: each full rebuild is a ~240 ms frame):
//  - user-driven changes (a lamp of the bake placed, moved, removed; the switches) rebuild fast, at most one every 3 s;
//  - automatic changes (switched on / off, dimmed, recoloured; the stuck-countdown fallback) at most once per 30 s after
//    the last rebuild of any kind;
//  - both only if the lamps the bake takes differ from the snapshot of the last rebuild (LotLightBridge::DiffBake), so a
//    rebuild the game made after the change (or any other rebuild) drops the kick, and the same state never rebuilds
//    twice;
//  - both wait until the camera has been still for 1 s.
bool g_editKickPending = false;
bool g_worldRigRefreshPending = false;
bool g_editUser = false;  // the pending change includes a user-driven one (fast path)
bool g_editForce = false; // a switch changed what the bake takes: no snapshot compare
Clock::time_point g_editFirstAt{}, g_editLastAt{};
Clock::time_point g_lastEditKick{};
std::string g_editReason = "lot lamps changed";
std::string g_lastEditOutcome = "none";
constexpr auto kEditQuiet = std::chrono::milliseconds(250);
constexpr auto kEditMinInterval = std::chrono::seconds(3);
constexpr auto kAutoMinInterval = std::chrono::seconds(5);   // automatic lamp changes: after the last rebuild of any kind (was 30 s: lamps switched by Sims stayed on the ground too long; flickering lamps are "animated" and never rebuild)
constexpr auto kAutoBusyInterval = std::chrono::seconds(30); // ... once two automatic rebuilds ran within the last minute
constexpr auto kCameraStill = std::chrono::seconds(1);
constexpr auto kCameraWaitMax = std::chrono::seconds(2); // a lamp change waits at most this long for the camera to stop (user, 30/09: the square's lamps took ~16 s to reach the ground while panning)
constexpr auto kGameRebuiltSlack = std::chrono::seconds(2); // a rebuild up to 2 s before a change was SEEN already had it
constexpr auto kSnapshotWaitMax = std::chrono::seconds(2);
enum class EditWait { None, Snapshot, Camera, Interval, Rate, Relight, LampRate };
EditWait g_editWait = EditWait::None;
// Lamps the bake took at the last rebuild (the first light enumeration after it was consumed)
LotLightBridge::BakeSnapshot g_baked;
bool g_haveBaked = false;
bool g_bakedDue = false;
int g_bakedDueEnum = 0;
Clock::time_point g_bakedDueAt{}, g_bakedAt{};
Clock::time_point g_lastRebuildAt{};
LotLightBridge::BakeDiff g_editDiff;
int g_editDiffEnum = -1;
std::vector<uint64_t> g_editUserLots; // lots of the user-driven changes of the pending batch
// decisions (developer status)
int g_decRebuiltUser = 0, g_decRebuiltAuto = 0, g_decSkipGame = 0, g_decSkipSame = 0, g_decCovered = 0, g_decDeferCamera = 0, g_decDeferRate = 0;

// Local terrain relight (relightNearbyChunks; terrain-relight.md "Local terrain relight"). A lamp change queues the chunks
// under the changed lamps' old and new rects (ChunkRelight); the batch remembers its changes and the lamps of their lots
// as they were when it was decided, and when every chunk of it was re-rendered the changed lamps take that state in
// g_baked (LotLightBridge::CoverLots). Changes on a lot the last rebuild did not have always take the full path. While a
// batch is queued the next lamp change waits for it (then it is compared with the updated snapshot). A consumed rebuild
// drops the batches (its own snapshot covers everything).
struct LocalBatch {
    int id = 0;
    std::vector<LotLightBridge::BakeChange> changes;
    std::vector<LotLightBridge::BakeLamp> lamps; // the lamps of the changes' lots when it was decided
    std::string what;
};
std::vector<LocalBatch> g_localBatches;
// Automatic changes (switched, dimmed, recoloured) relight the same lamp at most once per 5 s (the full rebuild's 30 s
// rule was sized for ~240 ms frames); user-driven changes are relit at once.
struct RelitLamp {
    uint64_t lot = 0;
    int type = 0;
    float pos[3] = {};
    Clock::time_point at{};
};
std::vector<RelitLamp> g_relitLamps;
constexpr auto kLocalAutoPerLamp = std::chrono::seconds(5);
int g_decLocalUser = 0, g_decLocalAuto = 0, g_decLocalRefused = 0, g_localDone = 0, g_sweepsStarted = 0, g_sweepsDone = 0, g_localFailures = 0;
std::string g_lastLocal = "none", g_lastLocalRefusal = "none";
int g_sweepId = 0;
std::string g_sweepReason;
Clock::time_point g_sweepAt{};
struct LotArrival { Clock::time_point first{}, last{}, retry{}; };
std::map<uint64_t, LotArrival> g_arrivals;
std::map<uint64_t, Clock::time_point> g_arrivalRelit;

// Camera eye [[root]+camera]+eye, the read WorldManager::Update does (0x00C6D5BD..0x00C6D5C9 on Steam; offsets parsed from
// the code: root getter "A1 imm32 C3", camera getter "8B 41 disp8 C3", "0F 28 40 disp8" after the getter's call;
// docs/engine/camera-and-map-view.md). Sampled every Present; moving = the eye went more than 2 cm from where it was
// when it last counted as moving (so a slow orbit or zoom adds up). Unknown (not found, unreadable) = still.
uintptr_t g_camRootGlobal = 0;
uint32_t g_camOff = 0, g_eyeOff = 0;
bool g_camOk = false, g_camHave = false;
float g_camRef[3] = {};
Clock::time_point g_camMovedAt{};
constexpr float kCameraMoveM = 0.02f;

// Lot lighting quality: FUN_00adb5a0 and FUN_00adb850 pass (lot is active || Build mode) to FUN_006a5ef0. The default
// "mov byte [esp+0Ch],0" becomes 1 so every lot is lit at the high quality the active lot uses.
uintptr_t kQualitySites[2] = {}; // 0x00ADB66B, 0x00ADB884
const std::vector<BYTE> kQualityOrig = {0xC6, 0x44, 0x24, 0x0C, 0x00};
const std::vector<BYTE> kQualityNew = {0xC6, 0x44, 0x24, 0x0C, 0x01};
float g_delaySec = 2.0f;

volatile LONG g_forcedLampUses = 0;
std::atomic<int> g_lotRelights{0};
std::atomic<int> g_roomsQueued{0};
std::atomic<bool> g_relightLotsRequested{false};
bool g_lotRelightPending = false;
bool g_loadKickPending = false;
Clock::time_point g_lotRelightAt{};
// World load: the rebuild waits until the world is live (its terrain is drawn) and the night level has been steady for
// 1 s, so it never runs during the loading screen with the night level still at 0 (lamps off).
Clock::time_point g_worldAt{}, g_liveAt{}, g_levelRefAt{};
bool g_live = false;
std::string g_liveSignal = "none";
std::string g_loadInfo = "none";
float g_levelRef = -1.0f;
constexpr auto kLiveSettle = std::chrono::seconds(1);   // world drawn for this long
constexpr auto kLevelSteady = std::chrono::seconds(1);  // night level within 0.02 for this long
constexpr auto kLevelWaitMax = std::chrono::seconds(20); // after the world is live: rebuild even if the level keeps moving
constexpr auto kLiveFallback = std::chrono::seconds(30); // no draw seen (street lamps on lots off): assume live
std::string g_lastLotRelight = "none";

// Replaces movaps xmm0,[esi+0E0h] at 0x6BE18C (ESI = light). May clobber EAX, XMM2 and flags (dead at that point).
__declspec(naked) void StreetLampColourStub() {
    __asm {
        test byte ptr [esi+0x100], 0x20
        jnz lit
        cmp dword ptr [esi+0xB0], 0x0B
        jne lit
        mov eax, dword ptr [esi+0xC0]
        or eax, dword ptr [esi+0xC4]
        jnz lit
        lock inc dword ptr [g_forcedLampUses]
        movups xmm0, xmmword ptr [esi+0xF0]
        mov eax, dword ptr [esi+0x10]
        test eax, 0x7FFFFFFF
        jz done
        movups xmm2, xmmword ptr [esi+0x10]
        mulps xmm0, xmm2
    done:
        ret
    lit:
        movaps xmm0, xmmword ptr [esi+0xE0]
        ret
    }
}

// "Street lamps" / "Lot lamps" on the ground: the terrain bake (FUN_00c292b0) copies each lamp's colour into its shader
// parameter at 0xC2950F ("movaps xmm0,[edi+0F0h]; movaps [esi+120h],xmm0", EDI = the light). The stub loads it times the
// multiplier of the lamp's kind (a world street lamp: lot id +0xC0/+0xC4 = 0; else a lot lamp), so only the bake sees it:
// the lamp object, its rig, the room solves and the lamp change tracking keep the real colour. EAX and the flags are dead
// there (EAX is written at 0xC29523); .w is overwritten later (0xC29586). Both arrays: 16-byte aligned for mulps.
alignas(16) float g_bakeStreetMul[4] = {1.0f, 1.0f, 1.0f, 1.0f};
alignas(16) float g_bakeLotMul[4] = {1.0f, 1.0f, 1.0f, 1.0f};
uintptr_t kBakeColourSite = 0; // 0x00C2950F
const std::vector<BYTE> kBakeColourOrig = {0x0F, 0x28, 0x87, 0xF0, 0x00, 0x00, 0x00};
bool g_bakeGainInstalled = false;
float g_bakeGainSeen[2] = {1.0f, 1.0f}; // street / lot gains the multipliers hold
float g_tintSeen[2] = {1.0f, 1.0f};     // street / lot lamp colours the stock lamps were last re-coloured with

__declspec(naked) void BakeColourStub() {
    __asm {
        movaps xmm0, xmmword ptr [edi+0xF0]
        mov eax, dword ptr [edi+0xC0]
        or eax, dword ptr [edi+0xC4]
        jnz lot
        mulps xmm0, xmmword ptr [g_bakeStreetMul]
        ret
    lot:
        mulps xmm0, xmmword ptr [g_bakeLotMul]
        ret
    }
}

// A menu slider is being dragged right now (then the values that cost a rebuild wait for its release). Safe from the
// Present hook: false without an ImGui context or with the menu hidden (its frame count stops while it is hidden).
bool MenuSliderHeld() { return ImGui::GetCurrentContext() && Overlay::IsVisible() && ApexUi::SliderDragging(); }

void SetBakeGains(float street, float lot) {
    for (int i = 0; i < 3; i++) {
        g_bakeStreetMul[i] = street;
        g_bakeLotMul[i] = lot;
    }
    g_bakeGainSeen[0] = street;
    g_bakeGainSeen[1] = lot;
}

// "Moonlight": the game multiplies the sun / moon colour by the "Sunlight Scale" float every frame (FUN_00c11ad0,
// "mov ecx,011D0918h; call getter; movss xmm0,[eax]" at 0x00C11B01, its only reader) before it hands it to the light
// manager (+0x130 = ExteriorLightData, read by terrain, roads, walls, floors, roofs, objects, Sims and ponds). Written
// as its original value x lerp(1, moonlight, night level): by day it is the game's. Plain data, no code patched.
uintptr_t kSunlightScale = 0; // 0x011D0918
float g_sunlightBase = -1.0f;  // its value when first seen (< 0 = not read yet / not usable)
float g_moonWritten = -1.0f;
float g_moonSeen = 1.0f;       // moonlight value the object rigs gathered with

bool ReadFloat(uintptr_t at, float& v) {
    __try {
        v = *reinterpret_cast<const float*>(at);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool WriteFloat(uintptr_t at, float v) {
    __try {
        *reinterpret_cast<float*>(at) = v;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Every frame (render thread): the scale for the current night level. False when not available.
bool ApplyMoonlight(float level) {
    if (!kSunlightScale) return false;
    if (g_sunlightBase < 0.0f) {
        float v = 0.0f;
        if (!ReadFloat(kSunlightScale, v) || !(v > 0.0f && v < 100.0f)) {
            kSunlightScale = 0; // not the float we expect: leave it alone for this session
            LOG_WARNING("[NightTerrainRelight] Moonlight: the sunlight scale is not a usable value; moonlight control off");
            return false;
        }
        g_sunlightBase = v;
        LOG_INFO(std::format("[NightTerrainRelight] Moonlight: sunlight scale {:.3f} at {:#x}", v, kSunlightScale));
    }
    // Someone else wrote it since (Sims3SettingsSetter's "Sunlight brightness" edits the same variable): that is the new
    // base, so the moonlight scales the user's own choice and never stacks on itself
    float now = 0.0f;
    if (!ReadFloat(kSunlightScale, now)) return false;
    if (g_moonWritten >= 0.0f && now != g_moonWritten && now > 0.0f && now < 100.0f) {
        g_sunlightBase = now;
        LOG_INFO(std::format("[NightTerrainRelight] Moonlight: sunlight scale changed elsewhere to {:.3f} (new base)", now));
    }
    const float want = g_sunlightBase * (1.0f + (g_moonlight - 1.0f) * level);
    if (want == now) {
        g_moonWritten = want;
        return true;
    }
    if (!WriteFloat(kSunlightScale, want)) return false;
    g_moonWritten = want;
    return true;
}

void RestoreMoonlight() {
    if (kSunlightScale && g_sunlightBase >= 0.0f && g_moonWritten >= 0.0f) WriteFloat(kSunlightScale, g_sunlightBase);
    g_moonWritten = -1.0f;
}

// Queues room 0 of every loaded lot level, like picking up a lamp does. Returns rooms queued, -1 on failure.
int QueueAllLotOutdoorRooms(uintptr_t lightMgr) {
    int queued = 0;
    __try {
        const uintptr_t tree = *reinterpret_cast<const uintptr_t*>(lightMgr + 0xD4);
        if (!tree) return -1;
        auto* buckets = *reinterpret_cast<uintptr_t**>(tree + 0x58);
        const uint32_t bucketCount = *reinterpret_cast<const uint32_t*>(tree + 0x5C);
        if (!buckets || bucketCount == 0 || bucketCount > (1u << 20)) return -1;
        const uintptr_t endNode = buckets[bucketCount];
        uintptr_t* slot = buckets;
        uintptr_t node = *slot;
        int guard = 0;
        while (node == 0 && guard++ < (1 << 20)) node = *++slot;
        guard = 0;
        while (node != endNode && guard++ < 100000) {
            const uintptr_t tracker = *reinterpret_cast<const uintptr_t*>(node + 8);
            if (tracker) {
                for (int level = -4; level <= 7; level++) {
                    const uintptr_t treeLevel = tracker + 0x6A0 + static_cast<intptr_t>(level) * 0x1A4;
                    const uintptr_t manager = *reinterpret_cast<const uintptr_t*>(treeLevel);
                    if (!manager || *reinterpret_cast<const uintptr_t*>(manager) != lightMgr) continue;
                    reinterpret_cast<QueueRoom_t>(kQueueRoom)(reinterpret_cast<void*>(treeLevel), 0);
                    queued++;
                }
            }
            node = *reinterpret_cast<const uintptr_t*>(node + 0x10);
            int g2 = 0;
            while (node == 0 && g2++ < (1 << 20)) node = *++slot;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return -1;
    }
    return queued;
}

// ---- runtime state (render thread) ----
uintptr_t g_rootPtrAddr = 0;
uintptr_t g_lastCells = 0;
bool g_lastNight = false;
TerrainLightingPolicy::Cycle g_cycle;
bool& g_scheduled = g_cycle.pending;
int g_prevCounter = INT_MIN;
std::string g_pendingReason;
bool g_pendingDusk = false; // the armed rebuild is (also) the dusk rebuild
bool g_pendingLoad = false; // the armed rebuild is the world load's (29/09: no lot relight after it, see below)
Clock::time_point g_kickAt{};
std::string g_lastTiming = "none";
int g_crossUp = 0, g_crossDown = 0;
Clock::time_point g_lastCrossAt{};
bool g_lotLampsSeen = true; // g_lotLamps as OnPresent last saw it (the switch applies live: one rebuild at night)
std::atomic<bool> g_kickRequested{false};

std::atomic<int> g_kicks{0};
std::atomic<int> g_rebuilds{0};
std::atomic<int> g_lotLampArms{0};
std::atomic<int> g_lotLampsBaked{0};
std::atomic<int> g_lotLampsSkippedOff{0};
float g_level = 0.0f;
std::atomic<float> g_menuLevel{-1.0f}; // g_level for the menu's status pill; -1 = no world loaded or the feature is off
int g_counter38 = 0;
int g_counter3C = 0;
std::string g_status = "Waiting for the game to load a world";
std::string g_lastEvent = "none";

// ---- light predicates (called from game code, must be __stdcall and preserve ebx/esi/edi/ebp) ----
using BoolVfn = bool(__thiscall*)(void*);

bool OriginalWorldLightTest(void* light) {
    auto vtable = *reinterpret_cast<uintptr_t**>(light);
    return reinterpret_cast<BoolVfn>(vtable[0x20 / 4])(light);
}

// Lamp that belongs to a lot, is an ordinary lamp type, is alive/enabled and sits outdoors (room 0).
bool IsOutdoorLotLamp(const BYTE* L) {
    __try {
        const uint32_t lotLo = *reinterpret_cast<const uint32_t*>(L + 0xC0);
        const uint32_t lotHi = *reinterpret_cast<const uint32_t*>(L + 0xC4);
        if ((lotLo | lotHi) == 0) return false;
        const int type = *reinterpret_cast<const int*>(L + 0xB0);
        if (type < 3 || type > 6) return false;
        const BYTE f = L[0x100];
        if (!(f & 0x01) || !(f & 0x40) || !(f & 0x04)) return false; // alive, enabled, room known
        return *reinterpret_cast<const int*>(L + 0x08) == 0;           // room 0 = outdoors
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

// Replaces the vfunc+0x20 test where the light cells arm the terrain relight countdown.
bool __stdcall ArmTest(BYTE* light) {
    if (OriginalWorldLightTest(light)) return true;
    if (!g_lotLamps || !IsOutdoorLotLamp(light)) return false;
    g_lotLampArms.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// Replaces the vfunc+0x20 test in the terrain light bake collector. Lot lamps only while switched on.
bool __stdcall TerrainLightTest(BYTE* light) {
    // Type-11 vfunc+0x20 is FUN_007EAEA0 (always true). For a lot-owned outdoor
    // lamp, respect the observed enabled/lit state before accepting that original test.
    // World-owned lights and other classes keep their original behavior.
    if (*reinterpret_cast<const int*>(light + 0xB0) == 11 &&
        (*reinterpret_cast<const uint32_t*>(light + 0xC0) | *reinterpret_cast<const uint32_t*>(light + 0xC4)) != 0 &&
        (light[0x100] & 0x04) && *reinterpret_cast<const int*>(light + 0x08) == 0 &&
        (light[0x100] & 0x61) != 0x61) return false;
    if (OriginalWorldLightTest(light)) return true;
    if (!g_lotLamps || !IsOutdoorLotLamp(light)) return false;
    if (!(light[0x100] & 0x20)) {
        g_lotLampsSkippedOff.fetch_add(1, std::memory_order_relaxed);
        return false;
    }
    g_lotLampsBaked.fetch_add(1, std::memory_order_relaxed);
    return true;
}

// ---- game state access ----
struct LightState {
    uintptr_t lightMgr = 0;
    uintptr_t cells = 0;
    float level = 0.0f;
};

bool ReadLightState(LightState& out) {
    if (!g_rootPtrAddr) return false;
    __try {
        const uintptr_t root = *reinterpret_cast<const uintptr_t*>(g_rootPtrAddr);
        if (!root) return false;
        const uintptr_t lightMgr = *reinterpret_cast<const uintptr_t*>(root + 0x1C0);
        if (!lightMgr) return false;
        const uintptr_t cells = *reinterpret_cast<const uintptr_t*>(lightMgr + 0x104);
        if (!cells) return false;
        out.lightMgr = lightMgr;
        out.cells = cells;
        out.level = *reinterpret_cast<const float*>(lightMgr + 0xF0);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ReadCounters(uintptr_t cells, int& c38, int& c3C) {
    __try {
        c38 = *reinterpret_cast<const int*>(cells + 0x38);
        c3C = *reinterpret_cast<const int*>(cells + 0x3C);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool ArmCounters(uintptr_t cells) {
    __try {
        *reinterpret_cast<int*>(cells + 0x38) = kArmFrames;
        *reinterpret_cast<int*>(cells + 0x3C) = kArmFrames;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

std::string LevelText(float level) {
    return std::format("night level {:.2f}", level);
}

double MsSince(Clock::time_point t) { return std::chrono::duration<double, std::milli>(Clock::now() - t).count(); }

void Kick(uintptr_t cells, float level, const std::string& reason, bool dusk = false) {
    if (!ArmCounters(cells)) {
        g_lastEvent = std::format("Could not arm the rebuild ({})", reason);
        return;
    }
    g_kicks.fetch_add(1);
    g_pendingReason = reason;
    g_pendingDusk = dusk;
    g_pendingLoad = false;
    g_kickAt = Clock::now();
    g_prevCounter = kArmFrames;
    LightmapSmooth::NoteKick(reason.c_str());
    g_lastEvent = std::format("Rebuild armed: {} ({})", reason, LevelText(level));
    LOG_INFO("[NightTerrainRelight] " + g_lastEvent);
}

// ---- camera still (lamp change kicks wait for it) ----
bool ReadCode(uintptr_t at, void* out, size_t n) {
    if (!at) return false;
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(at), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

void ResolveCamera() {
    g_camOk = false;
    g_camHave = false;
    BYTE r[6] = {}, g[4] = {}, e[4] = {};
    const uintptr_t call = GameAddr::Get(GameAddr::Id::CameraGetterCall);
    if (!ReadCode(GameAddr::Get(GameAddr::Id::CameraRootGetter), r, sizeof r) || r[0] != 0xA1 || r[5] != 0xC3) return;
    if (!ReadCode(GameAddr::Get(GameAddr::Id::CameraGetter), g, sizeof g) || g[0] != 0x8B || g[1] != 0x41 || g[3] != 0xC3) return;
    if (!call || !ReadCode(call + 5, e, sizeof e) || e[0] != 0x0F || e[1] != 0x28 || e[2] != 0x40) return;
    uint32_t root = 0;
    std::memcpy(&root, r + 1, 4);
    g_camRootGlobal = root;
    g_camOff = g[2];
    g_eyeOff = e[3];
    g_camOk = root != 0;
}

bool ReadEye(float out[3]) {
    __try {
        const uintptr_t root = *reinterpret_cast<const uintptr_t*>(g_camRootGlobal);
        if (!root) return false;
        const uintptr_t camera = *reinterpret_cast<const uintptr_t*>(root + g_camOff);
        if (!camera) return false;
        const float* p = reinterpret_cast<const float*>(camera + g_eyeOff);
        out[0] = p[0];
        out[1] = p[1];
        out[2] = p[2];
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
}

void SampleCamera(Clock::time_point now) {
    float e[3];
    if (!g_camOk || !ReadEye(e)) {
        g_camHave = false;
        return;
    }
    if (!g_camHave) { // first sample (or readable again): the reference, not a move
        std::memcpy(g_camRef, e, sizeof e);
        g_camHave = true;
        return;
    }
    const float dx = e[0] - g_camRef[0], dy = e[1] - g_camRef[1], dz = e[2] - g_camRef[2];
    if (dx * dx + dy * dy + dz * dz > kCameraMoveM * kCameraMoveM) {
        std::memcpy(g_camRef, e, sizeof e);
        g_camMovedAt = now;
    }
}

bool CameraStill(Clock::time_point now) { return !g_camOk || !g_camHave || now - g_camMovedAt >= kCameraStill; }
// A pending lamp change goes ahead while the camera moves once it has waited kCameraWaitMax (one ~0.3 s rebuild then)
bool CameraAllowsEdit(Clock::time_point now) { return CameraStill(now) || now - g_editFirstAt >= kCameraWaitMax; }

std::string CameraText(Clock::time_point now) {
    if (!g_camOk) return "not found (lamp rebuilds do not wait for it)";
    if (!g_camHave) return "unreadable now (treated as still)";
    return CameraStill(now) ? "still" : "moving";
}

bool g_editLocalRefused = false; // the local path refused the pending change: the full path takes it (until a new change)

// A lamp change (or a switch) to decide on once nothing changed for kEditQuiet. user: fast path; force: no snapshot
// compare (the switch changes what the bake takes, not the lamps); backdate: decide at the next frame.
void NoteEdit(Clock::time_point now, bool user, bool force, const std::string& reason, bool backdate = false) {
    g_editLocalRefused = false; // new information: the local path may take it now
    if (!g_editKickPending) {
        g_editFirstAt = now;
        g_editUser = false;
        g_editForce = false;
        g_editWait = EditWait::None;
        g_editUserLots.clear();
        g_worldRigRefreshPending = false;
    }
    g_editKickPending = true;
    g_editLastAt = backdate ? now - kEditQuiet : now;
    // the reason shown is the strongest one: switch > user-driven > automatic
    if (force || (user && !g_editForce) || (!g_editUser && !g_editForce)) g_editReason = reason;
    g_editUser |= user;
    g_editForce |= force;
}

void RefreshWorldRigs(const char* reason) {
    if (!g_worldRigRefreshPending) return;
    g_worldRigRefreshPending = false;
    ObjectLightBridge::RequestRigRefresh();
    if (Recorder::Verbose()) LOG_INFO(std::format("[NightTerrainRelight] Observed world lamp edit: native rig refresh requested {}", reason));
}

void FinishEdit(const std::string& outcome) {
    // Terrain completion can precede the edit debounce. It does not cover
    // native object rigs: consume that independent request before ending the edit.
    RefreshWorldRigs("before terrain edit completion");
    g_editKickPending = false;
    g_editWait = EditWait::None;
    g_lastEditOutcome = outcome;
    if (Recorder::Verbose()) LOG_INFO("[NightTerrainRelight] Lamp change: " + outcome);
}

// Deferred: logged once per state (dev build), the change stays pending and is decided again every frame.
void WaitEdit(EditWait state, const std::string& text) {
    if (g_editWait == state) return;
    g_editWait = state;
    if (state == EditWait::Camera) g_decDeferCamera++;
    if (state == EditWait::Rate || state == EditWait::Interval || state == EditWait::LampRate) g_decDeferRate++;
    if (state == EditWait::Snapshot || state == EditWait::Relight) return; // at most a few seconds, not worth a line
    g_lastEditOutcome = std::format("{}: {}", g_editReason, text);
    if (Recorder::Verbose()) LOG_INFO("[NightTerrainRelight] Lamp change: " + g_lastEditOutcome);
}

const char* EditKind() { return g_editForce ? "switch" : (g_editUser ? "user-driven" : "automatic"); }

// ---- local terrain relight and paced sweep (relightNearbyChunks / relightPacedSweep) ----

bool SameLamp(uint64_t lot, int type, const float* pos, const RelitLamp& r) {
    if (lot != r.lot || type != r.type) return false;
    const float dx = pos[0] - r.pos[0], dy = pos[1] - r.pos[1], dz = pos[2] - r.pos[2];
    return dx * dx + dy * dy + dz * dz <= 0.05f * 0.05f;
}

// The lamps of the pending change for ChunkRelight: every counted difference with its old and / or new rect. False when a
// rect does not hold its lamp's place (+-1 m): the light rect +0x134 is written by separate updaters (0x006BDE66,
// 0x006BE816, 0x006BE8AB), not verified to run in the call that moves the lamp, so a stale one is refused.
bool LocalLamps(std::vector<ChunkRelight::Lamp>& out) {
    constexpr float kSlack = 1.0f;
    auto holds = [](const float* r, const float* pos) {
        return pos[0] >= r[0] - kSlack && pos[0] <= r[2] + kSlack && pos[2] >= r[1] - kSlack && pos[2] <= r[3] + kSlack;
    };
    for (const LotLightBridge::BakeChange& c : g_editDiff.changes) {
        ChunkRelight::Lamp l;
        l.x = c.pos[0]; // position +0x120 (x, y, z): the rect is x / z
        l.z = c.pos[2];
        if (c.hasOld) std::memcpy(l.rect[l.rects++], c.oldRect, sizeof c.oldRect);
        if (c.hasNew) std::memcpy(l.rect[l.rects++], c.newRect, sizeof c.newRect);
        for (int k = 0; k < l.rects; k++)
            if (!holds(l.rect[k], c.pos)) return false;
        if (l.rects > 0) out.push_back(l);
    }
    return true;
}

// The pending change through the local relight. 1 = queued (FinishEdit called), 2 = waiting (WaitEdit called), 0 = not
// possible (`why`): the caller takes the full path with its own limits.
int TryLocal(Clock::time_point now, const std::vector<uint64_t>& newLots, const std::string& diffText, std::string& why) {
    // A lot the last rebuild did not have: its lamps may have been baked since (LOD transitions re-bake chunks with every
    // registered lamp, chunkrelight.md 1.4), but their old places are unknown, so a removed or moved lamp would leave its
    // old light on the ground.
    if (!newLots.empty()) {
        why = "a change on a lot the last rebuild did not have";
        return 0;
    }
    std::vector<ChunkRelight::Lamp> lamps;
    if (!LocalLamps(lamps)) {
        why = "a lamp's light rect does not hold its place yet";
        return 0;
    }
    if (lamps.empty()) {
        why = "no lamp rect to relight";
        return 0;
    }
    const bool user = g_editUser;
    std::erase_if(g_relitLamps, [now](const RelitLamp& r) { return now - r.at >= kLocalAutoPerLamp; });
    if (!user) { // automatic: camera still, and at most once per 5 s per lamp (user-driven changes are relit at once)
        if (!CameraAllowsEdit(now)) {
            WaitEdit(EditWait::Camera, std::format("deferred: camera moving ({})", diffText));
            return 2;
        }
        for (const LotLightBridge::BakeChange& c : g_editDiff.changes)
            for (const RelitLamp& r : g_relitLamps)
                if (SameLamp(c.lot, c.type, c.pos, r)) {
                    WaitEdit(EditWait::LampRate, std::format("rate-limited: a lamp of this change was relit locally less than {} s ago ({})", kLocalAutoPerLamp.count(), diffText));
                    return 2;
                }
    }
    std::string chunks;
    const int id = ChunkRelight::QueueLocal(lamps, why, chunks, user);
    if (!id) return 0;
    LocalBatch b;
    b.id = id;
    b.changes = g_editDiff.changes;
    b.lamps = LotLightBridge::LampsOfLots(LotLightBridge::CurrentBakeLamps(), g_editDiff.Lots());
    b.what = std::format("{} lamp{}, chunks {}", lamps.size(), lamps.size() == 1 ? "" : "s", chunks);
    for (const LotLightBridge::BakeChange& c : g_editDiff.changes) {
        RelitLamp r;
        r.lot = c.lot;
        r.type = c.type;
        std::memcpy(r.pos, c.pos, sizeof r.pos);
        r.at = now;
        g_relitLamps.push_back(r);
    }
    // the "lights changed" fallback must not fire again for the same arms
    g_lastStuckKick = now;
    g_armsAtLastStuckKick = g_lotLampArms.load();
    (user ? g_decLocalUser : g_decLocalAuto)++;
    FinishEdit(std::format("{} ({}): relit locally ({}; {})", g_editReason, EditKind(), diffText, b.what));
    g_localBatches.push_back(std::move(b));
    return 1;
}

// A newly drawn lot can register its outdoor lamps after the world's load bake.
// Refresh the union of the old and current footprints, even when the baseline
// adopted the first observed state and therefore cannot report a difference.
// Remove old entries then add the complete current set when the batch finishes.
bool ArrivalFootprint(const std::vector<LotLightBridge::BakeLamp>& old, const std::vector<LotLightBridge::BakeLamp>& current,
                      bool plain, std::vector<ChunkRelight::Lamp>& footprints, std::vector<LotLightBridge::BakeChange>& changes) {
    for (int pass = 0; pass < 2; ++pass)
        for (const auto& b : pass == 0 ? old : current) {
            if (!plain && b.type >= 3 && b.type <= 6) continue;
            const auto* r = b.rect;
            for (int k = 0; k < 4; ++k) if (!std::isfinite(r[k])) return false;
            if (!(r[0] < r[2] && r[1] < r[3])) {
                if (b.baked) return false;
                continue; // an inactive zero-range lamp has no footprint to clear
            }
            if (!(r[0] <= b.pos[0] + 1 && b.pos[0] - 1 <= r[2]
                && r[1] <= b.pos[2] + 1 && b.pos[2] - 1 <= r[3])) return false;
            ChunkRelight::Lamp l;
            l.x = b.pos[0]; l.z = b.pos[2]; l.rects = 1;
            std::memcpy(l.rect[0], r, sizeof b.rect);
            footprints.push_back(l);
            LotLightBridge::BakeChange c;
            c.lot = b.lot; c.type = b.type; c.user = true;
            std::memcpy(c.pos, b.pos, sizeof b.pos);
            c.hasOld = pass == 0; c.hasNew = pass == 1;
            std::memcpy(pass == 0 ? c.oldRect : c.newRect, r, sizeof b.rect);
            changes.push_back(c);
        }
    return !footprints.empty();
}

void RefreshArrivingLots(Clock::time_point now, bool night) {
    for (uint64_t lot : LotLightBridge::TakeLotArrivals()) {
        if (!night || !LotLightBridge::LotVisible(lot)) continue;
        if (g_arrivals.size() >= 64 && !g_arrivals.contains(lot)) continue;
        auto [it, first] = g_arrivals.try_emplace(lot);
        if (first) it->second.first = now;
        it->second.last = now;
    }
    std::erase_if(g_arrivals, [now, night](const auto& item) {
        return !night || !LotLightBridge::LotVisible(item.first) || now - item.second.first > std::chrono::seconds(8);
    });
    std::erase_if(g_arrivalRelit, [now](const auto& item) { return now - item.second > std::chrono::seconds(10); });
    // Real edits have priority. Arrival work never triggers a synchronous full
    // rebuild on refusal, and stays behind another local batch already running.
    if (!night || !g_haveBaked || g_bakedDue || g_loadKickPending || (g_editKickPending && (g_editUser || g_editForce))
        || (ChunkRelight::Busy() && g_sweepId == 0) || !ChunkRelight::LikelyAvailable()) return;
    for (auto it = g_arrivals.begin(); it != g_arrivals.end(); ++it) {
        auto& pending = it->second;
        const auto last = g_arrivalRelit.find(it->first);
        if (now < pending.retry || (last != g_arrivalRelit.end() && now - last->second < std::chrono::milliseconds(500))) continue;
        if (now - pending.last < std::chrono::milliseconds(150) && now - pending.first < std::chrono::milliseconds(500)) continue;
        const std::vector<uint64_t> lot{it->first};
        auto current = LotLightBridge::LampsOfLots(LotLightBridge::CurrentBakeLamps(), lot);
        const auto old = LotLightBridge::LampsOfLots(g_baked, lot);
        std::vector<ChunkRelight::Lamp> footprints;
        LocalBatch b;
        std::string why, chunks;
        if (!ArrivalFootprint(old, current, g_lotLamps, footprints, b.changes)) { pending.retry = now + std::chrono::milliseconds(500); continue; }
        b.id = ChunkRelight::QueueLocal(footprints, why, chunks, true);
        if (!b.id) {
            pending.retry = now + std::chrono::milliseconds(500);
            if (!kPublicBuild) LOG_INFO(std::format("[NightTerrainRelight] Visible lot {:016X}: local entry update postponed ({})", it->first, why));
            continue;
        }
        b.lamps = std::move(current);
        b.what = std::format("visible lot {:016X}, {} lamp footprints, chunks {}", it->first, footprints.size(), chunks);
        if (!kPublicBuild) LOG_INFO("[NightTerrainRelight] Entry relight queued: " + b.what);
        g_arrivalRelit[it->first] = now;
        g_localBatches.push_back(std::move(b));
        g_arrivals.erase(it);
        break; // at most one bounded arrival batch per frame
    }
}

// A paced sweep started (relightPacedSweep): like a consumed rebuild, the lamps as they are now go into the snapshot (the
// chunks are re-rendered over the next seconds, as the game's own sweep after a full rebuild is).
void StartSweep(int id, const std::string& reason, const std::string& info, Clock::time_point now) {
    g_sweepId = id;
    g_sweepReason = reason;
    g_sweepAt = now;
    g_sweepsStarted++;
    g_localBatches.clear(); // their chunks were dropped from the queue: the sweep bakes those lamps too
    g_lastRebuildAt = now;
    g_bakedDue = true;
    g_bakedDueAt = now;
    g_bakedDueEnum = LotLightBridge::LampEnumerations();
    LotLightBridge::RequestLampRefresh();
    LightmapSmooth::NoteKick(reason.c_str());
    g_lastEvent = std::format("Terrain sweep started: {} ({})", reason, info);
    LOG_INFO("[NightTerrainRelight] " + g_lastEvent);
}

// Apex's own rebuild of every chunk (dusk; a lamp change the local path did not take; a switch): with relightPacedSweep a
// paced sweep, one chunk at a time nearest to the camera first; otherwise (or when the sweep is not possible) the kick:
// every chunk baked in one frame (~240 ms), then the game's own sweep.
void RebuildAll(uintptr_t cells, float level, const std::string& reason, bool dusk, Clock::time_point now) {
    if (g_pacedSweep) {
        float eye[3] = {};
        const bool haveEye = g_camOk && ReadEye(eye);
        const float eyeXZ[2] = {eye[0], eye[2]};
        std::string why, info;
        if (const int id = ChunkRelight::QueueSweep(haveEye ? eyeXZ : nullptr, why, info, ChunkRelight::Editing())) return StartSweep(id, reason, info, now);
        LOG_INFO(std::format("[NightTerrainRelight] Paced sweep not possible ({}): {}; full rebuild instead", reason, why));
    }
    Kick(cells, level, reason, dusk);
}

bool EditReady(Clock::time_point now, Clock::time_point first, Clock::time_point last, bool priority) {
    return now - last >= (priority ? std::chrono::milliseconds(80) : kEditQuiet)
        || (priority && now - first >= std::chrono::milliseconds(500));
}

// The pending lamp change, once quiet (render thread; c38 = cells+0x38 this frame).
void DecideEdit(uintptr_t cells, float level, bool night, int c38, Clock::time_point now) {
    const bool worldLampEdit = std::find(g_editUserLots.begin(), g_editUserLots.end(), uint64_t{0}) != g_editUserLots.end();
    if (TerrainLightingPolicy::DeferDayEdit(night, g_autoDusk, g_editUser, g_editForce, worldLampEdit))
        return FinishEdit(std::format("{}: left to the dusk rebuild (day)", g_editReason));
    if (g_loadKickPending || g_scheduled)
        return FinishEdit(std::format("{}: merged into the {} rebuild", g_editReason, g_loadKickPending ? "load" :
                                     (g_cycle.target == TerrainLightingPolicy::Phase::Night ? "dusk" : "daylight")));
    // A countdown does not prove that a new user edit reached the bake. Keep
    // priority edits alive and queue their footprints rather than consuming them.
    if (!g_editUser && !g_editForce && !g_pendingReason.empty() && c38 > 0)
        return FinishEdit(std::format("{}: merged into the armed rebuild ({})", g_editReason, g_pendingReason));
    if (g_bakedDue) return WaitEdit(EditWait::Snapshot, "waiting for the snapshot of the rebuild that just ran");
    // a local relight or paced sweep in progress: its lamps go into the snapshot when it ends, then this change is
    // compared with it (so the same lamps are never queued twice)
    // QueueLocal owns fresh batch membership for an urgent queued/in-flight
    // chunk. User edits may promote their footprint ahead of arrival work too;
    // automatic edits and global switches still wait for the current batch.
    if (ChunkRelight::Busy() && !g_editUser) return WaitEdit(EditWait::Relight, "waiting for the terrain relight in progress");
    std::string diffText = "no snapshot of the last rebuild: rebuilt to be safe";
    std::vector<uint64_t> newLots; // user-driven changes on lots the last rebuild did not have (sorted, unique)
    if (g_editForce)
        diffText = "switch";
    else if (g_haveBaked) {
        if (g_editDiffEnum != LotLightBridge::LampEnumerations()) {
            g_editDiff = LotLightBridge::DiffBake(g_baked, LotLightBridge::CurrentBakeLamps(), g_lotLamps, g_editUserLots);
            g_editDiffEnum = LotLightBridge::LampEnumerations();
        }
        diffText = g_editDiff.Text();
        // DiffBake only looks at lots the last rebuild had: a lamp placed / moved / removed on a lot that streamed in
        // after it still rebuilds (Build mode on a lot visited later)
        for (uint64_t lot : g_editUserLots)
            if (!std::binary_search(g_baked.lots.begin(), g_baked.lots.end(), lot)) newLots.push_back(lot);
        std::sort(newLots.begin(), newLots.end());
        newLots.erase(std::unique(newLots.begin(), newLots.end()), newLots.end());
        const bool userLotNotBaked = !newLots.empty();
        if (userLotNotBaked) diffText += "; a user-driven change on a lot the last rebuild did not have";
        if (!g_editDiff.Any() && !userLotNotBaked) {
            // the bake already has the lamps as they are: a rebuild ran after the change (the game's own, or any other),
            // or the change went back, or it was below what the bake shows
            const bool rebuiltSince = g_lastRebuildAt != Clock::time_point{} && g_lastRebuildAt + kGameRebuiltSlack >= g_editFirstAt;
            (rebuiltSince ? g_decSkipGame : g_decSkipSame)++;
            return FinishEdit(std::format("{} ({}): skipped: {} ({})", g_editReason, EditKind(),
                                          rebuiltSince ? "the terrain was rebuilt after the change" : "no change the terrain bake uses", diffText));
        }
    }
    // Local terrain relight: only the chunks under the changed lamps (not for switches, which change every lot lamp, nor
    // without a snapshot to compare with). Refused (layout, more than 16 chunks, no rebuilt light map, ...): the full path
    // below, with its own limits, until the next change comes in.
    if ((g_localRelight || g_editUser) && g_haveBaked && !g_editForce && !g_editLocalRefused) {
        std::string why;
        if (TryLocal(now, newLots, diffText, why) != 0) return;
        g_editLocalRefused = true;
        g_decLocalRefused++;
        g_lastLocalRefusal = why;
        if (Recorder::Verbose()) LOG_INFO(std::format("[NightTerrainRelight] Lamp change: {}: local relight not possible ({}): full rebuild path", g_editReason, why));
        diffText += "; local relight not possible: " + why;
    }
    if (!CameraAllowsEdit(now)) return WaitEdit(EditWait::Camera, std::format("deferred: camera moving ({})", diffText));
    const bool fast = g_editForce || g_editUser;
    if (fast && now - g_lastEditKick < kEditMinInterval) return WaitEdit(EditWait::Interval, "rate-limited: at most one lamp rebuild every 3 s");
    // Automatic changes (lamps switched by Sims): 5 s after the last rebuild for an occasional switch, 30 s once two
    // automatic rebuilds ran within the last minute (lamps switching one by one, sensor lamps at dusk across lots: no
    // ~240 ms rebuild every 5 s; review 29/09)
    static std::vector<Clock::time_point> autoRebuilds;
    std::erase_if(autoRebuilds, [&](const Clock::time_point& t) { return now - t > std::chrono::seconds(60); });
    const auto autoInterval = autoRebuilds.size() >= 2 ? kAutoBusyInterval : kAutoMinInterval;
    if (!fast && now - g_lastRebuildAt < autoInterval)
        return WaitEdit(EditWait::Rate, std::format("rate-limited: automatic changes rebuild at most once per {} s ({} s since the last rebuild; {})",
                                                    autoInterval.count(), std::chrono::duration_cast<std::chrono::seconds>(now - g_lastRebuildAt).count(), diffText));
    if (!fast) autoRebuilds.push_back(now);
    const std::string reason = g_editReason;
    const std::string kind = EditKind();
    const int sweepsBefore = g_sweepsStarted;
    RebuildAll(cells, level, reason, false, now);
    g_lastEditKick = now;
    // the "lights changed" fallback must not rebuild again for the same arms
    g_lastStuckKick = now;
    g_armsAtLastStuckKick = g_lotLampArms.load();
    (fast ? g_decRebuiltUser : g_decRebuiltAuto)++;
    FinishEdit(std::format("{} ({}): {} ({})", reason, kind, g_sweepsStarted != sweepsBefore ? "rebuilt by a paced sweep" : "rebuilt", diffText));
}

// Runs on the render thread, the same thread as the game's light update and terrain update.
void OnPresent() {
    LightDiag::OnPresent(); // the lighting snapshot (F8 with the F-key set; Report a problem page)
    Recorder::OnPresent(); // the recording (F6): a few seconds of lighting activity, with the clock time
    LightState s;
    if (!ReadLightState(s)) {
        g_lastCells = 0;
        g_scheduled = false;
        g_prevCounter = INT_MIN;
        g_status = "Waiting for the game to load a world";
        g_menuLevel.store(-1.0f);
        return;
    }
    const auto now = Clock::now();
    const bool night = s.level > 0.99f;
    int c38 = 0, c3C = 0;
    ReadCounters(s.cells, c38, c3C);
    g_level = s.level;
    g_menuLevel.store(s.level);
    g_counter38 = c38;
    g_counter3C = c3C;

    if (s.cells != g_lastCells) { // new world
        g_lastCells = s.cells;
        g_lastNight = night;
        g_cycle.Reset(s.level);
        g_prevCounter = c38;
        g_lotRelightPending = false;
        g_pendingReason.clear();
        g_pendingDusk = false;
        g_editKickPending = false;
        g_worldRigRefreshPending = false;
        g_editWait = EditWait::None;
        g_haveBaked = false; // the new world's first rebuild (the load rebuild) takes the first snapshot
        g_bakedDue = false;
        g_baked = LotLightBridge::BakeSnapshot{};
        g_editDiffEnum = -1;
        ChunkRelight::OnWorldChanged(); // the local relight waits for this world's first rebuild again
        g_localBatches.clear();
        g_arrivals.clear();
        g_arrivalRelit.clear();
        g_relitLamps.clear();
        g_sweepId = 0;
        LOG_INFO(std::format("[NightTerrainRelight] World loaded ({}): the terrain rebuild waits until the world is drawn and the night level is steady",
                             LevelText(s.level)));
        LotLightBridge::OnWorldChanged(); // drop the previous world's chunk maps, smoothed maps and atlas
        LevelLightShare::OnWorldChanged();
        UnlitRooms::OnWorldChanged();
        // Rebuild the terrain light maps once after loading: the ones baked into the world file miss the part of a lamp's
        // light that crosses into the neighbouring 256 m chunk (straight cut on world grass at chunk borders). Not during
        // the loading screen (a kick there was consumed at the first world update with the night level still 0.00: a
        // lamps-off bake, then the dusk rebuild on top): see "world live" below.
        g_loadKickPending = true;
        g_worldAt = now;
        g_live = false;
        g_liveSignal = "none";
        g_levelRef = s.level;
        g_levelRefAt = now;
    }

    // Night level steady: no move of more than 0.02 for kLevelSteady (the level jumps 0 -> 1 when a night save starts).
    if (std::fabs(s.level - g_levelRef) > 0.02f) {
        g_levelRef = s.level;
        g_levelRefAt = now;
    }
    const bool levelSteady = now - g_levelRefAt >= kLevelSteady;

    // World live: its terrain is being drawn (the lot light bridge saw a world terrain chunk draw after the world
    // change; the loading screen draws no terrain). Without the bridge ("Street lamps light lots" off) no draw is
    // recorded: then 30 s after the world change.
    if (!g_live) {
        const char* signal = nullptr;
        // With the bridge on, its terrain draws are the signal: a load screen longer than 30 s (29/09: 54 s) must not count
        // as live, or the load rebuild runs during it at night level 0 (lamps off in the bake). 120 s covers a bridge that
        // could not start.
        if (LotLightBridge::ChunkCount() > 0) signal = "world terrain drawn";
        // 30/09: the fallback also needs a loaded lot (a session that sat 2 minutes at the main menu went "live" there: the
        // start note showed and the after-load refresh ran on 0 lots before the save was even loaded)
        else if (now - g_worldAt >= (g_bridge ? kLiveFallback * 4 : kLiveFallback) && LevelLightShare::LoadedLots() > 0)
            signal = "fallback after the world change (no terrain draw seen)";
        if (signal) {
            g_live = true;
            g_liveAt = now;
            g_liveSignal = std::format("{} after {:.1f} s", signal, MsSince(g_worldAt) / 1000.0);
            if (!kPublicBuild) LOG_INFO(std::format("[NightTerrainRelight] World live: {} ({})", g_liveSignal, LevelText(s.level)));
            GameAddr::CheckWorldStructs(); // builds other than Steam 1.67.2: once, logs whether the assumed struct offsets hold
            LevelLightShare::OnWorldLive();
            RequestRefreshAfterLoad();
        }
    }

    // The game consumed the countdown: the terrain chunks were rebuilt this frame.
    if (g_prevCounter >= 0 && c38 == -1) {
        g_rebuilds.fetch_add(1);
        const bool duskRebuild = g_pendingDusk;
        const bool loadRebuild = g_pendingLoad;
        g_pendingLoad = false;
        const bool ours = !g_pendingReason.empty();
        const std::string rebuiltBy = ours ? g_pendingReason : std::string("by the game itself");
        g_lastEvent = std::format("Terrain rebuilt ({}; {})", rebuiltBy, LevelText(s.level));
        if (ours) g_lastTiming = std::format("{}: armed -> rebuilt {:.0f} ms", g_pendingReason, MsSince(g_kickAt));
        g_pendingReason.clear();
        g_pendingDusk = false;
        LOG_INFO("[NightTerrainRelight] " + g_lastEvent + (ours ? std::format(" {:.0f} ms after it was armed", MsSince(g_kickAt)) : std::string()));
        LightmapSmooth::OnTerrainRebuilt(); // every chunk map is re-rendered over the next frames
        // Every chunk is re-rendered by this rebuild: a pending local relight or paced sweep is dropped (the snapshot
        // taken below covers its lamps). The first rebuild of a world (the load rebuild) enables the local path.
        const bool localPending = ChunkRelight::OnFullRebuild();
        if (localPending && !kPublicBuild) LOG_INFO("[NightTerrainRelight] Pending local terrain relight / paced sweep dropped: this rebuild re-renders every chunk");
        g_localBatches.clear();
        g_sweepId = 0;
        // This rebuild bakes the lamps as they are now: a lamp change still waiting is taken by it (29/09: the game's own
        // rebuilds were followed within ~1 s by an Apex kick for the same change, two ~240 ms frames in a row), and the
        // snapshot of what it baked is taken from the light enumeration forced for this frame.
        g_lastRebuildAt = now;
        if (g_editKickPending) {
            g_decCovered++;
            FinishEdit(std::format("{} ({}): skipped: {}", g_editReason, EditKind(),
                                   ours ? std::format("the {} rebuild just ran", rebuiltBy) : std::string("the game rebuilt the terrain itself")));
        }
        g_bakedDue = true;
        g_bakedDueAt = now;
        g_bakedDueEnum = LotLightBridge::LampEnumerations();
        LotLightBridge::RequestLampRefresh();
        // Relight lots ONLY after the dusk rebuild. Relighting re-registers lot lamps, which re-arms the countdown; doing it
        // after every rebuild made a loop that kept invalidating the (slow, high quality) lot solves.
        // Not after a world loaded at night: the game has just solved every lot with its night lamps (29/09: the relight 3 s later
        // re-solved 5-25 lot stories and started the first wave of room solves after the load; lamps that switch on later
        // send their own rooms).
        if (night && g_relightLots && duskRebuild && !loadRebuild) {
            g_lotRelightPending = true;
            g_lotRelightAt = now + std::chrono::seconds(3);
        }
    }
    g_prevCounter = c38;

    // Night level crossings, both directions (developer log: finds rebuilds triggered by a flickering level).
    if (night != g_lastNight) {
        (night ? g_crossUp : g_crossDown)++;
        g_lastCrossAt = now;
        if (!kPublicBuild)
            LOG_INFO(std::format("[NightTerrainRelight] Night level crossed 0.99 {} ({}; {:.1f} s after the world change; up {} / down {})", night ? "upwards" : "downwards",
                                 LevelText(s.level), MsSince(g_worldAt) / 1000.0, g_crossUp, g_crossDown));
    }
    const auto cycleMs = std::chrono::duration_cast<std::chrono::milliseconds>(now.time_since_epoch()).count();
    const auto phaseDelay = TerrainLightingPolicy::PhaseDelay(static_cast<int64_t>(g_delaySec * 1000.0f), ChunkRelight::Editing());
    g_cycle.Observe(s.level, cycleMs, phaseDelay, g_autoDusk, g_loadKickPending);
    g_lastNight = night;

    if (g_loadKickPending && g_live && now - g_liveAt >= kLiveSettle && (levelSteady || now - g_liveAt >= kLevelWaitMax)) {
        g_loadKickPending = false;
        g_scheduled = false; // a dusk rebuild waiting is covered by this one
        g_loadInfo = std::format("{}; rebuilt {:.1f} s after the world change ({})", g_liveSignal, MsSince(g_worldAt) / 1000.0, LevelText(s.level));
        Kick(s.cells, s.level, night ? "world load (night: also the dusk rebuild)" : "world load", night);
        g_pendingLoad = true;
    }
    if (g_kickRequested.exchange(false)) Kick(s.cells, s.level, "button");
    TerrainLightingPolicy::Phase cyclePhase;
    if (g_cycle.Consume(cycleMs, cyclePhase)) {
        const bool dusk = cyclePhase == TerrainLightingPolicy::Phase::Night;
        RebuildAll(s.cells, s.level, dusk ? "dusk" : "daylight transition", dusk, now);
        if (dusk) {
            if (g_relightLots) { // fallback if the terrain rebuild does not happen
                g_lotRelightPending = true;
                g_lotRelightAt = now + std::chrono::seconds(6);
            }
        }
    }

    // Outdoor lot lamps changed (edited, added or removed, e.g. in Build mode, on lots that were already loaded; see
    // LotLightBridge::TrackLotLampEdits): the game rebuilds their light on the ground only when the lot is reloaded. And
    // the "lot lamps light the street" switch applies live (the bake reads it at run time): one rebuild.
    // The snapshot of the lamps the last rebuild baked: the light enumeration forced in the frame the rebuild was consumed
    // (LotLightBridge::RequestLampRefresh). If no enumeration comes (light enumeration unavailable), no snapshot: lamp
    // changes are then decided without the compare (rate limits and camera only).
    SampleCamera(now);
    if (g_bakedDue) {
        if (LotLightBridge::LampEnumerations() != g_bakedDueEnum) {
            g_bakedDue = false;
            g_baked = LotLightBridge::CurrentBakeLamps();
            g_haveBaked = true;
            g_bakedAt = now;
            g_editDiffEnum = -1;
        } else if (now - g_bakedDueAt > kSnapshotWaitMax) {
            g_bakedDue = false;
            g_haveBaked = false;
        }
    }
    // A lot lamp switched on or off anywhere (indoors too): the rooms get the game's own lighting budget for 3 s, so they
    // relight at once even while the camera moves
    static int lampSwitches = -1;
    if (const int sw = LotLightBridge::LampSwitches(); sw != lampSwitches) {
        if (lampSwitches >= 0) LotLightingMotion::Boost(3000);
        lampSwitches = sw;
    }

    // Outdoor lot lamps changed (see LotLightBridge::TrackLotLampEdits: only changes of lamps the bake takes, on lots
    // already loaded; user-driven = placed / moved / removed, automatic = switched, dimmed, recoloured): the game rebuilds
    // their light on the ground only when the lot is reloaded. And the "lot lamps light the street" switch applies live
    // (the bake reads it at run time): one rebuild. Decided by DecideEdit once quiet (see the variables above).
    if (const int edits = LotLightBridge::LotLampEdits(); edits != g_lastLampEdits) {
        const int user = LotLightBridge::LotLampUserEdits();
        const bool userDriven = user != g_lastUserEdits;
        g_lastLampEdits = edits;
        g_lastUserEdits = user;
        NoteEdit(now, userDriven, false, userDriven ? "observed lot lamps switched or edited" : "lot lamps switched, dimmed or recoloured");
        if (userDriven)
            for (uint64_t lot : LotLightBridge::LastUserChangeLots()) {
                g_editUserLots.push_back(lot);
                if (lot == 0) g_worldRigRefreshPending = true;
            }
    }
    if (g_lotLamps != g_lotLampsSeen) {
        g_lotLampsSeen = g_lotLamps;
        NoteEdit(now, true, true, g_lotLamps ? "lot lamps on the ground turned on" : "lot lamps on the ground turned off", true);
    }
    // Keep the first observed baseline of new lots even while another lot's edit waits.
    // Exclude new lots whose priority edit was just seen: their already-changed state
    // cannot establish the missing old baseline (the full path remains the fallback).
    static int adoptEnum = -1;
    if (g_haveBaked && !g_bakedDue && LotLightBridge::LampEnumerations() != adoptEnum) {
        adoptEnum = LotLightBridge::LampEnumerations();
        const std::vector<uint64_t> noEdits;
        if (const int n = LotLightBridge::AdoptNewLots(g_baked, LotLightBridge::CurrentBakeLamps(), g_editKickPending ? g_editUserLots : noEdits); n > 0) {
            g_editDiffEnum = -1;
            if (!kPublicBuild) LOG_INFO(std::format("[NightTerrainRelight] Snapshot: {} lot(s) loaded after the last rebuild joined it", n));
        }
    }

    // Street / lot lamp brightness in the terrain bake and the lamp colours: once the slider is let go, one terrain rebuild
    // (a switch: no snapshot compare) with the new values; the colours also re-solve the lots' outdoor light once.
    const bool dragging = MenuSliderHeld();
    if (!dragging && g_bakeGainInstalled && (g_streetLampGain != g_bakeGainSeen[0] || g_lotLampGain != g_bakeGainSeen[1])) {
        SetBakeGains(g_streetLampGain, g_lotLampGain);
        NoteEdit(now, true, true, "lamp brightness on the ground changed", true);
    }
    const float tintLot = g_lotTintOwn ? g_lotLampTint : g_lampTint;
    if (!dragging && (g_lampTint != g_tintSeen[0] || tintLot != g_tintSeen[1])) {
        g_tintSeen[0] = g_lampTint;
        g_tintSeen[1] = tintLot;
        ObjectLightBridge::SetLampTint(g_lampTint, tintLot);
        std::vector<uintptr_t> lights;
        if (LotLightBridge::EnumerateAllLights(lights) && ObjectLightBridge::RetintLamps(lights) > 0) {
            LotLightBridge::RequestLampRefresh();
            NoteEdit(now, true, true, "lamp colour changed", true);
            g_relightLotsRequested = true;
        }
    }
    // The stock lamps tracked for a live re-colour: freed lights are dropped every 30 s (RetintLamps keeps only the
    // enumerated ones; with the colours unchanged it re-colours nothing)
    static Clock::time_point lastPrune{};
    if (now - lastPrune > std::chrono::seconds(30)) {
        lastPrune = now;
        std::vector<uintptr_t> lights;
        if (LotLightBridge::EnumerateAllLights(lights)) ObjectLightBridge::RetintLamps(lights);
    }
    // Moonlight: every frame for the current night level; the object rigs gather it again once the slider is let go
    ApplyMoonlight(s.level);
    if (!dragging && g_moonlight != g_moonSeen) {
        g_moonSeen = g_moonlight;
        ObjectLightBridge::RequestRigRefresh();
    }

    // Stuck countdown (v0.1.0 fallback): a light change armed only +0x38, which never rebuilds in play (only +0x3C does),
    // so a lamp change the tracking above did not count (e.g. on a lot still settling) would wait forever. Only when an
    // outdoor LOT lamp armed it (street lamps streaming in with lots also arm it, and the vanilla game never rebuilds for
    // those), looked at most every 15 s, and since 29/09 it is an AUTOMATIC lamp change like the others: rebuilt only if
    // the bake's lamps differ from the last rebuild's snapshot (so the same state never rebuilds twice: the old session's
    // 15 s cadence of ~240 ms frames), at most once per 30 s, with the camera still.
    if (night && c38 == 0 && c3C <= 0) {
        const int arms = g_lotLampArms.load();
        if (++g_stuckFrames >= 120 && arms != g_armsAtLastStuckKick && now - g_lastStuckKick > std::chrono::seconds(15)) {
            g_lastStuckKick = now;
            g_armsAtLastStuckKick = arms;
            NoteEdit(now, false, false, "lights changed (stuck countdown)", true);
        }
    } else
        g_stuckFrames = 0;

    // A continuous colour drag cannot postpone its final-state reconciliation forever.
    if (g_editKickPending && EditReady(now, g_editFirstAt, g_editLastAt, g_editUser || g_editForce)) {
        // Native rigs are independent of the terrain bake. Reconcile once for a
        // coalesced observed world edit, even when terrain completion must wait.
        RefreshWorldRigs("after edit debounce");
        DecideEdit(s.cells, s.level, night, c38, now);
    }
    RefreshArrivingLots(now, night);

    // Local terrain relight / paced sweep: completion of the chunk in flight, release of the next one (at most one per
    // frame, a free frame in between, 8 per second plus a measured cheap-chunk priority reserve). A finished local batch puts its lamps into the snapshot; a
    // failure (a chunk never rendered, the terrain changed) falls back to the full rebuild, once (the local path stays
    // off for this world).
    {
        ChunkRelight::FrameResult fr;
        ChunkRelight::OnPresent(fr);
        for (const ChunkRelight::Done& d : fr.done) {
            if (d.sweep) {
                if (d.id != g_sweepId) continue;
                g_sweepId = 0;
                g_sweepsDone++;
                g_lastEvent = std::format("Terrain sweep done: {} ({}, {:.1f} s after it started)", g_sweepReason, d.text, MsSince(g_sweepAt) / 1000.0);
                LOG_INFO("[NightTerrainRelight] " + g_lastEvent);
                continue;
            }
            const auto it = std::find_if(g_localBatches.begin(), g_localBatches.end(), [&d](const LocalBatch& b) { return b.id == d.id; });
            if (it == g_localBatches.end()) continue;
            if (g_haveBaked) LotLightBridge::CoverLots(g_baked, it->changes, it->lamps);
            g_editDiffEnum = -1; // compare again with the updated snapshot
            g_localDone++;
            g_lastLocal = std::format("{}: {}", it->what, d.text);
            LOG_INFO("[NightTerrainRelight] Local relight done: " + g_lastLocal);
            g_localBatches.erase(it);
        }
        if (fr.failed) {
            g_localFailures++;
            g_localBatches.clear();
            g_sweepId = 0;
            Kick(s.cells, s.level, "local terrain relight failed (" + fr.why + ")");
        }
    }

    // A rebuild is coming (load, dusk, user-driven lamp change, armed): the smoothed maps hold new smoothing jobs until it
    // happened (the game's maps are shown meanwhile), so no chunk is smoothed twice. Not for automatic lamp changes: they
    // can wait up to 30 s and are often skipped. Nor for lamp changes the local relight will take (it re-renders only a
    // few chunks, which are smoothed at once).
    const bool armed = !g_pendingReason.empty() && c38 >= 0 && now - g_kickAt < std::chrono::seconds(5);
    const bool localLikely = (g_localRelight || g_editUser) && g_haveBaked && !g_editForce && !g_editLocalRefused && ChunkRelight::LikelyAvailable();
    const bool editSoon = g_editKickPending && (g_editUser || g_editForce) && !localLikely;
    if (g_loadKickPending || g_scheduled || editSoon || armed) LightmapSmooth::ExpectRebuild(30);

    const bool relightNow = g_relightLotsRequested.exchange(false);
    if (relightNow || (g_lotRelightPending && now >= g_lotRelightAt)) {
        g_lotRelightPending = false;
        const int n = QueueAllLotOutdoorRooms(s.lightMgr);
        if (n >= 0) {
            g_lotRelights.fetch_add(1);
            g_roomsQueued.fetch_add(n);
        }
        g_lastLotRelight = n < 0 ? std::string("failed (light tree not found)") : std::format("{} lot stories queued ({}; {})", n, relightNow ? "button" : "automatic", LevelText(s.level));
        LOG_INFO("[NightTerrainRelight] Lots: " + g_lastLotRelight);
    }

    if (g_loadKickPending)
        g_status = g_live ? "World loaded: rebuilding the terrain light once the night level is steady" : "World loading: the terrain light is rebuilt once the world is drawn";
    else if (c38 == 0 && !night)
        g_status = "Rebuild pending: the game only rebuilds the terrain at night (or in Build mode)";
    else if (c38 > 0)
        g_status = std::format("Rebuild in {} frames", c38);
    else
        g_status = night ? "Night: ok" : "Day: ok";
}

// ---- menu helpers ----

// "Reload save" badge of the rows whose change shows only when a save / world loads again
constexpr const char* kReloadBadge = "Reload save";
constexpr const char* kReloadTip = "This change shows after you load a save again";

// Colour of the lamp colour slider at t (0 = pink ... 1 = warm white), from the real tint math of
// ObjectLightBridge::TintStockColour (features/object_light_bridge.cpp): the stock pink base colour (1, 0.75, 0.79)
// blended towards warm white (1, 0.80, 0.62) scaled to the pink's luminance. The game's values are light colours, shown
// here as they are (an approximation of how they look on screen).
ImU32 LampColourAt(float t) {
    t = t < 0.0f ? 0.0f : (t > 1.0f ? 1.0f : t);
    const float pink[3] = {1.0f, 0.75f, 0.79f};
    const float lumPink = 0.2126f * pink[0] + 0.7152f * pink[1] + 0.0722f * pink[2];
    const float lumWarm = 0.2126f + 0.7152f * 0.80f + 0.0722f * 0.62f;
    const float k = pink[0] * lumPink / lumWarm;
    const float warm[3] = {k, 0.80f * k, 0.62f * k};
    int c[3];
    for (int i = 0; i < 3; i++) {
        const float v = pink[i] + (warm[i] - pink[i]) * t;
        c[i] = static_cast<int>(v * 255.0f + 0.5f);
        c[i] = c[i] < 0 ? 0 : (c[i] > 255 ? 255 : c[i]);
    }
    return IM_COL32(c[0], c[1], c[2], 255);
}

std::vector<BYTE> CallPatch(uintptr_t site, size_t prefixLen, const std::vector<BYTE>& prefix, void* target, size_t totalLen) {
    std::vector<BYTE> b(prefix.begin(), prefix.end());
    const uintptr_t callAt = site + prefixLen;
    const int32_t rel = static_cast<int32_t>(reinterpret_cast<uintptr_t>(target) - (callAt + 5));
    b.push_back(0xE8);
    for (int i = 0; i < 4; i++) b.push_back(static_cast<BYTE>((rel >> (8 * i)) & 0xFF));
    while (b.size() < totalLen) b.push_back(0x90);
    return b;
}

std::vector<BYTE> WithDword(std::vector<BYTE> head, uint32_t v, const std::vector<BYTE>& tail) {
    for (int i = 0; i < 4; i++) head.push_back(static_cast<BYTE>(v >> (8 * i)));
    head.insert(head.end(), tail.begin(), tail.end());
    return head;
}

// The addresses above from GameAddr (Steam: the fixed values in the comments; other builds: 0 where not found) and the
// expected bytes that embed an address (on Steam the very bytes the checks always had).
void LoadAddresses() {
    using GameAddr::Get;
    using GameAddr::Id;
    kRootGetter = Get(Id::RootGetter);
    kVisitorSite = Get(Id::TerrainVisitorSite);
    kArmSites[0].addr = Get(Id::ArmSiteRegister);
    kArmSites[1].addr = Get(Id::ArmSiteRemoval);
    kArmSites[2].addr = Get(Id::ArmSiteMoved);
    kChunkRenderCall = Get(Id::ChunkRenderCall);
    kChunkRenderFn = Get(Id::ChunkRenderFn);
    kChunkRenderContextAt = kChunkRenderCall ? kChunkRenderCall - 5 : 0;
    const uint32_t chunkRel = static_cast<uint32_t>(MemPatch::CalculateRelativeOffset(kChunkRenderCall, kChunkRenderFn));
    kChunkRenderContext = WithDword({0x6A, 0x00, 0x56, 0x8B, 0xCF, 0xE8}, chunkRel, {0xC6, 0x44, 0x24, 0x0C, 0x01});
    kChunkRenderOrig = WithDword({0xE8}, chunkRel, {});
    kLampColourSite = Get(Id::LampColourSite);
    kQueueRoom = Get(Id::QueueRoom);
    kLotPassSite = Get(Id::LotPassSite);
    kLotPassConst = Get(Id::LotPassConst);
    kLotPassNullBind = Get(Id::LotPassNullBind);
    kLotPassContext = WithDword({0xF3, 0x0F, 0x10, 0x05}, static_cast<uint32_t>(kLotPassConst), {0xF3, 0x0F, 0x11, 0x44, 0x24, 0x18, 0x74, 0x13});
    kLotPassNullBindBytes = WithDword({0xA1}, static_cast<uint32_t>(Get(Id::LotPassTexGlobal)), {0x6A, 0x00, 0x6A, 0x00});
    kQualitySites[0] = Get(Id::QualitySite0);
    kQualitySites[1] = Get(Id::QualitySite1);
    kBakeColourSite = Get(Id::BakeColourSite);
    if (g_sunlightBase < 0.0f) { // once read, keep it (the restore needs it)
        kSunlightScale = Get(Id::SunlightScale);
        // Steam: its only reader must still be "mov ecx,011D0918h; call" at 0x00C11B01
        static const BYTE kReader[] = {0xB9, 0x18, 0x09, 0x1D, 0x01, 0xE8};
        if (kSunlightScale && GameAddr::IsFixed() && !MemPatch::ValidateBytes(reinterpret_cast<LPVOID>(0x00C11B01), kReader, sizeof(kReader))) kSunlightScale = 0;
    }
    ResolveCamera(); // lamp change rebuilds wait for the camera to be still
    ChunkRelight::Init(); // local terrain relight: WorldManager global and terrain link (optional)
    if (!kPublicBuild)
        LOG_INFO(g_camOk ? std::format("[NightTerrainRelight] Camera eye for the lamp change rebuilds: [[{:#010x}]+{:#x}]+{:#x}", g_camRootGlobal, g_camOff, g_eyeOff)
                         : std::string("[NightTerrainRelight] Camera eye not found in the game's code: lamp change rebuilds do not wait for the camera"));
}

// "Not available on <version>: missing ..." when one of ids was not found on this build, else empty
std::string Missing(std::initializer_list<GameAddr::Id> ids) {
    std::string missing;
    return GameAddr::Have(ids, &missing) ? std::string() : GameAddr::NotAvailable(missing);
}

} // namespace

// Reinstalling after a setting change must happen on the render thread: Uninstall releases textures, shaders and maps
// that the draw hooks are using, and rewrites code the game runs there. Update() (message-loop thread) only schedules it.
std::atomic<bool> g_reinstallDue{false};
void (*g_reinstallFn)() = nullptr;
// Any lighting setting changed (menu, profile, undo, reset, the upper floors switch; 30/09, user: "whenever any setting of the
// lights changes, do the F9 refresh automatically"): "Refresh the lighting" runs once, kAutoRefreshDelay after the last change
// (a slider being dragged keeps pushing it back), on the render thread (Present)
constexpr DWORD kAutoRefreshDelay = 1000;
std::atomic<DWORD> g_autoRefreshAt{0};
// The pending refresh leaves the terrain alone (after a load: the load's own rebuild made it); any setting change clears it
std::atomic<bool> g_autoRefreshRoomsOnly{false};
void RequestAutoRefresh() {
    g_autoRefreshRoomsOnly.store(false);
    g_autoRefreshAt.store((GetTickCount() + kAutoRefreshDelay) | 1);
}
// 30/09, user: "right after loading, apply that refresh" (a room lit only by lamps of another story was black after a load
// until something solved it again): the lots and every room light again once, kAfterLoadDelay after the world is on screen
// (the lots have settled by then: at the latest 6 s, level_light_share), and the rigs with them; not the terrain
constexpr DWORD kAfterLoadDelay = 500, kAfterLoadMax = 8000;
DWORD g_afterLoadStarted = 0; std::uint32_t g_afterLoadQuiet = 0;
void RequestRefreshAfterLoad() {
    if (g_autoRefreshAt.load()) return; // a setting change is already pending: that refresh does it all
    g_afterLoadStarted = GetTickCount();
    g_afterLoadQuiet = 0;
    g_autoRefreshRoomsOnly.store(true);
    g_autoRefreshAt.store((GetTickCount() + kAfterLoadDelay) | 1);
}
void DeferredReinstall(IDirect3DDevice9*) {
    if (g_reinstallDue.exchange(false) && g_reinstallFn) g_reinstallFn();
}

class NightTerrainRelightPatch : public ApexPatch {
    std::vector<MemPatch::PatchLocation> patchedLocations;
    bool installedLotLampCode = false; // visitor + arm sites patched (the option itself is read live)
    bool installedStreetLamps = false;
    bool installedAllLotsHQ = false;
    bool installedLotPass = false;
    bool reinstalling = false; // ReinstallNow: LevelLightShare does not depend on the reinstalled options, leave it alone

  public:
    NightTerrainRelightPatch() : ApexPatch("NightTerrainRelight", nullptr) {
        g_reinstallFn = [] { if (g_self) g_self->ReinstallNow(); };
        g_self = this;
        RenderCallbacks::Add(RenderCallbacks::endSceneBeforeOverlay, DeferredReinstall);
        RegisterBoolSetting(&g_bridge, "luzDoPosteNaGramaDoLote", true,
            S3SS_TR("A grama do lote usa a mesma luz de poste que a grama do mundo (sem corte na divisa).",
                    "Street lamp light reaches inside lots, with no straight cut at the lot border."));
        RegisterBoolSetting(&g_objLamps, "postesNosObjetos", true,
            S3SS_TR("Postes e luminarias iluminam cercas, arbustos e objetos de fora como iluminam o chao.",
                    "Lamps light nearby fences, bushes and outdoor objects the way they light the ground."));
        RegisterFloatSetting(&g_objStrength, "forcaNosObjetos", SettingWidget::Slider, 1.0f, 0.25f, 3.0f,
            S3SS_TR("Forca da luz dos postes nos objetos.", "How strongly lamps light objects."));
        RegisterBoolSetting(&g_objAll, "lampadasEmTodosObjetos", true,
            S3SS_TR("Tambem escadas, grades, colunas e outros objetos que o jogo deixa sem luz de lampada (vale ao carregar o mundo).",
                    "Also stairs, railings, columns and other objects the game leaves without lamp light (applies when a world loads)."));
        RegisterBoolSetting(&g_smoothMaps, "mapaDeLuzSuavizado", true,
            S3SS_TR("Luz dos postes no chao mais lisa e sem manchas coloridas: o mapa de luz do terreno e ampliado 4x e limpo da compressao.",
                    "Smooth lamp light on the ground, with no blocky steps or colored specks."));
        if (!kPublicBuild) // developer A/B only; the public build always prefers the GPU (CPU when it is not available)
            RegisterBoolSetting(&g_smoothMapsGpu, "mapaDeLuzSuavizadoNaGpu", true,
                "Developer: smooth the ground light maps on the GPU in the same frame they change (off = the CPU worker path, "
                "with a plain copy until each map is ready). Falls back to the CPU by itself when the GPU path is not available.");
        if (!kPublicBuild) // developer A/B only; the public build always has soft lot edges
            RegisterBoolSetting(&g_softLotEdges, "bordaSuaveLote", true,
                "Developer: within 3 m of a lot edge, lot grass fades its lamp light to the ground light the world grass shows "
                "outside the lot, so a lamp near a lot edge leaves no step at the edge (off = the plain max of lot and ground light).");
        RegisterFloatSetting(&g_sidewalkClear, "calcadaComNevePisada", SettingWidget::Slider, 0.5f, 0.0f, 1.0f,
            S3SS_TR("Na neve, quanto do concreto das calcadas aparece por baixo da neve (0 = igual ao jogo, tudo coberto).",
                    "In snow, how much of the sidewalk concrete shows through (0 = like the game, fully covered)."));
        RegisterFloatSetting(&g_lampTint, "luzDasLampadasNatural", SettingWidget::Slider, 1.0f, 0.0f, 1.0f,
            S3SS_TR("Cor das lampadas de fabrica: 0 = rosada como no jogo, 1 = branco quente (vale ao carregar o save).",
                    "Color of stock lamps: 0 = pink like the game, 1 = warm white (applies when a save loads)."));
        RegisterBoolSetting(&g_fenceGround, "cercasComLuzDoChao", true,
            S3SS_TR("Cercas, grades, postes de cerca e escadas recebem a luz das lampadas do chao em volta (o jogo quase nunca manda lampada para elas).",
                    "Fences, railings, fence posts and stairs get the lamp light of the ground around them."));
        RegisterFloatSetting(&g_fenceGroundStrength, "forcaNasCercas", SettingWidget::Slider, 1.0f, 0.25f, 2.0f,
            S3SS_TR("Forca da luz do chao nas cercas, grades e escadas.", "How strongly fences, railings, stairs and the snow on them are lit."));
        RegisterBoolSetting(&g_walls, "paredesComLuz", true,
            S3SS_TR("As paredes externas recebem a luz das lampadas com a forca escolhida (desligado = como o jogo).",
                    "Outside walls get lamp light at the chosen brightness (off = the game's own dim wall light)."));
        RegisterFloatSetting(&g_wallStrength, "forcaNasParedes", SettingWidget::Slider, 2.0f, 0.25f, 4.0f,
            S3SS_TR("Intensidade da luz das lampadas nas paredes externas, durante o dia e a noite.",
                    "Intensity of lamp light on outside walls, by day and night."));
        RegisterBoolSetting(&g_levelShare, "luzExternaEntreAndares", true,
            S3SS_TR("Luminarias externas iluminam as paredes e pisos de todos os andares (a luz nao corta mais na linha do piso).",
                    "Outdoor lights reach the walls and floors of every story (no cut at the floor line)."));
        RegisterBoolSetting(&g_indoorShare, "luzInternaEntreAndares", true,
            "Indoor lamps light the story above or below through stairwells, atriums and removed floors (needs \"Outdoor light between floors\").");
        RegisterBoolSetting(&g_wallAlign, "paredesSemEmendaEntreAndares", true,
            "Walls are lit at the heights the game draws their light at, so the walls above and below a floor line meet on the same light (needs \"Outdoor light between floors\").");
        RegisterBoolSetting(&g_allFloors, "todosOsAndaresEmDetalhe", true,
            "Every floor of the lot being played is lit in full detail, so changing floors keeps the light instead of solving it again (more work when entering a lot).");
        RegisterBoolSetting(&g_objPixel, "objetosDeForaComLuzDoChao", true,
            S3SS_TR("Portas, janelas, balcoes e outros objetos de fora recebem, ponto a ponto, no minimo a luz do chao em volta (sem escurecer nada).",
                    "Outdoor doors, windows, counters and similar objects get at least the ground light around them, point by point."));
        RegisterBoolSetting(&g_objPixelLights, "luzPorPixelNosObjetos", true,
            S3SS_TR("Objetos de fora (balcoes, pecas modulares, portas) recebem as lampadas calculadas em cada ponto, iguais para todas as pecas: sem emendas de cor entre pecas vizinhas.",
                    "Outdoor objects (counters, modular pieces, doors) get lamp light computed at every point, the same for every piece: no colour seams between neighbouring pieces."));
        RegisterFloatSetting(&g_objPixelLightStrength, "forcaLuzPorPixelNosObjetos", SettingWidget::Slider, 1.0f, 0.25f, 3.0f,
            S3SS_TR("Forca das lampadas calculadas por ponto nos objetos de fora.", "Strength of the per-point lamp light on outdoor objects."));
        RegisterBoolSetting(&g_roofs, "telhadosComLuz", true,
            S3SS_TR("Telhados recebem a luz das lampadas e postes proximos (sombra mais suave tambem).",
                    "Roofs receive light from nearby lamps (with softer shadows)."));
        RegisterFloatSetting(&g_roofStrengthSetting, "forcaNosTelhados", SettingWidget::Slider, 0.6f, 0.05f, 2.0f,
            S3SS_TR("Forca da luz das lampadas nos telhados.", "How strongly lamps light roofs."));
        RegisterBoolSetting(&g_unlitOn, "comodosEscurosSemLuz", true,
            "Rooms with every lamp off keep only a little light (set below) instead of the game's blue glow.");
        RegisterFloatSetting(&g_unlitLight, "luzQueSobraNosComodos", SettingWidget::Slider, 0.35f, 0.1f, 0.8f,
            "How much of the game's light stays in a room with every lamp off, on walls, floors and furniture.");
        RegisterFloatSetting(&g_unlitBlue, "azulNosComodos", SettingWidget::Slider, 0.0f, 0.0f, 1.0f,
            "How blue the light left in rooms is, on walls, floors and furniture (1 = the game's blue, 0 = grey).");
        RegisterBoolSetting(&g_waterFilter, "waterSpecularFilter", true, "Stabilize lamp sparkles on water");
        RegisterBoolSetting(&g_waterColorCompression, "waterPreserveLampColors", true, "Preserve bright lamp colors on water");
        RegisterBoolSetting(&g_water, "lagosRefletemLampadas", true,
            S3SS_TR("A agua dos lagos reflete as lampadas e postes proximos a noite.", "Ponds glow and reflect nearby lamps at night."));
        RegisterFloatSetting(&g_waterStrengthSetting, "brilhoNaAgua", SettingWidget::Slider, 0.4f, 0.1f, 0.4f,
            S3SS_TR("Brilho do reflexo das lampadas na agua.", "Brightness of lamp reflections on water."));
        RegisterFloatSetting(&g_waterReflSetting, "reflexoNoLago", SettingWidget::Slider, 1.0f, 0.0f, 3.0f,
            S3SS_TR("Forca do reflexo da margem (arvores, casas, postes) na agua dos lagos.",
                    "Strength of the shore reflection (trees, houses, lamps) on ponds (needs Depth Blur)."));
        RegisterFloatSetting(&g_groundBrightness, "brilhoNoChao", SettingWidget::Slider, 1.0f, 0.25f, 3.0f,
            "How bright lamp light is on grass, lots and outdoor floors (1 = the default).");
        RegisterFloatSetting(&g_roadBrightness, "brilhoNasRuas", SettingWidget::Slider, 1.0f, 0.25f, 3.0f,
            "How bright lamp light is on roads and sidewalks, relative to the ground (1 = the same).");
        RegisterFloatSetting(&g_streetLampGain, "forcaDosPostes", SettingWidget::Slider, 1.0f, 0.25f, 3.0f,
            "How strongly street lamps light the ground (1 = the default; the ground light is rebuilt when it changes).");
        RegisterFloatSetting(&g_lotLampGain, "forcaDasLampadasDoLote", SettingWidget::Slider, 1.0f, 0.25f, 3.0f,
            "How strongly outdoor lot lamps light the ground (1 = the default; the ground light is rebuilt when it changes).");
        RegisterFloatSetting(&g_moonlight, "luar", SettingWidget::Slider, 1.0f, 0.0f, 2.0f,
            "How strong the moonlight is at night (1 = the game; 0 = no moonlight, only lamps and the sky's glow).");
        RegisterBoolSetting(&g_edgePad, "bordasDosMapasDeLuz", true,
            "Stairs, curtains and furniture indoors get smooth light from the game's room light maps: no dark side along outer walls, no steps between neighbouring objects.");
        RegisterBoolSetting(&g_lotTintOwn, "corPropriaNoLote", false,
            "Lot lamps get their own color (off = the same color as street lamps).");
        RegisterFloatSetting(&g_lotLampTint, "corDasLampadasDoLote", SettingWidget::Slider, 1.0f, 0.0f, 1.0f,
            "Color of stock lot lamps when they have their own: 0 = pink like the game, 1 = warm white.");
        RegisterBoolSetting(&g_autoDusk, "automaticoAoAnoitecer", true,
            S3SS_TR("Quando anoitece, manda o jogo refazer a luz do terreno com os postes acesos (o mesmo que acontece ao mover um poste no modo construcao).",
                    "At dusk, rebuild the terrain light with the lamps on."));
        RegisterBoolSetting(&g_lotLamps, "luzDoLoteNaGrama", true,
            S3SS_TR("Luminarias externas do lote tambem iluminam a grama do mundo fora do lote (quando acesas).",
                    "Outdoor lot lights also light the ground outside the lot."));
        RegisterFloatSetting(&g_delaySec, "atrasoSegundos", SettingWidget::Slider, 2.0f, 0.5f, 10.0f,
            S3SS_TR("Espera depois de anoitecer antes de refazer (da tempo de todas as luzes acenderem).",
                    "Delay after dusk before the rebuild (lets every lamp switch on)."));
        RegisterBoolSetting(&g_streetLampsLit, "postesAcesosNoCalculo", false,
            S3SS_TR("Quando o jogo calcula a luz de um lote, os postes da rua contam como acesos (igual a carregar o save a noite).",
                    "Experimental: street lamps count as lit when the game solves a lot's light."));
        RegisterBoolSetting(&g_allLotsHQ, "qualidadeAltaEmTodosOsLotes", false,
            S3SS_TR("Todos os lotes usam a qualidade de luz alta do lote ativo (corrige o corte da luz dos postes na divisa). Vale para lotes carregados depois de ligar.",
                    "Experimental: every lot uses the active lot's high lighting quality."));
        RegisterBoolSetting(&g_lotPassNoTerrainMap, "gramaDoLoteUsaLuzDoLote", false,
            S3SS_TR("A grama do lote continua usando a luz do proprio lote mesmo depois de refazer o terreno (a luz dos postes nao some dentro do lote).",
                    "Experimental: lot grass keeps the lot's own light after a terrain rebuild."));
        RegisterBoolSetting(&g_relightLots, "recalcularLotesAoAnoitecer", false,
            S3SS_TR("Depois de refazer o terreno a noite, recalcula a luz de todos os lotes como quando um poste e movido no modo construcao.",
                    "Experimental: after the dusk terrain rebuild, re-solve the light of every lot."));
        if (!kPublicBuild) { // Developer controls only; public uses the approved paced sweep default without exposing diagnostic toggles.
            RegisterBoolSetting(&g_localRelight, "relightNearbyChunks", false,
                "Developer: a lamp change re-renders only the terrain chunks under the changed lamps, one per frame through the game's own "
                "chunk sweep, instead of rebuilding every chunk at once (~240 ms). Falls back to the full rebuild when it cannot.");
            RegisterBoolSetting(&g_pacedSweep, "relightPacedSweep", true,
                "Developer: the dusk rebuild and lamp-change rebuilds re-render every terrain chunk one at a time, nearest to the camera "
                "first, instead of all at once. The rebuild after loading a world stays a full rebuild.");
        }
    }

    bool Install() override {
        if (isEnabled) return true;
        lastError.clear();
        LOG_INFO("[NightTerrainRelight] Installing...");
        using GameAddr::Id;
        LoadAddresses();
        if (const std::string missing = Missing({Id::RootGetter, Id::RootPtr, Id::QueueRoom}); !missing.empty()) return Fail(missing);

        BYTE getter[sizeof(kRootGetterBytes)];
        std::memcpy(getter, reinterpret_cast<const void*>(kRootGetter), sizeof(getter));
        std::memcpy(getter + 1, kRootGetterBytes + 1, 4); // imm32 is the pointer we want, do not compare it
        if (std::memcmp(getter, kRootGetterBytes, sizeof(getter)) != 0)
            return Fail(std::format("Light manager code differs at 0x{:X} (different game version?)", kRootGetter));
        g_rootPtrAddr = *reinterpret_cast<const uint32_t*>(kRootGetter + 1);
        if (!LightDiag::Init()) LOG_WARNING("[NightTerrainRelight] Light diagnostics not available on this game version");

        // The lot-lamp predicates read luzDoLoteNaGrama (g_lotLamps) at run time and, with it off, answer exactly like
        // the game's own test (OriginalWorldLightTest first). So they are installed whatever the option says and the
        // option applies live (no reinstall); only when the code differs is the option required to be off.
        const std::string lotMissing = Missing({Id::TerrainVisitorSite, Id::ArmSiteRegister, Id::ArmSiteRemoval, Id::ArmSiteMoved});
        if (!lotMissing.empty() && g_lotLamps) return Fail(lotMissing);
        bool lotCodeOk = lotMissing.empty() && MemPatch::ValidateBytes(reinterpret_cast<LPVOID>(kVisitorSite), kVisitorOrig.data(), kVisitorOrig.size());
        if (!lotCodeOk && g_lotLamps)
            return Fail(std::format("Terrain light gathering differs at 0x{:X} (different game version or another mod?)", kVisitorSite));
        for (const auto& s : kArmSites)
            if (lotCodeOk && !MemPatch::ValidateBytes(reinterpret_cast<LPVOID>(s.addr), kArmOrig.data(), kArmOrig.size())) {
                if (g_lotLamps) return Fail(std::format(S3SS_TR("Teste de luz nao confere em {:#x}: {}", "Light test differs at {:#x}: {}"), s.addr, s.name));
                lotCodeOk = false;
            }
        if (!lotCodeOk) LOG_WARNING("[NightTerrainRelight] Terrain light code differs: \"Lot lamps light the street\" cannot be turned on");
        if (lotCodeOk) {
            // mov esi,ecx; push edi; call TerrainLightTest; nop x3
            const auto visitorBytes = CallPatch(kVisitorSite, 3, {0x8B, 0xF1, 0x57}, reinterpret_cast<void*>(&TerrainLightTest), kVisitorOrig.size());
            if (!MemPatch::WriteBytes(kVisitorSite, visitorBytes, &patchedLocations, &kVisitorOrig)) {
                MemPatch::RestoreAll(patchedLocations);
                return Fail(S3SS_TR("Falha ao alterar a coleta de luzes do terreno", "Could not patch the terrain light gathering"));
            }
            for (const auto& s : kArmSites) {
                // push edi; call ArmTest; nop x3
                const auto armBytes = CallPatch(s.addr, 1, {0x57}, reinterpret_cast<void*>(&ArmTest), kArmOrig.size());
                if (!MemPatch::WriteBytes(s.addr, armBytes, &patchedLocations, &kArmOrig)) {
                    MemPatch::RestoreAll(patchedLocations);
                    return Fail(std::format(S3SS_TR("Falha ao alterar {}", "Could not patch {}"), s.name));
                }
            }
            FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
        }
        // Steam: its exact prologue; other builds: the signature (the same bytes, or the target of a call to it) found it
        if (GameAddr::IsFixed() && std::memcmp(reinterpret_cast<const void*>(kQueueRoom), kQueueRoomBytes, sizeof(kQueueRoomBytes)) != 0) {
            MemPatch::RestoreAll(patchedLocations);
            return Fail(std::format("Room queue code differs at 0x{:X} (different game version?)", kQueueRoom));
        }
        if (g_streetLampsLit) {
            if (const std::string missing = Missing({Id::LampColourSite}); !missing.empty()) {
                MemPatch::RestoreAll(patchedLocations);
                return Fail(missing);
            }
            if (!MemPatch::ValidateBytes(reinterpret_cast<LPVOID>(kLampColourSite), kLampColourOrig.data(), kLampColourOrig.size())) {
                MemPatch::RestoreAll(patchedLocations);
                return Fail(std::format("Street lamp light code differs at 0x{:X} (different game version or another mod?)", kLampColourSite));
            }
            // call StreetLampColourStub; nop x2
            const auto lampBytes = CallPatch(kLampColourSite, 0, {}, reinterpret_cast<void*>(&StreetLampColourStub), kLampColourOrig.size());
            if (!MemPatch::WriteBytes(kLampColourSite, lampBytes, &patchedLocations, &kLampColourOrig)) {
                MemPatch::RestoreAll(patchedLocations);
                return Fail(S3SS_TR("Falha ao alterar o calculo de luz do poste", "Could not patch the street lamp light"));
            }
            FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
        }
        installedStreetLamps = g_streetLampsLit;
        if (g_allLotsHQ) {
            if (const std::string missing = Missing({Id::QualitySite0, Id::QualitySite1}); !missing.empty()) {
                MemPatch::RestoreAll(patchedLocations);
                return Fail(missing);
            }
            for (uintptr_t site : kQualitySites)
                if (!MemPatch::ValidateBytes(reinterpret_cast<LPVOID>(site), kQualityOrig.data(), kQualityOrig.size())) {
                    MemPatch::RestoreAll(patchedLocations);
                    return Fail(std::format(S3SS_TR("Qualidade do lote nao confere em {:#x}", "Lot quality code differs at {:#x}"), site));
                }
            for (uintptr_t site : kQualitySites)
                if (!MemPatch::WriteBytes(site, kQualityNew, &patchedLocations, &kQualityOrig)) {
                    MemPatch::RestoreAll(patchedLocations);
                    return Fail(S3SS_TR("Falha ao alterar a qualidade do lote", "Could not patch the lot quality"));
                }
        }
        installedAllLotsHQ = g_allLotsHQ;
        if (g_lotPassNoTerrainMap) {
            if (const std::string missing = Missing({Id::LotPassSite, Id::LotPassConst, Id::LotPassTexGlobal, Id::LotPassNullBind}); !missing.empty()) {
                MemPatch::RestoreAll(patchedLocations);
                return Fail(missing);
            }
            if (!MemPatch::ValidateBytes(reinterpret_cast<LPVOID>(kLotPassSite), kLotPassOrig.data(), kLotPassOrig.size()) ||
                std::memcmp(reinterpret_cast<const void*>(kLotPassSite + 8), kLotPassContext.data(), kLotPassContext.size()) != 0 ||
                std::memcmp(reinterpret_cast<const void*>(kLotPassNullBind), kLotPassNullBindBytes.data(), kLotPassNullBindBytes.size()) != 0) {
                MemPatch::RestoreAll(patchedLocations);
                return Fail(std::format("Terrain light pass differs at 0x{:X}", kLotPassSite));
            }
            const auto lotPassBytes = CallPatch(kLotPassSite, 0, {}, reinterpret_cast<void*>(&LotPassStub), kLotPassOrig.size());
            if (!MemPatch::WriteBytes(kLotPassSite, lotPassBytes, &patchedLocations, &kLotPassOrig)) {
                MemPatch::RestoreAll(patchedLocations);
                return Fail(S3SS_TR("Falha ao alterar a passada de luz do lote", "Could not patch the lot light pass"));
            }
            FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
        }
        installedLotPass = g_lotPassNoTerrainMap;
        installedLotLampCode = lotCodeOk;
        // "Street lamps" / "Lot lamps" on the ground (optional: without it those two sliders do nothing and say so)
        g_bakeGainInstalled = false;
        SetBakeGains(g_streetLampGain, g_lotLampGain);
        if (kBakeColourSite && Missing({Id::BakeColourSite}).empty() &&
            MemPatch::ValidateBytes(reinterpret_cast<LPVOID>(kBakeColourSite), kBakeColourOrig.data(), kBakeColourOrig.size())) {
            // call BakeColourStub; nop x2
            const auto bakeBytes = CallPatch(kBakeColourSite, 0, {}, reinterpret_cast<void*>(&BakeColourStub), kBakeColourOrig.size());
            g_bakeGainInstalled = MemPatch::WriteBytes(kBakeColourSite, bakeBytes, &patchedLocations, &kBakeColourOrig);
            if (g_bakeGainInstalled) FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(kBakeColourSite), kBakeColourOrig.size());
        }
        if (!g_bakeGainInstalled) LOG_WARNING("[NightTerrainRelight] Terrain bake colour code differs or was not found: street / lot lamp brightness on the ground not available");
        g_lotLampsSeen = g_lotLamps; // installed with the current value: no "switch changed" rebuild for it

        // Chunk re-render notices for the smoothed maps (optional: without it the maps are found by hashing).
        g_chunkHookInstalled = false;
        const std::string chunkMissing = Missing({Id::ChunkRenderCall, Id::ChunkRenderFn});
        if (chunkMissing.empty() && MemPatch::ValidateBytes(reinterpret_cast<LPVOID>(kChunkRenderContextAt), kChunkRenderContext.data(), kChunkRenderContext.size())) {
            const auto callBytes = CallPatch(kChunkRenderCall, 0, {}, reinterpret_cast<void*>(&ChunkRenderThunk), kChunkRenderOrig.size());
            g_chunkHookInstalled = MemPatch::WriteBytes(kChunkRenderCall, callBytes, &patchedLocations, &kChunkRenderOrig);
            if (g_chunkHookInstalled) FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(kChunkRenderCall), 5);
        }
        if (!g_chunkHookInstalled)
            LOG_WARNING(chunkMissing.empty() ? std::format("[NightTerrainRelight] Terrain chunk re-render call differs at 0x{:X}: changed ground light maps are found by hashing only",
                                                           kChunkRenderCall)
                                             : "[NightTerrainRelight] Terrain chunk re-render call: " + chunkMissing + " (changed ground light maps are found by hashing only)");
        ChunkRelight::SetHooked(g_chunkHookInstalled); // the local relight and the paced sweep need its completion signal

        // Installed again in the world it was removed from (the menu's on/off): no new-world handling (no clear, no load
        // rebuild), only one rebuild at night so lot lamps changed meanwhile reach the ground.
        if (g_lastCells != 0 && !reinstalling) {
            // a switch (fast path, no snapshot compare: lamps changed while the patch was off were not tracked)
            g_editKickPending = true;
            g_editFirstAt = Clock::now();
            g_editLastAt = g_editFirstAt - kEditQuiet;
            g_editUser = true;
            g_editForce = true;
            g_editWait = EditWait::None;
            g_editUserLots.clear();
            g_editReason = "Night Lights turned on";
            g_worldRigRefreshPending = false;
        }

        D3D9Hooks::RegisterPresent("NightTerrainRelight", [](D3D9Hooks::DeviceContext& ctx, const RECT*, const RECT*, HWND, const RGNDATA*) {
            static bool capsLogged = false; // step 3 (increment 0): the per-pixel lamp shaders need about 600 slots
            if (!capsLogged && ctx.device) {
                capsLogged = true;
                D3DCAPS9 caps{};
                if (SUCCEEDED(ctx.device->GetDeviceCaps(&caps)))
                    LOG_INFO(std::format("[NightTerrainRelight] Shader limits: PS 3.0 {} instruction slots, VS 3.0 {}, PS version {:X}", caps.MaxPixelShader30InstructionSlots,
                                         caps.MaxVertexShader30InstructionSlots, caps.PixelShaderVersion & 0xFFFF));
            }
            OnPresent();
            LightProbe::OnPresent(ctx.device); // the light capture (F7): the draws painting the pixel under the mouse
            ObjectLightBridge::SetStrength(g_objStrength);
            ObjectLightBridge::SetAllObjects(g_objAll);
            ObjectLightBridge::OnPresent();
            LevelLightShare::SetIndoor(g_indoorShare);
            LevelLightShare::SetWallAlign(g_wallAlign);
            LevelLightShare::SetAllFloors(g_allFloors);
            UnlitRooms::Set(g_unlitOn, g_unlitLight, g_unlitBlue);
            UnlitRooms::SetNightLevel(std::fmax(g_menuLevel.load(), 0.0f));
            UnlitRooms::OnPresent();
            LampMarkFilter::OnPresent(g_menuLevel.load());
            if (DWORD at = g_autoRefreshAt.load(); at && static_cast<int32_t>(GetTickCount() - at) >= 0 && !g_reinstallDue.load() &&
                g_autoRefreshAt.compare_exchange_strong(at, 0)) {
                const bool afterLoad = g_autoRefreshRoomsOnly.load();
                const DWORD tick = GetTickCount();
                const bool busy = afterLoad && tick - g_afterLoadStarted < kAfterLoadMax && LevelLightShare::LoadedRoomsBusy();
                const bool ready = !afterLoad || RoomAmbientPolicy::AfterLoadRefreshReady(tick, g_afterLoadStarted, busy, g_afterLoadQuiet);
                if (ready) {
                    g_autoRefreshRoomsOnly.store(false);
                    NightLighting::RefreshAll(afterLoad ? "after loading" : "a setting changed", !afterLoad);
                } else g_autoRefreshAt.store((tick + 200) | 1);
            }
            LevelLightShare::OnPresent();
            LotLightBridge::SetNightLevel(g_level);
            LotLightBridge::SetRoofFix(g_roofs, g_roofStrengthSetting);
            // The lake pass draws the lamp glow AND the shore reflection: it runs while either is wanted. With the glow
            // switched off its lamp strength is 0, so the pass adds the reflection alone (menu: Water Reflections card);
            // the reflection alone needs the scene depth (Depth Blur), without it the pass would add nothing.
            const bool shoreOnly = !g_water && g_waterReflSetting > 0.0f && DepthShare::Texture() != nullptr;
            LotLightBridge::SetWaterFix(g_water || shoreOnly, g_water ? std::clamp(g_waterStrengthSetting, 0.1f, 0.4f) : 0.0f, g_waterReflSetting, g_waterFilter, g_waterColorCompression);
            LotLightBridge::OnPresent();
            LightmapSmooth::SetEnabled(g_smoothMaps);
            LotLightBridge::SetSidewalkClear(g_sidewalkClear);
            LotLightBridge::SetGroundBrightness(g_groundBrightness, g_roadBrightness, g_lotLampGain);
            RoomMapPadding::SetEnabled(g_edgePad);
            LotLightBridge::SetIndoorSmooth(g_edgePad);
            RoomMapPadding::OnPresent();
            // lamps created from now on take the new colour once the slider is let go (OnPresent re-colours the others)
            if (!MenuSliderHeld()) ObjectLightBridge::SetLampTint(g_lampTint, g_lotTintOwn ? g_lotLampTint : g_lampTint);
            LotLightBridge::SetFenceGroundLight(g_fenceGround, g_fenceGroundStrength);
            LotLightBridge::SetWallGain(g_wallStrength, g_walls);
            LotLightBridge::SetObjectPixelLamps(g_objPixel && RigTracker::IsInstalled(), g_objStrength);
            LotLightBridge::SetObjectPixelLights(g_objPixelLights, g_objPixelLightStrength);
            LightmapSmooth::SetGpuPreferred(g_smoothMapsGpu);
            LotLightBridge::SetSoftLotEdges(g_softLotEdges);
            LightmapSmooth::OnPresent(ctx.device);
            return D3D9Hooks::HookAction::Continue;
        }, D3D9Hooks::Priority::Last);

        // The first install takes the saved colours; later ones leave g_tintSeen as it is, so a colour changed while Night
        // Lights was off re-colours the lamps once it is on again
        static bool tintInit = false;
        if (!tintInit) {
            tintInit = true;
            g_tintSeen[0] = g_lampTint;
            g_tintSeen[1] = g_lotTintOwn ? g_lotLampTint : g_lampTint;
        }
        ObjectLightBridge::SetLampTint(g_tintSeen[0], g_tintSeen[1]);
        ObjectLightBridge::InstallLampColour();
        LightmapSmooth::SetEnabled(g_smoothMaps);
        RenderCallbacks::Add(RenderCallbacks::preReset, LightmapSmooth::OnPreReset);
        LotLightBridge::SetEnabled(g_bridge);
        LotLightBridge::SetObjectShadowFix(g_objLamps);
        ObjectLightBridge::SetAllObjects(g_objAll);
        if (g_objLamps) {
            std::string objErr;
            if (!ObjectLightBridge::Install(objErr)) LOG_WARNING("[NightTerrainRelight] " + objErr);
        }
        if (g_objPixel && !RigTracker::IsInstalled()) RigTracker::Install();
        {
            std::string unlitErr;
            if (!UnlitRooms::Install(unlitErr)) LOG_WARNING("[NightTerrainRelight] " + unlitErr);
        }
        if (!LampMarkFilter::IsInstalled()) {
            std::string markErr;
            if (!LampMarkFilter::Install(markErr)) LOG_WARNING("[NightTerrainRelight] " + markErr);
        }
        if (g_levelShare && !LevelLightShare::IsInstalled()) {
            std::string shareErr;
            if (!LevelLightShare::Install(shareErr)) LOG_WARNING("[NightTerrainRelight] " + shareErr);
        }
        isEnabled = true;
        LOG_INFO(std::format("[NightTerrainRelight] Installed (at dusk={}, lot lamps on the ground={}, delay={}s, root={:#x})", g_autoDusk, g_lotLamps, g_delaySec, g_rootPtrAddr));
        return true;
    }

    bool Uninstall() override {
        if (!isEnabled) return true;
        lastError.clear();
        D3D9Hooks::UnregisterAll("NightTerrainRelight");
        RestoreMoonlight();
        LightProbe::Shutdown();
        RenderCallbacks::Remove(RenderCallbacks::preReset, LightmapSmooth::OnPreReset);
        // A reinstall (render thread, Install follows at once in the same world) keeps the chunk maps, smoothed maps and
        // atlas: no device reset can happen in between. A real uninstall releases them (the preReset callback is gone).
        LotLightBridge::Shutdown(reinstalling);
        if (!reinstalling) LightmapSmooth::Clear();
        ObjectLightBridge::UninstallLampColour();
        ObjectLightBridge::Uninstall();
        if (!reinstalling) UnlitRooms::Uninstall(); // first: its last retint still queues rooms with the solve deferral (review M5)
        if (!reinstalling) LampMarkFilter::Uninstall();
        if (!reinstalling) LevelLightShare::Uninstall();
        if (!reinstalling) RigTracker::Uninstall();
        if (!MemPatch::RestoreAll(patchedLocations)) return Fail(S3SS_TR("Falha ao restaurar os bytes originais", "Could not restore the original code"));
        FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
        patchedLocations.clear();
        g_chunkHookInstalled = false;
        g_bakeGainInstalled = false;
        // The chunk re-render call is the game's own again: a local relight in progress is dropped (the chunk in flight, if
        // any, is rendered by the game at its next update). Its lamps were never marked as baked: the change is decided
        // again (the diff against the snapshot still shows it). A paced sweep in progress is cut short: its snapshot was
        // taken when it started, so the chunks it did not reach get a rebuild without the snapshot compare (a switch).
        ChunkRelight::SetHooked(false);
        const bool localPending = !g_localBatches.empty(), sweepPending = g_sweepId != 0;
        ChunkRelight::Drop();
        g_localBatches.clear();
        g_sweepId = 0;
        if (!reinstalling) {
            g_arrivals.clear();
            g_arrivalRelit.clear();
        }
        if (sweepPending) NoteEdit(Clock::now(), true, true, "paced terrain sweep interrupted", true);
        else if (localPending) NoteEdit(Clock::now(), true, false, "local terrain relight interrupted", true);
        g_lotRelightPending = false;
        g_scheduled = false;
        // g_lastCells is kept: installing again in the same world is not a world load (no clear, no load rebuild). A
        // world that changed meanwhile has another cells pointer and is handled as new; with no world at all OnPresent
        // resets it.
        g_menuLevel.store(-1.0f);
        isEnabled = false;
        LOG_INFO("[NightTerrainRelight] Uninstalled");
        return true;
    }

    // "At dusk", the delay and "lot lamps light the street" (luzDoLoteNaGrama: its predicates read it at run time) are
    // live; only the developer options that change code bytes need a reinstall (and luzDoLoteNaGrama if its code could
    // not be installed). Runs on the message-loop thread: it only schedules the reinstall, which DeferredReinstall runs on
    // the render thread. The reinstall keeps the world state and the ground light maps (see Uninstall).
    void Update() override {
        if (!pendingReinstall) return;
        const bool lotCodeMissing = g_lotLamps && !installedLotLampCode;
        if (!isEnabled.load() || (!lotCodeMissing && g_streetLampsLit == installedStreetLamps && g_allLotsHQ == installedAllLotsHQ &&
                                  g_lotPassNoTerrainMap == installedLotPass)) {
            pendingReinstall = false;
            return;
        }
        if (std::chrono::steady_clock::now() - lastSettingChange < SETTING_CHANGE_DEBOUNCE) return;
        pendingReinstall = false;
        g_reinstallDue = true;
    }

    void ReinstallNow() {
        if (!isEnabled.load()) return;
        LOG_INFO("[NightTerrainRelight] Reinstalling after an option change");
        reinstalling = true;
        const bool removed = Uninstall();
        reinstalling = false;
        // LevelLightShare was left in place: if the rest cannot come back, take it out too (the patch shows as off). The
        // chunk maps kept for the reinstall are released as well (no preReset callback without the patch).
        reinstalling = true; // Install below: not "turned on again" (no extra rebuild)
        const bool installed = removed && Install();
        reinstalling = false;
        if (installed) RequestAutoRefresh(); // the options that needed the reinstall show at once
        if (removed && !installed) {
            if (LevelLightShare::IsInstalled()) LevelLightShare::Uninstall();
            RigTracker::Uninstall();
            LotLightBridge::OnWorldChanged();
            LightmapSmooth::Clear();
        }
    }

    static inline NightTerrainRelightPatch* g_self = nullptr;

    // Parts that are installed or removed live when their option changes (public menu, reset to defaults).
    void ApplyLive(bool bridgeBefore, bool objBefore, bool shareBefore, bool objPixelBefore) {
        if (!isEnabled.load()) return;
        if (g_bridge != bridgeBefore) LotLightBridge::SetEnabled(g_bridge);
        if (g_objLamps != objBefore) {
            if (g_objLamps) {
                std::string objErr;
                if (!ObjectLightBridge::Install(objErr)) LOG_WARNING("[NightTerrainRelight] " + objErr);
            } else
                ObjectLightBridge::Uninstall();
            LotLightBridge::SetObjectShadowFix(g_objLamps);
        }
        if (g_levelShare != shareBefore) {
            if (g_levelShare) {
                std::string shareErr;
                if (!LevelLightShare::Install(shareErr)) LOG_WARNING("[NightTerrainRelight] " + shareErr);
            } else
                LevelLightShare::Uninstall();
        }
        if (g_objPixel != objPixelBefore) {
            if (g_objPixel) RigTracker::Install();
            else RigTracker::Uninstall();
        }
    }

    // Profiles, looks and undo: the settings (and on / off) of a saved table, with the parts that follow their option
    // installed or removed live, like a change in the menu
    void ApplyTableLive(const toml::table& table) override {
        const bool wasEnabled = isEnabled.load();
        const auto changedBefore = lastSettingChange;
        const bool bridgeBefore = g_bridge, objBefore = g_objLamps, shareBefore = g_levelShare, objPixelBefore = g_objPixel;
        ApexPatch::ApplyTableLive(table);
        if (wasEnabled && isEnabled.load()) ApplyLive(bridgeBefore, objBefore, shareBefore, objPixelBefore);
        if (wasEnabled && isEnabled.load() && lastSettingChange != changedBefore) RequestAutoRefresh(); // a profile, look or undo
    }

    static void ResetDefaults() {
        g_bridge = true;
        g_lotLamps = true;
        g_autoDusk = true;
        g_levelShare = true;
        g_indoorShare = true;
        g_wallAlign = true;
        g_allFloors = true;
        g_unlitOn = true;
        g_unlitLight = 0.35f;
        g_unlitBlue = 0.0f;
        g_smoothMaps = true;
        g_smoothMapsGpu = true;
        g_softLotEdges = true;
        g_lampTint = 1.0f;
        g_lotTintOwn = false;
        g_lotLampTint = 1.0f;
        g_edgePad = true;
        g_groundBrightness = 1.0f;
        g_roadBrightness = 1.0f;
        g_streetLampGain = 1.0f;
        g_lotLampGain = 1.0f;
        g_moonlight = 1.0f;
        g_objLamps = true;
        g_objStrength = 1.0f;
        g_objAll = true;
        g_objPixel = true;
        g_objPixelLights = true;
        g_objPixelLightStrength = 1.0f;
        g_fenceGround = true;
        g_fenceGroundStrength = 1.0f;
        g_walls = true;
        g_wallStrength = 2.0f;
        g_roofs = true;
        g_roofStrengthSetting = 0.6f;
        g_water = true;
        g_waterFilter = g_waterColorCompression = true;
        g_waterStrengthSetting = 0.4f;
        g_waterReflSetting = 1.0f;
        g_sidewalkClear = 0.5f;
        g_delaySec = 2.0f;
        g_streetLampsLit = false;
        g_allLotsHQ = false;
        g_lotPassNoTerrainMap = false;
        g_relightLots = false;
        g_localRelight = false;
        g_pacedSweep = true;
    }

    // ---- menu (see night_lighting.h): the Night Lights page draws these pieces card by card ----

    // Runs body() (it draws controls and returns whether one changed); on a change, installs / removes the parts that
    // follow their option live and schedules the save (and, for the lot lamp options, the reinstall).
    template <typename Body> void Edit(Body&& body) {
        const bool bridgeBefore = g_bridge, objBefore = g_objLamps, shareBefore = g_levelShare, objPixelBefore = g_objPixel;
        if (body()) {
            ApplyLive(bridgeBefore, objBefore, shareBefore, objPixelBefore);
            NotifySettingChanged();
            RequestAutoRefresh();
        }
    }

    // Every row goes through ApexUi::SwitchRow / Slider with its label as the stable id (unique within its card).

    // Light styles: every brightness setting on the Lighting page at once (the moonlight and colors are not part of a style)
    // each: ground, roads, street lamps, lot lamps, objects, pieces, fences, walls, roofs (the StyleValue order)
    struct LightStyle {
        float v[9];
    };
    // Preserve the approved Soft surface ratios; variants change intensity conservatively.
    static constexpr LightStyle kStyles[] = {
        {{0.675f, 1.0f, 0.72f, 0.72f, 0.675f, 0.675f, 0.675f, 1.35f, 0.405f}}, // Subtle
        {{0.75f, 1.0f, 0.8f, 0.8f, 0.75f, 0.75f, 0.75f, 1.5f, 0.45f}},         // Soft reference
        {{1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 1.0f, 2.0f, 0.6f}},             // Natural
    };
    static float* StyleValue(int i) {
        float* const v[] = {&g_groundBrightness, &g_roadBrightness, &g_streetLampGain, &g_lotLampGain, &g_objStrength,
                            &g_objPixelLightStrength, &g_fenceGroundStrength, &g_wallStrength, &g_roofStrengthSetting};
        return v[i];
    }
    static int CurrentStyle() {
        for (int s = 0; s < static_cast<int>(std::size(kStyles)); s++) {
            const float* want = kStyles[s].v;
            bool same = true;
            for (int i = 0; i < 9 && same; i++) same = std::fabs(*StyleValue(i) - want[i]) < 0.005f;
            if (same) return s;
        }
        return -1; // custom
    }

    void DrawLightingBalance() {
        Edit([] {
            static const char* const kStyleNames[] = {"Subtle", "Soft", "Natural"};
            static const char* const kStyleTips[] = {"Less light, more contrast at night", "Gentle, balanced lighting", "The original Apex lighting balance"};
            static LightStyle previous{};
            static bool canUndo = false;
            int style = CurrentStyle();
            bool changed = false;
            const float u = ApexUi::Unit();
            const ApexUi::IconId icons[] = {ApexUi::IconId::Moon, ApexUi::IconId::MoonStar, ApexUi::IconId::Lightbulb, ApexUi::IconId::SlidersHorizontal};
            for (int i = 0; i < 4; ++i) {
                const bool selected = i == 3 ? style < 0 : style == i;
                const char* name = i == 3 ? "Custom" : kStyleNames[i];
                const char* tip = i == 3 ? "Your individual lighting settings are active" : kStyleTips[i];
                ImGui::PushID(i);
                const bool clicked = ApexUi::ProfileChoiceRow("LightingStyle", icons[i], name, tip, selected);
                if (clicked && i < 3 && !selected) {
                    for (int j = 0; j < 9; ++j) { previous.v[j] = *StyleValue(j); *StyleValue(j) = kStyles[i].v[j]; }
                    canUndo = true;
                    style = i;
                    changed = true;
                    ApexUi::ReportChange("Lighting balance changed");
                }
                if (i == 3 && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", I18n::Tr("Custom lighting balance"));
                ImGui::PopID();
            }
            ApexUi::Gap(ApexUi::kSpace2);
            if (ApexUi::BeginAdvanced("LightingBalanceScope", "What does this choice change?")) {
                ApexUi::MutedText("Changes lamp intensity on the Lighting page. Water settings, lamp colors and room background light stay as they are.");
                ApexUi::EndAdvanced();
            }
            if (canUndo) {
                if (ApexUi::IconTextButton("Undo choice", ApexUi::IconId::Undo2)) {
                    for (int j = 0; j < 9; ++j) *StyleValue(j) = previous.v[j];
                    canUndo = false;
                    changed = true;
                    ApexUi::ReportChange("Lighting balance restored");
                }
            }
            return changed;
        });
    }

    // Lighting > Ground
    // Lighting > Stories: everything about lamp light between the floors of a house (2026-09-29, user: "everything about
    // stories deserves its own tab")
    void DrawStoriesCard(void (*drawUpperFloorRow)()) {
        using ApexUi::IconId;
        ImGui::PushID("NightStories");
        if (ApexUi::BeginCard("##Card")) {
            ApexUi::CardHeader(IconId::Layers, "Stories", "Lamp light between the floors of a house", nullptr, nullptr);
            ApexUi::CardDivider();
            if (drawUpperFloorRow) drawUpperFloorRow();
            Edit([] {
                bool changed = ApexUi::SwitchRow("Outdoor light between floors", &g_levelShare, "Outdoor lamps light the floors above and below, with no hard edge", true);
                if (!g_levelShare) ApexUi::IconNote(ApexUi::IconId::Info, "Needs \"Outdoor light between floors\"");
                ImGui::BeginDisabled(!g_levelShare);

                changed |= ApexUi::SwitchRow("Indoor light between floors", &g_indoorShare, "Lamps inside shine through stairwells and open floors", true);
                // 30/09 (user): the game solves rooms one after the other (after a load, a change or a lamp switched), so
                // a room can show its old light for a moment; switching floors solves the rooms shown again
                if (g_indoorShare) ApexUi::IconNote(ApexUi::IconId::Info, "Rooms may take a few seconds to update; if one lags, change floors");
                if (ApexUi::BeginAdvanced("StoryDetail", "Floor detail")) {
                    changed |= ApexUi::SwitchRow("Seamless walls between floors", &g_wallAlign, "Walls above and below the floor line meet with no step in the light", true);
                    changed |= ApexUi::SwitchRow("Every floor in full detail", &g_allFloors, "Changing floors keeps the light; entering a lot takes a little longer", true);
                    ApexUi::EndAdvanced();
                }
                ImGui::EndDisabled();
                return changed;
            });
        }
        ApexUi::EndCard();
        ImGui::PopID();
    }

    void DrawGroundCard() {
        using ApexUi::IconId;
        ImGui::PushID("NightGround");
        if (ApexUi::BeginCard("##Card")) {
            ApexUi::CardHeader(IconId::LandPlot, "Ground & Lots", "Lamp light on grass, streets and lots", nullptr, nullptr);
            ApexUi::CardDivider();
            Edit([] {
                bool changed = ApexUi::SwitchRow("Street lamps light lots", &g_bridge, "Street lamp light flows onto lots with no hard edge", true);
                changed |= ApexUi::SwitchRow("Lot lamps light the street", &g_lotLamps, "Outdoor lot lamps also light the grass and street nearby", true);
                changed |= ApexUi::SwitchRow("Smooth ground light", &g_smoothMaps, "Soft lamp light on the ground, without blocky steps or specks", true);
                return changed;
            });
            // When the ground light is rebuilt (lamp changes are always followed; this is the rebuild at dusk)
            if (ApexUi::BeginAdvanced("Updates##NightGround", "Updates")) {
                Edit([] {
                    bool changed = ApexUi::SwitchRow("Update at dusk", &g_autoDusk, "When night falls, the ground light is rebuilt with every lamp on", true);
                    ImGui::BeginDisabled(!g_autoDusk);
                    ApexUi::SliderOptions o;
                    o.format = "%.1f s";
                    o.tooltip = "Waits after dusk so every lamp has switched on first";
                    o.defaultValue = 2.0f;
                    changed |= ApexUi::Slider("Delay after dusk", &g_delaySec, 0.5f, 10.0f, o);
                    ImGui::EndDisabled();
                    return changed;
                });
                ApexUi::EndAdvanced();
            }
        }
        ApexUi::EndCard();
        if (ApexUi::BeginCard("##GroundIntensity")) {
            ApexUi::CardHeader(IconId::SlidersHorizontal, "Ground intensity", "Balance surfaces and lamp brightness", nullptr, nullptr);
            ApexUi::CardDivider();
            Edit([] {
                // The ground and road gains are applied in the draws of "Street lamps light lots"
                if (!g_bridge) ApexUi::IconNote(ApexUi::IconId::Info, "Needs \"Street lamps light lots\"");
                ImGui::BeginDisabled(!g_bridge);
                bool changed = ApexUi::SliderPercent("Ground brightness", &g_groundBrightness, 0.25f, 3.0f, "Lamp light on grass, lots and patios", 1.0f);
                changed |= ApexUi::SliderPercent("Roads and sidewalks", &g_roadBrightness, 0.25f, 3.0f, "Balance roads against the surrounding ground", 1.0f);
                ImGui::EndDisabled();
                // In the terrain light bake: the ground is rebuilt once the slider is let go
                const char* bakeTip = g_bakeGainInstalled ? nullptr : "Not available on this game version";
                ImGui::BeginDisabled(!g_bakeGainInstalled);
                changed |= ApexUi::SliderPercent("Street lamp brightness", &g_streetLampGain, 0.25f, 3.0f, bakeTip ? bakeTip : "Ground light cast by street lamps", 1.0f);
                changed |= ApexUi::SliderPercent("Lot lamp brightness", &g_lotLampGain, 0.25f, 3.0f, bakeTip ? bakeTip : "Ground light cast by lamps on lots", 1.0f);
                ImGui::EndDisabled();
                return changed;
            });

        }
        ApexUi::EndCard();
        ImGui::PopID();
    }

    // Lighting > Objects: external objects, connected pieces and indoor surfaces.
    void DrawObjectsCard() {
        using ApexUi::IconId;
        ImGui::PushID("NightObjects");
        if (ApexUi::BeginCard("##Card")) {
            ApexUi::CardHeader(IconId::Armchair, "Objects", "Lamp light on plants and outdoor furniture", nullptr, nullptr);
            ApexUi::CardDivider();
            Edit([] {
                bool changed = ApexUi::SwitchRow("Lamps light objects", &g_objLamps, "Outdoor objects get lamp light, even in the shade of walls", true);
                if (g_objLamps)
                    changed |= ApexUi::SliderPercent("Brightness##Objects", &g_objStrength, 0.25f, 3.0f, "Raise it if objects look dark next to lamps", 1.0f);
                ImGui::BeginDisabled(!g_objLamps);
                ApexUi::SetNextRowBadge(kReloadBadge, kReloadTip); // the game builds these pieces' light when a world loads
                changed |= ApexUi::SwitchRow("Light stairs, railings, columns", &g_objAll, "Pieces the game leaves unlit", true);
                ImGui::EndDisabled();

                return changed;
            });
        }
        ApexUi::EndCard();
        if (ApexUi::BeginCard("##Pieces")) {
            ApexUi::CardHeader(IconId::Fence, "Doors, counters and fences", "Match connected pieces to the surrounding light", nullptr, nullptr);
            ApexUi::CardDivider();
            Edit([] {
                bool changed = false;
                // These read the ground light of two Ground & Lots options (lot light bridge + smoothed maps)
                const bool groundLight = g_bridge && g_smoothMaps;
                if (!groundLight) {
                    const bool both = !g_bridge && !g_smoothMaps;
                    ApexUi::IconNote(IconId::Info, both ? "Needs \"Street lamps light lots\" and \"Smooth ground light\" (Ground tab)"
                                                        : !g_bridge ? "Needs \"Street lamps light lots\" (Ground tab)" : "Needs \"Smooth ground light\" (Ground tab)");
                    if (ApexUi::IconTextButton(both ? "Turn both on##GroundLight" : "Turn it on##GroundLight", IconId::LandPlot, nullptr, ApexUi::ButtonKind::Primary)) {
                        ApexUi::ReportChange(both ? "Ground light turned on" : !g_bridge ? "Street lamps light lots turned on" : "Smooth ground light turned on");
                        g_bridge = true;
                        g_smoothMaps = true;
                        changed = true;
                    }
                }
                ImGui::BeginDisabled(!groundLight);
                changed |= ApexUi::SwitchRow("Doors and windows stay lit", &g_objPixel, "A front door is never darker than the wall around it", true);
                changed |= ApexUi::SwitchRow("Seamless light on pieces", &g_objPixelLights, "Counters and modular pieces outside show no color steps", true);
                if (g_objPixelLights)
                    changed |= ApexUi::SliderPercent("Seamless light brightness", &g_objPixelLightStrength, 0.25f, 3.0f, "Intensity on counters and modular pieces", 1.0f);
                changed |= ApexUi::SwitchRow("Fences and stairs catch light", &g_fenceGround, "Fences, posts, stairs and their snow match the lit ground", true);
                if (g_fenceGround)
                    changed |= ApexUi::SliderPercent("Fence brightness", &g_fenceGroundStrength, 0.25f, 2.0f, "Balance fences against the surrounding ground", 1.0f);
                ImGui::EndDisabled();

                return changed;
            });
        }
        ApexUi::EndCard();
        if (ApexUi::BeginCard("##IndoorObjects")) {
            ApexUi::CardHeader(IconId::Lightbulb, "Indoor objects", "Furniture and stairs inside rooms", nullptr, nullptr);
            ApexUi::CardDivider();
            Edit([] {
                bool changed = false;
                ApexUi::SetNextRowBadge("Experimental", "Still being tested: if anything looks wrong or the game crashes, turn it off");
                changed |= ApexUi::SwitchRow("Smooth indoor light", &g_edgePad, "Light changes smoothly on stairs, curtains and furniture; no dark sides", true);
                return changed;
            });
        }
        ApexUi::EndCard();
        ImGui::PopID();
    }

    // Lighting > Buildings
    void DrawBuildingsCard() {
        ImGui::PushID("NightBuildings");
        if (ApexUi::BeginCard("##Card")) {
            ApexUi::CardHeader(ApexUi::IconId::House, "Buildings", "Outside walls and roofs", nullptr, nullptr);
            ApexUi::CardDivider();
            Edit([] {
                ApexUi::GroupLabel("WALLS");
                bool changed = ApexUi::SwitchRow("Lamps light walls", &g_walls, "Outside walls near lamps get brighter; off keeps the game's dim walls", true);
                if (g_walls)
                    changed |= ApexUi::SliderPercent("Brightness##Walls", &g_wallStrength, 0.25f, 4.0f, "Intensity of lamp light on outside walls, by day and night", 2.0f);
                ApexUi::GroupLabel("ROOFS");
                changed |= ApexUi::SwitchRow("Lamps light roofs", &g_roofs, "Roofs no longer stay black at night; softer roof shadows too", true);
                if (g_roofs)
                    changed |= ApexUi::SliderPercent("Brightness##Roofs", &g_roofStrengthSetting, 0.05f, 2.0f, "Intensity of lamp light on roofs", 0.6f);
                return changed;
            });
        }
        ApexUi::EndCard();
        ImGui::PopID();
    }

    // Lighting > Buildings: rooms with every lamp off (2026-09-29, user: the game leaves them "super blue", S3SS's switch
    // makes them far too dark; a control in between)
    void DrawRoomsCard() {
        ImGui::PushID("NightRooms");
        if (ApexUi::BeginCard("##Card")) {
            ApexUi::CardHeader(ApexUi::IconId::Moon, "Rooms at Night", "The soft background light inside rooms", nullptr, nullptr);
            ApexUi::CardDivider();
            Edit([] {
                bool changed = ApexUi::SwitchRow("Adjust the background light", &g_unlitOn, "Set the ambient glow indoors, with lamps on or off", true);
                if (g_unlitOn) {
                    changed |= ApexUi::SliderPercent("Brightness##Unlit", &g_unlitLight, 0.1f, 0.8f, "How bright that background light is, on walls and furniture", 0.35f);
                    changed |= ApexUi::SliderPercent("Blue tint##Unlit", &g_unlitBlue, 0.0f, 1.0f, "0% is neutral grey, 100% is the game's blue, on walls and furniture", 0.0f);
                }
                return changed;
            });
            // S3SS's saved room colour: Rooms at Night already uses the game's blue in its place; removing it is on the Attention page
            if (g_unlitOn && S3SSDetect::SavedRoomAmbientOverride())
                ApexUi::IconNote(ApexUi::IconId::Puzzle, "S3SS saves its own room color; Rooms at Night replaces it (see Attention)");
            // 30/09 (user: "a button to recalculate these lights when they bug"): the "Refresh the lighting" shortcut as a button
            const std::string key = ApexConfig::KeyChordText(Hotkeys::Key(Hotkeys::Action::Refresh));
            const float gap = ImGui::GetStyle().ItemSpacing.x;
            if (ApexUi::BeginControlRow("Refresh the lighting", "If a room or its furniture looks wrong: rooms, lots and objects light again",
                                        ApexUi::ChipSize(key.c_str()).x + gap + ApexUi::ButtonWidth("Refresh##RefreshLighting", true))) {
                ApexUi::Chip(key.c_str());
                ImGui::SameLine();
                if (ApexUi::IconTextButton("Refresh##RefreshLighting", ApexUi::IconId::Lightbulb, "The same as its shortcut")) NightLighting::RefreshAll("button");
                ApexUi::EndControlRow();
            }
        }
        ApexUi::EndCard();
        ImGui::PopID();
    }

    // Water & Snow > Water (the lamp glow; the shore reflection is the menu's Water Reflections card)
    void DrawWaterCard() {
        ImGui::PushID("NightWater");
        if (ApexUi::BeginCard("##Card")) {
            ApexUi::CardHeader(ApexUi::IconId::WavesHorizontal, "Lamp Glow", "Lamp light on ponds at night", nullptr, nullptr);
            ApexUi::CardDivider();
            Edit([] {
                bool changed = ApexUi::SwitchRow("Lamps glow on ponds", &g_water, "Ponds glow and sparkle near lamps at night", true);
                if (g_water)
                    changed |= ApexUi::SliderPercent("Glow brightness", &g_waterStrengthSetting, 0.1f, 0.4f, "Intensity of lamp glow and sparkles on ponds", 0.4f);
                return changed;
            });
        }
        ApexUi::EndCard();
        ImGui::PopID();
    }

    // Water & Snow > Snow
    void DrawSnowCard() {
        using ApexUi::IconId;
        ImGui::PushID("NightSnow");
        if (ApexUi::BeginCard("##Card")) {
            ApexUi::CardHeader(IconId::Snowflake, "Snow", "Footsteps reveal the sidewalk beneath the snow", nullptr, nullptr);
            ApexUi::CardDivider();
            Edit([] {
                ImGui::BeginDisabled(!g_bridge);
                bool changed = ApexUi::SliderPercent("Sidewalk visibility", &g_sidewalkClear, 0.0f, 1.0f,
                    "How much sidewalk shows where Sims have walked; 0% keeps the game's original look", 0.5f);
                ImGui::EndDisabled();
                if (!g_bridge) {
                    ApexUi::IconNote(IconId::Info, "Needs \"Street lamps light lots\" (Lighting page, Ground tab)");
                    if (ApexUi::IconTextButton("Turn it on##StreetLamps", IconId::LandPlot, nullptr, ApexUi::ButtonKind::Primary)) {
                        ApexUi::ReportChange("Street lamps light lots turned on");
                        g_bridge = true;
                        changed = true;
                    }
                }
                return changed;
            });
        }
        ApexUi::EndCard();
        ImGui::PopID();
    }

    // Lighting > Lamps, under the Night Lights card: the reset button (the rows that need a reload say so themselves)
    // The generic per-feature controls: the lamp colour (the menu draws the cards itself)
    void RenderCustomUI() override {
        SAFE_IMGUI_BEGIN();
        DrawLightingBalance();
    }

    // Development build, Developer page > Lighting: status lines, diagnostics, the census and every individual option.
    void RenderDeveloperUI() override {
        SAFE_IMGUI_BEGIN();
        using ApexUi::IconId;
        if (ApexUi::BeginCard("##CollectLighting")) {
            ApexUi::CardHeader(IconId::Camera, "Collect lighting evidence", "Capture a state before changing the scene", nullptr, nullptr);
            ApexUi::CardDivider();
        if (ApexUi::TextButton("Save light diagnostics")) LightDiag::RequestDump();
        ImGui::SameLine();
        ImGui::TextDisabled("(or %s)", ApexConfig::KeyChordText(Hotkeys::Key(Hotkeys::Action::Diagnostics)).c_str());
        bool storySamples = LevelLightShare::DiagArmed();
        if (ApexUi::Checkbox("Record story light samples for the diagnostics", &storySamples)) LevelLightShare::SetDiagArmed(storySamples);
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("Records, in every lot light solve, the points near each lamp of the active lot with the game's wall test and ours\n"
                              "(the \"stories\" section of the diagnostics). Costs time in every solve, so it is off until checked or until the\n"
                              "first diagnostics of the session are saved.");
        {
            // Census: which lamp-lit draws no fix claimed
            bool falseColor = LotLightBridge::FalseColor();
            if (ApexUi::Checkbox("False colour: magenta = gets lamp light but no fix claimed it", &falseColor)) LotLightBridge::SetFalseColor(falseColor);
            if (ApexUi::TextButton("Census: write ApexRadiance_Censo.txt")) LotLightBridge::RequestCensus();
            ImGui::SameLine();
            ImGui::TextDisabled("(%s)", LotLightBridge::CensusStatus().c_str());
        }
        }
        ApexUi::EndCard();
        if (ApexUi::BeginCard("##CompareLighting")) {
            ApexUi::CardHeader(IconId::Columns2, "Compare lighting paths", "Change one option at a time, then compare the same scene", nullptr, nullptr);
            ApexUi::CardDivider();
        {
            // Rooms keep their light when their lamps did not change (lamp_mark_filter.cpp; 30/09, on by default, A/B here)
            bool keep = LampMarkFilter::Enabled();
            if (ApexUi::Checkbox("Rooms keep their light when their lamps did not change (floor switches)", &keep)) LampMarkFilter::SetEnabled(keep);
            ImGui::TextWrapped("%s", LampMarkFilter::Status().c_str());
        }
        if (ApexUi::Checkbox("Soft lot edges (A/B: off = plain max of lot and ground light)", &g_softLotEdges)) NotifySettingChanged();
        if (ApexUi::Checkbox("Smooth the ground light maps on the GPU (A/B: off = CPU worker)", &g_smoothMapsGpu)) NotifySettingChanged();
        ImGui::SameLine();
        if (ApexUi::TextButton("Compare GPU vs CPU (one chunk)")) LightmapSmooth::RequestCompare();
        if (ApexUi::BeginAdvanced("WaterHighlights", "Water highlights")) {
        if (ApexUi::SwitchRow("Stabilize lamp sparkles on water", &g_waterFilter, "Filters tiny highlights without temporal smoothing. Turn off to compare the original", true)) NotifySettingChanged();
        if (ApexUi::SwitchRow("Preserve bright lamp colors on water", &g_waterColorCompression, "Softens excessive lamp brightness while keeping its color. Turn off to compare the original", true)) NotifySettingChanged();
            ApexUi::EndAdvanced();
        }
        }
        ApexUi::EndCard();
        NightLighting::DrawRefreshCard();
        if (ApexUi::BeginCard("##InspectLighting")) {
            ApexUi::CardHeader(IconId::Scan, "Inspect lighting state", "Open the data involved in your test", nullptr, nullptr);
            ApexUi::CardDivider();
        ApexUi::MutedText(g_status.c_str());
        if (ApexUi::BeginAdvanced("SurfaceState", "Surface and provider state")) {
        ImGui::TextWrapped("Diagnostics: %s", LightDiag::Status().c_str());
        ImGui::TextWrapped("Street lamps in lots: %s", LotLightBridge::Status().c_str());
        ImGui::TextWrapped("Soft lot edges: %s", LotLightBridge::LotEdgeStatus().c_str());
        ImGui::TextWrapped("Objects: %s", ObjectLightBridge::Status().c_str());
        ImGui::TextWrapped("Shadow: %s", LotLightBridge::ObjectStatus().c_str());
        ImGui::TextWrapped("Walls: %s", LotLightBridge::WallStatus().c_str());
        ImGui::TextWrapped("Ground brightness: %s", LotLightBridge::GroundBrightnessStatus().c_str());
        ImGui::TextWrapped("Room light map edges: %s", RoomMapPadding::Status().c_str());
        ImGui::TextWrapped("Smooth indoor light: %s", LotLightBridge::IndoorSmoothStatus().c_str());
        ImGui::TextWrapped("Terrain bake: %s | moonlight: %s", g_bakeGainInstalled ? std::format("street lamps x{:.2f}, lot lamps x{:.2f}", g_bakeStreetMul[0], g_bakeLotMul[0]).c_str() : "not installed",
                           g_sunlightBase < 0.0f ? (kSunlightScale ? "waiting for a world" : "not available")
                                                 : std::format("sunlight scale {:.3f} (base {:.3f}, moonlight x{:.2f})", g_moonWritten, g_sunlightBase, g_moonlight).c_str());
        ImGui::TextWrapped("Roofs: %s", LotLightBridge::RoofStatus().c_str());
        ImGui::TextWrapped("Water: %s", LotLightBridge::WaterStatus().c_str());
        ImGui::TextWrapped("Smoothed light map: %s", LightmapSmooth::Status().c_str());
        ImGui::TextDisabled("GPU vs CPU: %s", LightmapSmooth::CompareStatus().c_str());
        ImGui::TextWrapped("Lamp colour: %s", ObjectLightBridge::LampColourStatus().c_str());
        ImGui::TextWrapped("Stories: %s", LevelLightShare::Status().c_str());
        ImGui::TextWrapped("Rooms at night: %s", UnlitRooms::Status().c_str());
            ApexUi::EndAdvanced();
        }
        if (ApexUi::BeginAdvanced("RebuildEvents", "Rebuild events and terrain tests")) {
        ImGui::TextWrapped("Last event: %s", g_lastEvent.c_str());
        ImGui::Text("Night level: %.2f | countdown: %d / %d", g_level, g_counter38, g_counter3C);
        ImGui::Text("Terrain: armed %d | rebuilt %d | last: %s", g_kicks.load(), g_rebuilds.load(), g_lastTiming.c_str());
        ImGui::TextWrapped("World load: %s | night level crossings: up %d, down %d", g_loadKickPending ? (g_live ? "live, waiting for a steady night level" : "waiting for the world to be drawn") : g_loadInfo.c_str(),
                           g_crossUp, g_crossDown);
        ImGui::TextWrapped("Lamp changes: %s", LotLightBridge::LotLampStatus().c_str());
        {
            const auto now = Clock::now();
            const char* wait = !g_editKickPending ? "none"
                               : g_editWait == EditWait::Camera ? "waiting for the camera to stop"
                               : g_editWait == EditWait::Rate ? "rate-limited (automatic: 30 s after the last rebuild)"
                               : g_editWait == EditWait::Interval ? "rate-limited (3 s between lamp rebuilds)"
                               : g_editWait == EditWait::Snapshot ? "waiting for the snapshot of the last rebuild"
                               : g_editWait == EditWait::Relight  ? "waiting for the terrain relight in progress"
                               : g_editWait == EditWait::LampRate ? "rate-limited (a lamp relit locally less than 5 s ago)"
                                                                   : "quiet time (250 ms)";
            const std::string snap = g_haveBaked ? std::format("{} lamps on {} lots, taken {:.0f} s ago", g_baked.lamps.size(), g_baked.lots.size(),
                                                               std::chrono::duration<double>(now - g_bakedAt).count())
                                                 : std::string(g_bakedDue ? "being taken" : "none yet");
            ImGui::TextWrapped("Lamp change decisions: rebuilt %d user-driven / %d automatic | skipped: terrain rebuilt after the change %d, covered by a rebuild %d, no bake "
                               "change %d | deferred: camera %d, rate-limited %d | pending: %s | camera: %s | last rebuild's lamps: %s | last: %s",
                               g_decRebuiltUser, g_decRebuiltAuto, g_decSkipGame, g_decCovered, g_decSkipSame, g_decDeferCamera, g_decDeferRate,
                               g_editKickPending ? std::format("{} ({}), {}", g_editReason, EditKind(), wait).c_str() : "none", CameraText(now).c_str(), snap.c_str(),
                               g_lastEditOutcome.c_str());
        }
        ImGui::Text("Chunk re-render notices: %d (%s)", g_chunkRenders.load(), g_chunkHookInstalled ? "hooked at 0xC8504C" : "not hooked: hashing only");
        if (ApexUi::Checkbox("Relight only nearby terrain (lamp changes re-render only the chunks under the changed lamps)", &g_localRelight)) NotifySettingChanged();
        if (ApexUi::Checkbox("Paced terrain sweep (dusk and lamp-change rebuilds re-render one chunk at a time, nearest first)", &g_pacedSweep)) NotifySettingChanged();
        ImGui::TextWrapped("Local terrain relight: relit locally %d user-driven / %d automatic, done %d, refused %d (last: %s), failures %d | paced sweeps: %d started, %d done%s | "
                           "last: %s",
                           g_decLocalUser, g_decLocalAuto, g_localDone, g_decLocalRefused, g_lastLocalRefusal.c_str(), g_localFailures, g_sweepsStarted, g_sweepsDone,
                           g_sweepId ? std::format(" (one running: {})", g_sweepReason).c_str() : "", g_lastLocal.c_str());
        ImGui::TextWrapped("Terrain chunks: %s", ChunkRelight::Status().c_str());
        ImGui::TextWrapped("Lots: %s (times: %d, stories: %d)", g_lastLotRelight.c_str(), g_lotRelights.load(), g_roomsQueued.load());
        ImGui::Text("Street lamps counted as lit: %ld", static_cast<long>(g_forcedLampUses));
        ImGui::Text("Lot lamps: armed %d | on the ground %d | off %d", g_lotLampArms.load(), g_lotLampsBaked.load(), g_lotLampsSkippedOff.load());
            ApexUi::EndAdvanced();
        }
        if (ApexUi::BeginAdvanced("ProbeTextures", "Light probe textures")) {
        LightProbe::RenderUI();
            ApexUi::EndAdvanced();
        }
        if (ApexUi::BeginAdvanced("IndividualTests", "Individual options (for tests)")) {
            // The generic list only stores the value: install or remove the parts that are toggled live.
            const bool shareBefore = g_levelShare, objBefore = g_objLamps, bridgeBefore = g_bridge, objPixelBefore = g_objPixel;
            ApexPatch::RenderCustomUI();
            ApplyLive(bridgeBefore, objBefore, shareBefore, objPixelBefore);
            ApexUi::EndAdvanced();
        }
        }
        ApexUi::EndCard();
    }

    float ShoreReflection() const { return g_waterReflSetting; }
    void SetShoreReflection(float strength) {
        g_waterReflSetting = strength < 0.0f ? 0.0f : (strength > 3.0f ? 3.0f : strength);
        NotifySettingChanged();
    }
};

static std::vector<std::string> NightRemakeDetails() {
    if (kPublicBuild)
        return {"Outdoor lot lamps join the terrain light bake (0xC29626) and arm its rebuild (0x6B6516/0x6B60D3/0x6B6618); street lamps count as lit in lot solves (0x6BE18C).",
                "Outdoor lights are shared between the stories of a house, with the game's own wall occlusion.",
                "Lamp light on objects, roofs, water and snow is added in the game's own shaders while they draw. At dusk the terrain and lot lighting are rebuilt through the game's own paths."};
    else
        return {"All lots use high lighting quality (0xADB66F/0xADB888); the lot light pass never binds the rebuilt terrain lightmap (0xC7F87D).",
                "Outdoor lot lamps join the terrain light bake (0xC29626) and arm its rebuild (0x6B6516/0x6B60D3/0x6B6618); street lamps count as lit in lot solves (0x6BE18C).",
                "At dusk the terrain and lot lighting are rebuilt through the game's own paths. Diagnostics: Ctrl+Shift+F8."};
}

APEX_REGISTER_FEATURE(NightTerrainRelightPatch, {.displayName = "Night Lights",
                                             .description = "At night, street lamps and lot lamps light the ground, objects, fences, walls, roofs, ponds and "
                                                            "snow around them with smooth, warm light and no hard edges at lot borders. Part of "
                                                            APEX_PRODUCT_NAME ". Credits: @loinyx",
                                             .category = "Graphics",
                                             .experimental = true,
                                             .enabledByDefault = true,
                                             .supportedVersions = VERSION_STEAM,
                                             .technicalDetails = NightRemakeDetails(),
                                             .gameCodeGroup = "NightLights"})

bool NightLighting::MenuNightLevel(float& level) {
    const float v = g_menuLevel.load();
    if (v < 0.0f) return false;
    level = v;
    return true;
}

// ---- menu pieces (night_lighting.h); nothing when the feature was not created ----
namespace {
NightTerrainRelightPatch* MenuPatch() { return ImGui::GetCurrentContext() ? NightTerrainRelightPatch::g_self : nullptr; }
} // namespace

void NightLighting::DrawLightingBalance() {
    if (auto* p = MenuPatch()) p->DrawLightingBalance();
}
void NightLighting::DrawRefreshCard() {
    if (!MenuPatch()) return;
    ImGui::PushID("RefreshLightingCard");
    if (ApexUi::BeginCard("##Card")) {
        ApexUi::CardHeader(ApexUi::IconId::RotateCcw, "Refresh lighting", "Recalculate lighting if something looks wrong", nullptr, nullptr);
        ApexUi::CardDivider();
        ImGui::TextUnformatted(I18n::Tr("Terrain and lots"));
        ApexUi::MutedText("Use when light on the ground or a lot looks incorrect or has not updated");
        ApexUi::Gap(ApexUi::kSpace3);
        ApexUi::ControlSizeScope controls(ApexUi::ControlSize::Compact);
        const float gap = ImGui::GetStyle().ItemSpacing.x;
        const float terrainW = ApexUi::ButtonWidth("Refresh terrain", true), lotW = ApexUi::ButtonWidth("Refresh lots", true), lightsW = ApexUi::ButtonWidth("Refresh lights", true);
        const bool inlineActions = terrainW + lotW + lightsW + 2.0f * gap <= ImGui::GetContentRegionAvail().x;
        if (inlineActions) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - terrainW - lotW - lightsW - 2.0f * gap);
        ImGui::BeginDisabled(g_menuLevel.load() < 0.0f || !g_live);
        if (ApexUi::IconTextButton("Refresh terrain", ApexUi::IconId::LandPlot)) g_kickRequested = true;
        if (inlineActions) ImGui::SameLine();
        if (ApexUi::IconTextButton("Refresh lots", ApexUi::IconId::House)) g_relightLotsRequested = true;
        if (inlineActions) ImGui::SameLine();
        if (ApexUi::IconTextButton("Refresh lights", ApexUi::IconId::Lightbulb)) NightLighting::RefreshAll("button");
        ImGui::EndDisabled();
    }
    ApexUi::EndCard();
    ImGui::PopID();
}
void NightLighting::DrawGroundCard() {
    if (auto* p = MenuPatch()) p->DrawGroundCard();
}
void NightLighting::DrawStoriesCard(void (*drawUpperFloorRow)()) {
    if (auto* p = MenuPatch()) p->DrawStoriesCard(drawUpperFloorRow);
}
void NightLighting::DrawObjectsCard() {
    if (auto* p = MenuPatch()) p->DrawObjectsCard();
}
void NightLighting::DrawBuildingsCard() {
    if (auto* p = MenuPatch()) p->DrawBuildingsCard();
}
void NightLighting::DrawRoomsCard() {
    if (auto* p = MenuPatch()) p->DrawRoomsCard();
}
void NightLighting::DrawWaterCard() {
    if (auto* p = MenuPatch()) p->DrawWaterCard();
}
void NightLighting::DrawSnowCard() {
    if (auto* p = MenuPatch()) p->DrawSnowCard();
}

void NightLighting::DrawDeveloper() {
    if (auto* p = MenuPatch()) p->RenderDeveloperUI();
}
float NightLighting::ShoreReflection() { return g_waterReflSetting; }
void NightLighting::SetShoreReflection(float strength) {
    if (auto* p = NightTerrainRelightPatch::g_self) p->SetShoreReflection(strength);
}

// The "Refresh the lighting" shortcut (Hotkeys): what the Developer buttons "Rebuild terrain light now" and "Relight lots
// now" do, plus every room and the object rigs (turning Night Lighting off and on did it before). Only while it runs
// (g_menuLevel is -1 while it is off or no world is loaded).
void NightLighting::RefreshAll(const char* why, bool terrain) {
    if (g_menuLevel.load() < 0.0f) return;
    if (terrain) g_kickRequested = true;
    g_relightLotsRequested = true;
    LevelLightShare::RelightAllRooms(std::format("Refresh the lighting ({})", why).c_str());
    ObjectLightBridge::RequestRigRefresh();
    UnlitRooms::RigsAgainIn(2000); // and once the rooms are solved (their lights are what the rigs gather)
    LOG_INFO(std::format("[NightTerrainRelight] Refresh the lighting ({}): {}lots and rooms light again", why, terrain ? "terrain, " : ""));
}
void NightLighting::RefreshSoon() { RequestAutoRefresh(); }

// The world is on screen (its terrain is drawn, or the fallback after the world change); false during load screens
bool NightLighting::WorldLive() { return g_live; }
