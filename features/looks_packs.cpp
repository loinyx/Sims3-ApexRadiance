// Looks: the shader packs of ReShade's list, downloaded on the player's click (see looks_packs.h)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <winhttp.h>
#include "looks_packs.h"
#include "looks_fx.h"
#include "apex_log.h"
#include "apex_util.h"
#include "hook_guard.h"
#include "third_party/miniz/miniz.h"

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <mutex>
#include <set>

#pragma comment(lib, "winhttp.lib")

namespace Looks::Packs {

namespace {

std::string Lower(std::string s)
{
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
}

bool IEq(const std::string& a, const std::string& b) { return _stricmp(a.c_str(), b.c_str()) == 0; }

// ---------------------------------------------------------------------------------------------- shared state
std::mutex g_mu;                    // guards everything below
Batch g_batch;
bool g_workerRunning = false;
bool g_stop = false;
std::set<std::string> g_unavailable; // lower-case folders
std::string g_listText;              // ReShade's list as read ("" = not read: the built-in index)
ListState g_listState = ListState::BuiltIn;
int g_listGeneration = 0;            // changes with the list text or the downloaded folders
bool g_foldersDirty = true;          // Shaders\Packs changed: listed again on the render thread
FetchFn g_fetch;

std::wstring Wide(const std::string& s) { return ApexUtil::ToWide(s); }

// ---------------------------------------------------------------------------------------------- WinHTTP
Failure WinHttpFetch(const std::string& url8, uint64_t cap, std::string& body, const Progress& progress)
{
    body.clear();
    HINTERNET session = WinHttpOpen(L"ApexRadiance", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!session) return Failure::Offline;
    DWORD protocols = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
    WinHttpSetOption(session, WINHTTP_OPTION_SECURE_PROTOCOLS, &protocols, sizeof protocols); // Windows 7 without TLS 1.2 support: the default set
    WinHttpSetTimeouts(session, 15000, 15000, 30000, 60000);
    std::wstring url = Wide(url8);
    Failure result = Failure::Failed;
    for (int hop = 0; hop < 6; ++hop)
    {
        if (!AllowedUrl(url)) { result = Failure::Refused; break; }
        wchar_t host[256] = {}, path[2048] = {}, extra[2048] = {};
        URL_COMPONENTS uc{};
        uc.dwStructSize = sizeof uc;
        uc.lpszHostName = host; uc.dwHostNameLength = 255;
        uc.lpszUrlPath = path; uc.dwUrlPathLength = 2047;
        uc.lpszExtraInfo = extra; uc.dwExtraInfoLength = 2047;
        if (!WinHttpCrackUrl(url.c_str(), 0, 0, &uc)) { result = Failure::Refused; break; }
        const std::wstring target = std::wstring(path) + extra;
        HINTERNET connect = WinHttpConnect(session, host, uc.nPort, 0);
        HINTERNET req = connect ? WinHttpOpenRequest(connect, L"GET", target.c_str(), nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE) : nullptr;
        auto close = [&] { if (req) WinHttpCloseHandle(req); if (connect) WinHttpCloseHandle(connect); };
        if (!req) { close(); result = Failure::Offline; break; }
        DWORD noRedirects = WINHTTP_DISABLE_REDIRECTS; // each hop is checked here instead
        WinHttpSetOption(req, WINHTTP_OPTION_DISABLE_FEATURE, &noRedirects, sizeof noRedirects);
        if (!WinHttpSendRequest(req, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) || !WinHttpReceiveResponse(req, nullptr))
        { close(); result = Failure::Offline; break; }
        DWORD status = 0, len = sizeof status;
        WinHttpQueryHeaders(req, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX, &status, &len, WINHTTP_NO_HEADER_INDEX);
        auto header = [&](DWORD what, const wchar_t* name) {
            wchar_t buf[2048] = {};
            DWORD n = sizeof buf - sizeof(wchar_t);
            return WinHttpQueryHeaders(req, what, name, buf, &n, WINHTTP_NO_HEADER_INDEX) ? std::wstring(buf) : std::wstring();
        };
        if (status == 301 || status == 302 || status == 303 || status == 307 || status == 308)
        {
            std::wstring loc = header(WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX);
            close();
            if (loc.empty()) { result = Failure::Failed; break; }
            url = loc[0] == L'/' ? L"https://" + std::wstring(host) + loc : loc;
            continue;
        }
        if (status != 200)
        {
            const bool limited = header(WINHTTP_QUERY_CUSTOM, L"X-RateLimit-Remaining") == L"0" || !header(WINHTTP_QUERY_CUSTOM, L"Retry-After").empty();
            close();
            result = FailureFor((int)status, limited);
            break;
        }
        uint64_t total = 0;
        if (std::wstring cl = header(WINHTTP_QUERY_CONTENT_LENGTH, WINHTTP_HEADER_NAME_BY_INDEX); !cl.empty()) total = _wcstoui64(cl.c_str(), nullptr, 10);
        if (total > cap) { close(); result = Failure::TooLarge; break; }
        result = Failure::None;
        try // out of memory while the body grows: the handles still close
        {
        if (total) body.reserve((size_t)total); // one block, not a series of growing copies
        for (;;)
        {
            DWORD avail = 0;
            if (!WinHttpQueryDataAvailable(req, &avail)) { result = Failure::Offline; break; }
            if (!avail) break;
            const size_t at = body.size();
            if (at + avail > cap) { result = Failure::TooLarge; break; }
            body.resize(at + avail);
            DWORD got = 0;
            if (!WinHttpReadData(req, body.data() + at, avail, &got)) { result = Failure::Offline; break; }
            body.resize(at + got);
            if (progress && !progress(body.size(), total)) { result = Failure::Stopped; break; }
        }
        }
        catch (...) { result = Failure::Failed; }
        close();
        break;
    }
    WinHttpCloseHandle(session);
    if (result != Failure::None) body.clear();
    return result;
}

// ---------------------------------------------------------------------------------------------- unpacking
bool SafePart(const std::string& p)
{
    if (p.empty() || p == "." || p == ".." || p.size() > 120 || p.back() == '.' || p.back() == ' ') return false;
    for (unsigned char c : p)
        if (c < 32 || strchr("<>:\"|?*\\", c)) return false;
    return true;
}

// Deletes a folder inside Shaders\Packs (never outside it)
bool DeleteTree(const std::wstring& dir)
{
    const std::wstring root = PacksFolder();
    if (root.empty() || !Fx::InsideRoot(root, dir)) return false;
    std::error_code ec;
    std::filesystem::remove_all(dir, ec);
    return !ec;
}

// ---------------------------------------------------------------------------------------------- list
struct IniPack { std::string name, author, repo, branch; };

std::vector<IniPack> ParseList(const std::string& text)
{
    std::vector<IniPack> out;
    std::string name, url;
    auto flush = [&] {
        // https://github.com/<owner>/<repo>/archive/[refs/heads/]<branch>.zip
        const std::string pre = "https://github.com/";
        if (!name.empty() && url.rfind(pre, 0) == 0 && url.size() > 4 && url.compare(url.size() - 4, 4, ".zip") == 0)
        {
            const std::string rest = url.substr(pre.size(), url.size() - pre.size() - 4);
            const size_t a = rest.find("/archive/");
            if (a != std::string::npos && std::count(rest.begin(), rest.begin() + (std::ptrdiff_t)a, '/') == 1)
            {
                std::string branch = rest.substr(a + 9);
                if (branch.rfind("refs/heads/", 0) == 0) branch = branch.substr(11);
                const size_t by = name.find(" by ");
                out.push_back({ by == std::string::npos ? name : name.substr(0, by), by == std::string::npos ? "" : name.substr(by + 4), rest.substr(0, a), branch });
            }
        }
        name.clear(); url.clear();
    };
    size_t pos = 0;
    while (pos < text.size() && out.size() < 500)
    {
        size_t e = text.find('\n', pos);
        if (e == std::string::npos) e = text.size();
        std::string line = text.substr(pos, e - pos);
        pos = e + 1;
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (!line.empty() && line[0] == '[') flush();
        else if (line.rfind("PackageName=", 0) == 0) name = line.substr(12);
        else if (line.rfind("DownloadUrl=", 0) == 0) url = line.substr(12);
    }
    flush();
    return out;
}

Pack FromIndex(const IndexPack& x)
{
    Pack p;
    p.name = x.name; p.author = x.author; p.folder = x.folder; p.repo = x.repo; p.branch = x.branch; p.licence = x.licence;
    p.downloadKB = x.downloadKB; p.techniques = x.techniques; p.rank = x.rank; p.index = &x;
    return p;
}

std::string FolderName(const std::string& name)
{
    std::string f;
    for (char c : name)
        if (isalnum((unsigned char)c) || strchr(" ._-", c)) f += c;
    while (!f.empty() && (f.back() == '.' || f.back() == ' ')) f.pop_back();
    return f;
}

// render thread
std::vector<Pack> g_list;
int g_listBuilt = -1;
std::vector<std::string> g_folders; // downloaded, as last listed

void ListFolders()
{
    std::vector<std::string> out;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((PacksFolder() + L"*").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE)
    {
        do
        {
            const std::wstring n = fd.cFileName;
            if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) && n[0] != L'.' && n.find(L'.') == std::wstring::npos)
                out.push_back(ApexUtil::ToUtf8(n));
        } while (out.size() < 200 && FindNextFileW(h, &fd));
        FindClose(h);
    }
    g_folders = std::move(out);
}

