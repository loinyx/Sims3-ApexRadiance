#include "ui/widgets.h"
#include "developer_settings.h"
// Faster texture compression: the game's CPU DXT1 / DXT5 encoders replaced by a bit-identical four-blocks-at-a-time
// version (see fast_dxt.h, features/dxt_codec.h and docs/features/performance.md).
//
// Part of Apex Radiance. Credits: @loinyx
#include "fast_dxt.h"
#include "high_alloc.h"
#include "scratch_budget.h"
#include <type_traits>
#include "dxt_codec.h"
#include "entry_chain.h"
#include "game_addresses.h"
#include "apex_log.h"
#include "build_flavor.h"
#include "imgui.h"
#include <windows.h>
#include <atomic>
#include <cstring>
#include <format>
#include <mutex>
#include <new>
#include <vector>

namespace FastDxt {
namespace {

using EntryChain::Layer;
using EntryChain::Site;
using FnCdecl2 = uint64_t(__cdecl*)(uint32_t, uint32_t);

static_assert(sizeof(DxtCodec::Dst) == 0x14 && sizeof(DxtCodec::Src) == 0x14, "the game's image descriptors are 0x14 bytes");

constexpr uint32_t kStartupChecks = 16; // the first images of each session are checked against the game on every build
constexpr uint64_t kMaxCheckBytes = 64ull << 20;

std::mutex g_ctrl;
bool g_started = false; // guarded by g_ctrl
std::atomic<bool> g_on{false};
std::atomic<bool> g_selfDisabled{false};
std::atomic<int> g_verifyEvery{kPublicBuild ? 0 : 64};
std::atomic<uint64_t> g_verifyAllUntil{0};
std::atomic<uint32_t> g_seq{0};
std::atomic<uint64_t> c_bytesOut{0}; // DXT bytes written by the fast encoder (8 or 16 per 4x4 block)
std::atomic<uint64_t> c_images{0}, c_pixels{0}, c_blocks{0}, c_delegated{0}, c_power{0}, c_solid{0}, c_passed{0};
std::atomic<uint64_t> c_checked{0}, c_mismatch{0}, c_notCheckable{0};
std::atomic<uint64_t> g_fastTicks{0}, g_checkGameTicks{0}, g_checkFastTicks{0};
double g_qpcMs = 0.0;
// several cores
constexpr int kDefaultMinSide = 256;
std::atomic<bool> g_severalCores{true};
std::atomic<int> g_parWorkers{-1}; // -1 = DxtCodec::Parallel::DefaultWorkers()
std::atomic<int> g_parMinSide{kDefaultMinSide};
std::atomic<uint64_t> c_parImages{0}, c_parBusy{0}, c_parChunks{0}, c_parByWorkers{0}, c_parFpMismatch{0};
std::atomic<uint64_t> g_parWallUs{0}, g_parSavedUs{0};
std::atomic<bool> g_fpMismatchLogged{false};
std::atomic<bool> g_parSelfOff{false}; // a worker's FP state differed: one core only until the next Start

uint32_t WorkersToUse() {
    const int n = g_parWorkers.load(std::memory_order_relaxed);
    return n < 0 ? DxtCodec::Parallel::DefaultWorkers() : static_cast<uint32_t>(n);
}
std::mutex g_mismatchLock;
std::string g_lastMismatch; // guarded by g_mismatchLock

uint64_t Qpc() {
    LARGE_INTEGER q;
    QueryPerformanceCounter(&q);
    return static_cast<uint64_t>(q.QuadPart);
}

struct FallbackCtx {
    FnCdecl2 game;
};

// One block through the game's own encoder: a one-block image (cols x rows pixels) at the block's pixels, same pitch and
// format. The game's drivers fetch an edge block exactly like that inside a whole image (same helper, same cols / rows).
// May run on a DXT worker thread (several cores): the game's function only uses its stack and .rdata constants (the game
// itself calls it from several threads), and the worker has the calling thread's x87 control word and MXCSR.
void GameBlock(void* ctx, bool dxt5, const uint8_t* src, int32_t pitch, uint32_t cols, uint32_t rows, uint32_t format, uint8_t* out) {
    DxtCodec::Dst d{out, cols, rows, dxt5 ? 16u : 8u, 0};
    DxtCodec::Src s{src, 0, 0, pitch, format};
    static_cast<FallbackCtx*>(ctx)->game(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&d)), static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&s)));
}

