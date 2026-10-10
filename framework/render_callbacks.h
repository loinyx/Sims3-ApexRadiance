#pragma once
// Callback lists fired by the D3D9 bootstrap at points the D3D9Hooks registry does not cover: the end of the game's
// frame before the Apex overlay draws, and around IDirect3DDevice9::Reset (features release and recreate their
// D3DPOOL_DEFAULT resources there). The lists grow as needed (the first version had 4 fixed slots and dropped a fifth
// preReset user silently, so its resources survived the Reset and made it fail).
#include <d3d9.h>
#include <algorithm>
#include <iterator>
#include <mutex>
#include <string>
#include <vector>
#include "apex_log.h"
#include "hook_guard.h"

namespace RenderCallbacks {

using DeviceFn = void (*)(IDirect3DDevice9*);

class CallbackList {
  public:
    explicit CallbackList(const char* name, bool perFrame = true) : name_(name), perFrame_(perFrame) {}

    void Add(DeviceFn fn) {
        if (!fn) return;
        std::lock_guard<std::mutex> lock(mutex_);
        if (std::find(fns_.begin(), fns_.end(), fn) != fns_.end()) return;
        fns_.push_back(fn);
        LOG_DEBUG(std::string("[RenderCallbacks] ") + name_ + ": " + std::to_string(fns_.size()) + " callbacks");
    }

    void Remove(DeviceFn fn) {
        std::lock_guard<std::mutex> lock(mutex_);
        fns_.erase(std::remove(fns_.begin(), fns_.end(), fn), fns_.end());
    }

    // Calls every callback, in the order they were added. The list is copied first: a callback may add or remove.
    // 07/10, players' Runtime Error: never throws into the game's EndScene / Reset. The copy goes to the stack (no heap for
    // the usual few callbacks), and a callback that throws is caught; on a per-frame list it is skipped from then on, on
    // the Reset lists it keeps running (a preReset release skipped for good would make every later Reset fail).
    void Fire(IDirect3DDevice9* device) noexcept {
        try {
            DeviceFn local[32];
            std::vector<DeviceFn> more;
            const DeviceFn* run = local;
            size_t n = 0;
            {
                std::lock_guard<std::mutex> lock(mutex_);
                if (fns_.empty()) return;
                n = fns_.size();
                if (n <= std::size(local)) std::copy(fns_.begin(), fns_.end(), local);
                else run = (more = fns_).data();
            }
            for (size_t i = 0; i < n; i++) {
                const DeviceFn fn = run[i];
                if (perFrame_ && off_.Has(reinterpret_cast<const void*>(fn))) continue;
                try {
                    fn(device);
                } catch (...) {
                    if (perFrame_) off_.Add(reinterpret_cast<const void*>(fn));
                    HookGuard::NoteAt(name_, reinterpret_cast<const void*>(fn));
                }
            }
        } catch (...) {
            HookGuard::Note(name_); // the list copy itself failed: nothing ran this time
        }
    }

  private:
    std::mutex mutex_;
    std::vector<DeviceFn> fns_;
    const char* name_;
    bool perFrame_;
    HookGuard::OffList<16> off_;
};

inline CallbackList endSceneBeforeOverlay{"endSceneBeforeOverlay"};
// Filtered report photos: fired explicitly after Picture, before Apex's overlay.
inline CallbackList filteredSceneBeforeOverlay{"filteredSceneBeforeOverlay"};
inline CallbackList preReset{"preReset", false};
inline CallbackList postReset{"postReset", false};
// Manual recovery releases only Apex screen effects, never game lighting or hooks.
inline CallbackList restartEffects{"restartEffects", false};

inline void Add(CallbackList& list, DeviceFn fn) { list.Add(fn); }
inline void Remove(CallbackList& list, DeviceFn fn) { list.Remove(fn); }
inline void Fire(CallbackList& list, IDirect3DDevice9* device) { list.Fire(device); }

} // namespace RenderCallbacks