// ---------------------------------------------------------------------------------------------- worker
void Worker()
{
    for (;;)
    {
        Row job;
        std::string url;
        uint32_t estimateKB = 0;
        {
            std::lock_guard<std::mutex> lock(g_mu);
            Row* next = nullptr;
            for (Row& r : g_batch.rows) if (r.state == RowState::Waiting) { next = &r; break; }
            if (!next || g_stop) { g_workerRunning = false; g_batch.running = false; g_stop = false; return; }
            next->state = RowState::Downloading;
            job = *next;
        }
        Failure f = Failure::Failed;
        bool placed = false; // the new files are in place (a later exception does not make the pack Failed)
        try // out of memory (a large pack in the 32-bit game) fails this pack, never the worker
        {
        Pack pack;
        const Pack* p = nullptr;
        for (const IndexPack& x : kIndex) if (IEq(x.folder, job.folder)) { pack = FromIndex(x); p = &pack; }
        {
            std::lock_guard<std::mutex> lock(g_mu);
            if (!p) // a pack the list has and the index does not
                for (const IniPack& x : ParseList(g_listText))
                    if (IEq(FolderName(x.name), job.folder)) { pack.repo = x.repo; pack.branch = x.branch; p = &pack; }
        }
        if (p)
        {
            url = "https://codeload.github.com/" + p->repo + "/zip/refs/heads/" + p->branch;
            estimateKB = p->downloadKB;
            std::string zip;
            FetchFn fetch;
            { std::lock_guard<std::mutex> lock(g_mu); fetch = g_fetch; }
            const std::string folder = job.folder;
            auto progress = [&](uint64_t got, uint64_t total) {
                std::lock_guard<std::mutex> lock(g_mu);
                const double t = total ? (double)total : estimateKB ? estimateKB * 1024.0 : 0.0;
                for (Row& r : g_batch.rows)
                    if (r.folder == folder && r.state == RowState::Downloading) r.progress = t > 0 ? (float)std::min(0.99, got / t) : 0.0f;
                return !g_stop;
            };
            f = fetch ? fetch(url, kMaxZipBytes, zip, progress) : WinHttpFetch(url, kMaxZipBytes, zip, progress);
            if (f == Failure::None)
            {
                // into <folder>.part, then in place of the old download in one rename each (the old one stays if anything fails)
                const std::wstring dest = PacksFolder() + Wide(folder);
                std::error_code ec;
                std::filesystem::create_directories(PacksFolder(), ec);
                DeleteTree(dest + L".part");
                Unpacked u;
                f = Unpack(zip.data(), zip.size(), dest + L".part", &u);
                zip.clear(); zip.shrink_to_fit();
                if (f == Failure::None)
                {
                    const bool had = GetFileAttributesW(dest.c_str()) != INVALID_FILE_ATTRIBUTES;
                    DeleteTree(dest + L".old");
                    if ((had && !MoveFileW(dest.c_str(), (dest + L".old").c_str())) || !MoveFileW((dest + L".part").c_str(), dest.c_str()))
                    {
                        if (had && GetFileAttributesW(dest.c_str()) == INVALID_FILE_ATTRIBUTES) MoveFileW((dest + L".old").c_str(), dest.c_str());
                        f = Failure::Disk;
                    }
                    placed = f == Failure::None;
                    DeleteTree(dest + L".old");
                    LOG_INFO("[Looks] pack " + folder + " downloaded: " + std::to_string(u.shaders) + " shader files, " + std::to_string(u.textures) +
                             " textures, " + std::to_string(u.skipped) + " files left out");
                }
                DeleteTree(dest + L".part");
            }
            if (f != Failure::None) LOG_WARNING("[Looks] pack " + folder + ": " + FailureText(f));
        }
        }
        catch (...)
        {
            f = placed ? Failure::None : Failure::Failed;
            try
            {
                DeleteTree(PacksFolder() + Wide(job.folder) + L".part");
                LOG_WARNING("[Looks] pack " + job.folder + ": failed (out of memory)");
            }
            catch (...) {}
        }
        {
            std::lock_guard<std::mutex> lock(g_mu);
            for (Row& r : g_batch.rows)
                if (r.folder == job.folder && r.state == RowState::Downloading)
                {
                    r.state = f == Failure::None ? RowState::Done : RowState::Failed;
                    r.failure = f;
                    r.progress = f == Failure::None ? 1.0f : r.progress;
                }
            if (f == Failure::Gone) g_unavailable.insert(Lower(job.folder));
            g_foldersDirty = true;
            ++g_listGeneration;
        }
        if (f == Failure::None) Fx::RequestScan(false);
    }
}

