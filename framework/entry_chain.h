#pragma once
// Layered hooks on the ENTRY of game functions that several Apex modules wrap (the code-entry twin of slot_chain.h).
//
// The entry's first instructions (a verified relocation-free prologue) are copied to a trampoline (+ JMP back), and a
// 5-byte JMP to the outermost installed layer's hook is written over them. Each layer calls Next(site, layer): the next
// inner installed layer's hook, or the trampoline (= the game's own function). Layers have fixed positions (lower =
// outer), whatever order the modules install in:
//   - Install: the new layer's next pointer first; then either the entry JMP is (re)written to it (it becomes the
//     outermost) or the next outer layer's next pointer is re-pointed to it (one atomic store).
//   - Remove: the reverse; when the last layer goes, the original prologue bytes are written back.
//   - Code bytes are written only with every other thread suspended and none of them stopped inside the bytes
//     (MemPatch::WriteCodeSuspended): several threads call these functions. A removed hook keeps its next pointer, so a
//     thread still inside it finishes normally; hooks and trampolines stay in memory for the process lifetime.
//   - Before the first write the entry must hold the expected prologue; later, the JMP Apex wrote. Anything else
//     (another module hooked it) makes Install fail and is never overwritten by Remove.
// Used by the Frame Profiler (dev build, the outer layer) and the fast DXT encoder (features/fast_dxt.h) on the game's
// CPU DXT1 / DXT5 encoders, by the resource lookup cache's write epochs (features/resource_cache.h) on the writable
// package database's direct record write, and by the Frame Profiler and the object lookup index (features/object_index.h)
// on the object-by-ID lookup, and by the scene node budget (features/scene_budget.h) on the scene node destructor, AddNode
// and the holder teardown. Thread-safe (Install / Remove serialise on one mutex; Next is lock-free).
#include <atomic>
#include <cstdint>
#include <string>

