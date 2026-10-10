#pragma once
#include <windows.h>

namespace ModuleLifetime {

// Hooks and detached workers require their code to remain mapped for this process.
// This affects only the current process; Windows releases the image at process exit.
inline bool Pin(HMODULE module) noexcept {
    if (!module) return false;
    HMODULE pinned = nullptr;
    return GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                             reinterpret_cast<LPCWSTR>(module), &pinned) && pinned == module;
}

} // namespace ModuleLifetime