void StartWorker()
{
    // under g_mu
    if (g_workerRunning) return;
    g_workerRunning = true;
    g_batch.running = true;
    if (!HookGuard::StartDetached("Looks: download shader packs", [] { Worker(); }))
    {
        g_workerRunning = false;
        g_batch.running = false;
        for (Row& r : g_batch.rows) if (r.state == RowState::Waiting) { r.state = RowState::Failed; r.failure = Failure::Failed; }
    }
}

} // namespace

// ============================================================================================== parts
bool AllowedUrl(const std::wstring& url)
{
    const std::wstring pre = L"https://";
    if (url.size() <= pre.size() || _wcsnicmp(url.c_str(), pre.c_str(), pre.size()) != 0) return false;
    const size_t end = url.find_first_of(L"/?#", pre.size());
    const std::wstring host = url.substr(pre.size(), end == std::wstring::npos ? std::wstring::npos : end - pre.size());
    for (const wchar_t* h : { L"github.com", L"codeload.github.com", L"objects.githubusercontent.com", L"raw.githubusercontent.com" })
        if (_wcsicmp(host.c_str(), h) == 0) return true; // a port, user name or other host is refused
    return false;
}

Failure FailureFor(int status, bool rateLimited)
{
    if (status == 404 || status == 410 || status == 451) return Failure::Gone;
    if ((status == 403 || status == 429) && rateLimited) return Failure::Busy;
    if (status == 429 || status == 503) return Failure::Busy;
    return Failure::Failed;
}

