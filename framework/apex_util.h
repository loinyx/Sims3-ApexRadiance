#pragma once
// Small string and file helpers shared by the framework.
#include <windows.h>
#include <atomic>
#include <string>

namespace ApexUtil {

// Scoped SRW locks (07/10, players' Runtime Error): a C++ exception inside a locked section must not leave the lock held,
// or every later caller (game threads included) waits forever.
class SrwShared {
  public:
    explicit SrwShared(SRWLOCK& lock) noexcept : lock_(lock) { AcquireSRWLockShared(&lock_); }
    ~SrwShared() { ReleaseSRWLockShared(&lock_); }
    SrwShared(const SrwShared&) = delete;
    SrwShared& operator=(const SrwShared&) = delete;

  private:
    SRWLOCK& lock_;
};
class SrwExclusive {
  public:
    explicit SrwExclusive(SRWLOCK& lock) noexcept : lock_(lock) { AcquireSRWLockExclusive(&lock_); }
    ~SrwExclusive() { ReleaseSRWLockExclusive(&lock_); }
    SrwExclusive(const SrwExclusive&) = delete;
    SrwExclusive& operator=(const SrwExclusive&) = delete;

  private:
    SRWLOCK& lock_;
};

// (2.15.3) One more on a statistics counter without a locked add: a plain load and store (a lock xadd costs about 20 cycles
// and a full barrier; these run per lamp per sample or per draw). Two threads counting at once can lose a count, so this is
// only for numbers shown in the status and logs, never for a generation or anything a cache or a decision reads.
template <typename T> inline void CountOne(std::atomic<T>& c) { c.store(c.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed); }

inline std::wstring ToWide(const std::string& utf8) {
    if (utf8.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring out(static_cast<size_t>(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, utf8.data(), static_cast<int>(utf8.size()), out.data(), n);
    return out;
}

inline std::string ToUtf8(const std::wstring& wide) {
    if (wide.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(n > 0 ? n : 0), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, wide.data(), static_cast<int>(wide.size()), out.data(), n, nullptr, nullptr);
    return out;
}

// Whole file as bytes; false when it cannot be opened or read.
bool ReadFileBytes(const std::wstring& path, std::string& out);
// Writes data to path through a temporary file in the same folder and an atomic replace, so a crash or a full disk never
// leaves a half-written file behind.
bool WriteFileAtomic(const std::wstring& path, const std::string& data, std::string* error = nullptr);
bool FileExists(const std::wstring& path);

} // namespace ApexUtil
