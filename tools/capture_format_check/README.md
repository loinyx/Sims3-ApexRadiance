# Capture screen-format check

Compile `check.cpp` with MSVC and link `d3d9.lib` and `user32.lib`. No game files are read or modified. The test
creates a hidden native D3D9 HAL device.

Screenshots and videos read the picture as 8-bit `X8R8G8B8`; another back-buffer format (a 10-bit `A2R10G10B10` one set
by another mod) is converted by `StretchRect` on the GPU. The check fills a 10-bit render target and, when the device
accepts it, a 10-bit back buffer, copies each to an 8-bit target with `D3DTEXF_NONE` (screenshots) and
`D3DTEXF_LINEAR` (video) and reads the colour back.

It does not establish behaviour under DXVK or the RTX Remix bridge, or with multisampling.
