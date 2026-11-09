// Light detail (part of Night Lighting): an option to raise the resolution of the baked lamp light, which looks low
// resolution in the game.
//
// The game bakes lamp light into texture maps per story (room solve: walls into an atlas of strips, floors, ceilings and
// objects into maps over the lot), with a detail set by each room's lighting LOD class (room+0xF4, 0..2) from fixed tables
// in TS3W.exe (Steam 1.67.2, research\engine_map\full.asm and dwords.txt):
//  - 0x00FF36AC int[3] = {1, 2, 4}: texels per tile edge. Read by every per-class size: FUN_006a8d20 (the value),
//    FUN_006a8d30 (+1), FUN_006a8d40 (3n+1: the wall rows over 3 m, 4 / 7 / 13), FUN_006a8d50 (+2), FUN_006a8d60 /
//    FUN_006a8de0 (floor, ceiling and object map sizes: n x lot tiles, doubled in width where 0x00FF36A0[class] separates
//    diagonals, rounded up to a power of two by FUN_006a8c40, at most 2048), the wall packer FUN_006a90e0 (rows) and
//    CacheLightingParams FUN_006a4480 (the size the shaders' UV constants come from).
//  - 0x00FF36DC int[3] = {256, 512, 1024} and 0x00FF36E8 int[3] = {128, 256, 512}: the wall atlas per class, made by
//    FUN_006a90e0 -> FUN_006ac4f0(width, height, rows).
// High doubles both, class by class ({2, 4, 8} texels, walls {512, 1024, 2048} x {256, 512, 1024}): every size above
// keeps its proportions (a strip twice as long and twice as tall in an atlas twice as wide and twice as tall), the floor
// maps of the largest lot stay within the 2048 the game rounds to (2 x 8 x 64 = 1024 at class 2), and the lots the game
// lights at a low class keep small maps. Sims3SettingsSetter's Lighting Quality writes the same tables (every class at
// the top value); when another mod has changed them, nothing is written here.
// The tables are read when a lot's lighting is built: they are written once, before any lot is lit (at the main menu),
// and kept for the whole session (a lot lit with one detail must keep it). A change takes effect after a restart.
// Cost: four times the texels per room, so rooms take about four times longer to light (after loading and after a lamp
// changes) and their maps take four times the memory.
//
// The class-2 floor and ceiling layout (indoor lamp light in 1 m squares, a sconce's red through its wall in
// the next room, the floor strip along a sconce's wall black after a lamp edit; F7 light map T5): two places
// build it with the game's 4 written in the code instead of 0x00FF36AC[2], so High doubles them too:
//  - 0x006A7E62 "push 4" in FUN_006a7cf0 (the story manager's tile rebuild, the only caller of FUN_006aa480): every
//    quadrant no diagonal wall splits gets the texel origin (tile x * 4, tile z * 4) (0x006AA568, u16 at tile+0x8A /
//    +0x8C + q*0x14). Each class-2 floor and ceiling batch writes n x n texels from it (FUN_006aa9d0 -> FUN_006aabe0,
//    n = the table) and the floor mesh reads origin + n x (0..1) across the tile (FUN_006a6d10 -> FUN_006a9970). With
//    n = 8 every tile wrote over the first half of its +x and +z neighbours' texels (T5: 8-texel runs 4 texels apart,
//    row 108 texels 40..47 one ramp of room 0's tile 10) and showed its own first half metre twice: 1 m steps inside a
//    room, and at a wall the room solved last painted the first half metre behind it (a sconce's red in the next room;
//    room 0's wall-tested black along the house's wall when room 0 was solved after the room). Every other reader already
//    places the tiles n apart: the border fill FUN_006a9de0 (n x tile), the tile-less blocks of FUN_006a37f0, the
//    whole-tile blocks of FUN_006aa960, the lot-wide UV constants (FUN_006a4c10, FUN_006a4e10) and the atlas of split
//    tiles, which starts at W / 2 = pow2(n x lot tiles) (FUN_006a8f30), past the n x tiles of the lot.
//  - 0x006AA325 "push 4" in FUN_006aa2e0 (the tile constructor): FUN_006a9ad0 builds once (byte 0x011D0510) the offsets
//    (n, n - 1, n - 2) the border fill copies with into the 1-texel border of those atlas pieces.
// High writes 8 into both with the tables (the copy table is built again if a tile was already made). The first room
// solve checks every tile of its story (CheckLayout, "[LightDetail] layout check").
#include "light_detail.h"
#include "apex_log.h"
#include "game_addresses.h"
#include "level_light_share.h"
#include "memory_patch.h"
#include <windows.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <format>
#include <vector>

