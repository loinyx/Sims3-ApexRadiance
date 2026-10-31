// Walls shade lamp light near the ground, on the GPU (ground_shade_gpu.h). No game access: the offline harness
// (tools/ground_shade_gpu_test) compiles this file and the shaders of shaders/ground_shade_hlsl.h against GroundShadow.
#include "ground_shade_gpu.h"
#include <d3dcompiler.h>
#include <algorithm>
#include <cstring>

namespace GroundShadeGpu {

namespace {
template <class T> void Rel(T*& p) {
    if (p) p->Release();
    p = nullptr;
}
void Push4(std::vector<float>& v, float a, float b, float c, float d) { v.insert(v.end(), {a, b, c, d}); }
float F(int v) { return static_cast<float>(v); }
} // namespace

const char* EntryOf(ShaderId id) {
    static const char* const kEntries[kShaderCount] = {"FullVS", "ShadePS", "BlurHPS", "BlurVPS", "BytePS", "MinPS", "MaskPS", "ChunkPS", "ChunkBytePS", "MipPS"};
    return id >= 0 && id < kShaderCount ? kEntries[id] : "";
}
bool IsVertex(ShaderId id) { return id == kFullVs; }
unsigned FlagsOf(ShaderId id) {
    // (every fxc optimisation level runs ShadePS past ps_3_0's 32 temporaries); IEEE strictness: the CPU's float order
    return (id == kShadePs ? D3DCOMPILE_SKIP_OPTIMIZATION : D3DCOMPILE_OPTIMIZATION_LEVEL3) | D3DCOMPILE_IEEE_STRICTNESS;
}

bool Prepare(const GroundShadow::Grid& g, const std::vector<GroundShadow::Lamp>& lamps, const std::vector<GroundShadow::Lamp>& others, std::vector<GroundShadow::Wall> walls,
             const std::vector<WallOpenings::Patch>& patches, const std::vector<Canopy::Ellipsoid>& crowns, Inputs& in, std::string& why) {
    using namespace GroundShadow;
    in = {};
    in.grid = g;
    if (g.w <= 0 || g.h <= 0 || g.w > 4095 || g.h > 4096) return why = "grid size", false;
    if (lamps.size() > static_cast<size_t>(kMaxTested)) return why = "over 255 tested lamps", false;
    if (others.size() > static_cast<size_t>(kMaxOthers)) return why = "over 255 other lamps", false;
    // as ShadeJob::Init
    for (Wall& w : walls)
        if (static_cast<uint64_t>(w.p0) + w.pn > patches.size()) w.pn = 0;
    if (g.soft) MarkFreeEnds(walls);
    for (const Wall& w : walls)
        if (w.pn > static_cast<uint32_t>(kMaxPatches)) return why = "over 32 patches on a wall", false;
    std::vector<std::vector<float>> rows;
    // a lamp's rows (5 texels each): its walls in rows of 255, then its crowns in rows of 255, so ShadePS multiplies them in
    // ShadeJob's order (no entry left out); one row when it has none
    const auto lampRow = [&](const Lamp& L, bool tested) {
        const float reach = Reach(L, g.ty), h = std::max(TerrainBakeRule::StampHeight(L.y - g.ty, L.liftable), kMinHead);
        int lb[4] = {0, -1, 0, -1};
        if (!LampBox(g, L, reach, lb)) lb[0] = lb[2] = 0, lb[1] = lb[3] = -1;
        std::vector<float> ew, ec;
        if (tested) {
            const auto clip = [&](int b[4]) {
                b[0] = std::max(b[0], lb[0]), b[1] = std::min(b[1], lb[1]), b[2] = std::max(b[2], lb[2]), b[3] = std::min(b[3], lb[3]);
                return b[0] <= b[1] && b[2] <= b[3];
            };
            for (size_t k = 0; k < walls.size(); k++) {
                int b[4];
                if (WallBox(g, L, reach, walls[k], b) && clip(b)) Push4(ew, F(static_cast<int>(k)), 0, 0, 0), Push4(ew, F(b[0]), F(b[1]), F(b[2]), F(b[3]));
            }
            for (size_t k = 0; k < crowns.size(); k++) {
                int b[4];
                if (CrownBox(g, L, reach, crowns[k], b) && clip(b)) Push4(ec, F(static_cast<int>(k)), 0, 0, 0), Push4(ec, F(b[0]), F(b[1]), F(b[2]), F(b[3]));
            }
            in.entries += static_cast<int>((ew.size() + ec.size()) / 8);
        }
        // walls [w0, w0 + nw) then crowns [c0, c0 + nc) of the lamp's lists (8 floats an entry)
        const auto row = [&](size_t w0, size_t nw, size_t c0, size_t nc, bool first) {
            Push4(in.lamps, L.x, L.y, L.z, L.k);
            Push4(in.lamps, L.rx0, L.rz0, L.rx1, L.rz1);
            Push4(in.lamps, h, reach, F(static_cast<int>(nw)), F(static_cast<int>(nc)));
            Push4(in.lamps, F(lb[0]), F(lb[1]), F(lb[2]), F(lb[3]));
            Push4(in.lamps, first ? 1.0f : 0.0f, 0, 0, 0);
            in.lampRows++;
            if (!tested) return;
            std::vector<float>& r = rows.emplace_back(ew.begin() + static_cast<ptrdiff_t>(8 * w0), ew.begin() + static_cast<ptrdiff_t>(8 * (w0 + nw)));
            r.insert(r.end(), ec.begin() + static_cast<ptrdiff_t>(8 * c0), ec.begin() + static_cast<ptrdiff_t>(8 * (c0 + nc)));
        };
        // one row while both fit; else the wall blocks, the last one with the first crown block, then the
        // other crown blocks
        const size_t B = kMaxOccluders, nw = ew.size() / 8, nc = ec.size() / 8;
        size_t w = 0, c = 0;
        bool first = true;
        for (; nw - w > B; w += B, first = false) row(w, B, 0, 0, first);
        do {
            const size_t take = std::min(B, nc - c);
            row(w, nw - w, c, take, first);
            w = nw, c += take, first = false;
        } while (c < nc);
    };
    for (const Lamp& L : lamps) lampRow(L, true);
    in.tested = in.lampRows;
    if (in.tested > kMaxTested) return why = "over 255 lamp rows (a lamp takes another row per 255 walls or crowns in its reach)", false;
    if (in.entries > kMaxEntries) return why = "over 4000 walls and crowns in the tested lamps' reach", false;
    for (const Lamp& L : others) lampRow(L, false);
    size_t widest = 2;
    for (const auto& r : rows) widest = std::max(widest, r.size() / 4);
    in.entW = static_cast<int>(widest), in.entRows = std::max<int>(1, static_cast<int>(rows.size()));
    in.ent.assign(static_cast<size_t>(in.entW) * in.entRows * 4, 0.0f);
    for (size_t r = 0; r < rows.size(); r++) std::copy(rows[r].begin(), rows[r].end(), in.ent.begin() + static_cast<ptrdiff_t>(r * in.entW * 4));
    // walls, patches and crowns: kWrapRows rows a column block, the blocks side by side (AtW)
    const auto wrap = [](std::vector<float>& v, int cols, size_t n, int& w, int& h) {
        const size_t blocks = std::max<size_t>(1, (n + kWrapRows - 1) / kWrapRows);
        h = static_cast<int>(std::clamp<size_t>(n, 1, kWrapRows)), w = static_cast<int>(static_cast<size_t>(cols) * blocks);
        if (v.empty()) return v.assign(static_cast<size_t>(cols) * 4, 0.0f), true;
        if (blocks == 1) return true;
        if (w > 4096) return false;
        std::vector<float> out(static_cast<size_t>(w) * h * 4, 0.0f);
        for (size_t k = 0; k < n; k++)
            std::copy_n(v.begin() + static_cast<ptrdiff_t>(k * cols * 4), cols * 4,
                        out.begin() + static_cast<ptrdiff_t>(((k % kWrapRows) * w + (k / kWrapRows) * cols) * 4));
        v = std::move(out);
        return true;
    };
    for (const Wall& w : walls) {
        Push4(in.walls, w.x0, w.z0, w.x1, w.z1);
        Push4(in.walls, w.y0, w.y1, static_cast<float>(w.p0), static_cast<float>(w.pn));
        Push4(in.walls, w.free0 ? 1.0f : 0.0f, w.free1 ? 1.0f : 0.0f, w.pass, 0);
    }
    for (const WallOpenings::Patch& p : patches) {
        Push4(in.patches, p.u0, p.u1, p.top0, p.top1);
        Push4(in.patches, p.ou0, p.ou1, p.sill, p.glass);
        Push4(in.patches, p.head[0], p.head[1], p.head[2], p.head[3]);
        Push4(in.patches, p.head[4], p.open ? 1.0f : 0.0f, p.floorSill ? 1.0f : 0.0f, 0);
    }
    for (const Canopy::Ellipsoid& c : crowns) {
        Push4(in.crowns, c.cx, c.cy, c.cz, c.lad);
        Push4(in.crowns, c.rx, c.ry, c.rz, 0);
        Push4(in.crowns, c.c, c.s, 0, 0);
    }
    if (!wrap(in.walls, 3, walls.size(), in.wallW, in.wallRows) || !wrap(in.patches, 4, patches.size(), in.patchW, in.patchRows) ||
        !wrap(in.crowns, 3, crowns.size(), in.crownW, in.crownRows))
        return why = "input texture width", false;
    if (in.lamps.empty()) in.lamps.assign(20, 0.0f), in.lampRows = 1;
    in.others = static_cast<int>(others.size());
    return true;
}

// ---------------------------------------------------------------- the engine
bool Engine::Init(IDirect3DDevice9* dev, void* const* shaders, std::string& err, bool forceArgbChunk) {
    ReleaseAll();
    D3DCAPS9 caps{};
    if (FAILED(dev->GetDeviceCaps(&caps)) || caps.PixelShaderVersion < D3DPS_VERSION(3, 0) || caps.VertexShaderVersion < D3DVS_VERSION(3, 0))
        return err = "no ps_3_0 / vs_3_0", false;
    if (!(caps.DevCaps2 & D3DDEVCAPS2_CAN_STRETCHRECT_FROM_TEXTURES)) return err = "no StretchRect from textures", false; // (the chunk mips)
    IDirect3D9* d3d = nullptr;
    D3DDEVICE_CREATION_PARAMETERS cp{};
    if (FAILED(dev->GetDirect3D(&d3d)) || !d3d || FAILED(dev->GetCreationParameters(&cp))) {
        if (d3d) d3d->Release();
        return err = "no adapter", false;
    }
    const auto can = [&](DWORD usage, D3DFORMAT f) {
        return SUCCEEDED(d3d->CheckDeviceFormat(cp.AdapterOrdinal, cp.DeviceType, D3DFMT_X8R8G8B8, usage, D3DRTYPE_TEXTURE, f));
    };
    const bool formats = can(D3DUSAGE_RENDERTARGET, D3DFMT_G32R32F) && can(D3DUSAGE_RENDERTARGET, D3DFMT_R32F) && can(D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8) &&
                         can(0, D3DFMT_A32B32G32R32F) && can(0, D3DFMT_G32R32F) && can(0, D3DFMT_L8);
    chunkFmt_ = !forceArgbChunk && can(D3DUSAGE_RENDERTARGET, D3DFMT_L8) ? D3DFMT_L8 : D3DFMT_A8R8G8B8;
    d3d->Release();
    if (!formats) return err = "a float or 8-bit render target format is missing", false;
    if (!shaders) return err = "no shaders", false;
    for (int i = 0; i < kShaderCount; i++) {
        if (!shaders[i]) return err = std::string("no ") + EntryOf(static_cast<ShaderId>(i)), false;
        if (IsVertex(static_cast<ShaderId>(i))) {
            vs_ = static_cast<IDirect3DVertexShader9*>(shaders[i]);
            vs_->AddRef();
        } else {
            ps_[i] = static_cast<IDirect3DPixelShader9*>(shaders[i]);
            ps_[i]->AddRef();
        }
    }
    const D3DVERTEXELEMENT9 el[] = {{0, 0, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0}, D3DDECL_END()};
    void* p = nullptr;
    if (FAILED(dev->CreateVertexDeclaration(el, &decl_)) || FAILED(dev->CreateVertexBuffer(24, D3DUSAGE_WRITEONLY, 0, D3DPOOL_MANAGED, &tri_, nullptr)) ||
        FAILED(tri_->Lock(0, 24, &p, 0))) {
        ReleaseAll();
        return err = "vertex buffer", false;
    }
    const float tri[6] = {-1, -1, -1, 3, 3, -1};
    std::memcpy(p, tri, sizeof tri);
    tri_->Unlock();
    ready_ = true;
    return true;
}
void Engine::ReleaseDefault() {
    Rel(shade_), Rel(a_), Rel(b_), Rel(acc_[0]), Rel(acc_[1]), Rel(mip_);
    shadeW_ = shadeH_ = aW_ = aH_ = bW_ = bH_ = mipW_ = mipH_ = 0;
    accW_[0] = accW_[1] = accH_[0] = accH_[1] = 0;
    Rel(saved_); // (a state block is a device object: made again after a reset)
    Rel(qDis_), Rel(qFreq_), Rel(qT0_), Rel(qT1_);
    qBusy_ = false;
}
void Engine::ReleaseAll() {
    ReleaseDefault();
    for (int i = 0; i < 5; i++) Rel(in_[i]), inW_[i] = inH_[i] = 0;
    Rel(room_), roomW_ = roomH_ = 0;
    Rel(vs_);
    for (auto*& p : ps_) Rel(p);
    Rel(decl_), Rel(tri_);
    ready_ = false;
}

bool Engine::Begin(IDirect3DDevice9* dev) {
    if (!saved_ && (FAILED(dev->CreateStateBlock(D3DSBT_ALL, &saved_)) || !saved_)) return saved_ = nullptr, false; // (nothing to restore: nothing is touched)
    saved_->Capture();
    for (DWORD i = 0; i < 4; i++)
        if (FAILED(dev->GetRenderTarget(i, &savedRt_[i]))) savedRt_[i] = nullptr;
    if (!c_.GetDepthStencil || FAILED(c_.GetDepthStencil(dev, &savedDs_))) savedDs_ = nullptr;
    for (DWORD s = 0; s < 16; s++) c_.SetTexture(dev, s, nullptr);
    c_.SetDepthStencil(dev, nullptr);
    dev->SetRenderState(D3DRS_ZENABLE, FALSE);
    dev->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    dev->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    dev->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    dev->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    dev->SetRenderState(D3DRS_FILLMODE, D3DFILL_SOLID);
    dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    dev->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    dev->SetRenderState(D3DRS_FOGENABLE, FALSE);
    dev->SetRenderState(D3DRS_CLIPPLANEENABLE, 0);
    dev->SetRenderState(D3DRS_DITHERENABLE, FALSE);
    dev->SetRenderState(D3DRS_SCISSORTESTENABLE, TRUE);
    dev->SetStreamSourceFreq(0, 1);
    dev->SetVertexDeclaration(decl_);
    dev->SetStreamSource(0, tri_, 0, 8);
    c_.SetVertexShader(dev, vs_);
    return true;
}
void Engine::End(IDirect3DDevice9* dev) {
    for (DWORD s = 0; s < 16; s++) c_.SetTexture(dev, s, nullptr);
    c_.SetRenderTarget(dev, 0, savedRt_[0]);
    for (DWORD i = 1; i < 4; i++) c_.SetRenderTarget(dev, i, savedRt_[i]);
    c_.SetDepthStencil(dev, savedDs_);
    if (saved_) saved_->Apply(); // (after the targets: setting target 0 resets the viewport)
    if (c_.StateRestored) c_.StateRestored();
    for (auto*& s : savedRt_) Rel(s);
    Rel(savedDs_);
}

bool Engine::Ensure(IDirect3DDevice9* dev, IDirect3DTexture9*& t, int& w, int& h, int needW, int needH, D3DFORMAT f, D3DPOOL pool, DWORD usage) {
    if (t && w >= needW && h >= needH) return true;
    Rel(t);
    const int nw = std::max(needW, w), nh = std::max(needH, h); // (grown, never shrunk: one size for the session's lots)
    if (FAILED(dev->CreateTexture(static_cast<UINT>(nw), static_cast<UINT>(nh), 1, usage, f, pool, &t, nullptr)) || !t) {
        t = nullptr, w = h = 0;
        return false;
    }
    w = nw, h = nh;
    return true;
}
bool Engine::Upload(IDirect3DDevice9* dev, IDirect3DTexture9*& t, int& w, int& h, const std::vector<float>& rgba, int cols, int rows) {
    if (static_cast<size_t>(cols) * rows * 4 > rgba.size() || !Ensure(dev, t, w, h, cols, rows, D3DFMT_A32B32G32R32F, D3DPOOL_MANAGED, 0)) return false;
    D3DLOCKED_RECT lr{};
    const RECT rc{0, 0, cols, rows};
    if (FAILED(t->LockRect(0, &lr, &rc, 0)) || !lr.pBits) return false;
    for (int y = 0; y < rows; y++) std::memcpy(static_cast<BYTE*>(lr.pBits) + static_cast<size_t>(y) * lr.Pitch, &rgba[static_cast<size_t>(y) * cols * 4], static_cast<size_t>(cols) * 16);
    t->UnlockRect(0);
    return true;
}
bool Engine::Target(IDirect3DDevice9* dev, IDirect3DTexture9* t, UINT level) {
    IDirect3DSurface9* s = nullptr;
    if (!t || FAILED(t->GetSurfaceLevel(level, &s)) || !s) return false;
    D3DSURFACE_DESC d{};
    s->GetDesc(&d);
    const bool ok = SUCCEEDED(c_.SetRenderTarget(dev, 0, s));
    s->Release();
    for (DWORD i = 1; i < 4; i++) c_.SetRenderTarget(dev, i, nullptr);
    const D3DVIEWPORT9 vp{0, 0, d.Width, d.Height, 0.0f, 1.0f};
    c_.SetViewport(dev, &vp);
    return ok;
}
// The pass over texels [x0, x1) x [y0, y1) of the target (scissor)
void Engine::Draw(IDirect3DDevice9* dev, ShaderId ps, int x0, int y0, int x1, int y1) {
    const RECT rc{x0, y0, x1, y1};
    dev->SetScissorRect(&rc);
    c_.SetPixelShader(dev, ps_[ps]);
    c_.DrawPrimitive(dev, D3DPT_TRIANGLELIST, 0, 1);
}
void Engine::Point(IDirect3DDevice9* dev, DWORD s, IDirect3DBaseTexture9* t, bool mips) {
    c_.SetTexture(dev, s, t);
    dev->SetSamplerState(s, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(s, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    dev->SetSamplerState(s, D3DSAMP_MIPFILTER, mips ? D3DTEXF_POINT : D3DTEXF_NONE);
    dev->SetSamplerState(s, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(s, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
    dev->SetSamplerState(s, D3DSAMP_SRGBTEXTURE, FALSE);
    dev->SetSamplerState(s, D3DSAMP_MAXMIPLEVEL, 0);
    dev->SetSamplerState(s, D3DSAMP_MIPMAPLODBIAS, 0);
}
void Engine::Poll() {
    if (!qBusy_) return;
    BOOL disjoint = TRUE;
    UINT64 t0 = 0, t1 = 0, f = 0;
    if (qDis_->GetData(&disjoint, sizeof disjoint, 0) != S_OK || qT0_->GetData(&t0, sizeof t0, 0) != S_OK || qT1_->GetData(&t1, sizeof t1, 0) != S_OK ||
        qFreq_->GetData(&f, sizeof f, 0) != S_OK)
        return; // (not yet: read at a later call)
    qBusy_ = false;
    if (!disjoint && f && t1 >= t0) gpuMs_ = 1000.0 * static_cast<double>(t1 - t0) / static_cast<double>(f);
}

bool Engine::BuildLot(IDirect3DDevice9* dev, const Inputs& in, IDirect3DTexture9*& ext, std::string& err, IDirect3DQuery9** shades) {
    if (!ready_) return err = "not ready", false;
    const int W = in.grid.w, H = in.grid.h;
    if (!Upload(dev, in_[0], inW_[0], inH_[0], in.lamps, 5, in.lampRows) || !Upload(dev, in_[1], inW_[1], inH_[1], in.ent, in.entW, in.entRows) ||
        !Upload(dev, in_[2], inW_[2], inH_[2], in.walls, in.wallW, in.wallRows) || !Upload(dev, in_[3], inW_[3], inH_[3], in.patches, in.patchW, in.patchRows) ||
        !Upload(dev, in_[4], inW_[4], inH_[4], in.crowns, in.crownW, in.crownRows))
        return err = "input textures", false;
    if (!Ensure(dev, shade_, shadeW_, shadeH_, W, H, D3DFMT_G32R32F, D3DPOOL_DEFAULT, D3DUSAGE_RENDERTARGET) ||
        !Ensure(dev, a_, aW_, aH_, W, H, D3DFMT_R32F, D3DPOOL_DEFAULT, D3DUSAGE_RENDERTARGET) ||
        !Ensure(dev, b_, bW_, bH_, W, H, D3DFMT_R32F, D3DPOOL_DEFAULT, D3DUSAGE_RENDERTARGET))
        return err = "scratch targets", false;
    if (ext) { // (its size: (w + 1) x h)
        D3DSURFACE_DESC d{};
        if (FAILED(ext->GetLevelDesc(0, &d)) || d.Width != static_cast<UINT>(W + 1) || d.Height != static_cast<UINT>(H) || d.Pool != D3DPOOL_DEFAULT) Rel(ext);
    }
    if (!ext && (FAILED(dev->CreateTexture(static_cast<UINT>(W + 1), static_cast<UINT>(H), 1, D3DUSAGE_RENDERTARGET, D3DFMT_G32R32F, D3DPOOL_DEFAULT, &ext, nullptr)) || !ext))
        return ext = nullptr, err = "ext target", false;
    // timed when no earlier timing is still in flight
    Poll();
    bool timed = false;
    if (!qBusy_) {
        if (!qDis_) dev->CreateQuery(D3DQUERYTYPE_TIMESTAMPDISJOINT, &qDis_), dev->CreateQuery(D3DQUERYTYPE_TIMESTAMPFREQ, &qFreq_), dev->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &qT0_),
            dev->CreateQuery(D3DQUERYTYPE_TIMESTAMP, &qT1_);
        if (qDis_ && qFreq_ && qT0_ && qT1_) qDis_->Issue(D3DISSUE_BEGIN), qT0_->Issue(D3DISSUE_END), timed = true;
    }
    const GroundShadow::Grid& g = in.grid;
    const float c[32] = {g.m0, g.m2, g.m8, g.m10, g.tx, g.tz, g.ty, g.soft ? 1.0f : 0.0f,
                         F(in.tested), F(in.others), Penumbra::g_on.load(std::memory_order_relaxed) ? Penumbra::Radius() : -1.0f, Canopy::Extinction(),
                         1.0f / inW_[0], 1.0f / inH_[0], 1.0f / inW_[1], 1.0f / inH_[1],
                         1.0f / inW_[2], 1.0f / inH_[2], 1.0f / inW_[3], 1.0f / inH_[3],
                         1.0f / inW_[4], 1.0f / inH_[4], 1.0f / shadeW_, 1.0f / shadeH_,
                         F(W), F(H), 0, 0,
                         1.0f / shadeW_, 1.0f / shadeH_, 1.0f, 1.0f};
    c_.SetPsConst(dev, 0, c, 8);
    for (DWORD s = 0; s < 5; s++) Point(dev, s, in_[s]);
    Target(dev, shade_);
    Draw(dev, kShadePs, 0, 0, W, H);
    // Finish: [1 2 1] by rows then columns, twice; the last column pass to bytes (with the stamp weight) into ext. A pass's
    // target is set with nothing bound, then its source (no texture is read while it is the target)
    const auto pass = [&](IDirect3DTexture9* target, IDirect3DTexture9* t, int tw, int th) {
        Point(dev, 5, nullptr);
        Target(dev, target);
        const float inv[4] = {1.0f / inW_[4], 1.0f / inH_[4], 1.0f / tw, 1.0f / th};
        c_.SetPsConst(dev, 5, inv, 1);
        Point(dev, 5, t);
    };
    pass(a_, shade_, shadeW_, shadeH_), Draw(dev, kBlurHPs, 0, 0, W, H);
    pass(b_, a_, aW_, aH_), Draw(dev, kBlurVPs, 0, 0, W, H);
    pass(a_, b_, bW_, bH_), Draw(dev, kBlurHPs, 0, 0, W, H);
    pass(ext, a_, aW_, aH_), Point(dev, 6, shade_), Draw(dev, kBytePs, 0, 0, W, H);
    Point(dev, 6, nullptr);
    // the grid's lowest byte into texel (w, 0): 4 x 4 minima until one texel is left
    IDirect3DTexture9* cur = ext;
    int cw = W + 1, ch = H, sw = W, sh = H;
    bool toA = true;
    do {
        const float sz[4] = {F(sw), F(sh), 0, 0};
        c_.SetPsConst(dev, 6, sz, 1);
        IDirect3DTexture9* dst = toA ? a_ : b_;
        const int dw = (sw + 3) / 4, dh = (sh + 3) / 4;
        pass(dst, cur, cw, ch), Draw(dev, kMinPs, 0, 0, dw, dh);
        cur = dst, cw = toA ? aW_ : bW_, ch = toA ? aH_ : bH_, sw = dw, sh = dh, toA = !toA;
    } while (sw > 4 || sh > 4);
    const float last[4] = {F(sw), F(sh), F(W), 0};
    c_.SetPsConst(dev, 6, last, 1);
    pass(ext, cur, cw, ch), Draw(dev, kMinPs, W, 0, W + 1, 1);
    // the same texel again, kept only when under 255 and not written, counted by the occlusion query
    if (shades && (*shades || SUCCEEDED(dev->CreateQuery(D3DQUERYTYPE_OCCLUSION, shades))) && *shades) {
        const float kill[4] = {F(sw), F(sh), F(W), 1.0f};
        c_.SetPsConst(dev, 6, kill, 1);
        dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0);
        (*shades)->Issue(D3DISSUE_BEGIN);
        Draw(dev, kMinPs, W, 0, W + 1, 1);
        (*shades)->Issue(D3DISSUE_END);
        dev->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    }
    Point(dev, 5, nullptr);
    if (timed) qT1_->Issue(D3DISSUE_END), qFreq_->Issue(D3DISSUE_END), qDis_->Issue(D3DISSUE_END), qBusy_ = true;
    return true;
}

bool Engine::UploadExt(IDirect3DDevice9* dev, const GroundShadow::Grid& g, const std::vector<uint8_t>& v, const std::vector<float>& w, IDirect3DTexture9*& ext) {
    const size_t n = static_cast<size_t>(std::max(g.w, 0)) * static_cast<size_t>(std::max(g.h, 0));
    Rel(ext);
    if (!n || v.size() != n || w.size() != n) return false;
    if (FAILED(dev->CreateTexture(static_cast<UINT>(g.w + 1), static_cast<UINT>(g.h), 1, 0, D3DFMT_G32R32F, D3DPOOL_MANAGED, &ext, nullptr)) || !ext) return ext = nullptr, false;
    D3DLOCKED_RECT lr{};
    if (FAILED(ext->LockRect(0, &lr, nullptr, 0)) || !lr.pBits) return Rel(ext), false;
    uint8_t lo = 255;
    for (uint8_t b : v) lo = std::min(lo, b);
    for (int j = 0; j < g.h; j++) {
        float* row = reinterpret_cast<float*>(static_cast<BYTE*>(lr.pBits) + static_cast<size_t>(j) * lr.Pitch);
        for (int i = 0; i < g.w; i++) row[2 * i] = v[static_cast<size_t>(j) * g.w + i], row[2 * i + 1] = w[static_cast<size_t>(j) * g.w + i];
        row[2 * g.w] = j == 0 ? lo : 255.0f, row[2 * g.w + 1] = 0.0f;
    }
    ext->UnlockRect(0);
    return true;
}

bool Engine::BuildMask(IDirect3DDevice9* dev, IDirect3DTexture9* ext, int extW, int extH, int ox, int oz, int w, int h, const std::vector<uint8_t>& room, IDirect3DTexture9*& mask,
                       std::string& err) {
    if (!ready_ || !ext || w <= 0 || h <= 0 || room.size() != static_cast<size_t>(w) * h || ox < 0 || oz < 0 || ox + w > extW - 1 || oz + h > extH)
        return err = "mask inputs", false;
    if (!Ensure(dev, room_, roomW_, roomH_, w, h, D3DFMT_L8, D3DPOOL_MANAGED, 0)) return err = "room texture", false;
    D3DLOCKED_RECT lr{};
    const RECT rc{0, 0, w, h};
    if (FAILED(room_->LockRect(0, &lr, &rc, 0)) || !lr.pBits) return err = "room texture", false;
    for (int y = 0; y < h; y++) std::memcpy(static_cast<BYTE*>(lr.pBits) + static_cast<size_t>(y) * lr.Pitch, &room[static_cast<size_t>(y) * w], static_cast<size_t>(w));
    room_->UnlockRect(0);
    if (mask) {
        D3DSURFACE_DESC d{};
        if (FAILED(mask->GetLevelDesc(0, &d)) || d.Width != static_cast<UINT>(w) || d.Height != static_cast<UINT>(h) || d.Pool != D3DPOOL_DEFAULT) Rel(mask);
    }
    if (!mask && (FAILED(dev->CreateTexture(static_cast<UINT>(w), static_cast<UINT>(h), 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &mask, nullptr)) || !mask))
        return mask = nullptr, err = "mask target", false;
    const float c[12] = {0, 0, 1.0f / extW, 1.0f / extH, F(w), F(h), F(ox), F(oz), 1, 1, 1.0f / roomW_, 1.0f / roomH_};
    c_.SetPsConst(dev, 5, c, 3);
    Point(dev, 5, nullptr);
    Target(dev, mask);
    Point(dev, 5, ext), Point(dev, 7, room_);
    Draw(dev, kMaskPs, 0, 0, w, h);
    Point(dev, 5, nullptr), Point(dev, 7, nullptr);
    return true;
}

bool Engine::BuildChunk(IDirect3DDevice9* dev, const std::vector<ChunkSrc>& srcs, float wx0, float wz0, GroundShadow::TexRect r0, IDirect3DTexture9*& tex, std::string& err) {
    using namespace GroundShadow;
    constexpr int N = kChunkTexels;
    if (!ready_) return err = "not ready", false;
    if (!tex) {
        if (FAILED(dev->CreateTexture(N, N, kChunkLevels, D3DUSAGE_RENDERTARGET, chunkFmt_, D3DPOOL_DEFAULT, &tex, nullptr)) || !tex) return tex = nullptr, err = "chunk texture", false;
        r0 = {0, 0, N - 1, N - 1};
    }
    if (tex->GetLevelCount() < static_cast<DWORD>(kChunkLevels)) return err = "chunk levels", false;
    r0.i0 = std::max(r0.i0, 0), r0.j0 = std::max(r0.j0, 0), r0.i1 = std::min(r0.i1, N - 1), r0.j1 = std::min(r0.j1, N - 1);
    if (r0.Empty()) return true;
    if (!Ensure(dev, acc_[0], accW_[0], accH_[0], N, N, D3DFMT_G32R32F, D3DPOOL_DEFAULT, D3DUSAGE_RENDERTARGET) ||
        !Ensure(dev, acc_[1], accW_[1], accH_[1], N, N, D3DFMT_G32R32F, D3DPOOL_DEFAULT, D3DUSAGE_RENDERTARGET) ||
        !Ensure(dev, mip_, mipW_, mipH_, N / 2, N / 2, chunkFmt_, D3DPOOL_DEFAULT, D3DUSAGE_RENDERTARGET))
        return err = "chunk scratch", false;
    // the lots over r0 (the others add nothing there), in passes of kLotsPerPass
    std::vector<const ChunkSrc*> lots;
    for (const ChunkSrc& s : srcs)
        if (s.ext && s.grid.w > 0 && s.grid.h > 0 && !(s.box.i1 < r0.i0 || s.box.i0 > r0.i1 || s.box.j1 < r0.j0 || s.box.j0 > r0.j1)) lots.push_back(&s);
    const int x0 = r0.i0, y0 = r0.j0, x1 = r0.i1 + 1, y1 = r0.j1 + 1;
    int cur = 0;
    for (size_t first = 0, pass = 0; pass == 0 || first < lots.size(); first += kLotsPerPass, pass++) {
        const int n = static_cast<int>(std::min<size_t>(kLotsPerPass, lots.size() - std::min(first, lots.size())));
        const int out = pass ? 1 - cur : cur;
        Point(dev, 8, nullptr);
        Target(dev, acc_[out]);
        float lc[kLotsPerPass * 16] = {};
        for (int k = 0; k < kLotsPerPass; k++) {
            const ChunkSrc* s = k < n ? lots[first + k] : nullptr;
            Point(dev, 10 + k, s ? s->ext : nullptr);
            if (!s) continue;
            const Grid& g = s->grid;
            const float v[16] = {g.m0, g.m2, g.m8, g.m10, g.tx, g.tz, F(g.w), F(g.h), F(s->box.i0), F(s->box.i1), F(s->box.j0), F(s->box.j1), 1.0f / (g.w + 1), 1.0f / g.h, 0, 0};
            std::memcpy(&lc[k * 16], v, sizeof v);
        }
        c_.SetPsConst(dev, 16, lc, kLotsPerPass * 4);
        const float cc[8] = {wx0, wz0, F(n), pass ? 1.0f : 0.0f, 1.0f / accW_[0], 1.0f / accH_[0], 0, 0};
        c_.SetPsConst(dev, 8, cc, 2);
        Point(dev, 8, pass ? acc_[cur] : nullptr);
        Draw(dev, kChunkPs, x0, y0, x1, y1);
        cur = out;
    }
    for (int k = 0; k < kLotsPerPass; k++) c_.SetTexture(dev, 10 + k, nullptr);
    Point(dev, 8, nullptr);
    Target(dev, tex, 0);
    Point(dev, 8, acc_[cur]);
    Draw(dev, kChunkBytePs, x0, y0, x1, y1);
    Point(dev, 8, nullptr);
    // each mip over the texels above r0: drawn from the level below into the scratch, then copied into the level
    IDirect3DSurface9* scratch = nullptr;
    if (FAILED(mip_->GetSurfaceLevel(0, &scratch)) || !scratch) return err = "mip scratch", false;
    bool ok = true;
    for (int l = 1; ok && l < kChunkLevels; l++) {
        const TexRect rl = LevelRect(r0, l);
        const float s = 1.0f / static_cast<float>(N >> (l - 1));
        const float cc[4] = {s, s, F(l - 1), 0};
        c_.SetPsConst(dev, 9, cc, 1);
        Target(dev, mip_);
        Point(dev, 9, tex, true);
        Draw(dev, kMipPs, rl.i0, rl.j0, rl.i1 + 1, rl.j1 + 1);
        Point(dev, 9, nullptr);
        Target(dev, acc_[0]); // (neither the scratch nor the chunk is the target while copied)
        IDirect3DSurface9* level = nullptr;
        const RECT rc{rl.i0, rl.j0, rl.i1 + 1, rl.j1 + 1};
        ok = SUCCEEDED(tex->GetSurfaceLevel(static_cast<UINT>(l), &level)) && level && SUCCEEDED(dev->StretchRect(scratch, &rc, level, &rc, D3DTEXF_POINT));
        Rel(level);
    }
    scratch->Release();
    if (!ok) err = "mip copy";
    return ok;
}

} // namespace GroundShadeGpu
