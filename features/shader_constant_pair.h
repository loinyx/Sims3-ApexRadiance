#pragma once
#include <cstdint>
#include <cstring>

namespace ShaderConstantPair {
// Preserve the intervening rows when two constants share a small device block.
class Block {
  public:
    template <class Get> Block(uint32_t a, uint32_t b, Get get) : a_(a), b_(b) {
        if (b > a && b - a < 4 && get(a, old_, b - a + 1)) count_ = b - a + 1;
        else {
            std::memset(old_, 0, sizeof old_);
            get(a, old_, 1);
            get(b, old_ + 12, 1);
        }
    }
    template <class Set> void Apply(const float a[4], const float b[4], Set set) const {
        if (count_) {
            float rows[16];
            std::memcpy(rows, old_, count_ * 4 * sizeof(float));
            std::memcpy(rows, a, 4 * sizeof(float));
            std::memcpy(rows + (count_ - 1) * 4, b, 4 * sizeof(float));
            if (set(a_, rows, count_)) return;
        }
        set(a_, a, 1);
        set(b_, b, 1);
    }
    template <class Set> void Restore(Set set) const {
        if (count_ && set(a_, old_, count_)) return;
        set(b_, old_ + (count_ ? (count_ - 1) * 4 : 12), 1);
        set(a_, old_, 1);
    }
  private:
    uint32_t a_, b_, count_ = 0;
    float old_[16] = {};
};
} // namespace ShaderConstantPair
