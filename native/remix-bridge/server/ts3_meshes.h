#pragma once
// TS3 furniture caster meshes read from the player's own packages (docs/features/realtime-gi/wp/X-MESHCAST.md, MC-1).
// Header only, standard library only, for the 64-bit bridge server and the offline check (tools/gi_server_test/meshes_check.cpp).
// The game's data is read from the player's install at run time; nothing of it is copied or shipped.
//
// Chain: an object's LITE instance (LightObjectAttach) -> the VPXY that links that LITE to a MODL -> the MODL's shadow LOD
// (LOD id 0x10001, else 0x10000), else its LOD 0 -> triangles in model space. The same rules as tools/gi_server_test/casters.py
// and tools/object_legs_table/generate.py, which the offline check compares against.
// Packages: the game's FullBuild* / DeltaBuild* (a Delta overrides a Full), then Mods packages (any name; they override the
// game, as the game's own Mods priority does). Every read is bounds-checked: a broken or custom resource is a miss, never a
// fault. Not thread-safe: one thread builds and reads (the GI worker).
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <vector>

namespace ts3mesh {

constexpr uint32_t kModl = 0x01661233, kLite = 0x03B4C61D, kVpxy = 0x736884F1, kMlod = 0x01D10F34;
constexpr uint32_t kShadowLods[2] = {0x10001, 0x10000}; // shadow low, shadow high
constexpr uint32_t kOptScale = 0x487648E5, kOptOffset = 0x790EBF2C; // the shadow mesh's scale and offset material floats
constexpr uint32_t kDropShadow = 0x08 << 8; // MLOD mesh flag of the floor decal (above the primitive type byte)

struct Key {
    uint32_t type = 0, group = 0;
    uint64_t inst = 0;
    bool operator<(const Key& o) const { return type != o.type ? type < o.type : group != o.group ? group < o.group : inst < o.inst; }
    bool operator==(const Key& o) const { return type == o.type && group == o.group && inst == o.inst; }
};

enum class Source : uint8_t { None, Shadow, Lod0 };
struct Mesh {
    std::vector<float> pos;    // x y z a vertex, model space (metres)
    std::vector<uint32_t> tri; // a b c a triangle
    Source source = Source::None;
};

// A bounds-checked view: every read past the end throws std::out_of_range (caught by the callers below)
struct Bytes {
    const uint8_t* p = nullptr;
    size_t n = 0;
    void need(size_t at, size_t len) const {
        if (at > n || len > n - at) throw std::out_of_range("resource read past its end");
    }
    uint32_t u32(size_t at) const { need(at, 4); uint32_t v; std::memcpy(&v, p + at, 4); return v; }
    int32_t i32(size_t at) const { return static_cast<int32_t>(u32(at)); }
    int16_t i16(size_t at) const { need(at, 2); int16_t v; std::memcpy(&v, p + at, 2); return v; }
    float f32(size_t at) const { need(at, 4); float v; std::memcpy(&v, p + at, 4); return v; }
    uint64_t u64(size_t at) const { return static_cast<uint64_t>(u32(at + 4)) << 32 | u32(at); }
    bool tag(size_t at, const char* t) const { return at <= n && n - at >= 4 && std::memcmp(p + at, t, 4) == 0; }
    Bytes sub(size_t at, size_t len) const { need(at, len); return {p + at, len}; }
};

// RefPack (the DBPF 2.0 compression 0xFFFF); `want` from the package index caps the output
inline std::vector<uint8_t> Refpack(Bytes d, size_t want) {
    std::vector<uint8_t> out(want);
    size_t p = (d.n && (d.p[0] & 0x80)) ? 6 : 5, w = 0;
    const auto at = [&](size_t i) { d.need(i, 1); return d.p[i]; };
    while (p < d.n) {
        const uint32_t c = at(p);
        size_t lit, len, off;
        if (c < 0x80) {
            const uint32_t b = at(p + 1);
            lit = c & 3, len = ((c & 0x1C) >> 2) + 3, off = ((c & 0x60) << 3) + b + 1, p += 2;
        } else if (c < 0xC0) {
            const uint32_t b1 = at(p + 1), b2 = at(p + 2);
            lit = b1 >> 6, len = (c & 0x3F) + 4, off = ((b1 & 0x3F) << 8) + b2 + 1, p += 3;
        } else if (c < 0xE0) {
            const uint32_t b1 = at(p + 1), b2 = at(p + 2), b3 = at(p + 3);
            lit = c & 3, len = ((c & 0x0C) << 6) + b3 + 5, off = ((c & 0x10) << 12) + (b1 << 8) + b2 + 1, p += 4;
        } else if (c < 0xFC) {
            lit = ((c & 0x1F) << 2) + 4, len = 0, off = 0, p += 1;
        } else {
            lit = c & 3, len = 0, off = 0, p += 1;
        }
        d.need(p, lit);
        if (w + lit + len > want) throw std::out_of_range("refpack output past its size");
        std::memcpy(out.data() + w, d.p + p, lit);
        p += lit, w += lit;
        if (c >= 0xFC) break;
        if (off > w) throw std::out_of_range("refpack copy before the start");
        for (size_t i = 0; i < len; i++, w++) out[w] = out[w - off]; // (overlapping copies repeat bytes)
    }
    out.resize(w);
    return out;
}

// An RCOL resource: its chunks, public count and external keys
struct Rcol {
    std::vector<Bytes> chunks;
    uint32_t pub = 0;
    std::vector<Key> ext;
    // A chunk reference (kind in the top 4 bits, index + 1 below): kinds 0 and 1 are chunks here, 2 and up external
    static bool Ref(uint32_t v, uint32_t& kind, uint32_t& index) {
        if (!v) return false;
        kind = v >> 28, index = (v & 0x0FFFFFFF) - 1;
        return true;
    }
    Bytes Chunk(uint32_t kind, uint32_t index) const {
        const size_t i = index + (kind == 1 ? pub : 0);
        if (i >= chunks.size()) throw std::out_of_range("chunk reference");
        return chunks[i];
    }
    const Bytes* First(const char* tag) const {
        for (const Bytes& c : chunks)
            if (c.tag(0, tag)) return &c;
        return nullptr;
    }
};
inline Rcol ReadRcol(Bytes d) {
    Rcol r;
    r.pub = d.u32(4);
    const uint32_t ne = d.u32(12), ni = d.u32(16);
    if (ne > 4096 || ni > 4096) throw std::out_of_range("RCOL counts");
    size_t o = 20;
    std::vector<Key> its(ni);
    for (Key& k : its) k = {d.u32(o + 8), d.u32(o + 12), d.u64(o)}, o += 16;
    for (uint32_t i = 0; i < ne; i++) r.ext.push_back({d.u32(o + 8), d.u32(o + 12), d.u64(o)}), o += 16;
    for (uint32_t i = 0; i < ni; i++, o += 8) { // a chunk may name a size past the resource's end: cut it there (as Python's slices do)
        const size_t at = d.u32(o);
        r.chunks.push_back(d.sub(at, std::min<size_t>(d.u32(o + 4), at <= d.n ? d.n - at : 0)));
    }
    return r;
}

struct MlodMesh {
    uint32_t vrtf, vbuf, ibuf, flags, soff, sv, si, vc, pc, mat; // vrtf .. ibuf and mat: chunk references (0: none)
};
inline std::vector<MlodMesh> MlodMeshes(Bytes b) {
    if (!b.tag(0, "MLOD")) throw std::out_of_range("no MLOD");
    std::vector<MlodMesh> out;
    size_t o = 12;
    for (uint32_t i = 0, n = b.u32(8); i < n; i++) {
        const size_t size = b.u32(o), p = o + 4;
        MlodMesh m{b.u32(p + 8), b.u32(p + 12), b.u32(p + 16), b.u32(p + 20), b.u32(p + 24), b.u32(p + 28), b.u32(p + 32), b.u32(p + 40), b.u32(p + 44), 0};
        const size_t q = p + 72;
        m.mat = b.u32(q + 8 + 4 * static_cast<size_t>(b.u32(q + 4)));
        out.push_back(m);
        o += 4 + size;
    }
    return out;
}

// The IBUF's indices (int16 or int32, maybe differenced), as casters.py reads them
inline std::vector<int32_t> Indices(Bytes b) {
    const uint32_t fl = b.u32(8);
    const size_t w = (fl & 2) ? 4 : 2, n = b.n < 16 ? 0 : (b.n - 16) / w;
    std::vector<int32_t> v(n);
    int32_t cur = 0;
    for (size_t i = 0; i < n; i++) {
        const int32_t x = w == 4 ? b.i32(16 + 4 * i) : b.i16(16 + 2 * i);
        v[i] = (fl & 1) ? (cur += x) : x;
    }
    return v;
}

// (stride, offset) of a VRTF's position element when it is the four int16 kind (format 7)
inline bool VrtfPosition(Bytes b, uint32_t& stride, uint32_t& off) {
    if (!b.tag(0, "VRTF")) return false;
    stride = b.u32(8);
    const uint32_t n = b.u32(12), ext = b.u32(16);
    size_t o = 20;
    for (uint32_t i = 0; i < n; i++) {
        const uint32_t use = ext ? b.u32(o) : b.p[(b.need(o, 4), o)];
        const uint32_t fmt = ext ? b.u32(o + 8) : b.p[o + 2];
        const uint32_t at = ext ? b.u32(o + 12) : b.p[o + 3];
        o += ext ? 16 : 4;
        if (use == 0) return fmt == 7 ? (off = at, true) : false;
    }
    return false;
}

inline bool MaterialFloats(Bytes b, uint32_t name, float out[3]) {
    for (size_t p = 0; p + 4 <= b.n; p++) {
        if (!b.tag(p, "MTNF")) continue;
        size_t o = p + 16;
        for (uint32_t i = 0, n = b.u32(p + 12); i < n; i++, o += 16) {
            if (b.u32(o) != name || b.u32(o + 4) != 1 || b.u32(o + 8) < 3) continue;
            for (int k = 0; k < 3; k++) out[k] = b.f32(p + b.u32(o + 12) + 4 * k);
            return true;
        }
        return false;
    }
    return false;
}

// Appends one shadow-LOD mesh: 8 bytes a vertex, four int16 through its material's scale and offset
inline void AppendShadow(const Rcol& r, const MlodMesh& m, Mesh& out) {
    uint32_t k, i;
    if (!Rcol::Ref(m.mat, k, i)) return;
    float sc[3], of[3];
    const Bytes mat = r.Chunk(k, i);
    if (!MaterialFloats(mat, kOptScale, sc) || !MaterialFloats(mat, kOptOffset, of)) throw std::out_of_range("shadow material");
    Rcol::Ref(m.vbuf, k, i);
    const Bytes vb = r.Chunk(k, i);
    Rcol::Ref(m.ibuf, k, i);
    const std::vector<int32_t> idx = Indices(r.Chunk(k, i));
    const uint32_t base = static_cast<uint32_t>(out.pos.size() / 3);
    for (uint32_t v = 0; v < m.vc; v++) {
        const size_t at = 16 + static_cast<size_t>(m.sv + v) * 8 + m.soff;
        for (int c = 0; c < 3; c++) out.pos.push_back(static_cast<float>(vb.i16(at + 2 * c) * static_cast<double>(sc[c]) + of[c]));
    }
    for (size_t t = m.si; t + 3 <= std::min<size_t>(idx.size(), m.si + static_cast<size_t>(m.pc) * 3); t += 3)
        if (static_cast<uint32_t>(idx[t]) < m.vc && static_cast<uint32_t>(idx[t + 1]) < m.vc && static_cast<uint32_t>(idx[t + 2]) < m.vc)
            for (int c = 0; c < 3; c++) out.tri.push_back(base + idx[t + c]);
}

// Appends one LOD 0 mesh (a triangle list, not the drop shadow, with int16 x4 positions divided by the fourth)
inline void AppendLod0(const Rcol& r, const MlodMesh& m, Mesh& out) {
    uint32_t k, i, stride, off;
    if ((m.flags & 0xFF) != 3 || (m.flags & kDropShadow) || !m.vrtf || !m.vbuf || !m.ibuf) return;
    Rcol::Ref(m.vrtf, k, i);
    if (!VrtfPosition(r.Chunk(k, i), stride, off)) return;
    Rcol::Ref(m.vbuf, k, i);
    const Bytes vb = r.Chunk(k, i);
    Rcol::Ref(m.ibuf, k, i);
    const std::vector<int32_t> idx = Indices(r.Chunk(k, i));
    const uint32_t base = static_cast<uint32_t>(out.pos.size() / 3);
    for (uint32_t v = 0; v < m.vc; v++) {
        const size_t at = 16 + m.soff + static_cast<size_t>(m.sv + v) * stride + off;
        const int16_t w = vb.i16(at + 6);
        const double d = w ? w : 32767;
        for (int c = 0; c < 3; c++) out.pos.push_back(static_cast<float>(vb.i16(at + 2 * c) / d));
    }
    for (size_t t = m.si; t + 3 <= std::min<size_t>(idx.size(), m.si + static_cast<size_t>(m.pc) * 3); t += 3)
        if (static_cast<uint32_t>(idx[t]) < m.vc && static_cast<uint32_t>(idx[t + 1]) < m.vc && static_cast<uint32_t>(idx[t + 2]) < m.vc)
            for (int c = 0; c < 3; c++) out.tri.push_back(base + idx[t + c]);
}

class Library {
public:
    struct Stats {
        uint32_t packages = 0, unreadablePackages = 0, vpxys = 0, links = 0, unreadableVpxys = 0;
    };

