#pragma once
// Register ranges of the device's SetPixelShaderConstantF / SetVertexShaderConstantF calls, by calling site (developer
// mode; real-time GI S0-14). Read-only: it never changes a call. D3D9Hooks feeds it from its two detours with the
// caller's return address and prints it with the frame profiler report and an [ConstRange] log line.
//
// Why: a frame-constant block for Apex's patched shaders needs registers that nothing else writes. The desk check
// (docs/features/realtime-gi/research/13-desk-batching-constants.md) found the game's shaders read pixel c0-c24; the
// parameter tables (§2.5) put the game's vertex writes at c0-c241 (pet and ghost fur up to c241). It reserves pixel
// c208-c223 and vertex c242-c251. This counts every write by owner (the game's exe,
// Apex, any other module) so one run shows whether a write of the game or another mod reaches those registers.
//
// No Windows dependency: the owner of a new calling site comes from the caller's classify function, so the offline
// check (tools/const_range_check) runs the same code. Thread-safety: the counters are plain relaxed load + store (a
// statistic, as D3D9Hooks' call counters); a new site claims its slot with one compare-exchange.
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <format>
#include <string>
#include <vector>

namespace ConstRange {

enum Kind : int { kPs, kVs, kKinds };
enum Owner : int { kGame, kApex, kOther, kOwners };
inline constexpr const char* kKindNames[kKinds] = {"PS", "VS"};
inline constexpr const char* kOwnerNames[kOwners] = {"game", "Apex", "other"};
inline constexpr uint32_t kRegs[kKinds] = {224, 256}; // ps_3_0 and vs_3_0 float registers
// The frame block's registers [lo, hi): pixel c208-c223, vertex c242-c251 (research 13 §2.4, §2.5). Apex holds pixel
// c200-c204 and vertex c252 / c255 already, outside these ranges.
inline constexpr uint32_t kReservedLo[kKinds] = {208, 242};
inline constexpr uint32_t kReservedHi[kKinds] = {224, 252};
inline constexpr int kSites = 256; // calling sites kept; later new ones are only counted in g_droppedSites

struct Site {
    std::atomic<uint64_t> key{0}; // (return address << 1) | kind; 0 = free
    std::atomic<int> owner{kOther};
    std::atomic<uint32_t> calls{0}, lo{0xFFFFFFFFu}, hi{0}, reserved{0}, outOfRange{0};
    std::atomic<bool> reported{false}; // its reserved-range write was logged
};

inline void Add(std::atomic<uint32_t>& a, uint32_t v) { a.store(a.load(std::memory_order_relaxed) + v, std::memory_order_relaxed); }
inline void Max(std::atomic<uint32_t>& a, uint32_t v) {
    if (v > a.load(std::memory_order_relaxed)) a.store(v, std::memory_order_relaxed);
}
inline void Min(std::atomic<uint32_t>& a, uint32_t v) {
    if (v < a.load(std::memory_order_relaxed)) a.store(v, std::memory_order_relaxed);
}
inline std::string Reg(uint32_t r) { return std::format("c{}", r); }
inline std::string Span(uint32_t lo, uint32_t last) { return lo == last ? Reg(lo) : Reg(lo) + "-" + Reg(last); } // inclusive

class Histogram {
public:
    // One call: start register r, count n, from the calling site ret. classify(ret) -> Owner, asked once per new site.
    template <class Classify> void Note(Kind kind, uintptr_t ret, uint32_t r, uint32_t n, Classify&& classify) {
        if (n == 0) return;
        Site* s = Find(kind, ret, classify);
        const uint64_t endWide = static_cast<uint64_t>(r) + n;
        const uint32_t end = static_cast<uint32_t>(std::min<uint64_t>(endWide, kRegs[kind])); // exclusive
        const int owner = s ? s->owner.load(std::memory_order_relaxed) : kOther;
        if (s) {
            Add(s->calls, 1);
            Min(s->lo, r);
            Max(s->hi, static_cast<uint32_t>(std::min<uint64_t>(endWide, 0xFFFFFFFFu)));
        }
        if (endWide > kRegs[kind]) {
            Add(m_outOfRange[kind], 1);
            if (s) Add(s->outOfRange, 1);
        }
        if (r < kRegs[kind]) {
            Add(m_cover[kind][owner][r], 1); // difference array: [r, end) covered once more
            Add(m_coverEnd[kind][owner][end], 1);
            Max(m_maxEnd[kind][owner], end);
            Max(m_frameEnd[kind][owner], end);
        }
        if (owner != kApex && r < kReservedHi[kind] && endWide > kReservedLo[kind]) {
            Add(m_reserved[kind], 1);
            if (s) Add(s->reserved, 1);
            m_newReserved.store(true, std::memory_order_relaxed);
        }
    }

