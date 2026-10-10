#include "apex_util.h"
#include <atomic>

namespace ApexUtil {

bool FileExists(const std::wstring& path) {
    const DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES && !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

bool ReadFileBytes(const std::wstring& path, std::string& out) {
    out.clear();
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) return false;
    struct FileGuard {
        HANDLE handle;
        ~FileGuard() { CloseHandle(handle); }
    } guard{f};
    LARGE_INTEGER size{};
    bool ok = GetFileSizeEx(f, &size) && size.QuadPart >= 0 && size.QuadPart < (64LL << 20);
    if (ok) {
        out.resize(static_cast<size_t>(size.QuadPart));
        DWORD read = 0;
        ok = out.empty() || (ReadFile(f, out.data(), static_cast<DWORD>(out.size()), &read, nullptr) && read == out.size());
    }
    if (!ok) out.clear();
    return ok;
}

bool WriteFileAtomic(const std::wstring& path, const std::string& data, std::string* error) {
    // CREATE_NEW protects existing neighbours and prevents writers sharing a temporary file.
    static std::atomic<unsigned long long> sequence{0};
    std::wstring temp;
    HANDLE f = INVALID_HANDLE_VALUE;
    for (unsigned attempt = 0; attempt < 128; ++attempt) {
        temp = path + L".tmp-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(GetTickCount64()) + L"-" + std::to_wstring(sequence.fetch_add(1));
        f = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (f != INVALID_HANDLE_VALUE || GetLastError() != ERROR_FILE_EXISTS) break;
    }
    if (f == INVALID_HANDLE_VALUE) {
        if (error) *error = "cannot create " + ToUtf8(temp) + " (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    DWORD written = 0;
    const bool wrote = data.empty() || (WriteFile(f, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) && written == data.size());
    const bool flushed = wrote && FlushFileBuffers(f);
    CloseHandle(f);
    if (!wrote || !flushed) {
        DeleteFileW(temp.c_str());
        if (error) *error = "cannot write " + ToUtf8(temp);
        return false;
    }
    if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD e = GetLastError();
        DeleteFileW(temp.c_str());
        if (error) *error = "cannot replace " + ToUtf8(path) + " (error " + std::to_string(e) + ")";
        return false;
    }
    return true;
}

} // namespace ApexUtil
