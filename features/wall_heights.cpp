// Where the game draws its walls: see wall_heights.h.
//
// The game's wall vertex shader (shaders/shader_ids.h kWallVs, VS_1D21FA00 in the F7 captures) reads its position from
// the stream as stored / 256 in lot space (x, z; y its full height, the cutaway height in w) and turns it to world space
// with VS c8..c10. Each wall mesh draw (vertex buffer, first vertex, count, lot rows) is read once with a read-only lock,
// at most a few per frame, and again after kRescanMs; meshes not drawn for kForgetMs are forgotten. Vertices with the same
// world xz (to the centimetre) make one vertical edge: its lowest and highest y are the wall's foot and top there.
#include "wall_heights.h"
#include "d3d9_hooks.h"
#include "shader_ids.h"
#include "apex_log.h"
#include <d3d9.h>
#include <windows.h>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <format>
#include <map>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace WallHeights {
namespace {

constexpr const char* kHookName = "WallHeights";
constexpr DWORD kRescanMs = 4000, kForgetMs = 15000;
constexpr int kScansPerFrame = 3;
constexpr UINT kMaxVertices = 65536;

struct Edge {
    float x, z, lo, hi;
};
struct Mesh {
    std::vector<Edge> edges;
    DWORD scanned = 0, seen = 0;
};

std::mutex g_mx;                               // g_meshes, g_grid, g_pending, g_requeue
std::unordered_map<uint64_t, Mesh> g_meshes;   // a mesh draw's key -> its edges
std::unordered_map<int64_t, std::vector<std::pair<uint64_t, uint32_t>>> g_grid; // 1 m cell -> (mesh, edge)
bool g_gridDirty = false;
struct Pending {
    RoomKey room;
    float ax, az, bx, bz, ext, base;
};
std::vector<Pending> g_pending;      // rooms solved before their walls were measured (bounded)
std::vector<RoomKey> g_requeue;      // pending rooms now measured away from their base
std::atomic<bool> g_newMeasure{false}; // a mesh was read with edges that differ from before: pending rooms are checked

// render thread only
bool g_installed = false;
std::unordered_map<IDirect3DVertexShader9*, bool> g_isWallVs;
int g_scansThisFrame = 0;
DWORD g_lastPurge = 0;
std::atomic<long> g_scans{0}, g_unreadable{0}, g_requeued{0}, g_found{0}, g_missing{0};

int64_t Cell(float x, float z) {
    return (static_cast<int64_t>(static_cast<int32_t>(std::floor(x))) << 32) ^ static_cast<int64_t>(static_cast<uint32_t>(static_cast<int32_t>(std::floor(z))));
}

// (2.15.3) the last answer: asked on every draw, and the game draws many in a row with one vertex shader
IDirect3DVertexShader9* g_lastVs = nullptr;
bool g_lastWall = false;

bool IsWallVs(IDirect3DVertexShader9* vs) {
    if (!vs) return false;
    if (vs == g_lastVs) return g_lastWall;
    const auto it = g_isWallVs.find(vs);
    if (it != g_isWallVs.end()) {
        g_lastVs = vs, g_lastWall = it->second;
        return it->second;
    }
    bool wall = false;
    UINT size = 0;
    if (SUCCEEDED(vs->GetFunction(nullptr, &size)) && size == kWallVs.size) {
        std::vector<BYTE> code(size);
        if (SUCCEEDED(vs->GetFunction(code.data(), &size))) wall = IsShader(kWallVs, code.data(), size);
    }
    if (g_isWallVs.size() > 4096) g_isWallVs.clear(); // shaders released and their addresses reused
    g_isWallVs[vs] = wall;
    g_lastVs = vs, g_lastWall = wall;
    return wall;
}

// The positions of a draw's vertices (POSITION 0, as stored), read-only; empty when not readable
bool ReadRawPositions(IDirect3DDevice9* dev, UINT first, UINT count, std::vector<float>& out, IDirect3DVertexBuffer9*& vbOut, UINT& offOut) {
    out.clear();
    vbOut = nullptr;
    if (count == 0 || count > kMaxVertices) return false;
    IDirect3DVertexDeclaration9* decl = nullptr;
    if (FAILED(dev->GetVertexDeclaration(&decl)) || !decl) return false;
    D3DVERTEXELEMENT9 el[MAXD3DDECLLENGTH + 1] = {};
    UINT ne = MAXD3DDECLLENGTH + 1;
    const bool okDecl = SUCCEEDED(decl->GetDeclaration(el, &ne));
    decl->Release();
    if (!okDecl) return false;
    const D3DVERTEXELEMENT9* pos = nullptr;
    for (UINT i = 0; i < ne && el[i].Stream != 0xFF; i++)
        if (el[i].Usage == D3DDECLUSAGE_POSITION && el[i].UsageIndex == 0) pos = &el[i];
    if (!pos) return false;
    IDirect3DVertexBuffer9* vb = nullptr;
    UINT off = 0, stride = 0;
    if (FAILED(dev->GetStreamSource(pos->Stream, &vb, &off, &stride)) || !vb) return false;
    vbOut = vb;
    offOut = off;
    D3DVERTEXBUFFER_DESC d{};
    vb->GetDesc(&d);
    const UINT begin = off + first * stride, bytes = count * stride;
    void* p = nullptr;
    bool ok = false;
    bool readable = stride >= 8 && d.Pool != D3DPOOL_DEFAULT && begin + bytes <= d.Size;
    if (readable) {
        // the memory first (07/10, players' Runtime Error): a throw while the game's buffer is locked would leave it locked
        try {
            out.reserve(static_cast<size_t>(count) * 3);
        } catch (...) {
            readable = false;
        }
    }
    if (readable && SUCCEEDED(vb->Lock(begin, bytes, &p, D3DLOCK_READONLY)) && p) {
        const BYTE* b = static_cast<const BYTE*>(p);
        ok = true;
        for (UINT i = 0; i < count && ok; i++) {
            const BYTE* v = b + static_cast<size_t>(i) * stride + pos->Offset;
            float f[3] = {};
            switch (pos->Type) {
                case D3DDECLTYPE_FLOAT3:
                case D3DDECLTYPE_FLOAT4: std::memcpy(f, v, 12); break;
                case D3DDECLTYPE_SHORT4: for (int k = 0; k < 3; k++) f[k] = reinterpret_cast<const int16_t*>(v)[k]; break;
                case D3DDECLTYPE_UBYTE4: for (int k = 0; k < 3; k++) f[k] = v[k]; break;
                default: ok = false; continue;
            }
            out.insert(out.end(), f, f + 3);
        }
        vb->Unlock();
    }
    return ok && !out.empty();
}

void Scan(IDirect3DDevice9* dev, uint64_t key, UINT first, UINT count, const float (&m)[3][4]) {
    std::vector<float> raw;
    IDirect3DVertexBuffer9* vb = nullptr;
    UINT off = 0;
    const bool ok = ReadRawPositions(dev, first, count, raw, vb, off);
    if (vb) vb->Release();
    g_scans.fetch_add(1, std::memory_order_relaxed);
    const DWORD now = GetTickCount();
    if (!ok) {
        g_unreadable.fetch_add(1, std::memory_order_relaxed);
        std::lock_guard<std::mutex> lk(g_mx);
        Mesh& mesh = g_meshes[key];
        mesh.scanned = now | 1;
        mesh.seen = now | 1;
        return;
    }
    std::unordered_map<int64_t, Edge> byXz;
    for (size_t i = 0; i + 2 < raw.size(); i += 3) {
        const float l[3] = {raw[i] / 256.0f, raw[i + 1] / 256.0f, raw[i + 2] / 256.0f};
        float w[3];
        for (int r = 0; r < 3; r++) w[r] = m[r][0] * l[0] + m[r][1] * l[1] + m[r][2] * l[2] + m[r][3];
        if (!std::isfinite(w[0]) || !std::isfinite(w[1]) || !std::isfinite(w[2])) continue;
        const int64_t k = (static_cast<int64_t>(std::lround(w[0] * 100.0f)) << 32) ^ static_cast<int64_t>(static_cast<uint32_t>(std::lround(w[2] * 100.0f)));
        auto [it, fresh] = byXz.try_emplace(k, Edge{w[0], w[2], w[1], w[1]});
        if (!fresh) {
            it->second.lo = std::min(it->second.lo, w[1]);
            it->second.hi = std::max(it->second.hi, w[1]);
        }
    }
    std::vector<Edge> edges;
    edges.reserve(byXz.size());
    for (const auto& [k, e] : byXz)
        if (e.hi - e.lo > 0.2f) edges.push_back(e); // vertical edges only (a wall's foot and top)
    std::sort(edges.begin(), edges.end(), [](const Edge& a, const Edge& b) { return a.x != b.x ? a.x < b.x : a.z < b.z; });
    std::lock_guard<std::mutex> lk(g_mx);
    Mesh& mesh = g_meshes[key];
    bool changed = mesh.edges.size() != edges.size();
    for (size_t i = 0; !changed && i < edges.size(); i++)
        changed = std::fabs(mesh.edges[i].lo - edges[i].lo) > 0.01f || std::fabs(mesh.edges[i].hi - edges[i].hi) > 0.01f;
    mesh.edges = std::move(edges);
    mesh.scanned = now | 1;
    mesh.seen = now | 1;
    if (changed) {
        g_gridDirty = true;
        g_newMeasure.store(true, std::memory_order_relaxed);
    }
}

void RebuildGridLocked() {
    if (!g_gridDirty) return;
    g_gridDirty = false;
    g_grid.clear();
    for (const auto& [key, mesh] : g_meshes)
        for (uint32_t i = 0; i < mesh.edges.size(); i++) g_grid[Cell(mesh.edges[i].x, mesh.edges[i].z)].push_back({key, i});
}

bool DrawnFootPass(float ax, float az, float bx, float bz, float ext, float base, float& foot);
// the wall's own run first; then along its line (collinear walls drawn as one mesh have their vertices at the run's ends)
bool DrawnFootLocked(float ax, float az, float bx, float bz, float ext, float base, float& foot) {
    RebuildGridLocked();
    return DrawnFootPass(ax, az, bx, bz, ext, base, foot) || DrawnFootPass(ax, az, bx, bz, ext + 8.0f, base, foot);
}
bool DrawnFootPass(float ax, float az, float bx, float bz, float ext, float base, float& foot) {
    const float ux = bx - ax, uz = bz - az, L = std::sqrt(ux * ux + uz * uz);
    if (L < 0.01f) return false;
    const float pad = ext + 0.35f;
    struct Span {
        float lo, hi;
        int n;
    };
    std::vector<Span> spans;
    for (int cx = static_cast<int>(std::floor(std::min(ax, bx) - pad)); cx <= static_cast<int>(std::floor(std::max(ax, bx) + pad)); cx++)
        for (int cz = static_cast<int>(std::floor(std::min(az, bz) - pad)); cz <= static_cast<int>(std::floor(std::max(az, bz) + pad)); cz++) {
            const auto it = g_grid.find((static_cast<int64_t>(cx) << 32) ^ static_cast<int64_t>(static_cast<uint32_t>(cz)));
            if (it == g_grid.end()) continue;
            for (const auto& [key, idx] : it->second) {
                const auto mit = g_meshes.find(key);
                if (mit == g_meshes.end() || idx >= mit->second.edges.size()) continue;
                const Edge& e = mit->second.edges[idx];
                const float dx = e.x - ax, dz = e.z - az;
                const float along = (dx * ux + dz * uz) / L, across = std::fabs(dx * uz - dz * ux) / L;
                if (across > 0.3f || along < -ext || along > L + ext) continue;
                bool merged = false;
                for (Span& s : spans)
                    if (std::fabs(s.lo - e.lo) < 0.02f && std::fabs(s.hi - e.hi) < 0.02f) {
                        s.n++;
                        merged = true;
                        break;
                    }
                if (!merged) spans.push_back({e.lo, e.hi, 1});
            }
        }
    // The stories share the line, stacked: the wall drawn for this one has the highest foot from 3.2 m under its base to
    // 0.3 m over it (F7: story 1 drawn from 61.30 for a base of 63.325, story 2 from 64.30 for 66.325, the
    // foundation's side from 60.55 for 60.325; the largest overlap rule took the story above for the foundation's walls)
    const Span* best = nullptr;
    for (const Span& s : spans) {
        if (s.hi - s.lo < 0.5f || s.lo < base - 3.2f || s.lo > base + 0.3f) continue;
        if (!best || s.lo > best->lo + 0.02f || (std::fabs(s.lo - best->lo) <= 0.02f && s.n > best->n)) best = &s;
    }
    if (!best) return false;
    foot = best->lo;
    return true;
}

void OnWallDraw(IDirect3DDevice9* dev, UINT first, UINT count) {
    float m[3][4];
    if (FAILED(dev->GetVertexShaderConstantF(8, &m[0][0], 3))) return;
    IDirect3DVertexBuffer9* vb = nullptr;
    UINT off = 0, stride = 0;
    if (FAILED(dev->GetStreamSource(0, &vb, &off, &stride)) || !vb) return;
    const uintptr_t vbp = reinterpret_cast<uintptr_t>(vb);
    vb->Release();
    uint32_t tx = 0, tz = 0;
    std::memcpy(&tx, &m[0][3], 4);
    std::memcpy(&tz, &m[2][3], 4);
    const uint64_t key = (static_cast<uint64_t>(vbp) << 32) ^ (static_cast<uint64_t>(off + first * stride) * 2654435761u) ^ (static_cast<uint64_t>(count) << 16) ^ tx ^ (static_cast<uint64_t>(tz) << 7);
    const DWORD now = GetTickCount();
    {
        std::lock_guard<std::mutex> lk(g_mx);
        const auto it = g_meshes.find(key);
        if (it != g_meshes.end()) {
            it->second.seen = now | 1;
            if (now - it->second.scanned < kRescanMs) return;
        }
    }
    if (g_scansThisFrame >= kScansPerFrame) return;
    g_scansThisFrame++;
    Scan(dev, key, first, count, m);
}

void OnPresent() {
    g_scansThisFrame = 0;
    const DWORD now = GetTickCount();
    if (now - g_lastPurge > 1000) {
        g_lastPurge = now;
        std::lock_guard<std::mutex> lk(g_mx);
        for (auto it = g_meshes.begin(); it != g_meshes.end();)
            if (now - it->second.seen > kForgetMs) {
                it = g_meshes.erase(it);
                g_gridDirty = true;
            } else
                ++it;
    }
    if (g_newMeasure.exchange(false, std::memory_order_relaxed)) {
        std::lock_guard<std::mutex> lk(g_mx);
        for (auto it = g_pending.begin(); it != g_pending.end();) {
            float foot = 0;
            if (DrawnFootLocked(it->ax, it->az, it->bx, it->bz, it->ext, it->base, foot)) {
                if (std::fabs(foot - it->base) > 0.05f) {
                    bool dup = false;
                    for (const RoomKey& r : g_requeue) dup = dup || (r.tracker == it->room.tracker && r.level == it->room.level && r.id == it->room.id);
                    if (!dup) g_requeue.push_back(it->room);
                }
                it = g_pending.erase(it);
            } else
                ++it;
        }
    }
}

} // namespace

