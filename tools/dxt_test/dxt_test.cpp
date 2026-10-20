// Offline check of features/dxt_codec.{h,cpp}: the fast four-blocks-at-a-time DXT1 / DXT5 encoder against the reference
// translation of The Sims 3's encoder (TS3W.exe 0x006152F0 / 0x006154B0), over millions of generated blocks (random,
// solid, two-colour, gradients, near-flat noise, equal-luma colours, alpha extremes, saturated colours), every edge size
// (width / height not multiples of 4), both x87 precision settings the game's threads use, and optionally BMP images.
// Also the several-cores path (DxtCodec::Parallel): serial fast vs parallel, byte for byte over the whole destination
// (padding included), on random images of many sizes (non-multiples of 4, 1-pixel strips, padded source and
// destination rows), 1..6 workers, both x87 precisions and a non-default MXCSR (round toward zero, FTZ, DAZ: the workers
// must take the caller's), plus a stress run of thousands of tiny splits and several threads encoding at once (pool
// hand-off, busy pool). Times both encoders on a 1024 x 1024 image and serial vs parallel on 1024 / 2048. Console output
// only; writes no files.
//
// The reference is checked against the game itself in game: the ASI's development build encodes 1 texture in N (and
// the first 16 of every session in both builds) with the game's own function too and compares the bytes
// (Developer > Profiler > Performance).
//
// Build (x86: the code uses x87 inline assembly), from "x86 Native Tools Command Prompt for VS 2022" or after running
//   "C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools\VC\Auxiliary\Build\vcvars32.bat":
//   cd /d <repo>\tools\dxt_test
//   cl /nologo /O2 /EHsc /std:c++20 /arch:SSE2 /fp:precise /I..\..\features dxt_test.cpp ..\..\features\dxt_codec.cpp /Fe:dxt_test.exe
// Run:
//   dxt_test.exe                        10 million blocks per format, edge sizes, several cores, timing
//   dxt_test.exe --blocks 1000000       fewer blocks
//   dxt_test.exe --parallel-only        only the several-cores tests and their timing
//   dxt_test.exe --bmp C:\path\a.bmp    also an image (24 / 32-bit uncompressed BMP, read only; repeat --bmp for more)
//   dxt_test.exe --seed 123             another random sequence
// Exit code 0 = no difference.
//
// Part of Apex Radiance. Credits: @loinyx
#include "dxt_codec.h"
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <emmintrin.h>
#include <float.h>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

using namespace DxtCodec;

