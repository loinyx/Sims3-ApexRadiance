#pragma once
// Large buffers above the game's 2 GB line (14/10). TS3W.exe is large-address aware: on 64-bit Windows a 32-bit process of
// it has 4 GB of address space, but the game's saves and its script heap need room below 2 GB (a save fails with error 12
// or 13 when the largest free block there is small). Apex's big buffers (room solve captures, kept parts) took 200 MB
// and more of it. HighAllocator puts every allocation of 1 MB or more at the top of the address space (VirtualAlloc with
// MEM_TOP_DOWN, above 2 GB when there is room), the smaller ones on the heap 16-byte aligned: the game's SSE code reads some
// of these buffers with aligned loads (the light evaluation at 0x006BE1CC does movaps on a 0x30 room solve sample), and plain
// operator new gives only 8 bytes on 32-bit.
#include <windows.h>
#include <atomic>
#include <cstddef>
#include <new>

namespace ApexMem {

constexpr size_t kHighFrom = size_t{1} << 20;
constexpr std::align_val_t kHeapAlign{16};
inline std::atomic<size_t> g_highBytes{0}, g_highLowFallback{0}; // bytes held high; allocations that landed below 2 GB anyway

inline void* AllocHigh(size_t bytes) {
    void* p = VirtualAlloc(nullptr, bytes, MEM_RESERVE | MEM_COMMIT | MEM_TOP_DOWN, PAGE_READWRITE);
    if (!p) throw std::bad_alloc();
    g_highBytes.fetch_add(bytes, std::memory_order_relaxed);
    if (reinterpret_cast<uintptr_t>(p) < 0x80000000u) g_highLowFallback.fetch_add(1, std::memory_order_relaxed);
    return p;
}
inline void FreeHigh(void* p, size_t bytes) {
    if (!p) return;
    VirtualFree(p, 0, MEM_RELEASE);
    g_highBytes.fetch_sub(bytes, std::memory_order_relaxed);
}

template <class T> struct HighAllocator {
    using value_type = T;
    HighAllocator() noexcept = default;
    template <class U> HighAllocator(const HighAllocator<U>&) noexcept {}
    T* allocate(size_t n) {
        if (n > static_cast<size_t>(-1) / sizeof(T)) throw std::bad_alloc();
        const size_t bytes = n * sizeof(T);
        if (bytes >= kHighFrom) return static_cast<T*>(AllocHigh(bytes));
        return static_cast<T*>(::operator new(bytes, kHeapAlign));
    }
    void deallocate(T* p, size_t n) noexcept {
        const size_t bytes = n * sizeof(T);
        if (bytes >= kHighFrom) FreeHigh(p, bytes);
        else ::operator delete(p, kHeapAlign);
    }
    template <class U> bool operator==(const HighAllocator<U>&) const noexcept { return true; }
    template <class U> bool operator!=(const HighAllocator<U>&) const noexcept { return false; }
};

} // namespace ApexMem
