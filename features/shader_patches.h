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
};
bool PatchRoad(std::vector<DWORD>& t, RoadPatch& out);

// World terrain chunks: the constant cK whose .x scales the chunk light map read from `sampler` (read once, by no other
// instruction), -1 when there is none that scales the map alone.
int LightMapScaleConst(const std::vector<DWORD>& t, DWORD sampler);

// Snowy floor tiles: the vertex shader already outputs world xz in TEXCOORD0.zw. The pixel shader lights the floor
// only with the lot light map ("texld rA, v2, s2" ... "mul rB.xyz, rC.w, rA"); add the world light atlas:
// uv = v0.zw * cK.xy + cK.zw, "max rB.xyz, rB, atlas".
struct FloorPatch {
    DWORD atlasSampler = 0;
    DWORD atlasConst = 0;
    DWORD mapSampler = 0; // PatchSnowFloor: the sampler of the room light map it found
    int scaleConst = -1;  // K of the game's lamp scale cK.x applied to max(map, atlas), read by no other instruction; -1 unknown
};
bool PatchFloor(std::vector<DWORD>& t, FloorPatch& out);
// Snow lying on lot floor tiles (LightProbe-m69): the VS writes TEXCOORD7.xy = world xz / 2; the PS gets max(room map, atlas).
// texcoord: where the VS puts world xz / 2 (7 = TEXCOORD7.xy, 0 = TEXCOORD0.zw).
bool IsSnowFloorVs(const std::vector<DWORD>& t, int& texcoord);
bool PatchSnowFloor(std::vector<DWORD>& t, int texcoord, FloorPatch& out);
// Baked-light surfaces (summer outdoor floors): max(light map, atlas at TEXCOORDn.xy = world xz, n from PatchObjectLampVs) before "mad ..., cK.x".
bool PatchBakedAtlasPs(std::vector<DWORD>& t, int texcoord, FloorPatch& out);

// Winter foliage with a shadow map: "lrp rD.w, t5.x, cK.y, rS.w" is the moon shadow that also darkens lamp light.
// After it: rD.w = lerp(rD.w, 1, cN.x), cN.x = night level.
// Winter floor tile vertex shader: TEXCOORD0.zw = world xz (the floor patch reads it).
bool IsFloorVs(const std::vector<DWORD>& t);
bool PatchLeafShadow(std::vector<DWORD>& t, DWORD& nightConst);

// Foliage vertex shaders (summer and winter): "max r0, r0, cK.w" clamps the sun (r0.x) and three lamps (r0.yzw) N.L at 0.
// Lamps get wrap lighting instead (light passing through leaves): r0.yzw = max(N.L / 1.5 + 1/3, 0).
bool PatchFoliageVs(std::vector<DWORD>& t);

// Instanced lot structures (fence rails and posts, railings, stairs: SceneModelArray, one light rig for a whole group at
// its centre). Their vertex shader has POSITION1/POSITION2 instance streams, writes world xz to TEXCOORD1.zw and the 4
// "vertex lights" (only overflow lights, usually none) to COLOR0. True when the VS is one of those.
bool IsInstancedStructureVs(const std::vector<DWORD>& t);
// Their pixel shader adds COLOR0 once ("add rD.xyz, rS, vC"): replace it with max(vC, ground light atlas at the pixel's
// world xz * strength). uv = vT.zw * cA.xy + cA.zw, strength = cB.x.
struct InstancedPatch {
    DWORD atlasSampler = 0;
    DWORD atlasConst = 0;
    DWORD strengthConst = 0;
};
bool PatchInstancedLamps(std::vector<DWORD>& t, InstancedPatch& out);

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
struct ObjectLampPatch {
    DWORD atlasSampler = 0;
    DWORD atlasConst = 0;
    DWORD strengthConst = 0;
    DWORD lampParamConst = 0; // (0, lamp strength, 0, 1e-4)
    DWORD lampConst = 0;      // per lamp: (world pos, 1/R^2), (colour, 0)
    bool rigLamps = true;     // c5..c7 are the rig's lamp colours (false: no lamps in the pixel shader)
};
bool PatchObjectLampPs(std::vector<DWORD>& t, ObjectLampPatch& out);

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
};
bool BasisSamplers(const std::vector<DWORD>& t, int samplers[4]);
bool PatchBasisSmooth(std::vector<DWORD>& t, BasisSmoothPatch& out);
bool PatchIndoorBasis(std::vector<DWORD>& t, DWORD lmSampler, IndoorBasisPatch& out, bool capToFloorMap = true);
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
