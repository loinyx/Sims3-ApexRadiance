// Chunk light map choice: the production RecordWorldChunk and ChunkTex (lot_light_bridge.cpp) with a fake device and a
// fake rebuilt-map lookup. Run by run_chunk_map_checks.ps1, which inserts the production code below.
#include <windows.h>
#include <d3d9.h>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <format>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <utility>
#include "features/shader_patches.h"

struct FakeTexture {
    int id = 0, refs = 1;
    UINT levels = 4, size = 256;
    ULONG AddRef() { return ULONG(++refs); }
    ULONG Release() { return ULONG(--refs); }
    D3DRESOURCETYPE GetType() { return D3DRTYPE_TEXTURE; }
    DWORD GetLevelCount() { return levels; }
    HRESULT GetLevelDesc(UINT, D3DSURFACE_DESC* d) { *d = {}; d->Width = d->Height = size; d->Format = D3DFMT_DXT5; return S_OK; }
};
struct FakeShader { int id; };
struct FakeDevice {
    float vertex[32][4]{};
    FakeTexture* textures[16]{};
    HRESULT GetVertexShaderConstantF(UINT r, float* v, UINT n) { std::memcpy(v, vertex[r], n * 16); return S_OK; }
};
#define IDirect3DDevice9 FakeDevice
#define IDirect3DPixelShader9 FakeShader
#define IDirect3DTexture9 FakeTexture
#define IDirect3DBaseTexture9 FakeTexture
#define LOG_INFO(x) ((void)(x))

namespace D3D9Hooks {
HRESULT GetBoundTexture(FakeDevice* d, DWORD s, FakeTexture** t) { *t = d->textures[s]; if (*t) (*t)->AddRef(); return S_OK; }
}
namespace ApexUtil { template <class T> void CountOne(T& x) { ++x; } }
// the rebuilt map (+0xD8) of each chunk centre
std::map<std::pair<int, int>, const void*> fixtureRebuilt;
namespace ChunkRelight {
int RebuiltLightMapMatch(int x, int z, const void* t) {
    const auto it = fixtureRebuilt.find({x, z});
    return it == fixtureRebuilt.end() ? -2 : it->second == t ? 0x30 : -1;
}
}
constexpr bool kPublicBuild = true;
enum class PsClass { WorldCandidate };
PsClass g_curClass = PsClass::WorldCandidate;
FakeShader* g_curPs = nullptr;
uint32_t g_roomMapFrame = 1;
std::atomic<int> g_worldSeen{0}, g_chunkStraySkipped{0};
std::unordered_map<FakeShader*, uint16_t> g_worldSamplers;
std::unordered_map<FakeShader*, DWORD> g_worldMapSampler;
struct VsInfo { int worldMap = -1; uint32_t constsRead = ~0u; };
VsInfo* g_curVsInfo = nullptr;
bool Near(float a, float b) { return std::fabs(a - b) < 1e-5f; }
std::pair<int, int> Key(float x, float z) { return {int(std::lround(x)), int(std::lround(z))}; }

/* PRODUCTION_CODE */

int checks = 0, failures = 0;
void Check(bool ok, const char* what) {
    ++checks;
    if (!ok) { ++failures; std::printf("FAIL: %s\n", what); }
}
void Reset() {
    for (auto& [k, v] : g_chunks) if (v.tex) v.tex->Release();
    g_chunks.clear();
    g_chunkOfTexture.clear();
    g_worldMapSampler.clear();
    fixtureRebuilt.clear();
    g_roomMapFrame = 1;
}
// one draw: shader `ps` reading its map at `s`, `t` bound there, world translation (x, z); returns the sampler taken
// one draw: shader `ps` reading its map at `s`, `t` bound there, world translation (x, z), the light map mapping in VS
// constant `mapAt` and, as an earlier draw left it, in `leftover`; `vs` and `mapConst` as the caller gives them
DWORD Draw(FakeShader* ps, DWORD s, FakeTexture* t, float x, float z, ChunkTex** out = nullptr, std::string* why = nullptr, VsInfo* vs = nullptr,
           UINT mapConst = 14, int mapAt = 14, int leftover = -1) {
    FakeDevice d;
    d.vertex[8][3] = x, d.vertex[10][3] = z;
    for (int at : {mapAt, leftover})
        if (at >= 0) d.vertex[at][0] = d.vertex[at][1] = 1.0f / 256.0f, d.vertex[at][2] = d.vertex[at][3] = 0.5f;
    d.textures[s] = t;
    g_curPs = ps;
    g_curVsInfo = vs;
    std::pair<int, int> key;
    ChunkTex* chunk = nullptr;
    const DWORD r = RecordWorldChunk(&d, key, chunk, mapConst, 15, why);
    if (out) *out = chunk;
    return r;
}

