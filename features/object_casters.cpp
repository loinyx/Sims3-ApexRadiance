// Object casters: see object_casters.h
#include "object_casters.h"
#include "apex_log.h"
#include "object_casters_math.h"
#include "outdoor_delta.h"
#include "post_scene.h"
#include "rt_lamp_blockers.h"
#include "rt_lamp_gpu.h"
#include "rt_outdoor_lamps.h"
#include <windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <format>
#include <list>
#include <unordered_map>
#include <vector>

namespace ObjectCasters {
namespace {
using namespace ObjectCastersMath;
constexpr size_t kMeshBudget = 32u << 20;    // bytes of copied meshes (positions, uv, indices), least recently drawn out first
constexpr UINT kMaxTris = 120000;            // the lot space triangle list (32 B a corner: 11.5 MB of DEFAULT memory at most)
constexpr int kMaxTextures = 96;             // alpha-tested meshes' diffuse textures held (AddRef)
constexpr uint32_t kExpireFrames = 30;       // an instance in view but not drawn this many frames is gone
constexpr DWORD kEveryMs = 10000;

std::atomic<bool> g_option{false};
struct Family { // an object vertex shader: its POSITION0 register and whether it divides by .w (the probe's reading)
    IDirect3DVertexShader9* vs = nullptr;
    int posReg = -1;
    bool decodeW = false;
};
std::vector<Family> g_families;
struct DeclInfo { // a vertex declaration's POSITION0 and TEXCOORD0 (skinned: left out)
    IDirect3DVertexDeclaration9* decl = nullptr;
    bool skinned = false, havePos = false, haveUv = false;
    D3DVERTEXELEMENT9 pos{}, uv{};
};
std::vector<DeclInfo> g_decls; // shortcut: keyed by pointer (a freed declaration's address reused reads stale), cleared at a lot change
struct Mesh {
    std::vector<float> pos;  // 3 a vertex (object space)
    std::vector<float> uv;   // 2 a vertex (alpha-tested meshes only)
    std::vector<uint32_t> idx; // a triangle list into pos
    IDirect3DBaseTexture9* tex = nullptr; // alpha-tested: its diffuse texture (AddRef)
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f}; // object space
    size_t bytes = 0;
    std::list<uint64_t>::iterator lru;
};
std::unordered_map<uint64_t, Mesh> g_meshes;
std::list<uint64_t> g_lru; // front = most recently drawn
size_t g_meshBytes = 0;
int g_textures = 0;
struct Instance {
    uint64_t mesh = 0;
    float rows[12] = {};
    float centre[3] = {}; // world: its box's centre
    uint32_t lastFrame = 0;
};
std::unordered_map<uint64_t, Instance> g_inst;
struct Change {
    uint32_t gen;
    float p[3]; // world
};
std::vector<Change> g_changes; // the last kChanges appearances and departures
constexpr size_t kChanges = 256;
uint32_t g_gen = 1, g_frame = 0;
uintptr_t g_tracker = 0;
float g_xf[16] = {};
bool g_haveXf = false;
// the triangle list (DEFAULT) and its instances' places in it
IDirect3DVertexBuffer9* g_vb = nullptr;
UINT g_vbTris = 0;
uint32_t g_vbGen = 0;
float g_vbXf[16] = {};
struct Placed {
    UINT first, tris;
    IDirect3DBaseTexture9* tex;
    float lo[3], hi[3]; // lot space
};
std::vector<Placed> g_placed;
// counters of the 10 s summary
DWORD g_logAt = 0;
uint32_t g_tilesDrawn = 0, g_skinned = 0, g_decode = 0, g_lockFail = 0, g_uvFail = 0, g_overBudget = 0, g_rebuilds = 0;
struct Filtered {
    const RtLampGpu::BlockerTable* src = nullptr;
    std::shared_ptr<const RtLampGpu::BlockerTable> keep, out;
    uint32_t gen = 0;
};
std::array<Filtered, 8> g_filtered;
int g_filteredNext = 0;

void ReadFamily(Family& f) {
    UINT size = 0;
    if (FAILED(f.vs->GetFunction(nullptr, &size)) || size < 8 || size > (1u << 20)) return;
    std::vector<DWORD> t(size / 4);
    if (FAILED(f.vs->GetFunction(t.data(), &size))) return;
    for (size_t i = 1; i < t.size();) {
        const DWORD tok = t[i], op = tok & 0xFFFF;
        if (op == 0xFFFF) break;
        if (op == 0xFFFE) {
            i += 1 + ((tok >> 16) & 0x7FFF);
            continue;
        }
        const size_t len = (tok >> 24) & 0xF;
        if (op == 0x1F && i + 2 < t.size()) { // dcl usage, register
            const DWORD usage = t[i + 1] & 0x1F, index = (t[i + 1] >> 16) & 0xF, reg = t[i + 2];
            const DWORD type = ((reg >> 28) & 7) | ((reg >> 8) & 0x18);
            if (usage == D3DDECLUSAGE_POSITION && index == 0 && type == D3DSPR_INPUT) f.posReg = static_cast<int>(reg & 0x7FF);
        } else if (op == 6 && i + 2 < t.size() && f.posReg >= 0) { // rcp dst, vPos.w
            const DWORD src = t[i + 2], type = ((src >> 28) & 7) | ((src >> 8) & 0x18);
            if (type == D3DSPR_INPUT && static_cast<int>(src & 0x7FF) == f.posReg && ((src >> 16) & 0xFF) == 0xFF) f.decodeW = true;
        }
        i += 1 + len;
    }
}
const Family& FamilyOf(IDirect3DVertexShader9* vs) {
    for (const Family& f : g_families)
        if (f.vs == vs) return f;
    if (g_families.size() >= 64) g_families.clear(); // (shaders are few; a long session's turnover starts again)
    g_families.push_back({vs});
    ReadFamily(g_families.back());
    return g_families.back();
}
void Touch(Mesh& m, uint64_t key) {
    g_lru.erase(m.lru);
    g_lru.push_front(key);
    m.lru = g_lru.begin();
}
void DropMesh(uint64_t key) {
    const auto it = g_meshes.find(key);
    if (it == g_meshes.end()) return;
    if (it->second.tex) it->second.tex->Release(), g_textures--;
    g_meshBytes -= it->second.bytes;
    g_lru.erase(it->second.lru);
    g_meshes.erase(it);
}
void NoteChange(const float p[3]) {
    g_gen++;
    if (g_changes.size() >= kChanges) g_changes.erase(g_changes.begin());
    g_changes.push_back({g_gen, {p[0], p[1], p[2]}});
}
void DropInstance(std::unordered_map<uint64_t, Instance>::iterator it) {
    NoteChange(it->second.centre);
    g_inst.erase(it);
}
void ClearAll() {
    for (auto& [k, m] : g_meshes)
        if (m.tex) m.tex->Release();
    g_meshes.clear(), g_lru.clear(), g_meshBytes = 0, g_textures = 0;
    g_inst.clear(), g_changes.clear(), g_placed.clear(), g_decls.clear(), g_families.clear();
    g_gen++;
    if (g_vb) g_vb->Release(), g_vb = nullptr;
    g_vbTris = 0, g_vbGen = 0;
}
// The vertex range's positions (+ uv) and the triangles' indices copied from MANAGED buffers (read-only locks of their
// system copies). False: not a mesh this can copy
bool CopyMesh(IDirect3DDevice9* dev, const D3DVERTEXELEMENT9& pe, const D3DVERTEXELEMENT9* te, INT baseVertex, UINT minV, UINT numV, UINT start, UINT prims, bool alpha, Mesh& m) {
    IDirect3DVertexBuffer9* vb = nullptr;
    UINT off = 0, stride = 0;
    if (FAILED(dev->GetStreamSource(pe.Stream, &vb, &off, &stride)) || !vb) return false;
    IDirect3DIndexBuffer9* ib = nullptr;
    bool ok = false;
    D3DVERTEXBUFFER_DESC vd{};
    D3DINDEXBUFFER_DESC id{};
    if (SUCCEEDED(vb->GetDesc(&vd)) && vd.Pool == D3DPOOL_MANAGED && SUCCEEDED(dev->GetIndices(&ib)) && ib && SUCCEEDED(ib->GetDesc(&id)) && id.Pool == D3DPOOL_MANAGED) {
        const UINT isz = id.Format == D3DFMT_INDEX32 ? 4 : 2;
        const UINT vFirst = static_cast<UINT>(baseVertex) + minV;
        const size_t vEnd = static_cast<size_t>(off) + static_cast<size_t>(vFirst + numV) * stride, iEnd = static_cast<size_t>(start + prims * 3) * isz;
        BYTE* v = nullptr;
        BYTE* ix = nullptr;
        if (baseVertex >= 0 && vEnd <= vd.Size && iEnd <= id.Size && SUCCEEDED(vb->Lock(0, 0, reinterpret_cast<void**>(&v), D3DLOCK_READONLY)) && v) {
            if (SUCCEEDED(ib->Lock(0, 0, reinterpret_cast<void**>(&ix), D3DLOCK_READONLY)) && ix) {
                m.pos.resize(static_cast<size_t>(numV) * 3);
                if (alpha) m.uv.resize(static_cast<size_t>(numV) * 2);
                bool uvOk = true;
                for (UINT i = 0; i < numV; i++) {
                    const BYTE* p = v + off + static_cast<size_t>(vFirst + i) * stride;
                    int16_t s[4];
                    std::memcpy(s, p + pe.Offset, sizeof s);
                    DecodeShort4(s, &m.pos[3 * i]);
                    for (int k = 0; k < 3; k++) m.lo[k] = std::min(m.lo[k], m.pos[3 * i + k]), m.hi[k] = std::max(m.hi[k], m.pos[3 * i + k]);
                    if (!alpha) continue;
                    const BYTE* t = p + te->Offset;
                    float* u = &m.uv[2 * i];
                    if (te->Type == D3DDECLTYPE_FLOAT2 || te->Type == D3DDECLTYPE_FLOAT3 || te->Type == D3DDECLTYPE_FLOAT4) std::memcpy(u, t, 8);
                    else if (te->Type == D3DDECLTYPE_FLOAT16_2 || te->Type == D3DDECLTYPE_FLOAT16_4) {
                        uint16_t h[2];
                        std::memcpy(h, t, 4);
                        u[0] = Half(h[0]), u[1] = Half(h[1]);
                    } else if (te->Type == D3DDECLTYPE_SHORT2N || te->Type == D3DDECLTYPE_SHORT4N) {
                        int16_t q[2];
                        std::memcpy(q, t, 4);
                        u[0] = q[0] / 32767.0f, u[1] = q[1] / 32767.0f;
                    } else
                        uvOk = false;
                }
                m.idx.resize(static_cast<size_t>(prims) * 3);
                bool idxOk = true;
                for (UINT i = 0; i < prims * 3; i++) {
                    const uint32_t k = isz == 4 ? reinterpret_cast<const uint32_t*>(ix)[start + i] : reinterpret_cast<const uint16_t*>(ix)[start + i];
                    if (k < minV || k - minV >= numV) idxOk = false;
                    m.idx[i] = k - minV;
                }
                ib->Unlock();
                if (!uvOk) g_uvFail++, m.uv.clear(); // (an alpha-tested mesh with another uv type: drawn opaque)
                ok = idxOk;
            }
            vb->Unlock();
        }
    }
    if (!ok) g_lockFail++;
    if (ib) ib->Release();
    vb->Release();
    m.bytes = m.pos.size() * 4 + m.uv.size() * 4 + m.idx.size() * 4;
    return ok;
}
} // namespace

