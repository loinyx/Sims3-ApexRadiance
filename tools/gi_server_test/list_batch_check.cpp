// The gather batches of region-listed layers (real-time GI work package R-G1), standalone: no GPU, no bridge.
// Runs the GI worker's sizing on a cost model and checks that no gather submission runs far over the budget
// (server.giSliceUs). Two rules on the same lamps:
// - shared: the full-layer rule for every layer, as before R-G1. A batch is units / columns, where units (columns x
//   gathers that fit the budget) are learned by every layer and change by at most a factor of 2 a submission.
// - split: full layers keep that rule, listed layers size in whole gathers (native/remix-bridge/server/gi_list_batch.h).
// Cost model [inferred, see wp/R-G1.md]: a gather costs perColumnUs x max(columns, wave), as a dispatch of fewer columns
// than the GPU runs at once still takes one wave. Lamps differ in cost by a factor drawn from 0.8 to 1.25.
// Sequences: every lamp listed (the lots of rebake_check.cpp), and listed lamps mixed with unscoped re-bakes (an edit
// or a move solves unscoped, wp/S2-01b.md). Exit code 0: the split rule holds every submission under 2x the budget after
// the first layer, and the shared rule does not on the mixed sequence (so the check would catch the old behaviour).
// Build: cl /nologo /std:c++20 /EHsc /W4 /I native/remix-bridge/server tools/gi_server_test/list_batch_check.cpp
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <vector>

#include "gi_list_batch.h"

namespace {
  constexpr uint32_t kBudgetUs = 2000;
  constexpr uint32_t kGathers = 16;      // bounces a layer, the worker's default
  constexpr uint32_t kFullColumns = 163; // the 200-lamp lot: 10420 texels
  constexpr uint32_t kWave = 48;         // columns a gather dispatch costs at least
  constexpr float kPerColumnUs = 8.0f;   // a full gather about 1.3 ms

  struct Lamp {
    uint32_t columns;
    bool listed;
    float cost;
  };

  struct Result {
    uint32_t submissions = 0;
    uint32_t over = 0;   // submissions over 2x the budget after the first layer
    uint32_t maxUs = 0;  // longest submission after the first layer
  };

  uint32_t g_seed = 12345;
  float random01() {
    g_seed = g_seed * 1664525u + 1013904223u;
    return float(g_seed >> 8) / float(1u << 24);
  }

  std::vector<Lamp> makeLamps(uint32_t count, uint32_t unscopedEvery) {
    std::vector<Lamp> lamps;
    for (uint32_t i = 0; i < count; ++i) {
      Lamp lamp;
      lamp.listed = unscopedEvery == 0 || i % unscopedEvery != 0;
      // Listed: 300-1000 texels (the lots' touch boxes), sometimes a whole floor of 4000.
      lamp.columns = lamp.listed ? (i % 23 == 7 ? 63 : 5 + uint32_t(random01() * 11.0f)) : kFullColumns;
      lamp.cost = 0.8f + random01() * 0.45f;
      lamps.push_back(lamp);
    }
    return lamps;
  }

  uint32_t gatherUs(const Lamp& lamp) {
    return uint32_t(kPerColumnUs * float(std::max(lamp.columns, kWave)) * lamp.cost);
  }

  Result run(const std::vector<Lamp>& lamps, bool split) {
    Result result;
    uint32_t units = 16;
    GiListBatch list;
    for (size_t l = 0; l < lamps.size(); ++l) {
      const Lamp& lamp = lamps[l];
      for (uint32_t done = 0; done < kGathers;) {
        const bool byList = split && lamp.listed;
        uint32_t batch = byList ? list.next(lamp.columns, kGathers - done) : std::min(units / lamp.columns, kGathers - done);
        // Below 2 the worker runs one gather in slices of whole columns, each under the budget.
        const bool sliced = batch < 2;
        batch = std::max(batch, 1u);
        const uint32_t us = batch * gatherUs(lamp);
        done += batch;
        result.submissions += 1;
        if (!sliced && l > 0) {
          result.maxUs = std::max(result.maxUs, us);
          result.over += us > 2 * kBudgetUs ? 1 : 0;
        }
        if (byList) {
          list.learn(lamp.columns, batch, us, kBudgetUs);
        } else if (!sliced) {
          const uint64_t wanted = uint64_t(lamp.columns) * batch * kBudgetUs / std::max<uint32_t>(us, 1);
          units = uint32_t(std::clamp<uint64_t>(wanted, std::max<uint32_t>(units / 2, 1), std::min<uint64_t>(uint64_t(units) * 2, 1u << 24)));
        } else {
          // runSliced learns columns a slice from the one gather.
          const uint64_t wanted = uint64_t(lamp.columns) * kBudgetUs / std::max<uint32_t>(us, 1);
          units = uint32_t(std::clamp<uint64_t>(wanted, std::max<uint32_t>(units / 2, 1), std::min<uint64_t>(uint64_t(units) * 2, 1u << 24)));
        }
      }
    }
    return result;
  }
}

int main() {
  bool passed = true;
  const struct { const char* name; uint32_t unscopedEvery; bool sharedMustFail; } cases[] = {
    { "every lamp listed", 0, false },
    { "an unscoped re-bake every 2nd lamp", 2, true },
    { "an unscoped re-bake every 5th lamp", 5, true },
  };
  for (const auto& c : cases) {
    g_seed = 12345;
    const std::vector<Lamp> lamps = makeLamps(200, c.unscopedEvery);
    const Result shared = run(lamps, false);
    const Result split = run(lamps, true);
    std::printf("%s: shared rule %u submissions, %u over %u us, longest %u us; split rule %u submissions, %u over, longest %u us.\n",
                c.name, shared.submissions, shared.over, 2 * kBudgetUs, shared.maxUs, split.submissions, split.over, split.maxUs);
    if (split.over != 0) {
      std::printf("  FAILED: the split rule ran a submission over %u us.\n", 2 * kBudgetUs);
      passed = false;
    }
    if (c.sharedMustFail && shared.over == 0) {
      std::printf("  FAILED: the shared rule stayed under %u us, so this case does not show the difference.\n", 2 * kBudgetUs);
      passed = false;
    }
  }

  // The batch by list length: 32 gathers fit at 8 columns. A shorter list keeps them, a longer one cuts in proportion.
  GiListBatch list;
  list.gathers = 32.0f;
  list.columns = 8;
  if (list.next(8, 16) != 16 || list.next(4, 16) != 16 || list.next(16, 16) != 16 || list.next(64, 16) != 4) {
    std::printf("FAILED: batches for 8, 4, 16, 64 columns: %u %u %u %u, not 16 16 16 4.\n",
                list.next(8, 16), list.next(4, 16), list.next(16, 16), list.next(64, 16));
    passed = false;
  }
  // An overrun drops the batch at once, the next growth is a quarter.
  list.learn(8, 16, 8000, kBudgetUs);
  const uint32_t dropped = list.next(8, 16);
  list.learn(8, 4, 100, kBudgetUs);
  if (dropped != 4 || list.next(8, 16) != 5) {
    std::printf("FAILED: after 16 gathers in 4x the budget the batch is %u (not 4), after a fast one %u (not 5).\n", dropped, list.next(8, 16));
    passed = false;
  }
  std::printf(passed ? "list batch check passed\n" : "list batch check FAILED\n");
  return passed ? 0 : 1;
}
