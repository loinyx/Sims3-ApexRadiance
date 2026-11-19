# Bridge build: patch step (BUILD-PATCH)

Branch `gi/build-patch-fix`, off `feature/realtime-gi` (merge round 3). Found on `gi/xr-10` ([XR-10](XR-10.md)).

Marks: [V] verified by a run, [I] inferred.

## Cause [V]
- Upstream (dxvk-remix f35b86a) stores every file `bridge/` the patches touch with CRLF endings.
- Several patches (0014, 0018, 0020, 0021, 0022) had LF or mixed endings in their hunks, so they applied only to a
  checkout converted by `core.autocrlf=true` plus `--ignore-whitespace`, and failed on an exact checkout.
- `native/remix-bridge/.gitattributes` already has `*.patch -text`, so git keeps the patch bytes as committed.
- A failed patch left `src\` half patched; the next run saw `src\.git` and skipped patching.

## Change
- `build.cmd`: clones with `core.autocrlf=false` (src is exactly the upstream bytes, whatever the user's git config).
- Patches 0014, 0018, 0020, 0021, 0022: hunk lines rewritten to the CRLF of their target file (content unchanged).
  New files in a patch keep their authored endings. Plain `git apply --whitespace=nowarn`, no `--ignore-whitespace`.
- A failing patch now prints `PATCH FAILED: <name>`, deletes `src\` (so the next run refetches) and exits 1.
- `PATCH_ONLY=1 build.cmd` stops after the patch step (exit 0): a quick check of the series without a compile.
- New patches: make them from the exact upstream checkout (`core.autocrlf=false`), so hunks carry the file's CRLF.

## Proof [V]
- Fresh `build.cmd` with `PATCH_ONLY=1`, global `core.autocrlf=true`: all 21 patches present (0001-0014, 0016-0022; there
  is no 0015 on the branch, 0016 stands alone) applied in order, exit 0.
- Same with a copy of one patch broken (context line changed): `PATCH FAILED` named, `src\` removed, exit 1.
- Full compile of the pair not rerun here (the patch text is byte-identical apart from line endings).
