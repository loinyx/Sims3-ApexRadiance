// Table legs: which object is which model (features/object_legs.h).
#include "object_legs.h"
#include "apex_log.h"
#include "entry_chain.h"
#include "game_addresses.h"
#include <windows.h>
#include <atomic>
#include <cstring>
#include <format>
#include <mutex>
#include <string>
#include <unordered_map>

namespace {

constexpr uint32_t kLiteType = 0x03B4C61D;
struct Object {
    const ObjectLegs::Model* model;
    float m[16];
    bool placed;
};
// shortcut: a removed object stays here (its shape's next owner replaces the link) until 65536 tables were made; drop it through LightObjectRemove if that ever matters
std::mutex g_mx;
std::unordered_map<uint64_t, Object> g_objects;   // (under g_mx) objects of a baked model, by id
std::unordered_map<uintptr_t, uint64_t> g_shapes; // (under g_mx) their first shapes
std::atomic<bool> g_any{false};
std::atomic<long> g_lites{0}, g_baked{0}, g_filled{0};
std::atomic<uint32_t> g_gen{0}, g_hookMoves{0};
std::atomic<unsigned long> g_moveThread{0};
bool g_on[2] = {};

uint64_t IdOf(uint32_t lo, uint32_t hi) { return static_cast<uint64_t>(hi) << 32 | lo; }

char __fastcall AttachHook(void* manager, void* edx, uint32_t idLo, uint32_t idHi, const uint32_t* key) {
    using Fn = char(__fastcall*)(void*, void*, uint32_t, uint32_t, const uint32_t*);
    const char made = reinterpret_cast<Fn>(EntryChain::Next(EntryChain::Site::LightObjectAttach, EntryChain::Layer::LampMarkFilter))(manager, edx, idLo, idHi, key);
    if (!made || key[2] != kLiteType) return made; // (ResourceKey: instance, type, group)
    const long seen = g_lites.fetch_add(1, std::memory_order_relaxed);
    uint64_t lite;
    std::memcpy(&lite, key, sizeof lite);
    if (const ObjectLegs::Model* model = ObjectLegs::Baked(lite)) {
        std::lock_guard<std::mutex> lk(g_mx);
        if (g_objects.size() >= 65536) g_objects.clear(), g_shapes.clear();
        g_objects[IdOf(idLo, idHi)] = {model, {}, false};
        g_any.store(true, std::memory_order_relaxed);
        g_baked.fetch_add(1, std::memory_order_relaxed);
    } else if (seen == 0)
        LOG_INFO(std::format("[ObjectLegs] First LITE {:016X} of group {:08X}: not a legged table", lite, key[3]));
    return made;
}

char __fastcall MoveHook(void* manager, void* edx, uint32_t idLo, uint32_t idHi, const float* m, int a, int b, int c) {
    using Fn = char(__fastcall*)(void*, void*, uint32_t, uint32_t, const float*, int, int, int);
    const char moved = reinterpret_cast<Fn>(EntryChain::Next(EntryChain::Site::LightObjectMove, EntryChain::Layer::LampMarkFilter))(manager, edx, idLo, idHi, m, a, b, c);
    if (moved) {
        g_gen.fetch_add(1, std::memory_order_relaxed), g_hookMoves.fetch_add(1, std::memory_order_relaxed);
        g_moveThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
    }
    if (moved && g_any.load(std::memory_order_relaxed)) {
        std::lock_guard<std::mutex> lk(g_mx);
        if (const auto it = g_objects.find(IdOf(idLo, idHi)); it != g_objects.end()) {
            std::memcpy(it->second.m, m, sizeof it->second.m);
            it->second.placed = true;
        }
    }
    return moved;
}

} // namespace

namespace ObjectLegs {

void Install() {
    using GameAddr::Id;
    if (!GameAddr::Have({Id::LightObjectAttach, Id::LightObjectMove})) return;
    std::string err;
    g_on[0] = EntryChain::Install(EntryChain::Site::LightObjectAttach, EntryChain::Layer::LampMarkFilter, reinterpret_cast<void*>(&AttachHook), &err);
    if (g_on[0]) g_on[1] = EntryChain::Install(EntryChain::Site::LightObjectMove, EntryChain::Layer::LampMarkFilter, reinterpret_cast<void*>(&MoveHook), &err);
    if (!g_on[1]) LOG_WARNING("[ObjectLegs] Tables keep their tops alone in the 3D test: " + err);
}

void Uninstall() {
    if (g_on[1]) EntryChain::Remove(EntryChain::Site::LightObjectMove, EntryChain::Layer::LampMarkFilter), g_on[1] = false;
    if (g_on[0]) EntryChain::Remove(EntryChain::Site::LightObjectAttach, EntryChain::Layer::LampMarkFilter), g_on[0] = false;
}

uint32_t Gen() { return g_gen.load(std::memory_order_relaxed); }
void MoveStats(uint32_t& hook, unsigned long& thread) {
    hook = g_hookMoves.load(std::memory_order_relaxed), thread = g_moveThread.load(std::memory_order_relaxed);
}

void NoteShape(uint64_t id, uintptr_t shape) {
    g_gen.fetch_add(1, std::memory_order_relaxed);
    if (!shape || !g_any.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::mutex> lk(g_mx);
    if (g_objects.count(id)) g_shapes[shape] = id;
    else g_shapes.erase(shape); // (the address of a freed table's shape, another object's now)
}

void Fill(RoomLightTests::Room& room) {
    if (!g_any.load(std::memory_order_relaxed)) return;
    int records = 0;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        for (RoomLightTests::Record& rc : room.rec) {
            const auto shape = g_shapes.find(rc.shape);
            if (shape == g_shapes.end()) continue;
            const auto object = g_objects.find(shape->second);
            if (object == g_objects.end() || !object->second.placed) continue;
            Place(*object->second.model, object->second.m, rc.posts);
            records++;
        }
    }
    if (records && g_filled.fetch_add(1, std::memory_order_relaxed) == 0)
        LOG_INFO(std::format("[ObjectLegs] {} of a room's {} records take legs ({} of {} objects read are in the table)", records, room.rec.size(), g_baked.load(), g_lites.load()));
}

} // namespace ObjectLegs
