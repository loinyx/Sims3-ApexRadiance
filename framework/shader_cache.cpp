// Background precompile of Apex's HLSL shaders (see shader_cache.h).
//
// Why: D3DCompile takes 10-100+ ms per variant (SMAA's blending-weight pass the longest). Called lazily on the render
// thread (first lamp, first roof, first water, first depth-blur or SMAA frame, first lot pass) it made one-time hitches;
// the 2026-09-28 engine study found d3dcompiler_47.dll in 8.5% of the samples of "Render frame" hitches
// (research\perf2\plan.md, items 7 and C5). D3DCompile needs no device and is called here from one worker thread only.
#include "shader_cache.h"
#include "apex_log.h"
#include "hook_guard.h"
#include "apex_paths.h"
#include <windows.h>
#include <d3dcompiler.h>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <format>
#include <memory>
#include <mutex>
#include <unordered_map>

#pragma comment(lib, "d3dcompiler.lib")

namespace ShaderCache {
namespace {

struct Job {
    Desc d;
    std::vector<DWORD> code; // the bytecode; never changes once state == Done
    std::string error;       // the compiler's message when it failed
    enum State : int { Queued, Compiling, Done } state = Queued;
    bool urgent = false; // a thread waits for it: compiled next
    double ms = 0.0;
    uint64_t key = 0;      // the variant's identity (source, entry, target, flags, macros): its entry in the disk cache
    bool fromDisk = false; // its bytecode came from the disk cache (not compiled this session)
};

struct Registry {
    std::mutex m;
    std::condition_variable cv;
    std::vector<std::unique_ptr<Job>> jobs; // Id = index; entries never move or go away
    HANDLE worker = nullptr;
    HANDLE workerLeft = nullptr; // manual-reset event: the worker has finished its loop (Shutdown waits for it)
    bool running = false;
    bool stop = false;
    bool started = false;
    int waits = 0;
    double waitMs = 0.0, workerMs = 0.0;
    std::string slowest;
    double slowestMs = 0.0;
    std::unordered_map<uint64_t, std::vector<DWORD>> disk; // the disk cache as read at Start, entries taken by their jobs
    int fromDisk = 0, compiled = 0;
};

// Never destroyed (no static destructor at process exit while a worker may still run). Built by the first Add, i.e. by the
// features' namespace-scope initialisers.
Registry& R() {
    static Registry* r = new Registry;
    return *r;
}

double NowMs() {
    LARGE_INTEGER f, c;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c);
    return 1000.0 * static_cast<double>(c.QuadPart) / static_cast<double>(f.QuadPart);
}

// The compiler's message without the trailing NUL / line breaks
std::string Message(ID3DBlob* errors) {
    if (!errors) return "unknown error";
    std::string s(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize());
    while (!s.empty() && (s.back() == '\0' || s.back() == '\n' || s.back() == '\r')) s.pop_back();
    return s.empty() ? "unknown error" : s;
}


// ---- the disk cache (05/10, user: "use less RAM without losing any quality") ----
// Every variant's bytecode is kept in ApexRadiance_ShaderCache.bin once compiled, so a later start creates the same
// shaders without D3DCompile, and the compiler's heap peak never happens. (2.7.0 also delay-loaded d3dcompiler_47.dll so
// a complete cache never mapped it; BitDefender's generic detection flagged that build, 06/10, so 2.7.1 links it as before.) A variant whose identity changed (its source, macros,
// flags or target) misses the cache and is compiled as before; the file is then written again with the current set only.
constexpr uint32_t kDiskMagic = 0x53585041; // "APXS"
constexpr uint32_t kDiskVersion = 1;

uint64_t Fnv(uint64_t h, const void* p, size_t n) {
    const auto* b = static_cast<const unsigned char*>(p);
    for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}
uint64_t FnvStr(uint64_t h, const char* s) { return Fnv(Fnv(h, s, std::strlen(s)), "\0", 1); }

uint64_t KeyOf(const Desc& d) {
    uint64_t h = 1469598103934665603ull;
    h = Fnv(h, d.source.data(), d.source.size());
    h = FnvStr(FnvStr(FnvStr(h, d.entry), d.target), d.sourceName);
    h = Fnv(h, &d.flags, sizeof d.flags);
    for (const auto& [name, value] : d.macros) h = FnvStr(FnvStr(h, name.c_str()), value.c_str());
    return h;
}

// A whole shader token stream: version token first, end token last
bool PlausibleCode(const std::vector<DWORD>& c) {
    return c.size() >= 2 && c.size() < (1u << 20) && (c[0] >> 17) == 0x7FFFu && c.back() == 0x0000FFFFu;
}

std::wstring DiskFile() {
    const std::wstring& dir = ApexPaths::ApexDirectory();
    return dir.empty() ? std::wstring() : dir + L"ApexRadiance_ShaderCache.bin";
}

// Reads the file into r.disk (any damage: the rest is ignored, those variants compile)
void LoadDisk(Registry& r) {
    const std::wstring path = DiskFile();
    if (path.empty()) return;
    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") != 0 || !f) return;
    uint32_t head[3] = {};
    if (std::fread(head, sizeof head, 1, f) == 1 && head[0] == kDiskMagic && head[1] == kDiskVersion && head[2] < 4096) {
        for (uint32_t i = 0; i < head[2]; i++) {
            uint64_t key = 0;
            uint32_t words = 0;
            if (std::fread(&key, sizeof key, 1, f) != 1 || std::fread(&words, sizeof words, 1, f) != 1 || words == 0 || words > (1u << 18)) break;
            std::vector<DWORD> code(words);
            if (std::fread(code.data(), sizeof(DWORD), words, f) != words) break;
            if (PlausibleCode(code)) r.disk[key] = std::move(code);
        }
    }
    std::fclose(f);
}

// Lock held: a queued job found in the disk cache is done
void TakeFromDiskLocked(Registry& r, Job& j) {
    if (j.state != Job::Queued) return;
    const auto it = r.disk.find(j.key);
    if (it == r.disk.end()) return;
    j.code = std::move(it->second);
    r.disk.erase(it);
    std::string().swap(j.d.source);
    j.state = Job::Done;
    j.fromDisk = true;
    r.fromDisk++;
}

// Writes every compiled variant (a temporary file renamed over the old one); worker thread, after its loop
void SaveDisk(Registry& r) {
    const std::wstring path = DiskFile();
    if (path.empty() || !ApexPaths::EnsureApexDirectory()) return;
    // Done jobs and their bytecode are immutable and live for the process lifetime.
    // Snapshot pointers instead of duplicating every shader while writing the cache.
    std::vector<const Job*> all;
    {
        std::lock_guard<std::mutex> lk(r.m);
        for (const auto& j : r.jobs)
            if (j->state == Job::Done && PlausibleCode(j->code)) all.push_back(j.get());
    }
    const std::wstring tmp = path + L".tmp";
    FILE* f = nullptr;
    if (_wfopen_s(&f, tmp.c_str(), L"wb") != 0 || !f) return;
    const uint32_t head[3] = {kDiskMagic, kDiskVersion, static_cast<uint32_t>(all.size())};
    bool ok = std::fwrite(head, sizeof head, 1, f) == 1;
    for (const Job* j : all) {
        const uint64_t key = j->key;
        const auto& code = j->code;
        const uint32_t words = static_cast<uint32_t>(code.size());
        ok = ok && std::fwrite(&key, sizeof key, 1, f) == 1 && std::fwrite(&words, sizeof words, 1, f) == 1 &&
             std::fwrite(code.data(), sizeof(DWORD), words, f) == words;
    }
    ok = std::fclose(f) == 0 && ok;
    if (!ok || !MoveFileExW(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        DeleteFileW(tmp.c_str());
        LOG_WARNING("[ShaderCache] Could not write ApexRadiance_ShaderCache.bin: the shaders compile again at the next start");
        return;
    }
    LOG_INFO(std::format("[ShaderCache] Saved {} compiled Apex shaders to ApexRadiance_ShaderCache.bin (the next start loads them without the compiler)", all.size()));
}

// d3dcompiler_47.dll can be loaded (linked normally since 2.7.1, so it is: without it Apex does not load at all, as in
// 2.6.0; the check stays for a build that delay-loads it again).
bool CompilerAvailable() {
    static const bool ok = [] {
        if (LoadLibraryW(L"d3dcompiler_47.dll")) return true;
        LOG_WARNING("[ShaderCache] d3dcompiler_47.dll not found: Apex shaders missing from ApexRadiance_ShaderCache.bin stay off");
        return false;
    }();
    return ok;
}

// Outside the lock: a Compiling job is touched by the thread compiling it only.
void Compile(Job& j) {
    if (!CompilerAvailable()) {
        j.error = "d3dcompiler_47.dll not found";
        return;
    }
    try {
        std::vector<D3D_SHADER_MACRO> macros;
        for (const auto& [name, value] : j.d.macros) macros.push_back({name.c_str(), value.c_str()});
        macros.push_back({nullptr, nullptr});
        ID3DBlob *code = nullptr, *errors = nullptr;
        const double t0 = NowMs();
        const HRESULT hr = D3DCompile(j.d.source.data(), j.d.source.size(), j.d.sourceName, macros.data(), nullptr, j.d.entry, j.d.target, j.d.flags, 0, &code, &errors);
        j.ms = NowMs() - t0;
        if (FAILED(hr) || !code || code->GetBufferSize() < 4) {
            j.error = Message(errors);
        } else {
            j.code.resize((code->GetBufferSize() + 3) / 4, 0);
            std::memcpy(j.code.data(), code->GetBufferPointer(), code->GetBufferSize());
        }
        if (errors) errors->Release();
        if (code) code->Release();
        std::string().swap(j.d.source); // the bytecode is kept for the session: the source is not needed again
    } catch (...) {
        j.code.clear();
        j.error = "out of memory";
    }
}

// The next job: an urgent one, else the lowest priority number, then the oldest. Lock held.
Job* PickLocked(Registry& r) {
    Job* best = nullptr;
    for (auto& j : r.jobs) {
        if (j->state != Job::Queued) continue;
        if (j->urgent) return j.get();
        if (!best || j->d.priority < best->d.priority) best = j.get();
    }
    return best;
}

// The precompile loop (WorkerProc); `current` = the job it is compiling, for WorkerProc's clean-up after an exception
DWORD WorkerBody(Job*& current) {
    Registry& r = R();
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL); // the game is loading meanwhile
    const double t0 = NowMs();
    int n = 0, failed = 0;
    for (;;) {
        Job* j = nullptr;
        {
            std::lock_guard<std::mutex> lk(r.m);
            if (r.stop) break;
            j = PickLocked(r);
            if (!j) break;
            j->state = Job::Compiling;
            current = j;
        }
        Compile(*j);
        {
            std::lock_guard<std::mutex> lk(r.m);
            j->state = Job::Done;
            r.compiled++;
            current = nullptr;
            if (j->ms > r.slowestMs) {
                r.slowestMs = j->ms;
                r.slowest = j->d.tag;
            }
        }
        r.cv.notify_all();
        n++;
        if (!j->error.empty()) {
            failed++;
            LOG_ERROR(std::format("[ShaderCache] {} did not compile: {}", j->d.tag, j->error));
        } else {
            LOG_DEBUG(std::format("[ShaderCache] {}: {:.1f} ms, {} bytes", j->d.tag, j->ms, j->code.size() * 4));
        }
    }
    const double ms = NowMs() - t0;
    std::string slowest;
    double slowestMs = 0.0;
    bool stopped = false;
    {
        std::lock_guard<std::mutex> lk(r.m);
        r.running = false;
        r.workerMs += ms;
        slowest = r.slowest;
        slowestMs = r.slowestMs;
        stopped = r.stop;
    }
    r.cv.notify_all();
    if (n)
        LOG_INFO(std::format("[ShaderCache] Precompiled {} Apex shader{} in {:.0f} ms on a background thread ({} failed; slowest: {}, {:.0f} ms){}", n, n == 1 ? "" : "s", ms, failed,
                             slowest, slowestMs, stopped ? " - stopped early" : ""));
    if (n && !stopped) SaveDisk(r); // what compiled now starts the next session without the compiler
    SetEvent(r.workerLeft);
    return 0;
}

// 07/10, players' Runtime Error: an exception in the loop (a log line, the disk cache) ends this thread, not the game. The job it
// was compiling goes back to the queue and the worker counts as gone, so a thread waiting in Ready compiles it itself
// instead of waiting forever.
DWORD WINAPI WorkerProc(LPVOID) {
    CrashReport::ThreadStart();
    Job* current = nullptr;
    try {
        return WorkerBody(current);
    } catch (...) {
        HookGuard::Note("ShaderCache precompile thread");
    }
    Registry& r = R();
    try {
        std::lock_guard<std::mutex> lk(r.m);
        if (current && current->state == Job::Compiling) current->state = Job::Queued;
        r.running = false;
    } catch (...) {
    }
    r.cv.notify_all();
    if (r.workerLeft) SetEvent(r.workerLeft);
    return 1;
}

// Lock held
void StartLocked(Registry& r) {
    if (r.running || r.stop) return;
    if (r.worker) {
        CloseHandle(r.worker);
        r.worker = nullptr;
    }
    if (!r.workerLeft) r.workerLeft = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (r.workerLeft) ResetEvent(r.workerLeft);
    r.running = true;
    r.worker = CreateThread(nullptr, 0, WorkerProc, nullptr, 0, nullptr);
    if (!r.worker) {
        r.running = false;
        LOG_ERROR("[ShaderCache] Could not start the precompile thread: each shader compiles when it is first needed");
    }
}

// The job of `id`, compiled. When the worker has not reached it, it becomes the worker's next job and this thread waits
// (logged). Without a worker (thread creation failed, or stopped) it is compiled here, as a last resort.
Job* Ready(Id id) {
    Registry& r = R();
    std::unique_lock<std::mutex> lk(r.m);
    if (id < 0 || id >= static_cast<int>(r.jobs.size())) return nullptr;
    Job* j = r.jobs[static_cast<size_t>(id)].get();
    if (j->state == Job::Done) return j;
    const double t0 = NowMs();
    j->urgent = true;
    if (!r.started) r.started = true;
    StartLocked(r);
    if (r.running && r.worker) SetThreadPriority(r.worker, THREAD_PRIORITY_NORMAL); // someone waits now
    bool here = false;
    while (j->state != Job::Done) {
        if (!r.running && j->state == Job::Queued) { // no worker left to do it
            j->state = Job::Compiling;
            lk.unlock();
            Compile(*j);
            lk.lock();
            j->state = Job::Done;
            here = true;
            break;
        }
        r.cv.wait(lk);
    }
    const double waited = NowMs() - t0;
    r.waits++;
    r.waitMs += waited;
    lk.unlock();
    r.cv.notify_all();
    LOG_WARNING(std::format("[ShaderCache] {}: {} ({:.1f} ms) on thread {} - the background precompile had not reached it", j->d.tag,
                            here ? "compiled on the requesting thread" : "waited for the precompile", waited, GetCurrentThreadId()));
    if (here && !j->error.empty()) LOG_ERROR(std::format("[ShaderCache] {} did not compile: {}", j->d.tag, j->error));
    return j;
}

} // namespace

