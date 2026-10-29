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

#include "tree_light_rig.h"
#include "patch_base.h"
#include "apex_version.h"
#include "memory_patch.h"
#include "game_addresses.h"
#include "apex_log.h"
#include "d3d9_hooks.h"
#include "d3d9_bootstrap.h"
#include "depth_share.h"
#include "light_diag.h"
#include "daylight_probe.h"
#include "recorder.h"
#include "hotkeys.h"
#include "light_probe.h"
#include "screen_watch.h"
#include "lot_light_bridge.h"
#include "terrain_bake_rule.h"
#include "night_level.h"
#include "bloom_alpha_probe.h"
#include "lightmap_smooth.h"
#include "terrain_chunk_relight.h"
#include "terrain_lighting_policy.h"
#include "world_session.h"
#include "render_callbacks.h"
#include "object_light_bridge.h"
#include "level_light_share.h"
#include "room_map_dither.h"
#include "room_ambient_policy.h"
#include "unlit_rooms.h"
#include "probe_neutral.h"
#include "lamp_mark_filter.h"
#include "light_detail.h"
#include "rig_tracker.h"
#include "room_map_padding.h"
#include "build_flavor.h"
#include "night_lighting.h"
#include "lighting_restart_policy.h"
#include "lot_lighting_motion.h"
#include "outdoor_delta.h"
#include "rt_outdoor_lamps.h"
#include "terrain_stamps.h"
#include "terrain_bake_cone.h"
#include "terrain_bake_hlsl.h"
#include "shader_cache.h"
#include "imgui.h"
#include "ui/widgets.h"
#include "ui/i18n.h"
#include "overlay.h"
#include "hook_guard.h"
#include "load_timing.h"
#include "drag_lite.h"
#include "tester_toggles.h"
#include <windows.h>
#include <algorithm>
#include <map>
#include <atomic>
#include <chrono>
#include <climits>
#include <cmath>
#include <cstring>
#include <d3dcompiler.h>
#include <format>
#include <mutex>
#include <string>
#include <unordered_set>
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
void GiveBackBakeRows();
void __fastcall ChunkRenderThunk(void* terrain, void* /*edx*/, BYTE* chunk, int force) {
    LARGE_INTEGER t0, t1;
    QueryPerformanceCounter(&t0);
    reinterpret_cast<ChunkRender_t>(kChunkRenderFn)(terrain, chunk, static_cast<char>(force));
    GiveBackBakeRows();
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
bool g_streetLampsLit = true;
bool g_relightLots = true;
bool g_bridge = true;
bool g_objLamps = true;
float g_objStrength = 0.75f;
bool g_objAll = true;
bool g_smoothMaps = true;
bool g_smoothMapsGpu = true; // developer A/B: smoothing on the GPU (default) or on the CPU worker (dev-only setting)
bool g_softLotEdges = true; // lot grass feathers to the terrain light within 3 m of the lot edge (developer A/B, dev-only setting)
// Lamp lighting (Performance page): 0 Classic (the game's room solves; a dragged lamp relit after its release and an edit's stories shown together, lamp-drags.md), 1 Instant (Real-time outdoor
// lamps, features/rt_outdoor_lamps.h, on the kept parts of Quick outdoor lamp edits, features/outdoor_delta.h, which it needs)
constexpr int kLampClassic = 0, kLampInstant = 1;
int g_lampMode = kLampInstant; // (14/10: Instant by default; a saved lampLightingMode keeps its choice)
bool g_outdoorDeltaCheck = false; // the delta's self-check (developer)
float g_sidewalkClear = 0.5f;
float g_lampTint = 1.0f;
bool g_fenceGround = true;
bool g_levelShare = true;
bool g_indoorShare = true;       // indoor lamps light the story above or below through stair openings (LevelLightShare::SetIndoor)
bool g_wallAlign = true;         // walls lit where their light is drawn: no step at the floor line (LevelLightShare::SetWallAlign)
bool g_foundationWalls = true; // (07/10) wall lamp light at the height the game draws the wall (LevelLightShare::SetFoundationWalls)
bool g_allFloors = true;        // every floor of the active lot in full lighting detail (LevelLightShare::SetAllFloors)
bool g_floorWalls = true;       // walls block lamp light on outdoor floors (LevelLightShare::SetFloorWalls)
bool g_realisticOpenings = kRealisticOpeningsDefault; // (08/10, experimental) walled yards lit only through doors, windows and over walls (LevelLightShare::SetRealisticOpenings)
bool g_outdoorOpenings = true; // (2.12.4) roofed rooms take room 0's lamps through their doors, windows and arches (LevelLightShare::SetOutdoorThroughOpenings)
bool g_doorsBlock = true;       // (2.12.4) doors and gates stay closed for lamp light; arches and windows open (LevelLightShare::SetDoorsBlockLight)
bool g_indoorOpenings = true;   // (2.12.4) lit rooms shine out through their openings and into the next room (LevelLightShare::SetIndoorThroughOpenings)
bool g_indoorGround = true;     // (2.15.3) that light adds to the outdoor light on story 0's ground (LevelLightShare::SetIndoorLightOnGround)
float g_shadowSoftness = 0.15f; // (2.12.4) a lamp's radius in metres: the penumbra past openings, wall ends and tops (LevelLightShare::SetShadowSoftness)
bool g_objectWalls = true;      // walls block lamp light on outdoor objects and trees (LevelLightShare::SetObjectWalls)
bool g_groundShade = true;      // (14/10) walls shade lamp light on lot ground, paths, decks, curbs and low fences (LevelLightShare::SetGroundShade)
bool g_smoothRoomMaps = true;   // (2.12.4) room light maps: soft light cut, dither, B-spline reads (LevelLightShare::SetSoftLightCut, RoomMapDither, ShaderPatches)
int g_lightDetail = 0;          // lighting texels per metre on walls and floors: 0 the game's, 1 twice (LightDetail; read at startup)
bool g_unlitOn = true;           // rooms with every lamp off: Apex's light instead of the game's blue glow (UnlitRooms)
float g_unlitLight = 0.20f;      // how much of the game's unlit-room light stays
float g_unlitBlue = 0.0f;        // how much of its blue tint (0 = grey)
bool g_objPixel = true;
bool g_objPixelLights = true;          // outdoor rig objects: world lamps per pixel (seamless modular pieces)
float g_objPixelLightStrength = 0.75f;
bool g_windowOutdoor = true;           // (on by default) windows lit by the room take outdoor light on their outer side (LotLightBridge::SetWindowOutdoor)
bool g_neutralProbe = true;            // (07/10) at night the game's sky light keeps its brightness, loses the lamps' colour (ProbeNeutral)
float g_neutralProbeStrength = 1.0f;
float g_fenceGroundStrength = 0.75f;
bool g_walls = true;         // outdoor walls receive baked lamp light by day and night; off keeps the native draw
float g_wallStrength = 0.30f; // multiplier of baked wall lamp RGB, independent of the enabled state
// (12/10) "Light behind balconies" (luzSobSacadas, forcaSobSacadas) is gone: a placed floor blocks a lamp below it
// (LevelLightShare, lamp_visibility.h). Old configs, profiles and presets that still hold the keys load as before: only the
// registered settings are read (ApexPatch::LoadFromToml), so the two keys are skipped.
bool g_realisticReach = true;     // (12/10) lamp light on outdoor walls fades with distance like on the ground (LevelLightShare::SetRealisticReach)
bool g_roofs = true;
float g_roofStrengthSetting = 0.45f;
bool g_water = true;
bool g_waterFilter = true, g_waterColorCompression = true;
float g_waterStrengthSetting = 0.4f;
float g_waterReflSetting = 1.56f; // the tuned default
// Ground light: one control for lamp light on every ground (grass, roads, sidewalks, lots, patios, floors, snow), applied
// live in the ground shaders (LotLightBridge::SetGroundBrightness). Every lamp enters the terrain bake and the lot's own
// map at its full colour (the bake gains stay 1), so the ground looks the same on both sides of a lot border. 1 = the
// Soft balance of the four controls it replaces (ground 75% x lamps 80%); MigrateGround reads them from old configs.
float g_groundLight = 1.0f;
constexpr float kGroundSoft = 0.6f;
float GroundGain() { return kGroundSoft * g_groundLight; }
float g_moonlight = 0.0f;     // the moon's light at night
bool g_edgePad = true;          // pad the room light maps' edges (RoomMapPadding): no dark side on things along walls
bool g_lotTintOwn = false;      // lot lamps have their own colour
float g_lotLampTint = 1.0f;     // their colour (0 = pink, 1 = warm white)
bool g_allLotsHQ = false;
bool g_lotPassNoTerrainMap = true;
// Developer toggles (29/09, not yet tested in game; the public build never registers them: always off there):
//  - relightNearbyChunks: a lamp change re-renders only the terrain chunks under the changed lamps, one at a time through
//    the game's sweep branch (features/terrain_chunk_relight.cpp), instead of Apex's full rebuild;
//  - relightPacedSweep: Apex's own dusk and lamp-change full rebuilds become a paced sweep of every chunk, nearest to the
//    camera first (the world-load rebuild and the button stay full rebuilds).
bool g_localRelight = false;
bool g_pacedSweep = true; // test007: gradual external terrain updates, validated in game

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
// the last lamp change the local relight took (TryLocal): a drag's rebuild countdown is left idle only within
// kDragIdleProof of one, so a drag whose moves the local path never sees still gets the game's own rebuild
Clock::time_point g_localRelitAt{};
constexpr auto kDragIdleProof = std::chrono::milliseconds(3000);
int g_armsAtLastStuckKick = 0;
int g_lastLampEdits = 0;
int g_lastUserEdits = 0;
bool g_editsWhileLoading = false; // lot lamp changes seen before the world was drawn (see the lamp edit block in OnPresent)
// A world session that ended (main menu) re-arms the new-world handling: the next world may reuse the cells pointer (06/10:
// Sunset Valley -> main menu -> Bridgeport was never seen as a new world, its lamps were compared with the old snapshot)
bool g_sessionSeenActive = false, g_sessionRearm = false;
unsigned long long g_sessionInactiveSince = 0;
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
constexpr auto kCameraWaitMax = std::chrono::seconds(2); // a lamp change waits at most this long for the camera to stop (the square's lamps took ~16 s to reach the ground while panning)
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
int g_rebuildId = 0; // the paced full rebuild running (ChunkRelight::QueueRebuild): world load, button
std::string g_rebuildReason;
Clock::time_point g_rebuildAt{};
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
LightingRestartPolicy::Run g_restart;
bool g_restartRequested = false;
bool g_restartUnavailable = false;
bool g_restartFinishing = false;
bool g_restartPartial = false;
void PumpLightingRestart();
// The load is over: the after-load refresh ran (lots and rooms lit again), or 10 s passed since the world went live. With
// the game's interactive loading (Options.ini enableinteractiveloading = 1) terrain is drawn and the loading window is gone
// long before the lots finish (a log: the after-load refresh came 47 s after the world loaded), so "live" alone opened
// the start note and Depth Blur during that screen. Render thread writes, any thread reads.
std::atomic<bool> g_loadSettled{false};
constexpr auto kSettleFallback = std::chrono::seconds(10);
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
// Lamps with cones in the bake (the lawn, the deck and the fence caps greened by a wall sconce aimed down the
// wall; F8 capture). The bake gives every light only its position, its colour and a weight (0xC294E6..0xC295B7), no
// cone, so a type-5 sconce stamps its full colour all round. The game's own cone law (FUN_006BC940) gives the points off
// its cones only the spill +0x150 (per channel; 16-byte aligned: the game reads it with movaps at 0x006BCA6B): the stub
// stamps type-5 lot lamps at their spill. The pool right under the lamp comes from the room-solve map, which has the cones.
// The upper-floor gate mirrors it (LevelLightShare::SetBakeConeSpill).
volatile bool g_bakeConeSpill = true;
// The bake runs Apex's copy of its pixel shader (features/terrain_bake_cone.h), which takes each light's cones and spill
// from c5..c7 (BakeLightRows, from the position stub): the colour stub then leaves the spill to it. The game makes its
// shader long before Night Lighting installs, so the copy is bound in its place whenever the game binds it (render thread).
volatile bool g_bakeConeShader = false;
IDirect3DDevice9* g_bakeDevice = nullptr;
IDirect3DPixelShader9* g_gameBakePs = nullptr; // the game's bake pixel shader, found bound in a bake
IDirect3DPixelShader9* g_conePs = nullptr;     // Apex's copy (kept for the session: shaders survive a device reset)
bool g_coneFailed = false, g_bakeLogged = false;
float g_coneScale5 = 5.0f; // the shaded lamp's cone scale [0x011D11A0]
// (2.15.1) area lights (types 9, 10) at the share of the ground below they face (TerrainBakeRule::AreaGroundShare)
const float g_areaHalf = 0.5f, g_areaZero = 0.0f, g_areaOne = 1.0f;
// Lamps being dragged whose ground light TerrainStamps draws on the GPU: the bake gives them no colour (render thread,
// where the bake runs too; NightTerrainBake::SetExcluded)
uintptr_t g_bakeExcluded[NightTerrainBake::kMaxExcluded] = {};
int g_bakeExcludedCount = 0;
bool g_bakeGainInstalled = false;
float g_bakeGainSeen[2] = {1.0f, 1.0f}; // street / lot gains the multipliers hold
float g_tintSeen[2] = {1.0f, 1.0f};     // street / lot lamp colours the stock lamps were last re-coloured with

__declspec(naked) void BakeColourStub() {
    __asm {
        movaps xmm0, xmmword ptr [edi+0xF0]
        mov eax, dword ptr [g_bakeExcludedCount]
    excluded:                                 // a lamp being dragged: no colour (TerrainStamps draws it)
        test eax, eax
        jz kept
        dec eax
        cmp edi, dword ptr [g_bakeExcluded + eax*4]
        jne excluded
        xorps xmm0, xmm0
        ret
    kept:
        mov eax, dword ptr [edi+0xC0]
        or eax, dword ptr [edi+0xC4]
        jnz lot
        mulps xmm0, xmmword ptr [g_bakeStreetMul]
        ret
    lot:
        mulps xmm0, xmmword ptr [g_bakeLotMul]
        mov eax, dword ptr [edi+0xB0]
        cmp eax, 9                            // area lights: the share of the ground below they face
        je area
        cmp eax, 10
        je area
        cmp byte ptr [g_bakeConeSpill], 0
        je done
        cmp byte ptr [g_bakeConeShader], 0    // the shader gives its cones and spill
        jne done
        cmp eax, 5                            // type 5: a lamp with cones
        jne done
        mulps xmm0, xmmword ptr [edi+0x150]   // its spill, per channel
    done:
        ret
    area:
        sub esp, 16
        movups xmmword ptr [esp], xmm1        // (xmm1 kept for the game)
        movss xmm1, dword ptr [edi+0x184]     // its third axis' y (TerrainBakeRule::kAreaAxisYOffset)
        mulss xmm1, dword ptr [g_areaHalf]
        addss xmm1, dword ptr [g_areaHalf]
        maxss xmm1, dword ptr [g_areaZero]
        minss xmm1, dword ptr [g_areaOne]
        shufps xmm1, xmm1, 0
        mulps xmm0, xmm1
        movups xmm1, xmmword ptr [esp]
        add esp, 16
        ret
    }
}

// (2.15.1) Low lamps on the ground: the bake copies each light's position (vfunc+0x24) into its shader parameter at 0xC294F7
// ("movaps xmm0,[eax]; movaps [esi+110h],xmm0", EAX = the position, EDI = the light, ESI = the bake). The stub copies it and,
// for a lot lamp whose light stands lower than TerrainBakeRule::kLowLampLift over its lot's ground, stamps it from that height
// (TerrainBakeRule::StampHeight; the lot's ground from LotLightBridge::LotGroundY): a lamp lying on the lawn (light 5 cm up)
// lit only a tree above it, the lawn 2 m away took about 1 % of what a lamp 40 cm up gives it. The world grass, the lot edges,
// snow and the atlas then agree with the lot's own floor map (RtOutdoorLamps). EAX, ECX, EDX and the flags are dead there
// (written at 0xC29501, 0xC29508, 0xC2951D); xmm0 is loaded again at 0xC2950F; xmm1..7 are kept for the game.
uintptr_t kBakePosSite = 0; // 0x00C294F7 = BakeColourSite - 0x18
constexpr uintptr_t kBakePosBeforeColour = 0x18;
const std::vector<BYTE> kBakePosOrig = {0x0F, 0x28, 0x00, 0x0F, 0x29, 0x86, 0x10, 0x01, 0x00, 0x00};
bool g_bakePosInstalled = false;
bool g_lowLiftSeen = true; // TerrainBakeRule::LowLampLift() as the last rebuild had it
std::atomic<long> g_bakeLowLifted{0};
bool ReadLightLot(const BYTE* light, int& type, uint64_t& lot) {
    __try {
        type = *reinterpret_cast<const int*>(light + 0xB0);
        lot = *reinterpret_cast<const uint32_t*>(light + 0xC0) | static_cast<uint64_t>(*reinterpret_cast<const uint32_t*>(light + 0xC4)) << 32;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// A light's cone fields (SEH: the light may be gone)
bool ReadConeFields(const BYTE* light, TerrainBakeCone::Fields& f) {
    __try {
        f.type = *reinterpret_cast<const int*>(light + 0xB0);
        if (f.type == 4) {
            for (int i = 0; i < 3; i++) f.axis4[i] = reinterpret_cast<const float*>(light + 0x170)[i];
            f.off4 = *reinterpret_cast<const float*>(light + 0x158);
            f.scale4 = *reinterpret_cast<const float*>(light + 0x154);
        } else if (f.type == 5) {
            for (int i = 0; i < 3; i++) {
                f.a1[i] = reinterpret_cast<const float*>(light + 0x1A0)[i];
                f.a2[i] = reinterpret_cast<const float*>(light + 0x190)[i];
                f.S[i] = reinterpret_cast<const float*>(light + 0x150)[i];
            }
            f.o1 = *reinterpret_cast<const float*>(light + 0x174);
            f.o2 = *reinterpret_cast<const float*>(light + 0x170);
        }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
// c5..c7 as they were before a bake's first light wrote its cone rows: given back when the game binds another pixel shader
// (the bake shader does not read them; a shader after it may)
float g_bakeRowsSaved[3][4] = {};
bool g_bakeRowsHeld = false;
// The light about to be drawn: its cone rows into c5..c7 for Apex's bake shader (every light, so none keeps another's)
void BakeLightRows(const BYTE* light) {
    TerrainBakeCone::Fields f;
    if (!ReadConeFields(light, f)) f = {};
    float r[3][4];
    TerrainBakeCone::Rows(f, g_coneScale5, r);
    if (!g_bakeDevice) return;
    if (!g_bakeRowsHeld) g_bakeRowsHeld = SUCCEEDED(g_bakeDevice->GetPixelShaderConstantF(5, &g_bakeRowsSaved[0][0], 3));
    D3D9Hooks::CallOriginalSetPixelShaderConstantF(g_bakeDevice, 5, &r[0][0], 3);
}
// The saved c5..c7 back: after the chunk render that ran the bake (ChunkRenderThunk), before the game sets the next draw's
// constants; else at the game's next other pixel shader (a bake outside that call)
void GiveBackBakeRows() {
    if (!g_bakeRowsHeld || !g_bakeDevice) return;
    D3D9Hooks::CallOriginalSetPixelShaderConstantF(g_bakeDevice, 5, &g_bakeRowsSaved[0][0], 3);
    g_bakeRowsHeld = false;
}
const ShaderCache::Id kBakeConePs = [] {
    ShaderCache::Desc d;
    d.tag = "NightTerrainRelight BakeConePS";
    d.source = kTerrainBakeHlsl;
    d.sourceName = "terrain_bake_ps.hlsl";
    d.entry = "BakeConePS";
    d.target = "ps_2_0"; // the game's vertex shader is vs_2_0
    d.flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
    return ShaderCache::Add(std::move(d));
}();
bool IsGameBakePs(IDirect3DPixelShader9* ps) {
    UINT n = 0;
    if (!ps || FAILED(ps->GetFunction(nullptr, &n)) || n != TerrainBakeCone::kGamePsBytes) return false;
    std::vector<BYTE> code(n);
    return SUCCEEDED(ps->GetFunction(code.data(), &n)) && TerrainBakeCone::Fnv(code.data(), n) == TerrainBakeCone::kGamePsFnv;
}
// ps is about to draw a bake pass: the game's bake shader is found (once) and Apex's copy bound in its place; false
// when it is another shader or the copy cannot be made
bool BindConeShader(IDirect3DDevice9* dev, IDirect3DPixelShader9* ps) {
    if (!ps || g_coneFailed) return false;
    if (ps != g_conePs) {
        if (ps != g_gameBakePs) {
            if (g_gameBakePs || !IsGameBakePs(ps)) return false;
            std::string err;
            if (!g_conePs && ShaderCache::CreatePixelShader(dev, kBakeConePs, &g_conePs, &err) != ShaderCache::Result::Ok) {
                g_coneFailed = true;
                LOG_WARNING("[NightTerrainRelight] Terrain bake: the shader with lamp cones could not be made (" + (err.empty() ? std::string("create failed") : err) +
                            "): lamps with cones keep their spill on the ground");
                return false;
            }
            g_gameBakePs = ps;
            LOG_INFO("[NightTerrainRelight] Terrain bake: lamps light the ground through their cones, as on lots (Apex's copy of the bake shader)");
        }
        if (FAILED(D3D9Hooks::CallOriginalSetPixelShader(dev, g_conePs))) return false;
    }
    g_bakeDevice = dev;
    g_bakeConeShader = true;
    return true;
}
float ReadConeScale5() {
    const uintptr_t at = GameAddr::Get(GameAddr::Id::ConeScale5);
    __try {
        const float s = at ? *reinterpret_cast<const float*>(at) : 0.0f;
        return s > 0.0f && s < 100.0f ? s : 5.0f;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return 5.0f;
    }
}
volatile bool g_bakeConeHooked = false; // the SetPixelShader hook is in (Install)
bool g_inBakeLight = false;              // a bake light's position was just set: the next shader bound may be the bake's
void __cdecl BakeLiftHead(const BYTE* light, float* head) {
    if (g_bakeConeHooked && !g_coneFailed) { // the bake's shader may already be bound (a pass before): Apex's copy instead
        IDirect3DDevice9* dev = g_bakeDevice ? g_bakeDevice : ApexD3D::Device();
        IDirect3DPixelShader9* bound = nullptr;
        if (dev && SUCCEEDED(dev->GetPixelShader(&bound)) && bound) {
            BindConeShader(dev, bound);
            bound->Release();
        }
        g_inBakeLight = !g_gameBakePs;
        if (g_bakeConeShader) BakeLightRows(light);
    }
    int type = 0;
    uint64_t lot = 0;
    if (!TerrainBakeRule::LowLampLift() || !ReadLightLot(light, type, lot) || !lot || !TerrainBakeRule::LotLampType(type)) return;
    float ground = 0.0f;
    if (!LotLightBridge::LotGroundY(lot, ground)) return;
    const float h = head[1] - ground, s = TerrainBakeRule::StampHeight(h);
    if (std::isfinite(s) && s != h) {
        head[1] = ground + s;
        g_bakeLowLifted.fetch_add(1, std::memory_order_relaxed);
    }
}
__declspec(naked) void BakePosStub() {
    __asm {
        movaps xmm0, xmmword ptr [eax]
        movaps xmmword ptr [esi+0x110], xmm0
        mov edx, dword ptr [edi+0xC0]
        or edx, dword ptr [edi+0xC4]
        jnz helper                             // a lot lamp: maybe lifted (and its cone rows)
        cmp byte ptr [g_bakeConeShader], 0
        je street                              // a street lamp with no cone rows to write: as the game
    helper:
        sub esp, 112
        movups xmmword ptr [esp], xmm1
        movups xmmword ptr [esp+16], xmm2
        movups xmmword ptr [esp+32], xmm3
        movups xmmword ptr [esp+48], xmm4
        movups xmmword ptr [esp+64], xmm5
        movups xmmword ptr [esp+80], xmm6
        movups xmmword ptr [esp+96], xmm7
        lea eax, [esi+0x110]
        push eax
        push edi
        call BakeLiftHead
        add esp, 8
        movups xmm1, xmmword ptr [esp]
        movups xmm2, xmmword ptr [esp+16]
        movups xmm3, xmmword ptr [esp+32]
        movups xmm4, xmmword ptr [esp+48]
        movups xmm5, xmmword ptr [esp+64]
        movups xmm6, xmmword ptr [esp+80]
        movups xmm7, xmmword ptr [esp+96]
        add esp, 112
    street:
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
// manager (+0x130 = ExteriorLightData, read by terrain, roads, walls, floors, roofs, objects, Sims and ponds).
// That one call (to a shared "mov eax,ecx; ret" getter) is pointed at SunScaleGetter, which returns the game's value
// x lerp(1, moonlight, night level): the variable itself is never written, so a blend that starts from its current
// value, or Sims3SettingsSetter's "Sunlight brightness", is scaled once and never stacks. When the call cannot be
// redirected the value is written instead (older behaviour).
uintptr_t kSunlightScale = 0; // 0x011D0918
float g_sunlightBase = -1.0f;  // its value when first seen (< 0 = not read yet / not usable)
float g_moonWritten = -1.0f;
float g_moonSeen = 1.0f;       // moonlight value the object rigs gathered with
uintptr_t g_sunCall = 0;       // the redirected "call getter" (0 = not redirected)
BYTE g_sunCallOrig[5] = {};
bool g_sunCallTried = false;
float g_sunFactor = 1.0f;      // what SunScaleGetter multiplies by (render thread)
float g_sunScaled = 0.0f;

// (2.15.1) Moon height. The game's light direction (0x00C14F2E..0x00C150F5, every frame) is the sun's from the time of day
// with its height scaled by [obj+0x9B4] and held at a minimum [obj+0x9B8] (y = max(y k, min), then normalised); at night
// the sun is below the horizon, so y is that minimum and the moon (the same direction, x mirrored) sits at atan(min / its
// horizontal part): 0.7 degrees after dusk (min 0.0122), a few degrees at most later. At that height every bump of a road's
// normal map and every road piece's slight tilt switched the moon on or off and curbs cast shadows tens of metres long
// (dark bands on roads 40 % under the snow beside them, with moonlight x1.82). At full night the minimum is raised so
// the moon stands at least kMoonHeightDeg high; its horizontal direction still turns as the game moves it. FUN_00C11AD0
// calls this getter with ESI = that object (light direction +0x7F0, read for SetSun 0x006ACDF0 right after), so the new
// minimum shapes the next frame's direction. By day and through dusk the game's minimum is put back.
bool g_moonLift = true;          // developer A/B (moon_height)
float g_moonHeightDeg = 20.0f;   // developer slider: the least height of the moon at night
float g_liftWeight = 0.0f;       // night weight (render thread): 0 by day and at dusk, 1 at full night
uintptr_t g_liftObj = 0;         // the object whose minimum is managed
float g_liftGameMin = -1.0f;     // its own minimum (< 0 = not read)
float g_liftWritten = -1.0f;     // the minimum last written (< 0 = none)
float g_moonHeightNow = 0.0f;    // the light's height now, degrees (status line)
void LiftMoon(BYTE* obj) {
    __try {
        float& minRef = *reinterpret_cast<float*>(obj + 0x9B8);
        const float* n = reinterpret_cast<const float*>(obj + 0x7F0);
        if (g_liftObj != reinterpret_cast<uintptr_t>(obj)) { // a new world's sky: its own minimum
            g_liftObj = reinterpret_cast<uintptr_t>(obj);
            g_liftGameMin = minRef;
            g_liftWritten = -1.0f;
        } else if (g_liftWritten >= 0.0f && minRef != g_liftWritten) {
            g_liftGameMin = minRef; // the game set its minimum again (a new sky setting): that is the base now
        }
        const float base = g_liftGameMin;
        if (!(base > 0.0f && base < 1.0f) || !std::isfinite(n[0]) || !std::isfinite(n[1]) || !std::isfinite(n[2])) return;
        g_moonHeightNow = std::asin(std::clamp(n[1], -1.0f, 1.0f)) * 57.2957795f;
        float want = base;
        const float w = g_moonLift ? g_liftWeight : 0.0f;
        const float hor = std::sqrt(n[0] * n[0] + n[2] * n[2]);
        if (w > 0.0f && n[1] > 0.0f && hor > 1e-4f) {
            // n was normalised from (x, used, z): its horizontal part r = used x hor / n.y; the height H needs min = r tan H
            const float used = g_liftWritten >= 0.0f ? g_liftWritten : base;
            const float r = used * hor / n[1];
            const float target = std::max(base, r * std::tan(std::clamp(g_moonHeightDeg, 0.0f, 60.0f) * 0.0174532925f));
            want = base + w * (target - base);
        }
        if (minRef != want) minRef = want;
        g_liftWritten = want;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        g_liftObj = 0;
    }
}
bool LiftObjLive(); // the managed sky is still the world's (below, with ReadLightState)
void RestoreMoonHeight() {
    if (!g_liftObj || !(g_liftGameMin > 0.0f) || g_liftWritten < 0.0f) return;
    if (!LiftObjLive()) { // its world was left (main menu): the sky is freed, nothing to put back
        g_liftObj = 0;
        g_liftWritten = -1.0f;
        return;
    }
    __try {
        *reinterpret_cast<float*>(g_liftObj + 0x9B8) = g_liftGameMin;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    g_liftWritten = -1.0f;
}
float* __cdecl ScaledSun(const float* scale, BYTE* obj) {
    if (obj) LiftMoon(obj);
    g_sunScaled = *scale * g_sunFactor;
    return &g_sunScaled;
}

// (the game reloads xmm0 and ecx right after the call and keeps nothing in xmm or edx across it; esi and edi are kept)
__declspec(naked) void SunScaleGetter() {
    __asm {
        push esi
        push ecx
        call ScaledSun
        add esp, 8
        ret
    }
}

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

// (2.15.1) Day and night in Build mode (features/night_level.h): the sky copies its sunrise / sunset from the weather
// manager every update, "lea ecx,[esi+974h]; push ecx; mov ecx,[edx+74h]; call FUN_00D68FE0" at 0x00C15D9A
// (thiscall(weather, float* sunrise, float* sunset), ret 8; ESI = the sky). Build mode makes them the seasonless 0 / 0,
// and the sky's smooth night, the light manager's level and the lamp factors of the shaders then turned to day at dusk.
// That call goes to SunTimesHook, which keeps the season's times while the 0 / 0 stands with the clock still.
using SunTimesFn = void(__thiscall*)(void*, float*, float*);
SunTimesFn g_sunTimes = nullptr; // FUN_00D68FE0
uintptr_t g_sunTimesCall = 0;    // the redirected call (0 = not redirected)
std::vector<BYTE> g_sunTimesOrig;
const char* const kSunTimesPattern = "8D 8E 74 09 00 00 51 8B 4A 74 E8 ?? ?? ?? ?? 80 3D";
// FUN_00D68FE0's start: cmp byte [ecx+0B04h],0; lea eax,[ecx+0A48h]
const BYTE kSunTimesStart[] = {0x80, 0xB9, 0x04, 0x0B, 0x00, 0x00, 0x00, 0x8D, 0x81, 0x48, 0x0A, 0x00, 0x00};
// The clock (lightMgr+0xB4, hours) and the weather manager's debug set switch (+0xB04); NAN / false when unreadable
void ReadSkyState(uintptr_t sky, uintptr_t weather, float& hour, bool& debug) {
    hour = NAN;
    debug = false;
    __try {
        const uintptr_t lightMgr = *reinterpret_cast<const uintptr_t*>(sky + 0x1F4);
        if (lightMgr) hour = *reinterpret_cast<const float*>(lightMgr + 0xB4);
        if (weather) debug = *reinterpret_cast<const BYTE*>(weather + 0xB04) != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}
void __fastcall SunTimesHook(void* weather, void*, float* rise, float* set) {
    g_sunTimes(weather, rise, set);
    const uintptr_t sky = reinterpret_cast<uintptr_t>(rise) - 0x974;
    float hour;
    bool debug;
    ReadSkyState(sky, reinterpret_cast<uintptr_t>(weather), hour, debug);
    NightLevel::GateSunTimes(sky, reinterpret_cast<uintptr_t>(weather), debug, *rise, *set, hour);
}

// Finds "mov ecx,<scale>; call <mov eax,ecx; ret>; movss xmm0,[eax]" in the game image and points that call at
// SunScaleGetter (xmm0 is reloaded right after the call, eax is its result). Tried once per install.
void RedirectSunCall() {
    if (g_sunCallTried || !kSunlightScale) return;
    g_sunCallTried = true;
    BYTE* base = nullptr;
    size_t size = 0;
    if (!MemPatch::GetModuleInfo(GetModuleHandleW(nullptr), &base, &size)) return;
    const uint32_t v = static_cast<uint32_t>(kSunlightScale);
    const std::string pat = std::format("B9 {:02X} {:02X} {:02X} {:02X} E8 ?? ?? ?? ?? F3 0F 10 00", v & 0xFF, (v >> 8) & 0xFF, (v >> 16) & 0xFF, v >> 24);
    const uintptr_t site = MemPatch::ScanPattern(base, size, pat.c_str());
    BYTE orig[5] = {};
    int32_t rel = 0;
    static const BYTE kGetter[] = {0x8B, 0xC1, 0xC3}; // mov eax,ecx; ret
    if (!site || !MemPatch::ReadBytes(site + 5, orig, 5) || orig[0] != 0xE8 ||
        (std::memcpy(&rel, orig + 1, 4), !MemPatch::ValidateBytes(reinterpret_cast<LPCVOID>(site + 10 + rel), kGetter, sizeof(kGetter)))) {
        LOG_WARNING("[NightTerrainRelight] Moonlight: the sunlight scale reader is not the expected code; the value is written instead");
        return;
    }
    BYTE patch[5] = {0xE8};
    const int32_t to = MemPatch::CalculateRelativeOffset(site + 5, reinterpret_cast<uintptr_t>(&SunScaleGetter));
    std::memcpy(patch + 1, &to, 4);
    if (!MemPatch::WriteCodeSuspended(site + 5, patch, 5)) {
        LOG_WARNING("[NightTerrainRelight] Moonlight: the sunlight scale reader could not be redirected; the value is written instead");
        return;
    }
    std::memcpy(g_sunCallOrig, orig, 5);
    g_sunCall = site + 5;
    if (!kPublicBuild) LOG_INFO(std::format("[NightTerrainRelight] Moonlight: sunlight scale reader at {:#x} redirected (the variable is no longer written)", g_sunCall));
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
        RedirectSunCall();
    }
    const float factor = 1.0f + (g_moonlight - 1.0f) * level;
    // the moon height at full night only (night level 0.9 to 1 brings it in): by day and through dusk the sun is the game's
    g_liftWeight = level > 0.9f ? std::min((level - 0.9f) * 10.0f, 1.0f) : 0.0f;
    if (g_sunCall) {
        float game = 0.0f;
        if (ReadFloat(kSunlightScale, game) && game > 0.0f && game < 100.0f) g_sunlightBase = game;
        g_sunFactor = factor;
        g_moonWritten = g_sunlightBase * factor; // what the game now reads (status line)
        return true;
    }
    // Someone else wrote it since (Sims3SettingsSetter's "Sunlight brightness" edits the same variable): that is the new
    // base, so the moonlight scales the user's own choice and never stacks on itself
    float now = 0.0f;
    if (!ReadFloat(kSunlightScale, now)) return false;
    if (g_moonWritten >= 0.0f && now != g_moonWritten && now > 0.0f && now < 100.0f) {
        g_sunlightBase = now;
        LOG_INFO(std::format("[NightTerrainRelight] Moonlight: sunlight scale changed elsewhere to {:.3f} (new base)", now));
    }
    const float want = g_sunlightBase * factor;
    if (want == now) {
        g_moonWritten = want;
        return true;
    }
    if (!WriteFloat(kSunlightScale, want)) return false;
    g_moonWritten = want;
    return true;
}

void RestoreMoonlight() {
    RestoreMoonHeight();
    if (g_sunCall) {
        g_sunFactor = 1.0f;
        // only while the site still holds our call (another mod may have patched it since: then it is left alone)
        BYTE ours[5] = {0xE8};
        const int32_t to = MemPatch::CalculateRelativeOffset(g_sunCall, reinterpret_cast<uintptr_t>(&SunScaleGetter));
        std::memcpy(ours + 1, &to, 4);
        if (!MemPatch::ValidateBytes(reinterpret_cast<LPCVOID>(g_sunCall), ours, 5)) {
            LOG_WARNING("[NightTerrainRelight] Moonlight: the sunlight scale reader was changed by someone else; left as it is");
            g_sunCall = 0;
        } else if (MemPatch::WriteCodeSuspended(g_sunCall, g_sunCallOrig, 5)) {
            g_sunCall = 0;
            g_sunCallTried = false;
            g_sunlightBase = -1.0f; // read and redirect again on the next install
        }
    } else if (kSunlightScale && g_sunlightBase >= 0.0f && g_moonWritten >= 0.0f) {
        WriteFloat(kSunlightScale, g_sunlightBase);
    }
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
float g_levelStep = 0.0f; // (2.15.1) +0xF0 as the game steps it (g_level is the sky's smooth night)
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

// Lamp that belongs to a lot, is a lamp class (TerrainBakeRule::LotLampType), is alive/enabled and sits outdoors (room 0).
bool IsOutdoorLotLamp(const BYTE* L) {
    __try {
        const uint32_t lotLo = *reinterpret_cast<const uint32_t*>(L + 0xC0);
        const uint32_t lotHi = *reinterpret_cast<const uint32_t*>(L + 0xC4);
        if ((lotLo | lotHi) == 0) return false;
        const int type = *reinterpret_cast<const int*>(L + 0xB0);
        if (!TerrainBakeRule::LotLampType(type)) return false;
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

// The bake's light test without its counters: 0 rejected, 1 taken by the original test, 2 a lot lamp switched off, 3 a lot
// lamp taken
int BakeTest(BYTE* light) {
    // Type-11 vfunc+0x20 is FUN_007EAEA0 (always true). For a lot-owned outdoor
    // lamp, respect the observed enabled/lit state before accepting that original test.
    // World-owned lights and other classes keep their original behavior.
    if (*reinterpret_cast<const int*>(light + 0xB0) == 11 &&
        (*reinterpret_cast<const uint32_t*>(light + 0xC0) | *reinterpret_cast<const uint32_t*>(light + 0xC4)) != 0 &&
        (light[0x100] & 0x04) && *reinterpret_cast<const int*>(light + 0x08) == 0 &&
        (light[0x100] & 0x61) != 0x61) return 0;
    if (OriginalWorldLightTest(light)) return 1;
    if (!g_lotLamps || !IsOutdoorLotLamp(light)) return 0;
    return (light[0x100] & 0x20) ? 3 : 2;
}
// The lot lights the bake has given colour to (NightTerrainBake::EverBaked: a lamp new from the catalog is in no chunk's
// map yet). shortcut: never shrinks; it holds the lot lights of the session, a few thousand at most
std::mutex g_everBakedMx;
std::unordered_set<uintptr_t> g_everBaked;
std::atomic<bool> g_bakeTestSeen{false};
void NoteBaked(BYTE* light) {
    if ((*reinterpret_cast<const uint32_t*>(light + 0xC0) | *reinterpret_cast<const uint32_t*>(light + 0xC4)) == 0) return; // a street lamp
    for (int i = 0; i < g_bakeExcludedCount; i++)
        if (g_bakeExcluded[i] == reinterpret_cast<uintptr_t>(light)) return; // given no colour (BakeColourStub)
    std::lock_guard<std::mutex> lk(g_everBakedMx);
    g_everBaked.insert(reinterpret_cast<uintptr_t>(light));
}
// Replaces the vfunc+0x20 test in the terrain light bake collector. Lot lamps only while switched on.
bool __stdcall TerrainLightTest(BYTE* light) {
    const int t = BakeTest(light);
    if (t == 2) g_lotLampsSkippedOff.fetch_add(1, std::memory_order_relaxed);
    if (t == 3) g_lotLampsBaked.fetch_add(1, std::memory_order_relaxed);
    g_bakeTestSeen.store(true, std::memory_order_relaxed);
    if (t == 1 || t == 3) NoteBaked(light);
    return t == 1 || t == 3;
}

// A light's bake fields (SEH: the light may be gone)
struct BakeFields {
    float colour[4], spill[4], pos[3], rect[4], range, intensity, axisY;
    int type;
    uint64_t lot;
    bool taken;
};
bool ReadBakeFields(BYTE* light, BakeFields& f) {
    __try {
        const int t = BakeTest(light);
        f.taken = t == 1 || t == 3;
        std::memcpy(f.colour, light + 0xF0, 16);
        std::memcpy(f.spill, light + 0x150, 16);
        std::memcpy(f.pos, light + 0x120, 12);
        std::memcpy(f.rect, light + 0x134, 16);
        f.range = *reinterpret_cast<const float*>(light + 0x130);
        f.intensity = *reinterpret_cast<const float*>(light + 0x10);
        f.axisY = *reinterpret_cast<const float*>(light + 0x184);
        f.type = *reinterpret_cast<const int*>(light + 0xB0);
        f.lot = *reinterpret_cast<const uint32_t*>(light + 0xC0) | static_cast<uint64_t>(*reinterpret_cast<const uint32_t*>(light + 0xC4)) << 32;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
} // namespace

namespace NightTerrainBake {
void SetExcluded(const std::vector<uintptr_t>& lights) {
    int n = 0;
    for (uintptr_t L : lights)
        if (L && n < kMaxExcluded) g_bakeExcluded[n++] = L;
    g_bakeExcludedCount = n;
}
bool Excluded(uintptr_t light) {
    for (int i = 0; i < g_bakeExcludedCount; i++)
        if (g_bakeExcluded[i] == light) return true;
    return false;
}
bool ConeShader() { return g_bakeConeShader; }
bool EverBaked(uintptr_t light) {
    if (!g_bakeTestSeen.load(std::memory_order_relaxed)) return true; // the collector test is not in: unknown, so yes
    std::lock_guard<std::mutex> lk(g_everBakedMx);
    return g_everBaked.count(light) != 0;
}
// What BakeColourStub and BakePosStub give the bake for this light (0xC2950F colour x the kind's gain x the area share or
// the cone spill; .w = range x intensity x 0.2 as at 0xC29523 .. 0xC29586; the position with the low-lamp lift)
bool StampOf(uintptr_t light, float col[4], float pos[3], float rect[4], float& ground, float cone[3][4]) {
    BakeFields f{};
    if (!g_bakeGainInstalled || !light || !ReadBakeFields(reinterpret_cast<BYTE*>(light), f) || !f.taken || !f.lot) return false; // (lot lamps only)
    for (int k = 0; k < 3; k++) col[k] = f.colour[k] * g_bakeLotMul[k];
    if (f.type == 9 || f.type == 10) {
        const float share = std::clamp(f.axisY * g_areaHalf + g_areaHalf, g_areaZero, g_areaOne);
        for (int k = 0; k < 3; k++) col[k] *= share;
    } else if (g_bakeConeSpill && f.type == 5 && !g_bakeConeShader)
        for (int k = 0; k < 3; k++) col[k] *= f.spill[k];
    // its cone rows as BakeLightRows gives the bake (none while the bake has no cones)
    TerrainBakeCone::Fields cf;
    if (!g_bakeConeShader || !ReadConeFields(reinterpret_cast<const BYTE*>(light), cf)) cf = {};
    TerrainBakeCone::Rows(cf, g_coneScale5, cone);
    col[3] = f.range * f.intensity * 0.2f;
    std::memcpy(pos, f.pos, sizeof f.pos);
    std::memcpy(rect, f.rect, sizeof f.rect);
    if (!LotLightBridge::LotGroundY(f.lot, ground)) return false;
    if (g_bakePosInstalled && TerrainBakeRule::LowLampLift() && TerrainBakeRule::LotLampType(f.type)) { // as BakeLiftHead
        const float h = pos[1] - ground, s = TerrainBakeRule::StampHeight(h);
        if (std::isfinite(s) && s != h) pos[1] = ground + s;
    }
    return true;
}
} // namespace NightTerrainBake

namespace {

// ---- game state access ----
struct LightState {
    uintptr_t lightMgr = 0;
    uintptr_t cells = 0;
    float level = 0.0f;
};

bool LiftObjLive() {
    if (!g_rootPtrAddr || !g_liftObj) return false;
    __try {
        const uintptr_t root = *reinterpret_cast<const uintptr_t*>(g_rootPtrAddr);
        const uintptr_t lightMgr = root ? *reinterpret_cast<const uintptr_t*>(root + 0x1C0) : 0;
        const uintptr_t back = lightMgr ? *reinterpret_cast<const uintptr_t*>(lightMgr + 0xD0) : 0;
        return back > 0x210 && back - 0x210 == g_liftObj && *reinterpret_cast<const uintptr_t*>(g_liftObj + 0x1F4) == lightMgr;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

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
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
    return NightLevel::Read(out.lightMgr, out.level, true); // the sky's smooth night (Apex's lamp terms; +0xF0 without the redirect)
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

// (14/10, drag solves) The game's own countdown armed by a lamp drag is put back to idle (-1, what the game writes when it
// consumes it): a type-11 lamp dragged in Build mode is removed and registered again at every step, each re-arming +0x38,
// and 50 frames after the last step the game re-rendered every terrain chunk in one frame (a recording: a 210-230 ms frame
// after every drag). The lamp change path relights the chunks under the moved lamps locally meanwhile (TrackLotLampEdits ->
// DecideEdit), and once the lamp is still the last local relight leaves the terrain as the full rebuild would.
bool CancelCountdown(uintptr_t cells) {
    __try {
        *reinterpret_cast<int*>(cells + 0x38) = -1;
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
long g_dragArmsCancelled = 0;

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

// Every change of the diff is a lamp being dragged whose ground light TerrainStamps draws on the GPU (the bake excludes
// it). A dragged lamp is a removal at its last baked place and an addition at its new one: each addition stands within
// kStampedNear of an excluded lamp of its lot and type now, and each removal is paired with such an addition. A lamp of the
// same type deleted or placed beside the drag is left over and goes through the local relight (before, lot and type alone
// were matched, and a lamp deleted within the drag's last moments kept its ground light until its chunk was relit)
constexpr float kStampedNear = 0.5f; // m (the diff's place and the light's place now may be a step of the drag apart)
bool AllStamped(const std::vector<LotLightBridge::BakeChange>& changes) {
    struct Pair { uint64_t lot; int type; int added, removed; };
    std::vector<Pair> pairs;
    const auto pairOf = [&pairs](uint64_t lot, int type) -> Pair& {
        for (Pair& p : pairs)
            if (p.lot == lot && p.type == type) return p;
        return pairs.emplace_back(Pair{lot, type, 0, 0});
    };
    for (const LotLightBridge::BakeChange& c : changes) {
        if (!c.user || c.hasOld == c.hasNew) return false; // (a light change or a switch: no drag)
        bool nearby = false, sameKind = false;
        for (int i = 0; i < g_bakeExcludedCount && !nearby; i++) {
            BakeFields f{};
            if (!ReadBakeFields(reinterpret_cast<BYTE*>(g_bakeExcluded[i]), f) || f.lot != c.lot || f.type != c.type) continue;
            sameKind = true;
            const float dx = f.pos[0] - c.pos[0], dy = f.pos[1] - c.pos[1], dz = f.pos[2] - c.pos[2];
            nearby = dx * dx + dy * dy + dz * dz <= kStampedNear * kStampedNear;
        }
        if (!sameKind || (c.hasNew && !nearby)) return false;
        (c.hasNew ? pairOf(c.lot, c.type).added : pairOf(c.lot, c.type).removed)++;
    }
    return std::all_of(pairs.begin(), pairs.end(), [](const Pair& p) { return p.removed <= p.added; });
}

// The pending change through the local relight. 1 = queued (FinishEdit called), 2 = waiting (WaitEdit called), 0 = not
// possible (`why`): the caller takes the full path with its own limits.
int TryLocal(Clock::time_point now, const std::vector<uint64_t>& newLots, const std::string& diffText, std::string& why) {
    // a dragged lamp the GPU draws on the terrain: no relight while it moves (the stamp follows it; TerrainStamps relights
    // where it was when the drag starts and where it stays when it ends), and the drag's countdown stays idle
    if (g_bakeExcludedCount > 0 && !g_editDiff.changes.empty() && AllStamped(g_editDiff.changes)) {
        g_localRelitAt = now;
        FinishEdit(std::format("{} ({}): skipped: drawn on the GPU while dragged ({})", g_editReason, EditKind(), diffText));
        return 1;
    }
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
    std::vector<std::pair<int, int>> cells;
    const int id = ChunkRelight::QueueLocal(lamps, why, chunks, user, &cells);
    if (!id) return 0;
    // A player's switch (lamps on or off, nothing moved or recoloured): the ground keeps its light until every chunk of the
    // batch is re-rendered, then changes in one frame (06/10, "the ground as perfect as the walls"). Not for drags or
    // value edits, whose light should follow the lamp chunk by chunk.
    const LotLightBridge::BakeDiff& d = g_editDiff;
    if (user && d.switchedOn + d.switchedOff > 0 && d.added + d.removed + d.light == 0) LightmapSmooth::HoldChunks(id, cells);
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
    g_localRelitAt = now;
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
            if (!plain && TerrainBakeRule::LotLampType(b.type)) continue;
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

// The world-load and button rebuilds (item 13b): with relightPacedSweep the game's full rebuild one chunk per frame,
// nearest to the camera first (ChunkRelight::QueueRebuild), instead of every chunk in one ~240 ms frame. It starts like a
// consumed rebuild (the lamps as they are now go into the snapshot, a waiting lamp change is covered, the smoothed maps
// expect every chunk again); its end enables the local path. False when not possible: the caller arms the full rebuild.
// Off since 06/10: in game the nearest chunk (3,4) kept its +0x55 for 600 frames (the chunk loop has states before the
// +0x55 test, 0x00C84F2F..0x00C84FD3, not understood yet), and the terrain kept the old light ~10 s after the load until
// the fallback. Needs a runtime trace of that loop before it is enabled again.
constexpr bool kPacedRebuildEnabled = false;
bool StartPacedRebuild(const std::string& reason, Clock::time_point now) {
    if (!kPacedRebuildEnabled || !g_pacedSweep) return false;
    float eye[3] = {};
    const bool haveEye = g_camOk && ReadEye(eye);
    const float eyeXZ[2] = {eye[0], eye[2]};
    std::string why, info;
    const int id = ChunkRelight::QueueRebuild(haveEye ? eyeXZ : nullptr, why, info);
    if (!id) {
        LOG_INFO(std::format("[NightTerrainRelight] Paced rebuild not possible ({}): {}; full rebuild instead", reason, why));
        return false;
    }
    g_rebuildId = id;
    g_rebuildReason = reason;
    g_rebuildAt = now;
    g_rebuilds.fetch_add(1);
    LightmapSmooth::NoteKick(reason.c_str());
    LightmapSmooth::OnTerrainRebuilt(); // every chunk map is re-rendered over the next frames
    g_localBatches.clear(); // dropped by QueueRebuild: this rebuild bakes their lamps too
    g_sweepId = 0;
    g_lastRebuildAt = now;
    if (g_editKickPending) {
        g_decCovered++;
        FinishEdit(std::format("{} ({}): skipped: the {} rebuild started", g_editReason, EditKind(), reason));
    }
    g_bakedDue = true;
    g_bakedDueAt = now;
    g_bakedDueEnum = LotLightBridge::LampEnumerations();
    LotLightBridge::RequestLampRefresh();
    g_lastEvent = std::format("Terrain rebuild started, one chunk at a time: {} ({})", reason, info);
    LOG_INFO("[NightTerrainRelight] " + g_lastEvent);
    return true;
}

// A user-driven change goes ahead 50 ms after its last step, or every 100 ms while it goes on (a lamp dragged in Build
// mode: since 05/10 a move reads the lamp list again within 33 ms, so the steps never leave a quiet gap, and the 500 ms of
// before made the ground follow the lamp only twice a second; 100 ms stays under the local relight's 12 chunks a second)
// (14/10, drag solves) while a lamp is dragged every 250 ms (4 a second); the one 50 ms after the last step is the exact one
// (14/10, Classic: relight only after release) a dragged lamp's ground waits for the release, then relights once
bool EditReady(Clock::time_point now, Clock::time_point first, Clock::time_point last, bool priority) {
    if (DragLite::ClassicHolding()) return false;
    const bool slow = TesterToggles::dragSlowRefresh.load(std::memory_order_relaxed) && DragLite::Dragging();
    return now - last >= (priority ? std::chrono::milliseconds(50) : kEditQuiet)
        || (priority && now - first >= std::chrono::milliseconds(slow ? 250 : 100));
}

// The pending lamp change, once quiet (render thread; c38 = cells+0x38 this frame).
void DecideEdit(uintptr_t cells, float level, bool night, int c38, Clock::time_point now) {
    const bool worldLampEdit = std::find(g_editUserLots.begin(), g_editUserLots.end(), uint64_t{0}) != g_editUserLots.end();
    if (TerrainLightingPolicy::DeferDayEdit(night, g_autoDusk, g_editUser, g_editForce, worldLampEdit))
        return FinishEdit(std::format("{}: left to the dusk rebuild (day)", g_editReason));
    if (g_loadKickPending || g_scheduled)
        return FinishEdit(std::format("{}: merged into the {} rebuild", g_editReason, g_loadKickPending ? "load" :
                                     (g_cycle.target == TerrainLightingPolicy::Phase::Night ? "dusk" : "daylight")));
    // The paced load / button rebuild is still going (a second or two): decide after it, so the change does not arm a
    // full rebuild on top of it (before its end the local path is not available yet)
    if (ChunkRelight::RebuildActive()) return WaitEdit(EditWait::Relight, "waiting for the terrain rebuild in progress");
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
        g_restart.Cancel();
        g_restartRequested = false;
        LevelLightShare::StopLightingRestart();
        g_lastCells = 0;
        g_scheduled = false;
        g_prevCounter = INT_MIN;
        g_status = "Waiting for the game to load a world";
        g_menuLevel.store(-1.0f);
        g_liftObj = 0; // the world's sky is gone: nothing to put back
        g_liftWritten = -1.0f;
        NightLevel::ResetGates(); // the next world's sun times start fresh
        return;
    }
    DaylightProbe::OnPresent(s.lightMgr); // development builds only (daylight_log): logs the sky state, changes nothing
    const auto now = Clock::now();
    const bool night = s.level > 0.99f;
    int c38 = 0, c3C = 0;
    ReadCounters(s.cells, c38, c3C);
    g_level = s.level;
    {
        float f0 = 0.0f; // the light manager's level in its steps (smooth dusk is s.level)
        g_levelStep = NightLevel::ReadF0(s.lightMgr, f0) && std::isfinite(f0) ? std::clamp(f0, 0.0f, 1.0f) : s.level;
    }
    g_menuLevel.store(s.level);
    {
        static bool s_held = false;
        if (NightLevel::Holding() != s_held) {
            s_held = !s_held;
            LOG_INFO(s_held ? std::format("[NightTerrainRelight] Build mode's seasonless sun times: the season's are kept (night level {:.2f})", s.level)
                            : std::string("[NightTerrainRelight] The sky's own sun times again"));
        }
    }
    g_counter38 = c38;
    g_counter3C = c3C;
    // (14/10, drag solves) the countdown a lamp drag armed: idle again while the local path is there (CancelCountdown)
    {
        static Clock::time_point s_dragArmUntil{};
        static bool s_logged = false;
        if (DragLite::Engaged()) s_dragArmUntil = now + std::chrono::milliseconds(2000);
        else if (now >= s_dragArmUntil) s_logged = false;
        // (only while the local path has relit a lamp change lately: a drag of a lamp the GPU draws rereads the list every
        // 33 ms and its moves could go unseen, and the countdown it armed was left idle, so the ground kept the lamp's
        // old light, a recording: 13 s of drag, no local relight)
        if (TesterToggles::dragTerrainIdle.load(std::memory_order_relaxed) && night && c38 > 0 && c3C <= 0 && g_pendingReason.empty() && now < s_dragArmUntil &&
            g_haveBaked && !g_editLocalRefused && now - g_localRelitAt < kDragIdleProof && ChunkRelight::LikelyAvailable() && !ChunkRelight::RebuildActive() &&
            CancelCountdown(s.cells)) {
            c38 = -1;
            g_counter38 = -1;
            g_prevCounter = -1;
            g_dragArmsCancelled++;
            if (!s_logged) {
                s_logged = true;
                LOG_INFO(std::format("[NightTerrainRelight] Lamp drag: the game's terrain rebuild countdown left idle (the moved lamps are relit locally; {} so far)",
                                     g_dragArmsCancelled));
            }
        }
    }

    {
        const bool active = WorldSession::IsActive();
        const unsigned long long tick = GetTickCount64();
        if (active) {
            g_sessionInactiveSince = 0;
            if (g_sessionRearm) {
                g_sessionRearm = false;
                g_lastCells = 0; // handled as a new world below
                LOG_INFO("[NightTerrainRelight] A world session started again after the main menu: handled as a new world");
            }
            g_sessionSeenActive = true;
        } else if (g_sessionSeenActive) { // never at start-up: only after a world was active
            if (!g_sessionInactiveSince) g_sessionInactiveSince = tick;
            else if (tick - g_sessionInactiveSince >= 2000) g_sessionRearm = true;
        }
    }
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
        g_editsWhileLoading = false;
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
        g_rebuildId = 0;
        LOG_INFO(std::format("[NightTerrainRelight] World loaded ({}): the terrain rebuild waits until the world is drawn and the night level is steady",
                             LevelText(s.level)));
        LotLightBridge::OnWorldChanged(); // drop the previous world's chunk maps, smoothed maps and atlas
        RtOutdoorLamps::OnWorldChanged();
        LevelLightShare::OnWorldChanged();
        UnlitRooms::OnWorldChanged();
        // Rebuild the terrain light maps once after loading: the ones baked into the world file miss the part of a lamp's
        // light that crosses into the neighbouring 256 m chunk (straight cut on world grass at chunk borders). Not during
        // the loading screen (a kick there was consumed at the first world update with the night level still 0.00: a
        // lamps-off bake, then the dusk rebuild on top): see "world live" below.
        g_loadKickPending = true;
        g_worldAt = now;
        LoadTiming::NoteWorldChange();
        g_live = false;
        g_restart.Cancel();
        g_restartRequested = false;
        g_loadSettled.store(false);
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
            LoadTiming::NoteFirstWorldDraw(signal);
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
        g_rebuildId = 0;
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
        const std::string reason = night ? "world load (night: also the dusk rebuild)" : "world load";
        if (!StartPacedRebuild(reason, now)) {
            Kick(s.cells, s.level, reason, night);
            g_pendingLoad = true;
        }
    }
    if (g_kickRequested.exchange(false) && !StartPacedRebuild("button", now)) Kick(s.cells, s.level, "button");
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
        if (!WorldSession::InWorld()) {
            // lots and their lamps still streaming in during the load (a full rebuild every 3 s as "user edits"):
            // no decision until the world is drawn, then one automatic rebuild (below)
            g_editsWhileLoading = true;
        } else {
            NoteEdit(now, userDriven, false, userDriven ? "observed lot lamps switched or edited" : "lot lamps switched, dimmed or recoloured");
            if (userDriven)
                for (uint64_t lot : LotLightBridge::LastUserChangeLots()) {
                    g_editUserLots.push_back(lot);
                    if (lot == 0) g_worldRigRefreshPending = true;
                }
        }
    }
    if (g_editsWhileLoading && WorldSession::InWorld()) { // the world is drawn: the lamps it loaded with, in one automatic rebuild
        g_editsWhileLoading = false;
        NoteEdit(now, false, false, "lot lamps that loaded with the world");
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

    // The lamp colours: once the slider is let go, one terrain rebuild (a switch: no snapshot compare) with the new values;
    // they also re-solve the lots' outdoor light once.
    const bool dragging = MenuSliderHeld();
    if (g_bakePosInstalled && TerrainBakeRule::LowLampLift() != g_lowLiftSeen) {
        g_lowLiftSeen = TerrainBakeRule::LowLampLift();
        NoteEdit(now, true, true, "height of lamps on the ground changed", true);
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
            if (d.rebuild) {
                if (d.id != g_rebuildId) continue;
                g_rebuildId = 0;
                g_lastEvent = std::format("Terrain rebuilt one chunk at a time: {} ({}, {:.1f} s after it started)", g_rebuildReason, d.text, MsSince(g_rebuildAt) / 1000.0);
                LOG_INFO("[NightTerrainRelight] " + g_lastEvent);
                continue;
            }
            if (d.sweep) {
                if (d.id != g_sweepId) continue;
                g_sweepId = 0;
                g_sweepsDone++;
                g_lastEvent = std::format("Terrain sweep done: {} ({}, {:.1f} s after it started)", g_sweepReason, d.text, MsSince(g_sweepAt) / 1000.0);
                LOG_INFO("[NightTerrainRelight] " + g_lastEvent);
                continue;
            }
            LightmapSmooth::ReleaseHold(d.id); // a lamp switch's ground: every chunk of it shows its new light in the next frame
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
            LightmapSmooth::ReleaseHold(0);
            g_localBatches.clear();
            g_sweepId = 0;
            const bool rebuild = g_rebuildId != 0;
            g_rebuildId = 0;
            Kick(s.cells, s.level, (rebuild ? "paced rebuild failed (" : "local terrain relight failed (") + fr.why + ")");
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

    PumpLightingRestart();
    if (g_loadKickPending)
        g_status = g_live ? "World loaded: rebuilding the terrain light once the night level is steady" : "World loading: the terrain light is rebuilt once the world is drawn";
    else if (c38 == 0 && !night)
        g_status = "Rebuild pending: the game only rebuilds the terrain at night (or in Build mode)";
    else if (c38 > 0)
        g_status = std::format("Rebuild in {} frames", c38);
    else
        g_status = night ? "Night: ok" : "Day: ok";
}

void PumpLightingRestart() {
    if (g_restartRequested) {
        g_restartRequested = false;
        if (!g_live || !WorldSession::InWorld() || !LevelLightShare::BeginLightingRestart()) {
            g_restart.Cancel();
            g_restartUnavailable = true;
            return;
        }
        g_restart.Start(GetTickCount());
        g_restartUnavailable = false;
        g_restartFinishing = false;
        g_restartPartial = false;
        LightmapSmooth::Clear();
        RtOutdoorLamps::RestartLighting();
        g_kickRequested = true;
        ObjectLightBridge::RequestRigRefresh();
        LOG_INFO("[NightTerrainRelight] Lighting restart: temporary maps discarded; loaded stories rebuild in frame-sized batches");
    }
    if (!g_restart.active) return;
    if (!g_live || !WorldSession::InWorld()) {
        g_restart.Cancel();
        LevelLightShare::StopLightingRestart();
        return;
    }
    const auto rooms = LevelLightShare::LightingRestartProgress();
    const bool terrainBusy = g_kickRequested.load() || g_rebuildId || g_sweepId || !g_pendingReason.empty() || ChunkRelight::Busy() || LightmapSmooth::HoldPending();
    const bool workBusy = rooms.active || rooms.pending || terrainBusy || RtOutdoorLamps::RestartPending();
    if (workBusy) g_restartFinishing = false;
    else if (!g_restartFinishing) {
        ObjectLightBridge::RequestRigRefresh();
        UnlitRooms::RigsAgainIn(0);
        g_restartFinishing = true;
    }
    const bool busy = workBusy || ObjectLightBridge::RigRefreshPending() || UnlitRooms::RigRefreshPending();
    if (g_restart.Observe(GetTickCount(), busy)) {
        g_restartPartial = RtOutdoorLamps::RestartIncomplete();
        LOG_INFO(g_restartPartial ? "[NightTerrainRelight] Lighting restart finished with fallback: some areas keep the game's maps" :
                                   "[NightTerrainRelight] Lighting restart finished: loaded maps are idle and object refresh requests were dispatched");
    }
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
// Any lighting setting changed (menu, profile, undo, reset, the upper floors switch): the F9
// refresh, "Refresh the lighting", runs automatically once, kAutoRefreshDelay after the last change
// (a slider being dragged keeps pushing it back), on the render thread (Present)
constexpr DWORD kAutoRefreshDelay = 1000;
std::atomic<DWORD> g_autoRefreshAt{0};
// The pending refresh leaves the terrain alone (after a load: the load's own rebuild made it); any setting change clears it
std::atomic<bool> g_autoRefreshRoomsOnly{false};
void RequestAutoRefresh() {
    g_autoRefreshRoomsOnly.store(false);
    g_autoRefreshAt.store((GetTickCount() + kAutoRefreshDelay) | 1);
}
// The same refresh right after loading (a room lit only by lamps of another story was black after a load
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
        RegisterFloatSetting(&g_objStrength, "forcaNosObjetos", SettingWidget::Slider, 0.75f, 0.25f, 3.0f,
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
        // Lamp lighting (Performance page, every build; 2.12.3 + RT): Instant = Real-time outdoor lamps on the kept parts of Quick
        // outdoor lamp edits (the default since 14/10); Classic = the game's room solves (with the drag and floor holds of lamp-drags.md)
        RegisterNamedEnumSetting(&g_lampMode, "lampLightingMode", kLampInstant,
            "How lot lamps update when one is switched, recoloured or moved: classic (the game's room solves with every correction) "
            "or instant (the active lot's outdoor lamps drawn on the GPU with shadows into every story's outside light maps).",
            {"classic", "instant"});
        if (!kPublicBuild) // developer only
            RegisterBoolSetting(&g_outdoorDeltaCheck, "outdoorLampDeltaCheck", false,
                "Developer: each room 0 brought up to date by an outdoor lamp edit is solved in full once more a moment later, and the log "
                "gives the largest texel difference between the two.");
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
        RegisterFloatSetting(&g_fenceGroundStrength, "forcaNasCercas", SettingWidget::Slider, 0.75f, 0.25f, 2.0f,
            S3SS_TR("Forca da luz do chao nas cercas, grades e escadas.", "How strongly fences, railings, stairs and the snow on them are lit."));
        RegisterBoolSetting(&g_walls, "paredesComLuz", true,
            S3SS_TR("As paredes externas recebem a luz das lampadas com a forca escolhida (desligado = como o jogo).",
                    "Outside walls get lamp light at the chosen brightness (off = the game's own dim wall light)."));
        RegisterFloatSetting(&g_wallStrength, "forcaNasParedes", SettingWidget::Slider, 0.30f, 0.25f, 4.0f,
            S3SS_TR("Intensidade da luz das lampadas nas paredes externas, durante o dia e a noite.",
                    "Intensity of lamp light on outside walls, by day and night."));
        RegisterBoolSetting(&g_realisticReach, "alcanceRealistaDasLampadas", true,
            "Lamp light on outside walls, trees and bushes fades with distance as it does on the ground, instead of staying at full "
            "brightness for about 9 m and reaching upper stories far away (relights every room when changed).");
        RegisterBoolSetting(&g_levelShare, "luzExternaEntreAndares", true,
            S3SS_TR("Luminarias externas iluminam as paredes e pisos de todos os andares (a luz nao corta mais na linha do piso).",
                    "Outdoor lights reach the walls and floors of every story (no cut at the floor line)."));
        RegisterBoolSetting(&g_indoorShare, "luzInternaEntreAndares", true,
            "Indoor lamps light the story above or below through stairwells, atriums and removed floors (needs \"Outdoor light between floors\").");
        RegisterBoolSetting(&g_wallAlign, "paredesSemEmendaEntreAndares", true,
            "Walls are lit at the heights the game draws their light at, so the walls above and below a floor line meet on the same light (needs \"Outdoor light between floors\").");
        RegisterBoolSetting(&g_neutralProbe, "reflexosNeutrosANoite", true,
            "At night the game's sky light on outdoor floors, roofs and shiny objects keeps its brightness but loses the colour of the lamps it captured.");
        RegisterFloatSetting(&g_neutralProbeStrength, "forcaReflexosNeutros", SettingWidget::Slider, 1.0f, 0.0f, 1.0f, "How much of the lamps' colour leaves the night sky light (1 = all).");
        RegisterBoolSetting(&g_foundationWalls, "paredesExternasNaAlturaDesenhada", true,
            "Fixes two game bugs that put wall lamp light at the wrong height: outside walls of houses on a foundation, and walls taller than a story (up to the roof).");
        RegisterBoolSetting(&g_floorWalls, "paredesBloqueiamLuzNosPisos", true,
            "Walls stop lamp light on outdoor floors: decks and yards without a roof behind a wall stay dark (relights the rooms when changed).");
        RegisterBoolSetting(&g_realisticOpenings, "luzRealistaPorAberturas", kRealisticOpeningsDefault,
            "A yard, light well or deck without a roof closed by walls takes the light of its story's outdoor lamps only through its doors, "
            "windows and arches, at their height, or over walls lower than the lamp; one closed all round stays dark (needs \"Walls block light on floors\"; "
            "relights every room when changed).");
        RegisterBoolSetting(&g_doorsBlock, "portasBloqueiamLuz", true,
            "Doors and gates count as closed for lamp light, as the game keeps them closed except while they open; arches, windows and "
            "doors with glass (through the glass) and open passages let it through. Applies to light through doors and windows and to outdoor light through openings; "
            "only the stories with doors are lit again when changed.");
        RegisterBoolSetting(&g_indoorOpenings, "luzInternaPelasAberturas", true,
            "Lamps in a room with a roof light the garden, the terraces, the outside walls and the objects outside through the room's arches "
            "and windows, and a roofed room next door through an opening between them, blocked by the walls on the way; one opening only, no "
            "light through a closed door while doors block light (relights every room when changed).");
        RegisterBoolSetting(&g_indoorGround, "luzInternaSomaNoChao", true,
            "The light lit rooms send through their windows, doors and arches onto the lot ground and the outdoor floors of the ground "
            "story adds to the garden and street lamps' light there, instead of the brighter of the two, and the walls' shade of the "
            "outdoor lamps leaves it whole (needs \"Indoor light through openings\" and \"Walls block light on floors\"; relights every "
            "room when changed).");
        RegisterFloatSetting(&g_shadowSoftness, "suavidadeDasSombras", SettingWidget::Slider, 0.15f, 0.02f, 0.5f,
            "A lamp's size (its radius in metres; the game keeps none): the light passing an opening, a wall's end or top and a balcony's "
            "edge keeps a sharp edge next to it and a softer one farther behind, as with a real lamp; at least a texel and a half on the "
            "lighting maps, so edges show no stairs (relights the rooms with openings or yards, once the slider is let go).");
        RegisterBoolSetting(&g_outdoorOpenings, "luzDeForaPelasAberturas", true,
            "Garden and street lamps of a story light the rooms with a roof through their doors, windows and arches, bounded by the jambs and "
            "blocked by the walls on the way; a room with no opening to the outside takes none (relights every room when changed).");
        RegisterBoolSetting(&g_smoothRoomMaps, "mapasDosComodosSuaves", true,
            "Lamp light in rooms without blocks or rings: the rooms' light maps are read with a smooth curve instead of straight steps "
            "between texels, their 8-bit levels get a fine grain instead of bands, and a lamp's light fades out where the game cut it "
            "off (relights every room when changed).");
        RegisterBoolSetting(&g_objectWalls, "paredesBloqueiamLuzNosObjetos", true,
            "Walls and the floors of balconies and upper stories stop lamp light on outdoor windows, objects and fences: furniture "
            "in a yard without a roof behind a wall stays dark, and a lamp under a balcony does not light what stands on it; trees and bushes behind a building "
            "take no garden lamp light.");
        RegisterBoolSetting(&g_groundShade, "paredesSombreiamOChao", true,
            "Walls and garden walls shade lamp light on the ground: grass, paths, decks, curbs and stairs behind them stay dark, on "
            "the lot and on the grass, streets and sidewalks around it, with soft edges at wall ends and tops. Each wall shades by its "
            "own height.");
        RegisterEnumSetting(&g_lightDetail, "detalheDaLuz", 0,
            "How sharp the game's lamp light is on walls and floors. High doubles its detail: rooms take longer to light and use more memory "
            "(experimental; takes effect after restarting the game).",
            {"Game", "High"});
        RegisterBoolSetting(&g_allFloors, "todosOsAndaresEmDetalhe", true,
            "Every floor of the lot being played is lit in full detail, so changing floors keeps the light instead of solving it again (more work when entering a lot).");
        RegisterBoolSetting(&g_objPixel, "objetosDeForaComLuzDoChao", true,
            S3SS_TR("Portas, janelas, balcoes e outros objetos de fora recebem, ponto a ponto, no minimo a luz do chao em volta (sem escurecer nada).",
                    "Outdoor doors, windows, counters and similar objects get at least the ground light around them, point by point."));
        RegisterBoolSetting(&g_objPixelLights, "luzPorPixelNosObjetos", true,
            S3SS_TR("Objetos de fora (balcoes, pecas modulares, portas) recebem as lampadas calculadas em cada ponto, iguais para todas as pecas: sem emendas de cor entre pecas vizinhas.",
                    "Outdoor objects (counters, modular pieces, doors) get lamp light computed at every point, the same for every piece: no colour seams between neighbouring pieces."));
        RegisterFloatSetting(&g_objPixelLightStrength, "forcaLuzPorPixelNosObjetos", SettingWidget::Slider, 0.75f, 0.25f, 3.0f,
            S3SS_TR("Forca das lampadas calculadas por ponto nos objetos de fora.", "Strength of the per-point lamp light on outdoor objects."));
        RegisterBoolSetting(&g_windowOutdoor, "janelasLuzDeFora", true,
            "The outer side of a window lit by the room takes the outdoor ground light and lamps (a window turned inward no longer goes black outside).");
        RegisterBoolSetting(&g_roofs, "telhadosComLuz", true,
            S3SS_TR("Telhados recebem a luz das lampadas e postes proximos (sombra mais suave tambem).",
                    "Roofs receive light from nearby lamps (with softer shadows)."));
        RegisterFloatSetting(&g_roofStrengthSetting, "forcaNosTelhados", SettingWidget::Slider, 0.45f, 0.05f, 2.0f,
            S3SS_TR("Forca da luz das lampadas nos telhados.", "How strongly lamps light roofs."));
        RegisterBoolSetting(&g_unlitOn, "comodosEscurosSemLuz", true,
            "Rooms with every lamp off keep only a little light (set below) instead of the game's blue glow.");
        RegisterFloatSetting(&g_unlitLight, "luzQueSobraNosComodos", SettingWidget::Slider, 0.20f, 0.1f, 0.8f,
            "How much of the game's light stays in a room with every lamp off, on walls, floors and furniture.");
        RegisterFloatSetting(&g_unlitBlue, "azulNosComodos", SettingWidget::Slider, 0.0f, 0.0f, 1.0f,
            "How blue the light left in rooms is, on walls, floors and furniture (1 = the game's blue, 0 = grey).");
        RegisterBoolSetting(&g_waterFilter, "waterSpecularFilter", true, "Stabilize lamp sparkles on water");
        RegisterBoolSetting(&g_waterColorCompression, "waterPreserveLampColors", true, "Keep lamp colors on water");
        RegisterBoolSetting(&g_water, "lagosRefletemLampadas", true,
            S3SS_TR("A agua dos lagos reflete as lampadas e postes proximos a noite.", "Ponds glow and reflect nearby lamps at night."));
        RegisterFloatSetting(&g_waterStrengthSetting, "brilhoNaAgua", SettingWidget::Slider, 0.4f, 0.1f, 0.4f,
            S3SS_TR("Brilho do reflexo das lampadas na agua.", "Brightness of lamp reflections on water."));
        RegisterFloatSetting(&g_waterReflSetting, "reflexoNoLago", SettingWidget::Slider, 1.56f, 0.0f, 3.0f,
            S3SS_TR("Forca do reflexo da margem (arvores, casas, postes) na agua dos lagos.",
                    "Strength of the shore reflection (trees, houses, lamps) on ponds (needs Depth Blur)."));
        RegisterFloatSetting(&g_groundLight, "luzNoChao", SettingWidget::Slider, 1.0f, 0.25f, 3.0f,
            "How bright lamp light is on every ground: grass, roads, sidewalks, lots, outdoor floors and snow (1 = the default).");
        RegisterFloatSetting(&g_moonlight, "luar", SettingWidget::Slider, 0.0f, 0.0f, 2.0f,
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
        RegisterBoolSetting(&g_streetLampsLit, "postesAcesosNoCalculo", true,
            S3SS_TR("Quando o jogo calcula a luz de um lote, os postes da rua contam como acesos (igual a carregar o save a noite).",
                    "Street lamps count as lit when the game solves a lot's light."));
        RegisterBoolSetting(&g_allLotsHQ, "qualidadeAltaEmTodosOsLotes", false,
            S3SS_TR("Todos os lotes usam a qualidade de luz alta do lote ativo (corrige o corte da luz dos postes na divisa). Vale para lotes carregados depois de ligar.",
                    "Experimental: every lot uses the active lot's high lighting quality."));
        RegisterBoolSetting(&g_lotPassNoTerrainMap, "gramaDoLoteUsaLuzDoLote", true,
            S3SS_TR("A grama do lote continua usando a luz do proprio lote mesmo depois de refazer o terreno (a luz dos postes nao some dentro do lote).",
                    "Lot grass keeps the lot's own light after a terrain rebuild."));
        RegisterBoolSetting(&g_relightLots, "recalcularLotesAoAnoitecer", true,
            S3SS_TR("Depois de refazer o terreno a noite, recalcula a luz de todos os lotes como quando um poste e movido no modo construcao.",
                    "After the dusk terrain rebuild, re-solve the light of every lot."));
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
        SetBakeGains(1.0f, 1.0f);
        if (kBakeColourSite && Missing({Id::BakeColourSite}).empty() &&
            MemPatch::ValidateBytes(reinterpret_cast<LPVOID>(kBakeColourSite), kBakeColourOrig.data(), kBakeColourOrig.size())) {
            // call BakeColourStub; nop x2
            const auto bakeBytes = CallPatch(kBakeColourSite, 0, {}, reinterpret_cast<void*>(&BakeColourStub), kBakeColourOrig.size());
            g_bakeGainInstalled = MemPatch::WriteBytes(kBakeColourSite, bakeBytes, &patchedLocations, &kBakeColourOrig);
            if (g_bakeGainInstalled) FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(kBakeColourSite), kBakeColourOrig.size());
        }
        // Low lamps on the ground (optional: without it the bake stamps them from their real height)
        g_bakePosInstalled = false;
        kBakePosSite = kBakeColourSite ? kBakeColourSite - kBakePosBeforeColour : 0;
        if (g_bakeGainInstalled && MemPatch::ValidateBytes(reinterpret_cast<LPVOID>(kBakePosSite), kBakePosOrig.data(), kBakePosOrig.size())) {
            // call BakePosStub; nop x5
            const auto posBytes = CallPatch(kBakePosSite, 0, {}, reinterpret_cast<void*>(&BakePosStub), kBakePosOrig.size());
            g_bakePosInstalled = MemPatch::WriteBytes(kBakePosSite, posBytes, &patchedLocations, &kBakePosOrig);
            if (g_bakePosInstalled) FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(kBakePosSite), kBakePosOrig.size());
        }
        g_lowLiftSeen = TerrainBakeRule::LowLampLift();
        // Day and night in Build mode (optional: without it Apex gates only the level it reads; the game's lamps fade)
        NightLevel::g_timesHooked = false;
        g_sunTimesCall = 0;
        {
            BYTE* base = nullptr;
            size_t size = 0;
            const uintptr_t at = MemPatch::GetModuleInfo(GetModuleHandleW(nullptr), &base, &size) ? MemPatch::ScanPattern(base, size, kSunTimesPattern) : 0;
            const uintptr_t call = at ? at + 10 : 0;
            int32_t rel = 0;
            if (call && MemPatch::ReadBytes(call + 1, reinterpret_cast<BYTE*>(&rel), 4) &&
                MemPatch::ValidateBytes(reinterpret_cast<LPCVOID>(call + 5 + rel), kSunTimesStart, sizeof(kSunTimesStart))) {
                g_sunTimes = reinterpret_cast<SunTimesFn>(call + 5 + rel);
                g_sunTimesOrig.assign(reinterpret_cast<const BYTE*>(call), reinterpret_cast<const BYTE*>(call) + 5);
                const auto timesBytes = CallPatch(call, 0, {}, reinterpret_cast<void*>(&SunTimesHook), 5);
                if (MemPatch::WriteBytes(call, timesBytes, &patchedLocations, &g_sunTimesOrig)) {
                    FlushInstructionCache(GetCurrentProcess(), reinterpret_cast<void*>(call), 5);
                    g_sunTimesCall = call;
                    NightLevel::g_timesHooked = true;
                }
            }
            if (g_sunTimesCall) LOG_INFO(std::format("[NightTerrainRelight] Day and night: the sky's copy of the sun times at {:#x} goes through Apex (Build mode keeps the season's)", g_sunTimesCall));
            else LOG_WARNING("[NightTerrainRelight] Day and night: the sky's copy of the sun times was not found; Apex keeps the night only for its own lamp terms in Build mode");
        }
        if (g_bakePosInstalled) LOG_INFO("[NightTerrainRelight] Terrain bake: lot lamps on the ground are stamped from 40 cm over their lot's ground");
        else LOG_WARNING("[NightTerrainRelight] Terrain bake position code differs or was not found: lamps on the ground light the lawn from their real height");
        // the bake's lamp cones: Apex's copy of its pixel shader is bound wherever the game binds its own in a bake (both stubs
        // needed: the position stub writes each light's cone rows, the colour stub then leaves the spill to the shader).
        // Last: the other features saw the game's shader first.
        if (g_bakeGainInstalled && g_bakePosInstalled) {
            g_coneScale5 = ReadConeScale5();
            D3D9Hooks::RegisterSetPixelShader("NightTerrainRelight", [](D3D9Hooks::DeviceContext& ctx, IDirect3DPixelShader9* ps) {
                if (g_bakeRowsHeld && (!ps || ps != g_gameBakePs)) GiveBackBakeRows(); // the bake's passes are over
                if (!ps || g_coneFailed || (ps != g_gameBakePs && (g_gameBakePs || !g_inBakeLight))) return D3D9Hooks::HookAction::Continue;
                g_inBakeLight = false;
                return BindConeShader(ctx.device, ps) ? D3D9Hooks::HookAction::Skip : D3D9Hooks::HookAction::Continue;
            }, D3D9Hooks::Priority::Last);
            g_bakeConeHooked = true;
        }
        LevelLightShare::SetBakeConeSpill(g_bakeGainInstalled && g_bakeConeSpill);
        if (g_bakeGainInstalled && g_bakeConeSpill) LOG_INFO("[NightTerrainRelight] Terrain bake: lamps with cones (type 5) are stamped at their spill");
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
            // 07/10, players' Runtime Error: each part's frame update is caught on its own (HookGuard), so one that throws is
            // turned off and the others keep running; the settings pushed in between are plain stores
            HookGuard::Run("Night Lighting frame update", [] { OnPresent(); });
            HookGuard::Run("Light Probe frame update", [&ctx] { LightProbe::OnPresent(ctx.device); }); // the light capture (F7): the draws painting the pixel under the mouse
            HookGuard::Run("Screen watch frame update", [&ctx] { ScreenWatch::OnPresent(ctx.device); }); // the recording's screen pixels (light update trace)
            ObjectLightBridge::SetStrength(g_objStrength);
            ObjectLightBridge::SetAllObjects(g_objAll);
            HookGuard::Run("ObjectLightBridge frame update", [] { ObjectLightBridge::OnPresent(); });
            LevelLightShare::SetIndoor(g_indoorShare);
            LevelLightShare::SetWallAlign(g_wallAlign);
            LevelLightShare::SetAllFloors(g_allFloors);
            LevelLightShare::SetAllLotsHighQuality(g_allLotsHQ);
            LevelLightShare::SetFloorWalls(g_floorWalls);
            LevelLightShare::SetRealisticOpenings(g_realisticOpenings);
            LevelLightShare::SetOutdoorThroughOpenings(g_outdoorOpenings);
            if (!MenuSliderHeld()) // (applied once the slider is let go: a change relights every room)
                LevelLightShare::SetOpeningLampBalance(GroundGain(), 1.0f, g_bridge, g_wallStrength, g_walls);
            LevelLightShare::SetDoorsBlockLight(g_doorsBlock);
            LevelLightShare::SetIndoorThroughOpenings(g_indoorOpenings);
            LevelLightShare::SetIndoorLightOnGround(g_indoorGround);
            if (!MenuSliderHeld()) LevelLightShare::SetShadowSoftness(g_shadowSoftness); // (applied once the slider is let go)
            LevelLightShare::SetRealisticReach(g_realisticReach);
            LevelLightShare::SetObjectWalls(g_objectWalls);
            LevelLightShare::SetGroundShade(g_groundShade);
            // Smooth light maps, each part also behind its developer Tester switch (tester_toggles.h)
            const bool softCut = g_smoothRoomMaps && TesterToggles::smoothCut.load(std::memory_order_relaxed);
            const bool cutRelights = softCut != LevelLightShare::SoftLightCutOn(); // (that change relights every room already)
            LevelLightShare::SetSoftLightCut(softCut);
            {
                const bool dither = g_smoothRoomMaps && TesterToggles::smoothDither.load(std::memory_order_relaxed);
                static int s_dither = -1; // (the texels change at the rooms' next solve: a change relights them, not the first value)
                if (s_dither >= 0 && (s_dither != 0) != dither && RoomMapDither::Enabled() != dither && LevelLightShare::IsInstalled() && !cutRelights)
                    LevelLightShare::RelightAllRooms(dither ? "Smooth light maps (dither) on" : "Smooth light maps (dither) off");
                s_dither = dither ? 1 : 0;
                RoomMapDither::SetEnabled(dither);
            }
            LotLightBridge::SetSmoothRoomMaps(g_smoothRoomMaps && TesterToggles::smoothBSpline.load(std::memory_order_relaxed));
            LevelLightShare::SetFoundationWalls(g_foundationWalls);
            UnlitRooms::Set(g_unlitOn, g_unlitLight, g_unlitBlue);
            UnlitRooms::SetNightLevel(std::fmax(g_menuLevel.load(), 0.0f));
            HookGuard::Run("UnlitRooms frame update", [] { UnlitRooms::OnPresent(); });
            HookGuard::Run("LampMarkFilter frame update", [] { LampMarkFilter::OnPresent(g_menuLevel.load()); });
            if (DWORD at = g_autoRefreshAt.load(); at && static_cast<int32_t>(GetTickCount() - at) >= 0 && !g_reinstallDue.load() &&
                g_autoRefreshAt.compare_exchange_strong(at, 0)) {
                const bool afterLoad = g_autoRefreshRoomsOnly.load();
                const DWORD tick = GetTickCount();
                const bool busy = afterLoad && tick - g_afterLoadStarted < kAfterLoadMax && LevelLightShare::LoadedRoomsBusy();
                const bool ready = !afterLoad || RoomAmbientPolicy::AfterLoadRefreshReady(tick, g_afterLoadStarted, busy, g_afterLoadQuiet);
                if (ready) {
                    if (afterLoad && !g_loadSettled.exchange(true)) {
                        LOG_INFO("[NightTerrainRelight] Load settled: the after-load refresh runs now");
                        LoadTiming::NoteLoadSettled("the after-load refresh ran");
                    }
                    g_autoRefreshRoomsOnly.store(false);
                    HookGuard::Try("Night Lighting automatic refresh",
                                   [afterLoad] { NightLighting::RefreshAll(afterLoad ? "after loading" : "a setting changed", !afterLoad); });
                } else g_autoRefreshAt.store((tick + 200) | 1);
            }
            HookGuard::Run("LevelLightShare frame update", [] { LevelLightShare::OnPresent(); });
            LotLightBridge::SetNightLevel(g_level);
            LotLightBridge::SetRigNightLevel(g_levelStep); // the rigs' reach law keeps the game's steps (one re-gather per step)
            ProbeNeutral::SetNightLevel(g_level);
            ProbeNeutral::Set(g_neutralProbe, g_neutralProbeStrength);
            LotLightBridge::SetRoofFix(g_roofs, g_roofStrengthSetting);
            // The lake pass draws the lamp glow AND the shore reflection: it runs while either is wanted. With the glow
            // switched off its lamp strength is 0, so the pass adds the reflection alone (menu: Water Reflections card);
            // the reflection alone needs the scene depth (Depth Blur), without it the pass would add nothing.
            const bool shoreOnly = !g_water && g_waterReflSetting > 0.0f && DepthShare::Texture() != nullptr;
            LotLightBridge::SetWaterFix(g_water || shoreOnly, g_water ? std::clamp(g_waterStrengthSetting, 0.1f, 0.4f) : 0.0f, g_waterReflSetting, g_waterFilter, g_waterColorCompression);
            HookGuard::Run("LotLightBridge frame update", [] { LotLightBridge::OnPresent(); });
            // Lamp lighting: Instant = Real-time outdoor lamps, which takes its base from the parts Quick outdoor lamp edits keeps
            // (the delta itself then also brings room 0 up to date after an outdoor edit the GPU does not draw). Both need the room
            // solve hooks (LevelLightShare). Classic: neither runs, nothing of them is installed.
            {
                const bool instant = g_lampMode == kLampInstant;
                // Instant chosen and able to run (14/10: Instant is the default, so a device that cannot run it must not get
                // its parts half on): the delta follows it, not the raw choice
                const bool rtOn = instant && RtOutdoorLamps::Unsupported().empty();
                // Classic in effect (Classic chosen, Instant unable to run, or Instant doing nothing now: its menu note): a
                // lamp drag relights after its release and an edit's stories change together (DragLite, AtriumHold)
                DragLite::SetClassicLighting(!rtOn || !RtOutdoorLamps::InactiveNote().empty());
                OutdoorDelta::SetEnabled(rtOn && LevelLightShare::IsInstalled() && LevelLightShare::DeltaReady());
                // (reports why it does nothing when it cannot run: its status, the log and the menu's note)
                RtOutdoorLamps::SetEnabled(instant);
                // A mode change relights the active lot once (the menu's note says so): Instant captures its base from those
                // solves, Classic gives every room the game's solve again
                static int s_appliedMode = -1;
                if (s_appliedMode >= 0 && s_appliedMode != (instant ? 1 : 0) && LevelLightShare::IsInstalled()) {
                    uintptr_t trackers[4];
                    const int n = LevelLightShare::ActiveLotTrackers(trackers, 4);
                    int sent = 0;
                    for (int i = 0; i < n; i++) sent += std::max(0, LevelLightShare::RelightLot(trackers[i], "lamp lighting mode changed"));
                    LOG_INFO(std::format("[NightTerrainRelight] Lamp lighting now {}: {} rooms of {} active lots sent to their solve", instant ? "instant" : "classic", sent, n));
                }
                s_appliedMode = instant ? 1 : 0;
                HookGuard::Run("RtOutdoorLamps frame update", [&ctx] { RtOutdoorLamps::OnPresent(ctx.device); });
                OutdoorDelta::SetSelfCheck(rtOn && kDevToolsBuild && !kPublicBuild && g_outdoorDeltaCheck);
                if (rtOn || OutdoorDelta::Enabled()) HookGuard::Run("OutdoorDelta frame update", [] { OutdoorDelta::OnPresent(); });
            }
            LightmapSmooth::SetEnabled(g_smoothMaps);
            LotLightBridge::SetSidewalkClear(g_sidewalkClear);
            LotLightBridge::SetGroundBrightness(GroundGain(), 1.0f, 1.0f);
            RoomMapPadding::SetEnabled(g_edgePad);
            LotLightBridge::SetIndoorSmooth(g_edgePad);
            HookGuard::Run("RoomMapPadding frame update", [] { RoomMapPadding::OnPresent(); });
            // lamps created from now on take the new colour once the slider is let go (OnPresent re-colours the others)
            if (!MenuSliderHeld()) ObjectLightBridge::SetLampTint(g_lampTint, g_lotTintOwn ? g_lotLampTint : g_lampTint);
            LotLightBridge::SetFenceGroundLight(g_fenceGround, g_fenceGroundStrength);
            LotLightBridge::SetWallGain(g_wallStrength, g_walls);
            LotLightBridge::SetObjectPixelLamps(g_objPixel && RigTracker::IsInstalled(), g_objStrength);
            LotLightBridge::SetObjectPixelLights(g_objPixelLights, g_objPixelLightStrength);
            LotLightBridge::SetWindowOutdoor(g_windowOutdoor);
            LightmapSmooth::SetGpuPreferred(g_smoothMapsGpu);
            LotLightBridge::SetSoftLotEdges(g_softLotEdges);
            HookGuard::Run("LightmapSmooth frame update", [&ctx] { LightmapSmooth::OnPresent(ctx.device); });
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
        {
            std::string probeErr;
            if (!ProbeNeutral::Install(probeErr)) LOG_WARNING("[NightTerrainRelight] " + probeErr);
        }
        // Light detail: once per session, before any lot is lit (a later change takes effect after a restart)
        LightDetail::ApplyAtStartup(g_lightDetail);
        if (!TreeLightRig::Installed()) { // trees out of the game's underground light rig (tree_light_rig.h)
            std::string treeErr;
            if (!TreeLightRig::Install(&treeErr)) LOG_WARNING("[TreeLightRig] Not used: " + treeErr);
        }
        if (g_levelShare && !LevelLightShare::IsInstalled()) {
            std::string shareErr;
            if (!LevelLightShare::Install(shareErr)) LOG_WARNING("[NightTerrainRelight] " + shareErr);
        }
        if (LevelLightShare::IsInstalled() && !RoomMapDither::Running()) { // the rooms Apex solves
            std::string ditherErr;
            if (!RoomMapDither::Start(ditherErr)) LOG_WARNING("[NightTerrainRelight] " + ditherErr);
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
        OutdoorDelta::SetEnabled(false);
        RtOutdoorLamps::SetEnabled(false); // (its EndScene pass drops the relit maps at the next frame)
        ObjectLightBridge::UninstallLampColour();
        ObjectLightBridge::Uninstall();
        if (!reinstalling) UnlitRooms::Uninstall(); // first: its last retint still queues rooms with the solve deferral (review M5)
        if (!reinstalling) LampMarkFilter::Uninstall();
        if (!reinstalling) ProbeNeutral::Uninstall();
        if (!reinstalling) LevelLightShare::Uninstall();
        if (!reinstalling) TreeLightRig::Uninstall();
        if (!reinstalling) RoomMapDither::Stop();
        if (!reinstalling) RigTracker::Uninstall();
        if (!MemPatch::RestoreAll(patchedLocations)) return Fail(S3SS_TR("Falha ao restaurar os bytes originais", "Could not restore the original code"));
        FlushInstructionCache(GetCurrentProcess(), nullptr, 0);
        patchedLocations.clear();
        g_chunkHookInstalled = false;
        g_bakeGainInstalled = false;
        g_bakePosInstalled = false;
        g_bakeConeHooked = false; // (the hook went with UnregisterAll: the game's shader binds again)
        g_bakeConeShader = false;
        g_bakeRowsHeld = false;
        NightLevel::g_timesHooked = false;
        g_sunTimesCall = 0;
        LevelLightShare::SetBakeConeSpill(false);
        // The chunk re-render call is the game's own again: a local relight in progress is dropped (the chunk in flight, if
        // any, is rendered by the game at its next update). Its lamps were never marked as baked: the change is decided
        // again (the diff against the snapshot still shows it). A paced sweep in progress is cut short: its snapshot was
        // taken when it started, so the chunks it did not reach get a rebuild without the snapshot compare (a switch).
        ChunkRelight::SetHooked(false);
        const bool localPending = !g_localBatches.empty(), sweepPending = g_sweepId != 0;
        ChunkRelight::Drop();
        g_localBatches.clear();
        g_sweepId = 0;
        g_rebuildId = 0;
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
        if (std::chrono::steady_clock::now() - lastSettingChange.load() < SETTING_CHANGE_DEBOUNCE) return;
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
            TreeLightRig::Uninstall();
            RoomMapDither::Stop();
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
    // A config, profile or look saved with the four ground controls (ground, roads, street lamps, lot lamps): the ground
    // light they gave the terrain (ground x lot lamps) over Soft's; roads and street lamps follow the ground now
    static void MigrateGround(const toml::table& table) {
        if (table.contains("luzNoChao") || !(table.contains("brilhoNoChao") || table.contains("forcaDasLampadasDoLote"))) return;
        const double ground = table["brilhoNoChao"].value_or(0.75), lot = table["forcaDasLampadasDoLote"].value_or(0.8);
        g_groundLight = std::clamp(static_cast<float>(ground * lot / kGroundSoft), 0.25f, 3.0f);
    }
    bool LoadFromToml(const toml::table& table) override {
        MigrateGround(table); // before the base: it may install, which applies the ground light
        return ApexPatch::LoadFromToml(table);
    }

    void ApplyTableLive(const toml::table& table) override {
        const bool wasEnabled = isEnabled.load();
        const auto changedBefore = lastSettingChange.load();
        const bool bridgeBefore = g_bridge, objBefore = g_objLamps, shareBefore = g_levelShare, objPixelBefore = g_objPixel;
        const float groundBefore = g_groundLight;
        MigrateGround(table);
        if (g_groundLight != groundBefore) NotifySettingChanged();
        ApexPatch::ApplyTableLive(table);
        if (wasEnabled && isEnabled.load()) ApplyLive(bridgeBefore, objBefore, shareBefore, objPixelBefore);
        if (wasEnabled && isEnabled.load() && lastSettingChange.load() != changedBefore) RequestAutoRefresh(); // a profile, look or undo
    }

    static void ResetDefaults() {
        g_bridge = true;
        g_lotLamps = true;
        g_autoDusk = true;
        g_levelShare = true;
        g_indoorShare = true;
        g_wallAlign = true;
        g_allFloors = true;
        g_floorWalls = true;
        g_realisticOpenings = kRealisticOpeningsDefault;
        g_outdoorOpenings = true;
        g_doorsBlock = true;
        g_indoorOpenings = true;
        g_indoorGround = true;
        g_shadowSoftness = 0.15f;
        g_objectWalls = true;
        g_groundShade = true;
        g_smoothRoomMaps = true;
        g_foundationWalls = true;
        g_neutralProbe = true;
        g_neutralProbeStrength = 1.0f;
        g_lightDetail = 0;
        g_unlitOn = true;
        g_unlitLight = 0.20f;
        g_unlitBlue = 0.0f;
        g_smoothMaps = true;
        g_smoothMapsGpu = true;
        g_softLotEdges = true;
        g_lampMode = kLampInstant;
        g_outdoorDeltaCheck = false;
        g_lampTint = 1.0f;
        g_lotTintOwn = false;
        g_lotLampTint = 1.0f;
        g_edgePad = true;
        g_groundLight = 1.0f;
        g_moonlight = 0.0f;
        g_objLamps = true;
        g_objStrength = 0.75f;
        g_objAll = true;
        g_objPixel = true;
        g_objPixelLights = true;
        g_objPixelLightStrength = 0.75f;
        g_windowOutdoor = true;
        g_fenceGround = true;
        g_fenceGroundStrength = 0.75f;
        g_walls = true;
        g_wallStrength = 0.30f;
        g_realisticReach = true;
        g_roofs = true;
        g_roofStrengthSetting = 0.45f;
        g_water = true;
        g_waterFilter = g_waterColorCompression = true;
        g_waterStrengthSetting = 0.4f;
        g_waterReflSetting = 1.56f;
        g_sidewalkClear = 0.5f;
        g_delaySec = 2.0f;
        g_streetLampsLit = true;
        g_allLotsHQ = false;
        g_lotPassNoTerrainMap = true;
        g_relightLots = true;
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
    // each: ground light, objects, pieces, fences, walls, roofs (the StyleValue order)
    static constexpr int kStyleValues = 6;
    struct LightStyle {
        float v[kStyleValues];
    };
    // Preserve the approved Soft surface ratios; variants change intensity conservatively. Ground light: the four ground
    // controls' product (ground x lot lamps) over Soft's 0.6.
    static constexpr LightStyle kStyles[] = {
        {{0.81f, 0.675f, 0.675f, 0.675f, 0.27f, 0.405f}}, // Subtle (walls 27%, Soft 30% scaled; the others keep the ratio)
        {{1.0f, 0.75f, 0.75f, 0.75f, 0.30f, 0.45f}},      // Soft reference (the defaults; walls 30%)
        {{1.67f, 1.0f, 1.0f, 1.0f, 0.56f, 0.6f}},         // Natural (walls 56%)
    };
    static float* StyleValue(int i) {
        float* const v[] = {&g_groundLight, &g_objStrength, &g_objPixelLightStrength, &g_fenceGroundStrength, &g_wallStrength, &g_roofStrengthSetting};
        return v[i];
    }
    static int CurrentStyle() {
        for (int s = 0; s < static_cast<int>(std::size(kStyles)); s++) {
            const float* want = kStyles[s].v;
            bool same = true;
            for (int i = 0; i < kStyleValues && same; i++) same = std::fabs(*StyleValue(i) - want[i]) < 0.005f;
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
                    for (int j = 0; j < kStyleValues; ++j) { previous.v[j] = *StyleValue(j); *StyleValue(j) = kStyles[i].v[j]; }
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
                    for (int j = 0; j < kStyleValues; ++j) *StyleValue(j) = previous.v[j];
                    canUndo = false;
                    changed = true;
                    ApexUi::ReportChange("Lighting balance restored");
                }
            }
            // (2.13.1) it shapes walls, trees and bushes, not only buildings: here, beside the balance
            ApexUi::CardDivider();
            changed |= ApexUi::SwitchRow("Realistic lamp reach", &g_realisticReach, "Lamp light on outside walls, trees and bushes fades with distance, from each lamp's side", true);
            return changed;
        });
    }

    // Lighting > Ground
    // Lighting > Stories: everything about lamp light between the floors of a house (its own
    // tab)
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

                changed |= ApexUi::SwitchRow("Indoor light between floors", &g_indoorShare, "Lamps inside shine through stairwells and open floors; light bounces up the stairs", true);
                // The game solves rooms one after the other (after a load, a change or a lamp switched), so
                // a room can show its old light for a moment; switching floors solves the rooms shown again
                if (g_indoorShare) ApexUi::IconNote(ApexUi::IconId::Info, "If a room looks wrong, use Refresh lights (Overview tab)");
                changed |= ApexUi::SwitchRow("Walls block light on floors", &g_floorWalls, "Decks and yards without a roof stay dark behind a wall", true);
                ImGui::BeginDisabled(!g_floorWalls); // its floors rely on the walls' mask
                changed |= ApexUi::SwitchRow("Light through doors and windows", &g_realisticOpenings, "Lamps light walled yards only through openings or over the walls",
                                             kRealisticOpeningsDefault);
                ImGui::EndDisabled();
                // (the light through openings, doors and shadow softness: Rooms at Night, right below; the fixes: Game fixes tab)
                ImGui::EndDisabled();
                return changed;
            });
        }
        ApexUi::EndCard();
        ImGui::PopID();
    }

    // Lighting > Game fixes: one card per area. These correct game bugs, are on by default and stay switchable, so the
    // look tabs keep only the choices that change the look.
    void DrawGameFixes() {
        using ApexUi::IconId;
        ApexUi::MutedText("Fixes for game bugs. They are on by default; turn one off only to compare with the game.");
        ApexUi::Gap(ApexUi::kSpace2);
        ImGui::PushID("NightGroundFixes");
        if (ApexUi::BeginCard("##Card")) {
            ApexUi::CardHeader(IconId::LandPlot, "Ground & Lots", "Lamp light on grass, streets and lots", nullptr, nullptr);
            ApexUi::CardDivider();
            Edit([] {
                bool changed = ApexUi::SwitchRow("Street lamps light lots", &g_bridge, "Street lamp light flows onto lots with no hard edge", true);
                changed |= ApexUi::SwitchRow("Lamp colors stay near lamps", &g_neutralProbe, "A colored lamp no longer tints far floors, roofs and shiny objects", true);
                return changed;
            });
            // Smooth ground light only chooses which map the draws read (idavidveiga's fork): no lighting refresh, no
            // terrain, lot, room or wall solve and no object rig gather; the next draw reads the smoothed or the raw map
            if (ApexUi::SwitchRow("Smooth ground light", &g_smoothMaps, "Soft lamp light on the ground, without blocky steps or specks", true)) {
                NotifySettingChanged();
                LOG_INFO("[NightTerrainRelight] Smooth ground light changed: draw maps only; object rigs, terrain, lots, rooms and walls left untouched");
            }
            // When the ground light is rebuilt (lamp changes are always followed; this is the rebuild at dusk)
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
        }
        ApexUi::EndCard();
        ImGui::PopID();

        // The game's fixes on stairs, doors, counters, fences and indoor furniture
        ImGui::PushID("NightObjectFixes");
        if (ApexUi::BeginCard("##Card")) {
            ApexUi::CardHeader(IconId::Armchair, "Objects", "Lamp light on plants and outdoor furniture", nullptr, nullptr);
            ApexUi::CardDivider();
            Edit([] {
                ImGui::BeginDisabled(!g_objLamps);
                ApexUi::SetNextRowBadge(kReloadBadge, kReloadTip); // the game builds these pieces' light when a world loads
                bool changed = ApexUi::SwitchRow("Light stairs, railings, columns", &g_objAll, "Pieces the game leaves unlit", true);
                ImGui::EndDisabled();
                // These read the ground light of two ground fixes (lot light bridge + smoothed maps), in the card above
                const bool groundLight = g_bridge && g_smoothMaps;
                if (!groundLight) {
                    const bool both = !g_bridge && !g_smoothMaps;
                    ApexUi::IconNote(ApexUi::IconId::Info, both ? "Needs \"Street lamps light lots\" and \"Smooth ground light\""
                                                                : !g_bridge ? "Needs \"Street lamps light lots\"" : "Needs \"Smooth ground light\"");
                    if (ApexUi::IconTextButton(both ? "Turn both on##GroundLight" : "Turn it on##GroundLight", ApexUi::IconId::LandPlot, nullptr, ApexUi::ButtonKind::Primary)) {
                        ApexUi::ReportChange(both ? "Ground light turned on" : !g_bridge ? "Street lamps light lots turned on" : "Smooth ground light turned on");
                        g_bridge = true;
                        g_smoothMaps = true;
                        changed = true;
                    }
                }
                ImGui::BeginDisabled(!groundLight);
                changed |= ApexUi::SwitchRow("Doors and windows stay lit", &g_objPixel, "A front door is never darker than the wall around it", true);
                ImGui::BeginDisabled(!g_objPixel);
                changed |= ApexUi::SwitchRow("Windows take outdoor light", &g_windowOutdoor, "The outer side of windows lit by the room takes the outdoor light", true);
                ImGui::EndDisabled();
                changed |= ApexUi::SwitchRow("Seamless light on pieces", &g_objPixelLights, "Counters and modular pieces outside show no color steps", true);
                if (g_objPixelLights)
                    changed |= ApexUi::SliderPercent("Seamless light brightness", &g_objPixelLightStrength, 0.25f, 3.0f, "Lamp light on outdoor objects and trees, over the walls' brightness", 0.75f);
                changed |= ApexUi::SwitchRow("Fences and stairs catch light", &g_fenceGround, "Fences, posts, stairs and their snow match the lit ground", true);
                if (g_fenceGround)
                    changed |= ApexUi::SliderPercent("Fence brightness", &g_fenceGroundStrength, 0.25f, 2.0f, "Balance fences against the surrounding ground", 0.75f);
                ImGui::EndDisabled();
                changed |= ApexUi::SwitchRow("Smooth indoor light", &g_edgePad, "Stairs, curtains and furniture take their room's light and color smoothly; no dark sides", true);
                return changed;
            });
        }
        ApexUi::EndCard();
        ImGui::PopID();

        // Walls and light maps between floors: off with "Outdoor light between floors" (Inside & floors tab), as before
        ImGui::PushID("NightStoryFixes");
        if (ApexUi::BeginCard("##Card")) {
            ApexUi::CardHeader(IconId::Layers, "Stories", "Lamp light between the floors of a house", nullptr, nullptr);
            ApexUi::CardDivider();
            if (!g_levelShare) ApexUi::IconNote(ApexUi::IconId::Info, "Needs \"Outdoor light between floors\"");
            ImGui::BeginDisabled(!g_levelShare);
            Edit([] {
                bool changed = ApexUi::SwitchRow("Wall lamps at the right height", &g_foundationWalls, "Fixes a game bug: wall light too high on tall walls and foundations", true);
                changed |= ApexUi::SwitchRow("Seamless walls between floors", &g_wallAlign, "Walls above and below the floor line meet with no step in the light", true);
                changed |= ApexUi::SwitchRow("Every floor in full detail", &g_allFloors, "Changing floors keeps the light; entering a lot takes a little longer", true);
                changed |= ApexUi::SwitchRow("Smooth light maps", &g_smoothRoomMaps, "Lamp light in rooms without blocks, bands or rings; floors blend across arches", true);
                return changed;
            });
            ImGui::EndDisabled();
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
                ApexUi::CardGroup("Where the light goes");
                bool changed = ApexUi::SwitchRow("Lot lamps light the street", &g_lotLamps, "Outdoor lot lamps also light the grass and street nearby", true);
                changed |= ApexUi::SwitchRow("Walls shade lamp light near the ground", &g_groundShade, "Walls, garden walls, trees and bushes shade lamp light on the ground, also past the lot", true);
                return changed;
            });
            // (the game's fixes and the dusk update: Game fixes tab). The former Ground intensity card, as a group
            Edit([] {
                ApexUi::CardGroup("Brightness");
                // The ground and road gains are applied in the draws of "Street lamps light lots"
                if (!g_bridge) ApexUi::IconNote(ApexUi::IconId::Info, "Needs \"Street lamps light lots\" (Game fixes tab)");
                ImGui::BeginDisabled(!g_bridge);
                const bool changed = ApexUi::SliderPercent("Ground light", &g_groundLight, 0.25f, 3.0f, "Lamp light on grass, roads, sidewalks, lots, patios and snow", 1.0f);
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
                bool changed = ApexUi::SwitchRow("Lamps light objects", &g_objLamps, "Trees, bushes and outdoor objects take lamp light, even in moon shadow", true);
                if (g_objLamps)
                    changed |= ApexUi::SliderPercent("Brightness##Objects", &g_objStrength, 0.25f, 3.0f, "Ground light on outdoor objects; lamps follow the walls' brightness", 0.75f);
                changed |= ApexUi::SwitchRow("Buildings block lamp light on trees and objects", &g_objectWalls, "Trees, objects and fences behind a wall or balcony get no lamp light", true);
                return changed;
            });
            // (the game's fixes on stairs, doors, counters, fences and indoor furniture: Game fixes tab)
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
                ApexUi::CardGroup("Walls");
                bool changed = ApexUi::SwitchRow("Lamps light walls", &g_walls, "Outside walls near lamps get brighter; off keeps the game's dim walls", true);
                if (g_walls)
                    changed |= ApexUi::SliderPercent("Brightness##Walls", &g_wallStrength, 0.25f, 4.0f, "Lamp light on outside walls; trees and objects too with Realistic lamp reach", 0.30f);
                ApexUi::CardGroup("Roofs");
                changed |= ApexUi::SwitchRow("Lamps light roofs", &g_roofs, "Roofs no longer stay black at night; softer roof shadows too", true);
                if (g_roofs)
                    changed |= ApexUi::SliderPercent("Brightness##Roofs", &g_roofStrengthSetting, 0.05f, 2.0f, "Intensity of lamp light on roofs", 0.45f);
                return changed;
            });
        }
        ApexUi::EndCard();
        ImGui::PopID();
    }

    // Lighting > Buildings: how sharp the baked lamp light on walls and floors is (the game's lamp
    // light looks low resolution; see light_detail.cpp). Read at startup only.
    void DrawLightDetailCard() {
        ImGui::PushID("NightLightDetail");
        if (ApexUi::BeginCard("##Card")) {
            ApexUi::CardHeader(ApexUi::IconId::Gem, "Light detail", "How sharp lamp light is on walls and floors", nullptr, nullptr);
            ApexUi::CardDivider();
            Edit([] {
                static const char* const kLevels[] = {"Game", "High"};
                static const char* const kTips[] = {"The game's own detail", "Twice the detail: rooms take longer to light and use more memory"};
                const bool changed = ApexUi::SegmentedRow("Detail", "Sharper light pools and shadow edges on walls and floors", "##LightDetail", &g_lightDetail, kLevels, 2, kTips,
                                                          nullptr, 0);
                if (g_lightDetail != LightDetail::Active()) ApexUi::IconNote(ApexUi::IconId::Info, "Restart the game to apply");
                return changed;
            });
        }
        ApexUi::EndCard();
        ImGui::PopID();
    }

    // Lighting > Buildings: rooms with every lamp off (the game leaves them very blue, S3SS's switch
    // makes them far too dark; a control in between)
    void DrawRoomsCard() {
        ImGui::PushID("NightRooms");
        if (ApexUi::BeginCard("##Card")) {
            ApexUi::CardHeader(ApexUi::IconId::Moon, "Rooms at Night", "Background and lamp light inside rooms", nullptr, nullptr);
            ApexUi::CardDivider();
            Edit([] {
                ApexUi::CardGroup("Background light");
                bool changed = ApexUi::SwitchRow("Adjust the background light", &g_unlitOn, "Set the ambient glow indoors, with lamps on or off", true);
                if (g_unlitOn) {
                    changed |= ApexUi::SliderPercent("Brightness##Unlit", &g_unlitLight, 0.1f, 0.8f, "How bright that background light is, on walls and furniture", 0.20f);
                    changed |= ApexUi::SliderPercent("Blue tint##Unlit", &g_unlitBlue, 0.0f, 1.0f, "0% is neutral grey, 100% is the game's blue, on walls and furniture", 0.0f);
                }
                return changed;
            });
            // S3SS's saved room colour: Rooms at Night already uses the game's blue in its place; removing it is on the Attention page
            if (g_unlitOn && S3SSDetect::SavedRoomAmbientOverride())
                ApexUi::IconNote(ApexUi::IconId::Puzzle, "Sims3SettingsSetter saves its own room color; this replaces it (Attention page)");
            // 14/10, the approved player structure: lamp light through openings and its look. Each switch is one setting; the
            // parts inside them (roofed rooms taking the story below's lamps; the fade at the light cut, the dither and the
            // smooth reads) stay switchable only on Developer > Tester, on by default.
            // (they shape the light shared between stories: off with "Outdoor light between floors", in the Stories card above)
            ApexUi::CardGroup("Lamp light");
            if (!g_levelShare) ApexUi::IconNote(ApexUi::IconId::Info, "Needs \"Outdoor light between floors\"");
            ImGui::BeginDisabled(!g_levelShare);
            Edit([] {
                bool changed = ApexUi::SwitchRow("Outdoor light through openings", &g_outdoorOpenings, "Garden lamps light rooms through doors, windows and arches", true);
                changed |= ApexUi::SwitchRow("Indoor light through openings", &g_indoorOpenings, "Lit rooms shine through arches and windows onto the garden and the next room", true);
                ImGui::BeginDisabled(!g_indoorOpenings || !g_floorWalls); // (2.15.3) a part of the light above, kept in the floor maps' alpha
                changed |= ApexUi::SwitchRow("Indoor light adds to the ground", &g_indoorGround, "Window and door light on the lot ground adds to the garden lamps", true);
                ImGui::EndDisabled();
                ImGui::BeginDisabled(!g_outdoorOpenings && !g_indoorOpenings && !(g_realisticOpenings && g_floorWalls)); // it shapes the options above
                changed |= ApexUi::SwitchRow("Doors block light", &g_doorsBlock, "Closed doors stop lamp light; windows, arches and glass doors let it through", true);
                ImGui::EndDisabled();
                return changed;
            });
            ImGui::EndDisabled();
            // Shadow softness sends only the rooms an edge shapes (LevelLightShare::SetShadowSoftness, once the slider is let
            // go): no full lighting refresh. Not disabled with the light between floors: it also shapes the object rigs and
            // the ground shade
            {
                ApexUi::SliderOptions o;
                o.format = "%.0f cm";
                o.displayScale = 100.0f;
                o.tooltip = "Lamp size: shadows stay sharp next to a wall and soften farther from it";
                o.defaultValue = 0.15f;
                if (ApexUi::Slider("Shadow softness", &g_shadowSoftness, 0.02f, 0.5f, o)) NotifySettingChanged();
            }
            // (Smooth light maps: Game fixes tab, Stories card)
            // A button to recalculate the lights when they go wrong: the "Refresh the lighting" shortcut as a button
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
            // The on / off sits in the header, like Water Reflections (the former "Lamps glow on ponds" row)
            Edit([] {
                bool changed = ApexUi::CardHeader(ApexUi::IconId::WavesHorizontal, "Lamp Glow", "Ponds glow and sparkle near lamps at night", nullptr, &g_water);
                if (g_water) {
                    ApexUi::CardDivider();
                    changed |= ApexUi::SliderPercent("Glow brightness", &g_waterStrengthSetting, 0.1f, 0.4f, "Intensity of lamp glow and sparkles on ponds", 0.4f);
                }
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
                    ApexUi::IconNote(IconId::Info, "Needs \"Street lamps light lots\", a lighting fix");
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
            // idavidveiga's fork (04/10): which draw families Apex modifies by day / twilight / night, and the scene's bloom mask
            if (ApexUi::TextButton("Lighting + Bloom census")) LotLightBridge::RequestLightingBloomCensus();
            ImGui::SameLine();
            ImGui::TextDisabled("(%s)", LotLightBridge::LightingBloomCensusStatus().c_str());
            ApexUi::Tooltip("Writes ApexRadiance_LightingBloomCensus.txt. Read-only: records which visible draw families Apex modifies in day, twilight and night and flags known or possible bloom-mask families");
            if (ApexUi::TextButton("Exterior surface capture")) LotLightBridge::RequestExteriorCapture();
            ImGui::SameLine();
            ImGui::TextDisabled("(%s)", LotLightBridge::ExteriorCaptureStatus().c_str());
            ApexUi::Tooltip("Read-only. Writes ApexRadiance_ExteriorSurfaces.txt: for every visible outdoor wall, fence, stair, floor, ground and roof draw, its shaders, light maps, constants and the Apex fix that acted");
            if (ApexUi::TextButton("Capture bloom alpha mask")) BloomAlphaProbe::Request(g_level);
            ImGui::SameLine();
            ImGui::TextDisabled("(%s)", BloomAlphaProbe::Status().c_str());
            ApexUi::Tooltip("Read-only. Captures the scene alpha before the game's bloom composite and writes ApexRadiance_BloomAlphaProbe_Day/Twilight/Night.png and .txt. One short hitch from the GPU readback");
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
        if (ApexUi::Checkbox("Moon height at night (A/B: off = the game's moon on the horizon)", &g_moonLift)) ObjectLightBridge::RequestRigRefresh();
        bool smoothDusk = NightLevel::g_smoothDusk.load();
        if (ApexUi::Checkbox("Smooth dusk (A/B: off = the game's night level in steps of 0.2)", &smoothDusk)) NightLevel::g_smoothDusk = smoothDusk;
        bool lowLamp = RtOutdoorLamps::LowLampLift();
        if (ApexUi::Checkbox("Lamps on the ground light the lawn from 40 cm (A/B: off = from their real 5 cm)", &lowLamp)) RtOutdoorLamps::SetLowLampLift(lowLamp);
        ImGui::SameLine();
        ImGui::TextDisabled("%ld Instant draws, %ld terrain stamps lifted%s", RtOutdoorLamps::LowLampLifted(), g_bakeLowLifted.load(std::memory_order_relaxed),
                            g_bakePosInstalled ? "" : " (terrain bake: not installed)");
        if (g_moonLift) {
            ImGui::SetNextItemWidth(220.0f);
            if (ImGui::SliderFloat("Least moon height (degrees)##MoonHeight", &g_moonHeightDeg, 5.0f, 60.0f, "%.0f") && !ApexUi::SliderDragging())
                ObjectLightBridge::RequestRigRefresh();
        }
        // Lamp lighting is chosen on the Performance page (Lamp lighting card); read-only here
        ImGui::TextWrapped("Lamp lighting: %s (Performance > Lamp lighting)", g_lampMode == kLampInstant ? "Instant (Real-time outdoor lamps on Quick outdoor lamp edits' parts)" : "Classic");
        if (g_lampMode == kLampInstant && ApexUi::SwitchRow("Check quick lamp edits", &g_outdoorDeltaCheck,
                                                            "Solves those floors in full again later and logs the largest texel difference", false))
            NotifySettingChanged();
        if (ApexUi::Checkbox("Smooth the ground light maps on the GPU (A/B: off = CPU worker)", &g_smoothMapsGpu)) NotifySettingChanged();
        ImGui::SameLine();
        if (ApexUi::TextButton("Compare GPU vs CPU (one chunk)")) LightmapSmooth::RequestCompare();
        if (ApexUi::BeginAdvanced("WaterHighlights", "Water highlights")) {
        if (ApexUi::SwitchRow("Stabilize lamp sparkles on water", &g_waterFilter, "Filters tiny highlights without temporal smoothing. Turn off to compare the original", true)) NotifySettingChanged();
        if (ApexUi::SwitchRow("Keep lamp colors on water", &g_waterColorCompression, "Softens too-bright lamps but keeps their color. Turn off to compare the original", true)) NotifySettingChanged();
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
        ImGui::TextWrapped("Walls shade lamps on lot ground: %s", LotLightBridge::LotShadeStatus().c_str());
        ImGui::TextWrapped("Objects: %s", ObjectLightBridge::Status().c_str());
        ImGui::TextWrapped("Shadow: %s", LotLightBridge::ObjectStatus().c_str());
        ImGui::TextWrapped("Walls: %s", LotLightBridge::WallStatus().c_str());
        ImGui::TextWrapped("Ground brightness: %s", LotLightBridge::GroundBrightnessStatus().c_str());
        ImGui::TextWrapped("Room light map edges: %s", RoomMapPadding::Status().c_str());
        ImGui::TextWrapped("Smooth indoor light: %s", LotLightBridge::IndoorSmoothStatus().c_str());
        ImGui::TextWrapped("Windows take outdoor light: %s", LotLightBridge::WindowOutdoorStatus().c_str());
        ImGui::TextWrapped("Terrain bake: %s | moonlight: %s", g_bakeGainInstalled ? std::format("street lamps x{:.2f}, lot lamps x{:.2f}", g_bakeStreetMul[0], g_bakeLotMul[0]).c_str() : "not installed",
                           g_sunlightBase < 0.0f ? (kSunlightScale ? "waiting for a world" : "not available")
                                                 : std::format("sunlight scale {:.3f} (base {:.3f}, moonlight x{:.2f}; light height {:.1f} deg, moon height {})", g_moonWritten, g_sunlightBase, g_moonlight,
                                                             g_moonHeightNow, g_moonLift ? std::format("at least {:.0f} deg at night", g_moonHeightDeg) : std::string("the game's")).c_str());
        ImGui::TextWrapped("Roofs: %s", LotLightBridge::RoofStatus().c_str());
        ImGui::TextWrapped("Water: %s", LotLightBridge::WaterStatus().c_str());
        ImGui::TextWrapped("Smoothed light map: %s", LightmapSmooth::Status().c_str());
        ImGui::TextDisabled("GPU vs CPU: %s", LightmapSmooth::CompareStatus().c_str());
        ImGui::TextWrapped("Lamp colour: %s", ObjectLightBridge::LampColourStatus().c_str());
        ImGui::TextWrapped("Stories: %s", LevelLightShare::Status().c_str());
        ImGui::TextWrapped("Indoor light on the ground: %s", LevelLightShare::IndoorLightOnGroundStatus().c_str());
        ImGui::TextWrapped("Quick outdoor lamp edits: %s", OutdoorDelta::Status().c_str());
        ImGui::TextWrapped("Real-time outdoor lamps: %s", RtOutdoorLamps::Status().c_str());
        ImGui::TextWrapped("Smooth light maps: %s | %s | %s | %s", LevelLightShare::SoftLightCutStatus().c_str(), RoomMapDither::Status().c_str(),
                           LotLightBridge::RoomMapSmoothStatus().c_str(), LevelLightShare::WallBandStatus().c_str());
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
    // Developer Tester tab: a player option it shows, set as its own switch would set it (pushed at the next Present)
    void SetTesterOption(bool* option, bool value) {
        if (*option == value) return;
        *option = value;
        NotifySettingChanged();
    }
    // Lamp lighting (Performance page): the paths follow at the next Present, which relights the active lot once
    void SetLampMode(int mode) {
        mode = mode == kLampInstant ? kLampInstant : kLampClassic;
        if (mode == g_lampMode) return;
        g_lampMode = mode;
        LOG_INFO(std::format("[NightTerrainRelight] Lamp lighting: {}", mode == kLampInstant ? "instant" : "classic"));
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
                                             .experimental = false,
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
        ImGui::BeginDisabled(g_menuLevel.load() < 0.0f || !g_live || g_restart.active || g_restartRequested);
        if (ApexUi::IconTextButton("Refresh terrain", ApexUi::IconId::LandPlot)) g_kickRequested = true;
        if (inlineActions) ImGui::SameLine();
        if (ApexUi::IconTextButton("Refresh lots", ApexUi::IconId::House)) g_relightLotsRequested = true;
        if (inlineActions) ImGui::SameLine();
        if (ApexUi::IconTextButton("Refresh lights", ApexUi::IconId::Lightbulb)) NightLighting::RefreshAll("button");
        ImGui::EndDisabled();
        ApexUi::CardDivider();
        ApexUi::MutedText("Discard cached lighting and rebuild it without changing settings");
        ApexUi::Gap(ApexUi::kSpace3);
        ImGui::BeginDisabled(g_menuLevel.load() < 0.0f || !g_live || !WorldSession::InWorld() || g_restart.active || g_restartRequested);
        if (ApexUi::IconTextButton("Restart lighting", ApexUi::IconId::RotateCcw)) NightLighting::Restart();
        ImGui::EndDisabled();
        if (g_restart.active) {
            const auto progress = LevelLightShare::LightingRestartProgress();
            if (progress.scheduled < progress.total)
                ApexUi::MutedText(I18n::Trf("Rebuilding lighting: {} / {} stories queued", progress.scheduled, progress.total).c_str());
            else ApexUi::MutedText("Waiting for lighting calculations");
        } else if (g_restart.done && g_restartPartial) ApexUi::MutedText("Lighting restarted; some areas use the game's maps");
        else if (g_restart.done) ApexUi::MutedText("Lighting restarted");
        else if (g_restartUnavailable) ApexUi::MutedText("Lighting restart unavailable");
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
void NightLighting::DrawGameFixes() {
    if (auto* p = MenuPatch()) p->DrawGameFixes();
}
void NightLighting::DrawLightDetailCard() {
    if (auto* p = MenuPatch()) p->DrawLightDetailCard();
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
namespace {
bool* TesterOptionPtr(const char* key) {
    const std::string_view k(key ? key : "");
    return k == "luzDeForaPelasAberturas" ? &g_outdoorOpenings : k == "luzInternaPelasAberturas" ? &g_indoorOpenings : k == "portasBloqueiamLuz" ? &g_doorsBlock
           : k == "mapasDosComodosSuaves" ? &g_smoothRoomMaps
                                            : nullptr;
}
} // namespace
bool NightLighting::TesterOption(const char* key) {
    const bool* p = TesterOptionPtr(key);
    return p && *p;
}
void NightLighting::SetTesterOption(const char* key, bool value) {
    if (bool* p = TesterOptionPtr(key))
        if (auto* self = NightTerrainRelightPatch::g_self) self->SetTesterOption(p, value);
}
int NightLighting::LampMode() { return g_lampMode; }
void NightLighting::SetLampMode(int mode) {
    if (auto* p = NightTerrainRelightPatch::g_self) p->SetLampMode(mode);
}
std::string NightLighting::InstantUnavailable() { return RtOutdoorLamps::Unsupported(); }
std::string NightLighting::InstantInactive() { return g_lampMode == kLampInstant ? RtOutdoorLamps::InactiveNote() : std::string(); }
float NightLighting::InstantGpuMs() {
    if (g_lampMode != kLampInstant || !RtOutdoorLamps::Enabled()) return -1.0f;
    return RtOutdoorLamps::GpuMs();
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
void NightLighting::Restart() {
    if (g_menuLevel.load() < 0.0f || !g_live || !WorldSession::InWorld() || g_restart.active || g_restartRequested) return;
    g_restartRequested = true;
    g_restart.done = false;
    g_restartUnavailable = false;
}

// The world is on screen (its terrain is drawn, or the fallback after the world change); false during load screens
bool NightLighting::WorldLive() { return g_live; }
bool NightLighting::LoadSettled() {
    if (g_loadSettled.load()) return true;
    if (!g_live || Clock::now() - g_liveAt < kSettleFallback) return false;
    if (!g_loadSettled.exchange(true)) {
        LOG_INFO("[NightTerrainRelight] Load settled: 10 s after the world went live");
        LoadTiming::NoteLoadSettled("10 s after the world went live");
    }
    return true;
}
