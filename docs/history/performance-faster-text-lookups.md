# Faster Text Lookups: history

Chronological record of investigations and design decisions. Newest entries at the bottom. The current behaviour is
described in [features/performance/faster-text-lookups.md](../features/performance/faster-text-lookups.md).

### 2026-11-20: index of the loaded string tables

**Context:** with thousands of custom content packages every text lookup walks thousands of string tables; an offline
tool (CCStringMerge) merges the tables of all packages into one per language to cut that walk.

**Finding:** GetString (0x0059C5B0) walks the list at service + 0x48 and stops at the first table whose entry has a
non-empty text; the list changes only in three functions (clear 0x0059AF00, load one table 0x0059B190, rebuild
0x0059BDE0). The found path compares the winning node with the list's first node, so a copied text would change the
formatting; the node itself has to be handed over.

**Outcome:** an in-memory index of key -> winning node, built from the loaded tables, entered at the walk's loop test.
Offline merging was rejected: it changes the user's files and its order can differ from the game's priority order.
Sims3Performance's string cache and bucket layout tuning touch the same code; with either one on, the switch stands down.