void RunFast(bool dxt5, const DxtCodec::Dst* d, const DxtCodec::Src* s, FnCdecl2 game) {
    FallbackCtx f{game};
    DxtCodec::FastCounters c;
    DxtCodec::Parallel::Options o;
    if (g_severalCores.load(std::memory_order_relaxed) && !g_parSelfOff.load(std::memory_order_relaxed)) {
        o.workers = WorkersToUse();
        const uint64_t side = static_cast<uint64_t>(g_parMinSide.load(std::memory_order_relaxed));
        o.minPixels = side * side;
    }
    // o.workers == 0: serial on this thread (the same code as Fast::EncodeDxt1/5)
    const DxtCodec::Parallel::Result r = DxtCodec::Parallel::Encode(dxt5, d, s, &GameBlock, &f, &c, o);
    if (r.busy) c_parBusy.fetch_add(1, std::memory_order_relaxed);
    if (r.parallel) {
        c_parImages.fetch_add(1, std::memory_order_relaxed);
        c_parChunks.fetch_add(r.chunks, std::memory_order_relaxed);
        c_parByWorkers.fetch_add(r.chunksByWorkers, std::memory_order_relaxed);
        g_parWallUs.fetch_add(static_cast<uint64_t>(r.wallMs * 1000.0), std::memory_order_relaxed);
        if (r.workMs > r.wallMs) g_parSavedUs.fetch_add(static_cast<uint64_t>((r.workMs - r.wallMs) * 1000.0), std::memory_order_relaxed);
        if (r.fpStateMismatches) {
            // a worker ran with another FP state: its blocks may differ from the game's. Encode the whole image again on
            // this thread (every block is rewritten) and keep the workers out for the rest of the session.
            c_parFpMismatch.fetch_add(r.fpStateMismatches, std::memory_order_relaxed);
            g_parSelfOff.store(true);
            if (!g_fpMismatchLogged.exchange(true))
                LOG_ERROR("[FastDxt] A DXT worker thread did not take the calling thread's floating-point state (x87 control word / MXCSR); the image was encoded "
                          "again on the calling thread and several cores stay off for this session");
            c = DxtCodec::FastCounters{};
            if (dxt5) DxtCodec::Fast::EncodeDxt5(d, s, &GameBlock, &f, &c);
            else DxtCodec::Fast::EncodeDxt1(d, s, &GameBlock, &f, &c);
        }
    }
    c_blocks.fetch_add(c.blocks, std::memory_order_relaxed);
    c_delegated.fetch_add(c.delegated, std::memory_order_relaxed);
    c_power.fetch_add(c.powerAxis, std::memory_order_relaxed);
    c_solid.fetch_add(c.solid, std::memory_order_relaxed);
}

std::string Hex(const uint8_t* p, size_t n) {
    std::string s;
    for (size_t i = 0; i < n; i++) s += std::format("{:02x}", static_cast<unsigned>(p[i]));
    return s;
}

uint32_t Load32(const uint8_t* p) {
    uint32_t v;
    std::memcpy(&v, p, 4);
    return v;
}