    // Index a folder's packages: game = only FullBuild* / DeltaBuild* (Deltas after Fulls); else every *.package. Call the
    // game folder first, then Mods: a later key replaces an earlier one.
    void AddFolder(const std::filesystem::path& folder, bool game) {
        std::vector<std::filesystem::path> packs;
        std::error_code ec;
        for (auto it = std::filesystem::recursive_directory_iterator(folder, std::filesystem::directory_options::skip_permission_denied, ec);
             !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
            const std::filesystem::path& p = it->path();
            std::string ext = p.extension().string(), name = p.filename().string();
            std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            if (ext != ".package" || !it->is_regular_file(ec)) continue;
            if (game && name.rfind("FullBuild", 0) != 0 && name.rfind("DeltaBuild", 0) != 0) continue;
            packs.push_back(p);
        }
        const auto lower = [](const std::filesystem::path& p) {
            std::string s = p.string();
            std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
            return s;
        };
        std::sort(packs.begin(), packs.end(), [&](const std::filesystem::path& a, const std::filesystem::path& b) {
            const bool da = a.filename().string().rfind("DeltaBuild", 0) == 0, db = b.filename().string().rfind("DeltaBuild", 0) == 0;
            return da != db ? db : lower(a) < lower(b);
        });
        for (const auto& p : packs) IndexPackage(p);
    }

