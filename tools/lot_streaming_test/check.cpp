// Lot Streaming offline checks: the shared lot build time's decision (features/lot_build_share_policy.h), a replay of
// recorded lot loads with and without it, a model of the lot metric's detailed-lot hysteresis, and (with --exe) the game
// bytes the patches expect on Steam 1.67.2. Read-only: writes nothing.
//   check.exe [--csv-dir <folder with LotLoadAnalysis-*.csv>] [--exe <TS3W.exe>]
// Part of Apex Radiance. Credits: @loinyx
#include "lot_build_share_policy.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace P = LotBuildSharePolicy;
static int g_fail = 0;
#define CHECK(c, ...)                                                                                                  \
    do {                                                                                                               \
        if (!(c)) {                                                                                                    \
            std::printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                          \
            std::printf(__VA_ARGS__);                                                                                  \
            std::printf("\n");                                                                                         \
            g_fail++;                                                                                                  \
        }                                                                                                              \
    } while (0)

// ---- 1. the decision ----
static void PolicyChecks() {
    // waiting stages always run with the game's slice
    for (int stage = P::kFirstWaitStage; stage <= P::kLastStage; stage++) {
        const auto d = P::Decide(100.0, false, false, stage, 0);
        CHECK(d.run && d.sliceMs == 0, "stage %d must pass through", stage);
    }
    // first lot of a frame gets the whole budget
    auto d = P::Decide(0.0, false, false, 3, 0);
    CHECK(d.run && d.sliceMs == 6, "still budget 6, got %u", d.sliceMs);
    d = P::Decide(0.0, true, false, 3, 0);
    CHECK(d.run && d.sliceMs == 4, "moving budget 4, got %u", d.sliceMs);
    // what is left, floored, at least 1
    d = P::Decide(4.5, false, false, 5, 0);
    CHECK(d.run && d.sliceMs == 1, "1.5 ms left -> slice 1, got %u", d.sliceMs);
    // spent: wait, up to kMaxSkips frames in a row
    for (int skips = 0; skips < P::kMaxSkips; skips++) CHECK(!P::Decide(5.5, false, false, 6, skips).run, "must wait at %d skips", skips);
    d = P::Decide(30.0, false, false, 6, P::kMaxSkips);
    CHECK(d.run && d.sliceMs == 1, "after %d skips it runs one stage", P::kMaxSkips);
    CHECK(!P::Decide(30.0, false, false, 6, P::kMaxSkips + 3, true).run, "only one waiting lot is forced per frame");
    // the lot being played always runs, with at least half the budget
    d = P::Decide(30.0, false, true, 0, 0);
    CHECK(d.run && d.sliceMs == 3, "priority lot keeps half of 6, got %u", d.sliceMs);
    d = P::Decide(0.0, false, true, 0, 0);
    CHECK(d.run && d.sliceMs == 6, "priority lot first in frame gets 6, got %u", d.sliceMs);
    // a negative or unknown stage is never held
    CHECK(P::Decide(30.0, false, false, -1, 0).run, "unknown stage must pass");
}

// ---- 2. replay of recorded loads ----
struct Lot {
    double arriveMs = 0;
    std::vector<double> chunks; // build work, one entry per stage crossed (a call's time split over its stages)
};

static std::vector<std::string> SplitCsv(const std::string& line) {
    std::vector<std::string> out;
    std::stringstream ss(line);
    std::string f;
    while (std::getline(ss, f, ',')) out.push_back(f);
    return out;
}

struct Replay {
    std::vector<double> frameMs; // build time of each frame with any build
    std::vector<int> buildFrames; // frames from a lot's first build call to its last
};

