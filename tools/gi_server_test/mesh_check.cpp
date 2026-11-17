// Mesh-exact furniture casters, standalone (docs/features/realtime-gi/wp/X-MESHCAST.md; no bridge, no game): pixel_check.cpp's
// lot, kernel and reference with a table and two chairs, in two forms of the same picture.
//   boxes   (today, XR-04): each object is a box in the static set and in the texel layer; its mesh is a delta caster.
//   meshes  (this package): each object is its model's shadow mesh in the static set and in the texel layer, one bottom
//           level a model and an instance an object (the two chairs share theirs); no furniture delta caster.
// Both draw the same depth: the objects' LOD 0 meshes. The truth traces those meshes, the cabinet and the Sim.
// Checked: the depth of both forms is equal; the kernel against the CPU reference in both forms (pixel_check.cpp's
// check); the composed lamp light on the floor around the objects, (A + texel light) x multiplier, against the truth:
// the boxes form darkens it twice (box in the texel, mesh in the multiplier), the meshes form does not; the texel
// occlusion of a cube shadow map rasterized from the meshes (the SM3 tier) against rays.
// Printed: triangles, bottom-level bytes and build time a model, bytes for 100 objects.
// Read-only: prints to stdout, writes no file.
//   --casters <file>  casters.py's output (the game's chair and table); without it, synthetic ones
//   --device <text>   the device whose name holds <text>      --size <w> <h>  the depth's size (1920 1080)
#define PIXEL_CHECK_NO_MAIN
#include "pixel_check.cpp"
#include <array>
#include <cstdio>

namespace {

struct Model {
    std::string role;
    std::vector<Tri> lod0, shadow; // model space
};
struct Object {
    uint32_t model;
    float xf[12];          // model to world (rows)
    float lo[3], hi[3];    // the bounds of its LOD 0 in the world
};

// casters.py's file: "MCST", count; per model: instance, role (8 bytes), LOD 0 then shadow LOD as counts, xyz, abc
bool LoadCasters(const std::string& path, std::vector<Model>& out) {
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "rb") || !f) return false;
    const auto get = [f](void* p, size_t n) { return std::fread(p, 1, n, f) == n; };
    char magic[4];
    uint32_t count = 0;
    bool ok = get(magic, 4) && !std::memcmp(magic, "MCST", 4) && get(&count, 4) && count < 64;
    for (uint32_t m = 0; ok && m < count; m++) {
        uint64_t inst;
        char role[9] = {};
        Model model;
        ok = get(&inst, 8) && get(role, 8);
        model.role = role;
        for (std::vector<Tri>* lod : {&model.lod0, &model.shadow}) {
            uint32_t nv = 0, nt = 0;
            ok = ok && get(&nv, 4) && get(&nt, 4) && nv < (1u << 20) && nt < (1u << 20);
            std::vector<float> p(static_cast<size_t>(nv) * 3);
            std::vector<uint32_t> t(static_cast<size_t>(nt) * 3);
            ok = ok && get(p.data(), p.size() * 4) && get(t.data(), t.size() * 4);
            for (size_t i = 0; ok && i < t.size(); i += 3) {
                Tri tri;
                for (int k = 0; k < 3; k++) {
                    ok = ok && t[i + k] < nv;
                    if (ok) std::memcpy(tri.v[k], &p[static_cast<size_t>(t[i + k]) * 3], 12);
                }
                lod->push_back(tri);
            }
        }
        out.push_back(std::move(model));
    }
    std::fclose(f);
    return ok && !out.empty();
}

// A chair and a table of boxes (seat, back, four legs; top, four legs); the shadow mesh is the same mesh
std::vector<Model> Synthetic() {
    using namespace RtLampProxy;
    const auto part = [](Mesh& m, float cx, float cy, float cz, float hx, float hy, float hz) { AddShape(m, ObjShape{{cx, cy, cz}, {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}, {hx, hy, hz}}); };
    Mesh chair, table;
    part(chair, 0, 0.43f, 0, 0.22f, 0.02f, 0.22f), part(chair, 0, 0.75f, 0.2f, 0.22f, 0.30f, 0.02f);
    part(table, 0, 0.72f, 0, 0.8f, 0.03f, 0.45f);
    for (int i = 0; i < 4; i++) {
        part(chair, (i & 1) ? 0.19f : -0.19f, 0.205f, (i & 2) ? 0.19f : -0.19f, 0.02f, 0.205f, 0.02f);
        part(table, (i & 1) ? 0.72f : -0.72f, 0.345f, (i & 2) ? 0.38f : -0.38f, 0.03f, 0.345f, 0.03f);
    }
    return {{"chair", chair.tris, chair.tris}, {"table", table.tris, table.tris}};
}

