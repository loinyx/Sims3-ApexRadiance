// The block a client sends with one per-pixel shadow pass (real-time GI work package XR-06, research 16 section 3.3).
// Plain 32-bit fields: the 32-bit client and the 64-bit server see one layout. Everything up to `box` is the shader's
// block as it stands (tools/gi_server_test/pixel_shadow.comp, BRIDGE_PASS).
#pragma once
#include <cstdint>

struct GiPixelLamp {
  float pos[3], radius;  // radius of the emitter ball
  float col[3], pad;
};
struct GiPixelCapsule {  // a delta caster: a Sim's limb
  float a[3], r;
  float b[3], pad;
};
struct GiPixelCaster {   // bounds of a group of delta casters; a pixel traces only when a lamp's rays can touch one
  float lo[3], moved;    // moved: not 0 when the group moved this frame; the bounds then hold the old and the new place
  float hi[3], pad;
};
struct GiPixelBox {      // a static occluder: the unit cube (0..1 on each axis) under this transform
  float m[12];           // three rows of four, object to world
};

enum : uint32_t { kGiPixelLamps = 16, kGiPixelCapsules = 64, kGiPixelCasters = 32, kGiPixelBoxes = 96, kGiPixelOutputs = 4 };
enum : uint32_t { kGiPixelFlagClass = 1 };
// Mesh casters (MC-2): static occluders with their model's own triangles, one bottom level a model, an instance an object.
// They do not travel in the block: the server's object table (RemixBridge_GiObjects) hands them to the pass.
enum : uint32_t { kGiPixelMeshes = 1024 };  // instances a pass at most: the nearest to the camera
// Mesh casters are delta casters (custom index 2, mask 2): a ray they stop darkens unless a static occluder is on it too.
// Their reach bounds (the instances grouped by the server, at most this many) follow the block's casters in the shader's
// copy; the block itself does not carry them (`meshBounds`, set by the server)
enum : uint32_t { kGiPixelMeshBounds = 32 };
struct GiPixelMesh {
  uint32_t model;  // GiPixelPass::addModel's index
  float m[12];     // three rows of four, model to world
};  // set by the server when a motion class texture came with the pass

struct GiPixelBlock {
  float invViewProj[16];   // Direct3D row-vector inverse of view x projection, as stored
  float cam[3], ambient;   // ambient: the light no lamp ray accounts for (A of research 16 section 4.1)
  uint32_t width, height;  // of the depth; set by the server
  uint32_t lamps, casters;
  uint32_t frame, samples; // samples: 1 a frame; 16 = the whole set in one pass
  float bias, radiusScale; // radiusScale 0: point emitters
  float viewZ[3], zFar;    // camera forward and far distance: the guide's linear depth is over zFar
  uint32_t capsules, boxes;
  uint32_t flags, meshBounds;  // both set by the server: flags kGiPixelFlag*, meshBounds the mesh reach bounds in use
  GiPixelLamp lamp[kGiPixelLamps];
  GiPixelCapsule capsule[kGiPixelCapsules];
  GiPixelCaster caster[kGiPixelCasters];
  GiPixelBox box[kGiPixelBoxes];  // not sent to the shader: they become instances of the top level
};
static_assert(offsetof(GiPixelBlock, lamp) == 144 && offsetof(GiPixelBlock, box) == 3728 && sizeof(GiPixelBlock) == 8336, "GiPixelBlock layout");

// Outputs, quarter resolution (depth size / 4), A16B16G16R16F render targets of the client; any may be absent.
//   0 raw:   r this frame's delta visibility (1 = no caster on the rays), g a caster that moved is in reach,
//            b rays were traced, a the guide's depth (the device's own, not linear)
//   1 guide: rgb world normal, a linear depth / zFar (1 = nothing drawn)
//   2 num:   rgb sum over the lamps of colour x geometry x blocked samples, a as raw.g
//   3 den:   rgb sum over the lamps of colour x geometry x samples that reached the lamp, a rays traced
struct GiPixelCounters {
  uint32_t passes, faults;   // faults: passes refused or failed
  uint64_t rays;             // since the pass was created
  uint32_t raysLast, gpuUs;  // of a recent pass (the device answers a few passes late)
  uint32_t width, height;    // of the outputs
  uint32_t meshes, models;   // mesh instances in the last pass; models with a bottom level on the device
};
