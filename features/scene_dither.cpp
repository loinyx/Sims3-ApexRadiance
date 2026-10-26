// Banding Fix (scene dither)
// The game draws its 3D scene straight into an 8-bit back buffer, so a smooth light gradient (a lamp's pool of light on
// a wall, the fall-off of a room light map) is rounded to 256 steps per channel, and in the dark range each step is a
// visible ring. This adds a fixed, invisible grain to every scene pixel shader's colour before the rounding (triangular
// noise of +-1 step at the default Strength, from the R2 pattern of the pixel position, within half a step of black and
// white), so the rings become a fine grain. The alpha (the bloom mask) is never touched.
//
// How:
//  - ps_3_0 (ShaderPatches::AddDither): the pixel position is vPos. ps_2_0 / ps_2_x (AddDither2, 30/09 evening: in game
//    more than half of the scene draws, the walls among them, were ps_2_x): no vPos, so the position comes from a free
//    texture coordinate k that a copy of the paired vertex shader fills with the clip position (AddScreenPosVs), and
//    the pixel copy is ps_2_x (the grain does not fit in the slots of some ps_2_0).
//  - Copies are made at a shader's first draw in the 3D scene (05/10: before, every shader the game created got one at
//    once, shadow, reflection, UI and bloom ones too, and DXVK keeps every shader for the whole session; the game's memory
//    grew ~0.3 GB more in 30 min with Apex). At creation (last in the chain) only the old copies of a reused address go.
//  - At each draw (last in the chain, after every observer): when render target 0 is the back buffer and the depth
//    test is on and used (the 3D scene; the UI draws with it off, or on with ZFUNC ALWAYS and no depth write, and reads
//    the back buffer back many times per frame, where a dither would feed on itself), the copies are bound for the draw
//    with the amount constant, then the game's shaders and the constant's previous value are put back. Additive and
//    multiply blends get half the grain with a decorrelated phase (they add to a target that already has its grain).
//  - The mouse-pick pass draws into its own 16x16 target, shadows and reflections into their own targets: untouched.
//  - Glow sprites (a light capture of a car headlight: "texld r0, t1, s0; mul r0, r0, v0", additive, no depth
//    write, a 128x128 DXT5 glow in alpha): DXT keeps 8 levels per 4x4 block, so the magnified glow shows rings that no grain
//    removes. Such draws get, at s0, a smoothed 16-bit copy of the texture (sprite_smooth_logic.h), made at its first use
//    and kept by the game texture itself (SetPrivateData), also while the High-precision scene stands in for the grain.
// Offline checks (30/09, Shaders_Win32.precomp): ps_3_0 4907 / 4907 and ps_2_0 2996 / 3104 patched, vertex shaders
// vs_2_0 4035 / 4338, every copy accepted by native D3D9; a grey of 20.40 / 100.70 levels comes out as 20.355 / 100.657
// on average with the grain (20 / 101 without), for ps_3_0 and for a ps_2_0 + vs_2_0 pair alike.

#include "patch_base.h"
#include "apex_version.h"
#include "apex_log.h"
#include "d3d9_bootstrap.h"
#include "d3d9_hooks.h"
#include "render_callbacks.h"
#include "shader_patches.h"
#include "sprite_smooth_logic.h"
#include "scene_dither.h"
#include "scene_target.h"
#include "f10_study.h"
#include "shader_lookup_cache.h"
#include "imgui.h"
#include "ui/violet_theme.h"
#include "ui/widgets.h"
#include "build_flavor.h"
#include "perf_toggles.h"
#include <d3d9.h>
#include <algorithm>
#include <atomic>
#include <cstring>
#include <format>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace {

constexpr const char* kHookName = "SceneDither";
constexpr auto kAfterEveryone = static_cast<D3D9Hooks::Priority>(1000); // after Last: every observer has seen the draw
constexpr size_t kMaxShaderTokens = 16384;                              // 64 KB
constexpr int kUsualTexcoord = 7;                                       // the texture coordinate most ps_2_x copies use

std::atomic<bool> g_on{false};
thread_local bool t_own = false; // our own creations pass through
std::mutex g_lock;
struct Copy {
    IDirect3DPixelShader9* ps = nullptr; // the dithered copy, null = none
    int amountReg = -1;                 // its constant holding the amount (set around the draw)
    int texcoord = -1;                  // ps_2_x copies: the texture coordinate with the clip position (-1: ps_3_0, vPos)
    DWORD version = 0;                  // the game shader's version token
};
std::unordered_map<IDirect3DPixelShader9*, Copy> g_copies; // game pixel shader -> its copy
std::unordered_map<IDirect3DPixelShader9*, bool> g_sprites; // game pixel shader -> texture x vertex colour (a glow sprite)
// game vertex shader -> its copies writing the clip position to TEXCOORDk (null = cannot), key = pointer * 8 + k
std::unordered_map<uint64_t, IDirect3DVertexShader9*> g_vsCopies;
ShaderLookupCache<Copy> g_pixelLookup;
ShaderLookupCache<IDirect3DVertexShader9*> g_vertexLookup;
// Render-thread front caches checked without the lock (14/10: two mutexes on every scene draw): direct-mapped, valid
// while their generation equals g_copyGen, which every change of the copies bumps (under g_lock, after the change).
std::atomic<uint32_t> g_copyGen{1};
struct PsFront {
    IDirect3DPixelShader9* ps = nullptr;
    uint32_t gen = 0;
    Copy copy;
};
struct VsFront {
    uint64_t key = 0;
    uint32_t gen = 0;
    IDirect3DVertexShader9* copy = nullptr;
};
constexpr size_t kFront = 64;
PsFront g_psFront[kFront];
VsFront g_vsFront[kFront];
inline size_t FrontSlot(uint64_t k) { return static_cast<size_t>((static_cast<uint32_t>(k ^ (k >> 32)) * 2654435761u) >> 26) & (kFront - 1); }
float g_strength = 1.0f;                  // the grain in 8-bit steps (triangular, peak): 1 = +-1 step
bool g_showCovered = false;               // Developer: a coarse grain where the fix applies (not saved)
constexpr float kShowCoveredSteps = 24.0f;
IDirect3DSurface9* g_backBuffer = nullptr; // identity only (render thread)
// render target 0, followed through SetRenderTarget as PostScene and Picture do (05/10: GetRenderTarget + Release on every
// draw before); read again at the frame boundary after a Reset (which sets it without a SetRenderTarget call)
IDirect3DSurface9* g_rt0 = nullptr;

