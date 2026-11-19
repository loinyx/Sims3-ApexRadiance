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

### 2026-11-15: rewind mode for test harnesses

A world load takes minutes, and a test that needs the same start state pays it every time. A harness that snapshots
the game process and puts its memory back avoids the load, but under the bridge the renderer lives in the server,
which a rewind of the game alone leaves in the later state: other data queue positions, ids handed out twice,
destroyed objects the game uses again. Patch 0015 adds the server's side, off unless `rewindMode = True`.

The first version left render target contents alone and counted the writes it could not undo. That is a silent
difference in the picture, so the patch now copies render targets and depth surfaces at the snapshot and refuses
what it cannot put back exactly (multisampled targets, volume writes). The client code showed that nothing limits
D3D9 calls to one thread: a command takes only its channel's send lock and `Present` does not take the device
lock. The parked client therefore holds both send locks, and the server reads no command between a rewind and the
client's return to the snapshot.

The first run in game refused its snapshot: the copy of a 1024x768 D24S8 depth surface failed with
`D3DERR_INVALIDCALL`. The game makes its depth surfaces with Discard (`0x617C5F` passes TRUE), and `StretchRect`
refuses such a source in d3d9 and in DXVK. D3D9 also leaves a Discard surface undefined after a `Present`, which is
where a snapshot is taken, so there is nothing to keep as long as the game clears the surface before it draws with
it. The server now checks exactly that in the command stream and gives the snapshot up when a draw comes first.

### 2026-11-16: the server waited for a client that could not answer

The first rewind that went through in game left the server silent for 60 s. After a rewind the server reads no
command until the client is back at its snapshot, and it waited for an event the client set once it saw another
event from the server. The host tool puts kernel objects back as well as memory, so it unset the server's event with
everything else, the client never saw it, and the game hung in its next call until the server's time limit. The
state now lives where a restore is the signal: a spare word in the client's channel mapping that the server sets and
the host's restore clears. A `Present` that waits for the server also answers a park request now, since a host
that cannot park the game cannot tell a slow server from a stopped one.

### 2026-11-16: a new device with no reference of its own

One game start crashed while the device was made: an access violation reading 0 in the client's `GetBackBuffer`,
called from a mod's `CreateDevice` hook. The server's log showed the client destroying the implicit depth surface and
swapchain right after `CreateDevice`. Patch 0010 had made render target 0 the swapchain's back buffer but still
counted it among the references the implicit objects hold on the device, and a back buffer references its swapchain.
So the device read its creator's reference as an implicit one: `AddRef` returned 1, and the matching `Release` found
the count at the implicit references and tore the device's implicit objects down. Code that already holds a second
reference does not trigger it. Why only some starts took and returned a reference that early is not established.
The fix takes the render target out of the count; it is part of patch 0010 on the release branch.

### 2026-11-16: a texture that cannot be locked

Fourteen seconds after a snapshot a 4096x1 A32B32G32R32F surface was written and the server gave the snapshot up:
it keeps the old contents of a surface by locking it, and the lock failed. A level of a default pool texture that is
not dynamic cannot be locked, in d3d9 and in DXVK; `UpdateTexture` and `UpdateSurface` are how such a texture is
written. `StretchRect` does copy one into a render target of the same format, and a render target can be read, so
the server takes that way round before the write and puts the contents back with `UpdateSurface`. Compressed
formats have no render target and still give the snapshot up.

### 2026-11-19: graphics options reset on a start from another folder

A player's diagnostics showed the game seeing a GeForce GTX 1050 Ti (`0x1c82`) on one start and the usual GeForce GTX 580
(`0x1080`) on the others; the server's DXVK log of that start showed no device override, and the change of `lastdevice` resets
`Options.ini`. DXVK reads `dxvk.conf` from the working folder (`DXVK_CONFIG_FILE` overrides it, DXVK 3.1.1
`Config::getUserConfig`), and the client starts the server with the game's working folder, so a start from another folder
than Bin found no `dxvk.conf`. Which folder that start used is unverified. Patch 0016 points the server at the `dxvk.conf`
next to the client. The package's `dxvk.conf` keeps the profile's ids but no longer sets its name: the game finds device
`0x1080` in `GraphicsCards.sgr` and takes the tier from that entry, so the name only reaches the logs (verified in the
1.67 script files; in game unverified).
