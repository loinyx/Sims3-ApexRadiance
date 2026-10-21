# Steam Deck and Proton

Apex Radiance runs on Linux and the Steam Deck through Proton, the same ASI as on Windows. It recognises Proton, the
Steam Deck and DXVK, shows them in Settings > Compatibility, carries its own shaders already compiled, and suggests the
lighter Performance profile on the Deck.

## Status

| | |
|---|---|
| Availability | Released in 2.15.0 |
| Menu | Settings > Compatibility: Platform and DXVK rows, Steam Deck group; welcome page note |
| Configuration | No setting |
| Source | `framework/platform.{h,cpp}`, `framework/shader_cache.cpp` (`LoadEmbedded`), `apex_gui.cpp`, `ApexRadiance.rc`, `resources/ApexRadiance_ShaderCache.bin` |

## The problem

- **Detection.** Proton loads its own DXVK as `d3d9.dll` from the prefix's system folder. The menu took a `d3d9.dll`
  in the system folder for Windows' own and recommended downloading DXVK, although the game already draws through it.
  Under DXVK the adapter name says nothing either: DXVK reports The Sims 3 as a "Geforce GTX 580" (vendor `0x10DE`),
  its own fix for the game's shadows on unknown AMD cards, also on the Deck's AMD graphics.
- **Shaders.** Apex compiles its HLSL with `d3dcompiler_47.dll`. Under Proton that is Wine's builtin compiler unless
  Microsoft's is installed in the prefix, and it may refuse some of Apex's `ps_3_0` shaders (unverified on the Deck);
  a shader that does not compile leaves its effect off, as Ambient Occlusion was reported to stay off.
- **Cost.** The Deck's graphics (8 RDNA 2 compute units) share 88 GB/s of memory bandwidth with the CPU; full-screen
  passes (Ambient Occlusion, Depth Blur, picture filters) cost the most there.

## How Apex Radiance solves it

- **Detection** (`Platform::Get`, read once): Wine or Proton by the `wine_get_version` export of `ntdll.dll`, the host
  system by `wine_get_host_version`; a Steam Deck by the host's DMI files through Wine's `Z:` drive
  (`Z:\sys\devices\virtual\dmi\id`: vendor `Valve`, product `Jupiter` for the LCD model, `Galileo` for OLED). The
  `SteamDeck=1` environment variable is logged as a hint only, since other Linux desktops set it too. DXVK by the
  `DXVK_RegisterAnnotation` export of the loaded `d3d9.dll`, ordinal 28257, which has no name; under Wine also by the
  absence of `wined3d.dll`, which only Wine's own `d3d9` loads.
- **Menu.** Settings > Compatibility shows *Platform* (Windows, Steam Deck (Proton), Linux (Proton), macOS (Wine) or
  Wine, by the host system) and DXVK as
  *Installed*, or *Not in use (WineD3D)* when Proton runs with `PROTON_USE_WINED3D=1`. Under Proton the menu never
  recommends downloading DXVK.
- **Shaders built in.** The ASI carries every Apex shader compiled with Microsoft's `d3dcompiler_47` (resource
  `APEX_RC_SHADER_CACHE`, the format of `ApexRadiance_ShaderCache.bin`). At start-up the player's own cache is read
  first, then the built-in one for the variants it lacks; a variant whose source changed since the built-in file was
  made misses it and is compiled as before. Under Proton a compile failure logs once how to install Microsoft's
  compiler.
- **Steam Deck profile.** On a Deck the welcome page recommends Performance, and Settings > Compatibility offers
  *Load Performance*. It applies the profile's Color, Ambient Occlusion, Depth Blur, Edge Smoothing and Performance
  parts, never its lighting part, so night lighting stays as it is. Nothing changes without the player's choice.
- **Fewer waits under DXVK.** Under DXVK the floor mask of Instant (GPU) outdoor lamps alternates between two buffers, so a write
  never locks the buffer the previous draws read ([rt-outdoor-lamps.md](night-lighting/rt-outdoor-lamps.md)).

## Installing on Steam Deck or Linux

1. Install an ASI loader and `ApexRadiance.asi` in `Game\Bin` as on Windows. Do not add a `d3d9.dll` or `dxvk.conf`:
   Proton ships DXVK.
2. In Steam, game Properties > Launch Options: `WINEDLLOVERRIDES="wininet=n,b" %command%` (the name of the loader's
   DLL; `dinput8=n,b` for a `dinput8.dll` loader). Without it Wine loads its own `wininet.dll` and no ASI loads.
3. Use a recent Proton (Proton Experimental or 10 and later).

## Compatibility and interactions

The detection and the built-in shaders also apply on Windows: there the player's own cache is complete after the first
start, and the built-in copy only saves the first compile.

## Limitations

- Proton, the Steam Deck and Proton's DXVK versions are not validated in game
  ([validation](../validation/steam-deck.md)).
- A shader changed after the built-in file was made compiles at start-up as before, with Wine's compiler under Proton.
- Under Proton, opening a folder or web page from the menu depends on the prefix's file and browser handlers.

## Technical reference

- `Platform::Summary()` is logged as `[Main] Platform: ...` at start-up and `[D3D] Platform: ...` with the device, for
  bug reports.
- The built-in shaders are read before the player's cache (under Wine that file may hold the builtin compiler's output
  of the same source). The built-in cache is `resources/ApexRadiance_ShaderCache.bin`, regenerated for each release from a Windows run of
  the release build whose log reads `N Apex shaders: N from ApexRadiance_ShaderCache.bin`
  ([release-process.md](../release-process.md)).
- Ambient Occlusion's card shows a shared-depth error (Depth Blur's resources) as its own; Depth Blur logs once per
  depth format why the game's depth buffer is not shared.

## See also

- [Validation](../validation/steam-deck.md)
- [History](../history/steam-deck.md)
- [Architecture](../architecture.md)