void Install() {
    if (g_installed) return;
    using namespace D3D9Hooks;
    ObserveReissuedDraws(kHookName); // runs after Night Lighting: a wall it replaces is seen only in the re-issued draw
    RegisterDrawIndexedPrimitive(kHookName, [](DeviceContext& ctx, D3DPRIMITIVETYPE, INT bvi, UINT minV, UINT numV, UINT, UINT) {
        // (14/10: the bound shader as the registry follows it, no GetVertexShader + Release per draw; the device holds it)
        if (IDirect3DVertexShader9* vs = BoundVertexShader(ctx.device))
            if (IsWallVs(vs)) OnWallDraw(ctx.device, static_cast<UINT>(bvi + static_cast<INT>(minV)), numV);
        return HookAction::Continue;
    }, Priority::Last);
    RegisterPresent(kHookName, [](DeviceContext&, const RECT*, const RECT*, HWND, const RGNDATA*) {
        OnPresent();
        return HookAction::Continue;
    }, Priority::Normal);
    g_installed = true;
    LOG_INFO("[WallHeights] On: the game's wall meshes are measured where they are drawn");
}

void Uninstall() {
    if (!g_installed) return;
    D3D9Hooks::UnregisterAll(kHookName);
    g_installed = false;
    g_isWallVs.clear();
    g_lastVs = nullptr;
    std::lock_guard<std::mutex> lk(g_mx);
    g_meshes.clear();
    g_grid.clear();
    g_pending.clear();
    g_requeue.clear();
}

