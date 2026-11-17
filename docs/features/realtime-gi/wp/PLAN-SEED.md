# PLAN-SEED: exact 0 through a closed wall (plan field corner seeds, cascades back-read)

**Status:** done. Branch `gi/plan-seed-fix`, on `feature/realtime-gi` with `gi/x-05` merged in (the cascades harness and
its `check.cpp` repair). Fixes the open issue "Plan field corner seeds" of [X-05](X-05.md).

Marks: [V] verified by a harness named in the line, [I] inferred.

## Result

The sealed scene of `run.ps1 -Cascades` now leaks exactly 0 on every arm with the bilinear fix; its check is back to
`leakMax == 0`.

| Arm (sealed rooms, fix) | Before: lit outside, largest | Seeds only | Seeds + back-read |
|---|---|---|---|
| all bounces, exact start | 7 367, 0.007/255 | 7 256, 0.0068/255 | **0, 0** |
| all bounces, tile start | (not in X-05's table) | 7 256, 0.0076/255 | **0, 0** |
| bounces 2+ | 6 029, 0.003/255 | 5 485, 0.0030/255 | **0, 0** |

## The two causes

1. **Corner seeds** (`PlanField::SeedBand`, CPU; the ps_3_0 port uploads these seeds, so the GPU field follows with no
   shader change). One seed a pixel, chosen among the walls crossing that pixel, dropped the second wall where two
   walls share a pixel; the pixel beside an outside corner read 1.12 px for a wall 0.5 px away. Now every wall sample
   is offered to its pixel and the 8 around it, and each pixel keeps the nearest: every wall point within 1.5 px of a
   pixel centre reaches it, so the field is exact (to the 1/4 px sampling) wherever the trace's `kHit` test looks.
   [V] alone this removed only 1.5 % of the sealed leak: the corner was not the main cause there; X-05's inference
   for the sealed scene was wrong.
2. **The hit's back-read** (`PlanCascades::TraceSeg`, `CascadePS` in `shaders/plan_cascades_hlsl.h`,
   `tools/gi_cascades_test/cascades.comp`). A hit reads the wall's radiosity `kBack` = 1.5 px back along the ray. A
   cascade segment starts at its interval start, up to the arc gap (0.78 px) from where the parent ray ended, so a start
   can lie just past a wall line; it is blocked at once, and the read 1.5 px back lands on the wall's far side: the lit
   room's wall radiosity enters the sealed room. The read is now clamped to the segment's start
   (`max(s - kBack, 0)`), the same in all three forms. [V] this removed the rest.

## Checks after the fix [V]

| Harness | Result | Change |
|---|---|---|
| `run_plan_field.ps1` (S0-06) | 19 checks, 0 failed | doorway at 32 px a tile: field against twin leak 0.118 % to 0.006 %, dark 1.558 % to 2.200 %; all 16 px rows unchanged |
| `plan_field_gpu_test` (S2-03) | 259 checks, 0 failed | GPU against CPU verdicts, summed over all cases and arms: 443 to 395 differ, still 0 seen where the CPU blocks; diagonal wall, float: 0 to 1 pair differs in steps; diagonal against twin dark 0.864 % to 0.889 % (32 steps) |
| `run_plan_cascades.ps1` (X-05 kernel) | 11 checks, 0 failed | closed wall, fix: 0 of 2 880 (as before); doorway, fix: 0.998 / 1.033 of whole rays (1.034 before) |
| `plan_cascades_gpu_test` (ps_3_0) | 20 checks, 0 failed | fix arm: 0 probes outside the rooms lit |
| `gi_cascades_test` (compute) | 8 checks, 0 failed | |
| `rt_lamps_test -Cascades` | 14 checks, 0 failures (sealed check now `== 0`) | sealed: 0 lit outside on every fix arm; other scenes: means move by at most 0.002 of the sum, 0.02/255 |
| `rt_lamps_test -Gi` | 680 298 checks, 0 failures | |

- [V] The plain merge (no fix, kept as the bad arm) leaks a little more with the clamp: sealed, bounces 2+, 0.67 to
  0.83/255; all bounces 2.17 to 3.01/255. [I] Its rays are continued by probes behind walls anyway; the clamped
  read now returns that side's light where it used to return this side's.
- [V] Means against the reference on the sealed scene are unchanged to the printed digits (all bounces, exact start:
  sum 1.032, 2.44/255 lit).

## Open issues

- None for the leak. The field now differs from the twin a little more often in the dark direction at 32 px a tile
  (pairs near jambs, where the nearer seeds narrow the opening): blocked, never a leak.

## Unblocks

The "exactly 0 through a closed wall" condition of design §3.5 for the D3D9 cascades back end (X-05's recommended
role).