// statistics (Developer page, dev log)
std::atomic<unsigned> g_made{0}, g_madeVs{0}, g_refused[7] = {};
// blend classes of the dithered draws (Developer page): the grain is halved for additive and multiply blends (BlendOf)
enum Blend { kOpaque, kAlpha, kAdditive, kMultiply, kOtherBlend, kBlends };
struct FrameCount {
    unsigned dithered3 = 0, dithered2 = 0; // 3D scene draws with the grain: ps_3_0, ps_2_x
    unsigned ps2NoPair = 0;                // ps_2_x with a copy whose vertex shader could not carry the position
    unsigned ps2Refused = 0, ps3Refused = 0, other = 0;
    unsigned noDepth = 0;                  // depth on with ZFUNC ALWAYS and no depth write: the UI's passes, left alone
    unsigned noShader = 0;                 // fixed-function scene draws (no pixel shader): no grain
    unsigned blend[kBlends] = {};          // dithered draws by blend class
    unsigned sprites = 0, spritesAlone = 0; // glow sprite draws with the smoothed texture / without (refused texture, sRGB read)
    unsigned jittered = 0, jitterRefused = 0, jitterNoShader = 0; // temporal AA: scene draws moved / whose vertex shader was refused / fixed-function
};
FrameCount g_frame, g_last;
unsigned long long g_lastLog = 0;
int g_logs = 0;

// vertex copy key: the game shader, the texture coordinate k carrying the clip position (-1 none) and the temporal jitter
uint64_t VsKey(IDirect3DVertexShader9* vs, int k, bool jitter) {
    return (reinterpret_cast<uint64_t>(vs) << 5) | (jitter ? 16u : 0u) | static_cast<uint64_t>(k < 0 ? 8 : (k & 7));
}

// ---- the shared scene-draw binder (30/09): the Banding Fix's pixel copies and the temporal anti-aliasing's jittered vertex
// copies (Edge Smoothing, SMAA T2x) bound together in one final draw hook, as two features cannot both skip and redraw ----
std::atomic<int> g_hookUsers{0};  // Banding Fix and temporal AA (the hooks are registered while > 0)
bool g_jitterOn = false;          // render thread: this frame's scene draws are moved
std::atomic<bool> g_jitterWanted{false}; // temporal AA is on (vertex copies with the jitter made at creation)
float g_jitter[4] = {};           // clip-space offset (c252.xy)
constexpr int kJitterConst = 252; // no game vertex shader uses a constant above c241 (Shaders_Win32.precomp, 30/09)

