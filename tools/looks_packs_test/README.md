# Looks shader pack checks

Offline checks of the shader pack downloads of Looks (`features/looks_packs.cpp`). Run from the repository root in an
x86 VS developer prompt:

```bat
cl /nologo /c /O2 third_party\miniz\miniz.c /Fo:%TEMP%\
cl /nologo /std:c++20 /EHsc /utf-8 /O2 /DNOMINMAX /I. /Iframework /Ifeatures tools\looks_packs_test\check.cpp features\looks_packs.cpp %TEMP%\miniz.obj /Fe:%TEMP%\looks_packs_test.exe /Fo:%TEMP%\
%TEMP%\looks_packs_test.exe <empty test folder> [--real]
```

`<empty test folder>` plays the Apex Radiance folder: the checks write only in its `Shaders\Packs\`, which they empty
first. Without `--real` nothing goes online: every HTTP answer is simulated.

- Allowed hosts: HTTPS to github.com, codeload.github.com, objects.githubusercontent.com and
  raw.githubusercontent.com only (no other host, look-alike, port or user name).
- HTTP failures: 404, 410 and 451 a pack gone; 403 or 429 with a rate limit GitHub busy.
- Unpacking a zip made like GitHub's: `.fx` / `.fxh` under `Shaders\`, images under `Textures\`, other files left out,
  `..` and reserved names never written, more than four folders deep left out, not a zip and more than 5000 files
  refused.
- ReShade's list joined with the built-in index and the downloaded folders: a pack the index does not know, a
  downloaded pack no longer on the list (kept), a pack neither downloaded nor on the list (left out), by name.
- The queue with simulated answers: the standard effects first, two packs downloaded, a pack gone (unavailable for the
  session, the queue goes on), GitHub busy, no connection, a failed update that keeps the pack, Stop the rest; a scan
  after each pack.
- Which pack has a missing `.fx` (the technique, then the earliest on ReShade's list; never a pack that is gone).
- The miniz licence text shown in Settings > About is `third_party/miniz/LICENSE`.
- `--real`: one real download of a small pack (Ptho-FX) through WinHTTP.

The output ends with `OK` or `PROBLEMS`.
