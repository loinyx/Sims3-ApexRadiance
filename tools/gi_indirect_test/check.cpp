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

// one roofed room per story (8 x 8 lot), story 1 over story 0; story 0's room split in two by a wall at x = 3 (S2-06 bleed)
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
        for (int z = z0; z < z1 && s == 0; z++) wall(3.0f, float(z), 3.0f, float(z + 1));
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
    Check(GiIndirect::Build(s.built, o.density, 8, 8, s.grid, &s.lot), "the grid of the synthetic lot builds");
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
        // walls (S2-06): every axis-aligned wall texel reads back at its place through the wall vertex shader's uv
        size_t walls = 0, wallExact = 0;
        float wallWorst = 0.0f;
        for (const auto& [key, slot] : s.built.slots) {
            float p[3];
            int code;
            GiIndirect::DecodeKey(key, p, code);
            if (code >= 8 || (code & 1)) continue;
            walls++;
            float lp[3], uv[2], g[4];
            RtLampProxy::ToLot(s.lot, slot.world, lp);
            const float n[3] = {code == 0 ? 1.0f : code == 4 ? -1.0f : 0.0f, 0.0f, code == 2 ? 1.0f : code == 6 ? -1.0f : 0.0f};
            if (!GiIndirect::WallUvCpu(s.grid, lp, n, uv) || !GiIndirect::SampleUv(s.grid, s.cpuGrid, uv[0], uv[1], g)) continue;
            float d = std::fabs(g[3] - 1.0f);
            for (int c = 0; c < 3; c++) d = std::max(d, std::fabs(g[c] - AtlasValue(slot.pixel, c)));
            wallWorst = std::max(wallWorst, d);
            wallExact += d < 1e-4f;
        }
        std::printf("  walls (%s lot): %zu wall texels, %zu read back at their place, worst %.6f, region from column %u, %u along x %u high\n",
                    rotated ? "rotated" : "plain", walls, wallExact, wallWorst, s.grid.wallX0, s.grid.wallA, s.grid.wallY);
        Check(walls > 0 && wallExact == walls, "every axis-aligned wall texel's value at its place");
        // caps and diagonal facings read the empty block
        const float cap[3] = {0, 1, 0}, diag[3] = {0.7071f, 0, 0.7071f}, at[3] = {2.0f, 1.25f, 1.05f};
        bool empty = true;
        for (const float* n : {cap, diag}) {
            float uv[2], g[4];
            empty = empty && GiIndirect::WallUvCpu(s.grid, at, n, uv) && GiIndirect::SampleUv(s.grid, s.cpuGrid, uv[0], uv[1], g) && g[3] < 1e-6f && g[0] < 1e-6f;
        }
        Check(empty, "caps and diagonal facings read nothing");
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
              GiIndirect::KindOfSetIndex(2) < 0 && GiIndirect::KindOfSetIndex(3) == GiIndirect::kWall,
          "texture set index 0 = floor, 1 = ceiling, 3 = wall, objects no layer");
    // Apply (S2-06): the lamp share L of the room's ambient is what the ready atlas replaces; the base colour stays
    const float map[4] = {0.2f, 0.3f, 0.4f, 0.5f}, gi[4] = {0.6f, 0.5f, 0.4f, 1.0f}, amb[3] = {0.3f, 0.2f, 0.1f};
    float o[3], L[3];
    const float atlasK[4] = {1, 1, 1, 0}, debug[4] = {1, 0, 0, 0}, none[3] = {0, 0, 0};
    // night, Rooms at Night off: base colour 0, the whole ambient is lamp light
    const float baseNight[3] = {0, 0, 0};
    GiIndirect::LampShare(amb, baseNight, 1.0f, L);
    GiIndirect::Apply(map, gi, atlasK, amb, L, o);
    Check(std::fabs(o[0] - 0.8f) < 1e-6f, "night: lamp light + atlas, no game ambient");
    // day: the ambient is the base colour (daylight): no lamp share, the atlas adds to it
    GiIndirect::LampShare(amb, amb, 0.0f, L);
    GiIndirect::Apply(map, gi, atlasK, amb, L, o);
    Check(L[0] == 0 && std::fabs(o[0] - (0.2f + 0.6f + 0.5f * 0.3f)) < 1e-6f, "day: lamp light + atlas + the game's ambient");
    // evening, lamps on over a base colour: only the part over it is replaced, the base colour stays
    const float base[3] = {0.1f, 0.15f, 0.2f};
    GiIndirect::LampShare(amb, base, 1.0f, L);
    GiIndirect::Apply(map, gi, atlasK, amb, L, o);
    Check(std::fabs(L[0] - 0.2f) < 1e-6f && L[2] == 0 && std::fabs(o[0] - (0.2f + 0.6f + 0.5f * 0.1f)) < 1e-6f && std::fabs(o[2] - (0.4f + 0.4f + 0.5f * 0.1f)) < 1e-6f,
          "day and night: the lamp share (ambient over the base colour) alone is replaced");
    // the same lamp share by day and by night: the night factor does not reach a room whose base colour is known
    float Ld[3], Ln[3];
    GiIndirect::LampShare(amb, base, 0.0f, Ld), GiIndirect::LampShare(amb, base, 1.0f, Ln);
    Check(Ld[0] == Ln[0] && Ld[1] == Ln[1] && Ld[2] == Ln[2], "known base colour: the lamp share does not depend on the time of day");
    GiIndirect::LampShare(amb, nullptr, 0.0f, Ld), GiIndirect::LampShare(amb, nullptr, 1.0f, Ln);
    Check(Ld[0] == 0 && std::fabs(Ln[0] - 0.3f) < 1e-6f, "unknown base colour: by day nothing is replaced, by night the whole ambient");
    GiIndirect::Apply(map, gi, debug, amb, none, o);
    Check(std::fabs(o[0] - 0.6f) < 1e-6f && std::fabs(o[2] - 0.4f) < 1e-6f, "debug: the atlas alone");
    // a half-ready texel: G premultiplied, half the lamp share replaced
    const float half[4] = {0.3f, 0.25f, 0.2f, 0.5f};
    GiIndirect::LampShare(amb, baseNight, 1.0f, L);
    GiIndirect::Apply(map, half, atlasK, amb, L, o);
    Check(std::fabs(o[0] - (0.2f + 0.3f + 0.5f * 0.3f * 0.5f)) < 1e-6f, "half ready: half the lamp share replaced");

    // bleed (S2-06): a floor pixel 0.15 m from story 0's inner wall (x = 3) reads only its own room's texel
    const auto floorTexel = [&](float x, float z) {
        const float p[3] = {x, GiScene::kOff, z}, n[3] = {0, 1, 0};
        return s.built.slots.at(GiScene::TexelKey(p, n)).pixel;
    };
    GiIndirect::SampleCpu(s.grid, s.cpuGrid, 0, GiIndirect::kFloor, 2.85f, 2.25f, g);
    const uint32_t own = floorTexel(2.75f, 2.25f), other = floorTexel(3.25f, 2.25f);
    Check(std::fabs(g[0] - AtlasValue(own, 0)) < 1e-5f && std::fabs(AtlasValue(own, 0) - AtlasValue(other, 0)) > 1e-3f,
          "floor 0.15 m from an inner wall: its own room's texel only (none of the room beyond)");
    GiIndirect::SampleCpu(s.grid, s.cpuGrid, 0, GiIndirect::kFloor, 3.15f, 2.25f, g);
    Check(std::fabs(g[0] - AtlasValue(other, 0)) < 1e-5f, "and from the other side the same");
    GiIndirect::SampleCpu(s.grid, s.cpuGrid, 0, GiIndirect::kFloor, 2.5f, 2.25f, g);
    Check(std::fabs(g[0] - 0.5f * (AtlasValue(floorTexel(2.25f, 2.25f), 0) + AtlasValue(own, 0))) < 1e-5f, "inside the room the read still blends");
    // a wall pixel next to the junction of story 0's z = 1 wall (inside face, +z) and the inner wall: its own side only
    uint32_t wallOwn = ~0u, wallOther = ~0u;
    float wy = 0;
    for (const auto& [key, slot] : s.built.slots) {
        float p[3];
        int code;
        GiIndirect::DecodeKey(key, p, code);
        if (code != 2 || slot.story != 0 || std::fabs(p[2] - 1.05f) > 0.02f || std::fabs(p[1] - 1.25f) > 0.02f) continue;
        if (std::fabs(p[0] - 2.75f) < 0.02f) wallOwn = slot.pixel, wy = p[1];
        if (std::fabs(p[0] - 3.25f) < 0.02f) wallOther = slot.pixel;
    }
    Check(wallOwn != ~0u && wallOther != ~0u, "wall texels either side of the junction");
    if (wallOwn != ~0u) {
        const float at2[3] = {2.9f, wy, 1.05f}, nz[3] = {0, 0, 1};
        float uv[2];
        GiIndirect::WallUvCpu(s.grid, at2, nz, uv);
        GiIndirect::SampleUv(s.grid, s.cpuGrid, uv[0], uv[1], g);
        Check(std::fabs(g[0] - AtlasValue(wallOwn, 0)) < 1e-5f, "wall next to a junction: its own side's texel only");
        // and between the stories along the wall line (story 0's top texel row and story 1's bottom one)
        const float top[3] = {4.25f, 2.95f, 1.05f};
        GiIndirect::WallUvCpu(s.grid, top, nz, uv);
        GiIndirect::SampleUv(s.grid, s.cpuGrid, uv[0], uv[1], g);
        uint32_t below = ~0u;
        for (const auto& [key, slot] : s.built.slots) {
            float p[3];
            int code;
            GiIndirect::DecodeKey(key, p, code);
            if (code == 2 && slot.story == 0 && std::fabs(p[2] - 1.05f) < 0.02f && std::fabs(p[0] - 4.25f) < 0.02f && std::fabs(p[1] - 2.75f) < 0.02f) below = slot.pixel;
        }
        Check(below != ~0u && std::fabs(g[0] - AtlasValue(below, 0)) < 1e-5f, "wall at a story's top: its own story's texel only");
    }
}