// Tokens up to and with the end token (no length is given to Create*Shader), 0 when unreadable
size_t CodeLength(const DWORD* fn) {
    __try {
        if ((fn[0] >> 17) != 0x7FFFu) return 0; // 0xFFFF.... pixel, 0xFFFE.... vertex
        for (size_t i = 1; i < kMaxShaderTokens; i++)
            if (fn[i] == 0x0000FFFFu) return i + 1;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return 0;
}

std::vector<DWORD> ReadCode(const DWORD* fn) {
    const size_t n = CodeLength(fn);
    return n ? std::vector<DWORD>(fn, fn + n) : std::vector<DWORD>{};
}

template <typename Shader> std::vector<DWORD> ReadCode(Shader* s) {
    UINT size = 0;
    if (FAILED(s->GetFunction(nullptr, &size)) || size < 8 || size % 4) return {};
    std::vector<DWORD> t(size / 4);
    if (FAILED(s->GetFunction(t.data(), &size))) return {};
    return t;
}

// The dithered copy of a game pixel shader (Copy::ps null when it cannot have one)
Copy MakeCopy(IDirect3DDevice9* dev, std::vector<DWORD> t) {
    Copy c;
    if (t.empty()) {
        g_refused[static_cast<int>(ShaderPatches::DitherResult::Unreadable)]++;
        return c;
    }
    c.version = t[0];
    const bool ps2 = t[0] == 0xFFFF0200u || t[0] == 0xFFFF0201u;
    const ShaderPatches::DitherResult r = ps2 ? ShaderPatches::AddDither2(t, &c.amountReg, &c.texcoord) : ShaderPatches::AddDither(t, &c.amountReg);
    if (r != ShaderPatches::DitherResult::Ok) {
        g_refused[static_cast<int>(r)]++;
        c.texcoord = -1;
        return c;
    }
    t_own = true;
    const HRESULT hr = D3D9Hooks::CallOriginalCreatePixelShader(dev, t.data(), &c.ps);
    t_own = false;
    if (FAILED(hr) || !c.ps) {
        c.ps = nullptr;
        g_refused[static_cast<int>(ShaderPatches::DitherResult::Unreadable)]++;
        return c;
    }
    g_made++;
    return c;
}

// The copy of a game vertex shader that also writes the clip position to TEXCOORDk, or null
IDirect3DVertexShader9* MakeVsCopy(IDirect3DDevice9* dev, std::vector<DWORD> t, int k, bool jitter) {
    if (t.empty()) return nullptr;
    if (jitter && ShaderPatches::AddJitterVs(t, kJitterConst) != ShaderPatches::JitterResult::Ok) return nullptr;
    if (k >= 0 && !ShaderPatches::AddScreenPosVs(t, k)) return nullptr; // after the jitter: TEXCOORDk gets the moved position
    IDirect3DVertexShader9* copy = nullptr;
    t_own = true;
    const HRESULT hr = D3D9Hooks::CallOriginalCreateVertexShader(dev, t.data(), &copy);
    t_own = false;
    if (FAILED(hr)) return nullptr;
    g_madeVs++;
    return copy;
}

void Remember(IDirect3DPixelShader9* game, const Copy& copy) {
    std::lock_guard<std::mutex> lock(g_lock);
    g_pixelLookup.Clear();
    auto [it, fresh] = g_copies.try_emplace(game, copy);
    if (!fresh) { // an address the game reused for a new shader
        if (it->second.ps) it->second.ps->Release();
        it->second = copy;
    }
    g_copyGen.fetch_add(1, std::memory_order_release);
}

void RememberVs(uint64_t key, IDirect3DVertexShader9* copy) {
    std::lock_guard<std::mutex> lock(g_lock);
    g_vertexLookup.Clear();
    auto [it, fresh] = g_vsCopies.try_emplace(key, copy);
    if (!fresh) {
        if (it->second) it->second->Release();
        it->second = copy;
    }
    g_copyGen.fetch_add(1, std::memory_order_release);
}

// A new game pixel shader at an address: its old copy goes
void Forget(IDirect3DPixelShader9* ps) {
    std::lock_guard<std::mutex> lock(g_lock);
    g_pixelLookup.Clear();
    g_copyGen.fetch_add(1, std::memory_order_release); // (also with no copy: a front entry may hold this address)
    g_sprites.erase(ps);
    const auto it = g_copies.find(ps);
    if (it == g_copies.end()) return;
    if (it->second.ps) it->second.ps->Release();
    g_copies.erase(it);
}

// A new game vertex shader at an address: its old copies (every k) go
void ForgetVs(IDirect3DVertexShader9* vs) {
    std::lock_guard<std::mutex> lock(g_lock);
    g_vertexLookup.Clear();
    g_copyGen.fetch_add(1, std::memory_order_release);
    for (int j = 0; j < 2; j++)
        for (int k = -1; k < 8; k++) {
            const auto it = g_vsCopies.find(VsKey(vs, k, j != 0));
            if (it == g_vsCopies.end()) continue;
            if (it->second) it->second->Release();
            g_vsCopies.erase(it);
        }
}

void ReleaseCopies() {
    std::lock_guard<std::mutex> lock(g_lock);
    g_pixelLookup.Clear(); g_vertexLookup.Clear();
    g_copyGen.fetch_add(1, std::memory_order_release);
    for (auto& [game, copy] : g_copies)
        if (copy.ps) copy.ps->Release();
    g_copies.clear();
    g_sprites.clear();
    for (auto& [key, copy] : g_vsCopies)
        if (copy) copy->Release();
    g_vsCopies.clear();
}

// Render thread: the copy for the bound pixel shader (made now when the shader is older than the feature)
Copy CopyOf(IDirect3DDevice9* dev, IDirect3DPixelShader9* ps) {
    const uint32_t gen = g_copyGen.load(std::memory_order_acquire); // (read before the lookup: a change after it makes the entry stale)
    PsFront& front = g_psFront[FrontSlot(reinterpret_cast<std::uintptr_t>(ps))];
    if (front.ps == ps && front.gen == gen && PerfToggles::SceneDitherFrontCache.load(std::memory_order_relaxed)) return front.copy;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        Copy cached;
        const auto key = reinterpret_cast<std::uintptr_t>(ps);
        if (g_pixelLookup.Find(key, cached)) {
            front.ps = ps, front.gen = gen, front.copy = cached;
            return cached;
        }
        const auto it = g_copies.find(ps);
        if (it != g_copies.end()) {
            g_pixelLookup.Store(key, it->second);
            front.ps = ps, front.gen = gen, front.copy = it->second;
            return it->second;
        }
    }
    const Copy copy = MakeCopy(dev, ReadCode(ps));
    Remember(ps, copy);
    return copy;
}

// Render thread: the vertex copy writing TEXCOORDk (k -1: none) and / or moved by the jitter (made now when missing)
IDirect3DVertexShader9* VsCopyOf(IDirect3DDevice9* dev, IDirect3DVertexShader9* vs, int k, bool jitter) {
    const uint64_t key = VsKey(vs, k, jitter);
    const uint32_t gen = g_copyGen.load(std::memory_order_acquire);
    VsFront& front = g_vsFront[FrontSlot(key)];
    if (front.key == key && front.gen == gen && PerfToggles::SceneDitherFrontCache.load(std::memory_order_relaxed)) return front.copy;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        IDirect3DVertexShader9* cached = nullptr;
        if (g_vertexLookup.Find(key, cached)) {
            front.key = key, front.gen = gen, front.copy = cached;
            return cached;
        }
        const auto it = g_vsCopies.find(key);
        if (it != g_vsCopies.end()) {
            g_vertexLookup.Store(key, it->second);
            front.key = key, front.gen = gen, front.copy = it->second;
            return it->second;
        }
    }
    IDirect3DVertexShader9* copy = MakeVsCopy(dev, ReadCode(vs), k, jitter);
    RememberVs(key, copy);
    return copy;
}

