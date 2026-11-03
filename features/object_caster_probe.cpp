// Object caster probe: see object_caster_probe.h
#include "object_caster_probe.h"
#include "apex_log.h"
#include "outdoor_delta.h"
#include "recorder.h"
#include <windows.h>
#include <array>
#include <cstdint>
#include <format>
#include <vector>

namespace ObjectCasterProbe {
namespace {
constexpr int kFamilies = 24, kPerFrame = 128, kMeshSlots = 8192;
constexpr DWORD kEveryMs = 10000;
struct Family {
    IDirect3DVertexShader9* vs = nullptr;
    uint32_t hash = 0;
    int posReg = -1;         // the input register declared POSITION0 (-1: none found)
    bool decodeW = false;    // the code takes rcp of that register's .w (position = v.xyz / v.w)
    int worldK = -1;
    BYTE posType = 0xFF, uvType = 0xFF;
    WORD posOff = 0, stream = 0;
    UINT stride = 0;
    D3DPOOL pool = D3DPOOL_FORCE_DWORD;
    int lockRO = -1;         // -1 not tried, 0 failed, 1 worked (MANAGED only)
    uint32_t draws = 0, onLot = 0, alphaTest = 0, blend = 0, meshes = 0;
    uint64_t tris = 0, bytes = 0;
    DWORD alphaRef = 0, alphaFunc = 0;
    D3DFORMAT s0 = D3DFMT_UNKNOWN; // the texture at sampler 0 of its last alpha-tested draw
};
std::array<Family, kFamilies> g_fam;
int g_famN = 0, g_frameLeft = kPerFrame;
std::array<uint64_t, kMeshSlots> g_mesh{}; // open addressing: distinct (vertex buffer, range) seen in this window
DWORD g_windowAt = 0;
uint32_t g_sampled = 0, g_skipped = 0;

// The POSITION0 input register and whether the shader divides by its .w (rcp vN.w), from the shader's tokens
void ReadCode(Family& f) {
    UINT size = 0;
    if (FAILED(f.vs->GetFunction(nullptr, &size)) || size < 8 || size > (1u << 20)) return;
    std::vector<DWORD> t(size / 4);
    if (FAILED(f.vs->GetFunction(t.data(), &size))) return;
    uint32_t h = 2166136261u;
    for (DWORD w : t) h = (h ^ w) * 16777619u;
    f.hash = h;
    for (size_t i = 1; i < t.size();) {
        const DWORD tok = t[i], op = tok & 0xFFFF;
        if (op == 0xFFFF) break;
        if (op == 0xFFFE) { // comment
            i += 1 + ((tok >> 16) & 0x7FFF);
            continue;
        }
        const size_t len = (tok >> 24) & 0xF;
        if (op == 0x1F && i + 2 < t.size()) { // dcl usage, register
            const DWORD usage = t[i + 1] & 0x1F, index = (t[i + 1] >> 16) & 0xF, reg = t[i + 2];
            const DWORD type = ((reg >> 28) & 7) | ((reg >> 8) & 0x18);
            if (usage == D3DDECLUSAGE_POSITION && index == 0 && type == D3DSPR_INPUT) f.posReg = static_cast<int>(reg & 0x7FF);
        } else if (op == 6 && i + 2 < t.size() && f.posReg >= 0) { // rcp dst, src
            const DWORD src = t[i + 2], type = ((src >> 28) & 7) | ((src >> 8) & 0x18);
            if (type == D3DSPR_INPUT && static_cast<int>(src & 0x7FF) == f.posReg && ((src >> 16) & 0xFF) == 0xFF) f.decodeW = true;
        }
        i += 1 + len;
    }
}
Family* FamilyOf(IDirect3DVertexShader9* vs) {
    for (int i = 0; i < g_famN; i++)
        if (g_fam[i].vs == vs) return &g_fam[i];
    if (g_famN >= kFamilies) return nullptr;
    Family& f = g_fam[g_famN++];
    f = Family{};
    f.vs = vs;
    ReadCode(f);
    return &f;
}
bool NewMesh(uint64_t key) {
    for (uint32_t i = static_cast<uint32_t>((key * 0x9E3779B97F4A7C15ull) >> 51), n = 0; n < kMeshSlots; i = (i + 1) & (kMeshSlots - 1), n++) {
        if (g_mesh[i] == key) return false;
        if (!g_mesh[i]) return g_mesh[i] = key, true;
    }
    return false; // full: counted as seen
}
const char* TypeName(BYTE t) {
    switch (t) {
    case D3DDECLTYPE_FLOAT2: return "float2";
    case D3DDECLTYPE_FLOAT3: return "float3";
    case D3DDECLTYPE_FLOAT4: return "float4";
    case D3DDECLTYPE_SHORT2: return "short2";
    case D3DDECLTYPE_SHORT4: return "short4";
    case D3DDECLTYPE_SHORT2N: return "short2n";
    case D3DDECLTYPE_SHORT4N: return "short4n";
    case D3DDECLTYPE_UBYTE4: return "ubyte4";
    case D3DDECLTYPE_UBYTE4N: return "ubyte4n";
    case D3DDECLTYPE_FLOAT16_2: return "half2";
    case D3DDECLTYPE_FLOAT16_4: return "half4";
    case 0xFF: return "none";
    default: return "other";
    }
}
int TypeBytes(BYTE t) {
    switch (t) {
    case D3DDECLTYPE_FLOAT2: case D3DDECLTYPE_SHORT4: case D3DDECLTYPE_SHORT4N: case D3DDECLTYPE_FLOAT16_4: return 8;
    case D3DDECLTYPE_FLOAT3: return 12;
    case D3DDECLTYPE_FLOAT4: return 16;
    case 0xFF: return 0;
    default: return 4;
    }
}
const char* PoolName(D3DPOOL p) {
    return p == D3DPOOL_MANAGED ? "MANAGED" : p == D3DPOOL_DEFAULT ? "DEFAULT" : p == D3DPOOL_SYSTEMMEM ? "SYSTEMMEM" : "?";
}
} // namespace

bool Wanted() { return Recorder::Active() && OutdoorDelta::RoomLampPhysical(); }

void OnObjectDraw(IDirect3DDevice9* dev, IDirect3DVertexShader9* vs, int worldK, bool onLot, INT baseVertex, UINT minV, UINT numV, UINT start, UINT prims) {
    if (!dev || !vs) return;
    if (g_frameLeft <= 0) return void(g_skipped++);
    g_frameLeft--;
    Family* f = FamilyOf(vs);
    if (!f) return void(g_skipped++);
    g_sampled++;
    f->draws++, f->tris += prims, f->onLot += onLot ? 1 : 0;
    if (worldK >= 0) f->worldK = worldK;
    IDirect3DVertexDeclaration9* decl = nullptr;
    if (SUCCEEDED(dev->GetVertexDeclaration(&decl)) && decl) {
        D3DVERTEXELEMENT9 e[MAXD3DDECLLENGTH + 1];
        UINT n = MAXD3DDECLLENGTH + 1;
        if (SUCCEEDED(decl->GetDeclaration(e, &n)))
            for (UINT i = 0; i < n && e[i].Stream != 0xFF; i++) {
                if (e[i].Usage == D3DDECLUSAGE_POSITION && e[i].UsageIndex == 0) f->posType = e[i].Type, f->posOff = e[i].Offset, f->stream = e[i].Stream;
                if (e[i].Usage == D3DDECLUSAGE_TEXCOORD && e[i].UsageIndex == 0) f->uvType = e[i].Type;
            }
        decl->Release();
    }
    IDirect3DVertexBuffer9* vb = nullptr;
    UINT off = 0, stride = 0;
    if (f->posType != 0xFF && SUCCEEDED(dev->GetStreamSource(f->stream, &vb, &off, &stride)) && vb) {
        D3DVERTEXBUFFER_DESC d{};
        if (SUCCEEDED(vb->GetDesc(&d))) {
            f->pool = d.Pool, f->stride = stride;
            if (d.Pool == D3DPOOL_MANAGED && f->lockRO < 0) { // once per family: a read-only lock of a managed buffer reads its system copy
                void* p = nullptr;
                f->lockRO = SUCCEEDED(vb->Lock(0, 0, &p, D3DLOCK_READONLY)) && p ? 1 : 0;
                if (f->lockRO) vb->Unlock();
            }
        }
        const uint64_t key = (static_cast<uint64_t>(reinterpret_cast<uintptr_t>(vb)) << 32) ^ (static_cast<uint64_t>(baseVertex + minV) << 12) ^ start ^ off;
        if (NewMesh(key | 1)) f->meshes++, f->bytes += static_cast<uint64_t>(numV) * (TypeBytes(f->posType) + TypeBytes(f->uvType)) + static_cast<uint64_t>(prims) * 6;
        vb->Release();
    }
    DWORD at = FALSE, ab = FALSE;
    dev->GetRenderState(D3DRS_ALPHATESTENABLE, &at);
    dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &ab);
    if (ab) f->blend++;
    if (at) {
        f->alphaTest++;
        dev->GetRenderState(D3DRS_ALPHAREF, &f->alphaRef);
        dev->GetRenderState(D3DRS_ALPHAFUNC, &f->alphaFunc);
        IDirect3DBaseTexture9* t = nullptr;
        if (SUCCEEDED(dev->GetTexture(0, &t)) && t) {
            D3DSURFACE_DESC sd{};
            if (t->GetType() == D3DRTYPE_TEXTURE && SUCCEEDED(static_cast<IDirect3DTexture9*>(t)->GetLevelDesc(0, &sd))) f->s0 = sd.Format;
            t->Release();
        }
    }
}