namespace EntryChain {

enum class Site : int {
    DxtEncode1, // 0x006152F0 cdecl(Dst*, Src*), prologue 55 8B EC 83 E4 F0 (push ebp; mov ebp,esp; and esp,-16)
    DxtEncode5, // 0x006154B0, same prologue
    DpfWriteDirect, // 0x004A7FC0 thiscall(5 args), ret 0x14: the writable package database's direct record write; prologue
                    // 83 EC 28 53 56 (sub esp,28h; push ebx; push esi); bracketed by the write epochs of features/resource_cache.cpp
    ObjectById, // 0x00C62D40 thiscall(idLo, idHi, int* visited), ret 0xC; prologue 8B 44 24 0C 8B 54 24 08 (two movs, 8 bytes)
    SceneNodeDtor,       // 0x006FD930 thiscall(), ret: the scene node base destructor; prologue 55 8B EC 83 E4 F0 (as the DXT encoders)
    SceneAddNode,        // 0x006E6480 thiscall(node, group), ret 8: the pending holder's AddNode; prologue 56 8B 74 24 08
    SceneHolderTeardown, // 0x006E4DE0 thiscall(), ret: the pending holder's teardown; prologue 53 55 56 57 8B F9
    RoomInvalidate,      // 0x0069EED0 thiscall(room, char full, char keep), ret 8; prologue 56 8B F1 8B 0E (push esi; mov esi,ecx; mov ecx,[esi])
    RoomInvalidateFlag,  // 0x0069F160 thiscall(room, char flag), ret 4; prologue 8A 44 24 04 56 (mov al,[esp+4]; push esi)
    CasTriSort,          // 0x005D1960 cdecl(6 args), ret: the CAS model builder's triangle sort; prologue 55 8B EC 83 E4 F0 (as the DXT encoders)
    RecordCrc,           // 0x004FA4C0 cdecl(bytes, length, crc, bool invert), ret: the cache records' CRC-32; prologue 8B 4C 24 04 8B 44 24 08 (two movs)
    MonoTypeGetObject,   // 0x00EA8A00 cdecl(domain, type), ret: mono_type_get_object; prologue 53 8B 5C 24 0C 55 56 57 (push ebx; mov ebx,[esp+0Ch]; push ebp; push esi; push edi)
    MonoDomainFree,      // 0x00E75340 cdecl(domain, force), ret: mono_domain_free; prologue 55 56 8B 74 24 0C (push ebp; push esi; mov esi,[esp+0Ch])
    ObjMapFind,          // 0x00939100 thiscall(out, key), ret 8: the object service's map find; prologue 83 EC 08 56 8B 74 24 14 (sub esp,8; push esi; mov esi,[esp+14h])
    ObjMapInsert,        // 0x00939170 thiscall(out, node, flag), ret 0xC: its insert; prologue 53 55 56 8B 74 24 14 (push ebx; push ebp; push esi; mov esi,[esp+14h])
    ObjMapErase,         // 0x00938D00 thiscall(out, node, bucket), ret 0xC: its erase; prologue 8B 44 24 04 53 (mov eax,[esp+4]; push ebx)
    RenMapFind,          // 0x006FDFB0 the scene's map of the same kind (render_object_map.h): find, prologue as ObjMapFind
    RenMapInsert,        // 0x006FE1F0 its insert, prologue as ObjMapInsert
    RenMapErase,         // 0x006FDF40 its erase, prologue as ObjMapErase
    LightObjectRemove,   // 0x006C7610 thiscall(idLo, idHi), ret 8: an object out of a level's five light maps; prologue 53 8B 5C 24 08 56 57 (push ebx; mov ebx,[esp+8]; push esi; push edi)
    LotAddObjectsToScene, // Lot::AddLotObjectsToScene thiscall(lot, char, char), ret 8; prologue 83 EC 08 57 8B F9
    ProbeDiffuseWrite,    // 0x006B4720 thiscall(probe), ret: the light probe's diffuse cube writer; prologue 83 EC 28 53 55 (sub esp,28h; push ebx; push ebp)
    ProbeSpecularWrite,   // 0x006B4B10 thiscall(probe, rt), ret 4: its specular cube writer; prologue 81 EC F4 00 00 00 (sub esp,0F4h)
    TreeSetLevel,         // 0x0070D370 thiscall(cluster, level), ret 4: the SpeedTree cluster's level setter (vfunc +0x50); prologue 8B 44 24 04 56 50
    TreeSetCustomRig,     // 0x0070D800 thiscall(cluster, key, arg), ret 8: its custom light rig setter (vfunc +0x6C); prologue 8B 44 24 04 56 8B F1
    Count
};
// lower = outer. FastDxt: the DXT sites only; ResourceCache: the DpfWriteDirect site only; ObjectIndex: the ObjectById
// site only; SceneBudget: the three scene node sites only (features/scene_budget.h).
enum class Layer : int { FrameProfiler, FastDxt, ResourceCache, ObjectIndex, SceneBudget, LevelLightShare, FastCas, FastCrc, ScriptMath, ObjectIdMap, RoomLightQueue, LotObjectThrottle, ProbeNeutral, RenderObjectMap, TreeLightRig, Count };

// Installs `hook` as `layer` of `site` (true when installed, or already installed). error: why not.
bool Install(Site site, Layer layer, void* hook, std::string* error);
// Removes the layer (nothing when it is not installed). False only when the original bytes could not be written back.
bool Remove(Site site, Layer layer);
bool Installed(Site site, Layer layer);
// The function's entry (0 until the first Install resolved it)
uintptr_t GameFunction(Site site);
// A callable copy of the game's function (the trampoline), or nullptr before the first Install
void* Original(Site site);

// What `layer`'s hook must call: never null while the layer is installed (and afterwards, for threads still inside it).
extern std::atomic<void*> g_next[static_cast<int>(Site::Count)][static_cast<int>(Layer::Count)];
inline void* Next(Site site, Layer layer) { return g_next[static_cast<int>(site)][static_cast<int>(layer)].load(std::memory_order_acquire); }

} // namespace EntryChain