// The blend class of a draw, read only for draws that get the grain (no SetRenderState hook follows the blend states)
Blend BlendOf(IDirect3DDevice9* dev) {
    DWORD on = FALSE, src = D3DBLEND_ONE, dst = D3DBLEND_ZERO, op = D3DBLENDOP_ADD;
    dev->GetRenderState(D3DRS_ALPHABLENDENABLE, &on);
    if (!on) return kOpaque;
    dev->GetRenderState(D3DRS_SRCBLEND, &src);
    dev->GetRenderState(D3DRS_DESTBLEND, &dst);
    dev->GetRenderState(D3DRS_BLENDOP, &op);
    if (op != D3DBLENDOP_ADD) return kOtherBlend;
    if (src == D3DBLEND_SRCALPHA && dst == D3DBLEND_INVSRCALPHA) return kAlpha;
    if (src == D3DBLEND_DESTCOLOR || src == D3DBLEND_ZERO) return kMultiply; // decals: DESTCOLOR / SRCCOLOR, DESTCOLOR / ONE
    if (dst == D3DBLEND_ONE) return kAdditive;
    return kOtherBlend;
}

// ---- glow sprites: a smoothed copy of their DXT texture (sprite_smooth_logic.h) ----
namespace SS = SpriteSmoothLogic;
// The copy hangs on the game texture as private data (released with it, never met by another texture at its address);
// a texture that cannot have one is marked with its refusal so it is tried once
constexpr GUID kSmoothedGuid = {0x21dae682, 0x4e81, 0x474c, {0x9a, 0x24, 0xbc, 0x7b, 0x3a, 0xb5, 0x4a, 0x41}};
constexpr GUID kRefusedGuid = {0x321eb54b, 0x87f3, 0x4690, {0xa3, 0x86, 0xa8, 0x63, 0xc0, 0x8c, 0x8b, 0x35}};
std::atomic<unsigned> g_spriteMade{0}, g_spriteRefused[static_cast<int>(SS::Refusal::Count)] = {};
std::atomic<unsigned long long> g_spriteBytes{0};
int g_filter16 = -1; // A16B16G16R16 can be filtered (CheckDeviceFormat D3DUSAGE_QUERY_FILTER), -1 not asked yet
struct SpriteFront {
    IDirect3DPixelShader9* ps = nullptr;
    uint32_t gen = 0;
    bool sprite = false;
};
SpriteFront g_spriteFront[kFront];

bool IsSprite(IDirect3DPixelShader9* ps) {
    const uint32_t gen = g_copyGen.load(std::memory_order_acquire);
    SpriteFront& front = g_spriteFront[FrontSlot(reinterpret_cast<std::uintptr_t>(ps))];
    if (front.ps == ps && front.gen == gen) return front.sprite;
    bool sprite = false, known = false;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        const auto it = g_sprites.find(ps);
        if (it != g_sprites.end()) sprite = it->second, known = true;
    }
    if (!known) {
        const std::vector<DWORD> t = ReadCode(ps);
        sprite = SS::IsTintedSprite(t.data(), t.size());
        std::lock_guard<std::mutex> lock(g_lock);
        g_sprites.emplace(ps, sprite);
    }
    front.ps = ps, front.gen = gen, front.sprite = sprite;
    return sprite;
}

bool Filterable16(IDirect3DDevice9* dev) {
    if (g_filter16 >= 0) return g_filter16 != 0;
    IDirect3D9* d3d = nullptr;
    D3DDEVICE_CREATION_PARAMETERS cp{};
    D3DDISPLAYMODE mode{};
    g_filter16 = 0;
    if (FAILED(dev->GetDirect3D(&d3d)) || !d3d) return false;
    if (SUCCEEDED(dev->GetCreationParameters(&cp)) && SUCCEEDED(dev->GetDisplayMode(0, &mode)))
        g_filter16 = d3d->CheckDeviceFormat(cp.AdapterOrdinal, cp.DeviceType, mode.Format, D3DUSAGE_QUERY_FILTER, D3DRTYPE_TEXTURE, D3DFMT_A16B16G16R16) == D3D_OK;
    d3d->Release();
    return g_filter16 != 0;
}

// Decodes, smooths and uploads the copy (render thread, once per texture): same size and mip count, 16 bits a channel
SS::Refusal MakeSmoothed(IDirect3DDevice9* dev, IDirect3DTexture9* tex, IDirect3DTexture9** out) {
    D3DSURFACE_DESC d{};
    if (FAILED(tex->GetLevelDesc(0, &d))) return SS::Refusal::Format;
    if (d.Format != D3DFMT_DXT1 && d.Format != D3DFMT_DXT3 && d.Format != D3DFMT_DXT5) return SS::Refusal::Format;
    if (d.Pool != D3DPOOL_MANAGED && d.Pool != D3DPOOL_SYSTEMMEM) return SS::Refusal::Pool; // a DEFAULT texture cannot be read back
    if (d.Width > SS::kMaxSide || d.Height > SS::kMaxSide || d.Width < SS::kMinSide || d.Height < SS::kMinSide || d.Width % 4 || d.Height % 4)
        return SS::Refusal::Size;
    const int w = static_cast<int>(d.Width), h = static_cast<int>(d.Height);
    std::vector<float> img(static_cast<size_t>(w) * h * 4);
    D3DLOCKED_RECT lr{};
    if (FAILED(tex->LockRect(0, &lr, nullptr, D3DLOCK_READONLY)) || !lr.pBits) return SS::Refusal::Lock;
    SS::Decode(d.Format, static_cast<const uint8_t*>(lr.pBits), lr.Pitch, w, h, img.data());
    tex->UnlockRect(0);
    std::vector<float> level = img;
    const float edges = SS::Smooth(level.data(), w, h);
    if (!SS::SmoothEnough(img.data(), level.data(), w, h, edges)) return SS::Refusal::Detail;
    const bool wide = Filterable16(dev);
    const DWORD levels = tex->GetLevelCount();
    IDirect3DTexture9* copy = nullptr;
    if (FAILED(dev->CreateTexture(d.Width, d.Height, levels, 0, wide ? D3DFMT_A16B16G16R16 : D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &copy, nullptr)) || !copy)
        return SS::Refusal::Create;
    unsigned long long bytes = 0;
    int lw = w, lh = h;
    for (DWORD i = 0; i < levels; i++) {
        if (i) { // the next level: 2x2 averages of this one
            std::vector<float> next(static_cast<size_t>(std::max(1, lw / 2)) * std::max(1, lh / 2) * 4);
            SS::Half(level.data(), lw, lh, next.data());
            level.swap(next);
            lw = std::max(1, lw / 2), lh = std::max(1, lh / 2);
        }
        if (FAILED(copy->LockRect(i, &lr, nullptr, 0)) || !lr.pBits) {
            copy->Release();
            return SS::Refusal::Create;
        }
        for (int y = 0; y < lh; y++) {
            uint8_t* row = static_cast<uint8_t*>(lr.pBits) + static_cast<size_t>(y) * lr.Pitch;
            for (int x = 0; x < lw; x++) {
                const float* p = &level[4 * (static_cast<size_t>(y) * lw + x)];
                if (wide) {
                    const uint16_t v[4] = {SS::To16(p[0]), SS::To16(p[1]), SS::To16(p[2]), SS::To16(p[3])}; // R, G, B, A
                    std::memcpy(row + 8 * x, v, 8);
                } else {
                    const uint8_t v[4] = {SS::To8(p[2]), SS::To8(p[1]), SS::To8(p[0]), SS::To8(p[3])}; // B, G, R, A
                    std::memcpy(row + 4 * x, v, 4);
                }
            }
        }
        copy->UnlockRect(i);
        bytes += static_cast<unsigned long long>(lw) * lh * (wide ? 8 : 4);
    }
    IUnknown* held = copy;
    if (FAILED(tex->SetPrivateData(kSmoothedGuid, &held, sizeof(held), D3DSPD_IUNKNOWN))) { // (it adds its own reference)
        copy->Release();
        return SS::Refusal::Create;
    }
    copy->Release();
    g_spriteMade++;
    g_spriteBytes += bytes;
    *out = copy;
    return SS::Refusal::None;
}

