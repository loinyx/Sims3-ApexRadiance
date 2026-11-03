#pragma once
// Outdoor lamp edits by delta (runs with Lamp lighting > Instant; docs/features/night-lighting/outdoor-lamp-delta.md).
//
// Room 0 of a lot story (the outside, +0x18 set) stores the plain sum of what each light of its list added at each texel,
// min(sum, 1). When room 0 is solved, Apex keeps, per texel, that sum and what each light added to it (read from the solve's
// running sum, as the game's own per-light evaluation added it after every test the game and Apex apply). When a later solve
// of the same room finds only a few lights changed (a lamp moved, recoloured, dimmed, switched, added or removed), the game's
// maps are brought up to date in the solve's own map lock step instead of solving every light again:
//   - each changed light is evaluated alone with the game's batch solve FUN_006a31d0 (thiscall(room, samples, {base, pitch},
//     char flags[2], sampler, char) ret 0x14), the room's light list holding only that light for the call: the same
//     samples, the same batches (their per-light wall lists), Apex's tests, soft light cut and floor mask, only where the
//     light can reach;
//   - each texel it reaches takes S - old part + new part, packed as the solve packs it (min(S, 1), the floor mask's alpha,
//     Smooth light maps' dither, the game's pack 0x0060CD10); class-2 wall blocks are blurred again from their texels
//     before the blur;
//   - the lock step then reports "not locked" (0x006A3D2D: the solve goes to its finalize, 0x006A0E00, which unlocks and
//     shows the maps as after any solve), so the room's state, the edit hold and the scheduler see an ordinary solve.
// Anything else (a first solve, another class, walls or the room changed, a light list that changed otherwise, more than a
// few lights changed, a quick pass, a solve off the light tree thread) solves the room in full and keeps its parts again.
// Off (Lamp lighting: Classic, or Instant on a device that cannot run it): nothing here runs and every solve is the game's.
#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>
#include "gpu_lamp_law.h"
#include "rt_lamp_blockers.h"

namespace RtLampGpu {
struct KeptMapData;
}

