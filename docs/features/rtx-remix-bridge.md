# RTX Remix bridge

An optional download that moves the game's renderer out of the 32-bit game into its own 64-bit process. The picture is
the same as with DXVK; the game keeps about 600 to 800 MB more free memory, which matters in long sessions and big
worlds. Apex Radiance detects the bridge on its own and runs under it with every feature.

## Status

| | |
|---|---|
| Availability | Released in 3.0.0 as a separate download (`Apex-Radiance-RTX-Remix-bridge.zip`). Its `dxvk.conf` with `d3d9.countLosableResources = False` (no freeze on a switch to exclusive fullscreen or a new resolution): published again after 3.0.0. A folded install guide in Settings > Compatibility, the uncompressed-textures card and Fullscreen as Borderless under the bridge: Released in 3.1.0. Patches 0011 to 0013, Apex's guard for the old client's vertex input, the bridge logs in captures and the bridge's own Nexus file: Released in 3.1.1 |
| Default | Not installed |
| Menu | Settings > Compatibility: *RTX Remix bridge: Installed* when it runs; while DXVK runs inside the game, the bridge is offered under *Optional* |
| Configuration | `bridge.conf` (in `Game\Bin` and in `Game\Bin\.trex`) and `dxvk.conf` (in `Game\Bin`); nothing in `ApexRadiance.toml` |
| Source | [`native/remix-bridge/`](../../native/remix-bridge/) (build, patches, configuration, package), [`framework/platform.cpp`](../../framework/platform.cpp) (`RemixBridge`), [`framework/d3d9_bootstrap.cpp`](../../framework/d3d9_bootstrap.cpp) |

## The problem

The game is a 32-bit program: everything it loads shares 4 GB of address space (2 GB without the large-address flag).
With DXVK inside the game, DXVK's own image, its heaps and the Vulkan driver take part of that space, next to the
game's textures, meshes and scripts. Long sessions and big worlds then run out of large free blocks, and the game
crashes or stops loading. Measured on one old save with Apex off: 1203 MB as the largest free block after 30 minutes
in-process, against 1877 to 1944 MB after 60 minutes under the bridge ([validation](../validation/rtx-remix-bridge.md)).

## How Apex Radiance solves it

The bridge is NVIDIA's RTX Remix bridge (`dxvk-remix/bridge`, MIT), built with stock DXVK instead of the RTX renderer
and patched for The Sims 3.

1. A 32-bit `d3d9.dll` client in `Game\Bin` takes every Direct3D 9 call of the game and sends it over shared memory to
   `.trex\NvRemixBridge.exe`, a 64-bit process.
2. With `server.useVanillaDxvk = True`, that process draws through stock DXVK x64 (`.trex\d3d9vk_x64.dll`) and presents
   into the game's window. The renderer's memory leaves the game.
3. The client keeps CPU copies of what the game locks. Two patches free the copies nobody has used for 2 s and fetch
   them back on the next lock, so the copies do not eat the memory the bridge gives back.
