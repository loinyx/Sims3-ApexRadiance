#pragma once
// Read-only package checks use the actual technique pairs, including weather variants.
#include "shaders/shader_ids.h"
#include <algorithm>
#include <set>
#include <string>
namespace TerrainPackageChecks {
struct Blob {
    bool vs = false;
    std::vector<DWORD> code;
};
struct Tech {
    std::vector<std::pair<uint32_t, uint32_t>> passes; // (vs, ps), 1-based blob indices
};
std::vector<DWORD> Trim(const unsigned char* p, size_t n) {
    for (size_t k = 0; k + 4 <= std::min<size_t>(n, 12); k += 4) {
        DWORD v;
        std::memcpy(&v, p + k, 4);
        if ((v & 0xFFFE0000u) != 0xFFFE0000u) continue;
        size_t j = k + 4;
        while (j + 4 <= n) {
            DWORD tok;
            std::memcpy(&tok, p + j, 4);
            if (tok == 0x0000FFFFu) {
                std::vector<DWORD> c((j + 4 - k) / 4);
                std::memcpy(c.data(), p + k, c.size() * 4);
                return c;
            }
            j += ((tok & 0xFFFF) == 0xFFFE) ? 4 + 4 * ((tok >> 16) & 0x7FFF) : 4 + 4 * ((tok >> 24) & 0xF);
        }
    }
    return {};
}
bool LoadPrecomp(const std::vector<unsigned char>& d, std::vector<Blob>& blobs, std::vector<Tech>& techs) {
    auto find = [&](const char* tag, size_t from, size_t to) -> size_t {
        for (size_t i = from; i + 4 <= to; i++)
            if (std::memcmp(&d[i], tag, 4) == 0) return i;
        return std::string::npos;
    };
    const size_t shdb = find("SHDB", 0, d.size());
    if (shdb == std::string::npos) return false;
    uint32_t size;
    std::memcpy(&size, &d[shdb + 4], 4);
    const size_t end = std::min(d.size(), shdb + 8 + size);
    for (size_t j = shdb + 8; j + 8 <= end;) {
        const bool v = std::memcmp(&d[j], "VSHD", 4) == 0, p = std::memcmp(&d[j], "PSHD", 4) == 0;
        if (!v && !p) {
            j++;
            continue;
        }
        uint32_t n;
        std::memcpy(&n, &d[j + 4], 4);
        blobs.push_back({v, Trim(&d[j + 8], std::min<size_t>(n, d.size() - j - 8))});
        j += 8 + n;
    }
    std::vector<size_t> at;
    for (size_t i = end; i + 4 <= d.size(); i++)
        if (std::memcmp(&d[i], "TECH", 4) == 0) at.push_back(i);
    for (size_t k = 0; k < at.size(); k++) {
        const size_t lim = k + 1 < at.size() ? at[k + 1] : d.size();
        Tech t;
        uint32_t np;
        std::memcpy(&np, &d[at[k] + 8], 4);
        size_t q = at[k] + 12;
        for (uint32_t i = 0; i < np && i < 64; i++) {
            q = find("PASS", q, lim);
            if (q == std::string::npos || q + 20 > lim) break;
            uint32_t vs, ps;
            std::memcpy(&vs, &d[q + 12], 4);
            std::memcpy(&ps, &d[q + 16], 4);
            t.passes.push_back({vs, ps});
            q += 4;
        }
        techs.push_back(std::move(t));
    }
    return !blobs.empty();
}

inline void Run(const std::vector<unsigned char>& data, IDirect3DDevice9* device) {
    std::vector<Blob> blobs;
    std::vector<Tech> techniques;
    assert(LoadPrecomp(data, blobs, techniques));
    std::set<std::pair<uint32_t, uint32_t>> pairs;
    unsigned compact = 0, compactLayouts = 0, patched = 0, refused = 0;
    std::set<uint32_t> compactVertices;
    for (const Tech& technique : techniques)
        for (const auto& pass : technique.passes) {
            if (!pass.first || !pass.second || pass.first > blobs.size() || pass.second > blobs.size()) continue;
            const auto& vertex = blobs[pass.first - 1].code;
            const auto& pixel = blobs[pass.second - 1].code;
            if (vertex.empty() || pixel.empty()) continue;
            const bool exactCompact = IsShader(kWorldCompactPs, pixel.data(), pixel.size() * 4);
            const int mapping = ShaderPatches::WorldTerrainMapConstant(vertex);
            if (exactCompact) {
                assert(mapping >= 0 || IsShader(kWorldCompactVs, vertex.data(), vertex.size() * 4));
                compactVertices.insert(pass.first);
                ++compact;
            }
            if (mapping >= 0 && pairs.insert(pass).second) {
                if (exactCompact) ++compactLayouts;
                for (DWORD sampler = 1; sampler < 16; ++sampler) {
                    if (ShaderPatches::LightMapScaleConst(pixel, sampler) < 0) continue;
                    auto copy = pixel;
                    DWORD native = 0, shade = 0;
                    if (!ShaderPatches::PatchTerrainNativeAlpha(copy, sampler, native) ||
                        !ShaderPatches::PatchTerrainShade(copy, sampler, shade)) {
                        ++refused;
                        std::cout << "Terrain pair refused: VS " << pass.first << " PS " << pass.second
                                  << " sampler " << sampler << " map c" << mapping << " native " << native << '\n';
                        continue;
                    }
                    IDirect3DPixelShader9* shader = nullptr;
                    assert(SUCCEEDED(device->CreatePixelShader(copy.data(), &shader)) && shader);
                    shader->Release();
                    ++patched;
                }
            }
        }
    assert(compact && compactLayouts && patched);
    std::cout << "Technique pairs: compact terrain " << compact << " uses, " << compactVertices.size()
              << " vertex variants, " << compactLayouts << " unique compact pairs; " << pairs.size()
              << " verified terrain pairs; " << patched << " lamp paths load, " << refused << " refused\n";
}
}