    // Render thread, once a frame (Present): the frame's highest register per kind and owner moves to the last frame's.
    void EndFrame() {
        for (int k = 0; k < kKinds; k++)
            for (int o = 0; o < kOwners; o++) {
                const uint32_t e = m_frameEnd[k][o].exchange(0, std::memory_order_relaxed);
                m_lastFrameEnd[k][o] = e;
                m_frameMaxEnd[k][o] = std::max(m_frameMaxEnd[k][o], e);
            }
        m_frames++;
    }
    uint64_t Frames() const { return m_frames; }
    uint32_t Version() const { return m_version.load(std::memory_order_relaxed); }
    uint32_t HighestEnd(Kind k, Owner o) const { return m_maxEnd[k][o].load(std::memory_order_relaxed); } // 0 = none
    uint32_t ReservedWrites(Kind k) const { return m_reserved[k].load(std::memory_order_relaxed); }
    uint32_t OutOfRange(Kind k) const { return m_outOfRange[k].load(std::memory_order_relaxed); }

    // Sites with a reserved-range write not reported yet, each once: "PS c208-c215 by <site> (12 calls)".
    template <class Describe> std::vector<std::string> TakeNewReserved(Describe&& describe) {
        std::vector<std::string> out;
        if (!m_newReserved.exchange(false, std::memory_order_relaxed)) return out;
        for (Site& s : m_sites) {
            const uint64_t key = s.key.load(std::memory_order_acquire);
            if (!key || !s.reserved.load(std::memory_order_relaxed) || s.reported.exchange(true)) continue;
            const Kind k = static_cast<Kind>(key & 1);
            out.push_back(std::format("{} {} by {} ({}), {} of its {} calls", kKindNames[k], Span(s.lo.load(), s.hi.load() - 1),
                describe(static_cast<uintptr_t>(key >> 1)), kOwnerNames[s.owner.load()], s.reserved.load(), s.calls.load()));
        }
        return out;
    }

    // One line: the highest register written per kind and owner, and the reserved ranges' foreign writes.
    std::string Summary() const {
        std::string s;
        for (int k = 0; k < kKinds; k++) {
            s += std::format("{}{}: highest written", k ? "; " : "", kKindNames[k]);
            for (int o = 0; o < kOwners; o++) s += std::format(" {} {}", kOwnerNames[o], EndText(m_maxEnd[k][o].load(std::memory_order_relaxed)));
            s += std::format(", reserved {} written by others {} times", Span(kReservedLo[k], kReservedHi[k] - 1), m_reserved[k].load());
            if (const uint32_t bad = m_outOfRange[k].load()) s += std::format(", {} calls past c{}", bad, kRegs[k] - 1);
        }
        return s + std::format("; {} calling sites, {} frames", SiteCount(), m_frames);
    }

