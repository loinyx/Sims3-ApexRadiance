ReShade add-on API headers (`include/` of https://github.com/crosire/reshade, commit 7bf9de8), BSD-3-Clause OR MIT, see
`LICENSE.md`. Only the type headers are kept: Apex Radiance calls the exported `ReShade*` functions itself
(`features/reshade_bridge.cpp`) and never includes `reshade.hpp` or the ImGui overlay header.
