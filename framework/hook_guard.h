#pragma once
// Exception safety nets where the game or the D3D9 runtime calls into Apex (07/10, players' "Microsoft Visual C++ Runtime
// Library: Runtime Error! ... terminate it in an unusual way"). A C++ exception (bad_alloc, length_error, a filesystem
// error ...) that leaves an Apex hook unwinds into game frames that cannot handle it, and the game ends. Each net catches
// it, notes where without allocating (the catch may run with no memory left), lets the game do what it would have done
// without Apex, and turns that one call site off so it does not throw again every frame. The notes reach the log later,
// from Present (ReportPending), outside any catch block.
#include "crash_report.h"
#include "gpu_trace.h"
#include <atomic>
#include <thread>
#include <utility>

namespace HookGuard {

// Notes a caught exception at `where` (copied into fixed slots: no heap, safe inside a catch block)
void Note(const char* where) noexcept;
// Same, naming the code address too ("where (code at ApexRadiance.asi+0x1234)"), for callbacks known by address only
void NoteAt(const char* where, const void* code) noexcept;
// Logs the notes not logged yet. Allocates: render thread at Present, never from a catch block.
void ReportPending() noexcept;

// Runs f(); on a C++ exception notes `where`, returns `fallback` (what the game gets without Apex) and turns this call site
// off: later calls return `fallback` at once. One switch per call site, as every lambda expression is its own type (pass a
// lambda, not a function pointer).
template <class R, class F> R Run(const char* where, R fallback, F&& f) noexcept {
    static std::atomic<bool> s_off{false};
    if (s_off.load(std::memory_order_relaxed)) return fallback;
    GpuTrace::MarkIfOn(where); // (the last steps of a session that does not close)
    try {
        return static_cast<R>(std::forward<F>(f)());
    } catch (...) {
        s_off.store(true, std::memory_order_relaxed);
        Note(where);
        return fallback;
    }
}
// Same for an f() with no result: false when it threw now or before (the site is off)
template <class F> bool Run(const char* where, F&& f) noexcept {
    static std::atomic<bool> s_off{false};
    if (s_off.load(std::memory_order_relaxed)) return false;
    GpuTrace::MarkIfOn(where);
    try {
        std::forward<F>(f)();
        return true;
    } catch (...) {
        s_off.store(true, std::memory_order_relaxed);
        Note(where);
        return false;
    }
}

// Runs f(); a C++ exception is caught and noted, and the next call runs f() again (for rare calls that must not be turned
// off for good, such as the work around a device Reset). False when it threw.
template <class F> bool Try(const char* where, F&& f) noexcept {
    GpuTrace::MarkIfOn(where);
    try {
        std::forward<F>(f)();
        return true;
    } catch (...) {
        Note(where);
        return false;
    }
}

// Starts a detached std::thread running f(), with Apex's terminate handler set on it and a catch-all around f: a C++
// exception in f, or a thread that cannot be created (std::system_error, bad_alloc), is noted at `where` (a string
// literal) instead of ending the game. False when the thread did not start.
template <class F> bool StartDetached(const char* where, F&& f) noexcept {
    try {
        std::thread([where, fn = std::forward<F>(f)]() mutable noexcept {
            CrashReport::ThreadStart();
            try {
                fn();
            } catch (...) {
                Note(where);
            }
        }).detach();
        return true;
    } catch (...) {
        Note(where);
        return false;
    }
}

// Callbacks turned off after they threw (a function pointer list cannot drop them from inside the catch: that may
// allocate). Fixed size, lock-free; past N entries the extra ones keep running (and keep being caught).
template <int N> class OffList {
  public:
    bool Has(const void* p) const noexcept {
        if (count_.load(std::memory_order_acquire) == 0) return false;
        for (const auto& s : slots_)
            if (s.load(std::memory_order_relaxed) == p) return true;
        return false;
    }
    void Add(const void* p) noexcept {
        for (auto& s : slots_) {
            const void* expected = nullptr;
            if (s.compare_exchange_strong(expected, p) || expected == p) {
                count_.fetch_add(1, std::memory_order_release);
                return;
            }
        }
    }

  private:
    std::atomic<const void*> slots_[N] = {};
    std::atomic<int> count_{0};
};

} // namespace HookGuard