const char* FailureText(Failure f)
{
    switch (f)
    {
    case Failure::None: return "";
    case Failure::Offline: return "No connection to GitHub";
    case Failure::Gone: return "This pack is no longer on GitHub";
    case Failure::Busy: return "GitHub is busy; try again in a few minutes";
    case Failure::TooLarge: return "This pack is larger than Apex accepts";
    case Failure::Refused: return "GitHub sent the download somewhere else";
    case Failure::BadZip: return "The pack could not be unpacked";
    case Failure::Disk: return "The pack could not be saved in the Shaders folder";
    case Failure::Stopped: return "Stopped";
    case Failure::Failed: break;
    }
    return "The download failed";
}

Failure Unpack(const void* data, size_t size, const std::wstring& dest, Unpacked* out)
{
    Unpacked u;
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_mem(&zip, data, size, 0)) return Failure::BadZip;
    struct Close { mz_zip_archive& z; ~Close() { mz_zip_reader_end(&z); } } closeZip{ zip };
    const mz_uint n = mz_zip_reader_get_num_files(&zip);
    if (n > kMaxZipEntries) return Failure::TooLarge;
    struct Keep { mz_uint index; std::wstring path; };
    std::vector<Keep> keep;
    uint64_t total = 0;
    for (mz_uint i = 0; i < n; ++i)
    {
        mz_zip_archive_file_stat st;
        if (!mz_zip_reader_file_stat(&zip, i, &st)) return Failure::BadZip;
        if (st.m_is_directory) continue;
        std::string name = st.m_filename;
        std::replace(name.begin(), name.end(), '\\', '/');
        const size_t dot = name.find_last_of('.');
        const std::string ext = dot == std::string::npos ? "" : Lower(name.substr(dot));
        const bool code = ext == ".fx" || ext == ".fxh";
        const bool image = ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" || ext == ".dds";
        if (!code && !image) continue;
        std::vector<std::string> parts;
        for (size_t s = 0; s <= name.size();)
        {
            size_t e = name.find('/', s);
            if (e == std::string::npos) e = name.size();
            parts.push_back(name.substr(s, e - s));
            s = e + 1;
        }
        if (name.empty() || name[0] == '/' || parts.size() < 2) { ++u.skipped; continue; } // a zip of a branch has one top folder
        parts.erase(parts.begin());
        auto after = [&](const char* folder) -> int {
            for (size_t k = 0; k + 1 < parts.size(); ++k) if (IEq(parts[k], folder)) return (int)k + 1;
            return -1;
        };
        int from = code ? after("Shaders") : after("Textures");
        if (image && from < 0) from = after("Shaders"); // images next to the shaders
        if (code && from < 0) from = 0;                  // .fx outside a Shaders folder: as the repository has it
        if (from < 0) { ++u.skipped; continue; }         // pictures of the readme and the like
        std::wstring rel;
        bool ok = (int)parts.size() - from <= 4;
        for (size_t k = (size_t)from; k < parts.size() && ok; ++k)
        {
            ok = SafePart(parts[k]);
            rel += (rel.empty() ? L"" : L"\\") + Wide(parts[k]);
        }
        if (!ok || rel.size() > 200 || st.m_uncomp_size > (code ? Fx::kMaxFxBytes : Fx::kMaxTextureFileBytes)) { ++u.skipped; continue; }
        total += st.m_uncomp_size;
        if (keep.size() >= kMaxFiles || total > kMaxExtractedBytes) return Failure::TooLarge;
        keep.push_back({ i, (code ? L"Shaders\\" : L"Textures\\") + rel });
        ++(code ? u.shaders : u.textures);
    }
    std::error_code ec;
    for (const Keep& k : keep)
    {
        size_t got = 0;
        void* bytes = mz_zip_reader_extract_to_heap(&zip, k.index, &got, 0); // checks the CRC
        if (!bytes) { DeleteTree(dest); return Failure::BadZip; }
        const std::wstring path = dest + L"\\" + k.path;
        std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
        HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        DWORD put = 0;
        const bool wrote = h != INVALID_HANDLE_VALUE && (got == 0 || (WriteFile(h, bytes, (DWORD)got, &put, nullptr) && put == got));
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        mz_free(bytes);
        if (!wrote) { DeleteTree(dest); return Failure::Disk; }
    }
    if (keep.empty()) std::filesystem::create_directories(dest + L"\\Shaders", ec); // still a pack (shown as downloaded)
    if (out) *out = u;
    return Failure::None;
}

