# High-precision scene check

Run `powershell -File tools/scene_target_test/run.ps1 -OutDir <scratch-folder>` with the x86 MSVC toolchain installed.
`tools/development/check.ps1 -Suite Smoke` runs it too.

`check.cpp` includes `framework/scene_target_logic.h`, the code the ASI uses:

- Pure logic: the refusals (multisampled or alpha-less back buffer, no 16-bit blending), when `SetRenderTarget`
  substitutes (render target 0, the back buffer, before the resolve only), when a copy resolves first, and the resolve's
  grain over every third 16-bit value: exact 8-bit levels unchanged, never past 0 or 255, average error under 0.01
  level between 1 and 254 levels.
- Native D3D9 (printed as SKIP without a HAL device): the resolve shader is compiled from the same source and draws a
  16-bit target into an 8-bit one. Each channel is compared with the C++ copy, alpha with the plain rounding, exact levels
  must stay exact. A pass at strength 0 records the GPU's own float to 8-bit rounding.

It writes only its executable and objects into the output folder. It does not test the hooks, the substitution in the
game, DXVK or the frame time.
