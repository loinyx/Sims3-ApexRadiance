#pragma once
// Scene object map index (Apex Radiance; part of "Faster object lookups", feature "ObjectLookupIndex"), the twin of
// ObjectIdMap (object_id_map.h) for the scene's map of the same kind.
//
// The scene's message router (the 0x11A8-byte singleton made at 0x006E0E19 and registered by 0x006FDE20; vtable
// 0x00FFA1F4, constructed by 0x00703510) keeps objects by their 64-bit id in a hash map at +4: 1033 (0x409) buckets of
// singly linked nodes (id at node +8 / +0xC, next at node +0; the object is node - 8), keyed by idLo % 1033, the end
// sentinel at map +0x1024 and the count at +0x1028. In a CPU sample its find was the game's busiest function on the
// render thread (2.8% of it), walking long chains for the lot's light and scene updates (callers 0x006FE3B0 and the getter
// at 0x006FE420, each a thin "object by id"). Its functions (Steam 1.67.2):
//   0x006FDFB0 find, thiscall(map; out[2], key*), ret 8: out = {node, &bucket} or {*(map+0x1024), map+0x1024} (end);
//     56 call sites.
//   0x006FE1F0 insert, thiscall(map; out[3], node, flag), ret 0xC: the only writer of new links (one caller); out byte 8 =
//     1 when the node was linked, 0 when the id was there already.
//   0x006FDF40 erase, thiscall(map; out[2], node, bucket*), ret 0xC: the only unlinker (callers 0x007030FB, 0x00703406).
//   Nothing else writes the count: the only other write is the constructor's 0 (0x00703528); no write at +0x102C from
//   the owner either. Insert and erase are leaves.
// As ObjectIdMap: an open-addressing table per map (up to kMaps maps; a map seen first gets the next one), kept by the
// insert and erase hooks under an exclusive SRW lock; find answers from it under a shared lock (busy = the game's walk)
// with exactly the game's output, and only while the table's count equals the map's own. With every insert and erase
// seen, the table can only hold entries the map no longer has (a map made again at the same address), never miss one,
// so equal counts mean the same contents; a different count builds that map's table again from the game's links (up to
// kMaxRebuilds times, then that map is left to the game). The first 256 answers of each map (then 1 in 64 in the
// development build) are compared with the game's walk; a difference turns the index off for the session.
#include <cstdint>
#include <string>

namespace RenderObjectMap {

bool Start(std::string* error);
void Stop();
bool Running();
std::string StatusText();

} // namespace RenderObjectMap