Id Add(Desc desc) {
    Registry& r = R();
    std::lock_guard<std::mutex> lk(r.m);
    auto job = std::make_unique<Job>();
    job->d = std::move(desc);
    job->key = KeyOf(job->d);
    if (r.started) TakeFromDiskLocked(r, *job);
    r.jobs.push_back(std::move(job));
    if (r.started && !r.running && !r.stop && r.jobs.back()->state == Job::Queued) StartLocked(r); // added after the precompile finished: compile it too
    return static_cast<Id>(r.jobs.size() - 1);
}

void Start() {
    Registry& r = R();
    std::lock_guard<std::mutex> lk(r.m);
    if (r.started) return;
    r.started = true;
    LoadDisk(r);
    size_t left = 0;
    for (auto& j : r.jobs) {
        TakeFromDiskLocked(r, *j);
        left += j->state == Job::Queued;
    }
    LOG_INFO(std::format("[ShaderCache] {} Apex shaders: {} from ApexRadiance_ShaderCache.bin, {} to compile{}", r.jobs.size(), r.fromDisk, left,
                         left ? " on a background thread" : " (the compiler is not loaded)"));
    if (left) StartLocked(r);
}

bool PrecompileComplete() {
    Registry& r = R();
    std::unique_lock<std::mutex> lk(r.m, std::try_to_lock);
    if (!lk.owns_lock() || !r.started) return false;
    for (const auto& j : r.jobs)
        if (j->state != Job::Done) return false;
    return true;
}

