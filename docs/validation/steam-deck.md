# Steam Deck and Proton validation

[Feature page](../features/steam-deck.md)

## Scope

- Under Proton the log names Wine, the host and, on a Deck, its model; DXVK is found by its export.
- Settings > Compatibility shows the platform and DXVK correctly and never recommends downloading DXVK under Proton.
- Every Apex shader is taken from the player's cache or the ASI, with none compiled, on a first start under Proton.
- Ambient Occlusion and Depth Blur draw under Proton, or their card names the reason.
- Load Performance changes no lighting setting.
- Windows behaves as before.

## Automated tests

None specific. The platform detection reads the running process and has no offline harness.

## Latest results

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| 2026-10-29 | `3839ebc` (2.18.0 release) | Windows start of the release build with no player cache, then one with the rebuilt resource | The first start compiled the 10 shaders the old resource lacked and saved 81; with that file built in, a start with no player cache took all 81 from the ASI, 0 to compile | Windows, DXVK |
| 2026-10-20 | Working tree on `a4b0b99` | Build, `tools/i18n_check` | Public build compiles with the shader cache resource (8,596,480 bytes); 21 languages, 2,167 keys | None |

## In-game test plan

1. Steam Deck, Proton Experimental, launch option `WINEDLLOVERRIDES="wininet=n,b" %command%`, no `d3d9.dll` in
   `Game\Bin`. **Expected log:** `[Main] Platform: Wine ... on Linux ... (Steam Deck LCD|OLED)`, `[D3D] Platform: ...,
   d3d9: DXVK`, `[ShaderCache] N Apex shaders taken from the ones built into the ASI`, then `N from ..., 0 to compile`.
2. Settings > Compatibility: Platform *Steam Deck (Proton)*, DXVK *Installed*, the Steam Deck group, no DXVK download.
3. Image > Ambient Occlusion on, in a lot: shade visible where objects meet, or an error on the card. **Log:** `[AO]
   Resources ready`, `[DepthBlur] Resources ready (... INTZ depth swapped in ...)`, or `[DepthBlur] Depth buffer not
   shared: ...`.
4. Load Performance: AO, Depth Blur and picture filters off, night lighting unchanged.
5. Windows with DXVK 3.1.1 and with native D3D9: the same log and menu as before, plus `Platform: Windows`.

## Confirmed in game

Nothing yet.

## Open checks

- Every item of the in-game test plan.
- Whether Wine's builtin `d3dcompiler_47` refuses any Apex shader (with the built-in cache it is no longer used for them).
- Frame time on the Deck with the default profile and with Performance.
