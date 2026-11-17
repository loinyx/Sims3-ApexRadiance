// Real-time GI S2-05 offline check: the lighting reading the irradiance atlas (features/gi_indirect.h, ShaderPatches::PatchRoomMapGi,
// GiCompose::Scatter). Read-only: prints to stdout, writes no file.
//   check.exe [Shaders_Win32.precomp]
// 1. Grid (CPU): the builder's scene of a synthetic lot, its atlas filled with a value per pixel: at every floor and ceiling texel's
//    place (from its world position, through a rotated and moved lot too) the grid's bilinear read gives that texel's value; half
//    way between two texels their mean; layers do not bleed into each other.
// 2. Native D3D9: Scatter fills the grid from an atlas texture as FillCpu does; the patched copy of a synthetic room-lit ps_3_0
//    shader in the game's pattern, fed TEXCOORD4 as the floor vertex shader packs it, gives GiIndirect::Apply of SampleCpu (mode 1
//    by night and by day, mode 2) within 2 LSB; at texel centres the texel's own value.
// 3. Precomp (optional): every room-lit pixel shader (ShaderPatches::IsRoomMapPs) and its Smooth light maps copies (plain, arches,
//    walls): which take PatchRoomMapGi, slot and temp counts, native D3D9 creates every patched copy; every refusal leaves the bytes
//    as they were (indirect off draws the same bytes as before).
#define main RoomMapSuiteMain
#include "../room_map_smooth_check/check.cpp"
#undef main
#include "../../features/gi_compose.h"
#include "../../features/gi_indirect.h"

