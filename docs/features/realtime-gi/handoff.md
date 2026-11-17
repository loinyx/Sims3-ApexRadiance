# Real-time GI: work package handoff

The running record of the work in [plan.md](plan.md). One worker takes one work package, and this page is how the
next worker learns what happened without reading anyone's session.

## Protocol

1. **Read** [design.md](design.md), then the package's row in [plan.md](plan.md) and its inputs. Read this page's
   entry for every package the row depends on.
2. **Claim** the package: set its status to `in progress` in the table below, in its own commit, before starting.
   A package that is `in progress` is not taken by anyone else.
3. **Work** inside the package's scope. One deliverable, one acceptance check. A needed change outside the scope is
   written under *Open issues*, not done.
4. **Report** under the package's id in *Results*, using the entry format below, in the same commit as the
   deliverable.
5. **Update the design** when the package closes an open mark: edit the mark in design.md in the same commit and
   name the section in the entry.
6. **Unblock:** list the packages that can start now. If the result changes the plan (a gate fails, a package
   becomes unnecessary, a new one is needed), edit plan.md in the same commit and say so.
7. **Check** the changed files with `python tools/public_text_check.py --files <paths>` and commit as
   `area: imperative summary` on `feature/realtime-gi`.

Rules:

- The game is launched only by `game` packages, through the harness job named in the plan.
- A `game` package records the tested commit, the job, the arms and where the result files are; a cancelled or
  noisy run is repeated, not reported.
- Facts are marked verified, inferred or open. A verified fact names its file, function, address or capture.
- Statuses: `open`, `in progress`, `done`, `blocked` (say by what), `dropped` (say why).
- An entry is short: results and decisions, not a diary. Investigation detail goes to a research or engine page.

### Entry format

```
### <id>: <title>
- Status: done | blocked | dropped
- Commits: <hash> <subject>
- Deliverable: <paths>
- Acceptance: <the check and its result, with numbers>
- Findings: <facts, each marked verified / inferred / open>
- Design changes: <section and mark changed, or none>
- Open issues: <what is left, who could take it>
- Unblocks: <ids>
```

## Status

Merged into `feature/realtime-gi`: S0-01 to S0-14, S1-01, S1-02, S2-03, S3-02, R-01, R-08, R-02, GB-01, X-RTPP, X-R0MEMO, XR-04 and XR-05;
in merge round 3 also R-03, R-04, X-REBAKE, XR-06, XR-07, MC, MC-2, MC-3, R-06, S2-01, S2-01b, X-05, PLAN-SEED, S3-02b, S3-02c,
GB-02, GB-03, GB-04 and the const-range fix (see *Merge round 3* below). Each package's full report is `wp/<id>.md`.
Not merged yet (still being built at round 3): `gi/xr-08`, `gi/gb-05`.