V3 Apply(const float xf[12], const float p[3]) {
    return {xf[0] * p[0] + xf[1] * p[1] + xf[2] * p[2] + xf[3], xf[4] * p[0] + xf[5] * p[1] + xf[6] * p[2] + xf[7], xf[8] * p[0] + xf[9] * p[1] + xf[10] * p[2] + xf[11]};
}
std::vector<Tri> Place(const std::vector<Tri>& model, const float xf[12]) {
    std::vector<Tri> out(model.size());
    for (size_t i = 0; i < model.size(); i++)
        for (int k = 0; k < 3; k++) {
            const V3 p = Apply(xf, model[i].v[k]);
            out[i].v[k][0] = p.x, out[i].v[k][1] = p.y, out[i].v[k][2] = p.z;
        }
    return out;
}
void Bounds(const std::vector<Tri>& tris, float lo[3], float hi[3]) {
    for (int k = 0; k < 3; k++) lo[k] = 1e30f, hi[k] = -1e30f;
    for (const Tri& t : tris)
        for (const auto& v : t.v)
            for (int k = 0; k < 3; k++) lo[k] = std::min(lo[k], v[k]), hi[k] = std::max(hi[k], v[k]);
}

// The table at the texel tier's table (long side along x), one chair beside it as XR-04's, one across, turned
std::vector<Object> Layout(const std::vector<Model>& models) {
    std::vector<Object> objs;
    const auto put = [&](uint32_t m, float x, float z, float turn) {
        float lo[3], hi[3];
        Bounds(models[m].lod0, lo, hi);
        const float c = std::cos(turn), s = std::sin(turn), mx = 0.5f * (lo[0] + hi[0]), mz = 0.5f * (lo[2] + hi[2]);
        Object o{m, {c, 0, s, 0, 0, 1, 0, 0, -s, 0, c, 0}, {}, {}};
        const V3 mid = Apply(o.xf, std::array<float, 3>{mx, 0, mz}.data());
        o.xf[3] = x - mid.x, o.xf[11] = z - mid.z; // the model's footprint centred on (x, z), its floor kept
        Bounds(Place(models[m].lod0, o.xf), o.lo, o.hi);
        objs.push_back(o);
    };
    for (uint32_t m = 0; m < models.size(); m++) {
        float lo[3], hi[3];
        Bounds(models[m].lod0, lo, hi);
        if (models[m].role == "table") put(m, 12.5f, 13.0f, hi[0] - lo[0] < hi[2] - lo[2] ? 1.5707963f : 0.0f);
        if (models[m].role == "chair") put(m, 12.5f, 14.2f, 0.0f), put(m, 12.5f, 11.8f, 3.1415927f);
    }
    return objs;
}

RtLampProxy::Mesh Grid(const std::vector<Tri>& tris) {
    RtLampProxy::Mesh m;
    for (const Tri& t : tris) RtLampProxy::AddTri(m, t.v[0], t.v[1], t.v[2]);
    RtLampProxy::BuildGrid(m);
    return m;
}
const RtLampProxy::ObjShape kCabinet{{15.5f, 1.0f, 11.0f}, {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}}, {0.3f, 1.0f, 0.6f}};

