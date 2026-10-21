// Apex Radiance's own memory by owner (see apex_memory.h). Credits: @loinyx
#include "apex_memory.h"
#include "apex_log.h"
#include "high_alloc.h"
#include "outdoor_delta.h"
#include <d3d9.h>
#include <intrin.h>
#include <psapi.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <format>
#include <mutex>
#include <new>
#include <new.h>

#pragma intrinsic(_ReturnAddress)

namespace {

// ---- the C++ heap (development flavour) ----
// Each block: an 8-byte header (requested size, site index, tag) before the bytes operator new returns, so the returned
// pointer keeps malloc's 8-byte alignment. Sites: open addressing on the caller's address; 0 = the table was full,
// 1 = ImGui (ImGuiAlloc).
struct Header {
    uint32_t size;
    uint16_t site;
    uint16_t tag;
};
static_assert(sizeof(Header) == 8);
constexpr uint16_t kTag = 0xA9E5;
constexpr unsigned kSites = 4096; // power of two
constexpr uint16_t kSiteFull = 0, kSiteImGui = 1;
struct SiteRec {
    std::atomic<uintptr_t> ret{0};
    std::atomic<int64_t> bytes{0};
    std::atomic<int32_t> blocks{0};
};
SiteRec g_sites[kSites]; // constant-initialised: usable before any constructor runs
std::atomic<int64_t> g_heapBytes{0}, g_heapBlocks{0}, g_heapPeak{0};

uint16_t SiteOf(uintptr_t ret) {
    unsigned h = static_cast<unsigned>((static_cast<uint32_t>(ret) * 2654435761u) >> 20) & (kSites - 1);
    for (int probe = 0; probe < 64; probe++, h = (h + 1) & (kSites - 1)) {
        if (h <= kSiteImGui) continue;
        uintptr_t cur = g_sites[h].ret.load(std::memory_order_relaxed);
        if (cur == ret) return static_cast<uint16_t>(h);
        if (cur == 0) {
            if (g_sites[h].ret.compare_exchange_strong(cur, ret, std::memory_order_relaxed)) return static_cast<uint16_t>(h);
            if (cur == ret) return static_cast<uint16_t>(h);
        }
    }
    return kSiteFull;
}

void* TaggedAlloc(size_t n, uint16_t site) noexcept {
    if (n > 0xFFFFFFF0u - sizeof(Header)) return nullptr;
    auto* h = static_cast<Header*>(std::malloc(n + sizeof(Header)));
    if (!h) return nullptr;
    h->size = static_cast<uint32_t>(n);
    h->site = site;
    h->tag = kTag;
    SiteRec& s = g_sites[site];
    s.bytes.fetch_add(static_cast<int64_t>(n), std::memory_order_relaxed);
    s.blocks.fetch_add(1, std::memory_order_relaxed);
    const int64_t now = g_heapBytes.fetch_add(static_cast<int64_t>(n), std::memory_order_relaxed) + static_cast<int64_t>(n);
    g_heapBlocks.fetch_add(1, std::memory_order_relaxed);
    int64_t peak = g_heapPeak.load(std::memory_order_relaxed);
    while (now > peak && !g_heapPeak.compare_exchange_weak(peak, now, std::memory_order_relaxed)) {
    }
    return h + 1;
}
void TaggedFree(void* p) noexcept {
    if (!p) return;
    Header* h = static_cast<Header*>(p) - 1;
    if (h->tag == kTag) {
        SiteRec& s = g_sites[h->site < kSites ? h->site : kSiteFull];
        s.bytes.fetch_sub(static_cast<int64_t>(h->size), std::memory_order_relaxed);
        s.blocks.fetch_sub(1, std::memory_order_relaxed);
        g_heapBytes.fetch_sub(static_cast<int64_t>(h->size), std::memory_order_relaxed);
        g_heapBlocks.fetch_sub(1, std::memory_order_relaxed);
        h->tag = 0;
    }
    std::free(h);
}

// ---- named owners ----
constexpr int kSlots = 256;
ApexMemory::Slot g_slots[kSlots];
std::atomic<int> g_slotCount{0};
SRWLOCK g_slotLock = SRWLOCK_INIT;
std::atomic<uint32_t> g_request{0};

struct Released {
    const char* what;
    std::atomic<uint64_t> bytes{0}, times{0};
};
constexpr int kReleased = 32;
Released g_released[kReleased];
std::atomic<int> g_releasedCount{0};

// ---- Direct3D resources ----
// {6A5E1D2B-3C4F-4E8A-9B7D-41A9E5C0D3F1}
const GUID kTrackerGuid = {0x6a5e1d2b, 0x3c4f, 0x4e8a, {0x9b, 0x7d, 0x41, 0xa9, 0xe5, 0xc0, 0xd3, 0xf1}};
constexpr int kPools = 5; // D3DPOOL_DEFAULT, MANAGED, SYSTEMMEM, SCRATCH, render targets (default pool, own line)
const char* const kPoolNames[kPools] = {"default pool (video memory)", "managed pool (a RAM copy and video memory)", "system memory pool", "scratch pool",
                                        "render targets"};
std::atomic<int64_t> g_d3dBytes[kPools]{}, g_d3dCount[kPools]{};
std::atomic<int64_t> g_d3dCreated{0}, g_d3dUntracked{0};
constexpr unsigned kD3DSites = 128;
struct D3DSite {
    std::atomic<uintptr_t> ret{0};
    std::atomic<int64_t> bytes{0};
    std::atomic<int32_t> count{0};
};
D3DSite g_d3dSites[kD3DSites];
std::atomic<int64_t> g_psCreated{0}, g_vsCreated{0}, g_psBytes{0}, g_vsBytes{0};

unsigned D3DSiteOf(uintptr_t ret) {
    unsigned h = static_cast<unsigned>((static_cast<uint32_t>(ret) * 2654435761u) >> 25) & (kD3DSites - 1);
    for (unsigned probe = 0; probe < kD3DSites; probe++, h = (h + 1) & (kD3DSites - 1)) {
        uintptr_t cur = g_d3dSites[h].ret.load(std::memory_order_relaxed);
        if (cur == ret) return h;
        if (cur == 0) {
            if (g_d3dSites[h].ret.compare_exchange_strong(cur, ret, std::memory_order_relaxed)) return h;
            if (cur == ret) return h;
        }
    }
    return 0; // (shared with whichever site holds slot 0)
}

class Tracker final : public IUnknown {
public:
    Tracker(int pool, int64_t bytes, unsigned site) : pool_(pool), bytes_(bytes), site_(site) {
        g_d3dBytes[pool_].fetch_add(bytes_, std::memory_order_relaxed);
        g_d3dCount[pool_].fetch_add(1, std::memory_order_relaxed);
        g_d3dSites[site_].bytes.fetch_add(bytes_, std::memory_order_relaxed);
        g_d3dSites[site_].count.fetch_add(1, std::memory_order_relaxed);
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** out) override {
        if (!out) return E_POINTER;
        if (riid == __uuidof(IUnknown)) {
            *out = static_cast<IUnknown*>(this);
            AddRef();
            return S_OK;
        }
        *out = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return refs_.fetch_add(1, std::memory_order_relaxed) + 1; }
    ULONG STDMETHODCALLTYPE Release() override {
        const ULONG n = refs_.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (n == 0) {
            g_d3dBytes[pool_].fetch_sub(bytes_, std::memory_order_relaxed);
            g_d3dCount[pool_].fetch_sub(1, std::memory_order_relaxed);
            g_d3dSites[site_].bytes.fetch_sub(bytes_, std::memory_order_relaxed);
            g_d3dSites[site_].count.fetch_sub(1, std::memory_order_relaxed);
            delete this;
        }
        return n;
    }

private:
    std::atomic<ULONG> refs_{1};
    int pool_;
    int64_t bytes_;
    unsigned site_;
};

uint64_t SurfaceBytes(UINT w, UINT h, int format) {
    w = std::max<UINT>(w, 1), h = std::max<UINT>(h, 1);
    switch (static_cast<D3DFORMAT>(format)) {
    case D3DFMT_DXT1: return uint64_t{(w + 3) / 4} * ((h + 3) / 4) * 8;
    case D3DFMT_DXT2: case D3DFMT_DXT3: case D3DFMT_DXT4: case D3DFMT_DXT5: return uint64_t{(w + 3) / 4} * ((h + 3) / 4) * 16;
    case D3DFMT_A8: case D3DFMT_L8: case D3DFMT_P8: case D3DFMT_R3G3B2: case D3DFMT_A4L4: return uint64_t{w} * h;
    case D3DFMT_R5G6B5: case D3DFMT_X1R5G5B5: case D3DFMT_A1R5G5B5: case D3DFMT_A4R4G4B4: case D3DFMT_X4R4G4B4: case D3DFMT_A8L8: case D3DFMT_V8U8:
    case D3DFMT_L16: case D3DFMT_R16F: case D3DFMT_D16: case D3DFMT_D16_LOCKABLE: case D3DFMT_A8R3G3B2: case D3DFMT_L6V5U5: return uint64_t{w} * h * 2;
    case D3DFMT_R8G8B8: return uint64_t{w} * h * 3;
    case D3DFMT_A16B16G16R16: case D3DFMT_A16B16G16R16F: case D3DFMT_G32R32F: case D3DFMT_Q16W16V16U16: return uint64_t{w} * h * 8;
    case D3DFMT_A32B32G32R32F: return uint64_t{w} * h * 16;
    default: return uint64_t{w} * h * 4; // the 32-bit formats (and any other: an estimate)
    }
}

void Track(void* caller, IDirect3DResource9* res, int pool, uint64_t bytes) {
    g_d3dCreated.fetch_add(1, std::memory_order_relaxed);
    Tracker* t = new (std::nothrow) Tracker(pool, static_cast<int64_t>(bytes), D3DSiteOf(reinterpret_cast<uintptr_t>(caller)));
    if (!t) {
        g_d3dUntracked.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    // D3DSPD_IUNKNOWN: the resource keeps a reference and releases it when it is destroyed (native Direct3D 9 and DXVK)
    if (FAILED(res->SetPrivateData(kTrackerGuid, static_cast<IUnknown*>(t), sizeof(IUnknown*), D3DSPD_IUNKNOWN))) g_d3dUntracked.fetch_add(1, std::memory_order_relaxed);
    t->Release(); // ours; a refused SetPrivateData leaves none, so the counts go back at once
}

uintptr_t g_asiBase = 0, g_asiEnd = 0;
void FindAsi() {
    if (g_asiBase) return;
    HMODULE mod = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, reinterpret_cast<LPCWSTR>(&FindAsi), &mod)) return;
    MODULEINFO mi{};
    if (!GetModuleInformation(GetCurrentProcess(), mod, &mi, sizeof mi)) return;
    g_asiEnd = reinterpret_cast<uintptr_t>(mi.lpBaseOfDll) + mi.SizeOfImage;
    g_asiBase = reinterpret_cast<uintptr_t>(mi.lpBaseOfDll);
}
std::string Where(uintptr_t a) {
    FindAsi();
    if (a >= g_asiBase && a < g_asiEnd) return std::format("ApexRadiance.asi+0x{:X}", a - g_asiBase);
    return std::format("{:08X}", a);
}

