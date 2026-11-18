// Plants of custom content: tools/plant_puffs_table/generate.py's bake in C++ (features/plant_scan.h). The steps, constants and
// orders are the script's, so the same packages give the same balls (tools/plant_puffs_table/check.cpp).
#include "plant_scan.h"
#ifndef PLANT_SCAN_BAKE_ONLY // (tools/plant_puffs_table/check.cpp: the bake alone)
#include "apex_log.h"
#include "apex_paths.h"
#include "hook_guard.h"
#endif
#include "refpack_codec.h"
#include <windows.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <format>
#include <map>
#include <memory>
#include <mutex>

namespace {

constexpr uint32_t kModl = 0x01661233, kLite = 0x03B4C61D, kVpxy = 0x736884F1, kMlod = 0x01D10F34, kNmap = 0x0166038C;
constexpr uint32_t kOptScale = 0x487648E5, kOptOffset = 0x790EBF2C; // the shadow mesh's scale-offset material parameters
constexpr uint32_t kShadowLods[] = {0x10001, 0x10000};
constexpr double kSample = 0.004, kAreaPerPuff = 0.06, kRadius = 1.29, kTauMin = 0.1, kTauMax = 2.5, kDensity = 0.6;
constexpr double kSolidM = 0.05, kTauSolid = 6.0;
constexpr int kMaxK = 10, kMaxSolidK = 3;
// Custom content can be any bytes: nothing bigger is allocated (a 32-bit game), and a model past these is left out
constexpr uint32_t kMaxResource = 64u << 20;
constexpr size_t kMaxTris = 200000, kMaxSamples = 2000000;
const char* const kWords[] = {"plant", "palm", "fern", "ficus", "bonsai", "flower", "topiary", "cactus", "bamboo", "lotus", "papyrus",
                              "hosta", "lillies", "philodendron", "perennials", "planter", "coleus", "ivy", "bush", "shrub", "orchid",
                              "daffodil", "tulip", "grass", "reed", "cattail", "succulent", "leafy", "tree"};
const char* const kNot[] = {"food", "painting", "bookshelf", "sign", "yearbook", "decal", "frames", "holographic"};

struct Key {
    uint32_t t, g;
    uint64_t i;
    bool operator<(const Key& o) const { return t != o.t ? t < o.t : g != o.g ? g < o.g : i < o.i; }
};
struct Entry {
    size_t file;
    uint32_t off, size, mem;
    bool comp;
};
using Bytes = std::vector<uint8_t>;

template <class T> bool Get(const Bytes& b, size_t at, T& v) {
    if (at > b.size() || b.size() - at < sizeof(T)) return false; // (an offset from the file can be near SIZE_MAX)
    std::memcpy(&v, b.data() + at, sizeof(T));
    return true;
}
template <class T> T At(const Bytes& b, size_t at) { // (0 past the end: every caller's structure check then fails)
    T v{};
    Get(b, at, v);
    return v;
}

struct Packs {
    std::vector<std::wstring> files;
    std::map<Key, Entry> index;
    std::unordered_map<uint64_t, std::string>* names = nullptr;

