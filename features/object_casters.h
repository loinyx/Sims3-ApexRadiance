#pragma once
// Object casters (physical indoor lamps, "Object shadows: Real shapes"; docs/features/night-lighting/object-shadows.md): the
// lot's furniture and objects drawn with their own meshes into the lamps' cube shadow tiles, instead of the game's 3D-test
// shapes (a plant's disc, a table's one quad). LotLightBridge's early draw hook hands every rig-lit object draw of a lot; each
// mesh (vertex range of a MANAGED buffer, short4 positions decoded xyz / w) is copied once, each object instance kept with its
// world rows, and the instances become one triangle list in the lot's space that the tiles draw after the walls (same depth,
// same softness). A blocker record that holds a known instance is left out of the 3D test. Render thread only.
#include <d3d9.h>
#include <cstdint>
#include <memory>
#include <string>

namespace RtLampGpu {
class Engine;
struct BlockerTable;
} // namespace RtLampGpu

namespace ObjectCasters {
void SetRealShapes(bool on); // the option (night_terrain_relight_patch, every frame)
bool Active();               // Real shapes and the physical model on
// A rig-lit object draw (LotLightBridge's draw hook, before the bridge): worldK = its world rows' first constant, onLot = on a lot
void OnObjectDraw(IDirect3DDevice9* dev, IDirect3DVertexShader9* vs, int worldK, bool onLot, INT baseVertex, UINT minV, UINT numV, UINT start, UINT prims);
void OnPresent(); // the frame's end: instances in view but not drawn expire, the lot change clears, the 10 s summary
void SetLotXf(const float xf[16]); // the active lot's world -> lot rows (gpu_room_lamps Entry::xf) the triangle list is made in
uint32_t Gen();                    // changes when an instance appears, moves (a new one) or goes; 0 while not active
// Any instance appeared or went within r of lotPos (lot space) since generation `since` (true when the record of it is gone)
bool ChangedNear(uint32_t since, const float lotPos[3], float r);
// The instances into a lamp's tile after its walls (RtLampGpu::Engine::RenderCasters); those whose lot box holds the lamp
// (its own object) left out. True with nothing to draw
bool DrawIntoTile(IDirect3DDevice9* dev, RtLampGpu::Engine& eng, const float lotPos[3], int tile, float farDist, std::string& err);
// t without the records a known instance stands in (their blockers' count 0), kept per table and generation; t itself
// while not active or nothing is left out
std::shared_ptr<const RtLampGpu::BlockerTable> Filter(const std::shared_ptr<const RtLampGpu::BlockerTable>& t);
void OnDeviceLost(); // the DEFAULT triangle list
} // namespace ObjectCasters
