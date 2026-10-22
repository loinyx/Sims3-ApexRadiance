# Lot loading analysis

Lot loading analysis records native detail requests and load-state calls to study delayed lot appearance. It extends
the existing Frame Profiler wrappers; it does not install another set of game hooks or accelerate loading.

## Status

Released in 2.14.0 as developer diagnostics, off by default. Per-lot field reads are limited to Steam 1.67.2, PE timestamp 0x52DEC247.
The eight relevant profiler signatures match that executable uniquely in static validation. EA 1.69 field-layout
validation and in-game measurements remain pending; other builds do not read the proposed per-lot fields.

## How it works

Enable developer mode and start the existing Frame Profiler from its menu. While its target hooks are attached,
native detail flag changes publish request events. The existing lot-load call wrapper reads the renderer lot ID,
stage/sub-step and done/failed flags before and after the original function. QPC measures inclusive elapsed call time,
independently of the profiler's selectable clock. A call can span several stages; its time is not attributed to just
the entering or exiting stage.

A bounded queue passes events to the existing initialization pump. Hook producers use a nonblocking lock attempt;
contention and full queues increment a loss counter. The pump writes `LotLoadAnalysis-<pid>-<date>-<time>.csv` in the
Apex documents folder, capped at 16 MiB per session, and logs terminal events. Captures have separate epochs when
the profiler is toggled. Tracking holds at most 256 lot IDs; exceeding it discards request baselines with a warning.

## Lighting wait note

On Steam 1.67.2, while the profiler's load-stage wrapper is attached, after each load-stage call a lot that stays in stage 20 (the lighting wait) for more than 10 s has what it waits for
written to the log, at most once every 30 s per lot (`NoteWait`; the text is built under SEH and written by the pump):

```
[LotLoadAnalysis] Lot <id> waits in stage 20 (lighting) for N s: level <ptr>: lit L, step A/B, solvers S1/S2, rooms R, waiting in state 4: W <room ptrs>; ...
```

Per level of the lot lighting manager: the lit flag `+0x81`, the steps `+0x88` / `+0x284`, the two solver states
`+0x2A4` / `+0x2FC` and, for a lit level, the rooms of its table and how many of them still hold it in state 4 (up to
four listed). The conditions are those of stage 20 in
[engine/lot-loading-and-streaming.md](../engine/lot-loading-and-streaming.md#stage-20-the-lighting-wait). At most 256
waiting lots are tracked; a lot that leaves stage 20, finishes or fails is forgotten.

## Recorded data

- Full development build, QPC timestamp/frequency, thread and capture epoch.
- Native lot ID, detail request/demotion, entry and exit stage/sub-step, done and failed flags.
- Inclusive call duration, calls and summed inclusive time since the recorded request, and request-to-done interval
  when both ends are observed. A missing request produces -1. Unmatched load calls have no reliable request baseline.
- Cumulative dropped/discarded events, which prevent treating the capture as complete.

## Limitations

The native done flag is not proof that all textures, Sims and scene nodes are visibly complete. Request-to-done
includes scheduling gaps, while inclusive function time includes nested calls and waits. Neither is CPU time.
Object creation, resource reads, jobs and lighting have existing aggregate profiler categories; they cannot yet be
causally attributed to a particular lot from this CSV. Concurrent resource work cannot be attributed from nearby
timestamps alone. The recorder does not read save structures or infer memory ownership.

Signature availability, per-lot capture availability and profiler activation must be checked separately. A missing
CSV does not indicate instant loading. If the profiler is off or its target hooks cannot attach, there are no events.

## See also

- [Validation](../validation/lot-load-analysis.md)
- [Engine map](../engine/lot-loading-and-streaming.md)
- [Memory analysis](memory-analysis.md)