namespace {

// Game addresses (Steam 1.67.2 values in the comments), from GameAddr when ApplyAtStartup runs (every build)
uintptr_t kTexelsPerTile = 0; // 0x00FF36AC int[3] (TexelsPerTile)
uintptr_t kWallAtlasW = 0;    // 0x00FF36DC int[3] (WallAtlasW)
uintptr_t kWallAtlasH = 0;    // 0x00FF36E8 int[3] (WallAtlasH)
uintptr_t kTexelsGetter = 0;  // 0x006A8D20 FUN_006a8d20: mov eax,[ecx+8]; mov eax,[eax*4+0x00FF36AC]; ret (TexelsGetter)
uintptr_t kAtlasReads = 0;    // 0x006A90FC in FUN_006a90e0: mov edi,[eax+eax+0x00FF36DC]; mov ebx,[eax+eax+0x00FF36E8] (AtlasReads)
uintptr_t kOriginCall = 0;    // 0x006A7E5E in FUN_006a7cf0: mov ecx,[esp+14h]; push 4; push ecx; lea edx,[esi+200h]; push edx; mov ecx,edi; call FUN_006aa480 (OriginPush)
uintptr_t kOriginImm = 0;     // 0x006A7E63 the 4 of that push (6A 04)
uintptr_t kCopyCall = 0;      // 0x006AA322 in FUN_006aa2e0: mov [ecx+74h],eax; push 4; movss [ecx+78h],xmm0; mov [ecx+0CCh],al; call FUN_006a9ad0; ret 8 (CopyPush)
uintptr_t kCopyImm = 0;       // 0x006AA326 the 4 of that push (6A 04)
uintptr_t kCopyBuilt = 0;     // 0x011D0510 byte (.data): FUN_006a9ad0 built its table (it builds it once) (CopyBuilt)
uintptr_t kCopyBuild = 0;     // 0x006A9AD0 FUN_006a9ad0(int n): stdcall (ret 4), ecx unused (CopyBuild)
using CopyBuild_t = void(__stdcall*)(int n);
constexpr DWORD kGameTexels[3] = {1, 2, 4}, kGameAtlasW[3] = {256, 512, 1024}, kGameAtlasH[3] = {128, 256, 512};
constexpr BYTE kGameStride = 4;

int g_active = 0;
bool g_tried = false;
std::string g_status = "the game's own detail";
std::atomic<bool> g_layoutChecked{false};

bool ReadTable(uintptr_t at, DWORD* out) { return MemPatch::ReadBytes(at, out, 3 * sizeof(DWORD)); }
bool CodeIs(uintptr_t at, const BYTE* expected, size_t count) {
    BYTE seen[32] = {};
    return count <= sizeof seen && MemPatch::ReadBytes(at, seen, count) && std::memcmp(seen, expected, count) == 0;
}
// One immediate byte, written only over `was`
bool WriteImm(uintptr_t at, BYTE value, BYTE was) {
    const std::vector<BYTE> bytes{value}, expected{was};
    return MemPatch::WriteBytes(at, bytes, nullptr, &expected);
}
// FUN_006a9ad0 builds the border fill's copy table once, with the n the first tile pushes: when a tile was already made,
// it is built again for n (nothing is lit yet, so no border fill reads it meanwhile). POD only (SEH)
bool BuildCopyTable(int n, bool& rebuilt) {
    rebuilt = false;
    __try {
        volatile BYTE* built = reinterpret_cast<volatile BYTE*>(kCopyBuilt);
        if (!*built) return true; // the first tile builds it with the immediate
        *built = 0;
        reinterpret_cast<CopyBuild_t>(kCopyBuild)(n);
        rebuilt = true;
        return *built != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

struct LayoutCount {
    int story = -99, w = 0, h = 0, lot = 0, atlas = 0, unset = 0, other = 0, x = -1, z = -1, u = 0, v = 0;
};
// The class-2 texel origins of every tile of a room's story (u16 at tile+0x8A / +0x8C + q*0x14, FUN_006aa480): n x the
// tile, an atlas piece (past the lot's n x w texels: FUN_006a8f30 starts the atlas at W / 2 >= n x w), not set yet (0, 0),
// or anything else: over a neighbour's texels. POD only (SEH)
bool CountOrigins(const void* room, int n, LayoutCount& c) {
    __try {
        const uintptr_t mgr = *static_cast<const uintptr_t*>(room);
        if (!mgr) return false;
        c.story = *reinterpret_cast<const int*>(mgr + 0x88);
        c.w = *reinterpret_cast<const int*>(mgr + 0x264);
        c.h = *reinterpret_cast<const int*>(mgr + 0x268);
        const uintptr_t* grid = *reinterpret_cast<const uintptr_t* const*>(mgr + 0x260);
        if (!grid || c.w <= 0 || c.h <= 0 || c.w > 1024 || c.h > 1024) return false;
        for (int z = 0; z < c.h; z++)
            for (int x = 0; x < c.w; x++) {
                const uintptr_t tile = grid[static_cast<size_t>(z) * c.w + x];
                if (!tile) continue;
                for (int q = 0; q < 4; q++) {
                    const int u = *reinterpret_cast<const uint16_t*>(tile + 0x8A + q * 0x14);
                    const int v = *reinterpret_cast<const uint16_t*>(tile + 0x8C + q * 0x14);
                    if (u == x * n && v == z * n) c.lot++;
                    else if (u > c.w * n) c.atlas++;
                    else if (u == 0 && v == 0) c.unset++;
                    else if (c.other++ == 0) {
                        c.x = x;
                        c.z = z;
                        c.u = u;
                        c.v = v;
                    }
                }
            }
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

} // namespace

namespace LightDetail {

bool ApplyAtStartup(int level) {
    if (g_tried) return g_active == level;
    g_tried = true;
    if (level <= 0) return true;
    // The table addresses come from the code that reads them (GameAddr); that code and the two places that write the class-2
    // floor layout's 4 in the code are checked byte by byte (on Steam 1.67.2 with the rel32s it always had; on other builds
    // with the addresses found, the two CALLs only checked to land on their function)
    using GameAddr::Id;
    std::string missing;
    if (!GameAddr::Have({Id::TexelsGetter, Id::TexelsPerTile, Id::AtlasReads, Id::WallAtlasW, Id::WallAtlasH, Id::OriginPush, Id::CopyPush, Id::CopyBuild, Id::CopyBuilt}, &missing)) {
        g_status = GameAddr::NotAvailable(missing);
        LOG_WARNING("[LightDetail] " + g_status);
        return false;
    }
    kTexelsPerTile = GameAddr::Get(Id::TexelsPerTile);
    kWallAtlasW = GameAddr::Get(Id::WallAtlasW);
    kWallAtlasH = GameAddr::Get(Id::WallAtlasH);
    kTexelsGetter = GameAddr::Get(Id::TexelsGetter);
    kAtlasReads = GameAddr::Get(Id::AtlasReads);
    kOriginCall = GameAddr::Get(Id::OriginPush);
    kOriginImm = kOriginCall + 5;
    kCopyCall = GameAddr::Get(Id::CopyPush);
    kCopyImm = kCopyCall + 4;
    kCopyBuilt = GameAddr::Get(Id::CopyBuilt);
    kCopyBuild = GameAddr::Get(Id::CopyBuild);
    BYTE getter[] = {0x8B, 0x41, 0x08, 0x8B, 0x04, 0x85, 0xAC, 0x36, 0xFF, 0x00, 0xC3};
    BYTE atlas[] = {0x8B, 0xBC, 0x00, 0xDC, 0x36, 0xFF, 0x00, 0x8B, 0x9C, 0x00, 0xE8, 0x36, 0xFF, 0x00};
    BYTE originCall[] = {0x8B, 0x4C, 0x24, 0x14, 0x6A, 0x04, 0x51, 0x8D, 0x96, 0x00, 0x02, 0x00, 0x00, 0x52, 0x8B, 0xCF, 0xE8, 0x0D, 0x26, 0x00, 0x00};
    BYTE copyCall[] = {0x89, 0x41, 0x74, 0x6A, 0x04, 0xF3, 0x0F, 0x11, 0x41, 0x78, 0x88, 0x81, 0xCC, 0x00, 0x00, 0x00, 0xE8, 0x99, 0xF7, 0xFF, 0xFF, 0xC2, 0x08, 0x00};
    std::memcpy(getter + 6, &kTexelsPerTile, 4); // (on Steam these are the bytes above)
    std::memcpy(atlas + 3, &kWallAtlasW, 4);
    std::memcpy(atlas + 10, &kWallAtlasH, 4);
    bool same = CodeIs(kTexelsGetter, getter, sizeof getter) && CodeIs(kAtlasReads, atlas, sizeof atlas);
    if (GameAddr::IsFixed()) {
        same = same && CodeIs(kOriginCall, originCall, sizeof originCall) && CodeIs(kCopyCall, copyCall, sizeof copyCall);
    } else {
        const int32_t copyRel = static_cast<int32_t>(kCopyBuild - (kCopyCall + 21));
        std::memcpy(copyCall + 17, &copyRel, 4);
        same = same && CodeIs(kOriginCall, originCall, 17) && CodeIs(kCopyCall, copyCall, sizeof copyCall); // the origin call's target is not studied
    }
    if (!same) {
        g_status = "not available on this game version (the game code differs)";
        LOG_WARNING("[LightDetail] " + g_status);
        return false;
    }
    DWORD texels[3] = {}, w[3] = {}, h[3] = {};
    if (!ReadTable(kTexelsPerTile, texels) || !ReadTable(kWallAtlasW, w) || !ReadTable(kWallAtlasH, h)) {
        g_status = "the game's tables could not be read";
        LOG_WARNING("[LightDetail] " + g_status);
        return false;
    }
    if (std::memcmp(texels, kGameTexels, sizeof texels) != 0 || std::memcmp(w, kGameAtlasW, sizeof w) != 0 || std::memcmp(h, kGameAtlasH, sizeof h) != 0) {
        g_status = std::format("left as it is: another mod already changed it (texels per tile {{{}, {}, {}}}, wall maps {}x{}; Sims3SettingsSetter's Lighting Quality?)",
                               texels[0], texels[1], texels[2], w[2], h[2]);
        LOG_WARNING("[LightDetail] " + g_status);
        return false;
    }
    if (LevelLightShare::LoadedLots() > 0) { // a lot already lit keeps the detail it was built with
        g_status = "takes effect after restarting the game (a lot was already lit)";
        LOG_INFO("[LightDetail] " + g_status);
        return false;
    }
    const BYTE stride = static_cast<BYTE>(kGameTexels[2] * 2); // the class-2 texels per tile once doubled: 8
    bool ok = true;
    for (int c = 0; c < 3 && ok; c++) {
        // kept for the whole session: a lot lit with this detail must never meet the game's tables again
        ok = MemPatch::WriteDWORD(kTexelsPerTile + c * 4, kGameTexels[c] * 2, nullptr, &kGameTexels[c]) &&
             MemPatch::WriteDWORD(kWallAtlasW + c * 4, kGameAtlasW[c] * 2, nullptr, &kGameAtlasW[c]) &&
             MemPatch::WriteDWORD(kWallAtlasH + c * 4, kGameAtlasH[c] * 2, nullptr, &kGameAtlasH[c]);
    }
    // the class-2 floor and ceiling texels of a tile n apart, as every other reader places them
    ok = ok && WriteImm(kOriginImm, stride, kGameStride) && WriteImm(kCopyImm, stride, kGameStride);
    bool rebuilt = false;
    ok = ok && BuildCopyTable(stride, rebuilt);
    if (!ok) { // put back what was written (nothing is lit yet)
        for (int c = 0; c < 3; c++) {
            MemPatch::WriteDWORD(kTexelsPerTile + c * 4, kGameTexels[c]);
            MemPatch::WriteDWORD(kWallAtlasW + c * 4, kGameAtlasW[c]);
            MemPatch::WriteDWORD(kWallAtlasH + c * 4, kGameAtlasH[c]);
        }
        WriteImm(kOriginImm, kGameStride, stride); // only over a byte that holds the new value
        WriteImm(kCopyImm, kGameStride, stride);
        bool again = false;
        BuildCopyTable(kGameStride, again);
        g_status = "the game's tables could not be written";
        LOG_WARNING("[LightDetail] " + g_status);
        return false;
    }
    g_active = level;
    g_status = "high: twice the lighting texels per metre on walls and floors (texels per tile {2, 4, 8}, wall maps up to 2048x1024)";
    LOG_INFO("[LightDetail] " + g_status);
    LOG_INFO(std::format("[LightDetail] class-2 floor and ceiling texels {} per tile, {} apart (0x{:08X} push {}; atlas border copies 0x{:08X} push {}{})",
                         static_cast<int>(stride), static_cast<int>(stride), kOriginImm - 1, static_cast<int>(stride), kCopyImm - 1,
                         static_cast<int>(stride), rebuilt ? ", copy table built again" : ""));
    return true;
}

void CheckLayout(const void* room) {
    // (issue #55) no table address (Light detail off, or not found): reading it read address 8, a handled fault every solve
    if (!room || !kTexelsPerTile || g_layoutChecked.load(std::memory_order_relaxed)) return;
    DWORD n = 0;
    if (!MemPatch::ReadBytes(kTexelsPerTile + 2 * sizeof(DWORD), &n, sizeof n) || n == 0 || n > 64) return;
    LayoutCount c;
    if (!CountOrigins(room, static_cast<int>(n), c) || c.lot + c.other == 0) return; // a story not rebuilt yet: a later solve
    if (g_layoutChecked.exchange(true)) return;
    if (!c.other)
        LOG_INFO(std::format("[LightDetail] layout check (story {}, {}x{} tiles): {} quadrants at {} texels x the tile, {} in the atlas, none over a neighbour: OK",
                             c.story, c.w, c.h, c.lot, n, c.atlas));
    else
        LOG_WARNING(std::format("[LightDetail] layout check (story {}, {}x{} tiles): {} quadrants NOT at {} texels x the tile (first: tile ({}, {}) at texel ({}, {})): "
                                "class-2 floor and ceiling texels overlap their neighbours (1 m steps, light through walls)",
                                c.story, c.w, c.h, c.other, n, c.x, c.z, c.u, c.v));
}

int Active() { return g_active; }

std::string Status() { return g_status; }

} // namespace LightDetail