void SetRealShapes(bool on) { g_option.store(on, std::memory_order_relaxed); }
bool Active() { return g_option.load(std::memory_order_relaxed) && OutdoorDelta::RoomLampPhysical(); }

void OnObjectDraw(IDirect3DDevice9* dev, IDirect3DVertexShader9* vs, int worldK, bool onLot, INT baseVertex, UINT minV, UINT numV, UINT start, UINT prims) {
    if (!dev || !vs || worldK < 0 || !onLot || !numV || !prims) return;
    const Family& fam = FamilyOf(vs);
    if (fam.posReg < 0 || !fam.decodeW) return void(g_decode++); // (the lot's own families decode xyz alone: walls and floors the proxy holds)
    IDirect3DVertexDeclaration9* decl = nullptr;
    if (FAILED(dev->GetVertexDeclaration(&decl)) || !decl) return;
    decl->Release(); // (only its identity is kept: the device holds it while bound)
    const DeclInfo* di = nullptr;
    for (const DeclInfo& d : g_decls)
        if (d.decl == decl) di = &d;
    if (!di) {
        D3DVERTEXELEMENT9 e[MAXD3DDECLLENGTH + 1];
        UINT n = MAXD3DDECLLENGTH + 1;
        if (FAILED(decl->GetDeclaration(e, &n))) return;
        if (g_decls.size() >= 128) g_decls.clear();
        DeclInfo d;
        d.decl = decl;
        for (UINT i = 0; i < n && e[i].Stream != 0xFF; i++) {
            if (e[i].Usage == D3DDECLUSAGE_BLENDINDICES || e[i].Usage == D3DDECLUSAGE_BLENDWEIGHT) d.skinned = true; // (Sims and skinned objects)
            if (e[i].Usage == D3DDECLUSAGE_POSITION && e[i].UsageIndex == 0) d.pos = e[i], d.havePos = true;
            if (e[i].Usage == D3DDECLUSAGE_TEXCOORD && e[i].UsageIndex == 0) d.uv = e[i], d.haveUv = true;
        }
        g_decls.push_back(d);
        di = &g_decls.back();
    }
    if (di->skinned) return void(g_skinned++);
    const D3DVERTEXELEMENT9* pe = di->havePos ? &di->pos : nullptr;
    const D3DVERTEXELEMENT9* te = di->haveUv ? &di->uv : nullptr;
    if (!pe || pe->Type != D3DDECLTYPE_SHORT4) return void(g_decode++);
    float rows[12];
    if (FAILED(dev->GetVertexShaderConstantF(static_cast<UINT>(worldK), rows, 3))) return;
    DWORD at = FALSE;
    dev->GetRenderState(D3DRS_ALPHATESTENABLE, &at);
    const bool alpha = at && te && te->Stream == pe->Stream;
    IDirect3DVertexBuffer9* vb = nullptr;
    UINT off = 0, stride = 0;
    if (FAILED(dev->GetStreamSource(pe->Stream, &vb, &off, &stride)) || !vb) return;
    IDirect3DIndexBuffer9* ib = nullptr;
    dev->GetIndices(&ib);
    uint64_t key = 1469598103934665603ull;
    const auto mix = [&](uint64_t v) { key = (key ^ v) * 1099511628211ull; };
    mix(reinterpret_cast<uintptr_t>(vb)), mix(reinterpret_cast<uintptr_t>(ib)), mix(off), mix(static_cast<uint32_t>(baseVertex)), mix(minV), mix(numV), mix(start), mix(prims);
    vb->Release();
    if (ib) ib->Release();
    auto mit = g_meshes.find(key);
    if (mit == g_meshes.end()) {
        Mesh m;
        if (!CopyMesh(dev, *pe, te, baseVertex, minV, numV, start, prims, alpha, m)) return;
        if (alpha && !m.uv.empty() && g_textures < kMaxTextures) {
            IDirect3DBaseTexture9* t = nullptr;
            if (SUCCEEDED(dev->GetTexture(0, &t)) && t) m.tex = t, g_textures++; // (GetTexture's reference kept until the mesh goes)
        }
        while (g_meshBytes + m.bytes > kMeshBudget && !g_lru.empty()) DropMesh(g_lru.back());
        if (g_meshBytes + m.bytes > kMeshBudget) {
            if (m.tex) m.tex->Release(), g_textures--;
            return void(g_overBudget++);
        }
        g_lru.push_front(key);
        m.lru = g_lru.begin();
        g_meshBytes += m.bytes;
        mit = g_meshes.emplace(key, std::move(m)).first;
    } else
        Touch(mit->second, key);
    // the instance: this mesh at these world rows (a moved object is a new instance; the old one expires in view)
    uint64_t ik = key;
    for (float r : rows) ik = (ik ^ static_cast<uint64_t>(static_cast<int64_t>(std::lround(r * 256.0f)))) * 1099511628211ull;
    auto [it, fresh] = g_inst.try_emplace(ik);
    it->second.lastFrame = g_frame;
    if (fresh) {
        Instance& in = it->second;
        in.mesh = key;
        std::memcpy(in.rows, rows, sizeof rows);
        const Mesh& m = mit->second;
        const float c[3] = {(m.lo[0] + m.hi[0]) * 0.5f, (m.lo[1] + m.hi[1]) * 0.5f, (m.lo[2] + m.hi[2]) * 0.5f};
        ApplyRows(rows, c, in.centre);
        NoteChange(in.centre);
    }
}