namespace {

struct Rng {
    uint64_t s;
    uint32_t Next() {
        s ^= s >> 12;
        s ^= s << 25;
        s ^= s >> 27;
        return static_cast<uint32_t>((s * 2685821657736338717ull) >> 32);
    }
    uint32_t Below(uint32_t n) { return n ? Next() % n : 0; }
};

double NowMs() {
    static LARGE_INTEGER f = [] {
        LARGE_INTEGER x;
        QueryPerformanceFrequency(&x);
        return x;
    }();
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    return 1000.0 * static_cast<double>(q.QuadPart) / static_cast<double>(f.QuadPart);
}

uint32_t Pack(int r, int g, int b, int a) {
    auto c = [](int v) { return static_cast<uint32_t>(v < 0 ? 0 : (v > 255 ? 255 : v)); };
    return (c(a) << 24) | (c(r) << 16) | (c(g) << 8) | c(b);
}

enum Kind { kRandom, kSolid, kTwoColour, kGradient, kNearFlat, kEqualLuma, kAlphaExtremes, kSaturated, kKinds };
const char* const kKindName[kKinds] = {"random", "solid", "two colours", "gradient", "near-flat noise", "equal luma", "alpha extremes", "saturated"};

int RandomAlpha(Rng& r) {
    switch (r.Below(4)) {
    case 0: return 255;
    case 1: return static_cast<int>(r.Below(256));
    case 2: return 128 + static_cast<int>(r.Below(5)) - 2;
    default: return static_cast<int>(r.Below(2)) * 255;
    }
}

// 16 pixels (row-major) of one block of the given kind
void FillBlock(Rng& r, Kind k, uint32_t px[16]) {
    const int br = static_cast<int>(r.Below(256)), bg = static_cast<int>(r.Below(256)), bb = static_cast<int>(r.Below(256));
    const int alphaMode = static_cast<int>(r.Below(3)); // 0: opaque, 1: one random alpha, 2: per pixel
    const int a0 = RandomAlpha(r);
    auto alpha = [&](Rng& rr) { return alphaMode == 0 ? 255 : (alphaMode == 1 ? a0 : RandomAlpha(rr)); };
    switch (k) {
    case kRandom:
        for (int i = 0; i < 16; i++) px[i] = r.Next();
        break;
    case kSolid:
        for (int i = 0; i < 16; i++) px[i] = Pack(br, bg, bb, alphaMode == 2 ? RandomAlpha(r) : a0);
        break;
    case kTwoColour: {
        const uint32_t c0 = r.Next(), c1 = r.Next();
        for (int i = 0; i < 16; i++) px[i] = (r.Next() & 1) ? c0 : c1;
        break;
    }
    case kGradient: {
        const int dx[3] = {static_cast<int>(r.Below(33)) - 16, static_cast<int>(r.Below(33)) - 16, static_cast<int>(r.Below(33)) - 16};
        const int dy[3] = {static_cast<int>(r.Below(33)) - 16, static_cast<int>(r.Below(33)) - 16, static_cast<int>(r.Below(33)) - 16};
        for (int y = 0; y < 4; y++)
            for (int x = 0; x < 4; x++) px[y * 4 + x] = Pack(br + dx[0] * x + dy[0] * y, bg + dx[1] * x + dy[1] * y, bb + dx[2] * x + dy[2] * y, alpha(r));
        break;
    }
    case kNearFlat: {
        const int amp = 1 + static_cast<int>(r.Below(3));
        for (int i = 0; i < 16; i++)
            px[i] = Pack(br + static_cast<int>(r.Below(2 * amp + 1)) - amp, bg + static_cast<int>(r.Below(2 * amp + 1)) - amp, bb + static_cast<int>(r.Below(2 * amp + 1)) - amp, alpha(r));
        break;
    }
    case kEqualLuma: { // moves along (0.59, -0.3, 0) / (0.11, 0, -0.3) keep 0.3R + 0.59G + 0.11B almost constant
        for (int i = 0; i < 16; i++) {
            const int t = static_cast<int>(r.Below(33)) - 16, u = static_cast<int>(r.Below(17)) - 8;
            px[i] = Pack(br + (59 * t + 11 * u) / 30, bg - t, bb - u, alpha(r));
        }
        break;
    }
    case kAlphaExtremes:
        for (int i = 0; i < 16; i++) {
            const int a = r.Below(3) == 0 ? static_cast<int>(r.Below(256)) : static_cast<int>(r.Below(2)) * 255;
            px[i] = (r.Next() & 0x00FFFFFFu) | (static_cast<uint32_t>(a) << 24);
        }
        break;
    case kSaturated:
        for (int i = 0; i < 16; i++) {
            const uint32_t m = r.Next();
            px[i] = Pack((m & 1) ? 255 - static_cast<int>(r.Below(3)) : static_cast<int>(r.Below(3)), (m & 2) ? 255 : 0, (m & 4) ? 255 : static_cast<int>(r.Below(2)), alpha(r));
        }
        break;
    default:
        break;
    }
}

struct Fallback {
    uint64_t calls = 0;
};
void RefBlock(void* ctx, bool dxt5, const uint8_t* src, int32_t pitch, uint32_t cols, uint32_t rows, uint32_t format, uint8_t* out) {
    static_cast<Fallback*>(ctx)->calls++;
    Ref::EncodeBlock(nullptr, dxt5, src, pitch, cols, rows, format, out);
}

std::string Hex(const uint8_t* p, size_t n) {
    std::string s;
    char b[4];
    for (size_t i = 0; i < n; i++) {
        std::snprintf(b, sizeof b, "%02x", p[i]);
        s += b;
    }
    return s;
}

// Encodes the image with both encoders; counts blocks whose bytes differ (and prints the first few)
struct Compare {
    uint64_t blocks = 0, diffs = 0;
    int printed = 0;
};
void Run(bool dxt5, const std::vector<uint32_t>& img, uint32_t w, uint32_t h, Compare& c, Fallback& fb, const Kind* kinds, uint32_t kindStride, uint64_t* diffPerKind) {
    const uint32_t bs = dxt5 ? 16 : 8, bx = (w + 3) / 4, by = (h + 3) / 4, pitch = bx * bs;
    std::vector<uint8_t> a(static_cast<size_t>(pitch) * by, 0xCD), b(static_cast<size_t>(pitch) * by, 0xCD);
    Src s{reinterpret_cast<const uint8_t*>(img.data()), 0, 0, static_cast<int32_t>(w * 4), 0x3D};
    Dst da{a.data(), w, h, pitch, 0}, db{b.data(), w, h, pitch, 0};
    FastCounters fc;
    if (dxt5) {
        Ref::EncodeDxt5(&da, &s);
        Fast::EncodeDxt5(&db, &s, &RefBlock, &fb, &fc);
    } else {
        Ref::EncodeDxt1(&da, &s);
        Fast::EncodeDxt1(&db, &s, &RefBlock, &fb, &fc);
    }
    for (uint32_t y = 0; y < by; y++)
        for (uint32_t x = 0; x < bx; x++) {
            c.blocks++;
            const size_t o = static_cast<size_t>(y) * pitch + static_cast<size_t>(x) * bs;
            if (std::memcmp(a.data() + o, b.data() + o, bs) == 0) continue;
            c.diffs++;
            if (kinds && diffPerKind) diffPerKind[kinds[y * kindStride + x]]++;
            if (c.printed < 8) {
                c.printed++;
                std::printf("  DIFF DXT%d %ux%u block (%u,%u)%s%s: ref %s  fast %s\n    pixels:", dxt5 ? 5 : 1, w, h, x, y, kinds ? " kind " : "", kinds ? kKindName[kinds[y * kindStride + x]] : "",
                            Hex(a.data() + o, bs).c_str(), Hex(b.data() + o, bs).c_str());
                for (uint32_t r = 0; r < 4 && 4 * y + r < h; r++)
                    for (uint32_t q = 0; q < 4 && 4 * x + q < w; q++) std::printf(" %08x", img[static_cast<size_t>(4 * y + r) * w + 4 * x + q]);
                std::printf("\n");
            }
        }
}

bool LoadBmp(const char* path, std::vector<uint32_t>& img, uint32_t& w, uint32_t& h) {
    FILE* f = nullptr;
    if (fopen_s(&f, path, "rb") != 0 || !f) return false;
    std::vector<uint8_t> d;
    uint8_t buf[65536];
    size_t n;
    while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) d.insert(d.end(), buf, buf + n);
    std::fclose(f);
    if (d.size() < 54 || d[0] != 'B' || d[1] != 'M') return false;
    auto u32 = [&](size_t o) { return static_cast<uint32_t>(d[o]) | (static_cast<uint32_t>(d[o + 1]) << 8) | (static_cast<uint32_t>(d[o + 2]) << 16) | (static_cast<uint32_t>(d[o + 3]) << 24); };
    auto u16 = [&](size_t o) { return static_cast<uint32_t>(d[o]) | (static_cast<uint32_t>(d[o + 1]) << 8); };
    const uint32_t off = u32(10), ww = u32(18), hh = u32(22), bpp = u16(28), comp = u32(30);
    const bool topDown = static_cast<int32_t>(hh) < 0;
    const uint32_t H = topDown ? static_cast<uint32_t>(-static_cast<int32_t>(hh)) : hh;
    if ((bpp != 24 && bpp != 32) || (comp != 0 && comp != 3) || !ww || !H) return false;
    const size_t stride = ((static_cast<size_t>(ww) * bpp / 8) + 3) & ~static_cast<size_t>(3);
    if (off + stride * H > d.size()) return false;
    w = ww;
    h = H;
    img.assign(static_cast<size_t>(w) * h, 0);
    for (uint32_t y = 0; y < h; y++) {
        const uint8_t* row = d.data() + off + stride * (topDown ? y : h - 1 - y);
        for (uint32_t x = 0; x < w; x++) {
            const uint8_t* p = row + x * (bpp / 8);
            img[static_cast<size_t>(y) * w + x] = Pack(p[2], p[1], p[0], bpp == 32 ? p[3] : 255);
        }
    }
    return true;
}