// native: each lot's call runs stages until its slice (20 ms) is spent; share: the policy above, camera still
static Replay Simulate(const std::vector<std::vector<Lot>>& sessions, bool share) {
    constexpr double kFrameMs = 16.7;
    Replay out;
    for (const auto& lots : sessions) {
        if (lots.empty()) continue;
        double t0 = lots[0].arriveMs;
        for (const Lot& l : lots) t0 = std::min(t0, l.arriveMs);
        std::vector<size_t> next(lots.size(), 0);
        std::vector<int> start(lots.size()), skips(lots.size(), 0), done(lots.size(), -1);
        bool forced = false;
        for (size_t i = 0; i < lots.size(); i++) start[i] = static_cast<int>((lots[i].arriveMs - t0) / kFrameMs);
        size_t left = lots.size();
        for (int frame = 0; left > 0 && frame < 10000000; frame++) {
            double spent = 0;
            forced = false;
            for (size_t i = 0; i < lots.size(); i++) {
                if (done[i] >= 0 || start[i] > frame) continue;
                double slice = 20.0;
                if (share) {
                    const auto d = P::Decide(spent, false, false, 0, skips[i], forced);
                    if (!d.run) {
                        skips[i]++;
                        continue;
                    }
                    forced = forced || d.forced;
                    skips[i] = 0;
                    slice = d.sliceMs;
                }
                double used = 0;
                while (next[i] < lots[i].chunks.size()) {
                    used += lots[i].chunks[next[i]++];
                    if (used >= slice) break;
                }
                spent += used;
                if (next[i] >= lots[i].chunks.size()) {
                    done[i] = frame - start[i];
                    left--;
                }
            }
            if (spent > 0) out.frameMs.push_back(spent);
        }
        for (int d : done) out.buildFrames.push_back(d);
    }
    return out;
}

static double Pct(std::vector<double> v, double p) {
    if (v.empty()) return 0;
    std::sort(v.begin(), v.end());
    return v[std::min(v.size() - 1, static_cast<size_t>(v.size() * p))];
}

static void ReplayChecks(const std::string& dir) {
    std::vector<std::vector<Lot>> sessions;
    size_t calls = 0;
    for (const auto& e : std::filesystem::directory_iterator(dir)) {
        const std::string name = e.path().filename().string();
        if (name.rfind("LotLoadAnalysis-", 0) != 0 || e.path().extension() != ".csv") continue;
        std::ifstream in(e.path());
        std::string line;
        std::getline(in, line);
        const auto head = SplitCsv(line);
        auto col = [&](const char* n) { return static_cast<size_t>(std::find(head.begin(), head.end(), n) - head.begin()); };
        const size_t cQpc = col("qpc_start"), cFreq = col("qpc_frequency"), cKind = col("kind"), cLot = col("lot_id"), cSb = col("stage_before"),
                     cSa = col("stage_after"), cMs = col("call_elapsed_ms");
        if (cMs >= head.size()) continue;
        std::map<std::string, size_t> index;
        std::vector<Lot> lots;
        while (std::getline(in, line)) {
            const auto f = SplitCsv(line);
            if (f.size() < head.size() || f[cKind] != "load_call") continue;
            const int sb = std::atoi(f[cSb].c_str()), sa = std::atoi(f[cSa].c_str());
            if (sb >= P::kFirstWaitStage) continue;
            const double ms = std::atof(f[cMs].c_str());
            const double t = std::atof(f[cQpc].c_str()) / std::atof(f[cFreq].c_str()) * 1000.0;
            auto it = index.find(f[cLot]);
            if (it == index.end()) {
                it = index.emplace(f[cLot], lots.size()).first;
                lots.push_back({t, {}});
            }
            const int n = std::max(1, std::min(sa, P::kFirstWaitStage) - sb);
            for (int k = 0; k < n; k++) lots[it->second].chunks.push_back(ms / n);
            calls++;
        }
        sessions.push_back(std::move(lots));
    }
    if (!calls) {
        std::printf("replay: no LotLoadAnalysis CSV in %s (skipped)\n", dir.c_str());
        return;
    }
    const Replay native = Simulate(sessions, false), share = Simulate(sessions, true);
    auto over = [](const Replay& r, double ms) { return std::count_if(r.frameMs.begin(), r.frameMs.end(), [ms](double v) { return v > ms; }); };
    auto excess = [](const Replay& r, double ms) {
        double s = 0;
        for (double v : r.frameMs) s += std::max(0.0, v - ms);
        return s;
    };
    auto frames = [](const Replay& r, double p) {
        std::vector<double> v(r.buildFrames.begin(), r.buildFrames.end());
        return Pct(v, p);
    };
    std::printf("replay: %zu build calls, %zu lots\n", calls, native.buildFrames.size());
    for (const auto* r : {&native, &share})
        std::printf("  %-7s frames with builds %6zu  p99 %6.2f ms  >8 ms %5lld  >16 ms %5lld  excess over 8 ms %7.0f ms  | frames to build a lot p50 %3.0f p90 %3.0f\n",
                    r == &native ? "native" : "shared", r->frameMs.size(), Pct(r->frameMs, 0.99), static_cast<long long>(over(*r, 8)),
                    static_cast<long long>(over(*r, 16)), excess(*r, 8), frames(*r, 0.5), frames(*r, 0.9));
    CHECK(over(share, 16) * 4 <= over(native, 16), "shared build time must cut frames over 16 ms by 75%% or more (%lld vs %lld)",
          static_cast<long long>(over(share, 16)), static_cast<long long>(over(native, 16)));
    CHECK(excess(share, 8) * 2 <= excess(native, 8), "shared build time must halve the time over 8 ms");
    CHECK(frames(share, 0.9) <= 60, "90%% of lots must finish their build within 60 frames (1 s), got %.0f", frames(share, 0.9));
}

