// Looks: compile and run the player's own ReShade FX effects (see looks_fx.h). The GPU side mirrors ReShade 6.7.3's
// Direct3D 9 runtime for one technique: COLOR is copied from the picture at the technique's start and after any pass that
// wrote the picture; render targets are cleared to zero when created; stencil only for full-size passes, cleared at first
// use; auto-generated mips; vertex ids in a float TEXCOORD0 stream; __TEXEL_SIZE__ at VS c255; uniforms in PS and VS
// constants from c0.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <wincodec.h>
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "ole32.lib")
#include "looks_fx.h"
#include "looks_preset.h" // ParseDefines (a ReShade.ini's definitions)
#include "third_party/reshadefx/effect_parser.hpp"
#include "third_party/reshadefx/effect_codegen.hpp"
#include "third_party/reshadefx/effect_preprocessor.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <functional>
#include <map>
#include <d3dcompiler.h>
// ReShade resizes texture images with stb_image_resize2 (public domain, the commit ReShade 6.7.3 pins): the same code here
#define STB_IMAGE_RESIZE_IMPLEMENTATION
#include "third_party/stb/stb_image_resize2.h"
#pragma comment(lib, "d3dcompiler.lib")

namespace Looks::Fx {

const Ann* FindAnn(const std::vector<Ann>& as, const char* name)
{
    for (const Ann& a : as) if (a.name == name) return &a;
    return nullptr;
}

// ============================================================================================== files
namespace {

std::wstring Lower(std::wstring s)
{
    for (wchar_t& c : s) c = (wchar_t)towlower(c);
    return s;
}

std::wstring FullPath(const std::wstring& p)
{
    wchar_t buf[MAX_PATH * 2];
    const DWORD n = GetFullPathNameW(p.c_str(), (DWORD)std::size(buf), buf, nullptr);
    return n && n < std::size(buf) ? std::wstring(buf, n) : std::wstring();
}

bool FileSize(const std::wstring& path, uint64_t& size)
{
    WIN32_FILE_ATTRIBUTE_DATA a{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &a) || (a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return false;
    size = ((uint64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow;
    return true;
}

bool ReadCapped(const std::wstring& path, size_t cap, std::string& out)
{
    uint64_t size = 0;
    if (!FileSize(path, size) || size > cap) return false;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    out.resize((size_t)size);
    DWORD got = 0;
    const bool ok = size == 0 || (ReadFile(h, out.data(), (DWORD)size, &got, nullptr) && got == size);
    CloseHandle(h);
    return ok;
}

std::string Utf8(const std::wstring& w)
{
    if (w.empty()) return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n > 0 ? n : 0, '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}

std::wstring Wide(const std::string& s)
{
    if (s.empty()) return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n > 0 ? n : 0, L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

// Finds a texture file by name under the texture folders (and up to three levels of subfolders), inside root
std::wstring FindTextureFile(const Folders& f, const std::string& name)
{
    std::wstring n = Wide(name);
    std::replace(n.begin(), n.end(), L'/', L'\\');
    if (n.empty() || n.find(L':') != std::wstring::npos || n[0] == L'\\') return {};
    if (n.find(L"..") != std::wstring::npos) return {};
    std::function<std::wstring(const std::wstring&, int)> search = [&](const std::wstring& dir, int depth) -> std::wstring {
        uint64_t size = 0;
        const std::wstring direct = dir + n;
        if (FileSize(direct, size) && InsideRoot(f.root, FullPath(direct))) return FullPath(direct);
        if (depth == 0) return {};
        WIN32_FIND_DATAW fd;
        HANDLE h = FindFirstFileW((dir + L"*").c_str(), &fd);
        if (h == INVALID_HANDLE_VALUE) return {};
        std::wstring found;
        int dirs = 0;
        do
        {
            if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) || (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) || fd.cFileName[0] == L'.') continue;
            if (++dirs > 200) break;
            found = search(dir + fd.cFileName + L"\\", depth - 1);
        } while (found.empty() && FindNextFileW(h, &fd));
        FindClose(h);
        return found;
    };
    for (const std::wstring& d : f.textureDirs)
        if (std::wstring p = search(d, 3); !p.empty()) return p;
    return {};
}

struct IncludeContext
{
    std::wstring root;
    size_t count = 0;
};

bool IncludeFilter(void* context, const std::filesystem::path& path)
{
    IncludeContext& c = *static_cast<IncludeContext*>(context);
    if (++c.count > kMaxIncludes) return false;
    const std::wstring full = FullPath(path.wstring());
    if (path.wstring().find(L"..") != std::wstring::npos || !InsideRoot(c.root, full)) return false;
    uint64_t size = 0;
    return !FileSize(full, size) || size <= kMaxFxBytes; // a missing file is the preprocessor's own error
}

// Short, path-free reason from compiler output: the first error line, without the folder and the error code
std::string ReasonFrom(const std::string& text, const std::wstring& root)
{
    std::string line;
    for (size_t pos = 0; pos < text.size();)
    {
        size_t e = text.find('\n', pos);
        if (e == std::string::npos) e = text.size();
        std::string l = text.substr(pos, e - pos);
        pos = e + 1;
        if (l.find("error") != std::string::npos) { line = l; break; }
        if (line.empty()) line = l;
    }
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
    const std::string r = Utf8(root);
    for (size_t p; !r.empty() && (p = line.find(r)) != std::string::npos;) line.erase(p, r.size());
    const size_t err = line.find("error X");
    if (err != std::string::npos)
    {
        const std::string code = line.substr(err + 6, 5);
        const size_t colon = line.find(": ", err);
        const std::string msg = colon == std::string::npos ? line.substr(err) : line.substr(colon + 2);
        if (code == "X5608" || code == "X5609") return "too many instructions for shader model 3.0";
        if (code == "X4505" || code == "X5589") return "uses more registers than shader model 3.0 has";
        if (code == "X3511") return "has a loop shader model 3.0 cannot unroll";
        if (code == "X4014" || code == "X4121") return "has a loop shader model 3.0 cannot run (gradients in a loop)";
        if (code == "X3004" || code == "X3013" || code == "X3018") return "the shader code does not compile: " + msg;
        line = msg;
    }
    if (line.size() > 200) line = line.substr(0, 200) + "...";
    return line;
}

std::string TrimA(std::string s)
{
    const size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

bool AsInt(const std::string& s, int& v)
{
    char* end = nullptr;
    const long x = strtol(s.c_str(), &end, 10);
    if (s.empty() || *end || x < -1000000 || x > 1000000) return false;
    v = (int)x;
    return true;
}

bool IdChar(char c) { return isalnum((unsigned char)c) || c == '_'; }

// The definitions an effect tests, as ReShade 6 lists them under "Preprocessor definitions": not ReShade's own (a leading
// '_', BUFFER_*, RESHADE_*), not include guards (no value). def and lo..hi come from the #define and #if / #elif lines of
// the file and its includes.
std::vector<Macro> UsedMacros(const reshadefx::preprocessor& pp, const std::wstring& fxPath, const LooksDefines& defines)
{
    std::vector<Macro> out;
    for (const auto& [name, value] : pp.used_macro_definitions())
    {
        if (name.empty() || name[0] == '_' || !name.compare(0, 7, "BUFFER_") || !name.compare(0, 8, "RESHADE_")) continue;
        std::string v = TrimA(value);
        if (!v.empty() && out.size() < 128) out.push_back({ name, v, v });
    }
    if (out.empty()) return out;
    std::sort(out.begin(), out.end(), [](const Macro& a, const Macro& b) { return a.name < b.name; });
    auto find = [&](const std::string& n) -> Macro* {
        auto it = std::lower_bound(out.begin(), out.end(), n, [](const Macro& m, const std::string& k) { return m.name < k; });
        return it != out.end() && it->name == n ? &*it : nullptr;
    };
    std::vector<std::string> overridden; // their .fx default is the first #define of the name
    for (const auto& d : defines)
        if (Macro* m = find(d.first)) { overridden.push_back(d.first); m->def.clear(); } // "" = the .fx does not define it
    std::vector<std::wstring> files{ fxPath };
    for (const auto& p : pp.included_files()) files.push_back(p.wstring());
    std::vector<bool> seen(out.size(), false), compared(out.size(), false);
    std::string text;
    for (const std::wstring& f : files)
    {
        if (!ReadCapped(f, kMaxFxBytes, text)) continue;
        for (size_t pos = 0; pos < text.size();)
        {
            size_t e = text.find('\n', pos);
            if (e == std::string::npos) e = text.size();
            std::string line = TrimA(text.substr(pos, e - pos));
            pos = e + 1;
            if (line.empty() || line[0] != '#') continue;
            if (const size_t c = line.find("//"); c != std::string::npos) line.resize(c);
            size_t at = line.find_first_not_of(" \t", 1);
            if (at == std::string::npos) continue;
            size_t wordEnd = at;
            while (wordEnd < line.size() && IdChar(line[wordEnd])) ++wordEnd;
            const std::string word = line.substr(at, wordEnd - at);
            if (word == "define")
            {
                const size_t n0 = line.find_first_not_of(" \t", wordEnd);
                size_t n1 = n0;
                while (n1 != std::string::npos && n1 < line.size() && IdChar(line[n1])) ++n1;
                if (n0 == std::string::npos || n1 == n0 || (n1 < line.size() && line[n1] == '(')) continue;
                const std::string name = line.substr(n0, n1 - n0);
                Macro* m = find(name);
                if (!m || seen[m - out.data()]) continue;
                seen[m - out.data()] = true;
                if (std::find(overridden.begin(), overridden.end(), name) != overridden.end()) m->def = TrimA(line.substr(n1));
            }
            else if (word == "if" || word == "elif")
            {
                // NAME op integer: every integer it is compared with (a cheap look at the common "#if NAME == 2")
                for (size_t i = wordEnd; i < line.size();)
                {
                    if (!IdChar(line[i]) || (i > 0 && IdChar(line[i - 1]))) { ++i; continue; }
                    size_t j = i;
                    while (j < line.size() && IdChar(line[j])) ++j;
                    Macro* m = find(line.substr(i, j - i));
                    i = j;
                    if (!m) continue;
                    size_t k = line.find_first_not_of(" \t", j);
                    if (k == std::string::npos || !strchr("=!<>", line[k])) continue;
                    while (k < line.size() && strchr("=!<>", line[k])) ++k;
                    k = line.find_first_not_of(" \t", k);
                    if (k == std::string::npos) continue;
                    size_t l = k;
                    while (l < line.size() && (isdigit((unsigned char)line[l]) || (l == k && line[l] == '-'))) ++l;
                    int v = 0;
                    if (!AsInt(line.substr(k, l - k), v)) continue;
                    const size_t idx = m - out.data();
                    m->lo = compared[idx] ? std::min(m->lo, v) : v;
                    m->hi = compared[idx] ? std::max(m->hi, v) : v;
                    compared[idx] = true;
                }
            }
        }
    }
    for (Macro& m : out)
    {
        int v = 0;
        if (m.hi >= m.lo && AsInt(m.def, v)) { m.lo = std::min(m.lo, v); m.hi = std::max(m.hi, v); }
    }
    return out;
}

std::vector<float> Floats(const reshadefx::type& t, const reshadefx::constant& c, unsigned n)
{
    std::vector<float> v(n, 0.0f);
    for (unsigned i = 0; i < n && i < 16; ++i)
    {
        switch (t.base)
        {
        case reshadefx::type::t_float: case reshadefx::type::t_min16float: v[i] = c.as_float[i]; break;
        case reshadefx::type::t_int: case reshadefx::type::t_min16int: v[i] = (float)c.as_int[i]; break;
        default: v[i] = (float)c.as_uint[i]; break;
        }
    }
    return v;
}

std::vector<Ann> Anns(const std::vector<reshadefx::annotation>& in)
{
    std::vector<Ann> out;
    for (const auto& a : in)
    {
        Ann x;
        x.name = a.name;
        x.isString = a.type.base == reshadefx::type::t_string;
        if (x.isString) x.s = a.value.string_data;
        else { auto f = Floats(a.type, a.value, std::min(4u, a.type.components())); std::copy(f.begin(), f.end(), x.f); }
        out.push_back(std::move(x));
    }
    return out;
}

// reshadefx texture format -> Direct3D 9. Formats D3D9 does not have are lowered to the nearest one that holds the same
// values (*lowered names the fallback, for the log and tools/looks_fx_check). Shader model 3.0 reads every texture as
// float, so integer textures become 32-bit float ones (exact up to 2^24).
D3DFORMAT Format(reshadefx::texture_format f, const char** lowered)
{
    using tf = reshadefx::texture_format;
    *lowered = "";
    switch (f)
    {
    case tf::unknown: case tf::rgba8: return D3DFMT_A8R8G8B8;
    case tf::r8: return D3DFMT_L8;
    case tf::r16: return D3DFMT_L16;
    case tf::r16f: return D3DFMT_R16F;
    case tf::r32f: return D3DFMT_R32F;
    case tf::rg8: *lowered = "RG8 as RGBA8"; return D3DFMT_A8R8G8B8; // no two-channel 8-bit render target format
    case tf::rg16: return D3DFMT_G16R16;
    case tf::rg16f: return D3DFMT_G16R16F;
    case tf::rg32f: return D3DFMT_G32R32F;
    case tf::rgba16: return D3DFMT_A16B16G16R16;
    case tf::rgba16f: return D3DFMT_A16B16G16R16F;
    case tf::rgba32f: return D3DFMT_A32B32G32R32F;
    case tf::rgb10a2: return D3DFMT_A2B10G10R10;
    case tf::rg11b10f: *lowered = "RG11B10F as RGBA16F"; return D3DFMT_A16B16G16R16F;
    case tf::r32i: *lowered = "R32I as R32F"; return D3DFMT_R32F;
    case tf::r32u: *lowered = "R32U as R32F"; return D3DFMT_R32F;
    case tf::rgba32i: *lowered = "RGBA32I as RGBA32F"; return D3DFMT_A32B32G32R32F;
    case tf::rgba32u: *lowered = "RGBA32U as RGBA32F"; return D3DFMT_A32B32G32R32F;
    }
    return D3DFMT_A8R8G8B8;
}

// D3DCompile of one entry point's shader model 3.0 HLSL. ReShade compiles with optimisation level 1 (level 3 only in its
// performance mode, which Apex does not have: __RESHADE_PERFORMANCE_MODE__ is 0), so level 1 comes first and gives
// ReShade's code; when that fails here the other flag sets D3D9 offers are tried in turn: level 3, flow control kept
// (loops it cannot unroll, fewer instruction slots), flow control avoided (gradients inside loops), then no optimisation.
// *tried is the set that worked (0 = the first), or -1.
// The global PreprocessorDefinitions of a ReShade.ini (else GShade.ini) beside the game's exe ([GENERAL]), read once
// (a player coming from ReShade keeps the definitions set there); empty when there is none
const LooksDefines& GlobalDefines()
{
    static const LooksDefines defs = [] {
        wchar_t exe[MAX_PATH] = {};
        const DWORD n = GetModuleFileNameW(nullptr, exe, MAX_PATH);
        std::wstring dir(exe, n);
        dir.resize(dir.find_last_of(L"\\/") == std::wstring::npos ? 0 : dir.find_last_of(L"\\/") + 1);
        for (const wchar_t* name : { L"ReShade.ini", L"GShade.ini" })
        {
            std::string text;
            if (dir.empty() || !ReadCapped(dir + name, 1u << 20, text)) continue;
            bool general = false;
            for (size_t pos = 0; pos < text.size();)
            {
                size_t e = text.find('\n', pos); if (e == std::string::npos) e = text.size();
                std::string line = text.substr(pos, e - pos);
                pos = e + 1;
                while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
                if (!line.empty() && line.front() == '[') { general = _stricmp(line.c_str(), "[GENERAL]") == 0; continue; }
                if (general && line.rfind("PreprocessorDefinitions=", 0) == 0) return Looks::ParseDefines(line.substr(24));
            }
        }
        return LooksDefines{};
    }();
    return defs;
}

struct Try { UINT flags; const char* name; };
const Try kTries[] = {
    { D3DCOMPILE_OPTIMIZATION_LEVEL1, "" },
    { D3DCOMPILE_OPTIMIZATION_LEVEL3, "optimisation level 3" },
    { D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_PREFER_FLOW_CONTROL, "flow control kept" },
    { D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_AVOID_FLOW_CONTROL, "flow control avoided" },
    { D3DCOMPILE_SKIP_OPTIMIZATION, "no optimisation" },
};
bool CompileSm3(const std::string& hlsl, const std::string& entry, bool vs, std::vector<uint8_t>& code, std::string& errors, int* tried)
{
    *tried = -1;
    for (const Try& t : kTries)
    {
        ID3DBlob* blob = nullptr; ID3DBlob* err = nullptr;
        const HRESULT hr = D3DCompile(hlsl.data(), hlsl.size(), nullptr, nullptr, nullptr, entry.c_str(), vs ? "vs_3_0" : "ps_3_0", t.flags, 0, &blob, &err);
        if (err)
        {
            if (&t == kTries) errors.assign((const char*)err->GetBufferPointer(), err->GetBufferSize());
            err->Release();
        }
        if (SUCCEEDED(hr) && blob && blob->GetBufferSize() >= 8)
        {
            code.assign((const uint8_t*)blob->GetBufferPointer(), (const uint8_t*)blob->GetBufferPointer() + blob->GetBufferSize());
            blob->Release();
            *tried = (int)(&t - kTries);
            return true;
        }
        if (blob) blob->Release();
    }
    return false;
}

// ---- the compile cache (Folders::cacheDir): D3DCompile's results for one effect, keyed by every entry point's generated
// HLSL (the preprocessed source with its includes and macros, BUFFER_WIDTH / BUFFER_HEIGHT among them), its target and
// D3DCompiler's version, so a changed .fx, .fxh or picture size is compiled again. One file per effect and picture size.
constexpr char kCacheMagic[8] = { 'A', 'P', 'X', 'F', 'X', 'C', '2', 0 }; // 2: level 1 first (ReShade's flags), the 4.0.4 macros

uint64_t Fnv(uint64_t h, const void* data, size_t n)
{
    const uint8_t* p = (const uint8_t*)data;
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

struct CachedShader { int tried = -1; std::vector<uint8_t> code; std::string errors; };

// One file per effect, picture size and set of definitions (going back to definitions used before reads their file)
std::wstring CacheFile(const std::wstring& dir, const std::wstring& fxPath, UINT W, UINT H, const LooksDefines& defines)
{
    const std::wstring l = Lower(FullPath(fxPath));
    const uint64_t h = Fnv(14695981039346656037ull, l.data(), l.size() * sizeof(wchar_t));
    uint64_t d = 14695981039346656037ull;
    for (const auto& [name, value] : defines) { d = Fnv(d, name.c_str(), name.size() + 1); d = Fnv(d, value.c_str(), value.size() + 1); }
    wchar_t name[80];
    if (defines.empty()) swprintf_s(name, L"fx_%016llx_%ux%u.bin", (unsigned long long)h, W, H);
    else swprintf_s(name, L"fx_%016llx_%ux%u_%08x.bin", (unsigned long long)h, W, H, (unsigned)(d ^ (d >> 32)));
    return dir + name;
}

bool ReadCache(const std::wstring& file, uint64_t key, size_t count, std::vector<CachedShader>& out)
{
    std::string b;
    if (!ReadCapped(file, 64u << 20, b) || b.size() < 20 || memcmp(b.data(), kCacheMagic, 8) != 0) return false;
    size_t at = 8;
    auto get = [&](void* dst, size_t n) { if (b.size() - at < n) return false; memcpy(dst, b.data() + at, n); at += n; return true; };
    uint64_t k = 0; uint32_t n = 0;
    if (!get(&k, 8) || !get(&n, 4) || k != key || n != count) return false;
    out.assign(count, {});
    for (CachedShader& c : out)
    {
        int32_t tried = -1; uint32_t len = 0;
        if (!get(&tried, 4) || !get(&len, 4) || b.size() - at < len) return false;
        c.tried = tried;
        c.code.assign((const uint8_t*)b.data() + at, (const uint8_t*)b.data() + at + len);
        at += len;
        if (!get(&len, 4) || b.size() - at < len) return false;
        c.errors.assign(b.data() + at, len);
        at += len;
    }
    return at == b.size();
}

void WriteCache(const std::wstring& dir, const std::wstring& file, uint64_t key, const std::vector<CachedShader>& in)
{
    std::string b(kCacheMagic, 8);
    auto put = [&](const void* p, size_t n) { b.append((const char*)p, n); };
    const uint32_t n = (uint32_t)in.size();
    put(&key, 8); put(&n, 4);
    for (const CachedShader& c : in)
    {
        const int32_t tried = c.tried; const uint32_t code = (uint32_t)c.code.size(), err = (uint32_t)c.errors.size();
        put(&tried, 4); put(&code, 4); put(c.code.data(), code); put(&err, 4); put(c.errors.data(), err);
    }
    CreateDirectoryW(dir.c_str(), nullptr);
    const std::wstring tmp = file + L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    DWORD wrote = 0;
    const bool ok = WriteFile(h, b.data(), (DWORD)b.size(), &wrote, nullptr) && wrote == b.size();
    CloseHandle(h);
    if (!ok || !MoveFileExW(tmp.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING)) DeleteFileW(tmp.c_str());
}

std::atomic<int> g_cacheHits{ 0 }, g_cacheMisses{ 0 };
std::atomic<uint32_t> g_vendor{ 0 }, g_device{ 0 };

uint32_t BytesPerPixel(D3DFORMAT f)
{
    switch (f)
    {
    case D3DFMT_L8: return 1;
    case D3DFMT_L16: case D3DFMT_R16F: return 2;
    case D3DFMT_A16B16G16R16: case D3DFMT_A16B16G16R16F: case D3DFMT_G32R32F: return 8;
    case D3DFMT_A32B32G32R32F: return 16;
    default: return 4;
    }
}

uint32_t Blend(reshadefx::blend_factor f)
{
    using bf = reshadefx::blend_factor;
    switch (f)
    {
    case bf::zero: return D3DBLEND_ZERO;
    case bf::one: return D3DBLEND_ONE;
    case bf::source_color: return D3DBLEND_SRCCOLOR;
    case bf::one_minus_source_color: return D3DBLEND_INVSRCCOLOR;
    case bf::dest_color: return D3DBLEND_DESTCOLOR;
    case bf::one_minus_dest_color: return D3DBLEND_INVDESTCOLOR;
    case bf::source_alpha: return D3DBLEND_SRCALPHA;
    case bf::one_minus_source_alpha: return D3DBLEND_INVSRCALPHA;
    case bf::dest_alpha: return D3DBLEND_DESTALPHA;
    case bf::one_minus_dest_alpha: return D3DBLEND_INVDESTALPHA;
    }
    return D3DBLEND_ONE;
}

uint32_t StencilOp(reshadefx::stencil_op o)
{
    using so = reshadefx::stencil_op;
    switch (o)
    {
    case so::zero: return D3DSTENCILOP_ZERO;
    case so::keep: return D3DSTENCILOP_KEEP;
    case so::replace: return D3DSTENCILOP_REPLACE;
    case so::increment_saturate: return D3DSTENCILOP_INCRSAT;
    case so::decrement_saturate: return D3DSTENCILOP_DECRSAT;
    case so::invert: return D3DSTENCILOP_INVERT;
    case so::increment: return D3DSTENCILOP_INCR;
    case so::decrement: return D3DSTENCILOP_DECR;
    }
    return D3DSTENCILOP_KEEP;
}

uint32_t Filter(int bits) { return bits ? D3DTEXF_LINEAR : D3DTEXF_POINT; }

bool CompileImpl(const std::wstring& fxPath, const Folders& folders, UINT W, UINT H, Effect& out)
{
    out = Effect{};
    out.path = fxPath;
    out.file = Utf8(fxPath.substr(fxPath.find_last_of(L"\\/") + 1));
    out.width = W; out.height = H;
    auto fail = [&](std::string why) { out.error = std::move(why); return false; };
    if (!InsideRoot(folders.root, FullPath(fxPath))) return fail("the file is outside the Shaders folder");

    std::string source;
    uint64_t size = 0;
    if (!FileSize(fxPath, size)) return fail("the file can't be read");
    if (size > kMaxFxBytes) return fail("the file is larger than 2 MB");
    if (!ReadCapped(fxPath, kMaxFxBytes, source)) return fail("the file can't be read");
    if (source.size() >= 3 && (unsigned char)source[0] == 0xEF && (unsigned char)source[1] == 0xBB && (unsigned char)source[2] == 0xBF) source.erase(0, 3);
    source += '\n';

    // ---- preprocess, with the macros ReShade 6.7.3 defines on Direct3D 9
    reshadefx::preprocessor pp;
    IncludeContext inc{ folders.root };
    pp.include_filter = IncludeFilter;
    pp.include_filter_context = &inc;
    pp.max_output_size = kMaxPreprocessedBytes;
    pp.max_tokens = 20u << 20; // a large real effect reads well under a million
    pp.add_macro_definition("__RESHADE__", "60703");
    pp.add_macro_definition("__RESHADE_PERMUTATION__", "0");
    pp.add_macro_definition("__RESHADE_PERFORMANCE_MODE__", "0");
    pp.add_macro_definition("__VENDOR__", std::to_string(g_vendor.load()));
    pp.add_macro_definition("__DEVICE__", std::to_string(g_device.load()));
    pp.add_macro_definition("__RENDERER__", "36864"); // 0x9000: Direct3D 9
    pp.add_macro_definition("__APPLICATION__", std::to_string(std::hash<std::string>()("TS3W") & 0xFFFFFFFF));
    pp.add_macro_definition("BUFFER_WIDTH", std::to_string(W));
    pp.add_macro_definition("BUFFER_HEIGHT", std::to_string(H));
    pp.add_macro_definition("BUFFER_RCP_WIDTH", "(1.0 / BUFFER_WIDTH)");
    pp.add_macro_definition("BUFFER_RCP_HEIGHT", "(1.0 / BUFFER_HEIGHT)");
    pp.add_macro_definition("BUFFER_COLOR_SPACE", "1");   // sRGB
    pp.add_macro_definition("BUFFER_COLOR_FORMAT", "87"); // B8G8R8A8: ReShade runs an X8 back buffer's effects on an A8 copy
    pp.add_macro_definition("BUFFER_COLOR_BIT_DEPTH", "8");
    for (const auto& [name, value] : folders.defines) // the look's PreprocessorDefinitions: before the file, as ReShade adds them
        if (!name.empty()) pp.add_macro_definition(name, value.empty() ? "1" : value); // "FOO=" is 1 in ReShade
    // then a ReShade.ini / GShade.ini beside the game's exe, [GENERAL] PreprocessorDefinitions (the look's win, as in
    // ReShade). Its depth settings are not taken: Apex knows this game's depth (below), and a ReShade.ini's
    // RESHADE_DEPTH_INPUT_IS_REVERSED=1 (ReShade's default) would turn MXAO and DOF inside out here
    for (const auto& [name, value] : GlobalDefines())
        if (!name.empty() && name.rfind("RESHADE_DEPTH_", 0) != 0) pp.add_macro_definition(name, value.empty() ? "1" : value);
    // ReShade's global depth setting for this game: the INTZ depth is 0 near .. 1 far, as the runner's "not reversed";
    // ReShade.fxh and qUINT_common.fxh default to reversed. After the look's own, so a look that names it still wins
    pp.add_macro_definition("RESHADE_DEPTH_INPUT_IS_REVERSED", "0");
    for (const std::wstring& d : folders.includeDirs) pp.add_include_path(d);
    pp.append_string(
        "#define tex2Doffset(s, coords, offset) tex2D(s, coords, offset)\n"
        "#define tex2Dlodoffset(s, coords, offset) tex2Dlod(s, coords, offset)\n"
        "#define tex2Dgather(s, t, c) tex2Dgather##c(s, t)\n"
        "#define tex2Dgatheroffset(s, t, o, c) tex2Dgather##c(s, t, o)\n"
        "#define tex2Dgather0 tex2DgatherR\n"
        "#define tex2Dgather1 tex2DgatherG\n"
        "#define tex2Dgather2 tex2DgatherB\n"
        "#define tex2Dgather3 tex2DgatherA\n");
    if (!pp.append_string(std::move(source), std::filesystem::path(fxPath)))
        return fail(ReasonFrom(pp.errors(), folders.root));
    if (pp.output().size() > kMaxPreprocessedBytes) return fail("the file expands to more than 16 MB of code");
    out.macros = UsedMacros(pp, fxPath, folders.defines);
    out.compiledWith = folders.defines;

    // ---- parse and generate shader model 3.0 HLSL
    std::unique_ptr<reshadefx::codegen> cg(reshadefx::create_codegen_hlsl(30, false, false));
    reshadefx::parser parser;
    if (!parser.parse(pp.output(), cg.get())) return fail(ReasonFrom(parser.errors(), folders.root));
    const reshadefx::effect_module& m = cg->module();

    // ---- uniforms
    for (const reshadefx::uniform& u : m.uniforms)
    {
        Uniform x;
        x.name = u.name;
        x.base = u.type.is_boolean() ? kBool : u.type.is_floating_point() ? kFloat : u.type.is_signed() ? kInt : kUint;
        x.rows = (uint8_t)u.type.rows; x.cols = (uint8_t)u.type.cols;
        x.array = u.type.is_array() ? u.type.array_length : 0;
        x.offset = u.offset;
        if (u.has_initializer_value)
        {
            if (x.array)
                for (const auto& el : u.initializer_value.array_data) { auto f = Floats(u.type, el, x.Comps()); x.init.insert(x.init.end(), f.begin(), f.end()); }
            else x.init = Floats(u.type, u.initializer_value, x.Comps());
        }
        x.init.resize((size_t)x.Comps() * std::max(1u, x.array), 0.0f);
        x.ann = Anns(u.annotations);
        const uint32_t regs = (u.type.is_matrix() ? x.rows : 1u) * std::max(1u, x.array);
        out.constRegs = std::max(out.constRegs, u.offset / 16 + regs);
        out.uniforms.push_back(std::move(x));
    }
    if (m.uniforms.size() > 256) return fail("has more than 256 settings");

    // ---- textures
    if (m.textures.size() > kMaxTextures) return fail("uses more than " + std::to_string(kMaxTextures) + " textures");
    for (const reshadefx::texture& t : m.textures)
    {
        Texture x;
        x.name = t.name; x.unique = t.unique_name; x.semantic = t.semantic;
        x.w = t.width; x.h = t.height; x.levels = t.levels;
        if (!t.semantic.empty() && t.semantic_binding != 0xFFFFFFFF) x.semReg = (int32_t)t.semantic_binding;
        if (t.semantic.empty())
        {
            const char* lowered = "";
            x.format = Format(t.format, &lowered);
            if (*lowered) out.notes.push_back(std::string("texture ") + t.name + ": " + lowered);
            if (t.type == reshadefx::texture_type::texture_3d) x.error = "uses a 3D texture, which Direct3D 9 cannot draw to";
            else if (t.type == reshadefx::texture_type::texture_1d) { x.h = 1; out.notes.push_back("texture " + t.name + ": 1D as 2D (height 1)"); }
            else if (x.w > 8192 || x.h > 8192) x.error = "uses a texture larger than 8192 pixels";
            for (const auto& a : t.annotations)
                if (a.name == "source" && a.type.base == reshadefx::type::t_string)
                {
                    x.format = D3DFMT_A8R8G8B8; // images are uploaded as 8-bit BGRA
                    x.image = FindTextureFile(folders, a.value.string_data);
                    uint64_t isz = 0;
                    // a missing file: the texture stays cleared to zero and the effect runs, as ReShade does (it logs the file)
                    if (x.image.empty()) out.notes.push_back("texture " + t.name + ": " + a.value.string_data + " is not in the Textures folder (left black, as in ReShade)");
                    else if (!FileSize(x.image, isz) || isz > kMaxTextureFileBytes) x.error = "its texture " + a.value.string_data + " is larger than 64 MB";
                }
            if (x.error.empty())
            {
                x.bytes = (uint64_t)x.w * x.h * BytesPerPixel(x.format);
                if (x.levels > 1) x.bytes = x.bytes * 4 / 3;
                out.gpuBytes += x.bytes;
            }
        }
        else if (t.semantic != "COLOR" && t.semantic != "DEPTH")
            x.error = "needs the game's " + t.semantic + " texture, which Apex does not provide";
        out.textures.push_back(std::move(x));
    }
    if (out.gpuBytes > kMaxEffectGpuBytes) return fail("needs more than 256 MB of textures");
    for (const auto& t : out.textures) if (t.semReg >= 0 && (uint32_t)t.semReg < out.constRegs) return fail("uses too many settings for shader model 3.0");
    if (out.constRegs > (uint32_t)kConstRegs) return fail("uses too many settings for shader model 3.0");
    auto texIndex = [&](const std::string& unique) {
        for (size_t i = 0; i < out.textures.size(); ++i) if (out.textures[i].unique == unique) return (int)i;
        return -1;
    };

    // ---- samplers
    for (const reshadefx::sampler& s : m.samplers)
    {
        Sampler x;
        const int ti = texIndex(s.texture_name);
        x.texture = ti < 0 ? 0 : (uint32_t)ti;
        const int f = (int)s.filter;
        if (s.filter == reshadefx::filter_mode::anisotropic) x.minF = x.magF = D3DTEXF_ANISOTROPIC, x.mipF = D3DTEXF_LINEAR;
        else { x.minF = Filter((f >> 4) & 3); x.magF = Filter((f >> 2) & 3); x.mipF = Filter(f & 3); }
        x.addrU = (uint32_t)s.address_u; x.addrV = (uint32_t)s.address_v; x.addrW = (uint32_t)s.address_w; // same values as D3DTADDRESS_*
        x.lodBias = s.lod_bias;
        x.minLod = std::isfinite(s.min_lod) ? std::max(0.0f, s.min_lod) : 0.0f;
        out.samplers.push_back(x);
    }

    // ---- techniques and passes
    std::map<std::string, int> shaderOf;
    auto shaderIndex = [&](const std::string& entry, bool vs) {
        auto it = shaderOf.find(entry);
        if (it != shaderOf.end()) return it->second;
        Shader s;
        s.entry = entry; s.vs = vs;
        out.shaders.push_back(std::move(s));
        return shaderOf[entry] = (int)out.shaders.size() - 1;
    };
    for (const reshadefx::technique& t : m.techniques)
    {
        Technique x;
        x.name = t.name;
        x.ann = Anns(t.annotations);
        if (const Ann* h = FindAnn(x.ann, "hidden")) x.hidden = !h->isString && h->f[0] != 0.0f;
        auto refuse = [&](std::string why) { if (x.reason.empty()) x.reason = std::move(why); };
        if (t.passes.size() > kMaxPasses) refuse("has more than 64 passes");
        for (const reshadefx::pass& p : t.passes)
        {
            Pass y;
            if (!p.cs_entry_point.empty()) { refuse("uses compute shaders, which need Direct3D 11 or later"); continue; }
            y.vs = shaderIndex(p.vs_entry_point, true);
            y.ps = p.ps_entry_point.empty() ? -1 : shaderIndex(p.ps_entry_point, false);
            if (y.ps < 0) refuse("has a pass without a pixel shader");
            int rts = 0;
            for (int r = 0; r < 8; ++r)
            {
                if (p.render_target_names[r].empty()) continue;
                if (r >= kMaxRenderTargets) { refuse("draws to more than 4 textures in one pass (Direct3D 9 allows 4)"); break; }
                y.rt[r] = texIndex(p.render_target_names[r]);
                ++rts;
            }
            y.clear = p.clear_render_targets; y.mips = p.generate_mipmaps; y.srgbWrite = p.srgb_write_enable; y.blend = p.blend_enable[0];
            y.src = Blend(p.source_color_blend_factor[0]); y.dst = Blend(p.dest_color_blend_factor[0]); y.op = (uint32_t)p.color_blend_op[0];
            y.srcA = Blend(p.source_alpha_blend_factor[0]); y.dstA = Blend(p.dest_alpha_blend_factor[0]); y.opA = (uint32_t)p.alpha_blend_op[0];
            for (int r = 0; r < 4; ++r) y.mask[r] = p.render_target_write_mask[r];
            y.stencil = p.stencil_enable;
            y.sRef = p.stencil_reference_value; y.sRead = p.stencil_read_mask; y.sWrite = p.stencil_write_mask;
            y.sFunc = (uint32_t)p.stencil_comparison_func + 1; // never..always -> D3DCMP_NEVER..ALWAYS
            y.sPass = StencilOp(p.stencil_pass_op); y.sFail = StencilOp(p.stencil_fail_op); y.sZFail = StencilOp(p.stencil_depth_fail_op);
            y.prim = (uint32_t)p.topology; // same values as D3DPRIMITIVETYPE
            y.vertices = p.num_vertices;
            if (y.vertices > kMaxVertices) refuse("draws more than a million vertices in one pass");
            for (size_t b = 0; b < p.sampler_bindings.size(); ++b)
            {
                Binding bd;
                bd.sampler = (uint32_t)p.sampler_bindings[b].index;
                bd.reg = p.sampler_bindings[b].entry_point_binding;
                bd.srgb = b < p.texture_bindings.size() && p.texture_bindings[b].srgb;
                if (bd.reg >= 16 || bd.sampler >= out.samplers.size()) { refuse("uses more than 16 textures in one pass"); continue; }
                y.bind.push_back(bd);
            }
            // the textures this pass touches
            auto use = [&](int ti) {
                if (ti < 0 || ti >= (int)out.textures.size()) return;
                const Texture& tx = out.textures[ti];
                if (!tx.error.empty()) refuse(tx.error);
                if (tx.semantic == "DEPTH") x.needsDepth = true;
            };
            for (int r = 0; r < rts; ++r) use(y.rt[r]);
            for (const Binding& bd : y.bind) use((int)out.samplers[bd.sampler].texture);
            x.passes.push_back(std::move(y));
        }
        out.techniques.push_back(std::move(x));
    }

    // ---- D3DCompile each entry point a runnable technique uses (vs_3_0 / ps_3_0), or their results from the compile cache
    std::vector<bool> wanted(out.shaders.size(), false);
    for (const Technique& t : out.techniques)
        if (t.reason.empty())
            for (const Pass& p : t.passes) { if (p.vs >= 0) wanted[p.vs] = true; if (p.ps >= 0) wanted[p.ps] = true; }
    std::vector<size_t> todo;
    std::vector<std::string> hlsl;
    for (size_t i = 0; i < out.shaders.size(); ++i)
    {
        if (!wanted[i]) continue;
        Shader& s = out.shaders[i];
        std::string code, unused;
        if (!cg->assemble_code_for_entry_point(s.entry, code, unused, unused)) { s.error = "the shader does not compile"; continue; }
        todo.push_back(i);
        hlsl.push_back(std::move(code));
    }
    uint64_t key = 14695981039346656037ull;
    const uint32_t compilerVersion = D3D_COMPILER_VERSION;
    key = Fnv(key, kCacheMagic, sizeof kCacheMagic);
    key = Fnv(key, &compilerVersion, sizeof compilerVersion);
    for (const auto& [name, value] : folders.defines) { key = Fnv(key, name.c_str(), name.size() + 1); key = Fnv(key, value.c_str(), value.size() + 1); }
    for (size_t k = 0; k < todo.size(); ++k)
    {
        const Shader& s = out.shaders[todo[k]];
        key = Fnv(key, s.entry.c_str(), s.entry.size() + 1);
        key = Fnv(key, &s.vs, 1);
        key = Fnv(key, hlsl[k].c_str(), hlsl[k].size() + 1);
    }
    out.cacheKey = key;
    const std::wstring cacheFile = folders.cacheDir.empty() || todo.empty() ? std::wstring() : CacheFile(folders.cacheDir, fxPath, W, H, folders.defines);
    std::vector<CachedShader> results;
    const bool cached = !cacheFile.empty() && ReadCache(cacheFile, key, todo.size(), results);
    if (!cached)
    {
        results.assign(todo.size(), {});
        for (size_t k = 0; k < todo.size(); ++k)
        {
            const Shader& s = out.shaders[todo[k]];
            if (!CompileSm3(hlsl[k], s.entry, s.vs != 0, results[k].code, results[k].errors, &results[k].tried)) results[k].code.clear();
        }
        if (!cacheFile.empty()) WriteCache(folders.cacheDir, cacheFile, key, results);
    }
    if (!cacheFile.empty()) ++(cached ? g_cacheHits : g_cacheMisses);
    for (size_t k = 0; k < todo.size(); ++k)
    {
        Shader& s = out.shaders[todo[k]];
        CachedShader& r = results[k];
        if (r.tried >= 0 && r.tried < (int)std::size(kTries) && r.code.size() >= 8)
        {
            s.code = std::move(r.code);
            if (r.tried) out.notes.push_back("shader " + s.entry + ": " + kTries[r.tried].name);
        }
        else s.error = ReasonFrom(r.errors.empty() ? std::string("the shader does not compile") : r.errors, folders.root);
    }
    for (Technique& t : out.techniques)
        for (const Pass& p : t.passes)
            for (int si : { p.vs, p.ps })
                if (si >= 0 && t.reason.empty() && !out.shaders[si].error.empty()) t.reason = out.shaders[si].error;
    return true;
}

} // namespace

bool InsideRoot(const std::wstring& root, const std::wstring& path)
{
    if (root.empty() || path.size() <= root.size()) return false;
    std::wstring p = path;
    std::replace(p.begin(), p.end(), L'/', L'\\');
    for (size_t s = 0; s <= p.size();)
    {
        size_t e = p.find(L'\\', s);
        if (e == std::wstring::npos) e = p.size();
        if (p.compare(s, e - s, L"..") == 0) return false;
        s = e + 1;
    }
    return Lower(p.substr(0, root.size())) == Lower(root);
}

void CacheCounts(int* hits, int* misses) { *hits = g_cacheHits; *misses = g_cacheMisses; }

bool MacrosMatch(const Effect& fx, const LooksDefines& defines)
{
    // compiled with exactly these: a definition the file ignores (its own #define wins) would otherwise never match and the
    // file would compile again at every scan, its effect created again each time
    if (fx.compiledWith == defines) return true;
    for (const Macro& m : fx.macros)
    {
        const std::string* want = &m.def;
        for (const auto& d : defines) if (d.first == m.name) want = &d.second; // the last one wins, as in the preprocessor's order
        if (TrimA(*want) != m.value) return false;
    }
    return true;
}

bool Compile(const std::wstring& fxPath, const Folders& folders, UINT width, UINT height, Effect& out)
{
    try
    {
        return CompileImpl(fxPath, folders, width, height, out);
    }
    catch (const std::exception& e)
    {
        out.error = std::string("the compiler stopped: ") + e.what();
    }
    catch (...)
    {
        out.error = "the compiler stopped";
    }
    out.techniques.clear();
    return false;
}

// ============================================================================================== uniforms
void SetUniform(const Effect& fx, size_t index, const float* comps, size_t count, std::vector<float>& regs)
{
    if (index >= fx.uniforms.size()) return;
    const Uniform& u = fx.uniforms[index];
    const bool mat = u.cols > 1;
    const unsigned rows = mat ? u.rows : 1u, cols = mat ? u.cols : u.Comps();
    const unsigned per = u.Comps(), elems = std::max(1u, u.array);
    const size_t base = u.offset / 4; // one float4 register per vector, matrix row and array element
    for (unsigned e = 0; e < elems; ++e)
        for (unsigned r = 0; r < rows; ++r)
            for (unsigned c = 0; c < cols; ++c)
            {
                const size_t src = (size_t)e * per + r * cols + c;
                const size_t dst = base + ((size_t)e * rows + r) * 4 + c;
                if (src < count && dst < regs.size()) regs[dst] = comps[src];
            }
}

void DefaultValues(const Effect& fx, std::vector<float>& regs)
{
    regs.assign(std::max<size_t>(4, (size_t)fx.constRegs * 4), 0.0f);
    for (size_t i = 0; i < fx.uniforms.size(); ++i)
        SetUniform(fx, i, fx.uniforms[i].init.data(), fx.uniforms[i].init.size(), regs);
}

void UpdateSources(const Effect& fx, std::vector<float>& regs, SourceState& st)
{
    if (st.pingpong.size() != fx.uniforms.size() * 2) st.pingpong.assign(fx.uniforms.size() * 2, 0.0f);
    if (st.keys.size() != fx.uniforms.size() * 2) st.keys.assign(fx.uniforms.size() * 2, 0);
    if (st.wheelValue.size() != fx.uniforms.size()) st.wheelValue.assign(fx.uniforms.size(), NAN);
    // input as ReShade reads it: only while the game's window has the focus (this process's foreground window)
    HWND fg = GetForegroundWindow();
    DWORD pid = 0;
    if (fg) GetWindowThreadProcessId(fg, &pid);
    const bool focused = fg && pid == GetCurrentProcessId();
    float mouse[2] = { st.mouse[0], st.mouse[1] };
    if (focused)
    {
        POINT p{}; RECT rc{};
        if (GetCursorPos(&p) && ScreenToClient(fg, &p) && GetClientRect(fg, &rc) && rc.right > 0 && rc.bottom > 0)
            mouse[0] = (float)p.x * fx.width / (float)rc.right, mouse[1] = (float)p.y * fx.height / (float)rc.bottom;
    }
    const float delta[2] = { st.mouseSeen ? mouse[0] - st.mouse[0] : 0.0f, st.mouseSeen ? mouse[1] - st.mouse[1] : 0.0f };
    st.mouse[0] = mouse[0], st.mouse[1] = mouse[1], st.mouseSeen = true;
    // key / mousebutton: held; mode "toggle" (or toggle = true) flips on each press; mode "press" is true on the pressed frame
    auto keyValue = [&](size_t i, int vk) {
        const bool down = focused && vk > 0 && vk < 256 && (GetAsyncKeyState(vk) & 0x8000) != 0;
        uint8_t& was = st.keys[i * 2]; uint8_t& tog = st.keys[i * 2 + 1];
        const bool pressed = down && !was;
        was = down;
        const Ann* mode = FindAnn(fx.uniforms[i].ann, "mode");
        const Ann* toggle = FindAnn(fx.uniforms[i].ann, "toggle");
        if ((mode && mode->isString && mode->s == "toggle") || (toggle && !toggle->isString && toggle->f[0] != 0.0f)) { if (pressed) tog = !tog; return tog ? 1.0f : 0.0f; }
        if (mode && mode->isString && mode->s == "press") return pressed ? 1.0f : 0.0f;
        return down ? 1.0f : 0.0f;
    };
    for (size_t i = 0; i < fx.uniforms.size(); ++i)
    {
        const Uniform& u = fx.uniforms[i];
        const Ann* src = FindAnn(u.ann, "source");
        if (!src || !src->isString) continue;
        const std::string& n = src->s;
        float v[4] = { 0, 0, 0, 0 };
        if (n == "timer") v[0] = st.timerMs;
        else if (n == "frametime") v[0] = st.frametimeMs;
        else if (n == "framecount") v[0] = u.base == kBool ? (st.frame % 2 == 0 ? 1.0f : 0.0f) : (float)st.frame; // a bool: even frames (ReShade)
        else if (n == "random")
        {
            const Ann* mn = FindAnn(u.ann, "min"); const Ann* mx = FindAnn(u.ann, "max");
            const int lo = mn ? (int)mn->f[0] : 0, hi = mx ? (int)mx->f[0] : 32767;
            st.rng = st.rng * 1664525u + 1013904223u;
            v[0] = (float)(lo + (hi > lo ? (int)((st.rng >> 8) % (uint32_t)(hi - lo + 1)) : 0));
        }
        else if (n == "pingpong")
        {
            // as ReShade's runtime: a step between step[0] and step[1] (step[1] 0: step[0]), slowed by smoothing near the end it
            // moves to, at least 0.05 a second; from the uniform's own value (x, then its direction in y: 0 counts as rising)
            const Ann* mn = FindAnn(u.ann, "min"); const Ann* mx = FindAnn(u.ann, "max"); const Ann* sp = FindAnn(u.ann, "step");
            const Ann* sm = FindAnn(u.ann, "smoothing");
            const float lo = mn && !mn->isString ? mn->f[0] : 0.0f, hi = mx && !mx->isString ? mx->f[0] : 1.0f; // (max 1 when absent, as ReShade)
            const float s0 = sp && !sp->isString ? sp->f[0] : 0.0f, s1 = sp && !sp->isString ? sp->f[1] : 0.0f;
            const float smoothing = sm && !sm->isString ? sm->f[0] : 0.0f;
            float& val = st.pingpong[i * 2]; float& dir = st.pingpong[i * 2 + 1];
            if (dir == 0) { val = u.init.size() > 0 ? u.init[0] : 0.0f; dir = u.init.size() > 1 && u.init[1] < 0.0f ? -1.0f : 1.0f; }
            float inc = s0;
            if (s1 != 0.0f)
            {
                st.rng = st.rng * 1664525u + 1013904223u;
                inc = s0 + std::fmod((float)((st.rng >> 16) & 0x7FFF), s1 - s0 + 1.0f);
            }
            const float dt = st.frametimeMs * 0.001f;
            if (dir > 0)
            {
                inc = std::max(inc - std::max(0.0f, smoothing - (hi - val)), 0.05f) * dt;
                if ((val += inc) >= hi) { val = hi; dir = -1; }
            }
            else
            {
                inc = std::max(inc - std::max(0.0f, smoothing - (val - lo)), 0.05f) * dt;
                if ((val -= inc) <= lo) { val = lo; dir = 1; }
            }
            v[0] = val; v[1] = dir;
        }
        else if (n == "date")
        {
            SYSTEMTIME t; GetLocalTime(&t);
            v[0] = t.wYear; v[1] = t.wMonth; v[2] = t.wDay; v[3] = (float)(t.wHour * 3600 + t.wMinute * 60 + t.wSecond);
        }
        else if (n == "bufready_depth") v[0] = st.depthReady ? 1.0f : 0.0f;
        else if (n == "key")
        {
            const Ann* kc = FindAnn(u.ann, "keycode");
            v[0] = keyValue(i, kc && !kc->isString ? (int)kc->f[0] : 0);
        }
        else if (n == "mousebutton")
        {
            static const int kButtons[5] = { VK_LBUTTON, VK_RBUTTON, VK_MBUTTON, VK_XBUTTON1, VK_XBUTTON2 };
            const Ann* kc = FindAnn(u.ann, "keycode");
            const int b = kc && !kc->isString ? (int)kc->f[0] : -1;
            v[0] = keyValue(i, b >= 0 && b < 5 ? kButtons[b] : 0);
        }
        else if (n == "mousepoint") { v[0] = mouse[0]; v[1] = mouse[1]; }
        else if (n == "mousedelta") { v[0] = delta[0]; v[1] = delta[1]; }
        else if (n == "mousewheel")
        {
            // ReShade: value += notches x step (step 0 = 1), kept in min..max when they differ; .y = the frame's notches
            const Ann* mn = FindAnn(u.ann, "min"); const Ann* mx = FindAnn(u.ann, "max"); const Ann* sp = FindAnn(u.ann, "step");
            const float lo = mn && !mn->isString ? mn->f[0] : 0.0f, hi = mx && !mx->isString ? mx->f[0] : 0.0f;
            const float step = sp && !sp->isString && sp->f[0] != 0.0f ? sp->f[0] : 1.0f;
            float& val = st.wheelValue[i];
            if (std::isnan(val)) val = u.init.empty() ? 0.0f : u.init[0];
            const float notches = focused ? st.wheel : 0.0f;
            val += notches * step;
            if (lo != hi) val = std::min(std::max(val, lo), hi);
            v[0] = val; v[1] = notches;
        }
        else if (n == "overlay_open" || n == "ui_open") v[0] = st.overlayOpen ? 1.0f : 0.0f;
        SetUniform(fx, i, v, std::min<size_t>(4, u.Comps()), regs);
    }
}

// ============================================================================================== GPU
namespace {

SetRenderTargetFn g_setRT = nullptr;
SetDepthStencilFn g_setDS = nullptr;
void BindRT(IDirect3DDevice9* dev, DWORD i, IDirect3DSurface9* s) { if (g_setRT) g_setRT(dev, i, s); else dev->SetRenderTarget(i, s); }
void BindDS(IDirect3DDevice9* dev, IDirect3DSurface9* s) { if (g_setDS) g_setDS(dev, s); else dev->SetDepthStencilSurface(s); }

} // namespace

// An image file -> BGRA at w x h (WIC; COM is initialised here when the thread has not done it)
bool DecodeImage(const std::wstring& path, UINT w, UINT h, std::vector<uint8_t>& bgra)
{
    std::string bytes;
    if (!ReadCapped(path, kMaxTextureFileBytes, bytes)) return false;
    const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    IWICImagingFactory* f = nullptr; IWICStream* st = nullptr; IWICBitmapDecoder* dec = nullptr;
    IWICBitmapFrameDecode* fr = nullptr; IWICFormatConverter* conv = nullptr;
    UINT iw = 0, ih = 0;
    bool ok = SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&f)))
        && SUCCEEDED(f->CreateStream(&st)) && SUCCEEDED(st->InitializeFromMemory((BYTE*)bytes.data(), (DWORD)bytes.size()))
        && SUCCEEDED(f->CreateDecoderFromStream(st, nullptr, WICDecodeMetadataCacheOnDemand, &dec))
        && SUCCEEDED(dec->GetFrame(0, &fr)) && SUCCEEDED(f->CreateFormatConverter(&conv))
        && SUCCEEDED(conv->Initialize(fr, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))
        && SUCCEEDED(conv->GetSize(&iw, &ih)) && iw && ih && iw <= 16384 && ih <= 16384;
    if (ok && (iw != w || ih != h))
    {
        // ReShade resizes an image to its texture's size with stb_image_resize2 (runtime.cpp: STBIR_EDGE_CLAMP,
        // STBIR_FILTER_DEFAULT, alpha-weighted RGBA); the same call here gives its pixels (WIC's Fant scaler differed by
        // up to 43 levels on RetroTV's border). The alpha is the 4th byte in BGRA as in RGBA.
        std::vector<uint8_t> full((size_t)iw * ih * 4);
        ok = SUCCEEDED(conv->CopyPixels(nullptr, iw * 4, (UINT)full.size(), full.data()));
        if (ok)
        {
            bgra.resize((size_t)w * h * 4);
            ok = stbir_resize(full.data(), (int)iw, (int)ih, 0, bgra.data(), (int)w, (int)h, 0, STBIR_RGBA, STBIR_TYPE_UINT8, STBIR_EDGE_CLAMP, STBIR_FILTER_DEFAULT) != nullptr;
        }
    }
    else if (ok) { bgra.resize((size_t)w * h * 4); ok = SUCCEEDED(conv->CopyPixels(nullptr, w * 4, (UINT)bgra.size(), bgra.data())); }
    for (IUnknown* u : std::initializer_list<IUnknown*>{ conv, fr, dec, st, f }) if (u) u->Release();
    if (SUCCEEDED(com)) CoUninitialize();
    return ok;
}

void SetBindFunctions(SetRenderTargetFn rt, SetDepthStencilFn ds) { g_setRT = rt; g_setDS = ds; }
void SetAdapter(uint32_t vendor, uint32_t device) { g_vendor = vendor; g_device = device; }

namespace {
// Textures of live effects by unique name, shared as ReShade shares them (render thread): the texture and its users.
// Counted here, not by Release()'s result: the device may still hold a reference to a bound texture.
std::map<std::string, std::pair<IDirect3DTexture9*, int>> g_shared;
} // namespace

void Instance::Release()
{
    for (IDirect3DTexture9* t : tex_)
    {
        if (!t || t == colorTex_) continue;
        for (auto it = g_shared.begin(); it != g_shared.end(); ++it)
            if (it->second.first == t) { if (--it->second.second == 0) g_shared.erase(it); break; }
        t->Release();
    }
    tex_.clear();
    for (IUnknown* s : shaders_) if (s) s->Release();
    shaders_.clear();
    if (colorSurf_) colorSurf_->Release(), colorSurf_ = nullptr;
    if (colorTex_) colorTex_->Release(), colorTex_ = nullptr;
    if (stencil_) stencil_->Release(), stencil_ = nullptr;
    if (decl_) decl_->Release(), decl_ = nullptr;
    if (vb_) vb_->Release(), vb_ = nullptr;
    fx_.reset();
    gpuBytes_ = 0;
}

bool Instance::Create(IDirect3DDevice9* dev, std::shared_ptr<const Effect> fxp, std::string* err)
{
    Release();
    if (!fxp) return false;
    const Effect& fx = *fxp;
    auto fail = [&](const std::string& m) { if (err) *err = m; Release(); return false; };
    const UINT W = fx.width, H = fx.height;
    if (FAILED(dev->CreateTexture(W, H, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A8R8G8B8, D3DPOOL_DEFAULT, &colorTex_, nullptr)))
        return fail("could not create the picture copy");
    colorTex_->GetSurfaceLevel(0, &colorSurf_);
    gpuBytes_ = (uint64_t)W * H * 4;
    IDirect3D9* d3d = nullptr;
    dev->GetDirect3D(&d3d);
    D3DDEVICE_CREATION_PARAMETERS cp{};
    dev->GetCreationParameters(&cp);

    tex_.assign(fx.textures.size(), nullptr);
    for (size_t i = 0; i < fx.textures.size(); ++i)
    {
        const Texture& t = fx.textures[i];
        if (t.semantic == "COLOR") { tex_[i] = colorTex_; continue; }
        if (!t.semantic.empty() || !t.error.empty()) continue; // DEPTH (bound at Run) / refused: its techniques do not run
        if (auto sh = g_shared.find(t.unique); sh != g_shared.end()) { (tex_[i] = sh->second.first)->AddRef(); ++sh->second.second; continue; }
        const DWORD usage = D3DUSAGE_RENDERTARGET | (t.levels > 1 ? D3DUSAGE_AUTOGENMIPMAP : 0);
        D3DFORMAT fmt = t.format;
        // L8 / L16 render targets are optional in D3D9: the next wider format samples the same red channel
        if (d3d && FAILED(d3d->CheckDeviceFormat(cp.AdapterOrdinal, cp.DeviceType, D3DFMT_X8R8G8B8, D3DUSAGE_RENDERTARGET, D3DRTYPE_TEXTURE, fmt)))
            fmt = fmt == D3DFMT_L8 ? D3DFMT_A8R8G8B8 : fmt == D3DFMT_L16 ? D3DFMT_G16R16 : fmt;
        if (FAILED(dev->CreateTexture(t.w, t.h, t.levels > 1 ? 0 : 1, usage, fmt, D3DPOOL_DEFAULT, &tex_[i], nullptr)))
        {
            if (d3d) d3d->Release();
            return fail("this graphics card can't create the texture " + t.name);
        }
        g_shared[t.unique] = { tex_[i], 1 };
        gpuBytes_ += t.bytes;
        IDirect3DSurface9* s0 = nullptr;
        tex_[i]->GetSurfaceLevel(0, &s0);
        dev->ColorFill(s0, nullptr, 0); // ReShade clears new textures to zero
        if (t.levels > 1 && t.image.empty()) { tex_[i]->SetAutoGenFilterType(D3DTEXF_LINEAR); tex_[i]->GenerateMipSubLevels(); } // its mips too (ReShade)
        if (!t.image.empty())
        {
            std::vector<uint8_t> img;
            if (!DecodeImage(t.image, t.w, t.h, img)) { s0->Release(); if (d3d) d3d->Release(); return fail("the texture file of " + t.name + " can't be read"); }
            IDirect3DSurface9* sys = nullptr;
            if (SUCCEEDED(dev->CreateOffscreenPlainSurface(t.w, t.h, D3DFMT_A8R8G8B8, D3DPOOL_SYSTEMMEM, &sys, nullptr)))
            {
                D3DLOCKED_RECT lr;
                if (SUCCEEDED(sys->LockRect(&lr, nullptr, 0)))
                {
                    for (UINT y = 0; y < t.h; ++y) memcpy((char*)lr.pBits + (size_t)y * lr.Pitch, &img[(size_t)y * t.w * 4], (size_t)t.w * 4);
                    sys->UnlockRect();
                    dev->UpdateSurface(sys, nullptr, s0, nullptr);
                }
                sys->Release();
            }
            if (t.levels > 1) { tex_[i]->SetAutoGenFilterType(D3DTEXF_LINEAR); tex_[i]->GenerateMipSubLevels(); }
        }
        s0->Release();
    }
    if (d3d) d3d->Release();

    shaders_.assign(fx.shaders.size(), nullptr);
    for (size_t i = 0; i < fx.shaders.size(); ++i)
    {
        const Shader& s = fx.shaders[i];
        if (s.code.size() < 8) continue; // not compiled: its techniques have a reason and do not run
        HRESULT hr;
        if (s.vs) { IDirect3DVertexShader9* v = nullptr; hr = dev->CreateVertexShader((const DWORD*)s.code.data(), &v); shaders_[i] = v; }
        else { IDirect3DPixelShader9* p = nullptr; hr = dev->CreatePixelShader((const DWORD*)s.code.data(), &p); shaders_[i] = p; }
        if (FAILED(hr)) return fail("the graphics card refused the shader " + s.entry);
    }

    bool needStencil = false;
    UINT maxVerts = 3;
    for (const Technique& t : fx.techniques)
        for (const Pass& p : t.passes) { needStencil |= p.stencil != 0; maxVerts = std::max(maxVerts, p.vertices); }
    if (needStencil && FAILED(dev->CreateDepthStencilSurface(W, H, D3DFMT_D24S8, D3DMULTISAMPLE_NONE, 0, FALSE, &stencil_, nullptr)))
        return fail("could not create the stencil buffer");
    const D3DVERTEXELEMENT9 el[] = { { 0, 0, D3DDECLTYPE_FLOAT1, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0 }, D3DDECL_END() };
    if (FAILED(dev->CreateVertexDeclaration(el, &decl_))) return fail("could not create the vertex declaration");
    if (FAILED(dev->CreateVertexBuffer(maxVerts * sizeof(float), D3DUSAGE_WRITEONLY, 0, D3DPOOL_DEFAULT, &vb_, nullptr))) return fail("could not create the vertex buffer");
    {
        void* data = nullptr;
        if (FAILED(vb_->Lock(0, 0, &data, 0))) return fail("could not fill the vertex buffer");
        float* f = (float*)data;
        for (UINT i = 0; i < maxVerts; ++i) f[i] = (float)i;
        vb_->Unlock();
    }
    { D3DCAPS9 caps{}; maxPrims_ = SUCCEEDED(dev->GetDeviceCaps(&caps)) ? std::max<UINT>(1, std::min<UINT>(caps.MaxPrimitiveCount, 0x7FFFFF)) : 0xFFFF; }
    fx_ = std::move(fxp);
    return true;
}

bool Instance::Run(IDirect3DDevice9* dev, size_t technique, IDirect3DSurface9* target, IDirect3DTexture9* depth, const std::vector<float>& regs)
{
    if (!fx_ || technique >= fx_->techniques.size() || !target) return false;
    const Effect& fx = *fx_;
    const Technique& tech = fx.techniques[technique];
    if (!tech.reason.empty()) return false;
    const UINT W = fx.width, H = fx.height;

    // constants: uniforms + semantic pixel sizes (PS c0..223, VS c0..255 with __TEXEL_SIZE__ at c255)
    std::vector<float>& c = c_;
    c.assign(256 * 4, 0.0f);
    std::copy(regs.begin(), regs.begin() + std::min(regs.size(), (size_t)kConstRegs * 4), c.begin());
    for (const Texture& t : fx.textures)
        if (t.semReg >= 0 && t.semReg < kConstRegs)
        {
            c[t.semReg * 4 + 0] = 1.0f / W; c[t.semReg * 4 + 1] = 1.0f / H; // COLOR and DEPTH are the picture's size here
        }

    auto texOf = [&](size_t i) { return fx.textures[i].semantic == "DEPTH" ? depth : tex_[i]; };
    // an 8-bit texture (COLOR, RGBA8): the only kind ReShade gives an sRGB view; on others SRGBTexture / SRGBWriteEnable do nothing
    auto Srgb8 = [&](int i) {
        if (i < 0 || (size_t)i >= fx.textures.size()) return false;
        const Texture& t = fx.textures[i];
        return t.semantic == "COLOR" || (t.semantic.empty() && (t.format == D3DFMT_A8R8G8B8 || t.format == D3DFMT_X8R8G8B8));
    };
    auto texSize = [&](size_t i, UINT& w, UINT& h) {
        D3DSURFACE_DESC d{};
        if (!fx.textures[i].semantic.empty() || !tex_[i] || FAILED(tex_[i]->GetLevelDesc(0, &d))) { w = W; h = H; } else { w = d.Width; h = d.Height; } // a shared one: its maker's size
    };

    // States, samplers and textures are sent only when they change within the technique
    std::pair<DWORD, DWORD> rsCache[40]; // on the stack: no allocation per frame (29 states are set below)
    int rsCount = 0;
    auto RS = [&](D3DRENDERSTATETYPE st, DWORD v) {
        for (int i = 0; i < rsCount; ++i) if (rsCache[i].first == (DWORD)st) { if (rsCache[i].second != v) { rsCache[i].second = v; dev->SetRenderState(st, v); } return; }
        if (rsCount < (int)std::size(rsCache)) rsCache[rsCount++] = { (DWORD)st, v };
        dev->SetRenderState(st, v);
    };
    DWORD ssVal[16][14]; bool ssSet[16][14] = {};
    auto SS = [&](DWORD stage, D3DSAMPLERSTATETYPE st, DWORD v) {
        if (ssSet[stage][st] && ssVal[stage][st] == v) return;
        ssSet[stage][st] = true; ssVal[stage][st] = v; dev->SetSamplerState(stage, st, v);
    };
    IDirect3DBaseTexture9* bound[16] = {};
    IDirect3DBaseTexture9* vbound[4] = {}; // the vertex stages (D3DVERTEXTEXTURESAMPLER0..3); the caller saved them
    for (DWORD st = 0; st < 16; ++st) dev->SetTexture(st, nullptr);
    for (DWORD v = 0; v < 4; ++v) dev->SetTexture(D3DVERTEXTEXTURESAMPLER0 + v, nullptr);
    IDirect3DSurface9* boundRt[4] = { (IDirect3DSurface9*)1, nullptr, nullptr, nullptr }; // RT0 unknown; RT1-3 cleared here
    for (DWORD r = 1; r < 4; ++r) BindRT(dev, r, nullptr);
    bool dsKnown = false; IDirect3DSurface9* boundDs = nullptr;
    bool constsSent = false;
    IUnknown* curVs = nullptr; IUnknown* curPs = nullptr;
    dev->SetVertexDeclaration(decl_);

    // picture -> COLOR only before a pass that samples COLOR, when the picture changed since the last copy (the technique's
    // start, then after a pass that drew to the picture): the same COLOR a pass reads, without the copies nothing read
    // (MXAO's early passes read COLOR into their own textures; 0.03 ms at 1080p, 0.16 ms at 4K a copy)
    bool dirty = true, stencilCleared = false;
    for (const Pass& pass : tech.passes)
    {
        bool readsColor = false;
        for (const Binding& bd : pass.bind) readsColor |= fx.textures[fx.samplers[bd.sampler].texture].semantic == "COLOR";
        if (dirty && readsColor) { dev->StretchRect(target, nullptr, colorSurf_, nullptr, D3DTEXF_NONE); dirty = false; }

        IDirect3DSurface9* rts[4] = {};
        IDirect3DTexture9* rtTex[4] = {};
        const bool toPicture = pass.rt[0] < 0;
        UINT vpw = W, vph = H;
        if (toPicture) { rts[0] = target; target->AddRef(); }
        else
        {
            for (int r = 0; r < 4 && pass.rt[r] >= 0; ++r)
            {
                rtTex[r] = texOf(pass.rt[r]);
                if (rtTex[r]) rtTex[r]->GetSurfaceLevel(0, &rts[r]);
            }
            if (!rts[0]) { for (auto* s : rts) if (s) s->Release(); continue; }
            texSize(pass.rt[0], vpw, vph);
        }
        for (DWORD r = 0; r < 4; ++r) if (boundRt[r] != rts[r]) { BindRT(dev, r, rts[r]); boundRt[r] = rts[r]; }
        if (pass.clear) for (auto* s : rts) if (s) dev->ColorFill(s, nullptr, 0);
        D3DVIEWPORT9 vp{ 0, 0, vpw, vph, 0.0f, 1.0f };
        dev->SetViewport(&vp); // always: a SetRenderTarget(0) resets it

        const bool useStencil = pass.stencil && stencil_ && vpw == W && vph == H;
        IDirect3DSurface9* wantDs = useStencil ? stencil_ : nullptr;
        if (!dsKnown || boundDs != wantDs) { BindDS(dev, wantDs); boundDs = wantDs; dsKnown = true; }
        if (useStencil && !stencilCleared) { dev->Clear(0, nullptr, D3DCLEAR_STENCIL, 0, 1.0f, 0); stencilCleared = true; }
        RS(D3DRS_ZENABLE, FALSE);
        RS(D3DRS_ZWRITEENABLE, FALSE);
        RS(D3DRS_ALPHATESTENABLE, FALSE);
        RS(D3DRS_FOGENABLE, FALSE);
        RS(D3DRS_STENCILENABLE, useStencil);
        if (useStencil)
        {
            RS(D3DRS_STENCILFUNC, pass.sFunc);
            RS(D3DRS_STENCILPASS, pass.sPass);
            RS(D3DRS_STENCILFAIL, pass.sFail);
            RS(D3DRS_STENCILZFAIL, pass.sZFail);
            RS(D3DRS_STENCILREF, pass.sRef);
            RS(D3DRS_STENCILMASK, pass.sRead);
            RS(D3DRS_STENCILWRITEMASK, pass.sWrite);
        }
        RS(D3DRS_ALPHABLENDENABLE, pass.blend);
        RS(D3DRS_SEPARATEALPHABLENDENABLE, TRUE);
        RS(D3DRS_SRCBLEND, pass.src);
        RS(D3DRS_DESTBLEND, pass.dst);
        RS(D3DRS_BLENDOP, pass.op);
        RS(D3DRS_SRCBLENDALPHA, pass.srcA);
        RS(D3DRS_DESTBLENDALPHA, pass.dstA);
        RS(D3DRS_BLENDOPALPHA, pass.opA);
        RS(D3DRS_COLORWRITEENABLE, pass.mask[0]);
        RS(D3DRS_COLORWRITEENABLE1, pass.mask[1]);
        RS(D3DRS_COLORWRITEENABLE2, pass.mask[2]);
        RS(D3DRS_COLORWRITEENABLE3, pass.mask[3]);
        RS(D3DRS_SRGBWRITEENABLE, pass.srgbWrite && (toPicture || Srgb8(pass.rt[0])));
        RS(D3DRS_CULLMODE, D3DCULL_NONE);
        RS(D3DRS_SCISSORTESTENABLE, FALSE);
        RS(D3DRS_CLIPPLANEENABLE, 0);
        // as ReShade sets them on every pass: the game's own values (two-sided stencil, dithering) changed SMAA's stencil
        RS(D3DRS_TWOSIDEDSTENCILMODE, FALSE);
        RS(D3DRS_FILLMODE, D3DFILL_SOLID);
        RS(D3DRS_DITHERENABLE, FALSE);
        RS(D3DRS_LASTPIXEL, TRUE);

        // textures: this pass's bindings; a stage bound earlier and unused now is cleared
        IDirect3DBaseTexture9* want[16] = {};
        for (const Binding& bd : pass.bind)
        {
            const Sampler& s = fx.samplers[bd.sampler];
            IDirect3DTexture9* t = texOf(s.texture);
            for (auto* rt : rtTex) if (rt && rt == t) t = nullptr; // a render target is never sampled in the same pass
            want[bd.reg] = t;
            SS(bd.reg, D3DSAMP_MINFILTER, s.minF);
            SS(bd.reg, D3DSAMP_MAGFILTER, s.magF);
            SS(bd.reg, D3DSAMP_MIPFILTER, s.mipF);
            SS(bd.reg, D3DSAMP_ADDRESSU, s.addrU);
            SS(bd.reg, D3DSAMP_ADDRESSV, s.addrV);
            SS(bd.reg, D3DSAMP_ADDRESSW, s.addrW);
            SS(bd.reg, D3DSAMP_BORDERCOLOR, 0);
            SS(bd.reg, D3DSAMP_SRGBTEXTURE, bd.srgb && Srgb8(s.texture)); // ReShade has an sRGB view of 8-bit textures only
            SS(bd.reg, D3DSAMP_MAXANISOTROPY, s.minF == D3DTEXF_ANISOTROPIC || s.magF == D3DTEXF_ANISOTROPIC ? 16 : 1);
            float bias = s.lodBias; DWORD biasBits; memcpy(&biasBits, &bias, 4);
            SS(bd.reg, D3DSAMP_MIPMAPLODBIAS, biasBits);
            SS(bd.reg, D3DSAMP_MAXMIPLEVEL, (DWORD)s.minLod);
        }
        for (DWORD st = 0; st < 16; ++st) if (bound[st] != want[st]) { dev->SetTexture(st, want[st]); bound[st] = want[st]; }
        // the vertex stages too, as ReShade binds every sampler to both (the compiler gives a vertex shader's samplers the
        // lowest numbers): a vertex shader reading a texture (an exposure value) read nothing
        for (const Binding& bd : pass.bind)
        {
            if (bd.reg >= 4) continue;
            const Sampler& s = fx.samplers[bd.sampler];
            const DWORD vst = D3DVERTEXTEXTURESAMPLER0 + bd.reg;
            dev->SetSamplerState(vst, D3DSAMP_MINFILTER, s.minF);
            dev->SetSamplerState(vst, D3DSAMP_MAGFILTER, s.magF);
            dev->SetSamplerState(vst, D3DSAMP_MIPFILTER, s.mipF);
            dev->SetSamplerState(vst, D3DSAMP_ADDRESSU, s.addrU);
            dev->SetSamplerState(vst, D3DSAMP_ADDRESSV, s.addrV);
        }
        for (DWORD v = 0; v < 4; ++v) if (vbound[v] != want[v]) { dev->SetTexture(D3DVERTEXTEXTURESAMPLER0 + v, want[v]); vbound[v] = want[v]; }

        // the uniforms are the same in every pass, so they go once; only the VS pixel size (c255) follows the viewport
        const float px0 = -1.0f / vpw, px1 = 1.0f / vph;
        if (!constsSent)
        {
            dev->SetPixelShaderConstantF(0, c.data(), kConstRegs);
            c[255 * 4 + 0] = px0; c[255 * 4 + 1] = px1;
            dev->SetVertexShaderConstantF(0, c.data(), 256);
            constsSent = true;
        }
        else if (c[255 * 4 + 0] != px0 || c[255 * 4 + 1] != px1)
        {
            c[255 * 4 + 0] = px0; c[255 * 4 + 1] = px1;
            dev->SetVertexShaderConstantF(255, &c[255 * 4], 1);
        }

        if (curVs != shaders_[pass.vs]) { curVs = shaders_[pass.vs]; dev->SetVertexShader((IDirect3DVertexShader9*)curVs); }
        if (curPs != shaders_[pass.ps]) { curPs = shaders_[pass.ps]; dev->SetPixelShader((IDirect3DPixelShader9*)curPs); }
        UINT prims = pass.vertices / 3;
        switch (pass.prim)
        {
        case D3DPT_POINTLIST: prims = pass.vertices; break;
        case D3DPT_LINELIST: prims = pass.vertices / 2; break;
        case D3DPT_LINESTRIP: prims = pass.vertices > 1 ? pass.vertices - 1 : 0; break;
        case D3DPT_TRIANGLESTRIP: prims = pass.vertices > 2 ? pass.vertices - 2 : 0; break;
        default: break;
        }
        if (prims && curVs && curPs)
        {
            // one draw where the device allows it; lists split into chunks otherwise (ids stay absolute through StartVertex)
            const UINT perPrim = pass.prim == D3DPT_POINTLIST ? 1 : pass.prim == D3DPT_LINELIST ? 2 : pass.prim == D3DPT_TRIANGLELIST ? 3 : 0;
            dev->SetStreamSource(0, vb_, 0, sizeof(float));
            if (prims <= maxPrims_ || !perPrim) dev->DrawPrimitive((D3DPRIMITIVETYPE)pass.prim, 0, std::min(prims, maxPrims_));
            else for (UINT first = 0; first < prims; first += maxPrims_) dev->DrawPrimitive((D3DPRIMITIVETYPE)pass.prim, first * perPrim, std::min(maxPrims_, prims - first));
        }
        if (toPicture) dirty = true;
        else if (pass.mips)
            for (auto* t : rtTex) // an AUTOGENMIPMAP texture reports one level: its usage says it has mips
                if (D3DSURFACE_DESC d{}; t && SUCCEEDED(t->GetLevelDesc(0, &d)) && (d.Usage & D3DUSAGE_AUTOGENMIPMAP)) { t->SetAutoGenFilterType(D3DTEXF_LINEAR); t->GenerateMipSubLevels(); }
        for (auto* s : rts) if (s) s->Release();
    }
    dev->SetStreamSource(0, nullptr, 0, 0);
    for (DWORD r = 1; r < 4; ++r) if (boundRt[r]) BindRT(dev, r, nullptr);
    if (boundRt[0] != target) BindRT(dev, 0, target);
    if (!dsKnown || boundDs) BindDS(dev, nullptr);
    for (DWORD st = 0; st < 16; ++st) if (bound[st]) dev->SetTexture(st, nullptr);
    for (DWORD v = 0; v < 4; ++v) if (vbound[v]) dev->SetTexture(D3DVERTEXTEXTURESAMPLER0 + v, nullptr);
    return true;
}

} // namespace Looks::Fx
