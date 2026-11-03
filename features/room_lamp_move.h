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
#include "lamp_reach.h"
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
// The game's evaluation over the GPU law at a texel (p, n), for what the law adds of its own: on a wall texel near the lamp the law
// scales the game's evaluation by the hook's raise (gpu_lamp_law.h MountScale) where the game's evaluation does not (LightEvalHook
// raises it after, MountFactor), so an A over the game's evaluation is taken over the law as A x 1 / that scale; 1 where the law
// raises nothing. A tube's and an area light's law is the game's evaluation (gpu_lamp_law.h Tube, Area): 1.
inline float GameOverLaw(const GpuLampLaw::Lamp& L, const float* p, const float* n) {
    if (GpuLampLaw::Kind(L)) return 1.0f;
    float l[3] = {L.pos[0] - p[0], L.pos[1] - p[1], L.pos[2] - p[2]};
    const float plane = n[0] * l[0] + n[1] * l[1] + n[2] * l[2];
    const float d2 = std::max(l[0] * l[0] + l[1] * l[1] + l[2] * l[2], 1e-4f);
    const float inv = 1.0f / ApexMath::Sqrt(d2);
    for (float& v : l) v *= inv;
    return 1.0f / GpuLampLaw::MountScale(GpuLampLaw::Sat(n[0] * l[0] + n[1] * l[1] + n[2] * l[2]), plane, n[1]);
}
// (stage 3b) Realistic lamp reach's share of a lamp at L (w: range x intensity) on a texel (p, n), as LevelLightShare's
// ReachShareImpl gives it in room 0's solve (wall texels only; 1 with the option off)
inline float ReachAt(bool on, float w, const float* L, const float* p, const float* n) {
    if (!on || !(ApexMath::Fabs(n[1]) < 0.5f)) return 1.0f;
    float r0, R;
    LampReach::Radii(w, r0, R);
    const float dx = p[0] - L[0], dy = p[1] - L[1], dz = p[2] - L[2];
    return LampReach::Share(ApexMath::Sqrt(dx * dx + dy * dy + dz * dz), r0, R);
}
// (stage 3b) Room 0's part of a lamp its record takes through an opening of its own story (LightEvalHook in room 0's solve: no
// floor cut, no daytime curve, then InnerShadow), at a texel (p, n) for the lamp at L, from copies. A over the game's evaluation
// (gameSum: its r + g + b there; the law x GameOverLaw stands for it): MountFactor x reach (ReachAt); the threshold early-out on
// that colour returns false (no part); then pass x normK where pass > 0 (pass(): InnerPass at L, LevelLightShare::Room0PassAt,
// asked only past the early-out). B: the game's wall test is skipped for such a lamp (GameWallTest), so its 3D test alone when
// f1, the product dropped under K_DROP
template <class Pass>
inline bool Part0At(const RoomLightTests::Room& r, bool f1, const float* L, const float* p, const float* n, float gameSum, float thr, float reach, float normK,
                    Pass&& pass, float& A, float& B, int* unknownVt) {
    A = MountFactor(p, n, L) * reach;
    if (thr > gameSum * A) return false;
    const float ps = pass();
    A *= ps > 0.0f ? ps * normK : 0.0f;
    const RoomLightTests::V Lv{L[0], L[1], L[2]}, pv{p[0], p[1], p[2]};
    static const std::vector<int> s_none;
    B = RoomLightTests::GameShare(r, false, s_none, false, f1, Lv, Lv, pv, unknownVt);
    return true;
}

} // namespace RoomLampMove