// ---- several cores (DxtCodec::Parallel) ----

std::atomic<uint64_t> g_mtFallback{0};
// The fallback of the serial / parallel comparisons: may run on worker threads
void RefBlockMt(void*, bool dxt5, const uint8_t* src, int32_t pitch, uint32_t cols, uint32_t rows, uint32_t format, uint8_t* out) {
    g_mtFallback.fetch_add(1, std::memory_order_relaxed);
    Ref::EncodeBlock(nullptr, dxt5, src, pitch, cols, rows, format, out);
}

// A w x h image of random block kinds (4x4 tiles, clipped at the right and bottom edges); each row has `srcPad` extra
// bytes of random data after the pixels
struct Image {
    uint32_t w = 0, h = 0;
    int32_t pitch = 0;
    std::vector<uint8_t> bytes;
};
Image MakeImage(Rng& rng, uint32_t w, uint32_t h, uint32_t srcPad) {
    Image im;
    im.w = w;
    im.h = h;
    im.pitch = static_cast<int32_t>(w * 4 + srcPad);
    im.bytes.resize(static_cast<size_t>(im.pitch) * h);
    for (uint8_t& b : im.bytes) b = static_cast<uint8_t>(rng.Next());
    for (uint32_t y = 0; y < h; y += 4)
        for (uint32_t x = 0; x < w; x += 4) {
            uint32_t px[16];
            FillBlock(rng, static_cast<Kind>(rng.Below(kKinds)), px);
            for (uint32_t yy = 0; yy < 4 && y + yy < h; yy++)
                for (uint32_t xx = 0; xx < 4 && x + xx < w; xx++) std::memcpy(im.bytes.data() + static_cast<size_t>(y + yy) * im.pitch + static_cast<size_t>(x + xx) * 4, &px[yy * 4 + xx], 4);
        }
    return im;
}

