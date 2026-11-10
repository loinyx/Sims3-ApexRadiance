// Crash report (see crash_report.h).
#include "crash_report.h"
#include "apex_paths.h"
#include "apex_version.h"
#include "build_flavor.h"
#include <windows.h>
#include <dbghelp.h>
#include <intrin.h>
#include <atomic>
#include <csignal>
#include <cstdlib>
#include <exception>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace CrashReport {
namespace {

using MiniDumpWriteDumpFn = BOOL(WINAPI*)(HANDLE, DWORD, HANDLE, MINIDUMP_TYPE, PMINIDUMP_EXCEPTION_INFORMATION, PMINIDUMP_USER_STREAM_INFORMATION,
                                          PMINIDUMP_CALLBACK_INFORMATION);

LPTOP_LEVEL_EXCEPTION_FILTER g_previous = nullptr;
PVOID g_veh = nullptr; // the RecordThrow registration, removed on FreeLibrary
MiniDumpWriteDumpFn g_writeDump = nullptr;
wchar_t g_textPath[MAX_PATH] = {};
wchar_t g_dumpPath[MAX_PATH] = {};
std::atomic<bool> g_installed{false};
std::atomic<LONG> g_written{0};
thread_local int t_depth = 0;
constexpr DWORD kAbortCode = 0xE0415058; // 'APX' + E0: an abort reported by Apex's runtime (ReportAbort), not a hardware exception

// Apex's own image (07/10, players' Runtime Error): tells whether a C++ exception was thrown by Apex's code
uintptr_t g_selfBase = 0;
uintptr_t g_selfEnd = 0;
constexpr DWORD kCppExceptionCode = 0xE06D7363; // 'msc' + E0: an MSVC throw

// The last C++ exception thrown on this thread, recorded first-chance by a vectored handler (no heap, plain stores). A
// terminate()/abort() from an exception that left a noexcept function (a std::thread body, ...) never reaches the filter,
// and by then the exception record is gone: this is what the report prints for those.
thread_local ULONG_PTR t_lastThrowInfo = 0;
thread_local char t_lastWhat[160] = {};

// The feature line: two fixed buffers, the reader takes the last complete one
char g_features[2][1024] = {};
std::atomic<int> g_featuresCurrent{0};
std::mutex g_featuresLock;

struct Out {
    HANDLE file;
    void Write(const char* s) {
        DWORD n = 0;
        WriteFile(file, s, static_cast<DWORD>(std::strlen(s)), &n, nullptr);
    }
};

// "ADDRESS Module+0xOFFSET" for an address inside a loaded image, "ADDRESS (private memory)" otherwise; false when it
// is not committed memory. The base comes from VirtualQuery (no loader lock), the name from GetModuleFileNameW.
bool Describe(uintptr_t addr, char* out, size_t size, bool* executable = nullptr) {
    MEMORY_BASIC_INFORMATION mbi = {};
    if (!VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return false;
    if (executable)
        *executable = (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
    if (mbi.Type != MEM_IMAGE || !mbi.AllocationBase) {
        std::snprintf(out, size, "%08X (%s memory)", static_cast<unsigned>(addr), mbi.Type == MEM_PRIVATE ? "private" : "mapped");
        return true;
    }
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(static_cast<HMODULE>(mbi.AllocationBase), path, MAX_PATH);
    const wchar_t* base = path;
    for (const wchar_t* p = path; *p; ++p)
        if (*p == L'\\' || *p == L'/') base = p + 1;
    char name[MAX_PATH] = {};
    WideCharToMultiByte(CP_UTF8, 0, base, -1, name, sizeof name, nullptr, nullptr);
    std::snprintf(out, size, "%08X %s+0x%X", static_cast<unsigned>(addr), name[0] ? name : "?",
                  static_cast<unsigned>(addr - reinterpret_cast<uintptr_t>(mbi.AllocationBase)));
    return true;
}

bool Readable(uintptr_t addr, size_t n) {
    MEMORY_BASIC_INFORMATION mbi = {};
    if (!VirtualQuery(reinterpret_cast<LPCVOID>(addr), &mbi, sizeof mbi) || mbi.State != MEM_COMMIT) return false;
    if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) return false;
    return addr + n <= reinterpret_cast<uintptr_t>(mbi.BaseAddress) + mbi.RegionSize;
}

bool InSelf(uintptr_t addr) { return g_selfBase && addr >= g_selfBase && addr < g_selfEnd; }

// MSVC x86 throw metadata (the pointers are absolute on x86): ThrowInfo -> CatchableTypeArray -> CatchableType ->
// TypeDescriptor, whose name is the decorated type (".?AVbad_alloc@std@@").
struct TypeDescriptorX86 {
    const void* vftable;
    void* spare;
    char name[1];
};
struct CatchableTypeX86 {
    unsigned properties;
    const TypeDescriptorX86* type;
    int mdisp, pdisp, vdisp; // where this base lies in the thrown object (pdisp -1: not through a virtual base)
};
struct CatchableTypeArrayX86 {
    int count;
    const CatchableTypeX86* types[1];
};
struct ThrowInfoX86 {
    unsigned attributes;
    const void* unwind;
    const void* forwardCompat;
    const CatchableTypeArrayX86* catchables;
};

// The thrown type's decorated name (false when the metadata cannot be read) and the offset of its std::exception base in
// the thrown object (-1: none, or behind a virtual base). Under SEH: the ThrowInfo pointer comes from a broken process.
bool ReadThrowType(ULONG_PTR throwInfo, char* out, size_t size, int* stdOffset) {
    out[0] = 0;
    if (stdOffset) *stdOffset = -1;
    __try {
        const auto* ti = reinterpret_cast<const ThrowInfoX86*>(throwInfo);
        if (!ti || !ti->catchables || ti->catchables->count <= 0) return false;
        const int n = ti->catchables->count < 32 ? ti->catchables->count : 32;
        for (int i = 0; i < n; i++) {
            const CatchableTypeX86* ct = ti->catchables->types[i];
            if (!ct || !ct->type) continue;
            if (i == 0) strncpy_s(out, size, ct->type->name, _TRUNCATE);
            if (stdOffset && ct->pdisp == -1 && std::strcmp(ct->type->name, ".?AVexception@std@@") == 0) *stdOffset = ct->mdisp;
        }
        return out[0] != 0;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out[0] = 0;
        return false;
    }
}

// what() of a std::exception thrown by Apex's own runtime (same class layout as this module), copied under SEH
void ReadWhat(ULONG_PTR object, char* out, size_t size) {
    out[0] = 0;
    __try {
        const char* w = reinterpret_cast<const std::exception*>(object)->what();
        if (w) strncpy_s(out, size, w, _TRUNCATE);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        out[0] = 0;
    }
}

// what() only when the exception is a std::exception thrown by Apex's code (another module's class layout is unknown)
void ReadApexWhat(ULONG_PTR throwInfo, ULONG_PTR object, char* out, size_t size) {
    out[0] = 0;
    char raw[256];
    int offset = -1;
    if (InSelf(throwInfo) && ReadThrowType(throwInfo, raw, sizeof raw, &offset) && offset >= 0) ReadWhat(object + static_cast<ULONG_PTR>(offset), out, size);
}

// "std::bad_alloc" from ".?AVbad_alloc@std@@" for plain names; anything fancier (templates) stays decorated
void PrettyType(const char* decorated, char* out, size_t size) {
    strncpy_s(out, size, decorated, _TRUNCATE);
    if (std::strncmp(decorated, ".?AV", 4) != 0 && std::strncmp(decorated, ".?AU", 4) != 0) return;
    const char* body = decorated + 4;
    const char* end = std::strstr(body, "@@");
    if (!end || end - body >= 200) return;
    for (const char* p = body; p < end; ++p)
        if (*p == '?' || *p == '$') return;
    const char* parts[16];
    size_t lens[16];
    int count = 0;
    for (const char* p = body; p < end;) {
        if (count == 16) return;
        const char* at = p;
        while (at < end && *at != '@') at++;
        parts[count] = p;
        lens[count++] = static_cast<size_t>(at - p);
        p = at + 1;
    }
    char buf[256] = {};
    size_t len = 0;
    for (int i = count - 1; i >= 0; i--) {
        if (len + lens[i] + 3 >= sizeof buf) return;
        std::memcpy(buf + len, parts[i], lens[i]);
        len += lens[i];
        if (i) {
            std::memcpy(buf + len, "::", 2);
            len += 2;
        }
    }
    buf[len] = 0;
    strncpy_s(out, size, buf, _TRUNCATE);
}

// "<label>: std::bad_alloc ("bad allocation"), type info at ApexRadiance.asi+0x..., thrown by Apex Radiance's code"
void WriteCppException(Out& o, ULONG_PTR throwInfo, const char* what, const char* label) {
    char line[900], raw[256], pretty[256], where[400];
    if (!ReadThrowType(throwInfo, raw, sizeof raw, nullptr)) std::snprintf(raw, sizeof raw, "(type not readable)");
    PrettyType(raw, pretty, sizeof pretty);
    if (!Describe(throwInfo, where, sizeof where)) std::snprintf(where, sizeof where, "%08X (not mapped)", static_cast<unsigned>(throwInfo));
    const bool hasWhat = what && what[0];
    std::snprintf(line, sizeof line, "%s: %s%s%s%s, type info at %s (%s)\r\n", label, pretty, hasWhat ? " (\"" : "", hasWhat ? what : "", hasWhat ? "\")" : "", where,
                  InSelf(throwInfo) ? "thrown by Apex Radiance's code" : "not Apex Radiance's code");
    o.Write(line);
}

void WriteText(EXCEPTION_POINTERS* ep) {
    HANDLE f = CreateFileW(g_textPath, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    Out o{f};
    char line[1400], a[400], b[400];
    SYSTEMTIME st;
    GetLocalTime(&st);
    std::snprintf(line, sizeof line, "%s %s (%s) crash report, %04u-%02u-%02u %02u:%02u:%02u.%03u, thread %lu\r\n", APEX_PRODUCT_NAME, APEX_VERSION_STRING,
                  kPublicBuild ? "normal mode" : "developer mode", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                  GetCurrentThreadId());
    o.Write(line);
    const EXCEPTION_RECORD* er = ep->ExceptionRecord;
    const CONTEXT* c = ep->ContextRecord;
    const uintptr_t at = reinterpret_cast<uintptr_t>(er->ExceptionAddress);
    if (!Describe(at, a, sizeof a)) std::snprintf(a, sizeof a, "%08X (not mapped)", static_cast<unsigned>(at));
    std::snprintf(line, sizeof line, "Exception %08lX at %s", er->ExceptionCode, a);
    o.Write(line);
    if (er->ExceptionCode == kAbortCode) {
        static const char* const kReasons[] = {"?", "C++ exception not caught (std::terminate)", "abort()", "pure virtual call", "invalid C runtime parameter"};
        const ULONG_PTR r = er->NumberParameters ? er->ExceptionInformation[0] : 0;
        std::snprintf(line, sizeof line, " (Apex's runtime ended the game: %s; %s)", kReasons[r < 5 ? r : 0],
                      r == 4 ? "Windows Error Reporting follows" : "the \"Runtime Error!\" dialog follows");
        o.Write(line);
    }
    if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2)
        std::snprintf(line, sizeof line, ", %s %08X\r\n", er->ExceptionInformation[0] == 8 ? "executing" : (er->ExceptionInformation[0] ? "writing" : "reading"),
                      static_cast<unsigned>(er->ExceptionInformation[1]));
    else std::snprintf(line, sizeof line, "\r\n");
    o.Write(line);
    // 07/10, players' Runtime Error: name the C++ exception (the thrown type, and whether Apex's code threw it)
    if (er->ExceptionCode == kCppExceptionCode && er->NumberParameters >= 3) {
        char what[160];
        ReadApexWhat(er->ExceptionInformation[2], er->ExceptionInformation[1], what, sizeof what);
        WriteCppException(o, er->ExceptionInformation[2], what, "C++ exception");
    } else if (er->ExceptionCode == kAbortCode && t_lastThrowInfo) {
        WriteCppException(o, t_lastThrowInfo, t_lastWhat, "Last C++ exception thrown on this thread");
    }
    std::snprintf(line, sizeof line, "EAX %08lX EBX %08lX ECX %08lX EDX %08lX ESI %08lX EDI %08lX EBP %08lX ESP %08lX EIP %08lX\r\n", c->Eax, c->Ebx, c->Ecx, c->Edx,
                  c->Esi, c->Edi, c->Ebp, c->Esp, c->Eip);
    o.Write(line);
    // registers that point into code (a call through a register, a this pointer's vtable ...)
    const DWORD regs[] = {c->Eax, c->Ebx, c->Ecx, c->Edx, c->Esi, c->Edi, c->Ebp};
    const char* names[] = {"EAX", "EBX", "ECX", "EDX", "ESI", "EDI", "EBP"};
    for (int i = 0; i < 7; i++) {
        bool exec = false;
        if (Describe(regs[i], b, sizeof b, &exec) && exec) {
            std::snprintf(line, sizeof line, "  %s -> %s\r\n", names[i], b);
            o.Write(line);
        }
    }
    const int cur = g_featuresCurrent.load(std::memory_order_acquire);
    std::snprintf(line, sizeof line, "Apex features on: %s\r\n", g_features[cur][0] ? g_features[cur] : "(not recorded yet)");
    o.Write(line);
    // The stack: every value that points into a module's code (return addresses, mostly), with its position
    o.Write("Stack values pointing into code (ESP+offset: address module+offset):\r\n");
    const uintptr_t sp = c->Esp;
    int shown = 0;
    for (uintptr_t off = 0; off < 0x4000 && shown < 96; off += 4) {
        const uintptr_t p = sp + off;
        if ((off == 0 || (p & 0xFFF) == 0) && !Readable(p, 4)) break;
        const uintptr_t v = *reinterpret_cast<const uintptr_t*>(p);
        bool exec = false;
        if (v < 0x10000 || !Describe(v, b, sizeof b, &exec) || !exec) continue;
        std::snprintf(line, sizeof line, "  +%04X: %s\r\n", static_cast<unsigned>(off), b);
        o.Write(line);
        shown++;
    }
    // Raw words at ESP (the first 32), for the case where the return address itself is the garbage
    o.Write("Raw stack at ESP:\r\n");
    for (uintptr_t off = 0; off < 0x80; off += 0x20) {
        if (!Readable(sp + off, 0x20)) break;
        const DWORD* w = reinterpret_cast<const DWORD*>(sp + off);
        std::snprintf(line, sizeof line, "  +%02X: %08lX %08lX %08lX %08lX %08lX %08lX %08lX %08lX\r\n", static_cast<unsigned>(off), w[0], w[1], w[2], w[3], w[4], w[5], w[6],
                      w[7]);
        o.Write(line);
    }
    o.Write(g_writeDump ? "Minidump: ApexRadiance_Crash.dmp\r\n" : "Minidump: not written (dbghelp.dll not available)\r\n");
    FlushFileBuffers(f);
    CloseHandle(f);
}

void WriteDump(EXCEPTION_POINTERS* ep) {
    if (!g_writeDump) return;
    HANDLE f = CreateFileW(g_dumpPath, GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return;
    MINIDUMP_EXCEPTION_INFORMATION mei = {};
    mei.ThreadId = GetCurrentThreadId();
    mei.ExceptionPointers = ep;
    mei.ClientPointers = FALSE;
    const MINIDUMP_TYPE type = static_cast<MINIDUMP_TYPE>(MiniDumpWithIndirectlyReferencedMemory | MiniDumpScanMemory | MiniDumpWithThreadInfo |
                                                          MiniDumpWithFullMemoryInfo); // the region list (a few KB): who held the address space
    g_writeDump(GetCurrentProcess(), GetCurrentProcessId(), f, type, &mei, nullptr, nullptr);
    CloseHandle(f);
}

void WriteReport(EXCEPTION_POINTERS* ep) {
    __try {
        WriteText(ep);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    __try {
        WriteDump(ep);
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
}

// ---- Apex's own runtime aborts (07/10, players: "Microsoft Visual C++ Runtime Library: Runtime Error! ... terminate it in an
// unusual way" with an empty "Program:"). Apex links its C runtime statically, so an exception that escapes Apex's code
// (std::terminate), an abort(), a pure virtual call or a bad CRT parameter inside Apex ends in that dialog, never in the
// exception filter above. These handlers belong to Apex's runtime only (the game's own msvcr aborts never reach them), so a
// report written here means the abort came from Apex; then the runtime goes on as before (its dialog, then the end).
void ReportAbort(DWORD reason) {
    if (g_written.exchange(1) != 0) return;
    CONTEXT ctx = {};
    RtlCaptureContext(&ctx);
    EXCEPTION_RECORD er = {};
    er.ExceptionCode = kAbortCode;
    er.ExceptionAddress = reinterpret_cast<PVOID>(static_cast<uintptr_t>(ctx.Eip));
    er.NumberParameters = 1;
    er.ExceptionInformation[0] = reason; // 1 terminate, 2 abort (SIGABRT), 3 pure virtual call, 4 invalid CRT parameter
    EXCEPTION_POINTERS ep = {&er, &ctx};
    WriteReport(&ep);
}
void __cdecl OnTerminate() {
    ReportAbort(1);
    std::abort();
}
void __cdecl OnAbortSignal(int) { ReportAbort(2); }
void __cdecl OnPureCall() {
    ReportAbort(3);
    std::abort();
}
void __cdecl OnInvalidParameter(const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t) {
    ReportAbort(4);
    // End the way the runtime's default handler does (_invoke_watson: a fast fail into Windows Error Reporting), not
    // abort(), which would add a "Runtime Error!" dialog this case never showed before (07/10)
    __fastfail(FAST_FAIL_INVALID_ARG);
}

// First chance, every exception in the process: only C++ throws are recorded, with plain stores, then the search goes on
// untouched. what() is copied only for std::exception types thrown by Apex's own runtime (same class layout).
LONG CALLBACK RecordThrow(EXCEPTION_POINTERS* ep) {
    const EXCEPTION_RECORD* er = ep ? ep->ExceptionRecord : nullptr;
    if (!er || er->ExceptionCode != kCppExceptionCode || er->NumberParameters < 3) return EXCEPTION_CONTINUE_SEARCH;
    t_lastThrowInfo = er->ExceptionInformation[2];
    ReadApexWhat(er->ExceptionInformation[2], er->ExceptionInformation[1], t_lastWhat, sizeof t_lastWhat);
    return EXCEPTION_CONTINUE_SEARCH;
}

void FindSelf() {
    HMODULE self = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&FindSelf), &self) ||
        !self)
        return;
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(self);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(reinterpret_cast<const BYTE*>(self) + dos->e_lfanew);
    g_selfBase = reinterpret_cast<uintptr_t>(self);
    g_selfEnd = g_selfBase + nt->OptionalHeader.SizeOfImage;
}

LONG WINAPI Filter(EXCEPTION_POINTERS* ep) {
    // A filter that took over and chains back to this one would loop: a second entry on the same thread passes
    if (t_depth > 0) return EXCEPTION_CONTINUE_SEARCH;
    struct Depth {
        Depth() { ++t_depth; }
        ~Depth() { --t_depth; }
    } depth;
    if (g_written.exchange(1) == 0 && ep && ep->ExceptionRecord && ep->ContextRecord) WriteReport(ep);
    return g_previous ? g_previous(ep) : EXCEPTION_CONTINUE_SEARCH;
}

} // namespace

void Install() {
    if (g_installed.load()) return;
    const std::wstring dir = ApexPaths::ApexDirectory();
    if (dir.empty() || dir.size() + 32 >= MAX_PATH) return;
    wcscpy_s(g_textPath, (dir + L"ApexRadiance_Crash.txt").c_str());
    wcscpy_s(g_dumpPath, (dir + L"ApexRadiance_Crash.dmp").c_str());
    if (HMODULE dbghelp = LoadLibraryW(L"dbghelp.dll")) g_writeDump = reinterpret_cast<MiniDumpWriteDumpFn>(GetProcAddress(dbghelp, "MiniDumpWriteDump"));
    FindSelf();
    g_veh = AddVectoredExceptionHandler(0, RecordThrow); // last in the chain, and it only records
    g_previous = SetUnhandledExceptionFilter(Filter);
    std::set_terminate(OnTerminate);
    std::signal(SIGABRT, OnAbortSignal);
    _set_purecall_handler(OnPureCall);
    _set_invalid_parameter_handler(OnInvalidParameter);
    g_installed.store(true);
}

void ThreadStart() {
    // std::set_terminate is per thread in MSVC's runtime: each Apex thread sets it, so a terminate there is reported as one
    // (07/10, players' Runtime Error) instead of as a plain abort(). Before Install it is set by Install's own thread only.
    std::set_terminate(OnTerminate);
}

void Refresh() {
    if (!g_installed.load()) return;
    const LPTOP_LEVEL_EXCEPTION_FILTER cur = SetUnhandledExceptionFilter(Filter);
    if (cur != Filter) g_previous = cur; // someone set theirs since: it runs after the report
}

void Shutdown() {
    if (g_veh) {
        RemoveVectoredExceptionHandler(g_veh);
        g_veh = nullptr;
    }
    if (g_installed.exchange(false)) { // hand the unhandled filter back if it is still ours
        const LPTOP_LEVEL_EXCEPTION_FILTER cur = SetUnhandledExceptionFilter(g_previous);
        if (cur != Filter) SetUnhandledExceptionFilter(cur); // someone replaced ours since: leave theirs in place
    }
}

void SetFeatureLine(const std::string& line) {
    std::lock_guard<std::mutex> lock(g_featuresLock);
    const int next = 1 - g_featuresCurrent.load(std::memory_order_relaxed);
    strncpy_s(g_features[next], line.c_str(), _TRUNCATE);
    g_featuresCurrent.store(next, std::memory_order_release);
}

} // namespace CrashReport
