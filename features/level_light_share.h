#pragma once
// Outdoor lot lamps light the walls and floors of every floor, not only their own, and indoor lamps light the story above
// or below through stair openings (see level_light_share.cpp).
#include "ground_lamp_shadow.h"
#include <cstdint>
#include <string>
#include <utility>
#include <vector>
namespace RtLampProxy {
struct Lot;
}
namespace LevelLightShare {
bool Install(std::string& error);
void Uninstall();
bool IsInstalled();
// True only while the validated directional-map floor guard is installed and indoor sharing is ready.
bool BasisFloorGuardReady();
void OnPresent();      // render thread, every frame (runs the refresh asked by Install/Uninstall)
void OnWorldChanged(); // forget the rooms of the previous world
// The world is on screen after its load: every lot's rooms near openings gather once more (then settle)
void OnWorldLive();
// Render thread: how many lots are loaded now (0 at the main menu and during most of a load)
int LoadedLots();
bool LoadedRoomsBusy(); // cached room states plus pending ambient; startup refresh readiness
// Development tools (render thread): the story each loaded lot shows (low half of its id, story); returns the count
int DisplayLevels(uint32_t* lots, int* stories, int max);
// Development tools (F6 recorder, render thread): "[room]" lines for the indoor rooms whose ambient, state, class or light
// count changed since the last call (reset: every room again)
std::vector<std::string> TraceRooms(bool reset);
uint64_t TraceRoomsRelease(); // render thread, after a recording: its room states given back (bytes, estimated)
// Indoor lamps also light the story above or below through stair openings (the rooms near them gather again on a change)
void SetIndoor(bool on);
// Walls are lit at the heights the game draws their light at, so the walls of two stories meet the floor line on the same
// light (every room lights its walls again on a change)
void SetWallAlign(bool on);
// Every room of the active lot solved at the top LOD class on every story, so a floor change re-solves nothing (default on)
void SetAllFloors(bool on);
// "High quality on every lot" is on (night_terrain_relight): SetAllFloors then applies to the priority lot only
void SetAllLotsHighQuality(bool on);
// Walls block lamp light on outdoor floors (open rooms, decks): the floor map's alpha carries the blocked share of the
// outdoor lamps (relights every room when it changes); Active = installed, the game code matched and on
void SetFloorWalls(bool on);
bool FloorWallsActive();
// Light through doors and windows (experimental, option "luzRealistaPorAberturas"): a walled roofless room (a yard, a light
// well, a deck behind a half wall) takes the outdoor lamps of its own story that can reach it, each lit through doors,
// windows and arches at their height or over walls lower than the lamp (every wall of the story counts); a closed one
// takes none. Relights every room when changed. Active = on, installed, and "walls block light on floors" active too.
void SetRealisticOpenings(bool on);
bool RealisticOpeningsActive();
// Outdoor light through openings (option "luzDeForaPelasAberturas", default on): a roofed room with a door, window or arch
// to the outside (room 0 or a roofless room of its story) also takes the outdoor lamps of room 0 of its story and of the story
// below that see one of those openings, each tested per point against every wall of the story through the openings' holes and
// under its own roof; a room with no known opening takes none. Relights every room when changed. Active = on and installed,
// the openings noted.
void SetOutdoorThroughOpenings(bool on);
bool OutdoorThroughOpeningsActive();
// Doors block light (option "portasBloqueiamLuz", default on): a door or gate cut (and a cut of unknown kind) lets no lamp
// light through, as the game keeps doors closed except while they open; arches, windows and doors with glass (a window light
// of the door in its cell) stay open. Every reader of the
// openings follows it (the story grids, the roofed rooms' doors, the outside walls' copies, room 0's wall test). A change
// sends again only the stories with such a cut noted.
void SetDoorsBlockLight(bool on);
bool DoorsBlockLightActive();
// Indoor light through openings (option "luzInternaPelasAberturas", default on in the 2.12.4 development branch): a lit lamp
// of a roofed room shines through its arches, windows (and doors while doors do not block light) onto room 0 and the
// roofless rooms of its story (outside walls, outdoor floors, lot ground; room 0 of the story below for a house on a
// foundation) and into a roofed neighbour through an opening between them; each tested per point against every wall of the
// story through the openings' holes. One opening only. Relights every room when changed. Active = on and installed, the
// openings noted.
void SetIndoorThroughOpenings(bool on);
bool IndoorThroughOpeningsActive();
// Shadow softness (option "suavidadeDasSombras", 2.12.4): a lamp's radius in metres (0.02 .. 0.5, default 0.15), the size
// of the penumbra its light has past a wall's opening, end or top and a slab's edge (penumbra.h). A change sends the rooms
// such an edge shapes (of every loaded lot: room 0, the roofless rooms, the roofed rooms with openings), the object rigs
// and the ground shade; called every frame, applied once the slider is let go.
void SetShadowSoftness(float radius);
// Smooth light maps (option "mapasDosComodosSuaves", 2.12.4), the soft light cut: in the room solves Apex wraps, a lamp's
// light fades out over its last threshold step (r + g + b from the room's threshold to twice it) instead of stopping at the
// game's threshold, so a lamp's circle on a floor or wall has no ring. Relights every room when changed (not the first
// call: the saved value).
void SetSoftLightCut(bool on);
bool SoftLightCutOn();
std::string SoftLightCutStatus();
// Any thread: the half width (m) an edge is spread over at a texel of the room's light maps (0.75 texel; 0 = unknown)
float MapHalfTexelOf(const void* room);
// Any thread: every wall atlas block the solves laid out so far starts on the grid of its 3n + 1 rows (Smooth light maps'
// band clamp relies on it; no block seen yet = true)
bool WallBandsAligned();
std::string WallBandStatus();
// Render thread, the per-pixel lamps of outdoor objects and fences: the lamps shining out of a house through an opening
// (sorted), and the share (0..1) of such a lamp reaching `point` past the walls of its story through the openings, the
// floors under it and the outside walls' copy (LampVisibility); -1 when the lamp is not one of them
void ExitLamps(std::vector<uintptr_t>& out);
float ExitLampVisibility(const void* light, const float lamp[3], const float point[3], float nearSkip = 0.2f);
// Object rigs (any thread): the share (0..1) of an outdoor lamp that a roofed room took through its openings reaching
// `point` (world) past the walls of its story; -1 when the lamp is not one of those of that room. Reads an immutable copy
// published by the room gathers.
float OpeningLampReach(const void* room, const void* light, const float* point);
bool IsOpeningLamp(const void* room, const void* light); // (F7 notes) the same set, no test
// Render thread: the rooms whose set of such lamps changed since the last call (their object rigs gather again)
void TakeOpeningRigRooms(std::vector<const void*>& out);
// Realistic lamp reach (option "alcanceRealistaDasLampadas"): lamp light on outdoor walls fades with distance as it does
// on the ground (no flat plateau out to 9 m, nothing past 6..20 m depending on the lamp). Relights every room when changed.
void SetRealisticReach(bool on);
// The walls are lit by that law now (on and installed): the per-pixel lamps of outdoor objects, fences and windows follow
// the same law then (lot_light_bridge, lamp_reach.h). Any thread.
bool RealisticReachActive();
// Walls block light on objects: true when an outside wall stands between the lamp and the point at the height the ray
// crosses it (render thread; a copy of each story's outside walls taken when room 0 is solved). Since 2.12.4 the copy
// carries each wall's doors, windows and arches and its half-wall cells from its story grid (wall_openings.h): a ray
// through an opening or over a lower cell passes (LampVisibility takes the share an opening's soft rim leaves); a wall
// whose openings are not known stays whole.
// A wall crossed within nearSkip metres of the point is the wall the object sits in and does not block (0.2 m; Windows
// take outdoor light passes more: a turned window's origin is half a tile inside its wall).
bool WallBlocks(const float lamp[3], const float point[3], float nearSkip = 0.2f);
// Buildings block light on objects (render thread, option "paredesBloqueiamLuzNosObjetos", SetObjectWalls): the share
// (0..1) of a lamp's light that reaches the point.
// 0 when WallBlocks; otherwise the placed floors of every loaded lot's stories >= 1 that a ray going up crosses stop it,
// with the soft edge of a 0.3 m lantern at a slab's edge (lamp_visibility.h; a copy of the floors taken when each story's
// room 0 is solved). Rays going down are not tested (1). The point is one point: an object standing on a slab is tested
// where the caller puts it (on the slab's plane it is tested 5 cm toward the lamp).
float LampVisibility(const float lamp[3], const float point[3], float nearSkip = 0.2f);
// Everything LampVisibility reads besides its arguments, as one key (render thread), and whether F8 records its tests
uint64_t LampVisibilityState();
bool VisibilityDiagArmed();
// The same share from any thread (the object rigs' gather): tested against an immutable copy of the walls and slabs that
// OnPresent publishes when they change (up to a frame old). 1 while the option is off or before the first copy.
float LampVisibilityShared(const float lamp[3], const float point[3], float nearSkip = 0.2f);
// Render thread: copies of the outside walls (the copy WallBlocks tests) whose segment meets the world box [x0, x1] x
// [z0, z1]; returns the walls' copy generation (it changes when any story's outside walls change)
// patches (optional): the walls' patches (their lower cells and openings, wall_openings.h), each wall's p0 / pn into it;
// without it every wall is whole
uint32_t OutsideWallsIn(float x0, float z0, float x1, float z1, std::vector<GroundShadow::Wall>& out, std::vector<WallOpenings::Patch>* patches = nullptr);
uint32_t SlabCopyGen();     // the slab copy's generation (any thread)
uint32_t OutsideWallsGen(); // the outside walls' copy generation (any thread; OutsideWallsIn returns the one it read)
bool ObjectWallsOn();   // "Buildings block lamp light on trees and objects" is on and installed (any thread)
// "Walls shade lamp light near the ground" (option "paredesSombreiamOChao", default on; 14/10, was part of the option above):
// the lot ground shade (LotLightBridge) from the same copy of the outside walls. Any thread.
bool GroundShadeOn();
void SetGroundShade(bool on);
std::string SharedVisibilityStatus();
// An outside wall running along (dirX, dirZ) (unit, within about 10 degrees) passes within maxDist metres of the point at
// its height + 0.5 m (render thread): the object is set in that wall (Windows take outdoor light picks its lamps there)
bool OnWallLine(const float point[3], float dirX, float dirZ, float maxDist);
// F7 light capture: the wall piece a screen ray (world origin, direction) meets, as the solve laid it out (base, rows, the
// heights its rows were lit at, the atlas texel the pixel reads) and the lamps of its room; a note when none
std::string WallNotesOnRay(const float origin[3], const float dir[3], float* hitOut = nullptr);
// (development builds, arch_ambient) A floor draw of a room whose ambient colour is `own` (the draw's constant): its arches into
// other roofed rooms, one row pair each (ShaderPatches::PatchRoomMapSmooth archAmb): (a.x, a.z, e.x, e.z) in lot metres and
// (the neighbour's ambient over own, 0 where lower; 1 / |e|^2). Returns how many were written (at most maxCells); 0 when the
// room is not known. mapTex = the room light map the draw binds, basisTex = one of its directional maps (0 = none bound): they
// name the draw's story, and only rooms of that story give their arches (together when they share the ambient); with no story
// known, only matches all on one story of one lot do. Render thread.
int ArchAmbientConsts(const float own[3], uintptr_t mapTex, uintptr_t basisTex, float* rows, int maxCells);
std::string ArchAmbientStatus();
// (development builds, basis_wall_filter) The wall mask (64 x 64 A8R8G8B8, the directional maps' layout: r -x, g +x, b -z, a +z
// walled) of the story whose directional maps include the D3D texture basisTex (render thread). gen = the mask's version;
// px (16384 bytes) is written when it differs from knownGen. False: no mask known for that story.
bool WallMaskFor(uintptr_t basisTex, uint32_t knownGen, uint32_t& gen, uint8_t* px);
// (development builds, stair_room_fill) the story's ambient per tile (64 x 64 x 4 floats; its room's +0x110), for the draw's
// directional maps; sig changes with the values. False: no story or an unreadable room.
bool AmbientMapFor(uintptr_t basisTex, uint64_t knownSig, uint64_t& sig, float* rgba);
// (development builds, stair_story_blend) the story above's directional maps for a draw's (cur by direction, up in the same order) and the
// two stories' floor heights (world y); false: unknown story or none above
bool StoryAboveFor(const uintptr_t cur[4], uintptr_t up[4], float& yLow, float& yUp);
std::string WallMaskStatus();
// F7 light capture, the whole scene: every noted wall piece with the wall-shader vertices on its line (world positions of the
// frame's wall draws): the base its light uses against where it is drawn, per draw, and a summary per story
struct SurveyPoint {
    int draw;
    float x, y, z;
};
std::string WallSurvey(const std::vector<SurveyPoint>& points);
void SetObjectWalls(bool on);
// Outside walls lit where the game draws them (houses on a foundation draw them about 2 m under the base their light uses;
// wall_heights.h): only outdoor rooms' walls, from the measured wall meshes; off = the game's heights. Relights every room.
void SetFoundationWalls(bool on);
// The terrain bake stub stamps lamps with cones (type 5) at their spill (night_terrain_relight_patch.cpp BakeColourStub):
// the upper-floor gate (UnderFloorShare) weighs them the same way. Set at install, cleared at uninstall.
void SetBakeConeSpill(bool on);
bool BakeConeSpillOn(); // (Real-time outdoor lamps: the under-floor share takes the same stamps)
bool AllFloorsDetailed(); // current full-detail policy, shared with the room scheduler
// Render thread: every room of every loaded lot lights again (options that change how rooms are lit); why = log text
void RelightAllRooms(const char* why);
// Developer Tester tab: the story grids are built again at their next use (a switch that changes how they are built)
void TesterGridsChanged();
// Render thread: every room of one loaded lot (every story, room 0 too) lights again; the rooms sent, -1 = the lot is gone
// With changedAt, fresh running/completed solves (gathered after it) are retained; 0 forces the normal refresh. With lamps,
// only the rooms whose light list holds one of them (lamps that only moved).
// Watches this lot's completion for a furniture refresh, with a 1500 ms fallback and bounded 6 s lifetime.
// editNet (12/10): a lamp edit's safety net (LampMarkFilter): the rooms sent are tagged urgent (tier 2) and the lamp edit
// hold (AtriumHold) shows them with the other stories
int RelightLot(uintptr_t tracker, const char* why, unsigned long changedAt = 0, const uintptr_t* lamps = nullptr, int lampCount = 0,
               bool editNet = false);
// Render thread: RelightLot by the lot's 64-bit id (as LotLightBridge tracks lamps); -1 = not loaded. For a lamp deleted
// in Build mode (07/10): no lamp entry update runs, so neither LampMarkFilter nor AfterChangedWalk's cross-story sends
// always see it, and the outdoor rooms of the other stories (the walls) kept the deleted lamp in their light lists.
int RelightLotById(uint64_t lot, const char* why);
// The lamps are held by indoor rooms of two stories or more (taken through a stair opening): a lamp edit then relights the
// whole lot at once (a targeted refresh left a closed room lit for seconds). Render thread.
bool LampsCrossStories(uintptr_t tracker, const uintptr_t* lamps, int lampCount);
// A lamp switched where it was (same room, not moved): its tree level (story) and room
struct LampSwitch {
    uintptr_t light, tl;
    int room;
};
// Render thread: the lamp switch's safety net for those rooms only: the rooms holding a switched (or moved) lamp, the
// switched lamps' own rooms, the rooms near the stair openings of their story and the stories next to it, and the outdoor
// rooms of every story for an outdoor lamp; fresh solves retained as RelightLot does. -1 = the lot is gone.
int RelightLampSwitch(uintptr_t tracker, const char* why, unsigned long changedAt, const LampSwitch* switched, int count,
                      const uintptr_t* moved, int movedCount);
// Lamp edits first (light tree thread, from LampMarkFilter's lamp entry update): the room this tree level's update marks for
// a lamp edit (user = colour, intensity or on / off by a player or a Sim, not a flicker) is solved before any other room, and
// so are the rooms of other stories that take its light; pure = the lamp stayed in that room (moved, switched or
// changed a value): those rooms gather in the same update (a lamp added or moved into the room waits for the game)
void NoteLampMark(uintptr_t treeLevel, int room, bool user, bool pure);
// A lamp that only moved or changed a value while its room is being solved: true = the mark is held and given back by the
// room update once that solve is over (the light follows a dragged lamp instead of restarting every step)
bool HoldLampMark(uintptr_t treeLevel, int room, bool user);
// Scheduler factor of a room (RoomLightQueue's priority hook): 1, or above any other room's for a lamp edit's rooms
float LampUrgency(const void* room);
// (development builds, auto_light_priority) an automatic switch-on's own room first in the room queue: no edit, no hold, no
// extra budget; dusk: only on the story the camera shows
void NoteQuietMark(uintptr_t treeLevel, int room, bool dusk);
long QuietMarks();
// A lamp edit's room that is a member of an atrium (rooms joined through removed floors): the scheduler solves it right
// after the lamp's own room whatever its story, so the atrium's stories change together
bool StackedWithEdit(const void* room);
// The first seconds after the world went live, while the loaded lots' rooms correct themselves (LotLightingMotion and
// RoomLightQueue give the room solves a larger share of the frame meanwhile)
bool SettlingAfterLoad();
// Render thread: the room belongs to an atrium (a stacked-ambient group of 2 rooms or more)
bool InAtrium(const void* room);
// Light tree thread: the share (0..1) of a lamp of another story that reaches a point of an indoor room, tested as the room's
// directional maps test it (the floor crossed through a stair opening, the walls on the way); -1 when the lamp is not one
// the light between stories took for that room (ObjectLightBridge gives such lamps to the room's object rigs)
float CrossLampReach(const void* room, const void* light, const float* point);
// Render thread: another member of the room's atrium group is waiting for its gather or its solve, or being solved
// (AtriumHold keeps the room's new maps waiting meanwhile, so the atrium's stories change together)
bool GroupPending(const void* room);
// A lamp edit's rooms are waiting for or in their solve (RoomLightQueue: a larger budget per frame meanwhile)
bool LampEditPending();
// Lamp edits all at once (AtriumHold, render thread; 12/10, was SwitchRoomsPending): of the rooms lamp edits marked (the urgent
// list, the safety net's tier 2 included) whose latest mark has a LampMarkFilter::EditSerial above `after`, on a story other
// than the lamps' own (OnEditStory), how many are still gathered or solved or ended no solve begun after their mark;
// `others` gets how many such rooms there are, `ownPending` how many of the own stories' rooms still wait, `ownTotal` how many
// own stories' rooms there are
int EditRoomsPending(unsigned long after, int* others = nullptr, int* ownPending = nullptr, int* ownTotal = nullptr);
// Of the rooms lamp edits marked with urgency tier 0 (an edited lamp's own room and its story's rooms that hold it), how many
// still wait for or are in their solve, or ended no solve begun after their latest mark; `total` gets how many there are.
// Render thread (DragLite: these rooms are never under the spread cap).
int OwnRoomsPending(int* total = nullptr);
// This room is a lamp edit's (urgent, any tier) and still waits for or is in its solve, or ended no solve begun after its
// latest mark: its maps do not show the edit yet. Render thread (AtriumHold: maps and furniture kept until its solve ends)
bool EditRoomWaiting(const void* room);
// The room is on the story of a lamp an edit changed in the last 5 s (its own room's story, tier 0): the edit hold never
// holds its maps (12/10). Any thread.
bool OnEditStory(const void* room);
// How many urgent rooms reached the 8 s cap still waiting for their solve (RoomLightQueue's "settled" log: urgency expired)
long UrgentExpiredPending();
// This thread is in step 1 of a room's solve, where the game locks the room's maps (AtriumHold keeps only those)
bool InMapLockStep();
// The solve's end and its map lock step are hooked: lamp switches can be shown all at once (else they change room by room)
bool SolveHooksReady();
// Apex's own share of the room solves so far, ms (-1 until calibrated); render thread
double ApexSolveMs();
// The same split by test (wall pass, the game's wall test in it, ...), cumulative ms and calls; false until calibrated
constexpr int kApexTestParts = 8;
bool ApexTestPartMs(double* ms, long* calls);
const char* ApexTestPartName(int i);
// From LampMarkFilter: a lamp edit was noted; continuous = it moved or a value changed (a drag), not switched on or off
void NoteLampEditing(bool continuous);
// A lamp is being dragged (moved or a value changed within the last 200 ms). The one-go solve once an edit ended
// (05/10, option A) was taken out the same day: 300-1300 ms frames while moving lamps in Build mode (log). False once the
// mouse button let go of the lamp (DragLite::LetGo)
bool LampDragging();
// The room's lamp-edit urgency tier (0 = a lamp's own room or story, 1 = a room taking its light, 2 = the edit's safety net;
// -1 = none). Render thread (DragLite's re-pick throttle).
int UrgentTierOf(const void* room);
// A room of a lot's story sent to its solve (any room id; the sends a lamp drag left out): with no urgency, or with an
// urgency tier (0..2) while a Classic edit's stories change together (AtriumHold)
bool QueueRoomId(uintptr_t tracker, int story, int id, int urgentTier = -1);
// Rooms at Night: visits every room (id > 0) of every loaded lot (stories -4..7); visit returns true to send the room to
// gather again. Render thread. Returns the rooms visited; queued = how many were sent.
int ForEachRoom(bool (*visit)(unsigned char* room, void* ctx), void* ctx, int* queued,
    void (*ack)(unsigned char* room, bool queued, void* ctx) = nullptr);
// Stage original room colours, then refresh connected groups after the complete traversal.
bool StageAmbientBaseChange(unsigned char* room, const float* oldOwn, const float* newOwn, const float* oldSecond, const float* newSecond);
bool HoldsAmbientBase(unsigned char* room, const float* own, const float* second);
bool StageUnlitAmbientChange(unsigned char* room, const float* target, bool& changed);
void ApplyAmbientBaseChanges();
// Development build: the solve journal's notes since fromTick (GetTickCount), with their ticks (the recorder)
std::vector<std::pair<unsigned long, std::string>> JournalSince(unsigned long fromTick);
// The same notes as data (the recorder's light update summary). event: S ambient step done, W wall pass done, E solve ended
// (its new maps show from the next frame), Q sent by Apex, H held, I / F invalidated. While a recording runs every room is
// noted; otherwise only the rooms taking or giving light through an opening. lot = the story manager's lot id (low half).
struct SolveEvent {
    unsigned long tick;
    char event;
    uint32_t lot;
    int level, id, cls, shown, state;
    bool merged; // a member of an atrium (stacked-ambient group)
};
std::vector<SolveEvent> JournalEventsSince(unsigned long fromTick);
// A tree level's lot id (low half, as SolveEvent::lot) and story; false when unreadable
bool TreeLevelLot(uintptr_t treeLevel, uint32_t& lot, int& story);
// ---- Lamp lighting: Instant (2.12.3 + RT): Quick outdoor lamp edits (outdoor_delta.h) and Real-time outdoor lamps
// (rt_outdoor_lamps.h). Light tree thread unless said; only those two parts call these, and only while Instant is chosen. ----
// The hooks a delta needs are in: the solve's start, map lock step and finalize, the wall blur, the point solves and every
// light class's evaluation (Steam 1.67.2)
bool DeltaReady();
// The game's batch solve FUN_006a31d0 over `count` samples (0x30 bytes each, a batch as the solve made it) with the room's
// light list holding only `light`, `mult` times (as often as the room's list holds it), for that one call; each sample's
// sum reaches OutdoorDelta::ReplaySample. flags = the batch's 2D / 3D test switches. False when it could not run.
bool ReplayBatch(unsigned char* room, const unsigned char* samples, int count, const char* flags, uintptr_t light, int mult);
// The floor mask's share of the ground atlas from the lamps under a floor sample's plane (the whole light list of its story's
// room 0), as SolvePointBatch takes it; -1 when the sample is not more than 2 m over its lot base or nothing reaches it
float UnderFloorShareAt(const unsigned char* room, const float* sample);
std::string FirstUnwrappedLight(const unsigned char* room); // the first light of the room's list whose evaluation is not wrapped (the log)
bool ListAllWrapped(const unsigned char* room);             // every light of the room's list has its evaluation wrapped
// The geometry the room's solve depends on, as one key: every story's grid stamp, cut cells, the wall tops and floor edits,
// and each story's room 0 walls, wall blocks and 3D blockers; 0 when unreadable. Any thread.
uint64_t LotGeometryKey(const void* room);
// The rules every room solve follows (the options that change how rooms are lit), as one key
uint64_t PhysicsKey();
// Render thread: the room is sent to be solved again (its gather and solve, as a refresh sends it)
bool QueueRoomAgain(const void* room);
// Room 0 of a story of a lot (tracker), null when none; any thread
void* Room0Of(uintptr_t tracker, int story);
// Room 0 of a story sent to its solve (urgent, whatever its kept parts say)
bool QueueRoom0(uintptr_t tracker, int story);
// After a room update: room 0 of story L changed by a lamp edit the delta took (its mark dropped); the rooms that take its
// lamps are sent as the changed-room walk sends them. Returns the rooms sent.
int SendRoom0Dependents(uintptr_t tracker, int story);
// The tail of a GPU-handled lamp edit (Real-time outdoor lamps, the outdoor delta: the edit marked no room): the rooms that
// take room 0's lamps of the stories in storyMask, each once, sent with no urgency
int SendRoom0DependentsTail(uintptr_t tracker, unsigned storyMask);
// A GPU-handled lamp edit: the changed-room walk right after it takes its rooms as that edit's tail (no urgency)
void NoteGpuEdit();
// The finalize's own steps for a room 0 the delta locks outside a solve (Steam 1.67.2, checked once): no room holds the
// class's working maps locked (0x011D0508); the room's lock step 0x0069FA40; the class-2 edge copies, the unlock
// 0x0069E950(1) with the copy back and the room's lighting message 0x006A08E0
bool RoomMapsFree();
bool LockRoomMaps(void* room);
bool FinishRoomMaps(void* room);
// The lot's occluders from its story grids (every wall with its openings, the roofed quadrants, each story's placed floors),
// lot space; false + why when a story's grid could not be built
bool ExportLotProxy(uintptr_t tracker, RtLampProxy::Lot& out, std::string& why);
// Any thread: the lots lit at the high quality (the active lot, Build mode: story manager +0x288); returns the count
int ActiveLotTrackers(uintptr_t* out, int max);
// Any thread: the game still has this lot (its tracker is in the light manager's lot list)
bool TrackerLive(uintptr_t tracker);
std::string Status();
std::string DiagText(); // F8: samples near the active lot's lights, the game's wall test and ours (empties the record; arms the recording)
// Development build: the samples above are recorded only while armed (Developer checkbox, or the first F8 dump arms it)
void SetDiagArmed(bool on);
bool DiagArmed();
// On-demand recorder only: bounded raw wall-edge measurements, no lighting changes.
// The ground light mask of a lot (lot id as mgr+0x90 / +0x94): w x h texels, 4 per metre, 255 outside, 0 inside a room of
// the two lowest stories. True when it was read and differs from knownSig (px filled); false when unchanged or unknown.
struct GroundMask {
    int w = 0, h = 0;
    uint64_t sig = 0;
    std::vector<uint8_t> px;
};
bool LotGroundMask(uint64_t lot, uint64_t knownSig, GroundMask& out);
void BeginSeamRecording();
std::string EndSeamRecording(bool save);
}