// One form of the picture over pixel_check.cpp's lot, camera, lamps and Sim
PScene Form(const PScene& base, const std::vector<Model>& models, const std::vector<Object>& objs, bool meshes) {
    PScene ps = base;
    ps.boxes.clear(), ps.detail.clear(), ps.casters.clear(), ps.models.clear(), ps.placed.clear(), ps.shown.clear();
    ps.meshes = {}, ps.detailGrid = {};
    RtLampProxy::Mesh box;
    RtLampProxy::AddShape(box, kCabinet);
    ps.drawnBoxes = box.tris.size();
    std::vector<Tri> lod0, shadow;
    for (const Object& o : objs) {
        const std::vector<Tri> a = Place(models[o.model].lod0, o.xf), b = Place(models[o.model].shadow, o.xf);
        lod0.insert(lod0.end(), a.begin(), a.end()), shadow.insert(shadow.end(), b.begin(), b.end());
    }
    if (!meshes) {
        for (const Object& o : objs) {
            RtLampProxy::AddShape(box, {{0.5f * (o.lo[0] + o.hi[0]), 0.5f * (o.lo[1] + o.hi[1]), 0.5f * (o.lo[2] + o.hi[2])},
                                        {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}},
                                        {0.5f * (o.hi[0] - o.lo[0]), 0.5f * (o.hi[1] - o.lo[1]), 0.5f * (o.hi[2] - o.lo[2])}});
            ps.casters.push_back({{o.lo[0], o.lo[1], o.lo[2], 0}, {o.hi[0], o.hi[1], o.hi[2], 0}});
        }
        ps.detail = lod0, ps.twoSidedDetail = true, ps.detailGrid = Grid(lod0);
    } else {
        for (const Model& m : models) ps.models.push_back(m.shadow);
        for (const Object& o : objs) {
            PScene::Placed p{o.model, {}};
            std::memcpy(p.xf, o.xf, sizeof p.xf);
            ps.placed.push_back(p);
        }
        ps.meshes = Grid(shadow), ps.shown = lod0;
    }
    ps.boxes = box.tris;
    ps.casters.push_back(base.casters.back()); // the Sim
    return ps;
}

// ---------------------------------------------------------------- traces of the texel layer and of the truth
// The segment O -> e meets walls and floors, a triangle of `twoSided`, a triangle of `grid` or (when given) a capsule
bool Blocked(const PScene& ps, const std::vector<Tri>& twoSided, const RtLampProxy::Mesh& grid, const std::vector<Capsule>* caps, V3 O, V3 e) {
    thread_local std::vector<uint32_t> stamp;
    thread_local uint32_t mark = 0;
    const V3 d = e - O;
    const float len = Len(d), o[3] = {O.x, O.y, O.z}, b[3] = {e.x, e.y, e.z}, dd[3] = {d.x, d.y, d.z};
    if (!RtLampProxy::Clear(ps.walls, o, b, 0.0f, 1e-3f, &stamp, ++mark) || !RtLampProxy::Clear(grid, o, b, 0.0f, 1e-3f, &stamp, ++mark)) return true;
    for (const Tri& t : twoSided)
        if (RtLampProxy::SegTri(o, dd, t, 0.0f, 1.0f - 1e-3f / len)) return true;
    if (caps)
        for (const Capsule& c : *caps) {
            const float t = CapsuleEnter(O, d * (1.0f / len), At(c.a), At(c.b), c.r);
            if (t > 0.0f && t < len - 1e-3f) return true;
        }
    return false;
}

struct Truth {
    std::vector<Tri> cabinet;
    RtLampProxy::Mesh lod0;
};
bool NearObject(const std::vector<Object>& objs, V3 P, float grow) {
    for (const Object& o : objs)
        if (P.x > o.lo[0] - grow && P.x < o.hi[0] + grow && P.z > o.lo[2] - grow && P.z < o.hi[2] + grow) return true;
    return false;
}

// The lamp light a floor pixel around the objects ends with, (A + texel light) x multiplier, against the truth's
// A + lamp light; the 16 emitter samples of each of the pixel's lamps, the texel at the drawn point (shortcut: the texel
// layer's own resolution is not modelled; upgrade once XR-10 sets the texel density of floors under furniture)
struct Floor {
    long pixels = 0, darker = 0, lighter = 0;
    double mean = 0.0;
    float worst = 0.0f;
};
Floor Compose(const PScene& ps, const Drawn& dr, const std::vector<Pixel>& px, const Truth& truth, const std::vector<Object>& objs) {
    const int gw = dr.w / 4, gh = dr.h / 4;
    std::vector<float> err(px.size(), 1e30f);
    Rows(gh, [&](int y) {
        for (int x = 0; x < gw; x++) {
            const size_t i = static_cast<size_t>(y) * gw + x;
            const Pixel& p = px[i];
            if (!Drew(p)) continue;
            const size_t at = static_cast<size_t>(p.pos[3]);
            const V3 P = dr.P[at], N = dr.N[at], O = P + N * 1e-3f;
            if (dr.cls[at] != kWalls || N.y < 0.9f || P.y > 0.05f || !NearObject(objs, P, 0.4f)) continue;
            float lit[3] = {kA, kA, kA}, real[3] = {kA, kA, kA};
            for (uint32_t k = 0; k < kMaxLamps; k++) {
                const uint32_t id = p.sel >> (8 * k) & 0xFF;
                if (id == 0xFF) continue;
                const Lamp& l = ps.lamps[id];
                const float ge = Geo(l, P, N) / 16.0f;
                int vt = 0, vr = 0;
                for (uint32_t s = 0; s < 16; s++) {
                    float e[3];
                    EmitterPoint(l, s, e);
                    vt += !Blocked(ps, ps.boxes, ps.meshes, nullptr, O, At(e));
                    vr += !Blocked(ps, truth.cabinet, truth.lod0, &ps.capsules, O, At(e));
                }
                for (int ch = 0; ch < 3; ch++) lit[ch] += l.col[ch] * ge * vt, real[ch] += l.col[ch] * ge * vr;
            }
            float worst = 0.0f; // the channel furthest off, signed, in steps of 1/255 of the true value
            for (int ch = 0; ch < 3; ch++) {
                const float e = (lit[ch] * p.mul[ch] / real[ch] - 1.0f) * 255.0f;
                if (std::fabs(e) > std::fabs(worst)) worst = e;
            }
            err[i] = worst;
        }
    });
    Floor f;
    for (float e : err) {
        if (e > 1e29f) continue;
        f.pixels++, f.mean += e, f.darker += e < -8.0f, f.lighter += e > 8.0f, f.worst = std::min(f.worst, e);
    }
    f.mean /= std::max(1L, f.pixels);
    return f;
}

