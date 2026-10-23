# Lot loading analysis validation

## Static validation

The Steam executable has SHA-256 `10b2423e2368e161774462b0ef1960231ad0af298d564a7bb6a6e78c5e880609`.
Eight patterns for detail request, renderer update, load-stage call, lighting setup/update, object build, job wait and
file read each match once at the documented address. Disassembly confirms the renderer done flag at +0x1E, lot ID
words at +0x10/+0x14, stage at +0x24 and the 20/35/2000 ms load-call budgets. Existing engine documentation maps the
sub-step and failure fields. No matching EA 1.69 executable was available for this static check.

## Automated tests

`tools/lot_load_analysis_test/run.ps1` compiles the production recorder in a separate process. It checks the unsupported
version gate, synthetic renderer fields, unchanged and advancing stages, terminal state, queue overflow, CSV schema
and elapsed-time conversion. Synthetic objects are not validation of live object lifetime or runtime field layout.

## In-game test plan

1. Confirm the full development version. Enable developer mode and Frame Profiler; check target attachment results.
2. Approach a cold lot and then revisit it. Record request, repeated stages and native completion where observed.
3. Repeat with a dense lot and a large world, keeping camera route and graphics settings fixed.
4. Toggle profiling, switch worlds and return. Check epochs, missing-request flags, losses and bounded output.
5. Correlate with memory CSV and aggregate profiler reports, without assigning causality from correlation alone.

Live field validation, capture overhead and EA 1.69 support remain open.

## Open checks

- Lighting wait note (`7d69737`): a lot held in stage 20 for more than 10 s logs `[LotLoadAnalysis] Lot ... waits in
  stage 20 (lighting)`, at most every 30 s, and the level fields agree with the conditions in
  [engine/lot-loading-and-streaming.md](../engine/lot-loading-and-streaming.md#stage-20-the-lighting-wait). Not yet seen
  in a log.