// Encodes with the game into a scratch copy and with Apex into the destination, then compares every block. On a
// difference the game's bytes are kept and the feature turns itself off for the session.
void Checked(bool dxt5, const DxtCodec::Dst* d, const DxtCodec::Src* s, FnCdecl2 game) {
    const uint32_t bs = dxt5 ? 16u : 8u;
    const uint32_t bx = (d->width >> 2) + ((d->width & 3) ? 1u : 0u), by = (d->height >> 2) + ((d->height & 3) ? 1u : 0u);
    const uint64_t rowBytes = static_cast<uint64_t>(bx) * bs;
    const uint64_t total = static_cast<uint64_t>(d->pitch) * by;
    using ScratchAllocator=std::conditional_t<kMemoryOptimizationsEnabled,ApexMem::HighAllocator<uint8_t>,std::allocator<uint8_t>>;
    ScratchBudget::Lease lease(total,kMemoryOptimizationsEnabled);
    std::vector<uint8_t,ScratchAllocator> scratch;
    bool can = bx && by && d->pitch >= rowBytes && total <= kMaxCheckBytes && (!kMemoryOptimizationsEnabled || lease.bytes==total);
    if (can) {
        try {
            scratch.resize(static_cast<size_t>(total));
        } catch (const std::bad_alloc&) {
            can = false;
        }
    }
    if (!can) {
        c_notCheckable.fetch_add(1, std::memory_order_relaxed);
        const uint64_t t0 = Qpc();
        RunFast(dxt5, d, s, game);
        g_fastTicks.fetch_add(Qpc() - t0, std::memory_order_relaxed);
        return;
    }
    DxtCodec::Dst gd = *d;
    gd.ptr = scratch.data();
    const uint64_t t0 = Qpc();
    game(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(&gd)), static_cast<uint32_t>(reinterpret_cast<uintptr_t>(s)));
    const uint64_t t1 = Qpc();
    RunFast(dxt5, d, s, game);
    const uint64_t t2 = Qpc();
    g_fastTicks.fetch_add(t2 - t1, std::memory_order_relaxed);
    for (uint32_t y = 0; y < by; y++) {
        const uint8_t* a = scratch.data() + static_cast<size_t>(y) * d->pitch;
        const uint8_t* b = d->ptr + static_cast<size_t>(y) * d->pitch;
        if (std::memcmp(a, b, static_cast<size_t>(rowBytes)) == 0) continue;
        uint32_t x = 0;
        while (x + 1 < bx && std::memcmp(a + x * bs, b + x * bs, bs) == 0) x++;
        const uint32_t cols = x < (d->width >> 2) ? 4u : (d->width & 3u);
        const uint32_t rows = d->height - 4 * y < 4 ? d->height - 4 * y : 4u;
        std::string px;
        for (uint32_t r = 0; r < rows; r++) {
            const uint8_t* row = s->ptr + static_cast<intptr_t>(s->pitch) * static_cast<intptr_t>(4 * y + r) + 16 * static_cast<uintptr_t>(x);
            for (uint32_t c = 0; c < cols; c++) px += std::format("{:08x} ", Load32(row + 4 * c));
            if (r + 1 < rows) px += "| ";
        }
        const std::string msg = std::format("DXT{} {}x{} image (format {:#x}), block ({}, {}), {}x{} pixels: game {} / Apex {}; pixels 0xAARRGGBB: {}", dxt5 ? 5 : 1,
                                            d->width, d->height, s->format, x, y, cols, rows, Hex(a + x * bs, bs), Hex(b + x * bs, bs), px);
        LOG_ERROR("[FastDxt] Verification mismatch: " + msg + ". The game's bytes were used; the feature turns itself off for this session.");
        for (uint32_t r = 0; r < by; r++) std::memcpy(d->ptr + static_cast<size_t>(r) * d->pitch, scratch.data() + static_cast<size_t>(r) * d->pitch, static_cast<size_t>(rowBytes));
        {
            std::lock_guard<std::mutex> lock(g_mismatchLock);
            g_lastMismatch = msg;
        }
        c_mismatch.fetch_add(1, std::memory_order_relaxed);
        g_selfDisabled.store(true);
        return;
    }
    g_checkGameTicks.fetch_add(t1 - t0, std::memory_order_relaxed);
    g_checkFastTicks.fetch_add(t2 - t1, std::memory_order_relaxed);
    c_checked.fetch_add(1, std::memory_order_relaxed);
}

// The replacement of both encoders (layer FastDxt of the entry chain): cdecl(dst*, src*), eax = width & ~3 like the game
uint64_t Encode(bool dxt5, uint32_t dstp, uint32_t srcp) {
    const FnCdecl2 game = reinterpret_cast<FnCdecl2>(EntryChain::Next(dxt5 ? Site::DxtEncode5 : Site::DxtEncode1, Layer::FastDxt));
    if (!g_on.load(std::memory_order_acquire) || g_selfDisabled.load(std::memory_order_relaxed)) {
        c_passed.fetch_add(1, std::memory_order_relaxed);
        return game(dstp, srcp);
    }
    const auto* d = reinterpret_cast<const DxtCodec::Dst*>(static_cast<uintptr_t>(dstp));
    const auto* s = reinterpret_cast<const DxtCodec::Src*>(static_cast<uintptr_t>(srcp));
    if (dxt5 && s->format != 0x3D && s->format != 0x3E) return game(dstp, srcp); // the game encodes nothing for other formats
    const uint32_t ret = d->width & ~3u;
    const uint32_t seq = g_seq.fetch_add(1, std::memory_order_relaxed);
    bool check = seq < kStartupChecks;
    if (!check && !kPublicBuild) {
        const int n = g_verifyEvery.load(std::memory_order_relaxed);
        check = (n > 0 && seq % static_cast<uint32_t>(n) == 0) || GetTickCount64() < g_verifyAllUntil.load(std::memory_order_relaxed);
    }
    c_images.fetch_add(1, std::memory_order_relaxed);
    c_pixels.fetch_add(static_cast<uint64_t>(d->width) * d->height, std::memory_order_relaxed);
    c_bytesOut.fetch_add(static_cast<uint64_t>((d->width + 3) / 4) * ((d->height + 3) / 4) * (dxt5 ? 16u : 8u), std::memory_order_relaxed);
    if (check) {
        Checked(dxt5, d, s, game);
    } else {
        const uint64_t t0 = Qpc();
        RunFast(dxt5, d, s, game);
        g_fastTicks.fetch_add(Qpc() - t0, std::memory_order_relaxed);
    }
    return ret;
}

