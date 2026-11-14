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
#include <d3dcompiler.h>
#include "looks_fx.h"
#include "looks_runner.h"
#include "apex_log.h"
#include "apex_paths.h"
#include "apex_util.h"
#include "hook_guard.h"

#include <algorithm>
#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <set>

namespace Looks::Fx {

namespace {

constexpr size_t kMaxFiles = 500;              // .fx files read from the player's own folder, and from each pack
constexpr size_t kMaxPackFiles = 3000;         // .fx files read from every downloaded pack together
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
    std::string technique, file, reason, pack;
    std::shared_ptr<const Effect> fx;
    size_t tech = 0;
    size_t seq = 0;           // place in the scan's file order (List)
    bool hidden = false;
    std::deque<std::string> text;
    std::vector<LooksParam> params;
    std::vector<int> uniform; // per param: index in fx->uniforms
    LooksShader desc{};
    const char* Keep(std::string s) { text.push_back(std::move(s)); return text.back().c_str(); }
};

// A pack file not compiled this session whose techniques run here, as an earlier compile found (the check cache)
struct CheckedTech { std::string technique, file, pack; size_t seq = 0; };

struct Catalog
{
    std::vector<std::unique_ptr<UserTech>> techs;
    std::vector<CheckedTech> checked;                            // runnable, not hidden, of files not compiled
    std::vector<std::pair<std::string, std::string>> fileErrors; // file, reason
    std::vector<std::string> files;                              // lower-case .fx names in the folder
    std::vector<std::string> pending;                            // ... of those not compiled yet
    std::vector<std::string> lazy;                               // ... pack files left uncompiled (not wanted)
    std::vector<std::string> packs;                              // pack folders
    std::map<std::string, PackCount> counts;                     // per pack folder
    int generation = 0;
};

// The parameters of an effect: its uniforms without a source annotation, as the menu shows ReShade's
void Describe(UserTech& u)
{
    DescribeParams(*u.fx, u.text, u.params, u.uniform);
    const Effect& fx = *u.fx;
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

// The largest free block of this process's addresses
size_t LargestFreeBlock()
{
    size_t best = 0;
    MEMORY_BASIC_INFORMATION m{};
    for (uintptr_t p = 0x10000; VirtualQuery(reinterpret_cast<void*>(p), &m, sizeof m) == sizeof m; p = reinterpret_cast<uintptr_t>(m.BaseAddress) + m.RegionSize)
    {
        if (m.State == MEM_FREE && m.RegionSize > best) best = m.RegionSize;
        if (reinterpret_cast<uintptr_t>(m.BaseAddress) + m.RegionSize < p) break; // wrapped
    }
    return best;
}

// Compiles one file on its own thread (16 MB stack). A file that takes too long is refused and its thread left behind.
std::shared_ptr<const Effect> CompileGuarded(const std::wstring& path, const Folders& folders, UINT w, UINT h)
{
    // The compiler's own heap takes hundreds of MB for a few large shaders while it runs; the process heap growing beside it
    // then lands its new segments in the middle of the largest free block for good (measured: 1202 MB -> 794 MB after the
    // first pack check). Growing the process heap by 40 MB once, before the first compile, keeps them at the bottom
    // (1202 -> 1154 MB). 256 KB blocks: larger ones skip the heap segments and go straight to VirtualAlloc.
    static std::once_flag grown;
    std::call_once(grown, [] {
        void* blocks[160];
        for (void*& b : blocks) b = HeapAlloc(GetProcessHeap(), 0, 256u << 10);
        for (void* b : blocks) if (b) HeapFree(GetProcessHeap(), 0, b);
    });
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

// One .fx of the scan: the player's own (pack "") or a downloaded pack's, with where its includes and textures are found
struct Source { std::wstring path; std::string pack; size_t folders = 0; };

// The check cache (Shaders\.cache\checked.txt): per pack .fx, its size and time and which techniques run here, from the
// last compile of that file in any session. The Add a shader list and "<x> of <y> run here" read it, so browsing compiles
// nothing; a file not in it (new or changed) is checked once in the background while the player browses.
struct Checked
{
    uint64_t size = 0;
    FILETIME written{};
    std::vector<std::pair<std::string, uint8_t>> techs; // technique, kRuns | kHidden
};
constexpr uint8_t kRuns = 1, kHidden = 2;
std::map<std::wstring, Checked> g_checked; // worker only; by full path
bool g_checkedLoaded = false, g_checkedDirty = false;
std::atomic<int> g_checkedThisSession{ 0 }; // files checked in the background (log, tools)

std::wstring CheckedFile(const std::wstring& root) { return root + L".cache\\checked.txt"; }
std::string CheckedHeader() { return "ApexLooksChecked 1 " + std::to_string(D3D_COMPILER_VERSION); }

void LoadChecked(const std::wstring& root)
{
    g_checkedLoaded = true;
    g_checked.clear();
    HANDLE h = CreateFileW(CheckedFile(root).c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    std::string text;
    LARGE_INTEGER size{};
    if (GetFileSizeEx(h, &size) && size.QuadPart > 0 && size.QuadPart < (8 << 20))
    {
        text.resize((size_t)size.QuadPart);
        DWORD got = 0;
        if (!ReadFile(h, text.data(), (DWORD)text.size(), &got, nullptr) || got != text.size()) text.clear();
    }
    CloseHandle(h);
    // first line: the header (another D3DCompiler or format: start again); then size \t time \t path \t tech|flags;...
    size_t pos = text.find('\n');
    if (pos == std::string::npos || text.substr(0, pos) != CheckedHeader()) return;
    for (++pos; pos < text.size() && g_checked.size() < 20000;)
    {
        size_t e = text.find('\n', pos);
        if (e == std::string::npos) e = text.size();
        const std::string line = text.substr(pos, e - pos);
        pos = e + 1;
        const size_t t1 = line.find('\t'), t2 = t1 == std::string::npos ? t1 : line.find('\t', t1 + 1), t3 = t2 == std::string::npos ? t2 : line.find('\t', t2 + 1);
        if (t3 == std::string::npos) continue;
        Checked c;
        unsigned long long when = 0;
        c.size = strtoull(line.c_str(), nullptr, 10);
        when = strtoull(line.c_str() + t1 + 1, nullptr, 10);
        c.written.dwLowDateTime = (DWORD)when; c.written.dwHighDateTime = (DWORD)(when >> 32);
        for (size_t a = t3 + 1; a < line.size();)
        {
            size_t b = line.find(';', a);
            if (b == std::string::npos) b = line.size();
            const std::string item = line.substr(a, b - a);
            a = b + 1;
            const size_t bar = item.rfind('|');
            if (bar == std::string::npos || bar == 0) continue;
            c.techs.push_back({ item.substr(0, bar), (uint8_t)atoi(item.c_str() + bar + 1) });
        }
        g_checked[ApexUtil::ToWide(line.substr(t2 + 1, t3 - t2 - 1))] = std::move(c);
    }
}

void SaveChecked(const std::wstring& root)
{
    if (!g_checkedDirty) return;
    g_checkedDirty = false;
    std::string text = CheckedHeader() + "\n";
    for (const auto& [path, c] : g_checked)
    {
        const unsigned long long when = ((unsigned long long)c.written.dwHighDateTime << 32) | c.written.dwLowDateTime;
        text += std::to_string(c.size) + "\t" + std::to_string(when) + "\t" + Utf8(path) + "\t";
        for (const auto& [t, f] : c.techs) text += t + "|" + std::to_string(f) + ";";
        text += "\n";
    }
    const std::wstring dir = root + L".cache\\";
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring file = CheckedFile(root), tmp = file + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD put = 0;
    const bool ok = WriteFile(h, text.data(), (DWORD)text.size(), &put, nullptr) && put == text.size();
    CloseHandle(h);
    if (!ok || !MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING)) DeleteFileW(tmp.c_str());
}

// A compiled pack file's result, kept in the check cache
void RecordChecked(const std::wstring& path, const WIN32_FILE_ATTRIBUTE_DATA& a, const Effect& fx)
{
    Checked c;
    c.size = ((uint64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow;
    c.written = a.ftLastWriteTime;
    if (fx.error.empty())
        for (const Technique& t : fx.techniques) c.techs.push_back({ t.name, (uint8_t)((t.reason.empty() ? kRuns : 0) | (t.hidden ? kHidden : 0)) });
    g_checked[path] = std::move(c);
    g_checkedDirty = true;
}

// The check cache's entry for a file when it still matches the file (size and time), else nullptr
const Checked* CheckedFor(const std::wstring& path, const WIN32_FILE_ATTRIBUTE_DATA& a)
{
    auto it = g_checked.find(path);
    if (it == g_checked.end()) return nullptr;
    const uint64_t size = ((uint64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow;
    return it->second.size == size && CompareFileTime(&it->second.written, &a.ftLastWriteTime) == 0 ? &it->second : nullptr;
}

std::shared_ptr<Catalog> Build(const std::vector<std::shared_ptr<const Effect>>& effects, const std::vector<Source>& files,
                               const std::vector<bool>& lazy, const std::vector<std::string>& packs, const std::vector<const Checked*>& checked)
{
    auto c = std::make_shared<Catalog>();
    c->packs = packs;
    std::map<std::string, bool> seen; // "tech@file" lower: the first file of a name wins, as in ReShade's search (the player's own first)
    for (size_t i = 0; i < files.size(); ++i)
    {
        c->files.push_back(LowerA(Utf8(files[i].path.substr(files[i].path.find_last_of(L'\\') + 1))));
        if (!effects[i]) (lazy[i] ? c->lazy : c->pending).push_back(c->files.back());
        if (!files[i].pack.empty())
        {
            PackCount& n = c->counts[files[i].pack];
            if (i == 0 || files[i - 1].pack != files[i].pack) n.checked = true;
            n.checked &= effects[i] != nullptr || checked[i] != nullptr;
        }
    }
    for (size_t i = 0; i < effects.size(); ++i)
    {
        const auto& fx = effects[i];
        if (!fx && checked[i]) // not compiled this session: what an earlier compile found
        {
            const std::string file = Utf8(files[i].path.substr(files[i].path.find_last_of(L'\\') + 1));
            for (const auto& [name, flags] : checked[i]->techs)
            {
                if (!(flags & kHidden))
                {
                    PackCount& n = c->counts[files[i].pack];
                    ++n.total;
                    n.runnable += (flags & kRuns) != 0;
                }
                if (!seen.emplace(LowerA(name + "@" + file), true).second || !(flags & kRuns) || (flags & kHidden)) continue;
                c->checked.push_back({ name, file, files[i].pack, i });
            }
            continue;
        }
        if (!fx) continue;
        if (!fx->error.empty()) { c->fileErrors.push_back({ fx->file, fx->error }); continue; }
        for (size_t t = 0; t < fx->techniques.size(); ++t)
        {
            const Technique& tq = fx->techniques[t];
            if (!files[i].pack.empty() && !tq.hidden)
            {
                PackCount& n = c->counts[files[i].pack];
                ++n.total;
                n.runnable += tq.reason.empty();
            }
            if (!seen.emplace(LowerA(tq.name + "@" + fx->file), true).second) continue;
            auto u = std::make_unique<UserTech>();
            u->technique = tq.name; u->file = fx->file; u->reason = tq.reason; u->fx = fx; u->tech = t; u->hidden = tq.hidden; u->pack = files[i].pack;
            u->seq = i;
            Describe(*u);
            c->techs.push_back(std::move(u));
        }
    }
    return c;
}

std::set<std::string> g_want;    // lower-case .fx names the render thread wants compiled (under g_lock)
std::map<std::string, LooksDefines> g_defines; // the look's definitions per lower-case .fx name (under g_lock)
bool g_browse = false;            // the player browses shaders: pack files without a check result are checked (under g_lock)

void Publish(const std::vector<std::shared_ptr<const Effect>>& effects, const std::vector<Source>& files, const std::vector<bool>& lazy,
             const std::vector<std::string>& packs, const std::vector<const Checked*>& checked)
{
    auto c = Build(effects, files, lazy, packs, checked);
    std::lock_guard<std::mutex> lock(g_lock);
    c->generation = g_nextGeneration++;
    g_published = std::move(c);
}

// Shaders\Packs\<folder>: complete downloads only (a download in progress is "<folder>.part")
std::vector<std::string> ListPacks(const std::wstring& dir)
{
    std::vector<std::string> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return out;
    do
    {
        const std::wstring n = fd.cFileName;
        if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) || n[0] == L'.' || n.find(L'.') != std::wstring::npos)
            continue;
        out.push_back(Utf8(n));
    } while (out.size() < 200 && FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end(), [](const std::string& a, const std::string& b) { return _stricmp(a.c_str(), b.c_str()) < 0; });
    return out;
}

void ScanOnce(UINT W, UINT H, bool force)
{
    const std::wstring root = ShadersFolder();
    if (root.empty()) return;
    const std::wstring packRoot = root + L"Packs\\";
    const std::vector<std::string> packs = ListPacks(packRoot);
    auto byName = [](const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; };
    // includes and textures: the file's own folders, the player's folder, then every pack's (the standard effects first:
    // ReShade.fxh), as ReShade finds them in one reshade-shaders tree
    std::vector<std::wstring> packShaders, packTextures;
    for (const std::string& p : packs)
    {
        const std::wstring d = packRoot + ApexUtil::ToWide(p) + L"\\";
        const bool standard = _stricmp(p.c_str(), "ReShade standard effects") == 0;
        packShaders.insert(standard ? packShaders.begin() : packShaders.end(), d + L"Shaders\\");
        packTextures.insert(standard ? packTextures.begin() : packTextures.end(), d + L"Textures\\");
    }
    if (!g_checkedLoaded) LoadChecked(root);
    if (force && !g_checked.empty()) { g_checked.clear(); g_checkedDirty = true; } // Check again: an included file may have changed
    std::vector<Folders> folders(1 + packs.size());
    folders[0].root = root;
    folders[0].includeDirs = { root + L"Shaders\\", root };
    folders[0].textureDirs = { root + L"Textures\\", root };
    std::vector<Source> files;
    {
        std::vector<std::wstring> mine;
        ListFx(root, 0, mine);
        ListFx(root + L"Shaders\\", 3, mine);
        std::sort(mine.begin(), mine.end(), byName);
        for (auto& p : mine) files.push_back({ p, "", 0 });
    }
    size_t packFiles = 0;
    for (size_t k = 0; k < packs.size(); ++k)
    {
        const std::wstring d = packRoot + ApexUtil::ToWide(packs[k]) + L"\\";
        Folders& f = folders[k + 1];
        f.root = root;
        f.includeDirs = { d + L"Shaders\\" };
        f.textureDirs = { d + L"Textures\\" };
        std::vector<std::wstring> list;
        if (packFiles < kMaxPackFiles) ListFx(d + L"Shaders\\", 3, list);
        std::sort(list.begin(), list.end(), byName);
        for (auto& p : list)
            if (packFiles++ < kMaxPackFiles) files.push_back({ p, packs[k], k + 1 });
    }
    for (Folders& f : folders)
    {
        f.cacheDir = root + L".cache\\"; // compiled shaders, kept between sessions
        if (&f != &folders[0]) { f.includeDirs.push_back(root + L"Shaders\\"); f.textureDirs.push_back(root + L"Textures\\"); }
        f.includeDirs.insert(f.includeDirs.end(), packShaders.begin(), packShaders.end());
        f.textureDirs.insert(f.textureDirs.end(), packTextures.begin(), packTextures.end());
    }
    if (force) g_cache.clear();
    std::set<std::string> want;
    std::map<std::string, LooksDefines> defines;
    bool browse;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        want = g_want;
        defines = g_defines;
        browse = g_browse;
    }
    static const LooksDefines kNone;
    auto definesOf = [&](size_t i) -> const LooksDefines& {
        auto it = defines.find(LowerA(Utf8(files[i].path.substr(files[i].path.find_last_of(L'\\') + 1))));
        return it == defines.end() ? kNone : it->second;
    };

    std::vector<std::shared_ptr<const Effect>> effects(files.size());
    std::vector<bool> lazy(files.size(), false);
    std::vector<const Checked*> checked(files.size(), nullptr);
    std::vector<WIN32_FILE_ATTRIBUTE_DATA> attrs(files.size());
    std::vector<size_t> todo;
    for (size_t i = 0; i < files.size(); ++i)
    {
        WIN32_FILE_ATTRIBUTE_DATA& a = attrs[i];
        if (!GetFileAttributesExW(files[i].path.c_str(), GetFileExInfoStandard, &a)) continue;
        const uint64_t size = ((uint64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow;
        auto it = g_cache.find(files[i].path);
        if (it != g_cache.end() && it->second.size == size && CompareFileTime(&it->second.written, &a.ftLastWriteTime) == 0 &&
            it->second.w == W && it->second.h == H)
        {
            effects[i] = it->second.fx; // with other definitions: compiled again, this one runs until then
            if (!MacrosMatch(*it->second.fx, definesOf(i))) todo.push_back(i);
        }
        else if (!files[i].pack.empty() && !want.count(LowerA(Utf8(files[i].path.substr(files[i].path.find_last_of(L'\\') + 1)))))
        {
            lazy[i] = true; // a pack file no look needs: not compiled (memory and time); the check cache says what runs
            checked[i] = CheckedFor(files[i].path, a);
        }
        else todo.push_back(i);
    }
    Publish(effects, files, lazy, packs, checked); // what is known already (cached files) shows at once
    const DWORD t0 = GetTickCount();
    for (size_t i : todo)
    {
        WIN32_FILE_ATTRIBUTE_DATA a{};
        GetFileAttributesExW(files[i].path.c_str(), GetFileExInfoStandard, &a);
        Folders f = folders[files[i].folders];
        f.defines = definesOf(i);
        effects[i] = CompileGuarded(files[i].path, f, W, H);
        CacheEntry& e = g_cache[files[i].path];
        e.written = a.ftLastWriteTime; e.size = ((uint64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow; e.w = W; e.h = H; e.fx = effects[i];
        if (!files[i].pack.empty()) RecordChecked(files[i].path, a, *effects[i]);
        Publish(effects, files, lazy, packs, checked); // each compiled file shows as soon as it is ready
        const Effect& fx = *effects[i];
        const std::string where = files[i].pack.empty() ? fx.file : files[i].pack + "\\" + fx.file;
        if (!fx.error.empty()) LOG_INFO("[Looks] " + where + " can't run here: " + fx.error);
        for (const Technique& t : fx.techniques)
            if (!t.reason.empty()) LOG_INFO("[Looks] " + where + ", " + t.name + " can't run here: " + t.reason);
        for (const std::string& n : fx.notes) LOG_INFO("[Looks] " + where + ": Direct3D 9 fallback, " + n);
    }
    for (auto it = g_cache.begin(); it != g_cache.end();)
        it = std::find_if(files.begin(), files.end(), [&](const Source& s) { return s.path == it->first; }) == files.end() ? g_cache.erase(it) : std::next(it);
    if (!todo.empty())
        LOG_INFO("[Looks] Shaders folder: compiled " + std::to_string(todo.size()) + " of " + std::to_string(files.size()) + " .fx files for " +
                 std::to_string(W) + "x" + std::to_string(H) + " in " + std::to_string(GetTickCount() - t0) + " ms");
    SaveChecked(root);

    // While the player browses shaders: pack files the check cache does not know (new or changed) are checked once, one at a
    // time at low priority, each result kept and the effect let go. A new scan request (a look that wants a file) comes first.
    if (!browse) return;
    int done = 0;
    const DWORD t1 = GetTickCount();
    for (size_t i = 0; i < files.size(); ++i)
    {
        if (!lazy[i] || checked[i]) continue;
        {
            std::lock_guard<std::mutex> lock(g_lock);
            if (g_again || g_requested) break;
        }
        // a large shader's compile briefly needs up to ~420 MB: background checks wait for room rather than risk the game
        if (LargestFreeBlock() < (size_t(768) << 20))
        {
            LOG_INFO("[Looks] Shader packs: background checks paused, little free memory left");
            break;
        }
        const std::shared_ptr<const Effect> fx = CompileGuarded(files[i].path, folders[files[i].folders], W, H);
        RecordChecked(files[i].path, attrs[i], *fx);
        checked[i] = CheckedFor(files[i].path, attrs[i]);
        ++done;
        ++g_checkedThisSession;
        Publish(effects, files, lazy, packs, checked);
        if (done % 25 == 0) SaveChecked(root);
        Sleep(20); // the game first
    }
    SaveChecked(root);
    if (done) LOG_INFO("[Looks] Shader packs: checked " + std::to_string(done) + " .fx files in the background in " + std::to_string(GetTickCount() - t1) + " ms");
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

// ============================================================================================== parameters
void DescribeParams(const Effect& fx, std::deque<std::string>& text, std::vector<LooksParam>& params, std::vector<int>& uniform)
{
    auto keep = [&](std::string s) { text.push_back(std::move(s)); return text.back().c_str(); };
    auto str = [](const Ann* a) { return a && a->isString && !a->s.empty() ? a : nullptr; };
    for (size_t i = 0; i < fx.uniforms.size(); ++i)
    {
        const Uniform& un = fx.uniforms[i];
        LooksParam p{};
        p.key = keep(un.name);
        const Ann* label = str(FindAnn(un.ann, "ui_label"));
        p.label = keep(label ? label->s : un.name);
        p.comps = (int)std::min(4u, un.Comps());
        p.reg = (int)i;
        for (int c = 0; c < p.comps && c < (int)un.init.size(); ++c) p.def[c] = un.init[c];
        const Ann* type = FindAnn(un.ann, "ui_type");
        const std::string t = type && type->isString ? type->s : "";
        const Ann* hid = FindAnn(un.ann, "hidden");
        if (FindAnn(un.ann, "source") || un.array || un.cols > 1)
            p.kind = LooksParamKind::Source; // set by the runtime, or no control (matrix, array): the .fx's value
        else if (un.base == kBool) p.kind = LooksParamKind::Bool;
        else if (t == "combo" || t == "radio" || t == "list") p.kind = LooksParamKind::Combo;
        else if (t == "color") p.kind = LooksParamKind::Color;
        else p.kind = un.base == kFloat ? LooksParamKind::Float : LooksParamKind::Int;
        if (hid && (hid->isString ? hid->s == "true" : hid->f[0] != 0.0f)) p.flags |= kParamHidden;
        if (t.empty() || t == "input") p.flags |= kParamField;
        const Ann* mn = FindAnn(un.ann, "ui_min");
        const Ann* mx = FindAnn(un.ann, "ui_max");
        const Ann* step = FindAnn(un.ann, "ui_step");
        p.min = mn && !mn->isString ? mn->f[0] : 0.0f;
        p.max = mx && !mx->isString ? mx->f[0] : (p.kind == LooksParamKind::Color ? 1.0f : 0.0f);
        p.step = step && !step->isString && step->f[0] > 0.0f ? step->f[0] : 0.0f;
        if (const Ann* a = str(FindAnn(un.ann, "ui_tooltip"))) p.tooltip = keep(a->s);
        if (const Ann* a = str(FindAnn(un.ann, "ui_category"))) p.category = keep(a->s);
        if (const Ann* a = str(FindAnn(un.ann, "ui_text"))) p.text = keep(a->s);
        if (const Ann* a = FindAnn(un.ann, "ui_category_closed"); a && (a->isString ? a->s == "true" : a->f[0] != 0.0f)) p.flags |= kParamClosed;
        if (p.kind == LooksParamKind::Combo)
        {
            const Ann* items = FindAnn(un.ann, "ui_items");
            if (items && items->isString) p.items = keep(items->s + '\0'); // ReShade's '\0' separated list (the last '\0' optional)
            const size_t n = LooksItems(p).size();
            if (!mx && n) { p.min = 0; p.max = (float)(n - 1); }
        }
        if (p.kind == LooksParamKind::Source) p.items = "";
        params.push_back(p);
        uniform.push_back((int)i);
    }
}

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

int CheckedThisSession() { return g_checkedThisSession; }

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

const Effect* UserEffect(int index)
{
    if (!g_current || index < 0 || index >= (int)g_current->techs.size()) return nullptr;
    return g_current->techs[index]->fx.get();
}

void SetDefines(const std::map<std::string, LooksDefines>& perFile)
{
    {
        std::lock_guard<std::mutex> lock(g_lock);
        if (perFile == g_defines) return;
        g_defines = perFile;
    }
    if (g_current) RequestScan(false); // before the first scan: that scan uses them
}

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
    if (std::find(g_current->pending.begin(), g_current->pending.end(), l) != g_current->pending.end()) return true;
    if (std::find(g_current->lazy.begin(), g_current->lazy.end(), l) == g_current->lazy.end()) return false;
    std::lock_guard<std::mutex> lock(g_lock); // a pack file asked for since the last scan
    return g_want.count(l) != 0;
}

void WantFiles(const std::vector<std::string>& files)
{
    std::set<std::string> want;
    for (const std::string& f : files) want.insert(LowerA(f));
    bool scan = false;
    {
        std::lock_guard<std::mutex> lock(g_lock);
        for (const std::string& f : want)
            if (!g_want.count(f) && g_current && std::find(g_current->lazy.begin(), g_current->lazy.end(), f) != g_current->lazy.end()) scan = true;
        g_want.insert(want.begin(), want.end()); // kept for the session: a file compiled once stays compiled
    }
    if (scan) RequestScan(false);
}

void BrowsePacks()
{
    {
        std::lock_guard<std::mutex> lock(g_lock);
        if (g_browse) return;
        g_browse = true;
    }
    RequestScan(false);
}

std::map<std::string, std::string> FindFiles(const std::vector<std::string>& names)
{
    std::map<std::string, std::string> out;
    const std::wstring root = ShadersFolder();
    if (root.empty() || names.empty()) return out;
    std::set<std::string> wanted;
    for (const std::string& n : names) wanted.insert(LowerA(n));
    auto nameOf = [](const std::wstring& path) { return LowerA(Utf8(path.substr(path.find_last_of(L'\\') + 1))); };
    std::vector<std::wstring> mine;
    ListFx(root, 0, mine);
    ListFx(root + L"Shaders\\", 3, mine);
    for (const std::wstring& p : mine) if (wanted.count(nameOf(p))) out.emplace(nameOf(p), std::string());
    const std::wstring packRoot = root + L"Packs\\";
    size_t packFiles = 0;
    std::set<std::string> inPack;
    for (const std::string& pack : ListPacks(packRoot)) // in the scan's order: the first pack with the file
    {
        std::vector<std::wstring> list;
        if (packFiles < kMaxPackFiles) ListFx(packRoot + ApexUtil::ToWide(pack) + L"\\Shaders\\", 3, list);
        for (const std::wstring& p : list)
        {
            if (packFiles++ >= kMaxPackFiles) break;
            const std::string n = nameOf(p);
            if (wanted.count(n) && inPack.insert(n).second) out[n] = pack; // a pack's file is named over the player's own
        }
    }
    return out;
}

std::vector<std::string> PackFolders() { return g_current ? g_current->packs : std::vector<std::string>(); }

PackCount CountPack(const std::string& folder)
{
    if (!g_current) return {};
    for (const auto& [k, n] : g_current->counts)
        if (_stricmp(k.c_str(), folder.c_str()) == 0) return n;
    PackCount none;
    for (const std::string& p : g_current->packs) none.checked |= _stricmp(p.c_str(), folder.c_str()) == 0; // a pack without .fx files
    return none;
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
    std::vector<size_t> seq;
    for (size_t i = 0; i < g_current->techs.size(); ++i)
    {
        const UserTech& u = *g_current->techs[i];
        if (u.hidden) continue;
        Listed l;
        l.technique = u.technique; l.file = u.file; l.reason = u.reason; l.pack = u.pack;
        l.index = u.reason.empty() ? (int)i : -1;
        out.push_back(std::move(l));
        seq.push_back(u.seq);
    }
    for (const CheckedTech& t : g_current->checked) // in the scan's file order with the compiled ones
    {
        Listed l;
        l.technique = t.technique; l.file = t.file; l.pack = t.pack; l.checked = true;
        const size_t at = (size_t)(std::upper_bound(seq.begin(), seq.end(), t.seq) - seq.begin());
        out.insert(out.begin() + (std::ptrdiff_t)at, std::move(l));
        seq.insert(seq.begin() + (std::ptrdiff_t)at, t.seq);
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
    unsigned lastFrame = 0; // the last frame it ran: an instance not run for a while gives its GPU memory back
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

    // drop instances of effects no longer in the table, or not run for 120 frames (switched off, out of the look): their
    // render targets would otherwise stay until a device reset and count against the 384 MB
    for (auto it = g_live.begin(); it != g_live.end();)
    {
        bool used = it->first == &fx || (unsigned)f.frame - it->second->lastFrame < 120u;
        if (used)
        {
            used = false;
            for (const auto& t : g_current->techs) if (t->fx.get() == it->first) { used = true; break; }
        }
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
    live->lastFrame = (unsigned)f.frame; // a failed one too: not created again every 120 frames
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