// A glow sprite's draw: the smoothed copy of its s0 texture (*game: the game's, to put back), or null to draw it as it is.
// Texture x vertex colour, additive or alpha blend (the caller checked: no depth write), a DXT texture read without sRGB.
IDirect3DTexture9* SmoothedFor(IDirect3DDevice9* dev, IDirect3DPixelShader9* ps, IDirect3DBaseTexture9** game) {
    if (!IsSprite(ps)) return nullptr;
    const Blend blend = BlendOf(dev);
    if (blend != kAdditive && blend != kAlpha) return nullptr;
    IDirect3DBaseTexture9* base = D3D9Hooks::BoundTexture(dev, 0);
    if (!base || base->GetType() != D3DRTYPE_TEXTURE) return nullptr;
    DWORD srgb = FALSE;
    dev->GetSamplerState(0, D3DSAMP_SRGBTEXTURE, &srgb);
    if (srgb) { // an sRGB read would differ on the 16-bit copy (not seen in the captures)
        g_frame.spritesAlone++;
        return nullptr;
    }
    auto* tex = static_cast<IDirect3DTexture9*>(base);
    IUnknown* held = nullptr;
    DWORD size = sizeof(held);
    IDirect3DTexture9* copy = nullptr;
    if (SUCCEEDED(tex->GetPrivateData(kSmoothedGuid, &held, &size)) && held) {
        held->Release(); // (GetPrivateData added a reference; the game texture keeps the copy)
        copy = static_cast<IDirect3DTexture9*>(held);
    } else {
        DWORD why = 0;
        size = sizeof(why);
        if (FAILED(tex->GetPrivateData(kRefusedGuid, &why, &size))) {
            const SS::Refusal r = MakeSmoothed(dev, tex, &copy);
            if (r != SS::Refusal::None) {
                copy = nullptr;
                g_spriteRefused[static_cast<int>(r)]++;
                why = static_cast<DWORD>(r);
                tex->SetPrivateData(kRefusedGuid, &why, sizeof(why), 0);
            }
        }
    }
    if (!copy) {
        g_frame.spritesAlone++;
        return nullptr;
    }
    *game = base;
    return copy;
}

