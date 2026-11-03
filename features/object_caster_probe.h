#pragma once
// Object caster probe (development of real object shapes in the physical indoor lamps' shadow tiles): while a Recording
// runs with the physical model on, the rig-lit object draws are sampled and summed every 10 s in one log block, to tell
// whether their meshes can be replayed into a cube shadow tile (position format and decode, world rows, vertex buffer pool
// and a read-only lock, alpha test of foliage, the bytes a cache would hold). Render thread only; reads state, changes none.
#include <d3d9.h>

namespace ObjectCasterProbe {
bool Wanted(); // a Recording runs and the physical model is on
// A draw of a rig-lit object vertex shader (LotLightBridge's draw hook, before the bridge): worldK = its world rows' first
// constant (-1 unknown), onLot = its position lies on a lot
void OnObjectDraw(IDirect3DDevice9* dev, IDirect3DVertexShader9* vs, int worldK, bool onLot, INT baseVertex, UINT minV, UINT numV, UINT start, UINT prims);
void OnPresent(); // the per-frame sample budget, and the 10 s summary
} // namespace ObjectCasterProbe
