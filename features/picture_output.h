#pragma once
#include <d3d9.h>
namespace PictureOutput {
// Game alpha carries native composition data; only the final scene pass uses this mask.
inline HRESULT SetSceneWriteMask(IDirect3DDevice9* device) noexcept {
    return device->SetRenderState(D3DRS_COLORWRITEENABLE, D3DCOLORWRITEENABLE_RED | D3DCOLORWRITEENABLE_GREEN | D3DCOLORWRITEENABLE_BLUE);
}
}