// ============================================================================================== list
std::vector<Pack> MergeList(const std::string& ini, const std::vector<std::string>& downloaded)
{
    std::vector<Pack> out;
    if (ini.empty())
        for (const IndexPack& x : kIndex) out.push_back(FromIndex(x));
    else
    {
        int rank = 0;
        for (const IniPack& x : ParseList(ini))
        {
            const IndexPack* hit = nullptr;
            int sameRepo = 0;
            for (const IndexPack& k : kIndex) sameRepo += IEq(k.repo, x.repo);
            for (const IndexPack& k : kIndex)
                if (IEq(k.repo, x.repo) && (sameRepo == 1 || IEq(k.branch, x.branch))) hit = &k;
            Pack p;
            if (hit) p = FromIndex(*hit);
            else
            {
                p.name = x.name; p.author = x.author; p.repo = x.repo; p.branch = x.branch; p.folder = FolderName(x.name);
                p.licenceKnown = false;
            }
            p.rank = rank++;
            if (p.folder.empty() || std::any_of(out.begin(), out.end(), [&](const Pack& q) { return IEq(q.folder, p.folder); })) continue;
            out.push_back(std::move(p));
        }
    }
    for (const std::string& f : downloaded)
    {
        if (std::any_of(out.begin(), out.end(), [&](const Pack& q) { return IEq(q.folder, f); })) continue;
        Pack p;
        for (const IndexPack& x : kIndex) if (IEq(x.folder, f)) p = FromIndex(x);
        if (p.folder.empty()) { p.name = p.folder = f; p.licenceKnown = false; }
        p.inList = false;
        p.rank = 1000;
        out.push_back(std::move(p));
    }
    std::sort(out.begin(), out.end(), [](const Pack& a, const Pack& b) { return Lower(a.name) < Lower(b.name); });
    return out;
}