double MiB(int64_t b) { return static_cast<double>(b) / (1024.0 * 1024.0); }

struct SlotView {
    std::string group, name;
    uint64_t bytes, items, peak;
    DWORD at;
};
std::vector<SlotView> Slots() {
    std::vector<SlotView> v;
    const int n = g_slotCount.load(std::memory_order_acquire);
    for (int i = 0; i < n; i++) {
        const ApexMemory::Slot& s = g_slots[i];
        v.push_back(SlotView{s.group, s.name, s.bytes.load(), s.items.load(), s.peak.load(), s.at.load()});
    }
    std::sort(v.begin(), v.end(), [](const SlotView& a, const SlotView& b) { return a.bytes > b.bytes; });
    return v;
}

} // namespace

// ---- operator new / delete of the ASI (development flavour only) ----
#ifdef APEX_DEVELOPMENT_BUILD_FLAVOR
void* __cdecl operator new(size_t n) {
    const uint16_t site = SiteOf(reinterpret_cast<uintptr_t>(_ReturnAddress()));
    for (;;) {
        if (void* p = TaggedAlloc(n, site)) return p;
        if (_callnewh(n) == 0) throw std::bad_alloc();
    }
}
void* __cdecl operator new[](size_t n) {
    const uint16_t site = SiteOf(reinterpret_cast<uintptr_t>(_ReturnAddress()));
    for (;;) {
        if (void* p = TaggedAlloc(n, site)) return p;
        if (_callnewh(n) == 0) throw std::bad_alloc();
    }
}
void* __cdecl operator new(size_t n, const std::nothrow_t&) noexcept {
    const uint16_t site = SiteOf(reinterpret_cast<uintptr_t>(_ReturnAddress()));
    for (;;) {
        if (void* p = TaggedAlloc(n, site)) return p;
        if (_callnewh(n) == 0) return nullptr;
    }
}
void* __cdecl operator new[](size_t n, const std::nothrow_t&) noexcept {
    const uint16_t site = SiteOf(reinterpret_cast<uintptr_t>(_ReturnAddress()));
    for (;;) {
        if (void* p = TaggedAlloc(n, site)) return p;
        if (_callnewh(n) == 0) return nullptr;
    }
}
void __cdecl operator delete(void* p) noexcept { TaggedFree(p); }
void __cdecl operator delete[](void* p) noexcept { TaggedFree(p); }
void __cdecl operator delete(void* p, size_t) noexcept { TaggedFree(p); }
void __cdecl operator delete[](void* p, size_t) noexcept { TaggedFree(p); }
void __cdecl operator delete(void* p, const std::nothrow_t&) noexcept { TaggedFree(p); }
void __cdecl operator delete[](void* p, const std::nothrow_t&) noexcept { TaggedFree(p); }
#endif

