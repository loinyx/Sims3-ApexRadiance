#pragma once
// Read-back benchmark (developer builds with developer mode on; the players' build compiles none of it). A one-shot
// measurement on the render thread, at Present, of what a GPU read-back and a few device calls cost: GetRenderTargetData
// by size, two read-backs in a row, StretchRect then read-back, a system-memory lock after an event query, query
// polling, Get* calls and vertex shader constant batches. Started by `[readback_bench] run = true` in ApexRadiance.toml
// once a world is shown; the table goes to the log and to Diagnostics\ReadbackBench-<pid>.csv
// (docs/features/dev-tools/readback-bench.md).
namespace ReadbackBench {
void Install(); // after the features start; does nothing unless the key is set
}
