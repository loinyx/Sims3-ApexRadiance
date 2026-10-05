# Remember Missing Files: history

Chronological record of investigations and design decisions. Newest entries at the bottom. The current behaviour is
described in [features/performance/remember-missing-files.md](../features/performance/remember-missing-files.md).

### 2026-09-29: negative caching and write epochs (round 3, sections 3.3 and 3.4)

**Context:** measured with the lookup cache on (`round3.md` section 3): about 36% of FindProvider calls are keys that
exist in no package; each costs a full scan (about 19 us) and together they are 78% of the remaining lookup time. Cache
hits still probe about 17 non-read-only packages (about 1.3 us per hit).

**Finding:** three read-only studies of `full.asm` traced every write path of the DPF, DDF and packed-stream classes.
MemoryDB inserts keys through the non-virtual PutResource 0x0072C350, and the ContentManager changes its maps in
non-virtual install code, so neither can be counted through vtables.

**Outcome:** shipped experimental, off by default. MemoryDB and ContentManager stay probed; hooking PutResource's entry
or a resolve-level cache at 0x007D8110 was left open.

### 2026-09-29: exception safety and the before-lookup reliability rule (v1.5.0 merge)

**Context:** review of the merged round 3 code.

**Finding:** an exception unwinding through a hooked write could leave `g_writeBusy` raised, keeping every epoch sum
"unknown" until restart. A read-only package that failed to open during the game's lookup and was then opened by another
thread before `Remember` looked at it would look reliable afterwards.

**Outcome:** `WriteEnd` runs in a `__finally` (`Bracketed`). An absent answer is stored only when every read-only
package was reliable both before the game's lookup (`roBefore`) and after it.

### 2026-10-02: on by default (2.5.5)

**Outcome:** default changed to on. See [group history](performance.md).