// S2-06 item 8, objects and Sims: the rig write's point (RigPixel), its slots (RigSlots), who holds them (RigSlotState)
void RigChecks() {
    for (int rotated = 0; rotated < 2; rotated++) {
        Scene s = MakeScene(rotated != 0);
        size_t floors = 0, exact = 0;
        for (const auto& [key, slot] : s.built.slots) {
            float p[3];
            int code;
            GiIndirect::DecodeKey(key, p, code);
            if (code != 8) continue;
            floors++;
            float lp[3];
            RtLampProxy::ToLot(s.lot, slot.world, lp);
            lp[1] += 0.9f; // (a rig centre: the model's box centre, above its floor)
            uint32_t px = 0;
            if (!GiIndirect::RigPixel(s.grid, s.lot, lp, px)) continue;
            bool same = s.cpuGrid[static_cast<size_t>(px) * 4 + 3] > 0.999f;
            for (int c = 0; c < 3; c++) same &= std::fabs(s.cpuGrid[static_cast<size_t>(px) * 4 + c] - AtlasValue(slot.pixel, c)) < 1e-6f;
            exact += same;
        }
        std::printf("  rig point: %zu of %zu floor texels read back at a centre 0.9 m over them (%s lot)\n", exact, floors, rotated ? "rotated" : "plain");
        Check(floors > 0 && exact == floors, "a rig centre over a floor texel reads that texel (point read)");
    }
    Scene s = MakeScene(false);
    const auto floorTexel = [&](float x, float y, float z) {
        const float p[3] = {x, y + GiScene::kOff, z}, n[3] = {0, 1, 0};
        return s.built.slots.at(GiScene::TexelKey(p, n)).pixel;
    };
    const auto at = [&](float x, float y, float z, float out[4]) {
        const float p[3] = {x, y, z};
        uint32_t px = 0;
        if (!GiIndirect::RigPixel(s.grid, s.lot, p, px)) return false;
        for (int c = 0; c < 4; c++) out[c] = s.cpuGrid[static_cast<size_t>(px) * 4 + c];
        return true;
    };
    float g[4];
    Check(at(2.9f, 1.0f, 2.25f, g) && std::fabs(g[0] - AtlasValue(floorTexel(2.75f, 0, 2.25f), 0)) < 1e-6f, "a Sim 0.1 m from an inner wall: its own room only");
    Check(at(3.1f, 1.0f, 2.25f, g) && std::fabs(g[0] - AtlasValue(floorTexel(3.25f, 0, 2.25f), 0)) < 1e-6f, "and on the other side its room only");
    Check(at(3.25f, 3.9f, 2.25f, g) && std::fabs(g[0] - AtlasValue(floorTexel(3.25f, 3.0f, 2.25f), 0)) < 1e-6f, "on the upper story: the upper floor layer");
    Check(at(3.25f, 3.1f, 2.25f, g) && std::fabs(g[0] - AtlasValue(floorTexel(3.25f, 3.0f, 2.25f), 0)) < 1e-6f, "a low object on the upper story too");
    Check(!at(-0.5f, 1.0f, 2.25f, g) && !at(2.25f, 1.0f, 9.0f, g), "off the lot: no point (the rig keeps the game's values)");
    Check(at(0.25f, 1.0f, 0.25f, g) && g[3] == 0.0f && g[0] == 0.0f, "on the lot outside the rooms: nothing to add");
    const float under[3] = {2.25f, -1.0f, 2.25f};
    uint32_t px = 0;
    Check(!GiIndirect::RigPixel(s.grid, s.lot, under, px), "under the lowest floor: no point");
    // the slots: a regular tetrahedron, colour e each; the sum over normals is e on average
    const float e[3] = {0.4f, 0.2f, 0.1f};
    float slots[8][4];
    GiIndirect::RigSlots(e, slots);
    double sum = 0, lo = 1e9, hi = 0;
    const int n = 4000;
    for (int i = 0; i < n; i++) { // (a Fibonacci sphere)
        const double y = 1.0 - 2.0 * (i + 0.5) / n, r = std::sqrt(1.0 - y * y), phi = i * 2.399963229728653;
        const double nv[3] = {r * std::cos(phi), y, r * std::sin(phi)};
        double v = 0;
        for (int k = 0; k < 4; k++) v += slots[4 + k][0] * std::max(0.0, nv[0] * slots[k][0] + nv[1] * slots[k][1] + nv[2] * slots[k][2]);
        v /= e[0], sum += v, lo = std::min(lo, v), hi = std::max(hi, v);
    }
    std::printf("  rig slots: sum over %d normals / e: mean %.3f, min %.3f, max %.3f\n", n, sum / n, lo, hi);
    Check(std::fabs(sum / n - 1.0) < 0.01 && lo > 0.8 && hi < 1.5, "the four vertex lights give e on average over every normal");
    bool unit = true;
    for (int k = 0; k < 4; k++) unit &= std::fabs(slots[k][0] * slots[k][0] + slots[k][1] * slots[k][1] + slots[k][2] * slots[k][2] - 1.0f) < 1e-5f && slots[k][3] == 0;
    Check(unit && slots[5][1] == 0.2f && slots[7][2] == 0.1f && slots[4][3] == 0.0f, "unit directions, w 0, the colour e");
    const float big[3] = {2.0f, 0.5f, -1.0f};
    GiIndirect::RigSlots(big, slots);
    Check(slots[4][0] == 2.0f && slots[6][1] == 0.5f && slots[7][2] == 0.0f, "no channel clamp (the cap scales evenly), no negative light");
    {
        // the game's cap 0x006B92A0: one even scale cap / max(cap, brightest channel of the 6 axis sums) over lamps + slots
        float lamp[8][4] = {}, vl[8][4];
        lamp[0][1] = 1.0f, lamp[4][0] = 5.0f;               // slot 0 (the sun of a non-room rig) along +Y, red 5
        lamp[1][0] = 1.0f, lamp[5][0] = 0.6f, lamp[5][2] = 0.3f; // slot 1 along +X
        const float gl[3] = {0.4f, 0.2f, 0.1f};
        GiIndirect::RigSlots(gl, vl);
        double m = 0; // brute force: the 6 axis sums
        for (int a = 0; a < 6; a++)
            for (int c = 0; c < 3; c++) {
                double as = 0;
                for (int k = 1; k < 4; k++) as += std::max(0.0f, (a & 1 ? -1.0f : 1.0f) * lamp[k][a >> 1]) * lamp[4 + k][c];
                for (int k = 0; k < 4; k++) as += std::max(0.0f, (a & 1 ? -1.0f : 1.0f) * vl[k][a >> 1]) * vl[4 + k][c];
                m = std::max(m, as);
            }
        const float f = GiIndirect::RigCapScale(lamp, 1, vl, 0.5f);
        std::printf("  rig cap: brightest axis channel %.4f, cap 0.5, scale %.4f\n", m, f);
        Check(m > 0.5 && std::fabs(f - 0.5 / m) < 1e-5, "cap: scale = cap / brightest channel over lamps from the start slot + the 4 slots");
        Check(GiIndirect::RigCapScale(lamp, 1, vl, 2.0f) == 1.0f, "cap: under the cap, no scale");
        Check(GiIndirect::RigCapScale(lamp, 0, vl, 2.0f) < 1.0f, "cap: a room-mode rig (start 0) counts slot 0 too");
        Check(GiIndirect::RigCapScale(lamp, 1, vl, 0.0f) == 1.0f, "cap: no cap read, no scale");
        float zero[8][4] = {};
        Check(GiIndirect::RigCapScale(zero, 0, vl, 0.2f) < 1.0f && GiIndirect::RigCapScale(lamp, 1, zero, 1.0f) == 1.0f, "cap: slots alone and lamps alone");
    }
    // who holds the slots
    float zero[8][4] = {}, other[8][4];
    std::memcpy(other, slots, sizeof other);
    other[6][1] = 0.25f;
    Check(GiIndirect::RigSlotState(zero, nullptr) == GiIndirect::kRigEmpty && GiIndirect::RigSlotState(zero, slots) == GiIndirect::kRigEmpty, "cleared slots: empty");
    Check(GiIndirect::RigSlotState(slots, slots) == GiIndirect::kRigOurs, "the last write: ours");
    Check(GiIndirect::RigSlotState(other, slots) == GiIndirect::kRigGame && GiIndirect::RigSlotState(slots, nullptr) == GiIndirect::kRigGame,
          "anything else (overflow lamps, the fill light's moved slot): the game's, left alone");
    Check(GiIndirect::HalfToFloat(0x3C00) == 1.0f && GiIndirect::HalfToFloat(0xC000) == -2.0f && GiIndirect::HalfToFloat(0x0001) == std::ldexp(1.0f, -24) &&
              GiIndirect::HalfToFloat(0x7BFF) == 65504.0f && GiIndirect::HalfToFloat(0) == 0.0f,
          "fp16 decode of the probe read");
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
    // (S2-06 item 8) the rig probe: the atlas texel under each rig centre, one pixel a rig, as the runtime reads it back
    {
        std::unordered_map<uint32_t, size_t> pointOf;
        for (size_t i = 0; i < s.grid.points.size(); i++) pointOf[s.grid.points[i].grid] = i;
        std::vector<GiCompose::ScatterPoint> probe;
        std::vector<uint32_t> probePx;
        for (const auto& [key, slot] : s.built.slots) {
            float p[3];
            int code;
            GiIndirect::DecodeKey(key, p, code);
            if (code != 8) continue;
            float lp[3];
            RtLampProxy::ToLot(s.lot, slot.world, lp);
            lp[1] += 0.9f;
            uint32_t px = 0;
            if (!GiIndirect::RigPixel(s.grid, s.lot, lp, px) || !pointOf.count(px)) continue;
            const size_t i = pointOf[px];
            probe.push_back({static_cast<uint32_t>(probe.size()), s.grid.points[i].atlas, s.ready[i]});
            probePx.push_back(px);
        }
        IDirect3DTexture9* rt = nullptr;
        IDirect3DSurface9 *rs = nullptr, *sys = nullptr;
        const uint32_t pw = 1024, ph = 16;
        float pworst = 1.0f;
        if (SUCCEEDED(dev->CreateTexture(pw, ph, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &rt, nullptr)) &&
            comp.Scatter(dev, scatterPs, atlas, GiScene::kAtlasWidth, s.built.height, rt, pw, probe, {}) && SUCCEEDED(rt->GetSurfaceLevel(0, &rs)) &&
            SUCCEEDED(dev->CreateOffscreenPlainSurface(pw, ph, D3DFMT_A16B16G16R16F, D3DPOOL_SYSTEMMEM, &sys, nullptr)) && SUCCEEDED(dev->GetRenderTargetData(rs, sys))) {
            D3DLOCKED_RECT r{};
            if (SUCCEEDED(sys->LockRect(&r, nullptr, D3DLOCK_READONLY))) {
                pworst = 0.0f;
                for (size_t i = 0; i < probe.size(); i++) {
                    const uint16_t* h = reinterpret_cast<const uint16_t*>(static_cast<const char*>(r.pBits) + (i / pw) * r.Pitch) + (i % pw) * 4;
                    for (int c = 0; c < 3; c++) pworst = std::max(pworst, std::fabs(GiIndirect::HalfToFloat(h[c]) - s.cpuGrid[static_cast<size_t>(probePx[i]) * 4 + c]));
                }
                sys->UnlockRect();
            }
        }
        Release(sys), Release(rs), Release(rt);
        std::printf("  rig probe: %zu rigs, worst %.5f against the grid's texel (one waiting tile)\n", probe.size(), pworst);
        Check(!probe.empty() && pworst < 1.0f / 1024.0f, "the rig probe pass reads each rig's grid texel (premultiplied, fp16)");
    }
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
    IDirect3DTexture9* flags = nullptr;
    if (SUCCEEDED(dev->CreateTexture(s.grid.w, s.grid.h, 1, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &flags, nullptr))) {
        D3DLOCKED_RECT r{};
        if (SUCCEEDED(flags->LockRect(0, &r, nullptr, 0))) {
            for (uint32_t y = 0; y < s.grid.h; y++) std::memcpy(static_cast<char*>(r.pBits) + y * r.Pitch, &s.grid.flags[static_cast<size_t>(y) * s.grid.w * 4], s.grid.w * 4);
            flags->UnlockRect(0);
        }
    }
    Check(flags && gp.flagSampler == 2 && gp.ambientConst == 0, "flags texture made; the copy's flags sampler and ambient constant");
    if (!ps || !map || !flags) return;
    dev->SetTexture(0, map);
    dev->SetTexture(1, grid);
    dev->SetTexture(2, flags);
    for (DWORD st = 0; st < 3; st++) {
        dev->SetSamplerState(st, D3DSAMP_MINFILTER, st == 1 ? D3DTEXF_LINEAR : D3DTEXF_POINT);
        dev->SetSamplerState(st, D3DSAMP_MAGFILTER, st == 1 ? D3DTEXF_LINEAR : D3DTEXF_POINT);
        dev->SetSamplerState(st, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
        dev->SetSamplerState(st, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
        dev->SetSamplerState(st, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    }
    const float amb[4] = {0.3f, 0.2f, 0.1f, 0};
    dev->SetPixelShaderConstantF(0, amb, 1);
    float uv[4];
    GiIndirect::UvRow(s.grid, 0, GiIndirect::kFloor, uv);
    dev->SetPixelShaderConstantF(ShaderPatches::kGiUvConst, uv, 1);
    const float size[4] = {static_cast<float>(s.grid.w), static_cast<float>(s.grid.h), 1.0f / s.grid.w, 1.0f / s.grid.h};
    dev->SetPixelShaderConstantF(ShaderPatches::kGiSizeConst, size, 1);
    gpu.atlasU = -1.0f;
    struct Mode {
        const char* name;
        float k[4];
        float span;
        float lamp[4];
    } const modes[] = {{"night, texel centres", {1, 1, 1, 0}, 32.0f, {0.3f, 0.2f, 0.1f, 0}},
                       {"night, between texels and across the inner wall", {1, 1, 1, 0}, 8.0f, {0.3f, 0.2f, 0.1f, 0}},
                       {"lamp share over a base colour", {1, 1, 1, 0}, 8.0f, {0.2f, 0.05f, 0, 0}},
                       {"day", {1, 1, 1, 0}, 8.0f, {0, 0, 0, 0}},
                       {"debug", {1, 0, 0, 0}, 8.0f, {0, 0, 0, 0}}};
    for (const Mode& m : modes) {
        dev->SetPixelShaderConstantF(ShaderPatches::kGiKConst, m.k, 1);
        dev->SetPixelShaderConstantF(ShaderPatches::kGiLampConst, m.lamp, 1);
        gpu.lotSpan = m.span;
        const std::vector<DWORD> px = gpu.Render(ps);
        int worstLsb = 0, lit = 0, centres = 0;
        for (int y = 0; y < 64; y++)
            for (int x = 0; x < 64; x++) {
                const float lx = m.span * (x + 0.5f) / 64.0f, lz = m.span * (y + 0.5f) / 64.0f;
                float g[4], ref[3];
                GiIndirect::SampleCpu(s.grid, s.cpuGrid, 0, GiIndirect::kFloor, lx, lz, g);
                GiIndirect::Apply(mapV, g, m.k, amb, m.lamp, ref);
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
    dev->SetTexture(0, nullptr), dev->SetTexture(1, nullptr), dev->SetTexture(2, nullptr);
    comp.Release();
    Release(ps), Release(map), Release(flags), Release(grid), Release(atlas), Release(scatterPs);
}

// (S2-06) The wall vertex shader copy: a synthetic vs_3_0 in the InteriorWall VS's shape (position decode, normal decode, the
// world triple, the decode overwritten after it) patched by PatchWallGiVs; each wall texel of the rotated lot drawn as a point at
// its own pixel, TEXCOORD7 written out as float: the uv WallUvCpu gives (caps and a diagonal too)
void WallVsChecks(Gpu& gpu) {
    if (!gpu.dev) return;
    IDirect3DDevice9* dev = gpu.dev;
    Scene s = MakeScene(true);
    const std::vector<DWORD> game = AssemblePs("vs_3_0\ndef c10, 0.00390625, 0, 1, -2\ndcl_position v0\ndcl_normal v1\ndcl_texcoord v2\ndcl_position o0\n"
                                               "dcl_texcoord o1.xy\nmad r0.xzw, v0.xyzx, c10.xyxy, c10.yyyz\nmov r0.y, v0.y\nmad r3.xyz, -c10.w, v1, -c10.z\n"
                                               "mov o0, v2\ndp4 r4.x, r0, c4\ndp4 r4.y, r0, c5\ndp4 r4.z, r0, c6\nmul r3.xyz, r4, r4\nmov o1.xy, r3\n");
    std::vector<DWORD> t = game;
    Check(!game.empty() && ShaderPatches::PatchWallGiVs(t), "the synthetic wall vertex shader takes the uv copy");
    std::vector<DWORD> again = t;
    Check(!ShaderPatches::PatchWallGiVs(again) && again == t, "a copy that already writes TEXCOORD7 is refused unchanged");
    std::vector<DWORD> ps2 = AssemblePs("vs_2_0\ndcl_position v0\ndp4 r4.x, v0, c4\ndp4 r4.y, v0, c5\ndp4 r4.z, v0, c6\nmov oPos, r4\n");
    const std::vector<DWORD> ps2Before = ps2;
    Check(!ShaderPatches::PatchWallGiVs(ps2) && ps2 == ps2Before, "vs_2_0 refused unchanged");
    IDirect3DVertexShader9* vs = nullptr;
    IDirect3DPixelShader9* ps = nullptr;
    IDirect3DVertexDeclaration9* decl = nullptr;
    IDirect3DSurface9 *rt = nullptr, *sys = nullptr;
    const std::vector<DWORD> psCode = AssemblePs("ps_3_0\ndcl_texcoord7 v0.xy\nmov oC0, v0.xyxy\n");
    const D3DVERTEXELEMENT9 el[] = {{0, 0, D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_POSITION, 0}, {0, 16, D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_NORMAL, 0},
                                    {0, 32, D3DDECLTYPE_FLOAT4, 0, D3DDECLUSAGE_TEXCOORD, 0}, D3DDECL_END()};
    Check(SUCCEEDED(dev->CreateVertexShader(t.data(), &vs)) && SUCCEEDED(dev->CreatePixelShader(psCode.data(), &ps)) &&
              SUCCEEDED(dev->CreateVertexDeclaration(el, &decl)) &&
              SUCCEEDED(dev->CreateRenderTarget(64, 64, D3DFMT_A32B32G32R32F, D3DMULTISAMPLE_NONE, 0, FALSE, &rt, nullptr)) &&
              SUCCEEDED(dev->CreateOffscreenPlainSurface(64, 64, D3DFMT_A32B32G32R32F, D3DPOOL_SYSTEMMEM, &sys, nullptr)),
          "the patched wall VS copy is created natively; float target");
    if (vs && ps && decl && rt && sys) {
        struct V {
            float p[4], n[4], c[4];
        };
        struct Ref {
            float uv[2];
        };
        std::vector<V> pts;
        std::vector<Ref> refs;
        const auto& R = s.lot.worldToLot;
        const auto add = [&](const float world[3], const float nLot[3]) {
            float lp[3], uv[2], nw[3];
            for (int k = 0; k < 3; k++) nw[k] = nLot[0] * R[k][0] + nLot[1] * R[k][1] + nLot[2] * R[k][2]; // (lot = world x R, R a rotation)
            RtLampProxy::ToLot(s.lot, world, lp);
            if (!GiIndirect::WallUvCpu(s.grid, lp, nLot, uv) || pts.size() >= 4096) return;
            const int i = static_cast<int>(pts.size());
            V v{{world[0] * 256.0f, world[1], world[2] * 256.0f, 1}, {(nw[0] + 1) / 2, (nw[1] + 1) / 2, (nw[2] + 1) / 2, 0},
                {((i % 64) + 0.5f) / 32.0f - 1.0f, 1.0f - ((i / 64) + 0.5f) / 32.0f, 0.5f, 1}};
            pts.push_back(v), refs.push_back({{uv[0], uv[1]}});
        };
        for (const auto& [key, slot] : s.built.slots) {
            float p[3];
            int code;
            GiIndirect::DecodeKey(key, p, code);
            if (code >= 8 || (code & 1)) continue;
            const float n[3] = {code == 0 ? 1.0f : code == 4 ? -1.0f : 0.0f, 0.0f, code == 2 ? 1.0f : code == 6 ? -1.0f : 0.0f};
            add(slot.world, n);
        }
        const size_t walls = pts.size();
        const float w0[3] = {-28.0f, 3.25f, 52.0f}, cap[3] = {0, 1, 0}, diag[3] = {0.6f, 0, 0.8f}; // (not 45 degrees: a tie of |x| and |z| may pick either along)
        add(w0, cap), add(w0, diag);
        float r0[4], r1[4], rows[3][4];
        GiIndirect::WallRows(s.grid, r0, r1);
        for (int k = 0; k < 3; k++)
            for (int j = 0; j < 4; j++) rows[k][j] = R[j][k];
        const float id[3][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}};
        dev->SetVertexShaderConstantF(4, &id[0][0], 3);
        dev->SetVertexShaderConstantF(ShaderPatches::kWallGiVsConst, r0, 1);
        dev->SetVertexShaderConstantF(ShaderPatches::kWallGiVsConst + 1, r1, 1);
        dev->SetVertexShaderConstantF(ShaderPatches::kWallGiVsConst + 3, &rows[0][0], 3);
        IDirect3DSurface9* old = nullptr;
        dev->GetRenderTarget(0, &old);
        dev->SetRenderTarget(0, rt);
        dev->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1.0f, 0);
        dev->SetVertexDeclaration(decl);
        dev->SetVertexShader(vs);
        dev->SetPixelShader(ps);
        dev->BeginScene();
        Check(SUCCEEDED(dev->DrawPrimitiveUP(D3DPT_POINTLIST, static_cast<UINT>(pts.size()), pts.data(), sizeof(V))), "wall points drawn");
        dev->EndScene();
        dev->SetVertexShader(nullptr);
        dev->SetRenderTarget(0, old);
        Release(old);
        D3DLOCKED_RECT lr{};
        float worst = 0.0f;
        size_t exact = 0;
        if (SUCCEEDED(dev->GetRenderTargetData(rt, sys)) && SUCCEEDED(sys->LockRect(&lr, nullptr, D3DLOCK_READONLY))) {
            for (size_t i = 0; i < pts.size(); i++) {
                const float* px = reinterpret_cast<const float*>(static_cast<const char*>(lr.pBits) + (i / 64) * lr.Pitch) + (i % 64) * 4;
                // in texels of the grid: the read must land in the same texel at the same place
                const float d = std::max(std::fabs(px[0] - refs[i].uv[0]) * s.grid.w, std::fabs(px[1] - refs[i].uv[1]) * s.grid.h);
                worst = std::max(worst, d);
                exact += d < 1e-3f;
            }
            sys->UnlockRect();
        }
        std::printf("  wall VS copy: %zu wall texels + cap + diagonal, %zu within 1/1000 texel of WallUvCpu, worst %.6f texel\n", walls, exact, worst);
        Check(walls > 0 && exact == pts.size(), "the wall VS copy's TEXCOORD7 = WallUvCpu");
    }
    Release(sys), Release(rt), Release(decl), Release(ps), Release(vs);
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
    // (S2-06) the InteriorWall ps_3_0 shaders (game and smooth copies) with the wall uv (TEXCOORD7)
    int wallTried = 0, wallTaken = 0, wallCreated = 0, wallSlots = 0;
    for (uint32_t i = 0; i < blobs.size(); i++) {
        const Blob& b = blobs[i];
        if (b.vs || b.code.empty() || b.code[0] != 0xFFFF0300 || !ShaderPatches::IsRoomMapPs(b.code)) continue;
        bool wall = false;
        if (const auto tt = techOf.find(i + 1); tt != techOf.end())
            for (uint32_t h : tt->second)
                if (const auto n = names.find(h); n != names.end()) wall |= n->second.rfind("InteriorWall", 0) == 0;
        if (!wall) continue;
        for (int k = 0; k < 2; k++) {
            std::vector<DWORD> t = b.code;
            ShaderPatches::RoomMapPatch rp;
            if (k == 1 && !ShaderPatches::PatchRoomMapSmooth(t, rp, false)) continue;
            wallTried++;
            ShaderPatches::RoomMapGiPatch gp;
            if (!ShaderPatches::PatchRoomMapGi(t, gp, 7)) {
                std::printf("  wall ps_3_0 refused the TEXCOORD7 read: blob %u (%s)\n", i, k ? "smooth copy" : "game shader");
                continue;
            }
            wallTaken++, wallSlots = std::max(wallSlots, gp.slots);
            wallCreated += gpu.AcceptsPs(t);
        }
    }
    std::printf("  InteriorWall ps_3_0 with the wall uv: %d tried, %d take it, %d created natively, at most %d slots\n", wallTried, wallTaken, wallCreated, wallSlots);
    Check(wallTaken > 0 && wallTaken == wallTried && (wallCreated == wallTaken || !gpu.dev) && wallSlots <= 512, "every InteriorWall ps_3_0 takes the wall GI read");
    for (const Kind& k : kinds)
        std::printf("  %-16s %4d tried, %4d take the GI read, %4d created natively; at most %d slots (+%d), %d temps\n", k.name, k.tried, k.taken, k.created, k.maxSlots,
                    k.maxAdded, k.maxTemps);
    for (const auto& [n, r] : perTech) std::printf("  %-38s ps_3_0 %3d (%3d take it), ps_2_0 %3d\n", n.c_str(), r.ps3, r.taken, r.ps2);
    // (S2-06) the InteriorWall techniques' vs_3_0 vertex shaders take the wall uv copy, created natively
    std::set<uint32_t> wallVs;
    for (const Tech& tc : techs)
        if (const auto n = names.find(tc.name); n != names.end() && n->second.rfind("InteriorWall", 0) == 0)
            for (const auto& [vs, ps] : tc.passes)
                if (vs >= 1 && vs <= blobs.size() && !blobs[vs - 1].code.empty() && blobs[vs - 1].code[0] == 0xFFFE0300) wallVs.insert(vs - 1);
    int vsTaken = 0, vsCreated = 0;
    for (uint32_t i : wallVs) {
        std::vector<DWORD> t = blobs[i].code;
        if (!ShaderPatches::PatchWallGiVs(t)) {
            std::printf("  wall VS refused: blob %u\n", i);
            continue;
        }
        vsTaken++;
        IDirect3DVertexShader9* v = nullptr;
        vsCreated += gpu.dev && SUCCEEDED(gpu.dev->CreateVertexShader(t.data(), &v));
        Release(v);
    }
    std::printf("  InteriorWall vs_3_0 vertex shaders: %zu, %d take the wall uv copy, %d created natively\n", wallVs.size(), vsTaken, vsCreated);
    // the wall GI path covers the vs_3_0 / ps_3_0 passes only; the vs_2_0 / ps_2_0 ones keep the game's wall light
    std::map<std::pair<DWORD, DWORD>, int> wallPasses;
    for (const Tech& tc : techs)
        if (const auto n = names.find(tc.name); n != names.end() && n->second.rfind("InteriorWall", 0) == 0)
            for (const auto& [vs, ps] : tc.passes)
                if (vs >= 1 && vs <= blobs.size() && ps >= 1 && ps <= blobs.size() && !blobs[vs - 1].code.empty() && !blobs[ps - 1].code.empty() &&
                    ShaderPatches::IsRoomMapPs(blobs[ps - 1].code))
                    wallPasses[{blobs[vs - 1].code[0], blobs[ps - 1].code[0]}]++;
    for (const auto& [k, c] : wallPasses)
        std::printf("  InteriorWall* room-lit passes vs %s / ps %s: %d (%s)\n", k.first == 0xFFFE0300 ? "3_0" : "2_x", k.second == 0xFFFF0300 ? "3_0" : "2_x", c,
                    k.first == 0xFFFE0300 && k.second == 0xFFFF0300 ? "wall GI path" : "no wall GI path: not shader model 3");
    Check(!wallVs.empty() && vsTaken == static_cast<int>(wallVs.size()) && (vsCreated == vsTaken || !gpu.dev), "every InteriorWall vs_3_0 takes the wall uv copy");
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
    RigChecks();
    Gpu gpu;
    std::printf("Native D3D9\n");
    DeviceChecks(gpu);
    WallVsChecks(gpu);
    if (argc >= 2) PrecompChecks(argv[1], gpu);
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
