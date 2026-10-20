#pragma once
// Faster cache compression (Apex Radiance, feature "FastCacheCompression"; docs/features/performance.md).
//
// The game compresses resources it keeps in memory databases and writes to packages (the Sim / object compositor caches,
// terrain caches, saves) with a RefPack stream (write 0x004EC200 on Steam 1.67.2, stream vtable slot 0x00FB901C). Its
// compressor allocates 768 KB, clears a 256 KB table and compares EVERY earlier position with the same 3-byte hash inside
// a 16 or 128 KB window: 20-30% of the measured 50 ms+ hitches were dominated by it. This feature answers the stream
// write with features/refpack_codec.h's compressor: the same stream format (header, window, opcodes; any RefPack decoder
// reads it), a bounded hash-chain search with lazy matching, and reusable work memory (a pool of 4 x 512 KB, allocated
// on first use, never freed). Output bytes differ from the game's; the decompressed data is identical.
//
// Since 30/09: a counting run keeps its stream (per thread, with the source's CRC-32C) and its write copies it instead
// of compressing again; streams over 128 KB use the segmented compressor, their pieces parsed on a small pool of worker
// threads (same bytes whoever parses them; docs/features/performance.md).
// Hook: the vtable slot through framework/slot_chain.h (the Frame Profiler's "RefPack compress" counter is the outer
// layer). Calls without a destination: the size bound (flags & 1) passes through; a counting run (the package writer
// measures a stream before allocating it) is answered by the same compressor that then writes it (per thread, matched by
// source and size; the fast write reuses the counting run's flags and depth, so it has exactly the counted size), so a
// buffer sized by one compressor is never written by the other. A stream that does not fit the caller's capacity
// returns -1 (the callers then store the data uncompressed).
// Checks: the first 16 streams of each session (both builds) and, in the development build, 1 stream in 8 by default are
// decompressed with the game's own decoder (0x004EB3B0) and compared with the source; a difference is logged, the game's
// compressor writes that stream instead (when the destination can hold it; else -1), and the feature turns itself off.
#include <cstdint>
#include <string>

namespace FastRefPack {

// Starts answering the stream write (feature on) / stops (feature off: the layer is removed once no counting run of
// this compressor can still be waiting for its write, see Tick). Any thread.
bool Start(std::string* error);
void Stop();
bool Running();
// Pump thread: removes the layer after Stop once the last counting run is 2 s old
void Tick();
// Save thread: releases only inactive Apex-owned compression storage, without waiting for active callers.
uint64_t PrepareForSave();

// Developer mode preferences (saved; optionally included in Development profiles)
void SetVerifyEvery(int n); // 1 stream in n is decompressed and compared (0 = only the first 16 of the session)
int VerifyEvery();
void SetCompareEvery(int n); // 1 stream in n is also compressed by the game (counting only) to compare size and time (0 = never)
int CompareEvery();
void SetChainDepth(int n); // hash-chain candidates per position (4..256, default 32)
int ChainDepth();

struct Stats {
    uint64_t streams = 0, bytesIn = 0, bytesOut = 0; // real writes by the fast compressor
    uint64_t countingRuns = 0;                        // no-destination runs answered by the fast compressor
    double ms = 0.0, countingMs = 0.0;
    uint64_t passedThrough = 0;  // calls the game's compressor answered (off, turned off, or its own counting run's write)
    uint64_t paired = 0;         // writes that followed one of our counting runs
    uint64_t overflows = 0;      // did not fit the capacity: -1 returned
    uint64_t tempContexts = 0;   // all 4 pooled contexts busy: a temporary one was allocated
    uint64_t reused = 0, reuseMissed = 0;               // writes that copied their counting run's stream; whose source had changed
    uint64_t large = 0, largeParallel = 0, largeBusy = 0; // streams over one piece; of them on the pool; pool busy (calling thread alone)
    uint64_t pieces = 0, piecesByWorkers = 0;
    uint64_t checked = 0, mismatches = 0, notCheckable = 0;
    uint64_t compared = 0, comparedGameBytes = 0, comparedFastBytes = 0;
    double comparedGameMs = 0.0, comparedFastMs = 0.0;
    bool selfDisabled = false, installed = false, gameDecoder = false;
    std::string lastMismatch;
};
Stats GetStats();
std::string StatusText();
// Development build: counters and knobs
void RenderDeveloperUI();

} // namespace FastRefPack