namespace {
// The bytecode the device actually holds for a shader equals `code` (another mod's CreatePixelShader hook can hand back a
// different shader: a shader replacer keyed on the bytecode's hash or on the creation order)
bool SameFunction(IDirect3DPixelShader9* ps, const std::vector<DWORD>& code, UINT* heldBytes) {
    UINT size = 0;
    *heldBytes = 0;
    if (FAILED(ps->GetFunction(nullptr, &size)) || size == 0) return true; // cannot tell: trust it
    *heldBytes = size;
    if (size != code.size() * sizeof(DWORD)) return false;
    std::vector<DWORD> held(size / sizeof(DWORD));
    return SUCCEEDED(ps->GetFunction(held.data(), &size)) && std::memcmp(held.data(), code.data(), size) == 0;
}
} // namespace

Result CreatePixelShader(IDirect3DDevice9* dev, Id id, IDirect3DPixelShader9** out, std::string* compileError) {
    *out = nullptr;
    const Job* j = Ready(id);
    if (!j || j->code.empty()) {
        if (compileError) *compileError = j ? j->error : std::string("unknown shader id");
        return Result::CompileFailed;
    }
    if (!dev || FAILED(dev->CreatePixelShader(j->code.data(), out)) || !*out) {
        *out = nullptr;
        return Result::CreateFailed;
    }
    UINT held = 0;
    if (!SameFunction(*out, j->code, &held)) {
        // Replaced on the way: try once more with the same program plus a comment token after the version token (the
        // device ignores comments; a replacer matching the bytecode no longer recognises it)
        LOG_WARNING(std::format("[ShaderCache] {}: the device returned another pixel shader than Apex's ({} bytes instead of {}): another mod replaced it; "
                                "retrying with a marked copy",
                                j->d.tag, held, j->code.size() * sizeof(DWORD)));
        std::vector<DWORD> marked;
        marked.reserve(j->code.size() + 2);
        marked.push_back(j->code[0]);             // ps_3_0 version token
        marked.push_back(0x0000FFFE | (1u << 16)); // comment token, 1 DWORD long
        marked.push_back(0x58455041);              // "APEX"
        marked.insert(marked.end(), j->code.begin() + 1, j->code.end());
        IDirect3DPixelShader9* again = nullptr;
        if (SUCCEEDED(dev->CreatePixelShader(marked.data(), &again)) && again) {
            UINT heldAgain = 0;
            if (SameFunction(again, marked, &heldAgain)) {
                (*out)->Release();
                *out = again;
                LOG_INFO(std::format("[ShaderCache] {}: the marked copy is Apex's own shader", j->d.tag));
            } else {
                again->Release();
                LOG_WARNING(std::format("[ShaderCache] {}: replaced again ({} bytes); the effect may look wrong until that mod is removed", j->d.tag, heldAgain));
            }
        }
    }
    return Result::Ok;
}