template <typename DrawFn> D3D9Hooks::HookAction OnDraw(D3D9Hooks::DeviceContext& ctx, DrawFn draw) {
    IDirect3DDevice9* dev = ctx.device;
    // (the High-precision scene grains its 16-bit scene once, at its resolve)
    const bool on = g_on.load(std::memory_order_relaxed);
    const bool dither = on && !SceneTarget::Substituting(), jitter = g_jitterOn;
    if ((!on && !jitter) || !g_backBuffer) return D3D9Hooks::HookAction::Continue;
    if (!g_rt0 || g_rt0 != g_backBuffer) return D3D9Hooks::HookAction::Continue;
    if (ctx.ZEnable() == D3DZB_FALSE) return D3D9Hooks::HookAction::Continue; // UI and 2D passes
    // Depth on with ZFUNC ALWAYS and no depth write uses no depth (PostScene's rule): the UI's ~137 glass passes and
    // back-buffer copies drawn back (a light capture shows one under a panel: ps_2_0 reading a 2048x1024 copy). The grain
    // fed on itself there; the jitter already skipped them
    if (!ctx.ZWriteEnable() && ctx.ZFunc() == D3DCMP_ALWAYS) {
        g_frame.noDepth++;
        return D3D9Hooks::HookAction::Continue;
    }
    // the Banding Fix's pixel copy
    IDirect3DPixelShader9* ps = nullptr;
    Copy copy;
    if (on) ps = D3D9Hooks::BoundPixelShader(dev); // (followed through Set*Shader; the device keeps it alive)
    if (dither) {
        if (!ps) g_frame.noShader++;
        if (ps) {
            copy = CopyOf(dev, ps);
            if (!copy.ps) {
                const DWORD major = (copy.version >> 8) & 0xFF;
                (major == 2 ? g_frame.ps2Refused : major == 3 ? g_frame.ps3Refused : g_frame.other)++;
            }
        }
    }
    bool useDither = copy.ps != nullptr;
    const int k = useDither ? copy.texcoord : -1; // ps_2_x copies: the vertex shader must carry the position
    // the vertex copy: TEXCOORDk and / or the temporal jitter
    IDirect3DVertexShader9 *vs = nullptr, *vsCopy = nullptr;
    const DWORD zfunc = jitter ? ctx.ZFunc() : static_cast<DWORD>(D3DCMP_LESSEQUAL);
    // Depth enabled + ALWAYS is also used by screen-space composition draws;
    // ZENABLE alone does not identify a projected 3D surface (pool capture #202).
    bool useJitter = jitter && zfunc != D3DCMP_ALWAYS;
    if (useJitter || k >= 0) {
        vs = D3D9Hooks::BoundVertexShader(dev);
        if (!vs && useJitter) {
            g_frame.jitterNoShader++;
            useJitter = false;
        }
        if (vs && (useJitter || k >= 0)) {
            vsCopy = VsCopyOf(dev, vs, k, useJitter);
            if (!vsCopy && useJitter) { // refused with the jitter: at least the grain
                g_frame.jitterRefused++;
                useJitter = false;
                if (k >= 0) vsCopy = VsCopyOf(dev, vs, k, false);
            }
        }
        if (k >= 0 && !vsCopy) {
            g_frame.ps2NoPair++;
            useDither = false;
        }
    }
    // a glow sprite (no depth write): its smoothed texture at s0, with or without the grain
    IDirect3DBaseTexture9* sprite = nullptr;
    IDirect3DTexture9* smoothed = on && ps && !ctx.ZWriteEnable() ? SmoothedFor(dev, ps, &sprite) : nullptr;
    if (!useDither && !useJitter && !smoothed) {
#ifdef APEX_F10_STUDY
        F10Study::CompositionShader(dev, ps, ps, vs, false);
#endif
        return D3D9Hooks::HookAction::Continue;
    }
    float before[4] = {};
    Blend blend = kOpaque;
    if (useDither) {
        blend = BlendOf(dev);
        // Additive and multiply blends add their grain on top of the grain already in the target: half of it, with a phase
        // whose pattern does not correlate with the full one (R2 shifted by 0.19: correlation 0.006, offline)
        const bool half = blend == kAdditive || blend == kMultiply || blend == kOtherBlend;
        // (2.15.3) the viewport only for ps_2_x copies: the ps_3_0 grain reads the amount's .x and .w alone (AddDither), the
        // ps_2_x one rebuilds the pixel from .y / .z (AddDither2)
        D3DVIEWPORT9 vp{};
        if (k >= 0) dev->GetViewport(&vp);
        dev->GetPixelShaderConstantF(static_cast<UINT>(copy.amountReg), before, 1);
        // Developer "Show covered surfaces": a coarse grain only where the fix applies
        const float steps = g_showCovered ? kShowCoveredSteps : SceneDither::Strength() * (half ? 0.5f : 1.0f);
        const float amount[4] = {steps / 255.0f, 0.5f * static_cast<float>(vp.Width), 0.5f * static_cast<float>(vp.Height),
                                 SceneDither::GrainPhase() + (half ? 0.19f : 0.0f)};
        D3D9Hooks::CallOriginalSetPixelShaderConstantF(dev, static_cast<UINT>(copy.amountReg), amount, 1);
        D3D9Hooks::CallOriginalSetPixelShader(dev, copy.ps);
    }
    if (useJitter) D3D9Hooks::CallOriginalSetVertexShaderConstantF(dev, kJitterConst, g_jitter, 1); // no game shader reads c252
    if (vsCopy) D3D9Hooks::CallOriginalSetVertexShader(dev, vsCopy);
    if (smoothed) D3D9Hooks::CallOriginalSetTexture(dev, 0, smoothed);
#ifdef APEX_F10_STUDY
    F10Study::CompositionShader(dev, ps, useDither ? copy.ps : ps, vsCopy ? vsCopy : vs, useDither);
#endif
    draw();
    if (smoothed) {
        D3D9Hooks::CallOriginalSetTexture(dev, 0, sprite);
        g_frame.sprites++;
    }
    if (vsCopy) D3D9Hooks::CallOriginalSetVertexShader(dev, vs);
    if (useDither) {
        D3D9Hooks::CallOriginalSetPixelShader(dev, ps);
        D3D9Hooks::CallOriginalSetPixelShaderConstantF(dev, static_cast<UINT>(copy.amountReg), before, 1); // the game may cache it
        (k >= 0 ? g_frame.dithered2 : g_frame.dithered3)++;
        g_frame.blend[blend]++;
    }
    if (useJitter) g_frame.jittered++;
    return D3D9Hooks::HookAction::Skip;
}

