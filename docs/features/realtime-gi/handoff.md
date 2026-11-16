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

Design and plan are written; no package has started.

| Id | Status | Depends on | Notes |
|---|---|---|---|
| S0-01 | open | - | can start |
| S0-02 | open | - | can start |
| S0-03 | open | - | can start |
| S0-04 | open | - | can start |
| S0-05 | open | - | can start |
| S0-06 | open | - | can start |
| S0-07 | open | - | can start |
| S0-08 | open | - | can start |
| S0-09 | open | - | can start |
| S0-10 | open | - | can start |
| S0-11 | open | - | can start |
| S0-12 | open | - | can start |
| S0-13 | open | - | can start |
| S0-14 | open | - | can start |
| S0-15 | open | - | can start |
| S0-16 | open | - | can start |
| S0-17 | open | S0-01 | |
| S0-G1 | open | S0-11 | game run |
| S0-G2 | open | S0-14 | game run |
| S0-G3 | open | S0-15 | game run |
| R-01 | open | - | can start |
| R-02 | open | - | can start |
| R-03 | open | R-01 | bridge build |
| R-04 | open | R-02, R-03 | |
| R-05 | open | R-03, S2-01 | |
| R-06 | open | R-04 | |
| R-07 | open | R-04, S0-G2 | |
| R-08 | open | - | can start |
| R-G1 | open | R-05, S2-01 | game run |
| S1-01 | open | - | can start |
| S1-02 | open | S1-01 | |
| S1-03 | open | S1-01, S0-10, S0-G1 | |
| S1-04 | open | S1-02 | |
| S1-05 | open | S1-04, S0-03, S0-04 | |
| S1-06 | open | S1-03 | |
| S1-07 | open | S1-04 | |
| S1-G1 to S1-G3 | open | S1-03, S1-04 | game runs |
| S2-01 to S2-05, S2-G1 | open | see plan | |
| S3-01 to S3-06, S3-G1 | open | see plan | |
| S4-01 to S4-03, S4-G1 | open | see plan | |
| S5-01 to S5-05, S5-G1 | open | see plan | |
| S6-01 to S6-04, S6-G1 | open | see plan | |
| X-02, X-03, X-04 | open | - | adjacent, can start |

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
