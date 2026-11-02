#pragma once
// GPU room lamps stage 3 (developer; docs/features/night-lighting/gpu-room-lamps.md "Lamp moves"): one kept part of a lamp
// moved within its room, at a kept texel, from copies only (any thread; outdoor_delta.cpp runs it on its move worker,
// tools/apex_paths_replay over the probe's dump at the recorded positions):
//   A = what Apex's path of a plain lamp (not one of the room record's inner, opening or cross lamps; no outdoor lamp, no
//       reach, the lamp over its story's floor) does to the law before the soft light cut: MountedNdl's factor as
//       LevelLightShare's LightEvalHook applies it (the threshold early-out and the soft light cut stay the draw's, KeptAdd);
//   B = the game's 2D wall test from the lamp and its 3D test (room_light_tests.h GameShare) with the batch's switches and
//       wall list.
#include <algorithm>
#include <cstdint>
#include <vector>
#include "fast_math.h"
#include "gpu_lamp_law.h"
#include "room_light_tests.h"

namespace RoomLampMove {

// The factor LightEvalHook raises a lit colour by on a wall texel (p, n) a lamp at L hangs on (1: none)
inline float MountFactor(const float* p, const float* n, const float* L) {
    if (!(ApexMath::Fabs(n[1]) < 0.5f)) return 1.0f;
    const float l[3] = {L[0] - p[0], L[1] - p[1], L[2] - p[2]};
    const float plane = n[0] * l[0] + n[1] * l[1] + n[2] * l[2];
    if (!(plane > 0.0f && plane < GpuLampLaw::kMountReach)) return 1.0f;
    const float d = ApexMath::Sqrt(l[0] * l[0] + l[1] * l[1] + l[2] * l[2]);
    const float ndl = d > 1e-4f ? plane / d : 1.0f;
    const float raised = GpuLampLaw::MountedNdl(ndl, plane, n[1]);
    return raised > ndl && ndl > 1e-3f ? std::min(raised / ndl, 8.0f) : 1.0f;
}
// The solve built the per-light wall lists for a batch (0x006A31D0: 4 samples or more, a test on)
inline bool ListsBuilt(uint32_t count, bool f0, bool f1) { return count >= 4 && (f0 || f1); }
// A texel's A and B for a lamp at L (list: the batch's walls wallCull gave from its centroid, when built)
inline void PartAt(const RoomLightTests::Room& r, bool built, const std::vector<int>& list, bool f0, bool f1, const float* L, const float* p, const float* n,
                   float& A, float& B, int* unknownVt) {
    const RoomLightTests::V Lv{L[0], L[1], L[2]}, pv{p[0], p[1], p[2]};
    A = MountFactor(p, n, L);
    B = RoomLightTests::GameShare(r, built, list, f0, f1, Lv, Lv, pv, unknownVt);
}

} // namespace RoomLampMove