std::string StatusText() {
    Registry& r = R();
    std::lock_guard<std::mutex> lk(r.m);
    const size_t total = r.jobs.size();
    size_t done = 0, failed = 0;
    for (const auto& j : r.jobs) {
        if (j->state != Job::Done) continue;
        done++;
        if (j->code.empty()) failed++;
    }
    std::string s;
    if (!r.started) s = std::format("{} Apex shaders registered, precompile not started", total);
    else if (done < total) s = std::format("precompiling Apex shaders: {} of {} done", done, total);
    else s = std::format("{} Apex shaders ready: {} from the disk cache, {} compiled in {:.0f} ms on a background thread (slowest {} ms: {})", total, r.fromDisk,
                         r.compiled, r.workerMs, static_cast<int>(r.slowestMs), r.slowest);
    if (failed) s += std::format(", {} failed (see ApexRadiance_LOG.txt)", failed);
    s += std::format("; render-thread waits: {}", r.waits);
    if (r.waits) s += std::format(" ({:.1f} ms)", r.waitMs);
    return s;
}

void Shutdown() {
    Registry& r = R();
    HANDLE left = nullptr;
    bool running = false;
    {
        std::lock_guard<std::mutex> lk(r.m);
        r.stop = true;
        running = r.running;
        left = r.workerLeft;
    }
    // Under the loader lock (FreeLibrary): wait until the worker is out of its loop (it stops between two compiles), not
    // for the thread to end, which needs the loader lock.
    if (running && left) WaitForSingleObject(left, 3000);
}

} // namespace ShaderCache

namespace ShaderCache {
void RenderThreadWaits(int* waits, double* ms) {
    Registry& r = R();
    std::lock_guard<std::mutex> lk(r.m);
    if (waits) *waits = r.waits;
    if (ms) *ms = r.waitMs;
}
} // namespace ShaderCache