void OnPresent() {
    g_frameLeft = kPerFrame;
    const DWORD now = GetTickCount();
    if (!g_windowAt) g_windowAt = now;
    if (now - g_windowAt < kEveryMs) return;
    g_windowAt = now;
    if (g_sampled) {
        uint32_t meshes = 0, onLot = 0;
        uint64_t bytes = 0;
        for (int i = 0; i < g_famN; i++) meshes += g_fam[i].meshes, bytes += g_fam[i].bytes, onLot += g_fam[i].onLot;
        LOG_INFO(std::format("[ObjectCasterProbe] 10 s: {} object draws sampled ({} on a lot, {} over the {} a frame budget), {} vertex shaders, {} distinct meshes, "
                             "{} KB of positions + uv + indices",
                             g_sampled, onLot, g_skipped, kPerFrame, g_famN, meshes, bytes / 1024));
        for (int i = 0; i < g_famN; i++) {
            const Family& f = g_fam[i];
            if (!f.draws) continue;
            LOG_INFO(std::format("[ObjectCasterProbe]   vs {:08X}: draws {} ({} on a lot), tris {}, position {}@{} stream {} stride {}, uv {}, decode {}, world rows c{}, "
                                 "vb {} read-only lock {}, alpha test {} (ref {} func {}, s0 format {:#x}), blend {}, meshes {} ({} KB)",
                                 f.hash, f.draws, f.onLot, f.tris, TypeName(f.posType), f.posOff, f.stream, f.stride, TypeName(f.uvType),
                                 f.posReg < 0 ? "no POSITION0" : f.decodeW ? "xyz / w" : "xyz", f.worldK, PoolName(f.pool),
                                 f.lockRO < 0 ? "n/a" : f.lockRO ? "ok" : "failed", f.alphaTest, f.alphaRef, f.alphaFunc, static_cast<uint32_t>(f.s0), f.blend,
                                 f.meshes, f.bytes / 1024));
        }
    }
    // a new window: the families stay (their code was read), their counts start again
    for (int i = 0; i < g_famN; i++) {
        Family& f = g_fam[i];
        f.draws = f.onLot = f.alphaTest = f.blend = f.meshes = 0, f.tris = f.bytes = 0;
    }
    g_mesh.fill(0);
    g_sampled = g_skipped = 0;
}
} // namespace ObjectCasterProbe
