# Light Diag: validation

The feature is described in [features/dev-tools/light-diag.md](../features/dev-tools/light-diag.md). It must:

- Write a complete snapshot to a new `Captures\<date time> Lighting snapshot\` folder without changing the scene.
- Never crash on stale pointers (every read guarded).
- Refuse cleanly without a world, and stay inactive when the game addresses do not validate.

## Automated tests

| Harness | Covers | Run | Needs |
|---|---|---|---|
| [`tools/report_check`](../../tools/report_check/README.md) | Capture folder storage and the Report page Lighting snapshot row; the dump itself is stubbed | See the harness README | None (x86 MSVC, ImGui from vcpkg) |

The structure walk needs a running game; it has no offline harness.

## Latest results

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| | | No dedicated snapshot result is recorded | | |

## In-game test plan

1. Night Lights on, a world loaded. Press the Diagnostics shortcut (`B`, `5` or `F8`) or Report a problem > Lighting
   snapshot > Save. **Expected:** a short hitch, the notice "Lighting snapshot saved. Open Report a problem to find your
   files", and the log line `[LightDiag] Saved: N lights, M lot stories, R rooms`.
2. Open the folder. **Expected:** `Lighting snapshot.txt` with the header, all six sections and the `End:` footer, plus
   the log, settings and `About this capture.txt`.
3. On the main menu (no world), request a snapshot. **Expected:** the warning "Lighting snapshot: load a world first"
   and no folder.
4. Take a snapshot, relight the active lot (Developer > Lighting > Refresh lighting), take another. **Expected:** the
   first has "No samples: recording them was off ..." in the stories section (unless the checkbox was on), the second
   has sample records.
5. On a non-Steam build where the root getter does not validate. **Expected:** the install log line
   `[NightTerrainRelight] Light diagnostics not available on this game version` and the shortcut does nothing.

## Confirmed in game

- The snapshot produced the findings recorded in [history](../history/dev-tools-light-diag.md) (lamp colours, story
  lists, PASSO3 constants, lamp story field).

## Open checks

- Meaning of light flag 0x04 ("room known" in the code, "outdoor" in older notes).
- Unknown fields: flag bits 0x08/0x10/0x80, `flag280`, `x100`, `no_hash24`.