struct ParStats {
    uint64_t encodes = 0, split = 0, busy = 0, blocks = 0, diffs = 0, refDiffs = 0, fpMismatch = 0, counterDiffs = 0;
    int printed = 0;
    void Add(const ParStats& o) {
        encodes += o.encodes;
        split += o.split;
        busy += o.busy;
        blocks += o.blocks;
        diffs += o.diffs;
        refDiffs += o.refDiffs;
        fpMismatch += o.fpMismatch;
        counterDiffs += o.counterDiffs;
    }
};

// Blocks whose bytes differ between two destination buffers, plus 1 when only the padding differs
uint64_t DiffBlocks(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, uint32_t bx, uint32_t by, uint32_t bs, uint32_t pitch, const char* what, bool dxt5, const Image& im,
                    const Parallel::Options& o, uint32_t dstPad, int& printed) {
    if (a == b) return 0;
    uint64_t n = 0;
    for (uint32_t y = 0; y < by; y++)
        for (uint32_t x = 0; x < bx; x++) {
            const size_t off = static_cast<size_t>(y) * pitch + static_cast<size_t>(x) * bs;
            if (std::memcmp(a.data() + off, b.data() + off, bs) == 0) continue;
            n++;
            if (printed < 8) {
                printed++;
                std::printf("  DIFF %s DXT%d %ux%u (source pitch %d, destination padding %u, %u workers, chunks >= %u blocks) block (%u,%u): %s / %s\n", what, dxt5 ? 5 : 1, im.w, im.h,
                            im.pitch, dstPad, o.workers, o.minBlocksPerChunk, x, y, Hex(a.data() + off, bs).c_str(), Hex(b.data() + off, bs).c_str());
            }
        }
    if (!n) {
        n = 1;
        if (printed < 8) {
            printed++;
            std::printf("  DIFF %s DXT%d %ux%u: the row padding of the destination differs\n", what, dxt5 ? 5 : 1, im.w, im.h);
        }
    }
    return n;
}

// Serial fast vs parallel (and optionally the reference) on one image, the whole destination buffers compared
void CheckParallel(bool dxt5, const Image& im, uint32_t dstPad, const Parallel::Options& o, bool withRef, ParStats& st) {
    const uint32_t bs = dxt5 ? 16 : 8, bx = (im.w + 3) / 4, by = (im.h + 3) / 4, pitch = bx * bs + dstPad;
    const size_t n = static_cast<size_t>(pitch) * by;
    std::vector<uint8_t> a(n, 0xCD), b(n, 0xCD);
    Src s{im.bytes.data(), 0, 0, im.pitch, 0x3D};
    Dst da{a.data(), im.w, im.h, pitch, 0}, db{b.data(), im.w, im.h, pitch, 0};
    FastCounters ca, cb;
    if (dxt5) Fast::EncodeDxt5(&da, &s, &RefBlockMt, nullptr, &ca);
    else Fast::EncodeDxt1(&da, &s, &RefBlockMt, nullptr, &ca);
    const Parallel::Result r = Parallel::Encode(dxt5, &db, &s, &RefBlockMt, nullptr, &cb, o);
    st.encodes++;
    st.blocks += static_cast<uint64_t>(bx) * by;
    if (r.parallel) st.split++;
    if (r.busy) st.busy++;
    st.fpMismatch += r.fpStateMismatches;
    if (ca.blocks != cb.blocks || ca.delegated != cb.delegated) st.counterDiffs++; // (flat-luma / solid counts include padding lanes: not compared)
    st.diffs += DiffBlocks(a, b, bx, by, bs, pitch, "serial/parallel", dxt5, im, o, dstPad, st.printed);
    if (withRef) {
        std::vector<uint8_t> c(n, 0xCD);
        Dst dc{c.data(), im.w, im.h, pitch, 0};
        if (dxt5) Ref::EncodeDxt5(&dc, &s);
        else Ref::EncodeDxt1(&dc, &s);
        st.refDiffs += DiffBlocks(c, b, bx, by, bs, pitch, "reference/parallel", dxt5, im, o, dstPad, st.printed);
    }
}

// The calling thread's FP state for a test mode (the workers must take it)
struct FpMode {
    const char* name;
    bool pc24; // x87 precision 24-bit (Direct3D's device thread) instead of 53-bit
    bool rz;   // MXCSR: round toward zero, flush-to-zero, denormals-are-zero
};
struct FpScope {
    unsigned int oldCsr;
    bool pc24;
    explicit FpScope(const FpMode& m) : oldCsr(_mm_getcsr()), pc24(m.pc24) {
        unsigned int cw = 0;
        if (pc24) _controlfp_s(&cw, _PC_24, _MCW_PC);
        if (m.rz) _mm_setcsr((oldCsr & ~0x6000u) | 0x6000u | 0x8040u);
    }
    ~FpScope() {
        _mm_setcsr(oldCsr);
        unsigned int cw = 0;
        if (pc24) _controlfp_s(&cw, _PC_53, _MCW_PC);
    }
};