    // The report section: per kind and owner the registers written (runs) and the highest a frame; then every site.
    template <class Describe> std::string Report(Describe&& describe) const {
        if (!SiteCount()) return {};
        std::string s = "Shader constant registers written (Set*ShaderConstantF by calling site; real-time GI S0-14):\n   " + Summary() + "\n";
        for (int k = 0; k < kKinds; k++)
            for (int o = 0; o < kOwners; o++) {
                if (!m_maxEnd[k][o].load()) continue;
                s += std::format("   {} {:<5} registers {} | highest a frame: last {}, most {}\n", kKindNames[k], kOwnerNames[o], Runs(static_cast<Kind>(k), o),
                    EndText(m_lastFrameEnd[k][o]), EndText(m_frameMaxEnd[k][o]));
            }
        struct Row {
            int kind, owner;
            uint32_t calls, lo, hi, reserved, outOfRange;
            uintptr_t ret;
        };
        std::vector<Row> rows;
        for (const Site& x : m_sites)
            if (const uint64_t key = x.key.load(std::memory_order_acquire))
                rows.push_back({static_cast<int>(key & 1), x.owner.load(), x.calls.load(), x.lo.load(), x.hi.load(), x.reserved.load(), x.outOfRange.load(), static_cast<uintptr_t>(key >> 1)});
        std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
            return a.kind != b.kind ? a.kind < b.kind : a.owner != b.owner ? a.owner < b.owner : a.calls > b.calls;
        });
        s += "   site: kind owner calls registers [reserved writes] [past the last register]\n";
        for (const Row& r : rows) {
            s += std::format("   {:<36} {} {:<5} {:>10} {}", describe(r.ret), kKindNames[r.kind], kOwnerNames[r.owner], r.calls, Span(r.lo, r.hi - 1));
            if (r.reserved) s += std::format(" reserved {}", r.reserved);
            if (r.outOfRange) s += std::format(" past {}", r.outOfRange);
            s += "\n";
        }
        if (const uint32_t d = m_droppedSites.load()) s += std::format("   {} calls from sites past the table's {} not listed (counted above)\n", d, kSites);
        return s;
    }

    int SiteCount() const {
        int n = 0;
        for (const Site& x : m_sites) n += x.key.load(std::memory_order_relaxed) != 0;
        return n;
    }

private:
    template <class Classify> Site* Find(Kind kind, uintptr_t ret, Classify& classify) {
        const uint64_t key = (static_cast<uint64_t>(ret) << 1) | static_cast<uint64_t>(kind);
        uint32_t h = static_cast<uint32_t>(key * 0x9E3779B97F4A7C15ull >> 40);
        for (int i = 0; i < kSites; i++, h++) {
            Site& s = m_sites[h % kSites];
            uint64_t cur = s.key.load(std::memory_order_acquire);
            if (cur == key) return &s;
            if (cur) continue;
            // free: classify first, so no reader sees the key with the default owner for long
            const int owner = classify(ret);
            if (s.key.compare_exchange_strong(cur, key, std::memory_order_acq_rel)) {
                s.owner.store(owner, std::memory_order_relaxed);
                m_version.fetch_add(1, std::memory_order_relaxed);
                return &s;
            }
            if (cur == key) return &s; // another thread took it for the same site
        }
        Add(m_droppedSites, 1);
        return nullptr;
    }
    static std::string EndText(uint32_t end) { return end ? Reg(end - 1) : std::string("none"); }
    // "c0-c24, c40-c113": the registers at least one call of this kind and owner wrote
    std::string Runs(Kind k, int o) const {
        std::string s;
        int64_t depth = 0;
        int runStart = -1;
        for (uint32_t r = 0; r <= kRegs[k]; r++) {
            depth += static_cast<int64_t>(r < kRegs[k] ? m_cover[k][o][r].load() : 0) - m_coverEnd[k][o][r].load();
            const bool on = r < kRegs[k] && depth > 0;
            if (on && runStart < 0) runStart = static_cast<int>(r);
            if (!on && runStart >= 0) {
                s += std::format("{}{}", s.empty() ? "" : ", ", Span(static_cast<uint32_t>(runStart), r - 1));
                runStart = -1;
            }
        }
        return s.empty() ? std::string("none") : s;
    }

    Site m_sites[kSites];
    std::atomic<uint32_t> m_cover[kKinds][kOwners][257]{}, m_coverEnd[kKinds][kOwners][257]{};
    std::atomic<uint32_t> m_maxEnd[kKinds][kOwners]{}, m_frameEnd[kKinds][kOwners]{};
    uint32_t m_lastFrameEnd[kKinds][kOwners]{}, m_frameMaxEnd[kKinds][kOwners]{}; // render thread (EndFrame)
    std::atomic<uint32_t> m_reserved[kKinds]{}, m_outOfRange[kKinds]{}, m_droppedSites{0}, m_version{0};
    std::atomic<bool> m_newReserved{false};
    uint64_t m_frames = 0;
};

} // namespace ConstRange
