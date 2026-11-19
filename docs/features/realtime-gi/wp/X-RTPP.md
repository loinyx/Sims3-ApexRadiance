# X-RTPP: salvage of per-pixel ray-traced lamp shadows

- Status: done (desk). Branch `gi/x-rtpp`.
- Deliverable: [research 16](../research/16-salvage-per-pixel-rt.md).
- Acceptance: variants ranked, work packages of at most 2 h with smoke tests listed, every DXVK statement names its
  file and line at `v3.1.1` or is marked inferred or open.

## Findings

- Verified: R-08's costs belong to rays inside the game's translated shaders. A post-scene compute pass issued by
  Apex needs no shader-translator change.
- Verified: DXVK `v3.1.1` already has what a small patch needs: an optional-extension table
  (`dxvk_device_info.cpp:1024`), `EmitCs` from an interop method (`d3d9_interop.cpp:216-233`), and
  `DxvkContext::beginExternalRendering` with `acquireExternalResource` (`dxvk_context.h:113-123, :816-834`), used by
  the Direct3D 9 front end itself (`d3d9_device.cpp:5421-5429`).
- Verified: the stock interop interface has `WaitForResource`, which answers "Direct3D 9 has no shared fence" for
  the second-device route; DXVK exports `OPAQUE_WIN32_KMT` handles (`d3d9_common_texture.cpp:411-417`).
- Inferred: route D (DXVK from source with patches 0001 ray extensions and 0002 external pass, about 150 lines) is
  first: same frame, no cross-device sharing, no copy. Route B-P (second device, primary rays from the proxy,
  reprojected compose) is the fallback without a DXVK build.
- Inferred: tracing only delta casters (Sims, furniture detail) at quarter resolution with reach culling costs
  0-0.4 M rays a frame at 4K and 0 on a still picture; accumulate-then-freeze removes shimmer on still pictures.

## Open issues

- Nothing was built or measured. The patch sizes, the DXVK build with the bridge's toolchain and every figure of
  research 16 §5 are estimates.
- The compose constant `A` needs look pairs (XR-10). Sim capsules wait for S3.
- Not folded into `plan.md` or `design.md`: research 16 §3.5 proposes to replace research 14 WP11 and reopens the
  device question of server-export §4 behind gate G-dxvk.

## Unblocks

XR-04 and XR-05 (offline, on R-02's harness) and XR-01 (DXVK from source) can start now. XR-02 and XR-03 follow
XR-01. R-08's verdict stays for rays inside game shaders.
