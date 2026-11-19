// Table legs: which object is which model (features/object_legs.h).
#include "object_legs.h"
#include "plant_scan.h"
#include "apex_log.h"
#include "build_flavor.h"
#include "entry_chain.h"
#include "game_addresses.h"
#include "mesh_caster_set.h"
#include <windows.h>
#include <atomic>
#include <cstring>
#include <format>
#include <mutex>
#include <string>
#include <unordered_map>

namespace {

constexpr uint32_t kLiteType = 0x03B4C61D;
// A table or a plant of the tables, a custom content plant, and while PlantScan runs every object with a LITE (custom content's
// plants are known only once it ends, after most objects attached; then the rest are dropped at their next fill)
struct Object {
    uint64_t lite;
    const ObjectLegs::Model* model;   // a legged table, or
    const ObjectLegs::Puff* puffs;    // a plant's balls (the table's or PlantScan's), or neither yet
    uint16_t puffCount;
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
// Mesh casters: every LITE object's key and matrix, and the records not taken yet (under g_mx)
std::atomic<bool> g_sink{false};
ObjectLegs::Feed g_feed{false}; // (under g_mx) objects known while the hooks are in; output while the sink is on
std::atomic<uint32_t> g_meshed{0};

uint64_t IdOf(uint32_t lo, uint32_t hi) { return static_cast<uint64_t>(hi) << 32 | lo; }
// (MC-4b) The feed learns objects with the sink off too while developer mode is on (meshCasters is a developer switch): turned on
// inside a world, the seed sends the objects attached before. shortcut: developer mode itself turned on inside a world still
// misses them until they attach again; walk the light manager's map (0x006B1430's [this+4] hash map) if that matters
bool Learning() { return g_sink.load(std::memory_order_relaxed) || !kPublicBuild.load(std::memory_order_relaxed); }

char __fastcall AttachHook(void* manager, void* edx, uint32_t idLo, uint32_t idHi, const uint32_t* key) {
    using Fn = char(__fastcall*)(void*, void*, uint32_t, uint32_t, const uint32_t*);
    const char made = reinterpret_cast<Fn>(EntryChain::Next(EntryChain::Site::LightObjectAttach, EntryChain::Layer::LampMarkFilter))(manager, edx, idLo, idHi, key);
    if (!made || key[2] != kLiteType) return made; // (ResourceKey: instance, type, group)
    const long seen = g_lites.fetch_add(1, std::memory_order_relaxed);
    uint64_t lite;
    std::memcpy(&lite, key, sizeof lite);
    if (Learning()) {
        std::lock_guard<std::mutex> lk(g_mx);
        g_feed.Attach(IdOf(idLo, idHi), lite);
    }
    const ObjectLegs::Model* model = ObjectLegs::Baked(lite);
    const ObjectLegs::PlantModel* plant = model ? nullptr : ObjectLegs::BakedPlant(lite);
    const ObjectLegs::Puff* custom = nullptr;
    uint16_t customCount = 0;
    if (!model && !plant && PlantScan::Done() && !PlantScan::Find(lite, custom, customCount)) return made; // nothing of ours
    {
        std::lock_guard<std::mutex> lk(g_mx);
        if (g_objects.size() >= 65536) g_objects.clear(), g_shapes.clear();
        g_objects[IdOf(idLo, idHi)] = {lite, model, plant ? &ObjectLegs::kPuffs[plant->first] : custom, plant ? plant->count : customCount, {}, false};
        g_any.store(true, std::memory_order_relaxed);
    }
    if (model || plant) g_baked.fetch_add(1, std::memory_order_relaxed);
    else if (seen == 0)
        LOG_INFO(std::format("[ObjectLegs] First LITE {:016X} of group {:08X}: not in the tables", lite, key[3]));
    return made;
}

char __fastcall MoveHook(void* manager, void* edx, uint32_t idLo, uint32_t idHi, const float* m, int a, int b, int c) {
    using Fn = char(__fastcall*)(void*, void*, uint32_t, uint32_t, const float*, int, int, int);
    const char moved = reinterpret_cast<Fn>(EntryChain::Next(EntryChain::Site::LightObjectMove, EntryChain::Layer::LampMarkFilter))(manager, edx, idLo, idHi, m, a, b, c);
    if (moved) {
        g_gen.fetch_add(1, std::memory_order_relaxed), g_hookMoves.fetch_add(1, std::memory_order_relaxed);
        g_moveThread.store(GetCurrentThreadId(), std::memory_order_relaxed);
    }
    if (moved && Learning()) {
        std::lock_guard<std::mutex> lk(g_mx);
        g_feed.Move(IdOf(idLo, idHi), m);
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
    PlantScan::Start();
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
void Touch() { g_gen.fetch_add(1, std::memory_order_relaxed); }
void MoveStats(uint32_t& hook, unsigned long& thread) {
    hook = g_hookMoves.load(std::memory_order_relaxed), thread = g_moveThread.load(std::memory_order_relaxed);
}

bool Hooked() { return g_on[1]; }

void SetSink(bool on) {
    size_t seeded = 0, known = 0;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        seeded = g_feed.SetOn(on), known = g_feed.KnownCount();
        g_sink.store(on);
    }
    if (on) LOG_INFO(std::format("[GiMesh] {} objects known as the sink opened, {} sent (attached before it, e.g. the feature turned on inside a world)", known, seeded));
}

void TakePlaced(std::vector<Placed>& out) {
    std::lock_guard<std::mutex> lk(g_mx);
    g_feed.Take(out);
}

size_t Seed() {
    std::lock_guard<std::mutex> lk(g_mx);
    return g_feed.Seed();
}
uint32_t Seeded() {
    std::lock_guard<std::mutex> lk(g_mx);
    return g_feed.Seeded();
}
uint32_t Meshed() { return g_meshed.load(std::memory_order_relaxed); }
uint32_t MeshGen() {
    const uint32_t on = (MeshCasterSet::wanted.load(std::memory_order_relaxed) ? 1u : 0u) | (MeshCasterSet::pixelCasts.load(std::memory_order_relaxed) ? 2u : 0u);
    return MeshCasterSet::g_set.MarkGen() << 2 | on;
}

void NoteRemoved(uint64_t id) {
    std::lock_guard<std::mutex> lk(g_mx);
    g_feed.Remove(id);
}

void NoteShape(uint64_t id, uintptr_t shape) {
    g_gen.fetch_add(1, std::memory_order_relaxed);
    if (shape && Learning()) {
        std::lock_guard<std::mutex> lk(g_mx);
        g_feed.Shape(id, shape);
    }
    if (!shape || !g_any.load(std::memory_order_relaxed)) return;
    std::lock_guard<std::mutex> lk(g_mx);
    if (g_objects.count(id)) g_shapes[shape] = id;
    else g_shapes.erase(shape); // (the address of a freed table's shape, another object's now)
}

void Fill(RoomLightTests::Room& room) {
    const bool pixel = MeshCasterSet::pixelCasts.load(std::memory_order_relaxed);
    const bool mesh = MeshCasterSet::wanted.load(std::memory_order_relaxed) || pixel;
    if (!mesh)
        for (RoomLightTests::Record& rc : room.rec) rc.meshed = false;
    if (!g_any.load(std::memory_order_relaxed) && !mesh) return;
    int records = 0;
    {
        std::lock_guard<std::mutex> lk(g_mx);
        // (MC-4b) a kept room's table is marked again when MeshGen moves (OutdoorDelta::KeptRooms), not only at its solve
        // (XR-10) with the per-pixel pass casting, an object the server resolved is darkened there: its record leaves the 3D test too
        if (mesh)
            g_meshed.fetch_add(static_cast<uint32_t>(MarkMeshed(room.rec, g_feed, [pixel](uint64_t id) { return MeshCasterSet::g_set.InTiles(id) || (pixel && MeshCasterSet::g_set.ServerCasts(id)); })),
                               std::memory_order_relaxed);
        for (RoomLightTests::Record& rc : room.rec) {
            if (rc.meshed) continue;
            const auto shape = g_shapes.find(rc.shape);
            if (shape == g_shapes.end()) continue;
            const auto object = g_objects.find(shape->second);
            if (object == g_objects.end() || !object->second.placed) continue;
            Object& o = object->second;
            if (!o.model && !o.puffs) {
                if (!PlantScan::Find(o.lite, o.puffs, o.puffCount)) {
                    // (custom content: once its scan ended; an object it did not find is nothing of ours)
                    if (PlantScan::Done()) g_shapes.erase(shape), g_objects.erase(object);
                    continue;
                }
            }
            if (o.model) Place(*o.model, o.m, rc.posts);
            else PlacePuffs(o.puffs, o.puffCount, o.m, rc.puffs);
            records++;
        }
    }
    if (records && g_filled.fetch_add(1, std::memory_order_relaxed) == 0)
        LOG_INFO(std::format("[ObjectLegs] {} of a room's {} records take legs or leaves ({} of {} objects read are in the table)", records, room.rec.size(), g_baked.load(), g_lites.load()));
}

} // namespace ObjectLegs
