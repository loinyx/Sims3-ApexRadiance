# Room to Save: history

Chronological record of investigations and design decisions. Newest entries at the bottom. The current behaviour is
described in [features/performance/room-to-save.md](../features/performance/room-to-save.md).

### 2026-10-05: a reserve of address space for world saves

**Context:** commit `3328a4b`. A memory study (2026-09-30) found that what runs out first in a long session is one large
free block of the 32-bit address space, not memory. Error 12 is the world save (0x00C6D460, called at 0x00AAC320)
failing.

**Finding:** a reserved, uncommitted block costs nothing but address space and can be handed back at the moment the save
needs it; the game's own trim (0x00733E70) frees its idle resource cache without losing anything in use.

**Outcome:** a 128 MB reserve taken at start, let go before every world save and when the largest free block falls under
160 MB, taken again when 640 MB are free; before every save and under 320 MB the game's resource caches are shrunk on
the thread of their per-frame update (its call at 0x007377F7 is wrapped). The same commit limited the top detail class on
every story to the priority lot when *High quality on every lot* is on.

### 2026-10-05: the EA app build

**Context:** commit `733a7c5`.

**Outcome:** the ResourceSystem update call, the shrink of both caches and the world save call get signatures (each
unique on Steam). Steam keeps its fixed addresses and byte checks; other builds check the calls' targets. The EA code is
encrypted on disk, so the signatures there are confirmed by the startup scan's log.

### 2026-10-06: Experimental badge

**Context:** commit `7232c3b`.

**Outcome:** marked Experimental in the menu, still on by default. Released in 2.7.0.

### 2026-10-20: compatibility with sims3-modernization's ts3native

**Context:** with sims3-modernization 0.1.4 installed, `ts3native.log` showed its detour of the package save at
`0x00AAC110` (through its route gate, also with its own save write option off), and Apex logged *Early save sites
differ; existing world-save preparation retained*: the early save preparation was off. The rest of Room to Save stayed
on.

**Finding:** both mods only rewrite the call while it still points at `0x00AAC110`; the entry itself starts with a
`jmp rel32` into `ts3native.asi`.

**Outcome:** a first build accepted any `jmp` at the entry; the kept version accepts it only when `ts3native.asi` is
loaded and the jump lands inside it. The call site `0x00AB12AF` is still verified, and Apex's wrapper reaches the game
through ts3native's detour. Not yet confirmed with a save in game.