namespace ApexMemory {

bool Enabled() { return DevSwitches::MemoryOwners(); }

Slot* Register(const char* group, const char* name) {
    AcquireSRWLockExclusive(&g_slotLock);
    const int n = g_slotCount.load(std::memory_order_relaxed);
    Slot* found = nullptr;
    for (int i = 0; i < n && !found; i++)
        if (std::strcmp(g_slots[i].group, group) == 0 && std::strcmp(g_slots[i].name, name) == 0) found = &g_slots[i];
    if (!found && n < kSlots) {
        found = &g_slots[n];
        found->group = group;
        found->name = name;
        g_slotCount.store(n + 1, std::memory_order_release);
    }
    ReleaseSRWLockExclusive(&g_slotLock);
    static Slot s_overflow;
    if (!found) s_overflow.group = "other", s_overflow.name = "(more owners than slots)";
    return found ? found : &s_overflow;
}

void Set(Slot* slot, uint64_t bytes, uint64_t items) {
    if (!slot) return;
    slot->bytes.store(bytes, std::memory_order_relaxed);
    slot->items.store(items, std::memory_order_relaxed);
    if (bytes > slot->peak.load(std::memory_order_relaxed)) slot->peak.store(bytes, std::memory_order_relaxed);
    slot->at.store(GetTickCount() | 1, std::memory_order_relaxed);
}

void NoteReleased(const char* what, uint64_t bytes) {
    if (!DevSwitches::kCompiled) return;
    int n = g_releasedCount.load(std::memory_order_acquire);
    for (int i = 0; i < n; i++)
        if (g_released[i].what == what || std::strcmp(g_released[i].what, what) == 0) {
            g_released[i].bytes.fetch_add(bytes, std::memory_order_relaxed);
            g_released[i].times.fetch_add(1, std::memory_order_relaxed);
            return;
        }
    AcquireSRWLockExclusive(&g_slotLock);
    n = g_releasedCount.load(std::memory_order_relaxed);
    int i = 0;
    while (i < n && std::strcmp(g_released[i].what, what) != 0) i++;
    if (i == n && n < kReleased) {
        g_released[n].what = what;
        g_releasedCount.store(n + 1, std::memory_order_release);
    }
    ReleaseSRWLockExclusive(&g_slotLock);
    if (i < kReleased) {
        g_released[i].bytes.fetch_add(bytes, std::memory_order_relaxed);
        g_released[i].times.fetch_add(1, std::memory_order_relaxed);
    }
}

bool Cadence::Due() {
    if (!Enabled()) return false;
    const DWORD now = GetTickCount();
    const uint32_t req = g_request.load(std::memory_order_relaxed);
    if (started_ && req == seenRequest_ && now - last_ < 10000) return false;
    last_ = now, started_ = true;
    seenRequest_ = req;
    return true;
}
void RequestNow() { g_request.fetch_add(1, std::memory_order_relaxed); }

bool CallerInAsi(const void* address) {
    FindAsi();
    const uintptr_t a = reinterpret_cast<uintptr_t>(address);
    return a >= g_asiBase && a < g_asiEnd;
}

void NoteTexture(void* caller, IDirect3DBaseTexture9* tex, UINT w, UINT h, UINT levels, DWORD usage, int format, int pool) {
    if (!DevSwitches::kCompiled || !tex || !CallerInAsi(caller)) return;
    uint64_t bytes = 0;
    const UINT n = levels ? levels : tex->GetLevelCount();
    for (UINT i = 0; i < n; i++) bytes += SurfaceBytes(std::max<UINT>(w >> i, 1), std::max<UINT>(h >> i, 1), format);
    const int p = (usage & (D3DUSAGE_RENDERTARGET | D3DUSAGE_DEPTHSTENCIL)) ? 4 : std::clamp(pool, 0, 3);
    Track(caller, tex, p, bytes);
}
void NoteSurface(void* caller, IDirect3DResource9* surface, UINT w, UINT h, int format, bool renderTarget) {
    if (!DevSwitches::kCompiled || !surface || !CallerInAsi(caller)) return;
    Track(caller, surface, renderTarget ? 4 : 0, SurfaceBytes(w, h, format));
}
void NoteShader(void* caller, const DWORD* fn, bool pixel) {
    if (!DevSwitches::kCompiled || !fn || !CallerInAsi(caller)) return;
    size_t n = 1;
    __try {
        while (n < (1u << 20) && fn[n - 1] != 0x0000FFFF) n++;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        n = 0;
    }
    (pixel ? g_psCreated : g_vsCreated).fetch_add(1, std::memory_order_relaxed);
    (pixel ? g_psBytes : g_vsBytes).fetch_add(static_cast<int64_t>(n * 4), std::memory_order_relaxed);
}

void* ImGuiAlloc(size_t bytes, void*) {
    if constexpr (DevSwitches::kCompiled) return TaggedAlloc(bytes, kSiteImGui);
    else return std::malloc(bytes);
}
void ImGuiFree(void* p, void*) {
    if constexpr (DevSwitches::kCompiled) TaggedFree(p);
    else std::free(p);
}

HeapTotals Heap() { return HeapTotals{g_heapBytes.load(), g_heapBlocks.load(), g_heapPeak.load()}; }

std::string ReportText() {
    if (!DevSwitches::kCompiled) return "";
    std::string s = "\n==== APEX RADIANCE MEMORY (development build, memory_owners) ====\n";
    if (!Enabled()) return s + "Off (memory_owners = false in [dev_build]).\n";
    PROCESS_MEMORY_COUNTERS_EX pm{};
    pm.cb = sizeof pm;
    GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&pm), sizeof pm);
    const HeapTotals h = Heap();
    const int64_t imgui = g_sites[kSiteImGui].bytes.load();
    s += std::format("Process: private {:.1f} MiB, working set {:.1f} MiB\n", MiB(static_cast<int64_t>(pm.PrivateUsage)), MiB(static_cast<int64_t>(pm.WorkingSetSize)));
    s += std::format("C++ heap of the ASI (every operator new, exact): {:.2f} MiB live in {} blocks, session peak {:.2f} MiB; accounting headers {:.2f} MiB "
                     "(8 bytes a block, development build only); of the live bytes ImGui (menu, fonts) {:.2f} MiB\n",
                     MiB(h.bytes), h.blocks, MiB(h.peakBytes), MiB(h.blocks * 8), MiB(imgui));
    s += std::format("Large buffers above 2 GB (ApexMem::AllocHigh: Quick outdoor lamp edits, fast DXT): {:.2f} MiB; Quick outdoor lamp edits' own count of its room "
                     "parts: {:.2f} MiB (the high buffers and its smaller ones on the C++ heap)\n",
                     MiB(static_cast<int64_t>(ApexMem::g_highBytes.load())), MiB(static_cast<int64_t>(OutdoorDelta::KeptBytes())));

    // named owners
    const std::vector<SlotView> slots = Slots();
    uint64_t named = 0;
    for (const SlotView& v : slots) named += v.bytes;
    const DWORD now = GetTickCount();
    s += std::format("\nNamed owners (estimates: capacity x element size plus the containers' nodes; {} owners, {:.2f} MiB together, {:.0f}% of the C++ heap):\n", slots.size(),
                     MiB(static_cast<int64_t>(named)), h.bytes > 0 ? 100.0 * static_cast<double>(named) / static_cast<double>(h.bytes) : 0.0);
    for (const SlotView& v : slots)
        s += std::format("   {:9.3f} MiB  peak {:9.3f}  {:>8} items  {:<16} {}{}\n", MiB(static_cast<int64_t>(v.bytes)), MiB(static_cast<int64_t>(v.peak)), v.items, v.group, v.name,
                         v.at ? std::format(" ({:.0f} s ago)", std::max(0, static_cast<int32_t>(now - v.at)) / 1000.0) : " (not measured yet)");

    // heap sites
    struct SiteView {
        unsigned index;
        uintptr_t ret;
        int64_t bytes;
        int32_t blocks;
    };
    std::vector<SiteView> sites;
    for (unsigned i = 0; i < kSites; i++) {
        const int64_t b = g_sites[i].bytes.load(std::memory_order_relaxed);
        if (b > 0) sites.push_back(SiteView{i, g_sites[i].ret.load(), b, g_sites[i].blocks.load()});
    }
    std::sort(sites.begin(), sites.end(), [](const SiteView& a, const SiteView& b) { return a.bytes > b.bytes; });
    s += std::format("\nC++ heap by the address that called operator new (live; {} sites; symbolise with the build's PDB):\n", sites.size());
    for (size_t i = 0; i < sites.size() && i < 40; i++)
        s += std::format("   {:9.3f} MiB  {:>7} blocks  {}\n", MiB(sites[i].bytes), sites[i].blocks,
                         sites[i].index == kSiteImGui ? std::string("ImGui (menu, fonts)") : sites[i].index == kSiteFull ? std::string("(site table full)") : Where(sites[i].ret));

    // Direct3D
    s += std::format("\nDirect3D resources the ASI created (textures, render targets; live; {} created since the start, {} not trackable):\n", g_d3dCreated.load(),
                     g_d3dUntracked.load());
    for (int p = 0; p < kPools; p++)
        if (g_d3dCount[p].load()) s += std::format("   {:9.2f} MiB  {:>5} x  {}\n", MiB(g_d3dBytes[p].load()), g_d3dCount[p].load(), kPoolNames[p]);
    std::vector<std::pair<uintptr_t, std::pair<int64_t, int32_t>>> ds;
    for (unsigned i = 0; i < kD3DSites; i++)
        if (g_d3dSites[i].count.load() > 0) ds.push_back({g_d3dSites[i].ret.load(), {g_d3dSites[i].bytes.load(), g_d3dSites[i].count.load()}});
    std::sort(ds.begin(), ds.end(), [](const auto& a, const auto& b) { return a.second.first > b.second.first; });
    for (size_t i = 0; i < ds.size() && i < 16; i++) s += std::format("      {:9.2f} MiB  {:>5} x  created at {}\n", MiB(ds[i].second.first), ds[i].second.second, Where(ds[i].first));
    s += std::format("   shaders created by the ASI since the start (bytecode; the driver's copies are not visible): pixel {} ({:.2f} MiB), vertex {} ({:.2f} MiB)\n",
                     g_psCreated.load(), MiB(g_psBytes.load()), g_vsCreated.load(), MiB(g_vsBytes.load()));
    s += "   not counted: depth-stencil surfaces, vertex buffers and system-memory read-back surfaces (rt_lamp_gpu, captures; created and released "
         "around a read, or experimental)\n";

    // released
    const int nr = g_releasedCount.load();
    if (nr) {
        s += "\nGiven back by the memory switches since the start:\n";
        for (int i = 0; i < nr; i++) s += std::format("   {:9.3f} MiB  {} times  {}\n", MiB(static_cast<int64_t>(g_released[i].bytes.load())), g_released[i].times.load(), g_released[i].what);
    }
    return s;
}