std::wstring PacksFolder()
{
    const std::wstring s = Fx::ShadersFolder();
    return s.empty() ? s : s + L"Packs\\";
}

std::string GitHubUrl(const Pack& p) { return "https://github.com/" + p.repo + (p.branch.empty() ? "" : "/tree/" + p.branch); }

// The list a frame sees: rebuilt only here, at the start of a frame (OnPresent), so a Pack* or a loop over List() stays
// valid for the whole menu frame (a download finishing on another thread used to rebuild it under a caller)
const std::vector<Pack>& List()
{
    if (g_listBuilt < 0) BeginFrame(); // first use: the built-in list
    return g_list;
}

void BeginFrame()
{
    std::string text;
    int gen;
    bool dirty;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        text = g_listText; gen = g_listGeneration; dirty = g_foldersDirty;
        g_foldersDirty = false;
    }
    if (dirty) ListFolders();
    if (dirty || gen != g_listBuilt)
    {
        g_listBuilt = gen;
        g_list = MergeList(text, g_folders);
    }
}

void RefreshList()
{
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_foldersDirty = true;
        if (g_listState != ListState::BuiltIn) return; // once a session
        g_listState = ListState::Reading;
    }
    if (!HookGuard::StartDetached("Looks: read ReShade's pack list", [] {
        FetchFn fetch;
        { std::lock_guard<std::mutex> lock(g_mu); fetch = g_fetch; }
        std::string body;
        const std::string url = "https://raw.githubusercontent.com/crosire/reshade-shaders/list/EffectPackages.ini";
        const Failure f = fetch ? fetch(url, 1u << 20, body, nullptr) : WinHttpFetch(url, 1u << 20, body, nullptr);
        const bool good = f == Failure::None && !ParseList(body).empty();
        if (!good) LOG_INFO(std::string("[Looks] ReShade's pack list not read: ") + (f == Failure::None ? "no pack in it" : FailureText(f)));
        SetListText(good ? body : std::string(), !good);
    }))
    {
        std::lock_guard<std::mutex> lock(g_mu);
        g_listState = ListState::Offline;
    }
}

void SetListText(const std::string& text, bool offline)
{
    std::lock_guard<std::mutex> lock(g_mu);
    if (!offline) g_listText = text;
    g_listState = offline ? ListState::Offline : ListState::Read;
    ++g_listGeneration;
}

ListState CurrentListState()
{
    std::lock_guard<std::mutex> lock(g_mu);
    return g_listState;
}

const Pack* Find(const std::string& folder)
{
    for (const Pack& p : List()) if (IEq(p.folder, folder)) return &p;
    return nullptr;
}

const Pack* PackForFile(const std::string& file, const std::string& technique)
{
    const Pack* best = nullptr;
    int bestScore = -1;
    for (const Pack& p : List())
    {
        if (!p.index || !p.inList || Unavailable(p.folder)) continue;
        for (uint16_t i = 0; i < p.index->fileCount; ++i)
        {
            const IndexFile& f = p.index->files[i];
            if (!IEq(f.file, file)) continue;
            bool hasTech = false;
            const std::string t = std::string(",") + f.techniques + ",";
            hasTech = !technique.empty() && t.find("," + technique + ",") != std::string::npos;
            const int score = (hasTech ? 100000 : 0) + (10000 - p.rank); // the technique, then the earliest on ReShade's list
            if (score > bestScore) { bestScore = score; best = &p; }
        }
    }
    return best;
}