    void Read(const std::vector<std::wstring>& paths) {
        for (const std::wstring& path : paths) try {
            FILE* f = _wfopen(path.c_str(), L"rb");
            if (!f) continue;
            uint8_t head[96] = {};
            long long length = -1;
            if (fread(head, 1, sizeof head, f) != sizeof head || std::memcmp(head, "DBPF", 4) != 0 || _fseeki64(f, 0, SEEK_END) != 0 ||
                (length = _ftelli64(f)) < 0) {
                fclose(f);
                continue;
            }
            uint32_t count, size, pos, pos2;
            std::memcpy(&count, head + 36, 4), std::memcpy(&size, head + 44, 4), std::memcpy(&pos, head + 64, 4), std::memcpy(&pos2, head + 40, 4);
            if (!pos) pos = pos2;
            // an index row is at most 32 bytes after a 36-byte head: a bigger size, or one past the file, is not an index
            if (size < 4 || count > 4000000 || size > 36ull + 32ull * count || static_cast<unsigned long long>(pos) + size > static_cast<unsigned long long>(length)) {
                fclose(f);
                continue;
            }
            Bytes data(size);
            if (_fseeki64(f, pos, SEEK_SET) != 0 || fread(data.data(), 1, size, f) != size) {
                fclose(f);
                continue;
            }
            fclose(f);
            const size_t file = files.size();
            files.push_back(path);
            const uint32_t flags = At<uint32_t>(data, 0);
            size_t at = 4;
            uint32_t fixed[8] = {};
            for (int i = 0; i < 8; i++)
                if (flags >> i & 1) fixed[i] = At<uint32_t>(data, at), at += 4;
            for (uint32_t r = 0; r < count && at <= data.size(); r++) {
                uint32_t v[8];
                for (int i = 0; i < 8; i++) v[i] = (flags >> i & 1) ? fixed[i] : (at += 4, At<uint32_t>(data, at - 4));
                const Key k{v[0], v[1], static_cast<uint64_t>(v[2]) << 32 | v[3]};
                const Entry e{file, v[4], v[5] & 0x7FFFFFFFu, v[6], (v[7] & 0xFFFF) == 0xFFFF};
                if (k.t == kModl || k.t == kLite || k.t == kVpxy || k.t == kMlod) index[k] = e;
                else if (k.t == kNmap && names) ReadNames(e);
            }
        } catch (...) { // (a file the checks above let through: the rest are read)
        }
    }
    bool Has(const Key& k) const { return index.count(k) != 0; }
    Bytes Load(const Entry& e) const {
        if (e.size > kMaxResource || (e.comp && e.mem > kMaxResource)) return {};
        Bytes raw(e.size);
        FILE* f = _wfopen(files[e.file].c_str(), L"rb");
        if (!f) return {};
        const bool ok = _fseeki64(f, e.off, SEEK_SET) == 0 && fread(raw.data(), 1, raw.size(), f) == raw.size();
        fclose(f);
        if (!ok) return {};
        if (!e.comp) return raw;
        Bytes out(e.mem);
        return RefPackCodec::Decompress(out.data(), e.mem, raw.data(), e.size) == e.mem ? out : Bytes{};
    }
    Bytes Load(const Key& k) const {
        const auto it = index.find(k);
        return it == index.end() ? Bytes{} : Load(it->second);
    }
    void ReadNames(const Entry& e) {
        const Bytes d = Load(e);
        uint32_t n = 0;
        if (!Get(d, 4, n)) return;
        size_t o = 8;
        for (uint32_t i = 0; i < n; i++) {
            uint64_t inst;
            uint32_t len;
            if (!Get(d, o, inst) || !Get(d, o + 8, len) || len > d.size() - o - 12) return; // (Get: o + 12 <= size)
            (*names)[inst] = std::string(reinterpret_cast<const char*>(d.data() + o + 12), len);
            o += 12 + len;
        }
    }
};

// An RCOL resource: its chunks (bytes), the public count, its external references
struct Rcol {
    std::vector<Bytes> chunks;
    uint32_t pub = 0;
    std::vector<Key> ext;
    bool Parse(const Bytes& d) {
        uint32_t h[5];
        if (d.size() < 20) return false;
        std::memcpy(h, d.data(), 20);
        pub = h[1];
        const uint32_t ne = h[3], ni = h[4];
        if (ne > 4096 || ni > 4096) return false;
        size_t o = 20;
        std::vector<Key> its;
        for (uint32_t i = 0; i < ni; i++, o += 16) its.push_back({At<uint32_t>(d, o + 8), At<uint32_t>(d, o + 12), At<uint64_t>(d, o)});
        for (uint32_t i = 0; i < ne; i++, o += 16) ext.push_back({At<uint32_t>(d, o + 8), At<uint32_t>(d, o + 12), At<uint64_t>(d, o)});
        for (uint32_t i = 0; i < ni; i++, o += 8) {
            const uint32_t p = At<uint32_t>(d, o), s = At<uint32_t>(d, o + 4);
            if (o + 8 > d.size() || p > d.size() || s > d.size() - p) return false;
            chunks.emplace_back(d.begin() + p, d.begin() + p + s);
        }
        return true;
    }
    const Bytes* First(const char magic[4]) const {
        for (const Bytes& c : chunks)
            if (c.size() >= 4 && std::memcmp(c.data(), magic, 4) == 0) return &c;
        return nullptr;
    }
    // generate.py chunk(): a reference's chunk (type 1: past the public ones)
    const Bytes* Chunk(uint32_t ref) const {
        if (!ref) return nullptr;
        const size_t i = (ref & 0x0FFFFFFFu) - 1 + ((ref >> 28) == 1 ? pub : 0);
        return i < chunks.size() ? &chunks[i] : nullptr;
    }
};

using P3 = std::array<double, 3>;
struct Tri {
    P3 a, b, c;
};
P3 Sub(const P3& a, const P3& b) { return {a[0] - b[0], a[1] - b[1], a[2] - b[2]}; }
P3 Cross(const P3& a, const P3& b) { return {a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]}; }
double Norm(const P3& a) { return std::sqrt(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]); }
double D2(const P3& a, const P3& b) { return (a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2]); }

