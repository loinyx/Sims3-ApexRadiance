#pragma once
// Apex Radiance log (ApexRadiance_LOG.txt). Lines are queued in memory and written by a small background thread at
// most every half second; warnings and errors are written at once, together with everything queued before them, so a crash report
// ends right after the lines that led to it. Lines logged before Open() (DllMain, early init) are kept and written
// first. Debug lines go to OutputDebugString only, unless verbose logging is on.
#include <string>
#include <source_location>

namespace ApexLog {

enum class Level { Debug, Info, Warning, Error, Critical };

// Moves the previous session's log to <name>.old.txt (ApexRadiance_LOG.old.txt, replacing the one before; not while
// another process has it open for writing), creates the log file and starts the writer thread. Safe to call once; later
// calls return the first result.
bool Open(const std::wstring& path);
// Writes what is queued and stops the writer thread (FreeLibrary only; process exit flushes by itself).
void Close();
// Debug lines also go to the file.
void SetVerbose(bool on);

// Never throws (07/10, players' Runtime Error): a log line that cannot be built (no memory, ...) is dropped, it must not
// take the game down from inside a hook
void Write(Level level, const std::string& text, const std::source_location& where = std::source_location::current()) noexcept;
// Debug lines are wanted: verbose logging is on, or a debugger listens to OutputDebugString
bool DebugOn() noexcept;

} // namespace ApexLog

// The message is not even built when nobody reads debug lines (it runs in hot hooks; its allocation could throw)
#define LOG_DEBUG(msg)                                                                                                                     \
    do {                                                                                                                                   \
        if (::ApexLog::DebugOn()) ::ApexLog::Write(::ApexLog::Level::Debug, (msg));                                                      \
    } while (0)
#define LOG_INFO(msg) ::ApexLog::Write(::ApexLog::Level::Info, (msg))
#define LOG_WARNING(msg) ::ApexLog::Write(::ApexLog::Level::Warning, (msg))
#define LOG_ERROR(msg) ::ApexLog::Write(::ApexLog::Level::Error, (msg))
#define LOG_CRITICAL(msg) ::ApexLog::Write(::ApexLog::Level::Critical, (msg))
