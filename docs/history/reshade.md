# ReShade effects: history

Chronological record of investigations and design decisions. Newest entries at the bottom. The current behaviour is
described in [features/reshade.md](../features/reshade.md).

### 2026-10-12: add-on bridge to an installed ReShade

**Context:** ReShade's effects were wanted under the game's interface, with the game's exact depth and with effect
files added from the menu.

**Finding:** ReShade 6.3+ accepts external add-ons (`ReShadeRegisterAddon`, API 14). Calling
`effect_runtime::render_effects` at Apex's post-scene boundary moves its rendering point before the interface, and
`reshade_begin_effects` can bind Apex's INTZ depth as `DEPTH`. It needs ReShade installed for Direct3D 9; under DXVK
ReShade runs on Vulkan and the bridge cannot move it, and with the game's Edge Smoothing ReShade cannot render into the
multisampled back buffer outside Present.

**Outcome:** rejected before release. Players should not have to install ReShade; only its effects are wanted. Replaced
by the built-in runtime below. The bridge code is in commit b624aa5.

### 2026-10-12: built-in ReShade FX runtime

**Context:** run ReShade's `.fx` effects without ReShade.

**Finding:** ReShade's FX compiler (preprocessor, parser, HLSL code generator) is standard C++ with no ReShade runtime
dependency and is BSD-3-Clause. Its shader model 3 output for 108 effects from seven public packs (SweetFX, prod80,
qUINT, AstrayFX, Depth3D, reshade-shaders, fubax) parsed without errors once `ReShade.fxh` and a `ReShadeUI.fxh`
replacement were on the include path. Whether `D3DCompile` accepts each one was not yet measured.

**Outcome:** kept. Apex Radiance embeds the compiler and implements the Direct3D 9 runtime itself (see the feature page).