void OnFrameBoundary(IDirect3DDevice9* dev) {
    IDirect3DSurface9* bb = nullptr;
    if (SUCCEEDED(dev->GetBackBuffer(0, 0, D3DBACKBUFFER_TYPE_MONO, &bb)) && bb) {
        g_backBuffer = bb;
        bb->Release();
    }
    if (!g_rt0 && SUCCEEDED(dev->GetRenderTarget(0, &bb)) && bb) {
        g_rt0 = bb;
        bb->Release();
    }
    g_last = g_frame;
    g_frame = {};
    // Development build: the coverage in the log now and then (the first 12 times, every 20 s of scene)
    if (!kPublicBuild) {
        const unsigned long long now = GetTickCount64();
        const unsigned total = g_last.dithered3 + g_last.dithered2 + g_last.ps2NoPair + g_last.ps2Refused + g_last.ps3Refused + g_last.other;
        if (total > 50 && g_logs < 12 && now - g_lastLog >= 20000) {
            g_lastLog = now;
            g_logs++;
            LOG_INFO(std::format("[SceneDither] Last frame, 3D scene draws: {} dithered (ps_3_0), {} dithered (ps_2_x), {} ps_2_x whose vertex shader "
                                 "could not carry the position, {} ps_2_x refused, {} ps_3_0 refused, {} other | copies made: pixel {}, vertex {}; "
                                 "refused: no colour write {}, subroutines {}, relative constants {}, no free register {}, unreadable {} | glow sprites: "
                                 "{} smoothed, {} as drawn, textures smoothed {}; refused: format {}, size {}, pool {}, detail {}, lock {}, create {}",
                                 g_last.dithered3, g_last.dithered2, g_last.ps2NoPair, g_last.ps2Refused, g_last.ps3Refused, g_last.other, g_made.load(),
                                 g_madeVs.load(), g_refused[2].load(), g_refused[3].load(), g_refused[4].load(), g_refused[5].load(), g_refused[6].load(),
                                 g_last.sprites, g_last.spritesAlone, g_spriteMade.load(), g_spriteRefused[1].load(), g_spriteRefused[2].load(),
                                 g_spriteRefused[3].load(), g_spriteRefused[4].load(), g_spriteRefused[5].load(), g_spriteRefused[6].load()));
        }
    }
}

void OnPreReset(IDirect3DDevice9*) { // read again at the next frame boundary
    g_backBuffer = nullptr;
    g_rt0 = nullptr;
}

void RegisterHooks() {
    using namespace D3D9Hooks;
    RegisterPresent(kHookName, [](DeviceContext& ctx, const RECT*, const RECT*, HWND, const RGNDATA*) {
        OnFrameBoundary(ctx.device);
        return HookAction::Continue;
    }, Priority::First);
    RegisterSetRenderTarget(kHookName, [](DeviceContext& ctx, DWORD index, IDirect3DSurface9* rt) {
        if (index == 0) g_rt0 = rt;
        return HookAction::Continue;
    }, Priority::First);
    RegisterCreatePixelShader(kHookName, [](DeviceContext& ctx, const DWORD* fn, IDirect3DPixelShader9** out) {
        if (t_own || !fn || !out || !g_on.load()) return HookAction::Continue;
        // create the game's shader here (every earlier callback already ran) to learn its pointer, then its copy
        if (FAILED(CallOriginalCreatePixelShader(ctx.device, fn, out)) || !*out) return HookAction::Block;
        Forget(*out); // a reused address: its old copy goes; the new one is made at its first 3D scene draw (CopyOf)
        return HookAction::Skip;
    }, kAfterEveryone);
    RegisterCreateVertexShader(kHookName, [](DeviceContext& ctx, const DWORD* fn, IDirect3DVertexShader9** out) {
        const bool dither = g_on.load(), jitter = g_jitterWanted.load();
        if (t_own || !fn || !out || (!dither && !jitter)) return HookAction::Continue;
        if (FAILED(CallOriginalCreateVertexShader(ctx.device, fn, out)) || !*out) return HookAction::Block;
        ForgetVs(*out); // a reused address: the old shader's copies go; new ones are made at its first 3D scene draw (VsCopyOf)
        return HookAction::Skip;
    }, kAfterEveryone);
    ObserveReissuedDraws(kHookName); // dithers Night Lighting's replacement shaders too (they run in the re-issued draw)
    RegisterDrawIndexedPrimitive(kHookName, [](DeviceContext& ctx, D3DPRIMITIVETYPE type, INT bvi, UINT minV, UINT numV, UINT start, UINT prims) {
        return OnDraw(ctx, [&] { CallOriginalDrawIndexedPrimitive(ctx.device, type, bvi, minV, numV, start, prims); });
    }, kAfterEveryone);
    RegisterDrawPrimitive(kHookName, [](DeviceContext& ctx, D3DPRIMITIVETYPE type, UINT start, UINT prims) {
        return OnDraw(ctx, [&] { CallOriginalDrawPrimitive(ctx.device, type, start, prims); });
    }, kAfterEveryone);
    RenderCallbacks::Add(RenderCallbacks::preReset, OnPreReset);
}

// The draw hooks run while the Banding Fix or the temporal AA uses them
void AcquireHooks() {
    if (g_hookUsers.fetch_add(1) == 0) {
        g_backBuffer = nullptr;
        g_rt0 = nullptr;
        RegisterHooks();
    }
}
void ReleaseHooks() {
    if (g_hookUsers.fetch_sub(1) != 1) return;
    D3D9Hooks::UnregisterAll(kHookName);
    RenderCallbacks::Remove(RenderCallbacks::preReset, OnPreReset);
    ReleaseCopies(); // an address reused while off must never meet an old copy
    g_backBuffer = nullptr;
}

} // namespace

class SceneDitherPatch : public ApexPatch {
  public:
    SceneDitherPatch() : ApexPatch("SceneDither", nullptr) {
        RegisterFloatSetting(&g_strength, "forca", SettingWidget::Slider, 1.0f, 0.0f, 1.0f, "Grain in 8-bit steps (triangular peak); 1 = +-1 step, 0 = none");
        // "graoEmMovimento" (Moving grain, removed) is left in old files and ignored: the grain is always fixed
        RegisterBoolSetting(SceneTarget::Setting(), "cenaAltaPrecisao", true, "The 3D scene in a 16-bit target, resolved once (scene_target.cpp)");
    }

    bool Install() override {
        if (isEnabled) return true;
        lastError.clear();
        g_on = true;
        AcquireHooks();
        isEnabled = true;
        LOG_INFO("[SceneDither] Installed");
        return true;
    }

    bool Uninstall() override {
        if (!isEnabled) return true;
        lastError.clear();
        g_on = false;
        ReleaseHooks();
        isEnabled = false;
        LOG_INFO(std::format("[SceneDither] Uninstalled (copies made this session: pixel {}, vertex {})", g_made.load(), g_madeVs.load()));
        return true;
    }