// generate.py shadow_triangles: every shadow mesh of the model's shadow LOD (low first)
std::vector<Tri> ShadowTriangles(const Packs& pk, const Key& modl) {
    Rcol r;
    if (!r.Parse(pk.Load(modl))) return {};
    const Bytes* m = r.First("MODL");
    if (!m) return {};
    const uint32_t n = At<uint32_t>(*m, 8);
    if (n > 64 || 20ull * n > m->size()) return {};
    std::map<uint32_t, uint32_t> lods;
    for (uint32_t i = 0; i < n; i++) {
        const size_t o = m->size() - 20 * n + 20 * i;
        lods[At<uint32_t>(*m, o + 8)] = At<uint32_t>(*m, o); // (a later entry of the same id wins, as in the script's dict)
    }
    uint32_t want = 0;
    for (uint32_t id : kShadowLods)
        if (lods.count(id)) { want = lods[id]; break; }
    if (!want) return {};
    Rcol own;
    const Rcol* lod = &r;
    if ((want >> 28) >= 2) {
        const size_t e = (want & 0x0FFFFFFFu) - 1;
        if (e >= r.ext.size() || !pk.Has(r.ext[e]) || !own.Parse(pk.Load(r.ext[e]))) return {};
        lod = &own;
    }
    const Bytes* ml = lod->First("MLOD");
    if (!ml) return {};
    std::vector<Tri> tris;
    const uint32_t meshes = At<uint32_t>(*ml, 8);
    size_t o = 12;
    for (uint32_t k = 0; k < meshes && k < 256 && o + 4 <= ml->size(); k++) {
        const uint32_t size = At<uint32_t>(*ml, o);
        size_t p = o + 4;
        uint32_t f[12];
        for (int i = 0; i < 12; i++) f[i] = At<uint32_t>(*ml, p + 4 * i);
        const uint32_t vr = f[2], vb = f[3], ib = f[4], soff = f[6], sv = f[7], si = f[8], vc = f[10], pc = f[11];
        p += 72;
        p += 8 + 4 * static_cast<size_t>(At<uint32_t>(*ml, p + 4));
        const uint32_t so = At<uint32_t>(*ml, p);
        o += 4 + static_cast<size_t>(size);
        if (vr || !so) continue; // a shadow mesh has no vertex format and a shadow material
        // material floats (MTNF): scale and offset
        const Bytes* mat = lod->Chunk(so);
        const Bytes* vbuf = lod->Chunk(vb);
        const Bytes* ibuf = lod->Chunk(ib);
        if (!mat || !vbuf || !ibuf) continue;
        double sc[3] = {}, of[3] = {};
        bool haveS = false, haveO = false;
        for (size_t q = 0; q + 4 <= mat->size(); q++) {
            if (std::memcmp(mat->data() + q, "MTNF", 4) != 0) continue;
            size_t e = q + 16;
            const uint32_t cnt = At<uint32_t>(*mat, q + 12);
            for (uint32_t i = 0; i < cnt && i < 256; i++, e += 16) {
                const uint32_t h = At<uint32_t>(*mat, e), ty = At<uint32_t>(*mat, e + 4), c = At<uint32_t>(*mat, e + 8), off = At<uint32_t>(*mat, e + 12);
                if (ty != 1 || c < 3 || (h != kOptScale && h != kOptOffset)) continue;
                double* dst = h == kOptScale ? sc : of;
                for (int j = 0; j < 3; j++) dst[j] = At<float>(*mat, q + off + 4 * j);
                (h == kOptScale ? haveS : haveO) = true;
            }
            break;
        }
        if (!haveS || !haveO || vc > 1000000 || pc > 1000000) continue;
        bool finite = true;
        for (int j = 0; j < 3; j++) finite = finite && std::isfinite(sc[j]) && std::isfinite(of[j]) && std::fabs(sc[j]) < 1.0 && std::fabs(of[j]) < 1000.0;
        if (!finite) continue;
        std::vector<P3> pos(vc);
        for (uint32_t i = 0; i < vc; i++) {
            const size_t at = 16 + (static_cast<size_t>(sv) + i) * 8 + soff;
            for (int j = 0; j < 3; j++) pos[i][j] = At<int16_t>(*vbuf, at + 2 * j) * sc[j] + of[j];
        }
        // indices: 16-bit or 32-bit (flag 2), deltas (flag 1)
        const uint32_t fl = At<uint32_t>(*ibuf, 8);
        const size_t w = (fl & 2) ? 4 : 2, total = ibuf->size() > 16 ? (ibuf->size() - 16) / w : 0;
        std::vector<int64_t> idx;
        idx.reserve(std::min<size_t>(total, static_cast<size_t>(si) + 3ull * pc));
        int64_t cur = 0;
        for (size_t i = 0; i < total && i < static_cast<size_t>(si) + 3ull * pc; i++) {
            const int64_t v = w == 4 ? At<int32_t>(*ibuf, 16 + 4 * i) : At<int16_t>(*ibuf, 16 + 2 * i);
            idx.push_back((fl & 1) ? (cur += v) : v);
        }
        for (size_t t = si; t + 2 < idx.size() && t < static_cast<size_t>(si) + 3ull * pc; t += 3) {
            const int64_t a = idx[t], b = idx[t + 1], c = idx[t + 2];
            if (a < 0 || b < 0 || c < 0 || a >= vc || b >= vc || c >= vc) continue;
            tris.push_back({pos[static_cast<size_t>(a)], pos[static_cast<size_t>(b)], pos[static_cast<size_t>(c)]});
            if (tris.size() > kMaxTris) return {};
        }
    }
    return tris;
}

