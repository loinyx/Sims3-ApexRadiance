# Native post-scene boundary check

The fixture uses a native D3D9 device, the production post_scene.cpp and a 2048x1024 scratch render target.
It writes no game files. 49 checks cover four-effect ordering (AO, smoothing, blur, Color), normal/hidden fallback,
once-per-frame execution, depth rejection/recovery, short-scene/internal/reset guards and actual colour tile recognition.
The correct UI-visible boundary learns a scratch identity; only its first origin-256 tile can start the hidden chain.
Unknown targets/layouts keep the original paths. The actual WorldSession unknown-world readiness gate is exercised.
This is not gameplay/DXVK, pixel parity, real Picture shader or FPS validation.