int main() {
    FakeShader sidewalk{1}, paint{2};
    g_worldSamplers[&sidewalk] = 1u << 2, g_worldSamplers[&paint] = 1u << 3;
    const std::pair<int, int> K{896, 1152}, K2{1152, 1152};
    FakeTexture A{1}, B{2}, A2{3}, B2{4}, B3{5};
    ChunkTex* c = nullptr;

    // the world file's map seen first, then the rebuilt one: the rebuilt one stays the chunk's map for both variants
    Reset();
    fixtureRebuilt[K] = &B;
    Check(Draw(&sidewalk, 2, &A, 896, 1152, &c) == 2 && c->tex == &A && !c->rebuilt, "first map seen is taken");
    Check(Draw(&paint, 3, &B, 896, 1152, &c) == 3 && c->tex == &B && c->rebuilt, "rebuilt map replaces it");
    Check(Draw(&sidewalk, 2, &A, 896, 1152, &c) == 2 && c->tex == &B, "world file's draw keeps the rebuilt map (same frame)");
    g_roomMapFrame++;
    Check(Draw(&sidewalk, 2, &A, 896, 1152, &c) == 2 && c->tex == &B, "world file's draw keeps the rebuilt map (next frame)");
    Check(!g_chunkOfTexture.count(&A) && g_chunkOfTexture[&B] == K, "only the rebuilt map is registered");
    Check(A.refs == 1 && B.refs == 2, "references: the chunk holds the rebuilt map only");

    // the world file's map registered to another key first (an object drawn with a terrain shader): refused as that key's,
    // the chunk's own rebuilt map drawn at the shader's sampler
    Reset();
    fixtureRebuilt[K] = &B;
    Draw(&sidewalk, 2, &A, 900, 1140);
    Draw(&paint, 3, &B, 896, 1152);
    std::string why;
    Check(Draw(&sidewalk, 2, &A, 896, 1152, &c, &why) == 2 && c->tex == &B && why.empty(), "a map registered elsewhere: own rebuilt map drawn");
    Check(g_chunkOfTexture[&A] == std::make_pair(900, 1140) && A.refs == 2, "the other key keeps its map");

    // nothing confirmed (before the world's first rebuild): a map registered to another key is refused, never moved, so two
    // keys cannot take it from each other draw by draw
    Reset();
    Draw(&sidewalk, 2, &A, 900, 1140);
    for (int i = 0; i < 3; i++) Check(Draw(&sidewalk, 2, &A, 896, 1152, &c) == 0 && Draw(&sidewalk, 2, &A, 900, 1140) == 2, "no take-over back and forth");
    Check(g_chunkOfTexture[&A] == std::make_pair(900, 1140) && A.refs == 2 && !g_chunks.count(K), "registration and references unchanged");

    // another chunk's rebuilt map left in the sampler: refused, the chunk's own rebuilt map is drawn at the shader's sampler
    Reset();
    fixtureRebuilt[K] = &B, fixtureRebuilt[K2] = &B2;
    Draw(&paint, 3, &B2, 1152, 1152);
    Draw(&sidewalk, 2, &A2, 1152, 1152);
    Draw(&paint, 3, &B, 896, 1152);
    const int strays = g_chunkStraySkipped.load();
    Check(Draw(&sidewalk, 2, &B2, 896, 1152, &c) == 2 && c->tex == &B, "leftover rebuilt map: own rebuilt map at the learned sampler");
    Check(g_chunkStraySkipped.load() == strays + 1, "leftover counted");
    // ...and left to the game, with the reason, while the chunk's rebuilt map is unknown
    Reset();
    fixtureRebuilt[K2] = &B2;
    Draw(&paint, 3, &B2, 1152, 1152);
    why.clear();
    Check(Draw(&sidewalk, 2, &B2, 896, 1152, &c, &why) == 0 && why.find("s2 holds the map of chunk (1152, 1152)") != std::string::npos,
          "leftover with no known own map: refused with its reason");

    // the game makes a new rebuilt texture: taken at the first draw of it in a later frame
    Reset();
    fixtureRebuilt[K] = &B;
    Draw(&paint, 3, &B, 896, 1152);
    fixtureRebuilt[K] = &B3;
    g_roomMapFrame++;
    Check(Draw(&paint, 3, &B3, 896, 1152, &c) == 3 && c->tex == &B3 && c->rebuilt && B.refs == 1, "new rebuilt texture taken");
    // ...also when a world file's draw of the chunk checked first in that frame
    Reset();
    fixtureRebuilt[K] = &B;
    Draw(&paint, 3, &B, 896, 1152);
    g_roomMapFrame++;
    Draw(&sidewalk, 2, &A, 896, 1152);
    fixtureRebuilt[K] = &B3;
    Draw(&sidewalk, 2, &A, 896, 1152);
    Check(Draw(&paint, 3, &B3, 896, 1152, &c) == 3 && c->tex == &B3 && c->rebuilt, "new rebuilt texture taken after another check that frame");
    // ...and a moment the chunk cannot be read does not drop the confirmed rebuilt map
    Reset();
    fixtureRebuilt[K] = &B;
    Draw(&paint, 3, &B, 896, 1152);
    g_roomMapFrame++;
    fixtureRebuilt.erase(K);
    Check(Draw(&sidewalk, 2, &A, 896, 1152, &c) == 2 && c->tex == &B && c->rebuilt, "unreadable for a frame: the rebuilt map kept");

    // no rebuilt map readable (before the world's first rebuild): the last map seen, as before
    Reset();
    Draw(&sidewalk, 2, &A, 896, 1152);
    Check(Draw(&paint, 3, &B, 896, 1152, &c) == 3 && c->tex == &B && !c->rebuilt, "unreadable: last map seen");

    // the light map mapping must be in a constant the vertex shader reads (no known mapping constant: checked from c15)
    Reset();
    VsInfo sidewalkVs{-1, 1u << 8 | 1u << 10 | 1u << 14 | 1u << 16 | 1u << 17};
    Check(Draw(&sidewalk, 2, &A, 896, 1152, &c, nullptr, &sidewalkVs, 15, 14, -1) == 2, "mapping read at c14, nothing at c15: taken");
    Check(Draw(&sidewalk, 2, &A, 896, 1152, &c, nullptr, &sidewalkVs, 15, 14, 15) == 2, "mapping at c14 and a leftover at c15: taken");
    why.clear();
    Check(Draw(&sidewalk, 2, &A, 903, 1140, &c, &why, &sidewalkVs, 15, 14, -1) == 0 && why.find("not at a chunk's centre") != std::string::npos,
          "mapping found by the scan off a chunk centre (an object): refused");
    Check(!g_chunks.count({903, 1140}), "no chunk entry for it");
    VsInfo unreadVs{-1, 1u << 8 | 1u << 10};
    why.clear();
    Check(Draw(&sidewalk, 2, &A, 896, 1152, &c, &why, &unreadVs, 15, -1, 15) == 0 && !why.empty(), "only a leftover at c15 it does not read: refused");
    VsInfo knownVs{13, 1u << 13};
    Check(Draw(&sidewalk, 2, &A, 896, 1152, &c, nullptr, &knownVs, 13, -1, 15) == 0, "known mapping constant unset: no scan of the others");
    Check(Draw(&sidewalk, 2, &A, 896, 1152, &c, nullptr, &knownVs, 13, 13, -1) == 2, "known mapping constant set: taken");

    // ShaderPatches::ConstantsRead: mad r0.xy, r1, c14, c14.zwzw; mov r2, c20 (def'd); mov r3, c[a0.x + 30]
    const std::vector<DWORD> vs3{0xFFFE0300u, 0x05000051u, 0xA00F0014u, 0, 0, 0, 0, 0x04000004u, 0x80030000u, 0x80E40001u, 0xA0E4000Eu, 0xA0EE000Eu,
                                 0x02000001u, 0x800F0002u, 0xA0E40014u, 0x03000001u, 0x800F0003u, 0xA0E4201Eu, 0xB0000000u, 0x0000FFFFu};
    Check(ShaderPatches::ConstantsRead(vs3) == 1u << 14, "constants read: c14 only (def'd and relative left out)");
    Check(ShaderPatches::ConstantsRead({}) == ~0u, "constants read: unparsable code gives every bit");
    // m4x4 r0, v0, c12 reads c12..c15; vs_1_1 code is not parsed
    const std::vector<DWORD> matrix{0xFFFE0300u, 0x03000014u, 0x800F0000u, 0x90E40000u, 0xA0E4000Cu, 0x0000FFFFu};
    Check(ShaderPatches::ConstantsRead(matrix) == 0xFu << 12, "constants read: the four rows of an m4x4");
    // (p0) m4x4 r0, v0, c12: the predicate token sits before the sources
    const std::vector<DWORD> predicated{0xFFFE0300u, 0x14000014u, 0x800F0000u, 0xB0E41000u, 0x90E40000u, 0xA0E4000Cu, 0x0000FFFFu};
    Check(ShaderPatches::ConstantsRead(predicated) == 0xFu << 12, "constants read: the rows of a predicated m4x4");
    Check(ShaderPatches::ConstantsRead({0xFFFE0101u, 0x00000001u, 0x0000FFFFu}) == ~0u, "constants read: vs_1_1 gives every bit");

    Reset();
    std::printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
