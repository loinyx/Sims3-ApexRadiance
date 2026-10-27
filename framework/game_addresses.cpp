#include "game_addresses.h"
#include "apex_log.h"
#include "game_version.h"
#include "build_flavor.h"
#include <windows.h>
#include <atomic>
#include <cstring>
#include <format>
#include <iterator>
#include <string>
#include <vector>

// See game_addresses.h and docs/engine/game-versions.md. The signature table below is also read by the offline checker
// (research\port169\sigcheck.pl): keep one entry per line in the same shape.

namespace GameAddr {
namespace {

// ---------------------------------------------------------------------------------------------------------------------
// Names and the fixed Steam 1.67.2 addresses (same order as Id)
// ---------------------------------------------------------------------------------------------------------------------
struct Info {
    const char* name;
    uint32_t steam;
};
constexpr Info kInfo[] = {
    {"RootGetter", 0x006E97B0},
    {"RootPtr", 0x011D1860},
    {"QueueRoom", 0x006C7160},
    {"TerrainVisitorSite", 0x00C29626},
    {"ArmSiteRemoval", 0x006B60D3},
    {"ArmSiteRegister", 0x006B6516},
    {"ArmSiteMoved", 0x006B6618},
    {"EnumLights", 0x006ACF70},
    {"ChunkRenderCall", 0x00C8504C},
    {"ChunkRenderFn", 0x00C7E7A0},
    {"LampColourSite", 0x006BE18C},
    {"LotPassSite", 0x00C7F87D},
    {"LotPassConst", 0x0107A538},
    {"LotPassTexGlobal", 0x011ECE80},
    {"LotPassNullBind", 0x00C7F8B7},
    {"QualitySite0", 0x00ADB66B},
    {"QualitySite1", 0x00ADB884},
    {"LightJumpTable", 0x006AC7A0},
    {"LightVtable3", 0x00FF42A0},
    {"LightVtable4", 0x00FF4570},
    {"LightVtable5", 0x00FF4350},
    {"LightVtable6", 0x00FF4468},
    {"LightVtable7", 0x00FF43A8},
    {"LightVtable8", 0x00FF4408},
    {"LightVtable9", 0x00FF44C0},
    {"LightVtable10", 0x00FF4518},
    {"LightVtable11", 0x00FF42F8},
    {"LightColour3", 0x006C02A0},
    {"LightColour4", 0x006C1BC0},
    {"LightColour5", 0x006C0690},
    {"LightColour6", 0x006C1320},
    {"LightColour7", 0x006C0AF0},
    {"LightColour8", 0x006C0FE0},
    {"LightColour9", 0x006C16D0},
    {"LightColour10", 0x006C1980},
    {"LightColour11", 0x006C02A0},
    {"LightEval3", 0x006BDE90},
    {"LightEval4", 0x006BFFB0},
    {"LightEval5", 0x006BE1C0},
    {"LightEval6", 0x006BFA70},
    {"LightEval7", 0x006BEFD0},
    {"LightEval8", 0x006BF880},
    {"LightEval9", 0x006BFBA0},
    {"LightEval10", 0x006BFDC0},
    {"LightEval11", 0x006BE020},
    {"LightPos", 0x009691E0},
    {"CapOperandSite", 0x006B9418},
    {"CapGlobal", 0x011D0BA8},
    {"RigGatherReturn", 0x006BB2B3},
    {"LumaWeights", 0x011D1140},
    {"DirtyAllRigs", 0x006B58F0},
    {"RigCtor", 0x006BB8F0},
    {"RigVtable", 0x00FF4218},
    {"RigCtorCall0", 0x006F7905},
    {"RigCtorCall1", 0x006F795C},
    {"RigCtorCall2", 0x006F799D},
    {"RoomGatherCall", 0x006BBE70},
    {"RoomGather", 0x006BB2F0},
    {"CellGather", 0x006B5AF0},
    {"RigUpdate", 0x006BBF90},
    {"SetLightColour", 0x006BDA90},
    {"SetColourCall0", 0x006C047D},
    {"SetColourCall1", 0x006C051D},
    {"SetColourCall2", 0x006C05C1},
    {"SetColourCall3", 0x006C1251},
    {"SetColourCall4", 0x006C15D1},
    {"SetColourCall5", 0x006C1891},
    {"SetColourCall6", 0x006C1B11},
    {"ScriptSetColourCall", 0x006B0BDE},
    {"ScriptSetColour", 0x006BC3E0},
    {"AddWorldLights", 0x006C6AB0},
    {"AddWorldLightsCall0", 0x006C5816},
    {"AddWorldLightsCall1", 0x006C7094},
    {"LevelGather", 0x006C6990},
    {"LevelGatherCall0", 0x006C6B08},
    {"LevelGatherCall1", 0x006C6B2D},
    {"CascadeTest", 0x006C73AA},
    {"RoomByIdCall", 0x006C73F0},
    {"RoomById", 0x006A6550},
    {"InvalidateCall", 0x006C73FF},
    {"InvalidateRoom", 0x0069EED0},
    {"SetInsertCall", 0x006C741B},
    {"SetInsert", 0x00B7AAD0},
    {"SolvePoint", 0x0069FD60},
    {"SolvePointCall0", 0x006A1187},
    {"SolvePointCall1", 0x006A126F},
    {"SolvePointCall2", 0x006A3336},
    {"BatchSolveCall", 0x006A3336},
    {"LightEvalReturn", 0x0069FE19},
    {"WallTestCall", 0x0069FE93},
    {"WallTest", 0x0069FC40},
    {"BatchSamples", 0x01158AC8},
    {"WallCullBatchFn", 0x006A30B0},
    {"WallCullCall", 0x006A311F},
    {"WallCull", 0x0069DFF0},
    {"LightFilter", 0x006C7820},
    {"LightBright", 0x006BC520},
    {"AddRoomLight", 0x006A2060},
    {"RoomUpdatePush", 0x006C5E2A},
    {"RoomUpdate", 0x006C7250},
    {"LightEntryUpdate", 0x006C7BA0},
    {"ChangedClearCall", 0x006C7497},
    {"ChangedClear", 0x007F3790},
    {"FloorSet", 0x00A89DD0},
    {"FloorSetCall0", 0x00AA0ADB},
    {"FloorSetCall1", 0x00AA0CCC},
    {"FloorSetCall2", 0x00AA0E4A},
    {"FloorSetCall3", 0x00AA0F72},
    {"FloorRemove", 0x00A893A0},
    {"FloorRemoveCall", 0x00AA05C7},
    {"LevelVtable", 0x01062680},
    {"LevelCtorCall", 0x00AA179E},
    {"LevelCtor", 0x00A88790},
    {"LodChoice", 0x0069E710},
    {"LodChoiceCall0", 0x0069E82E},
    {"LodChoiceCall1", 0x0069EA86},
    {"LodChoiceCall2", 0x0069EF46},
    {"LodChoiceCall3", 0x0069F1B3},
    {"LodMax", 0x01158B00},
    {"RoomSolveStartCall", 0x006A3D0B},
    {"RoomSolveStart", 0x006A18B0},
    {"WallPassCall", 0x006A3D4C},
    {"WallPass", 0x006A3A30},
    {"WallSamplesCall", 0x006A3AF5},
    {"WallSamples", 0x006AC070},
    {"WallBlurCall", 0x006A3B62},
    {"WallBlur", 0x0069F650},
    {"WallBlurPasses", 0x01158B1C},
    {"WallBlurMode", 0x011D02E4},
    {"WallSolveCall", 0x006A3B0A},
    {"WallSolve", 0x006A31D0},
    {"RoomAmbient", 0x006A0F50},
    {"UnlitColourA", 0x006A0F95},
    {"UnlitColourB", 0x006A0F9C},
    {"DimAmbient", 0x006A00A0},
    {"DimColourA", 0x006A00C2},
    {"DimColourB", 0x006A00C9},
    {"DimAmbientCall0", 0x006A13F0},
    {"DimAmbientCall1", 0x006A1410},
    {"FillGate", 0x006BA57E},
    {"FillColour", 0x006B816D},
    {"RoomPriorityCall", 0x006A81DF},
    {"RoomPriority", 0x0069E770},
    {"LodStepSite", 0x0069EAA2},
    {"KeepClassA", 0x0069EF58},
    {"KeepClassB", 0x0069F1C5},
    {"InvalidateFlag", 0x0069F160},
    {"RoomPickJump", 0x006C5E39},
    {"RoomPick", 0x006C5C20},
    {"RoomSolveStep", 0x006A3F80},
    {"StopwatchCtor", 0x004F35B0},
    {"StopwatchStart", 0x00408700},
    {"StopwatchElapsed", 0x004F33C0},
    {"PriorityLotObject", 0x006FDE10},
    {"PriorityLotTest", 0x006FDC80},
    {"ModelDraw", 0x006F6250},
    {"BinderCall", 0x006F68C5},
    {"Binder", 0x006B8B30},
    {"InstanceFlush", 0x006CF920},
    {"GetLotIdGatherCall", 0x006B635D},
    {"GetLotId", 0x006BC020},
    {"ResFindProvider", 0x004AFFC0},
    {"ResFindProviderSlot0", 0x00FB2DE0},
    {"ResFindProviderSlot1", 0x00FFE290},
    {"RefPackCompress", 0x004EC200},
    {"RefPackCompressSlot", 0x00FB901C},
    {"SceneDrainCall", 0x006EBC49},
    {"SceneDrain", 0x006E4130},
    {"DxtEncode1", 0x006152F0},
    {"DxtEncode5", 0x006154B0},
    {"ObjectById", 0x00C62D40},
    {"RoomSolveCall", 0x00ADB9AD},
    {"RoomSolve", 0x006A8BA0},
    {"RemoteCallJob", 0x007D9840},
    {"RemoteMethodVtable", 0x010650C4},
    {"RemoteMethodVtable2", 0x010650D8},
    {"ResRegisterDb", 0x004B2D00},
    {"ResRegisterDbSlot", 0x00FB2DD4},
    {"ResRegisterDbDerived", 0x00736A70},
    {"ResRegisterDbDerivedSlot", 0x00FFE284},
    {"ResSetDbPriority", 0x004B2EC0},
    {"ResSetDbPrioritySlot0", 0x00FB2DDC},
    {"ResSetDbPrioritySlot1", 0x00FFE28C},
    {"ResDbChanged", 0x004B0960},
    {"ResDbChangedSlot0", 0x00FB2DEC},
    {"ResDbChangedSlot1", 0x00FFE29C},
    {"ShadowedDbVtable", 0x00FFE078},
    {"LotLightBudgetCall", 0x00ADB95D},
    {"LotLightBudget", 0x00ADB120},
    {"CameraRootCall", 0x00C6D5BD},
    {"CameraGetterCall", 0x00C6D5C4},
    {"CameraRootGetter", 0x006E8330},
    {"CameraGetter", 0x006E8400},
    {"RefPackDecompress", 0x004EB3B0},
    {"WallAoStep", 0x0068B810},
    {"WallAoStepSlot", 0x00FF05B0},
    {"WallAoDriver", 0x00688920},
    {"ResKeyList", 0x004B1AE0},
    {"ResKeyListSlot", 0x00FB2DC0},
    {"ResKeyListDerived", 0x00736660},
    {"ResKeyListDerivedSlot", 0x00FFE270},
    {"KeyTypeFilterVtable", 0x00FD8248},
    {"DpfVtable", 0x00FB2600},
    {"DpfDerivedVtable", 0x01048DA0},
    {"DdfVtable", 0x00FB2420},
    {"PackedStreamVtable", 0x00FFD790},
    {"DpfWriteDirect", 0x004A7FC0},
    {"SceneBoundsCall", 0x006E41DE},
    {"SceneNodeBounds", 0x006FB4B0},
    {"SceneSpatialCall", 0x006E41E6},
    {"SceneNodeSpatial", 0x006FAD70},
    {"SceneNodeDtor", 0x006FD930},
    {"SceneAddNode", 0x006E6480},
    {"SceneHolderTeardown", 0x006E4DE0},
    {"SwarmDrawableVtable", 0x00FFC248},
    {"SceneModelFurVtable", 0x00FF9B50},
    {"ObjectTreeWalk", 0x00C60D30},
    {"ObjectTreeSearch", 0x00C5FA60},
    {"LotLodScoring", 0x00C6C290},
    {"LotDetailRequest", 0x00AC20E0},
    {"LotLodThrottleTest", 0x00C6C695},
    {"LotLodThrottleFlag", 0x011ECBC0},
    {"LotLodActiveThresholdTest", 0x00C6C6B7},
    {"LotLodActiveThreshold", 0x01184328},
    {"LotVisibilityCameraBiasJZ", 0x00C63015},
    // Steam 1.67.2 addresses found offline in TS3W.exe with the fork's EA signatures (06/10, each unique; PostRemoteMethodCall: the
    // one called from LotDetailRequest at 0x00AC2117, the other copy has 4 unrelated callers)
    {"LotAddObjectsToScene", 0x00AC1130},
    {"LotUpdateObjectSceneNode", 0x00ABFAC0},
    {"ScriptMessageScopeCtor", 0x007D2DB0},
    {"ScriptMessageScopeDtor", 0x007D2DF0},
    {"PostRemoteMethodCall", 0x00ABE9C0},
    {"IsObjectLargeOrFlora", 0x00B088C0},
    {"WorldManagerPtr", 0x011ECBC4},
    {"TerrainUpdateCall", 0x00C6D68C},
    {"BakeColourSite", 0x00C2950F},
    {"SunlightScale", 0x011D0918},
    {"CasTriSort", 0x005D1960},
    {"AllocGlobal", 0x011CB864},
    {"AllocMmapFreeCall", 0x004E5306},
    {"RecordCrc", 0x004FA4C0},
    {"RecordCrcTable", 0x0114D330},
    {"TexCreateCall", 0x0060E1DC},
    {"TexCreate", 0x0060CEA0},
    {"TexFillCall", 0x0060E1FF},
    {"TexFill", 0x0060D290},
    {"UiServiceGetter", 0x0050AB70},
    {"RoomNormCall", 0x006A13B4},
    {"RoomNorm", 0x006A0230},
    {"BasisLightCall", 0x006A0C56},
    {"BasisLight", 0x0069F280},
    {"LampLitCall", 0x006C7CB6},
    {"LampMarkCall", 0x006C7CD6},
    {"LampMark", 0x006C7160},
    {"ResUpdateCall", 0x007377F7},
    {"ResUpdate", 0x00737560},
    {"ResShrinkBoth", 0x00733E70},
    {"WorldSaveCall", 0x00AAC320},
    {"WorldSave", 0x00C6D460},
    {"WindowRepaintJump", 0x004108AE},
    {"MonoTypeGetObject", 0x00EA8A00},
    {"MonoDomainFree", 0x00E75340},
    {"ObjMapFind", 0x00939100},
    {"ObjMapInsert", 0x00939170},
    {"ObjMapErase", 0x00938D00},
    {"RenMapFind", 0x006FDFB0},
    {"RenMapInsert", 0x006FE1F0},
    {"RenMapErase", 0x006FDF40},
    {"TreeSetLevel", 0x0070D370},
    {"TreeSetCustomRig", 0x0070D800},
    {"CompQueueCall", 0x006082F8},
    {"CompDispatch", 0x005FDEF0},
    {"CompState2Call", 0x005FDFEC},
    {"CompTileRender", 0x005FDDE0},
    {"CompState3Call", 0x005FDFFF},
    {"CompTileRead", 0x005FD420},
    {"CompReadbackCall", 0x005FDC8A},
    {"CompReadback", 0x005FBCE0},
    {"CompTileFetch", 0x005FDBF0},
    {"LightObjectRemove", 0x006C7610},
    {"LotLoadStagesCall", 0x00AEB306},
    {"LotPassRendererCall", 0x00C7CEDF},
    {"LotLoadStages", 0x00AEA680},
    {"LotBuildBudgetLoad", 0x00AEA6D8},
    {"ProbeDiffuseWrite", 0x006B4720},
    {"ProbeSpecularWrite", 0x006B4B10},
    {"LampK1", 0x011D0A60},
    {"LampK2", 0x011D0A68},
    {"LampCmax", 0x011D1160},
    {"StreetLampScale", 0x01158DA8},
    {"ConeScale5", 0x011D11A0},
    {"RigCutFlagged", 0x01158D64},
    {"RigCut", 0x01158D60},
    {"SolveTimeByClass", 0x011D1200},
    {"MapSlots", 0x011D0460},
    {"MapLocker", 0x011D0508},
    {"Finalize", 0x006A0E00},
    {"FinalizeCall", 0x006A3E65},
    {"MapEdges", 0x006A8CA0},
    {"EdgeCopy", 0x0069EFC0},
    {"MapUnlock", 0x0069E950},
    {"RoomLitNote", 0x006A08E0},
    {"ThresholdFn", 0x006A8E50},
    {"ThresholdFactor", 0x00FF36F4},
    {"ThresholdScale", 0x00FF3700},
    {"ClassWallModeFn", 0x006A8CB0},
    {"ClassWallMode", 0x00FF36A4},
    {"ClassWallsFn", 0x006A8CE0},
    {"ClassObjectsFn", 0x006A8D00},
    {"ClassWalls2D", 0x00FF36C4},
    {"ClassObjects3D", 0x00FF36D0},
    {"TexelsGetter", 0x006A8D20},
    {"TexelsPerTile", 0x00FF36AC},
    {"AtlasReads", 0x006A90FC},
    {"WallAtlasW", 0x00FF36DC},
    {"WallAtlasH", 0x00FF36E8},
    {"OriginPush", 0x006A7E5E},
    {"CopyPush", 0x006AA322},
    {"CopyBuild", 0x006A9AD0},
    {"CopyBuilt", 0x011D0510},
    {"WallUvCall0", 0x00C387B5},
    {"WallUvCall1", 0x00C387CC},
    {"WallUv", 0x00C38530},
    {"WallEdgeFn", 0x006A43B0},
    {"CutPostSite0", 0x00B0D866},
    {"CutPostSite1", 0x00B0DD15},
    {"OutdoorAlphaSite", 0x006A333B},
    {"WallBaseSite", 0x006AB32C},
    {"IndoorBaseSite", 0x006AB340},
    {"TexelPackCall", 0x006A34DB},
    {"TexelPack", 0x0060CD10},
    {"TexelAddrSite", 0x006A330C},
    {"PackConst255", 0x00FA312C},
    {"PackMagic", 0x00FE774C},
    {"OccMarkCall", 0x006C7939},
    {"EntryFlag", 0x006C4CF0},
    {"EntryFlagCall0", 0x006B0A8D},
    {"EntryFlagCall1", 0x006B0B33},
    {"EntryFlagCall2", 0x006B0BFA},
    {"EntryFlagCall3", 0x006B0C9A},
    {"EntryFlagCall4", 0x006B0D26},
};
static_assert(std::size(kInfo) == static_cast<size_t>(Id::Count), "kInfo must list every Id in order");

// ---------------------------------------------------------------------------------------------------------------------
// Signature table
// ---------------------------------------------------------------------------------------------------------------------
enum class K : uint8_t {
    Sig,       // one address from the signature (exactly one match, or every match gives the same value)
    Multi,     // exactly `arg` matches, the ids from `id` on get them by address
    InRange,   // the signature searched only in [Get(dep), Get(dep) + arg); exactly one match there
    LowestOf2, // exactly two matches (twin functions): the lower one
    CallIn,    // after the signature match, the CALL within `arg` bytes that lands on Get(dep)
    Deref,     // the dword at Get(dep) + arg (no signature)
    Target,    // the target of the CALL at Get(dep); the signatures are a fallback when dep is missing
    CallersOf, // every CALL in .text that lands on Get(dep): exactly `arg` of them, ids from `id` on, by address
    LightType, // the vtable the light factory's constructor for type `arg` stores (dep = the factory's jump table)
    SlotsOf,   // every 4-aligned dword equal to Get(dep) in the read-only data sections (vtable slots): exactly `arg`, ids from `id` on, by address
};
enum class M : uint8_t {
    At,    // address = match + offset
    Call,  // match + offset is a CALL rel32: address = its target
    Dword, // address = the dword at match + offset (a global, a vtable, a table)
};
enum class W : uint8_t { Text, Image }; // where the address must lie: the game's .text, or anywhere in its image

struct Sig {
    const char* pattern;
    int offset;
    M mode;
};
struct Entry {
    Id id;
    K kind;
    W where;
    Id dep;
    int arg;
    Sig sig[2]; // primary, alternate (tried when the primary is not unique); pattern nullptr = none
};

constexpr Id None = Id::Count;
#define NOSIG {nullptr, 0, M::At}

// clang-format off
// Order: every dependency before its users.
const Entry kTable[] = {
    // ---- Night Lights core ----
    {Id::RootGetter, K::Sig, W::Text, None, 0, {{"A1 ?? ?? ?? ?? 85 C0 75 01 C3 8B 80 C0 01 00 00 C3", 0, M::At}, {"C7 44 24 ?? ?? ?? ?? ?? E8 ?? ?? ?? ?? 3B C7 74 0C 8D 4C 24 ?? 51 8B C8 E8", 8, M::Call}}},
    {Id::RootPtr, K::Deref, W::Image, Id::RootGetter, 1, {NOSIG, NOSIG}},
    {Id::QueueRoom, K::Sig, W::Text, None, 0, {{"83 EC 2C 53 55 56 33 DB 8B F1 88 5C 24 0C 8B 44 24 0C", 0, M::At}, {"89 3E E8 ?? ?? ?? ?? 53 8B CF E8 ?? ?? ?? ?? 50 8B CE E8", 2, M::Call}}},
    {Id::TerrainVisitorSite, K::Sig, W::Text, None, 0, {{"56 57 8B 7C 24 0C 8B 07 8B 50 20 8B F1 8B CF FF D2 84 C0 74 ?? 8B 46 08 3B 46 0C", 6, M::At}, {"8B 07 8B 50 20 8B F1 8B CF FF D2 84 C0 74 ?? 8B 46 08 3B 46 0C 8D 4E 04", 0, M::At}}},
    {Id::ArmSiteRemoval, K::Multi, W::Text, None, 3, {{"8B 17 8B 42 20 8B CF FF D0 84 C0 74 ?? C7 46 38 32 00 00 00", 0, M::At}, {"80 7E 40 00 74 08 57 8B CE E8 ?? ?? ?? ?? 8B 17 8B 42 20 8B CF FF D0 84 C0 74", 14, M::At}}},
    {Id::EnumLights, K::Sig, W::Text, None, 0, {{"E8 ?? ?? ?? ?? 3B C7 74 0C 8D 4C 24 ?? 51 8B C8 E8 ?? ?? ?? ?? 8B 46 1C", 16, M::Call}, {"E8 ?? ?? ?? ?? 8B 4C 24 04 51 68 ?? ?? ?? ?? 8B C8 E8 ?? ?? ?? ?? C2 04 00", 0, M::At}}},
    {Id::ChunkRenderCall, K::Sig, W::Text, None, 0, {{"80 7E 54 00 74 ?? 6A 00 56 8B CF E8 ?? ?? ?? ?? C6 44 24 0C 01", 11, M::At}, {"6A 00 56 8B CF E8 ?? ?? ?? ?? C6 44 24 0C 01 EB", 5, M::At}}},
    {Id::ChunkRenderFn, K::Target, W::Text, Id::ChunkRenderCall, 0, {NOSIG, NOSIG}},
    // ---- light classes ----
    {Id::LightJumpTable, K::Sig, W::Text, None, 0, {{"8B B1 04 01 00 00 33 C0 85 F6 0F 84 ?? ?? ?? ?? 8B 4C 24 08 83 C1 FD 83 F9 08 0F 87 ?? ?? ?? ?? FF 24 8D", 35, M::Dword}, {"85 F6 0F 84 ?? ?? ?? ?? 8B 4C 24 08 83 C1 FD 83 F9 08 0F 87 ?? ?? ?? ?? FF 24 8D", 27, M::Dword}}},
    {Id::LightVtable3, K::LightType, W::Image, Id::LightJumpTable, 3, {NOSIG, NOSIG}},
    {Id::LightVtable4, K::LightType, W::Image, Id::LightJumpTable, 4, {NOSIG, NOSIG}},
    {Id::LightVtable5, K::LightType, W::Image, Id::LightJumpTable, 5, {NOSIG, NOSIG}},
    {Id::LightVtable6, K::LightType, W::Image, Id::LightJumpTable, 6, {NOSIG, NOSIG}},
    {Id::LightVtable7, K::LightType, W::Image, Id::LightJumpTable, 7, {NOSIG, NOSIG}},
    {Id::LightVtable8, K::LightType, W::Image, Id::LightJumpTable, 8, {NOSIG, NOSIG}},
    {Id::LightVtable9, K::LightType, W::Image, Id::LightJumpTable, 9, {NOSIG, NOSIG}},
    {Id::LightVtable10, K::LightType, W::Image, Id::LightJumpTable, 10, {NOSIG, NOSIG}},
    {Id::LightVtable11, K::LightType, W::Image, Id::LightJumpTable, 11, {NOSIG, NOSIG}},
    {Id::LightColour3, K::Deref, W::Text, Id::LightVtable3, 0x10, {NOSIG, NOSIG}},
    {Id::LightColour4, K::Deref, W::Text, Id::LightVtable4, 0x10, {NOSIG, NOSIG}},
    {Id::LightColour5, K::Deref, W::Text, Id::LightVtable5, 0x10, {NOSIG, NOSIG}},
    {Id::LightColour6, K::Deref, W::Text, Id::LightVtable6, 0x10, {NOSIG, NOSIG}},
    {Id::LightColour7, K::Deref, W::Text, Id::LightVtable7, 0x10, {NOSIG, NOSIG}},
    {Id::LightColour8, K::Deref, W::Text, Id::LightVtable8, 0x10, {NOSIG, NOSIG}},
    {Id::LightColour9, K::Deref, W::Text, Id::LightVtable9, 0x10, {NOSIG, NOSIG}},
    {Id::LightColour10, K::Deref, W::Text, Id::LightVtable10, 0x10, {NOSIG, NOSIG}},
    {Id::LightColour11, K::Deref, W::Text, Id::LightVtable11, 0x10, {NOSIG, NOSIG}},
    {Id::LightEval3, K::Deref, W::Text, Id::LightVtable3, 0x4C, {NOSIG, NOSIG}},
    {Id::LightEval4, K::Deref, W::Text, Id::LightVtable4, 0x4C, {NOSIG, NOSIG}},
    {Id::LightEval5, K::Deref, W::Text, Id::LightVtable5, 0x4C, {NOSIG, NOSIG}},
    {Id::LightEval6, K::Deref, W::Text, Id::LightVtable6, 0x4C, {NOSIG, NOSIG}},
    {Id::LightEval7, K::Deref, W::Text, Id::LightVtable7, 0x4C, {NOSIG, NOSIG}},
    {Id::LightEval8, K::Deref, W::Text, Id::LightVtable8, 0x4C, {NOSIG, NOSIG}},
    {Id::LightEval9, K::Deref, W::Text, Id::LightVtable9, 0x4C, {NOSIG, NOSIG}},
    {Id::LightEval10, K::Deref, W::Text, Id::LightVtable10, 0x4C, {NOSIG, NOSIG}},
    {Id::LightEval11, K::Deref, W::Text, Id::LightVtable11, 0x4C, {NOSIG, NOSIG}},
    {Id::LightPos, K::Deref, W::Text, Id::LightVtable3, 0x24, {NOSIG, NOSIG}},
    // ---- Night Lights developer options ----
    {Id::LampColourSite, K::InRange, W::Text, Id::LightEval11, 0x300, {{"0F 28 86 E0 00 00 00 8B 55 10", 0, M::At}, {"0F 28 86 E0 00 00 00", 0, M::At}}},
    {Id::LotPassSite, K::Sig, W::Text, None, 0, {{"8B 7D 08 8B 87 D8 00 00 00 85 C0 F3 0F 10 05 ?? ?? ?? ?? F3 0F 11 44 24 18 74 13", 3, M::At}, {"8B 87 D8 00 00 00 85 C0 F3 0F 10 05 ?? ?? ?? ?? F3 0F 11 44 24 ?? 74", 0, M::At}}},
    {Id::LotPassConst, K::Deref, W::Image, Id::LotPassSite, 0x0C, {NOSIG, NOSIG}},
    {Id::LotPassTexGlobal, K::Deref, W::Image, Id::LotPassSite, 0x1A, {NOSIG, NOSIG}},
    {Id::LotPassNullBind, K::InRange, W::Text, Id::LotPassSite, 0x80, {{"80 78 1D 00 75 ?? A1 ?? ?? ?? ?? 6A 00 6A 00 50", 6, M::At}, {"A1 ?? ?? ?? ?? 6A 00 6A 00 50 8B CE E8", 0, M::At}}},
    {Id::QualitySite0, K::Multi, W::Text, None, 2, {{"75 0B 80 ?? 4D 00 C6 44 24 0C 00 74 05 C6 44 24 0C 01", 6, M::At}, {"4D 00 C6 44 24 0C 00 74 05 C6 44 24 0C 01", 2, M::At}}},
    // ---- object light bridge ----
    {Id::CapOperandSite, K::Sig, W::Text, None, 0, {{"0F 5F F0 0F 28 00 0F 5F F0 B9 ?? ?? ?? ?? 0F 29 74 24 30 E8", 9, M::At}, {"B9 ?? ?? ?? ?? 0F 29 74 24 30 E8 ?? ?? ?? ?? D9 00", 0, M::At}}},
    {Id::CapGlobal, K::Deref, W::Image, Id::CapOperandSite, 1, {NOSIG, NOSIG}},
    {Id::RigGatherReturn, K::Sig, W::Text, None, 0, {{"8B 17 8B 52 10 8D 44 24 10 50 8D 8E 40 01 00 00 51 8B CF FF D2 83 7E 08 00 74", 21, M::At}, {"8D 8E 40 01 00 00 51 8B CF FF D2 83 7E 08 00", 11, M::At}}},
    {Id::LumaWeights, K::Sig, W::Image, None, 0, {{"0F 29 4E 10 0F 28 05 ?? ?? ?? ?? 0F 28 4E 10", 7, M::Dword}, {"0F 29 71 10 0F 28 05 ?? ?? ?? ??", 7, M::Dword}}},
    {Id::DirtyAllRigs, K::Sig, W::Text, None, 0, {{"83 EC 10 55 8B E9 33 C9 33 C0 39 4D 30 89 44 24 0C 76", 0, M::At}, {"55 8B E9 33 C9 33 C0 39 4D 30", -3, M::At}}},
    {Id::RigCtor, K::Sig, W::Text, None, 0, {{"56 8B F1 57 C7 06 ?? ?? ?? ?? 33 C0 8D 4E 04 87 01 8A 54 24 0C 8A 86 24 02 00 00", 0, M::At}, {"C7 06 ?? ?? ?? ?? 33 C0 8D 4E 04 87 01 8A 54 24 0C 8A 86 24 02 00 00", -4, M::At}}},
    {Id::RigVtable, K::InRange, W::Image, Id::RigCtor, 0x80, {{"C7 06 ?? ?? ?? ?? C7 46 08 00 00 00 00 C7 46 0C 00 00 00 00", 2, M::Dword}, {"24 E7 0A D0 8B CE C7 06 ?? ?? ?? ??", 8, M::Dword}}},
    {Id::RigCtorCall0, K::CallIn, W::Text, Id::RigCtor, 0x18, {{"6A 01 56 8B C8 81 E2 01 FF FF FF 52 E8", 0, M::At}, {"9C 02 00 00 C0 ?? 04 6A 01 56", 0, M::At}}},
    {Id::RigCtorCall1, K::CallIn, W::Text, Id::RigCtor, 0x18, {{"6A 02 56 81 E1 01 FF FF FF 51 8B C8 E8", 0, M::At}, {"9C 02 00 00 C0 ?? 04 6A 02 56", 0, M::At}}},
    {Id::RigCtorCall2, K::CallIn, W::Text, Id::RigCtor, 0x18, {{"6A 00 56 81 E1 01 FF FF FF 51 8B C8 E8", 0, M::At}, {"9C 02 00 00 C0 ?? 04 6A 00 56", 0, M::At}}},
    {Id::RoomGatherCall, K::Sig, W::Text, None, 0, {{"8D 97 C8 00 00 00 52 8D 47 30 50 8B CE E8 ?? ?? ?? ?? 8B CE E8", 13, M::At}, {"8D 47 30 50 8B CE E8 ?? ?? ?? ?? 8B CE E8", 6, M::At}}},
    {Id::RoomGather, K::Target, W::Text, Id::RoomGatherCall, 0, {NOSIG, NOSIG}},
    {Id::CellGather, K::Sig, W::Text, None, 0, {{"F6 86 24 02 00 00 10 74 ?? 8B 88 04 01 00 00 56 E8 ?? ?? ?? ?? 8B 0D", 16, M::Call}, {"83 EC 1C 8B 54 24 20 53 55 56 57 8B F1 8D 44 24 1C 50 8D 4C 24 28", 0, M::At}}},
    {Id::RigUpdate, K::Sig, W::Text, None, 0, {{"80 A1 24 02 00 00 F7 83 B9 EC 01 00 00 00 74 06 83 79 08 00 74 ?? 83 B9 D4 01 00 00 02", 0, M::At}, {"80 A1 24 02 00 00 F7 83 B9 EC 01 00 00 00", 0, M::At}}},
    {Id::SetLightColour, K::Sig, W::Text, None, 0, {{"74 ?? 8D 4F 10 51 8B CE E8 ?? ?? ?? ?? D9 47 1C", 8, M::Call}, {"8D 4F 10 51 8B CE E8 ?? ?? ?? ?? D9 47 1C", 6, M::Call}}},
    {Id::SetColourCall0, K::CallersOf, W::Text, Id::SetLightColour, 7, {NOSIG, NOSIG}},
    {Id::ScriptSetColourCall, K::Sig, W::Text, None, 0, {{"8B 0E 8D 44 24 10 50 0F 29 44 24 14 E8 ?? ?? ?? ?? 83 C6 04", 12, M::At}, {"50 0F 29 44 24 14 E8 ?? ?? ?? ?? 83 C6 04", 6, M::At}}},
    {Id::ScriptSetColour, K::Target, W::Text, Id::ScriptSetColourCall, 0, {{"55 8B EC 83 E4 F0 8B 45 08 0F 28 00 0F 29 81 F0 00 00 00 F6 81 00 01 00 00 20", 0, M::At}, NOSIG}},
    // ---- light between stories ----
    {Id::AddWorldLights, K::Sig, W::Text, None, 0, {{"55 8B EC 83 E4 F0 83 EC 34 53 56 33 DB F6 05 ?? ?? ?? ?? 01 57 8B F9 75", 0, M::At}, {"83 EC 34 53 56 33 DB F6 05 ?? ?? ?? ?? 01 57 8B F9", -6, M::At}}},
    {Id::AddWorldLightsCall0, K::CallersOf, W::Text, Id::AddWorldLights, 2, {NOSIG, NOSIG}},
    {Id::LevelGather, K::InRange, W::Text, Id::AddWorldLights, 0x100, {{"6A 01 56 8B CF E8 ?? ?? ?? ?? 39 5E 0C", 5, M::Call}, {"53 56 8D 88 A0 06 00 00 E8", 8, M::Call}}},
    {Id::LevelGatherCall0, K::CallersOf, W::Text, Id::LevelGather, 2, {NOSIG, NOSIG}},
    {Id::CascadeTest, K::Sig, W::Text, None, 0, {{"39 86 A0 01 00 00 0F 85 ?? ?? ?? ?? 33 ED 83 FD FC 8B C5 7D 07 B8 FC FF FF FF EB 0A 83 FD 08 7C 05 B8 07 00 00 00", 0, M::At}, {"39 86 A0 01 00 00 0F 85 ?? ?? ?? ?? 33 ED 83 FD FC", 0, M::At}}},
    {Id::RoomByIdCall, K::InRange, W::Text, Id::CascadeTest, 0x100, {{"74 ?? 6A 00 E8 ?? ?? ?? ?? 85 C0 74 ?? 6A 00 6A 01 8B C8 E8", 4, M::At}, {"6A 00 E8 ?? ?? ?? ?? 85 C0 74", 2, M::At}}},
    {Id::RoomById, K::Target, W::Text, Id::RoomByIdCall, 0, {NOSIG, NOSIG}},
    {Id::InvalidateCall, K::InRange, W::Text, Id::RoomByIdCall, 0x20, {{"6A 00 6A 01 8B C8 E8", 6, M::At}, {"8B C8 E8", 2, M::At}}},
    {Id::InvalidateRoom, K::Target, W::Text, Id::InvalidateCall, 0, {NOSIG, NOSIG}},
    {Id::SetInsertCall, K::InRange, W::Text, Id::CascadeTest, 0x100, {{"C6 44 24 10 00 8B 54 24 10 52 8D 44 24 18 50 8D 4C 24 34 51 8D 4F 28 E8", 23, M::At}, {"51 8D 4F 28 E8", 4, M::At}}},
    {Id::SetInsert, K::Target, W::Text, Id::SetInsertCall, 0, {NOSIG, NOSIG}},
    {Id::SolvePoint, K::Sig, W::Text, None, 0, {{"55 8B EC 83 E4 F0 83 EC 74 0F 57 C0 8B 45 08 53 8B D9 8B 8B CC 00 00 00 2B 8B C8 00 00 00", 0, M::At}, {"8D 54 24 30 52 8B CB E8 ?? ?? ?? ?? 80 7B 18 00 0F 57 C9 0F 85", 7, M::Call}}},
    {Id::SolvePointCall0, K::CallersOf, W::Text, Id::SolvePoint, 3, {NOSIG, NOSIG}},
    {Id::BatchSolveCall, K::Sig, W::Text, None, 0, {{"8D 54 24 30 52 8B CB E8 ?? ?? ?? ?? 80 7B 18 00 0F 57 C9 0F 85", 7, M::At}, {"E8 ?? ?? ?? ?? 80 7B 18 00 0F 57 C9 0F 85", 0, M::At}}},
    {Id::LightEvalReturn, K::InRange, W::Text, Id::SolvePoint, 0x300, {{"8B 3E 50 52 8B 57 4C 8B CE FF D2 0F 28 4C 24 40", 11, M::At}, {"8B 57 4C 8B CE FF D2", 7, M::At}}},
    {Id::WallTestCall, K::InRange, W::Text, Id::SolvePoint, 0x300, {{"8B CB E8 ?? ?? ?? ?? 84 C0 74 ?? 8B 45 14 80 78 01 00", 2, M::At}, {"E8 ?? ?? ?? ?? 84 C0 74 ?? 8B 45 14 80 78 01 00", 0, M::At}}},
    {Id::WallTest, K::Target, W::Text, Id::WallTestCall, 0, {NOSIG, NOSIG}},
    {Id::BatchSamples, K::Sig, W::Image, None, 0, {{"8D 46 44 50 68 ?? ?? ?? ?? E8", 5, M::Dword}, {"8D 46 30 50 83 C6 44 56 68 ?? ?? ?? ??", 9, M::Dword}}},
    {Id::WallCullBatchFn, K::LowestOf2, W::Text, None, 0, {{"55 8B EC 83 E4 F0 83 EC 24 53 56 8B F1 8B 9E CC 00 00 00 2B 9E C8 00 00 00 57 8B 7D 0C", 0, M::At}, {"8B 9E CC 00 00 00 2B 9E C8 00 00 00 57 8B 7D 0C 8B CF 89 74 24 1C C1 FB 02", -13, M::At}}},
    {Id::WallCullCall, K::InRange, W::Text, Id::WallCullBatchFn, 0x90, {{"56 83 C1 30 E8", 4, M::At}, {"83 C1 30 E8", 3, M::At}}},
    {Id::WallCull, K::Target, W::Text, Id::WallCullCall, 0, {NOSIG, NOSIG}},
    // ---- indoor lamps through stair openings ----
    {Id::LightFilter, K::Sig, W::Text, None, 0, {{"53 8B 5C 24 08 56 8B F1 8B 46 1C 3B 43 0C 75 ?? 8B 4E 20 F6 81 90 00 00 00 02 74", 0, M::At}, {"8B 46 1C 3B 43 0C 75 ?? 8B 4E 20 F6 81 90 00 00 00 02 74", -8, M::At}}},
    {Id::LightBright, K::InRange, W::Text, Id::LightFilter, 0x40, {{"F6 81 00 01 00 00 20 74 ?? E8", 9, M::Call}, {"20 74 ?? E8 ?? ?? ?? ?? 84 C0 74", 3, M::Call}}},
    {Id::AddRoomLight, K::InRange, W::Text, Id::LightFilter, 0x70, {{"57 8B CB E8", 3, M::Call}, {"83 F8 0B 75 ?? 57 8B CB E8", 8, M::Call}}},
    {Id::RoomUpdatePush, K::Sig, W::Text, None, 0, {{"56 6A 00 8B F1 E8 ?? ?? ?? ?? 68 ?? ?? ?? ?? 8B CE E8 ?? ?? ?? ?? 8B CE 5E E9", 10, M::At}, {"6A 00 8B F1 E8 ?? ?? ?? ?? 68 ?? ?? ?? ?? 8B CE E8", 9, M::At}}},
    {Id::RoomUpdate, K::Deref, W::Text, Id::RoomUpdatePush, 1, {NOSIG, NOSIG}},
    {Id::LightEntryUpdate, K::Sig, W::Text, None, 0, {{"55 8B EC 83 E4 F0 83 EC 24 53 56 8B F1 83 7E 24 00 57 0F 84 ?? ?? ?? ?? 8B 46 14 85 C0 0F 84", 0, M::At}, {"83 7E 24 00 57 0F 84 ?? ?? ?? ?? 8B 46 14 85 C0 0F 84 ?? ?? ?? ?? 83 38 00 0F 84", -13, M::At}}},
    {Id::ChangedClearCall, K::InRange, W::Text, Id::CascadeTest, 0x100, {{"8D 7E 08 52 50 8B CF E8", 7, M::At}, {"52 50 8B CF E8", 4, M::At}}},
    {Id::ChangedClear, K::Target, W::Text, Id::ChangedClearCall, 0, {NOSIG, NOSIG}},
    {Id::FloorSet, K::Sig, W::Text, None, 0, {{"53 55 56 8B F1 83 BE 64 02 00 00 00 57 75 ?? 6A 00 6A 00 6A 00 6A 00 68", 0, M::At}, {"83 BE 64 02 00 00 00 57 75 ?? 6A 00 6A 00 6A 00 6A 00 68", -5, M::At}}},
    {Id::FloorSetCall0, K::CallersOf, W::Text, Id::FloorSet, 4, {NOSIG, NOSIG}},
    {Id::FloorRemove, K::Sig, W::Text, None, 0, {{"53 55 56 57 8B F9 83 BF 64 02 00 00 00 75 ?? 6A 00 6A 00 6A 00 6A 00 68", 0, M::At}, {"57 8B F9 83 BF 64 02 00 00 00 75 ?? 6A 00", -3, M::At}}},
    {Id::FloorRemoveCall, K::CallersOf, W::Text, Id::FloorRemove, 1, {NOSIG, NOSIG}},
    {Id::LevelVtable, K::Sig, W::Image, None, 0, {{"E8 ?? ?? ?? ?? C7 06 ?? ?? ?? ?? 88 9E 10 02 00 00 89 9E 14 02 00 00", 7, M::Dword}, {"80 BE 10 02 00 00 00 C7 06 ?? ?? ?? ?? 74", 9, M::Dword}}},
    {Id::LevelCtorCall, K::Sig, W::Text, None, 0, {{"83 C4 30 85 C0 74 0B 8B C8 E8 ?? ?? ?? ?? 8B F8", 9, M::At}, {"68 50 03 00 00 ?? ?? ?? E8 ?? ?? ?? ?? 83 C4 30 85 C0 74 0B 8B C8 E8", 22, M::At}}},
    {Id::LevelCtor, K::Target, W::Text, Id::LevelCtorCall, 0, {NOSIG, NOSIG}},
    {Id::LodChoice, K::Sig, W::Text, None, 0, {{"8B 11 8B 82 88 00 00 00 56 8B B2 84 02 00 00 3B C6 7E", 0, M::At}, {"8B 82 88 00 00 00 56 8B B2 84 02 00 00 3B C6", -2, M::At}}},
    {Id::LodChoiceCall0, K::CallersOf, W::Text, Id::LodChoice, 4, {NOSIG, NOSIG}},
    {Id::LodMax, K::Deref, W::Image, Id::LodChoice, 0x4C, {NOSIG, NOSIG}},
    {Id::RoomSolveStartCall, K::Sig, W::Text, None, 0, {{"8B CE DD D8 E8 ?? ?? ?? ?? 88 9E 38 06 00 00 C7 86 EC 00 00 00 01 00 00 00", 4, M::At}, {"E8 ?? ?? ?? ?? 88 9E 38 06 00 00 C7 86 EC 00 00 00 01", 0, M::At}}},
    {Id::RoomSolveStart, K::Target, W::Text, Id::RoomSolveStartCall, 0, {NOSIG, NOSIG}},
    {Id::WallPassCall, K::Sig, W::Text, None, 0, {{"D9 1C 24 57 8B CE E8 ?? ?? ?? ?? 84 C0 0F 84 ?? ?? ?? ?? 88 9E 38 06 00 00 C7 86 EC 00 00 00 03", 6, M::At}, {"57 8B CE E8 ?? ?? ?? ?? 84 C0 0F 84 ?? ?? ?? ?? 88 9E 38 06 00 00 C7 86 EC 00 00 00 03", 3, M::At}}},
    {Id::WallPass, K::Target, W::Text, Id::WallPassCall, 0, {NOSIG, NOSIG}},
    {Id::WallSamplesCall, K::InRange, W::Text, Id::WallPass, 0x150, {{"68 ?? ?? ?? ?? 52 50 8B CF E8", 9, M::At}, {"52 50 8B CF E8", 4, M::At}}},
    {Id::WallSamples, K::Target, W::Text, Id::WallSamplesCall, 0, {NOSIG, NOSIG}},
    {Id::WallBlurCall, K::InRange, W::Text, Id::WallPass, 0x150, {{"8B CE E8 ?? ?? ?? ?? 5F 5E 5D B0 01", 2, M::At}, {"E8 ?? ?? ?? ?? 5F 5E 5D B0 01 5B", 0, M::At}}},
    {Id::WallBlur, K::Target, W::Text, Id::WallBlurCall, 0, {NOSIG, NOSIG}},
    {Id::WallBlurPasses, K::Deref, W::Image, Id::WallBlur, 0x20, {NOSIG, NOSIG}},
    {Id::WallBlurMode, K::Deref, W::Image, Id::WallBlur, 0x88, {NOSIG, NOSIG}},
    {Id::WallSolveCall, K::InRange, W::Text, Id::WallPass, 0x150, {{"53 68 ?? ?? ?? ?? 8B CE E8", 8, M::At}, {"68 ?? ?? ?? ?? 8B CE E8", 7, M::At}}},
    {Id::WallSolve, K::Target, W::Text, Id::WallSolveCall, 0, {NOSIG, NOSIG}},
    {Id::RoomAmbient, K::Sig, W::Text, None, 0, {{"55 8B EC 83 E4 F0 81 EC 04 01 00 00 53 56 8B F1 8B 86 C8 00 00 00 3B 86 CC 00 00 00 57 75", 0, M::At}, {"8B 86 C8 00 00 00 3B 86 CC 00 00 00 57 75 ?? 8B 56 14", -16, M::At}}},
    {Id::UnlitColourA, K::InRange, W::Text, Id::RoomAmbient, 0x80, {{"84 C0 B9 ?? ?? ?? ?? 75 05 B9 ?? ?? ?? ?? E8", 3, M::At}, {"B9 ?? ?? ?? ?? 75 05 B9", 1, M::At}}},
    {Id::UnlitColourB, K::InRange, W::Text, Id::RoomAmbient, 0x80, {{"84 C0 B9 ?? ?? ?? ?? 75 05 B9 ?? ?? ?? ?? E8", 10, M::At}, {"B9 ?? ?? ?? ?? 75 05 B9", 8, M::At}}},
    {Id::DimAmbient, K::Sig, W::Text, None, 0, {{"55 8B EC 83 E4 F0 83 EC 3C 8B C1 8B 50 14 8B 40 10 8B 0D", 0, M::At}, {"83 EC 3C 8B C1 8B 50 14 8B 40 10 8B 0D", -6, M::At}}},
    {Id::DimColourA, K::InRange, W::Text, Id::DimAmbient, 0x40, {{"84 C0 B9 ?? ?? ?? ?? 75 05 B9 ?? ?? ?? ?? E8", 3, M::At}, {"B9 ?? ?? ?? ?? 75 05 B9", 1, M::At}}},
    {Id::DimColourB, K::InRange, W::Text, Id::DimAmbient, 0x40, {{"84 C0 B9 ?? ?? ?? ?? 75 05 B9 ?? ?? ?? ?? E8", 10, M::At}, {"B9 ?? ?? ?? ?? 75 05 B9", 8, M::At}}},
    {Id::DimAmbientCall0, K::CallersOf, W::Text, Id::DimAmbient, 2, {NOSIG, NOSIG}},
    {Id::FillGate, K::Sig, W::Text, None, 0, {{"80 3D ?? ?? ?? ?? 00 74 26 8B CF E8", 2, M::At}, {"84 9F 24 02 00 00 74 2F 80 3D ?? ?? ?? ?? 00", 10, M::At}}},
    {Id::FillColour, K::Sig, W::Text, None, 0, {{"0F 28 15 ?? ?? ?? ?? 0F 58 C1 0F 59 C5 0F 57 C9", 3, M::At}, {"0F 28 15 ?? ?? ?? ?? 0F 58 C1 0F 59 C5", 3, M::At}}},
    {Id::RoomPriorityCall, K::Sig, W::Text, None, 0, {{"8B CF E8 ?? ?? ?? ?? 51 D9 1C 24 57 8D 4C 24 20 E8", 2, M::At}, NOSIG}},
    {Id::RoomPriority, K::Target, W::Text, Id::RoomPriorityCall, 0, {{"83 EC 0C 56 8B F1 57 8B 3E 85 FF 75 08 D9 EE", 0, M::At}, NOSIG}},
    {Id::LodStepSite, K::Sig, W::Text, None, 0, {{"85 FF 75 0A BF 01 00 00 00 8D 5F 01 EB 0C", 4, M::At}, NOSIG}},
    {Id::KeepClassA, K::Sig, W::Text, None, 0, {{"83 F9 04 74 0F 3B C8 7C 0B 5F 89 86 F4 00 00 00", 7, M::At}, NOSIG}},
    {Id::KeepClassB, K::Sig, W::Text, None, 0, {{"83 F9 04 74 06 3B C8 7C 02 8B F8 89 BE F4 00 00 00", 7, M::At}, NOSIG}},
    {Id::InvalidateFlag, K::Sig, W::Text, None, 0, {{"8A 44 24 04 56 8B F1 3A 46 19 74 ?? 8B 0E 85 C9 88 46 19", 0, M::At}, NOSIG}},
    {Id::RoomPickJump, K::Sig, W::Text, None, 0, {{"8B CE 5E E9 ?? ?? ?? ?? CC CC 8B 4C 24 04", 3, M::At}, NOSIG}},
    {Id::RoomPick, K::Sig, W::Text, None, 0, {{"81 EC 14 04 00 00 55 8B E9 83 7D 74 00 0F 85", 0, M::At}, NOSIG}},
    {Id::RoomSolveStep, K::Sig, W::Text, None, 0, {{"83 B9 F0 00 00 00 03 75 12", 0, M::At}, NOSIG}},
    {Id::StopwatchCtor, K::Sig, W::Text, None, 0, {{"6A 04 8D 4C 24 18 8D 6C 10 C0 E8 ?? ?? ?? ?? 8D 4C 24 10 E8", 10, M::Call}, NOSIG}},
    {Id::StopwatchStart, K::Sig, W::Text, None, 0, {{"6A 04 8D 4C 24 18 8D 6C 10 C0 E8 ?? ?? ?? ?? 8D 4C 24 10 E8", 19, M::Call}, NOSIG}},
    {Id::StopwatchElapsed, K::Sig, W::Text, None, 0, {{"8D 4C 24 14 E8 ?? ?? ?? ?? D9 44 24 10 D9 C9", 4, M::Call}, NOSIG}},
    {Id::PriorityLotObject, K::Sig, W::Text, None, 0, {{"40 4C 50 51 E8 ?? ?? ?? ?? 8B C8 E8 ?? ?? ?? ?? 84 C0", 4, M::Call}, NOSIG}},
    {Id::PriorityLotTest, K::Sig, W::Text, None, 0, {{"40 4C 50 51 E8 ?? ?? ?? ?? 8B C8 E8 ?? ?? ?? ?? 84 C0", 11, M::Call}, NOSIG}},
    // ---- rig tracker ----
    {Id::ModelDraw, K::Sig, W::Text, None, 0, {{"55 8B EC 83 E4 F0 81 EC 94 01 00 00 53 8B D9 F7 43 40 00 10 00 00 56 57 0F 85", 0, M::At}, {"55 8B EC 83 E4 F0 81 EC ?? ?? 00 00 53 8B D9 F7 43 40 00 10 00 00 56 57 0F 85", 0, M::At}}},
    {Id::BinderCall, K::InRange, W::Text, Id::ModelDraw, 0x1000, {{"0F 95 44 24 16 8B CF E8 ?? ?? ?? ?? 6A 00 E8", 7, M::At}, {"3A C1 0F 95 44 24 ?? 8B CF E8", 9, M::At}}},
    {Id::Binder, K::Target, W::Text, Id::BinderCall, 0, {NOSIG, NOSIG}},
    {Id::InstanceFlush, K::Sig, W::Text, None, 0, {{"55 8B EC 83 E4 F0 81 EC A4 0B 00 00 53 56 8B F1 80 7E 4C 01 57 89 74 24 10 0F 84", 0, M::At}, {"55 8B EC 83 E4 F0 81 EC ?? ?? 00 00 53 56 8B F1 80 7E 4C 01 57 89 74 24 10 0F 84", 0, M::At}}},
    // ---- Every-Story Ground Light ----
    {Id::GetLotIdGatherCall, K::Sig, W::Text, None, 0, {{"FF D0 84 C0 74 ?? 8B CE E8 ?? ?? ?? ?? 0B C2 75 ?? 8B 45 04", 8, M::At}, {"8B CE E8 ?? ?? ?? ?? 0B C2 75 ?? 8B 45 04", 2, M::At}}},
    {Id::GetLotId, K::Target, W::Text, Id::GetLotIdGatherCall, 0, {{"8B 81 C0 00 00 00 8B 91 C4 00 00 00 C3", 0, M::At}, NOSIG}},
    // ---- Frame Profiler counters (the profiler is development build only) ----
    {Id::ResFindProvider, K::Sig, W::Text, None, 0, {{"51 53 55 56 57 8B F9 8D 5F 48 68 ?? ?? ?? ?? 8B CB E8 ?? ?? ?? ?? 8B 77 30 8B 6F 34 3B F5", 0, M::At}, {"8B F9 8D 5F 48 68 ?? ?? ?? ?? 8B CB E8 ?? ?? ?? ?? 8B 77 30 8B 6F 34 3B F5 C7 44 24 10 00 00 00 00", -5, M::At}}},
    {Id::ResFindProviderSlot0, K::SlotsOf, W::Image, Id::ResFindProvider, 2, {NOSIG, NOSIG}},
    {Id::RefPackCompress, K::Sig, W::Text, None, 0, {{"8B 54 24 14 33 C0 F6 C2 02 74 07 B8 01 00 00 00 EB 0D F7 C2 00 00 01 00 74 05 B8 02 00 00 00 56", 0, M::At}, {"F6 C2 02 74 07 B8 01 00 00 00 EB 0D F7 C2 00 00 01 00 74 05 B8 02 00 00 00 56 8B 74 24 10 85 F6", -6, M::At}}},
    {Id::RefPackCompressSlot, K::SlotsOf, W::Image, Id::RefPackCompress, 1, {NOSIG, NOSIG}},
    {Id::SceneDrainCall, K::Sig, W::Text, None, 0, {{"8B 4E 08 E8 ?? ?? ?? ?? 80 BE A2 02 00 00 00 75 ?? 8B 4E 38 E8", 3, M::At}, {"E8 ?? ?? ?? ?? 80 BE A2 02 00 00 00 75 ?? 8B 4E 38 E8 ?? ?? ?? ?? 8B 4E 38 E8", 0, M::At}}},
    {Id::SceneDrain, K::Target, W::Text, Id::SceneDrainCall, 0, {{"55 8B EC 83 E4 F0 83 EC 34 53 56 57 8B F9 8B 77 20 8B 5F 24 8D 47 20 3B F0", 0, M::At}, NOSIG}},
    {Id::DxtEncode1, K::Sig, W::Text, None, 0, {{"55 8B EC 83 E4 F0 81 EC 54 01 00 00 8B 45 08 8B 50 04 8B 48 08 53 56 8D 72 03", 0, M::At}, {"81 EC 54 01 00 00 8B 45 08 8B 50 04 8B 48 08 53 56 8D 72 03 83 E6 FC 03 F6", -6, M::At}}},
    {Id::DxtEncode5, K::Sig, W::Text, None, 0, {{"55 8B EC 83 E4 F0 81 EC A4 01 00 00 8B 45 08 8B 48 04 8D 51 03 83 E2 FC", 0, M::At}, {"81 EC A4 01 00 00 8B 45 08 8B 48 04 8D 51 03 83 E2 FC 03 D2 03 D2 53 8B 18", -6, M::At}}},
    {Id::ObjectById, K::Sig, W::Text, None, 0, {{"8B 44 24 0C 8B 54 24 08 56 50 8B 44 24 0C 52 50 E8 ?? ?? ?? ?? 8B F0 85 F6 74 14 8B 16 8B 42 40 8B CE FF D0 83 F8 01", 0, M::At}, {"8B 44 24 0C 8B 54 24 08 56 50 8B 44 24 0C 52 50 E8", 0, M::At}}},
    {Id::RoomSolveCall, K::Sig, W::Text, None, 0, {{"85 C9 74 10 51 8D 54 24 18 D9 1C 24 52 E8", 13, M::At}, {"8B 0C 88 85 C9 74 ?? 51 8D 54 24 ?? D9 1C 24 52 E8", 16, M::At}}},
    {Id::RoomSolve, K::Target, W::Text, Id::RoomSolveCall, 0, {{"56 8B F1 80 BE 80 02 00 00 00 57 75 05 E8 ?? ?? ?? ?? 83 BE 88 00 00 00", 0, M::At}, NOSIG}},
    {Id::RemoteCallJob, K::Sig, W::Text, None, 0, {{"83 7C 24 0C 04 56 57 75 6C 8B 7C 24 0C 33 F6 F6 47 20 01 74 4A", 0, M::At}, {"F6 47 20 01 74 ?? 8B 35 ?? ?? ?? ?? 85 F6 74 ?? 8D 44 24 14 50 57 8B CE C7 44 24 1C 00 00 00 00 E8", -15, M::At}}},
    {Id::RemoteMethodVtable, K::Sig, W::Image, None, 0, {{"89 50 10 8A 54 24 1C 88 48 19 C7 00 ?? ?? ?? ?? 88 50 18", 12, M::Dword}, {"8A 54 24 1C 88 48 19 C7 00 ?? ?? ?? ?? 88 50 18 8B 10", 9, M::Dword}}},
    {Id::RemoteMethodVtable2, K::Sig, W::Image, None, 0, {{"89 50 10 8A 54 24 1C 89 48 14 C7 00 ?? ?? ?? ?? 88 50 18", 12, M::Dword}, {"8A 54 24 1C 89 48 14 C7 00 ?? ?? ?? ?? 88 50 18 8B 10", 9, M::Dword}}},
    // ---- Resource lookup cache (docs/features/performance.md) ----
    {Id::ResRegisterDb, K::Sig, W::Text, None, 0, {{"83 EC 10 53 55 56 57 8B F9 8D 4F 48 68 ?? ?? ?? ?? 89 4C 24 18 E8 ?? ?? ?? ?? 80 7C 24 24 00 C6 44 24 13 00 0F 84", 0, M::At}, {"8D 4F 48 68 ?? ?? ?? ?? 89 4C 24 18 E8 ?? ?? ?? ?? 80 7C 24 24 00 C6 44 24 13 00", -9, M::At}}},
    {Id::ResRegisterDbSlot, K::SlotsOf, W::Image, Id::ResRegisterDb, 1, {NOSIG, NOSIG}},
    {Id::ResRegisterDbDerived, K::Sig, W::Text, None, 0, {{"81 EC 14 02 00 00 80 BC 24 18 02 00 00 00 53 8B 9C 24 20 02 00 00 55 56 57 8B F9 75 ?? 85 DB 74", 0, M::At}, {"80 BC 24 18 02 00 00 00 53 8B 9C 24 20 02 00 00 55 56 57 8B F9", -6, M::At}}},
    {Id::ResRegisterDbDerivedSlot, K::SlotsOf, W::Image, Id::ResRegisterDbDerived, 1, {NOSIG, NOSIG}},
    {Id::ResSetDbPriority, K::Sig, W::Text, None, 0, {{"83 EC 0C 55 56 8B E9 57 8D 4D 48 68 ?? ?? ?? ?? 89 4C 24 18 E8 ?? ?? ?? ?? 8B 75 30 8B 45 34 3B F0 8D 7D 30 0F 84", 0, M::At}, {"8D 4D 48 68 ?? ?? ?? ?? 89 4C 24 18 E8 ?? ?? ?? ?? 8B 75 30 8B 45 34 3B F0 8D 7D 30", -8, M::At}}},
    {Id::ResSetDbPrioritySlot0, K::SlotsOf, W::Image, Id::ResSetDbPriority, 2, {NOSIG, NOSIG}},
    {Id::ResDbChanged, K::Sig, W::Text, None, 0, {{"51 53 8B D9 56 8D 73 48 68 ?? ?? ?? ?? 8B CE 89 74 24 0C E8 ?? ?? ?? ?? 8B 54 24 14 85 D2 0F 84 ?? ?? ?? ?? 83 7B 20 00", 0, M::At}, {"8D 73 48 68 ?? ?? ?? ?? 8B CE 89 74 24 0C E8 ?? ?? ?? ?? 8B 54 24 14 85 D2 0F 84", -5, M::At}}},
    {Id::ResDbChangedSlot0, K::SlotsOf, W::Image, Id::ResDbChanged, 2, {NOSIG, NOSIG}},
    {Id::ShadowedDbVtable, K::Sig, W::Image, None, 0, {{"8D 86 C0 00 00 00 50 C7 06 ?? ?? ?? ?? C7 07 ?? ?? ?? ?? C7 46 0C ?? ?? ?? ?? 89 9E B8 00 00 00", 15, M::Dword}, {"C7 06 ?? ?? ?? ?? C7 07 ?? ?? ?? ?? C7 46 0C ?? ?? ?? ?? 89 9E B8 00 00 00 FF 15", 8, M::Dword}}},
    // ---- Lot lighting while moving (docs/features/performance.md) ----
    {Id::LotLightBudgetCall, K::Sig, W::Text, None, 0, {{"8D 4C 24 10 E8 ?? ?? ?? ?? 8B CE E8 ?? ?? ?? ?? D9 54 24 0C 33 DB 85 ED 7E", 11, M::At}, {"8B CE E8 ?? ?? ?? ?? D9 54 24 0C 33 DB 85 ED 7E ?? 57 8B 4E 28", 2, M::At}}},
    {Id::LotLightBudget, K::Target, W::Text, Id::LotLightBudgetCall, 0, {{"51 A1 ?? ?? ?? ?? 85 C0 56 8B F1 74 ?? 83 B8 ?? ?? 00 00 00 75 ?? D9 05 ?? ?? ?? ?? 5E 59 C3 8B 46 14", 0, M::At}, NOSIG}},
    {Id::CameraRootCall, K::Sig, W::Text, None, 0, {{"E8 ?? ?? ?? ?? 8B C8 E8 ?? ?? ?? ?? 0F 28 40 ?? 8B C8 0F 29 44 24 ?? E8 ?? ?? ?? ?? 83 7E 58 00", 0, M::At}, {"8B C8 E8 ?? ?? ?? ?? 0F 28 40 ?? 8B C8 0F 29 44 24 ?? E8 ?? ?? ?? ?? 83 7E 58 00 0F 28 00", -5, M::At}}},
    {Id::CameraGetterCall, K::Sig, W::Text, None, 0, {{"E8 ?? ?? ?? ?? 8B C8 E8 ?? ?? ?? ?? 0F 28 40 ?? 8B C8 0F 29 44 24 ?? E8 ?? ?? ?? ?? 83 7E 58 00", 7, M::At}, {"8B C8 E8 ?? ?? ?? ?? 0F 28 40 ?? 8B C8 0F 29 44 24 ?? E8 ?? ?? ?? ?? 83 7E 58 00 0F 28 00", 2, M::At}}},
    {Id::CameraRootGetter, K::Target, W::Text, Id::CameraRootCall, 0, {NOSIG, NOSIG}},
    {Id::CameraGetter, K::Target, W::Text, Id::CameraGetterCall, 0, {NOSIG, NOSIG}},
    // ---- Faster cache compression (docs/features/performance.md) ----
    {Id::RefPackDecompress, K::Sig, W::Text, None, 0, {{"3B C2 77 14 8B 44 24 0C 50 56 52 51 E8 ?? ?? ?? ?? 83 C4 10 5E C2 14 00", 12, M::Call}, {"8B 44 24 0C 50 56 52 51 E8 ?? ?? ?? ?? 83 C4 10 5E C2 14 00", 8, M::Call}}},
    // ---- Wall shading while moving (docs/features/performance.md) ----
    {Id::WallAoStep, K::Sig, W::Text, None, 0, {{"83 EC 34 55 56 8B F1 83 7E 04 00 74 14 E8 ?? ?? ?? ?? 8B 4E 04 50 E8 ?? ?? ?? ?? 8B E8 85 ED 75 18 8B 46 04 8A 80 80 02 00 00", 0, M::At}, {"8B 46 04 8A 80 80 02 00 00 F6 D8 5E 5D 1B C0 83 E0 02 83 C4 34 C2 08 00 8B 85 DC 00 00 00 2B 85 D8 00 00 00", -33, M::At}}},
    {Id::WallAoStepSlot, K::SlotsOf, W::Image, Id::WallAoStep, 1, {NOSIG, NOSIG}},
    {Id::WallAoDriver, K::Sig, W::Text, None, 0, {{"56 8B F1 83 7E 14 02 74 27 8B 4E 08 8B 01 8B 50 0C FF D2 84 C0 74 19 D9 44 24 0C 8B 06 8B 50 1C 51 8B 4C 24 0C D9 1C 24 51 8B CE FF D2 89 46 14 5E C2 08 00", 0, M::At}, {"8B 06 8B 50 1C 51 8B 4C 24 0C D9 1C 24 51 8B CE FF D2 89 46 14 5E C2 08 00", -27, M::At}}},
    // ---- File list cache (docs/features/performance.md) ----
    {Id::ResKeyList, K::Sig, W::Text, None, 0, {{"83 EC 10 53 33 C0 38 44 24 20 56 57 89 44 24 0C 0F 84 ?? ?? ?? ?? 8B B1 A0 00 00 00 8B B9 A4 00 00 00 3B F7", 0, M::At}, {"8B 44 24 0C 8B 54 24 08 56 8B 74 24 08 50 52 56 E8 ?? ?? ?? ?? 85 C0 74 0D 85 F6 74 09 56 E8", 16, M::Call}}},
    {Id::ResKeyListSlot, K::SlotsOf, W::Image, Id::ResKeyList, 1, {NOSIG, NOSIG}},
    {Id::ResKeyListDerived, K::Sig, W::Text, None, 0, {{"8B 44 24 0C 8B 54 24 08 56 8B 74 24 08 50 52 56 E8 ?? ?? ?? ?? 85 C0 74 0D 85 F6 74 09 56 E8 ?? ?? ?? ?? 83 C4 04 5E C2 0C 00", 0, M::At}, NOSIG}},
    {Id::ResKeyListDerivedSlot, K::SlotsOf, W::Image, Id::ResKeyListDerived, 1, {NOSIG, NOSIG}},
    {Id::KeyTypeFilterVtable, K::Sig, W::Image, None, 0, {{"57 8D 4C 24 64 51 C7 44 24 68 ?? ?? ?? ?? C7 44 24 6C DA 7D 03 0A 8B 10 8B 52 20", 10, M::Dword}, {"C7 44 24 68 ?? ?? ?? ?? C7 44 24 6C DA 7D 03 0A 8B 10 8B 52 20 8D 4C 24 34 51 8B C8 FF D2", 4, M::Dword}}},
    // ---- Write epochs (docs/features/performance.md): the database classes' vtables from their constructors / destructors ----
    {Id::DpfVtable, K::Sig, W::Image, None, 0, {{"33 DB 3B C3 C7 06 ?? ?? ?? ?? C7 46 04 ?? ?? ?? ?? 88 5E 0C 88 5E 0D 88 5E 0E C6 46 0F 01", 6, M::Dword}, {"C7 06 ?? ?? ?? ?? C7 46 04 ?? ?? ?? ?? E8 ?? ?? ?? ?? 8D 8E 68 03 00 00 E8", 2, M::Dword}}},
    {Id::DpfDerivedVtable, K::Sig, W::Image, None, 0, {{"D9 EE 51 D9 1C 24 C7 06 ?? ?? ?? ?? C7 46 04 ?? ?? ?? ?? 8D BE 98 03 00 00 C7 47 0C", 8, M::Dword}, NOSIG}},
    {Id::DdfVtable, K::Sig, W::Image, None, 0, {{"33 DB 3B C3 C7 06 ?? ?? ?? ?? C7 46 04 ?? ?? ?? ?? 88 5E 0C 75 05 E8 ?? ?? ?? ?? 89 46 10 88 5E 14 89 5E 18", 6, M::Dword}, NOSIG}},
    {Id::PackedStreamVtable, K::Sig, W::Image, None, 0, {{"33 DB 3B C3 C7 06 ?? ?? ?? ?? C7 46 04 ?? ?? ?? ?? 88 5E 0C 75 05 E8 ?? ?? ?? ?? 89 46 10 89 5E 14 C7 46 28", 6, M::Dword}, NOSIG}},
    {Id::DpfWriteDirect, K::Sig, W::Text, None, 0, {{"83 EC 28 53 56 8B F1 8D 8E 70 02 00 00 68 ?? ?? ?? ?? 89 4C 24 10 E8 ?? ?? ?? ?? B3 02 84 5E 14", 0, M::At}, NOSIG}},
    // ---- Scene node budget (docs/features/performance.md): the two per-node calls inside the drain 0x006E4130 ----
    {Id::SceneBoundsCall, K::InRange, W::Text, Id::SceneDrain, 0xD1, {{"8D 44 24 20 50 8B CE E8 ?? ?? ?? ?? 50 8B CE E8", 7, M::At}, {"50 8B CE E8 ?? ?? ?? ?? 50 8B CE E8", 3, M::At}}},
    {Id::SceneNodeBounds, K::Target, W::Text, Id::SceneBoundsCall, 0, {{"55 8B EC 83 E4 F0 81 EC 8C 00 00 00 56 8B F1 8A 46 40 F6 D0 A8 01 75 ?? 8B 46 30 85 C0 74 ?? 83 78 2C 00 74", 0, M::At}, NOSIG}},
    {Id::SceneSpatialCall, K::InRange, W::Text, Id::SceneDrain, 0xD1, {{"50 8B CE E8 ?? ?? ?? ?? 8B 4C 24 1C 01 5F 18", 3, M::At}, {"E8 ?? ?? ?? ?? 8B 4C 24 1C 01 5F 18", 0, M::At}}},
    {Id::SceneNodeSpatial, K::Target, W::Text, Id::SceneSpatialCall, 0, {{"8B 41 30 85 C0 74 14 8B 40 2C 85 C0 74 0D 8B 54 24 04 52 51 8B C8 E8 ?? ?? ?? ?? C2 04 00", 0, M::At}, NOSIG}},
    //      the node lifetime hooks: base destructor (writes the base vtable, then the child list at +0x194), AddNode (owner test at +0x30), holder teardown (spatial tree at +0x2C)
    {Id::SceneNodeDtor, K::Sig, W::Text, None, 0, {{"55 8B EC 83 E4 F0 81 EC 94 00 00 00 53 56 57 8B F9 8D 9F 94 01 00 00 C7 07 ?? ?? ?? ?? 39 5B 04 74", 0, M::At}, {"8D 9F 94 01 00 00 C7 07 ?? ?? ?? ?? 39 5B 04 74 ?? 8D 4C 24 10 E8", -17, M::At}}},
    {Id::SceneAddNode, K::Sig, W::Text, None, 0, {{"56 8B 74 24 08 83 7E 30 00 57 8B F9 0F 85 ?? ?? ?? ?? 53 55 E8", 0, M::At}, {"83 7E 30 00 57 8B F9 0F 85 ?? ?? ?? ?? 53 55 E8", -5, M::At}}},
    {Id::SceneHolderTeardown, K::Sig, W::Text, None, 0, {{"53 55 56 57 8B F9 8B 4F 2C 85 C9 74 ?? E8 ?? ?? ?? ?? 8B 77 2C 85 F6 74", 0, M::At}, {"8B 4F 2C 85 C9 74 ?? E8 ?? ?? ?? ?? 8B 77 2C 85 F6 74 ?? 8B CE E8", -6, M::At}}},
    //      the nodes the budget never defers, by vtable (another module may swap a slot): the Swarm renderer drawable (constructor 0x0071C0A0) and SceneModelFur (constructor 0x006FA020)
    {Id::SwarmDrawableVtable, K::Sig, W::Image, None, 0, {{"81 4E 44 10 00 00 02 C7 06 ?? ?? ?? ?? C7 86 10 02 00 00 00 00 00 00 C7 86 14 02 00 00 00 00 00 00 C7 46 40 01 00 00 00", 9, M::Dword}, NOSIG}},
    {Id::SceneModelFurVtable, K::Sig, W::Image, None, 0, {{"88 86 DC 02 00 00 88 86 DD 02 00 00 C7 06 ?? ?? ?? ?? F3 0F 11 86 D8 02 00 00 8B C6 5E C2 08 00", 14, M::Dword}, NOSIG}},
    // ---- Object lookup index (docs/features/performance.md): the tree walk behind ObjectById ----
    {Id::ObjectTreeWalk, K::Sig, W::Text, None, 0, {{"53 8B 5C 24 08 55 8B 6C 24 10 56 8B F1 8B CB 33 C0 0B CD 74 ?? 8B 96 A0 00 00 00 2B 96 9C 00 00 00 57 33 FF C1 FA 02", 0, M::At}, {"8B 44 24 0C 52 50 E8 ?? ?? ?? ?? 8B F0 85 F6 74 14 8B 16 8B 42 40 8B CE FF D0 83 F8 01", 6, M::Call}}},
    {Id::ObjectTreeSearch, K::Sig, W::Text, None, 0, {{"53 55 56 8B 74 24 10 85 F6 0F 84 ?? ?? ?? ?? 8B 46 48 8B 5C 24 14 3B C3 8B 6C 24 18 75 ?? 8B 4E 4C 3B CD 74", 0, M::At}, {"8B 04 B8 51 55 53 50 E8 ?? ?? ?? ?? 83 C4 10 85 C0 75", 7, M::Call}}},
    // ---- Lot LoD streaming probe (docs/engine/lot-loading-and-streaming.md) ----
    // Find the scoring function by its prologue. Then, only inside that function, locate the byte-global test used by
    // "Throttle Lot LoD Transitions". If the test is not unique the resolver deliberately returns 0 and the probe
    // writes nothing; the [Addr] log contains the matches needed to refine the EA signature.
    {Id::LotLodScoring, K::Sig, W::Text, None, 0, {{"55 8B EC 83 E4 F0 81 EC 84 08 00 00 A1 ?? ?? ?? ?? 53 8B D9 8B 4D 0C 0F 28 8B A0 03 00 00", 0, M::At}, {"55 8B EC 83 E4 F0 81 EC ?? 08 00 00 A1 ?? ?? ?? ?? 53 8B D9 8B 4D 0C", 0, M::At}}},
    {Id::LotDetailRequest, K::Sig, W::Text, None, 0, {{"53 8A 5C 24 08 56 8B F1 8A 86 C1 00 00 00 3A C3 0F 84 ?? ?? ?? ?? 80 BE C9 00 00 00 00", 0, M::At}, NOSIG}},
    {Id::LotLodThrottleTest, K::InRange, W::Text, Id::LotLodScoring, 0x700, {{"80 3D ?? ?? ?? ?? 00", 0, M::At}, NOSIG}},
    {Id::LotLodThrottleFlag, K::Deref, W::Image, Id::LotLodThrottleTest, 2, {NOSIG, NOSIG}},
    // the throttle only applies while Max Active Lots (edx) <= this threshold: mov edx,[edi]; cmp edx,[imm32]; mov byte [esp+1Ah],1
    {Id::LotLodActiveThresholdTest, K::InRange, W::Text, Id::LotLodScoring, 0x700, {{"8B 17 3B 15 ?? ?? ?? ?? C6 44 24 ?? 01", 2, M::At}, NOSIG}},
    {Id::LotLodActiveThreshold, K::Deref, W::Image, Id::LotLodActiveThresholdTest, 2, {NOSIG, NOSIG}},
    // ---- Lot visibility override: camera-view distance bias JZ -> JMP ----
    {Id::LotVisibilityCameraBiasJZ, K::Sig, W::Text, None, 0,
     {{"74 ?? F3 0F 10 44 24 08 F3 0F 5C 87 E0 00 00 00 F3 0F 11 44 24 08 D9 44 24 08 5F 5E 8B E5 5D C2 0C 00", 0, M::At}, NOSIG}},
    // ---- Per-lot object streaming throttle (S3SS LotStreamingOptimizations objectThrottle, frozen 5eb2c65) ----
    {Id::LotAddObjectsToScene, K::Sig, W::Text, None, 0, {{"83 EC 08 57 8B F9 80 BF C9 00 00 00 00 74 0E C6 87 C1 00 00 00 00 5F 83 C4 08 C2 08", 0, M::At}, NOSIG}},
    {Id::LotUpdateObjectSceneNode, K::Sig, W::Text, None, 0, {{"83 EC 0C 83 B9 64 03 00 00 00 89 4C 24 04 0F 84 ?? ?? ?? ?? 83 B9 08 04 00 00 01", 0, M::At}, NOSIG}},
    {Id::ScriptMessageScopeCtor, K::Sig, W::Text, None, 0, {{"8B 44 24 08 56 8B F1 8B 4C 24 10 57 8B 7C 24 0C 85 FF 89 06 89 4E 04 74 19", 0, M::At}, NOSIG}},
    {Id::ScriptMessageScopeDtor, K::Sig, W::Text, None, 0, {{"56 8B F1 83 3E 00 74 1B E8 ?? ?? ?? ?? 85 C0 74 12 8B 4E 04 8B 10 8B 52 18 6A 00 51", 0, M::At}, NOSIG}},
    {Id::PostRemoteMethodCall, K::Sig, W::Text, None, 0, {{"53 56 6A 00 6A 00 6A 00 6A 00 68 ?? ?? ?? ?? 6A 20 E8 ?? ?? ?? ?? 83 C4 18 85 C0 74 ?? C7 00 ?? ?? ?? ?? 33 C9 8D 50 04 87 0A 8B 4C 24 10 8B 54 24 14 89 48 0C 8B 4C 24 18 89 48 14 8A 4C 24 20", 0, M::At}, NOSIG}},
    {Id::IsObjectLargeOrFlora, K::Sig, W::Text, None, 0, {{"8B 0D ?? ?? ?? ?? 56 8B 74 24 08 83 C6 18 56 E8 ?? ?? ?? ?? 85 C0 74 ?? 8B 80 98 00 00 00 8B C8 C1 E9 12 F6 C1 01 75 ?? C1 E8 07 A8 01 75 ??", 0, M::At}, NOSIG}},
    // ---- Local terrain relight (docs/features/night-lighting/terrain-relight.md): the WorldManager global from the store in
    //      FUN_00c6cf80 ("lea ecx,[ebp+9Ch]; mov [global],ebp; call"), alternate: the clear in FUN_00c6b500; the terrain link ----
    {Id::WorldManagerPtr, K::Sig, W::Image, None, 0, {{"8D 8D 9C 00 00 00 89 2D ?? ?? ?? ?? E8", 8, M::Dword}, {"51 53 56 33 DB 8B F1 89 1D ?? ?? ?? ?? 8B 8E 6C 01 00 00", 9, M::Dword}}},
    {Id::TerrainUpdateCall, K::Sig, W::Text, None, 0, {{"8B 44 24 0C 50 8D 4C 24 14 51 8B 4E 58 E8 ?? ?? ?? ?? 80 BE 58 02 00 00 00", 10, M::At}, {"51 8B 4E ?? E8 ?? ?? ?? ?? 80 BE 58 02 00 00 00 75", 1, M::At}}},
    {Id::BakeColourSite, K::Sig, W::Text, None, 0, {{"E8 ?? ?? ?? ?? 0F 28 87 F0 00 00 00 0F 29 86 20 01 00 00 8B 17 0F 28 47 10", 5, M::At}, {"0F 28 87 F0 00 00 00 0F 29 86 20 01 00 00 8B 17", 0, M::At}}},
    {Id::SunlightScale, K::Sig, W::Image, None, 0, {{"B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? F3 0F 10 00 0F 28 8E 00 08 00 00", 1, M::Dword}, {"B9 ?? ?? ?? ?? E8 ?? ?? ?? ?? F3 0F 10 00 0F 28 8E ?? ?? 00 00 8D 8E", 1, M::Dword}}},
    {Id::CasTriSort, K::Sig, W::Text, None, 0, {{"55 8B EC 83 E4 F0 81 EC A4 00 00 00 33 C0 89 44 24 08 89 44 24 0C 53 8D 44 24 0C 8B C8 89 44 24 0C 33 C0 56 57 89 44 24 20 89 44 24 24 89 44 24 28 8D 54 24 20 52 B8 AB AA AA AA F7 65 10", 0, M::At}, NOSIG}},
    // ---- Faster memory handling: operator new's "mov ecx,[global]"; FreeInternal's big-block release (stats update, then
    //      push base; call [VirtualFree]) ----
    {Id::AllocGlobal, K::Sig, W::Image, None, 0, {{"8B 44 24 0C 8B 4C 24 04 50 51 8B 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? C3", 12, M::Dword}, NOSIG}},
    {Id::AllocMmapFreeCall, K::Sig, W::Text, None, 0, {{"29 9F 8C 04 00 00 83 87 88 04 00 00 FF 51 FF 15 ?? ?? ?? ??", 14, M::At}, NOSIG}},
    // ---- Record checksum: the whole function (its table operand masked); the table is the dword at +0x25 ----
    {Id::RecordCrc, K::Sig, W::Text, None, 0, {{"8B 4C 24 04 8B 44 24 08 8D 14 01 3B CA 8B 44 24 0C 73 1F 56 57 0F B6 39 8B F0 C1 EE 18 33 F7 C1 E0 08 33 04 B5 ?? ?? ?? ?? 83 C1 01 3B CA 72 E5 5F 5E 80 7C 24 10 00 74 02 F7 D0 C3", 0, M::At}, NOSIG}},
    {Id::RecordCrcTable, K::Deref, W::Image, Id::RecordCrc, 0x25, {NOSIG, NOSIG}},
    // ---- Frame Profiler: the DDS loader's create and fill calls (pushes of their arguments, the CALL, the stack cleanup) ----
    {Id::TexCreateCall, K::Sig, W::Text, None, 0, {{"8B 45 DC 50 8B 4D F4 51 8B 55 08 52 E8 ?? ?? ?? ?? 83 C4 24", 12, M::At}, NOSIG}},
    {Id::TexCreate, K::Target, W::Text, Id::TexCreateCall, 0, {NOSIG, NOSIG}},
    {Id::TexFillCall, K::Sig, W::Text, None, 0, {{"8A 4D FB 51 8B 55 10 52 8B 45 0C 50 8B 4D 08 51 E8 ?? ?? ?? ?? 83 C4 10", 16, M::At}, NOSIG}},
    {Id::TexFill, K::Target, W::Text, Id::TexFillCall, 0, {NOSIG, NOSIG}},
    {Id::UiServiceGetter, K::Sig, W::Text, None, 0, {{"56 57 E8 ?? ?? ?? ?? 8B F8 E8 ?? ?? ?? ?? 85 C0 74 ?? 8B 10 8B C8 8B 42 04 FF D0 8B F0 85 F6", 9, M::Call}, NOSIG}},
    {Id::RoomNormCall, K::Sig, W::Text, None, 0, {{"DD D8 8D 44 24 50 50 8B CE E8", 9, M::At}, {"8D 44 24 50 50 8B CE E8 ?? ?? ?? ?? F3 0F 10 44 24 18", 7, M::At}}},
    {Id::RoomNorm, K::Target, W::Text, Id::RoomNormCall, 0, {NOSIG, NOSIG}},
    {Id::BasisLightCall, K::Sig, W::Text, None, 0, {{"8D 94 24 C8 00 00 00 52 8B CF E8", 10, M::At}, NOSIG}},
    {Id::BasisLight, K::Target, W::Text, Id::BasisLightCall, 0, {NOSIG, NOSIG}},
    {Id::LampLitCall, K::Sig, W::Text, None, 0, {{"8B CF E8 ?? ?? ?? ?? 8B 44 24 1C 3B 46 1C 74 09 8B 4E 14 50 E8 ?? ?? ?? ?? 8B 4E 1C 51 8B 4E 14 E8", 0, M::At}, NOSIG}},
    {Id::LampMarkCall, K::Sig, W::Text, None, 0, {{"3B 46 1C 74 09 8B 4E 14 50 E8 ?? ?? ?? ?? 8B 4E 1C 51 8B 4E 14 E8", 21, M::At}, NOSIG}},
    {Id::LampMark, K::Target, W::Text, Id::LampMarkCall, 0, {NOSIG, NOSIG}},
    {Id::LotLoadStagesCall, K::Sig, W::Text, None, 0, {{"80 7E 1E 00 75 0D 80 7E 1F 00 75 07 8B CE E8 ?? ?? ?? ?? 8D 8E 70 03 00 00 E8", 14, M::At}, NOSIG}},
    {Id::LotPassRendererCall, K::Sig, W::Text, None, 0, {{"8B 4E 08 85 C9 74 0D D9 44 24 10 51 D9 1C 24 E8", 15, M::At}, NOSIG}},
    {Id::LotLoadStages, K::Target, W::Text, Id::LotLoadStagesCall, 0, {{"81 EC BC 00 00 00 53 56 8B F1 33 DB 38 5E 1E 74", 0, M::At}, NOSIG}},
    {Id::LotBuildBudgetLoad, K::Sig, W::Text, None, 0, {{"C7 44 24 14 14 00 00 00 E8 ?? ?? ?? ?? 3B C3 74 1B 8B 4E 14 8B 56 10 51 52 8B C8 E8 ?? ?? ?? ?? 84 C0 74 08 C7 44 24 14 23 00 00 00 A1 ?? ?? ?? ?? 3B C3 74 10 39 98 B4 01 00 00 75 08 C7 44 24 14 D0 07 00 00", 0x2C, M::At}, NOSIG}},
    {Id::LightObjectRemove, K::Sig, W::Text, None, 0, {{"53 8B 5C 24 08 56 57 8B 7C 24 14 6A 00 6A 01 57 8B F1 53 8D 86 D4 00 00 00 50 E8 ?? ?? ?? ?? 6A 00 6A 01 57 53 8D 8E 90 00 00 00 51 8B CE E8", 0, M::At}, NOSIG}},
    {Id::ProbeDiffuseWrite, K::Sig, W::Text, None, 0, {{"83 EC 28 53 55 56 57 8B F9 80 BF 97 2C 00 00 00 74 ?? 8B B7 58 2C 00 00", 0, M::At}, NOSIG}},
    {Id::ProbeSpecularWrite, K::Sig, W::Text, None, 0, {{"81 EC F4 00 00 00 53 55 56 8B F1 80 BE 97 2C 00 00 00 57 89 74 24 18 74 ?? 8B BE 54 2C 00 00", 0, M::At}, NOSIG}},
    {Id::ObjMapFind, K::Sig, W::Text, None, 0, {{"83 EC 08 56 8B 74 24 14 8B 06 33 D2 57 BF 09 04 00 00 F7 F7 8B 04 91 85 C0 8D 14 91 74 16 8B 3E 8B 76 04 3B 78 08 75 05 3B 70 0C 74 31 8B 40 10 85 C0 75 EF 8D 81 24 10 00 00", 0, M::At}, NOSIG}},
    {Id::ObjMapInsert, K::Sig, W::Text, None, 0, {{"53 55 56 8B 74 24 14 8B 46 08 57 33 D2 BF 09 04 00 00 F7 F7 8B 04 91 8D 3C 91 8B D0 85 D2 74 17 8B 5E 08 8B 6E 0C 3B 5A 08 75 05 3B 6A 0C 74 2A 8B 52 10 85 D2 75 EF 89 46 10", 0, M::At}, NOSIG}},
    {Id::ObjMapErase, K::Sig, W::Text, None, 0, {{"8B 44 24 04 53 8B 5C 24 10 56 57 8B 7C 24 14 8B 57 10 85 D2 89 58 04 89 10 75 13 BE 04 00 00 00 01 70 04 8B 50 04 8B 12 85 D2 89 10 74 F2 8B 33 3B F7 8B 56 10 75 0F 5F 89 13 83 81 28 10 00 00 FF 5E 5B C2 0C 00", 0, M::At}, NOSIG}},
    {Id::RenMapFind, K::Sig, W::Text, None, 0, {{"83 EC 08 56 8B 74 24 14 8B 06 33 D2 57 BF 09 04 00 00 F7 F7 8B 04 91 85 C0 8D 14 91 74 15 8B 3E 8B 76 04 3B 78 08 75 05 3B 70 0C 74 30 8B 00 85 C0 75 F0 8D 81 24 10 00 00", 0, M::At}, NOSIG}},
    {Id::RenMapInsert, K::Sig, W::Text, None, 0, {{"53 55 56 8B 74 24 14 8B 46 08 57 33 D2 BF 09 04 00 00 F7 F7 8B 04 91 8D 3C 91 8B D0 85 D2 74 16 8B 5E 08 8B 6E 0C 3B 5A 08 75 05 3B 6A 0C 74 28 8B 12 85 D2 75 F0 89 06", 0, M::At}, NOSIG}},
    {Id::RenMapErase, K::Sig, W::Text, None, 0, {{"8B 44 24 04 53 8B 5C 24 10 56 57 8B 7C 24 14 8B 17 85 D2 89 58 04 89 10 75 14 BE 04 00 00 00 90 01 70 04 8B 50 04 8B 12 85 D2 89 10 74 F2 8B 33 3B F7 8B 16 75 0F 5F 89 13 83 81 28 10 00 00 FF 5E 5B C2 0C 00", 0, M::At}, NOSIG}},
    {Id::TreeSetLevel, K::Sig, W::Text, None, 0, {{"8B 44 24 04 56 50 8B F1 E8 ?? ?? ?? ?? 8B 86 3C 05 00 00 85 C0 C6 86 36 05 00 00 01 74 0A 89 86 F0 01 00 00 5E C2 04 00 33 C9 39 8E B8 01 00 00 0F 9D C1 83 E9 01 81 E1 8C 1E D1 08", 0, M::At}, NOSIG}},
    {Id::TreeSetCustomRig, K::Sig, W::Text, None, 0, {{"8B 44 24 04 56 8B F1 39 86 3C 05 00 00 74 33 89 86 3C 05 00 00 E8 ?? ?? ?? ?? 8B 86 3C 05 00 00 85 C0 C6 86 36 05 00 00 01 75 11 39 86 B8 01 00 00 0F 9D C0 83 E8 01 25 8C 1E D1 08", 0, M::At}, NOSIG}},
    {Id::CompQueueCall, K::Sig, W::Text, None, 0, {{"84 C0 75 1F 83 C6 04 56 8B CF E8 ?? ?? ?? ?? 83 F8 02 74 ?? 83 F8 03 74", 10, M::At}, NOSIG}},
    {Id::CompDispatch, K::Target, W::Text, Id::CompQueueCall, 0, {{"56 8B F1 83 7E 74 08 75 09 B8 01 00 00 00 5E C2 04 00 8B 0D", 0, M::At}, NOSIG}},
    {Id::CompState2Call, K::Sig, W::Text, None, 0, {{"55 8B CE E8 ?? ?? ?? ?? 8B F8 C7 46 74 03 00 00 00", 3, M::At}, NOSIG}},
    {Id::CompTileRender, K::Target, W::Text, Id::CompState2Call, 0, {{"83 EC 30 57 8B F9 83 7F 1C 00 75 0C B8 04 00 00 00 5F 83 C4 30 C2 04 00", 0, M::At}, NOSIG}},
    {Id::CompState3Call, K::Sig, W::Text, None, 0, {{"55 8B CE E8 ?? ?? ?? ?? 8B F8 C7 46 74 04 00 00 00", 3, M::At}, NOSIG}},
    {Id::CompTileRead, K::Target, W::Text, Id::CompState3Call, 0, {{"55 8B EC 83 E4 F0 83 EC 34 53 56 8B 75 08 83 7E 08 00 57 89 4C 24 18 75 0E", 0, M::At}, NOSIG}},
    {Id::CompReadbackCall, K::Sig, W::Text, None, 0, {{"8B 83 84 00 00 00 50 8B CB E8 ?? ?? ?? ?? 85 C0 75 ?? 8B 4B 20 50 50 68", 9, M::At}, NOSIG}},
    {Id::CompReadback, K::Target, W::Text, Id::CompReadbackCall, 0, {{"83 EC 28 53 8B D9 83 7B 20 00 75 09 33 C0 5B 83 C4 28 C2 04 00 56 57 E8", 0, M::At}, NOSIG}},
    {Id::CompTileFetch, K::Sig, W::Text, None, 0, {{"83 EC 28 53 8B D9 8B 43 1C 55 8B 6C 24 34 89 45 08 8B 43 70 8B 53 60", 0, M::At}, NOSIG}},
    {Id::MonoDomainFree, K::Sig, W::Text, None, 0, {{"55 56 8B 74 24 0C 33 ED 3B 35 ?? ?? ?? ?? 75 16 39 6C 24 10 75 10 68 ?? ?? ?? ?? E8 ?? ?? ?? ?? 83 C4 04 5E 5D C3 68", 0, M::At}, NOSIG}},
    {Id::MonoTypeGetObject, K::Sig, W::Text, None, 0, {{"53 8B 5C 24 0C 55 56 57 53 E8 ?? ?? ?? ?? 8B 74 24 18 83 C4 04 56 8B E8 FF 15 ?? ?? ?? ?? 83 7E 40 00 75 17 6A 02 68", 0, M::At}, NOSIG}},
    {Id::WindowRepaintJump, K::Sig, W::Text, None, 0, {{"F6 46 08 08 57 74 ?? 39 5E 20 C6 46 1C 01 75 ?? 8B 46 70 53 53 50 FF 15", 14, M::At}, NOSIG}},
    // ---- Room to save: the update thunk (mov eax,[ecx+14h]; push eax; add ecx,18h; call; ret 8), the shrink of both caches,
    //      the world save call before "mov eax,0Ch" (Error 12); every signature unique on Steam (checked on TS3W.exe 05/10) ----
    {Id::ResUpdateCall, K::Sig, W::Text, None, 0, {{"8B 41 14 50 83 C1 18 E8 ?? ?? ?? ?? C2 08 00", 7, M::At}, {"83 C1 18 E8 ?? ?? ?? ?? C2 08 00", 3, M::At}}},
    {Id::ResUpdate, K::Target, W::Text, Id::ResUpdateCall, 0, {NOSIG, NOSIG}},
    {Id::ResShrinkBoth, K::Sig, W::Text, None, 0, {{"56 8B F1 8B 8E E0 01 00 00 85 C9 74 ?? 8B 81 8C 00 00 00 6A 01 6A 00 50 E8", 0, M::At},
                                                   {"8B 8E E4 01 00 00 85 C9 5E 74 ?? 8B 91 8C 00 00 00 6A 01 6A 00 52 E8", -0x1D, M::At}}},
    {Id::WorldSaveCall, K::Sig, W::Text, None, 0, {{"51 8B 4C 24 30 E8 ?? ?? ?? ?? 84 C0 75 ?? 8D 4C 24 10 E8 ?? ?? ?? ?? 5F 5E B8 0C 00 00 00", 5, M::At},
                                                   {"E8 ?? ?? ?? ?? 84 C0 75 ?? 8D 4C 24 ?? E8 ?? ?? ?? ?? 5F 5E B8 0C 00 00 00", 0, M::At}}},
    {Id::WorldSave, K::Target, W::Text, Id::WorldSaveCall, 0, {NOSIG, NOSIG}},
    // ---- Formerly Steam-only addresses (2.13.2 EA port). Globals come from the instruction that reads them (M::Dword: several
    //      matches are accepted when they all name the same global) ----
    {Id::LampK1, K::Sig, W::Image, None, 0, {{"0F 52 C0 B9 ?? ?? ?? ?? 0F 29 44 24 20 E8", 4, M::Dword}, NOSIG}},
    {Id::LampK2, K::Sig, W::Image, None, 0, {{"B9 ?? ?? ?? ?? F3 0F 11 44 24 0C E8 ?? ?? ?? ?? F3 0F 10 44 24 0C F3 0F 59 00", 1, M::Dword}, NOSIG}},
    {Id::LampCmax, K::Sig, W::Image, None, 0, {{"F3 0F 11 44 24 30 0F 28 05 ?? ?? ?? ?? F3 0F 11 44 24 20", 9, M::Dword}, NOSIG}},
    {Id::StreetLampScale, K::Sig, W::Image, None, 0, {{"0F 59 C8 F3 0F 10 05 ?? ?? ?? ?? 0F C6 C0 00 0F 59 C8 0F 29 0A", 7, M::Dword}, NOSIG}},
    {Id::ConeScale5, K::Sig, W::Image, None, 0, {{"0F 28 89 A0 01 00 00 F3 0F 10 05 ?? ?? ?? ?? 0F 28 35 ?? ?? ?? ?? 0F 59 CB", 18, M::Dword}, NOSIG}},
    {Id::RigCutFlagged, K::Sig, W::Image, None, 0, {{"83 7E 08 00 74 0A F3 0F 10 05 ?? ?? ?? ?? EB 08 F3 0F 10 05 ?? ?? ?? ?? 0F 2F 44 24 38", 10, M::Dword}, NOSIG}},
    {Id::RigCut, K::Sig, W::Image, None, 0, {{"83 7E 08 00 74 0A F3 0F 10 05 ?? ?? ?? ?? EB 08 F3 0F 10 05 ?? ?? ?? ?? 0F 2F 44 24 38", 20, M::Dword}, NOSIG}},
    {Id::SolveTimeByClass, K::Sig, W::Image, None, 0, {{"89 35 ?? ?? ?? ?? 89 35 ?? ?? ?? ?? A3 ?? ?? ?? ?? A3 ?? ?? ?? ?? A3 ?? ?? ?? ?? E8", 13, M::Dword}, NOSIG}},
    {Id::MapSlots, K::Sig, W::Image, None, 0, {{"8B 44 24 04 C1 E0 04 05 ?? ?? ?? ?? C3 CC CC CC 8B 44 24 04 A3 ?? ?? ?? ?? C3 CC CC CC CC CC CC 8B 44 24 04 A3 ?? ?? ?? ?? C3", 8, M::Dword}, NOSIG}},
    {Id::MapLocker, K::Sig, W::Image, None, 0, {{"8B 44 24 04 C1 E0 04 05 ?? ?? ?? ?? C3 CC CC CC 8B 44 24 04 A3 ?? ?? ?? ?? C3 CC CC CC CC CC CC 8B 44 24 04 A3 ?? ?? ?? ?? C3", 0x25, M::Dword},
                                                {"8B 44 24 04 A3 ?? ?? ?? ?? C3 CC CC CC CC CC CC 8B 44 24 04 A3 ?? ?? ?? ?? C3", 0x15, M::Dword}}},
    {Id::Finalize, K::Sig, W::Text, None, 0, {{"56 8B F1 57 8B BE F4 00 00 00 C1 E7 06 03 7E 04 8B CF E8", 0, M::At}, NOSIG}},
    {Id::FinalizeCall, K::Sig, W::Text, None, 0, {{"8B CE DD D8 E8 ?? ?? ?? ?? E9 ?? ?? ?? ?? 8B CF DD D8", 4, M::At}, NOSIG}},
    {Id::MapEdges, K::InRange, W::Text, Id::Finalize, 0x40, {{"8B CF E8 ?? ?? ?? ?? 84 C0 74", 2, M::Call}, NOSIG}},
    {Id::EdgeCopy, K::InRange, W::Text, Id::Finalize, 0x40, {{"57 50 8B CE E8", 4, M::Call}, NOSIG}},
    {Id::MapUnlock, K::InRange, W::Text, Id::Finalize, 0x60, {{"6A 01 8B CE E8", 4, M::Call}, NOSIG}},
    {Id::RoomLitNote, K::InRange, W::Text, Id::Finalize, 0x60, {{"8B CE E8 ?? ?? ?? ?? 8B 0E 85 C9", 2, M::Call}, NOSIG}},
    {Id::ThresholdFn, K::Sig, W::Text, None, 0, {{"8B 41 08 D9 04 85 ?? ?? ?? ?? D8 0D ?? ?? ?? ?? C3", 0, M::At}, NOSIG}},
    {Id::ThresholdFactor, K::Deref, W::Image, Id::ThresholdFn, 6, {NOSIG, NOSIG}},
    {Id::ThresholdScale, K::Deref, W::Image, Id::ThresholdFn, 12, {NOSIG, NOSIG}},
    {Id::ClassWallModeFn, K::Sig, W::Text, None, 0, {{"8B 41 08 8A 80 ?? ?? ?? ?? C3 CC CC CC CC CC CC 8B 41 08 33 C9 38 88", 0, M::At}, NOSIG}},
    {Id::ClassWallMode, K::Deref, W::Image, Id::ClassWallModeFn, 5, {NOSIG, NOSIG}},
    {Id::ClassWallsFn, K::Multi, W::Text, None, 2, {{"8B 44 24 04 8B 49 08 8D 04 40 8A 84 08 ?? ?? ?? ?? C2 04 00", 0, M::At}, NOSIG}},
    {Id::ClassWalls2D, K::Deref, W::Image, Id::ClassWallsFn, 13, {NOSIG, NOSIG}},
    {Id::ClassObjects3D, K::Deref, W::Image, Id::ClassObjectsFn, 13, {NOSIG, NOSIG}},
    {Id::TexelsGetter, K::Sig, W::Text, None, 0, {{"8B 41 08 8B 04 85 ?? ?? ?? ?? C3", 0, M::At}, NOSIG}},
    {Id::TexelsPerTile, K::Deref, W::Image, Id::TexelsGetter, 6, {NOSIG, NOSIG}},
    {Id::AtlasReads, K::Sig, W::Text, None, 0, {{"03 C0 8B BC 00 ?? ?? ?? ?? 8B 9C 00 ?? ?? ?? ??", 2, M::At}, NOSIG}},
    {Id::WallAtlasW, K::Deref, W::Image, Id::AtlasReads, 3, {NOSIG, NOSIG}},
    {Id::WallAtlasH, K::Deref, W::Image, Id::AtlasReads, 10, {NOSIG, NOSIG}},
    {Id::OriginPush, K::Sig, W::Text, None, 0, {{"8B 4C 24 14 6A 04 51 8D 96 00 02 00 00 52 8B CF E8", 0, M::At}, NOSIG}},
    {Id::CopyPush, K::Sig, W::Text, None, 0, {{"89 41 74 6A 04 F3 0F 11 41 78 88 81 CC 00 00 00 E8", 0, M::At}, NOSIG}},
    {Id::CopyBuild, K::Sig, W::Text, None, 0, {{"89 41 74 6A 04 F3 0F 11 41 78 88 81 CC 00 00 00 E8", 16, M::Call}, {"83 EC 24 53 33 DB 38 1D ?? ?? ?? ?? 0F 85", 0, M::At}}},
    {Id::CopyBuilt, K::Deref, W::Image, Id::CopyBuild, 8, {NOSIG, NOSIG}},
    {Id::WallUvCall0, K::Sig, W::Text, None, 0, {{"57 8B CE E8 ?? ?? ?? ?? 0F BE 4C 24 0D 0F BE 54 24 0C", 3, M::At}, NOSIG}},
    {Id::WallUvCall1, K::InRange, W::Text, Id::WallUvCall0, 0x30, {{"57 8B CE E8 ?? ?? ?? ?? 5D 5F", 3, M::At}, NOSIG}},
    {Id::WallUv, K::Target, W::Text, Id::WallUvCall0, 0, {NOSIG, NOSIG}},
    {Id::WallEdgeFn, K::Sig, W::Text, None, 0, {{"8B 4C 24 04 83 C1 FF 33 C0 83 F9 22 77", 0, M::At}, NOSIG}},
    {Id::CutPostSite0, K::Multi, W::Text, None, 2, {{"8B 16 50 8B 42 1C 68 B4 7E 25 06 8B CE FF D0", 6, M::At}, NOSIG}},
    {Id::OutdoorAlphaSite, K::InRange, W::Text, Id::WallSolve, 0x400, {{"E8 ?? ?? ?? ?? 80 7B 18 00 0F 57 C9 0F 85", 5, M::At}, NOSIG}},
    {Id::WallBaseSite, K::Sig, W::Text, None, 0, {{"E8 ?? ?? ?? ?? 84 C0 74 07 F3 0F 10 45 10 EB 12 8B 0E 53 57 E8 ?? ?? ?? ?? 85 C0 74 0B F3 0F 10 40 78", 9, M::At}, NOSIG}},
    {Id::IndoorBaseSite, K::Sig, W::Text, None, 0, {{"E8 ?? ?? ?? ?? 84 C0 74 07 F3 0F 10 45 10 EB 12 8B 0E 53 57 E8 ?? ?? ?? ?? 85 C0 74 0B F3 0F 10 40 78", 29, M::At}, NOSIG}},
    {Id::TexelPackCall, K::InRange, W::Text, Id::WallSolve, 0x400,
     {{"0F 5D 44 24 70 8D 84 24 90 00 00 00 50 8D 4C 24 58 0F 29 84 24 94 00 00 00 E8 ?? ?? ?? ?? 8B 4C 24 54 83 44 24 0C 30 83 6C 24 18 01 89 0F", 25, M::At}, NOSIG}},
    {Id::TexelPack, K::Target, W::Text, Id::TexelPackCall, 0, {NOSIG, NOSIG}},
    {Id::TexelAddrSite, K::InRange, W::Text, Id::WallSolve, 0x400, {{"8B 45 0C 0F B7 56 22 0F AF 50 04 0F B7 4E 20 8D 3C 8A 8B 55 10 03 38", 0, M::At}, NOSIG}},
    {Id::PackConst255, K::Deref, W::Image, Id::TexelPack, 0x1A, {NOSIG, NOSIG}},
    {Id::PackMagic, K::Deref, W::Image, Id::TexelPack, 0x5E, {NOSIG, NOSIG}},
    {Id::OccMarkCall, K::Sig, W::Text, None, 0, {{"8B 4E 14 50 89 46 1C E8", 7, M::At}, NOSIG}},
    {Id::EntryFlag, K::LowestOf2, W::Text, None, 0, {{"8B 44 24 08 8B 54 24 04 50 52 68 ?? ?? ?? ?? E8 ?? ?? ?? ?? C2 08 00", 0, M::At}, NOSIG}},
    {Id::EntryFlagCall0, K::CallersOf, W::Text, Id::EntryFlag, 5, {NOSIG, NOSIG}},
};
// clang-format on
#undef NOSIG

// Feature groups: the addresses a feature needs before it can be switched on (its optional parts check their own ids).
struct Group {
    const char* name;
    std::vector<Id> ids;
};
const Group kGroups[] = {
    {"NightLights", {Id::RootGetter, Id::RootPtr, Id::QueueRoom, Id::TerrainVisitorSite, Id::ArmSiteRemoval, Id::ArmSiteRegister, Id::ArmSiteMoved}},
    {"SplitLevel", {Id::GetLotIdGatherCall, Id::GetLotId}},
    {"ProbeNeutral", {Id::ProbeDiffuseWrite, Id::ProbeSpecularWrite}},
    {"ResourceCache", {Id::ResFindProvider, Id::ResFindProviderSlot0, Id::ResFindProviderSlot1, Id::ResRegisterDb, Id::ResRegisterDbSlot, Id::ResRegisterDbDerived,
                       Id::ResRegisterDbDerivedSlot, Id::ResSetDbPriority, Id::ResSetDbPrioritySlot0, Id::ResSetDbPrioritySlot1, Id::ResDbChanged, Id::ResDbChangedSlot0,
                       Id::ResDbChangedSlot1, Id::ShadowedDbVtable}},
    {"LotLightingMotion", {Id::LotLightBudgetCall, Id::LotLightBudget, Id::CameraRootCall, Id::CameraGetterCall, Id::CameraRootGetter, Id::CameraGetter}},
    {"FastTextureCompression", {Id::DxtEncode1, Id::DxtEncode5}},
    {"FastCacheCompression", {Id::RefPackCompress, Id::RefPackCompressSlot}},
    {"WallShadingWhileMoving", {Id::WallAoStep, Id::WallAoStepSlot, Id::WallAoDriver, Id::CameraRootCall, Id::CameraGetterCall, Id::CameraRootGetter, Id::CameraGetter}},
    {"FileListCache", {Id::ResKeyList, Id::ResKeyListSlot, Id::ResKeyListDerived, Id::ResKeyListDerivedSlot, Id::KeyTypeFilterVtable, Id::ResRegisterDb, Id::ResRegisterDbSlot,
                       Id::ResRegisterDbDerived, Id::ResRegisterDbDerivedSlot, Id::ResSetDbPriority, Id::ResSetDbPrioritySlot0, Id::ResSetDbPrioritySlot1, Id::ResDbChanged,
                       Id::ResDbChangedSlot0, Id::ResDbChangedSlot1, Id::ShadowedDbVtable}},
    {"SceneNodeBudget", {Id::SceneDrainCall, Id::SceneDrain, Id::SceneBoundsCall, Id::SceneNodeBounds, Id::SceneSpatialCall, Id::SceneNodeSpatial, Id::SceneNodeDtor,
                         Id::SceneAddNode, Id::SceneHolderTeardown, Id::SwarmDrawableVtable, Id::SceneModelFurVtable, Id::CameraRootCall, Id::CameraGetterCall, Id::CameraRootGetter, Id::CameraGetter}},
    {"ObjectIndex", {Id::ObjectById, Id::ObjectTreeWalk, Id::ObjectTreeSearch}},
    {"RoomLightQueue", {Id::RoomPriorityCall, Id::RoomPriority, Id::LodStepSite, Id::KeepClassA, Id::KeepClassB, Id::RoomPickJump, Id::RoomPick, Id::RoomSolveStep,
                        Id::StopwatchCtor, Id::StopwatchStart, Id::StopwatchElapsed, Id::PriorityLotObject, Id::PriorityLotTest}},
    {"LotLodStreaming", {Id::LotLodScoring, Id::LotLodThrottleTest, Id::LotLodThrottleFlag, Id::WorldManagerPtr}},
    {"LotLodActiveFollow", {Id::LotLodActiveThresholdTest, Id::LotLodActiveThreshold}}, // optional part of LotLodStreaming
    {"LotBuildShare", {Id::LotLoadStagesCall, Id::LotLoadStages, Id::LotPassRendererCall, Id::WorldManagerPtr, Id::PriorityLotObject, Id::PriorityLotTest}},
    {"LotLodDistanceProbe", {Id::LotLodScoring, Id::LotDetailRequest}},
    {"LotObjectThrottle", {Id::LotAddObjectsToScene, Id::LotUpdateObjectSceneNode, Id::ScriptMessageScopeCtor, Id::ScriptMessageScopeDtor,
                            Id::PostRemoteMethodCall, Id::IsObjectLargeOrFlora}},
    {"LotVisibilityOverride", {Id::LotVisibilityCameraBiasJZ}},
    {"FastCasSort", {Id::CasTriSort}},
    {"FastMemory", {Id::AllocGlobal, Id::AllocMmapFreeCall}},
    {"FastRecordCrc", {Id::RecordCrc, Id::RecordCrcTable}},
    {"RoomLightQueue", {Id::PriorityLotObject, Id::PriorityLotTest}}, // each further part checks its own ids and bytes
    {"ScriptTypeCache", {Id::MonoTypeGetObject, Id::MonoDomainFree}},
    {"ObjectIdMap", {Id::ObjMapFind, Id::ObjMapInsert, Id::ObjMapErase}},
    {"RenderObjectMap", {Id::RenMapFind, Id::RenMapInsert, Id::RenMapErase}},
    {"TreeLightRig", {Id::TreeSetLevel, Id::TreeSetCustomRig}},
    {"CompositorReadback", {Id::CompQueueCall, Id::CompDispatch, Id::CompState2Call, Id::CompTileRender, Id::CompState3Call, Id::CompTileRead, Id::CompReadbackCall,
                            Id::CompReadback, Id::CompTileFetch}},
    {"WindowRepaint", {Id::WindowRepaintJump}},
    {"MemoryGuard", {Id::ResUpdateCall, Id::ResUpdate, Id::ResShrinkBoth, Id::WorldSaveCall, Id::WorldSave}},
};

// ---------------------------------------------------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------------------------------------------------
uintptr_t g_found[static_cast<size_t>(Id::Count)] = {}; // what the signatures gave (every build)
uintptr_t g_final[static_cast<size_t>(Id::Count)] = {}; // what Get returns on non-Steam builds
std::atomic<bool> g_resolved{false};
bool g_resolveRan = false;

struct Range {
    uintptr_t begin = 0, end = 0;
    bool Has(uintptr_t a, size_t n = 1) const { return a >= begin && a + n <= end && a + n >= a; }
};
Range g_text, g_image;
std::vector<Range> g_readable; // readable parts of .text (normally the whole section)
std::vector<Range> g_rodata;   // readable parts of the read-only data sections (.rdata, .rsrc): vtables (K::SlotsOf)

size_t Index(Id id) { return static_cast<size_t>(id); }

// ---- guarded memory access (SEH only, no C++ objects inside __try) ----
bool SafeRead(uintptr_t a, void* out, size_t n) {
    __try {
        std::memcpy(out, reinterpret_cast<const void*>(a), n);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool ReadU32(uintptr_t a, uint32_t& v) { return SafeRead(a, &v, 4); }

// Call target of an E8 rel32 at a, or 0
uintptr_t CallTargetAt(uintptr_t a) {
    uint8_t b[5];
    if (!g_text.Has(a, 5) || !SafeRead(a, b, 5) || b[0] != 0xE8) return 0;
    int32_t rel;
    std::memcpy(&rel, b + 1, 4);
    return a + 5 + static_cast<intptr_t>(rel);
}

// ---- patterns ----
struct Pattern {
    std::vector<uint8_t> bytes, mask;
    size_t anchor = 0; // first fixed byte (the one memchr looks for)
};

int Hex(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool ParsePattern(const char* s, Pattern& p) {
    p = {};
    for (const char* c = s; c && *c;) {
        if (*c == ' ') {
            ++c;
            continue;
        }
        if (*c == '?') {
            p.bytes.push_back(0);
            p.mask.push_back(0);
            ++c;
            if (*c == '?') ++c;
            continue;
        }
        const int hi = Hex(c[0]), lo = c[1] ? Hex(c[1]) : -1;
        if (hi < 0 || lo < 0) return false;
        p.bytes.push_back(static_cast<uint8_t>(hi * 16 + lo));
        p.mask.push_back(1);
        c += 2;
    }
    size_t a = 0;
    while (a < p.mask.size() && !p.mask[a]) a++;
    if (a == p.mask.size()) return false; // wildcards only
    p.anchor = a;
    return true;
}

// Match starts in [from, to) inside one readable range that ends at `limit` (a match may run past `to`, never past
// `limit`). Appends to out[found..maxOut) and returns the new total count (which can exceed maxOut). SEH only.
size_t ScanRaw(uintptr_t from, uintptr_t to, uintptr_t limit, const uint8_t* bytes, const uint8_t* mask, size_t n, size_t anchor, uintptr_t* out, size_t maxOut,
               size_t found) {
    __try {
        const uint8_t first = bytes[anchor];
        const uintptr_t searchEnd = to + anchor < limit ? to + anchor : limit;
        uintptr_t p = from + anchor;
        while (p < searchEnd) {
            const void* hit = std::memchr(reinterpret_cast<const void*>(p), first, searchEnd - p);
            if (!hit) break;
            const uintptr_t start = reinterpret_cast<uintptr_t>(hit) - anchor;
            if (start >= to || start + n > limit) break;
            const uint8_t* at = reinterpret_cast<const uint8_t*>(start);
            size_t k = 0;
            while (k < n && (!mask[k] || at[k] == bytes[k])) k++;
            if (k == n) {
                if (found < maxOut) out[found] = start;
                found++;
            }
            p = reinterpret_cast<uintptr_t>(hit) + 1;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return found;
}

// Every match in [from, to) of .text: the count is exact, the first 64 are recorded (address order)
size_t FindAll(const Pattern& pat, uintptr_t from, uintptr_t to, std::vector<uintptr_t>& out) {
    constexpr size_t kMax = 64;
    uintptr_t buf[kMax];
    size_t found = 0;
    for (const Range& r : g_readable) {
        const uintptr_t b = r.begin > from ? r.begin : from, e = r.end < to ? r.end : to;
        if (b >= e) continue;
        found = ScanRaw(b, e, r.end, pat.bytes.data(), pat.mask.data(), pat.bytes.size(), pat.anchor, buf, kMax, found);
    }
    out.assign(buf, buf + (found < kMax ? found : kMax));
    return found;
}

// CALL rel32 sites in [from, to) landing on target (a CALL may not run past `limit`). SEH only.
size_t ScanCalls(uintptr_t from, uintptr_t to, uintptr_t limit, uintptr_t target, uintptr_t* out, size_t maxOut, size_t found) {
    __try {
        for (uintptr_t p = from; p < to && p + 5 <= limit; p++) {
            if (*reinterpret_cast<const uint8_t*>(p) != 0xE8) continue;
            const int32_t rel = *reinterpret_cast<const int32_t*>(p + 1);
            if (p + 5 + static_cast<intptr_t>(rel) != target) continue;
            if (found < maxOut) out[found] = p;
            found++;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return found;
}

std::string Hex16(uintptr_t at) {
    uint8_t b[16];
    const uintptr_t from = at >= 4 ? at - 4 : at;
    if (!SafeRead(from, b, sizeof b)) return "(unreadable)";
    std::string s;
    for (int i = 0; i < 16; i++) s += std::format("{}{:02X}", from + i == at ? " | " : (i ? " " : ""), b[i]);
    return s;
}

bool IsFixedBuild() { return g_gameVersion == GameVersion::Steam; }

// ---- logging of one attempt ----
void LogMatches(const char* name, const char* which, size_t count, const std::vector<uintptr_t>& hits, uintptr_t result, Id id) {
    std::string at;
    for (size_t i = 0; i < hits.size() && i < 8; i++) at += std::format("{}{:#010x}", i ? ", " : "", hits[i]);
    if (hits.size() > 8) at += ", ...";
    const uint32_t steam = kInfo[Index(id)].steam;
    const bool steamOk = !IsFixedBuild() || result == steam;
    LOG_INFO(std::format("[Addr] {}{}: {} match{}{}{} -> {} (Steam {:#010x}){}", name, which, count, count == 1 ? "" : "es", count ? " at " : "", at,
                         result ? std::format("{:#010x}", result) : std::string("not found"), steam, IsFixedBuild() ? (steamOk ? " ok" : " DIFFERS: the fixed address is kept") : ""));
    // 16 bytes around each match (4 before, the match start after "|"): non-Steam builds, or a Steam self-check that failed
    if (!IsFixedBuild() || !steamOk)
        for (size_t i = 0; i < hits.size() && i < 3; i++) LOG_INFO(std::format("[Addr]   {:#010x}: {}", hits[i], Hex16(hits[i])));
}

// Value a signature gives for one match
uintptr_t Apply(const Sig& s, uintptr_t match) {
    const uintptr_t a = match + static_cast<intptr_t>(s.offset);
    switch (s.mode) {
    case M::At:
        return a;
    case M::Call:
        return CallTargetAt(a);
    case M::Dword: {
        uint32_t v = 0;
        return ReadU32(a, v) ? v : 0;
    }
    }
    return 0;
}

bool InWhere(W w, uintptr_t a) { return a && (w == W::Text ? g_text.Has(a) : g_image.Has(a)); }

// One signature over [from, to): the address when it matches once (or every match agrees on the value), else 0
uintptr_t TrySig(const Entry& e, int which, uintptr_t from, uintptr_t to) {
    const Sig& s = e.sig[which];
    if (!s.pattern) return 0;
    const char* tag = which == 0 ? "" : " (alternate)";
    Pattern pat;
    if (!ParsePattern(s.pattern, pat)) {
        LOG_ERROR(std::format("[Addr] {}{}: bad pattern", Name(e.id), tag));
        return 0;
    }
    std::vector<uintptr_t> hits;
    const size_t count = FindAll(pat, from, to, hits);
    uintptr_t result = 0;
    if (count == 1) result = Apply(s, hits[0]);
    else if (count > 1 && count == hits.size() && s.mode != M::At) { // every match must give the same value
        result = Apply(s, hits[0]);
        for (uintptr_t h : hits)
            if (Apply(s, h) != result) result = 0;
    }
    if (e.kind == K::LowestOf2) result = count == 2 ? Apply(s, hits[0] < hits[1] ? hits[0] : hits[1]) : 0;
    if (!InWhere(e.where, result)) result = 0;
    LogMatches(Name(e.id), tag, count, hits, result, e.id);
    return result;
}

// Multi: exactly arg matches; false when the count differs
bool TryMulti(const Entry& e, int which) {
    const Sig& s = e.sig[which];
    if (!s.pattern) return false;
    Pattern pat;
    if (!ParsePattern(s.pattern, pat)) return false;
    std::vector<uintptr_t> hits;
    const size_t count = FindAll(pat, g_text.begin, g_text.end, hits);
    const bool ok = count == static_cast<size_t>(e.arg) && hits.size() == count;
    if (ok) {
        for (size_t i = 0; i < count; i++) {
            const uintptr_t a = Apply(s, hits[i]); // hits are in address order
            g_found[Index(e.id) + i] = InWhere(e.where, a) ? a : 0;
        }
    }
    LogMatches(Name(e.id), which ? " (alternate)" : "", count, hits, ok ? g_found[Index(e.id)] : 0, e.id);
    if (!ok) LOG_INFO(std::format("[Addr]   {} expects exactly {} matches", Name(e.id), e.arg));
    for (int i = 1; ok && i < e.arg; i++)
        LOG_INFO(std::format("[Addr] {}: {:#010x} (Steam {:#010x})", Name(static_cast<Id>(Index(e.id) + i)), g_found[Index(e.id) + i], kInfo[Index(e.id) + i].steam));
    return ok;
}

// Every CALL in .text landing on target (address order; up to 64 recorded, the count is exact)
size_t CallersOf(uintptr_t target, std::vector<uintptr_t>& sites) {
    constexpr size_t kMax = 64;
    uintptr_t buf[kMax];
    size_t found = 0;
    for (const Range& r : g_readable) found = ScanCalls(r.begin, r.end, r.end, target, buf, kMax, found);
    sites.assign(buf, buf + (found < kMax ? found : kMax));
    return found;
}

// 4-aligned dwords equal to value in [from, to). SEH only.
size_t ScanDwords(uintptr_t from, uintptr_t to, uint32_t value, uintptr_t* out, size_t maxOut, size_t found) {
    __try {
        for (uintptr_t p = (from + 3) & ~static_cast<uintptr_t>(3); p + 4 <= to; p += 4) {
            if (*reinterpret_cast<const uint32_t*>(p) != value) continue;
            if (found < maxOut) out[found] = p;
            found++;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return found;
}

// Every vtable slot (read-only data) holding target (address order; up to 64 recorded, the count is exact)
size_t SlotsOf(uintptr_t target, std::vector<uintptr_t>& slots) {
    constexpr size_t kMax = 64;
    uintptr_t buf[kMax];
    size_t found = 0;
    for (const Range& r : g_rodata) found = ScanDwords(r.begin, r.end, static_cast<uint32_t>(target), buf, kMax, found);
    slots.assign(buf, buf + (found < kMax ? found : kMax));
    return found;
}

void ResolveSlotsOf(const Entry& e) {
    const uintptr_t target = g_found[Index(e.dep)];
    if (!target) {
        LOG_INFO(std::format("[Addr] {}: skipped ({} not found)", Name(e.id), Name(e.dep)));
        return;
    }
    std::vector<uintptr_t> slots;
    const size_t count = SlotsOf(target, slots);
    std::string at;
    for (size_t i = 0; i < slots.size() && i < 12; i++) at += std::format("{}{:#010x}", i ? ", " : "", slots[i]);
    const bool ok = count == static_cast<size_t>(e.arg) && slots.size() == count;
    bool steamOk = ok;
    for (int i = 0; ok && i < e.arg; i++) steamOk = steamOk && slots[i] == kInfo[Index(e.id) + i].steam;
    LOG_INFO(std::format("[Addr] {}..{}: {} vtable slot(s) holding {} at {} (expects {}; Steam {:#010x}..){}", Name(e.id), e.arg - 1, count, Name(e.dep), at, e.arg,
                         kInfo[Index(e.id)].steam, IsFixedBuild() && ok ? (steamOk ? " ok" : " DIFFERS: the fixed addresses are kept") : ""));
    if (!ok) return;
    for (int i = 0; i < e.arg; i++) g_found[Index(e.id) + i] = slots[i];
}

void ResolveCallersOf(const Entry& e) {
    const uintptr_t target = g_found[Index(e.dep)];
    if (!target) {
        LOG_INFO(std::format("[Addr] {}: skipped ({} not found)", Name(e.id), Name(e.dep)));
        return;
    }
    std::vector<uintptr_t> sites;
    const size_t count = CallersOf(target, sites);
    std::string at;
    for (size_t i = 0; i < sites.size() && i < 12; i++) at += std::format("{}{:#010x}", i ? ", " : "", sites[i]);
    const bool ok = count == static_cast<size_t>(e.arg) && sites.size() == count;
    LOG_INFO(std::format("[Addr] {}..{}: {} call(s) of {} at {} (expects {}; Steam {:#010x}..){}", Name(e.id), e.arg - 1, count, Name(e.dep), at, e.arg, kInfo[Index(e.id)].steam,
                         IsFixedBuild() && ok ? (sites[0] == kInfo[Index(e.id)].steam ? " ok" : " DIFFERS: the fixed addresses are kept") : ""));
    if (!ok) return;
    for (int i = 0; i < e.arg; i++) g_found[Index(e.id) + i] = sites[i];
}


// Light factory: jump table [type - 3] -> case code "mov ecx,eax; call ctor" -> the ctor's first "mov [esi|edi], vtable"
uintptr_t LightVtableOfType(uintptr_t table, int type) {
    uint32_t caseCode = 0;
    if (!ReadU32(table + 4 * static_cast<uintptr_t>(type - 3), caseCode) || !g_text.Has(caseCode, 0x48)) return 0;
    uint8_t c[0x48];
    if (!SafeRead(caseCode, c, sizeof c)) return 0;
    uintptr_t ctor = 0;
    for (int i = 0; i + 7 <= static_cast<int>(sizeof c); i++)
        if (c[i] == 0x8B && c[i + 1] == 0xC8 && c[i + 2] == 0xE8) {
            ctor = CallTargetAt(caseCode + i + 2);
            break;
        }
    if (!ctor || !g_text.Has(ctor, 0x88)) return 0;
    uint8_t f[0x88];
    if (!SafeRead(ctor, f, sizeof f)) return 0;
    for (int i = 0; i + 6 <= static_cast<int>(sizeof f); i++)
        if (f[i] == 0xC7 && (f[i + 1] == 0x06 || f[i + 1] == 0x07)) {
            uint32_t v;
            std::memcpy(&v, f + i + 2, 4);
            if (g_image.Has(v, 0x50) && !g_text.Has(v)) return v;
        }
    return 0;
}

void ResolveEntry(const Entry& e) {
    const size_t i = Index(e.id);
    const bool hasDep = e.dep != None;
    const uintptr_t dep = hasDep ? g_found[Index(e.dep)] : 0;
    const char* name = Name(e.id);
    switch (e.kind) {
    case K::Sig:
    case K::LowestOf2: {
        uintptr_t a = TrySig(e, 0, g_text.begin, g_text.end);
        if (!a && e.sig[1].pattern) a = TrySig(e, 1, g_text.begin, g_text.end);
        g_found[i] = a;
        break;
    }
    case K::Multi:
        if (!TryMulti(e, 0) && e.sig[1].pattern) TryMulti(e, 1);
        break;
    case K::InRange: {
        if (!dep) {
            LOG_INFO(std::format("[Addr] {}: skipped ({} not found)", name, Name(e.dep)));
            break;
        }
        const uintptr_t to = dep + static_cast<uintptr_t>(e.arg) < g_text.end ? dep + static_cast<uintptr_t>(e.arg) : g_text.end;
        uintptr_t a = TrySig(e, 0, dep, to);
        if (!a && e.sig[1].pattern) a = TrySig(e, 1, dep, to);
        g_found[i] = a;
        break;
    }
    case K::CallIn: {
        if (!dep) {
            LOG_INFO(std::format("[Addr] {}: skipped ({} not found)", name, Name(e.dep)));
            break;
        }
        for (int w = 0; w < 2 && !g_found[i]; w++) {
            const Sig& s = e.sig[w];
            if (!s.pattern) continue;
            Pattern pat;
            if (!ParsePattern(s.pattern, pat)) continue;
            std::vector<uintptr_t> hits;
            const size_t count = FindAll(pat, g_text.begin, g_text.end, hits);
            uintptr_t a = 0;
            if (count == 1)
                for (int k = 0; k + 5 <= e.arg && !a; k++)
                    if (CallTargetAt(hits[0] + s.offset + k) == dep) a = hits[0] + s.offset + k;
            LogMatches(name, w ? " (alternate)" : "", count, hits, a, e.id);
            g_found[i] = a;
        }
        break;
    }
    case K::Deref: {
        uint32_t v = 0;
        g_found[i] = dep && ReadU32(dep + static_cast<uintptr_t>(e.arg), v) && InWhere(e.where, v) ? v : 0;
        LOG_INFO(std::format("[Addr] {}: {} from {} + {:#x} (Steam {:#010x}){}", name, g_found[i] ? std::format("{:#010x}", g_found[i]) : std::string("not found"), Name(e.dep), e.arg,
                             kInfo[i].steam, IsFixedBuild() ? (g_found[i] == kInfo[i].steam ? " ok" : " DIFFERS: the fixed address is kept") : ""));
        break;
    }
    case K::Target: {
        uintptr_t a = dep ? CallTargetAt(dep) : 0;
        if (!InWhere(e.where, a)) a = 0;
        if (a || !e.sig[0].pattern)
            LOG_INFO(std::format("[Addr] {}: {} = target of {} (Steam {:#010x}){}", name, a ? std::format("{:#010x}", a) : std::string("not found"), Name(e.dep), kInfo[i].steam,
                                 IsFixedBuild() ? (a == kInfo[i].steam ? " ok" : " DIFFERS: the fixed address is kept") : ""));
        if (!a && e.sig[0].pattern) { // fallback: its own signature
            a = TrySig(e, 0, g_text.begin, g_text.end);
            if (!a && e.sig[1].pattern) a = TrySig(e, 1, g_text.begin, g_text.end);
        }
        g_found[i] = a;
        break;
    }
    case K::CallersOf:
        ResolveCallersOf(e);
        break;
    case K::SlotsOf:
        ResolveSlotsOf(e);
        break;
    case K::LightType: {
        const uintptr_t v = dep ? LightVtableOfType(dep, e.arg) : 0;
        g_found[i] = InWhere(e.where, v) ? v : 0;
        LOG_INFO(std::format("[Addr] {}: {} from the light factory, type {} (Steam {:#010x}){}", name, g_found[i] ? std::format("{:#010x}", g_found[i]) : std::string("not found"), e.arg,
                             kInfo[i].steam, IsFixedBuild() ? (g_found[i] == kInfo[i].steam ? " ok" : " DIFFERS: the fixed address is kept") : ""));
        break;
    }
    }
}

// Cross-checks between entries (non-Steam: a failed check clears the ids concerned)
void CrossChecks() {
    auto clear = [](std::initializer_list<Id> ids, const char* why) {
        std::string names;
        for (Id id : ids) {
            names += (names.empty() ? "" : ", ") + std::string(Name(id));
            g_found[Index(id)] = 0;
        }
        LOG_WARNING(std::format("[Addr] {}: {} dropped", why, names));
    };
    // The nine light vtables: distinct, and the same position function in all of them
    bool classesOk = true;
    for (int a = 0; a < 9; a++)
        for (int b = a + 1; b < 9; b++) {
            const uintptr_t va = g_found[Index(Id::LightVtable3) + a], vb = g_found[Index(Id::LightVtable3) + b];
            if (va && va == vb) classesOk = false;
        }
    for (int t = 0; t < 9; t++) {
        uint32_t pos = 0;
        const uintptr_t vt = g_found[Index(Id::LightVtable3) + t];
        if (vt && (!ReadU32(vt + 0x24, pos) || pos != g_found[Index(Id::LightPos)])) classesOk = false;
    }
    if (!classesOk) {
        for (int t = 0; t < 9; t++) g_found[Index(Id::LightVtable3) + t] = g_found[Index(Id::LightColour3) + t] = g_found[Index(Id::LightEval3) + t] = 0;
        clear({Id::LightPos, Id::LampColourSite}, "Light classes are not nine distinct vtables with one position function");
    }
    // Calls that must lie inside the function they were searched from, and the batch solve among the solve calls
    const uintptr_t batch = g_found[Index(Id::BatchSolveCall)];
    bool batchListed = false;
    for (int k = 0; k < 3; k++) batchListed |= batch && g_found[Index(Id::SolvePointCall0) + k] == batch;
    if (batch && g_found[Index(Id::SolvePointCall0)] && !batchListed) clear({Id::BatchSolveCall}, "The batch solve call is not one of the three solve calls");
    if (g_found[Index(Id::BatchSolveCall)] && CallTargetAt(g_found[Index(Id::BatchSolveCall)]) != g_found[Index(Id::SolvePoint)])
        clear({Id::BatchSolveCall}, "The batch solve call does not call the point solve");
    // The cascade's three calls lie after its test, in order
    const uintptr_t c = g_found[Index(Id::CascadeTest)], r = g_found[Index(Id::RoomByIdCall)], v = g_found[Index(Id::InvalidateCall)], s = g_found[Index(Id::SetInsertCall)];
    if (c && r && v && s && !(c < r && r < v && v < s)) clear({Id::RoomByIdCall, Id::InvalidateCall, Id::SetInsertCall}, "The story refresh calls are out of order");
}

void FindSections() {
    const auto base = reinterpret_cast<uintptr_t>(GetModuleHandleW(nullptr));
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    g_image = {base, base + nt->OptionalHeader.SizeOfImage};
    const IMAGE_SECTION_HEADER* sec = IMAGE_FIRST_SECTION(nt);
    std::vector<Range> rodataSections; // initialised data, readable, neither writable nor executable (.rdata, .rsrc)
    for (WORD k = 0; k < nt->FileHeader.NumberOfSections; k++, sec++) {
        const DWORD ch = sec->Characteristics;
        if ((ch & IMAGE_SCN_CNT_INITIALIZED_DATA) && (ch & IMAGE_SCN_MEM_READ) && !(ch & (IMAGE_SCN_MEM_WRITE | IMAGE_SCN_MEM_EXECUTE)))
            rodataSections.push_back({base + sec->VirtualAddress, base + sec->VirtualAddress + sec->Misc.VirtualSize});
    }
    sec = IMAGE_FIRST_SECTION(nt);
    for (WORD k = 0; k < nt->FileHeader.NumberOfSections; k++, sec++) {
        const bool named = std::strncmp(reinterpret_cast<const char*>(sec->Name), ".text", IMAGE_SIZEOF_SHORT_NAME) == 0;
        if (named || (!g_text.begin && (sec->Characteristics & IMAGE_SCN_MEM_EXECUTE))) {
            g_text = {base + sec->VirtualAddress, base + sec->VirtualAddress + sec->Misc.VirtualSize};
            if (named) break;
        }
    }
    // readable parts (a section can hold guard or no-access pages)
    auto readableParts = [](const Range& section, std::vector<Range>& out) {
        uintptr_t at = section.begin;
        while (at < section.end) {
            MEMORY_BASIC_INFORMATION mbi{};
            if (!VirtualQuery(reinterpret_cast<LPCVOID>(at), &mbi, sizeof mbi)) break;
            uintptr_t regionEnd = reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
            if (regionEnd > section.end) regionEnd = section.end;
            const DWORD prot = mbi.Protect & 0xFF;
            const bool readable = mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_GUARD) && prot != PAGE_NOACCESS && prot != 0;
            if (readable) {
                if (!out.empty() && out.back().end == at) out.back().end = regionEnd;
                else out.push_back({at, regionEnd});
            }
            if (regionEnd <= at) break;
            at = regionEnd;
        }
    };
    g_readable.clear();
    readableParts(g_text, g_readable);
    g_rodata.clear();
    for (const Range& r : rodataSections) readableParts(r, g_rodata);
}

// "push ebp; mov ebp,esp" count in .text: thousands in plain code, ~0 while it is still encrypted
size_t PrologueCount() {
    Pattern p;
    ParsePattern("55 8B EC", p);
    std::vector<uintptr_t> hits;
    return FindAll(p, g_text.begin, g_text.end, hits);
}

} // namespace

// ---------------------------------------------------------------------------------------------------------------------
// Public
// ---------------------------------------------------------------------------------------------------------------------
bool IsFixed() { return IsFixedBuild(); }

uintptr_t Get(Id id) {
    const size_t i = Index(id);
    if (i >= Index(Id::Count)) return 0;
    if (IsFixedBuild()) return kInfo[i].steam;
    return g_resolved.load(std::memory_order_acquire) ? g_final[i] : 0;
}

const char* Name(Id id) {
    const size_t i = Index(id);
    return i < Index(Id::Count) ? kInfo[i].name : "?";
}

bool Resolved() { return IsFixedBuild() || g_resolved.load(std::memory_order_acquire); }

bool Scanned() { return g_resolved.load(std::memory_order_acquire); }

bool Have(std::initializer_list<Id> ids, std::string* missing) {
    bool all = true;
    for (Id id : ids) {
        if (Get(id)) continue;
        all = false;
        if (missing) *missing += (missing->empty() ? "" : ", ") + std::string(Name(id));
    }
    return all;
}

std::string NotAvailable(const std::string& missing) {
    std::string s = std::string("Not available on ") + GetGameVersionName();
    if (!Resolved()) return s + " (game code not scanned yet)";
    return missing.empty() ? s : s + ": missing " + missing;
}

bool GroupAvailable(const char* group, std::string* missing) {
    if (!group) return false;
    for (const Group& g : kGroups) {
        if (std::strcmp(g.name, group) != 0) continue;
        if (!Resolved()) {
            if (missing) *missing = "game code not scanned yet";
            return false;
        }
        bool all = true;
        for (Id id : g.ids)
            if (!Get(id)) {
                all = false;
                if (missing) *missing += (missing->empty() ? "" : ", ") + std::string(Name(id));
            }
        return all;
    }
    return false;
}

int LightTypeOfVtable(uintptr_t vtable) {
    if (!vtable) return -1;
    for (int t = 0; t < 9; t++)
        if (Get(static_cast<Id>(Index(Id::LightVtable3) + t)) == vtable) return t + 3;
    return -1;
}

void Resolve() {
    if (g_resolveRan) return;
    g_resolveRan = true;
    FindSections();
    if (!g_text.begin || g_readable.empty()) {
        LOG_ERROR("[Addr] The game's code section was not found: game-code features stay off");
        g_resolved.store(true, std::memory_order_release);
        return;
    }
    const auto started = GetTickCount64();
    // EA app build: .text is decrypted in memory by the activation stub at start-up. By the first Present it long has
    // been; if it still looks encrypted, wait a little.
    size_t prologues = PrologueCount();
    for (int tries = 0; !IsFixedBuild() && prologues < 1000 && tries < 15; tries++) {
        LOG_WARNING(std::format("[Addr] The game's code looks encrypted still ({} \"push ebp; mov ebp,esp\"): waiting 1 s", prologues));
        Sleep(1000);
        prologues = PrologueCount();
    }
    // (2.15.3) On Steam 1.67.2 the fixed addresses are used and the signature scan is only a self-check for the log (about
    // 0.9 s of the start, during which the world load already runs without Apex's caches): developer mode only
    if (IsFixedBuild() && kPublicBuild) {
        LOG_INFO("[Addr] Steam 1.67.2: the fixed addresses are used (the signature self-check runs in developer mode)");
        g_resolved.store(true, std::memory_order_release);
        return;
    }
    LOG_INFO(std::format("[Addr] Scanning the game's code for {}: .text {:#010x}..{:#010x} ({} readable range(s), {} \"push ebp; mov ebp,esp\")", GetGameVersionName(), g_text.begin,
                         g_text.end, g_readable.size(), prologues));
    if (IsFixedBuild()) LOG_INFO("[Addr] Steam 1.67.2: the fixed addresses are used; the signatures below are only a self-check");
    for (const Entry& e : kTable) ResolveEntry(e);
    CrossChecks();
    int found = 0, differs = 0;
    for (size_t i = 0; i < Index(Id::Count); i++) {
        g_final[i] = g_found[i];
        if (g_found[i]) found++;
        if (IsFixedBuild() && g_found[i] != kInfo[i].steam) differs++;
    }
    g_resolved.store(true, std::memory_order_release);
    std::string groups;
    for (const Group& g : kGroups) {
        std::string missing;
        const bool ok = GroupAvailable(g.name, &missing);
        groups += std::format(" | {}: {}", g.name, ok ? "available" : "missing " + missing);
    }
    LOG_INFO(std::format("[Addr] {} of {} addresses found in {} ms{}{}", found, Index(Id::Count), GetTickCount64() - started,
                         IsFixedBuild() ? std::format(" ({} differ from the fixed Steam addresses)", differs) : std::string(), groups));
    // One line with every id the signatures did not give on this build (on Steam: the ids whose signature disagrees with
    // the fixed address), so a single log tells which signatures to refine
    std::string missing, differing;
    for (size_t i = 0; i < Index(Id::Count); i++) {
        if (!g_found[i]) missing += (missing.empty() ? "" : ", ") + std::string(kInfo[i].name);
        else if (IsFixedBuild() && g_found[i] != kInfo[i].steam) differing += (differing.empty() ? "" : ", ") + std::string(kInfo[i].name);
    }
    LOG_INFO(std::format("[Addr] Not found on {} ({} of {}): {}", GetGameVersionName(), Index(Id::Count) - static_cast<size_t>(found), Index(Id::Count), missing.empty() ? "none" : missing));
    if (IsFixedBuild()) LOG_INFO("[Addr] Signatures differing from the fixed Steam addresses: " + (differing.empty() ? std::string("none") : differing));
}

// ---------------------------------------------------------------------------------------------------------------------
// Struct offset sanity check (non-Steam builds, once, first world)
// ---------------------------------------------------------------------------------------------------------------------
namespace {
std::vector<uintptr_t> g_checkLights;
void __fastcall CheckVisit(void*, void*, uintptr_t light) {
    if (g_checkLights.size() < 50000) g_checkLights.push_back(light);
}
void* g_checkVtbl[1] = {reinterpret_cast<void*>(&CheckVisit)};
struct CheckVisitor {
    void** vtbl;
} g_checkVisitor{g_checkVtbl};

bool CallEnum(uintptr_t fn) {
    __try {
        reinterpret_cast<void(__stdcall*)(void*)>(fn)(&g_checkVisitor);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
bool g_worldChecked = false;
} // namespace

void CheckWorldStructs() {
    if (IsFixedBuild() || g_worldChecked) return;
    g_worldChecked = true;
    const uintptr_t rootPtr = Get(Id::RootPtr);
    uint32_t root = 0, mgr = 0, cells = 0, tree = 0, buckets = 0;
    float level = -1.0f;
    int c38 = 0, c3C = 0;
    const bool chain = rootPtr && ReadU32(rootPtr, root) && root && ReadU32(root + 0x1C0, mgr) && mgr && SafeRead(mgr + 0xF0, &level, 4) && ReadU32(mgr + 0x104, cells) &&
                       ReadU32(mgr + 0xD4, tree) && cells && SafeRead(cells + 0x38, &c38, 4) && SafeRead(cells + 0x3C, &c3C, 4);
    if (tree) ReadU32(tree + 0x5C, buckets);
    const bool plausible = chain && level >= 0.0f && level <= 1.01f && c38 >= -1 && c38 < 100000 && c3C >= -1 && c3C < 100000 && buckets < (1u << 20);
    LOG_INFO(std::format("[Addr] Struct check ({}): light manager {:#010x} (root+0x1C0), night level {:.2f} (+0xF0), cells {:#010x} (+0x104) countdowns {} / {} (+0x38/+0x3C), "
                         "light tree {:#010x} (+0xD4) buckets {} (+0x5C): {}",
                         GetGameVersionName(), mgr, level, cells, c38, c3C, tree, buckets, plausible ? "plausible" : "NOT plausible (offsets may differ on this build)"));
    // Lights: vtable -> type from the factory; the type field (+0xB0) must agree
    const uintptr_t en = Get(Id::EnumLights);
    if (!en) return;
    g_checkLights.clear();
    if (!CallEnum(en)) {
        LOG_WARNING("[Addr] Struct check: the light enumeration faulted");
        return;
    }
    int agree = 0, disagree = 0, unknown = 0, alive = 0;
    for (uintptr_t L : g_checkLights) {
        uint32_t vt = 0;
        int type = -1;
        uint8_t flags = 0;
        if (!ReadU32(L, vt) || !SafeRead(L + 0xB0, &type, 4) || !SafeRead(L + 0x100, &flags, 1)) {
            unknown++;
            continue;
        }
        const int t = LightTypeOfVtable(vt);
        if (t < 0) unknown++;
        else if (t == type) agree++;
        else disagree++;
        if (flags & 1) alive++;
    }
    LOG_INFO(std::format("[Addr] Struct check: {} lights; vtable type = type field (+0xB0) for {}, differs for {}, unknown vtable {}; alive flag (+0x100 bit 0) on {}{}",
                         g_checkLights.size(), agree, disagree, unknown, alive, disagree || (unknown && !agree) ? " -- light offsets may differ on this build" : ""));
    g_checkLights.clear();
    g_checkLights.shrink_to_fit();
}

} // namespace GameAddr