// generate.py solid_pieces: each triangle's piece at least kSolidM thick
std::vector<bool> SolidPieces(const std::vector<Tri>& tris) {
    std::map<std::array<int64_t, 3>, int> id;
    std::vector<int> parent;
    const auto key = [](const P3& q) { return std::array<int64_t, 3>{static_cast<int64_t>(std::nearbyint(q[0] * 1000)), static_cast<int64_t>(std::nearbyint(q[1] * 1000)), static_cast<int64_t>(std::nearbyint(q[2] * 1000))}; };
    const auto node = [&](const P3& q) {
        const auto [it, made] = id.emplace(key(q), static_cast<int>(parent.size()));
        if (made) parent.push_back(it->second);
        return it->second;
    };
    const auto find = [&](int x) {
        while (parent[x] != x) parent[x] = parent[parent[x]], x = parent[x];
        return x;
    };
    std::vector<std::array<int, 3>> v;
    for (const Tri& t : tris) v.push_back({node(t.a), node(t.b), node(t.c)});
    for (const auto& n : v) {
        const int ra = find(n[0]);
        parent[find(n[1])] = ra;
        parent[find(n[2])] = ra;
    }
    std::vector<double> area(parent.size(), 0.0), vol(parent.size(), 0.0);
    for (size_t i = 0; i < tris.size(); i++) {
        const P3 &a = tris[i].a, &b = tris[i].b, &c = tris[i].c;
        const int r = find(v[i][0]);
        area[r] += 0.5 * Norm(Cross(Sub(b, a), Sub(c, a)));
        vol[r] += (a[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (b[0] * c[2] - b[2] * c[0]) + a[2] * (b[0] * c[1] - b[1] * c[0])) / 6.0;
    }
    std::vector<bool> out(tris.size());
    for (size_t i = 0; i < tris.size(); i++) {
        const int r = find(v[i][0]);
        out[i] = 3.0 * std::fabs(vol[r]) / std::max(area[r], 1e-9) >= kSolidM;
    }
    return out;
}

struct Sample {
    P3 p;
    double w;
};
std::vector<Sample> Samples(const std::vector<Tri>& tris, double& total) {
    std::vector<Sample> out;
    total = 0.0;
    for (const Tri& t : tris) {
        const P3 ab = Sub(t.b, t.a), ac = Sub(t.c, t.a);
        const double area = 0.5 * Norm(Cross(ab, ac));
        if (!(area > 0.0)) continue;
        total += area;
        const int n = std::max(1, static_cast<int>(std::ceil(std::sqrt(area / kSample))));
        if (n > 512 || out.size() > kMaxSamples) return {}; // (a malformed mesh: the model is left out)
        const double w = area / (n * (n + 1) / 2.0);
        for (int i = 0; i < n; i++)
            for (int j = 0; j < n - i; j++) {
                const double u = (i + 1.0 / 3) / n, v = (j + 1.0 / 3) / n;
                out.push_back({{t.a[0] + ab[0] * u + ac[0] * v, t.a[1] + ab[1] * u + ac[1] * v, t.a[2] + ab[2] * u + ac[2] * v}, w});
            }
    }
    return out;
}

// generate.py kmeans and balls
void Balls(const std::vector<Tri>& tris, bool solid, int maxK, std::vector<ObjectLegs::Puff>& out) {
    double total = 0.0;
    const std::vector<Sample> pts = Samples(tris, total);
    if (pts.empty()) return;
    const int k = std::max(1, std::min({maxK, static_cast<int>(pts.size()), static_cast<int>(std::nearbyint(total / kAreaPerPuff))}));
    double sw = 0.0;
    P3 cx{};
    for (const Sample& s : pts) sw += s.w;
    for (int j = 0; j < 3; j++) {
        double v = 0.0;
        for (const Sample& s : pts) v += s.p[j] * s.w;
        cx[j] = v / sw;
    }
    std::vector<P3> centres;
    {
        size_t best = 0;
        for (size_t i = 1; i < pts.size(); i++)
            if (D2(pts[i].p, cx) < D2(pts[best].p, cx)) best = i;
        centres.push_back(pts[best].p);
    }
    while (static_cast<int>(centres.size()) < k) {
        size_t best = 0;
        double bestD = -1.0;
        for (size_t i = 0; i < pts.size(); i++) {
            double d = 1e300;
            for (const P3& c : centres) d = std::min(d, D2(pts[i].p, c));
            if (d > bestD) bestD = d, best = i;
        }
        centres.push_back(pts[best].p);
    }
    std::vector<int> group(pts.size());
    for (int it = 0; it < 40; it++) {
        for (size_t i = 0; i < pts.size(); i++) {
            int g = 0;
            for (int c = 1; c < static_cast<int>(centres.size()); c++)
                if (D2(pts[i].p, centres[c]) < D2(pts[i].p, centres[g])) g = c;
            group[i] = g;
        }
        std::vector<P3> next = centres;
        for (size_t c = 0; c < centres.size(); c++) {
            double w = 0.0;
            P3 m{};
            for (size_t i = 0; i < pts.size(); i++)
                if (group[i] == static_cast<int>(c)) {
                    w += pts[i].w;
                    for (int j = 0; j < 3; j++) m[j] += pts[i].p[j] * pts[i].w;
                }
            if (w > 0.0) next[c] = {m[0] / w, m[1] / w, m[2] / w};
        }
        if (next == centres) break;
        centres = next;
    }
    for (size_t c = 0; c < centres.size(); c++) {
        double area = 0.0, s2 = 0.0;
        for (size_t i = 0; i < pts.size(); i++)
            if (group[i] == static_cast<int>(c)) area += pts[i].w, s2 += D2(pts[i].p, centres[c]) * pts[i].w;
        if (!(area > 0.0)) continue;
        const double r = std::max(0.03, kRadius * std::sqrt(s2 / area));
        const double tau = solid ? kTauSolid : std::clamp(kDensity * 1.5 * (area / 2.0) / (3.14159265358979323846 * r * r), kTauMin, kTauMax);
        const auto mm = [](double v) { return static_cast<int16_t>(std::clamp(std::nearbyint(v * 1000.0), -32768.0, 32767.0)); };
        out.push_back({mm(centres[c][0]), mm(centres[c][1]), mm(centres[c][2]), mm(r), static_cast<uint16_t>(std::nearbyint(tau * 1000.0))});
    }
}

bool IsPlant(std::string name) {
    std::transform(name.begin(), name.end(), name.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (name.rfind("table", 0) == 0 || name.rfind("column", 0) == 0) return false;
    for (const char* w : kNot)
        if (name.find(w) != std::string::npos) return false;
    for (const char* w : kWords)
        if (name.find(w) != std::string::npos) return true;
    return false;
}

#ifndef PLANT_SCAN_BAKE_ONLY
std::mutex g_mx;
std::shared_ptr<const std::unordered_map<uint64_t, std::vector<ObjectLegs::Puff>>> g_found; // (under g_mx) set once, when the scan ends
std::atomic<bool> g_started{false}, g_done{false};

// (as the game's Resource.cfg reads Mods\Packages: a few folders deep; a junction or link is not followed, so a loop cannot run away)
void Collect(const std::wstring& dir, const wchar_t* pattern, int depth, std::vector<std::wstring>& out) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        const std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            if (depth > 0 && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) Collect(dir + name + L"\\", pattern, depth - 1, out);
        } else if (name.size() > wcslen(pattern) && _wcsicmp(name.c_str() + name.size() - wcslen(pattern), pattern) == 0)
            out.push_back(dir + name);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

#endif
} // namespace