// ---------------------------------------------------------------- the SM3 tier: a cube shadow map a lamp, rasterized
struct Cube {
    int R;
    V3 L;
    std::vector<float> w; // 6 faces of R x R: the largest 1 / depth (0: nothing)
};
void RasterTri(Cube& c, const Tri& t) {
    constexpr float kNearFace = 0.02f;
    for (int f = 0; f < 6; f++) {
        const int ax = f >> 1;
        const float sg = (f & 1) ? -1.0f : 1.0f;
        std::array<std::array<float, 3>, 8> in{}, out{};
        int n = 3;
        for (int k = 0; k < 3; k++) {
            const float d[3] = {t.v[k][0] - c.L.x, t.v[k][1] - c.L.y, t.v[k][2] - c.L.z};
            in[k] = {d[(ax + 1) % 3], d[(ax + 2) % 3], sg * d[ax]};
        }
        int m = 0; // clip to depth > kNearFace
        for (int k = 0; k < n; k++) {
            const auto& a = in[k];
            const auto& b = in[(k + 1) % n];
            if (a[2] > kNearFace) out[m++] = a;
            if ((a[2] > kNearFace) != (b[2] > kNearFace)) {
                const float s = (kNearFace - a[2]) / (b[2] - a[2]);
                out[m++] = {a[0] + s * (b[0] - a[0]), a[1] + s * (b[1] - a[1]), kNearFace};
            }
        }
        if (m < 3) continue;
        std::array<std::array<float, 3>, 8> s{}; // pixel x, y and 1 / depth
        for (int k = 0; k < m; k++) s[k] = {(out[k][0] / out[k][2] * 0.5f + 0.5f) * c.R, (out[k][1] / out[k][2] * 0.5f + 0.5f) * c.R, 1.0f / out[k][2]};
        float* face = c.w.data() + static_cast<size_t>(f) * c.R * c.R;
        for (int k = 1; k + 1 < m; k++) {
            const auto &a = s[0], &b = s[k], &d = s[k + 1];
            const float area = (b[0] - a[0]) * (d[1] - a[1]) - (b[1] - a[1]) * (d[0] - a[0]);
            if (std::fabs(area) < 1e-12f) continue;
            const int x0 = std::max(0, static_cast<int>(std::floor(std::min({a[0], b[0], d[0]})))), x1 = std::min(c.R - 1, static_cast<int>(std::ceil(std::max({a[0], b[0], d[0]}))));
            const int y0 = std::max(0, static_cast<int>(std::floor(std::min({a[1], b[1], d[1]})))), y1 = std::min(c.R - 1, static_cast<int>(std::ceil(std::max({a[1], b[1], d[1]}))));
            for (int y = y0; y <= y1; y++)
                for (int x = x0; x <= x1; x++) {
                    const float px = x + 0.5f, py = y + 0.5f;
                    const float wa = ((b[0] - px) * (d[1] - py) - (b[1] - py) * (d[0] - px)) / area, wb = ((d[0] - px) * (a[1] - py) - (d[1] - py) * (a[0] - px)) / area, wd = 1.0f - wa - wb;
                    if (wa < 0.0f || wb < 0.0f || wd < 0.0f) continue;
                    float& v = face[static_cast<size_t>(y) * c.R + x];
                    v = std::max(v, wa * a[2] + wb * b[2] + wd * d[2]);
                }
        }
    }
}
// Is P (normal N) in the map's shadow: the point moves along its normal by 1.5 of the map's texels there, then is
// compared with a bias of 5 mm
bool CubeShadow(const Cube& c, V3 P, V3 N) {
    const V3 d0 = P - c.L;
    const V3 Q = P + N * (1.5f * 2.0f * std::max({std::fabs(d0.x), std::fabs(d0.y), std::fabs(d0.z)}) / c.R) - c.L;
    const float d[3] = {Q.x, Q.y, Q.z};
    const int ax = std::fabs(d[0]) >= std::fabs(d[1]) && std::fabs(d[0]) >= std::fabs(d[2]) ? 0 : std::fabs(d[1]) >= std::fabs(d[2]) ? 1 : 2;
    const int f = ax * 2 + (d[ax] < 0.0f);
    const float z = std::fabs(d[ax]); // (RasterTri's face coordinates: u, v = the next two axes over the depth)
    const int x = std::clamp(static_cast<int>((d[(ax + 1) % 3] / z * 0.5f + 0.5f) * c.R), 0, c.R - 1);
    const int y = std::clamp(static_cast<int>((d[(ax + 2) % 3] / z * 0.5f + 0.5f) * c.R), 0, c.R - 1);
    const float w = c.w[(static_cast<size_t>(f) * c.R + y) * c.R + x];
    return w > 0.0f && 1.0f / w < z - 5e-3f;
}

