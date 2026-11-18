// Looks: ReShade preset files (.ini) and their sources (see looks_preset.h).

#include "looks_preset.h"
#include "apex_paths.h"
#include "apex_util.h"

#include <windows.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <set>

namespace Looks {

namespace {

std::string Trim(const std::string& s)
{
    const size_t a = s.find_first_not_of(" \t\r\n"), b = s.find_last_not_of(" \t\r\n");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

bool IEquals(const std::string& a, const std::string& b) { return _stricmp(a.c_str(), b.c_str()) == 0; }

const std::map<std::string, std::string>* FindSection(const Preset& preset, const std::string& file)
{
    for (auto& kv : preset.sections)
        if (IEquals(kv.first, file)) return &kv.second;
    return nullptr;
}

std::string TopValue(const Preset& preset, const char* key)
{
    const auto top = preset.sections.find("");
    if (top == preset.sections.end()) return {};
    const auto it = top->second.find(key);
    return it == top->second.end() ? std::string() : it->second;
}

bool StartsWithI(const std::wstring& s, const std::wstring& prefix)
{
    return !prefix.empty() && s.size() >= prefix.size() && _wcsnicmp(s.c_str(), prefix.c_str(), prefix.size()) == 0;
}

// A relative path inside a source folder: no drive, root, ".." or empty part, no wildcard or device characters
bool SafeRelative(const std::string& rel)
{
    if (rel.empty() || rel.size() > 400 || rel.find_first_of(":*?\"<>|") != std::string::npos) return false;
    for (unsigned char c : rel) if (c < 32) return false;
    size_t start = 0;
    for (;;)
    {
        const size_t end = rel.find_first_of("\\/", start);
        const std::string part = rel.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (part.empty() || part == "." || part == ".." || part.back() == ' ' || part.back() == '.') return false;
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return true;
}

bool ReadBounded(const std::wstring& path, std::string& text, std::string* error, size_t cap = kMaxPresetBytes)
{
    WIN32_FILE_ATTRIBUTE_DATA info{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &info) || (info.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY))
    {
        if (error) *error = "the file is missing";
        return false;
    }
    if (info.nFileSizeHigh || info.nFileSizeLow > cap)
    {
        if (error) *error = "the file is too large for a preset";
        return false;
    }
    if (!ApexUtil::ReadFileBytes(path, text) || text.size() > cap)
    {
        if (error) *error = "the file could not be read";
        return false;
    }
    return true;
}

void ListFolder(const std::wstring& root, const char* prefix, Source source, std::vector<LookItem>& out)
{
    std::error_code ec;
    if (root.empty() || !std::filesystem::is_directory(root, ec)) return;
    std::vector<LookItem> found;
    for (auto it = std::filesystem::recursive_directory_iterator(root, std::filesystem::directory_options::skip_permission_denied, ec);
         !ec && it != std::filesystem::recursive_directory_iterator() && found.size() < kMaxLooks; it.increment(ec))
    {
        if (!it->is_regular_file(ec) || _wcsicmp(it->path().extension().c_str(), L".ini") != 0) continue;
        const std::wstring full = it->path().wstring();
        if (!StartsWithI(full, root)) continue;
        const std::string rel = ApexUtil::ToUtf8(full.substr(root.size()));
        if (!SafeRelative(rel)) continue;
        const std::string ref = std::string(prefix) + rel;
        found.push_back({ ref, RefName(ref), source });
    }
    std::sort(found.begin(), found.end(), [](const LookItem& a, const LookItem& b) { return _stricmp(a.name.c_str(), b.name.c_str()) < 0; });
    out.insert(out.end(), found.begin(), found.end());
}

} // namespace

// ============================================================================================== .ini
std::vector<std::string> SplitList(const std::string& s)
{
    std::vector<std::string> out;
    size_t start = 0;
    while (out.size() < kMaxTechniques)
    {
        const size_t p = s.find(',', start);
        std::string part = Trim(s.substr(start, p == std::string::npos ? std::string::npos : p - start));
        if (!part.empty()) out.push_back(std::move(part));
        if (p == std::string::npos) break;
        start = p + 1;
    }
    return out;
}

bool ParsePreset(const std::string& raw, Preset& out)
{
    out = Preset{};
    if (raw.size() > kMaxPresetBytes) { out.error = "the file is too large for a preset"; return false; }
    std::string_view text(raw);
    if (text.size() >= 3 && (unsigned char)text[0] == 0xEF && (unsigned char)text[1] == 0xBB && (unsigned char)text[2] == 0xBF) text.remove_prefix(3);
    std::string section; // "" = top of the file
    size_t pos = 0;
    while (pos < text.size())
    {
        const size_t eol = text.find('\n', pos);
        const std::string line = Trim(std::string(text.substr(pos, eol == std::string::npos ? std::string::npos : eol - pos)));
        pos = eol == std::string::npos ? text.size() : eol + 1;
        if (line.empty() || line[0] == ';' || line[0] == '#') continue;
        if (line[0] == '[' && line.back() == ']') { section = Trim(line.substr(1, line.size() - 2)); continue; }
        const size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        out.sections[section][Trim(line.substr(0, eq))] = Trim(line.substr(eq + 1));
    }

    const std::vector<std::string> enabled = SplitList(TopValue(out, "Techniques"));
    const std::vector<std::string> sorting = PresetSorting(out);
    const std::set<std::string> enabledSet(enabled.begin(), enabled.end());
    // ReShade runs the enabled techniques in TechniqueSorting order; enabled ones missing from the sorting run last.
    std::vector<std::string> order;
    for (auto& t : sorting) if (enabledSet.count(t) && std::find(order.begin(), order.end(), t) == order.end()) order.push_back(t);
    for (auto& t : enabled) if (std::find(order.begin(), order.end(), t) == order.end()) order.push_back(t);
    for (auto& t : order)
    {
        PresetEntry e;
        const size_t at = t.find('@');
        e.technique = at == std::string::npos ? t : t.substr(0, at);
        e.file = at == std::string::npos ? std::string() : t.substr(at + 1);
        out.order.push_back(std::move(e));
    }
    return true;
}

std::vector<std::string> PresetSorting(const Preset& preset) { return SplitList(TopValue(preset, "TechniqueSorting")); }

LooksDefines ParseDefines(const std::string& value)
{
    LooksDefines out;
    for (const std::string& item : SplitList(value))
    {
        const size_t eq = item.find('=');
        std::string name = Trim(item.substr(0, eq)), v = eq == std::string::npos ? "1" : Trim(item.substr(eq + 1));
        bool ok = !name.empty() && name.size() <= 64 && !isdigit((unsigned char)name[0]) && v.size() <= 256;
        for (char c : name) ok &= isalnum((unsigned char)c) || c == '_';
        for (char c : v) ok &= (unsigned char)c >= 32;
        if (!ok) continue;
        auto it = std::find_if(out.begin(), out.end(), [&](const auto& d) { return d.first == name; });
        if (it != out.end()) it->second = v;
        else if (out.size() < 128) out.push_back({ std::move(name), std::move(v) });
    }
    return out;
}

std::string FormatDefines(const LooksDefines& defines)
{
    std::string s;
    for (const auto& [name, value] : defines) s += (s.empty() ? "" : ",") + name + "=" + value;
    return s;
}

LooksDefines PresetDefines(const Preset& preset, const std::string& file)
{
    std::string all = TopValue(preset, "PreprocessorDefinitions");
    if (const auto* sec = FindSection(preset, file))
        if (auto it = sec->find("PreprocessorDefinitions"); it != sec->end()) all += "," + it->second;
    return ParseDefines(all);
}

std::vector<float> PresetValues(const Preset& preset, const LooksShader& d, const char* section)
{
    std::vector<float> v((size_t)d.paramCount * 4, 0.0f);
    const auto* sec = FindSection(preset, section && *section ? section : d.file);
    for (int i = 0; i < d.paramCount; ++i)
    {
        const LooksParam& p = d.params[i];
        for (int c = 0; c < 4; ++c) v[i * 4 + c] = p.def[c];
        if (!sec) continue;
        auto it = sec->find(p.key);
        if (it == sec->end() && !_stricmp(d.file, "FakeHDR.fx")) // GShade's names for SweetFX's FakeHDR uniforms
        {
            const std::string key(p.key);
            const char* gshade = key == "HDRPower" ? "fHDRPower" : key == "radius1" ? "fradius1" : key == "radius2" ? "fradius2" : nullptr;
            if (gshade) it = sec->find(gshade);
        }
        if (it == sec->end()) continue;
        const std::vector<std::string> parts = SplitList(it->second);
        for (int c = 0; c < p.comps && c < (int)parts.size(); ++c)
        {
            const float x = !_stricmp(parts[c].c_str(), "true") ? 1.0f : !_stricmp(parts[c].c_str(), "false") ? 0.0f : (float)atof(parts[c].c_str());
            if (std::isfinite(x)) v[i * 4 + c] = x;
        }
    }
    return v;
}

std::string FormatValue(const LooksParam& p, const float* v)
{
    const bool isFloat = p.kind == LooksParamKind::Float || p.kind == LooksParamKind::Color;
    std::string s;
    char buf[64];
    for (int c = 0; c < p.comps; ++c)
    {
        if (isFloat) snprintf(buf, sizeof buf, "%f", v[c]);
        else snprintf(buf, sizeof buf, "%ld", std::lround(v[c]));
        s += (c ? "," : "") + std::string(buf);
    }
    return s;
}

// ============================================================================================== sources
std::wstring LooksFolder()
{
    const std::wstring& apex = ApexPaths::ApexDirectory();
    return apex.empty() ? std::wstring() : apex + L"Looks\\";
}

bool EnsureLooksFolder()
{
    const std::wstring dir = LooksFolder();
    return !dir.empty() && ApexPaths::EnsureApexDirectory() && (CreateDirectoryW(dir.c_str(), nullptr) || GetLastError() == ERROR_ALREADY_EXISTS);
}

std::wstring GShadeFolder()
{
    wchar_t path[MAX_PATH] = {};
    const DWORD n = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (!n || n >= MAX_PATH) return {};
    std::wstring p(path, n);
    const size_t slash = p.find_last_of(L"\\/");
    return slash == std::wstring::npos ? std::wstring() : p.substr(0, slash + 1) + L"1. Shader Presets\\";
}

Source RefSource(const std::string& ref)
{
    if (ref.rfind("looks:", 0) == 0) return Source::Mine;
    if (ref.rfind("gshade:", 0) == 0) return Source::GShade;
    return Source::None;
}

std::wstring RefPath(const std::string& ref)
{
    const Source s = RefSource(ref);
    if (s != Source::Mine && s != Source::GShade) return {};
    const std::string rel = ref.substr(ref.find(':') + 1);
    const std::wstring root = s == Source::Mine ? LooksFolder() : GShadeFolder();
    if (root.empty() || !SafeRelative(rel)) return {};
    std::wstring w = ApexUtil::ToWide(rel);
    std::replace(w.begin(), w.end(), L'/', L'\\');
    return root + w;
}

std::string RefName(const std::string& ref)
{
    if (RefSource(ref) == Source::None) return {};
    std::string name = ref.substr(ref.find(':') + 1);
    if (name.size() > 4 && _stricmp(name.c_str() + name.size() - 4, ".ini") == 0) name.resize(name.size() - 4);
    return name;
}

bool LookText(const std::string& ref, std::string& text, std::string* error)
{
    text.clear();
    const std::wstring path = RefPath(ref);
    if (path.empty()) { if (error) *error = "no look chosen"; return false; }
    return ReadBounded(path, text, error);
}

std::string MigrateRef(const std::string& absolutePath)
{
    if (absolutePath.empty()) return "gshade:[Amelie] Serene 2.0.ini";
    const std::wstring path = ApexUtil::ToWide(absolutePath);
    for (const auto& [root, prefix] : { std::pair{ GShadeFolder(), "gshade:" }, std::pair{ LooksFolder(), "looks:" } })
        if (StartsWithI(path, root))
        {
            const std::string rel = ApexUtil::ToUtf8(path.substr(root.size()));
            if (SafeRelative(rel)) return prefix + rel;
        }
    const size_t slash = absolutePath.find_last_of("\\/");
    const std::string name = slash == std::string::npos ? absolutePath : absolutePath.substr(slash + 1);
    return SafeRelative(name) ? "looks:" + name : std::string();
}

std::vector<LookItem> ListLooks()
{
    std::vector<LookItem> out;
    ListFolder(LooksFolder(), "looks:", Source::Mine, out);
    ListFolder(GShadeFolder(), "gshade:", Source::GShade, out);
    return out;
}

// ============================================================================================== storing
namespace {

// One file name (no folder) that ends in one of exts (lower case, with the dot), without reserved or unsafe characters
bool ValidFileName(const std::string& name, std::initializer_list<const char*> exts)
{
    if (name.size() < 2 || name.size() > 200 || name.find_first_of("\\/") != std::string::npos || !SafeRelative(name)) return false;
    const size_t dot = name.rfind('.');
    if (dot == std::string::npos || !dot) return false;
    if (std::none_of(exts.begin(), exts.end(), [&](const char* e) { return _stricmp(name.c_str() + dot, e) == 0; })) return false;
    std::string stem = name.substr(0, name.find('.'));
    for (char& c : stem) c = (char)toupper((unsigned char)c);
    static const char* const kReserved[] = { "CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
                                             "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9" };
    for (const char* r : kReserved) if (stem == r) return false;
    return true;
}

// Stores bytes as dir\<fileName>: identical bytes already there reuse that file; a different file with the name gets a
// numbered name ("Name-1.ext"); nothing is ever overwritten
bool StoreIn(const std::wstring& dir, const std::string& fileName, const std::string& bytes, size_t cap, std::string& stored, std::wstring* created,
             std::string* error, const char* failure)
{
    const size_t dot = fileName.rfind('.');
    const std::string stem = fileName.substr(0, dot), ext = fileName.substr(dot);
    for (unsigned n = 0; n < 10000; ++n)
    {
        const std::string chosen = n ? stem + "-" + std::to_string(n) + ext : fileName;
        const std::wstring target = dir + ApexUtil::ToWide(chosen);
        if (ApexUtil::FileExists(target))
        {
            std::string existing;
            if (ReadBounded(target, existing, nullptr, cap) && existing == bytes) { stored = chosen; return true; }
            continue;
        }
        const HANDLE file = CreateFileW(target.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE)
        {
            if (GetLastError() == ERROR_FILE_EXISTS) continue;
            if (error) *error = failure;
            return false;
        }
        DWORD written = 0;
        const bool ok = WriteFile(file, bytes.data(), (DWORD)bytes.size(), &written, nullptr) && written == bytes.size();
        CloseHandle(file);
        if (!ok) { DeleteFileW(target.c_str()); if (error) *error = failure; return false; }
        stored = chosen;
        if (created) *created = target;
        return true;
    }
    if (error) *error = failure;
    return false;
}

bool IsCode(const std::string& rel) { return rel.rfind("Shaders/", 0) == 0; }
std::wstring ShaderPath(const std::string& rel)
{
    std::wstring w = ApexUtil::ToWide(rel);
    std::replace(w.begin(), w.end(), L'/', L'\\');
    return PlayerShadersFolder() + w;
}

// The quoted names after #include and after "source =" (a texture annotation) in effect source text
void QuotedRefs(const std::string& text, std::vector<std::string>& includes, std::vector<std::string>& sources)
{
    auto quoted = [&](size_t q, std::vector<std::string>& out) {
        if (q == std::string::npos || text[q] != '"') return;
        const size_t e = text.find('"', q + 1);
        if (e != std::string::npos && e - q - 1 <= 200) out.push_back(text.substr(q + 1, e - q - 1));
    };
    for (size_t p = text.find("#include"); p != std::string::npos; p = text.find("#include", p + 8))
        quoted(text.find_first_not_of(" \t", p + 8), includes);
    for (size_t p = text.find("source"); p != std::string::npos; p = text.find("source", p + 6))
    {
        if (p && (isalnum((unsigned char)text[p - 1]) || text[p - 1] == '_')) continue;
        size_t q = text.find_first_not_of(" \t\r\n", p + 6);
        if (q == std::string::npos || text[q] != '=') continue;
        quoted(text.find_first_not_of(" \t\r\n", q + 1), sources);
    }
}

} // namespace

bool ValidLookFileName(const std::string& name)
{
    return name.size() >= 5 && ValidFileName(name, { ".ini" });
}

bool RenameLook(const std::string& ref, const std::string& newStem, std::string& newRef)
{
    if (RefSource(ref) != Source::Mine) return false;
    const std::string rel = ref.substr(6); // after "looks:"
    const size_t slash = rel.find_last_of("\\/");
    const std::string dir = slash == std::string::npos ? std::string() : rel.substr(0, slash + 1);
    const std::string file = newStem + ".ini";
    if (!ValidLookFileName(file)) return false;
    const std::wstring from = RefPath(ref), to = RefPath("looks:" + dir + file);
    if (from.empty() || to.empty()) return false;
    if (!MoveFileExW(from.c_str(), to.c_str(), 0)) return false; // no MOVEFILE_REPLACE_EXISTING: a taken name fails
    newRef = "looks:" + dir + file;
    return true;
}

bool StoreLook(const std::string& fileName, const std::string& bytes, std::string& stored, std::wstring* created, std::string* error)
{
    auto fail = [&](const char* why) { if (error) *error = why; return false; };
    if (created) created->clear();
    if (!ValidLookFileName(fileName)) return fail("the look's file name is not valid");
    if (bytes.size() > kMaxPresetBytes) return fail("the file is too large for a preset");
    if (!EnsureLooksFolder()) return fail("the Looks folder could not be created");
    return StoreIn(LooksFolder(), fileName, bytes, kMaxPresetBytes, stored, created, error, "the look could not be saved");
}

// ============================================================================================== the player's shader files
std::wstring PlayerShadersFolder()
{
    const std::wstring& apex = ApexPaths::ApexDirectory();
    return apex.empty() ? std::wstring() : apex + L"Shaders\\";
}

bool ValidShaderFileName(const std::string& rel)
{
    if (IsCode(rel)) return ValidFileName(rel.substr(8), { ".fx", ".fxh" });
    return rel.rfind("Textures/", 0) == 0 && ValidFileName(rel.substr(9), { ".png", ".jpg", ".jpeg", ".bmp", ".dds" });
}

// A file of that name (lower case) in dir or up to depth folders below it; "" when none
std::wstring FindInTree(const std::wstring& dir, int depth, const std::wstring& name)
{
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return {};
    std::wstring found;
    std::vector<std::wstring> sub;
    do
    {
        const std::wstring n = fd.cFileName;
        if (n == L"." || n == L".." || (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) { if (depth > 0 && sub.size() < 64) sub.push_back(dir + n + L"\\"); }
        else if (_wcsicmp(n.c_str(), name.c_str()) == 0) found = dir + n;
    } while (found.empty() && FindNextFileW(h, &fd));
    FindClose(h);
    for (size_t k = 0; found.empty() && k < sub.size(); ++k) found = FindInTree(sub[k], depth - 1, name);
    return found;
}

// Where the player's own file is, as the scan finds it: Shaders\Shaders\<name> (or Textures\<name>), else the Shaders
// folder itself or up to three folders below Shaders\Shaders (Textures)
std::wstring OwnFilePath(const std::string& rel)
{
    const std::wstring root = PlayerShadersFolder();
    if (root.empty() || !ValidShaderFileName(rel)) return {};
    const std::wstring direct = ShaderPath(rel);
    if (GetFileAttributesW(direct.c_str()) != INVALID_FILE_ATTRIBUTES) return direct;
    const std::wstring name = ApexUtil::ToWide(rel.substr(rel.find('/') + 1));
    const bool code = IsCode(rel);
    if (std::wstring f = FindInTree(root, 0, name); !f.empty()) return f;
    return FindInTree(root + (code ? L"Shaders\\" : L"Textures\\"), 3, name);
}

// A file a downloaded pack has (or ReShade's standard headers): the importer gets it with that pack, so it is not shared
bool InAPack(const std::string& rel)
{
    const std::string name = rel.substr(rel.find('/') + 1);
    if (!_stricmp(name.c_str(), "ReShade.fxh") || !_stricmp(name.c_str(), "ReShadeUI.fxh")) return true;
    const std::wstring packs = PlayerShadersFolder() + L"Packs\\";
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((packs + L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return false;
    bool found = false;
    do
    {
        const std::wstring n = fd.cFileName;
        if ((fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && n[0] != L'.' && n.find(L'.') == std::wstring::npos)
            found = !FindInTree(packs + n + (IsCode(rel) ? L"\\Shaders\\" : L"\\Textures\\"), 3, ApexUtil::ToWide(name)).empty();
    } while (!found && FindNextFileW(h, &fd));
    FindClose(h);
    return found;
}

bool ReadShaderFile(const std::string& rel, std::string& bytes)
{
    const std::wstring path = OwnFilePath(rel);
    return !path.empty() && ReadBounded(path, bytes, nullptr, IsCode(rel) ? kMaxSharedFxBytes : kMaxSharedTextureBytes);
}

std::vector<std::string> PlayerShaderFiles(const std::vector<std::string>& fxFiles)
{
    std::vector<std::string> out, queue;
    std::set<std::string> seen;
    for (const std::string& f : fxFiles) queue.push_back("Shaders/" + f);
    for (size_t next = 0; next < queue.size() && out.size() < kMaxSharedShaderFiles && next < 4 * kMaxSharedShaderFiles; ++next)
    {
        const std::string rel = queue[next];
        std::string key = rel;
        for (char& c : key) c = (char)tolower((unsigned char)c);
        if (!seen.insert(key).second || !ValidShaderFileName(rel)) continue;
        // shortcut: a pack file an own .fx includes is left out but its pack is not added to the preset's packs list; add it
        // when players share own shaders built on packs other than ReShade's standard effects
        if (next >= fxFiles.size() && InAPack(rel)) continue; // an include or texture, not one of the look's own .fx files
        if (!IsCode(rel)) // a texture: only whether it is there and within its cap
        {
            WIN32_FILE_ATTRIBUTE_DATA a{};
            if (GetFileAttributesExW(OwnFilePath(rel).c_str(), GetFileExInfoStandard, &a) && !(a.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) &&
                !a.nFileSizeHigh && a.nFileSizeLow <= kMaxSharedTextureBytes)
                out.push_back(rel);
            continue;
        }
        std::string text;
        if (!ReadShaderFile(rel, text)) continue; // not the player's (a pack's, or missing)
        out.push_back(rel);
        std::vector<std::string> includes, sources;
        QuotedRefs(text, includes, sources);
        for (const std::string& i : includes) queue.push_back("Shaders/" + i); // names with a folder fail ValidShaderFileName
        for (const std::string& t : sources) queue.push_back("Textures/" + t);
    }
    return out;
}

bool StoreShaderFile(const std::string& rel, const std::string& bytes, std::string& stored, std::wstring* created, std::string* error)
{
    auto fail = [&](const char* why) { if (error) *error = why; return false; };
    if (created) created->clear();
    if (!ValidShaderFileName(rel)) return fail("the shader file name is not valid");
    if (bytes.size() > (IsCode(rel) ? kMaxSharedFxBytes : kMaxSharedTextureBytes)) return fail("the shader file is too large");
    const std::wstring root = PlayerShadersFolder();
    if (root.empty() || !ApexPaths::EnsureApexDirectory()) return fail("the Shaders folder could not be created");
    for (const std::wstring& d : { root, root + L"Shaders\\", root + L"Textures\\" })
        if (!CreateDirectoryW(d.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS) return fail("the Shaders folder could not be created");
    const size_t slash = rel.find('/');
    // a different file of that name stays and the import fails: a renamed copy would never run, since the look and the
    // other files' #include lines name the original
    const std::wstring target = OwnFilePath(rel); // where the scan would find a file of that name, subfolders included
    if (!target.empty())
    {
        std::string existing;
        if (ReadBounded(target, existing, nullptr, IsCode(rel) ? kMaxSharedFxBytes : kMaxSharedTextureBytes) && existing == bytes) { stored = rel; return true; }
        if (error) *error = kShaderNameTaken + rel.substr(slash + 1);
        return false;
    }
    std::string name;
    if (!StoreIn(root + ApexUtil::ToWide(rel.substr(0, slash)) + L"\\", rel.substr(slash + 1), bytes, IsCode(rel) ? kMaxSharedFxBytes : kMaxSharedTextureBytes, name,
                 created, error, "the shader file could not be saved"))
        return false;
    stored = rel.substr(0, slash + 1) + name;
    return true;
}

} // namespace Looks