std::string FileForTechnique(const std::string& file, const std::string& technique)
{
    if (technique.empty()) return {};
    std::string word; // the original name's first run of letters and digits, lower case ("mxao" of "MXAO 4.0.2 EX.fx")
    for (char c : file)
    {
        if (isalnum((unsigned char)c)) word += (char)tolower((unsigned char)c);
        else if (!word.empty()) break;
    }
    std::string best;
    int bestScore = -1;
    for (const Pack& p : List())
    {
        if (!p.index || Unavailable(p.folder)) continue;
        for (uint16_t i = 0; i < p.index->fileCount; ++i)
        {
            const IndexFile& f = p.index->files[i];
            if (IEq(f.file, file)) return {}; // a pack has the file itself: PackForFile finds it
            if ((std::string(",") + f.techniques + ",").find("," + technique + ",") == std::string::npos) continue;
            const bool similar = word.size() >= 3 && Lower(f.file).find(word) != std::string::npos;
            const int score = (Downloaded(p.folder) ? 1000000 : 0) + (similar ? 100000 : 0) + (p.inList ? 10000 - p.rank : 0);
            if (score > bestScore) { bestScore = score; best = f.file; }
        }
    }
    return best;
}

// ============================================================================================== downloads
void Download(const std::vector<std::string>& folders)
{
    RefreshList(); // ReShade's list, read once a session when the player first downloads
    std::vector<std::string> want = folders;
    const char* standard = "ReShade standard effects";
    if (!want.empty() && !Downloaded(standard) && std::none_of(want.begin(), want.end(), [&](const std::string& f) { return IEq(f, standard); }))
        want.insert(want.begin(), standard);
    std::lock_guard<std::mutex> lock(g_mu);
    if (!g_batch.running)
        g_batch.rows.erase(std::remove_if(g_batch.rows.begin(), g_batch.rows.end(), [](const Row& r) { return r.state == RowState::Done || r.state == RowState::Failed; }), g_batch.rows.end());
    for (const std::string& f : want)
    {
        if (std::any_of(g_batch.rows.begin(), g_batch.rows.end(), [&](const Row& r) { return IEq(r.folder, f) && (r.state == RowState::Waiting || r.state == RowState::Downloading); })) continue;
        g_batch.rows.erase(std::remove_if(g_batch.rows.begin(), g_batch.rows.end(), [&](const Row& r) { return IEq(r.folder, f); }), g_batch.rows.end());
        Row r;
        r.folder = f;
        g_batch.rows.push_back(r);
    }
    g_stop = false;
    if (!g_batch.rows.empty()) StartWorker();
}

void StopRest()
{
    std::lock_guard<std::mutex> lock(g_mu);
    if (!g_batch.running) return;
    g_stop = true;
    for (Row& r : g_batch.rows)
        if (r.state == RowState::Waiting) { r.state = RowState::Failed; r.failure = Failure::Stopped; }
}

Batch Status()
{
    std::lock_guard<std::mutex> lock(g_mu);
    return g_batch;
}

void ClearFinished()
{
    std::lock_guard<std::mutex> lock(g_mu);
    if (!g_batch.running) g_batch.rows.clear();
}

bool Unavailable(const std::string& folder)
{
    std::lock_guard<std::mutex> lock(g_mu);
    return g_unavailable.count(Lower(folder)) != 0;
}

bool Downloaded(const std::string& folder)
{
    for (const std::string& f : g_folders) if (IEq(f, folder)) return true;
    return false;
}

void Remove(const std::string& folder)
{
    const std::wstring dir = PacksFolder() + Wide(folder);
    HookGuard::StartDetached("Looks: remove a shader pack", [dir, folder] {
        const bool ok = DeleteTree(dir);
        LOG_INFO("[Looks] pack " + folder + (ok ? " removed" : " could not be removed"));
        {
            std::lock_guard<std::mutex> lock(g_mu);
            g_foldersDirty = true;
            ++g_listGeneration;
        }
        Fx::RequestScan(false);
    });
}

void SetFetch(FetchFn f)
{
    std::lock_guard<std::mutex> lock(g_mu);
    g_fetch = std::move(f);
}

void WaitIdle()
{
    for (int i = 0; i < 6000; ++i)
    {
        {
            std::lock_guard<std::mutex> lock(g_mu);
            if (!g_workerRunning && g_listState != ListState::Reading) return;
        }
        Sleep(10);
    }
}

} // namespace Looks::Packs