// Returns the number of failures
uint64_t ParallelTests(Rng& rng, uint64_t seed) {
    uint64_t fails = 0;
    {
        Compare repeated;
        Fallback repeatedFallback;
        for(uint32_t w:{16u,32u,48u,65u})for(uint32_t h:{4u,8u,9u})for(int changed=0;changed<3;++changed){
            std::vector<uint32_t> img(static_cast<size_t>(w)*h);
            for(uint32_t y=0;y<h;++y){for(uint32_t x=0;x<std::min(w,16u);++x)img[y*w+x]=rng.Next();for(uint32_t x=16;x<w;++x)img[y*w+x]=img[y*w+(x%16)];}
            if(w>31&&changed)img[31]^=changed==1?0x00000100u:0x80000000u;
            for(bool dxt5:{false,true})Run(dxt5,img,w,h,repeated,repeatedFallback,nullptr,0,nullptr);
        }
        fails+=repeated.diffs;
        std::printf("Repeated strips and changed RGB/alpha: %llu blocks, %llu different\n",static_cast<unsigned long long>(repeated.blocks),static_cast<unsigned long long>(repeated.diffs));
    }

    // A cold two-chunk image needs one helper, not the requested maximum pool.
    {
        Fallback fb;
        uint32_t pixels[32];for(auto& pixel:pixels) pixel=rng.Next();
        uint8_t encoded[16]{},reference[16]{};
        Src src{reinterpret_cast<const uint8_t*>(pixels),0,0,16,0x3D};
        Dst dst{encoded,4,8,8,0},ref{reference,4,8,8,0};
        Parallel::Options options;options.workers=6;options.minPixels=0;options.minBlocksPerChunk=1;
        FastCounters counters;
        const auto result=Parallel::Encode(false,&dst,&src,&RefBlock,&fb,&counters,options);
        Fast::EncodeDxt1(&ref,&src,&RefBlock,&fb,nullptr);
        if(!result.parallel || result.chunks!=2 || Parallel::WorkersCreated()!=1 || std::memcmp(encoded,reference,sizeof encoded)!=0) {
            ++fails;std::printf("FAIL: useful worker sizing on cold pool\n");
        }
    }
    std::printf("Several cores: %u worker threads by default on this machine (logical processors - 2, at most %u)\n", Parallel::DefaultWorkers(), Parallel::kMaxWorkers);
    // ---- sizes x formats x workers x FP modes ----
    static const uint32_t kSizes[][2] = {{256, 256}, {257, 255}, {255, 257}, {300, 301}, {513, 130}, {1024, 1024}, {1023, 769}, {1025, 1026}, {258, 257}, {259, 259},
                                         {64, 2048}, {2048, 5},  {2051, 7},  {1, 1024},  {3, 999},   {1027, 3},    {16, 64},    {5, 9},      {13, 13},   {17, 33},
                                         {600, 1},   {1, 1},     {6, 4097}};
    static const FpMode kModes[] = {{"x87 53-bit", false, false}, {"x87 24-bit", true, false}, {"MXCSR round-to-zero, FTZ, DAZ", false, true}, {"x87 24-bit + MXCSR RZ/FTZ/DAZ", true, true}};
    static const uint32_t kWorkers[] = {1, 2, 3, 6};
    for (const FpMode& m : kModes) {
        ParStats st;
        const double t0 = NowMs();
        {
            FpScope scope(m);
            {
                Image repeated=MakeImage(rng,65,65,8);
                for(uint32_t y=0;y<repeated.h;++y)for(uint32_t x=16;x<repeated.w;++x)
                    std::memcpy(repeated.bytes.data()+y*repeated.pitch+x*4,repeated.bytes.data()+y*repeated.pitch+(x%16)*4,4);
                Parallel::Options o;o.workers=6;o.minPixels=0;o.minBlocksPerChunk=1;
                for(bool dxt5:{false,true})CheckParallel(dxt5,repeated,8,o,true,st);
            }
            for (const auto& sz : kSizes) {
                const uint32_t w = sz[0], h = sz[1];
                const bool odd = ((w | h) & 3) != 0;
                const Image im = MakeImage(rng, w, h, odd ? 12u : 0u);
                for (int fmt = 0; fmt < 2; fmt++)
                    for (uint32_t wc : kWorkers) {
                        Parallel::Options o;
                        o.workers = wc;
                        o.minPixels = 0;
                        o.minBlocksPerChunk = (wc & 1) ? 1u : 128u; // row-sized chunks and the default
                        const bool withRef = !m.rz && wc == 6;       // the reference is checked under the default MXCSR
                        CheckParallel(fmt == 1, im, odd ? 24u : 0u, o, withRef, st);
                    }
            }
        }
        std::printf("  %-32s %llu encodes (%llu split), %llu blocks: %llu different from serial, %llu from the reference; counter differences %llu, FP state "
                    "mismatches %llu (%.1f s)\n",
                    m.name, static_cast<unsigned long long>(st.encodes), static_cast<unsigned long long>(st.split), static_cast<unsigned long long>(st.blocks),
                    static_cast<unsigned long long>(st.diffs), static_cast<unsigned long long>(st.refDiffs), static_cast<unsigned long long>(st.counterDiffs),
                    static_cast<unsigned long long>(st.fpMismatch), (NowMs() - t0) / 1000.0);
        fails += st.diffs + st.refDiffs + st.fpMismatch + st.counterDiffs;
        if (!st.split) {
            std::printf("  (no image was split: the parallel path was not exercised)\n");
            fails++;
        }
    }
    // ---- stress: thousands of tiny splits on this thread, then 4 threads at once (hand-off, busy pool) ----
    {
        ParStats st;
        const double t0 = NowMs();
        for (int i = 0; i < 3000; i++) {
            const Image im = MakeImage(rng, 1 + rng.Below(160), 1 + rng.Below(160), rng.Below(2) * 8);
            Parallel::Options o;
            o.workers = 1 + rng.Below(Parallel::kMaxWorkers);
            o.minPixels = 0;
            o.minBlocksPerChunk = 1 + rng.Below(8);
            CheckParallel(rng.Below(2) != 0, im, rng.Below(2) * 16, o, false, st);
        }
        std::printf("  Stress, one thread: %llu encodes (%llu split): %llu different, counter differences %llu, FP state mismatches %llu (%.1f s)\n",
                    static_cast<unsigned long long>(st.encodes), static_cast<unsigned long long>(st.split), static_cast<unsigned long long>(st.diffs),
                    static_cast<unsigned long long>(st.counterDiffs), static_cast<unsigned long long>(st.fpMismatch), (NowMs() - t0) / 1000.0);
        fails += st.diffs + st.fpMismatch + st.counterDiffs;
        ParStats per[4];
        const double t1 = NowMs();
        std::vector<std::thread> threads;
        for (int t = 0; t < 4; t++)
            threads.emplace_back([&per, t, seed] {
                Rng r{seed * 31 + 7 + static_cast<uint64_t>(t)};
                const FpMode mode = {"", (t & 1) != 0, t == 3}; // the threads use different FP states at the same time
                FpScope scope(mode);
                for (int i = 0; i < 400; i++) {
                    const Image im = MakeImage(r, 8 + r.Below(300), 8 + r.Below(300), r.Below(2) * 8);
                    Parallel::Options o;
                    o.workers = 1 + r.Below(Parallel::kMaxWorkers);
                    o.minPixels = 0;
                    o.minBlocksPerChunk = 1 + r.Below(64);
                    CheckParallel(r.Below(2) != 0, im, r.Below(2) * 16, o, false, per[t]);
                }
            });
        for (std::thread& th : threads) th.join();
        ParStats all;
        for (const ParStats& p : per) all.Add(p);
        std::printf("  Stress, 4 threads at once: %llu encodes (%llu split, %llu on one core because the pool was busy): %llu different, counter differences %llu, FP "
                    "state mismatches %llu (%.1f s)\n",
                    static_cast<unsigned long long>(all.encodes), static_cast<unsigned long long>(all.split), static_cast<unsigned long long>(all.busy),
                    static_cast<unsigned long long>(all.diffs), static_cast<unsigned long long>(all.counterDiffs), static_cast<unsigned long long>(all.fpMismatch),
                    (NowMs() - t1) / 1000.0);
        fails += all.diffs + all.fpMismatch + all.counterDiffs;
    }
    std::printf("  Blocks handed to the reference by the fast paths here (non-finite values): %llu\n", static_cast<unsigned long long>(g_mtFallback.load()));
    // ---- timing: one core vs the default workers ----
    uint32_t workers = Parallel::DefaultWorkers();
    if (!workers) {
        std::printf("  (this machine has 2 or fewer logical processors: the game would use one core; timing with 2 workers anyway)\n");
        workers = 2;
    }
    for (uint32_t side : {256u, 512u, 1024u, 2048u}) {
        const Image im = MakeImage(rng, side, side, 0);
        for (int fmt = 0; fmt < 2; fmt++) {
            const bool dxt5 = fmt == 1;
            const uint32_t bs = dxt5 ? 16 : 8, pitch = (side / 4) * bs;
            std::vector<uint8_t> out(static_cast<size_t>(pitch) * (side / 4));
            Src s{im.bytes.data(), 0, 0, im.pitch, 0x3D};
            Dst d{out.data(), side, side, pitch, 0};
            Parallel::Options o;
            o.workers = workers;
            double bestSerial = 1e30, bestPar = 1e30;
            uint32_t participants = 0;
            for (int rep = 0; rep < 5; rep++) {
                const double t0 = NowMs();
                if (dxt5) Fast::EncodeDxt5(&d, &s, &RefBlockMt, nullptr, nullptr);
                else Fast::EncodeDxt1(&d, &s, &RefBlockMt, nullptr, nullptr);
                const double t1 = NowMs();
                const Parallel::Result r = Parallel::Encode(dxt5, &d, &s, &RefBlockMt, nullptr, nullptr, o);
                const double t2 = NowMs();
                if (t1 - t0 < bestSerial) bestSerial = t1 - t0;
                if (t2 - t1 < bestPar) {
                    bestPar = t2 - t1;
                    participants = r.participants;
                }
            }
            std::printf("  Timing DXT%d %ux%u: one core %.2f ms, %u workers + caller %.2f ms (%.1fx, %u threads took chunks)\n", dxt5 ? 5 : 1, side, side, bestSerial, workers,
                        bestPar, bestSerial / bestPar, participants);
        }
    }
    return fails;
}

} // namespace

