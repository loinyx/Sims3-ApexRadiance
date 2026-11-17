# X-R0MEMO: which GI back end is primary

- Status: done (desk). Branch `gi/x-r0memo`.
- Deliverable: [research 17](../research/17-backend-decision.md).
- Acceptance: a recommendation, the plan and design changes it implies (proposed, not applied), the client-server
  surface and the minimal fallback; every fact names its package or is marked inferred or open.

## Findings

- Verified (wp/R-02): rays and no-ray compute are one kernel and agree within 2/255 on every texel, also on an
  integrated GPU. `serverRays` and `serverCompute` are levels of one back end, not two back ends.
- Verified (wp/S2-03, wp/S0-05): the Direct3D 9 passes fit `ps_3_0` and run on the bridge's DXVK, but the gather
  itself is not built, needs loops written around two compiler faults, and has no furniture occlusion or stairwell
  walk.
- Verified (`native/dxvk` on `feature/dxvk-patch-series`): DXVK is already built from source with a patch series,
  so R-08's packaging cost no longer counts against a DXVK-side entry for post-scene passes.
- Inferred: the server solve is primary; the Direct3D 9 back end shrinks to direct light plus a room ambient term;
  the texel solve stays on R-03's second device and per-pixel passes use DXVK's device. Triggers for moving the
  texel solve are in research 17 §3.3.
- Inferred: the client exports are the same for either server device, so R-05 does not wait on the device question.

## Open issues

- No timing behind any of it (R-G1, XR-09).
- The fallback's look with `roomAmbient` only is unjudged until S1-G1.
- plan.md and design.md are not edited; research 17 §6 and §7 list the edits for whoever owns the merge.

## Unblocks

G-server (decided: pass). R-04 and R-05 with the narrowed fallback. Parks S2-02, the S2-03 wiring, the Direct3D 9
parts of S2-04 and S3-04, and S5-04.
