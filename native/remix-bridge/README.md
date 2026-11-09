# remix-bridge

NVIDIA's RTX Remix bridge (`dxvk-remix/bridge`, MIT, https://github.com/NVIDIAGameWorks/dxvk-remix) with our patches,
used to run TS3's DXVK in a 64-bit process. Ported from sims3-modernization (#316, by alexvixell35-stack), whose
maintainers handed it over to Apex Radiance. Findings, numbers and next steps: `docs/features/rtx-remix-bridge.md`.

- `build.cmd` fetches the pinned upstream commit into `src\`, applies `patches\` in order and builds `out\d3d9.dll`
  (x86 client) and `out\.trex\NvRemixBridge.exe` (x64 server). Needs git, VS 2022 C++ build tools and meson + ninja.
- `conf\`: `bridge.conf` (next to `d3d9.dll` and in `.trex`) and `dxvk.conf` (in Bin; it repeats DXVK's TS3W profile,
  without which the game resets Options.ini).

Patches. Each applies after the ones before it. "Internal" is the name the #40 run reports use.

| Patch | Internal | What | Default |
|---|---|---|---|
| 0001 | 0001 | `GetRenderTarget` / `GetDepthStencilSurface` return `D3DERR_NOTFOUND` on an empty slot (TS3 crashed on `S_OK` + NULL). Posted on rtx-remix#1028. | always |
| 0002 | 0002 | log-only counters of the client's CPU copies of locked surfaces and buffers (`BridgePerf` lines) | with `perfLogIntervalFrames` |
| 0003 | 0003-0005 | `client.freeIdleManagedCopiesSec`: free a MANAGED texture level's client copy after N s without a lock, and fetch it back on the next lock. Also adds per-pool copy counters and `GetRenderTargetData` caller lines. | conf: 2 |
| 0004 | 0006-0010 | `[BridgeDiag]` lines: memory and live objects, frame outliers, upload repeats (full-data CRC32C), re-uploads after destroy, and creation / destroy / lock sites named by the game's callers | with `perfLogIntervalFrames` |
| 0005 | 0011 | logger: a log line of 4 KB or more no longer ends the game with 0xC0000409 (upstream bug) | always |
| 0006 | 0012 | `client.ts3TextureCacheShift` (TS3W 1.67 only): the ResourceCache counts a MANAGED texture's pixels as bytes >> N | off |
| 0007 | 0013 | `client.ts3TextureKeepAliveSec` (TS3W 1.67 only): unforced ResourceCache trims keep a texture for N s | off |
| 0008 | 0014 | `client.freeIdleBufferCopiesSec`: free a static vertex / index buffer's client copy (64 KB to 4 MB) after N s without a lock. A CRC32C checks each fetch. | conf: 2 |
| 0009 | 0015 | `client.freeIdleManagedCopiesMs`: 0003's threshold in milliseconds | off |
| 0010 | 0016 | render target 0 is the implicit swapchain's back buffer 0 object, so `GetRenderTarget(0)` and `GetBackBuffer(0, 0)` return the same surface, as in d3d9 and DXVK (upstream made a separate place-holder; Apex Radiance compares the two by pointer and drew a black picture) | always |

The TS3-only options patch the game's code in memory only when the bytes there match TS3W 1.67. Otherwise they change
nothing.

`package.ps1` assembles the player package after `build.cmd`. It fetches the DXVK 3.1.1 release (checked by SHA-256)
for the server's renderer and writes `out\package\` and `out\Apex-Radiance-RTX-Remix-bridge.zip`, laid out as in Bin:
- `d3d9.dll` (the client), `bridge.conf` and `dxvk.conf`;
- `.trex\NvRemixBridge.exe` (the server), `.trex\d3d9vk_x64.dll` (DXVK x64) and `.trex\bridge.conf`;
- `licenses\` (bridge and Detours MIT, DXVK zlib) and the player notes from `PLAYER-README.txt`.

The EA app build runs `TS3.exe`: its players set `client.targetProcess = TS3.exe` in both bridge.conf files.
Apex Radiance 3.0.0 detects the bridge and works under it, with or without patch 0010 (issue 59 is fixed on Apex's
side; 0010 stays because it matches what d3d9 and DXVK return).

To move to a newer upstream: change `COMMIT` in `build.cmd`, delete `src\`, and run it. Refresh any patch that no
longer applies with `git diff`, from a checkout of the new commit that has the earlier patches applied.
