# Vulkan Driver Guard: history

Chronological record of investigations and design decisions. Newest entries at the bottom. The current behaviour is
described in [features/performance/vulkan-driver-guard.md](../features/performance/vulkan-driver-guard.md).

### 2026-09-30: address-space study

**Context:** TS3W.exe is 32-bit and large-address-aware (4 GB). Session notes `loadre\addrspace.md` used S3SS's memory
statistics in the 10 crash logs of 2026-09-29 and 2026-09-30: 1618 to 2250 MB free, largest free block at least 1 GB,
no out-of-memory crash.

**Finding:** on a PC with an RTX and an AMD integrated GPU, AMD's 32-bit Vulkan driver `amdvlk32.dll` (85 MB image at
0x0FB30000, in the low 2 GB the game heaps use) was loaded into the game. DXVK asks the Vulkan loader for every GPU
("Found device: AMD Radeon(TM) Graphics" in `TS3W_d3d9.log`) although it renders on the RTX. The PC's Vulkan loader was
1.4.341.

**Outcome:** commit `ccf1bee`: `VK_LOADER_DRIVERS_DISABLE` set for this process before the first Direct3DCreate9 call,
under the conditions on the feature page; the development address-space monitor added. The same commit capped the
RefPack per-thread kept buffers at 12 MB for all threads together
([Faster Cache Compression history](performance-fast-cache-compression.md)). The address space returned (about 85 to
100 MB) is an expectation from the image size and driver heaps, not a measurement.
