#pragma once
// Game-code addresses used by Night Lights (and its parts), Every-Story Ground Light, the Performance features
// (resource lookup cache, lot lighting while moving) and the Frame Profiler's counters, for every game build.
//
// Steam 1.67.2 (TS3W.exe 0x52DEC247): the fixed addresses the features were written for (each feature still checks the
// bytes it patches, exactly as before). Any other build (EA app 1.69.47 TS3.exe 0x6707155C, 1.69.43, retail, unknown):
// every address is found at startup by a masked byte signature scanned in the game's own .text in memory (on the EA app
// build .text is encrypted on disk and only decrypted in memory by the EA activation stub, so it cannot be matched from the
// file; the scan runs after the first Present, when it has long been decrypted). An address counts only when its
// signature matches exactly once (or every match gives the same value); a feature whose required addresses are not all
// found stays off with "Not available on <version>: <missing>" and writes nothing.
//
// Every signature is logged ("[Addr] name: N matches at ... (Steam 0x...)", with 16 bytes around each match on non-Steam
// builds) so a user's ApexRadiance_LOG.txt is enough to refine a signature. Table and method: docs/engine/game-versions.md.
#include <cstdint>
#include <initializer_list>
#include <string>

namespace GameAddr {

// Ids that come from one multi-match or callers-of entry are consecutive (sorted by address).
enum class Id : uint16_t {
    // ---- Night Lights core ----
    RootGetter,         // FUN_006e97b0: mov eax,[root]; ...; mov eax,[eax+1C0h]; ret
    RootPtr,            // the global it reads (*(root)+0x1C0 = light manager)
    QueueRoom,          // FUN_006c7160 thiscall(treeLevel, roomId)
    TerrainVisitorSite, // light test in the terrain bake visitor FUN_00c29620
    ArmSiteRemoval,     // light test before "mov [esi+38h],32h" in FUN_006b6090 (3 sites, by address)
    ArmSiteRegister,    //  ... in FUN_006b64b0
    ArmSiteMoved,       //  ... in FUN_006b6590
    EnumLights,         // FUN_006acf70 stdcall(visitor)
    ChunkRenderCall,    // call FUN_00c7e7a0 in the terrain update's per-chunk loop
    ChunkRenderFn,      // FUN_00c7e7a0
    LampColourSite,     // movaps xmm0,[esi+0E0h] in the street lamp evaluation (developer option)
    LotPassSite,        // mov eax,[edi+0D8h] in FUN_00c7f750 (developer option)
    LotPassConst,       // the float constant loaded right after it
    LotPassTexGlobal,   // the texture global its world pass binds
    LotPassNullBind,    // mov eax,[tex]; push 0; push 0 (lot pass binds nothing)
    QualitySite0,       // mov byte [esp+0Ch],0 in FUN_00adb5a0 (2 sites, by address; developer option)
    QualitySite1,       //  ... in FUN_00adb850
    // ---- light classes (light factory FUN_006ac590: type 3..11 -> constructor -> vtable) ----
    LightJumpTable,
    LightVtable3, LightVtable4, LightVtable5, LightVtable6, LightVtable7, LightVtable8, LightVtable9, LightVtable10, LightVtable11,
    LightColour3, LightColour4, LightColour5, LightColour6, LightColour7, LightColour8, LightColour9, LightColour10, LightColour11, // vfunc+0x10
    LightEval3, LightEval4, LightEval5, LightEval6, LightEval7, LightEval8, LightEval9, LightEval10, LightEval11,                   // vfunc+0x4C
    LightPos,           // vfunc+0x24 (the same function in all nine classes)
    // ---- object light bridge ----
    CapOperandSite,     // mov ecx,offset cap in FUN_006b92a0
    CapGlobal,
    RigGatherReturn,    // return address of the light colour call in FUN_006bb270
    LumaWeights,
    DirtyAllRigs,       // FUN_006b58f0 __fastcall(cells)
    RigCtor,            // FUN_006bb8f0
    RigVtable,
    RigCtorCall0, RigCtorCall1, RigCtorCall2, // its three calls in FUN_006f7880
    RoomGatherCall,     // call FUN_006bb2f0 in FUN_006bbde0
    RoomGather,
    CellGather,         // FUN_006b5af0
    RigUpdate,          // FUN_006bbf90 __fastcall(rig)
    SetLightColour,     // FUN_006bda90
    SetColourCall0, SetColourCall1, SetColourCall2, SetColourCall3, SetColourCall4, SetColourCall5, SetColourCall6, // its 7 callers
    ScriptSetColourCall,
    ScriptSetColour,    // FUN_006bc3e0
    // ---- light between stories ----
    AddWorldLights,     // FUN_006c6ab0
    AddWorldLightsCall0, AddWorldLightsCall1,
    LevelGather,        // FUN_006c6990
    LevelGatherCall0, LevelGatherCall1,
    CascadeTest,        // cmp [esi+1A0h],eax; jnz (JNZ at +7) in FUN_006c7250
    RoomByIdCall, RoomById,
    InvalidateCall, InvalidateRoom,
    SetInsertCall, SetInsert,
    SolvePoint,         // LightPointWithAllLights FUN_0069fd60
    SolvePointCall0, SolvePointCall1, SolvePointCall2,
    BatchSolveCall,     // the one in FUN_006a31d0
    LightEvalReturn,    // after "call edx" (light vfunc+0x4C) in SolvePoint
    WallTestCall, WallTest,
    BatchSamples,       // global vector of the wall sample batch
    WallCullBatchFn,    // FUN_006a30b0
    WallCullCall, WallCull,
    // ---- indoor lamps through stair openings (optional part of the light between stories) ----
    LightFilter,        // FUN_006c7820: the registry entry filter of a story's light gather
    LightBright,        // FUN_006bc520 fastcall(light): the light is bright enough to count
    AddRoomLight,       // FUN_006a2060 thiscall(room, light) ret 4: adds a light to a room's list
    RoomUpdatePush,     // push FUN_006c7250 in FUN_006c5e20: the per-story room update, run for every story of every lot
    RoomUpdate,         // FUN_006c7250 fastcall(treeLevel)
    LightEntryUpdate,   // FUN_006c7ba0 thiscall(entry): recomputes window room/sky activation
    ChangedClearCall,   // its call that empties the "changed rooms" set (ecx = treeLevel+8) after walking it
    ChangedClear,       // FUN_007f3790 thiscall(set, buckets, count) ret 8
    FloorSet,           // FUN_00a89dd0 thiscall(level floor object, ...): sets a floor quadrant
    FloorSetCall0, FloorSetCall1, FloorSetCall2, FloorSetCall3,
    FloorRemove,        // FUN_00a893a0 thiscall(level floor object, ...): removes one
    FloorRemoveCall,
    LevelVtable,        // vtable of the level floor object (ctor FUN_00a88790)
    LevelCtorCall,      // the only CALL of that ctor (after "new 0x350"): every level floor object is made there
    LevelCtor,          // FUN_00a88790 thiscall(object), returns it, plain ret
    LodChoice,          // FUN_0069e710 fastcall(room): the lighting LOD class a room should have (high only on the camera's story)
    LodChoiceCall0, LodChoiceCall1, LodChoiceCall2, LodChoiceCall3,
    LodMax,             // the int it returns for the camera's story (0x01158B00)
    RoomSolveStartCall, // the CALL of state 0 of the room solve in FUN_006a3c90
    RoomSolveStart,     // FUN_006a18b0 thiscall(room): ambient colour, normalisation and ambient ramp of an indoor room
    WallPassCall,       // the CALL of the wall texel pass (state 2) in FUN_006a3c90
    WallPass,           // FUN_006a3a30 thiscall(room, int, float) ret 8
    WallSamplesCall,    // its CALL of FUN_006ac070 thiscall(wall, piece, class, batch) ret 0xC: the samples of one piece of a wall
    WallSamples,
    WallBlurCall,       // its CALL of FUN_0069f650 fastcall(room): the wall atlas blur (LOD class 2)
    WallBlur,
    WallBlurPasses,     // the dword of blur passes (0x01158B1C)
    WallBlurMode,       // the byte of blur mode (0x011D02E4; 0 = [1 2 1] per axis)
    WallSolveCall,      // its CALL of FUN_006a31d0 thiscall(room, batch, {base, pitch}, flags, sampler, char) ret 0x14 for one piece
    WallSolve,
    // ---- unlit rooms (features/unlit_rooms.cpp) ----
    RoomAmbient,        // FUN_006a0f50 fastcall(room): the room's ambient (state 0 of the room solve)
    UnlitColourA,       // in it: imm32 of "mov ecx, 0x011D0B60" (the colour of a room with no lamp, on some lots)
    UnlitColourB,       // in it: imm32 of "mov ecx, 0x011D0B40" (the same, other lots)
    DimAmbient,         // FUN_006a00a0: a dim room's ambient topped up with that colour
    DimColourA,         // in it: the same two imm32
    DimColourB,
    DimAmbientCall0, DimAmbientCall1, // its two calls in FUN_006a0f50 (0x006A13F0: the result goes to room+0x110; 0x006A1410: +0x120)
    FillGate,           // imm32 of "cmp byte [0x01158D5C], 0" in FUN_006ba340: the fill light added to object rigs
    FillColour,         // imm32 of "movaps xmm2, [0x011D0E10]" in FUN_006b7e70: the fill light's colour (0.8, 0.8, 1, 0.8)
    // ---- room lighting queue (features/room_light_queue.cpp) ----
    RoomPriorityCall,   // the only CALL of the room priority (in FUN_006a8190)
    RoomPriority,       // FUN_0069e770 fastcall(room) -> float in ST0
    LodStepSite,        // "mov edi,1; lea ebx,[edi+1]" in FUN_0069ea70: class 0 -> 1 after a solve
    KeepClassA,         // "jl" in FUN_0069eed0: an invalidated room restarts at class 0 when shown < LodChoice
    KeepClassB,         // the same "jl" in FUN_0069f160
    InvalidateFlag,     // FUN_0069f160 thiscall(room, char flag) ret 4: invalidates a room when its +0x19 flag changes (from 0x006A5E00)
    RoomPickJump,       // "jmp FUN_006c5c20" at the end of FUN_006c5e20 (the per-frame light tree update)
    RoomPick,           // FUN_006c5c20 fastcall(tree): makes the best pending room current
    RoomSolveStep,      // FUN_006a3f80 thiscall(room, stopwatch*, float budget) ret 8: the budgeted solve of a room in state 3
    StopwatchCtor,      // FUN_004f35b0 thiscall(sw, kind, char start) ret 8 (kind 4 = ms)
    StopwatchStart,     // FUN_00408700 thiscall(sw)
    StopwatchElapsed,   // FUN_004f33c0 thiscall(sw) -> ST0
    PriorityLotObject,  // FUN_006fde10: mov eax,[SceneObjectManager]; ret
    PriorityLotTest,    // FUN_006fdc80 thiscall(som, lotLo, lotHi) ret 8 -> al: one of the two priority lots
    // ---- rig tracker ----
    ModelDraw,          // FUN_006f6250
    BinderCall, Binder,
    InstanceFlush,      // FUN_006cf920
    // ---- Every-Story Ground Light ----
    GetLotIdGatherCall, // call GetLotID in the outdoor-room light gather
    GetLotId,           // BaseLight::GetLotID FUN_006bc020
    // ---- Frame Profiler counters (the profiler is development build only; the addresses resolve in both builds). The
    //      first three are also the resource lookup cache's (features/resource_cache.h, both builds). ----
    ResFindProvider,      // ResourceMgr::FindProvider FUN_004affc0 thiscall(key*, cookie*), ret 8 (only called through vtables)
    ResFindProviderSlot0, // the vtable slots holding it (+0x40 of the base 0x00FB2DA0 and derived 0x00FFE250 vtables), by address
    ResFindProviderSlot1,
    RefPackCompress,      // RefPack stream write FUN_004ec200 thiscall(src, size, dst, capacity, flags), ret 0x14
    RefPackCompressSlot,  // its slot in the stream vtable 0x00FB9018 (+4)
    SceneDrainCall,       // call FUN_006e4130 in Scene::BeginFrame FUN_006ebb70
    SceneDrain,           // FUN_006e4130 thiscall(): drains the scene's pending-node list
    DxtEncode1,           // FUN_006152f0 cdecl(dst*, src*): CPU DXT1 encoder
    DxtEncode5,           // FUN_006154b0 cdecl(dst*, src*): CPU DXT5 encoder
    ObjectById,           // FUN_00c62d40 thiscall(idLo, idHi, flag), ret 0xC: object lookup by ID (linear tree walk)
    RoomSolveCall,        // call FUN_006a8ba0 in the lot lighting update FUN_00adb8f0 (its only caller)
    RoomSolve,            // FUN_006a8ba0 thiscall(timer*, float budget), ret 8: one room of the budgeted lot relight
    RemoteCallJob,        // FUN_007d9840 cdecl(handle, RemoteCall*, phase): the job function of remote calls
    RemoteMethodVtable,   // PostRemoteMethodCall object vtable, method(obj, byte, byte) (stored at 0x00ABEA0A; method at object +0x10)
    RemoteMethodVtable2,  // the same with method(obj, byte) (stored at 0x00ABEA93; method at object +0x10)
    // ---- Resource lookup cache (features/resource_cache.h; group "ResourceCache", with ResFindProvider and its slots) ----
    ResRegisterDb,            // ResourceMgr::RegisterDatabase FUN_004b2d00 thiscall(bool add, db*, int priority), ret 0xC (base class)
    ResRegisterDbSlot,        // its only reference: slot +0x34 of the base vtable 0x00FB2DA0
    ResRegisterDbDerived,     // ResourceSystem's override FUN_00736a70 (same arguments; calls FUN_004b2d00 directly)
    ResRegisterDbDerivedSlot, // its only reference: slot +0x34 of the derived vtable 0x00FFE250
    ResSetDbPriority,         // ResourceMgr::SetDatabasePriority FUN_004b2ec0 thiscall(db*, int priority), ret 8 (re-sorts the list)
    ResSetDbPrioritySlot0,    // slots +0x3C of both vtables, by address
    ResSetDbPrioritySlot1,
    ResDbChanged,             // ResourceMgr::DatabaseChanged FUN_004b0960 thiscall(db*, keyVector*), ret 8 (the engine's "keys of db changed")
    ResDbChangedSlot0,        // slots +0x4C of both vtables, by address
    ResDbChangedSlot1,
    ShadowedDbVtable,         // vtable 0x00FFE078 of the read-only package class that closes idle files (ctor FUN_007342f0)
    // ---- Lot lighting while moving (features/lot_lighting_motion.cpp; group "LotLightingMotion") ----
    LotLightBudgetCall,       // call FUN_00adb120 in the lot lighting update FUN_00adb8f0 (its only caller)
    LotLightBudget,           // FUN_00adb120 thiscall(lot lighting manager), ret, result in ST0: this lot's budget in ms
    CameraRootCall,           // call FUN_006e8330 (app root getter) in WorldManager::Update, right before the camera getter call
    CameraGetterCall,         // call FUN_006e8400 (root -> camera); the next instruction reads the camera eye (movaps xmm0,[eax+60h])
    CameraRootGetter,         // FUN_006e8330: mov eax,[root]; ret
    CameraGetter,             // FUN_006e8400: mov eax,[ecx+24h]; ret
    // ---- Faster texture / cache compression (features/fast_dxt.cpp, fast_refpack.cpp; groups "FastTextureCompression" =
    //      DxtEncode1 + DxtEncode5, "FastCacheCompression" = RefPackCompress + its slot) ----
    RefPackDecompress,        // FUN_004eb3b0 cdecl(dst, capacity, src, srcSize): the only RefPack decoder, found through its CALL in the
                              // stream read FUN_004ec010 (the official S3SS detours its entry; Apex only calls it, to check its own streams)
    // ---- Wall shading while moving (features/lot_lighting_motion.cpp; group "WallShadingWhileMoving" = these + the camera ids).
    //      The Frame Profiler's "Wall AO pass" counter uses the first two. ----
    WallAoStep,               // FUN_0068b810 thiscall(stopwatch*, float budget), ret 8: the wall ambient-occlusion solver step (returns the next state)
    WallAoStepSlot,           // its only reference: slot +0x1C of the solver vtable 0x00FF0594 (0x00FF05B0)
    WallAoDriver,             // FUN_00688920 thiscall(stopwatch*, float budget), ret 8: the solver driver (slot +0xC), stores the step's result as the state
    // ---- File list cache (features/resource_cache.h; group "FileListCache" = these + the package-list watchers). The Frame
    //      Profiler's "Key list" counter uses the first four. ----
    ResKeyList,               // ResourceMgr::GetKeyList FUN_004b1ae0 thiscall(vector* out, filter*, bool unique), ret 0xC: returns the count
    ResKeyListSlot,           // its only reference: slot +0x20 of the base vtable 0x00FB2DA0 (0x00FB2DC0)
    ResKeyListDerived,        // ResourceSystem's override FUN_00736660 (calls FUN_004b1ae0 directly, then sorts and uniques the output)
    ResKeyListDerivedSlot,    // its only reference: slot +0x20 of the derived vtable 0x00FFE250 (0x00FFE270)
    KeyTypeFilterVtable,      // vtable 0x00FD8248 of the "key.type == type" filter {vtable, type} (predicate FUN_005949f0 at +4), from CAS FUN_005da0c0
    // ---- Write epochs of "Remember missing files" (features/resource_cache.cpp; each class is optional: a class whose
    //      vtable or studied methods are not found keeps being probed) ----
    DpfVtable,                // 0x00FB2600: writable package database "DPF" (ctor FUN_004a8f70)
    DpfDerivedVtable,         // 0x01048DA0: its derived class (ctor near FUN_00996690; overrides only +0x7C / +0x84)
    DdfVtable,                // 0x00FB2420: loose-file folder database "DDF" (ctor near FUN_004a5510)
    PackedStreamVtable,       // 0x00FFD790: base packed stream database (ctor FUN_0072cc60; e.g. the CAS compositor cache at priority -1000)
    DpfWriteDirect,           // FUN_004a7fc0 thiscall(key, data, size, ...), ret 0x14: the DPF's non-virtual record write (removes and re-inserts the key)
    // ---- Scene node budget (features/scene_budget.cpp; group "SceneNodeBudget" = SceneDrainCall, SceneDrain, these nine
    //      and the camera ids) ----
    SceneBoundsCall,          // call FUN_006fb4b0 inside the drain FUN_006e4130 (node world bounds into an aligned 32-byte buffer)
    SceneNodeBounds,          // FUN_006fb4b0 thiscall(node, float out[8]), ret 4, returns out (the node's world AABB)
    SceneSpatialCall,         // call FUN_006fad70 inside the drain (right after the bounds)
    SceneNodeSpatial,         // FUN_006fad70 thiscall(node, bounds*), ret 4: moves the node in its owner's spatial tree ([node+0x30]+0x2C)
    SceneNodeDtor,            // FUN_006fd930 thiscall(): the scene node base destructor (every derived destructor ends in it; does not unlink +0x18)
    SceneAddNode,             // FUN_006e6480 thiscall(node, group), ret 8: the pending holder's AddNode (pushes the node's link without unlinking it)
    SceneHolderTeardown,      // FUN_006e4de0 thiscall(): the pending holder's teardown (SetOwner(0) + Release of every node, list left as it is)
    SwarmListGather,          // FUN_00722190 thiscall(): the Swarm effect renderer drawable's node update (vfunc +0x48): rebuilds its bucket draw lists
    SceneModelFurDraw,        // FUN_006fa0b0 SceneModelFur draw (vfunc +0x18): reads the model handle's current LOD (+0x210, slot +8) without a NULL check
    // ---- Object lookup index (features/object_index.cpp; group "ObjectIndex" = ObjectById + these two) ----
    ObjectTreeWalk,           // FUN_00c60d30 thiscall(idLo, idHi, int* visited), ret 0xC: walks the root vector [this+0x9C, this+0xA0)
    ObjectTreeSearch,         // FUN_00c5fa60 cdecl(node, idLo, idHi, int* visited): recursive depth-first search (id at +0x48/+0x4C)
    // ---- Lot LoD streaming probe (features/lot_lod_streaming.cpp; group "LotLodStreaming") ----
    LotLodScoring,            // FUN_00c6c290: scores nearby lots and requests Detailed View transitions
    LotDetailRequest,          // FUN_00ac20e0: applies Detailed View on/off to one lot
    LotLodThrottleTest,       // Steam 0x00c6c695: cmp byte [Throttle Lot LoD Transitions],0 inside LotLodScoring
    LotLodThrottleFlag,       // global byte tested above; Steam 0x011ecbc0
    // ---- Lot visibility override (features/lot_visibility_override.cpp; group "LotVisibilityOverride") ----
    LotVisibilityCameraBiasJZ, // JZ in the lot visibility/distance camera-bias metric; 0x74 -> 0xEB
    // ---- Per-lot object streaming throttle (features/lot_object_throttle.cpp; group "LotObjectThrottle") ----
    LotAddObjectsToScene,     // Lot::AddLotObjectsToScene thiscall(lot, initialLoad, alwaysVisibleOnly), ret 8
    LotUpdateObjectSceneNode, // Lot::UpdateObjectSceneNode thiscall(lot, object, initialLoad, alwaysVisibleOnly)
    ScriptMessageScopeCtor,   // ScriptMessageScope ctor thiscall(scope, beginMsg, endMsg, lot)
    ScriptMessageScopeDtor,   // ScriptMessageScope dtor fastcall(scope)
    PostRemoteMethodCall,     // cdecl(thread, lot, func, a4, initialLoad, alwaysVisibleOnly)
    IsObjectLargeOrFlora,     // cdecl(object) -> int; shells / large exterior geometry / outdoor flora
    // ---- Local terrain relight (features/terrain_chunk_relight.cpp; optional part of Night Lights: without them lamp
    //      changes keep the full terrain rebuild) ----
    WorldManagerPtr,          // the WorldManager global 0x011ECBC4 (FUN_00c6cf80 stores the manager there at 0x00C6D0CC)
    TerrainUpdateCall,        // "mov ecx,[esi+58h]; call FUN_00c845c0" in WorldManager::Update (0x00C6D68C): WorldManager+disp8 = terrain
    // ---- Night Lights brightness controls (optional: without them "Street lamps" / "Lot lamps" on the ground and
    //      "Moonlight" are not available) ----
    BakeColourSite,           // movaps xmm0,[edi+0F0h] in the terrain bake FUN_00c292b0: the lamp colour copied to its shader parameter
    SunlightScale,            // the "Sunlight Scale" float FUN_00c11ad0 multiplies the sun / moon colour by (mov ecx,imm32 at 0x00C11B01)
    CasTriSort,               // FUN_005d1960 cdecl(u16* indices, u8* vertices, u32 indexCount, u32 vertexCount, u16 stride, u8 offset):
                              // "CAS/ModelBuilder/TriangleSortDataList" (features/cas_tri_sort.h)
    // ---- Faster memory handling (features/fast_memory.h) ----
    AllocGlobal,              // the global general allocator pointer 0x011CB864 (operator new FUN_004e3f90: mov ecx,[global])
    AllocMmapFreeCall,        // "call [VirtualFree]" in the allocator's FreeInternal FUN_004e51b0 (0x004E5306): releases a big block
    // ---- Faster cache compression, record checksums (features/fast_crc.h) ----
    RecordCrc,                // FUN_004fa4c0 cdecl(bytes, length, crc, bool invert): MSB-first table CRC-32 of the cache records
    RecordCrcTable,           // its 256-entry table (0x0114D330)
    // ---- Frame Profiler: the DDS texture loader's create and fill calls (texture load finalize job 0x007297C0) ----
    TexCreateCall,            // call FUN_0060cea0 (0x0060E1DC): creates the D3D texture (cdecl, 9 args)
    TexCreate,
    TexFillCall,              // call FUN_0060d290 (0x0060E1FF): copies every mip level into it (cdecl, 4 args)
    TexFill,
    UiServiceGetter,          // UIManager_GetMainWindowImpl calls this read-only root-service getter
    RoomNormCall,             // CALL FUN_006a0230 thiscall(room, brightest): the room light normalisation (0x006A13B4)
    RoomNorm,
    BasisLightCall,           // CALL FUN_0069f280 in the directional basis maps (0x006A0C56)
    BasisLight,
    LampLitCall,              // "mov ecx, edi; call FUN_006bdca0" in FUN_006c7ba0 (0x006C7CB6)
    LampMarkCall,             // CALL FUN_006c7160 thiscall(treeLevel, room) in FUN_006c7ba0 (0x006C7CD6)
    LampMark,
    // ---- Room to save (features/memory_guard.h) ----
    ResUpdateCall,            // CALL FUN_00737560 (0x007377F7): the ResourceSystem's per-frame update (thiscall, 1 arg), trims both caches
    ResUpdate,
    ResShrinkBoth,            // FUN_00733E70 (ResourceSystem vtable 0x00FFE2F0 +0x50): force-trims both caches (+0x1E0, +0x1E4) to idle-free
    WorldSaveCall,            // CALL FUN_00c6d460 (0x00AAC320) in FUN_00aac110: the world save; false = Error 12
    WorldSave,
    WindowRepaintJump, // jne short over the window pump's per-frame InvalidateRect (0x004108AE, features/window_repaint.h)
    MonoTypeGetObject, // FUN_00ea8a00 cdecl(MonoDomain*, MonoType*): mono_type_get_object (domain lock at +0, type_hash at +0x40)
    MonoDomainFree, // FUN_00e75340 cdecl(MonoDomain*, int force): mono_domain_free
    ObjMapFind, // FUN_00939100 thiscall(map; out[2], key*), ret 8: the object service's hash map find (1033 buckets)
    ObjMapInsert, // FUN_00939170 thiscall(map; out[3], node, flag), ret 0xC: its insert (leaf)
    ObjMapErase, // FUN_00938d00 thiscall(map; out[2], node, bucket*), ret 0xC: its erase (leaf)
    // (2.15.3) The same 1033-bucket map as the scene's (the message router at 0x006FDE20's singleton keeps one at +4, keyed
    // by the 64-bit object id; features/render_object_map.h): node links at node+0 (the object service's at +0x10)
    RenMapFind, // FUN_006fdfb0 thiscall(map; out[2], key*), ret 8
    RenMapInsert, // FUN_006fe1f0 thiscall(map; out[3], node, flag), ret 0xC (leaf)
    RenMapErase, // FUN_006fdf40 thiscall(map; out[2], node, bucket*), ret 0xC (leaf)
    // (2.15.3) The SpeedTree cluster's light rig key setters (features/tree_light_rig.h)
    TreeSetLevel,     // FUN_0070d370 thiscall(cluster, level), ret 4
    TreeSetCustomRig, // FUN_0070d800 thiscall(cluster, key, arg), ret 8
    // ---- Compositor tile readback without the GPU wait (features/compositor_readback.cpp; group "CompositorReadback")
    CompQueueCall,    // call FUN_005fdef0 in the texture compositor queue loop FUN_00608270 (0x006082F8)
    CompDispatch,     // FUN_005fdef0 thiscall(builder; job), ret 4: the texture builder's state machine ([b+0x74] state, [b+0x70] tile)
    CompState2Call,   // its state-2 call of FUN_005fdde0 (0x005FDFEC)
    CompTileRender,   // FUN_005fdde0 thiscall(builder; job), ret 4: renders the tile, then FUN_005fdbf0
    CompState3Call,   // its state-3 call of FUN_005fd420 (0x005FDFFF)
    CompTileRead,     // FUN_005fd420 thiscall(builder; job), ret 4: reads the locked tile
    CompReadbackCall, // call FUN_005fbce0 in FUN_005fdbf0 (0x005FDC8A)
    CompReadback,     // FUN_005fbce0 thiscall(builder; format), ret 4: GetRenderTargetData into the staging texture [b+0x20]
    CompTileFetch,    // FUN_005fdbf0 thiscall(builder; job), ret 4: readback, then LockRect of [b+0x20] and [b+0x1C] into the job
    LightObjectRemove, // FUN_006c7610 thiscall(levelLights; idLo, idHi), ret 8: an object's entries out of the five per-level light maps
    LotBuildBudgetLoad, // 0x00AEA6D8 in FUN_00aea680 (a lot's per-frame build slice): "mov eax,[0x011ECBC4]" right after its budget [esp+14h] (20 / 35 ms) is set
    ProbeDiffuseWrite,  // 0x006B4720 thiscall(lightProbe), ret: writes the new diffuse cube (+0x2C60, or +0x2C50 when not double-buffered)
    ProbeSpecularWrite, // 0x006B4B10 thiscall(lightProbe, rt), ret 4, al: writes the new specular cube and its mips (+0x2C64 / +0x2C5C)
    // ---- Formerly Steam-only addresses (2.13.2 EA port; docs/engine/game-versions.md "Formerly Steam-only addresses") ----
    // light law constants read by the light evaluations (outdoor_delta, rt_outdoor_lamps, light_diag)
    LampK1,             // float 0x011D0A60 ("mov ecx, imm32" before the pow call in the light evaluations)
    LampK2,             // float 0x011D0A68
    LampCmax,           // float[4] 0x011D1160 (movaps xmm0,[imm32] in the light evaluations)
    StreetLampScale,    // float 0x01158DA8: the street lamp colour scale (in LightEval5, after LampColourSite)
    ConeScale5,         // float[4] 0x011D11A0: the type-5 cone sharpness FUN_006bc940 reads (object_light_bridge, lot_light_bridge)
    RigCutFlagged,      // float 0x01158D64: the rig light cut for a rig with +8 set (FUN_006bb270, after RigGatherReturn)
    RigCut,             // float 0x01158D60: the cut otherwise
    SolveTimeByClass,   // float[3] 0x011D1200: the game's cumulative room solve time per LOD class (ms)
    MapSlots,           // 0x011D0460: the per-class room map slots (FUN_006a4640: shl eax,4; add eax,imm32)
    MapLocker,          // 0x011D0508: the room that holds the class's working maps locked (FUN_006a4660: mov [imm32],eax)
    // room solve finalize and its steps (level_light_share: FinalizeHook, the outdoor delta's own finalize)
    Finalize,           // FUN_006a0e00 thiscall(room): step 8 of the budgeted solve
    FinalizeCall,       // its call in FUN_006a3c90 (0x006A3E65)
    MapEdges,           // FUN_006a8ca0 thiscall(classEntry) -> al (its first call)
    EdgeCopy,           // FUN_0069efc0 thiscall(room, bits, classEntry) (called twice)
    MapUnlock,          // FUN_0069e950 thiscall(room, char commit)
    RoomLitNote,        // FUN_006a08e0 thiscall(room)
    // LOD class tables (level_light_share QuickLikeRefinement / threshold; light_detail)
    ThresholdFn,        // FUN_006a8e50: fld [idx*4+factor]; fmul [scale]; ret
    ThresholdFactor,    // float[3] 0x00FF36F4
    ThresholdScale,     // float 0x00FF3700
    ClassWallModeFn,    // FUN_006a8cb0: mov al,[idx+table]; ret
    ClassWallMode,      // byte[3] 0x00FF36A4
    ClassWallsFn,       // FUN_006a8ce0 thiscall(pass) ret 4 (2 functions, by address)
    ClassObjectsFn,     // FUN_006a8d00
    ClassWalls2D,       // byte[3][4] 0x00FF36C4
    ClassObjects3D,     // byte[3][4] 0x00FF36D0
    TexelsGetter,       // FUN_006a8d20: mov eax,[idx*4+table]; ret
    TexelsPerTile,      // int[3] 0x00FF36AC
    AtlasReads,         // 0x006A90FC in FUN_006a90e0: mov edi,[eax+eax+W]; mov ebx,[eax+eax+H]
    WallAtlasW,         // int[3] 0x00FF36DC
    WallAtlasH,         // int[3] 0x00FF36E8
    OriginPush,         // 0x006A7E5E in FUN_006a7cf0: mov ecx,[esp+14h]; push 4; ... (the push imm8 at +5)
    CopyPush,           // 0x006AA322 in FUN_006aa2e0: mov [ecx+74h],eax; push 4; ... (the push imm8 at +4)
    CopyBuild,          // FUN_006a9ad0 stdcall(int n): the border copy table (its call at CopyPush + 0x10)
    CopyBuilt,          // byte 0x011D0510: the table was built
    // walls taller than a story, openings, outdoor alpha, wall bases (level_light_share)
    WallUvCall0,        // call FUN_00c38530 (0x00C387B5)
    WallUvCall1,        // the second call (0x00C387CC)
    WallUv,             // FUN_00c38530: the wall light-UV writer
    WallEdgeFn,         // FUN_006a43b0 cdecl(dir) -> edge
    CutPostSite0,       // "push 6257EB4h" of the wall cutout message in the cutout Add (0x00B0D866)
    CutPostSite1,       //  ... and Remove (0x00B0DD15)
    OutdoorAlphaSite,   // 0x006A333B in FUN_006a31d0: cmp byte [ebx+18h],0; xorps xmm1,xmm1; jne
    WallBaseSite,       // 0x006AB32C in FUN_006ab280: movss xmm0,[ebp+10h] of the outdoor branch
    IndoorBaseSite,     // 0x006AB340: movss xmm0,[eax+78h] of the indoor branch
    // room map dither (features/room_map_dither.cpp)
    TexelPackCall,      // 0x006A34DB: call FUN_0060cd10 in FUN_006a31d0
    TexelPack,          // FUN_0060cd10
    TexelAddrSite,      // 0x006A330C: the texel address code in FUN_006a31d0
    PackConst255,       // float 0x00FA312C (255.0) the pack reads at +0x16
    PackMagic,          // float 0x00FE774C (12582912.0) at +0x5A
    // lamp mark filter (features/lamp_mark_filter.cpp)
    OccMarkCall,        // 0x006C7939: the occluder's call of FUN_006c7160 in FUN_006c78e0
    EntryFlag,          // FUN_006c4cf0(objId): flags every light and occluder entry of an object
    EntryFlagCall0, EntryFlagCall1, EntryFlagCall2, EntryFlagCall3, EntryFlagCall4, // its 5 callers, by address (development build counters)
    Count
};

// Resolves the table: fixed addresses on Steam 1.67.2 (the signatures are still scanned and logged as a self-check),
// signature scan elsewhere. Init thread, after the game settled (first Present) and before the features install; on a
// build whose .text still looks encrypted it waits up to 15 s. Safe to call once; later calls do nothing.
void Resolve();
bool Resolved();
// True once Resolve has run, on every build (on Steam 1.67.2 the fixed addresses are usable before: see Resolved). The
// Frame Profiler waits for it before hooking its counters, so the signature self-check never sees its own hooks.
bool Scanned();

// True on Steam 1.67.2: every address is the fixed one and the features keep their exact byte checks.
bool IsFixed();

// The address, or 0 when it was not found (or Resolve has not run).
uintptr_t Get(Id id);
const char* Name(Id id);

// True when every id is known; otherwise `missing` (if given) gets their names, comma separated.
bool Have(std::initializer_list<Id> ids, std::string* missing = nullptr);
// "Not available on <version>: missing <names>"
std::string NotAvailable(const std::string& missing);

// Feature groups (FeatureInfo::gameCodeGroup): "NightLights" (core of Night Lights), "SplitLevel", "ResourceCache",
// "LotLightingMotion", "FastTextureCompression", "FastCacheCompression", "WallShadingWhileMoving", "FileListCache",
// "SceneNodeBudget", "ObjectIndex", "MemoryGuard", "LotLodStreaming", "LotObjectThrottle", "LotVisibilityOverride"
bool GroupAvailable(const char* group, std::string* missing = nullptr);

// Light type (3..11) of a light vtable, or -1
int LightTypeOfVtable(uintptr_t vtable);

// Non-Steam builds: once per session, when the first world is loaded (render thread), reads a few known objects and logs
// whether the struct offsets assumed from Steam look right (light manager, night level, light cells, light types).
void CheckWorldStructs();

} // namespace GameAddr
