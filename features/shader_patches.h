#pragma once
// Bytecode edits of the game's shaders for the "Night Remake" patch (see shader_patches.cpp). Pure functions on
// the D3D9 token stream, so they can be tested outside the game with captured shaders.
#include <windows.h>
#include <vector>

namespace ShaderPatches {

// Roads (any variant drawn with the road vertex shader): after "texld rX, v1, sL" (the road's own copy of the chunk
// light map, followed by "mul rX.xyz, rX, c4.x") take the max with the terrain map bound to extraSampler. When the
// shader has the sidewalk snow blend, sidewalkConst is the constant whose .x mixes the plain road texture back in.
// Road vertex shaders (all seasons and pieces): TEXCOORD1 declared .xy and written by "mad oT1.xy, rA.xzzw, cM, cM.zwzw"
// (terrain uv; cM = c14 in summer, c16 in winter) plus the world matrix rows c8/c10. Alpha-blended sidewalks may
// declare .xyzw with a separate zw opacity UV write using the captured (1, 2) scale. Terrain and lot shaders do not match.
// mapConst = M.
bool IsRoadVs(const std::vector<DWORD>& t, DWORD& mapConst);
struct RoadPatch {
    DWORD lightSampler = 0;
    DWORD extraSampler = 0;
    int sidewalkConst = -1;
    int scaleConst = -1; // N of the lamp scale cN.x after the inserted max, when no other instruction reads cN, else -1
    int shadeSampler = -1; // shade: the walls' shade map read at the light map's uv after the max (PatchTerrainShade), else -1
};
// shade (15/10): also "texld rT, v1, s<E+1>; mul rX.xyz, rX, rT.x" after the max, the walls' shade on the lamp light around lots
bool PatchRoad(std::vector<DWORD>& t, RoadPatch& out, bool shade = false);

// World terrain chunks: an exclusive cK.x lamp-map multiplier, either linear or
// squared through a scalar temporary. Unknown/shared layouts return -1.
int LightMapScaleConst(const std::vector<DWORD>& t, DWORD sampler, bool* squared = nullptr);

// Keep native terrain alpha (solar visibility) when smoothing lamp RGB. A single
// direct light-map read is required; unsupported layouts are left untouched.
bool PatchTerrainNativeAlpha(std::vector<DWORD>& t, DWORD sampler, DWORD& extraSampler);
// Captured single-pass variant: match the native multipass lighting range in daylight.
bool PatchTerrainDaylightRange(std::vector<DWORD>& t, DWORD& blendConst);
// Walls' shade on the lamp light of the world grass around lots (15/10): right after the single light-map read
// "texld rD, uv, s<sampler>", "texld rT, uv, s<shade>; mul rD.xyz, rD, rT.x". The shade map shares the light map's uv (one
// per terrain chunk, lot_light_bridge.cpp ChunkShade). Run after the two patches above (the daylight one matches exact bytes).
bool PatchTerrainShade(std::vector<DWORD>& t, DWORD sampler, DWORD& shadeSampler);

// TS3 object materials that write bloom as "add_sat oC0.w, rL.w, -cK.x": returns K only when that is the shader's
// sole alpha write and cK is not defined/read anywhere else, so temporarily changing cK.x cannot alter RGB.
int BloomThresholdConst(const std::vector<DWORD>& t);

// Snowy floor tiles: the vertex shader already outputs world xz in TEXCOORD0.zw. The pixel shader lights the floor
// only with the lot light map ("texld rA, v2, s2" ... "mul rB.xyz, rC.w, rA"); add the world light atlas:
// uv = v0.zw * cK.xy + cK.zw, "max rB.xyz, rB, atlas".
struct FloorPatch {
    DWORD atlasSampler = 0;
    DWORD atlasConst = 0;
    DWORD mapSampler = 0; // PatchSnowFloor: the sampler of the room light map it found
    int scaleConst = -1;  // K of the game's lamp scale cK.x applied to max(map, atlas), read by no other instruction; -1 unknown
    int heightConst = -1; // PatchBakedAtlasPs, PatchFloor, PatchSnowFloor: cH, the atlas weight saturate(map.a * cH.y + cH.x) ((1, 0) = always); -1 = none
    bool maskAlpha = false; // map.a is readable at the atlas (the floor visibility mask; the winter floors copy it after the map's texld)
    bool loggedMask = false; // lot_light_bridge logged maskAlpha once
    // The ground lamp shade of story-0 floors (13/10): max(map, atlas) is multiplied by 1 + gate (M - 1), M = the lot mask's
    // alpha (ground_lamp_shadow.h, the lot light pass's shade) bound to shadeSampler, gate = 1 within about 1 m of the lot's
    // ground. shadeConst .. + 2 = GroundShadow::FloorShadeRows (mask uv from world x and z, minus the ground height, the gate).
    // -1 = no shade (no world y, or no sampler / constants to spare): the draw binds nothing more.
    int shadeSampler = -1;
    int shadeConst = -1;
};
// Where a vertex shader writes world y to one output component: "mov oK.c, rW.y" with rW.y = "dp4 rW.y, rP, c9" (the
// last write before it) and rW.x / rW.z from c8 / c10, exactly one such write and nothing else writing oK.c, no flow control.
// reg = oK. The winter floor tile shader kFloorVs has it as TEXCOORD1.w.
struct WorldY {
    int texcoord = -1;
    int comp = 0;
    int reg = -1;
};
bool WorldYOutput(const std::vector<DWORD>& t, WorldY& out);
// worldY: where the paired vertex shader writes world y (WorldYOutput); with it, the ground lamp shade (out.shadeSampler).
bool PatchFloor(std::vector<DWORD>& t, FloorPatch& out, const WorldY& worldY = WorldY{});
// Snow lying on lot floor tiles (LightProbe-m69): the VS writes TEXCOORD7.xy = world xz / 2; the PS gets max(room map, atlas).
// texcoord: where the VS puts world xz / 2 (7 = TEXCOORD7.xy, 0 = TEXCOORD0.zw). worldY as PatchFloor (the shade's x and z
// rows are doubled by the caller: the input is world xz / 2).
bool IsSnowFloorVs(const std::vector<DWORD>& t, int& texcoord);
bool PatchSnowFloor(std::vector<DWORD>& t, int texcoord, FloorPatch& out, const WorldY& worldY = WorldY{});
// Baked-light surfaces (summer outdoor floors): max(light map, atlas at TEXCOORDn.xy = world xz, n from PatchObjectLampVs) before "mad ..., cK.x".
// The atlas has no walls, so it is weighted by the floor map's alpha (out.heightConst, out.maskAlpha): level_light_share
// stores there, for outdoor floor texels of the upper stories, the share of their lamps' light that walls block. Story 0's
// floor map is room 0's solve, with no wall test either: with groundShade, max(map, weighted atlas) is then multiplied by the
// ground lamp shade (out.shadeSampler; world y = TEXCOORDn.z, which PatchObjectLampVs writes). groundShade = false gives the
// bytes of the patch before the shade.
bool PatchBakedAtlasPs(std::vector<DWORD>& t, int texcoord, FloorPatch& out, bool groundShade = true);

// Winter foliage with a shadow map: "lrp rD.w, t5.x, cK.y, rS.w" is the moon shadow that also darkens lamp light.
// After it: rD.w = lerp(rD.w, 1, cN.x), cN.x = night level.
// Winter floor tile vertex shader: TEXCOORD0.zw = world xz (the floor patch reads it).
bool IsFloorVs(const std::vector<DWORD>& t);
bool PatchLeafShadow(std::vector<DWORD>& t, DWORD& nightConst);

// Foliage vertex shaders (summer and winter): "max r0, r0, cK.w" clamps the sun (r0.x) and three lamps (r0.yzw) N.L at 0.
// Lamps get wrap lighting instead (light passing through leaves): r0.yzw = max(N.L x c255.x + c255.y, 0), c255 =
// (1/1.5, 0.5/1.5) defined in the shader; runtimeWrap: c255 is not defined, the draw sets it (the daytime curve s:
// (s/1.5, s 0.5/1.5), so the lamps' weights, and so their per-instance colours, are x s; bit for bit the same at s = 1).
constexpr unsigned kFoliageWrapConst = 255;
bool PatchFoliageVs(std::vector<DWORD>& t, bool runtimeWrap = false);

// Instanced lot structures (fence rails and posts, railings, stairs: SceneModelArray, one light rig for a whole group at
// its centre). Their vertex shader has POSITION1/POSITION2 instance streams, writes world xz to TEXCOORD1.zw and the 4
// "vertex lights" (only overflow lights, usually none) to COLOR0. True when the VS is one of those.
bool IsInstancedStructureVs(const std::vector<DWORD>& t);
// Their pixel shader adds COLOR0 once ("add rD.xyz, rS, vC"): replace it with max(vC, ground light atlas at the pixel's
// world xz * strength). uv = vT.zw * cA.xy + cA.zw, strength = cB.x.
// With TEXCOORD2 (world normal xyz, world y w) the patch also gets the objects' per-pixel lamps (pixelLamps; constants
// lampParamConst = (direct mode, lamp strength, 0, 1e-4), lampConst .. + 2 * kObjectPixelLamps - 1 = per lamp (pos, 1/R^2),
// (colour, spill)) and the atlas only on faces turned up: max(lerp(vC, lamps x strength, direct mode), atlas x sat(N.y) x cB.x). Each lamp
// is gated by its cones (07/10): x S + (1 - S) sat(max(l.k1 + k1.w, l.k2 + k2.w)), S = colour .w, k1 / k2 at coneConst.
// A valid group uses pixel lamps; unavailable groups retain the native rig.
// reach (realistic lamp reach, 12/10): the per-pixel lamps follow the law of an outdoor wall texel (level_light_share
// ReachShareImpl, lamp_reach.h): colour (x the game's weight) x sat(N.l) / d^2 x min(1, r0^2 / d^2 x sat(1 - (d^2/R^2)^2)^2),
// the sum held under 1 per channel; r0^2 of lamp k at reachConst + k / 4, component k % 4 (2 rows after the cones). Without
// the per-pixel lamps, or without room for 2 more rows, the reach variant is refused (t unchanged). reach = false gives the
// bytes of the first law exactly.
// groundShade (13/10): with the per-pixel lamps, the sum max(lamps, atlas x sat(N.y) x cB.x) is multiplied by the lot ground's
// lamp shade as on the story-0 outdoor floors (FloorPatch::shadeSampler): 1 + gate (M - 1), M = the lot mask's alpha bound
// to shadeSampler, shadeConst .. + 2 = GroundShadow::FloorShadeRows (xzScale 1; world x, z from TEXCOORD1.zw, y from
// TEXCOORD2.w). -1 = no shade (no per-pixel lamps, or no sampler / constants to spare): the bytes of the patch without it.
struct InstancedPatch {
    DWORD atlasSampler = 0;
    DWORD atlasConst = 0;
    DWORD strengthConst = 0;
    bool pixelLamps = false;
    DWORD lampParamConst = 0;
    DWORD lampConst = 0;
    DWORD groundWeightConst = 0; // (a, b, 1, 0): atlas weight sat(N.y * a + b); (1, 0) with lamps, (0, 1) = every face (as before)
    DWORD coneConst = 0;         // per lamp k: its cones at coneConst + 2k / + 2k + 1 (= lampConst + 2 kObjectPixelLamps); the lamp colour .w = its spill
    DWORD reachConst = 0;        // reach variant: the 2 r0^2 rows (= coneConst + 2 kObjectPixelLamps); 0 = the first law
    int shadeSampler = -1;       // the ground lamp shade: the lot mask (atlasSampler + 1); -1 = none
    int shadeConst = -1;         // its 3 rows, right after the cones (first law) or the reach rows
};
bool PatchInstancedLamps(std::vector<DWORD>& t, InstancedPatch& out, bool reach = false, bool groundShade = true);

// Snow lying on objects (fence tops, rails, props: the winter snow-cover mesh drawn over the object). Its vertex shader
// morphs between two positions with flags in TEXCOORD1.x ("slt rX, cK, v.x") and writes world xz to TEXCOORD3.xy
// ("mov oT3.xy, rW.xzzw", rW from dp4 with c8 / c10). True when the VS is one of those.
bool IsSnowCoverVs(const std::vector<DWORD>& t);
// Its pixel shader lights the snow with the moon and the sky only: "mad rL.xyz, rX.w, c0, rY" and then
// "mul oC0.xyz, rL, rA" (rA = the snow texture). Before that mul: rL.xyz += ground light atlas at the pixel's world xz
// * strength, like the ground formula (light map + ambient). uv = vT.xy * cA.xy + cA.zw, strength = cB.x.
struct SnowCoverPatch {
    DWORD atlasSampler = 0;
    DWORD atlasConst = 0;
    DWORD strengthConst = 0;
};
bool PatchSnowCover(std::vector<DWORD>& t, SnowCoverPatch& out);

// Snow with procedural relief on objects (stair tops, LightProbe-neve-escada: "rep" loops of 3D noise). Its vertex shader
// writes world xz * 0.5 to TEXCOORD4.zw ("mul oT4.zw, rW.xyxz, cD.x", cD.x = 0.5 from a def, rW from dp4 with c8 / c10)
// and takes the snow's base position in TEXCOORD2 (roofs use the same family without it and have their own fix).
bool IsSnowReliefVs(const std::vector<DWORD>& t);
// Its pixel shader lights the snow with the moon and the sky only: "texld rC, rN, s0" (sky cube at the normal) then
// "mad rL.xyz, rC, cK.x, rS". After that mad: rL.xyz += ground light atlas at the pixel * strength, uv = vT.zw * cA.xy +
// cA.zw (the caller doubles cA.xy: vT.zw is world xz / 2), strength = cB.x.
bool PatchSnowRelief(std::vector<DWORD>& t, SnowCoverPatch& out);

// Objects lit by a per-object rig (doors, windows, sofas, counters: LightProbe-porta/janela/sofa/balcao). Vertex shader:
// no instancing (POSITIONn only with NORMALn: morphs are fine), world position outside any branch (skinned ones with
// mova / if b0 are fine), one world position temp (dp4 rW.x/y/z with the same source and consecutive constants
// cK..cK+2; several: the one from POSITION, else the one not computed from another). Adds TEXCOORD8.xyz = world
// (x, z, y) right after those dp4s; with texcoordOut (floors) the first free TEXCOORD from 7 instead. worldConstOut =
// K (c[K..K+2].w = translation); vertexLightOut = first colour constant of the rig's 4 vertex lights (COLOR0), or -1.
// needColor0: only vertex shaders with a COLOR0 output (the object rig family); floors have none. False (t unchanged)
// when the pattern is not there.
bool PatchObjectLampVs(std::vector<DWORD>& t, bool needColor0 = true, int* texcoordOut = nullptr, int* worldConstOut = nullptr, int* vertexLightOut = nullptr);
// Their pixel shader: diffuse = sun (dp3 rN, c9 / c0 / c13) + 3 lamps (chain ending "mad rD.xyz, rS.w, c7, rD"), then
// "mad rA.xyz, rCube, cK.x, rD" (sky) and "add rA.xyz, rA, vC" (COLOR0 vertex lights); or the same chain with no sky
// (light = rD); or no lamps at all, light = max(vC, light map, sky). The lamp part becomes max(lamps + per-pixel world
// lamps * cS.y, ground light atlas at the pixel * (0.5 + 0.5 N.y) * cB.x) (minus vC where vC is added after).
// World position from TEXCOORD8 (PatchObjectLampVs); uv = xz * cA.xy + cA.zw.
// Per-pixel lamps in the object pixel patch (constants lampConst .. lampConst + 2 * kObjectPixelLamps - 1).
inline constexpr unsigned kObjectPixelLamps = 8;
// reach: the outdoor walls' law, as PatchInstancedLamps (r0^2 rows at reachConst = coneConst + 2 kObjectPixelLamps, so
// the parameters, lamps, cones and r0^2 rows are one upload from lampParamConst); refused (t unchanged) without room for them.
struct ObjectLampPatch {
    DWORD atlasSampler = 0;
    DWORD atlasConst = 0;
    DWORD strengthConst = 0;
    DWORD lampParamConst = 0; // (direct mode, lamp strength, 0, 1e-4)
    DWORD lampConst = 0;      // per lamp: (world pos, 1/R^2), (colour, spill)
    DWORD coneConst = 0;      // two cone rows per lamp, evaluated at the pixel
    DWORD reachConst = 0;     // reach variant: the 2 r0^2 rows; 0 = the first law
    bool rigLamps = true;     // c5..c7 are the rig's lamp colours (false: no lamps in the pixel shader)
};
bool PatchObjectLampPs(std::vector<DWORD>& t, ObjectLampPatch& out, bool reach = false);

// Indoor room light: the 4 directional room light maps ("basis maps", 64x64) read with a smooth (bicubic) filter.
// BasisSamplers: the samplers of a basis-reading pixel shader, by direction (+X, -X, +Z, -Z). PatchBasisSmooth: its 4
// basis reads become bicubic reads. PatchIndoorBasis: an indoor rig-lit object shader whose diffuse lamp chain (c4..c7)
// is replaced by the 4 basis maps (bound to firstSampler .. +3 in +X, -X, +Z, -Z order) weighted by its world normal,
// times strengthConst.x; lmSampler = the sampler that holds the room light map (the basis uv is its uv).
struct IndoorBasisPatch {
    DWORD firstSampler = 0;
    DWORD strengthConst = 0;
    DWORD sizeConst = 0;   // set per draw: (w, h, 1/w, 1/h) of the basis maps
    int cubeWeightConst = -1; // the game's constant whose .w weights the ambient cube (the first cube read times c.w), -1 = none
    int tintConst = -1;       // set per draw: .x = how much of the cube's own colour stays (1 = all; 0 = its grey), .yzw = a colour
                              // the cube is then multiplied by (1, 1, 1 = unchanged; MUST be set), -1 = no cube
    int uvUsage = -1, uvIndex = -1; // the semantic of the room light map uv input (-1: not a declared input)
    int diffuseConst = -1;    // set per draw: the 4 rig colours the diffuse chain reads instead of c4..c7 (lamps at 0)
};
// The two VS constants whose dp4 with the position writes .x and .y of the output with that semantic (the room light map
// uv: uv.x = dot(pos, cX), uv.y = dot(pos, cY)); false when the uv is not made that way
bool UvRowConsts(const std::vector<DWORD>& vs, int usage, int index, int& cX, int& cY);
struct BasisSmoothPatch {
    DWORD sizeConst = 0;   // set per draw: (w, h, 1/w, 1/h) of the basis maps
    DWORD sizeSampler = 0; // a basis map sampler (to read that size)
    int maskSampler = -1;  // wallMask: bind the story's wall mask here (point filter, clamp), -1 = none
    int floorSampler = -1; // floorWall: the floor light map read through the uv's zw, -1 = none (no such read, or no room)
    int floorConst = -1;   // floorWall: set per draw: (sx, sy, on, 0); floor uv = basis uv x sxy; on 0 = the game's read
    int ambSampler = -1;   // stairRoomFill: bind the story's ambient per tile here (A32B32G32R32F 64 x 64, point, clamp), -1 = none
    bool floorTone = false; // floorTone: the floors' tone curve was put on the colour (InteriorFloor's, about x1.46 in the dark)
    int upSampler = -1;     // storyBlend: the story above's directional map for direction d (+X -X +Z -Z) at upSampler + d
    int upAmbSampler = -1;  // storyBlend: the story above's ambient map (as ambSampler's)
    int storyConst = -1;    // storyBlend: set per draw: (yLow + 0.5, 1 / (yUp - yLow), on, 0), heights in world metres
    int basisSampler[4] = {-1, -1, -1, -1}; // this story's directional map samplers by direction (to find the story above's)
    int blendRefused = 0;   // storyBlend not made: 1 no sampler or constant room, 2 no normal input (nrm of an input), 3 no light max after the floor read, 4 no dcl
};
bool BasisSamplers(const std::vector<DWORD>& t, int samplers[4]);
// preserveShadows caps each smoothed channel by the normalized native bilinear read.
// It prevents the wider filter adding colour, but cannot reconstruct geometry missing from the map.
// wallMask (development builds, basis_wall_filter): every basis read, the native ones too, takes a uv that never blends
// towards a texel across a wall (BasisWallUv; the mask at maskSampler: r -x, g +x, b -z, a +z walled, the maps' layout).
// floorWall (with wallMask): the floor light map read through the same uv's zw (a stair's top step) keeps to its own tile
// at a walled side too (half a floor texel inside it; the lower texel in the wall's thickness).
// stairRoomFill (with floorWall): the ambient cube fill becomes the tile's room ambient x the floor map's alpha, and the floor read
// takes map.rgb + map.a x ambient (the floor's own law), so a stair has the colour of the floor and walls around it.
// floorTone (with stairRoomFill): the floor shaders' tone curve on the final colour (f = (2 / (1 + exp(-4.2 l)) - 1) / l, l = rgb / 3 summed),
// which the object shaders a stair is drawn with lack: the same texture under the same light looks as it does on the floor.
// storyBlend (with stairRoomFill): the light blended by the pixel's height (the w of its normal input) with the story above's maps.
// All options off (the default) = the plain bicubic copy.
struct BasisSmoothOptions {
    bool preserveShadows = false;
    bool wallMask = false;
    bool floorWall = false;     // takes effect with wallMask
    bool stairRoomFill = false; // takes effect with floorWall (stair_room_fill)
    bool floorTone = false;     // takes effect with stairRoomFill
    bool storyBlend = false;    // takes effect with stairRoomFill
};
bool PatchBasisSmooth(std::vector<DWORD>& t, BasisSmoothPatch& out, const BasisSmoothOptions& options = {});
// Whether the vertex shader passes the world height in its TEXCOORD2.w ("mov oN.w, rX.y" after "mov oM.zw, rX.xyxz", oN the
// TEXCOORD2 output): the stair VSs (22EA00C8, 23C16560)
bool VsPassesWorldY(const std::vector<DWORD>& vs);
bool PatchIndoorBasis(std::vector<DWORD>& t, DWORD lmSampler, IndoorBasisPatch& out, bool capToFloorMap = true);
// Smooth light maps (2.12.4): the room light map read of the room-lit pixel shaders. The pattern (docs/features/night-
// lighting/room-map-smoothing.md; offline over every precomp pixel shader it matches InteriorFloor, InteriorWall and their
// siblings that read the same room map, nothing else): a plain "texld rL, <TEXCOORD1>, sM" (t1 in ps_2_0, the v# declared
// texcoord1 in ps_3_0; sM a 2D sampler read once), whose next use of rL, after at most one "mul rL, rS.c, rL", is
// "mad rD.xyz, rL.w, cA, rL" with cA not def'd (rgb + alpha x InteriorBuildingAmbientColor). PatchRoomMapSmooth replaces
// that read with a cubic B-spline read (4 bilinear taps, all four channels), the taps' v kept inside the texel band
// bandConst describes; a ps_2_0 shader becomes ps_2_x (about 45 more instructions, 11 more temps: check the device's
// PS20Caps). Per draw: sizeConst = (w, h, 1/w, 1/h) of the map at `sampler`; bandConst = (1/S, S, 0.5, S - 1), S = the
// texel rows of one block (wall atlas: 3n + 1; floors: 65536, one block).
struct RoomMapPatch {
    DWORD sampler = 0;
    DWORD sizeConst = 0;
    DWORD bandConst = 0;
    int temps = 0; // temps the copy uses (r0 .. r(temps - 1))
    int slots = 0; // its instructions (no dcl, no def)
    bool ps2x = false;
    int archConst = -1; // archAmb: kArchCells x 2 rows set per draw: (a.x, a.z, e.x, e.z), (D.rgb, 1 / |e|^2); -1 = none
    int ambConst = -1;  // archAmb: the constant holding the room's ambient colour (cA)
};
bool IsRoomMapPs(const std::vector<DWORD>& t);
// archAmb (development builds, arch_ambient): after the read, the share of the neighbours' ambient near the room's arches
// (lot position from the floor map uv, 4 texels a metre; arch_ambient.h's law), from archConst; all D = 0: no change
constexpr unsigned kArchCells = 6; // (6 since dev.31: the edges of a floor opening go with the arches, opening_bounce)
constexpr float kArchReach = 1.5f; // = ArchAmbient::kReach
bool PatchRoomMapSmooth(std::vector<DWORD>& t, RoomMapPatch& out, bool archAmb = false);
// The scale of the room map uv a vertex shader writes: "mul o<TEXCOORD1>.xy, v#, cK.c" (either order), the only write of
// that output's xy. The game's room-lit vertex shaders: 1/4096 for the wall atlas, 1/32767 for the floor maps (a def in
// vs_3_0, a constant it may set in vs_2_0: read it at the draw when not defined).
struct RoomMapUv {
    int constReg = -1, comp = 0;
    bool defined = false;
    float value = 0.0f;
};
bool RoomMapUvScale(const std::vector<DWORD>& vs, RoomMapUv& out);
// Packed interior-floor positions and static room UVs; these can also match the generic object VS detector.
bool IsPackedRoomFloorVs(const std::vector<DWORD>& vs);
struct WallMatrices { int clip = -1, world = -1; };
// Consecutive clip/world dp4 rows over the same position in a wall-map vertex shader.
bool FindWallMatrices(const std::vector<DWORD>& vs, WallMatrices& out);
// An object shader lit by the rig (Rooms at Night, lot_light_bridge): whether its diffuse sums the four rig light colours
// c4..c7 (room-mode rigs have no sun: c4 = the strongest room light), and the constant whose .w weights its ambient cube (-1 = none)
struct RigPsInfo {
    bool rigLights = false;
    int cubeWeightConst = -1;
};
bool AnalyzeRigPs(const std::vector<DWORD>& t, RigPsInfo& out);
// The same object shader with its ambient cube's colour pulled towards its grey (Rooms at Night's Blue tint on the game's
// own shaders): after that cube read, lrp(tintConst.x, cube, luma(cube)) x tintConst.yzw; tintConst is set per draw
// ((1, 1, 1, 1) = unchanged; .yzw at 0 would make the cube black)
bool PatchCubeTint(std::vector<DWORD>& t, int& tintConst);

// Windows take outdoor light (experimental, lot_light_bridge DrawWindowOutdoor). A window turned with its origin inside
// the room is a room-mode (rig mode 0) object whose part is drawn by a basis-reading object shader: its faces turned out
// of the house read the room's directional maps towards the outside, which hold no light (black). The patch keeps the
// game's light and, after the shader's last light max before oC0 ("max rL.xyz, .., .."), adds
//   rL = max(rL, (ground atlas(world.xz) x (0.5 + 0.5 N.y) x cB.x + per-pixel lamps x cS.y) x w)
// with w = sat(1 - plan alpha at (pixel + N x d)): the basis maps' alpha is 1 on the house plan and 0 outside, read
// at the basis uv moved by the world normal (planConst / planConst + 1 = the rows of world xz -> uv offset, d included,
// .z = 0, set per draw). So faces looking into the house keep the game's light exactly; faces looking out of it take the
// outdoor terms the outdoor-object patch uses. World (x, z, y) from TEXCOORD8 (PatchObjectLampVs); ps_3_0 without flow
// control only; the normal = the input the shader normalizes ("nrm rN.xyz, vK"). False (t unchanged) otherwise.
// reach: the outdoor walls' law, as PatchInstancedLamps (r0^2 rows at reachConst = planConst + 2); refused (t unchanged)
// without room for them.
struct WindowOutdoorPatch {
    DWORD atlasSampler = 0;
    DWORD atlasConst = 0;     // uv = world.xz x .xy + .zw
    DWORD strengthConst = 0;  // .x = ground strength
    DWORD lampParamConst = 0; // (direct mode, lamp strength, 0, 1e-4)
    DWORD lampConst = 0;      // per lamp: (world pos, 1/R^2), (colour, spill)
    DWORD planConst = 0;      // 2 constants: (du/dx, du/dz, 0, 0) x d, (dv/dx, dv/dz, 0, 0) x d
    DWORD reachConst = 0;     // reach variant: the 2 r0^2 rows; 0 = the first law
    int uvUsage = -1, uvIndex = -1; // the semantic of the basis uv input (the VS output to read the uv rows from)
};
bool PatchWindowOutdoor(std::vector<DWORD>& t, WindowOutdoorPatch& out, bool reach = false);
// The vertex shader side: how its output (usage, index) .xy is made from the world position, either "dp4 oN.x/.y, rW,
// cK" or "dp4 rA.c, rW, cK" then "mul oN.xy, rA.<cc>, cS.<ss>" (the game's basis uv: room light map uv x c.zw). rW must
// be the world-position temp of the triple c[worldK .. worldK + 2]. row[k] = K, scale[k] = S (-1: none), scaleComp[k] =
// the component of cS. False when the output is made another way.
struct UvRows {
    int row[2] = {-1, -1};
    int scale[2] = {-1, -1};
    int scaleComp[2] = {0, 0};
};
bool WorldUvRows(const std::vector<DWORD>& vs, int usage, int index, int worldK, UvRows& out);

// Banding Fix (scene dither, scene_dither.cpp): every write to oC0 goes to a free temp rO, and at the end
// oC0.rgb = rO.rgb + t(vPos) * cA.x, oC0.a = rO.a (the alpha is the bloom mask: untouched). t = triangular noise in
// (-1, 1) from interleaved gradient noise (a fixed per-pixel pattern, no time); cA.x = the amount in output units, set per
// draw (cA.w = a phase added to the pattern: 0 = still, a new value each frame = moving grain) (*amountConst = A; 1/255 = one 8-bit step). The 8-bit steps of smooth lighting become an invisible grain.
// ps_3_0 only (vPos); refused (t unchanged): no oC0 write, subroutines or ret, relative constant addressing, no free
// temp / constant.
enum class DitherResult { Ok, NotPs30, NoColorWrite, Subroutines, RelativeConstants, NoFreeRegister, Unreadable };
DitherResult AddDither(std::vector<DWORD>& t, int* amountConst = nullptr);
// The same grain for ps_2_0 / ps_2_x, which have no vPos: the pixel position comes from TEXCOORDk = the clip position
// (k = the highest texture coordinate the shader does not use, *texcoordOut), written by a copy of the paired vertex shader
// (AddScreenPosVs(vs, k)). cA = (amount, width / 2, height / 2, 0), set per draw. The copy is ps_2_x (more slots and
// temps: the grain does not fit in some ps_2_0). Refused: no free texture coordinate, temp or constant (32), subroutines.
// The end writes oC0 once, fully.
DitherResult AddDither2(std::vector<DWORD>& t, int* amountConst = nullptr, int* texcoordOut = nullptr);
// vs_1_1 / vs_2_x: every write to oPos goes to a free temp, then oPos and oTk = that position. False when oTk is
// already written, no free temp, subroutines, or another version (vs_3_0 pairs with ps_3_0 only).
bool AddScreenPosVs(std::vector<DWORD>& t, int texcoord);

// AO receiver mask: paired shader copies preserve geometry/alpha rejection, writing clip z/w to colour.
// Opaque R/G hold body/hair depth (other channel = 1) for MIN blending.
// Transparent R uses signed depth (negative hair); G retains original alpha coverage.
// Rejects unsupported models, occupied interpolators, extra colour/depth outputs and early returns.
// Inputs remain unchanged on failure.
bool MakeAoReceiverMask(std::vector<DWORD>& vs, std::vector<DWORD>& ps, bool hair = false, bool transparent = false);

// Temporal anti-aliasing (temporal_aa.cpp): every write to the position output (oPos, or the vs_3_0 output declared
// POSITION0) goes to a free temp rP, and at the end rP.xy += c[jitterConst].xy * rP.w, then position = rP: the image moves
// by c.xy in clip units (2 / width = one pixel), the depth is unchanged. Any vertex shader version. Refused (t unchanged):
// subroutines or ret, no position write, only plain copies of an input (pre-transformed full-screen passes), no free temp,
// or the constant already used. Composes with AddScreenPosVs applied after it (TEXCOORDk gets the moved position).
enum class JitterResult { Ok, NotVertexShader, Unreadable, NoPosition, Subroutines, PassThrough, NoFreeRegister, ConstantInUse, ProjectedWater };
JitterResult AddJitterVs(std::vector<DWORD>& t, int jitterConst);

} // namespace ShaderPatches
