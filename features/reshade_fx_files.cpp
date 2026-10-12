// ReShade FX effects, the file side (see reshade_fx.h): the folders, adding effect files and shader packs, the headers
// most effects include, texture images and the compiled shader cache.
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "reshade_fx.h"
#include "reshade_fx_internal.h"
#include <windows.h>
#include <wincodec.h>
#include <algorithm>
#include <cctype>
#include <cwctype>
#include <format>
#include <stdexcept>
#include <string_view>
extern "C" {
#include "third_party/puff/puff.h"
}
#include "apex_log.h"
#include "apex_paths.h"
#include "apex_util.h"

#pragma comment(lib, "windowscodecs.lib")

namespace ReShadeFx {
namespace Detail {
namespace {

constexpr size_t kMaxPackBytes = 64u * 1024u * 1024u; // a shader pack .zip
constexpr size_t kMaxFileBytes = 64u * 1024u * 1024u; // one extracted file
constexpr size_t kMaxPackFiles = 4000;

// crosire/reshade-shaders (slim branch, fd00221), Shaders/ReShade.fxh: CC0-1.0
constexpr const char* kReShadeFxh =
#include "reshade_fx_reshade_fxh.inc"
    ;

// The widget macros of ReShadeUI.fxh, written for Apex: every __UNIFORM_<WIDGET>_<TYPE><N> names its widget as a ui_type
// annotation, as ReShade 4.3 and newer do
std::string ReShadeUiFxh() {
    std::string out = "// ReShadeUI.fxh for Apex Radiance: the uniform widget macros older effects use\n#pragma once\n"
                      "#define RESHADE_VERSION(major,minor,build) (10000 * (major) + 100 * (minor) + (build))\n"
                      "#define SUPPORTED_VERSION(major,minor,build) (__RESHADE__ >= RESHADE_VERSION(major,minor,build))\n";
    static const char* const kWidgets[][2] = {{"INPUT", "input"}, {"DRAG", "drag"}, {"SLIDER", "slider"}, {"COMBO", "combo"},
                                              {"RADIO", "radio"}, {"COLOR", "color"}, {"LIST", "list"}};
    for (const auto& w : kWidgets) {
        out += std::format("#define __UNIFORM_{}_ANY ui_type = \"{}\";\n", w[0], w[1]);
        for (const char* type : {"BOOL", "INT", "FLOAT"})
            for (int n = 1; n <= 4; n++) out += std::format("#define __UNIFORM_{}_{}{} __UNIFORM_{}_ANY\n", w[0], type, n, w[0]);
    }
    return out;
}

bool HasExtension(const std::wstring& name, std::initializer_list<const wchar_t*> exts) {
    const std::wstring ext = LowerW(std::filesystem::path(name).extension().wstring());
    for (const wchar_t* e : exts)
        if (ext == e) return true;
    return false;
}

// ---- ZIP shader packs (store and deflate; no ZIP64, no encryption). Only named payloads are written, below Apex's folder.
uint32_t Get(std::string_view in, size_t p, int count) {
    if (p > in.size() || static_cast<size_t>(count) > in.size() - p) throw std::runtime_error("truncated ZIP");
    uint32_t n = 0;
    for (int i = 0; i < count; ++i) n |= uint32_t(uint8_t(in[p + i])) << (8 * i);
    return n;
}

bool SafeRelative(const std::string& name) {
    if (name.empty() || name.size() > 260 || name.front() == '/' || name.find(':') != std::string::npos || name.find('\0') != std::string::npos)
        return false;
    size_t start = 0;
    while (start < name.size()) {
        size_t end = name.find('/', start);
        if (end == std::string::npos) end = name.size();
        const std::string_view part(name.data() + start, end - start);
        if (part.empty() || part == "." || part == "..") return false;
        start = end + 1;
    }
    return true;
}

struct ZipEntry {
    std::string name, bytes;
};

std::vector<ZipEntry> ReadZip(const std::string& in) {
    if (in.size() < 22) throw std::runtime_error("not a ZIP file");
    // the end of central directory record (a comment may follow it)
    size_t end = std::string::npos;
    const size_t lowest = in.size() > 22 + 65535 ? in.size() - 22 - 65535 : 0;
    for (size_t p = in.size() - 22 + 1; p-- > lowest;)
        if (Get(in, p, 4) == 0x06054b50) {
            end = p;
            break;
        }
    if (end == std::string::npos) throw std::runtime_error("not a ZIP file");
    const uint32_t count = Get(in, end + 10, 2), dirOffset = Get(in, end + 16, 4);
    if (count == 0xFFFF || dirOffset == 0xFFFFFFFF) throw std::runtime_error("ZIP64 packs are not supported");
    if (count > kMaxPackFiles) throw std::runtime_error("too many files in the pack");
    std::vector<ZipEntry> out;
    size_t p = dirOffset, total = 0;
    for (uint32_t i = 0; i < count; ++i) {
        if (Get(in, p, 4) != 0x02014b50) throw std::runtime_error("damaged ZIP directory");
        const uint32_t flags = Get(in, p + 8, 2), method = Get(in, p + 10, 2), packed = Get(in, p + 20, 4), size = Get(in, p + 24, 4);
        const uint32_t nameSize = Get(in, p + 28, 2), extra = Get(in, p + 30, 2), comment = Get(in, p + 32, 2), local = Get(in, p + 42, 4);
        if (p + 46 + nameSize > in.size()) throw std::runtime_error("damaged ZIP directory");
        std::string name = in.substr(p + 46, nameSize);
        p += 46 + nameSize + extra + comment;
        std::replace(name.begin(), name.end(), '\\', '/');
        if (name.empty() || name.back() == '/') continue; // a folder
        if (flags & 1) throw std::runtime_error("encrypted ZIP packs are not supported");
        if (method != 0 && method != 8) throw std::runtime_error("unsupported ZIP compression");
        if (size > kMaxFileBytes || (total += size) > kMaxPackBytes) throw std::runtime_error("the pack is too large");
        if (Get(in, local, 4) != 0x04034b50) throw std::runtime_error("damaged ZIP entry");
        const size_t data = size_t(local) + 30 + Get(in, local + 26, 2) + Get(in, local + 28, 2);
        if (data > in.size() || packed > in.size() - data) throw std::runtime_error("damaged ZIP entry");
        ZipEntry e;
        e.name = std::move(name);
        if (method == 0) {
            if (packed != size) throw std::runtime_error("damaged ZIP entry");
            e.bytes = in.substr(data, size);
        } else {
            e.bytes.resize(size);
            unsigned long outLen = size, inLen = packed;
            if (puff(reinterpret_cast<unsigned char*>(e.bytes.data()), &outLen, reinterpret_cast<const unsigned char*>(in.data() + data), &inLen) != 0 ||
                outLen != size)
                throw std::runtime_error("damaged compressed data in the ZIP");
        }
        out.push_back(std::move(e));
    }
    return out;
}

// Where a file of a pack goes: below Shaders\ or Textures\ (keeping the folders after "Shaders/" / "Textures/"), a preset
// to Presets\, anything else (readme, add-ons, DLLs) is skipped. Empty = skip.
std::wstring Destination(const std::string& zipName) {
    if (!SafeRelative(zipName)) return {};
    const std::string lower = Lower(zipName);
    const std::wstring file = ApexUtil::ToWide(zipName.substr(zipName.find_last_of('/') + 1));
    auto below = [&](const char* folder) -> std::string {
        const std::string key = std::string(folder) + "/";
        size_t at = lower.rfind("/" + key);
        if (at != std::string::npos) return zipName.substr(at + key.size() + 1);
        if (lower.compare(0, key.size(), key) == 0) return zipName.substr(key.size());
        return {};
    };
    if (HasExtension(file, {L".addon", L".addon32", L".addon64", L".dll", L".exe", L".asi"})) return {};
    if (auto rel = below("shaders"); !rel.empty()) {
        std::wstring w = ApexUtil::ToWide(rel);
        std::replace(w.begin(), w.end(), L'/', L'\\');
        return Sub(L"Shaders") + w;
    }
    if (auto rel = below("textures"); !rel.empty()) {
        std::wstring w = ApexUtil::ToWide(rel);
        std::replace(w.begin(), w.end(), L'/', L'\\');
        return Sub(L"Textures") + w;
    }
    if (HasExtension(file, {L".fx", L".fxh"})) return Sub(L"Shaders") + file;
    if (HasExtension(file, {L".png", L".jpg", L".jpeg", L".bmp", L".dds", L".tga", L".gif", L".tif", L".tiff"})) return Sub(L"Textures") + file;
    if (HasExtension(file, {L".ini"}) && LowerW(file) != L"reshade.ini" && LowerW(file) != L"desktop.ini") return Sub(L"Presets") + file;
    return {};
}

bool WriteOut(const std::wstring& path, const std::string& bytes, std::string& error) {
    std::error_code ec;
    std::filesystem::create_directories(std::filesystem::path(path).parent_path(), ec);
    if (ec) {
        error = "could not create " + ApexUtil::ToUtf8(std::filesystem::path(path).parent_path().wstring()) + ": " + ec.message();
        return false;
    }
    std::string why;
    if (!ApexUtil::WriteFileAtomic(path, bytes, &why)) {
        error = "could not write " + ApexUtil::ToUtf8(path) + ": " + why;
        return false;
    }
    return true;
}

bool WriteIfDifferent(const std::wstring& path, const std::string& text, std::string& error) {
    std::string current;
    if (ApexUtil::ReadFileBytes(path, current) && current == text) return true;
    return WriteOut(path, text, error);
}

template <typename T> struct ComPtr {
    T* p = nullptr;
    ~ComPtr() {
        if (p) p->Release();
    }
    T** operator&() { return &p; }
    T* operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

} // namespace

std::wstring LowerW(std::wstring s) {
    for (wchar_t& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

std::string Lower(std::string s) {
    for (char& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return s;
}

std::wstring Sub(const wchar_t* name) { return EffectsFolder() + name + L"\\"; }

bool EnsureInternalHeaders(std::string& error) {
    return WriteIfDifferent(Sub(L"Internal") + L"ReShade.fxh", kReShadeFxh, error) && WriteIfDifferent(Sub(L"Internal") + L"ReShadeUI.fxh", ReShadeUiFxh(), error);
}

std::vector<std::filesystem::path> ScanEffects() {
    std::vector<std::filesystem::path> out;
    std::error_code ec;
    const std::filesystem::path folder = Sub(L"Shaders");
    if (!std::filesystem::is_directory(folder, ec)) return out;
    for (auto it = std::filesystem::recursive_directory_iterator(folder, std::filesystem::directory_options::skip_permission_denied, ec);
         !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec) || LowerW(it->path().extension().wstring()) != L".fx") continue;
        const std::wstring name = LowerW(it->path().filename().wstring());
        if (std::none_of(out.begin(), out.end(), [&](const auto& p) { return LowerW(p.filename().wstring()) == name; })) out.push_back(it->path());
    }
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return LowerW(a.filename().wstring()) < LowerW(b.filename().wstring()); });
    return out;
}

std::vector<std::filesystem::path> IncludeFolders() {
    std::vector<std::filesystem::path> out;
    std::error_code ec;
    const std::filesystem::path folder = Sub(L"Shaders");
    if (std::filesystem::is_directory(folder, ec)) {
        out.push_back(folder);
        for (auto it = std::filesystem::recursive_directory_iterator(folder, std::filesystem::directory_options::skip_permission_denied, ec);
             !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
            if (it->is_directory(ec)) out.push_back(it->path());
    }
    out.push_back(Sub(L"Internal"));
    return out;
}

std::filesystem::path FindTexture(const std::string& source) {
    if (source.empty()) return {};
    std::wstring rel = ApexUtil::ToWide(source);
    std::replace(rel.begin(), rel.end(), L'/', L'\\');
    if (rel.find(L"..") != std::wstring::npos || rel.find(L':') != std::wstring::npos) return {};
    const std::wstring wanted = LowerW(std::filesystem::path(rel).filename().wstring());
    std::error_code ec;
    for (const wchar_t* sub : {L"Textures", L"Shaders"}) {
        const std::filesystem::path folder = Sub(sub);
        if (std::filesystem::is_regular_file(folder / rel, ec)) return folder / rel;
        if (!std::filesystem::is_directory(folder, ec)) continue;
        for (auto it = std::filesystem::recursive_directory_iterator(folder, std::filesystem::directory_options::skip_permission_denied, ec);
             !ec && it != std::filesystem::recursive_directory_iterator(); it.increment(ec))
            if (it->is_regular_file(ec) && LowerW(it->path().filename().wstring()) == wanted) return it->path();
    }
    return {};
}

bool LoadImageFile(const std::filesystem::path& path, uint32_t width, uint32_t height, Image& out, std::string& error) {
    const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    struct ComScope {
        HRESULT hr;
        ~ComScope() {
            if (SUCCEEDED(hr)) CoUninitialize();
        }
    } comScope{com};
    ComPtr<IWICImagingFactory> factory;
    if (FAILED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)))) {
        error = "Windows Imaging Component is not available";
        return false;
    }
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(factory->CreateDecoderFromFilename(path.c_str(), nullptr, GENERIC_READ, WICDecodeMetadataCacheOnDemand, &decoder))) {
        error = "could not read " + ApexUtil::ToUtf8(path.filename().wstring()) + " (not an image Windows can open)";
        return false;
    }
    ComPtr<IWICBitmapFrameDecode> frame;
    if (FAILED(decoder->GetFrame(0, &frame))) {
        error = "could not decode " + ApexUtil::ToUtf8(path.filename().wstring());
        return false;
    }
    ComPtr<IWICFormatConverter> converter;
    if (FAILED(factory->CreateFormatConverter(&converter)) ||
        FAILED(converter->Initialize(frame.p, GUID_WICPixelFormat32bppBGRA, WICBitmapDitherTypeNone, nullptr, 0.0, WICBitmapPaletteTypeCustom))) {
        error = "could not convert " + ApexUtil::ToUtf8(path.filename().wstring());
        return false;
    }
    UINT w = 0, h = 0;
    converter->GetSize(&w, &h);
    IWICBitmapSource* source = converter.p;
    ComPtr<IWICBitmapScaler> scaler;
    if (width && height && (w != width || h != height)) {
        if (FAILED(factory->CreateBitmapScaler(&scaler)) || FAILED(scaler->Initialize(converter.p, width, height, WICBitmapInterpolationModeFant))) {
            error = "could not resize " + ApexUtil::ToUtf8(path.filename().wstring());
            return false;
        }
        source = scaler.p;
        w = width;
        h = height;
    }
    if (static_cast<unsigned long long>(w) * h > 64ull * 1024 * 1024) {
        error = ApexUtil::ToUtf8(path.filename().wstring()) + " is too large";
        return false;
    }
    out.width = w;
    out.height = h;
    out.bgra.resize(static_cast<size_t>(w) * h * 4);
    if (FAILED(source->CopyPixels(nullptr, w * 4, static_cast<UINT>(out.bgra.size()), out.bgra.data()))) {
        error = "could not decode " + ApexUtil::ToUtf8(path.filename().wstring());
        out.bgra.clear();
        return false;
    }
    return true;
}

