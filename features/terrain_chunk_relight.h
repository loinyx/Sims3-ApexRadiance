#pragma once
// Local terrain relight (part of Night Lights, driven by patches/night_terrain_relight_patch.cpp): re-renders only the
// terrain chunks a lamp change touches, through the game's own one-chunk-per-terrain-update sweep branch (chunk+0x54),
// one chunk at a time and paced, instead of arming the full rebuild (chunk+0x55 on every chunk: a synchronous bake + DXT
// of every chunk in one frame, ~240 ms, then a duplicate sweep). Also the paced sweep of every chunk (phase 2) that can
// replace Apex's own dusk and lamp-change full rebuilds. See terrain_chunk_relight.cpp and
// docs/features/night-lighting/terrain-relight.md "Local terrain relight".
//
// Render thread only (OnPresent and the game's terrain update, which run on the same thread).
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ChunkRelight {

// Install (after GameAddr::Resolve): the WorldManager global and the terrain link. Logs once what it found.
void Init();
// The chunk re-render call 0x00C8504C is redirected (ChunkRenderThunk). Without it nothing is ever released.
void SetHooked(bool hooked);

// From ChunkRenderThunk, right after the game's 0x00C7E7A0 returned: `rendered` = chunk+0x54 is 0 again (the render ran),
// `ticks` = QueryPerformanceCounter ticks the call took.
void OnChunkRendered(void* terrain, const void* chunk, bool rendered, int64_t ticks);

// A new world (cells pointer changed): forget the terrain, the queue and the per-world state.
void OnWorldChanged();
// The game consumed a full rebuild (+0x55 on every chunk): the queue is dropped (every chunk is re-rendered anyway). The
// first one of a world is the load rebuild: the local path needs it (chunks without a rebuilt light map, borders).
// Returns true when a queue was dropped.
bool OnFullRebuild();
// Drops the queue (the chunk in flight, if any, is left to the game: it re-renders it at its next terrain update).
// Returns true when something was dropped.
bool Drop();

// One changed lamp: where it stands (its own chunk is relit first) and its old and / or new light rect +0x134.
struct Lamp {
    float x = 0.0f, z = 0.0f;
    int rects = 0;
    float rect[2][4] = {}; // {minX, minZ, maxX, maxZ}
};

// Cheap check (flags only, no game memory read): the prerequisites that do not change frame to frame hold.
bool LikelyAvailable();
// Render thread: whether `texture` is the rebuilt light map +0xD8 of the chunk centred at (centreX, centreZ): the byte
// offset in +0xD8's object where it was found (0: +0xD8 is the texture itself), -1 when it is not, -2 when the chunk or its
// rebuilt map cannot be read (none yet: only the world file's map exists)
int RebuiltLightMapMatch(int centreX, int centreZ, const void* texture);
bool Editing(); // resolved WorldManager mode 2 (editInGameMode); unknown modes return false
// Queues the chunks whose bake rect overlaps a lamp rect (+1 m), each lamp's own chunk first, then its neighbours.
// Returns a batch id > 0, or 0 with `why` when the caller must use the full rebuild: terrain or grid layout not as
// studied, the world's first rebuild not seen yet, a chunk without a rebuilt light map, more than 16 chunks (or a
// quarter of the world), the terrain not ready. `chunks` lists "(ix,iz) ..." for the log, `cells` (optional) the same.
int QueueLocal(const std::vector<Lamp>& lamps, std::string& why, std::string& chunks, bool urgent = false,
               std::vector<std::pair<int, int>>* cells = nullptr);
// Phase 2: queues every chunk, nearest to (x, z) first (eye = nullptr: grid order); drops a pending local queue (the
// sweep covers it). An older in-flight bake is repeated for the new state.
// Interactive Build preview prioritizes at most four nearest chunks with a known
// eye; the measured-cost reserve and render gates are unchanged.
// 0 with `why` when not possible (as above, and any chunk without a rebuilt light map).
int QueueSweep(const float* eyeXZ, std::string& why, std::string& info, bool interactive = false);
// A batch is queued or in flight
bool Busy();

// Paced full rebuild (item 13b, 05/10): the game's own full rebuild (chunk+0x55: geometry, light map bake, road mark, the
// later +0x54 render) one chunk at a time, nearest to (x, z) first, instead of every chunk in one ~240 ms frame. The
// game's consume (0x00C84C3E..0x00C84C5E) only resets the countdown and sets +0x55 on every chunk, and its +0x55 branch
// clears the flag when done (0x00C850B1), so a chunk is finished when its +0x55 is 0 again. Unlike QueueSweep it needs
// no earlier rebuild of this world (it is the load rebuild) nor a light map (the bake creates it). Drops the +0x54 queue;
// local relights and sweeps queued meanwhile wait until it is done. Its end reports Done{rebuild = true} and enables the
// local path like a consumed full rebuild. 0 with `why` when not possible: the caller arms the full rebuild instead.
int QueueRebuild(const float* eyeXZ, std::string& why, std::string& info);
bool RebuildActive();

struct Done {
    int id = 0;
    bool sweep = false;
    bool rebuild = false; // the paced full rebuild (QueueRebuild)
    std::string text; // "N chunks in K frames, M ms per chunk (max X)"
};
struct FrameResult {
    std::vector<Done> done;
    bool failed = false; // a chunk never rendered (timeout) or the terrain changed under the queue: the queue was dropped
    std::string why;     // and the local path is off for this world; the caller falls back to the full rebuild
};
// Per frame (render thread, after the lamp-change decisions): completion of the chunk in flight, timeout, release of the
// next chunk (at most one per frame, never two frames in a row unless urgent, 8 per second; urgent work after a measured
// chunk cost <= 12 ms has 4 more a second and goes back to back; never while any chunk has +0x55 /
// +0x56 set or the render would return early).
void OnPresent(FrameResult& out);

std::string Status();

} // namespace ChunkRelight