namespace OutdoorDelta {

// Render thread, every frame: on = Lamp lighting is Instant and its parts (LevelLightShare's solve hooks) are in
void SetEnabled(bool on);
bool Enabled();
// Developer self-check: a room brought up to date by a delta is solved in full once more 1.5 s after its last delta, and
// the texels the full solve packs are compared with the ones the delta wrote (log: the largest difference in 8-bit levels)
void SetSelfCheck(bool on);
// Developer probe for GPU room lamps (docs/features/night-lighting/gpu-room-lamps-probe.md): every solve of a room other than
// room 0 of the active lot is captured as room 0's are (the sum and each light's part per texel), compared with the law the
// GPU draws each lot lamp with (unshadowed) and dropped; the log gives per room and lamp how the two differ. Nothing is kept,
// drawn or written
void SetRoomProbe(bool on);
bool RoomProbe();
// Developer, GPU room lamps stage 1 (docs/features/night-lighting/gpu-room-lamps.md): every solve of a room other than room 0
// of the active lot is captured as the probe captures it and kept per room and class (the base without the lot lamps the GPU
// draws, and per texel and drawn lamp the share Apex's tests and the game's tests left of the lamp's law), within a memory
// budget. With the probe on too, each kept solve is recomposed from what was kept and compared with the solve (the log)
void SetRoomLampCapture(bool on);
bool RoomLampCapture();
// (physical indoor lamps, stage 1) the kept rooms' model: replica (per-lamp parts, as above) or physical (the base without
// every lot lamp with a GPU law, the tail, samples, batches and the room's 3D test records; no parts, no room-0 kept lamps).
// A change drops the kept rooms; the caller sends the lot to its solves. Physical rooms > 0 are drawn from their lamps (stage 3b,
// gpu_room_lamps.cpp); room 0 and lamp edits still take the game's path
void SetRoomLampPhysical(bool on);
bool RoomLampPhysical();
// The room solve hooks record for the probe or the kept captures (either is on)
bool RoomHooks();
// GPU room lamps stage 2a (RoomMapDither's pack thunk, any thread; only the light tree thread's kept capture takes it): the
// texel the solve packs next at (x, y) of its map (xy: known), its pack input before the clamp (null: not known), whether the
// dither and the roofed alpha floor apply to it
void NotePack(const void* room, bool xy, unsigned x, unsigned y, const float* input, bool dithered, bool alphaFloor);
// GPU room lamps stage 2b (gpu_room_lamps.cpp, render thread): the kept solves of story manager mgr's rooms at class cls (with
// their tail), and what the GPU draws of map m of one of them (false: not that version any more, or nothing drawn in that map)
struct KeptRef {
    const void* room;
    int id;
    uint32_t version;
};
void KeptRooms(uintptr_t mgr, int cls, std::vector<KeptRef>& out);
bool BuildKeptMap(const void* room, int cls, uint32_t version, int m, RtLampGpu::KeptMapData& out, std::string& why);
// Any thread (LevelLightShare's FinalizeHook, after the game's finalize): the room's texels in its maps were written again, so
// its kept solves no longer hold them (a solve kept on the light tree thread is kept again right after). gather: the light tree
// thread. (physical) A solve off it keeps nothing: the room's kept physical copy is drawn on while the room's geometry is the
// same (its base without the lamps still holds), and the room is sent to one solve on the light tree thread with no urgency
void RoomFinalized(const void* room, bool gather = true);
// (physical indoor lamps, stage 4) the active lot's registered lights for the render thread (gpu_room_lamps.cpp,
// rt_outdoor_lamps.cpp): walked on the light tree thread after a room update (LevelLightShare::RegisteredLights), at most every
// 250 ms and at once after a lamp edit taken; each with its story, the room it is registered in, that room's normalisation and
// roofless flag (a room 0: outdoor). Returns the snapshot's version (0: none yet). gone: the lot's lamps that left the registry
// since they were seen (deleted, or moved to a basement or off the lot; PhysLampPick::NoteGone), never drawn from a kept list
struct LotLamp {
    uintptr_t light = 0;
    int story = 0, home = 0;
    float homeNorm = 1.0f;
    bool homeOutdoor = false;
    bool operator==(const LotLamp&) const = default;
};
uint32_t LotLamps(std::vector<LotLamp>& out, std::vector<uintptr_t>* gone = nullptr);
// (LampMarkFilter's occluder mark) whether the snapshot is of the lot `tracker` (known) and holds `light`
bool InLotLamps(uintptr_t tracker, uintptr_t light, bool& known);
// (any thread) an object left a story's light maps (EntryChain LightObjectRemove): the snapshot is read again after the next
// room update, not up to 250 ms later (a deleted lamp's light may be freed)
void NoteLightObjectRemoved();
// (physical, render thread) the rooms whose game's maps lack a lamp edit taken without their solve (phys_lamp_pick.h
// TouchedRooms; noted by TakesLampEditPhys, forgotten at the room's next solve's end): NoteTouched adds a room whose map drew
// an edited lamp; SolveTouched(room) sends that room to its solve if it is one (its map is the game's again), nullptr every
// such room (the GPU's maps all went: capture or Real-time outdoor lamps off, another active lot)
void NoteTouched(const void* room);
void SolveTouched(const void* room);
// (render thread) the rooms a solve left unkept for a passing reason (its class or the model changed meanwhile: nothing else
// sends a room with no lamp to its solve again, and one unkept room refuses every physical edit of the lot) go to their solve
void SolveUnkept();
// (physical indoor lamps, stage 4; LampMarkFilter's lamp entry update, light tree thread) an edit of `light` (kind "value",
// "switch on", "switch off", "move" (within its room), "move to another room", "move to another story" or "new" (first seen;
// settles only when no room lists it yet); a literal) of a story 0 .. 7 of the active lot when every story 0 .. 7 draws the
// lot's lamps from their laws now (phys_lamp_pick.h EditRefusal: each room > 0 a kept physical solve of the class it shows,
// drawn on every map, the lamp not in its base; each room 0 drawn physically): true = the edit marks no room, the maps its
// reach meets are relit from its law. settle: a switch or an intensity change (the rooms' normalisation, ambient and bounce in
// their bases change): the rooms listing it and its own room are solved once 2 s after its last edit, with no urgency; a
// colour or a move solves nothing. dry: only whether it would be taken now (no log, nothing noted)
bool TakesLampEditPhys(uintptr_t tl, uintptr_t light, const char* kind, bool settle, bool dry = false);
// (light tree thread) whether every story 0 .. 7 of the active lot (tracker) draws `light` from its law now, as
// TakesLampEditPhys finds it (RtOutdoorLamps::AfterRoomUpdate: an outdoor lamp's edit then sends no room that takes it)
bool PhysCoversLamp(uintptr_t tracker, uintptr_t light);
// GPU room lamps stage 2b (LampMarkFilter's lamp entry update, light tree thread): a value or switch edit of `light` (wasOn,
// on: before and after) that every room of its lot listing it draws on the GPU (a room > 0: its kept solve of the class it
// shows, every texel drawn, the lamp not in its base; a room 0: Real-time outdoor lamps draws it). True: the edit marks no
// room (the GPU shows it). A switch on is taken only while the rooms listing it keep the solves they had at its switch off
// (stage 3) move: a move within its room, taken when every room listing it is a kept room (no room 0) where a copy of the
// game's tests can redo its parts (LevelLightShare::MovePath, the same geometry, known 3D blockers): one job per room for the
// move worker, which keeps a copy of the room with the lamp's new parts. dry: only whether a value or switch edit would be
// taken now (no log, nothing noted or queued; LampMarkFilter's occluder mark of the lamp's object, made just before)
bool TakesLampEdit(uintptr_t tl, uintptr_t light, bool wasOn, bool on, bool move = false, bool dry = false);

// ---- the direct path (light tree thread; LevelLightShare's hooks) ----
// The lamp entry update of story tl's room 0 (LevelLightShare::HoldLampMark, a lamp that moved or changed a value in the same
// room): true = taken, its mark is dropped and every story's room 0 of the lot is brought up to date at the end of this room
// update (AfterRoomUpdate) without a solve: the changed lights evaluated alone, the room's maps locked through its own lock
// step 0x0069FA40, rewritten where they change and finished as the finalize finishes them (edge copies, unlock 0x0069E950
// with the copy back, the room's lighting message 0x006A08E0); the rooms that take room 0's lamps are sent after it
bool TakeEdit(uintptr_t treeLevel);
void AfterRoomUpdate(const void* treeLevel);
bool DirectWriting(); // the direct path is writing a room's maps (AtriumHold: no hold)
bool HasPending();    // light tree thread: lamp edits taken and not yet run (AfterRoomUpdate runs or returns them)
// Any thread: room 0's maps hold every light of its list as it is now (kept parts current, the geometry the same): sending it
// to be solved again changes nothing (LevelLightShare's QueueRoom leaves it)
bool UpToDate(const void* room);

// ---- the room solve (light tree thread; LevelLightShare's hooks) ----
void SolveStart(unsigned char* room);           // after the game's state 0
// After the game locked the room's maps (any thread; gather = the light tree thread, the only one a delta runs on): true =
// brought up to date. A solve that is not a delta rewrites the maps: the room's kept parts of that class are dropped.
bool AtLockStep(unsigned char* room, bool gather);
void SolveEnd(unsigned char* room);             // after the game's finalize
bool DeltaSolving(const void* room);            // any thread: the room's current solve is a delta (AtriumHold)
void NoteDirectional(const void* room);         // the solve wrote a directional map (not kept: that solve is not a delta's base)
// (GPU room lamps stage 4, light tree thread) the kept capture of `room` takes its directional parts (LevelLightShare's
// BasisLightHook): for each light of the list at each sample, the accumulator before and after it and the game's step
// FUN_0069f280 called again there with a zero accumulator
bool KeepsBasis(const void* room);
void NoteBasis(const void* room, const float* pos, uintptr_t light, const float* before, const float* after, const float* step);
// One batch sample of the room's solve (SolvePointBatch): Sample returns true when its sums are kept
bool Sample(const unsigned char* room);
// The sum the next point solve adds to: 0 the room's own, 1 the floor mask's copy with walls tested, 2 its copy with none
void Tap(int which, const float* sum);
void TapEnd();
// The game's per-light evaluation (LevelLightShare's LightEvalHook, depth 1) before it evaluates `light`
void OnEval(uintptr_t light);
// The room lamp probe: the colour that evaluation leaves (Apex's tests done), before the game's own wall and object tests
void OnEvalOut(uintptr_t light, const float* colour);
// The room lamp probe: Apex's path for that evaluation (0 the room's own lamp, 1 InnerShadow, 2 an outdoor lamp through an
// opening, 3 CrossFloorShadow, 4 IndoorShadow) and the colour before it (after MountedNdl, the floor cut, RoofedDay, reach)
void OnEvalPath(uintptr_t light, int path, const float* colour);
// The room lamp probe: what each of Apex's steps in LevelLightShare's LightEvalHook did to one evaluation, for an offline
// replay (docs/features/night-lighting/gpu-room-lamps-probe.md, the "ev" tokens of the dump). -1 = not reached.
struct ProbeEval {
    uintptr_t light = 0;
    uint8_t set = 0;            // the lamp's set in the room's record: 0 none, 1 inner, 2 same (outdoor through openings), 3 cross
    uint8_t cut = 0, early = 0; // the floor-under-this-story cut returned; the threshold early-out returned
    uint8_t has = 0;            // 1 InnerShadow, 2 the opening, 4 IndoorShadow, 8 the soft light cut ran
    float c0[3] = {};           // the game's evaluation
    float mount = 1.0f, day = 1.0f, reach = 1.0f; // the factors applied (1: none)
    float p6[4] = {-1.0f, -1.0f, -1.0f, -1.0f};   // InnerShadow: pass, its norm scale, the source room's +0x160, this room's
    // the opening: pass, tOpen, times, law, gain, total, cut, k, q, thrScale
    float p7[10] = {-1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f, -1.0f};
    float p8[3] = {-1.0f, -1.0f, -1.0f}; // IndoorShadow: mine, the slabs' share, the walls' transmission
    int8_t why = -1, stage = -1;         // PassWhy, PassStage (the test that stopped the ray)
    uint8_t nseg = 0, entry = 0;
    float ent[3] = {};                   // where the game's own wall test then starts (entry = 1)
    struct Seg {
        int8_t story = -1;
        uint8_t passed = 255; // 255 = not tested
        int16_t room = -1;
        float t0 = 0.0f, t1 = 0.0f, keep = -1.0f;
    } seg[8];
    float fade = 1.0f, fadeThr = -1.0f; // the soft light cut's factor and the threshold it used
    float preFade = -1.0f, thrScale = 1.0f; // r + g + b of the colour before the soft light cut, and its threshold's scale
    int32_t tf = -1; // the opening's outdoor total: the fill it took (its TF line in the dump, ProbeTotalLine), -1 none
};
// After the game's evaluation (c0) of `light` in a probed room's own sum: the record LightEvalHook fills, null otherwise
ProbeEval* ProbeEvalBegin(uintptr_t light, const float* c0, int set);
// The room lamp probe: one fill of the opening's outdoor total (LevelLightShare's OpenTotalFill) in a probed room's sum, as
// the dump's TF line after its index; the index it is given (-1: not probing, or past the solve's cap)
int ProbeTotalLine(const std::string& body);
// The room lamp probe: the game's 2D wall test of that evaluation (passed = not skipped; t its transmission)
void OnWallTest(bool passed, float t);
// The room lamp probe: the game's own wall test inside it, before Apex's overrides (from = the point the ray starts at)
void OnWallTestRaw(const float* from, bool ok, float t);
// The floor mask's fused copy took `light`'s no-wall-test colour: its sum before and after
void OnFuse(uintptr_t light, const float* before, const float* after);
void DropFused(); // the fused copy is not used (the copy is solved apart): the parts it gave go
// The floor mask of the sample: masked = an outdoor floor texel of a story >= 1; gated = more than 2 m over its lot base
// (the share of lamps under its plane applies); visLum / allLum = the luminance of its two copies
void Mask(bool masked, bool gated, float visLum, float allLum);
// (GPU room lamps) the capture keeps room 0 lamps (KeepsGround): a story-0 ground texel the exit light was tapped at, on = not
// under a floor tile (its alpha is the exit lamps' light)
bool KeepsGround();
void ExitGround(bool on);
// After the sample's point solve (out = what the batch packs from; flags = the batch's test switches). The texel the batch
// then packs is read back from its map at the next sample, before the wall blur and before the finalize.
void SampleEnd(unsigned char* room, const void* sample, const char* flags, const float* out);
// Before the game's finalize unlocks the maps (FinalizeHook)
void BeforeFinalize(unsigned char* room);
// The class-2 wall blur (LevelLightShare BlurWalls, before the game's blur): the room's blocks as the solve left them; the
// block with rows lit beyond its edges (ghosts) cannot be blurred again without them
struct WallBlockView {
    int x0, y0, x1, y1;
    const uint32_t* before; // (x1 - x0 + 1) x (y1 - y0 + 1), null when not copied
    bool ghosts;
    const uint32_t* ghost; // those rows, 4 of x1 - x0 + 1: 2 above the top row, then 2 below the bottom one (null: none)
    bool above, below;     // which of them BlurWalls takes
};
void NoteWallBlur(const unsigned char* room, const WallBlockView* blocks, int count, uint32_t passes);
void NoteGameBlur(const unsigned char* room); // the game's own blur ran (Apex's could not): walls cannot be blurred again

// ---- Lighting scene recordings (scene_record.h, tools/scene_replay; Recorder) ----
// Opens the recording at `temp` (render thread, the Recording capture's start); SceneEnd closes it and moves it to `to` (empty:
// dropped), returning the room solves written
bool SceneBegin(const std::filesystem::path& temp);
long SceneEnd(const std::filesystem::path& to);

// ---- a changed light alone (LevelLightShare::ReplayBatch, light tree thread) ----
bool Replaying();
void ReplaySample(const void* sample, const float* out, bool masked, float visLum, float allLum);

// ---- Real-time outdoor lamps (rt_outdoor_lamps.h; any thread) ----
// A room 0's kept solve of one class, for the GPU relight: every texel the solve wrote in its solve order (its map, place,
// world position and normal from the batch sample) with its sums less the parts of the lamps the GPU draws (RtLampModel::
// Drawn when their parts were first kept), the floor mask's two luminances less theirs, what the map holds there (the
// packed texel; a class-2 wall before the blur), and the class-2 wall blocks as the solve left them before the blur
struct Room0Export {
    uint32_t version = 0;
    int story = 0, cls = 0;
    uint64_t lot = 0;
    float thr = 0.0f;
    bool dither = false, wallsOk = true;
    uint32_t passes = 0; // the class-2 wall blur's passes (0: none)
    bool mapUsed[4] = {};
    uint32_t mapW[4] = {}, mapH[4] = {};
    struct Texel {
        float pos[3], nrm[3];
        float base[3];
        float vis, all;
        uint32_t packed;
        uint16_t x, y;
        uint8_t map;  // 0 wall atlas, 1 floor, 2 ceiling, 3 objects
        uint8_t bits; // 1 masked (an outdoor floor of a story >= 1), 2 gated (over its lot base: the under-floor share applies), 4 exit ground,
                      // 8 (physical) its batch ran the game's 3D test
        // (GPU room lamps, bit 4: a story-0 ground texel whose alpha is the exit lamps' light, set only when a kept lamp is an
        // exit lamp) the luminance of the exit lamps' light left in the base
        float exitBase;
    };
    std::vector<Texel> texels;
    struct Block {
        int x0, y0, x1, y1;
        bool ok;
        uint32_t off; // into preblur (ok blocks)
    };
    std::vector<Block> blocks;
    std::vector<uint32_t> preblur;
    std::vector<uintptr_t> drawn; // the lights whose parts left the base
    std::vector<uintptr_t> kept;  // every light with parts kept (the others' parts stay in the base)
    std::vector<uintptr_t> phys;  // (physical) the lot lamps with a GPU law whose parts left the base
    // (physical) the lamps of phys left the base (else why not: the solve was kept by the other model, or kept room 0 lamps);
    // per lamp of phys its room row (mult x norm scale, 1 the realistic reach share applies, 1 an exit lamp); room 0's 3D test
    // records as its solve started (blkHeader < 0: none read)
    bool physical = false;
    const char* physWhy = nullptr;
    std::vector<std::array<float, 3>> physRow;
    std::shared_ptr<const RtLampGpu::BlockerTable> blockers; // (the record's own: one for every export of its solve)
    float blkHeader = -1.0f;
    int lamps = 0;                // lights kept
    // (GPU room lamps, developer) the lamps of its list Real-time outdoor lamps does not draw, kept as a kept room's: their parts
    // are out of the base, and are added back from A and B (rt_lamp_model.h KeptAdd) with the lamp's law when kept, at the
    // sample's world position and normal (pos, nrm above)
    struct KeptPart0 {
        uint32_t texel; // into texels
        float A, B, thrScale;
        uint8_t mult, bits; // bits: 1 the early-out, 4 the soft light cut
    };
    struct KeptLamp0 {
        uintptr_t light = 0;
        bool colourOnly = false; // its law is its colour alone (a TubeLight without a segment)
        uint64_t geo = 0;        // (colourOnly) the signature of what its share reads
        float pos[3] = {};       // where it was
        GpuLampLaw::Lamp law;    // its law when kept
        std::vector<KeptPart0> parts;
        bool exit = false; // its parts are also the exit ground texels' alpha (bit 4)
        struct Mask0 {
            uint32_t texel; // into texels
            float vis, all; // the floor mask's luminances over its law's then with N.l ignored (rt_lamp_model.h KeptWrapLum)
        };
        std::vector<Mask0> mask;
    };
    std::vector<KeptLamp0> kept0;
};
// 0 = nothing kept for that room and class; baseOnly: the kept solve is a lot next to the active one's (SetNeighbourLots)
uint32_t RecordVersion(const void* room, int cls, bool* baseOnly = nullptr);
// physical: every lot lamp with a GPU law (not drawn by Real-time outdoor lamps) leaves the base too (its sum, floor mask vis /
// all and exit light), listed in out.phys with its room row (out.physical; a solve kept by the other model: none, physWhy)
bool ExportRoom0(const void* room, int cls, Room0Export& out, bool physical = false);
// The active lot's room 0s (render thread, once a frame): the memory budget never drops them; past kMaxPinnedBytes a new
// solve of one is not kept instead, and their drawn lamps' parts are kept as one sum per texel. Returns how many of their
// solves the budget has not kept so far (logged with their sizes)
long PinRooms(const void* const* rooms, int n);
// The lots next to the active one whose room 0s Real-time outdoor lamps relights with the active lot's lamps (render thread,
// every frame; at most 4, n = 0 clears): a solve of one of their room 0s keeps only its texels and their sums (every light of
// its list kept, none drawn, no light's part), so ExportRoom0's base is the whole sum and a delta solves the room in full.
// Never pinned: the memory budget drops them first, and they make room only for one another
void SetNeighbourLots(const uint64_t* lots, int n);
size_t KeptBytes();
// What the kept parts are made of, and what the budget and the left-lot sweep dropped (the memory report and its log line)
std::string MemoryText();
size_t PinnedBudgetBytes(); // the active lot's ceiling (Developer > Tester readout)
int KeptRooms();
// The last time a story's room 0 of that class lost its kept parts or had none kept, and why ("" when none was noted)
std::string LastDrop(int story, int cls);

// ---- render thread ----
void OnPresent();
void OnWorldChanged();
void InvalidateAll(const char* why); // options that change how rooms are lit (every room lights again)
std::string Status();

} // namespace OutdoorDelta

namespace LevelLightShare {
// The room lamp probe's dump (level_light_share.cpp): the room's solve state, its record's lamp sets, and the story grids and
// floors its lamps' tests read (each block once per content, then referred to by its hash)
std::string ProbeRoomText(const unsigned char* room, uintptr_t tracker, float normStart);
} // namespace LevelLightShare