// Synthetic: 7 lots promoted together, each 3 heavy stages of 7 ms and 12 light ones (the recorded stage costs)
static void SyntheticReplay() {
    std::vector<Lot> lots(7);
    for (Lot& l : lots) {
        l.arriveMs = 0;
        for (int s = 0; s < 15; s++) l.chunks.push_back(s % 5 == 0 ? 7.0 : 0.3);
    }
    const Replay native = Simulate({lots}, false), share = Simulate({lots}, true);
    const double nativeMax = *std::max_element(native.frameMs.begin(), native.frameMs.end());
    const double shareMax = *std::max_element(share.frameMs.begin(), share.frameMs.end());
    std::printf("synthetic 7 lots: native worst frame %.1f ms over %zu frames, shared %.1f ms over %zu frames\n", nativeMax, native.frameMs.size(), shareMax,
                share.frameMs.size());
    CHECK(nativeMax > 60.0, "native worst frame should exceed 60 ms (%.1f)", nativeMax);
    CHECK(shareMax <= P::kStillMs + 2 * 7.0 + 0.3 + 1e-9, "shared worst frame is the budget plus one stage and one forced stage (%.1f)", shareMax);
    CHECK(share.frameMs.size() <= 40, "7 lots build within 40 frames (%zu)", share.frameMs.size());
}

// ---- 3. detailed-lot hysteresis ----
// LotLodScoring: metric = squared distance from the camera target; a detailed lot's metric gets the bias; the lots are
// ranked by metric and the first maxActive below distance^2 are detailed (priority lots left out of the model).
static int Transitions(double keepFactor, double nativeBias, double distance, int maxActive, unsigned seed) {
    struct L { double x, z; bool detailed; };
    std::vector<L> lots;
    for (int i = 0; i < 40; i++) lots.push_back({(i % 8) * 40.0, (i / 8) * 40.0, false});
    uint32_t s = seed;
    auto rnd = [&s] { s = s * 1664525u + 1013904223u; return (s >> 8) / 16777216.0; };
    int changes = 0;
    double cx = 140, cz = 80;
    for (int frame = 0; frame < 3000; frame++) {
        // a pan that drifts and sways around the same area, the camera target moving up to ~2 m a frame
        cx = 140 + 60 * std::sin(frame * 0.01) + (rnd() - 0.5) * 4;
        cz = 80 + 25 * std::sin(frame * 0.023) + (rnd() - 0.5) * 4;
        std::vector<std::pair<double, int>> ranked;
        for (int i = 0; i < static_cast<int>(lots.size()); i++) {
            double m = (lots[i].x - cx) * (lots[i].x - cx) + (lots[i].z - cz) * (lots[i].z - cz);
            if (lots[i].detailed) m = keepFactor > 0 ? m * keepFactor : m - nativeBias;
            ranked.push_back({m, i});
        }
        std::sort(ranked.begin(), ranked.end());
        std::vector<bool> want(lots.size(), false);
        for (int k = 0; k < static_cast<int>(ranked.size()) && k < maxActive; k++)
            if (ranked[k].first < distance * distance) want[ranked[k].second] = true;
        for (size_t i = 0; i < lots.size(); i++)
            if (want[i] != lots[i].detailed) {
                lots[i].detailed = want[i];
                if (frame > 50) changes++; // after the initial fill
            }
    }
    return changes;
}

static void HysteresisChecks() {
    for (const auto& [distance, maxActive] : std::vector<std::pair<double, int>>{{70, 8}, {300, 16}, {150, 12}}) {
        int native = 0, keep = 0;
        for (unsigned seed = 1; seed <= 5; seed++) {
            native += Transitions(0, 8.0, distance, maxActive, seed);
            keep += Transitions(0.81, 0, distance, maxActive, seed);
        }
        std::printf("hysteresis: distance %.0f, %d lots: detail switches native %d, with 0.81 %d\n", distance, maxActive, native, keep);
        CHECK(keep * 2 <= native, "0.81 must at least halve the switches at %.0f / %d (%d vs %d)", distance, maxActive, keep, native);
    }
    // never wider than intended: a detailed lot stays detailed only up to distance / 0.9
    const double d = 300, keepAt = d / std::sqrt(0.81);
    CHECK(std::fabs(keepAt - 333.33) < 0.1, "keep distance %.2f", keepAt);
}

