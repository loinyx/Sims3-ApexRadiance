#include "scene_copy.h"
// See scene_copy.h.
//
// Part of Apex Radiance. Credits: @loinyx

#include "apex_log.h"
#include <format>
#include <mutex>

namespace SceneCopy {
namespace {

std::mutex g_lock;
IDirect3DTexture9* g_tex = nullptr;
IDirect3DSurface9* g_surf = nullptr;
UINT g_w = 0, g_h = 0;
D3DFORMAT g_fmt = D3DFMT_UNKNOWN;
int g_holders = 0;

bool Create(IDirect3DDevice9* dev, UINT width, UINT height, D3DFORMAT format, IDirect3DTexture9** texture, IDirect3DSurface9** surface) {
    IDirect3DTexture9* tex = nullptr;
    IDirect3DSurface9* surf = nullptr;
    if (FAILED(dev->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET, format, D3DPOOL_DEFAULT, &tex, nullptr)) || !tex) return false;
    if (FAILED(tex->GetSurfaceLevel(0, &surf)) || !surf) {
        tex->Release();
        return false;
    }
    *texture = tex;
    *surface = surf;
    return true;
}

} // namespace

bool Acquire(IDirect3DDevice9* dev, UINT width, UINT height, D3DFORMAT format, IDirect3DTexture9** texture, IDirect3DSurface9** surface) {
    if (!dev || !texture || !surface || !width || !height) return false;
    std::lock_guard<std::mutex> lock(g_lock);
    if (g_tex && (g_w != width || g_h != height || g_fmt != format)) {
        LOG_WARNING(std::format("[SceneCopy] A copy of {}x{} was asked while the shared one is {}x{}: that pass keeps its own", width, height, g_w, g_h));
        return Create(dev, width, height, format, texture, surface);
    }
    if (!g_tex) {
        if (!Create(dev, width, height, format, &g_tex, &g_surf)) return false;
        g_w = width, g_h = height, g_fmt = format, g_holders = 0;
        LOG_INFO(std::format("[SceneCopy] Shared copy of the picture created ({}x{})", width, height));
    }
    g_holders++;
    *texture = g_tex;
    *surface = g_surf;
    return true;
}

void Drop(IDirect3DTexture9** texture, IDirect3DSurface9** surface) {
    if (!texture || !surface) return;
    std::lock_guard<std::mutex> lock(g_lock);
    IDirect3DTexture9* tex = *texture;
    IDirect3DSurface9* surf = *surface;
    *texture = nullptr;
    *surface = nullptr;
    if (!tex || tex != g_tex) { // a pass's own texture (or a surface left without its texture)
        if (surf && surf != g_surf) surf->Release();
        if (tex) tex->Release();
        return;
    }
    if (--g_holders > 0) return;
    g_surf->Release();
    g_tex->Release();
    g_tex = nullptr, g_surf = nullptr, g_holders = 0;
}

} // namespace SceneCopy