uint64_t __cdecl Hook_Dxt1(uint32_t dst, uint32_t src) { return Encode(false, dst, src); }
uint64_t __cdecl Hook_Dxt5(uint32_t dst, uint32_t src) { return Encode(true, dst, src); }

} // namespace

bool Start(std::string* error) {
    std::lock_guard<std::mutex> lock(g_ctrl);
    if (g_started) return true;
    auto fail = [&](const std::string& why) {
        if (error) *error = why;
        return false;
    };
    if (!DxtCodec::CpuHasSse2()) return fail("This processor has no SSE2");
    std::string missing;
    if (!GameAddr::GroupAvailable("FastTextureCompression", &missing)) return fail(GameAddr::NotAvailable(missing));
    if (g_qpcMs == 0.0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        g_qpcMs = 1000.0 / static_cast<double>(f.QuadPart);
    }
    std::string err;
    if (!EntryChain::Install(Site::DxtEncode1, Layer::FastDxt, reinterpret_cast<void*>(&Hook_Dxt1), &err)) return fail("Could not hook the DXT1 encoder: " + err);
    if (!EntryChain::Install(Site::DxtEncode5, Layer::FastDxt, reinterpret_cast<void*>(&Hook_Dxt5), &err)) {
        EntryChain::Remove(Site::DxtEncode1, Layer::FastDxt);
        return fail("Could not hook the DXT5 encoder: " + err);
    }
    g_selfDisabled.store(false);
    g_parSelfOff.store(false);
    g_fpMismatchLogged.store(false);
    g_seq.store(0); // the startup checks run again after every start
    const bool cores = g_severalCores.load();
    if (cores) DxtCodec::Parallel::Prepare(WorkersToUse()); // the worker threads now, not during the first large texture
    g_on.store(true, std::memory_order_release);
    g_started = true;
    const std::string coresText = !cores ? std::string("several cores off")
                                  : WorkersToUse() ? std::format("images from {0}x{0} split over this thread and up to {1} workers ({2} created)", g_parMinSide.load(), WorkersToUse(),
                                                                 DxtCodec::Parallel::WorkersCreated())
                                                   : std::string("one core (this processor has too few for workers)");
    LOG_INFO(std::format("[FastDxt] On: DXT1 {:#010x} and DXT5 {:#010x} encoders replaced (CPU: {}); {}; the first {} images are checked against the game",
                         EntryChain::GameFunction(Site::DxtEncode1), EntryChain::GameFunction(Site::DxtEncode5), DxtCodec::CpuFeatureText(), coresText, kStartupChecks));
    return true;
}

bool Stop() {
    std::lock_guard<std::mutex> lock(g_ctrl);
    if (!g_started) return true;
    g_on.store(false, std::memory_order_release); // the hooks pass every call through from now on
    const bool a = EntryChain::Remove(Site::DxtEncode1, Layer::FastDxt);
    const bool b = EntryChain::Remove(Site::DxtEncode5, Layer::FastDxt);
    g_started = false;
    const Stats s = GetStats();
    LOG_INFO(std::format("[FastDxt] Off ({} images, {} blocks, {} encoded by the game's function, {} checked, {} different; {} split over several cores, about {:.0f} ms "
                         "saved){}",
                         s.images, s.blocks, s.delegated, s.checked, s.mismatches, s.parallelImages, s.savedMs,
                         a && b ? "" : "; an entry could not be put back (the hook stays and passes every call through)"));
    return a && b;
}

bool Running() { return g_on.load(std::memory_order_acquire); }

void SetSeveralCores(bool on) {
    const bool was = g_severalCores.exchange(on);
    if (on && !was && Running()) DxtCodec::Parallel::Prepare(WorkersToUse());
}
bool SeveralCores() { return g_severalCores.load(); }