    // Read live every draw
    void Update() override { pendingReinstall = false; }

    void RenderCustomUI() override {
        SAFE_IMGUI_BEGIN();
        if (ApexUi::SliderPercent("Strength", &g_strength, 0.0f, 1.0f, "How much grain hides the color steps; 100% hides them fully, 0% is off", 1.0f))
            NotifySettingChanged();
    }

    // Developer page > Debug views
    void RenderDeveloperUI() override {
        SAFE_IMGUI_BEGIN();
        size_t pairs = 0, vsPairs = 0;
        {
            std::lock_guard<std::mutex> lock(g_lock);
            pairs = g_copies.size();
            vsPairs = g_vsCopies.size();
        }
        ApexUi::Checkbox("Show covered surfaces", &g_showCovered);
        ApexUi::Tooltip("A coarse grain on every surface the fix covers; smooth surfaces are not covered (not saved)");
        if (ApexUi::BeginAdvanced("ShaderCoverage", "Shader coverage")) {
        ImGui::TextDisabled("Pixel shaders seen: %zu (copies %u)  |  vertex copies asked: %zu (made %u)", pairs, g_made.load(), vsPairs, g_madeVs.load());
        ImGui::TextDisabled("Refused: no colour write %u, subroutines %u, relative constants %u, no free register %u, unreadable %u", g_refused[2].load(),
                            g_refused[3].load(), g_refused[4].load(), g_refused[5].load(), g_refused[6].load());
        const unsigned dithered = g_last.dithered3 + g_last.dithered2;
        const unsigned total = dithered + g_last.ps2NoPair + g_last.ps2Refused + g_last.ps3Refused + g_last.other;
        ImGui::TextDisabled("Last frame, 3D scene draws: %u with the grain (%.0f%%: ps_3_0 %u, ps_2_x %u); without: %u ps_2_x (vertex shader), %u ps_2_x, "
                            "%u ps_3_0 refused, %u other",
                            dithered, total ? 100.0 * dithered / total : 0.0, g_last.dithered3, g_last.dithered2, g_last.ps2NoPair, g_last.ps2Refused,
                            g_last.ps3Refused, g_last.other);
        ImGui::TextDisabled("Dithered by blend: opaque %u, alpha %u, additive %u (half), multiply %u (half), other %u (half)  |  left alone: "
                            "%u depth ALWAYS without depth write (UI), %u without a pixel shader",
                            g_last.blend[kOpaque], g_last.blend[kAlpha], g_last.blend[kAdditive], g_last.blend[kMultiply], g_last.blend[kOtherBlend],
                            g_last.noDepth, g_last.noShader);
        const auto refused = [](SpriteSmoothLogic::Refusal r) { return g_spriteRefused[static_cast<int>(r)].load(); };
        using R = SpriteSmoothLogic::Refusal;
        ImGui::TextDisabled("Glow sprites, last frame: %u smoothed, %u as drawn  |  textures smoothed %u (%.0f KB, %s); refused: format %u, size %u, "
                            "pool %u, detail %u, lock %u, create %u",
                            g_last.sprites, g_last.spritesAlone, g_spriteMade.load(), g_spriteBytes.load() / 1024.0, g_filter16 == 0 ? "8-bit" : "16-bit",
                            refused(R::Format), refused(R::Size), refused(R::Pool), refused(R::Detail), refused(R::Lock), refused(R::Create));
            ApexUi::EndAdvanced();
        }
    }
};

APEX_REGISTER_FEATURE(SceneDitherPatch, {.displayName = "Banding Fix",
                                         .description = "Removes the color steps (banding) in lamp light, shadows and other smooth gradients of the 3D world: "
                                                        "an invisible, fixed grain where the game rounds its colors, so light fades smoothly. Menus are "
                                                        "untouched. Part of " APEX_PRODUCT_NAME ". Credits: @loinyx",
                                         .category = "Graphics",
                                         .experimental = false,
                                         .enabledByDefault = true,
                                         .supportedVersions = VERSION_ALL,
                                         .technicalDetails = {"A dithered copy of every pixel shader (triangular noise from the R2 pattern of the pixel position, "
                                                              "+-1/255 at Strength 100% on RGB, within half a step of black and white; alpha untouched), made at the "
                                                              "shader's first draw in the 3D scene.",
                                                              "ps_2_x copies take the pixel position from a texture coordinate that a copy of the vertex shader fills "
                                                              "with the clip position.",
                                                              "Bound only for draws into the back buffer that use the depth test (the 3D scene; not ZFUNC ALWAYS without "
                                                              "depth write), last in the draw chain, at half strength for additive and multiply blends; "
                                                              "the amount constant's previous value is put back after the draw.",
                                                              "Glow sprites (texture x vertex colour, additive or alpha blend, no depth write) whose texture is a small "
                                                              "DXT gradient are drawn with a smoothed 16-bit copy of it, made once per texture."}})

namespace SceneBinder {
void AcquireJitter() {
    g_jitterWanted = true;
    AcquireHooks();
}
void ReleaseJitter() {
    g_jitterWanted = false;
    g_jitterOn = false;
    ReleaseHooks();
}
void SetFrameJitter(bool on, float clipX, float clipY) {
    g_jitterOn = on && g_jitterWanted.load();
    g_jitter[0] = clipX;
    g_jitter[1] = clipY;
}
Coverage LastCoverage() { return {g_last.jittered, g_last.jitterRefused, g_last.jitterNoShader}; }
} // namespace SceneBinder

namespace SceneDither {
bool On() { return g_on.load(std::memory_order_relaxed); }
float Strength() { return std::clamp(g_strength, 0.0f, 1.0f); }
float GrainPhase() { return 0.0f; }
} // namespace SceneDither
