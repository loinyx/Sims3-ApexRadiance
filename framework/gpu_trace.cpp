// What Apex was doing when a session ended without closing (gpu_trace.h). Credits: @loinyx
#include "gpu_trace.h"
#include "apex_log.h"
#include "apex_paths.h"
#include "apex_util.h"
#include <windows.h>
#include <winevt.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <format>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#pragma comment(lib, "wevtapi.lib")

namespace GpuTrace {
namespace {

constexpr uint32_t kSlots = 256;
constexpr uint32_t kReported = 48; // the last steps put in the log
constexpr char kMagic[8] = { 'A', 'P', 'X', 'T', 'R', 'C', '1', 0 };
enum State : uint32_t { kRunning = 1, kClosed = 2, kCrashReport = 3 };

struct Entry
{
    uint32_t frame, ms, count;
    char text[48];
};
struct TraceFile
{
    char magic[8];
    uint32_t pid;
    volatile uint32_t state; // State (gpu_trace.h: g_state)
    uint64_t start; // FILETIME (UTC) of the session's start
    uint32_t frame;
    uint32_t head;  // entries written (the next one goes to head % kSlots)
    Entry e[kSlots];
};

TraceFile* g_f = nullptr;
ULONGLONG g_startMs = 0;
std::atomic<const char*> g_lastKey{nullptr}; // the last step (its pointer: a repeat adds to its count)
std::atomic<uint32_t> g_lastSlot{0};

std::string LocalTime(uint64_t fileTime)
{
    FILETIME ft{ (DWORD)fileTime, (DWORD)(fileTime >> 32) }, lt{};
    SYSTEMTIME st{};
    if (!FileTimeToLocalFileTime(&ft, &lt) || !FileTimeToSystemTime(&lt, &st)) return "?";
    return std::format("{:04}-{:02}-{:02} {:02}:{:02}:{:02}", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
}

std::wstring XmlTime(uint64_t fileTime) // for an event log query (UTC)
{
    FILETIME ft{ (DWORD)fileTime, (DWORD)(fileTime >> 32) };
    SYSTEMTIME st{};
    FileTimeToSystemTime(&ft, &st);
    return std::format(L"{:04}-{:02}-{:02}T{:02}:{:02}:{:02}.000Z", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
}

// The text between <tag ...> and </tag> (or the attribute's value), from position at
std::wstring Between(const std::wstring& xml, const std::wstring& open, const std::wstring& close, size_t& at)
{
    const size_t a = xml.find(open, at);
    if (a == std::wstring::npos) { at = std::wstring::npos; return {}; }
    const size_t b = xml.find(close, a + open.size());
    if (b == std::wstring::npos) { at = std::wstring::npos; return {}; }
    at = b + close.size();
    return xml.substr(a + open.size(), b - a - open.size());
}

// One line per event of the query since the session's start: time, source, id, its data (the text a driver writes there)
void LogEvents(const wchar_t* channel, const std::wstring& providers, uint64_t since, const char* what, const wchar_t* mustContain)
{
    const std::wstring query = L"*[System[(" + providers + L") and TimeCreated[@SystemTime>='" + XmlTime(since) + L"']]]";
    EVT_HANDLE q = EvtQuery(nullptr, channel, query.c_str(), EvtQueryChannelPath | EvtQueryForwardDirection);
    if (!q) return;
    EVT_HANDLE ev[16];
    DWORD got = 0;
    int lines = 0;
    while (lines < 24 && EvtNext(q, 16, ev, 1000, 0, &got))
    {
        for (DWORD i = 0; i < got; ++i)
        {
            DWORD used = 0, props = 0;
            EvtRender(nullptr, ev[i], EvtRenderEventXml, 0, nullptr, &used, &props);
            std::wstring xml(used / sizeof(wchar_t) + 1, L'\0');
            if (used && EvtRender(nullptr, ev[i], EvtRenderEventXml, (DWORD)(xml.size() * sizeof(wchar_t)), xml.data(), &used, &props) && lines < 24)
            {
                size_t at = 0;
                const std::wstring provider = Between(xml, L"Provider Name='", L"'", at);
                at = 0;
                std::wstring id = Between(xml, L"<EventID", L"</EventID>", at);
                if (const size_t gt = id.find(L'>'); gt != std::wstring::npos) id = id.substr(gt + 1);
                at = 0;
                const std::wstring time = Between(xml, L"SystemTime='", L"'", at);
                std::wstring data;
                at = 0;
                while (at != std::wstring::npos)
                {
                    std::wstring d = Between(xml, L"<Data", L"</Data>", at);
                    if (at == std::wstring::npos) break;
                    if (const size_t gt = d.find(L'>'); gt != std::wstring::npos) d = d.substr(gt + 1);
                    if (d.empty() || d == L"0") continue;
                    data += (data.empty() ? L"" : L" | ") + d;
                }
                if (!mustContain || data.find(mustContain) != std::wstring::npos)
                {
                    std::string line = std::format("[Session] {} {}: {} {}: {}", what, ApexUtil::ToUtf8(time.substr(0, 19)), ApexUtil::ToUtf8(provider),
                                                   ApexUtil::ToUtf8(id), ApexUtil::ToUtf8(data));
                    if (line.size() > 400) line = line.substr(0, 400) + "...";
                    LOG_WARNING(line);
                    ++lines;
                }
            }
            EvtClose(ev[i]);
        }
    }
    EvtClose(q);
}

// The RTX Remix bridge's log of that session (written by its server, which starts with the game's device: after this)
void LogBridge(uint64_t since)
{
    wchar_t exe[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
    std::wstring path(exe, n);
    path.resize(path.find_last_of(L"\\/") + 1);
    path += L"NvRemixBridge_d3d9.log";
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a)) return;
    const uint64_t written = ((uint64_t)a.ftLastWriteTime.dwHighDateTime << 32) | a.ftLastWriteTime.dwLowDateTime;
    FILETIME created{}, x1{}, x2{}, x3{};
    GetProcessTimes(GetCurrentProcess(), &created, &x1, &x2, &x3);
    const uint64_t now = ((uint64_t)created.dwHighDateTime << 32) | created.dwLowDateTime;
    if (written < since) return; // not that session's
    if (written >= now) { LOG_INFO("[Session] The RTX Remix bridge's log of that session was already written over by this start"); return; }
    std::ifstream f(path, std::ios::binary);
    std::vector<std::pair<std::string, int>> kept; // its errors and warnings, a line repeated at once counted
    std::string line;
    while (std::getline(f, line))
    {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.rfind("err:", 0) != 0 && line.rfind("warn:", 0) != 0) continue;
        if (!kept.empty() && kept.back().first == line) { ++kept.back().second; continue; }
        kept.emplace_back(line.substr(0, 300), 1);
    }
    if (kept.empty()) { LOG_INFO("[Session] The RTX Remix bridge's log of that session has no error or warning"); return; }
    LOG_WARNING(std::format("[Session] The RTX Remix bridge's log of that session ({} errors and warnings; the last ones):", kept.size()));
    for (size_t i = kept.size() > 12 ? kept.size() - 12 : 0; i < kept.size(); ++i)
        LOG_WARNING(kept[i].second > 1 ? std::format("    {} (x{})", kept[i].first, kept[i].second) : "    " + kept[i].first);
}

void Report(const TraceFile& t)
{
    LOG_WARNING(std::format("[Session] The last session (process {}, started {}) did not close: it ended at frame {} with no crash report (a "
                            "hang ended from the Task Manager, a graphics driver fault, or the power). Its last steps, oldest first:",
                            t.pid, LocalTime(t.start), t.frame));
    const uint32_t n = t.head < kReported ? t.head : kReported;
    for (uint32_t k = t.head - n; k < t.head; ++k)
    {
        const Entry& e = t.e[k % kSlots];
        char text[49] = {};
        std::memcpy(text, e.text, sizeof e.text);
        LOG_WARNING(e.count > 1 ? std::format("    frame {}, {:.1f} s: {} (x{})", e.frame, e.ms / 1000.0, text, e.count)
                                : std::format("    frame {}, {:.1f} s: {}", e.frame, e.ms / 1000.0, text));
    }
    LogEvents(L"System",
              L"Provider[@Name='nvlddmkm'] or Provider[@Name='amdkmdag'] or Provider[@Name='amdwddmg'] or Provider[@Name='igfx'] or "
              L"Provider[@Name='igfxn'] or Provider[@Name='Display'] or Provider[@Name='Microsoft-Windows-Kernel-LiveDump']",
              t.start, "Graphics driver event", nullptr);
    LogEvents(L"Application", L"Provider[@Name='Application Hang'] or Provider[@Name='Application Error']", t.start, "Windows: the game",
              L"TS3");
    LogBridge(t.start);
}

} // namespace

void Start() noexcept
{
    try
    {
        if (g_f || !ApexPaths::EnsureApexDirectory()) return;
        const std::wstring path = ApexPaths::ApexDirectory() + L"ApexRadiance_Trace.bin";
        {
            std::ifstream f(path, std::ios::binary);
            static TraceFile last; // (8 KB+: off the stack)
            if (f.read(reinterpret_cast<char*>(&last), sizeof last) && !std::memcmp(last.magic, kMagic, sizeof kMagic) && last.state == kRunning &&
                last.pid != GetCurrentProcessId())
                Report(last);
        }
        const HANDLE file = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return;
        const HANDLE map = CreateFileMappingW(file, nullptr, PAGE_READWRITE, 0, sizeof(TraceFile), nullptr);
        if (!map) { CloseHandle(file); return; }
        auto* t = static_cast<TraceFile*>(MapViewOfFile(map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(TraceFile)));
        if (!t) { CloseHandle(map); CloseHandle(file); return; }
        std::memset(t, 0, sizeof *t); // (the handles stay open: the view lives as long as the process)
        std::memcpy(t->magic, kMagic, sizeof kMagic);
        t->pid = GetCurrentProcessId();
        FILETIME now{};
        GetSystemTimeAsFileTime(&now);
        t->start = ((uint64_t)now.dwHighDateTime << 32) | now.dwLowDateTime;
        t->state = kRunning;
        g_startMs = GetTickCount64();
        g_f = t;
        g_state.store(&t->state);
        g_mark.store(Mark);
        Mark("Apex started");
    }
    catch (...)
    {
    }
}

void Mark(const char* step) noexcept
{
    TraceFile* t = g_f;
    if (!t || !step) return;
    const uint32_t ms = (uint32_t)(GetTickCount64() - g_startMs);
    if (g_lastKey.load(std::memory_order_relaxed) == step)
    {
        Entry& e = t->e[g_lastSlot.load(std::memory_order_relaxed) % kSlots];
        if (e.count != UINT32_MAX) ++e.count;
        e.frame = t->frame;
        e.ms = ms;
        return;
    }
    const uint32_t h = (uint32_t)InterlockedIncrement(reinterpret_cast<volatile LONG*>(&t->head)) - 1;
    Entry& e = t->e[h % kSlots];
    e.frame = t->frame;
    e.ms = ms;
    e.count = 1;
    strncpy_s(e.text, step, _TRUNCATE);
    g_lastSlot.store(h, std::memory_order_relaxed);
    g_lastKey.store(step, std::memory_order_relaxed);
}

void Frame() noexcept
{
    if (TraceFile* t = g_f) ++t->frame;
    Mark("Present");
}

} // namespace GpuTrace