// ---------------------------------------------------------------- bottom-level bytes and build time of one model
VkDeviceSize BlasBytes(const Gpu& g, const std::vector<Tri>& tris, double& ms) {
    Trash trash;
    const auto t0 = std::chrono::steady_clock::now();
    const Buf verts = DeviceBuf(g, trash, tris.size() * sizeof(Tri), VK_BUFFER_USAGE_ACCELERATION_STRUCTURE_BUILD_INPUT_READ_ONLY_BIT_KHR, tris.data(), tris.size() * sizeof(Tri));
    VkAccelerationStructureGeometryKHR geo{VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_KHR};
    geo.geometryType = VK_GEOMETRY_TYPE_TRIANGLES_KHR;
    geo.geometry.triangles = {VK_STRUCTURE_TYPE_ACCELERATION_STRUCTURE_GEOMETRY_TRIANGLES_DATA_KHR};
    geo.geometry.triangles.vertexFormat = VK_FORMAT_R32G32B32_SFLOAT, geo.geometry.triangles.vertexStride = 12;
    geo.geometry.triangles.vertexData.deviceAddress = Addr(g, verts), geo.geometry.triangles.maxVertex = static_cast<uint32_t>(tris.size()) * 3 - 1;
    geo.geometry.triangles.indexType = VK_INDEX_TYPE_NONE_KHR;
    VkDeviceSize bytes = 0;
    Accel(g, trash, VK_ACCELERATION_STRUCTURE_TYPE_BOTTOM_LEVEL_KHR, geo, static_cast<uint32_t>(tris.size()), nullptr, &bytes);
    ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    vkDeviceWaitIdle(g.dev);
    for (auto it = trash.rbegin(); it != trash.rend(); ++it) (*it)();
    return bytes;
}
} // namespace