    // After the folders: every VPXY's LITE -> MODL link (both in the index). Returns the time-free counts.
    const Stats& BuildLinks() {
        liteToModl_.clear();
        for (const auto& [key, at] : index_) {
            if (key.type != kVpxy) continue;
            stats_.vpxys++;
            std::vector<uint8_t> data;
            try {
                if (!Read(key, data)) continue;
                const Rcol r = ReadRcol({data.data(), data.size()});
                if (r.chunks.empty()) continue;
                const Bytes b = r.chunks[0];
                const size_t p = 12 + static_cast<size_t>(b.u32(8));
                const Key* lite = nullptr;
                const Key* modl = nullptr;
                std::vector<Key> links(std::min<uint32_t>(b.u32(p), 4096));
                for (size_t i = 0; i < links.size(); i++) {
                    const size_t q = p + 4 + 16 * i;
                    links[i] = {b.u32(q), b.u32(q + 4), static_cast<uint64_t>(b.u32(q + 12)) << 32 | b.u32(q + 8)};
                    if (!index_.count(links[i])) continue;
                    if (!lite && links[i].type == kLite) lite = &links[i];
                    if (!modl && links[i].type == kModl) modl = &links[i];
                }
                if (lite && modl) {
                    stats_.links++;
                    liteToModl_.emplace(lite->inst, *modl); // the first VPXY of a LITE keeps it (casters.py / generate.py order)
                }
            } catch (const std::exception&) {
                stats_.unreadableVpxys++;
            }
        }
        return stats_;
    }

