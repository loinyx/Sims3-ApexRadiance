// Layered hooks on game CALL instructions (see call_chain.h).
#include "call_chain.h"
#include "apex_log.h"
#include "game_addresses.h"
#include "memory_patch.h"
#include <windows.h>
#include <cstring>
#include <format>
#include <iterator>
#include <mutex>

namespace CallChain {

std::atomic<void*> g_next[static_cast<int>(Site::Count)][static_cast<int>(Layer::Count)] = {};

namespace {

constexpr int kSites = static_cast<int>(Site::Count);
constexpr int kLayers = static_cast<int>(Layer::Count);
constexpr int kCallLen = 5;

struct SiteInfo {
    const char* name;
    GameAddr::Id call;   // the CALL instruction
    GameAddr::Id callee; // the game function it reaches
};
// No branch in .text lands on bytes 1..4 of the CALL (checked in research\engine_map\full.asm).
const SiteInfo kSiteInfo[kSites] = {
    {"Scene::BeginFrame pending-node drain", GameAddr::Id::SceneDrainCall, GameAddr::Id::SceneDrain},
    {"texture compositor queue step", GameAddr::Id::CompQueueCall, GameAddr::Id::CompDispatch},
    {"texture builder state 2 (render and read back a tile)", GameAddr::Id::CompState2Call, GameAddr::Id::CompTileRender},
    {"texture builder state 3 (use the tile)", GameAddr::Id::CompState3Call, GameAddr::Id::CompTileRead},
    {"texture builder tile readback", GameAddr::Id::CompReadbackCall, GameAddr::Id::CompReadback},
    {"lot renderer load stages", GameAddr::Id::LotLoadStagesCall, GameAddr::Id::LotLoadStages},
};
static_assert(std::size(kSiteInfo) == static_cast<size_t>(Site::Count), "kSiteInfo must list every Site in order");

struct SiteState {
    uintptr_t call = 0;
    uintptr_t fn = 0;
    uint8_t orig[kCallLen] = {};
    void* hook[kLayers] = {}; // installed layers (nullptr = not installed)
};
SiteState g_state[kSites]; // guarded by g_mutex
std::mutex g_mutex;

void MakeCall(uint8_t out[kCallLen], uintptr_t from, uintptr_t to) {
    out[0] = 0xE8;
    const int32_t rel = static_cast<int32_t>(to - (from + kCallLen));
    std::memcpy(out + 1, &rel, 4);
}

uintptr_t TargetOf(const uint8_t bytes[kCallLen], uintptr_t at) {
    int32_t rel;
    std::memcpy(&rel, bytes + 1, 4);
    return at + kCallLen + static_cast<uintptr_t>(static_cast<intptr_t>(rel));
}

// The pointer the CALL targets now: the outermost installed layer's hook, else the game function
uintptr_t Outermost(const SiteState& s) {
    for (int l = 0; l < kLayers; l++)
        if (s.hook[l]) return reinterpret_cast<uintptr_t>(s.hook[l]);
    return s.fn;
}

// The next inner installed layer's hook after `layer`, else the game function
uintptr_t InnerOf(const SiteState& s, int layer) {
    for (int l = layer + 1; l < kLayers; l++)
        if (s.hook[l]) return reinterpret_cast<uintptr_t>(s.hook[l]);
    return s.fn;
}

// The nearest installed layer outside `layer`, or -1
int OuterOf(const SiteState& s, int layer) {
    for (int l = layer - 1; l >= 0; l--)
        if (s.hook[l]) return l;
    return -1;
}

bool Resolve(int site, std::string* error) {
    SiteState& s = g_state[site];
    const SiteInfo& info = kSiteInfo[site];
    if (s.call) return true;
    if (!GameAddr::Resolved()) {
        if (error) *error = "the game code was not scanned yet";
        return false;
    }
    const uintptr_t call = GameAddr::Get(info.call), fn = GameAddr::Get(info.callee);
    if (!call || !fn) {
        std::string missing = !call ? GameAddr::Name(info.call) : "";
        if (!fn) missing += std::string(missing.empty() ? "" : ", ") + GameAddr::Name(info.callee);
        if (error) *error = GameAddr::NotAvailable(missing);
        return false;
    }
    uint8_t cur[kCallLen] = {};
    if (!MemPatch::ReadBytes(call, cur, kCallLen)) {
        if (error) *error = std::format("the CALL {:#010x} is not readable", call);
        return false;
    }
    if (cur[0] != 0xE8 || TargetOf(cur, call) != fn) {
        if (error) *error = std::format("the CALL at {:#010x} does not reach {:#010x} (redirected by another module?)", call, fn);
        return false;
    }
    std::memcpy(s.orig, cur, kCallLen);
    s.call = call;
    s.fn = fn;
    return true;
}

// Rewrites the CALL to reach `to` if it still reaches `expect` (every other thread suspended, none inside the bytes)
bool WriteCall(const SiteState& s, uintptr_t to, uintptr_t expect, std::string* error) {
    uint8_t cur[kCallLen] = {};
    if (!MemPatch::ReadBytes(s.call, cur, kCallLen) || cur[0] != 0xE8 || TargetOf(cur, s.call) != expect) {
        if (error) *error = std::format("the CALL at {:#010x} was changed by another module", s.call);
        return false;
    }
    uint8_t bytes[kCallLen];
    if (to == s.fn) std::memcpy(bytes, s.orig, kCallLen);
    else MakeCall(bytes, s.call, to);
    if (!MemPatch::WriteCodeSuspended(s.call, bytes, kCallLen)) {
        if (error) *error = std::format("could not write the CALL at {:#010x} (a thread kept running it)", s.call);
        return false;
    }
    return true;
}

} // namespace

bool Install(Site site, Layer layer, void* hook, std::string* error) {
    const int si = static_cast<int>(site), li = static_cast<int>(layer);
    if (si < 0 || si >= kSites || li < 0 || li >= kLayers || !hook) {
        if (error) *error = "bad arguments";
        return false;
    }
    std::lock_guard<std::mutex> lock(g_mutex);
    SiteState& s = g_state[si];
    if (s.hook[li] == hook) return true;
    if (s.hook[li]) {
        if (error) *error = "another hook is installed for this layer";
        return false;
    }
    if (!Resolve(si, error)) return false;
    const uintptr_t inner = InnerOf(s, li);
    g_next[si][li].store(reinterpret_cast<void*>(inner), std::memory_order_release); // before anything can reach the hook
    const int o = OuterOf(s, li);
    if (o >= 0) {
        // An outer layer keeps the CALL: it now calls this one
        s.hook[li] = hook;
        g_next[si][o].store(hook, std::memory_order_release);
    } else {
        if (!WriteCall(s, reinterpret_cast<uintptr_t>(hook), Outermost(s), error)) return false;
        s.hook[li] = hook;
    }
    LOG_INFO(std::format("[CallChain] {} (CALL {:#010x} -> {:#010x}): layer {} installed (next = {:#010x}{})", kSiteInfo[si].name, s.call, s.fn, li, inner,
                         o >= 0 ? ", inside an outer layer" : (inner == s.fn ? ", the game's code" : "")));
    return true;
}

bool Remove(Site site, Layer layer) {
    const int si = static_cast<int>(site), li = static_cast<int>(layer);
    if (si < 0 || si >= kSites || li < 0 || li >= kLayers) return true;
    std::lock_guard<std::mutex> lock(g_mutex);
    SiteState& s = g_state[si];
    if (!s.hook[li]) return true;
    const uintptr_t inner = InnerOf(s, li);
    const int o = OuterOf(s, li);
    if (o >= 0) {
        g_next[si][o].store(reinterpret_cast<void*>(inner), std::memory_order_release);
    } else {
        std::string error;
        if (!WriteCall(s, inner, reinterpret_cast<uintptr_t>(s.hook[li]), &error)) {
            uint8_t cur[kCallLen] = {};
            if (MemPatch::ReadBytes(s.call, cur, kCallLen) && (cur[0] != 0xE8 || TargetOf(cur, s.call) != reinterpret_cast<uintptr_t>(s.hook[li]))) {
                // changed by someone else after us: left as it is (it may still reach the removed layer, which keeps forwarding)
                LOG_WARNING(std::format("[CallChain] {}: {}; left as it is", kSiteInfo[si].name, error));
            } else {
                LOG_ERROR(std::format("[CallChain] {}: layer {} not removed: {}", kSiteInfo[si].name, li, error));
                return false;
            }
        }
    }
    s.hook[li] = nullptr; // g_next[si][li] keeps its value for threads still inside the removed hook
    LOG_INFO(std::format("[CallChain] {}: layer {} removed", kSiteInfo[si].name, li));
    return true;
}

bool Installed(Site site, Layer layer) {
    const int si = static_cast<int>(site), li = static_cast<int>(layer);
    if (si < 0 || si >= kSites || li < 0 || li >= kLayers) return false;
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_state[si].hook[li] != nullptr;
}

uintptr_t CallAddress(Site site) {
    const int si = static_cast<int>(site);
    if (si < 0 || si >= kSites) return 0;
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_state[si].call;
}

uintptr_t GameFunction(Site site) {
    const int si = static_cast<int>(site);
    if (si < 0 || si >= kSites) return 0;
    std::lock_guard<std::mutex> lock(g_mutex);
    return g_state[si].fn;
}

} // namespace CallChain