4. `dxvk.conf` in `Game\Bin` (the server's working folder) repeats DXVK's built-in TS3W profile, which keys on the exe
   name and so misses `NvRemixBridge.exe`; without it the game sees a new graphics card and resets `Options.ini`. It
   also sets `d3d9.countLosableResources = False`, so a Reset succeeds.
5. Apex Radiance sees the bridge (`Platform::RemixBridge`: the loaded `d3d9.dll` exports `remixapi_InitializeLibrary` and
   is not DXVK itself), names it in the log and in Settings > Compatibility, and leaves the Vulkan driver alone
   (`VulkanDriverGuard`).

**Installing** (the README's steps): close the game; download the zip from the latest release; move an existing
`Game\Bin\d3d9.dll` (for example DXVK) to a backup folder; unzip into `Game\Bin` keeping the `.trex` folder. On the EA
app (`TS3.exe`), change `client.targetProcess = TS3W.exe` to `TS3.exe` in both `bridge.conf` files. **Removing:** close
the game, delete `d3d9.dll`, `bridge.conf`, `dxvk.conf` and `.trex` from Bin, and put the old `d3d9.dll` back. The
bridge writes its logs to `rtx-remix\logs`.

**The package** holds only what goes into `Game\Bin`: `d3d9.dll`, `bridge.conf`, `dxvk.conf`, the `.trex` folder
(the server, its `bridge.conf` and DXVK's x64 `d3d9vk_x64.dll`) and one text file, *Apex Radiance RTX Remix
bridge.txt*, with the install steps followed by the licence texts of the bridge, Detours and DXVK. On Nexus Mods the
zip is a separate optional file next to `ApexRadiance.asi` ([release-process.md](../release-process.md)).

**In the menu** (Settings > Compatibility, `BridgeGuide` in [`apex_gui.cpp`](../../apex_gui.cpp)): a folded Advanced section, on Windows only (never under Wine or Proton), that shows nothing until it is opened
(no start note, no pop-up). Without the bridge it is *RTX Remix bridge: how to install*: what the bridge does, that the
picture stays the same and that it replaces the `d3d9.dll` in `Game\Bin`, the steps above (the EA app step for
`TS3.exe`), and *Download* (the releases page) and *Open Game\Bin* (the running exe's folder) buttons. With the bridge
it is *RTX Remix bridge: how to remove*, with the removal steps, the logs to attach to a bug report and *Open Game\Bin*.
Its texts are translated with the rest of the menu.

## Settings

Shipped in the package; players do not need to change them.

| File | Key | Value | Effect |
|---|---|---|---|
| `bridge.conf` | `client.targetProcess` | `TS3W.exe` | The process the client attaches to; `TS3.exe` on the EA app |
| `bridge.conf` | `client.enableDpiAwareness` | `True` | The game runs DPI-aware (without it the picture is scaled and blurry on a scaled desktop) |
| `bridge.conf` | `server.useVanillaDxvk` | `True` | Stock DXVK instead of the RTX renderer |
| `bridge.conf` | `client.freeIdleManagedCopiesSec` | `2` | Frees the copy of a MANAGED texture level not locked for 2 s (patch 0003) |
| `bridge.conf` | `client.freeIdleBufferCopiesSec` | `2` | The same for static vertex and index buffers of 64 KB to 4 MB (patch 0008) |
| `bridge.conf` | `perfLogIntervalFrames` | `0` | Diagnostics off: with them on, their tables fragment the process heap |
| `bridge.conf` | `serverChannelMemSize` | `72MB` | The server's channel |
| `dxvk.conf` | `d3d9.customVendorId`, `d3d9.customDeviceId`, `d3d9.customDeviceDesc` | `10de`, `1080`, `Geforce GTX 580` | DXVK's TS3W profile, so `Options.ini` is kept |
| `dxvk.conf` | `d3d9.samplerAnisotropy` | `16` | The same filtering as the in-process DXVK setup |
| `dxvk.conf` | `d3d9.countLosableResources` | `False` | A Reset succeeds although one losable resource is still alive |

## Compatibility and interactions

- **Apex Radiance:** every feature runs under the bridge. Apex's [Display](display.md) window mode and resolution apply
  through its Reset hook; the bridge's client is the game's `d3d9.dll`. Under the bridge, Apex runs exclusive
  fullscreen as *Borderless*, whether Apex's *Fullscreen* or the game's own request in *Game setting*: an
  exclusive-fullscreen Reset hung the game, and so did the first device creation when the game asked for fullscreen.
- **Sims3SettingsSetter:** its borderless window does not apply under the bridge (no Reset reaches the server). Use
  Apex's window mode instead. Its uncompressed lot and Sim textures were reported to stutter under the bridge
  (unverified on this machine).
- **EA app 1.69.47:** works with `client.targetProcess = TS3.exe` in both `bridge.conf` files.
- **DXVK in the game:** the bridge replaces it; the package's `d3d9.dll` takes its place.
- **Wine and Proton:** not offered under Wine (*Optional* hides it there); untested.
- **Graphics cards:** the server needs a Vulkan-capable card and driver, as DXVK does. The 64-bit server uses about
  3 GB of its own memory.

## Limitations

- When the graphics card or V-Sync is the limit, the bridge adds one refresh of input latency (+4.2 to 4.6 ms at
  240 Hz); when the game is CPU-bound, as usual, the difference is about 0.5 ms.
- In a fixed scene the bridge costs under 0.6 ms a frame. In live play the CAS compositor's readback
  (`GetRenderTargetData`) blocks about 1.9 ms a frame.
- The client's process heap still grows from about 41 to 64 MB over an hour (fragmentation of its own objects), then
  stays flat.
- One run of 11 minutes saw the simulation clock stop for 60 s with a test option that is off in the package; not seen
  since and not understood.
- The bridge's freeze on a switch to exclusive fullscreen is fixed by the `dxvk.conf` setting; which resource stays
  alive at the Reset is unverified.

## Technical reference

### Patches to the bridge (`native/remix-bridge/patches/`)

`build.cmd` fetches `dxvk-remix` at f35b86a (sparse: `bridge`, `public`, `src/util`) and Detours at ea6c4ae, applies the
patches in order and builds x86 and x64 with meson; `package.ps1` makes the zip.

| Patch | Fixes |
|---|---|
| 0001 | `GetDepthStencilSurface` and `GetRenderTarget` on an empty slot returned `S_OK` and NULL; d3d9 returns `D3DERR_NOTFOUND`. The game's `Device_Clear` (`0x60F270`) tests only `hr < 0` and read NULL (access violation at `0x60F299`) |
| 0002 | Counts the client's copies of locked surfaces and buffers, by pool |
| 0003 | `client.freeIdleManagedCopiesSec`: frees idle MANAGED texture copies; a lock fetches the level back (copies of 64 KB and more come from `VirtualAlloc`, so a free returns address space) |
| 0004 | Diagnostics (upload, content and release tables); off in the package |
| 0005 | The client's logger fast-failed (`0xC0000409`) on a line of 4 KB or more (`sprintf_s` into a 4096-byte buffer) |
| 0006 | `ts3TextureCacheShift` (TS3 only, off): the game's resource cache counts a MANAGED texture's pixels against its 200 MB budget although they live in the server |
| 0007 | `ts3TextureKeepAliveSec` (TS3 only, off): keeps destroyed textures for a few seconds |
| 0008 | `client.freeIdleBufferCopiesSec`: frees idle static buffer copies, checked by a CRC32C on each fetch |
| 0009 | `freeIdleManagedCopiesMs` (off): a finer threshold for patch 0003 |
| 0010 | Render target 0 is back buffer 0: the client made render target 0 a separate place-holder, so `GetRenderTarget(0)` and `GetBackBuffer(0, 0)` returned two objects (Apex's scene detection, which compares them, saw no scene and the window stayed black). Apex 3.0.0 also handles the old client (`framework/d3d9_bootstrap.cpp`) |
| 0011 | Render target 0 resets the viewport: `SetRenderTarget(0)` reset the viewport and scissor rect on the server but not in the client's copy, so `GetViewport` / `GetScissorRect` returned the previous target's. Light-map smoothing saves and restores them in the middle of a frame and put the stale viewport back: the sea lost its reflection for one frame now and then. Unverified in game that it is the whole cause |
| 0012 | `SetFVF` and `SetVertexDeclaration` replace each other in the client's copy: `SetFVF` kept the previous declaration, so `GetVertexDeclaration` returned it while an FVF was in use. The High-precision scene's resolve saves the vertex input and restores "declaration if any, else FVF": on loading screens it put a game shader's declaration (`POSITION` float2) back, and the game drew its fixed-function UI (FVF `0x142`) with it, a black top-right quarter (248 of 249 no-scene frames STALE in the trace). Now `SetFVF` clears the declaration and `SetVertexDeclaration` sets the FVF to 0, under the device lock. d3d9 returns a declaration made for the FVF after `SetFVF`; the client returns none, and restoring the FVF gives the same result. The FVF also moved into the client's state, so a state block captures and applies it with the declaration (before, `Apply` put the declaration back and left the FVF of the last call). Apex 3.1.1 also handles the old client (below) |
| 0013 | Idle copy fetches: under the bridge, small parts of the picture (a lamp's sprite, pieces of objects or pillars) blinked now and then with the lot left still, and stopped with `freeIdle*Sec = 0`. The fetch of a freed copy now fails the lock when the server sends no whole copy (before, the lock went on and the next `Unlock` uploaded an unfetched copy over the server's); a freed MANAGED level is always fetched (`DISCARD`, which MANAGED ignores, skipped the fetch and left the rest of the copy undefined); a texture fetch is checked by a CRC32C like a buffer's; and only copies of 64 KB and more, which come from `VirtualAlloc`, are freed (a heap copy gives back almost no address space). Which of these caused the blinking is unverified |

### Diagnostics

- Every capture folder (Report, Recording, Light capture, Lighting snapshot, session) also gets the bridge and DXVK
  logs present in `Game\Bin`: `rtx-remix\logs\bridge32.log`, `rtx-remix\logs\bridge64.log`,
  `NvRemixBridge_d3d9.log` (the server's DXVK) and `TS3W_d3d9.log` (DXVK in-process). A log over 8 MB keeps its last
  8 MB. They are copied on a background thread; `About this capture.txt` lists them with their size and last write
  time (`CopyBridgeLogs`, `features/captures.cpp`).
- **No-scene frame trace** (`features/no_scene_trace.cpp`): armed with every recording under the bridge (and in the
  development build). For frames whose back buffer gets fewer than 4 depth-tested draws (loading screens, the save
  screen), up to 300 per recording, `Recording.txt` ends with a `[no-scene]` section: per frame the state at its start,
  then each `SetRenderTarget(0)`, `SetViewport`, `StretchRect` or `ColorFill` into the back buffer, back-buffer
  `Clear` and back-buffer draw with the viewport, scissor, FVF or vertex declaration (with its position element,
  `POSITIONT` or `POSITION`), shaders and, for `DrawPrimitiveUP` / `DrawIndexedPrimitiveUP`, the first vertex and the
  x / y / 4th-float range of all vertices. The values are those set through Apex's detours (`D3D9Hooks::Trace`; FVF,
  declaration and scissor through `ExtraHooks::EnsureTraceHooks`), not the client's `Get*` answers, which can be stale
  (patch 0011). Each Apex pass that reads the vertex input and viewport back to restore them reports through
  `NoSceneTrace::Saved`; *STALE* marks a declaration read back although the last call was `SetFVF`, *STALE vp* a
  viewport read back that differs from the one set last (the restore then puts old state back). Calls from another
  thread or device are counted per frame, not traced. Reading it: STALE hits with an XYZRHW quad drawn through a
  `POSITION` declaration point to the stale declaration; STALE vp hits to the stale viewport; clean state with no draw,
  `Clear` or copy covering the quarter (and no untraced calls) to undefined `DISCARD` content. It changes no call.
  State blocks, `Reset` and `UpdateSurface` are not followed. Written for the black top-right quarter seen on loading
  screens under the bridge; it found the stale declaration (patch 0012).
- **Vertex input restore** (`ExtraHooks::UseVertexInputSetLast`, `framework/d3d9_extra_hooks.cpp`): every Apex pass
  that saves the vertex input (High-precision scene resolve, light-map smoothing, Picture's depth copy and colour pass,
  ambient occlusion, depth blur, edge smoothing's depth, FXAA and SMAA passes) restores the FVF set last when `SetFVF`
  was called after the last `SetVertexDeclaration`, instead of the declaration the client answers. The `SetFVF` /
  `SetVertexDeclaration` detours note which came last at every call (two relaxed atomic stores); they are installed at
  the first frame under the bridge (`framework/d3d9_bootstrap.cpp`) and otherwise only with the trace. Under d3d9 and
  DXVK the restore is the same (the FVF set last). A state block's `Apply` sets the input without the detours seeing
  it: Apex's own state blocks (RT lamps, ground shade, the menu's ImGui backend) take a mark of what the detours know
  before their work and give it back after `Apply` (`ExtraHooks::MarkVertexInput` / `RestoreVertexInput`). A game state
  block that applies a declaration after a `SetFVF` is still not seen, and the restore would put that FVF back (the
  game is not known to use state blocks; unverified).

### Game addresses (Steam 1.67.2)

| Address | What |
|---|---|
| `0x60F270` | `Device_Clear`: calls `GetDepthStencilSurface` (+0xA0) and reads the surface after testing only `hr < 0` |
| `0x60D290` | `DDS_UploadLevels`: writes a MANAGED texture level once |
| `0x732E90` | `ResourceCache_Trim` (via `ResourceManager_Update`): destroys about 2,300 textures per 600 frames; about 70% come back within minutes |
| `0x619DB0` | `Texture_CacheSizeBytes`: counts a MANAGED texture's pixels in full against `CacheBudget` |
| `0x5FBCE0` | The CAS compositor's 256 x 256 readback (`GetRenderTargetData`), called only from `0x5FDBF0` |

## Rejected approaches

- `freeIdleManagedCopiesMs = 500`: frees 4 times as many copies for a gain inside the spread between runs, and the one
  simulation stall was seen with it. Off.
- `ts3TextureKeepAliveSec = 10`: halves texture re-creation, but over an hour the game's own resources grow 36 MB and
  the largest free block drifts down 154 MB. Off.
- `CacheOverflowBudget` 128 MB, or `CacheBudget` 476 MB: fewer re-uploads, but cached textures outlive the copy free
  (454 MB of copies) or the largest block shrinks 47 MB.
- `clientChannelMemSize` 32 MB: no gain in the largest free block.
- `presentSemaphoreMaxFrames` above 1: more frames ahead, more latency.

Details in [history](../history/rtx-remix-bridge.md).

## See also

- [Validation](../validation/rtx-remix-bridge.md): the runs and their numbers
- [History](../history/rtx-remix-bridge.md)
- [Display](display.md), [Memory budget](memory-budget.md), [Steam Deck and Proton](steam-deck.md) (DXVK detection)
