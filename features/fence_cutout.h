#pragma once
// The open share of a mesh fence (chain-link, lattice: GroundShadow::Wall::pass). Its pixel shader cuts the mesh out with its
// texture's alpha (ShaderPatches::FindCutout: texkill where tex.a x tint x scale < ref), so the share of its texels under
// that alpha is the share of the lamp light the fence lets through. Pure: no D3D calls, the caller locks the texture.
#include <cstdint>

namespace FenceCutout {

enum class Fmt { Argb, Dxt1, Dxt3, Dxt5 };

// The alpha (0..255) of texel i (0..15, row-major) of a 4x4 block
inline int BlockAlpha(Fmt f, const uint8_t* b, int i) {
    switch (f) {
    case Fmt::Dxt1: { // 1-bit alpha: index 3 is transparent when colour 0 <= colour 1
        const unsigned c0 = b[0] | b[1] << 8, c1 = b[2] | b[3] << 8;
        const uint32_t idx = b[4] | b[5] << 8 | b[6] << 16 | static_cast<uint32_t>(b[7]) << 24;
        return c0 <= c1 && ((idx >> (2 * i)) & 3) == 3 ? 0 : 255;
    }
    case Fmt::Dxt3: return ((b[i >> 1] >> (4 * (i & 1))) & 15) * 17;
    case Fmt::Dxt5: {
        const int a0 = b[0], a1 = b[1];
        const uint64_t bits = static_cast<uint64_t>(b[2]) | static_cast<uint64_t>(b[3]) << 8 | static_cast<uint64_t>(b[4]) << 16 |
                              static_cast<uint64_t>(b[5]) << 24 | static_cast<uint64_t>(b[6]) << 32 | static_cast<uint64_t>(b[7]) << 40;
        const int k = static_cast<int>((bits >> (3 * i)) & 7);
        if (k == 0) return a0;
        if (k == 1) return a1;
        if (a0 > a1) return ((8 - k) * a0 + (k - 1) * a1) / 7;
        if (k == 6) return 0;
        if (k == 7) return 255;
        return ((6 - k) * a0 + (k - 1) * a1) / 5;
    }
    default: return 255;
    }
}

// The share of a w x h level's texels whose alpha is under `keep` (the smallest alpha the shader keeps). pitch: bytes per
// row (A8R8G8B8) or per row of 4x4 blocks (DXT). 0 for an empty level.
inline float OpenShare(Fmt f, const uint8_t* bits, int pitch, int w, int h, int keep) {
    if (!bits || w <= 0 || h <= 0) return 0.0f;
    uint64_t open = 0;
    if (f == Fmt::Argb) {
        for (int y = 0; y < h; y++)
            for (int x = 0; x < w; x++) open += bits[static_cast<size_t>(y) * pitch + x * 4 + 3] < keep;
    } else {
        const int blockBytes = f == Fmt::Dxt1 ? 8 : 16; // (DXT3 / DXT5: the alpha half comes first)
        for (int by = 0; by < (h + 3) / 4; by++)
            for (int bx = 0; bx < (w + 3) / 4; bx++) {
                const uint8_t* b = bits + static_cast<size_t>(by) * pitch + static_cast<size_t>(bx) * blockBytes;
                for (int i = 0; i < 16; i++)
                    if (bx * 4 + (i & 3) < w && by * 4 + (i >> 2) < h) open += BlockAlpha(f, b, i) < keep;
            }
    }
    return static_cast<float>(static_cast<double>(open) / (static_cast<double>(w) * h));
}

} // namespace FenceCutout
