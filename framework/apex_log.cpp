#include "apex_log.h"
#include "apex_version.h"
#include <windows.h>
#include <atomic>
#include <cstdio>
#include <mutex>

namespace ApexLog {
namespace {

constexpr DWORD kWriteIntervalMs = 500;          // queued Info lines reach the file at most this late
constexpr size_t kMaxQueuedBytes = 256 * 1024;   // beyond this the logging thread writes the queue itself
constexpr size_t kMaxEarlyBytes = 1024 * 1024;   // lines kept before Open (a failed Open must not grow forever)

std::mutex g_fileLock;             // held across every write (keeps them in order); always taken before g_lock
std::mutex g_lock;
std::string g_queue;               // lines not written yet (guarded by g_lock)
HANDLE g_file = INVALID_HANDLE_VALUE; // changed only under both locks
HANDLE g_wake = nullptr;           // writer thread: set to stop it
bool g_opened = false;             // Open ran (successfully or not)
bool g_openResult = false;
std::atomic<bool> g_verbose{false};

// Caller holds g_fileLock and g_lock (Open, Close, exit: run once).
void WriteQueueLocked() {
    if (g_queue.empty() || g_file == INVALID_HANDLE_VALUE) return;
    DWORD written = 0;
    WriteFile(g_file, g_queue.data(), static_cast<DWORD>(g_queue.size()), &written, nullptr);
    g_queue.clear();
}

// Caller holds g_fileLock only. g_lock covers just the swap, so a line logged during WriteFile (render thread) does not
// wait for the disk.
void WriteQueue() {
    std::string pending;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        if (g_file == INVALID_HANDLE_VALUE) return;
        pending.swap(g_queue);
    }
    if (pending.empty()) return;
    DWORD written = 0;
    WriteFile(g_file, pending.data(), static_cast<DWORD>(pending.size()), &written, nullptr);
}

DWORD WINAPI WriterThread(LPVOID param) {
    const HANDLE wake = static_cast<HANDLE>(param);
    for (;;) {
        const bool stop = WaitForSingleObject(wake, kWriteIntervalMs) == WAIT_OBJECT_0;
        {
            std::lock_guard<std::mutex> fileLock(g_fileLock);
            WriteQueue();
        }
        if (stop) break;
    }
    CloseHandle(wake);
    return 0;
}

const char* LevelName(Level level) {
    switch (level) {
    case Level::Debug: return "DEBUG";
    case Level::Info: return "INFO";
    case Level::Warning: return "WARN";
    case Level::Error: return "ERROR";
    case Level::Critical: return "CRITICAL";
    }
    return "?";
}

const char* BaseName(const char* path) {
    const char* base = path;
    for (const char* p = path; *p; ++p)
        if (*p == '\\' || *p == '/') base = p + 1;
    return base;
}

// At process exit DllMain does not close the log (the loader may hold other threads' locks), so what is still queued is
// written by this static object's destructor. Another thread may have died holding a lock: never wait for it.
struct ExitWriter {
    ~ExitWriter() {
        if (!g_fileLock.try_lock()) return;
        if (g_lock.try_lock()) {
            WriteQueueLocked();
            g_lock.unlock();
        }
        g_fileLock.unlock();
    }
} g_exitWriter;

} // namespace

bool Open(const std::wstring& path) {
    std::lock_guard<std::mutex> fileLock(g_fileLock);
    std::lock_guard<std::mutex> lock(g_lock);
    if (g_opened) return g_openResult;
    g_opened = true;
    // The previous session's log is kept as <name>.old.txt (replacing the one before); none yet or locked: truncated as before.
    // Still open for writing (a second game process, or one still exiting): the probe fails and nothing is moved, so that
    // process keeps writing its own log and this one's create fails as before
    const HANDLE probe = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (probe != INVALID_HANDLE_VALUE) {
        CloseHandle(probe);
        std::wstring old = path;
        const size_t dot = old.find_last_of(L'.'), name = old.find_last_of(L"\\/") + 1; // (npos + 1 = 0: no folder)
        old.insert(dot != std::wstring::npos && dot > name ? dot : old.size(), L".old");
        MoveFileExW(path.c_str(), old.c_str(), MOVEFILE_REPLACE_EXISTING);
    }
    g_file = CreateFileW(path.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (g_file == INVALID_HANDLE_VALUE) {
        g_queue.clear();
        return g_openResult = false;
    }
    SYSTEMTIME t{};
    GetLocalTime(&t);
    char header[160];
    std::snprintf(header, sizeof header, APEX_PRODUCT_NAME " Log - started %04u-%02u-%02u %02u:%02u:%02u\r\n"
                                         "------------------------------------------------------------\r\n",
                  t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond);
    g_queue.insert(0, header);
    WriteQueueLocked();
    if (HANDLE wake = CreateEventW(nullptr, TRUE, FALSE, nullptr)) {
        if (HANDLE thread = CreateThread(nullptr, 0, WriterThread, wake, 0, nullptr)) {
            CloseHandle(thread);
            g_wake = wake;
        } else {
            CloseHandle(wake); // no writer thread: every line is written at once
        }
    }
    return g_openResult = true;
}

void Close() {
    std::lock_guard<std::mutex> fileLock(g_fileLock);
    std::lock_guard<std::mutex> lock(g_lock);
    if (g_wake) {
        SetEvent(g_wake); // the thread wakes, writes nothing new and closes its event; never waited for (DllMain)
        g_wake = nullptr;
    }
    WriteQueueLocked();
    if (g_file != INVALID_HANDLE_VALUE) {
        CloseHandle(g_file);
        g_file = INVALID_HANDLE_VALUE;
    }
}

void SetVerbose(bool on) { g_verbose.store(on, std::memory_order_relaxed); }

bool DebugOn() noexcept { return g_verbose.load(std::memory_order_relaxed) || IsDebuggerPresent(); }

namespace {
void WriteLine(Level level, const std::string& text, const std::source_location& where) {
    SYSTEMTIME t{};
    GetLocalTime(&t);
    char prefix[64];
    std::snprintf(prefix, sizeof prefix, "%02u:%02u:%02u.%03u [%s] ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, LevelName(level));
    std::string line = prefix;
    line += text;
    if (level >= Level::Error || level == Level::Debug) {
        char loc[128];
        std::snprintf(loc, sizeof loc, " (%s:%u)", BaseName(where.file_name()), static_cast<unsigned>(where.line()));
        line += loc;
    }
    line += "\r\n";
    OutputDebugStringA(line.c_str());
    if (level == Level::Debug && !g_verbose.load(std::memory_order_relaxed)) return;

    bool writeNow;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        if (!g_opened) { // before Open: keep it (bounded)
            if (g_queue.size() + line.size() <= kMaxEarlyBytes) g_queue += line;
            return;
        }
        if (g_file == INVALID_HANDLE_VALUE) return;
        g_queue += line;
        writeNow = level >= Level::Warning || !g_wake || g_queue.size() >= kMaxQueuedBytes;
    }
    if (!writeNow) return;
    // Once g_fileLock is ours, any write that took this line first has finished
    std::lock_guard<std::mutex> fileLock(g_fileLock);
    WriteQueue();
}
} // namespace

void Write(Level level, const std::string& text, const std::source_location& where) noexcept {
    // 07/10, players' Runtime Error: logging runs inside game hooks and when memory is low; a line that cannot be built or
    // queued (bad_alloc, a mutex error) is dropped instead of escaping into the game
    try {
        WriteLine(level, text, where);
    } catch (...) {
    }
}

} // namespace ApexLog
