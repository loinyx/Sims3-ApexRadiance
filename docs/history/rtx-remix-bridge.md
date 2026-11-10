# RTX Remix bridge: history

Dated investigations behind [features/rtx-remix-bridge.md](../features/rtx-remix-bridge.md). Append only. Run ids
(alNNN) and #40 are the sims3-modernization harness's.

### Before 2026-11-09: the port in sims3-modernization

The bridge was ported to The Sims 3 in sims3-modernization (rodneypantonial/sims3-modernization#316, by
alexvixell35-stack, with dalexlol), and measured in its harness. Bugs found on the way:

| What | Cause | Outcome |
|---|---|---|
| Access violation reading 0x0 at `0x60F299` after 6.4 min (al280) | `Device_Clear` (`0x60F270`) calls `GetDepthStencilSurface` and tests only `hr < 0`; the client returned `S_OK` and NULL where d3d9 returns `D3DERR_NOTFOUND` (`GetRenderTarget` on an empty slot too) | Patch 0001; al281 then passed 30 min. Root cause and diff posted on upstream rtx-remix#1028 (an EA app build crash at 0x610329), which may be the same crash (inferred) |
| The game ends with 0xC0000409 right after a long log line (al306, 7.7 min) | The client's `Logger::emitLine` copies each line into a static 4096-byte buffer with `sprintf_s`, which fast-fails on 4 KB | Patch 0005; not reported upstream yet |
| `Options.ini` reset to defaults (1024x768, texture quality 1) | DXVK's TS3W profile keys on the exe name and the server is `NvRemixBridge.exe`; the game saw a new `lastdevice`. The server reads `dxvk.conf` from its working folder, Bin, not `.trex` (measured 2026-11-07) | The profile repeated in the package's `dxvk.conf` |
| Blurry, pixelated picture in a copy of Bin | The game ran DPI-unaware: Windows' `HIGHDPIAWARE` flag for TS3W is per exe path, and the runs had `client.enableDpiAwareness = False` | The package keeps the default `True` |
| A black window with Apex Radiance 2.18.3 | Render target 0 was a place-holder linked to the back buffer, so `GetRenderTarget(0)` and `GetBackBuffer(0, 0)` returned two objects; Apex's scene detection compares them and never saw the scene | Patch 0010 (al361 passed with Apex on); posted on loinyx/Sims3-ApexRadiance#59. Apex 3.0.0 also handles the old client |
| `fetched 8 bytes expected 4` errors (582 in al300) | The server pads small replies; the client copies the smaller size, so nothing is overrun | Quiet since patch 0004 |
| Sims3SettingsSetter's borderless window does not apply | It logs "Borderless fullscreen mode applied" but no Reset reaches the server; the same showed in-process | Open; Apex's own window mode applies through its Reset hook |

Memory: without the copy fixes the client's CPU copies of locked surfaces and buffers (585.5 MB of MANAGED copies at
the end of al285) ate the 350 MB the bridge freed at load. Patches 0003 and 0008 free the idle copies; both at 2 s
became the package defaults. A heap walk (al325) showed most of the remaining process-heap growth was patch 0004's
diagnostic tables, so the package ships `perfLogIntervalFrames = 0`.

### 2026-11-09: hand-over and release

The sims3-modernization maintainers handed the bridge over to Apex Radiance. Its build, patches, configuration and
package moved to `native/remix-bridge/` (`build.cmd`, `package.ps1`), and 3.0.0 published it as a separate download with
Settings > Compatibility naming it and offering it under *Optional* while DXVK runs inside the game.

### 2026-11-09: freeze on a switch to exclusive fullscreen

With the 3.0.0 zip, switching to exclusive fullscreen (or a new resolution) froze the game. The server's log showed
DXVK refusing each `Reset` ("device still has alive losable resources: Remaining resources: 1"), and the game called
Reset again every frame. DXVK's resources are not lost on a Reset, so the package's `dxvk.conf` sets
`d3d9.countLosableResources = False`: DXVK resets anyway and the one resource left alive stays valid. Confirmed in game
the same day; the zip was published again. Which resource stays alive was not found.