uint64_t Hash64(const std::string& a, const std::string& b, const std::string& c) {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](const std::string& s) {
        for (unsigned char ch : s) h = (h ^ ch) * 1099511628211ull;
        h = (h ^ 0xFF) * 1099511628211ull;
    };
    mix(a);
    mix(b);
    mix(c);
    return h;
}

bool ReadCache(uint64_t hash, std::string& bytecode) {
    return ApexUtil::ReadFileBytes(Sub(L"Cache") + std::format(L"{:016x}.cso", hash), bytecode) && !bytecode.empty();
}

void WriteCache(uint64_t hash, const std::string& bytecode) {
    std::string error;
    WriteOut(Sub(L"Cache") + std::format(L"{:016x}.cso", hash), bytecode, error);
}

} // namespace Detail

using namespace Detail;

std::wstring EffectsFolder() { return ApexPaths::ApexDirectory() + L"FX\\"; }

int AddFiles(const std::vector<std::wstring>& paths, std::string& error) {
    int added = 0;
    std::error_code ec;
    for (const wchar_t* sub : {L"Shaders", L"Textures", L"Presets"}) std::filesystem::create_directories(Sub(sub), ec);
    for (const std::wstring& path : paths) {
        const std::wstring file = std::filesystem::path(path).filename().wstring();
        try {
            if (HasExtension(file, {L".zip"})) {
                std::string bytes;
                const auto size = std::filesystem::file_size(path, ec);
                if (ec || size > kMaxPackBytes) throw std::runtime_error(ec ? "could not read it" : "the pack is too large");
                if (!ApexUtil::ReadFileBytes(path, bytes)) throw std::runtime_error("could not read it");
                int fromPack = 0;
                for (const ZipEntry& e : ReadZip(bytes)) {
                    const std::wstring dest = Destination(e.name);
                    if (dest.empty()) continue;
                    if (!WriteOut(dest, e.bytes, error)) return added;
                    fromPack++;
                }
                if (fromPack == 0) throw std::runtime_error("no shaders, textures or presets inside");
                LOG_INFO(std::format("[ReShade] Added {} files from {}", fromPack, ApexUtil::ToUtf8(file)));
                added += fromPack;
                continue;
            }
            const std::wstring dest = Destination(ApexUtil::ToUtf8(file));
            if (dest.empty()) throw std::runtime_error("not an effect, texture or preset file");
            std::string bytes;
            const auto size = std::filesystem::file_size(path, ec);
            if (ec || size > kMaxFileBytes || !ApexUtil::ReadFileBytes(path, bytes)) throw std::runtime_error("could not read it");
            if (!WriteOut(dest, bytes, error)) return added;
            LOG_INFO("[ReShade] Added " + ApexUtil::ToUtf8(dest));
            added++;
        } catch (const std::exception& e) {
            error = ApexUtil::ToUtf8(file) + ": " + e.what();
            LOG_WARNING("[ReShade] Could not add " + error);
            return added;
        }
    }
    return added;
}

} // namespace ReShadeFx
