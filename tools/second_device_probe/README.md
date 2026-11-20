# Second device probe

An in-game test helper for [Display](../../docs/features/display.md) and the RTX Remix bridge's second-device answer
([rtx-remix-bridge.md](../../docs/features/rtx-remix-bridge.md)): an ASI that makes and releases a throwaway Direct3D 9
device the way Turbo Simulation Engine does, at a chosen moment. Never shipped.

| `SecondDeviceProbe.ini` `[probe] when=` | The device is made |
|---|---|
| `before` (default) | right after Apex logs `Game device created`: before the game's first frame |
| `after` | 600 ms after Apex logs `First Present` |

Build with the command in `probe.cpp`'s header, copy `SecondDeviceProbe.asi` and its ini into `Game\Bin` for the run
and remove both after. It writes `SecondDeviceProbe.log` beside itself: the `CreateDevice` result and how many
references were left on the device and its `IDirect3D9` after its own releases.

Under the RTX Remix bridge, `before` makes a real second device (Apex's answer needs the game's first frame) and the
bridge stops: run `before` on the game's own Direct3D 9 or DXVK.
