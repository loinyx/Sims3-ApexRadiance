# Vulkan Driver Guard: validation

The feature is described in [features/performance/vulkan-driver-guard.md](../features/performance/vulkan-driver-guard.md).
It must disable the AMD 32-bit Vulkan driver only under its five conditions and never override a user-set Vulkan driver
variable.

## Automated tests

None.

## Latest results

| Date | Commit | Harness | Result | Backend |
|---|---|---|---|---|
| 2026-09-30 | 2.4.0 sources | Address-space study, hybrid PC (RTX + AMD integrated GPU), Vulkan loader 1.4.341 | `amdvlk32.dll` (85 MB image at 0x0FB30000) loaded in the game without the guard. Expected effect of the guard: about 85 to 100 MB of address space back (image and the driver's heaps), D3D9 adapter count 2 -> 1, device still on the RTX | DXVK |

## In-game test plan

1. Hybrid PC (dedicated non-AMD card + AMD integrated GPU), DXVK. **Expected log:** `[VulkanDriverGuard] ...` and
   `[D3D] Adapter N of M; AMD Vulkan driver in the game: not loaded`.
2. Set `VK_ICD_FILENAMES` before starting. **Expected:** the guard does nothing.

## Confirmed in game

- Nothing measured beyond the expected effect in the table.

## Open checks

- Measured address space with the guard on (the Frame Profiler's address space monitor).
- Behaviour with Vulkan loaders older than 1.3.234.