int main(int argc, char** argv) {
    uint64_t totalBlocks = 10000000;
    uint64_t seed = 0x5EED1234ABCDull;
    std::vector<std::string> bmps;
    bool parallelOnly = false;
    for (int i = 1; i < argc; i++) {
        if (!std::strcmp(argv[i], "--parallel-only")) parallelOnly = true;
        else if (!std::strcmp(argv[i], "--blocks") && i + 1 < argc) totalBlocks = std::strtoull(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--seed") && i + 1 < argc) seed = std::strtoull(argv[++i], nullptr, 10);
        else if (!std::strcmp(argv[i], "--bmp") && i + 1 < argc) bmps.push_back(argv[++i]);
    }
    std::printf("DXT encoder test: fast (4 blocks per SSE register) vs the translation of TS3W.exe 0x006152F0 / 0x006154B0\nCPU: %s\n", CpuFeatureText());
    if (!CpuHasSse2()) {
        std::printf("No SSE2: nothing to test\n");
        return 2;
    }
    Rng rng{seed};
    Fallback fb;
    uint64_t fails = 0;

    // ---- 1. generated blocks: images of 64 x 16 blocks, each block of a random kind ----
    for (int fmt = 0; fmt < 2 && !parallelOnly; fmt++) {
        const bool dxt5 = fmt == 1;
        const uint32_t bw = 64, bh = 16, w = bw * 4, h = bh * 4;
        std::vector<uint32_t> img(static_cast<size_t>(w) * h);
        std::vector<Kind> kinds(static_cast<size_t>(bw) * bh);
        uint64_t perKind[kKinds] = {}, diffKind[kKinds] = {};
        Compare c;
        const double t0 = NowMs();
        for (uint64_t done = 0; done < totalBlocks; done += static_cast<uint64_t>(bw) * bh) {
            for (uint32_t by = 0; by < bh; by++)
                for (uint32_t bx = 0; bx < bw; bx++) {
                    const Kind k = static_cast<Kind>(rng.Below(kKinds));
                    kinds[by * bw + bx] = k;
                    perKind[k]++;
                    uint32_t px[16];
                    FillBlock(rng, k, px);
                    for (int y = 0; y < 4; y++)
                        for (int x = 0; x < 4; x++) img[static_cast<size_t>(by * 4 + y) * w + bx * 4 + x] = px[y * 4 + x];
                }
            // every 8th image with the x87 unit at 24-bit precision (what Direct3D sets on the device thread)
            const bool pc24 = ((done / (static_cast<uint64_t>(bw) * bh)) & 7) == 7;
            unsigned int old = 0;
            if (pc24) _controlfp_s(&old, _PC_24, _MCW_PC);
            Run(dxt5, img, w, h, c, fb, kinds.data(), bw, diffKind);
            if (pc24) _controlfp_s(&old, _PC_53, _MCW_PC);
        }
        std::printf("DXT%d: %llu blocks in %.1f s, %llu different\n", dxt5 ? 5 : 1, static_cast<unsigned long long>(c.blocks), (NowMs() - t0) / 1000.0, static_cast<unsigned long long>(c.diffs));
        for (int k = 0; k < kKinds; k++)
            std::printf("  %-16s %10llu blocks, %llu different\n", kKindName[k], static_cast<unsigned long long>(perKind[k]), static_cast<unsigned long long>(diffKind[k]));
        fails += c.diffs;
    }

    // ---- 2. every edge size: widths and heights 1..13 (partial blocks, the DXT5 partial-width alpha fetch) ----
    if (!parallelOnly) {
        Compare c1, c5;
        for (int rep = 0; rep < 40; rep++)
            for (uint32_t w = 1; w <= 13; w++)
                for (uint32_t h = 1; h <= 13; h++) {
                    std::vector<uint32_t> img(static_cast<size_t>(w) * h);
                    const Kind k = static_cast<Kind>(rng.Below(kKinds));
                    for (uint32_t y = 0; y < h; y += 4)
                        for (uint32_t x = 0; x < w; x += 4) {
                            uint32_t px[16];
                            FillBlock(rng, k, px);
                            for (uint32_t yy = 0; yy < 4 && y + yy < h; yy++)
                                for (uint32_t xx = 0; xx < 4 && x + xx < w; xx++) img[static_cast<size_t>(y + yy) * w + x + xx] = px[yy * 4 + xx];
                        }
                    Run(false, img, w, h, c1, fb, nullptr, 0, nullptr);
                    Run(true, img, w, h, c5, fb, nullptr, 0, nullptr);
                }
        std::printf("Edge sizes 1..13 x 1..13: DXT1 %llu blocks, %llu different; DXT5 %llu blocks, %llu different\n", static_cast<unsigned long long>(c1.blocks),
                    static_cast<unsigned long long>(c1.diffs), static_cast<unsigned long long>(c5.blocks), static_cast<unsigned long long>(c5.diffs));
        fails += c1.diffs + c5.diffs;
    }
    if (!parallelOnly) std::printf("Blocks the fast path handed to the reference (non-finite intermediate values): %llu\n", static_cast<unsigned long long>(fb.calls));

    // ---- 3. images from files ----
    for (const std::string& path : bmps) {
        std::vector<uint32_t> img;
        uint32_t w = 0, h = 0;
        if (!LoadBmp(path.c_str(), img, w, h)) {
            std::printf("%s: not a 24/32-bit uncompressed BMP\n", path.c_str());
            continue;
        }
        Compare c1, c5;
        Run(false, img, w, h, c1, fb, nullptr, 0, nullptr);
        Run(true, img, w, h, c5, fb, nullptr, 0, nullptr);
        std::printf("%s (%ux%u): DXT1 %llu different of %llu blocks, DXT5 %llu different\n", path.c_str(), w, h, static_cast<unsigned long long>(c1.diffs),
                    static_cast<unsigned long long>(c1.blocks), static_cast<unsigned long long>(c5.diffs));
        fails += c1.diffs + c5.diffs;
    }

    // ---- 4. timing: a 1024 x 1024 texture-like image (gradients and noise, some flat and alpha areas) ----
    if (!parallelOnly) {
        const uint32_t w = 1024, h = 1024;
        std::vector<uint32_t> img(static_cast<size_t>(w) * h);
        for (uint32_t by = 0; by < h / 4; by++)
            for (uint32_t bx = 0; bx < w / 4; bx++) {
                const uint32_t pick = rng.Below(10);
                const Kind k = pick < 4 ? kGradient : (pick < 7 ? kNearFlat : (pick < 8 ? kSolid : (pick < 9 ? kTwoColour : kRandom)));
                uint32_t px[16];
                FillBlock(rng, k, px);
                for (int y = 0; y < 4; y++)
                    for (int x = 0; x < 4; x++) img[static_cast<size_t>(by * 4 + y) * w + bx * 4 + x] = px[y * 4 + x];
            }
        for (int fmt = 0; fmt < 2; fmt++) {
            const bool dxt5 = fmt == 1;
            const uint32_t bs = dxt5 ? 16 : 8, pitch = (w / 4) * bs;
            std::vector<uint8_t> out(static_cast<size_t>(pitch) * (h / 4));
            Src s{reinterpret_cast<const uint8_t*>(img.data()), 0, 0, static_cast<int32_t>(w * 4), 0x3D};
            Dst d{out.data(), w, h, pitch, 0};
            double bestRef = 1e30, bestFast = 1e30;
            for (int rep = 0; rep < 3; rep++) {
                double t0 = NowMs();
                if (dxt5) Ref::EncodeDxt5(&d, &s);
                else Ref::EncodeDxt1(&d, &s);
                double t1 = NowMs();
                FastCounters fc;
                if (dxt5) Fast::EncodeDxt5(&d, &s, &RefBlock, &fb, &fc);
                else Fast::EncodeDxt1(&d, &s, &RefBlock, &fb, &fc);
                double t2 = NowMs();
                if (t1 - t0 < bestRef) bestRef = t1 - t0;
                if (t2 - t1 < bestFast) bestFast = t2 - t1;
            }
            std::printf("Timing DXT%d 1024x1024: reference %.1f ms, fast %.1f ms (%.1fx)\n", dxt5 ? 5 : 1, bestRef, bestFast, bestRef / bestFast);
        }
        std::printf("(the reference follows the game's instruction sequence but is not the game's code; the game's own time on the same textures is\n"
                    " measured in game: Developer > Profiler > Performance, \"checked textures: game X ms, Apex Y ms\")\n");
    }

    // ---- 5. several cores: serial vs parallel (sizes, workers, FP states), stress, timing ----
    fails += ParallelTests(rng, seed);

    std::printf(fails ? "RESULT: %llu DIFFERENT BLOCKS\n" : "RESULT: all blocks identical\n", static_cast<unsigned long long>(fails));
    return fails ? 1 : 0;
}
