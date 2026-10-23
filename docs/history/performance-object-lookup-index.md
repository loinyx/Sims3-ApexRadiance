# Faster Object Lookups: history

Chronological record of investigations and design decisions. Newest entries at the bottom. The current behaviour is
described in [features/performance/object-lookup-index.md](../features/performance/object-lookup-index.md).

### 2026-09-29: validated index (plan candidate C8)

**Context:** plan section 2.2: 0x00C5FA60 is 13.6% of the samples of lot-lighting-dominated hitches (the hot leaf), with
0x00C62D55 / 0x00C60D76 on the stack in 16%; the lookup has 233 direct callers including the script natives.

**Finding:** the Layer mutators are reached only through the Layer vtable, but the WorldManager roots vector is written
by eight non-virtual methods and Layers load their children in 0x00AAA190. Hooking eight direct-called methods by their
entries, and proving there is no other writer, was judged not verifiable enough. Every caller's `visited` argument was
checked site by site to be 0.

**Outcome:** a validated cache with no mutator hooks; shipped experimental, off by default; merged with C6 in v1.5.0.

### 2026-10-02: on by default (2.5.5)

**Outcome:** default changed to on. See [group history](performance.md).

### 2026-10-05: object service ID index

**Context:** commit `b0a95b7`. The object service's map by ID (find 0x00939100) walks one of 1033 bucket chains per
lookup, with dozens of objects per chain in a populated world.

**Finding:** the map has exactly one insert (0x00939170) and one erase (0x00938D00), both leaves, and nothing else writes
its count (+0x1028), so an index kept by those two hooks can be checked against the count on every answer.

**Outcome:** `ObjectIdMap`, an open-addressing side index started with Faster Object Lookups; answers only while the
counts match, the first 256 checked against the game's walk. Released in 2.7.0.

### 2026-10-22: cheaper counters and 16 recent answers

**Context:** commit `e4fb59a`. A room-solve frame makes 150 to 180 thousand lookups, and each made up to three 64-bit
atomic adds (`lock cmpxchg8b` loops on 32-bit x86); the one remembered answer per thread missed whenever a solve
alternated between owners, and each miss took the shared SRW lock.

**Outcome:** per-thread counters added to the totals every 256 counts, a 32-bit verification serial, and 16 recent
answers per thread. Every answer is still validated against the live tree.

### 2026-10-22: scene object map index

**Context:** commit `cdff27e`. In a CPU sample the busiest game function on the render thread was 0x006FDFB0 (2.8% of
the thread): the find of the scene message router's map of objects by their 64-bit ID, walking long chains of its 1033
buckets for the lot's light and scene updates (callers 0x006FE3B0 and the getter at 0x006FE420).

**Finding:** the map is the twin of the object service's map that `ObjectIdMap` indexes: the same find, insert and
erase code, with nodes linked at +0 instead of +0x10. The insert 0x006FE1F0 (one caller) and the erase 0x006FDF40
(callers 0x007030FB, 0x00703406) are leaves and the only writers of its links; the count is written only by them and
by the constructor's 0 (0x00703528). With every insert and erase seen, a table can only hold entries the map no longer
has, never miss one, so equal counts mean equal contents.

**Outcome:** `RenderObjectMap`, one table per map, started with Faster Object Lookups. Unlike `ObjectIdMap`, a count
difference builds that map's table again from the game's links (at most 16 times) instead of turning the index off; a
wrong answer turns it off for the session. The first 256 answers of each map are checked against the game's walk.
Gameplay checks are open ([validation](../validation/performance-object-lookup-index.md)).