    const Key* ModelOfLite(uint64_t liteInstance) const {
        const auto it = liteToModl_.find(liteInstance);
        return it == liteToModl_.end() ? nullptr : &it->second;
    }
    size_t LinkCount() const { return liteToModl_.size(); }
    const Stats& GetStats() const { return stats_; }
    template <class F> void ForEachKey(uint32_t type, F&& f) const {
        for (const auto& [key, at] : index_)
            if (key.type == type) f(key);
    }

    // A MODL's caster mesh: its shadow LOD, else its LOD 0 (shadow = false: LOD 0 only). False when neither reads.
    bool Load(const Key& modl, Mesh& out, bool shadow = true) {
        out = {};
        std::vector<uint8_t> data, extData;
        try {
            if (!Read(modl, data)) return false;
            const Rcol r = ReadRcol({data.data(), data.size()});
            const Bytes* mb = r.First("MODL");
            if (!mb) return false;
            std::map<uint32_t, uint32_t> lods; // id -> reference
            const uint32_t n = mb->u32(8);
            if (n > 64 || mb->n < 20 * static_cast<size_t>(n)) return false;
            for (uint32_t i = 0; i < n; i++) {
                const size_t o = mb->n - 20 * static_cast<size_t>(n) + 20 * i;
                lods[mb->u32(o + 8)] = mb->u32(o);
            }
            const auto take = [&](uint32_t ref, Source src) {
                uint32_t k, i;
                if (!Rcol::Ref(ref, k, i)) return false;
                Rcol ext;
                const Rcol* at = &r;
                Bytes mlod;
                if (k < 2) {
                    mlod = r.Chunk(k, i);
                } else {
                    if (i >= r.ext.size() || !Read(r.ext[i], extData)) return false;
                    ext = ReadRcol({extData.data(), extData.size()});
                    const Bytes* m = ext.First("MLOD");
                    if (!m) return false;
                    at = &ext, mlod = *m;
                }
                Mesh m;
                for (const MlodMesh& mm : MlodMeshes(mlod)) {
                    if (src == Source::Shadow && mm.vrtf == 0 && mm.mat) AppendShadow(*at, mm, m);
                    if (src == Source::Lod0) AppendLod0(*at, mm, m);
                }
                if (m.tri.empty()) return false;
                m.source = src, out = std::move(m);
                return true;
            };
            if (shadow)
                for (uint32_t id : kShadowLods)
                    if (lods.count(id)) {
                        try {
                            if (take(lods[id], Source::Shadow)) return true;
                        } catch (const std::exception&) { // an unreadable shadow LOD falls back to LOD 0
                        }
                        break; // (casters.py takes the first shadow LOD present)
                    }
            return lods.count(0) && take(lods[0], Source::Lod0);
        } catch (const std::exception&) {
            out = {};
            return false;
        }
    }