void SetLotXf(const float xf[16]) {
    if (std::memcmp(g_xf, xf, sizeof g_xf)) std::memcpy(g_xf, xf, sizeof g_xf), g_gen++;
    g_haveXf = true;
}

uint32_t Gen() { return Active() ? g_gen : 0; }

bool ChangedNear(uint32_t since, const float lotPos[3], float r) {
    if (!g_haveXf) return true;
    if (g_changes.empty() || g_changes.front().gen > since + 1) return since != g_gen; // (the record of some changes is gone)
    for (const Change& c : g_changes) {
        if (c.gen <= since) continue;
        float q[3];
        ApplyXf(g_xf, c.p, q);
        const float dx = q[0] - lotPos[0], dy = q[1] - lotPos[1], dz = q[2] - lotPos[2];
        if (dx * dx + dy * dy + dz * dz <= r * r) return true;
    }
    return false;
}

void OnPresent() {
    g_frame++;
    const DWORD now = GetTickCount();
    if (!Active()) {
        if (!g_inst.empty() || !g_meshes.empty()) ClearAll();
        return;
    }
    const uintptr_t tracker = RtOutdoorLamps::ActiveTracker();
    if (tracker != g_tracker) ClearAll(), g_tracker = tracker, g_haveXf = false;
    float vp[4][4];
    if (PostScene::RecentCameraViewProj(vp)) // (without the camera, instances stay until the lot changes or the option goes off)
        for (auto it = g_inst.begin(); it != g_inst.end();)
            if (g_frame - it->second.lastFrame > kExpireFrames && InView(vp, it->second.centre)) {
                const auto next = std::next(it);
                DropInstance(it);
                it = next;
            } else
                ++it;
    if (!g_logAt) g_logAt = now;
    if (now - g_logAt >= kEveryMs) {
        g_logAt = now;
        uint32_t alpha = 0;
        for (const auto& [k, in] : g_inst)
            if (const auto m = g_meshes.find(in.mesh); m != g_meshes.end() && m->second.tex) alpha++;
        LOG_INFO(std::format("[ObjectCasters] 10 s: {} objects ({} alpha-tested) of {} meshes, {} KB copied, {} triangles in the lot list, {} rebuilds, {} tiles drawn "
                             "with them; left out: {} skinned draws, {} of another position decode, {} buffers not readable, {} uv types, {} over the budget",
                             g_inst.size(), alpha, g_meshes.size(), g_meshBytes / 1024, g_vbTris, g_rebuilds, g_tilesDrawn, g_skinned, g_decode, g_lockFail, g_uvFail,
                             g_overBudget));
        g_tilesDrawn = g_skinned = g_decode = g_lockFail = g_uvFail = g_overBudget = g_rebuilds = 0;
    }
}