| Id | Status | Branch | Commit | Depends on | Unblocks / notes |
|---|---|---|---|---|---|
| S0-01 | done (synthetic stories; captured-lot run open) | gi/s0-01 | 50a4007 | - | S0-17, S2-05, S2-01, S0-02 and R-02 comparisons |
| S0-02 | done (CPU path-traced reference) | gi/s0-02 | 6169415 | - | R-02 parity comparison, S0-17, S6-01 |
| S0-03 | done (static; one capture check open) | gi/s0-03 | ea72756 | - | S1-05 (with S0-04) |
| S0-04 | done | gi/s0-04 | b6593a4 | - | S1-05, S3-04 |
| S0-05 | done | gi/s0-05 | 8486eb8 | - | S2-02, S3-02 |
| S0-06 | done | gi/s0-06 | 4b3ef0f | - | S2-03, X-05 |
| S0-07 | done (synthetic stories; captured lots open) | gi/s0-07 | 749593f | - | S2-01 (with S0-01) |
| S0-08 | done (static; tunable values open) | gi/s0-08 | 3185975 | - | S4-01 (with S2-01) |
| S0-09 | done (static) | gi/s0-09 | e8e1d39 | - | S3-03, S3-05 |
| S0-10 | done (static) | gi/s0-10 | a786574 | - | S1-03, S1-06 |
| S0-11 | done | gi/s0-11 | f3d9852 | - | S0-G1 (with S0-13) |
| S0-12 | done (static RE) | gi/s0-12 | 042b253 | - | R-02/R-04 reflectance input, S3-03 strobe exemption, S6 albedoBoost |
| S0-13 | done (no S0-14 job yet) | gi/s0-13 | 4b6dc60 | - | S0-G1, S1-G1, S1-G2, S2-G1, S0-G3 |
| S0-14 | done (const-range-smoke job; game run open) | gi/s0-14 | 89fa91c | - | S0-G2 |
| S0-15 | open |  |  | - | can start |
| S0-16 | in progress | gi/s0-16 |  | - |  |
| S0-17 | open |  |  | S0-01 | can start |
| S0-G1 | open |  |  | S0-11, S0-13 | game run: `realtime-gi-profile`, can start |
| S0-G2 | open |  |  | S0-14 | game run |
| S0-G3 | open |  |  | S0-15 | game run |
| R-01 | done | gi/r-01 | f353213 | - | R-03; entry points for R-07, R-08 |
| R-02 | done (synthetic lot; captured lot and S0-02 parity open) | gi/r-02 | b1d8932 | - | R-04, G-server, S0-02 comparison |
| R-03 | done (GI server prototype, bridge patch 0018) | gi/r-03 | ad7360b | R-01 | R-04 |
| R-04 | done (texel solver, scene submit; patch 0018 extended) | gi/r-04 | fbe80d1 | R-02, R-03 | R-06, S2-01, R-05 |
| R-05 | open |  |  | R-03, S2-01 |  |
| R-06 | done (self-test: point queries, touch boxes, region-scoped dispatch; patch 0021 since round 3) | gi/r-06 | d65a017 | R-04 | S3-05, S2-01b |
| R-07 | open |  |  | R-04, S0-G2 |  |
| R-08 | done (verdict: no-go now) | gi/r-08 | 94953b4 | - | nothing waits on it |
| R-G1 | open |  |  | R-05, S2-01 | game run |
| S1-01 | done | gi/s1-01 | ec9662b | - | S1-02, S1-03, S2-01, S3-01 |
| S1-02 | done (lamp snapshot; job realtime-gi-lamps) | gi/s1-02 | c032889 | S1-01 | S1-04, S3-03 |
| S1-03 | open |  |  | S1-01, S0-10, S0-G1 | waits on S0-G1 |
| S1-04 | open |  |  | S1-02 |  |
| S1-05 | open |  |  | S1-04, S0-03, S0-04 | waits on S1-04 |
| S1-06 | open |  |  | S1-03 |  |
| S1-07 | open |  |  | S1-04 |  |
| S1-G1 to S1-G3 | open |  |  | S1-03, S1-04 | game runs |
| S2-03 | done offline; shaders merged, Apex wiring parked with S2-02 | gi/s2-03 | c115de7 | S0-06 | X-05 |
| S2-02 | parked (research 17: server primary, d3d9 = fallback only) |  |  |  |  |
| S2-04 | split: feedback bounces are R-04; only `roomAmbient` (d3d9 fallback) remains |  |  | S2-01 |  |
| S2-01 | done (layer scheduler, gi_sched_test, gi-sched-smoke job) | gi/s2-01 | fa06237 | R-04 | S2-01b |
| S2-01b | done offline (scene builder, compose, edit box; touch-box scope through patch 0021) | gi/s2-01b | 645a272 | S2-01, R-06 | R-G1 |
| S2-05, S2-G1 | open |  |  | see plan |  |
| S3-02 | done (looped lamp index, PS c208-c214; tools/lamp_index_test) | gi/s3-02 | 9f3e29d | S0-05 | S3-03 |
| S3-02b, S3-02c | done offline (cells under both laws, sliced; fence and window draws; lamp-index-smoke job) | gi/s3-02c | 839b2fc | S3-02 | S3-03 |
| S3-01, S3-03 to S3-06, S3-G1 | open |  |  | see plan |  |
| S4-01 to S4-03, S4-G1 | open |  |  | see plan |  |
| S5-01 to S5-05, S5-G1 | open |  |  | see plan |  |
| S6-01 to S6-04, S6-G1 | open |  |  | see plan |  |
| GB-01 | done offline; in-game smoke `gbuffer-motion-smoke` open | gi/gb-01 | 0d37d81 | research 14 | WP2 now; WP7 after the smoke; WP8 |
| GB-02 to GB-04 | done offline (G-buffer copies, full shader table, AO normals and temporal filter) | gi/gb-04 | fa9adba | GB-01 | GB-05 (building) |
| X-R0MEMO | done (research 17: two back ends, server primary; plan.md and design.md updated) | gi/x-r0memo | 185c519 | - | R-05 fallback target, G-server decided: pass |
| X-RTPP | done (research/16, wp/X-RTPP) | gi/x-rtpp | 286f91f | - | XR-04 base |
| XR-04 | done (per-pixel delta shadow kernel, Vulkan ray queries; on gi/r-02) | gi/xr-04 | 28288c5 | X-RTPP, R-02 | XR-05, XR-06, XR-11 |
| XR-09 | done (see wp/XR-09.md) | gi/xr-09 | 4a0c476 | XR-04 | see wp |
| XR-05 | done (ps_3_0 temporal/atrous/upsample filter for the RT mask; tools/gi_pixel_filter_test) | gi/xr-05 | 1d72119 | XR-04 | XR-06 |
| XR-06, XR-07 | done (per-pixel shadow pass in the server's DXVK, patch 0019; Apex side `pixelShadows`, off) | gi/xr-07 | ad3050e | XR-04, XR-05 | XR-08 (building) |
| MC, MC-2, MC-3 | done offline (package mesh reader, patch 0020 mesh casters, patch 0022 read-back for the shadow tiles) | gi/mc-3 | 0764866 | XR-06 | in-game smoke `mesh-casters-smoke` |
| X-REBAKE | done (re-bake cost against lamp count) | gi/x-rebake | 688a15f | R-02 | R-06 |
| X-05, PLAN-SEED | done offline (2D radiance cascades over the wall-plan field; exact 0 through closed walls) | gi/plan-seed-fix | 23b0a09 | S2-03 |  |
| X-02, X-03, X-04 | open |  |  | adjacent, can start |

Parked by research 17: S2-02, the S2-03 Apex wiring, the Direct3D 9 parts of S2-04 and S3-04, S5-04. d3d9 fallback = S1 direct + CPU roomAmbient + rig write + capsule occlusion.

S0-12 answers (static RE, wp/S0-12.md, docs/engine/room-light-maps.md "Reflectance source"): floor colour per quarter-tile sits at tile section +0x80 (Apex runs the 0x0069FF20 tile loop itself); lot +0x3B8 is the LotLightingManager (+0x34 strobe room, so the S0-09 clamp is only the strobe flash; +0x30 black-lit); [0x01158D30] is the min-spec shaders flag (0 with Advanced Rendering on).

Gates (plan.md): G-split, G-server, G-scale, G-rig, G-constants, G-stage1: all undecided.


## Merge round 3 (branch `gi/merge-r3`, not yet fast-forwarded into `feature/realtime-gi`)

State: all branches merged and committed on `gi/merge-r3` (worktree `gi-merge-r3`). Stopped for a restart before the
last bridge test run; `feature/realtime-gi` is still at b74d192.

Bridge patches (all apply in order on f35b86a in a fresh `build.cmd` checkout, verified with `git apply`):

| Patch | Package | Note |
|---|---|---|
| 0016 | rewind work | device reference fix only (no 0015 test hunk); the GI test apps crash (0xC0000005 after CreateDevice) without it |
| 0017 | async read-back | unchanged |
| 0018 | R-03 + R-04 | regenerated: its test `meson.build` hunk assumed 0015-0016; now a one-line change |
| 0019 | XR-06 | unchanged (xr-07 and mc-2 rebases were identical) |
| 0020 | MC-2 | unchanged |
| 0021 | R-06 (was 0020) | rebased onto 0019 and 0020; GI block layout 3 keeps counter words 14-21, GiPixel block separate |
| 0022 | MC-3 | rebased onto 0021; its GiMesh slot is a separate mapping |

Done and passing: Apex Developer and Public builds (before the 0016 commit; Apex code unchanged since), Smoke suite,
`tools/i18n_check` (OK), `menu_audit` (same counts as b74d192), `public_text_check`, `tools/const_range_check`,
`tools/lamp_index_test -Precomp` (OK), `tools/gi_sched_test`, `test-gi-pixel` mode off, compose test scope checks
(lamps scoped to touch box + 0.5 m byte-equal to unscoped). Fixed in the merge: `rebake_check.cpp` got back the wire
scene writer S2-01b removed from `check.cpp`.

Next steps (from the `gi-merge-r3` worktree; the bridge pair with 0016 is built in `native/remix-bridge/out`):
1. Run the bridge tests: `pwsh -File <scratch>/alltests.ps1` or by hand: `native/remix-bridge/test-gi.cmd` (with
   `DXVK_X64` = the `native/dxvk` build's d3d9.dll, meson venv on PATH), `test-gi-pixel.cmd`,
   `pwsh tools/gi_server_test/run.ps1 -Mesh -Game "<Sims 3 folder>" -Bridge`,
   `pwsh tools/gi_compose_test/run.ps1 -BridgeSrc native/remix-bridge/src`. All GI device modes failed before 0016.
2. If green: `git -C apex-gi merge --ff-only gi/merge-r3` (local only), then delete build intermediates
   (`obj/`, `native/remix-bridge/src/bridge/_b_*`, `_t_x86`; keep `Release/`, `Public/`, `native/remix-bridge/out/`).
3. Round 4: `gi/xr-08`, `gi/gb-05` (not merged).

Open items:
- R-06: touch boxes cut 8-11 %: S2-01b box size to follow.
- R-06: scoped slices 6-10 ms: R-G1 batch sizing from the scoped list.
- R-06 query acceptance changed to bias <= 1/255, mean <= 8/255.

## Results

### Design: reconciled architecture, plan and handoff
- Status: done
- Deliverable: `design.md`, `plan.md`, `handoff.md`
- Findings: research pages 00-12 are folded in. The carrier is the texel-space light buffer; indirect light is one
  linear layer a lamp; the solves run in the bridge server (ray queries, then compute) with Direct3D 9 passes as
  the fallback; the room solve is capped at class 0, not skipped (all inferred until S0 closes the open marks).
- Open issues: research page 13 is not in the folder yet (design §12, plan "To fold in"). Page 11 states that page
  09 is missing; both exist.
- Unblocks: every package marked "can start" above.
