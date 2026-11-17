#pragma once
// Per-pixel delta shadows (real-time GI work package XR-07; research 16 sections 3-4, wp/XR-06.md "Handoff: XR-07").
// [patches.RealtimeGI] pixelShadows (default off, developer keys until the menu stage). Off: no bridge export is looked up
// and no command is sent. On, with the bridge pair of patch 0019 on the DXVK of native/dxvk: every scene, the bridge
// traces the lamps' rays against the delta casters into quarter-resolution targets, XR-05's filter passes take them and
// a depth-aware upsample multiplies the back buffer. Any other renderer: "[GiPixel] off: <reason>" once, then nothing.
// Casters: three capsules for each Sim the scene drew (gi_sim_capsules.h, XR-08) and, with meshCasters, the objects'
// meshes, which the bridge server adds itself (MC-2).

namespace GiPixelShadows {

struct Options {
    int view = 0;            // 0 off, 1 the filtered mask, 2 the raw mask (output 0's r), on screen instead of the multiply
    bool testCaster = false; // developer proof: one capsule under the strongest lamp, beside the Sims' own (XR-08)
};

void Start(const Options& o); // RealtimeGI::Start
void Stop();

} // namespace GiPixelShadows
