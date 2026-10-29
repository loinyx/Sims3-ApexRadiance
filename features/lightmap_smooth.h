#pragma once
// Smoothed terrain light maps (part of Night Lighting, see lightmap_smooth.cpp).
#include <d3d9.h>
#include <string>
#include <utility>
#include <vector>

namespace LightmapSmooth {
using Key = std::pair<int, int>; // chunk centre (x, z), same key as the lot light bridge

void SetEnabled(bool on);
bool Enabled();
// Render thread. Registers the game's light map of a chunk (seen in a world terrain draw) and returns its smoothed
// version, or nullptr while no smoothed map of the game's CURRENT map exists (then the draw keeps the game's map:
// correct first, smooth when ready). GPU path: a changed map is smoothed right here, before the draw (same frame).
// Get, Find and Atlas(forDraw) may draw on the GPU path: call them from a draw hook, before changing device state.
IDirect3DTexture9* Get(const Key& key, IDirect3DTexture9* original);
// Smoothed version of an already registered chunk (same rule: nullptr while it would be stale).
IDirect3DTexture9* Find(const Key& key);
// World light atlas (all chunk maps, 2 texels/m) and its mapping: uv = world xz * c.xy + c.zw. nullptr if not ready.
// CPU path: a chunk whose map changed holds a plain 2x copy of the game's current map until its smoothed map is ready.
// GPU path: changed chunks are smoothed into their cells before the draw that asks (forDraw = false: no GPU work, e.g.
// from a status line).
IDirect3DTexture9* Atlas(float c[4], bool forDraw = true);
// Developer A/B (dev-only setting): smooth on the GPU when possible (default), else on the CPU worker. Applied at the
// next OnPresent (switching drops every smoothed map; the new path rebuilds them). The GPU path falls back to the CPU
// by itself when shaders / formats / render targets are not available (logged, shown in Status).
void SetGpuPreferred(bool on);
bool GpuActive();
// Developer: compare the GPU result with the CPU result for one chunk in view (one-off read back; result in the log and
// in Status / CompareStatus).
void RequestCompare();
std::string CompareStatus();
void OnPresent(IDirect3DDevice9* dev); // render thread, every frame: detects changed maps, uploads finished ones
void OnPreReset(IDirect3DDevice9* dev);
void Clear();
std::string Status();

// ---- driven by the terrain relight (night_terrain_relight_patch.cpp), render thread ----
// A rebuild was armed (timing for the developer status and log).
void NoteKick(const char* reason);
// The game consumed a rebuild this frame: every chunk map will be re-rendered over the next frames (one per frame).
// Chunks are checked faster, smoothing waits for each chunk's own re-render, neighbour re-smooths wait for the end.
void OnTerrainRebuilt();
// A rebuild is imminent (armed, or waiting for the world to be live): do not start smoothing jobs for `frames` more
// frames (call every frame while it holds). The game's maps are shown meanwhile (they are correct).
void ExpectRebuild(int frames);
// Called from the game's per-chunk texture re-render (FUN_00C7E7A0 call site, render thread): chunk grid index.
void NoteChunkRendered(int ix, int iz);
// A lamp switch's local relight (batch id, the chunks' grid indices): those chunks keep showing their smoothed light from
// before the switch until every batch held is released (or after 1.5 s), then all of them, with their borders and atlas
// cells, change in the same frame. GPU path only; a chunk with no smoothed map yet is not held.
void HoldChunks(int batch, const std::vector<std::pair<int, int>>& cells);
// Dragged lamps (TerrainStamps): the chunks a rect {minX, minZ, maxX, maxZ} touches (grown by 1 m, as the local relight)
// are built again with the lamps' stamps; and those chunks with their re-render counts (a same map counts)
void BumpStamps(const float rect[4]);
std::vector<std::pair<Key, uint64_t>> ChunkVersions(const float rect[4]);
// That batch is done (0: every batch, e.g. the queue failed)
void ReleaseHold(int batch);
// A lamp switch's chunks are still being re-rendered (a switch shown all at once waits for them, AtriumHold). Render thread.
bool HoldPending();
}
