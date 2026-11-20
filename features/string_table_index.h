#pragma once
// String table index (Apex Radiance, feature "StringTableIndex"; Performance page, experimental, off by default).
//
// The game keeps one hash table per loaded string table (STBL) in a list (string service + 0x48, primary locale first,
// in resource priority order). Every text lookup, GetString (Steam 1.67.2: 0x0059C5B0, thiscall(service; u64 key),
// ret 8), walks that list and probes each table until one has the key with a non-empty text; a key no table has walks
// all of them. With thousands of custom content packages each pie menu, tooltip and catalog name pays thousands of probes.
//
// The index maps each key to the list node the walk would stop at (the first table in list order whose text is not
// empty), built in memory from the tables the game already loaded: no file reads. The walk's entry (0x0059C5C2, after
// the list head is read) jumps to Apex, which hands the walk the node to start from: the winning node (the game then
// probes that one table and goes on exactly as before), the list's end (no table has the key: the game's own "missing"
// path) or the list's first node (no answer: the full walk). So the text, the formatting after it and the returned
// string are the game's own.
//
// The list changes only in three game functions, each wrapped (Detours): the clear (0x0059AF00), the load of one table
// into its node (0x0059B190: at the rebuild and from the resource change notification) and the rebuild (0x0059BDE0: at
// start and on a locale change). Any of them makes the index stale; it is rebuilt in steps of at most 2 ms inside the
// lookups that find it stale, which take the full walk meanwhile. Each answer also checks that the list's ends are as
// built, and the first 256 answers after each build plus 1 in 64 later ones are compared with a walk done like the
// game's: any difference turns the index off for the session.
//
// Stands down (stays off, logged): another module changed GetString's entry or the walk, or
// Mods\Packages\CCStringMerge_Strings.package exists (string tables merged offline already).
// Thread-safe: the index is read under a shared SRW lock and built under the exclusive one, both taken with Try (a
// lookup never waits: it takes the walk); no lock is held while game code runs.
#include <string>

namespace StringTableIndex {

bool Start(std::string* error);
void Stop();
bool Running();
std::string StatusText();
std::string ReportLine(); // counters: hits, misses, walks, entries, bytes, build time

} // namespace StringTableIndex
