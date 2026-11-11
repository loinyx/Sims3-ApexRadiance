Apex Radiance - RTX Remix bridge for The Sims 3

What it does
The bridge moves the game's renderer (DXVK) out of the 32-bit game into its own 64-bit process. The game keeps
about 600 MB more free memory, which helps long sessions and big worlds. The picture is the same as with DXVK.
It is NVIDIA's RTX Remix bridge with patches for The Sims 3, set to plain DXVK (no path tracing).
Ported to The Sims 3 by dalexlol and alexvixell35-stack in sims3-modernization.

Install
1. Close the game.
2. Open The Sims 3\Game\Bin. If it already has a d3d9.dll (for example DXVK), move it to a backup folder.
3. Unzip this package into Game\Bin, keeping the .trex folder. Bin then holds d3d9.dll, bridge.conf, dxvk.conf and
   .trex\ (NvRemixBridge.exe, d3d9vk_x64.dll, bridge.conf).
4. EA app (TS3.exe) only: in both bridge.conf files, change "client.targetProcess = TS3W.exe" to
   "client.targetProcess = TS3.exe".
5. Start the game. Apex Radiance 3.0.0 or later detects the bridge on its own.

Remove
Close the game, delete d3d9.dll, bridge.conf, dxvk.conf and the .trex folder from Bin, and put your old d3d9.dll back.

Logs
The bridge writes its logs to rtx-remix\logs (bridge32.log, bridge64.log). Attach them, with Apex Radiance's log,
to any bug report.

Licences
RTX Remix bridge (MIT), Microsoft Detours (MIT) and DXVK (zlib): their texts follow below.