void LogLine() {
    if (!Enabled()) return;
    const HeapTotals h = Heap();
    const std::vector<SlotView> slots = Slots();
    uint64_t named = 0;
    std::string top;
    for (size_t i = 0; i < slots.size(); i++) {
        named += slots[i].bytes;
        if (i < 8) top += std::format("{}{} {:.2f}", i ? ", " : "", slots[i].name, MiB(static_cast<int64_t>(slots[i].bytes)));
    }
    int64_t d3d = 0;
    for (int p = 0; p < kPools; p++) d3d += g_d3dBytes[p].load();
    uint64_t released = 0;
    for (int i = 0; i < g_releasedCount.load(); i++) released += g_released[i].bytes.load();
    LOG_INFO(std::format("[ApexMemory] C++ heap {:.1f} MiB in {} blocks (peak {:.1f}, ImGui {:.1f}), named owners {:.1f} MiB, high buffers {:.1f} MiB, D3D resources {:.1f} MiB "
                         "(managed {:.1f}, default {:.1f}, render targets {:.1f}), given back by the memory switches {:.1f} MiB | top: {}",
                         MiB(h.bytes), h.blocks, MiB(h.peakBytes), MiB(g_sites[kSiteImGui].bytes.load()), MiB(static_cast<int64_t>(named)),
                         MiB(static_cast<int64_t>(ApexMem::g_highBytes.load())), MiB(d3d), MiB(g_d3dBytes[1].load()), MiB(g_d3dBytes[0].load()), MiB(g_d3dBytes[4].load()),
                         MiB(static_cast<int64_t>(released)), top));
}
void LogLineEvery(DWORD ms) {
    static std::atomic<DWORD> s_last{0};
    static std::atomic<bool> s_started{false};
    const DWORD now = GetTickCount();
    if (s_started.load(std::memory_order_relaxed) && now - s_last.load(std::memory_order_relaxed) < ms) return;
    s_last.store(now, std::memory_order_relaxed);
    s_started.store(true, std::memory_order_relaxed);
    LogLine();
}

} // namespace ApexMemory