    // One resource's bytes, decompressed; false when missing or unreadable
    bool Read(const Key& key, std::vector<uint8_t>& out) {
        const auto it = index_.find(key);
        if (it == index_.end()) return false;
        const Entry& e = it->second;
        std::ifstream& f = files_[e.file];
        if (!f.is_open()) f.open(paths_[e.file], std::ios::binary);
        std::vector<uint8_t> raw(e.size);
        f.clear();
        f.seekg(e.offset);
        if (!f.read(reinterpret_cast<char*>(raw.data()), raw.size())) return false;
        if (!e.compressed) return out.swap(raw), true;
        try {
            out = Refpack({raw.data(), raw.size()}, e.memSize);
        } catch (const std::exception&) {
            return false;
        }
        return true;
    }

private:
    struct Entry {
        uint32_t file, offset, size, memSize;
        bool compressed;
    };
    std::map<Key, Entry> index_;
    std::unordered_map<uint64_t, Key> liteToModl_;
    std::vector<std::filesystem::path> paths_;
    std::unordered_map<uint32_t, std::ifstream> files_;
    Stats stats_;

    // A DBPF 2.0 package's index; only the four types the chain needs are kept
    void IndexPackage(const std::filesystem::path& path) {
        stats_.packages++;
        std::ifstream f(path, std::ios::binary);
        uint8_t head[96] = {};
        if (!f.read(reinterpret_cast<char*>(head), sizeof head) || std::memcmp(head, "DBPF", 4) != 0) return void(stats_.unreadablePackages++);
        const Bytes h{head, sizeof head};
        const uint32_t count = h.u32(36), size = h.u32(44), pos = h.u32(64) ? h.u32(64) : h.u32(40);
        if (size > (256u << 20)) return void(stats_.unreadablePackages++);
        std::vector<uint8_t> data(size);
        f.seekg(pos);
        if (!f.read(reinterpret_cast<char*>(data.data()), size)) return void(stats_.unreadablePackages++);
        const uint32_t file = static_cast<uint32_t>(paths_.size());
        paths_.push_back(path);
        try {
            const Bytes d{data.data(), data.size()};
            const uint32_t flags = d.u32(0);
            size_t at = 4;
            uint32_t fixed[8] = {};
            for (int i = 0; i < 8; i++)
                if (flags >> i & 1) fixed[i] = d.u32(at), at += 4;
            for (uint32_t r = 0; r < count; r++) {
                uint32_t v[8];
                for (int i = 0; i < 8; i++) v[i] = (flags >> i & 1) ? fixed[i] : (at += 4, d.u32(at - 4));
                if (v[0] != kModl && v[0] != kLite && v[0] != kVpxy && v[0] != kMlod) continue;
                index_[{v[0], v[1], static_cast<uint64_t>(v[2]) << 32 | v[3]}] = {file, v[4], v[5] & 0x7FFFFFFF, v[6], (v[7] & 0xFFFF) == 0xFFFF};
            }
        } catch (const std::exception&) {
            stats_.unreadablePackages++;
        }
    }
};

} // namespace ts3mesh
