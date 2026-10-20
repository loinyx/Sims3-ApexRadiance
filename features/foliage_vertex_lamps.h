#pragma once
// Per-vertex lamp light on instanced foliage (numbered development builds, switch foliage_vertex_lamps).
//
// The foliage vertex shaders (vs_2_0, docs/features/night-lighting/foliage.md) light every leaf of an instance with one
// colour per lamp (c54..c56[a0.w]) computed at the rig's box point nearest the lamp, and one direction (c27..c29[a0.w]): a
// leaf at the top of a crown is as bright as one next to a lamp at ground height. Patch (applied to the copy
// ShaderPatches::PatchFoliageVs made), for each of the three lamps, per vertex:
//   - the real direction and distance from the vertex to the lamp head; Lambert N.l with the wrap constants c255;
//   - the reach law (LampReach::PixelLaw without its gain) at the vertex, relative to the law at the point the colour was
//     taken: colour x law(v) / law(point), capped so no channel passes 1 (the rig's own cap), the hue kept;
//   - leaf transmission: tau/rho x max(-N.l, 0) (thin green leaves: reflectance about 0.12, transmittance about 0.10 at
//     550 nm, PROSPECT, Jacquemoud & Baret 1990; the pixel shader multiplies by the leaf colour, so the light through a
//     leaf is tinted by it);
//   - the crown's own leaves on the way: Beer-Lambert along the vertex -> lamp segment inside the crown ellipsoid,
//     extinction G x Omega x leaf area density (foliage_canopy.h);
// and, for the first (strongest) lamp only, light reflected by the ground under the vertex: the ground's irradiance from
// the lamp x rho_g / 2 x (1 - n.y) (Lambertian plane view factor; grass reflectance about 0.15 in the visible).
// The sun, the sky, the fog and every other output are untouched.
//
// Constants (B = 1 + the highest constant the shader uses; 130 in the captured VS_9A10A3A8; stride 3 per instance so the
// shader's own instance index a0.w addresses them like c0 / c27 / c54):
//   c[B + 3i + k]       lamp k of instance i: xyz head, camera-relative; w = r0^2
//   c[B + 27 + 3i + k]  x = 1/R^2, y = scale (0: lamp unused), z = minimum d^2, w = head height over the instance's ground
//   c[B + 54 + 3i]      crown centre, camera-relative; w = 1 / horizontal semi-axis
//   c[B + 55 + 3i]      x = 1 / vertical semi-axis, y = extinction x leaf area density / ln 2 (exp is base 2)
//   c[B + 56 + 3i]      x, y, z = the cap of lamps 0, 1, 2 (1 / the brightest channel of the colour); w = tau/rho (0: none)
//   c[B + 81]           x = tau/rho, y = rho_g / 2, w = 1
// The draw sets every one of them or does not use the patched copy. Credits: @loinyx
#include <cstdint>
#include <vector>

namespace FoliageVertexLamps {

constexpr float kTransmitOverReflect = 0.83f; // tau / rho, green leaf at 550 nm
constexpr float kGroundHalfAlbedo = 0.075f;   // rho_g / 2, grass in the visible (0.15)

// Patches the token stream in place (false: unchanged; the shader is not of the captured family or would not fit
// vs_2_0). base: B above.
bool Patch(std::vector<uint32_t>& tokens, int& base);

} // namespace FoliageVertexLamps