int main(int argc, char** argv) {
    std::string want, casters;
    int w = 1920, h = 1080;
    for (int i = 1; i < argc; i++) {
        const std::string a = argv[i];
        if (a == "--device" && i + 1 < argc) want = argv[++i];
        else if (a == "--casters" && i + 1 < argc) casters = argv[++i];
        else if (a == "--size" && i + 2 < argc) w = std::clamp(std::atoi(argv[i + 1]), 64, 3840) & ~3, h = std::clamp(std::atoi(argv[i + 2]), 64, 2160) & ~3, i += 2;
        else {
            std::printf("FAIL  unknown argument %s\n", a.c_str());
            return 1;
        }
    }
    std::vector<Model> models;
    if (!casters.empty() && !LoadCasters(casters, models)) {
        std::printf("FAIL  %s is not casters.py's output\n", casters.c_str());
        return 1;
    }
    const bool game = !models.empty();
    if (!game) models = Synthetic();
    const std::vector<Object> objs = Layout(models);
    const PScene base = BuildPixelScene(w, h);
    const PScene boxes = Form(base, models, objs, false), meshes = Form(base, models, objs, true);
    Truth truth;
    {
        RtLampProxy::Mesh cab;
        RtLampProxy::AddShape(cab, kCabinet);
        truth.cabinet = cab.tris, truth.lod0 = Grid(meshes.shown);
    }
    const Gpu g = OpenGpu(want, true);
    std::printf("Mesh casters on %s (%s meshes)\n", g.name.c_str(), game ? "the game's" : "synthetic");
    if (!g.rays) {
        std::printf("FAIL  the device has no ray queries: the kernel did not run\n");
        return 1;
    }
    for (const Model& m : models) {
        float lo[3], hi[3];
        Bounds(m.lod0, lo, hi);
        std::printf("  %s: %.2f x %.2f x %.2f m, LOD 0 %zu triangles, shadow mesh %zu\n", m.role.c_str(), hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2], m.lod0.size(), m.shadow.size());
    }

    // 1. The same picture
    const Drawn dr = Draw(boxes, w, h), dm = Draw(meshes, w, h);
    Check(dr.depth == dm.depth, "both forms draw the same depth");

    // 2. The kernel against the CPU reference, then the floor around the objects
    Floor floor[2];
    for (int form = 0; form < 2; form++) {
        const PScene& ps = form ? meshes : boxes;
        const char* const tag = form ? "meshes" : "boxes";
        std::printf("  [%s] static %zu + %zu triangles in %zu instances, delta %zu triangles + %zu capsules\n", tag, ps.walls.tris.size(), ps.boxes.size() + ps.meshes.tris.size(),
                    2 + ps.placed.size(), ps.detail.size(), ps.capsules.size());
        Kernel kernel(g, ps, dr);
        kernel.Dispatch(0, 1, 1.0f);
        uint32_t stat[2];
        const std::vector<Pixel> point = kernel.Dispatch(0, 1, 0.0f), ball = kernel.Dispatch(0, 16, 1.0f);
        kernel.Dispatch(0, 1, 1.0f, nullptr, stat);
        std::printf("    a frame: %u rays from %u guide pixels\n", stat[0], stat[1]);
        CheckOcclusion(ps, dr, point, 0.0f, 1, (std::string(tag) + ", point emitters").c_str(), 0.999);
        CheckOcclusion(ps, dr, ball, 1.0f, 16, (std::string(tag) + ", emitter balls").c_str(), 0.99);
        floor[form] = Compose(ps, dr, ball, truth, objs);
        const Floor& f = floor[form];
        std::printf("    floor around the objects, (A + texel) x multiplier against the truth: %ld pixels, mean %+.1f/255, darker by more than 8/255 on %.1f %%, "
                    "lighter on %.1f %%, worst %+.0f/255\n",
                    f.pixels, f.mean, 100.0 * f.darker / std::max(1L, f.pixels), 100.0 * f.lighter / std::max(1L, f.pixels), f.worst);
    }
    Check(floor[0].pixels > 100 && floor[0].darker > floor[0].pixels / 20, "boxes: the floor around the objects is darkened twice (the check sees XR-04's issue)");
    Check(floor[1].darker * 10 < floor[0].darker && floor[1].darker <= floor[1].pixels / 50, "meshes: no double darkening (at most 2 % of the floor pixels darker by 8/255)");

    // 3. The SM3 tier: texel occlusion from cube maps rasterized from the static meshes, against rays; and the texel
    // layer's occlusion of boxes and of shadow meshes against the truth's (rays, point lamps, floor texels at 8 a metre
    // within 0.6 m of an object)
    {
        std::vector<Tri> raster = meshes.walls.tris;
        raster.insert(raster.end(), meshes.boxes.begin(), meshes.boxes.end());
        raster.insert(raster.end(), meshes.meshes.tris.begin(), meshes.meshes.tris.end());
        std::vector<V3> texels;
        for (float z = 10.0625f; z < 16.0f; z += 0.125f)
            for (float x = 10.0625f; x < 16.0f; x += 0.125f)
                if (NearObject(objs, {x, 0, z}, 0.6f)) texels.push_back({x, 0.0f, z});
        const V3 up{0, 1, 0};
        for (const Lamp& l : base.lamps) {
            if (l.room != kRoomA) continue;
            long agreeBox = 0, agreeMesh = 0, shadowed = 0, agreeMap[2] = {};
            std::vector<uint8_t> ray(texels.size());
            for (size_t i = 0; i < texels.size(); i++) {
                const V3 O = texels[i] + up * 1e-3f;
                const bool t = Blocked(base, truth.cabinet, truth.lod0, nullptr, O, At(l.pos)), b = Blocked(boxes, boxes.boxes, boxes.meshes, nullptr, O, At(l.pos));
                ray[i] = Blocked(meshes, meshes.boxes, meshes.meshes, nullptr, O, At(l.pos));
                shadowed += t, agreeBox += b == t, agreeMesh += (ray[i] != 0) == t;
            }
            double ms[2] = {};
            for (int r = 0; r < 2; r++) {
                Cube c{r ? 1024 : 512, At(l.pos), {}};
                c.w.assign(static_cast<size_t>(6) * c.R * c.R, 0.0f);
                const auto t0 = std::chrono::steady_clock::now();
                for (const Tri& t : raster) RasterTri(c, t);
                ms[r] = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                for (size_t i = 0; i < texels.size(); i++) agreeMap[r] += CubeShadow(c, texels[i], up) == (ray[i] != 0);
            }
            const double n = static_cast<double>(texels.size());
            std::printf("  %s: %zu floor texels (%ld shadowed); against the truth: boxes %.1f %%, shadow meshes %.1f %% equal; cube map of the meshes against their rays: "
                        "512 %.1f %%, 1024 %.1f %% (%zu triangles, %.0f / %.0f ms on one CPU core)\n",
                        l.name, texels.size(), shadowed, 100.0 * agreeBox / n, 100.0 * agreeMesh / n, 100.0 * agreeMap[0] / n, 100.0 * agreeMap[1] / n, raster.size(), ms[0], ms[1]);
            Check(agreeMesh > agreeBox || agreeBox == static_cast<long>(texels.size()), std::string(l.name) + ": the shadow meshes give the texel layer a truer occlusion than the boxes");
            Check(agreeMesh >= static_cast<long>(n * 0.97), std::string(l.name) + ": the shadow meshes' occlusion equals the LOD 0 meshes' on 97 % of the texels");
            Check(agreeMap[1] >= static_cast<long>(n * 0.97), std::string(l.name) + ": the 1024 cube map's occlusion equals the rays' on 97 % of the texels");
        }
    }

    // 4. Memory and build time of the cache: a bottom level a model, an instance an object
    {
        double bytesPerTri = 0.0;
        size_t tris = 0;
        for (const Model& m : models)
            for (int lod = 0; lod < 2; lod++) {
                const std::vector<Tri>& t = lod ? m.shadow : m.lod0;
                double ms[3];
                VkDeviceSize b = 0;
                for (double& x : ms) b = BlasBytes(g, t, x);
                std::sort(ms, ms + 3);
                std::printf("  %s %s: %zu triangles, bottom level %.1f KiB (%.0f bytes a triangle), upload and build %.2f ms (median of 3, submit to idle)\n", m.role.c_str(),
                            lod ? "shadow mesh" : "LOD 0", t.size(), b / 1024.0, static_cast<double>(b) / t.size(), ms[1]);
                if (lod) bytesPerTri += static_cast<double>(b), tris += t.size();
            }
        bytesPerTri /= std::max<size_t>(1, tris);
        // The survey of the game's models (casters.py --survey): shadow mesh median 200 triangles, 90 % 848
        for (const int each : {200, 848})
            std::printf("  100 objects of %d shadow triangles, no model shared: bottom levels %.0f KiB + instances %.1f KiB; the CPU copy (36 bytes a triangle) %.0f KiB\n", each,
                        100.0 * each * bytesPerTri / 1024.0, 100.0 * sizeof(VkAccelerationStructureInstanceKHR) / 1024.0, 100.0 * each * 36.0 / 1024.0);
        Check(bytesPerTri > 0.0, "bottom levels built");
    }
    vkDestroyCommandPool(g.dev, g.pool, nullptr);
    vkDestroyDevice(g.dev, nullptr);
    vkDestroyInstance(g.inst, nullptr);
    std::printf(g_failed ? "FAILED: %d check(s)\n" : "All checks passed\n", g_failed);
    return g_failed ? 1 : 0;
}