bool DrawnFoot(float ax, float az, float bx, float bz, float ext, float base, float& foot) {
    std::lock_guard<std::mutex> lk(g_mx);
    const bool ok = DrawnFootLocked(ax, az, bx, bz, ext, base, foot);
    (ok ? g_found : g_missing).fetch_add(1, std::memory_order_relaxed);
    return ok;
}

void NotePending(uintptr_t tracker, int level, int id, float ax, float az, float bx, float bz, float ext, float base) {
    if (!tracker) return;
    std::lock_guard<std::mutex> lk(g_mx);
    for (const Pending& p : g_pending)
        if (p.room.tracker == tracker && p.room.level == level && p.room.id == id && std::fabs(p.ax - ax) < 0.01f && std::fabs(p.az - az) < 0.01f) return;
    if (g_pending.size() >= 8192) g_pending.erase(g_pending.begin(), g_pending.begin() + 1024);
    g_pending.push_back({{tracker, level, id}, ax, az, bx, bz, ext, base});
}

std::vector<RoomKey> TakeRequeue() {
    std::lock_guard<std::mutex> lk(g_mx);
    std::vector<RoomKey> out;
    out.swap(g_requeue);
    g_requeued.fetch_add(static_cast<long>(out.size()), std::memory_order_relaxed);
    return out;
}

std::string Status() {
    size_t meshes = 0, edges = 0, pending = 0;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        meshes = g_meshes.size();
        for (const auto& [k, m] : g_meshes) edges += m.edges.size();
        pending = g_pending.size();
    }
    return std::format("{} | wall meshes measured {} ({} vertical edges; {} reads, {} not readable) | wall pieces lit at their drawn foot {}, not measured yet {} | rooms waiting for "
                       "their walls {}, solved again once measured {}",
                       g_installed ? "on" : "off", meshes, edges, g_scans.load(), g_unreadable.load(), g_found.load(), g_missing.load(), pending, g_requeued.load());
}

} // namespace WallHeights
