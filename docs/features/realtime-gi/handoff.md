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

Merged into `feature/realtime-gi`: S0-01 to S0-14, S1-01, S1-02, S2-03, R-01, R-08, R-02, GB-01, X-RTPP, XR-04 and XR-05.
Each package's full report is `wp/<id>.md`. Other branches still running: `gi/s0-08x`, `gi/x-6d`, `gi/x-leak`.

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
| R-03 | in progress | gi/r-03 |  | R-01 | bridge build |
| R-04 | open |  |  | R-02, R-03 |  |
| R-05 | open |  |  | R-03, S2-01 |  |
| R-06 | open |  |  | R-04 |  |
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
| S2-03 | done (GPU plan field shaders, on gi/s0-06) | gi/s2-03 | c115de7 | S0-06 | S2-02, X-05 |
| S2-01, S2-02, S2-04, S2-05, S2-G1 | open |  |  | see plan |  |
| S3-01 to S3-06, S3-G1 | open; S3-02 in progress (gi/s3-02) |  |  | see plan |  |
| S4-01 to S4-03, S4-G1 | open |  |  | see plan |  |
| S5-01 to S5-05, S5-G1 | open |  |  | see plan |  |
| S6-01 to S6-04, S6-G1 | open |  |  | see plan |  |
| GB-01 | done offline; in-game smoke `gbuffer-motion-smoke` open | gi/gb-01 | 0d37d81 | research 14 | WP2 now; WP7 after the smoke; WP8 |
| GB-02 | in progress | gi/gb-02 |  | GB-01 |  |
| X-RTPP | done (research/16, wp/X-RTPP) | gi/x-rtpp | 286f91f | - | XR-04 base |
| XR-04 | done (per-pixel delta shadow kernel, Vulkan ray queries; on gi/r-02) | gi/xr-04 | 28288c5 | X-RTPP, R-02 | XR-05, XR-06, XR-11 |
| XR-05 | done (ps_3_0 temporal/atrous/upsample filter for the RT mask; tools/gi_pixel_filter_test) | gi/xr-05 | 1d72119 | XR-04 | XR-06 |
| X-02, X-03, X-04 | open |  |  | adjacent, can start |

S0-12 answers (static RE, wp/S0-12.md, docs/engine/room-light-maps.md "Reflectance source"): floor colour per quarter-tile sits at tile section +0x80 (Apex runs the 0x0069FF20 tile loop itself); lot +0x3B8 is the LotLightingManager (+0x34 strobe room, so the S0-09 clamp is only the strobe flash; +0x30 black-lit); [0x01158D30] is the min-spec shaders flag (0 with Advanced Rendering on).

Gates (plan.md): G-split, G-server, G-scale, G-rig, G-constants, G-stage1: all undecided.

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