// ---- 4. game bytes (Steam 1.67.2) ----
static void ExeChecks(const std::string& exe) {
    std::ifstream in(exe, std::ios::binary);
    if (!in) {
        std::printf("exe: %s not readable (skipped)\n", exe.c_str());
        return;
    }
    std::vector<uint8_t> b((std::istreambuf_iterator<char>(in)), {});
    auto u32 = [&](size_t o) { uint32_t v; std::memcpy(&v, &b[o], 4); return v; };
    auto u16 = [&](size_t o) { uint16_t v; std::memcpy(&v, &b[o], 2); return v; };
    const size_t pe = u32(0x3C), nsec = u16(pe + 6), opt = pe + 24, sec = opt + u16(pe + 20);
    const uint32_t base = u32(opt + 28), stamp = u32(pe + 8);
    auto fileOf = [&](uint32_t va) -> size_t {
        for (size_t i = 0; i < nsec; i++) {
            const size_t h = sec + i * 40;
            const uint32_t rva = u32(h + 12), vs = u32(h + 8), raw = u32(h + 20);
            if (va - base >= rva && va - base < rva + vs) return raw + (va - base - rva);
        }
        return 0;
    };
    if (stamp != 0x52DEC247) {
        std::printf("exe: not Steam 1.67.2 (timestamp %08X; skipped)\n", stamp);
        return;
    }
    struct Site { uint32_t va; const char* what; std::vector<int> bytes; };
    const std::vector<Site> sites = {
        {0x00C63015, "metric: JZ over the detailed-lot bias, then subss xmm0,[edi+0E0h]", {0x74, 0x14, 0xF3, 0x0F, 0x10, 0x44, 0x24, 0x08, 0xF3, 0x0F, 0x5C, 0x87, 0xE0, 0, 0, 0}},
        {0x00C6C6B5, "scoring: mov edx,[edi]; cmp edx,[0x01184328] (Max Active Lot Threshold)", {0x8B, 0x17, 0x3B, 0x15, 0x28, 0x43, 0x18, 0x01, 0xC6, 0x44, 0x24, 0x1A, 0x01}},
        {0x00AEB306, "renderer update: call 0x00AEA680 (load stages)", {0xE8, 0x75, 0xF3, 0xFF, 0xFF}},
        {0x00C7CEDF, "lot pass: call 0x00AEB2E0 (renderer update)", {0xE8, 0xFC, 0xE3, 0xE6, 0xFF}},
        {0x00AEB2E0, "renderer update: push esi only before the load stages call", {0x56, 0x8B, 0xF1, 0xE8}},
        {0x00AEA6D8, "load stages: mov eax,[0x011ECBC4] after the slice is chosen", {0xA1, 0xC4, 0xCB, 0x1E, 0x01}},
    };
    for (const Site& s : sites) {
        const size_t o = fileOf(s.va);
        bool ok = o && o + s.bytes.size() <= b.size();
        for (size_t k = 0; ok && k < s.bytes.size(); k++) ok = b[o + k] == s.bytes[k];
        CHECK(ok, "exe bytes at %08X: %s", s.va, s.what);
        if (ok) std::printf("exe: %08X ok (%s)\n", s.va, s.what);
    }
    const uint32_t threshold = u32(fileOf(0x01184328));
    std::printf("exe: Max Active Lot Threshold initial value %u (the throttle applies only while Max Active Lots <= it)\n", threshold);
    CHECK(threshold == 12, "threshold initial value 12, got %u", threshold);
}

int main(int argc, char** argv) {
    std::string csvDir, exe;
    for (int i = 1; i + 1 < argc; i++) {
        if (!std::strcmp(argv[i], "--csv-dir")) csvDir = argv[++i];
        else if (!std::strcmp(argv[i], "--exe")) exe = argv[++i];
    }
    PolicyChecks();
    SyntheticReplay();
    HysteresisChecks();
    if (!csvDir.empty()) ReplayChecks(csvDir);
    if (!exe.empty()) ExeChecks(exe);
    std::printf(g_fail ? "Lot Streaming checks: %d failure(s)\n" : "Lot Streaming checks passed\n", g_fail);
    return g_fail ? 1 : 0;
}