namespace {
// The lot space triangle list of every instance whose mesh is still held, made again when the instances or the lot change
bool Build(IDirect3DDevice9* dev) {
    if (g_vb && g_vbGen == g_gen && !std::memcmp(g_vbXf, g_xf, sizeof g_xf)) return true;
    UINT tris = 0;
    for (const auto& [k, in] : g_inst)
        if (const auto m = g_meshes.find(in.mesh); m != g_meshes.end()) tris += static_cast<UINT>(m->second.idx.size() / 3);
    tris = std::min(tris, kMaxTris);
    if (g_vb && g_vbTris < tris) g_vb->Release(), g_vb = nullptr;
    if (!g_vb && tris &&
        FAILED(dev->CreateVertexBuffer(std::max(tris, 1024u) * 3 * sizeof(RtLampGpu::CasterVertex), D3DUSAGE_WRITEONLY | D3DUSAGE_DYNAMIC, 0, D3DPOOL_DEFAULT, &g_vb, nullptr)))
        return g_vb = nullptr, false;
    g_placed.clear();
    g_vbTris = 0;
    RtLampGpu::CasterVertex* out = nullptr;
    if (tris && FAILED(g_vb->Lock(0, tris * 3 * sizeof(RtLampGpu::CasterVertex), reinterpret_cast<void**>(&out), D3DLOCK_DISCARD))) return false;
    // alpha-tested instances last, grouped by texture, so a tile switches its pixel shader and texture rarely
    std::vector<const Instance*> order;
    order.reserve(g_inst.size());
    for (const auto& [k, in] : g_inst) order.push_back(&in);
    const auto texOf = [](const Instance* in) {
        const auto m = g_meshes.find(in->mesh);
        return m == g_meshes.end() ? nullptr : m->second.tex;
    };
    std::sort(order.begin(), order.end(), [&](const Instance* a, const Instance* b) { return reinterpret_cast<uintptr_t>(texOf(a)) < reinterpret_cast<uintptr_t>(texOf(b)); });
    for (const Instance* in : order) {
        const auto mit = g_meshes.find(in->mesh);
        if (mit == g_meshes.end()) continue;
        const Mesh& m = mit->second;
        const UINT mt = static_cast<UINT>(m.idx.size() / 3);
        if (g_vbTris + mt > tris) continue; // shortcut: past kMaxTris an object casts its game shape only (logged as triangles), raise with a split list
        Placed pl{g_vbTris, mt, m.uv.empty() ? nullptr : m.tex, {1e30f, 1e30f, 1e30f}, {-1e30f, -1e30f, -1e30f}};
        for (UINT t = 0; t < mt; t++) {
            float p[3][3];
            for (int c = 0; c < 3; c++) {
                float w[3];
                ApplyRows(in->rows, &m.pos[3 * m.idx[3 * t + c]], w);
                ApplyXf(g_xf, w, p[c]);
                for (int k = 0; k < 3; k++) pl.lo[k] = std::min(pl.lo[k], p[c][k]), pl.hi[k] = std::max(pl.hi[k], p[c][k]);
            }
            const float a[3] = {p[1][0] - p[0][0], p[1][1] - p[0][1], p[1][2] - p[0][2]}, b[3] = {p[2][0] - p[0][0], p[2][1] - p[0][1], p[2][2] - p[0][2]};
            float nrm[3] = {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
            const float len = std::sqrt(nrm[0] * nrm[0] + nrm[1] * nrm[1] + nrm[2] * nrm[2]);
            for (float& v : nrm) v = len > 1e-12f ? v / len : 0.0f;
            for (int c = 0; c < 3; c++) {
                RtLampGpu::CasterVertex& v = out[3 * (g_vbTris + t) + c];
                std::memcpy(v.p, p[c], sizeof v.p), std::memcpy(v.n, nrm, sizeof v.n);
                const uint32_t i = m.idx[3 * t + c];
                v.uv[0] = m.uv.empty() ? 0.0f : m.uv[2 * i], v.uv[1] = m.uv.empty() ? 0.0f : m.uv[2 * i + 1];
            }
        }
        g_vbTris += mt;
        g_placed.push_back(pl);
    }
    if (tris) g_vb->Unlock();
    g_vbGen = g_gen;
    std::memcpy(g_vbXf, g_xf, sizeof g_xf);
    g_rebuilds++;
    return true;
}
} // namespace

bool DrawIntoTile(IDirect3DDevice9* dev, RtLampGpu::Engine& eng, const float lotPos[3], int tile, float farDist, std::string& err) {
    if (!Active() || !g_haveXf || g_inst.empty() || !eng.CastersReady()) return true;
    if (!Build(dev)) return err = "the objects' triangle list could not be made", false;
    std::vector<RtLampGpu::Engine::CasterDraw> draws;
    for (const Placed& p : g_placed) {
        if (Holds(p.lo, p.hi, lotPos)) continue; // (the lamp's own object)
        if (!draws.empty() && draws.back().tex == p.tex && draws.back().first + draws.back().tris == p.first) draws.back().tris += p.tris;
        else draws.push_back({p.first, p.tris, p.tex});
    }
    if (draws.empty()) return true;
    g_tilesDrawn++;
    return eng.RenderCasters(dev, lotPos, tile, farDist, g_vb, draws, err);
}

std::shared_ptr<const RtLampGpu::BlockerTable> Filter(const std::shared_ptr<const RtLampGpu::BlockerTable>& t) {
    if (!t || !Active() || g_inst.empty()) return t;
    for (const Filtered& f : g_filtered)
        if (f.src == t.get() && f.keep == t && f.gen == g_gen) return f.out ? f.out : t;
    auto copy = std::make_shared<RtLampGpu::BlockerTable>(*t);
    auto& rows = copy->rows;
    bool any = false;
    // the tables are rooms one after another: a header (first record, records), 2 rows a record, then its blockers
    for (size_t h = 0; h < rows.size();) {
        const size_t firstRec = static_cast<size_t>(rows[h].v[0]), recs = static_cast<size_t>(rows[h].v[1]);
        size_t end = std::max(h + 1, firstRec + 2 * recs);
        if (firstRec + 2 * recs > rows.size()) break;
        for (size_t r = 0; r < recs; r++) {
            const auto& c = rows[firstRec + 2 * r];
            auto& ix = rows[firstRec + 2 * r + 1];
            end = std::max(end, static_cast<size_t>(ix.v[0]) + static_cast<size_t>(ix.v[1]) * RtLampGpu::BlockerTable::kBlockerRows);
            const float centre[3] = {c.v[0] + ix.v[2], c.v[1], c.v[2] + ix.v[3]};
            for (const auto& [k, in] : g_inst)
                if (Covers(centre, c.v[3], in.centre) && g_meshes.count(in.mesh)) {
                    if (ix.v[1] != 0.0f) ix.v[1] = 0.0f, any = true; // (its blockers skipped: the mesh casts instead)
                    break;
                }
        }
        h = end;
    }
    Filtered& slot = g_filtered[g_filteredNext++ % g_filtered.size()];
    slot.src = t.get(), slot.keep = t, slot.gen = g_gen, slot.out = any ? std::shared_ptr<const RtLampGpu::BlockerTable>(std::move(copy)) : nullptr;
    return any ? slot.out : t;
}

void OnDeviceLost() {
    if (g_vb) g_vb->Release(), g_vb = nullptr;
    g_vbTris = 0, g_vbGen = 0;
}
} // namespace ObjectCasters
