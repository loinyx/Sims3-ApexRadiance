// Offline checks of Apex's own memory accounting (features/apex_memory.cpp, development flavour; dev switch memory_owners):
//  - the replaced operator new / delete count every block exactly (bytes, blocks, the session peak) and by calling site;
//  - the container estimates (ApexMemory::Est) match what the containers really allocate (MSVC x86 layout), within 1%;
//  - owner slots, cadence, the bytes given back and the report text;
//  - Direct3D 9 (a real HAL device on a hidden window): a texture or render target noted at creation is counted with its
//    pool and size, and subtracted when the device destroys it (private data released). The loaded d3d9.dll is printed:
//    the system one, or the game's DXVK placed next to the executable.
// Read-only: prints to stdout, returns non-zero on any failure.
#define APEX_DEVELOPMENT_BUILD_FLAVOR
#include "../../features/apex_memory.cpp"
#include <d3d9.h>
#include <cstdio>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

void ApexLog::Write(Level, const std::string& text, const std::source_location&) noexcept { std::printf("  log: %s\n", text.c_str()); }
size_t OutdoorDelta::KeptBytes() { return 0; }

namespace {
int g_checks = 0, g_failures = 0;
void Check(bool ok, const std::string& what) {
    g_checks++;
    if (!ok) {
        g_failures++;
        std::printf("FAIL: %s\n", what.c_str());
    }
}
// What the containers built in between allocated, as the estimates count it (8-byte heap header per block)
struct Probe {
    ApexMemory::HeapTotals before = ApexMemory::Heap();
    int64_t Counted() const {
        const ApexMemory::HeapTotals now = ApexMemory::Heap();
        return (now.bytes - before.bytes) + 8 * (now.blocks - before.blocks);
    }
};
void Near(uint64_t estimate, int64_t counted, const char* what) {
    const double tol = std::max(64.0, 0.01 * static_cast<double>(counted));
    const bool ok = std::abs(static_cast<double>(estimate) - static_cast<double>(counted)) <= tol;
    std::printf("  %-44s estimate %10llu  counted %10lld  %s\n", what, static_cast<unsigned long long>(estimate), static_cast<long long>(counted), ok ? "ok" : "DIFFERS");
    Check(ok, std::string("estimate of ") + what);
}
struct Rec {
    float a[5];
    int b;
};
__declspec(noinline) void* AllocHere(size_t n) { return ::operator new(n); }
__declspec(noinline) void* AllocThere(size_t n) { return ::operator new(n); }

void HeapChecks() {
    std::printf("C++ heap counters\n");
    DevSwitches::memoryOwners.store(true); // (the report is written only with memory_owners on)
    const ApexMemory::HeapTotals h0 = ApexMemory::Heap();
    char* p = new char[1000];
    ApexMemory::HeapTotals h1 = ApexMemory::Heap();
    Check(h1.bytes - h0.bytes == 1000 && h1.blocks - h0.blocks == 1, "new char[1000]: 1000 bytes, 1 block");
    Check(h1.peakBytes >= h1.bytes, "peak at least the live bytes");
    Check((reinterpret_cast<uintptr_t>(p) & 7) == 0, "8-byte alignment kept");
    delete[] p;
    h1 = ApexMemory::Heap();
    Check(h1.bytes == h0.bytes && h1.blocks == h0.blocks, "delete[] gives it back");
    void* z = ::operator new(0);
    Check(z != nullptr, "new(0) gives a pointer");
    ::operator delete(z);
    void* nt = ::operator new(64, std::nothrow);
    { const int64_t d = ApexMemory::Heap().bytes - h0.bytes; Check(d == 64, "nothrow new counted"); } // (measured before the message string is made)
    ::operator delete(nt, std::nothrow);
    // two call sites, two entries
    void* a = AllocHere(300000);
    void* b = AllocThere(200000);
    {
        const std::string report = ApexMemory::ReportText();
        Check(report.find("C++ heap of the ASI") != std::string::npos, "report has the heap line");
        int sites = 0;
        for (size_t at = report.find("ApexRadiance.asi+0x"); at != std::string::npos; at = report.find("ApexRadiance.asi+0x", at + 1)) sites++;
        Check(sites >= 2, "two calling sites listed");
        Check(report.find("0.286 MiB") != std::string::npos && report.find("0.191 MiB") != std::string::npos, "each site with its bytes");
    }
    ::operator delete(a);
    ::operator delete(b);
    // ImGui's allocations: counted apart and in the total
    void* g = ApexMemory::ImGuiAlloc(5000, nullptr);
    { const int64_t d = ApexMemory::Heap().bytes - h0.bytes; Check(d == 5000, "ImGui block counted"); }
    Check(ApexMemory::ReportText().find("ImGui (menu, fonts)") != std::string::npos, "ImGui site named");
    ApexMemory::ImGuiFree(g, nullptr);
    { const int64_t d = ApexMemory::Heap().bytes - h0.bytes; Check(d == 0, "ImGui block given back"); }
}

void EstimateChecks() {
    using namespace ApexMemory::Est;
    std::printf("Container estimates against the counted allocations\n");
    {
        Probe pr;
        std::vector<int> v;
        v.reserve(1000);
        Near(Vec(v), pr.Counted(), "vector<int> reserve 1000");
    }
    {
        Probe pr;
        std::vector<Rec> v;
        for (int i = 0; i < 777; i++) v.push_back(Rec{});
        Near(Vec(v), pr.Counted(), "vector<Rec> grown to 777");
    }
    {
        Probe pr;
        std::unordered_map<uint64_t, Rec> m;
        for (uint64_t i = 0; i < 10000; i++) m[i * 7919] = Rec{};
        Near(Hash(m), pr.Counted(), "unordered_map<uint64_t, Rec> 10000");
    }
    {
        Probe pr;
        std::unordered_map<uintptr_t, std::vector<uintptr_t>> m;
        for (uintptr_t i = 0; i < 3000; i++) m[i].assign(i % 17, i);
        Near(Hash(m, [](const auto& kv) { return Vec(kv.second); }), pr.Counted(), "unordered_map of vectors 3000");
    }
    {
        Probe pr;
        std::unordered_set<uintptr_t> s;
        for (uintptr_t i = 0; i < 5000; i++) s.insert(i * 13);
        Near(Hash(s), pr.Counted(), "unordered_set<uintptr_t> 5000");
    }
    {
        Probe pr;
        std::map<std::pair<uint64_t, int>, std::tuple<int, int, int, int>> m;
        for (int i = 0; i < 4000; i++) m[{static_cast<uint64_t>(i) * 3, i}] = {};
        Near(Tree(m), pr.Counted(), "map<pair, tuple> 4000");
    }
    {
        Probe pr;
        std::vector<std::string> v;
        for (int i = 0; i < 500; i++) v.push_back(std::string(static_cast<size_t>(i % 90), 'x'));
        Near(Vec(v, [](const std::string& s) { return Str(s); }), pr.Counted(), "vector<string> 500");
    }
    {
        Probe pr;
        std::vector<std::shared_ptr<const Rec>> v;
        v.reserve(200);
        for (int i = 0; i < 200; i++) v.push_back(std::make_shared<Rec>());
        Near(Vec(v) + 200 * Shared<Rec>(), pr.Counted(), "make_shared<Rec> x 200");
    }
}

void SlotChecks() {
    std::printf("Owner slots, cadence, given back\n");
    DevSwitches::memoryOwners.store(true);
    ApexMemory::Slot* s = ApexMemory::Register("Test", "owner A");
    Check(ApexMemory::Register("Test", "owner A") == s, "one slot per name");
    ApexMemory::Set(s, 3 << 20, 42);
    ApexMemory::Set(s, 1 << 20, 41);
    Check(s->bytes.load() == (1u << 20) && s->peak.load() == (3u << 20) && s->items.load() == 41, "bytes, items and peak");
    ApexMemory::Cadence c;
    Check(c.Due(), "first measure due");
    bool again = false;
    for (int i = 0; i < 2000 && !again; i++) again = c.Due(), Sleep(i % 100 == 0 ? 1 : 0); // (over several ticks: any tick parity)
    Check(!again, "not again within 10 s");
    ApexMemory::RequestNow();
    Check(c.Due(), "due again after an F8");
    DevSwitches::memoryOwners.store(false);
    Check(!c.Due() && !(ApexMemory::RequestNow(), c.Due()), "never due with memory_owners off");
    DevSwitches::memoryOwners.store(true);
    ApexMemory::NoteReleased("test buffers", 1000);
    ApexMemory::NoteReleased("test buffers", 500);
    const std::string r = ApexMemory::ReportText();
    Check(r.find("owner A") != std::string::npos && r.find("test buffers") != std::string::npos && r.find("2 times") != std::string::npos, "report lists owners and what was given back");
}

void D3DChecks() {
    std::printf("Direct3D resources\n");
    HMODULE d3d = LoadLibraryW(L"d3d9.dll");
    wchar_t path[MAX_PATH] = {};
    if (d3d) GetModuleFileNameW(d3d, path, MAX_PATH);
    std::printf("  d3d9.dll: %ls\n", path);
    WNDCLASSW wc{};
    wc.lpfnWndProc = DefWindowProcW;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"ApexMemoryOwnersCheck";
    RegisterClassW(&wc);
    HWND wnd = CreateWindowW(wc.lpszClassName, L"", WS_OVERLAPPEDWINDOW, 0, 0, 64, 64, nullptr, nullptr, wc.hInstance, nullptr);
    using Create_t = IDirect3D9*(WINAPI*)(UINT);
    const auto create = d3d ? reinterpret_cast<Create_t>(GetProcAddress(d3d, "Direct3DCreate9")) : nullptr;
    IDirect3D9* d = create ? create(D3D_SDK_VERSION) : nullptr;
    D3DPRESENT_PARAMETERS pp{};
    pp.Windowed = TRUE, pp.SwapEffect = D3DSWAPEFFECT_DISCARD, pp.BackBufferWidth = 64, pp.BackBufferHeight = 64, pp.hDeviceWindow = wnd;
    IDirect3DDevice9* dev = nullptr;
    if (!d || FAILED(d->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, wnd, D3DCREATE_HARDWARE_VERTEXPROCESSING, &pp, &dev)) || !dev) {
        Check(false, "Direct3D 9 HAL device");
        return;
    }
    D3DADAPTER_IDENTIFIER9 id{};
    d->GetAdapterIdentifier(D3DADAPTER_DEFAULT, 0, &id);
    std::printf("  adapter: %s\n", id.Description);
    void* caller = reinterpret_cast<void*>(&D3DChecks); // inside this module: an "Apex" call site
    const auto pool = [](int p) { return g_d3dBytes[p].load(); };
    const int64_t m0 = pool(1), r0 = pool(4), d0 = pool(0);
    IDirect3DTexture9* t = nullptr;
    Check(SUCCEEDED(dev->CreateTexture(256, 256, 0, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED, &t, nullptr)) && t, "managed texture created");
    ApexMemory::NoteTexture(caller, t, 256, 256, 0, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED);
    Check(pool(1) - m0 == 349524, "256 x 256 ARGB with 9 mips: 349524 bytes, managed");
    IDirect3DTexture9* x = nullptr;
    Check(SUCCEEDED(dev->CreateTexture(64, 64, 1, 0, D3DFMT_DXT1, D3DPOOL_MANAGED, &x, nullptr)) && x, "DXT1 texture created");
    ApexMemory::NoteTexture(caller, x, 64, 64, 1, 0, D3DFMT_DXT1, D3DPOOL_MANAGED);
    Check(pool(1) - m0 == 349524 + 2048, "64 x 64 DXT1: 2048 bytes");
    IDirect3DTexture9* rt = nullptr;
    Check(SUCCEEDED(dev->CreateTexture(128, 64, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &rt, nullptr)) && rt, "render target texture created");
    ApexMemory::NoteTexture(caller, rt, 128, 64, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT);
    Check(pool(4) - r0 == 128 * 64 * 8, "render target texture: 65536 bytes on its own line");
    IDirect3DSurface9* s = nullptr;
    Check(SUCCEEDED(dev->CreateRenderTarget(32, 32, D3DFMT_A8R8G8B8, D3DMULTISAMPLE_NONE, 0, FALSE, &s, nullptr)) && s, "render target surface created");
    ApexMemory::NoteSurface(caller, s, 32, 32, D3DFMT_A8R8G8B8, true);
    Check(pool(4) - r0 == 128 * 64 * 8 + 4096, "render target surface: 4096 bytes");
    ApexMemory::NoteTexture(reinterpret_cast<void*>(uintptr_t{0x10}), t, 256, 256, 0, 0, D3DFMT_A8R8G8B8, D3DPOOL_MANAGED);
    Check(pool(1) - m0 == 349524 + 2048, "a caller outside the module is not counted");
    Check(ApexMemory::ReportText().find("created at ApexRadiance.asi+0x") != std::string::npos, "creation site listed");
    // destroyed by the device: the counts go back
    t->AddRef();
    t->Release();
    Check(pool(1) - m0 == 349524 + 2048, "an AddRef / Release pair keeps it");
    t->Release();
    x->Release();
    Check(pool(1) == m0, "managed textures released: back to 0");
    rt->Release();
    s->Release();
    Check(pool(4) == r0 && pool(0) == d0, "render targets released: back to 0");
    Check(g_d3dUntracked.load() == 0, "every resource took the tracker");
    dev->Release();
    d->Release();
    DestroyWindow(wnd);
}
} // namespace

int main() {
    HeapChecks();
    EstimateChecks();
    SlotChecks();
    D3DChecks();
    std::printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
