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
#include <cstdint>
#include <string>
#include <vector>

namespace OutdoorDelta {

// Render thread, every frame: on = Lamp lighting is Instant and its parts (LevelLightShare's solve hooks) are in
void SetEnabled(bool on);
bool Enabled();
// Developer self-check: a room brought up to date by a delta is solved in full once more 1.5 s after its last delta, and
// the texels the full solve packs are compared with the ones the delta wrote (log: the largest difference in 8-bit levels)
void SetSelfCheck(bool on);

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
// One batch sample of the room's solve (SolvePointBatch): Sample returns true when its sums are kept
bool Sample(const unsigned char* room);
// The sum the next point solve adds to: 0 the room's own, 1 the floor mask's copy with walls tested, 2 its copy with none
void Tap(int which, const float* sum);
void TapEnd();
// The game's per-light evaluation (LevelLightShare's LightEvalHook, depth 1) before it evaluates `light`
void OnEval(uintptr_t light);
// The floor mask's fused copy took `light`'s no-wall-test colour: its sum before and after
void OnFuse(uintptr_t light, const float* before, const float* after);
void DropFused(); // the fused copy is not used (the copy is solved apart): the parts it gave go
// The floor mask of the sample: masked = an outdoor floor texel of a story >= 1; gated = more than 2 m over its lot base
// (the share of lamps under its plane applies); visLum / allLum = the luminance of its two copies
void Mask(bool masked, bool gated, float visLum, float allLum);
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
};
void NoteWallBlur(const unsigned char* room, const WallBlockView* blocks, int count, uint32_t passes);
void NoteGameBlur(const unsigned char* room); // the game's own blur ran (Apex's could not): walls cannot be blurred again

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
        uint8_t bits; // 1 masked (an outdoor floor of a story >= 1), 2 gated (over its lot base: the under-floor share applies)
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
    int lamps = 0;                // lights kept
};
// 0 = nothing kept for that room and class; baseOnly: the kept solve is a lot next to the active one's (SetNeighbourLots)
uint32_t RecordVersion(const void* room, int cls, bool* baseOnly = nullptr);
bool ExportRoom0(const void* room, int cls, Room0Export& out);
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
