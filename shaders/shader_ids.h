#pragma once
// Identifiers of the game's shaders that Night Remake recognises by exact bytecode (Steam 1.67.2.024037): size in bytes
// and FNV-1a 32 over the bytecode's DWORDs. Only the identifiers are kept here, never the game's bytecode itself.
#include "shader_structure.h"
#include <cstdint>
#include <cstring>
#include <vector>

struct ShaderId {
    size_t size;
    uint32_t hash;
};

// pixel shaders
constexpr ShaderId kLotLightPs = {568, 0xFDAD274Bu};   // lot terrain light pass
constexpr ShaderId kWorldMultiLightPs = {756, 0xEC3141ABu}; // summer multi-pass WORLD terrain, F7 2026-10-02
// Rain variants (F7 capture, raining at night): the shaders above with their c5/c6 moved to c7/c8 and a wet tail,
// output * (1 + c5.x * (c6.x - 1)) (c5.x = wetness ~0.037, c6.x = 0.5: about 2% darker). Same vertex shaders, samplers and c0..c4.
constexpr ShaderId kLotLightRainPs = {608, 0xA8E5F9F8u};
constexpr ShaderId kWorldMultiLightRainPs = {796, 0xCB0A4C22u};
// Structural ids (shader_structure.h): the dry body of each pass, so that any weather variant of it is recognised.
// Checked offline (tools/shader_structure_test) over the 8011 pixel shaders of Shaders_Win32.precomp: each matches
// exactly its dry shader and its rain variant above.
constexpr ShaderStructure::StructId kLotLightStruct = {134, 0x8B5DD2C9u, 0xA9A54253u};
constexpr ShaderStructure::StructId kWorldMultiLightStruct = {181, 0x73A96AF2u, 0x3BA540C0u};
constexpr ShaderId kWorldMultiLightVs = {656, 0x5882F972u}; // s2 lamp UV from c13; chunk matrix c8/c10
constexpr ShaderId kWorldCompactPs = {1296, 0x73376C6Au}; // single diffuse layer WORLD terrain, s3 lamp, c7.x gain; F7 capture
constexpr ShaderId kWorldCompactVs = {744, 0x34E1F1B7u}; // lamp UV c15; chunk matrix c8/c10, same pair in the same capture
constexpr ShaderId kObjectRigPs = {600, 0x0A2D0BE4u};  // instanced outdoor objects (fences, shrubs)
constexpr ShaderId kRoofPs = {1136, 0x6EC87E3Bu};      // roofs
constexpr ShaderId kLakePs = {1344, 0x4F52846Au};      // lake water (sun shadow read with a hardware depth compare, texldp)
constexpr ShaderId kLakePs2 = {1308, 0xB21E05D4u};     // the same lake water with the sun shadow compared by hand (texld + cmp): the
                                                       // game uses it in other weather (2.5.1 report: reflections gone when not sunny)
constexpr ShaderId kSeaNoReflRainPs = {1296, 0xAC8AF7EBu}; // the same sea in rain (ripple map s7, noise s2; the other registers as kSeaNoReflPs; F7 capture)
constexpr ShaderId kLakeRainPs = {2892, 0x91A9AEF7u};   // lake water in rain (Seasons weather: ripple maps s9..s13, camera c3, wave maps s2 / s3 scaled by c9,
                                                       // scene copy s8); F7 capture, with kLakeWeatherVs
constexpr ShaderId kLakeSnowPs = {2756, 0x8C1D384Bu};   // the same lake water while it snows (same registers; F7 capture)
constexpr ShaderId kSeaNoReflPs = {1136, 0xC1F59F1Bu};  // sea water that never reads the planar reflection (s6 = the scene copy, sky cube s3):
                                                       // Twinbrook's sea; the reflecting ocean (Sunset Valley) is another shader
constexpr ShaderId kSnowLotPs = {1852, 0x08DF01E8u};   // snowy lot light pass
constexpr ShaderId kSnowLotCutPs = {1904, 0x998A6795u}; // the same pass on the ground under a rug (footprint texkill on s7, so s8..s12; F7 capture)
constexpr ShaderId kMeltLotPs = {2144, 0x69580706u};   // lot light pass while snow melts (puddles; F7 capture): the snowy
                                                       // pass's lamp term (s2 x basis, later x 0.25) with the dry pass's VS c14/c15 uv
constexpr ShaderId kRoofSnowPs = {4992, 0x3CEB025Eu};  // snowy roofs
// EA 1.69 Light Probe / day-night census, 2026-10-04: cinema/theatre facade.
// The main marquee uses D5ED0EF3. The narrow centre panel uses 4E570819 by day and 36F5E915 at night,
// all paired with the exact BFFCCC56 object VS.
constexpr ShaderId kCinemaMarqueeDayPs = {864, 0xD5ED0EF3u};
constexpr ShaderId kCinemaMarqueeNightPs = {1748, 0xDD77CDE4u}; // main marquee at night; captured 2026-10-04
constexpr ShaderId kCinemaMarqueePanelDayPs = {500, 0x4E570819u};
constexpr ShaderId kCinemaMarqueePanelNightPs = {1296, 0x36F5E915u};
constexpr ShaderId kCinemaMarqueeDayVs = {1060, 0xBFFCCC56u};
// vertex shaders
constexpr ShaderId kLotLightVs = {680, 0x0C8CC5E8u}; // regular lot light pass: contracted UV, F7 capture
constexpr ShaderId kRoofVs = {1192, 0x1F851ECBu};
constexpr ShaderId kLakeVs = {1088, 0x23CCB61Bu};
constexpr ShaderId kSeaNoReflVs = {1136, 0xA784C725u}; // the sea water drawn without the planar reflection (Twinbrook, F7 capture)
constexpr ShaderId kWallVs = {1488, 0x1A921AE5u}; // the game's wall mesh (position = stored / 256 in lot space, world rows c8..c10; F7 capture)
constexpr ShaderId kLakeWeatherVs = {1208, 0x3123FF89u}; // the lake vertex shader of rain and snow (world-view-projection c4..c7, world c8..c10)
constexpr ShaderId kSnowLotVs = {1400, 0x9256F0DFu};
constexpr ShaderId kSnowLotCutVs = {1440, 0xF45FE856u}; // kSnowLotVs + the footprint uv input (v2); its TEXCOORD1 is the same terrain uv (c15/c16)
constexpr ShaderId kFloorVs = {1492, 0x2BC34FA8u};     // snowy floor tiles

inline uint32_t ShaderHash(const void* code, size_t bytes) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i + 4 <= bytes; i += 4) {
        uint32_t d;
        std::memcpy(&d, static_cast<const unsigned char*>(code) + i, 4);
        h = (h ^ d) * 16777619u;
    }
    return h;
}
inline bool IsShader(const ShaderId& id, const void* code, size_t bytes) { return bytes == id.size && ShaderHash(code, bytes) == id.hash; }