namespace PlantScan {

void Bake(const std::vector<std::wstring>& files, std::unordered_map<uint64_t, std::vector<ObjectLegs::Puff>>& out,
          std::unordered_map<uint64_t, std::string>* names) {
    Packs pk;
    std::unordered_map<uint64_t, std::string> modelNames;
    pk.names = &modelNames;
    pk.Read(files);
    // (the script's order, by key, decides which VPXY gives a LITE its balls; the reads happen in file order first, so a large
    // DCCache is read forward, not at random)
    std::vector<std::pair<Key, Entry>> vpxy;
    for (const auto& [key, entry] : pk.index)
        if (key.t == kVpxy) vpxy.push_back({key, entry});
    std::vector<Bytes> loaded(vpxy.size());
    {
        std::vector<size_t> order(vpxy.size());
        for (size_t i = 0; i < order.size(); i++) order[i] = i;
        std::sort(order.begin(), order.end(), [&](size_t a, size_t b) {
            return vpxy[a].second.file != vpxy[b].second.file ? vpxy[a].second.file < vpxy[b].second.file : vpxy[a].second.off < vpxy[b].second.off;
        });
        for (size_t i : order) try { loaded[i] = pk.Load(vpxy[i].second); } catch (...) {}
    }
    for (size_t v = 0; v < vpxy.size(); v++) try {
        const Key& key = vpxy[v].first;
        Rcol vp;
        if (!vp.Parse(loaded[v]) || vp.chunks.empty()) continue;
        Bytes().swap(loaded[v]);
        const Bytes& b = vp.chunks[0];
        const uint32_t off = At<uint32_t>(b, 8);
        const size_t p = 12 + static_cast<size_t>(off);
        const uint32_t count = At<uint32_t>(b, p);
        Key lite{}, modl{};
        bool haveLite = false, haveModl = false;
        for (uint32_t i = 0; i < count && i < 1024; i++) {
            const size_t q = p + 4 + 16 * static_cast<size_t>(i);
            const Key k{At<uint32_t>(b, q), At<uint32_t>(b, q + 4), static_cast<uint64_t>(At<uint32_t>(b, q + 12)) << 32 | At<uint32_t>(b, q + 8)};
            if (!haveLite && k.t == kLite && pk.Has(k)) lite = k, haveLite = true;
            if (!haveModl && k.t == kModl && pk.Has(k)) modl = k, haveModl = true;
        }
        if (!haveLite || !haveModl || out.count(lite.i)) continue;
        Rcol lr;
        if (!lr.Parse(pk.Load(lite)) || lr.chunks.empty()) continue;
        const Bytes& l = lr.chunks[0];
        if (l.size() < 16 || std::memcmp(l.data(), "LITE", 4) != 0) continue;
        size_t o = 16 + 128 * static_cast<size_t>(l[12]);
        const int occ = l[13];
        if (!occ) continue; // no blocker in the game: nothing to replace
        std::vector<P3> upright;
        for (int i = 0; i < occ; i++, o += 56) {
            const P3 n{At<float>(l, o + 4 + 12), At<float>(l, o + 4 + 16), At<float>(l, o + 4 + 20)};
            if (std::fabs(n[1]) < 0.1) upright.push_back(n);
        }
        const bool crossed = occ == 2 && upright.size() == 2 && std::fabs(upright[0][0] * upright[1][0] + upright[0][1] * upright[1][1] + upright[0][2] * upright[1][2]) < 0.2;
        const auto nm = modelNames.find(key.i);
        if (!crossed && !(nm != modelNames.end() && IsPlant(nm->second))) continue;
        const std::vector<Tri> tris = ShadowTriangles(pk, modl);
        if (tris.empty()) continue;
        const std::vector<bool> solid = SolidPieces(tris);
        std::vector<Tri> hard, soft;
        for (size_t i = 0; i < tris.size(); i++) (solid[i] ? hard : soft).push_back(tris[i]);
        std::vector<ObjectLegs::Puff> puffs;
        Balls(hard, true, kMaxSolidK, puffs);
        Balls(soft, false, kMaxK - static_cast<int>(puffs.size()), puffs);
        if (puffs.empty()) continue;
        out[lite.i] = std::move(puffs);
        if (names) (*names)[lite.i] = nm != modelNames.end() ? nm->second : std::string();
    } catch (...) { // (a model the checks let through: the others are baked)
    }
}

#ifndef PLANT_SCAN_BAKE_ONLY
void Start() {
    if (g_started.exchange(true)) return;
    const std::wstring docs = ApexPaths::GameDocumentsDirectory();
    if (docs.empty()) return;
    HookGuard::StartDetached("Plant shadows: custom content", [docs] {
        const auto t0 = std::chrono::steady_clock::now();
        std::vector<std::wstring> files;
        SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN); // (low CPU and disk priority while the game loads)
        Collect(docs + L"Mods\\", L".package", 6, files);
        Collect(docs + L"DCCache\\", L".dbc", 0, files);
        auto found = std::make_shared<std::unordered_map<uint64_t, std::vector<ObjectLegs::Puff>>>();
        Bake(files, *found);
        size_t balls = 0, known = 0;
        for (auto it = found->begin(); it != found->end();)
            if (ObjectLegs::BakedPlant(it->first)) it = found->erase(it), known++; // the table's own wins
            else balls += it++->second.size();
        const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        LOG_INFO(std::format("[ObjectLegs] Custom content: {} files, {} plant models baked ({} balls; {} already in the table) in {:.0f} ms",
                             files.size(), found->size(), balls, known, ms));
        {
            std::lock_guard<std::mutex> lk(g_mx);
            g_found = std::move(found);
        }
        g_done.store(true, std::memory_order_release);
        ObjectLegs::Touch(); // kept rooms read their records again (OutdoorDelta's LiveTests): plants solved meanwhile get their balls
    });
}

bool Done() { return g_done.load(std::memory_order_acquire); }

bool Find(uint64_t lite, const ObjectLegs::Puff*& first, uint16_t& count) {
    std::shared_ptr<const std::unordered_map<uint64_t, std::vector<ObjectLegs::Puff>>> found;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        found = g_found;
    }
    if (!found) return false;
    const auto it = found->find(lite);
    if (it == found->end()) return false;
    first = it->second.data(), count = static_cast<uint16_t>(it->second.size());
    return true;
}

#endif
} // namespace PlantScan
