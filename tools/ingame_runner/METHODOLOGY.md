# In-game test methodology

How the in-game runner turns game loads into a result a feature can be judged by: what counts, what is left out, how a
feature is triggered and compared, when a run stops and how sure a result is. The runner's [README](README.md) says how
to run it; this page says why it works the way it does.

## 1. Real hitches only

A result holds only what a player would see. Everything else is excluded point by point and listed by reason, never
silently dropped:

| Excluded | Why |
|---|---|
| The first minute of the profile | The save load settles and the profilers start; its hitches belong to loading, not play |
| The profiler edges (last 2 s) | Attaching and detaching hooks costs time on the hooked threads |
| Harness commands | Every job step (camera, mode switch, save, reload, repro start) from its start to 10 s after it returns, and every test agent call (200 ms either side): agent calls run on the simulation thread. They are game cost, but the scenario's, not play's: `results.md` lists them per command as the scenario cost |
| Sampler windows | A stack sampler suspends the simulation thread several times a second, so every slow iteration inside its window overlaps a suspension, and the main thread waits for render work the suspended thread would have done. The whole window goes, not single samples. The runner has no sampler, so its loads have none (section 2) |
| Render-paced publish gaps | A gap between two simulation publishes with no slow iteration inside is the render side waiting (section 3) |

A whole load is **invalid** when the profile was cut short (under 80% of its planned seconds), the machine was noisy
over the measured window (other programs at 10% of the machine's CPU or more, after the first minute), or the game
crashed or did not quit. An invalid load stays in the table, marked, and leaves the means and the pair statistics.

## 2. Measurement runs and cause runs

Two kinds of run answer two questions and are never mixed:

- **Measurement runs** count events and time frames: hitch rates, visible freezes, frame times, A/B pairs. They run
  without stack samplers and, where the profiler's own cost matters, with the lean simulation profiler (`-Lean`:
  iterations, steps and publish gaps only).
- **Cause runs** find what leads a slow frame or iteration: stage timers, the services split, stack samples. Their
  rates are disturbed by the instruments, so a cause run is read for causes, never for rates.

## 3. Unfocused realism

The runner never focuses the game window, and the game sleeps 10 ms per main-loop pass while another window is in
front. A player in front of the game never sees that sleep. The game's own `-NoInactiveIdle` command-line switch turns it off;
the runner starts every load with it. With it, every frame counts.

A frame limiter that wakes the simulation once per presented frame (Sims3SettingsSetter's precise limiter does) ties
simulation publishes to the render rate. A publish gap without a slow simulation iteration inside is therefore the
render side waiting and is excluded as render-paced. With `NoInactiveIdle` on, a gap over two or more iterations that
did not publish is a simulation cause and stays.

## 4. Forced repros

A feature's code path fires a few times per load in natural play: too few to compare. A repro sets the triggering
state directly and fires the path many times per load:

- **Set the state, do not wait for it.** Jump the camera across the town instead of waiting for Sims to travel; build
  Sims in view instead of waiting for visitors; switch modes instead of waiting for the player.
- **Re-create the state every loop.** Each loop removes the Sims the previous loop spawned and builds new ones, re-ranks
  the populated lots and re-creates the effects, so the path keeps firing at the same rate instead of hitting a warm
  cache after the first pass.
- **Real work, nothing lasting.** Saves are real saves to new files, outfit changes are real Sim builds, effects are
  real effects. Nothing burns, spreads or is saved over the tester's save, and spawned Sims are removed.

The [README](README.md#feature-repros) lists the repro per feature, its expected events and what to read.

## 5. A/B design

The best comparison flips the feature per firing inside one load, in ABBA order (off, on, on, off, ...), and credits
each firing's cost to its arm from that firing to the next. Both arms then share the same world, the same Sims and the
same moment, and a drift over the session (a warming file cache, a growing heap) cancels out.

Apex Radiance reads its settings at start, so the runner cannot flip a feature inside a load. It compares separate game
starts in ABBA order instead (off, on, on, off). That keeps drift balanced but adds start-to-start noise: each start
loads a slightly different world. A per-firing A/B becomes possible once a feature can be switched while the game runs.

## 6. When a run stops, and how sure a result is

- **Stop at a fixed event count set before the run.** A repro wait carries `until` (for example 75 camera jumps) and
  a cap in minutes; the window ends when the repro reaches the count. The count never depends on the on/off
  difference, so stopping there biases nothing. A run is never stopped because a difference looks significant:
  looking after every firing and stopping on the first "significant" look inflates false results.
- **Intervals.** Events over real minutes are Poisson counts: exact (Garwood) 95% intervals per variant, a Wald interval
  for the difference of rates, an exact conditional (Clopper-Pearson) interval for the rate ratio.
- **Not enough events below 10.** With fewer than 10 events in both variants together the ratio interval spans 1 for
  nearly every split, so the pair is not read.
- **Power.** `results.md` gives the loads per variant needed to see a 30% cut at the off variant's rate (two-sided 5%,
  power 80%). Slow iterations of 100 ms or more are frequent enough for one pair; a 30% cut of 300 ms events needs tens
  of loads in natural play, and freezes of a second or more are too rare for any natural-play A/B. Those need forced
  repros where the event rate is high, or are judged by their mechanism (a counter and a repro), not by a count.
- Slow iterations cluster (one stall delays several Sims), so the Poisson interval is somewhat too narrow; both halves
  of the ABBA order agreeing is the practical check.

## 7. Natural play and soak runs

Natural-play runs (a save left to play, camera still, no repro) give per-hour rates of what players meet and serve as
the final no-regression check of a set of features. They are not the test for one feature: their event rates are too
low (section 6). Effects that build up slowly (memory use, address space, save size, cache growth) need a soak run: one
long load with saves at fixed points, compared at the same points.

## 8. Before a timing run

- **Check the tool first.** A short correctness run (one load, a few minutes) before any timing run shows that every
  profiler hooked, every file is written and the classifier reads them. A timing run on a broken tool is a lost run.
- **Quiet machine.** Nobody uses the computer during a measured load; other programs over 10% of the machine's CPU
  make the load invalid. Rerun invalid loads rather than reading them.
- **One change at a time.** A pair compares one switch; everything else stays as it was for both variants.
