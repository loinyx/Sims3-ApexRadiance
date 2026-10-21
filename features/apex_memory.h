#pragma once
// Apex Radiance's own memory by owner (numbered development builds only; dev switch memory_owners). Credits: @loinyx
//
// Three measures, read together in the lighting snapshot (F8) and in an [ApexMemory] log line every minute:
//  - the C++ heap: every operator new / delete of the ASI is counted (live bytes and blocks, the session peak), by the
//    address that called operator new (one level: for a container that is the instantiation that grew it, so the site names
//    the element type once symbolised with the build's PDB). Development flavour only: the replacement is not compiled
//    into other builds. Each block carries an 8-byte header (size and site), counted apart as "accounting overhead".
//  - named owners: the large containers and caches of the lighting code, measured by the thread that owns them (a gather
//    thread, the render thread) at most every 10 s and at once after an F8, as capacity x element size plus the heap
//    nodes and buckets the standard containers allocate (MSVC layout; Est below). A measure never locks for long: the
//    containers under a mutex are walked under it, those of one thread on that thread.
//  - Direct3D resources the ASI creates (textures, render targets, shaders): counted at their creation in the device hooks
//    when the caller is inside the ASI, and subtracted when Direct3D destroys them (a private-data object the resource
//    releases), by pool; memory in the driver or DXVK is not visible to Apex and is not estimated.
// Measuring never changes what the lighting computes.
#include "dev_switches.h"
#include <windows.h>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

struct IDirect3DResource9;
struct IDirect3DBaseTexture9;

namespace ApexMemory {

bool Enabled(); // development flavour with memory_owners on

// ---- named owners ----
struct Slot {
    const char* group = "";
    const char* name = "";
    std::atomic<uint64_t> bytes{0}, items{0}, peak{0};
    std::atomic<DWORD> at{0}; // GetTickCount of the last measure (0 = never)
};
// The slot of (group, name), made at the first call; the strings must live as long as the ASI (literals). Thread-safe.
Slot* Register(const char* group, const char* name);
void Set(Slot* slot, uint64_t bytes, uint64_t items);
// Bytes a memory switch gave back since the start (released on a world change, a debug buffer dropped, ...)
void NoteReleased(const char* what, uint64_t bytes);

// A thread's measures are due every 10 s, or at its next chance after RequestNow (F8). One per measuring thread.
class Cadence {
public:
    bool Due();
private:
    DWORD last_ = 0;
    bool started_ = false;
    uint32_t seenRequest_ = 0;
};
void RequestNow();

// ---- Direct3D resources created by the ASI (D3D9Hooks create hooks) ----
void NoteTexture(void* caller, IDirect3DBaseTexture9* tex, UINT w, UINT h, UINT levels, DWORD usage, int format, int pool);
void NoteSurface(void* caller, IDirect3DResource9* surface, UINT w, UINT h, int format, bool renderTarget);
void NoteShader(void* caller, const DWORD* function, bool pixel);
bool CallerInAsi(const void* address);

// ImGui's allocations (menu, fonts), routed here before its context is made (overlay)
void* ImGuiAlloc(size_t bytes, void* user);
void ImGuiFree(void* p, void* user);

// ---- report ----
std::string ReportText(); // the lighting snapshot's section (F8)
void LogLine();           // [ApexMemory] line in the log
void LogLineEvery(DWORD ms); // the same at most every ms (LevelLightShare's render-thread measure calls it)
// The C++ heap counters (development flavour; zero elsewhere)
struct HeapTotals {
    int64_t bytes = 0, blocks = 0, peakBytes = 0;
};
HeapTotals Heap();

// ---- estimates of what a standard container owns (MSVC x86 layout) ----
namespace Est {
constexpr size_t kHeapHeader = 8; // the Windows heap's header per block on x86
constexpr uint64_t Block(size_t bytes) { return bytes ? bytes + kHeapHeader : 0; }
constexpr size_t AlignUp(size_t n, size_t a) { return (n + a - 1) / a * a; }
template <class V> constexpr size_t ListNode() { return AlignUp(2 * sizeof(void*), alignof(V)) + sizeof(V); }     // unordered_*: next, prev, value
template <class V> constexpr size_t TreeNode() { return AlignUp(3 * sizeof(void*) + 2, alignof(V)) + sizeof(V); } // map / set: 3 links, colour, nil

inline uint64_t Str(const std::string& s) { return s.capacity() > 15 ? Block(s.capacity() + 1) : 0; }
template <class T, class A> uint64_t Vec(const std::vector<T, A>& v) { return Block(v.capacity() * sizeof(T)); }
template <class T, class A, class F> uint64_t Vec(const std::vector<T, A>& v, F each) {
    uint64_t b = Vec(v);
    for (const auto& e : v) b += each(e);
    return b;
}
// unordered_map / unordered_set: a node per element and the list's sentinel; the bucket vector holds 2 iterators a bucket
template <class M> uint64_t Hash(const M& m) {
    using V = typename M::value_type;
    return Block(ListNode<V>()) * (m.size() + 1) + Block(m.bucket_count() * 2 * sizeof(void*));
}
template <class M, class F> uint64_t Hash(const M& m, F each) {
    uint64_t b = Hash(m);
    for (const auto& e : m) b += each(e);
    return b;
}
// map / set: a node per element and the head node
template <class M> uint64_t Tree(const M& m) {
    using V = typename M::value_type;
    return Block(TreeNode<V>()) * (m.size() + 1);
}
template <class M, class F> uint64_t Tree(const M& m, F each) {
    uint64_t b = Tree(m);
    for (const auto& e : m) b += each(e);
    return b;
}
// std::make_shared<T>: one block, the control block (vtable, two counts) before the object
template <class T> constexpr uint64_t Shared() { return Block(AlignUp(3 * sizeof(void*), alignof(T)) + sizeof(T)); }
} // namespace Est

} // namespace ApexMemory
