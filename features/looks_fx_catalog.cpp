// Looks: the player's Shaders folder (see looks_fx.h). A background scan compiles every .fx there for the picture size
// (each file on its own thread with a large stack, a time limit and SEH around it: a file that crashes or hangs the
// compiler is refused, never the game); the render thread adopts each published result between frames and turns the
// techniques into LooksShader entries the run list, the presets and the menu use like the built-in ports.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "looks_fx.h"
#include "looks_runner.h"
#include "apex_log.h"
#include "apex_paths.h"
#include "hook_guard.h"

#include <algorithm>
#include <atomic>
#include <deque>
#include <map>
#include <mutex>

namespace Looks::Fx {

namespace {

constexpr size_t kMaxFiles = 500;              // .fx files read from the folder
constexpr DWORD kCompileTimeoutMs = 120000;    // one file
constexpr uint64_t kMaxTotalGpuBytes = 384ull << 20; // every user effect running at once (a 32-bit game: room left for its own)
constexpr int kMaxFramePasses = 256;           // user passes drawn in one frame

std::string LowerA(std::string s)
{
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

std::string Utf8(const std::wstring& w)
{
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

// One user technique, as a LooksShader the rest of Looks uses (its strings live in `text`, never moved)
struct UserTech
{
    std::string technique, file, reason;
    std::shared_ptr<const Effect> fx;
    size_t tech = 0;
    bool hidden = false;
    std::deque<std::string> text;
    std::vector<LooksParam> params;
    std::vector<int> uniform; // per param: index in fx->uniforms
    LooksShader desc{};
    const char* Keep(std::string s) { text.push_back(std::move(s)); return text.back().c_str(); }
};

struct Catalog
{
    std::vector<std::unique_ptr<UserTech>> techs;
    std::vector<std::pair<std::string, std::string>> fileErrors; // file, reason
    std::vector<std::string> files;                              // lower-case .fx names in the folder
    std::vector<std::string> pending;                            // ... of those not compiled yet
    int generation = 0;
};

// The parameters of an effect: its uniforms without a source annotation, as the menu shows ReShade's
void Describe(UserTech& u)
{
    const Effect& fx = *u.fx;
    for (size_t i = 0; i < fx.uniforms.size(); ++i)
    {
        const Uniform& un = fx.uniforms[i];
        LooksParam p{};
        p.key = u.Keep(un.name);
        const Ann* label = FindAnn(un.ann, "ui_label");
        p.label = u.Keep(label && label->isString && !label->s.empty() ? label->s : un.name);
        p.comps = (int)std::min(4u, un.Comps());
        p.reg = (int)i;
        for (int c = 0; c < p.comps && c < (int)un.init.size(); ++c) p.def[c] = un.init[c];
        const Ann* type = FindAnn(un.ann, "ui_type");
        const std::string t = type && type->isString ? type->s : "";
        const Ann* src = FindAnn(un.ann, "source");
        const Ann* hid = FindAnn(un.ann, "hidden");
        if (src || (hid && !hid->isString && hid->f[0] != 0.0f) || un.array || un.cols > 1)
            p.kind = LooksParamKind::Source; // set by the runtime, hidden, or not a slider (matrix, array)
        else if (un.base == kBool) p.kind = LooksParamKind::Bool;
        else if (t == "combo" || t == "radio" || t == "list") p.kind = LooksParamKind::Combo;
        else if (t == "color") p.kind = LooksParamKind::Color;
        else p.kind = un.base == kFloat ? LooksParamKind::Float : LooksParamKind::Int;
        const Ann* mn = FindAnn(un.ann, "ui_min");
        const Ann* mx = FindAnn(un.ann, "ui_max");
        p.min = mn && !mn->isString ? mn->f[0] : 0.0f;
        p.max = mx && !mx->isString ? mx->f[0] : (p.kind == LooksParamKind::Color ? 1.0f : 0.0f);
        if (p.kind == LooksParamKind::Combo)
        {
            const Ann* items = FindAnn(un.ann, "ui_items");
            if (items && items->isString)
            {
                p.items = u.Keep(items->s + '\0');
                int n = 0;
                for (char c : items->s) n += c == '\0';
                if (!mx) { p.min = 0; p.max = (float)std::max(0, n - 1); }
            }
        }
        if (p.kind == LooksParamKind::Source) p.items = "";
        u.params.push_back(p);
        u.uniform.push_back((int)i);
    }
    u.desc.technique = u.technique.c_str();
    u.desc.file = u.file.c_str();
    u.desc.params = u.params.data();
    u.desc.paramCount = (int)u.params.size();
    u.desc.needsDepth = fx.techniques[u.tech].needsDepth;
    u.desc.description = u.file.c_str(); // the menu shows the .fx file name
    u.desc.kind = (LooksShaderKind)0;
}

// ---------------------------------------------------------------------------------------------- scan (worker)
struct CacheEntry { FILETIME written{}; uint64_t size = 0; UINT w = 0, h = 0; std::shared_ptr<const Effect> fx; };

std::mutex g_lock;                         // guards everything below up to g_current
std::shared_ptr<Catalog> g_published;
std::map<std::wstring, CacheEntry> g_cache; // worker only (but cleared under the lock)
bool g_running = false, g_again = false, g_force = false;
UINT g_scanW = 0, g_scanH = 0;              // size requested for the next scan
int g_nextGeneration = 1;
std::atomic<bool> g_requested{ false };
std::atomic<bool> g_scanning{ false };
std::string g_folderNote;

// render thread
std::shared_ptr<Catalog> g_current;
UINT g_wantW = 0, g_wantH = 0; // the size a run asked for (0 = the back buffer's)
int g_framePasses = 0;

struct Job
{
    std::wstring path;
    Folders folders;
    UINT w = 0, h = 0;
    Effect fx;
};

void CompileJob(Job* j) { Compile(j->path, j->folders, j->w, j->h, j->fx); }

bool SehCompile(Job* j)
{
    __try
    {
        CompileJob(j);
        return true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
}

DWORD WINAPI JobThread(void* p)
{
    auto* holder = static_cast<std::shared_ptr<Job>*>(p);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
    if (!SehCompile(holder->get()))
    {
        (*holder)->fx.error = "the compiler stopped on this file";
        (*holder)->fx.techniques.clear();
        return 0; // the holder is leaked on purpose: the heap may not be trusted after a crash
    }
    delete holder;
    return 0;
}

// Compiles one file on its own thread (16 MB stack). A file that takes too long is refused and its thread left behind.
std::shared_ptr<const Effect> CompileGuarded(const std::wstring& path, const Folders& folders, UINT w, UINT h)
{
    auto job = std::make_shared<Job>();
    job->path = path; job->folders = folders; job->w = w; job->h = h;
    auto* holder = new std::shared_ptr<Job>(job);
    HANDLE t = CreateThread(nullptr, 16u << 20, JobThread, holder, STACK_SIZE_PARAM_IS_A_RESERVATION, nullptr);
    auto out = std::make_shared<Effect>();
    out->path = path;
    out->file = Utf8(path.substr(path.find_last_of(L"\\/") + 1));
    out->width = w; out->height = h;
    if (!t) { delete holder; out->error = "the compiler could not start"; return out; }
    const DWORD r = WaitForSingleObject(t, kCompileTimeoutMs);
    CloseHandle(t);
    if (r != WAIT_OBJECT_0) { out->error = "took more than two minutes to compile"; return out; }
    *out = std::move(job->fx);
    return out;
}

void ListFx(const std::wstring& dir, int depth, std::vector<std::wstring>& out)
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do
    {
        const std::wstring n = fd.cFileName;
        if (n == L"." || n == L".." || (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { if (depth > 0) ListFx(dir + n + L"\\", depth - 1, out); }
        else if (n.size() > 3 && _wcsicmp(n.c_str() + n.size() - 3, L".fx") == 0 && out.size() < kMaxFiles) out.push_back(dir + n);
    } while (out.size() < kMaxFiles && FindNextFileW(h, &fd));
    FindClose(h);
}

std::shared_ptr<Catalog> Build(const std::vector<std::shared_ptr<const Effect>>& effects, const std::vector<std::wstring>& files)
{
    auto c = std::make_shared<Catalog>();
    std::map<std::string, bool> seen; // "tech@file" lower: the first file of a name wins, as in ReShade's search
    for (size_t i = 0; i < files.size(); ++i)
    {
        c->files.push_back(LowerA(Utf8(files[i].substr(files[i].find_last_of(L'\\') + 1))));
        if (i < effects.size() && !effects[i]) c->pending.push_back(c->files.back());
    }
    for (const auto& fx : effects)
    {
        if (!fx) continue;
        if (!fx->error.empty()) { c->fileErrors.push_back({ fx->file, fx->error }); continue; }
        for (size_t t = 0; t < fx->techniques.size(); ++t)
        {
            const Technique& tq = fx->techniques[t];
            if (!seen.emplace(LowerA(tq.name + "@" + fx->file), true).second) continue;
            auto u = std::make_unique<UserTech>();
            u->technique = tq.name; u->file = fx->file; u->reason = tq.reason; u->fx = fx; u->tech = t; u->hidden = tq.hidden;
            Describe(*u);
            c->techs.push_back(std::move(u));
        }
    }
    return c;
}

void Publish(const std::vector<std::shared_ptr<const Effect>>& effects, const std::vector<std::wstring>& files)
{
    auto c = Build(effects, files);
    std::lock_guard<std::mutex> lock(g_lock);
    c->generation = g_nextGeneration++;
    g_published = std::move(c);
}

void ScanOnce(UINT W, UINT H, bool force)
{
    const std::wstring root = ShadersFolder();
    if (root.empty()) return;
    Folders folders;
    folders.root = root;
    folders.includeDirs = { root + L"Shaders\\", root };
    folders.textureDirs = { root + L"Textures\\", root };
    std::vector<std::wstring> files;
    ListFx(root, 0, files);
    ListFx(root + L"Shaders\\", 3, files);
    std::sort(files.begin(), files.end(), [](const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
    if (force) g_cache.clear();

    std::vector<std::shared_ptr<const Effect>> effects(files.size());
    std::vector<size_t> todo;
    for (size_t i = 0; i < files.size(); ++i)
    {
        WIN32_FILE_ATTRIBUTE_DATA a{};
        if (!GetFileAttributesExW(files[i].c_str(), GetFileExInfoStandard, &a)) continue;
        const uint64_t size = ((uint64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow;
        auto it = g_cache.find(files[i]);
        if (it != g_cache.end() && it->second.size == size && CompareFileTime(&it->second.written, &a.ftLastWriteTime) == 0 &&
            it->second.w == W && it->second.h == H)
            effects[i] = it->second.fx;
        else todo.push_back(i);
    }
    Publish(effects, files); // what is known already (cached files) shows at once
    const DWORD t0 = GetTickCount();
    for (size_t i : todo)
    {
        WIN32_FILE_ATTRIBUTE_DATA a{};
        GetFileAttributesExW(files[i].c_str(), GetFileExInfoStandard, &a);
        effects[i] = CompileGuarded(files[i], folders, W, H);
        CacheEntry& e = g_cache[files[i]];
        e.written = a.ftLastWriteTime; e.size = ((uint64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow; e.w = W; e.h = H; e.fx = effects[i];
        Publish(effects, files); // each compiled file shows as soon as it is ready
        const Effect& fx = *effects[i];
        if (!fx.error.empty()) LOG_INFO("[Looks] " + fx.file + " can't run here: " + fx.error);
        for (const Technique& t : fx.techniques)
            if (!t.reason.empty()) LOG_INFO("[Looks] " + fx.file + ", " + t.name + " can't run here: " + t.reason);
        for (const std::string& n : fx.notes) LOG_INFO("[Looks] " + fx.file + ": Direct3D 9 fallback, " + n);
    }
    for (auto it = g_cache.begin(); it != g_cache.end();)
        it = std::find(files.begin(), files.end(), it->first) == files.end() ? g_cache.erase(it) : std::next(it);
    if (!todo.empty())
        LOG_INFO("[Looks] Shaders folder: compiled " + std::to_string(todo.size()) + " of " + std::to_string(files.size()) + " .fx files for " +
                 std::to_string(W) + "x" + std::to_string(H) + " in " + std::to_string(GetTickCount() - t0) + " ms");
}

void StartWorker()
{
    // under g_lock
    g_running = true;
    g_scanning = true;
    if (!HookGuard::StartDetached("Looks: compile the Shaders folder", [] {
        for (;;)
        {
            UINT w, h; bool force;
            {
                std::lock_guard<std::mutex> lock(g_lock);
                w = g_scanW; h = g_scanH; force = g_force;
                g_again = false; g_force = false;
            }
            try
            {
                ScanOnce(w, h, force);
            }
            catch (...) // out of memory while listing or publishing: the next request scans again
            {
                HookGuard::Note("Looks: compile the Shaders folder");
            }
            std::lock_guard<std::mutex> lock(g_lock);
            if (!g_again) { g_running = false; g_scanning = false; return; }
        }
    }))
    {
        g_running = false;
        g_scanning = false;
    }
}

} // namespace

// ============================================================================================== folder
std::wstring ShadersFolder()
{
    const std::wstring& apex = ApexPaths::ApexDirectory();
    return apex.empty() ? std::wstring() : apex + L"Shaders\\";
}

bool EnsureShadersFolder()
{
    const std::wstring dir = ShadersFolder();
    if (dir.empty() || !ApexPaths::EnsureApexDirectory()) return false;
    for (const std::wstring& d : { dir, dir + L"Shaders\\", dir + L"Textures\\" })
        if (!CreateDirectoryW(d.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return false;
    return true;
}

void RequestScan(bool force)
{
    std::lock_guard<std::mutex> lock(g_lock);
    g_force |= force;
    g_requested = true;
}

bool Scanning() { return g_scanning || g_requested; }

bool Poll(UINT width, UINT height)
{
    g_framePasses = 0;
    const UINT W = g_wantW ? g_wantW : width, H = g_wantH ? g_wantH : height;
    std::shared_ptr<Catalog> pub;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        if (g_requested && W && H)
        {
            g_requested = false;
            g_scanW = W; g_scanH = H;
            if (g_running) g_again = true; else StartWorker();
        }
        pub = g_published;
    }
    if (!pub || (g_current && pub->generation == g_current->generation)) return false;
    g_current = std::move(pub);
    // instances of effects the new table no longer has are released at the next ReleaseGpu or Run
    return true;
}

int Generation() { return g_current ? g_current->generation : 0; }
bool Scanned() { return g_current != nullptr; }

const LooksShader* UserShader(int index)
{
    if (!g_current || index < 0 || index >= (int)g_current->techs.size()) return nullptr;
    return &g_current->techs[index]->desc;
}

int FindUser(const std::string& technique, const std::string& file)
{
    if (!g_current) return -1;
    for (size_t i = 0; i < g_current->techs.size(); ++i)
    {
        const UserTech& u = *g_current->techs[i];
        if (u.reason.empty() && u.technique == technique && _stricmp(u.file.c_str(), file.c_str()) == 0) return (int)i;
    }
    return -1;
}

std::string Reason(const std::string& technique, const std::string& file)
{
    if (!g_current) return {};
    for (const auto& u : g_current->techs)
        if (u->technique == technique && _stricmp(u->file.c_str(), file.c_str()) == 0) return u->reason;
    for (const auto& [f, why] : g_current->fileErrors)
        if (_stricmp(f.c_str(), file.c_str()) == 0) return why;
    if (HasFile(file)) return "the file has no technique of this name";
    return {};
}

bool Pending(const std::string& file)
{
    if (!g_current) return Scanning();
    const std::string l = LowerA(file);
    return std::find(g_current->pending.begin(), g_current->pending.end(), l) != g_current->pending.end();
}

bool HasFile(const std::string& file)
{
    if (!g_current) return false;
    const std::string l = LowerA(file);
    return std::find(g_current->files.begin(), g_current->files.end(), l) != g_current->files.end();
}

std::vector<Listed> List()
{
    std::vector<Listed> out;
    if (!g_current) return out;
    for (size_t i = 0; i < g_current->techs.size(); ++i)
    {
        const UserTech& u = *g_current->techs[i];
        if (u.hidden) continue;
        Listed l;
        l.technique = u.technique; l.file = u.file; l.reason = u.reason;
        l.index = u.reason.empty() ? (int)i : -1;
        out.push_back(std::move(l));
    }
    for (const auto& [f, why] : g_current->fileErrors)
    {
        Listed l;
        l.file = f; l.technique = f.substr(0, f.rfind('.')); l.reason = why;
        out.push_back(std::move(l));
    }
    return out;
}

AddResult AddFiles(const std::vector<std::wstring>& paths)
{
    AddResult r;
    if (!EnsureShadersFolder()) { r.refused = (int)paths.size(); return r; }
    const std::wstring root = ShadersFolder();
    for (const std::wstring& path : paths)
    {
        const size_t slash = path.find_last_of(L"\\/"), dot = path.find_last_of(L'.');
        const std::wstring name = path.substr(slash == std::wstring::npos ? 0 : slash + 1);
        const std::wstring ext = dot == std::wstring::npos || (slash != std::wstring::npos && dot < slash) ? L"" : path.substr(dot);
        auto is = [&](const wchar_t* e) { return _wcsicmp(ext.c_str(), e) == 0; };
        const bool code = is(L".fx") || is(L".fxh");
        const bool image = is(L".png") || is(L".jpg") || is(L".jpeg") || is(L".bmp") || is(L".dds");
        WIN32_FILE_ATTRIBUTE_DATA a{};
        const uint64_t cap = code ? kMaxFxBytes : kMaxTextureFileBytes;
        if ((!code && !image) || name.empty() || !GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a) || a.nFileSizeHigh ||
            (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || a.nFileSizeLow > cap)
        { ++r.refused; continue; }
        std::string bytes;
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h == INVALID_HANDLE_VALUE) { ++r.refused; continue; }
        bytes.resize(a.nFileSizeLow);
        DWORD got = 0;
        const bool read = bytes.empty() || (ReadFile(h, bytes.data(), (DWORD)bytes.size(), &got, nullptr) && got == bytes.size());
        CloseHandle(h);
        if (!read) { ++r.refused; continue; }
        const std::wstring dir = root + (code ? L"Shaders\\" : L"Textures\\");
        const std::wstring stem = name.substr(0, name.size() - ext.size());
        bool done = false;
        for (int n = 0; n < 100 && !done; ++n)
        {
            const std::wstring target = dir + (n ? stem + L"-" + std::to_wstring(n) + ext : name);
            WIN32_FILE_ATTRIBUTE_DATA t{};
            if (GetFileAttributesExW(target.c_str(), GetFileExInfoStandard, &t))
            {
                if (t.nFileSizeHigh || t.nFileSizeLow != bytes.size()) continue;
                std::string have;
                HANDLE hh = CreateFileW(target.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
                if (hh == INVALID_HANDLE_VALUE) continue;
                have.resize(bytes.size());
                DWORD g = 0;
                const bool ok = have.empty() || (ReadFile(hh, have.data(), (DWORD)have.size(), &g, nullptr) && g == have.size());
                CloseHandle(hh);
                if (ok && have == bytes) { ++r.reused; done = true; }
                continue;
            }
            HANDLE w = CreateFileW(target.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (w == INVALID_HANDLE_VALUE) continue;
            DWORD put = 0;
            const bool ok = bytes.empty() || (WriteFile(w, bytes.data(), (DWORD)bytes.size(), &put, nullptr) && put == bytes.size());
            CloseHandle(w);
            if (!ok) { DeleteFileW(target.c_str()); break; }
            ++r.added;
            done = true;
        }
        if (!done) ++r.refused;
    }
    LOG_INFO("[Looks] Add .fx files: " + std::to_string(r.added) + " added, " + std::to_string(r.reused) + " already there, " + std::to_string(r.refused) + " refused");
    RequestScan(true); // an included file may have changed: compile every file again
    return r;
}

// ============================================================================================== run (render thread)
namespace {
struct Live
{
    Instance inst;
    SourceState sources;
    std::vector<float> regs;
    std::string error; // Create failed: not tried again until ReleaseGpu
};
// never destroyed: at process exit the device may be gone before static destructors run
std::map<const Effect*, std::unique_ptr<Live>>& g_live = *new std::map<const Effect*, std::unique_ptr<Live>>();
}

void ReleaseGpu()
{
    g_live.clear();
}

uint64_t GpuBytes()
{
    uint64_t n = 0;
    for (auto& [k, l] : g_live) n += l->inst.GpuBytes();
    return n;
}

bool Run(const RunFrame& f, int index, const float* values, std::string* why)
{
    auto fail = [&](std::string s) { if (why) *why = std::move(s); return false; };
    if (!g_current || index < 0 || index >= (int)g_current->techs.size()) return fail("not compiled yet");
    const UserTech& u = *g_current->techs[index];
    const Effect& fx = *u.fx;
    if (fx.width != f.width || fx.height != f.height)
    {
        if (g_wantW != f.width || g_wantH != f.height) { g_wantW = f.width; g_wantH = f.height; RequestScan(false); }
        return fail("compiling for " + std::to_string(f.width) + "x" + std::to_string(f.height));
    }
    const size_t passes = fx.techniques[u.tech].passes.size();
    if (g_framePasses + (int)passes > kMaxFramePasses) return fail("over the frame's limit of 256 passes");

    // drop instances of effects no longer in the table
    for (auto it = g_live.begin(); it != g_live.end();)
    {
        bool used = false;
        for (const auto& t : g_current->techs) if (t->fx.get() == it->first) { used = true; break; }
        it = used ? std::next(it) : g_live.erase(it);
    }
    std::unique_ptr<Live>& live = g_live[&fx];
    if (!live)
    {
        live = std::make_unique<Live>();
        if (GpuBytes() + fx.gpuBytes > kMaxTotalGpuBytes) live->error = "the shaders together would need more than 384 MB";
        else if (!live->inst.Create(f.dev, u.fx, &live->error) && live->error.empty()) live->error = "could not be created";
        if (!live->error.empty()) LOG_WARNING("[Looks] " + u.file + ": " + live->error);
    }
    if (!live->error.empty()) return fail(live->error);

    DefaultValues(fx, live->regs);
    for (size_t p = 0; p < u.params.size(); ++p)
        if (u.params[p].kind != LooksParamKind::Source) SetUniform(fx, (size_t)u.uniform[p], values + p * 4, (size_t)u.params[p].comps, live->regs);
    SourceState& st = live->sources;
    st.timerMs = f.timerMs; st.frametimeMs = f.frametimeMs; st.frame = f.frame; st.depthReady = f.depth != nullptr;
    UpdateSources(fx, live->regs, st);
    g_framePasses += (int)passes;
    if (!live->inst.Run(f.dev, u.tech, f.target, f.depth, live->regs)) return fail("did not run");
    return true;
}

} // namespace Looks::Fx
