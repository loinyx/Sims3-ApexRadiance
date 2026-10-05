# Vulkan Driver Guard

On PCs with a dedicated graphics card and an AMD integrated GPU, the game can end up loading AMD's 32-bit Vulkan driver
even though it renders on the other card. The Vulkan driver guard keeps that unused driver out of the game, which gives
back roughly 85 to 100 MB of the 32-bit game's address space. Nothing drawn changes.

## Status

| | |
|---|---|
| Availability | Released in 2.4.0 |
| Default | Always on; no menu row |
| Menu | None |
| Configuration | None |
| Source | [`features/vulkan_driver_guard.{h,cpp}`](../../../features/vulkan_driver_guard.cpp) |

## The problem

TS3W.exe is 32-bit and large-address-aware (4 GB of address space). With DXVK, the Vulkan loader loads every installed
driver when DXVK enumerates GPUs. On a PC with an NVIDIA card and an AMD integrated GPU this puts AMD's 32-bit Vulkan
driver `amdvlk32.dll` (an 85 MB image) into the low 2 GB that the game's heaps use, and DXVK reports
"Found device: AMD Radeon(TM) Graphics" in `TS3W_d3d9.log` though it renders on the other card.

## How Apex Radiance solves it

In Apex's `Direct3DCreate9` detour, before the first real call (DXVK loads `vulkan-1.dll` there), Apex sets
`VK_LOADER_DRIVERS_DISABLE=amd-vulkan32.json,amdvlk32.json` for this process only, when all of the following hold:

1. `vulkan-1.dll` is not loaded yet;
2. none of `VK_LOADER_DRIVERS_*`, `VK_DRIVER_FILES`, `VK_ICD_FILENAMES` or `VK_ADD_DRIVER_FILES` is set;
3. DXGI lists an AMD adapter (vendor 0x1002) and a non-AMD one;
4. the adapter with the most dedicated memory is not AMD;
5. every AMD adapter has under half of that memory.

## Settings

None.

## Compatibility and interactions

- `VK_LOADER_DRIVERS_DISABLE` needs Vulkan loader 1.3.234 or later. Behaviour with older loaders is unverified.
- The D3D9 adapter count drops from 2 to 1; the game creates its device on adapter 0 (the dedicated card) either way.
- A user-set Vulkan driver variable always wins: the guard does nothing when one is present.

## Limitations

- Only AMD integrated GPUs next to a stronger non-AMD card are handled.

## Technical reference

- Log: `[VulkanDriverGuard] ...` (the decision) and `[D3D] Adapter N of M; AMD Vulkan driver in the game: loaded / not
  loaded`.
- The adapter list comes from `CreateDXGIFactory1` / `EnumAdapters1` (`ListAdapters`). The decision runs once
  (`std::once_flag`).

## Rejected approaches

None recorded.

## See also

- [Performance overview](README.md)
- [Validation](../../validation/performance-vulkan-driver-guard.md)
- [History](../../history/performance-vulkan-driver-guard.md)