namespace {

// one roofed room per story (8 x 8 lot), story 1 over story 0
RtLampProxy::Lot TwoStories() {
    RtLampProxy::Lot lot;
    for (int s = 0; s < 2; s++) {
        RtLampProxy::Story st;
        st.story = s, st.w = st.h = 8, st.plane = 3.0f * s;
        st.floorY.assign(64, 3.0f * s), st.roofed.assign(64, 0), st.ceil.assign(64, RtLampProxy::kNoFloor);
        const int x0 = 1 + s, z0 = 1, x1 = 5 + s, z1 = 4;
        for (int z = z0; z < z1; z++)
            for (int x = x0; x < x1; x++) st.roofed[static_cast<size_t>(z) * 8 + x] = 0xF, st.ceil[static_cast<size_t>(z) * 8 + x] = 3.0f * s + 3.0f;
        const auto wall = [&](float ax, float az, float bx, float bz) {
            RtLampProxy::Edge e;
            e.x0 = ax, e.z0 = az, e.x1 = bx, e.z1 = bz, e.base = 3.0f * s, e.topL = e.topM = e.topR = 3.0f * s + 3.0f;
            st.edges.push_back(e);
        };
        for (int x = x0; x < x1; x++) wall(float(x), float(z0), float(x + 1), float(z0)), wall(float(x), float(z1), float(x + 1), float(z1));
        for (int z = z0; z < z1; z++) wall(float(x0), float(z), float(x0), float(z + 1)), wall(float(x1), float(z), float(x1), float(z + 1));
        lot.stories.push_back(st);
    }
    return lot;
}
float AtlasValue(uint32_t px, int c) { // a distinct value a pixel, 0.05 .. 0.95
    const uint32_t m[3] = {37, 53, 11}, q[3] = {101, 97, 13};
    return 0.05f + 0.9f * static_cast<float>((px * m[c] + 7) % q[c]) / static_cast<float>(q[c] - 1);
}
std::vector<float> FakeAtlas(const GiScene::Built& b) {
    std::vector<float> a(static_cast<size_t>(GiScene::kAtlasWidth) * b.height * 4, 0.0f);
    for (uint32_t px = 0; px < GiScene::kAtlasWidth * b.height; px++)
        for (int c = 0; c < 3; c++) a[static_cast<size_t>(px) * 4 + c] = AtlasValue(px, c);
    return a;
}

struct Scene {
    RtLampProxy::Lot lot;
    GiScene::Built built;
    GiIndirect::Grid grid;
    std::vector<float> atlas, ready, cpuGrid;
};
Scene MakeScene(bool rotated, uint64_t notReadyTile = ~0ull) {
    Scene s;
    s.lot = TwoStories();
    if (rotated) {
        const float rot[4][4] = {{0, 0, -1, 0}, {0, 1, 0, 0}, {1, 0, 0, 0}, {-30, -2, 50, 1}}; // lot = world x rows
        std::memcpy(s.lot.worldToLot, rot, sizeof rot);
    }
    GiScene::Input in;
    in.lot = &s.lot;
    GiScene::Options o;
    o.density = 2;
    GiScene::Build(in, o, nullptr, s.built);
    Check(GiIndirect::Build(s.built, o.density, 8, 8, s.grid), "the grid of the synthetic lot builds");
    s.atlas = FakeAtlas(s.built);
    for (const GiIndirect::Point& p : s.grid.points) s.ready.push_back(p.tile == notReadyTile ? 0.0f : 1.0f);
    GiIndirect::FillCpu(s.grid, s.atlas, s.ready, s.cpuGrid);
    return s;
}

// 1. CPU
void GridChecks() {
    for (int rotated = 0; rotated < 2; rotated++) {
        Scene s = MakeScene(rotated != 0);
        size_t floors = 0, ceilings = 0, exact = 0, total = 0;
        float worst = 0.0f;
        for (const auto& [key, slot] : s.built.slots) {
            float p[3];
            int code;
            GiIndirect::DecodeKey(key, p, code);
            if (code != 8 && code != 9) continue;
            (code == 8 ? floors : ceilings)++;
            float lp[3];
            RtLampProxy::ToLot(s.lot, slot.world, lp); // (the texel's world position back to the lot, as the floor VS packs it)
            float g[4];
            if (!GiIndirect::SampleCpu(s.grid, s.cpuGrid, slot.story, code == 8 ? GiIndirect::kFloor : GiIndirect::kCeiling, lp[0], lp[2], g)) continue;
            total++;
            float d = std::fabs(g[3] - 1.0f);
            for (int c = 0; c < 3; c++) d = std::max(d, std::fabs(g[c] - AtlasValue(slot.pixel, c)));
            worst = std::max(worst, d);
            exact += d < 1e-4f;
        }
        std::printf("  grid (%s lot): %zu floor and %zu ceiling texels, %zu read back at their world place, worst %.6f, layers %zu, %u x %u\n", rotated ? "rotated" : "plain",
                    floors, ceilings, exact, worst, s.grid.rows.size(), s.grid.w, s.grid.h);
        Check(floors > 0 && ceilings > 0 && exact == floors + ceilings && total == exact, "every floor and ceiling texel's value at its world place");
        Check(s.grid.rows.size() == 4, "two stories x floor and ceiling layers");
    }
    // half way between two neighbouring floor texels of story 0 (x = 2.25 and 2.75 m at z = 1.25): their mean
    Scene s = MakeScene(false);
    const auto at = [&](float x, float z) {
        const float p[3] = {x, GiScene::kOff, z}, n[3] = {0, 1, 0};
        return s.built.slots.at(GiScene::TexelKey(p, n)).pixel;
    };
    float g[4];
    GiIndirect::SampleCpu(s.grid, s.cpuGrid, 0, GiIndirect::kFloor, 2.5f, 1.25f, g);
    const uint32_t a = at(2.25f, 1.25f), b = at(2.75f, 1.25f);
    Check(std::fabs(g[0] - 0.5f * (AtlasValue(a, 0) + AtlasValue(b, 0))) < 1e-5f && std::fabs(g[3] - 1.0f) < 1e-6f, "between two texels: their mean");
    // a layer's last row and the next layer's first stay apart (the empty row): at the room's far edge the read falls to 0 ready
    for (const float z : {4.75f, 7.75f, 7.99f}) {
        GiIndirect::SampleCpu(s.grid, s.cpuGrid, 0, GiIndirect::kFloor, 2.25f, z, g);
        Check(g[3] < 1e-6f && g[0] < 1e-6f, "on the lot outside the rooms, to its edge: nothing (no other layer's texels)");
    }
    // a tile waiting for a lamp: ready 0 there (the copy keeps the game's ambient), 1 next to it
    const uint64_t tile = GiSched::TileKey(0, 3, 2);
    Scene w = MakeScene(false, tile);
    GiIndirect::SampleCpu(w.grid, w.cpuGrid, 0, GiIndirect::kFloor, 3.25f, 2.25f, g);
    float h[4];
    GiIndirect::SampleCpu(w.grid, w.cpuGrid, 0, GiIndirect::kFloor, 1.25f, 2.25f, h);
    Check(g[3] < 1e-6f && h[3] > 0.999f, "a waiting tile reads ready 0, another 1");
    float c[4];
    Check(!GiIndirect::UvRow(s.grid, 5, GiIndirect::kFloor, c), "no layer for a story the scene has not");
    // the map lookup's set indices: the game's texture set order (RtOutdoorLamps::kSetIdx = wall 3, floor 0, ceiling 1, objects 2)
    Check(GiIndirect::KindOfSetIndex(0) == GiIndirect::kFloor && GiIndirect::KindOfSetIndex(1) == GiIndirect::kCeiling &&
              GiIndirect::KindOfSetIndex(2) < 0 && GiIndirect::KindOfSetIndex(3) < 0,
          "texture set index 0 = floor, 1 = ceiling, objects and wall no layer");
    // Apply: mode 1 by night replaces the ready share of the ambient, by day adds; mode 2 the atlas alone
    const float map[4] = {0.2f, 0.3f, 0.4f, 0.5f}, gi[4] = {0.6f, 0.5f, 0.4f, 1.0f}, amb[3] = {0.3f, 0.2f, 0.1f};
    float o[3];
    const float night[4] = {1, 1, 1, 1}, day[4] = {1, 1, 1, 0}, debug[4] = {1, 0, 0, 0};
    GiIndirect::Apply(map, gi, night, amb, o);
    Check(std::fabs(o[0] - 0.8f) < 1e-6f, "night: lamp light + atlas, no game ambient");
    GiIndirect::Apply(map, gi, day, amb, o);
    Check(std::fabs(o[0] - (0.2f + 0.6f + 0.5f * 0.3f)) < 1e-6f, "day: lamp light + atlas + the game's ambient");
    GiIndirect::Apply(map, gi, debug, amb, o);
    Check(std::fabs(o[0] - 0.6f) < 1e-6f && std::fabs(o[2] - 0.4f) < 1e-6f, "debug: the atlas alone");
}

// 2. native D3D9
IDirect3DTexture9* FloatTexture(IDirect3DDevice9* dev, uint32_t w, uint32_t h, const std::vector<float>& rgba) {
    IDirect3DTexture9* t = nullptr;
    if (FAILED(dev->CreateTexture(w, h, 1, 0, D3DFMT_A32B32G32R32F, D3DPOOL_MANAGED, &t, nullptr))) return nullptr;
    D3DLOCKED_RECT r{};
    if (SUCCEEDED(t->LockRect(0, &r, nullptr, 0))) {
        for (uint32_t y = 0; y < h; y++) std::memcpy(static_cast<char*>(r.pBits) + y * r.Pitch, &rgba[static_cast<size_t>(y) * w * 4], w * 16);
        t->UnlockRect(0);
    }
    return t;
}
bool ReadFloat16(IDirect3DDevice9* dev, IDirect3DTexture9* rt, uint32_t w, uint32_t h, std::vector<float>& out) {
    IDirect3DSurface9 *s = nullptr, *sys = nullptr;
    bool ok = SUCCEEDED(rt->GetSurfaceLevel(0, &s)) && SUCCEEDED(dev->CreateOffscreenPlainSurface(w, h, D3DFMT_A16B16G16R16F, D3DPOOL_SYSTEMMEM, &sys, nullptr)) &&
              SUCCEEDED(dev->GetRenderTargetData(s, sys));
    D3DLOCKED_RECT r{};
    if (ok && SUCCEEDED(sys->LockRect(&r, nullptr, D3DLOCK_READONLY))) {
        out.resize(static_cast<size_t>(w) * h * 4);
        for (uint32_t y = 0; y < h; y++)
            for (uint32_t x = 0; x < w * 4; x++) {
                const uint16_t v = reinterpret_cast<const uint16_t*>(static_cast<const char*>(r.pBits) + y * r.Pitch)[x];
                const uint32_t sign = (v >> 15) & 1, e = (v >> 10) & 0x1F, m = v & 0x3FF;
                const float f = e == 0 ? std::ldexp(static_cast<float>(m), -24) : std::ldexp(static_cast<float>(m | 0x400), static_cast<int>(e) - 25);
                out[static_cast<size_t>(y) * w * 4 + x] = sign ? -f : f;
            }
        sys->UnlockRect();
    } else
        ok = false;
    Release(sys);
    Release(s);
    return ok;
}

void DeviceChecks(Gpu& gpu) {
    if (!gpu.dev) return;
    IDirect3DDevice9* dev = gpu.dev;
    const uint64_t waiting = GiSched::TileKey(0, 3, 2);
    Scene s = MakeScene(false, waiting);
    // Scatter: the atlas into the grid
    std::vector<DWORD> scatterCode = Compile(GiCompose::kScatterShader, "ps_2_0");
    IDirect3DPixelShader9* scatterPs = nullptr;
    IDirect3DTexture9* atlas = FloatTexture(dev, GiScene::kAtlasWidth, s.built.height, s.atlas);
    IDirect3DTexture9* grid = nullptr;
    Check(!scatterCode.empty() && SUCCEEDED(dev->CreatePixelShader(scatterCode.data(), &scatterPs)) && atlas &&
              SUCCEEDED(dev->CreateTexture(s.grid.w, s.grid.h, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &grid, nullptr)),
          "scatter shader, atlas and grid made");
    if (!scatterPs || !atlas || !grid) return;
    std::vector<GiCompose::ScatterPoint> pts;
    for (size_t i = 0; i < s.grid.points.size(); i++) pts.push_back({s.grid.points[i].grid, s.grid.points[i].atlas, s.ready[i]});
    GiCompose::Composer comp;
    int draws = 0;
    Check(comp.Scatter(dev, scatterPs, atlas, GiScene::kAtlasWidth, s.built.height, grid, s.grid.w, pts, {}, &draws) && draws == 1, "Scatter draws once");
    std::vector<float> got;
    float worst = 0.0f;
    if (ReadFloat16(dev, grid, s.grid.w, s.grid.h, got))
        for (size_t i = 0; i < got.size(); i++) worst = std::max(worst, std::fabs(got[i] - s.cpuGrid[i]));
    std::printf("  scatter: %zu points into %u x %u, worst %.5f against FillCpu\n", pts.size(), s.grid.w, s.grid.h, worst);
    Check(!got.empty() && worst < 1.0f / 1024.0f, "Scatter's grid = FillCpu (fp16)");
    // the patched copy of the game's pattern: texld r0, TEXCOORD1, s0; mad r0.xyz, r0.w, c0, r0
    const std::vector<DWORD> game = AssemblePs("ps_3_0\ndcl_texcoord1 v0.xy\ndcl_2d s0\ntexld r0, v0, s0\nmad r0.xyz, r0.w, c0, r0\nmov oC0, r0\n");
    std::vector<DWORD> t = game;
    ShaderPatches::RoomMapGiPatch gp;
    Check(!game.empty() && ShaderPatches::PatchRoomMapGi(t, gp) && gp.sampler == 1, "the synthetic room-lit shader takes the GI read");
    IDirect3DPixelShader9* ps = nullptr;
    Check(SUCCEEDED(dev->CreatePixelShader(t.data(), &ps)), "the patched copy is created natively");
    IDirect3DTexture9* map = nullptr;
    const float mapV[4] = {60 / 255.0f, 90 / 255.0f, 120 / 255.0f, 128 / 255.0f};
    if (SUCCEEDED(dev->CreateTexture(1, 1, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &map, nullptr))) {
        D3DLOCKED_RECT r{};
        if (SUCCEEDED(map->LockRect(0, &r, nullptr, 0))) *static_cast<DWORD*>(r.pBits) = 0x803C5A78u, map->UnlockRect(0);
    }
    if (!ps || !map) return;
    dev->SetTexture(0, map);
    dev->SetTexture(1, grid);
    for (DWORD st = 0; st < 2; st++) {
        dev->SetSamplerState(st, D3DSAMP_MINFILTER, st ? D3DTEXF_LINEAR : D3DTEXF_POINT);
        dev->SetSamplerState(st, D3DSAMP_MAGFILTER, st ? D3DTEXF_LINEAR : D3DTEXF_POINT);
        dev->SetSamplerState(st, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        dev->SetSamplerState(st, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        dev->SetSamplerState(st, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    }
    const float amb[4] = {0.3f, 0.2f, 0.1f, 0};
    dev->SetPixelShaderConstantF(0, amb, 1);
    float uv[4];
    GiIndirect::UvRow(s.grid, 0, GiIndirect::kFloor, uv);
    dev->SetPixelShaderConstantF(ShaderPatches::kGiUvConst, uv, 1);
    gpu.atlasU = -1.0f;
    struct Mode {
        const char* name;
        float k[4];
        float span;
    } const modes[] = {{"night, texel centres", {1, 1, 1, 1}, 32.0f}, {"night, between texels", {1, 1, 1, 1}, 8.0f}, {"day", {1, 1, 1, 0}, 8.0f}, {"debug", {1, 0, 0, 0}, 8.0f}};
    for (const Mode& m : modes) {
        dev->SetPixelShaderConstantF(ShaderPatches::kGiKConst, m.k, 1);
        gpu.lotSpan = m.span;
        const std::vector<DWORD> px = gpu.Render(ps);
        int worstLsb = 0, lit = 0, centres = 0;
        for (int y = 0; y < 64; y++)
            for (int x = 0; x < 64; x++) {
                const float lx = m.span * (x + 0.5f) / 64.0f, lz = m.span * (y + 0.5f) / 64.0f;
                float g[4], ref[3];
                GiIndirect::SampleCpu(s.grid, s.cpuGrid, 0, GiIndirect::kFloor, lx, lz, g);
                GiIndirect::Apply(mapV, g, m.k, amb, ref);
                if (lx >= 8.0f || lz >= 8.0f) continue; // (off the 8 x 8 lot: no floor of it is drawn there)
                const DWORD v = px[static_cast<size_t>(y) * 64 + x];
                const int gotC[3] = {static_cast<int>((v >> 16) & 0xFF), static_cast<int>((v >> 8) & 0xFF), static_cast<int>(v & 0xFF)};
                for (int c = 0; c < 3; c++) worstLsb = std::max(worstLsb, static_cast<int>(std::lround(std::fabs(gotC[c] - std::clamp(ref[c], 0.0f, 1.0f) * 255.0f))));
                lit += g[3] > 0.999f;
                // (texel centres) the texel's own atlas value: the place gives the texel
                if (m.span == 32.0f && g[3] > 0.999f) {
                    const float p[3] = {lx, GiScene::kOff, lz}, n[3] = {0, 1, 0};
                    const auto it = s.built.slots.find(GiScene::TexelKey(p, n));
                    if (it != s.built.slots.end() && std::fabs(g[0] - AtlasValue(it->second.pixel, 0)) < 1e-5f) centres++;
                }
            }
        std::printf("  patched copy (%s): worst %d LSB against Apply(SampleCpu), %d pixels fully ready%s\n", m.name, worstLsb, lit,
                    m.span == 32.0f ? (", " + std::to_string(centres) + " at a texel's centre read that texel").c_str() : "");
        Check(worstLsb <= 2 && lit > 0, "GPU = CPU reference within 2 LSB");
        if (m.span == 32.0f) Check(centres == lit, "every ready pixel at a texel centre reads its texel");
    }
    dev->SetTexture(0, nullptr), dev->SetTexture(1, nullptr);
    comp.Release();
    Release(ps), Release(map), Release(grid), Release(atlas), Release(scatterPs);
}

// 3. precomp
void PrecompChecks(const char* path, Gpu& gpu) {
    std::ifstream f(path, std::ios::binary);
    const std::vector<unsigned char> data{std::istreambuf_iterator<char>(f), {}};
    std::vector<Blob> blobs;
    std::vector<Tech> techs;
    Check(LoadPrecomp(data, blobs, techs), "precomp read");
    const char* roomLit[] = {"InteriorFloor", "InteriorWall", "InteriorWallWithBlacklight", "InteriorWallWithStrobelight", "InteriorWallWithStrobeAndBlacklight",
                             "FloorTileCeiling", "FloorWithBlacklight", "FloorWithStrobelight", "FloorWithStrobelightAndBlacklight", "Hideable"};
    std::map<uint32_t, std::string> names;
    for (const char* n : roomLit) names[Fnv1Lower(n)] = n;
    std::map<uint32_t, std::set<uint32_t>> techOf;
    for (const Tech& t : techs)
        for (const auto& [vs, ps] : t.passes) techOf[ps].insert(t.name);
    struct Row {
        int ps3 = 0, ps2 = 0, taken = 0;
    };
    std::map<std::string, Row> perTech;
    int matched = 0, ps2 = 0, ps2Refused = 0, unchanged = 0, refusals = 0;
    struct Kind {
        const char* name;
        int tried = 0, taken = 0, created = 0, maxSlots = 0, maxTemps = 0, maxAdded = 0;
    } kinds[] = {{"game shader"}, {"smooth copy"}, {"smooth + arches"}, {"smooth + walls"}};
    for (uint32_t i = 0; i < blobs.size(); i++) {
        const Blob& b = blobs[i];
        if (b.vs || b.code.empty() || !ShaderPatches::IsRoomMapPs(b.code)) continue;
        matched++;
        const bool ps3 = b.code[0] == 0xFFFF0300;
        bool anyTaken = false;
        for (int k = 0; k < 4; k++) {
            std::vector<DWORD> base = b.code;
            ShaderPatches::RoomMapPatch rp;
            if (k == 1 && !ShaderPatches::PatchRoomMapSmooth(base, rp, false)) continue;
            if (k == 2 && (!ShaderPatches::PatchRoomMapSmooth(base, rp, true) || rp.archConst < 0)) continue;
            if (k == 3 && (!ShaderPatches::PatchRoomMapSmooth(base, rp, false, true) || rp.maskSampler < 0)) continue;
            Kind& kd = kinds[k];
            kd.tried++;
            std::vector<DWORD> t = base;
            ShaderPatches::RoomMapGiPatch gp;
            if (!ShaderPatches::PatchRoomMapGi(t, gp)) {
                refusals++;
                unchanged += t == base;
                if (ps3 && k == 0) std::printf("  ps_3_0 refused: blob %u\n", i);
                continue;
            }
            kd.taken++;
            anyTaken |= k == 0;
            kd.maxSlots = std::max(kd.maxSlots, gp.slots), kd.maxTemps = std::max(kd.maxTemps, gp.temps), kd.maxAdded = std::max(kd.maxAdded, gp.added);
            kd.created += gpu.AcceptsPs(t);
        }
        if (!ps3) {
            ps2++;
            std::vector<DWORD> t = b.code;
            ShaderPatches::RoomMapGiPatch gp;
            ps2Refused += !ShaderPatches::PatchRoomMapGi(t, gp) && t == b.code;
        }
        if (const auto tt = techOf.find(i + 1); tt != techOf.end())
            for (uint32_t h : tt->second)
                if (const auto n = names.find(h); n != names.end()) {
                    Row& r = perTech[n->second];
                    (ps3 ? r.ps3 : r.ps2)++;
                    r.taken += anyTaken;
                }
    }
    std::printf("Precomp: %d room-lit pixel shaders (%d ps_2_0, all refused unchanged: %d)\n", matched, ps2, ps2Refused);
    for (const Kind& k : kinds)
        std::printf("  %-16s %4d tried, %4d take the GI read, %4d created natively; at most %d slots (+%d), %d temps\n", k.name, k.tried, k.taken, k.created, k.maxSlots,
                    k.maxAdded, k.maxTemps);
    for (const auto& [n, r] : perTech) std::printf("  %-38s ps_3_0 %3d (%3d take it), ps_2_0 %3d\n", n.c_str(), r.ps3, r.taken, r.ps2);
    Check(ps2 == ps2Refused, "every ps_2_0 shader refused, bytes unchanged");
    Check(unchanged == refusals, "every refusal leaves the bytes unchanged (indirect off: the same shaders)");
    for (const Kind& k : kinds) {
        Check(k.taken > 0, "some copies take the GI read");
        Check(k.created == k.taken || !gpu.dev, "every patched copy is created natively");
        Check(k.maxSlots <= 512 && k.maxTemps <= 32, "within ps_3_0 limits (512 slots, 32 temps)");
    }
}

} // namespace

int main(int argc, char** argv) {
    std::printf("GI grid (CPU)\n");
    GridChecks();
    Gpu gpu;
    std::printf("Native D3D9\n");
    DeviceChecks(gpu);
    if (argc >= 2) PrecompChecks(argv[1], gpu);
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