void SetVerifyEvery(int n) { g_verifyEvery.store(n < 0 ? 0 : n); }
int VerifyEvery() { return g_verifyEvery.load(); }
void VerifyAllFor(double seconds) { g_verifyAllUntil.store(GetTickCount64() + static_cast<uint64_t>(seconds * 1000.0)); }

void SetParallelWorkers(int n) {
    const int maxN = static_cast<int>(DxtCodec::Parallel::DefaultWorkers());
    n = n < 0 ? 0 : (n > maxN ? maxN : n);
    g_parWorkers.store(n);
    if (n && Running() && g_severalCores.load()) DxtCodec::Parallel::Prepare(static_cast<uint32_t>(n));
}
int ParallelWorkers() { return static_cast<int>(WorkersToUse()); }
void SetParallelMinSide(int side) { g_parMinSide.store(side < 4 ? 4 : (side > 4096 ? 4096 : side)); }
int ParallelMinSide() { return g_parMinSide.load(); }

Stats GetStats() {
    Stats s;
    s.images = c_images.load();
    s.pixels = c_pixels.load();
    s.blocks = c_blocks.load();
    s.bytesOut = c_bytesOut.load();
    s.delegated = c_delegated.load();
    s.powerAxis = c_power.load();
    s.solid = c_solid.load();
    s.passedThrough = c_passed.load();
    s.checked = c_checked.load();
    s.mismatches = c_mismatch.load();
    s.notCheckable = c_notCheckable.load();
    s.fastMs = static_cast<double>(g_fastTicks.load()) * g_qpcMs;
    s.checkedGameMs = static_cast<double>(g_checkGameTicks.load()) * g_qpcMs;
    s.checkedFastMs = static_cast<double>(g_checkFastTicks.load()) * g_qpcMs;
    s.parallelImages = c_parImages.load();
    s.parallelBusy = c_parBusy.load();
    s.parallelChunks = c_parChunks.load();
    s.chunksByWorkers = c_parByWorkers.load();
    s.fpStateMismatches = c_parFpMismatch.load();
    s.parallelWallMs = static_cast<double>(g_parWallUs.load()) / 1000.0;
    s.savedMs = static_cast<double>(g_parSavedUs.load()) / 1000.0;
    s.workersCreated = DxtCodec::Parallel::WorkersCreated();
    s.selfDisabled = g_selfDisabled.load();
    std::lock_guard<std::mutex> lock(g_mismatchLock);
    s.lastMismatch = g_lastMismatch;
    return s;
}

std::string StatusText() {
    if (!Running()) return "Off";
    const Stats s = GetStats();
    if (s.selfDisabled) return "Turned itself off: a check found a block that differs from the game's (see ApexRadiance_LOG.txt)";
    if (!s.images) return "On (no textures compressed yet)";
    std::string t = std::format("On: {} textures ({:.1f} M pixels) in {:.0f} ms; {} checked against the game, all equal", s.images, static_cast<double>(s.pixels) / 1e6, s.fastMs,
                                s.checked);
    if (s.checkedFastMs > 0.0 && s.checkedGameMs > 0.0) t += std::format(" ({:.1f}x faster on those)", s.checkedGameMs / s.checkedFastMs);
    if (s.parallelImages) t += std::format("; {} large ones split over several cores", s.parallelImages);
    return t;
}

