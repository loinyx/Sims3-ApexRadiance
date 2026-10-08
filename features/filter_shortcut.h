#pragma once
// Allocation-free chords, including simultaneous letter keys. No binding is assigned by default.
#include <array>
#include <cstdint>
#include <bit>

struct FilterShortcut {
    std::array<std::uint64_t, 4> keys{};
    static unsigned Normalize(unsigned vk) {
        if (vk == 0xA0 || vk == 0xA1) return 0x10; // Shift
        if (vk == 0xA2 || vk == 0xA3) return 0x11; // Ctrl
        if (vk == 0xA4 || vk == 0xA5) return 0x12; // Alt
        return vk;
    }
    void Add(unsigned vk) { vk = Normalize(vk); if (vk > 6 && vk < 256) keys[vk / 64] |= std::uint64_t{1} << (vk % 64); }
    bool Has(unsigned vk) const { return vk < 256 && (keys[vk / 64] & (std::uint64_t{1} << (vk % 64))); }
    unsigned Count() const { unsigned n = 0; for (auto k : keys) n += std::popcount(k); return n; }
    bool Empty() const { return Count() == 0; }
    bool HasMainKey() const {
        for (unsigned k = 7; k < 256; ++k) if (k != 0x10 && k != 0x11 && k != 0x12 && Has(k)) return true;
        return false;
    }
    template<class Down> bool Matches(unsigned pressed, Down down) const {
        if (!Has(Normalize(pressed)) || !HasMainKey()) return false;
        // Modifiers are exact: plain A must never fire for Ctrl+A.
        for (unsigned k : {0x10u, 0x11u, 0x12u}) if (Has(k) != bool(down(k))) return false;
        for (unsigned k = 7; k < 256; ++k) if (Has(k) && !down(k)) return false;
        return true;
    }
    bool operator==(const FilterShortcut&) const = default;
};