void RenderDeveloperUI() {
    if (kPublicBuild) return;
    if (!ImGui::GetCurrentContext()) return;
    const Stats s = GetStats();
    int every = g_verifyEvery.load();
    if (ApexUi::DiagnosticIntRow("Check 1 texture in N against the game##FdVerify", &every, 0, 64)) SetVerifyEvery(every);
    if (ApexUi::BeginControlRow("Validation run", "Adds comparison work while the test is active", ApexUi::ButtonWidth("Check every texture for 30 s##FdVerifyAll", true))) {
        if (ApexUi::IconTextButton("Check every texture for 30 s##FdVerifyAll", ApexUi::IconId::Scan)) VerifyAllFor(30.0);
        ApexUi::EndControlRow();
    }
    const bool all = GetTickCount64() < g_verifyAllUntil.load();
    ImGui::TextWrapped("Checks: %llu equal, %llu different, %llu too large to check%s", static_cast<unsigned long long>(s.checked), static_cast<unsigned long long>(s.mismatches),
                        static_cast<unsigned long long>(s.notCheckable), all ? "  [checking every texture]" : "");
    if (!s.lastMismatch.empty()) ImGui::TextColored(ImVec4(0.91f, 0.44f, 0.42f, 1.0f), "Last difference: %s", s.lastMismatch.c_str());
    // several cores (DxtCodec::Parallel)
    const uint32_t maxWorkers = DxtCodec::Parallel::DefaultWorkers();
    if (ApexUi::BeginAdvanced("WorkerDetails", "Worker counters")) {
    const char* coresState = !g_severalCores.load() ? "off (Use several cores)" : g_parSelfOff.load() ? "turned itself off (a worker's FP state differed, see the log)" : "on";
    ImGui::TextWrapped("Several cores: %s; %u worker threads created (up to %u on this processor)", coresState, s.workersCreated, maxWorkers);
    ImGui::TextWrapped("Split textures %llu (%llu chunks, %.0f%% by workers) in %.1f ms; about %.1f ms saved (summed chunk time minus wall time); on one core because another "
                        "texture had the workers %llu; FP state mismatches %llu",
                        static_cast<unsigned long long>(s.parallelImages), static_cast<unsigned long long>(s.parallelChunks),
                        s.parallelChunks ? 100.0 * static_cast<double>(s.chunksByWorkers) / static_cast<double>(s.parallelChunks) : 0.0, s.parallelWallMs, s.savedMs,
                        static_cast<unsigned long long>(s.parallelBusy), static_cast<unsigned long long>(s.fpStateMismatches));
        ApexUi::EndAdvanced();
    }
    if (maxWorkers) {
        int workers = ParallelWorkers();
            if (ApexUi::DiagnosticIntRow("Worker threads per texture (0 = one core)##FdWorkers", &workers, 0, static_cast<int>(maxWorkers))) SetParallelWorkers(workers);
    } else {
        ImGui::TextWrapped("This processor has too few logical processors for workers: one core");
    }
    int side = ParallelMinSide();
    if (ApexUi::DiagnosticIntRow("Split textures from (side, pixels)##FdMinSide", &side, 32, 2048, nullptr, ImGuiSliderFlags_Logarithmic)) SetParallelMinSide(side);
    if (ApexUi::TextButton("Default##FdParDefaults")) {
        SetParallelWorkers(static_cast<int>(maxWorkers));
        SetParallelMinSide(kDefaultMinSide);
    }
    if (ApexUi::BeginAdvanced("LiveCounters", "Live counters")) {
    ImGui::TextWrapped("%s", ("Faster texture compression: " + StatusText()).c_str());
    ImGui::TextWrapped("Textures %llu, blocks %llu (flat-luma %llu, solid %llu), encoded by the game's own function %llu; passed through while off %llu",
                        static_cast<unsigned long long>(s.images), static_cast<unsigned long long>(s.blocks), static_cast<unsigned long long>(s.powerAxis),
                        static_cast<unsigned long long>(s.solid), static_cast<unsigned long long>(s.delegated), static_cast<unsigned long long>(s.passedThrough));
    ImGui::TextWrapped("Apex's encoder %.1f ms in total; checked textures: game %.1f ms, Apex %.1f ms%s", s.fastMs, s.checkedGameMs, s.checkedFastMs,
                        s.checkedFastMs > 0.0 ? std::format(" ({:.1f}x)", s.checkedGameMs / s.checkedFastMs).c_str() : "");
    ImGui::TextWrapped("CPU: %s", DxtCodec::CpuFeatureText());
        ApexUi::EndAdvanced();
    }
}


void SaveDeveloperState(toml::table& out) {
    out.insert("verify_every", g_verifyEvery.load());
    out.insert("workers", g_parWorkers.load());
    out.insert("min_side", g_parMinSide.load());
}
void LoadDeveloperState(const toml::table& t) {
    if (auto n = t["verify_every"].value<int64_t>()) { const int v = static_cast<int>(*n); g_verifyEvery.store(std::clamp(v, 0, 1024)); }
    if (auto n = t["workers"].value<int64_t>()) { const int v = static_cast<int>(*n); if (v < 0) g_parWorkers.store(-1); else SetParallelWorkers(v); }
    if (auto n = t["min_side"].value<int64_t>()) { const int v = static_cast<int>(*n); g_parMinSide.store(std::clamp(v, 4, 4096)); }
}
} // namespace FastDxt
